//! Source scan for legacy platform APIs (design/migration-plan.md section 4)
//! and for the placement of `allow(unsafe_code)` (design/decisions.md T14).

use std::fs;
use std::path::Path;

use crate::graph::Finding;
use crate::lexer::{is_ident, strip_rust_comments, strip_toml_comments};
use crate::workspace::Workspace;

/// Directories, relative to the workspace root, in which a module may
/// allow unsafe code. The work package that adds an FFI module adds its
/// directory here and says so in its pull request.
const UNSAFE_ALLOWED: &[&str] = &["crates/neuron-client/src/platform/"];

struct Rule {
    needle: &'static str,
    ignore_case: bool,
    /// The match must not continue an identifier on its left.
    word_start: bool,
    reason: &'static str,
}

const fn api(needle: &'static str, reason: &'static str) -> Rule {
    Rule {
        needle,
        ignore_case: false,
        word_start: false,
        reason,
    }
}

const fn library(needle: &'static str, reason: &'static str) -> Rule {
    Rule {
        needle,
        ignore_case: true,
        word_start: false,
        reason,
    }
}

const fn word(needle: &'static str, reason: &'static str) -> Rule {
    Rule {
        needle,
        ignore_case: false,
        word_start: true,
        reason,
    }
}

const D3D: &str = "Direct3D 8/9 and D3DX are replaced by Direct3D 12";
const DDRAW: &str = "DirectDraw is removed";
const DPLAY: &str = "DirectPlay is replaced by QUIC";
const DSOUND: &str = "DirectSound and EAX are replaced by XAudio2";
const ACM: &str = "the Audio Compression Manager is replaced by symphonia";
const DINPUT: &str = "DirectInput is replaced by Raw Input";
const DSHOW: &str =
    "DirectShow, Windows Media and Video for Windows are replaced by Media Foundation";
const WINMM: &str = "winmm timers are replaced by std::time::Instant";
const GDI_TEXT: &str = "GDI text is replaced by DirectWrite";
const IMM: &str = "private IMM32 calls are replaced by documented IME positioning";
const ASM: &str = "inline assembly is not used (decision T11)";

/// Rules for Rust and TOML text with comments removed.
const LEGACY: &[Rule] = &[
    api("Direct3DCreate8", D3D),
    api("Direct3DCreate9", D3D),
    api("IDirect3D8", D3D),
    api("IDirect3D9", D3D),
    api("IDirect3DDevice8", D3D),
    api("IDirect3DDevice9", D3D),
    api("D3DX", D3D),
    api("Win32_Graphics_Direct3D9", D3D),
    library("\"d3d8", D3D),
    library("\"d3d9", D3D),
    library("\"d3dx", D3D),
    api("DirectDrawCreate", DDRAW),
    api("IDirectDraw", DDRAW),
    api("Win32_Graphics_DirectDraw", DDRAW),
    library("\"ddraw", DDRAW),
    api("DirectPlay", DPLAY),
    library("dpnet", DPLAY),
    api("DirectSound", DSOUND),
    api("IKsPropertySet", DSOUND),
    library("\"dsound", DSOUND),
    api("acmStream", ACM),
    api("acmDriver", ACM),
    library("\"msacm32", ACM),
    api("DirectInput", DINPUT),
    library("\"dinput", DINPUT),
    api("Win32_Media_DirectShow", DSHOW),
    api("IGraphBuilder", DSHOW),
    api("IMediaControl", DSHOW),
    api("IWMReader", DSHOW),
    api("MCIWnd", DSHOW),
    library("\"quartz", DSHOW),
    library("\"msvfw32", DSHOW),
    api("timeGetTime", WINMM),
    api("ExtTextOut", GDI_TEXT),
    api("CreateFontIndirect", GDI_TEXT),
    api("GetTextExtentPoint", GDI_TEXT),
    api("ImmLockIMC", IMM),
    api("ImmDisableTextFrameService", IMM),
];

/// Additional rules for Rust code only.
const RUST_ONLY: &[Rule] = &[
    word("asm!", ASM),
    word("global_asm!", ASM),
    word("naked_asm!", ASM),
    api("#[naked]", ASM),
    api("#[unsafe(naked)]", ASM),
];

pub struct ScanResult {
    pub files: usize,
    pub findings: Vec<Finding>,
}

pub fn scan_workspace(workspace: &Workspace) -> ScanResult {
    let mut result = ScanResult {
        files: 0,
        findings: Vec::new(),
    };
    scan_file(workspace, &workspace.root_manifest, &mut result);
    for member in &workspace.members {
        let dir = workspace.root.join(&member.dir);
        if member.name == "xtask" {
            // The rule tables above would match themselves.
            scan_file(workspace, &dir.join("Cargo.toml"), &mut result);
        } else {
            walk(workspace, &dir, &mut result);
        }
    }
    result
}

fn walk(workspace: &Workspace, dir: &Path, result: &mut ScanResult) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    let mut paths: Vec<_> = entries.filter_map(|e| e.ok().map(|e| e.path())).collect();
    paths.sort();
    for path in paths {
        let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if path.is_dir() {
            if name != "target" && !name.starts_with('.') {
                walk(workspace, &path, result);
            }
        } else {
            scan_file(workspace, &path, result);
        }
    }
}

fn scan_file(workspace: &Workspace, path: &Path, result: &mut ScanResult) {
    let rust = match path.extension().and_then(|e| e.to_str()) {
        Some("rs") => true,
        Some("toml") => false,
        _ => return,
    };
    let Ok(text) = fs::read_to_string(path) else {
        result.findings.push(Finding {
            location: relative(workspace, path),
            message: "cannot be read as UTF-8".into(),
        });
        return;
    };
    result.files += 1;
    let code = if rust {
        strip_rust_comments(&text)
    } else {
        strip_toml_comments(&text)
    };
    check_text(
        &relative(workspace, path),
        &code,
        rust,
        &mut result.findings,
    );
}

fn check_text(path: &str, code: &str, rust: bool, findings: &mut Vec<Finding>) {
    let rules = LEGACY.iter().chain(if rust { RUST_ONLY } else { &[] });
    for (index, line) in code.lines().enumerate() {
        let location = format!("{path}:{}", index + 1);
        for rule in rules.clone() {
            if matches(line, rule) {
                findings.push(Finding {
                    location: location.clone(),
                    message: format!("`{}`: {}", rule.needle, rule.reason),
                });
            }
        }
        let allows_unsafe =
            line.contains("allow(unsafe_code)") || line.contains("expect(unsafe_code)");
        if rust && allows_unsafe && !UNSAFE_ALLOWED.iter().any(|dir| path.starts_with(dir)) {
            findings.push(Finding {
                location,
                message: "unsafe code may only be allowed in the directories listed in \
                          xtask/src/scan.rs (decision T14)"
                    .into(),
            });
        }
    }
}

fn matches(line: &str, rule: &Rule) -> bool {
    let (haystack, needle) = if rule.ignore_case {
        (line.to_lowercase(), rule.needle.to_lowercase())
    } else {
        (line.to_owned(), rule.needle.to_owned())
    };
    haystack.match_indices(&needle).any(|(at, _)| {
        !rule.word_start
            || haystack[..at]
                .chars()
                .next_back()
                .is_none_or(|c| !is_ident(c))
    })
}

fn relative(workspace: &Workspace, path: &Path) -> String {
    path.strip_prefix(&workspace.root)
        .unwrap_or(path)
        .to_string_lossy()
        .replace('\\', "/")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rust(path: &str, src: &str) -> Vec<Finding> {
        let mut findings = Vec::new();
        check_text(path, &strip_rust_comments(src), true, &mut findings);
        findings
    }

    fn toml(src: &str) -> Vec<Finding> {
        let mut findings = Vec::new();
        check_text(
            "Cargo.toml",
            &strip_toml_comments(src),
            false,
            &mut findings,
        );
        findings
    }

    #[test]
    fn legacy_api_in_code_is_reported_with_its_line() {
        let found = rust("crates/a/src/lib.rs", "fn a() {}\nlet d: IDirectSound8;\n");
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].location, "crates/a/src/lib.rs:2");
    }

    #[test]
    fn legacy_api_in_comments_is_ignored() {
        assert!(rust("x.rs", "/// Replaces DirectSound.\n// DirectPlay\n").is_empty());
    }

    #[test]
    fn library_names_in_strings_are_reported_regardless_of_case() {
        assert_eq!(rust("x.rs", "load(\"DSOUND.DLL\");").len(), 1);
    }

    #[test]
    fn inline_assembly_is_reported_but_not_identifiers_ending_in_asm() {
        assert_eq!(rust("x.rs", "core::arch::asm!(\"nop\");").len(), 1);
        assert!(rust("x.rs", "wasm!(x);").is_empty());
    }

    #[test]
    fn legacy_windows_features_are_reported_in_manifests() {
        let found = toml("windows = { features = [\"Win32_Media_DirectShow\"] } # DirectPlay");
        assert_eq!(found.len(), 1);
        assert!(found[0].message.contains("Win32_Media_DirectShow"));
    }

    #[test]
    fn unsafe_allowed_only_in_listed_directories() {
        let src = "#[allow(unsafe_code)]\nmod ffi {}\n";
        assert!(rust("crates/neuron-client/src/platform/window.rs", src).is_empty());
        assert_eq!(rust("crates/neuron-client/src/lib.rs", src).len(), 1);
        assert_eq!(rust("crates/game-sim/src/lib.rs", src).len(), 1);
    }
}
