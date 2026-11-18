//! P4.12: the DirectWrite font backend against GDI on the same machine.
//!
//! `Oracle.exe gdi-fonts` (`CppSource/Oracle/oracle_gdi_fonts.cpp`) records
//! GDI's text metrics, advance widths and pangram extent for the faces of
//! `tests/fixtures/oracle/gdi_fonts.list` in `tests/vectors/gdi_fonts.jsonl`
//! on CI's Windows runner. These tests compare them with the DirectWrite
//! backend's, and run the font autoscale search of `fonts.ini` over both.
//! Without the vector file they print a notice and return.

#![cfg(windows)]

use std::cell::Cell;
use std::collections::HashMap;
use std::path::Path;
use std::rc::Rc;

use neuron_client::platform::dwrite::DWriteBackend;
use neuron_client::text::font::config::read_fonts_ini;
use neuron_client::text::font::face::PANGRAM;
use neuron_client::text::font::{
    FaceHandle, FontBackend, FontEnv, FontSystem, LogFont, TextMetrics,
};

/// Advances may differ by this many pixels, on at most `WIDTH_SHARE` of
/// the characters of a face; metrics by `METRIC_TOLERANCE`; the pangram by
/// the larger of 2 pixels and 2 %; autoscaled heights by 1 pixel.
const METRIC_TOLERANCE: i32 = 1;
const WIDTH_TOLERANCE: i32 = 1;
const WIDTH_SHARE: f64 = 0.05;

#[derive(Clone, Debug)]
struct GdiRecord {
    face: String,
    height: i32,
    weight: i32,
    italic: bool,
    text_face: String,
    tm: Vec<i32>,
    widths: Vec<i32>,
    pangram: (i32, i32),
}

fn field<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let pattern = format!("\"{key}\":\"");
    let start = line.find(&pattern)? + pattern.len();
    let end = line[start..].find('"')? + start;
    Some(&line[start..end])
}

fn numbers(text: &str) -> Vec<i32> {
    text.split(' ').filter_map(|n| n.parse().ok()).collect()
}

fn records() -> Option<Vec<GdiRecord>> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/vectors/gdi_fonts.jsonl");
    let Ok(text) = std::fs::read_to_string(&path) else {
        println!(
            "{} is not committed yet; nothing to compare (see CppSource/Oracle/oracle_gdi_fonts.cpp)",
            path.display()
        );
        return None;
    };
    let mut out = Vec::new();
    for line in text.lines() {
        if field(line, "error").is_some() {
            continue;
        }
        let pangram = numbers(field(line, "pangram")?);
        out.push(GdiRecord {
            face: field(line, "face")?.to_owned(),
            height: field(line, "height")?.parse().ok()?,
            weight: field(line, "weight")?.parse().ok()?,
            italic: field(line, "italic")? == "1",
            text_face: field(line, "text_face")?.to_owned(),
            tm: numbers(field(line, "tm")?),
            widths: numbers(field(line, "widths")?),
            pangram: (*pangram.first()?, *pangram.get(1)?),
        });
    }
    Some(out)
}

fn directwrite() -> Option<DWriteBackend> {
    match DWriteBackend::new() {
        Ok(b) => Some(b),
        Err(e) => {
            println!("DirectWrite is not available here ({e}); nothing to compare");
            None
        }
    }
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().collect()
}

/// GDI's own pangram extent is the sum of its advance widths (the model
/// measures strings that way).
fn ascii_sum(widths: &[i32], text: &str) -> i32 {
    text.bytes()
        .map(|b| widths[usize::from(b - 0x20)])
        .sum::<i32>()
}

#[test]
fn metrics_match_gdi() {
    let Some(records) = records() else {
        return;
    };
    let Some(mut dw) = directwrite() else {
        return;
    };
    let mut failures = Vec::new();
    let mut compared = 0;
    for r in &records {
        if r.widths.len() != 95 || r.tm.len() != 7 {
            failures.push(format!("{} {}: malformed record", r.face, r.height));
            continue;
        }
        if ascii_sum(&r.widths, PANGRAM) != r.pangram.0 {
            failures.push(format!(
                "{} {}: GDI's pangram extent {} is not its summed advances {}",
                r.face,
                r.height,
                r.pangram.0,
                ascii_sum(&r.widths, PANGRAM)
            ));
        }
        let lf = LogFont::new(
            r.face.as_bytes(),
            r.height,
            r.weight >= 700,
            r.italic,
            false,
        );
        let Some(face) = dw.create_font(&lf) else {
            failures.push(format!("{} {}: DirectWrite made no face", r.face, r.height));
            continue;
        };
        // Compare only faces both have under the name asked for.
        if dw.substituted(face) || !r.text_face.eq_ignore_ascii_case(&r.face) {
            println!(
                "{} {}: GDI {:?}, DirectWrite {:?}{}; not compared",
                r.face,
                r.height,
                r.text_face,
                dw.family(face),
                if dw.substituted(face) {
                    " (substituted)"
                } else {
                    ""
                }
            );
            continue;
        }
        compared += 1;
        let tm: TextMetrics = dw.text_metrics(face);
        let ours = [tm.height, tm.ascent, tm.descent];
        for (name, (a, b)) in ["height", "ascent", "descent"]
            .iter()
            .zip(ours.iter().zip(&r.tm[..3]))
        {
            if (a - b).abs() > METRIC_TOLERANCE {
                failures.push(format!(
                    "{} {} w{} i{}: {name} {a}, GDI {b}",
                    r.face, r.height, r.weight, r.italic
                ));
            }
        }
        let mut differing = 0;
        for (i, &gdi) in r.widths.iter().enumerate() {
            let ch = 0x20 + i as u16;
            let ours = dw.char_width(face, ch).unwrap_or(-1);
            if ours != gdi {
                differing += 1;
                if (ours - gdi).abs() > WIDTH_TOLERANCE {
                    failures.push(format!(
                        "{} {}: U+{ch:04X} advance {ours}, GDI {gdi}",
                        r.face, r.height
                    ));
                }
            }
        }
        if f64::from(differing) > WIDTH_SHARE * r.widths.len() as f64 {
            failures.push(format!(
                "{} {}: {differing} of {} advances differ",
                r.face,
                r.height,
                r.widths.len()
            ));
        }
        let (cx, cy) = dw.text_extent(face, &wide(PANGRAM)).unwrap_or((-1, -1));
        let limit = 2.max(r.pangram.0 / 50);
        if (cx - r.pangram.0).abs() > limit || (cy - r.pangram.1).abs() > METRIC_TOLERANCE {
            failures.push(format!(
                "{} {}: pangram {cx} x {cy}, GDI {} x {}",
                r.face, r.height, r.pangram.0, r.pangram.1
            ));
        }
    }
    println!("{compared} faces compared");
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}

/// GDI's recorded answers as a backend: faces of weight 400, upright, at
/// the recorded heights; ASCII advances only.
struct RecordedGdi {
    by_face_height: HashMap<(String, i32), GdiRecord>,
    faces: Vec<GdiRecord>,
    missing: Rc<Cell<bool>>,
}

impl FontBackend for RecordedGdi {
    fn create_font(&mut self, lf: &LogFont) -> Option<FaceHandle> {
        let key = (
            String::from_utf8_lossy(lf.face()).to_ascii_lowercase(),
            lf.height,
        );
        let Some(r) = self.by_face_height.get(&key) else {
            self.missing.set(true);
            return None;
        };
        self.faces.push(r.clone());
        Some(FaceHandle(self.faces.len() as u32))
    }

    fn delete_font(&mut self, _face: FaceHandle) {}

    fn text_metrics(&mut self, face: FaceHandle) -> TextMetrics {
        let tm = &self.faces[face.0 as usize - 1].tm;
        TextMetrics {
            height: tm[0],
            ascent: tm[1],
            descent: tm[2],
            internal_leading: tm[3],
            external_leading: tm[4],
            ave_char_width: tm[5],
            max_char_width: tm[6],
        }
    }

    fn char_width(&mut self, face: FaceHandle, ch: u16) -> Option<i32> {
        let r = &self.faces[face.0 as usize - 1];
        (0x20..0x7F)
            .contains(&ch)
            .then(|| r.widths[usize::from(ch - 0x20)])
    }

    fn text_extent(&mut self, face: FaceHandle, text: &[u16]) -> Option<(i32, i32)> {
        let mut sum = 0;
        for &ch in text {
            sum += self.char_width(face, ch)?;
        }
        Some((sum, self.faces[face.0 as usize - 1].tm[0]))
    }

    fn draw_text(
        &mut self,
        _face: FaceHandle,
        _x: i32,
        _y: i32,
        _width: u32,
        _height: u32,
        _text: &[u16],
    ) -> Option<Vec<u8>> {
        None
    }
}

#[test]
fn autoscale_over_gdi_and_directwrite() {
    let Some(records) = records() else {
        return;
    };
    let Some(mut probe) = directwrite() else {
        return;
    };
    let ini = std::fs::read(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data/FONTS/fonts.ini"),
    )
    .expect("fonts.ini");
    let entries = read_fonts_ini(&ini);
    let by_face_height: HashMap<(String, i32), GdiRecord> = records
        .iter()
        .filter(|r| r.weight == 400 && !r.italic)
        .map(|r| ((r.face.to_ascii_lowercase(), r.height), r.clone()))
        .collect();
    let mut failures = Vec::new();
    let mut compared = 0;
    for (width, height) in [
        (800, 600),
        (1024, 768),
        (1280, 960),
        (1600, 1200),
        (1280, 720),
        (1920, 1080),
    ] {
        for entry in &entries {
            // Compare only where GDI and DirectWrite realise the same face.
            let lf = LogFont::new(&entry.font, 20, false, false, false);
            let Some(face) = probe.create_font(&lf) else {
                continue;
            };
            let name = String::from_utf8_lossy(&entry.font).to_ascii_lowercase();
            let gdi_face = by_face_height
                .get(&(name.clone(), 20))
                .map(|r| r.text_face.to_ascii_lowercase());
            if probe.substituted(face) || gdi_face.as_deref() != Some(name.as_str()) {
                continue;
            }
            let missing = Rc::new(Cell::new(false));
            let gdi = RecordedGdi {
                by_face_height: by_face_height.clone(),
                faces: Vec::new(),
                missing: Rc::clone(&missing),
            };
            let mut over_gdi = FontSystem::new(Box::new(gdi), FontEnv::new(width, height));
            over_gdi.load_fonts(std::slice::from_ref(entry));
            if missing.get() {
                println!(
                    "{} at {width} x {height}: a height GDI was not asked for; not compared",
                    String::from_utf8_lossy(&entry.nickname)
                );
                continue;
            }
            let Some(dw) = directwrite() else {
                return;
            };
            let mut over_dw = FontSystem::new(Box::new(dw), FontEnv::new(width, height));
            over_dw.load_fonts(std::slice::from_ref(entry));
            let (a, b) = (
                over_gdi.fonts[0].realized_height(),
                over_dw.fonts[0].realized_height(),
            );
            compared += 1;
            if (a - b).abs() > 1 {
                failures.push(format!(
                    "{} at {width} x {height}: GDI {a}, DirectWrite {b}",
                    String::from_utf8_lossy(&entry.nickname)
                ));
            } else if a != b {
                println!(
                    "{} at {width} x {height}: GDI {a}, DirectWrite {b}",
                    String::from_utf8_lossy(&entry.nickname)
                );
            }
        }
    }
    println!("{compared} autoscaled fonts compared");
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}
