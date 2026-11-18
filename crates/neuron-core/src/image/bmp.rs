//! A reader of the uncompressed BMP files that frame images and expected
//! images are kept in (record 0004, points 11, 14 and 15): 24-bit and
//! 32-bit `BI_RGB`, and 32-bit `BI_BITFIELDS` with byte-aligned masks,
//! bottom-up or top-down, behind a `BITMAPINFOHEADER` or a later version
//! of it. Alpha is dropped: the comparison is on 8-bit RGB.

/// Why a BMP could not be read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BmpError {
    /// Not a BMP, or the headers are cut short.
    Header,
    /// A depth, compression or mask layout this reader does not take.
    Unsupported,
    /// The pixel rows run past the end of the file.
    Truncated,
}

impl std::fmt::Display for BmpError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::Header => "not a BMP file",
            Self::Unsupported => "a BMP layout this reader does not take",
            Self::Truncated => "the BMP pixel data is cut short",
        })
    }
}

impl std::error::Error for BmpError {}

/// An 8-bit RGB image, rows top to bottom without padding.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RgbImage {
    pub width: u32,
    pub height: u32,
    /// `width * height * 3` bytes: red, green, blue.
    pub rgb: Vec<u8>,
}

impl RgbImage {
    /// The image of `width * height` BGRA pixels, as a backend reads its
    /// target back; alpha dropped.
    #[must_use]
    pub fn from_bgra(width: u32, height: u32, bgra: &[u8]) -> RgbImage {
        let mut rgb = Vec::with_capacity(bgra.len() / 4 * 3);
        for p in bgra.as_chunks::<4>().0 {
            rgb.extend_from_slice(&[p[2], p[1], p[0]]);
        }
        RgbImage { width, height, rgb }
    }

    /// The pixel at (`x`, `y`).
    #[must_use]
    pub fn pixel(&self, x: u32, y: u32) -> [u8; 3] {
        let i = (y as usize * self.width as usize + x as usize) * 3;
        [self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]]
    }

    /// The pixels as BGRA with a full alpha, for the writers of
    /// [`super::write`].
    #[must_use]
    pub fn to_bgra(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.rgb.len() / 3 * 4);
        for p in self.rgb.as_chunks::<3>().0 {
            out.extend_from_slice(&[p[2], p[1], p[0], 0xff]);
        }
        out
    }
}

fn u16_at(b: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(at..at + 2)?.try_into().ok()?))
}

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

/// The byte index (0 to 3) a 32-bit mask selects, if it is one whole byte.
fn mask_byte(mask: u32) -> Option<usize> {
    (0..4).find(|&i| mask == 0xff << (i * 8))
}

/// Reads a BMP file.
pub fn decode(bytes: &[u8]) -> Result<RgbImage, BmpError> {
    if bytes.get(0..2) != Some(b"BM") {
        return Err(BmpError::Header);
    }
    let offset = u32_at(bytes, 10).ok_or(BmpError::Header)? as usize;
    let header = u32_at(bytes, 14).ok_or(BmpError::Header)? as usize;
    if header < 40 {
        return Err(BmpError::Unsupported);
    }
    let width = u32_at(bytes, 18).ok_or(BmpError::Header)? as i32;
    let height = u32_at(bytes, 22).ok_or(BmpError::Header)? as i32;
    let bits = u16_at(bytes, 28).ok_or(BmpError::Header)?;
    let compression = u32_at(bytes, 30).ok_or(BmpError::Header)?;
    if width <= 0 || height == 0 || height == i32::MIN {
        return Err(BmpError::Unsupported);
    }
    // Byte positions of red, green and blue within a pixel.
    let (bpp, order) = match (bits, compression) {
        (24, 0) => (3, [2, 1, 0]),
        (32, 0) => (4, [2, 1, 0]),
        (32, 3) => {
            // The masks follow a 40-byte header, or sit inside a later one.
            let at = 14 + 40;
            let m = |k: usize| u32_at(bytes, at + k * 4).ok_or(BmpError::Header);
            let pick = |mask: u32| mask_byte(mask).ok_or(BmpError::Unsupported);
            (4, [pick(m(0)?)?, pick(m(1)?)?, pick(m(2)?)?])
        }
        _ => return Err(BmpError::Unsupported),
    };
    let w = width as usize;
    let h = height.unsigned_abs() as usize;
    let row = (w * bpp).next_multiple_of(4);
    let end = offset
        .checked_add(row.checked_mul(h).ok_or(BmpError::Truncated)?)
        .ok_or(BmpError::Truncated)?;
    if bytes.len() < end {
        return Err(BmpError::Truncated);
    }
    let mut rgb = Vec::with_capacity(w * h * 3);
    for y in 0..h {
        // Positive height: bottom-up.
        let source = if height > 0 { h - 1 - y } else { y };
        let line = &bytes[offset + source * row..][..w * bpp];
        for p in line.chunks_exact(bpp) {
            rgb.extend_from_slice(&[p[order[0]], p[order[1]], p[order[2]]]);
        }
    }
    Ok(RgbImage {
        width: width as u32,
        height: h as u32,
        rgb,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::image::write;

    #[test]
    fn reads_back_what_the_writer_wrote() {
        // 3 by 2: odd width, so rows are padded.
        let bgra: Vec<u8> = (0..24u8).map(|i| i * 10).collect();
        let file = write::bmp(3, 2, &bgra).unwrap();
        let image = decode(&file).unwrap();
        assert_eq!((image.width, image.height), (3, 2));
        assert_eq!(image, RgbImage::from_bgra(3, 2, &bgra));
        assert_eq!(image.pixel(1, 0), [60, 50, 40]);
        assert_eq!(RgbImage::from_bgra(3, 2, &image.to_bgra()), image);
    }

    #[test]
    fn reads_top_down_32_bit_bitfields() {
        let mut f = Vec::new();
        f.extend_from_slice(b"BM");
        f.extend_from_slice(&(66u32 + 8).to_le_bytes());
        f.extend_from_slice(&[0; 4]);
        f.extend_from_slice(&66u32.to_le_bytes());
        f.extend_from_slice(&40u32.to_le_bytes());
        f.extend_from_slice(&1i32.to_le_bytes());
        f.extend_from_slice(&(-2i32).to_le_bytes());
        f.extend_from_slice(&1u16.to_le_bytes());
        f.extend_from_slice(&32u16.to_le_bytes());
        f.extend_from_slice(&3u32.to_le_bytes());
        f.extend_from_slice(&[0; 20]);
        // Masks: red in byte 0, green in byte 1, blue in byte 2.
        for m in [0xffu32, 0xff00, 0xff_0000] {
            f.extend_from_slice(&m.to_le_bytes());
        }
        f.extend_from_slice(&[1, 2, 3, 0, 4, 5, 6, 0]);
        let image = decode(&f).unwrap();
        assert_eq!(image.rgb, vec![1, 2, 3, 4, 5, 6]);
    }

    #[test]
    fn refuses_what_it_cannot_read() {
        assert_eq!(decode(b"PNG"), Err(BmpError::Header));
        let mut file = write::bmp(2, 2, &[0; 16]).unwrap();
        file.truncate(file.len() - 1);
        assert_eq!(decode(&file), Err(BmpError::Truncated));
        let mut file = write::bmp(2, 2, &[0; 16]).unwrap();
        file[28] = 8;
        assert_eq!(decode(&file), Err(BmpError::Unsupported));
    }
}
