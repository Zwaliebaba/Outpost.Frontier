//! The conditions' run time (`Content/MissionCnd.cpp`, TU21; work package
//! P3.5c1): `OnActivate` ([`on_activate`]) and `Matches` ([`matches`]) of
//! the 44 `Cnd_*` classes, and the mission object's label lookups they
//! share (`Content/MissionObject.cpp:1984-2165`, `:2792-2830`).
//!
//! A condition is taken out of its trigger for both calls (record 0008
//! point 1): neither can dispatch. They take `&mut Cx` only because some
//! getters do (`get_behavior_id`, `GetAttitude`, `InZones`,
//! `Zone::GetProperties`); the script is looked up again around them.
//! `Slot10`, which `EndMission` calls on each active trigger's condition,
//! is the base's empty body in every class (`MissionCnd.cpp:258-261`), so
//! the engine has nothing to call.
//!
//! The message parameters a condition reads are the dwords of the C++
//! parameter block ([`block`]); the trigger engine passes a heartbeat's
//! frame time, the synthetic `NpcSystemEnter` words of `OnSystemEnter`, or
//! nothing (the activation probe and the heartbeat's `TetherObject` pass).
//!
//! The C++'s comparisons are kept as written, negations included: an
//! x87 `FCOMP` branch is false for a NaN on either side.
#![allow(clippy::neg_cmp_op_on_partial_ord, clippy::double_comparisons)]

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector, transform_matrix};
use neuron_server::controller::ControllerId;

use super::MissionScript;
use super::condition::{
    Condition, DistShip, DistVec, DistVecLbl, NpcSystemEnter, NpcSystemEntry, TetherBroke,
};
use super::trigger::{STATE_ACTIVE, STATE_COMPLETE, STATE_INACTIVE};
use crate::api;
use crate::content::Instance;
use crate::content::players::PlayerState;
use crate::events::{ControllerMessage, NpcShipKey};
use crate::server::Cx;

/// `TEST id,0x3fffffff`: an id is set when its low 30 bits are.
pub const ID_MASK: u32 = 0x3fff_ffff;

/// What the trigger engine hands a condition with its event id.
#[derive(Clone, Copy, Debug)]
pub enum Parms<'a, 'm> {
    /// A null pointer: the activation probe and the heartbeat's second
    /// pass (`TetherObject`).
    Null,
    /// The message's own parameter block.
    Message(&'a ControllerMessage<'m>),
    /// A heartbeat's frame time (the local copy `HandleMissionMessage`
    /// passes).
    Delta(f32),
    /// `OnSystemEnter`'s `NpcSystemEnter` words: the player's ship, the
    /// system and the arrival object (or 0).
    NpcEnter([u32; 3]),
}

/// The parameter block's dwords as the C++ reads them (nine is the longest
/// block a condition reads); `None` for a null pointer.
#[must_use]
pub fn block(parms: &Parms<'_, '_>) -> Option<[u32; 9]> {
    use ControllerMessage as M;
    let mut w = [0u32; 9];
    let mut put = |words: &[u32]| w[..words.len()].copy_from_slice(words);
    match *parms {
        Parms::Null => return None,
        Parms::Delta(dt) => put(&[dt.to_bits()]),
        Parms::NpcEnter(words) => put(&words),
        Parms::Message(m) => match m {
            M::BaseEnter { base, player } | M::BaseExit { base, player } => put(&[*base, *player]),
            M::CargoScanned { scanner, target } => put(&[*scanner, *target]),
            M::PlayerLaunch { player, base } => put(&[*player, *base]),
            M::CommComplete {
                listener,
                system,
                speaker,
                line,
                result,
            } => put(&[*listener, *system, *speaker, *line, result.value()]),
            M::ObjectDamaged(d) => put(&[
                d.id,
                d.system,
                d.inflictor,
                d.equip_type,
                d.collision_group_type,
                d.cause.cast_unsigned(),
                d.damage.to_bits(),
                d.relative_damage.to_bits(),
                d.relative_health.to_bits(),
            ]),
            M::TradelaneEnter {
                ship,
                entrance,
                exit,
                system,
            }
            | M::TradelaneExit {
                ship,
                entrance,
                exit,
                system,
            } => put(&[*ship, *entrance, *exit, *system]),
            M::LootAcquired { loot, ship, .. } => put(&[*loot, *ship]),
            M::GfObjSelect { object, player } => put(&[*object, *player]),
            M::Heartbeat { elapsed } => put(&[elapsed.to_bits()]),
            M::RtcDone { player, rtc } => put(&[*player, *rtc]),
            M::JumpInComplete { arg, ship } => put(&[*arg, *ship]),
            M::LaunchComplete { id, ship } => put(&[*id, *ship]),
            M::LocationEnter { location, player } | M::LocationExit { location, player } => {
                put(&[*location, *player]);
            }
            M::MissionResponse {
                offer,
                kind,
                accepted,
                player,
            } => put(&[*offer, *kind, u32::from(*accepted), *player]),
            M::PlayerManeuver {
                player,
                object,
                value,
            } => put(&[*player, *object, *value]),
            M::PopUpDialog { player, button } => put(&[*player, *button]),
            M::SpaceEnter {
                system,
                player,
                launch_object,
                launch_kind,
            } => put(&[
                *system,
                *player,
                *launch_object,
                launch_kind.cast_unsigned(),
            ]),
            M::SpaceExit { system, player } => put(&[*system, *player]),
            M::ObjectDestroyed(d) => {
                put(&[
                    d.id,
                    d.killer,
                    d.system,
                    d.owner_player,
                    d.kind.cast_unsigned(),
                ]);
            }
            M::SystemExit {
                system,
                destination,
                player,
            } => put(&[*system, *destination, *player]),
            M::SystemEnter {
                system,
                from,
                player,
                arrival,
                ..
            } => put(&[*system, *from, *player, *arrival]),
            M::JumpgateAct { target } => put(&[*target]),
            M::NpcSystemExit { ship, system } => put(&[*ship, *system]),
            M::NpcSystemEnter {
                ship,
                system,
                object,
            } => put(&[*ship, *system, object.unwrap_or(0)]),
            // The tether report's byte +0x0c is always 0 (`NpcShip.cpp:527-533`).
            M::TetherObject { object, other } => put(&[*object, *other, 0, 0]),
            M::RumorHeard { player, .. } => put(&[*player]),
            M::NpcShipCreated { ship } => put(&[ship.0]),
            _ => {}
        },
    }
    Some(w)
}

/// The frame time of a heartbeat's parameters (`*(float const*)parms`).
fn delta(parms: &Parms<'_, '_>) -> Option<f32> {
    block(parms).map(|w| f32::from_bits(w[0]))
}

/// The mission script of controller `id`.
#[must_use]
pub fn script<'c>(cx: &'c Cx<'_>, id: ControllerId) -> Option<&'c MissionScript> {
    match cx.logic.content.instances.get(&id) {
        Some(Instance::Mission(script)) => Some(script),
        _ => None,
    }
}

/// The mission script of controller `id`, to change.
pub fn script_mut<'c>(cx: &'c mut Cx<'_>, id: ControllerId) -> Option<&'c mut MissionScript> {
    match cx.logic.content.instances.get_mut(&id) {
        Some(Instance::Mission(script)) => Some(script),
        _ => None,
    }
}

/// `FindContentPlayerState(Mission->PlayerId)`.
#[must_use]
pub fn player_state<'c>(cx: &'c Cx<'_>, s: &MissionScript) -> Option<&'c PlayerState> {
    cx.logic.content.state.players.get(&s.object.player)
}

/// The player's ship's space object, 0 without a ship (`GetPlayerShipId`).
fn ship_of(state: &PlayerState) -> u32 {
    state.ship.as_ref().map_or(0, |s| s.ship)
}

/// The player's live ship, when the player has a state and a ship.
fn live_ship(cx: &Cx<'_>, s: &MissionScript) -> Option<u32> {
    player_state(cx, s)
        .and_then(|p| p.ship.as_ref())
        .map(|s| s.ship)
}

/// An NPC ship by its key, `None` for a null entry.
fn npc<'c>(cx: &'c Cx<'_>, key: NpcShipKey) -> Option<&'c crate::content::npc_ships::NpcShip> {
    cx.logic.content.state.npc_ships.get(&key)
}

/// `FindNpcByName` (`MissionObject.cpp:2008-2027`): with a live player ship,
/// the first NPC by space object id whose nickname matches (`_stricmp`;
/// a null entry ends the walk), else [`find_formation_npc`].
#[must_use]
pub fn find_npc_by_name(cx: &Cx<'_>, s: &MissionScript, label: &[u8]) -> Option<NpcShipKey> {
    if live_ship(cx, s).is_some() {
        for &key in s.object.npc_objects.values() {
            let Some(n) = npc(cx, key) else { break };
            if eq_ignore_case(&n.nickname, c_str(label)) {
                return Some(key);
            }
        }
    }
    find_formation_npc(cx, s, label)
}

/// `FindFormationNpc` (`MissionObject.cpp:2029-2059`): each slot of each
/// record named `label`, the first NPC with the slot's nickname, returned
/// when it is no formation's follower.
#[must_use]
pub fn find_formation_npc(cx: &Cx<'_>, s: &MissionScript, label: &[u8]) -> Option<NpcShipKey> {
    for group in &s.object.ships {
        if !eq_ignore_case(&group.nickname, c_str(label)) {
            continue;
        }
        for slot in &group.ships {
            let found = s.object.npc_objects.values().copied().find(|&key| {
                npc(cx, key).is_some_and(|n| eq_ignore_case(&n.nickname, &slot.nickname))
            });
            let Some(key) = found else { continue };
            let Some(n) = npc(cx, key) else { continue };
            if n.base.formation_slot == 0 || n.base.formation_id == u32::MAX {
                return Some(key);
            }
        }
    }
    // FindRandEncFormationNpc: the random encounters' arms are filled by
    // AttachRandEncNpc on NpcShipCreated, which comes with the spawns
    // (P3.5c2, D975); no arm has a member before.
    None
}

/// `GetLabelShipId` (`MissionObject.cpp:1984-2003`): 0 without a player
/// ship; `Player` (`_stricmp`) is the player's ship, any other label an NPC's.
#[must_use]
pub fn label_ship_id(cx: &Cx<'_>, s: &MissionScript, label: &[u8]) -> u32 {
    let Some(ship) = live_ship(cx, s) else {
        return 0;
    };
    if eq_ignore_case(c_str(label), b"Player") {
        return ship;
    }
    find_npc_by_name(cx, s, label)
        .and_then(|key| npc(cx, key))
        .map_or(0, |n| n.base.ship)
}

/// `GetLabelObjectId` (`MissionObject.cpp:2096-2111`): a ship, else a
/// loot, else a solar; 0 without a player ship.
#[must_use]
pub fn label_object_id(cx: &Cx<'_>, s: &MissionScript, label: &[u8]) -> u32 {
    if live_ship(cx, s).is_none() {
        return 0;
    }
    let ship = label_ship_id(cx, s, label);
    if ship & ID_MASK != 0 {
        return ship;
    }
    // GetLootId: strcmp of each [MsnLoot] nickname.
    let loot = s
        .object
        .loot
        .iter()
        .find(|l| l.nickname == c_str(label))
        .map_or(0, |l| l.space_obj_id);
    if loot & ID_MASK != 0 {
        return loot;
    }
    solar_id(cx, s, label)
}

/// `GetSolarId` (`MissionObject.cpp:2125-2149`): a spawned `[MsnSolar]`
/// (`strcmp`), else the universe object of that nickname when it exists
/// with a type (`IsSpaceObjTypeSet`, `:69-76`).
fn solar_id(cx: &Cx<'_>, s: &MissionScript, label: &[u8]) -> u32 {
    if live_ship(cx, s).is_none() {
        return 0;
    }
    let solar = s
        .object
        .solars
        .iter()
        .find(|o| o.nickname == c_str(label))
        .map_or(0, |o| o.space_obj_id);
    if solar & ID_MASK != 0 {
        return solar;
    }
    let id = api::misc::nickname_id(c_str(label));
    match api::space_obj::object_type(cx, id) {
        Ok(kind) if kind & 0xfdff != 0 => id,
        _ => 0,
    }
}

/// `HasObjectLabel` (`MissionObject.cpp:2792-2833`): the NPC, solar or loot
/// with space object `object` carries `label`.
#[must_use]
pub fn has_object_label(cx: &Cx<'_>, s: &MissionScript, object: u32, label: u32) -> bool {
    if let Some(n) = s.object.npc_objects.get(&object).and_then(|&k| npc(cx, k))
        && n.field_0x17c.contains(&label)
    {
        return true;
    }
    if let Some(solar) = s.object.solars.iter().find(|o| o.space_obj_id == object)
        && solar.label_ids.contains(&label)
    {
        return true;
    }
    s.object
        .loot
        .iter()
        .find(|l| l.space_obj_id == object)
        .is_some_and(|l| l.label_ids.contains(&label))
}

/// The location of an object, or `None` when `GetLocation` fails.
fn location(cx: &Cx<'_>, object: u32) -> Option<(Vector, Matrix)> {
    api::space_obj::location(cx, object).ok()
}

/// `x87` difference of two floats, kept in `f64` (T11).
fn diff(a: f32, b: f32) -> f64 {
    f64::from(a) - f64::from(b)
}

/// `Cnd::OnActivate` of each class.
pub fn on_activate(cx: &mut Cx<'_>, id: ControllerId, condition: &mut Condition) {
    let Some(s) = script(cx, id) else { return };
    let cx_ref: &Cx<'_> = cx;
    match condition {
        Condition::CargoScanned(c) => {
            if c_str(&c.scanner_label) != b"none" {
                c.field_0x74 = label_ship_id(cx_ref, s, &c.scanner_label);
            }
            if c_str(&c.scanned_label) != b"none" {
                c.field_0x78 = label_ship_id(cx_ref, s, &c.scanned_label);
            }
        }
        Condition::CommComplete(c) => {
            if !c.source_label.is_empty() {
                c.source_obj_id = label_ship_id(cx_ref, s, &c.source_label);
            }
        }
        Condition::CmpToPlane(c) => c.object_id = label_ship_id(cx_ref, s, &c.object_label),
        Condition::Destroyed(c) => {
            c.count = 0;
            if !c.killer_label.is_empty() {
                c.killer_obj_id = label_ship_id(cx_ref, s, &c.killer_label);
            }
        }
        Condition::DistCircle(c) => {
            c.circle_obj_id = api::misc::nickname_id(c_str(&c.circle_obj));
            // GetLocation zeroes the position first; the orientation of a
            // missing object is the stack's (D978): identity here.
            let (center, orientation) =
                location(cx_ref, c.circle_obj_id).unwrap_or((Vector::ZERO, Matrix::IDENTITY));
            c.circle_center = center;
            let axis = transform_matrix(&orientation, Vector::new(0.0, 0.0, -1.0));
            let (nx, ny, nz) = (f64::from(axis.x), f64::from(axis.y), f64::from(axis.z));
            let inv = 1.0 / (nx * nx + ny * ny + nz * nz).sqrt();
            c.normal = Vector::new((inv * nx) as f32, (inv * ny) as f32, (inv * nz) as f32);
            let n = c.normal;
            c.plane_offset = (-(f64::from(c.circle_center.z) * f64::from(n.z)
                + f64::from(c.circle_center.y) * f64::from(n.y)
                + f64::from(c.circle_center.x) * f64::from(n.x)))
                as f32;
            // GetRadius zeroes both outputs first.
            let (radius, center) =
                api::space_obj::radius(cx_ref, c.circle_obj_id).unwrap_or((0.0, Vector::ZERO));
            c.radius = radius;
            c.circle_center = center;
            let object = label_ship_id(cx_ref, s, &c.object_label);
            if object & ID_MASK != 0 {
                c.last_position = location(cx_ref, object).map_or(Vector::ZERO, |l| l.0);
            }
        }
        Condition::DistShip(c) => {
            if !c.object_a.is_empty() {
                c.object_a_id = label_object_id(cx_ref, s, &c.object_a);
            }
            if !c.object_b.is_empty() {
                c.object_b_id = label_object_id(cx_ref, s, &c.object_b);
            }
            c.tick_timer = c.tick_interval;
            c.last_distance_sq = 0.0;
        }
        Condition::DistVec(c) => {
            if !c.object_label.is_empty() {
                c.object_id = label_ship_id(cx_ref, s, &c.object_label);
            }
            c.tick_timer = c.tick_interval;
            c.last_distance_sq = 0.0;
        }
        Condition::DistVecLbl(c) => c.tick_timer = c.tick_interval,
        Condition::HealthDec(c) => {
            if !c.object_label.is_empty() {
                c.object_id = label_object_id(cx_ref, s, &c.object_label);
            }
            if !c.attacker_label.is_empty() {
                c.attacker_id = label_object_id(cx_ref, s, &c.attacker_label);
            }
        }
        Condition::InZone(c) => c.object_id = label_object_id(cx_ref, s, &c.object_label),
        Condition::LaunchComplete(c) => c.ship_id = label_ship_id(cx_ref, s, &c.ship_label),
        Condition::LootAcquired(c) => c.count = 0,
        Condition::NpcSystemEnter(c) => c.pending.clone_from(&c.ships),
        Condition::NpcSystemExit(c) => c.pending.clone_from(&c.ships),
        Condition::PlayerManeuver(c) => {
            if !c.target_label.is_empty() {
                c.target_id = label_ship_id(cx_ref, s, &c.target_label);
            }
        }
        Condition::ProjHit(c) => {
            c.target_id = label_object_id(cx_ref, s, &c.target_label);
            if !c.shooter_label.is_empty() {
                c.shooter_id = label_object_id(cx_ref, s, &c.shooter_label);
            }
            c.count = 0;
        }
        Condition::ProjHitShipToLbl(c) => {
            if !c.shooter_label.is_empty() {
                c.shooter_id = label_object_id(cx_ref, s, &c.shooter_label);
            }
            c.count = 0;
        }
        Condition::TetherBroke(c) => {
            c.field_0x90 = 0.0;
            c.field_0x88 = 0.0;
            c.field_0x94 = 0.0;
            c.ship_a_id = label_ship_id(cx_ref, s, &c.ship_a);
            c.ship_b_id = label_ship_id(cx_ref, s, &c.ship_b);
        }
        Condition::Timer(c) => c.elapsed = 0.0,
        Condition::TlEntered(c) => c.ship_id = label_ship_id(cx_ref, s, &c.ship_label),
        Condition::TlExited(c) => c.ship_id = label_ship_id(cx_ref, s, &c.ship_label),
        Condition::WatchVibe(c) => {
            c.object_a_id = label_object_id(cx_ref, s, &c.object_a);
            c.object_b_id = label_object_id(cx_ref, s, &c.object_b);
        }
        Condition::BaseEnter(_)
        | Condition::BaseExit(_)
        | Condition::CharSelect(_)
        | Condition::EncLaunched(_)
        | Condition::HasMsn(_)
        | Condition::InSpace(_)
        | Condition::InTradelane(_)
        | Condition::JumpgateAct(_)
        | Condition::JumpInComplete(_)
        | Condition::LocEnter(_)
        | Condition::LocExit(_)
        | Condition::MsnResponse(_)
        | Condition::PlayerLaunch(_)
        | Condition::PopUpDialog(_)
        | Condition::RtcDone(_)
        | Condition::RumorHeard(_)
        | Condition::SpaceEnter(_)
        | Condition::SpaceExit(_)
        | Condition::SystemEnter(_)
        | Condition::SystemExit(_)
        | Condition::True(_)
        | Condition::WatchTrigger(_) => {}
    }
}

/// `MatchesIdOrAny`: a null block fails, an unset id matches anything, else
/// the block's first word must be it.
fn id_or_any(id: u32, parms: &Parms<'_, '_>) -> bool {
    let Some(w) = block(parms) else { return false };
    id & ID_MASK == 0 || w[0] == id
}

/// `TickDistanceCondition` (the shared tail of `Cnd_DistShip` and
/// `Cnd_DistVec`).
#[allow(clippy::too_many_arguments)]
fn tick_distance(
    distance_sq: f32,
    limit_sq: f32,
    inside: bool,
    tick_timer: &mut f32,
    tick_interval: f32,
    tick_mode: i32,
    last_distance_sq: &mut f32,
    dt: f32,
) -> bool {
    let in_range = if inside {
        distance_sq < limit_sq
    } else {
        distance_sq > limit_sq
    };
    if !in_range {
        *tick_timer = tick_interval;
        return false;
    }
    let step = diff(distance_sq, *last_distance_sq);
    *last_distance_sq = distance_sq;
    let tick = tick_mode == 0 || (step <= 0.0 && tick_mode == 1) || (step >= 0.0 && tick_mode == 2);
    if tick {
        *tick_timer -= dt;
    }
    *tick_timer <= 0.0
}

/// `Cnd::Matches(EventId, EventParms)` of each class.
#[allow(clippy::too_many_lines)]
pub fn matches(
    cx: &mut Cx<'_>,
    id: ControllerId,
    condition: &mut Condition,
    event: u32,
    parms: &Parms<'_, '_>,
) -> bool {
    let Some(s) = script(cx, id) else {
        return false;
    };
    // The getters that need the context mutably come last in their
    // conditions; the rest read through this shared borrow.
    let cx_ref: &Cx<'_> = cx;
    let w = block(parms);
    match condition {
        Condition::BaseEnter(c) => id_or_any(c.base_id, parms),
        Condition::BaseExit(c) => id_or_any(c.base_id, parms),
        Condition::SpaceEnter(c) => id_or_any(c.system_id, parms),
        Condition::SpaceExit(c) => id_or_any(c.system_id, parms),
        Condition::CargoScanned(c) => {
            let Some(w) = w else { return false };
            let mut scanner_missing = false;
            if c_str(&c.scanner_label) != b"none" && c.field_0x74 & ID_MASK == 0 {
                c.field_0x74 = label_ship_id(cx_ref, s, &c.scanner_label);
                scanner_missing = c.field_0x74 & ID_MASK == 0;
            }
            if c_str(&c.scanned_label) != b"none" && c.field_0x78 & ID_MASK == 0 {
                c.field_0x78 = label_ship_id(cx_ref, s, &c.scanned_label);
                if c.field_0x78 & ID_MASK == 0 {
                    return false;
                }
            }
            if scanner_missing {
                return false;
            }
            let scanner = c.field_0x74 == w[0];
            let scanned = c.field_0x78 == w[1];
            if c.field_0x74 & ID_MASK != 0 && c.field_0x78 & ID_MASK != 0 {
                return scanner && scanned;
            }
            c.field_0x74 & ID_MASK != 0 && scanner
        }
        Condition::CharSelect(c) => {
            let Some(_) = w else { return false };
            let Some(p) = player_state(cx_ref, s) else {
                return false;
            };
            // `GetBaseRecord()->BaseId` without a null test (D976).
            if p.base != Some(c.base_id) || c.location_id != s.current_location {
                return false;
            }
            // The base's room states and bar characters are BaseSupport's
            // (P3.6e, D977): no character is found before.
            false
        }
        Condition::CommComplete(c) => {
            let Some(w) = w else { return false };
            if c.source_label.is_empty() {
                return w[3] == c.comm_id;
            }
            c.source_obj_id = label_ship_id(cx_ref, s, &c.source_label);
            c.source_obj_id & ID_MASK != 0 && w[3] == c.comm_id && w[2] == c.source_obj_id
        }
        Condition::CmpToPlane(c) => {
            let Some(p) = player_state(cx_ref, s) else {
                return false;
            };
            if p.ship.is_none() || p.system.is_none() {
                return false;
            }
            if c.object_id & ID_MASK == 0 {
                c.object_id = label_ship_id(cx_ref, s, &c.object_label);
            }
            if c.object_id & ID_MASK == 0 {
                return false;
            }
            let Some((pos, _)) = location(cx_ref, c.object_id) else {
                return false;
            };
            let value = (f64::from(pos.z) * f64::from(c.plane_c)
                + f64::from(pos.y) * f64::from(c.plane_b)
                + f64::from(pos.x) * f64::from(c.plane_a)
                + f64::from(c.plane_d)) as f32;
            if 0.0 > value && c.side == 0 {
                return true;
            }
            0.0 < value && c.side == 1
        }
        Condition::Destroyed(c) => {
            let Some(w) = w else { return false };
            let Some(p) = player_state(cx_ref, s) else {
                return false;
            };
            if w[4].cast_signed() != c.mode
                && c.mode != 0x3e7
                && (p.ship.is_none() || c.mode != 0x3e6)
            {
                return false;
            }
            if !has_object_label(cx_ref, s, w[0], c.label) {
                return false;
            }
            if !c.killer_label.is_empty() && c.killer_obj_id & ID_MASK == 0 {
                c.killer_obj_id = label_ship_id(cx_ref, s, &c.killer_label);
            }
            if c.killer_obj_id & ID_MASK != 0 && c.killer_obj_id != w[1] {
                return false;
            }
            if c.needed == -1 {
                return s.object.label_count(c.label) <= 1;
            }
            c.count += 1;
            c.count == c.needed
        }
        Condition::DistCircle(c) => {
            let object = label_ship_id(cx_ref, s, &c.object_label);
            let Some(p) = player_state(cx_ref, s) else {
                return false;
            };
            if p.ship.is_none() || p.system.is_none() || object & ID_MASK == 0 {
                return false;
            }
            let mut result = false;
            let found = location(cx_ref, object);
            let pos = found.map_or(Vector::ZERO, |l| l.0);
            if found.is_some() {
                let n = c.normal;
                let (dx, dy, dz) = (
                    diff(pos.x, c.last_position.x),
                    diff(pos.y, c.last_position.y),
                    diff(pos.z, c.last_position.z),
                );
                let denom = dy * f64::from(n.y) + dz * f64::from(n.z) + dx * f64::from(n.x);
                if denom != 0.0 {
                    let param = (f64::from(pos.y) * f64::from(n.y)
                        + f64::from(pos.z) * f64::from(n.z)
                        + f64::from(pos.x) * f64::from(n.x)
                        + f64::from(c.plane_offset))
                        / denom;
                    let dz_f = dz as f32;
                    let length = (dz * f64::from(dz_f) + dx * dx + dy * dy).sqrt();
                    let inv = (1.0 / length) as f32;
                    let dir_x = (dx * f64::from(inv)) as f32;
                    let dir_y = (dy * f64::from(inv)) as f32;
                    let dir_z = (f64::from(dz_f) * f64::from(inv)) as f32;
                    if 0.0 < param && param < 1.0 {
                        let along = f64::from(dir_z) * f64::from(n.z)
                            + f64::from(dir_y) * f64::from(n.y)
                            + f64::from(dir_x) * f64::from(n.x);
                        if along > 0.0 {
                            let cxd = diff(c.circle_center.x, pos.x);
                            let cyd = diff(c.circle_center.y, pos.y);
                            let czd = diff(c.circle_center.z, pos.z);
                            if (czd * czd + cxd * cxd + cyd * cyd).sqrt() < f64::from(c.radius) {
                                result = true;
                            }
                        }
                    }
                }
            }
            c.last_position = pos;
            result
        }
        Condition::DistShip(c) => dist_ship(cx_ref, s, c, parms),
        Condition::DistVec(c) => dist_vec(cx_ref, s, c, parms),
        Condition::DistVecLbl(c) => dist_vec_lbl(cx_ref, s, c, parms),
        Condition::EncLaunched(c) => match parms {
            Parms::Message(ControllerMessage::EncounterLaunched { encounters }) => {
                encounters.contains(&c.encounter_id)
            }
            _ => false,
        },
        Condition::HasMsn(c) => {
            let Some(p) = player_state(cx_ref, s) else {
                return false;
            };
            if p.live_mission & ID_MASK != 0 {
                c.wanted == 0
            } else {
                c.wanted == 1
            }
        }
        Condition::HealthDec(c) => {
            let Some(w) = w else {
                if c.object_label.is_empty() || !c.attacker_label.is_empty() {
                    return false;
                }
                if live_ship(cx_ref, s).is_none() {
                    return false;
                }
                c.object_id = label_object_id(cx_ref, s, &c.object_label);
                if c.object_id & ID_MASK == 0 {
                    return false;
                }
                let relative = api::space_obj::relative_health(cx_ref, c.object_id).unwrap_or(0.0);
                return relative < c.health || relative == c.health;
            };
            let mut object_missing = false;
            if c.object_id & ID_MASK == 0 && !c.object_label.is_empty() {
                c.object_id = label_object_id(cx_ref, s, &c.object_label);
                object_missing = c.object_id & ID_MASK == 0;
            }
            if c.attacker_id & ID_MASK == 0 && !c.attacker_label.is_empty() {
                c.attacker_id = label_object_id(cx_ref, s, &c.attacker_label);
                if c.attacker_id & ID_MASK == 0 {
                    return false;
                }
            }
            if object_missing || c.object_id != w[0] || w[3] != 0 || w[4] != 0 {
                return false;
            }
            let health = f32::from_bits(w[8]);
            if !(health < c.health || health == c.health) {
                return false;
            }
            c.attacker_label.is_empty() || c.attacker_id == w[2]
        }
        Condition::InSpace(c) => {
            let Some(p) = player_state(cx_ref, s) else {
                return false;
            };
            if c.in_space {
                p.ship.is_some()
            } else {
                p.ship.is_none()
            }
        }
        Condition::InTradelane(c) => {
            let Some(p) = player_state(cx_ref, s) else {
                return false;
            };
            let Some(ship) = p.ship.as_ref() else {
                return false;
            };
            let lane = if eq_ignore_case(b"Player", c_str(&c.object_label)) {
                Some(ship.lane_entry)
            } else {
                find_npc_by_name(cx_ref, s, &c.object_label)
                    .and_then(|k| npc(cx_ref, k))
                    .map(|n| n.base.lane_entry)
            };
            let Some(lane) = lane else { return false };
            if lane != 0 && c.in_lane {
                return true;
            }
            lane == 0 && !c.in_lane
        }
        Condition::InZone(c) => {
            if live_ship(cx_ref, s).is_none() {
                return false;
            }
            c.object_id = label_ship_id(cx_ref, s, &c.object_label);
            if c.object_id & ID_MASK == 0 {
                return false;
            }
            let Some(system) = player_state(cx_ref, s).and_then(|p| p.system) else {
                return false;
            };
            let Some((pos, _)) = location(cx_ref, c.object_id) else {
                return false;
            };
            let (flags, in_zone) = (c.zone_flags, c.in_zone);
            let zones = api::system::in_zones(cx, system, pos, 0.0, 8).unwrap_or_default();
            let mut matched = false;
            for zone in zones {
                let properties = api::zone::properties(cx, zone).unwrap_or(0);
                if flags & properties != 0 {
                    matched = true;
                }
            }
            if in_zone {
                return matched;
            }
            !matched || flags == 0
        }
        Condition::JumpgateAct(c) => w.is_some_and(|w| w[0] == c.gate_id),
        Condition::JumpInComplete(c) => {
            let Some(w) = w else { return false };
            if w[0] != c.system_id && c.system_id & ID_MASK != 0 {
                return false;
            }
            player_state(cx_ref, s).is_some_and(|p| w[1] == ship_of(p))
        }
        Condition::LaunchComplete(c) => {
            let Some(w) = w else { return false };
            if c.ship_id & ID_MASK == 0 {
                c.ship_id = label_ship_id(cx_ref, s, &c.ship_label);
            }
            if c.ship_id & ID_MASK == 0 || w[1] != c.ship_id {
                return false;
            }
            c.base_id & ID_MASK == 0 || w[0] == c.base_id
        }
        Condition::LocEnter(c) => player_location(cx_ref, s, c.location_id, c.base_id, w),
        Condition::LocExit(c) => player_location(cx_ref, s, c.location_id, c.base_id, w),
        Condition::LootAcquired(c) => {
            let Some(w) = w else { return false };
            let ship = label_ship_id(cx_ref, s, &c.ship_label);
            if has_object_label(cx_ref, s, w[0], c.loot_label) && ship == w[1] {
                c.count += 1;
            }
            if ship == w[1]
                && c.needed == -1
                && usize::try_from(c.count).ok() == Some(s.object.label_count(c.loot_label))
            {
                return true;
            }
            c.needed == c.count
        }
        Condition::MsnResponse(c) => {
            let Some(w) = w else { return false };
            if w[2] & 0xff != 0 {
                c.response == 0
            } else {
                c.response == 1
            }
        }
        Condition::NpcSystemEnter(c) => npc_system_enter(cx_ref, s, c, event, parms),
        Condition::NpcSystemExit(c) => {
            let Some(w) = w else { return false };
            let mut result = true;
            let mut i = 0;
            while i < c.pending.len() {
                let ship = label_ship_id(cx_ref, s, &c.pending[i].label);
                if ship & ID_MASK == 0 {
                    if !c.awaited.is_empty() {
                        result = false;
                        i += 1;
                        continue;
                    }
                    c.pending.remove(i);
                    continue;
                }
                if let Some(at) = c
                    .awaited
                    .iter()
                    .position(|a| c_str(&a.label) == c_str(&c.pending[i].label))
                {
                    c.awaited.remove(at);
                }
                let left = w[1];
                if ship == w[0] {
                    if left & ID_MASK != 0 && (left == c.system_id || c.any_system) {
                        c.pending[i].system_id = left;
                    }
                } else if c.pending[i].system_id != left {
                    result = false;
                }
                i += 1;
            }
            result
        }
        Condition::PlayerLaunch(_) => w.is_some_and(|w| w[0] == s.object.player),
        Condition::PlayerManeuver(c) => {
            let (target_id, wanted, inverse) = (c.target_id, c.behavior_id, c.inverse);
            let mut result = false;
            if let Some(p) = player_state(cx_ref, s) {
                let ship = ship_of(p);
                let player_formation = p.ship.as_ref().map(|s| s.formation_id);
                let target = api::space_obj::target(cx_ref, ship).unwrap_or(0);
                let npc_formation = s
                    .object
                    .npc_objects
                    .get(&target)
                    .and_then(|&k| npc(cx_ref, k))
                    .map(|n| n.base.formation_id);
                let behavior = api::ai::behavior_id(cx, ship);
                if behavior == wanted {
                    if behavior == 0xa {
                        if let (Some(a), Some(b)) = (npc_formation, player_formation) {
                            result = a == b;
                        }
                    } else if target_id == 0 || target == target_id {
                        result = true;
                    }
                }
            }
            if inverse { !result } else { result }
        }
        Condition::PopUpDialog(c) => w.is_some_and(|w| w[1] & c.button_mask != 0),
        Condition::ProjHit(c) => {
            let Some(w) = w else { return false };
            if c.target_id & ID_MASK == 0 {
                c.target_id = label_object_id(cx_ref, s, &c.target_label);
            }
            if !c.shooter_label.is_empty() && c.shooter_id & ID_MASK == 0 {
                c.shooter_id = label_object_id(cx_ref, s, &c.shooter_label);
            }
            if c.target_id & ID_MASK == 0 || w[0] != c.target_id {
                return false;
            }
            if c.shooter_id & ID_MASK != 0 && w[2] != c.shooter_id {
                return false;
            }
            c.count += 1;
            c.count >= c.needed
        }
        Condition::ProjHitShipToLbl(c) => {
            let Some(w) = w else { return false };
            if !c.shooter_label.is_empty() {
                c.shooter_id = label_object_id(cx_ref, s, &c.shooter_label);
            }
            if !has_object_label(cx_ref, s, w[0], c.target_label) {
                return false;
            }
            if !c.shooter_label.is_empty() && c.shooter_id & ID_MASK == 0 {
                c.shooter_id = label_object_id(cx_ref, s, &c.shooter_label);
            }
            if c.shooter_id & ID_MASK != 0 && w[2] != c.shooter_id {
                return false;
            }
            c.count += 1;
            c.count >= c.needed
        }
        // A started cut scene's sequence id comes with the bar scenes
        // (P3.5c4, P3.6e; D977): no script of the list is started before.
        Condition::RtcDone(_) => false,
        Condition::RumorHeard(c) => {
            let Parms::Message(ControllerMessage::RumorHeard { player, role }) = parms else {
                return false;
            };
            *player == s.object.player
                && c.location_id == s.current_location
                && eq_ignore_case(c_str(&c.rumor), c_str(role))
        }
        Condition::SystemEnter(c) => {
            let Some(w) = w else {
                // The cached player state, not looked up (D976).
                return player_state(cx_ref, s).and_then(|p| p.system).unwrap_or(0) == c.system_id;
            };
            if w[0] != c.system_id && c.system_id & ID_MASK != 0 {
                return false;
            }
            !(w[1] != c.from_system_id && c.from_system_id & ID_MASK != 0)
        }
        Condition::SystemExit(c) => {
            let Some(w) = w else { return false };
            if w[0] != c.system_id && c.system_id & ID_MASK != 0 {
                return false;
            }
            c.to_system_id & ID_MASK == 0 || w[1] == c.to_system_id
        }
        Condition::TetherBroke(c) => tether_broke(cx_ref, s, c, w),
        Condition::Timer(c) => {
            let Some(p) = player_state(cx_ref, s) else {
                return false;
            };
            if p.ship.is_none() && !c.run_on_base {
                return false;
            }
            if let Some(dt) = delta(parms) {
                c.elapsed += dt;
            }
            !(c.elapsed < c.duration)
        }
        Condition::TlEntered(c) => {
            let Some(w) = w else { return false };
            c.ship_id = label_ship_id(cx_ref, s, &c.ship_label);
            c.ship_id & ID_MASK != 0
                && w[0] == c.ship_id
                && w[1] == c.ring_a_id
                && w[2] == c.ring_b_id
        }
        Condition::TlExited(c) => {
            let Some(w) = w else { return false };
            c.ship_id = label_ship_id(cx_ref, s, &c.ship_label);
            if c.ship_id & ID_MASK == 0 || w[0] != c.ship_id {
                return false;
            }
            if c.ring_a_id & ID_MASK != 0 && w[2] != c.ring_a_id {
                return false;
            }
            c.ring_b_id & ID_MASK == 0 || w[1] == c.ring_b_id
        }
        Condition::True(_) => true,
        Condition::WatchTrigger(c) => {
            let Some(trigger) = s.triggers.get(&c.watched_trigger_id) else {
                return false;
            };
            watch_trigger(c.wanted_state, trigger.state)
        }
        Condition::WatchVibe(c) => {
            if c.object_a_id & ID_MASK == 0 {
                c.object_a_id = label_object_id(cx_ref, s, &c.object_a);
            }
            if c.object_b_id & ID_MASK == 0 {
                c.object_b_id = label_object_id(cx_ref, s, &c.object_b);
            }
            if c.object_a_id & ID_MASK == 0 || c.object_b_id & ID_MASK == 0 {
                return false;
            }
            let rep_a = api::space_obj::rep(cx_ref, c.object_a_id).unwrap_or(0);
            let rep_b = api::space_obj::rep(cx_ref, c.object_b_id).unwrap_or(0);
            let (compare, value) = (c.compare, c.value);
            let attitude = api::reputation::attitude(cx, rep_a, rep_b).unwrap_or(0.0);
            match compare {
                0 => attitude < value || attitude == value,
                1 => attitude >= value,
                2 => attitude == value,
                _ => false,
            }
        }
    }
}

/// `Cnd_WatchTrigger`'s test of a trigger's state against the wanted
/// state's id.
fn watch_trigger(wanted: u32, state: i32) -> bool {
    if wanted == create_id(b"ACTIVE") && state == STATE_ACTIVE {
        return true;
    }
    if wanted == create_id(b"INACTIVE") && state == STATE_INACTIVE {
        return true;
    }
    if wanted == create_id(b"COMPLETE") && state == STATE_COMPLETE {
        return true;
    }
    if wanted == create_id(b"ON") && state == STATE_ACTIVE {
        return true;
    }
    wanted == create_id(b"OFF") && (state == STATE_INACTIVE || state == STATE_COMPLETE)
}

/// `MatchesPlayerLocation` of `Cnd_LocEnter` and `Cnd_LocExit`.
fn player_location(
    cx: &Cx<'_>,
    s: &MissionScript,
    location: u32,
    base: u32,
    w: Option<[u32; 9]>,
) -> bool {
    let Some(w) = w else { return false };
    let Some(p) = player_state(cx, s) else {
        return false;
    };
    p.base == Some(base) && w[0] == location
}

/// `Cnd_DistShip::Matches`.
fn dist_ship(cx: &Cx<'_>, s: &MissionScript, c: &mut DistShip, parms: &Parms<'_, '_>) -> bool {
    let Some(dt) = delta(parms) else { return false };
    let Some(p) = player_state(cx, s) else {
        return false;
    };
    if p.ship.is_none() || p.system.is_none() {
        return false;
    }
    c.object_a_id = label_object_id(cx, s, &c.object_a);
    if c.object_a_id & ID_MASK == 0 {
        return false;
    }
    let Some((a, _)) = location(cx, c.object_a_id) else {
        return false;
    };
    c.object_b_id = label_object_id(cx, s, &c.object_b);
    if c.object_b_id & ID_MASK == 0 {
        return false;
    }
    let Some((b, _)) = location(cx, c.object_b_id) else {
        return false;
    };
    let system_a = api::space_obj::system(cx, c.object_a_id).unwrap_or(0);
    let system_b = api::space_obj::system(cx, c.object_b_id).unwrap_or(0);
    if system_a != system_b {
        return false;
    }
    let (dx, dy, dz) = (diff(a.x, b.x), diff(a.y, b.y), diff(a.z, b.z));
    let current = (dz * dz + dy * dy + dx * dx) as f32;
    tick_distance(
        current,
        c.distance_sq,
        c.inside,
        &mut c.tick_timer,
        c.tick_interval,
        c.tick_mode,
        &mut c.last_distance_sq,
        dt,
    )
}

/// `Cnd_DistVec::Matches`.
fn dist_vec(cx: &Cx<'_>, s: &MissionScript, c: &mut DistVec, parms: &Parms<'_, '_>) -> bool {
    let Some(dt) = delta(parms) else { return false };
    let Some(p) = player_state(cx, s) else {
        return false;
    };
    if p.ship.is_none() || p.system.is_none() {
        return false;
    }
    c.object_id = label_object_id(cx, s, &c.object_label);
    if c.object_id & ID_MASK == 0 {
        return false;
    }
    let Some((pos, _)) = location(cx, c.object_id) else {
        return false;
    };
    let (dx, dy, dz) = (
        diff(pos.x, c.position.x),
        diff(pos.y, c.position.y),
        diff(pos.z, c.position.z),
    );
    let current = (dz * dz + dx * dx + dy * dy) as f32;
    tick_distance(
        current,
        c.distance_sq,
        c.inside,
        &mut c.tick_timer,
        c.tick_interval,
        c.tick_mode,
        &mut c.last_distance_sq,
        dt,
    )
}

/// `Cnd_DistVecLbl::Matches`, `MatchesAny` and `MatchesAll`.
fn dist_vec_lbl(cx: &Cx<'_>, s: &MissionScript, c: &mut DistVecLbl, parms: &Parms<'_, '_>) -> bool {
    let Some(dt) = delta(parms) else { return false };
    let Some(p) = player_state(cx, s) else {
        return false;
    };
    if p.ship.is_none() || p.system.is_none() {
        return false;
    }
    let members = s
        .object
        .formation_members
        .get(&c.label)
        .map_or(&[][..], Vec::as_slice);
    let limit = f64::from(c.distance_sq);
    let in_range = |d: f64| if c.inside { d < limit } else { d > limit };
    if c.any {
        for &member in members {
            let Some((pos, _)) = location(cx, member) else {
                continue;
            };
            let (dx, dy, dz) = (
                diff(pos.x, c.position.x),
                diff(pos.y, c.position.y),
                diff(pos.z, c.position.z),
            );
            if in_range(dz * dz + dx * dx + dy * dy) {
                c.tick_timer -= dt;
                if c.tick_timer <= 0.0 {
                    return true;
                }
            } else {
                c.tick_timer = c.tick_interval;
            }
        }
        return false;
    }
    let mut inside = 0;
    for &member in members {
        let Some((pos, _)) = location(cx, member) else {
            continue;
        };
        let (dx, dy, dz) = (
            diff(pos.x, c.position.x),
            diff(pos.y, c.position.y),
            diff(pos.z, c.position.z),
        );
        if in_range(dz * dz + dy * dy + dx * dx) {
            inside += 1;
        }
    }
    if inside != members.len() {
        c.tick_timer = c.tick_interval;
        return false;
    }
    c.tick_timer -= dt;
    c.tick_timer <= 0.0
}

/// `Cnd_NPCSystemEnter::Matches` (`MissionCnd.cpp:1740-1846`).
fn npc_system_enter(
    cx: &Cx<'_>,
    s: &MissionScript,
    c: &mut NpcSystemEnter,
    event: u32,
    parms: &Parms<'_, '_>,
) -> bool {
    use super::condition::event::{HEARTBEAT, NPC_SHIP_CREATED, NPC_SYSTEM_ENTER};
    let start_delay = |c: &mut NpcSystemEnter, delay: f32| {
        c.delay_running = true;
        c.delay_start = s.elapsed_time;
        c.delay_elapsed = 0.0;
        c.delay = delay;
    };
    if event == NPC_SYSTEM_ENTER {
        // The words are not tested for null; the engine always passes them.
        let w = block(parms).unwrap_or([0; 9]);
        let mut result = true;
        let mut i = 0;
        while i < c.pending.len() {
            let ship = label_ship_id(cx, s, &c.pending[i].label);
            if ship & ID_MASK == 0 {
                if !c.awaited.is_empty() {
                    result = false;
                    i += 1;
                    continue;
                }
                c.pending.remove(i);
                continue;
            }
            let entered = w[1];
            if ship == w[0] {
                if entered & ID_MASK != 0 && (entered == c.system_id || c.any_system) {
                    c.pending[i].system_id = entered;
                } else {
                    result = false;
                }
            } else if c.pending[i].system_id != entered {
                result = false;
            }
            i += 1;
        }
        if !result {
            return false;
        }
        let Some(p) = player_state(cx, s) else {
            return true;
        };
        let Some(ship) = p.ship.as_ref() else {
            return true;
        };
        if ship.ship != w[0] || w[2] & ID_MASK == 0 {
            return true;
        }
        let kind = api::space_obj::object_type(cx, w[2]).unwrap_or(0);
        if kind & 0x800 == 0 && kind & 0x40 == 0 {
            return true;
        }
        start_delay(c, 10.0);
        return false;
    }
    if event == NPC_SHIP_CREATED {
        if c.awaited.is_empty() {
            return false;
        }
        // The cached player state, not looked up (D976).
        let system = player_state(cx, s).and_then(|p| p.system).unwrap_or(0);
        if system != c.system_id && !c.any_system {
            return false;
        }
        let Parms::Message(ControllerMessage::NpcShipCreated { ship }) = parms else {
            return false;
        };
        let nickname = npc(cx, *ship).map(|n| c_str(&n.nickname).to_vec());
        if let Some(nickname) = nickname
            && let Some(at) = c
                .awaited
                .iter()
                .position(|a: &NpcSystemEntry| c_str(&a.label) == nickname.as_slice())
        {
            c.awaited.remove(at);
        }
        if c.awaited.is_empty() {
            start_delay(c, 2.0);
            return false;
        }
        if c.awaited.len() == 1 && c_str(&c.awaited[0].label) == b"Player" {
            c.awaited.clear();
            start_delay(c, 2.0);
        }
        return false;
    }
    if event == HEARTBEAT && c.delay_running {
        let elapsed = diff(s.elapsed_time, c.delay_start);
        c.delay_elapsed = elapsed as f32;
        return !(elapsed < f64::from(c.delay));
    }
    false
}

/// `Cnd_TetherBroke::Matches` (`MissionCnd.cpp:2521-2571`) with
/// `GetObjectDistanceSq`.
fn tether_broke(cx: &Cx<'_>, s: &MissionScript, c: &mut TetherBroke, w: Option<[u32; 9]>) -> bool {
    let Some(p) = player_state(cx, s) else {
        return false;
    };
    if p.ship.is_none() || p.system.is_none() {
        return false;
    }
    c.ship_a_id = label_ship_id(cx, s, &c.ship_a);
    c.ship_b_id = label_ship_id(cx, s, &c.ship_b);
    if c.ship_a_id & ID_MASK == 0 || c.ship_b_id & ID_MASK == 0 {
        return false;
    }
    let distance = match (location(cx, c.ship_a_id), location(cx, c.ship_b_id)) {
        (Some((a, _)), Some((b, _))) => {
            let (dx, dy, dz) = (diff(a.x, b.x), diff(a.y, b.y), diff(a.z, b.z));
            (dz * dz + dy * dy + dx * dx) as f32
        }
        _ => -1.0,
    };
    if let Some(w) = w
        && w[0] == c.ship_a_id
        && w[1] == c.ship_b_id
    {
        c.field_0x7c = w[3] & 0xff == 0;
    }
    if !c.field_0x7c {
        c.field_0x90 = 0.0;
        return false;
    }
    if !(c.distance_sq <= distance) {
        return false;
    }
    let now = f64::from(s.elapsed_time);
    let mut time_step = 0.0f32;
    if c.field_0x94 != 0.0 {
        time_step = (now - f64::from(c.field_0x94)) as f32;
    }
    c.field_0x94 = now as f32;
    let mut distance_step = 0.0f64;
    if c.field_0x88 != 0.0 {
        distance_step = diff(distance, c.field_0x88);
    }
    c.field_0x88 = distance;
    if !(c.field_0x84 <= distance) && (!(c.field_0x84 > distance) || !(distance_step >= 0.0)) {
        return false;
    }
    let accumulated = f64::from(time_step) + f64::from(c.field_0x90);
    c.field_0x90 = accumulated as f32;
    !(accumulated < f64::from(c.field_0x8c))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_watched_states_follow_their_names() {
        assert!(watch_trigger(create_id(b"ACTIVE"), STATE_ACTIVE));
        assert!(watch_trigger(create_id(b"ON"), STATE_ACTIVE));
        assert!(watch_trigger(create_id(b"OFF"), STATE_COMPLETE));
        assert!(watch_trigger(create_id(b"OFF"), STATE_INACTIVE));
        assert!(!watch_trigger(create_id(b"OFF"), STATE_ACTIVE));
        assert!(!watch_trigger(create_id(b"COMPLETE"), STATE_INACTIVE));
    }

    #[test]
    fn distance_ticks_only_while_in_range() {
        let (mut timer, mut last) = (1.0f32, 0.0f32);
        assert!(!tick_distance(
            4.0, 9.0, true, &mut timer, 1.0, 0, &mut last, 0.5
        ));
        assert_eq!((timer, last), (0.5, 4.0));
        assert!(tick_distance(
            5.0, 9.0, true, &mut timer, 1.0, 0, &mut last, 0.5
        ));
        assert!(!tick_distance(
            10.0, 9.0, true, &mut timer, 1.0, 0, &mut last, 0.5
        ));
        assert_eq!(timer, 1.0);
        // TICK_TOWARDS does not tick while the distance grows.
        let (mut timer, mut last) = (1.0f32, 1.0f32);
        assert!(!tick_distance(
            2.0, 9.0, true, &mut timer, 1.0, 1, &mut last, 0.5
        ));
        assert_eq!(timer, 1.0);
    }

    #[test]
    fn a_null_block_has_no_words() {
        assert!(block(&Parms::Null).is_none());
        assert_eq!(block(&Parms::Delta(0.5)).unwrap()[0], 0.5f32.to_bits());
        assert_eq!(block(&Parms::NpcEnter([1, 2, 3])).unwrap()[..3], [1, 2, 3]);
    }
}
