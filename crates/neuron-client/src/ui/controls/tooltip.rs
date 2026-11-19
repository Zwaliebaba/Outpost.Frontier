//! `ToolTip` (`nn_tooltip.cpp`) and `Rollover` (`nn_rollover.cpp`): the two
//! text windows controls describe themselves through.
//!
//! Both are `TextWin`s kept in a singleton of the manager
//! ([`Ui::tooltip`], [`Ui::rollover`]) and ticked by its update. A control
//! under the mouse feeds a hover record (`FToolTipHoverInfo`,
//! [`ToolTipHover`]) to the tooltip; the tooltip hands it on to the
//! rollover, then shows the id's string (with the key bound to the
//! record's command) in a framed box under the cursor or at the control's
//! box once the cursor has rested for a quarter second, and hides when no
//! record came in a frame. The rollover maps the record's id through the
//! `[RolloverTable]` of `interface\Rollover.ini` and shows the mapped text
//! at a fixed anchor of the screen; the game can also show an id there for
//! a while.

use std::any::Any;
use std::collections::BTreeMap;

use neuron_core::ini::IniReader;
use neuron_core::math::{Vector, transform_matrix};
use neuron_core::num::trunc_to_i32;

use crate::text::font::string::DrawOrigin;
use crate::ui::controls::text_control::OPT_INTERFACE;
use crate::ui::controls::text_control::OPT_TEXT_CONTROLS;
use crate::ui::controls::text_win::{TextWin, text_point};
use crate::ui::immediate::{capability, primitive};
use crate::ui::pulse::HUD_CHROME_RGBA;
use crate::ui::services::{HoverKey, ToolTipHover};
use crate::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Ui, VISIBLE, Widget};

/// The framed box both windows draw behind their text: the floored
/// origin, the extent plus 3 by 2, filled with `fill` then outlined in the
/// HUD chrome colour, in a 2D projection.
fn draw_box(host: &mut dyn DrawHost, x: f32, y: f32, width: f32, height: f32, fill: u32) {
    let Some(imm) = host.immediate() else { return };
    let x0 = f64::from(x).floor() as f32;
    let y0 = f64::from(y).floor() as f32;
    let x1 = x0 + (width + 3.0);
    let y1 = y0 + (height + 2.0);
    imm.push_projection();
    imm.set_ortho_2d();
    imm.color4ubv(fill.to_le_bytes());
    imm.disable(capability::TEXTURE_2D);
    imm.begin(primitive::QUADS);
    imm.vertex(x0, y0, 0.0);
    imm.vertex(x1, y0, 0.0);
    imm.vertex(x1, y1, 0.0);
    imm.vertex(x0, y1, 0.0);
    imm.end();
    imm.color4ubv(HUD_CHROME_RGBA);
    imm.begin(primitive::LINE_LOOP);
    imm.vertex(x0, y0, 0.0);
    imm.vertex(x1, y0, 0.0);
    imm.vertex(x1, y1, 0.0);
    imm.vertex(x0, y1, 0.0);
    imm.end();
    imm.pop_projection();
}

/// The string's extent: laid out through the host when there is one, else
/// the last layout's (divergence D437).
fn text_extent_of(tw: &mut TextWin, ui: &mut Ui, host: Option<&mut dyn DrawHost>) -> (i32, i32) {
    if let Some(host) = host
        && let (Some(sys), Some(cx)) = (ui.fonts.as_mut(), host.paint_cx())
    {
        return tw.text.extent(sys, cx.pipeline);
    }
    tw.text.extents()
}

/// `FontStringSetScale` on a text window's string.
fn set_scale(tw: &mut TextWin, ui: &Ui, width: f32, depth: f32) {
    tw.text
        .set_scale(width, depth, ui.camera.x_scale, ui.camera.z_near);
}

/// The font of a text window's string by name, rebound when it changed.
fn bind_font(tw: &mut TextWin, ui: &mut Ui, name: &[u8]) {
    if let Some(sys) = ui.fonts.as_mut() {
        let font = sys.find_font(name);
        tw.text.set_font(sys, font);
    }
}

fn flags_and(ui: &mut Ui, id: NodeId, mask: u8) {
    if let Some(n) = ui.node_mut(id) {
        n.flags &= mask;
    }
}

fn flags_or(ui: &mut Ui, id: NodeId, bits: u8) {
    if let Some(n) = ui.node_mut(id) {
        n.flags |= bits;
    }
}

/// `ToolTip`.
#[derive(Clone, Debug, PartialEq)]
pub struct ToolTip {
    pub text_win: TextWin,
    /// `+0xf4`: the record shown.
    pub hover: ToolTipHover,
    /// `+0x158`: the tip was visible at the last tick.
    pub built: bool,
    /// `+0x159`.
    pub has_text: bool,
    /// `+0x164`: a record came in this frame.
    pub fed_this_frame: bool,
    /// `+0x15c`, `+0x160`, `+0x168`.
    pub delay_remain: f32,
    pub delay_accum: f32,
    pub delay_threshold: f32,
    /// `+0xec`, `+0xf0`.
    pub offset_x: f32,
    pub offset_y: f32,
    /// `G_TipCursorX`, `G_TipCursorY`.
    pub last_cursor: (i32, i32),
}

impl Default for ToolTip {
    fn default() -> Self {
        let mut hover = ToolTipHover::default();
        hover.reset();
        ToolTip {
            text_win: TextWin::new(),
            hover,
            built: false,
            has_text: false,
            fed_this_frame: false,
            delay_remain: 0.0,
            delay_accum: 0.0,
            delay_threshold: 0.25,
            offset_x: 4.0,
            offset_y: -42.0,
            last_cursor: (0, 0),
        }
    }
}

impl ToolTip {
    /// `InitToolTipDefaults`.
    fn init_defaults(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(n) = ui.node_mut(id) {
            n.draw_layer = 10;
        }
        self.delay_remain = 0.0;
        self.delay_accum = 0.0;
        self.built = false;
        self.has_text = false;
        self.fed_this_frame = false;
        self.delay_threshold = 0.25;
        self.offset_x = 4.0;
        self.offset_y = -42.0;
        bind_font(&mut self.text_win, ui, b"ToolTip");
        self.text_win.text.set_multi_line(true);
    }

    /// `ReadHover` (`NnToolTipReadHover`).
    #[must_use]
    pub fn read_hover(&self) -> ToolTipHover {
        ToolTipHover {
            caption: None,
            aux: 0,
            ..self.hover.clone()
        }
    }

    /// `UpdateFromHover`.
    pub fn update_from_hover(&mut self, ui: &mut Ui, id: NodeId, info: &ToolTipHover) {
        self.fed_this_frame = true;
        if !ui.cursor_drawn {
            self.has_text = false;
            flags_and(ui, id, !VISIBLE);
            return;
        }
        if let Some(r) = ui.rollover {
            ui.with_widget(r, (), |w, ui| {
                if let Some(roll) = w.as_any_mut().downcast_mut::<Rollover>() {
                    roll.feed_hover(ui, r, info);
                }
            });
        }
        if f64::from(ui.services.perf_option(b"enable_tooltips")) < 0.999 {
            return;
        }
        let same_pos = |h: &ToolTipHover| {
            h.world_pos.x == info.world_pos.x && h.world_pos.y == info.world_pos.y
        };
        if self.has_text {
            let same = self.hover.key == info.key
                && self.hover.live == info.live
                && (!self.hover.live || same_pos(&self.hover));
            if same {
                flags_and(ui, id, !VISIBLE);
                return;
            }
            self.has_text = false;
            self.delay_remain = 0.0;
            self.delay_accum = 0.0;
        }
        let rebuild = self.hover.key != info.key
            || self.hover.live != info.live
            || (self.hover.live && !same_pos(&self.hover))
            || !self.built;
        if rebuild {
            if self.delay_remain <= 0.0 && self.delay_accum < self.delay_threshold {
                return;
            }
            self.apply_hover(ui, id, info);
        }
        flags_or(ui, id, ACTIVE | VISIBLE);
    }

    /// `ApplyHover`: the record's live fields, the tip's colours, the
    /// caption with the key bound to its command, the place.
    pub fn apply_hover(&mut self, ui: &mut Ui, id: NodeId, info: &ToolTipHover) {
        self.hover.key = info.key;
        self.hover.ids_alt = info.ids_alt;
        self.hover.world_pos = info.world_pos;
        self.hover.half_extents = info.half_extents;
        self.hover.live = info.live;
        let tw = &mut self.text_win;
        tw.color_d8 = 0xFF70_6143;
        tw.color_d4 = 0x19E0_C387;
        tw.color_idle = 0xFFFF_F7B9;
        tw.text_scale = 0.0011;
        tw.flag_e0 = 1;
        tw.flag_e1 = 1;
        let mut resolved: Vec<u16> = match &info.caption {
            None => {
                let mut s = ui.ids_string(self.hover.ids_alt.cast_unsigned());
                s.truncate(0x7F);
                s
            }
            Some(c) => crate::text::font::wide_c_str(c).to_vec(),
        };
        if resolved == [u16::from(b' ')] {
            self.has_text = false;
            flags_and(ui, id, !VISIBLE);
            return;
        }
        if info.aux != 0
            && let Some(name) = ui.services.key_binding_name(info.aux)
        {
            resolved.extend(" (".encode_utf16());
            resolved.extend_from_slice(crate::text::font::wide_c_str(&name));
            resolved.push(u16::from(b')'));
        }
        self.text_win.set_text(ui, &resolved);
        self.reposition(ui, id, info);
    }

    /// `RepositionAfterCaption`: under the cursor's ray on the z = -1
    /// plane (or at a live record's box corner), kept inside the viewport.
    fn reposition(&mut self, ui: &mut Ui, id: NodeId, info: &ToolTipHover) {
        let cam = ui.camera;
        let top_left_local =
            cam.screen_to_near_plane(cam.viewport[0] as f32, cam.viewport[1] as f32);
        let top_left = transform_matrix(&cam.world, top_left_local);
        let near = f64::from(cam.z_near);
        let bottom_right_local = Vector::new(
            ((f64::from(cam.viewport[2]) - f64::from(cam.screen_center_x)) / f64::from(cam.x_scale)
                * near) as f32,
            ((f64::from(cam.viewport[3]) - f64::from(cam.screen_center_y)) / f64::from(cam.y_scale)
                * near) as f32,
            -cam.z_near,
        );
        let bottom_right = transform_matrix(&cam.world, bottom_right_local);
        let ray = ui.mouse_ray();
        let ray_scale = -1.0 / f64::from(ray.z);
        let mut pos_x = (f64::from(ray.x) * ray_scale) as f32;
        let mut pos_y = (f64::from(ray.y) * ray_scale) as f32;
        let mut pos_z = (ray_scale * f64::from(ray.z)) as f32;
        let bound_left = (f64::from(top_left.x) * (-1.0 / f64::from(top_left.z))) as f32;
        let bound_scale = -1.0 / f64::from(bottom_right.z);
        let bound_right = (f64::from(bottom_right.x) * bound_scale) as f32;
        let bound_vert = (f64::from(bottom_right.y) * bound_scale) as f32;
        if info.live {
            let scale = -1.0 / f64::from(info.world_pos.z);
            let stored = f64::from(scale as f32);
            pos_x = ((f64::from(info.world_pos.x) - f64::from(info.half_extents.x)) * scale) as f32;
            pos_y =
                ((f64::from(info.world_pos.y) - f64::from(info.half_extents.y)) * stored) as f32;
            pos_z = (f64::from(info.world_pos.z) * stored) as f32;
            pos_x = (f64::from(pos_x) + f64::from(0.004_f32)) as f32;
            pos_y = (f64::from(pos_y) - f64::from(0.03_f32)) as f32;
        }
        let depth = ui.node(id).map_or(0.0, |n| n.position.z);
        set_scale(&mut self.text_win, ui, 0.35, depth);
        let (extent_w, _) = text_extent_of(&mut self.text_win, ui, None);
        let extent_w = f64::from(extent_w.cast_unsigned().wrapping_add(4));
        // The reference divides by (near / near) * x scale.
        #[allow(clippy::eq_op)]
        let width = extent_w / ((near / near) * f64::from(cam.x_scale));
        if f64::from(pos_x) + width > f64::from(bound_right) {
            pos_x = (f64::from(bound_right) - width) as f32;
        }
        if pos_x < bound_left {
            pos_x = (f64::from(bound_left) + f64::from(0.004_f32)) as f32;
        }
        let vert_bias = f64::from(self.offset_y) * f64::from(self.text_win.text_scale);
        if f64::from(pos_y) + vert_bias < f64::from(bound_vert) {
            pos_y = (f64::from(pos_y) - vert_bias * 1.5) as f32;
        }
        ui.set_position(id, Vector::new(pos_x, pos_y, pos_z));
    }

    /// `TickToolTip`.
    pub fn tick(&mut self, ui: &mut Ui, id: NodeId, delta: f64) {
        if !self.fed_this_frame {
            flags_and(ui, id, !VISIBLE);
            self.has_text = false;
        }
        self.fed_this_frame = false;
        if !ui.cursor_drawn {
            self.has_text = false;
            flags_and(ui, id, !VISIBLE);
            return;
        }
        let flags = ui.node(id).map_or(0, |n| n.flags);
        if flags & VISIBLE == 0 {
            flags_and(ui, id, !ACTIVE);
            if self.delay_remain > 0.0 {
                self.delay_remain = (f64::from(self.delay_remain) - delta) as f32;
                if self.delay_remain < 0.0 {
                    self.delay_remain = 0.0;
                }
            }
        } else {
            self.delay_remain = self.delay_threshold;
        }
        let flags = ui.node(id).map_or(0, |n| n.flags);
        self.built = flags & VISIBLE != 0;
        flags_and(ui, id, !VISIBLE);
        if self.last_cursor == ui.cursor {
            self.delay_accum = (f64::from(self.delay_accum) + delta) as f32;
        } else {
            self.last_cursor = ui.cursor;
            self.delay_accum = 0.0;
        }
        if ui.services.mouse_buttons() & 3 != 0 {
            self.has_text = true;
        }
    }

    /// `HideNow`.
    pub fn hide(&mut self, ui: &mut Ui, id: NodeId) {
        flags_and(ui, id, !VISIBLE);
        self.has_text = false;
    }

    /// `ToolTip::Draw`.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        if host.game_option(OPT_INTERFACE) == 0 || host.game_option(OPT_TEXT_CONTROLS) == 0 {
            return;
        }
        let Some(node) = ui.node(id) else { return };
        if node.flags & VISIBLE == 0 {
            return;
        }
        let position = node.position;
        if ui.services.letterbox_visible() {
            return;
        }
        host.end_draw();
        if let Some(imm) = host.immediate() {
            imm.set_depth_write(false);
            imm.disable(capability::DEPTH_TEST);
            imm.disable(capability::CULL_FACE);
        }
        set_scale(&mut self.text_win, ui, 0.35, position.z);
        let tw = &mut self.text_win;
        tw.bounds = [0.0, 0.0, tw.width, -tw.height];
        let (w, h) = text_extent_of(tw, ui, Some(host));
        tw.width = f64::from(w.cast_unsigned()) as f32;
        tw.height = f64::from(h.cast_unsigned()) as f32;
        let (sx, sy) = ui
            .camera
            .project(position)
            .map_or((0.0, 0.0), |(x, y, _)| (x, y));
        if tw.flag_e1 != 0 {
            draw_box(host, sx, sy, tw.width, tw.height, tw.color_d8);
        }
        let mut origin = DrawOrigin {
            x: sx + 1.0,
            y: sy,
            width: tw.width,
            height: tw.height,
            h_mode: 1,
            v_mode: 0,
        };
        if let Some(sys) = ui.fonts.as_mut() {
            sys.atlas.draw_color = tw.color_idle;
            if let Some(mut cx) = host.paint_cx() {
                tw.text.draw_children(sys, &mut cx, &mut origin, 0.0, 0.0);
            }
        }
        if let Some(imm) = host.immediate() {
            imm.enable(capability::DEPTH_TEST);
            imm.set_depth_write(true);
        }
    }
}

impl Widget for ToolTip {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    /// Never picked.
    fn hit_test(&mut self, _: &mut Ui, _: NodeId, _: Vector) -> bool {
        false
    }
    /// The factory's part (the defaults, hidden, the singleton), then
    /// `ToolTip::Create`: the `TextWin` defaults, a 0.1 box, hidden,
    /// sub-layer 7.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        self.init_defaults(ui, id);
        flags_and(ui, id, !VISIBLE);
        ui.tooltip = Some(id);
        ui.base_create(id, parent);
        self.text_win.init_text_defaults();
        self.text_win.width = 0.1;
        self.text_win.height = 0.1;
        if let Some(n) = ui.node_mut(id) {
            n.flags &= !VISIBLE;
            n.draw_sub_layer = 7;
        }
        true
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.text_win.destroy(ui, id);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        if ui.tooltip == Some(id) {
            ui.tooltip = None;
        }
        self.text_win.destroy(ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ToolTip::draw(self, ui, id, host);
    }
    fn tooltip_hover(&mut self, ui: &mut Ui, id: NodeId, hover: &ToolTipHover) {
        self.update_from_hover(ui, id, hover);
    }
    fn tooltip_hide(&mut self, ui: &mut Ui, id: NodeId) {
        self.hide(ui, id);
    }
}

/// The rollover's anchor height and depth.
const ROLLOVER_ANCHOR_Y: f32 = 0.385;

/// `Rollover`.
#[derive(Clone, Debug, PartialEq)]
pub struct Rollover {
    pub text_win: TextWin,
    /// `+0xec`: the record shown.
    pub entry: ToolTipHover,
    /// `+0x150`: `[RolloverTable] map = key, value`.
    pub table: BTreeMap<i32, i32>,
    /// `+0x164`.
    pub hold_seconds: f32,
    /// `+0x16c`.
    pub anchor: Vector,
    /// `+0x178`.
    pub was_visible: bool,
    /// `+0x17c`, `+0x180`.
    pub hidden_grace: f32,
    pub cursor_still: f32,
    /// `+0x184`, `+0x188`.
    pub anchor_x_space: f32,
    pub anchor_x_intro: f32,
    /// `G_RolloverCursorX`, `G_RolloverCursorY`.
    pub last_cursor: (i32, i32),
}

impl Default for Rollover {
    fn default() -> Self {
        let mut entry = ToolTipHover::default();
        entry.reset();
        Rollover {
            text_win: TextWin::new(),
            entry,
            table: BTreeMap::new(),
            hold_seconds: 0.0,
            anchor: Vector::ZERO,
            was_visible: false,
            hidden_grace: 0.0,
            cursor_still: 0.0,
            anchor_x_space: -0.51,
            anchor_x_intro: 0.2,
            last_cursor: (0, 0),
        }
    }
}

impl Rollover {
    /// `InitRolloverDefaults`.
    fn init_defaults(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(n) = ui.node_mut(id) {
            n.draw_layer = 10;
        }
        self.hold_seconds = 0.0;
        self.hidden_grace = 0.0;
        self.cursor_still = 0.0;
        self.anchor_x_space = -0.51;
        self.anchor_x_intro = 0.2;
        self.was_visible = false;
        bind_font(&mut self.text_win, ui, b"Rollover");
        self.text_win.text.max_lines = 4;
        self.text_win.text.set_multi_line(true);
    }

    /// `LoadRolloverTable` over the text of `interface\Rollover.ini`: the
    /// first value of a repeated key stays.
    pub fn load_table(&mut self, text: &[u8]) {
        let mut ini = IniReader::new();
        if !ini.open_memory(text) || !ini.read_header() {
            return;
        }
        loop {
            if ini.is_header(b"RolloverTable") {
                while ini.read_value() {
                    if ini.is_value(b"map") {
                        let key = trunc_to_i32(ini.value_float(0));
                        let value = trunc_to_i32(ini.value_float(1));
                        self.table.entry(key).or_insert(value);
                    }
                }
            }
            if !ini.read_header() {
                break;
            }
        }
    }

    fn anchor_x(&self, ui: &mut Ui) -> f32 {
        if ui.services.intro_environment() {
            self.anchor_x_intro
        } else {
            self.anchor_x_space
        }
    }

    fn enabled(ui: &mut Ui) -> bool {
        !(f64::from(ui.services.perf_option(b"enable_rollover")) < 0.999)
    }

    /// The record's text (the mapped id's string, else its caption), the
    /// place and the knowledge map.
    fn show_record(&mut self, ui: &mut Ui, id: NodeId, ids_alt: i32, caption: Option<&[u16]>) {
        if ids_alt != 0 {
            let s = ui.ids_string(ids_alt.cast_unsigned());
            self.text_win.set_text(ui, &s);
        } else {
            self.text_win.set_text(ui, caption.unwrap_or_default());
        }
        let anchor = self.anchor;
        ui.set_position(id, anchor);
        ui.services.knowledge_map_reveal(ids_alt.cast_unsigned());
    }

    /// `TickRollover`.
    pub fn tick(&mut self, ui: &mut Ui, id: NodeId, delta: f64) {
        if !ui.cursor_drawn {
            flags_and(ui, id, !VISIBLE);
            self.hold_seconds = 0.0;
            return;
        }
        let flags = ui.node(id).map_or(0, |n| n.flags);
        if flags & VISIBLE == 0 {
            flags_and(ui, id, !ACTIVE);
            if self.hidden_grace > 0.0 {
                let left = f64::from(self.hidden_grace) - delta;
                self.hidden_grace = left as f32;
                if left < 0.0 {
                    self.hidden_grace = 0.0;
                }
            }
        } else {
            self.hidden_grace = 0.5;
        }
        let flags = ui.node(id).map_or(0, |n| n.flags);
        self.was_visible = flags & VISIBLE != 0;
        if self.last_cursor == ui.cursor {
            self.cursor_still = (f64::from(self.cursor_still) + delta) as f32;
        } else {
            self.last_cursor = ui.cursor;
            self.cursor_still = 0.0;
        }
        if self.hold_seconds > 0.0 {
            let left = f64::from(self.hold_seconds) - delta;
            self.hold_seconds = left as f32;
            if left > 0.0 {
                return;
            }
            self.hold_seconds = 0.0;
        }
        flags_and(ui, id, !VISIBLE);
    }

    /// `FeedHover`: the tooltip's record mapped through the table.
    pub fn feed_hover(&mut self, ui: &mut Ui, id: NodeId, info: &ToolTipHover) {
        if info.ids_alt == 0 {
            return;
        }
        if !ui.cursor_drawn {
            flags_and(ui, id, !VISIBLE);
            return;
        }
        if !Self::enabled(ui) || self.hold_seconds > 0.0 {
            return;
        }
        if (self.entry.key != info.key || !self.was_visible)
            && !(self.hidden_grace > 0.0)
            && self.cursor_still < 0.25
        {
            return;
        }
        let Some(&mapped) = self.table.get(&info.ids_alt) else {
            return;
        };
        flags_or(ui, id, ACTIVE);
        if self.entry.key == info.key {
            flags_or(ui, id, VISIBLE);
            return;
        }
        self.anchor = Vector::new(self.anchor_x(ui), ROLLOVER_ANCHOR_Y, -1.0);
        self.entry.key = info.key;
        flags_or(ui, id, VISIBLE);
        self.entry.ids_alt = mapped;
        self.show_record(ui, id, mapped, info.caption.as_deref());
    }

    /// `ShowHover` (`anchor_x` `None`) and `ShowHoverAt`: a record shown
    /// for `hold` seconds.
    pub fn show_hover(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        info: &ToolTipHover,
        hold: f32,
        anchor_x: Option<f32>,
    ) {
        if info.ids_alt == 0 {
            return;
        }
        if !ui.cursor_drawn {
            flags_and(ui, id, !VISIBLE);
            return;
        }
        if !Self::enabled(ui) {
            return;
        }
        flags_or(ui, id, ACTIVE);
        if self.entry.key == info.key && self.hold_seconds > 0.0 {
            flags_or(ui, id, VISIBLE);
            return;
        }
        let x = match anchor_x {
            Some(x) => x,
            None => self.anchor_x(ui),
        };
        self.anchor = Vector::new(x, ROLLOVER_ANCHOR_Y, -1.0);
        self.hold_seconds = hold;
        flags_or(ui, id, VISIBLE);
        self.entry.key = info.key;
        self.entry.ids_alt = info.ids_alt;
        self.show_record(ui, id, info.ids_alt, info.caption.as_deref());
    }

    /// `HideRollover`.
    pub fn hide(&mut self, ui: &mut Ui, id: NodeId) {
        self.hold_seconds = 0.0;
        flags_and(ui, id, !VISIBLE);
    }

    /// `Rollover::Draw`.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        if host.game_option(OPT_INTERFACE) == 0 {
            return;
        }
        let Some(node) = ui.node(id) else { return };
        if node.flags & VISIBLE == 0 {
            return;
        }
        let position = node.position;
        if ui.services.letterbox_visible() {
            return;
        }
        if let Some(imm) = host.immediate() {
            imm.set_depth_write(false);
            imm.disable(capability::DEPTH_TEST);
            imm.disable(capability::CULL_FACE);
        }
        set_scale(&mut self.text_win, ui, 0.29, position.z);
        let (w, h) = text_extent_of(&mut self.text_win, ui, Some(host));
        let extent_w = f64::from(w.cast_unsigned()) as f32;
        let extent_h = f64::from(h.cast_unsigned()) as f32;
        let tw = &mut self.text_win;
        tw.width = extent_w;
        tw.height = extent_h;
        tw.bounds = [0.0, 0.0, extent_w, -extent_h];
        let (sx, sy) = text_point(ui, position);
        if tw.flag_e1 != 0 {
            draw_box(host, sx, sy, tw.width, tw.height, tw.color_d8);
        }
        let mut origin = DrawOrigin {
            x: sx + 1.0,
            y: sy,
            width: extent_w,
            height: extent_h,
            h_mode: 0,
            v_mode: 0,
        };
        if let Some(sys) = ui.fonts.as_mut() {
            sys.atlas.draw_color = tw.color_idle;
            if let Some(mut cx) = host.paint_cx() {
                tw.text.draw_children(sys, &mut cx, &mut origin, 0.0, 0.0);
            }
        }
        if let Some(imm) = host.immediate() {
            imm.enable(capability::DEPTH_TEST);
            imm.set_depth_write(true);
        }
    }
}

impl Widget for Rollover {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn hit_test(&mut self, _: &mut Ui, _: NodeId, _: Vector) -> bool {
        false
    }
    /// The factory's part (the defaults, hidden, the singleton), then
    /// `Rollover::Create`: the `TextWin` defaults, a 0.1 box, hidden,
    /// sub-layer 7, the colours, the anchor and the table.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        self.init_defaults(ui, id);
        flags_and(ui, id, !VISIBLE);
        ui.rollover = Some(id);
        ui.base_create(id, parent);
        let tw = &mut self.text_win;
        tw.init_text_defaults();
        tw.width = 0.1;
        tw.height = 0.1;
        tw.flag_e0 = 0;
        tw.text_scale = 0.0011;
        tw.flag_e1 = 1;
        tw.color_d8 = 0xFF43_4343;
        tw.color_idle = 0xFFE0_C387;
        if let Some(n) = ui.node_mut(id) {
            n.flags &= !VISIBLE;
            n.draw_sub_layer = 7;
        }
        self.anchor = Vector::new(self.anchor_x(ui), ROLLOVER_ANCHOR_Y, -1.0);
        if let Some(text) = ui.services.data_file(b"interface\\Rollover.ini") {
            self.load_table(&text);
        }
        true
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.text_win.destroy(ui, id);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        if ui.rollover == Some(id) {
            ui.rollover = None;
        }
        self.text_win.destroy(ui, id);
    }
    /// Message 0xEF02 hides the rollover.
    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
        if msg != crate::ui::message::MSG_HIDE {
            return false;
        }
        flags_and(ui, id, !VISIBLE);
        true
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        Rollover::draw(self, ui, id, host);
    }
}

impl Ui {
    /// `NnToolTipTickGlobal` and `NnRolloverTickGlobal`.
    pub fn tick_tooltips(&mut self, delta: f64) {
        if let Some(tip) = self.tooltip {
            self.with_widget(tip, (), |w, ui| {
                if let Some(t) = w.as_any_mut().downcast_mut::<ToolTip>() {
                    t.tick(ui, tip, delta);
                }
            });
        }
        if let Some(r) = self.rollover {
            self.with_widget(r, (), |w, ui| {
                if let Some(roll) = w.as_any_mut().downcast_mut::<Rollover>() {
                    roll.tick(ui, r, delta);
                }
            });
        }
    }

    /// `NnRolloverShowIdsGlobal` (and `...AtGlobal` with an anchor).
    pub fn rollover_show_ids(&mut self, ids: i32, hold: f32, anchor_x: Option<f32>) {
        let Some(r) = self.rollover else { return };
        let record = ToolTipHover::for_ids(ids);
        self.with_widget(r, (), |w, ui| {
            if let Some(roll) = w.as_any_mut().downcast_mut::<Rollover>() {
                roll.show_hover(ui, r, &record, hold, anchor_x);
            }
        });
    }
}

/// A [`HoverKey`] for an id, as the game's records use them.
#[must_use]
pub fn ids_key(ids: i32) -> HoverKey {
    HoverKey::Ids(ids)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ui::services::UiServices;
    use crate::ui::widescreen::Surface;

    struct Keys;

    impl UiServices for Keys {
        fn key_binding_name(&mut self, command: i32) -> Option<Vec<u16>> {
            (command == 29).then(|| "F1".encode_utf16().collect())
        }
        fn data_file(&mut self, _: &[u8]) -> Option<Vec<u8>> {
            Some(b"[RolloverTable]\nmap = 7, 70\nmap = 7, 71\nmap = 8, 0\n".to_vec())
        }
    }

    fn ui() -> Ui {
        let mut ui = Ui::new(crate::ui::widescreen::Surface::new(640, 480));
        ui.services = Box::new(Keys);
        ui
    }

    fn caption(s: &str) -> Option<Vec<u16>> {
        Some(s.encode_utf16().collect())
    }

    #[test]
    fn the_tooltip_waits_for_a_resting_cursor_then_shows_the_caption_and_binding() {
        let _ = Surface::default();
        let mut ui = ui();
        let tip = ui
            .create_by_class(Some(b"ToolTip"), b"ToolTip", None, &mut Payload::None)
            .unwrap();
        assert_eq!(ui.tooltip, Some(tip));
        assert_eq!(ui.node(tip).unwrap().draw_sub_layer, 7);
        assert_eq!(ui.node(tip).unwrap().draw_layer, 10);
        let hover = ToolTipHover {
            key: HoverKey::Ids(5),
            ids_alt: 5,
            caption: caption("Launch"),
            aux: 29,
            ..Default::default()
        };
        // The cursor has not rested: nothing is shown.
        ui.feed_tooltip_hover(&hover);
        assert_eq!(ui.node(tip).unwrap().flags & VISIBLE, 0);
        // A resting cursor builds up the delay.
        ui.tick_tooltips(0.2);
        ui.tick_tooltips(0.2);
        ui.feed_tooltip_hover(&hover);
        let t = ui.widget::<ToolTip>(tip).unwrap();
        let shown: Vec<u16> = "Launch (F1)".encode_utf16().collect();
        assert_eq!(t.text_win.text.text(), Some(shown.as_slice()));
        assert_eq!(t.text_win.color_idle, 0xFFFF_F7B9);
        assert_ne!(ui.node(tip).unwrap().flags & VISIBLE, 0);
        // A caption of one space drops the text; the feed's tail shows the
        // window again, as the reference's does.
        let blank = ToolTipHover {
            key: HoverKey::Ids(6),
            caption: caption(" "),
            ..hover.clone()
        };
        ui.tick_tooltips(0.016);
        ui.feed_tooltip_hover(&blank);
        assert!(!ui.widget::<ToolTip>(tip).unwrap().has_text);
        assert_eq!(
            ui.widget::<ToolTip>(tip).unwrap().hover.key,
            HoverKey::Ids(6)
        );
        ui.hide_tooltip();
        assert!(!ui.widget::<ToolTip>(tip).unwrap().has_text);
        ui.destroy(tip);
        ui.delete(tip);
        assert_eq!(ui.tooltip, None);
    }

    #[test]
    fn the_rollover_maps_ids_through_its_table_and_holds_shown_ids() {
        let mut ui = ui();
        let roll = ui
            .create_by_class(Some(b"Rollover"), b"Rollover", None, &mut Payload::None)
            .unwrap();
        let r = ui.widget::<Rollover>(roll).unwrap();
        // The first value of a repeated key stays.
        assert_eq!(r.table.get(&7), Some(&70));
        assert_eq!(r.anchor, Vector::new(-0.51, 0.385, -1.0));
        let hover = ToolTipHover {
            key: HoverKey::Ids(7),
            ids_alt: 7,
            ..Default::default()
        };
        ui.with_widget(roll, (), |w, ui| {
            let r = w.as_any_mut().downcast_mut::<Rollover>().unwrap();
            // Not rested and no grace: nothing.
            r.feed_hover(ui, roll, &hover);
            assert_eq!(r.entry.key, HoverKey::Ids(-1));
            r.cursor_still = 0.3;
            r.feed_hover(ui, roll, &hover);
            assert_eq!(r.entry.key, HoverKey::Ids(7));
            assert_eq!(r.entry.ids_alt, 70);
        });
        assert_ne!(ui.node(roll).unwrap().flags & VISIBLE, 0);
        assert_eq!(
            ui.node(roll).unwrap().position,
            Vector::new(-0.51, 0.385, -1.0)
        );
        ui.rollover_show_ids(9, 2.0, Some(0.25));
        let r = ui.widget::<Rollover>(roll).unwrap();
        assert_eq!(r.hold_seconds, 2.0);
        assert_eq!(r.anchor.x, 0.25);
        ui.tick_tooltips(1.5);
        assert_ne!(ui.node(roll).unwrap().flags & VISIBLE, 0);
        ui.tick_tooltips(1.0);
        assert_eq!(ui.node(roll).unwrap().flags & VISIBLE, 0);
        assert!(ui.send_broadcast(roll, crate::ui::message::MSG_HIDE, 0, 0));
    }
}
