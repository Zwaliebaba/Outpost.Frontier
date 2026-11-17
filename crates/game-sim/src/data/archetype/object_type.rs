//! `DecodeType`: the `OBJECT_TYPE` names (`common_archetype.cpp`).

use neuron_core::hash::eq_ignore_case;

use crate::data::{Reports, lossy};

/// The 29 names and their bits, in the C++ table's order.
pub const OBJECT_TYPES: [(&[u8], u32); 29] = [
    (b"NONE", 0),
    (b"MOON", 1),
    (b"PLANET", 2),
    (b"SUN", 4),
    (b"ASTEROID", 0x2000_0000),
    (b"BLACKHOLE", 8),
    (b"SATELLITE", 0x10),
    (b"DOCKING_RING", 0x20),
    (b"JUMP_GATE", 0x40),
    (b"JUMP_HOLE", 0x800),
    (b"WEAPONS_PLATFORM", 0x1000),
    (b"DESTROYABLE_DEPOT", 0x2000),
    (b"NON_TARGETABLE", 0x4000),
    (b"MISSION_SATELLITE", 0x8000),
    (b"AIRLOCK_GATE", 0x400),
    (b"TRADELANE_RING", 0x80),
    (b"STATION", 0x100),
    (b"WAYPOINT", 0x200),
    (b"FIGHTER", 0x1_0000),
    (b"FREIGHTER", 0x2_0000),
    (b"GUNBOAT", 0x4_0000),
    (b"CRUISER", 0x8_0000),
    (b"TRANSPORT", 0x10_0000),
    (b"CAPITAL", 0x20_0000),
    (b"MINING", 0x40_0000),
    (b"GUIDED", 0x100_0000),
    (b"MINE", 0x400_0000),
    (b"LOOT", 0x1000_0000),
    (b"BULLET", 0x200_0000),
];

/// `DecodeType(name)`: the bits of the first entry `stricmp` matches; an
/// unknown name is reported and gives 0.
pub fn decode_type(name: &[u8], reports: &mut Reports) -> u32 {
    if let Some((_, bits)) = OBJECT_TYPES.iter().find(|(n, _)| eq_ignore_case(name, n)) {
        return *bits;
    }
    reports.push(
        super::SOURCE,
        0xe1,
        format!("*** WARNING: unknown OBJECT_TYPE = '{}'", lossy(name)),
    );
    0
}
