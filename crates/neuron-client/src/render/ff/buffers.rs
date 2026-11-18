//! Vertex and index buffers as DX8RENDERER manages them (`FDx8RendererVb`,
//! `FDx8RendererIb`, the vertex pool and the stream cache in
//! `CppSource/DX8RENDERER/DX8RENDERER.cpp`), with the bytes the GPU would
//! hold kept on the CPU.
//!
//! Three kinds of vertex buffer exist: plain ones (`CreateVb`), dynamic
//! rings, and one pooled ring per vertex format (`CreateVb` with flag 1,
//! `LockVertexMemory`, and `CopyVertices` on handle -1), which grows in
//! blocks of 1,024 vertices and is never destroyed. A dynamic ring appends
//! each lock after the previous one (`NOOVERWRITE`) and starts again at 0
//! when the request does not fit (`DISCARD`). Index buffers are 16-bit;
//! handle -1 names the renderer's own dynamic scratch buffer of 0x4000
//! indices, into which `DrawIndexedPrimitiveVb` copies the caller's
//! indices.
//!
//! Offsets differ by kind, as in the C++: a vertex-buffer lock takes a byte
//! offset (used only by non-dynamic buffers) and returns a vertex index; an
//! index-buffer lock takes and returns index positions.

use std::collections::BTreeMap;
use std::fmt;

use super::arena::{Arena, Handle};
use super::fvf::Fvf;

/// The pooled rings grow in blocks of this many vertices.
pub const POOL_BLOCK: u32 = 0x400;

/// The scratch index buffer's capacity in indices.
pub const SCRATCH_INDICES: u32 = 0x4000;

/// The results the C++ returns for refused buffer calls.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BufferError {
    /// `E_FAIL` (0x80004005): no buffer, no GPU buffer, or a lock larger
    /// than the buffer.
    Fail,
    /// The handle names no live buffer (the C++ would use freed memory;
    /// divergence D206).
    Stale,
    /// A write outside the locked range (the C++ would write past it;
    /// divergence D206).
    OutOfRange,
}

impl BufferError {
    /// The `GENRESULT` the C++ returns.
    #[must_use]
    pub fn code(self) -> u32 {
        0x8000_4005
    }
}

impl fmt::Display for BufferError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Fail => "the buffer call failed (E_FAIL)",
            Self::Stale => "the handle names no live buffer",
            Self::OutOfRange => "the write is outside the locked range",
        })
    }
}

impl std::error::Error for BufferError {}

/// A vertex buffer.
#[derive(Clone, Debug, Default)]
pub struct VertexBuffer {
    pub fvf: Fvf,
    /// Capacity in vertices.
    pub count: u32,
    /// `D3DUSAGE_DYNAMIC`: a ring.
    pub dynamic: bool,
    /// One of the per-format pool rings.
    pub pooled: bool,
    /// `+0xc`: bytes used in the ring.
    used: u32,
    /// The locked byte range, if locked.
    locked: Option<(u32, u32)>,
    /// Whether the GPU buffer exists (`Gpu != 0`).
    live: bool,
    data: Vec<u8>,
}

impl VertexBuffer {
    fn stride(&self) -> u32 {
        self.fvf.stride()
    }

    /// `ResizeVb` and the pool's grow: a new, empty GPU buffer.
    fn recreate(&mut self, fvf: Fvf, count: u32) {
        self.fvf = fvf;
        self.count = count;
        self.used = 0;
        self.locked = None;
        self.live = true;
        self.data = vec![0; (fvf.stride() as usize) * (count as usize)];
    }

    /// The bytes the buffer holds.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.data
    }

    #[must_use]
    pub fn is_locked(&self) -> bool {
        self.locked.is_some()
    }
}

/// A 16-bit index buffer.
#[derive(Clone, Debug, Default)]
pub struct IndexBuffer {
    /// Capacity in indices.
    pub capacity: u32,
    pub dynamic: bool,
    /// The ring cursor in indices.
    cursor: u32,
    locked: Option<(u32, u32)>,
    live: bool,
    data: Vec<u16>,
}

impl IndexBuffer {
    fn recreate(&mut self, capacity: u32) {
        self.capacity = capacity;
        self.cursor = 0;
        self.locked = None;
        self.live = true;
        self.data = vec![0; capacity as usize];
    }

    #[must_use]
    pub fn indices(&self) -> &[u16] {
        &self.data
    }
}

pub type VbHandle = Handle<VertexBuffer>;
pub type IbHandle = Handle<IndexBuffer>;

/// Which index buffer a call names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IndexSource {
    Buffer(IbHandle),
    /// Handle -1.
    Scratch,
}

/// A successful lock.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Lock {
    /// Where the locked range starts: a vertex index for a vertex buffer,
    /// an index position for an index buffer.
    pub start: u32,
    /// The locked range in bytes (vertex buffer) or indices (index buffer).
    pub offset: u32,
    pub len: u32,
}

/// `FVbListing` of a `LockVertexMemory`: the pooled ring, its format and
/// the first vertex written.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VertexListing {
    pub buffer: VbHandle,
    pub fvf: Fvf,
    pub base_vertex: u32,
}

/// `FDx8RendererStreamCache`: what the next draw binds.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Streams {
    pub vertex_buffer: Option<VbHandle>,
    /// A format that overrides the buffer's (0 for none).
    pub fvf: u32,
    pub index_buffer: Option<IndexSource>,
    /// `0xFFFF_FFFF` after a reset, as in the C++.
    pub base_vertex: u32,
}

/// The buffers of a device.
#[derive(Debug)]
pub struct Buffers {
    vbs: Arena<VertexBuffer>,
    /// The pool rings by vertex format code.
    pool: BTreeMap<u32, VbHandle>,
    ibs: Arena<IndexBuffer>,
    scratch: IndexBuffer,
    pub streams: Streams,
    /// `USE_SYSLOCK`; it changes only the Direct3D lock flags.
    pub sys_lock: bool,
}

impl Default for Buffers {
    fn default() -> Self {
        Self::new()
    }
}

impl Buffers {
    #[must_use]
    pub fn new() -> Buffers {
        let mut scratch = IndexBuffer {
            dynamic: true,
            ..IndexBuffer::default()
        };
        scratch.recreate(SCRATCH_INDICES);
        Buffers {
            vbs: Arena::new(),
            pool: BTreeMap::new(),
            ibs: Arena::new(),
            scratch,
            streams: Streams {
                base_vertex: 0xFFFF_FFFF,
                ..Streams::default()
            },
            sys_lock: false,
        }
    }

    pub fn vb(&self, h: VbHandle) -> Result<&VertexBuffer, BufferError> {
        self.vbs.get(h).map_err(|_| BufferError::Stale)
    }

    fn vb_mut(&mut self, h: VbHandle) -> Result<&mut VertexBuffer, BufferError> {
        self.vbs.get_mut(h).map_err(|_| BufferError::Stale)
    }

    pub fn ib(&self, source: IndexSource) -> Result<&IndexBuffer, BufferError> {
        match source {
            IndexSource::Scratch => Ok(&self.scratch),
            IndexSource::Buffer(h) => self.ibs.get(h).map_err(|_| BufferError::Stale),
        }
    }

    fn ib_mut(&mut self, source: IndexSource) -> Result<&mut IndexBuffer, BufferError> {
        match source {
            IndexSource::Scratch => Ok(&mut self.scratch),
            IndexSource::Buffer(h) => self.ibs.get_mut(h).map_err(|_| BufferError::Stale),
        }
    }

    /// `SelectVertexStream`.
    fn select_stream(&mut self, vb: Option<VbHandle>, fvf: u32) {
        self.streams.vertex_buffer = vb;
        self.streams.fvf = fvf;
    }

    /// `FindOrInsertPoolVb` and `GrowPoolVb`: the pool ring for `fvf`, with
    /// room for `count` vertices; a grown ring that was selected is
    /// deselected.
    pub fn pool_vb(&mut self, fvf: Fvf, count: u32) -> VbHandle {
        let h = *self.pool.entry(fvf.0).or_insert_with(|| {
            self.vbs.insert(VertexBuffer {
                dynamic: true,
                pooled: true,
                ..VertexBuffer::default()
            })
        });
        let vb = self.vbs.get_mut(h).expect("pool rings are never removed");
        if !(vb.live && vb.count >= count) {
            let mut new_count = vb.count;
            if new_count == 0 {
                new_count = POOL_BLOCK;
            }
            if new_count < count {
                new_count = count.wrapping_add(POOL_BLOCK - 1);
            }
            new_count = (new_count >> 10) << 10;
            vb.recreate(fvf, new_count);
            if self.streams.vertex_buffer == Some(h) {
                self.select_vb(None).ok();
            }
        }
        h
    }

    /// `CreateVb`: flag 1 gives the pool ring for `fvf`, flag 2 is
    /// `D3DUSAGE_SOFTWAREPROCESSING` (no effect here).
    pub fn create_vb(&mut self, fvf: Fvf, count: u32, flags: u32) -> VbHandle {
        if flags & 1 != 0 {
            let h = self.pool_vb(fvf, count);
            self.select_stream(Some(h), 0);
            return h;
        }
        let h = self.vbs.insert(VertexBuffer::default());
        self.resize_vb(h, fvf, count).expect("a new handle is live");
        h
    }

    /// `DestroyVb`: a pool ring is not destroyed; a selected buffer is
    /// deselected first.
    pub fn destroy_vb(&mut self, h: VbHandle) -> Result<(), BufferError> {
        if self.vb(h)?.pooled {
            return Ok(());
        }
        if self.streams.vertex_buffer == Some(h) {
            self.select_vb(None).ok();
        }
        self.vbs.remove(h).map_err(|_| BufferError::Stale)?;
        Ok(())
    }

    /// `ResizeVb`: a new, empty buffer of `count` vertices of `fvf`.
    pub fn resize_vb(&mut self, h: VbHandle, fvf: Fvf, count: u32) -> Result<(), BufferError> {
        self.vb_mut(h)?.recreate(fvf, count);
        if self.streams.vertex_buffer == Some(h) {
            self.select_vb(None).ok();
        }
        Ok(())
    }

    /// `LockVb`: `offset` is a byte offset, used by buffers that are not
    /// dynamic; a ring places the lock itself. Returns the first vertex.
    pub fn lock_vb(&mut self, h: VbHandle, offset: u32, count: u32) -> Result<Lock, BufferError> {
        let vb = self.vb_mut(h)?;
        if !vb.live || vb.count < count {
            return Err(BufferError::Fail);
        }
        let stride = vb.stride();
        let capacity = stride.wrapping_mul(vb.count);
        let len = count.wrapping_mul(stride);
        let mut start = offset;
        if vb.dynamic {
            if vb.used.wrapping_add(len) > capacity {
                vb.used = 0;
                start = 0;
            } else {
                start = vb.used;
            }
        }
        if u64::from(start) + u64::from(len) > u64::from(capacity) {
            // Direct3D refuses a range past the buffer.
            return Err(BufferError::Fail);
        }
        vb.locked = Some((start, len));
        vb.used = start + len;
        Ok(Lock {
            start: start.checked_div(stride).unwrap_or(0),
            offset: start,
            len,
        })
    }

    /// Writes `bytes` at byte `at` of a locked vertex buffer, inside the
    /// locked range.
    pub fn write_vb(&mut self, h: VbHandle, at: u32, bytes: &[u8]) -> Result<(), BufferError> {
        let vb = self.vb_mut(h)?;
        let (start, len) = vb.locked.ok_or(BufferError::OutOfRange)?;
        let end = at as usize + bytes.len();
        if (at as usize) < start as usize || end > (start + len) as usize {
            return Err(BufferError::OutOfRange);
        }
        vb.data[at as usize..end].copy_from_slice(bytes);
        Ok(())
    }

    /// `UnlockVb`.
    pub fn unlock_vb(&mut self, h: VbHandle) -> Result<(), BufferError> {
        let vb = self.vb_mut(h)?;
        if !vb.live {
            return Err(BufferError::Fail);
        }
        vb.locked = None;
        Ok(())
    }

    /// `LockVertexMemory`: room for `count` vertices of `fvf` in the pool
    /// ring, which becomes the selected stream.
    pub fn lock_vertex_memory(
        &mut self,
        fvf: Fvf,
        count: u32,
    ) -> Result<(VertexListing, Lock), BufferError> {
        let h = self.pool_vb(fvf, count);
        self.select_stream(Some(h), 0);
        let lock = self.lock_vb(h, 0, count)?;
        Ok((
            VertexListing {
                buffer: h,
                fvf,
                base_vertex: lock.start,
            },
            lock,
        ))
    }

    /// `SelectVb`; `None` clears the stream and returns `E_FAIL`, as the
    /// C++ does.
    pub fn select_vb(&mut self, h: Option<VbHandle>) -> Result<(), BufferError> {
        let Some(h) = h else {
            self.streams = Streams {
                base_vertex: 0xFFFF_FFFF,
                ..Streams::default()
            };
            return Err(BufferError::Fail);
        };
        if !self.vb(h)?.live {
            return Err(BufferError::Fail);
        }
        self.select_stream(Some(h), 0);
        Ok(())
    }

    /// `GetVbInfo`: format and capacity.
    pub fn vb_info(&self, h: VbHandle) -> Result<(Fvf, u32), BufferError> {
        let vb = self.vb(h)?;
        if !vb.live {
            return Err(BufferError::Fail);
        }
        Ok((vb.fvf, vb.count))
    }

    /// `CreateIb`: flag 1 makes a ring.
    pub fn create_ib(&mut self, count: u32, flags: u32) -> IbHandle {
        let h = self.ibs.insert(IndexBuffer {
            dynamic: flags & 1 != 0,
            ..IndexBuffer::default()
        });
        self.resize_ib(IndexSource::Buffer(h), count)
            .expect("a new handle is live");
        h
    }

    /// `DestroyIb`: a selected buffer is deselected first. The scratch
    /// buffer cannot be destroyed.
    pub fn destroy_ib(&mut self, h: IbHandle) -> Result<(), BufferError> {
        self.ibs.get(h).map_err(|_| BufferError::Stale)?;
        if self.streams.index_buffer == Some(IndexSource::Buffer(h)) {
            self.select_ib(None, 0).ok();
        }
        self.ibs.remove(h).map_err(|_| BufferError::Stale)?;
        Ok(())
    }

    /// `ResizeIb`: a live buffer of the same capacity is kept.
    pub fn resize_ib(&mut self, source: IndexSource, count: u32) -> Result<(), BufferError> {
        let ib = self.ib_mut(source)?;
        if ib.live && ib.capacity == count {
            return Ok(());
        }
        ib.recreate(count);
        Ok(())
    }

    /// `LockRange` (`LockIb`, `CopyIndices`): a ring places the lock at its
    /// cursor or starts again at 0.
    pub fn lock_ib(
        &mut self,
        source: IndexSource,
        start: u32,
        count: u32,
    ) -> Result<Lock, BufferError> {
        let ib = self.ib_mut(source)?;
        if !ib.live || count > ib.capacity {
            return Err(BufferError::Fail);
        }
        let mut start = start;
        if ib.dynamic {
            if ib.cursor.wrapping_add(count) > ib.capacity {
                ib.cursor = 0;
                start = 0;
            } else {
                start = ib.cursor;
            }
        }
        if u64::from(start) + u64::from(count) > u64::from(ib.capacity) {
            return Err(BufferError::Fail);
        }
        ib.locked = Some((start, count));
        ib.cursor = start + count;
        Ok(Lock {
            start,
            offset: start,
            len: count,
        })
    }

    /// Writes indices at position `at` of a locked index buffer.
    pub fn write_ib(
        &mut self,
        source: IndexSource,
        at: u32,
        indices: &[u16],
    ) -> Result<(), BufferError> {
        let ib = self.ib_mut(source)?;
        let (start, len) = ib.locked.ok_or(BufferError::OutOfRange)?;
        let end = at as usize + indices.len();
        if (at as usize) < start as usize || end > (start + len) as usize {
            return Err(BufferError::OutOfRange);
        }
        ib.data[at as usize..end].copy_from_slice(indices);
        Ok(())
    }

    pub fn unlock_ib(&mut self, source: IndexSource) -> Result<(), BufferError> {
        self.ib_mut(source)?.locked = None;
        Ok(())
    }

    /// `CopyIndices`: lock, copy, unlock; returns where the indices went.
    pub fn copy_indices(
        &mut self,
        source: IndexSource,
        start: u32,
        indices: &[u16],
    ) -> Result<u32, BufferError> {
        let count = u32::try_from(indices.len()).map_err(|_| BufferError::Fail)?;
        let lock = self.lock_ib(source, start, count)?;
        self.write_ib(source, lock.start, indices)?;
        self.unlock_ib(source)?;
        Ok(lock.start)
    }

    /// `SelectIb`; `None` clears the index stream and returns `E_FAIL`.
    pub fn select_ib(
        &mut self,
        source: Option<IndexSource>,
        base_vertex: u32,
    ) -> Result<(), BufferError> {
        let Some(source) = source else {
            self.streams.index_buffer = None;
            self.streams.base_vertex = 0;
            return Err(BufferError::Fail);
        };
        if !self.ib(source)?.live {
            return Err(BufferError::Fail);
        }
        self.streams.index_buffer = Some(source);
        self.streams.base_vertex = base_vertex;
        Ok(())
    }

    /// `FDx8RendererStreamCache::Reset`, after a user-pointer draw.
    pub fn release_streams(&mut self) {
        self.streams = Streams {
            base_vertex: 0xFFFF_FFFF,
            ..Streams::default()
        };
    }

    /// `VbMgrReleaseAll` and the device loss: every GPU buffer is gone and
    /// the streams reset; buffers keep their handles and are made again by
    /// a resize.
    pub fn release_all(&mut self) {
        self.streams = Streams {
            base_vertex: 0xFFFF_FFFF,
            ..Streams::default()
        };
        for &h in self.pool.values() {
            if let Ok(vb) = self.vbs.get_mut(h) {
                vb.live = false;
                vb.locked = None;
                vb.count = 0;
                vb.data.clear();
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const XYZ_DIFFUSE: Fvf = Fvf(0x42); // 16 bytes

    #[test]
    fn plain_buffers_lock_at_the_callers_byte_offset() {
        let mut b = Buffers::new();
        let h = b.create_vb(XYZ_DIFFUSE, 10, 0);
        let lock = b.lock_vb(h, 32, 2).unwrap();
        assert_eq!(
            lock,
            Lock {
                start: 2,
                offset: 32,
                len: 32
            }
        );
        b.write_vb(h, 32, &[7; 32]).unwrap();
        assert_eq!(b.write_vb(h, 0, &[1]), Err(BufferError::OutOfRange));
        b.unlock_vb(h).unwrap();
        assert_eq!(&b.vb(h).unwrap().bytes()[32..64], &[7; 32]);
        assert_eq!(b.lock_vb(h, 0, 11), Err(BufferError::Fail));
        assert_eq!(b.vb_info(h), Ok((XYZ_DIFFUSE, 10)));
    }

    #[test]
    fn pool_rings_append_then_discard() {
        let mut b = Buffers::new();
        let (listing, lock) = b.lock_vertex_memory(XYZ_DIFFUSE, 1000).unwrap();
        assert_eq!(listing.base_vertex, 0);
        assert_eq!(b.vb(listing.buffer).unwrap().count, POOL_BLOCK);
        assert_eq!(lock.len, 16_000);
        b.unlock_vb(listing.buffer).unwrap();
        // 24 more fit; the next 25 do not and start again at 0.
        let (l2, _) = b.lock_vertex_memory(XYZ_DIFFUSE, 24).unwrap();
        assert_eq!(l2.base_vertex, 1000);
        let (l3, _) = b.lock_vertex_memory(XYZ_DIFFUSE, 25).unwrap();
        assert_eq!(l3.base_vertex, 0);
        assert_eq!(b.streams.vertex_buffer, Some(listing.buffer));
        // A larger request grows the ring in whole blocks.
        let (l4, _) = b.lock_vertex_memory(XYZ_DIFFUSE, 2000).unwrap();
        assert_eq!(l4.buffer, listing.buffer);
        assert_eq!(b.vb(l4.buffer).unwrap().count, 2048);
        // Pool rings survive destroy.
        b.destroy_vb(l4.buffer).unwrap();
        assert!(b.vb(l4.buffer).is_ok());
    }

    #[test]
    fn index_rings_and_the_scratch_buffer() {
        let mut b = Buffers::new();
        assert_eq!(
            b.ib(IndexSource::Scratch).unwrap().capacity,
            SCRATCH_INDICES
        );
        let at = b.copy_indices(IndexSource::Scratch, 0, &[1, 2, 3]).unwrap();
        assert_eq!(at, 0);
        let at = b.copy_indices(IndexSource::Scratch, 0, &[4, 5, 6]).unwrap();
        assert_eq!(at, 3);
        assert_eq!(
            &b.ib(IndexSource::Scratch).unwrap().indices()[..6],
            &[1, 2, 3, 4, 5, 6]
        );
        let h = b.create_ib(8, 0);
        let src = IndexSource::Buffer(h);
        assert_eq!(b.copy_indices(src, 5, &[9, 9, 9]).unwrap(), 5);
        assert_eq!(b.lock_ib(src, 0, 9), Err(BufferError::Fail));
        b.select_ib(Some(src), 4).unwrap();
        assert_eq!(b.streams.base_vertex, 4);
        b.destroy_ib(h).unwrap();
        assert_eq!(b.streams.index_buffer, None);
        assert_eq!(b.ib(src).unwrap_err(), BufferError::Stale);
    }

    #[test]
    fn selecting_nothing_resets_the_stream() {
        let mut b = Buffers::new();
        let h = b.create_vb(XYZ_DIFFUSE, 4, 0);
        b.select_vb(Some(h)).unwrap();
        assert_eq!(b.select_vb(None), Err(BufferError::Fail));
        assert_eq!(b.streams.vertex_buffer, None);
        assert_eq!(b.streams.base_vertex, 0xFFFF_FFFF);
        b.destroy_vb(h).unwrap();
        assert_eq!(b.destroy_vb(h), Err(BufferError::Stale));
    }
}
