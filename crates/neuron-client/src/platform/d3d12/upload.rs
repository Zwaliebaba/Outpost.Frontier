//! The upload ring (record 0004, point 8): persistently mapped upload-heap
//! buffers that a frame fills front to back with the bytes it sends to the
//! GPU, user-pointer vertices and indices, constant buffers and copy
//! sources. A frame that outgrows the ring takes another chunk; once the
//! GPU is idle the ring keeps its first chunk and starts again.

use windows::Win32::Graphics::Direct3D12::{
    D3D12_HEAP_FLAG_NONE, D3D12_HEAP_PROPERTIES, D3D12_HEAP_TYPE, D3D12_HEAP_TYPE_UPLOAD,
    D3D12_RANGE, D3D12_RESOURCE_DESC, D3D12_RESOURCE_DIMENSION_BUFFER, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATES, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    ID3D12Device, ID3D12Resource,
};
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC};

use super::{Context, GpuError};

/// The size of a chunk unless a single allocation needs more.
const CHUNK: u64 = 4 << 20;

/// A buffer resource of `size` bytes on `heap` in `state`.
pub(super) fn buffer(
    device: &ID3D12Device,
    heap: D3D12_HEAP_TYPE,
    size: u64,
    state: D3D12_RESOURCE_STATES,
) -> Result<ID3D12Resource, GpuError> {
    let heap_props = D3D12_HEAP_PROPERTIES {
        Type: heap,
        ..Default::default()
    };
    let desc = D3D12_RESOURCE_DESC {
        Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
        Width: size.max(1),
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
    let mut resource: Option<ID3D12Resource> = None;
    // SAFETY: the descriptions are fully initialised and `resource` is a
    // valid out slot.
    unsafe {
        device.CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &desc,
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

/// A default-heap buffer starts in the common state.
pub(super) const DEFAULT_BUFFER_STATE: D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_COMMON;

struct Chunk {
    resource: ID3D12Resource,
    /// The mapped start; the mapping lives as long as the resource.
    base: *mut u8,
    size: u64,
    used: u64,
    gpu: u64,
}

impl Chunk {
    fn new(device: &ID3D12Device, size: u64) -> Result<Chunk, GpuError> {
        let resource = buffer(
            device,
            D3D12_HEAP_TYPE_UPLOAD,
            size,
            D3D12_RESOURCE_STATE_GENERIC_READ,
        )?;
        let mut data: *mut std::ffi::c_void = std::ptr::null_mut();
        let none = D3D12_RANGE { Begin: 0, End: 0 };
        // SAFETY: an upload buffer may stay mapped for its whole life; the
        // CPU only writes, so the read range is empty.
        unsafe { resource.Map(0, Some(&none), Some(&mut data)) }.at("Map")?;
        // SAFETY: a live buffer resource.
        let gpu = unsafe { resource.GetGPUVirtualAddress() };
        Ok(Chunk {
            resource,
            base: data.cast(),
            size,
            used: 0,
            gpu,
        })
    }
}

/// Where an allocation went.
#[derive(Clone)]
pub struct Allocation {
    pub resource: ID3D12Resource,
    /// The offset in `resource`.
    pub offset: u64,
    /// The GPU address of the first byte.
    pub gpu_address: u64,
}

/// The ring.
#[derive(Default)]
pub struct UploadRing {
    chunks: Vec<Chunk>,
}

impl UploadRing {
    /// Copies `bytes` into the ring at an offset aligned to `align` (a power
    /// of two).
    pub fn push(
        &mut self,
        device: &ID3D12Device,
        bytes: &[u8],
        align: u64,
    ) -> Result<Allocation, GpuError> {
        let len = bytes.len() as u64;
        let fits = |c: &Chunk| c.used.next_multiple_of(align) + len <= c.size;
        if !self.chunks.last().is_some_and(fits) {
            self.chunks
                .push(Chunk::new(device, CHUNK.max(len + align))?);
        }
        let chunk = self.chunks.last_mut().expect("a chunk was just ensured");
        let offset = chunk.used.next_multiple_of(align);
        // SAFETY: `offset + len` is inside the mapped chunk (checked by
        // `fits`), and the GPU does not read this range before the command
        // list that uses it runs, after this copy.
        unsafe {
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr(),
                chunk.base.add(offset as usize),
                bytes.len(),
            );
        }
        chunk.used = offset + len;
        Ok(Allocation {
            resource: chunk.resource.clone(),
            offset,
            gpu_address: chunk.gpu + offset,
        })
    }

    /// Starts again once the GPU has finished with everything pushed: the
    /// first chunk is kept, the others released.
    pub fn reset(&mut self) {
        self.chunks.truncate(1);
        if let Some(c) = self.chunks.first_mut() {
            c.used = 0;
        }
    }
}
