//! Particle effects at run time (work package P4.8, from
//! `CppSource/particlefx/`).
//!
//! `neuron_core::fx_data` loads the `.ale` files (P1.7); this module runs
//! and draws them. The first increment is the simulation, platform-free:
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
//! The second increment draws, against the renderer through
//! [`ParticleDraw`] and the camera of the draw walk ([`Camera`]):
//!
//! * [`draw`]: `BeginDraw`, `EndDraw` and the draw walk (`DrawNodes`), the
//!   six classes' draw contexts over the module's vertex buffer and the
//!   dynamic index buffer, the batch lists of the batcher, the counters.
//! * The vertex fills of the six billboard kinds: basic quads, streaks and
//!   triangles, rect, perp, oriented, the dust's streaks with the camera
//!   transform its spawns record, the beam's ribbon.
//! * Bounding spheres: each appearance's and the effect's merge
//!   ([`Effect::bounds`], `Slot15`).
//!
//! The third increment ports the two appearances that make no billboards
//! ([`child`]): the particle appearance, whose particles carry child
//! effects under the appearance node (the life effect along the particle's
//! life, turned along its velocity, and the death effect where it ends),
//! and the mesh appearance, whose particles carry meshes of the client's
//! mesh factory ([`FxMeshFactory`]).
//!
//! [`crate::render::stack::RenderStack`] answers both service traits on
//! the device model. What the simulation asks of the texture library and
//! the mesh factory goes through [`ParticleServices`].

// The reference's x87 branches treat an unordered comparison as one side of
// a test; the negated comparisons keep that.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

mod appearance;
mod bounds;
mod child;
pub mod draw;
mod effect;
mod emitter;
mod field;
mod fill;
pub mod node;
pub mod pool;
pub mod props;
pub mod ramp;
pub mod rand;
mod ribbon;
pub mod services;
mod system;

pub use bounds::NO_SPHERE;
pub use draw::{DrawContext, DrawState, PARTICLE_FVF, VERTEX_BYTES};
pub use effect::{Effect, EffectError, EffectRecord, OVERRIDE_FOLDER_HASH};
pub use fill::sin_cos;
pub use node::{Family, Node, NodeId};
pub use pool::{Particle, ParticleRecord, PoolKind, Pools};
pub use services::{Camera, FxMesh, FxMeshFactory, NoTextures, ParticleDraw, ParticleServices};
pub use system::{ParticleFx, pool_of};
