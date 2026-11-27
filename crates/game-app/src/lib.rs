//! Outpost:Frontier client startup and game-specific presentation.
#![forbid(unsafe_code)]

use std::io::{self, Write};

pub mod account_keys;
pub mod base;
pub mod boot_interface;
pub mod cameras;
pub mod chat;
pub mod client;
pub mod client_state;
pub mod frame;
pub mod input_router;
pub mod intro;
pub mod keymap;
pub mod loading_screen;
pub mod login;
pub mod login_history;
pub mod machine_warning;
#[cfg(windows)]
pub mod movies;
pub mod nn_preferences;
pub mod objects;
pub mod oracle;
pub mod perf_options;
pub mod player_behavior;
pub mod preferences;
pub mod profile;
pub mod receiver;
pub mod render_mode;
pub mod session;
pub mod space_camera;
pub mod space_frame;
pub mod space_state;
pub mod star_system;
pub mod startup_config;
pub mod table_layouts;
pub mod timing;

/// Start the client application: off Windows, only the banner.
#[cfg(not(windows))]
pub fn run(_start_server: session::StartLocalServer<'_>) -> io::Result<()> {
    writeln!(
        io::stdout().lock(),
        "Outpost:Frontier client (Neuron {})",
        neuron_client::ENGINE_VERSION
    )
}

/// Start the client application: the profile (imported from the C++
/// client's registry on the first run), `PerfOptions.ini` with its display
/// mode and the low-machine warning, the window in the mode
/// `SelectInitialRenderMode` picks, the engine (`neuron_client::engine`,
/// work package P4.16: the renderer on the first hardware adapter or WARP,
/// the audio device, the fonts and the UI manager, built from
/// `NeuronComponents.ini` and `OutpostFrontier.ini` in the working
/// directory and the data path), `PerfOptions.ini` saved with the mode the
/// device took, the startup movies, and the engine's frame on the client
/// clock until the window closes. Then, as `WinMain` ends its start-up,
/// the client's context ([`client::ClientContext`]) is built over the
/// engine, client state 6 is entered (the intro, whose `Init` starts the
/// in-process server and logs in) and the `IDR_ALWAYS_PRESENT` group's key
/// map is registered. Each frame pumps the messages through GameApp's input
/// router, runs `TickClientFrame` ([`frame::tick_client_frame`]: the
/// oracle's tick, the interface update, the local server, the state's
/// update) and then the engine's frame with the window's mouse, which draws
/// and presents. The close command quits at once until the exit dialog
/// exists (P5).
///
/// With `+oracle <script> <out>` the oracle mode runs (decision O53 (2)):
/// the display is forced, the in-process server starts on the oracle's
/// clock, and the script runs at the start of each frame on the fixed step
/// until it ends; its files are written then. Screenshots, key-map
/// commands and keys answer "not ported yet".
#[cfg(windows)]
pub fn run(start_server: session::StartLocalServer<'_>) -> io::Result<()> {
    use neuron_client::engine::{EngineConfig, FrameInput, windows::create_engine};
    use neuron_client::platform::window::{Pump, Window, pump};
    use neuron_client::platform::{dpapi::Dpapi, known_folder, machine, registry};
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

    // `LoadStartupConfig` and the command line.
    let args: Vec<Vec<u8>> = std::env::args_os()
        .skip(1)
        .map(|a| a.to_string_lossy().into_owned().into_bytes())
        .collect();
    let args: Vec<&[u8]> = args.iter().map(Vec::as_slice).collect();
    // OracleParseCommandLine: a missing path or an unreadable script ends the
    // process with exit code 3.
    let tokens: Vec<String> = args
        .iter()
        .map(|a| String::from_utf8_lossy(a).into_owned())
        .collect();
    let mut oracle = match oracle::script::parse_command_line(&tokens) {
        None => None,
        Some(Err(())) => {
            eprintln!("OutpostFrontier +oracle: usage: +oracle <script> <output folder>");
            std::process::exit(3);
        }
        Some(Ok((script, out_dir))) => match oracle::OracleRun::open(&script, &out_dir) {
            Ok(run) => Some(run),
            Err(message) => {
                eprintln!("{message}");
                std::process::exit(3);
            }
        },
    };
    let ini = std::fs::read(String::from_utf8_lossy(startup_config::INI_NAME).as_ref()).ok();
    let config = startup_config::StartupConfig::load(ini.clone(), &args);

    // InitializeEngine's render mode: PerfOptions.ini's [Display], the
    // low-machine warning, the display switches, SelectInitialRenderMode.
    let perf_path = paths.file(perf_options::FILE_NAME);
    let mut perf = perf_options::PerfOptions::default();
    let mut block = render_mode::initial_mode();
    let mut display = render_mode::perf_display(&block);
    let applied = perf.load(std::fs::read(&perf_path).ok(), Some(&mut display));
    render_mode::set_perf_display(&mut block, &display);
    if !low_machine_warning(&mut perf) {
        writeln!(out, "the low-machine warning was declined")?;
        return Ok(());
    }
    startup_config::apply_display_overrides(&mut block, &args);
    if let Some(run) = oracle.as_mut() {
        run.force_display(&mut block);
    }
    let desktop = machine::desktop_size();
    let taken =
        render_mode::select_initial(&block, &render_mode::AdapterFacts::direct3d12(desktop))
            .ok_or_else(|| failure("no display mode could be made".into()))?;
    let mode = DisplayMode {
        width: taken.width,
        height: taken.height,
        fullscreen: taken.fullscreen,
    };
    let mut window = Window::new("Outpost:Frontier", mode).map_err(|e| failure(e.to_string()))?;
    // The callbacks the port has: the mouse speed (P4.11). The rest of the
    // options are the game's (P5).
    if let Some(applied) = &applied
        && let Some(index) = perf_options::PerfOptions::find(b"mouse_speed")
        && applied.contains(&index)
    {
        let speed = perf.options[index].applied;
        window.with_mouse(|m, _| m.apply_speed_option(speed));
    }

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
    // InitializeEngine's interface windows: the briefing's parent, the
    // group-request window, the tool tip and the rollover.
    if let Some(ui) = engine.ui_mut() {
        // The interface's strings: the text catalogs beside the executable
        // (decision T21), as the low-machine warning reads them.
        let dir = std::env::current_exe()
            .ok()
            .and_then(|exe| exe.parent().map(std::path::Path::to_path_buf))
            .unwrap_or_default();
        let (catalogs, missing) = neuron_core::catalog::TextCatalogs::load_client(&dir);
        if !missing.is_empty() {
            writeln!(out, "text catalogs: {} modules not loaded", missing.len())?;
        }
        ui.catalogs = Some(std::rc::Rc::new(catalogs));
        boot_interface::register(&mut ui.factory);
        boot_interface::create(ui);
    }
    // SavePerfOptions with the mode the device took.
    if let Err(e) = paths
        .create()
        .and_then(|()| std::fs::write(&perf_path, perf.save(&render_mode::perf_display(&taken))))
    {
        writeln!(out, "{}: {e}", perf_path.display())?;
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

    // OracleSeedRandom: the client's seed and the server's clock (O53 (3)).
    if let Some(run) = oracle.as_mut() {
        run.script.note_seed(oracle::script::RAND_SEED, 1000.0);
    }

    // The client's state (record 0009 point 1). The single-player server
    // starts when the intro connects (`StartServer`).
    let request = session::LocalServerRequest {
        install_dir: std::env::current_dir().unwrap_or_default(),
        user_data: paths.root().to_path_buf(),
        seed: oracle::script::RAND_SEED,
        oracle: oracle.is_some(),
    };
    let started = std::time::Instant::now();
    let mut cx = client::ClientContext::new(
        engine,
        timing::ClientTiming::new(InstantClock::new()),
        oracle,
        start_server,
        request,
        Box::new(move || started.elapsed().as_secs_f64()),
    );
    // What the preferences window reads that the start-up knows; the key
    // names, the device's modes and abilities are not given yet (D1111).
    cx.core.preferences.perf = perf.clone();
    cx.core.preferences.display = nn_preferences::DisplayMode {
        width: taken.width.cast_signed(),
        height: taken.height.cast_signed(),
        color_bpp: taken.color_bpp,
    };
    if let Some((w, h)) = desktop {
        cx.core.preferences.desktop = (w.cast_signed(), h.cast_signed());
    }

    // EnterClientState(ComputeInitialEnvironment()): the intro, whose Init
    // starts the single-player server and logs in (MenuNavStartSinglePlayer).
    let mut states = client::client_states();
    states.enter(&mut cx, client_state::ClientStateId::Intro.raw());
    if let Some(l) = cx.core.link.as_ref() {
        writeln!(
            out,
            "single player: logged in (status {})",
            l.receiver.login.login_status
        )?;
    } else {
        writeln!(out, "the single-player login failed")?;
    }

    // RegisterAcceleratorGroup("IDR_ALWAYS_PRESENT"), which loads the key map.
    {
        struct Files<'a> {
            data: &'a std::path::Path,
            user: std::path::PathBuf,
        }
        impl keymap::KeyMapFiles for Files<'_> {
            fn base(&mut self) -> (Option<Vec<u8>>, String) {
                let path = neuron_core::vfs::Host::new(self.data)
                    .locate(keymap::KEY_MAP_PATH.as_bytes())
                    .ok()
                    .flatten()
                    .map(|e| e.path);
                let shown = path.as_ref().map_or_else(
                    || keymap::KEY_MAP_PATH.to_string(),
                    |p| p.display().to_string(),
                );
                (path.and_then(|p| std::fs::read(p).ok()), shown)
            }
            fn user(&mut self) -> Option<Vec<u8>> {
                std::fs::read(&self.user).ok()
            }
        }
        let data =
            std::path::PathBuf::from(String::from_utf8_lossy(&config.data_path).replace('\\', "/"));
        let mut files = Files {
            data: &data,
            user: paths.file(keymap::USER_KEY_MAP_FILE),
        };
        let core = &mut cx.core;
        let frame = core.oracle.as_ref().map_or(0, |r| r.script.frame());
        let lines = &mut core.oracle.as_mut().map(|r| &mut r.keymap);
        let mut note = |m: &keymap::KeyMap, phase: &str, result: i32| {
            if let Some(lines) = lines.as_mut() {
                lines.extend(oracle::keymap::phase_lines(m, frame, phase, result));
            }
        };
        if core
            .keymap
            .register_group(b"IDR_ALWAYS_PRESENT", &mut files, &mut note)
            == keymap::GroupResult::Invalid
        {
            writeln!(out, "Invalid accelerator: IDR_ALWAYS_PRESENT")?;
        }
    }

    let mut camera = cameras::StillCamera::new(mode.back_buffer());
    let mut window = Some(window);
    let mut frame_state = frame::FrameState::default();
    loop {
        // UpdateClientTiming.
        cx.core.clock.update(true, 0, || {
            std::thread::sleep(std::time::Duration::from_millis(1))
        });
        // The oracle's step: 1/30 s paced in real time, reported exactly.
        let mut dt = cx.core.clock.frame_time_capped();
        if cx.core.oracle.is_some() {
            std::thread::sleep(std::time::Duration::from_secs_f64(
                oracle::script::FRAME_SECONDS,
            ));
            dt = oracle::script::FRAME_SECONDS;
        }
        let frame_ms = cx.core.clock.frame_timestamp_ms();
        // PumpMessages through GameApp's router.
        if let Some(w) = window.as_mut() {
            let (cursor, wheel) = w
                .with_mouse(|m, _| {
                    let (x, y, _) = m.cursor();
                    ((x, y), m.wheel())
                })
                .unwrap_or_default();
            let mut ime = w.ime();
            let pumped = {
                let mut router =
                    input_router::GameRouter::new(&mut cx.interface, &mut ime, cursor, wheel);
                let pumped = w.pump_routed(&mut router);
                let events = match &pumped {
                    Pump::Events(events) | Pump::Quit(_, events) => events,
                };
                for event in events {
                    router.window_event(event);
                }
                pumped
            };
            match pumped {
                Pump::Quit(code, _) => {
                    writeln!(out, "quit ({code})")?;
                    return finish(&mut cx.core, &mut out);
                }
                Pump::Events(events) => {
                    if events.contains(&WindowEvent::CloseRequested)
                        && let Some(w) = window.take()
                    {
                        w.destroy();
                    }
                }
            }
        } else if let Pump::Quit(code, _) = pump() {
            // The window is gone: the queue still has to see WM_QUIT.
            writeln!(out, "quit ({code})")?;
            return finish(&mut cx.core, &mut out);
        }

        // TickClientFrame: the oracle's tick, the interface update, the
        // local server and the state's update; the state's draw is the
        // engine's frame after it (its interface update already ran).
        frame::tick_client_frame(&mut frame_state, &mut states, &mut cx, dt);
        if cx.core.quit_posted {
            return finish(&mut cx.core, &mut out);
        }
        let input = FrameInput {
            delta_seconds: dt,
            frame_ms,
            mouse: None,
            ui_input: Vec::new(),
            ui_updated: true,
        };
        match window.as_mut() {
            Some(w) => {
                cx.interface.frame_with_mouse(input, &mut camera, w);
            }
            None => {
                cx.interface.frame(input, &mut camera);
            }
        }
        if let Some(e) = cx.interface.device().backend.errors.first() {
            return Err(failure(format!("renderer: {e}")));
        }
    }
}

/// The client's end: the server shut down (its `Shutdown` calls the
/// client's) and the oracle's files written.
#[cfg(windows)]
fn finish<C: neuron_core::time::Clock>(
    core: &mut client::ClientCore<'_, C>,
    out: &mut impl Write,
) -> io::Result<()> {
    core.shutdown_server_connection();
    if let Some(run) = core.oracle.as_ref() {
        run.write(&core.iclient_lines())?;
        writeln!(out, "oracle: {} written", run.out.display())?;
    }
    Ok(())
}

/// `BuildLowMachineWarning` and its dialog over this machine's memory (the
/// renderer has no verdict on the card, D534), with the strings of the text
/// catalogs beside the executable (decision T21). Whether the start-up
/// goes on.
#[cfg(windows)]
fn low_machine_warning(perf: &mut perf_options::PerfOptions) -> bool {
    use neuron_client::platform::{machine, window};
    use neuron_core::catalog::TextCatalogs;

    let memory = machine::memory_mib().map(|(total_physical, available_virtual)| {
        machine_warning::MemoryMib {
            total_physical,
            available_virtual,
        }
    });
    let lines = machine_warning::warning_lines(memory, perf.skip_machine_warnings, 0);
    if lines.is_empty() {
        return true;
    }
    let dir = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(std::path::Path::to_path_buf))
        .unwrap_or_default();
    let (catalogs, _) = TextCatalogs::load_client(&dir);
    let mut text = machine_warning::warning_text(&lines, &catalogs);
    if text.is_empty() {
        return true;
    }
    text.extend(machine_warning::text(
        &catalogs,
        machine_warning::IDS_QUESTION,
    ));
    let caption = machine_warning::text(&catalogs, machine_warning::IDS_CAPTION);
    let yes = window::ask_yes_no(None, &caption, &text);
    // The message box has no check box: the flag stays as it was (D534).
    let checked = !perf.skip_machine_warnings;
    machine_warning::answer(yes, checked, &mut perf.skip_machine_warnings)
}
