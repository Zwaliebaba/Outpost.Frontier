//! The runtime machine's sub-machines and its triggers: the records of what
//! it spawned (`FRmSolarMachine`, `FRmShipMachine`, `FRmLootMachine`,
//! `RandomMissionMachine.cpp:556-985`) and the trigger machines of TU57
//! (`RandomMissionTriggers.cpp:66-292`) and TU61 (`FRmShipCountTrigger`,
//! `FRmShipHealthTrigger`, `RandomMissionVignette.cpp:141-260`).

use neuron_core::math::Vector;
use neuron_core::num::trunc_to_i32;

use super::message::{Body, Controller, MessageType, RmMessage};
use super::{Event, Host, Loot, Machine, MachineKind, NpcShip, controller_of, is_enter, message_of};
use crate::api;
use crate::content::game_support::time_distance;
use crate::content::players;

/// A record of a spawned object (`FRmSpawnRecord` and its solar, ship and
/// loot forms; one struct, each form's fields beside the base's).
#[derive(Clone, Debug, PartialEq)]
pub struct SpawnRecord {
    /// `+0x04` (true): a ship record of the hostile group counts here.
    pub counted: bool,
    /// `Name` (`TString<0x30>`).
    pub name: Vec<u8>,
    /// `State`: 2 alive, 3 destroyed, 4 a loot taken without a player.
    pub state: i32,
    /// `Flags`: bit 1 a target.
    pub flags: u32,
    /// `Attitude` (-2 for none).
    pub attitude: f32,
    /// `ObjectId`.
    pub object: u32,
    /// `NpcShip`: a ship record's NPC ship.
    pub npc_ship: NpcShip,
    /// `Loot`: what it drops.
    pub loot: Loot,
    /// Solar `SolarType` (-1).
    pub solar_type: i32,
    /// Solar `Reputation` (-1).
    pub reputation: i32,
    /// Ship `Kind` (2 the big ship, 1 a defensive ship).
    pub ship_kind: i32,
    /// Ship `Group`: the group's serial.
    pub group: u32,
    /// Loot `ParentId`.
    pub parent: u32,
    /// Loot `Units`.
    pub units: i32,
    /// Loot `Kind` (2 critical).
    pub loot_kind: i32,
    /// Loot `Field_0x6c`: the critical loot's mode.
    pub mode: i32,
    /// Loot `HolderId`: the player holding it.
    pub holder: u32,
    /// Loot `CargoSlot` (`SUBOBJ_ID_NONE`).
    pub cargo_slot: u16,
    /// Loot `GoodId`.
    pub good: u32,
}

impl Default for SpawnRecord {
    /// `FRmSpawnRecord::FRmSpawnRecord` and the forms' defaults.
    fn default() -> Self {
        Self {
            counted: true,
            name: Vec::new(),
            state: 2,
            flags: 0,
            attitude: -2.0,
            object: 0,
            npc_ship: None,
            loot: Vec::new(),
            solar_type: -1,
            reputation: -1,
            ship_kind: 0,
            group: 0,
            parent: 0,
            units: 0,
            loot_kind: 0,
            mode: 0,
            holder: 0,
            cargo_slot: SUBOBJ_ID_NONE,
            good: 0,
        }
    }
}

/// `SUBOBJ_ID_NONE`.
pub const SUBOBJ_ID_NONE: u16 = 0xffff;

/// A sub-machine: its owner (the runtime machine's handler id) and its
/// records in list order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SubMachine {
    /// `OwnerId`.
    pub owner: u32,
    /// `Records`.
    pub records: Vec<SpawnRecord>,
}

impl SubMachine {
    /// `FindById`: the first record of the object; 0 never matches.
    #[must_use]
    pub fn find_by_id(&self, id: u32) -> Option<usize> {
        if id == 0 {
            return None;
        }
        self.records.iter().position(|r| r.object == id)
    }

    /// `FindByName`: the first record of the name (`strcmp`).
    #[must_use]
    pub fn find_by_name(&self, name: &[u8]) -> Option<usize> {
        self.records.iter().position(|r| r.name == name)
    }

    /// `FRmSolarMachine::Query(Kind, State, Mask)`: -1, 0 and 0 match any.
    #[must_use]
    pub fn query_solars(&self, kind: i32, state: i32, mask: u32) -> Vec<usize> {
        (0..self.records.len())
            .filter(|&i| {
                let r = &self.records[i];
                (kind == -1 || r.solar_type == kind)
                    && (state == 0 || r.state == state)
                    && (mask == 0 || r.flags & mask != 0)
            })
            .collect()
    }

    /// `FRmShipMachine::Query(Kind, State, Mask)`: 0 matches any.
    #[must_use]
    pub fn query_ships(&self, kind: i32, state: i32, mask: u32) -> Vec<usize> {
        (0..self.records.len())
            .filter(|&i| {
                let r = &self.records[i];
                (kind == 0 || r.ship_kind == kind)
                    && (state == 0 || r.state == state)
                    && (mask == 0 || r.flags & mask != 0)
            })
            .collect()
    }

    /// `FRmShipMachine::QueryGroup`: the records of the group; none for -1.
    #[must_use]
    pub fn query_group(&self, group: u32) -> Vec<usize> {
        if group == u32::MAX {
            return Vec::new();
        }
        (0..self.records.len())
            .filter(|&i| self.records[i].group == group)
            .collect()
    }

    /// `FRmLootMachine::Query(Field6c, Kind, Field60, State, Holder)`: 0
    /// matches any.
    #[must_use]
    pub fn query_loot(&self, mode: i32, kind: i32, parent: u32, state: i32, holder: u32) -> Vec<usize> {
        (0..self.records.len())
            .filter(|&i| {
                let r = &self.records[i];
                (parent == 0 || r.parent == parent)
                    && (holder == 0 || r.holder == holder)
                    && (mode == 0 || r.mode == mode)
                    && (kind == 0 || r.loot_kind == kind)
                    && (state == 0 || r.state == state)
            })
            .collect()
    }
}

/// A trigger (`FRmTrigger`): states 0 idle, 1 armed, 2 fired, 3 waiting to
/// re-arm, 4 done.
#[derive(Clone, Debug, PartialEq)]
pub struct Trigger {
    /// `OwnerId`: the handler the fired message goes to.
    pub owner: u32,
    /// `Activator`.
    pub activator: u32,
    /// `bOneShot`.
    pub one_shot: bool,
    /// `RearmDelay`.
    pub rearm_delay: f32,
    /// `LastFireTime`.
    pub last_fire_time: f64,
    /// `Cookie`.
    pub cookie: u32,
    /// The kind and its fields.
    pub kind: TriggerKind,
}

impl Trigger {
    /// The base constructor every kind uses (`bOneShot` 1, `RearmDelay` 1).
    #[must_use]
    pub fn new(cookie: u32, owner: u32, kind: TriggerKind) -> Self {
        Self {
            owner,
            activator: 0,
            one_shot: true,
            rearm_delay: 1.0,
            last_fire_time: 0.0,
            cookie,
            kind,
        }
    }
}

/// The trigger kinds.
#[derive(Clone, Debug, PartialEq)]
pub enum TriggerKind {
    /// `FRmProximityTrigger`: a mission player's ship of the system within
    /// the radius of the centre.
    Proximity {
        /// `Center`.
        center: Vector,
        /// `RadiusSq`.
        radius_sq: f32,
        /// `SystemId`.
        system: u32,
    },
    /// `FRmTimerTrigger`: the delay after it is armed.
    Timer {
        /// `Delay`.
        delay: f32,
    },
    /// `FRmProximityLeaveTrigger`: no such ship within the radius.
    Leave {
        /// `Center`.
        center: Vector,
        /// `RadiusSq`.
        radius_sq: f32,
        /// `SystemId`.
        system: u32,
    },
    /// `FRmShipCountTrigger`: at most `threshold` records counted alive.
    ShipCount {
        /// The ship machine whose records it counts.
        ships: u32,
        /// `Threshold`.
        threshold: i32,
    },
    /// `FRmShipHealthTrigger`: the health of the records of a kind.
    ShipHealth {
        /// The ship machine.
        ships: u32,
        /// `Threshold`.
        threshold: i32,
        /// `Kind`.
        kind: i32,
    },
}

impl TriggerKind {
    /// The kind's name in the traces.
    #[must_use]
    pub const fn name(&self) -> &'static str {
        match self {
            Self::Proximity { .. } => "proximity",
            Self::Timer { .. } => "timer",
            Self::Leave { .. } => "leave",
            Self::ShipCount { .. } => "count",
            Self::ShipHealth { .. } => "health",
        }
    }

    /// `InRadius * InRadius`, stored as a float.
    #[must_use]
    pub fn square(radius: f32) -> f32 {
        (f64::from(radius) * f64::from(radius)) as f32
    }
}

/// The records of sub-machine `key`.
fn sub<'a>(host: &'a Host<'_, '_>, key: u32) -> Option<&'a SubMachine> {
    match &host.machine_ref(key)?.kind {
        MachineKind::Solars(s) | MachineKind::Ships(s) | MachineKind::Loot(s) => Some(s),
        _ => None,
    }
}

/// The records of sub-machine `key`, to change.
pub(super) fn sub_mut<'a>(host: &'a mut Host<'_, '_>, key: u32) -> Option<&'a mut SubMachine> {
    match &mut host.machine_mut(key)?.kind {
        MachineKind::Solars(s) | MachineKind::Ships(s) | MachineKind::Loot(s) => Some(s),
        _ => None,
    }
}

/// A sub-machine's records (for the runtime machine's queries).
pub(super) fn records<'a>(host: &'a Host<'_, '_>, key: u32) -> &'a [SpawnRecord] {
    sub(host, key).map_or(&[], |s| s.records.as_slice())
}

/// A sub-machine as built by the runtime machine's constructor.
pub(super) fn sub_machine(kind: fn(SubMachine) -> MachineKind, owner: u32) -> Machine {
    Machine {
        sm: neuron_core::util::state_machine::StateMachine::new(),
        kind: kind(SubMachine {
            owner,
            records: Vec::new(),
        }),
    }
}

/// `FRmSolarMachine` slot 2 (`:595-614`): in state 0, `ObjectDestroyed`
/// of a record makes it destroyed and tells the owner.
pub(super) fn solar_slot2(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>, state: i32) -> bool {
    if state != 0 {
        return false;
    }
    let Some(Controller::ObjectDestroyed { object, .. }) = controller_of(event).map(|c| c.message)
    else {
        return false;
    };
    let Some(s) = sub_mut(host, key) else {
        return true;
    };
    let owner = s.owner;
    if let Some(i) = s.find_by_id(object) {
        s.records[i].state = 3;
        let id = s.records[i].object;
        host.send_immediate(
            key,
            RmMessage::new(MessageType::SolarDestroyed, Body::Solar { object: id }),
            owner,
        );
    }
    true
}

/// `FRmShipMachine` slot 2 (`:664-713`): in state 0, `ObjectDestroyed` of
/// a record: destroyed, the owner told; a group all destroyed and a
/// target group without a live target are reported too.
pub(super) fn ship_slot2(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>, state: i32) -> bool {
    if state != 0 {
        return false;
    }
    let Some(Controller::ObjectDestroyed { object, .. }) = controller_of(event).map(|c| c.message)
    else {
        return false;
    };
    let Some(s) = sub_mut(host, key) else {
        return true;
    };
    let Some(i) = s.find_by_id(object) else {
        return true;
    };
    let owner = s.owner;
    s.records[i].state = 3;
    let (group, id, flagged) = (
        s.records[i].group,
        s.records[i].object,
        (s.records[i].flags >> 1) & 1 != 0,
    );
    host.send_immediate(
        key,
        RmMessage::new(MessageType::ShipDestroyed, Body::Ship { group, object: id }),
        owner,
    );
    // The record is read again: a nested handler may have changed it.
    let Some(s) = sub(host, key) else { return true };
    let group = s.records.get(i).map_or(group, |r| r.group);
    let members = s.query_group(group);
    let mut dead = 0;
    let mut no_flagged_alive = true;
    for &m in &members {
        let r = &s.records[m];
        if r.state == 3 {
            dead += 1;
        }
        if r.state == 2 && r.flags & 2 != 0 {
            no_flagged_alive = false;
        }
    }
    let object = s.records.get(i).map_or(id, |r| r.object);
    if dead == members.len() {
        host.send_immediate(
            key,
            RmMessage::new(MessageType::GroupDestroyed, Body::Ship { group, object }),
            owner,
        );
    }
    if flagged && no_flagged_alive {
        host.send_immediate(
            key,
            RmMessage::new(MessageType::FlaggedShipsLost, Body::Ship { group, object }),
            owner,
        );
    }
    true
}

/// `FRmLootMachine` slot 2 (`:786-984`): in state 0, the drop of a held
/// loot (a routed message), and the controller messages 0x39, 0x18, 0x27
/// and 0x3d.
#[allow(clippy::too_many_lines)]
pub(super) fn loot_slot2(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>, state: i32) -> bool {
    if state != 0 {
        return false;
    }
    let Some(owner) = sub(host, key).map(|s| s.owner) else {
        return false;
    };
    let current = host.state_of(key);
    if let Some(m) = message_of(event)
        && m.is(MessageType::LootDrop, current)
    {
        let Body::Loot { object, value: ship } = m.body else {
            return true;
        };
        drop_loot(host, key, owner, object, ship);
        return true;
    }
    let Some(c) = controller_of(event) else {
        return false;
    };
    match c.message {
        Controller::ObjectDestroyed {
            object,
            killer,
            kind,
            ..
        } => {
            let Some(s) = sub_mut(host, key) else { return true };
            let Some(i) = s.find_by_id(object) else {
                return true;
            };
            s.records[i].state = if kind == 1 || kind == 0 { 3 } else { 4 };
            let (id, loot_kind) = (s.records[i].object, s.records[i].loot_kind);
            if kind == 1 {
                host.send_immediate(
                    key,
                    RmMessage::new(MessageType::LootDestroyedByPlayer, Body::Loot { object: id, value: 0 }),
                    owner,
                );
            } else if kind == 0 {
                host.send_immediate(
                    key,
                    RmMessage::new(MessageType::LootDestroyed, Body::Loot { object: id, value: 0 }),
                    owner,
                );
            }
            if loot_kind == 2 && sub(host, key).is_some_and(|s| s.query_loot(0, 2, 0, 2, 0).is_empty()) {
                host.send_immediate(
                    key,
                    RmMessage::new(MessageType::CriticalLootGone, Body::Loot { object: id, value: killer }),
                    owner,
                );
            }
            true
        }
        Controller::LootAcquired {
            loot,
            ship,
            slot,
            good,
            count,
        } => {
            let holder = players::find_by_ship(&host.cx.logic.content.state.players, ship).unwrap_or(0);
            let Some(s) = sub_mut(host, key) else { return true };
            let Some(i) = s.find_by_id(loot) else {
                return true;
            };
            let r = &mut s.records[i];
            r.units = r.units.wrapping_sub(count);
            r.good = good;
            r.cargo_slot = slot;
            r.holder = holder;
            if r.units != 0 {
                return true;
            }
            let (id, loot_kind) = (r.object, r.loot_kind);
            host.send_immediate(
                key,
                RmMessage::new(MessageType::LootPickedUp, Body::Loot { object: id, value: ship }),
                owner,
            );
            if loot_kind == 2 && sub(host, key).is_some_and(|s| s.query_loot(0, 2, 0, 2, 0).is_empty()) {
                host.send_immediate(
                    key,
                    RmMessage::new(MessageType::CriticalLootGone, Body::Loot { object: id, value: ship }),
                    owner,
                );
            }
            true
        }
        Controller::Docked { ship, base } => {
            let Some(player) = players::find_by_ship(&host.cx.logic.content.state.players, ship) else {
                return true;
            };
            let held: Vec<u32> = sub(host, key).map_or_else(Vec::new, |s| {
                s.query_loot(0, 2, 0, 4, player)
                    .into_iter()
                    .map(|i| s.records[i].object)
                    .collect()
            });
            for object in held {
                host.send_immediate(
                    key,
                    RmMessage::new(MessageType::LootHolderDocked, Body::LootPlayer { object, ship, base }),
                    owner,
                );
            }
            true
        }
        Controller::SystemUnloaded(system) => {
            let first = sub(host, key).and_then(|s| {
                s.query_loot(0, 2, 0, 2, 0).first().map(|&i| s.records[i].object)
            });
            if let Some(object) = first {
                let in_system = api::space_obj::system(host.cx, object).unwrap_or(0);
                if system == in_system {
                    let _ = api::space_obj::destroy(host.cx, object, 0);
                }
            }
            true
        }
        _ => false,
    }
}

/// The loot machine's drop (`:796-863`): the loot leaves its holder's
/// cargo and is created again 10 units above the ship, with the ship's
/// motion and a hit point scale of `rand() / 32768 * 0.5 + 0.5`.
fn drop_loot(host: &mut Host<'_, '_>, key: u32, owner: u32, object: u32, ship: u32) {
    let player = players::find_by_ship(&host.cx.logic.content.state.players, ship);
    let Some((holder, slot, good)) = sub(host, key)
        .and_then(|s| s.find_by_id(object).map(|i| &s.records[i]))
        .map(|r| (r.holder, r.cargo_slot, r.good))
    else {
        return;
    };
    let _ = api::player::remove_cargo(host.cx, holder, slot, 1);
    let Some(player) = player else { return };
    let system = host
        .cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .and_then(|s| s.system)
        .unwrap_or(0);
    let player_ship = host
        .cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map_or(0, players::PlayerState::ship_id);
    let archetype = api::space_obj::archetype_id(host.cx, good).unwrap_or(0);
    let (linear, angular) =
        api::space_obj::motion(host.cx, player_ship).unwrap_or((Vector::ZERO, Vector::ZERO));
    let at = super::player_position(host.cx, player);
    let roll = host.cx.world.rng().rand();
    let scaled = (f64::from(roll) * 3.051_757_812_5e-05) as f32;
    let mut info = api::space_obj::LootInfo::new(
        system,
        archetype,
        1,
        Vector::new(at.x, (f64::from(at.y) + 10.0) as f32, at.z),
    );
    info.owner = player_ship;
    info.velocity = Vector::new(linear.x, (f64::from(linear.y) + 10.0) as f32, linear.z);
    info.angular_velocity = angular;
    info.hit_pts_scale = (f64::from(scaled) * 0.5 + 0.5) as f32;
    info.can_ai_tractor = false;
    info.mission = true;
    // `CreateLoot(Record->ObjectId, Info)`: the record takes the new id.
    let created = api::space_obj::create_loot(host.cx, &info).ok();
    let Some(s) = sub_mut(host, key) else { return };
    let Some(i) = s.find_by_id(object) else { return };
    let r = &mut s.records[i];
    if let Some(id) = created {
        r.object = id;
    }
    let object = r.object;
    r.state = 2;
    r.units = 1;
    r.holder = 0;
    r.cargo_slot = SUBOBJ_ID_NONE;
    host.send_immediate(
        key,
        RmMessage::new(MessageType::LootRespawned, Body::Loot { object, value: ship }),
        owner,
    );
}

/// The trigger of machine `key`.
fn trigger<'a>(host: &'a Host<'_, '_>, key: u32) -> Option<&'a Trigger> {
    match &host.machine_ref(key)?.kind {
        MachineKind::Trigger(t) => Some(t),
        _ => None,
    }
}

/// The trigger of machine `key`, to change.
fn trigger_mut<'a>(host: &'a mut Host<'_, '_>, key: u32) -> Option<&'a mut Trigger> {
    match &mut host.machine_mut(key)?.kind {
        MachineKind::Trigger(t) => Some(t),
        _ => None,
    }
}

/// `FRmTrigger::Slot2` (`:94-109`): the state picks the slot; its result
/// is dropped.
pub(super) fn trigger_slot2(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>, state: i32) -> bool {
    match state {
        0 => {
            if is_enter(event) {
                host.set_state(key, 1);
            }
        }
        1 => armed(host, key, event),
        2 => {
            if is_enter(event) {
                fire(host, key);
                let Some(t) = trigger(host, key) else {
                    return false;
                };
                let next = if t.one_shot {
                    4
                } else if t.rearm_delay > 0.0 {
                    3
                } else {
                    1
                };
                host.set_state(key, next);
            }
        }
        3 => {
            if matches!(controller_of(event).map(|c| c.message), Some(Controller::Heartbeat(_))) {
                let now = host.cx.logic.content.state.game_time;
                let Some(t) = trigger(host, key) else {
                    return false;
                };
                if time_distance(now, t.last_fire_time) > f64::from(t.rearm_delay) {
                    host.set_state(key, 1);
                }
            }
        }
        _ => {}
    }
    false
}

/// `FRmTrigger::Fire` (`:150-158`): the game time kept; the fired message
/// (final when one-shot, unless the time is the -1 sentinel) to the owner
/// after 0.01 s.
fn fire(host: &mut Host<'_, '_>, key: u32) {
    let now = host.cx.logic.content.state.game_time;
    let Some(t) = trigger_mut(host, key) else {
        return;
    };
    t.last_fire_time = now;
    let last = now != -1.0 && t.one_shot;
    let (owner, cookie, activator) = (t.owner, t.cookie, t.activator);
    host.send_delayed(
        key,
        RmMessage::new(
            MessageType::TriggerFired,
            Body::TriggerFired {
                cookie,
                last,
                activator,
            },
        ),
        0.01,
        owner,
    );
}

/// The squared distance of the triggers (`RmTriggerDistanceSq`): the
/// deltas in x, y, z order, `(dz * dz + dx * dx) + dy * dy`.
fn distance_sq(at: Vector, center: Vector) -> f64 {
    let dx = f64::from(at.x) - f64::from(center.x);
    let dy = f64::from(at.y) - f64::from(center.y);
    let dz = f64::from(at.z) - f64::from(center.z);
    dz * dz + dx * dx + dy * dy
}

/// The mission players with a ship in `system` and their ship's position.
fn ships_in_system(host: &mut Host<'_, '_>, system: u32) -> Vec<(u32, u32, Vector)> {
    let mut out = Vec::new();
    for player in host.players() {
        let Some(s) = host.cx.logic.content.state.players.get(&player) else {
            continue;
        };
        let Some(ship) = &s.ship else { continue };
        // The C++ does not test the system record (D1025).
        if s.system != Some(system) {
            continue;
        }
        let ship = ship.ship;
        let at = super::player_position(host.cx, player);
        out.push((player, ship, at));
    }
    out
}

/// Slot 6 (`OnArmed`) of each kind, in state 1.
fn armed(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) {
    let Some(kind) = trigger(host, key).map(|t| t.kind.clone()) else {
        return;
    };
    let heartbeat = matches!(
        controller_of(event).map(|c| c.message),
        Some(Controller::Heartbeat(_))
    );
    match kind {
        TriggerKind::Proximity {
            center,
            radius_sq,
            system,
        } => {
            if !heartbeat {
                return;
            }
            for (_, ship, at) in ships_in_system(host, system) {
                if distance_sq(at, center) <= f64::from(radius_sq) {
                    if let Some(t) = trigger_mut(host, key) {
                        t.activator = ship;
                    }
                    host.set_state(key, 2);
                    return;
                }
            }
        }
        TriggerKind::Leave {
            center,
            radius_sq,
            system,
        } => {
            if !heartbeat {
                return;
            }
            let mut last = None;
            for (_, ship, at) in ships_in_system(host, system) {
                let d = distance_sq(at, center);
                if d <= f64::from(radius_sq) {
                    return;
                }
                if d < 1.0e10 {
                    last = Some(ship);
                }
            }
            if let Some(ship) = last
                && let Some(t) = trigger_mut(host, key)
            {
                t.activator = ship;
            }
            host.set_state(key, 2);
        }
        TriggerKind::Timer { delay } => {
            if is_enter(event) {
                host.send_to_me_self(key, RmMessage::plain(MessageType::Timeout), delay);
                return;
            }
            let current = host.state_of(key);
            if message_of(event).is_some_and(|m| m.is(MessageType::Timeout, current)) {
                host.set_state(key, 2);
            }
        }
        TriggerKind::ShipCount { ships, threshold } => {
            let destroyed = matches!(
                controller_of(event).map(|c| c.message),
                Some(Controller::ObjectDestroyed { .. })
            );
            if !is_enter(event) && !destroyed {
                return;
            }
            let alive = records(host, ships)
                .iter()
                .filter(|r| r.counted && r.state == 2)
                .count() as i32;
            if alive <= threshold {
                host.set_state(key, 2);
            }
        }
        TriggerKind::ShipHealth {
            ships,
            threshold,
            kind,
        } => {
            if is_enter(event) {
                let lowest = lowest_health(host, ships, kind);
                if lowest != -1 && lowest <= threshold {
                    host.set_state(key, 2);
                }
                return;
            }
            let Some(Controller::ObjectDamaged {
                object,
                relative_damage,
            }) = controller_of(event).map(|c| c.message)
            else {
                return;
            };
            let Some(record) = records(host, ships).iter().find(|r| r.object == object) else {
                return;
            };
            if record.ship_kind != kind {
                return;
            }
            let id = record.object;
            let (current, max) = api::space_obj::health(host.cx, id).unwrap_or((0.0, 0.0));
            let health = trunc_to_i32(f64::from(current) / f64::from(max) * -100.0);
            let damage = trunc_to_i32(f64::from(relative_damage) * -100.0);
            if damage.wrapping_sub(health) < threshold {
                if let Some(t) = trigger_mut(host, key) {
                    t.activator = id;
                }
                host.set_state(key, 2);
            }
        }
    }
}

/// `FRmShipHealthTrigger::GetLowestHealth` (`:244-260`): the lowest health
/// percent of the counted, alive records of the kind; -1 without one.
fn lowest_health(host: &mut Host<'_, '_>, ships: u32, kind: i32) -> i32 {
    let ids: Vec<u32> = records(host, ships)
        .iter()
        .filter(|r| r.counted && r.state == 2 && r.ship_kind == kind)
        .map(|r| r.object)
        .collect();
    let mut lowest = -1;
    for id in ids {
        let (current, max) = api::space_obj::health(host.cx, id).unwrap_or((0.0, 0.0));
        let health = trunc_to_i32(f64::from(current) / f64::from(max) * 100.0);
        if lowest == -1 || health < lowest {
            lowest = health;
        }
    }
    lowest
}
