//! What `FLAppearance` calls: the interfaces it acquires (the engine it is
//! aggregated into, the system's material library, pipeline, vertex-buffer
//! manager and texture library, the engine's renderer), the material
//! library's calls for the override materials, the pipeline's for the
//! backdrop, starsphere and shadow draws, the texture library's for
//! `efxshadow`, the vertex memory, the engine's instances, the renderer's
//! `GetBounds` and `RenderInstance`, the visible-geometry counters and
//! `FDUMP`, as [`AppearanceServices`]: one method per call, in the
//! reference's order and with its arguments, as the oracle suite's
//! recording contract (`CppSource/Oracle/oracle_material_appearance.cpp`)
//! states.
//!
//! The reference holds these as interface pointers in the appearance and
//! publishes the engine to the surface materials through
//! `G_FlmEngineOuter`; here the services make the calls and own the
//! instance values the materials read (divergence D318). `RenderInstance`
//! is the render manager's ([`crate::render::mesh::RenderManager`], with
//! the camera seen through [`super::MeshView`]), `GetBounds` its slot 4;
//! the services that join the render manager, the material library and
//! the engine come with the engine's instances (P4.6).

use crate::render::material::{InstanceValues, MaterialId, VertexLock};
use crate::render::mesh::{Archetype, Instance};
use crate::render::texture::FrameOut;

use super::camera::Camera;

/// An interface `FLAppearance` acquires.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Face {
    /// `IID_IEngine` from the outer component it is aggregated into.
    Engine,
    /// `IID_IMaterialLibrary` from the system.
    MaterialLibrary,
    /// `IID_IRenderPipeline` from the system.
    Pipeline,
    /// `IID_IVertexBufferManager` from the system.
    VertexBuffers,
    /// `IID_ITextureLibrary` from the system.
    Textures,
    /// `IID_IRenderer` from the engine.
    Renderer,
}

/// The calls `FLAppearance` makes.
pub trait AppearanceServices {
    /// A texture record `FindTextureByCrc` hands out with a reference.
    type TextureRecord;

    // --- components --------------------------------------------------------------------------

    /// `QueryInterface` for `face` succeeds (the reference releases the
    /// reference at once and keeps the pointer).
    fn query(&mut self, face: Face) -> bool;
    /// The aggregate descriptor names a system component. No call.
    fn has_system(&mut self) -> bool;
    /// `G_FlmEngineOuter = Engine_`: the surface materials see the engine.
    fn publish_engine(&mut self);

    // --- IMaterialLibrary --------------------------------------------------------------------

    /// `FindOrCreate(name, type_name, null, &out)`: the result and the
    /// material written (`None` for a null one or none written), with a
    /// reference for the caller.
    fn find_or_create(&mut self, name: &str, type_name: &str) -> (i32, Option<MaterialId>);
    /// `RemoveMaterial(0, {name, crc})`.
    fn remove_material(&mut self, name: &str, crc: u32);
    /// `SetGlobalRecord(index, &value)`.
    fn set_global_record(&mut self, index: u32, value: u32);
    /// `Release` on a material.
    fn release_material(&mut self, material: MaterialId);

    // --- the instance values -----------------------------------------------------------------

    /// `G_IdValues`: the values the appearance stores and the surface
    /// materials read.
    fn instance_values(&mut self) -> &mut InstanceValues;

    // --- IRenderPipeline ---------------------------------------------------------------------

    /// `QueryDeviceAbility(index, &value)`: the value written, `None` when
    /// nothing is written.
    fn device_ability(&mut self, index: u32) -> Option<u32>;
    /// `SetPerspective(fov, aspect, near, far)`.
    fn set_perspective(&mut self, fov_y_deg: f32, aspect: f32, near: f32, far: f32);
    /// `GetRenderState(state, &value)` with the value starting at 0.
    fn render_state(&mut self, state: u32) -> u32;
    /// `SetRenderState`.
    fn set_render_state(&mut self, state: u32, value: u32);
    /// `SetModelview`.
    fn set_modelview(&mut self, matrix: &[f32; 12]);
    /// `SetTextureStageState`.
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32);
    /// `SetTextureStageTexture`.
    fn set_stage_texture(&mut self, stage: u32, handle: u32);
    /// `DrawPrimitiveVb(prim, vb, start, count, null)` (the result is
    /// unused).
    fn draw_vb(&mut self, prim: u32, vertex_buffer: u32, start: u32, count: u32);

    // --- ITextureLibrary2 --------------------------------------------------------------------

    /// `FindTextureByCrc({name, crc}, &record)`: the record on success.
    ///
    /// # Errors
    ///
    /// The library's result; nothing is written.
    fn find_texture(&mut self, name: &str, crc: u32) -> Result<Self::TextureRecord, i32>;
    /// `GetTextureFrame(record, frame, out)`: the five words, `None` when
    /// the call fails.
    fn record_frame(&mut self, record: &Self::TextureRecord, frame: i32) -> Option<FrameOut>;
    /// `ReleaseTextureRecord`.
    fn release_texture(&mut self, record: Self::TextureRecord);

    // --- IVertexBufferManager ----------------------------------------------------------------

    /// `LockVertexMemory(fvf, count, &lock)`, accepted only when it
    /// returns `GR_OK`.
    ///
    /// # Errors
    ///
    /// Any other result.
    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32>;
    /// `UnlockVertexMemory`, with the vertices written into the lock.
    fn unlock_vertex_memory(&mut self, lock: VertexLock);

    // --- IEngine -----------------------------------------------------------------------------

    /// `GetInstanceTransform(instance)`: the twelve words it points to.
    fn instance_transform(&mut self, instance: Instance) -> [f32; 12];
    /// `GetInstanceArchetype(instance)`: -1 for none.
    fn instance_archetype(&mut self, instance: Instance) -> Archetype;
    /// `ReleaseHandle(archetype)`.
    fn release_handle(&mut self, archetype: Archetype);

    // --- IRenderer2 --------------------------------------------------------------------------

    /// `GetBounds(archetype, lod, out)`: the result and the six floats
    /// written (`None` when nothing is written).
    fn bounds(&mut self, archetype: Archetype, lod: f32) -> (i32, Option<[f32; 6]>);
    /// `RenderInstance(camera, instance, threshold, flags, null, extra)`
    /// (the result is unused).
    fn render_instance(
        &mut self,
        camera: &mut dyn Camera,
        instance: Instance,
        threshold: f32,
        flags: u32,
        extra: Option<MaterialId>,
    );

    // --- the visible-geometry counters -------------------------------------------------------

    /// `FlmVisibleCounterReset`.
    fn reset_visible(&mut self);
    /// `FlmVisibleCounterCopy`: `G_VisibleCounterA` to `C`.
    fn visible(&mut self) -> [u32; 3];

    // --- diagnostics -------------------------------------------------------------------------

    /// `FDUMP(flags, ...)` with the text formatted.
    fn dump(&mut self, flags: i32, text: &str);
}
