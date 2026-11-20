//! What `fread` gives from a file opened in text mode (`"rt"`).

/// The bytes the Microsoft C runtime's `fread` returns for a whole file
/// opened in text mode: each CR LF pair read as LF, a lone CR kept, and the
/// input ending at the first Ctrl-Z (0x1A).
#[must_use]
pub fn text_mode_read(bytes: &[u8]) -> Vec<u8> {
    let mut text = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while let Some(&b) = bytes.get(i) {
        if b == 0x1A {
            break;
        }
        if b == b'\r' && bytes.get(i + 1) == Some(&b'\n') {
            i += 1;
            continue;
        }
        text.push(b);
        i += 1;
    }
    text
}

#[cfg(test)]
mod tests {
    use super::text_mode_read;

    #[test]
    fn translates_crlf_and_stops_at_ctrl_z() {
        assert_eq!(text_mode_read(b"a\r\nb\rc\r"), b"a\nb\rc\r");
        assert_eq!(text_mode_read(b"ab\x1acd"), b"ab");
        assert_eq!(text_mode_read(b"\r\r\n"), b"\r\n");
        assert!(text_mode_read(b"").is_empty());
    }
}
