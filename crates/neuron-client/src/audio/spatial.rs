//! What the device hands X3DAudio for a voice of a 3D archetype (owner
//! decision O26, divergence D220): the listener's frame, the emitter, the
//! distance curve and the Doppler inputs, derived from the mixer's
//! [`Listener`] and [`Emitter`] without platform code.
//!
//! The reference set these on DirectSound 3D buffers, whose model the port
//! keeps where X3DAudio has a counterpart (divergence D273):
//!
//! - Vectors are the stored ones, already in the left-handed frame both
//!   DirectSound 3D and X3DAudio use (the listener's Z scale did that). The
//!   stored front and up go in as the listener's front and top, made
//!   orthonormal as X3DAudio requires.
//! - Minimum and maximum distance are clamped to `1..=1e9` as
//!   `ClampDsDistance` clamps them. The volume curve is DirectSound 3D's
//!   rolloff law, `min / (min + rolloff * (d - min))` beyond the minimum
//!   distance, full volume inside it and no further attenuation beyond the
//!   maximum, sampled as a piecewise-linear X3DAudio curve over
//!   `0..=max`.
//! - Velocities are in units per second; the distance factor (metres per
//!   unit) turns them into metres per second for X3DAudio's speed of sound,
//!   and the Doppler factor scales the shift.

use super::listener::Listener;
use super::mixer::Emitter;

/// X3DAudio's speed of sound in metres per second
/// (`X3DAUDIO_SPEED_OF_SOUND`).
pub const SPEED_OF_SOUND: f32 = 343.5;

/// `ClampDsDistance`'s limits.
pub const DISTANCE_RANGE: (f32, f32) = (1.0, 1.0e9);

/// The points of the volume curve between the minimum and the maximum
/// distance (both included).
pub const CURVE_SEGMENTS: usize = 16;

/// An orthonormal listener frame.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Frame {
    /// `OrientFront`.
    pub front: [f32; 3],
    /// `OrientTop`.
    pub top: [f32; 3],
}

fn dot(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn normalised(v: [f64; 3]) -> Option<[f64; 3]> {
    let len = dot(v, v).sqrt();
    (len.is_finite() && len > 1.0e-12).then(|| v.map(|c| c / len))
}

/// The listener's front and top for X3DAudio: the stored front
/// normalised, the stored up with its component along the front removed
/// and normalised. A front or up of no length, or an up along the front,
/// gives the default frame (front +Z, top +Y).
#[must_use]
pub fn listener_frame(listener: &Listener) -> Frame {
    let default = Frame {
        front: [0.0, 0.0, 1.0],
        top: [0.0, 1.0, 0.0],
    };
    let Some(front) = normalised(listener.front.map(f64::from)) else {
        return default;
    };
    let up = listener.up.map(f64::from);
    let along = dot(up, front);
    let Some(top) = normalised([
        up[0] - front[0] * along,
        up[1] - front[1] * along,
        up[2] - front[2] * along,
    ]) else {
        return default;
    };
    Frame {
        front: front.map(|c| c as f32),
        top: top.map(|c| c as f32),
    }
}

/// `ClampDsDistance`; NaN takes the minimum.
#[must_use]
pub fn clamp_distance(value: f32) -> f32 {
    if value.is_nan() {
        return DISTANCE_RANGE.0;
    }
    value.clamp(DISTANCE_RANGE.0, DISTANCE_RANGE.1)
}

/// DirectSound 3D's rolloff law: the amplitude at `distance` for a source
/// of `min` and `max` distance and the listener's `rolloff` factor.
#[must_use]
pub fn rolloff_gain(distance: f32, min: f32, max: f32, rolloff: f32) -> f32 {
    let d = f64::from(distance.min(max));
    let min = f64::from(min);
    if d <= min {
        return 1.0;
    }
    let rolloff = f64::from(rolloff.max(0.0));
    (min / (min + rolloff * (d - min))) as f32
}

/// One point of an X3DAudio distance curve
/// (`X3DAUDIO_DISTANCE_CURVE_POINT`): a distance normalised to the curve's
/// scaler and the gain there.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CurvePoint {
    /// Normalised distance, `0..=1`.
    pub distance: f32,
    /// The gain.
    pub gain: f32,
}

/// The volume curve and its distance scaler (`CurveDistanceScaler`) for
/// an emitter: full volume to the minimum distance, then the rolloff law
/// at [`CURVE_SEGMENTS`] distances spaced evenly in their logarithm up to
/// the maximum; beyond the scaler X3DAudio keeps the last gain.
#[must_use]
pub fn volume_curve(min: f32, max: f32, rolloff: f32) -> (f32, Vec<CurvePoint>) {
    let min = clamp_distance(min);
    let max = clamp_distance(max);
    if max <= min {
        let flat = vec![
            CurvePoint {
                distance: 0.0,
                gain: 1.0,
            },
            CurvePoint {
                distance: 1.0,
                gain: 1.0,
            },
        ];
        return (max, flat);
    }
    let mut points = Vec::with_capacity(CURVE_SEGMENTS + 2);
    points.push(CurvePoint {
        distance: 0.0,
        gain: 1.0,
    });
    let ratio = f64::from(max) / f64::from(min);
    for i in 0..=CURVE_SEGMENTS {
        let d = if i == CURVE_SEGMENTS {
            f64::from(max)
        } else {
            f64::from(min) * ratio.powf(i as f64 / CURVE_SEGMENTS as f64)
        };
        let distance = if i == CURVE_SEGMENTS {
            1.0
        } else {
            (d / f64::from(max)) as f32
        };
        points.push(CurvePoint {
            distance,
            gain: rolloff_gain(d as f32, min, max, rolloff),
        });
    }
    (max, points)
}

/// The emitter X3DAudio receives, apart from the curve.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EmitterInputs {
    /// `Position`.
    pub position: [f32; 3],
    /// `Velocity`, metres per second.
    pub velocity: [f32; 3],
    /// `DopplerScaler`.
    pub doppler_scaler: f32,
}

/// The listener X3DAudio receives.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ListenerInputs {
    /// `OrientFront` and `OrientTop`.
    pub frame: Frame,
    /// `Position`.
    pub position: [f32; 3],
    /// `Velocity`, metres per second.
    pub velocity: [f32; 3],
}

/// A velocity in units per second as metres per second.
fn metres_per_second(velocity: [f32; 3], distance_factor: f32) -> [f32; 3] {
    let factor = if distance_factor.is_finite() {
        f64::from(distance_factor)
    } else {
        1.0
    };
    velocity.map(|v| (f64::from(v) * factor) as f32)
}

/// The listener inputs.
#[must_use]
pub fn listener_inputs(listener: &Listener) -> ListenerInputs {
    ListenerInputs {
        frame: listener_frame(listener),
        position: listener.position,
        velocity: metres_per_second(listener.velocity, listener.distance_factor),
    }
}

/// The emitter inputs.
#[must_use]
pub fn emitter_inputs(listener: &Listener, emitter: &Emitter) -> EmitterInputs {
    let doppler = listener.doppler_factor;
    EmitterInputs {
        position: emitter.position,
        velocity: metres_per_second(emitter.velocity, listener.distance_factor),
        doppler_scaler: if doppler.is_finite() {
            doppler.max(0.0)
        } else {
            0.0
        },
    }
}

/// A voice's frequency ratio: its pitch ratio times the Doppler factor,
/// within what the voice was created for. A non-finite product plays at
/// the pitch ratio.
#[must_use]
pub fn voice_ratio(pitch_ratio: f32, doppler: f32, max_ratio: f32) -> f32 {
    let r = f64::from(pitch_ratio) * f64::from(doppler);
    let r = if r.is_finite() && r > 0.0 {
        r
    } else {
        f64::from(pitch_ratio)
    };
    (r as f32).clamp(1.0 / 1024.0, max_ratio)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_frame_is_orthonormal() {
        let mut l = Listener {
            front: [0.0, 0.0, 2.0],
            up: [0.0, 1.0, 1.0],
            ..Listener::default()
        };
        let f = listener_frame(&l);
        assert_eq!(f.front, [0.0, 0.0, 1.0]);
        assert_eq!(f.top, [0.0, 1.0, 0.0]);
        l.up = [0.0, 0.0, 3.0];
        assert_eq!(listener_frame(&l).top, [0.0, 1.0, 0.0]);
        l.front = [0.0; 3];
        assert_eq!(listener_frame(&l).front, [0.0, 0.0, 1.0]);
    }

    #[test]
    fn the_curve_follows_the_rolloff_law() {
        assert_eq!(rolloff_gain(5.0, 10.0, 100.0, 1.0), 1.0);
        assert_eq!(rolloff_gain(20.0, 10.0, 100.0, 1.0), 0.5);
        assert_eq!(rolloff_gain(1000.0, 10.0, 100.0, 1.0), 0.1);
        assert_eq!(rolloff_gain(20.0, 10.0, 100.0, 0.0), 1.0);
        let (scaler, points) = volume_curve(10.0, 100.0, 1.0);
        assert_eq!(scaler, 100.0);
        assert_eq!(points.len(), CURVE_SEGMENTS + 2);
        assert_eq!(
            points[0],
            CurvePoint {
                distance: 0.0,
                gain: 1.0
            }
        );
        assert_eq!(points[1].gain, 1.0);
        assert!((points[1].distance - 0.1).abs() < 1e-6);
        assert_eq!(
            points.last().copied(),
            Some(CurvePoint {
                distance: 1.0,
                gain: 0.1
            })
        );
        assert!(points.windows(2).all(|w| w[0].distance < w[1].distance));
        assert!(points.windows(2).all(|w| w[0].gain >= w[1].gain));
        // Clamped as DirectSound clamps, and flat when max is not beyond min.
        let (scaler, points) = volume_curve(0.0, f32::NAN, 1.0);
        assert_eq!((scaler, points.len()), (1.0, 2));
        assert_eq!(clamp_distance(2.0e9), 1.0e9);
    }

    #[test]
    fn doppler_inputs() {
        let l = Listener {
            velocity: [1.0, 2.0, 3.0],
            distance_factor: 2.0,
            doppler_factor: 0.1,
            ..Listener::default()
        };
        assert_eq!(listener_inputs(&l).velocity, [2.0, 4.0, 6.0]);
        let e = Emitter {
            position: [1.0, 0.0, 0.0],
            velocity: [0.5, 0.0, 0.0],
            min_distance: 10.0,
            max_distance: 100.0,
            mute_at_max_distance: false,
        };
        let i = emitter_inputs(&l, &e);
        assert_eq!((i.velocity, i.doppler_scaler), ([1.0, 0.0, 0.0], 0.1));
        assert_eq!(voice_ratio(2.0, 1.5, 4.0), 3.0);
        assert_eq!(voice_ratio(2.0, 3.0, 4.0), 4.0);
        assert_eq!(voice_ratio(2.0, f32::NAN, 4.0), 2.0);
    }
}
