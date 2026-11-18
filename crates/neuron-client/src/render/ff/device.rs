//! The device facade (record 0004, points 1 and 5): one value that owns the
//! state model, the transforms, the lights, the buffers and the textures,
//! and turns each draw into a [`DrawPacket`] for a [`Backend`]. The draw
//! entry points follow DX8RENDERER's (`DrawPrimitive`,
//! `DrawIndexedPrimitiveUp`, `DrawPrimitiveVb`, `DrawIndexedPrimitiveVb`,
//! `IRPDraw::DrawIndexedPrimitive` in `CppSource/DX8RENDERER/DX8RENDERER.cpp`):
//! the primitive count from `PrimCountFromVerts`, nothing drawn when it is
//! not above zero, the light flush before every draw, the caller's indices
//! of `DrawIndexedPrimitiveVb` copied into the scratch ring, and the stream
//! reset after a user-pointer draw.

use std::collections::BTreeSet;
use std::fmt;

use super::buffers::{BufferError, BufferEvent, Buffers, IndexSource, VbHandle};
use super::defaults::Defaults;
use super::draw::{
    DrawConstants, PipelineKey, approximated_samplers, unemulated_stages, unemulated_states,
};
use super::fvf::Fvf;
use super::geometry::{Primitive, draws_anything, fan_to_list, prim_count_from_verts};
use super::lights::Lights;
use super::model::StateModel;
use super::state::{PipelineProperty, RenderStateId, STAGES};
use super::textures::{TextureEvent, TextureHandle, Textures};
use super::transforms::Transforms;

/// Where a draw's vertices come from.
#[derive(Clone, Debug, PartialEq)]
pub enum VertexData {
    /// User-pointer vertices, copied.
    Bytes(Vec<u8>),
    /// A vertex buffer from `first_vertex` on.
    Buffer { buffer: VbHandle, first_vertex: u32 },
}

/// Where a draw's indices come from. Fans are always converted to lists,
/// so their indices are generated.
#[derive(Clone, Debug, PartialEq)]
pub enum IndexData {
    None,
    /// Indices relative to the vertex data's first vertex.
    List(Vec<u16>),
    Buffer {
        source: IndexSource,
        start: u32,
        count: u32,
        base_vertex: u32,
    },
}

/// One draw as the backend receives it.
#[derive(Clone, Debug, PartialEq)]
pub struct DrawPacket {
    pub key: PipelineKey,
    pub constants: DrawConstants,
    /// The primitive the backend draws; a fan arrives as a triangle list.
    pub primitive: Primitive,
    pub primitive_count: u32,
    pub fvf: Fvf,
    pub stride: u32,
    pub vertices: VertexData,
    pub vertex_count: u32,
    pub indices: IndexData,
    pub textures: [Option<TextureHandle>; 2],
    /// The viewport the draw renders into.
    pub viewport: Viewport,
}

/// `ClearBuffers` flags.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClearFlags(pub u32);

impl ClearFlags {
    pub const TARGET: u32 = 1;
    pub const DEPTH: u32 = 2;
    pub const STENCIL: u32 = 4;
}

/// A clear as the backend receives it, with the values of the pipeline
/// properties `CLEAR_COLOR`, `CLEAR_ZDEPTH` and `CLEAR_STENCIL`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Clear {
    pub flags: ClearFlags,
    pub rect: Option<[i32; 4]>,
    pub color: u32,
    pub depth: f32,
    pub stencil: u32,
}

/// `D3DVIEWPORT8`.
#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct Viewport {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
    pub min_z: f32,
    pub max_z: f32,
}

/// A change to a buffer or texture, handed to the backend before the
/// draw or present that may use it (record 0004, point 8; owner answer of
/// 2026-11-18): the backend keeps its own copies current from these.
#[derive(Clone, Copy, Debug)]
pub enum ResourceEvent<'a> {
    Buffer(BufferEvent<'a>),
    Texture(TextureEvent<'a>),
}

/// What a renderer backend does with the model's output.
pub trait Backend {
    /// A resource changed; called before the draw that may read it.
    fn resource(&mut self, event: ResourceEvent<'_>);
    fn draw(&mut self, packet: DrawPacket);
    fn clear(&mut self, clear: Clear, viewport: Viewport);
    fn present(&mut self);
}

/// A resource event as [`RecordingBackend`] keeps it: what changed and how
/// much, without the bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ResourceRecord {
    Vertices {
        buffer: VbHandle,
        recreated: bool,
        offset: usize,
        len: usize,
    },
    Indices {
        source: IndexSource,
        recreated: bool,
        offset: usize,
        len: usize,
    },
    TextureLevel {
        texture: TextureHandle,
        created: bool,
        level: u32,
    },
    ReleaseVertices(VbHandle),
    ReleaseIndices(super::buffers::IbHandle),
    ReleaseTexture(TextureHandle),
}

impl ResourceRecord {
    #[must_use]
    pub fn of(event: &ResourceEvent<'_>) -> ResourceRecord {
        match *event {
            ResourceEvent::Buffer(BufferEvent::Vertices {
                buffer,
                recreated,
                offset,
                bytes,
                ..
            }) => ResourceRecord::Vertices {
                buffer,
                recreated,
                offset,
                len: bytes.len(),
            },
            ResourceEvent::Buffer(BufferEvent::Indices {
                source,
                recreated,
                offset,
                indices,
                ..
            }) => ResourceRecord::Indices {
                source,
                recreated,
                offset,
                len: indices.len(),
            },
            ResourceEvent::Buffer(BufferEvent::ReleaseVertices(h)) => {
                ResourceRecord::ReleaseVertices(h)
            }
            ResourceEvent::Buffer(BufferEvent::ReleaseIndices(h)) => {
                ResourceRecord::ReleaseIndices(h)
            }
            ResourceEvent::Texture(TextureEvent::Level {
                texture,
                created,
                level,
                ..
            }) => ResourceRecord::TextureLevel {
                texture,
                created,
                level,
            },
            ResourceEvent::Texture(TextureEvent::Release(h)) => ResourceRecord::ReleaseTexture(h),
        }
    }
}

/// A backend that keeps what it is given, for tests and model replay.
#[derive(Clone, Debug, Default)]
pub struct RecordingBackend {
    pub resources: Vec<ResourceRecord>,
    pub draws: Vec<DrawPacket>,
    pub clears: Vec<Clear>,
    pub presents: u32,
}

impl Backend for RecordingBackend {
    fn resource(&mut self, event: ResourceEvent<'_>) {
        self.resources.push(ResourceRecord::of(&event));
    }
    fn draw(&mut self, packet: DrawPacket) {
        self.draws.push(packet);
    }
    fn clear(&mut self, clear: Clear, _viewport: Viewport) {
        self.clears.push(clear);
    }
    fn present(&mut self) {
        self.presents += 1;
    }
}

/// Why a draw drew nothing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DrawError {
    /// `E_FAIL`: too few vertices or indices for one primitive, or no
    /// vertex stream.
    Fail,
    Buffer(BufferError),
}

impl DrawError {
    #[must_use]
    pub fn code(self) -> u32 {
        0x8000_4005
    }
}

impl fmt::Display for DrawError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Fail => f.write_str("the draw has nothing to draw (E_FAIL)"),
            Self::Buffer(e) => write!(f, "the draw's buffer call failed: {e}"),
        }
    }
}

impl std::error::Error for DrawError {}

impl From<BufferError> for DrawError {
    fn from(e: BufferError) -> Self {
        DrawError::Buffer(e)
    }
}

/// The renderer.
pub struct Device<B: Backend> {
    pub state: StateModel,
    pub transforms: Transforms,
    pub lights: Lights,
    pub buffers: Buffers,
    pub textures: Textures,
    stage_textures: [Option<TextureHandle>; STAGES],
    viewport: Viewport,
    /// States and stages a draw relied on that the shaders do not emulate,
    /// each reported once (record 0004, point 3).
    pub unemulated: BTreeSet<RenderStateId>,
    pub unemulated_stage_seen: BTreeSet<usize>,
    /// Emulated stages drawn with an approximated sampler (D209).
    pub approximated_sampler_seen: BTreeSet<usize>,
    pub backend: B,
}

impl<B: Backend> Device<B> {
    /// `Startup` with `defaults` and `CreateBuffers` for a back buffer of
    /// `width` by `height`.
    pub fn new(defaults: Defaults, width: u32, height: u32, backend: B) -> Self {
        let state = StateModel::new(defaults);
        let viewspace = state.pipeline(PipelineProperty::ViewspaceLights) != 0;
        let buffers = Buffers::new();
        Device {
            transforms: Transforms::new(viewspace),
            state,
            lights: Lights::new(),
            buffers,
            textures: Textures::new(),
            stage_textures: [None; STAGES],
            viewport: Viewport {
                x: 0,
                y: 0,
                width,
                height,
                min_z: 0.0,
                max_z: 1.0,
            },
            unemulated: BTreeSet::new(),
            unemulated_stage_seen: BTreeSet::new(),
            approximated_sampler_seen: BTreeSet::new(),
            backend,
        }
    }

    fn viewspace_lights(&self) -> bool {
        self.state.pipeline(PipelineProperty::ViewspaceLights) != 0
    }

    pub fn set_modelview(&mut self, m: &super::transforms::Affine12) {
        let v = self.viewspace_lights();
        self.transforms.set_modelview(m, v);
    }

    #[must_use]
    pub fn modelview(&self) -> super::transforms::Affine12 {
        self.transforms.modelview(self.viewspace_lights())
    }

    /// `ResetRenderStatesToDefaults`: render and stage states, and the
    /// stage textures and transforms of `ApplyDefaultTextureStages`.
    pub fn reset_render_states(&mut self) {
        self.state.reset_render_states();
        self.stage_textures = [None; STAGES];
        self.transforms.reset_texture_transforms();
    }

    /// `SetTextureStageTexture`: nothing while the pipeline property
    /// `TEXTURE` is off.
    pub fn set_stage_texture(&mut self, stage: usize, texture: Option<TextureHandle>) {
        if self.state.pipeline(PipelineProperty::Texture) == 0 {
            return;
        }
        if let Some(slot) = self.stage_textures.get_mut(stage) {
            *slot = texture;
        }
    }

    /// `GetTextureStageTexture`: `None` while `TEXTURE` is off.
    #[must_use]
    pub fn stage_texture(&self, stage: usize) -> Option<TextureHandle> {
        if self.state.pipeline(PipelineProperty::Texture) == 0 {
            return None;
        }
        self.stage_textures.get(stage).copied().flatten()
    }

    /// `DestroyTexture` and `SetTextureLevelData` unbind the texture from
    /// every stage.
    pub fn unbind_texture(&mut self, texture: TextureHandle) {
        for slot in &mut self.stage_textures {
            if *slot == Some(texture) {
                *slot = None;
            }
        }
    }

    pub fn destroy_texture(
        &mut self,
        texture: TextureHandle,
    ) -> Result<(), super::textures::TextureError> {
        self.unbind_texture(texture);
        self.textures.destroy(texture)
    }

    pub fn set_viewport(&mut self, x: u32, y: u32, width: u32, height: u32) {
        self.viewport.x = x;
        self.viewport.y = y;
        self.viewport.width = width;
        self.viewport.height = height;
    }

    pub fn set_depth_range(&mut self, min_z: f32, max_z: f32) {
        self.viewport.min_z = min_z;
        self.viewport.max_z = max_z;
    }

    #[must_use]
    pub fn viewport(&self) -> Viewport {
        self.viewport
    }

    /// `ClearBuffers`.
    pub fn clear(&mut self, flags: u32, rect: Option<[i32; 4]>) {
        let clear = Clear {
            flags: ClearFlags(flags),
            rect,
            color: self.state.pipeline(PipelineProperty::ClearColor),
            depth: f32::from_bits(self.state.pipeline(PipelineProperty::ClearZDepth)),
            stencil: self.state.pipeline(PipelineProperty::ClearStencil),
        };
        self.sync_resources();
        self.backend.clear(clear, self.viewport);
    }

    /// `SwapBuffers`.
    /// Hands the backend every buffer and texture change since the last
    /// call.
    pub fn sync_resources(&mut self) {
        let backend = &mut self.backend;
        self.buffers
            .drain_changes(|e| backend.resource(ResourceEvent::Buffer(e)));
        self.textures
            .drain_changes(|e| backend.resource(ResourceEvent::Texture(e)));
    }

    pub fn present(&mut self) {
        self.sync_resources();
        self.backend.present();
    }

    /// `FlushDrawState` and the packet.
    fn submit(
        &mut self,
        primitive: Primitive,
        primitive_count: u32,
        fvf: Fvf,
        mut vertices: VertexData,
        vertex_count: u32,
        indices: IndexData,
    ) {
        // The C++ ignores the light flush's result at a draw.
        let _ = self.lights.flush();
        self.unemulated.extend(unemulated_states(&self.state));
        self.unemulated_stage_seen
            .extend(unemulated_stages(&self.state));
        self.approximated_sampler_seen
            .extend(approximated_samplers(&self.state));
        let (primitive, indices, primitive_count) = if primitive == Primitive::TriangleFan {
            let fan: Vec<u16> = match &indices {
                IndexData::List(list) => list.clone(),
                IndexData::Buffer {
                    source,
                    start,
                    count,
                    ..
                } => self
                    .buffers
                    .ib(*source)
                    .map(|ib| {
                        let s = *start as usize;
                        ib.indices()
                            .get(s..s + *count as usize)
                            .unwrap_or_default()
                            .to_vec()
                    })
                    .unwrap_or_default(),
                IndexData::None => (0..vertex_count).map(|i| i as u16).collect(),
            };
            let list = fan_to_list(&fan, primitive_count as usize);
            let count = (list.len() / 3) as u32;
            // The list's indices are relative to the first vertex, so the
            // buffer's base vertex moves there.
            if let (
                IndexData::Buffer { base_vertex, .. },
                VertexData::Buffer { first_vertex, .. },
            ) = (&indices, &mut vertices)
            {
                *first_vertex = first_vertex.wrapping_add(*base_vertex);
            }
            (Primitive::TriangleList, IndexData::List(list), count)
        } else {
            (primitive, indices, primitive_count)
        };
        let packet = DrawPacket {
            key: PipelineKey::new(&self.state, fvf, primitive),
            constants: DrawConstants::new(&self.state, &self.transforms, &self.lights, fvf),
            primitive,
            primitive_count,
            fvf,
            stride: fvf.stride(),
            vertices,
            vertex_count,
            indices,
            textures: [self.stage_texture(0), self.stage_texture(1)],
            viewport: self.viewport,
        };
        self.sync_resources();
        self.backend.draw(packet);
    }

    fn primitive(prim: u32) -> Result<Primitive, DrawError> {
        Primitive::from_raw(prim).ok_or(DrawError::Fail)
    }

    /// `DrawPrimitive` (user pointer): `count` vertices of `fvf`.
    pub fn draw_up(
        &mut self,
        prim: u32,
        fvf: Fvf,
        vertices: &[u8],
        count: u32,
    ) -> Result<(), DrawError> {
        if count.cast_signed() <= 0 {
            return Err(DrawError::Fail);
        }
        let primitive = Self::primitive(prim)?;
        let pc = prim_count_from_verts(prim, count);
        let bytes = (fvf.stride() as usize) * (count as usize);
        let data = vertices.get(..bytes).ok_or(DrawError::Fail)?.to_vec();
        self.submit(
            primitive,
            pc,
            fvf,
            VertexData::Bytes(data),
            count,
            IndexData::None,
        );
        self.buffers.release_streams();
        Ok(())
    }

    /// `DrawIndexedPrimitiveUp`.
    pub fn draw_indexed_up(
        &mut self,
        prim: u32,
        fvf: Fvf,
        vertices: &[u8],
        vertex_count: u32,
        indices: &[u16],
    ) -> Result<(), DrawError> {
        let count = u32::try_from(indices.len()).map_err(|_| DrawError::Fail)?;
        let pc = prim_count_from_verts(prim, count);
        if !draws_anything(pc) {
            return Err(DrawError::Fail);
        }
        let primitive = Self::primitive(prim)?;
        let bytes = (fvf.stride() as usize) * (vertex_count as usize);
        let data = vertices.get(..bytes).ok_or(DrawError::Fail)?.to_vec();
        self.submit(
            primitive,
            pc,
            fvf,
            VertexData::Bytes(data),
            vertex_count,
            IndexData::List(indices.to_vec()),
        );
        self.buffers.release_streams();
        Ok(())
    }

    fn resolve_vb(&self, vb: Option<VbHandle>) -> Result<VbHandle, DrawError> {
        vb.or(self.buffers.streams.vertex_buffer)
            .ok_or(DrawError::Fail)
    }

    /// `DrawPrimitiveVb`: `vb` `None` is handle -1, the selected buffer.
    pub fn draw_vb(
        &mut self,
        prim: u32,
        vb: Option<VbHandle>,
        start: u32,
        count: u32,
    ) -> Result<(), DrawError> {
        let pc = prim_count_from_verts(prim, count);
        if !draws_anything(pc) {
            return Err(DrawError::Fail);
        }
        let primitive = Self::primitive(prim)?;
        let h = self.resolve_vb(vb)?;
        let fvf = self.buffers.vb(h)?.fvf;
        self.buffers.streams.vertex_buffer = Some(h);
        self.buffers.streams.fvf = 0;
        self.submit(
            primitive,
            pc,
            fvf,
            VertexData::Buffer {
                buffer: h,
                first_vertex: start,
            },
            count,
            IndexData::None,
        );
        Ok(())
    }

    /// `DrawIndexedPrimitiveVb`: the indices go into the scratch ring
    /// first; vertices are read from `base_vertex` on.
    pub fn draw_indexed_vb(
        &mut self,
        prim: u32,
        vb: Option<VbHandle>,
        base_vertex: u32,
        vertex_count: u32,
        indices: &[u16],
    ) -> Result<(), DrawError> {
        let count = u32::try_from(indices.len()).map_err(|_| DrawError::Fail)?;
        let pc = prim_count_from_verts(prim, count);
        if !draws_anything(pc) {
            return Err(DrawError::Fail);
        }
        let primitive = Self::primitive(prim)?;
        let start = self
            .buffers
            .copy_indices(IndexSource::Scratch, 0, indices)?;
        let h = self.resolve_vb(vb)?;
        let fvf = self.buffers.vb(h)?.fvf;
        self.buffers.streams.vertex_buffer = Some(h);
        self.buffers.streams.fvf = 0;
        self.buffers.streams.index_buffer = Some(IndexSource::Scratch);
        self.buffers.streams.base_vertex = base_vertex;
        self.submit(
            primitive,
            pc,
            fvf,
            VertexData::Buffer {
                buffer: h,
                first_vertex: 0,
            },
            vertex_count,
            IndexData::Buffer {
                source: IndexSource::Scratch,
                start,
                count,
                base_vertex,
            },
        );
        Ok(())
    }

    /// `IRPDraw::DrawIndexedPrimitive` on the selected streams.
    pub fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> Result<(), DrawError> {
        let pc = prim_count_from_verts(prim, count);
        if !draws_anything(pc) {
            return Err(DrawError::Fail);
        }
        let primitive = Self::primitive(prim)?;
        let h = self.buffers.streams.vertex_buffer.ok_or(DrawError::Fail)?;
        let source = self.buffers.streams.index_buffer.ok_or(DrawError::Fail)?;
        let fvf = match self.buffers.streams.fvf {
            0 => self.buffers.vb(h)?.fvf,
            f => Fvf(f),
        };
        let base_vertex = self.buffers.streams.base_vertex;
        self.submit(
            primitive,
            pc,
            fvf,
            VertexData::Buffer {
                buffer: h,
                first_vertex: min_index,
            },
            vertex_count,
            IndexData::Buffer {
                source,
                start: start_index,
                count,
                base_vertex,
            },
        );
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::super::state::{RenderState, StageState, TextureOp};
    use super::*;
    use neuron_core::image::PixelFormat;

    fn device() -> Device<RecordingBackend> {
        Device::new(Defaults::builtin(), 640, 480, RecordingBackend::default())
    }

    const XYZ_DIFFUSE: Fvf = Fvf(0x42);

    #[test]
    fn user_pointer_draws_copy_their_vertices() {
        let mut d = device();
        let verts = vec![1u8; 16 * 4];
        d.draw_up(4, XYZ_DIFFUSE, &verts, 3).unwrap();
        let p = &d.backend.draws[0];
        assert_eq!(p.primitive, Primitive::TriangleList);
        assert_eq!(p.primitive_count, 1);
        assert_eq!(p.vertices, VertexData::Bytes(vec![1; 48]));
        assert_eq!(d.draw_up(4, XYZ_DIFFUSE, &verts, 0), Err(DrawError::Fail));
        // Too few indices for a triangle: nothing drawn.
        assert_eq!(
            d.draw_indexed_up(4, XYZ_DIFFUSE, &verts, 4, &[0, 1]),
            Err(DrawError::Fail)
        );
        assert_eq!(d.backend.draws.len(), 1);
    }

    #[test]
    fn indexed_fans_keep_their_base_vertex() {
        let mut d = device();
        let (listing, lock) = d.buffers.lock_vertex_memory(XYZ_DIFFUSE, 8).unwrap();
        d.buffers
            .write_vb(listing.buffer, lock.offset, &[1; 128])
            .unwrap();
        d.buffers.unlock_vb(listing.buffer).unwrap();
        d.draw_indexed_vb(6, Some(listing.buffer), 5, 8, &[0, 1, 2, 3])
            .unwrap();
        let p = &d.backend.draws[0];
        assert_eq!(p.indices, IndexData::List(vec![0, 1, 2, 0, 2, 3]));
        assert_eq!(
            p.vertices,
            VertexData::Buffer {
                buffer: listing.buffer,
                first_vertex: 5
            }
        );
    }

    #[test]
    fn fans_arrive_as_lists() {
        let mut d = device();
        let verts = vec![0u8; 16 * 5];
        d.draw_up(6, XYZ_DIFFUSE, &verts, 5).unwrap();
        let p = &d.backend.draws[0];
        assert_eq!(p.primitive, Primitive::TriangleList);
        assert_eq!(p.primitive_count, 3);
        assert_eq!(p.indices, IndexData::List(vec![0, 1, 2, 0, 2, 3, 0, 3, 4]));
    }

    #[test]
    fn indexed_vb_draws_go_through_the_scratch_ring() {
        let mut d = device();
        let (listing, lock) = d.buffers.lock_vertex_memory(XYZ_DIFFUSE, 4).unwrap();
        d.buffers
            .write_vb(listing.buffer, lock.offset, &[2; 64])
            .unwrap();
        d.buffers.unlock_vb(listing.buffer).unwrap();
        d.draw_indexed_vb(
            4,
            Some(listing.buffer),
            listing.base_vertex,
            4,
            &[0, 1, 2, 2, 1, 3],
        )
        .unwrap();
        d.draw_indexed_vb(4, None, 0, 4, &[0, 1, 2]).unwrap();
        let p = &d.backend.draws[1];
        assert_eq!(
            p.indices,
            IndexData::Buffer {
                source: IndexSource::Scratch,
                start: 6,
                count: 3,
                base_vertex: 0
            }
        );
        assert_eq!(d.buffers.streams.vertex_buffer, Some(listing.buffer));
        // The backend saw each change once, before the draw that read it:
        // the new ring and the scratch indices, then only the 3 new indices.
        let r = &d.backend.resources;
        assert!(r.contains(&ResourceRecord::Vertices {
            buffer: listing.buffer,
            recreated: true,
            offset: 0,
            len: 1024 * 16,
        }));
        assert_eq!(
            r.last(),
            Some(&ResourceRecord::Indices {
                source: IndexSource::Scratch,
                recreated: false,
                offset: 6,
                len: 3,
            })
        );
        let seen = d.backend.resources.len();
        d.sync_resources();
        assert_eq!(d.backend.resources.len(), seen);
    }

    #[test]
    fn packets_carry_state_lights_and_textures() {
        let mut d = device();
        let t = d
            .textures
            .create(4, 4, PixelFormat::A8R8G8B8, 1, 0)
            .unwrap();
        d.set_stage_texture(0, Some(t));
        d.state
            .set_stage_state(0, StageState::ColorOp(TextureOp::Modulate));
        d.state.set_render_state(RenderState::StencilEnable(true));
        d.draw_up(1, XYZ_DIFFUSE, &[0; 16], 1).unwrap();
        let p = &d.backend.draws[0];
        assert_eq!(p.textures, [Some(t), None]);
        assert_eq!(p.constants.stages[0].color_op, TextureOp::Modulate);
        assert!(d.unemulated.contains(&RenderStateId::StencilEnable));
        // Destroying the texture unbinds it.
        d.destroy_texture(t).unwrap();
        assert_eq!(d.stage_texture(0), None);
        // With TEXTURE off, binding does nothing.
        d.state.set_pipeline(PipelineProperty::Texture, 0);
        let u = d
            .textures
            .create(4, 4, PixelFormat::A8R8G8B8, 1, 0)
            .unwrap();
        d.set_stage_texture(0, Some(u));
        d.state.set_pipeline(PipelineProperty::Texture, 1);
        assert_eq!(d.stage_texture(0), None);
    }

    #[test]
    fn clears_use_the_pipeline_properties() {
        let mut d = device();
        d.state
            .set_pipeline(PipelineProperty::ClearColor, 0xff00_00ff);
        d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        let c = d.backend.clears[0];
        assert_eq!(c.color, 0xff00_00ff);
        assert_eq!(c.depth, 1.0);
        assert_eq!(c.stencil, u32::MAX);
        d.present();
        assert_eq!(d.backend.presents, 1);
    }
}
