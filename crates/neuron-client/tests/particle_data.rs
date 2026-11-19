//! Every shipped particle effect (`outdir/data`, decision O15) through the
//! simulation of work package P4.8: all `.ale` files load into one node
//! library and one effect library, as the client loads them, and every
//! effect is instantiated under a folder, stepped for a second of frames,
//! told to stop emitting, stepped until its appearances finish (or a time
//! limit), and released. Nothing is compared with the oracle (the
//! synthetic suites `particle_ramp` and `particle_sim` do that); the test
//! checks that no effect panics, that every record finds its node, that
//! particles are made, that the particle appearances carry their life and
//! death effects and the mesh appearances ask a mesh factory for meshes
//! (each given back), and that the pools are whole again at the end.
//! A second test draws every effect through `RenderStack` to the device
//! model's recording backend. Nothing either computes is committed.

use std::fs;
use std::path::{Path, PathBuf};

use neuron_client::render::appearance::Camera;
use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::ff::device::{Device, RecordingBackend};
use neuron_client::render::material::{MaterialBatcher, Shading, ShadingFactory};
use neuron_client::render::mesh::{Archetype, Instance, VMeshRegistry};
use neuron_client::render::particles::services::{FrameOut, TextureRecord};
use neuron_client::render::particles::{
    Camera as FxCamera, Effect, FxMesh, FxMeshFactory, NoTextures, ParticleFx, ParticleServices,
    PoolKind,
};
use neuron_client::render::stack::{RenderStack, StackEngine};
use neuron_client::render::texture::{FormatClasses, LibraryConfig, TextureLibrary};
use neuron_core::fx_data::{self as fx, AleFile, NodeClass};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

fn ale_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            ale_files(&path, out);
        } else if path
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("ale"))
        {
            out.push(path);
        }
    }
}

/// What the loaded ramps hold that the reference would read past its
/// tables or keys for (divergence D370): flat ramps of more than one key
/// with a mode above 6 (the shipped data has seven such ramps, each with a
/// single key, whose interpolator is never called), nested ramps without
/// keys, loop types outside 0 to 4 per nibble.
#[derive(Default, Debug)]
struct Oddities {
    flat_modes: usize,
    empty_nested: usize,
    loop_types: usize,
}

impl Oddities {
    fn anim(&mut self, ramp: &fx::AnimatedSingleRamp) {
        self.empty_nested += usize::from(ramp.keys.is_empty());
        for key in &ramp.keys {
            let lt = key.value.loop_type();
            self.loop_types += usize::from(lt & 0xF > 4 || (lt >> 4) & 0xF > 4 || lt > 0xFF);
        }
    }

    fn single(&mut self, ramp: &fx::RampSingleRamp) {
        self.empty_nested += usize::from(ramp.keys.is_empty());
        for key in &ramp.keys {
            self.flat_modes += usize::from(key.value.mode > 6 && key.value.keys.len() > 1);
        }
    }

    fn slot(&mut self, slot: Option<&fx::NestedSingle>) {
        match slot {
            Some(fx::NestedSingle::Ramp(r)) => self.single(r),
            Some(fx::NestedSingle::Animated(r)) => self.anim(r),
            None => {}
        }
    }

    fn set(&mut self, set: &fx::PropertySet) {
        set.transform.channels.iter().for_each(|c| self.anim(c));
        match &set.data {
            fx::ClassData::Emitter(e) => {
                for r in [
                    &e.frequency,
                    &e.max_particles,
                    &e.init_life_span,
                    &e.pressure,
                    &e.emit_count,
                    &e.velocity_approach,
                ] {
                    self.anim(r);
                }
                self.single(&e.lod_curve);
            }
            fx::ClassData::Billboard(b) => {
                self.single(&b.lod_curve);
                self.empty_nested += usize::from(b.color.keys.is_empty());
                for key in &b.color.keys {
                    self.flat_modes += usize::from(key.value.mode > 6 && key.value.keys.len() > 1);
                }
                for slot in [
                    &b.size,
                    &b.primary_size,
                    &b.tex_frame,
                    &b.axis_override,
                    &b.frame,
                    &b.common_tex_frame,
                ] {
                    self.slot(slot.as_ref());
                }
            }
            fx::ClassData::Field(fx::Field::Radial { attenuation, .. }) => self.single(attenuation),
            _ => {}
        }
    }
}

const IDENTITY: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];
const DT: f32 = 1.0 / 30.0;

/// No textures, and a mesh factory that counts the meshes it hands out and
/// gets back.
#[derive(Default)]
struct Counting {
    made: u32,
    released: u32,
    posed: u32,
    updated: u32,
}

impl FxMeshFactory for Counting {
    fn create_mesh(&mut self, _id: u32, _name: &[u8], _six: &[f32; 6]) -> (i32, Option<FxMesh>) {
        self.made += 1;
        (0, Some(self.made))
    }
    fn set_mesh_transform(&mut self, _mesh: FxMesh, transform: &[f32; 12]) {
        assert!(transform.iter().all(|v| v.is_finite()), "a finite pose");
        self.posed += 1;
    }
    fn mesh_transform(&mut self, _mesh: FxMesh) -> [f32; 12] {
        IDENTITY
    }
    fn update_mesh(&mut self, _mesh: FxMesh, _dt: f32) {
        self.updated += 1;
    }
    fn draw_mesh(&mut self, _mesh: FxMesh, _camera: &mut dyn FxCamera) {}
    fn drop_mesh(&mut self, _mesh: FxMesh) {}
    fn release_mesh(&mut self, _mesh: FxMesh) {
        self.released += 1;
    }
}

impl ParticleServices for Counting {
    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<TextureRecord> {
        NoTextures.find_texture(name, crc)
    }
    fn create_playback(&mut self, _record: TextureRecord) -> i32 {
        -1
    }
    fn frame_count(&mut self, _record: TextureRecord) -> Option<i32> {
        None
    }
    fn release_texture_record(&mut self, _record: TextureRecord) {}
    fn set_wrap_mode(&mut self, _id: i32, _mode: i32) {}
    fn texture_frame(&mut self, _id: i32, _frame: i32) -> Option<FrameOut> {
        None
    }
    fn advance_time(&mut self, _id: i32, _seconds: f32) {}
    fn dec_ref_texture(&mut self, _id: i32) {}
    fn mesh_factory(&mut self) -> Option<&mut dyn FxMeshFactory> {
        Some(self)
    }
}

/// The child effects (life and death) the particle appearances of an
/// effect's records carry now.
fn child_effects(fx: &ParticleFx, effect: &Effect) -> (usize, usize) {
    let (mut life, mut death) = (0, 0);
    for record in effect.records() {
        let Some(node) = fx.node(record.node) else {
            continue;
        };
        if node.class() != NodeClass::ParticleAppearance {
            continue;
        }
        let mut cursor = node.appearance().and_then(|a| a.head);
        while let Some(index) = cursor {
            let r = fx.pools.record(index);
            life += usize::from(r.life.is_some());
            death += usize::from(r.death.is_some());
            cursor = fx.pools.get(PoolKind::Particle).item(index).next;
        }
    }
    (life, death)
}

#[test]
fn every_shipped_effect_runs() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut files = Vec::new();
    ale_files(&root, &mut files);
    if files.is_empty() {
        println!("{} has no .ale files: nothing to run", root.display());
        return;
    }
    files.sort();
    let mut fx = ParticleFx::new();
    let mut hashes = Vec::new();
    let mut oddities = Oddities::default();
    for path in &files {
        let bytes = fs::read(path).expect("readable");
        let ale = AleFile::parse(bytes).expect("a UTF archive");
        for node in ale.nodes.nodes() {
            oddities.set(&node.props);
        }
        hashes.extend(
            ale.effects
                .effects()
                .iter()
                .map(|e| neuron_core::fx_data::name_hash(&e.name)),
        );
        fx.load(&ale);
    }
    hashes.sort_unstable();
    hashes.dedup();
    let mut svc = Counting::default();
    let (mut created, mut missing, mut emitting, mut finished) = (0, 0, 0, 0);
    let mut peak = 0_u32;
    let mut child_classes = 0;
    let (mut with_life, mut with_death, mut with_meshes) = (0, 0, 0);
    for &hash in &hashes {
        let parent = fx.create_node(NodeClass::Node);
        let mut effect = match fx.create_effect(parent, hash, &mut svc) {
            Ok(effect) => effect,
            Err(_) => {
                missing += 1;
                fx.release(parent, &mut svc);
                continue;
            }
        };
        created += 1;
        child_classes += effect
            .records()
            .iter()
            .filter(|r| {
                matches!(
                    fx.node(r.node).map(|n| n.class()),
                    Some(NodeClass::ParticleAppearance | NodeClass::MeshAppearance)
                )
            })
            .count();
        let mut live_peak = 0;
        let (mut saw_life, mut saw_death) = (false, false);
        let meshes_before = svc.made;
        let frame = |effect: &Effect, fx: &mut ParticleFx, svc: &mut Counting| {
            effect.set_transform(fx, 0, &IDENTITY);
            effect.compose(fx, svc);
            effect.update(fx, DT, svc);
            let live: u32 = effect
                .records()
                .iter()
                .filter_map(|r| fx.node(r.node).and_then(|n| n.appearance()).map(|a| a.live))
                .sum();
            live
        };
        for _ in 0..30 {
            live_peak = live_peak.max(frame(&effect, &mut fx, &mut svc));
            let (life, death) = child_effects(&fx, &effect);
            saw_life |= life > 0;
            saw_death |= death > 0;
        }
        effect.dispatch(&mut fx, 1, &mut svc);
        let mut done = false;
        for _ in 0..600 {
            frame(&effect, &mut fx, &mut svc);
            let (life, death) = child_effects(&fx, &effect);
            saw_life |= life > 0;
            saw_death |= death > 0;
            if effect.finished(&fx, 4) {
                done = true;
                break;
            }
        }
        with_life += usize::from(saw_life);
        with_death += usize::from(saw_death);
        with_meshes += usize::from(svc.made > meshes_before);
        if live_peak > 0 {
            emitting += 1;
        }
        if done {
            finished += 1;
        }
        peak = peak.max(live_peak);
        effect.dispatch(&mut fx, 7, &mut svc);
        effect.release(&mut fx, &mut svc);
        fx.release(parent, &mut svc);
    }
    // Every particle went back to its pool.
    for kind in PoolKind::ALL {
        let pool = fx.pools.get(kind);
        assert_eq!(pool.free_count(), pool.capacity(), "{kind:?} pool leaked");
    }
    println!(
        "{} files, {} effects: {created} created, {missing} with a missing node, \
         {emitting} emitted particles (peak {peak} live), {finished} finished after the stop; \
         {child_classes} particle and mesh appearance records: {with_life} effects carried \
         life effects, {with_death} death effects, {with_meshes} made meshes ({} meshes, \
         {} poses, {} updates)",
        files.len(),
        hashes.len(),
        svc.made,
        svc.posed,
        svc.updated
    );
    assert_eq!(svc.made, svc.released, "every mesh went back");
    assert!(with_life > 0 && with_death > 0, "the child effects run");
    assert!(with_meshes > 0, "the mesh appearances ask for meshes");
    assert_eq!(oddities.flat_modes, 0, "an interpolator past the table");
    assert_eq!(oddities.empty_nested, 0, "a nested ramp without keys");
    assert_eq!(
        oddities.loop_types, 0,
        "a loop type the track does not know"
    );
    assert_eq!(missing, 0, "every record finds its node");
    assert!(emitting > created / 2, "most effects make particles");
}

/// The engine the stack needs; particle draws ask it nothing.
struct NoEngine;

impl StackEngine for NoEngine {
    fn instance_transform(&mut self, _instance: Instance) -> [f32; 12] {
        IDENTITY
    }
    fn instance_archetype(&mut self, _instance: Instance) -> Archetype {
        -1
    }
    fn instance_extent(&mut self, _instance: Instance) -> Option<(f32, [f32; 3])> {
        None
    }
    fn set_instance_extent(&mut self, _instance: Instance, _radius: f32, _centre: [f32; 3]) {}
}

/// A camera behind the effects, looking down -z, backing away a little each
/// frame (the dust's speed fade reads the step).
struct Viewer {
    z: f32,
}

impl Camera for Viewer {
    fn position(&mut self) -> [f32; 3] {
        [0.0, 0.0, self.z]
    }
    fn orientation(&mut self) -> [f32; 9] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    }
    fn viewport(&mut self) -> [i32; 4] {
        [0, 0, 639, 479]
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        45.0
    }
    fn fov_y_deg(&mut self) -> f32 {
        60.0
    }
    fn z_near(&mut self) -> f32 {
        1.0
    }
    fn z_far(&mut self) -> f32 {
        10_000.0
    }
    fn aspect(&mut self) -> f32 {
        1.333
    }
    fn x_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn y_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn project(&mut self, _world: [f32; 3], _out: &mut [f32; 3]) -> i32 {
        -1
    }
    fn unproject(&mut self, _out: &mut [f32; 3], _x: f32, _y: f32) {}
    fn classify_sphere(&mut self, _centre: [f32; 3], _radius: f32) -> i32 {
        4
    }
    fn view_transform(&mut self) -> [f32; 12] {
        [
            1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, -self.z,
        ]
    }
}

fn txm_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            txm_files(&path, out);
        } else if path
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("txm"))
        {
            out.push(path);
        }
    }
}

/// The texture libraries of the archives (a `.txm` file, or an `.ale` that
/// holds one), loaded into the stack's texture library.
fn load_textures(
    stack: &mut RenderStack<RecordingBackend>,
    root: &Path,
    files: &[PathBuf],
) -> usize {
    let host = Host::new(root);
    let mut groups = 0;
    for path in files {
        let relative = path.strip_prefix(root).unwrap_or(path);
        let text = relative.to_string_lossy().replace('/', "\\");
        let Ok(mut fs) = UtfFileSystem::open(
            &host,
            &FsDesc::read(text.as_bytes()).with_implementation(b"UTF"),
        ) else {
            continue;
        };
        if fs.open_node(b"Texture library").is_err() {
            continue;
        }
        let _ = fs.open_node(b"..");
        let (textures, mut pipeline) = stack.shading.texture_parts();
        groups += usize::from(
            textures
                .load_group(&mut fs, &mut pipeline, true)
                .group
                .is_some(),
        );
    }
    groups
}

/// Every shipped effect drawn through [`RenderStack`] to the device model's
/// recording backend: each is stepped for 20 frames under a moving camera
/// and drawn on every fifth (alternately through the batch lists), its
/// bounding sphere taken, then released. The spheres must be finite, every
/// begin and end succeed and the pools be whole at the end.
#[test]
#[allow(clippy::too_many_lines)]
fn every_shipped_effect_draws_through_the_stack() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut files = Vec::new();
    ale_files(&root, &mut files);
    if files.is_empty() {
        println!("{} has no .ale files: nothing to draw", root.display());
        return;
    }
    files.sort();
    let mut txm = Vec::new();
    txm_files(&root.join("FX"), &mut txm);
    txm.sort();
    let device = Device::new(Defaults::builtin(), 640, 480, RecordingBackend::default());
    let shading = Shading::new(
        device,
        TextureLibrary::new(LibraryConfig::default()),
        FormatClasses::builtin(),
    );
    let batcher: MaterialBatcher<RenderStack<RecordingBackend>> =
        MaterialBatcher::new(Box::new(ShadingFactory::new()), None);
    let mut stack = RenderStack::new(
        shading,
        batcher,
        VMeshRegistry::shared(),
        Box::new(NoEngine),
    );
    let texture_groups =
        load_textures(&mut stack, &root, &txm) + load_textures(&mut stack, &root, &files);
    let mut fx = ParticleFx::new();
    let mut hashes = Vec::new();
    for path in &files {
        let ale = AleFile::parse(fs::read(path).expect("readable")).expect("a UTF archive");
        hashes.extend(ale.effects.effects().iter().map(|e| fx::name_hash(&e.name)));
        fx.load(&ale);
    }
    hashes.sort_unstable();
    hashes.dedup();
    let mut camera = Viewer { z: 30.0 };
    let (mut drawn, mut packets, mut with_sphere, mut frames, mut textured) = (0, 0, 0, 0, 0);
    let mut vertices = 0_usize;
    for &hash in &hashes {
        let parent = fx.create_node(NodeClass::Node);
        let Ok(mut effect) = fx.create_effect(parent, hash, &mut stack) else {
            fx.release(parent, &mut stack);
            continue;
        };
        let before = stack.shading.device.backend.draws.len();
        let mut sphere_seen = false;
        for frame in 1..=20 {
            camera.z += 0.25;
            effect.set_transform(&mut fx, 0, &IDENTITY);
            effect.compose(&mut fx, &mut stack);
            effect.update(&mut fx, DT, &mut stack);
            if frame % 5 != 0 {
                continue;
            }
            frames += 1;
            fx.set_use_material_batcher(frame % 10 == 0);
            assert_eq!(fx.begin_draw(&mut stack), 0, "begin");
            effect.draw(&mut fx, &mut camera, &mut stack);
            assert_eq!(fx.end_draw(&mut camera, &mut stack), 0, "end");
            let mut sphere = [0.0; 4];
            if effect.bounds(&fx, &mut sphere) {
                assert!(
                    sphere.iter().all(|v| v.is_finite()),
                    "a finite sphere: {sphere:?}"
                );
                sphere_seen = true;
            }
        }
        textured += usize::from(effect.records().iter().any(|r| {
            fx.node(r.node)
                .and_then(|n| n.appearance())
                .is_some_and(|a| a.texture.handle != -1)
        }));
        let draws = &stack.shading.device.backend.draws[before..];
        vertices += draws.iter().map(|p| p.vertex_count as usize).sum::<usize>();
        packets += draws.len();
        drawn += usize::from(!draws.is_empty());
        with_sphere += usize::from(sphere_seen);
        effect.dispatch(&mut fx, 7, &mut stack);
        effect.release(&mut fx, &mut stack);
        fx.release(parent, &mut stack);
        stack.shading.device.backend.draws.clear();
    }
    for kind in PoolKind::ALL {
        let pool = fx.pools.get(kind);
        assert_eq!(pool.free_count(), pool.capacity(), "{kind:?} pool leaked");
    }
    println!(
        "{} effects drawn on {frames} frames: {drawn} reached the device ({packets} draw packets, \
         {vertices} vertices), {with_sphere} with a bounding sphere, {textured} with a texture \
         from the {texture_groups} texture libraries of the effects",
        hashes.len()
    );
    assert!(drawn > hashes.len() / 2, "most effects draw");
    assert!(textured > 0, "the effects find their textures");
}
