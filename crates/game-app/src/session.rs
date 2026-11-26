//! The client's seam to the server (decision O53, record 0009 point 3).
//!
//! `game-app` never depends on `game-logic`: the executable implements
//! [`Session`] over the in-process server in single player (`ServerHost`
//! and the loopback, record 0002) and over the client's own world in
//! multiplayer, where the server is remote and [`Session::update`] and
//! [`Session::elapse_time`] have nothing to run.
//!
//! The frame order is rpclocal's (`rpclocal/rpclocal.cpp:536-550`):
//! `G_Server->Update()` first delivers what the server sent in the previous
//! frame to the client, then hands the client's messages to the server;
//! `G_Server->ElapseTime(dt)` then runs the server's frame, which queues the
//! next frame's messages. [`tick_local_server`] keeps that order, with the
//! client's delivery given by the caller.

use game_sim::object::World;

/// The server as the client's frame sees it (`G_Server`, an `IServer`).
pub trait Session {
    /// The world the client's own passes run over: the one shared world in
    /// single player (record 0002), the client's own in multiplayer.
    fn world(&mut self) -> &mut World;

    /// The server's pump (`IServer::Update`, slot 11, after the client's
    /// delivery): the messages the client sent reach the server.
    fn update(&mut self);

    /// The server's frame (`IServer::ElapseTime`, slot 12).
    fn elapse_time(&mut self, dt: f32);

    /// `SinglePlayer()`: the server runs in this process.
    fn single_player(&self) -> bool;
}

/// `TickLocalServer` (`server_connect.cpp:155-162`): nothing without a
/// server or while the single-player server tick is paused
/// (`ShouldSkipLocalServerTick`); otherwise the client's delivery of the
/// server's messages (`deliver`, rpclocal's client-bound drain), the
/// server's pump and the server's frame, in that order.
pub fn tick_local_server<S: Session + ?Sized>(
    session: Option<&mut S>,
    skip: bool,
    dt: f32,
    deliver: impl FnOnce(&mut S),
) {
    let Some(session) = session else {
        return;
    };
    if skip {
        return;
    }
    deliver(session);
    session.update();
    session.elapse_time(dt);
}

/// What one modal frame does with the server (`NnErrorRunModal`'s loop,
/// `nn_error.cpp:221-252`): the pump when the dialog asked for it, never the
/// server's frame (record 0009 point 5).
pub fn tick_modal_server<S: Session + ?Sized>(
    session: Option<&mut S>,
    update_server: bool,
    deliver: impl FnOnce(&mut S),
) {
    let Some(session) = session else {
        return;
    };
    if !update_server {
        return;
    }
    deliver(session);
    session.update();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Recording {
        calls: Vec<String>,
    }

    impl Session for Recording {
        fn world(&mut self) -> &mut World {
            unreachable!("the frame order needs no world")
        }
        fn update(&mut self) {
            self.calls.push("update".into());
        }
        fn elapse_time(&mut self, dt: f32) {
            self.calls.push(format!("elapse {dt}"));
        }
        fn single_player(&self) -> bool {
            true
        }
    }

    #[test]
    fn the_local_tick_delivers_then_pumps_then_elapses() {
        let mut s = Recording::default();
        tick_local_server(Some(&mut s), false, 0.5, |s| s.calls.push("deliver".into()));
        assert_eq!(s.calls, ["deliver", "update", "elapse 0.5"]);
    }

    #[test]
    fn a_paused_tick_or_no_server_does_nothing() {
        let mut s = Recording::default();
        tick_local_server(Some(&mut s), true, 0.5, |s| s.calls.push("deliver".into()));
        assert!(s.calls.is_empty());
        tick_local_server::<Recording>(None, false, 0.5, |_| unreachable!());
    }

    #[test]
    fn a_modal_frame_pumps_only_when_asked_and_never_elapses() {
        let mut s = Recording::default();
        tick_modal_server(Some(&mut s), false, |s| s.calls.push("deliver".into()));
        assert!(s.calls.is_empty());
        tick_modal_server(Some(&mut s), true, |s| s.calls.push("deliver".into()));
        assert_eq!(s.calls, ["deliver", "update"]);
    }
}

/// What the client asks the executable for when it starts the in-process
/// server (`StartServer` in single player): the executable owns
/// `game-logic`, so it makes the server (record 0002 point 4, record 0009
/// point 3).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LocalServerRequest {
    /// The client's folder, with `OutpostFrontier.ini`.
    pub install_dir: std::path::PathBuf,
    /// The user folder (`Documents\My Games\OutpostFrontier`).
    pub user_data: std::path::PathBuf,
    /// The CRT generator's seed for `IServerImpl::Startup`.
    pub seed: u32,
    /// The oracle mode is on: the server's frame takes the fixed clock
    /// (decision O53 (3)).
    pub oracle: bool,
}

/// The single-player transport's client end.
pub type LocalClientEnd = neuron_core::net::transport::LoopbackTransport<
    game_sim::protocol::ToServer,
    game_sim::protocol::ToClient,
>;

/// How the executable starts the in-process server: the session and the
/// client's end of its loopback, `None` when it does not start.
pub type StartLocalServer<'a> =
    &'a mut dyn FnMut(&LocalServerRequest) -> Option<(Box<dyn Session>, LocalClientEnd)>;
