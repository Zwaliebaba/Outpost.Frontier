//! `ScrollControl` (`scroll_control.cpp`): the NeuroNet mesh scroll bar
//! of the trade lists.
//!
//! Four meshes of their own: a back plate, a "next" and a "prev" button
//! and a thumb sliding between two hardpoints of the parent, over an
//! integer range. Each part is placed at its hardpoint moved by its offset,
//! the position and the control's offset, and hit through a box of its own
//! half extents. A press on a button steps the value and repeats every 0.3
//! seconds while held; a press on the back plate pages towards the cursor
//! (or drags, "slider_behavior"); the thumb drags. Every change the user
//! makes is told to the parent (property 0x3E). The meshes are drawn
//! tinted, the hot part in yellow.

use std::any::Any;

use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Transform, Vector, transform_xform};
use neuron_core::num::trunc_to_i32;

use crate::ui::control::{ControlMesh, WinControl, base, cursor_ray_inline, ray_hits_box};
use crate::ui::controls::static_control::ini_vector;
use crate::ui::controls::text_control::{OPT_INTERFACE, OPT_NEURONET};
use crate::ui::immediate::capability;
use crate::ui::message::MouseButton;
use crate::ui::pulse::palette;
use crate::ui::tree::{DrawHost, MeshPass, NodeId, Payload, Property, Ui, Widget};

/// The hot colour of a part.
const HOT_RGBA: [u8; 4] = [0xF5, 0xEA, 0x52, 0xFF];
/// The auto-repeat period.
const REPEAT_SECONDS: f64 = 0.3;

/// One part of the bar (back plate, next, prev).
#[derive(Default)]
pub struct Part {
    pub mesh: Option<Box<dyn ControlMesh>>,
    /// "<part>_offset".
    pub offset: Vector,
    /// The instance transform: the hardpoint moved.
    pub xform: Transform,
    /// "<part>_mouse": the half extents the ray is tested on.
    pub mouse_size: Vector,
    pub no_mesh_render: bool,
    /// "<part>_hp": the parent's hardpoint (identity without one).
    pub hp: Transform,
}

impl std::fmt::Debug for Part {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Part")
            .field("has_mesh", &self.mesh.is_some())
            .field("xform", &self.xform)
            .finish_non_exhaustive()
    }
}

impl Part {
    fn new() -> Part {
        Part {
            mesh: None,
            offset: Vector::ZERO,
            xform: Transform::IDENTITY,
            mouse_size: Vector::ZERO,
            no_mesh_render: false,
            hp: Transform::IDENTITY,
        }
    }
}

/// The parts of a [`ScrollControl`] a key or argument names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PartId {
    Back,
    Next,
    Prev,
    Thumb,
}

fn add(a: Vector, b: Vector) -> Vector {
    Vector::new(a.x + b.x, a.y + b.y, a.z + b.z)
}

fn sub(a: Vector, b: Vector) -> Vector {
    Vector::new(a.x - b.x, a.y - b.y, a.z - b.z)
}

/// The translation moved by `v`, each component a float sum.
fn moved(t: &Transform, v: Vector) -> Transform {
    let mut out = *t;
    out.translation = Vector::new(
        t.translation.x + v.x,
        t.translation.y + v.y,
        t.translation.z + v.z,
    );
    out
}

/// The bit-trick reciprocal square root with one Newton step.
#[must_use]
pub fn fast_inv_sqrt(x: f32) -> f64 {
    let bits = 0x5F37_59DF_i32.wrapping_sub(x.to_bits().cast_signed() >> 1);
    let y = f64::from(f32::from_bits(bits.cast_unsigned()));
    (1.5 - y * y * f64::from(x) * 0.5) * y
}

/// The step inlined with the squared length kept unrounded.
#[must_use]
pub fn fast_length_extended(len2: f64) -> f32 {
    let f = len2 as f32;
    let bits = 0x5F37_59DF_i32.wrapping_sub(f.to_bits().cast_signed() >> 1);
    let y = f64::from(f32::from_bits(bits.cast_unsigned()));
    ((1.5 - y * y * len2 * 0.5) * y * len2) as f32
}

/// The function form: the squared length stored as a float first.
#[must_use]
pub fn fast_length_stored(len2: f64) -> f32 {
    let f = len2 as f32;
    (fast_inv_sqrt(f) * f64::from(f)) as f32
}

/// `z z + y y + x x`.
fn squared_length_zyx(v: Vector) -> f64 {
    (f64::from(v.z) * f64::from(v.z) + f64::from(v.y) * f64::from(v.y))
        + f64::from(v.x) * f64::from(v.x)
}

/// `HudVec3ScaleByInverse`: z by the unrounded reciprocal, y and x by its
/// float.
fn scale_by_inverse(v: Vector, length: f32) -> Vector {
    let inv = 1.0 / f64::from(length);
    let inv_f = f64::from(inv as f32);
    Vector::new(
        (inv_f * f64::from(v.x)) as f32,
        (inv_f * f64::from(v.y)) as f32,
        (inv * f64::from(v.z)) as f32,
    )
}

/// `1 / length` kept unrounded for all three products.
fn scale_by_exact_inverse(v: Vector, length: f32) -> Vector {
    let inv = 1.0 / f64::from(length);
    Vector::new(
        (f64::from(v.x) * inv) as f32,
        (f64::from(v.y) * inv) as f32,
        (f64::from(v.z) * inv) as f32,
    )
}

/// `ScrollControl`.
pub struct ScrollControl {
    pub control: WinControl,
    pub back: Part,
    pub next: Part,
    pub prev: Part,
    pub thumb: Option<Box<dyn ControlMesh>>,
    pub thumb_start: Transform,
    pub thumb_end: Transform,
    pub thumb_offset0: Vector,
    pub thumb_offset1: Vector,
    /// The thumb now (`UpdateThumb`).
    pub thumb_xform: Transform,
    pub thumb_mouse: Vector,
    pub thumb_no_mesh_render: bool,
    pub thumb_hp0: Transform,
    pub thumb_hp1: Transform,
    pub min_range: i32,
    pub max_range: i32,
    pub page_size: i32,
    pub value: i32,
    /// The button step (property 0x3c).
    pub step: i32,
    pub slider_behavior: bool,
    pub thumb_valid: bool,
    /// The last auto-repeat.
    pub repeat_time: f32,
    pub dragging: bool,
    pub paging: bool,
    pub next_pressed: bool,
    pub prev_pressed: bool,
    pub thumb_hover: bool,
    pub next_hover: bool,
    pub prev_hover: bool,
    pub page_toward_min: bool,
}

impl std::fmt::Debug for ScrollControl {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ScrollControl")
            .field("value", &self.value)
            .field("min_range", &self.min_range)
            .field("max_range", &self.max_range)
            .field("thumb_valid", &self.thumb_valid)
            .finish_non_exhaustive()
    }
}

impl Default for ScrollControl {
    fn default() -> Self {
        ScrollControl {
            control: WinControl::new(),
            back: Part::new(),
            next: Part::new(),
            prev: Part::new(),
            thumb: None,
            thumb_start: Transform::IDENTITY,
            thumb_end: Transform::IDENTITY,
            thumb_offset0: Vector::ZERO,
            thumb_offset1: Vector::ZERO,
            thumb_xform: Transform::IDENTITY,
            thumb_mouse: Vector::ZERO,
            thumb_no_mesh_render: false,
            thumb_hp0: Transform::IDENTITY,
            thumb_hp1: Transform::IDENTITY,
            min_range: 0,
            max_range: 0,
            page_size: 1,
            value: 0,
            step: 1,
            slider_behavior: false,
            thumb_valid: false,
            repeat_time: 0.0,
            dragging: false,
            paging: false,
            next_pressed: false,
            prev_pressed: false,
            thumb_hover: false,
            next_hover: false,
            prev_hover: false,
            page_toward_min: false,
        }
    }
}

/// The instance's matrix pushed (`SetInstanceMatrix`, `UpdateInstance`).
fn push_matrix(mesh: &mut Option<Box<dyn ControlMesh>>, t: &Transform) {
    if let Some(m) = mesh.as_mut() {
        m.set_transform(*t);
    }
}

/// The instance's position pushed.
fn push_position(mesh: &mut Option<Box<dyn ControlMesh>>, p: Vector) {
    if let Some(m) = mesh.as_mut() {
        m.set_position(p);
    }
}

impl ScrollControl {
    fn position(ui: &Ui, id: NodeId) -> Vector {
        ui.node(id).map_or(Vector::ZERO, |n| n.position)
    }

    /// `UpdateThumb`: the thumb between its ends by the value; invalid for
    /// an empty range or a value outside it.
    pub fn update_thumb(&mut self) {
        let (min, max) = (self.min_range, self.max_range);
        if min >= max || min > self.value || max < self.value {
            self.thumb_valid = false;
            return;
        }
        let t = f64::from(self.value.wrapping_sub(min)) / f64::from(max.wrapping_sub(min));
        self.thumb_valid = true;
        self.thumb_xform = self.thumb_start;
        let s = self.thumb_start.translation;
        let e = self.thumb_end.translation;
        let dx = f64::from(e.x) - f64::from(s.x);
        let dy = f64::from(e.y) - f64::from(s.y);
        let dz = (f64::from(e.z) - f64::from(s.z)) as f32;
        let dxt = (dx * t) as f32;
        let dzt = (f64::from(dz) * t) as f32;
        self.thumb_xform.translation = Vector::new(
            (f64::from(dxt) + f64::from(s.x)) as f32,
            (dy * t + f64::from(s.y)) as f32,
            (f64::from(dzt) + f64::from(s.z)) as f32,
        );
        let x = self.thumb_xform;
        push_matrix(&mut self.thumb, &x);
    }

    /// `UpdateLayout`: with a `WinControl` parent every working transform
    /// from its hardpoint, offset, the position and the control's offset;
    /// the parts pushed, then the thumb.
    pub fn update_layout(&mut self, ui: &Ui, id: NodeId) {
        let has_parent = ui.parent(id).is_some_and(|p| ui.win_control(p).is_some());
        if has_parent {
            let p = Self::position(ui, id);
            let o = self.control.offset;
            for part in [&mut self.back, &mut self.next, &mut self.prev] {
                part.xform = moved(&part.hp, add(add(part.offset, p), o));
            }
            self.thumb_start = moved(&self.thumb_hp0, add(add(p, self.thumb_offset0), o));
            self.thumb_end = moved(&self.thumb_hp1, add(add(self.thumb_offset1, p), o));
        }
        for part in [&mut self.back, &mut self.next, &mut self.prev] {
            let x = part.xform;
            push_matrix(&mut part.mesh, &x);
        }
        self.update_thumb();
    }

    fn screen(ui: &Ui, world: Vector) -> Option<Vector> {
        ui.camera
            .project(world)
            .map(|(x, y, z)| Vector::new(x, y, z))
    }

    /// `ChoosePageDirection`: whether a back-plate click pages towards the
    /// minimum.
    pub fn choose_page_direction(&mut self, ui: &Ui, screen_x: u32, screen_y: u32) {
        if self.value == self.min_range {
            self.page_toward_min = false;
        } else if self.value == self.max_range {
            self.page_toward_min = true;
        }
        let mouse = Vector::new(screen_x as f32, screen_y as f32, 0.0);
        let to_max = self.max_range.wrapping_sub(self.value).wrapping_abs();
        let to_min = self.min_range.wrapping_sub(self.value).wrapping_abs();
        let current = transform_xform(&self.thumb_xform, Vector::ZERO);
        if to_max < to_min {
            let start = transform_xform(&self.thumb_start, Vector::ZERO);
            let (Some(cs), Some(ss)) = (Self::screen(ui, current), Self::screen(ui, start)) else {
                self.page_toward_min = true;
                return;
            };
            let track = sub(cs, ss);
            let track_len = fast_length_extended(squared_length_zyx(track));
            let to_mouse = sub(mouse, ss);
            let mouse_len = fast_length_extended(squared_length_zyx(to_mouse));
            if track_len == 0.0 || mouse_len == 0.0 {
                self.page_toward_min = true;
                return;
            }
            let td = scale_by_inverse(track, track_len);
            let md = scale_by_inverse(to_mouse, mouse_len);
            let dot = (f64::from(md.z) * f64::from(td.z) + f64::from(md.y) * f64::from(td.y))
                + f64::from(md.x) * f64::from(td.x);
            self.page_toward_min = dot * f64::from(mouse_len) < f64::from(track_len);
            return;
        }
        let end = transform_xform(&self.thumb_end, Vector::ZERO);
        let (Some(cs), Some(es)) = (Self::screen(ui, current), Self::screen(ui, end)) else {
            self.page_toward_min = true;
            return;
        };
        let track = sub(cs, es);
        let track_len = fast_length_extended(squared_length_zyx(track));
        let to_mouse = Vector::new(mouse.x - es.x, mouse.y - es.y, -es.z);
        let mouse_len = fast_length_extended(squared_length_zyx(to_mouse));
        if track_len == 0.0 || mouse_len == 0.0 {
            self.page_toward_min = true;
            return;
        }
        let td = scale_by_inverse(track, track_len);
        let md = scale_by_exact_inverse(to_mouse, mouse_len);
        let dot = (f64::from(td.x) * f64::from(md.x) + f64::from(md.y) * f64::from(td.y))
            + f64::from(md.z) * f64::from(td.z);
        self.page_toward_min = !(dot * f64::from(mouse_len) < f64::from(track_len));
    }

    /// `PageStep`: a page towards the cursor while the thumb has not reached
    /// it.
    pub fn page_step(&mut self, ui: &mut Ui, id: NodeId) {
        let mouse = Vector::new(ui.cursor.0 as f32, ui.cursor.1 as f32, 0.0);
        let current = transform_xform(&self.thumb_xform, Vector::ZERO);
        let toward_min = self.page_toward_min;
        let other = if toward_min {
            transform_xform(&self.thumb_start, Vector::ZERO)
        } else {
            transform_xform(&self.thumb_end, Vector::ZERO)
        };
        let Some(cs) = Self::screen(ui, current) else {
            return;
        };
        let Some(os) = Self::screen(ui, other) else {
            return;
        };
        let track = sub(cs, os);
        let track_len = fast_length_stored(squared_length_zyx(track));
        let to_mouse = sub(mouse, os);
        let mouse_len = if toward_min {
            fast_length_stored(squared_length_zyx(to_mouse))
        } else {
            fast_length_extended(squared_length_zyx(to_mouse))
        };
        if track_len == 0.0 || mouse_len == 0.0 {
            return;
        }
        let td = scale_by_inverse(track, track_len);
        let md = scale_by_inverse(to_mouse, mouse_len);
        let dot = (f64::from(td.x) * f64::from(md.x) + f64::from(td.y) * f64::from(md.y))
            + f64::from(td.z) * f64::from(md.z);
        if !(dot * f64::from(mouse_len) < f64::from(track_len)) {
            return;
        }
        if toward_min {
            self.value = self.value.wrapping_sub(self.page_size);
            if self.value < self.min_range {
                self.value = self.min_range;
            }
        } else {
            self.value = self.value.wrapping_add(self.page_size);
            if self.value > self.max_range {
                self.value = self.max_range;
            }
        }
        self.update_thumb();
        self.notify_value(ui, id);
    }

    /// `DragThumb`: the value under the cursor projected on the thumb's
    /// track.
    pub fn drag_thumb(&mut self, ui: &mut Ui, id: NodeId, screen_x: u32, screen_y: u32) {
        let start = transform_xform(&self.thumb_start, Vector::ZERO);
        let end = transform_xform(&self.thumb_end, Vector::ZERO);
        let Some(ss) = Self::screen(ui, start) else {
            return;
        };
        let Some(es) = Self::screen(ui, end) else {
            return;
        };
        let mouse = Vector::new(screen_x as f32, screen_y as f32, 0.0);
        let track = sub(es, ss);
        let track_len = fast_length_extended(squared_length_zyx(track));
        let to_mouse = sub(mouse, ss);
        let mouse_len = fast_length_extended(squared_length_zyx(to_mouse));
        if track_len == 0.0 || mouse_len == 0.0 {
            return;
        }
        let inv_track = 1.0 / f64::from(track_len);
        let inv_track_f = f64::from(inv_track as f32);
        let td = scale_by_exact_inverse(track, track_len);
        let md = scale_by_exact_inverse(to_mouse, mouse_len);
        let dot = (f64::from(td.x) * f64::from(md.x) + f64::from(td.y) * f64::from(md.y))
            + f64::from(td.z) * f64::from(md.z);
        let min = self.min_range;
        let range = self.max_range.wrapping_sub(min);
        let scaled = dot * f64::from(mouse_len) * inv_track_f * f64::from(range) + 0.5;
        self.value = trunc_to_i32(scaled.floor()).wrapping_add(min);
        if self.value > self.max_range {
            self.value = self.max_range;
        }
        if self.value < min {
            self.value = min;
        }
        self.update_thumb();
        self.notify_value(ui, id);
    }

    /// `NotifyValue`: the parent hears 0x3E with the value.
    pub fn notify_value(&self, ui: &mut Ui, id: NodeId) {
        let Some(parent) = ui.parent(id) else { return };
        if ui.win_control(parent).is_none() {
            return;
        }
        ui.property(parent, Property::ScrollChanged(id, self.value));
    }

    fn part_mut(&mut self, part: PartId) -> &mut Part {
        match part {
            PartId::Back | PartId::Thumb => &mut self.back,
            PartId::Next => &mut self.next,
            PartId::Prev => &mut self.prev,
        }
    }

    /// The "<part>_mesh" sequence: the instance released, a non-empty path
    /// loaded, the transform pushed.
    pub fn load_part_mesh(&mut self, ui: &mut Ui, part: PartId, path: &[u8]) {
        let mesh = if path.is_empty() {
            None
        } else {
            ui.mesh_loader.as_mut().and_then(|l| l.load(path, b""))
        };
        if part == PartId::Thumb {
            self.thumb = mesh;
            let x = self.thumb_xform;
            push_matrix(&mut self.thumb, &x);
        } else {
            let p = self.part_mut(part);
            p.mesh = mesh;
            let x = p.xform;
            push_matrix(&mut p.mesh, &x);
        }
    }

    /// The "<part>_offset" sequence: the layout, then the instance's
    /// position pushed.
    fn after_offset(&mut self, ui: &Ui, id: NodeId, part: PartId) {
        self.update_layout(ui, id);
        if part == PartId::Thumb {
            let t = self.thumb_xform.translation;
            push_position(&mut self.thumb, t);
        } else {
            let p = self.part_mut(part);
            let t = p.xform.translation;
            push_position(&mut p.mesh, t);
        }
    }

    fn ask_hardpoint(ui: &Ui, parent: Option<NodeId>, name: &[u8]) -> Transform {
        let mut t = Transform::IDENTITY;
        if let Some(p) = parent.and_then(|p| ui.win_control(p)) {
            t = p.hardpoint_transform(name).1;
        }
        t
    }

    /// `ScrollControl::Create`: the `[ScrollControl]` keys of the INI
    /// payload, hardpoints asked of the Create argument.
    pub fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        ui.base_create(id, parent);
        if let Payload::Ini(ini) = payload {
            self.read_ini(ui, id, parent, ini);
        }
        if let Some(p) = ui.parent(id) {
            let at = Self::position(ui, p);
            ui.set_position(p, at);
            if let Some(pp) = crate::ui::control::parent_slot5_position(ui, id) {
                self.set_position(ui, id, pp);
            }
        }
        true
    }

    fn read_ini(&mut self, ui: &mut Ui, id: NodeId, parent: Option<NodeId>, ini: &mut IniReader) {
        let parts: [(&[u8], PartId); 3] = [
            (b"back", PartId::Back),
            (b"nextb", PartId::Next),
            (b"prevb", PartId::Prev),
        ];
        let key = |prefix: &[u8], suffix: &[u8]| [prefix, suffix].concat();
        while ini.read_value() {
            let mut handled = false;
            for (prefix, part) in parts {
                if ini.is_value(&key(prefix, b"_mesh")) {
                    let path = ini.value_string();
                    self.load_part_mesh(ui, part, &path);
                } else if ini.is_value(&key(prefix, b"_offset")) {
                    self.part_mut(part).offset = ini_vector(ini);
                    self.after_offset(ui, id, part);
                } else if ini.is_value(&key(prefix, b"_hp")) {
                    let name = ini.value_string();
                    self.part_mut(part).hp = Self::ask_hardpoint(ui, parent, &name);
                } else if ini.is_value(&key(prefix, b"_mouse")) {
                    self.part_mut(part).mouse_size = ini_vector(ini);
                } else if ini.is_value(&key(prefix, b"_no_mesh_render")) {
                    self.part_mut(part).no_mesh_render = ini.value_float(0) != 0.0;
                } else {
                    continue;
                }
                handled = true;
                break;
            }
            if handled {
                continue;
            }
            if ini.is_value(b"thumb_mesh") {
                let path = ini.value_string();
                self.load_part_mesh(ui, PartId::Thumb, &path);
            } else if ini.is_value(b"thumb_offset0") {
                self.thumb_offset0 = ini_vector(ini);
                self.after_offset(ui, id, PartId::Thumb);
            } else if ini.is_value(b"thumb_offset1") {
                self.thumb_offset1 = ini_vector(ini);
                self.after_offset(ui, id, PartId::Thumb);
            } else if ini.is_value(b"thumb_hp0") {
                let name = ini.value_string();
                self.thumb_hp0 = Self::ask_hardpoint(ui, parent, &name);
            } else if ini.is_value(b"thumb_hp1") {
                let name = ini.value_string();
                self.thumb_hp1 = Self::ask_hardpoint(ui, parent, &name);
            } else if ini.is_value(b"thumb_mouse") {
                self.thumb_mouse = ini_vector(ini);
            } else if ini.is_value(b"thumb_no_mesh_render") {
                self.thumb_no_mesh_render = ini.value_float(0) != 0.0;
            } else if ini.is_value(b"min_range") {
                self.min_range = trunc_to_i32(ini.value_float(0));
            } else if ini.is_value(b"max_range") {
                self.max_range = trunc_to_i32(ini.value_float(0));
            } else if ini.is_value(b"page_size") {
                self.page_size = trunc_to_i32(ini.value_float(0));
            } else if ini.is_value(b"slider_behavior") {
                self.slider_behavior = ini.value_float(0) != 0.0;
            } else if ini.is_value(b"no_z_enable") {
                self.control.no_z_enable = ini.value_float(0) != 0.0;
            }
        }
    }

    /// `ScrollControl::Init` for one button part (back, next; prev is
    /// skipped whole without a mesh).
    #[allow(clippy::too_many_arguments)]
    pub fn init_part(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        part: PartId,
        mesh: Option<&[u8]>,
        offset: Vector,
        hardpoint: Option<&[u8]>,
        mouse: Vector,
        no_mesh_render: bool,
    ) {
        if part == PartId::Prev && mesh.is_none() {
            return;
        }
        if let Some(path) = mesh {
            self.load_part_mesh(ui, part, path);
        }
        self.part_mut(part).offset = offset;
        self.after_offset(ui, id, part);
        if let Some(name) = hardpoint {
            let parent = ui.parent(id);
            self.part_mut(part).hp = Self::ask_hardpoint(ui, parent, name);
        }
        let p = self.part_mut(part);
        p.mouse_size = mouse;
        p.no_mesh_render = no_mesh_render;
    }

    /// `ScrollControl::Init`'s thumb and range part.
    #[allow(clippy::too_many_arguments)]
    pub fn init_thumb(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        mesh: Option<&[u8]>,
        offset0: Vector,
        offset1: Vector,
        hp0: Option<&[u8]>,
        hp1: Option<&[u8]>,
        mouse: Vector,
        no_mesh_render: bool,
        range: (i32, i32, i32),
        slider_behavior: bool,
        no_z_enable: bool,
    ) {
        if let Some(path) = mesh {
            self.load_part_mesh(ui, PartId::Thumb, path);
        }
        self.thumb_offset0 = offset0;
        self.after_offset(ui, id, PartId::Thumb);
        self.thumb_offset1 = offset1;
        self.after_offset(ui, id, PartId::Thumb);
        let parent = ui.parent(id);
        if let Some(name) = hp0 {
            self.thumb_hp0 = Self::ask_hardpoint(ui, parent, name);
        }
        if let Some(name) = hp1 {
            self.thumb_hp1 = Self::ask_hardpoint(ui, parent, name);
        }
        self.thumb_mouse = mouse;
        self.thumb_no_mesh_render = no_mesh_render;
        (self.min_range, self.max_range, self.page_size) = range;
        self.slider_behavior = slider_behavior;
        self.control.no_z_enable = no_z_enable;
    }

    /// `ScrollControl::SetPosition`: `WinControl`'s, then the layout.
    pub fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        base::set_position(ui, id, &mut self.control, position);
        self.update_layout(ui, id);
    }

    /// `ScrollControl::HitTest`: any button, or a valid thumb.
    #[must_use]
    pub fn hit_test(&self, ray: Vector) -> bool {
        ray_hits_box(self.back.mouse_size, &self.back.xform, ray)
            || ray_hits_box(self.next.mouse_size, &self.next.xform, ray)
            || ray_hits_box(self.prev.mouse_size, &self.prev.xform, ray)
            || (self.thumb_valid && ray_hits_box(self.thumb_mouse, &self.thumb_xform, ray))
    }

    /// `ScrollControl::Update`.
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        if !self.thumb_valid {
            ui.remove_capture(id);
            self.dragging = false;
            self.paging = false;
            self.next_pressed = false;
            self.prev_pressed = false;
            return;
        }
        let (cx, cy) = ui.cursor;
        if self.dragging {
            self.drag_thumb(ui, id, cx.cast_unsigned(), cy.cast_unsigned());
            return;
        }
        let now = ui.services.elapsed_time();
        let due = now - f64::from(self.repeat_time) > REPEAT_SECONDS;
        if self.paging {
            if due {
                self.page_step(ui, id);
                self.repeat_time = ui.services.elapsed_time() as f32;
            }
            return;
        }
        if self.next_pressed {
            if !due {
                return;
            }
            if self.value < self.max_range {
                self.step_value(ui, id, true);
            }
            self.repeat_time = ui.services.elapsed_time() as f32;
            return;
        }
        if self.prev_pressed {
            if !due {
                return;
            }
            if self.value > self.min_range {
                self.step_value(ui, id, false);
            }
            self.repeat_time = ui.services.elapsed_time() as f32;
        }
    }

    fn step_value(&mut self, ui: &mut Ui, id: NodeId, up: bool) {
        if up {
            self.value = self.value.wrapping_add(self.step);
            if self.value > self.max_range {
                self.value = self.max_range;
            }
        } else {
            self.value = self.value.wrapping_sub(self.step);
            if self.value < self.min_range {
                self.value = self.min_range;
            }
        }
        self.update_thumb();
        self.notify_value(ui, id);
    }

    /// `ScrollControl::Draw`: while the thumb is valid, each part tinted
    /// (the hot part yellow), the back plate and the thumb glowing.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        if host.game_option(OPT_INTERFACE) == 0 || host.game_option(OPT_NEURONET) == 0 {
            return;
        }
        ui.draw_children(id, host);
        if !self.thumb_valid {
            return;
        }
        let idle = palette::IDLE.to_le_bytes();
        let body = |host: &mut dyn DrawHost, mesh: &dyn ControlMesh, tint: [u8; 4]| {
            host.submit_mesh(
                mesh,
                MeshPass::Body {
                    tint,
                    front_cull: false,
                    scale: 1.0,
                },
            );
            if let Some(imm) = host.immediate() {
                imm.disable(capability::DEPTH_TEST);
            }
        };
        if let Some(mesh) = self.back.mesh.as_deref() {
            if !self.back.no_mesh_render {
                body(host, mesh, idle);
            }
            if let Some(imm) = host.immediate() {
                imm.disable(capability::DEPTH_TEST);
            }
            host.submit_mesh(
                mesh,
                MeshPass::Tinted {
                    tint: idle,
                    lighting: true,
                },
            );
        }
        for (part, hot) in [
            (&self.next, self.next_pressed || self.next_hover),
            (&self.prev, self.prev_pressed || self.prev_hover),
        ] {
            if let Some(mesh) = part.mesh.as_deref() {
                if !part.no_mesh_render {
                    body(host, mesh, if hot { HOT_RGBA } else { idle });
                }
                if let Some(imm) = host.immediate() {
                    imm.disable(capability::DEPTH_TEST);
                }
            }
        }
        let Some(mesh) = self.thumb.as_deref() else {
            return;
        };
        if !self.thumb_no_mesh_render {
            body(host, mesh, idle);
        }
        let tint = if self.dragging || self.thumb_hover {
            HOT_RGBA
        } else {
            idle
        };
        host.submit_mesh(
            mesh,
            MeshPass::Tinted {
                tint,
                lighting: true,
            },
        );
        if let Some(imm) = host.immediate() {
            imm.disable(capability::DEPTH_TEST);
        }
    }

    /// `MouseEnter` (and `MouseOver` and `MouseLeave`): the hover bytes; a
    /// hovered valid thumb masks both buttons.
    pub fn mouse_enter(&mut self, ray: Vector) {
        self.next_hover = ray_hits_box(self.next.mouse_size, &self.next.xform, ray);
        self.prev_hover = ray_hits_box(self.prev.mouse_size, &self.prev.xform, ray);
        self.thumb_hover = false;
        if !self.thumb_valid {
            return;
        }
        self.thumb_hover = ray_hits_box(self.thumb_mouse, &self.thumb_xform, ray);
        if self.thumb_hover {
            self.next_hover = false;
            self.prev_hover = false;
        }
    }

    /// `ScrollControl::MouseEvent`.
    pub fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if !self.thumb_valid {
            return false;
        }
        let release = event.field0() == -1 && event.field1() == 1;
        for held in [
            &mut self.dragging,
            &mut self.next_pressed,
            &mut self.prev_pressed,
            &mut self.paging,
        ] {
            if *held {
                if release {
                    ui.remove_capture(id);
                    *held = false;
                }
                return true;
            }
        }
        if event.field0() != -1 || event.field1() != 2 {
            return true;
        }
        let ray = cursor_ray_inline(ui);
        if self.thumb_valid && ray_hits_box(self.thumb_mouse, &self.thumb_xform, ray) {
            ui.push_capture(id);
        } else if ray_hits_box(self.next.mouse_size, &self.next.xform, ray) {
            ui.push_capture(id);
            if self.value < self.max_range {
                self.step_value(ui, id, true);
            }
            self.next_pressed = true;
            self.repeat_time = ui.services.elapsed_time() as f32;
            return true;
        } else if ray_hits_box(self.prev.mouse_size, &self.prev.xform, ray) {
            ui.push_capture(id);
            if self.value > self.min_range {
                self.step_value(ui, id, false);
            }
            self.prev_pressed = true;
            self.repeat_time = ui.services.elapsed_time() as f32;
            return true;
        } else {
            if !self.thumb_valid || !ray_hits_box(self.back.mouse_size, &self.back.xform, ray) {
                return true;
            }
            ui.push_capture(id);
            if !self.slider_behavior {
                self.paging = true;
                self.repeat_time = ui.services.elapsed_time() as f32;
                let (cx, cy) = ui.cursor;
                self.choose_page_direction(ui, cx.cast_unsigned(), cy.cast_unsigned());
                self.page_step(ui, id);
                return true;
            }
        }
        {
            self.dragging = true;
            let (cx, cy) = ui.cursor;
            self.drag_thumb(ui, id, cx.cast_unsigned(), cy.cast_unsigned());
        }
        true
    }

    /// `ScrollControl::SetProperty`'s own messages.
    ///
    /// # Errors
    ///
    /// The property when it is `WinControl`'s.
    pub fn property<'a>(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        property: Property<'a>,
    ) -> Result<isize, Property<'a>> {
        Ok(match property {
            Property::Offset(offset) => {
                self.control.offset = offset;
                self.update_layout(ui, id);
                0
            }
            Property::ScrollRange(first, second) => {
                self.min_range = first;
                self.max_range = second;
                if first > second {
                    self.max_range = first;
                    self.min_range = second;
                }
                if self.value > self.max_range {
                    self.value = self.max_range;
                }
                if self.value < self.min_range {
                    self.value = self.min_range;
                }
                self.update_thumb();
                self.notify_value(ui, id);
                0
            }
            Property::ScrollValue(v) => {
                self.value = v;
                if self.value < self.min_range {
                    self.value = self.min_range;
                }
                if self.value > self.max_range && self.max_range > self.min_range {
                    self.value = self.max_range;
                }
                self.update_thumb();
                0
            }
            Property::Other(0x39, ..) => self.max_range as isize,
            Property::Other(0x3A, ..) => self.min_range as isize,
            Property::GetScrollValue => self.value as isize,
            Property::Other(0x3C, a, b) => {
                self.page_size = a as i32;
                self.step = if b != 0 { b as i32 } else { 1 };
                0
            }
            Property::Other(0x3D, ..) => self.page_size as isize,
            other => return Err(other),
        })
    }

    /// `AddAttachOffset`: the delta onto every hardpoint; no layout.
    pub fn add_attach_offset(&mut self, delta: Vector) {
        for t in [
            &mut self.back.hp,
            &mut self.next.hp,
            &mut self.prev.hp,
            &mut self.thumb_hp0,
            &mut self.thumb_hp1,
        ] {
            *t = moved(t, delta);
        }
    }

    /// `~ScrollControl`: the meshes released; a held part drops the capture.
    pub fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.back.mesh = None;
        self.prev.mesh = None;
        self.next.mesh = None;
        self.thumb = None;
        if self.dragging || self.paging || self.next_pressed || self.prev_pressed {
            ui.remove_capture(id);
        }
        base::on_delete(ui, id, &mut self.control);
    }
}

/// The "ScrollControl" factory product.
#[derive(Debug, Default)]
pub struct ScrollControlWidget {
    pub scroll: ScrollControl,
}

impl Widget for ScrollControlWidget {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.scroll.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.scroll.control)
    }
    fn hit_test(&mut self, _ui: &mut Ui, _id: NodeId, ray: Vector) -> bool {
        self.scroll.hit_test(ray)
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.scroll.set_position(ui, id, position);
    }
    /// Nine words into the orientation (not `WinControl`'s slot 7), then
    /// the layout.
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        if let Some(n) = ui.node_mut(id) {
            n.orientation = orientation;
        }
        self.scroll.update_layout(ui, id);
    }
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        self.scroll.create(ui, id, parent, payload)
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.scroll.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.scroll.on_delete(ui, id);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.scroll.update(ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.scroll.draw(ui, id, host);
    }
    fn mouse_over(&mut self, _ui: &mut Ui, _id: NodeId, ray: Vector) {
        self.scroll.mouse_enter(ray);
    }
    fn mouse_enter(&mut self, _ui: &mut Ui, _id: NodeId, ray: Vector) {
        self.scroll.mouse_enter(ray);
    }
    fn mouse_leave(&mut self, _ui: &mut Ui, _id: NodeId, ray: Vector) {
        self.scroll.mouse_enter(ray);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        self.scroll.mouse_event(ui, id, event)
    }
    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
        base::broadcast(ui, id, self, msg)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        match self.scroll.property(ui, id, property) {
            Ok(r) => r,
            Err(p) => base::property(ui, id, self, p),
        }
    }
}
