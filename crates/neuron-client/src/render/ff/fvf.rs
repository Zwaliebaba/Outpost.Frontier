//! Flexible vertex formats (`D3DFVF_*`): the layout of a vertex and the
//! stride DX8RENDERER uses for it.
//!
//! DX8RENDERER does not ask Direct3D for the stride: `VertexStrideFromFvf`
//! (`CppSource/DX8RENDERER/DX8RENDERER.cpp`) computes it, and it differs
//! from Direct3D's rule for some formats: it counts no point size, gives
//! `XYZB2` no position, `XYZB3` the size of `XYZ` and `XYZB5` the size of
//! `XYZB4`. [`Fvf::stride`] is that function, which the renderer's buffers
//! and copies use; [`Fvf::layout`] places the elements as Direct3D 8 does.

/// A flexible vertex format code.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Fvf(pub u32);

/// `D3DFVF_POSITION_MASK` values.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Position {
    None,
    Xyz,
    /// Pre-transformed: screen x, y, z and reciprocal w.
    XyzRhw,
    /// Position and `n` blend weights (1 to 5).
    XyzBlend(u8),
}

/// One element of a vertex.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Element {
    Position,
    PositionRhw,
    BlendWeights(u8),
    Normal,
    PointSize,
    Diffuse,
    Specular,
    /// A texture coordinate set with its number of floats.
    TexCoord {
        set: u8,
        floats: u8,
    },
}

impl Element {
    /// The size in bytes Direct3D 8 gives the element.
    #[must_use]
    pub fn size(self) -> u32 {
        match self {
            Element::Position | Element::Normal => 12,
            Element::PositionRhw => 16,
            Element::BlendWeights(n) => 4 * u32::from(n),
            Element::PointSize | Element::Diffuse | Element::Specular => 4,
            Element::TexCoord { floats, .. } => 4 * u32::from(floats),
        }
    }
}

impl Fvf {
    pub const XYZ: u32 = 0x002;
    pub const XYZRHW: u32 = 0x004;
    pub const NORMAL: u32 = 0x010;
    pub const PSIZE: u32 = 0x020;
    pub const DIFFUSE: u32 = 0x040;
    pub const SPECULAR: u32 = 0x080;

    #[must_use]
    pub fn position(self) -> Position {
        match self.0 & 0xe {
            0x0 => Position::None,
            0x2 => Position::Xyz,
            0x4 => Position::XyzRhw,
            n => Position::XyzBlend(((n - 0x4) / 2) as u8),
        }
    }

    /// `D3DFVF_TEXCOUNT`: the number of coordinate sets (up to 8).
    #[must_use]
    pub fn tex_count(self) -> u32 {
        (self.0 & 0xf00) >> 8
    }

    /// The floats of coordinate set `set` (`D3DFVF_TEXCOORDSIZEn`).
    #[must_use]
    pub fn tex_floats(self, set: u32) -> u8 {
        match (self.0 >> (set * 2 + 16)) & 3 {
            0 => 2,
            1 => 3,
            2 => 4,
            _ => 1,
        }
    }

    /// `VertexStrideFromFvf`, with its differences from Direct3D's rule.
    #[must_use]
    pub fn stride(self) -> u32 {
        let f = self.0;
        let position = if f & 0xc == 0xc {
            0x1c
        } else if f & 4 != 0 {
            0x10
        } else if f & 2 != 0 {
            0xc
        } else {
            0
        };
        let normal = if f & Self::NORMAL != 0 { 12 } else { 0 };
        let colours = if f & Self::DIFFUSE != 0 { 4 } else { 0 }
            + if f & Self::SPECULAR != 0 { 4 } else { 0 };
        // The count field takes up to 15, though Direct3D allows 8; the
        // C++ loop reads past set 7 into bits 32 and up, which a 32-bit
        // shift of the C++ wraps (divergence D204: here a set past 7 adds
        // nothing).
        let tex: u32 = (0..self.tex_count().min(8))
            .map(|set| 4 * u32::from(self.tex_floats(set)))
            .sum();
        position + normal + colours + tex
    }

    /// The elements in Direct3D 8's order, with their offsets under
    /// Direct3D's sizes.
    #[must_use]
    pub fn layout(self) -> Vec<(Element, u32)> {
        let mut elements = Vec::new();
        match self.position() {
            Position::None => {}
            Position::Xyz => elements.push(Element::Position),
            Position::XyzRhw => elements.push(Element::PositionRhw),
            Position::XyzBlend(n) => {
                elements.push(Element::Position);
                elements.push(Element::BlendWeights(n));
            }
        }
        if self.0 & Self::NORMAL != 0 {
            elements.push(Element::Normal);
        }
        if self.0 & Self::PSIZE != 0 {
            elements.push(Element::PointSize);
        }
        if self.0 & Self::DIFFUSE != 0 {
            elements.push(Element::Diffuse);
        }
        if self.0 & Self::SPECULAR != 0 {
            elements.push(Element::Specular);
        }
        for set in 0..self.tex_count().min(8) {
            elements.push(Element::TexCoord {
                set: set as u8,
                floats: self.tex_floats(set),
            });
        }
        let mut offset = 0;
        elements
            .into_iter()
            .map(|e| {
                let at = offset;
                offset += e.size();
                (e, at)
            })
            .collect()
    }

    /// The stride under Direct3D's own rule (`D3DXGetFVFVertexSize`).
    #[must_use]
    pub fn direct3d_stride(self) -> u32 {
        self.layout().iter().map(|(e, _)| e.size()).sum()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strides_as_dx8renderer_computes_them() {
        // XYZ | NORMAL | TEX1: the common mesh format.
        assert_eq!(Fvf(0x112).stride(), 32);
        // XYZRHW | DIFFUSE | TEX1: the 2D layer.
        assert_eq!(Fvf(0x144).stride(), 28);
        // XYZ | DIFFUSE | SPECULAR | TEX2 with a 3D first set.
        assert_eq!(Fvf(0x0001_02c2).stride(), 12 + 8 + 8 + 12);
        // The renderer's own formats of VerifyState.
        assert_eq!(Fvf(0x152).stride(), 12 + 12 + 4 + 8);
        assert_eq!(Fvf(0x252).stride(), 12 + 12 + 4 + 16);
        // The quirks: PSIZE, XYZB2, XYZB3, XYZB5.
        assert_eq!(Fvf(0x022).stride(), 12);
        assert_eq!(Fvf(0x022).direct3d_stride(), 16);
        assert_eq!(Fvf(0x008).stride(), 0);
        assert_eq!(Fvf(0x008).direct3d_stride(), 20);
        assert_eq!(Fvf(0x00a).stride(), 12);
        assert_eq!(Fvf(0x00a).direct3d_stride(), 24);
        assert_eq!(Fvf(0x00c).stride(), 28);
        assert_eq!(Fvf(0x00e).stride(), 28);
        assert_eq!(Fvf(0x00e).direct3d_stride(), 32);
        // A one-float set (size code 3).
        assert_eq!(Fvf(0x0003_0102).stride(), 16);
    }

    #[test]
    fn layouts_follow_direct3d_order() {
        assert_eq!(
            Fvf(0x1d2).layout(),
            vec![
                (Element::Position, 0),
                (Element::Normal, 12),
                (Element::Diffuse, 24),
                (Element::Specular, 28),
                (Element::TexCoord { set: 0, floats: 2 }, 32),
            ]
        );
        assert_eq!(Fvf(0x006).position(), Position::XyzBlend(1));
        assert_eq!(
            Fvf(0x006).layout(),
            vec![(Element::Position, 0), (Element::BlendWeights(1), 12)]
        );
    }
}
