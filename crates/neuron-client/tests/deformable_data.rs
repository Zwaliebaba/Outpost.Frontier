//! Every shipped deformable model (`outdir/data`, decision O15) through
//! work package P4.7: each `.dfm` file becomes an instance as the client
//! makes one (`Deformable::create` with the scene engine of P1.8 making
//! the bone and skeleton archetypes, the instance tree and the bone link,
//! and the hardpoint component connecting the parts), every bound bone is
//! posed through its joint and given the engine's transform, and every
//! level of every part is skinned and drawn against counting services.
//! Nothing is compared with the oracle (the synthetic suite `deformable`
//! does that); the test checks that every file loads, that every mesh
//! skins to finite positions and normals, and that the instance tears
//! down. Nothing it computes is committed.

use std::cell::RefCell;
use std::collections::{BTreeSet, HashMap};
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use neuron_client::render::deformable::{
    BoneLink, ChannelSample, DeformServices, Deformable, EngineHandle, Instance, LodDesc, PartDesc,
    UvBinding,
};
use neuron_client::render::material::{
    DrawArgs, Material, MaterialId, Materials, SubmitContext, VertexSource,
};
use neuron_client::render::mesh::MeshCamera;
use neuron_core::scene::{
    ArchetypeId, Engine, EngineComponent, Hardpoints, InstanceId, SceneError, connect_hardpoints,
};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

fn dfm_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            dfm_files(&path, out);
        } else if path
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("dfm"))
        {
            out.push(path);
        }
    }
}

/// The instances the engine makes, in order, for the bone link.
#[derive(Debug, Default)]
struct Made(Rc<RefCell<Vec<(InstanceId, ArchetypeId)>>>);

impl EngineComponent for Made {
    fn create_archetype(
        &mut self,
        _: ArchetypeId,
        _: &mut dyn FileSystem,
    ) -> Result<(), SceneError> {
        Ok(())
    }
    fn create_instance(&mut self, instance: InstanceId, archetype: ArchetypeId) {
        self.0.borrow_mut().push((instance, archetype));
    }
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
}

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

#[derive(Debug, Default)]
struct Counts {
    meshes_missing: u64,
    eye_binds: u64,
    submits: u64,
    nonfinite: u64,
    written_vertices: u64,
    connected: u64,
}

struct Services {
    engine: Engine,
    made: Rc<RefCell<Vec<(InstanceId, ArchetypeId)>>>,
    archetypes: Vec<ArchetypeId>,
    plain: MaterialId,
    eye: MaterialId,
    live_vb: BTreeSet<u32>,
    live_ib: BTreeSet<u32>,
    next: u32,
    vb_sizes: HashMap<u32, usize>,
    counts: Counts,
}

impl Services {
    fn handle(&mut self, id: Option<ArchetypeId>) -> EngineHandle {
        let Some(id) = id else { return -1 };
        let at = self
            .archetypes
            .iter()
            .position(|&a| a == id)
            .unwrap_or_else(|| {
                self.archetypes.push(id);
                self.archetypes.len() - 1
            });
        i32::try_from(at).unwrap_or(-1)
    }
    fn arch(&self, handle: EngineHandle) -> Option<ArchetypeId> {
        usize::try_from(handle)
            .ok()
            .and_then(|i| self.archetypes.get(i))
            .copied()
    }
    fn inst(instance: Instance) -> Option<InstanceId> {
        u32::try_from(instance).ok().map(InstanceId::from_index)
    }
    fn id(instance: Option<InstanceId>) -> Instance {
        instance.map_or(-1, |i| i32::try_from(i.index()).unwrap_or(-1))
    }
}

impl DeformServices for Services {
    fn create_archetype(&mut self, name: &[u8], fs: &mut dyn FileSystem) -> EngineHandle {
        let id = self
            .engine
            .create_archetype_handle(name, Some(fs))
            .ok()
            .flatten();
        self.handle(id)
    }
    fn create_skeleton_archetype(&mut self, name: &[u8], fs: &mut dyn FileSystem) -> EngineHandle {
        self.create_archetype(name, fs)
    }
    fn release_handle(&mut self, handle: EngineHandle) {
        if let Some(id) = self.arch(handle) {
            self.engine.release_handle(id);
        }
    }
    fn query_vmesh_part(&mut self, _: EngineHandle) -> Result<u32, i32> {
        Err(-1)
    }
    fn add_ref_vmesh_part(&mut self, _: u32) {}
    fn release_vmesh_part(&mut self, _: u32) {}
    fn draw_vmesh_part(&mut self, _: u32, _: Instance) {}
    fn create_instance(
        &mut self,
        handle: EngineHandle,
        link: Option<&mut dyn BoneLink>,
    ) -> Instance {
        let Some(arch) = self.arch(handle) else {
            return -1;
        };
        self.made.borrow_mut().clear();
        let root = self.engine.create_instance(arch);
        let made = std::mem::take(&mut *self.made.borrow_mut());
        if let Some(link) = link {
            for (instance, archetype) in made {
                let h = self.handle(Some(archetype));
                link.link(self, Self::id(Some(instance)), h);
            }
        }
        Self::id(root)
    }
    fn destroy_instance(&mut self, instance: Instance) {
        if let Some(id) = Self::inst(instance) {
            self.engine.destroy_instance(id);
        }
    }
    fn set_instance_transform(&mut self, instance: Instance, _: usize) -> Option<[f32; 12]> {
        let id = Self::inst(instance)?;
        Some(self.engine.instance_transform(Some(id)).to_words())
    }
    fn instance_parent(&mut self, instance: Instance) -> Instance {
        Self::id(self.engine.instance_parent(Self::inst(instance)))
    }
    fn instance_relative(&mut self, instance: Instance) -> [u32; 23] {
        self.engine
            .instance_relative(Self::inst(instance))
            .map_or([0; 23], |p| p.0)
    }
    fn instance_position(&mut self, instance: Instance) -> [f32; 3] {
        self.engine
            .instance_position(Self::inst(instance))
            .to_words()
    }
    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9] {
        self.engine
            .instance_orientation(Self::inst(instance))
            .to_words()
    }
    fn set_instance_position(&mut self, _: Instance, _: &[f32; 3]) {}
    fn set_instance_orientation(&mut self, _: Instance, _: &[f32; 9]) {}
    fn joint_params(&mut self, instance: Instance, index: u32) -> Vec<f32> {
        self.engine
            .joint_params(Self::inst(instance), index)
            .unwrap_or_default()
    }
    fn has_hardpoints(&mut self) -> bool {
        true
    }
    fn hardpoint_names(&mut self, archetype: EngineHandle) -> Vec<Vec<u8>> {
        let Some(arch) = self.arch(archetype) else {
            return Vec::new();
        };
        self.engine
            .component::<Hardpoints>()
            .map(|h| h.names(arch).map(<[u8]>::to_vec).collect())
            .unwrap_or_default()
    }
    fn connect_hardpoints(&mut self, a: Instance, b: Instance, name: &[u8]) -> u32 {
        let crc = neuron_core::hash::name_crc32(name);
        let r = connect_hardpoints(&mut self.engine, Self::inst(a), crc, Self::inst(b), crc);
        self.counts.connected += u64::from(r == 0);
        r
    }
    fn play_script(&mut self, _: u32, _: Instance, _: u32, _: u32) -> i32 {
        -1
    }
    fn destroy_script(&mut self, _: i32) {}
    fn filter_objects(&mut self, _: i32) -> u32 {
        0
    }
    fn root_eval(
        &mut self,
        _: i32,
        _: [f32; 3],
        _: u32,
        _: f32,
        _: f32,
        _: u32,
        _: u32,
    ) -> Option<(u8, [f32; 3])> {
        None
    }
    fn named_duration(&mut self, _: u32, _: u32) -> Option<f32> {
        None
    }
    fn eval_playback(
        &mut self,
        _: i32,
        _: u32,
        _: f32,
        _: f32,
        _: u32,
        _: u32,
        _: &[f32; 12],
    ) -> i32 {
        0
    }
    fn has_object(&mut self, _: i32, _: u32) -> u32 {
        0
    }
    fn first_instance_field(&mut self, _: i32) -> Option<u32> {
        None
    }
    fn duration_if_named(&mut self, _: i32) -> Option<f32> {
        None
    }
    fn touch_mapped(&mut self, _: i32) -> ([f32; 3], [f32; 3]) {
        ([0.0; 3], [0.0; 3])
    }
    fn set_overlay_pos(&mut self, _: i32, _: &[f32; 3]) {}
    fn set_overlay_extra(&mut self, _: i32, _: &[f32; 9]) {}
    fn add_overlay_pos(&mut self, _: i32, _: &[f32; 3]) {}
    fn find_object(&mut self, _: i32, _: u32, _: Instance) -> u32 {
        0
    }
    fn register_handle(&mut self, _: i32) {}
    fn has_handle(&mut self, _: i32) -> u8 {
        0
    }
    fn has_any_handle(&mut self, _: i32) {}
    fn create_typed_channel(&mut self, _: u32, _: Instance, _: Instance) -> u32 {
        0
    }
    fn play_channel(&mut self, _: u32, _: &[u32; 7]) {}
    fn destroy_channel(&mut self, _: u32) {}
    fn channel_in_mixer(&mut self, _: u32) -> u8 {
        0
    }
    fn sample_channel(&mut self, _: u32, _: f32) -> ChannelSample {
        ChannelSample {
            a: Vec::new(),
            b: Vec::new(),
            alpha: 0.0,
        }
    }
    fn render_instance(&mut self, _: Instance, _: u32, _: u32) -> i32 {
        3
    }
    fn pipeline_state(&mut self, _: u32) -> Option<i32> {
        Some(1)
    }
    fn set_modelview(&mut self, _: &[f32; 12]) {}
    fn set_world(&mut self, _: &[f32; 12]) {}
    fn set_world_n(&mut self, _: u32, _: &[f32; 12]) {}
    fn render_state(&mut self, _: u32) -> i32 {
        0
    }
    fn set_render_state(&mut self, _: u32, _: i32) {}
    fn is_vb_valid(&mut self, vb: u32) -> i32 {
        i32::from(self.live_vb.contains(&vb))
    }
    fn select_vb(&mut self, vb: u32) -> i32 {
        if self.live_vb.contains(&vb) { 0 } else { -1 }
    }
    fn create_vb(&mut self, _: u32, count: u32) -> Result<u32, i32> {
        self.next += 1;
        self.live_vb.insert(self.next);
        self.vb_sizes.insert(self.next, count as usize * 0x20);
        Ok(self.next)
    }
    fn lock_vb(&mut self, vb: u32, count: u32) -> Result<(u32, Vec<u8>), i32> {
        let size = self
            .vb_sizes
            .get(&vb)
            .copied()
            .unwrap_or(0)
            .max(count as usize * 0x20);
        Ok((0, vec![0; size]))
    }
    fn copy_vertices(
        &mut self,
        _: u32,
        _: u32,
        _: &VertexSource,
        _: u32,
        _: u32,
        _: &mut [u8],
    ) -> i32 {
        0
    }
    fn unlock_vb(&mut self, _: u32, locked: Vec<u8>) {
        for vertex in locked.as_chunks::<0x20>().0 {
            let words: Vec<f32> = vertex[..24]
                .as_chunks::<4>()
                .0
                .iter()
                .map(|w| f32::from_le_bytes(*w))
                .collect();
            if words.iter().any(|w| *w != 0.0) {
                self.counts.written_vertices += 1;
            }
            self.counts.nonfinite += words.iter().filter(|w| !w.is_finite()).count() as u64;
        }
    }
    fn destroy_vb(&mut self, vb: u32) {
        self.live_vb.remove(&vb);
    }
    fn is_ib_valid(&mut self, ib: u32) -> i32 {
        i32::from(self.live_ib.contains(&ib))
    }
    fn select_ib(&mut self, ib: u32, _: u32) -> i32 {
        if self.live_ib.contains(&ib) { 0 } else { -1 }
    }
    fn create_ib(&mut self, _: u32) -> Result<u32, i32> {
        self.next += 1;
        self.live_ib.insert(self.next);
        Ok(self.next)
    }
    fn lock_ib(&mut self, _: u32, _: u32) -> Result<u32, i32> {
        Ok(0)
    }
    fn unlock_ib(&mut self, _: u32, _: &[u16]) {}
    fn destroy_ib(&mut self, ib: u32) {
        self.live_ib.remove(&ib);
    }
    fn find_material(&mut self, name: &[u8], _: u32) -> (i32, Option<MaterialId>) {
        let eye = name.len() >= 3 && name[..3].eq_ignore_ascii_case(b"eye");
        (0, Some(if eye { self.eye } else { self.plain }))
    }
    fn release_material(&mut self, _: MaterialId) {}
    fn material_class_name(&mut self, material: MaterialId) -> Vec<u8> {
        if material == self.eye {
            b"EyeMaterial".to_vec()
        } else {
            b"Material".to_vec()
        }
    }
    fn bind(&mut self, _: MaterialId, _: &SubmitContext<'_>, binding: Option<&mut UvBinding>) {
        if binding.is_some() {
            self.counts.eye_binds += 1;
        }
    }
    fn submit(&mut self, _: MaterialId, _: &SubmitContext<'_>, _: &DrawArgs) {
        self.counts.submits += 1;
    }
    fn teardown(&mut self, _: MaterialId, _: &SubmitContext<'_>) {}
    fn rand(&mut self) -> i32 {
        0
    }
    fn dump(&mut self, _: i32, _: &str, _: u32, message: &str) {
        if message.starts_with("Deformable: Could not load mesh") {
            self.counts.meshes_missing += 1;
        }
    }
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
        [0, 0, 640, 480]
    }
    fn x_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn z_near(&mut self) -> f32 {
        1.0
    }
    fn position(&mut self) -> [f32; 3] {
        [0.0; 3]
    }
}

#[test]
fn every_shipped_deformable_model_poses() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut files = Vec::new();
    dfm_files(&root, &mut files);
    if files.is_empty() {
        println!("{} has no .dfm files: nothing to load", root.display());
        return;
    }
    files.sort();
    let host = Host::new(&root);
    let mut arena: Materials<()> = Materials::new();
    let made = Rc::new(RefCell::new(Vec::new()));
    let mut engine = Engine::new();
    engine.add_component(Box::new(Hardpoints::new()));
    engine.add_component(Box::new(Made(Rc::clone(&made))));
    let mut services = Services {
        engine,
        made,
        archetypes: Vec::new(),
        plain: arena.insert(Box::new(Dummy), 1),
        eye: arena.insert(Box::new(Dummy), 1),
        live_vb: BTreeSet::new(),
        live_ib: BTreeSet::new(),
        next: 0,
        vb_sizes: HashMap::new(),
        counts: Counts::default(),
    };
    let mut module = Deformable::new(None);
    let (mut created, mut posed_bones, mut bound_bones, mut levels, mut draws) =
        (0u64, 0u64, 0u64, 0u64, 0u64);
    let mut failed_draws = 0u64;
    for path in &files {
        let relative = path.strip_prefix(&root).unwrap_or(path);
        let text = relative.to_string_lossy().replace('/', "\\");
        let mut desc = [PartDesc {
            lod_count: 8,
            lods: vec![LodDesc::default(); 8],
            path: Some(text.clone().into_bytes()),
            flags: 0,
            lod_limit: 8,
        }];
        let host_ref = &host;
        let mut open = |d: &PartDesc| -> Option<Box<dyn FileSystem>> {
            let path = d.path.clone()?;
            let fs_desc = FsDesc::read(&path).with_implementation(b"UTF");
            UtfFileSystem::open(host_ref, &fs_desc)
                .ok()
                .map(|fs| Box::new(fs) as Box<dyn FileSystem>)
        };
        let Ok(mut instance) = module.create(&mut desc, 0, 0, &mut open, &mut services) else {
            continue;
        };
        created += 1;
        assert!(desc[0].lod_count > 0, "{text}: no levels");
        // Pose: every bound bone's joint turned a little, the tree updated
        // from the root, the engine's transforms handed to the bone
        // runtimes as its IInstanceTransform calls would.
        let root_instance = Services::inst(instance.first_bone());
        for part in &mut instance.parts {
            let Some(holder) = part.lod.as_mut() else {
                continue;
            };
            let bound = holder.bound();
            for bone in &holder.bones[..bound] {
                let id = Services::inst(bone.instance);
                // An angle 0.1 further, or a rotation of 0.2 about y.
                let current = services.engine.joint_params(id, 0).unwrap_or_default();
                let posed = match current.len() {
                    1 => vec![current[0] + 0.1],
                    4 => vec![0.995_004_2, 0.0, 0.099_833_42, 0.0],
                    _ => continue,
                };
                if services.engine.set_joint_params(id, 0, &posed) {
                    posed_bones += 1;
                }
            }
        }
        services.engine.update_instance(root_instance, 0);
        for part in &mut instance.parts {
            let Some(holder) = part.lod.as_mut() else {
                continue;
            };
            bound_bones += holder.bound() as u64;
            for bone in 0..holder.bound() {
                let id = Services::inst(holder.bones[bone].instance);
                if id.is_some() {
                    let m = services.engine.instance_transform(id).to_words();
                    holder.bones[bone].set_matrix(&m);
                    holder.bones[bone].flags |= 2;
                }
            }
        }
        for part in &mut instance.parts {
            for lod in 0..i32::try_from(part.meshes.len()).unwrap_or(0) {
                part.select_lod(lod);
                levels += 1;
                let r = part.draw_live(&mut Camera, &mut module.meshes, &mut services);
                draws += 1;
                failed_draws += u64::from(r == 0);
            }
        }
        module.destroy(instance, &mut services);
    }
    println!(
        "{} .dfm files, {created} instances, {bound_bones} bound bones ({posed_bones} posed through \
         their joints), {levels} levels drawn ({failed_draws} failed), {} meshes not loaded, {} \
         vertices written, {} submits, {} eye binds, {} hardpoint connections",
        files.len(),
        services.counts.meshes_missing,
        services.counts.written_vertices,
        services.counts.submits,
        services.counts.eye_binds,
        services.counts.connected
    );
    assert_eq!(created, files.len() as u64, "every file makes an instance");
    assert_eq!(
        services.counts.nonfinite, 0,
        "skinned positions and normals are finite"
    );
    assert_eq!(failed_draws, 0, "every level skins and draws");
    assert!(draws > 0 && services.counts.written_vertices > 0);
    assert!(module.meshes.is_empty(), "every mesh released");
}
