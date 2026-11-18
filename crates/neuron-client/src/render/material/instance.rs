//! Material instances: the `IMaterial` objects the library creates through
//! the component manager, here boxed [`Material`] values in an arena with
//! generational handles and the reference count the reference keeps on each
//! object.
//!
//! The reference's materials are COM objects: `AddRef` and `Release` count
//! references, the last `Release` destroys the object. The library and the
//! batcher call `AddRef` and `Release` at fixed points, and callers that
//! receive a material (`FindOrCreate`, `FindMaterialByCrc`,
//! `EnumGroupMaterial`) receive a reference they must release. The arena
//! keeps that count: [`Materials::add_ref`] and [`Materials::release`]
//! stand for the two calls, and the last release calls
//! [`Material::destroy`] and frees the slot. A handle to a freed slot names
//! nothing (divergence D303).
//!
//! The 22 material kinds and `BatchedMaterial` implement [`Material`]
//! (the next increment of work package P4.5); a [`MaterialFactory`] makes
//! them by type name, as the component manager does for the factories
//! `Shading.cpp` registers (divergence D301).

use neuron_core::vfs::FileSystem;

use super::GR_OK;
use super::batcher::{DrawArgs, SubmitContext};

/// A material in [`Materials`]: a slot index and the generation of the
/// slot's current occupant.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct MaterialId {
    index: u32,
    generation: u32,
}

impl MaterialId {
    /// The slot index. Slots are taken in creation order and reused after
    /// a material is freed.
    #[must_use]
    pub fn index(self) -> u32 {
        self.index
    }
}

/// The `IMaterial` and `IBatchedMaterial` slots the library and the batcher
/// call. `S` is what the materials draw and load with (the device, the
/// texture library, the global records), handed to every call that may
/// need it; the reference's materials hold those as interface pointers.
///
/// Results are `GENRESULT` values: 0 or above succeeds.
pub trait Material<S: ?Sized> {
    /// `IMaterial::Initialize(Outer)`.
    fn initialize(&mut self, services: &mut S) -> i32;

    /// `IMaterial::SetName`.
    fn set_name(&mut self, name: &[u8]) -> i32;

    /// `IMaterial::LoadFromFileSystem`, with the material's node current.
    fn load(&mut self, services: &mut S, fs: &mut dyn FileSystem) -> i32;

    /// `IMaterial::ApplyVerify` (both arguments are null at every call
    /// site).
    fn apply_verify(&mut self, services: &mut S) -> i32;

    /// `IMaterial::ResolveTextures`.
    fn resolve_textures(&mut self, services: &mut S, delta: f32) -> i32;

    /// `IMaterial::GetRenderFlags` as the batcher calls it on a queued
    /// material. A batched material queues its inner material, whose flags
    /// these are.
    fn render_flags(&mut self, services: &mut S) -> u32;

    /// `QueryInterface(IID_IBatchedMaterial)` succeeds: the material has
    /// the batched face (`BatchedMaterial`, and the inner materials it
    /// aggregates).
    fn is_batched(&self) -> bool {
        false
    }

    /// `IBatchedMaterial::CreateInnerMaterial`: the material of type
    /// `type_name` a batched material draws with, made with `factory`.
    fn create_inner_material(
        &mut self,
        services: &mut S,
        factory: &mut dyn MaterialFactory<S>,
        type_name: &[u8],
    ) -> i32 {
        let _ = (services, factory, type_name);
        super::GR_INTERFACE_UNSUPPORTED
    }

    /// `IMaterial::Submit`: draws now.
    fn submit(&mut self, services: &mut S, context: &SubmitContext<'_>, args: &DrawArgs) -> i32;

    /// `IBatchedMaterial::Bind`.
    fn batch_bind(&mut self, services: &mut S, context: &SubmitContext<'_>) -> i32 {
        let _ = (services, context);
        super::GR_GENERIC
    }

    /// `IBatchedMaterial::Teardown`.
    fn batch_teardown(&mut self, services: &mut S, context: &SubmitContext<'_>) -> i32 {
        let _ = (services, context);
        super::GR_GENERIC
    }

    /// `IBatchedMaterial::Submit`.
    fn batch_submit(
        &mut self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let _ = (services, context, args);
        super::GR_GENERIC
    }

    /// What the reference's destructor does beyond freeing memory
    /// (releasing texture references), called by the last release.
    fn destroy(&mut self, services: &mut S) {
        let _ = services;
    }
}

/// Makes materials by type name (`ICOManager::CreateInstance` with a
/// `FMaterialAggregateDesc` naming the type): the factories `Shading.cpp`
/// and `surfacematerials` register. An error is the `GENRESULT` the
/// manager returns, also for a component that is not a material.
pub trait MaterialFactory<S: ?Sized> {
    /// A new material of type `type_name` (the exact name).
    fn create(&mut self, services: &mut S, type_name: &[u8]) -> Result<Box<dyn Material<S>>, i32>;
}

struct Entry<S: ?Sized> {
    refs: u32,
    object: Box<dyn Material<S>>,
}

struct Slot<S: ?Sized> {
    generation: u32,
    entry: Option<Entry<S>>,
}

/// The material arena.
pub struct Materials<S: ?Sized> {
    slots: Vec<Slot<S>>,
    free: Vec<u32>,
}

impl<S: ?Sized> Default for Materials<S> {
    fn default() -> Self {
        Self::new()
    }
}

impl<S: ?Sized> std::fmt::Debug for Materials<S> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Materials")
            .field("live", &self.live().count())
            .finish()
    }
}

impl<S: ?Sized> Materials<S> {
    /// An empty arena.
    #[must_use]
    pub fn new() -> Materials<S> {
        Materials {
            slots: Vec::new(),
            free: Vec::new(),
        }
    }

    /// Takes `object` with `refs` references (the reference's object
    /// starts with one, held by whoever created it).
    pub fn insert(&mut self, object: Box<dyn Material<S>>, refs: u32) -> MaterialId {
        let entry = Some(Entry { refs, object });
        if let Some(index) = self.free.pop() {
            let slot = &mut self.slots[index as usize];
            slot.entry = entry;
            return MaterialId {
                index,
                generation: slot.generation,
            };
        }
        let index = self.slots.len() as u32;
        self.slots.push(Slot {
            generation: 0,
            entry,
        });
        MaterialId {
            index,
            generation: 0,
        }
    }

    fn entry(&self, id: MaterialId) -> Option<&Entry<S>> {
        self.slots
            .get(id.index as usize)
            .filter(|s| s.generation == id.generation)
            .and_then(|s| s.entry.as_ref())
    }

    fn entry_mut(&mut self, id: MaterialId) -> Option<&mut Entry<S>> {
        self.slots
            .get_mut(id.index as usize)
            .filter(|s| s.generation == id.generation)
            .and_then(|s| s.entry.as_mut())
    }

    /// Whether `id` names a live material.
    #[must_use]
    pub fn contains(&self, id: MaterialId) -> bool {
        self.entry(id).is_some()
    }

    /// The reference count of a live material.
    #[must_use]
    pub fn refs(&self, id: MaterialId) -> Option<u32> {
        self.entry(id).map(|e| e.refs)
    }

    /// The material.
    #[must_use]
    pub fn get(&self, id: MaterialId) -> Option<&dyn Material<S>> {
        self.entry(id).map(|e| &*e.object)
    }

    /// The material, mutably.
    pub fn get_mut(&mut self, id: MaterialId) -> Option<&mut (dyn Material<S> + 'static)> {
        self.entry_mut(id).map(|e| &mut *e.object)
    }

    /// `AddRef`; the new count, 0 for a freed handle.
    pub fn add_ref(&mut self, id: MaterialId) -> u32 {
        match self.entry_mut(id) {
            Some(entry) => {
                entry.refs = entry.refs.wrapping_add(1);
                entry.refs
            }
            None => 0,
        }
    }

    /// `Release`: the count drops (it stays at 0 if it is there), and at 0
    /// the material is destroyed and its slot freed. The new count, 0 for a
    /// freed handle.
    pub fn release(&mut self, id: MaterialId, services: &mut S) -> u32 {
        let Some(entry) = self.entry_mut(id) else {
            return 0;
        };
        entry.refs = entry.refs.saturating_sub(1);
        if entry.refs != 0 {
            return entry.refs;
        }
        let slot = &mut self.slots[id.index as usize];
        if let Some(mut entry) = slot.entry.take() {
            entry.object.destroy(services);
        }
        slot.generation = slot.generation.wrapping_add(1);
        self.free.push(id.index);
        0
    }

    /// The live materials in slot order.
    pub fn live(&self) -> impl Iterator<Item = MaterialId> + '_ {
        self.slots.iter().enumerate().filter_map(|(index, slot)| {
            slot.entry.as_ref().map(|_| MaterialId {
                index: index as u32,
                generation: slot.generation,
            })
        })
    }
}

/// Whether a `GENRESULT` succeeds (0 or above).
#[must_use]
pub fn succeeded(result: i32) -> bool {
    result >= GR_OK
}
