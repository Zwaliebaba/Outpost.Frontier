//! The faction, NPC ship archetype and cargo helpers of TU44
//! (`Content/Cargo.cpp`), over Content's static tables (work package
//! P3.4c, decision record 0008 point 10). The encounter spawn (P3.6d), the
//! random missions (P3.7) and the system records' trade-lane attacks
//! (P3.4d) call them.
//!
//! `GenerateCargo` (`:499-643`) builds a trade-guided NPC's cargo into its
//! guide description and ship parameters; it comes with those, in P3.6c and
//! P3.6d.
//!
//! The C++ reads a faction's properties and each `npc_ship` archetype
//! without a null test in several helpers; the shipped data has both for
//! every faction and every `npc_ship`, and the port skips a missing one
//! where the C++ would read through a null pointer (D797).

use super::encounter_params::{EncounterShip, add_unique};
use super::tables::{ContentTables, NpcShipArch};
use crate::api;
use crate::server::Cx;

/// The thresholds of the feeling masks: friendly (1) from 0.2 (a NaN is
/// not), neutral (2) above -0.2, else hostile (4).
#[must_use]
pub fn feeling_mask(feeling: f32) -> u32 {
    if feeling >= 0.2 {
        1
    } else if feeling > -0.2 {
        2
    } else {
        4
    }
}

/// `GetFactionFeelingMask`: the mask of `from`'s feeling towards `to`
/// (`pub::Reputation::GetReputation`, group to group; 0 when it fails, where
/// the C++ leaves its local unset).
pub fn faction_feeling_mask(cx: &Cx<'_>, from: u32, to: u32) -> u32 {
    feeling_mask(api::reputation::group_reputation(cx, from, to).unwrap_or(0.0))
}

/// `GetVibeFeelingMask`: the mask of the vibe's feeling towards `group`.
pub fn vibe_feeling_mask(cx: &mut Cx<'_>, group: u32, vibe: i32) -> u32 {
    feeling_mask(api::reputation::reputation(cx, vibe, group).unwrap_or(0.0))
}

/// `GetHostileFactionShares`: of the total weight of `factions`, the share
/// of the factions `group` is hostile to and the share of the factions
/// hostile to `group`; both 0 for an empty list. The sums are float stores;
/// the shares are the unrounded `1 / total` times each sum.
pub fn hostile_faction_shares(cx: &Cx<'_>, factions: &[(u32, f32)], group: u32) -> (f32, f32) {
    if factions.is_empty() {
        return (0.0, 0.0);
    }
    let mut hostile_from = 0.0_f32;
    let mut hostile_to = 0.0_f32;
    let mut total = 0.0_f32;
    for &(faction, weight) in factions {
        if faction_feeling_mask(cx, group, faction) & 4 != 0 {
            hostile_from = (f64::from(hostile_from) + f64::from(weight)) as f32;
        }
        if faction_feeling_mask(cx, faction, group) & 4 != 0 {
            hostile_to = (f64::from(hostile_to) + f64::from(weight)) as f32;
        }
        total = (f64::from(total) + f64::from(weight)) as f32;
    }
    let scale = 1.0 / f64::from(total);
    (
        (f64::from(hostile_from) * scale) as f32,
        (scale * f64::from(hostile_to)) as f32,
    )
}

/// `FindNpcShipArch` of a faction's `npc_ship` id.
fn arch(tables: &ContentTables, id: u32) -> Option<&NpcShipArch> {
    tables.npc_ship(id)
}

/// The `npc_ship` ids of a faction.
fn npc_ships(tables: &ContentTables, group: u32) -> &[u32] {
    tables
        .faction_props(group)
        .map_or(&[][..], |p| p.npc_ships.as_slice())
}

/// `GetNpcArchFactions`: every faction (in `GetFactionGroups` order) with
/// properties whose `npc_ship` list holds every id of `npc_archs`.
#[must_use]
pub fn npc_arch_factions(tables: &ContentTables, npc_archs: &[u32]) -> Vec<u32> {
    let mut out = Vec::new();
    for &group in &tables.factions.groups {
        let Some(props) = tables.faction_props(group) else {
            continue;
        };
        if npc_archs.iter().all(|a| props.npc_ships.contains(a)) {
            add_unique(&mut out, group);
        }
    }
    out
}

/// `GetNpcClassFactions`: every faction with an `npc_ship` archetype that
/// shares an npc class with `classes`.
#[must_use]
pub fn npc_class_factions(tables: &ContentTables, classes: &[u32]) -> Vec<u32> {
    let mut out = Vec::new();
    for &group in &tables.factions.groups {
        for &id in npc_ships(tables, group) {
            let Some(a) = arch(tables, id) else {
                continue;
            };
            if classes.iter().any(|c| a.npc_classes.contains(c)) {
                add_unique(&mut out, group);
                break;
            }
        }
    }
    out
}

/// `GetShipLineFactions`: every faction with an `npc_ship` archetype that
/// fits the ship line ([`npc_arch_fits_ship_line`]).
#[must_use]
pub fn ship_line_factions(tables: &ContentTables, ship: &EncounterShip) -> Vec<u32> {
    let mut out = Vec::new();
    for &group in &tables.factions.groups {
        for &id in npc_ships(tables, group) {
            if arch(tables, id).is_some_and(|a| npc_arch_fits_ship_line(tables, ship, a)) {
                add_unique(&mut out, group);
                break;
            }
        }
    }
    out
}

/// `NpcArchFitsShipLine`: every class of the line is matched by the
/// archetype: a ship class (`FindShipClass`) through one of its members
/// among the archetype's npc classes; any other id among its npc classes
/// or its ship's mission properties.
#[must_use]
pub fn npc_arch_fits_ship_line(
    tables: &ContentTables,
    ship: &EncounterShip,
    arch: &NpcShipArch,
) -> bool {
    ship.ship_classes
        .iter()
        .all(|class| match tables.ship_class(*class) {
            None => {
                arch.npc_classes.contains(class)
                    || tables
                        .ship_mission_properties
                        .get(&arch.ship_archetype)
                        .is_some_and(|p| p.contains(class))
            }
            Some(members) => members.iter().any(|m| arch.npc_classes.contains(m)),
        })
}

/// `FactionHasNpcClass`: one of the faction's `npc_ship` archetypes has
/// one of `classes`; false without properties.
#[must_use]
pub fn faction_has_npc_class(tables: &ContentTables, group: u32, classes: &[u32]) -> bool {
    let Some(props) = tables.faction_props(group) else {
        return false;
    };
    props.npc_ships.iter().any(|&id| {
        classes
            .iter()
            .any(|c| arch(tables, id).is_some_and(|a| a.npc_classes.contains(c)))
    })
}

/// `GetFactionShipLineArchs`: the faction's `npc_ship` archetypes (ids) that
/// fit the ship line, in `npc_ship` order.
#[must_use]
pub fn faction_ship_line_archs(
    tables: &ContentTables,
    group: u32,
    ship: &EncounterShip,
) -> Vec<u32> {
    npc_ships(tables, group)
        .iter()
        .copied()
        .filter(|&id| arch(tables, id).is_some_and(|a| npc_arch_fits_ship_line(tables, ship, a)))
        .collect()
}

/// `PickFactionShipLineArch`: a random fitting `npc_ship` archetype (an
/// id) of the faction. With `level > 0` only those within one level of it,
/// else the closest one (the first strictly closer one wins). `None` when
/// the faction has no properties or nothing fits. One `rand()` when more
/// than one candidate remains: index `rand() * count / 0x8000`.
pub fn pick_faction_ship_line_arch(
    cx: &mut Cx<'_>,
    tables: &ContentTables,
    group: u32,
    ship: &EncounterShip,
    level: i32,
) -> Option<u32> {
    let mut candidates = Vec::new();
    let mut closest: Option<u32> = None;
    if tables.faction_props(group).is_some() {
        candidates = faction_ship_line_archs(tables, group, ship);
        let level_of = |id: u32| arch(tables, id).map_or(0, |a| a.level);
        let mut index = 0;
        while index < candidates.len() {
            if level <= 0 {
                index += 1;
                continue;
            }
            let id = candidates[index];
            let arch_level = level_of(id);
            let keep = arch_level >= level - 1 && arch_level <= level + 1;
            if closest.is_none_or(|c| (level - arch_level).abs() < (level - level_of(c)).abs()) {
                closest = Some(id);
            }
            if keep {
                index += 1;
            } else {
                candidates.remove(index);
            }
        }
        if let Some(c) = closest
            && candidates.is_empty()
        {
            candidates.push(c);
        }
    }
    let count = candidates.len() as i32;
    if count == 0 {
        return closest;
    }
    let mut index = 0;
    if count - 1 != 0 {
        index = cx.world.rng().rand().wrapping_mul(count) / 0x8000;
    }
    candidates.get(index as usize).copied().or(closest)
}

/// A cargo candidate (`FCargoPick`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CargoPick {
    /// The good.
    pub good: u32,
    /// The count; [`fit_cargo_to_hold`] rewrites it.
    pub count: u32,
    /// The weight.
    pub weight: f32,
}

/// One entry of a base's market list (`FBaseCommodity`): a good and, for a
/// good in demand, its minimum inventory (1 for a good for sale).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BaseCommodity {
    /// The good.
    pub good: u32,
    /// The count.
    pub count: u32,
}

/// MSVCRT `_ftol` to an unsigned dword: 64-bit truncation, the low dword.
fn ftol_low(value: f64) -> u32 {
    neuron_core::num::trunc_to_i32_via_i64(value) as u32
}

/// `FitCargoToHold`: 80% of the ship archetype's hold, less the volume the
/// counts already listed take, shared out by weight; each share becomes a
/// count (share over the good's volume) and a pick left with none is
/// dropped.
pub fn fit_cargo_to_hold(cx: &Cx<'_>, ship: u32, cargo: &mut Vec<CargoPick>) {
    let hold = api::misc::cargo_hold_size(cx, ship).unwrap_or(0);
    let mut free = ftol_low(f64::from(hold) * 0.8);
    let volume = |good: u32| api::misc::good_properties(cx, good).map_or(0.0, |(v, _)| v);
    let mut total = 0u32;
    for pick in cargo.iter() {
        total = ftol_low(f64::from(total) + f64::from(pick.weight));
        if pick.count == 0 {
            continue;
        }
        let used = ftol_low(f64::from(pick.count) * f64::from(volume(pick.good)));
        free = free.saturating_sub(used);
    }
    cargo.retain_mut(|pick| {
        if pick.weight > 0.0 && total != 0 {
            let share = (f64::from(pick.weight) / f64::from(total) * f64::from(free as i32)) as f32;
            let v = volume(pick.good);
            pick.count = if v > 0.0 && v <= share {
                ftol_low(f64::from(share) / f64::from(v))
            } else {
                0
            };
        }
        pick.count != 0
    });
}

/// `FilterCargoPicks`: for every good of `goods` (outer) and every market
/// entry of it (inner), a pick weighted by its count (1 for 0), appended.
pub fn filter_cargo_picks(goods: &[u32], market: &[BaseCommodity], out: &mut Vec<CargoPick>) {
    for &good in goods {
        for entry in market.iter().filter(|e| e.good == good) {
            let weight = if entry.count == 0 { 1 } else { entry.count };
            out.push(CargoPick {
                good: entry.good,
                count: entry.count,
                weight: f64::from(weight) as f32,
            });
        }
    }
}

/// `PickWeightedCargo`: a pick by weight, `rand() / 32768` of the total;
/// the last pick for an empty list, a zero total or a roll past the last
/// sum, where the C++ answers the end of the list (which no caller reaches).
pub fn pick_weighted_cargo(cx: &mut Cx<'_>, picks: &[CargoPick]) -> Option<CargoPick> {
    let sum: f64 = picks.iter().map(|p| f64::from(p.weight)).sum();
    let total = sum as f32;
    if picks.is_empty() || total == 0.0 {
        return picks.last().copied();
    }
    let roll = (f64::from(cx.world.rng().rand()) * (1.0 / 32_768.0) * f64::from(total)) as f32;
    let mut running = 0.0;
    for pick in picks {
        running += f64::from(pick.weight);
        if f64::from(roll) <= running {
            return Some(*pick);
        }
    }
    picks.last().copied()
}
