//! The programmable side of the backend (record 0005, point 8): programs
//! from compiled blobs, render targets made again at every back-buffer
//! change, volume textures, the scene depth sampled through its typeless
//! view, and programmable draws on the pass root signature.
//!
//! A draw carries everything it needs ([`ExDraw`]), so the backend keeps
//! no pass state of its own; [`ExBackend::ex_end_pass`] only moves the
//! depth buffer back to the depth-write state the fixed-function draws
//! expect. Resources move between the render-target and shader-resource
//! states as draws use them.

use std::collections::HashMap;

use windows::Win32::Graphics::Direct3D12::{
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, D3D12_DESCRIPTOR_HEAP_DESC,
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, D3D12_INDEX_BUFFER_VIEW,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT, D3D12_RESOURCE_DESC, D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    D3D12_RESOURCE_DIMENSION_TEXTURE3D, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE,
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET,
    D3D12_RESOURCE_STATES, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_LAYOUT_UNKNOWN,
    D3D12_VERTEX_BUFFER_VIEW, D3D12_VIEWPORT, ID3D12PipelineState, ID3D12Resource,
    ID3D12RootSignature,
};
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_R16_UINT, DXGI_SAMPLE_DESC};

use super::super::pass::{
    ROOT_PIXEL_CONSTANTS, ROOT_SAMPLERS, ROOT_VERTEX_CONSTANTS, ROOT_VIEWS, dxgi_format,
    pipeline_state, root_signature, sampled_mapping, sampler_desc, view_2d, view_3d,
};
use super::super::{Gpu, GpuError, read_texture};
use super::{
    D3d12Backend, DRAW_SLOTS, EX_SAMPLER_RING, Heap, barrier, committed_texture, copy_to_texture,
    rect, topology,
};
use crate::render::pass::{
    ExBackend, ExBinding, ExCaps, ExCreate, ExDraw, ExFormat, ExPipelineKey, ExProgram, ExRelease,
    ExTexture, PassTarget, RenderTargetDesc, SamplerState, TEXTURE_SLOTS, TargetSize,
};
use crate::render::shader::input::DEFAULTS;
use crate::render::shader::samplers;

/// A render target with its own view.
pub(super) struct ExTargetGpu {
    desc: RenderTargetDesc,
    width: u32,
    height: u32,
    resource: ID3D12Resource,
    rtv: Heap,
    /// Its sampled view in the backend's view heap.
    slot: u32,
    state: D3D12_RESOURCE_STATES,
}

/// A volume texture.
pub(super) struct ExVolumeGpu {
    resource: ID3D12Resource,
    slot: u32,
    state: D3D12_RESOURCE_STATES,
}

/// The programmable resources on the GPU.
pub(super) struct ExGpu {
    root: ID3D12RootSignature,
    /// Each program's vertex and pixel blobs.
    programs: HashMap<ExProgram, (Vec<u8>, Vec<u8>)>,
    pipelines: HashMap<ExPipelineKey, ID3D12PipelineState>,
    targets: HashMap<ExTexture, ExTargetGpu>,
    volumes: HashMap<ExTexture, ExVolumeGpu>,
    /// One sampler per [`SamplerState`], by its index (not shader
    /// visible: draws copy from it).
    samplers: Heap,
}

impl ExGpu {
    pub(super) fn new(gpu: &Gpu) -> Result<ExGpu, GpuError> {
        let samplers = Heap::new(
            gpu,
            &D3D12_DESCRIPTOR_HEAP_DESC {
                Type: D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                NumDescriptors: SamplerState::COUNT,
                ..Default::default()
            },
        )?;
        for i in 0..SamplerState::COUNT {
            let state = SamplerState::from_index(i).expect("below COUNT");
            // SAFETY: slot `i` belongs to the heap kept with the set.
            unsafe {
                gpu.device
                    .CreateSampler(&sampler_desc(state), samplers.cpu(i));
            }
        }
        Ok(ExGpu {
            root: root_signature(&gpu.device)?,
            programs: HashMap::new(),
            pipelines: HashMap::new(),
            targets: HashMap::new(),
            volumes: HashMap::new(),
            samplers,
        })
    }
}

fn gpu_error(call: &'static str) -> GpuError {
    GpuError { call, code: 0 }
}

/// A render target's resource of `width` by `height`, in the render-target
/// state.
fn target_resource(
    gpu: &Gpu,
    format: ExFormat,
    width: u32,
    height: u32,
) -> Result<ID3D12Resource, GpuError> {
    let desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        Width: u64::from(width),
        Height: height,
        DepthOrArraySize: 1,
        MipLevels: 1,
        Format: dxgi_format(format),
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
        Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        ..Default::default()
    };
    committed_texture(gpu, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET)
}

/// Bytes `D3DCOLOR` `0xAARRGGBB` as red, green, blue and alpha from 0 to 1.
fn rgba(color: u32) -> [f32; 4] {
    let n = |shift: u32| f32::from((color >> shift) as u8) / 255.0;
    [n(16), n(8), n(0), n(24)]
}

impl D3d12Backend {
    /// Moves the depth buffer back to the depth-write state.
    pub(super) fn depth_to_write(&mut self) {
        if self.depth_state == D3D12_RESOURCE_STATE_DEPTH_WRITE {
            return;
        }
        if let Err(e) = self.open() {
            self.errors.push(e);
            return;
        }
        let depth = self.depth.clone();
        barrier(
            &self.gpu,
            &depth,
            &mut self.depth_state,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
        );
    }

    /// Makes `texture`'s resource and views at `width` by `height`; a
    /// target made before keeps its view slots.
    fn make_target(
        &mut self,
        texture: ExTexture,
        desc: RenderTargetDesc,
        width: u32,
        height: u32,
    ) -> Result<(), GpuError> {
        let resource = target_resource(&self.gpu, desc.format, width, height)?;
        let (rtv, slot) = match self.ex.targets.remove(&texture) {
            Some(old) => {
                self.garbage.push(old.resource);
                (old.rtv, old.slot)
            }
            None => {
                let rtv = Heap::new(
                    &self.gpu,
                    &D3D12_DESCRIPTOR_HEAP_DESC {
                        Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                        NumDescriptors: 1,
                        ..Default::default()
                    },
                )?;
                let slot = self
                    .free_slots
                    .pop()
                    .ok_or(gpu_error("texture views exhausted"))?;
                (rtv, slot)
            }
        };
        let view = view_2d(dxgi_format(desc.format), sampled_mapping(desc.format));
        // SAFETY: the RTV slot belongs to the target's own heap and the
        // view slot to the backend's view heap, both kept with the
        // resource; a draw that used the old views has recorded them.
        unsafe {
            self.gpu
                .device
                .CreateRenderTargetView(&resource, None, rtv.cpu(0));
            self.gpu
                .device
                .CreateShaderResourceView(&resource, Some(&view), self.srvs.cpu(slot));
        }
        self.ex.targets.insert(
            texture,
            ExTargetGpu {
                desc,
                width,
                height,
                resource,
                rtv,
                slot,
                state: D3D12_RESOURCE_STATE_RENDER_TARGET,
            },
        );
        Ok(())
    }

    /// Every render target sized by the back buffer, made again at its
    /// size for `width` by `height`.
    pub(super) fn resize_ex_targets(&mut self, width: u32, height: u32) -> Result<(), GpuError> {
        let scaled: Vec<(ExTexture, RenderTargetDesc)> = self
            .ex
            .targets
            .iter()
            .filter(|(_, t)| matches!(t.desc.size, TargetSize::Scaled(_)))
            .map(|(h, t)| (*h, t.desc))
            .collect();
        for (texture, desc) in scaled {
            let (w, h) = desc.size.resolve(width, height);
            self.make_target(texture, desc, w, h)?;
        }
        Ok(())
    }

    fn create_volume(
        &mut self,
        texture: ExTexture,
        format: ExFormat,
        size: [u32; 3],
        texels: &[u8],
    ) -> Result<(), GpuError> {
        let [width, height, depth] = size;
        // Feature level 11_0's volume limit; the depth field is 16 bits.
        if width > 2048 || height > 2048 || depth > 2048 {
            return Err(gpu_error("volume texture larger than 2048"));
        }
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE3D,
            Width: u64::from(width),
            Height: height,
            DepthOrArraySize: depth as u16,
            MipLevels: 1,
            Format: dxgi_format(format),
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
            ..Default::default()
        };
        let resource = committed_texture(&self.gpu, &desc, D3D12_RESOURCE_STATE_COMMON)?;
        let mut footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default();
        let mut rows = 0u32;
        let mut row_bytes = 0u64;
        let mut total = 0u64;
        // SAFETY: a plain query of the resource's own description, with
        // valid out pointers for one subresource.
        unsafe {
            self.gpu.device.GetCopyableFootprints(
                &desc,
                0,
                1,
                0,
                Some(&mut footprint),
                Some(&mut rows),
                Some(&mut row_bytes),
                Some(&mut total),
            );
        }
        let pitch = footprint.Footprint.RowPitch as usize;
        let slice_pitch = pitch * rows as usize;
        let row = (width * format.bytes_per_texel()) as usize;
        let mut staging = vec![0u8; total as usize];
        for z in 0..depth as usize {
            for y in 0..height as usize {
                let from = (z * height as usize + y) * row;
                let to = z * slice_pitch + y * pitch;
                staging[to..to + row].copy_from_slice(&texels[from..from + row]);
            }
        }
        let slot = self
            .free_slots
            .pop()
            .ok_or(gpu_error("texture views exhausted"))?;
        let view = view_3d(dxgi_format(format), sampled_mapping(format));
        // SAFETY: the slot belongs to the view heap and to this volume
        // until it is released.
        unsafe {
            self.gpu
                .device
                .CreateShaderResourceView(&resource, Some(&view), self.srvs.cpu(slot));
        }
        self.open()?;
        let alloc = self.upload.push(
            &self.gpu.device,
            &staging,
            u64::from(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),
        )?;
        footprint.Offset = alloc.offset;
        let mut state = D3D12_RESOURCE_STATE_COMMON;
        barrier(
            &self.gpu,
            &resource,
            &mut state,
            D3D12_RESOURCE_STATE_COPY_DEST,
        );
        copy_to_texture(&self.gpu, &resource, 0, &alloc.resource, footprint);
        self.ex.volumes.insert(
            texture,
            ExVolumeGpu {
                resource,
                slot,
                state,
            },
        );
        Ok(())
    }

    /// The pipeline state of a draw, made on first use.
    fn ex_pipeline(&mut self, key: &ExPipelineKey) -> Result<ID3D12PipelineState, GpuError> {
        if let Some(p) = self.ex.pipelines.get(key) {
            return Ok(p.clone());
        }
        let (vs, ps) = self
            .ex
            .programs
            .get(&key.program)
            .ok_or(gpu_error("draw: program not on the GPU"))?;
        let state = pipeline_state(&self.gpu.device, &self.ex.root, key, vs, ps)?;
        self.ex.pipelines.insert(*key, state.clone());
        Ok(state)
    }

    /// The render-target view of `target`, moved to the render-target
    /// state.
    fn ex_target_view(
        &mut self,
        target: PassTarget,
    ) -> Result<windows::Win32::Graphics::Direct3D12::D3D12_CPU_DESCRIPTOR_HANDLE, GpuError> {
        match target {
            PassTarget::Scene => {
                let scene = self.target.resource.clone();
                barrier(
                    &self.gpu,
                    &scene,
                    &mut self.scene_state,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                );
                Ok(self.target.view)
            }
            PassTarget::Texture(h) => {
                let t = self
                    .ex
                    .targets
                    .get_mut(&h)
                    .ok_or(gpu_error("pass: render target not on the GPU"))?;
                barrier(
                    &self.gpu,
                    &t.resource,
                    &mut t.state,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                );
                Ok(t.rtv.cpu(0))
            }
        }
    }

    fn try_ex_clear(&mut self, target: PassTarget, color: u32) -> Result<(), GpuError> {
        self.open()?;
        let view = self.ex_target_view(target)?;
        // SAFETY: the target's view, in the render-target state.
        unsafe {
            self.gpu
                .list
                .ClearRenderTargetView(view, &rgba(color), None);
        }
        Ok(())
    }

    /// The sampled view of a slot's binding, its resource moved to the
    /// shader-resource state.
    fn ex_view(
        &mut self,
        binding: ExBinding,
    ) -> Result<windows::Win32::Graphics::Direct3D12::D3D12_CPU_DESCRIPTOR_HANDLE, GpuError> {
        let psr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        let slot = match binding {
            ExBinding::SceneDepth => {
                let depth = self.depth.clone();
                barrier(&self.gpu, &depth, &mut self.depth_state, psr);
                self.depth_slot
            }
            ExBinding::Ex(h) => {
                if let Some(t) = self.ex.targets.get_mut(&h) {
                    barrier(&self.gpu, &t.resource, &mut t.state, psr);
                    t.slot
                } else if let Some(v) = self.ex.volumes.get_mut(&h) {
                    barrier(&self.gpu, &v.resource, &mut v.state, psr);
                    v.slot
                } else {
                    return Err(gpu_error("draw: Ex texture not on the GPU"));
                }
            }
            ExBinding::Device(h) => {
                let t = self
                    .textures
                    .get_mut(&h)
                    .ok_or(gpu_error("draw: texture not on the GPU"))?;
                barrier(&self.gpu, &t.resource, &mut t.state, psr);
                t.slot
            }
        };
        Ok(self.srvs.cpu(slot))
    }

    fn try_ex_draw(&mut self, d: &ExDraw) -> Result<(), GpuError> {
        self.open()?;
        let slots = TEXTURE_SLOTS as u32;
        if self.next_view + slots > DRAW_SLOTS || self.next_sampler + slots > EX_SAMPLER_RING {
            self.flush()?;
            self.open()?;
        }
        let pso = self.ex_pipeline(&d.key)?;
        if d.key.depth_bound {
            self.depth_to_write();
        }
        let rtv = self.ex_target_view(d.target)?;
        let device = self.gpu.device.clone();

        // The slots' views and samplers, copied into the shader-visible
        // rings; an empty slot reads the null 2D view.
        let views = self.next_view;
        let sampler_base = samplers::COUNT + self.next_sampler;
        for (i, slot) in d.textures.iter().enumerate() {
            let (src, sampler) = match slot {
                Some((binding, sampler)) => (self.ex_view(*binding)?, *sampler),
                None => (self.srvs.cpu(0), SamplerState::default()),
            };
            // SAFETY: the sources are slots of live CPU heaps of the same
            // types; the destinations are this draw's slots of the rings.
            unsafe {
                device.CopyDescriptorsSimple(
                    1,
                    self.draw_views.cpu(views + i as u32),
                    src,
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                );
                device.CopyDescriptorsSimple(
                    1,
                    self.samplers.cpu(sampler_base + i as u32),
                    self.ex.samplers.cpu(sampler.index()),
                    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                );
            }
        }
        self.next_view += slots;
        self.next_sampler += slots;

        let registers = |r: &[[f32; 4]]| -> Vec<u8> {
            r.iter().flatten().flat_map(|f| f.to_le_bytes()).collect()
        };
        let align = u64::from(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        let vcb = self
            .upload
            .push(&device, &registers(&d.vertex_constants), align)?;
        let pcb = self
            .upload
            .push(&device, &registers(&d.pixel_constants), align)?;
        let vb = self.upload.push(&device, &d.vertices, 16)?;
        let vbv = D3D12_VERTEX_BUFFER_VIEW {
            BufferLocation: vb.gpu_address,
            SizeInBytes: d.vertices.len() as u32,
            StrideInBytes: d.stride,
        };
        let defaults = D3D12_VERTEX_BUFFER_VIEW {
            // SAFETY: a live buffer.
            BufferLocation: unsafe { self.defaults.GetGPUVirtualAddress() },
            SizeInBytes: DEFAULTS.len() as u32,
            StrideInBytes: 0,
        };
        let index = match &d.indices {
            Some(list) => {
                let bytes: Vec<u8> = list.iter().flat_map(|i| i.to_le_bytes()).collect();
                let a = self.upload.push(&device, &bytes, 4)?;
                Some((
                    D3D12_INDEX_BUFFER_VIEW {
                        BufferLocation: a.gpu_address,
                        SizeInBytes: bytes.len() as u32,
                        Format: DXGI_FORMAT_R16_UINT,
                    },
                    list.len() as u32,
                ))
            }
            None => None,
        };
        let (w, h) = d.target_size;
        let dsv = d.key.depth_bound.then(|| self.dsv.cpu(0));
        let list = self.gpu.list.clone();
        // SAFETY: every object the list is given lives until the frame is
        // flushed: the pipeline state in the cache, the views in their
        // heaps, the uploads in the ring.
        unsafe {
            list.SetGraphicsRootSignature(&self.ex.root);
            list.SetPipelineState(&pso);
            list.SetGraphicsRootConstantBufferView(ROOT_VERTEX_CONSTANTS, vcb.gpu_address);
            list.SetGraphicsRootConstantBufferView(ROOT_PIXEL_CONSTANTS, pcb.gpu_address);
            list.SetGraphicsRootDescriptorTable(ROOT_VIEWS, self.draw_views.gpu(views));
            list.SetGraphicsRootDescriptorTable(ROOT_SAMPLERS, self.samplers.gpu(sampler_base));
            list.IASetPrimitiveTopology(topology(d.primitive));
            list.IASetVertexBuffers(0, Some(&[vbv, defaults]));
            list.RSSetViewports(&[D3D12_VIEWPORT {
                TopLeftX: 0.0,
                TopLeftY: 0.0,
                Width: w as f32,
                Height: h as f32,
                MinDepth: 0.0,
                MaxDepth: 1.0,
            }]);
            list.RSSetScissorRects(&[rect(0, 0, w, h)]);
            list.OMSetRenderTargets(1, Some(&rtv), false, dsv.as_ref().map(std::ptr::from_ref));
            match index {
                None => list.DrawInstanced(d.vertex_count, 1, 0, 0),
                Some((ibv, count)) => {
                    list.IASetIndexBuffer(Some(&ibv));
                    list.DrawIndexedInstanced(count, 1, 0, 0, 0);
                }
            }
        }
        Ok(())
    }

    /// Runs the frame and reads a render target back: its texels as stored
    /// (BGRA8 for [`ExFormat::Bgra8`]), rows unpadded.
    pub fn read_ex_target(&mut self, texture: ExTexture) -> Result<Vec<u8>, GpuError> {
        self.flush()?;
        let t = self
            .ex
            .targets
            .get(&texture)
            .ok_or(gpu_error("read: render target not on the GPU"))?;
        let (resource, format, size, state) = (
            t.resource.clone(),
            t.desc.format,
            (t.width, t.height),
            t.state,
        );
        read_texture(
            &mut self.gpu,
            &resource,
            dxgi_format(format),
            format.bytes_per_texel(),
            size,
            state,
        )
    }

    /// Releases a program's pipeline states once the GPU is done with
    /// them.
    fn drop_pipelines(&mut self, program: ExProgram) -> Result<(), GpuError> {
        if self.ex.pipelines.keys().any(|k| k.program == program) {
            self.flush()?;
            self.ex.pipelines.retain(|k, _| k.program != program);
        }
        Ok(())
    }
}

impl ExBackend for D3d12Backend {
    fn ex_caps(&self) -> ExCaps {
        ExCaps::feature_level_11_0(self.gpu.adapter_name().to_string())
    }

    fn ex_create(&mut self, create: ExCreate<'_>) -> Result<(), String> {
        let result = match create {
            ExCreate::Program { program, vs, ps } => self.drop_pipelines(program).map(|()| {
                self.ex.programs.insert(program, (vs.to_vec(), ps.to_vec()));
            }),
            ExCreate::RenderTarget {
                texture,
                desc,
                width,
                height,
            } => self.make_target(texture, desc, width, height),
            ExCreate::Volume {
                texture,
                format,
                width,
                height,
                depth,
                texels,
            } => self.create_volume(texture, format, [width, height, depth], texels),
        };
        result.map_err(|e| e.to_string())
    }

    fn ex_release(&mut self, release: ExRelease) {
        match release {
            ExRelease::Program(program) => {
                let result = self.drop_pipelines(program);
                self.fail(result);
                self.ex.programs.remove(&program);
            }
            ExRelease::Texture(texture) => {
                if let Some(t) = self.ex.targets.remove(&texture) {
                    self.free_slots.push(t.slot);
                    self.garbage.push(t.resource);
                }
                if let Some(v) = self.ex.volumes.remove(&texture) {
                    self.free_slots.push(v.slot);
                    self.garbage.push(v.resource);
                }
            }
        }
    }

    fn ex_clear(&mut self, target: PassTarget, color: u32) {
        let result = self.try_ex_clear(target, color);
        self.fail(result);
    }

    fn ex_draw(&mut self, draw: ExDraw) {
        let result = self.try_ex_draw(&draw);
        self.fail(result);
    }

    fn ex_end_pass(&mut self) {
        self.depth_to_write();
    }

    fn back_buffer_resized(&mut self, width: u32, height: u32) {
        let result = self.resize(width, height);
        self.fail(result);
    }
}

#[cfg(test)]
mod tests {
    use super::super::super::AdapterChoice;
    use super::*;
    use crate::render::ff::defaults::Defaults;
    use crate::render::ff::device::{ClearFlags, Device};
    use crate::render::ff::fvf::Fvf;
    use crate::render::ff::state::{Blend, Compare, PipelineProperty, RenderState, ZBuffer};
    use crate::render::pass::{
        Address, Filter, MipFilter, PassDesc, ProgramBlobs, RenderTargetDesc,
    };

    const SIZE: u32 = 8;

    const CONSTANT: ProgramBlobs = crate::program_blobs!(
        "pass_test constant",
        "pass_test_vs",
        "pass_test_constant_ps"
    );
    const DEPTH: ProgramBlobs =
        crate::program_blobs!("pass_test depth", "pass_test_vs", "pass_test_depth_ps");
    const VOLUME: ProgramBlobs =
        crate::program_blobs!("pass_test volume", "pass_test_vs", "pass_test_volume_ps");

    /// A device drawing on WARP, or `None` off CI where there is no WARP
    /// device (on CI a missing device fails), or while the test blobs are
    /// the empty placeholders CI's shader check replaces (record 0005,
    /// point 10: the job must reach that check to print them).
    fn device() -> Option<Device<D3d12Backend>> {
        if [CONSTANT, DEPTH, VOLUME]
            .iter()
            .any(|p| p.vs.is_empty() || p.ps.is_empty())
        {
            println!("the pass_test blobs are placeholders; nothing to test");
            return None;
        }
        let layer = super::super::super::enable_debug_layer();
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

    fn check(d: &Device<D3d12Backend>) {
        assert!(
            d.backend.errors.is_empty(),
            "{:?}\n{}",
            d.backend.errors,
            d.backend.debug_messages().join("\n")
        );
    }

    fn pixel(pixels: &[u8], width: u32, x: u32, y: u32) -> [u8; 4] {
        let at = ((y * width + x) * 4) as usize;
        pixels[at..at + 4].try_into().unwrap()
    }

    /// Two clockwise triangles from (x0, 0) to (x1, SIZE) as `XYZRHW |
    /// DIFFUSE` vertices at depth `z`.
    fn quad(x0: f32, x1: f32, z: f32, color: u32) -> Vec<u8> {
        let s = SIZE as f32;
        let corners = [(x0, 0.0), (x1, 0.0), (x0, s), (x1, 0.0), (x1, s), (x0, s)];
        let mut v = Vec::new();
        for (x, y) in corners {
            for f in [x, y, z, 1.0] {
                v.extend_from_slice(&f32::to_le_bytes(f));
            }
            v.extend_from_slice(&color.to_le_bytes());
        }
        v
    }

    const XYZRHW_DIFFUSE: Fvf = Fvf(0x044);

    fn target(d: &mut Device<D3d12Backend>, size: TargetSize) -> ExTexture {
        d.create_render_target(RenderTargetDesc {
            size,
            format: ExFormat::Bgra8,
        })
        .unwrap()
    }

    /// A program draws into a target of half the back buffer, which is
    /// read back; after a resize the same handle has the new size.
    #[test]
    fn programs_draw_into_scaled_targets() {
        let Some(mut d) = device() else { return };
        let program = d.create_program(&CONSTANT).unwrap();
        let rt = target(&mut d, TargetSize::Scaled(0.5));
        {
            let mut pass = d
                .begin_pass(PassDesc {
                    target: PassTarget::Texture(rt),
                    keep_depth: false,
                    clear: Some(0xff00_00ff),
                })
                .unwrap();
            pass.bind_program(Some(program)).unwrap();
            pass.set_pixel_constants(0, &[[1.0, 0.5, 0.0, 1.0]])
                .unwrap();
            pass.draw_fullscreen_quad().unwrap();
        }
        let pixels = d.backend.read_ex_target(rt).unwrap();
        check(&d);
        assert_eq!(pixels.len(), 4 * 4 * 4);
        // BGRA: blue 0, green 0.5, red 1, alpha 1, in every pixel.
        for px in pixels.as_chunks::<4>().0 {
            assert_eq!(px[0], 0, "{px:?}");
            assert!((127..=128).contains(&px[1]), "{px:?}");
            assert_eq!(&px[2..], &[255, 255], "{px:?}");
        }
        d.resize(16, 6);
        assert_eq!(d.ex_texture_size(rt), Ok((8, 3)));
        drop(
            d.begin_pass(PassDesc {
                target: PassTarget::Texture(rt),
                keep_depth: false,
                clear: Some(0xff00_ff00),
            })
            .unwrap(),
        );
        let pixels = d.backend.read_ex_target(rt).unwrap();
        check(&d);
        assert_eq!(pixels.len(), 8 * 3 * 4);
        assert!(
            pixels
                .as_chunks::<4>()
                .0
                .iter()
                .all(|p| *p == [0, 255, 0, 255])
        );
    }

    /// The scene depth, written by a fixed-function draw, is sampled by a
    /// program; the depth buffer then works as a depth buffer again.
    #[test]
    fn programs_sample_the_scene_depth() {
        let Some(mut d) = device() else { return };
        d.state.set_render_state(RenderState::ZEnable(ZBuffer::On));
        d.state.set_render_state(RenderState::ZWriteEnable(true));
        d.state
            .set_render_state(RenderState::ZFunc(Compare::LessEqual));
        d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        // The left half at depth 0.25; the right half keeps the cleared 1.
        d.draw_up(4, XYZRHW_DIFFUSE, &quad(0.0, 4.0, 0.25, 0xffff_ffff), 6)
            .unwrap();
        let program = d.create_program(&DEPTH).unwrap();
        let rt = target(
            &mut d,
            TargetSize::Fixed {
                width: SIZE,
                height: SIZE,
            },
        );
        let depth = d.scene_depth();
        {
            let mut pass = d
                .begin_pass(PassDesc {
                    target: PassTarget::Texture(rt),
                    keep_depth: false,
                    clear: None,
                })
                .unwrap();
            pass.bind_program(Some(program)).unwrap();
            pass.bind_texture(0, Some(depth), SamplerState::default())
                .unwrap();
            pass.draw_fullscreen_quad().unwrap();
        }
        let pixels = d.backend.read_ex_target(rt).unwrap();
        check(&d);
        for y in [0, 3, 7] {
            let left = pixel(&pixels, SIZE, 1, y);
            assert!((63..=64).contains(&left[0]), "{left:?}");
            assert_eq!(left[0], left[2]);
            assert_eq!(pixel(&pixels, SIZE, 6, y), [255; 4]);
        }
        // A red quad at depth 0.5 is hidden on the left, drawn on the right.
        d.draw_up(4, XYZRHW_DIFFUSE, &quad(0.0, 8.0, 0.5, 0xffff_0000), 6)
            .unwrap();
        let scene = d.backend.read_back().unwrap();
        check(&d);
        assert_eq!(pixel(&scene, SIZE, 1, 4), [255, 255, 255, 255]);
        assert_eq!(pixel(&scene, SIZE, 6, 4), [0, 0, 255, 255]);
    }

    /// A program samples a volume texture at a depth from its constants.
    #[test]
    fn programs_sample_volume_textures() {
        let Some(mut d) = device() else { return };
        // 2 x 2 x 2 BGRA texels: slice 0 red, green / blue, white; slice 1
        // black, yellow / cyan, magenta.
        let texels: [[u8; 4]; 8] = [
            [0, 0, 255, 255],
            [0, 255, 0, 255],
            [255, 0, 0, 255],
            [255, 255, 255, 255],
            [0, 0, 0, 255],
            [0, 255, 255, 255],
            [255, 255, 0, 255],
            [255, 0, 255, 255],
        ];
        let volume = d
            .create_volume_texture(2, 2, 2, ExFormat::Bgra8, Some(texels.as_flattened()))
            .unwrap();
        let program = d.create_program(&VOLUME).unwrap();
        let rt = target(&mut d, TargetSize::Scaled(1.0));
        let point = SamplerState::new(Filter::Point, MipFilter::None, Address::Clamp);
        for (slice, w) in [(0, 0.25_f32), (1, 0.75)] {
            {
                let mut pass = d
                    .begin_pass(PassDesc {
                        target: PassTarget::Texture(rt),
                        keep_depth: false,
                        clear: None,
                    })
                    .unwrap();
                pass.bind_program(Some(program)).unwrap();
                pass.bind_texture(1, Some(volume), point).unwrap();
                pass.set_pixel_constants(0, &[[w, 0.0, 0.0, 0.0]]).unwrap();
                pass.draw_fullscreen_quad().unwrap();
            }
            let pixels = d.backend.read_ex_target(rt).unwrap();
            check(&d);
            let at = |x, y| pixel(&pixels, SIZE, x, y);
            assert_eq!(at(1, 1), texels[slice * 4], "slice {slice}");
            assert_eq!(at(6, 1), texels[slice * 4 + 1], "slice {slice}");
            assert_eq!(at(1, 6), texels[slice * 4 + 2], "slice {slice}");
            assert_eq!(at(6, 6), texels[slice * 4 + 3], "slice {slice}");
        }
    }

    /// A pass into the scene with its own blend states and viewport: after
    /// it the device draws with its own viewport and no blending.
    #[test]
    fn a_pass_restores_the_device_state() {
        let Some(mut d) = device() else { return };
        d.set_viewport(0, 0, 4, SIZE);
        d.state
            .set_pipeline(PipelineProperty::ClearColor, 0xff00_0000);
        d.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        let program = d.create_program(&CONSTANT).unwrap();
        {
            let mut pass = d
                .begin_pass(PassDesc {
                    target: PassTarget::Scene,
                    keep_depth: false,
                    clear: Some(0xffff_0000),
                })
                .unwrap();
            assert_eq!(pass.target_size(), (SIZE, SIZE));
            pass.bind_program(Some(program)).unwrap();
            pass.set_render_state(RenderState::AlphaBlendEnable(true));
            pass.set_render_state(RenderState::SrcBlend(Blend::One));
            pass.set_render_state(RenderState::DestBlend(Blend::One));
            // Blue added to the red clear, over the whole scene.
            pass.set_pixel_constants(0, &[[0.0, 0.0, 1.0, 1.0]])
                .unwrap();
            pass.draw_fullscreen_quad().unwrap();
        }
        assert_eq!(
            d.state
                .render_state(crate::render::ff::state::RenderStateId::AlphaBlendEnable),
            RenderState::AlphaBlendEnable(false)
        );
        // Green over the whole scene, clipped to the device's viewport.
        d.draw_up(4, XYZRHW_DIFFUSE, &quad(0.0, 8.0, 0.5, 0xff00_ff00), 6)
            .unwrap();
        let scene = d.backend.read_back().unwrap();
        check(&d);
        assert_eq!(pixel(&scene, SIZE, 1, 4), [0, 255, 0, 255]);
        assert_eq!(pixel(&scene, SIZE, 6, 4), [255, 0, 255, 255]);
    }
}
