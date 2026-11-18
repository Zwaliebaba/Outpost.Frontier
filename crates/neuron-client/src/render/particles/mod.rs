//! Particle effects at run time (work package P4.8, from
//! `CppSource/particlefx/`).
//!
//! `neuron_core::fx_data` loads the `.ale` files (P1.7); this module runs
//! them. The first increment is the simulation, platform-free:
//!
//! * [`ramp`]: evaluation of every keyed value (the flat ramps with their
//!   interpolator tables and evaluation cache, the animated Hermite tracks
//!   with loop types, the nested ramps, the colour ramps and the animated
//!   transform).
//! * [`ParticleFx`]: the node arena with the reference's reference counts,
//!   the node library of prototypes, the effect library, the eight
//!   appearance [`pool`]s, the random generator ([`rand`]), the tree and
//!   its walks: transform composition and the per-node update.
//! * The node families: emitters (shape update, emission, the cone, cube
//!   and sphere samplers), fields (gravity, air, radial, turbulence,
//!   collide, dust, beam) and appearances (spawn into the pools, texture
//!   frame, fields on every particle, integration).
//! * [`Effect`]: an instance of an effect archetype and the calls the client
//!   makes on it each frame.
//!
//! Drawing (the draw contexts, vertex building, material submission and
//! bounds) and the particle and mesh appearances' child effects and meshes
//! are the second increment. What the simulation asks of the texture
//! library goes through [`ParticleServices`].

// The reference's x87 branches treat an unordered comparison as one side of
// a test; the negated comparisons keep that.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

mod appearance;
mod effect;
mod emitter;
mod field;
pub mod node;
pub mod pool;
pub mod props;
pub mod ramp;
pub mod rand;
pub mod services;
mod system;

pub use effect::{Effect, EffectError, EffectRecord, OVERRIDE_FOLDER_HASH};
pub use node::{Family, Node, NodeId};
pub use pool::{Particle, PoolKind, Pools};
pub use services::{NoTextures, ParticleServices};
pub use system::{ParticleFx, pool_of};
