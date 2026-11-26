//! The server's state of the shared world's objects (work package P3.3,
//! decision record 0007, point 1): `GameObject`'s fields
//! (`include/server_gameobject.h:251`) in a side table keyed by the
//! object's handle, and the base class's behaviour
//! (`Server/server_gameobject.cpp`) through the world's server hooks
//! ([`crate::world`]).
//!
//! An entry is made the first time the server touches an object, with the
//! C++ constructor's defaults (`InvincibleTimer` 1, the pending damage's
//! cause 0x18), since the world makes objects (munitions, loot) without
//! asking the server. Entries of objects the world has reaped are swept
//! after each sub-step's deletes.
//!
//! The per-kind wrappers' state lives in the entry too: `EqObj`'s fuses
//! ([`crate::fuses`]) and server equipment, `Ship`'s ([`crate::ship`]),
//! `Loot`'s ([`crate::loot`]) and `Mine`'s ([`crate::munitions`]).

use std::collections::BTreeMap;

use game_sim::equip::{Fate, SUBOBJ_ID_ROOT};
use game_sim::object::damage::DamageList;
use game_sim::object::world::{DamageState, Hit, HitRule};
use game_sim::object::{ObjHandle, ObjectKind, World};
use neuron_server::controller::ControllerId;

use crate::events::{ControllerMessage, ObjectDestroyed};
use crate::server::Cx;

const GAMEOBJ: &str = "Server\\space\\Gameobj.cpp";

/// The cause a fresh pending damage list carries (`GameObject`'s
/// constructor).
const PENDING_CAUSE: game_sim::object::damage::DamageCause = 0x18;

/// One invincibility command (`FInvincibility`, `pub::SpaceObj::
/// SetInvincible`): its kind, whether it protects from everyone and from
/// players, and the largest fraction of hit points the object may lose.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Invincibility {
    /// `Type`: commands of one type replace each other.
    pub kind: u32,
    /// `bProtect`.
    pub protect: bool,
    /// `bFromPlayers`.
    pub from_players: bool,
    /// `MaxHpLoss`.
    pub max_hp_loss: f32,
}

/// `GameObject`'s server state.
#[derive(Clone, Debug, PartialEq)]
pub struct ServerObject {
    /// `MarkedByPlayers`, in marking order.
    pub marked_by: Vec<u32>,
    /// `LastDamageTime`.
    pub last_damage_time: f64,
    /// `Invincibilities`.
    pub invincibilities: Vec<Invincibility>,
    /// `bInvincible`.
    pub invincible: bool,
    /// `bInvincibleFlag`: protected from players.
    pub invincible_flag: bool,
    /// `InvincibleTimer`.
    pub invincible_timer: f32,
    /// `bDestroyed`.
    pub destroyed: bool,
    /// `bInDamageCallback`.
    pub in_damage_callback: bool,
    /// `PendingDamage`: damage set while a list was being applied.
    pub pending_damage: DamageList,
    /// `EqObj::bDeathFuseLit`: `destroy`'s death-fuse branch ran, or a
    /// fuse with a `destroy_root` was lit.
    pub death_fuse_lit: bool,
    /// `EqObj::Fuses`: the fuses the object has lit, in lighting order
    /// (never removed).
    pub fuses: Vec<crate::fuses::ServerFuse>,
    /// A loot container's own state (`Loot`).
    pub loot: Option<crate::loot::LootState>,
    /// `EqObj::ServerEquips`: the server's wrappers of the equipment
    /// ([`crate::ship::rebuild_server_equip`]).
    pub server_equip: Vec<crate::ship::ServerEquip>,
    /// A ship's own state (`Ship`).
    pub ship: Option<crate::ship::ShipState>,
    /// `EqObj::DockQueue`: the dock requests the object hosts, in order
    /// ([`crate::dock`]).
    pub dock_queue: Vec<crate::dock::DockRequest>,
    /// A mine's own state (`Mine`).
    pub mine: Option<crate::munitions::MineState>,
    /// `EqObj::CargoDecayTimer`: the time since the cargo last decayed
    /// ([`crate::cargo::decay_cargo`]).
    pub cargo_decay_timer: f32,
}

impl Default for ServerObject {
    fn default() -> Self {
        Self {
            marked_by: Vec::new(),
            last_damage_time: 0.0,
            invincibilities: Vec::new(),
            invincible: false,
            invincible_flag: false,
            invincible_timer: 1.0,
            destroyed: false,
            in_damage_callback: false,
            pending_damage: DamageList::new(PENDING_CAUSE, 0, 0),
            death_fuse_lit: false,
            fuses: Vec::new(),
            loot: None,
            server_equip: Vec::new(),
            ship: None,
            dock_queue: Vec::new(),
            mine: None,
            cargo_decay_timer: 0.0,
        }
    }
}

impl ServerObject {
    /// `mark_for_player`: mark 1 adds the player once, mark 0 removes it;
    /// other values do nothing.
    pub fn mark_for_player(&mut self, player: u32, mark: i32) {
        let at = self.marked_by.iter().position(|&p| p == player);
        match (mark, at) {
            (1, None) => self.marked_by.push(player),
            (0, Some(i)) => {
                self.marked_by.remove(i);
            }
            _ => {}
        }
    }

    /// `is_marked_by`.
    #[must_use]
    pub fn is_marked_by(&self, player: u32) -> bool {
        self.marked_by.contains(&player)
    }

    /// `set_invincible`: the command's flags and loss become the object's,
    /// then each other kept command adds its flags and lowers the loss to
    /// its own when smaller; a command of a kind already kept replaces it
    /// (and is not counted again), a new kind is kept at the end.
    pub fn set_invincible(&mut self, command: Invincibility) {
        self.invincible = command.protect;
        self.invincible_flag = command.from_players;
        self.invincible_timer = command.max_hp_loss;
        let mut new = true;
        for kept in &mut self.invincibilities {
            if kept.kind == command.kind {
                *kept = command;
                new = false;
                continue;
            }
            if !self.invincible {
                self.invincible = kept.protect;
            }
            if !self.invincible_flag {
                self.invincible_flag = kept.from_players;
            }
            // `(Node > Current) ? Current : Node`: unordered takes the node.
            if kept.max_hp_loss <= self.invincible_timer
                || kept.max_hp_loss.is_nan()
                || self.invincible_timer.is_nan()
            {
                self.invincible_timer = kept.max_hp_loss;
            }
        }
        if new {
            self.invincibilities.push(command);
        }
    }

    /// `is_damage_blocked`: never for causes from 0x10 up; for a player's
    /// damage the player flag, else the general one.
    #[must_use]
    pub fn is_damage_blocked(
        &self,
        cause: game_sim::object::damage::DamageCause,
        inflictor_owner: u32,
    ) -> bool {
        if cause >= 0x10 {
            return false;
        }
        if inflictor_owner != 0 {
            self.invincible_flag
        } else {
            self.invincible
        }
    }
}

/// The side table (`ServerObjects`, record 0007 point 1).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ServerObjects {
    entries: BTreeMap<ObjHandle, ServerObject>,
}

impl ServerObjects {
    /// The entry of an object, if the server has one.
    #[must_use]
    pub fn get(&self, h: ObjHandle) -> Option<&ServerObject> {
        self.entries.get(&h)
    }

    /// The entry of a live object, made with the constructor's defaults on
    /// first use; `None` for an object the world does not have.
    pub fn entry(&mut self, world: &World, h: ObjHandle) -> Option<&mut ServerObject> {
        world.get(h)?;
        Some(self.entries.entry(h).or_default())
    }

    /// The entry of an object, if the server has one, mutably.
    pub fn entry_mut(&mut self, h: ObjHandle) -> Option<&mut ServerObject> {
        self.entries.get_mut(&h)
    }

    /// Drops the entries of objects the world has reaped.
    pub fn sweep(&mut self, world: &World) {
        self.entries.retain(|h, _| world.get(*h).is_some());
    }

    /// The number of entries.
    #[must_use]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Whether there are none.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

/// `damage_state` for the world's damage computation: the invincibility
/// `is_damage_blocked` answers for a list whose inflictor belongs to
/// `inflictor_owner`, with its loss (the computation tests the cause
/// itself).
#[must_use]
pub fn damage_state(objects: &ServerObjects, h: ObjHandle, inflictor_owner: u32) -> DamageState {
    let Some(o) = objects.get(h) else {
        return DamageState::default();
    };
    let blocked = if inflictor_owner != 0 {
        o.invincible_flag
    } else {
        o.invincible
    };
    DamageState {
        invincible: blocked.then_some(o.invincible_timer),
        difficulty_scale: None,
        death_fuse_burning: crate::fuses::is_death_fuse_burning(objects, h),
        root_immune: false,
    }
}

/// The wrappers' per-kind hit rules (decision O46): an indestructible
/// solar's root takes no damage (`Solar::damage_sub_obj`, `munition_hit`,
/// `explosion_hit_hull`); the asteroid fields' rocks and mines take none
/// (`MedAsteroid`, `MedMine`); a counter-measure is destroyed by any
/// collision or munition (`CounterMeasure`); a mine or guided munition by
/// any collision and by a seeking munition (`Mine::munition_hit`, which a
/// guided munition shares).
#[must_use]
pub fn hit_rule(world: &World, h: ObjHandle, hit: Hit) -> HitRule {
    let Some(o) = world.get(h) else {
        return HitRule::Common;
    };
    match (o.kind, hit) {
        (ObjectKind::Solar, _) => match &o.data {
            game_sim::object::KindData::Solar(s) if !s.destructible => HitRule::RootImmune,
            _ => HitRule::Common,
        },
        (ObjectKind::Asteroid, _) => HitRule::Ignore,
        (ObjectKind::CounterMeasure, Hit::SubObj | Hit::Munition { .. }) => HitRule::Destroy,
        (ObjectKind::Mine | ObjectKind::Guided, Hit::SubObj | Hit::Munition { seeker: true }) => {
            HitRule::Destroy
        }
        _ => HitRule::Common,
    }
}

/// `Solar::munition_hit`'s and `explosion_hit_hull`'s notice of a root hit
/// on an indestructible solar (`Server/server_gameobject.cpp:6271-6320`):
/// `ObjectDamaged` at relative health 1 with no damage, and the same
/// record to the solar's behaviour manager (which adds no damage).
pub fn root_hit_ignored(cx: &mut Cx<'_>, h: ObjHandle, list: &DamageList) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let (id, system) = (o.simple.id, o.physical.system);
    notify_damaged(
        cx,
        h,
        crate::events::ObjectDamaged {
            id,
            system,
            inflictor: list.inflictor_id,
            equip_type: 0,
            collision_group_type: 0,
            cause: list.cause,
            damage: 0.0,
            relative_damage: 0.0,
            relative_health: 1.0,
        },
    );
}

/// `ObjectDamaged` to the bus and the damage record to the object's
/// behaviour manager (`EqObj_NotifyBehaviorOfDamage`, which carries the
/// event's fields and the cause, `Server/server_gameobject.cpp:1497-1520`).
fn notify_damaged(cx: &mut Cx<'_>, h: ObjHandle, event: crate::events::ObjectDamaged) {
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::ObjectDamaged(event),
    );
    cx.world
        .behavior_damage_event(h, event.id, event.cause, event.damage);
}

/// The server's part of entry `i` of a damage list applied to a ship or a
/// solar, before the entry is written (`EqObj::apply_root_entry`,
/// `apply_equip_entry`, `apply_power_entry`, `apply_group_entry`,
/// `Server/server_gameobject.cpp:1528-1720`):
///
/// - **Root**: `ObjectDamaged` with the hull's hit points before less the
///   entry's and both over the archetype's hit points, the behaviour's
///   damage record, then the archetype's fuses
///   ([`crate::fuses::sweep_entry`]).
/// - **Equipment**: nothing for an item the object lacks or that is
///   destroyed; otherwise `ObjectDamaged` with the item's type, its hit
///   points before less the entry's and both over its maximum (0 for a
///   maximum of 0), and the behaviour's record. (A live launcher's time of
///   damage is stamped with the list, [`crate::ship::stamp_damage_times`].)
/// - **Power**: `ObjectDamaged` with type `0x40000` over the maximum power,
///   and the behaviour's record.
/// - **Group**: a fate clears the object's dock queue (a fate of 4 is
///   reported: persistent groups are not supported); `ObjectDamaged` with
///   the group's collision type over its maximum hit points (unrounded, 0
///   for a maximum of 0), the behaviour's record, then the group's fuses
///   and the death fuse's hold on the entry's fate
///   ([`crate::fuses::sweep_entry`]). A group the object lacks is skipped
///   (the C++ reads through null, D618).
pub fn damage_entry(cx: &mut Cx<'_>, h: ObjHandle, list: &mut DamageList, i: usize) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    if !matches!(o.kind, ObjectKind::Ship | ObjectKind::Solar) {
        return;
    }
    let Some(eq) = o.equipped.as_deref() else {
        return;
    };
    let Some(&entry) = list.entries.get(i) else {
        return;
    };
    let base = crate::events::ObjectDamaged {
        id: o.simple.id,
        system: o.physical.system,
        inflictor: list.inflictor_id,
        equip_type: 0,
        collision_group_type: 0,
        cause: list.cause,
        damage: 0.0,
        relative_damage: 0.0,
        relative_health: 0.0,
    };
    let after = f64::from(entry.hit_pts);
    let id = entry.sub_obj_id;
    if id == SUBOBJ_ID_ROOT {
        let max = f64::from(o.simple.max_hit_pts);
        let damage = (f64::from(o.simple.hit_pts) - after) as f32;
        let event = crate::events::ObjectDamaged {
            damage,
            relative_damage: (f64::from(damage) / max) as f32,
            relative_health: (after / max) as f32,
            ..base
        };
        notify_damaged(cx, h, event);
        crate::fuses::sweep_entry(cx, h, list, i);
    } else if id == game_sim::equip::SUBOBJ_ID_POWER {
        let max = f64::from(eq.max_power);
        let damage = (f64::from(eq.power) - after) as f32;
        let event = crate::events::ObjectDamaged {
            equip_type: 0x40000,
            damage,
            relative_damage: (f64::from(damage) / max) as f32,
            relative_health: (after / max) as f32,
            ..base
        };
        notify_damaged(cx, h, event);
    } else if neuron_core::id::sub_object::is_arch_group_id(id) {
        let Some(group) = eq.arch_groups.iter().find(|g| g.id == id) else {
            return;
        };
        let (group_type, before, max) = (
            group.group_type,
            group.hit_pts,
            f64::from(group.max_hit_pts),
        );
        if entry.fate != Fate::None {
            if let Some(server) = cx.logic.objects.entry(cx.world, h) {
                server.dock_queue.clear();
            }
            if entry.fate.value() == 4 {
                cx.logic.reports.push(
                    "Server\\Space\\EqObj.cpp",
                    0xaf5,
                    "*** WARNING: Persistent CArchGroups not supported.".to_owned(),
                );
            }
        }
        let damage = (f64::from(before) - after) as f32;
        let (relative_damage, relative_health) = if max != 0.0 {
            ((f64::from(damage) / max) as f32, (after / max) as f32)
        } else {
            (0.0, 0.0)
        };
        let event = crate::events::ObjectDamaged {
            collision_group_type: group_type,
            damage,
            relative_damage,
            relative_health,
            ..base
        };
        notify_damaged(cx, h, event);
        crate::fuses::sweep_entry(cx, h, list, i);
    } else if neuron_core::id::sub_object::is_equip_id(id) {
        let Some(item) = eq.equipment.by_id(id).filter(|e| !e.is_destroyed()) else {
            return;
        };
        let max = f64::from(item.max_hit_points());
        let damage = (f64::from(item.hit_points()) - after) as f32;
        let (relative_damage, relative_health) = if max != 0.0 {
            ((f64::from(damage) / max) as f32, (after / max) as f32)
        } else {
            (0.0, 0.0)
        };
        let event = crate::events::ObjectDamaged {
            equip_type: item.type_bits,
            damage,
            relative_damage,
            relative_health,
            ..base
        };
        notify_damaged(cx, h, event);
    }
}

/// `GameObject::apply_damage_list`'s head: a list applied inside another's
/// callback is reported (and applied all the same), then the callback
/// latch is set.
pub fn begin_damage_list(cx: &mut Cx<'_>, h: ObjHandle, list: &DamageList) {
    // `Ship::apply_damage_list`: a cruise disruptor's list (cause 6) on an
    // NPC ship ends its cruise first (`slot_50(false, true, 0)`); a cruise
    // that ended tells the players who can see the ship (`notify_cruise`).
    if list.cause == 6
        && cx
            .world
            .get(h)
            .is_some_and(|o| o.kind == ObjectKind::Ship && o.simple.owner_player == 0)
    {
        let cruising = crate::replication::cruise_active(cx.world, h);
        cx.world.command(
            h,
            game_sim::object::Command::EngageCruise {
                engage: false,
                instant: true,
            },
        );
        if cruising
            && let Some((system, id)) = cx.world.get(h).map(|o| (o.physical.system, o.simple.id))
        {
            crate::replication::cruise_changed(cx, system, id, false, true, 0);
        }
    }
    let id = cx.world.get(h).map_or(0, |o| o.simple.id);
    let Some(o) = cx.logic.objects.entry(cx.world, h) else {
        return;
    };
    if o.in_damage_callback {
        cx.logic.reports.push(
            GAMEOBJ,
            0x1cf,
            format!(
                "*** ERROR: ID(0x{id:X}): Tried to apply damage of type {} within another damage callback.",
                list.cause
            ),
        );
    }
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.in_damage_callback = true;
    }
}

/// `apply_damage_list`'s test before the destruction: a destroyed list
/// does not destroy an object that is already destroyed, whose damage is
/// blocked, or whose death fuse burns while the cause is below 0x10.
pub fn end_damage_list(cx: &mut Cx<'_>, h: ObjHandle, list: &mut DamageList) {
    if !list.destroyed {
        return;
    }
    let fuse_holds = list.cause < 0x10 && crate::fuses::is_death_fuse_burning(&cx.logic.objects, h);
    let keep =
        fuse_holds
            || cx.logic.objects.get(h).is_some_and(|o| {
                o.destroyed || o.is_damage_blocked(list.cause, list.inflictor_owner)
            });
    if keep {
        list.destroyed = false;
    }
}

/// `set_in_damage_callback(false)`: the latch is cleared and the damage
/// queued meanwhile is taken, to be applied next.
pub fn take_pending_damage(cx: &mut Cx<'_>, h: ObjHandle) -> Option<DamageList> {
    let o = cx.logic.objects.entry(cx.world, h)?;
    o.in_damage_callback = false;
    if o.pending_damage.entries.is_empty() {
        return None;
    }
    // `GameObject_FlushPendingDamage`: a copy is applied; the pending list
    // loses its entries and keeps its flags.
    let pending = o.pending_damage.clone();
    o.pending_damage.entries.clear();
    Some(pending)
}

/// `set_relative_health`: a root entry at that fraction of the maximum
/// hit points is queued (destroying at 0 or below); outside a damage
/// callback it is applied at once. Returns the list to apply now, if any.
pub fn set_relative_health(cx: &mut Cx<'_>, h: ObjHandle, relative: f32) -> Option<DamageList> {
    let max = cx.world.get(h)?.simple.max_hit_pts;
    let o = cx.logic.objects.entry(cx.world, h)?;
    let hit_pts = (f64::from(max) * f64::from(relative)) as f32;
    o.pending_damage
        .add_damage_entry(SUBOBJ_ID_ROOT, hit_pts, Fate::None);
    if relative <= 0.0 {
        o.pending_damage.destroyed = true;
    }
    if o.in_damage_callback {
        return None;
    }
    let pending = o.pending_damage.clone();
    o.pending_damage.entries.clear();
    Some(pending)
}

/// `StarSystem::on_object_destroyed`: a player's current ship ends its
/// trade and its trade request ([`crate::trade::player_terminate_trade`],
/// [`crate::trade::cancel_trade_request`]), its health goes to 0 when destroyed by
/// damage (kind 1), the slot's ship is cleared and `SpaceExit` goes to the
/// bus; then every player of the system hears of the object
/// ([`crate::replication::object_destroyed`]).
fn on_object_destroyed(cx: &mut Cx<'_>, h: ObjHandle, session: u32, id: u32, kind: i32) {
    on_player_object_destroyed(cx, session, id, kind);
    crate::replication::object_destroyed(cx, h, kind);
}

fn on_player_object_destroyed(cx: &mut Cx<'_>, session: u32, id: u32, kind: i32) {
    if session == 0
        || cx
            .logic
            .players
            .get(session)
            .is_none_or(|slot| slot.ship_object_id != id)
    {
        return;
    }
    crate::trade::player_terminate_trade(cx, session);
    crate::trade::cancel_trade_request(cx, session, true);
    if kind == 1 {
        crate::player::set_relative_health(cx, session, 0.0, false);
    }
    let system = match cx.logic.players.get_mut(session) {
        Some(slot) => {
            slot.ship_object_id = 0;
            slot.system_id
        }
        None => 0,
    };
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::SpaceExit {
            system,
            player: session,
        },
    );
}

/// `GameObject::destroy`: the hit points to 0, `on_object_destroyed`, the
/// destroyed latch (`destroy_notify`).
pub fn base_destroy(cx: &mut Cx<'_>, h: ObjHandle, kind: i32) {
    let Some(o) = cx.world.get_mut(h) else {
        return;
    };
    o.simple.hit_pts = 0.0;
    let (session, id) = (o.simple.owner_player, o.simple.id);
    on_object_destroyed(cx, h, session, id, kind);
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.destroyed = true;
    }
}

/// The server's `destroy(kind, killer)` of an object (the world's
/// `destroy` hook). True lets the world mark it dead.
///
/// - `GameObject::destroy` for objects without equipment: the hit points
///   to 0, `on_object_destroyed`, the destroyed latch.
/// - `EqObj::destroy` for ships and solars. The first call (unless a fuse
///   with a `destroy_root` was lit before) runs the death fuse branch once:
///   an NPC destroyed by damage sends `DeathFuseLit`, and a positive
///   answer (the death conversation's length) lights `death_comm` with
///   that lifetime; once it is lit the object loses its behaviour manager
///   (`CEqObj::disconnect`), its queued docks their always-updated entries
///   ([`crate::dock::release_always_update`]), and it stays until the fuse
///   destroys it. The behaviour event 7 comes with the behaviour manager's
///   server link. A player's
///   ship destroyed by damage on a multiplayer server is announced to
///   every session (group notice 0x14). Then the hit points
///   go to 0, `ObjectDestroyed` carries what a destroyed-by-damage object
///   drops (Content may change it), `on_object_destroyed`, the queued
///   docks' release (`destroy_notify`) and the latch follow, and an object destroyed by damage explodes and, unless it is
///   a player's on a multiplayer server, drops its loot.
pub fn destroy(cx: &mut Cx<'_>, h: ObjHandle, kind: i32, killer: u32) -> bool {
    let Some(o) = cx.world.get(h) else {
        return true;
    };
    let equipped = matches!(o.kind, ObjectKind::Ship | ObjectKind::Solar) && o.equipped.is_some();
    let (session, id, system, position) = (
        o.simple.owner_player,
        o.simple.id,
        o.physical.system,
        o.engine.position,
    );
    let is_player = o.as_ship().is_some_and(|s| s.is_player);
    if o.kind == ObjectKind::Loot {
        return crate::loot::destroy(cx, h, kind, killer);
    }
    if matches!(o.kind, ObjectKind::Mine | ObjectKind::CounterMeasure) {
        return crate::munitions::destroy(cx, h, kind, killer);
    }
    // `Solar::destroy`: once `EqObj::destroy` answers true, the system
    // drops the solar (`StarSystem::remove_solar`), which takes it out of
    // the solar BSP.
    let solar_system = (o.kind == ObjectKind::Solar)
        .then_some(o.simple.system)
        .flatten();
    if !equipped {
        base_destroy(cx, h, kind);
        if let Some(system) = solar_system {
            crate::api::system::remove_from_solar_tree(cx, system, h);
        }
        return true;
    }
    let lit = cx
        .logic
        .objects
        .entry(cx.world, h)
        .map(|o| std::mem::replace(&mut o.death_fuse_lit, true));
    if lit == Some(false) {
        if session == 0 {
            if kind == 1 {
                let lifetime = crate::fuses::send_death_fuse_lit(cx, h);
                if lifetime > 0.0
                    && crate::fuses::light_fuse(
                        cx,
                        h,
                        killer,
                        crate::fuses::death_comm_fuse(),
                        game_sim::equip::SUBOBJ_ID_NONE,
                        0.0,
                        lifetime,
                    )
                {
                    // The behaviour event 7 comes with the behaviour
                    // manager's server link.
                    crate::dock::release_always_update(cx, h);
                    cx.world.drop_behavior_manager(h);
                    return false;
                }
            }
        } else if !cx.world.config().single_player && kind == 1 && is_player {
            let sessions: Vec<u32> = cx.logic.players.active_ids().collect();
            for to in sessions {
                let client = cx.logic.players.get(to).map_or(0, |p| p.client_id);
                crate::group::send_notice(cx, client, 0x14, session);
            }
            // `++GServerStats.Field_0x10` (`server_gameobject.cpp:1150`).
            cx.logic.stats.player_deaths += 1;
        }
    }
    if let Some(o) = cx.world.get_mut(h) {
        o.simple.hit_pts = 0.0;
    }
    let dropped = if kind == 1 {
        // `Ship::collect_droppable_equip`: a ship's queued jettison too.
        let mut dropped = cx.world.droppable_equip(h);
        dropped.extend(crate::cargo::queued_jettison(cx, h));
        dropped
    } else {
        Vec::new()
    };
    let mut message = ControllerMessage::ObjectDestroyed(ObjectDestroyed {
        id,
        killer,
        system,
        owner_player: session,
        kind,
        position,
        dropped_equip: dropped,
    });
    let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
    let dropped = match message {
        ControllerMessage::ObjectDestroyed(e) => e.dropped_equip,
        _ => Vec::new(),
    };
    on_object_destroyed(cx, h, session, id, kind);
    // `EqObj::destroy_notify`: the queued docks' always-updated entries.
    crate::dock::release_always_update(cx, h);
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.destroyed = true;
    }
    if kind == 1 {
        let mut hooks = crate::world::Hooks {
            logic: &mut *cx.logic,
        };
        cx.world.explode_object_hooked(h, &mut hooks);
        if cx.world.config().single_player || session == 0 {
            let spawns = cx.world.loot_drops(h, &dropped);
            create_loot(cx, system, id, &spawns);
        }
    }
    if let Some(system) = solar_system {
        crate::api::system::remove_from_solar_tree(cx, system, h);
    }
    true
}

/// `StarSystem::create_loot` (`Server/server_classes.cpp:9285`) in mode 2,
/// the mode of the destruction drops and the loot splits.
pub fn create_loot(
    cx: &mut Cx<'_>,
    system: u32,
    source: u32,
    spawns: &[game_sim::object::world::LootSpawn],
) -> Vec<ObjHandle> {
    create_loot_mode(cx, system, source, 2, spawns)
}

/// `StarSystem::create_loot`: each container made in the system
/// (`CLoot::init`, owned by the source), with its server entry, then
/// `LootSpawned` with the contents' good and count; then every player of
/// the system gets the containers' create with the source and the mode
/// ([`crate::replication::loot_created`]). Returns the containers.
pub fn create_loot_mode(
    cx: &mut Cx<'_>,
    system: u32,
    source: u32,
    mode: u32,
    spawns: &[game_sim::object::world::LootSpawn],
) -> Vec<ObjHandle> {
    create_loot_marked(cx, system, source, mode, spawns, &[])
}

/// [`create_loot_mode`] with each record's player to mark the container
/// for (`FLootRecord::Field_0x18`, by index; a missing one is 0): a
/// player whose low 30 bits are not 0 marks it (`mark_for_player(_, 1)`)
/// before `LootSpawned`.
pub fn create_loot_marked(
    cx: &mut Cx<'_>,
    system: u32,
    source: u32,
    mode: u32,
    spawns: &[game_sim::object::world::LootSpawn],
    marks: &[u32],
) -> Vec<ObjHandle> {
    let mut made = Vec::new();
    for (index, spawn) in spawns.iter().enumerate() {
        let Ok(h) = cx.world.spawn_loot(system, spawn) else {
            continue;
        };
        let entry = cx.logic.objects.entry(cx.world, h);
        let mark = marks.get(index).copied().unwrap_or(0);
        if mark & 0x3fff_ffff != 0
            && let Some(entry) = entry
        {
            entry.mark_for_player(mark, 1);
        }
        // `enter_system(this, nullptr)`'s remote correction.
        crate::replication::begin_remote_control(cx, h);
        let loot = cx.world.get(h).map_or(0, |o| o.simple.id);
        let good = cx.world.data().goods.arch_to_good(spawn.contents);
        let _ = cx.dispatch(
            ControllerId::BROADCAST,
            &mut ControllerMessage::LootSpawned {
                loot,
                source,
                good,
                count: spawn.units as i32,
            },
        );
        made.push(h);
    }
    crate::replication::loot_created(cx, system, source, mode, &made);
    made
}
