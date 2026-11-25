//! The QUIC host the server thread polls: `CDPServer`, `CDPClientProxy`
//! and `RemoteClient`'s proxy table (`NeuronRuntime/CDPServer.cpp`,
//! `NeuronRuntime/CDPClientProxy.cpp`, `RemoteClient/RemoteClient_transport.cpp`).

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::fmt;
use std::marker::PhantomData;
use std::net::{SocketAddr, UdpSocket};
use std::sync::atomic::Ordering;
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::sync::{Arc, Mutex, PoisonError};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use neuron_core::net::frame::{ALPN, MAX_MESSAGE_LEN};
use neuron_core::net::pin::CertificateFingerprint;
use neuron_core::net::transport::{
    ConnectionId, DisconnectReason, LinkStats, Transport, TransportError, TransportEvent,
};
use neuron_core::net::wake::WakeQueue;
use neuron_core::net::wire::{WireMessage, WireWriter};
use quinn::crypto::rustls::QuicServerConfig;
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};

use super::governor::{Batcher, SAMPLE_INTERVAL, SaturationGovernor};
use super::identity::ServerIdentity;
use super::io::{self, Control, HostShared, IoEvent, Link, Outbound};

/// Client ids run from 1 to 128: `CClientImpl::ClientProxies[128]`.
pub const MAX_CLIENTS: usize = 128;

/// The C++ server's default player limit (`ServerConfigDlg`, 16).
pub const DEFAULT_MAX_PLAYERS: u16 = 16;

/// What the handshake checks and a status query reports; the server may
/// change it while it runs (`SetRemoteClientPassword`,
/// `SetRemoteClientSessionName`, `SetRemoteClientResponseData`,
/// `CDPServer::SetMaxPlayers`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HostSettings {
    /// Players admitted at once, 1 to [`MAX_CLIENTS`].
    pub max_players: u16,
    /// The session password; empty for none.
    pub password: String,
    /// The session name.
    pub name: String,
    /// The server description.
    pub description: String,
    /// Whether players may fight each other.
    pub pvp: bool,
    /// Whether the server accepts new accounts.
    pub new_players: bool,
    /// The server's ID.
    pub server_id: u32,
    /// The server's signature text.
    pub server_signature: String,
}

impl Default for HostSettings {
    /// `CClientImpl::Startup`: the default session name, no password, and
    /// the dialog's default player limit.
    fn default() -> Self {
        Self {
            max_players: DEFAULT_MAX_PLAYERS,
            password: String::new(),
            name: "OutpostFrontier Default Session Name".to_owned(),
            description: String::new(),
            pvp: false,
            new_players: true,
            server_id: 0,
            server_signature: String::new(),
        }
    }
}

/// The host's clocks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HostTiming {
    /// A connection that receives nothing for this long has timed out.
    pub idle_timeout: Duration,
    /// Each side sends a QUIC ping after this long without sending, so an
    /// idle game connection stays open.
    pub keep_alive: Duration,
    /// The QUIC and the game handshake must each finish in this time.
    pub handshake_timeout: Duration,
    /// How long a closing side waits for its last frames to be
    /// acknowledged before it closes anyway.
    pub close_linger: Duration,
    /// How often the round-trip time and the send window are refreshed.
    pub stats_interval: Duration,
    /// How often the governor samples the backlog ([`SAMPLE_INTERVAL`]).
    pub saturation_interval: Duration,
}

impl Default for HostTiming {
    fn default() -> Self {
        Self {
            idle_timeout: Duration::from_secs(15),
            keep_alive: Duration::from_secs(1),
            handshake_timeout: Duration::from_secs(5),
            close_linger: Duration::from_secs(1),
            stats_interval: Duration::from_millis(100),
            saturation_interval: SAMPLE_INTERVAL,
        }
    }
}

/// How to start a host.
#[derive(Clone, Debug)]
pub struct HostConfig {
    /// The UDP address to listen on; port 0 lets the system choose
    /// (`CDPServer::BeginHosting` with port 0).
    pub bind: SocketAddr,
    /// The certificate clients pin.
    pub identity: ServerIdentity,
    /// The server's build number; a client of another build is refused.
    pub build: u32,
    /// The handshake's and the status query's settings.
    pub settings: HostSettings,
    /// The clocks.
    pub timing: HostTiming,
}

/// Why a host cannot start.
#[derive(Debug)]
pub enum HostError {
    /// The socket, the runtime or the I/O thread could not be set up.
    Io(std::io::Error),
    /// The identity is not a usable certificate and key.
    Tls(rustls::Error),
    /// A timing is outside what QUIC accepts.
    Config(&'static str),
}

impl fmt::Display for HostError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(f, "cannot host: {error}"),
            Self::Tls(error) => write!(f, "unusable server certificate: {error}"),
            Self::Config(what) => write!(f, "invalid host setting: {what}"),
        }
    }
}

impl std::error::Error for HostError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            Self::Tls(error) => Some(error),
            Self::Config(_) => None,
        }
    }
}

impl From<std::io::Error> for HostError {
    fn from(error: std::io::Error) -> Self {
        Self::Io(error)
    }
}

/// What the host knows of a connected client.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PeerInfo {
    /// The client's address.
    pub address: SocketAddr,
    /// The account name of its handshake (decision T16).
    pub account: String,
}

struct Connection {
    key: u64,
    link: Arc<Link>,
    batch: Batcher,
    governor: SaturationGovernor,
    peer: PeerInfo,
}

impl Connection {
    fn flush(&mut self) {
        if let Some(frame) = self.batch.flush() {
            self.link.enqueue(frame);
        }
    }

    fn queued_bytes(&self) -> u64 {
        self.batch.pending_bytes() as u64 + self.link.backlog_bytes.load(Ordering::Relaxed)
    }
}

/// The server side of the QUIC transport: sends `Out` to clients and
/// receives `In` from them.
///
/// A client's [`ConnectionId`] is its client id: the lowest free slot from
/// 1 to [`MAX_CLIENTS`] when its handshake passed, free again once the
/// server thread has polled its [`TransportEvent::Disconnected`].
/// Messages sent to a client wait in a batch until it is full, until
/// [`Transport::flush`], or until the next [`Transport::poll`].
pub struct QuicServer<Out, In> {
    local_address: SocketAddr,
    fingerprint: CertificateFingerprint,
    shared: Arc<HostShared>,
    control: Arc<WakeQueue<Control>>,
    events: Receiver<IoEvent>,
    connections: BTreeMap<ConnectionId, Connection>,
    keys: HashMap<u64, ConnectionId>,
    pending: VecDeque<TransportEvent<In>>,
    saturation_interval: Duration,
    thread: Option<JoinHandle<()>>,
    io_gone: bool,
    _messages: PhantomData<fn(Out) -> In>,
}

impl<Out, In> fmt::Debug for QuicServer<Out, In> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("QuicServer")
            .field("local_address", &self.local_address)
            .field("connections", &self.connections.len())
            .finish_non_exhaustive()
    }
}

impl<Out, In> QuicServer<Out, In> {
    /// Binds the socket, starts the I/O thread and waits until the
    /// endpoint listens (`CDPServer::BeginHosting`).
    ///
    /// # Errors
    /// [`HostError`] when the address cannot be bound, the identity is
    /// unusable or a timing is out of range.
    pub fn bind(config: HostConfig) -> Result<Self, HostError> {
        let HostConfig {
            bind,
            identity,
            build,
            mut settings,
            timing,
        } = config;
        settings.max_players = clamp_players(settings.max_players);
        let server_config = server_config(&identity, &timing)?;
        let socket = UdpSocket::bind(bind)?;
        let local_address = socket.local_addr()?;
        let shared = Arc::new(HostShared {
            build,
            settings: Mutex::new(settings),
            slots: Mutex::new([false; MAX_CLIENTS]),
        });
        let control = Arc::new(WakeQueue::new());
        let (event_tx, events) = mpsc::channel();
        let (ready_tx, ready) = mpsc::channel();
        let thread = {
            let shared = Arc::clone(&shared);
            let control = Arc::clone(&control);
            std::thread::Builder::new()
                .name("neuron-server-net".to_owned())
                .spawn(move || {
                    io::run(
                        socket,
                        server_config,
                        shared,
                        timing,
                        control,
                        event_tx,
                        ready_tx,
                    );
                })?
        };
        match ready.recv() {
            Ok(Ok(())) => {}
            Ok(Err(error)) => {
                let _ = thread.join();
                return Err(HostError::Io(error));
            }
            Err(_) => {
                let _ = thread.join();
                return Err(HostError::Io(std::io::Error::other(
                    "the network thread ended while starting",
                )));
            }
        }
        Ok(Self {
            local_address,
            fingerprint: identity.fingerprint(),
            shared,
            control,
            events,
            connections: BTreeMap::new(),
            keys: HashMap::new(),
            pending: VecDeque::new(),
            saturation_interval: timing.saturation_interval,
            thread: Some(thread),
            io_gone: false,
            _messages: PhantomData,
        })
    }

    /// The address the host listens on (`GetRemoteClientPort`).
    #[must_use]
    pub const fn local_address(&self) -> SocketAddr {
        self.local_address
    }

    /// The fingerprint of the certificate the host presents.
    #[must_use]
    pub const fn fingerprint(&self) -> CertificateFingerprint {
        self.fingerprint
    }

    /// The handshake's settings now.
    #[must_use]
    pub fn settings(&self) -> HostSettings {
        self.shared.settings()
    }

    /// Changes the handshake's settings; clients already connected stay.
    pub fn set_settings(&mut self, mut settings: HostSettings) {
        settings.max_players = clamp_players(settings.max_players);
        *self
            .shared
            .settings
            .lock()
            .unwrap_or_else(PoisonError::into_inner) = settings;
    }

    /// Connections open now (`GetNumClients`).
    #[must_use]
    pub fn connection_count(&self) -> usize {
        self.connections.len()
    }

    /// The address and account of an open connection.
    #[must_use]
    pub fn peer(&self, connection: ConnectionId) -> Option<&PeerInfo> {
        self.connections.get(&connection).map(|c| &c.peer)
    }

    fn take_event(&mut self) -> Option<TransportEvent<In>> {
        let event = self.pending.pop_front()?;
        if let TransportEvent::Disconnected { connection, .. } = &event {
            self.shared.free_slot(connection.get());
        }
        Some(event)
    }

    /// Ends `connection` with `reason`: its batch is flushed, the writer
    /// finishes, and the server thread is told now.
    fn close(&mut self, id: ConnectionId, reason: DisconnectReason) {
        if let Some(mut connection) = self.connections.remove(&id) {
            self.keys.remove(&connection.key);
            connection.flush();
            let _ = connection.link.queue.push(Outbound::Close(reason));
            connection.link.queue.close();
            self.pending.push_back(TransportEvent::Disconnected {
                connection: id,
                reason,
            });
        }
    }

    fn io_ended(&mut self) {
        self.io_gone = true;
        let open: Vec<ConnectionId> = self.connections.keys().copied().collect();
        for id in open {
            self.close(id, DisconnectReason::Failed);
        }
    }
}

impl<Out, In> QuicServer<Out, In>
where
    In: WireMessage,
{
    fn receive(&mut self, event: IoEvent) {
        match event {
            IoEvent::Connected {
                key,
                id,
                link,
                address,
                account,
            } => {
                let id = ConnectionId::new(id);
                self.keys.insert(key, id);
                self.connections.insert(
                    id,
                    Connection {
                        key,
                        link,
                        batch: Batcher::new(),
                        governor: SaturationGovernor::new(Instant::now(), self.saturation_interval),
                        peer: PeerInfo { address, account },
                    },
                );
                self.pending.push_back(TransportEvent::Connected(id));
            }
            IoEvent::Messages { key, messages } => {
                let Some(&id) = self.keys.get(&key) else {
                    return;
                };
                for bytes in messages {
                    match In::from_wire(&bytes) {
                        Ok(message) => self.pending.push_back(TransportEvent::Message {
                            connection: id,
                            message,
                        }),
                        Err(error) => {
                            // `CRemoteClientProxy::OnReceive`: a message that
                            // does not decode disconnects the client.
                            tracing::info!(connection = %id, %error, "undecodable client message");
                            self.close(id, DisconnectReason::Protocol);
                            return;
                        }
                    }
                }
            }
            IoEvent::Closed { key, reason } => {
                if let Some(id) = self.keys.remove(&key) {
                    self.connections.remove(&id);
                    self.pending.push_back(TransportEvent::Disconnected {
                        connection: id,
                        reason,
                    });
                }
            }
        }
    }
}

impl<Out, In> Transport for QuicServer<Out, In>
where
    Out: WireMessage,
    In: WireMessage,
{
    type Outgoing = Out;
    type Incoming = In;

    /// `CClientImpl::PushToClient`: encodes the message (up to the C++'s
    /// retry buffer, [`MAX_MESSAGE_LEN`]) and adds it to the batch.
    fn send(&mut self, connection: ConnectionId, message: Out) -> Result<(), TransportError> {
        let open = self
            .connections
            .get_mut(&connection)
            .ok_or(TransportError::UnknownConnection(connection))?;
        let mut writer = WireWriter::with_limit(MAX_MESSAGE_LEN);
        message.encode(&mut writer)?;
        let mut ready = Vec::new();
        open.batch.push(writer.as_bytes(), &mut ready)?;
        for frame in ready {
            open.link.enqueue(frame);
        }
        Ok(())
    }

    /// `CDPServer::DispatchMsgs`: flushes the batches, then the next event;
    /// each call also lets every connection's governor take its sample
    /// (`UpdateQueueHistory`).
    fn poll(&mut self) -> Option<TransportEvent<In>> {
        self.flush();
        let now = Instant::now();
        for connection in self.connections.values_mut() {
            let queued = connection.queued_bytes();
            connection.governor.update(now, queued);
        }
        loop {
            if let Some(event) = self.take_event() {
                return Some(event);
            }
            if self.io_gone {
                return None;
            }
            match self.events.try_recv() {
                Ok(event) => self.receive(event),
                Err(TryRecvError::Empty) => return None,
                Err(TryRecvError::Disconnected) => self.io_ended(),
            }
        }
    }

    /// `CDPClientProxy::Disconnect` and `ForceClientLogout`.
    fn disconnect(
        &mut self,
        connection: ConnectionId,
        reason: DisconnectReason,
    ) -> Result<(), TransportError> {
        if !self.connections.contains_key(&connection) {
            return Err(TransportError::UnknownConnection(connection));
        }
        // A connection the server closed reports nothing more, not even
        // messages that arrived before.
        self.pending.retain(|event| {
            !matches!(event, TransportEvent::Message { connection: from, .. } if *from == connection)
        });
        self.close(connection, reason);
        Ok(())
    }

    /// `GetSendQSize` and `GetSendQBytes`: the batch being filled and the
    /// frames QUIC has not taken yet; the round-trip time QUIC measures.
    fn link_stats(&self, connection: ConnectionId) -> Option<LinkStats> {
        let open = self.connections.get(&connection)?;
        Some(LinkStats {
            queued_messages: open.batch.pending_messages()
                + open.link.backlog_frames.load(Ordering::Relaxed),
            queued_bytes: open.queued_bytes(),
            round_trip: Some(open.link.round_trip()),
        })
    }

    fn flush(&mut self) {
        for connection in self.connections.values_mut() {
            connection.flush();
        }
    }

    /// `CClientImpl::GetLinkSaturation`.
    fn link_saturation(&self, connection: ConnectionId) -> f64 {
        self.connections
            .get(&connection)
            .map_or(0.0, |c| c.governor.saturation())
    }
}

impl<Out, In> Drop for QuicServer<Out, In> {
    /// `CDPServer::~CDPServer`: closes every connection and the endpoint.
    fn drop(&mut self) {
        for connection in self.connections.values() {
            connection.link.queue.close();
        }
        let _ = self.control.push(Control::Shutdown);
        self.control.close();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn clamp_players(players: u16) -> u16 {
    players.clamp(1, MAX_CLIENTS as u16)
}

fn server_config(
    identity: &ServerIdentity,
    timing: &HostTiming,
) -> Result<quinn::ServerConfig, HostError> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut tls = rustls::ServerConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .map_err(HostError::Tls)?
        .with_no_client_auth()
        .with_single_cert(
            vec![CertificateDer::from(identity.certificate_der().to_vec())],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(identity.key_der().to_vec())),
        )
        .map_err(HostError::Tls)?;
    tls.alpn_protocols = vec![ALPN.to_vec()];
    let crypto = QuicServerConfig::try_from(tls)
        .map_err(|_| HostError::Config("the TLS provider has no QUIC cipher suite"))?;
    let mut config = quinn::ServerConfig::with_crypto(Arc::new(crypto));
    config.transport_config(Arc::new(transport_config(timing)?));
    Ok(config)
}

/// One bidirectional stream, opened by the client; no datagrams.
fn transport_config(timing: &HostTiming) -> Result<quinn::TransportConfig, HostError> {
    let mut transport = quinn::TransportConfig::default();
    transport
        .max_idle_timeout(Some(
            quinn::IdleTimeout::try_from(timing.idle_timeout)
                .map_err(|_| HostError::Config("idle timeout"))?,
        ))
        .keep_alive_interval(Some(timing.keep_alive))
        .max_concurrent_bidi_streams(1u32.into())
        .max_concurrent_uni_streams(0u32.into())
        .datagram_receive_buffer_size(None)
        .send_window(INITIAL_SEND_WINDOW);
    Ok(transport)
}

/// The send window until the first statistics: two initial congestion
/// windows.
const INITIAL_SEND_WINDOW: u64 = 32 * 1024;
