//! What the surface materials call beyond [`MaterialServices`]: three more
//! renderer and texture-library calls, the engine `FLAppearance` publishes
//! (`G_FlmEngineOuter`: instance user data, roots, the mesh of an
//! instance), the instance values `FLAppearance` keeps (`G_IdValues`), the
//! visible-geometry counters, and the material a submission replaces
//! (`FMaterialSubmitContext::ReplacedMaterial`, the "candidate" the
//! override materials draw with).
//!
//! As in [`MaterialServices`], each method stands for one call of the
//! reference, in its order and with its arguments.
//!
//! The reference reaches the candidate through the context's interface
//! pointer; here the context names it by [`MaterialId`] and the services
//! make the call, since a material cannot reach the arena it lives in
//! (divergence D315). The candidate calls are made only for a context
//! whose `replaced` is set.

use super::super::batcher::{DrawArgs, SubmitContext};
use super::super::instance::MaterialId;
use super::super::services::MaterialServices;
use crate::render::texture::FrameOut;

/// `GetFaceGroupIndices`: the indices and the count the call writes.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MeshIndices {
    /// The index array (`None` for a null pointer).
    pub indices: Option<Vec<u16>>,
    /// The count output.
    pub count: u32,
}

/// The calls of the surface materials that [`MaterialServices`] does not
/// have.
pub trait SurfaceServices: MaterialServices {
    // --- IRenderPipeline ---------------------------------------------------------------------

    /// `GetMaterial`: the 17 words written, `None` when the call fails and
    /// writes nothing.
    fn material(&mut self) -> Option<[u32; 17]>;

    /// `GetTextureStageState`: the value written, `None` when the call
    /// fails and writes nothing.
    fn stage_state(&mut self, stage: u32, state: u32) -> Option<u32>;

    /// `DrawPrimitiveVb(prim, vb, start, count, null)`.
    fn draw_vb(&mut self, prim: u32, vertex_buffer: u32, start: u32, count: u32) -> i32;

    // --- ITextureLibrary2 ----------------------------------------------------------------------

    /// `GetTextureFrame(record, frame, out)`: the five words, `None` when
    /// the call fails.
    fn record_frame(&mut self, record: Self::TextureRecord, frame: i32) -> Option<FrameOut>;

    // --- The engine (`G_FlmEngineOuter`) -------------------------------------------------------

    /// `G_FlmEngineOuter` is set (`FLAppearance` initialized). No call.
    fn has_engine(&mut self) -> bool;

    /// `IEngine::GetInstanceUserData(instance)`: the first five words the
    /// pointer addresses, `None` for a null pointer.
    fn instance_user_data(&mut self, instance: usize) -> Option<[u32; 5]>;

    /// `IEngine::GetInstanceRoot(instance)`.
    fn instance_root(&mut self, instance: usize) -> usize;

    /// `IEngine::GetInstanceRelative(instance)` (the result is unused).
    fn instance_relative(&mut self, instance: usize);

    /// `IEngine::QueryInstance(instance, IID_IVMeshWire)` succeeds.
    fn query_wire(&mut self, instance: usize) -> bool;

    /// `IVMeshWire::Draw` on the instance's wire.
    fn wire_draw(&mut self, instance: usize);

    /// `IEngine::QueryInstance(instance, IID_IMesh)` succeeds.
    fn query_mesh(&mut self, instance: usize) -> bool;

    /// `IMesh::OpenFaceGroup(index, mode)`.
    fn mesh_open_face_group(&mut self, instance: usize, index: i32, mode: i32) -> i32;

    /// `IMesh::CloseFaceGroup`.
    fn mesh_close_face_group(&mut self, instance: usize);

    /// `IMesh::OpenFaceGroupChannels(flags)`.
    fn mesh_open_channels(&mut self, instance: usize, flags: u32) -> i32;

    /// `IMesh::LockFaceGroupChannels(mask)`.
    fn mesh_lock_channels(&mut self, instance: usize, mask: u32);

    /// `IMesh::UnlockFaceGroupChannels`.
    fn mesh_unlock_channels(&mut self, instance: usize);

    /// `IMesh::CloseFaceGroupChannels`.
    fn mesh_close_channels(&mut self, instance: usize);

    /// `IMesh::GetFaceGroupData(channel, &data, ...)`: the words of the
    /// channel's array (`None` for a null pointer).
    fn mesh_data(&mut self, instance: usize, channel: u32) -> Option<Vec<u32>>;

    /// `IMesh::GetFaceGroupIndices(channel, &indices, &count)`.
    fn mesh_indices(&mut self, instance: usize, channel: u32) -> MeshIndices;

    // --- FLAppearance's instance values --------------------------------------------------------

    /// `FlmLookupIdValue(id)`: the value stored for `id`, `None` for id -1
    /// or a miss.
    fn instance_value(&mut self, id: i32) -> Option<u32>;

    /// The string the value stored for `id` points to (`EnvMapMaterial`
    /// reads the value as a texture name): `None` for a miss or a null
    /// value.
    fn instance_value_name(&mut self, id: i32) -> Option<Vec<u8>>;

    /// `VisibleCounterMaterial::Submit`: `G_VisibleCounterA` gains 1, `B`
    /// the index span and `C` the vertex count.
    fn count_visible(&mut self, index_span: u32, count: u32);

    // --- The candidate (`ReplacedMaterial`) ----------------------------------------------------

    /// `QueryInterface(IID_IBatchedMaterial)` on the candidate succeeds.
    fn candidate_is_batched(&mut self, candidate: MaterialId) -> bool;

    /// `Bind(context)` on the candidate's batched face (`batched`) or its
    /// material face.
    fn candidate_bind(
        &mut self,
        candidate: MaterialId,
        batched: bool,
        context: &SubmitContext<'_>,
    ) -> i32;

    /// `Teardown(context)` on one of the candidate's faces.
    fn candidate_teardown(
        &mut self,
        candidate: MaterialId,
        batched: bool,
        context: &SubmitContext<'_>,
    ) -> i32;

    /// `Submit(context, ...)` on one of the candidate's faces.
    fn candidate_submit(
        &mut self,
        candidate: MaterialId,
        batched: bool,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32;

    /// `IMaterial::GetPassCount`: the count written, `None` when nothing is
    /// written.
    fn candidate_pass_count(&mut self, candidate: MaterialId) -> Option<i32>;

    /// `IMaterial::ClassName`.
    fn candidate_class_name(&mut self, candidate: MaterialId) -> Vec<u8>;

    /// `IMaterial::GetParameterDescriptor(index)`: the record's leaf name
    /// (`None` for a null one) when the call returns `GR_OK`, else the
    /// result.
    ///
    /// # Errors
    ///
    /// The candidate's result.
    fn candidate_parameter_tag(
        &mut self,
        candidate: MaterialId,
        index: u32,
    ) -> Result<Option<Vec<u8>>, i32>;

    /// `IMaterial::GetParameterValue(index, value)` with a float value: the
    /// first payload word afterwards (0 when the call writes nothing).
    fn candidate_parameter_word(&mut self, candidate: MaterialId, index: u32) -> u32;
}

/// The engine for [`super::super::Shading`]'s [`SurfaceServices`]: what
/// `FLAppearance` publishes as `G_FlmEngineOuter`. Every method answers as
/// an engine that is not there. The instance values are
/// [`super::super::Shading::values`].
pub trait SurfaceEngine {
    /// `G_FlmEngineOuter` is set.
    fn present(&self) -> bool {
        false
    }

    /// `GetInstanceUserData`.
    fn user_data(&mut self, instance: usize) -> Option<[u32; 5]> {
        let _ = instance;
        None
    }

    /// `GetInstanceRoot`.
    fn root(&mut self, instance: usize) -> usize {
        instance
    }
}

/// A value `IFLAppearance::SetInstanceValue` stores (`void*`): the game
/// stores an opacity's bits for the cloak and colourize materials and a
/// texture name for `EnvMapMaterial`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum InstanceValue {
    /// A word stored as the pointer (an opacity's bits, or null for 0).
    Word(u32),
    /// A pointer to a NUL-terminated name.
    Name(Vec<u8>),
}

/// `G_IdValues`, the instance values `FLAppearance` keeps by instance id
/// and the surface materials read (`FlmLookupIdValue`). Id -1 is refused.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct InstanceValues {
    values: std::collections::BTreeMap<i32, InstanceValue>,
}

impl InstanceValues {
    /// No values.
    #[must_use]
    pub fn new() -> InstanceValues {
        InstanceValues::default()
    }

    /// `SetInstanceValue`: stores or replaces the value; -1 for id -1.
    pub fn set(&mut self, id: i32, value: InstanceValue) -> i32 {
        if id == -1 {
            return -1;
        }
        self.values.insert(id, value);
        0
    }

    /// `GetInstanceValue` and `FlmLookupIdValue`: the value stored, `None`
    /// for id -1 or a miss.
    #[must_use]
    pub fn get(&self, id: i32) -> Option<&InstanceValue> {
        if id == -1 {
            return None;
        }
        self.values.get(&id)
    }

    /// `RemoveInstanceValue`: -1 for id -1, else 0 whether or not a value
    /// was stored.
    pub fn remove(&mut self, id: i32) -> i32 {
        if id == -1 {
            return -1;
        }
        self.values.remove(&id);
        0
    }

    /// The value as the word a material reads (`CloakMaterial`,
    /// `ColorizeMaterial`): `None` for a miss or a name, whose pointer the
    /// reference would read (divergence D320).
    #[must_use]
    pub fn word(&self, id: i32) -> Option<u32> {
        match self.get(id)? {
            InstanceValue::Word(word) => Some(*word),
            InstanceValue::Name(_) => None,
        }
    }

    /// The name the value points to (`EnvMapMaterial`): `None` for a miss,
    /// a null word or another word, which the reference would read as a
    /// pointer (divergence D320).
    #[must_use]
    pub fn name(&self, id: i32) -> Option<Vec<u8>> {
        match self.get(id)? {
            InstanceValue::Name(name) => Some(name.clone()),
            InstanceValue::Word(_) => None,
        }
    }

    /// The number of values stored.
    #[must_use]
    pub fn len(&self) -> usize {
        self.values.len()
    }

    /// No value is stored.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }
}
