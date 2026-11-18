//! Saved table layouts (`FUiFastTableEdges::SaveToRegistry` and
//! `LoadFromRegistry` in `Client/ui_fast_table.cpp`; work package P4.1,
//! decision O27): the column and row edges the player dragged on the
//! character and server tables.
//!
//! The C++ writes them as `REG_BINARY` arrays of `float` (`Columns`,
//! `Rows`) under the keys `CharacterTable` and `ServerTable` of the
//! client's registry key. The port keeps them in `tables.toml`, one table
//! per layout; [`TableLayouts::from_registry`] reads the C++'s values once.

use std::collections::BTreeMap;

use neuron_client::user_state::RegKey;

/// The state file of the table layouts.
pub const FILE_NAME: &str = "tables.toml";

/// The format version of `tables.toml`.
pub const VERSION: i64 = 1;

/// The tables whose layouts are saved: the character and server lists.
pub const TABLES: [&str; 2] = ["CharacterTable", "ServerTable"];

/// The edges of one table.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Edges {
    pub columns: Vec<f32>,
    pub rows: Vec<f32>,
}

/// The saved layouts by table name.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TableLayouts {
    pub tables: BTreeMap<String, Edges>,
}

/// `LoadEdgeVector`: a `REG_BINARY` value as little-endian `float`s, the
/// bytes past the last whole one dropped. The C++ treats a missing value
/// as size -1 and resizes the vector to 2^30 entries; here a missing value
/// keeps no edges (D270).
fn floats(key: &RegKey, name: &str) -> Vec<f32> {
    key.binary(name)
        .map(|bytes| {
            bytes
                .as_chunks::<4>()
                .0
                .iter()
                .map(|b| f32::from_le_bytes(*b))
                .collect()
        })
        .unwrap_or_default()
}

fn to_values(edges: &[f32]) -> toml::Value {
    toml::Value::Array(
        edges
            .iter()
            .map(|&e| toml::Value::Float(f64::from(e)))
            .collect(),
    )
}

fn from_values(value: Option<&toml::Value>) -> Option<Vec<f32>> {
    match value {
        None => Some(Vec::new()),
        Some(toml::Value::Array(items)) => items
            .iter()
            .map(|v| v.as_float().map(|f| f as f32))
            .collect(),
        Some(_) => None,
    }
}

impl TableLayouts {
    /// The layouts the C++ saved under the client's registry key.
    #[must_use]
    pub fn from_registry(root: &RegKey) -> TableLayouts {
        let mut layouts = TableLayouts::default();
        for name in TABLES {
            if let Some(key) = root.key(name) {
                layouts.tables.insert(
                    name.to_owned(),
                    Edges {
                        columns: floats(key, "Columns"),
                        rows: floats(key, "Rows"),
                    },
                );
            }
        }
        layouts
    }

    /// The layouts as the TOML of `tables.toml`; every `f32` is written as
    /// the `f64` of the same value, so it reads back exactly.
    #[must_use]
    pub fn to_toml(&self) -> toml::Table {
        let mut table = toml::Table::new();
        table.insert("version".into(), toml::Value::Integer(VERSION));
        for (name, edges) in &self.tables {
            let mut t = toml::Table::new();
            t.insert("columns".into(), to_values(&edges.columns));
            t.insert("rows".into(), to_values(&edges.rows));
            table.insert(name.clone(), toml::Value::Table(t));
        }
        table
    }

    /// Reads a `tables.toml` table; `None` for another version or a value
    /// of the wrong type.
    #[must_use]
    pub fn from_toml(table: &toml::Table) -> Option<TableLayouts> {
        if table.get("version").and_then(toml::Value::as_integer) != Some(VERSION) {
            return None;
        }
        let mut layouts = TableLayouts::default();
        for (name, value) in table {
            if name == "version" {
                continue;
            }
            let t = value.as_table()?;
            layouts.tables.insert(
                name.clone(),
                Edges {
                    columns: from_values(t.get("columns"))?,
                    rows: from_values(t.get("rows"))?,
                },
            );
        }
        Some(layouts)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_client::user_state::RegValue;

    fn bytes(values: &[f32]) -> Vec<u8> {
        values.iter().flat_map(|v| v.to_le_bytes()).collect()
    }

    #[test]
    fn the_registry_import_reads_both_tables() {
        let mut character = RegKey::default();
        let mut cols = bytes(&[0.1, 120.5, 300.0]);
        // A stray byte past the last whole float is dropped.
        cols.push(7);
        character
            .values
            .insert("Columns".into(), RegValue::Binary(cols));
        character
            .values
            .insert("Rows".into(), RegValue::Binary(bytes(&[16.0])));
        let mut server = RegKey::default();
        server
            .values
            .insert("Columns".into(), RegValue::Binary(bytes(&[42.0])));
        let mut root = RegKey::default();
        root.keys.insert("CharacterTable".into(), character);
        root.keys.insert("ServerTable".into(), server);
        let l = TableLayouts::from_registry(&root);
        assert_eq!(l.tables["CharacterTable"].columns, vec![0.1, 120.5, 300.0]);
        assert_eq!(l.tables["CharacterTable"].rows, vec![16.0]);
        assert_eq!(l.tables["ServerTable"].columns, vec![42.0]);
        assert!(l.tables["ServerTable"].rows.is_empty());
    }

    #[test]
    fn floats_survive_the_state_file_exactly() {
        let mut l = TableLayouts::default();
        l.tables.insert(
            "CharacterTable".into(),
            Edges {
                columns: vec![0.1, 1.0e-30, 123_456.79, -0.0],
                rows: vec![],
            },
        );
        let text = toml::to_string(&l.to_toml()).unwrap();
        let back = TableLayouts::from_toml(&text.parse().unwrap()).unwrap();
        let bits = |e: &Edges| e.columns.iter().map(|f| f.to_bits()).collect::<Vec<_>>();
        assert_eq!(
            bits(&back.tables["CharacterTable"]),
            bits(&l.tables["CharacterTable"])
        );
        assert!(TableLayouts::from_toml(&"version = 9".parse().unwrap()).is_none());
    }
}
