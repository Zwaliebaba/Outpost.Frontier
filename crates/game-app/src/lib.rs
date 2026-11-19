//! Outpost:Frontier client startup and game-specific presentation.
#![forbid(unsafe_code)]

use std::io::{self, Write};

pub mod account_keys;
pub mod cameras;
pub mod login_history;
#[cfg(windows)]
pub mod movies;
pub mod profile;
pub mod startup_config;
pub mod table_layouts;
pub mod timing;

/// Start the client application: off Windows, only the banner.
#[cfg(not(windows))]
pub fn run() -> io::Result<()> {
    writeln!(
        io::stdout().lock(),
        "Outpost:Frontier client (Neuron {})",
        neuron_client::ENGINE_VERSION
    )
}

/// Start the client application: the profile (imported from the C++
/// client's registry on the first run), the window, the engine
/// (`neuron_client::engine`, work package P4.16: the renderer on the first
/// hardware adapter or WARP, the audio device, the fonts and the UI
/// manager, built from `NeuronComponents.ini` and `OutpostFrontier.ini` in
/// the working directory and the data path), the startup movies, and the
/// engine's frame on the client clock until the window closes. The close
/// command quits at once until the exit dialog exists (P5).
#[cfg(windows)]
pub fn run() -> io::Result<()> {
    use neuron_client::engine::{EngineConfig, FrameInput, windows::create_engine};
    use neuron_client::platform::window::{Pump, Window, pump};
    use neuron_client::platform::{dpapi::Dpapi, known_folder, registry};
    use neuron_client::user_state::{LEGACY_REGISTRY_KEY, UserPaths};
    use neuron_client::window::{DisplayMode, WindowEvent};
    use neuron_core::time::InstantClock;

    let failure = |what: String| io::Error::other(what);
    let mut out = io::stdout().lock();
    writeln!(
        out,
        "Outpost:Frontier client (Neuron {})",
        neuron_client::ENGINE_VERSION
    )?;

    let documents = known_folder::documents()
        .ok_or_else(|| failure("the Documents folder could not be found".into()))?;
    let paths = UserPaths::from_documents(&documents);
    let (profile, origin) = profile::Profile::load_or_import(
        &paths,
        || registry::read_current_user(LEGACY_REGISTRY_KEY),
        &Dpapi,
    )
    .map_err(|e| failure(e.to_string()))?;
    writeln!(
        out,
        "profile: {origin:?} from {} ({} saved logins)",
        paths.root().display(),
        profile.logins.records.len()
    )?;

    // `LoadStartupConfig` and the display switches; the mode block that
    // `PerfOptions.ini` fills in the C++ is not read yet.
    let args: Vec<Vec<u8>> = std::env::args_os()
        .skip(1)
        .map(|a| a.to_string_lossy().into_owned().into_bytes())
        .collect();
    let args: Vec<&[u8]> = args.iter().map(Vec::as_slice).collect();
    let ini = std::fs::read(String::from_utf8_lossy(startup_config::INI_NAME).as_ref()).ok();
    let config = startup_config::StartupConfig::load(ini.clone(), &args);
    let mut display = config.display;
    startup_config::apply_display_overrides(&mut display, &args);
    let mode = DisplayMode {
        width: display.width,
        height: display.height,
        fullscreen: display.fullscreen,
    };
    let mut window = Window::new("Outpost:Frontier", mode).map_err(|e| failure(e.to_string()))?;

    // InitializeEngine: NeuronRuntime::Startup with NEURONCOMPONENTS.INI
    // from the working directory, then the data path.
    let mut engine_config = EngineConfig::new(
        std::path::PathBuf::from(String::from_utf8_lossy(&config.data_path).replace('\\', "/")),
        mode,
    );
    engine_config.engine_ini = std::fs::read("NeuronComponents.ini").ok();
    engine_config.client_ini = ini;
    engine_config.audio_device = true;
    let mut engine = create_engine(engine_config, None, Some(window.hwnd()))
        .map_err(|e| failure(e.to_string()))?;
    for notice in &engine.notices {
        writeln!(out, "engine: {notice}")?;
    }

    if movies::play(
        &config,
        &mut window,
        &mut engine.device(),
        engine.audio_device(),
    ) == movies::MoviesEnd::Quit
    {
        writeln!(out, "quit during the startup movies")?;
        return Ok(());
    }

    let mut clock = timing::ClientTiming::new(InstantClock::new());
    let mut camera = cameras::StillCamera::new(mode.back_buffer());
    let mut window = Some(window);
    loop {
        clock.update(true, 0, || {
            std::thread::sleep(std::time::Duration::from_millis(1))
        });
        let events = match window.as_mut().map_or_else(pump, Window::pump) {
            Pump::Quit(code, _) => {
                writeln!(out, "quit ({code})")?;
                return Ok(());
            }
            Pump::Events(events) => events,
        };
        if events.contains(&WindowEvent::CloseRequested) {
            if let Some(w) = window.take() {
                w.destroy();
            }
            continue;
        }
        let input = FrameInput {
            delta_seconds: clock.frame_time_capped(),
            frame_ms: clock.frame_timestamp_ms(),
            mouse: None,
            ui_input: Vec::new(),
        };
        engine.frame(input, &mut camera);
        if let Some(e) = engine.device().backend.errors.first() {
            return Err(failure(format!("renderer: {e}")));
        }
    }
}
