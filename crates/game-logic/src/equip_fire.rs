//! The server equipment's fire and launches (work package P3.3, the object
//! part): `EqObj::slot_34` (`IObjAffect` +0x34, the fire request), the
//! launcher wrappers' `fire`, `fire_forward`, `fire_unchecked`, `use_ammo`
//! and launches (`SrvGun`, `SrvMineDropper`, `SrvCounterMeasureDropper`,
//! `SrvLauncher`, `Server/server_gameobject.cpp:4457-4846`),
//! `StarSystem::create_bullet` and `create_guided`
//! (`Server/server_classes.cpp:9608`, `9725`), the wrappers' equipment
//! notices (`SrvThruster::notify`, `SrvCounterMeasureDropper::notify`) with
//! `Ship::slot_54` and `EqObj::slot_4c`, and the server's part of an AI's
//! shot.
//!
//! - **A request** ([`fire_weapons`]): each listed item's wrapper fires. A
//!   client's shot (`SourceClient` other than 0 and -1) is
//!   `fire_unchecked`: the ammunition is paid for (outside single player,
//!   where it must be there) and only a seeking munition is launched; the
//!   client makes its own bullets. The server's own shot fires at the aim
//!   point (`fire`: `CELauncher::Fire`, `ConsumeFireResources`, then the
//!   launch: missiles, bullets outside single player, mines, counter-
//!   measures) or, without one or for a projectile with `force_gun_ori`,
//!   forward (`fire_forward`: the barrels' average position plus their
//!   average direction scaled to 1e10). The unguided guns that fired are
//!   announced together.
//! - **The launches** go through the system's creators: [`create_bullet`],
//!   [`create_guided`] (with the players' creates),
//!   [`crate::munitions::create_mine_from`] and
//!   [`crate::munitions::create_counter_measure_from`].
//! - **The notices** ([`equip_event`], the world's `equip_event` hook, as
//!   the item raises them): an NPC's starved thruster switches the ship's
//!   thrusters off ([`set_thrusters_active`]); an NPC's counter-measure
//!   dropper that sees a missile aimed at its owner drops one
//!   (`slot_34(&id, 1, null, -1)`).
//! - **An AI's shot** ([`ai_fired`]): the world fires inside the behaviour
//!   tick (`World::fire`); after the tick the server counts the shots,
//!   wraps the missiles and tells the players of them, and makes the
//!   fire notices.
//!
//! The observers' notices (`notify_fire_weapon`, `notify_thrusters`,
//! `notify_activate_equip`) are replication's; their call sites are the
//! functions at the end of this module, which do nothing yet.

use game_sim::equip::launcher::{FireResult, ProjLaunchInfo};
use game_sim::equip::{EquipEvent, EquipKind, SUBOBJ_ID_NONE, SubObjId, types};
use game_sim::object::world::ProjectileSpawn;
use game_sim::object::{Command, KindData, ObjHandle};
use neuron_core::math::{Transform, Vector, inverse_transform};

use crate::server::Cx;
use crate::ship::ServerEquipKind;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `OwnerSystem`: the system the object is in (`None` in transit).
fn owner_system(cx: &Cx<'_>, h: ObjHandle) -> Option<u32> {
    cx.world.get(h)?.simple.system
}

/// `GameObject::is_destroyed`: the server's latch (or an object the world
/// no longer has).
fn is_destroyed(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.logic.objects.get(h).is_some_and(|o| o.destroyed) || cx.world.get(h).is_none_or(|o| o.dead)
}

/// The wrapper `EqObj_FindServerEquip` finds for an item.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Listener {
    /// No wrapper listens to it.
    None,
    /// A wrapper whose item is gone (`Equip == null`), which a missing item
    /// matches.
    Forgotten,
    /// The wrapper at this index of the object's list.
    Live(usize),
}

/// A live (not destroyed) item of the object.
fn live_item(cx: &Cx<'_>, h: ObjHandle, id: SubObjId) -> bool {
    cx.world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .and_then(|eq| eq.equipment.by_id(id))
        .is_some_and(|e| !e.destroyed)
}

/// `EqObj_FindServerEquip(FindByID(id))`: the wrapper of a live item; a
/// destroyed item has none (its wrapper forgot it); a missing item matches
/// the first forgotten wrapper.
fn find_listener(cx: &Cx<'_>, h: ObjHandle, id: SubObjId) -> Listener {
    let Some(entry) = cx.logic.objects.get(h) else {
        return Listener::None;
    };
    let exists = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .is_some_and(|eq| eq.equipment.by_id(id).is_some());
    if exists {
        if !live_item(cx, h, id) {
            return Listener::None;
        }
        return entry
            .server_equip
            .iter()
            .position(|s| s.equip == id)
            .map_or(Listener::None, Listener::Live);
    }
    if entry
        .server_equip
        .iter()
        .any(|s| !live_item(cx, h, s.equip))
    {
        Listener::Forgotten
    } else {
        Listener::None
    }
}

/// The kind of the wrapper at `index`.
fn wrapper_kind(cx: &Cx<'_>, h: ObjHandle, index: usize) -> Option<(SubObjId, ServerEquipKind)> {
    let s = cx.logic.objects.get(h)?.server_equip.get(index)?;
    Some((s.equip, s.kind))
}

/// `++FireCount` of the launcher wrapper at `index`.
fn count_shot(cx: &mut Cx<'_>, h: ObjHandle, index: usize) {
    if let Some(l) = cx
        .logic
        .objects
        .entry_mut(h)
        .and_then(|o| o.server_equip.get_mut(index))
        .and_then(crate::ship::ServerEquip::launcher_mut)
    {
        l.fire_count = l.fire_count.wrapping_add(1);
    }
}

/// The projectile of a gun seeks (`MunitionArch()->Seeker != 0`); a gun
/// without a projectile archetype counts as unguided.
fn seeks(cx: &Cx<'_>, h: ObjHandle, id: SubObjId) -> bool {
    cx.world
        .launcher_projectile(h, id)
        .is_some_and(|p| p.seeker != 0)
}

/// `EqObj::slot_34(EquipIds, Count, TargetPos, SourceClient)`: -5 for an
/// object outside a system or destroyed. Each item's wrapper fires; an item
/// without one makes the answer -2 (a missing item matched by a forgotten
/// wrapper is skipped quietly). A client's shot (`SourceClient` other than
/// 0 and -1) is `fire_unchecked` at the aim point; the server's fires at
/// the aim point (`fire`) unless there is none or the projectile has
/// `force_gun_ori`, when it fires forward (`fire_forward`, which makes its
/// own notice). The unguided guns that fired, with the first one's lock
/// target, are announced in one `notify_fire_weapon`, the aim point turned
/// into the lock target's frame outside single player. A wrapper that is
/// no launcher, and a client's shot without an aim point, fire nothing
/// (D650).
pub fn fire_weapons(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    equip_ids: &[SubObjId],
    target: Option<Vector>,
    source_client: i32,
) -> i32 {
    let Some(system) = owner_system(cx, h) else {
        return -5;
    };
    if is_destroyed(cx, h) {
        return -5;
    }
    let mut fired_ids: Vec<SubObjId> = Vec::new();
    let mut result = 0;
    let mut lock: Option<ObjHandle> = None;
    for &id in equip_ids {
        let index = match find_listener(cx, h, id) {
            Listener::None => {
                result = -2;
                continue;
            }
            Listener::Forgotten => continue,
            Listener::Live(index) => index,
        };
        let Some((_, kind)) = wrapper_kind(cx, h, index) else {
            continue;
        };
        if !matches!(
            kind,
            ServerEquipKind::Gun(_)
                | ServerEquipKind::MineDropper(_)
                | ServerEquipKind::CounterMeasureDropper(_)
        ) {
            continue;
        }
        let fired = if source_client != -1 && source_client != 0 {
            let Some(t) = target else {
                continue;
            };
            fire_unchecked(cx, h, index, t)
        } else {
            let force = cx
                .world
                .launcher_projectile(h, id)
                .is_some_and(|p| p.force_gun_ori);
            match target {
                Some(t) if !force => fire(cx, h, index, t),
                _ => {
                    fire_forward(cx, h, index, source_client);
                    continue;
                }
            }
        };
        if !fired || !matches!(kind, ServerEquipKind::Gun(_)) || seeks(cx, h, id) {
            continue;
        }
        fired_ids.push(id);
        if lock.is_none() {
            lock = cx.world.gun_target(h, id).and_then(|(t, _)| t);
        }
    }
    if !fired_ids.is_empty()
        && let Some(aim) = target
    {
        let aim = in_lock_frame(cx, lock, aim);
        let id = cx.world.get(h).map_or(0, |o| o.simple.id);
        notify_fire_weapon(cx, system, id, &fired_ids, aim, source_client);
    }
    result
}

/// Outside single player, the aim point in the lock target's frame
/// (`I3DMathEngine::InverseTransform` over the target's orientation and
/// position); the point itself otherwise.
fn in_lock_frame(cx: &Cx<'_>, lock: Option<ObjHandle>, aim: Vector) -> Vector {
    if cx.world.config().single_player {
        return aim;
    }
    match lock.and_then(|t| cx.world.get(t)) {
        Some(t) => inverse_transform(
            &Transform::new(t.engine.orientation, t.engine.position),
            aim,
        ),
        None => aim,
    }
}

/// The forward aim of `fire_forward`: the barrels' average position plus
/// their average direction scaled to 1e10 over its length. `SrvGun` sums
/// it `(z*z + y*y) + x*x` and `SrvLauncher` `(y*y + z*z) + x*x`: the same
/// value, since a sum of two terms does not depend on their order.
fn forward_aim(cx: &Cx<'_>, h: ObjHandle, id: SubObjId) -> Vector {
    let (p, d) = cx
        .world
        .launcher_barrel_average(h, id)
        .unwrap_or((Vector::ZERO, Vector::ZERO));
    let (x, y, z) = (e(d.x), e(d.y), e(d.z));
    let length_sq = (z * z + y * y) + x * x;
    let scale = 10_000_000_000.0 / length_sq.sqrt();
    let d = Vector::new(r(x * scale), r(y * scale), r(z * scale));
    Vector::new(r(e(p.x) + e(d.x)), r(e(d.y) + e(p.y)), r(e(d.z) + e(p.z)))
}

/// `fire` (slot 3) of the launcher wrapper at `index`: a live item on a
/// live owner whose `CELauncher::Fire` answers `FireOk` pays for the shot
/// (`ConsumeFireResources`), counts it and launches it: a gun's seeking
/// munition as missiles, its bullets only outside single player; a
/// dropper's mines or counter-measures. True when it fired.
fn fire(cx: &mut Cx<'_>, h: ObjHandle, index: usize, target: Vector) -> bool {
    let Some((id, kind)) = wrapper_kind(cx, h, index) else {
        return false;
    };
    if !live_item(cx, h, id) || is_destroyed(cx, h) {
        return false;
    }
    if cx.world.launcher_fire(h, id, target) != Some(FireResult::Ok) {
        return false;
    }
    cx.world.launcher_consume_fire_resources(h, id);
    count_shot(cx, h, index);
    match kind {
        ServerEquipKind::Gun(_) => {
            if seeks(cx, h, id) {
                launch_guided(cx, h, id, target);
            } else if !cx.world.config().single_player {
                launch_bullets(cx, h, id, target);
            }
        }
        ServerEquipKind::MineDropper(_) => launch_mines(cx, h, id, target),
        ServerEquipKind::CounterMeasureDropper(_) => launch_counter_measures(cx, h, id, target),
        _ => {}
    }
    true
}

/// `fire_unchecked` (slot 5): `use_ammo`, then a gun launches only a
/// seeking munition and a dropper its mines or counter-measures. True when
/// the ammunition was paid for.
fn fire_unchecked(cx: &mut Cx<'_>, h: ObjHandle, index: usize, target: Vector) -> bool {
    let Some((id, kind)) = wrapper_kind(cx, h, index) else {
        return false;
    };
    if !use_ammo(cx, h, index) {
        return false;
    }
    match kind {
        ServerEquipKind::Gun(_) => {
            if seeks(cx, h, id) {
                launch_guided(cx, h, id, target);
            }
        }
        ServerEquipKind::MineDropper(_) => launch_mines(cx, h, id, target),
        ServerEquipKind::CounterMeasureDropper(_) => launch_counter_measures(cx, h, id, target),
        _ => {}
    }
    true
}

/// `SrvLauncher::use_ammo`: a live item on a live owner counts the shot;
/// outside single player the ammunition must be there
/// (`AmmoNeedsMet`) and is paid for (`ConsumeFireResources`). A player's
/// current ship then loses one round of an ammunition-using projectile
/// from the player's cargo (`PlayerData_RemoveCargo(arch, 1, 1.0, no
/// notify)`).
fn use_ammo(cx: &mut Cx<'_>, h: ObjHandle, index: usize) -> bool {
    let Some((id, _)) = wrapper_kind(cx, h, index) else {
        return false;
    };
    if !live_item(cx, h, id) || is_destroyed(cx, h) {
        return false;
    }
    count_shot(cx, h, index);
    if !cx.world.config().single_player {
        if !cx.world.launcher_ammo_needs_met(h, id) {
            return false;
        }
        cx.world.launcher_consume_fire_resources(h, id);
    }
    let (owner_player, owner_id) = cx
        .world
        .get(h)
        .map_or((0, 0), |o| (o.simple.owner_player, o.simple.id));
    let current = cx.logic.players.is_valid(owner_player)
        && cx
            .logic
            .players
            .get(owner_player)
            .is_some_and(|p| p.ship_object_id == owner_id);
    if current
        && let Some(p) = cx.world.launcher_projectile(h, id)
        && p.requires_ammo
    {
        crate::player::remove_cargo(cx, owner_player, p.nickname, 1, 1.0, false);
    }
    true
}

/// `fire_forward` (slot 4): the wrapper fires at its forward aim. A gun
/// that fired (`SrvGun::fire_forward`) on an object in a system announces
/// itself (`notify_fire_weapon` with its id alone), the aim in its lock
/// target's frame outside single player; a dropper (`SrvLauncher`'s)
/// announces nothing. True when it fired.
fn fire_forward(cx: &mut Cx<'_>, h: ObjHandle, index: usize, source_client: i32) -> bool {
    let Some((id, kind)) = wrapper_kind(cx, h, index) else {
        return false;
    };
    let gun = matches!(kind, ServerEquipKind::Gun(_));
    let target = forward_aim(cx, h, id);
    let fired = fire(cx, h, index, target);
    if !fired || !gun {
        return fired;
    }
    let Some(system) = owner_system(cx, h) else {
        return fired;
    };
    let lock = cx.world.gun_target(h, id).and_then(|(t, _)| t);
    let aim = in_lock_frame(cx, lock, target);
    let owner = cx.world.get(h).map_or(0, |o| o.simple.id);
    notify_fire_weapon(cx, system, owner, &[id], aim, source_client);
    fired
}

/// The shared part of a launch: the owner's system (re-read for each
/// shot), its id, the projectile archetype.
fn launch_owner(cx: &Cx<'_>, h: ObjHandle, id: SubObjId) -> Option<(u32, u32, u32)> {
    let o = cx.world.get(h)?;
    let system = o.simple.system?;
    let arch = cx.world.launcher_projectile(h, id)?.nickname;
    Some((system, o.simple.id, arch))
}

fn spawn_of(arch: u32, owner: u32, shot: &ProjLaunchInfo) -> ProjectileSpawn {
    ProjectileSpawn {
        archetype: arch,
        owner,
        position: shot.pos,
        orientation: shot.orient,
        velocity: shot.velocity,
        target: None,
        angular_velocity: Vector::ZERO,
    }
}

/// `SrvGun::launch_bullets`: one bullet per barrel's launch record
/// ([`create_bullet`]).
fn launch_bullets(cx: &mut Cx<'_>, h: ObjHandle, id: SubObjId, target: Vector) {
    let shots = cx.world.launcher_launch_info(h, id, target);
    let Some((system, owner, arch)) = launch_owner(cx, h, id) else {
        return;
    };
    for shot in &shots {
        create_bullet(cx, system, &spawn_of(arch, owner, shot));
    }
}

/// `SrvGun::launch_guided`: nothing without launch records; otherwise one
/// missile per record ([`create_guided`]) with the record's lock target
/// (its id, 0 for none) and sub-object.
fn launch_guided(cx: &mut Cx<'_>, h: ObjHandle, id: SubObjId, target: Vector) {
    let shots = cx.world.launcher_launch_info(h, id, target);
    if shots.is_empty() {
        return;
    }
    for shot in &shots {
        let Some((system, owner, arch)) = launch_owner(cx, h, id) else {
            return;
        };
        let (target_id, target_sub) = match shot.lock {
            Some((t, sub)) => (cx.world.get(t).map_or(0, |o| o.simple.id), sub),
            None => (0, SUBOBJ_ID_NONE),
        };
        let launch = GuidedLaunch {
            spawn: spawn_of(arch, owner, shot),
            equip: id,
            target_id,
            target_sub,
        };
        create_guided(cx, system, &launch);
    }
}

/// `SrvMineDropper::launch`: nothing without launch records; otherwise one
/// mine per record through [`crate::munitions::create_mine_from`], with the
/// dropper's id.
fn launch_mines(cx: &mut Cx<'_>, h: ObjHandle, id: SubObjId, target: Vector) {
    let shots = cx.world.launcher_launch_info(h, id, target);
    for shot in &shots {
        let Some((system, owner, arch)) = launch_owner(cx, h, id) else {
            return;
        };
        crate::munitions::create_mine_from(cx, system, &spawn_of(arch, owner, shot), id);
    }
}

/// `SrvCounterMeasureDropper::launch`: as [`launch_mines`], through
/// [`crate::munitions::create_counter_measure_from`].
fn launch_counter_measures(cx: &mut Cx<'_>, h: ObjHandle, id: SubObjId, target: Vector) {
    let shots = cx.world.launcher_launch_info(h, id, target);
    for shot in &shots {
        let Some((system, owner, arch)) = launch_owner(cx, h, id) else {
            return;
        };
        crate::munitions::create_counter_measure_from(cx, system, &spawn_of(arch, owner, shot), id);
    }
}

/// `StarSystem::create_bullet`: the round in the system (`CBeam::init`
/// with no id, its owner's id, no spin) and its server entry. No player
/// hears of it: the clients make their rounds from the fire notice.
pub fn create_bullet(cx: &mut Cx<'_>, system: u32, spawn: &ProjectileSpawn) -> Option<ObjHandle> {
    let h = cx.world.create_bullet(system, spawn).ok()?;
    let _ = cx.logic.objects.entry(cx.world, h);
    Some(h)
}

/// `FServerLaunchRecord` of a missile: the launch, the gun's id and the
/// lock target's id and sub-object.
#[derive(Clone, Debug, PartialEq)]
pub struct GuidedLaunch {
    /// The archetype, owner, position, orientation and velocity (the
    /// target is the record's).
    pub spawn: ProjectileSpawn,
    /// `EquipId`: the launcher.
    pub equip: SubObjId,
    /// `TargetId` (0 none).
    pub target_id: u32,
    /// `TargetSubId`.
    pub target_sub: SubObjId,
}

/// `StarSystem::create_guided`: the lock target found by id, the missile
/// in the system (`CGuided::init` with the target and sub-object, then
/// `Guided::arm` and the target's behaviour event 6, which the world's
/// creator makes), its server entry, then every player of the system gets
/// its create with the launcher's id
/// ([`crate::replication::guided_created`]). `None` for an unknown
/// archetype or system.
pub fn create_guided(cx: &mut Cx<'_>, system: u32, launch: &GuidedLaunch) -> Option<ObjHandle> {
    let target = if launch.target_id == 0 {
        None
    } else {
        cx.world.find(launch.target_id)
    };
    let spawn = ProjectileSpawn {
        target: target.map(|t| (t, launch.target_sub)),
        ..launch.spawn.clone()
    };
    let h = cx.world.create_guided(system, &spawn).ok()?;
    if let Some(KindData::Guided(g)) = cx.world.get_mut(h).map(|o| &mut o.data) {
        g.sub_target = launch.target_sub;
    }
    let _ = cx.logic.objects.entry(cx.world, h);
    crate::replication::guided_created(cx, system, h, launch.equip);
    Some(h)
}

/// The world's `equip_event` hook (`ServerEquip::notify` of the wrapper
/// listening to the item): `SrvThruster`'s starved thruster on an NPC's
/// object switches its thrusters off (`slot_54(false, 0)`);
/// `SrvCounterMeasureDropper`'s incoming missile on an NPC's object drops a
/// counter-measure (`slot_34(&id, 1, null, -1)`, which fires forward). A
/// player's object ignores both; the other events change nothing on the
/// server (a destroyed item's wrapper forgets it by itself).
pub fn equip_event(cx: &mut Cx<'_>, h: ObjHandle, equip: SubObjId, event: EquipEvent) {
    let kind = cx.logic.objects.get(h).and_then(|o| {
        o.server_equip
            .iter()
            .find(|s| s.equip == equip)
            .map(|s| s.kind)
    });
    let Some(kind) = kind else {
        return;
    };
    let npc = cx.world.get(h).is_some_and(|o| o.simple.owner_player == 0);
    match (kind, event) {
        (ServerEquipKind::Thruster, EquipEvent::ThrusterStarved) if npc => {
            set_thrusters_active(cx, h, false, 0);
        }
        (ServerEquipKind::CounterMeasureDropper(_), EquipEvent::IncomingMissile) if npc => {
            fire_weapons(cx, h, &[equip], None, -1);
        }
        _ => {}
    }
}

/// `Ship::slot_54(bActivate, SourceClient)`: -5 outside a system or
/// destroyed; a source of -1 is 0. Unless a client's request in single
/// player (which is only relayed): the server's own request to switch them
/// on is refused (-5) while cruising and first switches an inactive engine
/// on (`slot_4c`); every thruster not as asked is switched; the server's
/// own request that changed nothing answers 0 at once. Then the observers
/// hear of it (`notify_thrusters`). An object without an engine reads as
/// not cruising with its engine on (D650).
pub fn set_thrusters_active(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    activate: bool,
    source_client: i32,
) -> i32 {
    let Some(system) = owner_system(cx, h) else {
        return -5;
    };
    if is_destroyed(cx, h) {
        return -5;
    }
    let source = if source_client == -1 {
        0
    } else {
        source_client
    };
    if !(cx.world.config().single_player && source != 0) {
        let engine = cx
            .world
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.first(types::ENGINE))
            .map(|e| {
                let cruise = matches!(&e.kind, EquipKind::Engine(s) if s.cruise_active);
                (e.id, cruise, e.is_active())
            });
        if source == 0 && activate {
            if engine.is_some_and(|(_, cruise, _)| cruise) {
                return -5;
            }
            if let Some((id, _, false)) = engine {
                activate_equip(cx, h, id, true, 0);
            }
        }
        let thrusters: Vec<(SubObjId, bool)> = cx
            .world
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .map(|eq| {
                eq.equipment
                    .traverse(types::THRUSTER, false)
                    .into_iter()
                    .filter_map(|slot| eq.equipment.get(slot))
                    .map(|t| (t.id, t.is_active()))
                    .collect()
            })
            .unwrap_or_default();
        let mut changed = false;
        for (id, active) in thrusters {
            if active != activate {
                changed = true;
                cx.world.command(h, Command::ActivateEquip(id, activate));
            }
        }
        if source == 0 && !changed {
            return 0;
        }
    }
    let id = cx.world.get(h).map_or(0, |o| o.simple.id);
    notify_thrusters(cx, system, id, activate, source);
    0
}

/// `Ship::slot_50(bEngage, bArg2, SourceClient)`, the cruise request:
/// -3 for an object that is not a ship (`GameObject::slot_50`); -5
/// outside a system or destroyed; a source of -1 is 0. Unless a client's
/// request in single player (which is only relayed): when the first
/// engine's cruise is not as asked, the server's own request to engage
/// first switches the thrusters off (`slot_54(false, 0)`) and the engine
/// engages or leaves cruise (`CEEngine::EngageCruise(bEngage, bArg2)`);
/// the server's own request that changed nothing answers 0 at once. Then
/// the observers hear of it (`notify_cruise`,
/// [`crate::replication::cruise_changed`]). A ship without an engine,
/// which the C++ reads through null, changes nothing (D708).
pub fn engage_cruise(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    engage: bool,
    arg2: bool,
    source_client: i32,
) -> i32 {
    if cx
        .world
        .get(h)
        .is_none_or(|o| o.kind != game_sim::object::ObjectKind::Ship || o.equipped.is_none())
    {
        return -3;
    }
    let Some(system) = owner_system(cx, h) else {
        return -5;
    };
    if is_destroyed(cx, h) {
        return -5;
    }
    let source = if source_client == -1 {
        0
    } else {
        source_client
    };
    if !(cx.world.config().single_player && source != 0) {
        let cruising = cx
            .world
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.first(types::ENGINE))
            .map(|e| matches!(&e.kind, EquipKind::Engine(s) if s.cruise_active));
        let mut changed = false;
        if let Some(cruising) = cruising
            && cruising != engage
        {
            changed = true;
            if engage && source == 0 {
                set_thrusters_active(cx, h, false, 0);
            }
            cx.world.command(
                h,
                Command::EngageCruise {
                    engage,
                    instant: arg2,
                },
            );
        }
        if source == 0 && !changed {
            return 0;
        }
    }
    let id = cx.world.get(h).map_or(0, |o| o.simple.id);
    crate::replication::cruise_changed(cx, system, id, engage, arg2, source);
    0
}

/// `EqObj::slot_4c(&EquipId, bActivate, SourceClient)`: -5 outside a
/// system or destroyed; a source of -1 is 0. Unless a client's request in
/// single player: a missing or destroyed item is -2; switching an engine
/// off is refused (-5) while the ship strafes or has a thruster on; an
/// item not as asked is switched. The server's own request that changed
/// nothing or failed answers at once; otherwise the observers hear of it
/// (`notify_activate_equip`).
pub fn activate_equip(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    equip: SubObjId,
    activate: bool,
    source_client: i32,
) -> i32 {
    let Some(system) = owner_system(cx, h) else {
        return -5;
    };
    if is_destroyed(cx, h) {
        return -5;
    }
    let source = if source_client == -1 {
        0
    } else {
        source_client
    };
    let mut result = 0;
    let mut changed = false;
    if !cx.world.config().single_player || source == 0 {
        let item = cx
            .world
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.by_id(equip))
            .filter(|e| !e.is_destroyed())
            .map(|e| (e.type_bits, e.is_active()));
        match item {
            None => result = -2,
            Some((type_bits, active)) => {
                let mut refused = false;
                if !activate && type_bits == types::ENGINE {
                    let o = cx.world.get(h);
                    let strafing = o.and_then(|o| o.as_ship()).is_some_and(|s| {
                        s.flight.strafe.current != game_sim::equip::flight::StrafeDir::None
                    });
                    let thrusting = o.and_then(|o| o.equipped.as_deref()).is_some_and(|eq| {
                        eq.equipment
                            .traverse(types::THRUSTER, false)
                            .into_iter()
                            .filter_map(|slot| eq.equipment.get(slot))
                            .any(|t| t.is_active())
                    });
                    if strafing || thrusting {
                        result = -5;
                        refused = true;
                    }
                }
                if !refused && active != activate {
                    cx.world.command(h, Command::ActivateEquip(equip, activate));
                    changed = true;
                }
            }
        }
        if source == 0 && (!changed || result != 0) {
            return result;
        }
    }
    let id = cx.world.get(h).map_or(0, |o| o.simple.id);
    notify_activate_equip(cx, system, id, equip, activate, source);
    result
}

/// A source of -1 is 0 (the AI's requests are the server's own).
const fn own_source(source_client: i32) -> i32 {
    if source_client == -1 {
        0
    } else {
        source_client
    }
}

/// `Ship::set_target(Target, SubTarget, SourceClient)`
/// (`Server/server_gameobject.cpp:5802`): -5 outside a system or
/// destroyed; a source of -1 is 0. Unless a client's request in single
/// player (which is only relayed): a target or sub-target other than the
/// ship's becomes its own (`CShip::set_target`, `set_sub_target`); the
/// server's own request that changed nothing answers 0 at once. Then the
/// observers hear of it (`notify_set_target` with `SUBOBJ_ID_NONE`,
/// [`crate::replication::target_changed`]). The caller has narrowed the
/// object to a ship.
pub fn set_target(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    target: Option<ObjHandle>,
    sub_target: SubObjId,
    source_client: i32,
) -> i32 {
    let Some(system) = owner_system(cx, h) else {
        return -5;
    };
    if is_destroyed(cx, h) {
        return -5;
    }
    let source = own_source(source_client);
    if !(cx.world.config().single_player && source != 0) {
        let current = cx
            .world
            .get(h)
            .and_then(|o| o.as_ship())
            .map(|s| (s.target, s.sub_target));
        let changed = current != Some((target, sub_target));
        if changed {
            cx.world.command(h, Command::SetTarget(target, sub_target));
        }
        if source == 0 && !changed {
            return 0;
        }
    }
    let id = cx.world.get(h).map_or(0, |o| o.simple.id);
    crate::replication::target_changed(cx, system, id, SUBOBJ_ID_NONE, target, sub_target, source);
    0
}

/// `EqObj::slot_44(&EquipId, Target, SubTarget, SourceClient)`
/// (`server_gameobject.cpp:2091`), a gun's target: -5 outside a system or
/// destroyed; a source of -1 is 0. Unless a client's request in single
/// player: a missing or destroyed item is -2, an item that is not a gun
/// -4; otherwise the gun locks the target narrowed to a simple object (or
/// none) and its sub-object (`CEGun::SetTarget`). The server's own request
/// that changed nothing or failed answers at once; otherwise the observers
/// hear of it (`notify_set_target` with the gun, the target unnarrowed).
pub fn set_gun_target(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    equip: SubObjId,
    target: Option<ObjHandle>,
    sub_target: SubObjId,
    source_client: i32,
) -> i32 {
    let Some(system) = owner_system(cx, h) else {
        return -5;
    };
    if is_destroyed(cx, h) {
        return -5;
    }
    let source = own_source(source_client);
    let mut result = 0;
    if !cx.world.config().single_player || source == 0 {
        let item = cx
            .world
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.by_id(equip))
            .filter(|e| !e.is_destroyed())
            .map(|e| matches!(e.kind, EquipKind::Gun(_)));
        let mut changed = false;
        match item {
            None => result = -2,
            Some(false) => result = -4,
            Some(true) => {
                let simple = target.filter(|&t| {
                    cx.world
                        .get(t)
                        .is_some_and(|o| o.is(game_sim::object::class::SIMPLE))
                });
                changed = cx.world.gun_set_target(h, equip, simple, sub_target);
            }
        }
        if source == 0 && (!changed || result != 0) {
            return result;
        }
    }
    let id = cx.world.get(h).map_or(0, |o| o.simple.id);
    crate::replication::target_changed(cx, system, id, equip, target, sub_target, source);
    result
}

/// `EqObj::slot_3c(&EquipId, Targets, SourceClient)`
/// (`server_gameobject.cpp:2030`), a tractor beam on several objects: -5
/// outside a system or destroyed; a source of -1 is 0. Unless a client's
/// request in single player: an item that is not a tractor is -2;
/// otherwise each target is added (`CETractor::AddTarget` of the target
/// narrowed to loot, which refuses anything else and reports it), and none
/// taken is -4. The server's own request that failed answers at once;
/// otherwise the system's other players hear of it
/// ([`crate::replication::tractor_objects`]).
pub fn tractor_objects(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    equip: SubObjId,
    targets: &[ObjHandle],
    source_client: i32,
) -> i32 {
    let Some(system) = owner_system(cx, h) else {
        return -5;
    };
    if is_destroyed(cx, h) {
        return -5;
    }
    let source = own_source(source_client);
    let mut result = 0;
    if !cx.world.config().single_player || source == 0 {
        let tractor = cx
            .world
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.by_id(equip))
            .is_some_and(|e| matches!(e.kind, EquipKind::Tractor(_)));
        if tractor {
            let mut took = 0;
            for &t in targets {
                if cx.world.tractor_add_target(h, equip, t)
                    == game_sim::equip::misc::TractorFailure::Ok
                {
                    took += 1;
                }
            }
            if took == 0 {
                result = -4;
            }
        } else {
            result = -2;
        }
        if source == 0 && result != 0 {
            return result;
        }
    }
    let id = cx.world.get(h).map_or(0, |o| o.simple.id);
    let target_ids: Vec<u32> = targets
        .iter()
        .map(|&t| cx.world.get(t).map_or(0, |o| o.simple.id))
        .collect();
    crate::replication::tractor_objects(cx, system, id, equip, &target_ids, source);
    result
}

/// The server's part of an AI's `Ship::set_target`, `EqObj::slot_44`,
/// `Ship::slot_50`, `slot_54` or `EqObj::slot_4c` (the world's
/// `ServerRequest::Changed`, after the behaviour tick; the world made the
/// change inside it): the players who can see the ship hear of it
/// (`notify_set_target`, `notify_cruise`, `notify_thrusters`,
/// `notify_activate_equip`, the server's own source 0; D857). Nothing for a
/// ship outside a system.
pub fn ai_changed(cx: &mut Cx<'_>, ship: ObjHandle, change: game_sim::object::world::AiChange) {
    use game_sim::object::world::AiChange;
    let Some(system) = owner_system(cx, ship) else {
        return;
    };
    let id = cx.world.get(ship).map_or(0, |o| o.simple.id);
    match change {
        AiChange::Target {
            equip,
            target,
            sub_target,
        } => crate::replication::target_changed(cx, system, id, equip, target, sub_target, 0),
        AiChange::Cruise { engage, instant } => {
            crate::replication::cruise_changed(cx, system, id, engage, instant, 0);
        }
        AiChange::Thrusters(on) => notify_thrusters(cx, system, id, on, 0),
        AiChange::Equip { equip, on } => notify_activate_equip(cx, system, id, equip, on, 0),
    }
}

/// The server's part of an AI's `slot_34` (the world's
/// `ServerRequest::Fired`, after the behaviour tick): each gun that fired
/// counts its shot; each missile gets its server entry and the players'
/// create with its gun ([`crate::replication::guided_created`]); the guns
/// that fired forward (no aim point, or `force_gun_ori`) are announced one
/// by one with their forward aim (`SrvGun::fire_forward`), the unguided
/// guns that fired at the aim point together with the first one's lock
/// target. The fire itself (`CELauncher::Fire`, the resources, the
/// launch) ran inside the tick (D651).
pub fn ai_fired(
    cx: &mut Cx<'_>,
    ship: ObjHandle,
    aim: Option<Vector>,
    results: &[(SubObjId, FireResult)],
    guided: &[(SubObjId, ObjHandle)],
) {
    let Some(system) = owner_system(cx, ship) else {
        return;
    };
    for &(id, result) in results {
        if result != FireResult::Ok {
            continue;
        }
        let index = cx
            .logic
            .objects
            .entry(cx.world, ship)
            .and_then(|o| o.server_equip.iter().position(|s| s.equip == id));
        if let Some(index) = index {
            count_shot(cx, ship, index);
        }
    }
    for &(gun, missile) in guided {
        if cx.world.get(missile).is_none() {
            continue;
        }
        let missile_system = cx.world.get(missile).map_or(system, |o| o.physical.system);
        let _ = cx.logic.objects.entry(cx.world, missile);
        crate::replication::guided_created(cx, missile_system, missile, gun);
    }
    let owner = cx.world.get(ship).map_or(0, |o| o.simple.id);
    let mut fired_ids: Vec<SubObjId> = Vec::new();
    let mut lock: Option<ObjHandle> = None;
    for &(id, result) in results {
        if result != FireResult::Ok {
            continue;
        }
        let force = cx
            .world
            .launcher_projectile(ship, id)
            .is_some_and(|p| p.force_gun_ori);
        match aim {
            Some(_) if !force => {
                if seeks(cx, ship, id) {
                    continue;
                }
                fired_ids.push(id);
                if lock.is_none() {
                    lock = cx.world.gun_target(ship, id).and_then(|(t, _)| t);
                }
            }
            _ => {
                let target = forward_aim(cx, ship, id);
                let gun_lock = cx.world.gun_target(ship, id).and_then(|(t, _)| t);
                let at = in_lock_frame(cx, gun_lock, target);
                notify_fire_weapon(cx, system, owner, &[id], at, -1);
            }
        }
    }
    if !fired_ids.is_empty()
        && let Some(aim) = aim
    {
        let at = in_lock_frame(cx, lock, aim);
        notify_fire_weapon(cx, system, owner, &fired_ids, at, -1);
    }
}

// ---------------------------------------------------------------------------
// The observers' notices: each is the C++ call's site, sent through
// replication's per-object fan-out.

/// `StarSystem::notify_fire_weapon(&ObjId, EquipIds, Target, SourceClient)`
/// (`Server/server_classes.cpp:10145`): the players' `FIREWEAPON`.
fn notify_fire_weapon(
    cx: &mut Cx<'_>,
    system: u32,
    obj_id: u32,
    equip_ids: &[SubObjId],
    target: Vector,
    source_client: i32,
) {
    crate::replication::weapon_fired(cx, system, obj_id, equip_ids, target, source_client);
}

/// `StarSystem::notify_thrusters(&ObjId, bActivate, &SourceClient)`
/// (`server_classes.cpp:10043`).
fn notify_thrusters(cx: &mut Cx<'_>, system: u32, obj_id: u32, activate: bool, source_client: i32) {
    crate::replication::thrusters_changed(cx, system, obj_id, activate, source_client);
}

/// `StarSystem::notify_activate_equip(&ObjId, EquipId, bActivate,
/// &SourceClient)`.
fn notify_activate_equip(
    cx: &mut Cx<'_>,
    system: u32,
    obj_id: u32,
    equip: SubObjId,
    activate: bool,
    source_client: i32,
) {
    crate::replication::equip_activated(cx, system, obj_id, equip, activate, source_client);
}
