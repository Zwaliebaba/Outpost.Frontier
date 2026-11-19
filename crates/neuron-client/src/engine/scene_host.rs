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

use neuron_core::math::{Matrix, Vector};
use neuron_core::scene::{
    ArchetypeId, Engine as SceneEngine, EngineComponent, Hardpoints, InstanceId, SceneError,
    connect_hardpoints,
};
use neuron_core::vfs::FileSystem;

use crate::render::deformable::{
    BoneLink, ChannelSample, DeformHost, EngineHandle, Instance as DeformInstanceHandle,
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

/// The engine half of `Deformable2`'s calls ([`DeformHost`]) on the scene
/// engine: archetypes, instances, transforms, joints and hardpoints.
///
/// The animation system's calls of the aim events and the channel calls
/// (`PlayScript`, `EvalPlayback`, the root data, the typed channels) answer
/// as the reference's interfaces do when the call fails: the deformable's
/// aim events are not driven yet (the open item of P4.7, D406), and no
/// shipped model needs them to skin and draw.
pub struct SceneDeformHost {
    scene: SharedScene,
    links: SharedLinks,
    /// `rand()`'s state: the Microsoft C run time's linear congruential
    /// generator, its own and seeded with 1 (D456).
    seed: u32,
}

impl SceneDeformHost {
    /// The host over the engine.
    #[must_use]
    pub fn new(scene: SharedScene, links: SharedLinks) -> SceneDeformHost {
        SceneDeformHost {
            scene,
            links,
            seed: 1,
        }
    }

    fn arch(&self, handle: EngineHandle) -> Option<ArchetypeId> {
        self.links.borrow().archetype(handle)
    }

    fn id(instance: Option<InstanceId>) -> DeformInstanceHandle {
        instance.map_or(-1, |i| i32::try_from(i.index()).unwrap_or(-1))
    }

    fn inst(instance: DeformInstanceHandle) -> Option<InstanceId> {
        u32::try_from(instance).ok().map(InstanceId::from_index)
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
        Self::id(self.scene.borrow().instance_parent(Self::inst(instance)))
    }

    fn instance_relative(&mut self, instance: DeformInstanceHandle) -> [u32; 23] {
        self.scene
            .borrow()
            .instance_relative(Self::inst(instance))
            .map_or([0; 23], |p| p.0)
    }

    fn instance_position(&mut self, instance: DeformInstanceHandle) -> [f32; 3] {
        self.scene
            .borrow()
            .instance_position(Self::inst(instance))
            .to_words()
    }

    fn instance_orientation(&mut self, instance: DeformInstanceHandle) -> [f32; 9] {
        self.scene
            .borrow()
            .instance_orientation(Self::inst(instance))
            .to_words()
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
        self.scene
            .borrow()
            .joint_params(Self::inst(instance), index)
            .unwrap_or_default()
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

    fn play_script(&mut self, _: u32, _: DeformInstanceHandle, _: u32, _: u32) -> i32 {
        -1
    }

    fn destroy_script(&mut self, _: i32) {}

    fn filter_objects(&mut self, _: i32) -> u32 {
        0
    }

    fn root_eval(
        &mut self,
        _: i32,
        _: [f32; 3],
        _: u32,
        _: f32,
        _: f32,
        _: u32,
        _: u32,
    ) -> Option<(u8, [f32; 3])> {
        None
    }

    fn named_duration(&mut self, _: u32, _: u32) -> Option<f32> {
        None
    }

    fn eval_playback(
        &mut self,
        _: i32,
        _: u32,
        _: f32,
        _: f32,
        _: u32,
        _: u32,
        _: &[f32; 12],
    ) -> i32 {
        0
    }

    fn has_object(&mut self, _: i32, _: u32) -> u32 {
        0
    }

    fn first_instance_field(&mut self, _: i32) -> Option<u32> {
        None
    }

    fn duration_if_named(&mut self, _: i32) -> Option<f32> {
        None
    }

    fn touch_mapped(&mut self, _: i32) -> ([f32; 3], [f32; 3]) {
        ([0.0; 3], [0.0; 3])
    }

    fn set_overlay_pos(&mut self, _: i32, _: &[f32; 3]) {}

    fn set_overlay_extra(&mut self, _: i32, _: &[f32; 9]) {}

    fn add_overlay_pos(&mut self, _: i32, _: &[f32; 3]) {}

    fn find_object(&mut self, _: i32, _: u32, _: DeformInstanceHandle) -> u32 {
        0
    }

    fn register_handle(&mut self, _: i32) {}

    fn has_handle(&mut self, _: i32) -> u8 {
        0
    }

    fn has_any_handle(&mut self, _: i32) {}

    fn create_typed_channel(
        &mut self,
        _: u32,
        _: DeformInstanceHandle,
        _: DeformInstanceHandle,
    ) -> u32 {
        0
    }

    fn play_channel(&mut self, _: u32, _: &[u32; 7]) {}

    fn destroy_channel(&mut self, _: u32) {}

    fn channel_in_mixer(&mut self, _: u32) -> u8 {
        0
    }

    fn sample_channel(&mut self, _: u32, _: f32) -> ChannelSample {
        ChannelSample {
            a: Vec::new(),
            b: Vec::new(),
            alpha: 0.0,
        }
    }

    fn rand(&mut self) -> i32 {
        self.seed = self.seed.wrapping_mul(214_013).wrapping_add(2_531_011);
        ((self.seed >> 16) & 0x7FFF).cast_signed()
    }
}
