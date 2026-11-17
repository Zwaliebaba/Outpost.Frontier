//! The JSON of the scenario oracle's records (`design/sim-records.md`,
//! "Encoding"): the subset the C++ writer `oracle_json.h` produces, read
//! into a small value tree, and the matching writer pieces for
//! [`super::PsiRecord::to_line`].
//!
//! Floats travel as strings of their bit patterns, so no value here goes
//! through decimal formatting: numbers stay text until a field asks for
//! an integer.

use super::RecordError;

/// One JSON value. Objects keep their keys in order.
#[derive(Clone, Debug, PartialEq)]
pub(super) enum Json {
    Null,
    Bool(bool),
    /// A number as written.
    Number(String),
    Str(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

fn err(text: impl Into<String>) -> RecordError {
    RecordError(text.into())
}

/// Parses one line holding one JSON value.
pub(super) fn parse(line: &str) -> Result<Json, RecordError> {
    let mut parser = Parser {
        bytes: line.as_bytes(),
        pos: 0,
    };
    let value = parser.value()?;
    parser.skip_space();
    if parser.pos != parser.bytes.len() {
        return Err(err(format!("text after the value at byte {}", parser.pos)));
    }
    Ok(value)
}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl Parser<'_> {
    fn skip_space(&mut self) {
        while self
            .bytes
            .get(self.pos)
            .is_some_and(|b| matches!(b, b' ' | b'\t' | b'\r' | b'\n'))
        {
            self.pos += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn expect(&mut self, byte: u8) -> Result<(), RecordError> {
        self.skip_space();
        if self.peek() == Some(byte) {
            self.pos += 1;
            Ok(())
        } else {
            Err(err(format!(
                "expected '{}' at byte {}",
                char::from(byte),
                self.pos
            )))
        }
    }

    fn literal(&mut self, word: &str, value: Json) -> Result<Json, RecordError> {
        if self.bytes[self.pos..].starts_with(word.as_bytes()) {
            self.pos += word.len();
            Ok(value)
        } else {
            Err(err(format!("bad literal at byte {}", self.pos)))
        }
    }

    fn value(&mut self) -> Result<Json, RecordError> {
        self.skip_space();
        match self.peek() {
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => self.string().map(Json::Str),
            Some(b't') => self.literal("true", Json::Bool(true)),
            Some(b'f') => self.literal("false", Json::Bool(false)),
            Some(b'n') => self.literal("null", Json::Null),
            Some(b'-' | b'0'..=b'9') => {
                let start = self.pos;
                while self
                    .peek()
                    .is_some_and(|b| matches!(b, b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9'))
                {
                    self.pos += 1;
                }
                let text = core::str::from_utf8(&self.bytes[start..self.pos])
                    .map_err(|e| err(e.to_string()))?;
                Ok(Json::Number(text.to_owned()))
            }
            _ => Err(err(format!("expected a value at byte {}", self.pos))),
        }
    }

    fn object(&mut self) -> Result<Json, RecordError> {
        self.expect(b'{')?;
        let mut fields = Vec::new();
        self.skip_space();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Ok(Json::Object(fields));
        }
        loop {
            self.skip_space();
            let key = self.string()?;
            self.expect(b':')?;
            let value = self.value()?;
            fields.push((key, value));
            self.skip_space();
            match self.peek() {
                Some(b',') => self.pos += 1,
                Some(b'}') => {
                    self.pos += 1;
                    return Ok(Json::Object(fields));
                }
                _ => return Err(err(format!("expected ',' or '}}' at byte {}", self.pos))),
            }
        }
    }

    fn array(&mut self) -> Result<Json, RecordError> {
        self.expect(b'[')?;
        let mut items = Vec::new();
        self.skip_space();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Ok(Json::Array(items));
        }
        loop {
            items.push(self.value()?);
            self.skip_space();
            match self.peek() {
                Some(b',') => self.pos += 1,
                Some(b']') => {
                    self.pos += 1;
                    return Ok(Json::Array(items));
                }
                _ => return Err(err(format!("expected ',' or ']' at byte {}", self.pos))),
            }
        }
    }

    fn hex4(&mut self) -> Result<u32, RecordError> {
        let digits = self
            .bytes
            .get(self.pos..self.pos + 4)
            .and_then(|d| core::str::from_utf8(d).ok())
            .ok_or_else(|| err(format!("short \\u escape at byte {}", self.pos)))?;
        let unit = u32::from_str_radix(digits, 16).map_err(|e| err(e.to_string()))?;
        self.pos += 4;
        Ok(unit)
    }

    fn string(&mut self) -> Result<String, RecordError> {
        self.expect(b'"')?;
        let mut out = String::new();
        loop {
            let start = self.pos;
            while self.peek().is_some_and(|b| b != b'"' && b != b'\\') {
                self.pos += 1;
            }
            out.push_str(
                core::str::from_utf8(&self.bytes[start..self.pos])
                    .map_err(|e| err(e.to_string()))?,
            );
            match self.peek() {
                Some(b'"') => {
                    self.pos += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    self.pos += 1;
                    let escape = self.peek().ok_or_else(|| err("unterminated string"))?;
                    self.pos += 1;
                    match escape {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut unit = self.hex4()?;
                            if (0xd800..0xdc00).contains(&unit)
                                && self.bytes[self.pos..].starts_with(b"\\u")
                            {
                                self.pos += 2;
                                let low = self.hex4()?;
                                unit =
                                    0x10000 + ((unit - 0xd800) << 10) + (low.wrapping_sub(0xdc00));
                            }
                            out.push(char::from_u32(unit).unwrap_or('\u{fffd}'));
                        }
                        other => {
                            return Err(err(format!("bad escape \\{}", char::from(other))));
                        }
                    }
                }
                _ => return Err(err("unterminated string")),
            }
        }
    }
}

impl Json {
    /// The value of `key` in an object.
    pub(super) fn get(&self, key: &str) -> Result<&Json, RecordError> {
        match self {
            Json::Object(fields) => fields
                .iter()
                .find(|(k, _)| k == key)
                .map(|(_, v)| v)
                .ok_or_else(|| err(format!("missing {key}"))),
            _ => Err(err(format!("{key}: not in an object"))),
        }
    }

    /// A `u` number.
    pub(super) fn as_u32(&self, what: &str) -> Result<u32, RecordError> {
        match self {
            Json::Number(text) => text.parse().map_err(|e| err(format!("{what}: {e}"))),
            _ => Err(err(format!("{what}: not a number"))),
        }
    }

    /// A `b` value.
    pub(super) fn as_bool(&self, what: &str) -> Result<bool, RecordError> {
        match self {
            Json::Bool(value) => Ok(*value),
            _ => Err(err(format!("{what}: not true or false"))),
        }
    }

    /// An `s` value.
    pub(super) fn as_str(&self, what: &str) -> Result<&str, RecordError> {
        match self {
            Json::Str(text) => Ok(text),
            _ => Err(err(format!("{what}: not a string"))),
        }
    }

    /// An `f` value: the bit pattern in 8 hex digits.
    pub(super) fn as_f32(&self, what: &str) -> Result<f32, RecordError> {
        let text = self.as_str(what)?;
        if text.len() != 8 {
            return Err(err(format!("{what}: {text} is not 8 hex digits")));
        }
        u32::from_str_radix(text, 16)
            .map(f32::from_bits)
            .map_err(|e| err(format!("{what}: {e}")))
    }

    /// The elements of an array.
    pub(super) fn as_array(&self, what: &str) -> Result<&[Json], RecordError> {
        match self {
            Json::Array(items) => Ok(items),
            _ => Err(err(format!("{what}: not an array"))),
        }
    }

    /// An array of exactly `count` `f` values.
    pub(super) fn as_floats(&self, what: &str, count: usize) -> Result<Vec<f32>, RecordError> {
        let items = self.as_array(what)?;
        if items.len() != count {
            return Err(err(format!(
                "{what}: {} floats, expected {count}",
                items.len()
            )));
        }
        items.iter().map(|item| item.as_f32(what)).collect()
    }
}

/// Appends `"key":` with the comma the writer puts between fields.
pub(super) fn key(out: &mut String, name: &str) {
    if !matches!(out.chars().last(), Some('{' | '[') | None) {
        out.push(',');
    }
    out.push('"');
    out.push_str(name);
    out.push_str("\":");
}

/// An `f` value.
pub(super) fn float(value: f32) -> String {
    format!("\"{:08x}\"", value.to_bits())
}

/// An array of `f` values.
pub(super) fn floats(values: &[f32]) -> String {
    let items: Vec<String> = values.iter().map(|v| float(*v)).collect();
    format!("[{}]", items.join(","))
}

/// An `s` value as `oracle_json.h` writes a narrow string: `"` and `\`
/// escaped, printable ASCII as is, everything else as `\u00XX` per byte
/// (the C++ writes Latin-1 bytes; a Rust string's characters above U+00FF
/// are written by their UTF-16 units).
pub(super) fn string(text: &str) -> String {
    let mut out = String::from("\"");
    for ch in text.chars() {
        match ch {
            '"' | '\\' => {
                out.push('\\');
                out.push(ch);
            }
            ' '..='~' => out.push(ch),
            _ => {
                let mut units = [0u16; 2];
                for unit in ch.encode_utf16(&mut units) {
                    out.push_str(&format!("\\u{unit:04x}"));
                }
            }
        }
    }
    out.push('"');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn values_parse() {
        let line = r#"{"a":1,"b":[true,false,null],"c":{"d":"3f800000"},"e":"x\"\\\u00e9\ud83d\ude00","f":-2.5e3,"g":[]}"#;
        let value = parse(line).unwrap();
        assert_eq!(value.get("a").unwrap().as_u32("a").unwrap(), 1);
        let b = value.get("b").unwrap().as_array("b").unwrap();
        assert_eq!(b, &[Json::Bool(true), Json::Bool(false), Json::Null]);
        let d = value.get("c").unwrap().get("d").unwrap();
        assert_eq!(d.as_f32("d").unwrap(), 1.0);
        assert_eq!(
            value.get("e").unwrap().as_str("e").unwrap(),
            "x\"\\\u{e9}\u{1f600}"
        );
        assert_eq!(value.get("f").unwrap(), &Json::Number("-2.5e3".into()));
        assert!(
            value
                .get("g")
                .unwrap()
                .as_floats("g", 0)
                .unwrap()
                .is_empty()
        );
        assert!(value.get("z").is_err());
        assert!(parse("{\"a\":1} x").is_err());
        assert!(parse("{\"a\":}").is_err());
        assert!(parse("[1,2").is_err());
    }

    #[test]
    fn strings_round_trip() {
        let text = "ship \"one\" \\ \u{e9}";
        let written = string(text);
        assert_eq!(written, "\"ship \\\"one\\\" \\\\ \\u00e9\"");
        assert_eq!(parse(&written).unwrap(), Json::Str(text.into()));
        let mut out = String::from("{");
        key(&mut out, "a");
        out.push_str(&floats(&[1.0, -0.0]));
        key(&mut out, "b");
        out.push_str(&float(0.5));
        out.push('}');
        assert_eq!(out, r#"{"a":["3f800000","80000000"],"b":"3f000000"}"#);
    }
}
