//! The order the engine builds and tears down its subsystems in, and where
//! the reference makes each step (work package P4.16, decision T9).
//!
//! The reference composes the client in four places:
//!
//! - `NeuronRuntime::Startup` (`CppSource/NeuronRuntime/NeuronRuntime.cpp`):
//!   the component manager reads the engine INI (`SetINIConfig`), the
//!   system container creates one component per `[System]` line in order
//!   (`LoadSystemComponents`, `system.cpp`), the sound device starts
//!   (`StartupDevice`) and the streamer is given it (`InitializeDevice`),
//!   the scene engine creates one component per `[Engine]` line in order
//!   and then initialises each in order (`LoadEngineComponents`,
//!   `SceneRuntime.cpp`'s two passes), and the effect library is bound to
//!   the particle system (`BindParticleFx`).
//! - `InitializeEngine` (`Client/engine_init.cpp`): the data file system
//!   (`OpenData`), the render mode (`SelectInitialRenderMode`, whose
//!   `RenderMgrCreateBuffers` creates the device buffers, initialises the
//!   rich text and the fonts' device state and enables the mouse), the
//!   particle vertex buffer (`CreateVertexBuffer`), then, after the startup
//!   movies and the game data, the fonts of `OutpostFrontier.ini`'s `[Data]`
//!   (`LoadClientIniData`), the UI manager (`Win3DStartupManager`), the IME
//!   focus (`EnsureClientIMEFocus`) and the rich text's image root
//!   (`DeviceTRC::set_image_root`).
//! - `ClientAtExitTeardown` (`Client/main.cpp`): the UI manager, the fonts,
//!   the rich text, the render manager's window state and the data file
//!   system go before `NeuronRuntime::Shutdown`; the IME focus after it.
//! - `NeuronRuntime::Shutdown`: the effects are cleared, the scene engine
//!   is released (its destructor clears its instances and archetypes and
//!   releases its components in `[Engine]` order), then the libraries, the
//!   particle system, the streamer, the sound device and the pipeline, and
//!   last the component manager.
//!
//! [`CREATION_ORDER`] and [`TEARDOWN_ORDER`] are that order for the
//! subsystems the port has; [`Stage::reference`] names the C++ function
//! and the call in it that makes each step, and `tests/engine_order.rs`
//! checks the order of those calls in the C++ sources and the shipped
//! `[System]` and `[Engine]` lists against these tables.
//!
//! The `[Libraries]` list (design/facts.md F1) is the order the reference
//! loads its DLLs and registers their factories in; nothing is created
//! then. Under static composition the libraries are Rust modules, listed
//! in [`LIBRARIES`] with the module that holds each.

use std::cell::RefCell;
use std::rc::Rc;

/// One step of the engine's construction or teardown.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Stage {
    /// The engine INI (`SetINIConfig`; the component manager's shutdown).
    Config,
    /// `[System] VMeshLibrary`: the shared `VMesh` registry.
    VMeshLibrary,
    /// `[System] DirectX8`: the render device (`render::ff::Device`).
    RenderDevice,
    /// `[System] SoundManager`: the mixer and its INI settings.
    SoundManager,
    /// `[System] SoundStreamer`: the streamer's buffer length.
    SoundStreamer,
    /// `[System] TextureLibrary`.
    TextureLibrary,
    /// `[System] MaterialAnimation`: the material animation library.
    MaterialAnimation,
    /// `[System] MaterialBatcher`: the batcher with the material library
    /// and its factory chain (`Shading`, the `EyeMaterial` of
    /// `Deformable2` and `surfacematerials`).
    MaterialBatcher,
    /// `[System] ParticleFx` and `FxRuntime`: the particle system.
    ParticleFx,
    /// `StartupDevice` and the streamer's `InitializeDevice`: the audio
    /// device (Windows, when asked for).
    SoundDevice,
    /// The scene engine (`CreateInstance("Engine")`).
    SceneEngine,
    /// `[Engine] Animation`: the animation system.
    Animation,
    /// `[Engine] Hardpoint`: the hardpoint component.
    Hardpoint,
    /// `[Engine] Deformable`: the deformable module.
    Deformable,
    /// `[Engine] RenderManager`, with the `[RenderManager]` plugins.
    RenderManager,
    /// `[Engine] FLAppearance` (created; initialised by
    /// [`Stage::EngineInitialize`]).
    Appearance,
    /// `[Engine] VMeshWire`.
    VMeshWire,
    /// The engine's second pass: every component's `Initialize`, in
    /// order (`FLAppearance`'s is the one with work).
    EngineInitialize,
    /// `FxEffectLibrary` bound to the particle system: the live effects.
    Effects,
    /// The playing scene scripts. `SceneScript.dll` has no system
    /// component; the client makes a script engine per scene (P5.9).
    SceneScripts,
    /// `OpenData`: the data file system.
    Data,
    /// `RenderMgrCreateBuffers`: the back buffer, the gamma ramp, the
    /// viewport (`ShutdownRenderMgr(false)` in the teardown).
    RenderBuffers,
    /// `InitializeRichText`, `ReinitializeRichTextFonts`, `FontDeviceReset`:
    /// the font system and the rich text's faces for the surface.
    RichText,
    /// `MouseSetEnabled`: the mouse model for the surface.
    Input,
    /// `ParticleFx->CreateVertexBuffer`.
    ParticleVertexBuffer,
    /// `LoadClientIniData`'s `fonts` and `rich_fonts` keys.
    Fonts,
    /// `Win3DStartupManager`: the UI manager and its camera, the cursors
    /// of `mouse.ini`, the scroll-bar shapes of `UITextures.ini`.
    Ui,
    /// `EnsureClientIMEFocus`: the edit-box focus manager.
    ImeFocus,
    /// `InitializeRichText` again and `DeviceTRC::set_image_root`.
    ImageRoot,
}

/// Construction or teardown.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Phase {
    Create,
    Teardown,
}

/// One entry of the lifecycle log.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct LifeEvent {
    pub stage: Stage,
    pub phase: Phase,
}

/// The engine's lifecycle log, shared so that it can be read after the
/// engine is dropped.
#[derive(Clone, Debug, Default)]
pub struct Lifecycle(Rc<RefCell<Vec<LifeEvent>>>);

impl Lifecycle {
    /// An empty log.
    #[must_use]
    pub fn new() -> Lifecycle {
        Lifecycle::default()
    }

    /// Appends a step.
    pub fn record(&self, stage: Stage, phase: Phase) {
        self.0.borrow_mut().push(LifeEvent { stage, phase });
    }

    /// Every step so far.
    #[must_use]
    pub fn events(&self) -> Vec<LifeEvent> {
        self.0.borrow().clone()
    }

    /// The stages of one phase, in order.
    #[must_use]
    pub fn stages(&self, phase: Phase) -> Vec<Stage> {
        self.0
            .borrow()
            .iter()
            .filter(|e| e.phase == phase)
            .map(|e| e.stage)
            .collect()
    }
}

/// The construction order: `NeuronRuntime::Startup`, then
/// `InitializeEngine`.
pub const CREATION_ORDER: [Stage; 29] = [
    Stage::Config,
    Stage::VMeshLibrary,
    Stage::RenderDevice,
    Stage::SoundManager,
    Stage::SoundStreamer,
    Stage::TextureLibrary,
    Stage::MaterialAnimation,
    Stage::MaterialBatcher,
    Stage::ParticleFx,
    Stage::SoundDevice,
    Stage::SceneEngine,
    Stage::Animation,
    Stage::Hardpoint,
    Stage::Deformable,
    Stage::RenderManager,
    Stage::Appearance,
    Stage::VMeshWire,
    Stage::EngineInitialize,
    Stage::Effects,
    Stage::SceneScripts,
    Stage::Data,
    Stage::RenderBuffers,
    Stage::RichText,
    Stage::Input,
    Stage::ParticleVertexBuffer,
    Stage::Fonts,
    Stage::Ui,
    Stage::ImeFocus,
    Stage::ImageRoot,
];

/// The teardown order: `ClientAtExitTeardown` up to
/// `NeuronRuntime::Shutdown`, its body, then the rest of
/// `ClientAtExitTeardown`. Stages whose state needs no release
/// ([`Stage::EngineInitialize`], [`Stage::ImageRoot`]) have no teardown
/// step. Where the reference releases a reference without freeing anything
/// (the libraries the system container keeps), the port frees them in
/// the order that leaves no references behind (divergence D451).
pub const TEARDOWN_ORDER: [Stage; 27] = [
    Stage::Ui,
    Stage::Fonts,
    Stage::RichText,
    Stage::RenderBuffers,
    Stage::Data,
    Stage::Effects,
    Stage::SceneScripts,
    Stage::SceneEngine,
    Stage::Animation,
    Stage::Hardpoint,
    Stage::Deformable,
    Stage::RenderManager,
    Stage::Appearance,
    Stage::VMeshWire,
    Stage::VMeshLibrary,
    Stage::MaterialBatcher,
    Stage::MaterialAnimation,
    Stage::TextureLibrary,
    Stage::ParticleFx,
    Stage::ParticleVertexBuffer,
    Stage::SoundStreamer,
    Stage::SoundDevice,
    Stage::SoundManager,
    Stage::RenderDevice,
    Stage::Config,
    Stage::ImeFocus,
    Stage::Input,
];

/// Where the reference makes a step: a C++ source file under `CppSource/`,
/// the function, and a call that occurs once in its body. Steps of one
/// function happen in the order of their calls; steps that share a call
/// happen inside it, in this module's order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Reference {
    pub file: &'static str,
    pub function: &'static str,
    pub call: &'static str,
}

const RUNTIME: &str = "NeuronRuntime/NeuronRuntime.cpp";
const ENGINE_INIT: &str = "OutpostFrontier/Client/engine_init.cpp";
const MAIN: &str = "OutpostFrontier/Client/main.cpp";
const STARTUP: &str = "bool __cdecl NeuronRuntime::Startup(";
const SHUTDOWN: &str = "void __cdecl NeuronRuntime::Shutdown(";
const INITIALIZE: &str = "bool InitializeEngine(";
const CREATE_BUFFERS: &str = "bool RenderMgrCreateBuffers(";
const AT_EXIT: &str = "void __cdecl ClientAtExitTeardown(";

const fn at(file: &'static str, function: &'static str, call: &'static str) -> Option<Reference> {
    Some(Reference {
        file,
        function,
        call,
    })
}

impl Stage {
    /// The `[System]` or `[Engine]` line that creates the stage, if any.
    #[must_use]
    pub fn component_names(self) -> &'static [&'static str] {
        match self {
            Stage::VMeshLibrary => &["VMeshLibrary"],
            Stage::RenderDevice => &["DirectX8"],
            Stage::SoundManager => &["SoundManager"],
            Stage::SoundStreamer => &["SoundStreamer"],
            Stage::TextureLibrary => &["TextureLibrary"],
            Stage::MaterialAnimation => &["MaterialAnimation"],
            Stage::MaterialBatcher => &["MaterialBatcher"],
            Stage::ParticleFx => &["ParticleFx", "FxRuntime"],
            Stage::Animation => &["Animation"],
            Stage::Hardpoint => &["Hardpoint"],
            Stage::Deformable => &["Deformable"],
            Stage::RenderManager => &["RenderManager"],
            Stage::Appearance => &["FLAppearance"],
            Stage::VMeshWire => &["VMeshWire"],
            _ => &[],
        }
    }

    /// Where the reference makes the step in `phase`; `None` for a step
    /// with no call of its own (the port's scene-script registry, the
    /// mouse model's release, the engine's initialise pass inside
    /// `LoadEngineComponents`).
    #[must_use]
    pub fn reference(self, phase: Phase) -> Option<Reference> {
        match phase {
            Phase::Create => self.created_at(),
            Phase::Teardown => self.torn_down_at(),
        }
    }

    fn created_at(self) -> Option<Reference> {
        match self {
            Stage::Config => at(RUNTIME, STARTUP, "SetINIConfig"),
            Stage::VMeshLibrary
            | Stage::RenderDevice
            | Stage::SoundManager
            | Stage::SoundStreamer
            | Stage::TextureLibrary
            | Stage::MaterialAnimation
            | Stage::MaterialBatcher
            | Stage::ParticleFx => at(RUNTIME, STARTUP, "LoadSystemComponents"),
            Stage::SoundDevice => at(RUNTIME, STARTUP, "StartupDevice"),
            Stage::SceneEngine
            | Stage::Animation
            | Stage::Hardpoint
            | Stage::Deformable
            | Stage::RenderManager
            | Stage::Appearance
            | Stage::VMeshWire
            | Stage::EngineInitialize => at(RUNTIME, STARTUP, "LoadEngineComponents"),
            Stage::Effects => at(RUNTIME, STARTUP, "BindParticleFx"),
            Stage::SceneScripts => None,
            Stage::Data => at(ENGINE_INIT, INITIALIZE, "NeuronRuntime::OpenData"),
            Stage::RenderBuffers => at(ENGINE_INIT, CREATE_BUFFERS, "Pipeline->CreateBuffers"),
            Stage::RichText => at(ENGINE_INIT, CREATE_BUFFERS, "InitializeRichText"),
            Stage::Input => at(ENGINE_INIT, CREATE_BUFFERS, "MouseSetEnabled"),
            Stage::ParticleVertexBuffer => at(ENGINE_INIT, INITIALIZE, "CreateVertexBuffer"),
            Stage::Fonts => at(ENGINE_INIT, INITIALIZE, "LoadClientIniData"),
            Stage::Ui => at(ENGINE_INIT, INITIALIZE, "Win3DStartupManager"),
            Stage::ImeFocus => at(ENGINE_INIT, INITIALIZE, "EnsureClientIMEFocus"),
            Stage::ImageRoot => at(ENGINE_INIT, INITIALIZE, "DeviceTRC::set_image_root"),
        }
    }

    fn torn_down_at(self) -> Option<Reference> {
        match self {
            Stage::Ui => at(MAIN, AT_EXIT, "Win3DShutdownManager"),
            Stage::Fonts => at(MAIN, AT_EXIT, "FontShutdown"),
            Stage::RichText => at(MAIN, AT_EXIT, "ShutdownRichText"),
            Stage::RenderBuffers => at(MAIN, AT_EXIT, "ShutdownRenderMgr(false)"),
            Stage::Data => at(MAIN, AT_EXIT, "NeuronRuntime::CloseData"),
            Stage::ImeFocus => at(MAIN, AT_EXIT, "ReleaseClientIMEFocus"),
            Stage::Effects => at(RUNTIME, SHUTDOWN, "ClearEffects"),
            Stage::SceneScripts
            | Stage::SceneEngine
            | Stage::Animation
            | Stage::Hardpoint
            | Stage::Deformable
            | Stage::RenderManager
            | Stage::Appearance
            | Stage::VMeshWire => at(RUNTIME, SHUTDOWN, "ReleaseGlobal(Engine);"),
            // The reference releases these references in the order texture
            // library, batcher, material library and never frees the two
            // libraries it does not release at all (D451); the port frees
            // the meshes and materials before the textures they hold.
            Stage::VMeshLibrary | Stage::MaterialBatcher | Stage::MaterialAnimation => {
                at(RUNTIME, SHUTDOWN, "ReleaseGlobal(MaterialLib);")
            }
            Stage::TextureLibrary => at(RUNTIME, SHUTDOWN, "ReleaseGlobal(TextureLib);"),
            Stage::ParticleFx => at(RUNTIME, SHUTDOWN, "ClearNodeLibrary"),
            Stage::ParticleVertexBuffer => at(RUNTIME, SHUTDOWN, "ReleaseVertexBuffer"),
            Stage::SoundStreamer => at(RUNTIME, SHUTDOWN, "ReleaseGlobal(Streamer);"),
            Stage::SoundDevice | Stage::SoundManager => at(RUNTIME, SHUTDOWN, "ShutdownDevice"),
            Stage::RenderDevice => at(RUNTIME, SHUTDOWN, "ShutdownPipeline"),
            Stage::Config => at(RUNTIME, SHUTDOWN, "NeuronComponents->Shutdown"),
            Stage::Input | Stage::EngineInitialize | Stage::ImageRoot => None,
        }
    }
}

/// The `[Libraries]` list of the client's engine INI (design/facts.md F1)
/// and the Rust module that holds each library under static composition.
pub const LIBRARIES: [(&str, &str); 14] = [
    ("ReadFile.dll", "neuron_core::vfs, neuron_core::utf"),
    ("x86math.dll", "neuron_core::math"),
    ("SceneRuntime.dll", "neuron_core::scene"),
    ("system.dll", "neuron_client::engine"),
    ("DX8RENDERER.dll", "neuron_client::render::ff"),
    ("SoundStreamer.dll", "neuron_client::audio::stream"),
    ("SoundManager.dll", "neuron_client::audio"),
    ("Deformable2.dll", "neuron_client::render::deformable"),
    ("SceneScript.dll", "neuron_client::scene_script"),
    ("Shading.dll", "neuron_client::render::material"),
    ("RendComp.dll", "neuron_client::render::mesh"),
    ("particlefx.dll", "neuron_client::render::particles"),
    ("ximage.dll", "neuron_client::render::texture"),
    (
        ".\\surfacematerials.dll",
        "neuron_client::render::material::surface, neuron_client::render::appearance",
    ),
];

/// The names of a `[System]` or `[Engine]` section as the reference reads
/// them: every line, white space skipped at its start, lines starting
/// with `;` or empty skipped, the name cut at `=`, a space or a tab.
#[must_use]
pub fn component_lines(section: Option<&neuron_core::ini::EngineSection>) -> Vec<Vec<u8>> {
    let Some(section) = section else {
        return Vec::new();
    };
    (0..section.entries.len())
        .filter_map(|i| section.line(i))
        .filter_map(|line| {
            let start = line.iter().position(|&c| c != b' ' && c != b'\t')?;
            let name = &line[start..];
            if name.first() == Some(&b';') {
                return None;
            }
            let end = name
                .iter()
                .position(|&c| matches!(c, b'=' | b' ' | b'\t'))
                .unwrap_or(name.len());
            (end > 0).then(|| name[..end].to_vec())
        })
        .collect()
}

/// The stages a `[System]` or `[Engine]` list creates, in its order, with
/// consecutive repeats (`ParticleFx`, `FxRuntime`) merged; `Err` with the
/// first name the port has no stage for.
///
/// # Errors
///
/// A name no stage answers to.
pub fn stages_of(names: &[Vec<u8>]) -> Result<Vec<Stage>, Vec<u8>> {
    let mut out: Vec<Stage> = Vec::new();
    for name in names {
        let stage = CREATION_ORDER
            .iter()
            .copied()
            .find(|s| {
                s.component_names()
                    .iter()
                    .any(|n| neuron_core::hash::eq_ignore_case(n.as_bytes(), name))
            })
            .ok_or_else(|| name.clone())?;
        if out.last() != Some(&stage) {
            out.push(stage);
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_stage_is_created_once_and_torn_down_at_most_once() {
        let mut created = CREATION_ORDER.to_vec();
        created.sort();
        created.dedup();
        assert_eq!(created.len(), CREATION_ORDER.len());
        let mut torn = TEARDOWN_ORDER.to_vec();
        torn.sort();
        torn.dedup();
        assert_eq!(torn.len(), TEARDOWN_ORDER.len());
        for stage in TEARDOWN_ORDER {
            assert!(CREATION_ORDER.contains(&stage), "{stage:?}");
        }
        for stage in CREATION_ORDER {
            let torn = TEARDOWN_ORDER.contains(&stage);
            let none = matches!(stage, Stage::EngineInitialize | Stage::ImageRoot);
            assert_eq!(torn, !none, "{stage:?}");
        }
    }

    #[test]
    fn component_lines_follow_the_reference_reader() {
        let mut ini = neuron_core::ini::EngineIni::new();
        ini.load_buffer(
            b"[System]\nVMeshLibrary\nDirectX8 ; comment\n;Skipped\nFxRuntime = extra\n",
            &mut neuron_core::ini::NoIncludes,
        )
        .unwrap();
        let names = component_lines(ini.section(b"System"));
        assert_eq!(
            names,
            vec![
                b"VMeshLibrary".to_vec(),
                b"DirectX8".to_vec(),
                b"FxRuntime".to_vec()
            ]
        );
        assert_eq!(
            stages_of(&names),
            Ok(vec![
                Stage::VMeshLibrary,
                Stage::RenderDevice,
                Stage::ParticleFx
            ])
        );
        assert_eq!(stages_of(&[b"Nothing".to_vec()]), Err(b"Nothing".to_vec()));
    }
}
