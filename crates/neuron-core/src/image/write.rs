//! Image files for screenshots (record 0004, point 6 and owner answer 4):
//! BMP and PNG writers of 8-bit BGRA pixels, rows top to bottom without
//! padding, as a renderer reads its target back.
//!
//! The C++ saved through `D3DXSaveSurfaceToFileA`, whose exact headers this
//! does not copy: the BMP is a 24-bit `BI_RGB` bottom-up image with a
//! `BITMAPINFOHEADER`, the PNG an 8-bit RGB image (colour type 2) with one
//! zlib stream; alpha is dropped, as a front buffer has none.

use flate2::{Compress, Compression, FlushCompress, Status};

/// Why an image could not be written.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WriteError {
    /// The pixel buffer is not `width * height * 4` bytes.
    Size,
    /// Zero width or height, or a size the format cannot hold.
    Dimensions,
    /// The compressor failed.
    Compress,
}

impl std::fmt::Display for WriteError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::Size => "the pixels do not fill the image",
            Self::Dimensions => "the image size cannot be written",
            Self::Compress => "the PNG data could not be compressed",
        })
    }
}

impl std::error::Error for WriteError {}

fn check(width: u32, height: u32, bgra: &[u8]) -> Result<(), WriteError> {
    if width == 0 || height == 0 || width > 0x7fff_ffff / 4 || height > 0x7fff_ffff {
        return Err(WriteError::Dimensions);
    }
    if bgra.len() as u64 != u64::from(width) * u64::from(height) * 4 {
        return Err(WriteError::Size);
    }
    Ok(())
}

/// A 24-bit BMP of `width * height` BGRA pixels.
pub fn bmp(width: u32, height: u32, bgra: &[u8]) -> Result<Vec<u8>, WriteError> {
    check(width, height, bgra)?;
    let row = (width as usize * 3).next_multiple_of(4);
    let image = row * height as usize;
    let size = u32::try_from(54 + image).map_err(|_| WriteError::Dimensions)?;
    let mut out = Vec::with_capacity(size as usize);
    // BITMAPFILEHEADER.
    out.extend_from_slice(b"BM");
    out.extend_from_slice(&size.to_le_bytes());
    out.extend_from_slice(&[0; 4]);
    out.extend_from_slice(&54u32.to_le_bytes());
    // BITMAPINFOHEADER: positive height, rows bottom-up.
    out.extend_from_slice(&40u32.to_le_bytes());
    out.extend_from_slice(&(width as i32).to_le_bytes());
    out.extend_from_slice(&(height as i32).to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&24u16.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes()); // BI_RGB
    out.extend_from_slice(&(image as u32).to_le_bytes());
    out.extend_from_slice(&2835i32.to_le_bytes()); // 72 dpi
    out.extend_from_slice(&2835i32.to_le_bytes());
    out.extend_from_slice(&[0; 8]);
    for y in (0..height as usize).rev() {
        let start = out.len();
        for px in bgra[y * width as usize * 4..(y + 1) * width as usize * 4]
            .as_chunks::<4>()
            .0
        {
            out.extend_from_slice(&px[..3]);
        }
        out.resize(start + row, 0);
    }
    Ok(out)
}

/// The CRC-32 of PNG chunks (ISO 3309, reflected 0xEDB88320).
fn crc32(parts: &[&[u8]]) -> u32 {
    let mut crc = 0xffff_ffffu32;
    for part in parts {
        for &byte in *part {
            crc ^= u32::from(byte);
            for _ in 0..8 {
                crc = if crc & 1 != 0 {
                    (crc >> 1) ^ 0xedb8_8320
                } else {
                    crc >> 1
                };
            }
        }
    }
    !crc
}

fn chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(kind);
    out.extend_from_slice(data);
    out.extend_from_slice(&crc32(&[kind, data]).to_be_bytes());
}

/// An 8-bit RGB PNG of `width * height` BGRA pixels.
pub fn png(width: u32, height: u32, bgra: &[u8]) -> Result<Vec<u8>, WriteError> {
    check(width, height, bgra)?;
    // Each row: filter type 0, then RGB.
    let mut raw = Vec::with_capacity((width as usize * 3 + 1) * height as usize);
    for row in bgra.chunks_exact(width as usize * 4) {
        raw.push(0);
        for px in row.as_chunks::<4>().0 {
            raw.extend_from_slice(&[px[2], px[1], px[0]]);
        }
    }
    let mut compress = Compress::new(Compression::default(), true);
    let mut deflated = Vec::with_capacity(raw.len() / 2 + 64);
    loop {
        let before = compress.total_in() as usize;
        deflated.reserve(raw.len().max(1024));
        match compress.compress_vec(&raw[before..], &mut deflated, FlushCompress::Finish) {
            Ok(Status::StreamEnd) => break,
            Ok(_) => {}
            Err(_) => return Err(WriteError::Compress),
        }
    }
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut header = Vec::with_capacity(13);
    header.extend_from_slice(&width.to_be_bytes());
    header.extend_from_slice(&height.to_be_bytes());
    // Bit depth 8, colour type 2 (RGB), deflate, adaptive filtering, no
    // interlace.
    header.extend_from_slice(&[8, 2, 0, 0, 0]);
    chunk(&mut out, b"IHDR", &header);
    chunk(&mut out, b"IDAT", &deflated);
    chunk(&mut out, b"IEND", &[]);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::{Decompress, FlushDecompress};

    /// 3x2: red, green, blue / white, black, grey; BGRA bytes.
    const PIXELS: [u8; 24] = [
        0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, //
        255, 255, 255, 255, 0, 0, 0, 255, 128, 128, 128, 255,
    ];

    #[test]
    fn bmp_rows_are_bottom_up_and_padded() {
        let b = bmp(3, 2, &PIXELS).unwrap();
        assert_eq!(&b[..2], b"BM");
        // 3 * 3 = 9 bytes padded to 12 per row.
        assert_eq!(b.len(), 54 + 24);
        assert_eq!(u32::from_le_bytes(b[2..6].try_into().unwrap()), 78);
        assert_eq!(u16::from_le_bytes(b[28..30].try_into().unwrap()), 24);
        // The first stored row is the bottom one: white, black, grey.
        assert_eq!(&b[54..63], &[255, 255, 255, 0, 0, 0, 128, 128, 128]);
        assert_eq!(&b[63..66], &[0, 0, 0]);
        // Then the top row: red, green, blue as BGR.
        assert_eq!(&b[66..75], &[0, 0, 255, 0, 255, 0, 255, 0, 0]);
    }

    #[test]
    fn png_holds_the_rgb_rows() {
        let p = png(3, 2, &PIXELS).unwrap();
        assert_eq!(&p[..8], b"\x89PNG\r\n\x1a\n");
        assert_eq!(&p[12..16], b"IHDR");
        assert_eq!(&p[16..24], &[0, 0, 0, 3, 0, 0, 0, 2]);
        // The CRC of the empty IEND chunk is a known constant.
        assert_eq!(crc32(&[b"IEND", &[]]), 0xae42_6082);
        let idat_len = u32::from_be_bytes(p[33..37].try_into().unwrap()) as usize;
        assert_eq!(&p[37..41], b"IDAT");
        let mut d = Decompress::new(true);
        let mut raw = Vec::with_capacity(64);
        d.decompress_vec(&p[41..41 + idat_len], &mut raw, FlushDecompress::Finish)
            .unwrap();
        assert_eq!(
            raw,
            [
                0, 255, 0, 0, 0, 255, 0, 0, 0, 255, //
                0, 255, 255, 255, 0, 0, 0, 128, 128, 128
            ]
        );
        assert!(p.ends_with(&[0, 0, 0, 0, b'I', b'E', b'N', b'D', 0xae, 0x42, 0x60, 0x82]));
    }

    #[test]
    fn sizes_are_checked() {
        assert_eq!(bmp(0, 2, &[]), Err(WriteError::Dimensions));
        assert_eq!(png(2, 2, &[0; 15]), Err(WriteError::Size));
    }
}
