//! The keyed values of node properties (`particlefx_ramp.cpp`) and their
//! readers (`particlefx_common.cpp`, `Read*Block`, `ReadNested*Ramp`,
//! `ReadAnimatedTransform`).
//!
//! Only the stored data and the operations loading performs are here:
//! construction defaults, `Reset`, `SetMode`, `AddKey` (with each class's
//! insertion rule for equal times), `SetWeight`, `SetLoopType`,
//! `SetPackedModes` and `UpdateChannelFlags`. Evaluation (the
//! interpolators, slopes and the transform's matrix) belongs to the client
//! (work package P4.8).

use super::chunk::{Chunk, Short};
use super::{VERSION_1_0, VERSION_1_1};
use crate::num::trunc_to_i32_via_i64;

/// One key of a flat ramp.
#[derive(Clone, Debug, PartialEq)]
pub struct RampKey<T> {
    /// The key time.
    pub time: f32,
    /// The value at that time.
    pub value: T,
}

/// The index `AddKey` inserts a key at in the flat and nested ramps
/// (`TRampKeyVector::UpperBound`): after every key whose time is not
/// greater, so a new key follows keys of equal time; a NaN time goes last.
fn upper_bound<T>(keys: &[RampKey<T>], time: f32) -> usize {
    keys.iter()
        .position(|k| time < k.time)
        .unwrap_or(keys.len())
}

fn insert_key<T>(keys: &mut Vec<RampKey<T>>, time: f32, value: T) -> usize {
    let index = upper_bound(keys, time);
    keys.insert(index, RampKey { time, value });
    index
}

/// The interpolation mode every ramp is constructed with.
pub const DEFAULT_MODE: u32 = 4;

/// `FxRampSingle`: scalar keys and an interpolation mode. Constructed with
/// mode 4 and no keys. The mode is stored as given; the reference also
/// selects an interpolator from a 14-entry table with it, which only
/// evaluation uses.
#[derive(Clone, Debug, PartialEq)]
pub struct RampSingle {
    /// The interpolation mode (`SetMode`).
    pub mode: u32,
    /// The keys, ordered by time.
    pub keys: Vec<RampKey<f32>>,
}

impl Default for RampSingle {
    fn default() -> Self {
        Self::new()
    }
}

impl RampSingle {
    /// The constructor: mode 4, no keys.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            mode: DEFAULT_MODE,
            keys: Vec::new(),
        }
    }

    /// `MakeRampSingle`: mode 4 and one key `(0, value)`.
    #[must_use]
    pub fn with_value(value: f32) -> Self {
        let mut ramp = Self::new();
        ramp.add_key(0.0, value);
        ramp
    }

    /// `AddKey`: inserts after keys of equal time; returns the index.
    pub fn add_key(&mut self, time: f32, value: f32) -> usize {
        insert_key(&mut self.keys, time, value)
    }

    /// `ReadRampSingleBlock`: `Reset`, the mode byte, the key count byte,
    /// then per key the time and the value (four bytes each), each key
    /// added as it is read.
    pub(crate) fn read(&mut self, chunk: &mut Chunk) -> Result<(), Short> {
        self.keys.clear();
        self.mode = u32::from(chunk.u8()?);
        let count = chunk.u8()?;
        for _ in 0..count {
            let time = chunk.f32()?;
            let value = chunk.f32()?;
            self.add_key(time, value);
        }
        Ok(())
    }
}

/// The colour packing of `FxRampColor::AddKey` (`RampPackRgb`): each
/// channel truncated from `c * 255` (as `__ftol`, NaN and out of range give
/// 0), combined as `((r << 8 | g) << 8) | b` with no masking, so an out of
/// range channel spills into its neighbours.
#[must_use]
pub fn pack_rgb(rgb: [f32; 3]) -> u32 {
    let channel = |c: f32| trunc_to_i32_via_i64(f64::from(c) * 255.0).cast_unsigned();
    let green = channel(rgb[1]);
    let red = channel(rgb[0]);
    ((green | (red << 8)) << 8) | channel(rgb[2])
}

/// `FxRampColor::GetKeyRgb`: each byte of `0x00RRGGBB` times `1/255`
/// (rounded up to `0x3B808081`).
#[must_use]
pub fn unpack_rgb(packed: u32) -> [f32; 3] {
    const INV_255: f32 = f32::from_bits(0x3B80_8081);
    // Each byte converts exactly; the product is rounded once.
    let byte = |shift: u32| f32::from(((packed >> shift) & 0xFF) as u8) * INV_255;
    [byte(16), byte(8), byte(0)]
}

/// The grey `{0.8, 0.8, 0.8}` the colour defaults use, packed: `0xCCCCCC`.
#[must_use]
pub fn default_grey() -> u32 {
    let grey = f32::from_bits(0x3F4C_CCCD);
    pack_rgb([grey; 3])
}

/// `FxRampColor`: colour keys stored packed as `0x00RRGGBB` (the bytes the
/// float channels truncate to) and an interpolation mode.
#[derive(Clone, Debug, PartialEq)]
pub struct RampColor {
    /// The interpolation mode.
    pub mode: u32,
    /// The keys, ordered by time; values packed as by [`pack_rgb`].
    pub keys: Vec<RampKey<u32>>,
}

impl Default for RampColor {
    fn default() -> Self {
        Self::new()
    }
}

impl RampColor {
    /// The constructor: mode 4, no keys.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            mode: DEFAULT_MODE,
            keys: Vec::new(),
        }
    }

    /// `MakeRampColor`: mode 4 and one key `(0, packed)`.
    #[must_use]
    pub fn with_packed(packed: u32) -> Self {
        let mut ramp = Self::new();
        insert_key(&mut ramp.keys, 0.0, packed);
        ramp
    }

    /// `AddKey(time, rgb)`: packs the channels, then inserts after keys of
    /// equal time; returns the index.
    pub fn add_key(&mut self, time: f32, rgb: [f32; 3]) -> usize {
        insert_key(&mut self.keys, time, pack_rgb(rgb))
    }

    /// `ReadRampColorBlock`: as [`RampSingle`]'s block with a time and
    /// three float channels (twelve bytes) per key.
    pub(crate) fn read(&mut self, chunk: &mut Chunk) -> Result<(), Short> {
        self.keys.clear();
        self.mode = u32::from(chunk.u8()?);
        let count = chunk.u8()?;
        for _ in 0..count {
            let time = chunk.f32()?;
            let rgb = [chunk.f32()?, chunk.f32()?, chunk.f32()?];
            self.add_key(time, rgb);
        }
        Ok(())
    }
}

/// One key of an [`AnimatedSingle`], in the reference's field order.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AnimatedKey {
    /// The value.
    pub value: f32,
    /// The key time.
    pub time: f32,
    /// The incoming slope.
    pub in_slope: f32,
    /// The outgoing slope; the bit pattern `0xFFFFFFFF` marks a step.
    pub out_slope: f32,
}

/// The key track an [`AnimatedSingle`] allocates on its first key.
#[derive(Clone, Debug, PartialEq)]
pub struct AnimatedTrack {
    /// The loop type (`SetLoopType`), 0 when the track is made.
    pub loop_type: u32,
    /// The keys, ordered by time.
    pub keys: Vec<AnimatedKey>,
}

/// `FxAnimatedSingle`: a weight (the value while there is no key) and a
/// key track made by the first `AddKey`. `Reset` empties the track but
/// keeps it, its loop type and the weight. `SetLoopType` fails while there
/// is no track.
#[derive(Clone, Debug, PartialEq)]
pub struct AnimatedSingle {
    /// The weight, 1.0 when constructed.
    pub weight: f32,
    /// The key track, if a key was ever added.
    pub track: Option<AnimatedTrack>,
}

impl Default for AnimatedSingle {
    fn default() -> Self {
        Self::new()
    }
}

impl AnimatedSingle {
    /// The constructor: weight 1.0, no track.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            weight: 1.0,
            track: None,
        }
    }

    /// A new value with `SetWeight(weight)`, as the property defaults make it.
    #[must_use]
    pub const fn with_weight(weight: f32) -> Self {
        Self {
            weight,
            track: None,
        }
    }

    /// `GetKeyCount`: 0 without a track.
    #[must_use]
    pub fn key_count(&self) -> usize {
        self.track.as_ref().map_or(0, |t| t.keys.len())
    }

    /// `GetLoopType`: 0 without a track.
    #[must_use]
    pub fn loop_type(&self) -> u32 {
        self.track.as_ref().map_or(0, |t| t.loop_type)
    }

    /// The keys; empty without a track.
    #[must_use]
    pub fn keys(&self) -> &[AnimatedKey] {
        self.track.as_ref().map_or(&[], |t| &t.keys)
    }

    /// `Reset`: the track's keys are removed, the track stays.
    pub fn reset(&mut self) {
        if let Some(track) = &mut self.track {
            track.keys.clear();
        }
    }

    /// `AddKey(time, {value, in, out})`: makes the track if needed and
    /// inserts before the first key whose time is not smaller, so a new key
    /// precedes keys of equal time; a NaN time goes last. Returns the index.
    pub fn add_key(&mut self, time: f32, value: [f32; 3]) -> usize {
        let track = self.track.get_or_insert_with(|| AnimatedTrack {
            loop_type: 0,
            keys: Vec::new(),
        });
        let index = track
            .keys
            .iter()
            .position(|k| time <= k.time)
            .unwrap_or(track.keys.len());
        track.keys.insert(
            index,
            AnimatedKey {
                value: value[0],
                time,
                in_slope: value[1],
                out_slope: value[2],
            },
        );
        index
    }

    /// `SetLoopType`: fails (returns `false`) without a track.
    pub fn set_loop_type(&mut self, loop_type: u32) -> bool {
        match &mut self.track {
            Some(track) => {
                track.loop_type = loop_type;
                true
            }
            None => false,
        }
    }

    /// `ReadAnimatedSingleBlock`: `Reset`; the weight (4 bytes), the loop
    /// type and the key count (two bytes each); `SetWeight`; per key the
    /// time and the value triple (4 + 12 bytes), each added as read; then
    /// `SetLoopType`, whose failure fails the block unless the loop type
    /// is 0.
    pub(crate) fn read(&mut self, chunk: &mut Chunk) -> Result<(), Short> {
        self.reset();
        let weight = chunk.f32()?;
        let loop_type = chunk.u16()?;
        let count = chunk.u16()?;
        self.weight = weight;
        for _ in 0..count {
            let time = chunk.f32()?;
            let value = [chunk.f32()?, chunk.f32()?, chunk.f32()?];
            self.add_key(time, value);
        }
        if !self.set_loop_type(u32::from(loop_type)) && loop_type != 0 {
            return Err(Short);
        }
        Ok(())
    }
}

/// `FxRampSingleRamp`, `FxAnimatedSingleRamp` and `FxRampColorRamp`: keys
/// whose values are whole ramps, and a mode stored as given.
#[derive(Clone, Debug, PartialEq)]
pub struct NestedRamp<T> {
    /// The mode (`SetMode`, a plain store).
    pub mode: u32,
    /// The keys, ordered by time.
    pub keys: Vec<RampKey<T>>,
}

/// A ramp of [`RampSingle`] keys (property kind 0x200).
pub type RampSingleRamp = NestedRamp<RampSingle>;
/// A ramp of [`AnimatedSingle`] keys (property kind 0x202).
pub type AnimatedSingleRamp = NestedRamp<AnimatedSingle>;
/// A ramp of [`RampColor`] keys (property kind 0x201).
pub type ColorRampRamp = NestedRamp<RampColor>;

impl<T> Default for NestedRamp<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> NestedRamp<T> {
    /// The constructor (and the factory's, whose `Initialize` resets it):
    /// mode 4, no keys.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            mode: DEFAULT_MODE,
            keys: Vec::new(),
        }
    }

    /// A ramp with the one key `(0, value)`, as the property defaults make
    /// them (`MakeRampSingleRamp`, `MakeAnimatedSingleRamp`,
    /// `MakeRampColorRamp`).
    #[must_use]
    pub fn with_key(value: T) -> Self {
        let mut ramp = Self::new();
        ramp.add_key(0.0, value);
        ramp
    }

    /// `AddKey`: inserts after keys of equal time; returns the index.
    pub fn add_key(&mut self, time: f32, value: T) -> usize {
        insert_key(&mut self.keys, time, value)
    }
}

/// Version-dependent part shared by the three nested readers: a 1.0 stream
/// holds one child block (keyed at time 0); a 1.1 stream a mode byte, a
/// count byte and per key a time and a child block. Children are fresh
/// values made by `child`. `Err` covers short reads, child failures and an
/// unknown version (the reference logs "Unknown version"). The ramp is
/// reset first in every case.
fn read_nested<T>(
    ramp: &mut NestedRamp<T>,
    chunk: &mut Chunk,
    version: u32,
    child: impl Fn() -> T,
    read_child: impl Fn(&mut T, &mut Chunk) -> Result<(), Short>,
) -> Result<(), Short> {
    ramp.keys.clear();
    if version == VERSION_1_0 {
        let mut value = child();
        read_child(&mut value, chunk)?;
        ramp.add_key(0.0, value);
        Ok(())
    } else if version == VERSION_1_1 {
        ramp.mode = u32::from(chunk.u8()?);
        let count = chunk.u8()?;
        for _ in 0..count {
            let mut value = child();
            let time = chunk.f32()?;
            read_child(&mut value, chunk)?;
            ramp.add_key(time, value);
        }
        Ok(())
    } else {
        Err(Short)
    }
}

impl RampSingleRamp {
    /// `ReadNestedSingleRamp` (kind 0x200): after a complete read every key
    /// whose ramp has no key gets the key `(0, 0.0)`.
    pub(crate) fn read(&mut self, chunk: &mut Chunk, version: u32) {
        if read_nested(self, chunk, version, RampSingle::new, RampSingle::read).is_ok() {
            for key in &mut self.keys {
                if key.value.keys.is_empty() {
                    key.value.add_key(0.0, 0.0);
                }
            }
        }
    }
}

impl ColorRampRamp {
    /// `ReadNestedColorRamp` (kind 0x201): after a complete read every key
    /// whose ramp has no key gets the key `(0, grey)` ([`default_grey`]).
    pub(crate) fn read(&mut self, chunk: &mut Chunk, version: u32) {
        if read_nested(self, chunk, version, RampColor::new, RampColor::read).is_ok() {
            for key in &mut self.keys {
                if key.value.keys.is_empty() {
                    insert_key(&mut key.value.keys, 0.0, default_grey());
                }
            }
        }
    }
}

impl AnimatedSingleRamp {
    /// `ReadAnimatedSingleRamp` (kind 0x202 and the transform channels).
    pub(crate) fn read(&mut self, chunk: &mut Chunk, version: u32) -> Result<(), Short> {
        read_nested(
            self,
            chunk,
            version,
            AnimatedSingle::new,
            AnimatedSingle::read,
        )
    }
}

/// Channels of an [`AnimatedTransform`].
pub const CHANNEL_COUNT: usize = 9;

/// The channel defaults: translation x, y, z, rotation x, y, z in degrees,
/// scale x, y, z.
pub const CHANNEL_DEFAULTS: [f32; CHANNEL_COUNT] = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0];

/// [`AnimatedTransform::channel_flags`] when every channel holds its default.
pub const ALL_CHANNELS_DEFAULT: u32 = 0x1FF;

/// `FxAnimatedTransform` (property kind 0x105): nine animated channels and
/// two words. Constructed (and `Reset`) with every channel holding one key
/// at time 0 whose value has the channel's default as its weight, the
/// packed modes `0x435` and the flags [`ALL_CHANNELS_DEFAULT`].
#[derive(Clone, Debug, PartialEq)]
pub struct AnimatedTransform {
    /// `(a << 8) | (b << 4) | c` with no masking (`SetPackedModes`);
    /// `GetPackedModes` hands out the three low nibbles.
    pub packed_modes: u32,
    /// Bit `i` set: channel `i` holds its default (`UpdateChannelFlags`).
    pub channel_flags: u32,
    /// The channels.
    pub channels: [AnimatedSingleRamp; CHANNEL_COUNT],
}

impl Default for AnimatedTransform {
    fn default() -> Self {
        Self::new()
    }
}

impl AnimatedTransform {
    /// The constructor, which ends with `Reset`.
    #[must_use]
    pub fn new() -> Self {
        Self {
            packed_modes: 0x435,
            channel_flags: ALL_CHANNELS_DEFAULT,
            channels: CHANNEL_DEFAULTS
                .map(|weight| NestedRamp::with_key(AnimatedSingle::with_weight(weight))),
        }
    }

    /// `GetPackedModes`: the three nibbles.
    #[must_use]
    pub const fn modes(&self) -> [u32; 3] {
        [
            (self.packed_modes >> 8) & 0xF,
            (self.packed_modes >> 4) & 0xF,
            self.packed_modes & 0xF,
        ]
    }

    /// `SetPackedModes(a, b, c)`.
    pub const fn set_modes(&mut self, a: u32, b: u32, c: u32) {
        self.packed_modes = (((a << 4) | b) << 4) | c;
    }

    /// `UpdateChannelFlags`: a channel's bit is set when it has no key, or
    /// when its first key's value has no key and a weight equal to the
    /// default, or exactly one key whose value equals it. (The reference
    /// tests key 0 once per key of the channel, which gives the same.)
    pub fn update_channel_flags(&mut self) {
        let mut flags = 0;
        for (index, channel) in self.channels.iter().enumerate() {
            let matches = channel.keys.first().is_none_or(|key| {
                let entry = &key.value;
                let value = match entry.key_count() {
                    0 => entry.weight,
                    1 => entry.keys()[0].value,
                    _ => return false,
                };
                value == CHANNEL_DEFAULTS[index]
            });
            if matches {
                flags |= 1 << index;
            }
        }
        self.channel_flags = flags;
    }

    /// `ReadAnimatedTransform`: four signed header bytes; the first three
    /// (sign-extended) are the packed modes. Only when the fourth is
    /// negative do the nine channel ramps follow, after which the flags are
    /// recomputed; a failed channel leaves the flags as they were.
    pub(crate) fn read(&mut self, chunk: &mut Chunk, version: u32) -> Result<(), Short> {
        let header = chunk.bytes::<4>()?;
        let sext = |b: u8| i32::from(i8::from_le_bytes([b])).cast_unsigned();
        self.set_modes(sext(header[0]), sext(header[1]), sext(header[2]));
        if header[3] < 0x80 {
            return Ok(());
        }
        for channel in &mut self.channels {
            channel.read(chunk, version)?;
        }
        self.update_channel_flags();
        Ok(())
    }
}
