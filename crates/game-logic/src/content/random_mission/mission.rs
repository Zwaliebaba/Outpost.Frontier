//! The random mission and its loader (TU27 `Content/DestroyMission.cpp`
//! and TU47 `Content/RandomMissionDB.cpp`): `FRandomMissionData` with its
//! `[RandomMission]` save section, the loader's `CreateMission` with its
//! zone search and enemy pick, `FinalizeMission`, `Load` (the restore
//! from a save), `GetOfferDescription`, `CanAccept` and `IsRouteBlocked`,
//! and `PickRmMissionDifficulty` and `ReleaseRmDestSystems` (work package
//! P3.7c, record 0008 points 8 and 10).
//!
//! The C++ mission data is reference counted (`FContentRefObject`): the
//! loader holds one reference, the `DestroyMission` controller another.
//! The port keeps every mission in [`RmMissions`], a table of Content's
//! state, by [`RmDataKey`], with its count; [`release`] at 0 is the
//! destructor's effect: the objective zone leaves [`super::RmState`]'s
//! `objective_zones` and each offer is released
//! ([`offer::release_offer`]). The loader ([`Loader`]) is owned by its
//! holder, as the bar and vendor offers own theirs.

// The C++'s negated float tests (`!(x > 0.0f)`) send NaN to the failing
// side; the port spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use std::collections::BTreeMap;
use std::io::{self, Write};

use game_sim::protocol::base_props::MissionVendorProperties;
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::msvcrt::format_g;

use super::generator::{self, OfferQuery};
use super::offer::{self, Offer};
use super::tables::add_unique;
use super::vignette::TreePath;
use super::{RmDataKey, register_dest_system};
use crate::api;
use crate::content::patrol_paths::distance_squared_to_segment;
use crate::content::{NoDispatch, data_source, system_paths, systems};
use crate::server::Cx;

/// `GRmRangeScale` (300): game units per range unit.
const RANGE_SCALE: f32 = 300.0;

/// The reputation group `CollectHostileGroups` skips (an id constant in the
/// listing).
const SKIPPED_GROUP: u32 = 0x62e8;

/// `FRandomMissionData` (0x88 bytes) with its base `FRmMissionDataBase`.
#[derive(Clone, Debug, PartialEq)]
pub struct MissionData {
    /// `PlayerId` (`+0x0c`).
    pub player: u32,
    /// `DestSys` (`+0x10`).
    pub dest_sys: u32,
    /// `OfferGroup` (`+0x14`, -1).
    pub offer_group: i32,
    /// `EnemyGroup` (`+0x18`, -1).
    pub enemy_group: u32,
    /// `OfferBase` (`+0x1c`).
    pub offer_base: u32,
    /// `OfferIds` (`+0x20`).
    pub offer_ids: i32,
    /// `Description` (`+0x24`): the last built offer's text.
    pub description: FmtStr,
    /// `Reward` (`+0x3c`).
    pub reward: i32,
    /// `RepReward` (`+0x40`).
    pub rep_reward: f32,
    /// `RepHit` (`+0x44`).
    pub rep_hit: f32,
    /// `FlagTime` (`+0x48`, 360; not saved).
    pub flag_time: f32,
    /// `Difficulty` (`+0x4c`).
    pub difficulty: f32,
    /// `JumpPref` (`+0x50`).
    pub jump_pref: i32,
    /// `ObjectivePos` (`+0x54`).
    pub objective_pos: Vector,
    /// `ObjectiveZone` (`+0x60`).
    pub objective_zone: u32,
    /// `RandomSeed` (`+0x64`): heap garbage in the original until written,
    /// 0 in the reconstruction and the port (D950).
    pub random_seed: u32,
    /// `TreePath` (`+0x68`), written by `BuildOffer`.
    pub tree_path: TreePath,
    /// `Offers` (`+0x7c`), in build order.
    pub offers: Vec<Offer>,
}

impl Default for MissionData {
    fn default() -> Self {
        Self {
            player: 0,
            dest_sys: 0,
            offer_group: -1,
            enemy_group: u32::MAX,
            offer_base: 0,
            offer_ids: 0,
            description: FmtStr::new(0),
            reward: 0,
            rep_reward: 0.0,
            rep_hit: 0.0,
            flag_time: 360.0,
            difficulty: 0.0,
            jump_pref: 0,
            objective_pos: Vector::ZERO,
            objective_zone: 0,
            random_seed: 0,
            tree_path: TreePath::default(),
            offers: Vec::new(),
        }
    }
}

/// `%g` of a float pushed as a double.
fn g(value: f32) -> String {
    format_g(f64::from(value), 6)
}

impl MissionData {
    /// `FRandomMissionData::Save` (`DestroyMission.cpp:567-577`) after
    /// `FRmMissionDataBase::Save` (`RandomMissionDB.cpp:307-317`): the
    /// base's eight lines, then six more; the floats through `%g`, the tree
    /// path not at all.
    ///
    /// # Errors
    ///
    /// The writer's.
    pub fn save(&self, out: &mut dyn Write) -> io::Result<()> {
        writeln!(out, "offer_group = {}", self.offer_group)?;
        writeln!(out, "offer_base = {}", self.offer_base)?;
        writeln!(out, "offer_ids = {}", self.offer_ids)?;
        writeln!(out, "reward = {}", self.reward)?;
        writeln!(out, "rep_reward = {}", g(self.rep_reward))?;
        writeln!(out, "rep_hit = {}", g(self.rep_hit))?;
        writeln!(out, "difficulty = {}", g(self.difficulty))?;
        writeln!(out, "jump_pref = {}", self.jump_pref)?;
        writeln!(out, "enemy_group = {}", self.enemy_group)?;
        writeln!(out, "dest_sys = {}", self.dest_sys)?;
        writeln!(
            out,
            "objective_pos = {},{},{}",
            g(self.objective_pos.x),
            g(self.objective_pos.y),
            g(self.objective_pos.z)
        )?;
        writeln!(out, "objective_zone = {}", self.objective_zone)?;
        writeln!(out, "random_seed = {}", self.random_seed)?;
        writeln!(out)
    }

    /// `FRandomMissionData::LoadValue` (`:582-614`) after the base's
    /// (`RandomMissionDB.cpp:320-361`): the first key that matches is
    /// stored. The position is read as three integers; a loaded objective
    /// zone joins `objective_zones` when it is not listed.
    pub fn load_value(&mut self, objective_zones: &mut Vec<u32>, reader: &mut IniReader) {
        if reader.is_value(b"offer_group") {
            self.offer_group = reader.value_int(0);
        } else if reader.is_value(b"offer_base") {
            self.offer_base = reader.value_int(0) as u32;
        } else if reader.is_value(b"offer_ids") {
            self.offer_ids = reader.value_int(0);
        } else if reader.is_value(b"reward") {
            self.reward = reader.value_int(0);
        } else if reader.is_value(b"rep_reward") {
            self.rep_reward = reader.value_float(0) as f32;
        } else if reader.is_value(b"rep_hit") {
            self.rep_hit = reader.value_float(0) as f32;
        } else if reader.is_value(b"difficulty") {
            self.difficulty = reader.value_float(0) as f32;
        } else if reader.is_value(b"jump_pref") {
            self.jump_pref = reader.value_int(0);
        }
        // The base's result is not read: its keys fall through to these
        // tests, which none of them matches.
        if reader.is_value(b"enemy_group") {
            self.enemy_group = reader.value_int(0) as u32;
        } else if reader.is_value(b"dest_sys") {
            self.dest_sys = reader.value_int(0) as u32;
        } else if reader.is_value(b"objective_pos") {
            self.objective_pos = Vector::new(
                reader.value_int(0) as f32,
                reader.value_int(1) as f32,
                reader.value_int(2) as f32,
            );
        } else if reader.is_value(b"objective_zone") {
            self.objective_zone = reader.value_int(0) as u32;
            add_unique(objective_zones, self.objective_zone);
        } else if reader.is_value(b"random_seed") {
            self.random_seed = reader.value_int(0) as u32;
        }
    }

    /// `FinalizeMission`'s query from the mission's values.
    #[must_use]
    pub fn query(&self) -> OfferQuery {
        OfferQuery {
            offer_group: self.offer_group,
            enemy_group: self.enemy_group,
            reward: self.reward,
            rep_reward: self.rep_reward,
            rep_hit: self.rep_hit,
            offer_base: self.offer_base,
            difficulty: self.difficulty,
            objective_zone: self.objective_zone,
            objective_pos: self.objective_pos,
            player: self.player,
            dest_sys: self.dest_sys,
        }
    }
}

/// The section a `DestroyMission` controller writes on `PlayerSave`
/// (`DestroyMission.cpp:399-414`): the header, the type and
/// [`MissionData::save`]. The controller's handler calls it with its gates
/// (single player, the mission's player, the first running machine's
/// `CanSave`; [`super::runtime`]).
///
/// # Errors
///
/// The writer's.
pub fn write_section(data: &MissionData, out: &mut dyn Write) -> io::Result<()> {
    writeln!(out, "[RandomMission]")?;
    writeln!(out, "type = DestroyMission")?;
    data.save(out)
}

/// One mission of [`RmMissions`]: its data and its reference count.
#[derive(Clone, Debug, PartialEq)]
pub struct MissionEntry {
    /// The references (`FContentRefObject`'s count).
    pub refs: u32,
    /// The mission.
    pub data: MissionData,
}

/// Content's random missions by key.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RmMissions {
    /// The live missions.
    pub entries: BTreeMap<RmDataKey, MissionEntry>,
    /// The last key handed out (keys start at 1 and are never reused).
    pub last_key: u32,
}

impl RmMissions {
    /// `FRandomMissionData::Create`: a new mission with one reference.
    pub fn create(&mut self) -> RmDataKey {
        self.last_key = self.last_key.wrapping_add(1);
        let key = RmDataKey(self.last_key);
        self.entries.insert(
            key,
            MissionEntry {
                refs: 1,
                data: MissionData::default(),
            },
        );
        key
    }

    /// The mission of `key`.
    #[must_use]
    pub fn get(&self, key: RmDataKey) -> Option<&MissionData> {
        self.entries.get(&key).map(|e| &e.data)
    }

    /// The mission of `key`, to change.
    pub fn get_mut(&mut self, key: RmDataKey) -> Option<&mut MissionData> {
        self.entries.get_mut(&key).map(|e| &mut e.data)
    }

    /// `AddRef`; nothing for a key that names no mission.
    pub fn add_ref(&mut self, key: RmDataKey) {
        if let Some(e) = self.entries.get_mut(&key) {
            e.refs = e.refs.wrapping_add(1);
        }
    }
}

/// `Release` of mission `key`: the last reference destroys it
/// (`~FRandomMissionData`, `DestroyMission.cpp:552-563`): a non-zero
/// objective zone leaves the objective zones (its first entry), then every
/// offer is released in order.
pub fn release(cx: &mut Cx<'_>, key: RmDataKey) {
    let missions = &mut cx.logic.content.state.rm.missions;
    let Some(entry) = missions.entries.get_mut(&key) else {
        return;
    };
    entry.refs = entry.refs.saturating_sub(1);
    if entry.refs != 0 {
        return;
    }
    let Some(entry) = missions.entries.remove(&key) else {
        return;
    };
    let data = entry.data;
    if data.objective_zone != 0 {
        let zones = &mut cx.logic.content.state.rm.objective_zones;
        if let Some(i) = zones.iter().position(|&z| z == data.objective_zone) {
            zones.remove(i);
        }
    }
    for o in &data.offers {
        offer::release_offer(cx, o);
    }
}

/// `FRmMissionRequest`: the four values `CreateMission` reads.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MissionRequest {
    /// `PlayerId`.
    pub player: u32,
    /// `OfferBase`.
    pub offer_base: u32,
    /// `OfferGroup`.
    pub offer_group: i32,
    /// `Difficulty`.
    pub difficulty: f32,
}

/// `CreateMission`'s results (`RandomMission.h:141-143`).
pub mod result {
    /// The mission has an offer.
    pub const OK: i32 = 0;
    /// No enemy group.
    pub const NO_ENEMY: i32 = 1;
    /// No offer (`FinalizeMission`).
    pub const NO_OFFER: i32 = 2;
    /// No objective zone.
    pub const NO_OBJECTIVE: i32 = 3;
}

/// The random-mission loader (`FRandomMissionLoader`, 0x0c bytes): the
/// mission it holds one reference to.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Loader {
    /// `Mission` (`+0x08`).
    pub mission: Option<RmDataKey>,
}

/// `PickRandomId` (`DestroyMission.cpp:112-119`): index 0 of one id, else
/// `(rand() * n) / 32768`.
fn pick_random_id(cx: &mut Cx<'_>, ids: &[u32]) -> u32 {
    let count = ids.len() as i32;
    let mut index = 0;
    if count - 1 != 0 {
        index = cx.world.rng().rand().wrapping_mul(count) / 32768;
    }
    ids.get(index as usize).copied().unwrap_or(0)
}

/// `CollectMissionZones` (`:67-79`): every zone `InZones` finds (at most
/// 0x100) that `system`'s record knows as lawful or unlawful, appended once.
fn collect_mission_zones(
    cx: &mut Cx<'_>,
    system: u32,
    position: Vector,
    radius: f32,
    out: &mut Vec<u32>,
) {
    let found = api::system::in_zones(cx, system, position, radius, 0x100).unwrap_or_default();
    let Some(record) = cx.logic.content.state.systems.get(&system) else {
        return;
    };
    for zone in found {
        if let Some(i) = record.find_zone(zone)
            && (record.zones[i].lawful || record.zones[i].unlawful)
        {
            add_unique(out, zone);
        }
    }
}

/// `CollectHostileGroups` (`:85-106`): every group but 0x62e8 populating
/// `zone` that `offer_group` feels at or below -0.2 about, appended once. A
/// failed reputation lookup reads 0 as in the reconstruction; a zone
/// without a population has none (D951).
fn collect_hostile_groups(cx: &mut Cx<'_>, zone: u32, offer_group: i32, out: &mut Vec<u32>) {
    let population = api::zone::population(cx, zone).unwrap_or_default();
    for (group, _) in population {
        if group == SKIPPED_GROUP {
            continue;
        }
        let feeling =
            api::reputation::group_reputation(cx, offer_group as u32, group).unwrap_or(0.0);
        if feeling <= -0.2 {
            add_unique(out, group);
        }
    }
}

/// `FindTradeLanesNear(Position, Radius)` (`SystemInfo.cpp:1732-1775`) for a
/// radius other than -1: the lanes whose segment passes strictly within
/// the radius (squared as a float), by index.
fn trade_lanes_near(record: &systems::SystemInfo, position: Vector, radius: f32) -> Vec<usize> {
    let radius_squared = (f64::from(radius) * f64::from(radius)) as f32;
    let mut out = Vec::new();
    for (i, lane) in record.trade_lanes.iter().enumerate() {
        let (distance, _) = distance_squared_to_segment(position, lane.start, lane.end);
        if distance < f64::from(radius_squared) {
            add_unique(&mut out, i);
        }
    }
    out
}

/// A squared distance with float differences, summed in z, y, x order and
/// stored as a float.
fn distance_squared_f32(a: Vector, b: Vector) -> f32 {
    let dx = (f64::from(a.x) - f64::from(b.x)) as f32;
    let dy = (f64::from(a.y) - f64::from(b.y)) as f32;
    let dz = (f64::from(a.z) - f64::from(b.z)) as f32;
    let (dx, dy, dz) = (f64::from(dx), f64::from(dy), f64::from(dz));
    (dz * dz + dy * dy + dx * dx) as f32
}

impl Loader {
    /// `CreateRandomMissionLoader`: a loader without a mission (the type is
    /// not read).
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The loader's own `Release` reaching 0 (`~FRandomMissionLoader`,
    /// `RandomMissionDB.cpp:377-384`): the held mission is released.
    pub fn release(&mut self, cx: &mut Cx<'_>) {
        if let Some(key) = self.mission.take() {
            release(cx, key);
        }
    }

    /// The held mission's data (a copy).
    fn data(&self, cx: &Cx<'_>) -> MissionData {
        self.mission
            .and_then(|k| cx.logic.content.state.rm.missions.get(k))
            .cloned()
            .unwrap_or_default()
    }

    /// A field of the held mission (its default without one).
    fn field<T: Default>(&self, cx: &Cx<'_>, read: impl FnOnce(&MissionData) -> T) -> T {
        self.mission
            .and_then(|k| cx.logic.content.state.rm.missions.get(k))
            .map(read)
            .unwrap_or_default()
    }

    /// `FinalizeMission` (`DestroyMission.cpp:623-650`): the offer query
    /// from the mission's values, the world's generator seeded with its
    /// seed, then `BuildOffer` into the mission's tree path; a built offer
    /// is appended and its text becomes the description, one made but not
    /// built is released. 2 while the mission has no offer, else 0.
    pub fn finalize_mission(&mut self, cx: &mut Cx<'_>) -> i32 {
        let Some(key) = self.mission else {
            return result::NO_OFFER;
        };
        let mut data = self.data(cx);
        let query = data.query();
        cx.world.rng().seed(data.random_seed);
        let (built, made) = generator::build_offer(cx, &query, &mut data.tree_path);
        if built && let Some(o) = made {
            data.description = o.text.clone();
            data.offers.push(o);
        } else if let Some(o) = &made {
            offer::release_offer(cx, o);
        }
        let empty = data.offers.is_empty();
        if let Some(slot) = cx.logic.content.state.rm.missions.get_mut(key) {
            *slot = data;
        }
        if empty { result::NO_OFFER } else { result::OK }
    }

    /// `Load` (`:661-689`): the reader's place kept and the reader reset;
    /// the held mission released and a new one made for `player`; every
    /// `[RandomMission]` section feeds its values to
    /// [`MissionData::load_value`] and then [`Self::finalize_mission`],
    /// whose last result decides. The reader goes back to its place.
    pub fn load(
        &mut self,
        cx: &mut Cx<'_>,
        reader: &mut IniReader,
        player: u32,
    ) -> (bool, RmDataKey) {
        let saved = reader.state();
        reader.reset();
        self.release(cx);
        let key = cx.logic.content.state.rm.missions.create();
        self.mission = Some(key);
        if let Some(m) = cx.logic.content.state.rm.missions.get_mut(key) {
            m.player = player;
        }
        let mut loaded = false;
        while reader.read_header() {
            if !reader.is_header(b"RandomMission") {
                continue;
            }
            while reader.read_value() {
                let rm = &mut cx.logic.content.state.rm;
                if let Some(entry) = rm.missions.entries.get_mut(&key) {
                    entry.data.load_value(&mut rm.objective_zones, reader);
                }
            }
            loaded = self.finalize_mission(cx) == result::OK;
        }
        reader.set_state(&saved);
        (loaded, key)
    }

    /// `CreateMission` (`:1041-1120`): the held mission released and a new
    /// one made with a seed from `rand()` (also given to `srand`), the
    /// request's values, the reward for the difficulty and ±0.15. The
    /// objective zone is searched from the base's dock object in the
    /// player's system, with a range of 90, in multiplayer 30 more per
    /// other player there, at most 300. A zone becomes an objective, and
    /// the enemy group is picked around it, else around the origin of the
    /// base's system. Returns the result ([`result`]) and the mission.
    ///
    /// A player without a system record, which the C++ dereferences, finds
    /// no zone (D951).
    pub fn create_mission(
        &mut self,
        cx: &mut Cx<'_>,
        request: &MissionRequest,
    ) -> (i32, RmDataKey) {
        self.release(cx);
        let key = cx.logic.content.state.rm.missions.create();
        self.mission = Some(key);
        let seed = cx.world.rng().rand() as u32;
        cx.world.rng().seed(seed);
        let reward = cx
            .logic
            .content
            .rm
            .tables
            .difficulty_to_money(request.difficulty) as i32;
        self.update(cx, |m| {
            m.random_seed = seed;
            m.player = request.player;
            m.offer_base = request.offer_base;
            m.offer_group = request.offer_group;
            m.difficulty = request.difficulty;
            m.reward = reward;
            m.rep_reward = 0.15;
            m.rep_hit = -0.15;
        });

        let dock = find_dock_object(cx, request.offer_base);
        let system = cx
            .logic
            .content
            .state
            .players
            .get(&request.player)
            .and_then(|s| s.system)
            .filter(|s| cx.logic.content.state.systems.contains_key(s));
        let Some(system) = system else {
            return (result::NO_OBJECTIVE, key);
        };
        register_dest_system(cx, system);
        let mut range = 90.0f32;
        if !api::misc::single_player(cx) {
            let players = cx
                .logic
                .content
                .state
                .systems
                .get(&system)
                .map_or(0, |r| r.players.len()) as i32;
            let mut scaled = f64::from(players - 1) * 30.0 + 90.0;
            if scaled > 300.0 {
                scaled = 300.0;
            }
            range = scaled as f32;
        }
        let zone = self.pick_objective_zone(cx, request.player, system, dock, range);
        if zone == 0 {
            return (result::NO_OBJECTIVE, key);
        }
        add_unique(&mut cx.logic.content.state.rm.objective_zones, zone);
        let dest_sys = api::zone::system(cx, zone);
        let position = api::zone::pos(cx, zone);
        self.update(cx, |m| {
            m.dest_sys = dest_sys;
            m.objective_zone = zone;
            m.objective_pos = position;
        });
        let mut enemy = if cx.logic.content.state.systems.contains_key(&dest_sys) {
            pick_enemy_group(cx, dest_sys, position, request.offer_group)
        } else {
            u32::MAX
        };
        if enemy == u32::MAX {
            let base_system = api::misc::base_system(cx, request.offer_base).unwrap_or(0);
            enemy = if cx.logic.content.state.systems.contains_key(&base_system) {
                pick_enemy_group(cx, base_system, Vector::ZERO, request.offer_group)
            } else {
                u32::MAX
            };
        }
        self.update(cx, |m| m.enemy_group = enemy);
        if enemy != u32::MAX {
            return (self.finalize_mission(cx), key);
        }
        (result::NO_ENEMY, key)
    }

    fn update(&self, cx: &mut Cx<'_>, change: impl FnOnce(&mut MissionData)) {
        if let Some(m) = self
            .mission
            .and_then(|k| cx.logic.content.state.rm.missions.get_mut(k))
        {
            change(m);
        }
    }

    /// `PickObjectiveZone` (`:952-997`): candidates from `range - 10` (a
    /// float) around `position`, along trade lanes and through jumps; one
    /// survives when no other mission holds it and the offering faction's
    /// legality matches the zone's. One is picked; 0 when its route from
    /// `system` is blocked, or when none survived. A candidate without a
    /// record or a faction without properties, which the C++ dereferences,
    /// does not survive (D951).
    fn pick_objective_zone(
        &self,
        cx: &mut Cx<'_>,
        player: u32,
        system: u32,
        position: Vector,
        range: f32,
    ) -> u32 {
        register_dest_system(cx, system);
        let mut zones = Vec::new();
        let search = (f64::from(range) - 10.0) as f32;
        self.collect_zones_in_range(cx, system, position, search, &mut zones);
        self.collect_zones_along_trade_lanes(cx, player, system, position, search, &mut zones);
        self.collect_zones_through_jumps(cx, player, system, position, search, &mut zones);
        let offer_group = self.field(cx, |m| m.offer_group) as u32;
        let legality = cx
            .logic
            .content
            .tables
            .faction_props(offer_group)
            .map(|p| p.legality);
        let mut kept = Vec::new();
        for zone in zones {
            let zone_system = api::zone::system(cx, zone);
            let record = cx.logic.content.state.systems.get(&zone_system);
            let z = record.and_then(|r| r.find_zone(zone).map(|i| &r.zones[i]));
            let keep = !cx.logic.content.state.rm.objective_zones.contains(&zone)
                && match (legality, z) {
                    (Some(l), Some(z)) => {
                        (l == crate::content::tables::factions::legality::LAWFUL && z.lawful)
                            || (l == crate::content::tables::factions::legality::UNLAWFUL
                                && z.unlawful)
                    }
                    _ => false,
                };
            if keep {
                kept.push(zone);
            }
        }
        if kept.is_empty() {
            return 0;
        }
        let picked = pick_random_id(cx, &kept);
        let to = api::zone::system(cx, picked);
        if self.is_route_blocked(cx, system, to, player) {
            return 0;
        }
        picked
    }

    /// `CollectZonesInRange` (`:778-785`): a range above 0 collects the
    /// mission zones within `range * 300`.
    #[allow(clippy::unused_self)]
    fn collect_zones_in_range(
        &self,
        cx: &mut Cx<'_>,
        system: u32,
        position: Vector,
        range: f32,
        out: &mut Vec<u32>,
    ) {
        if !(range > 0.0) {
            return;
        }
        let radius = (f64::from(range) * f64::from(RANGE_SCALE)) as f32;
        collect_mission_zones(cx, system, position, radius, out);
    }

    /// `CollectZonesAlongTradeLanes` (`:794-863`): above a range of 10, for
    /// every trade lane passing within `range * 300`, entered at its nearer
    /// end, the zones around its other end when the range left there is
    /// above 0, then the lanes and jumps from that end.
    fn collect_zones_along_trade_lanes(
        &self,
        cx: &mut Cx<'_>,
        player: u32,
        system: u32,
        position: Vector,
        range: f32,
        out: &mut Vec<u32>,
    ) {
        if !(range > 10.0) {
            return;
        }
        let radius = (f64::from(range) * f64::from(RANGE_SCALE)) as f32;
        let lanes: Vec<(Vector, Vector)> = match cx.logic.content.state.systems.get(&system) {
            Some(record) => trade_lanes_near(record, position, radius)
                .into_iter()
                .map(|i| (record.trade_lanes[i].start, record.trade_lanes[i].end))
                .collect(),
            None => Vec::new(),
        };
        for (start, end) in lanes {
            let start_sq = distance_squared_f32(start, position);
            let end_sq = distance_squared_f32(end, position);
            // `3DMathEngine::Sqrt` of the float.
            let (distance, exit) = if start_sq < end_sq {
                (f64::from(start_sq).sqrt() as f32, end)
            } else {
                (f64::from(end_sq).sqrt() as f32, start)
            };
            if !(distance > 0.0) {
                continue;
            }
            let remaining = f64::from(range) - f64::from(distance) / f64::from(RANGE_SCALE) - 10.0;
            if !(remaining > 0.0) {
                continue;
            }
            let exit_exact = remaining - 10.0;
            let exit_range = exit_exact as f32;
            if exit_exact > 0.0 {
                let r = (f64::from(exit_range) * f64::from(RANGE_SCALE)) as f32;
                collect_mission_zones(cx, system, exit, r, out);
            }
            self.collect_zones_along_trade_lanes(cx, player, system, exit, exit_range, out);
            self.collect_zones_through_jumps(cx, player, system, exit, exit_range, out);
        }
    }

    /// `CollectZonesThroughJumps` (`:872-945`): above a range of 45, for
    /// every jump of the faction's jump type at a distance D > 0 with
    /// `range - D / 300 - 45 > 0` whose gate is not locked for the player,
    /// the far system is registered and its jump back is the arrival
    /// point; 10 less is the range there, for its zones and then its lanes
    /// and jumps. A far system without a record, and an offering faction
    /// without properties (any jump type then), which the C++ dereferences,
    /// are D951's.
    fn collect_zones_through_jumps(
        &self,
        cx: &mut Cx<'_>,
        player: u32,
        system: u32,
        position: Vector,
        range: f32,
        out: &mut Vec<u32>,
    ) {
        if !(range > 45.0) {
            return;
        }
        let offer_group = self.field(cx, |m| m.offer_group) as u32;
        let preference = cx
            .logic
            .content
            .tables
            .faction_props(offer_group)
            .map_or(0, |p| p.jump_preference);
        let jumps = cx
            .logic
            .content
            .state
            .systems
            .get(&system)
            .map(|r| r.jumps.clone())
            .unwrap_or_default();
        for jump in jumps {
            let dx = (f64::from(position.x) - f64::from(jump.position.x)) as f32;
            let dy = (f64::from(position.y) - f64::from(jump.position.y)) as f32;
            let dz = (f64::from(position.z) - f64::from(jump.position.z)) as f32;
            let (dx, dy, dz) = (f64::from(dx), f64::from(dy), f64::from(dz));
            let distance_exact = (dz * dz + dy * dy + dx * dx).sqrt();
            let distance = distance_exact as f32;
            if !(distance_exact > 0.0) {
                continue;
            }
            if preference != 0 && preference != jump.kind {
                continue;
            }
            let remaining_exact =
                f64::from(range) - f64::from(distance) / f64::from(RANGE_SCALE) - 45.0;
            let remaining = remaining_exact as f32;
            if !(remaining_exact > 0.0) {
                continue;
            }
            if cx
                .logic
                .content
                .state
                .gate_locks
                .is_locked(player, jump.goto_object)
            {
                continue;
            }
            if !cx
                .logic
                .content
                .state
                .systems
                .contains_key(&jump.goto_system)
            {
                continue;
            }
            register_dest_system(cx, jump.goto_system);
            let arrival = cx
                .logic
                .content
                .state
                .systems
                .get(&jump.goto_system)
                .and_then(|t| t.jumps.iter().find(|b| b.object == jump.goto_object))
                .map(|b| b.position);
            let Some(arrival) = arrival else {
                continue;
            };
            if arrival.x == 1e10 || arrival.y == 1e10 || arrival.z == 1e10 {
                continue;
            }
            let arrival_exact = f64::from(remaining) - 10.0;
            let arrival_range = arrival_exact as f32;
            if arrival_exact > 0.0 {
                let r = (f64::from(arrival_range) * f64::from(RANGE_SCALE)) as f32;
                collect_mission_zones(cx, jump.goto_system, arrival, r, out);
            }
            self.collect_zones_along_trade_lanes(
                cx,
                player,
                jump.goto_system,
                arrival,
                arrival_range,
                out,
            );
            self.collect_zones_through_jumps(
                cx,
                player,
                jump.goto_system,
                arrival,
                arrival_range,
                out,
            );
        }
    }

    /// `IsRouteBlocked` (`:723-752`): the same system is open; an id of 0
    /// is blocked. Otherwise the route is walked one connection at a time
    /// with the mission's jump preference: no next connection, or a gate
    /// locked for the player or for everybody, blocks it. A connection
    /// whose far system is not found, where the C++ would loop for ever,
    /// blocks it (D951).
    pub fn is_route_blocked(&self, cx: &mut Cx<'_>, from: u32, to: u32, player: u32) -> bool {
        if from == to {
            return false;
        }
        if from == 0 || to == 0 || player == 0 {
            return true;
        }
        let jump_pref = self.field(cx, |m| m.jump_pref);
        let locked = cx.logic.content.state.gate_locks.collect(player);
        let paths = std::mem::take(&mut cx.logic.content.state.system_paths);
        let take = NoDispatch::begin(&cx.logic.content);
        let mut current = from;
        let mut blocked = false;
        // A route visits each system at most once.
        let mut steps = 0usize;
        while current != to {
            let connection =
                system_paths::next_hop_connection_object(cx, &paths, current, to, jump_pref);
            if connection == 0 || locked.contains(&connection) {
                blocked = true;
                break;
            }
            match system_paths::connection_goto_system(cx, current, connection) {
                Some(next) if steps < 0x10000 => current = next,
                _ => {
                    blocked = true;
                    break;
                }
            }
            steps += 1;
        }
        take.end(&cx.logic.content);
        cx.logic.content.state.system_paths = paths;
        blocked
    }

    /// `GetOfferDescription` (`:696-716`): the offer base, the class (the
    /// first offer's type + 1), the title (mad lib 0x734 with the
    /// *loader's* mission's system), the description (mad lib 0x72a with
    /// the offering group's name), the briefing (the mission's text), the
    /// reward in the icon field. The target and system ids are not
    /// written. The second value is the player filter. A mission without
    /// an offer, whose first offer the C++ reads through the list's head,
    /// has class 0 (D951).
    #[must_use]
    pub fn offer_description(
        &self,
        cx: &Cx<'_>,
        mission: RmDataKey,
        out: &mut MissionVendorProperties,
    ) -> u32 {
        let own = self.data(cx);
        let data = cx
            .logic
            .content
            .state
            .rm
            .missions
            .get(mission)
            .cloned()
            .unwrap_or_default();
        out.base_id = data.offer_base;
        out.mission_class = data
            .offers
            .first()
            .map_or(0, |o| u32::from(o.kind).wrapping_add(1));
        out.title = FmtStr::new(0x734);
        out.title.append_system(own.dest_sys);
        out.description = FmtStr::new(0x72a);
        let name = api::reputation::group_name(cx, data.offer_group as u32).unwrap_or(0);
        out.description.append_string(name);
        out.briefing = data.description.clone();
        out.icon_texture_id = data.reward as u32;
        data.player
    }

    /// `CanAccept` (`:756-772`): the offer base's system against the
    /// mission's; another system whose route is blocked gives mad lib
    /// 0x740 and code 2.
    ///
    /// # Errors
    ///
    /// The reason and the code when the route is blocked.
    pub fn can_accept(&self, cx: &mut Cx<'_>) -> Result<(), (FmtStr, i32)> {
        let data = self.data(cx);
        let offer_system = api::misc::base_system(cx, data.offer_base).unwrap_or(0);
        if offer_system != data.dest_sys
            && self.is_route_blocked(cx, offer_system, data.dest_sys, data.player)
        {
            return Err((FmtStr::new(0x740), 2));
        }
        Ok(())
    }
}

/// `FContentBase::FindDockObject` (`BaseDB.cpp:1330-1348`): the position
/// of the first object of the base's system that docks with it; zero
/// without one, as the reconstruction leaves `CreateMission`'s local
/// (D954). A base without a record or a system, which the C++
/// dereferences, has none (D951).
pub fn find_dock_object(cx: &mut Cx<'_>, base: u32) -> Vector {
    if !cx.logic.content.state.bases.contains_key(&base) {
        return Vector::ZERO;
    }
    let Ok(system) = api::misc::base_system(cx, base) else {
        return Vector::ZERO;
    };
    if !cx.logic.content.state.systems.contains_key(&system) {
        return Vector::ZERO;
    }
    let source = data_source(cx);
    let mut found = Vector::ZERO;
    let _ = api::system::enumerate_objects(cx, &*source, system, |object| {
        if object.dock_with != base {
            return true;
        }
        found = object.pos;
        false
    });
    found
}

/// `PickEnemyGroup` (`:1000-1027`): the hostile groups populating the
/// zones within 7500 of `position`; without any, those of every zone of
/// the system. One is picked, -1 without any.
pub fn pick_enemy_group(cx: &mut Cx<'_>, system: u32, position: Vector, offer_group: i32) -> u32 {
    register_dest_system(cx, system);
    let mut groups = Vec::new();
    let zones = api::system::in_zones(cx, system, position, 7500.0, 0x100).unwrap_or_default();
    for zone in zones {
        collect_hostile_groups(cx, zone, offer_group, &mut groups);
    }
    if groups.is_empty() {
        let source = data_source(cx);
        let mut all = Vec::new();
        let _ = api::system::enumerate_zones(cx, &*source, system, |id, _| all.push(id));
        for zone in all {
            collect_hostile_groups(cx, zone, offer_group, &mut groups);
        }
    }
    if groups.is_empty() {
        return u32::MAX;
    }
    pick_random_id(cx, &groups)
}

/// `ReleaseRmDestSystems` (`RandomMissionDB.cpp:414-422`): every listed
/// system's record unloaded once, in order, and the list emptied.
pub fn release_dest_systems(cx: &mut Cx<'_>) {
    let listed = std::mem::take(&mut cx.logic.content.state.rm.dest_systems);
    for system in listed {
        if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
            systems::unload(record);
        }
    }
}

/// `ClampRmDifficulty` (`RandomMissionDB.cpp:191-199`).
fn clamp_difficulty(value: f64) -> f32 {
    let low = if 0.0 > value { 0.0 } else { value };
    if !(low < 100.0) {
        return 100.0;
    }
    if 0.0 > value {
        return 0.0;
    }
    value as f32
}

/// `RandomRmDifficultySpread` (`:203-207`): `cos(rand() / 32768 *
/// (float)(pi / 2))`.
fn difficulty_spread(cx: &mut Cx<'_>) -> f64 {
    let r = f64::from(cx.world.rng().rand());
    (r * f64::from(1.0f32 / 32768.0) * 1.570_796_370_506_286_6).cos()
}

/// `PickRmMissionDifficulty(Player, Min, Max)` (`RandomMissionDB.cpp:216-292`);
/// `pow` and `cos` through `f64` (D957).
/// In multiplayer, two times out of three the top grows by 1.3689 to the
/// power 2, 4 or 6; the result is `max - spread * (max - min)`. In single
/// player the window around the player's stage difficulty decides, and 0
/// means no mission. An empty stage table, which the C++ reads through
/// `begin()`, gives a stage difficulty of 0 (D951).
pub fn pick_mission_difficulty(cx: &mut Cx<'_>, player: u32, min: f32, max: f32) -> f32 {
    let step = f64::from(1.3689f32);
    let mut max = max;
    if !api::misc::single_player(cx) {
        if cx.world.rng().rand() % 3 != 0 {
            let exponent = (cx.world.rng().rand() % 3) * 2 + 2;
            max = (step.powf(f64::from(exponent)) * f64::from(max)) as f32;
        }
        max = clamp_difficulty(f64::from(max));
        let spread = difficulty_spread(cx);
        return (f64::from(max) - spread * (f64::from(max) - f64::from(min))) as f32;
    }
    let stage = cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map_or(0, |p| p.mission_number) as i32;
    let table = &cx.logic.content.tables.rank_difficulties;
    let mut previous = table.first().map_or(0.0, |r| r.difficulty);
    let mut stage_difficulty = None;
    for r in table {
        if r.stage == stage {
            stage_difficulty = Some(r.difficulty);
            break;
        }
        if r.stage > stage {
            break;
        }
        previous = r.difficulty;
    }
    let stage_difficulty = stage_difficulty.unwrap_or(previous);
    let window = step.powf(3.0);
    let window_low = clamp_difficulty(f64::from(stage_difficulty) / window);
    let window_high = clamp_difficulty(f64::from(stage_difficulty) * window);
    if window_high <= min {
        return 0.0;
    }
    if window_high >= max {
        let mut low = (f64::from(max) / window) as f32;
        if min > low {
            low = min;
        }
        let spread = difficulty_spread(cx);
        return (f64::from(max) - spread * (f64::from(max) - f64::from(low))) as f32;
    }
    let mut low = window_low;
    if low < min {
        low = min;
    }
    let spread = difficulty_spread(cx);
    (f64::from(window_high) - spread * (f64::from(window_high) - f64::from(low))) as f32
}

/// `FContentBase::FindDockObject` as the critical-loot state reads it
/// (`BaseDB.cpp:1330-1348`, `RandomMissionVignette.cpp:1494-1500`): the
/// nickname id (`pub::GetNicknameId`) of the first object of the base's
/// system that docks with it, 0 without one; a base without a record or
/// a system has none (D951).
pub fn find_dock_object_id(cx: &mut Cx<'_>, base: u32) -> u32 {
    if !cx.logic.content.state.bases.contains_key(&base) {
        return 0;
    }
    let Ok(system) = api::misc::base_system(cx, base) else {
        return 0;
    };
    if !cx.logic.content.state.systems.contains_key(&system) {
        return 0;
    }
    let source = data_source(cx);
    let mut found = 0;
    let _ = api::system::enumerate_objects(cx, &*source, system, |object| {
        if object.dock_with != base {
            return true;
        }
        found = api::misc::nickname_id(&object.nickname);
        false
    });
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_section_is_written_as_the_cpp_writes_it() {
        let data = MissionData {
            player: 7,
            dest_sys: 123,
            offer_group: 4567,
            enemy_group: u32::MAX,
            offer_base: 89,
            reward: 12_000,
            rep_reward: 0.15,
            rep_hit: -0.15,
            difficulty: 1.687_204_1,
            objective_pos: Vector::new(-12_345.678, 0.5, 1e7),
            objective_zone: 42,
            random_seed: 31_337,
            ..MissionData::default()
        };
        let mut out = Vec::new();
        write_section(&data, &mut out).expect("in memory");
        let text = String::from_utf8(out).expect("ascii");
        assert_eq!(
            text,
            "[RandomMission]\ntype = DestroyMission\noffer_group = 4567\noffer_base = 89\n\
             offer_ids = 0\nreward = 12000\nrep_reward = 0.15\nrep_hit = -0.15\n\
             difficulty = 1.6872\njump_pref = 0\nenemy_group = 4294967295\ndest_sys = 123\n\
             objective_pos = -12345.7,0.5,1e+07\nobjective_zone = 42\nrandom_seed = 31337\n\n"
        );
    }

    #[test]
    fn a_reload_reads_the_position_as_integers_and_lists_the_zone() {
        let text = b"[RandomMission]\ntype = DestroyMission\ndifficulty = 1.6872\n\
                     objective_pos = -12345.7,0.5,1e+07\nobjective_zone = 42\n\
                     enemy_group = 4294967295\nrandom_seed = 31337\n";
        let mut reader = IniReader::new();
        reader.open_memory(text);
        let mut zones = vec![42, 7];
        let mut data = MissionData::default();
        assert!(reader.read_header());
        while reader.read_value() {
            data.load_value(&mut zones, &mut reader);
        }
        assert_eq!(data.difficulty, 1.6872f32);
        assert_eq!(data.objective_pos, Vector::new(-12_345.0, 0.0, 1.0));
        assert_eq!(data.objective_zone, 42);
        assert_eq!(zones, [42, 7]);
        assert_eq!(data.enemy_group, u32::MAX);
        assert_eq!(data.random_seed, 31_337);
    }

    #[test]
    fn the_clamp_keeps_nan_at_the_top() {
        assert_eq!(clamp_difficulty(-1.0), 0.0);
        assert_eq!(clamp_difficulty(100.0), 100.0);
        assert_eq!(clamp_difficulty(f64::NAN), 100.0);
        assert_eq!(clamp_difficulty(42.5), 42.5);
    }
}
