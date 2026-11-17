//! Field encodings of the game messages (work package P1.9, decision T17).
//!
//! The C++ codecs (`RemoteCommon/`, `include/rpc_packet_codec.h`) copy
//! every field as raw x86 little-endian bytes through a cursor
//! (`FRpcWireStream`): integers of 1, 2 and 4 bytes, `f32` and `f64` bit
//! patterns, vectors as three `f32`, quaternions as four (`w`, `x`, `y`,
//! `z`), byte runs and UTF-16 runs behind a count. There is no
//! bit-packing; the lossy fields go through the quantisers in
//! [`quantise`].
//!
//! [`WireWriter`] and [`WireReader`] reproduce those encodings. Both check
//! every field before they touch it, as the C++ cursor does (advance, then
//! compare with the limit), and report a failure as a [`WireError`]
//! instead of the C++ sticky error codes `0x16` (encode overflow) and
//! `0x17` (decode overrun). Malformed input never panics.

pub mod quantise;

use core::fmt;

/// Bytes of the message header.
pub const HEADER_LEN: usize = 2;

/// The two-byte header in front of every message: low byte opcode, high
/// byte channel, little-endian (`FRpcWireStream::PutHeader`).
///
/// Channel 1 is common to both directions, 2 is server to client and 3 is
/// client to server (`include/network.h`). The message catalog lives in
/// `game-sim`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct MessageHeader {
    /// The channel, the C++ packet ID's high half.
    pub channel: u8,
    /// The opcode within the channel.
    pub opcode: u8,
}

impl MessageHeader {
    /// A header for `channel` and `opcode`.
    #[must_use]
    pub const fn new(channel: u8, opcode: u8) -> Self {
        Self { channel, opcode }
    }

    /// The header as the little-endian word on the wire.
    #[must_use]
    pub const fn to_word(self) -> u16 {
        u16::from_le_bytes([self.opcode, self.channel])
    }

    /// The header a wire word spells; the receiver splits it as the C++
    /// dispatchers do (`RpcDispatchMsgs`: channel = word >> 8, opcode =
    /// word & 0xff).
    #[must_use]
    pub const fn from_word(word: u16) -> Self {
        let [opcode, channel] = word.to_le_bytes();
        Self { channel, opcode }
    }

    /// The C++ packet ID, `channel << 16 | opcode`.
    #[must_use]
    pub const fn packet_id(self) -> u32 {
        (self.channel as u32) << 16 | self.opcode as u32
    }

    /// The header word the C++ writes for a packet ID:
    /// `(id & 0xfff) | ((id >> 16) & 0xff) << 8`, truncated to 16 bits.
    /// An opcode above `0xff` overlaps the channel byte; no catalog
    /// opcode is that large.
    #[must_use]
    pub const fn word_for_packet_id(packet_id: u32) -> u16 {
        let word = (packet_id & 0xfff) | ((packet_id >> 16) & 0xff) << 8;
        let [low, high, ..] = word.to_le_bytes();
        u16::from_le_bytes([low, high])
    }
}

/// A field that does not fit or cannot be read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum WireError {
    /// The writer's limit would be exceeded; the C++ code `0x16`.
    Overflow {
        /// Bytes the message would need with this field.
        needed: usize,
        /// The writer's limit.
        limit: usize,
    },
    /// The input ends inside a field; the C++ code `0x17`.
    Underrun {
        /// Bytes the field needs.
        needed: usize,
        /// Bytes left in the input.
        remaining: usize,
    },
    /// A count does not fit its prefix, or exceeds a bound the reader
    /// enforces. The C++ truncates such counts (divergence register).
    LengthOutOfRange {
        /// The count.
        length: usize,
        /// The largest count allowed.
        max: usize,
    },
    /// A header that names no message the decoder knows.
    UnknownMessage(MessageHeader),
    /// A message decoded without consuming all of its bytes.
    TrailingBytes {
        /// Bytes left over.
        count: usize,
    },
    /// A field value the decoder rejects.
    Invalid(&'static str),
}

impl WireError {
    /// The C++ codec's error code for the same failure: `0x16` for an
    /// encode overflow and `0x17` for a decode overrun.
    #[must_use]
    pub const fn legacy_code(&self) -> Option<u32> {
        match self {
            Self::Overflow { .. } => Some(0x16),
            Self::Underrun { .. } => Some(0x17),
            _ => None,
        }
    }
}

impl fmt::Display for WireError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Overflow { needed, limit } => {
                write!(f, "message needs {needed} bytes, limit is {limit}")
            }
            Self::Underrun { needed, remaining } => {
                write!(f, "field needs {needed} bytes, {remaining} left")
            }
            Self::LengthOutOfRange { length, max } => {
                write!(f, "count {length} exceeds {max}")
            }
            Self::UnknownMessage(header) => write!(
                f,
                "unknown message: channel {}, opcode {:#04x}",
                header.channel, header.opcode
            ),
            Self::TrailingBytes { count } => write!(f, "{count} bytes after the message"),
            Self::Invalid(what) => write!(f, "invalid field: {what}"),
        }
    }
}

impl std::error::Error for WireError {}

/// A message with a wire encoding: header first, then its fields. The
/// message catalog in `game-sim` implements it; the transports use it to
/// encode, and the loopback transport to check the encoding.
pub trait WireMessage: Sized {
    /// Writes the header and the fields.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the message passes the writer's limit,
    /// or [`WireError::LengthOutOfRange`] for a count that does not fit.
    fn encode(&self, writer: &mut WireWriter) -> Result<(), WireError>;

    /// Reads one message, header included.
    ///
    /// # Errors
    /// Any [`WireError`] for input that is not such a message.
    fn decode(reader: &mut WireReader<'_>) -> Result<Self, WireError>;

    /// The message's bytes.
    ///
    /// # Errors
    /// As [`WireMessage::encode`].
    fn to_wire(&self) -> Result<Vec<u8>, WireError> {
        let mut writer = WireWriter::new();
        self.encode(&mut writer)?;
        Ok(writer.into_bytes())
    }

    /// The message `bytes` hold, which must be all of them.
    ///
    /// # Errors
    /// As [`WireMessage::decode`], and [`WireError::TrailingBytes`] when
    /// the message ends early.
    fn from_wire(bytes: &[u8]) -> Result<Self, WireError> {
        let mut reader = WireReader::new(bytes);
        let message = Self::decode(&mut reader)?;
        reader.finish()?;
        Ok(message)
    }
}

/// The bits an x87 `FLD`/`FSTP` round trip leaves of an `f32`: a
/// signalling NaN comes out quiet (quiet bit set), every other value
/// unchanged. The C++ encoders move some floats through the x87 stack
/// (`FRpcWireStream::PutFloat`) and their decoders copy the bits.
#[must_use]
pub const fn x87_copy_f32(bits: u32) -> u32 {
    const EXPONENT: u32 = 0x7f80_0000;
    const QUIET: u32 = 0x0040_0000;
    const MANTISSA: u32 = 0x007f_ffff;
    if bits & EXPONENT == EXPONENT && bits & MANTISSA != 0 {
        bits | QUIET
    } else {
        bits
    }
}

/// As [`x87_copy_f32`], for an `f64` moved with `FLD`/`FSTP qword`.
#[must_use]
pub const fn x87_copy_f64(bits: u64) -> u64 {
    const EXPONENT: u64 = 0x7ff0_0000_0000_0000;
    const QUIET: u64 = 0x0008_0000_0000_0000;
    const MANTISSA: u64 = 0x000f_ffff_ffff_ffff;
    if bits & EXPONENT == EXPONENT && bits & MANTISSA != 0 {
        bits | QUIET
    } else {
        bits
    }
}

/// Builds one encoded message.
///
/// Every `put_*` checks the limit first and appends nothing on failure,
/// like the C++ `Put` (advance the cursor, compare with the capacity,
/// copy only when it fits). The C++ encode buffers hold 10 240 bytes, and
/// the server retries at 102 400; [`WireWriter::with_limit`] reproduces such
/// a capacity.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WireWriter {
    bytes: Vec<u8>,
    limit: usize,
}

impl Default for WireWriter {
    fn default() -> Self {
        Self::new()
    }
}

impl WireWriter {
    /// A writer without a size limit.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            bytes: Vec::new(),
            limit: usize::MAX,
        }
    }

    /// A writer that refuses to grow beyond `limit` bytes.
    #[must_use]
    pub const fn with_limit(limit: usize) -> Self {
        Self {
            bytes: Vec::new(),
            limit,
        }
    }

    /// The limit given at construction.
    #[must_use]
    pub const fn limit(&self) -> usize {
        self.limit
    }

    /// Bytes written so far.
    #[must_use]
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    /// Whether nothing has been written.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// The bytes written so far.
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// The finished message.
    #[must_use]
    pub fn into_bytes(self) -> Vec<u8> {
        self.bytes
    }

    fn check(&self, width: usize) -> Result<(), WireError> {
        match self.bytes.len().checked_add(width) {
            Some(needed) if needed <= self.limit => Ok(()),
            needed => Err(WireError::Overflow {
                needed: needed.unwrap_or(usize::MAX),
                limit: self.limit,
            }),
        }
    }

    /// Appends `bytes` as one field.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_bytes(&mut self, bytes: &[u8]) -> Result<(), WireError> {
        self.check(bytes.len())?;
        self.bytes.extend_from_slice(bytes);
        Ok(())
    }

    /// The message header.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the limit is below two bytes. The C++
    /// writes the header without a check.
    pub fn put_header(&mut self, header: MessageHeader) -> Result<(), WireError> {
        self.put_u16(header.to_word())
    }

    /// One byte.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_u8(&mut self, value: u8) -> Result<(), WireError> {
        self.put_bytes(&[value])
    }

    /// A little-endian `u16`.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_u16(&mut self, value: u16) -> Result<(), WireError> {
        self.put_bytes(&value.to_le_bytes())
    }

    /// A little-endian `u32`.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_u32(&mut self, value: u32) -> Result<(), WireError> {
        self.put_bytes(&value.to_le_bytes())
    }

    /// A little-endian `u64`.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_u64(&mut self, value: u64) -> Result<(), WireError> {
        self.put_bytes(&value.to_le_bytes())
    }

    /// One signed byte.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_i8(&mut self, value: i8) -> Result<(), WireError> {
        self.put_bytes(&value.to_le_bytes())
    }

    /// A little-endian `i16`.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_i16(&mut self, value: i16) -> Result<(), WireError> {
        self.put_bytes(&value.to_le_bytes())
    }

    /// A little-endian `i32`.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_i32(&mut self, value: i32) -> Result<(), WireError> {
        self.put_bytes(&value.to_le_bytes())
    }

    /// The bit pattern of an `f32`, copied as an integer (`Put`, `memcpy`).
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_f32(&mut self, value: f32) -> Result<(), WireError> {
        self.put_u32(value.to_bits())
    }

    /// An `f32` moved through the x87 stack (`PutFloat`): a signalling NaN
    /// is written quiet ([`x87_copy_f32`]).
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_f32_x87(&mut self, value: f32) -> Result<(), WireError> {
        self.put_u32(x87_copy_f32(value.to_bits()))
    }

    /// The bit pattern of an `f64`, copied as an integer.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_f64(&mut self, value: f64) -> Result<(), WireError> {
        self.put_u64(value.to_bits())
    }

    /// An `f64` moved through the x87 stack (`FLD`/`FSTP qword`, as
    /// `CHARSELECTVERIFIED`): a signalling NaN is written quiet.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_f64_x87(&mut self, value: f64) -> Result<(), WireError> {
        self.put_u64(x87_copy_f64(value.to_bits()))
    }

    /// A vector as three `f32` bit patterns, `x`, `y`, `z`, one 12-byte
    /// field (`RpcWirePutVector`).
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_vector(&mut self, value: [f32; 3]) -> Result<(), WireError> {
        let mut field = [0; 12];
        for (chunk, component) in field.as_chunks_mut::<4>().0.iter_mut().zip(value) {
            *chunk = component.to_bits().to_le_bytes();
        }
        self.put_bytes(&field)
    }

    /// A quaternion as four `f32` bit patterns in the C++ member order
    /// `w`, `x`, `y`, `z`, one 16-byte field.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_quaternion(&mut self, wxyz: [f32; 4]) -> Result<(), WireError> {
        let mut field = [0; 16];
        for (chunk, component) in field.as_chunks_mut::<4>().0.iter_mut().zip(wxyz) {
            *chunk = component.to_bits().to_le_bytes();
        }
        self.put_bytes(&field)
    }

    /// UTF-16 code units, two bytes each, as one field and without a count.
    ///
    /// # Errors
    /// [`WireError::Overflow`] when the field passes the limit.
    pub fn put_wide(&mut self, units: &[u16]) -> Result<(), WireError> {
        let field: Vec<u8> = units.iter().flat_map(|u| u.to_le_bytes()).collect();
        self.put_bytes(&field)
    }

    /// A `u8` count, then that many bytes.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above 255 bytes; otherwise
    /// [`WireError::Overflow`] when a field passes the limit.
    pub fn put_bytes_u8(&mut self, bytes: &[u8]) -> Result<(), WireError> {
        self.put_u8(count(bytes.len(), u8::MAX)?)?;
        self.put_bytes(bytes)
    }

    /// A `u16` count, then that many bytes (character IDs, character
    /// names).
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above 65 535 bytes; otherwise
    /// [`WireError::Overflow`] when a field passes the limit.
    pub fn put_bytes_u16(&mut self, bytes: &[u8]) -> Result<(), WireError> {
        self.put_u16(count(bytes.len(), u16::MAX)?)?;
        self.put_bytes(bytes)
    }

    /// A `u32` count, then that many bytes (the C++ owned blocks).
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above `u32::MAX` bytes; otherwise
    /// [`WireError::Overflow`] when a field passes the limit.
    pub fn put_bytes_u32(&mut self, bytes: &[u8]) -> Result<(), WireError> {
        self.put_u32(count_u32(bytes.len())?)?;
        self.put_bytes(bytes)
    }

    /// A `u8` count of code units, then the units.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above 255 units; otherwise
    /// [`WireError::Overflow`] when a field passes the limit.
    pub fn put_wide_u8(&mut self, units: &[u16]) -> Result<(), WireError> {
        self.put_u8(count(units.len(), u8::MAX)?)?;
        self.put_wide(units)
    }

    /// A `u16` count of code units, then the units (`RpcEncodeWideString`,
    /// `RpcServerPutWideText`).
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above 65 535 units; otherwise
    /// [`WireError::Overflow`] when a field passes the limit.
    pub fn put_wide_u16(&mut self, units: &[u16]) -> Result<(), WireError> {
        self.put_u16(count(units.len(), u16::MAX)?)?;
        self.put_wide(units)
    }
}

/// `length` as a count prefix whose largest value is `max`.
fn count<T: TryFrom<usize>>(length: usize, max: T) -> Result<T, WireError>
where
    usize: From<T>,
{
    T::try_from(length).map_err(|_| WireError::LengthOutOfRange {
        length,
        max: usize::from(max),
    })
}

/// `length` as a `u32` count prefix.
fn count_u32(length: usize) -> Result<u32, WireError> {
    u32::try_from(length).map_err(|_| WireError::LengthOutOfRange {
        length,
        max: usize::try_from(u32::MAX).unwrap_or(usize::MAX),
    })
}

/// Reads the fields of one encoded message.
///
/// A failed read leaves the position where it was. Counted reads take
/// their count from the input and fail with [`WireError::Underrun`] when
/// the input is shorter; the caller bounds any allocation through the
/// input length.
#[derive(Clone, Debug)]
pub struct WireReader<'a> {
    bytes: &'a [u8],
    position: usize,
}

impl<'a> WireReader<'a> {
    /// A reader at the start of `bytes`.
    #[must_use]
    pub const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, position: 0 }
    }

    /// Bytes consumed so far.
    #[must_use]
    pub const fn position(&self) -> usize {
        self.position
    }

    /// Bytes not yet consumed.
    #[must_use]
    pub const fn remaining(&self) -> usize {
        self.bytes.len() - self.position
    }

    /// Whether every byte has been consumed.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.remaining() == 0
    }

    /// The bytes not yet consumed.
    #[must_use]
    pub fn rest(&self) -> &'a [u8] {
        self.bytes.get(self.position..).unwrap_or_default()
    }

    /// Succeeds when every byte has been consumed.
    ///
    /// # Errors
    /// [`WireError::TrailingBytes`] otherwise.
    pub const fn finish(&self) -> Result<(), WireError> {
        match self.remaining() {
            0 => Ok(()),
            count => Err(WireError::TrailingBytes { count }),
        }
    }

    /// The next `count` bytes.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when fewer remain.
    pub fn get_bytes(&mut self, count: usize) -> Result<&'a [u8], WireError> {
        let rest = self.rest();
        let field = rest.get(..count).ok_or(WireError::Underrun {
            needed: count,
            remaining: rest.len(),
        })?;
        self.position += count;
        Ok(field)
    }

    fn get_array<const N: usize>(&mut self) -> Result<[u8; N], WireError> {
        let field = self.get_bytes(N)?;
        let mut out = [0; N];
        out.copy_from_slice(field);
        Ok(out)
    }

    /// The message header.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when fewer than two bytes remain.
    pub fn get_header(&mut self) -> Result<MessageHeader, WireError> {
        self.get_u16().map(MessageHeader::from_word)
    }

    /// One byte.
    ///
    /// # Errors
    /// [`WireError::Underrun`] at the end of the input.
    pub fn get_u8(&mut self) -> Result<u8, WireError> {
        self.get_array::<1>().map(|[b]| b)
    }

    /// A little-endian `u16`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_u16(&mut self) -> Result<u16, WireError> {
        self.get_array().map(u16::from_le_bytes)
    }

    /// A little-endian `u32`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_u32(&mut self) -> Result<u32, WireError> {
        self.get_array().map(u32::from_le_bytes)
    }

    /// A little-endian `u64`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_u64(&mut self) -> Result<u64, WireError> {
        self.get_array().map(u64::from_le_bytes)
    }

    /// One signed byte.
    ///
    /// # Errors
    /// [`WireError::Underrun`] at the end of the input.
    pub fn get_i8(&mut self) -> Result<i8, WireError> {
        self.get_array().map(i8::from_le_bytes)
    }

    /// A little-endian `i16`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_i16(&mut self) -> Result<i16, WireError> {
        self.get_array().map(i16::from_le_bytes)
    }

    /// A little-endian `i32`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_i32(&mut self) -> Result<i32, WireError> {
        self.get_array().map(i32::from_le_bytes)
    }

    /// An `f32` from its bit pattern. The C++ decoders copy float bits as
    /// integers, even where the encoder used the x87 stack.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_f32(&mut self) -> Result<f32, WireError> {
        self.get_u32().map(f32::from_bits)
    }

    /// An `f64` from its bit pattern.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_f64(&mut self) -> Result<f64, WireError> {
        self.get_u64().map(f64::from_bits)
    }

    /// A vector, `x`, `y`, `z`, one 12-byte field.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_vector(&mut self) -> Result<[f32; 3], WireError> {
        let field: [u8; 12] = self.get_array()?;
        Ok(floats(&field))
    }

    /// A quaternion in the order `w`, `x`, `y`, `z`, one 16-byte field.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_quaternion(&mut self) -> Result<[f32; 4], WireError> {
        let field: [u8; 16] = self.get_array()?;
        Ok(floats(&field))
    }

    /// `count` UTF-16 code units as one field.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the field is cut short.
    pub fn get_wide(&mut self, count: usize) -> Result<Vec<u16>, WireError> {
        let width = count.checked_mul(2).ok_or(WireError::LengthOutOfRange {
            length: count,
            max: usize::MAX / 2,
        })?;
        let field = self.get_bytes(width)?;
        Ok(field
            .as_chunks::<2>()
            .0
            .iter()
            .map(|pair| u16::from_le_bytes(*pair))
            .collect())
    }

    /// A `u8` count, then that many bytes.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the input is cut short; the count
    /// stays consumed.
    pub fn get_bytes_u8(&mut self) -> Result<&'a [u8], WireError> {
        let count = self.get_u8()?;
        self.get_bytes(usize::from(count))
    }

    /// A `u16` count, then that many bytes.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the input is cut short; the count
    /// stays consumed.
    pub fn get_bytes_u16(&mut self) -> Result<&'a [u8], WireError> {
        let count = self.get_u16()?;
        self.get_bytes(usize::from(count))
    }

    /// A `u32` count, then that many bytes (`RpcWireDecodeOwnedBlock`).
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the input is cut short; the count
    /// stays consumed.
    pub fn get_bytes_u32(&mut self) -> Result<&'a [u8], WireError> {
        let count = self.get_u32()?;
        let count = usize::try_from(count).map_err(|_| WireError::Underrun {
            needed: usize::MAX,
            remaining: self.remaining(),
        })?;
        self.get_bytes(count)
    }

    /// A `u8` count of code units, then the units.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the input is cut short; the count
    /// stays consumed.
    pub fn get_wide_u8(&mut self) -> Result<Vec<u16>, WireError> {
        let count = self.get_u8()?;
        self.get_wide(usize::from(count))
    }

    /// A `u16` count of code units, then the units.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the input is cut short; the count
    /// stays consumed.
    pub fn get_wide_u16(&mut self) -> Result<Vec<u16>, WireError> {
        let count = self.get_u16()?;
        self.get_wide(usize::from(count))
    }
}

fn floats<const N: usize>(field: &[u8]) -> [f32; N] {
    let mut out = [0.0; N];
    for (component, chunk) in out.iter_mut().zip(field.as_chunks::<4>().0) {
        *component = f32::from_bits(u32::from_le_bytes(*chunk));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_word_layout() {
        let header = MessageHeader::new(2, 0x4f);
        assert_eq!(header.to_word(), 0x024f);
        assert_eq!(MessageHeader::from_word(0x024f), header);
        assert_eq!(header.packet_id(), 0x2004f);
        assert_eq!(MessageHeader::word_for_packet_id(0x2004f), 0x024f);
        // The C++ masks the opcode to 12 bits and ORs the channel in.
        assert_eq!(MessageHeader::word_for_packet_id(0x30abc), 0x0bbc);
        let mut writer = WireWriter::new();
        writer.put_header(header).unwrap();
        assert_eq!(writer.as_bytes(), [0x4f, 0x02]);
    }

    #[test]
    fn fields_are_little_endian() {
        let mut writer = WireWriter::new();
        writer.put_u8(0x01).unwrap();
        writer.put_u16(0x0302).unwrap();
        writer.put_u32(0x0706_0504).unwrap();
        writer.put_i16(-2).unwrap();
        writer.put_f32(1.0).unwrap();
        writer.put_vector([1.0, -2.0, 0.5]).unwrap();
        writer.put_quaternion([1.0, 0.0, 0.0, -0.0]).unwrap();
        let bytes = writer.into_bytes();
        assert_eq!(
            &bytes[..13],
            [1, 2, 3, 4, 5, 6, 7, 0xfe, 0xff, 0, 0, 0x80, 0x3f]
        );
        let mut reader = WireReader::new(&bytes);
        assert_eq!(reader.get_u8(), Ok(1));
        assert_eq!(reader.get_u16(), Ok(0x0302));
        assert_eq!(reader.get_u32(), Ok(0x0706_0504));
        assert_eq!(reader.get_i16(), Ok(-2));
        assert_eq!(reader.get_f32(), Ok(1.0));
        assert_eq!(reader.get_vector(), Ok([1.0, -2.0, 0.5]));
        let q = reader.get_quaternion().unwrap();
        assert_eq!(q[3].to_bits(), 0x8000_0000);
        assert_eq!(reader.finish(), Ok(()));
    }

    #[test]
    fn limit_is_checked_before_writing() {
        let mut writer = WireWriter::with_limit(5);
        writer.put_u32(1).unwrap();
        assert_eq!(
            writer.put_u16(2),
            Err(WireError::Overflow {
                needed: 6,
                limit: 5
            })
        );
        assert_eq!(writer.len(), 4);
        writer.put_u8(3).unwrap();
        assert_eq!(writer.put_bytes(&[]), Ok(()));
        assert_eq!(writer.put_u8(4).unwrap_err().legacy_code(), Some(0x16));
    }

    #[test]
    fn reads_fail_without_moving() {
        let mut reader = WireReader::new(&[1, 2, 3]);
        assert_eq!(
            reader.get_u32(),
            Err(WireError::Underrun {
                needed: 4,
                remaining: 3
            })
        );
        assert_eq!(reader.position(), 0);
        assert_eq!(reader.get_u16(), Ok(0x0201));
        assert_eq!(reader.get_u16().unwrap_err().legacy_code(), Some(0x17));
        assert_eq!(reader.finish(), Err(WireError::TrailingBytes { count: 1 }));
        assert_eq!(reader.rest(), [3]);
    }

    #[test]
    fn counted_fields() {
        let mut writer = WireWriter::new();
        writer.put_bytes_u16(b"abc").unwrap();
        writer.put_wide_u16(&[0x41, 0x20ac]).unwrap();
        writer.put_wide_u8(&[0x42]).unwrap();
        writer.put_bytes_u32(&[9; 3]).unwrap();
        writer.put_bytes_u8(&[]).unwrap();
        let bytes = writer.into_bytes();
        let mut reader = WireReader::new(&bytes);
        assert_eq!(reader.get_bytes_u16(), Ok(&b"abc"[..]));
        assert_eq!(reader.get_wide_u16(), Ok(vec![0x41, 0x20ac]));
        assert_eq!(reader.get_wide_u8(), Ok(vec![0x42]));
        assert_eq!(reader.get_bytes_u32(), Ok(&[9, 9, 9][..]));
        assert_eq!(reader.get_bytes_u8(), Ok(&[][..]));
        assert!(reader.is_empty());
    }

    #[test]
    fn counts_that_do_not_fit_are_refused() {
        let mut writer = WireWriter::new();
        assert_eq!(
            writer.put_bytes_u8(&[0; 256]),
            Err(WireError::LengthOutOfRange {
                length: 256,
                max: 255
            })
        );
        assert_eq!(
            writer.put_wide_u16(&vec![0; 65_536]),
            Err(WireError::LengthOutOfRange {
                length: 65_536,
                max: 65_535
            })
        );
        assert!(writer.is_empty());
    }

    #[test]
    fn malformed_counts_do_not_panic_or_allocate() {
        let mut reader = WireReader::new(&[0xff, 0xff, 0xff, 0xff, 1]);
        assert!(matches!(
            reader.get_bytes_u32(),
            Err(WireError::Underrun { .. })
        ));
        assert_eq!(reader.position(), 4);
        let mut reader = WireReader::new(&[0xff, 0xff]);
        assert!(reader.get_wide_u16().is_err());
        let mut reader = WireReader::new(&[]);
        assert!(reader.get_header().is_err());
        assert!(reader.get_wide(usize::MAX).is_err());
    }

    #[test]
    fn x87_copies_quiet_signalling_nans() {
        assert_eq!(x87_copy_f32(0x7f80_0001), 0x7fc0_0001);
        assert_eq!(x87_copy_f32(0xff80_0001), 0xffc0_0001);
        assert_eq!(x87_copy_f32(0x7fc0_0000), 0x7fc0_0000);
        assert_eq!(x87_copy_f32(0x7f80_0000), 0x7f80_0000);
        assert_eq!(x87_copy_f32(0x0000_0001), 0x0000_0001);
        assert_eq!(x87_copy_f64(0x7ff0_0000_0000_0001), 0x7ff8_0000_0000_0001);
        assert_eq!(x87_copy_f64(0xfff0_0000_0000_0000), 0xfff0_0000_0000_0000);
        let mut writer = WireWriter::new();
        writer.put_f32_x87(f32::from_bits(0x7fa0_0000)).unwrap();
        writer.put_f32(f32::from_bits(0x7fa0_0000)).unwrap();
        assert_eq!(writer.as_bytes(), [0, 0, 0xe0, 0x7f, 0, 0, 0xa0, 0x7f]);
    }
}
