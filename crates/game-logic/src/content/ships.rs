//! Content's ships (`FContentShipBase`, TU50, `Content/BehaviorCmds.h:66`;
//! work package P3.6a): the ship base that the player ship
//! (`FContentPlayerShip`, TU46) and the NPC ship (`FContentNpcShip`, TU43)
//! share, as the dock queues, the formations and the loot preparation read
//! it.
//!
//! A ship is named by a [`ShipKey`]: the players' ship objects
//! ([`PlayerShip`](super::players::PlayerShip)) and, with P3.6c, the NPC
//! ships of Content's arena ([`super::npc_ships`]), which embed the same
//! TU50 base. A system record's space objects (`FSystemInfo::SpaceObjs`)
//! are its NPC ships, and vtable slot 1 (`Slot04`) is true for those.

use neuron_core::math::Vector;

use super::ContentState;
use super::players::{self, PlayerShip};
use crate::api;
use crate::events::NpcShipKey;
use crate::server::Cx;

/// A Content ship: the ship object of a player (by the player's id) or an
/// NPC ship of Content's arena.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShipKey {
    /// `FContentPlayerState::ShipObject` of this player.
    Player(u32),
    /// An NPC ship (`FContentNpcShip`).
    Npc(NpcShipKey),
}

/// The ship base of `key`, `None` when it is gone.
#[must_use]
pub fn base(state: &ContentState, key: ShipKey) -> Option<&PlayerShip> {
    match key {
        ShipKey::Player(player) => state.players.get(&player)?.ship.as_ref(),
        ShipKey::Npc(npc) => state.npc_ships.get(&npc).map(|s| &s.base),
    }
}

/// The ship base of `key`, to change.
pub fn base_mut(state: &mut ContentState, key: ShipKey) -> Option<&mut PlayerShip> {
    match key {
        ShipKey::Player(player) => state.players.get_mut(&player)?.ship.as_mut(),
        ShipKey::Npc(npc) => state.npc_ships.get_mut(&npc).map(|s| &mut s.base),
    }
}

/// `SpaceObjId` (TU20 `+0x04`); 0 for a ship that is gone.
#[must_use]
pub fn id(state: &ContentState, key: ShipKey) -> u32 {
    base(state, key).map_or(0, |s| s.ship)
}

/// `FormationId` (`+0x154`); -1 for a ship that is gone.
#[must_use]
pub fn formation_id(state: &ContentState, key: ShipKey) -> u32 {
    base(state, key).map_or(u32::MAX, |s| s.formation_id)
}

/// `FormationSlot` (`+0x14c`); -1 for a ship that is gone.
#[must_use]
pub fn formation_slot(state: &ContentState, key: ShipKey) -> u32 {
    base(state, key).map_or(u32::MAX, |s| s.formation_slot)
}

/// `Formation` (`+0x150`), by nickname id.
#[must_use]
pub fn formation(state: &ContentState, key: ShipKey) -> Option<u32> {
    base(state, key).and_then(|s| s.formation)
}

/// A wingman: `FormationSlot != 0 && FormationId != -1`.
#[must_use]
pub fn is_wingman(state: &ContentState, key: ShipKey) -> bool {
    formation_slot(state, key) != 0 && formation_id(state, key) != u32::MAX
}

/// `SetFormation(Id, Slot, Formation)` (`:202-208`).
pub fn set_formation(
    state: &mut ContentState,
    key: ShipKey,
    formation_id: u32,
    slot: u32,
    formation: Option<u32>,
) {
    if let Some(ship) = base_mut(state, key) {
        ship.formation_id = formation_id;
        ship.formation_slot = slot;
        ship.formation = formation;
    }
}

/// Vtable slot 1 (`Slot04`): true for an NPC ship, false for a player's.
#[must_use]
pub fn is_npc(key: ShipKey) -> bool {
    matches!(key, ShipKey::Npc(_))
}

/// `ReputationHandle->GetReputation()`: a player ship's handle is its
/// player state, so the player's vibe, an NPC ship's its reputation
/// handle's; -1 for a ship that is gone.
#[must_use]
pub fn reputation(state: &ContentState, key: ShipKey) -> i32 {
    match key {
        ShipKey::Player(player) => state.players.get(&player).map_or(-1, |p| p.reputation),
        ShipKey::Npc(npc) => state
            .npc_ships
            .get(&npc)
            .map_or(-1, |s| state.rep_handles.vibe(s.reputation)),
    }
}

/// `ControllerId` (`+0x170`): the owning controller; 0 for a player's ship.
#[must_use]
pub fn controller(state: &ContentState, key: ShipKey) -> u32 {
    match key {
        ShipKey::Player(_) => 0,
        ShipKey::Npc(npc) => state.npc_ships.get(&npc).map_or(0, |s| s.controller),
    }
}

/// `GetSystem()->SystemId` (TU20 `+0xdc`): the ship's system record.
#[must_use]
pub fn system(state: &ContentState, key: ShipKey) -> Option<u32> {
    base(state, key).and_then(|s| s.system)
}

/// `FContentSpaceObj::GetPosition` (`Content/NpcBase.cpp:125-131`):
/// `pub::SpaceObj::GetLocation`'s position; zero when the call fails,
/// where the C++ copies its uninitialised local (D841).
#[must_use]
pub fn position(cx: &Cx<'_>, key: ShipKey) -> Vector {
    let ship = id(&cx.logic.content.state, key);
    api::space_obj::location(cx, ship).map_or(Vector::ZERO, |(p, _)| p)
}

/// `HasClassId` (`:162-170`).
#[must_use]
pub fn has_class_id(state: &ContentState, key: ShipKey, class: u32) -> bool {
    base(state, key).is_some_and(|s| s.class_ids.contains(&class))
}

/// `IsInShipClass` (`:174-185`): a member of the TU51 ship class among the
/// ship's class ids; false for an unknown class.
#[must_use]
pub fn is_in_ship_class(cx: &Cx<'_>, key: ShipKey, ship_class: u32) -> bool {
    let tables = &cx.logic.content.tables;
    let Some(members) = tables.ship_class(ship_class) else {
        return false;
    };
    let state = &cx.logic.content.state;
    members.iter().any(|&m| has_class_id(state, key, m))
}

/// `GetReputationRank` (`:90-96`): `pub::Reputation::GetRank` of the
/// ship's reputation, truncated; the C++'s uninitialised rank on a failed
/// call reads 0 (D841).
pub fn reputation_rank(cx: &mut Cx<'_>, key: ShipKey) -> i32 {
    let rep = reputation(&cx.logic.content.state, key);
    let rank = api::reputation::rank(cx, rep).unwrap_or(0.0);
    neuron_core::num::trunc_to_i32(f64::from(rank))
}

/// `FContentSpaceObj::GetAffiliation` (`Content/NpcBase.h:126`):
/// `pub::Reputation::GetAffiliation` of the ship's reputation, -1 when it
/// fails.
pub fn affiliation(cx: &mut Cx<'_>, key: ShipKey) -> u32 {
    let rep = reputation(&cx.logic.content.state, key);
    api::reputation::affiliation(cx, rep).unwrap_or(u32::MAX)
}

/// `AddDamage(&Attacker, Amount)` (`:119-133`): an attacker whose id's low
/// 30 bits are 0 is ignored; a known one adds (`float + float`, exact in
/// `f64` and stored), a new one is appended.
pub fn add_damage(state: &mut ContentState, key: ShipKey, attacker: u32, amount: f32) {
    if attacker & 0x3fff_ffff == 0 {
        return;
    }
    let Some(ship) = base_mut(state, key) else {
        return;
    };
    if let Some(entry) = ship.damage.iter_mut().find(|(id, _)| *id == attacker) {
        entry.1 = (f64::from(amount) + f64::from(entry.1)) as f32;
    } else {
        ship.damage.push((attacker, amount));
    }
}

/// `GetTopDamagers` (`:138-159`): the attacker that dealt the most, and the
/// player's ship that did; the last entry at or above the maximum so far
/// wins (a NaN never does). 0 for none.
#[must_use]
pub fn top_damagers(state: &ContentState, key: ShipKey) -> (u32, u32) {
    let (mut top_player, mut top_any) = (0, 0);
    let (mut max_player, mut max_any) = (0.0_f32, 0.0_f32);
    let Some(ship) = base(state, key) else {
        return (0, 0);
    };
    for &(attacker, amount) in &ship.damage {
        if players::find_by_ship(&state.players, attacker).is_some() && amount >= max_player {
            max_player = amount;
            top_player = attacker;
        }
        if amount >= max_any {
            max_any = amount;
            top_any = attacker;
        }
    }
    (top_player, top_any)
}

/// `FSystemInfo::FindShip(Id)` (`Content/SystemInfo.cpp:560-574`): the ship
/// of a player in the record's players, then the record's space objects
/// (its NPC ships). `None` without the record.
#[must_use]
pub fn find_in_system(cx: &Cx<'_>, system: u32, ship: u32) -> Option<ShipKey> {
    let record = cx.logic.content.state.systems.get(&system)?;
    if let Some(p) = super::systems::find_player_ship(cx, record, ship) {
        return Some(ShipKey::Player(p));
    }
    find_space_obj(&cx.logic.content.state, system, ship).map(ShipKey::Npc)
}

/// `FSystemInfo::FindSpaceObj(Id)` (`SystemInfo.cpp:592-601`): the first of
/// the record's NPC ships with the space object id.
#[must_use]
pub fn find_space_obj(state: &ContentState, system: u32, ship: u32) -> Option<NpcShipKey> {
    let record = state.systems.get(&system)?;
    record
        .space_objs
        .iter()
        .copied()
        .find(|k| state.npc_ships.get(k).is_some_and(|s| s.base.ship == ship))
}

/// `FContentShipBase::GetFormationLeader` (`:192-199`): a wingman's
/// formation's first member, else the ship itself. A wingman whose
/// formation has no member, whose front the C++ reads unchecked, is its
/// own leader (D840).
#[must_use]
pub fn formation_leader(state: &ContentState, key: ShipKey) -> ShipKey {
    if !is_wingman(state, key) {
        return key;
    }
    super::formations::collect_members(state, formation_id(state, key))
        .first()
        .copied()
        .unwrap_or(key)
}
