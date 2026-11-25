//! The QUIC client over 127.0.0.1 (work package P3.8): the handshake and
//! a refusal, the candidate addresses, the certificate pin, messages both
//! ways, the disconnects, the timeout and the status query.
//!
//! The server is a plain quinn endpoint written here from the framing of
//! `neuron_core::net::frame`, so the client is checked against the
//! protocol, including what the host never sends; `quic_end_to_end` runs
//! it against the host itself.

use std::collections::VecDeque;
use std::net::{SocketAddr, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use neuron_client::net::{
    ClientTiming, ConnectConfig, QuicClient, SERVER_CONNECTION, StatusQuery, verify,
};
use neuron_core::net::frame::{
    ALPN, Compression, Frame, FrameDecoder, Hello, HelloIntent, PROTOCOL_VERSION, Reject,
    RejectReason, ServerStatus, Welcome, encode_batch, encode_frame,
};
use neuron_core::net::pin::{CertificateFingerprint, ServerTrust};
use neuron_core::net::transport::{DisconnectReason, Transport, TransportError, TransportEvent};
use neuron_core::net::wire::{MessageHeader, WireError, WireMessage, WireReader, WireWriter};
use quinn::crypto::rustls::QuicServerConfig;
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use tokio::runtime::Runtime;

const BUILD: u32 = 1234;
const ACCOUNT: &str = "0123456789abcdef0123456789abcdef";
const WAIT: Duration = Duration::from_secs(10);

/// A stand-in for the message catalog.
#[derive(Clone, Debug, PartialEq)]
enum Sample {
    Ping(u32),
    Blob(Vec<u8>),
}

impl WireMessage for Sample {
    fn encode(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        match self {
            Self::Ping(value) => {
                writer.put_header(MessageHeader::new(1, 9))?;
                writer.put_u32(*value)
            }
            Self::Blob(bytes) => {
                writer.put_header(MessageHeader::new(1, 10))?;
                writer.put_bytes_u32(bytes)
            }
        }
    }

    fn decode(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        match reader.get_header()? {
            MessageHeader {
                channel: 1,
                opcode: 9,
            } => Ok(Self::Ping(reader.get_u32()?)),
            MessageHeader {
                channel: 1,
                opcode: 10,
            } => Ok(Self::Blob(reader.get_bytes_u32()?.to_vec())),
            other => Err(WireError::UnknownMessage(other)),
        }
    }
}

type Client = QuicClient<Sample, Sample>;

fn runtime() -> Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

fn timing() -> ClientTiming {
    ClientTiming {
        idle_timeout: Duration::from_secs(5),
        keep_alive: Duration::from_millis(100),
        handshake_timeout: Duration::from_secs(5),
        close_linger: Duration::from_millis(500),
        stats_interval: Duration::from_millis(20),
    }
}

fn config(candidates: Vec<SocketAddr>, trust: ServerTrust) -> ConnectConfig {
    let mut config = ConnectConfig::new(candidates, BUILD, ACCOUNT, "secret", trust);
    config.timing = timing();
    config
}

/// One accepted connection with its stream.
struct Session {
    connection: quinn::Connection,
    send: quinn::SendStream,
    recv: quinn::RecvStream,
    decoder: FrameDecoder,
    hello: Hello,
    bytes_read: usize,
}

impl Session {
    async fn frame(&mut self) -> Option<Frame> {
        let mut buffer = [0; 4096];
        loop {
            if let Some(frame) = self.decoder.next_frame().unwrap() {
                return Some(frame);
            }
            match self.recv.read(&mut buffer).await {
                Ok(Some(length)) => {
                    self.bytes_read += length;
                    self.decoder.push(&buffer[..length]);
                }
                Ok(None) | Err(_) => return None,
            }
        }
    }

    async fn messages(&mut self) -> Vec<Sample> {
        match self.frame().await {
            Some(Frame::Batch(messages)) => messages
                .iter()
                .map(|bytes| Sample::from_wire(bytes).unwrap())
                .collect(),
            other => panic!("expected a batch, got {other:?}"),
        }
    }

    async fn write(&mut self, frame: &Frame) {
        self.send
            .write_all(&encode_frame(frame).unwrap())
            .await
            .unwrap();
    }

    async fn write_raw(&mut self, bytes: &[u8]) {
        self.send.write_all(bytes).await.unwrap();
    }

    async fn welcome(&mut self) {
        self.write(&Frame::Welcome(Welcome {
            protocol_version: PROTOCOL_VERSION,
            build: BUILD,
        }))
        .await;
    }

    /// Finishes the stream, waits for it to be acknowledged, closes.
    async fn close(mut self, reason: DisconnectReason) {
        let _ = self.send.finish();
        let _ = self.send.stopped().await;
        self.connection.close(reason.close_code().into(), b"");
    }

    async fn close_reason(&self) -> DisconnectReason {
        match self.connection.closed().await {
            quinn::ConnectionError::ApplicationClosed(close) => {
                DisconnectReason::from_close_code(close.error_code.into_inner())
            }
            quinn::ConnectionError::TimedOut => DisconnectReason::TimedOut,
            other => panic!("unexpected end {other:?}"),
        }
    }
}

/// The stand-in server: accepts every connection in the background.
struct RawServer {
    address: SocketAddr,
    fingerprint: CertificateFingerprint,
    accepted: Arc<Mutex<VecDeque<quinn::Connection>>>,
    _endpoint: quinn::Endpoint,
}

impl RawServer {
    fn new(rt: &Runtime) -> Self {
        let certified = rcgen::generate_simple_self_signed(vec!["server".to_owned()]).unwrap();
        let certificate = certified.cert.der().to_vec();
        let key = certified.signing_key.serialize_der();
        let fingerprint = verify::fingerprint_of(&certificate);
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let mut tls = rustls::ServerConfig::builder_with_provider(provider)
            .with_protocol_versions(&[&rustls::version::TLS13])
            .unwrap()
            .with_no_client_auth()
            .with_single_cert(
                vec![CertificateDer::from(certificate)],
                PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key)),
            )
            .unwrap();
        tls.alpn_protocols = vec![ALPN.to_vec()];
        let mut config =
            quinn::ServerConfig::with_crypto(Arc::new(QuicServerConfig::try_from(tls).unwrap()));
        let mut transport = quinn::TransportConfig::default();
        transport
            .max_idle_timeout(Some(Duration::from_secs(5).try_into().unwrap()))
            .keep_alive_interval(Some(Duration::from_millis(100)));
        config.transport_config(Arc::new(transport));
        let _guard = rt.enter();
        let endpoint =
            quinn::Endpoint::server(config, SocketAddr::from(([127, 0, 0, 1], 0))).unwrap();
        let accepted = Arc::new(Mutex::new(VecDeque::new()));
        {
            let endpoint = endpoint.clone();
            let accepted = Arc::clone(&accepted);
            rt.spawn(async move {
                while let Some(incoming) = endpoint.accept().await {
                    if let Ok(connection) = incoming.await {
                        accepted.lock().unwrap().push_back(connection);
                    }
                }
            });
        }
        Self {
            address: endpoint.local_addr().unwrap(),
            fingerprint,
            accepted,
            _endpoint: endpoint,
        }
    }

    /// The next connection's stream and `Hello`.
    fn session(&self, rt: &Runtime) -> Session {
        let deadline = Instant::now() + WAIT;
        let connection = loop {
            if let Some(connection) = self.accepted.lock().unwrap().pop_front() {
                break connection;
            }
            assert!(Instant::now() < deadline, "no connection");
            idle(rt);
        };
        rt.block_on(async move {
            let (send, recv) = connection.accept_bi().await.unwrap();
            let mut session = Session {
                connection,
                send,
                recv,
                decoder: FrameDecoder::new(),
                hello: Hello {
                    protocol_version: 0,
                    build: 0,
                    intent: HelloIntent::Join,
                    account: String::new(),
                    password: String::new(),
                },
                bytes_read: 0,
            };
            let Some(Frame::Hello(hello)) = session.frame().await else {
                panic!("expected a hello");
            };
            session.hello = hello;
            session
        })
    }

    fn connections_waiting(&self) -> usize {
        self.accepted.lock().unwrap().len()
    }
}

fn idle(rt: &Runtime) {
    rt.block_on(async { tokio::time::sleep(Duration::from_millis(1)).await });
}

/// Polls `client` until it reports an event, letting the server's runtime
/// run between polls.
fn next_event(rt: &Runtime, client: &mut Client) -> TransportEvent<Sample> {
    let deadline = Instant::now() + WAIT;
    loop {
        if let Some(event) = client.poll() {
            return event;
        }
        assert!(Instant::now() < deadline, "no event from the client");
        idle(rt);
    }
}

fn connected(rt: &Runtime, server: &RawServer) -> (Client, Session) {
    let mut client = Client::connect(config(
        vec![server.address],
        ServerTrust::Pinned(server.fingerprint),
    ))
    .unwrap();
    let mut session = server.session(rt);
    rt.block_on(session.welcome());
    assert_eq!(
        next_event(rt, &mut client),
        TransportEvent::Connected(SERVER_CONNECTION)
    );
    (client, session)
}

#[test]
fn the_handshake_opens_the_game_stream() {
    let rt = runtime();
    let server = RawServer::new(&rt);
    let mut client = Client::connect(config(vec![server.address], ServerTrust::FirstUse)).unwrap();
    assert!(!client.is_open());
    assert_eq!(
        client.send(SERVER_CONNECTION, Sample::Ping(0)),
        Err(TransportError::UnknownConnection(SERVER_CONNECTION))
    );
    let mut session = server.session(&rt);
    assert_eq!(
        session.hello,
        Hello {
            protocol_version: PROTOCOL_VERSION,
            build: BUILD,
            intent: HelloIntent::Join,
            account: ACCOUNT.to_owned(),
            password: "secret".to_owned(),
        }
    );
    rt.block_on(session.welcome());
    assert_eq!(
        next_event(&rt, &mut client),
        TransportEvent::Connected(SERVER_CONNECTION)
    );
    assert!(client.is_open());
    assert_eq!(client.server_address(), Some(server.address));
    assert_eq!(client.server_build(), Some(BUILD));
    // First use: the fingerprint to keep is the server's.
    assert_eq!(client.server_fingerprint(), Some(server.fingerprint));
    let stats = client.link_stats(SERVER_CONNECTION).unwrap();
    assert!(stats.round_trip.is_some());
    assert!(client.since_last_receive().unwrap() < WAIT);
}

#[test]
fn messages_travel_both_ways() {
    let rt = runtime();
    let server = RawServer::new(&rt);
    let (mut client, mut session) = connected(&rt, &server);

    // Each client message is its own raw batch, even a compressible one.
    let blob = Sample::Blob(vec![0; 200]);
    client.send(SERVER_CONNECTION, Sample::Ping(1)).unwrap();
    client.send(SERVER_CONNECTION, blob.clone()).unwrap();
    let before = session.bytes_read;
    assert_eq!(rt.block_on(session.messages()), [Sample::Ping(1)]);
    assert_eq!(rt.block_on(session.messages()), [blob]);
    assert_eq!(session.bytes_read - before, (5 + 4 + 6) + (5 + 4 + 206));
    assert!(matches!(
        client.send(SERVER_CONNECTION, Sample::Blob(vec![0; 0x2800])),
        Err(TransportError::Wire(_))
    ));

    // A raw batch of two and a compressed batch arrive in order.
    let two = encode_batch(
        [
            Sample::Ping(7).to_wire().unwrap().as_slice(),
            Sample::Ping(8).to_wire().unwrap().as_slice(),
        ],
        Compression::Never,
    )
    .unwrap();
    let large = Sample::Blob(vec![3; 0x4000]);
    let compressed = encode_batch(
        [large.to_wire().unwrap().as_slice()],
        Compression::IfSmaller,
    )
    .unwrap();
    assert!(compressed.len() < 0x1000);
    rt.block_on(async {
        session.write_raw(&two).await;
        session.write_raw(&compressed).await;
    });
    for message in [Sample::Ping(7), Sample::Ping(8), large] {
        assert_eq!(
            next_event(&rt, &mut client),
            TransportEvent::Message {
                connection: SERVER_CONNECTION,
                message
            }
        );
    }
    assert_eq!(
        client.link_stats(SERVER_CONNECTION).unwrap().queued_bytes,
        0
    );
}

#[test]
fn a_refusal_ends_the_attempts() {
    let rt = runtime();
    let refusing = RawServer::new(&rt);
    let other = RawServer::new(&rt);
    let mut client = Client::connect(config(
        vec![refusing.address, other.address],
        ServerTrust::FirstUse,
    ))
    .unwrap();
    let mut session = refusing.session(&rt);
    rt.block_on(async {
        session
            .write(&Frame::Reject(Reject {
                reason: RejectReason::Password,
                protocol_version: PROTOCOL_VERSION,
                build: BUILD,
            }))
            .await;
    });
    assert_eq!(
        next_event(&rt, &mut client),
        TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::Rejected(RejectReason::Password)
        }
    );
    assert_eq!(client.poll(), None);
    assert!(!client.is_open());
    assert_eq!(other.connections_waiting(), 0);
}

#[test]
fn later_candidates_are_tried() {
    let rt = runtime();
    // A socket that never answers.
    let silent = UdpSocket::bind("127.0.0.1:0").unwrap();
    let server = RawServer::new(&rt);
    let mut connect = config(
        vec![silent.local_addr().unwrap(), server.address],
        ServerTrust::FirstUse,
    );
    connect.timing.handshake_timeout = Duration::from_millis(200);
    let mut client = Client::connect(connect).unwrap();
    let mut session = server.session(&rt);
    rt.block_on(session.welcome());
    assert_eq!(
        next_event(&rt, &mut client),
        TransportEvent::Connected(SERVER_CONNECTION)
    );
    assert_eq!(client.server_address(), Some(server.address));

    // With no candidate answering, the last failure is reported.
    let mut connect = config(vec![silent.local_addr().unwrap()], ServerTrust::FirstUse);
    connect.timing.handshake_timeout = Duration::from_millis(100);
    let mut lonely = Client::connect(connect).unwrap();
    assert_eq!(
        next_event(&rt, &mut lonely),
        TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::TimedOut
        }
    );
}

#[test]
fn a_pin_refuses_another_certificate() {
    let rt = runtime();
    let server = RawServer::new(&rt);
    let impostor = RawServer::new(&rt);
    assert_ne!(server.fingerprint, impostor.fingerprint);
    let mut client = Client::connect(config(
        vec![impostor.address, server.address],
        ServerTrust::Pinned(server.fingerprint),
    ))
    .unwrap();
    assert_eq!(
        next_event(&rt, &mut client),
        TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::CertificateChanged
        }
    );
    // The certificate shown, for the warning; the other candidate is not
    // tried.
    assert_eq!(client.server_fingerprint(), Some(impostor.fingerprint));
    assert_eq!(server.connections_waiting(), 0);

    // The pinned certificate connects.
    let (client, _session) = connected(&rt, &server);
    assert_eq!(client.server_fingerprint(), Some(server.fingerprint));
}

#[test]
fn a_kick_arrives_after_the_messages_before_it() {
    let rt = runtime();
    let server = RawServer::new(&rt);
    let (mut client, mut session) = connected(&rt, &server);
    rt.block_on(async {
        for value in 0..50 {
            let batch = encode_batch(
                [Sample::Blob(vec![value; 300]).to_wire().unwrap().as_slice()],
                Compression::Never,
            )
            .unwrap();
            session.write_raw(&batch).await;
        }
    });
    let closing = rt.spawn(session.close(DisconnectReason::Kicked));
    let mut received = Vec::new();
    let reason = loop {
        match next_event(&rt, &mut client) {
            TransportEvent::Message { message, .. } => received.push(message),
            TransportEvent::Disconnected { reason, .. } => break reason,
            other => panic!("unexpected {other:?}"),
        }
    };
    assert_eq!(reason, DisconnectReason::Kicked);
    assert_eq!(
        received,
        (0..50)
            .map(|v| Sample::Blob(vec![v; 300]))
            .collect::<Vec<_>>()
    );
    rt.block_on(closing).unwrap();
}

#[test]
fn a_disconnect_delivers_what_was_sent_first() {
    let rt = runtime();
    let server = RawServer::new(&rt);
    let (mut client, mut session) = connected(&rt, &server);
    for value in 0..3 {
        client.send(SERVER_CONNECTION, Sample::Ping(value)).unwrap();
    }
    client
        .disconnect(SERVER_CONNECTION, DisconnectReason::Closed)
        .unwrap();
    assert_eq!(
        client.poll(),
        Some(TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::Closed
        })
    );
    assert_eq!(client.poll(), None);
    assert!(
        client
            .disconnect(SERVER_CONNECTION, DisconnectReason::Closed)
            .is_err()
    );
    for value in 0..3 {
        assert_eq!(rt.block_on(session.messages()), [Sample::Ping(value)]);
    }
    assert_eq!(rt.block_on(session.frame()), None);
    assert_eq!(
        rt.block_on(session.close_reason()),
        DisconnectReason::Closed
    );

    // Dropping a client closes its connection the same way.
    let (client, session) = connected(&rt, &server);
    drop(client);
    assert_eq!(
        rt.block_on(session.close_reason()),
        DisconnectReason::Closed
    );
}

#[test]
fn an_undecodable_message_closes_the_connection() {
    let rt = runtime();
    let server = RawServer::new(&rt);
    let (mut client, mut session) = connected(&rt, &server);
    let batch = encode_batch([[9_u8, 9, 9].as_slice()], Compression::Never).unwrap();
    rt.block_on(session.write_raw(&batch));
    assert_eq!(
        next_event(&rt, &mut client),
        TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::Protocol
        }
    );
    assert_eq!(
        rt.block_on(session.close_reason()),
        DisconnectReason::Protocol
    );
}

/// A UDP relay between the client and a server that can stop forwarding.
struct Relay {
    address: SocketAddr,
    blocked: Arc<AtomicBool>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl Relay {
    fn new(server: SocketAddr) -> Self {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        socket
            .set_read_timeout(Some(Duration::from_millis(5)))
            .unwrap();
        let address = socket.local_addr().unwrap();
        let blocked = Arc::new(AtomicBool::new(false));
        let stop = Arc::new(AtomicBool::new(false));
        let thread = {
            let blocked = Arc::clone(&blocked);
            let stop = Arc::clone(&stop);
            std::thread::spawn(move || {
                let mut client = None;
                let mut buffer = [0; 65536];
                while !stop.load(Ordering::Relaxed) {
                    let Ok((length, from)) = socket.recv_from(&mut buffer) else {
                        continue;
                    };
                    if blocked.load(Ordering::Relaxed) {
                        continue;
                    }
                    let to = if from == server {
                        match client {
                            Some(client) => client,
                            None => continue,
                        }
                    } else {
                        client = Some(from);
                        server
                    };
                    let _ = socket.send_to(&buffer[..length], to);
                }
            })
        };
        Self {
            address,
            blocked,
            stop,
            thread: Some(thread),
        }
    }
}

impl Drop for Relay {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            thread.join().unwrap();
        }
    }
}

#[test]
fn a_silent_server_times_out() {
    let rt = runtime();
    let server = RawServer::new(&rt);
    let relay = Relay::new(server.address);
    let mut connect = config(vec![relay.address], ServerTrust::FirstUse);
    connect.timing.idle_timeout = Duration::from_millis(400);
    let mut client = Client::connect(connect).unwrap();
    let mut session = server.session(&rt);
    rt.block_on(session.welcome());
    assert_eq!(
        next_event(&rt, &mut client),
        TransportEvent::Connected(SERVER_CONNECTION)
    );
    relay.blocked.store(true, Ordering::Relaxed);
    let started = Instant::now();
    assert_eq!(
        next_event(&rt, &mut client),
        TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::TimedOut
        }
    );
    assert!(started.elapsed() >= Duration::from_millis(200));
    assert!(client.since_last_receive().unwrap() >= Duration::from_millis(200));
}

#[test]
fn a_status_query_reads_the_status() {
    let rt = runtime();
    let server = RawServer::new(&rt);
    let mut query =
        StatusQuery::start(server.address, BUILD, ServerTrust::FirstUse, timing()).unwrap();
    let mut session = server.session(&rt);
    assert_eq!(session.hello.intent, HelloIntent::Query);
    assert!(session.hello.account.is_empty() && session.hello.password.is_empty());
    let status = ServerStatus {
        build: BUILD,
        name: "Test Server".to_owned(),
        description: "A description".to_owned(),
        players: 3,
        max_players: 16,
        password_required: true,
        pvp: false,
        new_players: true,
        server_id: 9,
        server_signature: "sig".to_owned(),
    };
    rt.block_on(async {
        session.write(&Frame::Status(status.clone())).await;
    });
    let closing = rt.spawn(session.close(DisconnectReason::Closed));
    let deadline = Instant::now() + WAIT;
    let reply = loop {
        if let Some(reply) = query.poll() {
            break reply.unwrap();
        }
        assert!(Instant::now() < deadline, "no status");
        idle(&rt);
    };
    assert_eq!(reply.status, status);
    assert_eq!(reply.fingerprint, Some(server.fingerprint));
    assert!(reply.round_trip > Duration::ZERO);
    assert!(query.poll().is_none());
    rt.block_on(closing).unwrap();
}
