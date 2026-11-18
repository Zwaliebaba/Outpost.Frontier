//! Format classes: what format a texture asked for in a class (`DAOP`,
//! `DAOT`, `DAA1`, `DAA4`, `DAA8`, or a DXT format the device cannot make)
//! is created in. The texture library asks for classes (its format maps
//! end in them); DX8RENDERER's `CreateTexture` tries the format asked for,
//! then each candidate of its class in order, and takes the first the
//! device supports (`PopulateFormatCatalog`, `ResolvePixelFormatByComponents`,
//! `FormatClassKeyFromPfId`, `CheckTextureFormat` and `CreateTexture` in
//! `CppSource/DX8RENDERER/DX8RENDERER.cpp`).
//!
//! The candidates come from the built-in catalog, which
//! `[TextureFormatClasses]` in the engine INI can replace class by class.
//! The device support the C++ asks the Direct3D 8 driver for is fixed
//! here: every format [`crate::render::ff::textures`] can hold and fill
//! (divergence D260), after the gates `TEXTURE_ALLOW_8BIT`,
//! `TEXTURE_ALLOW_32BIT` and `TEXTURE_ALLOW_DXT`.

use neuron_core::image::PixelFormat;
use neuron_core::image::format::{D3D_FORMATS, fourcc};
use neuron_core::ini::EngineSection;

/// `ReadLine` into `buffer` (`GetLine`): a `key = value` line is written
/// with its terminator and leaves the bytes after it, a bare key fills the
/// rest of the buffer with zeros (`strncpy`). False when there is no line
/// `index` or it is empty, where the reference's loops stop.
pub fn read_line_into(section: &EngineSection, index: usize, buffer: &mut [u8]) -> bool {
    let Some(entry) = section.entries.get(index) else {
        return false;
    };
    let Some(line) = section.line(index) else {
        return false;
    };
    if buffer.is_empty() {
        return false;
    }
    let keep = line.len().min(buffer.len() - 1);
    buffer[..keep].copy_from_slice(&line[..keep]);
    if entry.value.is_some() {
        buffer[keep] = 0;
    } else {
        buffer[keep..].fill(0);
    }
    keep != 0
}

/// `kPfDescriptors`: format, bits per pixel, red bits (also compared with
/// the blue bits), green bits, alpha bits.
const DESCRIPTORS: [(u32, u32, u32, u32, u32); 8] = [
    (1, 8, 0, 0, 0),
    (2, 24, 8, 8, 0),
    (3, 16, 5, 6, 0),
    (4, 16, 5, 5, 0),
    (5, 16, 4, 4, 4),
    (6, 16, 5, 5, 1),
    (7, 32, 8, 8, 8),
    (8, 32, 8, 8, 0),
];

/// `ResolvePixelFormatByComponents`: the first descriptor whose red bits
/// equal both `red` and `blue`; 0 on a miss.
#[must_use]
pub fn by_components(bits: u32, red: u32, green: u32, blue: u32, alpha: u32) -> PixelFormat {
    DESCRIPTORS
        .iter()
        .find(|d| d.1 == bits && d.2 == red && d.2 == blue && d.3 == green && d.4 == alpha)
        .map_or(PixelFormat::UNKNOWN, |d| PixelFormat(d.0))
}

/// `FormatClassKeyFromPfId`: 0 up to 9, the table's Direct3D format or
/// FourCC up to 21, else the number itself.
#[must_use]
pub fn class_key(format: PixelFormat) -> u32 {
    let signed = format.0.cast_signed();
    if signed <= 9 {
        0
    } else if signed < 0x16 {
        D3D_FORMATS[format.0 as usize]
    } else {
        format.0
    }
}

/// The catalog: classes by key, in the order they were added.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FormatClasses {
    classes: Vec<(u32, Vec<PixelFormat>)>,
}

impl Default for FormatClasses {
    fn default() -> Self {
        Self::builtin()
    }
}

impl FormatClasses {
    fn find_or_insert(&mut self, key: u32) -> &mut Vec<PixelFormat> {
        let at = match self.classes.iter().position(|c| c.0 == key) {
            Some(at) => at,
            None => {
                self.classes.push((key, Vec::new()));
                self.classes.len() - 1
            }
        };
        &mut self.classes[at].1
    }

    fn append(&mut self, key: u32, list: &[(u32, u32, u32, u32, u32)]) {
        let node = self.find_or_insert(key);
        for &(bits, r, g, b, a) in list {
            let format = by_components(bits, r, g, b, a);
            if format != PixelFormat::UNKNOWN {
                node.push(format);
            }
        }
    }

    /// The built-in catalog of `PopulateFormatCatalog`.
    #[must_use]
    pub fn builtin() -> FormatClasses {
        let mut c = FormatClasses {
            classes: Vec::new(),
        };
        let a1 = (16, 5, 5, 5, 1);
        let a4 = (16, 4, 4, 4, 4);
        let a8 = (32, 8, 8, 8, 8);
        let opaque = [(16, 5, 6, 5, 0), (16, 5, 5, 5, 0), a1, (24, 8, 8, 8, 0), a8];
        c.append(fourcc(*b"DAOP"), &[(8, 0, 0, 0, 0)]);
        c.append(fourcc(*b"DAOP"), &opaque);
        c.append(fourcc(*b"DAOT"), &opaque);
        c.append(fourcc(*b"DAA1"), &[a1, a4, a8]);
        c.append(fourcc(*b"DAA4"), &[a4, a8, a1]);
        c.append(fourcc(*b"DAA8"), &[a8, a4, a1]);
        c.append(fourcc(*b"DXT1"), &[a1, a4]);
        for code in [*b"DXT2", *b"DXT3", *b"DXT4", *b"DXT5"] {
            c.append(fourcc(code), &[a8, a4]);
        }
        c
    }

    /// The built-in catalog, then `[TextureFormatClasses]`: a line starting
    /// with `clear` (three characters, any case) empties the catalog;
    /// otherwise the four bytes at the line's first word are a key whose
    /// candidates are replaced by the comma-separated
    /// `bits:red:green:blue:alpha` values after the `=`. The reference's
    /// 1025-byte line buffer keeps what earlier lines left past the
    /// terminator, and a line without `=` reads on into it; so does the
    /// port's.
    #[must_use]
    pub fn from_section(section: Option<&EngineSection>) -> FormatClasses {
        let mut catalog = Self::builtin();
        let Some(section) = section else {
            return catalog;
        };
        let mut line = [0u8; 0x401];
        let mut index = 0;
        while read_line_into(section, index, &mut line[..0x400]) {
            index += 1;
            catalog.parse_line(&mut line);
        }
        catalog
    }

    fn parse_line(&mut self, line: &mut [u8; 0x401]) {
        // Bytes past the buffer read as 0 (the reference reads its stack).
        fn at(line: &[u8], i: usize) -> u8 {
            line.get(i).copied().unwrap_or(0)
        }
        let end = line.iter().position(|&b| b == 0).unwrap_or(0x400);
        if let Some(semi) = line[..end].iter().position(|&b| b == b';') {
            line[semi] = 0;
        }
        let mut cursor = 0;
        while matches!(at(line, cursor), b' ' | b'\t') {
            cursor += 1;
        }
        let token = cursor;
        let word = [0, 1, 2, 3].map(|i| at(line, token + i));
        if word[..3].eq_ignore_ascii_case(b"cle") {
            self.classes.clear();
            return;
        }
        let key = u32::from_le_bytes(word);
        let at_class = match self.classes.iter().position(|c| c.0 == key) {
            Some(i) => i,
            None => {
                self.classes.push((key, Vec::new()));
                self.classes.len() - 1
            }
        };
        self.classes[at_class].1.clear();
        while !matches!(at(line, cursor), 0 | b'=') {
            cursor += 1;
        }
        cursor += 1;
        while at(line, cursor) != 0 {
            while matches!(at(line, cursor), b' ' | b'\t') {
                cursor += 1;
            }
            let start = cursor;
            while !matches!(at(line, cursor), 0 | b',') {
                cursor += 1;
            }
            let value_end = cursor;
            if at(line, cursor) != 0 {
                line[cursor] = 0;
                cursor += 1;
            }
            if start != cursor {
                let fields = scan_fields(&line[start..value_end]);
                let format = by_components(
                    fields[0].cast_unsigned(),
                    fields[1].cast_unsigned(),
                    fields[2].cast_unsigned(),
                    fields[3].cast_unsigned(),
                    fields[4].cast_unsigned(),
                );
                if format != PixelFormat::UNKNOWN {
                    self.classes[at_class].1.push(format);
                }
            }
        }
    }

    /// The candidates of a class.
    #[must_use]
    pub fn candidates(&self, key: u32) -> Option<&[PixelFormat]> {
        self.classes
            .iter()
            .find(|c| c.0 == key)
            .map(|c| c.1.as_slice())
    }

    /// `CreateTexture`'s choice: `format` itself when `supported` says so,
    /// else the first supported candidate of its class.
    #[must_use]
    pub fn resolve(
        &self,
        format: PixelFormat,
        supported: impl Fn(PixelFormat) -> bool,
    ) -> Option<PixelFormat> {
        if supported(format) {
            return Some(format);
        }
        let key = class_key(format);
        if key == 0 {
            return None;
        }
        self.candidates(key)?
            .iter()
            .copied()
            .find(|&f| supported(f))
    }
}

/// `sscanf("%d:%d:%d:%d:%d")` with the fields that do not convert left at
/// 0.
fn scan_fields(text: &[u8]) -> [i32; 5] {
    let mut out = [0i32; 5];
    let mut at = 0;
    for (i, slot) in out.iter_mut().enumerate() {
        if i != 0 {
            if text.get(at) != Some(&b':') {
                break;
            }
            at += 1;
        }
        while matches!(
            text.get(at),
            Some(b' ' | b'\t' | b'\n' | b'\r' | 0x0B | 0x0C)
        ) {
            at += 1;
        }
        let start = at;
        if matches!(text.get(at), Some(b'+' | b'-')) {
            at += 1;
        }
        let digits = at;
        while text.get(at).is_some_and(u8::is_ascii_digit) {
            at += 1;
        }
        if at == digits {
            break;
        }
        let mut value: i64 = 0;
        for &d in &text[digits..at] {
            value = (value * 10 + i64::from(d - b'0')).min(i64::from(u32::MAX) + 1);
        }
        if text[start] == b'-' {
            value = -value;
        }
        *slot = value as i32;
    }
    out
}

/// The port's device support with the three gates of `CheckTextureFormat`
/// (`allow_8bit`, `allow_32bit`, `allow_dxt`): the formats
/// [`crate::render::ff::textures::Textures`] can create and fill.
#[must_use]
pub fn port_supports(
    format: PixelFormat,
    allow_8bit: bool,
    allow_32bit: bool,
    allow_dxt: bool,
) -> bool {
    if format == PixelFormat::UNKNOWN {
        return false;
    }
    if !allow_8bit && format == PixelFormat::P8 {
        return false;
    }
    if !allow_32bit && format.bits_per_pixel() > 0x1F {
        return false;
    }
    if !allow_dxt && format.is_dxt() {
        return false;
    }
    matches!(
        format,
        PixelFormat::R8G8B8
            | PixelFormat::R5G6B5
            | PixelFormat::X1R5G5B5
            | PixelFormat::A4R4G4B4
            | PixelFormat::A1R5G5B5
            | PixelFormat::A8R8G8B8
            | PixelFormat::X8R8G8B8
    ) || format.is_dxt()
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::ini::{EngineIni, NoIncludes};

    fn support(f: PixelFormat) -> bool {
        port_supports(f, false, true, true)
    }

    #[test]
    fn builtin_classes_resolve_as_on_a_full_device() {
        let c = FormatClasses::builtin();
        assert_eq!(c.candidates(fourcc(*b"DAOP")).unwrap().len(), 6);
        assert_eq!(
            c.resolve(PixelFormat::DAOP, support),
            Some(PixelFormat::R5G6B5)
        );
        assert_eq!(
            c.resolve(PixelFormat::DAOT, support),
            Some(PixelFormat::R5G6B5)
        );
        assert_eq!(
            c.resolve(PixelFormat::DAA1, support),
            Some(PixelFormat::A1R5G5B5)
        );
        assert_eq!(
            c.resolve(PixelFormat::DAA4, support),
            Some(PixelFormat::A4R4G4B4)
        );
        assert_eq!(
            c.resolve(PixelFormat::DAA8, support),
            Some(PixelFormat::A8R8G8B8)
        );
        assert_eq!(
            c.resolve(PixelFormat::DXT3, support),
            Some(PixelFormat::DXT3)
        );
        let no_dxt = |f| port_supports(f, false, true, false);
        assert_eq!(
            c.resolve(PixelFormat::DXT1, no_dxt),
            Some(PixelFormat::A1R5G5B5)
        );
        assert_eq!(c.resolve(PixelFormat::P8, support), None);
        let with_8 = |f| port_supports(f, true, true, true);
        assert_eq!(c.resolve(PixelFormat::P8, with_8), None);
    }

    #[test]
    fn the_ini_replaces_classes() {
        let mut ini = EngineIni::new();
        ini.load_buffer(
            b"[TextureFormatClasses]\nDAOT = 32:8:8:8:0, 16:5:6:5:0 ; x\nDAA8 = \n",
            &mut NoIncludes,
        )
        .unwrap();
        let c = FormatClasses::from_section(ini.section(b"TextureFormatClasses"));
        assert_eq!(
            c.candidates(fourcc(*b"DAOT")).unwrap(),
            &[PixelFormat::X8R8G8B8, PixelFormat::R5G6B5]
        );
        assert_eq!(c.candidates(fourcc(*b"DAA8")).unwrap(), &[]);
        assert_eq!(scan_fields(b" 16: 5:x"), [16, 5, 0, 0, 0]);
    }
}
