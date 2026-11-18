//! Outpost:Frontier client startup and game-specific presentation.
#![forbid(unsafe_code)]

use std::io::{self, Write};

pub mod account_keys;
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
/// client's registry on the first run), the window, the renderer on the
/// first hardware adapter (WARP when there is none), and the frame loop on
/// the client clock until the window closes. The close command quits at
/// once until the exit dialog exists (P5).
#[cfg(windows)]
pub fn run() -> io::Result<()> {
    use neuron_client::platform::d3d12::AdapterChoice;
    use neuron_client::platform::d3d12::backend::D3d12Backend;
    use neuron_client::platform::window::{Pump, Window, pump};
    use neuron_client::platform::{dpapi::Dpapi, known_folder, registry};
    use neuron_client::render::ff::defaults::Defaults;
    use neuron_client::render::ff::device::{ClearFlags, Device};
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
    let config = startup_config::StartupConfig::load(ini, &args);
    let mut display = config.display;
    startup_config::apply_display_overrides(&mut display, &args);
    let mode = DisplayMode {
        width: display.width,
        height: display.height,
        fullscreen: display.fullscreen,
    };
    let window = Window::new("Outpost:Frontier", mode).map_err(|e| failure(e.to_string()))?;
    let (width, height) = mode.back_buffer();
    let backend = D3d12Backend::new(AdapterChoice::Hardware, width, height)
        .or_else(|_| D3d12Backend::new(AdapterChoice::Warp, width, height))
        .map_err(|e| failure(e.to_string()))?;
    let mut device = Device::new(Defaults::builtin(), width, height, backend);
    device
        .backend
        .attach_window(window.hwnd())
        .map_err(|e| failure(e.to_string()))?;

    let audio = neuron_client::platform::audio::AudioDevice::new(
        neuron_client::audio::config::stream_buffer_seconds(0.0),
    )
    .ok();
    let mut window = window;
    if movies::play(&config, &mut window, &mut device, audio.as_ref()) == movies::MoviesEnd::Quit {
        writeln!(out, "quit during the startup movies")?;
        return Ok(());
    }

    let mut clock = timing::ClientTiming::new(InstantClock::new());
    let mut window = Some(window);
    loop {
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
        clock.update(true, 0, || {
            std::thread::sleep(std::time::Duration::from_millis(1))
        });
        device.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        device.present();
        if let Some(e) = device.backend.errors.first() {
            return Err(failure(format!("renderer: {e}")));
        }
    }
}
