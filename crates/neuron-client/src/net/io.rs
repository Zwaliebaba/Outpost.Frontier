//! The client's I/O thread: connection attempts, the game handshake, and
//! the reader and writer of the game stream on a current-thread tokio
//! runtime (decisions T9, T17). The game thread pushes frames into a
//! [`WakeQueue`] and takes events from a standard channel, as
//! `CDPClient::DispatchMsgs` drains what `HandleClientMsg` queued
//! (`NeuronRuntime/CDPClient.cpp:349-499`).

use std::net::{Ipv4Addr, Ipv6Addr, SocketAddr};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::mpsc::Sender;
use std::sync::{Arc, Mutex, PoisonError};
use std::time::{Duration, Instant};

use neuron_core::net::frame::{
    ALPN, Frame, FrameDecoder, Hello, PROTOCOL_VERSION, ServerStatus, encode_frame,
};
use neuron_core::net::pin::{CertificateFingerprint, ServerTrust};
use neuron_core::net::transport::DisconnectReason;
use neuron_core::net::wake::WakeQueue;
use quinn::crypto::rustls::QuicClientConfig;
use quinn::{ConnectionError, Endpoint, RecvStream, SendStream, VarInt};

use super::client::ClientTiming;
use super::verify::PinVerifier;

/// The name the client asks for; the pin, not the name, is checked.
const SERVER_NAME: &str = "outpost-frontier-server";

/// The send window until the first statistics.
const INITIAL_SEND_WINDOW: u64 = 32 * 1024;

/// The smallest send window the statistics set.
const MIN_SEND_WINDOW: u64 = 4 * 1024;

/// No time recorded yet.
const NEVER: u64 = u64::MAX;

/// What the game thread hands the writer.
#[derive(Debug)]
pub(super) enum Outbound {
    /// One frame's bytes.
    Frame(Vec<u8>),
    /// Finish the stream, wait for its acknowledgement, close.
    Close(DisconnectReason),
}

/// The connection's state both threads see.
#[derive(Debug)]
pub(super) struct Link {
    pub(super) queue: WakeQueue<Outbound>,
    pub(super) backlog_bytes: AtomicU64,
    pub(super) backlog_frames: AtomicUsize,
    round_trip_micros: AtomicU64,
    /// When a datagram last arrived, in microseconds after `epoch`.
    last_receive_micros: AtomicU64,
    epoch: Instant,
    presented: Mutex<Option<CertificateFingerprint>>,
}

impl Link {
    pub(super) fn new() -> Self {
        Self {
            queue: WakeQueue::new(),
            backlog_bytes: AtomicU64::new(0),
            backlog_frames: AtomicUsize::new(0),
            round_trip_micros: AtomicU64::new(NEVER),
            last_receive_micros: AtomicU64::new(NEVER),
            epoch: Instant::now(),
            presented: Mutex::new(None),
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

    /// QUIC's smoothed round-trip time, once connected.
    pub(super) fn round_trip(&self) -> Option<Duration> {
        match self.round_trip_micros.load(Ordering::Relaxed) {
            NEVER => None,
            micros => Some(Duration::from_micros(micros)),
        }
    }

    /// The time since the last datagram from the server.
    pub(super) fn since_last_receive(&self) -> Option<Duration> {
        match self.last_receive_micros.load(Ordering::Relaxed) {
            NEVER => None,
            micros => Some(
                self.epoch
                    .elapsed()
                    .saturating_sub(Duration::from_micros(micros)),
            ),
        }
    }

    fn received_now(&self) {
        self.last_receive_micros
            .store(micros(self.epoch.elapsed()), Ordering::Relaxed);
    }

    /// The fingerprint of the certificate the server presented last.
    pub(super) fn presented(&self) -> Option<CertificateFingerprint> {
        *self
            .presented
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
    }

    fn set_presented(&self, fingerprint: Option<CertificateFingerprint>) {
        if fingerprint.is_some() {
            *self
                .presented
                .lock()
                .unwrap_or_else(PoisonError::into_inner) = fingerprint;
        }
    }
}

/// What the I/O thread reports, in order.
#[derive(Debug)]
pub(super) enum IoEvent {
    /// The server accepted the handshake.
    Connected { address: SocketAddr, build: u32 },
    /// The messages of one batch.
    Messages(Vec<Vec<u8>>),
    /// The server's answer to a status query.
    Status(ServerStatus),
    /// The connection ended or never opened.
    Closed(DisconnectReason),
}

/// One attempt's settings.
#[derive(Clone, Debug)]
pub(super) struct Attempt {
    pub(super) hello: Hello,
    pub(super) trust: ServerTrust,
    pub(super) timing: ClientTiming,
}

/// Runs `body` on a new current-thread runtime; reports `Failed` when no
/// runtime can be built.
pub(super) fn run<F>(events: &Sender<IoEvent>, body: F)
where
    F: Future<Output = ()>,
{
    match tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime.block_on(body),
        Err(_) => {
            let _ = events.send(IoEvent::Closed(DisconnectReason::Failed));
        }
    }
}

/// `CDPClient::ConnectToServer` and the retries of `DispatchMsgs`'
/// connect-complete case (`NeuronRuntime/CDPClient.cpp:131-168, 543-563`):
/// each candidate address in turn until one completes the handshake. A
/// refusal or a changed certificate ends the attempts; the server
/// answered.
pub(super) async fn join(
    candidates: Vec<SocketAddr>,
    attempt: Attempt,
    link: Arc<Link>,
    events: Sender<IoEvent>,
) {
    let mut last = DisconnectReason::Failed;
    for address in candidates {
        if link.queue.is_closed() {
            return;
        }
        let opened = handshake(address, &attempt, &link).await;
        let opened = match opened {
            Ok(opened) => opened,
            Err(Failure::Final(reason)) => {
                let _ = events.send(IoEvent::Closed(reason));
                return;
            }
            Err(Failure::Retry(reason)) => {
                last = reason;
                continue;
            }
        };
        match opened.first {
            Frame::Welcome(welcome) if welcome.protocol_version == PROTOCOL_VERSION => {
                link.round_trip_micros
                    .store(micros(opened.connection.rtt()), Ordering::Relaxed);
                if events
                    .send(IoEvent::Connected {
                        address,
                        build: welcome.build,
                    })
                    .is_err()
                {
                    opened
                        .connection
                        .close(close_code(DisconnectReason::Closed), b"");
                    return;
                }
                session(opened, &attempt.timing, link, events).await;
                return;
            }
            Frame::Reject(reject) => {
                let reason = DisconnectReason::Rejected(reject.reason);
                opened.connection.close(close_code(reason), b"");
                let _ = events.send(IoEvent::Closed(reason));
                return;
            }
            _ => {
                opened
                    .connection
                    .close(close_code(DisconnectReason::Protocol), b"");
                last = DisconnectReason::Protocol;
            }
        }
    }
    let _ = events.send(IoEvent::Closed(last));
}

/// A status query (decision O8's address book): the handshake with a
/// query, the status, a normal close.
pub(super) async fn query(
    address: SocketAddr,
    attempt: Attempt,
    link: Arc<Link>,
    events: Sender<IoEvent>,
) {
    let opened = match handshake(address, &attempt, &link).await {
        Ok(opened) => opened,
        Err(Failure::Final(reason) | Failure::Retry(reason)) => {
            let _ = events.send(IoEvent::Closed(reason));
            return;
        }
    };
    link.round_trip_micros
        .store(micros(opened.connection.rtt()), Ordering::Relaxed);
    let event = match opened.first {
        Frame::Status(status) => IoEvent::Status(status),
        Frame::Reject(reject) => IoEvent::Closed(DisconnectReason::Rejected(reject.reason)),
        _ => IoEvent::Closed(DisconnectReason::Protocol),
    };
    opened
        .connection
        .close(close_code(DisconnectReason::Closed), b"");
    let _ = events.send(event);
    let _ = tokio::time::timeout(attempt.timing.close_linger, opened.endpoint.wait_idle()).await;
}

enum Failure {
    /// The next candidate may answer.
    Retry(DisconnectReason),
    /// The server answered; stop.
    Final(DisconnectReason),
}

struct Opened {
    endpoint: Endpoint,
    connection: quinn::Connection,
    send: SendStream,
    recv: RecvStream,
    decoder: FrameDecoder,
    first: Frame,
}

/// Connects to `address`, sends the `Hello` and reads the server's first
/// frame.
async fn handshake(address: SocketAddr, attempt: &Attempt, link: &Link) -> Result<Opened, Failure> {
    let timing = &attempt.timing;
    let bind = if address.is_ipv4() {
        SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0))
    } else {
        SocketAddr::from((Ipv6Addr::UNSPECIFIED, 0))
    };
    let endpoint = Endpoint::client(bind).map_err(|_| Failure::Retry(DisconnectReason::Failed))?;
    let verifier = Arc::new(PinVerifier::new(attempt.trust));
    let config = client_config(Arc::clone(&verifier), timing)?;
    let connecting = endpoint
        .connect_with(config, address, SERVER_NAME)
        .map_err(|_| Failure::Retry(DisconnectReason::Failed))?;
    let connected = tokio::time::timeout(timing.handshake_timeout, connecting).await;
    link.set_presented(verifier.presented());
    let connection = match connected {
        Ok(Ok(connection)) => connection,
        _ if verifier.mismatched() => {
            return Err(Failure::Final(DisconnectReason::CertificateChanged));
        }
        Ok(Err(error)) => return Err(Failure::Retry(reason_of(&error, None))),
        Err(_) => return Err(Failure::Retry(DisconnectReason::TimedOut)),
    };
    let opened = tokio::time::timeout(timing.handshake_timeout, async {
        let (mut send, mut recv) = connection.open_bi().await.ok()?;
        let hello = encode_frame(&Frame::Hello(attempt.hello.clone())).ok()?;
        send.write_all(&hello).await.ok()?;
        let mut decoder = FrameDecoder::new();
        let first = read_frame(&mut recv, &mut decoder).await;
        Some((send, recv, decoder, first))
    })
    .await;
    match opened {
        Ok(Some((send, recv, decoder, Ok(Some(first))))) => {
            link.received_now();
            Ok(Opened {
                endpoint,
                connection,
                send,
                recv,
                decoder,
                first,
            })
        }
        Ok(Some((_, _, _, Err(StreamEnd::Closed(error))))) => {
            Err(Failure::Retry(reason_of(&error, None)))
        }
        Err(_) => {
            connection.close(close_code(DisconnectReason::TimedOut), b"");
            Err(Failure::Retry(DisconnectReason::TimedOut))
        }
        _ => match connection.close_reason() {
            Some(error) => Err(Failure::Retry(reason_of(&error, None))),
            None => {
                connection.close(close_code(DisconnectReason::Protocol), b"");
                Err(Failure::Retry(DisconnectReason::Protocol))
            }
        },
    }
}

/// The game stream after the handshake: the writer and the statistics
/// beside the reader; the reader reports the end.
async fn session(opened: Opened, timing: &ClientTiming, link: Arc<Link>, events: Sender<IoEvent>) {
    let Opened {
        endpoint,
        connection,
        send,
        recv,
        decoder,
        first: _,
    } = opened;
    let local_reason = Arc::new(Mutex::new(None));
    tokio::spawn(write(
        send,
        connection.clone(),
        Arc::clone(&link),
        Arc::clone(&local_reason),
        timing.close_linger,
    ));
    tokio::spawn(statistics(
        connection.clone(),
        Arc::clone(&link),
        timing.stats_interval,
    ));
    let reason = read(&connection, recv, decoder, &link, &events, &local_reason).await;
    link.queue.close();
    let _ = events.send(IoEvent::Closed(reason));
    let _ = tokio::time::timeout(timing.close_linger, endpoint.wait_idle()).await;
}

async fn write(
    mut send: SendStream,
    connection: quinn::Connection,
    link: Arc<Link>,
    local_reason: Arc<Mutex<Option<DisconnectReason>>>,
    linger: Duration,
) {
    // A queue closed without a close request is a client that went away.
    let reason = loop {
        match link.queue.pop().await {
            Some(Outbound::Frame(bytes)) => {
                let written = send.write_all(&bytes).await.is_ok();
                link.taken(bytes.len() as u64);
                if !written {
                    return;
                }
            }
            Some(Outbound::Close(reason)) => break reason,
            None => break DisconnectReason::Closed,
        }
    };
    *local_reason.lock().unwrap_or_else(PoisonError::into_inner) = Some(reason);
    if send.finish().is_ok() {
        let _ = tokio::time::timeout(linger, send.stopped()).await;
    }
    connection.close(close_code(reason), b"");
}

async fn read(
    connection: &quinn::Connection,
    mut recv: RecvStream,
    mut decoder: FrameDecoder,
    link: &Link,
    events: &Sender<IoEvent>,
    local_reason: &Mutex<Option<DisconnectReason>>,
) -> DisconnectReason {
    loop {
        match read_frame(&mut recv, &mut decoder).await {
            Ok(Some(Frame::Batch(messages))) => {
                link.received_now();
                if events.send(IoEvent::Messages(messages)).is_err() {
                    connection.close(close_code(DisconnectReason::Closed), b"");
                    return DisconnectReason::Closed;
                }
            }
            Ok(Some(_)) | Err(StreamEnd::Malformed) => {
                *local_reason.lock().unwrap_or_else(PoisonError::into_inner) =
                    Some(DisconnectReason::Protocol);
                connection.close(close_code(DisconnectReason::Protocol), b"");
                return DisconnectReason::Protocol;
            }
            Ok(None) => {
                let error = connection.closed().await;
                return reason_of(&error, Some(local_reason));
            }
            Err(StreamEnd::Closed(error)) => return reason_of(&error, Some(local_reason)),
        }
    }
}

/// Refreshes the round-trip time and the time of the last datagram, and
/// lets QUIC buffer two congestion windows.
async fn statistics(connection: quinn::Connection, link: Arc<Link>, interval: Duration) {
    let mut datagrams = connection.stats().udp_rx.datagrams;
    loop {
        tokio::time::sleep(interval).await;
        if connection.close_reason().is_some() {
            return;
        }
        let stats = connection.stats();
        if stats.udp_rx.datagrams != datagrams {
            datagrams = stats.udp_rx.datagrams;
            link.received_now();
        }
        link.round_trip_micros
            .store(micros(stats.path.rtt), Ordering::Relaxed);
        let window = connection.congestion_state().window();
        connection.set_send_window(window.saturating_mul(2).max(MIN_SEND_WINDOW));
    }
}

/// Why reading a stream stopped.
enum StreamEnd {
    Closed(ConnectionError),
    Malformed,
}

async fn read_frame(
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

fn reason_of(
    error: &ConnectionError,
    local_reason: Option<&Mutex<Option<DisconnectReason>>>,
) -> DisconnectReason {
    match error {
        ConnectionError::ApplicationClosed(close) => {
            DisconnectReason::from_close_code(close.error_code.into_inner())
        }
        ConnectionError::TimedOut => DisconnectReason::TimedOut,
        ConnectionError::LocallyClosed => local_reason
            .and_then(|reason| *reason.lock().unwrap_or_else(PoisonError::into_inner))
            .unwrap_or(DisconnectReason::Closed),
        ConnectionError::TransportError(_)
        | ConnectionError::ConnectionClosed(_)
        | ConnectionError::VersionMismatch
        | ConnectionError::Reset
        | ConnectionError::CidsExhausted => DisconnectReason::Failed,
    }
}

fn close_code(reason: DisconnectReason) -> VarInt {
    VarInt::from_u32(reason.close_code())
}

fn micros(duration: Duration) -> u64 {
    u64::try_from(duration.as_micros())
        .unwrap_or(NEVER - 1)
        .min(NEVER - 1)
}

fn client_config(
    verifier: Arc<PinVerifier>,
    timing: &ClientTiming,
) -> Result<quinn::ClientConfig, Failure> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut tls = rustls::ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .map_err(|_| Failure::Final(DisconnectReason::Failed))?
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    tls.alpn_protocols = vec![ALPN.to_vec()];
    let crypto =
        QuicClientConfig::try_from(tls).map_err(|_| Failure::Final(DisconnectReason::Failed))?;
    let mut config = quinn::ClientConfig::new(Arc::new(crypto));
    let mut transport = quinn::TransportConfig::default();
    transport
        .max_idle_timeout(quinn::IdleTimeout::try_from(timing.idle_timeout).ok())
        .keep_alive_interval(Some(timing.keep_alive))
        .max_concurrent_bidi_streams(0u32.into())
        .max_concurrent_uni_streams(0u32.into())
        .datagram_receive_buffer_size(None)
        .send_window(INITIAL_SEND_WINDOW);
    config.transport_config(Arc::new(transport));
    Ok(config)
}
