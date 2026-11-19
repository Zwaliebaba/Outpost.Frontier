//! The `Animation` component of `SceneRuntime.cpp`: scripts playing on
//! instances (`IAnimation2`: `PlayScript`, `EvalPlayback`, `MixerWalk`,
//! ...) and free typed channels (`IChannel`).
//!
//! `play_script` makes one typed channel per track of the named animation
//! and binds each to its target under the root: a joint map to the part
//! named by `Child name` whose parent is `Parent name`, an object map to
//! the part named by `Parent name` (or the root), a map without names to a
//! sink. A channel whose target is missing, or whose frames do not fit the
//! target, is dropped with a warning. `eval_playback` starts the channels
//! with a play header and attaches them to the mixer; `mixer_walk` and
//! `mixer_walk_all` advance them and write the engine.
//!
//! Playback ids start at 0xED as in the reference. Scripts are shared
//! (`Arc`), so a playback keeps the animation it was made from.
//!
//! The reference's handle registry (`RegisterHandle`) is a global list;
//! here it belongs to the component.

use std::collections::BTreeMap;
use std::sync::Arc;

use crate::anim::{AnimScript, Channel, ScriptEntry, Track};
use crate::diag::{DumpFlags, dump};
use crate::hash::name_crc32;
use crate::math::{Matrix, Vector, matrix_to_quaternion};

use super::engine::{Engine, InstanceId};
use super::mixer::{self, BindKey, Channels, MixerMap, NodeKind};
use super::sample::EventKey;
use super::typed::{
    BindDesc, ChannelEvent, ChannelOwner, ChannelSink, ChannelSource, MixerKey, PLAY_TICKED,
    PlayHeader, TypedChannel, TypedClass, TypedId,
};

/// `G_AnimFirstPlaybackId`.
pub const FIRST_PLAYBACK_ID: u32 = 0xED;

/// A playing animation (`FAnimPlayback`).
#[derive(Clone, Debug)]
pub struct Playback {
    /// The script.
    pub script: Arc<AnimScript>,
    /// The animation's name CRC in the script.
    pub entry: u32,
    /// The root it was bound under.
    pub root: Option<InstanceId>,
    /// The channels that bound, in track order.
    pub typed: Vec<TypedId>,
    /// The bound channel with the longest duration.
    pub longest: Option<TypedId>,
}

impl Playback {
    /// The animation.
    #[must_use]
    pub fn entry(&self) -> Option<&ScriptEntry> {
        self.script.get_by_crc(self.entry)
    }
}

/// The typed channels by id (the reference's channel set).
#[derive(Debug, Default)]
pub struct ChannelSet {
    slots: Vec<Option<TypedChannel>>,
}

impl ChannelSet {
    fn insert(&mut self, typed: TypedChannel) -> TypedId {
        self.slots.push(Some(typed));
        TypedId(u32::try_from(self.slots.len() - 1).unwrap_or(u32::MAX))
    }

    fn remove(&mut self, id: TypedId) -> Option<TypedChannel> {
        self.slots.get_mut(id.0 as usize)?.take()
    }

    fn ids(&self) -> Vec<TypedId> {
        self.slots
            .iter()
            .enumerate()
            .filter(|(_, s)| s.is_some())
            .map(|(i, _)| TypedId(i as u32))
            .collect()
    }
}

impl Channels for ChannelSet {
    fn get(&self, id: TypedId) -> Option<&TypedChannel> {
        self.slots.get(id.0 as usize)?.as_ref()
    }

    fn get_mut(&mut self, id: TypedId) -> Option<&mut TypedChannel> {
        self.slots.get_mut(id.0 as usize)?.as_mut()
    }
}

/// The animation component.
#[derive(Debug)]
pub struct Animation {
    next_playback_id: u32,
    playbacks: BTreeMap<i32, Playback>,
    channels: ChannelSet,
    mixers: MixerMap,
    handles: Vec<u32>,
}

impl Default for Animation {
    fn default() -> Self {
        Self::new()
    }
}

fn instance_key(instance: Option<InstanceId>) -> u32 {
    instance.map_or(0xFFFF_FFFF, InstanceId::index)
}

fn playback_key(id: u32) -> i32 {
    i32::from_ne_bytes(id.to_ne_bytes())
}

/// `AnimEngineFindInstanceByName`: `from` or the first instance of its
/// subtree (depth first) whose part name is `name` exactly.
fn find_instance_by_name(
    engine: &Engine,
    from: Option<InstanceId>,
    name: &[u8],
) -> Option<InstanceId> {
    let named = |id: Option<InstanceId>| engine.instance_name(id).0.unwrap_or_default() == name;
    if named(from) {
        return from;
    }
    let mut cursor = engine.related_instance(from, 0, None);
    while let Some(at) = cursor {
        if named(Some(at)) {
            return Some(at);
        }
        cursor = engine.related_instance(from, 0, Some(at));
    }
    None
}

/// The playbacks as a sink sees them while the mixer runs: the
/// `IAnimation2` queries on playbacks a sink makes from inside its calls
/// (`HasObject`, `GetDurationIfNamed`, `GetFirstInstanceField`).
#[derive(Clone, Copy, Debug)]
pub struct PlaybackView<'a> {
    playbacks: &'a BTreeMap<i32, Playback>,
}

impl PlaybackView<'_> {
    /// The playback `id`.
    #[must_use]
    pub fn playback(&self, id: u32) -> Option<&Playback> {
        self.playbacks.get(&playback_key(id))
    }

    /// `HasObject(id, channel)`.
    #[must_use]
    pub fn has_object(&self, id: u32, channel: TypedId) -> bool {
        self.playback(id)
            .is_some_and(|p| p.typed.contains(&channel))
    }

    /// `GetDurationIfNamed(id)`: the animation's `Root height`.
    #[must_use]
    pub fn root_height(&self, id: u32) -> Option<f32> {
        self.playback(id)?.entry()?.root_height
    }

    /// `GetFirstInstanceField(id)`: the animation's name.
    #[must_use]
    pub fn playback_name(&self, id: u32) -> Option<&[u8]> {
        Some(self.playback(id)?.entry()?.name.as_slice())
    }
}

/// The sinks of the channels a walk ticks (`IChannelSink`, the token of
/// each channel's bind descriptor), called as the channels tick, with the
/// playbacks to query.
pub trait PlaybackSink {
    /// `OnKeys(channel, context, batch)` on the sink `sink`.
    fn on_keys(
        &mut self,
        playbacks: &PlaybackView<'_>,
        channel: TypedId,
        sink: u32,
        context: u32,
        keys: &[EventKey],
        count: u32,
    );
    /// `OnLoop(channel, transform, context)` on the sink `sink`; the pose
    /// left in `transform` is the one the channel rebinds from.
    fn on_loop(
        &mut self,
        playbacks: &PlaybackView<'_>,
        channel: TypedId,
        sink: u32,
        context: u32,
        transform: &mut [f32; 12],
    );
}

/// Records the calls as [`ChannelEvent`]s and changes no pose.
impl PlaybackSink for Vec<ChannelEvent> {
    fn on_keys(
        &mut self,
        _: &PlaybackView<'_>,
        channel: TypedId,
        sink: u32,
        context: u32,
        keys: &[EventKey],
        count: u32,
    ) {
        ChannelSink::on_keys(self, channel, sink, context, keys, count);
    }

    fn on_loop(
        &mut self,
        _: &PlaybackView<'_>,
        channel: TypedId,
        sink: u32,
        context: u32,
        transform: &mut [f32; 12],
    ) {
        ChannelSink::on_loop(self, channel, sink, context, transform);
    }
}

/// A [`PlaybackSink`] as the channels' [`ChannelSink`].
struct Bridge<'a, 'b> {
    view: PlaybackView<'a>,
    sink: &'b mut dyn PlaybackSink,
}

impl ChannelSink for Bridge<'_, '_> {
    fn on_keys(
        &mut self,
        channel: TypedId,
        sink: u32,
        context: u32,
        keys: &[EventKey],
        count: u32,
    ) {
        self.sink
            .on_keys(&self.view, channel, sink, context, keys, count);
    }

    fn on_loop(&mut self, channel: TypedId, sink: u32, context: u32, transform: &mut [f32; 12]) {
        self.sink
            .on_loop(&self.view, channel, sink, context, transform);
    }
}

impl Animation {
    /// A component with no playbacks.
    #[must_use]
    pub fn new() -> Self {
        Self {
            next_playback_id: FIRST_PLAYBACK_ID,
            playbacks: BTreeMap::new(),
            channels: ChannelSet::default(),
            mixers: MixerMap::new(),
            handles: Vec::new(),
        }
    }

    /// The playback `id`.
    #[must_use]
    pub fn playback(&self, id: u32) -> Option<&Playback> {
        self.playbacks.get(&playback_key(id))
    }

    /// The typed channel `id`.
    #[must_use]
    pub fn channel(&self, id: TypedId) -> Option<&TypedChannel> {
        self.channels.get(id)
    }

    /// The typed channel `id`.
    pub fn channel_mut(&mut self, id: TypedId) -> Option<&mut TypedChannel> {
        self.channels.get_mut(id)
    }

    /// The mixer's lists.
    #[must_use]
    pub fn mixers(&self) -> &MixerMap {
        &self.mixers
    }

    /// The playbacks as the sinks of a walk see them.
    #[must_use]
    pub fn view(&self) -> PlaybackView<'_> {
        PlaybackView {
            playbacks: &self.playbacks,
        }
    }

    // --- handles ------------------------------------------------------------------------

    /// `RegisterHandle(key)`: marks an instance as driven elsewhere, so
    /// scripts do not bind to it. -1 for the key -1, else 0.
    pub fn register_handle(&mut self, key: u32) -> u32 {
        if key == 0xFFFF_FFFF {
            return 0xFFFF_FFFF;
        }
        if !self.handles.contains(&key) {
            self.handles.insert(0, key);
        }
        0
    }

    /// `HasHandle(key)`.
    #[must_use]
    pub fn has_handle(&self, key: u32) -> bool {
        self.handles.contains(&key)
    }

    /// `HasAnyHandle(key)`: unregisters the key; -1 when it was not
    /// registered.
    pub fn has_any_handle(&mut self, key: u32) -> u32 {
        match self.handles.iter().position(|&k| k == key) {
            Some(at) => {
                self.handles.remove(at);
                0
            }
            None => 0xFFFF_FFFF,
        }
    }

    // --- scripts ------------------------------------------------------------------------

    /// `BindTrackTarget`: the descriptor of `track` under `root`, whether
    /// it bound and whether its instance is registered elsewhere.
    fn bind_track_target(
        &self,
        engine: &Engine,
        root: Option<InstanceId>,
        track: &Track,
        arg_d: u32,
        arg_e: u32,
    ) -> (Option<BindDesc>, bool) {
        if root.is_none() && arg_d == 0 {
            return (None, false);
        }
        let mut out = BindDesc {
            kind: 2,
            extra: 0,
            context: arg_e,
            instance: None,
        };
        if track.child_crc() != 0 {
            let name = track.child.as_ref().map_or(&[][..], |n| n.name.as_slice());
            let Some(inst) = find_instance_by_name(engine, root, name) else {
                return (None, false);
            };
            if self.has_handle(instance_key(Some(inst))) {
                return (None, true);
            }
            let Some(parent) = engine.instance_parent(Some(inst)) else {
                return (None, false);
            };
            if track.parent_crc() != engine.instance_name(Some(parent)).1 {
                return (None, false);
            }
            out.kind = 0;
            out.instance = Some(inst);
            out.extra = arg_d;
            return (Some(out), false);
        }
        if track.parent_crc() != 0 {
            let name = track.parent.as_ref().map_or(&[][..], |n| n.name.as_slice());
            let inst = find_instance_by_name(engine, root, name).or(root);
            if self.has_handle(instance_key(inst)) {
                return (None, true);
            }
            out.instance = inst;
            out.kind = 1;
            out.extra = arg_d;
            return (Some(out), false);
        }
        out.instance = root;
        out.kind = 2;
        out.extra = arg_d;
        ((arg_d != 0).then_some(out), false)
    }

    /// `PlayScript(script, root, name, d, e)`: binds the animation `name`
    /// of `script` under `root`; the playback id, `None` (-1) when the
    /// script has no such animation.
    ///
    /// A track without a loaded channel, or whose channel's flags name no
    /// class (packed bits the loader could not unpack), makes the
    /// reference dereference null; the port gives it no channel and drops
    /// it silently (divergence register).
    pub fn play_script(
        &mut self,
        engine: &Engine,
        script: &Arc<AnimScript>,
        root: Option<InstanceId>,
        name: &[u8],
        arg_d: u32,
        arg_e: u32,
    ) -> Option<u32> {
        let crc = name_crc32(name);
        let entry = script.get_by_crc(crc)?;
        let mut playback = Playback {
            script: Arc::clone(script),
            entry: crc,
            root,
            typed: Vec::new(),
            longest: None,
        };
        let mut best = -1.0f32;
        let mut warnings = Vec::new();
        for (index, track) in entry.tracks.iter().enumerate() {
            let class = track
                .channel
                .as_ref()
                .and_then(|c| c.channel())
                .and_then(|c| TypedClass::from_type(c.flags()));
            let Some(class) = class else {
                continue;
            };
            let mut typed = TypedChannel::with_source(
                class,
                ChannelSource::Script {
                    script: Arc::clone(script),
                    entry: crc,
                    track: index,
                },
            );
            let (desc, already) = self.bind_track_target(engine, root, track, arg_d, arg_e);
            let bound = desc.is_some_and(|d| typed.bind_map(engine, d, MixerKey::of(root)));
            if bound {
                let duration = track.channel.as_ref().map_or(0.0, |c| c.duration());
                let id = self.channels.insert(typed);
                if best < duration {
                    playback.longest = Some(id);
                    best = duration;
                }
                playback.typed.push(id);
                continue;
            }
            if already {
                continue;
            }
            let text = |n: &Option<crate::anim::MapName>| {
                String::from_utf8_lossy(n.as_ref().map_or(&[][..], |n| n.name.as_slice()))
                    .into_owned()
            };
            let anim = String::from_utf8_lossy(&entry.name).into_owned();
            if track.child_crc() != 0 {
                warnings.push(format!(
                    "{anim} failed to map JOINT (parent: {}, child: {}) channel",
                    text(&track.parent),
                    text(&track.child)
                ));
            } else if track.parent_crc() != 0 {
                warnings.push(format!(
                    "{anim} failed to map OBJECT ({}) channel",
                    text(&track.parent)
                ));
            }
        }
        for warning in warnings {
            dump(DumpFlags::WARNING, &warning);
        }
        let id = self.next_playback_id;
        self.next_playback_id = id.wrapping_add(1);
        self.playbacks.insert(playback_key(id), playback);
        Some(id)
    }

    /// `DestroyScript(id)`: the playback leaves the mixer and goes.
    pub fn destroy_script(&mut self, engine: &Engine, id: u32) {
        let Some(playback) = self.playbacks.remove(&playback_key(id)) else {
            return;
        };
        for typed in playback.typed {
            mixer::unbind(&mut self.mixers, engine, &mut self.channels, typed);
            self.channels.remove(typed);
        }
    }

    /// `PlaybackEvalTyped` and the attach of `EvalPlayback(id, flags,
    /// start, speed, blend, scale, factor, block)`: every channel takes
    /// the header (rate `|speed|`, negative with flag 1; loop flags
    /// `flags & !1`) and joins the mixer, blending in over `blend`. -1
    /// when the id is unknown or any channel cannot start at `start`; 1
    /// when flag 1 is set and the channels' durations differ; else 0.
    #[allow(clippy::too_many_arguments)]
    pub fn eval_playback(
        &mut self,
        engine: &Engine,
        id: u32,
        flags: u32,
        start: f32,
        speed: f32,
        blend: f32,
        scale: f32,
        factor: f32,
        block: Option<[f32; 12]>,
    ) -> u32 {
        let result = self.eval_typed(engine, id, flags, start, speed, blend, scale, factor, block);
        if result == 0xFFFF_FFFF {
            return result;
        }
        let typed = self
            .playback(id)
            .map(|p| p.typed.clone())
            .unwrap_or_default();
        for t in typed {
            mixer::blend_attach(
                &mut self.mixers,
                engine,
                &mut self.channels,
                t,
                blend,
                flags,
            );
        }
        result
    }

    #[allow(clippy::too_many_arguments)]
    fn eval_typed(
        &mut self,
        engine: &Engine,
        id: u32,
        flags: u32,
        start: f32,
        speed: f32,
        blend: f32,
        scale: f32,
        factor: f32,
        block: Option<[f32; 12]>,
    ) -> u32 {
        let Some(playback) = self.playbacks.get(&playback_key(id)) else {
            return 0xFFFF_FFFF;
        };
        let sign = if flags & 1 == 0 { 1.0 } else { -1.0 };
        let header = PlayHeader {
            start,
            flags: flags & 0xFE,
            rate: (f64::from(speed).abs() * sign) as f32,
            blend,
            scale,
            factor,
            block,
        };
        let typed = playback.typed.clone();
        let mut result = 0;
        if flags & 1 != 0 && !typed.is_empty() {
            result = u32::from(self.durations_differ(&typed));
        }
        self.bind_all(engine, &typed, &header, result)
    }

    /// `PlaybackDurationsDiffer`.
    fn durations_differ(&self, typed: &[TypedId]) -> bool {
        let mut first = -1.0f32;
        let mut differ = false;
        for &id in typed {
            let duration = self
                .channels
                .get(id)
                .and_then(TypedChannel::channel)
                .map_or(0.0, Channel::duration);
            if first < 0.0 {
                first = duration;
            } else if first != duration {
                differ = true;
            }
        }
        differ
    }

    /// `PlaybackBindAll`.
    fn bind_all(
        &mut self,
        engine: &Engine,
        typed: &[TypedId],
        header: &PlayHeader,
        mut result: u32,
    ) -> u32 {
        for &id in typed {
            if let Some(t) = self.channels.get_mut(id)
                && !t.try_bind(engine, header)
            {
                result = 0xFFFF_FFFF;
            }
        }
        result
    }

    /// `EvalStored(id)`: rebinds every channel from the longest one's
    /// header, where each stands. -1 for an unknown id or a playback
    /// without channels.
    pub fn eval_stored(&mut self, engine: &Engine, id: u32) -> u32 {
        let Some(playback) = self.playbacks.get(&playback_key(id)) else {
            return 0xFFFF_FFFF;
        };
        let Some(longest) = playback.longest.and_then(|l| self.channels.get(l)) else {
            return 0xFFFF_FFFF;
        };
        let header = PlayHeader {
            start: -2.0,
            flags: longest.loop_flags & 0xFE,
            rate: longest.rate,
            blend: 0.0,
            scale: longest.scale,
            factor: longest.factor,
            block: None,
        };
        let typed = playback.typed.clone();
        self.bind_all(engine, &typed, &header, 0)
    }

    /// `StopPlayback(id)`: every channel stops, is posed where it stands
    /// and leaves the mixer.
    pub fn stop_playback(&mut self, engine: &Engine, id: u32) {
        let Some(playback) = self.playbacks.get(&playback_key(id)) else {
            return;
        };
        let typed = playback.typed.clone();
        for &t in &typed {
            if let Some(c) = self.channels.get_mut(t) {
                c.play_state &= 0xFE;
                c.apply_from_archetype();
            }
        }
        for t in typed {
            mixer::unbind(&mut self.mixers, engine, &mut self.channels, t);
        }
    }

    /// `ApplyChannels(id)`: poses every channel where it stands.
    pub fn apply_channels(&mut self, id: u32) {
        let typed = self
            .playback(id)
            .map(|p| p.typed.clone())
            .unwrap_or_default();
        for t in typed {
            if let Some(c) = self.channels.get_mut(t) {
                c.apply_from_archetype();
            }
        }
    }

    /// `StopChannels(id, time)`: `SetTime(time)` on every channel.
    pub fn stop_channels(&mut self, id: u32, time: f32) {
        let typed = self
            .playback(id)
            .map(|p| p.typed.clone())
            .unwrap_or_default();
        for t in typed {
            if let Some(c) = self.channels.get_mut(t) {
                c.set_time(time);
            }
        }
    }

    /// `GetScriptDuration(id)`: the animation's longest channel's
    /// duration; 0 for an unknown id.
    #[must_use]
    pub fn script_duration(&self, id: u32) -> f32 {
        self.playback(id).map_or(0.0, |p| {
            p.entry()
                .and_then(ScriptEntry::longest_channel)
                .map_or(-1.0, |c| c.duration())
        })
    }

    /// `GetStoredDuration(id)`: the longest bound channel's time; -1 for
    /// an unknown id or a playback without channels.
    #[must_use]
    pub fn stored_duration(&self, id: u32) -> f32 {
        self.playback(id)
            .and_then(|p| p.longest)
            .and_then(|l| self.channels.get(l))
            .map_or(-1.0, |c| c.time)
    }

    /// `GetDurationIfNamed(id)`: the animation's `Root height`.
    #[must_use]
    pub fn root_height(&self, id: u32) -> Option<f32> {
        self.playback(id)?.entry()?.root_height
    }

    /// `GetFirstInstanceField(id)`: the animation's name.
    #[must_use]
    pub fn playback_name(&self, id: u32) -> Option<&[u8]> {
        Some(self.playback(id)?.entry()?.name.as_slice())
    }

    /// `FilterObjects(id)`: whether no channel (but event channels) is a
    /// leaf of the mixer; false for an unknown id.
    #[must_use]
    pub fn filter_objects(&self, id: u32) -> bool {
        let Some(playback) = self.playback(id) else {
            return false;
        };
        playback.typed.iter().all(|&t| {
            self.channels
                .get(t)
                .is_some_and(|c| c.class() == TypedClass::Event)
                || !mixer::has_leaf(&self.mixers, &self.channels, t)
        })
    }

    /// `HasObject(id, channel)`.
    #[must_use]
    pub fn has_object(&self, id: u32, channel: TypedId) -> bool {
        self.playback(id)
            .is_some_and(|p| p.typed.contains(&channel))
    }

    /// `FindObject(id, desc)`: the playback's channel bound like `desc`
    /// (same kind; same instance for a joint or object, same sink token
    /// for a sink).
    #[must_use]
    pub fn find_object(&self, id: u32, desc: &BindDesc) -> Option<TypedId> {
        self.playback(id)?.typed.iter().copied().find(|&t| {
            self.channels.get(t).is_some_and(|c| {
                c.bind.kind == desc.kind
                    && match desc.kind {
                        0 | 1 => c.bind.instance == desc.instance,
                        2 => c.bind.extra == desc.extra,
                        _ => false,
                    }
            })
        })
    }

    /// `EnumObjects(id)`: the playback's channels.
    #[must_use]
    pub fn objects(&self, id: u32) -> Option<&[TypedId]> {
        Some(self.playback(id)?.typed.as_slice())
    }

    /// `TouchMapped(id)`: the first channel of class 2 or 6's
    /// `TouchMapped`: for a position-quaternion channel, its transformed
    /// position and its raw sample. `Some(None)` when that channel is a
    /// vector channel (which does nothing), `None` when there is none.
    #[must_use]
    pub fn touch_mapped(&self, id: u32) -> Option<Option<(Vector, Vector)>> {
        let playback = self.playback(id)?;
        playback.typed.iter().find_map(|&t| {
            let c = self.channels.get(t)?;
            (c.class().code() & 2 != 0).then(|| c.touch_mapped())
        })
    }

    /// `RootData::Eval`: `EvalPlayback`'s header without the mixer, the
    /// touch, then every channel stops and is posed. `None` (0) when the
    /// header fails; else whether a channel of class 2 or 6 was touched
    /// and the raw sample a position-quaternion one wrote.
    #[allow(clippy::too_many_arguments)]
    pub fn root_data_eval(
        &mut self,
        engine: &Engine,
        id: u32,
        flags: u32,
        time: f32,
        speed: f32,
        blend: f32,
        scale: f32,
        factor: f32,
    ) -> Option<(bool, Option<Vector>)> {
        if self.eval_typed(engine, id, flags, time, speed, blend, scale, factor, None)
            == 0xFFFF_FFFF
        {
            return None;
        }
        let touch = self.touch_mapped(id);
        let touched = (touch.is_some(), touch.flatten().map(|(_, sampled)| sampled));
        let typed = self
            .playback(id)
            .map(|p| p.typed.clone())
            .unwrap_or_default();
        for t in typed {
            if let Some(c) = self.channels.get_mut(t) {
                c.play_state &= 0xFE;
                c.apply_from_archetype();
            }
        }
        Some(touched)
    }

    /// The playback's root entry in the mixer (the overlay writers'
    /// `Inner`).
    fn overlay_targets(&self, id: u32) -> Option<(i32, Vec<TypedId>)> {
        let playback = self.playback(id)?;
        let key = MixerKey::of(playback.root).0;
        self.mixers
            .contains_key(&key)
            .then(|| (key, playback.typed.clone()))
    }

    fn for_overlays(&mut self, key: i32, typed: TypedId, mut f: impl FnMut(&mut [f32; 7])) {
        let Some(c) = self.channels.get(typed) else {
            return;
        };
        if c.bind.kind != 1 {
            return;
        }
        let bind = BindKey::of(&c.bind);
        let Some(list) = self.mixers.get_mut(&key).and_then(|i| i.get_mut(&bind)) else {
            return;
        };
        list.for_each_overlay(&mut f);
    }

    /// `SetOverlayPos(id, pos)`: moves the object channels (and the
    /// overlays of their lists) to `pos`.
    pub fn set_overlay_pos(&mut self, id: u32, pos: Vector) {
        let Some((key, typed)) = self.overlay_targets(id) else {
            return;
        };
        for t in typed {
            if let Some(c) = self.channels.get_mut(t) {
                c.set_overlay_pos(pos);
            }
            self.for_overlays(key, t, |v| v[..3].copy_from_slice(&pos.to_words()));
        }
    }

    /// `SetOverlayExtra(id, orient)`: turns the object channels (and the
    /// overlays) to `orient`.
    pub fn set_overlay_orientation(&mut self, engine: &Engine, id: u32, orient: &Matrix) {
        let Some((key, typed)) = self.overlay_targets(id) else {
            return;
        };
        let q = matrix_to_quaternion(orient).to_words();
        for t in typed {
            if let Some(c) = self.channels.get_mut(t) {
                c.set_overlay_orientation(engine, orient);
            }
            self.for_overlays(key, t, |v| v[3..].copy_from_slice(&q));
        }
    }

    /// `AddOverlayPos(id, delta)`: shifts the object channels (and the
    /// overlays) by `delta`.
    pub fn add_overlay_pos(&mut self, id: u32, delta: Vector) {
        let Some((key, typed)) = self.overlay_targets(id) else {
            return;
        };
        let d = delta.to_words();
        for t in typed {
            if let Some(c) = self.channels.get_mut(t) {
                c.add_overlay_pos(delta);
            }
            self.for_overlays(key, t, |v| {
                for i in 0..3 {
                    v[i] = (f64::from(d[i]) + f64::from(v[i])) as f32;
                }
            });
        }
    }

    // --- mixer --------------------------------------------------------------------------

    /// `MixerWalk(key, dt)`: walks every list under one mixer key (a
    /// playback's root). Returns what the channels reported.
    pub fn mixer_walk(&mut self, engine: &mut Engine, key: MixerKey, dt: f32) -> Vec<ChannelEvent> {
        let mut events = Vec::new();
        self.mixer_walk_into(engine, key, dt, &mut events);
        events
    }

    /// `MixerWalk(key, dt)` with the channels' sink calls made on `sink`
    /// as the channels tick (an `OnLoop` that changes the pose changes
    /// where the channel rebinds).
    pub fn mixer_walk_into(
        &mut self,
        engine: &mut Engine,
        key: MixerKey,
        dt: f32,
        sink: &mut dyn PlaybackSink,
    ) {
        let mut bridge = Bridge {
            view: PlaybackView {
                playbacks: &self.playbacks,
            },
            sink,
        };
        mixer::walk_key(
            &mut self.mixers,
            engine,
            &mut self.channels,
            key.0,
            dt,
            &mut bridge,
        );
    }

    /// `MixerWalkAll(dt)`: every list, then every playing channel no list
    /// ticked; a channel that runs out stops and leaves its list.
    pub fn mixer_walk_all(&mut self, engine: &mut Engine, dt: f32) -> Vec<ChannelEvent> {
        let mut events = Vec::new();
        self.mixer_walk_all_into(engine, dt, &mut events);
        events
    }

    /// `MixerWalkAll(dt)` with the sink calls made on `sink` as the
    /// channels tick.
    pub fn mixer_walk_all_into(
        &mut self,
        engine: &mut Engine,
        dt: f32,
        sink: &mut dyn PlaybackSink,
    ) {
        let mut events = Bridge {
            view: PlaybackView {
                playbacks: &self.playbacks,
            },
            sink,
        };
        let ids = self.channels.ids();
        for &id in &ids {
            if let Some(c) = self.channels.get_mut(id) {
                c.play_flags &= !PLAY_TICKED;
            }
        }
        let keys: Vec<i32> = self.mixers.keys().copied().collect();
        for key in keys {
            let Some(inner) = self.mixers.get_mut(&key) else {
                continue;
            };
            let binds: Vec<BindKey> = inner.keys().copied().collect();
            for bind in binds {
                let Some(list) = inner.get_mut(&bind) else {
                    continue;
                };
                mixer::walk_list(engine, list, &mut self.channels, dt, &mut events);
                if list.count == 0 {
                    inner.remove(&bind);
                }
            }
            if inner.is_empty() {
                self.mixers.remove(&key);
            }
        }
        for id in ids {
            let Some(c) = self.channels.get_mut(id) else {
                continue;
            };
            if c.play_state & 1 != 0
                && c.play_flags & PLAY_TICKED == 0
                && c.tick(engine, id, dt, &mut events) == 1
            {
                c.play_state &= 0xFE;
                mixer::unbind(&mut self.mixers, engine, &mut self.channels, id);
            }
        }
    }

    // --- IChannel -----------------------------------------------------------------------

    /// `IChannel::CreateTyped(owner, type, bind, key)`: a channel an owner
    /// drives. `None` (-1) for a type that names no class: the reference
    /// then treats the owner as a channel (divergence register).
    pub fn create_typed(
        &mut self,
        engine: &Engine,
        owner: Box<dyn ChannelOwner>,
        code: u32,
        bind: BindDesc,
        key: MixerKey,
    ) -> Option<TypedId> {
        let Some(class) = TypedClass::from_type(code) else {
            dump(DumpFlags::FATAL, "Unknown data type!\n");
            return None;
        };
        let mut typed = TypedChannel::with_owner(class, owner);
        typed
            .bind_map(engine, bind, key)
            .then(|| self.channels.insert(typed))
    }

    /// `IChannel::CreateFromObject(archetype, bind, key)`: a channel
    /// playing `channel`; `None` (-1) when its flags name no class or it
    /// does not fit the target.
    pub fn create_from_object(
        &mut self,
        engine: &Engine,
        channel: Arc<Channel>,
        bind: BindDesc,
        key: MixerKey,
    ) -> Option<TypedId> {
        let Some(class) = TypedClass::from_type(channel.flags()) else {
            dump(DumpFlags::FATAL, "Unknown data type!\n");
            return None;
        };
        let mut typed = TypedChannel::with_source(class, ChannelSource::Object(channel));
        typed
            .bind_map(engine, bind, key)
            .then(|| self.channels.insert(typed))
    }

    /// `IChannel::Destroy(key)`.
    pub fn destroy_channel(&mut self, engine: &Engine, id: TypedId) -> bool {
        if self.channels.get(id).is_none() {
            return false;
        }
        mixer::unbind(&mut self.mixers, engine, &mut self.channels, id);
        self.channels.remove(id);
        true
    }

    /// `IChannel::Play(key, header)`: binds the channel with the header
    /// and, when it can start, attaches it (blending over `header.blend`).
    /// Whether it plays.
    pub fn play_channel(&mut self, engine: &Engine, id: TypedId, header: &PlayHeader) -> bool {
        let Some(c) = self.channels.get_mut(id) else {
            return false;
        };
        let bound = c.try_bind(engine, header);
        c.play_state = (c.play_state & 0xFE) | u8::from(bound);
        if bound {
            mixer::blend_attach(
                &mut self.mixers,
                engine,
                &mut self.channels,
                id,
                header.blend,
                header.flags,
            );
        }
        bound
    }

    /// `IChannel::StopGraph(key)`: a playing channel is posed and stops.
    pub fn stop_graph(&mut self, id: TypedId) -> bool {
        let Some(c) = self.channels.get_mut(id) else {
            return false;
        };
        if c.play_state & 1 == 0 {
            return false;
        }
        c.apply_from_archetype();
        c.play_state &= 0xFE;
        true
    }

    /// `IChannel::InMixer(key)`.
    #[must_use]
    pub fn in_mixer(&self, id: TypedId) -> bool {
        mixer::has_leaf(&self.mixers, &self.channels, id)
    }
}

impl mixer::MixerList {
    /// Applies `f` to the values of every overlay node.
    fn for_each_overlay(&mut self, f: &mut impl FnMut(&mut [f32; 7])) {
        for node in self.slots_mut() {
            if let NodeKind::Overlay { values, .. } = &mut node.kind {
                f(values);
            }
        }
    }
}
