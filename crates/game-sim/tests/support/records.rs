//! The scenario oracle's records (`design/sim-records.md`): a small reader
//! for the JSON subset `oracle_json.h` writes, the `psi.jsonl` controller
//! inputs (`ctl`) and pushes, the `calls.jsonl` munition hits and the
//! `tick.jsonl` object states.

use std::fs;
use std::path::{Path, PathBuf};

/// A JSON value; objects keep their key order, numbers stay text.
#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    /// `null`.
    Null,
    /// `true` / `false`.
    Bool(bool),
    /// A number as written.
    Number(String),
    /// A string.
    Str(String),
    /// An array.
    Array(Vec<Json>),
    /// An object.
    Object(Vec<(String, Json)>),
}

impl Json {
    /// A field of an object.
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(fields) => fields.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    /// A string.
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    /// An `f32` written as its bit pattern.
    pub fn as_f32(&self) -> Option<f32> {
        u32::from_str_radix(self.as_str()?, 16).ok().map(f32::from_bits)
    }

    /// An `f64` written as its bit pattern.
    pub fn as_f64(&self) -> Option<f64> {
        u64::from_str_radix(self.as_str()?, 16).ok().map(f64::from_bits)
    }

    /// An unsigned integer.
    pub fn as_u32(&self) -> Option<u32> {
        match self {
            Json::Number(n) => n.parse().ok(),
            _ => None,
        }
    }

    /// A signed integer.
    pub fn as_i32(&self) -> Option<i32> {
        match self {
            Json::Number(n) => n.parse().ok(),
            _ => None,
        }
    }

    /// A boolean.
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Json::Bool(b) => Some(*b),
            _ => None,
        }
    }

    /// An array.
    pub fn as_array(&self) -> Option<&[Json]> {
        match self {
            Json::Array(a) => Some(a),
            _ => None,
        }
    }

    /// An array of floats.
    pub fn as_floats(&self) -> Option<Vec<f32>> {
        self.as_array()?.iter().map(Json::as_f32).collect()
    }

    /// Whether the value is `null`.
    pub fn is_null(&self) -> bool {
        matches!(self, Json::Null)
    }
}

struct Parser<'a> {
    b: &'a [u8],
    i: usize,
}

impl Parser<'_> {
    fn ws(&mut self) {
        while self.i < self.b.len() && matches!(self.b[self.i], b' ' | b'\t' | b'\r' | b'\n') {
            self.i += 1;
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        self.ws();
        let c = *self.b.get(self.i).ok_or("end of line")?;
        match c {
            b'{' => {
                self.i += 1;
                let mut fields = Vec::new();
                self.ws();
                if self.b.get(self.i) == Some(&b'}') {
                    self.i += 1;
                    return Ok(Json::Object(fields));
                }
                loop {
                    self.ws();
                    let Json::Str(k) = self.value()? else {
                        return Err(format!("key expected at {}", self.i));
                    };
                    self.ws();
                    if self.b.get(self.i) != Some(&b':') {
                        return Err(format!("':' expected at {}", self.i));
                    }
                    self.i += 1;
                    let v = self.value()?;
                    fields.push((k, v));
                    self.ws();
                    match self.b.get(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b'}') => {
                            self.i += 1;
                            return Ok(Json::Object(fields));
                        }
                        _ => return Err(format!("',' or '}}' expected at {}", self.i)),
                    }
                }
            }
            b'[' => {
                self.i += 1;
                let mut items = Vec::new();
                self.ws();
                if self.b.get(self.i) == Some(&b']') {
                    self.i += 1;
                    return Ok(Json::Array(items));
                }
                loop {
                    items.push(self.value()?);
                    self.ws();
                    match self.b.get(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b']') => {
                            self.i += 1;
                            return Ok(Json::Array(items));
                        }
                        _ => return Err(format!("',' or ']' expected at {}", self.i)),
                    }
                }
            }
            b'"' => {
                self.i += 1;
                let mut out = String::new();
                while let Some(&c) = self.b.get(self.i) {
                    self.i += 1;
                    match c {
                        b'"' => return Ok(Json::Str(out)),
                        b'\\' => {
                            let e = *self.b.get(self.i).ok_or("bad escape")?;
                            self.i += 1;
                            match e {
                                b'u' => {
                                    let hex = std::str::from_utf8(self.b.get(self.i..self.i + 4).ok_or("bad \\u")?)
                                        .map_err(|e| e.to_string())?;
                                    let code = u32::from_str_radix(hex, 16).map_err(|e| e.to_string())?;
                                    out.push(char::from_u32(code).unwrap_or('?'));
                                    self.i += 4;
                                }
                                b'n' => out.push('\n'),
                                b't' => out.push('\t'),
                                other => out.push(char::from(other)),
                            }
                        }
                        other => out.push(char::from(other)),
                    }
                }
                Err("unterminated string".into())
            }
            b't' if self.b[self.i..].starts_with(b"true") => {
                self.i += 4;
                Ok(Json::Bool(true))
            }
            b'f' if self.b[self.i..].starts_with(b"false") => {
                self.i += 5;
                Ok(Json::Bool(false))
            }
            b'n' if self.b[self.i..].starts_with(b"null") => {
                self.i += 4;
                Ok(Json::Null)
            }
            _ => {
                let start = self.i;
                while self.i < self.b.len() && matches!(self.b[self.i], b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9') {
                    self.i += 1;
                }
                if start == self.i {
                    return Err(format!("unexpected byte at {start}"));
                }
                Ok(Json::Number(String::from_utf8_lossy(&self.b[start..self.i]).into_owned()))
            }
        }
    }
}

/// Parses one line.
pub fn parse(line: &str) -> Result<Json, String> {
    let mut p = Parser { b: line.as_bytes(), i: 0 };
    let v = p.value()?;
    p.ws();
    if p.i != p.b.len() {
        return Err(format!("text after the value at {}", p.i));
    }
    Ok(v)
}

/// Every file named `name` under `dir`, sorted.
pub fn find_files(dir: &Path, name: &str) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(entries) = fs::read_dir(&d) else {
            continue;
        };
        for entry in entries.flatten() {
            let p = entry.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.file_name().is_some_and(|n| n == name) {
                out.push(p);
            }
        }
    }
    out.sort();
    out
}

/// The parsed lines of a `.jsonl` file (blank lines skipped).
pub fn read_lines(path: &Path) -> Vec<Json> {
    let text = fs::read_to_string(path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    text.lines()
        .enumerate()
        .filter(|(_, l)| !l.trim().is_empty())
        .map(|(n, l)| parse(l).unwrap_or_else(|e| panic!("{}:{}: {e}", path.display(), n + 1)))
        .collect()
}

/// `OUTPOST_ORACLE_DIR`, or `None` with the notice printed.
pub fn oracle_dir(what: &str) -> Option<PathBuf> {
    match std::env::var_os("OUTPOST_ORACLE_DIR") {
        Some(d) => Some(PathBuf::from(d)),
        None => {
            println!("OUTPOST_ORACLE_DIR is not set: no {what} were replayed");
            None
        }
    }
}

/// Whether two floats agree within `tolerance` relative to the larger
/// magnitude (and at least absolute).
pub fn close(a: f32, b: f32, tolerance: f32) -> bool {
    if a == b {
        return true;
    }
    let scale = a.abs().max(b.abs()).max(1.0);
    (f64::from(a) - f64::from(b)).abs() <= f64::from(tolerance) * f64::from(scale)
}
