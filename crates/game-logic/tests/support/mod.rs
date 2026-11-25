//! Helpers shared by game-logic's oracle comparisons.
//!
//! The Tier B bound of decision T11 for positions the reference turns with
//! the x87 `FSIN` and `FCOS` and the port with Rust's `sin` and `cos`. A
//! zone's orientation goes through them, so game-sim's data dump compares
//! it within this bound (`crates/game-sim/tests/data_dump.rs`); a patrol
//! path's points are its zones' ends, moved along that orientation, and
//! `content_loaders.rs` compares them within the same bound. The
//! assassination target's position comes from `random_inside_ring`, which
//! turns its angle the same way (D879), and `random_missions.rs` compares
//! it within the bound too.

/// The bound in units in the last place.
pub const POINT_ULPS: u32 = 4;
/// The bound in value.
pub const POINT_ABSOLUTE: f32 = 1.0e-7;

/// Two float bit patterns within [`POINT_ULPS`] or [`POINT_ABSOLUTE`].
pub fn within_bound(a: u32, b: u32) -> bool {
    let (x, y) = (f32::from_bits(a), f32::from_bits(b));
    if !x.is_finite() || !y.is_finite() {
        return a == b;
    }
    if (f64::from(x) - f64::from(y)).abs() <= f64::from(POINT_ABSOLUTE) {
        return true;
    }
    // Ordered integers of the two floats; equal signs only.
    let order = |v: u32| {
        if v & 0x8000_0000 != 0 {
            -i64::from(v & 0x7fff_ffff)
        } else {
            i64::from(v)
        }
    };
    (order(a) - order(b)).unsigned_abs() <= u64::from(POINT_ULPS)
}

/// Whether two lists of float bits (eight hex digits each, separated by `,`
/// or `:`) have the same length and are pairwise within the bound. A token
/// that is not eight hex digits fails.
pub fn floats_within_bound(oracle: &str, rust: &str) -> bool {
    let floats = |text: &str| -> Option<Vec<u32>> {
        text.split([',', ':'])
            .map(|t| {
                if t.len() == 8 {
                    u32::from_str_radix(t, 16).ok()
                } else {
                    None
                }
            })
            .collect()
    };
    match (floats(oracle), floats(rust)) {
        (Some(x), Some(y)) => {
            x.len() == y.len() && x.iter().zip(&y).all(|(p, q)| within_bound(*p, *q))
        }
        _ => false,
    }
}
