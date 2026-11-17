//! Small utilities from `Common` and `NeuronRuntime` (work package P1.5):
//! rectangles, the Catmull-Rom segment, state machines and the MD5 text
//! form.
//!
//! The C++ pool allocator (`Heap`), the synchronous work queue (`Async`)
//! and the critical-section wrapper (`MPCritSec`) are not ported. None of
//! them has an observable result: `Heap` recycles blocks (its 0xCD fill is
//! visible only through use after free), `Async::AddWork` runs the item at
//! once, and `MPCritSec` is a lock that single player skips. Rust
//! allocation, direct calls and owned or locked values replace them.

pub mod md5;
pub mod rect;
pub mod spline;
pub mod state_machine;
