//! The edit box of `imeui.dll`: `CInputBox` with `IMEInputBox`'s overrides,
//! and its `CCaret`.
//!
//! The box keeps the C++'s buffer: a fixed number of UTF-16 units plus a
//! terminator, read up to the first zero. Its editing rules are the C++'s,
//! including the unusual ones: deleting (Delete, Backspace, overwrite mode,
//! a full buffer) writes a terminator at the caret and so cuts the text
//! there, and units after that terminator come back when a character is
//! typed over it. "Free space" is counted from the caret, not from the end
//! of the text. Where the C++ reads or writes past its buffer the box stops
//! at the buffer (divergence D282, proposed).
//!
//! The C++ client gives such a box to every rich-text edit window while an
//! IME is installed, forwards it every key-down and character, and writes
//! the box's text back into the window (`FLImeInputBox::UpdatedText`); the
//! box's caret is where the IME windows go.

use crate::text_input::ime::CaretRect;
use crate::window::Rect;

/// Units in a new box's buffer (`SetInputBufferSize(32)`).
pub const DEFAULT_BUFFER_SIZE: u32 = 32;
/// The title's capacity as the C++ copies it (`lstrcpynW` with 0x3f: up to
/// 62 units).
pub const TITLE_CAPACITY: usize = 0x3e;

/// Measures text in the box's font (`ImeUi_GetTextExtent`, the GDI extent
/// of the box's `LOGFONT` in the C++): the UI toolkit implements it with
/// the font it draws the box with.
pub trait TextMeasure {
    /// Width and height of `text` in surface pixels, or `None` when the
    /// font cannot measure. An empty text has width 0 and the line height.
    fn extent(&self, text: &[u16]) -> Option<(i32, i32)>;

    /// The line height the candidate list opens below.
    fn line_height(&self) -> i32 {
        self.extent(&[]).map_or(0, |(_, h)| h)
    }
}

/// `CCaret`: visibility, blink state and position (surface pixels).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Caret {
    shown: bool,
    blink_stamp: u32,
    state: bool,
    x: i32,
    y: i32,
    blink: bool,
}

impl Default for Caret {
    /// The C++ constructor leaves the blink flag uninitialised; it starts
    /// set here (the box starts in insert mode, which blinks).
    fn default() -> Self {
        Caret {
            shown: false,
            blink_stamp: 0,
            state: false,
            x: 0,
            y: 0,
            blink: true,
        }
    }
}

impl Caret {
    /// `Show`.
    pub fn show(&mut self, show: bool) {
        self.shown = show;
    }

    #[must_use]
    pub fn is_shown(&self) -> bool {
        self.shown
    }

    /// `SetPos` (the C++ also hands the point to the IME; the box queues
    /// that for its focus manager).
    pub fn set_pos(&mut self, x: i32, y: i32) {
        self.x = x;
        self.y = y;
    }

    #[must_use]
    pub fn position(&self) -> (i32, i32) {
        (self.x, self.y)
    }

    /// `UpdateCaret`: whether to draw the caret now. Hidden while a
    /// composition is in progress or the caret is not shown; a blinking
    /// caret flips once the blink time (`GetCaretBlinkTime`) has passed
    /// since the last flip at `now` milliseconds (`GetTickCount`).
    pub fn update(&mut self, now: u32, blink_time: u32, composition_empty: bool) -> bool {
        if !composition_empty || !self.shown {
            return false;
        }
        if self.blink {
            if now < self.blink_stamp {
                self.blink_stamp = 0;
            }
            if blink_time < now - self.blink_stamp {
                self.blink_stamp = now;
                self.state = !self.state;
            }
        }
        self.state
    }

    /// `SetBlink`: the caret shows at once.
    pub fn set_blink(&mut self, blink: bool) {
        self.state = true;
        self.blink = blink;
    }

    #[must_use]
    pub fn blinks(&self) -> bool {
        self.blink
    }
}

/// Whether a UTF-16 unit is a control character (`CT_CTYPE1` `C1_CNTRL`
/// of `GetStringTypeExW`: the Unicode `Cc` class, U+0000 to U+001F and
/// U+007F to U+009F).
#[must_use]
pub fn is_control(unit: u16) -> bool {
    unit < 0x20 || (0x7f..=0x9f).contains(&unit)
}

/// What the box asks of the IME after an edit; the focus manager applies
/// it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Pending {
    pub caret: Option<CaretRect>,
    pub finalize: bool,
}

/// `IMEInputBox` over `CInputBox`.
pub struct EditBox {
    caret: Caret,
    rect: Rect,
    caret_x: i32,
    caret_y: i32,
    title: Vec<u16>,
    margin: (u32, u32),
    has_focus: bool,
    /// The buffer's units and one more for the terminator; the last unit
    /// stays zero.
    buffer: Vec<u16>,
    caret_index: u32,
    insert_mode: bool,
    measure: Box<dyn TextMeasure>,
    updated: bool,
    pub(crate) pending: Pending,
}

impl std::fmt::Debug for EditBox {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("EditBox")
            .field("text", &String::from_utf16_lossy(self.text()))
            .field("caret_index", &self.caret_index)
            .field("buffer_size", &self.buffer_size())
            .field("insert_mode", &self.insert_mode)
            .field("has_focus", &self.has_focus)
            .finish_non_exhaustive()
    }
}

impl EditBox {
    /// A box measuring with `measure` (the C++ box's font): 32 units,
    /// margins of one pixel, insert mode, no focus.
    #[must_use]
    pub fn new(measure: Box<dyn TextMeasure>) -> EditBox {
        EditBox {
            caret: Caret::default(),
            rect: Rect::default(),
            caret_x: 0,
            caret_y: 0,
            title: Vec::new(),
            margin: (1, 1),
            has_focus: false,
            buffer: vec![0; DEFAULT_BUFFER_SIZE as usize + 1],
            caret_index: 0,
            insert_mode: true,
            measure,
            updated: false,
            pending: Pending::default(),
        }
    }

    /// The text: the buffer up to its first zero.
    #[must_use]
    pub fn text(&self) -> &[u16] {
        &self.buffer[..self.text_len_from(0)]
    }

    /// The whole buffer, terminator included, with any units after the
    /// first zero.
    #[must_use]
    pub fn buffer(&self) -> &[u16] {
        &self.buffer
    }

    /// The buffer's size in units (`m_dwBufferSize`).
    #[must_use]
    pub fn buffer_size(&self) -> u32 {
        (self.buffer.len() - 1) as u32
    }

    /// The caret as an index into the buffer (`m_dwCaretPos`).
    #[must_use]
    pub fn caret_index(&self) -> u32 {
        self.caret_index
    }

    /// The caret's offset within the box (`m_nCaretPosX`, `m_nCaretPosY`):
    /// the width of the text before it, kept from the last time the text
    /// was not empty.
    #[must_use]
    pub fn caret_offset(&self) -> (i32, i32) {
        (self.caret_x, self.caret_y)
    }

    #[must_use]
    pub fn caret(&self) -> &Caret {
        &self.caret
    }

    pub fn caret_mut(&mut self) -> &mut Caret {
        &mut self.caret
    }

    #[must_use]
    pub fn insert_mode(&self) -> bool {
        self.insert_mode
    }

    #[must_use]
    pub fn has_focus(&self) -> bool {
        self.has_focus
    }

    /// The bounding rectangle (`GetRect`).
    #[must_use]
    pub fn rect(&self) -> Rect {
        self.rect
    }

    /// `GetMarginX`, `GetMarginY`.
    #[must_use]
    pub fn margins(&self) -> (u32, u32) {
        self.margin
    }

    /// `SetMargins`.
    pub fn set_margins(&mut self, x: u32, y: u32) {
        self.margin = (x, y);
    }

    #[must_use]
    pub fn title(&self) -> &[u16] {
        &self.title
    }

    /// `SetTitle`: up to the first zero, at most 62 units.
    pub fn set_title(&mut self, title: &[u16]) {
        let end = title.iter().position(|&u| u == 0).unwrap_or(title.len());
        self.title = title[..end.min(TITLE_CAPACITY)].to_vec();
    }

    /// Whether the text was handed on since the last call (`UpdatedText`,
    /// which the C++ client's box uses to write the text into its window).
    pub fn take_updated(&mut self) -> bool {
        std::mem::take(&mut self.updated)
    }

    /// `wcslen` from `at`, within the buffer.
    fn text_len_from(&self, at: usize) -> usize {
        self.buffer.get(at..).map_or(0, |rest| {
            rest.iter().position(|&u| u == 0).unwrap_or(rest.len())
        })
    }

    /// `OnSetFocus` (the focus manager opens the IME first).
    pub(crate) fn on_set_focus(&mut self) {
        self.has_focus = true;
        self.reset_caret();
        self.caret.show(true);
    }

    /// `OnKillFocus` (the focus manager then finalises the composition and
    /// closes the IME).
    pub(crate) fn on_kill_focus(&mut self) {
        self.has_focus = false;
        self.caret.show(false);
    }

    /// `Resize`: the bounding rectangle in surface pixels. The C++
    /// client's box is given its window's size at 0, 0; the IME windows
    /// land at the caret only if the toolkit gives the box's place on the
    /// surface.
    pub fn resize(&mut self, rect: Rect) {
        self.rect = rect;
        self.recalc_caret_pos();
    }

    /// `SetFont`: a new measure, and the caret placed again.
    pub fn set_measure(&mut self, measure: Box<dyn TextMeasure>) {
        self.measure = measure;
        self.reset_caret();
    }

    /// `HandleKey`: Backspace, Delete, Left, Right, Home, End and Insert.
    /// Returns whether the box changed (Insert toggles the mode and
    /// returns false).
    pub fn handle_key(&mut self, key: u32) -> bool {
        use crate::input::keyboard::vk;
        match key {
            vk::BACK => self.move_caret(false, false) && self.delete_char(),
            vk::DELETE => self.delete_char(),
            vk::LEFT => self.move_caret(false, false),
            vk::RIGHT => self.move_caret(true, false),
            vk::HOME => self.move_caret(false, true),
            vk::END => self.move_caret(true, true),
            vk::INSERT => {
                self.toggle_insert_mode();
                false
            }
            _ => false,
        }
    }

    /// `CharFilter`: control characters are refused.
    #[must_use]
    pub fn char_filter(&self, unit: u16) -> bool {
        !is_control(unit)
    }

    /// `HandleWideChar`: a typed UTF-16 unit, inserted at the caret when
    /// the filter accepts it.
    pub fn handle_wide_char(&mut self, unit: u16) -> bool {
        let accepted = self.char_filter(unit);
        if accepted {
            self.add_char(unit);
        }
        accepted
    }

    /// `ToggleInsertMode` with `IMEInputBox`'s caret: insert blinks,
    /// overwrite does not.
    pub fn toggle_insert_mode(&mut self) {
        self.insert_mode = !self.insert_mode;
        self.caret.set_blink(self.insert_mode);
    }

    /// `MoveCaret`: one unit right (not past the text) or left, or to the
    /// end or the start. Returns whether the caret moved.
    pub fn move_caret(&mut self, right: bool, head_tail: bool) -> bool {
        let previous = self.caret_index;
        if right {
            if head_tail {
                self.caret_index = self.text_len_from(0) as u32;
            } else if self.caret_index < self.buffer_size()
                && self.buffer[self.caret_index as usize] != 0
            {
                self.caret_index += 1;
            }
        } else if head_tail {
            self.caret_index = 0;
        } else {
            self.caret_index = self.caret_index.saturating_sub(1);
        }
        let changed = self.caret_index != previous;
        if changed {
            self.recalc_caret_pos();
        }
        changed
    }

    /// `SetInputBufferSize`: the text is kept up to the new size. The
    /// caret is kept within the new size (the C++ keeps it within the old
    /// one).
    pub fn set_input_buffer_size(&mut self, size: u32) {
        let old = self.buffer_size();
        if size == old {
            return;
        }
        let mut buffer = vec![0u16; size as usize + 1];
        let count = size.min(old) as usize;
        let copied = self.text_len_from(0).min(count);
        buffer[..copied].copy_from_slice(&self.buffer[..copied]);
        self.caret_index = self.caret_index.min(old).min(size);
        self.buffer = buffer;
    }

    /// `GetFreeInputBufferSize`: units from the caret to the buffer's end.
    #[must_use]
    pub fn free_buffer_size(&self) -> u32 {
        self.buffer_size() - self.caret_index
    }

    /// `AddChar`. In overwrite mode, and when the caret is at the buffer's
    /// end, the text is first cut at the caret (`DeleteChar`); a buffer
    /// still full asks the IME to finalise its composition and drops the
    /// unit. The unit is inserted at the caret; a unit pushed past the
    /// buffer's end is dropped.
    fn add_char(&mut self, unit: u16) {
        if !self.insert_mode {
            self.delete_char();
        }
        if self.free_buffer_size() == 0 {
            self.delete_char();
            if self.free_buffer_size() == 0 {
                self.pending.finalize = true;
                return;
            }
        }
        let at = self.caret_index as usize;
        let size = self.buffer_size() as usize;
        let len = self.text_len_from(at).min(size - at - 1);
        if len != 0 {
            self.buffer.copy_within(at..at + len, at + 1);
        }
        self.buffer[at] = unit;
        self.caret_index += 1;
        self.recalc_caret_pos();
        self.updated = true;
    }

    /// `DeleteChar`: a terminator at the caret (at the last unit when the
    /// caret is at the end).
    fn delete_char(&mut self) -> bool {
        let size = self.buffer_size();
        if self.caret_index >= size {
            self.caret_index = size.saturating_sub(1);
        }
        self.buffer[self.caret_index as usize] = 0;
        self.recalc_caret_pos();
        self.updated = true;
        true
    }

    /// `ResetCaret`.
    fn reset_caret(&mut self) {
        self.recalc_caret_pos();
    }

    /// `IMEInputBox::CalcCaretPos`: the width of the text before the
    /// caret; nothing for an empty text.
    fn calc_caret_pos(&self) -> Option<i32> {
        if self.buffer[0] == 0 {
            return None;
        }
        let end = (self.caret_index as usize).min(self.buffer.len());
        self.measure.extent(&self.buffer[..end]).map(|(w, _)| w)
    }

    /// `RecalcCaretPos`: with the focus, the caret goes to the box's corner
    /// plus the offset and the margins, and the IME follows it.
    fn recalc_caret_pos(&mut self) {
        if let Some(x) = self.calc_caret_pos() {
            self.caret_x = x;
        }
        if self.has_focus {
            let x = self
                .rect
                .left
                .wrapping_add(self.caret_x)
                .wrapping_add(self.margin.0 as i32);
            let y = self
                .rect
                .top
                .wrapping_add(self.caret_y)
                .wrapping_add(self.margin.1 as i32);
            self.caret.set_pos(x, y);
            self.pending.caret = Some(CaretRect {
                x,
                y,
                height: self.measure.line_height(),
            });
        }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::input::keyboard::vk;

    /// Eight pixels a unit, 16 high.
    pub(crate) struct Fixed;

    impl TextMeasure for Fixed {
        fn extent(&self, text: &[u16]) -> Option<(i32, i32)> {
            Some((8 * text.len() as i32, 16))
        }
    }

    pub(crate) fn utf16(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    fn typed(b: &mut EditBox, s: &str) {
        for u in utf16(s) {
            b.handle_wide_char(u);
        }
    }

    fn text(b: &EditBox) -> String {
        String::from_utf16_lossy(b.text())
    }

    #[test]
    fn typing_inserts_at_the_caret() {
        let mut b = EditBox::new(Box::new(Fixed));
        typed(&mut b, "hello");
        assert_eq!(text(&b), "hello");
        assert_eq!(b.caret_index(), 5);
        assert!(b.take_updated() && !b.take_updated());
        assert!(b.handle_key(vk::HOME));
        assert!(!b.handle_key(vk::HOME));
        typed(&mut b, ">");
        assert!(b.handle_key(vk::END));
        assert_eq!(b.caret_index(), 6);
        assert!(!b.handle_key(vk::RIGHT), "not past the text");
        assert!(b.handle_key(vk::LEFT));
        typed(&mut b, "L");
        assert_eq!(text(&b), ">hellLo");
        // Control characters are refused.
        assert!(!b.handle_wide_char(0x08));
        assert!(!b.handle_wide_char(0x85));
        assert!(b.handle_wide_char(0x00a0));
    }

    #[test]
    fn deleting_cuts_the_text_at_the_caret() {
        let mut b = EditBox::new(Box::new(Fixed));
        typed(&mut b, "abcdef");
        for _ in 0..3 {
            b.handle_key(vk::LEFT);
        }
        // Delete writes a terminator at the caret: "abc".
        assert!(b.handle_key(vk::DELETE));
        assert_eq!(text(&b), "abc");
        assert_eq!(b.buffer()[..6], utf16("abc\0ef"));
        // Typing over the terminator brings the old tail back.
        typed(&mut b, "X");
        assert_eq!(text(&b), "abcXef");
        // Backspace moves left, then cuts there.
        assert!(b.handle_key(vk::BACK));
        assert_eq!(text(&b), "abc");
        b.handle_key(vk::HOME);
        assert!(!b.handle_key(vk::BACK), "nothing left of the caret");
        assert_eq!(text(&b), "abc");
    }

    #[test]
    fn overwrite_mode_cuts_before_each_unit() {
        let mut b = EditBox::new(Box::new(Fixed));
        typed(&mut b, "abcdef");
        b.handle_key(vk::HOME);
        b.handle_key(vk::RIGHT);
        assert!(!b.handle_key(vk::INSERT));
        assert!(!b.insert_mode() && !b.caret().blinks());
        // The cut at the caret is typed over: one unit replaced.
        typed(&mut b, "Z");
        assert_eq!(text(&b), "aZcdef");
        b.handle_key(vk::INSERT);
        assert!(b.insert_mode() && b.caret().blinks());
    }

    #[test]
    fn a_full_buffer_overwrites_its_last_unit_and_free_space_counts_from_the_caret() {
        let mut b = EditBox::new(Box::new(Fixed));
        b.set_input_buffer_size(4);
        typed(&mut b, "abcd");
        assert_eq!(b.free_buffer_size(), 0);
        typed(&mut b, "e");
        assert_eq!(text(&b), "abce");
        assert!(!b.pending.finalize);
        // In the middle the text grows until the buffer's end, and the
        // unit pushed past it is dropped.
        b.handle_key(vk::HOME);
        typed(&mut b, "Q");
        assert_eq!(text(&b), "Qabc");
        assert_eq!(b.buffer().len(), 5);
        assert_eq!(b.buffer()[4], 0);
        // A buffer of no units refuses and finalises the composition.
        let mut b = EditBox::new(Box::new(Fixed));
        b.set_input_buffer_size(0);
        typed(&mut b, "a");
        assert_eq!(text(&b), "");
        assert!(b.pending.finalize);
    }

    #[test]
    fn resizing_the_buffer_keeps_the_text_that_fits() {
        let mut b = EditBox::new(Box::new(Fixed));
        typed(&mut b, "abcdef");
        b.set_input_buffer_size(3);
        assert_eq!(text(&b), "abc");
        assert_eq!(b.caret_index(), 3);
        b.set_input_buffer_size(10);
        assert_eq!(text(&b), "abc");
        assert_eq!(b.buffer_size(), 10);
        // Units after the terminator are not copied.
        typed(&mut b, "de");
        b.handle_key(vk::HOME);
        b.handle_key(vk::RIGHT);
        b.handle_key(vk::RIGHT);
        b.handle_key(vk::DELETE);
        assert_eq!(b.buffer()[..5], utf16("ab\0de"));
        b.set_input_buffer_size(11);
        assert_eq!(b.buffer()[..5], utf16("ab\0\0\0"));
    }

    #[test]
    fn the_caret_follows_the_text_width_while_focused() {
        let mut b = EditBox::new(Box::new(Fixed));
        b.resize(Rect {
            left: 100,
            top: 40,
            right: 300,
            bottom: 60,
        });
        typed(&mut b, "abc");
        assert_eq!(b.caret_offset(), (24, 0));
        assert_eq!(b.pending.caret, None, "no IME caret without the focus");
        b.on_set_focus();
        assert!(b.caret().is_shown());
        assert_eq!(b.caret().position(), (125, 41));
        assert_eq!(
            b.pending.caret,
            Some(CaretRect {
                x: 125,
                y: 41,
                height: 16
            })
        );
        b.handle_key(vk::LEFT);
        assert_eq!(b.caret().position(), (117, 41));
        // An empty text keeps the last offset.
        b.set_input_buffer_size(0);
        b.set_input_buffer_size(5);
        b.resize(b.rect());
        assert_eq!(b.text(), []);
        assert_eq!(b.caret_offset(), (16, 0));
        b.on_kill_focus();
        assert!(!b.has_focus() && !b.caret().is_shown());
    }

    #[test]
    fn the_caret_blinks_unless_composing() {
        let mut c = Caret::default();
        assert!(!c.update(1000, 500, true), "hidden until shown");
        c.show(true);
        assert!(c.update(1000, 500, true));
        assert!(c.update(1400, 500, true));
        assert!(!c.update(1600, 500, true));
        assert!(!c.update(5000, 500, false), "hidden while composing");
        c.set_blink(false);
        assert!(c.update(9000, 500, true) && c.update(99_000, 500, true));
        // The tick count wrapping restarts the stamp.
        let mut c = Caret::default();
        c.show(true);
        assert!(c.update(10_000, 500, true));
        assert!(c.update(100, 500, true));
        assert!(!c.update(700, 500, true));
    }

    #[test]
    fn titles_stop_at_62_units() {
        let mut b = EditBox::new(Box::new(Fixed));
        b.set_title(&[0x41; 100]);
        assert_eq!(b.title().len(), TITLE_CAPACITY);
        b.set_title(&utf16("ab\0cd"));
        assert_eq!(b.title(), utf16("ab"));
        b.set_margins(3, 4);
        assert_eq!(b.margins(), (3, 4));
    }
}
