//! The IVP compact surface: the blob of a `.sur` file's `surf` chunk
//! (`CppSource/IVP/ivp_collision/ivp_compact_surface.h`,
//! `CppSource/IVP/ivp_compact_builder/ivp_compact_ledge.h`).
//!
//! The blob holds no pointers. Every link is a byte offset relative to the
//! record that carries it, and the reference follows them unchecked. The
//! port resolves each one against the blob and returns a [`CompactError`]
//! where the reference would read outside it or loop.
//!
//! Layout, little-endian, offsets from the start of the blob (which the
//! reference allocates 16-byte aligned):
//!
//! * Surface header, 0x24 bytes: `mass_center[3]` (+0x00),
//!   `rotation_inertia[3]` (+0x0C), `upper_limit_radius` (+0x18), a dword
//!   whose low byte is the maximum surface deviation and whose upper 24 bits
//!   (arithmetic shift) are the blob size (+0x1C), and the self-relative
//!   offset of the ledge-tree root (+0x20). The builder writes the header as
//!   0x30 bytes, the last 12 zero; nothing reads them.
//! * Ledge-tree node, 0x1C bytes: offset of the right son (+0x00, 0 for a
//!   terminal node; the left son is the next node, +0x1C), offset of the
//!   node's compact ledge (+0x04, always set on a terminal node, optional on
//!   an inner node, where it is the hull of its subtree), `center[3]`
//!   (+0x08), `radius` (+0x14), three box-size bytes (+0x18) and one unused
//!   byte (+0x1B).
//! * Compact ledge, a 16-byte header: offset of its point array (+0x00),
//!   client data (+0x04: the back link to its node on an inner node's hull
//!   ledge, otherwise data of the builder's client), a packed dword (+0x08:
//!   bits 0..1 the has-children flag, bits 8..31 the ledge's byte size / 16),
//!   the triangle count (`i16`, +0x0C) and an `i16` no traced code reads
//!   (+0x0E). The triangles follow the header.
//! * Compact triangle, 16 bytes: a packed dword (bits 0..11 the triangle's
//!   index in its ledge, bits 12..23 the pierce index) and three edges.
//! * Compact edge, 4 bytes: bits 0..15 the start point's index, bits 16..30
//!   the signed offset of the opposite edge counted in edges, bit 31 the
//!   piercing flag. An edge runs from its start point to the start point of
//!   the next edge of its triangle; the reference finds the next and
//!   previous edges from the address bits, which needs every ledge 16-byte
//!   aligned in the blob.
//! * Poly point, 16 bytes: `x`, `y`, `z` and a fourth float (the Hesse value
//!   slot, kept as read). Points may be shared between the ledges of a
//!   surface; the ledge does not store their count, so the port reads the
//!   points from index 0 up to the highest index an edge of the ledge names.

use std::collections::HashMap;

use crate::math::Vector;

/// Bytes of the surface header the reference reads.
pub const SURFACE_HEADER_SIZE: usize = 0x24;
/// Bytes of a ledge-tree node.
pub const NODE_SIZE: usize = 0x1C;
/// Bytes of a compact ledge header.
pub const LEDGE_HEADER_SIZE: usize = 0x10;
/// Bytes of a compact triangle.
pub const TRIANGLE_SIZE: usize = 0x10;
/// Bytes of a poly point.
pub const POINT_SIZE: usize = 0x10;

/// Why a compact surface cannot be decoded. Offsets are from the start of
/// the blob. In every case the reference would read outside the blob, read
/// garbage or never return.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompactError {
    /// The blob is shorter than the surface header.
    TooShort,
    /// A ledge-tree node does not lie inside the blob.
    NodeOutOfRange {
        /// The node's offset (may be negative).
        offset: i64,
    },
    /// The tree reaches a node twice, so it is not a tree. The reference
    /// walks it again, endlessly when it loops.
    NodeRevisited {
        /// The node's offset.
        offset: u32,
    },
    /// A terminal node has no ledge. The reference reads the node itself
    /// as its ledge.
    TerminalWithoutLedge {
        /// The node's offset.
        node: u32,
    },
    /// A ledge header does not lie inside the blob.
    LedgeOutOfRange {
        /// The ledge's offset (may be negative).
        offset: i64,
    },
    /// A ledge is not 16-byte aligned, so the reference's address-bit edge
    /// arithmetic would leave its triangles.
    MisalignedLedge {
        /// The ledge's offset.
        offset: u32,
    },
    /// A ledge's triangle count is negative.
    NegativeTriangleCount {
        /// The ledge's offset.
        ledge: u32,
    },
    /// A ledge's triangles run past the end of the blob.
    TrianglesOutOfRange {
        /// The ledge's offset.
        ledge: u32,
    },
    /// The points a ledge's edges name do not lie inside the blob.
    PointsOutOfRange {
        /// The ledge's offset.
        ledge: u32,
    },
    /// An edge's opposite edge is not an edge of the same ledge.
    OppositeOutOfRange {
        /// The ledge's offset.
        ledge: u32,
        /// The triangle's index in the ledge.
        triangle: u32,
        /// The edge's index in the triangle.
        edge: u8,
    },
}

impl core::fmt::Display for CompactError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::TooShort => f.write_str("compact surface shorter than its header"),
            Self::NodeOutOfRange { offset } => {
                write!(f, "ledge-tree node {offset:#x} outside the surface")
            }
            Self::NodeRevisited { offset } => {
                write!(f, "ledge-tree node {offset:#x} reached twice")
            }
            Self::TerminalWithoutLedge { node } => {
                write!(f, "terminal ledge-tree node {node:#x} has no ledge")
            }
            Self::LedgeOutOfRange { offset } => {
                write!(f, "compact ledge {offset:#x} outside the surface")
            }
            Self::MisalignedLedge { offset } => {
                write!(f, "compact ledge {offset:#x} is not 16-byte aligned")
            }
            Self::NegativeTriangleCount { ledge } => {
                write!(f, "compact ledge {ledge:#x} has a negative triangle count")
            }
            Self::TrianglesOutOfRange { ledge } => {
                write!(
                    f,
                    "triangles of compact ledge {ledge:#x} outside the surface"
                )
            }
            Self::PointsOutOfRange { ledge } => {
                write!(f, "points of compact ledge {ledge:#x} outside the surface")
            }
            Self::OppositeOutOfRange {
                ledge,
                triangle,
                edge,
            } => write!(
                f,
                "opposite of edge {edge} of triangle {triangle} of compact ledge {ledge:#x} \
                 is not an edge of the ledge"
            ),
        }
    }
}

impl std::error::Error for CompactError {}

/// A decoded compact surface.
#[derive(Clone, Debug, PartialEq)]
pub struct CompactSurface {
    /// +0x00.
    pub mass_center: Vector,
    /// +0x0C.
    pub rotation_inertia: Vector,
    /// +0x18.
    pub upper_limit_radius: f32,
    /// +0x1C: maximum deviation (low byte) and byte size (upper 24 bits).
    pub max_deviation_and_size: u32,
    /// +0x20: the root node's offset from the start of the blob.
    pub offset_ledgetree_root: i32,
    /// The ledge-tree nodes in pre-order (a node, its left subtree, its
    /// right subtree); `nodes[0]` is the root.
    pub nodes: Vec<LedgeTreeNode>,
    /// Every ledge a node names, in the order of [`CompactSurface::nodes`].
    /// A ledge that several nodes name appears once.
    pub ledges: Vec<CompactLedge>,
}

impl CompactSurface {
    /// The low byte of +0x1C (`max_factor_surface_deviation`).
    #[must_use]
    pub fn max_deviation(&self) -> u8 {
        self.max_deviation_and_size.to_le_bytes()[0]
    }

    /// The blob size the header records: +0x1C shifted right by 8 as a
    /// signed dword (`PhyArch_SurfaceByteSize`).
    #[must_use]
    pub fn byte_size(&self) -> i32 {
        i32::from_le_bytes(self.max_deviation_and_size.to_le_bytes()) >> 8
    }

    /// Indices into [`CompactSurface::ledges`] of the terminal nodes' ledges,
    /// left subtree first (`IVP_Compact_Ledge_Solver::get_all_ledges`).
    #[must_use]
    pub fn terminal_ledges(&self) -> Vec<usize> {
        let mut out = Vec::new();
        let mut stack = vec![0usize];
        while let Some(index) = stack.pop() {
            let node = &self.nodes[index];
            match (node.left, node.right) {
                (Some(left), Some(right)) => {
                    stack.push(right);
                    stack.push(left);
                }
                _ => out.extend(node.ledge),
            }
        }
        out
    }

    /// The surface's single convex ledge: the root's ledge when the root is
    /// terminal, else the root's hull ledge if it has one
    /// (`IVP_SurfaceManager_Polygon::get_single_convex`).
    #[must_use]
    pub fn single_convex(&self) -> Option<usize> {
        self.nodes[0].ledge
    }

    /// Decodes a compact surface blob.
    ///
    /// # Errors
    ///
    /// A [`CompactError`] where a link leaves the blob, the tree is not a
    /// tree, or a ledge breaks the layout the reference relies on.
    pub fn parse(blob: &[u8]) -> Result<Self, CompactError> {
        if blob.len() < SURFACE_HEADER_SIZE {
            return Err(CompactError::TooShort);
        }
        let mut decoder = Decoder {
            blob,
            nodes: Vec::new(),
            node_index: HashMap::new(),
            ledges: Vec::new(),
            ledge_index: HashMap::new(),
        };
        let offset_ledgetree_root = read_i32(blob, 0x20);
        decoder.walk(i64::from(offset_ledgetree_root))?;
        Ok(Self {
            mass_center: read_vector(blob, 0x00),
            rotation_inertia: read_vector(blob, 0x0C),
            upper_limit_radius: read_f32(blob, 0x18),
            max_deviation_and_size: read_u32(blob, 0x1C),
            offset_ledgetree_root,
            nodes: decoder.nodes,
            ledges: decoder.ledges,
        })
    }
}

/// A ledge-tree node (`IVP_Compact_Ledgetree_Node`).
#[derive(Clone, Debug, PartialEq)]
pub struct LedgeTreeNode {
    /// The node's offset from the start of the blob.
    pub offset: u32,
    /// +0x00: offset of the right son from this node, 0 for a terminal node.
    pub offset_right_node: i32,
    /// +0x04: offset of the node's ledge from this node, 0 for none.
    pub offset_compact_ledge: i32,
    /// +0x08.
    pub center: Vector,
    /// +0x14.
    pub radius: f32,
    /// +0x18: per-axis half extent in units of `radius * 0.004`.
    pub box_sizes: [u8; 3],
    /// +0x1B: unused.
    pub free_0: u8,
    /// Index of the left son (the next node) in [`CompactSurface::nodes`];
    /// `None` on a terminal node.
    pub left: Option<usize>,
    /// Index of the right son in [`CompactSurface::nodes`]; `None` on a
    /// terminal node.
    pub right: Option<usize>,
    /// Index of the node's ledge in [`CompactSurface::ledges`]; always set
    /// on a terminal node.
    pub ledge: Option<usize>,
}

impl LedgeTreeNode {
    /// Whether the node has no sons.
    #[must_use]
    pub fn is_terminal(&self) -> bool {
        self.offset_right_node == 0
    }
}

/// A compact ledge: one convex piece (`IVP_Compact_Ledge`).
#[derive(Clone, Debug, PartialEq)]
pub struct CompactLedge {
    /// The ledge's offset from the start of the blob.
    pub offset: u32,
    /// +0x00: offset of the point array from this ledge.
    pub point_offset: i32,
    /// +0x04: client data, or on a hull ledge the offset of its node.
    pub client_data: i32,
    /// +0x08: has-children flag (bits 0..1) and byte size / 16 (bits 8..31).
    pub size_and_flags: u32,
    /// +0x0E: not read by any traced code.
    pub unknown_0x0e: i16,
    /// The triangles, `n_triangles` (+0x0C) of them.
    pub triangles: Vec<CompactTriangle>,
    /// The points from index 0 to the highest index an edge names.
    pub points: Vec<PolyPoint>,
}

impl CompactLedge {
    /// Bits 0..1 of +0x08: nonzero on an inner node's hull ledge.
    #[must_use]
    pub fn has_children_flag(&self) -> u8 {
        (self.size_and_flags & 0x3).to_le_bytes()[0]
    }

    /// Bits 2..7 of +0x08, which no traced code interprets.
    #[must_use]
    pub fn unknown_flag_bits(&self) -> u8 {
        ((self.size_and_flags >> 2) & 0x3F).to_le_bytes()[0]
    }

    /// The ledge's byte size, `(+0x08 >> 8) << 4`.
    #[must_use]
    pub fn byte_size(&self) -> u32 {
        (self.size_and_flags >> 8) << 4
    }

    /// The edge `edge` of triangle `triangle`'s successor in its triangle:
    /// the edge that starts where this one ends.
    #[must_use]
    pub fn next_edge(edge: EdgeRef) -> EdgeRef {
        EdgeRef {
            triangle: edge.triangle,
            edge: (edge.edge + 1) % 3,
        }
    }

    /// The edge before `edge` in its triangle.
    #[must_use]
    pub fn prev_edge(edge: EdgeRef) -> EdgeRef {
        EdgeRef {
            triangle: edge.triangle,
            edge: (edge.edge + 2) % 3,
        }
    }

    /// The edge `edge` names.
    #[must_use]
    pub fn edge(&self, edge: EdgeRef) -> &CompactEdge {
        &self.triangles[edge.triangle].edges[usize::from(edge.edge)]
    }

    /// The start point of `edge`.
    #[must_use]
    pub fn start_point(&self, edge: EdgeRef) -> &PolyPoint {
        &self.points[usize::from(self.edge(edge).start_point_index())]
    }
}

/// A compact triangle (`IVP_Compact_Triangle`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CompactTriangle {
    /// +0x00: packed triangle index, pierce index and higher bits.
    pub tri_info: u32,
    /// +0x04, +0x08, +0x0C.
    pub edges: [CompactEdge; 3],
}

impl CompactTriangle {
    /// Bits 0..11 of the info dword: the triangle's index in its ledge.
    #[must_use]
    pub fn tri_index(&self) -> u32 {
        self.tri_info & 0x0FFF
    }

    /// Bits 12..23 of the info dword: the index of the triangle this one is
    /// pierce-linked to.
    #[must_use]
    pub fn pierce_index(&self) -> u32 {
        (self.tri_info >> 12) & 0x0FFF
    }

    /// Bits 24..31 of the info dword, which no traced code interprets.
    #[must_use]
    pub fn unknown_bits_24_31(&self) -> u8 {
        self.tri_info.to_le_bytes()[3]
    }
}

/// A compact edge (`IVP_Compact_Edge`) with its opposite edge resolved.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CompactEdge {
    /// The dword as stored.
    pub data: u32,
    /// The opposite edge, which the stored offset names.
    pub opposite: EdgeRef,
}

impl CompactEdge {
    /// Bits 0..15: the start point's index in the ledge's point array.
    #[must_use]
    pub fn start_point_index(&self) -> u16 {
        let [low, high, _, _] = self.data.to_le_bytes();
        u16::from_le_bytes([low, high])
    }

    /// Bits 16..30 as a signed 15-bit value: the opposite edge's distance
    /// from this one, counted in edges (`GetOppositeIndex`).
    #[must_use]
    pub fn opposite_index(&self) -> i32 {
        i32::from_le_bytes((self.data << 1).to_le_bytes()) >> 17
    }

    /// Bit 31.
    #[must_use]
    pub fn is_piercing(&self) -> bool {
        self.data & 0x8000_0000 != 0
    }
}

/// An edge of a ledge: a triangle index and the edge's index (0..3) in it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct EdgeRef {
    /// Index into [`CompactLedge::triangles`].
    pub triangle: usize,
    /// 0, 1 or 2.
    pub edge: u8,
}

/// A poly point (`IVP_U_Float_Point`, 16 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PolyPoint {
    /// +0x00.
    pub position: Vector,
    /// +0x0C: the Hesse-value slot, kept as read.
    pub hesse_val: f32,
}

struct Decoder<'a> {
    blob: &'a [u8],
    nodes: Vec<LedgeTreeNode>,
    node_index: HashMap<u32, usize>,
    ledges: Vec<CompactLedge>,
    ledge_index: HashMap<u32, usize>,
}

impl Decoder<'_> {
    /// `offset` as a record start inside the blob with `size` bytes.
    fn place(&self, offset: i64, size: usize) -> Option<u32> {
        let start = usize::try_from(offset).ok()?;
        let end = start.checked_add(size)?;
        if end > self.blob.len() {
            return None;
        }
        u32::try_from(start).ok()
    }

    /// Walks the tree from the root at `root` in pre-order, without
    /// recursion.
    fn walk(&mut self, root: i64) -> Result<(), CompactError> {
        // (offset of the node, index of the parent and whether this is its
        // right son).
        let mut stack: Vec<(i64, Option<(usize, bool)>)> = vec![(root, None)];
        while let Some((offset, parent)) = stack.pop() {
            let Some(at) = self.place(offset, NODE_SIZE) else {
                return Err(CompactError::NodeOutOfRange { offset });
            };
            if self.node_index.contains_key(&at) {
                return Err(CompactError::NodeRevisited { offset: at });
            }
            let index = self.nodes.len();
            self.node_index.insert(at, index);
            let base = at as usize;
            let blob = self.blob;
            let offset_right_node = read_i32(blob, base);
            let offset_compact_ledge = read_i32(blob, base + 4);
            let ledge = if offset_compact_ledge != 0 {
                Some(self.ledge(offset + i64::from(offset_compact_ledge))?)
            } else if offset_right_node == 0 {
                return Err(CompactError::TerminalWithoutLedge { node: at });
            } else {
                None
            };
            self.nodes.push(LedgeTreeNode {
                offset: at,
                offset_right_node,
                offset_compact_ledge,
                center: read_vector(blob, base + 0x08),
                radius: read_f32(blob, base + 0x14),
                box_sizes: [blob[base + 0x18], blob[base + 0x19], blob[base + 0x1A]],
                free_0: blob[base + 0x1B],
                left: None,
                right: None,
                ledge,
            });
            if let Some((parent, is_right)) = parent {
                if is_right {
                    self.nodes[parent].right = Some(index);
                } else {
                    self.nodes[parent].left = Some(index);
                }
            }
            if offset_right_node != 0 {
                stack.push((offset + i64::from(offset_right_node), Some((index, true))));
                stack.push((offset + NODE_SIZE as i64, Some((index, false))));
            }
        }
        Ok(())
    }

    /// The index of the ledge at `offset`, decoding it on first sight.
    fn ledge(&mut self, offset: i64) -> Result<usize, CompactError> {
        let Some(at) = self.place(offset, LEDGE_HEADER_SIZE) else {
            return Err(CompactError::LedgeOutOfRange { offset });
        };
        if let Some(&index) = self.ledge_index.get(&at) {
            return Ok(index);
        }
        if at % 16 != 0 {
            return Err(CompactError::MisalignedLedge { offset: at });
        }
        let ledge = decode_ledge(self.blob, at)?;
        let index = self.ledges.len();
        self.ledges.push(ledge);
        self.ledge_index.insert(at, index);
        Ok(index)
    }
}

fn decode_ledge(blob: &[u8], at: u32) -> Result<CompactLedge, CompactError> {
    let base = at as usize;
    let point_offset = read_i32(blob, base);
    let client_data = read_i32(blob, base + 4);
    let size_and_flags = read_u32(blob, base + 8);
    let n_triangles = read_i16(blob, base + 0x0C);
    let unknown_0x0e = read_i16(blob, base + 0x0E);
    let count = usize::try_from(n_triangles)
        .map_err(|_| CompactError::NegativeTriangleCount { ledge: at })?;
    let first = base + LEDGE_HEADER_SIZE;
    let end = first + count * TRIANGLE_SIZE;
    if end > blob.len() {
        return Err(CompactError::TrianglesOutOfRange { ledge: at });
    }
    let span = i64::try_from(count * TRIANGLE_SIZE).unwrap_or(i64::MAX);
    let mut triangles = Vec::with_capacity(count);
    let mut point_count = 0usize;
    for triangle in 0..count {
        let t = first + triangle * TRIANGLE_SIZE;
        let tri_info = read_u32(blob, t);
        let mut edges = [CompactEdge {
            data: 0,
            opposite: EdgeRef {
                triangle: 0,
                edge: 0,
            },
        }; 3];
        for (edge, slot) in (0u8..).zip(edges.iter_mut()) {
            let data = read_u32(blob, t + 4 + 4 * usize::from(edge));
            let mut resolved = CompactEdge {
                data,
                opposite: EdgeRef {
                    triangle: 0,
                    edge: 0,
                },
            };
            // Byte position of this edge and of its opposite in the
            // triangle array; the opposite must be an edge slot, not a
            // triangle's info dword.
            let here = (triangle * TRIANGLE_SIZE) as i64 + 4 * i64::from(edge + 1);
            let there = here + 4 * i64::from(resolved.opposite_index());
            if there < 0 || there >= span || there % 16 == 0 {
                return Err(CompactError::OppositeOutOfRange {
                    ledge: at,
                    triangle: u32::try_from(triangle).unwrap_or(u32::MAX),
                    edge,
                });
            }
            let there = there as usize;
            resolved.opposite = EdgeRef {
                triangle: there / TRIANGLE_SIZE,
                edge: u8::try_from((there % TRIANGLE_SIZE) / 4 - 1).unwrap_or(0),
            };
            point_count = point_count.max(usize::from(resolved.start_point_index()) + 1);
            *slot = resolved;
        }
        triangles.push(CompactTriangle { tri_info, edges });
    }
    // A ledge without triangles names no point, so its point offset is not
    // followed.
    let points_at = i64::from(at) + i64::from(point_offset);
    let points_ok = if point_count == 0 {
        Some(0)
    } else {
        usize::try_from(points_at).ok().and_then(|start| {
            let end = start.checked_add(point_count * POINT_SIZE)?;
            (end <= blob.len()).then_some(start)
        })
    };
    let Some(points_start) = points_ok else {
        return Err(CompactError::PointsOutOfRange { ledge: at });
    };
    let points = (0..point_count)
        .map(|index| {
            let p = points_start + index * POINT_SIZE;
            PolyPoint {
                position: read_vector(blob, p),
                hesse_val: read_f32(blob, p + 12),
            }
        })
        .collect();
    Ok(CompactLedge {
        offset: at,
        point_offset,
        client_data,
        size_and_flags,
        unknown_0x0e,
        triangles,
        points,
    })
}

// The readers below are called only on ranges already checked against the
// blob.

fn read_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

fn read_i32(bytes: &[u8], at: usize) -> i32 {
    i32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

fn read_i16(bytes: &[u8], at: usize) -> i16 {
    i16::from_le_bytes([bytes[at], bytes[at + 1]])
}

fn read_f32(bytes: &[u8], at: usize) -> f32 {
    f32::from_bits(read_u32(bytes, at))
}

fn read_vector(bytes: &[u8], at: usize) -> Vector {
    Vector::new(
        read_f32(bytes, at),
        read_f32(bytes, at + 4),
        read_f32(bytes, at + 8),
    )
}
