//! The in-process single-player server behind the client's [`Session`]
//! (work package P5.1, decision O53 (1), record 0009 point 3).
//!
//! `StartServer` (`OutpostFrontier/Client/server_connect.cpp`) loads
//! `rpclocal.dll` in single player and starts the server with one player;
//! here [`start_local_server`] runs `game-logic`'s single-player start-up and
//! puts its [`ServerHost`] on a loopback transport, whose other end the
//! client keeps. [`LocalSession`] gives the client's frame the shared world
//! (record 0002), the server's pump and the server's frame.

use std::cell::RefCell;
use std::rc::Rc;
use std::time::Instant;

use game_app::session::Session;
use game_logic::host::{ServerHost, drain_reports};
use game_logic::startup::{LocalStartupInfo, StartupError, startup_single_player};
use game_sim::object::World;
use game_sim::protocol::{ToClient, ToServer};
use neuron_core::net::transport::{LoopbackTransport, Transport, loopback};

/// The server's end of the loopback: it sends [`ToClient`].
pub type ServerEnd = LoopbackTransport<ToClient, ToServer>;

/// The client's end of the loopback: it sends [`ToServer`].
pub type ClientEnd = LoopbackTransport<ToServer, ToClient>;

/// Where the server's frame takes its wall-clock delta from
/// (`IServerImpl::ElapseTime`'s `g_TimingDelta.read_dt()`).
pub enum RealClock {
    /// The real clock, as the C++ reads it.
    Wall(Option<Instant>),
    /// The frame's own `dt`: the oracle's fixed clock (decision O53 (3)),
    /// under which the C++ adds the fixed frame instead.
    Fixed,
}

impl RealClock {
    fn delta(&mut self, dt: f32) -> f64 {
        match self {
            Self::Fixed => f64::from(dt),
            Self::Wall(last) => {
                let now = Instant::now();
                let delta = last.map_or(0.0, |l| now.duration_since(l).as_secs_f64());
                *last = Some(now);
                delta
            }
        }
    }
}

/// The client's single-player session: the in-process server and the world
/// both sides run over.
pub struct LocalSession {
    host: ServerHost<ServerEnd>,
    transport: Rc<RefCell<ServerEnd>>,
    clock: RealClock,
}

impl LocalSession {
    /// The server's state, for the executable's own uses (saves, reports).
    pub fn host(&mut self) -> &mut ServerHost<ServerEnd> {
        &mut self.host
    }
}

/// `StartServer` in single player: the server started from the client's INI
/// and data, on a loopback whose client end is returned beside it.
///
/// # Errors
///
/// [`StartupError`] when the INI or its data path is missing.
pub fn start_local_server(
    info: &LocalStartupInfo,
    clock: RealClock,
) -> Result<(LocalSession, ClientEnd), StartupError> {
    let started = startup_single_player(info)?;
    let (client, server) = loopback::<ToServer, ToClient>();
    let transport = Rc::new(RefCell::new(server));
    let host = ServerHost::new(
        started.world,
        started.logic,
        started.files,
        Rc::clone(&transport),
    );
    Ok((
        LocalSession {
            host,
            transport,
            clock,
        },
        client,
    ))
}

impl Session for LocalSession {
    fn world(&mut self) -> &mut World {
        &mut self.host.world
    }

    fn update(&mut self) {
        self.host.update();
    }

    fn elapse_time(&mut self, dt: f32) {
        let real_dt = self.clock.delta(dt);
        self.host.elapse_time(dt, real_dt);
        self.transport.borrow_mut().flush();
        drain_reports(&mut self.host.logic.reports);
    }

    fn single_player(&self) -> bool {
        !self.host.logic.multiplayer_server
    }
}
