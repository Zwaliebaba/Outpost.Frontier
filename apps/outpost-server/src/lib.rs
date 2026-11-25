//! `OutpostServer`, the headless dedicated server (work package P3.9,
//! decision O9), in place of the MFC admin application
//! (`CppSource/OutpostServer`): the configuration ([`config`]), the log
//! set-up, the start-up of the game ([`game_logic::startup`]), the server's
//! certificate and QUIC host, the command console ([`console`]) and the
//! frame loop of `ServerThreadProc` (`OutpostServer.cpp:337-410`).
#![forbid(unsafe_code)]

pub mod config;
pub mod console;

use std::cell::RefCell;
use std::io::BufRead;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::mpsc::TryRecvError;
use std::time::Instant;

use game_logic::admin::{self, HostControl};
use game_logic::host::ServerHost;
use game_logic::startup::{StartupInfo, startup};
use game_sim::protocol::{GAME_BUILD, MultiplayerToClient, MultiplayerToServer};
use neuron_server::account::AccountsRoot;
use neuron_server::admin_files::{MOTD_FILE, read_motd};
use neuron_server::log::{
    ErrorLog, LogFilter, NOTICE_TARGET, ServerLog, ServerLogSubscriber, ServerVersion,
};
use neuron_server::machine::{ClockSeed, PortableMachine};
use neuron_server::net::{HostConfig, HostSettings, HostTiming, QuicServer, ServerIdentity};
use neuron_server::tick::{FramePacer, LoadHistory};

use crate::config::{CONFIG_FILE, ServerConfig};
use crate::console::Input;

/// The server's QUIC host.
pub type Host = QuicServer<MultiplayerToClient, MultiplayerToServer>;

/// The error log's file name (`c:\OutpostServerErrors.log` in the C++).
pub const ERROR_LOG_FILE: &str = "OutpostServerErrors.log";

/// The command line: `OutpostServer [--config <file>]`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Options {
    /// The configuration file; [`CONFIG_FILE`] in the working folder by
    /// default.
    pub config: PathBuf,
}

/// Parses the arguments after the program name.
///
/// # Errors
///
/// The usage text for `--help`, an unknown argument or a missing value.
pub fn parse_args(args: impl IntoIterator<Item = String>) -> Result<Options, String> {
    let usage = format!("usage: OutpostServer [--config <file>]  (default {CONFIG_FILE})");
    let mut options = Options {
        config: PathBuf::from(CONFIG_FILE),
    };
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--config" | "-c" => {
                options.config = PathBuf::from(args.next().ok_or_else(|| usage.clone())?);
            }
            _ => return Err(usage),
        }
    }
    Ok(options)
}

fn notice(text: &str) {
    tracing::trace!(target: NOTICE_TARGET, "{text}");
}

/// The host's side of the admin commands.
struct Control<'a> {
    transport: &'a Rc<RefCell<Host>>,
    started: Instant,
    load: &'a LoadHistory,
}

impl HostControl for Control<'_> {
    fn close(&mut self, client: u32) -> bool {
        use neuron_core::net::transport::{ConnectionId, DisconnectReason, Transport};
        self.transport
            .borrow_mut()
            .disconnect(ConnectionId::new(client), DisconnectReason::Kicked)
            .is_ok()
    }

    fn connection_count(&self) -> usize {
        self.transport.borrow().connection_count()
    }

    fn peer(&self, client: u32) -> Option<String> {
        self.transport
            .borrow()
            .peer(neuron_core::net::transport::ConnectionId::new(client))
            .map(|p| p.address.to_string())
    }

    fn status_lines(&self) -> Vec<String> {
        let host = self.transport.borrow();
        let settings = host.settings();
        vec![
            format!("Server: {}", settings.name),
            format!("Address: {}", host.local_address()),
            format!("Certificate: {}", host.fingerprint()),
            format!(
                "Password: {}",
                if settings.password.is_empty() {
                    "none"
                } else {
                    "set"
                }
            ),
            format!("Load: {} msec", self.load.average()),
            format!("Uptime: {} s", self.started.elapsed().as_secs()),
        ]
    }
}

/// Starts the server with `options` and runs it until the console says
/// `quit`.
///
/// # Errors
///
/// The configuration, the start-up, the certificate or the host failed.
pub fn run(
    options: &Options,
    input: impl BufRead + Send + 'static,
) -> Result<(), Box<dyn std::error::Error>> {
    let (config, imported) = config::load_or_create(&options.config, |c| {
        AccountsRoot::new(&c.user_data_dir(), true)
            .path()
            .to_path_buf()
    })?;
    let user_data = config.user_data_dir();
    let accounts = AccountsRoot::new(&user_data, true);
    install_log(&config, &accounts)?;
    if imported {
        notice(&format!(
            "Imported {} into {}",
            neuron_server::admin_files::LEGACY_CONFIG_FILE,
            options.config.display()
        ));
    }
    notice("Loading initial data...");
    let info = StartupInfo {
        install_dir: config.install_dir(),
        user_data,
        max_players: usize::from(config.max_players),
        allow_new_players: config.allow_new_players,
        allow_pvp: config.allow_pvp,
        seed: clock_seed(),
    };
    let mut machine = PortableMachine::default();
    let started = startup(&info, &mut machine, &mut ClockSeed::default())?;
    let identity = ServerIdentity::load_or_generate(accounts.path())?;
    let settings = HostSettings {
        max_players: config.max_players,
        password: config.password.clone(),
        name: config.name.clone(),
        description: config.description.clone(),
        pvp: config.allow_pvp,
        new_players: config.allow_new_players,
        server_id: started.account.id,
        server_signature: started.account.keys.signature.clone().unwrap_or_default(),
    };
    let transport = QuicServer::bind(HostConfig {
        bind: SocketAddr::new(config.address, config.port),
        identity,
        build: GAME_BUILD,
        settings,
        timing: HostTiming::default(),
    })?;
    let transport = Rc::new(RefCell::new(transport));
    let mut host = ServerHost::new(
        started.world,
        started.logic,
        started.files,
        Rc::clone(&transport),
    );
    host.logic.chat_hooks = admin::console_chat_hooks();
    notice("Starting logic thread...");
    // ServerThreadProc: motd.dat, then ApplyServerNews.
    if let Ok(bytes) = std::fs::read(accounts.local_user_path(MOTD_FILE)) {
        let text = read_motd(&bytes);
        let mut cx = host.cx();
        game_logic::session::set_motd(&mut cx, &text);
    }
    {
        let t = transport.borrow();
        notice(&format!("Listening on {}", t.local_address()));
        notice(&format!("Certificate fingerprint {}", t.fingerprint()));
    }
    game_logic::host::drain_reports(&mut host.logic.reports);
    notice("Server running, ready for log in.");
    let commands = console::spawn(input)?;
    serve(&mut host, &transport, &commands);
    shutdown(&mut host);
    notice("Server stopped.");
    Ok(())
}

/// `srand(timeGetTime())`: the clock's milliseconds.
fn clock_seed() -> u32 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| (d.as_millis() & 0xFFFF_FFFF) as u32)
}

/// The log of `ServerLogf` as the global `tracing` subscriber, to the
/// standard output.
fn install_log(config: &ServerConfig, accounts: &AccountsRoot) -> std::io::Result<()> {
    let mut filter = LogFilter::default();
    filter.set_level(config.log_level);
    for &category in &config.log_categories {
        filter.add_category(category);
    }
    let mut log = ServerLog::new(Box::new(std::io::stdout()), filter);
    if let Some(file) = &config.log_file {
        log.open_file(file)?;
    }
    let errors = config
        .error_log
        .clone()
        .unwrap_or_else(|| accounts.local_user_path(ERROR_LOG_FILE));
    let version = ServerVersion {
        major: (GAME_BUILD >> 24) as u8,
        minor: (GAME_BUILD >> 16) as u8,
        build: GAME_BUILD as u16,
    };
    log.set_error_log(ErrorLog::new(errors, version));
    // A second server in one process (the tests) keeps the first log.
    let _ = tracing::subscriber::set_global_default(ServerLogSubscriber::new(log));
    Ok(())
}

/// The server thread's loop: pace, run the console's commands, one frame.
fn serve(
    host: &mut ServerHost<Host>,
    transport: &Rc<RefCell<Host>>,
    commands: &std::sync::mpsc::Receiver<Input>,
) {
    let started = Instant::now();
    let mut pacer = FramePacer::new(Instant::now());
    let mut last_frame = Instant::now();
    let mut load = LoadHistory::default();
    let mut console_open = true;
    loop {
        let frame = pacer.begin(Instant::now());
        if !frame.sleep.is_zero() {
            std::thread::sleep(frame.sleep);
        }
        let frame_start = Instant::now();
        while console_open {
            let input = match commands.try_recv() {
                Ok(input) => input,
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    console_open = false;
                    break;
                }
            };
            match input {
                Input::Quit => return,
                Input::Help => println!("{}", console::HELP),
                Input::Invalid(reason) => println!("{reason}"),
                Input::Command(command) => {
                    let mut control = Control {
                        transport,
                        started,
                        load: &load,
                    };
                    let mut cx = host.cx();
                    for line in admin::execute(&mut cx, &mut control, &command) {
                        println!("{line}");
                    }
                    game_logic::host::drain_reports(&mut host.logic.reports);
                }
            }
        }
        let now = Instant::now();
        let real_dt = (now - last_frame).as_secs_f64();
        last_frame = now;
        host.frame(frame.elapsed as f32, real_dt);
        load.push(neuron_core::num::trunc_to_i32(
            frame_start.elapsed().as_secs_f64() * 1000.0,
        ));
    }
}

/// `IServerImpl::Shutdown`'s `PlayerDB::logout_all`: every session in use
/// logs out (autosaving its character), then the reports are logged.
fn shutdown(host: &mut ServerHost<Host>) {
    let sessions: Vec<u32> = host.logic.players.active_ids().collect();
    let mut cx = host.cx();
    for session in sessions {
        game_logic::login::logout(&mut cx, session);
    }
    neuron_core::net::transport::Transport::flush(&mut *host.transport().borrow_mut());
    game_logic::host::drain_reports(&mut host.logic.reports);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arguments() {
        assert_eq!(
            parse_args(Vec::new()).unwrap().config,
            PathBuf::from(CONFIG_FILE)
        );
        assert_eq!(
            parse_args(["--config".to_owned(), "a.toml".to_owned()])
                .unwrap()
                .config,
            PathBuf::from("a.toml")
        );
        assert!(parse_args(["--config".to_owned()]).is_err());
        assert!(parse_args(["/C".to_owned()]).is_err());
    }
}
