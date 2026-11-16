//! Framing of the game stream and the connection handshake (decision T17).
//!
//! The C++ has no framing of its own: messages are self-delimiting by
//! codec, a batch is their plain concatenation, and the receiver tries to
//! inflate every DirectPlay message and falls back to the raw bytes when
//! that fails (`CDPClient::DispatchMsgs`). None of that is kept. The
//! stream of one connection is a sequence of frames:
//!
//! ```text
//! frame            := kind:u8  length:u32  body[length]
//! kind 0 Batch            body := { message_length:u32 message[message_length] }+
//! kind 1 CompressedBatch  body := raw_length:u32 zlib(Batch body of raw_length bytes)
//! kind 2 Hello            client -> server, first frame of a connection
//! kind 3 Welcome          server -> client, the server accepts the Hello
//! kind 4 Reject           server -> client, then the server closes
//! kind 5 Status           server -> client, answer to a Hello that only asks
//! ```
//!
//! All integers are little-endian. A message is the two-byte header and
//! the fields of one game message ([`super::wire`]). Compression is
//! explicit and applies to batches only (decision T13): the sender
//! deflates a batch body of at least [`COMPRESS_THRESHOLD`] bytes and
//! keeps the result when it is smaller, as the C++ server did.
//!
//! On QUIC the connection's application protocol is [`ALPN`]. The handshake
//! carries the protocol version, the build, the session password (which
//! the C++ client never sent) and the account name, the machine-derived
//! MD5 text of decision T16.

use core::fmt;

use flate2::{Compress, Compression as Level, Decompress, FlushCompress, FlushDecompress, Status};

use super::wire::{HEADER_LEN, WireError, WireReader, WireWriter};

/// The QUIC application protocol name; the suffix is the protocol version.
pub const ALPN: &[u8] = b"outpost-frontier/1";

/// Version of the framing, the handshake and the message catalog. Raise it
/// together with [`ALPN`] whenever any of them changes.
pub const PROTOCOL_VERSION: u16 = 1;

/// Bytes in front of every frame body: the kind and the length.
pub const FRAME_HEADER_LEN: usize = 5;

/// Largest frame body accepted.
pub const MAX_FRAME_LEN: usize = 1 << 20;

/// Largest message: the C++ server's retry encode buffer (`0x19000`).
pub const MAX_MESSAGE_LEN: usize = 0x1_9000;

/// Smallest batch body worth deflating (`CDPNetCodec::Compress`).
pub const COMPRESS_THRESHOLD: usize = 0x20;

/// Largest handshake text field, in bytes of UTF-8.
pub const MAX_TEXT_LEN: usize = 1024;

/// The zlib level of the C++ (`Z_DEFAULT_COMPRESSION`).
const DEFLATE_LEVEL: u32 = 6;

/// The frame kinds.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum FrameKind {
    /// Messages, each behind its length.
    Batch = 0,
    /// A deflated batch body.
    CompressedBatch = 1,
    /// The client's handshake.
    Hello = 2,
    /// The server accepts the handshake.
    Welcome = 3,
    /// The server refuses the handshake.
    Reject = 4,
    /// The server's answer to a status query.
    Status = 5,
}

impl FrameKind {
    /// The kind a byte names.
    #[must_use]
    pub const fn from_byte(byte: u8) -> Option<Self> {
        Some(match byte {
            0 => Self::Batch,
            1 => Self::CompressedBatch,
            2 => Self::Hello,
            3 => Self::Welcome,
            4 => Self::Reject,
            5 => Self::Status,
            _ => return None,
        })
    }
}

/// Why a frame cannot be encoded or decoded.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FrameError {
    /// A kind byte this version does not know.
    UnknownKind(u8),
    /// A frame, batch or message longer than its limit.
    TooLong {
        /// The length.
        length: usize,
        /// The limit.
        max: usize,
    },
    /// A message shorter than its header.
    MessageTooShort(usize),
    /// A batch without messages.
    EmptyBatch,
    /// A compressed body that does not inflate to its stated length.
    BadCompression,
    /// A handshake text that is not UTF-8.
    InvalidText,
    /// A field of the frame body is malformed.
    Wire(WireError),
}

impl fmt::Display for FrameError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnknownKind(kind) => write!(f, "unknown frame kind {kind}"),
            Self::TooLong { length, max } => write!(f, "length {length} exceeds {max}"),
            Self::MessageTooShort(length) => write!(f, "message of {length} bytes has no header"),
            Self::EmptyBatch => f.write_str("batch without messages"),
            Self::BadCompression => f.write_str("compressed batch does not inflate"),
            Self::InvalidText => f.write_str("handshake text is not UTF-8"),
            Self::Wire(error) => write!(f, "malformed frame: {error}"),
        }
    }
}

impl std::error::Error for FrameError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Wire(error) => Some(error),
            _ => None,
        }
    }
}

impl From<WireError> for FrameError {
    fn from(error: WireError) -> Self {
        Self::Wire(error)
    }
}

/// What the client wants from the server.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum HelloIntent {
    /// Log in and play.
    Join,
    /// Only the server's [`ServerStatus`], for the address book (O8); no
    /// login, and the server closes after answering.
    Query,
}

/// The client's handshake, the first frame of a connection.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Hello {
    /// The client's [`PROTOCOL_VERSION`].
    pub protocol_version: u16,
    /// The client's build number.
    pub build: u32,
    /// Join or query.
    pub intent: HelloIntent,
    /// The account name: the machine-derived MD5 text (T16). Empty for a
    /// query.
    pub account: String,
    /// The session password; empty when the server has none.
    pub password: String,
}

/// The server accepts a [`Hello`]; game messages follow.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Welcome {
    /// The server's [`PROTOCOL_VERSION`].
    pub protocol_version: u16,
    /// The server's build number.
    pub build: u32,
}

/// Why the server refuses a [`Hello`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RejectReason {
    /// The protocol versions differ.
    ProtocolVersion,
    /// The builds differ.
    Build,
    /// Wrong session password.
    Password,
    /// No free player slot.
    Full,
    /// The account is banned or locked.
    Banned,
    /// The account is already logged in.
    AlreadyConnected,
    /// The server is not accepting new accounts.
    NewPlayersClosed,
    /// A code this version does not know.
    Other(u8),
}

impl RejectReason {
    /// The reason's code on the wire.
    #[must_use]
    pub const fn code(self) -> u8 {
        match self {
            Self::ProtocolVersion => 1,
            Self::Build => 2,
            Self::Password => 3,
            Self::Full => 4,
            Self::Banned => 5,
            Self::AlreadyConnected => 6,
            Self::NewPlayersClosed => 7,
            Self::Other(code) => code,
        }
    }

    /// The reason a code names.
    #[must_use]
    pub const fn from_code(code: u8) -> Self {
        match code {
            1 => Self::ProtocolVersion,
            2 => Self::Build,
            3 => Self::Password,
            4 => Self::Full,
            5 => Self::Banned,
            6 => Self::AlreadyConnected,
            7 => Self::NewPlayersClosed,
            other => Self::Other(other),
        }
    }
}

/// The server refuses a [`Hello`] and closes the connection.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Reject {
    /// Why.
    pub reason: RejectReason,
    /// The server's [`PROTOCOL_VERSION`].
    pub protocol_version: u16,
    /// The server's build number.
    pub build: u32,
}

/// What a server tells a status query; the fields of the C++ enumeration
/// reply and of the DirectPlay session description.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServerStatus {
    /// The server's build number.
    pub build: u32,
    /// The session name.
    pub name: String,
    /// The server description.
    pub description: String,
    /// Players logged in.
    pub players: u16,
    /// Player slots.
    pub max_players: u16,
    /// Whether joining needs a password.
    pub password_required: bool,
    /// Whether players may fight each other.
    pub pvp: bool,
    /// Whether the server accepts new accounts.
    pub new_players: bool,
    /// The server's ID.
    pub server_id: u32,
    /// The server's signature text.
    pub server_signature: String,
}

/// One decoded frame.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Frame {
    /// Game messages in order, each with its header.
    Batch(Vec<Vec<u8>>),
    /// [`Hello`].
    Hello(Hello),
    /// [`Welcome`].
    Welcome(Welcome),
    /// [`Reject`].
    Reject(Reject),
    /// [`ServerStatus`].
    Status(ServerStatus),
}

/// Whether a batch may be deflated.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Compression {
    /// Always send the batch raw.
    Never,
    /// Deflate a body of at least [`COMPRESS_THRESHOLD`] bytes; send the
    /// result when it is smaller than the raw body.
    IfSmaller,
}

/// Collects messages into one batch frame.
#[derive(Clone, Debug, Default)]
pub struct BatchEncoder {
    body: Vec<u8>,
    count: usize,
}

impl BatchEncoder {
    /// An empty batch.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            body: Vec::new(),
            count: 0,
        }
    }

    /// Messages added so far.
    #[must_use]
    pub const fn message_count(&self) -> usize {
        self.count
    }

    /// Whether no message has been added.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Bytes of the raw batch body so far.
    #[must_use]
    pub fn body_len(&self) -> usize {
        self.body.len()
    }

    /// Whether `message` still fits.
    #[must_use]
    pub fn fits(&self, message: &[u8]) -> bool {
        self.body.len() + 4 + message.len() <= MAX_FRAME_LEN
    }

    /// Adds one encoded message, header included.
    ///
    /// # Errors
    /// [`FrameError::MessageTooShort`] for a message without a header,
    /// [`FrameError::TooLong`] for a message above [`MAX_MESSAGE_LEN`] or
    /// one that would take the batch above [`MAX_FRAME_LEN`].
    pub fn push(&mut self, message: &[u8]) -> Result<(), FrameError> {
        check_message_len(message.len())?;
        if !self.fits(message) {
            return Err(FrameError::TooLong {
                length: self.body.len() + 4 + message.len(),
                max: MAX_FRAME_LEN,
            });
        }
        self.body
            .extend_from_slice(&len_u32(message.len()).to_le_bytes());
        self.body.extend_from_slice(message);
        self.count += 1;
        Ok(())
    }

    /// The frame bytes, raw or compressed as `compression` allows.
    ///
    /// # Errors
    /// [`FrameError::EmptyBatch`] when no message was added.
    pub fn finish(self, compression: Compression) -> Result<Vec<u8>, FrameError> {
        if self.count == 0 {
            return Err(FrameError::EmptyBatch);
        }
        if compression == Compression::IfSmaller
            && self.body.len() >= COMPRESS_THRESHOLD
            && let Some(deflated) = deflate(&self.body)
            && deflated.len() + 4 < self.body.len()
        {
            let mut body = Vec::with_capacity(deflated.len() + 4);
            body.extend_from_slice(&len_u32(self.body.len()).to_le_bytes());
            body.extend_from_slice(&deflated);
            return Ok(frame_bytes(FrameKind::CompressedBatch, &body));
        }
        Ok(frame_bytes(FrameKind::Batch, &self.body))
    }
}

/// One batch frame of `messages`.
///
/// # Errors
/// As [`BatchEncoder::push`] and [`BatchEncoder::finish`].
pub fn encode_batch<'a, I>(messages: I, compression: Compression) -> Result<Vec<u8>, FrameError>
where
    I: IntoIterator<Item = &'a [u8]>,
{
    let mut batch = BatchEncoder::new();
    for message in messages {
        batch.push(message)?;
    }
    batch.finish(compression)
}

/// The frame bytes of a handshake frame.
///
/// # Errors
/// [`FrameError::TooLong`] for a text above [`MAX_TEXT_LEN`] bytes, and
/// [`FrameError::EmptyBatch`] for an empty [`Frame::Batch`], which is
/// encoded raw.
pub fn encode_frame(frame: &Frame) -> Result<Vec<u8>, FrameError> {
    let mut body = WireWriter::with_limit(MAX_FRAME_LEN);
    let kind = match frame {
        Frame::Batch(messages) => {
            return encode_batch(messages.iter().map(Vec::as_slice), Compression::Never);
        }
        Frame::Hello(hello) => {
            body.put_u16(hello.protocol_version)?;
            body.put_u32(hello.build)?;
            body.put_u8(match hello.intent {
                HelloIntent::Join => 0,
                HelloIntent::Query => 1,
            })?;
            put_text(&mut body, &hello.account)?;
            put_text(&mut body, &hello.password)?;
            FrameKind::Hello
        }
        Frame::Welcome(welcome) => {
            body.put_u16(welcome.protocol_version)?;
            body.put_u32(welcome.build)?;
            FrameKind::Welcome
        }
        Frame::Reject(reject) => {
            body.put_u8(reject.reason.code())?;
            body.put_u16(reject.protocol_version)?;
            body.put_u32(reject.build)?;
            FrameKind::Reject
        }
        Frame::Status(status) => {
            body.put_u32(status.build)?;
            put_text(&mut body, &status.name)?;
            put_text(&mut body, &status.description)?;
            body.put_u16(status.players)?;
            body.put_u16(status.max_players)?;
            let flags = u8::from(status.password_required)
                | u8::from(status.pvp) << 1
                | u8::from(status.new_players) << 2;
            body.put_u8(flags)?;
            body.put_u32(status.server_id)?;
            put_text(&mut body, &status.server_signature)?;
            FrameKind::Status
        }
    };
    Ok(frame_bytes(kind, body.as_bytes()))
}

/// Decodes the frame at the start of `input`. `Ok(None)` means the frame
/// is not complete yet; otherwise the frame and the bytes it took.
///
/// # Errors
/// Any [`FrameError`]; the connection should be closed, since the stream
/// cannot be resynchronised.
pub fn decode_frame(input: &[u8]) -> Result<Option<(Frame, usize)>, FrameError> {
    let Some((&kind, rest)) = input.split_first() else {
        return Ok(None);
    };
    let kind = FrameKind::from_byte(kind).ok_or(FrameError::UnknownKind(kind))?;
    let Some(length) = rest.get(..4) else {
        return Ok(None);
    };
    let length = usize::try_from(u32::from_le_bytes([
        length[0], length[1], length[2], length[3],
    ]))
    .unwrap_or(usize::MAX);
    if length > MAX_FRAME_LEN {
        return Err(FrameError::TooLong {
            length,
            max: MAX_FRAME_LEN,
        });
    }
    let Some(body) = input.get(FRAME_HEADER_LEN..FRAME_HEADER_LEN + length) else {
        return Ok(None);
    };
    let frame = decode_body(kind, body)?;
    Ok(Some((frame, FRAME_HEADER_LEN + length)))
}

/// Splits a byte stream into frames: push what the connection delivers,
/// then take frames until `Ok(None)`.
#[derive(Clone, Debug, Default)]
pub struct FrameDecoder {
    buffer: Vec<u8>,
    start: usize,
}

impl FrameDecoder {
    /// An empty decoder.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            buffer: Vec::new(),
            start: 0,
        }
    }

    /// Bytes received and not yet taken as a frame.
    #[must_use]
    pub fn buffered(&self) -> usize {
        self.buffer.len() - self.start
    }

    /// Appends received bytes.
    pub fn push(&mut self, bytes: &[u8]) {
        if self.start > 0 && self.start == self.buffer.len() {
            self.buffer.clear();
            self.start = 0;
        }
        self.buffer.extend_from_slice(bytes);
    }

    /// The next complete frame, if any.
    ///
    /// # Errors
    /// Any [`FrameError`]; the decoder then stays failed at that frame.
    pub fn next_frame(&mut self) -> Result<Option<Frame>, FrameError> {
        let pending = self.buffer.get(self.start..).unwrap_or_default();
        let Some((frame, used)) = decode_frame(pending)? else {
            if self.start > 0 {
                self.buffer.drain(..self.start);
                self.start = 0;
            }
            return Ok(None);
        };
        self.start += used;
        Ok(Some(frame))
    }
}

fn decode_body(kind: FrameKind, body: &[u8]) -> Result<Frame, FrameError> {
    match kind {
        FrameKind::Batch => decode_batch(body).map(Frame::Batch),
        FrameKind::CompressedBatch => {
            let mut reader = WireReader::new(body);
            let raw_length = usize::try_from(reader.get_u32()?).unwrap_or(usize::MAX);
            if raw_length > MAX_FRAME_LEN {
                return Err(FrameError::TooLong {
                    length: raw_length,
                    max: MAX_FRAME_LEN,
                });
            }
            let raw = inflate(reader.rest(), raw_length).ok_or(FrameError::BadCompression)?;
            decode_batch(&raw).map(Frame::Batch)
        }
        FrameKind::Hello => {
            let mut reader = WireReader::new(body);
            let protocol_version = reader.get_u16()?;
            let build = reader.get_u32()?;
            let intent = match reader.get_u8()? {
                0 => HelloIntent::Join,
                1 => HelloIntent::Query,
                _ => return Err(WireError::Invalid("hello intent").into()),
            };
            let account = get_text(&mut reader)?;
            let password = get_text(&mut reader)?;
            reader.finish()?;
            Ok(Frame::Hello(Hello {
                protocol_version,
                build,
                intent,
                account,
                password,
            }))
        }
        FrameKind::Welcome => {
            let mut reader = WireReader::new(body);
            let protocol_version = reader.get_u16()?;
            let build = reader.get_u32()?;
            reader.finish()?;
            Ok(Frame::Welcome(Welcome {
                protocol_version,
                build,
            }))
        }
        FrameKind::Reject => {
            let mut reader = WireReader::new(body);
            let reason = RejectReason::from_code(reader.get_u8()?);
            let protocol_version = reader.get_u16()?;
            let build = reader.get_u32()?;
            reader.finish()?;
            Ok(Frame::Reject(Reject {
                reason,
                protocol_version,
                build,
            }))
        }
        FrameKind::Status => {
            let mut reader = WireReader::new(body);
            let build = reader.get_u32()?;
            let name = get_text(&mut reader)?;
            let description = get_text(&mut reader)?;
            let players = reader.get_u16()?;
            let max_players = reader.get_u16()?;
            let flags = reader.get_u8()?;
            if flags & !7 != 0 {
                return Err(WireError::Invalid("status flags").into());
            }
            let server_id = reader.get_u32()?;
            let server_signature = get_text(&mut reader)?;
            reader.finish()?;
            Ok(Frame::Status(ServerStatus {
                build,
                name,
                description,
                players,
                max_players,
                password_required: flags & 1 != 0,
                pvp: flags & 2 != 0,
                new_players: flags & 4 != 0,
                server_id,
                server_signature,
            }))
        }
    }
}

fn decode_batch(body: &[u8]) -> Result<Vec<Vec<u8>>, FrameError> {
    let mut reader = WireReader::new(body);
    let mut messages = Vec::new();
    while !reader.is_empty() {
        let length = usize::try_from(reader.get_u32()?).unwrap_or(usize::MAX);
        check_message_len(length)?;
        messages.push(reader.get_bytes(length)?.to_vec());
    }
    if messages.is_empty() {
        return Err(FrameError::EmptyBatch);
    }
    Ok(messages)
}

fn check_message_len(length: usize) -> Result<(), FrameError> {
    if length < HEADER_LEN {
        Err(FrameError::MessageTooShort(length))
    } else if length > MAX_MESSAGE_LEN {
        Err(FrameError::TooLong {
            length,
            max: MAX_MESSAGE_LEN,
        })
    } else {
        Ok(())
    }
}

fn put_text(writer: &mut WireWriter, text: &str) -> Result<(), FrameError> {
    if text.len() > MAX_TEXT_LEN {
        return Err(FrameError::TooLong {
            length: text.len(),
            max: MAX_TEXT_LEN,
        });
    }
    writer.put_bytes_u16(text.as_bytes())?;
    Ok(())
}

fn get_text(reader: &mut WireReader<'_>) -> Result<String, FrameError> {
    let bytes = reader.get_bytes_u16()?;
    if bytes.len() > MAX_TEXT_LEN {
        return Err(FrameError::TooLong {
            length: bytes.len(),
            max: MAX_TEXT_LEN,
        });
    }
    String::from_utf8(bytes.to_vec()).map_err(|_| FrameError::InvalidText)
}

/// `length` as the `u32` of a length field. Every caller has checked it
/// against [`MAX_FRAME_LEN`].
fn len_u32(length: usize) -> u32 {
    u32::try_from(length).unwrap_or(u32::MAX)
}

fn frame_bytes(kind: FrameKind, body: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(FRAME_HEADER_LEN + body.len());
    out.push(kind as u8);
    out.extend_from_slice(&len_u32(body.len()).to_le_bytes());
    out.extend_from_slice(body);
    out
}

fn deflate(raw: &[u8]) -> Option<Vec<u8>> {
    let mut compress = Compress::new(Level::new(DEFLATE_LEVEL), true);
    // A zlib stream is never more than the input plus a small overhead.
    let mut out = Vec::with_capacity(raw.len() + raw.len() / 1000 + 64);
    match compress.compress_vec(raw, &mut out, FlushCompress::Finish) {
        Ok(Status::StreamEnd) => Some(out),
        _ => None,
    }
}

/// Inflates a complete zlib stream that must yield exactly `raw_length`
/// bytes and end where the input ends.
fn inflate(compressed: &[u8], raw_length: usize) -> Option<Vec<u8>> {
    let mut decompress = Decompress::new(true);
    let mut out = Vec::with_capacity(raw_length);
    let status = decompress
        .decompress_vec(compressed, &mut out, FlushDecompress::Finish)
        .ok()?;
    let consumed = usize::try_from(decompress.total_in()).ok()?;
    (status == Status::StreamEnd && out.len() == raw_length && consumed == compressed.len())
        .then_some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn message(channel: u8, opcode: u8, payload: &[u8]) -> Vec<u8> {
        let mut out = vec![opcode, channel];
        out.extend_from_slice(payload);
        out
    }

    fn decode_all(bytes: &[u8]) -> Result<Vec<Frame>, FrameError> {
        let mut frames = Vec::new();
        let mut rest = bytes;
        while !rest.is_empty() {
            let Some((frame, used)) = decode_frame(rest)? else {
                panic!("incomplete frame");
            };
            frames.push(frame);
            rest = &rest[used..];
        }
        Ok(frames)
    }

    #[test]
    fn alpn_carries_the_version() {
        let suffix = format!("/{PROTOCOL_VERSION}");
        assert!(ALPN.ends_with(suffix.as_bytes()));
    }

    #[test]
    fn raw_batch_layout() {
        let a = message(1, 1, &[0xaa; 3]);
        let b = message(3, 0x2f, &[]);
        let bytes = encode_batch([a.as_slice(), b.as_slice()], Compression::Never).unwrap();
        assert_eq!(
            bytes,
            [
                0, 15, 0, 0, 0, // kind, length
                5, 0, 0, 0, 1, 1, 0xaa, 0xaa, 0xaa, // first message
                2, 0, 0, 0, 0x2f, 3, // second message
            ]
        );
        assert_eq!(decode_all(&bytes).unwrap(), [Frame::Batch(vec![a, b])]);
    }

    #[test]
    fn compression_is_explicit_and_only_when_smaller() {
        let big = message(2, 4, &[7; 600]);
        let bytes = encode_batch([big.as_slice()], Compression::IfSmaller).unwrap();
        assert_eq!(bytes[0], FrameKind::CompressedBatch as u8);
        assert!(bytes.len() < big.len());
        assert_eq!(
            decode_all(&bytes).unwrap(),
            [Frame::Batch(vec![big.clone()])]
        );
        // Below the threshold the batch stays raw.
        let small = message(2, 4, &[7; 20]);
        let bytes = encode_batch([small.as_slice()], Compression::IfSmaller).unwrap();
        assert_eq!(bytes[0], FrameKind::Batch as u8);
        // Incompressible input stays raw.
        let noise: Vec<u8> = (0..400_u32)
            .map(|i| i.wrapping_mul(2_654_435_761).to_le_bytes()[3])
            .collect();
        let noisy = message(2, 4, &noise);
        let bytes = encode_batch([noisy.as_slice()], Compression::IfSmaller).unwrap();
        assert_eq!(bytes[0], FrameKind::Batch as u8);
        assert_eq!(decode_all(&bytes).unwrap(), [Frame::Batch(vec![noisy])]);
    }

    #[test]
    fn handshake_round_trips() {
        let frames = [
            Frame::Hello(Hello {
                protocol_version: PROTOCOL_VERSION,
                build: 0x0102_0304,
                intent: HelloIntent::Join,
                account: "0123456789abcdef0123456789abcdef".to_owned(),
                password: "s\u{e9}same".to_owned(),
            }),
            Frame::Hello(Hello {
                protocol_version: 9,
                build: 0,
                intent: HelloIntent::Query,
                account: String::new(),
                password: String::new(),
            }),
            Frame::Welcome(Welcome {
                protocol_version: PROTOCOL_VERSION,
                build: 7,
            }),
            Frame::Reject(Reject {
                reason: RejectReason::Password,
                protocol_version: PROTOCOL_VERSION,
                build: 7,
            }),
            Frame::Reject(Reject {
                reason: RejectReason::Other(200),
                protocol_version: 2,
                build: 8,
            }),
            Frame::Status(ServerStatus {
                build: 7,
                name: "Outpost".to_owned(),
                description: "A server".to_owned(),
                players: 3,
                max_players: 16,
                password_required: true,
                pvp: false,
                new_players: true,
                server_id: 0xdead_beef,
                server_signature: "sig".to_owned(),
            }),
        ];
        let mut stream = Vec::new();
        for frame in &frames {
            stream.extend(encode_frame(frame).unwrap());
        }
        assert_eq!(decode_all(&stream).unwrap(), frames);
    }

    #[test]
    fn hello_layout() {
        let hello = Frame::Hello(Hello {
            protocol_version: 1,
            build: 2,
            intent: HelloIntent::Join,
            account: "ab".to_owned(),
            password: "c".to_owned(),
        });
        assert_eq!(
            encode_frame(&hello).unwrap(),
            [
                2, 14, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 2, 0, b'a', b'b', 1, 0, b'c'
            ]
        );
    }

    #[test]
    fn decoder_reassembles_split_input() {
        let a = message(1, 5, &[1, 2, 3]);
        let mut stream = encode_batch([a.as_slice()], Compression::Never).unwrap();
        stream.extend(
            encode_frame(&Frame::Welcome(Welcome {
                protocol_version: 1,
                build: 1,
            }))
            .unwrap(),
        );
        let mut decoder = FrameDecoder::new();
        let mut frames = Vec::new();
        for byte in &stream {
            decoder.push(std::slice::from_ref(byte));
            while let Some(frame) = decoder.next_frame().unwrap() {
                frames.push(frame);
            }
        }
        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0], Frame::Batch(vec![a]));
        assert_eq!(decoder.buffered(), 0);
    }

    #[test]
    fn malformed_frames_are_errors() {
        assert_eq!(decode_frame(&[]), Ok(None));
        assert_eq!(decode_frame(&[0, 1, 0]), Ok(None));
        assert_eq!(decode_frame(&[9]), Err(FrameError::UnknownKind(9)));
        assert!(matches!(
            decode_frame(&[0, 0xff, 0xff, 0xff, 0xff]),
            Err(FrameError::TooLong { .. })
        ));
        assert_eq!(decode_frame(&[0, 0, 0, 0, 0]), Err(FrameError::EmptyBatch));
        // A message shorter than its header.
        assert_eq!(
            decode_frame(&[0, 5, 0, 0, 0, 1, 0, 0, 0, 7]),
            Err(FrameError::MessageTooShort(1))
        );
        // A message length past the end of the frame.
        assert!(matches!(
            decode_frame(&[0, 6, 0, 0, 0, 9, 0, 0, 0, 1, 1]),
            Err(FrameError::Wire(WireError::Underrun { .. }))
        ));
        // A message above the limit.
        let mut huge = vec![0, 4, 0, 0, 0];
        huge.extend(len_u32(MAX_MESSAGE_LEN + 1).to_le_bytes());
        assert!(matches!(
            decode_frame(&huge),
            Err(FrameError::TooLong { .. })
        ));
        // Garbage that claims to be compressed.
        assert_eq!(
            decode_frame(&[1, 7, 0, 0, 0, 3, 0, 0, 0, 1, 2, 3]),
            Err(FrameError::BadCompression)
        );
        // Trailing bytes after a handshake frame.
        let mut welcome = encode_frame(&Frame::Welcome(Welcome {
            protocol_version: 1,
            build: 1,
        }))
        .unwrap();
        welcome[1] += 1;
        welcome.push(0);
        assert_eq!(
            decode_frame(&welcome),
            Err(FrameError::Wire(WireError::TrailingBytes { count: 1 }))
        );
        // Text that is not UTF-8.
        assert_eq!(
            decode_frame(&[2, 12, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0xff, 0, 0]),
            Err(FrameError::InvalidText)
        );
        // An unknown intent.
        assert_eq!(
            decode_frame(&[2, 11, 0, 0, 0, 1, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0]),
            Err(FrameError::Wire(WireError::Invalid("hello intent")))
        );
    }

    #[test]
    fn compressed_length_must_match() {
        let big = message(2, 4, &[7; 600]);
        let good = encode_batch([big.as_slice()], Compression::IfSmaller).unwrap();
        // State one byte more than the stream inflates to.
        let mut bad = good.clone();
        bad[5] = bad[5].wrapping_add(1);
        assert_eq!(decode_frame(&bad), Err(FrameError::BadCompression));
        // A raw length above the limit is refused before inflating.
        let mut bad = good;
        bad[5..9].copy_from_slice(&len_u32(MAX_FRAME_LEN + 1).to_le_bytes());
        assert!(matches!(
            decode_frame(&bad),
            Err(FrameError::TooLong { .. })
        ));
    }

    #[test]
    fn encoder_limits() {
        let mut batch = BatchEncoder::new();
        assert_eq!(batch.push(&[1]), Err(FrameError::MessageTooShort(1)));
        assert!(matches!(
            batch.push(&vec![0; MAX_MESSAGE_LEN + 1]),
            Err(FrameError::TooLong { .. })
        ));
        assert_eq!(
            BatchEncoder::new().finish(Compression::Never),
            Err(FrameError::EmptyBatch)
        );
        for _ in 0..10 {
            batch.push(&vec![0; MAX_MESSAGE_LEN]).unwrap();
        }
        assert!(!batch.fits(&vec![0; MAX_MESSAGE_LEN]));
        assert!(batch.push(&vec![0; MAX_MESSAGE_LEN]).is_err());
        assert_eq!(batch.message_count(), 10);
        let long = "x".repeat(MAX_TEXT_LEN + 1);
        let hello = Frame::Hello(Hello {
            protocol_version: 1,
            build: 1,
            intent: HelloIntent::Join,
            account: long,
            password: String::new(),
        });
        assert!(matches!(
            encode_frame(&hello),
            Err(FrameError::TooLong { .. })
        ));
    }
}
