//! The encounters' run time (TU29 `Content/EncounterParams.cpp` and TU30
//! `Content/Encounters.cpp`; record 0008 point 7; work package P3.6d): the
//! encounter parameters' refresh with their faction and permutation picks,
//! the zone populations' validation and spawns, the formation spawn with
//! its blocks, names, guide, cargo and placement.
//!
//! **Where the C++ holds pointers.** A zone's population lives in its
//! system record (`FSystemZone::Population`); the port names a zone by its
//! system and its index in the record ([`ZoneRef`]). The refresh, which
//! dispatches nothing, runs on the encounter taken out of its slot
//! ([`with_encounter_taken`], record 0008 point 1). A spawn dispatches
//! (`NpcShipCreated`), so it works on copies of what it reads and writes
//! the fields it changes back by index: the formation's count, its ship
//! lines' archetypes and its created objects.

// The C++'s `!(x > 0.0f)` tests send NaN to the failing side; the port
// spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::{Transform, Vector};

use super::encounter_params::{EncounterFormation, EncounterParameters};
use super::guides::{GuideDesc, SpawnPlan};
use super::npc_names::{self, NpcNameParams, NpcNameRequest};
use super::npc_ships::{self, NpcShipParams};
use super::spawn_placement::{self, SpawnRequest};
use super::tables::ContentTables;
use super::zones::{DensityRestriction, ZonePopulation};
use super::{NoDispatch, cargo, ships};
use crate::api;
use crate::api::space_obj::CargoDesc;
use crate::server::Cx;

/// A zone of a system record: the system and the zone's index in the
/// record's zones.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ZoneRef {
    /// The system record.
    pub system: u32,
    /// The zone's index in its zones.
    pub zone: usize,
}

/// `GEncounterDensityRadiusSP` (`Encounters.cpp:387`, 3750).
pub const DENSITY_RADIUS_SP: f32 = 3750.0;
/// `GEncounterDensityRadiusMP` (`:389`): `float(850.0 + 850.0 + 7500.0)`.
pub const DENSITY_RADIUS_MP: f32 = 9200.0;
/// `GEncounterRestrictionCone` (`:390`, 3750).
pub const RESTRICTION_CONE: f32 = 3750.0;
/// `GEncounterPlacementRange` (`:391`, 2700).
pub const PLACEMENT_RANGE: f32 = 2700.0;

/// The `IntersectIds` of TU29 (`EncounterParams.cpp:50-80`): the ids of `a`
/// that are also in `b`, each once, in `a`'s order.
fn intersect_ids(a: &[u32], b: &[u32]) -> Vec<u32> {
    let mut out: Vec<u32> = Vec::new();
    for &id in a {
        if b.contains(&id) && !out.contains(&id) {
            out.push(id);
        }
    }
    out
}

/// `FEncounterParameters::BuildFormationFactions(Formation)`
/// (`EncounterParams.cpp:498-572`): the formation's faction choices are the
/// pairs of the parameters' faction weights whose faction may fly every
/// ship line: the lines with an `npc_arch` through `GetNpcArchFactions`,
/// the others through `GetShipLineFactions`, intersected. True when any
/// remain.
pub fn build_formation_factions(
    tables: &ContentTables,
    factions: &[(u32, f32)],
    formation: &mut EncounterFormation,
) -> bool {
    formation.factions.clear();
    let mut class_factions: Vec<u32> = Vec::new();
    let mut have_class_factions = false;
    let mut npc_archs: Vec<u32> = Vec::new();
    for ship in &formation.ships {
        if ship.npc_arch != 0 {
            npc_archs.push(ship.npc_arch);
            continue;
        }
        let line = cargo::ship_line_factions(tables, ship);
        if have_class_factions {
            class_factions = intersect_ids(&class_factions, &line);
        } else {
            class_factions = line;
            have_class_factions = true;
        }
    }
    let allowed = if npc_archs.is_empty() {
        class_factions
    } else {
        let arch_factions = cargo::npc_arch_factions(tables, &npc_archs);
        if have_class_factions {
            intersect_ids(&class_factions, &arch_factions)
        } else {
            arch_factions
        }
    };
    if allowed.is_empty() {
        return false;
    }
    for &(faction, weight) in factions {
        if allowed.contains(&faction) {
            formation.factions.push((faction, weight));
        }
    }
    !formation.factions.is_empty()
}

/// `weighted_vector::Pick` in a server process (Common's
/// `common_weighted_vector.h`, the 53-bit x87 mode): the weights summed in
/// `f64`, the total stored as a float, the target `rand() * 2^-15 * total`
/// stored as a float, the first entry whose running `f64` sum reaches it.
/// `None` without a draw for no entries or a zero sum; `None` after the
/// draw for a walk past the end.
pub fn weighted_pick(cx: &mut Cx<'_>, items: &[(u32, f32)]) -> Option<usize> {
    if items.is_empty() {
        return None;
    }
    let sum: f64 = items.iter().fold(0.0, |s, &(_, w)| s + f64::from(w));
    let total = sum as f32;
    if sum == 0.0 {
        return None;
    }
    let target =
        (f64::from(cx.world.rng().rand()) * f64::from(1.0f32 / 32768.0) * f64::from(total)) as f32;
    let mut acc = 0.0;
    for (index, &(_, w)) in items.iter().enumerate() {
        acc += f64::from(w);
        if f64::from(target) <= acc {
            return Some(index);
        }
    }
    None
}

/// `FEncounterParameters::PickFaction(Formation)` (`:577-621`): the
/// formation's faction, a weighted pick of its choices; with
/// `feeling_to_formation` only among the choices whose feeling towards the
/// faction of formation `feeling_value` has a bit of the mask (`same`, 8:
/// that very faction). The C++ reads that formation without a bound; one
/// past the list reads as faction -1 (D1060).
pub fn pick_faction(cx: &mut Cx<'_>, params: &mut EncounterParameters, index: usize) -> bool {
    let formation = &params.formations[index];
    if formation.factions.is_empty() {
        return false;
    }
    let picked = if formation.feeling_to_formation != 1 {
        let choices = formation.factions.clone();
        weighted_pick(cx, &choices).map(|i| choices[i].0)
    } else {
        let other = usize::try_from(formation.feeling_value)
            .ok()
            .and_then(|i| params.formations.get(i))
            .map_or(u32::MAX, |f| f.faction);
        let mask = formation.feeling_mask;
        let matching: Vec<(u32, f32)> = formation
            .factions
            .iter()
            .copied()
            .filter(|&(faction, _)| {
                if mask == 8 {
                    faction == other
                } else {
                    mask & cargo::faction_feeling_mask(cx, faction, other) != 0
                }
            })
            .collect();
        if matching.is_empty() {
            return false;
        }
        weighted_pick(cx, &matching).map(|i| matching[i].0)
    };
    match picked {
        Some(faction) => {
            params.formations[index].faction = faction;
            true
        }
        None => false,
    }
}

/// Whether a formation's faction is picked on this refresh: `once` (0) on
/// the first, `synch` (1) on a reroll, `every` (2) always, nothing else
/// never.
fn picks_faction(reroll_frequency: i32, first: bool, reroll: bool) -> bool {
    match reroll_frequency {
        0 => first,
        1 => reroll,
        2 => true,
        _ => false,
    }
}

/// `FEncounterParameters::Refresh(Time)` (`:624-728`).
///
/// - A reroll is due on the first call (`LastRerollTime`'s bits are those
///   of -1) or once `Time - LastRerollTime` reaches the reroll interval.
/// - The first call builds every formation's faction choices
///   ([`build_formation_factions`], results dropped).
/// - Each formation forgets the created objects its system no longer has
///   (`FSystemInfo::FindShip`), and a reroll zeroes its count.
/// - The formations whose reroll frequency asks for it pick their faction
///   ([`pick_faction`]): those not tied to another formation first, then
///   those tied; a failed pick ends the refresh with false.
/// - A reroll picks the permutation by weight (float sums; none when every
///   weight is 0), and the time is kept.
pub fn refresh(cx: &mut Cx<'_>, params: &mut EncounterParameters, time: f32) -> bool {
    let first = params.last_reroll_time.to_bits() == 0xbf80_0000;
    // The difference stays on the x87 stack; an unordered compare is no
    // reroll.
    let reroll = first
        || f64::from(time) - f64::from(params.last_reroll_time)
            >= f64::from(params.reroll_interval);
    if first {
        let tables = cx.logic.content.tables.clone();
        let factions = params.factions.clone();
        for formation in &mut params.formations {
            build_formation_factions(&tables, &factions, formation);
        }
    }
    for index in 0..params.formations.len() {
        let system = params.system;
        let objects = std::mem::take(&mut params.formations[index].created_objects);
        let kept: Vec<u32> = objects
            .into_iter()
            .filter(|&id| ships::find_in_system(cx, system, id).is_some())
            .collect();
        let formation = &mut params.formations[index];
        formation.created_objects = kept;
        if reroll {
            formation.field_0x78 = 0;
        }
        if picks_faction(formation.reroll_frequency, first, reroll)
            && formation.feeling_to_formation != 1
            && !pick_faction(cx, params, index)
        {
            return false;
        }
    }
    for index in 0..params.formations.len() {
        let formation = &params.formations[index];
        if picks_faction(formation.reroll_frequency, first, reroll)
            && formation.feeling_to_formation == 1
            && !pick_faction(cx, params, index)
        {
            return false;
        }
    }
    if reroll {
        let mut picked = None;
        let mut sum = 0.0f32;
        for p in &params.permutations {
            sum = (f64::from(sum) + f64::from(p.weight)) as f32;
        }
        if !params.permutations.is_empty() && sum != 0.0 {
            let target = (f64::from(cx.world.rng().rand())
                * f64::from(1.0f32 / 32768.0)
                * f64::from(sum)) as f32;
            let mut running = 0.0f32;
            for (at, p) in params.permutations.iter().enumerate() {
                running = (f64::from(running) + f64::from(p.weight)) as f32;
                if target <= running {
                    picked = Some(at);
                    break;
                }
            }
        }
        params.current_permutation = picked;
    }
    params.last_reroll_time = time;
    true
}

/// `FZonePopulation::Validate` (`Encounters.cpp:368-382`): every encounter
/// refreshes at time 0; the first that fails leaves the population invalid
/// and ends the validation.
pub fn validate(cx: &mut Cx<'_>, population: &mut super::zones::ZonePopulation) -> bool {
    for encounter in &mut population.encounters {
        if !refresh(cx, &mut encounter.parameters, 0.0) {
            population.valid = false;
            return false;
        }
    }
    population.valid = true;
    true
}

/// Runs `call` on encounter `encounter` of the zone, taken out of its
/// slot; the call must not dispatch (record 0008 point 1). `None` without
/// the zone or the encounter.
pub fn with_encounter_taken<R>(
    cx: &mut Cx<'_>,
    zone: ZoneRef,
    encounter: usize,
    call: impl FnOnce(&mut Cx<'_>, &mut EncounterParameters) -> R,
) -> Option<R> {
    let slot = cx
        .logic
        .content
        .state
        .systems
        .get_mut(&zone.system)?
        .zones
        .get_mut(zone.zone)?
        .population
        .encounters
        .get_mut(encounter)?;
    let mut parameters = std::mem::replace(&mut slot.parameters, placeholder());
    let take = NoDispatch::begin(&cx.logic.content);
    let result = call(cx, &mut parameters);
    take.end(&cx.logic.content);
    if let Some(slot) = cx
        .logic
        .content
        .state
        .systems
        .get_mut(&zone.system)
        .and_then(|r| r.zones.get_mut(zone.zone))
        .and_then(|z| z.population.encounters.get_mut(encounter))
    {
        slot.parameters = parameters;
    }
    Some(result)
}

/// The empty parameters that stand in a slot while its own are taken.
fn placeholder() -> EncounterParameters {
    EncounterParameters {
        nickname: 0,
        system: 0,
        zone: 0,
        reroll_interval: 0.0,
        last_reroll_time: -1.0,
        difficulty: -1,
        permutations: Vec::new(),
        current_permutation: None,
        formations: Vec::new(),
        factions: Vec::new(),
    }
}

// ---- the spawns ---------------------------------------------------------------------------------

/// The encounter `encounter` of the zone.
fn encounter<'a>(
    cx: &'a Cx<'_>,
    zone: ZoneRef,
    encounter: usize,
) -> Option<&'a EncounterParameters> {
    cx.logic
        .content
        .state
        .systems
        .get(&zone.system)?
        .zones
        .get(zone.zone)?
        .population
        .encounters
        .get(encounter)
        .map(|e| &e.parameters)
}

/// The encounter `encounter` of the zone, to change.
fn encounter_mut<'a>(
    cx: &'a mut Cx<'_>,
    zone: ZoneRef,
    encounter: usize,
) -> Option<&'a mut EncounterParameters> {
    cx.logic
        .content
        .state
        .systems
        .get_mut(&zone.system)?
        .zones
        .get_mut(zone.zone)?
        .population
        .encounters
        .get_mut(encounter)
        .map(|e| &mut e.parameters)
}

/// The zone's population.
fn population<'a>(cx: &'a Cx<'_>, zone: ZoneRef) -> Option<&'a ZonePopulation> {
    cx.logic
        .content
        .state
        .systems
        .get(&zone.system)?
        .zones
        .get(zone.zone)
        .map(|z| &z.population)
}

/// The zone's population, to change.
fn population_mut<'a>(cx: &'a mut Cx<'_>, zone: ZoneRef) -> Option<&'a mut ZonePopulation> {
    cx.logic
        .content
        .state
        .systems
        .get_mut(&zone.system)?
        .zones
        .get_mut(zone.zone)
        .map(|z| &mut z.population)
}

/// `BuildFormationBlocks(Formation, Blocks, Zone, ControllerId)`
/// (`EncounterParams.cpp:1037-1087`): per ship line, `min + rand() * (max -
/// min + 1) / 32768` blocks of the line's archetype
/// ([`npc_ships::apply_npc_ship_arch`] with the line's pilot job) and its
/// `make_class` ids (each once), owned by the controller. The first block
/// carries a guide description of the formation's behaviour wandering in
/// the zone; the longevity goes to the blocks' life (`+0x104`) and twice it
/// to the life left (`+0x108`). One block is built per line and copied for
/// each of its ships.
pub fn build_formation_blocks(
    cx: &mut Cx<'_>,
    formation: &EncounterFormation,
    zone_id: u32,
    controller: u32,
) -> Vec<NpcShipParams> {
    let tables = cx.logic.content.tables.clone();
    let mut blocks = Vec::new();
    for (line_index, line) in formation.ships.iter().enumerate() {
        let mut params = NpcShipParams {
            controller,
            ..NpcShipParams::default()
        };
        let arch = line.arch.and_then(|a| tables.npc_ship(a));
        params.level = arch.map_or(0, |a| a.level);
        if line_index == 0 {
            params.guide = Some(GuideDesc {
                kind: formation.behavior,
                wander_zone: Some(zone_id),
                ..GuideDesc::default()
            });
        }
        params.life_time = formation.longevity as f32;
        params.life_left = (f64::from(formation.longevity) * 2.0) as f32;
        let count = cx
            .world
            .rng()
            .rand()
            .wrapping_mul(line.max_count - line.min_count + 1)
            / 0x8000
            + line.min_count;
        for ship_index in 0..count {
            if let Some(arch) = arch {
                npc_ships::apply_npc_ship_arch(cx, &mut params, arch, None, line.pilot_job);
            }
            if ship_index > 0 {
                params.guide = None;
            }
            for &class in &line.make_classes {
                if !params.class_ids.contains(&class) {
                    params.class_ids.push(class);
                }
            }
            blocks.push(params.clone());
        }
    }
    blocks
}

/// `NameFormationBlocks(Blocks, Formation)` (`:1091-1121`): every block a
/// name of the formation's faction (`BuildNpcName` with the leader's guide
/// type as the name index, the formation's size, the block's level and
/// ship), its costume and voice ([`npc_ships::apply_npc_ship_faction`]) and
/// a named reputation; the leader's name fixes the designation of the
/// others, each the next escort.
pub fn name_formation_blocks(cx: &mut Cx<'_>, blocks: &mut [NpcShipParams], faction: u32) {
    let kind = blocks
        .first()
        .and_then(|b| b.guide.as_ref())
        .map_or(0, |g| g.kind);
    let mut request = NpcNameRequest {
        name_index: kind,
        formation_size: blocks.len() as i32,
        ..NpcNameRequest::default()
    };
    for (index, block) in blocks.iter_mut().enumerate() {
        request.rank_level = block.level;
        request.ship_arch = block.archetype;
        let mut name = NpcNameParams::default();
        npc_names::pick_npc_name_gender(cx, faction, &mut name);
        if npc_names::build_npc_name(cx, faction, &request, &mut name, true) {
            npc_ships::apply_npc_ship_faction(cx, faction, name.gender, block);
            if let Some(old) = block.reputation.take() {
                super::rep_handles::release(cx, old);
            }
            block.reputation = Some(npc_names::create_npc_name_reputation(cx, &name));
        }
        if index == 0 {
            request.desig = npc_names::first_name_value(&name.first).1;
            request.desig_number = npc_names::first_name_last_value(&name.first).1;
        }
        request.escort_number += 1;
    }
}

/// `SpacePopClampBlockAttitudes(PlayerShip, Params)` (`Populators.cpp:600-658`):
/// with the attack clamp, or for a player whose flags have bit 2, the
/// block's reputation and every NPC ship of the player's system on the
/// other side (SpacePop's or not; for a SpacePop block also the players'
/// ships) feel at least -0.59 towards each other.
pub fn clamp_block_attitudes(cx: &mut Cx<'_>, player: u32, block: &NpcShipParams) {
    let state = &cx.logic.content.state;
    let Some(player_state) = state.players.get(&player) else {
        return;
    };
    if state.clamps.attack_clamp == 0 && player_state.flags & 4 == 0 {
        return;
    }
    let Some(system) = player_state.ship.as_ref().and_then(|s| s.system) else {
        return;
    };
    let space_pop = state.space_pop;
    let block_rep = state.rep_handles.vibe(block.reputation);
    let floor = (f64::from(-0.6f32) + 0.01) as f32;
    let ids = api::system::scan_objects(cx, system, 0x40, Vector::ZERO, -1.0, 0x7f_0000)
        .unwrap_or_default();
    for id in ids {
        let state = &cx.logic.content.state;
        let mut other_side = block.controller == space_pop.0;
        if let Some(key) = ships::find_space_obj(state, system, id) {
            let pop = state
                .npc_ships
                .get(&key)
                .is_some_and(|s| s.is_space_pop_ship(space_pop));
            other_side = pop != other_side;
        } else if super::players::find_by_ship(&state.players, id).is_none() {
            continue;
        }
        if !other_side {
            continue;
        }
        let object_rep = api::space_obj::rep(cx, id).unwrap_or(-1);
        let attitude = api::reputation::attitude(cx, block_rep, object_rep).unwrap_or(0.0);
        if attitude < floor {
            let _ = api::reputation::set_attitude(cx, block_rep, object_rep, floor);
        }
        let attitude = api::reputation::attitude(cx, object_rep, block_rep).unwrap_or(0.0);
        if attitude < floor {
            let _ = api::reputation::set_attitude(cx, object_rep, block_rep, floor);
        }
    }
}

/// `FEncounterParameters::SpawnFormation(Request, Plan, Formation, Blocks)`
/// (`:944-1033`): the formation's blocks ([`build_formation_blocks`]; an
/// arrival in single player gives the first a life left of 76), placed
/// ([`spawn_placement::place_spawn_formation`]; none when that fails),
/// named ([`name_formation_blocks`]), the leader's guide given its targets
/// and the plan; a trade guide's leader its cargo ([`cargo::generate_cargo`])
/// and one unit of it fitted to each freighter or transport behind it.
/// Then the blocks are laid out in the formation (`formation_by_class`
/// through the faction's formations, else `formation`) from the leader's
/// placement, and clamped against SpacePop ([`clamp_block_attitudes`]).
pub fn spawn_formation(
    cx: &mut Cx<'_>,
    request: &SpawnRequest,
    params: &EncounterParameters,
    formation: &EncounterFormation,
    plan: &mut SpawnPlan,
) -> Vec<NpcShipParams> {
    let system = params.system;
    let zone = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .and_then(|r| r.find_zone(params.zone));
    let mut blocks = build_formation_blocks(cx, formation, params.zone, request.controller);
    if api::misc::single_player(cx)
        && request.arrival
        && let Some(first) = blocks.first_mut()
    {
        first.life_left = 76.0;
    }
    let placed = spawn_placement::place_spawn_formation(
        cx,
        request,
        system,
        zone,
        formation.zone_creation_distance,
        formation.arrival,
        plan,
        formation.faction,
        &mut blocks,
    );
    if !placed {
        for block in blocks {
            npc_ships::release_params(cx, block);
        }
        return Vec::new();
    }
    name_formation_blocks(cx, &mut blocks, formation.faction);
    if let Some(leader) = blocks.first_mut() {
        spawn_placement::set_leader_guide_targets(cx, system, plan, leader);
    }
    let mut trade = false;
    if let Some(desc) = blocks.first_mut().and_then(|l| l.guide.as_mut()) {
        desc.plan = *plan;
        trade = desc.kind == 2;
    }
    if trade && let Some(leader) = blocks.first_mut() {
        let mut picks = cargo::generate_cargo(cx, leader);
        if !picks.is_empty() {
            let freighter = neuron_core::id::create_id(b"class_freighter");
            let transport = neuron_core::id::create_id(b"class_transport");
            for block in blocks.iter_mut().skip(1) {
                if !block.class_ids.contains(&freighter) && !block.class_ids.contains(&transport) {
                    continue;
                }
                picks[0].count = 1;
                cargo::fit_cargo_to_hold(cx, block.archetype, &mut picks);
                for pick in &picks {
                    block.cargo.push(CargoDesc {
                        good: pick.good,
                        count: pick.count,
                    });
                }
            }
        }
    }
    let placement = blocks.first().map(|l| Transform {
        rotation: l.orientation,
        translation: l.position,
    });
    let tables = cx.logic.content.tables.clone();
    let formation_id = if formation.formation_by_class != 0 {
        tables
            .faction_props(formation.faction)
            .and_then(|p| p.formation(formation.formation_by_class))
            .unwrap_or(0)
    } else {
        formation.formation
    };
    if formation_id != 0
        && blocks.len() > 1
        && let Some(placement) = placement
    {
        let layout = tables.formation(formation_id).map(|_| formation_id);
        super::dock_queues::place_formation_blocks(cx, &mut blocks, layout, &placement);
    }
    for block in &blocks {
        clamp_block_attitudes(cx, request.player, block);
    }
    blocks
}

/// `FEncounterParameters::CheckDensityRestrictions(Placement, Restrictions,
/// Formation)` (`:909-940`): for each restriction whose class the formation
/// has (a line's ship classes or `make_class`, or its npc archetype's
/// classes), the NPC ships of that class within 3750 or the cone of the
/// placement plus the formation's lines of it may not exceed the count.
/// The archetype is read without a test; one that is missing has no
/// classes (D1061).
pub fn check_density_restrictions(
    cx: &Cx<'_>,
    params: &EncounterParameters,
    request: &SpawnRequest,
    restrictions: &[DensityRestriction],
    formation: &EncounterFormation,
) -> bool {
    let tables = &cx.logic.content.tables;
    for restriction in restrictions {
        let kind = restriction.kind;
        let mut count = 0;
        for ship in &formation.ships {
            if ship.ship_classes.contains(&kind) || ship.make_classes.contains(&kind) {
                count += 1;
                continue;
            }
            if ship.npc_arch == 0 {
                continue;
            }
            if tables
                .npc_ship(ship.npc_arch)
                .is_some_and(|a| a.npc_classes.contains(&kind))
            {
                count += 1;
            }
        }
        let near = spawn_placement::count_npc_ships_near(
            cx,
            params.system,
            &request.orientation,
            request.position,
            DENSITY_RADIUS_SP,
            RESTRICTION_CONE,
            Some(kind),
        );
        if count >= 1 && near + count > restriction.max_count {
            return false;
        }
    }
    true
}

/// Whether the player's state lets the formation of `faction` spawn:
/// the standing (a hostile faction, at or below -0.6, needs the hostile
/// clamp in single player; the player's flags refuse hostile factions with
/// bit 0 and the others with bit 1) and the combat state (a ship in combat
/// needs fewer hostile contacts than the zone's battle size, a calm ship
/// the zone's relief time of calm).
fn player_allows(cx: &mut Cx<'_>, zone: ZoneRef, request: &SpawnRequest, faction: u32) -> bool {
    let state = &cx.logic.content.state;
    let Some(player) = state.players.get(&request.player) else {
        return false;
    };
    let player_rep = player.reputation;
    let flags = player.flags;
    let ship = player
        .ship
        .as_ref()
        .map(|s| (s.in_combat, s.hostile_contact_count, s.calm_since));
    let standing = api::reputation::reputation(cx, player_rep, faction).unwrap_or(0.0);
    if api::misc::single_player(cx)
        && standing <= -0.6
        && cx.logic.content.state.clamps.hostile_clamp == 0
    {
        return false;
    }
    if flags & 1 != 0 && standing <= -0.6 {
        return false;
    }
    if flags & 2 != 0 && standing > -0.6 {
        return false;
    }
    let (battle_size, relief_time) =
        population(cx, zone).map_or((0, 0.0), |p| (p.max_battle_size, p.relief_time));
    let Some((in_combat, contacts, calm_since)) = ship else {
        return true;
    };
    if in_combat {
        return (contacts as u32) < battle_size as u32;
    }
    let mut elapsed = -1.0;
    if calm_since != -1.0 {
        elapsed = super::game_support::game_time(cx) - calm_since;
    }
    let stored = elapsed as f32;
    if elapsed > 0.0 {
        !(stored < relief_time)
    } else {
        calm_since == -1.0
    }
}

/// `FEncounterParameters::Spawn(Request, &bStarred, Restrictions)`
/// (`:763-905`) of encounter `index` of the zone: the current permutation's
/// formations whose star is `starred`, in order, each when
/// - the player allows it ([`player_allows`]);
/// - the formation may be created (simultaneously or with none left alive,
///   below `times_to_create`, with a faction and a ship line);
/// - every ship line has an archetype (its npc archetype, else one picked
///   for the faction; the count goes up once they have);
/// - (unstarred) the density restrictions allow it.
///
/// The blocks are spawned ([`spawn_formation`]) and their ships made (a
/// dock launch from the plan's launcher); each remembers the encounter and
/// joins the formation's created objects. The answer is the block count of
/// the last formation spawned; an unstarred pass stops at the first.
///
/// The C++ dereferences the current permutation and a permutation entry's
/// formation without a test; none spawns nothing here (D1061). An
/// arrival's ships made without a launch have their dock hails
/// suppressed for 30 s.
pub fn spawn(
    cx: &mut Cx<'_>,
    zone: ZoneRef,
    index: usize,
    request: &SpawnRequest,
    starred: bool,
) -> i32 {
    let Some(params) = encounter(cx, zone, index) else {
        return 0;
    };
    let Some(entries) = params
        .current_permutation
        .and_then(|p| params.permutations.get(p))
        .map(|p| p.entries.clone())
    else {
        return 0;
    };
    let restrictions =
        population(cx, zone).map_or_else(Vec::new, |p| p.density_restrictions.clone());
    let mut result = 0;
    for entry in entries {
        let Ok(at) = usize::try_from(entry.formation_index) else {
            continue;
        };
        let Some(faction) = encounter(cx, zone, index)
            .and_then(|p| p.formations.get(at))
            .map(|f| f.faction)
        else {
            continue;
        };
        if !player_allows(cx, zone, request, faction) {
            continue;
        }
        let Some(formation) = encounter(cx, zone, index).map(|p| p.formations[at].clone()) else {
            return result;
        };
        let mut spawned = false;
        if entry.star == starred
            && (formation.allow_simultaneous_creation || formation.created_objects.is_empty())
            && (formation.times_to_create == -1 || formation.field_0x78 < formation.times_to_create)
            && formation.faction != u32::MAX
            && !formation.ships.is_empty()
        {
            let tables = cx.logic.content.tables.clone();
            let mut archs = Vec::with_capacity(formation.ships.len());
            let mut all = true;
            for line in &formation.ships {
                let arch = if line.npc_arch == 0 {
                    cargo::pick_faction_ship_line_arch(cx, &tables, formation.faction, line, -1)
                } else {
                    tables.npc_ship(line.npc_arch).map(|_| line.npc_arch)
                };
                archs.push(arch);
                if arch.is_none() {
                    all = false;
                    break;
                }
            }
            if let Some(f) = encounter_mut(cx, zone, index).and_then(|p| p.formations.get_mut(at)) {
                for (line, arch) in f.ships.iter_mut().zip(&archs) {
                    line.arch = *arch;
                }
                if all {
                    f.field_0x78 += 1;
                }
            }
            if all && let Some(params) = encounter(cx, zone, index) {
                let formation = &params.formations[at];
                if starred
                    || check_density_restrictions(cx, params, request, &restrictions, formation)
                {
                    spawned = true;
                }
            }
        }
        if spawned {
            let Some(params) = encounter(cx, zone, index).cloned() else {
                return result;
            };
            let formation = params.formations[at].clone();
            let mut plan = SpawnPlan::default();
            let blocks = spawn_formation(cx, request, &params, &formation, &mut plan);
            if !blocks.is_empty() {
                let dock_target = if plan.kind == 2 { plan.dock_target } else { 0 };
                let made = npc_ships::create_npc_ships(cx, params.system, &blocks, dock_target);
                // An arrival's ships made without a launch: the dock hail
                // handler keeps quiet about each for 30 s
                // (`FGcsDockHailHandler::SuppressHail`, `:878`).
                if plan.kind != 2 && request.arrival {
                    for &key in &made {
                        let ship = npc_ships::space_obj(cx, key);
                        super::gcs::suppress_hail(cx, ship, None, 30);
                    }
                }
                for &key in &made {
                    if let Some(s) = npc_ships::get_mut(cx, key) {
                        s.encounter_params = params.nickname;
                    }
                }
                let ids: Vec<u32> = made.iter().map(|&k| npc_ships::space_obj(cx, k)).collect();
                if let Some(f) =
                    encounter_mut(cx, zone, index).and_then(|p| p.formations.get_mut(at))
                {
                    f.created_objects.extend(ids);
                }
            }
            result = blocks.len() as i32;
            for block in blocks {
                npc_ships::release_params(cx, block);
            }
        }
        if result != 0 && !starred {
            return result;
        }
    }
    result
}

/// `RefreshAndSpawnStarred` (`Encounters.cpp:401-421`): every encounter
/// refreshes at the time; those that fail leave the zone (an emptied zone
/// is invalid), the others spawn their starred formations. The blocks made.
fn refresh_and_spawn_starred(
    cx: &mut Cx<'_>,
    zone: ZoneRef,
    request: &SpawnRequest,
    time: f32,
) -> i32 {
    let mut created = 0;
    let mut at = 0;
    loop {
        let count = population(cx, zone).map_or(0, |p| p.encounters.len());
        if at >= count {
            break;
        }
        let ok = with_encounter_taken(cx, zone, at, |cx, p| refresh(cx, p, time)).unwrap_or(false);
        if ok {
            created += spawn(cx, zone, at, request, true);
            at += 1;
            continue;
        }
        if let Some(p) = population_mut(cx, zone) {
            p.encounters.remove(at);
            if p.encounters.is_empty() {
                p.valid = false;
            }
        }
    }
    created
}

/// `BuildEncounterPool`: each encounter's index with its chance.
fn encounter_pool(cx: &Cx<'_>, zone: ZoneRef) -> Vec<(u32, f32)> {
    population(cx, zone).map_or_else(Vec::new, |p| {
        p.encounters
            .iter()
            .enumerate()
            .map(|(i, e)| (i as u32, e.chance))
            .collect()
    })
}

/// `FZonePopulation::RollSpawnChance(Placement, &Scale, &MaxDensity)`
/// (`Encounters.cpp:463-494`): the limit (the zone's density for
/// `0xffffffff`) must be above the NPC ships near the placement (3750 in
/// single player, 9200 in multiplayer, or the 3750 cone); then the chance
/// `((limit * scale - ships) / 3) / (repop_time / scale / 3)`, clamped to
/// `[0, 1]`, against `rand() * 2^-15`.
pub fn roll_spawn_chance(
    cx: &mut Cx<'_>,
    zone: ZoneRef,
    request: &SpawnRequest,
    scale: f32,
    max_density: u32,
) -> bool {
    let Some((density, repop_time)) = population(cx, zone).map(|p| (p.density, p.repop_time))
    else {
        return false;
    };
    let limit = if max_density == u32::MAX {
        density as u32
    } else {
        max_density
    };
    let radius = if api::misc::single_player(cx) {
        DENSITY_RADIUS_SP
    } else {
        DENSITY_RADIUS_MP
    };
    let ships = spawn_placement::count_npc_ships_near(
        cx,
        zone.system,
        &request.orientation,
        request.position,
        radius,
        RESTRICTION_CONE,
        None,
    );
    if ships as u32 >= limit {
        return false;
    }
    let third = f64::from(1.0f32 / 3.0);
    let numerator = (f64::from(limit) * f64::from(scale) - f64::from(ships)) * third;
    let denominator = f64::from(repop_time) / f64::from(scale) * third;
    let mut chance = (numerator / denominator) as f32;
    if chance > 0.0 {
        if !(chance < 1.0) {
            chance = 1.0;
        }
    } else {
        chance = 0.0;
    }
    f64::from(cx.world.rng().rand()) * f64::from(1.0f32 / 32768.0) < f64::from(chance)
}

/// `FZonePopulation::Populate(Request, &Time, &Scale, &MaxDensity)`
/// (`Encounters.cpp:429-460`): the starred formations
/// ([`refresh_and_spawn_starred`]), then, when the spawn chance rolls
/// ([`roll_spawn_chance`]), encounters drawn by chance until one spawns
/// (the draw of that extra round is still made). The blocks made.
pub fn populate(
    cx: &mut Cx<'_>,
    zone: ZoneRef,
    request: &SpawnRequest,
    time: f32,
    scale: f32,
    max_density: u32,
) -> i32 {
    let starred = refresh_and_spawn_starred(cx, zone, request, time);
    let mut drawn = 0;
    if !roll_spawn_chance(cx, zone, request, scale, max_density) {
        return starred + drawn;
    }
    let mut pool = encounter_pool(cx, zone);
    while let Some(picked) = weighted_pick(cx, &pool) {
        if drawn != 0 {
            break;
        }
        drawn = spawn(cx, zone, pool[picked].0 as usize, request, false);
        pool.remove(picked);
    }
    starred + drawn
}

/// `FZonePopulation::PopulateUpTo(Request, &Time, &MaxCount)`
/// (`Encounters.cpp:497-531`): the starred formations, then encounters
/// drawn by chance while the blocks made stay below the count; each drawn
/// one refreshes (its result ignored) and spawns, and stays in the pool
/// while it spawns. The blocks made.
pub fn populate_up_to(
    cx: &mut Cx<'_>,
    zone: ZoneRef,
    request: &SpawnRequest,
    time: f32,
    max_count: u32,
) -> i32 {
    let starred = refresh_and_spawn_starred(cx, zone, request, time);
    let mut drawn = 0;
    let mut pool = encounter_pool(cx, zone);
    while let Some(picked) = weighted_pick(cx, &pool) {
        if (drawn + starred) as u32 >= max_count {
            break;
        }
        let index = pool[picked].0 as usize;
        let _ = with_encounter_taken(cx, zone, index, |cx, p| refresh(cx, p, time));
        let created = spawn(cx, zone, index, request, false);
        if created != 0 {
            drawn += created;
            continue;
        }
        pool.remove(picked);
    }
    starred + drawn
}
