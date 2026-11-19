//! The IME's fonts (`CImeUiFont.cpp`, `IMEFocus.cpp`'s `FSymbolFont`): the
//! font contract the IME draws through (`CImeUiFont_Base`, here
//! [`ImeFont`]), an edit box's own font (`CImeUiFont`, here [`ImeUiFont`])
//! and the toolbar's symbol font ([`SymbolFont`]).
//!
//! `CImeUiFont` keeps a `LOGFONTA`, realises it on a memory DC with
//! `CreateFontIndirectA` when it changed, and measures with
//! `GetTextExtentPoint32W`. Here the face is made and measured by the
//! client's font backend (`text::font::FontBackend`: DirectWrite with GDI's
//! mapping on Windows, the deterministic metrics backend elsewhere), the
//! same calls the client's own fonts make (divergence D550). The backend is
//! shared with the font system through [`SharedFontBackend`].

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use crate::text::font::{FaceHandle, FontBackend, LogFont, TextMetrics};
use crate::text_input::edit_box::TextMeasure;

/// A font backend shared between the font system and the IME's fonts, as
/// the C++'s GDI is one: every call borrows it for its own duration.
#[derive(Clone)]
pub struct SharedFontBackend(Rc<RefCell<Box<dyn FontBackend>>>);

impl std::fmt::Debug for SharedFontBackend {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("SharedFontBackend")
    }
}

impl SharedFontBackend {
    #[must_use]
    pub fn new(backend: Box<dyn FontBackend>) -> SharedFontBackend {
        SharedFontBackend(Rc::new(RefCell::new(backend)))
    }
}

impl FontBackend for SharedFontBackend {
    fn create_font(&mut self, log_font: &LogFont) -> Option<FaceHandle> {
        self.0.borrow_mut().create_font(log_font)
    }
    fn delete_font(&mut self, face: FaceHandle) {
        self.0.borrow_mut().delete_font(face);
    }
    fn text_metrics(&mut self, face: FaceHandle) -> TextMetrics {
        self.0.borrow_mut().text_metrics(face)
    }
    fn char_width(&mut self, face: FaceHandle, ch: u16) -> Option<i32> {
        self.0.borrow_mut().char_width(face, ch)
    }
    fn text_extent(&mut self, face: FaceHandle, text: &[u16]) -> Option<(i32, i32)> {
        self.0.borrow_mut().text_extent(face, text)
    }
    fn draw_text(
        &mut self,
        face: FaceHandle,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        text: &[u16],
    ) -> Option<Vec<u8>> {
        self.0
            .borrow_mut()
            .draw_text(face, x, y, width, height, text)
    }
}

/// Where the IME's drawing goes: `GDrawRect`, `GDrawFan` (with the symbol
/// font's text and colour, as `StaticDrawFan` passes them) and an IME
/// font's text through its owner box (`DrawString`, slot 12).
pub trait ImeDraw {
    /// `GDrawRect` (`IMEFocus::DrawRect`).
    fn draw_rect(&mut self, x1: i32, y1: i32, x2: i32, y2: i32, color: u32);
    /// `GDrawFan` (`IMEFocus::DrawTextW` with the symbol and its colour).
    fn draw_fan(&mut self, vertices: &[FanVertex], text: &[u16], color: u32);
    /// The owner box's `DrawString`.
    fn draw_string(&mut self, x: i32, y: i32, color: u32, text: &[u16]);
}

/// One vertex of the toolbar's fan (`FIMEToolbarVertex`, 0x20 bytes: an
/// XYZRHW position and an `AARRGGBB` colour).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FanVertex {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub rhw: f32,
    pub color: u32,
}

/// `CImeUiFont_Base`: the five calls the IME makes of a font.
pub trait ImeFont {
    /// `SetHeight`.
    fn set_height(&self, height: i32);
    /// `SetColor`.
    fn set_color(&self, color: u32);
    /// `SetPosition`.
    fn set_position(&self, x: i32, y: i32);
    /// `GetTextExtent`: width and height of the text up to its first NUL.
    fn text_extent(&self, text: &[u16]) -> (i32, i32);
    /// `Draw`.
    fn draw(&self, text: &[u16], out: &mut dyn ImeDraw);
}

/// The text up to its first NUL (`lstrlenW`).
fn c_str(text: &[u16]) -> &[u16] {
    let end = text.iter().position(|&u| u == 0).unwrap_or(text.len());
    &text[..end]
}

/// A `LOGFONTA` of zeros (`memset` in the constructor).
#[must_use]
pub fn zeroed_log_font() -> LogFont {
    LogFont {
        height: 0,
        width: 0,
        escapement: 0,
        orientation: 0,
        weight: 0,
        italic: 0,
        underline: 0,
        strike_out: 0,
        char_set: 0,
        out_precision: 0,
        clip_precision: 0,
        quality: 0,
        pitch_and_family: 0,
        face_name: [0; 32],
    }
}

#[derive(Debug)]
struct FontState {
    /// The memory DC exists (`CreateCompatibleDC` never fails here).
    dc: bool,
    face: Option<FaceHandle>,
    log_font: LogFont,
    dirty: bool,
    color: u32,
    x: i32,
    y: i32,
}

/// `CImeUiFont`: an edit box's font. The face is realised lazily, when the
/// `LOGFONTA` changed and the font is next used.
pub struct ImeUiFont {
    backend: RefCell<Box<dyn FontBackend>>,
    state: RefCell<FontState>,
}

impl std::fmt::Debug for ImeUiFont {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ImeUiFont")
            .field("state", &self.state)
            .finish_non_exhaustive()
    }
}

impl ImeUiFont {
    /// The constructor: a zeroed `LOGFONTA`, no DC, no face, colour and
    /// position 0.
    #[must_use]
    pub fn new(backend: Box<dyn FontBackend>) -> ImeUiFont {
        ImeUiFont {
            backend: RefCell::new(backend),
            state: RefCell::new(FontState {
                dc: false,
                face: None,
                log_font: zeroed_log_font(),
                dirty: false,
                color: 0,
                x: 0,
                y: 0,
            }),
        }
    }

    /// `UpdateFont`: when dirty, the old face goes and the `LOGFONTA` is
    /// realised again (no face when the backend cannot make one).
    pub fn update_font(&self) {
        let mut s = self.state.borrow_mut();
        if !s.dirty {
            return;
        }
        let mut backend = self.backend.borrow_mut();
        if let Some(face) = s.face.take() {
            backend.delete_font(face);
        }
        s.dc = true;
        s.face = backend.create_font(&s.log_font);
        s.dirty = false;
    }

    /// `SetLogFont`: a different `LOGFONTA` is copied and marks the font
    /// dirty.
    pub fn set_log_font(&self, log_font: &LogFont) {
        let mut s = self.state.borrow_mut();
        if s.log_font != *log_font {
            s.log_font = *log_font;
            s.dirty = true;
        }
    }

    /// `GetLogFont`.
    #[must_use]
    pub fn log_font(&self) -> LogFont {
        self.state.borrow().log_font
    }

    /// `GetHFont`: the face, realised first.
    pub fn face(&self) -> Option<FaceHandle> {
        self.update_font();
        self.state.borrow().face
    }

    /// The colour and position the next draw uses.
    #[must_use]
    pub fn color_and_position(&self) -> (u32, i32, i32) {
        let s = self.state.borrow();
        (s.color, s.x, s.y)
    }

    /// `IMEInputBox::CalcCaretPos`'s `ImeUi_GetTextExtent` of the first
    /// `text.len()` units on the font's DC: `None` before the font was
    /// first realised (no DC) or when it has no face (the C++ measures in
    /// the DC's default font then, D550).
    #[must_use]
    pub fn dc_extent(&self, text: &[u16]) -> Option<(i32, i32)> {
        self.update_font();
        let s = self.state.borrow();
        if !s.dc {
            return None;
        }
        let face = s.face?;
        self.backend.borrow_mut().text_extent(face, text)
    }
}

impl Drop for ImeUiFont {
    fn drop(&mut self) {
        let face = self.state.get_mut().face.take();
        if let Some(face) = face {
            self.backend.get_mut().delete_font(face);
        }
    }
}

impl ImeFont for ImeUiFont {
    /// `SetHeight`: `lfHeight` becomes the negated height (an em height),
    /// dirty when it changed.
    fn set_height(&self, height: i32) {
        let mut s = self.state.borrow_mut();
        let negative = height.wrapping_neg();
        if s.log_font.height != negative {
            s.log_font.height = negative;
            s.dirty = true;
        }
    }

    fn set_color(&self, color: u32) {
        self.state.borrow_mut().color = color;
    }

    fn set_position(&self, x: i32, y: i32) {
        let mut s = self.state.borrow_mut();
        s.x = x;
        s.y = y;
    }

    /// `GetTextExtent`: 0 by 0 for an empty text or without a DC and a
    /// face, else the extent of the text up to its NUL.
    fn text_extent(&self, text: &[u16]) -> (i32, i32) {
        let text = c_str(text);
        if text.is_empty() {
            return (0, 0);
        }
        self.update_font();
        let s = self.state.borrow();
        match (s.dc, s.face) {
            (true, Some(face)) => self
                .backend
                .borrow_mut()
                .text_extent(face, text)
                .unwrap_or((0, 0)),
            _ => (0, 0),
        }
    }

    /// `Draw`: the font realised, then the owner box's `DrawString` at the
    /// font's position and colour.
    fn draw(&self, text: &[u16], out: &mut dyn ImeDraw) {
        self.update_font();
        let (color, x, y) = self.color_and_position();
        out.draw_string(x, y, color, text);
    }
}

/// The box's caret measure is its font: `CalcCaretPos` measures the text
/// before the caret on the font's DC. The line height the IME windows'
/// caret carries (the port's, for the system's candidate window) is the
/// extent of an empty text, which realises the face; a focused box so
/// makes its face when it first places the caret, before its first
/// measure (D550).
impl TextMeasure for ImeUiFont {
    fn extent(&self, text: &[u16]) -> Option<(i32, i32)> {
        self.dc_extent(text)
    }
}

impl<T: TextMeasure + ?Sized> TextMeasure for Rc<T> {
    fn extent(&self, text: &[u16]) -> Option<(i32, i32)> {
        (**self).extent(text)
    }
    fn line_height(&self) -> i32 {
        (**self).line_height()
    }
}

/// `FSymbolFont`: the toolbar's font. Height, position and draw do nothing
/// and every extent is 0 by 0; its colour and symbol (never set by the
/// C++, so empty) go with the fan.
#[derive(Debug)]
pub struct SymbolFont {
    symbol: RefCell<[u16; 5]>,
    color: Cell<u32>,
}

impl Default for SymbolFont {
    fn default() -> Self {
        SymbolFont {
            symbol: RefCell::new([0; 5]),
            color: Cell::new(0xFFFF_FFFF),
        }
    }
}

impl SymbolFont {
    /// `SetSymbol`: up to four units and a terminator.
    pub fn set_symbol(&self, text: &[u16]) {
        let mut symbol = [0u16; 5];
        for (dst, &src) in symbol.iter_mut().zip(c_str(text).iter().take(4)) {
            *dst = src;
        }
        *self.symbol.borrow_mut() = symbol;
    }

    /// The symbol up to its NUL.
    #[must_use]
    pub fn symbol(&self) -> Vec<u16> {
        c_str(&*self.symbol.borrow()).to_vec()
    }

    #[must_use]
    pub fn color(&self) -> u32 {
        self.color.get()
    }
}

impl ImeFont for SymbolFont {
    fn set_height(&self, _height: i32) {}
    fn set_color(&self, color: u32) {
        self.color.set(color);
    }
    fn set_position(&self, _x: i32, _y: i32) {}
    fn text_extent(&self, _text: &[u16]) -> (i32, i32) {
        (0, 0)
    }
    fn draw(&self, _text: &[u16], _out: &mut dyn ImeDraw) {}
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::text::font::fake::FakeBackend;

    fn units(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    fn arial(height: i32) -> LogFont {
        LogFont::new(b"Arial", height, false, false, false)
    }

    #[derive(Default)]
    struct Strings(Vec<(i32, i32, u32, Vec<u16>)>);

    impl ImeDraw for Strings {
        fn draw_rect(&mut self, _: i32, _: i32, _: i32, _: i32, _: u32) {}
        fn draw_fan(&mut self, _: &[FanVertex], _: &[u16], _: u32) {}
        fn draw_string(&mut self, x: i32, y: i32, color: u32, text: &[u16]) {
            self.0.push((x, y, color, text.to_vec()));
        }
    }

    #[test]
    fn the_face_is_made_when_the_font_is_next_used() {
        let (fake, log) = FakeBackend::with_log();
        let font = ImeUiFont::new(Box::new(SharedFontBackend::new(Box::new(fake))));
        assert_eq!(font.dc_extent(&units("ab")), None, "no DC yet");
        assert_eq!(font.text_extent(&units("ab")), (0, 0));
        font.set_log_font(&arial(16));
        assert!(log.borrow().created.is_empty());
        assert_eq!(font.text_extent(&units("ab\0cd")), (16, 16));
        assert_eq!(font.dc_extent(&[]), Some((0, 16)));
        assert_eq!(font.text_extent(&[]), (0, 0));
        assert_eq!(log.borrow().created, [16]);
        // The same LOGFONT again changes nothing; a new height makes the
        // face again.
        font.set_log_font(&arial(16));
        font.set_height(16);
        assert_eq!(font.log_font().height, -16);
        assert_eq!(font.text_extent(&units("M")), (19, 19));
        assert_eq!(log.borrow().created, [16, -16]);
        assert_eq!(log.borrow().deleted, 1);
        drop(font);
        assert_eq!(log.borrow().deleted, 2);
    }

    #[test]
    fn a_face_the_backend_refuses_measures_nothing() {
        let font = ImeUiFont::new(Box::new(FakeBackend::default()));
        font.set_log_font(&LogFont::new(b"", 12, false, false, false));
        assert_eq!(font.text_extent(&units("a")), (0, 0));
        assert_eq!(font.dc_extent(&units("a")), None);
        assert_eq!(font.face(), None);
    }

    #[test]
    fn drawing_goes_to_the_owner_at_the_position_and_colour() {
        let font = ImeUiFont::new(Box::new(FakeBackend::default()));
        font.set_position(3, -4);
        font.set_color(0xFF00_FF00);
        let mut out = Strings::default();
        font.draw(&units("x"), &mut out);
        assert_eq!(out.0, [(3, -4, 0xFF00_FF00, units("x"))]);
        let symbol = SymbolFont::default();
        assert_eq!(symbol.color(), 0xFFFF_FFFF);
        symbol.set_color(7);
        symbol.set_symbol(&units("abcdef"));
        assert_eq!(symbol.symbol(), units("abcd"));
        assert_eq!(symbol.text_extent(&units("abc")), (0, 0));
        symbol.draw(&units("x"), &mut out);
        assert_eq!(out.0.len(), 1);
        assert_eq!(symbol.color(), 7);
    }
}
