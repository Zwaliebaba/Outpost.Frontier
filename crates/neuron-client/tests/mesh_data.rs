//! The shipped meshes (`outdir/data`, decision O15) through the render
//! manager of work package P4.6: every UTF archive under the data tree has
//! its `VMeshLibrary` loaded into a registry, every node that holds a
//! `VMeshPart`, `MultiLevel`, `Mesh` or `Sphere` made into a render
//! archetype by the client's plugins (`VMesh`, `TriMesh` without strips,
//! `SphereMesh`), and its `VMeshWire` loaded; each archetype gets an
//! instance that is drawn at every level, then everything is destroyed
//! again. The services answer every call with success and one material,
//! and check what the meshes hand them. Nothing is written or compared
//! with the oracle (`neuron-core`'s mesh data dump does that for the
//! loaders).

use std::cell::RefCell;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use neuron_client::render::material::{
    DrawArgs, Material, MaterialId, Materials, SubmitContext, VbListing, VertexLock, VertexSource,
};
use neuron_client::render::mesh::manager::{DRAW_KEEP_CURRENT, DRAW_NO_CULL, DrawRequest};
use neuron_client::render::mesh::wire::draw_wire;
use neuron_client::render::mesh::{
    Instance, MeshCamera, MeshServices, RenderComponent, RenderManager, SpherePlugin,
    TriMeshPlugin, VMeshPlugin, VMeshRegistry, VMeshWire,
};
use neuron_core::hash::eq_ignore_case;
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

struct Dummy;

impl Material<()> for Dummy {
    fn initialize(&mut self, _: &mut ()) -> i32 {
        0
    }
    fn set_name(&mut self, _: &[u8]) -> i32 {
        0
    }
    fn load(&mut self, _: &mut (), _: &mut dyn FileSystem) -> i32 {
        0
    }
    fn apply_verify(&mut self, _: &mut ()) -> i32 {
        0
    }
    fn resolve_textures(&mut self, _: &mut (), _: f32) -> i32 {
        0
    }
    fn render_flags(&mut self, _: &mut ()) -> u32 {
        0
    }
    fn submit(&mut self, _: &mut (), _: &SubmitContext<'_>, _: &DrawArgs) -> i32 {
        0
    }
}

/// Services that succeed: buffers are numbers, one material answers every
/// CRC, and every submission is checked against its vertex source and
/// indices.
#[derive(Default)]
struct Counting {
    material: Option<MaterialId>,
    refs: i64,
    next: u32,
    live_vb: Vec<u32>,
    live_ib: Vec<u32>,
    submits: u64,
    triangles: u64,
    bad: Vec<String>,
    dumps: u64,
}

impl MeshServices for Counting {
    fn pipeline_state(&mut self, _id: u32) -> Option<i32> {
        Some(1)
    }
    fn set_modelview(&mut self, _: &[f32; 12]) {}
    fn set_world(&mut self, _: &[f32; 12]) {}
    fn set_view(&mut self, _: &[f32; 12]) {}
    fn lock_vertex_memory(&mut self, fvf: u32, _count: u32) -> Result<VertexLock, i32> {
        Ok(VertexLock {
            listing: VbListing {
                vertex_buffer: 0x900,
                fvf,
                base_vertex: 0,
            },
            bytes: Vec::new(),
            offset: 0,
        })
    }
    fn copy_vertex_data(&mut self, _: &mut [u8], _: u32, _: &VertexSource, _: u32, _: u32) -> i32 {
        0
    }
    fn unlock_vertex_memory(&mut self, _: VertexLock) {}
    fn select_vb(&mut self, vb: u32) -> i32 {
        if self.live_vb.contains(&vb) { 0 } else { -1 }
    }
    fn create_vb(&mut self, _: u32, _: u32) -> Result<u32, i32> {
        self.next += 1;
        self.live_vb.push(self.next);
        Ok(self.next)
    }
    fn lock_vb(&mut self, _: u32, _: u32) -> i32 {
        0
    }
    fn copy_vertices(&mut self, _: u32, base: u32, _: &VertexSource, _: u32, _: u32) -> (i32, u32) {
        (0, base)
    }
    fn unlock_vb(&mut self, _: u32) {}
    fn destroy_vb(&mut self, vb: u32) {
        self.live_vb.retain(|&h| h != vb);
    }
    fn select_ib(&mut self, ib: u32, _: u32) -> i32 {
        if ib == u32::MAX || self.live_ib.contains(&ib) {
            0
        } else {
            -1
        }
    }
    fn create_ib(&mut self, _: u32) -> Result<u32, i32> {
        self.next += 1;
        self.live_ib.push(self.next);
        Ok(self.next)
    }
    fn lock_ib(&mut self, _: u32, _: u32) -> i32 {
        0
    }
    fn unlock_ib(&mut self, _: u32, _: &[u16]) {}
    fn destroy_ib(&mut self, ib: u32) {
        self.live_ib.retain(|&h| h != ib);
    }
    fn copy_scratch_indices(&mut self, _: &[u16]) -> u32 {
        0
    }
    fn draw_indexed(&mut self, _: u32, _: u32, _: u32, _: u32, _: u32) -> i32 {
        0
    }
    fn instance_transform(&mut self, _: Instance) -> [f32; 12] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 100.0]
    }
    fn instance_archetype(&mut self, instance: Instance) -> i32 {
        i32::try_from(instance).unwrap_or(-1)
    }
    fn release_handle(&mut self, _: i32) {}
    fn instance_extent(&mut self, _: Instance) -> Option<(f32, [f32; 3])> {
        Some((10.0, [0.0; 3]))
    }
    fn set_instance_extent(&mut self, _: Instance, _: f32, _: [f32; 3]) {}
    fn related_instance(&mut self, _: Instance, _: Option<Instance>) -> Option<Instance> {
        None
    }
    fn instance_root(&mut self, _: Instance) {}
    fn find_material(&mut self, _crc: u32) -> (i32, Option<MaterialId>) {
        self.refs += 1;
        (0, self.material)
    }
    fn add_ref_material(&mut self, _: MaterialId) {
        self.refs += 1;
    }
    fn release_material(&mut self, _: MaterialId) {
        self.refs -= 1;
    }
    fn material_flags(&mut self, _: MaterialId) -> u32 {
        8
    }
    fn material_class(&mut self, _: MaterialId) -> Vec<u8> {
        b"Shading".to_vec()
    }
    fn material_parameter_count(&mut self, _: MaterialId) -> u32 {
        0
    }
    fn material_parameter(&mut self, _: MaterialId, _: u32) -> (u32, u32) {
        (0, 0)
    }
    fn bind(&mut self, _: MaterialId, _: &SubmitContext<'_>) {}
    fn submit(&mut self, _: MaterialId, context: &SubmitContext<'_>, args: &DrawArgs) {
        self.submits += 1;
        let end = args.ib_start as usize + args.index_span as usize;
        if end > context.indices.len() {
            self.bad.push(format!(
                "indices {}..{end} of {}",
                args.ib_start,
                context.indices.len()
            ));
        }
        if context.vertices.is_none() {
            self.bad.push("no vertex source".to_owned());
        }
        self.triangles += u64::from(args.index_span / 3);
    }
    fn teardown(&mut self, _: MaterialId, _: &SubmitContext<'_>) {}
    fn dump(&mut self, _: i32, _: &str, _: u32, _: &str) {
        self.dumps += 1;
    }
    fn log_event(&mut self, _: &str, _: f32, _: u32) {}
}

struct Camera;

impl MeshCamera for Camera {
    fn view_transform(&mut self) -> [f32; 12] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
    }
    fn classify_sphere(&mut self, _: [f32; 3], _: f32) -> i32 {
        4
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        45.0
    }
    fn viewport(&mut self) -> [i32; 4] {
        [0, 0, 1023, 767]
    }
    fn x_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn z_near(&mut self) -> f32 {
        1.0
    }
    fn position(&mut self) -> [f32; 3] {
        [0.0, 0.0, 0.0]
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

/// The node at `path` (components after the root, `\` separated).
fn node(root: &UtfFileSystem, path: &[u8]) -> Option<Box<dyn FileSystem>> {
    let mut current: Box<dyn FileSystem> = Box::new(root.clone());
    for part in path.split(|&b| b == b'\\').filter(|p| !p.is_empty()) {
        current = current.create_child(&FsDesc::read(part)).ok()?;
    }
    Some(current)
}

#[test]
fn shipped_meshes_load_draw_and_unload() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut archives = Vec::new();
    files(&root, &root, &mut archives);
    if archives.is_empty() {
        println!("outdir/data has no UTF archives: nothing to load");
        return;
    }
    archives.sort();
    let host = Host::new(&root);
    let mut arena: Materials<()> = Materials::new();
    let mut services = Counting {
        material: Some(arena.insert(Box::new(Dummy), 1)),
        ..Counting::default()
    };
    let (mut archives_with_meshes, mut libraries, mut library_meshes) = (0u64, 0u64, 0u64);
    let (mut archetypes, mut levels, mut wires, mut draws) = (0u64, 0u64, 0u64, 0u64);
    let mut by_plugin = [0u64; 3];
    let mut leftover = 0u64;
    for path in &archives {
        let text = path.to_string_lossy().replace('/', "\\");
        let desc = FsDesc::read(text.as_bytes()).with_implementation(b"UTF");
        let Ok(archive) = UtfFileSystem::open(&host, &desc) else {
            continue;
        };
        let mut nodes = Vec::new();
        archetype_nodes(&archive, b"", 0, &mut nodes);
        let mut probe: Box<dyn FileSystem> = Box::new(archive.clone());
        let has_library = probe.open_node(b"VMeshLibrary").is_ok();
        if nodes.is_empty() && !has_library {
            continue;
        }
        archives_with_meshes += 1;
        let registry = VMeshRegistry::shared();
        let plugins: Vec<Box<dyn RenderComponent<Counting>>> = vec![
            Box::new(VMeshPlugin::new(Rc::clone(&registry))),
            Box::new(TriMeshPlugin::new(false)),
            Box::new(SpherePlugin::new()),
        ];
        let mut manager = RenderManager::new(plugins);
        let mut wire = VMeshWire::new();
        let mut root_fs: Box<dyn FileSystem> = Box::new(archive.clone());
        let list = registry
            .borrow_mut()
            .load_library(&mut services, root_fs.as_mut(), -1);
        if let Some(list) = &list {
            libraries += 1;
            library_meshes += list.len() as u64;
        }
        let mut made = Vec::new();
        for (index, (node_path, has_wire)) in nodes.iter().enumerate() {
            let id = i32::try_from(index + 1).unwrap_or(i32::MAX);
            let Some(mut fs) = node(&archive, node_path) else {
                continue;
            };
            if manager.create_archetype(&mut services, id, fs.as_mut()) == 1 {
                archetypes += 1;
                made.push(id);
                let set = manager.lod_set(id).expect("the archetype's levels");
                levels += set.entries.len() as u64;
                for entry in &set.entries {
                    if let Some(p) = entry.plugin {
                        by_plugin[p] += 1;
                    }
                }
            }
            if *has_wire
                && let Some(mut fs) = node(&archive, node_path)
                && wire.create_archetype(&mut services, id, fs.as_mut()) == 1
            {
                wires += 1;
                if let Some(w) = wire.wire(id).cloned() {
                    let r = draw_wire(&w, &mut registry.borrow_mut(), &mut services);
                    assert!(r == 0 || r == -8 || r == -1, "{text}: wire draw {r}");
                }
            }
        }
        for &id in &made {
            let instance = usize::try_from(id).unwrap_or(0);
            assert_eq!(
                manager.create_instance(&mut services, instance, id),
                1,
                "{text}: instance"
            );
            let count = manager.archetype_lod_count(id).unwrap_or(0);
            for level in 0..count {
                manager.set_archetype_lod(id, level);
                let request = DrawRequest {
                    flags: DRAW_KEEP_CURRENT | DRAW_NO_CULL,
                    matrix: None,
                    extra: None,
                };
                manager.render_instance(&mut services, &mut Camera, instance, 1.0, &request);
                draws += 1;
            }
            manager.destroy_instance(&mut services, instance);
        }
        for &id in &made {
            manager.destroy_archetype(&mut services, id);
        }
        manager.destroy(&mut services);
        registry.borrow_mut().unload_library(&mut services, list);
        let left = RefCell::borrow(&registry).meshes().count();
        leftover += left as u64;
    }
    println!(
        "{archives_with_meshes} archives with meshes: {libraries} libraries ({library_meshes} meshes), \
         {archetypes} archetypes ({levels} levels: VMesh {}, TriMesh {}, SphereMesh {}), {wires} wires, \
         {draws} level draws, {} submissions ({} triangles), {} dumps, {leftover} meshes left, {} material references left",
        by_plugin[0],
        by_plugin[1],
        by_plugin[2],
        services.submits,
        services.triangles,
        services.dumps,
        services.refs
    );
    assert!(
        services.bad.is_empty(),
        "submissions outside their data: {:?}",
        &services.bad[..services.bad.len().min(10)]
    );
    assert!(archetypes > 0);
    assert_eq!(leftover, 0, "meshes left in the registries");
    assert_eq!(services.refs, 0, "material references left");
}
