//! The combat reputation of TU56 (`Content/CombatReputation.cpp`; record
//! 0008 point 7; work package P3.6c, given to it by the owner on
//! 2026-11-25): how damage and kills by players change attitudes and
//! reputations, and how NPCs near a player side against an attacker.
//!
//! - [`on_damage_reputation`] (`OnDamageReputation`, SpaceSupport's
//!   `ObjectDamaged`): a player's hits on a ship or a solar build up the
//!   attacker slot of the player's ship; at 25 the victim's controller is
//!   warned (`DamageReputation`, not provoked), at 50 (or at once for
//!   causes 5 and 6) the victim and its formation turn hostile.
//! - [`apply_kill_reputation`] (`ApplyKillReputation`, a kill): the
//!   killer's reputation with the victim's faction moves by the faction's
//!   kill rate, with empathy.
//! - [`on_heartbeat_reputation`] (`OnHeartbeatReputation`, SpaceSupport's
//!   heartbeat): around every player's ship, an object that likes another
//!   turns hostile to whoever is hostile to its friend
//!   (`NpcTurnedHostile`).
//!
//! The x87 compares keep their unordered cases as the C++'s jumps do.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::Vector;
use neuron_server::controller::ControllerId;

use super::players::{self, PlayerShip};
use super::ships;
use crate::api;
use crate::events::{ControllerMessage, ObjectDamaged};
use crate::server::Cx;

/// `GNpcAllyHostileAttitude` (-0.6, `:63`).
const ALLY_HOSTILE_ATTITUDE: f32 = -0.6;

/// One of the eight attacker slots of a space object (`FSpaceObjAttacker`,
/// TU20 `+0x14`), kept on the attacking player's ship, keyed by the
/// damaged object.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Attacker {
    /// `AttackerId` (`+0x00`): the damaged object, 0 for a free slot.
    pub victim: u32,
    /// `Hostility` (`+0x04`).
    pub hostility: f32,
    /// `FirstTime` (`+0x08`).
    pub first_time: f32,
    /// `LastTime` (`+0x0c`).
    pub last_time: f32,
    /// `HostilityTime` (`+0x10`).
    pub hostility_time: f32,
    /// `bWarned` (`+0x14`).
    pub warned: bool,
    /// `bProvoked` (`+0x15`).
    pub provoked: bool,
}

/// `FContentSpaceObj::FindOrAllocAttacker(AttackerId, &Out)`
/// (`NpcBase.cpp:155-190`): the slot of the object, else an empty one (the
/// last), else the one hit least recently (the first of the smallest
/// time), cleared.
pub fn find_or_alloc_attacker(slots: &mut [Attacker; 8], victim: u32) -> usize {
    let mut min_time = f32::MAX;
    let mut oldest: Option<usize> = None;
    let mut empty: Option<usize> = None;
    for (index, slot) in slots.iter().enumerate() {
        if slot.victim == victim {
            return index;
        }
        if slot.victim == 0 {
            empty = Some(index);
        }
        if min_time > slot.last_time {
            oldest = Some(index);
            min_time = slot.last_time;
        }
    }
    // Every slot has a time below `FLT_MAX` unless one is NaN or `FLT_MAX`
    // itself; the C++ indexes slot -1 then (D853: slot 0).
    let chosen = empty.or(oldest).unwrap_or(0);
    slots[chosen] = Attacker::default();
    chosen
}

/// `DamageReputation {bProvoked, Attacker, Victim, System}` to every
/// controller.
fn send_damage_reputation(
    cx: &mut Cx<'_>,
    provoked: bool,
    attacker: u32,
    victim: u32,
    system: u32,
) {
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::DamageReputation {
            provoked,
            attacker,
            victim,
            system,
        },
    );
}

/// The attacker slots of the space object `id` of `system`'s record
/// (`FindShipOrSolar`): a player's ship or an NPC ship of the record. A
/// solar NPC's slots are not kept: the attacker is a player's ship, which
/// the record's players or space objects hold (D853).
fn attacker_slots<'a>(cx: &'a mut Cx<'_>, system: u32, id: u32) -> Option<&'a mut [Attacker; 8]> {
    let key = ships::find_in_system(cx, system, id)?;
    ships::base_mut(&mut cx.logic.content.state, key).map(|s| &mut s.attackers)
}

/// `HandleDamageReputation(Event)` (`:105-268`), SpaceSupport's
/// `ObjectDamaged` (`OnDamageReputation`).
#[allow(clippy::too_many_lines)]
pub fn on_damage_reputation(cx: &mut Cx<'_>, event: &ObjectDamaged) {
    let mut event = *event;
    let cause = event.cause;
    if !(2..=7).contains(&cause) {
        return;
    }
    if players::find_by_ship(&cx.logic.content.state.players, event.inflictor).is_none() {
        return;
    }
    if event.inflictor == event.id {
        return;
    }
    if let Ok(parent) = api::space_obj::solar_parent(cx, event.id) {
        event.id = parent;
    }
    let mut victim_type = 0;
    if event.id != 0 {
        victim_type = api::space_obj::object_type(cx, event.id).unwrap_or(0);
        if victim_type & 0x7f_b110 == 0 {
            return;
        }
    }
    let attacker_rep = api::space_obj::rep(cx, event.inflictor).unwrap_or(-1);
    let attacker_affiliation = api::reputation::affiliation(cx, attacker_rep).unwrap_or(u32::MAX);
    let victim_rep = api::space_obj::rep(cx, event.id).unwrap_or(-1);
    let victim_affiliation = api::reputation::affiliation(cx, victim_rep).unwrap_or(u32::MAX);
    if victim_type == 0x2000 && event.relative_health <= 0.0 && event.equip_type == 0x800 {
        apply_faction_reputation_change(cx, attacker_rep, -0.1, victim_affiliation, true);
    }
    if attacker_rep == -1 || victim_rep == -1 {
        return;
    }
    if attacker_affiliation == u32::MAX && victim_affiliation == u32::MAX {
        return;
    }
    let attacker_attitude = api::reputation::attitude(cx, attacker_rep, victim_rep).unwrap_or(0.0);
    let victim_attitude = api::reputation::attitude(cx, victim_rep, attacker_rep).unwrap_or(0.0);
    if attacker_attitude <= -0.6 || victim_attitude <= -0.6 || victim_attitude >= 1.0 {
        return;
    }
    let now = api::misc::rw_time(cx);
    // The record is not null-tested (D843: no record, nothing).
    let system = event.system;
    if attacker_slots(cx, system, event.inflictor).is_none() {
        return;
    }
    if api::misc::single_player(cx) {
        let state = &cx.logic.content.state;
        if let Some(first) = state.players.values().next()
            && first.live_mission != 0
            && first.ship.as_ref().is_some_and(|s| s.in_combat)
        {
            return;
        }
    }
    let victim = event.id;
    let Some(slots) = attacker_slots(cx, system, event.inflictor) else {
        return;
    };
    let at = find_or_alloc_attacker(slots, victim);
    if cause == 5 || cause == 6 {
        send_damage_reputation(cx, true, event.inflictor, victim, system);
        let _ = api::reputation::set_attitude(cx, victim_rep, attacker_rep, -1.0);
        return;
    }
    let Some(slots) = attacker_slots(cx, system, event.inflictor) else {
        return;
    };
    let slot = &mut slots[at];
    let mut elapsed = now - f64::from(slot.last_time);
    slot.last_time = now as f32;
    if slot.victim != victim {
        slot.victim = victim;
        slot.first_time = now as f32;
        slot.hostility_time = now as f32;
        elapsed = 0.0;
    }
    if elapsed > 1.0 {
        slot.hostility_time = now as f32;
    }
    if now - f64::from(slot.hostility_time) > 1.0 {
        slot.hostility_time = now as f32;
        slot.hostility = (f64::from(slot.hostility) + 10.0) as f32;
    }
    slot.hostility = (f64::from(event.relative_damage) * 50.0 + f64::from(slot.hostility)) as f32;
    if slot.hostility >= 50.0 && !slot.provoked {
        slot.provoked = true;
        send_damage_reputation(cx, true, event.inflictor, victim, system);
        let _ = api::reputation::set_attitude(cx, victim_rep, attacker_rep, -1.0);
        let state = &cx.logic.content.state;
        let Some(key) = ships::find_space_obj(state, system, victim) else {
            return;
        };
        let group = ships::formation_id(state, ships::ShipKey::Npc(key));
        if group == u32::MAX {
            return;
        }
        let members: Vec<u32> = state
            .systems
            .get(&system)
            .map(|r| {
                r.space_objs
                    .iter()
                    .filter_map(|k| state.npc_ships.get(k))
                    .filter(|s| s.base.formation_id == group)
                    .map(|s| s.base.ship)
                    .collect()
            })
            .unwrap_or_default();
        for member in members {
            let member_rep = api::space_obj::rep(cx, member).unwrap_or(-1);
            let _ = api::reputation::set_attitude(cx, member_rep, attacker_rep, -1.0);
        }
        return;
    }
    if !(slot.hostility > 25.0) || slot.warned {
        return;
    }
    slot.warned = true;
    send_damage_reputation(cx, false, event.inflictor, victim, system);
}

/// `ClampFactionReputation(Sum)` (`FactionProps.cpp:196-211`): the sum
/// within `[-0.9, 0.9]` (`±1.0f ∓ 0.1` in double), the float of the sum
/// otherwise.
#[must_use]
pub fn clamp_faction_reputation(sum: f64) -> f32 {
    let upper = f64::from(1.0_f32) - 0.1;
    let lower = f64::from(-1.0_f32) + 0.1;
    let sum_f = sum as f32;
    let candidate = if upper < sum { upper } else { f64::from(sum_f) };
    if candidate <= lower {
        return lower as f32;
    }
    if upper < f64::from(sum_f) {
        return upper as f32;
    }
    sum_f
}

/// `ApplyFactionReputationChange(Vibe, Delta, Group, bEmpathy)`
/// (`FactionProps.cpp:1041-1075`): the vibe's reputation with the group
/// moves by the delta (clamped); with empathy, its reputation with every
/// group of the group's empathy list by the delta times the rate. A
/// failed read keeps the value read before.
pub fn apply_faction_reputation_change(
    cx: &mut Cx<'_>,
    vibe: i32,
    delta: f32,
    group: u32,
    empathy: bool,
) {
    if delta == 0.0 || group == u32::MAX {
        return;
    }
    let mut reputation = 0.0_f32;
    if let Ok(r) = api::reputation::reputation(cx, vibe, group) {
        reputation = r;
    }
    let clamped = clamp_faction_reputation(f64::from(reputation) + f64::from(delta));
    let _ = api::reputation::set_reputation(cx, vibe, group, clamped);
    if !empathy {
        return;
    }
    let tables = cx.logic.content.tables.clone();
    let Some(entry) = tables.empathy.get(&group) else {
        return;
    };
    for rate in &entry.empathy_rates {
        if let Ok(r) = api::reputation::reputation(cx, vibe, rate.group) {
            reputation = r;
        }
        let sum = f64::from(delta) * f64::from(rate.rate) + f64::from(reputation);
        let clamped = clamp_faction_reputation(sum);
        let _ = api::reputation::set_reputation(cx, vibe, rate.group, clamped);
    }
}

/// `ApplyKillReputation(Ship, &Killer)` (`:276-299`): unless the
/// population's attack clamp is set, a killer who is a player moves his
/// reputation with the victim's faction by its kill rate (event 0), with
/// empathy.
pub fn apply_kill_reputation(cx: &mut Cx<'_>, ship: u32, killer: u32) {
    if cx.logic.content.state.clamps.attack_clamp == 1 {
        return;
    }
    let state = &cx.logic.content.state;
    let Some(player) = players::find_by_ship(&state.players, killer) else {
        return;
    };
    let killer_rep = state.players.get(&player).map_or(-1, |p| p.reputation);
    // The C++'s slots are uninitialised but always written.
    let ship_rep = api::space_obj::rep(cx, ship).unwrap_or(-1);
    let affiliation = api::reputation::affiliation(cx, ship_rep).unwrap_or(u32::MAX);
    let rate = cx.logic.content.tables.faction_event_rate(affiliation, 0);
    if rate == 0.0 {
        return;
    }
    apply_faction_reputation_change(cx, killer_rep, rate, affiliation, true);
}

/// One object of the heartbeat's scan table (`FNpcReputationScanEntry`).
#[derive(Clone, Copy, Debug)]
struct ScanEntry {
    valid: bool,
    id: u32,
    position: Vector,
    kind: u32,
    affiliation: u32,
    reputation: i32,
    player: bool,
    attitudes: [f32; 32],
}

/// The distance of `UpdateNpcReputations` (`ScanDistance`): the x87 sum
/// `(dz² + dy²) + dx²` of the float differences, then `FastLength`.
fn scan_distance(from: Vector, to: Vector) -> f64 {
    let dx = f64::from(from.x) - f64::from(to.x);
    let dy = f64::from(from.y) - f64::from(to.y);
    let dz = f64::from(from.z) - f64::from(to.z);
    super::space_paths::fast_length((dz * dz + dy * dy) + dx * dx)
}

/// `UpdateNpcReputations` (`OnHeartbeatReputation`, `:302-559`).
#[allow(clippy::too_many_lines)]
pub fn on_heartbeat_reputation(cx: &mut Cx<'_>) {
    let multiplayer = !api::misc::single_player(cx);
    let systems: Vec<u32> = cx.logic.content.state.systems.keys().copied().collect();
    for system in systems {
        let player_ids: Vec<u32> = cx
            .logic
            .content
            .state
            .systems
            .get(&system)
            .map(|r| r.players.clone())
            .unwrap_or_default();
        if player_ids.is_empty() {
            continue;
        }
        for &player in &player_ids {
            let Some(ship) = player_ship_id(cx, player) else {
                continue;
            };
            let position = ships::position(cx, ships::ShipKey::Player(player));
            let found = api::system::scan_objects(cx, system, 0x20, position, 5000.0, 0x7f_31e0)
                .unwrap_or_default();
            let _ = ship;
            let mut table: Vec<ScanEntry> = Vec::with_capacity(found.len());
            for &id in &found {
                let mut entry = ScanEntry {
                    valid: true,
                    id,
                    position: Vector::ZERO,
                    kind: 0,
                    affiliation: u32::MAX,
                    reputation: -1,
                    player: false,
                    attitudes: [0.0; 32],
                };
                entry.player = player_ids
                    .iter()
                    .any(|&p| player_ship_id(cx, p) == Some(entry.id));
                if entry.id == 0 {
                    table.push(entry);
                    continue;
                }
                let state = &cx.logic.content.state;
                let mut excluded = false;
                if let Some(key) = ships::find_space_obj(state, system, entry.id)
                    && let Some(npc) = state.npc_ships.get(&key)
                {
                    if !multiplayer && (npc.creation_kind == 0 || npc.creation_kind == 1) {
                        excluded = true;
                    }
                    if npc.base.lane_entry != 0 {
                        excluded = true;
                    }
                }
                let is_mission_object = state
                    .systems
                    .get(&system)
                    .is_some_and(|r| r.mission_objects.contains(&id));
                if (!multiplayer && is_mission_object) || excluded {
                    entry.valid = false;
                    table.push(entry);
                    continue;
                }
                let location = api::space_obj::location(cx, entry.id);
                if let Ok((p, _)) = location {
                    entry.position = p;
                }
                entry.kind = api::space_obj::object_type(cx, entry.id).unwrap_or(0);
                let rep = if entry.kind & 0xfdff != 0 {
                    api::space_obj::solar_rep(cx, entry.id)
                        .or_else(|_| api::space_obj::rep(cx, entry.id))
                } else {
                    api::space_obj::rep(cx, entry.id)
                };
                if let Ok(r) = rep {
                    entry.reputation = r;
                }
                let affiliation = api::reputation::affiliation(cx, entry.reputation);
                if let Ok(a) = affiliation {
                    entry.affiliation = a;
                }
                if location.is_err() || rep.is_err() || affiliation.is_err() {
                    entry.id = 0;
                    entry.valid = false;
                }
                table.push(entry);
            }
            let count = table.len();
            for a in 0..count {
                if !table[a].valid || table[a].id == 0 {
                    continue;
                }
                for b in 0..count {
                    if !table[b].valid || table[b].id == 0 {
                        continue;
                    }
                    if a == b {
                        table[a].attitudes[b] = 0.0;
                    } else if let Ok(v) =
                        api::reputation::attitude(cx, table[a].reputation, table[b].reputation)
                    {
                        table[a].attitudes[b] = v;
                    }
                }
            }
            side_against_attackers(cx, system, &table);
        }
    }
}

/// The ship id of `player`'s ship state.
fn player_ship_id(cx: &Cx<'_>, player: u32) -> Option<u32> {
    cx.logic
        .content
        .state
        .players
        .get(&player)
        .and_then(|p| p.ship.as_ref())
        .map(|s: &PlayerShip| s.ship)
}

/// The third pass of `UpdateNpcReputations`: A, liking B within 5000,
/// turns hostile to C when C is hostile to B within 5000 of B (or to B in
/// the mirrored cases); each change is `NpcTurnedHostile` to SpaceSupport.
fn side_against_attackers(cx: &mut Cx<'_>, system: u32, table: &[ScanEntry]) {
    let count = table.len();
    for a in 0..count {
        let ea = table[a];
        if !ea.valid || ea.id == 0 || ea.affiliation == u32::MAX {
            continue;
        }
        for b in 0..count {
            if a == b {
                continue;
            }
            let eb = table[b];
            if !eb.valid || eb.id == 0 {
                continue;
            }
            if scan_distance(ea.position, eb.position) > 5000.0 {
                continue;
            }
            let ab = ea.attitudes[b];
            if ab < 0.6 {
                continue;
            }
            for c in 0..count {
                if b == c {
                    continue;
                }
                let ec = table[c];
                if !ec.valid || ec.id == 0 {
                    continue;
                }
                if eb.player && ec.player {
                    continue;
                }
                if eb.affiliation == u32::MAX && ec.affiliation == u32::MAX {
                    continue;
                }
                if ea.kind & 0x80 != 0 && (eb.kind & 0x80 != 0 || ec.kind & 0x80 != 0) {
                    continue;
                }
                if scan_distance(eb.position, ec.position) > 5000.0 {
                    continue;
                }
                let ac = ea.attitudes[c];
                if ac < -0.6 {
                    continue;
                }
                let bc = eb.attitudes[c];
                let cb = ec.attitudes[b];
                if ec.affiliation == u32::MAX {
                    if !(bc <= -0.6) {
                        continue;
                    }
                } else if !(cb <= -0.6) {
                    continue;
                }
                let likes_b = ab >= 0.6;
                let likes_c = ac >= 0.6;
                let (object, other) = if eb.affiliation == u32::MAX {
                    if !likes_c {
                        continue;
                    }
                    let b_rep = api::reputation::reputation(cx, eb.reputation, ec.affiliation)
                        .unwrap_or(0.0);
                    let gap = (f64::from(b_rep) - f64::from(cb)).abs();
                    if 0.01 + 0.01 > gap {
                        if ac >= 1.0 {
                            continue;
                        }
                        let _ = api::reputation::set_attitude(
                            cx,
                            ea.reputation,
                            ec.reputation,
                            ALLY_HOSTILE_ATTITUDE,
                        );
                        (ea.id, eb.id)
                    } else {
                        if ab >= 1.0 {
                            continue;
                        }
                        let _ = api::reputation::set_attitude(
                            cx,
                            ea.reputation,
                            eb.reputation,
                            ALLY_HOSTILE_ATTITUDE,
                        );
                        (ea.id, ec.id)
                    }
                } else if likes_b {
                    if likes_c || ac >= 1.0 || !(ac <= -0.6) {
                        continue;
                    }
                    let _ = api::reputation::set_attitude(
                        cx,
                        ea.reputation,
                        ec.reputation,
                        ALLY_HOSTILE_ATTITUDE,
                    );
                    (ea.id, eb.id)
                } else {
                    if ab >= 1.0 || !(ab <= -0.6) {
                        continue;
                    }
                    let _ = api::reputation::set_attitude(
                        cx,
                        ea.reputation,
                        eb.reputation,
                        ALLY_HOSTILE_ATTITUDE,
                    );
                    (ea.id, eb.id)
                };
                let to = cx.logic.content.state.space_support;
                let _ = cx.dispatch(
                    to,
                    &mut ControllerMessage::NpcTurnedHostile {
                        object,
                        other,
                        system,
                    },
                );
            }
        }
    }
}

/// One hostile contact of a player's ship (`FHostileContact`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct HostileContact {
    /// `Object`: an NPC ship of the system.
    pub object: crate::events::NpcShipKey,
    /// `TimeNear`.
    pub time_near: f32,
    /// `TimeSinceSeen`.
    pub time_since_seen: f32,
}

/// `FContentPlayerShip::UpdateHostileContacts(DeltaTime)`
/// (`PlayerState.cpp:999-1089`), SpaceSupport's heartbeat: the hostile NPC
/// ships of the ship's system within 2500 are its contacts; the first
/// contact puts the ship in combat and mutes the system's NPC ships, the
/// last one gone ends it (the calm time) and unmutes them. A ship whose
/// system has no record has no contact (D843).
pub fn update_hostile_contacts(cx: &mut Cx<'_>, player: u32, dt: f32) {
    let key = ships::ShipKey::Player(player);
    let Some(ship) = ships::base_mut(&mut cx.logic.content.state, key) else {
        return;
    };
    for c in &mut ship.hostile_contacts {
        c.time_since_seen = (f64::from(c.time_since_seen) + f64::from(dt)) as f32;
    }
    let system = ship.system;
    let position = ships::position(cx, key);
    let objects: Vec<crate::events::NpcShipKey> = system
        .and_then(|s| cx.logic.content.state.systems.get(&s))
        .map(|r| r.space_objs.clone())
        .unwrap_or_default();
    let single = api::misc::single_player(cx);
    for object in objects {
        let at = ships::position(cx, ships::ShipKey::Npc(object));
        let dx = (f64::from(at.x) - f64::from(position.x)) as f32;
        let dy = (f64::from(at.y) - f64::from(position.y)) as f32;
        let dz = (f64::from(at.z) - f64::from(position.z)) as f32;
        let distance_square = f64::from(dy) * f64::from(dy)
            + f64::from(dz) * f64::from(dz)
            + f64::from(dx) * f64::from(dx);
        if !(distance_square <= 6_250_000.0) {
            continue;
        }
        let state = &cx.logic.content.state;
        let ship_rep = ships::reputation(state, key);
        let object_rep = ships::reputation(state, ships::ShipKey::Npc(object));
        let listed = ships::base(state, key)
            .and_then(|s| s.hostile_contacts.iter().position(|c| c.object == object));
        if let Some(at) = listed {
            if single {
                let attitude = api::reputation::attitude(cx, object_rep, ship_rep).unwrap_or(0.0);
                if attitude > -0.6 {
                    continue;
                }
            }
            if let Some(s) = ships::base_mut(&mut cx.logic.content.state, key) {
                let c = &mut s.hostile_contacts[at];
                c.time_near = (f64::from(dt) + f64::from(c.time_near)) as f32;
                c.time_since_seen = 0.0;
            }
            continue;
        }
        let attitude = api::reputation::attitude(cx, object_rep, ship_rep).unwrap_or(0.0);
        if !(attitude <= -0.6) {
            continue;
        }
        if let Some(s) = ships::base_mut(&mut cx.logic.content.state, key) {
            s.hostile_contacts.push(HostileContact {
                object,
                time_near: dt,
                time_since_seen: 0.0,
            });
            s.hostile_contact_count += 1;
        }
    }
    let time = super::game_support::game_time(cx);
    let Some(s) = ships::base_mut(&mut cx.logic.content.state, key) else {
        return;
    };
    s.hostile_contacts.retain(|c| c.time_since_seen == 0.0);
    let mute = if s.hostile_contacts.is_empty() {
        if !s.in_combat {
            return;
        }
        s.in_combat = false;
        s.calm_since = time;
        s.hostile_contact_count = 0;
        false
    } else {
        if s.in_combat {
            return;
        }
        s.in_combat = true;
        s.calm_since = f64::from(-1.0_f32);
        true
    };
    // `SetNpcShipsField1b1(!mute)` of the ship's system.
    let Some(system) = system else {
        return;
    };
    let ships: Vec<crate::events::NpcShipKey> = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .map(|r| r.space_objs.clone())
        .unwrap_or_default();
    for npc in ships {
        super::npc_ships::set_muted(cx, npc, mute);
    }
}
