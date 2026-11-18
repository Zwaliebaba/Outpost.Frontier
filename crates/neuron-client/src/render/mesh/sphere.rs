//! The `SphereMesh` plugin (`Plugins.cpp`): planets drawn as six cube
//! faces of a tessellated sphere.
//!
//! The archetype ([`SphereArchetype`], `FSphereHandle`) holds the radius
//! and one material per side found by the names of the `Sphere` node
//! (`neuron-core::mesh_data::sphere`); a seventh, `AtmosphereMaterial`,
//! draws the atmosphere shell and gives the archetype its scale. An
//! instance ([`SphereInstance`], `FSphereAnim`) tessellates one face at a
//! level chosen from the camera distance and draws it six times, rotated,
//! for the faces that face the camera, then the shell.

use std::collections::BTreeMap;

use neuron_core::hash::name_crc32;
use neuron_core::math::{Matrix, Transform, Vector, mul_transform, transform_matrix};
use neuron_core::mesh_data::sphere;
use neuron_core::num::trunc_to_i32;
use neuron_core::vfs::FileSystem;

use super::manager::{DrawRequest, GeometryQuery, RenderComponent};
use super::services::{MeshCamera, MeshServices};
use super::{FIELD20_ENGINE, HANDLE_NONE, Handle, Instance, MeshIid, dump, file, transform};
use crate::render::material::{
    DrawArgs, MaterialId, SubmitContext, VbListing, VertexSource, VertexStream,
};

/// `0x3f13cd3a`: the cube corners' coordinate, 1 / sqrt(3) as a float.
const K: f32 = 0.577_350_26;

/// `0.57735026918962573`: the double the draw compares the limits with.
#[allow(clippy::excessive_precision)]
const KF64: f64 = 0.577_350_269_189_625_73;

/// A planet archetype (`FSphereHandle`).
#[derive(Clone, Debug, PartialEq)]
pub struct SphereArchetype {
    /// One material per side, each with the reference the hook-up took.
    pub materials: Vec<Option<MaterialId>>,
    /// `Radius`; -1 when it did not read.
    pub radius: f32,
    /// 1, or the `AtmosphereMaterial`'s parameter 8.
    pub scale: f32,
    /// The lower-cased material names; `None` where one did not read.
    pub names: Vec<Option<Vec<u8>>>,
}

impl SphereArchetype {
    /// `FSphereHandle::HookUpMaterials`: each side's material by its name
    /// CRC; a miss leaves the side without one. 0, or -1 for a miss.
    pub fn hook_up<S: MeshServices + ?Sized>(&mut self, services: &mut S) -> i32 {
        let mut failed = false;
        for index in 0..self.materials.len() {
            let previous = self.materials[index];
            let crc = self.names[index].as_deref().map_or(0, name_crc32);
            let (result, material) = services.find_material(crc);
            self.materials[index] = material;
            if result != 0 {
                self.materials[index] = None;
                failed = true;
            }
            if let Some(previous) = previous {
                services.release_material(previous);
            }
        }
        if failed { -1 } else { 0 }
    }

    /// `FSphereHandle::Load` after the node was read: the warnings, the
    /// materials hooked up and the atmosphere's scale.
    fn load<S: MeshServices + ?Sized>(
        services: &mut S,
        node: &sphere::SphereMesh,
    ) -> SphereArchetype {
        if !node.radius_read {
            services.dump(
                dump::WARNING,
                file::SPHERE_ARCHETYPE,
                0x5F,
                "SphereMeshArchetype: load: no radius! invalid sphere file?",
            );
        }
        if !node.sides_read {
            services.dump(
                dump::WARNING,
                file::SPHERE_ARCHETYPE,
                0x63,
                "SphereMeshArchetype: load: no sides! invalid sphere file?",
            );
        }
        let sides = node.sides as usize;
        let mut archetype = SphereArchetype {
            materials: vec![None; sides],
            radius: node.radius,
            scale: node.scale,
            names: (0..node.sides)
                .map(|i| node.material_name(i).map(<[u8]>::to_vec))
                .collect(),
        };
        if sides == 0 {
            return archetype;
        }
        for name in &archetype.names {
            if name.is_none() {
                services.dump(
                    dump::WARNING,
                    file::SPHERE_ARCHETYPE,
                    0x78,
                    "SphereMeshArchetype: load: material name not found! invalid sphere file?",
                );
            }
        }
        archetype.hook_up(services);
        if let Some(Some(candidate)) = archetype.materials.get(6).copied().filter(|_| sides > 6)
            && services.material_class(candidate) == b"AtmosphereMaterial"
            && services.material_parameter_count(candidate) > 8
        {
            let (_kind, payload) = services.material_parameter(candidate, 8);
            archetype.scale = f32::from_bits(payload);
        }
        archetype
    }

    /// The destructor: every side's material released.
    fn destroy<S: MeshServices + ?Sized>(&mut self, services: &mut S) {
        for material in self.materials.iter_mut().filter_map(Option::take) {
            services.release_material(material);
        }
    }
}

/// One material slot of an instance (`FSphereMaterialSlot`); the first
/// also holds the tessellated face.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SphereSlot {
    /// The side's material, with a reference.
    pub material: Option<MaterialId>,
    /// The face's first vertex (`Submit` start).
    pub base_vertex: u32,
    /// The face's vertices.
    pub vertex_count: u32,
    /// The face's indices.
    pub indices: Vec<u16>,
}

/// A planet instance (`FSphereAnim`).
#[derive(Clone, Debug, PartialEq)]
pub struct SphereInstance {
    /// Bit 1: rebuild requested; bit 0: built.
    pub flags: u32,
    /// The tessellation radius.
    pub radius: f32,
    /// The archetype.
    pub archetype: Handle,
    /// The scale of the camera distance before the level formula (1.5;
    /// `SetLodDistanceScale`).
    pub lod_scale: f32,
    /// The lowest level, 3.
    pub level_min: i32,
    /// The highest level, 9 (`SetMaxTessellationLevel`).
    pub level_max: i32,
    /// The level of the current tessellation, -1 before the first.
    pub cached_level: i32,
    /// One slot per side.
    pub slots: Vec<SphereSlot>,
    /// Positions, normals and texture coordinates of the face (format
    /// 0x112, not indexed).
    pub vertices: VertexSource,
    /// The vertices the streams are sized for.
    pub vertex_count: u32,
    /// More than six sides: the shell is drawn.
    pub has_atmosphere: bool,
}

/// The rotation of face `face` (`SphereFaceRotation`): about y by a
/// quarter turn (1), a half turn (2) or minus a quarter turn (3), about x
/// by minus (4) or plus (5) a quarter turn, identity for 0. The
/// translation is zero throughout.
fn face_rotation(face: usize, half_pi: f32) -> Transform {
    let hp = f64::from(half_pi);
    let (cos, sin, about_x) = match face {
        1 => (hp.cos(), hp.sin(), false),
        2 => (
            std::f64::consts::PI.cos(),
            f64::from(std::f64::consts::PI as f32).sin(),
            false,
        ),
        3 => ((-hp).cos(), (-hp).sin(), false),
        4 => ((-hp).cos(), (-hp).sin(), true),
        5 => (hp.cos(), hp.sin(), true),
        _ => return Transform::IDENTITY,
    };
    let (c, s) = (cos as f32, sin as f32);
    let rows = if about_x {
        [[1.0, 0.0, 0.0], [0.0, c, -s], [0.0, s, c]]
    } else {
        [[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]]
    };
    Transform::new(Matrix::from_rows(rows), Vector::ZERO)
}

fn put_f32s(stream: &mut VertexStream, at: usize, values: &[f32]) {
    let data = stream.data.get_or_insert_with(Vec::new);
    let end = at + values.len() * 4;
    if data.len() < end {
        data.resize(end, 0);
    }
    for (i, v) in values.iter().enumerate() {
        data[at + i * 4..at + i * 4 + 4].copy_from_slice(&v.to_le_bytes());
    }
}

impl SphereSlot {
    /// One vertex at `base_vertex + vertex_count`: the position is the
    /// normal times the radius, the texture coordinate (s, t).
    fn write_vertex(&self, source: &mut VertexSource, n: [f32; 3], radius: f32, s: f64, t: f64) {
        let vertex = (self.vertex_count + self.base_vertex) as usize;
        put_f32s(
            &mut source.position,
            vertex * 12,
            &[n[0] * radius, n[1] * radius, n[2] * radius],
        );
        put_f32s(&mut source.normal, vertex * 12, &n);
        put_f32s(&mut source.tex[0], vertex * 8, &[s as f32, t as f32]);
    }

    /// `FSphereMaterialSlot::Tessellate`: face `face` of the cube at
    /// `level`, its corners projected onto the sphere row by row. An even
    /// level is a square grid of `2^(level/2)` cells a side, two triangles
    /// a cell; an odd level adds a centre vertex to each cell (the odd
    /// rows) and four triangles round it.
    #[allow(clippy::too_many_lines)]
    fn tessellate(
        &mut self,
        radius: f32,
        face: usize,
        level: i32,
        base_vertex: u32,
        source: &mut VertexSource,
    ) {
        let k = (1.0 / 3.0f64.sqrt()) as f32;
        self.base_vertex = base_vertex;
        self.vertex_count = 0;
        self.indices.clear();
        let mut c = [[k; 3]; 4];
        let flips: &[(usize, usize)] = match face {
            0 => &[(0, 0), (0, 1), (1, 1), (2, 0)],
            1 => &[(0, 1), (1, 1), (1, 2), (3, 2)],
            2 => &[
                (0, 1),
                (0, 2),
                (1, 0),
                (1, 1),
                (1, 2),
                (2, 2),
                (3, 0),
                (3, 2),
            ],
            3 => &[
                (0, 0),
                (0, 1),
                (0, 2),
                (1, 0),
                (1, 1),
                (2, 0),
                (2, 2),
                (3, 0),
            ],
            4 => &[(0, 0), (2, 2), (2, 0), (3, 2)],
            5 => &[
                (0, 0),
                (0, 1),
                (0, 2),
                (1, 1),
                (1, 2),
                (2, 0),
                (2, 1),
                (3, 1),
            ],
            _ => &[],
        };
        for &(row, column) in flips {
            c[row][column] *= -1.0;
        }
        let half = trunc_to_i32(f64::from(level as f32 * 0.5));
        let n = 1i32.wrapping_shl(half as u32);
        let step = (1.0 / f64::from(n)) as f32;
        let width = n + 1;
        let f = f64::from;
        let mut t = 0.0f64;
        let odd = level % 2 != 0;
        let half_step = (f(step) * 0.5) as f32;
        let rows = if odd { width + width - 1 } else { width };
        for row in 0..rows {
            let omt = 1.0 - t;
            let c2x = (f(c[2][0]) * t) as f32;
            let c2y = (f(c[2][1]) * t) as f32;
            let c2z = (f(c[2][2]) * t) as f32;
            let c0y = (f(c[0][1]) * omt) as f32;
            let c0z = (f(c[0][2]) * omt) as f32;
            let ax = f(c[0][0]) * omt + f(c2x);
            let ay = (f(c0y) + f(c2y)) as f32;
            let az = (f(c0z) + f(c2z)) as f32;
            let c3x = t * f(c[3][0]);
            let c3y = t * f(c[3][1]);
            let c3z = (t * f(c[3][2])) as f32;
            let c1x = f(c[1][0]) * omt;
            let c1y = (omt * f(c[1][1])) as f32;
            let c1z = (omt * f(c[1][2])) as f32;
            let mut bx = (c1x + c3x) as f32;
            let mut by = (f(c1y) + c3y) as f32;
            let mut bz = (f(c1z) + f(c3z)) as f32;
            let (anx, any, anz) = if odd {
                let inv_a = (1.0 / ((ax * ax + f(az) * f(az)) + f(ay) * f(ay)).sqrt()) as f32;
                (
                    f((ax * f(inv_a)) as f32),
                    (f(ay) * f(inv_a)) as f32,
                    (f(az) * f(inv_a)) as f32,
                )
            } else {
                let inv_a = (1.0 / ((f(az) * f(az) + f(ay) * f(ay)) + ax * ax).sqrt()) as f32;
                (
                    ax * f(inv_a),
                    (f(ay) * f(inv_a)) as f32,
                    (f(az) * f(inv_a)) as f32,
                )
            };
            let inv_b = 1.0 / ((f(bz) * f(bz) + f(by) * f(by)) + f(bx) * f(bx)).sqrt();
            bx = (f(bx) * inv_b) as f32;
            by = (f(by) * inv_b) as f32;
            bz = (f(bz) * inv_b) as f32;
            let (mut s, columns) = if odd && row % 2 != 0 {
                (f(half_step), width - 1)
            } else {
                (0.0, width)
            };
            for _ in 0..columns {
                let bsx = f(bx) * s;
                let bsy = f(by) * s;
                let bsz = (f(bz) * s) as f32;
                let oms = 1.0 - s;
                let asy = (f(any) * oms) as f32;
                let asz = (f(anz) * oms) as f32;
                let px = (anx * oms + bsx) as f32;
                let py = f(asy) + bsy;
                let pz = f(asz) + f(bsz);
                let inv = 1.0 / ((pz * pz + py * py) + f(px) * f(px)).sqrt();
                let normal = [(f(px) * inv) as f32, (py * inv) as f32, (pz * inv) as f32];
                self.write_vertex(source, normal, radius, s, t);
                s += f(step);
                if s > 1.0 {
                    s = 1.0;
                }
                self.vertex_count += 1;
            }
            t += f(if odd { half_step } else { step });
            if t > 1.0 {
                t = 1.0;
            }
        }
        if n <= 0 {
            return;
        }
        let mut push = |value: i32| self.indices.push(value as u16);
        let mut base = 0;
        if odd {
            for _ in 0..n {
                for column in 0..n {
                    let a0 = base + column;
                    let a1 = a0 + 1;
                    let m = base + column + width;
                    let cc = base + column + rows;
                    for v in [a0, a1, m, a1, cc + 1, m, cc + 1, cc, m, cc, a0, m] {
                        push(v);
                    }
                }
                base += rows;
            }
            return;
        }
        for row in 0..n {
            for column in 0..n {
                let p = base + column;
                let q = width + column + base;
                let order = if (row + column) % 2 != 0 {
                    [p, p + 1, q, p + 1, q + 1, q]
                } else {
                    [p, q + 1, q, p, p + 1, q + 1]
                };
                for v in order {
                    push(v);
                }
            }
            base += width;
        }
    }
}

impl SphereInstance {
    /// `FSphereAnim::Init(archetype, instance)` after the constructor's
    /// reset: a slot per side with a reference on its material.
    fn new<S: MeshServices + ?Sized>(
        services: &mut S,
        handle: Handle,
        archetype: &SphereArchetype,
    ) -> SphereInstance {
        let slots: Vec<SphereSlot> = archetype
            .materials
            .iter()
            .map(|&material| {
                if let Some(material) = material {
                    services.add_ref_material(material);
                }
                SphereSlot {
                    material,
                    ..SphereSlot::default()
                }
            })
            .collect();
        SphereInstance {
            flags: 0,
            radius: archetype.radius,
            archetype: handle,
            lod_scale: 1.5,
            level_min: 3,
            level_max: 9,
            cached_level: -1,
            has_atmosphere: slots.len() > 6,
            slots,
            vertices: VertexSource {
                fvf: 0x112,
                ..VertexSource::default()
            },
            vertex_count: 0,
        }
    }

    /// `FSphereAnim::Reset`: every slot's material released.
    fn reset<S: MeshServices + ?Sized>(&mut self, services: &mut S) {
        for slot in self.slots.drain(..) {
            if let Some(material) = slot.material {
                services.release_material(material);
            }
        }
        self.vertex_count = 0;
        self.vertices = VertexSource::default();
    }

    /// The level for a scaled distance: `2 log2(pi/2 / acos(1 - d/r)) +
    /// 0.5`, truncated and clamped.
    #[must_use]
    pub fn level_for(&self, distance: f32) -> i32 {
        let half_pi = std::f64::consts::PI * 0.5;
        let ratio = half_pi / (1.0 - f64::from(distance) / f64::from(self.radius)).acos();
        let log2 = ratio.log10() / 2.0f64.log10();
        let mut level = trunc_to_i32(log2 + log2 + 0.5);
        if level < self.level_min {
            level = self.level_min;
        }
        if self.level_max < level {
            level = self.level_max;
        }
        level
    }

    /// `FSphereAnim::UpdateLevel`: the face tessellated again in the first
    /// slot when the level changed or a rebuild was requested.
    pub fn update_level(&mut self, distance: f32) {
        let level = self.level_for(distance);
        if self.cached_level == level && self.flags & 2 == 0 {
            return;
        }
        self.cached_level = level;
        let half = trunc_to_i32(f64::from(level as f32 * 0.5));
        let n = 1u32.wrapping_shl(half as u32);
        let mut count = (n + 1).wrapping_mul(n + 1);
        if level % 2 != 0 {
            count = count.wrapping_add(n.wrapping_mul(n));
        }
        self.vertex_count = count;
        let bytes = count as usize * 12;
        let v = &mut self.vertices;
        for s in [&mut v.position, &mut v.normal, &mut v.tex[0]] {
            s.data.get_or_insert_with(Vec::new).resize(bytes, 0);
        }
        self.vertices.count = 0;
        let radius = self.radius;
        let Some(geometry) = self.slots.first_mut() else {
            return;
        };
        geometry.tessellate(radius, 0, level, 0, &mut self.vertices);
        let built = geometry.vertex_count;
        let v = &mut self.vertices;
        v.count = built;
        v.position.count = built;
        v.position.stride = 0xC;
        v.position.byte_size = 0xC;
        v.normal.stride = 0xC;
        v.normal.byte_size = 0xC;
        v.tex[0].stride = 8;
        v.tex[0].byte_size = 8;
        self.flags = (self.flags & !2) | 1;
        v.normal.count = built;
        v.tex[0].count = built;
    }

    /// `FSphereAnim::Draw`: the faces turned towards the camera (the best
    /// face and those with a visible corner), each rotated into place and
    /// drawn with its side's material (or the override), then, without an
    /// override and with more than six sides, the atmosphere shell over the
    /// faces its own wider test keeps.
    #[allow(clippy::too_many_lines)]
    fn draw<S: MeshServices + ?Sized>(
        &mut self,
        archetype: &SphereArchetype,
        services: &mut S,
        camera: &mut dyn MeshCamera,
        instance: Instance,
        request: &DrawRequest,
    ) -> bool {
        let f = f64::from;
        let view_words = camera.view_transform();
        let world_words = services.instance_transform(instance);
        let (view, world) = (transform(view_words), transform(world_words));
        let scaled_radius = archetype.scale * archetype.radius;
        let arch_radius = archetype.radius;
        const FACE_CORNERS: [[usize; 4]; 6] = [
            [0, 3, 4, 7],
            [0, 1, 4, 5],
            [1, 2, 5, 6],
            [2, 3, 6, 7],
            [0, 1, 2, 3],
            [4, 5, 6, 7],
        ];
        let mut normals = [
            Vector::new(0.0, 0.0, 1.0),
            Vector::new(1.0, 0.0, 0.0),
            Vector::new(0.0, 0.0, -1.0),
            Vector::new(-1.0, 0.0, 0.0),
            Vector::new(0.0, 1.0, 0.0),
            Vector::new(0.0, -1.0, 0.0),
        ];
        let mut corners = [
            Vector::new(K, K, K),
            Vector::new(K, K, -K),
            Vector::new(-K, K, -K),
            Vector::new(-K, K, K),
            Vector::new(K, -K, K),
            Vector::new(K, -K, -K),
            Vector::new(-K, -K, -K),
            Vector::new(-K, -K, K),
        ];
        let rotation = world.rotation;
        let mut visible = [false; 8];
        for normal in &mut normals {
            *normal = transform_matrix(&rotation, *normal);
        }
        let camera_position = camera.position();
        let mut to = Vector::new(
            camera_position[0] - world.translation.x,
            camera_position[1] - world.translation.y,
            camera_position[2] - world.translation.z,
        );
        let distance = ((f(to.x) * f(to.x) + f(to.z) * f(to.z)) + f(to.y) * f(to.y)).sqrt() as f32;
        let inv_distance = (1.0 / f(distance)) as f32;
        to.x *= inv_distance;
        to.y *= inv_distance;
        to.z *= inv_distance;
        let mut best = -1.0f32;
        let mut best_face = None;
        for (face, normal) in normals.iter().enumerate() {
            let dot =
                ((f(to.x) * f(normal.x) + f(to.z) * f(normal.z)) + f(to.y) * f(normal.y)) as f32;
            if dot > best {
                best = dot;
                best_face = Some(face);
            }
        }
        let limit = f(inv_distance) * f(arch_radius);
        let threshold = if limit > KF64 { K } else { limit as f32 };
        let best_corners = FACE_CORNERS[best_face.unwrap_or(0)];
        for &corner in &best_corners {
            corners[corner] = transform_matrix(&rotation, corners[corner]);
            let p = corners[corner];
            let dot = ((f(to.z) * f(p.z) + f(to.y) * f(p.y)) + f(to.x) * f(p.x)) as f32;
            visible[corner] = dot > threshold;
        }
        let biased = distance * self.lod_scale;
        let x_scale = camera.x_scale_over_near() as f32;
        let z_near = f(camera.z_near());
        self.update_level((f(biased) / (z_near * f(x_scale))) as f32);
        let mut listing = VbListing::default();
        if let Ok(mut lock) = services.lock_vertex_memory(self.vertices.fvf, self.vertices.count) {
            let fvf = lock.listing.fvf;
            let mut bytes = std::mem::take(&mut lock.bytes);
            services.copy_vertex_data(&mut bytes, fvf, &self.vertices, 0, self.vertices.count);
            lock.bytes = bytes;
            listing = lock.listing;
            services.unlock_vertex_memory(lock);
        }
        let half_pi = (std::f64::consts::PI * 0.5) as f32;
        let Some(geometry) = self.slots.first() else {
            return true;
        };
        let (base_vertex, vertex_count, index_count) = (
            geometry.base_vertex,
            geometry.vertex_count,
            u32::try_from(geometry.indices.len()).unwrap_or(u32::MAX),
        );
        let indices = geometry.indices.clone();
        let args = DrawArgs {
            prim_type: 4,
            start: base_vertex,
            count: vertex_count,
            ib_start: 0,
            index_span: index_count,
            flags: 0,
        };
        let mut face_rot = face_rotation(0, half_pi);
        let len = self.slots.len();
        let mut walk = 0usize;
        for (face, corners_of) in FACE_CORNERS.iter().enumerate() {
            let own = self.slots[walk].material;
            let (material, replaced) = match request.extra {
                Some(extra) => (Some(extra), own),
                None => (own, None),
            };
            let drawn = Some(face) == best_face || corners_of.iter().any(|&c| visible[c]);
            if drawn {
                if face != 0 {
                    face_rot = face_rotation(face, half_pi);
                }
                let face_world = mul_transform(&world, &face_rot).to_words();
                let state = services.pipeline_state(7).unwrap_or(1);
                if state != 0 {
                    let product = mul_transform(&view, &transform(face_world));
                    services.set_modelview(&product.to_words());
                } else {
                    services.set_world(&face_world);
                }
                if let Some(material) = material {
                    let context = SubmitContext {
                        instance,
                        world: &face_world,
                        view: &view_words,
                        vertices: Some(&self.vertices),
                        indices: &indices,
                        vb: Some(&listing),
                        ib: None,
                        replaced,
                        field20: 0,
                    };
                    services.bind(material, &context);
                    services.submit(material, &context, &args);
                    services.teardown(material, &context);
                }
            }
            walk = (walk + 1) % len;
        }
        if !self.has_atmosphere || request.extra.is_some() {
            return true;
        }
        let atmosphere = self.slots[walk].material;
        let limit = f(inv_distance) * f(scaled_radius);
        let threshold = if limit > KF64 { f(K) } else { limit };
        for &corner in &best_corners {
            let p = corners[corner];
            let dot = (f(to.z) * f(p.z) + f(to.y) * f(p.y)) + f(to.x) * f(p.x);
            visible[corner] = dot > threshold;
        }
        for (face, corners_of) in FACE_CORNERS.iter().enumerate() {
            if Some(face) != best_face && !corners_of.iter().any(|&c| visible[c]) {
                continue;
            }
            face_rot = face_rotation(face, half_pi);
            let shell_world = mul_transform(&world, &face_rot).to_words();
            if let Some(material) = atmosphere {
                let context = SubmitContext {
                    instance,
                    world: &shell_world,
                    view: &view_words,
                    vertices: Some(&self.vertices),
                    indices: &indices,
                    vb: Some(&listing),
                    ib: None,
                    replaced: None,
                    field20: FIELD20_ENGINE,
                };
                services.bind(material, &context);
                services.submit(material, &context, &args);
                services.teardown(material, &context);
            }
        }
        true
    }

    /// `ISphereMesh::SetMaxTessellationLevel`: a rebuild follows.
    pub fn set_max_level(&mut self, level: i32) {
        self.level_max = level;
        self.flags |= 2;
    }

    /// `ISphereMesh::SetRadius`: a rebuild follows.
    pub fn set_radius(&mut self, radius: f32) {
        self.radius = radius;
        self.flags |= 2;
    }

    /// `ISphereMesh::SetLodDistanceScale`.
    pub fn set_lod_scale(&mut self, scale: f32) {
        self.lod_scale = scale;
    }

    /// `ISphereMesh::SetMaterial(index, material)`: -1 past the slots.
    pub fn set_material<S: MeshServices + ?Sized>(
        &mut self,
        services: &mut S,
        index: i32,
        material: Option<MaterialId>,
    ) -> i32 {
        let Some(slot) = usize::try_from(index)
            .ok()
            .and_then(|i| self.slots.get_mut(i))
        else {
            return -1;
        };
        let previous = std::mem::replace(&mut slot.material, material);
        if let Some(material) = material {
            services.add_ref_material(material);
        }
        if let Some(previous) = previous {
            services.release_material(previous);
        }
        0
    }
}

/// The `SphereMesh` render plugin (`FSphereMeshPlugin`).
#[derive(Debug, Default)]
pub struct SpherePlugin {
    archetypes: Vec<Option<SphereArchetype>>,
    instances: BTreeMap<Instance, SphereInstance>,
}

impl SpherePlugin {
    /// No archetypes.
    #[must_use]
    pub fn new() -> SpherePlugin {
        SpherePlugin::default()
    }

    /// An archetype by handle (1 and up).
    #[must_use]
    pub fn archetype(&self, handle: Handle) -> Option<&SphereArchetype> {
        let index = usize::try_from(handle).ok()?.checked_sub(1)?;
        self.archetypes.get(index).and_then(Option::as_ref)
    }

    /// An instance (`QueryMeshInstance(IID_ISphereMesh)`).
    pub fn instance_mut(&mut self, instance: Instance) -> Option<&mut SphereInstance> {
        self.instances.get_mut(&instance)
    }
}

fn valid(handle: Handle) -> bool {
    handle != HANDLE_NONE && handle != 0
}

impl<S: MeshServices + ?Sized> RenderComponent<S> for SpherePlugin {
    fn claim(&mut self, services: &mut S, fs: &mut dyn FileSystem) -> Handle {
        let Ok(Some(node)) = sphere::load(fs) else {
            return HANDLE_NONE;
        };
        let archetype = SphereArchetype::load(services, &node);
        self.archetypes.push(Some(archetype));
        u32::try_from(self.archetypes.len()).unwrap_or(HANDLE_NONE)
    }

    fn clone_handle(&mut self, _services: &mut S, _handle: Handle) -> Handle {
        HANDLE_NONE
    }

    fn destroy_handle(&mut self, services: &mut S, handle: Handle) {
        if !valid(handle) {
            return;
        }
        let Some(index) = usize::try_from(handle).ok().and_then(|h| h.checked_sub(1)) else {
            return;
        };
        if let Some(mut archetype) = self.archetypes.get_mut(index).and_then(Option::take) {
            archetype.destroy(services);
        }
    }

    fn query_handle(&mut self, handle: Handle, iid: MeshIid) -> i32 {
        if !valid(handle) || self.archetype(handle).is_none() {
            return -1;
        }
        if iid == MeshIid::MatHookUp { 0 } else { -1 }
    }

    fn create_mesh_instance(
        &mut self,
        services: &mut S,
        instance: Instance,
        handle: Handle,
    ) -> i32 {
        if !valid(handle) {
            return 0;
        }
        let Some(archetype) = self.archetype(handle).cloned() else {
            return 0;
        };
        let anim = SphereInstance::new(services, handle, &archetype);
        let mut extent = archetype.radius;
        if 0.0 <= archetype.radius {
            extent = archetype.scale * archetype.radius;
        }
        services.set_instance_extent(instance, extent, [0.0; 3]);
        self.instances.entry(instance).or_insert(anim);
        1
    }

    fn destroy_mesh_instance(
        &mut self,
        services: &mut S,
        instance: Instance,
        _handle: Handle,
    ) -> i32 {
        if let Some(mut anim) = self.instances.remove(&instance) {
            anim.reset(services);
        }
        0
    }

    fn update_mesh_instance(&mut self, _services: &mut S, _instance: Instance, _arg: u32) -> i32 {
        0
    }

    fn query_mesh_instance(&mut self, instance: Instance, _handle: Handle, iid: MeshIid) -> i32 {
        if !self.instances.contains_key(&instance) {
            return -1;
        }
        match iid {
            MeshIid::SphereMesh | MeshIid::MatHookUp => 0,
            _ => -1,
        }
    }

    fn update_mesh_instances(&mut self, _services: &mut S, _arg: u32) -> i32 {
        0
    }

    fn query_geometry(
        &mut self,
        handle: Handle,
        _lod_bits: u32,
        query: GeometryQuery,
    ) -> (i32, Option<i32>) {
        if !valid(handle) || !query.can_write() {
            return (0, None);
        }
        (1, Some(0))
    }

    fn bounds(&mut self, handle: Handle, _lod: f32) -> (i32, Option<[f32; 6]>) {
        if !valid(handle) {
            return (0, None);
        }
        let Some(archetype) = self.archetype(handle) else {
            return (0, None);
        };
        if archetype.radius >= 0.0 {
            let e = archetype.scale * archetype.radius;
            (1, Some([e, -e, e, -e, e, -e]))
        } else {
            (1, Some([0.0; 6]))
        }
    }

    fn centroid(&mut self, handle: Handle, _lod: f32) -> (i32, Option<[f32; 3]>) {
        if !valid(handle) {
            return (0, None);
        }
        (1, Some([0.0; 3]))
    }

    fn render_instance(
        &mut self,
        services: &mut S,
        camera: &mut dyn MeshCamera,
        _handle: Handle,
        instance: Instance,
        request: &DrawRequest,
    ) -> i32 {
        let Self {
            archetypes,
            instances,
        } = self;
        let Some(anim) = instances.get_mut(&instance) else {
            return 0;
        };
        let index = usize::try_from(anim.archetype)
            .ok()
            .and_then(|h| h.checked_sub(1));
        let Some(archetype) = index
            .and_then(|i| archetypes.get(i))
            .and_then(Option::as_ref)
        else {
            return 0;
        };
        i32::from(anim.draw(archetype, services, camera, instance, request))
    }

    fn bind_mesh_instance(&mut self, _instance: Instance, _handle: Handle) -> i32 {
        -1
    }

    fn sphere_mesh(&mut self, instance: Instance) -> Option<&mut SphereInstance> {
        self.instances.get_mut(&instance)
    }
}
