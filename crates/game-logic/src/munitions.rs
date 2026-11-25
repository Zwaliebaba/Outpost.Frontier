//! The server's munition wrappers through the world's hooks (work package
//! P3.3, the object part): `Mine` (`Server/server_mine.cpp`),
//! `CounterMeasure` (`server_countermeasure.cpp`) and the server's half of
//! `Guided` (`server_guided.cpp`), with their parts of
//! `Server/server_classes.cpp` (`StarSystem::create_mine` and
//! `create_counter_measure`, and the PvP gate of `explode`,
//! `munition_collision` and `collision_damage`).
//!
//! - **Mine.** `create_mine` makes the mine with the angular velocity it
//!   draws and arms it: a mine with a seek distance gets its controller
//!   and a phantom of that radius. After the mine's own update (its
//!   lifetime, which destroys it quietly when spent), the phantom's
//!   collisions go on; with intruders, the closest one (not the owner
//!   while the owner-safe time runs) detonates the mine inside the
//!   detonation distance and is sought otherwise; an empty phantom stops
//!   the seeking, and a touch the phantom latched detonates. Its
//!   `destroy` explodes it when it was destroyed by damage or a hit
//!   (kind 1), inflicted by its owner, cause 7. The push towards the
//!   target is game-sim's mine controller.
//! - **CounterMeasure.** `create_counter_measure` makes it and diverts the
//!   other owners' missiles in its range, each with the archetype's
//!   chance (`divert_missiles`, one `rand()` per missile). Its lifetime
//!   is the world's update; its `destroy` explodes it on kind 1, cause 4.
//! - **Guided.** `set_target` (`IObjAffect::set_target`): a munition that
//!   does not seek refuses (-3); in single player a client's change is
//!   only relayed. The seeker, the lost target, the lifetime and the
//!   detonation are game-sim's `Guided` (moved into Phase 2); a missile a
//!   counter-measure destroys does not explode.
//!
//! The creators tell the system's players ([`crate::replication`]), and so
//! does a retargeted missile (`XSetTarget`).

use game_sim::equip::SubObjId;
use game_sim::object::damage::{self, DamageCause};
use game_sim::object::world::ProjectileSpawn;
use game_sim::object::{KindData, ObjHandle, ObjectKind, World, class};
use neuron_core::math::Vector;
use neuron_core::physics::PhantomEvent;

use crate::server::Cx;
use crate::world::Hooks;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// The cause of a mine's explosion (`Mine::destroy`).
pub const CAUSE_MINE_EXPLOSION: DamageCause = 7;
/// The cause of a counter-measure's explosion (`CounterMeasure::destroy`).
pub const CAUSE_COUNTER_MEASURE_EXPLOSION: DamageCause = 4;

/// The type bits a mine seeks or detonates on
/// (`GetClosestPhantomIntruderPos`'s first filter in `Mine::update`).
pub const MINE_INTRUDER_TYPES: u32 = 0x7f_fdf8;

/// The most objects `divert_missiles` looks at (`GCounterMeasureHits`).
const DIVERT_MAX: usize = 0x1e;

/// `Mine`'s own fields besides those its controller reads (which game-sim's
/// `MineData` holds: the phantom, `bSeeking`, `Target`).
#[derive(Clone, Debug, PartialEq)]
pub struct MineState {
    /// `IntruderCount`, as the intruders counted: the key of each body
    /// whose `added` counted it, so that its `deleted` uncounts it after
    /// the world has reaped its object (the C++ reads the object's class
    /// while it is being deleted).
    pub intruders: Vec<u64>,
    /// `SeekTimer` (`FLT_MAX` until the mine first seeks).
    pub seek_timer: f32,
    /// `bTriggered`: a non-seeking mine's phantom was touched.
    pub triggered: bool,
    /// `TriggerObjId`: the id of what touched it.
    pub trigger_obj: u32,
}

impl Default for MineState {
    fn default() -> Self {
        Self {
            intruders: Vec::new(),
            seek_timer: f32::MAX,
            triggered: false,
            trigger_obj: 0,
        }
    }
}

impl MineState {
    /// `IntruderCount`.
    #[must_use]
    pub fn intruder_count(&self) -> usize {
        self.intruders.len()
    }
}

fn mine_state<'c>(cx: &'c mut Cx<'_>, h: ObjHandle) -> Option<&'c mut MineState> {
    let o = cx.logic.objects.entry(cx.world, h)?;
    Some(o.mine.get_or_insert_with(MineState::default))
}

/// `random_inside_sphere(|velocity|)`: the angular velocity
/// `create_mine` and `create_counter_measure` give the new object; the
/// length is `(x*x + y*y) + z*z` rooted and stored as a float.
fn launch_spin(world: &mut World, velocity: Vector) -> Vector {
    let length_sq = e(velocity.x) * e(velocity.x)
        + e(velocity.y) * e(velocity.y)
        + e(velocity.z) * e(velocity.z);
    let speed = r(length_sq.sqrt());
    game_sim::object::csys::random_inside_sphere(speed, world.rng())
}

/// `StarSystem::create_mine` (`Server/server_classes.cpp:9427`): the mine
/// in the system with the launch's position, orientation, velocity and
/// owner and an angular velocity drawn inside a sphere of the launch
/// speed, its server entry, then `arm`. `None` for an unknown archetype or
/// system (the C++ reads both through null). Then every player of the
/// system gets its create ([`crate::replication::mine_created`]), with no
/// dropper's item ([`create_mine_from`] names one).
pub fn create_mine(cx: &mut Cx<'_>, system: u32, launch: &ProjectileSpawn) -> Option<ObjHandle> {
    create_mine_from(cx, system, launch, game_sim::equip::SUBOBJ_ID_NONE)
}

/// [`create_mine`] by the dropper `equip` (`FServerLaunchRecord::EquipId`),
/// which the players' create carries.
pub fn create_mine_from(
    cx: &mut Cx<'_>,
    system: u32,
    launch: &ProjectileSpawn,
    equip: SubObjId,
) -> Option<ObjHandle> {
    let spin = launch_spin(cx.world, launch.velocity);
    let spawn = ProjectileSpawn {
        angular_velocity: spin,
        target: None,
        ..launch.clone()
    };
    let h = cx.world.create_mine(system, &spawn).ok()?;
    mine_state(cx, h)?;
    arm(cx, h);
    let info = crate::replication::launch_create_info(cx.world, h, &spawn, equip, spin);
    crate::replication::mine_created(cx, system, h, &info);
    Some(h)
}

/// `Mine::arm`: a mine with a seek distance registers its controller and
/// takes the seek distance as its phantom's radius; one with only a
/// detonation distance takes the seek distance too (so no phantom for a
/// seek distance of 0); one with neither has no phantom. A radius that is
/// not positive makes no phantom (D630).
pub fn arm(cx: &mut Cx<'_>, h: ObjHandle) {
    let Some(KindData::Mine(m)) = cx.world.get(h).map(|o| &o.data) else {
        return;
    };
    let (seek, detonation) = (m.seek_dist, m.detonation_dist);
    let radius = if seek > 0.0 {
        cx.world.register_mine_controller(h);
        seek
    } else if detonation > 0.0 {
        seek
    } else {
        return;
    };
    // `!(radius > 0)`: NaN makes none either.
    if radius <= 0.0 || radius.is_nan() {
        return;
    }
    cx.world.attach_mine_phantom(h, radius);
}

fn is_destroyed(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.logic.objects.get(h).is_some_and(|o| o.destroyed) || cx.world.get(h).is_none_or(|o| o.dead)
}

/// The root of a phantom intruder (`GetRoot`): an attached item's body
/// stands for its owner.
fn intruder_root(world: &World, key: u64) -> Option<ObjHandle> {
    let intruder = ObjHandle::from_key(key);
    match world.get(intruder).map(|o| &o.data) {
        Some(KindData::EquipmentObject {
            owner: Some(owner), ..
        }) => Some(*owner),
        Some(_) => Some(intruder),
        None => None,
    }
}

/// The root of an intruder when it is a `CSimple` (`(ObjectClass & 3) ==
/// 3`).
fn simple_root(world: &World, key: u64) -> Option<ObjHandle> {
    intruder_root(world, key).filter(|&r| world.get(r).is_some_and(|o| o.is(class::SIMPLE)))
}

/// `Mine::update` after the mine's own update (the world's projectile
/// update spent its lifetime and destroyed it quietly, `destroy(0, 0)`):
/// the phantom's collisions go on; with intruders, the closest of type
/// [`MINE_INTRUDER_TYPES`] (not the owner while the owner-safe time runs)
/// is the target: strictly inside the detonation distance it detonates
/// the mine (`destroy(1, its id)`), otherwise the mine wakes and seeks it
/// and the seek timer runs (reset past 1 s); with none the mine stops
/// seeking. With no intruders the mine stops seeking, and a touch latched
/// by `added` detonates it (`destroy(1, toucher)`).
pub fn mine_after_update(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) {
    if is_destroyed(cx, h) {
        return;
    }
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let KindData::Mine(m) = &o.data else {
        return;
    };
    let (system, position) = (o.physical.system, o.engine.position);
    let (phantom, detonation) = (m.phantom, m.detonation_dist);
    let exempt = if m.projectile.is_owner_safe() {
        m.projectile.owner_id
    } else {
        0
    };
    if let Some(p) = phantom
        && !cx
            .world
            .physics(system)
            .is_some_and(|w| w.phantom_collision_state(p))
    {
        cx.world.set_mine_phantom_collisions(h, true);
    }
    let Some(state) = mine_state(cx, h).cloned() else {
        return;
    };
    if state.intruder_count() == 0 {
        stop_seeking(cx, h);
        if state.triggered {
            crate::world::destroy_object(cx, h, 1, state.trigger_obj);
        }
        return;
    }
    let closest = phantom.and_then(|p| {
        let world = &*cx.world;
        world.physics(system)?.closest_phantom_intruder(p, |key| {
            simple_root(world, key)
                .and_then(|r| world.get(r))
                .is_some_and(|o| {
                    o.simple.type_bits & MINE_INTRUDER_TYPES != 0 && o.simple.id != exempt
                })
        })
    });
    let Some((key, target)) = closest else {
        stop_seeking(cx, h);
        return;
    };
    let closest_id = simple_root(cx.world, key)
        .and_then(|r| cx.world.get(r))
        .map_or(0, |o| o.simple.id);
    if let Some(KindData::Mine(m)) = cx.world.get_mut(h).map(|o| &mut o.data) {
        m.seek_target = target;
    }
    let detonation_sq = r(e(detonation) * e(detonation));
    let dx = e(target.x) - e(position.x);
    let dy = e(target.y) - e(position.y);
    let dz = e(target.z) - e(position.z);
    let dist_sq = (dz * dz + dx * dx) + dy * dy;
    if dist_sq < e(detonation_sq) {
        crate::world::destroy_object(cx, h, 1, closest_id);
        return;
    }
    cx.world.wakeup_body(h);
    if let Some(KindData::Mine(m)) = cx.world.get_mut(h).map(|o| &mut o.data) {
        m.seeking = true;
    }
    if let Some(s) = mine_state(cx, h) {
        let timer = e(dt) + e(s.seek_timer);
        s.seek_timer = r(timer);
        if timer > 1.0 {
            s.seek_timer = 0.0;
        }
    }
}

/// `bSeeking = false; Target = 0`.
fn stop_seeking(cx: &mut Cx<'_>, h: ObjHandle) {
    if let Some(KindData::Mine(m)) = cx.world.get_mut(h).map(|o| &mut o.data) {
        m.seeking = false;
        m.seek_target = Vector::ZERO;
    }
}

/// A phantom event the world did not claim ([`game_sim::object::world::
/// ServerHooks::phantom_event`]): a mine's (`Mine::added`, `deleted`).
/// A seeking mine counts each `CSimple` root that enters and uncounts it
/// when it leaves; a non-seeking mine latches the first `CSimple` root to
/// touch it, unless that is its owner while the owner-safe time runs.
pub fn phantom_event(cx: &mut Cx<'_>, system: u32, event: &PhantomEvent) {
    let Some(mine) = cx.world.objects_in(system).into_iter().find(|&m| {
        matches!(
            cx.world.get(m).map(|o| &o.data),
            Some(KindData::Mine(d)) if d.phantom == Some(event.phantom)
        )
    }) else {
        // No mine's: one of `pub::Phantom`'s (`ServerPhantom::added`,
        // `deleted`), if any.
        crate::api::phantom::phantom_event(cx, system, event);
        return;
    };
    let Some(KindData::Mine(m)) = cx.world.get(mine).map(|o| &o.data) else {
        return;
    };
    let seeking = m.seek_dist > 0.0;
    let (owner, safe) = (m.projectile.owner_id, m.projectile.is_owner_safe());
    let root = simple_root(cx.world, event.user_key);
    let root_id = root
        .and_then(|r| cx.world.get(r))
        .map_or(0, |o| o.simple.id);
    let Some(state) = mine_state(cx, mine) else {
        return;
    };
    if seeking {
        if event.entered {
            if root.is_some() {
                state.intruders.push(event.user_key);
            }
        } else if let Some(at) = state.intruders.iter().position(|k| *k == event.user_key) {
            state.intruders.remove(at);
        }
        return;
    }
    // `Mine::deleted` does nothing for a mine that does not seek.
    if !event.entered || root.is_none() {
        return;
    }
    if root_id == owner && safe {
        return;
    }
    state.triggered = true;
    state.trigger_obj = root_id;
}

/// `StarSystem::create_counter_measure` (`server_classes.cpp:9526`): the
/// counter-measure in the system, as `create_mine` makes a mine, its
/// server entry, then `divert_missiles`. `None` for an unknown archetype
/// or system. Then every player of the system gets its create
/// ([`crate::replication::counter_created`]), with no dropper's item
/// ([`create_counter_measure_from`] names one).
pub fn create_counter_measure(
    cx: &mut Cx<'_>,
    system: u32,
    launch: &ProjectileSpawn,
) -> Option<ObjHandle> {
    create_counter_measure_from(cx, system, launch, game_sim::equip::SUBOBJ_ID_NONE)
}

/// [`create_counter_measure`] by the dropper `equip`, which the players'
/// create carries.
pub fn create_counter_measure_from(
    cx: &mut Cx<'_>,
    system: u32,
    launch: &ProjectileSpawn,
    equip: SubObjId,
) -> Option<ObjHandle> {
    let spin = launch_spin(cx.world, launch.velocity);
    let spawn = ProjectileSpawn {
        angular_velocity: spin,
        target: None,
        ..launch.clone()
    };
    let h = cx.world.create_counter_measure(system, &spawn).ok()?;
    let _ = cx.logic.objects.entry(cx.world, h);
    divert_missiles(cx, h);
    let info = crate::replication::launch_create_info(cx.world, h, &spawn, equip, spin);
    crate::replication::counter_created(cx, system, h, &info);
    Some(h)
}

/// `CounterMeasure::divert_missiles`: each guided munition among the first
/// 30 objects within the archetype's range (`PhySys::FindSphereCollisions`)
/// that another owner launched rolls `rand() * 100 / 0x8000`; a roll not
/// above the diversion percentage turns it onto the counter-measure
/// (`set_target(this, SUBOBJ_ID_NONE, 0)`). Returns the missiles diverted.
pub fn divert_missiles(cx: &mut Cx<'_>, h: ObjHandle) -> Vec<ObjHandle> {
    let mut diverted = Vec::new();
    let Some(o) = cx.world.get(h) else {
        return diverted;
    };
    let KindData::CounterMeasure(c) = &o.data else {
        return diverted;
    };
    let (system, position, range, pctg, owner) = (
        o.physical.system,
        o.engine.position,
        c.range,
        c.diversion_pctg,
        c.projectile.owner_id,
    );
    let hits: Vec<ObjHandle> = cx
        .world
        .physics(system)
        .map(|w| w.find_sphere_collisions(position, range, DIVERT_MAX))
        .unwrap_or_default()
        .into_iter()
        .map(ObjHandle::from_key)
        .collect();
    for hit in hits {
        let Some(KindData::Guided(g)) = cx.world.get(hit).map(|o| &o.data) else {
            continue;
        };
        if g.projectile.owner_id == owner {
            continue;
        }
        let roll = cx.world.rng().rand() * 100 / 0x8000;
        if roll > pctg {
            continue;
        }
        let _ = set_target(cx, hit, Some(h), game_sim::equip::SUBOBJ_ID_NONE, 0);
        diverted.push(hit);
    }
    diverted
}

/// `Guided::set_target` (`IObjAffect::set_target`): a source of -1 is 0; a
/// munition that does not seek refuses with -3; on a multiplayer server,
/// or from no client, a different target or sub-target is taken. Then the
/// players hear of it (`notify_set_target` on the root,
/// [`crate::replication::target_changed`]) unless the change came from no
/// client and changed nothing. In single player a
/// client's change is only relayed. Returns 0 otherwise.
pub fn set_target(
    cx: &mut Cx<'_>,
    missile: ObjHandle,
    target: Option<ObjHandle>,
    sub_target: SubObjId,
    source_client: i32,
) -> i32 {
    let source = if source_client == -1 {
        0
    } else {
        source_client
    };
    let single = cx.world.config().single_player;
    let live = |world: &World, t: Option<ObjHandle>| t.filter(|&t| world.get(t).is_some());
    let target = live(cx.world, target);
    let current = match cx.world.get(missile).map(|o| &o.data) {
        Some(KindData::Guided(g)) => {
            if g.seeker == 0 {
                return -3;
            }
            (live(cx.world, g.target), g.sub_target)
        }
        _ => return -3,
    };
    if !single || source == 0 {
        let changed = current != (target, sub_target);
        if changed && let Some(KindData::Guided(g)) = cx.world.get_mut(missile).map(|o| &mut o.data)
        {
            g.target = target;
            g.sub_target = sub_target;
        }
        if source == 0 && !changed {
            return 0;
        }
    }
    // `StarSystem::notify_set_target` on the munition's root.
    if let Some((system, id)) = cx
        .world
        .get(missile)
        .map(|o| (o.physical.system, o.simple.id))
    {
        crate::replication::target_changed(
            cx,
            system,
            id,
            game_sim::equip::SUBOBJ_ID_ROOT,
            target,
            sub_target,
            source,
        );
    }
    0
}

/// `Guided::update`'s lost target (`server_guided.cpp:297`): the seeker
/// dropped it in the world, so `Guided::set_target(nullptr,
/// SUBOBJ_ID_ROOT, 0)` changed the pair and tells the players
/// (`notify_set_target` on the root, from no client).
pub fn target_lost(cx: &mut Cx<'_>, missile: ObjHandle) {
    if let Some((system, id)) = cx
        .world
        .get(missile)
        .map(|o| (o.physical.system, o.simple.id))
    {
        crate::replication::target_changed(
            cx,
            system,
            id,
            game_sim::equip::SUBOBJ_ID_ROOT,
            None,
            game_sim::equip::SUBOBJ_ID_ROOT,
            0,
        );
    }
}

/// The explosion of a mine or counter-measure destroyed by damage or a hit
/// (`Mine::destroy`, `CounterMeasure::destroy`): with an explosion
/// archetype whose radius exceeds 1e-4 and that has some impulse, hull or
/// energy damage above 1e-4, `StarSystem::explode` at its position,
/// inflicted by its owner, with `cause`.
fn explode(cx: &mut Cx<'_>, h: ObjHandle, cause: DamageCause) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let owner = match &o.data {
        KindData::Mine(m) => m.projectile.owner_id,
        KindData::CounterMeasure(c) => c.projectile.owner_id,
        _ => return,
    };
    let (system, position, archetype) = (o.physical.system, o.engine.position, o.archetype);
    let data = cx.world.data();
    let Some(arch) = data.archetypes.equipment.get(&archetype) else {
        return;
    };
    if arch.root.explosion_arch == 0 {
        return;
    }
    let Some(x) = data
        .archetypes
        .explosions
        .get(&arch.root.explosion_arch)
        .cloned()
    else {
        return;
    };
    let epsilon = 0.0001f64;
    if !(e(x.radius) > epsilon
        && (e(x.impulse) > epsilon || e(x.hull_damage) > epsilon || e(x.energy_damage) > epsilon))
    {
        return;
    }
    let blast = damage::Blast {
        position,
        radius: x.radius,
        hull_damage: x.hull_damage,
        energy_damage: x.energy_damage,
    };
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world
        .explode_caused_hooked(system, &blast, cause, owner, Some(h), x.impulse, &mut hooks);
}

/// `Mine::destroy` and `CounterMeasure::destroy`: `GameObject::destroy`,
/// then, destroyed by damage or a hit (kind 1), the explosion (cause 7 for
/// a mine, 4 for a counter-measure). True: the world marks it dead.
pub fn destroy(cx: &mut Cx<'_>, h: ObjHandle, kind: i32, killer: u32) -> bool {
    let _ = killer;
    let Some(object_kind) = cx.world.get(h).map(|o| o.kind) else {
        return true;
    };
    crate::objects::base_destroy(cx, h, kind);
    if kind == 1 {
        let cause = if object_kind == ObjectKind::Mine {
            CAUSE_MINE_EXPLOSION
        } else {
            CAUSE_COUNTER_MEASURE_EXPLOSION
        };
        explode(cx, h, cause);
    }
    true
}

/// The PvP gate (`GbAllowPvp`, `server_classes.cpp:9093`, `9197`, `9790`):
/// with PvP off, damage an object that is a player's ship deals is not
/// applied to a player's ship (`is_player` on both).
#[must_use]
pub fn pvp_blocked(
    forbid_pvp: bool,
    world: &World,
    inflictor: Option<ObjHandle>,
    target: ObjHandle,
) -> bool {
    if !forbid_pvp {
        return false;
    }
    let is_player = |h: Option<ObjHandle>| {
        h.and_then(|h| world.get(h))
            .and_then(|o| o.as_ship())
            .is_some_and(|s| s.is_player)
    };
    is_player(inflictor) && is_player(Some(target))
}
