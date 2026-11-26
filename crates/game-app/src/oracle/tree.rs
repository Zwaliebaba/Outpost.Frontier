//! The interface tree's records (`oracle_mode.cpp`): `FlattenFrom`,
//! `FlattenAll` and `NodeFields`, shared by `tree.jsonl`'s dumps
//! (`WriteTree`) and `layout.jsonl`'s `winini` and `winnode` records
//! (`OracleOnWinControlIni`).
//!
//! A node's `rtti` is the C++ type name, which the port does not have; it
//! writes `null` there, and the comparisons leave the field out (decision
//! O53 (4)).

use neuron_client::ui::message::{ButtonAction, MouseButton};
use neuron_client::ui::tree::{NodeId, UNREGISTERED, Ui, VISIBLE};
use neuron_core::math::{Matrix, Vector};

use super::json::OracleJson;

/// `kMaxTreeNodes`.
pub const MAX_TREE_NODES: usize = 100_000;
/// `kMaxTreeDepth`.
pub const MAX_TREE_DEPTH: i32 = 64;

/// `FNode`: a node of a flattened tree.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TreeNode {
    pub id: NodeId,
    /// The parent's index in the flattened list, -1 for a root.
    pub parent: i32,
    pub depth: i32,
    /// Its own and every ancestor's visible bit.
    pub visible: bool,
}

fn index(i: usize) -> i32 {
    i32::try_from(i).unwrap_or(i32::MAX)
}

fn flatten(
    ui: &Ui,
    id: NodeId,
    parent: i32,
    depth: i32,
    parent_visible: bool,
    out: &mut Vec<TreeNode>,
) {
    let Some(node) = ui.node(id) else {
        return;
    };
    if out.len() >= MAX_TREE_NODES || depth > MAX_TREE_DEPTH {
        return;
    }
    let visible = parent_visible && node.is(VISIBLE);
    let me = index(out.len());
    out.push(TreeNode {
        id,
        parent,
        depth,
        visible,
    });
    for &child in node.children() {
        if out.len() >= MAX_TREE_NODES {
            return;
        }
        flatten(ui, child, me, depth + 1, visible, out);
    }
}

/// `FlattenFrom(root, -1, 0, true)`: the subtree in pre-order.
pub fn flatten_from(ui: &Ui, root: NodeId, out: &mut Vec<TreeNode>) {
    flatten(ui, root, -1, 0, true, out);
}

/// `FlattenAll`: every registered root (no parent, not unregistered) in
/// registry order, each flattened.
#[must_use]
pub fn flatten_all(ui: &Ui) -> Vec<TreeNode> {
    let roots: Vec<NodeId> = ui
        .registered()
        .filter(|&id| {
            ui.node(id)
                .is_some_and(|n| n.parent().is_none() && !n.is(UNREGISTERED))
        })
        .collect();
    let mut out = Vec::new();
    for root in roots {
        flatten_from(ui, root, &mut out);
    }
    out
}

fn vector(j: &mut OracleJson, name: &str, v: Vector) {
    j.begin_array(name)
        .item_float(v.x)
        .item_float(v.y)
        .item_float(v.z)
        .end_array();
}

fn matrix(j: &mut OracleJson, name: &str, m: &Matrix) {
    j.begin_array(name);
    for row in &m.m {
        for &value in row {
            j.item_float(value);
        }
    }
    j.end_array();
}

fn wide(j: &mut OracleJson, name: &str, text: Option<&[u16]>) {
    match text {
        Some(t) => j.wstr(name, t),
        None => j.null(name),
    };
}

/// `NodeFields(J, node, i)`: the fields after the caller's own keys.
pub fn node_fields(j: &mut OracleJson, ui: &Ui, node: &TreeNode, i: i32) {
    let Some(n) = ui.node(node.id) else {
        return;
    };
    j.int("i", i).int("p", node.parent).int("d", node.depth);
    match n.class_name {
        Some(c) => j.str("class", c),
        None => j.null("class"),
    };
    j.null("rtti")
        .str_bytes("name", n.name())
        .uint("flags", u32::from(n.flags & 0x0f))
        .bool("vis", node.visible)
        .uint("layer", n.draw_layer)
        .int("sub", n.draw_sub_layer)
        .int("hit", n.hit_priority);
    vector(j, "pos", n.position);
    matrix(j, "orient", &n.orientation);

    let widget = n.widget();
    if let Some(c) = widget.and_then(|w| w.win_control()) {
        j.begin_object("ctl");
        vector(j, "offset", c.offset);
        vector(j, "half", c.half_extents);
        vector(j, "hitoff", c.hit_offset);
        j.int("tooltip", c.tooltip_ids)
            .str_bytes("mesh", &c.mesh_path)
            .str_bytes("hp", &c.hardpoint)
            .uint("nz", u32::from(c.no_z_enable))
            .end_object();
    }
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
                node_fields(&mut j, ui, node, index(i));
                j.finish()
            })
            .collect(),
    }
}

/// `OracleOnWinControlIni(path, control)`: the `winini` head and a
/// `winnode` per node of the control's subtree, at `frame`.
#[must_use]
pub fn winini_lines(ui: &Ui, frame: u32, path: &[u8], root: NodeId) -> Vec<String> {
    let mut nodes = Vec::new();
    flatten_from(ui, root, &mut nodes);
    let mut lines = Vec::with_capacity(nodes.len() + 1);
    let mut j = OracleJson::new();
    j.uint("frame", frame)
        .str("rec", "winini")
        .str_bytes("path", path)
        .uint("nodes", u32::try_from(nodes.len()).unwrap_or(u32::MAX));
    lines.push(j.finish());
    for (i, node) in nodes.iter().enumerate() {
        let mut j = OracleJson::new();
        j.uint("frame", frame)
            .str("rec", "winnode")
            .str_bytes("path", path);
        node_fields(&mut j, ui, node, index(i));
        lines.push(j.finish());
    }
    lines
}

#[cfg(test)]
mod tests;
