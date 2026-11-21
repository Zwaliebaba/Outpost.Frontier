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
//! The per-kind wrappers (`EqObj`, `Ship`, `Loot`, `Mine`, the munitions)
//! and their state arrive with the later parts of the object work.

use std::collections::BTreeMap;

use game_sim::equip::{Fate, SUBOBJ_ID_ROOT};
use game_sim::object::damage::DamageList;
use game_sim::object::world::DamageState;
use game_sim::object::{ObjHandle, World};
use neuron_server::controller::ControllerId;

use crate::events::ControllerMessage;
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
        death_fuse_burning: false,
    }
}

/// `GameObject::apply_damage_list`'s head: a list applied inside another's
/// callback is reported (and applied all the same), then the callback
/// latch is set.
pub fn begin_damage_list(cx: &mut Cx<'_>, h: ObjHandle, list: &DamageList) {
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
/// does not destroy an object that is already destroyed or whose damage
/// is blocked.
pub fn end_damage_list(cx: &mut Cx<'_>, h: ObjHandle, list: &mut DamageList) {
    if !list.destroyed {
        return;
    }
    let keep = cx
        .logic
        .objects
        .get(h)
        .is_some_and(|o| o.destroyed || o.is_damage_blocked(list.cause, list.inflictor_owner));
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

/// `GameObject::destroy`'s server part: the hit points to 0,
/// `StarSystem::on_object_destroyed`'s player part (a player's current
/// ship: health 0 when destroyed by damage, kind 1, the ship id cleared,
/// `SpaceExit`), and the destroyed latch (`destroy_notify`). The
/// observers' `OnObjectDestroyed` comes with replication and the trade
/// ends with the trade work.
pub fn destroy(cx: &mut Cx<'_>, h: ObjHandle, kind: i32) {
    let Some(o) = cx.world.get_mut(h) else {
        return;
    };
    o.simple.hit_pts = 0.0;
    let (session, id) = (o.simple.owner_player, o.simple.id);
    if session != 0
        && let Some(slot) = cx.logic.players.get(session)
        && slot.ship_object_id == id
    {
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
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.destroyed = true;
    }
}
