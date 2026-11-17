//! Oracle vectors for work package P1.5: rectangles, the Catmull-Rom
//! segment, the MD5 text form, the frame timer and the `ErrorHandler` log
//! channels. The inputs are under `tests/fixtures/oracle/util/`, written by
//! `make_util_fixtures.py`; the C++ side is `CppSource/Oracle/oracle_util*.cpp`.
//!
//! Every suite but `spline.jsonl` is Tier A and compared as text. The spline
//! is designed to match the reference bit for bit at 53-bit precision, but
//! is compared within Tier B bounds (see [`spline`]) so that the committed
//! file can be the oracle's output whatever its last bits are.

use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::Path;
use std::sync::{Arc, Mutex, PoisonError};

mod support;

use support::{check_suite, hex, input_lines, read_fixture, unhex};

use neuron_core::diag::DumpFlags;
use neuron_core::diag::channels::{LogChannels, Sink};
use neuron_core::ini::IniReader;
use neuron_core::time::{Delta, ManualClock, Timing};
use neuron_core::util::md5::{Md5Hash, Md5Words, md5_text};
use neuron_core::util::rect::{Rect, ViewRect, WinRect};
use neuron_core::util::spline::CrSplineSegment;

fn ints(line: &str) -> Vec<i64> {
    line.split(' ')
        .map(|v| v.parse().unwrap_or_else(|e| panic!("{line}: {e}")))
        .collect()
}

fn i32_at(values: &[i64], index: usize) -> i32 {
    i32::try_from(values[index]).unwrap_or_else(|e| panic!("{values:?}: {e}"))
}

fn rect_text(r: Rect) -> String {
    format!("{},{},{},{}", r.left, r.top, r.width, r.height)
}

fn edges_text(left: i32, top: i32, right: i32, bottom: i32) -> String {
    format!("{left},{top},{right},{bottom}")
}

#[test]
fn rect() {
    let lines: Vec<String> = input_lines("util/rect.list")
        .iter()
        .map(|line| {
            let v = ints(line);
            let a = Rect::new(i32_at(&v, 0), i32_at(&v, 1), i32_at(&v, 2), i32_at(&v, 3));
            let b = Rect::new(i32_at(&v, 4), i32_at(&v, 5), i32_at(&v, 6), i32_at(&v, 7));
            let (x, y) = (i32_at(&v, 8), i32_at(&v, 9));
            let (cx, cy) = a.center();
            let view = a.to_view();
            let win = a.to_win();
            let from_view = Rect::from_view(ViewRect {
                left: b.left,
                top: b.top,
                right: b.width,
                bottom: b.height,
            });
            let from_win = Rect::from_win(WinRect {
                left: b.left,
                top: b.top,
                right: b.width,
                bottom: b.height,
            });
            let ltrb = Rect::from_ltrb(b.left, b.top, b.width, b.height);
            format!(
                "{{\"a\":\"{}\",\"b\":\"{}\",\"p\":\"{x},{y}\",\"right\":\"{}\",\"bottom\":\"{}\",\"area\":\"{}\",\"null\":\"{}\",\"nonnull\":\"{}\",\"center\":\"{cx},{cy}\",\"inside\":\"{}\",\"eq\":\"{}\",\"ne\":\"{}\",\"view\":\"{}\",\"win\":\"{}\",\"from_view\":\"{}\",\"from_win\":\"{}\",\"ltrb\":\"{}\",\"isect\":\"{}\",\"union\":\"{}\"}}",
                rect_text(a),
                rect_text(b),
                a.rightmost(),
                a.bottommost(),
                a.area(),
                u8::from(a.is_null()),
                u8::from(a.is_nonnull()),
                u8::from(a.contains(x, y)),
                u8::from(a.same_as(&b)),
                u8::from(a.differs_from(&b)),
                edges_text(view.left, view.top, view.right, view.bottom),
                edges_text(win.left, win.top, win.right, win.bottom),
                rect_text(from_view),
                rect_text(from_win),
                rect_text(ltrb),
                rect_text(a.intersect(&b)),
                rect_text(a.union(&b)),
            )
        })
        .collect();
    check_suite("rect.jsonl", &lines);
}

fn f32_hex(text: &str) -> f32 {
    f32::from_bits(u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}")))
}

fn vec_text(v: [f32; 3]) -> String {
    format!(
        "{:08x},{:08x},{:08x}",
        v[0].to_bits(),
        v[1].to_bits(),
        v[2].to_bits()
    )
}

/// The `"key":"value"` pairs of a vector line.
fn fields(line: &str) -> BTreeMap<String, String> {
    line.trim_matches(|c| c == '{' || c == '}')
        .split("\",\"")
        .filter_map(|pair| {
            let (key, value) = pair.split_once("\":\"")?;
            Some((
                key.trim_matches('"').to_owned(),
                value.trim_matches('"').to_owned(),
            ))
        })
        .collect()
}

fn floats(value: &str) -> Vec<f32> {
    value.split(',').map(f32_hex).collect()
}

/// Within `bound` relative to the reference, or `floor` absolute.
fn close(got: f32, want: f32, bound: f32, floor: f32) -> bool {
    got.to_bits() == want.to_bits() || (got - want).abs() <= (want.abs() * bound).max(floor)
}

/// Tier B: point, tangent and lengths within 1e-5 relative (1e-6 absolute
/// near zero); the parameter search within one step, since a last-bit
/// difference in a distance can stop it one step earlier or later.
#[test]
fn spline() {
    let inputs = input_lines("util/spline.list");
    let mut lines = Vec::new();
    let mut steps = Vec::new();
    for line in &inputs {
        let parts: Vec<&str> = line.split(' ').collect();
        let p: Vec<f32> = parts[..12].iter().map(|v| f32_hex(v)).collect();
        let mut segment = CrSplineSegment::new(
            [p[0], p[1], p[2]],
            [p[3], p[4], p[5]],
            [p[6], p[7], p[8]],
            [p[9], p[10], p[11]],
        );
        segment.sample_count = parts[12].parse().unwrap_or_else(|e| panic!("{line}: {e}"));
        let t = f32_hex(parts[13]);
        let point = [f32_hex(parts[14]), f32_hex(parts[15]), f32_hex(parts[16])];
        let (start, step) = (f32_hex(parts[17]), f32_hex(parts[18]));
        let at = segment.point(t);
        let tangent = segment.tangent(t);
        let length = segment.spline_length();
        segment.calculate_arclength();
        let search = segment
            .t_from_point(point, start, step)
            .unwrap_or_else(|| panic!("{line}: the reference would not return"));
        steps.push(step);
        lines.push(format!(
            "{{\"in\":\"{line}\",\"pt\":\"{}\",\"tan\":\"{}\",\"arc\":\"{:08x}\",\"len\":\"{:08x}\",\"search\":\"{:08x}\"}}",
            vec_text(at),
            vec_text(tangent),
            segment.arc_length.to_bits(),
            length.to_bits(),
            search.to_bits(),
        ));
    }
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite("spline.jsonl", &lines);
        return;
    }
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/vectors/spline.jsonl");
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "spline.jsonl: line count");
    for (index, ((want, got), step)) in committed.iter().zip(&lines).zip(&steps).enumerate() {
        let (want, got) = (fields(want), fields(got));
        assert_eq!(want["in"], got["in"], "line {}", index + 1);
        for key in ["pt", "tan", "arc", "len"] {
            for (g, w) in floats(&got[key]).into_iter().zip(floats(&want[key])) {
                assert!(
                    close(g, w, 1e-5, 1e-6),
                    "line {} {key}: {g} vs {w}",
                    index + 1
                );
            }
        }
        let (g, w) = (f32_hex(&got["search"]), f32_hex(&want["search"]));
        assert!(
            close(g, w, 0.0, step * 1.0001 + 1e-6),
            "line {} search: {g} vs {w}",
            index + 1
        );
    }
}

#[test]
fn md5_text_form() {
    let lines: Vec<String> = input_lines("util/md5.hex")
        .iter()
        .map(|line| {
            let data = unhex(line);
            let text = md5_text(&data);
            let sig = md5_text(text.as_bytes());
            let chunked = |size: usize| {
                let mut hash = Md5Hash::new();
                for chunk in data.chunks(size) {
                    hash.add_data(chunk);
                }
                hash.calc_value().to_text()
            };
            format!(
                "{{\"in\":\"{line}\",\"text\":\"{text}\",\"sig\":\"{sig}\",\"c7\":\"{}\",\"c64\":\"{}\"}}",
                chunked(7),
                chunked(64)
            )
        })
        .collect();
    check_suite("md5_text.jsonl", &lines);
}

#[test]
fn md5_parse() {
    let lines: Vec<String> = input_lines("util/md5_parse.hex")
        .iter()
        .map(|line| {
            let text = Md5Words::parse(&unhex(line)).to_text();
            format!("{{\"in\":\"{line}\",\"text\":\"{text}\"}}")
        })
        .collect();
    check_suite("md5_parse.jsonl", &lines);
}

#[test]
fn timing_delta() {
    let lines: Vec<String> = input_lines("util/timing.list")
        .iter()
        .map(|line| {
            let v = ints(line);
            let clock = ManualClock::new(Some(v[0]));
            let mut timing = Timing::new(&clock);
            timing.init();
            let tick = |i: usize| u32::try_from(v[i]).unwrap_or_else(|e| panic!("{line}: {e}"));
            let mut delta =
                Delta::from_readings(v[1], v[2], tick(3), tick(4), v[5] != 0, v[6] != 0);
            let dt = delta.calc_dt(&timing);
            let seconds = timing.seconds(v[2]);
            format!(
                "{{\"in\":\"{line}\",\"dt\":\"{:016x}\",\"seconds\":\"{:016x}\"}}",
                dt.to_bits(),
                seconds.to_bits()
            )
        })
        .collect();
    check_suite("timing_delta.jsonl", &lines);
}

type Files = Arc<Mutex<BTreeMap<Vec<u8>, Vec<u8>>>>;

struct MemoryFile {
    name: Vec<u8>,
    files: Files,
}

impl Write for MemoryFile {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        let mut files = self.files.lock().unwrap_or_else(PoisonError::into_inner);
        files
            .entry(self.name.clone())
            .or_default()
            .extend_from_slice(buf);
        Ok(buf.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

fn text_arg(arg: &str) -> Vec<u8> {
    if arg == "-" { Vec::new() } else { unhex(arg) }
}

fn int_arg(arg: &str) -> i32 {
    arg.parse().unwrap_or_else(|e| panic!("{arg}: {e}"))
}

#[test]
fn diag_log() {
    let files = Files::default();
    let shared = Arc::clone(&files);
    let mut log = LogChannels::new(Box::new(move |name: &[u8]| {
        let mut map = shared.lock().unwrap_or_else(PoisonError::into_inner);
        map.insert(name.to_vec(), Vec::new());
        Some(Box::new(MemoryFile {
            name: name.to_vec(),
            files: Arc::clone(&shared),
        }) as Sink)
    }));
    let mut lines = Vec::new();
    for line in input_lines("util/diag.script") {
        let args: Vec<&str> = line.split(' ').collect();
        match args[0] {
            "ini" => {
                let path = Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("tests/fixtures/oracle/util")
                    .join(args[1]);
                let mut reader = IniReader::new();
                let open = path.exists()
                    && reader.open(
                        args[1].as_bytes(),
                        read_fixture(&format!("util/{}", args[1])),
                    );
                log.activate(open.then_some(&mut reader));
                lines.push(format!("{{\"op\":\"ini\",\"file\":\"{}\"}}", args[1]));
            }
            "new" => {
                let ok = log.new_log(
                    &text_arg(args[1]),
                    int_arg(args[2]),
                    int_arg(args[3]),
                    args[4] == "1",
                    args[5] == "1",
                );
                lines.push(format!(
                    "{{\"op\":\"new\",\"name\":\"{}\",\"ok\":\"{}\"}}",
                    hex(&text_arg(args[1])),
                    u8::from(ok)
                ));
            }
            "dump" => {
                let flags =
                    u32::from_str_radix(args[1], 16).unwrap_or_else(|e| panic!("{line}: {e}"));
                log.dump(DumpFlags::from_bits(flags), &text_arg(args[2]));
            }
            "comment" => log.log_comment(int_arg(args[1]), &text_arg(args[2])),
            "enable" => log.log_enable(int_arg(args[1])),
            "disable" => log.log_disable(int_arg(args[1])),
            "enabled" => {
                let enabled = log.is_log_enabled(int_arg(args[1]));
                lines.push(format!(
                    "{{\"op\":\"enabled\",\"id\":\"{}\",\"r\":\"{}\"}}",
                    args[1],
                    u8::from(enabled)
                ));
            }
            "close" => {
                log.deactivate();
                lines.push("{\"op\":\"close\"}".to_owned());
            }
            "read" => {
                let name = text_arg(args[1]);
                let data = files
                    .lock()
                    .unwrap_or_else(PoisonError::into_inner)
                    .remove(&name);
                lines.push(match data {
                    Some(data) => format!(
                        "{{\"op\":\"read\",\"name\":\"{}\",\"data\":\"{}\"}}",
                        args[1],
                        hex(&data)
                    ),
                    None => format!(
                        "{{\"op\":\"read\",\"name\":\"{}\",\"missing\":\"1\"}}",
                        args[1]
                    ),
                });
            }
            other => panic!("unknown script command {other}"),
        }
    }
    check_suite("diag_log.jsonl", &lines);
}
