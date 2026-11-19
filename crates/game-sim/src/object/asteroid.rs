//! Asteroids (`CppSource/Common/common_casteroidobj.cpp`): `CAsteroid`, the
//! static rocks an asteroid field places in its cubes, and
//! `CDynamicAsteroid`, the small free rocks.
//!
//! The field side (`CAsteroidField`, the cube walk that creates and drops
//! rocks around the ships, `CmnAsteroid::Update`) is [`super::field`]; the
//! world makes the rocks through [`crate::object::World::spawn_asteroid`]
//! and arms the field mines (the server's `MedMine`).

use neuron_core::math::Vector;
use neuron_core::physics::PhantomHandle;

/// `CAsteroid` (+0xe4..+0xfc).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AsteroidData {
    /// +0xe8 the rock id (`compute_asteroid_id`).
    pub asteroid_id: u32,
    /// +0xec the cube's id.
    pub cube_id: u32,
    /// +0xf0 a mine.
    pub mine: bool,
    /// +0xf1 kills on contact.
    pub instant_kill: bool,
    /// +0xf4 the physics system id (`set_system`).
    pub collision_group_id: u32,
    /// +0xf8 the owning field (an index into the system's fields).
    pub field: Option<u32>,
    /// The server lists it (`MedAsteroid` or `MedMine` in `ListC0`): from
    /// its cube's first change pass until the cube went far.
    pub listed: bool,
    /// A listed mine's server state (`MedMine`).
    pub armed: Option<MineArm>,
}

/// The server's `MedMine` (+0x64..+0x84): the detection phantom, the
/// intruders and the recharge.
#[derive(Clone, Debug, PartialEq)]
pub struct MineArm {
    /// +0x64 the detection phantom, merged into the rock's body.
    pub phantom: PhantomHandle,
    /// +0x68 the ships in the phantom.
    pub intruders: i32,
    /// +0x78 the explosion archetype.
    pub explosion: u32,
    /// +0x84 the explosion's offset along the direction.
    pub explosion_offset: f32,
    /// +0x80 the recharge time.
    pub recharge_time: f32,
    /// +0x7c the time since the last blast.
    pub recharge_timer: f32,
    /// +0x6c the direction from the mine to the last ship that entered.
    pub direction: Vector,
}

/// `CDynamicAsteroid`: no state beyond the object.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DynamicAsteroidData;

/// `CAsteroid::init_physics`'s body: mass 1000, damping 1e6 on every
/// axis, fixed, collision group "Asteroid".
pub const ASTEROID_BODY: (f32, f32, &[u8]) = (1000.0, 1.0e6, b"Asteroid");

/// `CDynamicAsteroid::init_physics`'s body: mass 1, linear damping 1e-5,
/// no angular damping, free, collision group "DynAst".
pub const DYNAMIC_ASTEROID_BODY: (f32, f32, &[u8]) = (1.0, 0.000_01, b"DynAst");
