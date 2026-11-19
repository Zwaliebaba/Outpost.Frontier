//! The engine's construction and teardown order (work package P4.16) on
//! the recording backend, checked three ways against the reference:
//!
//! - the lifecycle log equals the compiled tables
//!   ([`CREATION_ORDER`], [`TEARDOWN_ORDER`]);
//! - the tables' `[System]` and `[Engine]` parts equal the lists of the
//!   shipped `NeuronComponents.ini` (`outdir/`, decision O15), and the
//!   library table equals its `[Libraries]` list (design/facts.md F1);
//! - the calls that make each step occur in the C++ sources
//!   (`CppSource/NeuronRuntime/NeuronRuntime.cpp`,
//!   `CppSource/OutpostFrontier/Client/engine_init.cpp` and `main.cpp`) in
//!   the tables' order, with the one exception divergence D451 records.

use std::fs;
use std::path::{Path, PathBuf};

use neuron_client::engine::order::{component_lines, stages_of};
use neuron_client::engine::{
    CREATION_ORDER, Engine, EngineConfig, EngineError, FrameInput, LIBRARIES, Phase, Stage,
    TEARDOWN_ORDER,
};
use neuron_client::render::appearance::Camera;
use neuron_client::render::ff::device::RecordingBackend;
use neuron_client::text::font::fake::FakeBackend;
use neuron_client::window::DisplayMode;
use neuron_core::ini::{EngineIni, NoIncludes};

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("outpost-engine-{name}-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    dir
}

const MODE: DisplayMode = DisplayMode {
    width: 640,
    height: 480,
    fullscreen: false,
};

fn config(data: &Path, ini: Option<Vec<u8>>) -> EngineConfig {
    let mut config = EngineConfig::new(data, MODE);
    config.engine_ini = ini;
    config.font_backend = Some(Box::new(FakeBackend::default()));
    config
}

/// A camera at the origin looking down -z.
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

#[test]
fn construction_and_teardown_follow_the_compiled_order() {
    let data = scratch("order");
    let ini = fs::read(root().join("outdir/NeuronComponents.ini")).ok();
    let engine: Engine<RecordingBackend> =
        Engine::new(config(&data, ini), RecordingBackend::default()).expect("the engine");
    let lifecycle = engine.lifecycle();
    assert_eq!(lifecycle.stages(Phase::Create), CREATION_ORDER.to_vec());
    assert!(lifecycle.stages(Phase::Teardown).is_empty());
    drop(engine);
    assert_eq!(lifecycle.stages(Phase::Teardown), TEARDOWN_ORDER.to_vec());
    let events = lifecycle.events();
    assert_eq!(events.len(), CREATION_ORDER.len() + TEARDOWN_ORDER.len());
    assert!(
        events[..CREATION_ORDER.len()]
            .iter()
            .all(|e| e.phase == Phase::Create),
        "nothing is torn down while the engine is built"
    );
}

#[test]
fn a_missing_data_directory_stops_the_construction() {
    let data = scratch("missing").join("no-such-dir");
    let result: Result<Engine<RecordingBackend>, _> =
        Engine::new(config(&data, None), RecordingBackend::default());
    assert!(matches!(result, Err(EngineError::NoData(_))));
}

#[test]
fn an_empty_frame_clears_draws_the_interface_and_presents() {
    let data = scratch("frame");
    let mut engine: Engine<RecordingBackend> =
        Engine::new(config(&data, None), RecordingBackend::default()).expect("the engine");
    assert!(engine.notices.iter().any(|n| n.contains("no engine INI")));
    let report = engine.frame(FrameInput::idle(1.0 / 30.0), &mut Still);
    assert_eq!(report.models, 0);
    {
        let device = engine.device();
        assert_eq!(device.backend.presents, 1);
        // The scene's target and depth clear, then the interface's depth
        // clear.
        let flags: Vec<u32> = device.backend.clears.iter().map(|c| c.flags.0).collect();
        assert_eq!(flags, vec![3, 2]);
        assert_eq!(device.backend.clears[0].color, 0xFF00_0000);
    }
    engine.frame(FrameInput::idle(1.0 / 30.0), &mut Still);
    assert_eq!(engine.frames(), 2);
    assert_eq!(engine.device().backend.presents, 2);
}

fn shipped_ini() -> Option<EngineIni> {
    let bytes = fs::read(root().join("outdir/NeuronComponents.ini")).ok()?;
    let mut ini = EngineIni::new();
    ini.load_buffer(&bytes, &mut NoIncludes)
        .expect("the engine INI parses");
    Some(ini)
}

#[test]
fn the_shipped_lists_match_the_compiled_order() {
    let Some(ini) = shipped_ini() else {
        println!("outdir/NeuronComponents.ini is missing: nothing to compare");
        return;
    };
    let system = stages_of(&component_lines(ini.section(b"System"))).expect("known names");
    assert_eq!(system, CREATION_ORDER[1..9].to_vec(), "[System]");
    let engine = stages_of(&component_lines(ini.section(b"Engine"))).expect("known names");
    assert_eq!(engine, CREATION_ORDER[11..17].to_vec(), "[Engine]");
    // [Libraries]: the line before its tab and comment.
    let libraries: Vec<Vec<u8>> = component_lines(ini.section(b"Libraries"));
    let expected: Vec<Vec<u8>> = LIBRARIES
        .iter()
        .map(|(dll, _)| dll.as_bytes().to_vec())
        .collect();
    assert_eq!(libraries, expected, "[Libraries] (design/facts.md F1)");

    let data = scratch("lists");
    let bytes = fs::read(root().join("outdir/NeuronComponents.ini")).unwrap();
    let built: Engine<RecordingBackend> =
        Engine::new(config(&data, Some(bytes)), RecordingBackend::default()).expect("the engine");
    assert!(
        !built.notices.iter().any(|n| n.contains("decision T22")),
        "{:?}",
        built.notices
    );
}

/// The body of the function whose definition starts with `signature`: from
/// the signature to its matching closing brace.
fn body<'a>(source: &'a str, signature: &str) -> &'a str {
    let start = source
        .find(signature)
        .unwrap_or_else(|| panic!("{signature} not found"));
    let open = start + source[start..].find('{').expect("a body");
    let mut depth = 0usize;
    for (i, c) in source[open..].char_indices() {
        match c {
            '{' => depth += 1,
            '}' => {
                depth -= 1;
                if depth == 0 {
                    return &source[start..open + i];
                }
            }
            _ => {}
        }
    }
    panic!("{signature}: unbalanced");
}

fn position(source: &str, function: &str, call: &str) -> usize {
    body(source, function)
        .find(call)
        .unwrap_or_else(|| panic!("{call} not found in {function}"))
}

#[test]
fn the_reference_calls_come_in_the_compiled_order() {
    let cpp = root().join("CppSource");
    if !cpp.is_dir() {
        println!("CppSource is missing: nothing to compare");
        return;
    }
    let read = |file: &str| fs::read_to_string(cpp.join(file)).expect(file);
    for (phase, order) in [
        (Phase::Create, &CREATION_ORDER[..]),
        (Phase::Teardown, &TEARDOWN_ORDER[..]),
    ] {
        // The last position seen in each function.
        let mut last: Vec<(&str, &str, usize, Stage)> = Vec::new();
        for &stage in order {
            let Some(r) = stage.reference(phase) else {
                continue;
            };
            let at = position(&read(r.file), r.function, r.call);
            if let Some(entry) = last
                .iter_mut()
                .find(|(f, func, _, _)| *f == r.file && *func == r.function)
            {
                let allowed = phase == Phase::Teardown
                    && stage == Stage::TextureLibrary
                    && entry.3 == Stage::MaterialAnimation;
                assert!(
                    at >= entry.2 || allowed,
                    "{phase:?}: {stage:?} ({}) comes before {:?} in {}",
                    r.call,
                    entry.3,
                    r.function
                );
                if at >= entry.2 {
                    entry.2 = at;
                    entry.3 = stage;
                }
            } else {
                last.push((r.file, r.function, at, stage));
            }
        }
    }

    // D451: the reference releases the texture library before the
    // material library; the port frees the materials first.
    let runtime = read("NeuronRuntime/NeuronRuntime.cpp");
    let shutdown = "void __cdecl NeuronRuntime::Shutdown(";
    assert!(
        position(&runtime, shutdown, "ReleaseGlobal(TextureLib);")
            < position(&runtime, shutdown, "ReleaseGlobal(MaterialLib);")
    );

    // Across the functions: Startup runs at the start of InitializeEngine,
    // before the data; the render mode (RenderMgrCreateBuffers) between
    // the data and the particle vertex buffer; NeuronRuntime::Shutdown
    // between the data's close and the IME focus's release.
    let init = read("OutpostFrontier/Client/engine_init.cpp");
    let initialize = "bool InitializeEngine(";
    let startup = position(&init, initialize, "NeuronRuntime::Startup");
    let open_data = position(&init, initialize, "NeuronRuntime::OpenData");
    let mode = position(&init, initialize, "SelectInitialRenderMode");
    let vertex_buffer = position(&init, initialize, "CreateVertexBuffer");
    assert!(startup < open_data && open_data < mode && mode < vertex_buffer);
    let main = read("OutpostFrontier/Client/main.cpp");
    let at_exit = "void __cdecl ClientAtExitTeardown(";
    let close = position(&main, at_exit, "NeuronRuntime::CloseData");
    let shutdown_call = position(&main, at_exit, "NeuronRuntime::Shutdown");
    let ime = position(&main, at_exit, "ReleaseClientIMEFocus");
    assert!(close < shutdown_call && shutdown_call < ime);
    // The engine's components are created in one pass and initialised in
    // a second (SceneRuntime.cpp's LoadEngineComponents).
    let scene = read("SceneRuntime/SceneRuntime.cpp");
    let load = "static GENRESULT NEURONCOMPONENTS_API EngineLoadEngineComponents(";
    assert!(position(&scene, load, "CreateInstance") < position(&scene, load, "->Initialize()"));
}
