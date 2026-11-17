//! The `VMeshData`, `VMeshRef` and `VWireData` leaves
//! (`include/render/rendcomp.h`, packed, little-endian).
//!
//! * `VMeshData`: a 0x10-byte header {u8 flags, 3 bytes the reference
//!   never reads, u32 primitive type, u16 face groups, u16 indices, u16
//!   FVF, u16 vertices}, then 0xC-byte face groups {u32 material CRC, u16
//!   first vertex, u16 last vertex, u16 index count, u16 unread}, then the
//!   u16 indices, then the vertices at the FVF stride.
//! * `VMeshRef`: 0x3C bytes naming a range of one `VMeshData` by its name
//!   CRC, with bounds, centre and radius.
//! * `VWireData`: a 0x10-byte header naming a `VMeshData` by CRC, then u16
//!   line-list indices.

use super::fvf::{Channel, Fvf};
use super::{f32_at, u16_at, u32_at};

/// Bytes of the `VMeshData` header.
pub const DATA_HEADER_SIZE: usize = 0x10;
/// Bytes of one `VMeshData` face group.
pub const FACE_GROUP_SIZE: usize = 0xC;
/// Bytes of a `VMeshRef` record.
pub const REF_SIZE: usize = 0x3C;
/// Bytes of the `VWireData` header.
pub const WIRE_HEADER_SIZE: usize = 0x10;

/// Why a leaf does not parse.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VMeshError {
    /// Flag bit 0 is clear: "mesh in old format", which `VMeshDataLoad`
    /// refuses with a warning.
    OldFormat,
    /// The leaf is shorter than its header and counts need. The reference
    /// does not check and reads past the leaf later (divergence register).
    Truncated {
        /// Bytes the counts need.
        needed: usize,
        /// Bytes there are.
        len: usize,
    },
    /// A face group, vertex or index range lies outside the mesh data it
    /// names; the reference reads past the leaf there.
    OutOfRange,
}

impl core::fmt::Display for VMeshError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::OldFormat => f.write_str("VMeshData in old format"),
            Self::Truncated { needed, len } => {
                write!(f, "leaf of {len} bytes, its counts need {needed}")
            }
            Self::OutOfRange => f.write_str("range outside the mesh data"),
        }
    }
}

impl std::error::Error for VMeshError {}

fn need(bytes: &[u8], needed: usize) -> Result<(), VMeshError> {
    if bytes.len() < needed {
        Err(VMeshError::Truncated {
            needed,
            len: bytes.len(),
        })
    } else {
        Ok(())
    }
}

/// The 0x10-byte `VMeshData` header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VMeshDataHeader {
    /// +0x00. Bit 0 set: face-group-relative indices; clear: the old
    /// format, which the loader refuses.
    pub flags: u8,
    /// +0x01..+0x03, never read by the reference.
    pub unknown_01: [u8; 3],
    /// +0x04, the primitive type handed to `IMaterial::Submit` (4, a
    /// triangle list, in all the data).
    pub primitive_type: u32,
    /// +0x08.
    pub face_group_count: u16,
    /// +0x0A.
    pub index_count: u16,
    /// +0x0C.
    pub fvf: u16,
    /// +0x0E.
    pub vertex_count: u16,
}

impl VMeshDataHeader {
    /// The header at the start of `bytes`, or `None` for fewer than 0x10.
    #[must_use]
    pub fn read(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < DATA_HEADER_SIZE {
            return None;
        }
        Some(Self {
            flags: bytes[0],
            unknown_01: [bytes[1], bytes[2], bytes[3]],
            primitive_type: u32_at(bytes, 4),
            face_group_count: u16_at(bytes, 8),
            index_count: u16_at(bytes, 0xA),
            fvf: u16_at(bytes, 0xC),
            vertex_count: u16_at(bytes, 0xE),
        })
    }

    /// Whether flag bit 0 is set.
    #[must_use]
    pub const fn relative_indices(&self) -> bool {
        self.flags & 1 != 0
    }

    /// The vertex format.
    #[must_use]
    pub const fn fvf(&self) -> Fvf {
        Fvf(self.fvf as u32)
    }

    /// Where the indices start: after the header and the face groups.
    #[must_use]
    pub const fn indices_offset(&self) -> usize {
        DATA_HEADER_SIZE + self.face_group_count as usize * FACE_GROUP_SIZE
    }

    /// Where the vertices start: after the indices.
    #[must_use]
    pub const fn vertices_offset(&self) -> usize {
        self.indices_offset() + self.index_count as usize * 2
    }
}

/// One 0xC-byte face group of a `VMeshData`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VMeshFaceGroup {
    /// +0x00, the key of the material in the material library.
    pub material_crc: u32,
    /// +0x04, added to each of the group's indices; the first vertex the
    /// submit names.
    pub vertex_begin: u16,
    /// +0x06, the last vertex: the submit count is `end - begin + 1`.
    pub vertex_end: u16,
    /// +0x08.
    pub index_count: u16,
    /// +0x0A, never read (0xCC in all the data: exporter padding).
    pub unknown_0a: u16,
}

impl VMeshFaceGroup {
    fn read(bytes: &[u8]) -> Self {
        Self {
            material_crc: u32_at(bytes, 0),
            vertex_begin: u16_at(bytes, 4),
            vertex_end: u16_at(bytes, 6),
            index_count: u16_at(bytes, 8),
            unknown_0a: u16_at(bytes, 0xA),
        }
    }

    /// The vertex count the submit passes, `end - begin + 1` in `int`.
    #[must_use]
    pub fn vertex_span(&self) -> i32 {
        i32::from(self.vertex_end) - i32::from(self.vertex_begin) + 1
    }
}

/// Where a channel of a vertex range starts and how it steps
/// (`VMeshFillChannelRecord`). `offset` counts from the first vertex of the
/// mesh.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChannelRecord {
    /// Byte offset of the first element in the vertex bytes.
    pub offset: u64,
    /// The table's element size.
    pub element_bytes: u32,
    /// The FVF stride.
    pub stride: u32,
    /// Vertices in the range.
    pub count: u32,
}

/// A parsed `VMeshData` leaf.
#[derive(Clone, Debug, PartialEq)]
pub struct VMeshData {
    /// The header.
    pub header: VMeshDataHeader,
    /// `face_group_count` face groups.
    pub face_groups: Vec<VMeshFaceGroup>,
    /// `index_count` indices, relative to their face group's first vertex.
    pub indices: Vec<u16>,
    /// `vertex_count` vertices at the FVF stride.
    pub vertices: Vec<u8>,
    /// Bytes after the vertices, which nothing reads (none in the data).
    pub trailing: Vec<u8>,
}

impl VMeshData {
    /// Parses a leaf as `VMeshDataLoad` accepts it: flag bit 0 must be set
    /// (it is the one byte the loader tests), and here the leaf must also
    /// hold everything its counts name.
    ///
    /// # Errors
    ///
    /// [`VMeshError::OldFormat`], or [`VMeshError::Truncated`] where the
    /// reference would read past the leaf.
    pub fn parse(bytes: &[u8]) -> Result<Self, VMeshError> {
        need(bytes, 1)?;
        if bytes[0] & 1 == 0 {
            return Err(VMeshError::OldFormat);
        }
        need(bytes, DATA_HEADER_SIZE)?;
        let header = VMeshDataHeader::read(bytes).ok_or(VMeshError::OutOfRange)?;
        let vertices_at = header.vertices_offset();
        let stride = header.fvf().stride() as usize;
        let end = vertices_at + header.vertex_count as usize * stride;
        need(bytes, end)?;
        let face_groups = bytes[DATA_HEADER_SIZE..header.indices_offset()]
            .as_chunks::<FACE_GROUP_SIZE>()
            .0
            .iter()
            .map(|chunk| VMeshFaceGroup::read(chunk))
            .collect();
        let indices = super::u16s(&bytes[header.indices_offset()..vertices_at]);
        Ok(Self {
            header,
            face_groups,
            indices,
            vertices: bytes[vertices_at..end].to_vec(),
            trailing: bytes[end..].to_vec(),
        })
    }

    /// The vertex format.
    #[must_use]
    pub fn fvf(&self) -> Fvf {
        self.header.fvf()
    }

    /// The vertex stride.
    #[must_use]
    pub fn stride(&self) -> u32 {
        self.fvf().stride()
    }

    /// `VMeshFillChannelRecord(header, start, count, channel)`: `None` when
    /// the format lacks the channel.
    #[must_use]
    pub fn channel_record(
        &self,
        channel: Channel,
        start: u32,
        count: u32,
    ) -> Option<ChannelRecord> {
        let fvf = self.fvf();
        if !fvf.has_channel(channel) {
            return None;
        }
        let stride = fvf.stride();
        Some(ChannelRecord {
            offset: u64::from(start) * u64::from(stride) + u64::from(fvf.channel_offset(channel)),
            element_bytes: channel.element_bytes(),
            stride,
            count,
        })
    }

    /// The element bytes of `channel` for vertices `start..start + count`,
    /// read where the reference's channel record points. `Ok(None)` when
    /// the format lacks the channel.
    ///
    /// # Errors
    ///
    /// [`VMeshError::OutOfRange`] when an element lies past the vertices.
    pub fn channel_bytes(
        &self,
        channel: Channel,
        start: u32,
        count: u32,
    ) -> Result<Option<Vec<&[u8]>>, VMeshError> {
        let Some(record) = self.channel_record(channel, start, count) else {
            return Ok(None);
        };
        (0..u64::from(count))
            .map(|i| {
                let at = record.offset + i * u64::from(record.stride);
                let end = at + u64::from(record.element_bytes);
                usize::try_from(at)
                    .ok()
                    .zip(usize::try_from(end).ok())
                    .and_then(|(at, end)| self.vertices.get(at..end))
                    .ok_or(VMeshError::OutOfRange)
            })
            .collect::<Result<Vec<_>, _>>()
            .map(Some)
    }

    /// The positions of every vertex (channel 1), bit for bit.
    ///
    /// # Errors
    ///
    /// [`VMeshError::OutOfRange`] as for [`Self::channel_bytes`].
    pub fn positions(&self) -> Result<Option<Vec<[f32; 3]>>, VMeshError> {
        self.vec3s(Channel::Position)
    }

    /// The normals of every vertex (channel 8).
    ///
    /// # Errors
    ///
    /// [`VMeshError::OutOfRange`] as for [`Self::channel_bytes`].
    pub fn normals(&self) -> Result<Option<Vec<[f32; 3]>>, VMeshError> {
        self.vec3s(Channel::Normal)
    }

    /// Texture set 0 (`set` 0, channel 2) or 1 (`set` 1, channel 4) of every
    /// vertex.
    ///
    /// # Errors
    ///
    /// [`VMeshError::OutOfRange`] as for [`Self::channel_bytes`].
    pub fn tex_coords(&self, set: u32) -> Result<Option<Vec<[f32; 2]>>, VMeshError> {
        let channel = if set == 0 {
            Channel::Tex0
        } else {
            Channel::Tex1
        };
        Ok(self
            .channel_bytes(channel, 0, u32::from(self.header.vertex_count))?
            .map(|items| items.iter().map(|b| [f32_at(b, 0), f32_at(b, 4)]).collect()))
    }

    /// The diffuse colours of every vertex (channel 0x10), as stored.
    ///
    /// # Errors
    ///
    /// [`VMeshError::OutOfRange`] as for [`Self::channel_bytes`].
    pub fn diffuse(&self) -> Result<Option<Vec<u32>>, VMeshError> {
        Ok(self
            .channel_bytes(Channel::Diffuse, 0, u32::from(self.header.vertex_count))?
            .map(|items| items.iter().map(|b| u32_at(b, 0)).collect()))
    }

    fn vec3s(&self, channel: Channel) -> Result<Option<Vec<[f32; 3]>>, VMeshError> {
        Ok(self
            .channel_bytes(channel, 0, u32::from(self.header.vertex_count))?
            .map(|items| {
                items
                    .iter()
                    .map(|b| [f32_at(b, 0), f32_at(b, 4), f32_at(b, 8)])
                    .collect()
            }))
    }

    /// The indices as `VMeshGroupEnsureIb` copies them into the index
    /// buffer: each face group's `index_count` indices in turn, with the
    /// group's first vertex added (16-bit wrapping).
    ///
    /// # Errors
    ///
    /// [`VMeshError::OutOfRange`] when the face groups name more indices
    /// than there are; the reference reads past them.
    pub fn buffer_indices(&self) -> Result<Vec<u16>, VMeshError> {
        let mut out = Vec::with_capacity(self.indices.len());
        let mut source = self.indices.iter();
        for group in &self.face_groups {
            for _ in 0..group.index_count {
                let index = source.next().ok_or(VMeshError::OutOfRange)?;
                out.push(group.vertex_begin.wrapping_add(*index));
            }
        }
        Ok(out)
    }
}

/// The 0x3C-byte `VMeshRef` record of a `VMeshPart` node.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct VMeshRef {
    /// +0x00, never read (0x3C, the record size, in all the data).
    pub unknown_00: u32,
    /// +0x04, the name CRC of the `VMeshData` it draws from.
    pub mesh_crc: u32,
    /// +0x08.
    pub vertex_start: u16,
    /// +0x0A.
    pub vertex_count: u16,
    /// +0x0C.
    pub index_start: u16,
    /// +0x0E.
    pub index_count: u16,
    /// +0x10.
    pub face_group_start: u16,
    /// +0x12.
    pub face_group_count: u16,
    /// +0x14, the six floats `GetBounds` copies out, in file order.
    pub bounds: [f32; 6],
    /// +0x2C, the centre of the bounding sphere.
    pub center: [f32; 3],
    /// +0x38, its radius.
    pub radius: f32,
}

impl VMeshRef {
    /// The record at the start of `bytes`. A longer leaf is fine (the
    /// reference uses the first record only).
    ///
    /// # Errors
    ///
    /// [`VMeshError::Truncated`] for fewer than 0x3C bytes, where the
    /// reference reads past its buffer.
    pub fn parse(bytes: &[u8]) -> Result<Self, VMeshError> {
        need(bytes, REF_SIZE)?;
        let f = |at| f32_at(bytes, at);
        Ok(Self {
            unknown_00: u32_at(bytes, 0),
            mesh_crc: u32_at(bytes, 4),
            vertex_start: u16_at(bytes, 8),
            vertex_count: u16_at(bytes, 0xA),
            index_start: u16_at(bytes, 0xC),
            index_count: u16_at(bytes, 0xE),
            face_group_start: u16_at(bytes, 0x10),
            face_group_count: u16_at(bytes, 0x12),
            bounds: [f(0x14), f(0x18), f(0x1C), f(0x20), f(0x24), f(0x28)],
            center: [f(0x2C), f(0x30), f(0x34)],
            radius: f(0x38),
        })
    }

    /// The face groups of `data` this part draws.
    ///
    /// # Errors
    ///
    /// [`VMeshError::OutOfRange`] when the range passes the data's face
    /// groups.
    pub fn face_groups<'a>(&self, data: &'a VMeshData) -> Result<&'a [VMeshFaceGroup], VMeshError> {
        let start = usize::from(self.face_group_start);
        data.face_groups
            .get(start..start + usize::from(self.face_group_count))
            .ok_or(VMeshError::OutOfRange)
    }

    /// `FVMeshPart::TriangleCount`: each face group's index count divided
    /// by 3, summed.
    ///
    /// # Errors
    ///
    /// As [`Self::face_groups`].
    pub fn triangle_count(&self, data: &VMeshData) -> Result<i32, VMeshError> {
        Ok(self
            .face_groups(data)?
            .iter()
            .map(|g| i32::from(g.index_count) / 3)
            .sum())
    }
}

/// A `VWireData` leaf.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VWireData {
    /// +0x00, never read in RendComp (0x10, the header size, in all the
    /// data).
    pub unknown_00: u16,
    /// +0x02, never read in RendComp (0 in all the data).
    pub unknown_02: u16,
    /// +0x04, the name CRC of the `VMeshData` whose vertices it draws.
    pub mesh_crc: u32,
    /// +0x08, added to the mesh's base vertex when the indices are
    /// selected.
    pub index_bias: u16,
    /// +0x0A, not read by RendComp; the client's part cache reads it.
    pub remap_count: u16,
    /// +0x0C, the number of indices drawn.
    pub index_count: u16,
    /// +0x0E, the vertex count the draw passes.
    pub vertex_count: u16,
    /// `index_count` line-list indices.
    pub indices: Vec<u16>,
    /// Bytes after the indices (none in the data).
    pub trailing: Vec<u8>,
}

impl VWireData {
    /// Parses a `VWireData` leaf. The reference keeps the leaf as read and
    /// checks nothing.
    ///
    /// # Errors
    ///
    /// [`VMeshError::Truncated`] when the leaf is shorter than the header
    /// and its indices; the reference reads past it when it draws.
    pub fn parse(bytes: &[u8]) -> Result<Self, VMeshError> {
        need(bytes, WIRE_HEADER_SIZE)?;
        let index_count = u16_at(bytes, 0xC);
        let end = WIRE_HEADER_SIZE + usize::from(index_count) * 2;
        need(bytes, end)?;
        Ok(Self {
            unknown_00: u16_at(bytes, 0),
            unknown_02: u16_at(bytes, 2),
            mesh_crc: u32_at(bytes, 4),
            index_bias: u16_at(bytes, 8),
            remap_count: u16_at(bytes, 0xA),
            index_count,
            vertex_count: u16_at(bytes, 0xE),
            indices: super::u16s(&bytes[WIRE_HEADER_SIZE..end]),
            trailing: bytes[end..].to_vec(),
        })
    }
}
