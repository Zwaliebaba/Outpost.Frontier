//! Channel archetypes: the `Channel` node of an animation map (the
//! `ChannelArchetype` component of `CppSource/SceneRuntime/SceneRuntime.cpp`,
//! `ChannelArchetypeLoad`, `ChannelStrideFromHeader`,
//! `ChannelDecompressFrames`).
//!
//! A channel is two files. `Header` holds 12 bytes, little-endian: the
//! frame count (u32), the interval (f32; negative means every frame starts
//! with its own f32 time stamp) and the flags (u32). `Frames` holds the
//! frames, `frame count * stride` bytes, where the stride follows from the
//! flags (see [`stride_for`]). Flags exactly [`FLAG_SCALAR_ONLY`] (event
//! channels) skip the size check: their file carries data after the frames.
//!
//! A channel with any of the packed bits ([`COMPRESS_MASK`]) is unpacked at
//! load time into time stamp, position and quaternion words, and its flags
//! become [`FLAG_POSITION`] and/or [`FLAG_QUATERNION`]; the reference does
//! not keep the packed bytes. The unpacking of [`FLAG_PACKED_QUATERNION`]
//! and [`FLAG_PACKED_QUATERNION4`] uses basic arithmetic and `sqrt` and is
//! bit-exact (Tier A); [`FLAG_PACKED_AXIS_ANGLE`] goes through `sin` and is
//! Tier B (decision T11; the bound is in `tests/oracle_anim.rs`).
//!
//! Sampling, interpolation and the typed channels that consume a channel
//! are evaluation, in [`crate::scene`]. This module exposes what they read:
//! [`Channel::frame_time`], [`Channel::payload`] and the typed views.

use crate::diag::{DumpFlags, dump};
use crate::math::{Quaternion, Vector};
use crate::vfs::{FileSystem, FsDesc, VfsError, VfsFile};

use super::{AnimError, lift};

/// Header flag: one f32 per frame.
pub const FLAG_SCALAR: u32 = 0x1;
/// Header flag: a position (three f32) per frame.
pub const FLAG_POSITION: u32 = 0x2;
/// Header flag: a quaternion (four f32, w first) per frame.
pub const FLAG_QUATERNION: u32 = 0x4;
/// Header flag: one dword per frame, the offset of the frame's event data
/// after the frames (`CollectRange`, `CollectAt`). Only a header whose
/// flags are exactly this value skips the frame-size check.
pub const FLAG_SCALAR_ONLY: u32 = 0x8;
/// Packed flag: a zero position, no bytes in the file.
pub const FLAG_ZERO_POSITION: u32 = 0x10;
/// Packed flag: the identity quaternion, no bytes in the file.
pub const FLAG_IDENTITY_QUATERNION: u32 = 0x20;
/// Packed flag: x, y, z of a unit quaternion as three i16 (6 bytes); w is
/// rebuilt.
pub const FLAG_PACKED_QUATERNION: u32 = 0x40;
/// Packed flag: a rotation axis scaled by the angle over pi as three i16
/// (6 bytes).
pub const FLAG_PACKED_AXIS_ANGLE: u32 = 0x80;
/// Packed flag: four i16 (8 bytes), renormalised by one Newton step.
pub const FLAG_PACKED_QUATERNION4: u32 = 0x100;
/// The packed bits (`G_ChannelCompressMask`).
pub const COMPRESS_MASK: u32 = 0x1F0;
/// `G_ShortToUnit`: a packed i16 times this is the component.
pub const SHORT_TO_UNIT: f32 = 3.051_851e-5;

/// The bytes of a `Header` file the reference reads.
pub const HEADER_SIZE: usize = 12;

/// `3.1415927f` in the axis-angle unpacking (the f32 nearest to pi).
const PI_F32: f32 = core::f32::consts::PI;

/// The `Header` file.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ChannelHeader {
    /// +0x00: frames in the channel.
    pub frame_count: u32,
    /// +0x04: seconds between frames; negative when each frame carries its
    /// own time stamp.
    pub interval: f32,
    /// +0x08: the flags.
    pub flags: u32,
}

impl ChannelHeader {
    /// The 12 header bytes.
    #[must_use]
    pub fn parse(bytes: [u8; HEADER_SIZE]) -> Self {
        let word = |at: usize| {
            u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
        };
        Self {
            frame_count: word(0),
            interval: f32::from_bits(word(4)),
            flags: word(8),
        }
    }

    /// The 12 bytes this header is read from.
    #[must_use]
    pub fn to_bytes(self) -> [u8; HEADER_SIZE] {
        let mut out = [0; HEADER_SIZE];
        out[0..4].copy_from_slice(&self.frame_count.to_le_bytes());
        out[4..8].copy_from_slice(&self.interval.to_bits().to_le_bytes());
        out[8..12].copy_from_slice(&self.flags.to_le_bytes());
        out
    }

    /// Whether every frame starts with a time stamp: `interval < 0`.
    #[must_use]
    pub fn has_time_stamps(self) -> bool {
        has_time_stamps(self.interval)
    }

    /// The stride the reference derives from this header.
    #[must_use]
    pub fn stride(self) -> u32 {
        stride_for(self.flags, self.interval)
    }
}

fn has_time_stamps(interval: f32) -> bool {
    f64::from(interval) < 0.0
}

/// `ChannelStrideFromHeader`: bytes per frame. [`FLAG_SCALAR_ONLY`] resets
/// the sum to 4, as the reference's sequence of tests does; the
/// [`FLAG_ZERO_POSITION`] and [`FLAG_IDENTITY_QUATERNION`] bits add
/// nothing; a time stamp adds 4.
#[must_use]
pub fn stride_for(flags: u32, interval: f32) -> u32 {
    let mut stride = 0;
    if flags & FLAG_SCALAR != 0 {
        stride = 4;
    }
    if flags & FLAG_POSITION != 0 {
        stride += 0xC;
    }
    if flags & FLAG_QUATERNION != 0 {
        stride += 0x10;
    }
    if flags & FLAG_SCALAR_ONLY != 0 {
        stride = 4;
    }
    if flags & FLAG_PACKED_QUATERNION != 0 {
        stride += 6;
    }
    if flags & FLAG_PACKED_AXIS_ANGLE != 0 {
        stride += 6;
    }
    if flags & FLAG_PACKED_QUATERNION4 != 0 {
        stride += 8;
    }
    if interval < 0.0 {
        stride += 4;
    }
    stride
}

/// Why a channel did not load. [`ChannelError::code`] is the `GENRESULT`
/// `IChannelArchetype::Load` returns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChannelError {
    /// `Header` cannot be opened.
    NoHeader,
    /// Fewer than 12 bytes of `Header` were read.
    ShortHeader,
    /// `Frames` cannot be opened.
    NoFrames,
    /// The size of `Frames` is not frame count times stride (and the flags
    /// are not exactly [`FLAG_SCALAR_ONLY`]).
    SizeMismatch {
        /// The size of `Frames`.
        size: u32,
        /// Frame count times stride, wrapping at 32 bits as in the C++.
        expected: u32,
    },
    /// Fewer bytes of `Frames` were read than its size.
    ShortFrames,
    /// The product of frame count and stride overflows 32 bits but wraps
    /// to the file size, or the unpacked frames would not fit in 32 bits.
    /// The reference reads and writes outside its buffers (divergence
    /// register); the port fails with result -6.
    Overflow,
    /// The last frame's time stamp, which gives the duration, lies outside
    /// the frames: a stamped channel with no frames, or an event channel
    /// whose file is shorter than its frames. The reference reads outside
    /// its buffer and succeeds (divergence register); the port fails with
    /// result -6.
    StampOutsideFrames,
    /// A channel with packed bits whose `Frames` file is empty. The
    /// reference points such frames at its shared unpacking buffer, which
    /// is null while no channel holds it, and fails with
    /// `GR_OUT_OF_MEMORY`; while another channel holds it the load
    /// succeeds. The port always fails (divergence register).
    EmptyPackedFrames,
}

impl ChannelError {
    /// `ChannelArchetypeLoad`'s result: -6 for open and read failures, -1
    /// (`GR_GENERIC`) for a size mismatch, -4 (`GR_OUT_OF_MEMORY`) for empty
    /// packed frames.
    #[must_use]
    pub const fn code(self) -> u32 {
        match self {
            Self::SizeMismatch { .. } => 0xFFFF_FFFF,
            Self::EmptyPackedFrames => 0xFFFF_FFFC,
            Self::NoHeader
            | Self::ShortHeader
            | Self::NoFrames
            | Self::ShortFrames
            | Self::Overflow
            | Self::StampOutsideFrames => 0xFFFF_FFFA,
        }
    }
}

impl core::fmt::Display for ChannelError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::NoHeader => f.write_str("channel has no Header"),
            Self::ShortHeader => f.write_str("channel Header shorter than 12 bytes"),
            Self::NoFrames => f.write_str("channel has no Frames"),
            Self::SizeMismatch { size, expected } => {
                write!(
                    f,
                    "channel Frames size {size:#x}, header gives {expected:#x}"
                )
            }
            Self::ShortFrames => f.write_str("channel Frames shorter than its size"),
            Self::Overflow => f.write_str("channel frame size overflows"),
            Self::StampOutsideFrames => f.write_str("channel time stamp outside its frames"),
            Self::EmptyPackedFrames => f.write_str("packed channel with empty Frames"),
        }
    }
}

impl std::error::Error for ChannelError {}

/// Why `ChannelDecompressFrames` left a packed channel as it was read. The
/// reference reports it through `FDUMP` with the severity "fatal" and goes
/// on with the packed bytes, flags and stride.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodeFatal {
    /// [`FLAG_SCALAR`] or [`FLAG_SCALAR_ONLY`] with a packed bit: "Cannot
    /// uncompress data other than vectors and quaternions".
    NotVectorOrQuaternion,
    /// [`FLAG_POSITION`] with [`FLAG_ZERO_POSITION`]: "Inconsistent
    /// specification for vector data".
    InconsistentVector,
    /// More than one quaternion source: "Inconsistent specification for
    /// quaternion data".
    InconsistentQuaternion,
}

impl DecodeFatal {
    /// The `FDUMP` text.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::NotVectorOrQuaternion => {
                "Cannot uncompress data other than vectors and quaternions"
            }
            Self::InconsistentVector => "Inconsistent specification for vector data",
            Self::InconsistentQuaternion => "Inconsistent specification for quaternion data",
        }
    }
}

/// The frame layouts the typed channels of the runtime are made for: the
/// flags of a loaded channel, when they are exactly one of these.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChannelKind {
    /// Flags 1: one f32.
    Scalar,
    /// Flags 2: a position.
    Vector,
    /// Flags 4: a quaternion.
    Quaternion,
    /// Flags 6: a position, then a quaternion.
    PositionQuaternion,
    /// Flags 8: an event-data offset.
    Event,
}

/// A loaded channel: the frame buffer the reference keeps, unpacked.
#[derive(Clone, Debug, PartialEq)]
pub struct Channel {
    header: ChannelHeader,
    flags: u32,
    stride: u32,
    frames: Vec<u8>,
    duration: f32,
    fatal: Option<DecodeFatal>,
}

impl Channel {
    /// `ChannelArchetypeLoad` after both files were read: checks the size,
    /// unpacks packed frames and computes the duration. `frames` is the
    /// whole `Frames` file.
    ///
    /// # Errors
    ///
    /// [`ChannelError::SizeMismatch`], [`ChannelError::Overflow`] or
    /// [`ChannelError::StampOutsideFrames`].
    pub fn decode(header: ChannelHeader, frames: Vec<u8>) -> Result<Self, ChannelError> {
        let stride = header.stride();
        if header.flags != FLAG_SCALAR_ONLY {
            let size = u32::try_from(frames.len()).map_err(|_| ChannelError::Overflow)?;
            let expected = header.frame_count.wrapping_mul(stride);
            if size != expected {
                return Err(ChannelError::SizeMismatch { size, expected });
            }
            if u64::from(header.frame_count) * u64::from(stride) != u64::from(size) {
                return Err(ChannelError::Overflow);
            }
        }
        let mut channel = Self {
            header,
            flags: header.flags,
            stride,
            frames,
            duration: 0.0,
            fatal: None,
        };
        channel.decompress()?;
        channel.duration = channel.compute_duration()?;
        Ok(channel)
    }

    /// `ChannelDecompressFrames`.
    fn decompress(&mut self) -> Result<(), ChannelError> {
        let flags = self.flags;
        if flags & 0xF0 == 0 && flags & FLAG_PACKED_QUATERNION4 == 0 {
            return Ok(());
        }
        if let Some(fatal) = decode_fatal(flags) {
            dump(DumpFlags::FATAL, fatal.message());
            self.fatal = Some(fatal);
            return Ok(());
        }
        let stamp = has_time_stamps(self.header.interval);
        let pos = flags & FLAG_POSITION != 0;
        let zero_pos = flags & FLAG_ZERO_POSITION != 0;
        let has_pos = pos || zero_pos;
        let quat = flags & FLAG_QUATERNION != 0;
        let ident = flags & FLAG_IDENTITY_QUATERNION != 0;
        let comp3 = flags & FLAG_PACKED_QUATERNION != 0;
        let axis = flags & FLAG_PACKED_AXIS_ANGLE != 0;
        let comp4 = flags & FLAG_PACKED_QUATERNION4 != 0;
        let has_quat = quat || ident || comp3 || axis || comp4;

        let words = u64::from(stamp) + if has_pos { 3 } else { 0 } + if has_quat { 4 } else { 0 };
        let out_len = u64::from(self.header.frame_count) * words * 4;
        let out_len = u32::try_from(out_len).map_err(|_| ChannelError::Overflow)?;
        let mut out = Vec::with_capacity(out_len as usize);

        let mut new_flags = flags & 0xFFFF_FE0F;
        if has_pos {
            new_flags |= FLAG_POSITION;
        }
        if has_quat {
            new_flags |= FLAG_QUATERNION;
        }

        let src = &self.frames;
        let mut at = 0usize;
        // The size check guarantees every read below is inside `src`.
        let take = |at: &mut usize, n: usize| -> &[u8] {
            let s = &src[*at..*at + n];
            *at += n;
            s
        };
        for _ in 0..self.header.frame_count {
            if stamp {
                out.extend_from_slice(take(&mut at, 4));
            }
            if pos {
                out.extend_from_slice(take(&mut at, 12));
            } else if zero_pos {
                for _ in 0..3 {
                    put(&mut out, 0.0);
                }
            }
            if !has_quat {
                continue;
            }
            if quat {
                out.extend_from_slice(take(&mut at, 16));
            } else if ident {
                for value in [1.0, 0.0, 0.0, 0.0] {
                    put(&mut out, value);
                }
            } else if comp3 {
                for value in unpack_quaternion3(take(&mut at, 6)) {
                    put(&mut out, value);
                }
            } else if axis {
                for value in unpack_axis_angle(take(&mut at, 6)) {
                    put(&mut out, value);
                }
            } else {
                for value in unpack_quaternion4(take(&mut at, 8)) {
                    put(&mut out, value);
                }
            }
        }
        self.flags = new_flags;
        self.stride = stride_for(new_flags, self.header.interval);
        self.frames = out;
        Ok(())
    }

    /// The duration `ChannelArchetypeLoad` stores: the last frame's time.
    fn compute_duration(&self) -> Result<f32, ChannelError> {
        let last = self.header.frame_count.wrapping_sub(1);
        if has_time_stamps(self.header.interval) {
            let at = u64::from(self.stride) * u64::from(last);
            return self.f32_at(at).ok_or(ChannelError::StampOutsideFrames);
        }
        Ok((f64::from(last) * f64::from(self.header.interval)) as f32)
    }

    fn f32_at(&self, at: u64) -> Option<f32> {
        let at = usize::try_from(at).ok()?;
        let bytes = self.frames.get(at..at.checked_add(4)?)?;
        Some(f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    /// The header as read.
    #[must_use]
    pub fn header(&self) -> ChannelHeader {
        self.header
    }

    /// `GetFlags`: the header flags, or after unpacking [`FLAG_POSITION`]
    /// and/or [`FLAG_QUATERNION`] with the other bits kept.
    #[must_use]
    pub fn flags(&self) -> u32 {
        self.flags
    }

    /// Bytes per frame in [`Channel::frames`].
    #[must_use]
    pub fn stride(&self) -> u32 {
        self.stride
    }

    /// The frame count.
    #[must_use]
    pub fn frame_count(&self) -> u32 {
        self.header.frame_count
    }

    /// The interval; negative for time-stamped frames.
    #[must_use]
    pub fn interval(&self) -> f32 {
        self.header.interval
    }

    /// Whether every frame starts with its time stamp.
    #[must_use]
    pub fn has_time_stamps(&self) -> bool {
        self.header.has_time_stamps()
    }

    /// `GetDuration`: the last frame's time stamp, or (frame count - 1)
    /// times the interval rounded to f32.
    #[must_use]
    pub fn duration(&self) -> f32 {
        self.duration
    }

    /// The frame buffer: unpacked frames, or the whole `Frames` file
    /// (event channels keep their event data after the frames).
    #[must_use]
    pub fn frames(&self) -> &[u8] {
        &self.frames
    }

    /// Set when the flags combine packed bits in a way the reference cannot
    /// unpack: the frames, flags and stride are then the file's.
    #[must_use]
    pub fn fatal(&self) -> Option<DecodeFatal> {
        self.fatal
    }

    /// The runtime's frame layout, when the flags are exactly one.
    #[must_use]
    pub fn kind(&self) -> Option<ChannelKind> {
        match self.flags {
            1 => Some(ChannelKind::Scalar),
            2 => Some(ChannelKind::Vector),
            4 => Some(ChannelKind::Quaternion),
            6 => Some(ChannelKind::PositionQuaternion),
            8 => Some(ChannelKind::Event),
            _ => None,
        }
    }

    /// `ChannelFrameTime`: the frame's time stamp, or `index * interval`
    /// in double precision. `None` for a stamp outside the frames.
    #[must_use]
    pub fn frame_time(&self, index: u32) -> Option<f64> {
        if self.has_time_stamps() {
            return self
                .f32_at(u64::from(self.stride) * u64::from(index))
                .map(f64::from);
        }
        Some(f64::from(index) * f64::from(self.header.interval))
    }

    /// `ChannelFramePayload`: the frame's bytes after its time stamp, or
    /// `None` past the frames.
    #[must_use]
    pub fn payload(&self, index: u32) -> Option<&[u8]> {
        let skip = if self.has_time_stamps() { 4 } else { 0 };
        let start = usize::try_from(u64::from(self.stride) * u64::from(index)).ok()?;
        let end = start.checked_add(self.stride as usize)?;
        self.frames.get(start + skip.min(self.stride as usize)..end)
    }

    /// The `word`th f32 of the frame's payload.
    #[must_use]
    pub fn payload_f32(&self, index: u32, word: usize) -> Option<f32> {
        let payload = self.payload(index)?;
        let bytes = payload.get(word * 4..word * 4 + 4)?;
        Some(f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    /// The f32 of a [`ChannelKind::Scalar`] frame.
    #[must_use]
    pub fn scalar(&self, index: u32) -> Option<f32> {
        (self.kind() == Some(ChannelKind::Scalar))
            .then(|| self.payload_f32(index, 0))
            .flatten()
    }

    /// The position of a [`ChannelKind::Vector`] or
    /// [`ChannelKind::PositionQuaternion`] frame.
    #[must_use]
    pub fn position(&self, index: u32) -> Option<Vector> {
        match self.kind()? {
            ChannelKind::Vector | ChannelKind::PositionQuaternion => Some(Vector {
                x: self.payload_f32(index, 0)?,
                y: self.payload_f32(index, 1)?,
                z: self.payload_f32(index, 2)?,
            }),
            _ => None,
        }
    }

    /// The quaternion (stored w, x, y, z) of a [`ChannelKind::Quaternion`]
    /// or [`ChannelKind::PositionQuaternion`] frame.
    #[must_use]
    pub fn quaternion(&self, index: u32) -> Option<Quaternion> {
        let first = match self.kind()? {
            ChannelKind::Quaternion => 0,
            ChannelKind::PositionQuaternion => 3,
            _ => return None,
        };
        Some(Quaternion {
            w: self.payload_f32(index, first)?,
            x: self.payload_f32(index, first + 1)?,
            y: self.payload_f32(index, first + 2)?,
            z: self.payload_f32(index, first + 3)?,
        })
    }

    /// `ChannelRelocatedKey` as an offset into [`Channel::frames`]: the
    /// end of the frames plus the [`ChannelKind::Event`] frame's dword,
    /// wrapping at 32 bits as the reference's pointer sum does.
    #[must_use]
    pub fn event_offset(&self, index: u32) -> Option<u32> {
        if self.kind() != Some(ChannelKind::Event) {
            return None;
        }
        let payload = self.payload(index)?;
        let reloc = u32::from_le_bytes(payload.get(..4)?.try_into().ok()?);
        Some(
            self.stride
                .wrapping_mul(self.header.frame_count)
                .wrapping_add(reloc),
        )
    }
}

fn put(out: &mut Vec<u8>, value: f32) {
    out.extend_from_slice(&value.to_le_bytes());
}

/// The fatal checks of `ChannelDecompressFrames`, in its order.
fn decode_fatal(flags: u32) -> Option<DecodeFatal> {
    if flags & (FLAG_SCALAR | FLAG_SCALAR_ONLY) != 0 {
        return Some(DecodeFatal::NotVectorOrQuaternion);
    }
    if flags & FLAG_POSITION != 0 && flags & FLAG_ZERO_POSITION != 0 {
        return Some(DecodeFatal::InconsistentVector);
    }
    let quat = flags & FLAG_QUATERNION != 0;
    let ident = flags & FLAG_IDENTITY_QUATERNION != 0;
    let comp3 = flags & FLAG_PACKED_QUATERNION != 0;
    let axis = flags & FLAG_PACKED_AXIS_ANGLE != 0;
    let comp4 = flags & FLAG_PACKED_QUATERNION4 != 0;
    let packed = comp3 || axis || comp4;
    if (quat && ident)
        || ((quat || ident) && packed)
        || (comp3 && axis)
        || (comp3 && comp4)
        || (axis && comp4)
    {
        return Some(DecodeFatal::InconsistentQuaternion);
    }
    None
}

/// `ChannelUnpackShort`: an i16 times [`SHORT_TO_UNIT`], exact in double.
fn unpack_short(bytes: &[u8], at: usize) -> f64 {
    f64::from(i16::from_le_bytes([bytes[at], bytes[at + 1]])) * f64::from(SHORT_TO_UNIT)
}

/// [`FLAG_PACKED_QUATERNION`]: w = sqrt(1 - |xyz|^2) when that is below 1,
/// else 0. Returns w, x, y, z as stored.
#[must_use]
pub fn unpack_quaternion3(bytes: &[u8]) -> [f32; 4] {
    let x = unpack_short(bytes, 0);
    let y = unpack_short(bytes, 2);
    let z = unpack_short(bytes, 4) as f32;
    let sum = f64::from(z) * f64::from(z) + y * y + x * x;
    let sum_stored = sum as f32;
    let w = if sum < 1.0 {
        (1.0 - f64::from(sum_stored)).sqrt()
    } else {
        0.0
    };
    [w as f32, x as f32, y as f32, z]
}

/// [`FLAG_PACKED_AXIS_ANGLE`]: the length of the vector is the angle over
/// pi; the result is the quaternion of that rotation. Tier B: it calls
/// `sin`. Returns w, x, y, z as stored.
#[must_use]
pub fn unpack_axis_angle(bytes: &[u8]) -> [f32; 4] {
    let x = unpack_short(bytes, 0);
    let y = unpack_short(bytes, 2);
    let z_full = unpack_short(bytes, 4);
    let z = z_full as f32;
    let sum = z_full * f64::from(z) + y * y + x * x;
    if sum > 0.0 {
        let length = sum.sqrt();
        let sine = (f64::from(PI_F32) * length * 0.5).sin();
        let sine_stored = sine as f32;
        let scale = (sine / length) as f32;
        let w = (1.0 - f64::from(sine_stored) * f64::from(sine_stored)).sqrt();
        [
            w as f32,
            (x * f64::from(scale)) as f32,
            (y * f64::from(scale)) as f32,
            (f64::from(scale) * f64::from(z)) as f32,
        ]
    } else {
        [1.0, x as f32, y as f32, z]
    }
}

/// [`FLAG_PACKED_QUATERNION4`]: the four components times one Newton step
/// towards unit length, `(3 - |q|^2) / 2`. The first component uses the
/// unrounded factor, the others the factor rounded to f32, as the
/// reference does. Returns the four words in file order.
#[must_use]
pub fn unpack_quaternion4(bytes: &[u8]) -> [f32; 4] {
    let x = unpack_short(bytes, 0);
    let y = unpack_short(bytes, 2);
    let z = unpack_short(bytes, 4) as f32;
    let w_full = unpack_short(bytes, 6);
    let w = w_full as f32;
    let sum = w_full * f64::from(w) + f64::from(z) * f64::from(z) + y * y + x * x;
    let mid = (3.0 - sum) * 0.5;
    let mid_stored = f64::from(mid as f32);
    [
        (mid * x) as f32,
        (mid_stored * y) as f32,
        (mid_stored * f64::from(z)) as f32,
        (mid_stored * f64::from(w)) as f32,
    ]
}

/// A `ChannelArchetype` after `Load`: what it read of its header, and the
/// channel or why it failed. The reference keeps the archetype either way
/// and answers [`LoadedChannel::duration`] 0 for a failed load.
#[derive(Clone, Debug, PartialEq)]
pub struct LoadedChannel {
    /// The header, once 12 bytes of it were read.
    pub header: Option<ChannelHeader>,
    /// The loaded channel.
    pub outcome: Result<Channel, ChannelError>,
}

impl LoadedChannel {
    /// `IChannelArchetype::Load`'s result: 0 or [`ChannelError::code`].
    #[must_use]
    pub fn result_code(&self) -> u32 {
        self.outcome.as_ref().err().map_or(0, |e| e.code())
    }

    /// The channel, if it loaded.
    #[must_use]
    pub fn channel(&self) -> Option<&Channel> {
        self.outcome.as_ref().ok()
    }

    /// The archetype's frame count: the header's, 0 before it was read.
    #[must_use]
    pub fn frame_count(&self) -> u32 {
        self.header.map_or(0, |h| h.frame_count)
    }

    /// The archetype's interval: the header's, 0 before it was read.
    #[must_use]
    pub fn interval(&self) -> f32 {
        self.header.map_or(0.0, |h| h.interval)
    }

    /// `GetFlags`.
    #[must_use]
    pub fn flags(&self) -> u32 {
        match &self.outcome {
            Ok(channel) => channel.flags(),
            Err(_) => self.header.map_or(0, |h| h.flags),
        }
    }

    /// The archetype's stride.
    #[must_use]
    pub fn stride(&self) -> u32 {
        match &self.outcome {
            Ok(channel) => channel.stride(),
            Err(_) => self.header.map_or(0, ChannelHeader::stride),
        }
    }

    /// `GetDuration`: 0 for a failed load.
    #[must_use]
    pub fn duration(&self) -> f32 {
        self.channel().map_or(0.0, Channel::duration)
    }
}

/// One `Read(handle, buffer, size)` of a whole file from its start: whether
/// the read succeeded, and the bytes it copied. The buffer never grows past
/// the bytes the file has.
pub(crate) fn read_whole(file: &mut VfsFile, size: u32) -> Result<(bool, Vec<u8>), AnimError> {
    let available = file.contents().map_or(0, <[u8]>::len);
    let mut buf = vec![0; (size as usize).min(available)];
    let (ok, got) = match lift(file.read(&mut buf))? {
        Ok(got) => (true, got),
        Err(VfsError::ShortRead { read }) => (false, read),
        Err(_) => (false, 0),
    };
    buf.truncate(got);
    Ok((ok, buf))
}

/// `ChannelArchetypeLoad(fs)`: reads `Header` and `Frames` below `fs`
/// (opened as `"DOS"` descriptors, as the reference does) and decodes them.
///
/// # Errors
///
/// [`AnimError`] for a malformed archive. Load failures the reference
/// reports are in [`LoadedChannel::outcome`].
pub fn load_channel(fs: &dyn FileSystem) -> Result<LoadedChannel, AnimError> {
    let failed = |header, error| {
        Ok(LoadedChannel {
            header,
            outcome: Err(error),
        })
    };
    let Ok(mut file) = lift(fs.open_file(&FsDesc::read(b"Header").with_implementation(b"DOS")))?
    else {
        return failed(None, ChannelError::NoHeader);
    };
    let mut bytes = [0; HEADER_SIZE];
    if !matches!(lift(file.read(&mut bytes))?, Ok(HEADER_SIZE)) {
        return failed(None, ChannelError::ShortHeader);
    }
    let header = ChannelHeader::parse(bytes);
    let Ok(mut file) = lift(fs.open_file(&FsDesc::read(b"Frames").with_implementation(b"DOS")))?
    else {
        return failed(Some(header), ChannelError::NoFrames);
    };
    let size = lift(file.size())?.unwrap_or(0);
    let expected = header.frame_count.wrapping_mul(header.stride());
    if header.flags != FLAG_SCALAR_ONLY && size != expected {
        return failed(Some(header), ChannelError::SizeMismatch { size, expected });
    }
    if header.flags & COMPRESS_MASK != 0 && size == 0 {
        return failed(Some(header), ChannelError::EmptyPackedFrames);
    }
    let (ok, frames) = read_whole(&mut file, size)?;
    if !ok || frames.len() != size as usize {
        return failed(Some(header), ChannelError::ShortFrames);
    }
    Ok(LoadedChannel {
        header: Some(header),
        outcome: Channel::decode(header, frames),
    })
}
