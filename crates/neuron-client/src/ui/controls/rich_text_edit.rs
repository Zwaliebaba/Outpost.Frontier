//! `RichTextEditWin` (`rich_text_win.cpp`, second half), the editable
//! rich-text box, and `UITextEdit` (`ui_text_edit.cpp`), the page
//! descriptor's edit field over it.
//!
//! The text is a row of cells, each a character with the attributes it was
//! typed with. Every change rebuilds the window's source list as two
//! references, one to the prefix list and one to the generated text list,
//! which holds an attribute node at every change of attributes (or one
//! reference to the fixed-style list) and a text node per run (`*` for
//! each character in password mode). Keys and characters come from the
//! HUD's command dispatcher as events 1 and 2 while the box holds its
//! input focus ([`RichTextEditWin::activate`]); the game's dispatcher
//! calls [`RichTextEditWin::on_event`] with the event's record. The caret
//! blinks in the update and is drawn as one line through the immediate
//! mode layer after the window's image, at the laid-out position of the
//! character after it.
//!
//! The HUD's input focus and event registry and the IME box are the
//! game's, behind [`crate::ui::services::UiServices`] (divergences D470
//! and D471).

use std::any::Any;
use std::collections::BTreeSet;
use std::rc::Rc;

use neuron_core::math::{Matrix, Vector};

use crate::text::layout::TextRenderContext;
use crate::text::layout::device::DeviceTrcCx;
use crate::text::rdl::{PackedAttributes, RdlNode, RenderDisplayList, TextAttributes};
use crate::ui::control::{WinControl, base};
use crate::ui::controls::rich_text_win::RichTextWin;
use crate::ui::immediate::{capability, primitive};
use crate::ui::message::MouseButton;
use crate::ui::page::EntryPayload;
use crate::ui::services::{HudRecord, ImeBox};
use crate::ui::tree::{
    ACTIVE, ChildMessage, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget,
};

/// The default attributes (`G_RichEditDefaultAttr`: font 0, white).
pub const EDIT_DEFAULT_ATTRIBUTES: PackedAttributes = 0xFFFF_FF00;

/// The HUD events the box registers for: a key, a character, and
/// `USER_CANCEL` while read-only.
pub const EVENT_KEY: u32 = 1;
pub const EVENT_CHAR: u32 = 2;
pub const EVENT_CANCEL: u32 = 0xF;

/// One cell of the text (`FRichEditCell`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EditCell {
    pub ch: u16,
    pub attrs: PackedAttributes,
}

/// `iswcntrl` of the C runtime: the C0 and C1 control characters and DEL
/// (divergence D470).
#[must_use]
pub fn is_control(ch: u16) -> bool {
    ch < 0x20 || (0x7F..=0x9F).contains(&ch)
}

/// `towupper` in the `"C"` locale: ASCII letters only (divergence D297).
#[must_use]
pub fn to_upper(ch: u16) -> u16 {
    if (0x61..=0x7A).contains(&ch) {
        ch - 0x20
    } else {
        ch
    }
}

/// `towlower` in the `"C"` locale: ASCII letters only (divergence D297).
#[must_use]
pub fn to_lower(ch: u16) -> u16 {
    if (0x41..=0x5A).contains(&ch) {
        ch + 0x20
    } else {
        ch
    }
}

/// `RichTextEditWin`.
pub struct RichTextEditWin {
    pub win: RichTextWin,
    /// `+0x49c`.
    pub caret: i32,
    /// `+0x4a0`, `+0x4a4`: written with the caret; every edit needs them
    /// equal (the reference gives them no other use).
    pub field_4a0: i32,
    pub field_4a4: i32,
    /// `+0x4a8`: 4 from the constructor, read nowhere.
    pub field_4a8: i32,
    /// `+0x4ac`.
    pub caret_visible: bool,
    /// `+0x4b0`.
    pub caret_timer: f32,
    /// `+0x4b4`.
    pub caret_period: f32,
    /// `+0x4b8`: R, G, B, A.
    pub caret_color: [u8; 4],
    /// `+0x4bc`.
    pub caret_height_scale: f32,
    /// `+0x4c0`.
    pub editable: bool,
    /// `+0x4c4`.
    pub cells: Vec<EditCell>,
    /// `+0x4d4`: the attributes the next character is typed with.
    pub current_attrs: PackedAttributes,
    /// `+0x4dc`: the list shown before the text (the owner's).
    pub prefix: Rc<RenderDisplayList>,
    /// `+0x4e8`: the generated text list.
    pub text_display: Rc<RenderDisplayList>,
    /// `+0x4f4`: the formatting the keys may change (bit 0 bold, 1
    /// italic, 2 underline, 0xF8 the size).
    pub format_mask: i32,
    /// `+0x4f8`.
    pub fixed_style: bool,
    /// `+0x4fc`: the list that opens the text in the fixed style (the
    /// owner's).
    pub fixed_style_list: Rc<RenderDisplayList>,
    /// `+0x508`.
    pub password_mode: bool,
    /// `+0x50c`: 0 as typed, 1 upper case, 2 lower case.
    pub case_mode: i32,
    /// `+0x510`: 0 for no limit.
    pub max_length: i32,
    /// `+0x514`.
    pub filter: BTreeSet<u16>,
    /// `+0x528`.
    pub replacement: u16,
    /// `+0x52a`.
    pub filter_is_allow_list: bool,
    /// `+0x52b`.
    pub ime_enabled: bool,
    /// `+0x52c`.
    pub ime_box: Option<Box<dyn ImeBox>>,
}

impl std::fmt::Debug for RichTextEditWin {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RichTextEditWin")
            .field("caret", &self.caret)
            .field("cells", &self.cells)
            .field("ime_box", &self.ime_box.is_some())
            .finish_non_exhaustive()
    }
}

impl Default for RichTextEditWin {
    fn default() -> Self {
        RichTextEditWin::new()
    }
}

impl RichTextEditWin {
    /// The constructor. The IME box is made by `Create`, when the game's
    /// services are at hand (divergence D471).
    #[must_use]
    pub fn new() -> RichTextEditWin {
        RichTextEditWin {
            win: RichTextWin::new(),
            caret: 0,
            field_4a0: 0,
            field_4a4: 0,
            field_4a8: 4,
            caret_visible: true,
            caret_timer: 0.5,
            caret_period: 0.5,
            caret_color: [0xFF; 4],
            caret_height_scale: 1.0,
            editable: true,
            cells: Vec::new(),
            current_attrs: EDIT_DEFAULT_ATTRIBUTES,
            prefix: Rc::new(RenderDisplayList::new()),
            text_display: Rc::new(RenderDisplayList::new()),
            format_mask: -1,
            fixed_style: false,
            fixed_style_list: Rc::new(RenderDisplayList::new()),
            password_mode: false,
            case_mode: 0,
            max_length: 0,
            filter: BTreeSet::new(),
            replacement: u16::from(b'_'),
            filter_is_allow_list: false,
            ime_enabled: true,
            ime_box: None,
        }
    }

    fn count(&self) -> i32 {
        i32::try_from(self.cells.len()).unwrap_or(i32::MAX)
    }

    /// `ClampedCellIndex`: the position clamped to the cells.
    fn cell_index(&self, position: i32) -> usize {
        usize::try_from(position.max(0))
            .unwrap_or(0)
            .min(self.cells.len())
    }

    /// The case mode and the filter applied to a character (`OnChar`'s and
    /// `SetText`'s shared steps).
    fn filtered(&self, ch: u16, case: bool) -> u16 {
        let ch = if case {
            match self.case_mode {
                1 => to_upper(ch),
                2 => to_lower(ch),
                _ => ch,
            }
        } else {
            ch
        };
        let in_set = self.filter.contains(&ch);
        if self.filter_is_allow_list != in_set {
            self.replacement
        } else {
            ch
        }
    }

    /// `SetMaxLength`: a negative count is ignored.
    pub fn set_max_length(&mut self, max: i32) {
        if max >= 0 {
            self.max_length = max;
        }
    }

    /// `SetFixedStyle`: a change rebuilds the display.
    pub fn set_fixed_style(&mut self, fixed: bool) {
        let changed = fixed != self.fixed_style;
        self.fixed_style = fixed;
        if changed {
            self.rebuild_display();
        }
    }

    /// `SetPasswordMode`: a change rebuilds the display.
    pub fn set_password_mode(&mut self, password: bool) {
        let changed = password != self.password_mode;
        self.password_mode = password;
        if changed {
            self.rebuild_display();
        }
    }

    /// `SetImeEnabled`: on, a box is made when there is none and the game
    /// has an IME focus, and the text cleared (which makes it again); off,
    /// the box goes.
    pub fn set_ime_enabled(&mut self, ui: &mut Ui, id: NodeId, enabled: bool) {
        self.ime_enabled = enabled;
        if enabled {
            if self.ime_box.is_some() {
                return;
            }
            let Some(created) = ui.services.create_ime_box(id) else {
                return;
            };
            self.ime_box = Some(created);
            self.clear_text(ui, id);
            return;
        }
        self.ime_box = None;
    }

    /// `SetEditable`: while the box holds the input focus, read-only
    /// registers `USER_CANCEL` and editable drops it.
    pub fn set_editable(&mut self, ui: &mut Ui, id: NodeId, editable: bool) {
        if self.editable == editable {
            return;
        }
        self.editable = editable;
        if ui.services.input_focus() != Some(id) {
            return;
        }
        if self.editable {
            ui.services.unregister_event(id, EVENT_CANCEL);
        } else {
            ui.services.register_event(id, EVENT_CANCEL);
        }
    }

    /// `ClearText`: no cells, the caret at 0, the display rebuilt; an IME
    /// box is made again.
    pub fn clear_text(&mut self, ui: &mut Ui, id: NodeId) {
        self.cells.clear();
        self.field_4a4 = 0;
        self.field_4a0 = 0;
        self.caret = 0;
        self.rebuild_display();
        if self.ime_box.is_some() {
            self.ime_box = None;
            self.ime_box = ui.services.create_ime_box(id);
        }
    }

    /// `SetText`: one cell per character up to the first NUL or the
    /// maximum length, through the case mode and the filter, with
    /// `attrs` (`None`: the current attributes); the caret at the end.
    pub fn set_text(&mut self, text: &[u16], attrs: Option<PackedAttributes>) {
        self.cells.clear();
        let attrs = attrs.unwrap_or(self.current_attrs);
        for &unit in text {
            if unit == 0 {
                break;
            }
            if self.max_length != 0 && self.cells.len() as u32 >= self.max_length.cast_unsigned() {
                break;
            }
            let ch = match self.case_mode {
                0 => unit,
                1 => to_upper(unit),
                _ => to_lower(unit),
            };
            let ch = self.filtered(ch, false);
            self.cells.push(EditCell { ch, attrs });
        }
        let count = self.count();
        self.field_4a4 = count;
        self.field_4a0 = count;
        self.caret = count;
        self.rebuild_display();
    }

    /// `GetText`: at most `max_chars - 1` characters (then the reference
    /// writes a NUL) and the count written with the NUL, or the cell count
    /// and nothing when `max_chars` is not positive.
    #[must_use]
    pub fn get_text(&self, max_chars: i32) -> (Vec<u16>, i32) {
        let count = self.count();
        if max_chars <= 0 {
            return (Vec::new(), count);
        }
        let limit = count.min(max_chars - 1).max(0) as usize;
        let text: Vec<u16> = self.cells[..limit].iter().map(|c| c.ch).collect();
        (text, limit as i32 + 1)
    }

    /// The characters of the cells.
    #[must_use]
    pub fn text(&self) -> Vec<u16> {
        self.cells.iter().map(|c| c.ch).collect()
    }

    /// `Activate`: shown, the input focus, events 1 and 2 (and
    /// `USER_CANCEL` while read-only), the IME box focused.
    pub fn activate(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(n) = ui.node_mut(id) {
            n.flags |= ACTIVE | VISIBLE;
        }
        ui.services.push_input_focus(id);
        ui.services.register_event(id, EVENT_KEY);
        ui.services.register_event(id, EVENT_CHAR);
        if self.editable {
            ui.services.unregister_event(id, EVENT_CANCEL);
        } else {
            ui.services.register_event(id, EVENT_CANCEL);
        }
        if let Some(ime) = self.ime_box.as_mut() {
            ime.take_focus();
        }
        // Slot 51, `OnActivated`: empty.
    }

    /// `Deactivate`: the events dropped, the input focus given up, hidden
    /// on request, the IME focus cleared while the IME is enabled.
    pub fn deactivate(&mut self, ui: &mut Ui, id: NodeId, hide: bool) {
        // Slot 52, `OnDeactivating`: empty.
        ui.services.unregister_event(id, EVENT_KEY);
        ui.services.unregister_event(id, EVENT_CHAR);
        ui.services.unregister_event(id, EVENT_CANCEL);
        ui.services.pop_input_focus(id);
        if hide && let Some(n) = ui.node_mut(id) {
            n.flags &= !(ACTIVE | VISIBLE);
        }
        if self.ime_enabled {
            ui.services.clear_ime_focus();
        }
    }

    /// `SetCaseMode`.
    pub fn set_case_mode(&mut self, mode: i32) {
        self.case_mode = mode;
    }

    /// `SetFilterIsAllowList`.
    pub fn set_filter_is_allow_list(&mut self, allow: bool) {
        self.filter_is_allow_list = allow;
    }

    /// `AddFilterChars`.
    pub fn add_filter_chars(&mut self, chars: &[u16]) {
        for &ch in chars.iter().rev() {
            self.filter.insert(ch);
        }
    }

    /// `SetReplacementChar`.
    pub fn set_replacement_char(&mut self, ch: u16) {
        self.replacement = ch;
    }

    /// `AppendTextRun`: a text node of `count` cells from `start` (`*` each
    /// in password mode), cut at a NUL as the node's constructor copies.
    fn text_run(&self, list: &mut RenderDisplayList, start: usize, count: usize) {
        if count == 0 {
            return;
        }
        let run: Vec<u16> = if self.password_mode {
            vec![u16::from(b'*'); count]
        } else {
            self.cells[start..start + count]
                .iter()
                .map(|c| c.ch)
                .collect()
        };
        let text = crate::text::font::wide_c_str(&run).to_vec();
        list.push(RdlNode::Text(text));
    }

    /// `RebuildDisplay`: the source list emptied, then a reference to the
    /// prefix list and one to the text list; the text list refilled from
    /// the cells (the reference fills it after appending its reference,
    /// which points at the live list; here the list is complete before
    /// it is shared).
    pub fn rebuild_display(&mut self) {
        let mut text = RenderDisplayList::new();
        if let Some(first) = self.cells.first() {
            let mut run_attrs = first.attrs;
            if self.fixed_style {
                text.push(RdlNode::Ref(Rc::clone(&self.fixed_style_list)));
            } else {
                text.push(RdlNode::Attributes(TextAttributes::new(
                    run_attrs,
                    0xFFFF_FFFF,
                    0,
                )));
            }
            let mut start = 0usize;
            if !self.fixed_style {
                for index in 0..self.cells.len() {
                    if self.cells[index].attrs != run_attrs {
                        self.text_run(&mut text, start, index - start);
                        run_attrs = self.cells[index].attrs;
                        text.push(RdlNode::Attributes(TextAttributes::new(
                            run_attrs,
                            0xFFFF_FFFF,
                            0,
                        )));
                        start = index;
                    }
                }
            }
            if start < self.cells.len() {
                self.text_run(&mut text, start, self.cells.len() - start);
            }
        }
        self.text_display = Rc::new(text);
        let base = &mut self.win.base;
        if !self.win.content.nodes.is_empty() {
            self.win.content.nodes.clear();
            base.mark_dirty(true, true);
        }
        self.win.content.push(RdlNode::Ref(Rc::clone(&self.prefix)));
        self.win.base.mark_dirty(true, true);
        self.win
            .content
            .push(RdlNode::Ref(Rc::clone(&self.text_display)));
        self.win.base.mark_dirty(true, true);
    }

    /// `SetCaret`: clamped to the cells; the current attributes follow
    /// the cell before the caret (the first at 0).
    pub fn set_caret(&mut self, position: i32) {
        self.caret = position.clamp(0, self.count());
        let Some(&first) = self.cells.first() else {
            return;
        };
        let count = self.count();
        self.current_attrs = if self.caret == count {
            self.cells[(count - 1) as usize].attrs
        } else if self.caret > 0 {
            self.cells[(self.caret - 1) as usize].attrs
        } else {
            first.attrs
        };
    }

    /// The IME box's new text, when a key or character changed it
    /// (`UpdatedText`: the owner's `SetText` with the current attributes).
    fn ime_text(&mut self, text: Option<Vec<u16>>) {
        if let Some(text) = text {
            self.set_text(&text, None);
        }
    }

    /// A formatting key: allowed without a fixed style, with the mask's
    /// bits and Ctrl.
    fn format_allowed(&self, bits: i32, record: &HudRecord) -> bool {
        !self.fixed_style && self.format_mask & bits != 0 && record.modifiers & 0xC != 0
    }

    /// `OnKeyDown` (slot 49, event 1).
    pub fn on_key_down(&mut self, ui: &mut Ui, id: NodeId, record: &HudRecord) -> bool {
        if ui.node(id).is_none_or(|n| n.flags & ACTIVE == 0) {
            return false;
        }
        if !record.down {
            return false;
        }
        let mut result = false;
        if self.field_4a0 == self.field_4a4 {
            let key = record.key;
            result = true;
            match key {
                0x08 => {
                    if self.caret > 0 {
                        self.set_caret(self.caret - 1);
                        let at = self.cell_index(self.caret);
                        if at < self.cells.len() {
                            self.cells.remove(at);
                        }
                        self.rebuild_display();
                    }
                }
                0x09 | 0x2D => {
                    if self.ime_box.is_some() {
                        return false;
                    }
                    result = false;
                }
                0x0D | 0x1B => {
                    if let Some(ime) = self.ime_box.as_mut() {
                        let text = ime.handle_key(key);
                        self.ime_text(text);
                    }
                    if let Some(parent) = ui.parent(id) {
                        let message = if key == 0x0D {
                            ChildMessage::EditEnter { from: id }
                        } else {
                            ChildMessage::EditEscape { from: id }
                        };
                        ui.send_child_message(parent, &message);
                    }
                    return true;
                }
                0x23 | 0x24 | 0x25 | 0x27 => {
                    if self.ime_box.is_some() {
                        return false;
                    }
                    let to = match key {
                        0x23 => self.count(),
                        0x24 => 0,
                        0x25 => self.caret.wrapping_sub(1),
                        _ => self.caret.wrapping_add(1),
                    };
                    self.set_caret(to);
                }
                0x2E => {
                    if !self.cells.is_empty() {
                        let at = self.cell_index(self.caret);
                        if at != self.cells.len() {
                            self.cells.remove(at);
                            self.set_caret(self.caret);
                            self.rebuild_display();
                        }
                    }
                }
                0x42 | 0x49 | 0x55 => {
                    let bit = match key {
                        0x42 => 1,
                        0x49 => 2,
                        _ => 4,
                    };
                    if self.format_allowed(bit, record) {
                        self.current_attrs ^= bit as u32;
                    } else {
                        result = false;
                    }
                }
                0x6B => {
                    if self.format_allowed(0xF8, record) {
                        let low = self.current_attrs & 0xFF;
                        if low & 0xF8 < 0xF8 {
                            let new_low = (((low & 0xF8) + 8) & 0xFF) ^ (low & 7);
                            self.current_attrs = (self.current_attrs & !0xFF) | new_low;
                        }
                    } else {
                        result = false;
                    }
                }
                0x6D => {
                    if self.format_allowed(0xF8, record) {
                        let low = self.current_attrs & 0xFF;
                        if low & 0xF8 != 0 {
                            let new_low = (((low & 0xF8).wrapping_sub(1)) & 0xFF) ^ (!low & 7);
                            self.current_attrs = (self.current_attrs & !0xFF) | new_low;
                        }
                    } else {
                        result = false;
                    }
                }
                _ => result = false,
            }
        }
        if let Some(ime) = self.ime_box.as_mut() {
            let text = ime.handle_key(record.key);
            self.ime_text(text);
        }
        result
    }

    /// `OnChar` (slot 50, event 2): a control character is taken and does
    /// nothing; otherwise the case mode, the filter and an insert at the
    /// caret within the maximum length.
    pub fn on_char(&mut self, ui: &mut Ui, id: NodeId, record: &HudRecord) -> bool {
        if ui.node(id).is_none_or(|n| n.flags & ACTIVE == 0) {
            return false;
        }
        let unit = (record.wparam & 0xFFFF) as u16;
        if is_control(unit) {
            return true;
        }
        let ch = self.filtered(unit, true);
        if self.field_4a0 == self.field_4a4
            && !(self.max_length != 0 && self.cells.len() as u32 >= self.max_length.cast_unsigned())
        {
            let at = self.cell_index(self.caret);
            self.cells.insert(
                at,
                EditCell {
                    ch,
                    attrs: self.current_attrs,
                },
            );
            self.set_caret(self.caret.wrapping_add(1));
            self.rebuild_display();
        }
        if let Some(ime) = self.ime_box.as_mut() {
            let text = ime.handle_char(ch);
            self.ime_text(text);
        }
        true
    }

    /// `Event::Handler::OnEvent`: event 1 a key, 2 a character; a
    /// `USER_CANCEL` carrying a window message is posted again and taken.
    pub fn on_event(&mut self, ui: &mut Ui, id: NodeId, msg: u32, record: &HudRecord) -> bool {
        match msg {
            EVENT_KEY => self.on_key_down(ui, id, record),
            EVENT_CHAR => self.on_char(ui, id, record),
            EVENT_CANCEL if record.message != 0 => {
                ui.services.post_command(EVENT_CANCEL);
                true
            }
            _ => false,
        }
    }

    /// The caret blink of `Update`: the timer less the frame time; at or
    /// below zero the caret flips, and a negative timer gets the period
    /// added until it is not.
    pub fn blink(&mut self, frame_time: f64) {
        let mut remaining = f64::from(self.caret_timer) - frame_time;
        self.caret_timer = remaining as f32;
        if !(remaining <= 0.0) {
            return;
        }
        self.caret_visible = !self.caret_visible;
        if !(remaining < 0.0) {
            return;
        }
        let period = f64::from(self.caret_period);
        loop {
            remaining += period;
            if !(remaining < 0.0) {
                break;
            }
        }
        self.caret_timer = remaining as f32;
    }

    /// `Update`: the window's update, then the caret blink.
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.win.update_window(ui, id);
        self.blink(ui.frame_time);
    }

    /// The caret line of `Draw` in window pixels (x, y, length), when the
    /// caret's character lies inside the window.
    #[must_use]
    pub fn caret_line(&mut self, ui: &mut Ui) -> Option<(f32, f32, f32)> {
        let fonts = ui.fonts.as_mut()?;
        let base = &mut self.win.base;
        let mut ctx = DeviceTrcCx {
            trc: &mut base.trc,
            fonts: &fonts.rich,
            backend: fonts.backend.as_mut(),
            surface: base.surface.as_mut(),
        };
        ctx.reset_to_default();
        ctx.trc_mut().pos_v = 0;
        ctx.trc_mut().pos_h = 0;
        let index = (self.prefix.element_count() as i32).wrapping_add(self.caret);
        let (pos_h, pos_v, depth) = base.layout_list.element_pos(&mut ctx, &fonts.styles, index);
        let offset_v = match base.align_mode {
            0 => base.scroll_y,
            1 => 0,
            2 => -(base.pixel_height.wrapping_sub(base.layout_height) / 2),
            3 => base.layout_height.wrapping_sub(base.pixel_height),
            // The reference reads an unwritten local (divergence D472).
            _ => 0,
        };
        let pos_h = (f64::from(pos_h) - f64::from(base.scroll_x)) as f32;
        let mut depth = (f64::from(depth) * f64::from(self.caret_height_scale) - 4.0) as f32;
        let mut pos_v = (f64::from(pos_v) - f64::from(offset_v) + 2.0) as f32;
        let height = base.pixel_height as f32;
        if !(pos_h >= 0.0
            && f64::from(base.pixel_width) > f64::from(pos_h)
            && pos_v < height
            && -depth < pos_v)
        {
            return None;
        }
        if pos_v < 0.0 {
            depth = (f64::from(pos_v) + f64::from(depth)) as f32;
            pos_v = 0.0;
        } else if !(f64::from(pos_v) + f64::from(depth) < f64::from(height)) {
            depth = (f64::from(height) - f64::from(pos_v)) as f32;
        }
        let x = (f64::from(base.screen_rect[0]) + f64::from(pos_h)) as f32;
        let y = (f64::from(base.screen_rect[1]) + f64::from(pos_v)) as f32;
        Some((x, y, depth))
    }

    /// `Draw`: the window, the IME box resized to the window, then the
    /// caret while the box is shown, holds the input focus and is in the
    /// visible half of its blink.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.win.draw(ui, id, host);
        if let Some(ime) = self.ime_box.as_mut() {
            let size = if self.max_length > 0 {
                self.max_length.cast_unsigned()
            } else {
                0x50
            };
            ime.resize(size, self.win.base.screen_rect);
        }
        if ui.node(id).is_none_or(|n| n.flags & VISIBLE == 0) {
            return;
        }
        if ui.services.input_focus() != Some(id) || !self.caret_visible {
            return;
        }
        let line = self.caret_line(ui);
        let Some(imm) = host.immediate() else { return };
        imm.set_depth_write(false);
        imm.disable(capability::DEPTH_TEST);
        imm.disable(capability::CULL_FACE);
        imm.disable(capability::BLEND);
        imm.push_projection();
        imm.set_ortho_2d();
        if let Some((x, y, length)) = line {
            imm.color4ubv(self.caret_color);
            imm.disable(capability::TEXTURE_2D);
            imm.begin(primitive::LINES);
            imm.vertex(x, y, 0.0);
            imm.vertex(x, (f64::from(y) + f64::from(length)) as f32, 0.0);
            imm.end();
        }
        imm.pop_projection();
        imm.enable(capability::DEPTH_TEST);
        imm.set_depth_write(true);
    }

    /// `CreateSized`: `Create` without a payload, then the size.
    pub fn create_sized(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        width: f32,
        height: f32,
    ) -> bool {
        let created = self.create_window(ui, id, parent);
        self.win.base.norm_width = width;
        self.win.base.norm_height = height;
        created
    }

    /// Slot 9 (`RichTextWin::Create`), and the IME box the constructor
    /// makes while the game has an IME focus (divergence D471).
    pub fn create_window(&mut self, ui: &mut Ui, id: NodeId, parent: Option<NodeId>) -> bool {
        let created = self.win.create(ui, id, parent, &mut Payload::None);
        if self.ime_box.is_none() {
            self.ime_box = ui.services.create_ime_box(id);
        }
        created
    }
}

impl Widget for RichTextEditWin {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.win.base.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.win.base.control)
    }
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        self.create_window(ui, id, parent)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        self.win.hit(ui, id, ray)
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.win.set_position(ui, id, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        self.win.set_orientation(ui, id, orientation);
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.win.destroy(ui, id);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.win.on_delete(ui, id);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        RichTextEditWin::update(self, ui, id);
    }
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        self.win.update_inactive_window(ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        RichTextEditWin::draw(self, ui, id, host);
    }
    /// Slot 19: to the parent's slot 19, else `WinControl`'s.
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if ui.parent(id).is_some() {
            return ui.base_mouse_event(id, event);
        }
        base::mouse_event(ui, id, self, event)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        self.win.property(ui, id, property)
    }
}

/// `FLImeInputBox::UpdatedText`'s `Owner->SetText(Text, 0)` for a box
/// whose text changed outside its window's own call (the IME's result
/// characters, a focus move): the window's text set with its current
/// attributes. False when the node is not an edit window.
pub fn set_ime_owner_text(ui: &mut Ui, id: NodeId, text: &[u16]) -> bool {
    if let Some(w) = ui.widget_mut::<RichTextEditWin>(id) {
        w.set_text(text, None);
        return true;
    }
    if let Some(w) = ui.widget_mut::<UiTextEdit>(id) {
        w.edit.set_text(text, None);
        return true;
    }
    false
}

/// `UITextEdit`: a read-only edit box sized and filled by its payload.
#[derive(Debug, Default)]
pub struct UiTextEdit {
    pub edit: RichTextEditWin,
    /// `+0x530`, `+0x534`: the payload's size (also the window's).
    pub local_width: f32,
    pub local_height: f32,
}

impl Widget for UiTextEdit {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.edit.win.base.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.edit.win.base.control)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        self.edit.hit_test(ui, id, ray)
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.edit.set_position(ui, id, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        self.edit.set_orientation(ui, id, orientation);
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.edit.destroy(ui, id);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.edit.on_delete(ui, id);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.edit.update(ui, id);
    }
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        Widget::update_inactive(&mut self.edit, ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.edit.draw(ui, id, host);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if ui.parent(id).is_some() {
            return ui.base_mouse_event(id, event);
        }
        base::mouse_event(ui, id, self, event)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        self.edit.property(ui, id, property)
    }

    /// `UITextEdit::Create`: `RichTextWin::Create`, the text "TextEdit",
    /// the payload's size and caption, read-only.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        let created = self.edit.create_window(ui, id, parent);
        self.edit.clear_text(ui, id);
        let placeholder: Vec<u16> = "TextEdit".encode_utf16().collect();
        self.edit.set_text(&placeholder, None);
        if let Payload::Page(EntryPayload::Text {
            right,
            bottom,
            caption,
        }) = payload
        {
            self.local_width = *right;
            self.edit.win.base.norm_width = *right;
            self.local_height = *bottom;
            self.edit.win.base.norm_height = *bottom;
            let caption = caption.clone();
            self.edit.clear_text(ui, id);
            self.edit.set_text(&caption, None);
        }
        self.edit.set_editable(ui, id, false);
        created
    }
}

#[cfg(test)]
mod tests;
