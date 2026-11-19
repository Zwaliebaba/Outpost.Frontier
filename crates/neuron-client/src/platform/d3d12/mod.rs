//! Direct3D 12 through the `windows` crate (decision T1, record 0004 point
//! 19): safe types over the COM interfaces the backend uses. Sizes and
//! offsets are checked here, resources live as long as their Rust owner,
//! and every submission is fenced so nothing is released while the GPU may
//! still use it.
//!
//! This first part is the device on a hardware adapter or on WARP, the
//! direct queue with its fence, an offscreen render target, clearing and
//! reading pixels back: enough to test the device on CI's WARP adapter.

pub mod backend;
pub mod pipeline;
pub mod present;
mod upload;

use std::fmt;

use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
use windows::Win32::Graphics::Direct3D12::{
    D3D_SHADER_MODEL_6_1, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_DESC,
    D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_DESC, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
    D3D12_FEATURE_DATA_SHADER_MODEL, D3D12_FEATURE_SHADER_MODEL, D3D12_FENCE_FLAG_NONE,
    D3D12_HEAP_FLAG_NONE, D3D12_HEAP_PROPERTIES, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_READBACK,
    D3D12_MESSAGE, D3D12_PLACED_SUBRESOURCE_FOOTPRINT, D3D12_RANGE, D3D12_RESOURCE_BARRIER,
    D3D12_RESOURCE_BARRIER_0, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_DESC, D3D12_RESOURCE_DIMENSION_BUFFER,
    D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE,
    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATES, D3D12_RESOURCE_TRANSITION_BARRIER,
    D3D12_TEXTURE_COPY_LOCATION, D3D12_TEXTURE_COPY_LOCATION_0,
    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12CreateDevice, D3D12GetDebugInterface,
    ID3D12CommandAllocator, ID3D12CommandList, ID3D12CommandQueue, ID3D12Debug,
    ID3D12DescriptorHeap, ID3D12Device, ID3D12Fence, ID3D12GraphicsCommandList, ID3D12InfoQueue,
    ID3D12Resource,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::{
    CreateDXGIFactory2, DXGI_CREATE_FACTORY_FLAGS, IDXGIAdapter1, IDXGIFactory4,
};
use windows::Win32::System::Threading::{CreateEventW, INFINITE, WaitForSingleObject};
use windows::core::Interface;

/// A failed Direct3D 12 or DXGI call.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GpuError {
    pub call: &'static str,
    pub code: i32,
}

impl fmt::Display for GpuError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} failed with 0x{:08x}", self.call, self.code)
    }
}

impl std::error::Error for GpuError {}

trait Context<T> {
    fn at(self, call: &'static str) -> Result<T, GpuError>;
}

impl<T> Context<T> for windows::core::Result<T> {
    fn at(self, call: &'static str) -> Result<T, GpuError> {
        self.map_err(|e| GpuError {
            call,
            code: e.code().0,
        })
    }
}

/// Turns on the Direct3D 12 debug layer for devices made afterwards, when
/// the layer is installed (the Graphics Tools feature); true when it is on.
/// For tests: the layer explains failed calls in [`Gpu::debug_messages`].
pub fn enable_debug_layer() -> bool {
    let mut debug: Option<ID3D12Debug> = None;
    // SAFETY: a plain query with a valid out slot.
    if unsafe { D3D12GetDebugInterface(&mut debug) }.is_err() {
        return false;
    }
    match debug {
        Some(debug) => {
            // SAFETY: called before the devices it should affect are made.
            unsafe { debug.EnableDebugLayer() };
            true
        }
        None => false,
    }
}

/// Which adapter a device runs on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AdapterChoice {
    /// The first hardware adapter that offers feature level 11_0.
    Hardware,
    /// The WARP software rasteriser (CI, record 0004 owner answer 5).
    Warp,
}

/// A Win32 event handle closed on drop.
struct Event(HANDLE);

impl Drop for Event {
    fn drop(&mut self) {
        // SAFETY: the handle came from CreateEventW and is closed once.
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

/// The device, its direct queue, one command list and the fence that
/// orders submissions.
pub struct Gpu {
    device: ID3D12Device,
    queue: ID3D12CommandQueue,
    allocator: ID3D12CommandAllocator,
    list: ID3D12GraphicsCommandList,
    fence: ID3D12Fence,
    fence_value: u64,
    event: Event,
}

impl Gpu {
    /// Creates the device on `choice`.
    pub fn new(choice: AdapterChoice) -> Result<Gpu, GpuError> {
        // SAFETY: plain DXGI factory creation with no flags.
        let factory: IDXGIFactory4 =
            unsafe { CreateDXGIFactory2(DXGI_CREATE_FACTORY_FLAGS(0)) }.at("CreateDXGIFactory2")?;
        let adapter: IDXGIAdapter1 = match choice {
            // SAFETY: EnumWarpAdapter returns an owned interface.
            AdapterChoice::Warp => unsafe { factory.EnumWarpAdapter() }.at("EnumWarpAdapter")?,
            // SAFETY: EnumAdapters1 with index 0 returns an owned interface
            // or an error when there is no adapter.
            AdapterChoice::Hardware => unsafe { factory.EnumAdapters1(0) }.at("EnumAdapters1")?,
        };
        let mut device: Option<ID3D12Device> = None;
        // SAFETY: the adapter is live and `device` is a valid out slot.
        unsafe { D3D12CreateDevice(&adapter, D3D_FEATURE_LEVEL_11_0, &mut device) }
            .at("D3D12CreateDevice")?;
        let device = device.ok_or(GpuError {
            call: "D3D12CreateDevice",
            code: 0,
        })?;
        // The shaders are DXIL of shader model 6.1 (record 0004, point 16 as
        // amended on 2026-11-19): refuse a driver that cannot run them here
        // rather than at the first pipeline state.
        let mut model = D3D12_FEATURE_DATA_SHADER_MODEL {
            HighestShaderModel: D3D_SHADER_MODEL_6_1,
        };
        // SAFETY: the out structure is the one D3D12_FEATURE_SHADER_MODEL
        // takes, and its size is passed with it.
        unsafe {
            device.CheckFeatureSupport(
                D3D12_FEATURE_SHADER_MODEL,
                std::ptr::from_mut(&mut model).cast(),
                std::mem::size_of::<D3D12_FEATURE_DATA_SHADER_MODEL>() as u32,
            )
        }
        .at("CheckFeatureSupport(SHADER_MODEL)")?;
        if model.HighestShaderModel.0 < D3D_SHADER_MODEL_6_1.0 {
            return Err(GpuError {
                call: "shader model 6.1 support",
                code: model.HighestShaderModel.0,
            });
        }
        let queue_desc = D3D12_COMMAND_QUEUE_DESC {
            Type: D3D12_COMMAND_LIST_TYPE_DIRECT,
            ..Default::default()
        };
        // SAFETY: the description is fully initialised.
        let queue: ID3D12CommandQueue =
            unsafe { device.CreateCommandQueue(&queue_desc) }.at("CreateCommandQueue")?;
        // SAFETY: a direct allocator on a live device.
        let allocator: ID3D12CommandAllocator =
            unsafe { device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT) }
                .at("CreateCommandAllocator")?;
        // SAFETY: the allocator outlives the list (both are fields of Gpu).
        let list: ID3D12GraphicsCommandList = unsafe {
            device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &allocator, None)
        }
        .at("CreateCommandList")?;
        // SAFETY: a new list is open; it is closed until a recording starts.
        unsafe { list.Close() }.at("Close")?;
        // SAFETY: a fence with no flags.
        let fence: ID3D12Fence =
            unsafe { device.CreateFence(0, D3D12_FENCE_FLAG_NONE) }.at("CreateFence")?;
        // SAFETY: an unnamed auto-reset event.
        let event = Event(unsafe { CreateEventW(None, false, false, None) }.at("CreateEventW")?);
        Ok(Gpu {
            device,
            queue,
            allocator,
            list,
            fence,
            fence_value: 0,
            event,
        })
    }

    /// Records commands with `record`, submits them and waits until the GPU
    /// has run them.
    pub fn submit(
        &mut self,
        record: impl FnOnce(&ID3D12GraphicsCommandList) -> Result<(), GpuError>,
    ) -> Result<(), GpuError> {
        self.begin()?;
        let recorded = record(&self.list);
        let finished = self.finish();
        recorded.and(finished)
    }

    /// Opens the command list for recording. The GPU must be idle, which
    /// it is after [`Gpu::finish`] or [`Gpu::submit`].
    pub fn begin(&mut self) -> Result<(), GpuError> {
        // SAFETY: the previous submission was waited for, so the allocator
        // and the list are not in use by the GPU.
        unsafe { self.allocator.Reset() }.at("CommandAllocator::Reset")?;
        // SAFETY: as above; no initial pipeline state.
        unsafe { self.list.Reset(&self.allocator, None) }.at("CommandList::Reset")
    }

    /// The command list [`Gpu::begin`] opened.
    #[must_use]
    pub fn list(&self) -> &ID3D12GraphicsCommandList {
        &self.list
    }

    /// Closes the command list, runs it and waits until the GPU is idle.
    pub fn finish(&mut self) -> Result<(), GpuError> {
        // SAFETY: the list is open and recorded.
        unsafe { self.list.Close() }.at("CommandList::Close")?;
        let list: ID3D12CommandList = self.list.cast().at("cast")?;
        // SAFETY: the list is closed; every resource it uses is owned by
        // the caller until this call returns, which waits.
        unsafe { self.queue.ExecuteCommandLists(&[Some(list)]) };
        self.wait_idle()
    }

    /// Waits until the GPU has finished every submission.
    pub fn wait_idle(&mut self) -> Result<(), GpuError> {
        self.fence_value += 1;
        // SAFETY: the fence belongs to this device.
        unsafe { self.queue.Signal(&self.fence, self.fence_value) }.at("Signal")?;
        // SAFETY: the event handle is live for the life of self.
        unsafe {
            self.fence
                .SetEventOnCompletion(self.fence_value, self.event.0)
        }
        .at("SetEventOnCompletion")?;
        // SAFETY: waiting on a live event.
        unsafe { WaitForSingleObject(self.event.0, INFINITE) };
        Ok(())
    }

    #[must_use]
    pub fn device(&self) -> &ID3D12Device {
        &self.device
    }

    /// The debug layer's stored messages; empty without the layer.
    #[must_use]
    pub fn debug_messages(&self) -> Vec<String> {
        let Ok(queue) = self.device.cast::<ID3D12InfoQueue>() else {
            return Vec::new();
        };
        // SAFETY: plain queries of the queue.
        let count = unsafe { queue.GetNumStoredMessages() };
        let mut out = Vec::new();
        for i in 0..count {
            let mut len = 0usize;
            // SAFETY: the first call asks only for the size.
            if unsafe { queue.GetMessage(i, None, &mut len) }.is_err() || len == 0 {
                continue;
            }
            // A u64 buffer keeps the message aligned.
            let mut buf = vec![0u64; len.div_ceil(8)];
            let message = buf.as_mut_ptr().cast::<D3D12_MESSAGE>();
            // SAFETY: the buffer holds `len` bytes, as the queue asked.
            if unsafe { queue.GetMessage(i, Some(message), &mut len) }.is_err() {
                continue;
            }
            // SAFETY: the queue filled the message; its description is a
            // NUL-terminated string inside the same buffer.
            let text = unsafe {
                std::ffi::CStr::from_ptr((*message).pDescription.cast())
                    .to_string_lossy()
                    .into_owned()
            };
            out.push(text);
        }
        out
    }
}

impl Drop for Gpu {
    fn drop(&mut self) {
        // Nothing may still be running when the interfaces are released.
        let _ = self.wait_idle();
    }
}

fn transition(
    resource: &ID3D12Resource,
    before: D3D12_RESOURCE_STATES,
    after: D3D12_RESOURCE_STATES,
) -> D3D12_RESOURCE_BARRIER {
    D3D12_RESOURCE_BARRIER {
        Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        Anonymous: D3D12_RESOURCE_BARRIER_0 {
            Transition: std::mem::ManuallyDrop::new(D3D12_RESOURCE_TRANSITION_BARRIER {
                // The barrier borrows the resource; the clone is released
                // below in `release_barrier`.
                pResource: std::mem::ManuallyDrop::new(Some(resource.clone())),
                Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                StateBefore: before,
                StateAfter: after,
            }),
        },
        ..Default::default()
    }
}

/// Releases the resource reference a transition barrier holds.
fn release_barrier(barrier: D3D12_RESOURCE_BARRIER) {
    // SAFETY: `transition` built the barrier as a transition, so that is
    // the union member to read; its resource is dropped once.
    unsafe {
        let mut t = std::mem::ManuallyDrop::into_inner(barrier.Anonymous.Transition);
        std::mem::ManuallyDrop::drop(&mut t.pResource);
    }
}

/// An offscreen BGRA8 render target with its view.
pub struct RenderTarget {
    pub width: u32,
    pub height: u32,
    resource: ID3D12Resource,
    _heap: ID3D12DescriptorHeap,
    view: D3D12_CPU_DESCRIPTOR_HANDLE,
}

impl RenderTarget {
    pub fn new(gpu: &Gpu, width: u32, height: u32) -> Result<RenderTarget, GpuError> {
        let heap_props = D3D12_HEAP_PROPERTIES {
            Type: D3D12_HEAP_TYPE_DEFAULT,
            ..Default::default()
        };
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            Width: u64::from(width),
            Height: height,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: DXGI_FORMAT_B8G8R8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
            Flags: D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            ..Default::default()
        };
        let mut resource: Option<ID3D12Resource> = None;
        // SAFETY: the descriptions are fully initialised and `resource` is a
        // valid out slot.
        unsafe {
            gpu.device.CreateCommittedResource(
                &heap_props,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                None,
                &mut resource,
            )
        }
        .at("CreateCommittedResource")?;
        let resource = resource.ok_or(GpuError {
            call: "CreateCommittedResource",
            code: 0,
        })?;
        let heap_desc = D3D12_DESCRIPTOR_HEAP_DESC {
            Type: D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            NumDescriptors: 1,
            ..Default::default()
        };
        // SAFETY: a one-entry RTV heap.
        let heap: ID3D12DescriptorHeap =
            unsafe { gpu.device.CreateDescriptorHeap(&heap_desc) }.at("CreateDescriptorHeap")?;
        // SAFETY: the heap is live.
        let view = unsafe { heap.GetCPUDescriptorHandleForHeapStart() };
        // SAFETY: the view slot belongs to the heap kept with the target.
        unsafe { gpu.device.CreateRenderTargetView(&resource, None, view) };
        Ok(RenderTarget {
            width,
            height,
            resource,
            _heap: heap,
            view,
        })
    }

    /// Records a clear of the whole target to `rgba` (0 to 1 per channel).
    pub fn record_clear(&self, list: &ID3D12GraphicsCommandList, rgba: [f32; 4]) {
        // SAFETY: the view is this target's, which is in the render-target
        // state outside `read_back`.
        unsafe { list.ClearRenderTargetView(self.view, &rgba, None) };
    }

    /// Copies the target to the CPU: `width * height` pixels as BGRA8,
    /// rows without padding.
    pub fn read_back(&self, gpu: &mut Gpu) -> Result<Vec<u8>, GpuError> {
        let row = self.width * 4;
        let align = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        let pitch = row.div_ceil(align) * align;
        let size = u64::from(pitch) * u64::from(self.height);
        let heap_props = D3D12_HEAP_PROPERTIES {
            Type: D3D12_HEAP_TYPE_READBACK,
            ..Default::default()
        };
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Width: size,
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: DXGI_FORMAT_UNKNOWN,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            ..Default::default()
        };
        let mut buffer: Option<ID3D12Resource> = None;
        // SAFETY: a readback buffer large enough for the pitched copy.
        unsafe {
            gpu.device.CreateCommittedResource(
                &heap_props,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                None,
                &mut buffer,
            )
        }
        .at("CreateCommittedResource")?;
        let buffer = buffer.ok_or(GpuError {
            call: "CreateCommittedResource",
            code: 0,
        })?;
        let (width, height) = (self.width, self.height);
        let target = self.resource.clone();
        let readback = buffer.clone();
        gpu.submit(move |list| {
            let to_copy = transition(
                &target,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
            );
            // SAFETY: the barrier names a live resource in the state given.
            unsafe { list.ResourceBarrier(std::slice::from_ref(&to_copy)) };
            release_barrier(to_copy);
            let src = D3D12_TEXTURE_COPY_LOCATION {
                pResource: std::mem::ManuallyDrop::new(Some(target.clone())),
                Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                    SubresourceIndex: 0,
                },
            };
            let mut footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default();
            footprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            footprint.Footprint.Width = width;
            footprint.Footprint.Height = height;
            footprint.Footprint.Depth = 1;
            footprint.Footprint.RowPitch = pitch;
            let dst = D3D12_TEXTURE_COPY_LOCATION {
                pResource: std::mem::ManuallyDrop::new(Some(readback.clone())),
                Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                    PlacedFootprint: footprint,
                },
            };
            // SAFETY: both locations name live resources; the footprint fits
            // the buffer, whose size was computed from the same pitch.
            unsafe { list.CopyTextureRegion(&dst, 0, 0, 0, &src, None) };
            let mut src = src;
            let mut dst = dst;
            // SAFETY: each location's resource reference is dropped once.
            unsafe {
                std::mem::ManuallyDrop::drop(&mut src.pResource);
                std::mem::ManuallyDrop::drop(&mut dst.pResource);
            }
            let back = transition(
                &target,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
            );
            // SAFETY: as above.
            unsafe { list.ResourceBarrier(std::slice::from_ref(&back)) };
            release_barrier(back);
            Ok(())
        })?;
        let mut data: *mut std::ffi::c_void = std::ptr::null_mut();
        let range = D3D12_RANGE {
            Begin: 0,
            End: size as usize,
        };
        // SAFETY: the copy has finished (submit waited); the buffer is a
        // readback buffer of `size` bytes.
        unsafe { buffer.Map(0, Some(&range), Some(&mut data)) }.at("Map")?;
        let mut pixels = Vec::with_capacity((row * height) as usize);
        for y in 0..height as usize {
            // SAFETY: row `y` starts at `y * pitch` and has `row` bytes
            // inside the mapped `size` bytes.
            let line = unsafe {
                std::slice::from_raw_parts(
                    (data as *const u8).add(y * pitch as usize),
                    row as usize,
                )
            };
            pixels.extend_from_slice(line);
        }
        let none = D3D12_RANGE { Begin: 0, End: 0 };
        // SAFETY: unmapping what was mapped; nothing was written.
        unsafe { buffer.Unmap(0, Some(&none)) };
        Ok(pixels)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A clear on WARP reads back as the colour cleared to.
    #[test]
    fn warp_clears_and_reads_back() {
        let mut gpu = match Gpu::new(AdapterChoice::Warp) {
            Ok(gpu) => gpu,
            // CI's Windows runner has WARP: a missing device there is a
            // failure, not a skip, so a green job means the test ran.
            Err(e) if std::env::var_os("GITHUB_ACTIONS").is_some() => {
                panic!("no WARP device on CI: {e}")
            }
            Err(e) => {
                println!("no WARP device here ({e}); nothing to test");
                return;
            }
        };
        let target = RenderTarget::new(&gpu, 7, 3).unwrap();
        gpu.submit(|list| {
            target.record_clear(list, [1.0, 0.5, 0.0, 1.0]);
            Ok(())
        })
        .unwrap();
        let pixels = target.read_back(&mut gpu).unwrap();
        assert_eq!(pixels.len(), 7 * 3 * 4);
        // BGRA: blue 0, green 0.5 (128 or 127), red 255, alpha 255.
        for px in pixels.as_chunks::<4>().0 {
            assert_eq!(px[0], 0);
            assert!((127..=128).contains(&px[1]), "{px:?}");
            assert_eq!(px[2], 255);
            assert_eq!(px[3], 255);
        }
    }
}
