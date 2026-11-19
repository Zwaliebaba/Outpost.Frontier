//! The scene engine of P1.8 behind the render stack and the deformables
//! (work package P4.16): what the reference's `IEngine` answers to the
//! render manager, the mesh kinds, the surface materials and `Deformable2`,
//! and the engine component that hands the scene engine's archetypes and
//! instances to the render manager and the `VMeshWire` component, as the
//! reference's engine calls its `[Engine]` components.
//!
//! The reference's components are faces of one aggregate and call each
//! other through interface pointers while the engine runs. Here the scene
//! engine, the render stack and the render manager are separate values,
//! so a call that arrives while the other side is busy cannot reach it:
//!
//! - The render manager asks the engine an instance's archetype while it
//!   destroys the instance, which the scene engine does from inside its
//!   own `DestroyInstance`. [`Links`] keeps each instance's archetype
//!   handle as the scene engine announced it, and the stack's engine
//!   answers from there.
//! - The mesh kinds write an instance's extent while the scene engine
//!   creates it; the write is kept and applied when the scene engine's
//!   call returns ([`Links::take_extents`]).
//! - A deformable model creates its skeleton's archetypes and instances
//!   while it holds the render stack; the render manager cannot be offered
//!   those (the bones have no render node, so every plugin would decline),
//!   and each skipped offer is counted ([`Links::skipped`]).
//!
//! Divergence D453 records these.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::{Rc, Weak};
use std::sync::Arc;

use neuron_core::anim::AnimScript;
use neuron_core::math::{Matrix, Vector};
use neuron_core::scene::sample::{self, EventKey};
use neuron_core::scene::typed::payload_bytes;
use neuron_core::scene::{
    Animation, ArchetypeId, BindDesc, ChannelOwner, Engine as SceneEngine, EngineComponent,
    Hardpoints, InstanceId, MixerKey, PlayHeader, PlaybackSink, PlaybackView, SceneError,
    TypedChannel, TypedId, connect_hardpoints,
};
use neuron_core::vfs::FileSystem;

use crate::render::deformable::services::{AimHost, SinkHost};
use crate::render::deformable::{
    BoneLink, ChannelSample, DeformHost, DeformInstance, EngineHandle,
    Instance as DeformInstanceHandle, OwnerBind,
};
use crate::render::ff::device::Backend;
use crate::render::mesh::{Archetype, Instance, RenderManager};
use crate::render::stack::{RenderStack, StackEngine};

/// The render stack and the render manager, which the render half of the
/// engine shares.
pub struct RenderSide<B: Backend + 'static> {
    /// The joined services: the device, the texture library, the material
    /// library and batcher, the `VMesh` registry, the `VMeshWire`
    /// component.
    pub stack: RenderStack<B>,
    /// The render manager with the `[RenderManager]` plugins.
    pub manager: RenderManager<RenderStack<B>>,
}

/// The scene engine as the engine holds it.
pub type SharedScene = Rc<RefCell<SceneEngine>>;

/// What the scene engine announced, shared by the stack's engine, the
/// deformables' host and the component that links the render manager.
#[derive(Debug, Default)]
pub struct Links {
    /// The scene archetypes by handle (the handle is the arena index).
    archetypes: BTreeMap<Archetype, ArchetypeId>,
    /// Each live instance's archetype handle.
    instance_archetypes: BTreeMap<u32, Archetype>,
    /// The instances the scene engine made since the last
    /// [`Links::take_made`], in order.
    made: Vec<(InstanceId, ArchetypeId)>,
    /// Extent writes made while the scene engine was busy.
    extents: Vec<(InstanceId, f32, [f32; 3])>,
    /// Offers to the render manager skipped because the render side was
    /// busy.
    pub skipped: u32,
}

impl Links {
    /// A shared, empty table.
    #[must_use]
    pub fn shared() -> Rc<RefCell<Links>> {
        Rc::new(RefCell::new(Links::default()))
    }

    /// The handle of a scene archetype (its arena index), registered.
    pub fn handle(&mut self, id: ArchetypeId) -> Archetype {
        let handle = i32::try_from(id.index()).unwrap_or(-1);
        if handle >= 0 {
            self.archetypes.insert(handle, id);
        }
        handle
    }

    /// The scene archetype of a handle.
    #[must_use]
    pub fn archetype(&self, handle: Archetype) -> Option<ArchetypeId> {
        self.archetypes.get(&handle).copied()
    }

    /// The archetype handle the scene engine gave `instance`, -1 for none.
    #[must_use]
    pub fn instance_archetype(&self, instance: u32) -> Archetype {
        self.instance_archetypes
            .get(&instance)
            .copied()
            .unwrap_or(-1)
    }

    /// The instances made since the last call, in order.
    pub fn take_made(&mut self) -> Vec<(InstanceId, ArchetypeId)> {
        std::mem::take(&mut self.made)
    }

    /// The extent writes kept while the scene engine was busy.
    pub fn take_extents(&mut self) -> Vec<(InstanceId, f32, [f32; 3])> {
        std::mem::take(&mut self.extents)
    }
}

/// The links as the engine shares them.
pub type SharedLinks = Rc<RefCell<Links>>;

fn instance_id(instance: Instance) -> Option<InstanceId> {
    u32::try_from(instance).ok().map(InstanceId::from_index)
}

fn instance_index(instance: Option<InstanceId>) -> Option<Instance> {
    instance.map(|i| i.index() as usize)
}

const IDENTITY_12: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];
const IDENTITY_9: [f32; 9] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0];

/// The scene engine as the render stack asks it ([`StackEngine`]). It holds
/// the engine weakly: the engine's components hold the render side, which
/// holds the stack.
pub struct SceneHost {
    scene: Weak<RefCell<SceneEngine>>,
    links: SharedLinks,
}

impl SceneHost {
    /// The stack's engine over `scene`.
    #[must_use]
    pub fn new(scene: Weak<RefCell<SceneEngine>>, links: SharedLinks) -> SceneHost {
        SceneHost { scene, links }
    }

    /// An engine that answers nothing yet (before the scene engine is
    /// made).
    #[must_use]
    pub fn detached(links: SharedLinks) -> SceneHost {
        SceneHost::new(Weak::new(), links)
    }

    fn read<R>(&self, f: impl FnOnce(&SceneEngine) -> R) -> Option<R> {
        let scene = self.scene.upgrade()?;
        let borrowed = scene.try_borrow().ok()?;
        Some(f(&borrowed))
    }

    fn write<R>(&self, f: impl FnOnce(&mut SceneEngine) -> R) -> Option<R> {
        let scene = self.scene.upgrade()?;
        let mut borrowed = scene.try_borrow_mut().ok()?;
        Some(f(&mut borrowed))
    }
}

impl StackEngine for SceneHost {
    fn instance_transform(&mut self, instance: Instance) -> [f32; 12] {
        self.read(|s| s.instance_transform(instance_id(instance)).to_words())
            .unwrap_or(IDENTITY_12)
    }

    fn instance_archetype(&mut self, instance: Instance) -> Archetype {
        u32::try_from(instance).map_or(-1, |i| self.links.borrow().instance_archetype(i))
    }

    /// The instance's own extent. While the scene engine is busy (it is
    /// creating the instance, whose mesh kind asks before it writes one),
    /// the extent written meanwhile, else none yet (radius 0), as the
    /// reference's engine answers for a new instance.
    fn instance_extent(&mut self, instance: Instance) -> Option<(f32, [f32; 3])> {
        let id = instance_id(instance)?;
        let scene = self.scene.upgrade()?;
        let Ok(mut s) = scene.try_borrow_mut() else {
            let links = self.links.borrow();
            let pending = links.extents.iter().rev().find(|(i, _, _)| *i == id);
            return Some(pending.map_or((0.0, [0.0; 3]), |&(_, r, c)| (r, c)));
        };
        s.instance(id)?;
        let (radius, centre) = s.instance_extent(Some(id), 1);
        Some((radius, centre.to_words()))
    }

    fn set_instance_extent(&mut self, instance: Instance, radius: f32, centre: [f32; 3]) {
        let Some(id) = instance_id(instance) else {
            return;
        };
        let v = Vector::new(centre[0], centre[1], centre[2]);
        if self
            .write(|s| s.set_instance_extent(Some(id), 1, radius, v))
            .is_none()
        {
            self.links.borrow_mut().extents.push((id, radius, centre));
        }
    }

    fn related_instance(
        &mut self,
        instance: Instance,
        previous: Option<Instance>,
    ) -> Option<Instance> {
        let previous = previous.and_then(instance_id);
        self.read(|s| instance_index(s.related_instance(instance_id(instance), 1, previous)))
            .flatten()
    }

    fn instance_root(&mut self, instance: Instance) -> Instance {
        self.read(|s| instance_index(s.instance_root(instance_id(instance))))
            .flatten()
            .unwrap_or(instance)
    }

    fn instance_parent(&mut self, instance: Instance) -> i32 {
        self.read(|s| s.instance_parent(instance_id(instance)))
            .flatten()
            .map_or(-1, |p| i32::try_from(p.index()).unwrap_or(-1))
    }

    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9] {
        self.read(|s| s.instance_orientation(instance_id(instance)).to_words())
            .unwrap_or(IDENTITY_9)
    }
}

/// The engine component that offers the scene engine's archetypes and
/// instances to the render manager and the `VMeshWire` component (their
/// `IEngineComponent` slots), and records them in the [`Links`].
pub struct RenderLink<B: Backend + 'static> {
    side: Rc<RefCell<RenderSide<B>>>,
    links: SharedLinks,
}

impl<B: Backend + 'static> RenderLink<B> {
    /// The component over the render side.
    #[must_use]
    pub fn new(side: Rc<RefCell<RenderSide<B>>>, links: SharedLinks) -> RenderLink<B> {
        RenderLink { side, links }
    }

    fn with_side(&self, f: impl FnOnce(&mut RenderSide<B>)) {
        if let Ok(mut side) = self.side.try_borrow_mut() {
            f(&mut side);
        } else {
            self.links.borrow_mut().skipped += 1;
        }
    }
}

impl<B: Backend + 'static> std::fmt::Debug for RenderLink<B> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RenderLink").finish_non_exhaustive()
    }
}

impl<B: Backend + 'static> EngineComponent for RenderLink<B> {
    fn create_archetype(
        &mut self,
        archetype: ArchetypeId,
        fs: &mut dyn FileSystem,
    ) -> Result<(), SceneError> {
        let handle = self.links.borrow_mut().handle(archetype);
        self.with_side(|side| {
            let RenderSide { stack, manager } = side;
            manager.create_archetype(stack, handle, fs);
            let mut wires = std::mem::take(&mut stack.wires);
            wires.create_archetype(stack, handle, fs);
            stack.wires = wires;
        });
        Ok(())
    }

    fn duplicate_archetype(&mut self, new: ArchetypeId, old: ArchetypeId) {
        let (new, old) = {
            let mut links = self.links.borrow_mut();
            (links.handle(new), links.handle(old))
        };
        self.with_side(|side| {
            let RenderSide { stack, manager } = side;
            manager.duplicate_archetype(stack, new, old);
        });
    }

    fn destroy_archetype(&mut self, archetype: ArchetypeId) {
        let handle = self.links.borrow_mut().handle(archetype);
        self.with_side(|side| {
            let RenderSide { stack, manager } = side;
            manager.destroy_archetype(stack, handle);
            stack.wires.destroy_archetype(handle);
        });
        self.links.borrow_mut().archetypes.remove(&handle);
    }

    fn create_instance(&mut self, instance: InstanceId, archetype: ArchetypeId) {
        let handle = {
            let mut links = self.links.borrow_mut();
            let handle = links.handle(archetype);
            links.made.push((instance, archetype));
            links.instance_archetypes.insert(instance.index(), handle);
            handle
        };
        self.with_side(|side| {
            let RenderSide { stack, manager } = side;
            manager.create_instance(stack, instance.index() as usize, handle);
        });
    }

    fn destroy_instance(&mut self, instance: InstanceId) {
        self.with_side(|side| {
            let RenderSide { stack, manager } = side;
            manager.destroy_instance(stack, instance.index() as usize);
        });
        self.links
            .borrow_mut()
            .instance_archetypes
            .remove(&instance.index());
    }

    fn as_any(&self) -> &dyn std::any::Any {
        self
    }

    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
}

/// The animation system as the engine shares it: the engine's frame walks
/// its mixer, the deformables' host answers `Deformable2`'s `IAnimation2`
/// and `IChannel` calls on it.
pub type SharedAnimation = Rc<RefCell<Animation>>;

/// The engine's deformable models by id, shared with the owners of the
/// aim events' typed channels, which the animation system calls while it
/// walks the mixer.
pub type SharedDeformables = Rc<RefCell<Vec<Option<DeformInstance>>>>;

/// The key `Deformable2` holds for a typed channel (the reference's
/// channel pointer): the channel's id plus one, so that 0 stays "none"
/// (`FindObject`'s miss) and -1 a failure.
#[must_use]
pub fn channel_key(id: TypedId) -> u32 {
    id.0.wrapping_add(1)
}

/// The channel of a key (`None` for 0 and -1).
#[must_use]
pub fn typed_id(key: u32) -> Option<TypedId> {
    (key != 0 && key != u32::MAX).then(|| TypedId(key - 1))
}

/// The sink token of the deformable model `index` (the instance as the
/// `IChannelSink` its scripts are played with): the index plus one.
#[must_use]
pub fn sink_token(index: usize) -> u32 {
    u32::try_from(index).map_or(u32::MAX, |i| i.wrapping_add(1))
}

/// The names `Deformable2` passes as `const char*` (the animation names of
/// `PlayInstanceAnim`, the cookie `OnKeys` hands on), interned: a handle
/// is the index plus one.
#[derive(Debug, Default)]
pub struct Names(Vec<Vec<u8>>);

impl Names {
    /// The handle of `name`, interned.
    pub fn intern(&mut self, name: &[u8]) -> u32 {
        let at = match self.0.iter().position(|n| n == name) {
            Some(at) => at,
            None => {
                self.0.push(name.to_vec());
                self.0.len() - 1
            }
        };
        u32::try_from(at).map_or(0, |i| i.wrapping_add(1))
    }

    /// The name of a handle.
    #[must_use]
    pub fn get(&self, handle: u32) -> Option<&[u8]> {
        let at = usize::try_from(handle.checked_sub(1)?).ok()?;
        self.0.get(at).map(Vec::as_slice)
    }
}

fn inst(instance: DeformInstanceHandle) -> Option<InstanceId> {
    u32::try_from(instance).ok().map(InstanceId::from_index)
}

fn handle_of(instance: Option<InstanceId>) -> DeformInstanceHandle {
    instance.map_or(-1, |i| i32::try_from(i.index()).unwrap_or(-1))
}

fn playback_id(handle: i32) -> u32 {
    handle.cast_unsigned()
}

/// The engine's reads an aim event makes while the animation system ticks
/// its channels ([`AimHost`]), on the engine the channels are ticked on;
/// the deformables' host answers the same calls the same way.
struct EngineReads<'a>(&'a SceneEngine);

impl AimHost for EngineReads<'_> {
    fn instance_parent(&mut self, instance: DeformInstanceHandle) -> DeformInstanceHandle {
        handle_of(self.0.instance_parent(inst(instance)))
    }

    fn instance_relative(&mut self, instance: DeformInstanceHandle) -> [u32; 23] {
        self.0
            .instance_relative(inst(instance))
            .map_or([0; 23], |p| p.0)
    }

    fn instance_position(&mut self, instance: DeformInstanceHandle) -> [f32; 3] {
        self.0.instance_position(inst(instance)).to_words()
    }

    fn instance_orientation(&mut self, instance: DeformInstanceHandle) -> [f32; 9] {
        self.0.instance_orientation(inst(instance)).to_words()
    }

    fn joint_params(&mut self, instance: DeformInstanceHandle, index: u32) -> Vec<f32> {
        self.0
            .joint_params(inst(instance), index)
            .unwrap_or_default()
    }

    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str) {
        neuron_core::diag::dump(
            neuron_core::diag::DumpFlags::from_bits(flags.cast_unsigned()),
            &format!("{file}({line}) : {message}"),
        );
    }
}

/// The animation system's playback queries the instance's channel sink
/// makes ([`SinkHost`]); the cookie `GetFirstInstanceField` writes (the
/// animation's name in the reference) is the name's interned handle.
struct PlaybackReads<'a, 'b> {
    view: &'a PlaybackView<'a>,
    names: &'b mut Names,
}

impl SinkHost for PlaybackReads<'_, '_> {
    fn has_object(&mut self, handle: i32, key: u32) -> u32 {
        u32::from(typed_id(key).is_some_and(|t| self.view.has_object(playback_id(handle), t)))
    }

    fn first_instance_field(&mut self, handle: i32) -> Option<u32> {
        let name = self.view.playback_name(playback_id(handle))?;
        Some(self.names.intern(name))
    }

    fn duration_if_named(&mut self, handle: i32) -> Option<f32> {
        self.view.root_height(playback_id(handle))
    }
}

/// The owner of an aim event's typed channels (`CreateTyped(event, ...)`):
/// at each tick it forwards `FIkEvent::Apply` to the aim event of its
/// deformable model's list that made the channel, which solves on the
/// engine the channel is ticked on and writes the joint's quaternion into
/// the channel's sample.
#[derive(Debug)]
struct AimOwner {
    models: Weak<RefCell<Vec<Option<DeformInstance>>>>,
    index: usize,
}

impl ChannelOwner for AimOwner {
    fn apply(
        &mut self,
        engine: &SceneEngine,
        sampled: &mut [f32],
        channel: TypedId,
        bind: &BindDesc,
        _time: f32,
    ) -> u32 {
        let Some(models) = self.models.upgrade() else {
            return 0;
        };
        let Ok(mut models) = models.try_borrow_mut() else {
            return 0;
        };
        let Some(Some(instance)) = models.get_mut(self.index) else {
            return 0;
        };
        let gate = OwnerBind {
            skip: bind.kind.cast_signed(),
            joint: handle_of(bind.instance),
        };
        instance
            .apply_channel(
                sampled,
                channel_key(channel),
                &gate,
                &mut EngineReads(engine),
            )
            .map_or(0, i32::cast_unsigned)
    }
}

/// What a deformable model's caller sink (`IDeformable::Create`'s sink,
/// `UserAt18`) receives from the instance's `OnKeys` and `OnLoop`, queued
/// by the engine ([`super::Engine::take_deformable_events`]).
#[derive(Clone, Debug, PartialEq)]
pub enum DeformableEvent {
    /// `OnKeys(cookie, context, batch)`: the event keys a tick crossed; the
    /// cookie is the name of the animation that holds the channel (`None`
    /// when no animation of the list holds it).
    Keys {
        /// The model.
        model: usize,
        /// The animation's name.
        animation: Option<Vec<u8>>,
        /// The context (the script the animation was played with).
        context: u32,
        /// The keys (at most 0x40).
        keys: Vec<EventKey>,
        /// Every key found.
        count: u32,
    },
    /// `OnLoop(channel, transform, context)`: an object channel wrapped;
    /// the pose as the instance reset it (its height and heading).
    Loop {
        /// The model.
        model: usize,
        /// The channel's key.
        channel: u32,
        /// The pose.
        transform: [f32; 12],
        /// The context.
        context: u32,
    },
}

/// The instances' channel sinks during a mixer walk: a channel's sink
/// token names the deformable model; its `OnKeys` and `OnLoop` run as the
/// channel ticks (an `OnLoop` resets the pose the channel rebinds from),
/// and what they forward to the caller's sink is queued.
pub struct DeformSink<'a> {
    /// The models.
    pub models: &'a RefCell<Vec<Option<DeformInstance>>>,
    /// The names the cookies are interned in.
    pub names: &'a mut Names,
    /// The caller sinks' queue.
    pub events: &'a mut Vec<DeformableEvent>,
}

impl PlaybackSink for DeformSink<'_> {
    fn on_keys(
        &mut self,
        playbacks: &PlaybackView<'_>,
        channel: TypedId,
        sink: u32,
        context: u32,
        keys: &[EventKey],
        count: u32,
    ) {
        let Some(model) = sink.checked_sub(1).and_then(|i| usize::try_from(i).ok()) else {
            return;
        };
        let Ok(models) = self.models.try_borrow() else {
            return;
        };
        let Some(Some(instance)) = models.get(model) else {
            return;
        };
        let mut reads = PlaybackReads {
            view: playbacks,
            names: self.names,
        };
        let Some(cookie) = instance.on_keys(channel_key(channel), &mut reads) else {
            return;
        };
        self.events.push(DeformableEvent::Keys {
            model,
            animation: self.names.get(cookie).map(<[u8]>::to_vec),
            context,
            keys: keys.to_vec(),
            count,
        });
    }

    fn on_loop(
        &mut self,
        playbacks: &PlaybackView<'_>,
        channel: TypedId,
        sink: u32,
        context: u32,
        transform: &mut [f32; 12],
    ) {
        let Some(model) = sink.checked_sub(1).and_then(|i| usize::try_from(i).ok()) else {
            return;
        };
        let Ok(models) = self.models.try_borrow() else {
            return;
        };
        let Some(Some(instance)) = models.get(model) else {
            return;
        };
        let mut reads = PlaybackReads {
            view: playbacks,
            names: self.names,
        };
        let key = channel_key(channel);
        if instance.on_loop(key, transform, &mut reads) {
            self.events.push(DeformableEvent::Loop {
                model,
                channel: key,
                transform: *transform,
                context,
            });
        }
    }
}

/// The engine half of `Deformable2`'s calls ([`DeformHost`]) on the scene
/// engine and the animation system: archetypes, instances, transforms,
/// joints and hardpoints; scripts played on an instance's skeleton with
/// the instance as their channels' sink (`PlayScript`, `EvalPlayback`, the
/// root data, the overlays, the handle registry); the aim events' typed
/// channels, each owned by an [`AimOwner`] that forwards the channel's
/// tick to its aim event.
///
/// `Deformable2` passes the animation script and the animation's name as
/// pointers; here they are handles: a script added with
/// [`SceneDeformHost::add_script`], a name interned in
/// [`SceneDeformHost::names`]. The calls of one deformable model are
/// made with that model current ([`SceneDeformHost::set_current`]): its
/// index names its sink token and its aim events' owner.
pub struct SceneDeformHost {
    scene: SharedScene,
    links: SharedLinks,
    animation: SharedAnimation,
    models: Weak<RefCell<Vec<Option<DeformInstance>>>>,
    scripts: Vec<Arc<AnimScript>>,
    /// The interned names.
    pub names: Names,
    current: Option<usize>,
    /// `rand()`'s state: the Microsoft C run time's linear congruential
    /// generator, its own and seeded with 1 (D456).
    seed: u32,
}

impl SceneDeformHost {
    /// The host over the engine, the animation system and the models.
    #[must_use]
    pub fn new(
        scene: SharedScene,
        links: SharedLinks,
        animation: SharedAnimation,
        models: &SharedDeformables,
    ) -> SceneDeformHost {
        SceneDeformHost {
            scene,
            links,
            animation,
            models: Rc::downgrade(models),
            scripts: Vec::new(),
            names: Names::default(),
            current: None,
            seed: 1,
        }
    }

    /// Makes the calls that follow those of the deformable model `index`.
    pub fn set_current(&mut self, index: Option<usize>) {
        self.current = index;
    }

    /// An animation script (`OpenScript`'s result): its handle.
    pub fn add_script(&mut self, script: Arc<AnimScript>) -> u32 {
        self.scripts.push(script);
        u32::try_from(self.scripts.len()).unwrap_or(0)
    }

    /// The script of a handle.
    #[must_use]
    pub fn script(&self, handle: u32) -> Option<&Arc<AnimScript>> {
        let at = usize::try_from(handle.checked_sub(1)?).ok()?;
        self.scripts.get(at)
    }

    fn arch(&self, handle: EngineHandle) -> Option<ArchetypeId> {
        self.links.borrow().archetype(handle)
    }

    fn id(instance: Option<InstanceId>) -> DeformInstanceHandle {
        handle_of(instance)
    }

    fn inst(instance: DeformInstanceHandle) -> Option<InstanceId> {
        inst(instance)
    }

    fn reads<R>(&mut self, f: impl FnOnce(&mut PlaybackReads<'_, '_>) -> R) -> R {
        let animation = self.animation.borrow();
        let view = animation.view();
        let mut reads = PlaybackReads {
            view: &view,
            names: &mut self.names,
        };
        f(&mut reads)
    }
}

impl DeformHost for SceneDeformHost {
    fn create_archetype(&mut self, name: &[u8], fs: &mut dyn FileSystem) -> EngineHandle {
        let made = self
            .scene
            .borrow_mut()
            .create_archetype_handle(name, Some(fs))
            .ok()
            .flatten();
        made.map_or(-1, |id| self.links.borrow_mut().handle(id))
    }

    fn create_skeleton_archetype(&mut self, name: &[u8], fs: &mut dyn FileSystem) -> EngineHandle {
        self.create_archetype(name, fs)
    }

    fn release_handle(&mut self, handle: EngineHandle) {
        if let Some(id) = self.arch(handle) {
            self.scene.borrow_mut().release_handle(id);
        }
    }

    fn query_vmesh_part(&mut self, _: EngineHandle) -> Result<u32, i32> {
        Err(-1)
    }

    fn add_ref_vmesh_part(&mut self, _: u32) {}

    fn release_vmesh_part(&mut self, _: u32) {}

    fn draw_vmesh_part(&mut self, _: u32, _: DeformInstanceHandle) {}

    fn create_instance(
        &mut self,
        handle: EngineHandle,
        link: Option<&mut dyn BoneLink>,
    ) -> DeformInstanceHandle {
        let Some(arch) = self.arch(handle) else {
            return -1;
        };
        let before = self.links.borrow_mut().take_made();
        let root = self.scene.borrow_mut().create_instance(arch);
        let made = {
            let mut links = self.links.borrow_mut();
            let made = links.take_made();
            let mut kept = before;
            kept.extend(made.iter().copied());
            links.made = kept;
            made
        };
        if let Some(link) = link {
            for (instance, archetype) in made {
                let h = self.links.borrow_mut().handle(archetype);
                link.link(self, Self::id(Some(instance)), h);
            }
        }
        Self::id(root)
    }

    fn destroy_instance(&mut self, instance: DeformInstanceHandle) {
        if let Some(id) = Self::inst(instance) {
            self.scene.borrow_mut().destroy_instance(id);
        }
    }

    fn set_instance_transform(
        &mut self,
        instance: DeformInstanceHandle,
        _: usize,
    ) -> Option<[f32; 12]> {
        let id = Self::inst(instance)?;
        Some(self.scene.borrow().instance_transform(Some(id)).to_words())
    }

    fn instance_parent(&mut self, instance: DeformInstanceHandle) -> DeformInstanceHandle {
        EngineReads(&self.scene.borrow()).instance_parent(instance)
    }

    fn instance_relative(&mut self, instance: DeformInstanceHandle) -> [u32; 23] {
        EngineReads(&self.scene.borrow()).instance_relative(instance)
    }

    fn instance_position(&mut self, instance: DeformInstanceHandle) -> [f32; 3] {
        EngineReads(&self.scene.borrow()).instance_position(instance)
    }

    fn instance_orientation(&mut self, instance: DeformInstanceHandle) -> [f32; 9] {
        EngineReads(&self.scene.borrow()).instance_orientation(instance)
    }

    fn set_instance_position(&mut self, instance: DeformInstanceHandle, position: &[f32; 3]) {
        let v = Vector::new(position[0], position[1], position[2]);
        self.scene
            .borrow_mut()
            .set_instance_position(Self::inst(instance), v);
    }

    fn set_instance_orientation(&mut self, instance: DeformInstanceHandle, rows: &[f32; 9]) {
        let m = Matrix::from_words(*rows);
        self.scene
            .borrow_mut()
            .set_instance_orientation(Self::inst(instance), &m);
    }

    fn joint_params(&mut self, instance: DeformInstanceHandle, index: u32) -> Vec<f32> {
        EngineReads(&self.scene.borrow()).joint_params(instance, index)
    }

    fn has_hardpoints(&mut self) -> bool {
        self.scene.borrow().component::<Hardpoints>().is_some()
    }

    fn hardpoint_names(&mut self, archetype: EngineHandle) -> Vec<Vec<u8>> {
        let Some(arch) = self.arch(archetype) else {
            return Vec::new();
        };
        self.scene
            .borrow()
            .component::<Hardpoints>()
            .map(|h| h.names(arch).map(<[u8]>::to_vec).collect())
            .unwrap_or_default()
    }

    fn connect_hardpoints(
        &mut self,
        a: DeformInstanceHandle,
        b: DeformInstanceHandle,
        name: &[u8],
    ) -> u32 {
        let crc = neuron_core::hash::name_crc32(name);
        connect_hardpoints(
            &mut self.scene.borrow_mut(),
            Self::inst(a),
            crc,
            Self::inst(b),
            crc,
        )
    }

    /// `PlayScript(script, root, name, instance, context)`: `name` is the
    /// script's handle, `extra` the animation name's; the current model's
    /// sink token is the channels' sink. -1 for an unknown script (an
    /// invalid `FAnimScript`) or an animation the script lacks.
    fn play_script(
        &mut self,
        name: u32,
        root: DeformInstanceHandle,
        extra: u32,
        script: u32,
    ) -> i32 {
        let Some(anim_script) = self.script(name).cloned() else {
            return -1;
        };
        let Some(anim) = self.names.get(extra).map(<[u8]>::to_vec) else {
            return -1;
        };
        let sink = self.current.map_or(0, sink_token);
        let scene = self.scene.borrow();
        self.animation
            .borrow_mut()
            .play_script(&scene, &anim_script, inst(root), &anim, sink, script)
            .map_or(-1, u32::cast_signed)
    }

    fn destroy_script(&mut self, handle: i32) {
        let scene = self.scene.borrow();
        self.animation
            .borrow_mut()
            .destroy_script(&scene, playback_id(handle));
    }

    fn filter_objects(&mut self, handle: i32) -> u32 {
        u32::from(self.animation.borrow().filter_objects(playback_id(handle)))
    }

    /// `IRootData::Eval(handle, vec, flags, time, scale, d, f, 1.0)`: the
    /// vector receives a position-quaternion channel's raw sample.
    fn root_eval(
        &mut self,
        handle: i32,
        vec: [f32; 3],
        flags: u32,
        time: f32,
        scale: f32,
        d: u32,
        f: u32,
    ) -> Option<(u8, [f32; 3])> {
        let scene = self.scene.borrow();
        let eval = self.animation.borrow_mut().root_data_eval(
            &scene,
            playback_id(handle),
            flags,
            time,
            scale,
            f32::from_bits(d),
            f32::from_bits(f),
            1.0,
        );
        Some(match eval {
            None => (0, vec),
            Some((touched, raw)) => (u8::from(touched), raw.map_or(vec, |v| v.to_words())),
        })
    }

    /// `GetNamedDurationOut(script, name)`: the named animation's `Root
    /// height`.
    fn named_duration(&mut self, name: u32, extra: u32) -> Option<f32> {
        let script = self.script(name)?;
        let crc = neuron_core::hash::name_crc32(self.names.get(extra)?);
        script.get_by_crc(crc)?.root_height
    }

    fn eval_playback(
        &mut self,
        handle: i32,
        flags: u32,
        time: f32,
        scale: f32,
        d: u32,
        f: u32,
        xform: &[f32; 12],
    ) -> i32 {
        let scene = self.scene.borrow();
        self.animation
            .borrow_mut()
            .eval_playback(
                &scene,
                playback_id(handle),
                flags,
                time,
                scale,
                f32::from_bits(d),
                f32::from_bits(f),
                1.0,
                Some(*xform),
            )
            .cast_signed()
    }

    fn has_object(&mut self, handle: i32, key: u32) -> u32 {
        self.reads(|r| r.has_object(handle, key))
    }

    fn first_instance_field(&mut self, handle: i32) -> Option<u32> {
        self.reads(|r| r.first_instance_field(handle))
    }

    fn duration_if_named(&mut self, handle: i32) -> Option<f32> {
        self.reads(|r| r.duration_if_named(handle))
    }

    fn touch_mapped(&mut self, handle: i32) -> ([f32; 3], [f32; 3]) {
        self.animation
            .borrow()
            .touch_mapped(playback_id(handle))
            .flatten()
            .map_or(([0.0; 3], [0.0; 3]), |(a, b)| (a.to_words(), b.to_words()))
    }

    fn set_overlay_pos(&mut self, handle: i32, pos: &[f32; 3]) {
        self.animation
            .borrow_mut()
            .set_overlay_pos(playback_id(handle), Vector::from_words(*pos));
    }

    fn set_overlay_extra(&mut self, handle: i32, extra: &[f32; 9]) {
        let scene = self.scene.borrow();
        self.animation.borrow_mut().set_overlay_orientation(
            &scene,
            playback_id(handle),
            &Matrix::from_words(*extra),
        );
    }

    fn add_overlay_pos(&mut self, handle: i32, delta: &[f32; 3]) {
        self.animation
            .borrow_mut()
            .add_overlay_pos(playback_id(handle), Vector::from_words(*delta));
    }

    /// `FindObject(handle, {kind, 0, 0, instance})`: the channel's key, 0
    /// for none, -1 for an unknown playback.
    fn find_object(&mut self, handle: i32, kind: u32, instance: DeformInstanceHandle) -> u32 {
        let animation = self.animation.borrow();
        let id = playback_id(handle);
        if animation.playback(id).is_none() {
            return u32::MAX;
        }
        let desc = BindDesc {
            kind,
            extra: 0,
            context: 0,
            instance: inst(instance),
        };
        animation.find_object(id, &desc).map_or(0, channel_key)
    }

    fn register_handle(&mut self, key: i32) {
        self.animation
            .borrow_mut()
            .register_handle(key.cast_unsigned());
    }

    fn has_handle(&mut self, key: i32) -> u8 {
        u8::from(self.animation.borrow().has_handle(key.cast_unsigned()))
    }

    fn has_any_handle(&mut self, key: i32) {
        self.animation
            .borrow_mut()
            .has_any_handle(key.cast_unsigned());
    }

    /// `CreateTyped(event, 4, {0, 0, 0, joint}, bind)`: a quaternion
    /// channel on the joint under the mixer key `bind`, owned by the
    /// current model's aim events. -1 when it does not bind.
    fn create_typed_channel(
        &mut self,
        _: u32,
        joint: DeformInstanceHandle,
        bind: DeformInstanceHandle,
    ) -> u32 {
        let Some(index) = self.current else {
            return u32::MAX;
        };
        let owner = AimOwner {
            models: Weak::clone(&self.models),
            index,
        };
        let desc = BindDesc {
            kind: 0,
            extra: 0,
            context: 0,
            instance: inst(joint),
        };
        let scene = self.scene.borrow();
        self.animation
            .borrow_mut()
            .create_typed(&scene, Box::new(owner), 4, desc, MixerKey::of(inst(bind)))
            .map_or(u32::MAX, channel_key)
    }

    fn play_channel(&mut self, key: u32, blob: &[u32; 7]) {
        let Some(id) = typed_id(key) else {
            return;
        };
        // The header's seventh word is a pose pointer; the aim's is null.
        let header = PlayHeader {
            start: f32::from_bits(blob[0]),
            flags: blob[1],
            rate: f32::from_bits(blob[2]),
            blend: f32::from_bits(blob[3]),
            scale: f32::from_bits(blob[4]),
            factor: f32::from_bits(blob[5]),
            block: None,
        };
        let scene = self.scene.borrow();
        self.animation
            .borrow_mut()
            .play_channel(&scene, id, &header);
    }

    fn destroy_channel(&mut self, key: u32) {
        let Some(id) = typed_id(key) else {
            return;
        };
        let scene = self.scene.borrow();
        self.animation.borrow_mut().destroy_channel(&scene, id);
    }

    fn channel_in_mixer(&mut self, key: u32) -> u8 {
        typed_id(key).map_or(0, |id| u8::from(self.animation.borrow().in_mixer(id)))
    }

    /// `GetObject(key)`, `GetFlags` and `Sample(time)` on the channel's
    /// archetype: the two frames' payload words and the fraction; zeros
    /// for a key without an archetype or a time outside it (D501).
    fn sample_channel(&mut self, key: u32, time: f32) -> ChannelSample {
        let animation = self.animation.borrow();
        let channel = typed_id(key)
            .and_then(|id| animation.channel(id))
            .and_then(TypedChannel::channel);
        let Some(channel) = channel else {
            return ChannelSample {
                a: Vec::new(),
                b: Vec::new(),
                alpha: 0.0,
            };
        };
        let words = (payload_bytes(channel.flags()) / 4) as usize;
        match sample::sample(channel, time) {
            Ok(s) => ChannelSample {
                a: sample::frame_words::<7>(channel, s.a)[..words.min(7)].to_vec(),
                b: sample::frame_words::<7>(channel, s.b)[..words.min(7)].to_vec(),
                alpha: s.alpha,
            },
            Err(_) => ChannelSample {
                a: vec![0.0; words.min(7)],
                b: vec![0.0; words.min(7)],
                alpha: 0.0,
            },
        }
    }

    fn rand(&mut self) -> i32 {
        self.seed = self.seed.wrapping_mul(214_013).wrapping_add(2_531_011);
        ((self.seed >> 16) & 0x7FFF).cast_signed()
    }
}

/// Hands a deformable model's bound bones the scene engine's world
/// transforms, as the engine's `IInstanceTransform` calls on the bone
/// runtimes would (`SetMatrix`, `MarkWorldDirty`). The bones written.
pub fn pull_bones(instance: &mut DeformInstance, scene: &SceneEngine) -> usize {
    instance.pull_bone_transforms(|bone| {
        inst(bone).map(|i| scene.instance_transform(Some(i)).to_words())
    })
}

/// The walk and update of `FEngineCharacterBody::Advance(dt)` for the model
/// `index`: `MixerWalk` under its root bone, its instances' sinks and its
/// aim events' owners called as the channels tick (what the sinks forward
/// to the caller's sink queued in `events`), then `UpdateInstance` of the
/// root bone and the bound bones handed their world transforms.
/// `Advance` then counts the list (`GetListCount`, which sweeps it). False
/// for no such model.
pub fn walk_deformable(
    scene: &RefCell<SceneEngine>,
    animation: &RefCell<Animation>,
    models: &RefCell<Vec<Option<DeformInstance>>>,
    names: &mut Names,
    events: &mut Vec<DeformableEvent>,
    index: usize,
    dt: f32,
) -> bool {
    let root = {
        let models = models.borrow();
        let Some(Some(instance)) = models.get(index) else {
            return false;
        };
        inst(instance.first_bone())
    };
    let mut scene = scene.borrow_mut();
    let mut sink = DeformSink {
        models,
        names,
        events,
    };
    animation
        .borrow_mut()
        .mixer_walk_into(&mut scene, MixerKey::of(root), dt, &mut sink);
    scene.update_instance(root, 0);
    if let Some(Some(instance)) = models.borrow_mut().get_mut(index) {
        pull_bones(instance, &scene);
    }
    true
}
