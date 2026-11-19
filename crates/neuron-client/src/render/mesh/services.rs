//! What RendComp calls: the renderer interfaces (`IRenderPipeline`,
//! `IVertexBufferManager`, `IRPVertexBuffer`, `IRPIndexBuffer`, `IRPDraw`),
//! the engine (`IEngine`), the material library and the materials
//! (`IMaterialLibrary`, `IMaterial`), `FDUMP` and `LogStream::LogEvent`
//! as [`MeshServices`], and the camera (`ICamera`) as [`MeshCamera`].
//!
//! The reference reaches these through process globals (`G_Pipeline`,
//! `G_VertexBuffer`, `G_IndexBuffer`, `G_Draw`, `G_VbManager`,
//! `G_MaterialLibrary`, `G_Engine`) the render manager fills in
//! `Initialize`; here they are the `S` every call receives (divergence
//! D351). Each method stands for one call, in the reference's argument
//! order, so that the calls a mesh makes are the calls the reference made;
//! the oracle suite answers them with the recording contract
//! `CppSource/Oracle/oracle_mesh_render.cpp` states.

use crate::render::material::{DrawArgs, MaterialId, SubmitContext, VertexLock, VertexSource};

use super::{Archetype, Instance};

/// The calls of the render manager and the mesh kinds.
pub trait MeshServices {
    // --- IRenderPipeline ---------------------------------------------------------------------

    /// `GetPipelineState(id, &value)`: the value written, `None` when the
    /// call writes nothing (the caller's start value stays).
    fn pipeline_state(&mut self, id: u32) -> Option<i32>;
    /// `SetModelview`.
    fn set_modelview(&mut self, matrix: &[f32; 12]);
    /// `SetWorld`.
    fn set_world(&mut self, matrix: &[f32; 12]);
    /// `SetView`.
    fn set_view(&mut self, matrix: &[f32; 12]);

    // --- IVertexBufferManager ------------------------------------------------------------------

    /// `LockVertexMemory(fvf, count, &lock)`.
    ///
    /// # Errors
    ///
    /// The manager's result.
    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32>;
    /// `CopyVertexData(lock.Vertices, fvf, source, start, count)`.
    fn copy_vertex_data(
        &mut self,
        dest: &mut [u8],
        fvf: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    ) -> i32;
    /// `UnlockVertexMemory`.
    fn unlock_vertex_memory(&mut self, lock: VertexLock);

    // --- IRPVertexBuffer -----------------------------------------------------------------------

    /// `SelectVb(handle)`.
    fn select_vb(&mut self, vertex_buffer: u32) -> i32;
    /// `CreateVb(fvf, count, &handle, 0)`: the new handle.
    ///
    /// # Errors
    ///
    /// The result; the handle stays as it was (0 after `DestroyVb`).
    fn create_vb(&mut self, fvf: u32, count: u32) -> Result<u32, i32>;
    /// `LockVb(handle, &offset, &bits, count)`.
    fn lock_vb(&mut self, vertex_buffer: u32, count: u32) -> i32;
    /// `CopyVertices(handle, &base, source, start, count)` into a locked
    /// buffer: the result and the base the call leaves.
    fn copy_vertices(
        &mut self,
        vertex_buffer: u32,
        base: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    ) -> (i32, u32);
    /// `UnlockVb`.
    fn unlock_vb(&mut self, vertex_buffer: u32);
    /// `DestroyVb(&handle)`; the handle is 0 afterwards.
    fn destroy_vb(&mut self, vertex_buffer: u32);

    // --- IRPIndexBuffer ------------------------------------------------------------------------

    /// `SelectIb(handle, base_vertex, 0, 0)`.
    fn select_ib(&mut self, index_buffer: u32, base_vertex: u32) -> i32;
    /// `CreateIb(count, &handle, 0)`: the new handle.
    ///
    /// # Errors
    ///
    /// The result; the handle stays as it was.
    fn create_ib(&mut self, count: u32) -> Result<u32, i32>;
    /// `LockIb(handle, &offset, &bits, count)`.
    fn lock_ib(&mut self, index_buffer: u32, count: u32) -> i32;
    /// `UnlockIb(handle)`, with the indices written into the lock.
    fn unlock_ib(&mut self, index_buffer: u32, written: &[u16]);
    /// `DestroyIb(&handle)`; the handle is 0 afterwards.
    fn destroy_ib(&mut self, index_buffer: u32);
    /// `CopyIndices((void*)-1, &start, indices, count)` into the scratch
    /// index buffer: the start index the call leaves (0 when it writes
    /// nothing).
    fn copy_scratch_indices(&mut self, indices: &[u16]) -> u32;

    // --- IRPDraw -------------------------------------------------------------------------------

    /// `DrawIndexedPrimitive(prim, min_index, vertex_count, start_index,
    /// count)`.
    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> i32;

    // --- IEngine -------------------------------------------------------------------------------

    /// `GetInstanceTransform(instance)`: the twelve words it points to.
    fn instance_transform(&mut self, instance: Instance) -> [f32; 12];
    /// `GetInstanceArchetype(instance)`: -1 for none.
    fn instance_archetype(&mut self, instance: Instance) -> Archetype;
    /// `ReleaseHandle(archetype)`.
    fn release_handle(&mut self, archetype: Archetype);
    /// `GetInstanceExtent(instance, 1, &radius, centre)`: `None` when the
    /// call writes nothing.
    fn instance_extent(&mut self, instance: Instance) -> Option<(f32, [f32; 3])>;
    /// `SetInstanceExtent(instance, 1, radius, centre)`.
    fn set_instance_extent(&mut self, instance: Instance, radius: f32, centre: [f32; 3]);
    /// `GetRelatedInstance(instance, 1, previous)`: the next child after
    /// `previous` (`None` for -1, the first), `None` at the end.
    fn related_instance(
        &mut self,
        instance: Instance,
        previous: Option<Instance>,
    ) -> Option<Instance>;
    /// `GetInstanceRoot(instance)` (the result is unused).
    fn instance_root(&mut self, instance: Instance);

    // --- IMaterialLibrary and IMaterial --------------------------------------------------------

    /// `FindMaterialByCrc({name, crc}, &out)`: the result and the material
    /// written (`None` for a null one or none written), with a reference
    /// for the caller.
    fn find_material(&mut self, crc: u32) -> (i32, Option<MaterialId>);
    /// `AddRef`.
    fn add_ref_material(&mut self, material: MaterialId);
    /// `Release`.
    fn release_material(&mut self, material: MaterialId);
    /// `GetRenderFlags(&flags)` with the flags starting at 0.
    fn material_flags(&mut self, material: MaterialId) -> u32;
    /// `GetName(buffer, 0x40)`: the name when the call returns `GR_OK`.
    fn material_name(&mut self, material: MaterialId) -> Option<Vec<u8>>;
    /// `CopyClassName(buffer, 0x80)`: the name, to its NUL.
    fn material_class(&mut self, material: MaterialId) -> Vec<u8>;
    /// `GetParameterCount(&count)` with the count starting at 0.
    fn material_parameter_count(&mut self, material: MaterialId) -> u32;
    /// `GetParameterValue(index, &value)` with the type and the first
    /// payload word starting at 0: the two words the call leaves.
    fn material_parameter(&mut self, material: MaterialId, index: u32) -> (u32, u32);
    /// `Bind(context)`.
    fn bind(&mut self, material: MaterialId, context: &SubmitContext<'_>);
    /// `Submit(context, prim, start, count, ib_start, index_span, flags)`.
    fn submit(&mut self, material: MaterialId, context: &SubmitContext<'_>, args: &DrawArgs);
    /// `Teardown(context)`.
    fn teardown(&mut self, material: MaterialId, context: &SubmitContext<'_>);

    // --- diagnostics ---------------------------------------------------------------------------

    /// `FDUMP(flags, "%s(%d) : <severity>:General:%s", file, line, message)`.
    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str);
    /// `LogStream::LogEvent(name, value, flags)`.
    fn log_event(&mut self, name: &str, value: f32, flags: u32);
}

/// The calls on `ICamera`.
pub trait MeshCamera {
    /// `GetViewTransform`: the twelve words it points to.
    fn view_transform(&mut self) -> [f32; 12];
    /// `ClassifySphere(centre, radius)`.
    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32;
    /// `GetHalfFovXDeg` (the result is unused).
    fn half_fov_x_deg(&mut self) -> f32;
    /// `GetViewport`: the four integers it points to.
    fn viewport(&mut self) -> [i32; 4];
    /// `GetXScaleOverNear`.
    fn x_scale_over_near(&mut self) -> f64;
    /// `GetZNear`.
    fn z_near(&mut self) -> f32;
    /// `GetPosition`: the three floats it points to.
    fn position(&mut self) -> [f32; 3];
}
