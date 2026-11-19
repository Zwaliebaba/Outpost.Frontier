//! Rich-text execution and layout (work package P4.12): the text render
//! context of `CppSource/Common/common_textrender.cpp`, what each display
//! list node does against it (`common_rdlnodes.cpp`), the list functions,
//! the style registry and `LayoutRichText` of
//! `common_renderdisplaylist.cpp`, and the `[Style]` sections of
//! `rich_fonts.ini` (`menu_fonts_load.cpp`).
//!
//! # How the reference lays rich text out
//!
//! A `TextRenderContext` holds the current attributes (font byte and
//! colour), a stack of default attributes, a stack of saved states, a pen
//! position and an origin; eleven virtual functions measure and draw in
//! the current font. Executing a list runs every node against it in order:
//! attribute nodes merge into the current attributes, text nodes draw and
//! advance the pen, position nodes move it, clip nodes set the clip.
//!
//! `LayoutRichText` turns a source list into a list of lines: it walks the
//! nodes (descending into referenced lists and styles), measures each in
//! the font in effect, splits text runs at a word or a character to fit a
//! width, and writes every committed node behind a position node that
//! places it on its line by the line's ascent and the justification. The
//! result is executed against the device to draw ([`list`]).
//!
//! `DeviceTRC`, the context the client draws with, measures through GDI in
//! the faces of the rich-text font table and draws into a memory bitmap
//! ([`device`]).
//!
//! # The port
//!
//! The context is a trait, [`TextRenderContext`], over the base fields
//! ([`TrcState`]); its provided methods are the base class's. Node
//! behaviour is on [`RdlNode`] ([`nodes`]). The style registry is a value,
//! [`style::StyleCollection`], passed to what executes or lays out
//! (decision T10).

// The x87 compares are written as the reference makes them: a negated
// ordered compare is true for NaN.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod device;
pub mod list;
pub mod nodes;
pub mod style;

pub use list::{layout_rich_text, lex_compare};
pub use style::StyleCollection;

pub use crate::text::font::rich::VisualSize;
use crate::text::rdl::{ClipRect, DEFAULT_ATTRIBUTES, PackedAttributes, RichImage};

/// `TextRenderContext::State`: what `push_state` saves.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SavedState {
    /// The current attributes (`+0x00`).
    pub attributes: PackedAttributes,
    pub pos_v: i32,
    pub pos_h: i32,
    /// The clip rectangle the context reported (`+0x10`).
    pub clip: ClipRect,
}

/// The fields of `TextRenderContext`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TrcState {
    /// The state stack; its top (the reference's front) is the last.
    pub state_stack: Vec<SavedState>,
    /// The attribute stack; its top is the last.
    pub attr_stack: Vec<PackedAttributes>,
    /// `+0x1C`.
    pub current: PackedAttributes,
    /// `+0x24`.
    pub default: PackedAttributes,
    pub pos_v: i32,
    pub pos_h: i32,
    pub origin_v: i32,
    pub origin_h: i32,
}

impl Default for TrcState {
    fn default() -> Self {
        TrcState::new()
    }
}

impl TrcState {
    /// The constructor: both stacks empty, both attributes the global
    /// default, the pen and the origin at 0.
    #[must_use]
    pub fn new() -> TrcState {
        TrcState {
            state_stack: Vec::new(),
            attr_stack: Vec::new(),
            current: DEFAULT_ATTRIBUTES,
            default: DEFAULT_ATTRIBUTES,
            pos_v: 0,
            pos_h: 0,
            origin_v: 0,
            origin_h: 0,
        }
    }

    /// The font byte of the current attributes, which indexes the face
    /// table.
    #[must_use]
    pub fn font_byte(&self) -> u8 {
        (self.current & 0xFF) as u8
    }
}

/// `TextRenderContext`: the base fields and the eleven device functions
/// (`create_image` and `destroy_image` are the device's own, see
/// [`device::DeviceTrc::create_image`]). Text is passed with its length:
/// the reference's `-1` (`wcslen`) is the caller's business. `None` is a
/// null text.
pub trait TextRenderContext {
    fn trc(&self) -> &TrcState;
    fn trc_mut(&mut self) -> &mut TrcState;

    /// `get_cell_width` (`+0x04`).
    fn cell_width(&mut self) -> i32;
    /// `get_cell_height` (`+0x08`).
    fn cell_height(&mut self) -> i32;
    /// `get_text_visual_size` (`+0x0C`).
    fn text_visual_size(&mut self, text: Option<&[u16]>, size: &mut VisualSize);
    /// `get_font_metrics` (`+0x10`): ascent and descent.
    fn font_metrics(&mut self) -> (i32, i32);
    /// `get_text_fit` (`+0x14`): how many leading characters fit in
    /// `max_width`.
    fn text_fit(&mut self, text: Option<&[u16]>, max_width: i32) -> i32;
    /// `execute_text` (`+0x18`): draws when `highlight` is set, and
    /// advances the pen.
    fn execute_text(&mut self, text: Option<&[u16]>, highlight: bool);
    /// `execute_image` (`+0x1C`): draws when `draw` is set, and advances
    /// the pen.
    fn execute_image(&mut self, image: Option<&RichImage>, draw: bool);
    /// `set_clip_rect` (`+0x20`).
    fn set_clip_rect(&mut self, rect: ClipRect);
    /// `get_clip_rect` (`+0x24`).
    fn clip_rect(&mut self) -> ClipRect;
    /// The rich-text image scale `GRichTextScaleY` that image nodes size
    /// themselves by.
    fn image_scale(&self) -> f32;

    /// `get_default_attributes`: the top of the attribute stack, else the
    /// default attributes.
    fn default_attributes(&self) -> PackedAttributes {
        let trc = self.trc();
        trc.attr_stack.last().copied().unwrap_or(trc.default)
    }

    /// `push_default_attributes`: pushes the *current* attributes.
    fn push_default_attributes(&mut self) {
        let current = self.trc().current;
        self.trc_mut().attr_stack.push(current);
    }

    /// `pop_default_attributes`; nothing on an empty stack (the original
    /// unlinks its list head; the reference port, like this one, does not).
    fn pop_default_attributes(&mut self) {
        self.trc_mut().attr_stack.pop();
    }

    /// `clear_default_attributes`.
    fn clear_default_attributes(&mut self) {
        self.trc_mut().attr_stack.clear();
    }

    /// `set_pos`.
    fn set_pos(&mut self, h: i32, v: i32) {
        let trc = self.trc_mut();
        trc.pos_h = h;
        trc.pos_v = v;
    }

    /// `get_state`: the attributes, the pen and the clip rectangle.
    fn get_state(&mut self) -> SavedState {
        let (attributes, pos_v, pos_h) = {
            let t = self.trc();
            (t.current, t.pos_v, t.pos_h)
        };
        SavedState {
            attributes,
            pos_v,
            pos_h,
            clip: self.clip_rect(),
        }
    }

    /// `set_state`.
    fn set_state(&mut self, state: &SavedState) {
        let trc = self.trc_mut();
        trc.current = state.attributes;
        trc.pos_v = state.pos_v;
        trc.pos_h = state.pos_h;
        self.set_clip_rect(state.clip);
    }

    /// `push_state`.
    fn push_state(&mut self) {
        let state = self.get_state();
        self.trc_mut().state_stack.push(state);
    }

    /// `pop_state`; nothing on an empty stack (the original restores from
    /// its list head; the reference port, like this one, does not).
    fn pop_state(&mut self) {
        let Some(state) = self.trc().state_stack.last().copied() else {
            return;
        };
        self.set_state(&state);
        self.trc_mut().state_stack.pop();
    }

    /// `reset_to_default`: the attribute stack cleared (the reference logs
    /// its size first when it is not empty) and the current attributes the
    /// default ones. The pen stays.
    fn reset_to_default(&mut self) {
        let trc = self.trc_mut();
        trc.attr_stack.clear();
        trc.current = trc.default;
    }
}
