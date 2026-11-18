//! `HexToWide` and `WideToHex` (`Common/common_misc.cpp`): wide strings as
//! four lower-case hex digits per UTF-16 code unit. The client keeps login
//! names and passwords in this form, and the server its descriptions.

/// `WideToHex`: each code unit as `%04x`.
#[must_use]
pub fn wide_to_hex(wide: &[u16]) -> String {
    use std::fmt::Write;
    let mut out = String::with_capacity(wide.len() * 4);
    for &unit in wide.iter().take_while(|&&u| u != 0) {
        let _ = write!(out, "{unit:04x}");
    }
    out
}

/// `HexToWide`: text of hex digits only (the empty text included) is read
/// as `%04x` groups; any other text is widened byte by byte, as `mbstowcs`
/// does in the C locale.
///
/// The C++ steps four characters after every group, so a last group of one
/// to three digits is read and the scan then continues past the terminator
/// (undefined behaviour); here the scan stops after that group (D268).
#[must_use]
pub fn hex_to_wide(text: &[u8]) -> Vec<u16> {
    let text = &text[..text.iter().position(|&b| b == 0).unwrap_or(text.len())];
    if !text.iter().all(u8::is_ascii_hexdigit) {
        return text.iter().map(|&b| u16::from(b)).collect();
    }
    text.chunks(4)
        .map(|group| {
            group.iter().fold(0u16, |v, &d| {
                // Every byte is a hex digit (checked above).
                let digit = (d as char).to_digit(16).unwrap_or(0) as u16;
                (v << 4) | digit
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn round_trips_code_units() {
        let w = wide("Pilot é€");
        let hex = wide_to_hex(&w);
        assert_eq!(&hex[..8], "00500069");
        assert_eq!(&hex[hex.len() - 4..], "20ac");
        assert_eq!(hex_to_wide(hex.as_bytes()), w);
        assert_eq!(hex_to_wide(b"0041ABCD"), vec![0x41, 0xabcd]);
    }

    #[test]
    fn other_text_is_widened_and_empty_is_empty() {
        assert_eq!(hex_to_wide(b"name1!"), wide("name1!"));
        assert_eq!(hex_to_wide(b"\xe9x"), vec![0xe9, u16::from(b'x')]);
        assert!(hex_to_wide(b"").is_empty());
        assert_eq!(wide_to_hex(&[0x41, 0, 0x42]), "0041");
    }

    #[test]
    fn a_short_last_group_ends_the_scan() {
        assert_eq!(hex_to_wide(b"004104"), vec![0x41, 0x04]);
    }
}
