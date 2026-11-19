//! The deformable component's render half on the stack (work package
//! P4.7, second increment): [`StackDeform`] answers [`DeformRender`] on
//! the device model and the material library and passes the engine half
//! ([`DeformHost`]) to the caller's engine, so that a skinned character
//! skins into a device vertex buffer, fills a device index buffer, draws
//! each face group through its material from the library's arena (the
//! `EyeMaterial` with its UV-deform binding through the stack's
//! [`EyeHost`](crate::render::deformable::EyeHost)) and hands its root
//! instance to the render manager, as `IDeformable::Draw` does.
//!
//! The vertex and index buffers are the device's (`render::ff::buffers`):
//! `CreateVb` makes a plain buffer, `LockVb` hands out the locked range's
//! bytes as they are (a Direct3D lock without discard), and `UnlockVb`
//! writes them back before it unlocks, so the corners a skin does not
//! touch keep their vertices.

use std::cell::{RefCell, RefMut};

use neuron_core::vfs::FileSystem;

use crate::render::deformable::{
    BoneLink, ChannelSample, DeformHost, DeformInstance, DeformRender, Deformable, EngineHandle,
    Instance, UvBinding,
};
use crate::render::ff::buffers::BufferError;
use crate::render::ff::device::Backend;
use crate::render::material::{
    BatchDevice, DrawArgs, MaterialId, MaterialServices, SubmitContext, VertexSource,
};
use crate::render::mesh::manager::DrawRequest;
use crate::render::mesh::{MeshCamera, MeshServices, RenderManager};

use super::{E_FAIL, RenderStack};

fn code(error: BufferError) -> i32 {
    error.code().cast_signed()
}

/// The cell the camera lives in while the instance's draw and the render
/// manager's draw of its root share it.
trait CameraCell {
    fn camera(&self) -> RefMut<'_, dyn MeshCamera + '_>;
}

impl CameraCell for RefCell<&mut dyn MeshCamera> {
    fn camera(&self) -> RefMut<'_, dyn MeshCamera + '_> {
        RefMut::map(self.borrow_mut(), |c| {
            &mut **c as &mut (dyn MeshCamera + '_)
        })
    }
}

/// The shared camera: each call borrows it for its own length.
struct SharedCamera<'c>(&'c dyn CameraCell);

impl MeshCamera for SharedCamera<'_> {
    fn view_transform(&mut self) -> [f32; 12] {
        self.0.camera().view_transform()
    }
    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32 {
        self.0.camera().classify_sphere(centre, radius)
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        self.0.camera().half_fov_x_deg()
    }
    fn viewport(&mut self) -> [i32; 4] {
        self.0.camera().viewport()
    }
    fn x_scale_over_near(&mut self) -> f64 {
        self.0.camera().x_scale_over_near()
    }
    fn z_near(&mut self) -> f32 {
        self.0.camera().z_near()
    }
    fn position(&mut self) -> [f32; 3] {
        self.0.camera().position()
    }
}

/// The deformable component's services over the stack: the render half
/// on the device model, the material library and (for the root instance's
/// `RenderInstance`) the render manager, the engine half from `host`.
pub struct StackDeform<'a, 'c, B: Backend + 'static> {
    /// The device model, the material library and its arena.
    pub stack: &'a mut RenderStack<B>,
    /// The engine, the hardpoint component, the animation system, the
    /// channels and the generator.
    pub host: &'a mut dyn DeformHost,
    /// The render manager `RenderInstance` draws the root instance with;
    /// `None` answers rank 0 without a draw.
    pub manager: Option<&'a mut RenderManager<RenderStack<B>>>,
    /// The material `RenderInstance`'s last argument stands for when it
    /// is not 0 (the reference passes the override material's pointer
    /// through `IDeformable::Draw`).
    pub extra: Option<MaterialId>,
    camera: Option<&'c dyn CameraCell>,
    /// The byte offset of each vertex-buffer lock, by buffer number.
    vb_locks: Vec<(u32, u32)>,
}

impl<'a, B: Backend + 'static> StackDeform<'a, '_, B> {
    /// The services over `stack` and `host`, without a render manager.
    #[must_use]
    pub fn new(stack: &'a mut RenderStack<B>, host: &'a mut dyn DeformHost) -> Self {
        Self {
            stack,
            host,
            manager: None,
            extra: None,
            camera: None,
            vb_locks: Vec::new(),
        }
    }
}

/// `IDeformable::Draw` (slot 16) on the stack: the instance's parts
/// skinned and drawn through their materials into the device's draw
/// packets, and the root instance handed to `manager` (when given) with
/// the same camera and `extra` for a non-zero `render_arg`. The level
/// result.
#[allow(clippy::too_many_arguments)]
pub fn draw_deformable<B: Backend + 'static>(
    instance: &mut DeformInstance,
    module: &mut Deformable,
    camera: &mut dyn MeshCamera,
    render_arg: u32,
    flags: u32,
    stack: &mut RenderStack<B>,
    manager: Option<&mut RenderManager<RenderStack<B>>>,
    extra: Option<MaterialId>,
    host: &mut dyn DeformHost,
) -> i32 {
    let shared = RefCell::new(camera);
    let mut services = StackDeform {
        stack,
        host,
        manager,
        extra,
        camera: Some(&shared),
        vb_locks: Vec::new(),
    };
    let mut view = SharedCamera(&shared);
    instance.draw(
        &mut view,
        render_arg,
        flags,
        &mut module.meshes,
        &mut services,
    )
}

impl<B: Backend + 'static> DeformHost for StackDeform<'_, '_, B> {
    fn create_archetype(&mut self, name: &[u8], fs: &mut dyn FileSystem) -> EngineHandle {
        self.host.create_archetype(name, fs)
    }

    fn create_skeleton_archetype(&mut self, name: &[u8], fs: &mut dyn FileSystem) -> EngineHandle {
        self.host.create_skeleton_archetype(name, fs)
    }

    fn release_handle(&mut self, handle: EngineHandle) {
        self.host.release_handle(handle);
    }

    fn query_vmesh_part(&mut self, handle: EngineHandle) -> Result<u32, i32> {
        self.host.query_vmesh_part(handle)
    }

    fn add_ref_vmesh_part(&mut self, part: u32) {
        self.host.add_ref_vmesh_part(part);
    }

    fn release_vmesh_part(&mut self, part: u32) {
        self.host.release_vmesh_part(part);
    }

    fn draw_vmesh_part(&mut self, part: u32, instance: Instance) {
        self.host.draw_vmesh_part(part, instance);
    }

    fn create_instance(
        &mut self,
        handle: EngineHandle,
        link: Option<&mut dyn BoneLink>,
    ) -> Instance {
        self.host.create_instance(handle, link)
    }

    fn destroy_instance(&mut self, instance: Instance) {
        self.host.destroy_instance(instance);
    }

    fn set_instance_transform(&mut self, instance: Instance, bone: usize) -> Option<[f32; 12]> {
        self.host.set_instance_transform(instance, bone)
    }

    fn instance_parent(&mut self, instance: Instance) -> Instance {
        self.host.instance_parent(instance)
    }

    fn instance_relative(&mut self, instance: Instance) -> [u32; 23] {
        self.host.instance_relative(instance)
    }

    fn instance_position(&mut self, instance: Instance) -> [f32; 3] {
        self.host.instance_position(instance)
    }

    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9] {
        self.host.instance_orientation(instance)
    }

    fn set_instance_position(&mut self, instance: Instance, position: &[f32; 3]) {
        self.host.set_instance_position(instance, position);
    }

    fn set_instance_orientation(&mut self, instance: Instance, rows: &[f32; 9]) {
        self.host.set_instance_orientation(instance, rows);
    }

    fn joint_params(&mut self, instance: Instance, index: u32) -> Vec<f32> {
        self.host.joint_params(instance, index)
    }

    fn has_hardpoints(&mut self) -> bool {
        self.host.has_hardpoints()
    }

    fn hardpoint_names(&mut self, archetype: EngineHandle) -> Vec<Vec<u8>> {
        self.host.hardpoint_names(archetype)
    }

    fn connect_hardpoints(&mut self, a: Instance, b: Instance, name: &[u8]) -> u32 {
        self.host.connect_hardpoints(a, b, name)
    }

    fn play_script(&mut self, name: u32, root: Instance, extra: u32, script: u32) -> i32 {
        self.host.play_script(name, root, extra, script)
    }

    fn destroy_script(&mut self, handle: i32) {
        self.host.destroy_script(handle);
    }

    fn filter_objects(&mut self, handle: i32) -> u32 {
        self.host.filter_objects(handle)
    }

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
        self.host.root_eval(handle, vec, flags, time, scale, d, f)
    }

    fn named_duration(&mut self, name: u32, extra: u32) -> Option<f32> {
        self.host.named_duration(name, extra)
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
        self.host
            .eval_playback(handle, flags, time, scale, d, f, xform)
    }

    fn has_object(&mut self, handle: i32, key: u32) -> u32 {
        self.host.has_object(handle, key)
    }

    fn first_instance_field(&mut self, handle: i32) -> Option<u32> {
        self.host.first_instance_field(handle)
    }

    fn duration_if_named(&mut self, handle: i32) -> Option<f32> {
        self.host.duration_if_named(handle)
    }

    fn touch_mapped(&mut self, handle: i32) -> ([f32; 3], [f32; 3]) {
        self.host.touch_mapped(handle)
    }

    fn set_overlay_pos(&mut self, handle: i32, pos: &[f32; 3]) {
        self.host.set_overlay_pos(handle, pos);
    }

    fn set_overlay_extra(&mut self, handle: i32, extra: &[f32; 9]) {
        self.host.set_overlay_extra(handle, extra);
    }

    fn add_overlay_pos(&mut self, handle: i32, delta: &[f32; 3]) {
        self.host.add_overlay_pos(handle, delta);
    }

    fn find_object(&mut self, handle: i32, kind: u32, instance: Instance) -> u32 {
        self.host.find_object(handle, kind, instance)
    }

    fn register_handle(&mut self, key: i32) {
        self.host.register_handle(key);
    }

    fn has_handle(&mut self, key: i32) -> u8 {
        self.host.has_handle(key)
    }

    fn has_any_handle(&mut self, key: i32) {
        self.host.has_any_handle(key);
    }

    fn create_typed_channel(&mut self, owner: u32, joint: Instance, bind: Instance) -> u32 {
        self.host.create_typed_channel(owner, joint, bind)
    }

    fn play_channel(&mut self, key: u32, blob: &[u32; 7]) {
        self.host.play_channel(key, blob);
    }

    fn destroy_channel(&mut self, key: u32) {
        self.host.destroy_channel(key);
    }

    fn channel_in_mixer(&mut self, key: u32) -> u8 {
        self.host.channel_in_mixer(key)
    }

    fn sample_channel(&mut self, key: u32, time: f32) -> ChannelSample {
        self.host.sample_channel(key, time)
    }

    fn rand(&mut self) -> i32 {
        self.host.rand()
    }
}

impl<B: Backend + 'static> DeformRender for StackDeform<'_, '_, B> {
    /// `RenderInstance(camera, instance, 1.0, flags, null, arg)` on the
    /// render manager, with [`StackDeform::extra`] for a non-zero `arg`.
    fn render_instance(&mut self, instance: Instance, flags: u32, arg: u32) -> i32 {
        let (Some(manager), Some(camera)) = (self.manager.as_deref_mut(), self.camera) else {
            return 0;
        };
        let Ok(instance) = usize::try_from(instance) else {
            return 0;
        };
        let request = DrawRequest {
            flags,
            matrix: None,
            extra: if arg != 0 { self.extra } else { None },
        };
        manager.render_instance(
            self.stack,
            &mut SharedCamera(camera),
            instance,
            1.0,
            &request,
        )
    }

    fn pipeline_state(&mut self, id: u32) -> Option<i32> {
        BatchDevice::pipeline_state(self.stack, id)
    }

    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_modelview(self.stack, matrix);
    }

    fn set_world(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_world(self.stack, matrix);
    }

    fn set_world_n(&mut self, index: u32, matrix: &[f32; 12]) {
        self.stack
            .shading
            .device
            .transforms
            .set_world_n(index, matrix);
    }

    fn render_state(&mut self, state: u32) -> i32 {
        MaterialServices::render_state(self.stack, state).cast_signed()
    }

    fn set_render_state(&mut self, state: u32, value: i32) {
        MaterialServices::set_render_state(self.stack, state, value.cast_unsigned());
    }

    fn is_vb_valid(&mut self, vertex_buffer: u32) -> i32 {
        let buffers = &self.stack.shading.device.buffers;
        i32::from(
            self.stack
                .shading
                .vb(vertex_buffer)
                .is_some_and(|h| buffers.vb_info(h).is_ok()),
        )
    }

    fn select_vb(&mut self, vertex_buffer: u32) -> i32 {
        BatchDevice::select_vb(self.stack, vertex_buffer)
    }

    fn create_vb(&mut self, fvf: u32, count: u32) -> Result<u32, i32> {
        MeshServices::create_vb(self.stack, fvf, count)
    }

    fn lock_vb(&mut self, vertex_buffer: u32, count: u32) -> Result<(u32, Vec<u8>), i32> {
        let h = self.stack.shading.vb(vertex_buffer).ok_or(E_FAIL)?;
        let buffers = &mut self.stack.shading.device.buffers;
        let lock = buffers.lock_vb(h, 0, count).map_err(code)?;
        let bytes = buffers.vb(h).map_err(code)?.bytes();
        let range = lock.offset as usize..(lock.offset + lock.len) as usize;
        let locked = bytes.get(range).map(<[u8]>::to_vec).unwrap_or_default();
        self.vb_locks.retain(|&(n, _)| n != vertex_buffer);
        self.vb_locks.push((vertex_buffer, lock.offset));
        Ok((lock.start, locked))
    }

    /// `CopyVertices` into a locked buffer: `count` vertices from vertex
    /// `start` of the source, converted to the buffer's format, at vertex
    /// `base` of the lock.
    fn copy_vertices(
        &mut self,
        vertex_buffer: u32,
        base: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
        locked: &mut [u8],
    ) -> i32 {
        let Some(h) = self.stack.shading.vb(vertex_buffer) else {
            return E_FAIL;
        };
        let Ok((fvf, _)) = self.stack.shading.device.buffers.vb_info(h) else {
            return E_FAIL;
        };
        let at = (base as usize).saturating_mul(fvf.stride() as usize);
        let Some(dest) = locked.get_mut(at..) else {
            return E_FAIL;
        };
        MeshServices::copy_vertex_data(self.stack, dest, fvf.0, source, start, count)
    }

    fn unlock_vb(&mut self, vertex_buffer: u32, locked: Vec<u8>) {
        let Some(h) = self.stack.shading.vb(vertex_buffer) else {
            return;
        };
        let at = self
            .vb_locks
            .iter()
            .find(|&&(n, _)| n == vertex_buffer)
            .map_or(0, |&(_, at)| at);
        self.vb_locks.retain(|&(n, _)| n != vertex_buffer);
        let buffers = &mut self.stack.shading.device.buffers;
        let _ = buffers.write_vb(h, at, &locked);
        let _ = buffers.unlock_vb(h);
    }

    fn destroy_vb(&mut self, vertex_buffer: u32) {
        MeshServices::destroy_vb(self.stack, vertex_buffer);
    }

    fn is_ib_valid(&mut self, index_buffer: u32) -> i32 {
        let valid = self
            .stack
            .ib_source(index_buffer)
            .is_some_and(|s| self.stack.shading.device.buffers.ib(s).is_ok());
        i32::from(valid)
    }

    fn select_ib(&mut self, index_buffer: u32, base_vertex: u32) -> i32 {
        BatchDevice::select_ib(self.stack, index_buffer, base_vertex)
    }

    fn create_ib(&mut self, count: u32) -> Result<u32, i32> {
        MeshServices::create_ib(self.stack, count)
    }

    fn lock_ib(&mut self, index_buffer: u32, count: u32) -> Result<u32, i32> {
        let result = MeshServices::lock_ib(self.stack, index_buffer, count);
        if result < 0 {
            return Err(result);
        }
        Ok(self
            .stack
            .ib_locks
            .iter()
            .find(|&&(n, _)| n == index_buffer)
            .map_or(0, |&(_, start)| start))
    }

    fn unlock_ib(&mut self, index_buffer: u32, written: &[u16]) {
        MeshServices::unlock_ib(self.stack, index_buffer, written);
    }

    fn destroy_ib(&mut self, index_buffer: u32) {
        MeshServices::destroy_ib(self.stack, index_buffer);
    }

    fn find_material(&mut self, name: &[u8], crc: u32) -> (i32, Option<MaterialId>) {
        let _ = name;
        MeshServices::find_material(self.stack, crc)
    }

    fn release_material(&mut self, material: MaterialId) {
        MeshServices::release_material(self.stack, material);
    }

    fn material_class_name(&mut self, material: MaterialId) -> Vec<u8> {
        MeshServices::material_class(self.stack, material)
    }

    /// `Bind(context)`: the binding goes to the stack for the call, where
    /// an `EyeMaterial` takes it (`Field20`), and comes back with the
    /// count the material added.
    fn bind(
        &mut self,
        material: MaterialId,
        context: &SubmitContext<'_>,
        binding: Option<&mut UvBinding>,
    ) {
        match binding {
            Some(binding) => {
                self.stack.uv_binding = Some(*binding);
                MeshServices::bind(self.stack, material, context);
                if let Some(back) = self.stack.uv_binding.take() {
                    *binding = back;
                }
            }
            None => MeshServices::bind(self.stack, material, context),
        }
    }

    fn submit(&mut self, material: MaterialId, context: &SubmitContext<'_>, args: &DrawArgs) {
        MeshServices::submit(self.stack, material, context, args);
    }

    fn teardown(&mut self, material: MaterialId, context: &SubmitContext<'_>) {
        MeshServices::teardown(self.stack, material, context);
    }

    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str) {
        MeshServices::dump(self.stack, flags, file, line, message);
    }
}
