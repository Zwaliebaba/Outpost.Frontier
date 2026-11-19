//! The particle system (`FParticleFx` in `particlefx_update.cpp`, the tree
//! helpers of `particlefx_common.cpp` and the node base of
//! `particlefx_genericfx.cpp`): the node arena with the reference's
//! reference counts, the node library of prototypes, the effect library,
//! the pools and the random generator, the tree links and the walks that
//! compose transforms and update nodes.

use std::collections::BTreeMap;
use std::rc::Rc;

use neuron_core::fx_data::{self as fx, NodeClass};
use neuron_core::math::{Transform, general_inverse};

use super::draw::DrawState;
use super::node::{
    Behaviour, FLAG_DEAD, FLAG_DONE, FLAG_LOCAL_SET, FLAG_NO_ANIMATION, FLAG_WORLD_SET, Family,
    Node, NodeId,
};
use super::pool::{PoolKind, Pools};
use super::props::NodeProps;
use super::ramp::{IDENTITY, mul};
use super::rand::ParticleRand;
use super::services::ParticleServices;

#[derive(Clone, Debug)]
struct Slot {
    generation: u32,
    node: Option<Node>,
}

/// The particle system: everything the reference keeps in `particlefx.dll`'s
/// globals and its `ParticleFx` component.
#[derive(Clone, Debug)]
pub struct ParticleFx {
    slots: Vec<Slot>,
    free_slots: Vec<u32>,
    /// The appearance pools.
    pub pools: Pools,
    /// The random generator.
    pub rand: ParticleRand,
    prototypes: BTreeMap<u32, NodeId>,
    effects: fx::EffectLibrary,
    /// The draw state.
    pub draw: DrawState,
}

impl Default for ParticleFx {
    fn default() -> Self {
        Self::new()
    }
}

/// The pool of an appearance class.
#[must_use]
pub const fn pool_of(class: NodeClass) -> Option<PoolKind> {
    match class {
        NodeClass::BasicAppearance => Some(PoolKind::Basic),
        NodeClass::ParticleAppearance => Some(PoolKind::Particle),
        NodeClass::RectAppearance => Some(PoolKind::Rect),
        NodeClass::PerpAppearance => Some(PoolKind::Perp),
        NodeClass::MeshAppearance => Some(PoolKind::Mesh),
        NodeClass::OrientedAppearance => Some(PoolKind::Oriented),
        NodeClass::DustAppearance => Some(PoolKind::Dust),
        NodeClass::BeamAppearance => Some(PoolKind::Beam),
        _ => None,
    }
}

impl ParticleFx {
    /// A system with empty libraries, every pool at the default size and
    /// the generator in its load state.
    #[must_use]
    pub fn new() -> Self {
        Self {
            slots: Vec::new(),
            free_slots: Vec::new(),
            pools: Pools::new(),
            rand: ParticleRand::default(),
            prototypes: BTreeMap::new(),
            effects: fx::EffectLibrary::new(),
            draw: DrawState::default(),
        }
    }

    // --- The arena -----------------------------------------------------------------------

    /// A live node.
    #[must_use]
    pub fn node(&self, id: NodeId) -> Option<&Node> {
        let slot = self.slots.get(id.index as usize)?;
        if slot.generation != id.generation {
            return None;
        }
        slot.node.as_ref()
    }

    pub(crate) fn node_mut(&mut self, id: NodeId) -> Option<&mut Node> {
        let slot = self.slots.get_mut(id.index as usize)?;
        if slot.generation != id.generation {
            return None;
        }
        slot.node.as_mut()
    }

    /// A node that must be live (the reference would follow the pointer).
    pub(crate) fn n(&self, id: NodeId) -> &Node {
        self.node(id)
            .unwrap_or_else(|| panic!("particle node {id:?} is gone"))
    }

    pub(crate) fn n_mut(&mut self, id: NodeId) -> &mut Node {
        self.node_mut(id)
            .unwrap_or_else(|| panic!("particle node {id:?} is gone"))
    }

    fn alloc(&mut self, node: Node) -> NodeId {
        if let Some(index) = self.free_slots.pop() {
            let slot = &mut self.slots[index as usize];
            slot.node = Some(node);
            NodeId {
                index,
                generation: slot.generation,
            }
        } else {
            let index = u32::try_from(self.slots.len()).unwrap_or(u32::MAX);
            self.slots.push(Slot {
                generation: 0,
                node: Some(node),
            });
            NodeId {
                index,
                generation: 0,
            }
        }
    }

    /// The number of live nodes.
    #[must_use]
    pub fn node_count(&self) -> usize {
        self.slots.iter().filter(|s| s.node.is_some()).count()
    }

    /// `AddRef`.
    pub fn add_ref(&mut self, id: NodeId) {
        let node = self.n_mut(id);
        node.refs += 1;
    }

    /// `Release`: at zero the node is destroyed: its particles go back to
    /// its pool, its texture handle and the references it holds (an
    /// emitter's appearance, an appearance's fields) are released.
    pub fn release(&mut self, id: NodeId, svc: &mut dyn ParticleServices) {
        let Some(node) = self.node_mut(id) else {
            return;
        };
        node.refs = node.refs.saturating_sub(1);
        if node.refs != 0 {
            return;
        }
        let slot = &mut self.slots[id.index as usize];
        let node = slot.node.take().unwrap_or_else(|| unreachable!());
        slot.generation = slot.generation.wrapping_add(1);
        self.free_slots.push(id.index);
        self.destroy(node, svc);
    }

    /// The destructor (`~FGenericFxObjectBase`): the particle list, then the
    /// texture, then the associated appearance, then the fields.
    fn destroy(&mut self, node: Node, svc: &mut dyn ParticleServices) {
        match node.behaviour {
            Behaviour::Appearance(app) => {
                if let Some(kind) = pool_of(node.props.class) {
                    let pool = self.pools.get_mut(kind);
                    let mut cursor = app.head;
                    while let Some(index) = cursor {
                        cursor = pool.item(index).next;
                        let item = pool.item_mut(index);
                        item.next = None;
                        item.prev = None;
                        pool.free(index);
                    }
                }
                if app.texture.handle != -1 {
                    svc.dec_ref_texture(app.texture.handle);
                }
                for field in app.fields.into_iter().flatten() {
                    self.release(field, svc);
                }
            }
            Behaviour::Emitter(emitter) => {
                if let Some(appearance) = emitter.appearance {
                    self.release(appearance, svc);
                }
            }
            Behaviour::Field(_) | Behaviour::Folder => {}
        }
    }

    // --- Libraries -----------------------------------------------------------------------

    /// A node made by component name (`CreateInstance` of a class's
    /// factory): the class's default set, one reference for the caller.
    pub fn create_node(&mut self, class: NodeClass) -> NodeId {
        self.alloc(Node::new(Rc::new(NodeProps::for_class(class))))
    }

    /// Adds a loaded node as a prototype keyed by its name hash; a hash
    /// already present keeps the first prototype (`InsertNodeByName`).
    pub fn insert_prototype(&mut self, node: &fx::FxNode) -> bool {
        if self.prototypes.contains_key(&node.hash) {
            return false;
        }
        let id = self.alloc(Node::new(Rc::new(NodeProps::new(&node.props))));
        self.prototypes.insert(node.hash, id);
        true
    }

    /// The prototype of a name hash.
    #[must_use]
    pub fn prototype(&self, hash: u32) -> Option<NodeId> {
        self.prototypes.get(&hash).copied()
    }

    /// Adds an effect; a hash already present keeps the first.
    pub fn insert_effect(&mut self, effect: fx::EffectArchetype) -> bool {
        self.effects.insert(effect)
    }

    /// The effect of a name hash.
    #[must_use]
    pub fn effect_archetype(&self, hash: u32) -> Option<&fx::EffectArchetype> {
        self.effects.find(hash)
    }

    /// Both libraries of a loaded `.ale` file, as the client loads one into
    /// the node library and the effect library.
    pub fn load(&mut self, ale: &fx::AleFile) {
        for node in ale.nodes.nodes() {
            self.insert_prototype(node);
        }
        for effect in ale.effects.effects() {
            self.insert_effect(effect.clone());
        }
    }

    /// `CloneNode` of a prototype: a new node of the same class sharing
    /// its set, with the set's life span as duration; one reference.
    pub fn clone_node(&mut self, prototype: NodeId) -> NodeId {
        let props = Rc::clone(&self.n(prototype).props);
        self.alloc(Node::new(props))
    }

    // --- The tree ------------------------------------------------------------------------

    /// `LinkNodeTree(parent, child)`: the child goes after the parent's
    /// children whose sort key is not greater than its own.
    fn link_tree(&mut self, parent: NodeId, child: NodeId) {
        self.n_mut(child).parent = Some(parent);
        let Some(first) = self.n(parent).first_child else {
            self.n_mut(parent).first_child = Some(child);
            return;
        };
        let key = self.n(child).family().sort_key();
        let mut cursor = Some(first);
        let mut prev = None;
        while let Some(c) = cursor {
            if self.n(c).family().sort_key() > key {
                break;
            }
            prev = Some(c);
            cursor = self.n(c).next;
        }
        match prev {
            Some(p) => self.n_mut(p).next = Some(child),
            None => self.n_mut(parent).first_child = Some(child),
        }
        if let Some(c) = cursor {
            self.n_mut(c).prev = Some(child);
        }
        let node = self.n_mut(child);
        node.prev = prev;
        node.next = cursor;
    }

    /// `AttachNode(parent, child)`: fails for a child that has a parent;
    /// otherwise links it and takes a reference on both.
    pub fn attach(&mut self, parent: NodeId, child: NodeId) -> bool {
        if self.n(child).parent.is_some() {
            return false;
        }
        self.link_tree(parent, child);
        self.add_ref(parent);
        self.add_ref(child);
        true
    }

    /// `DetachNode(node)`: unlinks it and releases it and its parent; fails
    /// for a node without a parent.
    pub fn detach(&mut self, id: NodeId, svc: &mut dyn ParticleServices) -> bool {
        let Some(parent) = self.n(id).parent else {
            return false;
        };
        let (prev, next) = (self.n(id).prev, self.n(id).next);
        if let Some(p) = prev {
            self.n_mut(p).next = next;
        }
        if let Some(n) = next {
            self.n_mut(n).prev = prev;
        }
        if self.n(parent).first_child == Some(id) {
            self.n_mut(parent).first_child = next;
        }
        let node = self.n_mut(id);
        node.parent = None;
        node.next = None;
        node.prev = None;
        self.release(parent, svc);
        self.release(id, svc);
        true
    }

    /// `SetNodeFlags`: an emitter that gets bit 31 releases its emitter
    /// reference on its appearance, every time.
    pub fn set_node_flags(&mut self, id: NodeId, flags: u32) {
        let node = self.n_mut(id);
        let appearance = node.emitter().and_then(|e| e.appearance);
        node.flags = flags;
        if let Some(app) = appearance
            && flags & FLAG_DEAD != 0
            && let Some(state) = self.node_mut(app).and_then(Node::appearance_mut)
        {
            state.emitter_refs -= 1;
        }
    }

    /// `SubmitNode`: its children first (depth first), then the node gets
    /// bit 31 and is detached.
    pub fn submit(&mut self, id: NodeId, svc: &mut dyn ParticleServices) {
        if let Some(child) = self.n(id).first_child {
            self.submit_chain(child, svc);
        }
        let flags = self.n(id).flags | FLAG_DEAD;
        self.set_node_flags(id, flags);
        self.detach(id, svc);
    }

    /// `SubmitNodeChain`: every sibling from `first`, each after its own
    /// children; the next sibling is taken before the node is detached.
    fn submit_chain(&mut self, first: NodeId, svc: &mut dyn ParticleServices) {
        let mut cursor = Some(first);
        while let Some(id) = cursor {
            let Some(node) = self.node(id) else {
                return;
            };
            cursor = node.next;
            if let Some(child) = node.first_child {
                self.submit_chain(child, svc);
            }
            let flags = self.n(id).flags | FLAG_DEAD;
            self.set_node_flags(id, flags);
            self.detach(id, svc);
        }
    }

    // --- Transforms ----------------------------------------------------------------------

    /// `SetNodeTransform(slot, xform)`: slot 0 the composed transform (flag
    /// 8), slot 1 the raw one (flag 4); other slots fail.
    pub fn set_transform(&mut self, id: NodeId, slot: u32, xform: &[f32; 12]) -> bool {
        let node = self.n_mut(id);
        match slot {
            0 => {
                node.world = *xform;
                node.flags |= FLAG_WORLD_SET;
                true
            }
            1 => {
                node.local = *xform;
                node.flags |= FLAG_LOCAL_SET;
                true
            }
            _ => false,
        }
    }

    /// `ComposeNodeTransform(parent)`.
    fn compose(&mut self, id: NodeId, parent: &[f32; 12]) {
        let node = self.n_mut(id);
        let flags = node.flags;
        if flags & FLAG_WORLD_SET != 0 {
            node.flags = flags & !FLAG_WORLD_SET;
            return;
        }
        let skip = flags & FLAG_NO_ANIMATION != 0 || node.props.transform.is_identity();
        if flags & FLAG_LOCAL_SET == 0 {
            if skip {
                node.local = IDENTITY;
                node.world = *parent;
            } else {
                node.local = node.props.transform.evaluate(node.outer_time, node.age);
                node.world = mul(parent, &node.local);
            }
        } else if skip {
            node.world = mul(parent, &node.local);
        } else {
            let local = node.props.transform.evaluate(node.outer_time, node.age);
            let world = mul(parent, &node.local);
            node.world = mul(&world, &local);
        }
    }

    /// `ParticleFx::Slot15(_, start)`: the start node composed with the
    /// identity, then its children with its result
    /// (`PropagateNodeChainTransform`). Its siblings are not visited.
    pub fn compose_tree(&mut self, start: NodeId) {
        self.compose(start, &IDENTITY);
        let world = self.n(start).world;
        if let Some(child) = self.n(start).first_child {
            self.compose_chain(child, &world);
        }
    }

    fn compose_chain(&mut self, first: NodeId, parent: &[f32; 12]) {
        let mut cursor = Some(first);
        while let Some(id) = cursor {
            self.compose(id, parent);
            let node = self.n(id);
            if let Some(child) = node.first_child {
                let world = node.world;
                self.compose_chain(child, &world);
            }
            cursor = self.n(id).next;
        }
    }

    /// `ComputeRelativeTransform(other)` on `id`: the node's composed
    /// transform in `other`'s space, through `GeneralInverse` (whose
    /// output the reference uses whether the matrix inverts or not).
    pub(crate) fn relative_transform(&self, id: NodeId, other: NodeId) -> [f32; 12] {
        let inverse = general_inverse(&Transform::from_words(self.n(other).world)).transform;
        mul(&inverse.to_words(), &self.n(id).world)
    }

    // --- The update walk -----------------------------------------------------------------

    /// `TickLifetime(dt)`: the age grows; past the duration the node is
    /// done.
    pub(crate) fn tick_lifetime(&mut self, id: NodeId, dt: f32) {
        let node = self.n_mut(id);
        node.age = super::ramp::round(f64::from(dt) + f64::from(node.age));
        if node.age > node.duration {
            node.flags |= FLAG_DONE;
        }
    }

    /// The per-node update (node vtable slot +0x14).
    fn pad5(&mut self, id: NodeId, dt: f32, svc: &mut dyn ParticleServices) {
        match self.n(id).family() {
            _ if matches!(self.n(id).behaviour, Behaviour::Folder) => self.tick_lifetime(id, dt),
            Family::Emitter => self.emitter_update(id, dt, svc),
            Family::Field => self.field_update(id, dt),
            Family::Appearance => self.appearance_update(id, dt, svc),
            Family::Folder => self.tick_lifetime(id, dt),
        }
    }

    /// `ParticleFx::Slot16(dt, start)`: the start node's update; when it is
    /// done it is submitted, otherwise its children are walked
    /// (`WalkNodeChainFlagDispatch`).
    pub fn update_tree(&mut self, start: NodeId, dt: f32, svc: &mut dyn ParticleServices) {
        self.pad5(start, dt, svc);
        if self.n(start).flags & FLAG_DONE != 0 {
            self.submit(start, svc);
            return;
        }
        if let Some(child) = self.n(start).first_child {
            self.update_chain(child, dt, svc);
        }
    }

    fn update_chain(&mut self, first: NodeId, dt: f32, svc: &mut dyn ParticleServices) {
        let mut cursor = Some(first);
        while let Some(id) = cursor {
            let Some(node) = self.node(id) else {
                return;
            };
            cursor = node.next;
            self.pad5(id, dt, svc);
            if self.n(id).flags & FLAG_DONE != 0 {
                self.submit(id, svc);
            } else if let Some(child) = self.n(id).first_child {
                self.update_chain(child, dt, svc);
            }
        }
    }
}
