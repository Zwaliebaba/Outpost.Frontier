//! A small JSON reader for renderer trace lines (record 0004, point 10):
//! objects, arrays, strings with escapes, integers, true, false and null.
//! Floats in a trace are hex strings, so numbers are integers only; a
//! fraction or an exponent is an error.

use std::fmt;

/// A JSON value.
#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i64),
    Str(String),
    Array(Vec<Json>),
    /// Members in file order.
    Object(Vec<(String, Json)>),
}

impl Json {
    /// The member `key` of an object.
    #[must_use]
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(members) => members.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    #[must_use]
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Json::Int(n) => Some(*n),
            _ => None,
        }
    }

    #[must_use]
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    #[must_use]
    pub fn as_array(&self) -> Option<&[Json]> {
        match self {
            Json::Array(a) => Some(a),
            _ => None,
        }
    }

    #[must_use]
    pub fn is_null(&self) -> bool {
        matches!(self, Json::Null)
    }
}

/// Where and why a line did not parse.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct JsonError {
    pub at: usize,
    pub what: &'static str,
}

impl fmt::Display for JsonError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} at byte {}", self.what, self.at)
    }
}

impl std::error::Error for JsonError {}

struct Parser<'a> {
    s: &'a [u8],
    at: usize,
}

impl Parser<'_> {
    fn err<T>(&self, what: &'static str) -> Result<T, JsonError> {
        Err(JsonError { at: self.at, what })
    }

    fn ws(&mut self) {
        while self.s.get(self.at).is_some_and(|b| b" \t\r\n".contains(b)) {
            self.at += 1;
        }
    }

    fn eat(&mut self, b: u8) -> bool {
        if self.s.get(self.at) == Some(&b) {
            self.at += 1;
            true
        } else {
            false
        }
    }

    fn literal(&mut self, word: &[u8], value: Json) -> Result<Json, JsonError> {
        if self.s[self.at..].starts_with(word) {
            self.at += word.len();
            Ok(value)
        } else {
            self.err("unexpected character")
        }
    }

    fn value(&mut self) -> Result<Json, JsonError> {
        self.ws();
        match self.s.get(self.at) {
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => self.string().map(Json::Str),
            Some(b't') => self.literal(b"true", Json::Bool(true)),
            Some(b'f') => self.literal(b"false", Json::Bool(false)),
            Some(b'n') => self.literal(b"null", Json::Null),
            Some(b'-' | b'0'..=b'9') => self.number(),
            Some(_) => self.err("unexpected character"),
            None => self.err("unexpected end"),
        }
    }

    fn number(&mut self) -> Result<Json, JsonError> {
        let start = self.at;
        self.eat(b'-');
        while self.s.get(self.at).is_some_and(u8::is_ascii_digit) {
            self.at += 1;
        }
        if self.s.get(self.at).is_some_and(|b| b".eE".contains(b)) {
            return self.err("non-integer number");
        }
        std::str::from_utf8(&self.s[start..self.at])
            .ok()
            .and_then(|t| t.parse().ok())
            .map(Json::Int)
            .ok_or(JsonError {
                at: start,
                what: "bad number",
            })
    }

    fn hex4(&mut self) -> Result<u32, JsonError> {
        let digits = self.s.get(self.at..self.at + 4).ok_or(JsonError {
            at: self.at,
            what: "short escape",
        })?;
        let text = std::str::from_utf8(digits).map_err(|_| JsonError {
            at: self.at,
            what: "bad escape",
        })?;
        let v = u32::from_str_radix(text, 16).map_err(|_| JsonError {
            at: self.at,
            what: "bad escape",
        })?;
        self.at += 4;
        Ok(v)
    }

    fn string(&mut self) -> Result<String, JsonError> {
        self.at += 1;
        let mut out = String::new();
        loop {
            let Some(&b) = self.s.get(self.at) else {
                return self.err("unterminated string");
            };
            self.at += 1;
            match b {
                b'"' => return Ok(out),
                b'\\' => {
                    let Some(&e) = self.s.get(self.at) else {
                        return self.err("unterminated string");
                    };
                    self.at += 1;
                    match e {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut c = self.hex4()?;
                            if (0xd800..0xdc00).contains(&c)
                                && self.s[self.at..].starts_with(b"\\u")
                            {
                                self.at += 2;
                                let low = self.hex4()?;
                                c = 0x10000
                                    + ((c - 0xd800) << 10)
                                    + (low.wrapping_sub(0xdc00) & 0x3ff);
                            }
                            out.push(char::from_u32(c).unwrap_or('\u{fffd}'));
                        }
                        _ => return self.err("bad escape"),
                    }
                }
                _ => {
                    // Copy a run of plain UTF-8 bytes.
                    let start = self.at - 1;
                    while self
                        .s
                        .get(self.at)
                        .is_some_and(|&c| c != b'"' && c != b'\\')
                    {
                        self.at += 1;
                    }
                    out.push_str(&String::from_utf8_lossy(&self.s[start..self.at]));
                }
            }
        }
    }

    fn array(&mut self) -> Result<Json, JsonError> {
        self.at += 1;
        let mut items = Vec::new();
        self.ws();
        if self.eat(b']') {
            return Ok(Json::Array(items));
        }
        loop {
            items.push(self.value()?);
            self.ws();
            if self.eat(b']') {
                return Ok(Json::Array(items));
            }
            if !self.eat(b',') {
                return self.err("expected , or ]");
            }
        }
    }

    fn object(&mut self) -> Result<Json, JsonError> {
        self.at += 1;
        let mut members = Vec::new();
        self.ws();
        if self.eat(b'}') {
            return Ok(Json::Object(members));
        }
        loop {
            self.ws();
            if self.s.get(self.at) != Some(&b'"') {
                return self.err("expected a key");
            }
            let key = self.string()?;
            self.ws();
            if !self.eat(b':') {
                return self.err("expected :");
            }
            members.push((key, self.value()?));
            self.ws();
            if self.eat(b'}') {
                return Ok(Json::Object(members));
            }
            if !self.eat(b',') {
                return self.err("expected , or }");
            }
        }
    }
}

/// Parses one JSON value that fills `text` (white space around it aside).
pub fn parse(text: &str) -> Result<Json, JsonError> {
    let mut p = Parser {
        s: text.as_bytes(),
        at: 0,
    };
    let v = p.value()?;
    p.ws();
    if p.at != p.s.len() {
        return p.err("text after the value");
    }
    Ok(v)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_trace_lines() {
        let v = parse(r#"{"seq":5120,"frame":300,"method":"SetRenderState","args":{"State":7,"Value":-1},"ret":0,"m":["3f800000",null,true]}"#).unwrap();
        assert_eq!(v.get("seq").and_then(Json::as_i64), Some(5120));
        assert_eq!(
            v.get("args")
                .and_then(|a| a.get("Value"))
                .and_then(Json::as_i64),
            Some(-1)
        );
        assert_eq!(
            v.get("m").and_then(Json::as_array).map(<[Json]>::len),
            Some(3)
        );
        assert_eq!(parse(r#""aé\n""#).unwrap(), Json::Str("a\u{e9}\n".into()));
        assert!(parse("1.5").is_err());
        assert!(parse("{\"a\":1} x").is_err());
        assert!(parse("[1,]").is_err());
    }
}
