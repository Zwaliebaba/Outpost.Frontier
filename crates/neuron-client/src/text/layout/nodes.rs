//! What each node does against a context (`common_rdlnodes.cpp`):
//! `Execute`, `GetVisualSize`, `GetElementPos`, `SplitAtSize` and `Clone`.

use neuron_core::num::trunc_to_i32;

use super::style::StyleCollection;
use super::{TextRenderContext, VisualSize};
use crate::text::rdl::{RdlNode, StackOp, TextAttributes};

/// `iswspace` of the C runtime in the `"C"` locale for the units the
/// split looks at: tab to carriage return, space, the no-break space, and
/// above U+00FF the Unicode white space the system's character types are
/// taken to call space (divergence D297).
#[must_use]
pub fn is_wide_space(unit: u16) -> bool {
    matches!(
        unit,
        0x09..=0x0D
            | 0x20
            | 0xA0
            | 0x1680
            | 0x2000..=0x200A
            | 0x2028
            | 0x2029
            | 0x202F
            | 0x205F
            | 0x3000
    )
}

impl RdlNode {
    /// `Clone()`: an attribute node through the masking constructor, a
    /// text-pointer node through its (text, length) constructor, anything
    /// else field for field.
    #[must_use]
    pub fn cloned(&self) -> RdlNode {
        match self {
            RdlNode::Attributes(a) => {
                RdlNode::Attributes(TextAttributes::new(a.attrs, a.mask, a.default_mask))
            }
            other => other.clone(),
        }
    }

    /// `Execute(Ctx, bHighlight)`.
    pub fn execute(
        &self,
        ctx: &mut dyn TextRenderContext,
        styles: &StyleCollection,
        highlight: bool,
    ) {
        match self {
            RdlNode::Null | RdlNode::Paragraph | RdlNode::Justify(_) => {}
            RdlNode::Attributes(a) => {
                let default = ctx.default_attributes();
                let current = ctx.trc().current;
                ctx.trc_mut().current = a.apply(current, default);
            }
            RdlNode::Text(text) | RdlNode::TextPtr(text) => ctx.execute_text(Some(text), highlight),
            RdlNode::AttribStack(op) => match op {
                StackOp::Push => ctx.push_default_attributes(),
                StackOp::Pop => ctx.pop_default_attributes(),
                StackOp::Clear => ctx.clear_default_attributes(),
                StackOp::Other(_) => {}
            },
            RdlNode::Image(image) => ctx.execute_image(image.as_deref(), highlight),
            RdlNode::Position(p) => {
                // FLD + FIADD in double, then two truncating stores, V first.
                let mut h = f64::from(p.pos_h);
                if p.rel_h {
                    h += f64::from(ctx.trc().pos_h);
                }
                let mut v = f64::from(p.pos_v);
                if p.rel_v {
                    v += f64::from(ctx.trc().pos_v);
                }
                let trc = ctx.trc_mut();
                trc.pos_v = trunc_to_i32(v);
                trc.pos_h = trunc_to_i32(h);
            }
            RdlNode::Clip(rect) => ctx.set_clip_rect(*rect),
            RdlNode::Style(id) => styles.get(*id).execute(ctx, styles, highlight),
            RdlNode::Ref(list) => list.execute(ctx, styles, highlight),
        }
    }

    /// `GetVisualSize(Ctx, Size)`. A reference node leaves `size` as it
    /// was.
    pub fn visual_size(&self, ctx: &mut dyn TextRenderContext, size: &mut VisualSize) {
        match self {
            RdlNode::Text(text) | RdlNode::TextPtr(text) => ctx.text_visual_size(Some(text), size),
            RdlNode::Paragraph => {
                size.width = 0;
                size.height = ctx.cell_height();
                (size.ascent, size.descent) = ctx.font_metrics();
            }
            RdlNode::Image(Some(image)) => {
                let scale = f64::from(ctx.image_scale());
                size.width = trunc_to_i32(scale * f64::from(image.width));
                let height = trunc_to_i32(scale * f64::from(image.height));
                size.descent = 0;
                size.ascent = height;
                size.height = height;
            }
            RdlNode::Position(p) => {
                *size = VisualSize::default();
                if p.rel_h {
                    size.width = trunc_to_i32(f64::from(p.pos_h));
                }
                if p.rel_v {
                    size.height = trunc_to_i32(f64::from(p.pos_v));
                }
            }
            RdlNode::Ref(_) => {}
            _ => *size = VisualSize::default(),
        }
    }

    /// `GetElementPos(Index, Ctx, OutPos)`: the pen offset of a caret
    /// position within the node.
    pub fn element_pos(
        &self,
        index: i32,
        ctx: &mut dyn TextRenderContext,
        styles: &StyleCollection,
    ) -> i32 {
        match self {
            RdlNode::Text(text) | RdlNode::TextPtr(text) => {
                let count = i32::try_from(text.len()).unwrap_or(i32::MAX);
                if (0..count).contains(&index) {
                    let mut size = VisualSize::default();
                    ctx.text_visual_size(Some(&text[..index as usize]), &mut size);
                    size.width
                } else {
                    0
                }
            }
            RdlNode::Ref(list) => {
                let (h, _, _) = list.element_pos(ctx, styles, index);
                trunc_to_i32(f64::from(h))
            }
            _ => 0,
        }
    }

    /// `SplitAtSize(Ctx, MaxSize, Flags, Left, Right)`: a text run cut where
    /// it stops fitting `max_size`; without bit 0 of `flags`, back to just
    /// after the last white space before that point. Both halves are text
    /// nodes. `None` when the whole run fits or nothing would be left on
    /// the left; other nodes never split.
    #[must_use]
    pub fn split_at_size(
        &self,
        ctx: &mut dyn TextRenderContext,
        max_size: i32,
        flags: u32,
    ) -> Option<(RdlNode, RdlNode)> {
        let (RdlNode::Text(text) | RdlNode::TextPtr(text)) = self else {
            return None;
        };
        let fit = ctx.text_fit(Some(text), max_size);
        let len = i32::try_from(text.len()).unwrap_or(i32::MAX);
        if fit >= len {
            return None;
        }
        let mut split = fit.max(0) as usize;
        if flags & 1 == 0 {
            while split > 0 && !is_wide_space(text[split - 1]) {
                split -= 1;
            }
        }
        if split == 0 {
            return None;
        }
        Some((
            RdlNode::Text(text[..split].to_vec()),
            RdlNode::Text(text[split..].to_vec()),
        ))
    }
}
