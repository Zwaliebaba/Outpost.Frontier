//! Content's system records (`FSystemInfo`, TU58 `Content/SystemInfo.cpp`):
//! work package P3.4d, decision record 0008 points 5 and 10.
//!
//! `GSystemInfos` (`SystemInfo.cpp:32`) is a map by system id in
//! [`ContentState`](super::ContentState) ([`SystemInfos`]). GameSupport
//! builds it on every `LoadGame` ([`load_dyn_sys_info`]), so the records
//! exist before any system loads, in both modes. A record is loaded by
//! reference count ([`load_from_reader`], [`unload`]):
//!
//! - the first load reads the system file's header loop in one pass: the
//!   `[System]`'s `local_faction`, the `[EncounterParameters]` files (P3.4c,
//!   [`super::encounter_params`]), the lane rings and the jump gates and
//!   holes of the `[Object]`s; then the zones (P3.4c, [`super::zones`]), the
//!   patrol paths (P3.4c, [`super::patrol_paths`]) and the trade lanes from
//!   the rings' `prev_ring` and `next_ring` ([`build_trade_lanes`]);
//! - the full load on `SystemLoaded` ([`load_full`], SpaceSupport's case)
//!   reads the solar NPCs first ([`super::solar_npcs`]: the named objects
//!   with a `reputation` take their faction now, before any player's
//!   creates, which closes fact F17), then loads the record with its
//!   encounters, then rebuilds the trade lanes from the live rings with
//!   their attack zones ([`rebuild_trade_lanes`]).
//!
//! Where the C++ holds a pointer the record holds an index: a zone's patrol
//! path, a lane's attack zones and a ring's lane. The players of a system
//! are their ids. The NPC ships (`SpaceObjs`), the mission objects' run
//! time, the zones' population validation (`Validate`, which draws
//! `rand()`) and the solar NPCs' personality states are P3.5's and P3.6's
//! (D802, D803).
//!
//! A handler that loads a record takes it out of the table for calls that
//! cannot dispatch, as record 0008 point 1 allows ([`with_system_taken`]).

// The C++'s `!(x > 0.0f)` tests send NaN to the failing side; the port
// spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use std::collections::BTreeMap;

use neuron_core::hash::{c_str, stricmp};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::encounter_params::{EncounterParameters, SystemEncounterParameters};
use super::patrol_paths::{PatrolPath, build_patrol_paths};
use super::population;
use super::solar_npcs::{SolarNpc, read_solar_npc};
use super::tables::reputation_group;
use super::zones::{SystemZone, enumerate_system_zones};
use super::{NoDispatch, data_source};
use crate::api;
use crate::server::Cx;

/// One ring of a trade lane (`FTradeLaneRing`, 0x44 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct TradeLaneRing {
    /// The ring.
    pub object: u32,
    /// Its position.
    pub position: Vector,
    /// Its rotation (the transform's translation is always zero).
    pub rotation: Matrix,
    /// The ring's `reputation` from the file; -1 for a live ring.
    pub reputation: u32,
}

/// The zones around one ring whose population may attack the lane
/// (`FTradeLaneRingCandidates`).
#[derive(Clone, Debug, PartialEq)]
pub struct RingCandidates {
    /// The ring's index in the lane.
    pub ring_index: i32,
    /// `{zone, its tradelane_attack}`: the zone by its index in the
    /// record's zones.
    pub candidates: Vec<(usize, f32)>,
    /// The largest weight.
    pub max_weight: f32,
}

/// A trade lane (`FTradeLane`, 0x98 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct TradeLane {
    /// The rings, `prev_ring` to `next_ring`.
    pub rings: Vec<TradeLaneRing>,
    /// The ring without a `prev_ring`; zero when none is (the C++ leaves it
    /// unset, D801).
    pub start: Vector,
    /// The ring without a `next_ring`.
    pub end: Vector,
    /// The first ring's rotation.
    pub rotation: Matrix,
    /// The ships travelling towards the last ring (`+0x58`).
    pub forward_ships: Vec<u32>,
    /// Towards the first ring (`+0x68`).
    pub backward_ships: Vec<u32>,
    /// The game time a ship last left `forward_ships`; -1.
    pub last_forward_leave: f32,
    /// The game time a ship last left `backward_ships`; -1.
    pub last_backward_leave: f32,
    /// The largest weight of the rings' attack zones.
    pub max_weight: f32,
    /// The game time of the last attack roll (P3.6d); 0.
    pub last_attack_time: f32,
    /// The rings with attack zones (`+0x88`).
    pub ring_candidates: Vec<RingCandidates>,
}

impl Default for TradeLane {
    /// `FTradeLane::FTradeLane` (`:1228-1234`).
    fn default() -> Self {
        Self {
            rings: Vec::new(),
            start: Vector::ZERO,
            end: Vector::ZERO,
            rotation: Matrix::IDENTITY,
            forward_ships: Vec::new(),
            backward_ships: Vec::new(),
            last_forward_leave: -1.0,
            last_backward_leave: -1.0,
            max_weight: 0.0,
            last_attack_time: 0.0,
            ring_candidates: Vec::new(),
        }
    }
}

/// A jump gate or hole of the system (`FSystemJump`, 0x1c bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SystemJump {
    /// `pub::GetNicknameId(nickname)`.
    pub object: u32,
    /// 1 a jump gate, 2 a jump hole.
    pub kind: i32,
    /// `pos`.
    pub position: Vector,
    /// `goto`'s system.
    pub goto_system: u32,
    /// `goto`'s object.
    pub goto_object: u32,
}

/// A `Trade_Lane_Ring` `[Object]` as the loader keeps it
/// (`FTradeLaneRingInfo`).
#[derive(Clone, Debug, PartialEq)]
struct RingInfo {
    object: u32,
    position: Vector,
    rotation: Matrix,
    prev: u32,
    next: u32,
    reputation: u32,
}

/// `FSystemInfo` (0x124 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct SystemInfo {
    /// `SystemId` (`+0x48`).
    pub system: u32,
    /// `LoadCount` (`+0x4c`).
    pub load_count: i32,
    /// The patrol paths (`+0x50`).
    pub paths: Vec<PatrolPath>,
    /// The zones (`+0x60`).
    pub zones: Vec<SystemZone>,
    /// `ZoneById` (`+0x70`): a zone id to its index (a repeated id to the
    /// later zone).
    pub zone_by_id: BTreeMap<u32, usize>,
    /// `[System]` `local_faction` (`+0x84`).
    pub local_faction: u32,
    /// The solar NPCs (`+0x88`), only filled by the full load.
    pub solar_npcs: Vec<SolarNpc>,
    /// The objects the mission scripts made here (`+0x98`, P3.5).
    pub mission_objects: Vec<u32>,
    /// `+0xa8`: objects a random mission flagged (P3.7).
    pub flagged_objects: Vec<u32>,
    /// The players in the system (`+0xb8`), by id, added once each.
    pub players: Vec<u32>,
    /// The trade lanes (`+0xe8`).
    pub trade_lanes: Vec<TradeLane>,
    /// `TradeLaneByRing` (`+0xf8`): a ring to its lane's index. The full
    /// load adds to it and nothing clears it.
    pub lane_by_ring: BTreeMap<u32, usize>,
    /// The jump gates and holes (`+0x10c`).
    pub jumps: Vec<SystemJump>,
    /// `[SystemInfo]` `rpop_solar_detection` (`+0x11c`, true).
    pub rpop_solar_detection: bool,
}

/// `GSystemInfos`: the records by system id.
pub type SystemInfos = BTreeMap<u32, SystemInfo>;

impl SystemInfo {
    /// `FSystemInfo(&SystemId)` (`:36-39`).
    #[must_use]
    pub fn new(system: u32) -> Self {
        Self {
            system,
            load_count: 0,
            paths: Vec::new(),
            zones: Vec::new(),
            zone_by_id: BTreeMap::new(),
            local_faction: 0,
            solar_npcs: Vec::new(),
            mission_objects: Vec::new(),
            flagged_objects: Vec::new(),
            players: Vec::new(),
            trade_lanes: Vec::new(),
            lane_by_ring: BTreeMap::new(),
            jumps: Vec::new(),
            rpop_solar_detection: true,
        }
    }

    /// `FindZone(ZoneId)`: the zone's index.
    #[must_use]
    pub fn find_zone(&self, zone: u32) -> Option<usize> {
        self.zone_by_id.get(&zone).copied()
    }

    /// `FindTradeLaneByRing(&RingId, &Lane)`: the lane's index.
    #[must_use]
    pub fn lane_of_ring(&self, ring: u32) -> Option<usize> {
        self.lane_by_ring
            .get(&ring)
            .copied()
            .filter(|&l| l < self.trade_lanes.len())
    }

    /// `GetTradeLaneRingsByRing(&RingId, &OutRings)`: the rings of the
    /// ring's lane, `None` for an unknown ring.
    #[must_use]
    pub fn lane_rings_of_ring(&self, ring: u32) -> Option<&[TradeLaneRing]> {
        self.lane_of_ring(ring)
            .map(|l| self.trade_lanes[l].rings.as_slice())
    }

    /// `AddPlayer(Player)` (`:401-409`): appended unless listed.
    pub fn add_player(&mut self, player: u32) {
        if !self.players.contains(&player) {
            self.players.push(player);
        }
    }

    /// The list part of `RemovePlayer(Player)` (`:412-425`): the first
    /// entry goes. In single player the C++ then destroys the system's NPC
    /// ships and mission objects when no player there has a ship
    /// (`DestroyObjectsIfUnoccupied`), which P3.5 and P3.6 bring.
    pub fn remove_player(&mut self, player: u32) {
        if let Some(index) = self.players.iter().position(|&p| p == player) {
            self.players.remove(index);
        }
    }

    /// `AddTradeLaneShip(&RingId, &Direction, &ShipId)` (`:1670-1680`): the
    /// ship joins the forward ships (direction 1) or the backward ships of
    /// the ring's lane. An unknown ring adds nothing, where the C++ reads
    /// the map's head node (D801).
    pub fn add_trade_lane_ship(&mut self, ring: u32, direction: i32, ship: u32) {
        let Some(lane) = self.lane_of_ring(ring) else {
            return;
        };
        let lane = &mut self.trade_lanes[lane];
        if direction == 1 {
            lane.forward_ships.push(ship);
        } else {
            lane.backward_ships.push(ship);
        }
    }

    /// `RemoveTradeLaneShip(&ShipId, &RingId)` (`:1683-1717`): the ship
    /// leaves the ring's lane (every lane from the first when the ring is
    /// 0): the first match of the forward ships, and until a lane matched
    /// the first of the backward ships, each stamping its leave time with
    /// the game time (a `float`). An unknown ring removes nothing, where the
    /// C++ reads the map's head node (D801).
    pub fn remove_trade_lane_ship(&mut self, ship: u32, ring: u32, game_time: f64) {
        let first = if ring == 0 {
            0
        } else {
            match self.lane_of_ring(ring) {
                Some(l) => l,
                None => return,
            }
        };
        let now = game_time as f32;
        let mut removed = false;
        for lane in &mut self.trade_lanes[first..] {
            if let Some(i) = lane.forward_ships.iter().position(|&s| s == ship) {
                lane.forward_ships.remove(i);
                lane.last_forward_leave = now;
                removed = true;
            }
            if !removed && let Some(i) = lane.backward_ships.iter().position(|&s| s == ship) {
                lane.backward_ships.remove(i);
                lane.last_backward_leave = now;
                removed = true;
            }
            if ring != 0 {
                break;
            }
        }
    }
}

/// Takes system `system`'s record out of the table, runs `call`, which must
/// not dispatch, and puts it back (record 0008 point 1); `None`, without a
/// call, when there is no record.
pub fn with_system_taken<R>(
    cx: &mut Cx<'_>,
    system: u32,
    call: impl FnOnce(&mut Cx<'_>, &mut SystemInfo) -> R,
) -> Option<R> {
    let mut record = cx.logic.content.state.systems.remove(&system)?;
    let take = NoDispatch::begin(&cx.logic.content);
    let result = call(cx, &mut record);
    take.end(&cx.logic.content);
    cx.logic.content.state.systems.insert(system, record);
    Some(result)
}

/// `FSystemInfo::Load(bLoadEncounters)`: [`load_from_reader`] over the
/// system's own file.
pub fn load(cx: &mut Cx<'_>, system: u32, load_encounters: bool) {
    with_system_taken(cx, system, |cx, record| {
        load_from_reader(cx, record, None, load_encounters);
    });
}

/// `FSystemInfo::Unload` (`:303-310`): the last release clears the jumps
/// and the zones. The paths, the lanes and the solar NPCs stay.
pub fn unload(record: &mut SystemInfo) {
    record.load_count = record.load_count.wrapping_sub(1);
    if record.load_count != 0 {
        return;
    }
    record.jumps.clear();
    record.zone_by_id.clear();
    record.zones.clear();
}

/// `FSystemInfo::LoadFromReader(Reader, bLoadEncounters)` (`:48-300`):
/// counted; only the load that takes the count from 0 reads. Without a
/// reader the system's own file is read (`pub::BuildSystemReader`; a
/// missing file reads as empty).
///
/// After the zones and paths, the C++ validates the population of each
/// zone with population keys (`FZonePopulation::Validate`, the encounters'
/// first `Refresh`, which draws `rand()`); that is the populators' run
/// time and P3.6d's (D802).
pub fn load_from_reader(
    cx: &mut Cx<'_>,
    record: &mut SystemInfo,
    reader: Option<&mut IniReader>,
    load_encounters: bool,
) {
    let previous = record.load_count;
    record.load_count = previous.wrapping_add(1);
    if previous != 0 {
        return;
    }
    let source = data_source(cx);
    let mut own = IniReader::default();
    let reader = match reader {
        Some(r) => r,
        None => {
            if let Ok(r) = api::misc::build_system_reader(cx, source.as_ref(), record.system) {
                own = r;
            }
            &mut own
        }
    };
    record.local_faction = 0;
    let mut rings: BTreeMap<u32, RingInfo> = BTreeMap::new();
    let mut parameters = SystemEncounterParameters::new();
    while reader.read_header() {
        if reader.is_header(b"System") || reader.is_header(b"SystemInfo") {
            while reader.read_value() {
                if reader.is_value(b"local_faction") {
                    record.local_faction = reputation_group(cx, &reader.value_string());
                }
            }
        } else if load_encounters && reader.is_header(b"EncounterParameters") {
            reader.read_value();
            let id = neuron_core::id::create_id(c_str(&reader.value_string()));
            reader.read_value();
            let mut file = c_str(&reader.value_string()).to_vec();
            file.truncate(0x103);
            if let Some(mut file_reader) = game_sim::data::open_ini(source.as_ref(), &file) {
                parameters.insert(
                    id,
                    EncounterParameters::new(&mut file_reader, id, record.system),
                );
            }
        } else if reader.is_header(b"Object") {
            read_object(cx, record, reader, &mut rings);
        }
    }
    let mut zones = enumerate_system_zones(
        cx,
        source.as_ref(),
        record.system,
        load_encounters,
        &parameters,
    );
    drop(parameters);
    record.zones = zones.zones;
    record.zone_by_id.clear();
    for (index, zone) in record.zones.iter().enumerate() {
        record.zone_by_id.insert(zone.zone_id, index);
    }
    record.paths = build_patrol_paths(cx, record.system, &mut record.zones, &mut zones.path_labels);
    record.trade_lanes = build_trade_lanes(rings);
}

/// One `[Object]` of the header loop (`:115-261`): a lane ring goes into
/// `rings` by its id (a unique insert), a jump gate or hole into the
/// record's jumps. Any other `archetype` ends the section's values.
fn read_object(
    cx: &mut Cx<'_>,
    record: &mut SystemInfo,
    reader: &mut IniReader,
    rings: &mut BTreeMap<u32, RingInfo>,
) {
    let mut object = 0u32;
    let mut position = Vector::ZERO;
    let mut rotation = Matrix::IDENTITY;
    let mut prev = 0u32;
    let mut next = 0u32;
    // Left unset by the C++ when there is no `reputation` (D801).
    let mut reputation = 0u32;
    let (mut ring, mut gate, mut hole) = (false, false, false);
    let mut goto_system = 0u32;
    let mut goto_object = 0u32;
    if !reader.read_value() {
        return;
    }
    loop {
        if reader.is_value(b"nickname") {
            object = api::misc::nickname_id(c_str(&reader.value_string_at(0)));
        } else if reader.is_value(b"pos") {
            let z = reader.value_float(2) as f32;
            let y = reader.value_float(1) as f32;
            let x = reader.value_float(0) as f32;
            position = Vector::new(x, y, z);
        } else if reader.is_value(b"rotate") {
            let z = reader.value_float(2) as f32;
            let y = reader.value_float(1) as f32;
            let x = reader.value_float(0) as f32;
            rotation = game_sim::data::zone::euler_matrix(Vector::new(x, y, z));
        } else if reader.is_value(b"archetype") {
            let value = reader.value_string_at(0);
            let value = c_str(&value);
            if stricmp(b"Trade_Lane_Ring", value).is_eq() {
                ring = true;
            } else if stricmp(b"jumpgate", value).is_eq() {
                gate = true;
            } else if stricmp(b"jumphole", value).is_eq() {
                hole = true;
            } else {
                break;
            }
        } else if reader.is_value(b"prev_ring") {
            prev = api::misc::nickname_id(c_str(&reader.value_string_at(0)));
        } else if reader.is_value(b"next_ring") {
            next = api::misc::nickname_id(c_str(&reader.value_string_at(0)));
        } else if reader.is_value(b"goto") {
            if let Ok(id) = api::misc::system_id(cx, c_str(&reader.value_string_at(0))) {
                goto_system = id;
            }
            goto_object = api::misc::nickname_id(c_str(&reader.value_string_at(1)));
        } else if reader.is_value(b"reputation") {
            reputation = reputation_group(cx, &reader.value_string_at(0));
        }
        if !reader.read_value() {
            break;
        }
    }
    if ring {
        rings.entry(object).or_insert(RingInfo {
            object,
            position,
            rotation,
            prev,
            next,
            reputation,
        });
    }
    if gate || hole {
        record.jumps.push(SystemJump {
            object,
            kind: if gate { 1 } else { 2 },
            position,
            goto_system,
            goto_object,
        });
    }
}

/// The ring record of a loader's ring (`MakeTradeLaneRing`).
fn ring_of(info: &RingInfo) -> TradeLaneRing {
    TradeLaneRing {
        object: info.object,
        position: info.position,
        rotation: info.rotation,
        reputation: info.reputation,
    }
}

/// `FSystemInfo::BuildTradeLanes(Rings)` (`:1823-1891`): while rings are
/// left, the first by id starts a lane; its `prev_ring` chain goes in
/// front and its `next_ring` chain behind, each ring leaving the map. A
/// ring without a `prev_ring` (low 30 bits 0) gives the lane's start, one
/// without a `next_ring` its end.
fn build_trade_lanes(mut rings: BTreeMap<u32, RingInfo>) -> Vec<TradeLane> {
    const MASK: u32 = 0x3fff_ffff;
    let mut lanes = Vec::new();
    while let Some((_, first)) = rings.pop_first() {
        let mut lane = TradeLane::default();
        lane.rings.push(ring_of(&first));
        lane.rotation = first.rotation;
        if first.prev & MASK == 0 {
            lane.start = first.position;
        }
        if first.next & MASK == 0 {
            lane.end = first.position;
        }
        let mut last = first.object;
        let mut prev_key = first.prev;
        while let Some(info) = rings.remove(&prev_key) {
            let before = lane
                .rings
                .iter()
                .position(|r| r.object == last)
                .unwrap_or(lane.rings.len());
            lane.rings.insert(before, ring_of(&info));
            if info.prev & MASK == 0 {
                lane.start = info.position;
            }
            last = prev_key;
            prev_key = info.prev;
        }
        let mut last = first.object;
        let mut next_key = first.next;
        while let Some(info) = rings.remove(&next_key) {
            let after = lane
                .rings
                .iter()
                .position(|r| r.object == last)
                .map_or(lane.rings.len(), |i| i + 1);
            lane.rings.insert(after, ring_of(&info));
            if info.next & MASK == 0 {
                lane.end = info.position;
            }
            last = next_key;
            next_key = info.next;
        }
        lanes.push(lane);
    }
    lanes
}

/// `FSystemInfo::LoadSolarNpcs(Reader)` (`:313-364`): each `[Object]`'s
/// solar NPC appended ([`read_solar_npc`]), `[SystemInfo]`'s
/// `rpop_solar_detection`; the reader is reset at the end.
pub fn load_solar_npcs(cx: &mut Cx<'_>, record: &mut SystemInfo, reader: &mut IniReader) {
    while reader.read_header() {
        if reader.is_header(b"Object") {
            if let Some(npc) = read_solar_npc(cx, reader) {
                record.solar_npcs.push(npc);
            }
        } else if reader.is_header(b"SystemInfo") {
            while reader.read_value() {
                if reader.is_value(b"rpop_solar_detection") {
                    record.rpop_solar_detection = reader.value_bool(0);
                }
            }
        }
    }
    reader.reset();
}

/// `FSystemInfo::LoadFull` (`:367-380`), SpaceSupport's `SystemLoaded`:
/// the system file's solar NPCs, in single player SpacePop's solar
/// detection from the record, the load with encounters over the same
/// reader, then the trade lanes from the live rings. The solar NPCs'
/// personality states (`SubmitPersonalityState(true)`) are P3.6c's (D803).
/// Nothing for a system without a record, where the C++ dereferences null
/// (D801).
pub fn load_full(cx: &mut Cx<'_>, system: u32) {
    with_system_taken(cx, system, |cx, record| {
        let source = data_source(cx);
        let mut reader =
            api::misc::build_system_reader(cx, source.as_ref(), system).unwrap_or_default();
        load_solar_npcs(cx, record, &mut reader);
        if api::misc::single_player(cx) {
            population::set_solar_detection(cx, record.rpop_solar_detection);
        }
        reader.reset();
        load_from_reader(cx, record, Some(&mut reader), true);
        rebuild_trade_lanes(cx, record);
    });
}

/// `FSystemInfo::UnloadFull` (`:383-398`), SpaceSupport's
/// `SystemUnloaded`: the solar NPCs, mission objects and flagged objects
/// emptied (the NPCs' reputation handles go; their vibes keep the
/// faction), in single player SpacePop's solar detection on again, then
/// [`unload`]. The C++ first destroys the NPC ships and mission objects
/// when no player there has a ship, which P3.5 and P3.6 bring. Nothing for
/// a system without a record (D801).
pub fn unload_full(cx: &mut Cx<'_>, system: u32) {
    let Some(record) = cx.logic.content.state.systems.get_mut(&system) else {
        return;
    };
    record.solar_npcs.clear();
    record.mission_objects.clear();
    record.flagged_objects.clear();
    if api::misc::single_player(cx) {
        population::set_solar_detection(cx, true);
    }
    if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
        unload(record);
    }
}

/// The game id of a live ring and its placement (`MakeLiveRing`): its
/// location (zero and the identity for an object without one), reputation
/// -1.
fn live_ring(cx: &Cx<'_>, ring: u32) -> TradeLaneRing {
    let (position, rotation) =
        api::space_obj::location(cx, ring).unwrap_or((Vector::ZERO, Matrix::IDENTITY));
    TradeLaneRing {
        object: ring,
        position,
        rotation,
        reputation: u32::MAX,
    }
}

/// `FSystemInfo::RebuildTradeLanes` (`:1296-1421`): the lanes again from
/// the system's live rings (`ScanObjects` of type `0x80`, at most 128,
/// chained by `GetTradelaneNextAndPrev`, each ring taken off the scan list
/// as the C++ does), each lane's start and rotation from its first ring
/// and end from its last. Then every ring enters the ring map, and gets the
/// attack zones of its position against the ring's affiliation
/// ([`collect_ring_candidates`]); the lane's weight is the largest.
pub fn rebuild_trade_lanes(cx: &mut Cx<'_>, record: &mut SystemInfo) -> usize {
    record.trade_lanes.clear();
    let mut scan = api::system::scan_objects(cx, record.system, 0x80, Vector::ZERO, -1.0, 0x80)
        .unwrap_or_default();
    let count = scan.len();
    let links = |cx: &Cx<'_>, ring: u32| {
        api::space_obj::tradelane_next_and_prev(cx, ring).unwrap_or((0, 0))
    };
    for index in 0..count {
        if scan[index] == 0 {
            continue;
        }
        let mut chain = vec![live_ring(cx, scan[index])];
        let (mut next, _) = links(cx, scan[index]);
        while next != 0 {
            chain.push(live_ring(cx, next));
            if let Some(slot) = (index..count).find(|&s| scan[s] == next) {
                scan[slot] = 0;
            } else {
                for below in (0..index).rev() {
                    if scan[below] == next {
                        scan[below] = 0;
                    }
                }
            }
            next = links(cx, next).0;
        }
        let (_, mut prev) = links(cx, scan[index]);
        while prev != 0 {
            chain.insert(0, live_ring(cx, prev));
            for below in (0..index).rev() {
                if scan[below] == prev {
                    scan[below] = 0;
                }
            }
            if let Some(slot) = (index..count).find(|&s| scan[s] == prev) {
                scan[slot] = 0;
            }
            prev = links(cx, prev).1;
        }
        let lane = TradeLane {
            start: chain[0].position,
            rotation: chain[0].rotation,
            end: chain[chain.len() - 1].position,
            rings: chain,
            ..TradeLane::default()
        };
        record.trade_lanes.push(lane);
    }
    for lane_index in 0..record.trade_lanes.len() {
        let mut lane_max = 0.0_f32;
        let mut found_rings = Vec::new();
        for ring_index in 0..record.trade_lanes[lane_index].rings.len() {
            let ring = record.trade_lanes[lane_index].rings[ring_index].clone();
            record.lane_by_ring.insert(ring.object, lane_index);
            let rep = api::space_obj::solar_rep(cx, ring.object).unwrap_or(-1);
            let affiliation = api::reputation::affiliation(cx, rep).unwrap_or(u32::MAX);
            let candidates = collect_ring_candidates(cx, record, ring.position, affiliation);
            let mut ring_max = 0.0_f32;
            for &(_, weight) in &candidates {
                if ring_max < weight {
                    ring_max = weight;
                }
            }
            if ring_max > lane_max {
                lane_max = ring_max;
            }
            if !candidates.is_empty() {
                found_rings.push(RingCandidates {
                    ring_index: ring_index as i32,
                    candidates,
                    max_weight: ring_max,
                });
            }
        }
        let lane = &mut record.trade_lanes[lane_index];
        lane.ring_candidates.extend(found_rings);
        lane.max_weight = lane_max;
    }
    record.trade_lanes.len()
}

/// `FSystemInfo::CollectRingCandidates(Ring, Position, Affiliation, Out)`
/// (`:1424-1468`): SpacePop's zones at the ring's position under the
/// identity orientation ([`population::collect_space_pop_zones`], which may
/// draw `rand()`); of the zones at the ring, then the near ones, those with
/// a patrol path whose population is partly hostile to the affiliation and
/// whose `tradelane_attack` is above 0, with that weight.
fn collect_ring_candidates(
    cx: &mut Cx<'_>,
    record: &SystemInfo,
    position: Vector,
    affiliation: u32,
) -> Vec<(usize, f32)> {
    let zones = population::collect_space_pop_zones(cx, record, Matrix::IDENTITY, position);
    let mut out = Vec::new();
    for &index in zones.at_ship.iter().chain(&zones.near) {
        let zone = &record.zones[index];
        if zone.path.is_none() {
            continue;
        }
        let factions = api::zone::population(cx, zone.zone_id).unwrap_or_default();
        let (_, hostile_to) = super::cargo::hostile_faction_shares(cx, &factions, affiliation);
        if !(hostile_to > 0.0) {
            continue;
        }
        let weight = zone.tradelane_attack();
        if weight > 0.0 {
            out.push((index, weight));
        }
    }
    out
}

/// `BuildSystemInfos` (`:2056-2067`): one record per Universe system
/// (`pub::NextSystemID`), an existing record kept.
pub fn build_system_infos(cx: &mut Cx<'_>) {
    let mut id = 0;
    while api::misc::next_system_id(cx, &mut id) {
        cx.logic
            .content
            .state
            .systems
            .entry(id)
            .or_insert_with(|| SystemInfo::new(id));
    }
}

/// `LoadDynSysInfo(Reader)` (`:2076-2098`), GameSupport's `LoadGame`: the
/// table emptied and built again; the save's `[DynSysInfo]` nicknames are
/// looked up and the result dropped.
pub fn load_dyn_sys_info(cx: &mut Cx<'_>, reader: &mut IniReader) {
    cx.logic.content.state.systems.clear();
    build_system_infos(cx);
    while reader.read_header() {
        if !reader.is_header(b"DynSysInfo") {
            continue;
        }
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                let _ = api::misc::system_id(cx, c_str(&reader.value_string()));
                break;
            }
        }
    }
}

/// The ship of a player in the record's players with this id
/// (`FSystemInfo::FindShip`'s first loop): the player's id. The NPC ships
/// of the second loop are P3.6c's. A player no longer in the player states
/// is skipped, where the C++ reads the freed state (D801).
#[must_use]
pub fn find_player_ship(cx: &Cx<'_>, record: &SystemInfo, ship: u32) -> Option<u32> {
    record.players.iter().copied().find(|p| {
        cx.logic
            .content
            .state
            .players
            .get(p)
            .and_then(|s| s.ship.as_ref())
            .is_some_and(|s| s.ship == ship)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn info(object: u32, prev: u32, next: u32) -> RingInfo {
        RingInfo {
            object,
            position: Vector::new(object as f32, 0.0, 0.0),
            rotation: Matrix::IDENTITY,
            prev,
            next,
            reputation: 0,
        }
    }

    #[test]
    fn lanes_chain_their_rings_from_the_first_by_id() {
        let mut rings = BTreeMap::new();
        for r in [
            info(3, 2, 4),
            info(2, 1, 3),
            info(1, 0, 2),
            info(4, 3, 0),
            info(9, 0, 0),
        ] {
            rings.insert(r.object, r);
        }
        let lanes = build_trade_lanes(rings);
        assert_eq!(lanes.len(), 2);
        let ids: Vec<u32> = lanes[0].rings.iter().map(|r| r.object).collect();
        assert_eq!(ids, [1, 2, 3, 4]);
        assert_eq!((lanes[0].start.x, lanes[0].end.x), (1.0, 4.0));
        assert_eq!(lanes[1].rings.len(), 1);
    }

    #[test]
    fn lane_ships_come_and_go() {
        let mut record = SystemInfo::new(1);
        record.trade_lanes = vec![TradeLane::default(), TradeLane::default()];
        record.lane_by_ring.insert(10, 0);
        record.lane_by_ring.insert(20, 1);
        record.add_trade_lane_ship(10, 1, 7);
        record.add_trade_lane_ship(20, -1, 7);
        record.remove_trade_lane_ship(7, 0, 2.5);
        // The first lane's forward match stops the backward search.
        assert!(record.trade_lanes[0].forward_ships.is_empty());
        assert_eq!(record.trade_lanes[0].last_forward_leave, 2.5);
        assert_eq!(record.trade_lanes[1].backward_ships, [7]);
        record.remove_trade_lane_ship(7, 20, 3.0);
        assert!(record.trade_lanes[1].backward_ships.is_empty());
        assert_eq!(record.trade_lanes[1].last_backward_leave, 3.0);
        let mut unload_me = SystemInfo::new(2);
        unload_me.load_count = 2;
        unload_me.jumps.push(SystemJump {
            object: 1,
            kind: 1,
            position: Vector::ZERO,
            goto_system: 0,
            goto_object: 0,
        });
        unload(&mut unload_me);
        assert_eq!(unload_me.jumps.len(), 1);
        unload(&mut unload_me);
        assert!(unload_me.jumps.is_empty());
    }
}
