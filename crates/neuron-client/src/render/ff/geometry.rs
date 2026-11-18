//! Primitive types and counts, and the conversion of triangle fans, which
//! Direct3D 12 does not draw, into lists.

/// `D3DPRIMITIVETYPE`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Primitive {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
    TriangleFan,
}

impl Primitive {
    #[must_use]
    pub fn from_raw(raw: u32) -> Option<Primitive> {
        Some(match raw {
            1 => Primitive::PointList,
            2 => Primitive::LineList,
            3 => Primitive::LineStrip,
            4 => Primitive::TriangleList,
            5 => Primitive::TriangleStrip,
            6 => Primitive::TriangleFan,
            _ => return None,
        })
    }

    #[must_use]
    pub fn raw(self) -> u32 {
        match self {
            Primitive::PointList => 1,
            Primitive::LineList => 2,
            Primitive::LineStrip => 3,
            Primitive::TriangleList => 4,
            Primitive::TriangleStrip => 5,
            Primitive::TriangleFan => 6,
        }
    }
}

/// `FDirectX8::PrimCountFromVerts`: the primitives `count` vertices (or
/// indices) make. The divisions are signed and the subtractions wrap, so a
/// count too small for one primitive gives a result that is not above 0
/// as a signed number, which the callers test with `JLE`.
#[must_use]
pub fn prim_count_from_verts(primitive: u32, count: u32) -> u32 {
    let signed = count.cast_signed();
    match primitive {
        1 => count,
        2 => (signed / 2).cast_unsigned(),
        3 => count.wrapping_sub(1),
        4 => (signed / 3).cast_unsigned(),
        5 | 6 => count.wrapping_sub(2),
        _ => 0,
    }
}

/// Whether a draw of `count` primitives draws anything (`count` above 0
/// as a signed number).
#[must_use]
pub fn draws_anything(prim_count: u32) -> bool {
    prim_count.cast_signed() > 0
}

/// The list indices of a fan of `triangles` triangles over the vertex
/// indices `fan`: triangle `i` is `fan[0], fan[i + 1], fan[i + 2]`, the
/// order Direct3D draws a fan in, so the winding is kept.
#[must_use]
pub fn fan_to_list(fan: &[u16], triangles: usize) -> Vec<u16> {
    let mut out = Vec::with_capacity(triangles * 3);
    for i in 0..triangles.min(fan.len().saturating_sub(2)) {
        out.extend_from_slice(&[fan[0], fan[i + 1], fan[i + 2]]);
    }
    out
}

/// The list indices of a non-indexed fan of `triangles` triangles whose
/// first vertex is `base`.
#[must_use]
pub fn fan_to_list_sequential(base: u16, triangles: usize) -> Vec<u16> {
    let fan: Vec<u16> = (0..triangles + 2)
        .map(|i| base.wrapping_add(i as u16))
        .collect();
    fan_to_list(&fan, triangles)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counts_as_the_reference() {
        assert_eq!(prim_count_from_verts(1, 7), 7);
        assert_eq!(prim_count_from_verts(2, 7), 3);
        assert_eq!(prim_count_from_verts(3, 7), 6);
        assert_eq!(prim_count_from_verts(4, 7), 2);
        assert_eq!(prim_count_from_verts(5, 7), 5);
        assert_eq!(prim_count_from_verts(6, 7), 5);
        assert_eq!(prim_count_from_verts(0, 7), 0);
        assert_eq!(prim_count_from_verts(9, 7), 0);
        // Too few vertices: not above zero.
        assert!(!draws_anything(prim_count_from_verts(6, 1)));
        assert!(!draws_anything(prim_count_from_verts(3, 0)));
        // A negative count stays negative through the signed division.
        assert_eq!(
            prim_count_from_verts(4, u32::MAX - 5),
            (-2_i32).cast_unsigned()
        );
    }

    #[test]
    fn fans_become_lists_with_their_winding() {
        assert_eq!(
            fan_to_list(&[10, 11, 12, 13, 14], 3),
            [10, 11, 12, 10, 12, 13, 10, 13, 14]
        );
        assert_eq!(fan_to_list_sequential(4, 2), [4, 5, 6, 4, 6, 7]);
        assert!(fan_to_list(&[1, 2], 1).is_empty());
        assert_eq!(Primitive::from_raw(6), Some(Primitive::TriangleFan));
        assert_eq!(Primitive::from_raw(0), None);
    }
}
