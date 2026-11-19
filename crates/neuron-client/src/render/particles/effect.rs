//! An effect instance (`FFxEffectObject` in `particlefx_effectobject.cpp`,
//! `CreateEffect` in `particlefx_effectlibrary.cpp`): the archetype's node
//! records instantiated as clones of the library prototypes and folders,
//! the root records sorted by family, attached under the caller's parent,
//! the emitter and field pairs bound, and the per-frame calls the client
//! makes through `IFxEffect`.

use neuron_core::fx_data::{EffectArchetype, NodeClass, ROOT_PARENT};

use super::ParticleFx;
use super::node::{Family, NodeId};
use super::services::{Camera, ParticleDraw, ParticleServices};

/// The name hash of a folder whose id makes it the effect's transform
/// override (`0xEE223B51`).
pub const OVERRIDE_FOLDER_HASH: u32 = 0xEE22_3B51;

/// Why `CreateEffect` failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EffectError {
    /// No effect has the name hash (`GR_ALE_CREATE_FAIL`).
    UnknownEffect,
    /// A record names a node the library does not hold ("couldn't create
    /// instace of node"); the records made so far are released (D373).
    UnknownNode(u32),
}

/// One node record (`FFxEffectNodeRec`): the node with a reference, the
/// record's kind, id and parent id.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EffectRecord {
    /// The node.
    pub node: NodeId,
    /// 0 for a library node, otherwise a folder.
    pub kind: u32,
    /// The id within the effect.
    pub id: u32,
    /// The parent's id or [`ROOT_PARENT`].
    pub parent: u32,
}

/// An effect instance.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Effect {
    records: Vec<EffectRecord>,
    root_count: usize,
    override_record: Option<usize>,
}

impl Effect {
    /// The records: the root records first, sorted by family, then the
    /// others in archetype order.
    #[must_use]
    pub fn records(&self) -> &[EffectRecord] {
        &self.records
    }

    /// The number of root records.
    #[must_use]
    pub const fn root_count(&self) -> usize {
        self.root_count
    }

    /// The record whose node takes the transform calls alone.
    #[must_use]
    pub fn override_record(&self) -> Option<&EffectRecord> {
        self.override_record.map(|i| &self.records[i])
    }

    fn find(&self, id: u32) -> Option<NodeId> {
        // The reference reads the record past the end on a miss (D371).
        self.records.iter().find(|r| r.id == id).map(|r| r.node)
    }

    /// `Slot3(slot, xform)`: the override node alone, or every live root
    /// node; returns the override node's result, else success.
    pub fn set_transform(&self, fx: &mut ParticleFx, slot: u32, xform: &[f32; 12]) -> bool {
        if let Some(record) = self.override_record() {
            return fx.set_transform(record.node, slot, xform);
        }
        for record in &self.records[..self.root_count] {
            if !fx.n(record.node).is_dead() {
                // A failure only writes a warning.
                fx.set_transform(record.node, slot, xform);
            }
        }
        true
    }

    /// `Slot4(slot)`: the override node's transform, else the first
    /// record's (slot 0 composed, 1 raw).
    #[must_use]
    pub fn transform(&self, fx: &ParticleFx, slot: u32) -> Option<[f32; 12]> {
        let record = self.override_record().or_else(|| self.records.first())?;
        let node = fx.n(record.node);
        match slot {
            0 => Some(node.world),
            1 => Some(node.local),
            _ => None,
        }
    }

    /// `Slot7`: every live root node's subtree composes its transforms
    /// (`ParticleFx::Slot15`); particle appearances make their death
    /// effects and mesh appearances pose their meshes on the way.
    pub fn compose(&self, fx: &mut ParticleFx, svc: &mut dyn ParticleServices) {
        for record in &self.records[..self.root_count] {
            if !fx.n(record.node).is_dead() {
                fx.compose_tree(record.node, svc);
            }
        }
    }

    /// `Slot8(dt)`: every live root node's subtree updates
    /// (`ParticleFx::Slot16`).
    pub fn update(&self, fx: &mut ParticleFx, dt: f32, svc: &mut dyn ParticleServices) {
        for record in &self.records[..self.root_count] {
            if fx.node(record.node).is_some_and(|n| !n.is_dead()) {
                fx.update_tree(record.node, dt, svc);
            }
        }
    }

    /// `Slot9(camera)`: every live root node's subtree draws
    /// (`ParticleFx::DrawNodes`); the last walk's result, else success.
    pub fn draw(
        &self,
        fx: &mut ParticleFx,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) -> i32 {
        let mut result = 0;
        // After `Slot13(7)` the records are gone (the reference walks its
        // root count past the end).
        for record in self.records.iter().take(self.root_count) {
            if fx.node(record.node).is_some_and(|n| !n.is_dead()) {
                result = fx.draw_nodes(record.node, camera, svc);
            }
        }
        result
    }

    /// `Slot15(sphere)`: the bounding spheres of every record's node merged
    /// (`MergeNodeBoundsSphere`); `false` when no node has one.
    pub fn bounds(&self, fx: &ParticleFx, sphere: &mut [f32; 4]) -> bool {
        super::bounds::merge_node_spheres(fx, self.records.iter().map(|r| r.node), sphere)
    }

    /// `Slot5`: a value stored on every record's node.
    fn walk(&self, fx: &mut ParticleFx, set: impl Fn(&mut super::node::Node)) {
        for record in &self.records {
            set(fx.n_mut(record.node));
        }
    }

    /// `Slot5(age)`.
    pub fn set_age(&self, fx: &mut ParticleFx, age: f32) {
        self.walk(fx, |n| n.age = age);
    }

    /// `Slot6(outer_time)`: every record's node's `SetNodeOuterTime` (a
    /// particle appearance hands it on to its child effects).
    pub fn set_outer_time(&self, fx: &mut ParticleFx, outer_time: f32) {
        for record in &self.records {
            fx.set_node_outer_time(record.node, outer_time);
        }
    }

    /// `Slot14(lod_input)`: every record's node's `SetNodeLodInput` (a
    /// particle appearance hands it on to its child effects).
    pub fn set_lod_input(&self, fx: &mut ParticleFx, lod_input: f32) {
        for record in &self.records {
            fx.set_node_lod_input(record.node, lod_input);
        }
    }

    fn family_bit(fx: &ParticleFx, node: NodeId, flags: u32) -> bool {
        let family = fx.n(node).family();
        (flags & 1 != 0 && family == Family::Emitter)
            || (flags & 2 != 0 && family == Family::Field)
            || (flags & 4 != 0 && family == Family::Appearance)
    }

    /// `Slot11(flags)`: false when a live node (the override node aside) is
    /// of a family `flags` names (1 emitters, 2 fields, 4 appearances), or
    /// for `flags` 7 when any live node is left; true otherwise. The
    /// client asks with 4 whether the effect has finished.
    #[must_use]
    pub fn finished(&self, fx: &ParticleFx, flags: u32) -> bool {
        let skip = self.override_record().map(|r| r.node);
        for record in &self.records {
            if Some(record.node) == skip || fx.n(record.node).is_dead() {
                continue;
            }
            if flags == 7 || Self::family_bit(fx, record.node, flags) {
                return false;
            }
        }
        true
    }

    /// `Slot13(flags)`: every live node of the named families is submitted
    /// (1 stops the emitters). With 7 the live root nodes are submitted
    /// and every record is released.
    pub fn dispatch(&mut self, fx: &mut ParticleFx, flags: u32, svc: &mut dyn ParticleServices) {
        if flags == 7 {
            for record in &self.records[..self.root_count.min(self.records.len())] {
                if !fx.n(record.node).is_dead() {
                    fx.submit(record.node, svc);
                }
            }
            self.release_records(fx, svc);
            return;
        }
        for record in &self.records {
            if fx.n(record.node).is_dead() {
                continue;
            }
            let family = fx.n(record.node).family();
            for (bit, wanted) in [
                (1, Family::Emitter),
                (2, Family::Field),
                (4, Family::Appearance),
            ] {
                if flags & bit != 0 && family == wanted {
                    fx.submit(record.node, svc);
                }
            }
        }
    }

    fn release_records(&mut self, fx: &mut ParticleFx, svc: &mut dyn ParticleServices) {
        for record in std::mem::take(&mut self.records) {
            fx.release(record.node, svc);
        }
        self.override_record = None;
    }

    /// `Release` of the effect: its records release their nodes in order.
    pub fn release(mut self, fx: &mut ParticleFx, svc: &mut dyn ParticleServices) {
        self.release_records(fx, svc);
    }
}

impl ParticleFx {
    /// `CreateEffect(parent, hash)`: an instance of the effect `hash` whose
    /// root nodes are attached under `parent`.
    ///
    /// # Errors
    ///
    /// No effect of that hash, or a record whose library node is missing.
    pub fn create_effect(
        &mut self,
        parent: NodeId,
        hash: u32,
        svc: &mut dyn ParticleServices,
    ) -> Result<Effect, EffectError> {
        let archetype: EffectArchetype = self
            .effect_archetype(hash)
            .ok_or(EffectError::UnknownEffect)?
            .clone();
        let mut effect = Effect {
            records: Vec::with_capacity(archetype.nodes.len()),
            root_count: 0,
            override_record: None,
        };
        let mut sorted_roots = 0;
        let mut override_id = ROOT_PARENT;
        for rec in &archetype.nodes {
            let node = if rec.kind == 0 {
                let Some(prototype) = self.prototype(rec.node_hash) else {
                    effect.release(self, svc);
                    return Err(EffectError::UnknownNode(rec.node_hash));
                };
                self.clone_node(prototype)
            } else {
                if rec.node_hash == OVERRIDE_FOLDER_HASH {
                    override_id = rec.id;
                }
                self.create_node(NodeClass::Node)
            };
            let record = EffectRecord {
                node,
                kind: rec.kind,
                id: rec.id,
                parent: rec.parent_id,
            };
            if rec.parent_id == ROOT_PARENT {
                let key = self.n(node).family().sort_key();
                let mut index = 0;
                while index < sorted_roots
                    && self.n(effect.records[index].node).family().sort_key() <= key
                {
                    index += 1;
                }
                effect.records.insert(index, record);
                sorted_roots += 1;
            } else {
                effect.records.push(record);
            }
        }
        if override_id != ROOT_PARENT {
            effect.override_record = effect.records.iter().position(|r| r.id == override_id);
        }
        // Attach every record: roots under the caller's parent, the others
        // under the record of their parent id.
        for index in 0..effect.records.len() {
            let record = effect.records[index];
            let parent = if record.parent == ROOT_PARENT {
                effect.root_count += 1;
                Some(parent)
            } else {
                effect.find(record.parent)
            };
            if let Some(parent) = parent {
                self.attach(parent, record.node);
            }
        }
        // The emitter/appearance and appearance/field pairs.
        for pair in &archetype.pairs {
            let Some(first) = effect.find(pair.first) else {
                continue;
            };
            let second = effect.find(pair.second);
            match self.n(first).family() {
                Family::Emitter => {
                    let appearance = second.filter(|&s| self.n(s).family() == Family::Appearance);
                    self.associate_appearance(first, appearance, svc);
                }
                Family::Appearance => {
                    let field = second.filter(|&s| self.n(s).family() == Family::Field);
                    self.associate_field(first, field);
                }
                // "Unknown associate combination".
                Family::Field | Family::Folder => {}
            }
        }
        // A folder record of kind 1 lasts as long as its longest subtree.
        for record in &effect.records {
            if record.kind != 1 {
                continue;
            }
            let duration = self
                .n(record.node)
                .first_child
                .map_or(0.0, |child| self.max_subtree_duration(child));
            self.n_mut(record.node).duration = duration;
        }
        Ok(effect)
    }

    /// `ComputeMaxSubtreeDuration(first)`: the largest duration in a
    /// sibling chain and below it.
    fn max_subtree_duration(&self, first: NodeId) -> f32 {
        let mut max = 0.0_f32;
        let mut cursor = Some(first);
        while let Some(id) = cursor {
            let node = self.n(id);
            if node.duration > max {
                max = node.duration;
            }
            if let Some(child) = node.first_child {
                let sub = self.max_subtree_duration(child);
                if !(sub < max) {
                    max = sub;
                }
            }
            cursor = node.next;
        }
        max
    }
}
