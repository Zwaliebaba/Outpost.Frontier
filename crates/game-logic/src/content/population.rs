//! The populators, SpacePop and BasePop (`Content/Populators.cpp`; record
//! 0008 points 2 and 7). SpacePop's handler is P3.6d's ([`handle_space_pop`]):
//! the heartbeat population around the players' ships, the arrival
//! population where a ship launches, jumps in or leaves a lane, and the
//! lanes' attacks; over the encounters ([`super::encounters`]), their
//! placement ([`super::spawn_placement`]) and the random encounters
//! ([`super::random_encounters`]). BasePop's handler fills the rooms a
//! player enters with the base's GF_NPCs ([`handle_base_pop`]) over
//! BaseSupport's bar characters ([`super::bar`]).
//!
//! - **SpacePop** (`FSpacePop::FSpacePop`, `:237-241`): the controller's id
//!   into `GSpacePopControllerId`, then a 3 s heartbeat (one `rand()`). No
//!   destructor.
//! - **BasePop** (`FBasePop::FBasePop`, `:970-977`): a heartbeat of -1 (none;
//!   the bus draws its `rand()` all the same), then the instance into the
//!   process-wide `GBasePopInstance`. No destructor: the pointer is never
//!   cleared (D772). A second BasePop executes `int3` in the C++ (D771).
//!
//! P3.4d brings two pieces the system records need: SpacePop's solar
//! detection flag (`SpacePopSetSolarDetection`, `SpacePopGetSolarDetection`,
//! `:1064-1076`), which a system's full load sets in single player, and
//! the zone lists of a position (`CollectSpacePopZones`, `:661-786`), which
//! the lane rings' attack zones are made from.

// The C++'s `!(x > 0.0f)` tests send NaN to the failing side; the port
// spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use std::collections::BTreeMap;

use neuron_server::controller::ControllerId;

use neuron_core::math::{Matrix, Transform, Vector, transform_xform};

use super::Instance;
use super::bar;
use super::bases::BaseNpc;
use super::encounters::{self, ZoneRef};
use super::random_encounters;
use super::random_mission::vendor::BarOffer;
use super::spawn_placement::{self, SpawnRequest};
use super::systems::SystemInfo;
use crate::api;
use crate::events::ControllerMessage;
use crate::server::Cx;

/// SpacePop's heartbeat period in seconds (`:240`).
pub const SPACE_POP_HEARTBEAT: f32 = 3.0;

/// BasePop's heartbeat period: none (`:973`).
pub const BASE_POP_HEARTBEAT: f32 = -1.0;

/// `FBasePop`'s own state (`:796-807`, 0x1c bytes): the room characters
/// it placed for each player (`PlayerNpcs`), which its handler fills
/// (P3.6d).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BasePop {
    /// `PlayerNpcs`: by player id, the keys of the bar characters placed
    /// for them (the C++ keeps iterators into the base's bar list).
    pub player_npcs: BTreeMap<u32, Vec<u32>>,
}

/// `FSpacePop::FSpacePop(&ControllerId)`.
pub fn construct_space_pop(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    cx.logic.content.state.space_pop = id;
    super::set_heartbeat(cx, id, SPACE_POP_HEARTBEAT);
    Instance::SpacePop
}

/// `FBasePop::FBasePop(ControllerId)`. A second BasePop becomes the
/// instance, as the C++ continues after its breakpoint (D771).
pub fn construct_base_pop(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    super::set_heartbeat(cx, id, BASE_POP_HEARTBEAT);
    cx.logic.content.state.base_pop = Some(id);
    Instance::BasePop(BasePop::default())
}

/// `GBasePopInstance` as the C++ reads it (`BasePopHasPlayer`,
/// `:1043-1049`): the BasePop created last, when its controller still has
/// an instance; a torn-down BasePop reads as none (D772).
#[must_use]
pub fn base_pop<'a>(cx: &'a Cx<'_>) -> Option<&'a BasePop> {
    let content = &cx.logic.content;
    match content.instance(content.state.base_pop?) {
        Some(Instance::BasePop(pop)) => Some(pop),
        _ => None,
    }
}

/// `BasePopHasPlayer(PlayerId)` (`:1043-1049`): the BasePop instance
/// exists and tracks room characters for the player.
#[must_use]
pub fn base_pop_has_player(cx: &Cx<'_>, player: u32) -> bool {
    base_pop(cx).is_some_and(|pop| pop.player_npcs.contains_key(&player))
}

/// `SpacePopSetSolarDetection(Enabled)`: stored in single player only.
pub fn set_solar_detection(cx: &mut Cx<'_>, enabled: bool) {
    if api::misc::single_player(cx) {
        cx.logic.content.state.solar_detection = enabled;
    }
}

/// `SpacePopGetSolarDetection`: the stored flag in single player, else on.
#[must_use]
pub fn solar_detection(cx: &Cx<'_>) -> bool {
    !api::misc::single_player(cx) || cx.logic.content.state.solar_detection
}

/// The zone lists of `CollectSpacePopZones`, each zone by its index in the
/// system record's zones.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SpacePopZones {
    /// The populated zones within 10 of the position: those with a patrol
    /// path, and the smallest of the others at a random place.
    pub at_ship: Vec<usize>,
    /// The `population_additive` zones within 2000 not in the first list.
    pub near: Vec<usize>,
    /// The `population_additive` zones of the view ahead within 4000.
    pub ahead: Vec<usize>,
    /// A zone at the position has density 0: the population is blocked
    /// and the other lists are empty.
    pub blocked: bool,
}

/// `CollectSpacePopZones(System, Placement, ...)` (`:661-786`).
///
/// - **At the ship**: the zones `pub::System::InZones` finds within 10 (at
///   most 20). A populated zone with a patrol path is taken; of the
///   others only the one with the smallest radius (0 for a lane zone; the
///   first on a tie) survives, the rest's ids zeroed in the found list. The
///   survivor goes to a random place of the list (`rand() * (n + 1) /
///   0x8000`).
/// - A zone at the ship with density 0 blocks the population.
/// - **Near**: the zones within 2000 not in the first found list that are
///   populated and `population_additive`.
/// - **Ahead**: the zones of the view frustum (1.2217305 by 0.9670910
///   radians, 4000 deep) in neither found list whose surface is within
///   4000, populated and `population_additive`.
pub fn collect_space_pop_zones(
    cx: &mut Cx<'_>,
    record: &SystemInfo,
    orientation: Matrix,
    position: Vector,
) -> SpacePopZones {
    let mut out = SpacePopZones::default();
    let system = record.system;
    let mut at_ids = api::system::in_zones(cx, system, position, 10.0, 0x14).unwrap_or_default();
    let mut best: Option<usize> = None;
    let mut best_radius = -1.0_f32;
    for index in 0..at_ids.len() {
        let Some(z) = record.find_zone(at_ids[index]) else {
            continue;
        };
        let zone = &record.zones[z];
        if zone.unpopulated {
            continue;
        }
        if zone.path.is_some() {
            out.at_ship.push(z);
            continue;
        }
        let radius = if zone.tradelane {
            0.0
        } else {
            api::zone::radius(cx, zone.zone_id)
        };
        if best_radius == -1.0 || best_radius > radius {
            if let Some(b) = best {
                at_ids[b] = 0;
            }
            best_radius = radius;
            best = Some(index);
        } else {
            at_ids[index] = 0;
        }
    }
    if let Some(b) = best {
        let count = out.at_ship.len() as i32;
        let slot = cx.world.rng().rand().wrapping_mul(count + 1) / 0x8000;
        if let Some(z) = record.find_zone(at_ids[b]) {
            let slot = usize::try_from(slot).unwrap_or(0).min(out.at_ship.len());
            out.at_ship.insert(slot, z);
        }
    }
    if out
        .at_ship
        .iter()
        .any(|&z| record.zones[z].population.density == 0)
    {
        out.blocked = true;
        return out;
    }
    let near_ids = api::system::in_zones(cx, system, position, 2000.0, 0x14).unwrap_or_default();
    for &id in &near_ids {
        if at_ids.contains(&id) {
            continue;
        }
        if let Some(z) = record.find_zone(id) {
            let zone = &record.zones[z];
            if !zone.unpopulated && zone.population.population_additive {
                out.near.push(z);
            }
        }
    }
    let view = Transform {
        rotation: orientation,
        translation: position,
    };
    let angle_x = f32::from_bits(0x3f9c_61aa);
    let angle_y = f32::from_bits(0x3f77_954d);
    let ahead_ids = api::system::in_zones_frustum(cx, system, view, angle_x, angle_y, 4000.0, 0x14)
        .unwrap_or_default();
    for &id in &ahead_ids {
        if at_ids.contains(&id) || near_ids.contains(&id) {
            continue;
        }
        let Some(z) = record.find_zone(id) else {
            continue;
        };
        if !(api::zone::distance(cx, id, position) <= 4000.0) {
            continue;
        }
        let zone = &record.zones[z];
        if zone.unpopulated || !zone.population.population_additive {
            continue;
        }
        out.ahead.push(z);
    }
    out
}

/// `SpacePopAddExclusionSphere` (`Content/Populators.cpp:1161-1184`): a
/// sphere is kept unless one within 0.01 of each coordinate (squared
/// differences at most `0.0001f`) and less than 1 apart in radius is there.
pub fn add_exclusion_sphere(cx: &mut Cx<'_>, sphere: [f32; 4]) {
    let spheres = &mut cx.logic.content.state.exclusion_spheres;
    if spheres.iter().any(|s| same_sphere(s, &sphere)) {
        return;
    }
    spheres.push(sphere);
}

/// `SpacePopRemoveExclusionSphere` (`Content/Populators.cpp:1186-1209`):
/// the first sphere [`add_exclusion_sphere`] would call the same.
pub fn remove_exclusion_sphere(cx: &mut Cx<'_>, sphere: [f32; 4]) {
    let spheres = &mut cx.logic.content.state.exclusion_spheres;
    if let Some(at) = spheres.iter().position(|s| same_sphere(s, &sphere)) {
        spheres.remove(at);
    }
}

/// The sphere test the two share, in `f64` as the x87 runs it.
fn same_sphere(known: &[f32; 4], sphere: &[f32; 4]) -> bool {
    let limit = f64::from(0.0001f32);
    for axis in 0..3 {
        let d = f64::from(known[axis]) - f64::from(sphere[axis]);
        if !(d * d <= limit) {
            return false;
        }
    }
    let d = f64::from(known[3]) - f64::from(sphere[3]);
    d < 1.0 && d > -1.0
}

// ---- SpacePop's run time (P3.6d) ----------------------------------------------------------------

/// `SpacePopPositionIsExcluded(Position)` (`:1120-1148`): the point is
/// inside an exclusion sphere (its distance, from the refined estimate of
/// the square stored as a float, below the radius).
#[must_use]
pub fn position_is_excluded(cx: &Cx<'_>, position: Vector) -> bool {
    cx.logic.content.state.exclusion_spheres.iter().any(|s| {
        let delta = Vector::new(s[0] - position.x, s[1] - position.y, s[2] - position.z);
        let square = ((f64::from(delta.z) * f64::from(delta.z)
            + f64::from(delta.y) * f64::from(delta.y))
            + f64::from(delta.x) * f64::from(delta.x)) as f32;
        let length = spawn_placement::fast_reciprocal_sqrt(square) * f64::from(square);
        length < f64::from(s[3])
    })
}

/// `ZoneDensity`: the zone's density, read unsigned.
fn zone_density(record: &SystemInfo, zone: usize) -> u32 {
    record.zones[zone].population.density as u32
}

/// `ftol(density * 0.6f)` of a near zone.
fn near_density(record: &SystemInfo, zone: usize) -> u32 {
    (f64::from(zone_density(record, zone)) * f64::from(0.6f32)) as i64 as u32
}

/// `ComputeZoneShares(AtShip, Near, ...)` (`:127-192`): each zone's share of
/// the population. The weight is the largest density (a near zone's at 0.6)
/// over the zone's repop time times the average repop time (float stores);
/// the share is the weight over the sum of the weights (0 for a sum of 0).
fn zone_shares(record: &SystemInfo, at_ship: &[usize], near: &[usize]) -> (Vec<f32>, Vec<f32>) {
    let repop = |z: usize| record.zones[z].population.repop_time;
    let mut repop_sum = 0.0f32;
    let mut max_density = 0u32;
    for &z in at_ship {
        repop_sum = (f64::from(repop(z)) + f64::from(repop_sum)) as f32;
        max_density = max_density.max(zone_density(record, z));
    }
    for &z in near {
        repop_sum = (f64::from(repop(z)) + f64::from(repop_sum)) as f32;
        max_density = max_density.max(near_density(record, z));
    }
    let count = (at_ship.len() + near.len()) as u32;
    let average = if count == 0 {
        0.0
    } else {
        (f64::from(repop_sum) / f64::from(count)) as f32
    };
    let max_f = f64::from(max_density) as f32;
    let mut weights = Vec::with_capacity(count as usize);
    let mut weight_sum = 0.0f32;
    for &z in at_ship.iter().chain(near) {
        let w = (f64::from(max_f) / f64::from(repop(z)) * f64::from(average)) as f32;
        weights.push(w);
        weight_sum = (f64::from(w) + f64::from(weight_sum)) as f32;
    }
    let shares: Vec<f32> = weights
        .iter()
        .map(|&w| {
            if weight_sum != 0.0 {
                (f64::from(w) / f64::from(weight_sum)) as f32
            } else {
                0.0
            }
        })
        .collect();
    let (a, n) = shares.split_at(at_ship.len());
    (a.to_vec(), n.to_vec())
}

/// The zone lists of a placement in system `system`
/// ([`collect_space_pop_zones`] on the record taken out of its slot).
fn zones_at(
    cx: &mut Cx<'_>,
    system: u32,
    orientation: Matrix,
    position: Vector,
) -> Option<SpacePopZones> {
    super::systems::with_system_taken(cx, system, |cx, record| {
        collect_space_pop_zones(cx, record, orientation, position)
    })
}

/// The request of the population around `player`'s ship.
#[allow(clippy::too_many_arguments)]
fn request(
    cx: &Cx<'_>,
    arrival: bool,
    zone_list: u32,
    player: u32,
    orientation: Matrix,
    position: Vector,
    velocity: Vector,
) -> SpawnRequest {
    let state = &cx.logic.content.state;
    SpawnRequest {
        arrival,
        zone_list,
        controller: state.space_pop.0,
        player,
        orientation,
        position,
        velocity,
        spheres: state.exclusion_spheres.clone(),
    }
}

/// `FSpacePop::PopulateAround(System, Ship, &Position)` (`:258-283`): the
/// arrival population around a point outside the exclusion spheres
/// (identity orientation), from the zones at the point and near it
/// ([`populate_arrival`]) unless the population there is blocked.
pub fn populate_around(cx: &mut Cx<'_>, system: u32, player: u32, position: Vector) {
    if position_is_excluded(cx, position) {
        return;
    }
    let Some(zones) = zones_at(cx, system, Matrix::IDENTITY, position) else {
        return;
    };
    if !zones.blocked {
        populate_arrival(
            cx,
            system,
            player,
            Matrix::IDENTITY,
            position,
            &zones.at_ship,
            &zones.near,
        );
    }
}

/// `FSpacePop::PopulateAtLaneEnd(System, Lane, &Ring, Ship)` (`:286-318`):
/// [`populate_around`] the point 1200 beyond the ring, away from its
/// neighbour (the second ring for the first, else the previous one).
pub fn populate_at_lane_end(cx: &mut Cx<'_>, system: u32, rings: &[u32], ring: usize, player: u32) {
    let other = if ring == 0 {
        rings.get(1).copied()
    } else {
        rings.get(ring - 1).copied()
    };
    let (Some(&ring_id), Some(other)) = (rings.get(ring), other) else {
        return;
    };
    let (r, _) = api::space_obj::location(cx, ring_id).unwrap_or((Vector::ZERO, Matrix::IDENTITY));
    let (o, _) = api::space_obj::location(cx, other).unwrap_or((Vector::ZERO, Matrix::IDENTITY));
    let dx = f64::from(r.x) - f64::from(o.x);
    let dy = f64::from(r.y) - f64::from(o.y);
    let dz = f64::from(r.z) - f64::from(o.z);
    let dz_stored = dz as f32;
    let length_sq = dz * f64::from(dz_stored) + dy * dy + dx * dx;
    let inv = (1.0 / length_sq.sqrt()) as f32;
    let scaled_x = (f64::from((dx * f64::from(inv)) as f32) * 1200.0) as f32;
    let scaled_y = (dy * f64::from(inv) * 1200.0) as f32;
    let target = Vector::new(
        (f64::from(scaled_x) + f64::from(r.x)) as f32,
        (f64::from(scaled_y) + f64::from(r.y)) as f32,
        (f64::from(dz_stored) * f64::from(inv) * 1200.0 + f64::from(r.z)) as f32,
    );
    populate_around(cx, system, player, target);
}

/// `FSpacePop::PopulatePlayers()` (`:321-349`): the heartbeat population
/// around every player's ship that is not in a lane, in player order, from
/// the zones at it, near it and (single player only) ahead of it
/// ([`populate_zones`]), unless the population there is blocked.
pub fn populate_players(cx: &mut Cx<'_>) {
    let players: Vec<u32> = cx.logic.content.state.players.keys().copied().collect();
    for player in players {
        let Some(state) = cx.logic.content.state.players.get(&player) else {
            continue;
        };
        let Some(ship) = state.ship.as_ref() else {
            continue;
        };
        if ship.lane_entry != 0 {
            continue;
        }
        let ship_id = ship.ship;
        let Some(system) = state.system else {
            continue;
        };
        let (position, orientation) =
            api::space_obj::location(cx, ship_id).unwrap_or((Vector::ZERO, Matrix::IDENTITY));
        let (linear, _) =
            api::space_obj::motion(cx, ship_id).unwrap_or((Vector::ZERO, Vector::ZERO));
        let Some(mut zones) = zones_at(cx, system, orientation, position) else {
            continue;
        };
        if zones.blocked {
            continue;
        }
        if !api::misc::single_player(cx) {
            zones.ahead.clear();
        }
        populate_zones(cx, system, player, orientation, position, linear, &zones);
    }
}

/// `FSpacePop::PopulateZones(...)` (`:352-424`): one encounter at most, from
/// the first zone list that yields one. The density limit is the largest
/// at the ship, or a near zone's own when the largest is below 0.6 of it.
/// The ship's position must be outside every exclusion sphere. The zones at
/// the ship and near it populate at scale 1 up to the limit; a zone ahead
/// from a point 5000 ahead (identity orientation, no velocity) up to a
/// third of its own density.
pub fn populate_zones(
    cx: &mut Cx<'_>,
    system: u32,
    player: u32,
    orientation: Matrix,
    position: Vector,
    velocity: Vector,
    zones: &SpacePopZones,
) {
    let Some(record) = cx.logic.content.state.systems.get(&system) else {
        return;
    };
    let mut max_density = 0u32;
    for &z in &zones.at_ship {
        max_density = max_density.max(zone_density(record, z));
    }
    for &z in &zones.near {
        let density = zone_density(record, z);
        if !(f64::from(max_density) >= f64::from(density) * f64::from(0.6f32)) {
            max_density = density;
        }
    }
    let ahead_limits: Vec<u32> = zones
        .ahead
        .iter()
        .map(|&z| (f64::from(zone_density(record, z)) * f64::from(0.33f32)) as i64 as u32)
        .collect();
    let time = super::game_support::game_time(cx) as f32;
    let spheres = cx.logic.content.state.exclusion_spheres.clone();
    if !spawn_placement::test_exclusion_spheres(position, Some(&spheres), true, false) {
        return;
    }
    let mut spawned = 0;
    for (list, zone_list) in [(&zones.at_ship, 0), (&zones.near, 1)] {
        for &z in list {
            if spawned != 0 {
                break;
            }
            let req = request(
                cx,
                false,
                zone_list,
                player,
                orientation,
                position,
                velocity,
            );
            let zone = ZoneRef { system, zone: z };
            spawned = encounters::populate(cx, zone, &req, time, 1.0, max_density);
        }
    }
    for (&z, &limit) in zones.ahead.iter().zip(&ahead_limits) {
        if spawned != 0 {
            break;
        }
        let offset = Vector::new(0.0, 0.0, 2500.0 * -2.0);
        let ahead = transform_xform(
            &Transform {
                rotation: orientation,
                translation: position,
            },
            offset,
        );
        let req = request(cx, false, 2, player, Matrix::IDENTITY, ahead, Vector::ZERO);
        let zone = ZoneRef { system, zone: z };
        spawned = encounters::populate(cx, zone, &req, time, 1.0, limit);
    }
}

/// `FSpacePop::PopulateArrival(...)` (`:427-519`): the zones filled up to
/// their share of the density budget (the largest density at the ship, or
/// a near zone's at 0.6; in multiplayer less the NPC ships within 9200).
/// The position must be outside every exclusion sphere. The zones at the
/// ship, then those near, each get the budget times its share (-1 for no
/// share), at least 1, filled ([`encounters::populate_up_to`], the ship's
/// hostile contacts updated after each call) until a call makes nothing or
/// the count reaches it, while the blocks made stay below the budget.
pub fn populate_arrival(
    cx: &mut Cx<'_>,
    system: u32,
    player: u32,
    orientation: Matrix,
    position: Vector,
    at_ship: &[usize],
    near: &[usize],
) {
    let Some(record) = cx.logic.content.state.systems.get(&system) else {
        return;
    };
    let mut max_density = 0u32;
    for &z in at_ship {
        max_density = max_density.max(zone_density(record, z));
    }
    for &z in near {
        max_density = max_density.max(near_density(record, z));
    }
    let (at_shares, near_shares) = zone_shares(record, at_ship, near);
    let mut budget = max_density as i32;
    if !api::misc::single_player(cx) {
        budget -= spawn_placement::count_npc_ships_near(
            cx,
            system,
            &orientation,
            position,
            encounters::DENSITY_RADIUS_MP,
            0.0,
            None,
        );
    }
    let time = super::game_support::game_time(cx) as f32;
    let spheres = cx.logic.content.state.exclusion_spheres.clone();
    if !spawn_placement::test_exclusion_spheres(position, Some(&spheres), true, false) {
        return;
    }
    let mut spawned = 0;
    for (zones, shares) in [(at_ship, &at_shares), (near, &near_shares)] {
        for (index, &z) in zones.iter().enumerate() {
            if spawned >= budget {
                break;
            }
            let req = request(cx, true, 0, player, orientation, position, Vector::ZERO);
            let share = shares[index];
            let mut target =
                (f64::from(budget) * if share == 0.0 { -1.0 } else { f64::from(share) }) as f32;
            if !(target > 0.0) {
                continue;
            }
            if target < 1.0 {
                target = 1.0;
            }
            let limit = target as i64 as u32;
            let zone = ZoneRef { system, zone: z };
            let mut count = 0;
            loop {
                let created = encounters::populate_up_to(cx, zone, &req, time, limit);
                count += created;
                if cx
                    .logic
                    .content
                    .state
                    .players
                    .get(&player)
                    .is_some_and(|p| p.ship.is_some())
                {
                    super::combat_reputation::update_hostile_contacts(cx, player, 0.0);
                }
                if created == 0 || !((count as f32) < target) {
                    break;
                }
            }
            spawned += count;
        }
    }
}

/// `FSpacePop::HandleMessage` (`:522-608`):
/// - `Heartbeat`: [`populate_players`] with the population on;
/// - `SystemEnter` of a player with a ship from another system: the
///   arrival population at the arrival point;
/// - `SpaceEnter` (a launch): the arrival population at the ship;
/// - `TradelaneEnter` of a ship the record knows, a player's: the lane's
///   attack (with the encounters on; [`random_encounters::spawn_trade_lane_attack`])
///   and the arrival population beyond the lane's far end in the direction
///   of travel ([`populate_at_lane_end`]).
///
/// The C++ reads the player's state, a lane ring's lane and the message's
/// system record without a test; without them nothing is done (D1066).
pub fn handle_space_pop(cx: &mut Cx<'_>, _id: ControllerId, message: &mut ControllerMessage<'_>) {
    use ControllerMessage as M;
    let enabled = cx.logic.content.state.clamps.population_enabled != 0;
    match message {
        M::Heartbeat { .. } => {
            if enabled {
                populate_players(cx);
            }
        }
        M::SystemEnter {
            from,
            player,
            position,
            ..
        } => {
            if !enabled {
                return;
            }
            let Some(state) = cx.logic.content.state.players.get(player) else {
                return;
            };
            if state.ship.is_none() || *from == 0 {
                return;
            }
            let Some(system) = state.system else {
                return;
            };
            let (player, position) = (*player, *position);
            populate_around(cx, system, player, position);
        }
        M::SpaceEnter { player, .. } => {
            if !enabled {
                return;
            }
            let Some(state) = cx.logic.content.state.players.get(player) else {
                return;
            };
            let (Some(ship), Some(system)) = (state.ship.as_ref(), state.system) else {
                return;
            };
            let player = *player;
            let position = super::ships::position(cx, super::ships::ShipKey::Player(player));
            let _ = ship;
            populate_around(cx, system, player, position);
        }
        M::TradelaneEnter {
            ship,
            entrance,
            exit,
            system,
        } => {
            let (ship, entrance, exit, system) = (*ship, *entrance, *exit, *system);
            trade_lane_enter(cx, ship, entrance, exit, system, enabled);
        }
        _ => {}
    }
}

/// SpacePop's `TradelaneEnter` case (`:561-605`).
fn trade_lane_enter(
    cx: &mut Cx<'_>,
    ship: u32,
    entrance: u32,
    exit: u32,
    system: u32,
    enabled: bool,
) {
    if !cx.logic.content.state.systems.contains_key(&system) {
        return;
    }
    if super::ships::find_in_system(cx, system, ship).is_none() {
        return;
    }
    let Some(record) = cx.logic.content.state.systems.get(&system) else {
        return;
    };
    let Some(lane) = record.lane_of_ring(entrance) else {
        return;
    };
    let rings: Vec<u32> = record.trade_lanes[lane]
        .rings
        .iter()
        .map(|r| r.object)
        .collect();
    let mut entry: i32 = -1;
    let mut exit_index: i32 = -1;
    for (index, &ring) in rings.iter().enumerate() {
        if ring == entrance {
            entry = index as i32;
            if exit_index != -1 {
                break;
            }
        } else if ring == exit {
            exit_index = index as i32;
            if entry != -1 {
                break;
            }
        }
    }
    if !enabled {
        return;
    }
    let Some(player) = super::players::find_by_ship(&cx.logic.content.state.players, ship) else {
        return;
    };
    let encounters_on = cx.logic.content.state.clamps.encounter_enabled != 0;
    if (entry as u32) < (exit_index as u32) {
        if encounters_on {
            random_encounters::spawn_trade_lane_attack(cx, system, player, 1, entry, lane);
        }
        populate_at_lane_end(cx, system, &rings, rings.len().wrapping_sub(1), player);
    } else {
        if encounters_on {
            random_encounters::spawn_trade_lane_attack(cx, system, player, -1, entry, lane);
        }
        populate_at_lane_end(cx, system, &rings, 0, player);
    }
}

/// The oracle's populated scenario (`OracleSimPopulate`,
/// `Content/oracle_content_population.cpp`): `player` takes a fresh state in
/// the system's record with the ship as its ship object (as GameSupport's
/// launch makes it), then SpacePop is created and hears the launch
/// (`SpaceEnter`). SpacePop's controller id; 0 without the record.
pub fn oracle_populate(
    cx: &mut Cx<'_>,
    player: u32,
    system: u32,
    ship: u32,
    archetype: u32,
) -> ControllerId {
    if !cx.logic.content.state.systems.contains_key(&system) {
        return ControllerId::BROADCAST;
    }
    let state = super::players::PlayerState::new(cx, player);
    cx.logic.content.state.players.insert(player, state);
    super::game_support::on_system_change(cx, player, system);
    if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
        record.add_player(player);
    }
    super::game_support::make_ship_object(cx, player, ship, archetype);
    let pop = neuron_server::controller::create(
        cx,
        b"Content.dll",
        b"SpacePop",
        &crate::server::CreateParams::None,
        neuron_server::controller::Priority::Normal,
    );
    let _ = cx.dispatch(
        pop,
        &mut ControllerMessage::SpaceEnter {
            system,
            player,
            launch_object: 0,
            launch_kind: -1,
        },
    );
    pop
}

// ---- BasePop's room population (P3.6d) ----------------------------------------------------------

/// `CollectBasePopPlacements(RoomId, Out)` (`Populators.cpp:815-857`): the
/// room's character placements whose name, split at `\` and `/` (empty
/// tokens skipped, as `strtok` does), holds the tokens `zg`, `npc`, one
/// starting with `m` (any case) and `a`, in this order (`_stricmp`; other
/// tokens between are skipped); each placement once, at the `a`.
pub fn collect_base_pop_placements(cx: &mut Cx<'_>, room: u32) -> Vec<i32> {
    let mut out = Vec::new();
    let count = api::gf::num_character_placements(cx, room);
    for index in 0..count {
        let name = api::gf::character_placement_name(cx, room, index);
        let mut state = 0;
        for token in neuron_core::hash::c_str(&name)
            .split(|&b| b == b'\\' || b == b'/')
            .filter(|t| !t.is_empty())
        {
            match state {
                0 if neuron_core::hash::stricmp(token, b"zg").is_eq() => state = 1,
                1 if neuron_core::hash::stricmp(token, b"npc").is_eq() => state = 2,
                2 if token[0].eq_ignore_ascii_case(&b'm') => state = 3,
                3 if neuron_core::hash::stricmp(token, b"a").is_eq() => {
                    state = 4;
                    out.push(index as i32);
                }
                _ => {}
            }
        }
    }
    out
}

/// `PickUniform` (`:860-869`): none for an empty list, the first (no draw)
/// for one, else `rand() * n / 32768`.
fn pick_uniform(cx: &mut Cx<'_>, count: usize) -> Option<usize> {
    match count {
        0 => None,
        1 => Some(0),
        n => Some((cx.world.rng().rand().wrapping_mul(n as i32) / 32768) as usize),
    }
}

/// `CreateBasePopCharacter(Player, Base, RoomId, Placement, Npc)`
/// (`:875-904`): a bar character of the GF_NPC for the player on the
/// placement (its description, the nickname's string as the role, no
/// fidget) at the front of the room, the GF_NPC's offer for the player
/// ([`bar::choose_offer`]) and its script.
fn create_base_pop_character(
    cx: &mut Cx<'_>,
    player: u32,
    base: u32,
    room: u32,
    placement: i32,
    npc: &BaseNpc,
) -> Option<u32> {
    let d = &npc.desc;
    let mut role = if d.nickname == 0 {
        Vec::new()
    } else {
        neuron_core::id::IdString(d.nickname)
            .to_hex_string()
            .into_bytes()
    };
    role.truncate(31);
    let desc = bar::BarCharacterDesc {
        player,
        affiliation: d.affiliation,
        individual_name: d.individual_name,
        appearance: d.appearance.clone(),
        voice: d.voice,
        room,
        placement,
        nickname: d.nickname,
        role,
        ..bar::BarCharacterDesc::default()
    };
    let key = bar::add_bar_character(cx, base, &desc)?;
    let offer = bar::choose_offer(cx, npc, player);
    bar::set_offer(cx, base, key, offer);
    super::generic_scripts::start_bar_character_script(cx, base, key);
    Some(key)
}

/// `SpawnBasePopCharacters(Placements, Player, Base, RoomId, Out)`
/// (`:909-967`): the room filled up to its `character_density`, less the
/// player's characters already there, with GF_NPCs of the base's factions
/// (those of a weight above 0 and with GF_NPCs that may be in the room: a
/// GF_NPC with a room list must list it). A faction by weight, one of its
/// GF_NPCs and a placement uniformly; the placement and the GF_NPC are used
/// up, an emptied faction is dropped. After the draws (not on the early
/// returns) the random missions' destination systems are released.
///
/// The C++ reads a GF_NPC id missing from the base's map, and an empty
/// pick, without a test; such a GF_NPC may be in any room, and a pick of
/// nothing ends the filling (D1068).
pub fn spawn_base_pop_characters(
    cx: &mut Cx<'_>,
    placements: &mut Vec<i32>,
    player: u32,
    base: u32,
    room: u32,
) -> Vec<u32> {
    let mut out = Vec::new();
    let Some(record) = cx.logic.content.state.bases.get(&base) else {
        return out;
    };
    let present = record
        .room_states
        .get(&room)
        .map_or(0, |s| s.count_characters(player));
    let mut wanted = record.room_density(room) - present;
    if wanted <= 0 || placements.is_empty() {
        return out;
    }
    let npcs = record.npcs.clone();
    let mut factions: Vec<super::bases::BaseFaction> = Vec::new();
    for faction in &record.factions {
        if !(faction.weight <= 0.0) && !faction.npcs.is_empty() {
            let mut kept = faction.clone();
            kept.npcs.retain(|id| {
                npcs.get(id)
                    .is_none_or(|n| n.rooms.is_empty() || n.rooms.contains(&room))
            });
            if !kept.npcs.is_empty() {
                factions.push(kept);
            }
        }
    }
    while !factions.is_empty() && wanted > 0 && !placements.is_empty() {
        let weights: Vec<(u32, f32)> = factions.iter().map(|f| (f.group, f.weight)).collect();
        let Some(picked) = super::encounters::weighted_pick(cx, &weights) else {
            break;
        };
        let Some(npc_at) = pick_uniform(cx, factions[picked].npcs.len()) else {
            break;
        };
        let npc_id = factions[picked].npcs[npc_at];
        let Some(placement_at) = pick_uniform(cx, placements.len()) else {
            break;
        };
        let placement = placements[placement_at];
        if let Some(npc) = npcs.get(&npc_id)
            && let Some(key) = create_base_pop_character(cx, player, base, room, placement, npc)
        {
            out.push(key);
        }
        placements.remove(placement_at);
        wanted -= 1;
        factions[picked].npcs.remove(npc_at);
        if factions[picked].npcs.is_empty() {
            factions.remove(picked);
        }
    }
    super::random_mission::mission::release_dest_systems(cx);
    out
}

/// `FBasePop::HandleMessage` (`Populators.cpp:980-1040`):
/// - `LocationEnter` of a player in a base: the room's BasePop placements
///   ([`collect_base_pop_placements`]) without those the player already
///   has a character or a lock on, filled
///   ([`spawn_base_pop_characters`]); the characters made are kept for the
///   player (an existing entry stays as it is);
/// - `LocationExit` of a player BasePop keeps characters for: each loses
///   its behaviour and offer and leaves the player's base, and the entry
///   goes.
///
/// The C++ reads the leaving player's state and base without a test;
/// without them the characters are only forgotten (D1068).
pub fn handle_base_pop(cx: &mut Cx<'_>, id: ControllerId, message: &mut ControllerMessage<'_>) {
    match *message {
        ControllerMessage::LocationEnter { location, player } => {
            let Some(base) = cx
                .logic
                .content
                .state
                .players
                .get(&player)
                .and_then(|s| s.base)
            else {
                return;
            };
            if !cx.logic.content.state.bases.contains_key(&base) {
                return;
            }
            let mut placements = collect_base_pop_placements(cx, location);
            if let Some(state) = cx
                .logic
                .content
                .state
                .bases
                .get(&base)
                .and_then(|b| b.room_states.get(&location))
            {
                placements
                    .retain(|&p| !state.has_character(player, p) && !state.has_lock(player, p));
            }
            let characters = spawn_base_pop_characters(cx, &mut placements, player, base, location);
            if !characters.is_empty()
                && let Some(Instance::BasePop(pop)) = cx.logic.content.instance_mut(id)
            {
                pop.player_npcs.entry(player).or_insert(characters);
            }
        }
        ControllerMessage::LocationExit { player, .. } => {
            let Some(characters) = (match cx.logic.content.instance_mut(id) {
                Some(Instance::BasePop(pop)) => pop.player_npcs.remove(&player),
                _ => None,
            }) else {
                return;
            };
            let Some(base) = cx
                .logic
                .content
                .state
                .players
                .get(&player)
                .and_then(|s| s.base)
            else {
                return;
            };
            for key in characters {
                bar::set_behavior(cx, base, key, u32::MAX);
                bar::set_offer(cx, base, key, BarOffer::None);
                bar::remove_bar_character(cx, base, key, None);
            }
        }
        _ => {}
    }
}
