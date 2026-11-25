//! The QUIC connection the game thread polls: `CDPClient` and
//! `FRemoteServerImpl`'s transport (`NeuronRuntime/CDPClient.cpp`,
//! `RemoteServer/RemoteServer_main.cpp`).

use std::collections::VecDeque;
use std::fmt;
use std::marker::PhantomData;
use std::net::SocketAddr;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::time::Duration;

use neuron_core::net::frame::{
    Compression, Hello, HelloIntent, PROTOCOL_VERSION, ServerStatus, encode_batch,
};
use neuron_core::net::pin::{CertificateFingerprint, ServerTrust};
use neuron_core::net::transport::{
    ConnectionId, DisconnectReason, LinkStats, Transport, TransportError, TransportEvent,
};
use neuron_core::net::wire::{WireMessage, WireWriter};

use super::io::{self, Attempt, IoEvent, Link, Outbound};

/// The client's one connection, to its server.
pub const SERVER_CONNECTION: ConnectionId = ConnectionId::new(1);

/// Largest message the client sends: `FRemoteServerImpl::PushToServer`'s
/// encode buffer (`RemoteServer/RemoteServer_main.cpp:153`).
pub const MESSAGE_LIMIT: usize = 0x2800;

/// The client's clocks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClientTiming {
    /// A connection that receives nothing for this long has timed out;
    /// QUIC uses the shorter of the client's and the server's.
    pub idle_timeout: Duration,
    /// A QUIC ping goes out after this long without sending.
    pub keep_alive: Duration,
    /// The QUIC and the game handshake must each finish in this time, per
    /// candidate address.
    pub handshake_timeout: Duration,
    /// How long a closing client waits for its last frames to be
    /// acknowledged.
    pub close_linger: Duration,
    /// How often the round-trip time and the receive time are refreshed.
    pub stats_interval: Duration,
}

impl Default for ClientTiming {
    fn default() -> Self {
        Self {
            idle_timeout: Duration::from_secs(15),
            keep_alive: Duration::from_secs(1),
            handshake_timeout: Duration::from_secs(5),
            close_linger: Duration::from_secs(1),
            stats_interval: Duration::from_millis(100),
        }
    }
}

/// How to connect.
#[derive(Clone, Debug)]
pub struct ConnectConfig {
    /// The server's addresses, tried in order (the C++'s candidate ports).
    pub candidates: Vec<SocketAddr>,
    /// The client's build number.
    pub build: u32,
    /// The account name: the machine-derived MD5 text (T16).
    pub account: String,
    /// The session password; empty when the server has none.
    pub password: String,
    /// The certificate to accept.
    pub trust: ServerTrust,
    /// The protocol version the `Hello` states; [`PROTOCOL_VERSION`]
    /// except in tests of the refusal.
    pub protocol_version: u16,
    /// The clocks.
    pub timing: ClientTiming,
}

impl ConnectConfig {
    /// A join of `candidates` with the default clocks.
    #[must_use]
    pub fn new(
        candidates: Vec<SocketAddr>,
        build: u32,
        account: impl Into<String>,
        password: impl Into<String>,
        trust: ServerTrust,
    ) -> Self {
        Self {
            candidates,
            build,
            account: account.into(),
            password: password.into(),
            trust,
            protocol_version: PROTOCOL_VERSION,
            timing: ClientTiming::default(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    Connecting,
    Open,
    Closed,
}

/// The client side of the QUIC transport: sends `Out` to the server and
/// receives `In` from it, on [`SERVER_CONNECTION`].
///
/// [`QuicClient::connect`] returns at once; the I/O thread tries the
/// candidates and [`Transport::poll`] reports
/// [`TransportEvent::Connected`] after the server's `Welcome`, or
/// [`TransportEvent::Disconnected`] when no candidate admitted the client.
/// Each message goes to the server in its own raw batch, as the C++ client
/// sends each packet alone and uncompressed.
pub struct QuicClient<Out, In> {
    link: Arc<Link>,
    events: Receiver<IoEvent>,
    pending: VecDeque<TransportEvent<In>>,
    state: State,
    address: Option<SocketAddr>,
    server_build: Option<u32>,
    _messages: PhantomData<fn(Out) -> In>,
}

impl<Out, In> fmt::Debug for QuicClient<Out, In> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("QuicClient")
            .field("state", &self.state)
            .field("address", &self.address)
            .finish_non_exhaustive()
    }
}

impl<Out, In> QuicClient<Out, In> {
    /// Starts connecting (`FRemoteServerImpl::Connect`).
    ///
    /// # Errors
    /// When the I/O thread cannot be started.
    pub fn connect(config: ConnectConfig) -> std::io::Result<Self> {
        let ConnectConfig {
            candidates,
            build,
            account,
            password,
            trust,
            protocol_version,
            timing,
        } = config;
        let attempt = Attempt {
            hello: Hello {
                protocol_version,
                build,
                intent: HelloIntent::Join,
                account,
                password,
            },
            trust,
            timing,
        };
        let link = Arc::new(Link::new());
        let (event_tx, events) = mpsc::channel();
        {
            let link = Arc::clone(&link);
            // The thread ends by itself once the connection has closed;
            // nothing waits for it.
            std::thread::Builder::new()
                .name("neuron-client-net".to_owned())
                .spawn(move || {
                    io::run(
                        &event_tx.clone(),
                        io::join(candidates, attempt, link, event_tx),
                    );
                })?;
        }
        Ok(Self {
            link,
            events,
            pending: VecDeque::new(),
            state: State::Connecting,
            address: None,
            server_build: None,
            _messages: PhantomData,
        })
    }

    /// Whether the handshake passed and the connection has not ended.
    #[must_use]
    pub fn is_open(&self) -> bool {
        self.state == State::Open
    }

    /// The candidate address that admitted the client.
    #[must_use]
    pub const fn server_address(&self) -> Option<SocketAddr> {
        self.address
    }

    /// The server's build, from its `Welcome`.
    #[must_use]
    pub const fn server_build(&self) -> Option<u32> {
        self.server_build
    }

    /// The fingerprint of the certificate the server presented, also after
    /// a [`DisconnectReason::CertificateChanged`], so the player can be
    /// shown the new one. The game keeps it as the pin after a first
    /// connection.
    #[must_use]
    pub fn server_fingerprint(&self) -> Option<CertificateFingerprint> {
        self.link.presented()
    }

    /// The time since the last datagram from the server, in place of
    /// `CDPClient::GetLastMsgTimestamp` (the client's stall flag).
    #[must_use]
    pub fn since_last_receive(&self) -> Option<Duration> {
        self.link.since_last_receive()
    }

    fn close(&mut self, reason: DisconnectReason) {
        if self.state == State::Closed {
            return;
        }
        self.state = State::Closed;
        let _ = self.link.queue.push(Outbound::Close(reason));
        self.link.queue.close();
        self.pending.push_back(TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason,
        });
    }
}

impl<Out, In> QuicClient<Out, In>
where
    In: WireMessage,
{
    fn receive(&mut self, event: IoEvent) {
        match event {
            IoEvent::Connected { address, build } => {
                if self.state == State::Connecting {
                    self.state = State::Open;
                    self.address = Some(address);
                    self.server_build = Some(build);
                    self.pending
                        .push_back(TransportEvent::Connected(SERVER_CONNECTION));
                }
            }
            IoEvent::Messages(messages) => {
                if self.state != State::Open {
                    return;
                }
                for bytes in messages {
                    match In::from_wire(&bytes) {
                        Ok(message) => self.pending.push_back(TransportEvent::Message {
                            connection: SERVER_CONNECTION,
                            message,
                        }),
                        Err(error) => {
                            tracing::info!(%error, "undecodable server message");
                            self.close(DisconnectReason::Protocol);
                            return;
                        }
                    }
                }
            }
            IoEvent::Closed(reason) => {
                if self.state != State::Closed {
                    self.state = State::Closed;
                    self.link.queue.close();
                    self.pending.push_back(TransportEvent::Disconnected {
                        connection: SERVER_CONNECTION,
                        reason,
                    });
                }
            }
            IoEvent::Status(_) => {}
        }
    }
}

impl<Out, In> Transport for QuicClient<Out, In>
where
    Out: WireMessage,
    In: WireMessage,
{
    type Outgoing = Out;
    type Incoming = In;

    /// `FRemoteServerImpl::PushToServer` and `CDPClient::Send`: refused
    /// before the handshake passed and after the end.
    fn send(&mut self, connection: ConnectionId, message: Out) -> Result<(), TransportError> {
        if connection != SERVER_CONNECTION || self.state != State::Open {
            return Err(TransportError::UnknownConnection(connection));
        }
        let mut writer = WireWriter::with_limit(MESSAGE_LIMIT);
        message.encode(&mut writer)?;
        let frame = encode_batch([writer.as_bytes()], Compression::Never)?;
        self.link.enqueue(frame);
        Ok(())
    }

    /// `CDPClient::DispatchMsgs`.
    fn poll(&mut self) -> Option<TransportEvent<In>> {
        loop {
            if let Some(event) = self.pending.pop_front() {
                return Some(event);
            }
            if self.state == State::Closed {
                return None;
            }
            match self.events.try_recv() {
                Ok(event) => self.receive(event),
                Err(TryRecvError::Empty) => return None,
                Err(TryRecvError::Disconnected) => {
                    self.receive(IoEvent::Closed(DisconnectReason::Failed))
                }
            }
        }
    }

    fn disconnect(
        &mut self,
        connection: ConnectionId,
        reason: DisconnectReason,
    ) -> Result<(), TransportError> {
        if connection != SERVER_CONNECTION || self.state != State::Open {
            return Err(TransportError::UnknownConnection(connection));
        }
        // Nothing more is reported, not even messages that arrived before.
        self.pending.clear();
        self.close(reason);
        Ok(())
    }

    /// `CDPClient::GetSendQueueSize`, and QUIC's round-trip time in place
    /// of the ping's (`GetPingDelay`).
    fn link_stats(&self, connection: ConnectionId) -> Option<LinkStats> {
        if connection != SERVER_CONNECTION || self.state != State::Open {
            return None;
        }
        Some(LinkStats {
            queued_messages: self.link.backlog_frames.load(Ordering::Relaxed),
            queued_bytes: self.link.backlog_bytes.load(Ordering::Relaxed),
            round_trip: self.link.round_trip(),
        })
    }
}

impl<Out, In> Drop for QuicClient<Out, In> {
    /// Closes the connection normally, or abandons the attempts.
    fn drop(&mut self) {
        if self.state != State::Closed {
            let _ = self
                .link
                .queue
                .push(Outbound::Close(DisconnectReason::Closed));
        }
        self.link.queue.close();
    }
}

/// What a status query found.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StatusReply {
    /// The server's status.
    pub status: ServerStatus,
    /// The round-trip time QUIC measured during the handshake (the
    /// enumeration's `dwRoundTripLatencyMS`).
    pub round_trip: Duration,
    /// The fingerprint of the server's certificate.
    pub fingerprint: Option<CertificateFingerprint>,
}

/// A status query in flight: the address book's row (decisions O8, T17),
/// in place of `CDPClient::EnumerateHost`.
#[derive(Debug)]
pub struct StatusQuery {
    link: Arc<Link>,
    events: Receiver<IoEvent>,
    done: bool,
}

impl StatusQuery {
    /// Asks the server at `address`; a `Pinned` trust refuses another
    /// certificate as a join does.
    ///
    /// # Errors
    /// When the I/O thread cannot be started.
    pub fn start(
        address: SocketAddr,
        build: u32,
        trust: ServerTrust,
        timing: ClientTiming,
    ) -> std::io::Result<Self> {
        let attempt = Attempt {
            hello: Hello {
                protocol_version: PROTOCOL_VERSION,
                build,
                intent: HelloIntent::Query,
                account: String::new(),
                password: String::new(),
            },
            trust,
            timing,
        };
        let link = Arc::new(Link::new());
        let (event_tx, events) = mpsc::channel();
        {
            let link = Arc::clone(&link);
            std::thread::Builder::new()
                .name("neuron-client-query".to_owned())
                .spawn(move || {
                    io::run(
                        &event_tx.clone(),
                        io::query(address, attempt, link, event_tx),
                    );
                })?;
        }
        Ok(Self {
            link,
            events,
            done: false,
        })
    }

    /// The answer, once: the server's status, or why there is none.
    /// `None` while the query runs and after the answer was taken.
    pub fn poll(&mut self) -> Option<Result<StatusReply, DisconnectReason>> {
        if self.done {
            return None;
        }
        let event = match self.events.try_recv() {
            Ok(event) => event,
            Err(TryRecvError::Empty) => return None,
            Err(TryRecvError::Disconnected) => IoEvent::Closed(DisconnectReason::Failed),
        };
        self.done = true;
        Some(match event {
            IoEvent::Status(status) => Ok(StatusReply {
                status,
                round_trip: self.link.round_trip().unwrap_or_default(),
                fingerprint: self.link.presented(),
            }),
            IoEvent::Closed(reason) => Err(reason),
            IoEvent::Connected { .. } | IoEvent::Messages(_) => Err(DisconnectReason::Protocol),
        })
    }
}
