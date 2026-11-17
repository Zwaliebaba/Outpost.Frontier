//! Flight AI (work package P2.5, `Common`'s AI: steering, intercepts, ray
//! casts, obstacle avoidance, pathfinding, targeting, fire control, the
//! behaviours, the ideas, the behaviour manager and the state graphs).
//!
//! The first increment holds the parts that need no object world:
//!
//! * [`raycast`]: the ray, box, capsule and sphere tests and the bearing
//!   angles (`common_airaycast.cpp`).
//! * [`steer`]: orientation bases, the random spread frame, the slerp
//!   toward a direction and `TurnHelper::get_angular_throttle`
//!   (`common_aisteer.cpp`).
//! * [`state_graph`]: `AI\state_graph.db`, the behaviour type names and the
//!   weighted behaviour choice (`common_pubstategraph.cpp`,
//!   `common_behaviortypes.cpp`, the re-evaluation of
//!   `UpdateBehaviorTick`).
//! * [`personality`]: `pub::AI::Personality` with its defaults and layout.
//! * [`world`]: the inspect, command and query traits the behaviours will
//!   use (decision record 0001 point 8) and the random source.
//!
//! Everything is computed on the 53-bit model of decision T11: `f64`
//! evaluation, a rounding to `f32` at each `float` store the reference
//! makes, no fused multiply-add.

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true
// exactly when the reference takes that side, which needs negated
// comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

mod fp;
pub mod personality;
pub mod raycast;
pub mod state_graph;
pub mod steer;
pub mod world;
