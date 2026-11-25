//! The transport interface and the loopback transport (decisions T9,
//! T17).
//!
//! The game and server threads drive a transport by polling it once per
//! tick, as the C++ drains `CDPMsgList` in `DispatchMsgs`; nothing calls
//! back into game code from another thread. [`Transport::send`] and
//! [`Transport::poll`] never block. The QUIC transports of `neuron-server`
//! and `neuron-client` (work package P3.8) implement the trait with one
//! I/O thread that owns the async runtime and channels to the polling
//! thread; they encode messages through [`WireMessage`] and the framing in
//! [`super::frame`].
//!
//! Single player uses [`LoopbackTransport`]: the in-process server and the
//! client exchange typed messages without encoding, which replaces
//! `rpclocal`'s queues of packet objects. [`loopback_wire_checked`] forces
//! an encode and decode of every message for tests.

use core::fmt;
use std::collections::VecDeque;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::time::Duration;

use super::frame::{Compression, Frame, FrameDecoder, FrameError, RejectReason, encode_batch};
use super::wire::{WireError, WireMessage};

/// One connection of a transport. A server numbers its clients; a client
/// has one connection, to its server. The game's own client IDs (the
/// `CONNECTRESPONSE` slot) are separate.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ConnectionId(u32);

impl ConnectionId {
    /// The connection numbered `id`.
    #[must_use]
    pub const fn new(id: u32) -> Self {
        Self(id)
    }

    /// The number.
    #[must_use]
    pub const fn get(self) -> u32 {
        self.0
    }
}

impl fmt::Display for ConnectionId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "#{}", self.0)
    }
}

/// Why a connection ended or never opened.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum DisconnectReason {
    /// One side closed it normally, or dropped its end.
    Closed,
    /// The server removed the client (`ForceClientLogout`).
    Kicked,
    /// The peer stopped answering.
    TimedOut,
    /// The peer sent bytes that are not a valid frame or message.
    Protocol,
    /// The server refused the handshake.
    Rejected(RejectReason),
    /// The network or the connection attempt failed.
    Failed,
    /// The server presented a certificate other than the pinned one
    /// (decision T16); the client did not log in.
    CertificateChanged,
}

/// The QUIC application close code of a normal close.
const CLOSE_CLOSED: u32 = 0;
/// The close code of a kick.
const CLOSE_KICKED: u32 = 1;
/// The close code of a protocol violation.
const CLOSE_PROTOCOL: u32 = 2;
/// The close code of a failure the closing side reports.
const CLOSE_FAILED: u32 = 3;
/// The close code of a timeout the closing side detected.
const CLOSE_TIMED_OUT: u32 = 4;
/// The close code of a pin mismatch, sent by the client.
const CLOSE_CERTIFICATE: u32 = 5;
/// Added to a [`RejectReason`] code for the close after a refused
/// handshake.
const CLOSE_REJECTED: u32 = 0x100;

impl DisconnectReason {
    /// The QUIC application error code a side closes a connection with, so
    /// the peer reports the same reason (decision T17).
    #[must_use]
    pub const fn close_code(self) -> u32 {
        match self {
            Self::Closed => CLOSE_CLOSED,
            Self::Kicked => CLOSE_KICKED,
            Self::Protocol => CLOSE_PROTOCOL,
            Self::Failed => CLOSE_FAILED,
            Self::TimedOut => CLOSE_TIMED_OUT,
            Self::CertificateChanged => CLOSE_CERTIFICATE,
            Self::Rejected(reason) => CLOSE_REJECTED | reason.code() as u32,
        }
    }

    /// The reason a peer's close code names; an unknown code is
    /// [`DisconnectReason::Failed`].
    #[must_use]
    pub const fn from_close_code(code: u64) -> Self {
        if code >= CLOSE_REJECTED as u64 && code <= (CLOSE_REJECTED | 0xff) as u64 {
            return Self::Rejected(RejectReason::from_code((code & 0xff) as u8));
        }
        match code {
            0 => Self::Closed,
            1 => Self::Kicked,
            2 => Self::Protocol,
            4 => Self::TimedOut,
            5 => Self::CertificateChanged,
            _ => Self::Failed,
        }
    }
}

/// What [`Transport::poll`] reports.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TransportEvent<M> {
    /// A connection opened: on a server a new client, on a client its
    /// server after the handshake.
    Connected(ConnectionId),
    /// A connection ended; it reports nothing more and accepts no sends.
    Disconnected {
        /// The connection.
        connection: ConnectionId,
        /// Why.
        reason: DisconnectReason,
    },
    /// A message arrived, in order.
    Message {
        /// Its connection.
        connection: ConnectionId,
        /// The message.
        message: M,
    },
}

/// Why a send or disconnect was refused.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TransportError {
    /// No open connection has this ID.
    UnknownConnection(ConnectionId),
    /// The message cannot be encoded or decoded.
    Wire(WireError),
    /// The message cannot be framed or unframed.
    Frame(FrameError),
}

impl fmt::Display for TransportError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnknownConnection(id) => write!(f, "no open connection {id}"),
            Self::Wire(error) => write!(f, "message encoding: {error}"),
            Self::Frame(error) => write!(f, "message framing: {error}"),
        }
    }
}

impl std::error::Error for TransportError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::UnknownConnection(_) => None,
            Self::Wire(error) => Some(error),
            Self::Frame(error) => Some(error),
        }
    }
}

impl From<WireError> for TransportError {
    fn from(error: WireError) -> Self {
        Self::Wire(error)
    }
}

impl From<FrameError> for TransportError {
    fn from(error: FrameError) -> Self {
        Self::Frame(error)
    }
}

/// The state of one connection's link, cheap to read every tick. The
/// server's governor stops object updates when the link saturates (T17).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LinkStats {
    /// Messages sent and not yet delivered.
    pub queued_messages: usize,
    /// Encoded bytes sent and not yet delivered; zero where messages are
    /// not encoded.
    pub queued_bytes: u64,
    /// The round-trip time, where the transport measures one.
    pub round_trip: Option<Duration>,
}

/// A message transport polled by the game or server thread.
///
/// `Outgoing` and `Incoming` are the message types of the two directions;
/// a server sends server messages and receives client messages, a client
/// the reverse.
pub trait Transport {
    /// Messages this side sends.
    type Outgoing;
    /// Messages this side receives.
    type Incoming;

    /// Queues `message` for `connection`. Never blocks.
    ///
    /// # Errors
    /// [`TransportError::UnknownConnection`] when the connection is not
    /// open, or an encoding error.
    fn send(
        &mut self,
        connection: ConnectionId,
        message: Self::Outgoing,
    ) -> Result<(), TransportError>;

    /// The next event, or `None` when nothing is pending. Never blocks.
    fn poll(&mut self) -> Option<TransportEvent<Self::Incoming>>;

    /// Closes `connection`; messages already sent are delivered first. The
    /// local side reports one [`TransportEvent::Disconnected`] as well.
    ///
    /// # Errors
    /// [`TransportError::UnknownConnection`] when the connection is not
    /// open.
    fn disconnect(
        &mut self,
        connection: ConnectionId,
        reason: DisconnectReason,
    ) -> Result<(), TransportError>;

    /// The link state of an open connection.
    fn link_stats(&self, connection: ConnectionId) -> Option<LinkStats>;

    /// Hands every message queued by [`Transport::send`] to the network.
    /// A transport that batches sends a batch when it is full, here, and
    /// at its next [`Transport::poll`]; one that does not has nothing to
    /// do.
    fn flush(&mut self) {}

    /// `GetLinkSaturation`: how loaded the link to `connection` is, from
    /// the send backlog (decision T17). The server's observers stop
    /// sending object updates at 0.9. A transport without a governor, and
    /// a connection that is not open, answer 0, as `rpclocal` does.
    fn link_saturation(&self, connection: ConnectionId) -> f64 {
        let _ = connection;
        0.0
    }
}

/// The one connection of each loopback end.
pub const LOOPBACK_CONNECTION: ConnectionId = ConnectionId(1);

enum Envelope<M> {
    Message(M),
    Disconnect(DisconnectReason),
}

/// Re-creates a message from its encoding; [`loopback_wire_checked`].
type RoundTrip<M> = fn(M) -> Result<M, TransportError>;

/// One end of an in-process connection: sends `Out`, receives `In`.
/// [`loopback`] makes the pair. The ends may live on different threads
/// when the messages are `Send`.
pub struct LoopbackTransport<Out, In> {
    outgoing: Option<Sender<Envelope<Out>>>,
    incoming: Receiver<Envelope<In>>,
    events: VecDeque<TransportEvent<In>>,
    /// Messages this end sent that the peer has not polled yet.
    sent: Arc<AtomicUsize>,
    /// Messages the peer sent that this end has not polled yet.
    received: Arc<AtomicUsize>,
    round_trip: Option<RoundTrip<Out>>,
    open: bool,
}

impl<Out, In> fmt::Debug for LoopbackTransport<Out, In> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("LoopbackTransport")
            .field("open", &self.open)
            .field("queued", &self.sent.load(Ordering::Relaxed))
            .field("wire_checked", &self.round_trip.is_some())
            .finish_non_exhaustive()
    }
}

/// A connected pair: the client end sends `C` and receives `S`, the server
/// end the reverse. Messages pass as values, without encoding. Each end
/// first reports [`TransportEvent::Connected`] for [`LOOPBACK_CONNECTION`].
#[must_use]
pub fn loopback<C, S>() -> (LoopbackTransport<C, S>, LoopbackTransport<S, C>) {
    pair(None, None)
}

/// As [`loopback`], but every message is encoded, framed as a batch
/// (compressed when that is smaller), unframed and decoded on its way, so
/// single-player tests run the codecs of the multiplayer path. A message
/// that fails makes `send` return the error.
#[must_use]
pub fn loopback_wire_checked<C, S>() -> (LoopbackTransport<C, S>, LoopbackTransport<S, C>)
where
    C: WireMessage,
    S: WireMessage,
{
    pair(Some(wire_round_trip::<C>), Some(wire_round_trip::<S>))
}

fn pair<C, S>(
    client_check: Option<RoundTrip<C>>,
    server_check: Option<RoundTrip<S>>,
) -> (LoopbackTransport<C, S>, LoopbackTransport<S, C>) {
    let (to_server, from_client) = mpsc::channel();
    let (to_client, from_server) = mpsc::channel();
    let client_queue = Arc::new(AtomicUsize::new(0));
    let server_queue = Arc::new(AtomicUsize::new(0));
    let client = LoopbackTransport::new(
        to_server,
        from_server,
        Arc::clone(&client_queue),
        Arc::clone(&server_queue),
        client_check,
    );
    let server = LoopbackTransport::new(
        to_client,
        from_client,
        server_queue,
        client_queue,
        server_check,
    );
    (client, server)
}

impl<Out, In> LoopbackTransport<Out, In> {
    fn new(
        outgoing: Sender<Envelope<Out>>,
        incoming: Receiver<Envelope<In>>,
        sent: Arc<AtomicUsize>,
        received: Arc<AtomicUsize>,
        round_trip: Option<RoundTrip<Out>>,
    ) -> Self {
        Self {
            outgoing: Some(outgoing),
            incoming,
            events: VecDeque::from([TransportEvent::Connected(LOOPBACK_CONNECTION)]),
            sent,
            received,
            round_trip,
            open: true,
        }
    }

    /// Whether every message is encoded and decoded on its way.
    #[must_use]
    pub const fn is_wire_checked(&self) -> bool {
        self.round_trip.is_some()
    }

    fn check_open(&self, connection: ConnectionId) -> Result<(), TransportError> {
        if self.open && connection == LOOPBACK_CONNECTION {
            Ok(())
        } else {
            Err(TransportError::UnknownConnection(connection))
        }
    }

    fn close(&mut self, reason: DisconnectReason) -> TransportEvent<In> {
        self.open = false;
        self.outgoing = None;
        TransportEvent::Disconnected {
            connection: LOOPBACK_CONNECTION,
            reason,
        }
    }
}

impl<Out, In> Transport for LoopbackTransport<Out, In> {
    type Outgoing = Out;
    type Incoming = In;

    fn send(&mut self, connection: ConnectionId, message: Out) -> Result<(), TransportError> {
        self.check_open(connection)?;
        let message = match self.round_trip {
            Some(round_trip) => round_trip(message)?,
            None => message,
        };
        // Counted before the hand-over so the peer never counts below zero.
        self.sent.fetch_add(1, Ordering::Relaxed);
        let delivered = self
            .outgoing
            .as_ref()
            .is_some_and(|tx| tx.send(Envelope::Message(message)).is_ok());
        if !delivered {
            self.sent.fetch_sub(1, Ordering::Relaxed);
        }
        // A peer that has gone is reported by `poll`.
        Ok(())
    }

    fn poll(&mut self) -> Option<TransportEvent<In>> {
        if let Some(event) = self.events.pop_front() {
            return Some(event);
        }
        if !self.open {
            return None;
        }
        match self.incoming.try_recv() {
            Ok(Envelope::Message(message)) => {
                self.received.fetch_sub(1, Ordering::Relaxed);
                Some(TransportEvent::Message {
                    connection: LOOPBACK_CONNECTION,
                    message,
                })
            }
            Ok(Envelope::Disconnect(reason)) => Some(self.close(reason)),
            Err(TryRecvError::Empty) => None,
            Err(TryRecvError::Disconnected) => Some(self.close(DisconnectReason::Closed)),
        }
    }

    fn disconnect(
        &mut self,
        connection: ConnectionId,
        reason: DisconnectReason,
    ) -> Result<(), TransportError> {
        self.check_open(connection)?;
        if let Some(tx) = &self.outgoing {
            // The peer may have gone already; then there is no one to tell.
            let _ = tx.send(Envelope::Disconnect(reason));
        }
        let event = self.close(reason);
        self.events.push_back(event);
        Ok(())
    }

    fn link_stats(&self, connection: ConnectionId) -> Option<LinkStats> {
        self.check_open(connection).ok()?;
        Some(LinkStats {
            queued_messages: self.sent.load(Ordering::Relaxed),
            queued_bytes: 0,
            round_trip: Some(Duration::ZERO),
        })
    }
}

/// Encodes `message`, frames it as a batch, unframes and decodes it.
fn wire_round_trip<M: WireMessage>(message: M) -> Result<M, TransportError> {
    let bytes = message.to_wire()?;
    let framed = encode_batch([bytes.as_slice()], Compression::IfSmaller)?;
    let mut decoder = FrameDecoder::new();
    decoder.push(&framed);
    let frame = decoder.next_frame()?;
    let Some(Frame::Batch(messages)) = frame else {
        return Err(TransportError::Frame(FrameError::EmptyBatch));
    };
    let [only] = messages.as_slice() else {
        return Err(TransportError::Frame(FrameError::EmptyBatch));
    };
    Ok(M::from_wire(only)?)
}

#[cfg(test)]
mod tests {
    use super::super::wire::{MessageHeader, WireReader, WireWriter, quantise};
    use super::*;

    /// A stand-in for the game-sim catalog: two messages, one lossy.
    #[derive(Clone, Debug, PartialEq)]
    enum Sample {
        Ping(u32),
        Turn { object: u32, quaternion: [f32; 4] },
    }

    impl WireMessage for Sample {
        fn encode(&self, writer: &mut WireWriter) -> Result<(), WireError> {
            match self {
                Self::Ping(value) => {
                    writer.put_header(MessageHeader::new(1, 9))?;
                    writer.put_u32(*value)
                }
                Self::Turn { object, quaternion } => {
                    writer.put_header(MessageHeader::new(1, 1))?;
                    writer.put_u32(*object)?;
                    for component in quaternion {
                        writer.put_u8(quantise::quantise_unit(*component))?;
                    }
                    Ok(())
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
                    opcode: 1,
                } => {
                    let object = reader.get_u32()?;
                    let mut quaternion = [0.0; 4];
                    for component in &mut quaternion {
                        *component = quantise::expand_unit(reader.get_u8()?);
                    }
                    Ok(Self::Turn { object, quaternion })
                }
                other => Err(WireError::UnknownMessage(other)),
            }
        }
    }

    /// A message type without a wire encoding.
    #[derive(Debug, PartialEq)]
    struct Opaque(Vec<u8>);

    fn drain<Out, In>(end: &mut LoopbackTransport<Out, In>) -> Vec<TransportEvent<In>> {
        std::iter::from_fn(|| end.poll()).collect()
    }

    #[test]
    fn typed_messages_pass_unencoded() {
        let (mut client, mut server) = loopback::<Opaque, Sample>();
        assert!(!client.is_wire_checked());
        assert_eq!(
            drain(&mut server),
            [TransportEvent::Connected(LOOPBACK_CONNECTION)]
        );
        assert_eq!(
            drain(&mut client),
            [TransportEvent::Connected(LOOPBACK_CONNECTION)]
        );
        client
            .send(LOOPBACK_CONNECTION, Opaque(vec![1, 2]))
            .unwrap();
        client.send(LOOPBACK_CONNECTION, Opaque(vec![3])).unwrap();
        assert_eq!(
            client
                .link_stats(LOOPBACK_CONNECTION)
                .unwrap()
                .queued_messages,
            2
        );
        assert_eq!(
            drain(&mut server),
            [
                TransportEvent::Message {
                    connection: LOOPBACK_CONNECTION,
                    message: Opaque(vec![1, 2])
                },
                TransportEvent::Message {
                    connection: LOOPBACK_CONNECTION,
                    message: Opaque(vec![3])
                },
            ]
        );
        assert_eq!(
            client
                .link_stats(LOOPBACK_CONNECTION)
                .unwrap()
                .queued_messages,
            0
        );
        let exact = Sample::Turn {
            object: 5,
            quaternion: [0.3, 0.1, 0.2, 0.9],
        };
        server.send(LOOPBACK_CONNECTION, exact.clone()).unwrap();
        assert_eq!(
            client.poll(),
            Some(TransportEvent::Message {
                connection: LOOPBACK_CONNECTION,
                message: exact
            })
        );
        assert_eq!(client.poll(), None);
    }

    #[test]
    fn wire_checked_messages_take_the_encoding() {
        let (mut client, mut server) = loopback_wire_checked::<Sample, Sample>();
        assert!(server.is_wire_checked());
        drain(&mut client);
        drain(&mut server);
        client
            .send(
                LOOPBACK_CONNECTION,
                Sample::Turn {
                    object: 5,
                    quaternion: [1.0, -1.0, 0.5, 0.0],
                },
            )
            .unwrap();
        client.send(LOOPBACK_CONNECTION, Sample::Ping(7)).unwrap();
        assert_eq!(
            drain(&mut server),
            [
                TransportEvent::Message {
                    connection: LOOPBACK_CONNECTION,
                    message: Sample::Turn {
                        object: 5,
                        quaternion: [1.0, -1.0, quantise::expand_unit(63), 0.0],
                    }
                },
                TransportEvent::Message {
                    connection: LOOPBACK_CONNECTION,
                    message: Sample::Ping(7)
                },
            ]
        );
    }

    #[test]
    fn disconnect_reaches_both_ends_after_pending_messages() {
        let (mut client, mut server) = loopback::<u32, u32>();
        drain(&mut client);
        drain(&mut server);
        server.send(LOOPBACK_CONNECTION, 1).unwrap();
        server
            .disconnect(LOOPBACK_CONNECTION, DisconnectReason::Kicked)
            .unwrap();
        let gone = TransportEvent::Disconnected {
            connection: LOOPBACK_CONNECTION,
            reason: DisconnectReason::Kicked,
        };
        assert_eq!(drain(&mut server), std::slice::from_ref(&gone));
        assert_eq!(
            drain(&mut client),
            [
                TransportEvent::Message {
                    connection: LOOPBACK_CONNECTION,
                    message: 1
                },
                gone,
            ]
        );
        assert_eq!(
            client.send(LOOPBACK_CONNECTION, 2),
            Err(TransportError::UnknownConnection(LOOPBACK_CONNECTION))
        );
        assert!(server.link_stats(LOOPBACK_CONNECTION).is_none());
        assert!(
            server
                .disconnect(LOOPBACK_CONNECTION, DisconnectReason::Closed)
                .is_err()
        );
        assert_eq!(client.poll(), None);
    }

    #[test]
    fn dropping_an_end_closes_the_other() {
        let (client, mut server) = loopback::<u32, u32>();
        drop(client);
        assert_eq!(
            drain(&mut server),
            [
                TransportEvent::Connected(LOOPBACK_CONNECTION),
                TransportEvent::Disconnected {
                    connection: LOOPBACK_CONNECTION,
                    reason: DisconnectReason::Closed
                },
            ]
        );
        assert_eq!(
            server.send(ConnectionId::new(2), 0),
            Err(TransportError::UnknownConnection(ConnectionId::new(2)))
        );
    }

    #[test]
    fn ends_work_across_threads() {
        let (mut client, mut server) = loopback_wire_checked::<Sample, Sample>();
        let worker = std::thread::spawn(move || {
            let mut echoed = 0;
            while echoed < 100 {
                match server.poll() {
                    Some(TransportEvent::Message {
                        connection,
                        message,
                    }) => {
                        server.send(connection, message).unwrap();
                        echoed += 1;
                    }
                    Some(_) => {}
                    None => std::thread::yield_now(),
                }
            }
        });
        for value in 0..100 {
            client
                .send(LOOPBACK_CONNECTION, Sample::Ping(value))
                .unwrap();
        }
        let mut back = Vec::new();
        while back.len() < 100 {
            match client.poll() {
                Some(TransportEvent::Message { message, .. }) => back.push(message),
                Some(_) => {}
                None => std::thread::yield_now(),
            }
        }
        worker.join().unwrap();
        assert_eq!(back, (0..100).map(Sample::Ping).collect::<Vec<_>>());
    }

    #[test]
    fn close_codes_name_their_reason() {
        let reasons = [
            DisconnectReason::Closed,
            DisconnectReason::Kicked,
            DisconnectReason::TimedOut,
            DisconnectReason::Protocol,
            DisconnectReason::Failed,
            DisconnectReason::CertificateChanged,
            DisconnectReason::Rejected(RejectReason::Password),
            DisconnectReason::Rejected(RejectReason::Other(200)),
        ];
        for reason in reasons {
            assert_eq!(
                DisconnectReason::from_close_code(u64::from(reason.close_code())),
                reason
            );
        }
        assert_eq!(
            DisconnectReason::from_close_code(99),
            DisconnectReason::Failed
        );
        assert_eq!(
            DisconnectReason::from_close_code(0x200),
            DisconnectReason::Failed
        );
    }

    #[test]
    fn the_loopback_has_no_governor() {
        let (mut client, _server) = loopback::<u32, u32>();
        client.flush();
        assert_eq!(client.link_saturation(LOOPBACK_CONNECTION), 0.0);
    }

    #[test]
    fn a_message_that_does_not_encode_is_refused() {
        #[derive(Debug)]
        struct Broken;
        impl WireMessage for Broken {
            fn encode(&self, writer: &mut WireWriter) -> Result<(), WireError> {
                writer.put_u8(1)
            }
            fn decode(_: &mut WireReader<'_>) -> Result<Self, WireError> {
                Ok(Self)
            }
        }
        let (mut client, _server) = loopback_wire_checked::<Broken, Broken>();
        assert_eq!(
            client.send(LOOPBACK_CONNECTION, Broken).unwrap_err(),
            TransportError::Frame(FrameError::MessageTooShort(1))
        );
    }
}
