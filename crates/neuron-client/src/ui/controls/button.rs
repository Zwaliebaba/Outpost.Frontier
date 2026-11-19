//! `ButtonControl`, `ToggleButtonControl` and `RichTextButtonControl`
//! (`nn_button_control.cpp`): the NeuroNet and HUD mesh buttons.
//!
//! A button is a `WinControl` whose mesh (or the button-montage icon of
//! its mesh path) is drawn tinted by its state: grey while inactive,
//! yellow while selected or pressed, the hover pulse while focused or
//! hovered, else the idle cyan; a glow pass outlines it. A press holds the
//! mouse, follows the cursor on and off the button, and its release over
//! the button plays the click sound and posts the button's key-map command
//! or tells the parent 0x26 (0x27 for the right button). It can spin and
//! blink. A toggle button has a second mesh, tooltip and sound for its
//! state 1 and flips its state on each click. `RichTextButtonControl` adds
//! nothing to a button.
//!
//! The spin and the toggle's half turn take their sines and cosines from
//! the Rust library (Tier B, decision T11).

use std::any::Any;

use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector, mul_matrix};
use neuron_core::num::trunc_to_i32;

use crate::ui::control::{
    ControlMesh, WinControl, base, cursor_ray_inline, cursor_ray_near_plane, tell_parent,
};
use crate::ui::controls::static_control::{ask_hardpoint, ini_vector};
use crate::ui::controls::text_control::{OPT_BUTTON_CONTROLS, OPT_INTERFACE, OPT_NEURONET};
use crate::ui::immediate::capability;
use crate::ui::message::MouseButton;
use crate::ui::pulse::palette;
use crate::ui::services::{HoverKey, MontageTile, ToolTipHover};
use crate::ui::tree::{ACTIVE, DrawHost, MeshPass, NodeId, Payload, Property, Ui, VISIBLE, Widget};

/// The icon size without a tile scale.
const DEFAULT_ICON_SIZE: f32 = 0.022;
/// `sqrt(2)` as a float.
const SQRT_2: f32 = f32::from_bits(0x3FB5_04F3);
/// `pi` rounded to a float, as a double: the toggle's glow half turn.
const TOGGLE_GLOW_ANGLE: f64 = 3.141_592_741_012_573_2;

/// `ButtonKeyCommandLookup`: the command and tooltip of an `event` name
/// (`stricmp`, first match), zeros for any other.
#[must_use]
pub fn key_command(name: &[u8]) -> (i32, i32) {
    const TABLE: [(&[u8], i32, i32); 5] = [
        (b"tractor", 0x8B, 0xBC2),
        (b"next", 0x1D, 0xBB9),
        (b"prev", 0x1E, 0xBBA),
        (b"next_subtarget", 0x21, 0x41F),
        (b"prev_subtarget", 0x22, 0x420),
    ];
    TABLE
        .iter()
        .find(|(n, ..)| neuron_core::hash::eq_ignore_case(n, name))
        .map_or((0, 0), |&(_, c, t)| (c, t))
}

/// `RotateMatrix({0, angle, 0})`: the turn about y of an angle in degrees
/// (none at or below 0.01 degrees).
#[must_use]
pub fn rotate_matrix_y(degrees: f32) -> Matrix {
    if !(f64::from(degrees).abs() > 0.01) {
        return Matrix::IDENTITY;
    }
    let angle = degrees * 0.017_453_292;
    let c = f64::from(angle).cos() as f32;
    let s = f64::from(angle).sin() as f32;
    let rot = Matrix::from_rows([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]]);
    mul_matrix(&Matrix::IDENTITY, &rot)
}

/// The toggle button's state-1 parts.
#[derive(Default)]
pub struct Toggle {
    /// `+0x378`: the state-1 mesh.
    pub mesh: Option<Box<dyn ControlMesh>>,
    /// `+0x3a0`.
    pub state: i32,
    /// `+0x3a4`, `+0x3b4`: the button sounds of the two states.
    pub sounds: [Vec<u8>; 2],
    /// `+0x3c4`, `+0x3c8`.
    pub slots: [i32; 2],
}

impl std::fmt::Debug for Toggle {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Toggle")
            .field("has_mesh", &self.mesh.is_some())
            .field("state", &self.state)
            .finish_non_exhaustive()
    }
}

/// `ButtonControl` (and, with [`Toggle`] parts, `ToggleButtonControl`).
#[derive(Debug)]
pub struct ButtonControl {
    pub control: WinControl,
    /// `+0x32c` "tooltip2".
    pub tooltip2: i32,
    /// `+0x330`: 0x01 hovered, 0x02 focused, 0x04 lit while pressed, 0x08
    /// action button, 0x10 holds the press, 0x20 no colour change, 0x40
    /// selected, 0x80 the press was the right button.
    pub bits: u8,
    /// `+0x331`: 0x01 no mesh render, 0x02 enabled, 0x04 light while
    /// pressed, 0x08 pulse while hovered, 0x10 draw while inactive.
    pub bits2: u8,
    pub spin: bool,
    pub blink: bool,
    pub blink_phase: bool,
    pub montage_mode: u8,
    pub use_colour: bool,
    pub glow: bool,
    pub montage_tile: Option<MontageTile>,
    /// Degrees.
    pub spin_angle: f32,
    pub spin_speed: f32,
    pub blink_timer: f32,
    /// `+0x348`: the packed ARGB of the last glow pass.
    pub glow_argb: u32,
    /// `+0x34c`: the key-map command a click posts.
    pub command_id: i32,
    pub mesh2: Vec<u8>,
    pub sound_name: Vec<u8>,
    pub colour: [u8; 4],
    pub ui_sound_slot: i32,
    pub toggle: Option<Toggle>,
}

impl Default for ButtonControl {
    fn default() -> Self {
        ButtonControl::new()
    }
}

fn add3(a: Vector, b: Vector) -> Vector {
    Vector::new(a.x + b.x, a.y + b.y, a.z + b.z)
}

fn position(ui: &Ui, id: NodeId) -> Vector {
    ui.node(id).map_or(Vector::ZERO, |n| n.position)
}

/// The blink: the frame time added, the phase flipped once per whole
/// second past 1.
pub fn run_blink(ui: &Ui, timer: &mut f32, phase: &mut bool) {
    let mut t = ui.frame_time + f64::from(*timer);
    *timer = t as f32;
    if !(t > 1.0) {
        return;
    }
    let mut p = *phase;
    loop {
        t -= 1.0;
        p = !p;
        if !(t > 1.0) {
            break;
        }
    }
    *timer = t as f32;
    *phase = p;
}

impl ButtonControl {
    /// The constructor.
    #[must_use]
    pub fn new() -> ButtonControl {
        ButtonControl {
            control: WinControl::new(),
            tooltip2: 0,
            bits: 0,
            bits2: 0x0E,
            spin: false,
            blink: false,
            blink_phase: false,
            montage_mode: 0,
            use_colour: false,
            glow: false,
            montage_tile: None,
            spin_angle: 0.0,
            spin_speed: 180.0,
            blink_timer: 0.0,
            glow_argb: 0,
            command_id: 0,
            mesh2: Vec::new(),
            sound_name: Vec::new(),
            colour: [0; 4],
            ui_sound_slot: 0,
            toggle: None,
        }
    }

    /// The toggle button's constructor.
    #[must_use]
    pub fn new_toggle() -> ButtonControl {
        ButtonControl {
            toggle: Some(Toggle::default()),
            ..ButtonControl::new()
        }
    }

    fn placed_instance(&self, ui: &Ui, id: NodeId) -> Vector {
        let p = position(ui, id);
        if self.control.position_includes_offset {
            add3(self.control.offset, p)
        } else {
            p
        }
    }

    /// `ButtonControl::SetPosition` (and the toggle's, which then puts the
    /// main instance's transform on the state-1 one).
    pub fn set_position(&mut self, ui: &mut Ui, id: NodeId, new_position: Vector) {
        self.control.field_2e0 = new_position;
        self.control.attached = true;
        let placed = if self.control.position_includes_offset {
            add3(new_position, self.control.attach_position(ui, id))
        } else {
            new_position
        };
        base::set_position(ui, id, &mut self.control, placed);
        let at = self.placed_instance(ui, id);
        if let Some(mesh) = self.control.mesh.as_mut() {
            mesh.set_position(at);
        }
        if let Some(t) = self.toggle.as_mut()
            && let (Some(tm), Some(main)) = (t.mesh.as_mut(), self.control.mesh.as_ref())
        {
            tm.set_transform(main.transform());
        }
    }

    /// `ButtonControl::SetOrientation` (and the toggle's): times the attach
    /// orientation, onto the instance; no children.
    pub fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        self.control.attached = true;
        let attach = self.control.attach_orientation(ui, id);
        let result = mul_matrix(&orientation, &attach);
        if let Some(n) = ui.node_mut(id) {
            n.orientation = result;
        }
        if let Some(mesh) = self.control.mesh.as_mut() {
            mesh.set_orientation(result);
        }
        if let Some(t) = self.toggle.as_mut()
            && let (Some(tm), Some(main)) = (t.mesh.as_mut(), self.control.mesh.as_ref())
        {
            tm.set_orientation(main.transform().rotation);
        }
    }

    /// The instance moved to the offset plus the position.
    fn move_instance_to_offset(&mut self, ui: &Ui, id: NodeId) {
        let at = add3(position(ui, id), self.control.offset);
        if let Some(mesh) = self.control.mesh.as_mut() {
            mesh.set_position(at);
        }
    }

    /// `ButtonControl::Create` (the toggle's: montage mode 1 first, the
    /// state-1 mesh and sounds after).
    pub fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        if self.toggle.is_some() {
            self.montage_mode = 1;
        }
        ui.base_create(id, parent);
        if let Payload::Ini(ini) = payload {
            self.read_ini(ui, id, parent, ini);
            let path = self.control.mesh_path.clone();
            self.montage_tile = ui.montage_tile(&path);
        }
        if let Some(p) = ui.parent(id) {
            let at = position(ui, p);
            ui.set_position(p, at);
            if let Some(pp) = crate::ui::control::parent_slot5_position(ui, id) {
                self.set_position(ui, id, pp);
            }
        }
        if self.toggle.is_some() {
            if !self.mesh2.is_empty() {
                self.load_toggle_mesh(ui);
                self.share_toggle_floats();
                self.place_toggle_mesh(ui, id);
            }
            self.toggle_sounds_from_button(ui);
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
            } else if ini.is_value(b"mesh2") {
                self.mesh2 = ini.value_string();
            } else if ini.is_value(b"button_sound") {
                let value = ini.value_string();
                if value.is_empty() {
                    continue;
                }
                self.ui_sound_slot = ui.services.find_ui_sound_slot(&value);
                self.sound_name = value;
            } else if ini.is_value(b"event") {
                let value = ini.value_string();
                if value.is_empty() {
                    continue;
                }
                let (command, tooltip) = key_command(&value);
                self.command_id = command;
                self.control.tooltip_ids = tooltip;
                if command != 0 || tooltip != 0 {
                    base::refresh_attachment(ui, id, &mut self.control);
                    self.slot5_from_parent(ui, id);
                }
            } else if ini.is_value(b"offset") {
                self.control.offset = ini_vector(ini);
                self.move_instance_to_offset(ui, id);
            } else if ini.is_value(b"hardpoint") {
                let name = ini.value_string();
                ask_hardpoint(ui, &mut self.control, parent, &name);
            } else if ini.is_value(b"mouse_size") {
                self.control.half_extents = ini_vector(ini);
            } else if ini.is_value(b"no_mesh_render") {
                let bit = u8::from(ini.value_float(0) != 0.0);
                self.bits2 = (self.bits2 & 0xFE) | bit;
            } else if ini.is_value(b"action_button") {
                let bit = if ini.value_float(0) != 0.0 { 0x08 } else { 0 };
                self.bits = (self.bits & 0xF7) | bit;
            } else if ini.is_value(b"no_color_change") {
                let bit = if ini.value_float(0) != 0.0 { 0x20 } else { 0 };
                self.bits = (self.bits & 0xDF) | bit;
            } else if ini.is_value(b"no_z_enable") {
                self.control.no_z_enable = ini.value_float(0) != 0.0;
            } else if ini.is_value(b"tooltip") {
                self.control.tooltip_ids = trunc_to_i32(ini.value_float(0));
            } else if ini.is_value(b"tooltip2") {
                self.tooltip2 = trunc_to_i32(ini.value_float(0));
            }
        }
    }

    /// `ButtonControl::Init` (`command` 0 for the toggle's).
    #[allow(clippy::too_many_arguments)]
    pub fn init(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        mesh: Option<&[u8]>,
        mesh2: Option<&[u8]>,
        sound: Option<&[u8]>,
        key_command_name: Option<&[u8]>,
        offset: Vector,
        hardpoint: Option<&[u8]>,
        half_extents: Vector,
        no_mesh_render: u8,
        action_button: u8,
        no_color_change: u8,
        no_z_enable: bool,
        tooltip: i32,
        tooltip2: i32,
        command: i32,
    ) {
        if self.toggle.is_some() {
            self.montage_mode = 1;
        }
        if let Some(path) = mesh {
            self.control.mesh_path = path.to_vec();
            self.montage_tile = ui.montage_tile(path);
            if self.montage_tile.is_none_or(|t| t.scale == 0.0)
                && base::set_mesh(ui, id, &mut self.control, path, b"")
            {
                self.slot5_from_parent(ui, id);
            }
        }
        if let Some(m2) = mesh2 {
            self.mesh2 = m2.to_vec();
        }
        if let Some(sound) = sound.filter(|s| !s.is_empty()) {
            self.sound_name = sound.to_vec();
            self.ui_sound_slot = ui.services.find_ui_sound_slot(sound);
        }
        if let Some(name) = key_command_name.filter(|s| !s.is_empty()) {
            let (c, t) = key_command(name);
            self.command_id = c;
            self.control.tooltip_ids = t;
            if c != 0 || t != 0 {
                base::refresh_attachment(ui, id, &mut self.control);
                self.slot5_from_parent(ui, id);
            }
        }
        self.control.offset = offset;
        self.move_instance_to_offset(ui, id);
        if let Some(name) = hardpoint {
            self.control.hardpoint = name.to_vec();
            let parent = ui.parent(id);
            ask_hardpoint(ui, &mut self.control, parent, name);
        }
        self.control.half_extents = half_extents;
        self.bits2 = (self.bits2 & 0xFE) | (no_mesh_render & 1);
        self.bits = (self.bits & 0xF7) | ((action_button << 3) & 0x08);
        self.bits = (self.bits & 0xDF) | ((no_color_change << 5) & 0x20);
        self.control.no_z_enable = no_z_enable;
        self.control.tooltip_ids = tooltip;
        self.tooltip2 = tooltip2;
        self.command_id = command;
        if self.toggle.is_some() {
            if let Some(m2) = mesh2 {
                let path = m2.to_vec();
                self.load_toggle_mesh_from(ui, &path);
                self.share_toggle_floats();
                self.place_toggle_mesh(ui, id);
            }
            self.toggle_sounds_from_button(ui);
        }
    }

    fn load_toggle_mesh_from(&mut self, ui: &mut Ui, path: &[u8]) {
        let mesh = ui.mesh_loader.as_mut().and_then(|l| l.load(path, b""));
        if let Some(t) = self.toggle.as_mut() {
            t.mesh = mesh;
        }
    }

    /// `SetInstanceUserData(state-1 instance, main holder's floats)`: the
    /// state-1 instance reads the tint the body pass writes for the main
    /// holder (divergence D440).
    fn share_toggle_floats(&mut self) {
        let floats = self.control.mesh.as_ref().and_then(|m| m.user_floats());
        if let (Some(floats), Some(t)) = (floats, self.toggle.as_mut())
            && let Some(m2) = t.mesh.as_mut()
        {
            m2.set_root_user_floats(floats);
        }
    }

    fn load_toggle_mesh(&mut self, ui: &mut Ui) {
        let path = self.mesh2.clone();
        self.load_toggle_mesh_from(ui, &path);
    }

    /// `PlaceToggleMesh`: the state-1 instance at the offset plus the
    /// position, turned as the main one; then slot 35.
    fn place_toggle_mesh(&mut self, ui: &mut Ui, id: NodeId) {
        let at = add3(self.control.offset, position(ui, id));
        if let Some(t) = self.toggle.as_mut()
            && let Some(tm) = t.mesh.as_mut()
        {
            tm.set_position(at);
            if let Some(main) = self.control.mesh.as_ref() {
                tm.set_orientation(main.transform().rotation);
            }
        }
        base::refresh_attachment(ui, id, &mut self.control);
        self.slot5_from_parent(ui, id);
    }

    /// The parent's slot 5 reaching this button again after a refresh
    /// (divergence D347).
    fn slot5_from_parent(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(p) = crate::ui::control::parent_slot5_position(ui, id) {
            self.set_position(ui, id, p);
        }
    }

    /// State 0 and both state sounds from the button sound.
    fn toggle_sounds_from_button(&mut self, ui: &mut Ui) {
        let name = self.sound_name.clone();
        let slot = ui.services.find_ui_sound_slot(&name);
        let slot2 = ui.services.find_ui_sound_slot(&name);
        if let Some(t) = self.toggle.as_mut() {
            t.state = 0;
            t.sounds = [name.clone(), name];
            t.slots = [slot, slot2];
        }
    }

    /// `ButtonControl::Update` and the toggle's.
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        if self.toggle.is_some() {
            return self.update_toggle(ui, id);
        }
        ui.base_update(id);
        if self.bits & 0x10 != 0 {
            let ray = cursor_ray_inline(ui);
            if self.hit_test(ui, id, ray) {
                if self.bits & 0x04 == 0 {
                    self.bits |= 0x04;
                    tell_parent(ui, id, 0x2B, 0);
                    if ui.parent(id).is_some() {
                        ui.pulse.reset();
                        tell_parent(ui, id, 0x2C, 0);
                    }
                }
            } else if self.bits & 0x04 != 0 {
                self.bits &= 0xFB;
                tell_parent(ui, id, 0x2A, 0);
                tell_parent(ui, id, 0x2D, 0);
            }
        }
        if self.blink {
            run_blink(ui, &mut self.blink_timer, &mut self.blink_phase);
        }
    }

    fn update_toggle(&mut self, ui: &mut Ui, id: NodeId) {
        let unbound = self.toggle.as_ref().is_some_and(|t| t.mesh.is_none());
        if unbound && !self.mesh2.is_empty() {
            self.load_toggle_mesh(ui);
            self.place_toggle_mesh(ui, id);
        }
        ui.base_update(id);
        let ray = ui.mouse_ray();
        if self.hit_test(ui, id, ray) {
            let state = self.toggle.as_ref().map_or(0, |t| t.state);
            if state == 1 {
                if self.tooltip2 != 0 {
                    self.feed_toggle_tooltip(ui, self.tooltip2);
                }
            } else if self.control.tooltip_ids != 0 {
                self.feed_toggle_tooltip(ui, self.control.tooltip_ids);
            }
        }
        if self.bits & 0x10 != 0 {
            let ray = cursor_ray_near_plane(ui);
            let hit = self.hit_test(ui, id, ray);
            let bits = self.bits;
            if hit {
                if bits & 0x01 == 0 {
                    tell_parent(ui, id, 0x2B, 0);
                    if ui.parent(id).is_some() {
                        ui.pulse.reset();
                        tell_parent(ui, id, 0x2C, 0);
                    }
                }
            } else if bits & 0x04 != 0 {
                tell_parent(ui, id, 0x2A, 0);
                tell_parent(ui, id, 0x2D, 0);
            }
        }
        if self.blink {
            run_blink(ui, &mut self.blink_timer, &mut self.blink_phase);
        }
    }

    /// The toggle's hover record: the ids, the main instance's position.
    fn feed_toggle_tooltip(&self, ui: &mut Ui, ids: i32) {
        let world_pos = self
            .control
            .mesh
            .as_ref()
            .map_or(Vector::ZERO, |m| m.transform().translation);
        let hover = ToolTipHover {
            key: HoverKey::Ids(ids),
            ids_alt: ids,
            world_pos,
            half_extents: self.control.half_extents,
            live: true,
            ..Default::default()
        };
        ui.feed_tooltip_hover(&hover);
    }

    /// Slot 4: `WinControl::HitTest` with the button's slot 34.
    pub fn hit_test(&self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        base::hit_test_with(ui, id, &self.control, ray, |ui| self.show_tooltip(ui, id))
    }

    /// `ButtonControl::ShowToolTip`: the command as the record's binding.
    pub fn show_tooltip(&self, ui: &mut Ui, id: NodeId) {
        let world_pos = match &self.control.mesh {
            Some(mesh) => mesh.transform().translation,
            None => add3(position(ui, id), self.control.offset),
        };
        let hover = ToolTipHover {
            key: HoverKey::Ids(self.control.tooltip_ids),
            ids_alt: self.control.tooltip_ids,
            aux: self.command_id,
            world_pos,
            half_extents: self.control.half_extents,
            live: true,
            ..Default::default()
        };
        ui.feed_tooltip_hover(&hover);
    }

    /// The button body: the montage icon, else the tinted instance.
    fn draw_body(&self, ui: &Ui, id: NodeId, host: &mut dyn DrawHost, rgba: [u8; 4]) {
        let mut size = DEFAULT_ICON_SIZE;
        if let Some(tile) = self.montage_tile {
            size = tile.scale;
            if size == 0.0 {
                size = self.control.mesh.as_ref().map_or(0.0, |m| m.radius(1));
            }
        }
        size *= SQRT_2;
        let at = match &self.control.mesh {
            Some(mesh) => mesh.position(),
            None => add3(self.control.offset, position(ui, id)),
        };
        if host.draw_montage_tile(self.montage_tile, at, rgba, size) {
            return;
        }
        if let Some(mesh) = self.control.mesh.as_deref() {
            host.submit_mesh(
                mesh,
                MeshPass::Body {
                    tint: rgba,
                    front_cull: false,
                    scale: 1.0,
                },
            );
        }
    }

    /// `ButtonControl::Draw`.
    pub fn draw_button(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.glow_argb = 0;
        if host.game_option(OPT_INTERFACE) == 0
            || host.game_option(OPT_NEURONET) == 0
            || host.game_option(OPT_BUTTON_CONTROLS) == 0
        {
            return;
        }
        let flags = ui.node(id).map_or(0, |n| n.flags);
        if flags & VISIBLE == 0 {
            return;
        }
        if self.bits2 & 0x10 == 0 && flags & ACTIVE == 0 {
            return;
        }
        if self.blink && self.blink_phase {
            return;
        }
        ui.draw_children(id, host);
        let saved_selected = (self.bits >> 6) & 1;
        if self.spin {
            let frame = ui.frame_time;
            if self.spin_speed > 0.0 {
                let step = frame * f64::from(self.spin_speed);
                if self.bits & 0x01 != 0 {
                    self.spin_angle = (step + f64::from(self.spin_angle)) as f32;
                } else if self.spin_angle > 180.0 {
                    let angle = step + step + f64::from(self.spin_angle);
                    self.spin_angle = angle as f32;
                    if angle > 359.0 {
                        self.spin_angle = 0.0;
                    }
                } else {
                    let angle = f64::from(self.spin_angle) - (step + step);
                    self.spin_angle = angle as f32;
                    if angle < 1.0 {
                        self.spin_angle = 0.0;
                    }
                }
            }
            if self.spin_angle >= 360.0 {
                self.spin_angle -= 360.0;
            } else if self.spin_angle <= -360.0 {
                self.spin_angle += 360.0;
            }
            let spun = rotate_matrix_y(self.spin_angle);
            if let Some(mesh) = self.control.mesh.as_mut() {
                mesh.set_orientation(spun);
            }
        }
        let (mut glow, mut body);
        if flags & ACTIVE == 0 {
            self.bits &= 0xBB;
            glow = [0x80, 0x80, 0x80, 0xFF];
            body = glow;
        } else if self.bits & 0x40 != 0 || (self.bits2 & 0x04 != 0 && self.bits & 0x04 != 0) {
            glow = [0xFF, 0xFF, 0x00, 0xFF];
            body = glow;
        } else if self.bits & 0x02 != 0 || (self.bits & 0x01 != 0 && self.bits2 & 0x08 != 0) {
            body = ui.pulse.blend().to_le_bytes();
            glow = ui.pulse.blend().to_le_bytes();
        } else {
            glow = palette::IDLE.to_le_bytes();
            body = glow;
        }
        if self.bits & 0x20 != 0 {
            body = [0xFF; 4];
        }
        if self.use_colour {
            body = self.colour;
            glow = self.colour;
        }
        if self.bits2 & 0x01 == 0 {
            if self.spin && self.bits & 0x01 != 0 {
                if let Some(mesh) = self.control.mesh.as_deref() {
                    host.submit_mesh(
                        mesh,
                        MeshPass::Body {
                            tint: body,
                            front_cull: false,
                            scale: 1.0,
                        },
                    );
                }
            } else {
                self.draw_body(ui, id, host, body);
            }
        }
        if let Some(imm) = host.immediate() {
            imm.disable(capability::DEPTH_TEST);
        }
        if (self.bits2 & 0x01 != 0 || self.glow)
            && let Some(mesh) = self.control.mesh.as_deref()
        {
            self.glow_argb = (u32::from(glow[3]) << 24)
                | (u32::from(glow[0]) << 16)
                | (u32::from(glow[1]) << 8)
                | u32::from(glow[2]);
            host.submit_mesh(
                mesh,
                MeshPass::Tinted {
                    tint: glow,
                    lighting: true,
                },
            );
        }
        if let Some(imm) = host.immediate() {
            imm.disable(capability::DEPTH_TEST);
        }
        self.bits = (self.bits & 0xBF) | (saved_selected << 6);
    }

    /// `ButtonControl::Draw` and the toggle's: in state 1 the state-1
    /// instance stands in for the main one; after a glow pass the glow
    /// again, half a turn about y.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        let Some(state) = self.toggle.as_ref().map(|t| t.state) else {
            return self.draw_button(ui, id, host);
        };
        if state == 1 {
            self.swap_toggle_mesh();
            self.draw_button(ui, id, host);
            self.swap_toggle_mesh();
        } else {
            self.draw_button(ui, id, host);
        }
        if let Some(imm) = host.immediate() {
            imm.disable(capability::DEPTH_TEST);
        }
        if self.glow_argb != 0 {
            let argb = self.glow_argb;
            let instance = if state == 1 {
                self.toggle.as_mut().and_then(|t| t.mesh.as_mut())
            } else {
                self.control.mesh.as_mut()
            };
            if let Some(mesh) = instance {
                let saved = mesh.orientation();
                let c = TOGGLE_GLOW_ANGLE.cos() as f32;
                let s = TOGGLE_GLOW_ANGLE.sin() as f32;
                let turned = Matrix::from_rows([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]]);
                mesh.set_orientation(turned);
                let glow = [
                    (argb >> 16) as u8,
                    (argb >> 8) as u8,
                    argb as u8,
                    (argb >> 24) as u8,
                ];
                host.submit_mesh(
                    mesh.as_ref(),
                    MeshPass::Tinted {
                        tint: glow,
                        lighting: true,
                    },
                );
                mesh.put_orientation(saved);
            }
        }
        if let Some(imm) = host.immediate() {
            imm.disable(capability::DEPTH_TEST);
        }
    }

    fn swap_toggle_mesh(&mut self) {
        if let Some(t) = self.toggle.as_mut() {
            std::mem::swap(&mut t.mesh, &mut self.control.mesh);
        }
    }

    /// `ButtonControl::MouseEnter`.
    pub fn mouse_enter(&mut self, ui: &mut Ui, id: NodeId) {
        if ui.parent(id).is_some() {
            ui.pulse.reset();
            tell_parent(ui, id, 0x2C, 0);
        }
        self.bits |= 0x01;
        if self.bits2 & 0x02 != 0 {
            ui.services.play_ui_rollover_sound();
        }
    }

    /// `ButtonControl::MouseLeave`.
    pub fn mouse_leave(&mut self, ui: &mut Ui, id: NodeId) {
        if self.control.tooltip_ids != 0 {
            ui.hide_tooltip();
        }
        tell_parent(ui, id, 0x2D, 0);
        self.bits &= 0xFE;
    }

    /// `ButtonControl::MouseEvent` (the toggle's takes its state's sound
    /// first).
    pub fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if let Some(t) = self.toggle.as_ref() {
            let i = usize::from(t.state != 0);
            self.sound_name = t.sounds[i].clone();
            self.ui_sound_slot = t.slots[i];
        }
        if self.bits2 & 0x02 == 0 {
            return false;
        }
        if let Some(parent) = ui.parent(id)
            && ui.any_captured()
            && ui.win_control(parent).is_some_and(|c| !c.captured)
        {
            return false;
        }
        if self.bits & 0x10 != 0 {
            if event.field0() != -1 || event.field1() != 1 {
                return false;
            }
            self.bits &= 0xEB;
            ui.remove_capture(id);
            let ray = cursor_ray_near_plane(ui);
            if !self.hit_test(ui, id, ray) {
                tell_parent(ui, id, 0x29, 0);
                self.bits &= 0xFE;
                return true;
            }
            if self.ui_sound_slot != -1 {
                ui.services
                    .play_or_rewind_ui_sound_slot(self.ui_sound_slot.cast_unsigned());
            } else if !self.sound_name.is_empty() {
                let name = self.sound_name.clone();
                ui.services.play_sound_arch(&name);
            }
            if self.bits & 0x80 != 0 {
                tell_parent(ui, id, 0x27, 0);
                return true;
            }
            self.activate(ui, id);
            return true;
        }
        if event.field0() != -1 {
            return false;
        }
        if event.field1() != 2 && event.field1() != 3 {
            return false;
        }
        if event.field1() == 3 {
            self.bits |= 0x80;
        } else {
            self.bits &= 0x7F;
        }
        tell_parent(ui, id, 0x28, 0);
        self.bits |= 0x14;
        if ui.parent(id).is_some() {
            ui.pulse.reset();
            tell_parent(ui, id, 0x2C, 0);
        }
        ui.push_capture(id);
        true
    }

    /// `Activate` (slot 45): the command, else the parent hears 0x26; a
    /// toggle then flips its state and tells the parent 0x30.
    pub fn activate(&mut self, ui: &mut Ui, id: NodeId) {
        if self.command_id != 0 {
            ui.services.post_command(self.command_id.cast_unsigned());
        } else {
            tell_parent(ui, id, 0x26, 0);
        }
        if let Some(t) = self.toggle.as_mut() {
            t.state = i32::from(t.state == 0);
            let state = t.state;
            tell_parent(ui, id, 0x30, state);
        }
    }

    /// `ButtonControl::SetProperty`'s own messages (and the toggle's).
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
        if let Some(t) = self.toggle.as_mut() {
            match property {
                Property::Offset(offset) => {
                    self.control.offset = offset;
                    if self.control.mesh.is_some() {
                        let p = position(ui, id);
                        let at = if self.control.position_includes_offset {
                            add3(self.control.offset, p)
                        } else {
                            p
                        };
                        if let Some(tm) = t.mesh.as_mut() {
                            tm.set_position(at);
                        }
                    }
                    return Err(Property::Offset(offset));
                }
                Property::GetToggleState(out) => {
                    *out = t.state;
                    return Ok(0);
                }
                Property::Other(0x30, a, _) => {
                    t.state = i32::from(a == 0);
                    return Ok(0);
                }
                Property::ToggleSounds(s0, s1) => {
                    if let Some(s) = s0 {
                        t.sounds[0] = s.to_vec();
                        t.slots[0] = ui.services.find_ui_sound_slot(s);
                    }
                    if let Some(s) = s1 {
                        t.sounds[1] = s.to_vec();
                        t.slots[1] = ui.services.find_ui_sound_slot(s);
                    }
                    return Ok(0);
                }
                _ => {}
            }
        }
        Ok(match property {
            Property::Other(0x0C, a, _) => {
                self.bits = (self.bits & 0xBF) | if a != 0 { 0x40 } else { 0 };
                0
            }
            Property::Other(0x2C, ..) => {
                self.bits |= 0x02;
                0
            }
            Property::Other(0x2D, ..) => {
                self.bits &= 0xFD;
                0
            }
            Property::GetCommand(out) => {
                *out = self.command_id;
                0
            }
            Property::Other(0x32, a, _) => {
                self.bits2 = (self.bits2 & 0xFE) | u8::from(a != 0);
                0
            }
            Property::Colour(rgba, on) => {
                self.colour = rgba;
                self.use_colour = on;
                0
            }
            Property::Other(0x34, a, _) => {
                self.bits2 = (self.bits2 & 0xEF) | if a != 0 { 0x10 } else { 0 };
                0
            }
            Property::Other(0x35, a, _) => {
                self.glow = a != 0;
                0
            }
            Property::Other(0x36, a, _) => {
                self.spin = a == 1;
                0
            }
            Property::Other(0x51, a, _) => {
                self.blink = a != 0;
                0
            }
            other => return Err(other),
        })
    }

    /// `~ButtonControl`: a held press leaves the capture stack.
    pub fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        if self.bits & 0x10 != 0 {
            ui.remove_capture(id);
        }
        if let Some(t) = self.toggle.as_mut() {
            t.mesh = None;
        }
        base::on_delete(ui, id, &mut self.control);
    }
}

/// Mouse events, slots and properties shared by every button widget.
macro_rules! button_slots {
    ($field:ident) => {
        fn win_control(&self) -> Option<&WinControl> {
            Some(&self.$field.control)
        }
        fn win_control_mut(&mut self) -> Option<&mut WinControl> {
            Some(&mut self.$field.control)
        }
        fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
            self.$field.hit_test(ui, id, ray)
        }
        fn show_tooltip(&self, ui: &mut Ui, id: NodeId) {
            self.$field.show_tooltip(ui, id);
        }
        fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
            self.$field.set_position(ui, id, position);
        }
        fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
            self.$field.set_orientation(ui, id, orientation);
        }
        fn create(
            &mut self,
            ui: &mut Ui,
            id: NodeId,
            parent: Option<NodeId>,
            payload: &mut Payload<'_>,
        ) -> bool {
            self.$field.create(ui, id, parent, payload)
        }
        fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
            base::destroy(ui, id, &mut self.$field.control);
        }
        fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
            self.$field.on_delete(ui, id);
        }
        fn update(&mut self, ui: &mut Ui, id: NodeId) {
            self.$field.update(ui, id);
        }
        fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
            self.$field.draw(ui, id, host);
        }
        fn mouse_enter(&mut self, ui: &mut Ui, id: NodeId, _ray: Vector) {
            self.$field.mouse_enter(ui, id);
        }
        fn mouse_leave(&mut self, ui: &mut Ui, id: NodeId, _ray: Vector) {
            self.$field.mouse_leave(ui, id);
        }
        fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
            self.$field.mouse_event(ui, id, event)
        }
        fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
            base::broadcast(ui, id, self, msg)
        }
        fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
            match self.$field.property(ui, id, property) {
                Ok(r) => r,
                Err(p) => base::property(ui, id, self, p),
            }
        }
    };
}

/// The "ButtonControl" factory product.
#[derive(Debug, Default)]
pub struct ButtonWidget {
    pub button: ButtonControl,
}

impl Widget for ButtonWidget {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    button_slots!(button);
}

/// The "ToggleButtonControl" factory product.
#[derive(Debug)]
pub struct ToggleButtonWidget {
    pub button: ButtonControl,
}

impl Default for ToggleButtonWidget {
    fn default() -> Self {
        ToggleButtonWidget {
            button: ButtonControl::new_toggle(),
        }
    }
}

impl Widget for ToggleButtonWidget {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    button_slots!(button);
}

/// The "RichTextButtonControl" factory product: a button with two fields
/// nothing reads.
#[derive(Debug, Default)]
pub struct RichTextButtonWidget {
    pub button: ButtonControl,
}

impl Widget for RichTextButtonWidget {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    button_slots!(button);
}
