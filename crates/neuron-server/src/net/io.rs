//! The host's I/O thread: the QUIC endpoint on a current-thread tokio
//! runtime, one task per connection's reads and one for its writes, and
//! the hand-over to the server thread (decisions T9, T17).
//!
//! The server thread never waits on this side: it pushes frames into a
//! connection's [`WakeQueue`] and takes events from a standard channel,
//! as `CDPServer::DispatchMsgs` drains the list DirectPlay's callbacks
//! fill (`NeuronRuntime/CDPServer.cpp:326-427`).

use std::net::{SocketAddr, UdpSocket};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::mpsc::Sender;
use std::sync::{Arc, Mutex, PoisonError};
use std::time::Duration;

use neuron_core::net::frame::{
    Frame, FrameDecoder, Hello, HelloIntent, PROTOCOL_VERSION, Reject, RejectReason, ServerStatus,
    Welcome, encode_frame,
};
use neuron_core::net::transport::DisconnectReason;
use neuron_core::net::wake::WakeQueue;
use quinn::{ConnectionError, Endpoint, EndpointConfig, Incoming, RecvStream, SendStream, VarInt};

use super::host::{HostSettings, HostTiming, MAX_CLIENTS};

/// What the server thread hands one connection's writer.
#[derive(Debug)]
pub(super) enum Outbound {
    /// One frame's bytes.
    Frame(Vec<u8>),
    /// Finish the stream, wait for its acknowledgement, close.
    Close(DisconnectReason),
}

/// One connection's state both threads see.
#[derive(Debug)]
pub(super) struct Link {
    /// Frames for the writer.
    pub(super) queue: WakeQueue<Outbound>,
    /// Bytes of the frames queued and not yet taken by QUIC.
    pub(super) backlog_bytes: AtomicU64,
    /// Frames queued and not yet taken by QUIC.
    pub(super) backlog_frames: AtomicUsize,
    /// QUIC's smoothed round-trip time, in microseconds.
    pub(super) round_trip_micros: AtomicU64,
}

impl Link {
    fn new(round_trip: Duration) -> Self {
        Self {
            queue: WakeQueue::new(),
            backlog_bytes: AtomicU64::new(0),
            backlog_frames: AtomicUsize::new(0),
            round_trip_micros: AtomicU64::new(micros(round_trip)),
        }
    }

    /// Queues one frame, counted in the backlog until QUIC takes it.
    pub(super) fn enqueue(&self, frame: Vec<u8>) {
        let length = frame.len() as u64;
        self.backlog_bytes.fetch_add(length, Ordering::Relaxed);
        self.backlog_frames.fetch_add(1, Ordering::Relaxed);
        if self.queue.push(Outbound::Frame(frame)).is_err() {
            self.taken(length);
        }
    }

    fn taken(&self, length: u64) {
        self.backlog_bytes.fetch_sub(length, Ordering::Relaxed);
        self.backlog_frames.fetch_sub(1, Ordering::Relaxed);
    }

    /// The round-trip time.
    pub(super) fn round_trip(&self) -> Duration {
        Duration::from_micros(self.round_trip_micros.load(Ordering::Relaxed))
    }
}

/// What the I/O thread reports to the server thread, in order.
#[derive(Debug)]
pub(super) enum IoEvent {
    /// A client passed the handshake and holds slot `id`.
    Connected {
        key: u64,
        id: u32,
        link: Arc<Link>,
        address: SocketAddr,
        account: String,
    },
    /// The messages of one batch.
    Messages { key: u64, messages: Vec<Vec<u8>> },
    /// The connection ended.
    Closed { key: u64, reason: DisconnectReason },
}

/// What the server thread tells the I/O thread as a whole.
#[derive(Debug)]
pub(super) enum Control {
    /// Close every connection and the endpoint.
    Shutdown,
}

/// The host state the handshake reads, shared by both threads.
#[derive(Debug)]
pub(super) struct HostShared {
    pub(super) build: u32,
    pub(super) settings: Mutex<HostSettings>,
    /// Slot `i` is client id `i + 1`; true while it is in use. A slot is
    /// freed when the server thread reports the connection's end.
    pub(super) slots: Mutex<[bool; MAX_CLIENTS]>,
}

impl HostShared {
    pub(super) fn settings(&self) -> HostSettings {
        self.settings
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clone()
    }

    pub(super) fn free_slot(&self, id: u32) {
        let mut slots = self.slots.lock().unwrap_or_else(PoisonError::into_inner);
        if let Some(slot) = usize::try_from(id)
            .ok()
            .and_then(|id| id.checked_sub(1))
            .and_then(|index| slots.get_mut(index))
        {
            *slot = false;
        }
    }

    fn players(&self) -> u16 {
        let slots = self.slots.lock().unwrap_or_else(PoisonError::into_inner);
        slots.iter().filter(|used| **used).count() as u16
    }

    /// The answer to `hello`: the checks of the handshake, then the lowest
    /// free slot (`CRemoteClientProxy::OnConnect`,
    /// `RemoteClient/RemoteClient_transport.cpp:271-285`).
    fn admit(&self, hello: &Hello) -> Admission {
        if hello.protocol_version != PROTOCOL_VERSION {
            return Admission::Reject(RejectReason::ProtocolVersion);
        }
        let settings = self.settings();
        if hello.intent == HelloIntent::Query {
            return Admission::Query(ServerStatus {
                build: self.build,
                name: settings.name,
                description: settings.description,
                players: self.players(),
                max_players: settings.max_players,
                password_required: !settings.password.is_empty(),
                pvp: settings.pvp,
                new_players: settings.new_players,
                server_id: settings.server_id,
                server_signature: settings.server_signature,
            });
        }
        if hello.build != self.build {
            return Admission::Reject(RejectReason::Build);
        }
        if !settings.password.is_empty() && hello.password != settings.password {
            return Admission::Reject(RejectReason::Password);
        }
        let mut slots = self.slots.lock().unwrap_or_else(PoisonError::into_inner);
        let used = slots.iter().filter(|used| **used).count();
        if used >= usize::from(settings.max_players) {
            return Admission::Reject(RejectReason::Full);
        }
        match slots.iter().position(|used| !*used) {
            Some(index) => {
                slots[index] = true;
                Admission::Join(index as u32 + 1)
            }
            None => Admission::Reject(RejectReason::Full),
        }
    }
}

enum Admission {
    Join(u32),
    Query(ServerStatus),
    Reject(RejectReason),
}

/// Everything a connection task needs.
#[derive(Clone)]
struct Context {
    shared: Arc<HostShared>,
    events: Sender<IoEvent>,
    timing: HostTiming,
    serial: Arc<AtomicU64>,
}

/// Runs the endpoint on this thread until [`Control::Shutdown`] or until
/// the control queue closes. `ready` receives the result of setting up.
pub(super) fn run(
    socket: UdpSocket,
    server_config: quinn::ServerConfig,
    shared: Arc<HostShared>,
    timing: HostTiming,
    control: Arc<WakeQueue<Control>>,
    events: Sender<IoEvent>,
    ready: Sender<std::io::Result<()>>,
) {
    let runtime = match tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    runtime.block_on(async move {
        let endpoint = match Endpoint::new(
            EndpointConfig::default(),
            Some(server_config),
            socket,
            Arc::new(quinn::TokioRuntime),
        ) {
            Ok(endpoint) => endpoint,
            Err(error) => {
                let _ = ready.send(Err(error));
                return;
            }
        };
        let _ = ready.send(Ok(()));
        let context = Context {
            shared,
            events,
            timing,
            serial: Arc::new(AtomicU64::new(0)),
        };
        tokio::spawn(accept(endpoint.clone(), context));
        // `Shutdown`, or the server thread's end of the queue closed.
        let _: Option<Control> = control.pop().await;
        endpoint.close(close_code(DisconnectReason::Closed), b"");
        let _ = tokio::time::timeout(timing.close_linger, endpoint.wait_idle()).await;
    });
}

async fn accept(endpoint: Endpoint, context: Context) {
    while let Some(incoming) = endpoint.accept().await {
        tokio::spawn(serve(incoming, context.clone()));
    }
}

/// One client: the QUIC handshake, the game handshake, then the reader;
/// the writer and the statistics run beside it.
async fn serve(incoming: Incoming, context: Context) {
    let handshake = context.timing.handshake_timeout;
    let Ok(connecting) = incoming.accept() else {
        return;
    };
    let Ok(Ok(connection)) = tokio::time::timeout(handshake, connecting).await else {
        return;
    };
    let Ok(Ok((mut send, mut recv))) =
        tokio::time::timeout(handshake, connection.accept_bi()).await
    else {
        connection.close(close_code(DisconnectReason::Protocol), b"");
        return;
    };
    let mut decoder = FrameDecoder::new();
    let hello = match tokio::time::timeout(handshake, read_frame(&mut recv, &mut decoder)).await {
        Ok(Ok(Some(Frame::Hello(hello)))) => hello,
        Ok(Err(StreamEnd::Closed(_))) => return,
        Err(_) => {
            connection.close(close_code(DisconnectReason::TimedOut), b"");
            return;
        }
        _ => {
            connection.close(close_code(DisconnectReason::Protocol), b"");
            return;
        }
    };
    let linger = context.timing.close_linger;
    let id = match context.shared.admit(&hello) {
        Admission::Join(id) => id,
        Admission::Query(status) => {
            finish_with(
                &connection,
                send,
                &Frame::Status(status),
                DisconnectReason::Closed,
                linger,
            )
            .await;
            return;
        }
        Admission::Reject(reason) => {
            tracing::info!(
                address = %connection.remote_address(),
                ?reason,
                "client refused at the handshake"
            );
            let reject = Frame::Reject(Reject {
                reason,
                protocol_version: PROTOCOL_VERSION,
                build: context.shared.build,
            });
            finish_with(
                &connection,
                send,
                &reject,
                DisconnectReason::Rejected(reason),
                linger,
            )
            .await;
            return;
        }
    };
    let welcome = Frame::Welcome(Welcome {
        protocol_version: PROTOCOL_VERSION,
        build: context.shared.build,
    });
    let written = match encode_frame(&welcome) {
        Ok(bytes) => send.write_all(&bytes).await.is_ok(),
        Err(_) => false,
    };
    if !written {
        context.shared.free_slot(id);
        connection.close(close_code(DisconnectReason::Failed), b"");
        return;
    }

    let key = context.serial.fetch_add(1, Ordering::Relaxed);
    let link = Arc::new(Link::new(connection.rtt()));
    let local_reason = Arc::new(Mutex::new(None));
    if context
        .events
        .send(IoEvent::Connected {
            key,
            id,
            link: Arc::clone(&link),
            address: connection.remote_address(),
            account: hello.account,
        })
        .is_err()
    {
        // The server thread has gone.
        context.shared.free_slot(id);
        connection.close(close_code(DisconnectReason::Closed), b"");
        return;
    }
    tokio::spawn(write(
        send,
        connection.clone(),
        Arc::clone(&link),
        Arc::clone(&local_reason),
        linger,
    ));
    tokio::spawn(statistics(
        connection.clone(),
        Arc::clone(&link),
        context.timing.stats_interval,
    ));
    let reason = read(
        &connection,
        recv,
        decoder,
        key,
        &context.events,
        &local_reason,
    )
    .await;
    // The writer stops waiting for frames.
    link.queue.close();
    let _ = context.events.send(IoEvent::Closed { key, reason });
}

/// Sends one last frame, finishes the stream, waits up to `linger` for
/// the client to acknowledge it, and closes with `reason`.
async fn finish_with(
    connection: &quinn::Connection,
    mut send: SendStream,
    frame: &Frame,
    reason: DisconnectReason,
    linger: Duration,
) {
    if let Ok(bytes) = encode_frame(frame)
        && send.write_all(&bytes).await.is_ok()
        && send.finish().is_ok()
    {
        let _ = tokio::time::timeout(linger, send.stopped()).await;
    }
    connection.close(close_code(reason), b"");
}

/// The writer: frames in order, then the close the server thread asked for.
async fn write(
    mut send: SendStream,
    connection: quinn::Connection,
    link: Arc<Link>,
    local_reason: Arc<Mutex<Option<DisconnectReason>>>,
    linger: Duration,
) {
    while let Some(outbound) = link.queue.pop().await {
        match outbound {
            Outbound::Frame(bytes) => {
                let written = send.write_all(&bytes).await.is_ok();
                link.taken(bytes.len() as u64);
                if !written {
                    return;
                }
            }
            Outbound::Close(reason) => {
                *local_reason.lock().unwrap_or_else(PoisonError::into_inner) = Some(reason);
                if send.finish().is_ok() {
                    let _ = tokio::time::timeout(linger, send.stopped()).await;
                }
                connection.close(close_code(reason), b"");
                return;
            }
        }
    }
}

/// The reader: every batch to the server thread until the stream or the
/// connection ends; the reason it ended.
async fn read(
    connection: &quinn::Connection,
    mut recv: RecvStream,
    mut decoder: FrameDecoder,
    key: u64,
    events: &Sender<IoEvent>,
    local_reason: &Mutex<Option<DisconnectReason>>,
) -> DisconnectReason {
    loop {
        match read_frame(&mut recv, &mut decoder).await {
            Ok(Some(Frame::Batch(messages))) => {
                if events.send(IoEvent::Messages { key, messages }).is_err() {
                    connection.close(close_code(DisconnectReason::Closed), b"");
                    return DisconnectReason::Closed;
                }
            }
            // A handshake frame after the handshake.
            Ok(Some(_)) | Err(StreamEnd::Malformed) => {
                *local_reason.lock().unwrap_or_else(PoisonError::into_inner) =
                    Some(DisconnectReason::Protocol);
                connection.close(close_code(DisconnectReason::Protocol), b"");
                return DisconnectReason::Protocol;
            }
            // The client finished its stream: its close follows.
            Ok(None) => {
                let error = connection.closed().await;
                return reason_of(&error, local_reason);
            }
            Err(StreamEnd::Closed(error)) => return reason_of(&error, local_reason),
        }
    }
}

/// Refreshes the round-trip time and lets QUIC buffer two congestion
/// windows: one in flight and one waiting. What the game sends beyond
/// that stays in the [`Link`] backlog, which the governor reads.
async fn statistics(connection: quinn::Connection, link: Arc<Link>, interval: Duration) {
    loop {
        tokio::time::sleep(interval).await;
        if connection.close_reason().is_some() {
            return;
        }
        link.round_trip_micros
            .store(micros(connection.rtt()), Ordering::Relaxed);
        let window = connection.congestion_state().window();
        connection.set_send_window(window.saturating_mul(2).max(MIN_SEND_WINDOW));
    }
}

/// The smallest send window the statistics set.
pub(super) const MIN_SEND_WINDOW: u64 = 4 * 1024;

/// Why reading a stream stopped.
#[derive(Debug)]
pub(super) enum StreamEnd {
    /// The connection ended.
    Closed(ConnectionError),
    /// The bytes are not frames, or the stream was reset.
    Malformed,
}

/// The next frame from `recv`; `Ok(None)` when the stream ended cleanly
/// between frames.
pub(super) async fn read_frame(
    recv: &mut RecvStream,
    decoder: &mut FrameDecoder,
) -> Result<Option<Frame>, StreamEnd> {
    let mut buffer = [0; 16 * 1024];
    loop {
        match decoder.next_frame() {
            Ok(Some(frame)) => return Ok(Some(frame)),
            Ok(None) => {}
            Err(_) => return Err(StreamEnd::Malformed),
        }
        match recv.read(&mut buffer).await {
            Ok(Some(length)) => decoder.push(&buffer[..length]),
            Ok(None) if decoder.buffered() == 0 => return Ok(None),
            Ok(None) => return Err(StreamEnd::Malformed),
            Err(quinn::ReadError::ConnectionLost(error)) => return Err(StreamEnd::Closed(error)),
            Err(_) => return Err(StreamEnd::Malformed),
        }
    }
}

/// The reason a connection error stands for; a local close reports the
/// reason it was closed with.
pub(super) fn reason_of(
    error: &ConnectionError,
    local_reason: &Mutex<Option<DisconnectReason>>,
) -> DisconnectReason {
    match error {
        ConnectionError::ApplicationClosed(close) => {
            DisconnectReason::from_close_code(close.error_code.into_inner())
        }
        ConnectionError::TimedOut => DisconnectReason::TimedOut,
        ConnectionError::LocallyClosed => local_reason
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .unwrap_or(DisconnectReason::Closed),
        ConnectionError::TransportError(_)
        | ConnectionError::ConnectionClosed(_)
        | ConnectionError::VersionMismatch
        | ConnectionError::Reset
        | ConnectionError::CidsExhausted => DisconnectReason::Failed,
    }
}

/// The QUIC close code of `reason`.
pub(super) fn close_code(reason: DisconnectReason) -> VarInt {
    VarInt::from_u32(reason.close_code())
}

fn micros(duration: Duration) -> u64 {
    u64::try_from(duration.as_micros()).unwrap_or(u64::MAX)
}
