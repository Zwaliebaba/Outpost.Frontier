//! The random encounters' run time (TU31 `Content/RandomMissionDB.cpp:1762-2050`,
//! TU59 and TU60 `Content/RandomEncounters.cpp`; record 0008 point 7; work
//! package P3.6d): the encounter parameters' spawns, the two populators
//! that decide from the controller messages when an encounter fires, the
//! mission object's `NotifyRandEncs`, and the trade-lane attack SpacePop
//! starts when a player enters a lane (`FSystemInfo::SpawnTradeLaneAttack`,
//! `SystemInfo.cpp:1489-1667`).
//!
//! The parameters and their parse are P3.5a1's ([`RandEncParams`]); the
//! populator's own state (`FRandEncTLPopulator::bAttackArmed`,
//! `FRandEncSPPopulator::ElapsedTime`) lives in the parameters
//! ([`RandEncParams::populator`]), the one populator each record has.

// The C++'s `!(x > 0.0f)` tests send NaN to the failing side; the port
// spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::{Matrix, Transform, Vector, mul_matrix, transform_matrix};
use neuron_server::controller::ControllerId;

use super::encounter_params::EncounterShip;
use super::guides::{GuideDesc, PatrolPathRef, SpawnPlan};
use super::mission::obj_list::{Delay, DirectiveKind, Goto};
use super::mission::rand_enc::{RandEncFormation, RandEncKind, RandEncParams, ShipChoice};
use super::mission::spawn::place_facing_anchor;
use super::npc_names::{self, NpcNameParams, NpcNameRequest};
use super::npc_ships::{self, NpcShipParams};
use super::{cargo, systems};
use crate::api;
use crate::events::ControllerMessage;
use crate::server::Cx;

/// `GRandEncTurnAngle` and `GRandEncLaneTurnAngle`: `(double)(float)pi`.
const TURN_ANGLE: f64 = 3.141_592_741_012_573_2;

/// The 180° turn of a spawned formation towards its target: rows
/// `{cos, 0, sin}`, `{0, 1, 0}`, `{-sin, 0, cos}`.
fn turn_matrix() -> Matrix {
    let c = TURN_ANGLE.cos() as f32;
    let s = TURN_ANGLE.sin() as f32;
    Matrix::from_rows([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]])
}

/// `RandEncPickWeighted` (`RandomEncounters.h:278-298`): the weights summed
/// into a float; a zero sum picks nothing (no draw); else `R = sum *
/// (rand() * 2^-15)` stored as a float, and the first element whose running
/// float sum reaches it. `None` past the end.
pub fn pick_weighted(cx: &mut Cx<'_>, weights: &[f32]) -> Option<usize> {
    let mut sum = 0.0f32;
    for &w in weights {
        sum = (f64::from(sum) + f64::from(w)) as f32;
    }
    if sum == 0.0 {
        return None;
    }
    let scaled = f64::from(cx.world.rng().rand()) * f64::from(1.0f32 / 32768.0);
    let pick = (f64::from(sum) * scaled) as f32;
    let mut running = 0.0f32;
    for (at, &w) in weights.iter().enumerate() {
        running = (f64::from(running) + f64::from(w)) as f32;
        if pick <= running {
            return Some(at);
        }
    }
    None
}

/// The directive list the leader of each formation gets: one directive
/// with the list's reference (the creator's released).
fn leader_directives(cx: &mut Cx<'_>, kind: DirectiveKind) -> super::directives::DirectiveRef {
    let arena = &mut cx.logic.content.state.directives;
    let d = arena.create(kind);
    arena.add_ref(d);
    arena.release(d);
    d
}

/// `FRandEncParams::Spawn(bForced)` (`RandomMissionDB.cpp:1785-1897`): the
/// ship name counter and the ships spawned restart; around the target
/// (`TargetObjectId`), with a `GotoShip` directive at the goto target (200,
/// -1; `Field_58` 1, cruise 0) for each leader, each formation to spawn
/// takes a weighted point out of a copy of the positions (its value is not
/// read) and spawns at `PlaceFacingAnchor(target, SpawnAngle,
/// SpawnDistance)`, moved to an empty place (`GetEmptyPos`, 3D volume) when
/// the radius `EmptyPosRadius` there is occupied, turned 180° from the
/// target's rotation ([`spawn_formation`] with the dock target). Then no
/// formation is left to spawn.
pub fn spawn(cx: &mut Cx<'_>, params: &mut RandEncParams) {
    params.ship_name_counter = 0;
    params.ships_spawned = 0;
    let (target_position, target_rotation) = api::space_obj::location(cx, params.target_object_id)
        .unwrap_or((Vector::ZERO, Matrix::IDENTITY));
    let mut free: Vec<(Vector, f32)> = params.positions.clone();
    let turn = turn_matrix();
    let mut goto = Goto::default();
    goto.op.kind = 0;
    goto.op.field_1c = params.goto_target_id as i32;
    goto.op.field_58 = 0;
    goto.op.range = 200.0;
    goto.op.field_54 = -1.0;
    goto.op.cruise = 0;
    goto.op.field_58 = 1;
    let leader = leader_directives(cx, DirectiveKind::GotoShip(goto));
    for _ in 0..params.formations_to_spawn {
        let weights: Vec<f32> = free.iter().map(|p| p.1).collect();
        let picked = pick_weighted(cx, &weights);
        let target = Transform {
            rotation: target_rotation,
            translation: target_position,
        };
        let mut around = place_facing_anchor(&target, params.spawn_angle, params.spawn_distance);
        let system = params
            .player
            .and_then(|p| cx.logic.content.state.players.get(&p))
            .and_then(|s| s.system);
        if let Some(system) = system
            && params.empty_pos_radius > 0
        {
            let radius = params.empty_pos_radius as f32;
            let point = around.translation;
            let empty = api::space_obj::is_pos_empty(cx, system, point, radius).unwrap_or(true);
            if !empty
                && let Ok(p) = api::space_obj::empty_pos(cx, system, &around, radius, radius, 3)
            {
                around.translation = p;
            }
        }
        if let Some(at) = picked {
            free.remove(at);
        }
        around.rotation = mul_matrix(&turn, &target_rotation);
        let directives = vec![leader];
        spawn_formation(
            cx,
            params,
            &around,
            -1,
            &directives,
            None,
            None,
            0,
            params.dock_target,
        );
    }
    params.formations_to_spawn = 0;
    cx.logic.content.state.directives.release(leader);
}

/// `FRandEncParams::SpawnFormation(Placement, Count, Directives, Choices,
/// Names, Priority, DockTarget)` (`RandomMissionDB.cpp:1899-2017`): a
/// weighted formation (none: false) and its ship count (`-1`: drawn from
/// its range), counted in the ships spawned. Each ship: a block named from
/// `names` or `rand_enc_ship_%d`, owned by the controller, at the
/// placement, a life of 24 (none with a dock target), the priority and the
/// labels; its classes a weighted ship choice (or `choices`' element), the
/// archetype the attacker's for the class those members make (else for the
/// members; none: false, nothing made); a named pilot of the attacker
/// faction, its costume, voice, archetype and name reputation; the
/// archetype's level; the first ship the directives. The blocks are laid
/// out in the formation and made (a dock launch from the dock target).
/// False without the player's system.
#[allow(clippy::too_many_arguments)]
pub fn spawn_formation(
    cx: &mut Cx<'_>,
    params: &mut RandEncParams,
    placement: &Transform,
    count: i32,
    directives: &[super::directives::DirectiveRef],
    choices: Option<&[ShipChoice]>,
    mut names: Option<&mut Vec<Vec<u8>>>,
    priority: i32,
    dock_target: u32,
) -> bool {
    let Some(system) = params
        .player
        .and_then(|p| cx.logic.content.state.players.get(&p))
        .and_then(|s| s.system)
    else {
        return false;
    };
    let weights: Vec<f32> = params.formations.iter().map(|f| f.weight).collect();
    let Some(picked) = pick_weighted(cx, &weights) else {
        return false;
    };
    if count == -1 {
        let f = &params.formations[picked];
        let range = f.max_count - f.min_count + 1;
        let drawn = cx.world.rng().rand().wrapping_mul(range) / 32768 + f.min_count;
        params.formations[picked].count = drawn;
    } else {
        params.formations[picked].count = count;
    }
    let formation = params.formations[picked].clone();
    params.ships_spawned += formation.count;
    let no_dock_target = dock_target & 0x3fff_ffff == 0;
    let attacker = params.attacker_rep as u32;
    let tables = cx.logic.content.tables.clone();
    let mut blocks: Vec<NpcShipParams> = Vec::new();
    for index in 0..formation.count {
        let mut block = NpcShipParams::default();
        match names.as_deref_mut() {
            None => {
                block.nickname = format!("rand_enc_ship_{}", params.ship_name_counter).into_bytes();
                params.ship_name_counter += 1;
            }
            Some(list) => {
                if !list.is_empty() {
                    block.nickname = list.remove(0);
                }
            }
        }
        block.controller = params.controller;
        block.orientation = placement.rotation;
        block.position = placement.translation;
        block.life_time = if no_dock_target { 24.0 } else { -1.0 };
        block.priority = priority;
        block.field_0x118.extend_from_slice(&params.labels);
        let mut member_line = EncounterShip::default();
        match choices {
            None => {
                let weights: Vec<f32> = formation.ship_choices.iter().map(|c| c.weight).collect();
                if let Some(c) = pick_weighted(cx, &weights) {
                    member_line.ship_classes = formation.ship_choices[c].ship_classes.clone();
                }
            }
            Some(list) => {
                if let Some(c) = list.get(index as usize) {
                    member_line.ship_classes = c.ship_classes.clone();
                }
            }
        }
        let class_id = super::tables::ship_classes::find_class_by_members(
            &tables.ship_classes,
            &member_line.ship_classes,
        )
        .unwrap_or(0);
        let class_line = EncounterShip {
            ship_classes: vec![class_id],
            ..EncounterShip::default()
        };
        let arch = cargo::pick_faction_ship_line_arch(cx, &tables, attacker, &class_line, -1)
            .or_else(|| {
                cargo::pick_faction_ship_line_arch(cx, &tables, attacker, &member_line, -1)
            });
        let Some(arch) = arch.and_then(|a| tables.npc_ship(a)) else {
            for b in blocks {
                npc_ships::release_params(cx, b);
            }
            npc_ships::release_params(cx, block);
            return false;
        };
        let mut name = NpcNameParams::default();
        npc_names::pick_npc_name_gender(cx, attacker, &mut name);
        let request = NpcNameRequest::default();
        if npc_names::build_npc_name(cx, attacker, &request, &mut name, true) {
            npc_ships::apply_npc_ship_faction(cx, attacker, name.gender, &mut block);
            npc_ships::apply_npc_ship_arch(cx, &mut block, arch, None, 0);
            if let Some(old) = block.reputation.take() {
                super::rep_handles::release(cx, old);
            }
            block.reputation = Some(npc_names::create_npc_name_reputation(cx, &name));
        }
        block.level = arch.level;
        if index == 0 {
            for &d in directives {
                block.directives.push(d);
                cx.logic.content.state.directives.add_ref(d);
            }
        }
        blocks.push(block);
    }
    super::dock_queues::place_formation_blocks(cx, &mut blocks, formation.formation, placement);
    npc_ships::create_npc_ships(cx, system, &blocks, dock_target);
    for b in blocks {
        npc_ships::release_params(cx, b);
    }
    true
}

/// `FRandEncTLParams::Spawn(bForced)` (`RandomEncounters.cpp:586-674`): the
/// attacked ring's shields drained; forced, the base spawn ([`spawn`]).
/// Otherwise each formation to spawn takes a weighted spawn point out of a
/// copy of the positions (none: that formation is skipped), turned by the
/// target's rotation onto its position, the formation turned 180° from it,
/// with a 25 s delay directive for its leader ([`spawn_lane_formation`]).
pub fn spawn_lane(cx: &mut Cx<'_>, params: &mut RandEncParams, forced: bool) {
    let _ = api::space_obj::drain_shields(cx, params.target_tradelane);
    if forced {
        spawn(cx, params);
        return;
    }
    params.ship_name_counter = 0;
    let (target_position, target_rotation) = api::space_obj::location(cx, params.target_object_id)
        .unwrap_or((Vector::ZERO, Matrix::from_rows([[0.0; 3]; 3])));
    let mut free: Vec<(Vector, f32)> = params.positions.clone();
    let turn = turn_matrix();
    let mut placement = Transform {
        rotation: Matrix::IDENTITY,
        translation: Vector::ZERO,
    };
    let leader = leader_directives(
        cx,
        DirectiveKind::Delay(Delay {
            priority: 1,
            delay_seconds: 25.0,
        }),
    );
    for _ in 0..params.formations_to_spawn {
        let weights: Vec<f32> = free.iter().map(|p| p.1).collect();
        let Some(picked) = pick_weighted(cx, &weights) else {
            continue;
        };
        let offset = transform_matrix(&target_rotation, free[picked].0);
        placement.translation = Vector::new(
            target_position.x + offset.x,
            target_position.y + offset.y,
            target_position.z + offset.z,
        );
        free.remove(picked);
        placement.rotation = mul_matrix(&turn, &target_rotation);
        spawn_lane_formation(cx, params, &placement, -1, &[leader]);
    }
    params.formations_to_spawn = 0;
    cx.logic.content.state.directives.release(leader);
}

/// `FRandEncTLParams::SpawnLaneFormation(Placement, Count, Directives, ...)`
/// (`RandomEncounters.cpp:677-848`): a weighted formation (none: nothing)
/// and its count; per ship a block as [`spawn_formation`]'s with a life of
/// 24, the attacker's archetype for a weighted ship choice's classes (a
/// ship without one is skipped), the encounter's pilot, a scan chance of
/// 1, the named reputation; then the leader a patrol guide (type 3) on a
/// random segment of the attacked zone's path heading a random way, its
/// targets ([`super::spawn_placement::set_leader_guide_targets`]), the
/// blocks laid out and made.
///
/// The C++ reads the zone's path, its segments and the first block without
/// a test; without them nothing is made (the segment and direction draws
/// are still made when the zone has a path; D1065).
pub fn spawn_lane_formation(
    cx: &mut Cx<'_>,
    params: &mut RandEncParams,
    placement: &Transform,
    count: i32,
    directives: &[super::directives::DirectiveRef],
) {
    let weights: Vec<f32> = params.formations.iter().map(|f| f.weight).collect();
    let Some(picked) = pick_weighted(cx, &weights) else {
        return;
    };
    let mut count = count;
    if count == -1 {
        let f = &params.formations[picked];
        count = cx
            .world
            .rng()
            .rand()
            .wrapping_mul(f.max_count - f.min_count + 1)
            / 32768
            + f.min_count;
    }
    params.formations[picked].count = count;
    let formation: RandEncFormation = params.formations[picked].clone();
    let attacker = params.attacker_rep as u32;
    let tables = cx.logic.content.tables.clone();
    let mut blocks: Vec<NpcShipParams> = Vec::new();
    for index in 0..formation.count {
        let mut block = NpcShipParams {
            nickname: format!("rand_enc_ship_{}", params.ship_name_counter).into_bytes(),
            ..NpcShipParams::default()
        };
        params.ship_name_counter += 1;
        block.controller = params.controller;
        block.orientation = placement.rotation;
        block.position = placement.translation;
        block.life_time = 24.0;
        block.field_0x118.extend_from_slice(&params.labels);
        let mut line = EncounterShip::default();
        let weights: Vec<f32> = formation.ship_choices.iter().map(|c| c.weight).collect();
        if let Some(c) = pick_weighted(cx, &weights) {
            line.ship_classes = formation.ship_choices[c].ship_classes.clone();
        }
        let Some(arch) = cargo::pick_faction_ship_line_arch(cx, &tables, attacker, &line, -1)
            .and_then(|a| tables.npc_ship(a))
        else {
            continue;
        };
        let mut name = NpcNameParams::default();
        npc_names::pick_npc_name_gender(cx, attacker, &mut name);
        let request = NpcNameRequest::default();
        if npc_names::build_npc_name(cx, attacker, &request, &mut name, true) {
            npc_ships::apply_npc_ship_faction(cx, attacker, name.gender, &mut block);
            npc_ships::apply_npc_ship_arch(cx, &mut block, arch, Some(params.pilot_id), 0);
            block.personality.job.scan_chance = 1.0;
            if let Some(old) = block.reputation.take() {
                super::rep_handles::release(cx, old);
            }
            block.reputation = Some(npc_names::create_npc_name_reputation(cx, &name));
        }
        block.level = arch.level;
        if index == 0 {
            for &d in directives {
                block.directives.push(d);
                cx.logic.content.state.directives.add_ref(d);
            }
        }
        blocks.push(block);
    }
    let release = |cx: &mut Cx<'_>, blocks: Vec<NpcShipParams>| {
        for b in blocks {
            npc_ships::release_params(cx, b);
        }
    };
    let Some(system) = params.system else {
        release(cx, blocks);
        return;
    };
    let zone = params.target_zone.and_then(|z| {
        cx.logic
            .content
            .state
            .systems
            .get(&system)
            .and_then(|r| r.zones.get(z))
            .map(|zz| (zz.zone_id, zz.path))
    });
    let Some((zone_id, Some(path_index))) = zone else {
        release(cx, blocks);
        return;
    };
    let segments = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .and_then(|r| r.paths.get(path_index))
        .and_then(|p| p.zone_segments(zone_id))
        .unwrap_or_default();
    let at = (cx.world.rng().rand().wrapping_mul(segments.len() as i32) / 32768) as usize;
    let direction = if cx.world.rng().rand().wrapping_mul(2) / 32768 != 0 {
        1
    } else {
        -1
    };
    let (Some(segment), false) = (segments.get(at).copied(), blocks.is_empty()) else {
        release(cx, blocks);
        return;
    };
    let mut plan = SpawnPlan {
        path: Some(PatrolPathRef {
            system,
            index: path_index,
        }),
        path_direction: direction,
        ..SpawnPlan::default()
    };
    plan.path_target_point = if direction == 1 {
        segment.second_point
    } else {
        segment.first_point
    };
    blocks[0].guide = Some(GuideDesc {
        kind: 3,
        plan,
        wander_zone: Some(zone_id),
        ..GuideDesc::default()
    });
    super::spawn_placement::set_leader_guide_targets(cx, system, &mut plan, &mut blocks[0]);
    if let Some(desc) = blocks[0].guide.as_mut() {
        desc.plan = plan;
    }
    super::dock_queues::place_formation_blocks(cx, &mut blocks, formation.formation, placement);
    npc_ships::create_npc_ships(cx, system, &blocks, 0);
    release(cx, blocks);
}

/// The populator's own state (`FRandEncTLPopulator +0x08`,
/// `FRandEncSPPopulator +0x08`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PopulatorState {
    /// TL: `bAttackArmed`.
    pub armed: bool,
    /// SP: `ElapsedTime`.
    pub elapsed: f32,
}

/// The populator's `Reset` (`RandomEncounters.cpp:1242-1249`, `:1594-1600`).
pub fn reset(params: &mut RandEncParams) {
    params.goto_target_id = 0;
    match params.kind {
        RandEncKind::TradeLane => {
            params.populator.armed = false;
            params.tl_attack_chance = params.tl_attack_chance_read_in;
            params.target_tradelane = 0;
        }
        RandEncKind::Space => {
            params.cumulative_time = 0.0;
            params.populator.elapsed = 0.0;
        }
    }
}

/// The ship of the encounter's player, 0 without one.
fn player_ship(cx: &Cx<'_>, params: &RandEncParams) -> u32 {
    params
        .player
        .and_then(|p| cx.logic.content.state.players.get(&p))
        .map_or(0, super::players::PlayerState::ship_id)
}

/// The player's vibe (`FContentPlayerState::GetReputation`).
fn player_vibe(cx: &Cx<'_>, params: &RandEncParams) -> i32 {
    params
        .player
        .and_then(|p| cx.logic.content.state.players.get(&p))
        .map_or(-1, |s| s.reputation)
}

/// `CollectHostilePairs`: the pairs whose group is hostile to the vibe.
fn hostile_pairs(cx: &mut Cx<'_>, pairs: &[(u32, f32)], vibe: i32) -> Vec<(u32, f32)> {
    let mut out = Vec::new();
    for &(group, weight) in pairs {
        if cargo::vibe_feeling_mask(cx, group, vibe) & 4 != 0 {
            out.push((group, weight));
        }
    }
    out
}

/// The attacker of an encounter without one: of the zone's population,
/// the factions that fly a ship class of the formations given (each
/// once), those hostile to the vibe; one uniformly (no draw for one), and
/// every ship choice the attacker cannot fly erased. False when none is
/// hostile.
fn pick_attacker(
    cx: &mut Cx<'_>,
    params: &mut RandEncParams,
    population: &[(u32, f32)],
    formations: &[usize],
    vibe: i32,
) -> bool {
    let tables = cx.logic.content.tables.clone();
    let mut class_factions: Vec<u32> = Vec::new();
    for &f in formations {
        for choice in &params.formations[f].ship_choices {
            for faction in cargo::npc_class_factions(&tables, &choice.ship_classes) {
                if !class_factions.contains(&faction) {
                    class_factions.push(faction);
                }
            }
        }
    }
    let populated: Vec<(u32, f32)> = population
        .iter()
        .copied()
        .filter(|(g, _)| class_factions.contains(g))
        .collect();
    let allowed = hostile_pairs(cx, &populated, vibe);
    if allowed.is_empty() {
        return false;
    }
    let count = allowed.len() as i32;
    let mut pick = 0;
    if count - 1 != 0 {
        pick = cx.world.rng().rand().wrapping_mul(count) / 32768;
    }
    params.attacker_rep = allowed[pick as usize].0 as i32;
    let attacker = params.attacker_rep as u32;
    for formation in &mut params.formations {
        formation
            .ship_choices
            .retain(|c| cargo::faction_has_npc_class(&tables, attacker, &c.ship_classes));
    }
    true
}

/// `TrimTravelledRings` (`RandomEncounters.cpp:449-480`): the stretch of the
/// lane the player travels from the exit ring: towards the lane's start
/// (the exit's next ring is the entry) the direction is -1 and the rings
/// before the exit stay, without the first; else +1 and the rings from the
/// exit on, without the last.
fn trim_travelled_rings(
    cx: &Cx<'_>,
    params: &mut RandEncParams,
    rings: &mut Vec<systems::TradeLaneRing>,
) {
    let Some(exit) = rings.iter().position(|r| r.object == params.exit_ring) else {
        return;
    };
    let (next, _) =
        api::space_obj::tradelane_next_and_prev(cx, rings[exit].object).unwrap_or((0, 0));
    if next == params.entry_ring {
        params.direction = -1;
        rings.truncate(exit);
        if !rings.is_empty() {
            rings.remove(0);
        }
    } else {
        params.direction = 1;
        rings.drain(..exit);
        rings.pop();
    }
}

/// `FRandEncTLPopulator::Accept(Params, Message, Parms)` (`RandomEncounters.cpp:907-1137`):
/// - `SpaceExit` resets;
/// - `TradelaneEnter` of the player's ship, not armed: without a target
///   ring the attack chance rolls (`rand() * 101 / 32768` above it adds 15
///   and ends), then a random ring of the stretch travelled; the six
///   default spawn points along the direction, `pilot_0`, the formations to
///   spawn (1 to 3 by default), and without an attacker one hostile to the
///   player from the population of the ring's smallest zone; then armed;
/// - `Heartbeat`: armed and the player's ship within 10000 of the ring
///   fires; the goto target is the named ship or the player's;
/// - `SystemEnter` restores an attack chance at or below 50.
pub fn tl_accept(
    cx: &mut Cx<'_>,
    params: &mut RandEncParams,
    message: &ControllerMessage<'_>,
) -> bool {
    use ControllerMessage as M;
    let mut trigger = false;
    match message {
        M::SpaceExit { .. } => reset(params),
        M::TradelaneEnter {
            ship,
            entrance,
            exit,
            system,
        } => {
            if params.populator.armed {
                return false;
            }
            let own = player_ship(cx, params);
            if own == 0 || *ship != own {
                return false;
            }
            let rings = |cx: &Cx<'_>, ring: u32| {
                cx.logic
                    .content
                    .state
                    .systems
                    .get(system)
                    .and_then(|r| r.lane_rings_of_ring(ring))
                    .map(<[_]>::to_vec)
                    .unwrap_or_default()
            };
            if params.target_tradelane == 0 {
                let roll = cx.world.rng().rand().wrapping_mul(0x65) / 32768;
                if roll > params.tl_attack_chance {
                    params.tl_attack_chance += 15;
                    return false;
                }
                params.entry_ring = *entrance;
                params.exit_ring = *exit;
                params.system = Some(*system);
                let mut list = rings(cx, params.entry_ring);
                trim_travelled_rings(cx, params, &mut list);
                if list.is_empty() {
                    return false;
                }
                let count = list.len() as i32;
                let mut index = 0;
                if count - 1 != 0 {
                    index = cx.world.rng().rand().wrapping_mul(count) / 32768;
                }
                params.target_tradelane = list[index as usize].object;
                params.target_object_id = params.target_tradelane;
            } else {
                params.target_object_id = params.target_tradelane;
                params.entry_ring = *entrance;
                params.exit_ring = *exit;
                params.system = Some(*system);
                let mut list = rings(cx, params.target_tradelane);
                trim_travelled_rings(cx, params, &mut list);
            }
            if params.positions.is_empty() {
                for (x, z, w) in [
                    (0.0, 3000, 80.0),
                    (1250.0, 4000, 2.5),
                    (-1250.0, 4000, 2.5),
                    (2000.0, 5000, 5.0),
                    (-2000.0, 5000, 5.0),
                    (0.0, 6750, 5.0),
                ] {
                    params
                        .positions
                        .push((Vector::new(x, 0.0, (params.direction * z) as f32), w));
                }
            }
            if params.pilot_id == 0 {
                params.pilot_id = neuron_core::id::create_id(b"pilot_0");
            }
            if params.formations_to_spawn == 0 {
                if params.num_forms_min == 0 {
                    params.num_forms_min = 1;
                }
                if params.num_forms_max == 0 {
                    params.num_forms_max = 3;
                }
                let (min, max) = (params.num_forms_min, params.num_forms_max);
                params.formations_to_spawn =
                    cx.world.rng().rand().wrapping_mul(max - min + 1) / 32768 + min;
            }
            if params.attacker_rep == -1 {
                let (position, _) = api::space_obj::location(cx, params.target_tradelane)
                    .unwrap_or((Vector::ZERO, Matrix::IDENTITY));
                let Some(system) = params.system else {
                    return false;
                };
                params.target_zone = systems::find_smallest_zone_at(cx, system, position);
                let Some(zone) = params.target_zone.and_then(|z| {
                    cx.logic
                        .content
                        .state
                        .systems
                        .get(&system)
                        .and_then(|r| r.zones.get(z))
                        .map(|zz| zz.zone_id)
                }) else {
                    return false;
                };
                let Ok(population) = api::zone::population(cx, zone) else {
                    return false;
                };
                let mut picked = Vec::new();
                for _ in 0..params.formations_to_spawn {
                    let weights: Vec<f32> = params.formations.iter().map(|f| f.weight).collect();
                    if let Some(f) = pick_weighted(cx, &weights) {
                        picked.push(f);
                    }
                }
                let vibe = player_vibe(cx, params);
                if !pick_attacker(cx, params, &population, &picked, vibe) {
                    params.target_tradelane = 0;
                    return false;
                }
            }
            params.populator.armed = true;
            return false;
        }
        M::Heartbeat { .. } => {
            let own = player_ship(cx, params);
            if params.populator.armed {
                let distance_sq = object_distance_sq(cx, params.target_tradelane, own);
                let truncated = (distance_sq as i64) as u32 as i32;
                if truncated <= 100_000_000 {
                    trigger = true;
                }
            }
            if params.goto_target_id & 0x3fff_ffff == 0 {
                params.goto_target_id = named_target(cx, params).unwrap_or(own);
            }
        }
        M::SystemEnter { .. } if params.tl_attack_chance <= 0x32 => {
            params.tl_attack_chance = params.tl_attack_chance_read_in;
        }
        _ => {}
    }
    trigger
}

/// `GetObjectDistanceSq(&A, &B)` (`MissionCnd.cpp:2574-2587`): -1 when a
/// location fails.
fn object_distance_sq(cx: &Cx<'_>, a: u32, b: u32) -> f64 {
    let (Ok((pa, _)), Ok((pb, _))) = (
        api::space_obj::location(cx, a),
        api::space_obj::location(cx, b),
    ) else {
        return -1.0;
    };
    let dx = f64::from(pa.x) - f64::from(pb.x);
    let dy = f64::from(pa.y) - f64::from(pb.y);
    let dz = f64::from(pa.z) - f64::from(pb.z);
    dz * dz + dy * dy + dx * dx
}

/// The target ship the encounter names (`FindObjectIdByName` with the
/// controller), `None` without a name or when none is found.
fn named_target(cx: &Cx<'_>, params: &RandEncParams) -> Option<u32> {
    if params.target_ship_name.is_empty() {
        return None;
    }
    let system = params.system?;
    let id =
        systems::find_object_id_by_name(cx, system, &params.target_ship_name, params.controller);
    (id != 0).then_some(id)
}

/// `FRandEncSPPopulator::Accept(Params, Message, Parms)` (`RandomEncounters.cpp:1252-1466`):
/// `SpaceExit` resets; on a `Heartbeat`, with the player's ship in a
/// system and not in a lane:
/// - the goto and target object are the named ship or the player's;
/// - the activation: `time` (also an unset type) picks its threshold when
///   it is 0 (`rand()` between the bounds) and fires once the seconds
///   reach it; `location` fires within the range of the activation point;
/// - without an attacker the target's smallest zone must be populated
///   (whether or not it fired);
/// - firing: `pilot_0`, the formations to spawn (1 to 3), three default
///   spawn points behind the target (`y = rand() * 201 / 32768`), and
///   without an attacker one hostile to the target (the player's vibe, or
///   the target's reputation) from every formation's classes; none hostile
///   does not fire.
pub fn sp_accept(
    cx: &mut Cx<'_>,
    params: &mut RandEncParams,
    message: &ControllerMessage<'_>,
) -> bool {
    use ControllerMessage as M;
    let elapsed = match message {
        M::Heartbeat { elapsed } => *elapsed,
        M::SpaceExit { .. } => {
            reset(params);
            return false;
        }
        _ => return false,
    };
    let Some(player) = params
        .player
        .and_then(|p| cx.logic.content.state.players.get(&p))
    else {
        return false;
    };
    let (Some(ship), Some(system)) = (player.ship.as_ref(), player.system) else {
        return false;
    };
    if ship.lane_entry != 0 {
        return false;
    }
    let own = ship.ship;
    params.system = Some(system);
    if params.goto_target_id & 0x3fff_ffff == 0 {
        let target = named_target(cx, params).unwrap_or(own);
        params.goto_target_id = target;
        params.target_object_id = target;
    }
    let mut trigger = false;
    if params.activation_type == 0 {
        params.activation_type = 2;
    }
    if params.activation_type == 2 {
        if params.cumulative_time == 0.0 {
            let high = neuron_core::num::trunc_to_i32(f64::from(params.activation_time[0]));
            let low = neuron_core::num::trunc_to_i32(f64::from(params.activation_time[1]));
            let picked = cx.world.rng().rand().wrapping_mul(high - low + 1) / 32768 + low;
            params.cumulative_time = f64::from(picked as u32) as f32;
        }
        if !(params.populator.elapsed >= params.cumulative_time) {
            params.populator.elapsed += elapsed;
        } else {
            params.cumulative_time = 0.0;
            params.populator.elapsed = 0.0;
            trigger = true;
        }
    } else if params.activation_type == 1 {
        let distance_sq = match api::space_obj::location(cx, params.goto_target_id) {
            Ok((p, _)) => {
                let a = params.activation_location;
                let dx = f64::from(p.x) - f64::from(a.x);
                let dy = f64::from(p.y) - f64::from(a.y);
                let dz = f64::from(p.z) - f64::from(a.z);
                (dz * dz + dy * dy) + dx * dx
            }
            Err(_) => -1.0,
        };
        if f64::from(params.activation_range) >= distance_sq {
            trigger = true;
        }
    }
    let mut population = Vec::new();
    if params.attacker_rep == -1 {
        let Ok((position, _)) = api::space_obj::location(cx, params.goto_target_id) else {
            return false;
        };
        params.target_zone = systems::find_smallest_zone_at(cx, system, position);
        let Some(zone) = params.target_zone.and_then(|z| {
            cx.logic
                .content
                .state
                .systems
                .get(&system)
                .and_then(|r| r.zones.get(z))
                .map(|zz| zz.zone_id)
        }) else {
            return false;
        };
        let Ok(p) = api::zone::population(cx, zone) else {
            return false;
        };
        population = p;
    }
    if !trigger {
        return false;
    }
    if params.pilot_id == 0 {
        params.pilot_id = neuron_core::id::create_id(b"pilot_0");
    }
    if params.formations_to_spawn == 0 {
        if params.num_forms_min == 0 {
            params.num_forms_min = 1;
        }
        if params.num_forms_max == 0 {
            params.num_forms_max = 3;
        }
        let (min, max) = (params.num_forms_min, params.num_forms_max);
        params.formations_to_spawn =
            cx.world.rng().rand().wrapping_mul(max - min + 1) / 32768 + min;
    }
    if params.positions.is_empty() {
        for (x, z, w) in [
            (0.0, -3000.0, 80.0),
            (2000.0, -3500.0, 2.5),
            (-2000.0, -3500.0, 2.5),
        ] {
            let y = cx.world.rng().rand().wrapping_mul(0xc9) / 32768;
            params
                .positions
                .push((Vector::new(x, f64::from(y as u32) as f32, z), w));
        }
    }
    if params.attacker_rep != -1 {
        return true;
    }
    let vibe = if own == params.goto_target_id {
        player_vibe(cx, params)
    } else {
        api::space_obj::rep(cx, params.goto_target_id).unwrap_or(0)
    };
    let all: Vec<usize> = (0..params.formations.len()).collect();
    pick_attacker(cx, params, &population, &all, vibe)
}

/// The populator's `Accept` by the parameters' class.
pub fn accept(
    cx: &mut Cx<'_>,
    params: &mut RandEncParams,
    message: &ControllerMessage<'_>,
) -> bool {
    match params.kind {
        RandEncKind::TradeLane => tl_accept(cx, params, message),
        RandEncKind::Space => sp_accept(cx, params, message),
    }
}

/// The parameters' `Spawn(true)` by class: the trade-lane class drains the
/// ring's shields first.
pub fn spawn_forced(cx: &mut Cx<'_>, params: &mut RandEncParams) {
    match params.kind {
        RandEncKind::TradeLane => spawn_lane(cx, params, true),
        RandEncKind::Space => spawn(cx, params),
    }
}

/// `FMissionObject::NotifyRandEncs(Message, Parms)` (`MissionObject.cpp:2563-2581`):
/// with the player's ship, every enabled encounter whose populator accepts
/// the message has its NPC count zeroed, spawns (`Spawn(true)`) and is
/// disabled. Each record's parameters are taken out of the mission for the
/// calls, which make ships (record 0008 point 1: the mission's own item,
/// put back by index). A record without parameters, whose populator the
/// reference calls through null, is passed over (D1069).
pub fn notify_rand_encs(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) {
    let count = match cx.logic.content.instance(id) {
        Some(super::Instance::Mission(s)) => {
            let has_ship = cx
                .logic
                .content
                .state
                .players
                .get(&s.object.player)
                .is_some_and(|p| p.ship.is_some());
            if !has_ship {
                return;
            }
            s.object.rand_encs.len()
        }
        _ => return,
    };
    for at in 0..count {
        let taken = match cx.logic.content.instance_mut(id) {
            Some(super::Instance::Mission(s)) => s
                .object
                .rand_encs
                .get_mut(at)
                .filter(|r| r.enabled)
                .and_then(|r| r.params.take()),
            _ => None,
        };
        let Some(mut params) = taken else {
            continue;
        };
        let fire = accept(cx, &mut params, message);
        if fire {
            spawn_forced(cx, &mut params);
        }
        if let Some(super::Instance::Mission(s)) = cx.logic.content.instance_mut(id)
            && let Some(r) = s.object.rand_encs.get_mut(at)
        {
            r.params = Some(params);
            if fire {
                r.npc_count = 0;
                r.enabled = false;
            }
        }
    }
}

/// `CollectHostileFactions` (`SystemInfo.cpp:1474-1485`): the pairs whose
/// faction feels hostile towards the affiliation.
fn hostile_factions(cx: &Cx<'_>, pairs: &[(u32, f32)], affiliation: u32) -> Vec<(u32, f32)> {
    pairs
        .iter()
        .copied()
        .filter(|&(g, _)| cargo::faction_feeling_mask(cx, g, affiliation) & 4 != 0)
        .collect()
}

/// `FSystemInfo::SpawnTradeLaneAttack(PlayerShip, &Direction, EntryIndex,
/// &Lane)` (`SystemInfo.cpp:1489-1667`): unless the player's flags have bit
/// 8, at most every 180 s per lane (or its first time), with the chance of
/// the lane's largest attack weight, and while no ring of the lane is
/// destroyed: a ring ahead of the ship (beyond the next one) with no player
/// within 5000, drawn by its attack weight; one of its attack zones by
/// weight; one of the zone's encounters with a faction hostile to the
/// ring's owner, uniformly, and its first formation. A trade-lane attack
/// (type 3) on the ring by a faction of that formation hostile to the owner
/// (by weight), one formation at `(0, 0, direction * 1400)` (weight 80)
/// owned by SpacePop, the formation's lines as one formation line (the
/// summed counts, each line's classes a choice of weight 1), spawned out of
/// the lane ([`spawn_lane`] with `false`).
///
/// The C++ reads an empty pick's element, an encounter without formations
/// and a ring zone without a record without a test; the port stops there
/// (D1065). A failed `GetSolarRep` leaves the selected ring's squared
/// distance bits as the reputation in the C++; here it is -1 (D1065).
pub fn spawn_trade_lane_attack(
    cx: &mut Cx<'_>,
    system: u32,
    player: u32,
    direction: i32,
    entry_index: i32,
    lane: usize,
) {
    let state = &cx.logic.content.state;
    if state.players.get(&player).is_some_and(|p| p.flags & 8 != 0) {
        return;
    }
    let now = super::game_support::game_time(cx);
    let Some(l) = state
        .systems
        .get(&system)
        .and_then(|r| r.trade_lanes.get(lane))
    else {
        return;
    };
    if !(now - f64::from(l.last_attack_time) > 180.0) && l.last_attack_time != 0.0 {
        return;
    }
    let max_weight = l.max_weight;
    let rings: Vec<u32> = l.rings.iter().map(|r| r.object).collect();
    let mut candidates = l.ring_candidates.clone();
    let lane_rings = l.rings.clone();
    if let Some(l) = cx
        .logic
        .content
        .state
        .systems
        .get_mut(&system)
        .and_then(|r| r.trade_lanes.get_mut(lane))
    {
        l.last_attack_time = now as f32;
    }
    if !(f64::from(cx.world.rng().rand()) * (1.0 / 32768.0) < f64::from(max_weight)) {
        return;
    }
    for ring in &rings {
        if api::space_obj::relative_health(cx, *ring).unwrap_or(1.0) == 0.0 {
            return;
        }
    }
    let mut selected = None;
    while !candidates.is_empty() && selected.is_none() {
        let weights: Vec<(u32, f32)> = candidates.iter().map(|c| (0, c.max_weight)).collect();
        let Some(pick) = super::encounters::weighted_pick(cx, &weights) else {
            return;
        };
        let index = candidates[pick].ring_index;
        if (direction == 1 && index > entry_index + 1)
            || (direction == -1 && index < entry_index - 1)
        {
            let position = lane_rings
                .get(index as usize)
                .map_or(Vector::ZERO, |r| r.position);
            if !systems::player_ship_near(cx, system, position, 5000.0).0 {
                selected = Some(pick);
                continue;
            }
        }
        candidates.remove(pick);
    }
    let Some(selected) = selected else {
        return;
    };
    let selected = candidates[selected].clone();
    let zone_weights: Vec<(u32, f32)> = selected
        .candidates
        .iter()
        .map(|&(z, w)| (z as u32, w))
        .collect();
    let Some(zone_pick) = super::encounters::weighted_pick(cx, &zone_weights) else {
        return;
    };
    let zone = selected.candidates[zone_pick].0;
    let ring = lane_rings
        .get(selected.ring_index as usize)
        .map_or(0, |r| r.object);
    let rep = api::space_obj::solar_rep(cx, ring).unwrap_or(-1);
    let affiliation = api::reputation::affiliation(cx, rep).unwrap_or(u32::MAX);
    let Some(encounters) = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .and_then(|r| r.zones.get(zone))
        .map(|z| z.population.encounters.clone())
    else {
        return;
    };
    let mut indices = Vec::with_capacity(encounters.len());
    for (index, encounter) in encounters.iter().enumerate() {
        if !hostile_factions(cx, &encounter.parameters.factions, affiliation).is_empty() {
            indices.push(index);
        }
    }
    let pick = cx.world.rng().rand().wrapping_mul(indices.len() as i32) / 32768;
    let Some(formation) = indices
        .get(pick as usize)
        .and_then(|&i| encounters[i].parameters.formations.first())
        .cloned()
    else {
        return;
    };
    let hostile = hostile_factions(cx, &formation.factions, affiliation);
    let mut attack = RandEncParams::new(RandEncKind::TradeLane);
    attack.target_tradelane = ring;
    attack.target_object_id = ring;
    attack.goto_target_id = ring;
    attack.direction = direction;
    attack.target_zone = Some(zone);
    let Some(attacker) = super::encounters::weighted_pick(cx, &hostile) else {
        return;
    };
    attack.attacker_rep = hostile[attacker].0 as i32;
    attack.system = Some(system);
    attack.formations_to_spawn = 1;
    attack
        .positions
        .push((Vector::new(0.0, 0.0, (direction * 1400) as f32), 80.0));
    attack.controller = cx.logic.content.state.space_pop.0;
    let tables = cx.logic.content.tables.clone();
    for _ in 0..attack.formations_to_spawn {
        let nickname = if formation.formation_by_class != 0 {
            tables
                .faction_props(formation.faction)
                .and_then(|p| p.formation(formation.formation_by_class))
                .unwrap_or(0)
        } else {
            formation.formation
        };
        let mut entry = RandEncFormation {
            nickname,
            formation: tables.formation(nickname).map(|_| nickname),
            weight: 1.0,
            ..RandEncFormation::default()
        };
        for line in &formation.ships {
            entry.min_count += line.min_count;
            entry.max_count += line.max_count;
            entry.ship_choices.push(ShipChoice {
                ship_classes: line.ship_classes.clone(),
                weight: 1.0,
            });
        }
        attack.formations.push(entry);
    }
    spawn_lane(cx, &mut attack, false);
}
