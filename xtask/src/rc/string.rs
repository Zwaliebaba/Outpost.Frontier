//! String literals as `rc.exe` stores them: the bytes decoded through the
//! script's code page, then the escapes processed.

/// The code page a script is read in (`rc /c`, `#pragma code_page`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CodePage {
    /// Code page 65001, UTF-8. The resource DLL projects pass `/c65001`
    /// (`CppSource/Resources/ResourceDll.props`).
    Utf8,
    /// Code page 1252, Windows Western European.
    Windows1252,
}

impl CodePage {
    pub fn from_number(number: u32) -> Option<Self> {
        match number {
            65001 => Some(Self::Utf8),
            1252 => Some(Self::Windows1252),
            _ => None,
        }
    }

    /// The UTF-16 units of `bytes` in this code page.
    pub fn decode(self, bytes: &[u8]) -> Result<Vec<u16>, String> {
        match self {
            Self::Utf8 => std::str::from_utf8(bytes)
                .map(|text| text.encode_utf16().collect())
                .map_err(|e| format!("invalid UTF-8 in a string ({e})")),
            Self::Windows1252 => Ok(bytes.iter().map(|&b| cp1252(b)).collect()),
        }
    }
}

/// Code page 1252 as `MultiByteToWideChar` maps it: 0x80..=0x9F to the
/// typographic characters, the five unassigned bytes to the C1 controls,
/// every other byte to itself.
fn cp1252(byte: u8) -> u16 {
    const HIGH: [u16; 32] = [
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    ];
    match byte {
        0x80..=0x9F => HIGH[usize::from(byte - 0x80)],
        _ => u16::from(byte),
    }
}

fn hex_digit(unit: u16) -> Option<u16> {
    let ch = char::from_u32(u32::from(unit))?;
    ch.to_digit(16).and_then(|d| u16::try_from(d).ok())
}

fn is_octal(unit: u16) -> bool {
    (u16::from(b'0')..=u16::from(b'7')).contains(&unit)
}

/// The UTF-16 units `rc.exe` stores for one string literal: `raw` is the
/// content between the quotes, `wide` an `L"..."` literal.
///
/// - `""` is one quote; `\"` is one quote and must be followed by `"`.
/// - `\n`, `\r`, `\t` (also `\T`), `\\`; `\a` (also `\A`) is a backspace,
///   0x08, as in `rc.exe`.
/// - `\x` takes up to 2 hex digits (4 in a wide literal); an octal escape
///   up to 3 digits (7 in a wide literal).
/// - Any other escape is kept with its backslash in a narrow literal and
///   dropped in a wide one.
/// - In a narrow literal a numeric escape is a byte of the code page:
///   above 0xFF it is an error, and above 0x7F in code page 65001 too
///   (a lone byte is not UTF-8). A `\x` without digits is an error.
///   These cases do not occur in the game's scripts.
pub fn process(raw: &[u8], wide: bool, code_page: CodePage) -> Result<Vec<u16>, String> {
    let chars = code_page.decode(raw)?;
    let quote = u16::from(b'"');
    let backslash = u16::from(b'\\');
    let mut out = Vec::with_capacity(chars.len());
    let escaped = |value: u16| -> Result<u16, String> {
        if wide {
            return Ok(value);
        }
        if value > 0xFF {
            return Err(format!(
                "escape value 0x{value:x} does not fit a byte in a narrow string"
            ));
        }
        if value < 0x80 {
            return Ok(value);
        }
        match code_page {
            CodePage::Utf8 => Err(format!(
                "escaped byte 0x{value:02x} in a narrow string is not UTF-8 by itself"
            )),
            CodePage::Windows1252 => Ok(cp1252(u8::try_from(value).unwrap_or_default())),
        }
    };
    let mut i = 0;
    while i < chars.len() {
        let ch = chars[i];
        i += 1;
        if ch == quote {
            if chars.get(i) != Some(&quote) {
                return Err("a lone quote inside a string".to_owned());
            }
            i += 1;
            out.push(quote);
            continue;
        }
        if ch != backslash {
            out.push(ch);
            continue;
        }
        let Some(&kind) = chars.get(i) else {
            return Err("a backslash at the end of a string".to_owned());
        };
        i += 1;
        if kind == u16::from(b'x') || kind == u16::from(b'X') {
            let mut value = 0u16;
            let mut digits = 0;
            let max = if wide { 4 } else { 2 };
            while digits < max {
                let Some(d) = chars.get(i).copied().and_then(hex_digit) else {
                    break;
                };
                value = (value << 4) | d;
                digits += 1;
                i += 1;
            }
            if digits == 0 {
                return Err("\\x without hex digits".to_owned());
            }
            out.push(escaped(value)?);
            continue;
        }
        if is_octal(kind) {
            let mut value = u32::from(kind - u16::from(b'0'));
            let max = if wide { 6 } else { 2 };
            let mut digits = 0;
            while digits < max && chars.get(i).is_some_and(|&c| is_octal(c)) {
                value = (value << 3) | u32::from(chars[i] - u16::from(b'0'));
                digits += 1;
                i += 1;
            }
            let value = u16::try_from(value & 0xFFFF).unwrap_or_default();
            out.push(escaped(value)?);
            continue;
        }
        match u8::try_from(kind).unwrap_or(0) {
            b'a' | b'A' => out.push(0x08),
            b'n' => out.push(u16::from(b'\n')),
            b'r' => out.push(u16::from(b'\r')),
            b't' | b'T' => out.push(u16::from(b'\t')),
            b'\\' => out.push(backslash),
            b'"' => {
                if chars.get(i) != Some(&quote) {
                    return Err("\\\" must be followed by another quote".to_owned());
                }
                i += 1;
                out.push(quote);
            }
            _ => {
                if !wide {
                    out.push(backslash);
                    out.push(kind);
                }
            }
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn narrow(raw: &str) -> Result<Vec<u16>, String> {
        process(raw.as_bytes(), false, CodePage::Utf8)
    }

    fn units(text: &str) -> Vec<u16> {
        text.encode_utf16().collect()
    }

    #[test]
    fn simple_escapes() {
        assert_eq!(
            narrow(r"a\nb\tc\Td\\e\rf").unwrap(),
            units("a\nb\tc\td\\e\rf")
        );
        assert_eq!(narrow(r"\a\A").unwrap(), vec![8, 8]);
        assert_eq!(narrow(r#"say ""hi"" \"""#).unwrap(), units("say \"hi\" \""));
    }

    #[test]
    fn numeric_escapes() {
        assert_eq!(narrow(r"\x41\x4a2\101\0\1012").unwrap(), units("AJ2A\0A2"));
        assert_eq!(
            process(br"\x263a\x41", true, CodePage::Utf8).unwrap(),
            vec![0x263A, 0x41]
        );
        assert_eq!(
            process(br"\101\1010", true, CodePage::Utf8).unwrap(),
            vec![0o101, 0o1010]
        );
        assert!(narrow(r"\xe9").is_err());
        assert!(narrow(r"\777").is_err());
        assert!(narrow(r"\xg").is_err());
        assert_eq!(
            process(br"\x80\351", false, CodePage::Windows1252).unwrap(),
            vec![0x20AC, 0xE9]
        );
    }

    #[test]
    fn unknown_escapes() {
        assert_eq!(narrow(r"\q\N").unwrap(), units(r"\q\N"));
        assert_eq!(
            process(br"a\qb", true, CodePage::Utf8).unwrap(),
            units("ab")
        );
    }

    #[test]
    fn code_pages() {
        assert_eq!(
            narrow("caf\u{e9} \u{2019}").unwrap(),
            units("caf\u{e9} \u{2019}")
        );
        assert_eq!(narrow("\u{1F600}").unwrap(), units("\u{1F600}"));
        assert!(process(b"\x92", false, CodePage::Utf8).is_err());
        assert_eq!(
            process(b"\x92\x81\xe9", false, CodePage::Windows1252).unwrap(),
            vec![0x2019, 0x81, 0xE9]
        );
    }

    #[test]
    fn malformed() {
        assert!(narrow(r#"a\"b"#).is_err());
        assert!(narrow("a\\").is_err());
        assert!(narrow("a\"b").is_err());
    }
}
