//! The QUIC client against the QUIC host over 127.0.0.1 (work package
//! P3.8): the handshake and the slot, a refused password, messages both
//! ways across the batch limit and compressed, the certificate pin, a kick
//! and a client disconnect.
//!
//! `neuron-server` is a dev-only dependency of this crate (AGENTS.md). The
//! game's catalog lives in `game-sim`, which this crate may not use, so the
//! messages here are encoded with the catalog's layouts by hand:
//! `SETCASH` (channel 2, a `u32`), `REQUESTLAUNCH` (channel 3, a `u32`)
//! and `REQUEST_PLAYER_STATS` (channel 1, a counted buffer).

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::{Duration, Instant};

use neuron_client::net::{ClientTiming, ConnectConfig, QuicClient, SERVER_CONNECTION};
use neuron_core::net::frame::RejectReason;
use neuron_core::net::pin::{CertificateFingerprint, ServerTrust};
use neuron_core::net::transport::{
    ConnectionId, DisconnectReason, Transport, TransportError, TransportEvent,
};
use neuron_core::net::wire::{MessageHeader, WireError, WireMessage, WireReader, WireWriter};
use neuron_server::net::{HostConfig, HostSettings, HostTiming, QuicServer, ServerIdentity};

const BUILD: u32 = 4321;
const ACCOUNT: &str = "fedcba9876543210fedcba9876543210";
const WAIT: Duration = Duration::from_secs(10);

/// `FLPACKET_SERVER_SETCASH` and `FLPACKET_COMMON_REQUEST_PLAYER_STATS`.
#[derive(Clone, Debug, PartialEq)]
enum ToClient {
    SetCash(u32),
    Stats(Vec<u8>),
}

/// `FLPACKET_CLIENT_REQUESTLAUNCH` and `FLPACKET_COMMON_REQUEST_PLAYER_STATS`.
#[derive(Clone, Debug, PartialEq)]
enum ToServer {
    RequestLaunch(u32),
    Stats(Vec<u8>),
}

const SET_CASH: MessageHeader = MessageHeader::new(2, 0x30);
const REQUEST_LAUNCH: MessageHeader = MessageHeader::new(3, 0x04);
const PLAYER_STATS: MessageHeader = MessageHeader::new(1, 0x18);

impl WireMessage for ToClient {
    fn encode(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        match self {
            Self::SetCash(cash) => {
                writer.put_header(SET_CASH)?;
                writer.put_u32(*cash)
            }
            Self::Stats(data) => {
                writer.put_header(PLAYER_STATS)?;
                writer.put_bytes_u32(data)
            }
        }
    }

    fn decode(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        match reader.get_header()? {
            SET_CASH => Ok(Self::SetCash(reader.get_u32()?)),
            PLAYER_STATS => Ok(Self::Stats(reader.get_bytes_u32()?.to_vec())),
            other => Err(WireError::UnknownMessage(other)),
        }
    }
}

impl WireMessage for ToServer {
    fn encode(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        match self {
            Self::RequestLaunch(base) => {
                writer.put_header(REQUEST_LAUNCH)?;
                writer.put_u32(*base)
            }
            Self::Stats(data) => {
                writer.put_header(PLAYER_STATS)?;
                writer.put_bytes_u32(data)
            }
        }
    }

    fn decode(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        match reader.get_header()? {
            REQUEST_LAUNCH => Ok(Self::RequestLaunch(reader.get_u32()?)),
            PLAYER_STATS => Ok(Self::Stats(reader.get_bytes_u32()?.to_vec())),
            other => Err(WireError::UnknownMessage(other)),
        }
    }
}

type Host = QuicServer<ToClient, ToServer>;
type Client = QuicClient<ToServer, ToClient>;

fn host_on(address: SocketAddr, identity: ServerIdentity, password: &str) -> Host {
    QuicServer::bind(HostConfig {
        bind: address,
        identity,
        build: BUILD,
        settings: HostSettings {
            password: password.to_owned(),
            ..HostSettings::default()
        },
        timing: HostTiming {
            stats_interval: Duration::from_millis(20),
            close_linger: Duration::from_millis(500),
            ..HostTiming::default()
        },
    })
    .unwrap()
}

fn host(password: &str) -> Host {
    host_on(
        SocketAddr::from(([127, 0, 0, 1], 0)),
        ServerIdentity::generate().unwrap(),
        password,
    )
}

fn client(address: SocketAddr, password: &str, trust: ServerTrust) -> Client {
    let mut config = ConnectConfig::new(vec![address], BUILD, ACCOUNT, password, trust);
    config.timing = ClientTiming {
        stats_interval: Duration::from_millis(20),
        close_linger: Duration::from_millis(500),
        ..ClientTiming::default()
    };
    Client::connect(config).unwrap()
}

/// Polls both ends until `done` holds of what they reported.
struct Pump {
    host_events: Vec<TransportEvent<ToServer>>,
    client_events: Vec<TransportEvent<ToClient>>,
}

impl Pump {
    fn new() -> Self {
        Self {
            host_events: Vec::new(),
            client_events: Vec::new(),
        }
    }

    fn until(&mut self, host: &mut Host, client: &mut Client, mut done: impl FnMut(&Self) -> bool) {
        let deadline = Instant::now() + WAIT;
        while !done(self) {
            assert!(Instant::now() < deadline, "not reached: {self:?}");
            let mut idle = true;
            while let Some(event) = host.poll() {
                self.host_events.push(event);
                idle = false;
            }
            while let Some(event) = client.poll() {
                self.client_events.push(event);
                idle = false;
            }
            if idle {
                std::thread::sleep(Duration::from_millis(1));
            }
        }
    }

    fn client_messages(&self) -> Vec<ToClient> {
        self.client_events
            .iter()
            .filter_map(|event| match event {
                TransportEvent::Message { message, .. } => Some(message.clone()),
                _ => None,
            })
            .collect()
    }

    fn host_messages(&self) -> Vec<ToServer> {
        self.host_events
            .iter()
            .filter_map(|event| match event {
                TransportEvent::Message { message, .. } => Some(message.clone()),
                _ => None,
            })
            .collect()
    }
}

impl std::fmt::Debug for Pump {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "{} host events (last {:?}), {} client events (last {:?})",
            self.host_events.len(),
            self.host_events.last(),
            self.client_events.len(),
            self.client_events.last()
        )
    }
}

/// Connects `client` and returns its slot.
fn connect(host: &mut Host, client: &mut Client, pump: &mut Pump) -> ConnectionId {
    pump.until(host, client, |p| {
        !p.host_events.is_empty() && !p.client_events.is_empty()
    });
    assert_eq!(
        pump.client_events.remove(0),
        TransportEvent::Connected(SERVER_CONNECTION)
    );
    match pump.host_events.remove(0) {
        TransportEvent::Connected(id) => id,
        other => panic!("expected a connection, got {other:?}"),
    }
}

#[test]
fn the_handshake_admits_the_client_in_a_slot() {
    let mut host = host("secret");
    let mut first = client(host.local_address(), "secret", ServerTrust::FirstUse);
    let mut pump = Pump::new();
    let id = connect(&mut host, &mut first, &mut pump);
    assert_eq!(id, ConnectionId::new(1));
    assert_eq!(host.peer(id).unwrap().account, ACCOUNT);
    assert_eq!(first.server_build(), Some(BUILD));
    assert_eq!(first.server_address(), Some(host.local_address()));

    let mut second = client(host.local_address(), "secret", ServerTrust::FirstUse);
    let mut pump = Pump::new();
    assert_eq!(
        connect(&mut host, &mut second, &mut pump),
        ConnectionId::new(2)
    );
    assert_eq!(host.connection_count(), 2);
}

#[test]
fn a_wrong_password_is_refused() {
    let mut host = host("secret");
    let mut refused = client(host.local_address(), "wrong", ServerTrust::FirstUse);
    let mut pump = Pump::new();
    pump.until(&mut host, &mut refused, |p| !p.client_events.is_empty());
    assert_eq!(
        pump.client_events,
        [TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::Rejected(RejectReason::Password)
        }]
    );
    assert!(pump.host_events.is_empty());
    assert_eq!(host.connection_count(), 0);
    assert_eq!(
        refused.send(SERVER_CONNECTION, ToServer::RequestLaunch(1)),
        Err(TransportError::UnknownConnection(SERVER_CONNECTION))
    );
}

#[test]
fn messages_travel_both_ways() {
    let mut host = host("");
    let mut client = client(host.local_address(), "", ServerTrust::FirstUse);
    let mut pump = Pump::new();
    let id = connect(&mut host, &mut client, &mut pump);

    // Client to server: each message alone.
    client
        .send(SERVER_CONNECTION, ToServer::RequestLaunch(77))
        .unwrap();
    client
        .send(SERVER_CONNECTION, ToServer::Stats(vec![1, 2, 3]))
        .unwrap();
    pump.until(&mut host, &mut client, |p| p.host_messages().len() == 2);
    assert_eq!(
        pump.host_messages(),
        [ToServer::RequestLaunch(77), ToServer::Stats(vec![1, 2, 3])]
    );
    assert!(
        pump.host_events
            .iter()
            .all(|e| matches!(e, TransportEvent::Message { connection, .. } if *connection == id))
    );

    // Server to client: 200 six-byte messages cross the 0x400-byte cut
    // after 170 (170 * 6 = 1020); the rest wait for the flush.
    for cash in 0..200 {
        host.send(id, ToClient::SetCash(cash)).unwrap();
    }
    let queued = host.link_stats(id).unwrap().queued_messages;
    assert!(queued == 30 || queued == 31, "{queued}");
    // A large message goes alone, deflated.
    let large = ToClient::Stats(vec![0x5a; 0x3000]);
    host.send(id, large.clone()).unwrap();
    host.flush();
    pump.until(&mut host, &mut client, |p| p.client_messages().len() == 201);
    let mut expected: Vec<ToClient> = (0..200).map(ToClient::SetCash).collect();
    expected.push(large);
    assert_eq!(pump.client_messages(), expected);
    assert!(
        client
            .link_stats(SERVER_CONNECTION)
            .unwrap()
            .round_trip
            .is_some()
    );
    assert!(host.link_stats(id).unwrap().round_trip.is_some());
}

#[test]
fn the_first_connect_pins_the_certificate() {
    let identity = ServerIdentity::generate().unwrap();
    let mut host = host_on(SocketAddr::from(([127, 0, 0, 1], 0)), identity.clone(), "");
    let address = host.local_address();
    // The client's pin store: server address to fingerprint.
    let mut pins: HashMap<SocketAddr, CertificateFingerprint> = HashMap::new();

    let mut first = client(address, "", ServerTrust::FirstUse);
    let mut pump = Pump::new();
    connect(&mut host, &mut first, &mut pump);
    pins.insert(address, first.server_fingerprint().unwrap());
    assert_eq!(pins[&address], identity.fingerprint());

    let mut again = client(address, "", ServerTrust::Pinned(pins[&address]));
    let mut pump = Pump::new();
    connect(&mut host, &mut again, &mut pump);
    drop(first);
    drop(again);

    // The server comes back on the same address with a new certificate.
    drop(host);
    let mut host = host_on(address, ServerIdentity::generate().unwrap(), "");
    let mut changed = client(address, "", ServerTrust::Pinned(pins[&address]));
    let mut pump = Pump::new();
    pump.until(&mut host, &mut changed, |p| !p.client_events.is_empty());
    assert_eq!(
        pump.client_events,
        [TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::CertificateChanged
        }]
    );
    assert_eq!(changed.server_fingerprint(), Some(host.fingerprint()));
    assert_ne!(host.fingerprint(), pins[&address]);
    assert!(pump.host_events.is_empty());
}

#[test]
fn a_kick_and_a_disconnect_deliver_what_was_sent_first() {
    let mut host = host("");
    let mut kicked = client(host.local_address(), "", ServerTrust::FirstUse);
    let mut pump = Pump::new();
    let id = connect(&mut host, &mut kicked, &mut pump);
    for cash in 0..50 {
        host.send(id, ToClient::Stats(vec![cash as u8; 100]))
            .unwrap();
    }
    host.disconnect(id, DisconnectReason::Kicked).unwrap();
    assert_eq!(
        host.poll(),
        Some(TransportEvent::Disconnected {
            connection: id,
            reason: DisconnectReason::Kicked
        })
    );
    pump.until(&mut host, &mut kicked, |p| {
        matches!(
            p.client_events.last(),
            Some(TransportEvent::Disconnected { .. })
        )
    });
    assert_eq!(
        pump.client_messages(),
        (0..50)
            .map(|v| ToClient::Stats(vec![v; 100]))
            .collect::<Vec<_>>()
    );
    assert_eq!(
        pump.client_events.last(),
        Some(&TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::Kicked
        })
    );

    // The slot is free again; a client that leaves sends its last messages
    // first.
    let mut leaving = client(host.local_address(), "", ServerTrust::FirstUse);
    let mut pump = Pump::new();
    let id = connect(&mut host, &mut leaving, &mut pump);
    assert_eq!(id, ConnectionId::new(1));
    for base in 0..5 {
        leaving
            .send(SERVER_CONNECTION, ToServer::RequestLaunch(base))
            .unwrap();
    }
    leaving
        .disconnect(SERVER_CONNECTION, DisconnectReason::Closed)
        .unwrap();
    pump.until(&mut host, &mut leaving, |p| {
        matches!(
            p.host_events.last(),
            Some(TransportEvent::Disconnected { .. })
        )
    });
    assert_eq!(
        pump.host_messages(),
        (0..5).map(ToServer::RequestLaunch).collect::<Vec<_>>()
    );
    assert_eq!(
        pump.host_events.last(),
        Some(&TransportEvent::Disconnected {
            connection: id,
            reason: DisconnectReason::Closed
        })
    );
    assert_eq!(
        pump.client_events,
        [TransportEvent::Disconnected {
            connection: SERVER_CONNECTION,
            reason: DisconnectReason::Closed
        }]
    );
}
