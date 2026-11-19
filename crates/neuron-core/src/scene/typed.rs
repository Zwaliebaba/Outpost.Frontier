//! Typed channels: the channel classes of `SceneRuntime.cpp`
//! (`FTypedChannel` and its scalar, vector, quaternion and
//! position-quaternion subclasses) that play a channel archetype onto a
//! joint or an object. The fifteen virtual slots are methods here; the
//! class picks the behaviour each slot had in the reference's tables.
//!
//! A typed channel keeps its own clock (`time`), its play header (loop
//! flags, rate, scale, factor) and what it sampled last. The mixer reads
//! the sample ([`TypedChannel::copy_sampled`]) and blends it.
//!
//! Arithmetic follows the reference's operation order: `f64` throughout,
//! rounded to `f32` where it stores a `float` (decision T11).
//!
//! Where the reference dereferences a null pointer or reads memory it
//! never wrote (a sample before any succeeded, a channel without an
//! archetype asked for its duration, uninitialised members), the port
//! reads zeros or skips the step (divergence register).

use std::sync::Arc;

use crate::anim::{AnimScript, Channel};
use crate::math::{
    Matrix, Quaternion, Vector, matrix_to_quaternion, mul_matrix, mul_quat, quat_slerp,
    quaternion_to_matrix, transform_matrix,
};

use super::engine::{Engine, InstanceId};
use super::joint::value_count_for_code;
use super::sample::{self, frame_words};

/// `3.14159274f`.
const PI_F32: f32 = core::f32::consts::PI;
/// `6.28318548f`.
const TWO_PI_F32: f32 = core::f32::consts::TAU;

/// The bit of [`TypedChannel::play_flags`] `TypedApplyFromArchetype` sets.
pub const PLAY_POSED: u32 = 0x1;
/// The bit set when a non-looping channel ran into its end.
pub const PLAY_ENDED: u32 = 0x2;
/// The bit set when the channel was ticked this walk.
pub const PLAY_TICKED: u32 = 0x4;

/// A typed channel's class (`GetType`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TypedClass {
    /// 8: the base class (event channels).
    Event,
    /// 1: one float.
    Scalar,
    /// 2: a vector.
    Vector,
    /// 4: a quaternion.
    Quaternion,
    /// 6: a position and a quaternion.
    PosQuat,
}

impl TypedClass {
    /// The class for a type code (the archetype's flags).
    #[must_use]
    pub const fn from_type(code: u32) -> Option<Self> {
        match code {
            1 => Some(Self::Scalar),
            2 => Some(Self::Vector),
            4 => Some(Self::Quaternion),
            6 => Some(Self::PosQuat),
            8 => Some(Self::Event),
            _ => None,
        }
    }

    /// `GetType`.
    #[must_use]
    pub const fn code(self) -> u32 {
        match self {
            Self::Event => 8,
            Self::Scalar => 1,
            Self::Vector => 2,
            Self::Quaternion => 4,
            Self::PosQuat => 6,
        }
    }
}

/// The key a typed channel's mixer list hangs under (the `Bind` argument
/// of `BindMap`, a playback's root): an instance index, or -1.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct MixerKey(pub i32);

impl MixerKey {
    /// The key of an instance, -1 for none.
    #[must_use]
    pub fn of(instance: Option<InstanceId>) -> Self {
        Self(instance.map_or(-1, |i| i32::from_ne_bytes(i.index().to_ne_bytes())))
    }
}

/// `FAnimBindOut`, the 16-byte bind descriptor: what a typed channel
/// writes to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BindDesc {
    /// 0 a joint (the instance's joint parameters), 1 an object (the
    /// instance's world transform), 2 a sink only.
    pub kind: u32,
    /// The sink token (`ArgD`): event keys and loops are reported for it.
    pub extra: u32,
    /// The context token (`ArgE`) reported with them.
    pub context: u32,
    /// The instance; `None` is the reference's -1.
    pub instance: Option<InstanceId>,
}

impl Default for BindDesc {
    /// The descriptor of a new channel: kind 2, no instance.
    fn default() -> Self {
        Self {
            kind: 2,
            extra: 0,
            context: 0,
            instance: None,
        }
    }
}

/// The seven-dword play header `BindHeader` takes.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PlayHeader {
    /// Header 0: the start time; -1 the end, 0 the start, -2 where the
    /// channel stands.
    pub start: f32,
    /// Header 1: the loop flags (bit 1 loop, bit 2 ping-pong, bit 6 keep
    /// the object's pose, bits 3..=5 the axis a blend keeps).
    pub flags: u32,
    /// Header 2: the rate (negative plays backwards).
    pub rate: f32,
    /// Header 3: the blend duration.
    pub blend: f32,
    /// Header 4: the scale.
    pub scale: f32,
    /// Header 5: the position factor.
    pub factor: f32,
    /// Header 6: an object pose (orientation, then position) to play
    /// relative to.
    pub block: Option<[f32; 12]>,
}

/// Something a channel reports while it is ticked.
#[derive(Clone, Debug, PartialEq)]
pub enum ChannelEvent {
    /// `IChannelSink::OnKeys`: the event keys a tick crossed.
    Keys {
        /// The channel.
        channel: TypedId,
        /// The sink token.
        sink: u32,
        /// The context token.
        context: u32,
        /// The keys copied into the batch (at most 0x40).
        keys: Vec<sample::EventKey>,
        /// The batch's count: every key found.
        count: u32,
    },
    /// `IChannelSink::OnLoop`: an object channel wrapped around; the pose
    /// it wrapped from (orientation, then position).
    Loop {
        /// The channel.
        channel: TypedId,
        /// The sink token.
        sink: u32,
        /// The context token.
        context: u32,
        /// The pose.
        transform: [f32; 12],
    },
}

/// A typed channel's id (the reference hands out its address).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TypedId(pub u32);

/// The channel archetype a typed channel plays.
#[derive(Clone, Debug)]
pub enum ChannelSource {
    /// A track of a script.
    Script {
        /// The script.
        script: Arc<AnimScript>,
        /// The animation's name CRC.
        entry: u32,
        /// The track.
        track: usize,
    },
    /// A channel archetype of its own (`CreateFromObject`).
    Object(Arc<Channel>),
}

impl ChannelSource {
    /// The channel, when it loaded.
    #[must_use]
    pub fn channel(&self) -> Option<&Channel> {
        match self {
            Self::Script {
                script,
                entry,
                track,
            } => script
                .get_by_crc(*entry)?
                .tracks
                .get(*track)?
                .channel
                .as_ref()?
                .channel(),
            Self::Object(channel) => Some(channel),
        }
    }
}

/// `IChannelOwner`: drives a channel created without an archetype.
pub trait ChannelOwner: core::fmt::Debug {
    /// `Apply(sampled, key, bind, time)`: the channel's tick result.
    /// `sampled` is the channel's own sample (`GetSampled`: one word for a
    /// scalar, three for a vector, four for a quaternion, the applied
    /// position and orientation for a position-quaternion channel, none
    /// for the base class); what the owner writes there is the channel's
    /// sample afterwards, which the mixer blends (an aim event writes its
    /// joint's solved quaternion). `engine` is the engine the channel is
    /// ticked on, which the reference's owners reach through their own
    /// `IEngine` pointer (an aim event reads its chain's joints from it).
    fn apply(
        &mut self,
        engine: &Engine,
        sampled: &mut [f32],
        channel: TypedId,
        bind: &BindDesc,
        time: f32,
    ) -> u32;
}

/// `IChannelSink`: what a typed channel reports to the sink of its bind
/// descriptor while it is ticked, at the moment the reference calls it.
pub trait ChannelSink {
    /// `OnKeys(channel, context, batch)`: the event keys a tick crossed
    /// (at most 0x40 copied; `count` is every key found).
    fn on_keys(
        &mut self,
        channel: TypedId,
        sink: u32,
        context: u32,
        keys: &[sample::EventKey],
        count: u32,
    );
    /// `OnLoop(channel, transform, context)`: an object channel wrapped
    /// around; `transform` is the pose it wrapped from (orientation, then
    /// position). What the sink leaves there is the pose the channel
    /// rebinds from.
    fn on_loop(&mut self, channel: TypedId, sink: u32, context: u32, transform: &mut [f32; 12]);
}

/// A sink that records the calls as [`ChannelEvent`]s and changes no
/// pose.
impl ChannelSink for Vec<ChannelEvent> {
    fn on_keys(
        &mut self,
        channel: TypedId,
        sink: u32,
        context: u32,
        keys: &[sample::EventKey],
        count: u32,
    ) {
        self.push(ChannelEvent::Keys {
            channel,
            sink,
            context,
            keys: keys.to_vec(),
            count,
        });
    }

    fn on_loop(&mut self, channel: TypedId, sink: u32, context: u32, transform: &mut [f32; 12]) {
        self.push(ChannelEvent::Loop {
            channel,
            sink,
            context,
            transform: *transform,
        });
    }
}

/// Two frames and the fraction between them, as `Sample` writes them; a
/// failed sample leaves the previous values.
#[derive(Clone, Copy, Debug, Default)]
struct Keys {
    a: [f32; 7],
    b: [f32; 7],
    alpha: f32,
}

fn r(value: f64) -> f32 {
    value as f32
}

fn e(value: f32) -> f64 {
    f64::from(value)
}

fn vec3(w: &[f32]) -> Vector {
    Vector::new(w[0], w[1], w[2])
}

fn quat(w: &[f32]) -> Quaternion {
    Quaternion::new(w[0], w[1], w[2], w[3])
}

fn add3(a: Vector, b: Vector) -> Vector {
    Vector::new(r(e(a.x) + e(b.x)), r(e(a.y) + e(b.y)), r(e(a.z) + e(b.z)))
}

fn sub3(a: Vector, b: Vector) -> Vector {
    Vector::new(r(e(a.x) - e(b.x)), r(e(a.y) - e(b.y)), r(e(a.z) - e(b.z)))
}

/// `ChannelPayloadBytesFromFlags`.
#[must_use]
pub const fn payload_bytes(flags: u32) -> u32 {
    let mut bytes = 0;
    if flags & 1 != 0 {
        bytes = 4;
    }
    if flags & 2 != 0 {
        bytes += 0xC;
    }
    if flags & 4 != 0 {
        bytes += 0x10;
    }
    if flags & 8 != 0 {
        bytes = 4;
    }
    if flags & 0x40 != 0 {
        bytes += 6;
    }
    if flags & 0x80 != 0 {
        bytes += 6;
    }
    if flags & 0x100 != 0 {
        bytes += 8;
    }
    bytes
}

/// A typed channel.
#[derive(Debug)]
pub struct TypedChannel {
    class: TypedClass,
    owner: Option<Box<dyn ChannelOwner>>,
    source: Option<ChannelSource>,
    /// The bind descriptor.
    pub bind: BindDesc,
    /// The mixer key.
    pub key: MixerKey,
    /// The channel's clock.
    pub time: f32,
    /// The loop flags of the header it was bound with.
    pub loop_flags: u32,
    /// The rate (`Weight`).
    pub rate: f32,
    /// The scale.
    pub scale: f32,
    /// The position factor (`Field_0x30`).
    pub factor: f32,
    /// [`PLAY_POSED`], [`PLAY_ENDED`], [`PLAY_TICKED`].
    pub play_flags: u32,
    /// Bit 0: playing outside a mixer.
    pub play_state: u8,
    /// The sample of a scalar, vector or quaternion channel.
    pub sampled: [f32; 4],
    /// A scalar channel's joint type (-1 until bound to a joint).
    pub joint_type: u32,
    /// Position-quaternion: the orientation the motion is played in.
    pub matrix: Matrix,
    /// Position-quaternion: the offset added to the motion.
    pub delta: Vector,
    /// Position-quaternion: the velocity `UpdateDeltaRate` measured.
    pub step_delta: Vector,
    /// Position-quaternion: the position applied last.
    pub applied_pos: Vector,
    /// Position-quaternion: the orientation applied last.
    pub applied_quat: Quaternion,
}

impl TypedChannel {
    fn new(
        class: TypedClass,
        owner: Option<Box<dyn ChannelOwner>>,
        source: Option<ChannelSource>,
    ) -> Self {
        Self {
            class,
            owner,
            source,
            bind: BindDesc {
                kind: 2,
                extra: 0,
                context: 0,
                instance: None,
            },
            key: MixerKey(0),
            time: 0.0,
            loop_flags: 0,
            rate: 1.0,
            scale: 1.0,
            factor: 1.0,
            play_flags: 0,
            play_state: 0,
            sampled: [0.0; 4],
            joint_type: 0xFFFF_FFFF,
            matrix: Matrix::default(),
            delta: Vector::ZERO,
            step_delta: Vector::ZERO,
            applied_pos: Vector::ZERO,
            applied_quat: Quaternion::new(0.0, 0.0, 0.0, 0.0),
        }
    }

    /// A channel playing `source`, of the class its flags name.
    #[must_use]
    pub fn with_source(class: TypedClass, source: ChannelSource) -> Self {
        Self::new(class, None, Some(source))
    }

    /// A channel an owner drives.
    #[must_use]
    pub fn with_owner(class: TypedClass, owner: Box<dyn ChannelOwner>) -> Self {
        Self::new(class, Some(owner), None)
    }

    /// The class.
    #[must_use]
    pub fn class(&self) -> TypedClass {
        self.class
    }

    /// The archetype's source.
    #[must_use]
    pub fn source(&self) -> Option<&ChannelSource> {
        self.source.as_ref()
    }

    /// The channel archetype, when there is one and it loaded.
    #[must_use]
    pub fn channel(&self) -> Option<&Channel> {
        self.source.as_ref()?.channel()
    }

    fn duration(&self) -> Option<f32> {
        self.channel().map(Channel::duration)
    }

    fn flags(&self) -> u32 {
        self.channel().map_or(0, Channel::flags)
    }

    /// `Sample` into `keys`; false (and `keys` unchanged) on failure.
    fn sample_into(&self, time: f32, keys: &mut Keys) -> bool {
        let Some(channel) = self.channel() else {
            return false;
        };
        match sample::sample(channel, time) {
            Ok(s) => {
                keys.a = frame_words(channel, s.a);
                keys.b = frame_words(channel, s.b);
                keys.alpha = s.alpha;
                true
            }
            Err(_) => false,
        }
    }

    /// `GetFrame(last)`'s payload, zeros where the reference reads none.
    fn frame(&self, last: bool) -> [f32; 7] {
        self.channel()
            .and_then(|c| sample::frame(c, last).map(|i| frame_words(c, i)))
            .unwrap_or_default()
    }

    // --- slot 0, 3: interpolation ---------------------------------------------------------

    /// Slot 0 (`Interpolate`): the position-quaternion lerp, its rotation
    /// scaled; nothing for the other classes.
    fn interpolate(&self, out: &mut [f32; 7], left: &[f32; 7], right: &[f32; 7], alpha: f64) {
        if self.class != TypedClass::PosQuat {
            return;
        }
        self.interpolate_pose(out, left, right, alpha);
        if self.scale.to_bits() == 0x3F80_0000 {
            return;
        }
        let q = quat_slerp(&Quaternion::IDENTITY, &quat(&out[3..7]), self.scale);
        out[3..7].copy_from_slice(&q.to_words());
    }

    /// Slot 3 (`InterpolatePose`): the class's interpolation of two values
    /// (frames, or the mixer's inputs) into `out`.
    pub fn interpolate_pose(
        &self,
        out: &mut [f32; 7],
        left: &[f32; 7],
        right: &[f32; 7],
        alpha: f64,
    ) {
        match self.class {
            TypedClass::Event => {}
            TypedClass::Scalar => {
                let (l, rt) = (e(left[0]), e(right[0]));
                let sum = if self.joint_type == 2 {
                    let delta = rt - l;
                    let target = if delta < e(-PI_F32) {
                        rt + e(TWO_PI_F32)
                    } else if delta > e(PI_F32) {
                        rt - e(TWO_PI_F32)
                    } else {
                        rt
                    };
                    (target - l) * alpha + l
                } else {
                    (rt - l) * alpha + l
                };
                out[0] = r(sum);
                out[0] = r(e(self.scale) * e(out[0]));
            }
            TypedClass::Vector => {
                let d0 = e(right[0]) - e(left[0]);
                let d1 = e(right[1]) - e(left[1]);
                let d2 = r(e(right[2]) - e(left[2]));
                let blend = alpha as f32;
                let t0 = r(d0 * e(blend));
                let t1 = d1 * e(blend);
                let t2 = e(d2) * e(blend);
                out[0] = r(e(t0) + e(left[0]));
                out[1] = r(t1 + e(left[1]));
                out[2] = r(t2 + e(left[2]));
                let factor = e(self.factor) * e(self.scale);
                for v in &mut out[..3] {
                    *v = r(factor * e(*v));
                }
            }
            TypedClass::Quaternion => {
                let mut q = quat_slerp(&quat(&left[..4]), &quat(&right[..4]), alpha as f32);
                if self.scale.to_bits() != 0x3F80_0000 {
                    q = quat_slerp(&Quaternion::IDENTITY, &q, self.scale);
                }
                out[..4].copy_from_slice(&q.to_words());
            }
            TypedClass::PosQuat => {
                for i in 0..3 {
                    out[i] = r((e(right[i]) - e(left[i])) * alpha + e(left[i]));
                }
                let q = quat_slerp(&quat(&left[3..7]), &quat(&right[3..7]), alpha as f32);
                out[3..7].copy_from_slice(&q.to_words());
            }
        }
    }

    // --- slot 2: apply -------------------------------------------------------------------

    /// Slot 2 (`Apply`): poses the channel from two frames.
    fn apply(&mut self, left: &[f32; 7], right: &[f32; 7], alpha: f64) {
        match self.class {
            TypedClass::Event => {}
            TypedClass::Scalar | TypedClass::Vector | TypedClass::Quaternion => {
                let mut out = [0.0; 7];
                out[..4].copy_from_slice(&self.sampled);
                self.interpolate_pose(&mut out, left, right, alpha);
                self.sampled.copy_from_slice(&out[..4]);
            }
            TypedClass::PosQuat => {
                let mut local = [0.0; 7];
                self.interpolate(&mut local, left, right, alpha);
                if self.bind.kind == 1 {
                    let factor = e(self.factor) * e(self.scale);
                    let pos = Vector::new(
                        r(e(local[0]) * factor),
                        r(e(local[1]) * factor),
                        r(e(local[2]) * factor),
                    );
                    let xformed = transform_matrix(&self.matrix, pos);
                    self.applied_pos = add3(xformed, self.delta);
                    let mq = matrix_to_quaternion(&self.matrix);
                    self.applied_quat = mul_quat(&mq, &quat(&local[3..7]));
                } else if self.bind.kind == 0 {
                    self.applied_pos = vec3(&local[..3]);
                    self.applied_quat = quat(&local[3..7]);
                }
            }
        }
    }

    // --- slot 1, 4: binding and time ---------------------------------------------------

    /// Slot 4 (`SetTime`): -1 the end, 0 the start, -2 leaves the time;
    /// another negative time is refused (false).
    pub fn set_time(&mut self, time: f32) -> bool {
        if time == -1.0 {
            self.time = self.duration().unwrap_or(time);
            return true;
        }
        if time == 0.0 {
            self.time = 0.0;
            return true;
        }
        if time == -2.0 {
            return true;
        }
        if time < 0.0 {
            return false;
        }
        self.time = time;
        true
    }

    /// `TypedBind`, the base of slot 1.
    fn bind_base(&mut self, header: &PlayHeader) {
        self.loop_flags = header.flags;
        self.rate = header.rate;
        self.scale = header.scale;
        self.factor = header.factor;
        self.set_time(header.start);
        self.play_flags &= !3;
    }

    /// Slot 1 (`BindHeader`). A position-quaternion channel bound to an
    /// object also fixes the frame its motion plays in: the header's pose,
    /// or the instance's current one, minus where the motion starts.
    pub fn bind_header(&mut self, engine: &Engine, header: &PlayHeader) {
        self.bind_base(header);
        if self.class != TypedClass::PosQuat {
            return;
        }
        if self.bind.kind != 1 {
            self.matrix = Matrix::IDENTITY;
            self.delta = Vector::ZERO;
            return;
        }
        let instance = self.bind.instance;
        if header.start == 0.0 || header.flags & 0x40 != 0 {
            match &header.block {
                Some(block) => {
                    self.matrix = Matrix::from_words(core::array::from_fn(|i| block[i]));
                    self.delta = vec3(&block[9..12]);
                }
                None => {
                    self.matrix = engine.instance_orientation(instance);
                    self.delta = engine.instance_position(instance);
                }
            }
            return;
        }
        let mut keys = Keys::default();
        self.sample_into(self.time, &mut keys);
        let mut pose = [0.0; 7];
        self.interpolate(&mut pose, &keys.a, &keys.b, e(keys.alpha));
        let factor = e(self.factor) * e(self.scale);
        let pos = Vector::new(
            r(e(pose[0]) * factor),
            r(e(pose[1]) * factor),
            r(e(pose[2]) * factor),
        );
        match &header.block {
            Some(block) => {
                self.matrix = Matrix::from_words(core::array::from_fn(|i| block[i]));
                let xformed = transform_matrix(&self.matrix, pos);
                self.delta = sub3(vec3(&block[9..12]), xformed);
            }
            None => {
                let q = quaternion_to_matrix(&quat(&pose[3..7])).transpose();
                let orient = engine.instance_orientation(instance);
                self.matrix = mul_matrix(&orient, &q);
                let xformed = transform_matrix(&self.matrix, pos);
                self.delta = sub3(engine.instance_position(instance), xformed);
            }
        }
    }

    /// `TypedTimePlayable`: whether the header's start time can be played
    /// (-1, 0, -2, or a time inside the channel). A channel without an
    /// archetype cannot play another time.
    #[must_use]
    pub fn time_playable(&self, start: f32) -> bool {
        let bits = start.to_bits();
        if bits == 0xBF80_0000 || start == 0.0 || bits == 0xC000_0000 {
            return true;
        }
        let Some(duration) = self.duration() else {
            return false;
        };
        start <= duration && start >= 0.0
    }

    /// `TypedTryBindIfTimeValid`.
    pub fn try_bind(&mut self, engine: &Engine, header: &PlayHeader) -> bool {
        let ok = self.time_playable(header.start);
        if ok {
            self.bind_header(engine, header);
        }
        ok
    }

    /// Slot 7 (`BindMap`): takes the descriptor and the mixer key; whether
    /// the channel's frames fit what it binds to (a joint's parameter
    /// count, seven floats for an object, an event channel for a sink).
    pub fn bind_map(&mut self, engine: &Engine, bind: BindDesc, key: MixerKey) -> bool {
        self.bind = bind;
        self.key = key;
        if self.owner.is_some() {
            return true;
        }
        let flags = self.flags();
        let bytes = payload_bytes(flags);
        let bound = match bind.kind {
            0 => {
                let count = engine
                    .instance_relative(bind.instance)
                    .map_or(0, |rel| value_count_for_code(rel.code()));
                count * 4 == bytes as usize
            }
            2 => flags == 8,
            _ => bytes == 0x1C,
        };
        if bound
            && self.class == TypedClass::Scalar
            && let Some(rel) = engine.instance_relative(bind.instance)
        {
            self.joint_type = rel.code();
        }
        bound
    }

    // --- slot 6, 11: samples -----------------------------------------------------------

    /// Slot 6 (`CopySampled`): the words the mixer reads; nothing for the
    /// base class.
    pub fn copy_sampled(&self, out: &mut [f32; 7]) {
        match self.class {
            TypedClass::Event => {}
            TypedClass::Scalar => out[0] = self.sampled[0],
            TypedClass::Vector => out[..3].copy_from_slice(&self.sampled[..3]),
            TypedClass::Quaternion => out[..4].copy_from_slice(&self.sampled),
            TypedClass::PosQuat => {
                out[..3].copy_from_slice(&self.applied_pos.to_words());
                out[3..].copy_from_slice(&self.applied_quat.to_words());
            }
        }
    }

    /// Slot 11 (`GetSampled`): what an owner is handed.
    #[must_use]
    pub fn sampled_words(&self) -> Vec<f32> {
        let mut out = [0.0; 7];
        self.copy_sampled(&mut out);
        let n = match self.class {
            TypedClass::Event => 0,
            TypedClass::Scalar => 1,
            TypedClass::Vector => 3,
            TypedClass::Quaternion => 4,
            TypedClass::PosQuat => 7,
        };
        out[..n].to_vec()
    }

    /// Writes words of [`TypedChannel::sampled_words`]' layout back into
    /// the channel's sample: what an owner left in the buffer `GetSampled`
    /// handed it.
    fn store_sampled(&mut self, words: &[f32]) {
        let word = |i: usize| words.get(i).copied().unwrap_or(0.0);
        match self.class {
            TypedClass::Event => {}
            TypedClass::Scalar => self.sampled[0] = word(0),
            TypedClass::Vector => {
                for (i, slot) in self.sampled.iter_mut().take(3).enumerate() {
                    *slot = word(i);
                }
            }
            TypedClass::Quaternion => {
                for (i, slot) in self.sampled.iter_mut().enumerate() {
                    *slot = word(i);
                }
            }
            TypedClass::PosQuat => {
                self.applied_pos = Vector::from_words([word(0), word(1), word(2)]);
                self.applied_quat = Quaternion::from_words([word(3), word(4), word(5), word(6)]);
            }
        }
    }

    // --- posing ------------------------------------------------------------------------

    /// `TypedApplyFromArchetype`: poses the channel at its time (at the
    /// end it ran into, once ended) without advancing it.
    pub fn apply_from_archetype(&mut self) {
        self.play_flags |= PLAY_POSED;
        let Some(duration) = self.duration() else {
            return;
        };
        if self.flags() == 8 {
            return;
        }
        let mut time = self.time;
        if self.play_flags & PLAY_ENDED != 0 {
            time = if self.rate < 0.0 { 0.0 } else { duration };
        }
        let mut keys = Keys::default();
        self.sample_into(time, &mut keys);
        self.apply(&keys.a, &keys.b, e(keys.alpha));
    }

    /// `TypedCollect`: the event keys from `t0` to `t1`, wrapping or
    /// bouncing at the ends as the loop flags say. True when a channel
    /// that does neither ran into its end.
    fn collect(
        &mut self,
        t0: f32,
        t1: f32,
        mode: u32,
        cap: u32,
        keys: &mut Vec<sample::EventKey>,
        count: &mut u32,
    ) -> bool {
        let Some(source) = self.source.clone() else {
            return false;
        };
        let Some(channel) = source.channel() else {
            return false;
        };
        let duration = channel.duration();
        let push = |c: sample::Collected, keys: &mut Vec<sample::EventKey>, count: &mut u32| {
            let room = cap.saturating_sub(*count) as usize;
            keys.extend(c.keys.into_iter().take(room));
            *count = count.wrapping_add(c.count);
        };
        let left = |count: &u32| cap.saturating_sub(*count);
        let wrap = if super::not_lt(e(self.rate), 0.0) {
            !(t1 < duration || t1.is_nan())
        } else {
            t1 <= 0.0
        };
        if !wrap {
            if mode & 1 != 0 {
                push(sample::collect_at(channel, t0, left(count)), keys, count);
            }
            if t0 != t1 {
                push(
                    sample::collect_range(channel, t0, t1, left(count)),
                    keys,
                    count,
                );
                if mode & 2 != 0 {
                    push(sample::collect_at(channel, t1, left(count)), keys, count);
                }
            }
            self.time = t1;
            return false;
        }
        let span = if self.rate < 0.0 {
            -t0
        } else {
            r(e(duration) - e(t0))
        };
        if mode & 1 != 0 {
            push(sample::collect_at(channel, t0, left(count)), keys, count);
        }
        if span != 0.0 {
            let end = r(e(span) + e(t0));
            push(
                sample::collect_range(channel, t0, end, left(count)),
                keys,
                count,
            );
            if mode & 2 != 0 {
                push(sample::collect_at(channel, end, left(count)), keys, count);
            }
        }
        let forward = super::not_lt(e(self.rate), 0.0);
        if self.loop_flags & 2 != 0 {
            if forward {
                self.collect(0.0, r(e(t1) - e(duration)), 3, cap, keys, count);
            } else {
                self.collect(duration, r(e(duration) + e(t1)), 3, cap, keys, count);
            }
            return false;
        }
        if self.loop_flags & 4 != 0 {
            let turn = if forward { duration } else { 0.0 };
            self.rate = -self.rate;
            self.collect(turn, r(e(turn) - e(t1)), 2, cap, keys, count);
            return false;
        }
        self.time = if forward { duration } else { 0.0 };
        self.play_flags |= PLAY_ENDED;
        true
    }

    /// `TypedTick(dt)`: advances the clock by `dt * rate`, wraps, bounces
    /// or stops at the ends, and poses the channel. Returns 1 when a
    /// channel that does not loop ran into its end this tick and nothing
    /// posed it since; an owner's channel answers what the owner does.
    pub fn tick(
        &mut self,
        engine: &Engine,
        id: TypedId,
        dt: f32,
        sink: &mut dyn ChannelSink,
    ) -> u32 {
        let mut result = 0;
        self.play_flags |= PLAY_TICKED;
        if self.play_flags & 3 != 0 {
            return 0;
        }
        let old_time = self.time;
        let new_time = r(e(dt) * e(self.rate) + e(self.time));
        self.time = new_time;
        if self.owner.is_some() {
            let mut sampled = self.sampled_words();
            let bind = self.bind;
            let result = self.owner.as_mut().map_or(0, |owner| {
                owner.apply(engine, &mut sampled, id, &bind, new_time)
            });
            self.store_sampled(&sampled);
            return result;
        }
        if self.flags() == 8 {
            let mut keys = Vec::new();
            let mut count = 0;
            let ended = self.collect(old_time, self.time, 3, 0x40, &mut keys, &mut count);
            result = u32::from(ended);
            if count == 0 || self.bind.extra == 0 {
                return result;
            }
            keys.truncate(0x40);
            sink.on_keys(id, self.bind.extra, self.bind.context, &keys, count);
            return result;
        }
        let Some(duration) = self.duration() else {
            return result;
        };
        let remaining = if self.rate < 0.0 {
            e(self.time)
        } else {
            e(duration) - e(self.time)
        };
        if remaining <= 0.0 {
            let mut wraps = 0u32;
            if duration != 0.0 {
                let low = crate::num::trunc_to_i32_via_i64(remaining / e(duration));
                wraps = low.unsigned_abs();
            }
            let frac = r(remaining.abs() - f64::from(wraps) * e(duration));
            let flags = self.loop_flags;
            if flags & 2 != 0 {
                let mut block = None;
                if self.bind.kind == 1 {
                    let frame = self.frame(super::not_lt(e(self.rate), 0.0));
                    self.apply(&frame, &frame, e(dt));
                    let m = quaternion_to_matrix(&self.applied_quat).to_words();
                    let mut pose = [0.0; 12];
                    pose[..9].copy_from_slice(&m);
                    pose[9..].copy_from_slice(&self.applied_pos.to_words());
                    if self.bind.extra != 0 {
                        sink.on_loop(id, self.bind.extra, self.bind.context, &mut pose);
                    }
                    block = Some(pose);
                }
                let start = if self.rate < 0.0 { duration } else { 0.0 };
                let header = PlayHeader {
                    start,
                    flags,
                    rate: self.rate,
                    blend: 0.0,
                    scale: self.scale,
                    factor: self.factor,
                    block,
                };
                self.bind_header(engine, &header);
                if duration != 0.0 {
                    self.time = if self.rate < 0.0 {
                        r(e(duration) - e(frac))
                    } else {
                        frac
                    };
                }
            } else if flags & 4 != 0 {
                let mut rate = self.rate;
                if wraps & 1 == 0 {
                    rate = -rate;
                }
                let start = if rate < 0.0 {
                    r(e(duration) - e(frac))
                } else {
                    frac
                };
                let header = PlayHeader {
                    start,
                    flags,
                    rate,
                    blend: 0.0,
                    scale: self.scale,
                    factor: self.factor,
                    block: None,
                };
                self.bind_header(engine, &header);
            } else {
                self.time = if self.rate < 0.0 { 0.0 } else { duration };
                self.play_flags |= PLAY_ENDED;
                if self.play_flags & PLAY_POSED == 0 {
                    result = 1;
                }
            }
        }
        let mut keys = Keys::default();
        self.sample_into(self.time, &mut keys);
        self.apply(&keys.a, &keys.b, e(keys.alpha));
        result
    }

    // --- position-quaternion slots 8..=10, 12..=14 ---------------------------------------

    /// Slot 8 (`SetOverlayPos`): moves an object channel so that its
    /// current position is `target`.
    pub fn set_overlay_pos(&mut self, target: Vector) {
        if self.class != TypedClass::PosQuat || self.bind.kind != 1 {
            return;
        }
        let mut keys = Keys::default();
        self.sample_into(self.time, &mut keys);
        let (a, b, alpha) = (&keys.a, &keys.b, e(keys.alpha));
        let d0 = e(b[0]) - e(a[0]);
        let d1 = e(b[1]) - e(a[1]);
        let d2 = r(e(b[2]) - e(a[2]));
        let v0 = r(d0 * alpha);
        let p0 = r(e(v0) + e(a[0]));
        let p1 = r(d1 * alpha + e(a[1]));
        let p2 = e(d2) * alpha + e(a[2]);
        let p0s = r(e(p0) * e(self.scale));
        let p1s = r(e(p1) * e(self.scale));
        let p2s = p2 * e(self.scale);
        let pos = Vector::new(
            r(e(p0s) * e(self.factor)),
            r(e(p1s) * e(self.factor)),
            r(p2s * e(self.factor)),
        );
        let xformed = transform_matrix(&self.matrix, pos);
        self.delta = sub3(target, xformed);
        self.applied_pos = target;
    }

    /// Slot 9 (`SetOverlayExtra`): turns an object channel so that its
    /// frame becomes `orient` relative to the instance's orientation,
    /// keeping its position.
    pub fn set_overlay_orientation(&mut self, engine: &Engine, orient: &Matrix) {
        if self.class != TypedClass::PosQuat || self.bind.kind != 1 {
            return;
        }
        let mut keys = Keys::default();
        self.sample_into(self.time, &mut keys);
        let mut pose = [0.0; 7];
        self.interpolate(&mut pose, &keys.a, &keys.b, e(keys.alpha));
        let pos = vec3(&pose[..3]);
        let q = quat(&pose[3..7]);
        let transposed = engine.instance_orientation(self.bind.instance).transpose();
        let r1 = mul_matrix(orient, &transposed);
        let r2 = mul_matrix(&r1, &self.matrix);
        let world = add3(transform_matrix(&self.matrix, pos), self.delta);
        self.delta = sub3(world, transform_matrix(&r2, pos));
        self.matrix = r2;
        self.applied_pos = add3(transform_matrix(&self.matrix, pos), self.delta);
        let r3 = mul_matrix(&r2, &quaternion_to_matrix(&q));
        self.applied_quat = matrix_to_quaternion(&r3);
    }

    /// Slot 10 (`AddOverlayPos`): shifts an object channel by `add`, posing
    /// it again when it is posed.
    pub fn add_overlay_pos(&mut self, add: Vector) {
        if self.class != TypedClass::PosQuat {
            return;
        }
        self.delta = add3(self.delta, add);
        if self.play_flags & PLAY_POSED != 0 {
            self.apply_from_archetype();
        }
    }

    /// Slot 12 (`TouchMapped`): the position-quaternion channel's current
    /// position (or its end once ended): the transformed position and the
    /// raw sample.
    #[must_use]
    pub fn touch_mapped(&self) -> Option<(Vector, Vector)> {
        if self.class != TypedClass::PosQuat {
            return None;
        }
        let mut keys = Keys::default();
        if self.play_flags & PLAY_ENDED != 0 {
            let frame = self.frame(super::not_lt(e(self.rate), 0.0));
            keys.a = frame;
            keys.b = frame;
        } else {
            self.sample_into(self.time, &mut keys);
        }
        let mut pose = [0.0; 7];
        self.interpolate(&mut pose, &keys.a, &keys.b, e(keys.alpha));
        let sampled = vec3(&pose[..3]);
        let out = add3(transform_matrix(&self.matrix, sampled), self.delta);
        Some((out, sampled))
    }

    /// `TypedType6SampleXform`: the scaled position at `time`, transformed.
    fn scaled_position_at(&self, time: f32, keys: &mut Keys) -> Vector {
        self.sample_into(time, keys);
        let (a, b, alpha) = (&keys.a, &keys.b, e(keys.alpha));
        let factor = e(self.scale) * e(self.factor);
        let v: [f32; 3] = core::array::from_fn(|i| {
            let d = r((e(b[i]) - e(a[i])) * alpha);
            let p = r(e(d) + e(a[i]));
            r(e(p) * factor)
        });
        transform_matrix(&self.matrix, Vector::from_words(v))
    }

    /// Slot 13 (`UpdateDeltaRate`): the velocity over the next `dt`.
    pub fn update_delta_rate(&mut self, dt: f32) {
        if self.class != TypedClass::PosQuat {
            return;
        }
        let shifted = if e(self.rate) < 0.0 {
            r(e(self.time) - e(dt))
        } else {
            r(e(self.time) + e(dt))
        };
        let mut keys = Keys::default();
        let now = add3(self.scaled_position_at(self.time, &mut keys), self.delta);
        let x = self.scaled_position_at(shifted, &mut keys);
        let s0 = e(x.x) + e(self.delta.x);
        let s1 = e(x.y) + e(self.delta.y);
        let s2 = r(e(x.z) + e(self.delta.z));
        let dx = r(s0 - e(now.x));
        let dy = s1 - e(now.y);
        let dz = e(s2) - e(now.z);
        let inv = 1.0 / e(dt);
        self.step_delta = Vector::new(r(e(dx) * inv), r(dy * inv), r(dz * inv));
    }

    /// `TypedType6ScaledFrame`.
    fn scaled_frame(&self, last: bool) -> [f32; 3] {
        let frame = self.frame(last);
        core::array::from_fn(|i| {
            let scaled = r(e(self.scale) * e(frame[i]));
            r(e(scaled) * e(self.factor))
        })
    }

    /// Slot 14 (`Step`, the mixer's `SampleDisplacement`): how far the
    /// position moves over the next `dt` (across a loop's wrap), turned
    /// into the channel's frame; zero for the other classes.
    #[must_use]
    pub fn step(&self, dt: f32) -> Vector {
        if self.class != TypedClass::PosQuat {
            return Vector::ZERO;
        }
        let wdt = r(e(dt) * e(self.rate));
        let duration = self.duration().unwrap_or(0.0);
        let mut step = [0.0f32; 3];
        let reverse = e(self.rate) < 0.0;
        let wide = if reverse {
            e(self.time) - e(wdt)
        } else {
            e(self.time) + e(wdt)
        };
        let sample_time = r(wide);
        let crossed = if reverse {
            wide < 0.0
        } else {
            wide > e(duration)
        };
        let mut keys = Keys::default();
        self.sample_into(self.time, &mut keys);
        let lerp = |keys: &Keys| -> [f32; 3] {
            core::array::from_fn(|i| {
                let d = r(e(keys.b[i]) - e(keys.a[i]));
                let d = r(e(d) * e(keys.alpha));
                r(e(keys.a[i]) + e(d))
            })
        };
        let factor = e(self.scale) * e(self.factor);
        let l = lerp(&keys);
        let now: [f32; 3] = core::array::from_fn(|i| r(e(l[i]) * factor));
        if crossed && self.loop_flags & 2 != 0 {
            let end_a = self.scaled_frame(!reverse);
            for i in 0..3 {
                step[i] = r(e(end_a[i]) - e(now[i]));
            }
            let end_b = self.scaled_frame(reverse);
            let leftover = if reverse {
                r(e(duration) - e(sample_time))
            } else {
                r(e(sample_time) - e(duration))
            };
            self.sample_into(leftover, &mut keys);
            let l = lerp(&keys);
            let wrapped: [f32; 3] = core::array::from_fn(|i| r(e(l[i]) * factor - e(end_b[i])));
            step[0] = r(e(wrapped[0]) + e(step[0]));
            step[1] = r(e(step[1]) + e(wrapped[1]));
            step[2] = r(e(step[2]) + e(wrapped[2]));
        } else if self.sample_into(sample_time, &mut keys) {
            let (a, b, alpha) = (&keys.a, &keys.b, e(keys.alpha));
            let diff: [f32; 3] = core::array::from_fn(|i| r(e(b[i]) - e(a[i])));
            let sh0 = r(e(diff[0]) * alpha + e(a[0]));
            let sh1 = r(e(diff[1]) * alpha + e(a[1]));
            let d2a = r(e(diff[2]) * alpha);
            let sh2 = r(e(d2a) + e(a[2]));
            let sc1 = r(e(sh1) * factor);
            let sc2 = r(e(sh2) * factor);
            step[0] = r(e(sh0) * factor - e(now[0]));
            step[1] = r(e(sc1) - e(now[1]));
            step[2] = r(e(sc2) - e(now[2]));
        }
        transform_matrix(&self.matrix, Vector::from_words(step))
    }
}

/// `MixerJointValueCount` of an instance's joint: how many words the mixer
/// writes for a joint channel.
#[must_use]
pub fn joint_value_count(engine: &Engine, instance: Option<InstanceId>) -> usize {
    engine
        .instance_relative(instance)
        .map_or(0, |rel| value_count_for_code(rel.code()))
}

#[cfg(test)]
mod owner_tests {
    use super::*;

    /// Writes a fixed quaternion into the sample, as an aim event does.
    #[derive(Debug)]
    struct Aim(u32);

    impl ChannelOwner for Aim {
        fn apply(
            &mut self,
            _engine: &Engine,
            sampled: &mut [f32],
            _channel: TypedId,
            _bind: &BindDesc,
            _time: f32,
        ) -> u32 {
            self.0 += 1;
            sampled.copy_from_slice(&[0.0, 0.6, 0.0, 0.8][..sampled.len()]);
            7
        }
    }

    #[test]
    fn an_owner_writes_the_channels_sample() {
        let engine = Engine::new();
        let mut events = Vec::new();
        let mut quat = TypedChannel::with_owner(TypedClass::Quaternion, Box::new(Aim(0)));
        assert_eq!(quat.tick(&engine, TypedId(1), 0.1, &mut events), 7);
        assert_eq!(quat.sampled, [0.0, 0.6, 0.0, 0.8]);
        let mut out = [0.0; 7];
        quat.copy_sampled(&mut out);
        assert_eq!(&out[..4], &[0.0, 0.6, 0.0, 0.8]);

        let mut scalar = TypedChannel::with_owner(TypedClass::Scalar, Box::new(Aim(0)));
        scalar.tick(&engine, TypedId(2), 0.1, &mut events);
        assert_eq!(scalar.sampled[0], 0.0);

        let mut event = TypedChannel::with_owner(TypedClass::Event, Box::new(Aim(0)));
        assert_eq!(event.tick(&engine, TypedId(3), 0.1, &mut events), 7);
        assert!(events.is_empty());
    }
}
