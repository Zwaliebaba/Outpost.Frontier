//! The QUIC host over 127.0.0.1 (work package P3.8): the handshake and
//! its refusals, messages both ways, batching, the backlog and the
//! saturation, the disconnects and the timeout.
//!
//! The peer is a plain quinn client written here from the framing of
//! `neuron_core::net::frame`, so the host is checked against the protocol
//! rather than against the client crate (which this crate may not use).

use std::net::{SocketAddr, UdpSocket};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use neuron_core::net::frame::{
    ALPN, Compression, Frame, FrameDecoder, Hello, HelloIntent, PROTOCOL_VERSION, RejectReason,
    encode_batch, encode_frame,
};
use neuron_core::net::transport::{
    ConnectionId, DisconnectReason, Transport, TransportError, TransportEvent,
};
use neuron_core::net::wire::{MessageHeader, WireError, WireMessage, WireReader, WireWriter};
use neuron_server::net::{HostConfig, HostSettings, HostTiming, QuicServer, ServerIdentity};
use quinn::crypto::rustls::QuicClientConfig;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{DigitallySignedStruct, SignatureScheme};
use tokio::runtime::Runtime;

const BUILD: u32 = 1234;
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

type Host = QuicServer<Sample, Sample>;

fn fast_timing() -> HostTiming {
    HostTiming {
        idle_timeout: Duration::from_secs(5),
        keep_alive: Duration::from_millis(100),
        handshake_timeout: Duration::from_secs(5),
        close_linger: Duration::from_millis(500),
        stats_interval: Duration::from_millis(20),
        saturation_interval: Duration::from_millis(20),
    }
}

fn host_with(settings: HostSettings, timing: HostTiming) -> Host {
    QuicServer::bind(HostConfig {
        bind: SocketAddr::from(([127, 0, 0, 1], 0)),
        identity: ServerIdentity::generate().unwrap(),
        build: BUILD,
        settings,
        timing,
    })
    .unwrap()
}

fn host() -> Host {
    host_with(HostSettings::default(), fast_timing())
}

fn hello(password: &str) -> Hello {
    Hello {
        protocol_version: PROTOCOL_VERSION,
        build: BUILD,
        intent: HelloIntent::Join,
        account: "0123456789abcdef0123456789abcdef".to_owned(),
        password: password.to_owned(),
    }
}

fn runtime() -> Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

/// Accepts any certificate but still checks the handshake signature.
#[derive(Debug)]
struct AnyCertificate;

impl ServerCertVerifier for AnyCertificate {
    fn verify_server_cert(
        &self,
        _: &CertificateDer<'_>,
        _: &[CertificateDer<'_>],
        _: &ServerName<'_>,
        _: &[u8],
        _: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _: &[u8],
        _: &CertificateDer<'_>,
        _: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Err(rustls::Error::General("no TLS 1.2".to_owned()))
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &rustls::crypto::ring::default_provider().signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        rustls::crypto::ring::default_provider()
            .signature_verification_algorithms
            .supported_schemes()
    }
}

/// The plain client: one connection, its stream and a frame decoder.
struct Peer {
    endpoint: quinn::Endpoint,
    connection: quinn::Connection,
    send: quinn::SendStream,
    recv: quinn::RecvStream,
    decoder: FrameDecoder,
}

fn client_config(receive_window: Option<u32>) -> quinn::ClientConfig {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut tls = rustls::ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .unwrap()
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(AnyCertificate))
        .with_no_client_auth();
    tls.alpn_protocols = vec![ALPN.to_vec()];
    let mut config = quinn::ClientConfig::new(Arc::new(QuicClientConfig::try_from(tls).unwrap()));
    let mut transport = quinn::TransportConfig::default();
    transport
        .max_idle_timeout(Some(Duration::from_secs(5).try_into().unwrap()))
        .keep_alive_interval(Some(Duration::from_millis(100)));
    if let Some(window) = receive_window {
        transport
            .stream_receive_window(window.into())
            .receive_window(window.into());
    }
    config.transport_config(Arc::new(transport));
    config
}

impl Peer {
    /// Connects, sends `hello` and returns the peer and the server's
    /// first frame.
    async fn open(
        address: SocketAddr,
        hello: &Hello,
        receive_window: Option<u32>,
    ) -> (Self, Frame) {
        let endpoint = quinn::Endpoint::client(SocketAddr::from(([127, 0, 0, 1], 0))).unwrap();
        let connection = endpoint
            .connect_with(client_config(receive_window), address, "server")
            .unwrap()
            .await
            .unwrap();
        let (mut send, recv) = connection.open_bi().await.unwrap();
        send.write_all(&encode_frame(&Frame::Hello(hello.clone())).unwrap())
            .await
            .unwrap();
        let mut peer = Self {
            endpoint,
            connection,
            send,
            recv,
            decoder: FrameDecoder::new(),
        };
        let first = peer.frame().await.expect("a first frame");
        (peer, first)
    }

    /// The next frame, `None` at the end of the stream.
    async fn frame(&mut self) -> Option<Frame> {
        let mut buffer = [0; 4096];
        loop {
            if let Some(frame) = self.decoder.next_frame().unwrap() {
                return Some(frame);
            }
            match self.recv.read(&mut buffer).await {
                Ok(Some(length)) => self.decoder.push(&buffer[..length]),
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

    async fn send_raw(&mut self, messages: &[Vec<u8>]) {
        let frame = encode_batch(messages.iter().map(Vec::as_slice), Compression::Never).unwrap();
        self.send.write_all(&frame).await.unwrap();
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

/// Polls `host` until it reports an event, letting the peer's runtime
/// run between polls.
fn next_event(rt: &Runtime, host: &mut Host) -> TransportEvent<Sample> {
    let deadline = Instant::now() + WAIT;
    loop {
        if let Some(event) = host.poll() {
            return event;
        }
        assert!(Instant::now() < deadline, "no event from the host");
        rt.block_on(async { tokio::time::sleep(Duration::from_millis(1)).await });
    }
}

/// Polls until `done` holds, running the peer's runtime between polls.
fn pump_until(rt: &Runtime, host: &mut Host, mut done: impl FnMut(&mut Host) -> bool) {
    let deadline = Instant::now() + WAIT;
    while !done(host) {
        assert!(Instant::now() < deadline, "condition not reached");
        while host.poll().is_some() {}
        rt.block_on(async { tokio::time::sleep(Duration::from_millis(1)).await });
    }
}

#[test]
fn joins_take_the_lowest_free_slot() {
    let rt = runtime();
    let mut host = host();
    let address = host.local_address();

    let (first, welcome) = rt.block_on(Peer::open(address, &hello(""), None));
    let Frame::Welcome(welcome) = welcome else {
        panic!("expected a welcome, got {welcome:?}");
    };
    assert_eq!(welcome.protocol_version, PROTOCOL_VERSION);
    assert_eq!(welcome.build, BUILD);
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Connected(ConnectionId::new(1))
    );
    let peer = host.peer(ConnectionId::new(1)).unwrap();
    assert_eq!(peer.account, "0123456789abcdef0123456789abcdef");
    assert_eq!(peer.address, first.endpoint.local_addr().unwrap());

    let (second, _) = rt.block_on(Peer::open(address, &hello(""), None));
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Connected(ConnectionId::new(2))
    );
    assert_eq!(host.connection_count(), 2);

    // The first client leaves; its slot is the next one given.
    first.connection.close(0u32.into(), b"");
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Disconnected {
            connection: ConnectionId::new(1),
            reason: DisconnectReason::Closed
        }
    );
    let (_third, _) = rt.block_on(Peer::open(address, &hello(""), None));
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Connected(ConnectionId::new(1))
    );
    drop(second);
}

#[test]
fn bad_versions_passwords_and_a_full_host_are_refused() {
    let rt = runtime();
    let settings = HostSettings {
        max_players: 1,
        password: "secret".to_owned(),
        ..HostSettings::default()
    };
    let mut host = host_with(settings, fast_timing());
    let address = host.local_address();

    let refused = |hello: Hello| {
        let (peer, frame) = rt.block_on(Peer::open(address, &hello, None));
        let Frame::Reject(reject) = frame else {
            panic!("expected a refusal, got {frame:?}");
        };
        assert_eq!(reject.protocol_version, PROTOCOL_VERSION);
        assert_eq!(reject.build, BUILD);
        // The host finishes the stream and closes with the reason.
        let mut peer = peer;
        assert_eq!(rt.block_on(peer.frame()), None);
        assert_eq!(
            rt.block_on(peer.close_reason()),
            DisconnectReason::Rejected(reject.reason)
        );
        reject.reason
    };
    let mut old = hello("secret");
    old.protocol_version = PROTOCOL_VERSION + 1;
    assert_eq!(refused(old), RejectReason::ProtocolVersion);
    let mut other_build = hello("secret");
    other_build.build = BUILD + 1;
    assert_eq!(refused(other_build), RejectReason::Build);
    assert_eq!(refused(hello("wrong")), RejectReason::Password);
    assert_eq!(refused(hello("")), RejectReason::Password);
    assert_eq!(host.poll(), None);

    let (_admitted, frame) = rt.block_on(Peer::open(address, &hello("secret"), None));
    assert!(matches!(frame, Frame::Welcome(_)));
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Connected(ConnectionId::new(1))
    );
    assert_eq!(refused(hello("secret")), RejectReason::Full);

    // A new password applies to the next handshake.
    host.set_settings(HostSettings {
        max_players: 2,
        password: String::new(),
        ..host.settings()
    });
    let (_second, frame) = rt.block_on(Peer::open(address, &hello("anything"), None));
    assert!(matches!(frame, Frame::Welcome(_)));
}

#[test]
fn a_query_gets_the_status_without_a_slot() {
    let rt = runtime();
    let settings = HostSettings {
        max_players: 8,
        password: "pw".to_owned(),
        name: "Test Server".to_owned(),
        description: "A description".to_owned(),
        pvp: true,
        new_players: false,
        server_id: 77,
        server_signature: "sig".to_owned(),
    };
    let mut host = host_with(settings, fast_timing());
    let address = host.local_address();
    let (_player, _) = rt.block_on(Peer::open(address, &hello("pw"), None));
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Connected(ConnectionId::new(1))
    );

    let query = Hello {
        build: 1,
        intent: HelloIntent::Query,
        account: String::new(),
        password: String::new(),
        ..hello("")
    };
    let (mut peer, frame) = rt.block_on(Peer::open(address, &query, None));
    let Frame::Status(status) = frame else {
        panic!("expected a status, got {frame:?}");
    };
    assert_eq!(status.build, BUILD);
    assert_eq!(status.name, "Test Server");
    assert_eq!(status.description, "A description");
    assert_eq!((status.players, status.max_players), (1, 8));
    assert!(status.password_required && status.pvp && !status.new_players);
    assert_eq!(
        (status.server_id, status.server_signature.as_str()),
        (77, "sig")
    );
    assert_eq!(rt.block_on(peer.frame()), None);
    assert_eq!(rt.block_on(peer.close_reason()), DisconnectReason::Closed);
    assert_eq!(host.poll(), None);
}

#[test]
fn messages_travel_both_ways_and_batch() {
    let rt = runtime();
    let mut host = host();
    let (mut peer, _) = rt.block_on(Peer::open(host.local_address(), &hello(""), None));
    let id = ConnectionId::new(1);
    assert_eq!(next_event(&rt, &mut host), TransportEvent::Connected(id));

    // One client frame with two messages arrives as two events, in order.
    let messages = [Sample::Ping(1), Sample::Blob(vec![5; 40])];
    let encoded: Vec<Vec<u8>> = messages.iter().map(|m| m.to_wire().unwrap()).collect();
    rt.block_on(peer.send_raw(&encoded));
    for message in messages {
        assert_eq!(
            next_event(&rt, &mut host),
            TransportEvent::Message {
                connection: id,
                message
            }
        );
    }

    // Three small messages sent before a flush go in one batch.
    for value in 10..13 {
        host.send(id, Sample::Ping(value)).unwrap();
    }
    let stats = host.link_stats(id).unwrap();
    assert_eq!(stats.queued_messages, 3);
    assert_eq!(stats.queued_bytes, 18);
    host.flush();
    assert_eq!(
        rt.block_on(peer.messages()),
        [Sample::Ping(10), Sample::Ping(11), Sample::Ping(12)]
    );

    // 0x400 message bytes close a batch; a larger message goes alone,
    // compressed when that is smaller.
    let half = Sample::Blob(vec![1; 0x200 - 8]);
    host.send(id, half.clone()).unwrap();
    host.send(id, half.clone()).unwrap();
    host.send(id, Sample::Ping(20)).unwrap();
    let large = Sample::Blob(vec![2; 0x2000]);
    host.send(id, large.clone()).unwrap();
    assert!(host.poll().is_none());
    assert_eq!(rt.block_on(peer.messages()), [half.clone(), half]);
    assert_eq!(rt.block_on(peer.messages()), [Sample::Ping(20)]);
    assert_eq!(rt.block_on(peer.messages()), [large]);

    // The largest message the C++ server sent, and one past it.
    assert!(host.send(id, Sample::Blob(vec![0; 0x19000 - 6])).is_ok());
    assert!(matches!(
        host.send(id, Sample::Blob(vec![0; 0x19000])),
        Err(TransportError::Wire(_))
    ));
    host.flush();
    assert_eq!(rt.block_on(peer.messages()).len(), 1);
}

#[test]
fn an_undecodable_message_disconnects_the_client() {
    let rt = runtime();
    let mut host = host();
    let (mut peer, _) = rt.block_on(Peer::open(host.local_address(), &hello(""), None));
    let id = ConnectionId::new(1);
    assert_eq!(next_event(&rt, &mut host), TransportEvent::Connected(id));
    rt.block_on(peer.send_raw(&[Sample::Ping(3).to_wire().unwrap(), vec![7, 7, 0]]));
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Message {
            connection: id,
            message: Sample::Ping(3)
        }
    );
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Disconnected {
            connection: id,
            reason: DisconnectReason::Protocol
        }
    );
    assert_eq!(
        host.send(id, Sample::Ping(0)),
        Err(TransportError::UnknownConnection(id))
    );
    assert_eq!(rt.block_on(peer.close_reason()), DisconnectReason::Protocol);
}

#[test]
fn a_kick_delivers_what_was_sent_first() {
    let rt = runtime();
    let mut host = host();
    let (mut peer, _) = rt.block_on(Peer::open(host.local_address(), &hello(""), None));
    let id = ConnectionId::new(1);
    assert_eq!(next_event(&rt, &mut host), TransportEvent::Connected(id));
    for value in 0..100 {
        host.send(id, Sample::Blob(vec![value; 200])).unwrap();
    }
    host.disconnect(id, DisconnectReason::Kicked).unwrap();
    assert_eq!(
        host.poll(),
        Some(TransportEvent::Disconnected {
            connection: id,
            reason: DisconnectReason::Kicked
        })
    );
    assert!(host.link_stats(id).is_none());
    assert!(host.disconnect(id, DisconnectReason::Kicked).is_err());
    let mut received = Vec::new();
    while let Some(frame) = rt.block_on(peer.frame()) {
        let Frame::Batch(messages) = frame else {
            panic!("expected a batch");
        };
        received.extend(messages.iter().map(|m| Sample::from_wire(m).unwrap()));
    }
    assert_eq!(
        received,
        (0..100)
            .map(|v| Sample::Blob(vec![v; 200]))
            .collect::<Vec<_>>()
    );
    assert_eq!(rt.block_on(peer.close_reason()), DisconnectReason::Kicked);
    assert_eq!(host.poll(), None);
}

#[test]
fn a_backlog_saturates_the_link_and_drains() {
    let rt = runtime();
    let mut host = host();
    // A client whose flow-control window holds 2 KiB and which reads
    // nothing until told to.
    let (mut peer, _) = rt.block_on(Peer::open(host.local_address(), &hello(""), Some(2048)));
    let id = ConnectionId::new(1);
    assert_eq!(next_event(&rt, &mut host), TransportEvent::Connected(id));
    assert_eq!(host.link_saturation(id), 0.0);
    assert!(host.link_stats(id).unwrap().round_trip.is_some());

    // Bytes that do not deflate, so the batches stay their size.
    let mut seed = 0x1234_5678_u32;
    let mut noise = || {
        seed = seed.wrapping_mul(1_103_515_245).wrapping_add(12345);
        (seed >> 16) as u8
    };
    for _ in 0..64 {
        let blob = Sample::Blob((0..1000).map(|_| noise()).collect());
        host.send(id, blob).unwrap();
    }
    host.flush();
    // The governor's three samples reach 0.9 once the mean backlog is
    // 1800 bytes.
    pump_until(&rt, &mut host, |host| host.link_saturation(id) >= 0.9);
    let stats = host.link_stats(id).unwrap();
    assert!(stats.queued_bytes > 30_000, "{stats:?}");
    assert!(stats.queued_messages > 0);

    // The client reads; the backlog empties and the saturation falls.
    let mut received = 0;
    while received < 64 {
        received += rt.block_on(peer.messages()).len();
    }
    pump_until(&rt, &mut host, |host| {
        host.link_stats(id).unwrap().queued_bytes == 0
    });
    pump_until(&rt, &mut host, |host| host.link_saturation(id) == 0.0);
    assert_eq!(host.link_stats(id).unwrap().queued_messages, 0);
}

/// A UDP relay between a client and the host that can stop forwarding.
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
fn a_silent_client_times_out() {
    let rt = runtime();
    let timing = HostTiming {
        idle_timeout: Duration::from_millis(400),
        ..fast_timing()
    };
    let mut host = host_with(HostSettings::default(), timing);
    let relay = Relay::new(host.local_address());
    let (peer, _) = rt.block_on(Peer::open(relay.address, &hello(""), None));
    let id = ConnectionId::new(1);
    assert_eq!(next_event(&rt, &mut host), TransportEvent::Connected(id));
    relay.blocked.store(true, Ordering::Relaxed);
    let started = Instant::now();
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Disconnected {
            connection: id,
            reason: DisconnectReason::TimedOut
        }
    );
    assert!(started.elapsed() >= Duration::from_millis(200));
    assert_eq!(rt.block_on(peer.close_reason()), DisconnectReason::TimedOut);
}

#[test]
fn dropping_the_host_closes_its_clients() {
    let rt = runtime();
    let mut host = host();
    let (peer, _) = rt.block_on(Peer::open(host.local_address(), &hello(""), None));
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Connected(ConnectionId::new(1))
    );
    drop(host);
    assert_eq!(rt.block_on(peer.close_reason()), DisconnectReason::Closed);
}

#[test]
fn a_disconnected_client_reports_nothing_more() {
    let rt = runtime();
    let mut host = host();
    let (mut peer, _) = rt.block_on(Peer::open(host.local_address(), &hello(""), None));
    let id = ConnectionId::new(1);
    assert_eq!(next_event(&rt, &mut host), TransportEvent::Connected(id));
    let batch: Vec<Vec<u8>> = (0..3).map(|v| Sample::Ping(v).to_wire().unwrap()).collect();
    rt.block_on(peer.send_raw(&batch));
    assert_eq!(
        next_event(&rt, &mut host),
        TransportEvent::Message {
            connection: id,
            message: Sample::Ping(0)
        }
    );
    // The other two have arrived; after the disconnect they are not
    // reported.
    host.disconnect(id, DisconnectReason::Kicked).unwrap();
    assert_eq!(
        host.poll(),
        Some(TransportEvent::Disconnected {
            connection: id,
            reason: DisconnectReason::Kicked
        })
    );
    assert_eq!(host.poll(), None);
    assert_eq!(rt.block_on(peer.close_reason()), DisconnectReason::Kicked);
}
