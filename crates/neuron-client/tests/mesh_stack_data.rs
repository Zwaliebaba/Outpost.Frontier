//! The shipped meshes and materials (`outdir/data`, decision O15) drawn
//! from the render manager through the materials to the device model
//! (work package P4.6, second increment): every `Material library` and its
//! archive's `Texture library` are loaded into one [`RenderStack`] over
//! the recording backend; then each archive with meshes has its
//! `VMeshLibrary` loaded, its render nodes made into archetypes by the
//! client's plugins with the materials hooked up from that library, its
//! wires loaded into the `VMeshWire` component, and an instance of each
//! archetype drawn at every level, counted through the visible-geometry
//! override and drawn with the wire, cloak and environment-map overrides
//! of `FLAppearance` (whose candidate and `IMesh` calls the stack answers).
//! Everything is destroyed again, and the material arena must be empty
//! once the libraries are deleted. Nothing is written or committed.

use std::cell::RefCell;
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use neuron_client::render::appearance::{Appearance, Camera, MeshView};
use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::ff::device::{Device, RecordingBackend};
use neuron_client::render::material::{MaterialBatcher, Shading, ShadingFactory, SurfaceFactory};
use neuron_client::render::mesh::manager::{DRAW_KEEP_CURRENT, DRAW_NO_CULL, DrawRequest};
use neuron_client::render::mesh::{
    Archetype, Instance, RenderComponent, RenderManager, SpherePlugin, TriMeshPlugin, VMeshPlugin,
    VMeshRegistry, dump,
};
use neuron_client::render::stack::{RenderStack, StackAppearance, StackEngine};
use neuron_client::render::texture::{FormatClasses, LibraryConfig, TextureLibrary};
use neuron_core::hash::eq_ignore_case;
use neuron_core::ini::{EngineIni, NoIncludes};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

type Stack = RenderStack<RecordingBackend>;

/// The engine's instances: each instance stands 40 units in front of the
/// camera, keeps the extent the plugins give it and has no children.
#[derive(Default)]
struct Engine {
    archetypes: Rc<RefCell<HashMap<Instance, Archetype>>>,
    extents: HashMap<Instance, (f32, [f32; 3])>,
}

impl StackEngine for Engine {
    fn instance_transform(&mut self, _instance: Instance) -> [f32; 12] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, -40.0]
    }
    fn instance_archetype(&mut self, instance: Instance) -> Archetype {
        self.archetypes
            .borrow()
            .get(&instance)
            .copied()
            .unwrap_or(-1)
    }
    fn instance_extent(&mut self, instance: Instance) -> Option<(f32, [f32; 3])> {
        self.extents.get(&instance).copied()
    }
    fn set_instance_extent(&mut self, instance: Instance, radius: f32, centre: [f32; 3]) {
        self.extents.insert(instance, (radius, centre));
    }
    fn user_data(&mut self, _instance: Instance) -> Option<[u32; 5]> {
        Some([0; 5])
    }
}

/// A camera at the origin looking down -z that sees everything.
struct Still;

impl Camera for Still {
    fn position(&mut self) -> [f32; 3] {
        [0.0; 3]
    }
    fn orientation(&mut self) -> [f32; 9] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    }
    fn viewport(&mut self) -> [i32; 4] {
        [0, 0, 639, 479]
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        35.0
    }
    fn fov_y_deg(&mut self) -> f32 {
        50.0
    }
    fn z_near(&mut self) -> f32 {
        1.0
    }
    fn z_far(&mut self) -> f32 {
        10_000.0
    }
    fn aspect(&mut self) -> f32 {
        4.0 / 3.0
    }
    fn x_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn y_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn project(&mut self, _: [f32; 3], _: &mut [f32; 3]) -> i32 {
        0
    }
    fn unproject(&mut self, _: &mut [f32; 3], _: f32, _: f32) {}
    fn classify_sphere(&mut self, _: [f32; 3], _: f32) -> i32 {
        4
    }
    fn view_transform(&mut self) -> [f32; 12] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
    }
}

fn files(root: &Path, dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            files(root, &path, out);
        } else if fs::File::open(&path)
            .and_then(|mut f| {
                let mut magic = [0u8; 4];
                std::io::Read::read_exact(&mut f, &mut magic).map(|()| magic)
            })
            .is_ok_and(|m| &m == b"UTF ")
        {
            out.push(path.strip_prefix(root).unwrap_or(&path).to_path_buf());
        }
    }
}

const RENDER: [&[u8]; 4] = [b"VMeshPart", b"MultiLevel", b"Mesh", b"Sphere"];

/// The nodes holding a render node or a wireframe, as paths from the root.
fn archetype_nodes(fs: &dyn FileSystem, path: &[u8], depth: usize, out: &mut Vec<(Vec<u8>, bool)>) {
    let Ok(entries) = fs.find(b"*") else {
        return;
    };
    let dirs: Vec<Vec<u8>> = entries
        .iter()
        .filter(|e| e.data.attributes & 0x10 != 0)
        .map(|e| neuron_core::hash::c_str(&e.data.name).to_vec())
        .collect();
    let named = |n: &[u8]| dirs.iter().any(|d| eq_ignore_case(d, n));
    let render = RENDER.iter().any(|&n| named(n));
    let wire = named(b"VMeshWire");
    if render || wire {
        out.push((path.to_vec(), wire));
    }
    if depth >= 8 {
        return;
    }
    for name in dirs {
        let owned = eq_ignore_case(&name, b"VMeshLibrary")
            || eq_ignore_case(&name, b"VMeshWire")
            || RENDER.iter().any(|&n| eq_ignore_case(&name, n));
        if owned || name.is_empty() {
            continue;
        }
        if let Ok(child) = fs.create_child(&FsDesc::read(&name)) {
            let mut sub = path.to_vec();
            sub.push(b'\\');
            sub.extend_from_slice(&name);
            archetype_nodes(child.as_ref(), &sub, depth + 1, out);
        }
    }
}

fn node(root: &UtfFileSystem, path: &[u8]) -> Option<Box<dyn FileSystem>> {
    let mut current: Box<dyn FileSystem> = Box::new(root.clone());
    for part in path.split(|&b| b == b'\\').filter(|p| !p.is_empty()) {
        current = current.create_child(&FsDesc::read(part)).ok()?;
    }
    Some(current)
}

fn open(host: &Host, path: &Path) -> Option<UtfFileSystem> {
    let text = path.to_string_lossy().replace('/', "\\");
    UtfFileSystem::open(
        host,
        &FsDesc::read(text.as_bytes()).with_implementation(b"UTF"),
    )
    .ok()
}

#[test]
#[allow(clippy::too_many_lines)]
fn shipped_meshes_draw_through_the_materials_to_the_device() {
    let base = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let root = base.join("data");
    let mut archives = Vec::new();
    files(&root, &root, &mut archives);
    let Ok(ini_bytes) = fs::read(base.join("NeuronComponents.ini")) else {
        println!("outdir/NeuronComponents.ini is missing: nothing to draw");
        return;
    };
    if archives.is_empty() {
        println!("outdir/data has no UTF archives: nothing to draw");
        return;
    }
    archives.sort();
    let mut ini = EngineIni::new();
    ini.load_buffer(&ini_bytes, &mut NoIncludes)
        .expect("the engine INI parses");
    let device = Device::new(Defaults::builtin(), 640, 480, RecordingBackend::default());
    let shading = Shading::new(
        device,
        TextureLibrary::new(LibraryConfig::default()),
        FormatClasses::builtin(),
    );
    let batcher: MaterialBatcher<Stack> = MaterialBatcher::new(
        Box::new(ShadingFactory::with_fallback(Box::new(
            SurfaceFactory::new(),
        ))),
        Some(&ini),
    );
    let registry = VMeshRegistry::shared();
    let engine = Engine::default();
    let instances = Rc::clone(&engine.archetypes);
    let mut stack = RenderStack::new(shading, batcher, Rc::clone(&registry), Box::new(engine));
    let host = Host::new(&root);

    // Every material and texture library first: the meshes name their
    // materials by CRC from other archives.
    let (mut material_groups, mut texture_groups) = (Vec::new(), Vec::new());
    for path in &archives {
        let Some(mut fs) = open(&host, path) else {
            continue;
        };
        if fs.open_node(b"Material library").is_err() {
            continue;
        }
        let _ = fs.open_node(b"..");
        if fs.open_node(b"Texture library").is_ok() {
            let _ = fs.open_node(b"..");
            let (textures, mut pipeline) = stack.shading.texture_parts();
            texture_groups.extend(textures.load_group(&mut fs, &mut pipeline, true).group);
        }
        let load = stack
            .with_batcher(|b, s| b.library_mut().load_library(s, &mut fs, true))
            .expect("the batcher");
        let Some(group) = load.group else {
            continue;
        };
        stack.with_batcher(|b, s| {
            b.library_mut().apply_verify_group(s, group.cast_signed());
            b.library_mut()
                .resolve_textures_group(s, group.cast_signed(), 0.0);
        });
        material_groups.push(group);
    }
    let materials = stack.batcher().library().materials().live().count();

    // FLAppearance's override materials.
    let vmesh = VMeshPlugin::new(Rc::clone(&registry));
    let trimesh = TriMeshPlugin::new(false);
    let (vmesh_faces, trimesh_faces) = (vmesh.faces(), trimesh.faces());
    let plugins: Vec<Box<dyn RenderComponent<Stack>>> = vec![
        Box::new(vmesh),
        Box::new(trimesh),
        Box::new(SpherePlugin::new()),
    ];
    let mut manager = RenderManager::new(plugins);
    manager.set_faces(0, Rc::new(vmesh_faces));
    manager.set_faces(1, Rc::new(trimesh_faces));
    stack.set_directory(manager.directory());
    let mut appearance = {
        let mut services = StackAppearance {
            stack: &mut stack,
            manager: &mut manager,
        };
        let mut appearance = Appearance::create(&mut services).expect("the appearance");
        assert_eq!(
            appearance.initialize(&mut services),
            0,
            "the appearance's Initialize"
        );
        appearance
    };
    let overrides: Vec<_> = [1u32, 3, 4]
        .iter()
        .map(|&i| appearance.override_material(i).expect("an override"))
        .collect();

    let (mut mesh_archives, mut archetypes, mut levels, mut wires) = (0u64, 0u64, 0u64, 0u64);
    let (mut plain_packets, mut override_packets, mut counted) = (0usize, 0usize, [0u64; 3]);
    let mut leftover = 0usize;
    let mut next_id: Archetype = 1;
    for path in &archives {
        let Some(archive) = open(&host, path) else {
            continue;
        };
        let mut nodes = Vec::new();
        archetype_nodes(&archive, b"", 0, &mut nodes);
        let mut probe: Box<dyn FileSystem> = Box::new(archive.clone());
        let has_library = probe.open_node(b"VMeshLibrary").is_ok();
        if nodes.is_empty() && !has_library {
            continue;
        }
        mesh_archives += 1;
        let mut root_fs: Box<dyn FileSystem> = Box::new(archive.clone());
        let list = registry
            .borrow_mut()
            .load_library(&mut stack, root_fs.as_mut(), -1);
        let mut made = Vec::new();
        for (node_path, has_wire) in &nodes {
            let id = next_id;
            next_id += 1;
            if let Some(mut fs) = node(&archive, node_path)
                && manager.create_archetype(&mut stack, id, fs.as_mut()) == 1
            {
                archetypes += 1;
                made.push(id);
            }
            if *has_wire && let Some(mut fs) = node(&archive, node_path) {
                let mut component = std::mem::take(&mut stack.wires);
                if component.create_archetype(&mut stack, id, fs.as_mut()) == 1 {
                    wires += 1;
                }
                stack.wires = component;
            }
        }
        for &id in &made {
            let instance = usize::try_from(id).expect("a positive id");
            instances.borrow_mut().insert(instance, id);
            assert_eq!(manager.create_instance(&mut stack, instance, id), 1);
            let count = manager.archetype_lod_count(id).unwrap_or(0);
            for level in 0..count {
                manager.set_archetype_lod(id, level);
                let before = stack.shading.device.backend.draws.len();
                let request = DrawRequest {
                    flags: DRAW_KEEP_CURRENT | DRAW_NO_CULL,
                    matrix: None,
                    extra: None,
                };
                manager.render_instance(
                    &mut stack,
                    &mut MeshView(&mut Still),
                    instance,
                    1.0,
                    &request,
                );
                plain_packets += stack.shading.device.backend.draws.len() - before;
                levels += 1;
            }
            manager.set_archetype_lod(id, 0);
            {
                let mut services = StackAppearance {
                    stack: &mut stack,
                    manager: &mut manager,
                };
                if let Some(visible) =
                    appearance.count_visible_geometry(&mut services, &mut Still, instance)
                {
                    for (total, value) in counted.iter_mut().zip(visible) {
                        *total += u64::from(value);
                    }
                }
            }
            for &extra in &overrides {
                let before = stack.shading.device.backend.draws.len();
                let request = DrawRequest {
                    flags: DRAW_KEEP_CURRENT | DRAW_NO_CULL,
                    matrix: None,
                    extra: Some(extra),
                };
                manager.render_instance(
                    &mut stack,
                    &mut MeshView(&mut Still),
                    instance,
                    1.0,
                    &request,
                );
                override_packets += stack.shading.device.backend.draws.len() - before;
            }
            manager.destroy_instance(&mut stack, instance);
        }
        for &id in &made {
            manager.destroy_archetype(&mut stack, id);
            stack.wires.destroy_archetype(id);
        }
        registry.borrow_mut().unload_library(&mut stack, list);
        leftover += registry.borrow().meshes().count();
        stack.shading.device.backend.draws.clear();
    }
    manager.destroy(&mut stack);
    {
        let mut services = StackAppearance {
            stack: &mut stack,
            manager: &mut RenderManager::new(Vec::new()),
        };
        appearance.release(&mut services);
    }
    let missing = stack
        .dumps
        .iter()
        .filter(|d| d.flags == dump::NOTICE && d.text.contains("couldnt find material"))
        .count();
    for group in material_groups {
        stack.with_batcher(|b, s| b.library_mut().delete_group(s, group.cast_signed()));
    }
    let live = stack.batcher().library().materials().live().count();
    let (textures, mut pipeline) = stack.shading.texture_parts();
    for group in texture_groups {
        textures.garbage_collect(group, &mut pipeline);
    }
    println!(
        "{materials} materials loaded; {mesh_archives} archives with meshes: {archetypes} \
         archetypes, {levels} level draws ({plain_packets} draw packets), {wires} wires, \
         {override_packets} draw packets with the wire, cloak and environment-map overrides, \
         visible counts {counted:?}, {missing} face groups without their material, \
         {leftover} meshes left, {live} materials left"
    );
    assert!(archetypes > 0);
    assert!(plain_packets > 0, "no draw reached the device");
    assert!(override_packets > 0, "no override draw reached the device");
    assert!(
        counted[0] > 0,
        "the visible-geometry override counted nothing"
    );
    assert_eq!(leftover, 0, "meshes left in the registry");
    assert_eq!(live, 0, "materials left after the libraries were deleted");
}
