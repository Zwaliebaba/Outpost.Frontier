//! `StaticControl` (`nn_static_control.cpp`): the NeuroNet and HUD mesh
//! that only draws (frames, gauges, plates).
//!
//! Its mesh is drawn in two passes, the mesh tinted by the idle cyan (or
//! a property's colour, white with "no_coloring") and its wire outline
//! (grey while inactive), scaled to a forced radius, with or without the
//! depth test, optionally into a window of the screen through its own
//! camera. The instance is turned by a roll, pitch and yaw that rotate
//! with a speed or snap back to their start. A click tells the parent
//! 0x1A (0x1B for a right press).
//!
//! The sines and cosines of the turns come from the Rust library, so the
//! instance's orientation is Tier B (decision T11).

use std::any::Any;

use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Transform, Vector, mul_matrix};

use crate::ui::control::{ControlMesh, WinControl, base, tell_parent};
use crate::ui::controls::text_control::{OPT_INTERFACE, OPT_NEURONET, OPT_STATIC_CONTROLS};
use crate::ui::immediate::{capability, render_state};
use crate::ui::message::MouseButton;
use crate::ui::pulse::palette;
use crate::ui::services::{HoverKey, ToolTipHover};
use crate::ui::tree::{
    ACTIVE, DrawHost, MeshPass, NodeId, Payload, Property, Ui, Widget, WindowView,
};

/// `Update`'s "already at the start angle" threshold (1e-4).
const ANGLE_EPSILON: f32 = f32::from_bits(0x38D1_B717);
/// The render-to-window camera's vertical field of view.
const WINDOW_FOV_Y: f32 = 35.0;
/// Where a render-to-window instance sits while it is drawn.
const WINDOW_DEPTH: f32 = -87.0;

/// One plane rotation of the rows of a 3x3: per row `a' = c a + s b`,
/// `b' = c b - s a` with both read first.
pub fn rotate_columns(m: &mut Matrix, col_a: usize, col_b: usize, c: f64, s: f64) {
    for row in &mut m.m {
        let a = f64::from(row[col_a]);
        let b = f64::from(row[col_b]);
        row[col_a] = (c * a + s * b) as f32;
        row[col_b] = (c * b - s * a) as f32;
    }
}

fn add3(a: Vector, b: Vector) -> Vector {
    Vector::new(a.x + b.x, a.y + b.y, a.z + b.z)
}

/// A flag key: `get_value_float(0) != 0`.
fn ini_flag(ini: &mut IniReader) -> bool {
    ini.value_float(0) != 0.0
}

/// The three floats of a vector key, read 1, 0, 2.
pub fn ini_vector(ini: &mut IniReader) -> Vector {
    let y = ini.value_float(1) as f32;
    let x = ini.value_float(0) as f32;
    let z = ini.value_float(2) as f32;
    Vector::new(x, y, z)
}

/// The attach transform reset and filled from `parent`'s hardpoint; the
/// answer.
pub fn ask_hardpoint(
    ui: &Ui,
    control: &mut WinControl,
    parent: Option<NodeId>,
    name: &[u8],
) -> Option<bool> {
    control.attach_orientation = Matrix::IDENTITY;
    control.attach_position = Vector::ZERO;
    let p = ui.win_control(parent?)?;
    let (found, t) = p.hardpoint_transform(name);
    control.attach_orientation = t.rotation;
    control.attach_position = t.translation;
    Some(found)
}

/// `StaticControl`.
#[derive(Debug)]
pub struct StaticControl {
    pub control: WinControl,
    /// `+0x32c`, `+0x330`, `+0x334`: radians.
    pub cur_roll: f32,
    pub cur_pitch: f32,
    pub cur_yaw: f32,
    /// `+0x338` yaw, pitch, roll.
    pub start_ypr: [f32; 3],
    pub no_mesh_render: bool,
    pub no_wire_render: bool,
    pub no_coloring: bool,
    /// `+0x347` (property 0xc).
    pub field_347: bool,
    /// `+0x348`: the colours of property 0x18.
    pub use_colours: bool,
    pub enable_rotation: bool,
    pub no_clear_render: bool,
    pub render_to_window: bool,
    pub zclear: bool,
    pub mesh_colour: [u8; 4],
    pub wire_colour: [u8; 4],
    /// "force_radius": the draw scale is it over the instance's radius.
    pub force_radius: f32,
    /// "rotation_speed": radians per second.
    pub rotation_speed: f32,
}

impl Default for StaticControl {
    fn default() -> Self {
        StaticControl {
            control: WinControl::new(),
            cur_roll: 0.0,
            cur_pitch: 0.0,
            cur_yaw: 0.0,
            start_ypr: [0.0; 3],
            no_mesh_render: false,
            no_wire_render: false,
            no_coloring: false,
            field_347: false,
            use_colours: false,
            enable_rotation: false,
            no_clear_render: true,
            render_to_window: false,
            zclear: false,
            mesh_colour: [0; 4],
            wire_colour: [0; 4],
            force_radius: 0.0,
            rotation_speed: 0.0,
        }
    }
}

impl StaticControl {
    fn position(ui: &Ui, id: NodeId) -> Vector {
        ui.node(id).map_or(Vector::ZERO, |n| n.position)
    }

    /// The instance at the offset plus the position, identity orientation.
    fn place_instance_at_offset(&mut self, ui: &Ui, id: NodeId) {
        let at = add3(self.control.offset, Self::position(ui, id));
        if let Some(mesh) = self.control.mesh.as_mut() {
            mesh.set_transform(Transform::new(Matrix::IDENTITY, at));
        }
    }

    /// Properties 0xd, 0xf and 0x11: the instance turned back by the old
    /// angle and on by the new one.
    fn turn_instance(&mut self, col_a: usize, col_b: usize, old: f32, new: f32) {
        let Some(mesh) = self.control.mesh.as_mut() else {
            return;
        };
        let mut t = mesh.transform();
        let back = -f64::from(old);
        rotate_columns(
            &mut t.rotation,
            col_a,
            col_b,
            f64::from(back.cos() as f32),
            back.sin(),
        );
        let forward = f64::from(new);
        rotate_columns(&mut t.rotation, col_a, col_b, forward.cos(), forward.sin());
        mesh.set_transform(t);
    }

    /// The parent's slot 5 reaching this control again (divergence D347).
    fn slot5_from_parent(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(p) = crate::ui::control::parent_slot5_position(ui, id) {
            self.set_position(ui, id, p);
        }
    }

    /// `StaticControl::SetPosition`.
    pub fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.control.field_2e0 = position;
        let placed = if self.control.position_includes_offset {
            add3(self.control.attach_position(ui, id), position)
        } else {
            position
        };
        ui.base_set_position(id, placed);
        if let Some(mesh) = self.control.mesh.as_mut() {
            let at = if self.control.position_includes_offset {
                add3(placed, self.control.offset)
            } else {
                placed
            };
            mesh.set_position(at);
        }
        for child in ui.children(id) {
            ui.set_position(child, placed);
        }
    }

    /// `StaticControl::SetOrientation`: times the attach orientation; the
    /// instance turned by the pitch, the yaw and the roll.
    pub fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        let attach = self.control.attach_orientation(ui, id);
        let result = mul_matrix(&orientation, &attach);
        if let Some(n) = ui.node_mut(id) {
            n.orientation = result;
        }
        let Some(mesh) = self.control.mesh.as_mut() else {
            return;
        };
        let mut turned = result;
        let pitch = f64::from(self.cur_pitch);
        rotate_columns(
            &mut turned,
            1,
            2,
            f64::from(pitch.cos() as f32),
            pitch.sin(),
        );
        let yaw = f64::from(self.cur_yaw);
        rotate_columns(&mut turned, 2, 0, f64::from(yaw.cos() as f32), yaw.sin());
        let roll = f64::from(self.cur_roll);
        rotate_columns(&mut turned, 0, 1, f64::from(roll.cos() as f32), roll.sin());
        mesh.set_orientation(turned);
    }

    /// `StaticControl::Create`.
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
            let position = Self::position(ui, p);
            ui.set_position(p, position);
            self.slot5_from_parent(ui, id);
        }
        true
    }

    fn read_ini(&mut self, ui: &mut Ui, id: NodeId, parent: Option<NodeId>, ini: &mut IniReader) {
        while ini.read_value() {
            if ini.is_value(b"mesh") {
                let path = ini.value_string();
                if base::set_mesh(ui, id, &mut self.control, &path, b"") {
                    self.slot5_from_parent(ui, id);
                }
            } else if ini.is_value(b"mouse_size") {
                self.control.half_extents = ini_vector(ini);
            } else if ini.is_value(b"offset") {
                self.control.offset = ini_vector(ini);
                self.place_instance_at_offset(ui, id);
            } else if ini.is_value(b"hardpoint") {
                let name = ini.value_string();
                self.control.hardpoint = name.clone();
                if let Some(found) = ask_hardpoint(ui, &mut self.control, parent, &name) {
                    self.control.attached = found;
                }
            } else if ini.is_value(b"no_z_enable") {
                self.control.no_z_enable = ini_flag(ini);
            } else if ini.is_value(b"zclear") {
                self.zclear = ini_flag(ini);
            } else if ini.is_value(b"no_mesh_render") {
                self.no_mesh_render = ini_flag(ini);
            } else if ini.is_value(b"no_wire_render") {
                self.no_wire_render = ini_flag(ini);
            } else if ini.is_value(b"no_coloring") {
                self.no_coloring = ini_flag(ini);
            } else if ini.is_value(b"render_to_window") {
                self.render_to_window = ini_flag(ini);
            } else if ini.is_value(b"no_clear_render") {
                self.no_clear_render = ini_flag(ini);
            } else if ini.is_value(b"force_radius") {
                self.force_radius = ini.value_float(0) as f32;
            } else if ini.is_value(b"rotation_speed") {
                self.rotation_speed = ini.value_float(0) as f32;
            } else if ini.is_value(b"enable_rotation") {
                self.enable_rotation = ini_flag(ini);
            } else if ini.is_value(b"starting_yaw_pitch_roll") {
                let pitch = ini.value_float(1) as f32;
                let yaw = ini.value_float(0) as f32;
                let roll = ini.value_float(2) as f32;
                self.start_ypr = [yaw, pitch, roll];
                self.cur_yaw = yaw;
                self.cur_pitch = pitch;
                self.cur_roll = roll;
            }
        }
    }

    /// `StaticControl::Init`.
    #[allow(clippy::too_many_arguments)]
    pub fn init(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        mesh: Option<&[u8]>,
        offset: Vector,
        hardpoint: Option<&[u8]>,
        half_extents: Vector,
        no_z_enable: bool,
        zclear: bool,
        no_mesh_render: bool,
        no_wire_render: bool,
        no_coloring: bool,
        render_to_window: bool,
        no_clear_render: bool,
        force_radius: i32,
        rotation_speed: i32,
        enable_rotation: bool,
        start_ypr: [f32; 3],
    ) {
        if let Some(path) = mesh
            && base::set_mesh(ui, id, &mut self.control, path, b"")
        {
            self.slot5_from_parent(ui, id);
        }
        self.control.half_extents = half_extents;
        self.control.offset = offset;
        self.place_instance_at_offset(ui, id);
        if let Some(name) = hardpoint {
            self.control.hardpoint = name.to_vec();
            let parent = ui.parent(id);
            if let Some(found) = ask_hardpoint(ui, &mut self.control, parent, name) {
                self.control.attached = found;
            }
        }
        self.control.no_z_enable = no_z_enable;
        self.force_radius = force_radius as f32;
        self.zclear = zclear;
        self.no_mesh_render = no_mesh_render;
        self.rotation_speed = rotation_speed as f32;
        self.no_wire_render = no_wire_render;
        self.no_coloring = no_coloring;
        self.render_to_window = render_to_window;
        self.no_clear_render = no_clear_render;
        self.enable_rotation = enable_rotation;
        self.start_ypr = start_ypr;
        self.cur_yaw = start_ypr[0];
        self.cur_pitch = start_ypr[1];
        self.cur_roll = start_ypr[2];
    }

    /// Property 0x17: a new engine instance bound (the old one released),
    /// every angle cleared, the instance placed at the offset.
    pub fn bind_instance(&mut self, ui: &Ui, id: NodeId, mesh: Option<Box<dyn ControlMesh>>) {
        self.control.mesh = mesh;
        self.start_ypr = [0.0; 3];
        self.cur_roll = 0.0;
        self.cur_pitch = 0.0;
        self.cur_yaw = 0.0;
        self.place_instance_at_offset(ui, id);
    }

    /// `StaticControl::Update`.
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        if self.control.mesh.is_none() || self.rotation_speed == 0.0 {
            ui.base_update(id);
            return;
        }
        if self.enable_rotation {
            self.cur_yaw =
                (ui.frame_time * f64::from(self.rotation_speed) + f64::from(self.cur_yaw)) as f32;
            self.set_orientation(ui, id, Matrix::IDENTITY);
            ui.base_update(id);
            return;
        }
        let d_yaw = f64::from(self.start_ypr[0]) - f64::from(self.cur_yaw);
        let d_pitch = f64::from(self.start_ypr[1]) - f64::from(self.cur_pitch);
        let d_roll = f64::from(self.start_ypr[2]) - f64::from(self.cur_roll);
        let eps = f64::from(ANGLE_EPSILON);
        if !(d_yaw * d_yaw <= eps) || !(d_pitch * d_pitch <= eps) || !(d_roll * d_roll <= eps) {
            self.cur_pitch = self.start_ypr[1];
            self.cur_yaw = self.start_ypr[0];
            self.cur_roll = self.start_ypr[2];
            self.set_orientation(ui, id, Matrix::IDENTITY);
        }
        ui.base_update(id);
    }

    /// The render-to-window rectangle: the instance's box projected by the
    /// Win3D camera, truncated, shrunk by two pixels; `None` when it is
    /// off the surface.
    fn window_rect(&self, ui: &Ui, placed: Vector) -> Option<[i32; 4]> {
        let h = self.control.half_extents;
        let project = |p: Vector| ui.camera.project(p).map_or((0.0, 0.0), |(x, y, _)| (x, y));
        let (x0, y0) = project(Vector::new(placed.x - h.x, placed.y - h.y, placed.z - h.z));
        let (mut left, mut bottom) = (x0 as i32, y0 as i32);
        let (x1, y1) = project(Vector::new(placed.x + h.x, placed.y + h.y, placed.z + h.z));
        let (mut right, mut top) = (x1 as i32, y1 as i32);
        if left < 0 || top < 0 || right < 0 || bottom < 0 {
            (left, top, right, bottom) = (0, 0, 0, 0);
        }
        let rect = [left + 2, top + 2, right - 2, bottom - 2];
        let surface = ui.camera.surface;
        if rect[1] >= surface.height || rect[3] >= surface.height {
            return None;
        }
        if rect[0] >= surface.width || rect[2] >= surface.width {
            return None;
        }
        if rect.iter().any(|&v| v < 0) {
            return None;
        }
        Some(rect)
    }

    /// `StaticControl::Draw`.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ui.draw_children(id, host);
        if host.game_option(OPT_NEURONET) == 0
            || host.game_option(OPT_INTERFACE) == 0
            || host.game_option(OPT_STATIC_CONTROLS) == 0
        {
            return;
        }
        let Some(radius) = self.control.mesh.as_ref().map(|m| m.radius(0)) else {
            return;
        };
        let (mut mesh_rgba, mut wire_rgba) = if self.use_colours {
            (self.mesh_colour, self.wire_colour)
        } else {
            (palette::IDLE.to_le_bytes(), palette::IDLE.to_le_bytes())
        };
        let active = ui.node(id).is_some_and(|n| n.flags & ACTIVE != 0);
        if !active {
            wire_rgba = palette::GREY.to_le_bytes();
        }
        if self.no_coloring {
            mesh_rgba = [0xFF; 4];
        }
        let scale = if self.force_radius != 0.0 {
            self.force_radius / radius
        } else {
            1.0
        };
        let mut placed = Vector::ZERO;
        if self.render_to_window {
            placed = self
                .control
                .mesh
                .as_ref()
                .map_or(Vector::ZERO, |m| m.position());
            let Some(rect) = self.window_rect(ui, placed) else {
                return;
            };
            if let Some(mesh) = self.control.mesh.as_mut() {
                mesh.set_position(Vector::new(0.0, 0.0, WINDOW_DEPTH));
            }
            host.window_view(Some(WindowView {
                rect,
                z_near: ui.camera.z_near,
                z_far: ui.camera.z_far,
                fov_y_deg: WINDOW_FOV_Y,
            }));
        }
        let no_z = self.control.no_z_enable;
        if !self.no_mesh_render {
            let pass = if no_z {
                if let Some(imm) = host.immediate() {
                    imm.enable(capability::DEPTH_TEST);
                }
                if self.zclear {
                    if let Some(imm) = host.immediate() {
                        imm.set_render_state(render_state::ZFUNC, 8);
                        imm.set_render_state(render_state::ZWRITEENABLE, 1);
                    }
                    MeshPass::Direct {
                        tint: mesh_rgba,
                        scale,
                    }
                } else {
                    MeshPass::Body {
                        tint: mesh_rgba,
                        front_cull: true,
                        scale,
                    }
                }
            } else {
                if let Some(imm) = host.immediate() {
                    imm.disable(capability::DEPTH_TEST);
                }
                MeshPass::Body {
                    tint: mesh_rgba,
                    front_cull: false,
                    scale,
                }
            };
            if let Some(mesh) = self.control.mesh.as_deref() {
                host.submit_mesh(mesh, pass);
            }
        }
        if let Some(imm) = host.immediate() {
            imm.disable(capability::DEPTH_TEST);
        }
        if !self.no_wire_render {
            let pass = if no_z {
                MeshPass::TintedScaled {
                    tint: wire_rgba,
                    front_cull: true,
                    lighting: false,
                    scale,
                }
            } else {
                MeshPass::Outlined {
                    wire: wire_rgba,
                    scale,
                }
            };
            if let Some(mesh) = self.control.mesh.as_deref() {
                host.submit_mesh(mesh, pass);
            }
        }
        if self.render_to_window {
            host.window_view(None);
            if let Some(mesh) = self.control.mesh.as_mut() {
                mesh.put_position(placed);
            }
        }
        if let Some(imm) = host.immediate() {
            imm.disable(capability::DEPTH_TEST);
        }
    }

    /// `StaticControl::MouseEvent`: a button event is taken and told to the
    /// parent (0x1B for a right press, else 0x1A).
    pub fn mouse_event(ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if event.field0() != -1 {
            return false;
        }
        let msg = if event.field1() == 3 { 0x1B } else { 0x1A };
        tell_parent(ui, id, msg, 0);
        true
    }

    /// `StaticControl::ShowToolTip`.
    pub fn show_tooltip(&self, ui: &mut Ui, id: NodeId) {
        let world_pos = match &self.control.mesh {
            Some(mesh) => mesh.transform().translation,
            None => add3(Self::position(ui, id), self.control.offset),
        };
        let hover = ToolTipHover {
            key: HoverKey::Ids(self.control.tooltip_ids),
            ids_alt: self.control.tooltip_ids,
            world_pos,
            half_extents: self.control.half_extents,
            live: true,
            ..Default::default()
        };
        ui.feed_tooltip_hover(&hover);
    }

    /// `StaticControl::SetProperty`'s own messages.
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
        let bits = |a: usize| f32::from_bits(a as u32);
        Ok(match property {
            Property::Other(0x0C, a, _) => {
                self.field_347 = a != 0;
                0
            }
            Property::Other(0x0D, a, _) => {
                self.turn_instance(0, 1, self.cur_roll, bits(a));
                self.cur_roll = bits(a);
                self.start_ypr[2] = bits(a);
                0
            }
            Property::Other(0x0E, _, _) => self.cur_roll.to_bits().cast_signed() as isize,
            Property::Other(0x0F, a, _) => {
                self.turn_instance(1, 2, self.cur_pitch, bits(a));
                self.cur_pitch = bits(a);
                self.start_ypr[1] = bits(a);
                0
            }
            Property::Other(0x10, _, _) => self.cur_pitch.to_bits().cast_signed() as isize,
            Property::Other(0x11, a, _) => {
                self.turn_instance(2, 0, self.cur_yaw, bits(a));
                self.cur_yaw = bits(a);
                self.start_ypr[0] = bits(a);
                0
            }
            Property::Other(0x12, _, _) => self.cur_yaw.to_bits().cast_signed() as isize,
            Property::Colours(mesh, wire) => {
                self.use_colours = true;
                self.mesh_colour = mesh;
                self.wire_colour = wire;
                0
            }
            Property::Other(0x19, a, b) => {
                self.no_mesh_render = a & 0xFFFF != 0;
                self.no_wire_render = a >> 16 != 0;
                self.control.no_z_enable = b & 0xFFFF != 0;
                self.no_coloring = b >> 16 != 0;
                0
            }
            Property::Other(0x1C, a, _) => {
                self.enable_rotation = a == 1;
                if !self.enable_rotation {
                    self.set_orientation(ui, id, Matrix::IDENTITY);
                }
                0
            }
            Property::Other(0x4D, a, _) => {
                self.force_radius = (a as u32) as f32;
                0
            }
            other => return Err(other),
        })
    }
}

/// The "StaticControl" factory product.
#[derive(Debug, Default)]
pub struct StaticControlWidget {
    pub control: StaticControl,
}

impl Widget for StaticControlWidget {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.control.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.control.control)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        let this = &self.control;
        base::hit_test_with(ui, id, &this.control, ray, |ui| this.show_tooltip(ui, id))
    }
    fn show_tooltip(&self, ui: &mut Ui, id: NodeId) {
        self.control.show_tooltip(ui, id);
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.control.set_position(ui, id, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        self.control.set_orientation(ui, id, orientation);
    }
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        self.control.create(ui, id, parent, payload)
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.control.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.control.control);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.control.update(ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.control.draw(ui, id, host);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        StaticControl::mouse_event(ui, id, event)
    }
    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
        base::broadcast(ui, id, self, msg)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        match self.control.property(ui, id, property) {
            Ok(r) => r,
            Err(p) => base::property(ui, id, self, p),
        }
    }
}
