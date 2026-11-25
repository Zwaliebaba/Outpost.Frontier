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

use super::encounter_params::{EncounterFormation, EncounterParameters};
use super::tables::ContentTables;
use super::{NoDispatch, cargo, ships};
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
                * f64::from(3.051_757_812_5e-5_f32)
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
