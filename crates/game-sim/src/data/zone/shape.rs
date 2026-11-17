//! The zone shape queries (`common_zone.cpp`): membership, distance, the
//! radius along a direction and the surface point, in the reference's
//! operation order. The zone's frame goes through the `x86math` kernels of
//! `neuron-core::math`, as the C++ goes through the 3DMathEngine.

use neuron_core::math::{
    Transform, Vector, transform_matrix, transform_xform, transpose_transform,
};

use super::{Zone, ZoneShape};
use crate::data::not_below;

impl Zone {
    /// The zone's transform (`ZoneFrame`'s first 12 words).
    fn transform(&self) -> Transform {
        Transform::new(self.orientation, self.pos)
    }

    /// `InZone(point, margin)`.
    #[must_use]
    pub fn in_zone(&self, point: Vector, margin: f32) -> bool {
        let dx = point.x - self.pos.x;
        let dy = point.y - self.pos.y;
        let dz_exact = f64::from(point.z) - f64::from(self.pos.z);
        let dz = dz_exact as f32;
        let dist_sq = (dz_exact * f64::from(dz) + f64::from(dx) * f64::from(dx))
            + f64::from(dy) * f64::from(dy);
        self.in_zone_tail(point, margin, dist_sq, [dx, dy, dz], false)
    }

    /// `InZoneXZ(point, margin)`: the test in the zone's XZ plane.
    #[must_use]
    pub fn in_zone_xz(&self, point: Vector, margin: f32) -> bool {
        let dx = point.x - self.pos.x;
        let dz = point.z - self.pos.z;
        let dist_sq = f64::from(dx) * f64::from(dx) + f64::from(dz) * f64::from(dz);
        self.in_zone_tail(point, margin, dist_sq, [dx, 0.0, dz], true)
    }

    fn in_zone_tail(
        &self,
        point: Vector,
        margin: f32,
        dist_sq: f64,
        d: [f32; 3],
        xz: bool,
    ) -> bool {
        let dist_sq_f = dist_sq as f32;
        if dist_sq < f64::from(1.0e-4_f32) {
            return true;
        }
        let reach = f64::from(self.radius()) + f64::from(margin);
        if not_below(f64::from(dist_sq_f), reach * reach) {
            return false;
        }
        let m = f64::from(margin);
        match self.shape {
            ZoneShape::Sphere => true,
            ZoneShape::Ellipsoid => {
                let length = f64::from(dist_sq_f).sqrt() as f32;
                let inv = 1.0 / f64::from(length);
                let dir = Vector {
                    x: (f64::from(d[0]) * inv) as f32,
                    y: (f64::from(d[1]) * inv) as f32,
                    z: (f64::from(d[2]) * inv) as f32,
                };
                self.radius_along(dir) + m >= f64::from(length)
            }
            ZoneShape::Box => {
                let mut frame = self.transform();
                if xz {
                    frame.translation.y = 0.0;
                }
                let half = [self.size.x * 0.5, self.size.y * 0.5, self.size.z * 0.5];
                let local = transpose_transform(&frame.rotation, sub(point, frame.translation));
                let (clamped, outside) = clamp_to_box_surface(half, local);
                let diff = sub(local, clamped);
                let (a, b, c) = (f64::from(diff.x), f64::from(diff.y), f64::from(diff.z));
                let mut value = if xz {
                    (b * b + c * c) + a * a
                } else {
                    (a * a + b * b) + c * c
                };
                if !outside {
                    value = -value;
                }
                value < m * m
            }
            ZoneShape::Cylinder | ZoneShape::Ring => {
                let local = transpose_transform(&self.orientation, sub(point, self.pos));
                let height = f64::from(if self.shape == ZoneShape::Cylinder {
                    self.size.y
                } else {
                    self.size.z
                }) * 0.5;
                let along = if xz { 0.0 } else { f64::from(local.y).abs() };
                if !ge(height, along - m) {
                    return false;
                }
                let radial = f64::from(local.x) * f64::from(local.x)
                    + f64::from(local.z) * f64::from(local.z);
                let sx = f64::from(self.size.x);
                let outer = (m + m + sx) * sx + m * m;
                if self.shape == ZoneShape::Cylinder {
                    return radial - outer < 0.0;
                }
                let sy = f64::from(self.size.y);
                let sz = f64::from(self.size.z);
                let inner = f64::from((sy * sy + (sz * sz) * 0.25).sqrt() as f32);
                if !le(radial - outer, 0.0) {
                    return false;
                }
                radial - ((inner + inner + sy) * sy + inner * inner) >= 0.0
            }
            ZoneShape::Mesh | ZoneShape::Invalid => false,
        }
    }

    /// `GetRadiusAlong(direction)`: the distance from the centre to the
    /// surface along a unit world direction.
    #[must_use]
    pub fn radius_along(&self, direction: Vector) -> f64 {
        match self.shape {
            ZoneShape::Sphere => f64::from(self.size.x),
            ZoneShape::Ellipsoid => {
                let local = transpose_transform(&self.orientation, direction);
                ellipsoid_radius_along(self.size, local)
            }
            ZoneShape::Box => {
                let local = transpose_transform(&self.orientation, direction);
                let small = f64::from(1.0e-4_f32);
                let abs_x = f64::from(local.x).abs();
                let along_x = if abs_x > small {
                    (f64::from(self.size.x) / abs_x) as f32
                } else {
                    1.0e10
                };
                let abs_y = f64::from(local.y).abs();
                let along_y = if abs_y > small {
                    f64::from(self.size.y) / abs_y
                } else {
                    f64::from(1.0e10_f32)
                };
                let abs_z = f64::from(local.z).abs();
                let along_z = if abs_z > small {
                    (f64::from(self.size.z) / abs_z) as f32
                } else {
                    1.0e10
                };
                let mut smallest = if f64::from(along_x) < along_y {
                    f64::from(along_x)
                } else {
                    along_y
                };
                if not_below(smallest, f64::from(along_z)) {
                    smallest = f64::from(along_z);
                }
                smallest * 0.5
            }
            ZoneShape::Cylinder | ZoneShape::Ring => {
                let local = transpose_transform(&self.orientation, direction);
                let half = f64::from(if self.shape == ZoneShape::Cylinder {
                    self.size.y
                } else {
                    self.size.z
                }) * 0.5;
                let abs_y = f64::from(local.y).abs();
                let sx = f64::from(self.size.x);
                let cutoff = half / (sx * sx + half * half).sqrt();
                if abs_y > cutoff {
                    return half / abs_y;
                }
                sx / (f64::from(local.x) * f64::from(local.x)
                    + f64::from(local.z) * f64::from(local.z))
                .sqrt()
            }
            ZoneShape::Mesh | ZoneShape::Invalid => f64::from(self.radius()),
        }
    }

    /// `GetDistance(point)`: negative inside.
    #[must_use]
    pub fn distance(&self, point: Vector) -> f64 {
        match self.shape {
            ZoneShape::Sphere => {
                let d = sub(point, self.pos);
                ((f64::from(d.z) * f64::from(d.z) + f64::from(d.y) * f64::from(d.y))
                    + f64::from(d.x) * f64::from(d.x))
                .sqrt()
                    - f64::from(self.size.x)
            }
            ZoneShape::Cylinder | ZoneShape::Box => {
                let half = if self.shape == ZoneShape::Cylinder {
                    [
                        self.size.x,
                        (f64::from(self.size.y) * 0.5) as f32,
                        self.size.x,
                    ]
                } else {
                    [self.size.x * 0.5, self.size.y * 0.5, self.size.z * 0.5]
                };
                let local = transpose_transform(&self.orientation, sub(point, self.pos));
                let (clamped, outside) = clamp_to_box_surface(half, local);
                let mut value = length_sq(sub(local, clamped));
                if !outside {
                    value = -value;
                }
                if value < 0.0 {
                    -(-value).sqrt()
                } else {
                    value.sqrt()
                }
            }
            ZoneShape::Ellipsoid => {
                let delta = sub(point, self.pos);
                let length = length_sq(delta).sqrt();
                let length_f = length as f32;
                if length < 1.0e-4 {
                    return -f64::from(ellipsoid_min_radius(self.size));
                }
                let inv = (1.0 / f64::from(length_f)) as f32;
                let scaled = Vector {
                    x: inv * delta.x,
                    y: inv * delta.y,
                    z: inv * delta.z,
                };
                let local = transpose_transform(&self.orientation, scaled);
                f64::from(length_f) - ellipsoid_radius_along(self.size, local)
            }
            ZoneShape::Ring | ZoneShape::Mesh | ZoneShape::Invalid => {
                let d = sub(self.surface_point(point), self.pos);
                let length = ((f64::from(d.z) * f64::from(d.z) + f64::from(d.y) * f64::from(d.y))
                    + f64::from(d.x) * f64::from(d.x))
                .sqrt() as f32;
                if self.in_zone(point, 0.0) {
                    -f64::from(length)
                } else {
                    f64::from(length)
                }
            }
        }
    }

    /// `GetSurfacePoint(point)`: the clamp for a box, the nearest point of
    /// the tube for a ring, else the surface along the direction of
    /// `point` (the centre when `point` is on it).
    #[must_use]
    pub fn surface_point(&self, point: Vector) -> Vector {
        match self.shape {
            ZoneShape::Box => {
                let half = [self.size.x * 0.5, self.size.y * 0.5, self.size.z * 0.5];
                let local = transpose_transform(&self.orientation, sub(point, self.pos));
                let (clamped, _) = clamp_to_box_surface(half, local);
                transform_xform(&self.transform(), clamped)
            }
            ZoneShape::Ring => self.ring_surface_point(point),
            _ => {
                let delta = sub(point, self.pos);
                let length = length_sq(delta).sqrt();
                if length < f64::from(1.0e-4_f32) {
                    return self.pos;
                }
                let inv = 1.0 / length;
                let dir = Vector {
                    x: (f64::from(delta.x) * inv) as f32,
                    y: (f64::from(delta.y) * inv) as f32,
                    z: (f64::from(delta.z) * inv) as f32,
                };
                let radius = self.radius_along(dir) as f32;
                Vector {
                    x: radius * dir.x + self.pos.x,
                    y: radius * dir.y + self.pos.y,
                    z: radius * dir.z + self.pos.z,
                }
            }
        }
    }

    fn ring_surface_point(&self, point: Vector) -> Vector {
        let local = transpose_transform(&self.orientation, sub(point, self.pos));
        let rim = Vector {
            x: local.x,
            y: 0.0,
            z: local.z,
        };
        let mid = ((f64::from(self.size.y) + f64::from(self.size.x)) * 0.5) as f32;
        let scale = f64::from(mid) / length_sq(rim).sqrt();
        let rim = Vector {
            x: (f64::from(rim.x) * scale) as f32,
            y: (0.0 * scale) as f32,
            z: (f64::from(rim.z) * scale) as f32,
        };
        let mut tube = sub(local, rim);
        let tube_length = length_sq(tube).sqrt();
        if not_below(tube_length, f64::from(1.0e-4_f32)) {
            let inv = 1.0 / tube_length;
            tube = Vector {
                x: (f64::from(tube.x) * inv) as f32,
                y: (f64::from(tube.y) * inv) as f32,
                z: (inv * f64::from(tube.z)) as f32,
            };
        }
        let surface = ring_surface_scale(self.size, tube);
        let offset = Vector {
            x: (f64::from(tube.x) * surface) as f32,
            y: (f64::from(tube.y) * surface) as f32,
            z: (f64::from(tube.z) * surface) as f32,
        };
        let local_surface = Vector {
            x: rim.x + offset.x,
            y: rim.y + offset.y,
            z: rim.z + offset.z,
        };
        let world = transform_matrix(&self.orientation, local_surface);
        Vector {
            x: self.pos.x + world.x,
            y: self.pos.y + world.y,
            z: self.pos.z + world.z,
        }
    }
}

/// `a >= b` (false when unordered); the C++ tests `!(a >= b)`.
fn ge(a: f64, b: f64) -> bool {
    a >= b
}

/// `a > b` (false when unordered); the C++ tests `!(a > b)`.
fn gt(a: f64, b: f64) -> bool {
    a > b
}

/// `a <= b` (false when unordered); the C++ tests `!(a <= b)`.
fn le(a: f64, b: f64) -> bool {
    a <= b
}

/// `Vec3Sub`, float stores.
fn sub(a: Vector, b: Vector) -> Vector {
    Vector {
        x: a.x - b.x,
        y: a.y - b.y,
        z: a.z - b.z,
    }
}

/// `VecLengthSq`: `(x*x + y*y) + z*z`, unrounded.
fn length_sq(v: Vector) -> f64 {
    (f64::from(v.x) * f64::from(v.x) + f64::from(v.y) * f64::from(v.y))
        + f64::from(v.z) * f64::from(v.z)
}

/// `ClampPointToBoxSurface`: the point moved onto the surface of a box of
/// half extents `half`, and whether it was outside. An inside point moves
/// along the axis it is nearest the surface on.
#[must_use]
pub fn clamp_to_box_surface(half: [f32; 3], point: Vector) -> (Vector, bool) {
    let input = [point.x, point.y, point.z];
    let mut out = input;
    let mut overshoot = [0.0_f32; 3];
    let mut outside = false;
    for axis in 0..3 {
        overshoot[axis] = input[axis].abs() - half[axis];
        if overshoot[axis] >= 0.0 {
            out[axis] = half[axis].copysign(input[axis]);
            outside = true;
        }
    }
    if !outside {
        let nearest = if overshoot[0] >= overshoot[1] {
            if overshoot[0] >= overshoot[2] { 0 } else { 2 }
        } else if overshoot[1] >= overshoot[2] {
            1
        } else {
            2
        };
        out[nearest] = half[nearest].copysign(input[nearest]);
    }
    (
        Vector {
            x: out[0],
            y: out[1],
            z: out[2],
        },
        outside,
    )
}

/// `EllipsoidRadiusAlong` on a local unit direction.
fn ellipsoid_radius_along(radii: Vector, dir: Vector) -> f64 {
    let a = f64::from(radii.x) * f64::from(radii.x);
    let b = f64::from(radii.y) * f64::from(radii.y);
    let c = f64::from(radii.z) * f64::from(radii.z);
    let (x, y, z) = (f64::from(dir.x), f64::from(dir.y), f64::from(dir.z));
    let sum = ((((x * x) * c) * b) + (((z * z) * b) * a)) + (((y * y) * c) * a);
    let product = (a * (c * b)) * sum;
    if !gt(product, 1.0e-4) {
        return 0.0;
    }
    product.sqrt() / sum
}

/// `EllipsoidMinRadius`.
fn ellipsoid_min_radius(radii: Vector) -> f32 {
    let (r0, r1, r2) = (radii.x, radii.y, radii.z);
    if r0 < r1 {
        if r0 < r2 { r0 } else { r2 }
    } else if r1 < r2 {
        r1
    } else {
        r2
    }
}

/// `RingSurfaceScale`: the scale along a unit tube direction to the
/// ring's surface.
fn ring_surface_scale(size: Vector, w: Vector) -> f64 {
    let half = f64::from(size.z) * 0.5;
    let abs_y = f64::from(w.y).abs();
    let cutoff = half / (f64::from(size.x) * f64::from(size.x) + half * half).sqrt();
    if abs_y > cutoff {
        return half / abs_y;
    }
    ((f64::from(size.x) - f64::from(size.y)) * 0.5)
        / (f64::from(w.z) * f64::from(w.z) + f64::from(w.x) * f64::from(w.x)).sqrt()
}
