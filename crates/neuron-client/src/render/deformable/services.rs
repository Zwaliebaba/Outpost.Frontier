//! What `Deformable2` calls outside itself, as [`DeformServices`]: the
//! engine (`IEngine`), the hardpoint component (`IHardpoint`), the
//! animation system (`IAnimation2`, `IChannel`, `IChannelArchetype`,
//! `IRootData`), the renderer (`IRenderer2::RenderInstance`), the pipeline
//! (`IRenderPipeline`), the vertex and index buffers (`IRPVertexBuffer`,
//! `IRPIndexBuffer`), the material library and the materials, the VMesh
//! part a VMesh-backed mesh draws through, the C run time's `rand` and
//! `FDUMP`. The camera is [`MeshCamera`](crate::render::mesh::MeshCamera).
//!
//! The reference reaches these through the module globals `Initialize`
//! fills (`G_Engine`, `G_Animation`, `G_Channel`, `G_Renderer`,
//! `G_Pipeline`, `G_VertexBuffer`, `G_IndexBuffer`, `G_MaterialLibrary`);
//! here every call receives the services (divergence D390). Each method
//! stands for one call in the reference's argument order. The math engine
//! (`I3DMathEngine`) is `neuron_core::math`, called directly.
//!
//! The calls fall in two halves, so that the client can answer them from
//! two places: [`DeformHost`] is the engine, the hardpoint component, the
//! animation system, the channels, the VMesh part the engine hands out
//! and `rand` (the scene runtime of P1.8 and the caller's state, other
//! crates' pieces); [`DeformRender`] is the renderer, the pipeline, the
//! buffers, the material library, the materials and `FDUMP`, which
//! [`crate::render::stack::StackDeform`] answers on the device model.
//! [`DeformServices`] is both, implemented for every type that has both.
//!
//! The traits are object safe: the bone link of `CreateInstance` hands the
//! engine half back to the holder ([`BoneLink`]).

use neuron_core::vfs::FileSystem;

use crate::render::material::{DrawArgs, MaterialId, SubmitContext, VertexSource};

use super::{EngineHandle, Instance, UvBinding};

/// The bone link the reference registers around `CreateInstance` of a
/// skeleton (`RegisterBoneLinkCallback`): the engine calls it for every
/// instance it makes (`IEngineComponent::CreateInstance` of the
/// Deformable component, `EngineCallbackTrampoline`).
pub trait BoneLink {
    /// One call: the new instance and its archetype. The result is the
    /// component's.
    fn link(
        &mut self,
        services: &mut dyn DeformHost,
        instance: Instance,
        archetype: EngineHandle,
    ) -> bool;
}

/// A channel archetype's sample (`IChannelArchetype::Sample`): the two
/// keys' words and the fraction between them.
#[derive(Clone, Debug, PartialEq)]
pub struct ChannelSample {
    /// The words at the first key (`OutA`).
    pub a: Vec<f32>,
    /// The words at the second key (`OutB`).
    pub b: Vec<f32>,
    /// `Alpha`.
    pub alpha: f32,
}

/// The engine half of the component's calls: the engine, the hardpoint
/// component, the animation system, the channels, the VMesh part and the C
/// run time's generator.
pub trait DeformHost {
    // --- IEngine -------------------------------------------------------------------------------

    /// `CreateArchetypeHandle({name, crc}, fs)`.
    fn create_archetype(&mut self, name: &[u8], fs: &mut dyn FileSystem) -> EngineHandle;
    /// `LoadSkeleton`'s tail: `GetSearchPath(&saved)`, `SetFilesystem(fs)`,
    /// `CreateArchetypeHandle({name, crc}, fs)`, `SetFilesystem(saved)`,
    /// `saved->Release()`.
    fn create_skeleton_archetype(&mut self, name: &[u8], fs: &mut dyn FileSystem) -> EngineHandle;
    /// `ReleaseHandle(handle)`.
    fn release_handle(&mut self, handle: EngineHandle);
    /// `QueryEngineComponent(handle, IID_IVMeshLibrary, &out)`: the VMesh
    /// part with a reference, or the failing result.
    ///
    /// # Errors
    ///
    /// The engine's result.
    fn query_vmesh_part(&mut self, handle: EngineHandle) -> Result<u32, i32>;
    /// `IVMeshPart::AddRef`.
    fn add_ref_vmesh_part(&mut self, part: u32);
    /// `IVMeshPart::Release`.
    fn release_vmesh_part(&mut self, part: u32);
    /// `IVMeshPart::Draw(camera, instance, 0, null, null)`.
    fn draw_vmesh_part(&mut self, part: u32, instance: Instance);
    /// `CreateInstance(handle, null)`, with the bone link registered when
    /// `link` is given.
    fn create_instance(
        &mut self,
        handle: EngineHandle,
        link: Option<&mut dyn BoneLink>,
    ) -> Instance;
    /// `DestroyInstance(instance)`.
    fn destroy_instance(&mut self, instance: Instance);
    /// `SetInstanceTransform(instance, &BoneSlots[bone])`: the bone runtime
    /// `bone` of the holder becomes the instance's transform. The engine
    /// calls `Init(instance)` on it and copies the old transform in
    /// (`SetMatrix` with the old `GetMatrix`, then the extra and the user
    /// word, which a bone runtime ignores): the matrix copied, `None` when
    /// the engine attaches nothing (a -1 instance).
    fn set_instance_transform(&mut self, instance: Instance, bone: usize) -> Option<[f32; 12]>;
    /// `GetInstanceParent`: -1 for none.
    fn instance_parent(&mut self, instance: Instance) -> Instance;
    /// `GetInstanceRelative`: the 23 words of the joint to the parent.
    fn instance_relative(&mut self, instance: Instance) -> [u32; 23];
    /// `GetInstancePosition`: the three floats it points to.
    fn instance_position(&mut self, instance: Instance) -> [f32; 3];
    /// `GetInstanceOrientation`: the nine floats it points to.
    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9];
    /// `SetInstancePosition`.
    fn set_instance_position(&mut self, instance: Instance, position: &[f32; 3]);
    /// `SetInstanceOrientation`.
    fn set_instance_orientation(&mut self, instance: Instance, rows: &[f32; 9]);
    /// `GetJointParams(instance, index, out)`: the floats written (four
    /// for most joints, seven for a loose one, none for no joint).
    fn joint_params(&mut self, instance: Instance, index: u32) -> Vec<f32>;

    // --- IHardpoint ----------------------------------------------------------------------------

    /// `QueryInterface(IID_IHardpoint)` on the engine succeeds.
    fn has_hardpoints(&mut self) -> bool;
    /// `EnumerateHardpoints(callback, archetype, ctx)`: the names the
    /// callback receives, in order.
    fn hardpoint_names(&mut self, archetype: EngineHandle) -> Vec<Vec<u8>>;
    /// `ConnectHardpoints(a, {name, crc}, b, {name, crc})`.
    fn connect_hardpoints(&mut self, a: Instance, b: Instance, name: &[u8]) -> u32;

    // --- IAnimation2, IRootData ----------------------------------------------------------------

    /// `PlayScript(script, root, name, sink, script2)`; `sink` is the
    /// instance's own sink (`IChannelSink*` of the instance) and `script`
    /// the script argument or the instance's.
    fn play_script(&mut self, name: u32, root: Instance, extra: u32, script: u32) -> i32;
    /// `DestroyScript(handle)`.
    fn destroy_script(&mut self, handle: i32);
    /// `FilterObjects(handle)`.
    fn filter_objects(&mut self, handle: i32) -> u32;
    /// `QueryInterface(IID_IRootData)` and `Eval(handle, vec, flags, time,
    /// scale, d, f, 1.0)`: `None` when the query fails, else the result
    /// and the vector as the call leaves it.
    #[allow(clippy::too_many_arguments)]
    fn root_eval(
        &mut self,
        handle: i32,
        vec: [f32; 3],
        flags: u32,
        time: f32,
        scale: f32,
        d: u32,
        f: u32,
    ) -> Option<(u8, [f32; 3])>;
    /// `GetNamedDurationOut(name, extra, &out)`: the value when non-zero.
    fn named_duration(&mut self, name: u32, extra: u32) -> Option<f32>;
    /// `EvalPlayback(handle, flags, time, scale, d, f, 1.0f, xform)`.
    #[allow(clippy::too_many_arguments)]
    fn eval_playback(
        &mut self,
        handle: i32,
        flags: u32,
        time: f32,
        scale: f32,
        d: u32,
        f: u32,
        xform: &[f32; 12],
    ) -> i32;
    /// `HasObject(handle, key)`.
    fn has_object(&mut self, handle: i32, key: u32) -> u32;
    /// `GetFirstInstanceField(handle, &out)`: the word written (`None` for
    /// none).
    fn first_instance_field(&mut self, handle: i32) -> Option<u32>;
    /// `GetDurationIfNamed(handle, &out)`: the value when non-zero.
    fn duration_if_named(&mut self, handle: i32) -> Option<f32>;
    /// `TouchMapped(handle, pos, extra)` with both zeroed: the two as the
    /// call leaves them.
    fn touch_mapped(&mut self, handle: i32) -> ([f32; 3], [f32; 3]);
    /// `SetOverlayPos(handle, pos)`.
    fn set_overlay_pos(&mut self, handle: i32, pos: &[f32; 3]);
    /// `SetOverlayExtra(handle, extra)` (nine words).
    fn set_overlay_extra(&mut self, handle: i32, extra: &[f32; 9]);
    /// `AddOverlayPos(handle, delta)`.
    fn add_overlay_pos(&mut self, handle: i32, delta: &[f32; 3]);
    /// `FindObject(handle, {kind, 0, 0, instance})`.
    fn find_object(&mut self, handle: i32, kind: u32, instance: Instance) -> u32;
    /// `RegisterHandle(key)`.
    fn register_handle(&mut self, key: i32);
    /// `HasHandle(key, &out)`: the byte written.
    fn has_handle(&mut self, key: i32) -> u8;
    /// `HasAnyHandle(key)`.
    fn has_any_handle(&mut self, key: i32);

    // --- IChannel, IChannelArchetype -----------------------------------------------------------

    /// `CreateTyped(owner, 4, {0, 0, 0, joint}, bind)`: the channel key.
    fn create_typed_channel(&mut self, owner: u32, joint: Instance, bind: Instance) -> u32;
    /// `Play(key, blob)` with the seven words of the play header.
    fn play_channel(&mut self, key: u32, blob: &[u32; 7]);
    /// `Destroy(key)`.
    fn destroy_channel(&mut self, key: u32);
    /// `InMixer(key)`: the low byte.
    fn channel_in_mixer(&mut self, key: u32) -> u8;
    /// `GetObject(key)`, `GetFlags`, `Sample(time)` and `Release` on the
    /// archetype.
    fn sample_channel(&mut self, key: u32, time: f32) -> ChannelSample;

    // --- the C run time --------------------------------------------------------------------------

    /// `rand()`.
    fn rand(&mut self) -> i32;
}

/// The render half: the renderer, the pipeline, the vertex and index
/// buffers, the material library and its materials, and `FDUMP`.
pub trait DeformRender {
    // --- IRenderer2 ----------------------------------------------------------------------------

    /// `RenderInstance(camera, instance, 1.0, flags, null, arg)`: the rank.
    fn render_instance(&mut self, instance: Instance, flags: u32, arg: u32) -> i32;

    // --- IRenderPipeline -----------------------------------------------------------------------

    /// `GetPipelineState(id, &value)`: the value written, `None` when the
    /// call writes nothing.
    fn pipeline_state(&mut self, id: u32) -> Option<i32>;
    /// `SetModelview`.
    fn set_modelview(&mut self, matrix: &[f32; 12]);
    /// `SetWorld`.
    fn set_world(&mut self, matrix: &[f32; 12]);
    /// `SetWorldN(index, matrix)`.
    fn set_world_n(&mut self, index: u32, matrix: &[f32; 12]);
    /// `GetRenderState(state, &old)`: the value written (0 for none).
    fn render_state(&mut self, state: u32) -> i32;
    /// `SetRenderState`.
    fn set_render_state(&mut self, state: u32, value: i32);

    // --- IRPVertexBuffer -----------------------------------------------------------------------

    /// `IsVbValid(handle)`.
    fn is_vb_valid(&mut self, vertex_buffer: u32) -> i32;
    /// `SelectVb(handle)`.
    fn select_vb(&mut self, vertex_buffer: u32) -> i32;
    /// `CreateVb(fvf, count, &handle, 0)`.
    ///
    /// # Errors
    ///
    /// The result; the handle stays as it was.
    fn create_vb(&mut self, fvf: u32, count: u32) -> Result<u32, i32>;
    /// `LockVb(handle, &offset, &bits, count)`: the offset and the locked
    /// bytes as they are.
    ///
    /// # Errors
    ///
    /// The result.
    fn lock_vb(&mut self, vertex_buffer: u32, count: u32) -> Result<(u32, Vec<u8>), i32>;
    /// `CopyVertices(handle, &base, source, start, count)` into the locked
    /// bytes.
    fn copy_vertices(
        &mut self,
        vertex_buffer: u32,
        base: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
        locked: &mut [u8],
    ) -> i32;
    /// `UnlockVb(handle)` with the bytes written into the lock.
    fn unlock_vb(&mut self, vertex_buffer: u32, locked: Vec<u8>);
    /// `DestroyVb(&handle)`; the handle is 0 afterwards.
    fn destroy_vb(&mut self, vertex_buffer: u32);

    // --- IRPIndexBuffer ------------------------------------------------------------------------

    /// `IsIbValid(handle)`.
    fn is_ib_valid(&mut self, index_buffer: u32) -> i32;
    /// `SelectIb(handle, base_vertex, 0, 0)`.
    fn select_ib(&mut self, index_buffer: u32, base_vertex: u32) -> i32;
    /// `CreateIb(count, &handle, 0)`.
    ///
    /// # Errors
    ///
    /// The result; the handle stays as it was.
    fn create_ib(&mut self, count: u32) -> Result<u32, i32>;
    /// `LockIb(handle, &offset, &bits, count)`: the offset.
    ///
    /// # Errors
    ///
    /// The result.
    fn lock_ib(&mut self, index_buffer: u32, count: u32) -> Result<u32, i32>;
    /// `UnlockIb(handle)` with the indices written into the lock.
    fn unlock_ib(&mut self, index_buffer: u32, written: &[u16]);
    /// `DestroyIb(&handle)`; the handle is 0 afterwards.
    fn destroy_ib(&mut self, index_buffer: u32);

    // --- IMaterialLibrary and IMaterial --------------------------------------------------------

    /// `FindMaterialByCrc({name, crc}, &out)`: the result and the material
    /// written, with a reference for the caller.
    fn find_material(&mut self, name: &[u8], crc: u32) -> (i32, Option<MaterialId>);
    /// `Release`.
    fn release_material(&mut self, material: MaterialId);
    /// `ClassName()` (slot 20).
    fn material_class_name(&mut self, material: MaterialId) -> Vec<u8>;
    /// `Bind(context)`; `binding` is the UV-deform triple the context's
    /// `Field20` points to (`None` for null).
    fn bind(
        &mut self,
        material: MaterialId,
        context: &SubmitContext<'_>,
        binding: Option<&mut UvBinding>,
    );
    /// `Submit(context, prim, start, count, ib_start, index_span, flags)`.
    fn submit(&mut self, material: MaterialId, context: &SubmitContext<'_>, args: &DrawArgs);
    /// `Teardown(context)`.
    fn teardown(&mut self, material: MaterialId, context: &SubmitContext<'_>);

    // --- diagnostics -----------------------------------------------------------------------------

    /// `FDUMP(flags, "%s(%d) : <severity>:General:%s", file, line, message)`.
    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str);
}

/// The calls of the Deformable component: both halves.
pub trait DeformServices: DeformHost + DeformRender {}

impl<T: DeformHost + DeformRender + ?Sized> DeformServices for T {}

/// The calls an aim event makes from inside its typed channels' tick
/// (`FIkEvent::Apply`, `IkApply` and the solvers): the engine's reads of
/// the chain's joints and `FDUMP`. The animation system calls the owner
/// while it walks the mixer, when only the engine it ticks on is at hand;
/// every [`DeformServices`] answers them too.
pub trait AimHost {
    /// `GetInstanceParent`: -1 for none.
    fn instance_parent(&mut self, instance: Instance) -> Instance;
    /// `GetInstanceRelative`: the 23 words of the joint to the parent.
    fn instance_relative(&mut self, instance: Instance) -> [u32; 23];
    /// `GetInstancePosition`.
    fn instance_position(&mut self, instance: Instance) -> [f32; 3];
    /// `GetInstanceOrientation`.
    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9];
    /// `GetJointParams(instance, index, out)`.
    fn joint_params(&mut self, instance: Instance, index: u32) -> Vec<f32>;
    /// `FDUMP`.
    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str);
}

impl<T: DeformHost + DeformRender + ?Sized> AimHost for T {
    fn instance_parent(&mut self, instance: Instance) -> Instance {
        DeformHost::instance_parent(self, instance)
    }

    fn instance_relative(&mut self, instance: Instance) -> [u32; 23] {
        DeformHost::instance_relative(self, instance)
    }

    fn instance_position(&mut self, instance: Instance) -> [f32; 3] {
        DeformHost::instance_position(self, instance)
    }

    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9] {
        DeformHost::instance_orientation(self, instance)
    }

    fn joint_params(&mut self, instance: Instance, index: u32) -> Vec<f32> {
        DeformHost::joint_params(self, instance, index)
    }

    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str) {
        DeformRender::dump(self, flags, file, line, message);
    }
}

/// The calls the instance's channel sink makes (`OnKeys`, `OnLoop`): the
/// animation system's queries on its playbacks, which the animation
/// system answers from inside its mixer walk; every [`DeformServices`]
/// answers them too.
pub trait SinkHost {
    /// `HasObject(handle, key)`.
    fn has_object(&mut self, handle: i32, key: u32) -> u32;
    /// `GetFirstInstanceField(handle, &out)`.
    fn first_instance_field(&mut self, handle: i32) -> Option<u32>;
    /// `GetDurationIfNamed(handle, &out)`.
    fn duration_if_named(&mut self, handle: i32) -> Option<f32>;
}

impl<T: DeformHost + ?Sized> SinkHost for T {
    fn has_object(&mut self, handle: i32, key: u32) -> u32 {
        DeformHost::has_object(self, handle, key)
    }

    fn first_instance_field(&mut self, handle: i32) -> Option<u32> {
        DeformHost::first_instance_field(self, handle)
    }

    fn duration_if_named(&mut self, handle: i32) -> Option<f32> {
        DeformHost::duration_if_named(self, handle)
    }
}
