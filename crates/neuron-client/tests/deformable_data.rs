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
    BoneLink, ChannelSample, DeformHost, DeformInstance, DeformRender, Deformable, EngineHandle,
    Instance, LodDesc, PartDesc, UvBinding,
};
use neuron_client::render::deformable::{DeformMesh, EyeFactory, bind_eye_materials};
use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::ff::device::{Device, RecordingBackend};
use neuron_client::render::material::{
    DrawArgs, Material, MaterialBatcher, MaterialId, Materials, Shading, ShadingFactory,
    SubmitContext, SurfaceFactory, VertexSource,
};
use neuron_client::render::mesh::{MeshCamera, RenderManager, VMeshRegistry};
use neuron_client::render::stack::{RenderStack, StackDeform, StackEngine, draw_deformable};
use neuron_client::render::texture::{FormatClasses, LibraryConfig, TextureLibrary};
use neuron_core::ini::{EngineIni, NoIncludes};
use neuron_core::scene::{
    ArchetypeId, Engine, EngineComponent, Hardpoints, InstanceId, SceneError, connect_hardpoints,
};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

type Stack = RenderStack<RecordingBackend>;

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
    engine: Rc<RefCell<Engine>>,
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

impl DeformHost for Services {
    fn create_archetype(&mut self, name: &[u8], fs: &mut dyn FileSystem) -> EngineHandle {
        let id = self
            .engine
            .borrow_mut()
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
            self.engine.borrow_mut().release_handle(id);
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
        let root = self.engine.borrow_mut().create_instance(arch);
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
            self.engine.borrow_mut().destroy_instance(id);
        }
    }
    fn set_instance_transform(&mut self, instance: Instance, _: usize) -> Option<[f32; 12]> {
        let id = Self::inst(instance)?;
        Some(
            self.engine
                .borrow_mut()
                .instance_transform(Some(id))
                .to_words(),
        )
    }
    fn instance_parent(&mut self, instance: Instance) -> Instance {
        Self::id(
            self.engine
                .borrow_mut()
                .instance_parent(Self::inst(instance)),
        )
    }
    fn instance_relative(&mut self, instance: Instance) -> [u32; 23] {
        self.engine
            .borrow_mut()
            .instance_relative(Self::inst(instance))
            .map_or([0; 23], |p| p.0)
    }
    fn instance_position(&mut self, instance: Instance) -> [f32; 3] {
        self.engine
            .borrow_mut()
            .instance_position(Self::inst(instance))
            .to_words()
    }
    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9] {
        self.engine
            .borrow_mut()
            .instance_orientation(Self::inst(instance))
            .to_words()
    }
    fn set_instance_position(&mut self, _: Instance, _: &[f32; 3]) {}
    fn set_instance_orientation(&mut self, _: Instance, _: &[f32; 9]) {}
    fn joint_params(&mut self, instance: Instance, index: u32) -> Vec<f32> {
        self.engine
            .borrow_mut()
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
            .borrow()
            .component::<Hardpoints>()
            .map(|h| h.names(arch).map(<[u8]>::to_vec).collect())
            .unwrap_or_default()
    }
    fn connect_hardpoints(&mut self, a: Instance, b: Instance, name: &[u8]) -> u32 {
        let crc = neuron_core::hash::name_crc32(name);
        let r = connect_hardpoints(
            &mut self.engine.borrow_mut(),
            Self::inst(a),
            crc,
            Self::inst(b),
            crc,
        );
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
    fn rand(&mut self) -> i32 {
        0
    }
}

impl DeformRender for Services {
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

/// Poses an instance: every bound bone's joint turned a little (an angle
/// 0.1 further, or a rotation of 0.2 about y), the tree updated from the
/// root, and the engine's world transforms forwarded to the bone runtimes
/// as its `IInstanceTransform` calls would (`SetMatrix`, `MarkWorldDirty`).
/// The bones posed through their joints and the bound bones.
fn pose(instance: &mut DeformInstance, engine: &RefCell<Engine>) -> (u64, u64) {
    let mut engine = engine.borrow_mut();
    let (mut posed, mut bound) = (0u64, 0u64);
    let root = Services::inst(instance.first_bone());
    for part in &instance.parts {
        let Some(holder) = part.lod.as_ref() else {
            continue;
        };
        bound += holder.bound() as u64;
        for bone in &holder.bones[..holder.bound()] {
            let id = Services::inst(bone.instance);
            let current = engine.joint_params(id, 0).unwrap_or_default();
            let angles = match current.len() {
                1 => vec![current[0] + 0.1],
                4 => vec![0.995_004_2, 0.0, 0.099_833_42, 0.0],
                _ => continue,
            };
            if engine.set_joint_params(id, 0, &angles) {
                posed += 1;
            }
        }
    }
    engine.update_instance(root, 0);
    instance.pull_bone_transforms(|bone| {
        Services::inst(bone).map(|id| engine.instance_transform(Some(id)).to_words())
    });
    (posed, bound)
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
        engine: Rc::new(RefCell::new(engine)),
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
        let (posed, bound) = pose(&mut instance, &services.engine);
        posed_bones += posed;
        bound_bones += bound;
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

/// The scene engine as the stack asks it (the eye's parent and
/// orientation; the bones have no render archetype).
struct SceneStackEngine(Rc<RefCell<Engine>>);

impl SceneStackEngine {
    fn id(instance: usize) -> Option<InstanceId> {
        u32::try_from(instance).ok().map(InstanceId::from_index)
    }
}

impl StackEngine for SceneStackEngine {
    fn instance_transform(&mut self, instance: usize) -> [f32; 12] {
        self.0
            .borrow()
            .instance_transform(Self::id(instance))
            .to_words()
    }
    fn instance_archetype(&mut self, _: usize) -> i32 {
        -1
    }
    fn instance_extent(&mut self, _: usize) -> Option<(f32, [f32; 3])> {
        None
    }
    fn set_instance_extent(&mut self, _: usize, _: f32, _: [f32; 3]) {}
    fn instance_parent(&mut self, instance: usize) -> i32 {
        Services::id(self.0.borrow().instance_parent(Self::id(instance)))
    }
    fn instance_orientation(&mut self, instance: usize) -> [f32; 9] {
        self.0
            .borrow()
            .instance_orientation(Self::id(instance))
            .to_words()
    }
}

/// The UTF archives under `dir`, relative to `root`.
fn utf_files(root: &Path, dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            utf_files(root, &path, out);
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

fn open_utf(host: &Host, path: &Path) -> Option<UtfFileSystem> {
    let text = path.to_string_lossy().replace('/', "\\");
    UtfFileSystem::open(
        host,
        &FsDesc::read(text.as_bytes()).with_implementation(b"UTF"),
    )
    .ok()
}

/// The words of a skinned buffer's positions and normals that are not
/// finite, and the vertices written (any word not zero).
fn skinned_vertices(bytes: &[u8]) -> (u64, u64) {
    let (mut nonfinite, mut written) = (0, 0);
    for vertex in bytes.as_chunks::<0x20>().0 {
        let words: Vec<f32> = vertex[..24]
            .as_chunks::<4>()
            .0
            .iter()
            .map(|w| f32::from_le_bytes(*w))
            .collect();
        written += u64::from(words.iter().any(|w| *w != 0.0));
        nonfinite += words.iter().filter(|w| !w.is_finite()).count() as u64;
    }
    (nonfinite, written)
}

/// Every shipped `.dfm` drawn through the render stack (work package P4.7,
/// second increment): all shipped material and texture libraries loaded
/// into one [`RenderStack`] over the recording backend with the
/// `EyeMaterial`'s factory in the chain and its `^eye*` rule added first;
/// each model made with the stack answering the render half (its face
/// groups find their materials in the library by name CRC), posed through
/// the scene engine with the transforms forwarded, and every level of
/// every part skinned into a device vertex buffer and drawn through its
/// materials into the device's draw packets, the eye groups binding their
/// `EyeMaterial` with the UV-deform row; then the whole instance drawn
/// once through `IDeformable::Draw` with the render manager. Nothing is
/// written or committed.
#[test]
#[allow(clippy::too_many_lines)]
fn every_shipped_deformable_model_draws_through_the_stack() {
    let base = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let root = base.join("data");
    let mut models = Vec::new();
    dfm_files(&root, &mut models);
    let Ok(ini_bytes) = fs::read(base.join("NeuronComponents.ini")) else {
        println!("outdir/NeuronComponents.ini is missing: nothing to draw");
        return;
    };
    if models.is_empty() {
        println!("{} has no .dfm files: nothing to draw", root.display());
        return;
    }
    models.sort();
    let mut archives = Vec::new();
    utf_files(&root, &root, &mut archives);
    archives.sort();
    let mut ini = EngineIni::new();
    ini.load_buffer(&ini_bytes, &mut NoIncludes)
        .expect("the engine INI parses");

    let made = Rc::new(RefCell::new(Vec::new()));
    let mut engine = Engine::new();
    engine.add_component(Box::new(Hardpoints::new()));
    engine.add_component(Box::new(Made(Rc::clone(&made))));
    let engine = Rc::new(RefCell::new(engine));
    let mut arena: Materials<()> = Materials::new();
    let mut host = Services {
        engine: Rc::clone(&engine),
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

    let device = Device::new(Defaults::builtin(), 640, 480, RecordingBackend::default());
    let shading = Shading::new(
        device,
        TextureLibrary::new(LibraryConfig::default()),
        FormatClasses::builtin(),
    );
    let mut batcher: MaterialBatcher<Stack> = MaterialBatcher::new(
        Box::new(ShadingFactory::with_fallback(Box::new(
            EyeFactory::with_fallback(Box::new(SurfaceFactory::new())),
        ))),
        Some(&ini),
    );
    bind_eye_materials(batcher.library_mut());
    let mut stack = RenderStack::new(
        shading,
        batcher,
        VMeshRegistry::shared(),
        Box::new(SceneStackEngine(Rc::clone(&engine))),
    );
    let files = Host::new(&root);
    let (mut material_groups, mut texture_groups) = (Vec::new(), Vec::new());
    for path in &archives {
        let Some(mut fs) = open_utf(&files, path) else {
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
    let library = stack.batcher().library();
    let materials = library.materials().live().count();
    let eye_materials = library
        .materials()
        .live()
        .filter(|&id| {
            library
                .materials()
                .get(id)
                .is_some_and(|m| m.class_name() == "EyeMaterial")
        })
        .count();

    let mut manager: RenderManager<Stack> = RenderManager::new(Vec::new());
    let mut module = Deformable::new(None);
    let (mut created, mut levels, mut failed, mut level_packets) = (0u64, 0u64, 0u64, 0usize);
    let (mut nonfinite, mut written, mut eye_transforms) = (0u64, 0u64, 0u64);
    let mut instance_packets = 0usize;
    for path in &models {
        let relative = path.strip_prefix(&root).unwrap_or(path);
        let text = relative.to_string_lossy().replace('/', "\\");
        let mut desc = [PartDesc {
            lod_count: 8,
            lods: vec![LodDesc::default(); 8],
            path: Some(text.clone().into_bytes()),
            flags: 0,
            lod_limit: 8,
        }];
        let files_ref = &files;
        let mut open = |d: &PartDesc| -> Option<Box<dyn FileSystem>> {
            let path = d.path.clone()?;
            let fs_desc = FsDesc::read(&path).with_implementation(b"UTF");
            UtfFileSystem::open(files_ref, &fs_desc)
                .ok()
                .map(|fs| Box::new(fs) as Box<dyn FileSystem>)
        };
        let made = {
            let mut services = StackDeform::new(&mut stack, &mut host);
            module.create(&mut desc, 0, 0, &mut open, &mut services)
        };
        let Ok(mut instance) = made else {
            continue;
        };
        created += 1;
        pose(&mut instance, &engine);
        for index in 0..instance.parts.len() {
            for lod in 0..i32::try_from(instance.parts[index].meshes.len()).unwrap_or(0) {
                let part = &mut instance.parts[index];
                part.select_lod(lod);
                part.mark_skin_dirty();
                let before = stack.shading.device.backend.draws.len();
                let r = {
                    let mut services = StackDeform::new(&mut stack, &mut host);
                    part.draw_live(&mut Camera, &mut module.meshes, &mut services)
                };
                levels += 1;
                failed += u64::from(r == 0);
                level_packets += stack.shading.device.backend.draws.len() - before;
                if let Some(h) = stack.shading.vb(part.vb.vertex_buffer)
                    && let Ok(vb) = stack.shading.device.buffers.vb(h)
                {
                    let (n, w) = skinned_vertices(vb.bytes());
                    nonfinite += n;
                    written += w;
                }
                if let Some(id) = part.current
                    && let Some(DeformMesh::Dfm(mesh)) = module.meshes.get_mut(id)
                {
                    eye_transforms += u64::from(mesh.eye_uv_transform_count);
                }
            }
            instance.parts[index].select_lod(0);
        }
        let before = stack.shading.device.backend.draws.len();
        draw_deformable(
            &mut instance,
            &mut module,
            &mut Camera,
            0,
            0,
            &mut stack,
            Some(&mut manager),
            None,
            &mut host,
        );
        instance_packets += stack.shading.device.backend.draws.len() - before;
        {
            let mut services = StackDeform::new(&mut stack, &mut host);
            module.destroy(instance, &mut services);
        }
        stack.shading.device.backend.draws.clear();
    }
    let missing = stack
        .dumps
        .iter()
        .filter(|d| d.text.contains("couldnt find material"))
        .count();
    let unloaded = stack
        .dumps
        .iter()
        .filter(|d| d.text.contains("Could not load mesh"))
        .count();
    manager.destroy(&mut stack);
    for group in material_groups {
        stack.with_batcher(|b, s| b.library_mut().delete_group(s, group.cast_signed()));
    }
    let live = stack.batcher().library().materials().live().count();
    let (textures, mut pipeline) = stack.shading.texture_parts();
    for group in texture_groups {
        textures.garbage_collect(group, &mut pipeline);
    }
    println!(
        "{materials} materials loaded ({eye_materials} EyeMaterials); {} .dfm files, {created} \
         instances, {levels} levels skinned and drawn ({failed} failed, {level_packets} draw \
         packets), {instance_packets} draw packets through IDeformable::Draw, {written} skinned \
         vertices written, {eye_transforms} eye texture transforms, {missing} face groups \
         without their material, {unloaded} meshes not loaded, {live} materials left",
        models.len()
    );
    assert_eq!(created, models.len() as u64, "every file makes an instance");
    assert_eq!(failed, 0, "every level skins and draws");
    assert_eq!(nonfinite, 0, "skinned positions and normals are finite");
    assert!(level_packets > 0, "no deformable draw reached the device");
    assert!(written > 0);
    assert!(module.meshes.is_empty(), "every mesh released");
    assert_eq!(live, 0, "materials left after the libraries were deleted");
}
