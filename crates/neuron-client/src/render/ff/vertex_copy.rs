//! `CopyVertexData`: DX8RENDERER's conversion of a caller's vertex source
//! (`FRpVertexSrcDesc`: separate or interleaved streams, sequential or
//! indexed) into a vertex buffer's format (`CopyVertexData` and the
//! converters before it in `CppSource/DX8RENDERER/DX8RENDERER.cpp`).
//!
//! The choice of converter is the reference's: the selector
//! `(source format == destination format) | (flags & 6)`, two 65-entry
//! jump tables indexed by the destination format, and the hard-wired
//! source/destination pairs of the two dispatchers. The specialised
//! converters read their channels at fixed source strides (12 bytes for
//! position and normal, 4 for diffuse, 8 for a texture coordinate pair)
//! whatever the descriptor says, and leave the bytes between channels as
//! they were; many table slots are no-ops. All of that is kept.

use std::fmt;

/// One source stream (`FRpVertexStream`).
#[derive(Clone, Copy, Debug, Default)]
pub struct Stream<'a> {
    /// The stream's bytes from element 0.
    pub data: &'a [u8],
    /// Bytes of one element.
    pub byte_size: u32,
    /// Bytes between two elements.
    pub stride: u32,
    /// One element index per vertex, for an indexed source.
    pub index: &'a [u32],
    pub count: u32,
}

/// A vertex source (`FRpVertexSrcDesc`).
#[derive(Clone, Copy, Debug, Default)]
pub struct Source<'a> {
    /// 2: interleaved (the position stream holds whole vertices); 4:
    /// indexed.
    pub flags: u32,
    pub position: Stream<'a>,
    pub normal: Stream<'a>,
    pub diffuse: Stream<'a>,
    pub tex: [Stream<'a>; 2],
    pub fvf: u32,
    pub count: u32,
}

/// A read or write outside the slices given (the C++ reads and writes
/// through raw pointers; divergence D208).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CopyOutOfRange;

impl fmt::Display for CopyOutOfRange {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("the vertex copy reads or writes outside its buffers")
    }
}

impl std::error::Error for CopyOutOfRange {}

/// The converters of the reference.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Conv {
    NoOp,
    SeqGeneric,
    IndexedGeneric,
    /// A specialised converter: sequential or indexed, the destination
    /// stride, and the channels with their destination offsets.
    Special {
        indexed: bool,
        stride: u32,
        channels: &'static [(Channel, u32)],
    },
}

/// A channel a specialised converter copies.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Channel {
    Position,
    Normal,
    Diffuse,
    Tex0,
    Tex1,
}

impl Channel {
    /// The fixed size and source stride the specialised converters use.
    fn size(self) -> u32 {
        match self {
            Channel::Position | Channel::Normal => 12,
            Channel::Diffuse => 4,
            Channel::Tex0 | Channel::Tex1 => 8,
        }
    }

    fn stream<'s, 'a>(self, src: &'s Source<'a>) -> &'s Stream<'a> {
        match self {
            Channel::Position => &src.position,
            Channel::Normal => &src.normal,
            Channel::Diffuse => &src.diffuse,
            Channel::Tex0 => &src.tex[0],
            Channel::Tex1 => &src.tex[1],
        }
    }
}

use Channel::{Diffuse, Normal, Position, Tex0, Tex1};

const fn special(indexed: bool, stride: u32, channels: &'static [(Channel, u32)]) -> Conv {
    Conv::Special {
        indexed,
        stride,
        channels,
    }
}

const PN: &[(Channel, u32)] = &[(Position, 0), (Normal, 0xC)];
const PNT_18: &[(Channel, u32)] = &[(Position, 0), (Normal, 0xC), (Tex0, 0x18)];
const PNT_1C: &[(Channel, u32)] = &[(Position, 0), (Normal, 0xC), (Tex0, 0x1C)];
const PNDT: &[(Channel, u32)] = &[(Position, 0), (Normal, 0xC), (Diffuse, 0x18), (Tex0, 0x1C)];
const PNTT_1C: &[(Channel, u32)] = &[(Position, 0), (Normal, 0xC), (Tex0, 0x1C), (Tex1, 0x24)];
const PNTT_18: &[(Channel, u32)] = &[(Position, 0), (Normal, 0xC), (Tex0, 0x18), (Tex1, 0x20)];

pub const SEQ_POS_NORM_TEX1_S20: Conv = special(false, 0x20, PNT_18);
pub const SEQ_POS_NORM_TEX1_S28: Conv = special(false, 0x28, PNT_18);
pub const SEQ_POS_NORM_TEX1_S2C: Conv = special(false, 0x2C, PNT_1C);
pub const SEQ_POS_NORM_TEX2_S2C: Conv = special(false, 0x2C, PNTT_1C);
pub const IDX_POS_NORM_S18: Conv = special(true, 0x18, PN);
pub const IDX_POS_NORM_S20: Conv = special(true, 0x20, PN);
pub const IDX_POS_NORM_S28: Conv = special(true, 0x28, PN);
pub const IDX_POS_NORM_S2C: Conv = special(true, 0x2C, PN);
pub const IDX_POS_NORM_TEX1_S20: Conv = special(true, 0x20, PNT_18);
pub const IDX_POS_NORM_TEX1_S28: Conv = special(true, 0x28, PNT_18);
pub const IDX_POS_NORM_TEX1_S2C: Conv = special(true, 0x2C, PNT_1C);
pub const IDX_POS_NORM_DIFF_TEX1_S24: Conv = special(true, 0x24, PNDT);
pub const IDX_POS_NORM_DIFF_TEX1_S2C: Conv = special(true, 0x2C, PNDT);
pub const IDX_POS_NORM_TEX2_S28: Conv = special(true, 0x28, PNTT_18);
pub const IDX_POS_NORM_TEX2_S2C: Conv = special(true, 0x2C, PNTT_1C);

/// The non-no-op slots of `kFamilyASeqJumpTable`.
const SEQ_SLOTS: &[(usize, Conv)] = &[
    (0, Conv::SeqGeneric),
    (2, Conv::SeqGeneric),
    (4, Conv::SeqGeneric),
    (6, Conv::SeqGeneric),
    (12, Conv::SeqGeneric),
    (14, Conv::SeqGeneric),
    (16, Conv::SeqGeneric),
    (18, SEQ_POS_NORM_TEX1_S20),
    (20, Conv::SeqGeneric),
    (22, Conv::SeqGeneric),
    (28, Conv::SeqGeneric),
    (30, Conv::SeqGeneric),
    (48, Conv::SeqGeneric),
    (50, Conv::SeqGeneric),
    (52, Conv::SeqGeneric),
    (54, Conv::SeqGeneric),
    (60, Conv::SeqGeneric),
    (62, Conv::SeqGeneric),
    (64, Conv::SeqGeneric),
];

/// The non-no-op slots of `kFamilyBIndexedJumpTable`.
const INDEXED_SLOTS: &[(usize, Conv)] = &[
    (0, Conv::IndexedGeneric),
    (2, IDX_POS_NORM_S18),
    (4, Conv::IndexedGeneric),
    (6, Conv::IndexedGeneric),
    (12, Conv::IndexedGeneric),
    (14, Conv::IndexedGeneric),
    (16, Conv::IndexedGeneric),
    (18, IDX_POS_NORM_TEX1_S20),
    (20, Conv::IndexedGeneric),
    (22, IDX_POS_NORM_DIFF_TEX1_S24),
    (28, Conv::IndexedGeneric),
    (30, Conv::IndexedGeneric),
    (48, Conv::IndexedGeneric),
    (50, IDX_POS_NORM_TEX2_S28),
    (52, Conv::IndexedGeneric),
    (54, Conv::IndexedGeneric),
    (60, Conv::IndexedGeneric),
    (62, Conv::IndexedGeneric),
    (64, Conv::IndexedGeneric),
];

/// Slot `i` of a jump table (`SEQ_SLOTS` or `INDEXED_SLOTS`).
#[must_use]
pub fn table_slot(indexed: bool, i: usize) -> Conv {
    let slots = if indexed { INDEXED_SLOTS } else { SEQ_SLOTS };
    slots
        .iter()
        .find(|(s, _)| *s == i)
        .map_or(Conv::NoOp, |(_, c)| *c)
}

/// `Dx8RendererFvfJumpIndex`.
fn jump_index(fvf: u32) -> usize {
    let bucket = (fvf >> 8) & 3;
    let high = if bucket == 2 { 0x30 } else { bucket << 4 };
    (high | (((fvf >> 6) & 3) << 2) | (((fvf >> 4) & 1) << 1) | ((fvf >> 2) & 1)) as usize
}

/// `Dx8RendererJumpTableIndex`: three or more coordinate sets without
/// `XYZRHW` go to slot 0x40.
fn table_index(dst_fvf: u32) -> usize {
    if ((dst_fvf & 0xF00) >> 8) < 3 || dst_fvf & 4 != 0 {
        jump_index(dst_fvf)
    } else {
        0x40
    }
}

/// `DispatchFamilyASeqHardcoded`.
fn dispatch_seq(src_fvf: u32, dst_fvf: u32) -> Conv {
    match (src_fvf, dst_fvf) {
        (0x112, 0x252) => SEQ_POS_NORM_TEX1_S2C,
        (0x112, 0x212) => SEQ_POS_NORM_TEX1_S28,
        (0x212, 0x252) => SEQ_POS_NORM_TEX2_S2C,
        _ => Conv::SeqGeneric,
    }
}

/// `DispatchFamilyBHardcoded`.
fn dispatch_indexed(src_fvf: u32, dst_fvf: u32) -> Conv {
    match (src_fvf, dst_fvf) {
        (0x12, 0x252) => IDX_POS_NORM_S2C,
        (0x12, 0x212) => IDX_POS_NORM_S28,
        (0x12, 0x112) => IDX_POS_NORM_S20,
        (0x112, 0x252) => IDX_POS_NORM_TEX1_S2C,
        (0x112, 0x212) => IDX_POS_NORM_TEX1_S28,
        (0x152, 0x252) => IDX_POS_NORM_DIFF_TEX1_S2C,
        (0x212, 0x252) => IDX_POS_NORM_TEX2_S2C,
        _ => Conv::IndexedGeneric,
    }
}

/// What `CopyVertexData` does for a source and destination format.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Plan {
    Convert(Conv),
    /// Selector 3: an interleaved source of the destination's format,
    /// copied whole.
    Interleaved,
}

/// The reference's selector and dispatch.
#[must_use]
pub fn plan(src: &Source<'_>, dst_fvf: u32) -> Plan {
    let selector = u32::from(src.fvf == dst_fvf) | (src.flags & 6);
    Plan::Convert(match selector {
        0 => dispatch_seq(src.fvf, dst_fvf),
        1 => table_slot(false, table_index(dst_fvf)),
        2 => Conv::SeqGeneric,
        3 => return Plan::Interleaved,
        4 => dispatch_indexed(src.fvf, dst_fvf),
        5 => table_slot(true, table_index(dst_fvf)),
        7 => Conv::IndexedGeneric,
        // 6: no case in the reference's switch.
        _ => Conv::NoOp,
    })
}

/// `Dx8RendererTexelByteSize`.
fn texel_size(code: u32) -> u32 {
    match code & 3 {
        0 => 8,
        1 => 12,
        2 => 16,
        _ => 4,
    }
}

fn copy(
    dst: &mut [u8],
    at: usize,
    src: &[u8],
    from: usize,
    len: usize,
) -> Result<(), CopyOutOfRange> {
    let s = src.get(from..from + len).ok_or(CopyOutOfRange)?;
    dst.get_mut(at..at + len)
        .ok_or(CopyOutOfRange)?
        .copy_from_slice(s);
    Ok(())
}

/// The element of a stream for vertex `v` (0-based from `start`): its
/// index for an indexed source, else its position.
fn element(
    stream: &Stream<'_>,
    indexed: bool,
    start: u32,
    v: u32,
) -> Result<usize, CopyOutOfRange> {
    if indexed {
        stream
            .index
            .get((start + v) as usize)
            .map(|&i| i as usize)
            .ok_or(CopyOutOfRange)
    } else {
        Ok((start + v) as usize)
    }
}

/// `CopyVertexData`: writes `count` vertices from vertex `start` of the
/// source into `dst`, a buffer of `dst_fvf` vertices addressed from
/// vertex 0 (the converters add `start` themselves).
pub fn copy_vertex_data(
    dst: &mut [u8],
    dst_fvf: u32,
    src: &Source<'_>,
    start: u32,
    count: u32,
) -> Result<(), CopyOutOfRange> {
    match plan(src, dst_fvf) {
        Plan::Interleaved => {
            let stride = super::fvf::Fvf(dst_fvf).stride() as usize;
            let from = start as usize * src.position.stride as usize;
            copy(
                dst,
                start as usize * stride,
                src.position.data,
                from,
                stride * count as usize,
            )
        }
        Plan::Convert(Conv::NoOp) => Ok(()),
        Plan::Convert(Conv::Special {
            indexed,
            stride,
            channels,
        }) => {
            for v in 0..count {
                let out = ((start + v) * stride) as usize;
                for &(channel, offset) in channels {
                    let stream = channel.stream(src);
                    let size = channel.size() as usize;
                    let e = element(stream, indexed, start, v)?;
                    copy(dst, out + offset as usize, stream.data, e * size, size)?;
                }
            }
            Ok(())
        }
        Plan::Convert(Conv::SeqGeneric) => generic(dst, dst_fvf, src, start, count, false),
        Plan::Convert(Conv::IndexedGeneric) => generic(dst, dst_fvf, src, start, count, true),
    }
}

/// `ConvSeqGeneric` and `ConvIndexedGeneric`: position always (12 bytes),
/// then normal, diffuse and the coordinate sets the destination has, each
/// copied when the source has it and skipped otherwise. Specular is not
/// copied, nor counted in the cursor, as in the reference.
fn generic(
    dst: &mut [u8],
    dst_fvf: u32,
    src: &Source<'_>,
    start: u32,
    count: u32,
    indexed: bool,
) -> Result<(), CopyOutOfRange> {
    let src_tex = ((src.fvf & 0xF00) >> 8).min(8);
    let dst_tex = ((dst_fvf & 0xF00) >> 8).min(8);
    let dst_stride = super::fvf::Fvf(dst_fvf).stride() as usize;
    let at = |s: &Stream<'_>, v: u32| -> Result<usize, CopyOutOfRange> {
        Ok(element(s, indexed, start, v)? * s.stride as usize)
    };
    for v in 0..count {
        let out = (start + v) as usize * dst_stride;
        copy(dst, out, src.position.data, at(&src.position, v)?, 12)?;
        let mut cursor = out + 12;
        if dst_fvf & 0x10 != 0 {
            if src.fvf & 0x10 != 0 {
                copy(dst, cursor, src.normal.data, at(&src.normal, v)?, 12)?;
            }
            cursor += 12;
        }
        if dst_fvf & 0x40 != 0 {
            if src.fvf & 0x40 != 0 {
                copy(dst, cursor, src.diffuse.data, at(&src.diffuse, v)?, 4)?;
            }
            cursor += 4;
        }
        for set in 0..dst_tex {
            if set < src_tex {
                // Sets past the descriptor's two read beyond it in the
                // C++ (divergence D208); here they are absent.
                if let Some(stream) = src.tex.get(set as usize) {
                    copy(
                        dst,
                        cursor,
                        stream.data,
                        at(stream, v)?,
                        stream.byte_size as usize,
                    )?;
                }
            }
            cursor += texel_size(dst_fvf >> (set * 2 + 16)) as usize;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bytes(n: usize, base: u8) -> Vec<u8> {
        (0..n).map(|i| base.wrapping_add(i as u8)).collect()
    }

    #[test]
    fn selects_converters_as_the_reference() {
        let s = |fvf, flags| Source {
            fvf,
            flags,
            ..Source::default()
        };
        assert_eq!(
            plan(&s(0x112, 0), 0x252),
            Plan::Convert(SEQ_POS_NORM_TEX1_S2C)
        );
        assert_eq!(plan(&s(0x152, 0), 0x252), Plan::Convert(Conv::SeqGeneric));
        assert_eq!(
            plan(&s(0x112, 0), 0x112),
            Plan::Convert(SEQ_POS_NORM_TEX1_S20)
        );
        assert_eq!(plan(&s(0x112, 2), 0x112), Plan::Interleaved);
        assert_eq!(
            plan(&s(0x112, 4), 0x112),
            Plan::Convert(IDX_POS_NORM_TEX1_S20)
        );
        assert_eq!(plan(&s(0x12, 4), 0x252), Plan::Convert(IDX_POS_NORM_S2C));
        assert_eq!(plan(&s(0x112, 6), 0x212), Plan::Convert(Conv::NoOp));
        assert_eq!(
            plan(&s(0x112, 6), 0x112),
            Plan::Convert(Conv::IndexedGeneric)
        );
        // XYZRHW destinations land on odd slots: no-ops.
        assert_eq!(plan(&s(0x144, 0), 0x144), Plan::Convert(Conv::NoOp));
        // Three coordinate sets go to slot 0x40.
        assert_eq!(plan(&s(0x312, 0), 0x312), Plan::Convert(Conv::SeqGeneric));
    }

    #[test]
    fn specialised_converters_use_fixed_strides_and_keep_gaps() {
        let pos = bytes(12 * 3, 0);
        let norm = bytes(12 * 3, 100);
        let tex = bytes(8 * 3, 200);
        // The descriptor's strides are ignored by the specialised path.
        let src = Source {
            fvf: 0x112,
            flags: 0,
            position: Stream {
                data: &pos,
                byte_size: 12,
                stride: 99,
                ..Stream::default()
            },
            normal: Stream {
                data: &norm,
                byte_size: 12,
                stride: 99,
                ..Stream::default()
            },
            tex: [
                Stream {
                    data: &tex,
                    byte_size: 8,
                    stride: 99,
                    ..Stream::default()
                },
                Stream::default(),
            ],
            ..Source::default()
        };
        let mut dst = vec![0xEE; 0x2C * 3];
        copy_vertex_data(&mut dst, 0x252, &src, 1, 2).unwrap();
        assert!(dst[..0x2C].iter().all(|&b| b == 0xEE));
        assert_eq!(&dst[0x2C..0x2C + 12], &pos[12..24]);
        assert_eq!(&dst[0x2C + 12..0x2C + 24], &norm[12..24]);
        // Bytes 0x18..0x1C (the diffuse slot) stay as they were.
        assert_eq!(&dst[0x2C + 0x18..0x2C + 0x1C], &[0xEE; 4]);
        assert_eq!(&dst[0x2C + 0x1C..0x2C + 0x24], &tex[8..16]);
    }

    #[test]
    fn the_generic_path_follows_the_descriptor() {
        let pos = bytes(16 * 2, 0); // stride 16
        let diff = bytes(4 * 2, 50);
        let idx = [1u32, 0];
        let src = Source {
            fvf: 0x42,
            flags: 4,
            position: Stream {
                data: &pos,
                byte_size: 12,
                stride: 16,
                index: &idx,
                count: 2,
            },
            diffuse: Stream {
                data: &diff,
                byte_size: 4,
                stride: 4,
                index: &idx,
                count: 2,
            },
            ..Source::default()
        };
        // Destination XYZ | NORMAL | DIFFUSE: the normal slot is skipped.
        let mut dst = vec![0; 28 * 2];
        copy_vertex_data(&mut dst, 0x52, &src, 0, 2).unwrap();
        assert_eq!(&dst[..12], &pos[16..28]);
        assert_eq!(&dst[12..24], &[0; 12]);
        assert_eq!(&dst[24..28], &diff[4..8]);
        assert_eq!(&dst[28..40], &pos[..12]);
        // Out of range is an error, not a read past the slice.
        let mut small = vec![0; 8];
        assert_eq!(
            copy_vertex_data(&mut small, 0x52, &src, 0, 1),
            Err(CopyOutOfRange)
        );
    }

    #[test]
    fn interleaved_sources_copy_whole_vertices() {
        let verts = bytes(16 * 4, 1);
        let src = Source {
            fvf: 0x42,
            flags: 2,
            position: Stream {
                data: &verts,
                byte_size: 16,
                stride: 16,
                ..Stream::default()
            },
            ..Source::default()
        };
        let mut dst = vec![0; 16 * 4];
        copy_vertex_data(&mut dst, 0x42, &src, 1, 2).unwrap();
        assert_eq!(&dst[16..48], &verts[16..48]);
        assert_eq!(&dst[..16], &[0; 16]);
    }

    /// The jump tables equal the reference's (`CppSource` present only).
    #[test]
    fn jump_tables_match_the_reference_source() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../CppSource/DX8RENDERER/DX8RENDERER.cpp");
        let Ok(text) = std::fs::read_to_string(path) else {
            return;
        };
        let name = |c: Conv, indexed: bool| -> String {
            match c {
                Conv::NoOp => "ConvNoOp".into(),
                Conv::SeqGeneric => "ConvSeqGeneric".into(),
                Conv::IndexedGeneric => "ConvIndexedGeneric".into(),
                Conv::Special {
                    stride, channels, ..
                } => {
                    let mut n = String::from(if indexed {
                        "ConvIndexedPosNorm"
                    } else {
                        "ConvSeqPosNorm"
                    });
                    if channels.iter().any(|(c, _)| *c == Diffuse) {
                        n.push_str("Diff");
                    }
                    if channels.iter().any(|(c, _)| *c == Tex1) {
                        n.push_str("Tex2");
                    } else if channels.iter().any(|(c, _)| *c == Tex0) {
                        n.push_str("Tex1");
                    }
                    format!("{n}_S{stride:X}")
                }
            }
        };
        for (table, indexed) in [
            ("kFamilyASeqJumpTable[0x41] = {", false),
            ("kFamilyBIndexedJumpTable[0x41] = {", true),
        ] {
            let from = text.find(table).expect("table") + table.len();
            let to = from + text[from..].find("};").expect("end");
            let entries: Vec<&str> = text[from..to]
                .split(',')
                .map(str::trim)
                .filter(|s| !s.is_empty())
                .collect();
            assert_eq!(entries.len(), 0x41);
            for (i, entry) in entries.iter().enumerate() {
                assert_eq!(*entry, name(table_slot(indexed, i), indexed), "slot {i}");
            }
        }
    }
}
