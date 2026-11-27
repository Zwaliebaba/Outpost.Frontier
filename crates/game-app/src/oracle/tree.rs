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
//! comparison: the port writes null. The `text`, `tw` and `tc` fields of
//! a `TextWin` and a `TextControl` are not written yet; they come with the
//! first screen whose tree holds one.

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
}
