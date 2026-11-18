//! The [`World`] (decision records 0001 and 0002): every loaded system's
//! objects in one arena, the id → handle map, deferred destruction, the
//! update loop, the physics controller hook and the headless instance
//! tree.
//!
//! # The tick
//!
//! [`World::step`] runs `StarSystem_Update`'s sub-step loop
//! (`Server/IServerImpl.cpp:3812`): the step is cut into pieces of at most
//! 0.2 s outside single player, and each piece updates the object lists
//! in the server's order (ships, guided missiles, the solars near
//! players, loot, bullets, mines, counter-measures, asteroids), reaps the
//! objects marked dead, then, when the world steps physics on the server
//! side, moves the bullets, steps the physics system and processes its
//! collisions. Lists iterate in insertion order, as the C++ lists do.
//!
//! # References and lifetime
//!
//! Watchers and raw pointers of the C++ are [`ObjHandle`]s; a destroyed
//! object stays readable until the reap at the end of the tick, then its
//! handle reads as gone (decision record 0001, points 4 and 5).
//!
//! # Engine coupling
//!
//! The world owns a [`neuron_core::scene::Engine`] with the hardpoint
//! component. With assets ([`World::with_assets`]), objects get the
//! instance trees of their archetype files, so hardpoints (exhaust
//! nozzles, gun mounts, barrels) and part ids come from the data, and
//! bodies are built from the archetypes' `.sur` surfaces; without assets
//! every body is the default part and no hardpoint is found.

mod ai;
mod ambient;
mod animate;
mod combat;
mod sensors;
mod spawn;
mod step;
mod turret;

use std::collections::BTreeMap;
use std::sync::Arc;

use neuron_core::hash::name_crc32;
use neuron_core::id::ObjectIdAllocator;
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::physics::{
    BodyHandle, Geometry, PhySysConstants, PhysicsSystems, PhysicsWorld, StepMode,
};
use neuron_core::rng::CrtRand;
use neuron_core::scene::{Engine, HardpointData, Hardpoints, InstanceId};
use neuron_core::sur::SurPart;
use neuron_core::vfs::{FsDesc, Host, UtfFileSystem, create_file_system};

use crate::data::config::GameData;
use crate::equip::{EquipEvent, SubObjId};

use super::{Arena, DamageList, ObjHandle, Object};

pub use ai::ManagerHook;
pub use combat::damage_sub_obj;
pub use sensors::{EquipStatus, EquipmentVal, equipment_val};
pub(crate) use spawn::shield_status;
pub use spawn::{LootSpawn, ProjectileSpawn, ShipSpawn, SolarSpawn, SpawnError};

/// A force controller the world registered with the physics
/// (`PhySys::Controller::Register`): which object and which of its
/// controllers.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ControllerRef {
    /// `CShip` itself (its `do_simulation_controller` does nothing).
    Ship,
    /// `CSteering`.
    Steering,
    /// `CNudgeEngine`.
    Nudge,
    /// `CStrafeEngine`.
    Strafe,
    /// An equipment item's controller (`CEEngine`, `CEThruster`).
    Equip(SubObjId),
}

/// The behaviour manager's tick (`IBehaviorManager::UpdateBehaviorTick`),
/// which P2.5 implements. The world calls it for each equipped object with
/// a behaviour from inside that object's update, where `CEqObj::update`
/// calls it; the hook issues commands through [`World::command`].
pub trait BehaviorHook {
    /// One behaviour tick of `object`.
    fn update_behavior_tick(&mut self, world: &mut World, object: ObjHandle, dt: f32);
}

/// Something that happened during a step, for the server and client sides
/// (and the scenario test) to read.
#[derive(Clone, Debug, PartialEq)]
pub enum WorldEvent {
    /// `GameObject::on_collision` of `a` against `b` and the collision
    /// damage it computed (`Collision::CalcCollisionDamage`).
    Collision {
        /// The reporting object's id.
        a: u32,
        /// The other object's id.
        b: u32,
        /// The relative speed.
        speed: f32,
        /// The damage applied to `b`.
        damage: f32,
    },
    /// `StarSystem::munition_collision`: a munition hit `target`.
    MunitionHit {
        /// The target's id.
        target: u32,
        /// The shooter's id.
        shooter: u32,
        /// The munition archetype.
        munition: u32,
        /// The sub-object hit.
        sub: SubObjId,
        /// The damage list before it was applied.
        damage: DamageList,
        /// The target's hull before the hit.
        hull_before: f32,
        /// The target's first shield before the hit: its id and hit points.
        shield_before: Option<(SubObjId, f32)>,
    },
    /// An object was destroyed.
    Destroyed {
        /// Its id.
        id: u32,
        /// The killer's id.
        killer: u32,
    },
    /// An equipment item notified its listeners.
    Equip {
        /// The owner's id.
        owner: u32,
        /// The item.
        equip: SubObjId,
        /// The event.
        event: EquipEvent,
    },
    /// `CShip::open_bay` or `close_bay` moved the doors
    /// (`notify(BayStateChanged, open)`).
    BayDoor {
        /// The ship's id.
        ship: u32,
        /// Opening.
        open: bool,
    },
    /// `CSolar::update_system_gate` notified (`notify(1, &code)`): 0 the
    /// gate starts to open, 1 it is open, 2 it starts to close, 3 closed.
    Gate {
        /// The gate's id.
        solar: u32,
        /// The code.
        code: i32,
    },
    /// A gun fired a bullet.
    Fired {
        /// The shooter's id.
        shooter: u32,
        /// The gun.
        equip: SubObjId,
        /// The projectile's id: 0 for a bullet, which has none.
        bullet: u32,
    },
}

/// `StarSystem_MpCollisionFilter` (`Server/server_classes.cpp:11409`): on
/// the multiplayer server a pair collides only when one side is a bullet or
/// a missile (`(ObjectClass & 0x27) == 0x27`); ships, solars, loot and the
/// rest pass through each other (the clients report those collisions).
fn mp_server_collision_filter(class_a: u32, class_b: u32) -> bool {
    class_a & 0x27 == 0x27 || class_b & 0x27 == 0x27
}

/// How the world steps.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WorldConfig {
    /// Single player (`SinglePlayer()`): the mass centre is not pinned,
    /// the server's sub-step is the whole frame, and the launcher fate
    /// exception does not apply.
    pub single_player: bool,
    /// The physics stepping (`PhysicalSystem::Update`'s mode).
    pub step_mode: StepMode,
    /// [`World::step`] steps the physics inside the server's sub-step loop
    /// (the multiplayer server, and the scenario harness). A single-player
    /// client steps it in its own frame ([`World::step_physics`]).
    pub server_physics: bool,
    /// The seed of the CRT random generator (`srand`).
    pub seed: u32,
}

impl Default for WorldConfig {
    fn default() -> Self {
        Self {
            single_player: false,
            step_mode: StepMode::Multiplayer,
            server_physics: true,
            seed: 1,
        }
    }
}

/// The order the server's sub-step updates the object lists in
/// (`StarSystem_Update`): ships, guided missiles, loot, bullets, mines,
/// counter-measures, asteroids. Solars are updated only when near a
/// player's ship (`StarSystem_UpdateNearShips`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum TickList {
    /// `Ships` (+0x34).
    Ships,
    /// `List70`: guided missiles.
    Guided,
    /// `Solars` (+0x5c).
    Solars,
    /// `Loot` (+0x48).
    Loot,
    /// `Beams` (+0x84): bullets.
    Beams,
    /// `List98`: mines.
    Mines,
    /// `ListAC`: counter-measures.
    CounterMeasures,
    /// `ListC0`: asteroids.
    Asteroids,
    /// Objects no list updates (equipment objects, dynamic asteroids).
    Other,
}

impl TickList {
    /// The list an object kind is filed in.
    #[must_use]
    pub fn of(kind: super::ObjectKind) -> Self {
        use super::ObjectKind as K;
        match kind {
            K::Ship => Self::Ships,
            K::Guided => Self::Guided,
            K::Solar => Self::Solars,
            K::Loot => Self::Loot,
            K::Beam => Self::Beams,
            K::Mine => Self::Mines,
            K::CounterMeasure => Self::CounterMeasures,
            K::Asteroid => Self::Asteroids,
            K::DynamicAsteroid | K::EquipmentObject => Self::Other,
        }
    }
}

/// One loaded system: its object lists in insertion order.
#[derive(Clone, Debug, Default)]
pub struct SystemObjects {
    /// The lists.
    pub lists: BTreeMap<TickList, Vec<ObjHandle>>,
}

/// The data files objects read beyond the databases: archetype models and
/// collision surfaces, under the data root.
#[derive(Clone, Debug)]
pub struct Assets {
    host: Host,
}

impl Assets {
    /// The data root (the `DATA` folder).
    #[must_use]
    pub fn new(root: impl Into<std::path::PathBuf>) -> Self {
        Self {
            host: Host::new(root.into()),
        }
    }
}

/// A `.sur` file's parts, by part id.
pub type SurParts = Arc<BTreeMap<u32, SurPart>>;

/// One fire call's shooter id and each gun's result ([`World::set_fire_log`]).
pub type FireCall = (
    u32,
    Vec<(crate::equip::SubObjId, crate::equip::launcher::FireResult)>,
);

/// The simulation world of one process side.
pub struct World {
    pub(crate) data: Arc<GameData>,
    pub(crate) config: WorldConfig,
    assets: Option<Assets>,
    pub(crate) objects: Arena<Object>,
    ids: BTreeMap<u32, ObjHandle>,
    systems: BTreeMap<u32, SystemObjects>,
    pub(crate) physics: PhysicsSystems,
    pub(crate) scene: Engine,
    instances: BTreeMap<InstanceId, ObjHandle>,
    surfaces: BTreeMap<u32, Option<SurParts>>,
    pub(crate) controllers: BTreeMap<u64, (ObjHandle, ControllerRef)>,
    next_controller: u64,
    /// The serial of the next free part's body key.
    pub(crate) next_free_part: u32,
    id_alloc: ObjectIdAllocator,
    pub(crate) rng: CrtRand,
    pub(crate) time: f64,
    pub(crate) events: Vec<WorldEvent>,
    behavior: Option<Box<dyn BehaviorHook>>,
    pub(crate) behavior_managers: BTreeMap<ObjHandle, crate::ai::manager::BehaviorManager<World>>,
    /// `GAvoidIndexTableShuffled`: the first obstacle probe of the process
    /// has drawn its 50 `rand()` values.
    pub(crate) avoid_table_shuffled: bool,
    /// The attitudes the AI reads (`get_attitude_towards`), by object
    /// ids: the reputation groups are not ported, so a harness sets them
    /// ([`World::set_attitude`]).
    pub(crate) attitudes: BTreeMap<(u32, u32), f32>,
    always_update: Vec<ObjHandle>,
    anim_channels: Vec<turret::AnimChannel>,
    anim_playbacks: Vec<(ObjHandle, animate::AnimKind)>,
    /// The scene's animation component (`NeuronRuntime::Anim`), which
    /// plays the ambient animations.
    pub(crate) animation: neuron_core::scene::animation::Animation,
    /// The animation scripts of archetype files, by path.
    pub(crate) anim_scripts: BTreeMap<Vec<u8>, Option<Arc<neuron_core::anim::AnimScript>>>,
    /// `AnimDB`'s playback map for the ambient animations
    /// (`CEInternalFX`'s `use_animation`), by playback id.
    pub(crate) ambient: BTreeMap<i32, ambient::AmbientSub>,
    /// The results of every fire call, when kept ([`World::set_fire_log`]).
    pub(crate) fire_log: Option<Vec<FireCall>>,
}

impl core::fmt::Debug for World {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("World")
            .field("objects", &self.objects.len())
            .field("systems", &self.systems.len())
            .field("time", &self.time)
            .finish_non_exhaustive()
    }
}

/// `[PhySys]` constants from the game data's `[PhySys]` section.
#[must_use]
pub fn phy_sys_constants(data: &GameData) -> PhySysConstants {
    let p = &data.constants.phy_sys;
    PhySysConstants {
        physical_sim_rate: p.physical_sim_rate,
        max_spawned_mindist_count: p.max_spawned_mindist_count,
        anom_limits_max_velocity: p.anom_limits_max_velocity,
        anom_limits_max_angular_velocity_per_psi: p.anom_limits_max_angular_velocity_per_psi,
        material_friction: p.material_friction,
        material_elasticity: p.material_elasticity,
        min_time_between_collisions: p.min_time_between_collisions,
        rmgr_look_ahead_time_world: p.rmgr_look_ahead_time_world,
        rmgr_look_ahead_max_radius_world: p.rmgr_look_ahead_max_radius_world,
        rmgr_look_ahead_min_distance_world: p.rmgr_look_ahead_min_distance_world,
        rmgr_look_ahead_max_distance_world: p.rmgr_look_ahead_max_distance_world,
        rmgr_look_ahead_min_seconds_world: p.rmgr_look_ahead_min_seconds_world,
        rmgr_look_ahead_time_intra: p.rmgr_look_ahead_time_intra,
        rmgr_look_ahead_max_radius_intra: p.rmgr_look_ahead_max_radius_intra,
        rmgr_look_ahead_min_distance_intra: p.rmgr_look_ahead_min_distance_intra,
        rmgr_look_ahead_max_distance_intra: p.rmgr_look_ahead_max_distance_intra,
        rmgr_look_ahead_min_seconds_intra: p.rmgr_look_ahead_min_seconds_intra,
        default_linear_damping: p.default_linear_damping,
        default_angular_damping: p.default_angular_damping,
        golem_child_mass: p.golem_child_mass,
        golem_child_linear_damp: p.golem_child_linear_damp,
        golem_child_angular_damp: p.golem_child_angular_damp,
        golem_max_delta_position: p.golem_max_delta_position,
        golem_delta_orientation: p.golem_delta_orientation,
        golem_max_torque: p.golem_max_torque,
        golem_force_factor: p.golem_force_factor,
        golem_damp_factor: p.golem_damp_factor,
        golem_torque_factor: p.golem_torque_factor,
        golem_angular_damp_factor: p.golem_angular_damp_factor,
        rc_max_delta_position: p.rc_max_delta_position,
        rc_max_delta_orientation: p.rc_max_delta_orientation,
        golem_max_translation_force: p.golem_max_translation_force,
        ..PhySysConstants::default()
    }
}

impl World {
    /// A world over loaded game data, without assets.
    #[must_use]
    pub fn new(data: Arc<GameData>, config: WorldConfig) -> Self {
        let constants = phy_sys_constants(&data);
        let mut scene = Engine::new();
        scene.add_component(Box::new(Hardpoints::new()));
        Self {
            data,
            config,
            assets: None,
            objects: Arena::default(),
            ids: BTreeMap::new(),
            systems: BTreeMap::new(),
            physics: PhysicsSystems::new(constants),
            scene,
            instances: BTreeMap::new(),
            surfaces: BTreeMap::new(),
            controllers: BTreeMap::new(),
            next_controller: 1,
            next_free_part: 0,
            id_alloc: ObjectIdAllocator::default(),
            rng: CrtRand::with_seed(config.seed),
            time: 0.0,
            events: Vec::new(),
            behavior: None,
            behavior_managers: BTreeMap::new(),
            avoid_table_shuffled: false,
            attitudes: BTreeMap::new(),
            always_update: Vec::new(),
            anim_channels: Vec::new(),
            anim_playbacks: Vec::new(),
            animation: neuron_core::scene::animation::Animation::new(),
            anim_scripts: BTreeMap::new(),
            ambient: BTreeMap::new(),
            fire_log: None,
        }
    }

    /// The world with the data root for archetype models and surfaces.
    #[must_use]
    pub fn with_assets(mut self, assets: Assets) -> Self {
        let host = assets.host.clone();
        self.scene
            .set_search_path(Some(Box::new(move |desc: &FsDesc<'_>| {
                create_file_system(&host, desc)
            })));
        self.assets = Some(assets);
        self
    }

    /// The game data.
    #[must_use]
    pub fn data(&self) -> &GameData {
        &self.data
    }

    /// The configuration.
    #[must_use]
    pub fn config(&self) -> &WorldConfig {
        &self.config
    }

    /// Installs the behaviour manager's tick (P2.5).
    pub fn set_behavior_hook(&mut self, hook: Option<Box<dyn BehaviorHook>>) {
        self.behavior = hook;
    }

    /// The global time (`Timing::GetGlobalTime`), advanced by each step.
    #[must_use]
    pub fn time(&self) -> f64 {
        self.time
    }

    /// The CRT random generator (`rand()`, decision T11).
    pub fn rng(&mut self) -> &mut CrtRand {
        &mut self.rng
    }

    /// Takes the events since the last call.
    pub fn take_events(&mut self) -> Vec<WorldEvent> {
        std::mem::take(&mut self.events)
    }

    /// `StarSystem::load`'s physics part: the system's physics world
    /// (`PhySys::Startup`) and its object lists.
    pub fn load_system(&mut self, system: u32) {
        if self.systems.contains_key(&system) {
            return;
        }
        self.physics.startup(system);
        // `StarSystem::init`: outside single player the server installs its
        // collision filter (`Server/server_classes.cpp:11438-11445`).
        if !self.config.single_player
            && self.config.server_physics
            && let Some(world) = self.physics.world_mut(system)
        {
            world.set_collision_filter(Some(mp_server_collision_filter));
        }
        self.systems.insert(system, SystemObjects::default());
    }

    /// Whether a loaded system's physics world keeps the multiplayer server's
    /// collision filter that [`World::load_system`] installs. Off is the
    /// filter's single-player branch (every pair collides), which the
    /// scenario oracle's `collisions sp` selects on its multiplayer server
    /// (`StarSystem_MpCollisionFilter`, `Server/server_classes.cpp:11409`).
    pub fn set_server_collision_filter(&mut self, system: u32, on: bool) {
        let install = on && !self.config.single_player && self.config.server_physics;
        if let Some(world) = self.physics.world_mut(system) {
            world.set_collision_filter(
                install.then_some(mp_server_collision_filter as fn(u32, u32) -> bool),
            );
        }
    }

    /// The physics world of a system.
    #[must_use]
    pub fn physics(&self, system: u32) -> Option<&PhysicsWorld> {
        self.physics.world(system)
    }

    /// The scene (the headless instance tree).
    #[must_use]
    pub fn scene(&self) -> &Engine {
        &self.scene
    }

    /// An object.
    #[must_use]
    pub fn get(&self, h: ObjHandle) -> Option<&Object> {
        self.objects.get(h)
    }

    /// An object, mutably.
    pub fn get_mut(&mut self, h: ObjHandle) -> Option<&mut Object> {
        self.objects.get_mut(h)
    }

    /// The object with an id (`CObject::Find`, `IObjDB::FindById`).
    #[must_use]
    pub fn find(&self, id: u32) -> Option<ObjHandle> {
        self.ids
            .get(&id)
            .copied()
            .filter(|h| self.objects.contains(*h))
    }

    /// The object an engine instance belongs to (`GetInstanceUserData`).
    #[must_use]
    pub fn object_of_instance(&self, instance: InstanceId) -> Option<ObjHandle> {
        self.instances.get(&instance).copied()
    }

    /// `AllocateObjectID`: the next dynamic id.
    pub fn allocate_id(&mut self) -> u32 {
        self.id_alloc.allocate()
    }

    /// Every live object of a system, list by list in update order.
    #[must_use]
    pub fn objects_in(&self, system: u32) -> Vec<ObjHandle> {
        self.systems
            .get(&system)
            .map(|s| {
                s.lists
                    .values()
                    .flatten()
                    .copied()
                    .filter(|h| self.objects.contains(*h))
                    .collect()
            })
            .unwrap_or_default()
    }

    /// Keeps a solar updated every step (the server's `ListD4`), whether or
    /// not a player is near.
    pub fn set_always_update(&mut self, h: ObjHandle, on: bool) {
        self.always_update.retain(|x| *x != h);
        if on {
            self.always_update.push(h);
        }
    }

    // ---- objects ------------------------------------------------------------------------

    pub(crate) fn insert(&mut self, system: u32, object: Object) -> ObjHandle {
        let kind = object.kind;
        let id = object.simple.id;
        let instance = object.engine.instance;
        let h = self.objects.insert(object);
        if id != 0 {
            self.ids.insert(id, h);
        }
        if let Some(i) = instance {
            self.instances.insert(i, h);
        }
        self.systems
            .entry(system)
            .or_default()
            .lists
            .entry(TickList::of(kind))
            .or_default()
            .push(h);
        h
    }

    /// Marks an object dead; it stays readable until the end of the tick
    /// (decision record 0001, point 5). The physics body leaves the
    /// collision checks at once.
    pub fn destroy(&mut self, h: ObjHandle, killer: u32) {
        let Some(obj) = self.objects.get_mut(h) else {
            return;
        };
        if obj.dead {
            return;
        }
        obj.dead = true;
        let id = obj.simple.id;
        let body = obj.physical.body;
        let system = obj.physical.system;
        if let (Some(body), Some(world)) = (body, self.physics.world_mut(system)) {
            world.set_collision_state(body, false);
        }
        self.events.push(WorldEvent::Destroyed { id, killer });
    }

    /// The end-of-tick reap: dead objects leave the lists, the id map, the
    /// physics and the instance tree; their handles read as gone.
    pub fn reap(&mut self) {
        let dead: Vec<ObjHandle> = self
            .objects
            .iter()
            .filter(|(_, o)| o.dead)
            .map(|(h, _)| h)
            .collect();
        for h in dead {
            self.remove_now(h);
        }
    }

    fn remove_now(&mut self, h: ObjHandle) {
        let Some(obj) = self.objects.remove(h) else {
            return;
        };
        if self.ids.get(&obj.simple.id) == Some(&h) {
            self.ids.remove(&obj.simple.id);
        }
        let system = obj.physical.system;
        for (_, handle, key) in &obj.physical.controllers {
            if let Some(world) = self.physics.world_mut(system) {
                world.unregister_controller(*handle);
            }
            self.controllers.remove(key);
        }
        if let (Some(body), Some(world)) = (obj.physical.body, self.physics.world_mut(system)) {
            world.destroy_body(body);
        }
        if let Some(world) = self.physics.world_mut(system) {
            for part in &obj.physical.free_parts {
                world.destroy_body(part.body);
            }
        }
        self.end_ambient(h);
        if let Some(instance) = obj.engine.instance {
            self.instances.retain(|_, v| *v != h);
            self.scene.destroy_instance(instance);
        }
        for sys in self.systems.values_mut() {
            for list in sys.lists.values_mut() {
                list.retain(|x| *x != h);
            }
        }
        self.always_update.retain(|x| *x != h);
        self.behavior_managers.remove(&h);
    }

    // ---- assets -----------------------------------------------------------------------------

    /// Loads an archetype file into the scene and makes an instance of it
    /// (`Root::load` and `create_instance`); `None` without assets or when
    /// the file does not load.
    pub(crate) fn create_scene_instance(&mut self, path: &[u8]) -> Option<InstanceId> {
        let assets = self.assets.as_ref()?;
        if path.is_empty() {
            return None;
        }
        // An archetype the scene already holds needs no file
        // (`CreateArchetypeHandle` finds it by name first).
        if let Ok(Some(archetype)) = self.scene.create_archetype_handle(path, None) {
            return self.scene.create_instance(archetype);
        }
        let mut fs = UtfFileSystem::open(
            &assets.host,
            &FsDesc::read(path).with_implementation(b"UTF"),
        )
        .ok()?;
        let archetype = self
            .scene
            .create_archetype_handle(path, Some(&mut fs))
            .ok()
            .flatten()?;
        self.scene.create_instance(archetype)
    }

    /// The `.sur` parts of an archetype file (`PhyArch::LoadSurfaces` of the
    /// file with the extension `sur`), cached by archetype id.
    pub(crate) fn surface_parts(&mut self, archetype: u32, path: &[u8]) -> Option<SurParts> {
        if let Some(cached) = self.surfaces.get(&archetype) {
            return cached.clone();
        }
        let parts = self.assets.as_ref().and_then(|assets| {
            let lower = path.to_ascii_lowercase();
            if lower.ends_with(b".sph") {
                return None;
            }
            let dot = path.iter().rposition(|&c| c == b'.')?;
            let mut sur = path[..dot].to_vec();
            sur.extend_from_slice(b".sur");
            let bytes = assets.host.read_file(&sur).ok()?.data;
            let load = neuron_core::sur::load(&bytes);
            Some(Arc::new(load.parts))
        });
        self.surfaces.insert(archetype, parts.clone());
        parts
    }

    /// The geometry of a part (`get_phy_part`), or the default part
    /// (`PhyArch::GetDefaultPart`) when the archetype has none for it.
    pub(crate) fn part_geometry(parts: Option<&SurParts>, part_id: u32) -> (Geometry, Vec<u32>) {
        let part = parts.and_then(|p| {
            p.get(&part_id).or_else(|| {
                // A simple (non-compound) model's instance has no part
                // name; its file holds one part.
                (part_id == 0 && p.len() == 1)
                    .then(|| p.values().next())
                    .flatten()
            })
        });
        if let Some(part) = part
            && let Some(Ok(surface)) = part.compact_surface()
        {
            return (Geometry::from_surface(&surface), part.hardpoint_ids.clone());
        }
        (Geometry::default_part(), Vec::new())
    }

    /// The hardpoint ids of a part, as [`World::part_geometry`] gives them,
    /// without building the part's hulls.
    pub(crate) fn part_hardpoint_list(parts: Option<&SurParts>, part_id: u32) -> Vec<u32> {
        let part = parts.and_then(|p| {
            p.get(&part_id).or_else(|| {
                (part_id == 0 && p.len() == 1)
                    .then(|| p.values().next())
                    .flatten()
            })
        });
        match part {
            Some(part) if matches!(part.compact_surface(), Some(Ok(_))) => {
                part.hardpoint_ids.clone()
            }
            _ => Vec::new(),
        }
    }

    /// `FindHardpoint_OS(instance, name)`: the hardpoint on the instance or
    /// the first of its descendants (depth first) that has it.
    #[must_use]
    pub fn find_hardpoint(
        &self,
        instance: Option<InstanceId>,
        name: &[u8],
    ) -> Option<(InstanceId, HardpointData)> {
        let id = instance?;
        let crc = name_crc32(name);
        let hardpoints = self.scene.component::<Hardpoints>()?;
        let mut stack = vec![id];
        while let Some(i) = stack.pop() {
            if let Some(arch) = self.scene.instance_archetype(i)
                && let Some(data) = hardpoints.get(arch, crc)
            {
                return Some((i, data));
            }
            let mut children = Vec::new();
            let mut cursor = None;
            while let Some(c) = self.scene.related_instance(Some(i), 1, cursor) {
                children.push(c);
                cursor = Some(c);
            }
            for c in children.into_iter().rev() {
                stack.push(c);
            }
        }
        None
    }

    /// Every instance of a tree, root first, depth first.
    pub(crate) fn instance_tree(&self, root: Option<InstanceId>) -> Vec<InstanceId> {
        let mut out = Vec::new();
        let Some(root) = root else {
            return out;
        };
        let mut stack = vec![root];
        while let Some(i) = stack.pop() {
            out.push(i);
            let mut children = Vec::new();
            let mut cursor = None;
            while let Some(c) = self.scene.related_instance(Some(i), 1, cursor) {
                children.push(c);
                cursor = Some(c);
            }
            for c in children.into_iter().rev() {
                stack.push(c);
            }
        }
        out
    }

    /// Puts an object's instance tree at its frame (the engine sync of
    /// `PhySys_SyncObjectFromBody` and `CSimple::init`'s `UpdateInstance`).
    pub(crate) fn sync_instance(&mut self, h: ObjHandle) {
        let Some(obj) = self.objects.get(h) else {
            return;
        };
        let Some(instance) = obj.engine.instance else {
            return;
        };
        let t = Transform::new(obj.engine.orientation, obj.engine.position);
        self.scene.set_instance_matrix(Some(instance), &t);
        self.scene.update_instance(Some(instance), 0);
    }

    /// Starts (or stops) recording a system's simulation units per PSI.
    pub fn set_unit_trace(&mut self, system: u32, on: bool) {
        if let Some(world) = self.physics.world_mut(system) {
            world.set_unit_trace(on);
        }
    }

    /// A system's recorded simulation units: per PSI, in the order they
    /// ran, each unit's object as `units.jsonl` names it (`CSimple::Id`,
    /// 0 for a free part, and the `ObjectClass`) and whether the movement
    /// check came to it.
    #[must_use]
    pub fn unit_trace(&self, system: u32) -> Vec<Vec<(u32, u32, bool)>> {
        let Some(world) = self.physics.world(system) else {
            return Vec::new();
        };
        world
            .unit_trace()
            .iter()
            .map(|psi| {
                psi.iter()
                    .map(|&(key, check)| {
                        let h = ObjHandle::from_key(key);
                        let (id, class) = if h.generation() == crate::object::FREE_PART_GENERATION {
                            (0, crate::object::FREE_PART_CLASS)
                        } else {
                            self.objects
                                .get(h)
                                .map_or((0, 0), |o| (o.simple.id, o.kind.class()))
                        };
                        (id, class, check)
                    })
                    .collect()
            })
            .collect()
    }

    /// A system's recorded unit cores of the first PSIs
    /// ([`PhysicsWorld::unit_core_trace`]).
    #[must_use]
    pub fn unit_core_trace(&self, system: u32) -> Vec<Vec<neuron_core::physics::world::UnitCore>> {
        self.physics
            .world(system)
            .map(|w| w.unit_core_trace().to_vec())
            .unwrap_or_default()
    }

    /// The physics body of an object.
    #[must_use]
    pub fn body(&self, h: ObjHandle) -> Option<(u32, BodyHandle)> {
        let o = self.objects.get(h)?;
        Some((o.physical.system, o.physical.body?))
    }

    pub(crate) fn next_controller_key(&mut self) -> u64 {
        let k = self.next_controller;
        self.next_controller += 1;
        k
    }

    /// The world transform of an instance.
    #[must_use]
    pub fn instance_frame(&self, instance: Option<InstanceId>) -> (Matrix, Vector) {
        let t = self.scene.instance_transform(instance);
        (t.rotation, t.translation)
    }

    pub(crate) fn take_behavior(&mut self) -> Option<Box<dyn BehaviorHook>> {
        self.behavior.take()
    }

    pub(crate) fn put_behavior(&mut self, hook: Option<Box<dyn BehaviorHook>>) {
        if self.behavior.is_none() {
            self.behavior = hook;
        }
    }

    pub(crate) fn list(&self, system: u32, list: TickList) -> Vec<ObjHandle> {
        self.systems
            .get(&system)
            .and_then(|s| s.lists.get(&list))
            .cloned()
            .unwrap_or_default()
    }

    pub(crate) fn always_update(&self) -> &[ObjHandle] {
        &self.always_update
    }
}
