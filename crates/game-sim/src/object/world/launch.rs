//! The Common geometry of docking and launching that the server reads
//! (P3.3): `CEqObj::launch_pos` (`common_ceqobj.cpp:961`), a hardpoint
//! in object space carried to the world through the root's transform
//! (`FindHardpoint_OS` and `MulTransform`, as `EqObj::process_dock_queue`
//! does), an archetype's own hardpoint (`HardpointPositionA` on the
//! archetype's model), `PhySys::BeamR` (`CObject::beam_object`) and
//! `get_physical_radius_r`. The rules that use them are the server's
//! (`game-logic::dock`).

use neuron_core::math::{
    Matrix, Quaternion, Transform, Vector, mul_matrix, mul_transform, quaternion_to_matrix,
};

use crate::ai::fp::{e, r};
use crate::object::ObjHandle;

use super::World;

/// `launch_pos`'s spread angle: π/2 through a float, as a double.
const SPREAD_ANGLE: f64 = 1.570_796_370_506_286_6;

/// `launch_pos`'s fallback turn: π through a float, as a double.
const FALLBACK_ANGLE: f64 = 3.141_592_741_012_573_2;

impl World {
    /// `CEqObj::launch_pos(Pos, Orient, Index)`. With the index of one of
    /// the object's docking spheres: the sphere's hardpoint in world space
    /// (`HardpointPosition`), its orientation turned a quarter about its
    /// own Y axis (the quaternion `{cos A, sin A / |Y| · Y}` times the
    /// orientation), and true. Otherwise the object's position and its
    /// orientation with rows 0 and 2 turned by π, and false.
    ///
    /// The reference's `FCOS` and `FSIN` round once from 64 bits; here the
    /// `f64` functions of the same double are used, which agree after the
    /// stores to float. A sphere whose hardpoint the model lacks leaves the
    /// reference's outputs as the caller's bytes; here the object's own
    /// frame is turned (D623).
    #[must_use]
    pub fn launch_pos(&self, h: ObjHandle, index: i32) -> (bool, Vector, Matrix) {
        let Some(o) = self.get(h) else {
            return (false, Vector::ZERO, Matrix::IDENTITY);
        };
        let sphere = usize::try_from(index)
            .ok()
            .filter(|_| index != -1)
            .and_then(|i| o.as_equipped()?.docking_points.get(i));
        if let Some(sphere) = sphere {
            let (pos, orient) = self
                .hardpoint_world(h, &sphere.hardpoint)
                .unwrap_or((o.engine.position, o.engine.orientation));
            let col1 = Vector::new(orient.m[0][1], orient.m[1][1], orient.m[2][1]);
            let w = SPREAD_ANGLE.cos() as f32;
            let len =
                (e(col1.z) * e(col1.z) + e(col1.x) * e(col1.x) + e(col1.y) * e(col1.y)).sqrt();
            let scale = r(SPREAD_ANGLE.sin() / len);
            let quat = Quaternion {
                w,
                x: r(e(scale) * e(col1.x)),
                y: r(e(scale) * e(col1.y)),
                z: r(e(scale) * e(col1.z)),
            };
            let spread = quaternion_to_matrix(&quat);
            return (true, pos, mul_matrix(&spread, &orient));
        }
        let mut orient = o.engine.orientation;
        let (cos_a, sin_a) = (FALLBACK_ANGLE.cos(), FALLBACK_ANGLE.sin());
        for col in 0..3 {
            let row0 = e(orient.m[0][col]);
            let row2 = e(orient.m[2][col]);
            orient.m[0][col] = r(sin_a * row2 + row0 * cos_a);
            orient.m[2][col] = r(cos_a * row2 - row0 * sin_a);
        }
        (false, o.engine.position, orient)
    }

    /// `FindHardpoint_OS(index, name)` and `MulTransform` with the root
    /// instance's transform: the hardpoint's point in world space. `None`
    /// when the model has no such hardpoint.
    #[must_use]
    pub fn hardpoint_os_world(&self, h: ObjHandle, name: &[u8]) -> Option<Vector> {
        let o = self.get(h)?;
        let root = o.engine.instance;
        let (_, local) = self.find_hardpoint_ps(root, name)?;
        let root_xform = self.scene.instance_transform(root);
        Some(mul_transform(&root_xform, &local).translation)
    }

    /// The root instance's world translation (`GetInstanceTransform`'s).
    #[must_use]
    pub fn root_translation(&self, h: ObjHandle) -> Option<Vector> {
        let o = self.get(h)?;
        Some(self.scene.instance_transform(o.engine.instance).translation)
    }

    /// `HardpointPositionA(Archetype->EngineIndex, name)`: a hardpoint of a
    /// ship archetype's model in its own part's frame (no transform), from
    /// a model instance made for the query and destroyed after it. `None`
    /// without assets, for an unknown archetype or a missing hardpoint.
    pub fn ship_archetype_hardpoint(&mut self, archetype: u32, name: &[u8]) -> Option<Transform> {
        let path = {
            let arch = self.data.archetypes.ships.get(&archetype)?;
            self.strings.get(arch.eq_obj.root.da_archetype).to_vec()
        };
        let instance = self.create_scene_instance(&path)?;
        let found = self
            .find_hardpoint(Some(instance), name)
            .map(|(_, data)| Transform::new(data.orientation(), data.position()));
        self.scene.destroy_instance(instance);
        found
    }

    /// `Archetype::Load` and `HardpointPositionA` apart (`pub::FindHardpoint`):
    /// `None` when the ship archetype is unknown or its model does not load
    /// (no assets), else the hardpoint as [`World::ship_archetype_hardpoint`]
    /// finds it, `None` inside when the model lacks it.
    pub fn ship_archetype_hardpoint_lookup(
        &mut self,
        archetype: u32,
        name: &[u8],
    ) -> Option<Option<Transform>> {
        let path = {
            let arch = self.data.archetypes.ships.get(&archetype)?;
            self.strings.get(arch.eq_obj.root.da_archetype).to_vec()
        };
        let instance = self.create_scene_instance(&path)?;
        let found = self
            .find_hardpoint(Some(instance), name)
            .map(|(_, data)| Transform::new(data.orientation(), data.position()));
        self.scene.destroy_instance(instance);
        Some(found)
    }

    /// `CObject::beam_object(Pos, Orient, false)` (`PhySys::BeamR`): the
    /// object and its body at the pose, at rest. The attached equipment
    /// shares the ship's body (record 0001), so no child is beamed apart.
    pub fn beam_object(&mut self, h: ObjHandle, position: Vector, orientation: Matrix) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        o.engine.position = position;
        o.engine.orientation = orientation;
        if let Some((system, body)) = self.body(h)
            && let Some(world) = self.physics.world_mut(system)
        {
            world.beam(body, position, &orientation, false);
        }
        self.sync_instance(h);
    }

    /// `CSimple::get_physical_radius_r(center)` as the AI view answers it
    /// (`PhySys::GetRadiusR`): the body core's radius and its mass centre,
    /// the object's own sphere without a body.
    #[must_use]
    pub fn physical_radius_r(&self, h: ObjHandle) -> (f32, Vector) {
        self.physical_sphere(h)
    }
}
