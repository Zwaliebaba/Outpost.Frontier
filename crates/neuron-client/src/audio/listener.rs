//! The listener and the listener-relative pose of a 3D source: the
//! listener setters and getters of `FSoundManager` and the pose code of
//! `CreateInstanceFromKey`, `RefreshInstancePose` and `MixerAllowCreate`
//! (`CppSource/SoundManager/SoundManager.cpp`).
//!
//! The reference keeps every vector with its Z multiplied by the
//! coordinate scale, -1 when `rightHandedCoordinates` is set (the client
//! starts the device with that flag) and 1 otherwise; the getters
//! multiply again on the way out. The port keeps that representation so
//! the pan and distance arithmetic sees the same numbers.

use super::mix::{DEFAULT_MAX_DISTANCE, DEFAULT_MIN_DISTANCE};

/// The listener state of the manager (`Front` to `RolloffFactor`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Listener {
    /// The stored front vector: the negated front the client gave, Z
    /// scaled.
    pub front: [f32; 3],
    /// The stored up vector, Z scaled.
    pub up: [f32; 3],
    /// The stored position, Z scaled.
    pub position: [f32; 3],
    /// The stored velocity, Z scaled.
    pub velocity: [f32; 3],
    /// `SetDistanceFactor`.
    pub distance_factor: f32,
    /// `SetDopplerFactor`. Kept for the client; the software path does
    /// not use it.
    pub doppler_factor: f32,
    /// `SetRolloffFactor`.
    pub rolloff_factor: f32,
    /// `CoordScale`: -1 for right-handed coordinates, else 1.
    pub coord_scale: i32,
    /// Flags bit 0x10 (`rightHandedCoordinates`): picks the operand order
    /// of the right vector in [`Listener::orientation_matrix`].
    pub right_handed: bool,
}

impl Default for Listener {
    /// The constructor's values: front +Z, up +Y, factors 1, scale 1.
    fn default() -> Self {
        Listener {
            front: [0.0, 0.0, 1.0],
            up: [0.0, 1.0, 0.0],
            position: [0.0; 3],
            velocity: [0.0; 3],
            distance_factor: 1.0,
            doppler_factor: 1.0,
            rolloff_factor: 1.0,
            coord_scale: 1,
            right_handed: false,
        }
    }
}

/// `value * scale` stored as a float (an exact product rounded once).
fn scaled(value: f32, scale: f32) -> f32 {
    (f64::from(value) * f64::from(scale)) as f32
}

impl Listener {
    /// A default listener with the handedness `StartupDevice` reads:
    /// right-handed sets the scale to -1.
    #[must_use]
    pub fn with_handedness(right_handed: bool) -> Listener {
        Listener {
            coord_scale: if right_handed { -1 } else { 1 },
            right_handed,
            ..Listener::default()
        }
    }

    fn scale(&self) -> f32 {
        self.coord_scale as f32
    }

    /// `SetOrientation`: front negated, both Z components scaled.
    pub fn set_orientation(&mut self, front: [f32; 3], up: [f32; 3]) {
        let scale = self.scale();
        self.front = [-front[0], -front[1], scaled(-front[2], scale)];
        self.up = [up[0], up[1], scaled(up[2], scale)];
    }

    /// `SetOrientationFromMatrix`: column 2 of the 3x3 (row-major words)
    /// is the front, column 1 the up.
    pub fn set_orientation_from_matrix(&mut self, matrix: &[f32; 9]) {
        let up = [matrix[1], matrix[4], matrix[7]];
        let front = [matrix[2], matrix[5], matrix[8]];
        self.set_orientation(front, up);
    }

    /// `SetListenerPosition`: Z scaled.
    pub fn set_position(&mut self, position: [f32; 3]) {
        self.position = [position[0], position[1], scaled(position[2], self.scale())];
    }

    /// `SetListenerVelocity`: Z scaled.
    pub fn set_velocity(&mut self, velocity: [f32; 3]) {
        self.velocity = [velocity[0], velocity[1], scaled(velocity[2], self.scale())];
    }

    /// `GetListenerPosition`: the stored position with Z scaled again.
    #[must_use]
    pub fn get_position(&self) -> [f32; 3] {
        let p = self.position;
        [p[0], p[1], scaled(p[2], self.scale())]
    }

    /// `GetListenerVelocity`.
    #[must_use]
    pub fn get_velocity(&self) -> [f32; 3] {
        let v = self.velocity;
        [v[0], v[1], scaled(v[2], self.scale())]
    }

    /// `GetOrientationSpace`: the front as the client gave it (Z scaled
    /// back, all three negated back) and the up with Z scaled back.
    #[must_use]
    pub fn orientation_space(&self) -> ([f32; 3], [f32; 3]) {
        let scale = self.scale();
        let f = self.front;
        let front = [-f[0], -f[1], -scaled(f[2], scale)];
        let up = [self.up[0], self.up[1], scaled(self.up[2], scale)];
        (front, up)
    }

    /// `GetOrientationMatrix`: row-major words with the right vector in
    /// column 0 (its Z scaled), the up in column 1 and the front in column
    /// 2. The right vector comes from the stored vectors, each component a
    /// difference of two exact products rounded once.
    #[must_use]
    pub fn orientation_matrix(&self) -> [f32; 9] {
        let (space_front, space_up) = self.orientation_space();
        let [fx, fy, fz] = self.front.map(f64::from);
        let [ux, uy, uz] = self.up.map(f64::from);
        let right = if self.right_handed {
            [uz * fy - fz * uy, fz * ux - uz * fx, fx * uy - ux * fy]
        } else {
            [uy * fz - fy * uz, fx * uz - fz * ux, fy * ux - fx * uy]
        }
        .map(|v| v as f32);
        [
            right[0],
            space_up[0],
            space_front[0],
            right[1],
            space_up[1],
            space_front[1],
            (f64::from(self.coord_scale) * f64::from(right[2])) as f32,
            space_up[2],
            space_front[2],
        ]
    }

    /// The difference `point - position` as the pose code forms it: each
    /// component rounded to a float, Z first.
    fn offset(&self, point: [f32; 3]) -> [f32; 3] {
        let sub = |a: f32, b: f32| (f64::from(a) - f64::from(b)) as f32;
        let z = sub(point[2], self.position[2]);
        let y = sub(point[1], self.position[1]);
        let x = sub(point[0], self.position[0]);
        [x, y, z]
    }

    /// The pose of a 3D source (`CreateInstanceFromKey`,
    /// `RefreshInstancePose`). The position starts as the stored listener
    /// position and the distances as 10 and 500; whatever the source
    /// answers replaces them. Then the position's Z is scaled (also when
    /// it is still the listener's own), the squares are stored as floats
    /// and `dist_sq = (x*x + y*y) + z*z` is stored once.
    #[must_use]
    pub fn pose(
        &self,
        position: Option<[f32; 3]>,
        min_distance: Option<f32>,
        max_distance: Option<f32>,
    ) -> Pose {
        let mut position = position.unwrap_or(self.position);
        let min_distance = min_distance.unwrap_or(DEFAULT_MIN_DISTANCE);
        let max_distance = max_distance.unwrap_or(DEFAULT_MAX_DISTANCE);
        position[2] = scaled(position[2], self.scale());
        let direction = self.offset(position);
        let [x, y, z] = direction.map(f64::from);
        Pose {
            position,
            min_distance,
            max_distance,
            min_dist_sq: min_distance * min_distance,
            max_dist_sq: max_distance * max_distance,
            direction,
            dist_sq: ((x * x + y * y) + z * z) as f32,
        }
    }

    /// The range test of `MixerAllowCreate` for an archetype that asks for
    /// it: the source's position (Z scaled) against its maximum distance
    /// (the largest float when it answers none), compared unrounded as
    /// `max*max > (z*z + y*y) + x*x`; equal and NaN fail. The reference
    /// reads an uninitialised position when the source answers none; the
    /// port uses the origin (divergence D226).
    #[must_use]
    pub fn in_creation_range(&self, position: Option<[f32; 3]>, max_distance: Option<f32>) -> bool {
        let mut position = position.unwrap_or([0.0; 3]);
        position[2] = scaled(position[2], self.scale());
        let max = f64::from(max_distance.unwrap_or(f32::MAX));
        let [x, y, z] = self.offset(position).map(f64::from);
        let dist_sq = (z * z + y * y) + x * x;
        max * max > dist_sq
    }
}

/// A 3D source relative to the listener (the instance node's pose
/// fields).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Pose {
    /// The source position, Z scaled.
    pub position: [f32; 3],
    /// The minimum distance.
    pub min_distance: f32,
    /// The maximum distance.
    pub max_distance: f32,
    /// `min_distance` squared, stored as a float.
    pub min_dist_sq: f32,
    /// `max_distance` squared, stored as a float.
    pub max_dist_sq: f32,
    /// Listener to source. The software pan normalises it in place.
    pub direction: [f32; 3],
    /// The squared distance, stored as a float.
    pub dist_sq: f32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn right_handed_coordinates_negate_z_on_the_way_in_and_out() {
        let mut l = Listener::with_handedness(true);
        l.set_orientation([0.0, 0.0, 1.0], [0.0, 1.0, 0.0]);
        assert_eq!(l.front, [-0.0, -0.0, 1.0]);
        assert_eq!(l.up, [0.0, 1.0, -0.0]);
        l.set_position([1.0, 2.0, 3.0]);
        assert_eq!(l.position, [1.0, 2.0, -3.0]);
        assert_eq!(l.get_position(), [1.0, 2.0, 3.0]);
        let (front, up) = l.orientation_space();
        assert_eq!(front, [0.0, 0.0, 1.0]);
        assert_eq!(up, [0.0, 1.0, 0.0]);
    }

    #[test]
    fn orientation_matrix_columns() {
        let mut l = Listener::default();
        l.set_orientation([0.0, 0.0, 1.0], [0.0, 1.0, 0.0]);
        let m = l.orientation_matrix();
        // Stored front is -Z; right = (uy*fz - fy*uz, ...) = (-1, 0, 0).
        assert_eq!([m[0], m[3], m[6]], [-1.0, 0.0, 0.0]);
        assert_eq!([m[1], m[4], m[7]], [0.0, 1.0, 0.0]);
        assert_eq!([m[2], m[5], m[8]], [0.0, 0.0, 1.0]);
        let mut from_matrix = Listener::default();
        from_matrix.set_orientation_from_matrix(&[1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]);
        assert_eq!(from_matrix, l);
    }

    #[test]
    fn pose_defaults_and_squares() {
        let mut l = Listener::default();
        l.set_position([1.0, 1.0, 1.0]);
        let p = l.pose(Some([4.0, 5.0, 1.0]), None, None);
        assert_eq!(p.direction, [3.0, 4.0, 0.0]);
        assert_eq!(p.dist_sq, 25.0);
        assert_eq!((p.min_dist_sq, p.max_dist_sq), (100.0, 250000.0));
        // No position: the stored listener position, its Z scaled again.
        let mut r = Listener::with_handedness(true);
        r.set_position([0.0, 0.0, 2.0]);
        let p = r.pose(None, Some(1.0), Some(2.0));
        assert_eq!(p.position, [0.0, 0.0, 2.0]);
        assert_eq!(p.direction, [0.0, 0.0, 4.0]);
    }

    #[test]
    fn creation_range_is_strict() {
        let l = Listener::default();
        assert!(l.in_creation_range(Some([3.0, 4.0, 0.0]), Some(5.5)));
        assert!(!l.in_creation_range(Some([3.0, 4.0, 0.0]), Some(5.0)));
        assert!(!l.in_creation_range(Some([3.0, 4.0, 0.0]), Some(f32::NAN)));
        assert!(l.in_creation_range(Some([1.0e30, 0.0, 0.0]), None));
    }
}
