//! One star system's physics (`PhysicalSystem` with its `IVP_Environment`,
//! `CppSource/Common/common_physicalsystem.cpp`): bodies and their cores,
//! the PSI, controllers, contacts, phantoms and queries, keyed by handles
//! instead of `CObject` pointers (decision record 0001).

use std::collections::BTreeMap;

use super::arena::{Arena, Index};
use super::golem::{GOLEM_PRIORITY, Golem, GolemParms, RemotePhysicsController};
use super::ivp::core::Core;
use super::ivp::math::{IvpMatrix, IvpQuat, IvpTime, e, r};
use super::ivp::psi::{self, AnomalyLimits, EventSim};
use super::ivp::{
    IvpRandom, MOTION_IDLE_TIME, MovementCheck, PsiClock, initial_psi_time, revive_core,
    unit_psi_begin, unit_psi_end,
};
use super::shape::{Geometry, GeometryKind};
use super::{CollisionEvent, PhySysConstants};
use crate::math::{Matrix, Quaternion, Vector, matrix_to_quaternion, quaternion_to_matrix};

pub mod step;
#[cfg(test)]
mod tests;

/// A body: an `IVP_Real_Object` and the `CObject` it stands for.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BodyHandle(pub Index);

/// A phantom (`PhySys::CreatePhantom`'s handle).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PhantomHandle(pub Index);

/// A registered force controller (`PhySys::Controller::Register`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ControllerHandle(pub Index);

/// `PhySys::Controller`'s priority (`IVP_CP_MOTION`, 1500).
pub const CONTROLLER_PRIORITY: i32 = 1500;
/// The standard controller's priority (`IVP_CP_GRAVITY`, 1000).
pub const STANDARD_PRIORITY: i32 = 1000;

/// What a game force controller sees during a PSI
/// (`do_simulation_controller(IVP_Event_Sim*, cores)`), and the push API
/// it uses (`PhySys::Controller::push` and `rotate`).
pub struct ControllerContext<'a> {
    /// The PSI length and its reciprocal.
    pub event: EventSim,
    /// The environment's time at the start of this PSI.
    pub current_time: IvpTime,
    core: &'a mut Core,
}

impl ControllerContext<'_> {
    /// `PhySys::Controller::push`: `push_core_linear(force, dt)`.
    pub fn push(&mut self, force: Vector, dt: f32) {
        self.core.push_core_linear(force.to_words(), dt);
    }

    /// `PhySys::Controller::rotate`: `push_core_angular_ws(torque, dt)`.
    pub fn rotate(&mut self, torque: Vector, dt: f32) {
        self.core.push_core_angular_ws(torque.to_words(), dt);
    }

    /// The controlled core, read only.
    #[must_use]
    pub fn core(&self) -> &Core {
        self.core
    }
}

/// The game side of the controller hook: `game-sim` dispatches each
/// registered controller's key to its engine, thruster, steering and
/// other controllers (P2.4).
pub trait ControllerHost {
    /// One controller's `do_simulation_controller` for this PSI.
    fn do_simulation_controller(&mut self, key: u64, context: &mut ControllerContext<'_>);
}

/// A host without controllers.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoControllers;

impl ControllerHost for NoControllers {
    fn do_simulation_controller(&mut self, _key: u64, _context: &mut ControllerContext<'_>) {}
}

/// `PhySys::CreateParms`, without the fields that belong to the object
/// model (part walk, child type).
#[derive(Clone, Debug, PartialEq)]
pub struct CreateParms {
    /// +0x00 mass.
    pub mass: f32,
    /// +0x04 linear damping.
    pub linear_damping: f32,
    /// +0x08 angular damping.
    pub angular_damping: Vector,
    /// +0x1e the short name, which IVP's collision filter compares
    /// (`itoa` of the object id until `SetCollisionGroup`).
    pub name: Vec<u8>,
    /// +0x1d whether collisions start enabled.
    pub enable_collisions: bool,
    /// +0x26/+0x28 an explicit inertia.
    pub inertia: Option<Vector>,
}

/// A new body: geometry, parameters and world pose.
#[derive(Clone, Debug)]
pub struct BodyDesc {
    /// The `CObject` id or any key the caller uses.
    pub user_key: u64,
    /// The geometry.
    pub geometry: Geometry,
    /// The parameters.
    pub parms: CreateParms,
    /// The object's world position.
    pub position: Vector,
    /// The object's world orientation.
    pub orientation: Matrix,
    /// Outside single player the mass centre is pinned to the object's
    /// origin (`BuildPartIVP_Real`).
    pub pin_mass_center: bool,
}

/// `PhySys::DetachParms`.
#[derive(Clone, Debug, PartialEq)]
pub struct DetachParms {
    /// +0x00.
    pub linear_damping: f32,
    /// +0x04.
    pub angular_damping: Vector,
    /// +0x10, the new short name when not empty.
    pub name: Vec<u8>,
    /// +0x18.
    pub mass: f32,
    /// +0x1c/+0x20.
    pub inertia: Option<Vector>,
}

/// A phantom's enter or leave event (`CObjectCB::added` / `deleted`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PhantomEvent {
    /// The phantom.
    pub phantom: PhantomHandle,
    /// The intruder's key.
    pub user_key: u64,
    /// True when it entered, false when it left.
    pub entered: bool,
}

/// One hit of a ray query (`PhySys::RayHit`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RayHit {
    /// The body hit.
    pub body: BodyHandle,
    /// Its key.
    pub user_key: u64,
    /// The hit point.
    pub point: Vector,
    /// The surface normal there.
    pub normal: Vector,
    /// The hit ledge's part id.
    pub part_id: u32,
    /// The distance along the ray.
    pub distance: f32,
}

#[derive(Clone, Debug)]
enum GolemKind {
    Part(Golem),
    Remote(Box<RemotePhysicsController>),
}

#[derive(Clone, Debug)]
struct Body {
    core: Index,
    user_key: u64,
    geometry: Geometry,
    q_core_f_object: Option<IvpQuat>,
    shift_core_f_object: [f32; 3],
    collisions_enabled: bool,
    name: [u8; 8],
    detached_parts: std::collections::BTreeSet<u32>,
    single_contact: bool,
    golem: Option<GolemKind>,
    golem_order: u64,
    /// The class of the body's client object (`client_data->ObjectClass`),
    /// which a game collision filter reads; 0 until set.
    client_class: u32,
    /// A radius around the object's origin that holds every ledge: the
    /// contact pass skips a pair whose spheres cannot meet in the PSI.
    bound_radius: f32,
}

/// The radius around the object's origin that holds every ledge of a
/// geometry.
fn bound_radius(geometry: &Geometry) -> f32 {
    geometry
        .ledges
        .iter()
        .map(|l| {
            let s = l.shape.compute_local_bounding_sphere();
            s.center.coords.norm() + s.radius
        })
        .fold(0.0, f32::max)
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum ControllerKind {
    Standard,
    External { handle: ControllerHandle, key: u64 },
    Golem(BodyHandle),
}

#[derive(Clone, Debug)]
struct CoreSlot {
    core: Core,
    unit_state: u32,
    objects: Vec<BodyHandle>,
    /// The core's controllers in registration order (`IVP_Core::
    /// Controllers`).
    controllers: Vec<(i32, u64, ControllerKind)>,
    /// The simulation unit's controller buckets for this core
    /// (`IVP_Simulation_Unit::ControllerBuckets`): a registration appends
    /// one, an unregistration removes it, and a rebuild of the unit (a
    /// merge or split, [`PhysicsWorld::rebuild_controller_buckets`]) makes
    /// them the core's controllers from the last. The PSI runs them sorted
    /// by priority (stably), from the last.
    buckets: Vec<(i32, u64, ControllerKind)>,
    wake_pending: bool,
    simulation_disabled: bool,
}

#[derive(Clone, Debug)]
struct Phantom {
    geometry: Geometry,
    rotation: IvpQuat,
    position: [f32; 3],
    attached: Option<BodyHandle>,
    name: [u8; 8],
    enabled: bool,
    listener: bool,
    intruders: Vec<BodyHandle>,
}

fn short_name(name: &[u8]) -> [u8; 8] {
    // `safe_strcpy(name, 8, group)`: at most 7 bytes and a NUL.
    let mut out = [0u8; 8];
    for (i, b) in name.iter().take(7).enumerate() {
        if *b == 0 {
            break;
        }
        out[i] = *b;
    }
    out
}

/// `IVP_Collision_Filter_Coll_Group_Ident`: two objects with the same
/// non-empty short name do not collide.
fn names_collide(a: &[u8; 8], b: &[u8; 8]) -> bool {
    a[0] == 0 || b[0] == 0 || a != b
}

fn neg(v: [f32; 3]) -> [f32; 3] {
    [-v[0], -v[1], -v[2]]
}

fn cross(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

fn dot(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn matrix_from_ivp(m: &IvpMatrix) -> Matrix {
    Matrix::from_rows(m.rows)
}

/// A contact's value of the one material on both sides: the material's
/// `float` times itself, the product rounded to `float` (decision T11).
fn material_product(value: f64) -> f64 {
    let v = f64::from(value as f32);
    f64::from((v * v) as f32)
}

/// The physics of one star system.
#[derive(Clone, Debug)]
pub struct PhysicsWorld {
    system_id: u32,
    clock: PsiClock,
    movement_check: MovementCheck,
    limits: AnomalyLimits,
    friction: f64,
    elasticity: f64,
    min_time_between_collisions: f64,
    bullet_length_fudge: f32,
    bodies: Arena<Body>,
    cores: Arena<CoreSlot>,
    phantoms: Arena<Phantom>,
    controllers: Arena<BodyHandle>,
    moving: Vec<BodyHandle>,
    collisions: Vec<CollisionEvent>,
    phantom_events: Vec<PhantomEvent>,
    pair_last_collision: BTreeMap<(BodyHandle, BodyHandle), IvpTime>,
    order: u64,
    collision_filter: Option<fn(u32, u32) -> bool>,
    /// The manager's list of simulated units (`sim_units_head`), head
    /// first: a revived unit goes to the front, a unit that falls asleep
    /// leaves it (D88).
    active: Vec<Index>,
    /// The environment's wake queue (`event_queue`) in the order the cores
    /// were queued.
    wake_queue: Vec<Index>,
    /// Per PSI, the units in the order they ran: the key of each unit's
    /// first body and whether the movement check came to it (when
    /// [`PhysicsWorld::set_unit_trace`] turned it on).
    unit_trace: Option<Vec<Vec<(u64, bool)>>>,
    /// For the first [`UNIT_CORE_PSIS`] PSIs of the trace, each unit's core
    /// at the start and at the end of its PSI.
    unit_core_trace: Vec<Vec<(super::replay::CoreRecord, super::replay::CoreRecord)>>,
}

/// The PSIs [`PhysicsWorld::unit_core_trace`] keeps (`unit_cores.jsonl`'s
/// `kUnitCorePsis`).
pub const UNIT_CORE_PSIS: usize = 12;

/// The order `simulate_sim_units_psi` steps a unit list in: with the next
/// three units fetched ahead, all but the last three in list order, then
/// the last three from the last (`A, B, C, D, E` runs `A, B, E, D, C`).
#[must_use]
pub fn unit_run_order<T: Copy>(list: &[T]) -> Vec<T> {
    let n = list.len();
    let split = n.saturating_sub(3);
    let mut out = list[..split].to_vec();
    out.extend(list[split..].iter().rev());
    out
}

impl PhysicsWorld {
    /// `PhysicalSystem::AddRef`'s environment: zero gravity, the first PSI
    /// `1 / PHYSICAL_SIM_RATE`, the anomaly limits and the material from
    /// the constants.
    #[must_use]
    pub fn new(system_id: u32, constants: &PhySysConstants) -> Self {
        Self {
            system_id,
            clock: PsiClock::new(initial_psi_time(constants.physical_sim_rate)),
            movement_check: MovementCheck::default(),
            limits: constants.anomaly_limits(),
            // `IVP_Material_Manager::get_friction_factor` and `get_elasticity`
            // (`IVP/ivp_physics/ivp_material.cpp:44-58`): a contact's values are
            // the products of its two sides' materials, here both the one
            // `IVP_Material_Simple` of `PhySys` (`common_physicalsystem.cpp:613`),
            // its values stored as `float`.
            friction: material_product(constants.material_friction),
            elasticity: material_product(constants.material_elasticity),
            min_time_between_collisions: constants.min_time_between_collisions,
            bullet_length_fudge: constants.bullet_length_fudge,
            bodies: Arena::default(),
            cores: Arena::default(),
            phantoms: Arena::default(),
            controllers: Arena::default(),
            moving: Vec::new(),
            collisions: Vec::new(),
            phantom_events: Vec::new(),
            pair_last_collision: BTreeMap::new(),
            order: 0,
            collision_filter: None,
            active: Vec::new(),
            wake_queue: Vec::new(),
            unit_trace: None,
            unit_core_trace: Vec::new(),
        }
    }

    /// Starts (or stops) recording the simulation units of every PSI
    /// ([`PhysicsWorld::unit_trace`]).
    pub fn set_unit_trace(&mut self, on: bool) {
        self.unit_trace = on.then(Vec::new);
        self.unit_core_trace.clear();
    }

    /// Per PSI of the first [`UNIT_CORE_PSIS`] of the trace, each unit's
    /// core (and its unit flags) at the start and at the end of its PSI,
    /// before `calc_next_PSI_matrix`, in the order the units ran.
    #[must_use]
    pub fn unit_core_trace(
        &self,
    ) -> &[Vec<(super::replay::CoreRecord, super::replay::CoreRecord)>] {
        &self.unit_core_trace
    }

    /// The recorded simulation units: per PSI, each unit's first body's key
    /// and whether the movement check came to it.
    #[must_use]
    pub fn unit_trace(&self) -> &[Vec<(u64, bool)>] {
        self.unit_trace.as_deref().unwrap_or_default()
    }

    /// The simulated units now, head first, each as its first body's key.
    #[must_use]
    pub fn active_units(&self) -> Vec<u64> {
        self.active
            .iter()
            .filter_map(|c| self.unit_key(*c))
            .collect()
    }

    fn unit_key(&self, core: Index) -> Option<u64> {
        let slot = self.cores.get(core)?;
        let first = slot.objects.first()?;
        self.bodies.get(first.0).map(|b| b.user_key)
    }

    /// `IVP_Simulation_Unit_Revive_If_Sleeping` for a frozen core: revived
    /// now, its unit at the front of the simulated list.
    fn revive_now(&mut self, core: Index) {
        let clock = self.clock;
        let Some(slot) = self.cores.get_mut(core) else {
            return;
        };
        if slot.simulation_disabled || !slot.core.is_frozen() {
            return;
        }
        revive_core(&mut slot.core, &mut slot.unit_state, &clock);
        let objects = slot.objects.clone();
        for o in objects {
            if !self.moving.contains(&o) {
                self.moving.push(o);
            }
        }
        self.active.retain(|c| *c != core);
        self.active.insert(0, core);
    }

    /// The system id the world was started for.
    #[must_use]
    pub fn system_id(&self) -> u32 {
        self.system_id
    }

    /// The environment's movement-check countdown (`next_movement_check`,
    /// +0x118): the simulation-unit steps until the next unit checks its
    /// motion.
    #[must_use]
    pub fn movement_check(&self) -> i16 {
        self.movement_check.counter
    }

    /// The environment's clock.
    #[must_use]
    pub fn clock(&self) -> &PsiClock {
        &self.clock
    }

    /// `PhySys::GetCurrentTime`.
    #[must_use]
    pub fn current_time(&self) -> IvpTime {
        self.clock.current_time
    }

    /// `PhySys::m_pCollisionFilter`: a game filter asked about every pair
    /// the group filter lets through, given the two bodies' client classes
    /// ([`PhysicsWorld::set_client_class`]).
    pub fn set_collision_filter(&mut self, filter: Option<fn(u32, u32) -> bool>) {
        self.collision_filter = filter;
    }

    /// Sets the class of a body's client object (the `ObjectClass` of the
    /// `CObject` in `client_data`), which the game collision filter reads.
    pub fn set_client_class(&mut self, h: BodyHandle, class: u32) {
        if let Some(body) = self.bodies.get_mut(h.0) {
            body.client_class = class;
        }
    }

    fn next_order(&mut self) -> u64 {
        self.order += 1;
        self.order
    }

    fn body(&self, h: BodyHandle) -> Option<&Body> {
        self.bodies.get(h.0)
    }

    fn core_of(&self, h: BodyHandle) -> Option<&Core> {
        let body = self.bodies.get(h.0)?;
        self.cores.get(body.core).map(|s| &s.core)
    }

    fn core_of_mut(&mut self, h: BodyHandle) -> Option<&mut Core> {
        let core = self.bodies.get(h.0)?.core;
        self.cores.get_mut(core).map(|s| &mut s.core)
    }

    /// Whether a body still exists.
    #[must_use]
    pub fn contains(&self, h: BodyHandle) -> bool {
        self.bodies.get(h.0).is_some()
    }

    /// A body's key.
    #[must_use]
    pub fn user_key(&self, h: BodyHandle) -> Option<u64> {
        self.body(h).map(|b| b.user_key)
    }

    /// A body's core, read only (for replay and diagnostics).
    #[must_use]
    pub fn core(&self, h: BodyHandle) -> Option<&Core> {
        self.core_of(h)
    }

    /// `BuildIVP_Real` / `BuildIVP_Sphere` and IVP's object construction
    /// (`ivp_real_object.cpp:2370-2540`): a new core at rest, frozen until
    /// something wakes it.
    pub fn create_body(&mut self, desc: BodyDesc) -> BodyHandle {
        let parms = &desc.parms;
        let geometry = desc.geometry;
        let center = if desc.pin_mass_center || matches!(geometry.kind, GeometryKind::Ball(_)) {
            [0.0; 3]
        } else {
            geometry.mass_center
        };
        let (radius, deviation) = geometry.radius_and_deviation(center);
        let mut core = Core {
            upper_limit_radius: radius,
            max_surface_deviation: deviation,
            ..Core::default()
        };
        let mass_value = if !(parms.mass >= f32::from_bits(0x3b44_9ba6)) {
            1.0
        } else {
            parms.mass
        };
        let clamp_factor;
        if let Some(inertia) = parms.inertia {
            core.rot_inertia = inertia.to_words();
            clamp_factor = 0.0f32;
        } else {
            let scratch = match geometry.kind {
                GeometryKind::Polygon => geometry.rotation_inertia,
                GeometryKind::Ball(_) => {
                    let s = r(e(radius) * e(radius) * e(0.4f32));
                    [s; 3]
                }
            };
            let scaled_z = r(e(scratch[2]) * 1.0);
            core.rot_inertia = [
                r(e(scratch[0]) * 1.0 * e(mass_value)),
                r(e(scratch[1]) * 1.0 * e(mass_value)),
                r(e(scaled_z) * e(mass_value)),
            ];
            clamp_factor = f32::from_bits(0x3cf5_c28f);
        }
        if clamp_factor < 0.0 || clamp_factor > 0.0 {
            let i = core.rot_inertia.map(e);
            let threshold = ((i[0] * i[0] + i[1] * i[1]) + i[2] * i[2]).sqrt() * e(clamp_factor);
            for c in &mut core.rot_inertia {
                if threshold > e(*c) {
                    *c = r(threshold);
                }
            }
        }
        core.mass = mass_value;
        core.speed_damp_factor = parms.linear_damping;
        core.rot_speed_damp_factor = parms.angular_damping.to_words();
        core.recompute_inverse_inertia();

        let object_rotation = {
            let mut m = IvpMatrix {
                rows: desc.orientation.m,
                vv: desc.position.to_words(),
            };
            // `CreatePolygonBody` hands IVP the math engine's
            // `MatrixToQuaternion` of the orientation, which the core
            // normalises and turns into its matrix (`IVP_Core`'s
            // constructor); `init_world_transform_from_local_csys` then
            // multiplies the matrix by the local frame (an identity
            // rotation, exactly) and takes the core's quaternion from it.
            let mut q0 = IvpQuat::from_math(matrix_to_quaternion(&desc.orientation));
            q0.normize();
            q0.set_matrix(&mut m);
            let mut q = IvpQuat::IDENTITY;
            q.set_from_matrix(&m);
            (q, m)
        };
        let (q_object, m_object) = object_rotation;
        let core_position = m_object.vmult4(center);
        core.q_world_f_core_last_psi = q_object;
        core.q_world_f_core_next_psi = q_object;
        core.pos_world_f_core_last_psi = core_position;
        core.m_world_f_core_last_psi = m_object;
        core.m_world_f_core_last_psi.vv = core_position;
        // The motion checkpoints (`PosLastMotion`, `QLastMotion` and their
        // long-term pair) stay zero until the first movement check finds
        // the core moved, as in the reference, which never writes them
        // before (unit_cores.jsonl).
        core.q_last_motion = IvpQuat::from_array([0.0; 4]);
        core.q_last_long_motion = IvpQuat::from_array([0.0; 4]);
        core.pos_last_motion = [0.0; 3];
        core.pos_last_long_motion = [0.0; 3];
        core.time_of_last_psi = self.clock.current_time;
        let (_, inv) = self.clock.time_to_next_psi();
        core.synchronize_simulation_time_snapshot(inv);

        let bound = bound_radius(&geometry);
        let order = self.next_order();
        let core_index = self.cores.insert(CoreSlot {
            core,
            unit_state: super::ivp::core::MOVEMENT_FROZEN,
            objects: Vec::new(),
            controllers: vec![(STANDARD_PRIORITY, order, ControllerKind::Standard)],
            buckets: vec![(STANDARD_PRIORITY, order, ControllerKind::Standard)],
            wake_pending: false,
            simulation_disabled: false,
        });
        let handle = BodyHandle(self.bodies.insert(Body {
            core: core_index,
            user_key: desc.user_key,
            geometry,
            q_core_f_object: None,
            shift_core_f_object: neg(center),
            collisions_enabled: parms.enable_collisions,
            name: short_name(&parms.name),
            detached_parts: Default::default(),
            single_contact: false,
            golem: None,
            golem_order: 0,
            client_class: 0,
            bound_radius: bound,
        }));
        if let Some(slot) = self.cores.get_mut(core_index) {
            slot.objects.push(handle);
        }
        handle
    }

    /// Removes a body (`PhySys_DeleteBody`). Its core goes with its last
    /// object, and the controllers registered on that core with it.
    pub fn destroy_body(&mut self, h: BodyHandle) {
        let Some(body) = self.bodies.remove(h.0) else {
            return;
        };
        self.moving.retain(|m| *m != h);
        self.pair_last_collision
            .retain(|(a, b), _| *a != h && *b != h);
        let phantoms = self.phantoms.indices();
        for p in phantoms {
            if let Some(ph) = self.phantoms.get_mut(p) {
                if ph.attached == Some(h) {
                    ph.attached = None;
                }
                if let Some(pos) = ph.intruders.iter().position(|i| *i == h) {
                    ph.intruders.remove(pos);
                    if ph.listener {
                        self.phantom_events.push(PhantomEvent {
                            phantom: PhantomHandle(p),
                            user_key: body.user_key,
                            entered: false,
                        });
                    }
                }
            }
        }
        self.remove_object_from_core(body.core, h);
    }

    fn remove_object_from_core(&mut self, core: Index, h: BodyHandle) {
        let empty = if let Some(slot) = self.cores.get_mut(core) {
            slot.objects.retain(|o| *o != h);
            slot.controllers
                .retain(|(_, _, k)| !matches!(k, ControllerKind::Golem(b) if *b == h));
            slot.buckets
                .retain(|(_, _, k)| !matches!(k, ControllerKind::Golem(b) if *b == h));
            slot.objects.is_empty()
        } else {
            false
        };
        if empty && let Some(slot) = self.cores.remove(core) {
            self.active.retain(|c| *c != core);
            self.wake_queue.retain(|c| *c != core);
            for (_, _, kind) in slot.controllers {
                if let ControllerKind::External { handle, .. } = kind {
                    self.controllers.remove(handle.0);
                }
            }
        }
    }

    // ---- wake-up -----------------------------------------------------

    /// `IVP_Real_Object::EnsureInSimulation` (`PhySys::Wakeup`): a frozen
    /// core is queued for revival at the next PSI; a moving one restarts
    /// its motion clock.
    pub fn wakeup(&mut self, h: BodyHandle) {
        let now = self.clock.current_time;
        let Some(core) = self.bodies.get(h.0).map(|b| b.core) else {
            return;
        };
        if let Some(slot) = self.cores.get_mut(core) {
            if slot.core.movement_state & 0xFF == super::ivp::core::MOVEMENT_FROZEN {
                // `IVP_Environment_Enqueue_Core`: once until the queue runs.
                if !slot.wake_pending {
                    slot.wake_pending = true;
                    self.wake_queue.push(core);
                }
            } else {
                slot.core.update_time_of_last_motion(now);
            }
        }
    }

    // ---- mass, damping, velocity, impulses (common_physys_query.cpp) ----

    /// `PhySys::SetMass`: only the mass changes; the inertia is kept.
    pub fn set_mass(&mut self, h: BodyHandle, mass: f32) {
        if let Some(core) = self.core_of_mut(h) {
            let saved = core.rot_inertia;
            core.set_mass(mass);
            core.set_rotation_inertia(saved);
        }
    }

    /// `PhySys::GetMass`.
    #[must_use]
    pub fn mass(&self, h: BodyHandle) -> Option<f32> {
        self.core_of(h).map(|c| c.mass)
    }

    /// `PhySys::SetMomentOfInertia`.
    pub fn set_moment_of_inertia(&mut self, h: BodyHandle, inertia: Vector) {
        if let Some(core) = self.core_of_mut(h) {
            core.set_rotation_inertia(inertia.to_words());
        }
    }

    /// `PhySys::GetMomentOfInertia`.
    #[must_use]
    pub fn moment_of_inertia(&self, h: BodyHandle) -> Option<Vector> {
        self.core_of(h).map(|c| Vector::from_words(c.rot_inertia))
    }

    /// `PhySys::GetDamping`.
    #[must_use]
    pub fn damping(&self, h: BodyHandle) -> Option<(f32, Vector)> {
        self.core_of(h).map(|c| {
            (
                c.speed_damp_factor,
                Vector::from_words(c.rot_speed_damp_factor),
            )
        })
    }

    /// `PhySys::SetDamping`.
    pub fn set_damping(&mut self, h: BodyHandle, linear: f32, angular: Vector) {
        if let Some(core) = self.core_of_mut(h) {
            core.speed_damp_factor = linear;
            core.rot_speed_damp_factor = angular.to_words();
        }
    }

    /// `PhySys::GetVelocity`: outside single player a remotely corrected
    /// body reports its dead reckoning's velocity.
    #[must_use]
    pub fn velocity(&self, h: BodyHandle, single_player: bool) -> Vector {
        let Some(body) = self.body(h) else {
            return Vector::ZERO;
        };
        if !single_player && let Some(GolemKind::Remote(rc)) = &body.golem {
            return rc.simulation.velocity;
        }
        self.core_of(h)
            .map_or(Vector::ZERO, |c| Vector::from_words(c.speed))
    }

    /// `PhySys::AddToVelocity`.
    pub fn add_to_velocity(&mut self, h: BodyHandle, delta: Vector) {
        self.wakeup(h);
        if let Some(core) = self.core_of_mut(h) {
            core.add_speed_change(delta.to_words());
        }
    }

    /// `PhySys::GetAngularVelocityOS`.
    #[must_use]
    pub fn angular_velocity_os(&self, h: BodyHandle) -> Vector {
        self.core_of(h)
            .map_or(Vector::ZERO, |c| Vector::from_words(c.rot_speed))
    }

    /// `PhySys::AddToAngularVelocityOS`.
    pub fn add_to_angular_velocity_os(&mut self, h: BodyHandle, delta: Vector) {
        self.wakeup(h);
        if let Some(core) = self.core_of_mut(h) {
            core.add_rot_speed_change(delta.to_words());
        }
    }

    /// `PhySys::AddImpulseAtPoint` (`async_push_object_ws`).
    pub fn add_impulse_at_point(&mut self, h: BodyHandle, impulse: Vector, point: Vector) {
        self.wakeup(h);
        let now = self.clock.current_time;
        if let Some(core) = self.core_of_mut(h) {
            let m = core.calc_interpolated_transform(now);
            let position_cs = m.vimult4(point.to_words());
            let impulse_cs = m.vimult(impulse.to_words());
            core.apply_impulse_at_point(position_cs, impulse_cs, impulse.to_words());
        }
    }

    /// `PhySys::LinearImpulse` (`push_core_linear`, no wake-up).
    pub fn linear_impulse(&mut self, h: BodyHandle, direction: Vector, magnitude: f32) {
        if let Some(core) = self.core_of_mut(h) {
            core.push_core_linear(direction.to_words(), magnitude);
        }
    }

    /// `PhySys::AngularImpulse` (`push_core_angular_ws`).
    pub fn angular_impulse(&mut self, h: BodyHandle, axis: Vector, magnitude: f32) {
        if let Some(core) = self.core_of_mut(h) {
            core.push_core_angular_ws(axis.to_words(), magnitude);
        }
    }

    /// `PhySys::AngularImpulseOS` (`push_core_angular_cs`).
    pub fn angular_impulse_os(&mut self, h: BodyHandle, axis: Vector, magnitude: f32) {
        if let Some(core) = self.core_of_mut(h) {
            core.push_core_angular_cs(axis.to_words(), magnitude);
        }
    }

    // ---- collision state and groups ------------------------------------

    /// `PhySys::GetCollisionState`.
    #[must_use]
    pub fn collision_state(&self, h: BodyHandle) -> bool {
        self.body(h).is_some_and(|b| b.collisions_enabled)
    }

    /// `PhySys::SetCollisionState`.
    pub fn set_collision_state(&mut self, h: BodyHandle, enable: bool) {
        if let Some(body) = self.bodies.get_mut(h.0) {
            body.collisions_enabled = enable;
        }
    }

    /// `PhySys::GetCollisionGroup`: the short name.
    #[must_use]
    pub fn collision_group(&self, h: BodyHandle) -> Vec<u8> {
        self.body(h).map_or_else(Vec::new, |b| {
            b.name.iter().copied().take_while(|c| *c != 0).collect()
        })
    }

    /// `PhySys::SetCollisionGroup`: at most 7 bytes are kept.
    pub fn set_collision_group(&mut self, h: BodyHandle, group: &[u8]) {
        if let Some(body) = self.bodies.get_mut(h.0) {
            body.name = short_name(group);
        }
    }

    /// The part ids a body's ledges carry, sorted, each once.
    #[must_use]
    pub fn ledge_part_ids(&self, h: BodyHandle) -> Vec<u32> {
        let mut ids: Vec<u32> = self
            .body(h)
            .map(|b| b.geometry.ledges.iter().map(|l| l.part_id).collect())
            .unwrap_or_default();
        ids.sort_unstable();
        ids.dedup();
        ids
    }

    /// Marks a part attached or detached; the ledges of a detached part
    /// take no part in contacts and queries (`PhySurfaceManager`).
    pub fn set_part_attached(&mut self, h: BodyHandle, part_id: u32, attached: bool) {
        if let Some(body) = self.bodies.get_mut(h.0) {
            if attached {
                body.detached_parts.remove(&part_id);
            } else {
                body.detached_parts.insert(part_id);
            }
        }
    }

    /// Projectiles (classes with the three low bits set) leave the
    /// simulation after their first contact (`event_pre_collision`).
    pub fn set_single_contact(&mut self, h: BodyHandle, single: bool) {
        if let Some(body) = self.bodies.get_mut(h.0) {
            body.single_contact = single;
        }
    }

    /// `PhySys::DeactivatePhysics`: the body's core leaves the simulation.
    pub fn deactivate(&mut self, h: BodyHandle) {
        let Some(core) = self.bodies.get(h.0).map(|b| b.core) else {
            return;
        };
        if let Some(slot) = self.cores.get_mut(core) {
            slot.simulation_disabled = true;
            slot.core.stop_movement();
            let objects = slot.objects.clone();
            self.moving.retain(|m| !objects.contains(m));
            self.active.retain(|c| *c != core);
        }
    }

    // ---- poses ---------------------------------------------------------

    fn object_pose_at(&self, h: BodyHandle, now: IvpTime) -> Option<(IvpQuat, [f32; 3])> {
        let body = self.body(h)?;
        let core = &self.cores.get(body.core)?.core;
        let m = core.calc_interpolated_transform(now);
        let delta = now.since(core.time_of_last_psi);
        let q_core = if core.is_frozen() || !(delta < 0.0 || delta > 0.0) {
            core.q_world_f_core_last_psi
        } else {
            let alpha = r(e(r(delta)) * e(core.i_delta_time));
            let mut q = IvpQuat::IDENTITY;
            q.set_interpolate_quat(
                &core.q_world_f_core_last_psi,
                &core.q_world_f_core_next_psi,
                alpha,
            );
            q
        };
        let q = match body.q_core_f_object {
            Some(qc) => IvpQuat::mult(&q_core, &qc),
            None => q_core,
        };
        Some((q, m.vmult4(body.shift_core_f_object)))
    }

    /// The object's world position and orientation now (`calc_at_time_AT`,
    /// what `PhySys_SyncObjectFromBody` copies into the `CObject`).
    #[must_use]
    pub fn transform(&self, h: BodyHandle) -> Option<(Vector, Matrix)> {
        let (q, p) = self.object_pose_at(h, self.clock.current_time)?;
        Some((Vector::from_words(p), quaternion_to_matrix(&q.to_math())))
    }

    /// `PhySys::GetCenterOfMass`: the core's position now.
    #[must_use]
    pub fn center_of_mass(&self, h: BodyHandle) -> Option<Vector> {
        let core = self.core_of(h)?;
        Some(Vector::from_words(
            core.calc_interpolated_transform(self.clock.current_time).vv,
        ))
    }

    /// The bodies whose cores move, in the order they started moving (the
    /// reference's `MovingObjects` set is ordered by address; divergence
    /// register).
    #[must_use]
    pub fn moving_objects(&self) -> &[BodyHandle] {
        &self.moving
    }

    /// `PhySys::MovingObjectCount`.
    #[must_use]
    pub fn moving_object_count(&self) -> usize {
        self.moving.len()
    }

    fn beam_body(&mut self, h: BodyHandle, rotation: IvpQuat, position: [f32; 3], optimize: bool) {
        let Some(body) = self.bodies.get(h.0) else {
            return;
        };
        let mut m = IvpMatrix::IDENTITY;
        rotation.set_matrix(&mut m);
        m.vv = position;
        let core_position = m.vmult4(neg(body.shift_core_f_object));
        let q_core = match body.q_core_f_object {
            Some(qc) => IvpQuat::mult_inv(&rotation, &qc),
            None => rotation,
        };
        let clock = self.clock.beam_clock();
        let core = body.core;
        if let Some(slot) = self.cores.get_mut(core) {
            psi::set_transformation(&mut slot.core, q_core, core_position, optimize, &clock);
        }
    }

    /// `PhySys::BeamR` for the body itself: put it at a pose, at rest.
    pub fn beam(&mut self, h: BodyHandle, position: Vector, orientation: &Matrix, optimize: bool) {
        let q = IvpQuat::from_math(matrix_to_quaternion(orientation));
        self.beam_body(h, q, position.to_words(), optimize);
    }

    // ---- controllers -----------------------------------------------------

    /// `PhySys::Controller::Register`: the controller acts on the body's
    /// core from the next PSI, and wakes it.
    pub fn register_controller(
        &mut self,
        h: BodyHandle,
        key: u64,
        priority: i32,
    ) -> Option<ControllerHandle> {
        let core = self.bodies.get(h.0)?.core;
        let handle = ControllerHandle(self.controllers.insert(h));
        let order = self.next_order();
        if let Some(slot) = self.cores.get_mut(core) {
            let entry = (priority, order, ControllerKind::External { handle, key });
            slot.controllers.push(entry);
            slot.buckets.push(entry);
        }
        // `IVP_Controller_Wake_Or_Refresh_Associated_Sim_Unit`: a sleeping
        // unit is revived at once (to the front of the simulated list), an
        // awake one restarts its motion clock.
        let now = self.clock.current_time;
        let frozen = self
            .cores
            .get(core)
            .is_some_and(|s| s.core.is_frozen() && !s.simulation_disabled);
        if frozen {
            self.revive_now(core);
        } else if let Some(slot) = self.cores.get_mut(core)
            && !slot.core.is_frozen()
        {
            slot.core.update_time_of_last_motion(now);
        }
        Some(handle)
    }

    /// `IVP_Simulation_Unit_Rebuild_Controller_Buckets` for the unit of a
    /// body's core: its buckets become the core's controllers from the
    /// last (IVP builds them walking `Core->Controllers` backwards), which
    /// reverses the PSI's order within a priority. IVP rebuilds a unit when
    /// it merges with another or splits (a friction system between two
    /// moving cores does both).
    pub fn rebuild_controller_buckets(&mut self, h: BodyHandle) {
        let Some(core) = self.bodies.get(h.0).map(|b| b.core) else {
            return;
        };
        if let Some(slot) = self.cores.get_mut(core) {
            slot.buckets = slot.controllers.iter().rev().copied().collect();
        }
    }

    /// The order the PSI runs a body's controllers in: the keys of its
    /// registered controllers (the standard and golem controllers left
    /// out).
    #[must_use]
    pub fn controller_run_order(&self, h: BodyHandle) -> Vec<u64> {
        let Some(slot) = self.bodies.get(h.0).and_then(|b| self.cores.get(b.core)) else {
            return Vec::new();
        };
        let mut buckets = slot.buckets.clone();
        buckets.sort_by_key(|(priority, _, _)| *priority);
        buckets
            .iter()
            .rev()
            .filter_map(|(_, _, k)| match k {
                ControllerKind::External { key, .. } => Some(*key),
                _ => None,
            })
            .collect()
    }

    /// `PhySys::Controller::UnRegister`.
    pub fn unregister_controller(&mut self, handle: ControllerHandle) {
        if self.controllers.remove(handle.0).is_none() {
            return;
        }
        for index in self.cores.indices() {
            if let Some(slot) = self.cores.get_mut(index) {
                let other = |k: &ControllerKind| !matches!(k, ControllerKind::External { handle: c, .. } if *c == handle);
                slot.controllers.retain(|(_, _, k)| other(k));
                slot.buckets.retain(|(_, _, k)| other(k));
            }
        }
    }

    fn set_golem(&mut self, h: BodyHandle, golem: Option<GolemKind>) {
        let order = self.next_order();
        let Some(body) = self.bodies.get_mut(h.0) else {
            return;
        };
        let had = body.golem.is_some();
        body.golem = golem;
        body.golem_order = order;
        let core = body.core;
        let has = body.golem.is_some();
        if let Some(slot) = self.cores.get_mut(core) {
            if !had && has {
                let entry = (GOLEM_PRIORITY, order, ControllerKind::Golem(h));
                slot.controllers.push(entry);
                slot.buckets.push(entry);
            } else if had && !has {
                let other = |k: &ControllerKind| !matches!(k, ControllerKind::Golem(b) if *b == h);
                slot.controllers.retain(|(_, _, k)| other(k));
                slot.buckets.retain(|(_, _, k)| other(k));
            }
        }
    }

    /// `AttachGolemController` mode 1: a golem holding the body at its
    /// current pose.
    pub fn attach_golem(&mut self, h: BodyHandle, parms: &GolemParms) {
        let now = self.clock.current_time;
        let Some((q, p)) = self.object_pose_at(h, now) else {
            return;
        };
        let mut golem = Golem::new(parms);
        golem.set_target_position(Vector::from_words(p), Vector::ZERO, now);
        golem.set_target_quaternion(q, now, None, 0.0);
        self.set_golem(h, Some(GolemKind::Part(golem)));
        self.wakeup(h);
    }

    /// `AttachGolemController` mode 1 as the reference calls it for a free
    /// part: the golem holds the body at the object's frame (`position`
    /// and `quat`, the math engine's `MatrixToQuaternion` of its
    /// orientation, in that engine's order), its speed zero, both stamped
    /// with the current time; then the core is queued for the simulation
    /// (`set_target_*` end in `ensure_core_to_be_in_simulation`).
    pub fn attach_golem_at(
        &mut self,
        h: BodyHandle,
        parms: &GolemParms,
        position: Vector,
        quat: Quaternion,
    ) {
        let now = self.clock.current_time;
        let mut golem = Golem::new(parms);
        golem.set_target_position(position, Vector::ZERO, now);
        golem.set_target_quaternion(IvpQuat::from_math(quat), now, None, 0.0);
        self.set_golem(h, Some(GolemKind::Part(golem)));
        self.wakeup(h);
    }

    /// `PhySys::GolemSetTargetPosition`.
    pub fn golem_set_target_position(
        &mut self,
        h: BodyHandle,
        position: Vector,
        speed: Vector,
        time: IvpTime,
    ) {
        if let Some(Body {
            golem: Some(GolemKind::Part(g)),
            ..
        }) = self.bodies.get_mut(h.0)
        {
            g.set_target_position(position, speed, time);
        }
        self.wakeup(h);
    }

    /// `PhySys::GolemSetTargetQuaternion` (`quat` in the math engine's
    /// order).
    pub fn golem_set_target_quaternion(
        &mut self,
        h: BodyHandle,
        quat: Quaternion,
        time: IvpTime,
        rot_quat: Option<Quaternion>,
        delta_time: f32,
    ) {
        if let Some(Body {
            golem: Some(GolemKind::Part(g)),
            ..
        }) = self.bodies.get_mut(h.0)
        {
            g.set_target_quaternion(
                IvpQuat::from_math(quat),
                time,
                rot_quat.map(IvpQuat::from_math),
                delta_time,
            );
        }
        self.wakeup(h);
    }

    /// `PhySys::RCBegin`: remote correction from a first sample. The
    /// angular velocity is not used, as in the reference.
    pub fn rc_begin(
        &mut self,
        h: BodyHandle,
        constants: &PhySysConstants,
        position: Vector,
        velocity: Vector,
        orientation: Quaternion,
        _angular_velocity: Vector,
    ) {
        let rc = RemotePhysicsController::new(
            &GolemParms::for_remote_correction(constants),
            position,
            orientation,
            velocity,
        );
        self.set_golem(h, Some(GolemKind::Remote(Box::new(rc))));
        self.wakeup(h);
    }

    /// `PhySys::RCSet`: a further sample; nothing without `rc_begin`.
    pub fn rc_set(&mut self, h: BodyHandle, position: Vector, orientation: Quaternion, time: f64) {
        let mut found = false;
        if let Some(Body {
            golem: Some(GolemKind::Remote(rc)),
            ..
        }) = self.bodies.get_mut(h.0)
        {
            rc.remote_update(position, orientation, time);
            found = true;
        }
        if found {
            self.wakeup(h);
        }
    }

    /// `PhySys::RCEnd`: the controller is deleted.
    pub fn rc_end(&mut self, h: BodyHandle) {
        if matches!(
            self.body(h).and_then(|b| b.golem.as_ref()),
            Some(GolemKind::Remote(_))
        ) {
            self.set_golem(h, None);
        }
    }

    /// The remote correction's dead reckoning, if active.
    #[must_use]
    pub fn remote_simulation(
        &self,
        h: BodyHandle,
    ) -> Option<&super::remote::RemotePhysicsSimulation> {
        match self.body(h)?.golem.as_ref()? {
            GolemKind::Remote(rc) => Some(&rc.simulation),
            GolemKind::Part(_) => None,
        }
    }

    // ---- attach and detach (compound rebuilds) ---------------------------

    /// `PhySys::Attach` (`IVP_Merge_Object_Into_Core`): the child joins the
    /// parent's core at its current pose. The core's mass and inertia stay
    /// the parent's; its radius grows by the distance between the cores.
    pub fn attach(&mut self, parent: BodyHandle, child: BodyHandle) {
        let now = self.clock.current_time;
        let (Some(pb), Some(cb)) = (self.body(parent), self.body(child)) else {
            return;
        };
        let (host_core, old_core) = (pb.core, cb.core);
        if host_core == old_core {
            return;
        }
        let Some((child_q, child_p)) = self.object_pose_at(child, now) else {
            return;
        };
        let (Some(host), Some(old)) = (self.cores.get(host_core), self.cores.get(old_core)) else {
            return;
        };
        let extrapolate = |c: &Core| {
            let dt = r(now.since(c.time_of_last_psi));
            [0, 1, 2].map(|i| {
                r(e(c.pos_world_f_core_last_psi[i]) + e(c.delta_world_f_core_psis[i]) * e(dt))
            })
        };
        let (a, b) = (extrapolate(&host.core), extrapolate(&old.core));
        let d = [0, 1, 2].map(|i| f64::from(a[i]) - f64::from(b[i]));
        let distance = r(((d[2] * d[2] + d[1] * d[1]) + d[0] * d[0]).sqrt());
        let (old_ulr, old_msd) = (old.core.upper_limit_radius, old.core.max_surface_deviation);
        let host_frozen = host.core.is_frozen();
        self.remove_object_from_core(old_core, child);
        self.moving.retain(|m| *m != child);
        let Some(host) = self.cores.get_mut(host_core) else {
            return;
        };
        let radius = e(distance) + e(old_ulr);
        if radius > e(host.core.upper_limit_radius) {
            host.core.upper_limit_radius = r(radius);
        }
        let limit = e(distance) + e(old_msd);
        if limit > e(host.core.max_surface_deviation) {
            host.core.max_surface_deviation = r(limit);
        }
        host.objects.push(child);
        let core_m = host.core.calc_interpolated_transform(now);
        let mut core_q = IvpQuat::IDENTITY;
        core_q.set_from_matrix(&core_m);
        host.core.recompute_inverse_inertia();
        let shift = core_m.vimult4(child_p);
        let q_core_f_object = IvpQuat::inv_mult(&core_q, &child_q);
        if let Some(body) = self.bodies.get_mut(child.0) {
            body.core = host_core;
            body.shift_core_f_object = shift;
            body.q_core_f_object = Some(q_core_f_object);
        }
        if !host_frozen {
            self.moving.push(child);
        }
    }

    /// `PhySys::Detach` (`IVP_Split_Object_From_Core`): the body gets a core
    /// of its own with the given parameters, moving with the old core's
    /// surface speed at its position and the old core's spin. The body's
    /// handle changes; the new one is returned.
    pub fn detach(&mut self, h: BodyHandle, parms: &DetachParms) -> Option<BodyHandle> {
        let now = self.clock.current_time;
        let body = self.body(h)?;
        let old_core = body.core;
        let slot = self.cores.get(old_core)?;
        if slot.objects.len() <= 1 {
            return Some(h);
        }
        let (q, p) = self.object_pose_at(h, now)?;
        let surface_speed = slot.core.surface_speed(p);
        let rot_speed = slot.core.rot_speed;
        let geometry = body.geometry.clone();
        let user_key = body.user_key;
        let collisions_enabled = body.collisions_enabled;
        let name = if parms.name.is_empty() {
            body.name.to_vec()
        } else {
            parms.name.clone()
        };
        let detached = body.detached_parts.clone();
        self.remove_object_from_core(old_core, h);
        self.moving.retain(|m| *m != h);
        if let Some(slot) = self.cores.get_mut(old_core) {
            slot.core.recompute_inverse_inertia();
        }
        let _ = self.bodies.remove(h.0);
        let mut m = IvpMatrix::IDENTITY;
        q.set_matrix(&mut m);
        let new = self.create_body(BodyDesc {
            user_key,
            geometry,
            parms: CreateParms {
                mass: parms.mass,
                linear_damping: parms.linear_damping,
                angular_damping: parms.angular_damping,
                name,
                enable_collisions: collisions_enabled,
                inertia: parms.inertia,
            },
            position: Vector::from_words(p),
            orientation: matrix_from_ivp(&m),
            pin_mass_center: false,
        });
        // The arena reuses the freed slot, so the handle stays the same
        // slot with a new generation; callers take the returned handle.
        if let Some(body) = self.bodies.get_mut(new.0) {
            body.detached_parts = detached;
        }
        if let Some(core) = self.core_of_mut(new) {
            core.add_speed_change(surface_speed);
            core.add_rot_speed_change(rot_speed);
        }
        self.wakeup(new);
        Some(new)
    }
}
