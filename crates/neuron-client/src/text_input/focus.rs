//! `IMEFocus`: the registered edit boxes in registration order, the one
//! with the IME focus, and the routing of characters and keys to it.
//!
//! The C++ keeps pointers to boxes its windows own; here the focus manager
//! owns the boxes and hands out [`BoxId`]s. Every call that can change a
//! box takes the [`ImeContext`] and applies what the box asked of the IME
//! (the caret to follow, a composition to finalise) when it returns.
//!
//! The C++ client builds its `IMEFocus` only when some installed keyboard
//! layout is an IME (`IsImeAvailable`); `platform::ime::ime_available`
//! answers that.

use crate::text_input::edit_box::EditBox;
use crate::text_input::ime::ImeContext;

/// A registered box.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct BoxId {
    index: u32,
    generation: u32,
}

#[derive(Debug, Default)]
struct Slot {
    generation: u32,
    edit_box: Option<EditBox>,
}

/// The focus manager.
#[derive(Debug, Default)]
pub struct ImeFocus {
    slots: Vec<Slot>,
    order: Vec<BoxId>,
    active: Option<BoxId>,
}

impl ImeFocus {
    /// The manager with no boxes; the window's IME is closed until a box
    /// takes the focus (`ImeUi_Initialize` ends with `ImeUi_SetState(false)`).
    pub fn new(ime: &mut dyn ImeContext) -> ImeFocus {
        ime.set_open(false);
        ime.set_boxes_registered(false);
        ImeFocus::default()
    }

    /// `NumInputBoxes`.
    #[must_use]
    pub fn len(&self) -> usize {
        self.order.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.order.is_empty()
    }

    /// `GetFocus`: the box with the IME focus.
    #[must_use]
    pub fn focus(&self) -> Option<BoxId> {
        self.active
    }

    /// `GetInputBox`: the box registered `index`th.
    #[must_use]
    pub fn box_at(&self, index: usize) -> Option<BoxId> {
        self.order.get(index).copied()
    }

    #[must_use]
    pub fn get(&self, id: BoxId) -> Option<&EditBox> {
        self.slots
            .get(id.index as usize)
            .filter(|s| s.generation == id.generation)
            .and_then(|s| s.edit_box.as_ref())
    }

    fn get_mut(&mut self, id: BoxId) -> Option<&mut EditBox> {
        self.slots
            .get_mut(id.index as usize)
            .filter(|s| s.generation == id.generation)
            .and_then(|s| s.edit_box.as_mut())
    }

    /// The box's text when it was handed on since the last call
    /// (`UpdatedText`, which the client's box writes into its window).
    pub fn take_updated_text(&mut self, id: BoxId) -> Option<Vec<u16>> {
        let b = self.get_mut(id)?;
        b.take_updated().then(|| b.text().to_vec())
    }

    /// Hands a box's pending requests to the IME.
    fn apply(&mut self, id: BoxId, ime: &mut dyn ImeContext) {
        let Some(b) = self.get_mut(id) else {
            return;
        };
        let pending = std::mem::take(&mut b.pending);
        if pending.finalize {
            ime.finalize(false);
        }
        if let Some(caret) = pending.caret {
            ime.set_caret(caret);
        }
    }

    /// Runs `f` on a box and applies what it asked of the IME.
    pub fn edit<R>(
        &mut self,
        id: BoxId,
        ime: &mut dyn ImeContext,
        f: impl FnOnce(&mut EditBox) -> R,
    ) -> Option<R> {
        let result = f(self.get_mut(id)?);
        self.apply(id, ime);
        Some(result)
    }

    /// `RegisterInputBox` (the `IMEInputBox` constructor registers itself):
    /// the first box while none has the focus takes it.
    pub fn register(&mut self, edit_box: EditBox, ime: &mut dyn ImeContext) -> BoxId {
        let index = self
            .slots
            .iter()
            .position(|s| s.edit_box.is_none())
            .unwrap_or_else(|| {
                self.slots.push(Slot::default());
                self.slots.len() - 1
            });
        let slot = &mut self.slots[index];
        slot.generation = slot.generation.wrapping_add(1);
        slot.edit_box = Some(edit_box);
        let id = BoxId {
            index: index as u32,
            generation: slot.generation,
        };
        if self.active.is_none() {
            self.set_focus(Some(id), false, ime);
        }
        self.order.push(id);
        ime.set_boxes_registered(true);
        id
    }

    /// `RemoveInputBox`: a box with the focus loses it first (its
    /// composition is finalised into it).
    pub fn remove(&mut self, id: BoxId, ime: &mut dyn ImeContext) -> Option<EditBox> {
        let position = self.order.iter().position(|&o| o == id)?;
        if self.active == Some(id) {
            self.kill_focus(id, ime);
            self.active = None;
        }
        self.order.remove(position);
        if self.order.is_empty() {
            ime.set_boxes_registered(false);
        }
        self.slots[id.index as usize].edit_box.take()
    }

    /// `RemoveInputBox(int)`.
    pub fn remove_at(&mut self, index: usize, ime: &mut dyn ImeContext) -> Option<EditBox> {
        let id = self.box_at(index)?;
        self.remove(id, ime)
    }

    /// `SetFocus`: the box losing the focus gets the composition in
    /// progress; the IME closes, then opens for the new box.
    pub fn set_focus(&mut self, id: Option<BoxId>, force: bool, ime: &mut dyn ImeContext) {
        let id = id.filter(|&i| self.get(i).is_some());
        if !force && self.active == id {
            return;
        }
        if let Some(old) = self.active {
            self.kill_focus(old, ime);
        }
        self.active = id;
        if let Some(new) = id {
            self.set_box_focus(new, ime);
        }
    }

    /// `IMEInputBox::OnSetFocus`.
    fn set_box_focus(&mut self, id: BoxId, ime: &mut dyn ImeContext) {
        ime.set_open(true);
        if let Some(b) = self.get_mut(id) {
            b.on_set_focus();
        }
        self.apply(id, ime);
    }

    /// `IMEInputBox::OnKillFocus`: the composition's text goes to the box
    /// that still has the focus (`GSendChar` reaches `OnChar`).
    fn kill_focus(&mut self, id: BoxId, ime: &mut dyn ImeContext) {
        if let Some(b) = self.get_mut(id) {
            b.on_kill_focus();
        }
        for unit in ime.finalize(true) {
            self.on_char(unit, ime);
        }
        ime.set_open(false);
        self.apply(id, ime);
    }

    /// The window gained the keyboard focus (`IMEFocus::OnSetFocus`).
    pub fn on_window_focus_gained(&mut self, ime: &mut dyn ImeContext) {
        if let Some(id) = self.active {
            self.set_box_focus(id, ime);
        }
    }

    /// The window lost the keyboard focus (`IMEFocus::OnKillFocus`).
    pub fn on_window_focus_lost(&mut self, ime: &mut dyn ImeContext) {
        if let Some(id) = self.active {
            self.kill_focus(id, ime);
        }
    }

    /// `OnChar`: a UTF-16 unit for the focused box (the IME's result
    /// string arrives here).
    pub fn on_char(&mut self, unit: u16, ime: &mut dyn ImeContext) -> bool {
        let Some(id) = self.active else {
            return false;
        };
        self.edit(id, ime, |b| b.handle_wide_char(unit))
            .unwrap_or(false)
    }

    /// `OnKeyDown`: a key for the focused box.
    pub fn on_key_down(&mut self, key: u32, ime: &mut dyn ImeContext) -> bool {
        let Some(id) = self.active else {
            return false;
        };
        self.edit(id, ime, |b| b.handle_key(key)).unwrap_or(false)
    }

    /// The destructor: the focused box loses the focus.
    pub fn shutdown(&mut self, ime: &mut dyn ImeContext) {
        self.set_focus(None, false, ime);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::input::keyboard::vk;
    use crate::text_input::edit_box::tests::{Fixed, utf16};
    use crate::text_input::ime::CaretRect;

    #[derive(Debug, Default)]
    struct Recorder {
        calls: Vec<String>,
        composition: Vec<u16>,
    }

    impl ImeContext for Recorder {
        fn set_open(&mut self, open: bool) {
            self.calls.push(format!("open {open}"));
        }
        fn finalize(&mut self, send: bool) -> Vec<u16> {
            self.calls.push(format!("finalize {send}"));
            let text = std::mem::take(&mut self.composition);
            if send { text } else { Vec::new() }
        }
        fn set_caret(&mut self, c: CaretRect) {
            self.calls
                .push(format!("caret {} {} {}", c.x, c.y, c.height));
        }
        fn set_boxes_registered(&mut self, registered: bool) {
            self.calls.push(format!("boxes {registered}"));
        }
    }

    fn new_box() -> EditBox {
        EditBox::new(Box::new(Fixed))
    }

    fn text(f: &ImeFocus, id: BoxId) -> String {
        String::from_utf16_lossy(f.get(id).unwrap().text())
    }

    #[test]
    fn the_first_box_takes_the_focus_and_opens_the_ime() {
        let mut ime = Recorder::default();
        let mut f = ImeFocus::new(&mut ime);
        assert_eq!(ime.calls, ["open false", "boxes false"]);
        ime.calls.clear();
        let a = f.register(new_box(), &mut ime);
        let b = f.register(new_box(), &mut ime);
        assert_eq!(f.focus(), Some(a));
        assert_eq!(f.len(), 2);
        assert_eq!(f.box_at(1), Some(b));
        assert_eq!(
            ime.calls,
            ["open true", "caret 1 1 16", "boxes true", "boxes true"]
        );
        assert!(f.get(a).unwrap().has_focus());
        assert!(!f.get(b).unwrap().has_focus());
    }

    #[test]
    fn characters_and_keys_go_to_the_focused_box() {
        let mut ime = Recorder::default();
        let mut f = ImeFocus::new(&mut ime);
        assert!(!f.on_char(0x41, &mut ime), "no box");
        let a = f.register(new_box(), &mut ime);
        ime.calls.clear();
        for u in utf16("ab") {
            assert!(f.on_char(u, &mut ime));
        }
        assert!(!f.on_char(0x0d, &mut ime));
        assert_eq!(text(&f, a), "ab");
        assert_eq!(ime.calls, ["caret 9 1 16", "caret 17 1 16"]);
        assert!(f.on_key_down(vk::LEFT, &mut ime));
        assert_eq!(f.get(a).unwrap().caret_index(), 1);
        // A full buffer finalises the composition (the caret keeps the
        // offset of the last non-empty text).
        f.edit(a, &mut ime, |b| b.set_input_buffer_size(0));
        ime.calls.clear();
        f.on_char(0x43, &mut ime);
        assert_eq!(ime.calls, ["finalize false", "caret 9 1 16"]);
    }

    #[test]
    fn moving_the_focus_hands_the_composition_to_the_old_box() {
        let mut ime = Recorder::default();
        let mut f = ImeFocus::new(&mut ime);
        let a = f.register(new_box(), &mut ime);
        let b = f.register(new_box(), &mut ime);
        ime.composition = utf16("xy");
        ime.calls.clear();
        f.set_focus(Some(b), false, &mut ime);
        assert_eq!(text(&f, a), "xy");
        assert_eq!(text(&f, b), "");
        assert_eq!(f.focus(), Some(b));
        assert_eq!(
            ime.calls,
            ["finalize true", "open false", "open true", "caret 1 1 16"]
        );
        // The same box again does nothing unless forced.
        ime.calls.clear();
        f.set_focus(Some(b), false, &mut ime);
        assert!(ime.calls.is_empty());
        f.set_focus(Some(b), true, &mut ime);
        assert_eq!(ime.calls.len(), 4);
    }

    #[test]
    fn removing_the_focused_box_finalises_into_it() {
        let mut ime = Recorder::default();
        let mut f = ImeFocus::new(&mut ime);
        let a = f.register(new_box(), &mut ime);
        let b = f.register(new_box(), &mut ime);
        ime.composition = utf16("z");
        let removed = f.remove(a, &mut ime).unwrap();
        assert_eq!(removed.text(), utf16("z"));
        assert_eq!(f.focus(), None, "nobody takes the focus over");
        assert!(f.get(a).is_none());
        ime.calls.clear();
        f.remove_at(0, &mut ime).unwrap();
        assert_eq!(ime.calls, ["boxes false"]);
        assert!(f.is_empty() && f.get(b).is_none());
        // A slot is reused under a new id.
        let c = f.register(new_box(), &mut ime);
        assert_ne!(c, a);
        assert!(f.get(a).is_none() && f.get(c).is_some());
        assert!(f.remove(a, &mut ime).is_none());
    }

    #[test]
    fn window_focus_changes_reach_the_focused_box() {
        let mut ime = Recorder::default();
        let mut f = ImeFocus::new(&mut ime);
        f.on_window_focus_lost(&mut ime);
        let a = f.register(new_box(), &mut ime);
        ime.calls.clear();
        f.on_window_focus_lost(&mut ime);
        assert!(!f.get(a).unwrap().has_focus());
        assert_eq!(f.focus(), Some(a), "the box keeps the IME focus");
        f.on_window_focus_gained(&mut ime);
        assert!(f.get(a).unwrap().has_focus());
        assert_eq!(
            ime.calls,
            ["finalize true", "open false", "open true", "caret 1 1 16"]
        );
        f.shutdown(&mut ime);
        assert_eq!(f.focus(), None);
    }
}
