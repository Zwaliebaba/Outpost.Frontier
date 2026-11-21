//! The scenario oracle's records (`design/sim-records.md`): the JSON
//! reader moved to `game_sim::scenario::records` (decision O45); the
//! oracle folder and the float comparison of the replays stay here.

use std::path::PathBuf;

#[allow(unused_imports)]
pub use game_sim::scenario::records::*;

/// `OUTPOST_ORACLE_DIR`, or `None` with the notice printed.
pub fn oracle_dir(what: &str) -> Option<PathBuf> {
    match std::env::var_os("OUTPOST_ORACLE_DIR") {
        Some(d) => Some(PathBuf::from(d)),
        None => {
            println!("OUTPOST_ORACLE_DIR is not set: no {what} were replayed");
            None
        }
    }
}

/// Whether two floats agree within `tolerance` relative to the larger
/// magnitude (and at least absolute).
pub fn close(a: f32, b: f32, tolerance: f32) -> bool {
    if a == b {
        return true;
    }
    let scale = a.abs().max(b.abs()).max(1.0);
    (f64::from(a) - f64::from(b)).abs() <= f64::from(tolerance) * f64::from(scale)
}
