//! What the flight AI needs from the object world: the typed inspect and
//! command API of decision record 0001 point 8, the spatial queries, and
//! the random generator, as traits.
//!
//! The reference's AI reads objects through `IObjInspect` (85 slots),
//! commands its own ship through `IObjAffect` (30 slots), finds objects
//! through the object database (`IObjDB::FindById`, `GetNext`) and the
//! physics queries (`PhySys::FindRayCollisions`, `FindRayIntersect`,
//! `FindSphereCollisions`), and draws from the C runtime's `rand()`
//! (`include/common_iface.h`, `common_cobjdb.h`, `common_physys.h`).
//! Here:
//!
//! * [`ObjectView`] is the value an inspection returns: the slots the AI
//!   reads as plain fields, filled by the world in one call.
//! * [`AiWorld`] is the world as the AI sees it: inspect by handle, look up
//!   by id, walk a system's objects, attitudes, the motion model of a
//!   ship, and [`AiCommand`]s for the `IObjAffect` slots. P2.4's object
//!   world implements it with its generational handles; the replay of a
//!   recorded behaviour tick implements it over the recorded objects with
//!   object ids as handles.
//! * [`SpatialQueries`] is the physics side, implemented here for
//!   [`neuron_core::physics::PhysicsSystems`].
//! * [`AiRand`] is `rand()`: [`CrtRand`] in the game, [`RecordedRand`] in a
//!   replay that feeds the values the oracle saw.
//!
//! Every AI function takes these as arguments (decision T10: no globals),
//! so a behaviour tick is a function of the world, the manager's own state
//! and the random values.

use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::physics::PhysicsSystems;
use neuron_core::rng::CrtRand;

/// The C runtime's `rand()` as the AI consumes it.
pub trait AiRand {
    /// The next value, 0..=0x7fff.
    fn rand(&mut self) -> i32;
}

impl<R: AiRand + ?Sized> AiRand for &mut R {
    fn rand(&mut self) -> i32 {
        (**self).rand()
    }
}

impl AiRand for CrtRand {
    fn rand(&mut self) -> i32 {
        CrtRand::rand(self)
    }
}

/// `rand()` values recorded by the oracle, handed out in order. A draw past
/// the end answers 0 and is counted, so a replay can report that the port
/// drew more often than the reference.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RecordedRand {
    values: Vec<i32>,
    next: usize,
    overrun: usize,
}

impl RecordedRand {
    /// A source over `values`.
    #[must_use]
    pub fn new(values: Vec<i32>) -> Self {
        Self {
            values,
            next: 0,
            overrun: 0,
        }
    }

    /// How many recorded values were drawn.
    #[must_use]
    pub fn consumed(&self) -> usize {
        self.next
    }

    /// How many draws found no recorded value.
    #[must_use]
    pub fn overrun(&self) -> usize {
        self.overrun
    }

    /// Whether every recorded value was drawn and no more.
    #[must_use]
    pub fn exhausted_exactly(&self) -> bool {
        self.next == self.values.len() && self.overrun == 0
    }
}

impl AiRand for RecordedRand {
    fn rand(&mut self) -> i32 {
        if let Some(&value) = self.values.get(self.next) {
            self.next += 1;
            value
        } else {
            self.overrun += 1;
            0
        }
    }
}

/// `StrafeDir` (`include/common_cequip.h`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum StrafeDir {
    /// 0.
    #[default]
    None,
    /// 1.
    Left,
    /// 2.
    Right,
    /// 3.
    Up,
    /// 4.
    Down,
}

impl StrafeDir {
    /// The reference's value.
    #[must_use]
    pub fn value(self) -> i32 {
        match self {
            Self::None => 0,
            Self::Left => 1,
            Self::Right => 2,
            Self::Up => 3,
            Self::Down => 4,
        }
    }

    /// From the reference's value; anything else is `None`.
    #[must_use]
    pub fn from_value(value: i32) -> Self {
        match value {
            1 => Self::Left,
            2 => Self::Right,
            3 => Self::Up,
            4 => Self::Down,
            _ => Self::None,
        }
    }
}

/// The object class masks the AI tests (`CObject::ObjectClass`,
/// `(class & mask) == mask`).
pub mod class {
    /// `CSimple` and below.
    pub const SIMPLE: u32 = 0x03;
    /// An equipped object (`CEqObj`).
    pub const EQ_OBJ: u32 = 0x103;
    /// A ship.
    pub const SHIP: u32 = 0x503;
    /// A solar.
    pub const SOLAR: u32 = 0x303;
    /// Loot.
    pub const LOOT: u32 = 0x803;

    /// `(class & mask) == mask`.
    #[must_use]
    pub fn is(class: u32, mask: u32) -> bool {
        class & mask == mask
    }
}

/// One object as the AI reads it: the `IObjInspect` slots the behaviours,
/// ideas and helpers call, as values (slot numbers of
/// `include/common_iface.h` in brackets). Links to other objects are
/// object ids; the world turns them into handles with
/// [`AiWorld::handle_of`].
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ObjectView {
    /// `get_id` (8).
    pub id: u32,
    /// `CObject::ObjectClass` (through `cobject`, 84); see [`class`].
    pub class: u32,
    /// `get_type` (34): the type flags (`0x30000` ship-like and so on).
    pub type_flags: u32,
    /// `get_position` (0).
    pub position: Vector,
    /// `get_transform` / `get_orientation` (3, 4).
    pub orientation: Matrix,
    /// `get_velocity` (1).
    pub velocity: Vector,
    /// `get_angular_velocity` (2).
    pub angular_velocity: Vector,
    /// `get_physical_radius` (11): the radius.
    pub radius: f32,
    /// `get_physical_radius` (11): the centre.
    pub center: Vector,
    /// `get_center_of_mass` (5).
    pub center_of_mass: Vector,
    /// `get_archetype_extents` (10): `None` when it fails (the reference
    /// then zeroes both).
    pub extents: Option<(Vector, Vector)>,
    /// `get_mass` (12).
    pub mass: f32,
    /// `get_status` (15): hit points and their maximum.
    pub hit_points: (f32, f32),
    /// `get_shield_status` (17): current, maximum, active.
    pub shield: Option<(f32, f32, bool)>,
    /// `get_throttle` (19).
    pub throttle: Option<f32>,
    /// `get_axis_throttle` (20).
    pub axis_throttle: Option<Vector>,
    /// `get_nudge_vec` (21).
    pub nudge: Option<Vector>,
    /// `get_strafe_dir` (22).
    pub strafe: Option<StrafeDir>,
    /// `is_cruise_active` (23): cruise, trade lane.
    pub cruise: Option<(bool, bool)>,
    /// `are_thrusters_active` (25).
    pub thrusters: Option<bool>,
    /// `get_target` (29), an object id.
    pub target: Option<u32>,
    /// `get_subtarget` (30).
    pub subtarget: Option<u16>,
    /// `get_rank` (32).
    pub rank: Option<u32>,
    /// `get_affiliation` (33).
    pub affiliation: Option<u32>,
    /// `get_base` (35).
    pub base: Option<u32>,
    /// `get_dock_target` (36).
    pub dock_target: Option<u32>,
    /// `get_power` (37).
    pub power: Option<f32>,
    /// `get_zone_props` (38).
    pub zone_props: Option<u32>,
    /// `get_formation_leader` (44), an object id.
    pub formation_leader: Option<u32>,
    /// `get_formation_offset` (43).
    pub formation_offset: Option<Vector>,
    /// `get_follow_leader` (46), an object id.
    pub follow_leader: Option<u32>,
    /// `get_follow_offset` (45).
    pub follow_offset: Option<Vector>,
    /// `get_formation_followers` (83), object ids.
    pub formation_followers: Vec<u32>,
    /// `is_player` (47).
    pub is_player: bool,
    /// `is_dying` (14).
    pub is_dying: bool,
    /// `is_targetable` (13).
    pub is_targetable: bool,
    /// `get_toughness` (81).
    pub toughness: i32,
    /// `get_behavior_id` (82).
    pub behavior_id: i32,
    /// `get_max_bank_angle` (64).
    pub max_bank_angle: f32,
    /// `get_scanner_interference` (39).
    pub scanner_interference: f32,
    /// The first engine: `find_equipment(engine)` and
    /// `get_equipment_status` (65, 66), `None` without one.
    pub engine: Option<EngineStatus>,
}

/// An engine as `find_equipment` and `get_equipment_status` report it
/// (`EquipStatus` flag bits 0 and 1).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EngineStatus {
    /// The sub-object id.
    pub id: u16,
    /// `EquipStatusFlagActive`.
    pub active: bool,
    /// `EquipStatusFlagDestroyed`.
    pub destroyed: bool,
}

impl ObjectView {
    /// The transform `get_transform` answers.
    #[must_use]
    pub fn transform(&self) -> Transform {
        Transform::new(self.orientation, self.position)
    }

    /// `(class & mask) == mask`.
    #[must_use]
    pub fn is(&self, mask: u32) -> bool {
        class::is(self.class, mask)
    }
}

/// A thrust source the motion queries take (`IObject::ThrustEquipType`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ThrustEquipType {
    /// 0: no engine, the archetype's drag only.
    Normal,
    /// 1: the engine.
    Engine,
    /// 2: the engine and the thrusters.
    Thrusters,
    /// 3: a trade lane (the nudge force).
    Tradelane,
    /// 4: the cruise engine.
    Cruise,
}

impl ThrustEquipType {
    /// The reference's value.
    #[must_use]
    pub fn value(self) -> i32 {
        match self {
            Self::Normal => 0,
            Self::Engine => 1,
            Self::Thrusters => 2,
            Self::Tradelane => 3,
            Self::Cruise => 4,
        }
    }
}

/// The motion model of a ship (`IObjInspect` slots 50 to 64): how fast it
/// accelerates and turns with its archetype and equipment. Pure functions
/// of the ship's data; P2.4 implements them with the engine and steering
/// equipment.
pub trait MotionModel {
    /// `get_time_to_accelerate` (50).
    fn time_to_accelerate(&self, from: f32, to: f32, dt: f32, kind: ThrustEquipType) -> f32;
    /// `get_distance_travelled` (51).
    fn distance_travelled(&self, speed: f32, dt: f32, accel: f32, kind: ThrustEquipType) -> f32;
    /// `get_projected_throttle` (53).
    fn projected_throttle(&self, speed: f32, kind: ThrustEquipType) -> f32;
    /// `get_speed` (54).
    fn speed(&self, throttle: f32, kind: ThrustEquipType) -> f32;
    /// `get_initial_speed_to_coast_distance` (55).
    fn initial_speed_to_coast_distance(&self, speed: f32, stop: bool) -> f32;
    /// `get_time_to_accelerate_angularly` (56): about the roll axis.
    fn time_to_accelerate_angularly(&self, from: f32, to: f32, dt: f32) -> f32;
    /// `get_time_to_accelerate_angularly` with pitch and yaw throttles
    /// (57): `CShip`'s four-argument version; -1 where there is none.
    fn time_to_accelerate_angularly_xy(&self, pitch: f32, yaw: f32, from: f32, to: f32) -> f32 {
        let _ = (pitch, yaw, from, to);
        -1.0
    }
    /// `get_angular_distance_travelled` (58): about the roll axis.
    fn angular_distance_travelled(&self, from: f32, to: f32, dt: f32) -> f32;
    /// `get_angular_distance_travelled` with pitch and yaw throttles (59);
    /// -1 where there is none.
    fn angular_distance_travelled_xy(&self, pitch: f32, yaw: f32, from: f32, time: f32) -> f32 {
        let _ = (pitch, yaw, from, time);
        -1.0
    }
    /// `get_angular_speed_XY` (60).
    fn angular_speed_xy(&self, speed: f32, max_speed: f32) -> f32;
    /// `get_angular_speed_Z` (61).
    fn angular_speed_z(&self, speed: f32) -> f32;
    /// `get_projected_axis_throttle_XY` (62).
    fn projected_axis_throttle_xy(&self, speed: f32) -> f32;
    /// `get_projected_axis_throttle_Z` (63).
    fn projected_axis_throttle_z(&self, speed: f32) -> f32;
}

/// The `IObjAffect` calls the AI makes on its ship (slot numbers of
/// `include/common_iface.h`). `H` is the world's handle type. Slots whose
/// meaning the reconstruction has not named keep their slot number.
#[derive(Clone, Debug, PartialEq)]
pub enum AiCommand<H> {
    /// 0 `set_throttle`.
    SetThrottle(f32),
    /// 1 `set_axis_throttle`.
    SetAxisThrottle(Vector),
    /// 2 `set_nudge_vec`.
    SetNudge(Vector),
    /// 3 `set_strafe_dir`.
    SetStrafe(StrafeDir),
    /// 4 `set_formation_leader`.
    SetFormationLeader(Option<H>),
    /// 5 `set_formation_offset`.
    SetFormationOffset(Vector),
    /// 6 `add_formation_follower`.
    AddFormationFollower(H),
    /// 7 `remove_formation_follower`.
    RemoveFormationFollower(H),
    /// 9 `set_follow_leader`.
    SetFollowLeader(Option<H>),
    /// 10 `set_follow_offset`.
    SetFollowOffset(Vector),
    /// 11 `add_follow_follower`.
    AddFollowFollower(H),
    /// 12 `remove_follow_follower`.
    RemoveFollowFollower(H),
    /// 13 (`slot_34`): fire the listed guns, at a position or along their
    /// barrels.
    FireGuns {
        /// The equipment sub-object ids.
        equipment: Vec<u16>,
        /// The aim point.
        target: Option<Vector>,
    },
    /// 18 `set_target`.
    SetTarget {
        /// The target.
        target: Option<H>,
        /// Its sub-target.
        subtarget: u16,
    },
    /// 25 `go_tradelane`.
    GoTradeLane {
        /// The entrance ring.
        entrance: H,
        /// The exit ring.
        exit: H,
    },
    /// 19 `slot_4c`: activate or deactivate an equipment item.
    ActivateEquip {
        /// The sub-object id.
        equipment: u16,
        /// Activate.
        on: bool,
    },
    /// 20 `slot_50`: engage or leave cruise.
    EngageCruise {
        /// Engage.
        engage: bool,
        /// The second flag (the thrust manager passes false).
        instant: bool,
    },
    /// 21 `slot_54`: the thrusters on or off.
    SetThrustersActive(bool),
    /// 26 `stop_tradelane`.
    StopTradeLane,
    /// Any other slot, with its integer and float arguments in call order.
    Slot {
        /// The slot number.
        slot: u8,
        /// Integer and id arguments.
        ints: Vec<i64>,
        /// Float arguments.
        floats: Vec<f32>,
    },
}

impl<H> AiCommand<H> {
    /// The `IObjAffect` slot number.
    #[must_use]
    pub fn slot(&self) -> u8 {
        match self {
            Self::SetThrottle(_) => 0,
            Self::SetAxisThrottle(_) => 1,
            Self::SetNudge(_) => 2,
            Self::SetStrafe(_) => 3,
            Self::SetFormationLeader(_) => 4,
            Self::SetFormationOffset(_) => 5,
            Self::AddFormationFollower(_) => 6,
            Self::RemoveFormationFollower(_) => 7,
            Self::SetFollowLeader(_) => 9,
            Self::SetFollowOffset(_) => 10,
            Self::AddFollowFollower(_) => 11,
            Self::RemoveFollowFollower(_) => 12,
            Self::FireGuns { .. } => 13,
            Self::SetTarget { .. } => 18,
            Self::ActivateEquip { .. } => 19,
            Self::EngageCruise { .. } => 20,
            Self::SetThrustersActive(_) => 21,
            Self::GoTradeLane { .. } => 25,
            Self::StopTradeLane => 26,
            Self::Slot { slot, .. } => *slot,
        }
    }
}

/// The world as the AI sees it (decision record 0001 point 8). Reads take
/// `&self`; commands go through [`AiWorld::command`], so a replay can
/// record and compare them.
pub trait AiWorld {
    /// The world's object handle: P2.4's generational handle, or the
    /// object id in a replay.
    type Handle: Copy + Eq + core::fmt::Debug;

    /// `IObjInspect` of one object; `None` once it is gone.
    fn inspect(&self, h: Self::Handle) -> Option<ObjectView>;

    /// `IObjDB::FindById`.
    fn handle_of(&self, id: u32) -> Option<Self::Handle>;

    /// `IObjDB::GetNext` over one system: the objects whose class matches
    /// `class_mask` (`(class & mask) == mask`; 0 for all), in database
    /// order.
    fn system_objects(&self, system: u32, class_mask: u32) -> Vec<Self::Handle>;

    /// `get_attitude_towards` (26) of `from` toward `to`.
    fn attitude(&self, from: Self::Handle, to: Self::Handle) -> Option<f32>;

    /// `get_attitude_towards_symmetrical` (27): `from` toward `to` and the
    /// reverse.
    fn attitude_symmetrical(&self, from: Self::Handle, to: Self::Handle) -> Option<(f32, f32)>;

    /// `get_hardpoint` (48): a named hardpoint's position and orientation.
    fn hardpoint(&self, h: Self::Handle, name: &[u8]) -> Option<(Vector, Matrix)>;

    /// The motion model of a ship (slots 50 to 64); `None` for objects
    /// that have none.
    fn motion(&self, h: Self::Handle) -> Option<Box<dyn MotionModel + '_>>;

    /// The star system an object is in.
    fn system_of(&self, h: Self::Handle) -> Option<u32>;

    /// One `IObjAffect` call on `h`; the slot's integer result.
    fn command(&mut self, h: Self::Handle, command: AiCommand<Self::Handle>) -> i32;
}

/// One hit of a ray query, as the AI uses it (`PhySys::RayHit`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RayContact {
    /// The object id the body belongs to (the physics user key).
    pub object: u32,
    /// The hit point.
    pub point: Vector,
    /// The surface normal there.
    pub normal: Vector,
    /// The distance along the ray.
    pub distance: f32,
}

/// The physics queries the AI makes (`PhySys::FindRayCollisions`,
/// `FindRayIntersect`, `FindSphereCollisions`), keyed by star system and
/// object id as the reference keys them.
pub trait SpatialQueries {
    /// The nearest hit per body along `from` → `to`, nearest first, at
    /// most `max_hits`.
    fn find_ray_collisions(
        &self,
        system: u32,
        from: Vector,
        to: Vector,
        max_hits: usize,
    ) -> Vec<RayContact>;

    /// The ray against one object's body.
    fn find_ray_intersect(
        &self,
        system: u32,
        object: u32,
        from: Vector,
        to: Vector,
        max_hits: usize,
    ) -> Vec<RayContact>;

    /// The objects a sphere touches, at most `max_objects`.
    fn find_sphere_collisions(
        &self,
        system: u32,
        center: Vector,
        radius: f32,
        max_objects: usize,
    ) -> Vec<u32>;
}

/// The physics world's user key is the object id (`u64` holding a `u32`).
fn key_to_id(key: u64) -> u32 {
    u32::try_from(key & 0xffff_ffff).unwrap_or(0)
}

impl SpatialQueries for PhysicsSystems {
    fn find_ray_collisions(
        &self,
        system: u32,
        from: Vector,
        to: Vector,
        max_hits: usize,
    ) -> Vec<RayContact> {
        self.world(system)
            .map(|w| {
                w.find_ray_collisions(from, to, max_hits)
                    .into_iter()
                    .map(|h| RayContact {
                        object: key_to_id(h.user_key),
                        point: h.point,
                        normal: h.normal,
                        distance: h.distance,
                    })
                    .collect()
            })
            .unwrap_or_default()
    }

    fn find_ray_intersect(
        &self,
        system: u32,
        object: u32,
        from: Vector,
        to: Vector,
        max_hits: usize,
    ) -> Vec<RayContact> {
        let Some(world) = self.world(system) else {
            return Vec::new();
        };
        // The body whose user key is the object; the hits of every such
        // body (an object with attached parts has one).
        world
            .find_ray_collisions(from, to, usize::MAX)
            .into_iter()
            .filter(|h| key_to_id(h.user_key) == object)
            .flat_map(|h| world.find_ray_intersect(h.body, from, to, max_hits))
            .take(max_hits)
            .map(|h| RayContact {
                object: key_to_id(h.user_key),
                point: h.point,
                normal: h.normal,
                distance: h.distance,
            })
            .collect()
    }

    fn find_sphere_collisions(
        &self,
        system: u32,
        center: Vector,
        radius: f32,
        max_objects: usize,
    ) -> Vec<u32> {
        self.world(system)
            .map(|w| {
                w.find_sphere_collisions(center, radius, max_objects)
                    .into_iter()
                    .map(key_to_id)
                    .collect()
            })
            .unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recorded_rand_counts() {
        let mut r = RecordedRand::new(vec![5, 7]);
        assert_eq!(r.rand(), 5);
        assert!(!r.exhausted_exactly());
        assert_eq!(r.rand(), 7);
        assert!(r.exhausted_exactly());
        assert_eq!(r.rand(), 0);
        assert_eq!(r.overrun(), 1);
        let mut crt = CrtRand::default();
        assert_eq!(AiRand::rand(&mut crt), 41);
    }

    #[test]
    fn strafe_and_class() {
        assert_eq!(StrafeDir::from_value(3), StrafeDir::Up);
        assert_eq!(StrafeDir::Down.value(), 4);
        assert!(class::is(0x503, class::EQ_OBJ));
        assert!(!class::is(0x303, class::SHIP));
        assert_eq!(AiCommand::<u32>::StopTradeLane.slot(), 26);
    }

    #[test]
    fn queries_on_an_empty_system() {
        let systems = PhysicsSystems::new(neuron_core::physics::PhySysConstants::default());
        assert!(
            systems
                .find_ray_collisions(1, Vector::ZERO, Vector::new(0.0, 0.0, 10.0), 8)
                .is_empty()
        );
        assert!(
            systems
                .find_sphere_collisions(1, Vector::ZERO, 10.0, 8)
                .is_empty()
        );
    }
}
