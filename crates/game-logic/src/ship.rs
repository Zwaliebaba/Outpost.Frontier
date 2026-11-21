//! The server's ship (work package P3.3, decision record 0007, point 1):
//! the `Ship` wrapper (`Server/server_gameobject.cpp:4828-5520`), the
//! server equipment of `EqObj` (`ServerEquip` and its kinds,
//! `rebuild_server_equip`, `:2722`, `:4219`), `EqObj::update`'s server
//! part (`:1019`) and `StarSystem::create_ship`
//! (`Server/server_classes.cpp:7163`).
//!
//! - **Creation** ([`create_ship`]): the ship is made in the shared world
//!   (`CShip::init` through game-sim's `spawn_ship`) or, for a player's
//!   ship in single player, found there (record 0007 point 8); its server
//!   entry gets the `Ship` state, the player's mark, and
//!   `set_equip_state`, which checks its fuses (`EqObj::check_fuses`, with
//!   the server equipment rebuilt first). `pub::SpaceObj::Create` builds
//!   the request ([`crate::api::space_obj::create`]).
//! - **The server equipment** ([`ServerEquip`]): one wrapper per gun,
//!   counter-measure dropper, mine dropper, thruster, repair droid and
//!   tractor, in the equipment's traversal order. A launcher's wrapper
//!   keeps the time of its last damage; a repair droid's repairs the
//!   object's most damaged item or group every 2 s.
//! - **The update** (through the world's `object_update`,
//!   `object_update_flags` and `object_after_update` hooks):
//!   `EqObj::update` (the destroyed gate, the weapons-offline repair, the
//!   server equipment, the fuses), then `Ship::update`: outside a death
//!   sequence the ship's own update (`NO_SCANNER` for an NPC's,
//!   `UPDATE_DUMB` for a player's on a multiplayer server, none for a
//!   player's in single player, which the client side updates) and the
//!   zone damage after it; in a death sequence (a completed dock, set by
//!   the docking part) no own update, and the ship joins the dying-ship
//!   list once it has jumped out (single player) or its timer ran out
//!   (an NPC's on a multiplayer server). The list is announced after the
//!   frame ([`crate::world::physics_update`]).
//!
//! Left to other parts: the dock queue's tick (`process_dock_queue`,
//! docking); the cargo decay (`decay_cargo`, which needs the server's
//! `apply_equip_entry` for a stack); the jettison (`tick_jettison`,
//! `compute_jettison_launch` and the jettison command that fills it, with
//! the cargo commands); the server equipment's firing (`fire`,
//! `fire_forward`, `use_ammo` and the launches, with the weapons'
//! commands) and its equipment notices (a starved thruster switching an
//! NPC's thrusters off, the counter-measure dropper's automatic drop:
//! game-sim queues equipment notices that the C++ delivers at once); the
//! observers' creates of a new ship and `RCBegin` (replication).

use game_sim::equip::{Fate, SUBOBJ_ID_NONE, SUBOBJ_ID_ROOT, SubObjId, types};
use game_sim::object::damage::{self, DamageList};
use game_sim::object::world::ShipSpawn;
use game_sim::object::{ObjHandle, ObjectKind, update_flags};
use neuron_core::id::sub_object::{EquipmentIds, is_arch_group_id, is_equip_id};
use neuron_core::math::{Quaternion, Vector, quaternion_to_matrix};
use neuron_server::controller::ControllerId;

use crate::events::ControllerMessage;
use crate::objects::Invincibility;
use crate::server::Cx;
use crate::world::Hooks;

const SHIP_CPP: &str = "Server\\space\\ship.cpp";

/// The cause of the zone damage's list (8, unnamed in `DamageCause`).
pub const CAUSE_ZONE: damage::DamageCause = 8;
/// The cause of the weapons-offline repair's list.
pub const CAUSE_WEAPONS_OFFLINE_REPAIR: damage::DamageCause = 0x1a;
/// The cause of a repair droid's list.
pub const CAUSE_REPAIR_DROID: damage::DamageCause = 0x11;

/// What a server equipment wrapper is (`create_server_equip`'s switch on
/// the item's type).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ServerEquipKind {
    /// `SrvGun` (0x80).
    Gun(LauncherState),
    /// `SrvCounterMeasureDropper` (0x40).
    CounterMeasureDropper(LauncherState),
    /// `SrvMineDropper` (0x20).
    MineDropper(LauncherState),
    /// `SrvThruster` (0x400).
    Thruster,
    /// `SrvRepairDroid` (0x200000): the time since the last repair and the
    /// sub-object it repairs (`SUBOBJ_ID_NONE` none).
    RepairDroid {
        /// `RepairTimer`.
        timer: f32,
        /// `TargetId`.
        target: SubObjId,
    },
    /// `SrvTractor` (0x100000).
    Tractor,
}

/// `SrvLauncher`'s fields.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LauncherState {
    /// `FireCount`.
    pub fire_count: u32,
    /// `LastDamageTime`: when an equipment entry last hit the launcher
    /// while it had health left.
    pub last_damage_time: f64,
}

/// One `ServerEquip`: the item it listens to and its kind. The C++ forgets
/// the item when it is destroyed (`EquipDestroying`); the port reads the
/// item again by id and treats a destroyed or missing one as forgotten.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ServerEquip {
    /// The item's sub-object id.
    pub equip: SubObjId,
    /// The kind and its state.
    pub kind: ServerEquipKind,
}

impl ServerEquip {
    /// The launcher state of a gun's or dropper's wrapper.
    #[must_use]
    pub fn launcher(&self) -> Option<&LauncherState> {
        match &self.kind {
            ServerEquipKind::Gun(l)
            | ServerEquipKind::CounterMeasureDropper(l)
            | ServerEquipKind::MineDropper(l) => Some(l),
            _ => None,
        }
    }

    fn launcher_mut(&mut self) -> Option<&mut LauncherState> {
        match &mut self.kind {
            ServerEquipKind::Gun(l)
            | ServerEquipKind::CounterMeasureDropper(l)
            | ServerEquipKind::MineDropper(l) => Some(l),
            _ => None,
        }
    }
}

/// `Ship`'s own fields (`Ship::Ship`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ShipState {
    /// `DeathSequenceObjId` (+0x90): the object a completed dock took the
    /// ship into (0 outside a death sequence).
    pub death_sequence_obj_id: u32,
    /// `DeathSequenceTimer` (+0x94).
    pub death_sequence_timer: f32,
    /// +0xb4, `get_data`: the equipment state's data word.
    pub data: u32,
    /// +0xb8: the launch invincibility was granted
    /// ([`apply_launch_invincibility`]).
    pub launch_invincibility: bool,
    /// `AccumulatedDamage` (+0xbc): the zone damage not yet applied.
    pub accumulated_damage: f32,
    /// `AccumulatedDamageTime` (+0xc0): the time the zone damage has run
    /// (never reset).
    pub accumulated_damage_time: f32,
    /// `EquipIds` (+0xc4): the ship's equipment id maker.
    pub equip_ids: EquipmentIds,
    /// The ship's own update ran this frame, so the zone damage follows it
    /// (`Ship::update`'s arm outside a death sequence).
    pub after_update_pending: bool,
}

/// `Ship::FEquipState`: the equipment id maker and the data word a ship
/// takes when it is created.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EquipState {
    /// `EquipIds`.
    pub equip_ids: EquipmentIds,
    /// `Data`.
    pub data: u32,
}

/// What `StarSystem::create_ship` makes a ship from (`FShipCreateRequest`,
/// the `CShipCreateInfo` the callers fill).
#[derive(Clone, Debug, PartialEq)]
pub struct ShipCreateRequest {
    /// `ObjId`.
    pub id: u32,
    /// `ArchetypeNickname`.
    pub archetype: u32,
    /// The loadout (+0x6c, 0 none).
    pub loadout: u32,
    /// `Position`.
    pub position: Vector,
    /// `Orientation`, a quaternion; the ship takes its matrix.
    pub orientation: Quaternion,
    /// `Velocity`.
    pub velocity: Vector,
    /// `AngularVelocity`.
    pub angular_velocity: Vector,
    /// `OwnerPlayer` (0 for an NPC).
    pub owner_player: u32,
    /// `HitPts`, an integer (-1 for the archetype's).
    pub hit_pts: i32,
    /// `Equipment`, after the loadout's.
    pub equipment: Vec<game_sim::data::equip_desc::EquipDesc>,
    /// `VibeGroupId`.
    pub vibe: i32,
    /// The costume.
    pub costume: game_sim::data::costume::Costume,
    /// `VoiceId`.
    pub voice: u32,
    /// `DockingBaseId`.
    pub docking_base: u32,
    /// +0xb0, the ship's `GroupNameId`.
    pub group_name_id: u32,
    /// `ReputationRank`.
    pub rank: u32,
    /// `bUsingTradelane`.
    pub using_tradelane: bool,
    /// `MarkPlayerId`: the player who marks the ship (0 none).
    pub mark_player: u32,
    /// `EquipIds`.
    pub equip_ids: EquipmentIds,
    /// `EquipStateData`.
    pub equip_state_data: u32,
}

/// `a > b`, false when unordered (the C++'s `TEST AH,0x41 / JNZ` skips).
fn above(a: f64, b: f64) -> bool {
    a > b
}

fn is_ship(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world
        .get(h)
        .is_some_and(|o| o.kind == ObjectKind::Ship && o.equipped.is_some())
}

fn is_eq_obj(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world.get(h).is_some_and(|o| {
        matches!(o.kind, ObjectKind::Ship | ObjectKind::Solar) && o.equipped.is_some()
    })
}

/// The ship's `Ship` state, made with the constructor's values on first
/// use; `None` for anything but a ship.
pub fn ship_state<'c>(cx: &'c mut Cx<'_>, h: ObjHandle) -> Option<&'c mut ShipState> {
    if !is_ship(cx, h) {
        return None;
    }
    let o = cx.logic.objects.entry(cx.world, h)?;
    Some(o.ship.get_or_insert_with(ShipState::default))
}

/// `StarSystem::create_ship`: in single player a player's ship is the one
/// the client side made (found by id); any other is made in `system`
/// (`Ship::create`, `CShip::init` with the request's values) and, for a
/// player's, takes the player's group. Then the server entry with its
/// `Ship` state, the mark of `mark_player`, `enter_system` and
/// `set_equip_state` (which checks the ship's fuses). The world puts the
/// ship in the system's list and turns its collisions on unless it is a
/// player's in single player. `None` when no ship is made or found (the
/// C++ reads through the null; D619).
pub fn create_ship(cx: &mut Cx<'_>, system: u32, req: &ShipCreateRequest) -> Option<ObjHandle> {
    let h = if req.owner_player != 0 && cx.world.config().single_player {
        cx.world
            .find(req.id)
            .filter(|&h| cx.world.get(h).is_some_and(|o| o.kind == ObjectKind::Ship))?
    } else {
        let spawn = ShipSpawn {
            velocity: req.velocity,
            angular_velocity: req.angular_velocity,
            owner_player: req.owner_player,
            hit_pts: req.hit_pts as f32,
            id: Some(req.id),
            equipment: req.equipment.clone(),
            vibe: req.vibe,
            costume: req.costume.clone(),
            voice: req.voice,
            docking_base: req.docking_base,
            group_name_id: req.group_name_id,
            using_tradelane: req.using_tradelane,
            rank: req.rank,
            ..ShipSpawn::new(
                req.archetype,
                req.loadout,
                req.position,
                quaternion_to_matrix(&req.orientation),
            )
        };
        let h = cx.world.spawn_ship(system, &spawn).ok()?;
        if req.owner_player != 0 {
            let group = cx.logic.players.get(req.owner_player).and_then(|p| p.group);
            if let Some(group) = group
                && let Some(ship) = cx.world.get_mut(h).and_then(|o| o.as_ship_mut())
            {
                ship.group_id = group;
            }
        }
        h
    };
    ship_state(cx, h)?;
    if req.mark_player & 0x3fff_ffff != 0
        && let Some(o) = cx.logic.objects.entry(cx.world, h)
    {
        o.mark_for_player(req.mark_player, 1);
    }
    // `GameObject::enter_system(this, nullptr)`: the owning system is the
    // world's; a multiplayer player's `RCBegin` is replication's.
    set_equip_state(
        cx,
        h,
        &EquipState {
            equip_ids: req.equip_ids.clone(),
            data: req.equip_state_data,
        },
    );
    Some(h)
}

/// `Ship::set_equip_state`: `EqObj::check_fuses` (the server equipment
/// rebuilt, the fuses lit), then the id maker and the data word.
pub fn set_equip_state(cx: &mut Cx<'_>, h: ObjHandle, state: &EquipState) {
    crate::fuses::check_fuses(cx, h);
    if let Some(s) = ship_state(cx, h) {
        s.equip_ids = state.equip_ids.clone();
        s.data = state.data;
    }
}

/// `Ship::allocate_equip_id`: the next id of the ship's maker. A player's
/// ship may not allocate: that is reported and `SUBOBJ_ID_NONE` answered.
pub fn allocate_equip_id(cx: &mut Cx<'_>, h: ObjHandle) -> SubObjId {
    let owner = cx.world.get(h).map_or(0, |o| o.simple.owner_player);
    if owner != 0 {
        cx.logic.reports.push(
            SHIP_CPP,
            0x1e5,
            "*** ERROR: Ship::allocate_equip_id (): Player tried to allocate id!".to_owned(),
        );
        return SUBOBJ_ID_NONE;
    }
    ship_state(cx, h).map_or(SUBOBJ_ID_NONE, |s| s.equip_ids.create())
}

/// `Ship::apply_launch_invincibility`: once per launch, invincibility of
/// type 1 against everyone with no loss allowed.
pub fn apply_launch_invincibility(cx: &mut Cx<'_>, h: ObjHandle) {
    let Some(s) = ship_state(cx, h) else {
        return;
    };
    if s.launch_invincibility {
        return;
    }
    s.launch_invincibility = true;
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.set_invincible(Invincibility {
            kind: 1,
            protect: true,
            from_players: true,
            max_hp_loss: 0.0,
        });
    }
}

/// `Ship::slot_70` (the launch completed): a granted launch
/// invincibility is lifted (type 1, unprotected, all loss allowed) and
/// `LaunchComplete` goes to the bus with `id`. Answers 0.
pub fn launch_complete(cx: &mut Cx<'_>, h: ObjHandle, id: u32) -> i32 {
    let Some(s) = ship_state(cx, h) else {
        return 0;
    };
    if !s.launch_invincibility {
        return 0;
    }
    s.launch_invincibility = false;
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.set_invincible(Invincibility {
            kind: 1,
            protect: false,
            from_players: false,
            max_hp_loss: 1.0,
        });
    }
    let ship = cx.world.get(h).map_or(0, |o| o.simple.id);
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::LaunchComplete { id, ship },
    );
    0
}

/// `EqObj::rebuild_server_equip`: the wrappers cleared, then one made for
/// each item of the equipment's traversal (`create_server_equip`) whose
/// type has one.
pub fn rebuild_server_equip(cx: &mut Cx<'_>, h: ObjHandle) {
    let items: Vec<(SubObjId, u32)> = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|eq| {
            eq.equipment
                .traverse(types::ALL, false)
                .into_iter()
                .filter_map(|slot| eq.equipment.get(slot))
                .map(|e| (e.id, e.type_bits))
                .collect()
        })
        .unwrap_or_default();
    let Some(o) = cx.logic.objects.entry(cx.world, h) else {
        return;
    };
    o.server_equip.clear();
    for (equip, type_bits) in items {
        let kind = match type_bits {
            types::GUN => ServerEquipKind::Gun(LauncherState::default()),
            types::CM_DROPPER => ServerEquipKind::CounterMeasureDropper(LauncherState::default()),
            types::MINE_DROPPER => ServerEquipKind::MineDropper(LauncherState::default()),
            types::THRUSTER => ServerEquipKind::Thruster,
            types::REPAIR_DROID => ServerEquipKind::RepairDroid {
                timer: 0.0,
                target: SUBOBJ_ID_NONE,
            },
            types::TRACTOR => ServerEquipKind::Tractor,
            _ => continue,
        };
        o.server_equip.push(ServerEquip { equip, kind });
    }
}

/// The item a wrapper listens to, unless it was destroyed or is gone.
fn held<'c>(cx: &'c Cx<'_>, h: ObjHandle, equip: SubObjId) -> Option<&'c game_sim::equip::Equip> {
    cx.world
        .get(h)?
        .equipped
        .as_deref()?
        .equipment
        .by_id(equip)
        .filter(|e| !e.destroyed)
}

/// The time stamps of `EqObj::apply_damage_list` and `apply_equip_entry`
/// for a list about to be applied: the object's last damage while its hull
/// has health left, and the last damage of each launcher an equipment
/// entry hits while it is alive and has health left (on its first
/// wrapper).
pub fn stamp_damage_times(cx: &mut Cx<'_>, h: ObjHandle, list: &DamageList) {
    if !is_eq_obj(cx, h) {
        return;
    }
    let now = cx.world.time();
    let alive = cx
        .world
        .get(h)
        .is_some_and(|o| o.simple.relative_health() > 0.0);
    let hit: Vec<SubObjId> = list
        .entries
        .iter()
        .map(|e| e.sub_obj_id)
        .filter(|&id| id != SUBOBJ_ID_ROOT && is_equip_id(id))
        .filter(|&id| {
            cx.world
                .get(h)
                .and_then(|o| o.equipped.as_deref())
                .and_then(|eq| eq.equipment.by_id(id))
                .is_some_and(|e| {
                    !e.is_destroyed()
                        && e.relative_health() > 0.0
                        && e.type_bits & types::LAUNCHER != 0
                })
        })
        .collect();
    let Some(o) = cx.logic.objects.entry(cx.world, h) else {
        return;
    };
    if alive {
        o.last_damage_time = now;
    }
    for id in hit {
        if let Some(l) = o
            .server_equip
            .iter_mut()
            .find(|s| s.equip == id)
            .and_then(ServerEquip::launcher_mut)
        {
            l.last_damage_time = now;
        }
    }
}

fn apply(cx: &mut Cx<'_>, h: ObjHandle, list: &DamageList) {
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, list, &mut hooks);
}

/// `EqObj::update`: false for a destroyed object (`GameObject::update`);
/// with its weapons offline, a hull without health and undamaged for more
/// than 60 s, and every disabled launcher undamaged for more than 60 s,
/// get their maximum hit points back (cause 0x1a); each server equipment
/// wrapper updates; the fuses burn. False when the object is destroyed
/// by then. The dock queue's tick and the cargo decay are their parts'.
pub fn eq_obj_update(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) -> bool {
    if cx.logic.objects.get(h).is_some_and(|o| o.destroyed) {
        return false;
    }
    weapons_offline_repair(cx, h);
    let count = cx.logic.objects.get(h).map_or(0, |o| o.server_equip.len());
    for index in 0..count {
        let kind = cx
            .logic
            .objects
            .get(h)
            .and_then(|o| o.server_equip.get(index))
            .map(|s| s.kind);
        if let Some(ServerEquipKind::RepairDroid { .. }) = kind {
            repair_droid_update(cx, h, index, dt);
        }
    }
    if cx.logic.objects.get(h).is_some_and(|o| !o.fuses.is_empty()) {
        crate::fuses::burn_fuses(cx, h, dt);
    }
    !cx.logic.objects.get(h).is_some_and(|o| o.destroyed)
}

fn weapons_offline_repair(cx: &mut Cx<'_>, h: ObjHandle) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let Some(eq) = o.equipped.as_deref() else {
        return;
    };
    if !eq.weapons_offline {
        return;
    }
    let now = cx.world.time();
    let Some(entry) = cx.logic.objects.get(h) else {
        return;
    };
    let mut repair = DamageList::new(CAUSE_WEAPONS_OFFLINE_REPAIR, 0, 0);
    if !above(o.simple.relative_health(), 0.0) && now - entry.last_damage_time > 60.0 {
        repair.add_damage_entry(SUBOBJ_ID_ROOT, o.simple.max_hit_pts, Fate::None);
    }
    for s in &entry.server_equip {
        let Some(launcher) = s.launcher() else {
            continue;
        };
        let Some(equip) = eq.equipment.by_id(s.equip).filter(|e| !e.destroyed) else {
            continue;
        };
        if equip.type_bits & types::LAUNCHER == 0 || !equip.is_disabled(eq.weapons_offline) {
            continue;
        }
        if !above(now - launcher.last_damage_time, 60.0) {
            continue;
        }
        repair.add_damage_entry(equip.id, equip.max_hit_points(), Fate::None);
    }
    if !repair.entries.is_empty() {
        apply(cx, h, &repair);
    }
}

/// `CEqObj::get_sub_obj_relative_health`, stored to a float.
fn sub_obj_relative_health(cx: &Cx<'_>, h: ObjHandle, id: SubObjId) -> Option<f32> {
    let o = cx.world.get(h)?;
    let eq = o.equipped.as_deref()?;
    if is_equip_id(id) {
        return eq.equipment.by_id(id).map(|e| e.relative_health() as f32);
    }
    if is_arch_group_id(id) {
        return eq
            .arch_groups
            .iter()
            .find(|g| g.id == id)
            .map(|g| g.relative_health() as f32);
    }
    (id == SUBOBJ_ID_ROOT).then(|| o.simple.relative_health() as f32)
}

/// `CEqObj::get_sub_obj_hit_pts`.
fn sub_obj_hit_pts(cx: &Cx<'_>, h: ObjHandle, id: SubObjId) -> Option<f32> {
    let eq = cx.world.get(h)?.equipped.as_deref()?;
    if is_equip_id(id) {
        return eq
            .equipment
            .by_id(id)
            .map(game_sim::equip::Equip::hit_points);
    }
    if is_arch_group_id(id) {
        return eq
            .arch_groups
            .iter()
            .find(|g| g.id == id)
            .map(|g| g.hit_pts);
    }
    None
}

/// `CEqObj::get_sub_obj_max_hit_pts`, 0 when the lookup fails.
fn sub_obj_max_hit_pts(cx: &Cx<'_>, h: ObjHandle, id: SubObjId) -> f32 {
    let Some(eq) = cx.world.get(h).and_then(|o| o.equipped.as_deref()) else {
        return 0.0;
    };
    if is_equip_id(id) {
        return eq
            .equipment
            .by_id(id)
            .map_or(0.0, game_sim::equip::Equip::max_hit_points);
    }
    if is_arch_group_id(id) {
        return eq
            .arch_groups
            .iter()
            .find(|g| g.id == id)
            .map_or(0.0, |g| g.max_hit_points() as f32);
    }
    0.0
}

/// `SrvRepairDroid::pick_target`: the attached item (shields aside), then
/// the collision group, with the lowest relative health below 1; the first
/// of equal ones.
fn pick_target(cx: &Cx<'_>, h: ObjHandle) -> SubObjId {
    let Some(eq) = cx.world.get(h).and_then(|o| o.equipped.as_deref()) else {
        return SUBOBJ_ID_NONE;
    };
    let mut lowest = 1.0f32;
    let mut best = SUBOBJ_ID_NONE;
    for slot in eq.equipment.traverse(types::ALL, false) {
        let Some(item) = eq.equipment.get(slot) else {
            continue;
        };
        if item.type_bits & types::ATTACHED == 0 || item.type_bits == types::SHIELD {
            continue;
        }
        let relative = item.relative_health();
        if relative < f64::from(lowest) {
            lowest = relative as f32;
            best = item.id;
        }
    }
    let mut groups: Vec<&damage::ArchGroup> = eq.arch_groups.iter().collect();
    groups.sort_by_key(|g| g.id);
    for g in groups {
        let relative = g.relative_health();
        if relative < f64::from(lowest) {
            lowest = relative as f32;
            best = g.id;
        }
    }
    best
}

/// `SrvRepairDroid::update`: an active droid picks the most damaged
/// sub-object when it has none; every 2 s it repairs it by its repair
/// rate times the elapsed time, out of its own hit points (cause 0x11,
/// inflicted by the object), up to the target's maximum, and starts over;
/// a target at full health, or one whose hit points cannot be read, is
/// dropped.
fn repair_droid_update(cx: &mut Cx<'_>, h: ObjHandle, index: usize, dt: f32) {
    let Some(se) = cx
        .logic
        .objects
        .get(h)
        .and_then(|o| o.server_equip.get(index))
        .copied()
    else {
        return;
    };
    let ServerEquipKind::RepairDroid {
        mut timer,
        mut target,
    } = se.kind
    else {
        return;
    };
    let Some(droid) = held(cx, h, se.equip) else {
        return;
    };
    if !droid.is_active() {
        return;
    }
    let rate = match &droid.kind {
        game_sim::equip::EquipKind::RepairDroid(d) => d.repair_rate,
        _ => 0.0,
    };
    let (droid_id, droid_hit_pts) = (droid.id, droid.hit_points());
    let store = |cx: &mut Cx<'_>, timer: f32, target: SubObjId| {
        if let Some(s) = cx
            .logic
            .objects
            .entry_mut(h)
            .and_then(|o| o.server_equip.get_mut(index))
        {
            s.kind = ServerEquipKind::RepairDroid { timer, target };
        }
    };
    if target == SUBOBJ_ID_NONE {
        target = pick_target(cx, h);
    }
    if target == SUBOBJ_ID_NONE {
        store(cx, timer, target);
        return;
    }
    timer = (f64::from(dt) + f64::from(timer)) as f32;
    if !above(f64::from(timer), 2.0) {
        store(cx, timer, target);
        return;
    }
    let relative = sub_obj_relative_health(cx, h, target);
    let Some(relative) = relative.filter(|r| *r < 1.0) else {
        store(cx, 0.0, SUBOBJ_ID_NONE);
        return;
    };
    let (id, owner) = cx
        .world
        .get(h)
        .map_or((0, 0), |o| (o.simple.id, o.simple.owner_player));
    let mut repair = DamageList::new(CAUSE_REPAIR_DROID, id, owner);
    if let Some(current) = sub_obj_hit_pts(cx, h, target) {
        let amount = (f64::from(rate) * f64::from(timer)) as f32;
        let left = f64::from(droid_hit_pts) - f64::from(amount);
        let droid_left = if left > 0.0 { left as f32 } else { 0.0 };
        repair.add_damage_entry(droid_id, droid_left, Fate::None);
        let max = if relative > 0.0 {
            (f64::from(current) / f64::from(relative)) as f32
        } else {
            sub_obj_max_hit_pts(cx, h, target)
        };
        let repaired = f64::from(amount) + f64::from(current);
        let new = if repaired < f64::from(max) {
            repaired as f32
        } else {
            max
        };
        repair.add_damage_entry(target, new, Fate::None);
        store(cx, 0.0, target);
        apply(cx, h, &repair);
    } else {
        store(cx, 0.0, SUBOBJ_ID_NONE);
    }
}

/// The `object_update` hook for the equipped objects: `EqObj::update`,
/// then for a ship `Ship::update`'s choice. True lets the world run the
/// object's own update.
pub fn object_update(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) -> bool {
    if !is_eq_obj(cx, h) {
        return true;
    }
    let ship = is_ship(cx, h);
    if !eq_obj_update(cx, h, dt) {
        if ship && let Some(s) = ship_state(cx, h) {
            s.after_update_pending = false;
        }
        return false;
    }
    if !ship {
        return true;
    }
    let single = cx.world.config().single_player;
    let owner = cx.world.get(h).map_or(0, |o| o.simple.owner_player);
    let Some(s) = ship_state(cx, h) else {
        return true;
    };
    if s.death_sequence_obj_id == 0 {
        s.after_update_pending = true;
        return !(owner != 0 && single);
    }
    s.after_update_pending = false;
    death_sequence(cx, h, dt);
    false
}

/// `Ship::update`'s death-sequence arm: in single player the ship joins
/// the dying-ship list once it has jumped out; on a multiplayer server an
/// NPC's ship counts its timer down and, when it runs out, gets its
/// collisions back and joins the list. Joining ends the sequence.
fn death_sequence(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) {
    let (jumped_out, is_player) = cx
        .world
        .get(h)
        .and_then(|o| o.as_ship())
        .map_or((false, false), |s| (s.jumped_out, s.is_player));
    if cx.world.config().single_player {
        if !jumped_out {
            return;
        }
        cx.logic.server_world.dying_ships.push(h);
        if let Some(s) = ship_state(cx, h) {
            s.death_sequence_obj_id = 0;
        }
        return;
    }
    if is_player {
        return;
    }
    let Some(s) = ship_state(cx, h) else {
        return;
    };
    s.death_sequence_timer = (f64::from(s.death_sequence_timer) - f64::from(dt)) as f32;
    if s.death_sequence_timer <= 0.0 {
        s.death_sequence_timer = 0.0;
        s.death_sequence_obj_id = 0;
        cx.world.set_collisions(h, true);
        cx.logic.server_world.dying_ships.push(h);
    }
}

/// The flags of a ship's own update: `NO_SCANNER` for an NPC's,
/// `UPDATE_DUMB` for a player's (which runs only on a multiplayer server);
/// `UPDATE_DEFAULT` (0) for anything else.
#[must_use]
pub fn update_flags(world: &game_sim::object::World, h: ObjHandle) -> u32 {
    match world.get(h) {
        Some(o) if o.kind == ObjectKind::Ship && o.equipped.is_some() => {
            if o.simple.owner_player == 0 {
                update_flags::NO_SCANNER
            } else {
                update_flags::UPDATE_DUMB
            }
        }
        _ => 0,
    }
}

/// The `object_after_update` hook for a ship: after its own update (or
/// in its place for a player's in single player), the zone damage. The
/// jettison before it is the cargo commands' part.
pub fn after_update(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) {
    let Some(s) = ship_state(cx, h) else {
        return;
    };
    if !std::mem::take(&mut s.after_update_pending) {
        return;
    }
    tick_zone_damage(cx, h, dt);
}

/// `Ship::tick_zone_damage`: unless the ship is invincible, the damage
/// rate of the zone it is in times its immersion and the time accumulates;
/// once the zone damage has run for more than 1 s, an accumulation of more
/// than 1 % of the ship's total maximum hit points is applied as that
/// fraction of each part's maximum (cause 8) and starts over.
pub fn tick_zone_damage(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) {
    if cx.logic.objects.get(h).is_some_and(|o| o.invincible) {
        return;
    }
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let rate = match (o.simple.zone, o.simple.system) {
        (Some(zone), Some(system)) => cx
            .world
            .data()
            .universe
            .system_zones(system)
            .find(|z| z.nickname == zone)
            .map(|z| z.damage),
        _ => None,
    };
    let immersion = o.simple.scanner_interference;
    let total = o.total_max_hit_pts();
    let Some(s) = ship_state(cx, h) else {
        return;
    };
    if let Some(rate) = rate
        && rate > 0.0
    {
        s.accumulated_damage = (f64::from(rate) * f64::from(immersion) * f64::from(dt)
            + f64::from(s.accumulated_damage)) as f32;
    }
    s.accumulated_damage_time = (f64::from(dt) + f64::from(s.accumulated_damage_time)) as f32;
    if s.accumulated_damage == 0.0 || s.accumulated_damage_time <= 1.0 {
        return;
    }
    let ratio = (f64::from(s.accumulated_damage) / f64::from(total)) as f32;
    if ratio <= 0.01 {
        return;
    }
    let mut list = DamageList::new(CAUSE_ZONE, 0, 0);
    apply_damage_ratio(cx, h, ratio, &mut list);
    apply(cx, h, &list);
    if let Some(s) = ship_state(cx, h) {
        s.accumulated_damage = 0.0;
    }
}

/// `Ship::apply_damage_ratio`: above 1 the whole hull and the list
/// destroys; otherwise the fraction of the hull's maximum, then, while the
/// list does not destroy, of each item of the mask `0x1ee0` and of each
/// collision group. True when any part is destroyed.
pub fn apply_damage_ratio(cx: &Cx<'_>, h: ObjHandle, ratio: f32, list: &mut DamageList) -> bool {
    let Some(o) = cx.world.get(h) else {
        return false;
    };
    let (hit_pts, max) = (o.simple.hit_pts, o.simple.max_hit_pts);
    if ratio > 1.0 {
        crate::fuses::compute(cx, h, list, |t, _, _, list| {
            damage::damage_hull(t, hit_pts, list);
        });
        list.destroyed = true;
        return true;
    }
    let mut any = false;
    let hull = (f64::from(max) * f64::from(ratio)) as f32;
    crate::fuses::compute(cx, h, list, |t, _, _, list| {
        any |= damage::damage_hull(t, hull, list);
    });
    let Some(eq) = o.equipped.as_deref() else {
        return any;
    };
    if !list.destroyed {
        let items: Vec<(SubObjId, f32)> = eq
            .equipment
            .traverse(0x1ee0, false)
            .into_iter()
            .filter_map(|slot| eq.equipment.get(slot))
            .map(|e| {
                (
                    e.id,
                    (f64::from(e.max_hit_points()) * f64::from(ratio)) as f32,
                )
            })
            .collect();
        crate::fuses::compute(cx, h, list, |t, _, _, list| {
            for (id, amount) in items {
                any |= damage::damage_equip(t, id, amount, list);
            }
        });
    }
    if !list.destroyed {
        let mut groups: Vec<(SubObjId, f32)> = eq
            .arch_groups
            .iter()
            .map(|g| (g.id, (g.max_hit_points() * f64::from(ratio)) as f32))
            .collect();
        groups.sort_by_key(|g| g.0);
        crate::fuses::compute(cx, h, list, |t, in_group, mount_of, list| {
            for (id, amount) in groups {
                any |= damage::damage_group(t, id, amount, list, in_group, mount_of);
            }
        });
    }
    any
}
