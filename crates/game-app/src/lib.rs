//! Outpost:Frontier client startup and game-specific presentation.
#![forbid(unsafe_code)]

use std::io::{self, Write};

pub mod account_keys;
pub mod base;
pub mod cameras;
pub mod client_state;
pub mod frame;
pub mod input_router;
pub mod keymap;
pub mod login;
pub mod login_history;
pub mod machine_warning;
#[cfg(windows)]
pub mod movies;
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
/// clock until the window closes: each frame pumps the messages through
/// GameApp's input router, then runs the engine's frame with the window's
/// mouse. The close command quits at once until the exit dialog exists
/// (P5).
///
/// With `+oracle <script> <out>` the oracle mode runs (decision O53 (2)):
/// the display is forced, the in-process server starts on the oracle's
/// clock, the single-player login and the `IDR_ALWAYS_PRESENT` group's key
/// map load at frame 0 as the C++ intro and `WinMain` do, and the script runs
/// each frame on the fixed step until it ends; its files are written then.
/// The screens are not ported yet: their trees, screenshots and inputs
/// answer "not ported yet".
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

    // EnterClientState(ComputeInitialEnvironment()): the intro starts the
    // single-player server and logs in (MenuNavStartSinglePlayer's
    // ConnectAndLogin and RequestCharacterInfo, P5.3).
    let request = session::LocalServerRequest {
        install_dir: std::env::current_dir().unwrap_or_default(),
        user_data: paths.root().to_path_buf(),
        seed: oracle::script::RAND_SEED,
        oracle: oracle.is_some(),
    };
    let mut server: Option<Box<dyn session::Session>> = None;
    let mut link = None;
    if let Some((mut s, client)) = start_server(&request) {
        let started = std::time::Instant::now();
        let mut now = || started.elapsed().as_secs_f64();
        let mut l =
            receiver::ClientLink::new(client, receiver::ClientReceiver::new(oracle.is_some()));
        // IServerImpl::Startup ends with g_pClientInterface->Startup(1, 0x36d9).
        l.receiver.startup(1, 0x36d9);
        let settings = login::LoginSettings::single_player(1.0);
        let connected = login::connect(&mut l, Some(s.as_mut()), &mut now) == 1;
        let logged_in =
            connected && login::login_to_server(&mut l, Some(s.as_mut()), &settings, &mut now);
        if logged_in {
            login::request_character_info(&mut l, Some(s.as_mut()), &mut now);
        }
        writeln!(
            out,
            "single player: connected {connected}, logged in {logged_in} (status {})",
            l.receiver.login.login_status
        )?;
        server = Some(s);
        link = Some(l);
    } else {
        writeln!(out, "the in-process server did not start")?;
    }

    // RegisterAcceleratorGroup("IDR_ALWAYS_PRESENT"), which loads the key map.
    let mut keymap = keymap::KeyMap::new();
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
        let frame = oracle.as_ref().map_or(0, |r| r.script.frame());
        let lines = &mut oracle.as_mut().map(|r| &mut r.keymap);
        let mut note = |m: &keymap::KeyMap, phase: &str, result: i32| {
            if let Some(lines) = lines.as_mut() {
                lines.extend(oracle::keymap::phase_lines(m, frame, phase, result));
            }
        };
        if keymap.register_group(b"IDR_ALWAYS_PRESENT", &mut files, &mut note)
            == keymap::GroupResult::Invalid
        {
            writeln!(out, "Invalid accelerator: IDR_ALWAYS_PRESENT")?;
        }
    }

    let mut clock = timing::ClientTiming::new(InstantClock::new());
    let mut camera = cameras::StillCamera::new(mode.back_buffer());
    let mut window = Some(window);
    let mut pending_state = None;
    loop {
        clock.update(true, 0, || {
            std::thread::sleep(std::time::Duration::from_millis(1))
        });
        // The oracle's step: 1/30 s paced in real time, reported exactly.
        let mut dt = clock.frame_time_capped();
        if let Some(run) = oracle.as_mut() {
            std::thread::sleep(std::time::Duration::from_secs_f64(
                oracle::script::FRAME_SECONDS,
            ));
            dt = oracle::script::FRAME_SECONDS;
            // TickClientFrame's OracleFrameTick, then the local server.
            run.script.frame_tick(&mut oracle::ClientHost {
                ui: engine.ui_mut(),
                pending_state: &mut pending_state,
            });
            if let Some(log) = link.as_mut().and_then(|l| l.receiver.log.as_mut()) {
                log.set_frame(run.script.frame());
            }
        }
        if let (Some(s), Some(l)) = (server.as_mut(), link.as_mut()) {
            #[allow(clippy::cast_possible_truncation)]
            let dt32 = dt as f32;
            session::tick_local_server(Some(s.as_mut()), false, dt32, |s| {
                l.deliver_all(Some(s.world()));
            });
        }
        if pending_state == Some(1) {
            // EnterClientState(1): the server shuts down (its Shutdown calls
            // the client's) and the client quits.
            if let Some(l) = link.as_mut() {
                l.receiver.shutdown();
            }
            if let Some(run) = oracle.as_ref() {
                let iclient = link
                    .as_ref()
                    .and_then(|l| l.receiver.log.as_ref())
                    .map(|log| log.lines().to_vec())
                    .unwrap_or_default();
                run.write(&iclient)?;
                writeln!(out, "oracle: {} written", run.out.display())?;
            }
            return Ok(());
        }
        let input = FrameInput {
            delta_seconds: dt,
            frame_ms: clock.frame_timestamp_ms(),
            mouse: None,
            ui_input: Vec::new(),
        };
        let Some(w) = window.as_mut() else {
            // The window is gone: the queue still has to see WM_QUIT.
            if let Pump::Quit(code, _) = pump() {
                writeln!(out, "quit ({code})")?;
                return Ok(());
            }
            engine.frame(input, &mut camera);
            continue;
        };
        // PumpMessages through GameApp's router.
        let (cursor, wheel) = w
            .with_mouse(|m, _| {
                let (x, y, _) = m.cursor();
                ((x, y), m.wheel())
            })
            .unwrap_or_default();
        let mut ime = w.ime();
        let pumped = {
            let mut router = input_router::GameRouter::new(&mut engine, &mut ime, cursor, wheel);
            let pumped = w.pump_routed(&mut router);
            let events = match &pumped {
                Pump::Events(events) | Pump::Quit(_, events) => events,
            };
            for event in events {
                router.window_event(event);
            }
            pumped
        };
        let events = match pumped {
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
        engine.frame_with_mouse(input, &mut camera, w);
        if let Some(e) = engine.device().backend.errors.first() {
            return Err(failure(format!("renderer: {e}")));
        }
    }
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
