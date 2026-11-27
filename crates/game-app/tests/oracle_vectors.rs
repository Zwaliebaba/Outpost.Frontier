//! The committed oracle vectors of this crate (`tests/vectors/`), rendered
//! through the port. `Oracle.exe game-app` writes the same files from the
//! C++ reference (`CppSource/Oracle/oracle_account.cpp`,
//! `oracle_perf_options.cpp`) over `tests/fixtures/oracle/`.

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use game_app::account_keys::{self, AccountKeys};
use game_app::perf_options::{PerfDisplay, PerfOptions};
use neuron_client::user_state::{MachineIdentity, ProductIdKey};

#[path = "support/effect_tables.rs"]
mod effect_tables;

fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/oracle")
}

/// The non-comment lines of an input list.
fn input_lines(relative: &str) -> Vec<String> {
    let path = fixtures().join(relative);
    let text = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    text.lines()
        .filter(|l| !l.starts_with('#'))
        .map(str::to_owned)
        .collect()
}

/// A hex field; `.` is no bytes.
fn unhex(text: &str) -> Vec<u8> {
    if text == "." {
        return Vec::new();
    }
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap_or_else(|e| panic!("{text}: {e}")))
        .collect()
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

/// Compares `lines` with the committed suite `tests/vectors/<name>`, or
/// writes it when `OUTPOST_BLESS_VECTORS` is set.
fn check_suite(name: &str, lines: &[String]) {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let rendered: String = lines.iter().map(|l| format!("{l}\n")).collect();
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        fs::write(&path, rendered).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        return;
    }
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        assert_eq!(*want, got, "{name} line {}", index + 1);
    }
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
}

/// The oracle's scripted machine: a key that does not open (`None`), a
/// missing value (`Some(None)`) or a value; a value larger than the claimed
/// size fails and leaves the size it needs, as `RegQueryValueExA` does.
#[derive(Default)]
struct Scripted {
    nine_x: Option<Option<Vec<u8>>>,
    nt: Option<Option<Vec<u8>>>,
    uuid: [u8; 16],
    user: Option<Vec<u8>>,
    ms: u32,
}

fn product_field(field: &str) -> Option<Option<Vec<u8>>> {
    match field {
        "nokey" => None,
        "none" => Some(None),
        hex => Some(Some(unhex(hex))),
    }
}

impl MachineIdentity for Scripted {
    fn product_id(&mut self, key: ProductIdKey, size: &mut u32) -> Option<Option<Vec<u8>>> {
        let value = match key {
            ProductIdKey::Windows9x => self.nine_x.clone()?,
            ProductIdKey::WindowsNt => self.nt.clone()?,
        };
        let Some(value) = value else {
            return Some(None);
        };
        let need = u32::try_from(value.len()).unwrap();
        if *size < need {
            *size = need;
            return Some(None);
        }
        *size = need;
        Some(Some(value))
    }
    fn uuid(&mut self) -> [u8; 16] {
        self.uuid
    }
    fn user_name(&mut self) -> Option<Vec<u8>> {
        self.user.clone()
    }
    fn milliseconds(&mut self) -> u32 {
        self.ms
    }
}

fn account_line(line: &str) -> String {
    let f: Vec<&str> = line.split(' ').collect();
    let head = format!("{{\"in\":\"{line}\"");
    match (f[0], f.len()) {
        ("seed", 7) => {
            let mut uuid = [0u8; 16];
            let bytes = unhex(f[3]);
            uuid[..bytes.len().min(16)].copy_from_slice(&bytes[..bytes.len().min(16)]);
            let mut machine = Scripted {
                nine_x: product_field(f[1]),
                nt: product_field(f[2]),
                uuid,
                user: (f[4] != "-").then(|| unhex(f[4])),
                ms: f[5].parse().unwrap(),
            };
            let name = (f[6] != "-").then(|| unhex(f[6]));
            let account = account_keys::generate_account(&mut machine, name.as_deref());
            let key = account_keys::generate_text_key(&mut machine);
            format!(
                "{head},\"name\":\"{}\",\"sig\":\"{}\",\"key\":\"{key}\"}}",
                account.name.unwrap(),
                account.signature.unwrap()
            )
        }
        ("setkey", 2) => {
            let key = String::from_utf8(unhex(f[1])).unwrap();
            match AccountKeys::from_text_key(&key) {
                None => format!("{head},\"ok\":\"0\"}}"),
                Some(keys) => format!(
                    "{head},\"ok\":\"1\",\"install\":\"{}\",\"name\":\"{}\",\"sig\":\"{}\"}}",
                    hex(keys.install_key.unwrap().as_bytes()),
                    keys.name.unwrap(),
                    keys.signature.unwrap()
                ),
            }
        }
        ("getkey", 2) => {
            let keys = AccountKeys {
                install_key: (f[1] != "nokey").then(|| String::from_utf8(unhex(f[1])).unwrap()),
                ..AccountKeys::default()
            };
            match keys.text_key() {
                None => format!("{head},\"ok\":\"0\"}}"),
                Some(key) => format!("{head},\"ok\":\"1\",\"key\":\"{}\"}}", hex(key.as_bytes())),
            }
        }
        ("load", 4) => {
            let value =
                |field: &str| (field != "-").then(|| String::from_utf8(unhex(field)).unwrap());
            let keys = AccountKeys {
                install_key: None,
                name: value(f[1]),
                signature: value(f[2]),
            };
            format!(
                "{head},\"verify\":\"{}\",\"server\":\"{}\"}}",
                u8::from(keys.verify()),
                keys.server_signature(&unhex(f[3]))
            )
        }
        _ => panic!("account.list: bad line {line}"),
    }
}

fn perf_line(line: &str) -> String {
    let (files, rest) = line.split_once(' ').unwrap();
    let rest: Vec<&str> = rest.split(' ').collect();
    let mut options = PerfOptions::default();
    let mut display = PerfDisplay {
        width: 0,
        height: 0,
        color_bpp: -1,
        depth_bpp: -1,
    };
    let loads: Vec<&str> = files
        .split(',')
        .map(|file| {
            let contents = fs::read(fixtures().join("perf").join(file)).ok();
            let block = (rest[0] == "d").then_some(&mut display);
            if options.load(contents, block).is_some() {
                "1"
            } else {
                "0"
            }
        })
        .collect();
    let field = |i: usize| rest[i].parse::<i32>().unwrap();
    let saved = options.save(&PerfDisplay {
        width: field(1),
        height: field(2),
        color_bpp: field(3),
        depth_bpp: field(4),
    });
    let bits = |applied: bool| {
        options
            .options
            .iter()
            .map(|o| {
                format!(
                    "{:08x}",
                    if applied { o.applied } else { o.value }.to_bits()
                )
            })
            .collect::<Vec<_>>()
            .join(",")
    };
    format!(
        "{{\"in\":\"{line}\",\"loads\":\"{}\",\"display\":\"{},{},{},{}\",\"skip\":\"{}\",\"values\":\"{}\",\"applied\":\"{}\",\"save\":\"{}\"}}",
        loads.join(","),
        display.width,
        display.height,
        display.color_bpp,
        display.depth_bpp,
        u8::from(options.skip_machine_warnings),
        bits(false),
        bits(true),
        hex(&saved)
    )
}

/// P4.1: `LoadPerfOptions` over the files of a line in turn, then
/// `SavePerfOptions` (`tests/fixtures/oracle/perf_options.list`, the files
/// under `perf/`).
#[test]
fn perf_options() {
    let lines: Vec<String> = input_lines("perf_options.list")
        .iter()
        .map(|l| perf_line(l))
        .collect();
    check_suite("perf_options.jsonl", &lines);
}

/// P4.1: `GenerateAccount` and `GenerateTextKey` over a scripted machine,
/// `SetTextKey`, `GetTextKey`, and `LoadAccount` with `VerifyAccount` and
/// `GetServerSignature` (`tests/fixtures/oracle/account.list`).
#[test]
fn account() {
    let lines: Vec<String> = input_lines("account.list")
        .iter()
        .map(|l| account_line(l))
        .collect();
    check_suite("account.jsonl", &lines);
}

#[test]
fn effect_arch() {
    use game_app::effect_arch::EffectArchDb;
    let source = neuron_core::vfs::Host::new(fixtures());
    let mut out = Vec::new();
    let mut case: Option<String> = None;
    let mut db = EffectArchDb::default();
    for line in input_lines("effect_arch.list") {
        if let Some(name) = line.strip_prefix("case ") {
            if let Some(c) = &case {
                effect_tables::render(c, &db, &mut out);
            }
            case = Some(name.to_owned());
            db = EffectArchDb::default();
        } else if let Some(file) = line.strip_prefix("ini ") {
            let grown = db.load_ini(&source, file.as_bytes(), &|_| false);
            out.push(format!(
                "{{\"case\":\"{}\",\"ini\":\"{file}\",\"r\":\"{grown}\"}}",
                case.as_deref().unwrap_or("")
            ));
        } else {
            panic!("effect_arch.list: bad line {line}");
        }
    }
    if let Some(c) = &case {
        effect_tables::render(c, &db, &mut out);
    }
    check_suite("effect_arch.jsonl", &out);
}
