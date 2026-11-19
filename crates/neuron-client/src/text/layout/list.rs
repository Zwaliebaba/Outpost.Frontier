//! The list functions of `common_renderdisplaylist.cpp`: `execute`,
//! `get_dimensions`, `get_element_pos`, `lex_compare` and
//! `LayoutRichText`.

use super::style::StyleCollection;
use super::{TextRenderContext, VisualSize};
use crate::text::rdl::{Position, RdlNode, RenderDisplayList, TextAttributes};

impl RenderDisplayList {
    /// `execute`: every node in order.
    pub fn execute(
        &self,
        ctx: &mut dyn TextRenderContext,
        styles: &StyleCollection,
        highlight: bool,
    ) {
        for node in &self.nodes {
            node.execute(ctx, styles, highlight);
        }
    }

    /// `get_dimensions`: the extent of the pen over the list and the
    /// largest ascent, executed from the default attributes at (0, 0)
    /// between a `push_state` and a `pop_state`. Width and height are the
    /// pen's range after each node (its position, not the nodes' extents);
    /// the minimums start at 100,000 and the maximums at 0, so an empty
    /// list gives -100,000 by -100,000.
    pub fn dimensions(
        &self,
        ctx: &mut dyn TextRenderContext,
        styles: &StyleCollection,
    ) -> (f32, f32, f32) {
        // The size every node writes into is the state record on the
        // stack: its ascent starts as the saved vertical pen.
        let mut size = VisualSize {
            width: ctx.trc().current.cast_signed(),
            height: 0,
            ascent: ctx.trc().pos_v,
            descent: ctx.trc().pos_h,
        };
        ctx.push_state();
        ctx.reset_to_default();
        ctx.set_pos(0, 0);
        let mut min_v = 100_000.0f32;
        let mut max_ascent = 0.0f32;
        let mut min_h = 100_000.0f32;
        let mut max_h = 0.0f32;
        let mut max_v = 0.0f32;
        for node in &self.nodes {
            node.visual_size(ctx, &mut size);
            node.execute(ctx, styles, false);
            let h = f64::from(ctx.trc().pos_h);
            let v = ctx.trc().pos_v as f32;
            if !(f64::from(max_h) > h) {
                max_h = h as f32;
            }
            if !(f64::from(min_h) < h) {
                min_h = h as f32;
            }
            if !(max_v > v) {
                max_v = v;
            }
            if !(min_v < v) {
                min_v = v;
            }
            let ascent = f64::from(size.ascent);
            if !(f64::from(max_ascent) > ascent) {
                max_ascent = ascent as f32;
            }
        }
        ctx.pop_state();
        (max_h - min_h, max_v - min_v, max_ascent)
    }

    /// `get_element_pos`: the nodes before the one holding caret position
    /// `index` are executed, then that node's offset is added to the pen.
    /// Gives the pen's horizontal and vertical position and the cell
    /// height; the reference's result is always true.
    pub fn element_pos(
        &self,
        ctx: &mut dyn TextRenderContext,
        styles: &StyleCollection,
        index: i32,
    ) -> (f32, f32, f32) {
        let mut index = index;
        let mut local = 0;
        for node in &self.nodes {
            let size = i32::try_from(node.element_size()).unwrap_or(i32::MAX);
            if index < size {
                local = node.element_pos(index, ctx, styles);
                break;
            }
            node.execute(ctx, styles, false);
            index = index.wrapping_sub(size);
        }
        let h = ctx.trc().pos_h.wrapping_add(local) as f32;
        let v = ctx.trc().pos_v as f32;
        let depth = ctx.cell_height() as f32;
        (h, v, depth)
    }
}

/// The text run a node offers `lex_compare`: a text node's when it is not
/// empty, a text-pointer node's always.
fn run(node: &RdlNode) -> Option<&[u16]> {
    match node {
        RdlNode::Text(text) if !text.is_empty() => Some(text),
        RdlNode::TextPtr(text) => Some(text),
        _ => None,
    }
}

/// `towlower` of the C runtime in the `"C"` locale: ASCII letters only
/// (divergence D297).
fn to_lower(unit: u16) -> u16 {
    if (0x41..=0x5A).contains(&unit) {
        unit + 0x20
    } else {
        unit
    }
}

/// `lex_compare(A, B, Raw)`: the text of two lists compared run by run,
/// raw or through `towlower`; the first difference of two units, -1 when
/// `a` runs out of text first, else 1 or 0 when `b` does.
///
/// As in the reference, `b`'s cursor stays on the node whose run it took,
/// so every later refill of `b` takes that run again from its start:
/// equal texts compare as -1. An empty text-pointer run in `b` makes the
/// reference loop for ever; the port skips it (divergence D298).
#[must_use]
pub fn lex_compare(a: &RenderDisplayList, b: &RenderDisplayList, raw: bool) -> i32 {
    let mut a_cur = 0usize;
    let mut b_cur = 0usize;
    let mut a_text: Option<&[u16]> = None;
    loop {
        let mut b_text: Option<&[u16]> = None;
        loop {
            while a_text.is_none() && a_cur < a.nodes.len() {
                a_text = run(&a.nodes[a_cur]);
                a_cur += 1;
            }
            if b_text.is_none() {
                loop {
                    let Some(node) = b.nodes.get(b_cur) else {
                        return i32::from(a_text.is_some());
                    };
                    if let Some(text) = run(node).filter(|t| !t.is_empty()) {
                        b_text = Some(text);
                        break;
                    }
                    b_cur += 1;
                }
            }
            let Some(a_run) = a_text else {
                return -1;
            };
            let b_run = b_text.unwrap_or_default();
            let common = a_run.len().min(b_run.len());
            for (&x, &y) in a_run[..common].iter().zip(&b_run[..common]) {
                let (x, y) = if raw {
                    (x, y)
                } else {
                    (to_lower(x), to_lower(y))
                };
                let diff = i32::from(x) - i32::from(y);
                if diff != 0 {
                    return diff;
                }
            }
            a_text = if a_run.len() > common {
                Some(&a_run[common..])
            } else {
                None
            };
            let rest = &b_run[common..];
            if rest.is_empty() {
                break;
            }
            b_text = Some(rest);
        }
    }
}

/// One line entry of `LayoutRichText`: the size, the node and whether it
/// is a paragraph end (tag 1, not written).
struct LineEntry {
    size: VisualSize,
    content: RdlNode,
    paragraph: bool,
}

/// The parameters of `LayoutRichText` after the context and the lists.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LayoutParams {
    /// `Param3`: the line width.
    pub width: i32,
    /// `Param5`: the horizontal origin of every line.
    pub origin_h: i32,
    /// `Param6`: the vertical origin of the first line.
    pub origin_v: i32,
    /// `Param7`: one line only, cut with a `.` marker.
    pub single_line: bool,
    /// `Param9`: text runs wrap at words.
    pub wrap: bool,
}

/// What `LayoutRichText` returns: whether any run was split, deferred or
/// dropped, and the laid-out height (`*Param8`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LayoutResult {
    pub broken: bool,
    pub height: i32,
}

/// `LayoutRichText`: `source` laid out into `out` as lines no wider than
/// `params.width`, each committed node behind a position node that places
/// it. The context's attributes, pen and clip come back as they were; its
/// attribute stack keeps what the nodes did to it, as in the reference.
///
/// A node is measured in the font in effect; one that fits is executed
/// (without drawing) and committed. One that does not fit is split at a
/// word when wrapping (not in single-line mode), else waits for the next
/// line when the line holds something and wrapping is on, else is split
/// at the last character that fits; a node that cannot split is committed
/// whole (dropped in single-line mode). Paragraph nodes end the line,
/// justification nodes set the mode of the lines that end after them,
/// reference and style nodes are replaced by their lists.
pub fn layout_rich_text(
    ctx: &mut dyn TextRenderContext,
    styles: &StyleCollection,
    source: &RenderDisplayList,
    out: &mut RenderDisplayList,
    params: LayoutParams,
) -> LayoutResult {
    let mut broken_any = false;
    let first = ctx.get_state();
    ctx.trc_mut().state_stack.push(first);
    let full_width = params.width;
    let mut width = params.width;

    // The "." marker in font byte 0x10.
    let marker = RdlNode::Text(vec![0x2E]);
    let marker_attrs = TextAttributes::new(0x10, 0xFF, 0);
    let saved = ctx.get_state();
    ctx.trc_mut().state_stack.push(saved);
    {
        let default = ctx.default_attributes();
        let current = ctx.trc().current;
        ctx.trc_mut().current = marker_attrs.apply(current, default);
    }
    let mut marker_size = VisualSize::default();
    ctx.text_visual_size(Some(&[0x2E]), &mut marker_size);
    if let Some(top) = ctx.trc().state_stack.last().copied() {
        ctx.set_state(&top);
        ctx.trc_mut().state_stack.pop();
    }
    if params.single_line && marker_size.width < width {
        width -= marker_size.width;
    }

    let mut line: Vec<LineEntry> = Vec::new();
    let mut justify = 0;
    let mut v_cursor = params.origin_v;
    let mut remaining = width;
    let mut frames: Vec<(&[RdlNode], usize)> = vec![(&source.nodes, 0)];
    let mut node: Option<RdlNode> = None;

    loop {
        if frames.is_empty() && node.is_none() && line.is_empty() {
            break;
        }
        let mut broken = false;
        let mut flush = false;
        if node.is_none() {
            match frames.last_mut() {
                None => flush = true,
                Some(top) => {
                    let (nodes, cursor) = *top;
                    if cursor == nodes.len() {
                        frames.pop();
                        continue;
                    }
                    top.1 += 1;
                    match &nodes[cursor] {
                        RdlNode::Ref(list) => {
                            frames.push((&list.nodes, 0));
                            continue;
                        }
                        RdlNode::Style(id) => {
                            frames.push((&styles.get(*id).nodes, 0));
                            continue;
                        }
                        other => node = Some(other.clone()),
                    }
                }
            }
        }

        if !flush && let Some(current) = node.take() {
            match current {
                RdlNode::Paragraph => {
                    current.execute(ctx, styles, false);
                    let mut size = VisualSize::default();
                    current.visual_size(ctx, &mut size);
                    line.push(LineEntry {
                        size,
                        content: current,
                        paragraph: true,
                    });
                }
                RdlNode::Justify(j) => {
                    justify = j.value();
                    current.execute(ctx, styles, false);
                    continue;
                }
                _ => {
                    let mut size = VisualSize::default();
                    current.visual_size(ctx, &mut size);
                    if size.width <= remaining {
                        current.execute(ctx, styles, false);
                        remaining = remaining.wrapping_sub(size.width);
                        line.push(LineEntry {
                            size,
                            content: current,
                            paragraph: false,
                        });
                        continue;
                    }
                    let mut done = false;
                    if !params.single_line {
                        if let Some((left, right)) = params
                            .wrap
                            .then(|| current.split_at_size(ctx, remaining, 0))
                            .flatten()
                        {
                            left.visual_size(ctx, &mut size);
                            line.push(LineEntry {
                                size,
                                content: left,
                                paragraph: false,
                            });
                            node = Some(right);
                            done = true;
                        } else if remaining != width && params.wrap {
                            // The node waits for the next line.
                            node = Some(current.clone());
                            done = true;
                        }
                        if done {
                            broken = true;
                            broken_any = true;
                        }
                    }
                    if !done {
                        if let Some((left, right)) = current.split_at_size(ctx, remaining, 1) {
                            left.visual_size(ctx, &mut size);
                            line.push(LineEntry {
                                size,
                                content: left,
                                paragraph: false,
                            });
                            node = Some(right);
                        } else if !params.single_line {
                            line.push(LineEntry {
                                size,
                                content: current,
                                paragraph: false,
                            });
                        }
                        broken = true;
                        broken_any = true;
                    }
                }
            }
        }

        // The line flush.
        if params.single_line && broken {
            if marker_size.width >= full_width {
                line.clear();
            }
            line.push(LineEntry {
                size: VisualSize::default(),
                content: RdlNode::Attributes(marker_attrs),
                paragraph: false,
            });
            line.push(LineEntry {
                size: marker_size,
                content: marker.clone(),
                paragraph: false,
            });
        }
        let mut line_width = 0i32;
        let mut max_below = 0;
        let mut max_ascent = 0;
        for entry in &line {
            let below = entry.size.height.wrapping_sub(entry.size.ascent);
            if below > max_below {
                max_below = below;
            }
            if entry.size.ascent > max_ascent {
                max_ascent = entry.size.ascent;
            }
            line_width = line_width.wrapping_add(entry.size.width);
        }
        let line_height = max_below.wrapping_add(max_ascent);
        let mut h_cursor = match justify {
            1 => (full_width.wrapping_sub(line_width) / 2).wrapping_add(params.origin_h),
            2 => full_width
                .wrapping_sub(line_width)
                .wrapping_add(params.origin_h),
            _ => params.origin_h,
        };
        for entry in &line {
            if !entry.paragraph {
                if entry.size.width > 0 {
                    out.push(RdlNode::Position(Position {
                        rel_h: false,
                        rel_v: false,
                        pos_h: h_cursor as f32,
                        pos_v: max_ascent
                            .wrapping_sub(entry.size.ascent)
                            .wrapping_add(v_cursor) as f32,
                    }));
                }
                out.push(entry.content.cloned());
            }
            h_cursor = h_cursor.wrapping_add(entry.size.width);
        }
        v_cursor = v_cursor.wrapping_add(line_height);
        line.clear();
        remaining = width;
        if params.single_line {
            break;
        }
    }

    if let Some(first) = ctx.trc().state_stack.last().copied() {
        ctx.trc_mut().current = first.attributes;
        ctx.set_pos(first.pos_h, first.pos_v);
        ctx.set_clip_rect(first.clip);
        ctx.trc_mut().state_stack.pop();
    }
    LayoutResult {
        broken: broken_any,
        height: v_cursor.wrapping_sub(params.origin_v),
    }
}
