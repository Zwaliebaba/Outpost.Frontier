//! Explosion bounding spheres (`CEqObj::compute_explosion_dmg_bounding_sphere`
//! and `get_explosion_dmg_bounding_sphere`, `Common/common_ceqobj.cpp`) and
//! the hardpoint lookups of `Common/common_hardpointsummary.cpp`
//! (`HardpointSummary`, `FindHardpoint_PS`, `HardpointToWorld`).

use neuron_core::hash::eq_ignore_case;
use neuron_core::math::{
    Transform, Vector, inverse_transform, mul_transform, transform_xform, transpose_transform,
};
use neuron_core::scene::{Engine, HardpointData, Hardpoints, InstanceId};

use super::ObjHandle;
use super::world::World;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `compute_explosion_dmg_bounding_sphere` in world space: the box around
/// the root's sphere and the root group's child spheres (each given as
/// radius and world centre), its centre, and the radius that covers the
/// box's widest side, the root sphere and every child sphere.
#[must_use]
pub fn explosion_dmg_sphere(root: (f32, Vector), children: &[(f32, Vector)]) -> (f32, Vector) {
    let (rr, rw) = root;
    let mut min = [r(e(rw.x) - e(rr)), r(e(rw.y) - e(rr)), r(e(rw.z) - e(rr))];
    let mut max = [r(e(rw.x) + e(rr)), r(e(rw.y) + e(rr)), r(e(rw.z) + e(rr))];
    for &(cr, c) in children {
        for (axis, v) in [c.x, c.y, c.z].into_iter().enumerate() {
            let low = e(v) - e(cr);
            if !(e(min[axis]) < low) {
                min[axis] = r(low);
            }
            let high = e(v) + e(cr);
            if !(e(max[axis]) > high) {
                max[axis] = r(high);
            }
        }
    }
    let sum_x = e(max[0]) + e(min[0]);
    let sum_y = e(max[1]) + e(min[1]);
    let sum_z = r(e(max[2]) + e(min[2]));
    let center = Vector::new(r(sum_x * 0.5), r(sum_y * 0.5), r(e(sum_z) * 0.5));
    let width_x = e(max[0]) - e(min[0]);
    let width_y = r(e(max[1]) - e(min[1]));
    let width_z = r(e(max[2]) - e(min[2]));
    let best_xy = if width_x > e(width_y) {
        width_x
    } else {
        e(width_y)
    };
    let widest = if best_xy > e(width_z) {
        best_xy
    } else {
        e(width_z)
    };
    let half = widest * 0.5;
    let mut radius = r(half);
    let reach_of = |p: Vector, q: Vector, extra: f32| {
        let dx = e(p.x) - e(q.x);
        let dy = e(p.y) - e(q.y);
        let dz = e(p.z) - e(q.z);
        ((dz * dz + dy * dy) + dx * dx).sqrt() + e(extra)
    };
    let reach = reach_of(center, rw, rr);
    if reach > half {
        radius = r(reach);
    }
    for &(cr, c) in children {
        let reach = reach_of(c, center, cr);
        if reach > e(radius) {
            radius = r(reach);
        }
    }
    (radius, center)
}

/// One hardpoint of a [`HardpointSummary`].
#[derive(Clone, Debug, PartialEq)]
pub struct HardpointEntry {
    /// Its name.
    pub name: Vec<u8>,
    /// Its record (position and orientation in its instance's space).
    pub info: HardpointData,
    /// The instance whose part carries it (-1 once expired).
    pub instance: Option<InstanceId>,
}

/// `HardpointSummary`: every hardpoint of an instance tree, root first,
/// depth first, each part's hardpoints in the hardpoint component's order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct HardpointSummary {
    /// The hardpoints.
    pub entries: Vec<HardpointEntry>,
}

impl HardpointSummary {
    /// `analyze_instance`: the summary of the tree under `instance`; its
    /// count.
    pub fn analyze_instance(&mut self, scene: &Engine, instance: Option<InstanceId>) -> usize {
        self.entries.clear();
        let Some(root) = instance else {
            return 0;
        };
        let Some(hardpoints) = scene.component::<Hardpoints>() else {
            return 0;
        };
        let mut stack = vec![root];
        while let Some(i) = stack.pop() {
            if let Some(arch) = scene.instance_archetype(i) {
                for name in hardpoints.names(arch) {
                    let crc = neuron_core::hash::name_crc32(name);
                    let info = hardpoints.get(arch, crc).unwrap_or_default();
                    self.entries.push(HardpointEntry {
                        name: name.to_vec(),
                        info,
                        instance: Some(i),
                    });
                }
            }
            let mut children = Vec::new();
            let mut cursor = None;
            while let Some(c) = scene.related_instance(Some(i), 1, cursor) {
                children.push(c);
                cursor = Some(c);
            }
            stack.extend(children.into_iter().rev());
        }
        self.entries.len()
    }

    /// `find_name`: the index of the last hardpoint of that name (case
    /// ignored).
    #[must_use]
    pub fn find_name(&self, name: &[u8]) -> Option<usize> {
        self.entries
            .iter()
            .rposition(|h| eq_ignore_case(&h.name, name))
    }

    /// `find_name_instance`.
    #[must_use]
    pub fn find_name_instance(&self, name: &[u8]) -> Option<InstanceId> {
        self.find_name(name).and_then(|i| self.entries[i].instance)
    }

    /// `get_info_by_name`.
    #[must_use]
    pub fn info_by_name(&self, name: &[u8]) -> Option<&HardpointData> {
        self.find_name(name).map(|i| &self.entries[i].info)
    }

    /// `expire_instance`: every entry's instance reads -1.
    pub fn expire_instance(&mut self) {
        for h in &mut self.entries {
            h.instance = None;
        }
    }
}

/// A hardpoint record's frame.
fn frame(info: &HardpointData) -> Transform {
    Transform::new(info.orientation(), info.position())
}

/// `HardpointToWorld`: a hardpoint frame of `instance` in world space.
#[must_use]
pub fn hardpoint_to_world(
    scene: &Engine,
    instance: Option<InstanceId>,
    info: &HardpointData,
) -> Transform {
    mul_transform(&scene.instance_transform(instance), &frame(info))
}

impl World {
    /// `FindHardpoint_PS`: the hardpoint on `instance` or its subtree, in
    /// `instance`'s space, and the instance that carries it.
    #[must_use]
    pub fn find_hardpoint_ps(
        &self,
        instance: Option<InstanceId>,
        name: &[u8],
    ) -> Option<(InstanceId, Transform)> {
        let (found, info) = self.find_hardpoint(instance, name)?;
        let local = frame(&info);
        if Some(found) == instance {
            return Some((found, local));
        }
        let world = mul_transform(&self.scene.instance_transform(Some(found)), &local);
        let space = self.scene.instance_transform(instance);
        let rotated = transpose_transform(&space.rotation, space.translation);
        let inverse = Transform::new(
            space.rotation.transpose(),
            Vector::new(-rotated.x, -rotated.y, -rotated.z),
        );
        Some((found, mul_transform(&inverse, &world)))
    }

    /// `CEqObj::get_explosion_dmg_bounding_sphere`: the explosion sphere
    /// (computed once, kept in the object's space) at the object's current
    /// frame: radius and world centre.
    pub fn explosion_dmg_bounding_sphere(&mut self, h: ObjHandle) -> Option<(f32, Vector)> {
        let o = self.objects.get(h)?;
        let xform = Transform::new(o.engine.orientation, o.engine.position);
        let cached = o.equipped.as_deref()?.explosion_dmg_sphere;
        let (radius, local) = match cached {
            Some(c) => c,
            None => {
                let c = self.compute_explosion_dmg_bounding_sphere(h)?;
                if let Some(eq) = self
                    .objects
                    .get_mut(h)
                    .and_then(|o| o.equipped.as_deref_mut())
                {
                    eq.explosion_dmg_sphere = Some(c);
                }
                c
            }
        };
        Some((radius, transform_xform(&xform, local)))
    }

    /// `CEqObj::compute_explosion_dmg_bounding_sphere`: radius and the
    /// centre in the object's space.
    fn compute_explosion_dmg_bounding_sphere(&mut self, h: ObjHandle) -> Option<(f32, Vector)> {
        let o = self.objects.get(h)?;
        let xform = Transform::new(o.engine.orientation, o.engine.position);
        let root = o.engine.instance;
        let (root_radius, root_local) = self.scene.instance_extent(root, 1);
        let root_world = transform_xform(&xform, root_local);
        let tree = self.instance_tree(root);
        let mut children = Vec::new();
        for child in tree.into_iter().skip(1) {
            if self.inst_to_sub_obj_id(h, Some(child)) != crate::equip::SUBOBJ_ID_ROOT {
                continue;
            }
            let (radius, local) = self.scene.instance_extent(Some(child), 1);
            let t = self.scene.instance_transform(Some(child));
            children.push((radius, transform_xform(&t, local)));
        }
        let (radius, center) = explosion_dmg_sphere((root_radius, root_world), &children);
        Some((radius, inverse_transform(&xform, center)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_lone_root_keeps_its_sphere() {
        let (radius, center) = explosion_dmg_sphere((10.0, Vector::new(1.0, 2.0, 3.0)), &[]);
        assert_eq!(radius, 10.0);
        assert_eq!(center, Vector::new(1.0, 2.0, 3.0));
    }

    #[test]
    fn children_widen_the_box() {
        let (radius, center) =
            explosion_dmg_sphere((1.0, Vector::ZERO), &[(1.0, Vector::new(10.0, 0.0, 0.0))]);
        assert_eq!(center, Vector::new(5.0, 0.0, 0.0));
        assert_eq!(radius, 6.0);
    }
}
