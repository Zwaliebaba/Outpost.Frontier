//! The dedicated server over a transport (work package P3.9): one frame
//! of `ServerThreadProc` (`OutpostServer/OutpostServer.cpp:370-403`),
//! `IServerImpl::Update` (the client pump, `CClientImpl::Update` and
//! `CDPServer::DispatchMsgs`) and `IServerImpl::ElapseTime`.
//!
//! [`ServerHost::update`] polls the transport until it is empty, as
//! `DispatchMsgs` drains its list: a new connection hears its client id
//! (`FLPACKET_SERVER_CONNECTRESPONSE`) and the message of the day
//! (`CRemoteClientProxy::OnConnect`, `IServerImpl::OnConnect`); a message
//! goes to the session entry points ([`crate::session::route`]), the chat
//! (`IChatServer::SubmitChat`) or [`crate::receive::deliver`]; a closed
//! connection is the game's disconnect (`CClientImpl::DisconnectClient`,
//! `IServerImpl::DisConnect`). [`ServerHost::elapse_time`] runs the
//! server frame ([`neuron_server::tick::elapse_time`]) and
//! [`ServerHost::frame`] adds the transport's flush and the reports.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;

use game_sim::object::World;
use game_sim::protocol::common::CommonMessage;
use game_sim::protocol::to_client::ConnectResponse;
use game_sim::protocol::{ToClient, ToServer};
use neuron_core::diag::{Category, DumpFlags, Severity};
use neuron_core::net::transport::{ConnectionId, DisconnectReason, Transport, TransportEvent};
use neuron_server::tick::{FrameHost, ServerClock};

use crate::login::ServerFiles;
use crate::server::{Cx, Logic, TransportChat, TransportSink};

impl FrameHost for Cx<'_> {
    fn clock(&mut self) -> &mut ServerClock {
        &mut self.logic.clock
    }

    fn physics_update(&mut self, dt: f32) {
        crate::world::physics_update(self, dt);
    }

    fn subsystem_update(&mut self, _dt: f32) {
        crate::bases::update(self);
    }
}

/// The flags word and line of a report as `FDUMP` would have printed it:
/// `"<source>(<line>) : <text>"`, or the text alone for a report without
/// a source (the `LOG:` lines); the severity is the text's first word
/// (`*** ` before it is skipped), a notice when it names none.
#[must_use]
pub fn report_line(report: &game_sim::data::LoadReport) -> (DumpFlags, String) {
    let text = report.text.trim_start_matches("*** ");
    let severity = [
        ("FATAL", Severity::Fatal),
        ("ERROR", Severity::Error),
        ("WARNING", Severity::Warning),
        ("NOTICE", Severity::Notice),
        ("TRACE", Severity::Trace1),
    ]
    .iter()
    .find(|(word, _)| text.starts_with(word))
    .map_or(Severity::Notice, |&(_, s)| s);
    let line = if report.source.is_empty() {
        report.text.clone()
    } else {
        format!("{}({}) : {}", report.source, report.line, report.text)
    };
    (DumpFlags::new(Category::GENERAL, severity), line)
}

/// Hands every report the server made since the last call to the log
/// ([`neuron_core::diag::dump`]).
pub fn drain_reports(reports: &mut game_sim::data::Reports) {
    for report in std::mem::take(&mut reports.0) {
        let (flags, line) = report_line(&report);
        neuron_core::diag::dump(flags, &line);
    }
}

/// What one [`ServerHost::update`] did.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Pumped {
    /// Connections opened.
    pub connected: u32,
    /// Messages handled.
    pub messages: u32,
    /// Connections closed.
    pub disconnected: u32,
}

/// The dedicated server: the world, the server's state, its files and the
/// transport its client and chat sinks write to.
pub struct ServerHost<T> {
    /// The world.
    pub world: World,
    /// The server's state.
    pub logic: Logic,
    /// The files outside the data tree.
    pub files: ServerFiles,
    transport: Rc<RefCell<T>>,
    /// Messages no entry point takes, by name, with their count: none on a
    /// dedicated server, where every message of both channels reaches its
    /// `IServerImpl` function (the single-player `SAVEGAME` is the caller's,
    /// [`crate::saves::server_save_game`]).
    pub unrouted: BTreeMap<&'static str, u32>,
}

impl<T> ServerHost<T>
where
    T: Transport + 'static,
    T::Outgoing: From<ToClient>,
    T::Incoming: Into<ToServer>,
{
    /// A host over `transport`: the server's client and chat sinks become
    /// the transport's ([`TransportSink`], [`TransportChat`]).
    #[must_use]
    pub fn new(
        world: World,
        mut logic: Logic,
        files: ServerFiles,
        transport: Rc<RefCell<T>>,
    ) -> Self {
        logic.client = Box::new(TransportSink::new(Rc::clone(&transport)));
        logic.chat = Box::new(TransportChat::new(Rc::clone(&transport)));
        Self {
            world,
            logic,
            files,
            transport,
            unrouted: BTreeMap::new(),
        }
    }

    /// The transport.
    #[must_use]
    pub const fn transport(&self) -> &Rc<RefCell<T>> {
        &self.transport
    }

    /// The context over the world and the server's state.
    pub fn cx(&mut self) -> Cx<'_> {
        Cx {
            world: &mut self.world,
            logic: &mut self.logic,
        }
    }

    /// `IServerImpl::Update`: every pending transport event, in order.
    pub fn update(&mut self) -> Pumped {
        let mut pumped = Pumped::default();
        loop {
            let Some(event) = self.transport.borrow_mut().poll() else {
                break;
            };
            match event {
                TransportEvent::Connected(connection) => {
                    pumped.connected += 1;
                    self.connected(connection.get());
                }
                TransportEvent::Message {
                    connection,
                    message,
                } => {
                    pumped.messages += 1;
                    self.message(connection.get(), &message.into());
                }
                TransportEvent::Disconnected { connection, .. } => {
                    pumped.disconnected += 1;
                    let mut cx = self.cx();
                    crate::session::disconnect(&mut cx, connection.get());
                }
            }
        }
        pumped
    }

    /// `CRemoteClientProxy::OnConnect`: the client id, then
    /// `IServerImpl::OnConnect` (the message of the day). An in-process
    /// single-player server hears only the client id: rpclocal's pump sends
    /// the CONNECTRESPONSE on its first update (`rpclocal/rpc_client_proxy.cpp`,
    /// `Update`), and nothing calls `Server.dll`'s `OnConnect`, which only
    /// `RemoteClient` does (`RemoteClient_transport.cpp:292`; rpclocal's own
    /// `IServerImpl::OnConnect` is empty, `rpclocal.cpp:601`). P5.1, record
    /// 0009.
    fn connected(&mut self, client: u32) {
        self.logic.client.send(
            client,
            ToClient::Server(ConnectResponse { value: client }.into()),
        );
        if self.logic.multiplayer_server {
            let mut cx = self.cx();
            crate::session::on_connect(&mut cx, client);
        }
    }

    /// `DispatchClientPacket` for one message of `client`.
    fn message(&mut self, client: u32, message: &ToServer) {
        let files = self.files.clone();
        let mut cx = Cx {
            world: &mut self.world,
            logic: &mut self.logic,
        };
        if let ToServer::Common(CommonMessage::ChatMsg(chat)) = message {
            crate::chat::submit_chat(&mut cx, client & 0xFFFF, &chat.data, chat.target_id, -1);
            return;
        }
        if crate::session::route(&mut cx, &files, client, message)
            || crate::session::route_common(&mut cx, client, message)
            || crate::receive::deliver(&mut cx, client, message)
        {
            return;
        }
        *self.unrouted.entry(message_name(message)).or_default() += 1;
    }

    /// `IServerImpl::ElapseTime(dt)` with the frame's wall-clock delta.
    pub fn elapse_time(&mut self, dt: f32, real_dt: f64) {
        let mut cx = self.cx();
        neuron_server::tick::elapse_time(&mut cx, dt, real_dt);
    }

    /// One frame of the server thread: [`Self::update`], then
    /// [`Self::elapse_time`], then the transport's flush, then the reports
    /// to the log.
    pub fn frame(&mut self, dt: f32, real_dt: f64) -> Pumped {
        let pumped = self.update();
        self.elapse_time(dt, real_dt);
        self.transport.borrow_mut().flush();
        drain_reports(&mut self.logic.reports);
        pumped
    }

    /// The admin's kick: the connection closes with
    /// [`DisconnectReason::Kicked`]; its `Disconnected` reaches the game at
    /// the next [`Self::update`]. False when no connection has the id.
    pub fn kick(&mut self, client: u32) -> bool {
        self.transport
            .borrow_mut()
            .disconnect(ConnectionId::new(client), DisconnectReason::Kicked)
            .is_ok()
    }
}

/// A message's packet name.
fn message_name(message: &ToServer) -> &'static str {
    match message {
        ToServer::Common(m) => m.name(),
        ToServer::Client(m) => m.name(),
    }
}
