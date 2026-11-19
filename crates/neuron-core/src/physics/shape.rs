//! Collision geometry on `parry3d` (decision O19): hulls from compact
//! surfaces, the default part and box phantoms without qhull, and the
//! conversions to `nalgebra`, which happen in this module only (T13).

use std::sync::Arc;

use parry3d::math::{Isometry, Point, Real, Translation, Vector as NaVector};
use parry3d::na::{Quaternion as NaQuaternion, UnitQuaternion};
use parry3d::shape::{Ball, ConvexPolyhedron, Cuboid, SharedShape};

use super::ivp::math::IvpQuat;
use crate::math::Vector;
use crate::sur::CompactSurface;

/// One convex piece of a body: a compact ledge's hull and the part id the
/// ledge carries (`IVP_Compact_Ledge::client_data`).
#[derive(Clone)]
pub struct Ledge {
    /// The part id; 0 for shapes without parts.
    pub part_id: u32,
    /// The hull in object space.
    pub shape: SharedShape,
}

impl core::fmt::Debug for Ledge {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Ledge")
            .field("part_id", &self.part_id)
            .finish_non_exhaustive()
    }
}

/// A body's or phantom's geometry in object space and the mass properties
/// IVP's surface manager reports for it.
#[derive(Clone, Debug)]
pub struct Geometry {
    /// The convex pieces.
    pub ledges: Arc<Vec<Ledge>>,
    /// The mass centre (`get_mass_center`).
    pub mass_center: [f32; 3],
    /// The inertia per unit mass (`get_rotation_inertia`).
    pub rotation_inertia: [f32; 3],
    /// The surface's `upper_limit_radius` around the mass centre.
    pub upper_limit_radius: f32,
    /// The surface's deviation byte (`max_deviation_and_size & 0xff`).
    pub max_deviation: u8,
    /// The kind of IVP object it makes.
    pub kind: GeometryKind,
}

/// The IVP object type of a geometry.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum GeometryKind {
    /// `IVP_Polygon` (object type 2) over a compact surface.
    Polygon,
    /// `IVP_Ball` (object type 3) of this radius.
    Ball(f32),
}

fn point(p: [f32; 3]) -> Point<Real> {
    Point::new(p[0], p[1], p[2])
}

impl Geometry {
    /// The hulls of a compact surface's terminal ledges with their part
    /// ids, and the surface's mass properties. A ledge whose points do not
    /// make a hull is skipped.
    #[must_use]
    pub fn from_surface(surface: &CompactSurface) -> Self {
        let mut ledges = Vec::new();
        for index in surface.terminal_ledges() {
            let ledge = &surface.ledges[index];
            // Only the points the ledge's edges name: the point array is
            // shared between the ledges of a surface, so the run from
            // index 0 also holds other ledges' (and other parts') points.
            let mut named: Vec<u16> = ledge
                .triangles
                .iter()
                .flat_map(|t| t.edges.iter().map(|e| e.start_point_index()))
                .collect();
            named.sort_unstable();
            named.dedup();
            let points: Vec<Point<Real>> = named
                .iter()
                .filter_map(|&i| ledge.points.get(usize::from(i)))
                .map(|p| point(p.position.to_words()))
                .collect();
            if let Some(hull) = ConvexPolyhedron::from_convex_hull(&points) {
                ledges.push(Ledge {
                    part_id: ledge.client_data as u32,
                    shape: SharedShape::new(hull),
                });
            }
        }
        Self {
            ledges: Arc::new(ledges),
            mass_center: surface.mass_center.to_words(),
            rotation_inertia: surface.rotation_inertia.to_words(),
            upper_limit_radius: surface.upper_limit_radius,
            max_deviation: surface.max_deviation(),
            kind: GeometryKind::Polygon,
        }
    }

    /// The default part (`PhyArch::GetDefaultPart`, `common_physys.cpp:437`):
    /// the hull of the four corners of the z = +0.5 face and the apex
    /// (0, 0, -0.5). The reference builds it with qhull and IVP's surface
    /// builder; the port takes the hull's exact solid mass properties
    /// (divergence register).
    #[must_use]
    pub fn default_part() -> Self {
        let corners = [
            [-0.5, -0.5, 0.5],
            [0.5, -0.5, 0.5],
            [0.5, 0.5, 0.5],
            [-0.5, 0.5, 0.5],
            [0.0, 0.0, -0.5],
        ];
        let points: Vec<Point<Real>> = corners.iter().map(|c| point(*c)).collect();
        let ledges = ConvexPolyhedron::from_convex_hull(&points)
            .map(|hull| {
                vec![Ledge {
                    part_id: 0,
                    shape: SharedShape::new(hull),
                }]
            })
            .unwrap_or_default();
        // A square pyramid of side 1 and height 1: the centroid a quarter
        // of the height above the base, inertia a^2/20 + 3h^2/80 across
        // and a^2/10 along the axis, per unit mass.
        Self {
            ledges: Arc::new(ledges),
            mass_center: [0.0, 0.0, 0.25],
            rotation_inertia: [0.0875, 0.0875, 0.1],
            upper_limit_radius: 0.75,
            max_deviation: 0,
            kind: GeometryKind::Polygon,
        }
    }

    /// A box phantom's cuboid between two corners (`common_phantom.cpp:219`;
    /// the reference makes the hull of the 8 corners with qhull).
    #[must_use]
    pub fn cuboid(min: Vector, max: Vector) -> Self {
        let half = [
            (max.x - min.x) * 0.5,
            (max.y - min.y) * 0.5,
            (max.z - min.z) * 0.5,
        ];
        let center = [
            (max.x + min.x) * 0.5,
            (max.y + min.y) * 0.5,
            (max.z + min.z) * 0.5,
        ];
        let cuboid = Cuboid::new(NaVector::new(half[0].abs(), half[1].abs(), half[2].abs()));
        let shape = SharedShape::compound(vec![(
            Isometry::from_parts(
                Translation::new(center[0], center[1], center[2]),
                UnitQuaternion::identity(),
            ),
            SharedShape::new(cuboid),
        )]);
        let radius = (half[0] * half[0] + half[1] * half[1] + half[2] * half[2]).sqrt();
        Self {
            ledges: Arc::new(vec![Ledge { part_id: 0, shape }]),
            mass_center: center,
            rotation_inertia: [1.0; 3],
            upper_limit_radius: radius,
            max_deviation: 0,
            kind: GeometryKind::Polygon,
        }
    }

    /// A ball of the given radius about the object's origin (`BuildIVP_Sphere`,
    /// sphere phantoms).
    #[must_use]
    pub fn ball(radius: f32) -> Self {
        Self {
            ledges: Arc::new(vec![Ledge {
                part_id: 0,
                shape: SharedShape::new(Ball::new(radius.abs())),
            }]),
            mass_center: [0.0; 3],
            rotation_inertia: [1.0; 3],
            upper_limit_radius: radius,
            max_deviation: 0,
            kind: GeometryKind::Ball(radius),
        }
    }

    /// `get_radius_and_radius_dev_to_given_center` (`ivp_surman_polygon.cpp:135`):
    /// the bounding radius and the radius deviation about `center`.
    #[must_use]
    pub fn radius_and_deviation(&self, center: [f32; 3]) -> (f32, f32) {
        if let GeometryKind::Ball(radius) = self.kind {
            return (radius, 0.0);
        }
        let dx = f64::from(self.mass_center[0]) - f64::from(center[0]);
        let dy = f64::from(self.mass_center[1]) - f64::from(center[1]);
        let dz = f64::from(self.mass_center[2]) - f64::from(center[2]);
        let dist = ((dz * dz + dy * dy) + dx * dx).sqrt();
        let radius = (dist + f64::from(self.upper_limit_radius)) as f32;
        let deviation = (f64::from(self.max_deviation)
            * f64::from(self.upper_limit_radius)
            * f64::from(0.004f32)
            + dist) as f32;
        (radius, deviation)
    }
}

/// An object's world pose as a `parry3d` isometry.
#[must_use]
pub fn isometry(rotation: IvpQuat, position: [f32; 3]) -> Isometry<Real> {
    let q = UnitQuaternion::new_normalize(NaQuaternion::new(
        rotation.w, rotation.x, rotation.y, rotation.z,
    ));
    Isometry::from_parts(Translation::new(position[0], position[1], position[2]), q)
}

/// A point to `parry3d`.
#[must_use]
pub fn to_point(p: [f32; 3]) -> Point<Real> {
    point(p)
}

/// A vector to `parry3d`.
#[must_use]
pub fn to_vector(v: [f32; 3]) -> NaVector<Real> {
    NaVector::new(v[0], v[1], v[2])
}

/// A `parry3d` point back.
#[must_use]
pub fn from_point(p: &Point<Real>) -> [f32; 3] {
    [p.x, p.y, p.z]
}

/// A `parry3d` vector back.
#[must_use]
pub fn from_vector(v: &NaVector<Real>) -> [f32; 3] {
    [v.x, v.y, v.z]
}

#[cfg(test)]
mod tests {
    use super::*;
    use parry3d::query::Ray;

    #[test]
    fn default_part_is_one_pyramid() {
        let g = Geometry::default_part();
        assert_eq!(g.ledges.len(), 1);
        let iso = isometry(IvpQuat::IDENTITY, [0.0; 3]);
        let ray = Ray::new(to_point([0.0, 0.0, -10.0]), to_vector([0.0, 0.0, 1.0]));
        let hit = g.ledges[0].shape.cast_ray(&iso, &ray, 100.0, true).unwrap();
        assert!((hit - 9.5).abs() < 1e-5);
        let (radius, deviation) = g.radius_and_deviation(g.mass_center);
        assert_eq!((radius, deviation), (0.75, 0.0));
    }

    /// A ledge's hull holds only the points its edges name: the shared
    /// point array's other points (other ledges', other parts') stay out.
    #[test]
    fn ledge_hull_ignores_points_its_edges_do_not_name() {
        let bytes = include_bytes!("../../tests/fixtures/oracle/sur/multi.sur");
        let loaded = crate::sur::load(bytes);
        let mut checked = 0;
        for part in loaded.parts.values() {
            let Some(Ok(mut surface)) = part.compact_surface() else {
                continue;
            };
            let before = Geometry::from_surface(&surface);
            for index in surface.terminal_ledges() {
                surface.ledges[index]
                    .points
                    .push(crate::sur::compact::PolyPoint {
                        position: Vector::new(1.0e4, 1.0e4, 1.0e4),
                        hesse_val: 0.0,
                    });
            }
            let after = Geometry::from_surface(&surface);
            assert_eq!(before.ledges.len(), after.ledges.len());
            for (a, b) in before.ledges.iter().zip(after.ledges.iter()) {
                let (Some(a), Some(b)) = (
                    a.shape.as_convex_polyhedron(),
                    b.shape.as_convex_polyhedron(),
                ) else {
                    panic!("a ledge is not a hull");
                };
                assert_eq!(a.points(), b.points());
                checked += 1;
            }
        }
        assert!(checked > 0);
    }

    #[test]
    fn cuboid_and_ball() {
        let g = Geometry::cuboid(Vector::new(0.0, 0.0, 0.0), Vector::new(2.0, 4.0, 6.0));
        assert_eq!(g.mass_center, [1.0, 2.0, 3.0]);
        let iso = isometry(IvpQuat::IDENTITY, [0.0; 3]);
        let ray = Ray::new(to_point([1.0, 2.0, -10.0]), to_vector([0.0, 0.0, 1.0]));
        let hit = g.ledges[0].shape.cast_ray(&iso, &ray, 100.0, true).unwrap();
        assert!((hit - 10.0).abs() < 1e-5);
        let b = Geometry::ball(2.0);
        assert_eq!(b.radius_and_deviation([0.0; 3]), (2.0, 0.0));
        assert_eq!(b.kind, GeometryKind::Ball(2.0));
        assert_eq!(from_point(&to_point([1.0, 2.0, 3.0])), [1.0, 2.0, 3.0]);
        assert_eq!(from_vector(&to_vector([1.0, 2.0, 3.0])), [1.0, 2.0, 3.0]);
    }
}
