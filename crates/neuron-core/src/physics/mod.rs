//! Physics (work package P2.3): the `PhySys` facade of Common
//! (`CppSource/include/common_physys.h:446-614`) keyed by body handles,
//! the `[PhySys]` constants, free flight transliterated from IVP, contacts
//! through `parry3d` and the project's own solver, queries, phantoms,
//! remote correction and dead reckoning (decision O19).
//!
//! # Layers
//!
//! * [`ivp`]: the free flight in the reference's operation order on the
//!   53-bit model (T11): the core state, the PSI clock, damping, the
//!   controllers' push API and `calc_next_PSI_matrix`. One PSI can be
//!   driven from a recorded core state ([`replay`]).
//! * [`world`]: one star system: bodies built from compact surfaces,
//!   cores shared by attached bodies, the controller hook for the game's
//!   force controllers, golems, contacts, phantoms and queries.
//! * [`systems`]: the per-system map with `PhySys::Startup`, `Update` and
//!   the two stepping modes.
//! * [`damage`], [`dead_reckoning`], [`remote`], [`golem`]: the Common
//!   pieces that are exact ports (`CalcCollisionDamage`,
//!   `CDeadReckonedVector`, `CRemotePhysicsSimulation`, the golem
//!   controllers).
//!
//! Geometry crosses into `nalgebra` (through `parry3d`'s re-export) only in
//! [`shape`]; everything else uses [`crate::math`] types or IVP's own.
//!
//! # Differences from the reference
//!
//! Contacts are not IVP's: a linear shape cast per pair over the PSI finds
//! the first impact, and an impulse with the one material's elasticity
//! and friction resolves it (decision O19). Simulation units, the moving
//! set and pair checks run in creation order, not by address. Both are in
//! the divergence register (`design/divergences.md`).

// The reference's x87 comparisons send unordered operands (NaN) to one
// side (`TEST AH,0x41` and friends); the port spells each outcome as the
// comparison that is true exactly when the reference takes that side,
// which needs negated comparisons and `a < b || a > b`.
#![allow(clippy::neg_cmp_op_on_partial_ord, clippy::double_comparisons)]

mod arena;
pub mod constants;
pub mod damage;
pub mod dead_reckoning;
pub mod golem;
pub mod ivp;
pub mod remote;
pub mod replay;
pub mod shape;
pub mod systems;
pub mod world;

pub use arena::Index;
pub use constants::PhySysConstants;
pub use damage::{CollisionDamage, DEFAULT_COLLISION_DAMAGE_FACTOR, calc_collision_damage_scaled};
pub use dead_reckoning::DeadReckonedVector;
pub use golem::{Golem, GolemParms, RemotePhysicsController};
pub use remote::RemotePhysicsSimulation;
pub use shape::Geometry;
pub use systems::{PhysicsSystems, StepMode};
pub use world::step::{BeamQuery, PhantomDesc};
pub use world::{
    BodyDesc, BodyHandle, CONTROLLER_PRIORITY, ControllerContext, ControllerHandle, ControllerHost,
    CreateParms, DetachParms, NoControllers, PhantomEvent, PhantomHandle, PhysicsWorld, RayHit,
};

use crate::math::Vector;

/// One collision of a step (`PhySys::CollisionEvent`, 0x34 bytes in the
/// reference), as `GetCollisions` lists them.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CollisionEvent {
    /// +0x00 the first object's key.
    pub object_a: u64,
    /// +0x04 the second object's key (the beam for beam hits).
    pub object_b: u64,
    /// +0x08 the part of the first object that was hit.
    pub part_id_a: u32,
    /// +0x0c the part of the second.
    pub part_id_b: u32,
    /// +0x10 the contact point.
    pub point: Vector,
    /// +0x1c the contact surface's normal, pointing toward object A.
    pub surface_normal: Vector,
    /// +0x28 object A's velocity relative to object B at the contact (for
    /// a beam: the beam's velocity minus the surface speed).
    pub relative_velocity: Vector,
}
