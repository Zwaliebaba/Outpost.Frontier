//! The formations of `missions\formations.ini` (TU31's tail,
//! `Content/Encounters.cpp:138-209`).

use std::collections::BTreeMap;

use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

/// One slot (`FFormationSlot`). The loader gives every slot the identity
/// orientation, so it is not kept.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FormationSlot {
    /// `pos` or `pl_pos`.
    pub position: [f32; 3],
    /// True for `pl_pos`.
    pub player_slot: bool,
}

/// One `[Formation]` (`FFormation`). The loader leaves its own position at
/// zero and its orientation the identity, so neither is kept.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Formation {
    /// `CreateID(nickname)`.
    pub nickname: u32,
    /// The slots in file order.
    pub slots: Vec<FormationSlot>,
}

impl Formation {
    /// `CountPlayerSlots`.
    #[must_use]
    pub fn player_slots(&self) -> usize {
        self.slots.iter().filter(|s| s.player_slot).count()
    }

    /// `CountShipSlots`.
    #[must_use]
    pub fn ship_slots(&self) -> usize {
        self.slots.iter().filter(|s| !s.player_slot).count()
    }
}

/// `LoadFormation`: one `[Formation]` under its nickname id; an existing
/// key keeps its formation. The caller matched the header.
pub fn load_formation(table: &mut BTreeMap<u32, Formation>, reader: &mut IniReader) {
    let mut formation = Formation::default();
    let mut key = 0;
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            key = create_id(&reader.value_string());
            formation.nickname = key;
        } else if reader.is_value(b"pos") || reader.is_value(b"pl_pos") {
            let player_slot = reader.is_value(b"pl_pos");
            let position = [0, 1, 2].map(|i| reader.value_float(i) as f32);
            formation.slots.push(FormationSlot {
                position,
                player_slot,
            });
        }
    }
    table.entry(key).or_insert(formation);
}
