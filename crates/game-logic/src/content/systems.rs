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
//! are their ids; the NPC ships (`SpaceObjs`, P3.6c) are their keys in
//! Content's arena. The mission objects' run time and the zones'
//! population validation (`Validate`, which draws `rand()`) are P3.5's
//! and P3.6d's (D802).
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
    /// `SpaceObjs` (`+0xdc`): the system's NPC ships (P3.6c).
    pub space_objs: Vec<crate::events::NpcShipKey>,
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
            space_objs: Vec::new(),
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
    /// ships and mission objects when no player there has a ship, which
    /// SpaceSupport's `SystemExit` calls after it
    /// ([`destroy_objects_if_unoccupied`]).
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
/// reader, then the trade lanes from the live rings, then each solar NPC's
/// personality state (`SubmitPersonalityState(true)`).
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
    // `SubmitPersonalityState(true)` of every solar NPC, after the record is
    // back in its slot (the submit reaches Content's callbacks).
    let count = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .map_or(0, |r| r.solar_npcs.len());
    for at in 0..count {
        super::solar_npcs::submit_personality_state(cx, system, at, true);
    }
}

/// `FSystemInfo::UnloadFull` (`:383-398`), SpaceSupport's
/// `SystemUnloaded`: the solar NPCs, mission objects and flagged objects
/// emptied (the NPCs' reputation handles go; their vibes keep the
/// faction), in single player SpacePop's solar detection on again, then
/// [`unload`]. First the NPC ships and mission objects are destroyed when
/// no player there has a ship ([`destroy_objects_if_unoccupied`]). Nothing
/// for a system without a record (D801).
pub fn unload_full(cx: &mut Cx<'_>, system: u32) {
    if !cx.logic.content.state.systems.contains_key(&system) {
        return;
    }
    destroy_objects_if_unoccupied(cx, system);
    let Some(record) = cx.logic.content.state.systems.get_mut(&system) else {
        return;
    };
    // `SubmitPersonalityState(false)` does nothing; the solars' callback
    // handles name nothing once they go (D850).
    for npc in &record.solar_npcs {
        if npc.callback != 0 {
            cx.logic.content.state.callbacks.release(npc.callback);
        }
    }
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

/// `FSystemInfo::FindObjectIdByName(&Name, ControllerId)`
/// (`SystemInfo.cpp:2117-2149`): 0 for an empty name; `Player` is the
/// ship of the player whose live mission is the controller (in single
/// player, else the first player's); else the first of the record's NPC
/// ships with the nickname; else the nickname's id when an object of a
/// type of `0xfdff` has it (`IsSpaceObjTypeSet`). The C++ reads the
/// record without a test; no record finds only the player and the
/// nickname id.
#[must_use]
pub fn find_object_id_by_name(cx: &Cx<'_>, system: u32, name: &[u8], controller: u32) -> u32 {
    let name = neuron_core::hash::c_str(name);
    if name.is_empty() {
        return 0;
    }
    let state = &cx.logic.content.state;
    if name == b"Player" {
        let player = state
            .players
            .values()
            .find(|p| p.live_mission == controller)
            .or_else(|| {
                if crate::api::misc::single_player(cx) {
                    state.players.values().next()
                } else {
                    None
                }
            });
        return player.and_then(|p| p.ship.as_ref()).map_or(0, |s| s.ship);
    }
    if let Some(record) = state.systems.get(&system) {
        for key in &record.space_objs {
            if let Some(ship) = state.npc_ships.get(key)
                && neuron_core::hash::c_str(&ship.nickname) == name
            {
                return ship.base.ship;
            }
        }
    }
    let id = crate::api::misc::nickname_id(name);
    match crate::api::space_obj::object_type(cx, id) {
        Ok(kind) if kind & 0xfdff != 0 => id,
        _ => 0,
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

/// `DistanceSquaredBetween(&Point, &Position)` (`:636-642`): the
/// differences and their squares in extended precision (`f64`), summed
/// z, x, y.
fn distance_squared_between(point: Vector, position: Vector) -> f64 {
    let dx = f64::from(point.x) - f64::from(position.x);
    let dy = f64::from(point.y) - f64::from(position.y);
    let dz = f64::from(point.z) - f64::from(position.z);
    dz * dz + dx * dx + dy * dy
}

/// The positions of the record's players' ships, in the record's order
/// (`FContentSpaceObj::GetPosition`: zero when `GetLocation` fails, D841).
fn player_ship_positions(cx: &Cx<'_>, system: u32) -> Vec<(u32, Vector)> {
    let state = &cx.logic.content.state;
    let Some(record) = state.systems.get(&system) else {
        return Vec::new();
    };
    record
        .players
        .iter()
        .filter_map(|p| {
            let ship = state.players.get(p)?.ship.as_ref()?.ship;
            let at = api::space_obj::location(cx, ship).map_or(Vector::ZERO, |(v, _)| v);
            Some((*p, at))
        })
        .collect()
}

/// `FSystemInfo::FindPlayersNear(&Point, &Radius, &Out)` (`:675-691`): the
/// record's players whose ship is within the radius (the square stored as
/// a float, at or below it).
#[must_use]
pub fn find_players_near(cx: &Cx<'_>, system: u32, point: Vector, radius: f32) -> Vec<u32> {
    let radius_square = (f64::from(radius) * f64::from(radius)) as f32;
    player_ship_positions(cx, system)
        .into_iter()
        .filter(|&(_, at)| distance_squared_between(point, at) <= f64::from(radius_square))
        .map(|(p, _)| p)
        .collect()
}

/// `FSystemInfo::IsPlayerShipNear(&Point, &Radius, &OutDistanceSq)`
/// (`:645-670`): a player's ship within the radius.
#[must_use]
pub fn is_player_ship_near(cx: &Cx<'_>, system: u32, point: Vector, radius: f32) -> bool {
    let radius_square = (f64::from(radius) * f64::from(radius)) as f32;
    player_ship_positions(cx, system)
        .into_iter()
        .any(|(_, at)| distance_squared_between(point, at) <= f64::from(radius_square))
}

/// `FSystemInfo::RemoveMissionObject(Id)` (`:473-497`): the first entry of
/// the mission objects goes, and its reputation is freed when the object
/// has one; then the first entry of the flagged objects.
pub fn remove_mission_object(cx: &mut Cx<'_>, system: u32, object: u32) {
    let Some(record) = cx.logic.content.state.systems.get_mut(&system) else {
        return;
    };
    let listed = record.mission_objects.iter().position(|&o| o == object);
    if let Some(at) = listed {
        record.mission_objects.remove(at);
    }
    if listed.is_some()
        && let Ok(rep) = api::space_obj::rep(cx, object)
    {
        let _ = api::reputation::free(cx, rep);
    }
    if let Some(record) = cx.logic.content.state.systems.get_mut(&system)
        && let Some(at) = record.flagged_objects.iter().position(|&o| o == object)
    {
        record.flagged_objects.remove(at);
    }
}

/// `FSystemInfo::AddMissionObject(Id)` (`:465-470`): appended.
pub fn add_mission_object(cx: &mut Cx<'_>, system: u32, object: u32) {
    if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
        record.mission_objects.push(object);
    }
}

/// `FSystemInfo::DestroyMissionObjects` (`:523-536`): each of a copy of the
/// mission objects destroyed (`DESTROY_NORMAL`), then
/// [`remove_mission_object`].
pub fn destroy_mission_objects(cx: &mut Cx<'_>, system: u32) {
    let objects = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .map(|r| r.mission_objects.clone())
        .unwrap_or_default();
    for object in objects {
        let _ = api::space_obj::destroy(cx, object, 0);
        remove_mission_object(cx, system, object);
    }
}

/// `FSystemInfo::FindObjectIdByName(Name, ControllerId)` (`:2117-2149`):
/// `Player` is the ship of the player whose live mission is the controller
/// (in single player, else the first player); another name the first of
/// the record's NPC ships of that nickname (`TString ==`), else the
/// universe object of that nickname when it has a type.
#[must_use]
pub fn find_object_id_by_name(cx: &Cx<'_>, system: u32, name: &[u8], controller: u32) -> u32 {
    let name = c_str(name);
    if name.is_empty() {
        return 0;
    }
    let state = &cx.logic.content.state;
    if name == b"Player" {
        let player = state
            .players
            .values()
            .find(|p| p.live_mission == controller)
            .or_else(|| {
                api::misc::single_player(cx)
                    .then(|| state.players.values().next())
                    .flatten()
            });
        return player.and_then(|p| p.ship.as_ref()).map_or(0, |s| s.ship);
    }
    if let Some(record) = state.systems.get(&system) {
        for key in &record.space_objs {
            if let Some(n) = state.npc_ships.get(key)
                && c_str(&n.nickname) == name
            {
                return n.base.ship;
            }
        }
    }
    let id = api::misc::nickname_id(name);
    match api::space_obj::object_type(cx, id) {
        Ok(kind) if kind & 0xfdff != 0 => id,
        _ => 0,
    }
}

/// `FSystemInfo::DestroyFlaggedObjectsAwayFromPlayers` (`:897-923`),
/// SpaceSupport's heartbeat: each flagged object with no player's ship
/// within 10000 is destroyed (`DESTROY_NORMAL`) and leaves the mission
/// objects; the location of an object that is gone reads zero (D841).
pub fn destroy_flagged_objects_away_from_players(cx: &mut Cx<'_>, system: u32) {
    let Some(flagged) = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .map(|r| r.flagged_objects.clone())
    else {
        return;
    };
    if flagged.is_empty() {
        return;
    }
    let away: Vec<u32> = flagged
        .into_iter()
        .filter(|&o| {
            let at = api::space_obj::location(cx, o).map_or(Vector::ZERO, |(v, _)| v);
            !is_player_ship_near(cx, system, at, 10000.0)
        })
        .collect();
    for object in away {
        let _ = api::space_obj::destroy(cx, object, 0);
        remove_mission_object(cx, system, object);
    }
}

/// `FSystemInfo::DestroyObjectsIfUnoccupied` (`:436-462`), single player's
/// `RemovePlayer` and `OnPlayerChanged`: with no player of the record in a
/// ship, the record's NPC ships (`DESTROY_NORMAL`) and then its mission
/// objects (`DESTROY_NORMAL`, then `RemoveMissionObject`) are destroyed.
pub fn destroy_objects_if_unoccupied(cx: &mut Cx<'_>, system: u32) {
    let state = &cx.logic.content.state;
    let Some(record) = state.systems.get(&system) else {
        return;
    };
    if record
        .players
        .iter()
        .any(|p| state.players.get(p).is_some_and(|s| s.ship.is_some()))
    {
        return;
    }
    // `DestroySpaceObj` of a copy of the space objects, then the mission
    // objects.
    let ships: Vec<u32> = record
        .space_objs
        .iter()
        .filter_map(|k| state.npc_ships.get(k).map(|s| s.base.ship))
        .collect();
    let objects = record.mission_objects.clone();
    for ship in ships {
        let _ = api::space_obj::destroy(cx, ship, 0);
    }
    for object in objects {
        let _ = api::space_obj::destroy(cx, object, 0);
        remove_mission_object(cx, system, object);
    }
}

/// `FSystemInfo::RemoveSpaceObj(Id, bDelete)` (`SystemInfo.cpp:543-556`):
/// the first NPC ship with the id leaves the record's space objects, and
/// with `delete` the ship is destroyed ([`super::npc_ships::destroy`]).
pub fn remove_space_obj(cx: &mut Cx<'_>, system: u32, object: u32, delete: bool) {
    let Some(key) = super::ships::find_space_obj(&cx.logic.content.state, system, object) else {
        return;
    };
    if delete {
        super::npc_ships::destroy(cx, key);
    }
    if let Some(record) = cx.logic.content.state.systems.get_mut(&system)
        && let Some(at) = record.space_objs.iter().position(|k| *k == key)
    {
        record.space_objs.remove(at);
    }
}

// ---- the NPC ships' lives (P3.6c) -------------------------------------------------------------

/// `FSystemInfo::IsPlayerShipNear(&Point, &Radius, &OutDistanceSq)`
/// (`:645-670`) with its out-parameter: whether a player's ship is within
/// the radius, and otherwise the smallest squared distance to one (stored
/// as a float; -1 when no player has a ship). -1 when one is near.
#[must_use]
pub fn player_ship_near(cx: &Cx<'_>, system: u32, point: Vector, radius: f32) -> (bool, f32) {
    let radius_square = (f64::from(radius) * f64::from(radius)) as f32;
    let mut out = -1.0_f32;
    for (_, at) in player_ship_positions(cx, system) {
        let d = distance_squared_between(point, at);
        if d <= f64::from(radius_square) {
            return (true, -1.0);
        }
        if out == -1.0 || d < f64::from(out) {
            out = d as f32;
        }
    }
    (false, out)
}

/// `ReciprocalSqrtGuess`: the integer guess `0x5f3759df - (bits >> 1)`.
fn reciprocal_sqrt_guess(value: f32) -> f32 {
    let bits = value.to_bits() as i32;
    f32::from_bits((0x5f37_59df_i32.wrapping_sub(bits >> 1)) as u32)
}

/// `IsPointInViewCone(&Point, &Viewer, &MaxDistance, &ConeAngle)` (TU53,
/// `SystemInfo.cpp:1968-2016`): the point within the distance of the
/// viewer and inside the cone of the angle (degrees) around its forward
/// axis, with the guesses and Newton steps of the C++.
#[must_use]
pub fn is_point_in_view_cone(
    point: Vector,
    position: Vector,
    orientation: &Matrix,
    max_distance: f32,
    cone_angle: f32,
) -> bool {
    let dx = f64::from(point.x) - f64::from(position.x);
    let dy = (f64::from(point.y) - f64::from(position.y)) as f32;
    let dz_wide = f64::from(point.z) - f64::from(position.z);
    let dz = dz_wide as f32;
    let length_sq = dz_wide * f64::from(dz) + f64::from(dy) * f64::from(dy) + dx * dx;
    let stored = length_sq as f32;
    let guess = f64::from(reciprocal_sqrt_guess(stored));
    let length = (1.5 - guess * guess * length_sq * 0.5) * guess * length_sq;
    if !(length <= f64::from(max_distance)) {
        return false;
    }
    let m = &orientation.m;
    let (fx, fy, fz) = (
        -f64::from(m[0][2]),
        -f64::from(m[1][2]),
        -f64::from(m[2][2]),
    );
    let forward_sq = fz * fz + fy * fy + fx * fx;
    let forward_guess = f64::from(reciprocal_sqrt_guess(forward_sq as f32));
    let rounded = f64::from(stored);
    let guess2 = f64::from(reciprocal_sqrt_guess(stored));
    let dot = fz * f64::from(dz) + fy * f64::from(dy) + fx * dx;
    let forward_step = 1.5 - forward_guess * forward_guess * forward_sq * 0.5;
    let length_step = 1.5 - guess2 * guess2 * rounded * 0.5;
    let norms = forward_step * length_step * guess2 * rounded * forward_guess * forward_sq;
    let cos_to_point = dot / norms;
    let cos_half = (f64::from(cone_angle) * 0.5 * f64::from(0.017_453_292_f32)).cos();
    cos_half <= cos_to_point
}

/// The lanes of the record's players' ships (`HasField120`, `GetField120`).
fn player_lanes(cx: &Cx<'_>, system: u32) -> Vec<(u32, u32)> {
    let state = &cx.logic.content.state;
    let Some(record) = state.systems.get(&system) else {
        return Vec::new();
    };
    record
        .players
        .iter()
        .filter_map(|p| state.players.get(p)?.ship.as_ref())
        .filter(|s| s.lane_entry != 0)
        .map(|s| (s.lane_entry, s.lane_end))
        .collect()
}

/// `IsNearPlayerField124Object(&Point, &Radius)` (`:712-731`): a player's
/// ship in a lane whose end ring is within the radius.
fn is_near_player_lane_end(cx: &Cx<'_>, system: u32, point: Vector, radius: f32) -> bool {
    let radius_square = (f64::from(radius) * f64::from(radius)) as f32;
    player_lanes(cx, system).into_iter().any(|(_, end)| {
        let at = api::space_obj::location(cx, end).map_or(Vector::ZERO, |(p, _)| p);
        distance_squared_between(point, at) <= f64::from(radius_square)
    })
}

/// `IsNearPlayerField120Segment(&Point, &Radius)` (`:734-756`): a player's
/// ship in a lane whose segment from the entry to the end ring passes
/// within the radius.
fn is_near_player_lane_segment(cx: &Cx<'_>, system: u32, point: Vector, radius: f32) -> bool {
    let radius_square = (f64::from(radius) * f64::from(radius)) as f32;
    player_lanes(cx, system).into_iter().any(|(entry, end)| {
        let start = api::space_obj::location(cx, entry).map_or(Vector::ZERO, |(p, _)| p);
        let stop = api::space_obj::location(cx, end).map_or(Vector::ZERO, |(p, _)| p);
        let (d, _) = super::patrol_paths::distance_squared_to_segment(point, start, stop);
        d <= f64::from(radius_square)
    })
}

/// `FSystemInfo::GetNpcShipLifeRate(Ship)` (`:822-894`): -1 while a player
/// is close (or the ship travels a lane), else how fast its life runs down
/// with the distance to the nearest player's ship, at most 15.
fn npc_ship_life_rate(cx: &Cx<'_>, system: u32, ship: crate::events::NpcShipKey) -> f64 {
    let near = -1.0;
    let key = super::ships::ShipKey::Npc(ship);
    let state = &cx.logic.content.state;
    if super::ships::base(state, key).is_some_and(|s| s.lane_entry != 0) {
        return near;
    }
    let position = super::ships::position(cx, key);
    let (close, distance_sq) = player_ship_near(cx, system, position, 2500.0);
    if close {
        return near;
    }
    if api::misc::single_player(cx) {
        let viewers: Vec<(Vector, Matrix)> = state
            .systems
            .get(&system)
            .map(|r| {
                r.players
                    .iter()
                    .filter_map(|p| state.players.get(p)?.ship.as_ref())
                    .map(|s| {
                        api::space_obj::location(cx, s.ship)
                            .unwrap_or((Vector::ZERO, Matrix::IDENTITY))
                    })
                    .collect()
            })
            .unwrap_or_default();
        if viewers
            .iter()
            .any(|(p, m)| is_point_in_view_cone(position, *p, m, 4000.0, 70.0))
        {
            return near;
        }
    }
    if is_near_player_lane_end(cx, system, position, 4000.0) {
        return near;
    }
    if is_near_player_lane_segment(cx, system, position, 2500.0) {
        return near;
    }
    if !(distance_sq > 6_250_000.0) {
        return 15.0;
    }
    let rate = (f64::from(distance_sq).sqrt() - 2500.0) * 0.01 + 1.0;
    if !(rate > 15.0) {
        return rate;
    }
    15.0
}

/// Whether the ship of some player of the record targets `id`
/// (`IsTargetedByPlayerShip`; a failed `GetTarget` reads 0).
fn is_targeted_by_player_ship(cx: &Cx<'_>, system: u32, id: u32) -> bool {
    let state = &cx.logic.content.state;
    state.systems.get(&system).is_some_and(|r| {
        r.players.iter().any(|p| {
            state
                .players
                .get(p)
                .and_then(|s| s.ship.as_ref())
                .is_some_and(|s| api::space_obj::target(cx, s.ship).unwrap_or(0) == id)
        })
    })
}

/// `FSystemInfo::UpdateNpcShipLives(DeltaTime)` (`:950-1030`), SpaceSupport's
/// heartbeat with its period: a wingman's life, and that of a ship a player
/// is near, is refilled; any other mortal ship's life runs down by its
/// rate, and one whose life ran out is destroyed (`DESTROY_NORMAL`) with
/// its whole formation, unless one of the formation is a player's ship or
/// (in single player) a player targets it.
pub fn update_npc_ship_lives(cx: &mut Cx<'_>, system: u32, dt: f32) {
    let ships: Vec<crate::events::NpcShipKey> = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .map(|r| r.space_objs.clone())
        .unwrap_or_default();
    if ships.is_empty() {
        return;
    }
    let single = api::misc::single_player(cx);
    let mut expired: Vec<crate::events::NpcShipKey> = Vec::new();
    for key in ships {
        let Some(ship) = cx.logic.content.state.npc_ships.get(&key) else {
            continue;
        };
        let refill = |cx: &mut Cx<'_>| {
            if let Some(s) = cx.logic.content.state.npc_ships.get_mut(&key)
                && s.life_left < s.life_time
            {
                s.life_left = s.life_time;
            }
        };
        if ship.base.formation_slot != 0 && ship.base.formation_id != u32::MAX {
            refill(cx);
            continue;
        }
        if ship.life_time == -1.0 {
            continue;
        }
        let product = npc_ship_life_rate(cx, system, key) * f64::from(dt);
        let step = product as f32;
        if product < 0.0 {
            refill(cx);
            continue;
        }
        let Some(ship) = cx.logic.content.state.npc_ships.get_mut(&key) else {
            continue;
        };
        if !(ship.life_left >= 0.0) {
            continue;
        }
        ship.life_left = (f64::from(ship.life_left) - f64::from(step)) as f32;
        if !(ship.life_left < 0.0) {
            continue;
        }
        ship.life_left = 0.0;
        let (id, formation) = (ship.base.ship, ship.base.formation_id);
        if single && is_targeted_by_player_ship(cx, system, id) {
            continue;
        }
        if formation == u32::MAX {
            expired.push(key);
            continue;
        }
        let members = super::formations::collect_members(&cx.logic.content.state, formation);
        let keep = members.iter().any(|&m| {
            let member = super::ships::id(&cx.logic.content.state, m);
            super::players::find_by_ship(&cx.logic.content.state.players, member).is_some()
                || (single && is_targeted_by_player_ship(cx, system, member))
        });
        if !keep {
            // No member is a player's ship (that keeps the formation).
            expired.extend(members.into_iter().filter_map(|m| match m {
                super::ships::ShipKey::Npc(k) => Some(k),
                super::ships::ShipKey::Player(_) => None,
            }));
        }
    }
    for key in expired {
        let id = cx
            .logic
            .content
            .state
            .npc_ships
            .get(&key)
            .map_or(0, |s| s.base.ship);
        let _ = api::space_obj::destroy(cx, id, 0);
    }
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
