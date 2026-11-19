//! `cargo xtask shaders`: compile the renderer's shaders with `dxc`
//! (decision record 0004, section 3, as amended on 2026-11-19: DXIL of
//! shader model 6.1) into the committed blobs, or check that the committed
//! blobs are what the sources compile to.
//!
//! The manifest is `crates/neuron-client/shaders/shaders.toml`: one
//! `[[shader]]` table per blob with `file`, `entry`, `profile`, `output`
//! and optional `defines`. The compiler is `dxc.exe` from the
//! `Microsoft.Direct3D.DXC` NuGet package (`--dxc <path>`, or the
//! `OUTPOST_DXC` environment variable), with `dxcompiler.dll` and
//! `dxil.dll` (the validator that signs the blobs) beside it; the
//! compiler DLL must have the file version `shaders/README.md` records, so
//! a blob changes only when its source does. The flags are fixed.
//!
//! `--check` compiles to a temporary directory and compares. On a
//! difference it prints each new blob in base64 between marker lines, so
//! a blob compiled on the Windows CI runner can be committed from a host
//! without the compiler.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

use toml::{Table, Value};

pub const USAGE: &str = "  shaders [--check] [--dxc <path>]
                 compile crates/neuron-client/shaders/shaders.toml with dxc
                 into crates/neuron-client/compiled_shaders (Windows only);
                 --check fails when a committed blob differs and prints the
                 new blobs in base64
";

/// `dxc`'s flags (record 0004, point 17 as amended), before `-T`, `-E`,
/// `-D` and `-Fo`.
const FLAGS: &[&str] = &[
    "-nologo",
    "-WX",
    "-O3",
    "-Ges",
    "-Gis",
    "-Zpr",
    "-HV",
    "2021",
    "-Qstrip_debug",
    "-Qstrip_reflect",
    "-Qstrip_priv",
];

/// The environment variable that names `dxc.exe` when `--dxc` does not.
const DXC_VARIABLE: &str = "OUTPOST_DXC";

/// The line of `shaders/README.md` that records the compiler DLL version.
const VERSION_PREFIX: &str = "dxcompiler.dll file version: ";

struct Shader {
    file: String,
    entry: String,
    profile: String,
    output: String,
    defines: Vec<String>,
}

fn string(t: &Table, key: &str) -> Result<String, String> {
    match t.get(key) {
        Some(Value::String(s)) => Ok(s.clone()),
        _ => Err(format!("a [[shader]] has no string `{key}`")),
    }
}

fn manifest(text: &str) -> Result<Vec<Shader>, String> {
    let table: Table = text.parse().map_err(|e: toml::de::Error| e.to_string())?;
    let Some(Value::Array(entries)) = table.get("shader") else {
        return Err("no [[shader]] entries".into());
    };
    entries
        .iter()
        .map(|entry| {
            let Value::Table(t) = entry else {
                return Err("a [[shader]] entry is not a table".into());
            };
            let defines = match t.get("defines") {
                None => Vec::new(),
                Some(Value::Array(a)) => a
                    .iter()
                    .map(|d| match d {
                        Value::String(s) => Ok(s.clone()),
                        _ => Err("`defines` holds a non-string".to_string()),
                    })
                    .collect::<Result<_, _>>()?,
                Some(_) => return Err("`defines` is not an array".into()),
            };
            Ok(Shader {
                file: string(t, "file")?,
                entry: string(t, "entry")?,
                profile: string(t, "profile")?,
                output: string(t, "output")?,
                defines,
            })
        })
        .collect()
}

/// The `VS_FIXEDFILEINFO` file version of a PE file, as `a.b.c.d`.
fn file_version(pe: &[u8]) -> Option<String> {
    const SIGNATURE: [u8; 4] = 0xfeef_04bd_u32.to_le_bytes();
    let at = pe.windows(4).position(|w| w == SIGNATURE)?;
    let word = |o: usize| -> Option<u32> {
        Some(u32::from_le_bytes(
            pe.get(at + o..at + o + 4)?.try_into().ok()?,
        ))
    };
    let (ms, ls) = (word(8)?, word(12)?);
    Some(format!(
        "{}.{}.{}.{}",
        ms >> 16,
        ms & 0xffff,
        ls >> 16,
        ls & 0xffff
    ))
}

/// The version `shaders/README.md` records, if it records one.
fn recorded_version(readme: &str) -> Option<String> {
    readme
        .lines()
        .find_map(|l| l.trim().strip_prefix(VERSION_PREFIX))
        .map(|v| v.trim().trim_matches('`').to_string())
        .filter(|v| !v.is_empty() && v != "unrecorded")
}

fn base64(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let b = [
            chunk[0],
            chunk.get(1).copied().unwrap_or(0),
            chunk.get(2).copied().unwrap_or(0),
        ];
        let n = (u32::from(b[0]) << 16) | (u32::from(b[1]) << 8) | u32::from(b[2]);
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(ALPHABET[((n >> (18 - 6 * i)) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

fn compile(dxc: &Path, source_dir: &Path, shader: &Shader, out: &Path) -> Result<(), String> {
    let mut command = Command::new(dxc);
    command.args(FLAGS);
    command.args(["-T", &shader.profile, "-E", &shader.entry]);
    for define in &shader.defines {
        command.args(["-D", define]);
    }
    command
        .arg("-Fo")
        .arg(out)
        .arg(source_dir.join(&shader.file));
    let output = command
        .output()
        .map_err(|e| format!("cannot run {}: {e}", dxc.display()))?;
    if output.status.success() {
        Ok(())
    } else {
        Err(format!(
            "dxc failed for {} ({}):\n{}{}",
            shader.output,
            shader.entry,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ))
    }
}

pub fn run(args: &[String], root: &Path) -> ExitCode {
    match run_inner(args, root) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("shaders: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run_inner(args: &[String], root: &Path) -> Result<ExitCode, String> {
    let mut check = false;
    let mut dxc: Option<PathBuf> = std::env::var_os(DXC_VARIABLE).map(PathBuf::from);
    let mut it = args.iter();
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--check" => check = true,
            "--dxc" => dxc = Some(it.next().ok_or("--dxc needs a path")?.into()),
            other => return Err(format!("unknown argument {other}\n{USAGE}")),
        }
    }
    let dxc = dxc.ok_or_else(|| {
        format!(
            "no dxc: pass --dxc <path to dxc.exe> or set {DXC_VARIABLE} (shaders/README.md says \
             where the pinned compiler comes from)"
        )
    })?;
    let crate_dir = root.join("crates/neuron-client");
    let source_dir = crate_dir.join("shaders");
    let blob_dir = crate_dir.join("compiled_shaders");
    let read = |p: &Path| std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()));
    let shaders = manifest(&String::from_utf8_lossy(&read(
        &source_dir.join("shaders.toml"),
    )?))?;
    let readme = String::from_utf8_lossy(&read(&source_dir.join("README.md"))?).into_owned();

    let validator = dxc.with_file_name("dxil.dll");
    if !validator.is_file() {
        return Err(format!(
            "{} is missing: without the validator dxc writes unsigned blobs",
            validator.display()
        ));
    }
    let dll = dxc.with_file_name("dxcompiler.dll");
    let found = file_version(&read(&dll)?)
        .ok_or_else(|| format!("{} has no file version", dll.display()))?;
    let version_ok = match recorded_version(&readme) {
        Some(recorded) if recorded == found => true,
        Some(recorded) => {
            return Err(format!(
                "{} is version {found}; shaders/README.md records {recorded}",
                dll.display()
            ));
        }
        None => {
            eprintln!("shaders: README.md records no compiler version; found {found}");
            false
        }
    };

    let out_dir = if check {
        let dir = root.join("target/xtask-shaders");
        std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        dir
    } else {
        std::fs::create_dir_all(&blob_dir).map_err(|e| format!("{}: {e}", blob_dir.display()))?;
        blob_dir.clone()
    };
    let mut differ = Vec::new();
    for shader in &shaders {
        let out = out_dir.join(&shader.output);
        compile(&dxc, &source_dir, shader, &out)?;
        if check {
            let new = read(&out)?;
            let committed = std::fs::read(blob_dir.join(&shader.output)).ok();
            if committed.as_deref() != Some(&new[..]) {
                differ.push((shader.output.clone(), new));
            }
        }
    }
    if !check {
        println!(
            "shaders: compiled {} blobs with dxcompiler {found}",
            shaders.len()
        );
        return Ok(if version_ok {
            ExitCode::SUCCESS
        } else {
            ExitCode::FAILURE
        });
    }
    for (name, blob) in &differ {
        let mut text = String::new();
        let _ = writeln!(text, "----- begin {name} ({} bytes) -----", blob.len());
        for line in base64(blob).as_bytes().chunks(76) {
            let _ = writeln!(text, "{}", String::from_utf8_lossy(line));
        }
        let _ = writeln!(text, "----- end {name} -----");
        print!("{text}");
    }
    if differ.is_empty() && version_ok {
        println!("shaders: {} blobs match their sources", shaders.len());
        Ok(ExitCode::SUCCESS)
    } else {
        eprintln!(
            "shaders: {} of {} blobs differ from their sources",
            differ.len(),
            shaders.len()
        );
        Ok(ExitCode::FAILURE)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_pads_like_rfc_4648() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn file_versions_come_from_the_fixed_info() {
        let mut pe = vec![0u8; 8];
        pe.extend_from_slice(&0xfeef_04bd_u32.to_le_bytes());
        pe.extend_from_slice(&0x0001_0000_u32.to_le_bytes());
        pe.extend_from_slice(&0x000a_0000_u32.to_le_bytes()); // 10.0
        pe.extend_from_slice(&0x6604_0001_u32.to_le_bytes()); // 26116.1
        assert_eq!(file_version(&pe).as_deref(), Some("10.0.26116.1"));
        assert_eq!(file_version(b"no info"), None);
    }

    #[test]
    fn the_readme_records_a_version_or_not() {
        assert_eq!(
            recorded_version("x\ndxcompiler.dll file version: `1.9.2609.5`\n").as_deref(),
            Some("1.9.2609.5")
        );
        assert_eq!(
            recorded_version("dxcompiler.dll file version: unrecorded"),
            None
        );
    }

    #[test]
    fn the_committed_manifest_parses() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
        let text = std::fs::read_to_string(root.join("crates/neuron-client/shaders/shaders.toml"))
            .unwrap();
        let shaders = manifest(&text).unwrap();
        assert!(shaders.iter().any(|s| s.entry == "vs_main"));
        assert!(
            shaders
                .iter()
                .all(|s| s.profile.ends_with("_6_1") && s.output.ends_with(".cso"))
        );
    }
}
