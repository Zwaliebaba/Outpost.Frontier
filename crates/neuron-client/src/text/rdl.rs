//! Rich text: the display-list node model of `Common`'s `RenderDisplayList`
//! (`CppSource/include/common_text.h`, `CppSource/Common/common_rdlnodes.cpp`)
//! and the client code that fills lists from text
//! (`CppSource/OutpostFrontier/Client/ids_rdl.cpp`). Work package P4.12.
//!
//! A list is a sequence of nodes executed in order against a text render
//! context: attribute changes, text runs, positions, paragraph breaks and
//! so on. Lists come from the rich-text cards and from markup strings
//! ([`markup`], the RDLX tags), from plain text split at line breaks
//! ([`append_plain_text`]), from the binary chat records (decoded by
//! `game-sim::protocol::rdl`), and from code.
//!
//! Text stays UTF-16 (decision T15): it is resource and wire text, and
//! the list's element indices (caret and selection positions) count
//! UTF-16 units.
//!
//! Executing, measuring, splitting and laying lists out against a text
//! render context is [`crate::text::layout`].

pub mod markup;
pub mod xml;

use std::rc::Rc;

use neuron_core::catalog::TextCatalogs;

/// The packed text attributes `TextRenderAttributes` holds: bits 0..=2
/// bold, italic and underline, bits 3..=7 the font index, bits 8..=31 the
/// colour as `0x00BBGGRR` shifted up by 8.
pub type PackedAttributes = u32;

/// The attribute bits of the colour.
pub const COLOR_BITS: u32 = 0xFFFF_FF00;
/// The attribute bits of the font index.
pub const FONT_BITS: u32 = 0xF8;
/// The bold bit.
pub const BOLD_BIT: u32 = 1;
/// The italic bit.
pub const ITALIC_BIT: u32 = 2;
/// The underline bit.
pub const UNDERLINE_BIT: u32 = 4;

/// The global default attributes (`GDefaultTextAttribsPacked`): font
/// index 0, white.
pub const DEFAULT_ATTRIBUTES: PackedAttributes = 0xFFFF_FF00;

/// A text render attribute change (`TRANode`).
///
/// `mask` selects the bits the node sets; `default_mask` selects, among
/// them, the bits taken from the context's default attributes instead of
/// from `attrs`. Every list holds clones, and the clone is built with the
/// masking constructor, so in a list `attrs` has no bits outside `mask`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextAttributes {
    /// The attribute values (`+0x04`).
    pub attrs: PackedAttributes,
    /// The bits the node applies (`+0x0C`).
    pub mask: u32,
    /// The bits inherited from the default attributes (`+0x10`).
    pub default_mask: u32,
}

impl TextAttributes {
    /// `TRANode(attrs, mask, default_mask)`: `attrs` masked by `mask`.
    #[must_use]
    pub fn new(attrs: PackedAttributes, mask: u32, default_mask: u32) -> Self {
        Self {
            attrs: attrs & mask,
            mask,
            default_mask,
        }
    }

    /// `TRANode::Execute`: the current attributes after this node, given
    /// the context's current and default attributes.
    #[must_use]
    pub fn apply(&self, current: PackedAttributes, default: PackedAttributes) -> PackedAttributes {
        let merged =
            ((self.attrs & !self.default_mask) | (default & self.default_mask)) & self.mask;
        merged | (current & !self.mask)
    }
}

/// The operation of an attribute-stack node (`AttribStackNode::Op`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StackOp {
    /// 0: push the default attributes (`<PUSH/>`).
    Push,
    /// 1: pop them (`<POP/>`).
    Pop,
    /// 2: clear the stack.
    Clear,
    /// Any other value; does nothing. The default constructor stores 3.
    Other(i32),
}

impl StackOp {
    /// The stored value.
    #[must_use]
    pub fn value(self) -> i32 {
        match self {
            Self::Push => 0,
            Self::Pop => 1,
            Self::Clear => 2,
            Self::Other(value) => value,
        }
    }

    /// The operation a stored value means.
    #[must_use]
    pub fn from_value(value: i32) -> Self {
        match value {
            0 => Self::Push,
            1 => Self::Pop,
            2 => Self::Clear,
            other => Self::Other(other),
        }
    }
}

/// Horizontal justification (`TextJustify`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Justify {
    /// 0.
    Left,
    /// 1.
    Center,
    /// 2.
    Right,
    /// Any other value (3 is unnamed in the reference).
    Other(i32),
}

impl Justify {
    /// The stored value.
    #[must_use]
    pub fn value(self) -> i32 {
        match self {
            Self::Left => 0,
            Self::Center => 1,
            Self::Right => 2,
            Self::Other(value) => value,
        }
    }

    /// The justification a stored value means.
    #[must_use]
    pub fn from_value(value: i32) -> Self {
        match value {
            0 => Self::Left,
            1 => Self::Center,
            2 => Self::Right,
            other => Self::Other(other),
        }
    }
}

/// A pen position (`PositionNode`): absolute, or relative to the current
/// position, per axis. `Execute` truncates the sum to an integer.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Position {
    /// The horizontal position is relative (`+0x04`).
    pub rel_h: bool,
    /// The vertical position is relative (`+0x05`).
    pub rel_v: bool,
    /// The vertical position (`+0x08`).
    pub pos_v: f32,
    /// The horizontal position (`+0x0C`).
    pub pos_h: f32,
}

/// A clip rectangle (`ClipNode`, a `RECT`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ClipRect {
    /// Left edge.
    pub left: i32,
    /// Top edge.
    pub top: i32,
    /// Right edge.
    pub right: i32,
    /// Bottom edge.
    pub bottom: i32,
}

/// An image a context made (`TextRenderContext::Image`, `DeviceTRC`'s
/// `BMPImage`): its size and, when it loaded, its pixels.
#[derive(Clone, Debug, PartialEq)]
pub struct RichImage {
    /// Width in pixels at scale 1 (`+0x04`, the bitmap's width as a float).
    pub width: f32,
    /// Height in pixels at scale 1 (`+0x08`).
    pub height: f32,
    /// The bitmap; `None` when the file did not load (the size is then 0).
    pub bitmap: Option<neuron_core::image::RgbImage>,
}

impl RichImage {
    /// An image of a size with no pixels (what a context without a
    /// bitmap, such as a test device, makes).
    #[must_use]
    pub fn sized(width: f32, height: f32) -> RichImage {
        RichImage {
            width,
            height,
            bitmap: None,
        }
    }
}

/// One node of a list. The variants are the twelve `RDLNode` classes.
#[derive(Clone, Debug, PartialEq)]
pub enum RdlNode {
    /// `NullNode`: does nothing.
    Null,
    /// `TRANode`.
    Attributes(TextAttributes),
    /// `TextNode`: a text run the node owns.
    Text(Vec<u16>),
    /// `TextPtrNode`: a text run the reference points at in the resource
    /// it came from; here it holds a copy (divergence D234). It is not a
    /// `TextNode` to the type queries (markup characters do not append to
    /// it).
    TextPtr(Vec<u16>),
    /// `AttribStackNode`.
    AttribStack(StackOp),
    /// `ImageNode`, with no image or with one.
    Image(Option<Rc<RichImage>>),
    /// `PositionNode`.
    Position(Position),
    /// `ParagraphNode`: a line break.
    Paragraph,
    /// `JustifyNode`.
    Justify(Justify),
    /// `ClipNode`.
    Clip(ClipRect),
    /// `StyleNode`: a style of the style collection by ID.
    Style(u16),
    /// `RDLRefNode`: another list, shared (a clone refers to the same
    /// list). The reference holds a C++ reference to a list its owner may
    /// still change; this one is shared and fixed (divergence D234).
    Ref(Rc<RenderDisplayList>),
}

impl RdlNode {
    /// `GetElementSize`: how many caret positions the node has (text units,
    /// one for an image, the referenced list's count for a reference).
    #[must_use]
    pub fn element_size(&self) -> usize {
        match self {
            Self::Text(text) | Self::TextPtr(text) => text.len(),
            Self::Image(image) => usize::from(image.is_some()),
            Self::Ref(list) => list.element_count(),
            _ => 0,
        }
    }
}

/// `RenderDisplayList`: the nodes in order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RenderDisplayList {
    /// The nodes.
    pub nodes: Vec<RdlNode>,
}

impl RenderDisplayList {
    /// An empty list.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `get_element_count`: the sum of the nodes' element sizes.
    #[must_use]
    pub fn element_count(&self) -> usize {
        self.nodes.iter().map(RdlNode::element_size).sum()
    }

    /// Appends a node (`RdlAppendNode`, `RdlxAppendNode`).
    pub fn push(&mut self, node: RdlNode) {
        self.nodes.push(node);
    }

    /// `RdlRemoveLastParagraph`: removes the last paragraph break, scanning
    /// from the end; whether there was one.
    pub fn remove_last_paragraph(&mut self) -> bool {
        match self.nodes.iter().rposition(|n| *n == RdlNode::Paragraph) {
            Some(index) => {
                self.nodes.remove(index);
                true
            }
            None => false,
        }
    }
}

/// The single space `AppendTextAsRdl` puts on an empty line.
const EMPTY_LINE: u16 = 0x20;

const CR: u16 = 0x0D;
const LF: u16 = 0x0A;
const VT: u16 = 0x0B;

fn text_run(list: &mut RenderDisplayList, run: &[u16], use_text_ptr: bool) {
    if run.is_empty() {
        return;
    }
    list.push(if use_text_ptr {
        RdlNode::TextPtr(run.to_vec())
    } else {
        RdlNode::Text(run.to_vec())
    });
}

/// `AppendTextAsRdl`: plain text split at CR, LF and VT into text runs
/// and paragraph breaks. A CR LF pair and an LF CR pair are one break. An
/// empty line before an LF or a VT gets a single space (always a
/// [`RdlNode::Text`]); one before a CR gets nothing.
pub fn append_plain_text(list: &mut RenderDisplayList, text: &[u16], use_text_ptr: bool) {
    let mut start = 0;
    let mut at = 0;
    while at < text.len() {
        let unit = text[at];
        if unit == CR {
            text_run(list, &text[start..at], use_text_ptr);
            list.push(RdlNode::Paragraph);
            at += 1;
            if text.get(at) == Some(&LF) {
                at += 1;
            }
            start = at;
        } else if unit == LF || unit == VT {
            if start == at {
                list.push(RdlNode::Text(vec![EMPTY_LINE]));
            } else {
                text_run(list, &text[start..at], use_text_ptr);
            }
            list.push(RdlNode::Paragraph);
            at += 1;
            if unit == LF && text.get(at) == Some(&CR) {
                at += 1;
            }
            start = at;
        } else {
            at += 1;
        }
    }
    text_run(list, &text[start..], use_text_ptr);
}

/// `AppendTextMaybeXmlRdl`: the text as markup ([`markup::read`]), and
/// when that fails, also as plain text. The nodes the markup reader added
/// before it failed stay in the list, as in the reference.
pub fn append_text_maybe_markup(list: &mut RenderDisplayList, text: &[u16], use_text_ptr: bool) {
    if !markup::read(list, text) {
        append_plain_text(list, text, use_text_ptr);
    }
}

/// `LoadIdsIntoRdl`: the card or string `ids` names, as markup or else as
/// plain text (text runs as [`RdlNode::TextPtr`]); false when the module
/// or the entry is missing.
pub fn load_ids(list: &mut RenderDisplayList, catalogs: &TextCatalogs, ids: u32) -> bool {
    let Some(text) = catalogs.text(ids) else {
        return false;
    };
    append_text_maybe_markup(list, &text.units(), true);
    true
}

/// `LoadIdsTextIntoRdl`: [`load_ids`] without the text's last paragraph
/// break; when the entry is missing, a justification node with the value 1
/// ([`Justify::Center`]; the reference's comment calls it left) and the
/// debug text up to its first NUL instead.
pub fn load_ids_text(
    list: &mut RenderDisplayList,
    catalogs: &TextCatalogs,
    ids: u32,
    debug_text: Option<&[u16]>,
) -> bool {
    if load_ids(list, catalogs, ids) {
        list.remove_last_paragraph();
        return true;
    }
    list.push(RdlNode::Justify(Justify::from_value(1)));
    if let Some(text) = debug_text {
        let end = text.iter().position(|&u| u == 0).unwrap_or(text.len());
        list.push(RdlNode::Text(text[..end].to_vec()));
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    fn units(text: &str) -> Vec<u16> {
        text.encode_utf16().collect()
    }

    fn text(t: &str) -> RdlNode {
        RdlNode::Text(units(t))
    }

    #[test]
    fn attributes_merge_like_tranode() {
        let bold = TextAttributes::new(0xFF, BOLD_BIT, 0);
        assert_eq!(bold.attrs, 1);
        assert_eq!(bold.apply(0x1234_5600, DEFAULT_ATTRIBUTES), 0x1234_5601);
        let default_color = TextAttributes::new(0, COLOR_BITS, COLOR_BITS);
        assert_eq!(default_color.apply(0x0000_FF05, 0xABCD_EF00), 0xABCD_EF05);
    }

    #[test]
    fn plain_text_splits_at_breaks() {
        let mut list = RenderDisplayList::new();
        append_plain_text(&mut list, &units("a\r\nb\n\rc\n\nd\r\re\x0b"), false);
        let p = RdlNode::Paragraph;
        assert_eq!(
            list.nodes,
            [
                text("a"),
                p.clone(),
                text("b"),
                p.clone(),
                text("c"),
                p.clone(),
                text(" "),
                p.clone(),
                text("d"),
                p.clone(),
                p.clone(),
                text("e"),
                p.clone()
            ]
        );
        let mut list = RenderDisplayList::new();
        append_plain_text(&mut list, &units("\nx"), true);
        assert_eq!(list.nodes, [text(" "), p, RdlNode::TextPtr(units("x"))]);
        let mut list = RenderDisplayList::new();
        append_plain_text(&mut list, &[], true);
        assert!(list.nodes.is_empty());
    }

    #[test]
    fn markup_failure_falls_back_to_plain_text() {
        let mut list = RenderDisplayList::new();
        append_text_maybe_markup(&mut list, &units("<P>x"), false);
        // The start tag `P` (a prefix of PARA) was read before the error.
        assert_eq!(list.nodes, [RdlNode::Paragraph, text("<P>x")]);
    }

    #[test]
    fn last_paragraph_goes() {
        let mut list = RenderDisplayList::new();
        list.push(RdlNode::Paragraph);
        list.push(text("x"));
        list.push(RdlNode::Paragraph);
        list.push(RdlNode::AttribStack(StackOp::Pop));
        assert!(list.remove_last_paragraph());
        assert_eq!(
            list.nodes,
            [
                RdlNode::Paragraph,
                text("x"),
                RdlNode::AttribStack(StackOp::Pop)
            ]
        );
        assert_eq!(list.element_count(), 1);
    }
}
