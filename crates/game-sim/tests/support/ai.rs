//! Lines of the AI suites shared by `oracle_ai.rs` and the `state_graph`
//! data dump (`CppSource/Oracle/oracle_ai.cpp`, `WriteStateGraphLines`).

use game_sim::ai::state_graph::{GraphType, StateGraphDb};

use super::render::{Line, json_text};

/// A float's bits, a NaN as one pattern (the harness's `F`).
pub fn f(value: f32) -> String {
    if value.is_nan() {
        "7fc00000".to_owned()
    } else {
        format!("{:08x}", value.to_bits())
    }
}

/// A double's bits, a NaN as one pattern (the harness's `D`).
pub fn d(value: f64) -> String {
    if value.is_nan() {
        "7ff8000000000000".to_owned()
    } else {
        format!("{:016x}", value.to_bits())
    }
}

/// Floats joined by commas.
pub fn fs(values: &[f32]) -> String {
    values.iter().map(|&v| f(v)).collect::<Vec<_>>().join(",")
}

/// The graphs, then the lookups of each graph's name.
pub fn state_graph_lines(db: &StateGraphDb) -> Vec<String> {
    let mut lines = Vec::new();
    for graph in &db.graphs {
        let weights: Vec<f32> = graph.weights.iter().flatten().copied().collect();
        lines.push(
            Line::new()
                .add("id", graph.id.to_string())
                .add("type", graph.kind.value().to_string())
                .add("name", json_text(&graph.name))
                .add("w", fs(&weights))
                .done(),
        );
    }
    for graph in &db.graphs {
        let lower = graph.name.to_ascii_lowercase();
        lines.push(
            Line::new()
                .add("find", json_text(&graph.name))
                .add(
                    "leader",
                    db.find(&graph.name, GraphType::Leader).to_string(),
                )
                .add(
                    "escort",
                    db.find(&graph.name, GraphType::Escort).to_string(),
                )
                .add(
                    "lower_leader",
                    db.find(&lower, GraphType::Leader).to_string(),
                )
                .add(
                    "by_id_leader",
                    db.find_by_id(graph.id, GraphType::Leader).to_string(),
                )
                .add(
                    "by_id_escort",
                    db.find_by_id(graph.id, GraphType::Escort).to_string(),
                )
                .done(),
        );
    }
    lines
}
