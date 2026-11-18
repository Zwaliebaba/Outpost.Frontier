//! The image comparison of image replay (record 0004, point 14; Tier C).
//!
//! Both images are 8-bit RGB of the same size. A pixel's error is its
//! largest channel difference against the reference pixel. A pixel matches
//! when that error, or its error against one of the eight neighbours of the
//! reference pixel, is at most `t`; the neighbour rule absorbs edge coverage
//! that differs between rasterisers. A frame passes when at most `p` of its
//! pixels do not match and the mean error over all pixels is at most `m`.
//! The mean is of the direct error, without the neighbour rule, so a
//! one-pixel shift of the whole image still shows in it.

use neuron_core::image::RgbImage;

/// The thresholds of the comparison.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Thresholds {
    /// The largest channel difference a matching pixel may have.
    pub t: u8,
    /// The largest share of pixels that may not match, as a fraction.
    pub p: f64,
    /// The largest mean error over all pixels.
    pub m: f64,
}

/// The starting numbers, approved on 2026-11-18 and to be confirmed after
/// the first real replays, before Gate G4 is judged.
pub const STARTING: Thresholds = Thresholds {
    t: 8,
    p: 0.005,
    m: 1.5,
};

/// The outcome of a comparison.
#[derive(Clone, Debug, PartialEq)]
pub struct Comparison {
    pub pixels: u64,
    /// Pixels that match neither directly nor through a neighbour.
    pub mismatched: u64,
    /// The mean direct error.
    pub mean_error: f64,
    /// The largest direct error.
    pub max_error: u8,
    pub passed: bool,
    /// The difference image: grey for the direct error scaled by 8, red
    /// where a pixel does not match.
    pub difference: RgbImage,
}

/// Why two images could not be compared.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompareError {
    /// The images differ in size, or one is empty.
    Size {
        reference: (u32, u32),
        actual: (u32, u32),
    },
}

impl std::fmt::Display for CompareError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Size { reference, actual } => write!(
                f,
                "the reference is {}x{} and the image {}x{}",
                reference.0, reference.1, actual.0, actual.1
            ),
        }
    }
}

impl std::error::Error for CompareError {}

fn error(a: [u8; 3], b: [u8; 3]) -> u8 {
    (0..3).map(|c| a[c].abs_diff(b[c])).max().unwrap_or(0)
}

/// Compares `actual` with `reference`.
pub fn compare(
    reference: &RgbImage,
    actual: &RgbImage,
    limits: Thresholds,
) -> Result<Comparison, CompareError> {
    let (w, h) = (reference.width, reference.height);
    if (w, h) != (actual.width, actual.height) || w == 0 || h == 0 {
        return Err(CompareError::Size {
            reference: (w, h),
            actual: (actual.width, actual.height),
        });
    }
    let mut mismatched = 0u64;
    let mut total = 0u64;
    let mut max_error = 0u8;
    let mut difference = Vec::with_capacity(reference.rgb.len());
    for y in 0..h {
        for x in 0..w {
            let pixel = actual.pixel(x, y);
            let direct = error(reference.pixel(x, y), pixel);
            total += u64::from(direct);
            max_error = max_error.max(direct);
            let matches = direct <= limits.t || {
                let mut near = false;
                for dy in -1i64..=1 {
                    for dx in -1i64..=1 {
                        let (nx, ny) = (i64::from(x) + dx, i64::from(y) + dy);
                        if (dx, dy) != (0, 0)
                            && (0..i64::from(w)).contains(&nx)
                            && (0..i64::from(h)).contains(&ny)
                            && error(reference.pixel(nx as u32, ny as u32), pixel) <= limits.t
                        {
                            near = true;
                        }
                    }
                }
                near
            };
            if matches {
                let grey = direct.saturating_mul(8);
                difference.extend_from_slice(&[grey, grey, grey]);
            } else {
                mismatched += 1;
                difference.extend_from_slice(&[0xff, 0, 0]);
            }
        }
    }
    let pixels = u64::from(w) * u64::from(h);
    let mean_error = total as f64 / pixels as f64;
    let passed = mismatched as f64 <= limits.p * pixels as f64 && mean_error <= limits.m;
    Ok(Comparison {
        pixels,
        mismatched,
        mean_error,
        max_error,
        passed,
        difference: RgbImage {
            width: w,
            height: h,
            rgb: difference,
        },
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn image(w: u32, h: u32, f: impl Fn(u32, u32) -> [u8; 3]) -> RgbImage {
        let mut rgb = Vec::new();
        for y in 0..h {
            for x in 0..w {
                rgb.extend_from_slice(&f(x, y));
            }
        }
        RgbImage {
            width: w,
            height: h,
            rgb,
        }
    }

    #[test]
    fn equal_images_pass_with_no_error() {
        let a = image(4, 4, |x, y| [x as u8 * 50, y as u8 * 50, 7]);
        let c = compare(&a, &a, STARTING).unwrap();
        assert!(c.passed);
        assert_eq!((c.mismatched, c.max_error, c.mean_error), (0, 0, 0.0));
    }

    #[test]
    fn small_differences_match_and_large_ones_do_not() {
        let a = image(10, 10, |_, _| [100, 100, 100]);
        let b = image(10, 10, |_, _| [108, 92, 100]);
        let c = compare(&a, &b, STARTING).unwrap();
        assert_eq!(c.mismatched, 0);
        assert_eq!(c.max_error, 8);
        // Mean 8 is above m = 1.5: the frame fails on the mean alone.
        assert!(!c.passed);
        let b = image(10, 10, |x, y| {
            if (x, y) == (5, 5) {
                [0, 0, 0]
            } else {
                [100, 100, 100]
            }
        });
        let c = compare(&a, &b, STARTING).unwrap();
        // One pixel in a hundred: above p = 0.5 %.
        assert_eq!(c.mismatched, 1);
        assert!(!c.passed);
        assert_eq!(c.difference.pixel(5, 5), [0xff, 0, 0]);
        assert_eq!(c.difference.pixel(0, 0), [0, 0, 0]);
    }

    #[test]
    fn an_edge_one_pixel_over_matches_through_a_neighbour() {
        // A vertical edge at x = 10 in the reference and at x = 11 in the
        // image: the column between matches its left neighbour.
        let edge = |at: u32| move |x: u32, _| if x < at { [0, 0, 0] } else { [255, 255, 255] };
        let a = image(20, 20, edge(10));
        let b = image(20, 20, edge(11));
        let c = compare(&a, &b, STARTING).unwrap();
        assert_eq!(c.mismatched, 0);
        assert_eq!(c.max_error, 255);
        // 20 pixels of 255 over 400: a mean of 12.75 fails m.
        assert!(!c.passed);
        let c = compare(
            &a,
            &b,
            Thresholds {
                m: 13.0,
                ..STARTING
            },
        )
        .unwrap();
        assert!(c.passed);
    }

    #[test]
    fn sizes_must_agree() {
        let a = image(2, 2, |_, _| [0; 3]);
        let b = image(2, 3, |_, _| [0; 3]);
        assert_eq!(
            compare(&a, &b, STARTING),
            Err(CompareError::Size {
                reference: (2, 2),
                actual: (2, 3)
            })
        );
    }
}
