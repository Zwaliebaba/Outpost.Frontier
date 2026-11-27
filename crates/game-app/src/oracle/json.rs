//! One line of an oracle file (`oracle_json.h`, `FOracleJson`): a JSON
//! object built in place, byte for byte as the C++ writes it. Floats are
//! their bit pattern in 8 lower-case hex digits, doubles in 16; narrow
//! strings byte by byte with every byte from 0x80 as `\u00XX`; wide strings
//! unit by unit with every unit outside printable ASCII as `\uXXXX`; byte
//! blocks one string of lower-case hex.

use std::fmt::Write as _;

/// A line under construction.
#[derive(Clone, Debug)]
pub struct OracleJson {
    text: String,
    need_comma: Vec<bool>,
}

impl Default for OracleJson {
    fn default() -> Self {
        Self::new()
    }
}

impl OracleJson {
    /// An open object.
    #[must_use]
    pub fn new() -> Self {
        Self {
            text: String::from("{"),
            need_comma: vec![false],
        }
    }

    fn sep(&mut self, name: Option<&str>) {
        if let Some(last) = self.need_comma.last_mut() {
            if *last {
                self.text.push(',');
            }
            *last = true;
        }
        if let Some(name) = name {
            self.text.push('"');
            self.text.push_str(name);
            self.text.push_str("\":");
        }
    }

    fn put_unit(&mut self, unit: u32) {
        if unit == u32::from(b'"') || unit == u32::from(b'\\') {
            self.text.push('\\');
            self.text.push(char::from_u32(unit).unwrap_or('?'));
        } else if (0x20..0x7f).contains(&unit) {
            self.text.push(char::from_u32(unit).unwrap_or('?'));
        } else {
            let _ = write!(self.text, "\\u{:04x}", unit & 0xffff);
        }
    }

    /// `Uint`: decimal.
    pub fn uint(&mut self, name: &str, value: u32) -> &mut Self {
        self.sep(Some(name));
        let _ = write!(self.text, "{value}");
        self
    }

    /// `Int`: decimal.
    pub fn int(&mut self, name: &str, value: i32) -> &mut Self {
        self.sep(Some(name));
        let _ = write!(self.text, "{value}");
        self
    }

    /// `Bool`.
    pub fn bool(&mut self, name: &str, value: bool) -> &mut Self {
        self.sep(Some(name));
        self.text.push_str(if value { "true" } else { "false" });
        self
    }

    /// `Null`.
    pub fn null(&mut self, name: &str) -> &mut Self {
        self.sep(Some(name));
        self.text.push_str("null");
        self
    }

    /// `Float`: the bit pattern.
    pub fn float(&mut self, name: &str, value: f32) -> &mut Self {
        self.sep(Some(name));
        let _ = write!(self.text, "\"{:08x}\"", value.to_bits());
        self
    }

    /// `Double`: the bit pattern.
    pub fn double(&mut self, name: &str, value: f64) -> &mut Self {
        self.sep(Some(name));
        let _ = write!(self.text, "\"{:016x}\"", value.to_bits());
        self
    }

    /// `Str` and `StrN`: a narrow string, byte by byte, up to its first NUL.
    pub fn str_bytes(&mut self, name: &str, value: &[u8]) -> &mut Self {
        self.sep(Some(name));
        self.text.push('"');
        for &b in value.iter().take_while(|&&b| b != 0) {
            self.put_unit(u32::from(b));
        }
        self.text.push('"');
        self
    }

    /// `Str` of ASCII text.
    pub fn str(&mut self, name: &str, value: &str) -> &mut Self {
        self.str_bytes(name, value.as_bytes())
    }

    /// `WStr` and `WStrN`: a wide string, unit by unit, up to its first NUL.
    pub fn wstr(&mut self, name: &str, value: &[u16]) -> &mut Self {
        self.sep(Some(name));
        self.text.push('"');
        for &u in value.iter().take_while(|&&u| u != 0) {
            self.put_unit(u32::from(u));
        }
        self.text.push('"');
        self
    }

    /// `Bytes`: lower-case hex.
    pub fn bytes(&mut self, name: &str, value: &[u8]) -> &mut Self {
        self.sep(Some(name));
        self.text.push('"');
        for b in value {
            let _ = write!(self.text, "{b:02x}");
        }
        self.text.push('"');
        self
    }

    /// `BeginObject`.
    pub fn begin_object(&mut self, name: &str) -> &mut Self {
        self.sep(Some(name));
        self.text.push('{');
        self.need_comma.push(false);
        self
    }

    /// `EndObject`.
    pub fn end_object(&mut self) -> &mut Self {
        self.text.push('}');
        self.need_comma.pop();
        self
    }

    /// `BeginArray(name)`.
    pub fn begin_array(&mut self, name: &str) -> &mut Self {
        self.sep(Some(name));
        self.text.push('[');
        self.need_comma.push(false);
        self
    }

    /// `BeginArray(nullptr)`: an array as an element of the open array.
    pub fn begin_array_item(&mut self) -> &mut Self {
        self.sep(None);
        self.text.push('[');
        self.need_comma.push(false);
        self
    }

    /// `BeginObject(nullptr)`: an object as an element of the open array.
    pub fn begin_object_item(&mut self) -> &mut Self {
        self.sep(None);
        self.text.push('{');
        self.need_comma.push(false);
        self
    }

    /// `Null(nullptr)` (also `Str(nullptr, nullptr)`): a null element.
    pub fn item_null(&mut self) -> &mut Self {
        self.sep(None);
        self.text.push_str("null");
        self
    }

    /// `EndArray`.
    pub fn end_array(&mut self) -> &mut Self {
        self.text.push(']');
        self.need_comma.pop();
        self
    }

    /// `Int(nullptr, value)`: an element of the open array.
    pub fn item_int(&mut self, value: i32) -> &mut Self {
        self.sep(None);
        let _ = write!(self.text, "{value}");
        self
    }

    /// `Uint(nullptr, value)`.
    pub fn item_uint(&mut self, value: u32) -> &mut Self {
        self.sep(None);
        let _ = write!(self.text, "{value}");
        self
    }

    /// `Float(nullptr, value)`: the bit pattern, as an element.
    pub fn item_float(&mut self, value: f32) -> &mut Self {
        self.sep(None);
        let _ = write!(self.text, "\"{:08x}\"", value.to_bits());
        self
    }

    /// `Str(nullptr, value)`: a narrow string element, up to its first NUL.
    pub fn item_str_bytes(&mut self, value: &[u8]) -> &mut Self {
        self.sep(None);
        self.text.push('"');
        for &b in value.iter().take_while(|&&b| b != 0) {
            self.put_unit(u32::from(b));
        }
        self.text.push('"');
        self
    }

    /// `Finish`: every open object closed; the line without its newline.
    #[must_use]
    pub fn finish(mut self) -> String {
        while !self.need_comma.is_empty() {
            self.text.push('}');
            self.need_comma.pop();
        }
        self.text
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_line_is_written_as_the_cpp_writes_it() {
        let mut j = OracleJson::new();
        j.uint("frame", 0).int("slot", -1).str("m", "A\"b\\c\u{1}");
        j.begin_object("a")
            .float("f", 1.0)
            .double("d", 1.0)
            .end_object();
        j.wstr("w", &[0x41, 0xe9, 0, 0x42])
            .bytes("b", &[0, 0xab])
            .bool("t", true);
        assert_eq!(
            j.finish(),
            r#"{"frame":0,"slot":-1,"m":"A\"b\\c\u0001","a":{"f":"3f800000","d":"3ff0000000000000"},"w":"A\u00e9","b":"00ab","t":true}"#
        );
    }

    #[test]
    fn narrow_strings_escape_high_bytes_and_stop_at_nul() {
        let mut j = OracleJson::new();
        j.str_bytes("s", &[0x41, 0x80, 0xff, 0, 0x42]);
        assert_eq!(j.finish(), r#"{"s":"A\u0080\u00ff"}"#);
    }

    #[test]
    fn arrays_hold_elements_and_nested_arrays() {
        let mut j = OracleJson::new();
        j.begin_array("keys");
        j.begin_array_item().item_int(-1).item_uint(4).end_array();
        j.begin_array_item().end_array();
        j.end_array()
            .begin_array("g")
            .item_str_bytes(b"IDR")
            .end_array();
        assert_eq!(j.finish(), r#"{"keys":[[-1,4],[]],"g":["IDR"]}"#);
    }
}
