//! The `EyeMaterial` in the material library (work package P4.7, second
//! increment): the material as a [`Material`] of `render::material`, so
//! that the library creates it, keeps it in its arena and draws it like
//! any other kind; its factory ([`EyeFactory`], `FEyeMaterialFactory`,
//! which `RegisterEyeMaterialFactory` registers with the component
//! manager) as one link of the factory chain; and the name rule
//! `BindEyeMaterials` adds ([`bind_eye_materials`]).
//!
//! The material's own calls ([`EyeServices`]) are answered by an adapter
//! over the library's services: the renderer, `IRPDraw`, the vertex-buffer
//! manager and the texture library through [`MaterialServices`]; the
//! engine's parent and orientation queries, the face group's UV-deform
//! binding (the context's `Field20`) and `FDUMP` through [`EyeHost`]. The
//! texture library's records, which the reference hands out as pointers
//! and the eye holds only within one `Setup`, are numbered by the adapter
//! for that call.

use neuron_core::material_data::{LeafWrite, PropertyValue};
use neuron_core::vfs::FileSystem;

use crate::render::material::{
    DrawArgs, GR_GENERIC, GR_INTERFACE_UNSUPPORTED, Material, MaterialFactory, MaterialLibrary,
    MaterialServices, SubmitContext, VertexLock, VertexSource,
};

use super::eye::{self, EyeMaterial, EyeServices, ParamValue, parameter_table};
use super::{Instance, UvBinding};

/// What the `EyeMaterial` needs beyond [`MaterialServices`]: the engine
/// (`G_Engine`'s parent and orientation queries), the UV-deform binding a
/// deformable face group hands its material through the context's
/// `Field20`, and `FDUMP`.
pub trait EyeHost: MaterialServices {
    /// `IEngine::GetInstanceParent`: -1 for none.
    fn eye_instance_parent(&mut self, instance: Instance) -> Instance;
    /// `IEngine::GetInstanceOrientation`: the nine floats.
    fn eye_instance_orientation(&mut self, instance: Instance) -> [f32; 9];
    /// The binding `Field20` points to, taken out for the bind; `None`
    /// when the drawing mesh gave none.
    fn take_uv_binding(&mut self) -> Option<UvBinding>;
    /// Puts the binding back after the bind, with the count it added.
    fn restore_uv_binding(&mut self, binding: UvBinding);
    /// `FDUMP(flags, "%s(%d) : <severity>:General:%s", file, line,
    /// message)`.
    fn eye_dump(&mut self, flags: i32, file: &str, line: u32, message: &str);
}

/// [`EyeServices`] over the library's services for one call into the
/// material: the context's indices for the vertex-buffer draws and the
/// texture records the call holds, by number.
struct Adapter<'a, S: MaterialServices + ?Sized> {
    services: &'a mut S,
    indices: &'a [u16],
    records: Vec<S::TextureRecord>,
}

impl<'a, S: MaterialServices + ?Sized> Adapter<'a, S> {
    fn new(services: &'a mut S, indices: &'a [u16]) -> Self {
        Self {
            services,
            indices,
            records: Vec::new(),
        }
    }

    fn record(&self, number: u32) -> Option<S::TextureRecord> {
        self.records.get(number as usize).copied()
    }
}

impl<S: EyeHost + ?Sized> EyeServices for Adapter<'_, S> {
    fn set_material(&mut self, block: &[u32; 17]) {
        self.services.set_material(block);
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        MaterialServices::set_render_state(self.services, state, value);
    }

    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        self.services.set_stage_state(stage, state, value);
    }

    fn set_stage_texture(&mut self, stage: u32, handle: u32) {
        self.services.set_stage_texture(stage, handle);
    }

    fn set_texture_transform(&mut self, stage: u32, matrix: &[f32; 16]) {
        self.services.set_texture_transform(stage, matrix);
    }

    fn texture_format(&mut self, handle: u32) -> Result<i32, i32> {
        self.services.texture_format(handle).map(u32::cast_signed)
    }

    fn verify_state(&mut self) -> i32 {
        self.services.verify_state()
    }

    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> i32 {
        MaterialServices::draw_indexed(
            self.services,
            prim,
            min_index,
            vertex_count,
            start_index,
            count,
        )
    }

    fn draw_indexed_vb(
        &mut self,
        prim: u32,
        vertex_buffer: u32,
        base_vertex: u32,
        vertex_count: u32,
        ib_start: u32,
        span: u32,
    ) -> i32 {
        // `indices + ib_start` for `span` indices; the context's array ends
        // where the reference would read on (an index past it reads as 0,
        // as the batcher's copies do).
        let start = (ib_start as usize).min(self.indices.len());
        let end = start.saturating_add(span as usize);
        let mut indices = self.indices[start..end.min(self.indices.len())].to_vec();
        indices.resize(span as usize, 0);
        self.services
            .draw_indexed_vb(prim, vertex_buffer, base_vertex, vertex_count, &indices)
    }

    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        self.services.lock_vertex_memory(fvf, count)
    }

    fn copy_vertex_data(
        &mut self,
        lock: &mut VertexLock,
        fvf: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    ) {
        self.services
            .copy_vertex_data(&mut lock.bytes, fvf, source, start, count);
    }

    fn unlock_vertex_memory(&mut self, lock: VertexLock) {
        self.services.unlock_vertex_memory(lock);
    }

    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<u32> {
        let record = self.services.find_texture(name, crc)?;
        self.records.push(record);
        u32::try_from(self.records.len() - 1).ok()
    }

    fn frame_count(&mut self, record: u32) -> i32 {
        // `GetFrameCount` leaves its output alone when it fails; the
        // library fails only for a null record, which the eye never passes.
        self.record(record)
            .and_then(|r| self.services.texture_frame_count(r))
            .unwrap_or(0)
    }

    fn create_playback(&mut self, record: u32) -> Result<i32, i32> {
        match self.record(record) {
            Some(r) => self.services.create_playback(r),
            None => Err(GR_GENERIC),
        }
    }

    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<u32> {
        self.services.texture_frame(id, frame).map(|f| f.handle)
    }

    fn release_texture_record(&mut self, record: u32) {
        if let Some(r) = self.record(record) {
            self.services.release_texture_record(r);
        }
    }

    fn dec_ref_texture(&mut self, id: i32) {
        self.services.dec_ref_texture(id);
    }

    fn instance_parent(&mut self, instance: Instance) -> Instance {
        self.services.eye_instance_parent(instance)
    }

    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9] {
        self.services.eye_instance_orientation(instance)
    }

    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str) {
        self.services.eye_dump(flags, file, line, message);
    }
}

fn to_property(cell: ParamValue) -> PropertyValue {
    PropertyValue {
        kind: cell.ty.cast_unsigned(),
        words: cell.words,
        text: cell.text,
    }
}

fn from_property(value: &PropertyValue) -> ParamValue {
    ParamValue {
        ty: value.kind.cast_signed(),
        words: value.words,
        text: value.text.clone(),
    }
}

impl<S: EyeHost + 'static> Material<S> for EyeMaterial {
    fn initialize(&mut self, services: &mut S) -> i32 {
        EyeMaterial::initialize(self, &mut Adapter::new(services, &[]))
    }

    fn set_name(&mut self, name: &[u8]) -> i32 {
        EyeMaterial::set_name(self, Some(name))
    }

    fn load(&mut self, services: &mut S, fs: &mut dyn FileSystem) -> i32 {
        EyeMaterial::load(self, &*fs, &mut Adapter::new(services, &[]))
    }

    fn apply_verify(&mut self, services: &mut S) -> i32 {
        EyeMaterial::apply_verify(self, &mut Adapter::new(services, &[]))
    }

    fn resolve_textures(&mut self, services: &mut S, delta: f32) -> i32 {
        let _ = delta;
        EyeMaterial::resolve_textures(self, &mut Adapter::new(services, &[]))
    }

    fn render_flags(&mut self, services: &mut S) -> u32 {
        let _ = services;
        EyeMaterial::render_flags(self)
    }

    fn submit(&mut self, services: &mut S, context: &SubmitContext<'_>, args: &DrawArgs) -> i32 {
        EyeMaterial::submit(
            self,
            &mut Adapter::new(services, context.indices),
            context,
            args,
        )
    }

    /// `Bind`: the binding is the one `Field20` points to (a non-zero
    /// `field20`), taken from the services for the call.
    fn bind(&mut self, services: &mut S, context: Option<&SubmitContext<'_>>) -> i32 {
        let mut binding = match context {
            Some(c) if c.field20 != 0 => services.take_uv_binding(),
            _ => None,
        };
        let indices = context.map_or(&[][..], |c| c.indices);
        let result = EyeMaterial::bind(
            self,
            &mut Adapter::new(services, indices),
            context,
            binding.as_mut(),
        );
        if let Some(binding) = binding {
            services.restore_uv_binding(binding);
        }
        result
    }

    fn teardown(&mut self, services: &mut S, context: Option<&SubmitContext<'_>>) -> i32 {
        EyeMaterial::teardown(self, &mut Adapter::new(services, &[]), context)
    }

    fn name(&self) -> Result<Vec<u8>, i32> {
        self.name.clone().ok_or(GR_GENERIC)
    }

    fn class_name(&self) -> &'static str {
        eye::CLASS_NAME
    }

    fn parameter_count(&self) -> Result<u32, i32> {
        Ok(u32::try_from(parameter_table().len()).unwrap_or(0))
    }

    fn parameter(&self, index: u32) -> Result<PropertyValue, i32> {
        self.get_value(index as usize)
            .map(to_property)
            .ok_or(GR_GENERIC)
    }

    /// `GetParameterDescriptor`'s leaf name: the entry's key.
    fn parameter_tag(&self, index: u32) -> Result<Option<Vec<u8>>, i32> {
        parameter_table()
            .get(index as usize)
            .map(|entry| Some(entry.key.as_bytes().to_vec()))
            .ok_or(GR_GENERIC)
    }

    fn set_parameter(&mut self, services: &mut S, index: u32, value: &PropertyValue) -> i32 {
        self.set_value(
            index as usize,
            &from_property(value),
            &mut Adapter::new(services, &[]),
        )
    }

    fn save(&mut self, fs: &dyn FileSystem) -> Result<Vec<LeafWrite>, i32> {
        Ok(EyeMaterial::save(self, fs))
    }

    fn destroy(&mut self, services: &mut S) {
        EyeMaterial::destroy(self, &mut Adapter::new(services, &[]));
    }
}

/// `FEyeMaterialFactory`: the `EyeMaterial` by its exact name, any other
/// name through the fallback (the factories registered before it, which
/// the component manager asks in turn).
pub struct EyeFactory<S: ?Sized> {
    fallback: Option<Box<dyn MaterialFactory<S>>>,
}

impl<S: ?Sized> std::fmt::Debug for EyeFactory<S> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("EyeFactory")
            .field("fallback", &self.fallback.is_some())
            .finish()
    }
}

impl<S: ?Sized> Default for EyeFactory<S> {
    fn default() -> Self {
        Self::new()
    }
}

impl<S: ?Sized> EyeFactory<S> {
    /// The `EyeMaterial`'s factory alone.
    #[must_use]
    pub fn new() -> EyeFactory<S> {
        EyeFactory { fallback: None }
    }

    /// The `EyeMaterial`'s factory, then `fallback` for any other name.
    #[must_use]
    pub fn with_fallback(fallback: Box<dyn MaterialFactory<S>>) -> EyeFactory<S> {
        EyeFactory {
            fallback: Some(fallback),
        }
    }
}

impl<S: EyeHost + 'static> MaterialFactory<S> for EyeFactory<S> {
    /// `CreateInstance` with a descriptor naming `EyeMaterial`: the
    /// constructed material (its `InitializeFromDescriptor` does nothing);
    /// the library initialises and names it.
    fn create(&mut self, services: &mut S, type_name: &[u8]) -> Result<Box<dyn Material<S>>, i32> {
        if type_name == eye::CLASS_NAME.as_bytes() {
            return Ok(Box::new(EyeMaterial::new()));
        }
        match self.fallback.as_mut() {
            Some(fallback) => fallback.create(services, type_name),
            None => Err(GR_INTERFACE_UNSUPPORTED),
        }
    }
}

/// `BindEyeMaterials`: the name rule `^eye*` to `EyeMaterial`, tried
/// before every rule the library has (`AddMapRule(0, "^eye*",
/// "EyeMaterial")`). The reference registers the factory first
/// (`RegisterEyeMaterialFactory`); here the library is made with an
/// [`EyeFactory`] in its chain. The pattern is the material regex's: `e*`
/// is any run of `e`, so every name that starts with `ey` matches.
pub fn bind_eye_materials<S: ?Sized>(library: &mut MaterialLibrary<S>) -> i32 {
    library.add_map_rule(0, eye::MAP_PATTERN.as_bytes(), eye::CLASS_NAME.as_bytes())
}
