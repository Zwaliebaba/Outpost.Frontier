//! The interface tree's node lines (`oracle_mode.cpp`: `FlattenFrom`,
//! `FlattenAll`, `NodeFields`), shared by `tree.jsonl`'s dumps and
//! `layout.jsonl`'s `winnode` records.
//!
//! A node's fields are its pre-order index, its parent's index, its depth,
//! the factory class name, `rtti`, the name, `flags` bits 0 to 3, whether it
//! and every ancestor are visible, the draw layer and sub-layer, the hit
//! priority, the position and the orientation; a `WinControl` adds `ctl`.
//!
//! `rtti` is the C++ type name, which decision O53 (4) leaves out of the
//! comparison: the port writes null. A `TextWin` adds `text` and `tw`, a
//! `TextControl` `text` and `tc`, found through the widgets' `text_win` and
//! `text_control` slots (the C++'s `dynamic_cast`s).
//!
//! `tree.jsonl`'s node lines ([`dump_nodes`]) and the script's `click`
//! targets ([`resolve_target`], [`click`]) are here too.

use neuron_client::ui::message::{ButtonAction, MouseButton};
use neuron_client::ui::tree::{NodeId, UNREGISTERED, Ui, VISIBLE};

use super::json::OracleJson;

/// `kMaxTreeNodes`.
pub const MAX_NODES: usize = 100_000;
/// `kMaxTreeDepth`.
pub const MAX_DEPTH: i32 = 64;

/// One node of a flattened tree (`FNode`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FlatNode {
    pub id: NodeId,
    /// The parent's index in the flattened list, -1 for a root.
    pub parent: i32,
    pub depth: i32,
    /// Its own and every ancestor's visible bit.
    pub visible: bool,
}

fn flatten_into(
    ui: &Ui,
    id: NodeId,
    parent: i32,
    depth: i32,
    parent_visible: bool,
    out: &mut Vec<FlatNode>,
) {
    let Some(node) = ui.node(id) else { return };
    if out.len() >= MAX_NODES || depth > MAX_DEPTH {
        return;
    }
    let visible = parent_visible && node.is(VISIBLE);
    let this = i32::try_from(out.len()).unwrap_or(i32::MAX);
    out.push(FlatNode {
        id,
        parent,
        depth,
        visible,
    });
    for &child in node.children() {
        if out.len() >= MAX_NODES {
            return;
        }
        flatten_into(ui, child, this, depth + 1, visible, out);
    }
}

/// `FlattenFrom(root, -1, 0, true)`: the subtree of `root` in pre-order.
#[must_use]
pub fn flatten_from(ui: &Ui, root: NodeId) -> Vec<FlatNode> {
    let mut out = Vec::new();
    flatten_into(ui, root, -1, 0, true, &mut out);
    out
}

/// `FlattenAll`: every registered root without a parent and not marked
/// unregistered, in registry order, each with its subtree.
#[must_use]
pub fn flatten_all(ui: &Ui) -> Vec<FlatNode> {
    let mut out = Vec::new();
    for id in ui.registered() {
        let Some(node) = ui.node(id) else { continue };
        if node.parent().is_some() || node.is(UNREGISTERED) {
            continue;
        }
        flatten_into(ui, id, -1, 0, true, &mut out);
    }
    out
}

fn floats(j: &mut OracleJson, name: &str, values: &[f32]) {
    j.begin_array(name);
    for &v in values {
        j.item_float(v);
    }
    j.end_array();
}

/// `NodeFields`: the fields of node `index`, after the caller's own keys.
pub fn node_fields(j: &mut OracleJson, ui: &Ui, flat: &FlatNode, index: i32) {
    let Some(node) = ui.node(flat.id) else { return };
    j.int("i", index).int("p", flat.parent).int("d", flat.depth);
    match node.class_name {
        Some(class) => j.str("class", class),
        None => j.null("class"),
    };
    j.null("rtti")
        .str_bytes("name", node.name())
        .uint("flags", u32::from(node.flags & 0x0f))
        .bool("vis", flat.visible)
        .uint("layer", node.draw_layer)
        .int("sub", node.draw_sub_layer)
        .int("hit", node.hit_priority);
    let p = node.position;
    floats(j, "pos", &[p.x, p.y, p.z]);
    let m = node.orientation.m;
    floats(
        j,
        "orient",
        &[
            m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2],
        ],
    );
    if let Some(c) = ui.win_control(flat.id) {
        j.begin_object("ctl");
        floats(j, "offset", &[c.offset.x, c.offset.y, c.offset.z]);
        floats(
            j,
            "half",
            &[c.half_extents.x, c.half_extents.y, c.half_extents.z],
        );
        floats(
            j,
            "hitoff",
            &[c.hit_offset.x, c.hit_offset.y, c.hit_offset.z],
        );
        j.int("tooltip", c.tooltip_ids)
            .str_bytes("mesh", &c.mesh_path)
            .str_bytes("hp", &c.hardpoint)
            .uint("nz", u32::from(c.no_z_enable))
            .end_object();
    }
    let widget = node.widget();
    if let Some(t) = widget.and_then(|w| w.text_win()) {
        wide(j, "text", t.text.text());
        j.begin_object("tw")
            .float("scale", t.text_scale)
            .float("w", t.width)
            .float("h", t.height)
            .end_object();
    } else if let Some(t) = widget.and_then(|w| w.text_control()) {
        wide(j, "text", t.text.text());
        j.begin_object("tc")
            .float("w", t.width)
            .float("h", t.height)
            .end_object();
    }
}

fn wide(j: &mut OracleJson, name: &str, text: Option<&[u16]>) {
    match text {
        Some(t) => j.wstr(name, t),
        None => j.null(name),
    };
}

/// The text a `TextWin` or `TextControl` shows (`NodeText`), or `None`.
#[must_use]
pub fn node_text(ui: &Ui, id: NodeId) -> Option<&[u16]> {
    let w = ui.node(id)?.widget()?;
    if let Some(t) = w.text_win() {
        return t.text.text();
    }
    w.text_control()?.text.text()
}

/// `CaptionEquals`: the text equals the caption, ASCII case ignored; a
/// unit beyond ASCII never matches.
#[must_use]
pub fn caption_equals(text: Option<&[u16]>, caption: &str) -> bool {
    let Some(text) = text else {
        return false;
    };
    let caption = caption.as_bytes();
    let end = text.iter().position(|&u| u == 0).unwrap_or(text.len());
    let text = &text[..end];
    text.len() == caption.len()
        && text
            .iter()
            .zip(caption)
            .all(|(&u, &c)| u <= 0x7f && u8::try_from(u).is_ok_and(|b| b.eq_ignore_ascii_case(&c)))
}

/// `ResolveTarget(spec)`: `#<n>` is node n of the tree flattened now; else
/// the first node whose name (ASCII case ignored) or, after `text:`, whose
/// caption matches, visible ones first.
///
/// # Errors
///
/// The note: "no such node" or "not found".
pub fn resolve_target(ui: &Ui, spec: &str) -> Result<NodeId, String> {
    let nodes = flatten_all(ui);
    if let Some(n) = spec.strip_prefix('#') {
        return super::script::parse_unsigned(n)
            .and_then(|i| nodes.get(usize::try_from(i).ok()?))
            .map(|n| n.id)
            .ok_or_else(|| "no such node".to_string());
    }
    let caption = spec.strip_prefix("text:");
    let mut fallback = None;
    for node in &nodes {
        let matched = match caption {
            Some(c) => caption_equals(node_text(ui, node.id), c),
            None => ui
                .node(node.id)
                .is_some_and(|n| n.name().eq_ignore_ascii_case(spec.as_bytes())),
        };
        if !matched {
            continue;
        }
        if node.visible {
            return Ok(node.id);
        }
        fallback.get_or_insert(node.id);
    }
    fallback.ok_or_else(|| "not found".to_string())
}

/// `DoClick(spec)`: a left press `{-1, 2}` and release `{-1, 1}` straight
/// to the target's slot 19; the note says which it took.
///
/// # Errors
///
/// The note of [`resolve_target`].
pub fn click(ui: &mut Ui, spec: &str) -> Result<String, String> {
    let target = resolve_target(ui, spec)?;
    let press = ui.mouse_event(target, MouseButton::LEFT_DOWN);
    let release = ui.mouse_event(
        target,
        MouseButton {
            index: 0,
            action: ButtonAction::Up,
        },
    );
    Ok(format!(
        "press={} release={}",
        u8::from(press),
        u8::from(release)
    ))
}

/// What a tree dump hands the script: the node lines and the root count.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TreeDump {
    pub roots: u32,
    /// One line per node: `dump`, then `NodeFields`.
    pub nodes: Vec<String>,
}

/// `WriteTree`'s node lines for dump `dump`.
#[must_use]
pub fn dump_nodes(ui: &Ui, dump: u32) -> TreeDump {
    let nodes = flatten_all(ui);
    let roots = nodes.iter().filter(|n| n.parent < 0).count();
    TreeDump {
        roots: u32::try_from(roots).unwrap_or(u32::MAX),
        nodes: nodes
            .iter()
            .enumerate()
            .map(|(i, node)| {
                let mut j = OracleJson::new();
                j.uint("dump", dump);
                node_fields(&mut j, ui, node, i32::try_from(i).unwrap_or(i32::MAX));
                j.finish()
            })
            .collect(),
    }
}

#[cfg(test)]
mod tests;
