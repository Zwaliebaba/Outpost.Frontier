//! The NPC ship archetypes of `missions\npcships.ini` and
//! `missions\npcships_test.ini` (TU42, `Content/NpcShips.cpp:44-110`).

use std::collections::BTreeMap;

use game_sim::ai::state_graph::GraphType;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::msvcrt::atoi;

use super::{loadout_id, ship_id};
use crate::server::Cx;

/// One `[NPCShipArch]` (`FNpcShipArch`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NpcShipArch {
    /// `nickname`, `CreateID` of value 0 (also the key).
    pub nickname: u32,
    /// `ship_archetype`, `pub::GetShipID`.
    pub ship_archetype: u32,
    /// `loadout`, `pub::GetLoadoutID`.
    pub loadout: u32,
    /// `pilot`, `CreateID`.
    pub pilot: u32,
    /// `state_graph`, the leader graph of that name (-1 for none).
    pub state_graph: i32,
    /// `npc_class`: every value's id, each once, in file order.
    pub npc_classes: Vec<u32>,
    /// `level`: `atoi` from the value's first digit.
    pub level: i32,
}

impl Default for NpcShipArch {
    /// `FNpcShipArch::FNpcShipArch`.
    fn default() -> Self {
        Self {
            nickname: 0,
            ship_archetype: 0,
            loadout: 0,
            pilot: 0,
            state_graph: -1,
            npc_classes: Vec::new(),
            level: 0,
        }
    }
}

/// `LoadNpcShipArchs`: every `[NPCShipArch]` under its nickname id; a
/// nickname seen before keeps its first entry.
pub fn load_npc_ship_archs(
    cx: &Cx<'_>,
    table: &mut BTreeMap<u32, NpcShipArch>,
    reader: &mut IniReader,
) {
    while reader.read_header() {
        if !reader.is_header(b"NPCShipArch") {
            continue;
        }
        let mut arch = NpcShipArch::default();
        let mut key = 0;
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                key = create_id(&reader.value_string_at(0));
                arch.nickname = key;
            } else if reader.is_value(b"ship_archetype") {
                ship_id(&mut arch.ship_archetype, &reader.value_string_at(0));
            } else if reader.is_value(b"pilot") {
                arch.pilot = create_id(&reader.value_string_at(0));
            } else if reader.is_value(b"loadout") {
                loadout_id(&mut arch.loadout, &reader.value_string_at(0));
            } else if reader.is_value(b"state_graph") {
                arch.state_graph = cx
                    .world
                    .data()
                    .state_graphs
                    .find(&reader.value_string_at(0), GraphType::Leader);
            } else if reader.is_value(b"npc_class") {
                let mut index = 0;
                while !reader.is_value_empty(index) {
                    let class = create_id(&reader.value_string_at(index));
                    if !arch.npc_classes.contains(&class) {
                        arch.npc_classes.push(class);
                    }
                    index += 1;
                }
            } else if reader.is_value(b"level") {
                let text = reader.value_string_at(0);
                let start = text
                    .iter()
                    .position(u8::is_ascii_digit)
                    .unwrap_or(text.len());
                arch.level = atoi(&text[start..]);
            }
        }
        table.entry(key).or_insert(arch);
    }
}

/// `FindNpcShipArchByShip`: the first archetype, in key order, of a ship.
#[must_use]
pub fn find_by_ship(table: &BTreeMap<u32, NpcShipArch>, ship: u32) -> Option<&NpcShipArch> {
    table.values().find(|a| a.ship_archetype == ship)
}
