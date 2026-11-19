//! The engine (work package P4.16, decisions T9 and T10): one [`Engine`]
//! value owns the client's subsystems in place of the reference's
//! component manager, system container and `NeuronRuntime` globals
//! (`CppSource/neuroncomponents`, `CppSource/system`,
//! `CppSource/NeuronRuntime`), and of the engine half of the client's
//! `InitializeEngine` and `ClientAtExitTeardown`.
//!
//! [`Engine::new`] builds the subsystems in the reference's order
//! ([`order::CREATION_ORDER`]) from the engine INI (`NeuronComponents.ini`)
//! and `OutpostFrontier.ini`, over a render backend the caller makes: the
//! Direct3D 12 backend on Windows ([`windows`]), the recording backend
//! anywhere. Dropping the engine tears them down in the reference's order
//! ([`order::TEARDOWN_ORDER`]). Each step is written to the
//! [`Lifecycle`] log, which outlives the engine.
//!
//! The `[Libraries]`, `[System]` and `[Engine]` lists lose their function
//! under static composition (decision T22): the order is compiled in. The
//! engine still reads them and keeps a notice when they differ from it.
//! Per-subsystem sections (`[RenderPipeline]`, `[TextureLibrary]`,
//! `[MaterialMap]`, `[BatchedMaterials]`, `[MaterialBatcher]`,
//! `[RenderManager]`, `[TriMesh]`, `[Deformable]`, `[ParticleFx]`,
//! `[SoundManager]`, `[AudioProperties]`) are read by their subsystems.
//!
//! The engine is complete when [`Engine::new`] returns, before the caller
//! plays the startup movies; the reference loads the fonts and starts the
//! UI manager after them (divergence D450).
//!
//! [`Engine::frame`] runs one frame in the reference's order (see
//! [`frame`]). The engine also loads what the frame draws: the libraries
//! of an archive ([`Engine::load_archive`]), models through the scene
//! engine ([`Engine::create_model`]), deformable models
//! ([`Engine::create_deformable`]), particle effects
//! ([`Engine::load_effects`], [`Engine::create_effect`]) and scene scripts
//! ([`Engine::load_scene_script`]), animations and aim events on
//! deformable models through `IDeformable` ([`Engine::load_animations`],
//! [`Engine::play_deformable_animation`], [`Engine::play_deformable`],
//! [`Engine::start_deformable_aim`]; [`deform_anim`]) and the client's
//! sounds: the sound tables and voice profiles ([`Engine::load_sound_tables`]),
//! played through the client's playing list and the frame's sound tick
//! ([`Engine::play_sound`]; [`sound`]). The frame's
//! camera is any [`Camera`](crate::render::appearance::Camera);
//! [`camera::ViewCamera`] is a perspective one.

pub mod camera;
pub mod deform_anim;
mod effects;
mod frame;
pub mod lights;
pub mod options;
pub mod order;
mod scene_host;
mod shapes;
pub mod sound;
pub mod ui_host;
pub mod ui_meshes;
#[cfg(windows)]
pub mod windows;

use std::cell::{RefCell, RefMut};
use std::fmt;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use neuron_core::fx_data::{self as fx, AleFile, NodeClass};
use neuron_core::ini::{EngineIni, EngineIniError, IniReader, NoIncludes};
use neuron_core::math::Transform;
use neuron_core::scene::{Animation, ArchetypeId, Engine as SceneEngine, Hardpoints, InstanceId};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

use crate::audio::config::{AudioConfig, CLIENT_STARTUP_FLAGS, stream_buffer_seconds};
use crate::audio::mixer::Mixer;
use crate::input::mouse::Mouse;
use crate::render::appearance::Appearance;
use crate::render::deformable::{
    DeformInstance, Deformable, EyeFactory, LodDesc, PartDesc, bind_eye_materials,
};
use crate::render::ff::defaults::Defaults;
use crate::render::ff::device::{Backend, Device};
use crate::render::material::{
    GroupId, MaterialAnimations, MaterialBatcher, Shading, ShadingFactory, SurfaceFactory,
};
use crate::render::mesh::manager::plugin_names;
use crate::render::mesh::{
    RenderComponent, RenderManager, SpherePlugin, TriMeshPlugin, VMeshLibraryLoad, VMeshPlugin,
    VMeshRegistry, VMeshWire,
};
use crate::render::particles::{Effect, NodeId, ParticleFx};
use crate::render::stack::{RenderStack, StackAppearance, StackDeform};
use crate::render::texture::{FormatClasses, LibraryConfig, TextureLibrary};
use crate::scene_script::playback::Player;
use crate::scene_script::{BuildState, Vm, build, load};
use crate::text::font::{FontBackend, FontEnv, FontSystem, config::read_fonts_ini};
use crate::text_input::ime::ImeContext;
use crate::text_input::ime_font::SharedFontBackend;
use crate::ui::controls::scroll::ScrollShapes;
use crate::ui::cursor::Cursors;
use crate::ui::ime::{ImeBoxes, ImeServices};
use crate::ui::shape::ShapeDb;
use crate::ui::tree::Ui;
use crate::ui::widescreen::Surface;
use crate::window::DisplayMode;

pub use camera::ViewCamera;
pub use deform_anim::DeformablePlay;
pub use effects::{EffectServices, FxMeshes, mesh_path};
pub use frame::{FrameInput, FrameMouse, FrameReport};
pub use order::{
    CREATION_ORDER, LIBRARIES, LifeEvent, Lifecycle, Phase, Reference, Stage, TEARDOWN_ORDER,
};
pub use scene_host::{
    DeformSink, DeformableEvent, Links, Names, RenderLink, RenderSide, SceneDeformHost, SceneHost,
    SharedAnimation, SharedDeformables, SharedLinks, SharedScene, channel_key, pull_bones,
    sink_token, typed_id, walk_deformable,
};
pub use shapes::StackShapes;
pub use sound::{SoundId, SoundInfo, SoundTables};

/// The interface's shared mesh and material file (`Win3DStartupManager`).
const INTERFACE_GENERIC: &[u8] = b"interface\\interface.generic.vms";

/// The engine's stack type over a backend.
pub type Stack<B> = RenderStack<B>;

/// What the engine is built from.
pub struct EngineConfig {
    /// The engine INI's contents (`NeuronComponents.ini`); `None` builds
    /// with the subsystems' built-in defaults (divergence D454).
    pub engine_ini: Option<Vec<u8>>,
    /// `OutpostFrontier.ini`'s contents, for its `[Data]` font keys.
    pub client_ini: Option<Vec<u8>>,
    /// The data directory (`OpenData`'s path, resolved by the caller).
    pub data_root: PathBuf,
    /// The display mode: the back buffer is its size
    /// ([`DisplayMode::back_buffer`]).
    pub mode: DisplayMode,
    /// The font backend; `None` takes DirectWrite on Windows (with the
    /// font files under the data's `fonts_dir`, decision O29) and the
    /// deterministic metrics backend elsewhere.
    pub font_backend: Option<Box<dyn FontBackend>>,
    /// The system double-click time the mouse model latches, in
    /// milliseconds (`GetDoubleClickTime`).
    pub double_click_ms: u32,
    /// Open the audio device (XAudio2; Windows only).
    pub audio_device: bool,
}

impl EngineConfig {
    /// A configuration with no INI files, no audio device and the default
    /// double-click time.
    #[must_use]
    pub fn new(data_root: impl Into<PathBuf>, mode: DisplayMode) -> EngineConfig {
        EngineConfig {
            engine_ini: None,
            client_ini: None,
            data_root: data_root.into(),
            mode,
            font_backend: None,
            double_click_ms: 500,
            audio_device: false,
        }
    }
}

/// Why the engine could not be built or could not load something.
#[derive(Debug)]
pub enum EngineError {
    /// The data directory does not exist (`could not find DATA directory`).
    NoData(PathBuf),
    /// The engine INI does not parse.
    Ini(EngineIniError),
    /// A file of the data could not be opened or read.
    Open(String),
    /// The scene engine refused an archive.
    Scene(String),
    /// A particle effect could not be made.
    Effect(String),
    /// A scene script could not be loaded.
    Script(String),
    /// A deformable model could not be made.
    Deformable(String),
    /// An animation could not be loaded or played.
    Animation(String),
}

impl fmt::Display for EngineError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NoData(p) => write!(f, "could not find the data directory ({})", p.display()),
            Self::Ini(e) => write!(f, "the engine INI: {e}"),
            Self::Open(p) => write!(f, "could not open {p}"),
            Self::Scene(p) => write!(f, "the scene engine refused {p}"),
            Self::Effect(e) => write!(f, "particle effect: {e}"),
            Self::Script(e) => write!(f, "scene script: {e}"),
            Self::Deformable(p) => write!(f, "deformable model {p} could not be made"),
            Self::Animation(e) => write!(f, "animation: {e}"),
        }
    }
}

impl std::error::Error for EngineError {}

/// A model made through the scene engine.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ModelId(usize);

/// A live particle effect.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct EffectId(usize);

/// A deformable model.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct DeformableId(usize);

/// A loaded scene script.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SceneScriptId(usize);

/// A loaded animation script (an `.anm` file's animations).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct AnimationsId(usize);

/// What [`Engine::load_archive`] loaded from one archive.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ArchiveLoad {
    /// The texture group of its `Texture library`.
    pub textures: Option<i32>,
    /// The material group of its `Material library`.
    pub materials: Option<u32>,
    /// The material animation group of its `MaterialAnim`.
    pub animations: bool,
    /// The meshes of its `VMeshLibrary`.
    pub meshes: usize,
}

#[derive(Clone, Copy, Debug)]
struct Model {
    root: InstanceId,
    archetype: ArchetypeId,
}

struct LiveEffect {
    parent: NodeId,
    effect: Effect,
}

/// `ParticleEffectCleanupParticleFx`: the effect stopped (`Slot13(7)`) and
/// released, its folder detached and released.
fn cleanup_effect(
    particles: &mut ParticleFx,
    mut live: LiveEffect,
    svc: &mut dyn crate::render::particles::ParticleServices,
) {
    live.effect.dispatch(particles, 7, svc);
    live.effect.release(particles, svc);
    particles.detach(live.parent, svc);
    particles.release(live.parent, svc);
}

/// The groups the engine loaded, freed at the teardown.
#[derive(Default)]
struct Loaded {
    textures: Vec<i32>,
    materials: Vec<u32>,
    animations: Vec<GroupId>,
    meshes: Vec<VMeshLibraryLoad>,
}

/// The `[Data]` keys of `OutpostFrontier.ini` the engine reads
/// (`LoadClientIniData`'s `fonts`, `rich_fonts`, `sounds` and `voices`, and
/// `fonts_dir`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ClientData {
    /// Each key in file order: `true` for `rich_fonts`.
    pub fonts: Vec<(bool, Vec<u8>)>,
    /// `fonts_dir`: the font files' directory under the data.
    pub fonts_dir: Option<Vec<u8>>,
    /// `sounds`: the sound tables, in file order.
    pub sounds: Vec<Vec<u8>>,
    /// `voices`: the voice profiles, in file order.
    pub voices: Vec<Vec<u8>>,
}

impl ClientData {
    /// The keys of `OutpostFrontier.ini`'s `[Data]` sections.
    #[must_use]
    pub fn read(contents: Option<&[u8]>) -> ClientData {
        let mut out = ClientData::default();
        let Some(contents) = contents else {
            return out;
        };
        let mut reader = IniReader::new();
        if !reader.open(b"OutpostFrontier.ini", contents.to_vec()) {
            return out;
        }
        while reader.read_header() {
            if !reader.is_header(b"Data") {
                continue;
            }
            while reader.read_value() {
                if reader.is_value(b"fonts") {
                    out.fonts.push((false, reader.value_string()));
                } else if reader.is_value(b"rich_fonts") {
                    out.fonts.push((true, reader.value_string()));
                } else if reader.is_value(b"fonts_dir") {
                    out.fonts_dir = Some(reader.value_string());
                } else if reader.is_value(b"sounds") {
                    out.sounds.push(reader.value_string());
                } else if reader.is_value(b"voices") {
                    out.voices.push(reader.value_string());
                }
            }
        }
        out
    }
}

/// The client engine. `B` is the render backend.
pub struct Engine<B: Backend + 'static> {
    lifecycle: Lifecycle,
    ini: EngineIni,
    /// What construction and loading reported (a missing INI, lists that
    /// differ from the compiled order, files that did not load).
    pub notices: Vec<String>,
    registry: crate::render::mesh::vmesh::SharedRegistry,
    render: Rc<RefCell<RenderSide<B>>>,
    /// The sound manager's mixer; its play keys are the client's sources
    /// ([`Engine::client_sound`]).
    pub mixer: Mixer<u32>,
    /// The sound manager's INI settings.
    pub audio: AudioConfig,
    /// The streamer's buffer length in seconds.
    pub stream_seconds: f32,
    #[cfg(windows)]
    audio_device: Option<crate::platform::audio::AudioDevice>,
    /// The particle system.
    pub particles: ParticleFx,
    /// The game option `BATCH_PARTICLES` (0 by default), which the frame's
    /// effect draw hands to `SetUseMaterialBatcher`.
    pub particle_batcher: bool,
    /// The client's mesh factory, bound to the particle system
    /// (`WarmUpEffectShapes`).
    fx_meshes: FxMeshes,
    scene: SharedScene,
    links: SharedLinks,
    /// The animation system.
    animation: SharedAnimation,
    deform: Deformable,
    deform_host: SceneDeformHost,
    /// What the deformable models' caller sinks received, oldest first.
    deformable_events: Vec<DeformableEvent>,
    appearance: Option<Appearance>,
    effects: Vec<Option<LiveEffect>>,
    scripts: Vec<Option<Player>>,
    /// The client's sounds.
    sound: sound::EngineSound,
    data: Option<Host>,
    mode: DisplayMode,
    fonts: Option<FontSystem>,
    mouse: Option<Mouse>,
    ui: Option<Ui>,
    cursors: Cursors,
    ui_shape_db: ShapeDb,
    ime: Option<ImeBoxes>,
    /// The game options the draw reads, at the reference's defaults until
    /// the game sets them (P5).
    pub options: options::GameOptions,
    /// The engine lights' registered list.
    pub lights: lights::EngineLights,
    models: Vec<Option<Model>>,
    deformables: SharedDeformables,
    backdrop: Option<ModelId>,
    starsphere: Option<ModelId>,
    loaded: Rc<RefCell<Loaded>>,
    win3d: crate::ui::win3d_immediate::Win3dLayer,
    batchers: crate::ui::win3d_renderables::LineBatchers,
    quads: crate::ui::quad_batch::QuadBatcher,
    /// The client clock of the frames run, for the cursor's animation.
    elapsed: f64,
    ui_meshes: Rc<std::cell::Cell<usize>>,
    frames: u64,
    torn_down: bool,
}

/// The render manager's plugins.
type Plugins<B> = Vec<Box<dyn RenderComponent<RenderStack<B>>>>;

/// The `[RenderManager]` plugins in the section's order.
fn render_plugins<B: Backend + 'static>(
    ini: &EngineIni,
    registry: &crate::render::mesh::vmesh::SharedRegistry,
    notices: &mut Vec<String>,
) -> (Plugins<B>, Vec<FaceSlot>) {
    let lines: Vec<Vec<u8>> = ini
        .section(b"RenderManager")
        .map(|s| (0..s.entries.len()).filter_map(|i| s.line(i)).collect())
        .unwrap_or_else(|| {
            vec![
                b"VMesh".to_vec(),
                b"TriMesh".to_vec(),
                b"SphereMesh".to_vec(),
            ]
        });
    let refs: Vec<&[u8]> = lines.iter().map(Vec::as_slice).collect();
    let tristrips = ini
        .section(b"TriMesh")
        .and_then(|s| s.value(b"tristrips"))
        .is_none_or(|v| crate::render::ff::defaults::profile_number(v) != 0);
    let mut plugins: Plugins<B> = Vec::new();
    let mut faces = Vec::new();
    for name in plugin_names(&refs) {
        let eq = |n: &[u8]| neuron_core::hash::eq_ignore_case(&name, n);
        if eq(b"VMesh") {
            let plugin = VMeshPlugin::new(Rc::clone(registry));
            faces.push(FaceSlot::VMesh(plugins.len(), plugin.faces()));
            plugins.push(Box::new(plugin));
        } else if eq(b"TriMesh") {
            let plugin = TriMeshPlugin::new(tristrips);
            faces.push(FaceSlot::TriMesh(plugins.len(), plugin.faces()));
            plugins.push(Box::new(plugin));
        } else if eq(b"SphereMesh") {
            plugins.push(Box::new(SpherePlugin::new()));
        } else if !name.is_empty() {
            notices.push(format!(
                "[RenderManager] {}: no such plugin",
                String::from_utf8_lossy(&name)
            ));
        }
    }
    (plugins, faces)
}

/// A plugin whose faces the render manager hands out.
enum FaceSlot {
    VMesh(usize, crate::render::mesh::VMeshFaces),
    TriMesh(usize, crate::render::mesh::TriMeshFaces),
}

/// The notices for `[System]` and `[Engine]` lists that differ from the
/// compiled order.
fn check_lists(ini: &EngineIni, notices: &mut Vec<String>) {
    for (section, expect) in [
        (&b"System"[..], &CREATION_ORDER[1..9]),
        (&b"Engine"[..], &CREATION_ORDER[11..17]),
    ] {
        let names = order::component_lines(ini.section(section));
        match order::stages_of(&names) {
            Ok(stages) if stages == expect => {}
            Ok(stages) => notices.push(format!(
                "[{}] lists {stages:?}; the engine builds {expect:?} (decision T22)",
                String::from_utf8_lossy(section)
            )),
            Err(name) => notices.push(format!(
                "[{}] names {}, which the engine does not have (decision T22)",
                String::from_utf8_lossy(section),
                String::from_utf8_lossy(&name)
            )),
        }
    }
}

/// The font backend when the caller gives none.
fn default_font_backend(data_root: &Path, fonts_dir: Option<&[u8]>) -> Box<dyn FontBackend> {
    #[cfg(windows)]
    {
        let dir = fonts_dir.map_or_else(
            || data_root.join("FONTS"),
            |d| data_root.join(String::from_utf8_lossy(d).replace('\\', "/")),
        );
        if let Ok(backend) = crate::platform::dwrite::DWriteBackend::with_font_dir(&dir) {
            return Box::new(backend);
        }
    }
    let _ = (data_root, fonts_dir);
    Box::new(crate::text::font::fake::FakeBackend::default())
}

fn lossy(path: &[u8]) -> String {
    String::from_utf8_lossy(path).into_owned()
}

impl<B: Backend + 'static> Engine<B> {
    /// `NeuronRuntime::Startup` and the engine half of `InitializeEngine`
    /// over `backend`: every subsystem in [`CREATION_ORDER`].
    ///
    /// # Errors
    ///
    /// The engine INI does not parse, or the data directory does not exist
    /// (the reference's `InitializeEngine` fails on both).
    #[allow(clippy::too_many_lines)]
    pub fn new(config: EngineConfig, backend: B) -> Result<Engine<B>, EngineError> {
        let lifecycle = Lifecycle::new();
        let log = |stage| lifecycle.record(stage, Phase::Create);
        let mut notices = Vec::new();
        let EngineConfig {
            engine_ini,
            client_ini,
            data_root,
            mode,
            font_backend,
            double_click_ms,
            audio_device: want_audio,
        } = config;
        let client = ClientData::read(client_ini.as_deref());
        let (width, height) = mode.back_buffer();

        // NeuronRuntime::Startup: the component manager and its INI.
        let mut ini = EngineIni::new();
        match &engine_ini {
            Some(bytes) => ini
                .load_buffer(bytes, &mut NoIncludes)
                .map_err(EngineError::Ini)?,
            None => notices.push("no engine INI: the subsystems' built-in defaults".to_owned()),
        }
        if engine_ini.is_some() {
            check_lists(&ini, &mut notices);
        }
        log(Stage::Config);

        // [System], in order.
        let registry = VMeshRegistry::shared();
        log(Stage::VMeshLibrary);
        let device = Device::new(
            Defaults::from_section(ini.section(b"RenderPipeline")),
            width,
            height,
            backend,
        );
        log(Stage::RenderDevice);
        let mut mixer: Mixer<u32> = Mixer::new();
        log(Stage::SoundManager);
        let stream_seconds = stream_buffer_seconds(0.0);
        log(Stage::SoundStreamer);
        let textures = TextureLibrary::new(LibraryConfig::from_engine_ini(&ini));
        log(Stage::TextureLibrary);
        let animations = MaterialAnimations::new();
        log(Stage::MaterialAnimation);
        let mut shading = Shading::new(device, textures, FormatClasses::builtin());
        shading.animations = Some(animations);
        let mut batcher: MaterialBatcher<RenderStack<B>> = MaterialBatcher::new(
            Box::new(ShadingFactory::with_fallback(Box::new(
                EyeFactory::with_fallback(Box::new(SurfaceFactory::new())),
            ))),
            Some(&ini),
        );
        bind_eye_materials(batcher.library_mut());
        let links = Links::shared();
        let mut stack = RenderStack::new(
            shading,
            batcher,
            Rc::clone(&registry),
            Box::new(SceneHost::detached(Rc::clone(&links))),
        );
        log(Stage::MaterialBatcher);
        let mut particles = ParticleFx::new();
        particles.configure(ini.section(b"ParticleFx"));
        log(Stage::ParticleFx);

        // StartupDevice: the sound manager's INI settings and the device.
        let audio = AudioConfig::read(&ini, CLIENT_STARTUP_FLAGS);
        mixer.max_channels = audio.max_sound_channels;
        mixer.max_3d_pan = audio.max_3d_pan;
        mixer.never_set_start_pos = audio.never_set_start_pos;
        #[cfg(windows)]
        let audio_device = if want_audio {
            match crate::platform::audio::AudioDevice::new(stream_seconds) {
                Ok(device) => Some(device),
                Err(e) => {
                    notices.push(format!("the audio device did not start: {e}"));
                    None
                }
            }
        } else {
            None
        };
        #[cfg(not(windows))]
        let _ = want_audio;
        log(Stage::SoundDevice);

        // The scene engine and its [Engine] components, in order.
        let scene: SharedScene = Rc::new(RefCell::new(SceneEngine::new()));
        stack.engine = Box::new(SceneHost::new(Rc::downgrade(&scene), Rc::clone(&links)));
        log(Stage::SceneEngine);
        let animation: SharedAnimation = Rc::new(RefCell::new(Animation::new()));
        let deformables: SharedDeformables = Rc::new(RefCell::new(Vec::new()));
        log(Stage::Animation);
        scene
            .borrow_mut()
            .add_component(Box::new(Hardpoints::new()));
        log(Stage::Hardpoint);
        let deform = Deformable::new(
            ini.section(b"Deformable")
                .and_then(|s| s.value(b"DeformRate")),
        );
        let deform_host = SceneDeformHost::new(
            Rc::clone(&scene),
            Rc::clone(&links),
            Rc::clone(&animation),
            &deformables,
        );
        log(Stage::Deformable);
        let (plugins, faces) = render_plugins::<B>(&ini, &registry, &mut notices);
        let mut manager = RenderManager::new(plugins);
        for slot in faces {
            match slot {
                FaceSlot::VMesh(i, f) => manager.set_faces(i, Rc::new(f)),
                FaceSlot::TriMesh(i, f) => manager.set_faces(i, Rc::new(f)),
            }
        }
        stack.set_directory(manager.directory());
        let render = Rc::new(RefCell::new(RenderSide { stack, manager }));
        scene.borrow_mut().add_component(Box::new(RenderLink::new(
            Rc::clone(&render),
            Rc::clone(&links),
        )));
        log(Stage::RenderManager);
        let mut appearance = {
            let mut side = render.borrow_mut();
            let RenderSide { stack, manager } = &mut *side;
            Appearance::create(&mut StackAppearance { stack, manager })
        }
        .map_err(|e| notices.push(format!("FLAppearance was not created ({e})")))
        .ok();
        log(Stage::Appearance);
        render.borrow_mut().stack.wires = VMeshWire::new();
        log(Stage::VMeshWire);
        // The engine's second pass: a component whose Initialize fails is
        // removed from the aggregate.
        if let Some(mut a) = appearance.take() {
            let mut side = render.borrow_mut();
            let RenderSide { stack, manager } = &mut *side;
            let mut services = StackAppearance { stack, manager };
            if a.initialize(&mut services) >= 0 {
                appearance = Some(a);
            } else {
                notices.push("FLAppearance's Initialize failed: removed".to_owned());
                a.release(&mut services);
            }
        }
        log(Stage::EngineInitialize);
        let effects = Vec::new();
        log(Stage::Effects);
        let scripts = Vec::new();
        log(Stage::SceneScripts);

        // InitializeEngine.
        if !data_root.is_dir() {
            return Err(EngineError::NoData(data_root));
        }
        let data = Host::new(&data_root);
        log(Stage::Data);
        {
            // RenderMgrCreateBuffers: the device has the mode's back buffer;
            // the gamma ramp (RenderMgrApplyGamma) and the full viewport.
            let mut side = render.borrow_mut();
            let device = &mut side.stack.shading.device;
            device.set_gamma_enabled(mode.fullscreen);
            device.set_gamma_function(7, 1.3, 0.0, 1.0, 0.0);
            device.set_viewport(0, 0, width, height);
        }
        log(Stage::RenderBuffers);
        // One backend for the font system and the IME's box fonts, as the
        // C++'s GDI is one.
        let backend = SharedFontBackend::new(
            font_backend
                .unwrap_or_else(|| default_font_backend(&data_root, client.fonts_dir.as_deref())),
        );
        let ime_backend = backend.clone();
        let backend: Box<dyn FontBackend> = Box::new(backend);
        let surface_height = i32::try_from(height).unwrap_or(i32::MAX);
        let surface_width = i32::try_from(width).unwrap_or(i32::MAX);
        let mut fonts = FontSystem::new(backend, FontEnv::new(surface_width, surface_height));
        fonts
            .rich
            .initialize(fonts.backend.as_mut(), surface_height);
        fonts.rich.reinitialize_fonts(fonts.backend.as_mut());
        fonts.device_reset();
        log(Stage::RichText);
        let mut mouse = Mouse::new(double_click_ms, mode);
        mouse.set_display(mode);
        log(Stage::Input);
        particles.create_vertex_buffer(&mut render.borrow_mut().stack);
        log(Stage::ParticleVertexBuffer);
        for (rich, path) in &client.fonts {
            match data.read_file(path) {
                Ok(file) if *rich => fonts.load_rich_fonts(&file.data),
                Ok(file) => {
                    fonts.load_fonts(&read_fonts_ini(&file.data));
                }
                Err(_) => notices.push(format!("{}: not loaded", lossy(path))),
            }
        }
        log(Stage::Fonts);
        let mut ui = Ui::new(Surface::new(surface_width, surface_height));
        ui.fonts = Some(fonts);
        let mut cursors = Cursors::new();
        let mut ui_shape_db = ShapeDb::new();
        {
            let mut side = render.borrow_mut();
            let mut lib = StackShapes::new(&mut side.stack, &data);
            match data.read_file(b"mouse.ini") {
                Ok(file) => cursors.init(&file.data, &mut lib),
                Err(_) => notices.push("mouse.ini: not loaded".to_owned()),
            }
            match data.read_file(b"Interface\\UI\\UITextures.ini") {
                Ok(file) => {
                    ui_shape_db.load(&file.data, &mut lib);
                    ui.ui_shapes = ScrollShapes::from_db(&ui_shape_db);
                }
                Err(_) => notices.push("Interface\\UI\\UITextures.ini: not loaded".to_owned()),
            }
            // `LoadButtonMontage`, which the reference runs on first use
            // (divergence D511).
            let textures = data.read_file(crate::ui::montage::BUTTON_TEXTURES_INI).ok();
            let montage = data.read_file(crate::ui::montage::BUTTON_MONTAGE_INI).ok();
            if montage.is_none() {
                notices.push("interface\\ButtonMontage.ini: not loaded".to_owned());
            }
            ui.montage.load(
                textures.as_ref().map(|f| f.data.as_slice()),
                montage.as_ref().map(|f| f.data.as_slice()),
                &mut lib,
            );
        }
        // `Win3DStartupManager`'s material entry: the interface's shared
        // meshes, materials and textures (`interface\interface.generic.vms`),
        // which the control meshes reference.
        let loaded: Rc<RefCell<Loaded>> = Rc::default();
        match UtfFileSystem::open(
            &data,
            &FsDesc::read(INTERFACE_GENERIC).with_implementation(b"UTF"),
        ) {
            Ok(fs) => {
                load_libraries(&render, &registry, &mut loaded.borrow_mut(), fs);
            }
            Err(_) => notices.push(format!("{}: not loaded", lossy(INTERFACE_GENERIC))),
        }
        log(Stage::Ui);
        // EnsureClientIMEFocus: the focus manager, on no IME until the
        // window's is given (`set_ime_context`); "ConfirmPlayerText" for its
        // strings and its boxes' LOGFONTA; the edit windows reach it through
        // the services.
        let ime = ImeBoxes::without_ime(ime_backend);
        if let Some(fonts) = ui.fonts.as_mut()
            && let Some(id) = fonts.find_font(b"ConfirmPlayerText")
        {
            let log_font = fonts
                .font_cx(id)
                .map(|(font, mut cx)| font.log_font(&mut cx));
            ime.set_confirm_font(Some(id), log_font);
        }
        ui.services = Box::new(ImeServices(ime.clone()));
        log(Stage::ImeFocus);
        if let Some(fonts) = ui.fonts.as_mut() {
            fonts
                .rich
                .initialize(fonts.backend.as_mut(), surface_height);
            fonts.rich.reinitialize_fonts(fonts.backend.as_mut());
            fonts
                .rich
                .set_image_root(data_root.to_string_lossy().as_bytes());
        }
        log(Stage::ImageRoot);
        apply_extents(&scene, &links);
        let ui_meshes = Rc::new(std::cell::Cell::new(0));
        // The control meshes through the scene engine (`ClientMeshLoadFromPath`).
        ui.mesh_loader = Some(Box::new(ui_meshes::EngineMeshLoader {
            data: data.clone(),
            registry: Rc::clone(&registry),
            render: Rc::clone(&render),
            scene: Rc::clone(&scene),
            links: Rc::clone(&links),
            loaded: Rc::clone(&loaded),
            made: Rc::clone(&ui_meshes),
        }));

        let sound = sound::EngineSound::new(
            &audio.reverb,
            audio.force_frequency_to_software,
            client.sounds.clone(),
            client.voices.clone(),
        );
        let mut engine = Engine {
            lifecycle,
            ini,
            notices,
            registry,
            render,
            mixer,
            audio,
            stream_seconds,
            #[cfg(windows)]
            audio_device,
            particles,
            particle_batcher: false,
            fx_meshes: FxMeshes::default(),
            scene,
            links,
            animation,
            deform,
            deform_host,
            deformable_events: Vec::new(),
            appearance,
            effects,
            scripts,
            sound,
            data: Some(data),
            mode,
            fonts: None,
            mouse: Some(mouse),
            ui: Some(ui),
            cursors,
            ui_shape_db,
            ime: Some(ime),
            options: options::GameOptions::default(),
            lights: lights::EngineLights::default(),
            models: Vec::new(),
            deformables,
            backdrop: None,
            starsphere: None,
            loaded,
            win3d: crate::ui::win3d_immediate::Win3dLayer::new(width, height),
            batchers: crate::ui::win3d_renderables::LineBatchers::new(),
            quads: crate::ui::quad_batch::QuadBatcher::new(),
            elapsed: 0.0,
            ui_meshes,
            frames: 0,
            torn_down: false,
        };
        // StartupDevice's SetEaxPreview of the [AudioProperties] reverb.
        engine.apply_reverb();
        Ok(engine)
    }

    /// The lifecycle log (shared: it can be read after the engine drops).
    #[must_use]
    pub fn lifecycle(&self) -> Lifecycle {
        self.lifecycle.clone()
    }

    /// The engine INI as parsed.
    #[must_use]
    pub fn ini(&self) -> &EngineIni {
        &self.ini
    }

    /// The display mode.
    #[must_use]
    pub fn mode(&self) -> DisplayMode {
        self.mode
    }

    /// The render stack and the render manager.
    pub fn render(&self) -> RefMut<'_, RenderSide<B>> {
        self.render.borrow_mut()
    }

    /// The render device (through the render side).
    pub fn device(&self) -> RefMut<'_, Device<B>> {
        RefMut::map(self.render.borrow_mut(), |s| &mut s.stack.shading.device)
    }

    /// The scene engine.
    #[must_use]
    pub fn scene(&self) -> &SharedScene {
        &self.scene
    }

    /// The links between the scene engine and the render side.
    #[must_use]
    pub fn links(&self) -> &SharedLinks {
        &self.links
    }

    /// The data file system's host.
    #[must_use]
    pub fn data(&self) -> Option<&Host> {
        self.data.as_ref()
    }

    /// The UI manager (which holds the fonts).
    pub fn ui_mut(&mut self) -> Option<&mut Ui> {
        self.ui.as_mut()
    }

    /// The UI manager.
    #[must_use]
    pub fn ui(&self) -> Option<&Ui> {
        self.ui.as_ref()
    }

    /// The font system.
    #[must_use]
    pub fn fonts(&self) -> Option<&FontSystem> {
        self.ui.as_ref().and_then(|u| u.fonts.as_ref())
    }

    /// The mouse model.
    pub fn mouse_mut(&mut self) -> Option<&mut Mouse> {
        self.mouse.as_mut()
    }

    /// The control meshes the interface made through the engine so far.
    #[must_use]
    pub fn ui_meshes_made(&self) -> usize {
        self.ui_meshes.get()
    }

    /// The `mouse.ini` cursors, to choose the one in use
    /// (`MouseSetCursor`).
    pub fn cursors_mut(&mut self) -> &mut Cursors {
        &mut self.cursors
    }

    /// The `mouse.ini` cursors.
    #[must_use]
    pub fn cursors(&self) -> &Cursors {
        &self.cursors
    }

    /// The client's IME boxes and their focus manager.
    #[must_use]
    pub fn ime_boxes(&self) -> Option<&ImeBoxes> {
        self.ime.as_ref()
    }

    /// The window's IME for the boxes the interface registers
    /// (`IMEFocus`'s window): on Windows the game window's
    /// (`platform::ime::WindowIme`), which `engine::windows::create_engine`
    /// gives a window's engine.
    pub fn set_ime_context(&mut self, context: Box<dyn ImeContext>) {
        if let Some(ime) = &self.ime {
            ime.set_context(context);
        }
    }

    /// `IMEFocus::OnChar` (`GSendChar`): a unit of the IME's result string
    /// for the focused box, whose window then shows the box's text.
    pub fn ime_char(&mut self, unit: u16) -> bool {
        let taken = self.ime.as_ref().is_some_and(|ime| ime.on_char(unit));
        self.apply_ime_texts();
        taken
    }

    /// `IMEFocus::OnSetFocus` (`gained`) or `OnKillFocus`: the window's
    /// keyboard focus changed.
    pub fn ime_window_focus(&mut self, gained: bool) {
        if let Some(ime) = &self.ime {
            ime.on_window_focus(gained);
        }
        self.apply_ime_texts();
    }

    /// The texts the boxes handed their windows outside the windows' own
    /// calls, set into the windows (`FLImeInputBox::UpdatedText`, D553).
    pub fn apply_ime_texts(&mut self) {
        let (Some(ime), Some(ui)) = (self.ime.as_ref(), self.ui.as_mut()) else {
            return;
        };
        for (owner, text) in ime.take_owner_texts() {
            crate::ui::controls::rich_text_edit::set_ime_owner_text(ui, owner, &text);
        }
    }

    /// `FLAppearance`.
    #[must_use]
    pub fn appearance(&self) -> Option<&Appearance> {
        self.appearance.as_ref()
    }

    /// The audio device, when it started.
    #[cfg(windows)]
    #[must_use]
    pub fn audio_device(&self) -> Option<&crate::platform::audio::AudioDevice> {
        self.audio_device.as_ref()
    }

    /// The frames run so far.
    #[must_use]
    pub fn frames(&self) -> u64 {
        self.frames
    }

    fn open_archive(&self, path: &[u8]) -> Result<UtfFileSystem, EngineError> {
        let data = self
            .data
            .as_ref()
            .ok_or_else(|| EngineError::Open(lossy(path)))?;
        UtfFileSystem::open(data, &FsDesc::read(path).with_implementation(b"UTF"))
            .map_err(|_| EngineError::Open(lossy(path)))
    }

    /// The libraries of one archive of the data, as the client loads an
    /// archetype's files: its `Texture library` into the texture library,
    /// its `Material library` into the material library (verified and
    /// resolved), its material animations and its `VMeshLibrary` into the
    /// shared registry. The groups are freed at the teardown.
    ///
    /// # Errors
    ///
    /// The archive does not open.
    pub fn load_archive(&mut self, path: &[u8]) -> Result<ArchiveLoad, EngineError> {
        let fs = self.open_archive(path)?;
        Ok(self.load_libraries(fs))
    }

    /// [`Engine::load_archive`] over an open archive.
    fn load_libraries(&mut self, fs: UtfFileSystem) -> ArchiveLoad {
        load_libraries(
            &self.render,
            &self.registry,
            &mut self.loaded.borrow_mut(),
            fs,
        )
    }

    /// A model through the scene engine: the archive's libraries
    /// ([`Engine::load_archive`]), its archetype (a compound's parts each
    /// offered to the render manager and the `VMeshWire` component) and an
    /// instance tree at the origin.
    ///
    /// # Errors
    ///
    /// The archive does not open, or the scene engine makes no archetype
    /// or instance from it.
    pub fn create_model(&mut self, path: &[u8]) -> Result<ModelId, EngineError> {
        self.load_archive(path)?;
        let fs = self.open_archive(path)?;
        self.create_model_in(path, fs)
    }

    /// [`Engine::create_model`] from an archive's bytes instead of a file
    /// of the data (a model made by a tool or a test); `path` names it.
    ///
    /// # Errors
    ///
    /// The bytes are not an archive, or the scene engine makes no
    /// archetype or instance from it.
    pub fn create_model_from_bytes(
        &mut self,
        path: &[u8],
        bytes: Arc<[u8]>,
    ) -> Result<ModelId, EngineError> {
        let open = |bytes: Arc<[u8]>| {
            UtfFileSystem::from_bytes(bytes, path).map_err(|_| EngineError::Open(lossy(path)))
        };
        let fs = open(Arc::clone(&bytes))?;
        self.load_libraries(fs);
        let fs = open(bytes)?;
        self.create_model_in(path, fs)
    }

    fn create_model_in(
        &mut self,
        path: &[u8],
        mut fs: UtfFileSystem,
    ) -> Result<ModelId, EngineError> {
        // Named by the whole relative path, as the client's mesh loader
        // names its archetypes: the shipped data has models of one file
        // name in several folders.
        let name = path.to_vec();
        let archetype = self
            .scene
            .borrow_mut()
            .create_archetype_handle(&name, Some(&mut fs))
            .map_err(|e| EngineError::Scene(format!("{}: {e}", lossy(path))))?
            .ok_or_else(|| EngineError::Scene(lossy(path)))?;
        self.links.borrow_mut().take_made();
        let root = self.scene.borrow_mut().create_instance(archetype);
        self.links.borrow_mut().take_made();
        apply_extents(&self.scene, &self.links);
        let Some(root) = root else {
            self.scene.borrow_mut().release_handle(archetype);
            return Err(EngineError::Scene(lossy(path)));
        };
        self.models.push(Some(Model { root, archetype }));
        Ok(ModelId(self.models.len() - 1))
    }

    /// Places a model's root instance.
    pub fn set_model_transform(&mut self, model: ModelId, transform: &Transform) {
        if let Some(Some(m)) = self.models.get(model.0) {
            self.scene
                .borrow_mut()
                .set_instance_matrix(Some(m.root), transform);
        }
    }

    /// The scene instance at a model's root.
    #[must_use]
    pub fn model_root(&self, model: ModelId) -> Option<InstanceId> {
        self.models.get(model.0).copied().flatten().map(|m| m.root)
    }

    /// Draws `model` as the backdrop (`DrawBackdrop`) or the star sphere
    /// (`DrawStarsphere`) of each frame instead of with the models; `None`
    /// clears the slot.
    pub fn set_backdrop(&mut self, model: Option<ModelId>, starsphere: bool) {
        if starsphere {
            self.starsphere = model;
        } else {
            self.backdrop = model;
        }
    }

    /// Destroys a model: its instance tree, then its archetype's reference.
    pub fn destroy_model(&mut self, model: ModelId) {
        let Some(m) = self.models.get_mut(model.0).and_then(Option::take) else {
            return;
        };
        for slot in [&mut self.backdrop, &mut self.starsphere] {
            if *slot == Some(model) {
                *slot = None;
            }
        }
        let mut scene = self.scene.borrow_mut();
        scene.destroy_instance(m.root);
        scene.release_handle(m.archetype);
    }

    /// The node and effect libraries of a `.ale` file of the data, into
    /// the particle system: the names of the effects it holds.
    ///
    /// # Errors
    ///
    /// The file cannot be read or is not an effect archive.
    pub fn load_effects(&mut self, path: &[u8]) -> Result<Vec<Vec<u8>>, EngineError> {
        let data = self
            .data
            .as_ref()
            .ok_or_else(|| EngineError::Open(lossy(path)))?;
        let file = data
            .read_file(path)
            .map_err(|_| EngineError::Open(lossy(path)))?;
        let ale = AleFile::parse(file.data)
            .map_err(|e| EngineError::Effect(format!("{}: {e}", lossy(path))))?;
        let names = ale
            .effects
            .effects()
            .iter()
            .map(|e| e.name.clone())
            .collect();
        self.particles.load(&ale);
        Ok(names)
    }

    /// An effect of the effect library by name, under a folder node of
    /// its own (`CreateEffect`).
    ///
    /// # Errors
    ///
    /// The library has no such effect, or one of its nodes is missing.
    pub fn create_effect(&mut self, name: &[u8]) -> Result<EffectId, EngineError> {
        let hash = fx::name_hash(name);
        let mut side = self.render.borrow_mut();
        let mut services = EffectServices {
            stack: &mut side.stack,
            manager: None,
            scene: &self.scene,
            data: self.data.as_ref(),
            meshes: Some(&mut self.fx_meshes),
        };
        let parent = self.particles.create_node(NodeClass::Node);
        match self.particles.create_effect(parent, hash, &mut services) {
            Ok(effect) => {
                self.effects.push(Some(LiveEffect { parent, effect }));
                Ok(EffectId(self.effects.len() - 1))
            }
            Err(e) => {
                self.particles.release(parent, &mut services);
                Err(EngineError::Effect(format!("{}: {e:?}", lossy(name))))
            }
        }
    }

    /// Places an effect (transform slot 0 of its folder).
    pub fn set_effect_transform(&mut self, effect: EffectId, transform: &[f32; 12]) {
        if let Some(Some(live)) = self.effects.get(effect.0) {
            live.effect.set_transform(&mut self.particles, 0, transform);
        }
    }

    /// The effect, for its `IFxEffect` calls.
    #[must_use]
    pub fn effect(&self, effect: EffectId) -> Option<&Effect> {
        self.effects
            .get(effect.0)
            .and_then(Option::as_ref)
            .map(|l| &l.effect)
    }

    /// Stops and releases an effect and its folder, as the client's
    /// `ParticleEffectCleanupParticleFx` does: `Slot13(7)` (its live roots
    /// submitted, its records released), the effect's `Release`, then the
    /// folder detached and released.
    pub fn destroy_effect(&mut self, effect: EffectId) {
        let Some(live) = self.effects.get_mut(effect.0).and_then(Option::take) else {
            return;
        };
        {
            let mut side = self.render.borrow_mut();
            let mut services = EffectServices {
                stack: &mut side.stack,
                manager: None,
                scene: &self.scene,
                data: self.data.as_ref(),
                meshes: Some(&mut self.fx_meshes),
            };
            cleanup_effect(&mut self.particles, live, &mut services);
        }
        self.settle_fx_meshes();
    }

    /// The meshes of the client's mesh factory still held by the effects'
    /// mesh appearances, and how many of them have an engine instance.
    #[must_use]
    pub fn fx_mesh_count(&self) -> (usize, usize) {
        (self.fx_meshes.live(), self.fx_meshes.instanced())
    }

    /// What the mesh factory left for after the particle walk (D493): the
    /// instances of released meshes destroyed, then the new meshes'
    /// archetypes loaded (`Archetype::Load`: the archive's libraries the
    /// first time, the archetype found or made) and instances made, placed
    /// and posed.
    fn settle_fx_meshes(&mut self) {
        for instance in self.fx_meshes.take_doomed() {
            self.scene.borrow_mut().destroy_instance(instance);
        }
        for (mesh, path) in self.fx_meshes.take_pending() {
            let instance = self.load_fx_mesh(&path);
            self.fx_meshes.bind(mesh, instance, &self.scene);
        }
    }

    /// `Archetype::Load(path)` and `ClientMeshHolderBindInstance` for a
    /// mesh of the mesh factory: the instance, whose archetype it holds.
    fn load_fx_mesh(&mut self, path: &[u8]) -> Option<InstanceId> {
        if self.fx_meshes.first_load(path) {
            let _ = self.load_archive(path);
        }
        let mut fs = self.open_archive(path).ok()?;
        let archetype = self
            .scene
            .borrow_mut()
            .create_archetype_handle(path, Some(&mut fs))
            .ok()
            .flatten()?;
        self.links.borrow_mut().take_made();
        let instance = self.scene.borrow_mut().create_instance(archetype);
        self.links.borrow_mut().take_made();
        self.scene.borrow_mut().release_handle(archetype);
        instance
    }

    /// A deformable model (`IDeformable::Create`) from one `.dfm` file of
    /// the data, its skeleton and instances made through the scene engine.
    ///
    /// # Errors
    ///
    /// The deformable component could not make it.
    pub fn create_deformable(&mut self, path: &[u8]) -> Result<DeformableId, EngineError> {
        let data = self
            .data
            .clone()
            .ok_or_else(|| EngineError::Open(lossy(path)))?;
        let mut desc = [PartDesc {
            lod_count: 8,
            lods: vec![LodDesc::default(); 8],
            path: Some(path.to_vec()),
            flags: 0,
            lod_limit: 8,
        }];
        let mut open = |d: &PartDesc| -> Option<Box<dyn FileSystem>> {
            let p = d.path.clone()?;
            UtfFileSystem::open(&data, &FsDesc::read(&p).with_implementation(b"UTF"))
                .ok()
                .map(|fs| Box::new(fs) as Box<dyn FileSystem>)
        };
        let index = self.deformables.borrow().len();
        let made = {
            let mut side = self.render.borrow_mut();
            self.deform_host.set_current(Some(index));
            let mut services = StackDeform::new(&mut side.stack, &mut self.deform_host);
            self.deform
                .create(&mut desc, 0, 0, &mut open, &mut services)
        };
        self.deform_host.set_current(None);
        self.links.borrow_mut().take_made();
        apply_extents(&self.scene, &self.links);
        let instance = made.map_err(|_| EngineError::Deformable(lossy(path)))?;
        self.deformables.borrow_mut().push(Some(instance));
        Ok(DeformableId(index))
    }

    /// A deformable model, for its `IDeformable` calls.
    pub fn deformable_mut(&mut self, id: DeformableId) -> Option<RefMut<'_, DeformInstance>> {
        RefMut::filter_map(self.deformables.borrow_mut(), |models| {
            models.get_mut(id.0).and_then(Option::as_mut)
        })
        .ok()
    }

    /// `IDeformable::SetLod(instance, dt, lods)`: each part's level (the
    /// game picks them, as `hud_comm.cpp` and `gf_character_object.cpp`
    /// do every frame) and the skin rate's countdown.
    pub fn set_deformable_lod(&mut self, id: DeformableId, dt: f32, lods: &[i32]) {
        let rate = self.deform.rate;
        if let Some(instance) = self
            .deformables
            .borrow_mut()
            .get_mut(id.0)
            .and_then(Option::as_mut)
        {
            instance.set_lod(dt, lods, rate);
        }
    }

    /// Destroys a deformable model (`IDeformable::Destroy`: its list of
    /// animations and aim events first).
    pub fn destroy_deformable(&mut self, id: DeformableId) {
        let taken = self
            .deformables
            .borrow_mut()
            .get_mut(id.0)
            .and_then(Option::take);
        let Some(instance) = taken else {
            return;
        };
        let mut side = self.render.borrow_mut();
        self.deform_host.set_current(Some(id.0));
        let mut services = StackDeform::new(&mut side.stack, &mut self.deform_host);
        self.deform.destroy(instance, &mut services);
        self.deform_host.set_current(None);
    }

    /// A scene script (`.thn`) of the data, loaded, built and started.
    ///
    /// # Errors
    ///
    /// The file cannot be read, or its chunk does not load or build.
    pub fn load_scene_script(&mut self, path: &[u8]) -> Result<SceneScriptId, EngineError> {
        let data = self
            .data
            .as_ref()
            .ok_or_else(|| EngineError::Open(lossy(path)))?;
        let bytes = data
            .read_file(path)
            .map_err(|_| EngineError::Open(lossy(path)))?
            .data;
        let mut vm = Vm::new();
        let mut state = BuildState::new();
        let summary = load(&mut vm, &bytes)
            .map_err(|e| EngineError::Script(format!("{}: {e}", lossy(path))))?;
        let duration = summary.duration_ms;
        let scene = build(&mut vm, &mut state, &bytes, summary, true)
            .map_err(|e| EngineError::Script(format!("{}: {e}", lossy(path))))?;
        let mut player = Player::new();
        player.load(&scene, duration);
        // The scene root answers the reverb events through the sound
        // manager, as the game's scene object does.
        sound::attach_scene_reverb(&mut player, &self.sound.reverb);
        player.start(0);
        self.scripts.push(Some(player));
        Ok(SceneScriptId(self.scripts.len() - 1))
    }

    /// A playing scene script.
    pub fn scene_script_mut(&mut self, id: SceneScriptId) -> Option<&mut Player> {
        self.scripts.get_mut(id.0).and_then(Option::as_mut)
    }

    /// Stops and removes a scene script.
    pub fn remove_scene_script(&mut self, id: SceneScriptId) {
        if let Some(mut player) = self.scripts.get_mut(id.0).and_then(Option::take) {
            player.stop();
        }
    }

    /// The bounding sphere of a model's instance tree (`GetInstanceExtent`
    /// of its root over the subtree): the radius and the centre.
    #[must_use]
    pub fn model_bounds(&self, model: ModelId) -> Option<(f32, [f32; 3])> {
        let root = self.model_root(model)?;
        let (radius, centre) = self.scene.borrow_mut().instance_extent(Some(root), 0);
        (radius > 0.0).then_some((radius, centre.to_words()))
    }

    /// The scene instance of a deformable model's root bone
    /// (`FirstBoneInstance`), which places the model and under which its
    /// animations bind.
    #[must_use]
    pub fn deformable_root(&self, id: DeformableId) -> Option<InstanceId> {
        let models = self.deformables.borrow();
        let instance = models.get(id.0)?.as_ref()?;
        u32::try_from(instance.first_bone())
            .ok()
            .map(InstanceId::from_index)
    }

    /// A deformable model's bounding sphere (`GetBoundSphere`): the radius
    /// and the centre of its first part.
    pub fn deformable_bounds(&mut self, id: DeformableId) -> Option<(f32, [f32; 3])> {
        let models = self.deformables.borrow();
        let instance = models.get(id.0)?.as_ref()?;
        instance.bound_sphere(&mut self.deform.meshes)
    }

    /// Places a deformable model: its root bone's transform, the tree
    /// updated and the bones' world transforms handed to the model.
    pub fn set_deformable_transform(&mut self, id: DeformableId, transform: &Transform) {
        let Some(root) = self.deformable_root(id) else {
            return;
        };
        let mut scene = self.scene.borrow_mut();
        scene.set_instance_matrix(Some(root), transform);
        scene.update_instance(Some(root), 0);
        if let Some(Some(instance)) = self.deformables.borrow_mut().get_mut(id.0) {
            pull_bones(instance, &scene);
        }
    }

    /// Every step of [`TEARDOWN_ORDER`]; [`Drop`] runs it.
    #[allow(clippy::too_many_lines)]
    fn teardown(&mut self) {
        if self.torn_down {
            return;
        }
        self.torn_down = true;
        let lifecycle = self.lifecycle.clone();
        let log = |stage| lifecycle.record(stage, Phase::Teardown);

        // ClientAtExitTeardown, up to NeuronRuntime::Shutdown.
        let mut montage = crate::ui::montage::ButtonMontage::default();
        if let Some(mut ui) = self.ui.take() {
            ui.shutdown();
            self.fonts = ui.fonts.take();
            montage = std::mem::take(&mut ui.montage);
        }
        if let Some(data) = self.data.as_ref() {
            let mut side = self.render.borrow_mut();
            let mut lib = StackShapes::new(&mut side.stack, data);
            // `ClearButtonMontage`.
            montage.clear(&mut lib);
            self.ui_shape_db.clear(&mut lib);
            self.cursors.shapes.clear(&mut lib);
        }
        log(Stage::Ui);
        if let Some(fonts) = self.fonts.as_mut() {
            let mut side = self.render.borrow_mut();
            let mut pipeline = side.stack.shading.pipeline();
            fonts.shutdown(&mut pipeline);
        }
        log(Stage::Fonts);
        if let Some(mut fonts) = self.fonts.take() {
            fonts.rich.shutdown(fonts.backend.as_mut());
        }
        log(Stage::RichText);
        log(Stage::RenderBuffers);
        self.data = None;
        log(Stage::Data);

        // NeuronRuntime::Shutdown.
        {
            let mut side = self.render.borrow_mut();
            let mut services = EffectServices {
                stack: &mut side.stack,
                manager: None,
                scene: &self.scene,
                data: None,
                meshes: Some(&mut self.fx_meshes),
            };
            for live in std::mem::take(&mut self.effects).into_iter().flatten() {
                cleanup_effect(&mut self.particles, live, &mut services);
            }
        }
        self.settle_fx_meshes();
        log(Stage::Effects);
        for player in self.scripts.iter_mut().flatten() {
            player.stop();
        }
        self.scripts.clear();
        log(Stage::SceneScripts);
        // The engine's destructor: its instances and archetypes, then its
        // components in [Engine] order. The deformable models go first, each
        // with its list of animations and aim events.
        let models = std::mem::take(&mut *self.deformables.borrow_mut());
        for (index, instance) in models.into_iter().enumerate() {
            let Some(instance) = instance else { continue };
            let mut side = self.render.borrow_mut();
            self.deform_host.set_current(Some(index));
            let mut services = StackDeform::new(&mut side.stack, &mut self.deform_host);
            self.deform.destroy(instance, &mut services);
        }
        self.deform_host.set_current(None);
        for id in 0..self.models.len() {
            self.destroy_model(ModelId(id));
        }
        self.models.clear();
        log(Stage::SceneEngine);
        *self.animation.borrow_mut() = Animation::new();
        log(Stage::Animation);
        log(Stage::Hardpoint);
        self.deform = Deformable::new(None);
        log(Stage::Deformable);
        let mut empty = RenderManager::new(Vec::new());
        {
            let mut side = self.render.borrow_mut();
            let RenderSide { stack, manager } = &mut *side;
            std::mem::replace(manager, RenderManager::new(Vec::new())).destroy(stack);
        }
        log(Stage::RenderManager);
        if let Some(appearance) = self.appearance.take() {
            let mut side = self.render.borrow_mut();
            appearance.release(&mut StackAppearance {
                stack: &mut side.stack,
                manager: &mut empty,
            });
        }
        log(Stage::Appearance);
        self.render.borrow_mut().stack.wires = VMeshWire::new();
        log(Stage::VMeshWire);

        // The libraries the system container keeps (D451): meshes,
        // materials and material animations before the textures they hold.
        {
            let mut side = self.render.borrow_mut();
            let stack = &mut side.stack;
            let mut loaded = self.loaded.borrow_mut();
            for list in std::mem::take(&mut loaded.meshes) {
                self.registry.borrow_mut().unload_library(stack, list);
            }
            log(Stage::VMeshLibrary);
            for group in std::mem::take(&mut loaded.materials) {
                stack.with_batcher(|b, s| b.library_mut().delete_group(s, group.cast_signed()));
            }
            log(Stage::MaterialBatcher);
            if let Some(animations) = stack.shading.animations.as_mut() {
                for group in std::mem::take(&mut loaded.animations) {
                    animations.unload_group(group);
                }
            }
            log(Stage::MaterialAnimation);
            let (textures, mut pipeline) = stack.shading.texture_parts();
            for group in std::mem::take(&mut loaded.textures) {
                textures.garbage_collect(group, &mut pipeline);
            }
            log(Stage::TextureLibrary);
            self.particles.clear_node_library(stack);
            log(Stage::ParticleFx);
            self.particles.release_vertex_buffer(stack);
            log(Stage::ParticleVertexBuffer);
        }
        log(Stage::SoundStreamer);
        // The client's sounds stop and their archetypes go before the
        // device (`StopAllClientSounds`, `ShutdownClientSoundArch`).
        self.shutdown_sounds();
        #[cfg(windows)]
        {
            self.audio_device = None;
        }
        log(Stage::SoundDevice);
        self.mixer = Mixer::new();
        log(Stage::SoundManager);
        self.render
            .borrow_mut()
            .stack
            .shading
            .device
            .sync_resources();
        log(Stage::RenderDevice);
        self.ini = EngineIni::new();
        log(Stage::Config);

        // The rest of ClientAtExitTeardown.
        self.ime = None;
        log(Stage::ImeFocus);
        self.mouse = None;
        log(Stage::Input);
    }
}

/// The libraries of an open archive into the render side (the engine's
/// [`Engine::load_archive`] and the interface's mesh loads): its `Texture
/// library` into the texture library, its `Material library` into the
/// material library (verified and resolved), its material animations and
/// its `VMeshLibrary` into the shared registry, each group kept in
/// `loaded` for the teardown.
fn load_libraries<B: Backend + 'static>(
    render: &RefCell<RenderSide<B>>,
    registry: &crate::render::mesh::vmesh::SharedRegistry,
    loaded: &mut Loaded,
    mut fs: UtfFileSystem,
) -> ArchiveLoad {
    let mut out = ArchiveLoad::default();
    let mut side = render.borrow_mut();
    let stack = &mut side.stack;
    if fs.open_node(b"Texture library").is_ok() {
        let _ = fs.open_node(b"..");
        let (textures, mut pipeline) = stack.shading.texture_parts();
        out.textures = textures.load_group(&mut fs, &mut pipeline, true).group;
        loaded.textures.extend(out.textures);
    }
    if fs.open_node(b"Material library").is_ok() {
        let _ = fs.open_node(b"..");
        let load = stack.with_batcher(|b, s| b.library_mut().load_library(s, &mut fs, true));
        if let Some(load) = load
            && load.result >= 0
            && let Some(group) = load.group
        {
            stack.with_batcher(|b, s| {
                b.library_mut().apply_verify_group(s, group.cast_signed());
                b.library_mut()
                    .resolve_textures_group(s, group.cast_signed(), 0.0);
            });
            out.materials = Some(group);
            loaded.materials.push(group);
        }
    }
    if let Some(animations) = stack.shading.animations.as_mut()
        && let Some(group) = animations.load_group(&mut fs)
    {
        out.animations = true;
        loaded.animations.push(group);
    }
    let mut root: Box<dyn FileSystem> = Box::new(fs);
    let list = registry.borrow_mut().load_library(stack, root.as_mut(), -1);
    out.meshes = list.as_ref().map_or(0, Vec::len);
    if list.is_some() {
        loaded.meshes.push(list);
    }
    out
}

/// Applies the extent writes the scene engine could not take while it was
/// busy.
fn apply_extents(scene: &SharedScene, links: &SharedLinks) {
    let pending = links.borrow_mut().take_extents();
    if pending.is_empty() {
        return;
    }
    let mut scene = scene.borrow_mut();
    for (id, radius, centre) in pending {
        scene.set_instance_extent(
            Some(id),
            1,
            radius,
            neuron_core::math::Vector::from_words(centre),
        );
    }
}

impl<B: Backend + 'static> Drop for Engine<B> {
    fn drop(&mut self) {
        self.teardown();
    }
}

impl<B: Backend + 'static> fmt::Debug for Engine<B> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Engine")
            .field("mode", &self.mode)
            .field("frames", &self.frames)
            .field("notices", &self.notices)
            .finish_non_exhaustive()
    }
}
