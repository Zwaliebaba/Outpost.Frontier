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
//! The second increment is the behaviour manager and what plugs into it:
//!
//! * [`manager`]: `IBehaviorManager` (`common_behaviormanager.cpp`): the
//!   fields, `UpdateBehaviorTick`, the transitions and the directives.
//! * [`behavior`]: the `Behavior` interface the 21 behaviours implement,
//!   the directive ops and the tick context ([`behavior::Cx`]).
//! * [`idea`]: the `BaseIdea` interface and the simple ideas
//!   (`common_idea.cpp`).
//! * [`thrust`]: the thrust manager the behaviours and ideas command the
//!   ship through (`common_thrustmanager.cpp`).
//! * [`behaviors`]: `CREATE_BEHAVIOR` and the ported behaviours.
//! * [`script`]: `ScriptBehavior`, a scripted behaviour record and its
//!   marshalling (`common_scriptbehavior.cpp`).
//!
//! `game_sim::object::World` implements the world traits
//! (`object::world::ai`) and keeps a manager per AI ship.
//!
//! Everything is computed on the 53-bit model of decision T11: `f64`
//! evaluation, a rounding to `f32` at each `float` store the reference
//! makes, no fused multiply-add.

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true
// exactly when the reference takes that side, which needs negated
// comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod behavior;
pub mod behaviors;
pub(crate) mod fp;
pub mod idea;
pub mod manager;
pub mod personality;
pub mod raycast;
pub mod script;
pub mod state_graph;
pub mod steer;
pub mod thrust;
pub mod world;

#[cfg(test)]
mod tests;
