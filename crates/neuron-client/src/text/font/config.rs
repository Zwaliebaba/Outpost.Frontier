//! The font configuration: the `[TrueType]` sections of `fonts.ini`
//! (`LoadFonts`, `font_imp.cpp`) and of `rich_fonts.ini`
//! (`LoadRichFontsFromIni`, `menu_fonts_load.cpp`), read with the
//! reference's `INI_Reader` ([`neuron_core::ini::IniReader`]).
//!
//! The `[Style]` sections of `rich_fonts.ini` build rich-text display lists
//! for the style registry; they belong to the rich-text layout and are not
//! read here.

use neuron_core::ini::IniReader;

/// The face, size and style of one of the 32 standard rich-text fonts
/// (`TextRenderContext::FontDesc`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StandardFont {
    /// The face, at most 127 bytes (`safe_strcpy` into 0x80).
    pub name: Vec<u8>,
    /// The unscaled height in pixels.
    pub size: u32,
    pub bold: bool,
    pub italic: bool,
    pub underline: bool,
}

/// One `[TrueType]` section of the fonts INI, as `LoadFonts` collects it.
#[derive(Clone, Debug, PartialEq)]
pub struct TrueTypeFont {
    /// `nickname`, the whole value, at most 127 bytes.
    pub nickname: Vec<u8>,
    /// `font`, its first field, at most 127 bytes.
    pub font: Vec<u8>,
    /// `fixed_height`, a share of the surface height.
    pub fixed_height: f32,
}

/// `CopyBoundedString(Dest, Capacity, Src)`: at most `capacity - 1` bytes.
fn bounded(value: &[u8], capacity: usize) -> Vec<u8> {
    let value = neuron_core::hash::c_str(value);
    value[..value.len().min(capacity - 1)].to_vec()
}

/// The `[TrueType]` sections of a fonts INI in file order, as `LoadFonts`
/// reads them: `nickname` (the whole value), `font` (field 0) and
/// `fixed_height` (field 0 as a float), each kept from its last line;
/// other sections and keys are skipped. An unreadable file gives none.
#[must_use]
pub fn read_fonts_ini(contents: &[u8]) -> Vec<TrueTypeFont> {
    let mut out = Vec::new();
    let mut reader = IniReader::new();
    if !reader.open_memory(contents) || !reader.read_header() {
        return out;
    }
    loop {
        if reader.is_header(b"TrueType") {
            let mut entry = TrueTypeFont {
                nickname: Vec::new(),
                font: Vec::new(),
                fixed_height: 0.0,
            };
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    entry.nickname = bounded(&reader.value_string(), 0x80);
                } else if reader.is_value(b"font") {
                    entry.font = bounded(&reader.value_string_at(0), 0x80);
                } else if reader.is_value(b"fixed_height") {
                    // get_value_float returns a float, stored as one.
                    entry.fixed_height = reader.value_float(0) as f32;
                }
            }
            out.push(entry);
        }
        if !reader.read_header() {
            break;
        }
    }
    out
}

/// The 32 standard fonts as the static initialiser of `Common.dll` sets
/// them before any `SetStandardFont`: a cycle over index `(i + 2) % 4` of
/// Vibrocentric 16, Vibrocentric 18, Arial Unicode MS 22 and Arial Unicode
/// MS 26, none bold, italic or underlined.
#[must_use]
pub fn default_standard_fonts() -> Vec<StandardFont> {
    const NAMES: [&[u8]; 4] = [
        b"Vibrocentric",
        b"Vibrocentric",
        b"Arial Unicode MS",
        b"Arial Unicode MS",
    ];
    const SIZES: [u32; 4] = [16, 18, 22, 26];
    (0..32)
        .map(|i| {
            let pattern = (i + 2) % 4;
            StandardFont {
                name: NAMES[pattern].to_vec(),
                size: SIZES[pattern],
                bold: false,
                italic: false,
                underline: false,
            }
        })
        .collect()
}

/// The `[TrueType]` part of `LoadRichFontsFromIni`: every `font = index,
/// face, size` line of every `[TrueType]` section, in order, passed to
/// `SetStandardFont` when the index (read as unsigned) is below 32. The
/// face is field 1, cut to 127 bytes; the size is field 2 through `atoi`,
/// as an unsigned long; the style bytes are always 0.
pub fn apply_rich_fonts_ini(contents: &[u8], fonts: &mut [StandardFont]) {
    let mut reader = IniReader::new();
    if !reader.open_memory(contents) || !reader.read_header() {
        return;
    }
    loop {
        if reader.is_header(b"TrueType") {
            while reader.read_value() {
                if !reader.is_value(b"font") {
                    continue;
                }
                let index = reader.value_int(0).cast_unsigned();
                let face = bounded(&reader.value_string_at(1), 0x80);
                let size = reader.value_int(2).cast_unsigned();
                if index < 0x20
                    && let Some(slot) = fonts.get_mut(index as usize)
                {
                    *slot = StandardFont {
                        name: face,
                        size,
                        bold: false,
                        italic: false,
                        underline: false,
                    };
                }
            }
        }
        if !reader.read_header() {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fonts_ini_sections() {
        let text = b"[Other]\nnickname = X\n[TrueType]\nnickname = ButtonText\nfont = Agency FB, 3\nfixed_height = 0.0434999987\n\n[TrueType]\nfont = Arial\n";
        let fonts = read_fonts_ini(text);
        assert_eq!(fonts.len(), 2);
        assert_eq!(fonts[0].nickname, b"ButtonText");
        assert_eq!(fonts[0].font, b"Agency FB");
        assert_eq!(fonts[0].fixed_height, 0.0435_f32);
        assert_eq!(fonts[1].nickname, b"");
        assert_eq!(fonts[1].fixed_height, 0.0);
    }

    #[test]
    fn rich_fonts_set_standard_fonts() {
        let mut fonts = default_standard_fonts();
        assert_eq!(fonts[0].name, b"Arial Unicode MS");
        assert_eq!(fonts[0].size, 22);
        assert_eq!(fonts[2].name, b"Vibrocentric");
        assert_eq!(fonts[3].size, 18);
        let text = b"[TrueType]\nfont = 0, Agency FB, 30\nfont = 32, Arial, 10\nfont = -1, Arial, 10\n[Style]\nfont = 1\n";
        apply_rich_fonts_ini(text, &mut fonts);
        assert_eq!(fonts[0].name, b"Agency FB");
        assert_eq!(fonts[0].size, 30);
        assert_eq!(
            (fonts[1].name.as_slice(), fonts[1].size),
            (&b"Arial Unicode MS"[..], 26)
        );
    }
}
