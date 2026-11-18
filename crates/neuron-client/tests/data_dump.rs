//! Data dumps of this crate over the original data (`support/data.rs`).

mod support;

use std::fs;

use neuron_client::scene_script::{BuildState, Vm};

use support::data::{Report, data_dirs, list_files};
use support::{scene_script, scene_script_build};

/// P4.14: every `.thn` under `DATA` in a fresh Lua state, and the state
/// after it (`RunSceneScriptDataDump`); kind `thn`.
#[test]
fn data_dump_thn() {
    let Some(dirs) = data_dirs("thn") else {
        return;
    };
    let files: Vec<String> = list_files(&dirs.root)
        .into_iter()
        .filter(|f| f.starts_with("DATA/") && f.to_ascii_lowercase().ends_with(".thn"))
        .collect();
    let mut report = Report::new("thn", &dirs);
    report.file_list(&files);
    let mut failed = 0;
    for file in &files {
        let bytes = fs::read(dirs.root.join(file)).unwrap_or_default();
        let mut vm = Vm::new();
        let status = scene_script::status(&mut vm, &bytes);
        failed += usize::from(status != 0);
        let head = format!("{{\"file\":\"{file}\",\"status\":\"{status}\"}}");
        report.check(file, &scene_script::render(&vm, head));
    }
    report.note(format!("{failed} scripts did not run"));
    report.finish();
}

/// P4.14: every `.thn` under `DATA` through both passes of the parser in a
/// fresh module state, and the engine calls of the second
/// (`RunSceneScriptBuildDataDump`); kind `thn_build`.
#[test]
fn data_dump_thn_build() {
    let Some(dirs) = data_dirs("thn_build") else {
        return;
    };
    let files: Vec<String> = list_files(&dirs.root)
        .into_iter()
        .filter(|f| f.starts_with("DATA/") && f.to_ascii_lowercase().ends_with(".thn"))
        .collect();
    let mut report = Report::new("thn_build", &dirs);
    report.file_list(&files);
    let mut calls = 0;
    for file in &files {
        let bytes = fs::read(dirs.root.join(file)).unwrap_or_default();
        let mut vm = Vm::new();
        let mut state = BuildState::new();
        let head = format!("{{\"file\":\"{file}\",");
        let lines = scene_script_build::run(&mut vm, &mut state, &bytes, true, &head);
        calls += lines.len() - 1;
        report.check(file, &lines);
    }
    report.note(format!("{calls} engine calls"));
    report.finish();
}
