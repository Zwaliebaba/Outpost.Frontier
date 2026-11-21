//! The model viewer of Gate G4 (design/migration-plan.md, Phase 4): a tool
//! over neuron-client's engine (`neuron_client::engine`, work package
//! P4.16) that shows ships, stations, a skinned character and particle
//! effects from the original data, plays sounds, a voice line and a scene
//! script, and draws a front-end page descriptor over the scene, with an
//! orbiting perspective camera (`engine::camera::ViewCamera`).
//!
//! In a window (Windows) it runs on the first hardware adapter or WARP
//! with the audio device open; keys and the mouse switch scenes and orbit
//! (`--help`). With `--headless` it runs each scene for a fixed number of
//! frames, writes a BMP screenshot per scene and `report.txt`
//! (design/g4-runbook.md); off Windows the headless mode runs on the
//! recording backend and reports draw packets instead of images. Without
//! a data root only the synthetic scene runs, a model made in memory
//! (`model_viewer/synthetic.rs`).
//!
//! Everything a run writes is made from the game data: keep it outside the
//! repository (AGENTS.md).

#[path = "model_viewer/args.rs"]
mod args;
#[path = "model_viewer/catalog.rs"]
mod catalog;
#[path = "model_viewer/headless.rs"]
mod headless;
#[path = "model_viewer/scenes.rs"]
mod scenes;
#[path = "model_viewer/synthetic.rs"]
mod synthetic;
#[path = "model_viewer/viewer.rs"]
mod viewer;
#[cfg(windows)]
#[path = "model_viewer/window.rs"]
mod window;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use neuron_client::engine::EngineConfig;
use neuron_client::window::DisplayMode;

use args::Args;

/// The engine's configuration for `args`: the data root (an empty
/// directory of its own when none is given, for the synthetic scene) and
/// the two INI files beside it.
fn engine_config(args: &Args) -> Result<EngineConfig, String> {
    let mode = DisplayMode {
        width: args.width,
        height: args.height,
        fullscreen: false,
    };
    let data = match &args.data {
        Some(d) => d.clone(),
        None => {
            let dir =
                std::env::temp_dir().join(format!("outpost-model-viewer-{}", std::process::id()));
            std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
            dir
        }
    };
    let mut config = EngineConfig::new(&data, mode);
    if let Some(ini) = args.ini.clone().or_else(|| {
        args.data
            .as_ref()
            .and_then(|d| d.parent().map(Path::to_path_buf))
    }) {
        config.engine_ini = std::fs::read(ini.join("NeuronComponents.ini")).ok();
        config.client_ini = std::fs::read(ini.join("OutpostFrontier.ini")).ok();
    }
    config.audio_device = args.audio;
    Ok(config)
}

fn header(args: &Args, backend: &str) -> String {
    format!(
        "Outpost:Frontier model viewer (Neuron {}), {backend}, {}x{}, data {}",
        neuron_client::ENGINE_VERSION,
        args.width,
        args.height,
        args.data.as_ref().map_or_else(
            || "none (synthetic scene only)".to_owned(),
            |d| d.display().to_string()
        )
    )
}

fn out_dir(args: &Args) -> PathBuf {
    args.out
        .clone()
        .unwrap_or_else(|| PathBuf::from("viewer-out"))
}

/// The scenes a run can show: those needing data are dropped without it.
fn runnable(args: &Args) -> Vec<scenes::Scene> {
    args.scenes
        .iter()
        .filter(|s| args.data.is_some() || !s.needs_data())
        .cloned()
        .collect()
}

#[cfg(windows)]
fn run(args: &Args) -> Result<(), String> {
    use neuron_client::engine::windows::create_engine;
    use neuron_client::platform::d3d12::AdapterChoice;
    let scenes = runnable(args);
    if scenes.is_empty() {
        return Err("no scene to show (the fixed scenes need --data)".to_owned());
    }
    let config = engine_config(args)?;
    let adapter = match args.adapter.as_deref() {
        Some("hardware") => Some(AdapterChoice::Hardware),
        Some(_) => Some(AdapterChoice::Warp),
        None if args.headless => Some(AdapterChoice::Warp),
        None => None,
    };
    if !args.headless {
        return window::run(config, adapter, &scenes, &args.catalogs);
    }
    let backend = match adapter {
        Some(AdapterChoice::Hardware) => "Direct3D 12 on the hardware adapter",
        _ => "Direct3D 12 on WARP",
    };
    let engine = create_engine(config, adapter, None).map_err(|e| e.to_string())?;
    let mut viewer = viewer::Viewer::new(engine);
    let catalogs = viewer.load_catalogs(&args.catalogs);
    let out = out_dir(args);
    let (_, runs) = headless::run(
        &mut viewer,
        &scenes,
        args.frames,
        &out,
        &format!(
            "{}
{catalogs}",
            header(args, backend)
        ),
    )
    .map_err(|e| format!("{}: {e}", out.display()))?;
    println!("report: {}", out.join("report.txt").display());
    finish(&runs)
}

#[cfg(not(windows))]
fn run(args: &Args) -> Result<(), String> {
    use neuron_client::render::ff::device::RecordingBackend;
    if !args.headless {
        println!("the window needs Windows: running headless on the recording backend");
    }
    let scenes = runnable(args);
    if scenes.is_empty() {
        return Err("no scene to show (the fixed scenes need --data)".to_owned());
    }
    let config = engine_config(args)?;
    let engine = neuron_client::engine::Engine::new(config, RecordingBackend::default())
        .map_err(|e| e.to_string())?;
    let mut viewer = viewer::Viewer::new(engine);
    let catalogs = viewer.load_catalogs(&args.catalogs);
    let out = out_dir(args);
    let header = format!(
        "{}
{catalogs}",
        header(args, "the recording backend (no images)")
    );
    let (_, runs) = headless::run(&mut viewer, &scenes, args.frames, &out, &header)
        .map_err(|e| format!("{}: {e}", out.display()))?;
    println!("report: {}", out.join("report.txt").display());
    finish(&runs)
}

/// Fails the run when a scene failed.
fn finish(runs: &[headless::SceneRun]) -> Result<(), String> {
    let failed: Vec<&str> = runs
        .iter()
        .filter(|r| r.result.is_err())
        .map(|r| r.label.as_str())
        .collect();
    if failed.is_empty() {
        Ok(())
    } else {
        Err(format!("scenes failed: {}", failed.join(", ")))
    }
}

fn main() -> ExitCode {
    let args = match args::parse(std::env::args().skip(1)) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::FAILURE;
        }
    };
    if args.help {
        println!("{}", args::USAGE);
        return ExitCode::SUCCESS;
    }
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_client::engine::Engine;
    use neuron_client::render::ff::device::RecordingBackend;
    use neuron_client::render::particles::PoolKind;
    use neuron_client::text::font::fake::FakeBackend;

    fn small_args() -> Args {
        args::parse(["--size", "160x120"].map(str::to_owned)).expect("arguments")
    }

    /// The synthetic scene on the recording backend: the model loads from
    /// its in-memory archive, finds its material and draws every frame.
    #[test]
    fn the_synthetic_scene_draws_on_the_recording_backend() {
        let args = small_args();
        let mut config = engine_config(&args).expect("a configuration");
        config.font_backend = Some(Box::new(FakeBackend::default()));
        let engine = Engine::new(config, RecordingBackend::default()).expect("the engine");
        let mut viewer = viewer::Viewer::new(engine);
        let run = headless::run_scene(&mut viewer, &scenes::Scene::Synthetic, 3, None);
        assert_eq!(run.result, Ok(()), "{:?}", run.notes);
        assert_eq!(run.last.models, 1);
        let draws = run.draws.expect("the recording backend counts draws");
        assert!(
            draws >= 3,
            "one draw a frame at least: {draws}; {:?}",
            run.notes
        );
        assert!(run.image.is_none(), "the recording backend keeps no image");
        viewer.clear();
        assert_eq!(viewer.engine.scene().borrow().instances().count(), 0);
    }

    /// The synthetic scene on WARP: the frame read back holds a picture,
    /// the cube lit over the black clear.
    #[cfg(windows)]
    #[test]
    fn the_synthetic_scene_renders_on_warp() {
        use neuron_client::engine::windows::{WindowsEngineError, create_engine};
        use neuron_client::platform::d3d12::AdapterChoice;
        let args = small_args();
        let mut config = engine_config(&args).expect("a configuration");
        config.font_backend = Some(Box::new(FakeBackend::default()));
        let engine = match create_engine(config, Some(AdapterChoice::Warp), None) {
            Ok(e) => e,
            Err(WindowsEngineError::Gpu(e)) if std::env::var_os("GITHUB_ACTIONS").is_none() => {
                println!("no WARP device here ({e}); nothing to test");
                return;
            }
            Err(e) => panic!("{e}"),
        };
        let mut viewer = viewer::Viewer::new(engine);
        let run = headless::run_scene(&mut viewer, &scenes::Scene::Synthetic, 3, None);
        assert_eq!(run.result, Ok(()), "{:?}", run.notes);
        assert_eq!(run.errors, 0, "renderer errors");
        let (stats, _) = run.image.clone().expect("WARP reads the frame back");
        println!("{}", headless::summary(&run));
        assert_eq!((stats.width, stats.height), (160, 120));
        assert!(stats.non_trivial(), "the cube shows: {stats:?}");
        assert!(
            stats.lit < 160 * 120,
            "the cube does not cover the whole frame: {stats:?}"
        );
    }

    /// The fixed scenes over the original data (`outdir/`, decision O15) on
    /// the recording backend: every one loads, and those that draw do.
    /// Nothing is written.
    #[test]
    fn the_fixed_scenes_run_over_the_original_data() {
        let base = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
        if !base.join("data/SHIPS").is_dir() {
            println!("outdir/data is missing: nothing to run");
            return;
        }
        let data = base.join("data");
        let args = args::parse(
            [
                "--size",
                "320x240",
                "--data",
                data.to_str().expect("a UTF-8 path"),
                "--scenes",
                "all",
            ]
            .map(str::to_owned),
        )
        .expect("arguments");
        let mut config = engine_config(&args).expect("a configuration");
        assert!(config.engine_ini.is_some() && config.client_ini.is_some());
        config.font_backend = Some(Box::new(FakeBackend::default()));
        let engine = Engine::new(config, RecordingBackend::default()).expect("the engine");
        let mut viewer = viewer::Viewer::new(engine);
        let mut lines = Vec::new();
        for scene in &args.scenes {
            // The explosion throws its first debris on its fourth frame.
            let frames = if matches!(scene, scenes::Scene::Effect { .. }) {
                8
            } else {
                2
            };
            let run = headless::run_scene(&mut viewer, scene, frames, None);
            lines.push(headless::summary(&run));
            assert_eq!(run.result, Ok(()), "{}: {:?}", run.label, run.notes);
            let r = run.last;
            let draws = run.draws.unwrap_or(0);
            match scene {
                scenes::Scene::Ship(_) | scenes::Scene::Solar(_) | scenes::Scene::Synthetic => {
                    assert_eq!(r.models, 1, "{}", run.label);
                    assert!(draws > 0, "{} draws", run.label);
                }
                scenes::Scene::Character { .. } => {
                    assert_eq!((r.deformables, r.animations), (1, 1), "{}", run.label);
                    assert!(draws > 0, "{} draws", run.label);
                }
                scenes::Scene::Effect { .. } => {
                    assert_eq!(r.effects, 1, "{}", run.label);
                    assert!(draws > 0, "{} draws", run.label);
                    // The debris carry their child effects.
                    let fx = &viewer.engine.particles;
                    let records = fx.pools.get(PoolKind::Particle).used();
                    let live: u32 = records
                        .iter()
                        .filter_map(|&i| fx.pools.record(i).life.as_ref())
                        .flat_map(|life| life.records())
                        .filter_map(|r| fx.node(r.node).and_then(|n| n.appearance()))
                        .map(|a| a.live)
                        .sum();
                    assert!(
                        !records.is_empty() && live > 0,
                        "{}: {} particle records, {live} child particles",
                        run.label,
                        records.len()
                    );
                }
                scenes::Scene::Script(_) => assert_eq!(r.scripts, 1, "{}", run.label),
                scenes::Scene::Sound(_) | scenes::Scene::Voice { .. } => {
                    // The sound plays through the client's playing list:
                    // ranked by the frames' sound tick and given a voice of
                    // the mixer.
                    assert!(r.sounds >= 1, "{}: {:?}", run.label, run.notes);
                    let mixer = &viewer.engine.mixer;
                    assert!(!mixer.instances().is_empty(), "{}", run.label);
                    assert!(mixer.instances().iter().all(|i| i.started), "{}", run.label);
                }
                scenes::Scene::Page(_) => {
                    assert!(r.ui_nodes > 0, "{}", run.label);
                    assert!(r.ui_draws > 0, "{} draws its widgets", run.label);
                }
                _ => {}
            }
        }
        viewer.clear();
        assert_eq!(viewer.engine.scene().borrow().instances().count(), 0);
        println!("{}", lines.join("\n"));
    }

    /// The `menu` scene over the original data on the recording backend:
    /// `page_charselectsingle.page`'s widgets draw through the engine's
    /// interface pass. The nine rich-text windows and three text buttons
    /// hand back their surfaces as big-image tiles, the scroll bar draws
    /// its slider shapes through the immediate-mode layer with their
    /// texture, the list boxes draw their frames, and the arrow cursor is
    /// drawn last; without the page the pass draws nothing.
    #[test]
    fn the_menu_scene_draws_its_widgets() {
        use neuron_client::render::ff::device::IndexData;
        let base = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
        if !base.join("data/INTERFACE").is_dir() {
            println!("outdir/data is missing: nothing to run");
            return;
        }
        let data = base.join("data");
        let args = args::parse(
            [
                "--size",
                "1024x768",
                "--data",
                data.to_str().expect("a UTF-8 path"),
                "--scenes",
                "menu",
            ]
            .map(str::to_owned),
        )
        .expect("arguments");
        let mut config = engine_config(&args).expect("a configuration");
        config.font_backend = Some(Box::new(FakeBackend::default()));
        let engine = Engine::new(config, RecordingBackend::default()).expect("the engine");
        let mut viewer = viewer::Viewer::new(engine);
        let run = headless::run_scene(&mut viewer, &args.scenes[0], 2, None);
        println!("{}", headless::summary(&run));
        assert_eq!(run.result, Ok(()), "{:?}", run.notes);
        let r = run.last;
        assert_eq!(r.ui_tiles, 12, "nine rich-text windows and three buttons");
        assert!(r.cursor, "the arrow cursor");
        let (immediate, textured_immediate, quads) = {
            let device = viewer.engine.device();
            let draws = &device.backend.draws;
            let frame = &draws[draws.len() - r.ui_draws..];
            let immediate: Vec<_> = frame.iter().filter(|d| d.fvf.0 == 0x142).collect();
            let textured = immediate.iter().filter(|d| d.textures[0].is_some()).count();
            let quads = frame
                .iter()
                .filter(|d| d.fvf.0 == 0x1C4 && d.textures[0].is_some())
                .filter(|d| matches!(d.indices, IndexData::Buffer { .. }))
                .count();
            (immediate.len(), textured, quads)
        };
        println!(
            "interface pass: {} draws, {immediate} immediate ({textured_immediate} textured), \
             {quads} textured screen quads",
            r.ui_draws
        );
        assert!(immediate >= 3, "the list boxes' frames and the scroll bar");
        assert!(textured_immediate > 0, "the scroll bar's slider shapes");
        assert!(quads >= 13, "the tiles and the cursor");
        viewer.clear();
        let after = viewer.frame(neuron_client::engine::FrameInput::idle(headless::STEP));
        assert_eq!(
            (after.ui_draws, after.ui_tiles, after.cursor),
            (0, 0, false),
            "nothing is drawn without the page"
        );
    }
}
