//! The Direct3D 12 backend of the renderer (record 0004, point 8; work
//! package P4.2): draws the device's [`DrawPacket`]s into an offscreen
//! scene target with a depth buffer.
//!
//! The backend keeps default-heap copies of the device's buffers and
//! textures, kept current from the device's resource events (owner answer
//! of 2026-11-18), and sends everything else (user-pointer vertices and
//! indices, constant buffers, copy sources) through the upload ring. One
//! command list records a frame; [`Backend::present`] runs it and waits.
//! Replaced or released GPU resources stay alive until then.
//!
//! The backend trait has no error path: a failed call is kept in
//! [`D3d12Backend::errors`] and the draw is skipped.
//!
//! A draw binds four views: each stage's 2D view and its cube view, the
//! null view of the other kind beside a stage's texture. A cube map from
//! `LoadCubemap` is one level of six array slices behind a cube view.

use std::collections::HashMap;

use windows::Win32::Foundation::{HWND, RECT};
use windows::Win32::Graphics::Direct3D::{
    D3D_PRIMITIVE_TOPOLOGY, D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,
    D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
};
use windows::Win32::Graphics::Direct3D12::{
    D3D12_CLEAR_FLAG_DEPTH, D3D12_CLEAR_FLAG_STENCIL, D3D12_CLEAR_FLAGS,
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, D3D12_DESCRIPTOR_HEAP_DESC,
    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
    D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_HEAP_FLAG_NONE,
    D3D12_HEAP_PROPERTIES, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD,
    D3D12_INDEX_BUFFER_VIEW, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, D3D12_RANGE, D3D12_RESOURCE_DESC,
    D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE,
    D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_INDEX_BUFFER,
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT,
    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
    D3D12_RESOURCE_STATES, D3D12_SHADER_RESOURCE_VIEW_DESC, D3D12_SHADER_RESOURCE_VIEW_DESC_0,
    D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURECUBE, D3D12_SUBRESOURCE_FOOTPRINT,
    D3D12_TEX2D_SRV, D3D12_TEXCUBE_SRV, D3D12_TEXTURE_COPY_LOCATION, D3D12_TEXTURE_COPY_LOCATION_0,
    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_VERTEX_BUFFER_VIEW,
    D3D12_VIEWPORT, ID3D12DescriptorHeap, ID3D12Resource,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM,
    DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16_UINT,
    DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_SAMPLE_DESC,
};

use neuron_core::image::PixelFormat;
use neuron_core::image::convert::to_rgba8;

use super::pipeline::{
    DEPTH_FORMAT, PipelineCache, ROOT_CONSTANTS, ROOT_SAMPLERS, ROOT_TEXTURES, Shaders,
    sampler_descs,
};
use super::present::{Blit, SwapChain, ramp_texels};
use super::upload::{DEFAULT_BUFFER_STATE, UploadRing, buffer};
use super::{AdapterChoice, Context, Gpu, GpuError, RenderTarget, release_barrier, transition};
use crate::render::ff::buffers::{BufferEvent, IndexSource, VbHandle};
use crate::render::ff::device::{
    Backend, Clear, ClearFlags, DrawPacket, IndexData, ResourceEvent, VertexData, Viewport,
};
use crate::render::ff::gamma::GammaRamp;
use crate::render::ff::geometry::Primitive;
use crate::render::ff::textures::{Texture, TextureEvent, TextureHandle};
use crate::render::shader::constants::{ViewportRect, pack};
use crate::render::shader::input::DEFAULTS;

/// The compiled fixed-function shaders (`cargo xtask shaders`).
pub const FIXED_FUNCTION: Shaders = Shaders {
    vs: include_bytes!("../../../compiled_shaders/fixed_function_vs.cso"),
    ps: include_bytes!("../../../compiled_shaders/fixed_function_ps.cso"),
    ps_flat: include_bytes!("../../../compiled_shaders/fixed_function_ps_flat.cso"),
};

/// Texture SRVs the CPU-side heap holds, the null descriptors included.
const TEXTURE_SLOTS: u32 = 4096;
/// The slot of the null cube view (the null 2D view is in slot 0).
const NULL_CUBE: u32 = 1;
/// Descriptors of the shader-visible ring: four per draw.
const DRAW_SLOTS: u32 = 4 * 4096;

/// A default-heap buffer and the state it was last moved to.
struct GpuBuffer {
    resource: ID3D12Resource,
    size: u64,
    state: D3D12_RESOURCE_STATES,
}

/// A default-heap texture with its view.
struct GpuTexture {
    resource: ID3D12Resource,
    /// Whether levels go through the RGBA8 conversion.
    convert: bool,
    /// Subresources: mip levels, or the six faces of a cube map.
    levels: u32,
    /// A cube map: six faces of one level, a cube view.
    cube: bool,
    slot: u32,
    state: D3D12_RESOURCE_STATES,
}

/// The upload format of a texture: a format Direct3D 12 samples as is, or
/// RGBA8 converted on the CPU.
fn texture_format(t: &Texture) -> (DXGI_FORMAT, bool) {
    let blocks = t.width.is_multiple_of(4) && t.height.is_multiple_of(4);
    match t.format {
        PixelFormat::A8R8G8B8 => (DXGI_FORMAT_B8G8R8A8_UNORM, false),
        PixelFormat::DXT1 if blocks => (DXGI_FORMAT_BC1_UNORM, false),
        PixelFormat::DXT2 | PixelFormat::DXT3 if blocks => (DXGI_FORMAT_BC2_UNORM, false),
        PixelFormat::DXT4 | PixelFormat::DXT5 if blocks => (DXGI_FORMAT_BC3_UNORM, false),
        _ => (DXGI_FORMAT_R8G8B8A8_UNORM, true),
    }
}

fn topology(p: Primitive) -> D3D_PRIMITIVE_TOPOLOGY {
    match p {
        Primitive::PointList => D3D_PRIMITIVE_TOPOLOGY_POINTLIST,
        Primitive::LineList => D3D_PRIMITIVE_TOPOLOGY_LINELIST,
        Primitive::LineStrip => D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,
        Primitive::TriangleStrip => D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
        // Fans arrive as lists.
        Primitive::TriangleList | Primitive::TriangleFan => D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
    }
}

fn rect(x: u32, y: u32, width: u32, height: u32) -> RECT {
    RECT {
        left: x as i32,
        top: y as i32,
        right: (x + width) as i32,
        bottom: (y + height) as i32,
    }
}

fn barrier(
    gpu: &Gpu,
    resource: &ID3D12Resource,
    state: &mut D3D12_RESOURCE_STATES,
    want: D3D12_RESOURCE_STATES,
) {
    if *state != want {
        let b = transition(resource, *state, want);
        // SAFETY: the barrier names a live resource in its tracked state.
        unsafe { gpu.list.ResourceBarrier(std::slice::from_ref(&b)) };
        release_barrier(b);
        *state = want;
    }
}

/// A descriptor heap and its handle size.
struct Heap {
    heap: ID3D12DescriptorHeap,
    step: usize,
}

impl Heap {
    fn new(gpu: &Gpu, desc: &D3D12_DESCRIPTOR_HEAP_DESC) -> Result<Heap, GpuError> {
        // SAFETY: a fully initialised description.
        let heap: ID3D12DescriptorHeap =
            unsafe { gpu.device.CreateDescriptorHeap(desc) }.at("CreateDescriptorHeap")?;
        // SAFETY: a plain query.
        let step = unsafe { gpu.device.GetDescriptorHandleIncrementSize(desc.Type) } as usize;
        Ok(Heap { heap, step })
    }

    fn cpu(&self, slot: u32) -> D3D12_CPU_DESCRIPTOR_HANDLE {
        // SAFETY: the heap is live.
        let base = unsafe { self.heap.GetCPUDescriptorHandleForHeapStart() };
        D3D12_CPU_DESCRIPTOR_HANDLE {
            ptr: base.ptr + slot as usize * self.step,
        }
    }

    fn gpu(&self, slot: u32) -> D3D12_GPU_DESCRIPTOR_HANDLE {
        // SAFETY: the heap is live and shader visible.
        let base = unsafe { self.heap.GetGPUDescriptorHandleForHeapStart() };
        D3D12_GPU_DESCRIPTOR_HANDLE {
            ptr: base.ptr + slot as u64 * self.step as u64,
        }
    }
}

/// The renderer's Direct3D 12 backend.
pub struct D3d12Backend {
    gpu: Gpu,
    pipelines: PipelineCache,
    upload: UploadRing,
    target: RenderTarget,
    /// The depth buffer behind `dsv`, kept alive with its view.
    _depth: ID3D12Resource,
    dsv: Heap,
    /// Texture views (CPU side): the null 2D view in slot 0, the null cube
    /// view in slot 1.
    srvs: Heap,
    free_slots: Vec<u32>,
    /// The shader-visible ring the draws copy their four views into (the
    /// two stages' 2D views, then their cube views).
    draw_views: Heap,
    next_view: u32,
    /// The fixed sampler set, shader visible, made once.
    samplers: Heap,
    defaults: ID3D12Resource,
    vbs: HashMap<VbHandle, GpuBuffer>,
    ibs: HashMap<IndexSource, GpuBuffer>,
    textures: HashMap<TextureHandle, GpuTexture>,
    /// Resources the open frame may still use.
    garbage: Vec<ID3D12Resource>,
    recording: bool,
    /// The ramp the final blit applies, and whether the ramp texture is
    /// behind it.
    gamma: GammaRamp,
    gamma_dirty: bool,
    blit: Blit,
    /// The scene target's view, for the blit.
    scene_slot: u32,
    scene_state: D3D12_RESOURCE_STATES,
    ramp: ID3D12Resource,
    ramp_slot: u32,
    ramp_state: D3D12_RESOURCE_STATES,
    swap: Option<(SwapChain, Heap)>,
    /// Calls that failed, in order.
    pub errors: Vec<GpuError>,
}

impl D3d12Backend {
    /// A backend drawing into a `width` by `height` scene on `adapter`.
    pub fn new(adapter: AdapterChoice, width: u32, height: u32) -> Result<D3d12Backend, GpuError> {
        let gpu = Gpu::new(adapter)?;
        let pipelines = PipelineCache::new(&gpu.device, FIXED_FUNCTION)?;
        let target = RenderTarget::new(&gpu, width, height)?;
        let depth = depth_buffer(&gpu, width, height)?;
        let dsv = Heap::new(
            &gpu,
            &D3D12_DESCRIPTOR_HEAP_DESC {
                Type: D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                NumDescriptors: 1,
                ..Default::default()
            },
        )?;
        // SAFETY: the view slot belongs to `dsv`, kept with the depth
        // buffer; a default view of a depth format.
        unsafe { gpu.device.CreateDepthStencilView(&depth, None, dsv.cpu(0)) };
        let srvs = Heap::new(
            &gpu,
            &D3D12_DESCRIPTOR_HEAP_DESC {
                Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                NumDescriptors: TEXTURE_SLOTS,
                ..Default::default()
            },
        )?;
        let null = srv_desc(DXGI_FORMAT_B8G8R8A8_UNORM, 1);
        let null_cube = cube_srv_desc(DXGI_FORMAT_B8G8R8A8_UNORM);
        // SAFETY: null views of a 2D texture and a cube in slots 0 and 1 of
        // the heap.
        unsafe {
            gpu.device
                .CreateShaderResourceView(None, Some(&null), srvs.cpu(0));
            gpu.device
                .CreateShaderResourceView(None, Some(&null_cube), srvs.cpu(NULL_CUBE));
        }
        let draw_views = Heap::new(
            &gpu,
            &D3D12_DESCRIPTOR_HEAP_DESC {
                Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                NumDescriptors: DRAW_SLOTS,
                Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                ..Default::default()
            },
        )?;
        let samplers = Heap::new(
            &gpu,
            &D3D12_DESCRIPTOR_HEAP_DESC {
                Type: D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                NumDescriptors: crate::render::shader::samplers::COUNT,
                Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                ..Default::default()
            },
        )?;
        for (i, desc) in sampler_descs().iter().enumerate() {
            // SAFETY: slot `i` belongs to the sampler heap kept with the
            // backend.
            unsafe { gpu.device.CreateSampler(desc, samplers.cpu(i as u32)) };
        }
        let defaults = buffer(
            &gpu.device,
            D3D12_HEAP_TYPE_UPLOAD,
            DEFAULTS.len() as u64,
            D3D12_RESOURCE_STATE_GENERIC_READ,
        )?;
        write_upload(&defaults, &DEFAULTS)?;
        let blit = Blit::new(&gpu.device)?;
        let mut free_slots: Vec<u32> = (NULL_CUBE + 1..TEXTURE_SLOTS).rev().collect();
        let scene_slot = free_slots.pop().expect("the heap has slots");
        let scene_view = srv_desc(DXGI_FORMAT_B8G8R8A8_UNORM, 1);
        // SAFETY: a view of the scene target in a slot of the view heap,
        // both kept with the backend.
        unsafe {
            gpu.device.CreateShaderResourceView(
                &target.resource,
                Some(&scene_view),
                srvs.cpu(scene_slot),
            );
        }
        let ramp = committed_texture(
            &gpu,
            &D3D12_RESOURCE_DESC {
                Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                Width: 256,
                Height: 1,
                DepthOrArraySize: 1,
                MipLevels: 1,
                Format: DXGI_FORMAT_R16G16B16A16_UNORM,
                SampleDesc: DXGI_SAMPLE_DESC {
                    Count: 1,
                    Quality: 0,
                },
                Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
                ..Default::default()
            },
            D3D12_RESOURCE_STATE_COMMON,
        )?;
        let ramp_slot = free_slots.pop().expect("the heap has slots");
        let ramp_view = srv_desc(DXGI_FORMAT_R16G16B16A16_UNORM, 1);
        // SAFETY: as above, for the ramp texture.
        unsafe {
            gpu.device
                .CreateShaderResourceView(&ramp, Some(&ramp_view), srvs.cpu(ramp_slot));
        }
        Ok(D3d12Backend {
            gpu,
            pipelines,
            upload: UploadRing::default(),
            target,
            _depth: depth,
            dsv,
            srvs,
            free_slots,
            draw_views,
            next_view: 0,
            samplers,
            defaults,
            vbs: HashMap::new(),
            ibs: HashMap::new(),
            textures: HashMap::new(),
            garbage: Vec::new(),
            recording: false,
            gamma: GammaRamp::default(),
            gamma_dirty: true,
            blit,
            scene_slot,
            scene_state: D3D12_RESOURCE_STATE_RENDER_TARGET,
            ramp,
            ramp_slot,
            ramp_state: D3D12_RESOURCE_STATE_COMMON,
            swap: None,
            errors: Vec::new(),
        })
    }

    /// The pipeline states made so far.
    #[must_use]
    pub fn pipeline_count(&self) -> usize {
        self.pipelines.len()
    }

    /// The debug layer's messages, when the layer is on.
    #[must_use]
    pub fn debug_messages(&self) -> Vec<String> {
        self.gpu.debug_messages()
    }

    fn fail(&mut self, result: Result<(), GpuError>) {
        if let Err(e) = result {
            self.errors.push(e);
        }
    }

    /// Opens the frame's command list if it is not open.
    fn open(&mut self) -> Result<(), GpuError> {
        if !self.recording {
            self.gpu.begin()?;
            self.recording = true;
            // SAFETY: the heap is live; the list is open.
            unsafe {
                self.gpu.list.SetDescriptorHeaps(&[
                    Some(self.draw_views.heap.clone()),
                    Some(self.samplers.heap.clone()),
                ]);
            }
        }
        Ok(())
    }

    /// Runs the frame and waits; then the ring, the views and the garbage
    /// are free again.
    pub fn flush(&mut self) -> Result<(), GpuError> {
        if self.recording {
            self.recording = false;
            self.gpu.finish()?;
        }
        self.upload.reset();
        self.next_view = 0;
        self.garbage.clear();
        Ok(())
    }

    /// Presents into `window` from now on, through a swap chain of the
    /// scene's size.
    pub fn attach_window(&mut self, window: HWND) -> Result<(), GpuError> {
        self.flush()?;
        let swap = SwapChain::new(&self.gpu, window, self.target.width, self.target.height)?;
        let rtvs = Heap::new(
            &self.gpu,
            &D3D12_DESCRIPTOR_HEAP_DESC {
                Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                NumDescriptors: swap.buffers.len() as u32,
                ..Default::default()
            },
        )?;
        for (i, buffer) in swap.buffers.iter().enumerate() {
            // SAFETY: slot `i` of the heap kept with the swap chain.
            unsafe {
                self.gpu
                    .device
                    .CreateRenderTargetView(buffer, None, rtvs.cpu(i as u32));
            }
        }
        self.swap = Some((swap, rtvs));
        Ok(())
    }

    /// A new back-buffer size, as `CreateBuffers` after a mode change
    /// makes one: the scene target, its depth buffer and both views, and
    /// the swap chain's buffers are made again at `width` by `height`.
    /// The scene's contents are lost, as a Direct3D 8 `Reset` loses them;
    /// resources, pipelines and samplers are kept. A window resized
    /// without a new size needs nothing: the flip-model swap chain
    /// stretches the back buffer to the client area, as a windowed
    /// Direct3D 8 `Present` did.
    pub fn resize(&mut self, width: u32, height: u32) -> Result<(), GpuError> {
        self.flush()?;
        let target = RenderTarget::new(&self.gpu, width, height)?;
        let depth = depth_buffer(&self.gpu, width, height)?;
        let scene_view = srv_desc(DXGI_FORMAT_B8G8R8A8_UNORM, 1);
        // SAFETY: the flush above finished every use of the old views and
        // resources; the slots belong to heaps kept with the backend, and
        // the new resources are kept with them below.
        unsafe {
            self.gpu
                .device
                .CreateDepthStencilView(&depth, None, self.dsv.cpu(0));
            self.gpu.device.CreateShaderResourceView(
                &target.resource,
                Some(&scene_view),
                self.srvs.cpu(self.scene_slot),
            );
        }
        self.target = target;
        self._depth = depth;
        self.scene_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if let Some((mut swap, rtvs)) = self.swap.take() {
            swap.resize(width, height)?;
            for (i, buffer) in swap.buffers.iter().enumerate() {
                // SAFETY: slot `i` of the heap kept with the swap chain;
                // the old views named buffers released by the resize.
                unsafe {
                    self.gpu
                        .device
                        .CreateRenderTargetView(buffer, None, rtvs.cpu(i as u32));
                }
            }
            self.swap = Some((swap, rtvs));
        }
        Ok(())
    }

    /// An offscreen target of the scene's size, for
    /// [`D3d12Backend::blit_to_target`].
    pub fn new_target(&self) -> Result<RenderTarget, GpuError> {
        RenderTarget::new(&self.gpu, self.target.width, self.target.height)
    }

    /// Records the final blit into `target` (of the scene's size).
    pub fn blit_to_target(&mut self, target: &RenderTarget) -> Result<(), GpuError> {
        self.blit_to(
            &target.resource,
            target.view,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
        )
    }

    /// Runs the frame and reads `target` back.
    pub fn read_target(&mut self, target: &RenderTarget) -> Result<Vec<u8>, GpuError> {
        self.flush()?;
        target.read_back(&mut self.gpu)
    }

    /// The final blit: the scene through the gamma ramp into `target`,
    /// which is in `before` and is left in `after`.
    fn blit_to(
        &mut self,
        target: &ID3D12Resource,
        rtv: D3D12_CPU_DESCRIPTOR_HANDLE,
        before: D3D12_RESOURCE_STATES,
        after: D3D12_RESOURCE_STATES,
    ) -> Result<(), GpuError> {
        self.open()?;
        if self.next_view + 2 > DRAW_SLOTS {
            self.flush()?;
            self.open()?;
        }
        if self.gamma_dirty {
            let texels = ramp_texels(&self.gamma);
            let alloc = self.upload.push(
                &self.gpu.device,
                &texels,
                u64::from(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),
            )?;
            barrier(
                &self.gpu,
                &self.ramp,
                &mut self.ramp_state,
                D3D12_RESOURCE_STATE_COPY_DEST,
            );
            let footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                Offset: alloc.offset,
                Footprint: D3D12_SUBRESOURCE_FOOTPRINT {
                    Format: DXGI_FORMAT_R16G16B16A16_UNORM,
                    Width: 256,
                    Height: 1,
                    Depth: 1,
                    RowPitch: 256 * 8,
                },
            };
            copy_to_texture(&self.gpu, &self.ramp, 0, &alloc.resource, footprint);
            self.gamma_dirty = false;
        }
        barrier(
            &self.gpu,
            &self.ramp,
            &mut self.ramp_state,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        );
        let scene = self.target.resource.clone();
        barrier(
            &self.gpu,
            &scene,
            &mut self.scene_state,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        );
        let base = self.next_view;
        self.next_view += 2;
        let device = self.gpu.device.clone();
        // SAFETY: the source slots hold live views; the destination slots
        // are this blit's in the shader-visible ring.
        unsafe {
            device.CopyDescriptorsSimple(
                1,
                self.draw_views.cpu(base),
                self.srvs.cpu(self.scene_slot),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            );
            device.CopyDescriptorsSimple(
                1,
                self.draw_views.cpu(base + 1),
                self.srvs.cpu(self.ramp_slot),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            );
        }
        let mut state = before;
        barrier(
            &self.gpu,
            target,
            &mut state,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
        );
        let (w, h) = (self.target.width, self.target.height);
        let list = self.gpu.list.clone();
        // SAFETY: everything bound lives until the frame is flushed.
        unsafe {
            list.SetGraphicsRootSignature(&self.blit.root);
            list.SetPipelineState(&self.blit.state);
            list.SetGraphicsRootDescriptorTable(0, self.draw_views.gpu(base));
            list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list.RSSetViewports(&[D3D12_VIEWPORT {
                TopLeftX: 0.0,
                TopLeftY: 0.0,
                Width: w as f32,
                Height: h as f32,
                MinDepth: 0.0,
                MaxDepth: 1.0,
            }]);
            list.RSSetScissorRects(&[rect(0, 0, w, h)]);
            list.OMSetRenderTargets(1, Some(&rtv), false, None);
            list.DrawInstanced(3, 1, 0, 0);
        }
        barrier(&self.gpu, target, &mut state, after);
        barrier(
            &self.gpu,
            &scene,
            &mut self.scene_state,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
        );
        Ok(())
    }

    /// Blits into the swap chain's back buffer, runs the frame and
    /// presents; without a window, runs the frame.
    fn try_present(&mut self) -> Result<(), GpuError> {
        let Some((swap, rtvs)) = self.swap.take() else {
            return self.flush();
        };
        let index = swap.current();
        let buffer = swap.buffers[index as usize].clone();
        let result = self
            .blit_to(
                &buffer,
                rtvs.cpu(index),
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_PRESENT,
            )
            .and_then(|()| self.flush())
            .and_then(|()| swap.present());
        self.swap = Some((swap, rtvs));
        result
    }

    /// Runs the frame and reads the scene back: BGRA8, rows unpadded.
    pub fn read_back(&mut self) -> Result<Vec<u8>, GpuError> {
        self.flush()?;
        self.target.read_back(&mut self.gpu)
    }

    fn vertices_changed(
        &mut self,
        buffer_handle: VbHandle,
        size: usize,
        recreated: bool,
        offset: usize,
        bytes: &[u8],
    ) -> Result<(), GpuError> {
        let replace = recreated
            || self
                .vbs
                .get(&buffer_handle)
                .is_none_or(|b| b.size != size as u64);
        if replace {
            if let Some(old) = self.vbs.remove(&buffer_handle) {
                self.garbage.push(old.resource);
            }
            if size == 0 {
                return Ok(());
            }
            let resource = buffer(
                &self.gpu.device,
                D3D12_HEAP_TYPE_DEFAULT,
                size as u64,
                DEFAULT_BUFFER_STATE,
            )?;
            self.vbs.insert(
                buffer_handle,
                GpuBuffer {
                    resource,
                    size: size as u64,
                    state: D3D12_RESOURCE_STATE_COMMON,
                },
            );
        }
        if bytes.is_empty() {
            return Ok(());
        }
        self.open()?;
        let src = self.upload.push(&self.gpu.device, bytes, 16)?;
        let dst = self.vbs.get_mut(&buffer_handle).expect("made above");
        copy_buffer(
            &self.gpu,
            dst,
            offset as u64,
            &src.resource,
            src.offset,
            bytes.len() as u64,
        );
        Ok(())
    }

    fn indices_changed(
        &mut self,
        source: IndexSource,
        count: usize,
        recreated: bool,
        offset: usize,
        indices: &[u16],
    ) -> Result<(), GpuError> {
        let size = count as u64 * 2;
        let replace = recreated || self.ibs.get(&source).is_none_or(|b| b.size != size);
        if replace {
            if let Some(old) = self.ibs.remove(&source) {
                self.garbage.push(old.resource);
            }
            if size == 0 {
                return Ok(());
            }
            let resource = buffer(
                &self.gpu.device,
                D3D12_HEAP_TYPE_DEFAULT,
                size,
                DEFAULT_BUFFER_STATE,
            )?;
            self.ibs.insert(
                source,
                GpuBuffer {
                    resource,
                    size,
                    state: D3D12_RESOURCE_STATE_COMMON,
                },
            );
        }
        if indices.is_empty() {
            return Ok(());
        }
        self.open()?;
        let bytes: Vec<u8> = indices.iter().flat_map(|i| i.to_le_bytes()).collect();
        let src = self.upload.push(&self.gpu.device, &bytes, 16)?;
        let dst = self.ibs.get_mut(&source).expect("made above");
        copy_buffer(
            &self.gpu,
            dst,
            offset as u64 * 2,
            &src.resource,
            src.offset,
            bytes.len() as u64,
        );
        Ok(())
    }

    fn release_texture(&mut self, h: TextureHandle) {
        if let Some(t) = self.textures.remove(&h) {
            self.free_slots.push(t.slot);
            self.garbage.push(t.resource);
        }
    }

    fn texture_level(
        &mut self,
        h: TextureHandle,
        desc: &Texture,
        created: bool,
        level: u32,
        bytes: &[u8],
    ) -> Result<(), GpuError> {
        if created {
            self.release_texture(h);
            let (format, convert) = texture_format(desc);
            let resource = texture_resource(&self.gpu, desc, format)?;
            let slot = self.free_slots.pop().ok_or(GpuError {
                call: "texture views exhausted",
                code: 0,
            })?;
            let view = if desc.cube {
                cube_srv_desc(format)
            } else {
                srv_desc(format, desc.level_count())
            };
            // SAFETY: the slot belongs to the texture-view heap and to this
            // texture until it is released.
            unsafe {
                self.gpu.device.CreateShaderResourceView(
                    &resource,
                    Some(&view),
                    self.srvs.cpu(slot),
                );
            }
            self.textures.insert(
                h,
                GpuTexture {
                    resource,
                    convert,
                    levels: desc.level_count(),
                    cube: desc.cube,
                    slot,
                    state: D3D12_RESOURCE_STATE_COMMON,
                },
            );
        }
        let Some(layout) = desc.layout(level) else {
            return Ok(());
        };
        let Some(t) = self.textures.get(&h) else {
            return Ok(());
        };
        if level >= t.levels {
            return Ok(());
        }
        // The rows to copy: the level as stored, or converted to RGBA8.
        let converted;
        let (src, src_pitch, rows) = if t.convert {
            converted = to_rgba8(
                desc.format,
                layout.width,
                layout.height,
                layout.pitch,
                bytes,
                None,
            )
            .unwrap_or_else(|_| vec![0; (layout.width * layout.height * 4) as usize]);
            (&converted[..], layout.width as usize * 4, layout.height)
        } else {
            (bytes, layout.pitch as usize, layout.rows)
        };
        let mut footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default();
        let mut num_rows = 0u32;
        let mut row_bytes = 0u64;
        let mut total = 0u64;
        // SAFETY: a plain query of the texture's own description, with
        // valid out pointers for one subresource.
        unsafe {
            let tdesc = t.resource.GetDesc();
            self.gpu.device.GetCopyableFootprints(
                &tdesc,
                level,
                1,
                0,
                Some(&mut footprint),
                Some(&mut num_rows),
                Some(&mut row_bytes),
                Some(&mut total),
            );
        }
        let pitch = footprint.Footprint.RowPitch as usize;
        let mut staging = vec![0u8; total as usize];
        let copy = (row_bytes as usize).min(src_pitch);
        for row in 0..(num_rows.min(rows)) as usize {
            let from = row * src_pitch;
            if let Some(line) = src.get(from..from + copy) {
                staging[row * pitch..row * pitch + copy].copy_from_slice(line);
            }
        }
        self.open()?;
        let alloc = self.upload.push(
            &self.gpu.device,
            &staging,
            u64::from(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),
        )?;
        footprint.Offset = alloc.offset;
        let t = self.textures.get_mut(&h).expect("looked up above");
        barrier(
            &self.gpu,
            &t.resource,
            &mut t.state,
            D3D12_RESOURCE_STATE_COPY_DEST,
        );
        copy_to_texture(&self.gpu, &t.resource, level, &alloc.resource, footprint);
        Ok(())
    }

    fn try_draw(&mut self, p: &DrawPacket) -> Result<(), GpuError> {
        self.open()?;
        if self.next_view + 4 > DRAW_SLOTS {
            self.flush()?;
            self.open()?;
        }
        let device = self.gpu.device.clone();
        let pso = self.pipelines.get(&device, &p.key)?.clone();
        let list = self.gpu.list.clone();

        // Slot 0: the vertices; slot 1: the defaults vertex, stride 0.
        let (vbv, first) = match &p.vertices {
            VertexData::Bytes(bytes) => {
                let a = self.upload.push(&device, bytes, 16)?;
                (
                    D3D12_VERTEX_BUFFER_VIEW {
                        BufferLocation: a.gpu_address,
                        SizeInBytes: bytes.len() as u32,
                        StrideInBytes: p.stride,
                    },
                    0,
                )
            }
            VertexData::Buffer {
                buffer: h,
                first_vertex,
            } => {
                let Some(b) = self.vbs.get_mut(h) else {
                    return Err(GpuError {
                        call: "draw: vertex buffer not on the GPU",
                        code: 0,
                    });
                };
                barrier(
                    &self.gpu,
                    &b.resource,
                    &mut b.state,
                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                );
                (
                    D3D12_VERTEX_BUFFER_VIEW {
                        // SAFETY: a live buffer.
                        BufferLocation: unsafe { b.resource.GetGPUVirtualAddress() },
                        SizeInBytes: b.size as u32,
                        StrideInBytes: p.stride,
                    },
                    *first_vertex,
                )
            }
        };
        let defaults = D3D12_VERTEX_BUFFER_VIEW {
            // SAFETY: a live buffer.
            BufferLocation: unsafe { self.defaults.GetGPUVirtualAddress() },
            SizeInBytes: DEFAULTS.len() as u32,
            StrideInBytes: 0,
        };

        // Indices: none, uploaded, or a GPU index buffer.
        let index = match &p.indices {
            IndexData::None => None,
            IndexData::List(list16) => {
                let bytes: Vec<u8> = list16.iter().flat_map(|i| i.to_le_bytes()).collect();
                let a = self.upload.push(&device, &bytes, 4)?;
                Some((
                    D3D12_INDEX_BUFFER_VIEW {
                        BufferLocation: a.gpu_address,
                        SizeInBytes: bytes.len() as u32,
                        Format: DXGI_FORMAT_R16_UINT,
                    },
                    list16.len() as u32,
                    0,
                    first,
                ))
            }
            IndexData::Buffer {
                source,
                start,
                count,
                base_vertex,
            } => {
                let Some(b) = self.ibs.get_mut(source) else {
                    return Err(GpuError {
                        call: "draw: index buffer not on the GPU",
                        code: 0,
                    });
                };
                barrier(
                    &self.gpu,
                    &b.resource,
                    &mut b.state,
                    D3D12_RESOURCE_STATE_INDEX_BUFFER,
                );
                Some((
                    D3D12_INDEX_BUFFER_VIEW {
                        // SAFETY: a live buffer.
                        BufferLocation: unsafe { b.resource.GetGPUVirtualAddress() },
                        SizeInBytes: b.size as u32,
                        Format: DXGI_FORMAT_R16_UINT,
                    },
                    *count,
                    *start,
                    base_vertex.wrapping_add(first),
                ))
            }
        };

        // The stage textures: a 2D view and a cube view per stage, the
        // texture's own where it is of that kind, else the null view.
        let mut bound = [false; 2];
        let mut cube = [false; 2];
        let base = self.next_view;
        for (stage, texture) in p.textures.iter().enumerate() {
            let mut flat = self.srvs.cpu(0);
            let mut cubed = self.srvs.cpu(NULL_CUBE);
            if let Some(t) = texture.and_then(|h| self.textures.get_mut(&h)) {
                barrier(
                    &self.gpu,
                    &t.resource,
                    &mut t.state,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                );
                if t.cube {
                    cubed = self.srvs.cpu(t.slot);
                } else {
                    flat = self.srvs.cpu(t.slot);
                }
                bound[stage] = true;
                cube[stage] = t.cube;
            }
            for (offset, src) in [(0, flat), (2, cubed)] {
                // SAFETY: both handles name slots of live heaps of the same
                // type; the destination slot is this draw's.
                unsafe {
                    device.CopyDescriptorsSimple(
                        1,
                        self.draw_views.cpu(base + offset + stage as u32),
                        src,
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    );
                }
            }
        }
        self.next_view += 4;

        let vp = p.viewport;
        let constants = pack(
            &p.constants,
            p.fvf,
            bound,
            cube,
            ViewportRect {
                x: vp.x as f32,
                y: vp.y as f32,
                width: vp.width as f32,
                height: vp.height as f32,
            },
        );
        let cb = self.upload.push(
            &device,
            &constants,
            u64::from(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT),
        )?;

        let rtv = self.target.view;
        let dsv = self.dsv.cpu(0);
        // SAFETY: every object the list is given lives until the frame is
        // flushed: the pipeline state in the cache, the views in their
        // heaps, the buffers in the maps or the garbage, the uploads in the
        // ring.
        unsafe {
            list.SetGraphicsRootSignature(self.pipelines.root_signature());
            list.SetPipelineState(&pso);
            list.SetGraphicsRootConstantBufferView(ROOT_CONSTANTS, cb.gpu_address);
            list.SetGraphicsRootDescriptorTable(ROOT_TEXTURES, self.draw_views.gpu(base));
            list.SetGraphicsRootDescriptorTable(ROOT_SAMPLERS, self.samplers.gpu(0));
            list.IASetPrimitiveTopology(topology(p.primitive));
            list.IASetVertexBuffers(0, Some(&[vbv, defaults]));
            list.RSSetViewports(&[D3D12_VIEWPORT {
                TopLeftX: vp.x as f32,
                TopLeftY: vp.y as f32,
                Width: vp.width as f32,
                Height: vp.height as f32,
                MinDepth: vp.min_z,
                MaxDepth: vp.max_z,
            }]);
            list.RSSetScissorRects(&[rect(vp.x, vp.y, vp.width, vp.height)]);
            list.OMSetRenderTargets(1, Some(&rtv), false, Some(&dsv));
            match index {
                None => list.DrawInstanced(p.vertex_count, 1, first, 0),
                Some((ibv, count, start, base_vertex)) => {
                    list.IASetIndexBuffer(Some(&ibv));
                    list.DrawIndexedInstanced(count, 1, start, base_vertex.cast_signed(), 0);
                }
            }
        }
        Ok(())
    }

    fn try_clear(&mut self, clear: Clear, viewport: Viewport) -> Result<(), GpuError> {
        self.open()?;
        let r = match clear.rect {
            Some([x0, y0, x1, y1]) => RECT {
                left: x0,
                top: y0,
                right: x1,
                bottom: y1,
            },
            None => rect(viewport.x, viewport.y, viewport.width, viewport.height),
        };
        let f = clear.flags.0;
        if f & ClearFlags::TARGET != 0 {
            let c = clear.color;
            let n = |shift: u32| f32::from((c >> shift) as u8) / 255.0;
            let rgba = [n(16), n(8), n(0), n(24)];
            // SAFETY: the scene target's view, in the render-target state.
            unsafe {
                self.gpu
                    .list
                    .ClearRenderTargetView(self.target.view, &rgba, Some(&[r]));
            }
        }
        let mut depth_flags = D3D12_CLEAR_FLAGS(0);
        if f & ClearFlags::DEPTH != 0 {
            depth_flags |= D3D12_CLEAR_FLAG_DEPTH;
        }
        if f & ClearFlags::STENCIL != 0 {
            depth_flags |= D3D12_CLEAR_FLAG_STENCIL;
        }
        if depth_flags.0 != 0 {
            // SAFETY: the depth buffer's view, in the depth-write state.
            unsafe {
                self.gpu.list.ClearDepthStencilView(
                    self.dsv.cpu(0),
                    depth_flags,
                    clear.depth,
                    clear.stencil as u8,
                    Some(&[r]),
                );
            }
        }
        Ok(())
    }
}

impl Backend for D3d12Backend {
    fn resource(&mut self, event: ResourceEvent<'_>) {
        let result = match event {
            ResourceEvent::Buffer(BufferEvent::Vertices {
                buffer,
                size,
                recreated,
                offset,
                bytes,
            }) => self.vertices_changed(buffer, size, recreated, offset, bytes),
            ResourceEvent::Buffer(BufferEvent::Indices {
                source,
                count,
                recreated,
                offset,
                indices,
            }) => self.indices_changed(source, count, recreated, offset, indices),
            ResourceEvent::Buffer(BufferEvent::ReleaseVertices(h)) => {
                if let Some(b) = self.vbs.remove(&h) {
                    self.garbage.push(b.resource);
                }
                Ok(())
            }
            ResourceEvent::Buffer(BufferEvent::ReleaseIndices(h)) => {
                if let Some(b) = self.ibs.remove(&IndexSource::Buffer(h)) {
                    self.garbage.push(b.resource);
                }
                Ok(())
            }
            ResourceEvent::Texture(TextureEvent::Level {
                texture,
                desc,
                created,
                level,
                bytes,
            }) => self.texture_level(texture, desc, created, level, bytes),
            ResourceEvent::Texture(TextureEvent::Release(h)) => {
                self.release_texture(h);
                Ok(())
            }
        };
        self.fail(result);
    }

    fn gamma(&mut self, ramp: &GammaRamp) {
        self.gamma = ramp.clone();
        self.gamma_dirty = true;
    }

    fn read_scene(&mut self) -> Option<(u32, u32, Vec<u8>)> {
        match self.read_back() {
            Ok(pixels) => Some((self.target.width, self.target.height, pixels)),
            Err(e) => {
                self.errors.push(e);
                None
            }
        }
    }

    fn draw(&mut self, packet: DrawPacket) {
        let result = self.try_draw(&packet);
        self.fail(result);
    }

    fn clear(&mut self, clear: Clear, viewport: Viewport) {
        let result = self.try_clear(clear, viewport);
        self.fail(result);
    }

    fn present(&mut self) {
        let result = self.try_present();
        self.fail(result);
    }
}

fn copy_buffer(
    gpu: &Gpu,
    dst: &mut GpuBuffer,
    dst_offset: u64,
    src: &ID3D12Resource,
    src_offset: u64,
    len: u64,
) {
    barrier(
        gpu,
        &dst.resource,
        &mut dst.state,
        D3D12_RESOURCE_STATE_COPY_DEST,
    );
    // SAFETY: both buffers are live; the ranges lie inside them (the
    // destination range came from the model's buffer of the same size).
    unsafe {
        gpu.list
            .CopyBufferRegion(&dst.resource, dst_offset, src, src_offset, len);
    }
}

/// Records a copy of `footprint` in `src` into subresource `level` of
/// `dst`, which must be in the copy-destination state.
fn copy_to_texture(
    gpu: &Gpu,
    dst: &ID3D12Resource,
    level: u32,
    src: &ID3D12Resource,
    footprint: D3D12_PLACED_SUBRESOURCE_FOOTPRINT,
) {
    let dst = D3D12_TEXTURE_COPY_LOCATION {
        pResource: std::mem::ManuallyDrop::new(Some(dst.clone())),
        Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
            SubresourceIndex: level,
        },
    };
    let src = D3D12_TEXTURE_COPY_LOCATION {
        pResource: std::mem::ManuallyDrop::new(Some(src.clone())),
        Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
            PlacedFootprint: footprint,
        },
    };
    // SAFETY: both locations name live resources; the footprint lies inside
    // the source allocation and fits the destination subresource.
    unsafe { gpu.list.CopyTextureRegion(&dst, 0, 0, 0, &src, None) };
    let mut dst = dst;
    let mut src = src;
    // SAFETY: each location's resource reference is dropped once.
    unsafe {
        std::mem::ManuallyDrop::drop(&mut dst.pResource);
        std::mem::ManuallyDrop::drop(&mut src.pResource);
    }
}

fn write_upload(resource: &ID3D12Resource, bytes: &[u8]) -> Result<(), GpuError> {
    let mut data: *mut std::ffi::c_void = std::ptr::null_mut();
    let none = D3D12_RANGE { Begin: 0, End: 0 };
    // SAFETY: an upload buffer at least `bytes.len()` long, written once
    // before the GPU reads it.
    unsafe {
        resource.Map(0, Some(&none), Some(&mut data)).at("Map")?;
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), data.cast::<u8>(), bytes.len());
        resource.Unmap(0, None);
    }
    Ok(())
}

fn srv_desc(format: DXGI_FORMAT, levels: u32) -> D3D12_SHADER_RESOURCE_VIEW_DESC {
    D3D12_SHADER_RESOURCE_VIEW_DESC {
        Format: format,
        ViewDimension: D3D12_SRV_DIMENSION_TEXTURE2D,
        Shader4ComponentMapping: D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 {
            Texture2D: D3D12_TEX2D_SRV {
                MostDetailedMip: 0,
                MipLevels: levels,
                PlaneSlice: 0,
                ResourceMinLODClamp: 0.0,
            },
        },
    }
}

fn cube_srv_desc(format: DXGI_FORMAT) -> D3D12_SHADER_RESOURCE_VIEW_DESC {
    D3D12_SHADER_RESOURCE_VIEW_DESC {
        Format: format,
        ViewDimension: D3D12_SRV_DIMENSION_TEXTURECUBE,
        Shader4ComponentMapping: D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 {
            TextureCube: D3D12_TEXCUBE_SRV {
                MostDetailedMip: 0,
                MipLevels: 1,
                ResourceMinLODClamp: 0.0,
            },
        },
    }
}

fn committed_texture(
    gpu: &Gpu,
    desc: &D3D12_RESOURCE_DESC,
    state: D3D12_RESOURCE_STATES,
) -> Result<ID3D12Resource, GpuError> {
    let heap = D3D12_HEAP_PROPERTIES {
        Type: D3D12_HEAP_TYPE_DEFAULT,
        ..Default::default()
    };
    let mut resource: Option<ID3D12Resource> = None;
    // SAFETY: fully initialised descriptions and a valid out slot.
    unsafe {
        gpu.device.CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            desc,
            state,
            None,
            &mut resource,
        )
    }
    .at("CreateCommittedResource")?;
    resource.ok_or(GpuError {
        call: "CreateCommittedResource",
        code: 0,
    })
}

fn texture_resource(
    gpu: &Gpu,
    t: &Texture,
    format: DXGI_FORMAT,
) -> Result<ID3D12Resource, GpuError> {
    let desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        Width: u64::from(t.width),
        Height: t.height,
        // A cube map's faces are the six slices of one level.
        DepthOrArraySize: if t.cube { 6 } else { 1 },
        MipLevels: if t.cube { 1 } else { t.level_count() as u16 },
        Format: format,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
        ..Default::default()
    };
    committed_texture(gpu, &desc, D3D12_RESOURCE_STATE_COMMON)
}

fn depth_buffer(gpu: &Gpu, width: u32, height: u32) -> Result<ID3D12Resource, GpuError> {
    let desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        Width: u64::from(width),
        Height: height,
        DepthOrArraySize: 1,
        MipLevels: 1,
        Format: DEPTH_FORMAT,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
        Flags: D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        ..Default::default()
    };
    committed_texture(gpu, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::ff::defaults::Defaults;
    use crate::render::ff::device::Device;
    use crate::render::ff::fvf::Fvf;
    use crate::render::ff::state::{PipelineProperty, StageState, TextureFilter, TextureOp};

    const SIZE: u32 = 8;

    /// A device drawing on WARP, or `None` off CI where there is no WARP
    /// device (on CI a missing device fails).
    fn device() -> Option<Device<D3d12Backend>> {
        let layer = super::super::enable_debug_layer();
        println!("debug layer: {layer}");
        match D3d12Backend::new(AdapterChoice::Warp, SIZE, SIZE) {
            Ok(b) => Some(Device::new(Defaults::builtin(), SIZE, SIZE, b)),
            Err(e) if std::env::var_os("GITHUB_ACTIONS").is_some() => {
                panic!("no WARP device on CI: {e}")
            }
            Err(e) => {
                println!("no WARP device here ({e}); nothing to test");
                None
            }
        }
    }

    /// Two clockwise triangles covering the target, as XYZRHW vertices with
    /// the given extra bytes per vertex (colour, coordinates).
    fn quad(extra: impl Fn(f32, f32) -> Vec<u8>) -> Vec<u8> {
        let s = SIZE as f32;
        let corners = [(0.0, 0.0), (s, 0.0), (0.0, s), (s, 0.0), (s, s), (0.0, s)];
        let mut v = Vec::new();
        for (x, y) in corners {
            for f in [x, y, 0.5, 1.0] {
                v.extend_from_slice(&f32::to_le_bytes(f));
            }
            v.extend(extra(x / s, y / s));
        }
        v
    }

    fn pixel(pixels: &[u8], x: u32, y: u32) -> [u8; 4] {
        let at = ((y * SIZE + x) * 4) as usize;
        pixels[at..at + 4].try_into().unwrap()
    }

    /// With the default stage states and no texture, the cascade ends at
    /// stage 0 and the vertex colour shows (Direct3D 9 "Texture Blending").
    #[test]
    fn untextured_quad_shows_its_diffuse_colour() {
        let Some(mut d) = device() else { return };
        d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        // XYZRHW | DIFFUSE: opaque red.
        let v = quad(|_, _| 0xffff_0000_u32.to_le_bytes().to_vec());
        d.draw_up(4, Fvf(0x044), &v, 6).unwrap();
        let pixels = d.backend.read_back().unwrap();
        assert!(
            d.backend.errors.is_empty(),
            "{:?}\n{}",
            d.backend.errors,
            d.backend.debug_messages().join("\n")
        );
        for (x, y) in [(1, 1), (4, 4), (6, 2), (2, 6)] {
            assert_eq!(pixel(&pixels, x, y), [0, 0, 255, 255], "pixel {x},{y}");
        }
        assert_eq!(d.backend.pipeline_count(), 1);
    }

    /// A 2x2 texture over the quad with point sampling: one texel per
    /// quadrant, through the resource events and the upload ring.
    #[test]
    fn textured_quad_samples_its_texels() {
        let Some(mut d) = device() else { return };
        let t = d
            .textures
            .create(2, 2, PixelFormat::A8R8G8B8, 1, 0)
            .unwrap();
        {
            let (layout, bytes) = d.textures.lock(t, 0).unwrap();
            // BGRA: red, green / blue, white.
            let texels = [
                [0, 0, 255, 255],
                [0, 255, 0, 255],
                [255, 0, 0, 255],
                [255; 4],
            ];
            for (i, texel) in texels.iter().enumerate() {
                let at = (i / 2) * layout.pitch as usize + (i % 2) * 4;
                bytes[at..at + 4].copy_from_slice(texel);
            }
        }
        d.textures.unlock(t, 0).unwrap();
        d.set_stage_texture(0, Some(t));
        d.state
            .set_stage_state(0, StageState::ColorOp(TextureOp::SelectArg1));
        d.state
            .set_stage_state(0, StageState::MagFilter(TextureFilter::Point));
        d.state
            .set_stage_state(0, StageState::MinFilter(TextureFilter::Point));
        d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        // XYZRHW | DIFFUSE | TEX1.
        let v = quad(|u, v| {
            let mut e = 0xffff_ffff_u32.to_le_bytes().to_vec();
            e.extend_from_slice(&u.to_le_bytes());
            e.extend_from_slice(&v.to_le_bytes());
            e
        });
        d.draw_up(4, Fvf(0x144), &v, 6).unwrap();
        let pixels = d.backend.read_back().unwrap();
        assert!(
            d.backend.errors.is_empty(),
            "{:?}\n{}",
            d.backend.errors,
            d.backend.debug_messages().join("\n")
        );
        assert_eq!(pixel(&pixels, 1, 1), [0, 0, 255, 255]);
        assert_eq!(pixel(&pixels, 6, 1), [0, 255, 0, 255]);
        assert_eq!(pixel(&pixels, 1, 6), [255, 0, 0, 255]);
        assert_eq!(pixel(&pixels, 6, 6), [255; 4]);
    }

    /// A cube map from `LoadCubemap` is sampled in the direction of the
    /// stage's three coordinates; faces in Direct3D order (+X, -X, +Y, -Y,
    /// +Z, -Z).
    #[test]
    fn cube_maps_sample_the_face_their_direction_points_at() {
        let Some(mut d) = device() else { return };
        // One texel per face, BGRA: a distinct colour each.
        let colours: [[u8; 4]; 6] = [
            [0, 0, 255, 255],
            [0, 255, 0, 255],
            [255, 0, 0, 255],
            [0, 255, 255, 255],
            [255, 0, 255, 255],
            [255, 255, 0, 255],
        ];
        let t = d
            .textures
            .create_cube(1, PixelFormat::A8R8G8B8, colours.map(|c| c.to_vec()))
            .unwrap();
        d.set_stage_texture(0, Some(t));
        d.state
            .set_stage_state(0, StageState::ColorOp(TextureOp::SelectArg1));
        // Point sampling: Direct3D 12 filters across cube faces (seamless,
        // D268), which with one-texel faces would mix in the neighbours.
        d.state
            .set_stage_state(0, StageState::MagFilter(TextureFilter::Point));
        d.state
            .set_stage_state(0, StageState::MinFilter(TextureFilter::Point));
        let directions: [[f32; 3]; 6] = [
            [1.0, 0.1, 0.2],
            [-1.0, 0.2, 0.1],
            [0.1, 1.0, 0.2],
            [0.2, -1.0, 0.1],
            [0.1, 0.2, 1.0],
            [0.2, 0.1, -1.0],
        ];
        for (face, dir) in directions.iter().enumerate() {
            d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
            // XYZRHW | DIFFUSE | TEX1 with three coordinates in set 0.
            let v = quad(|_, _| {
                let mut e = 0xffff_ffff_u32.to_le_bytes().to_vec();
                for c in dir {
                    e.extend_from_slice(&c.to_le_bytes());
                }
                e
            });
            d.draw_up(4, Fvf(0x1_0144), &v, 6).unwrap();
            let pixels = d.backend.read_back().unwrap();
            assert!(
                d.backend.errors.is_empty(),
                "{:?}\n{}",
                d.backend.errors,
                d.backend.debug_messages().join("\n")
            );
            assert_eq!(pixel(&pixels, 4, 4), colours[face], "face {face}");
        }
    }

    /// A new back-buffer size without a window: the scene and depth are
    /// made again and draw at the new size.
    #[test]
    fn the_scene_resizes() {
        let Some(mut d) = device() else { return };
        d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        d.present();
        println!("resizing offscreen");
        d.backend.resize(16, 4).unwrap();
        println!("resized offscreen");
        d.set_viewport(0, 0, 16, 4);
        d.state
            .set_pipeline(PipelineProperty::ClearColor, 0xff00_ff00);
        d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        let pixels = d.backend.read_back().unwrap();
        assert!(
            d.backend.errors.is_empty(),
            "{:?}\n{}",
            d.backend.errors,
            d.backend.debug_messages().join("\n")
        );
        assert_eq!(pixels.len(), 16 * 4 * 4);
        assert!(
            pixels
                .as_chunks::<4>()
                .0
                .iter()
                .all(|p| *p == [0, 255, 0, 255])
        );
    }

    /// The final blit applies the gamma ramp; the scene, and so a
    /// screenshot, keeps the colours as drawn.
    #[test]
    fn the_blit_applies_the_gamma_ramp() {
        let Some(mut d) = device() else { return };
        d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        // Opaque 0x40, 0x80, 0xc0 (red, green, blue).
        let v = quad(|_, _| 0xff40_80c0_u32.to_le_bytes().to_vec());
        d.draw_up(4, Fvf(0x044), &v, 6).unwrap();
        // An inverting ramp: entry i maps to 255 - i in 8 bits.
        let inverted: [u16; 256] = std::array::from_fn(|i| u16::MAX - (i as u16) * 257);
        d.set_gamma_ramp(7, &inverted);
        let out = d.backend.new_target().unwrap();
        d.backend.blit_to_target(&out).unwrap();
        let blitted = d.backend.read_target(&out).unwrap();
        assert!(
            d.backend.errors.is_empty(),
            "{:?}\n{}",
            d.backend.errors,
            d.backend.debug_messages().join("\n")
        );
        // BGRA: blue 0xc0 -> 0x3f, green 0x80 -> 0x7f, red 0x40 -> 0xbf.
        assert_eq!(pixel(&blitted, 4, 4), [0x3f, 0x7f, 0xbf, 255]);
        let bmp = d.screenshot("shot.bmp").unwrap();
        // The BMP's first stored row is the bottom one; its first pixel is
        // BGR of the colour as drawn.
        assert_eq!(&bmp[54..57], &[0xc0, 0x80, 0x40]);
    }

    /// Presenting through a swap chain on a (hidden) window.
    #[test]
    fn frames_present_into_a_window() {
        use windows::Win32::System::LibraryLoader::GetModuleHandleW;
        use windows::Win32::UI::WindowsAndMessaging::{
            CreateWindowExW, DestroyWindow, WINDOW_EX_STYLE, WS_POPUP,
        };
        use windows::core::w;
        let Some(mut d) = device() else { return };
        // SAFETY: a plain query of this module's handle.
        let module = unsafe { GetModuleHandleW(None) }.unwrap();
        // SAFETY: a popup of the predefined STATIC class, never shown,
        // destroyed below.
        let window = unsafe {
            CreateWindowExW(
                WINDOW_EX_STYLE(0),
                w!("STATIC"),
                w!("outpost-test"),
                WS_POPUP,
                0,
                0,
                SIZE as i32,
                SIZE as i32,
                None,
                None,
                Some(module.into()),
                None,
            )
        }
        .unwrap();
        d.backend.attach_window(window).unwrap();
        for _ in 0..3 {
            d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
            d.present();
        }
        // A new back-buffer size: the scene and the swap chain follow.
        println!("resizing with a window");
        d.backend.resize(16, 4).unwrap();
        println!("resized");
        d.set_viewport(0, 0, 16, 4);
        d.state
            .set_pipeline(PipelineProperty::ClearColor, 0xff00_ff00);
        d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        let pixels = d.backend.read_back().unwrap();
        assert_eq!(pixels.len(), 16 * 4 * 4);
        assert!(
            pixels
                .as_chunks::<4>()
                .0
                .iter()
                .all(|p| *p == [0, 255, 0, 255])
        );
        d.present();
        let errors = std::mem::take(&mut d.backend.errors);
        let messages = d.backend.debug_messages().join("\n");
        drop(d);
        // SAFETY: the window made above; the swap chain on it is gone.
        unsafe { DestroyWindow(window) }.unwrap();
        assert!(errors.is_empty(), "{errors:?}\n{messages}");
    }
}
