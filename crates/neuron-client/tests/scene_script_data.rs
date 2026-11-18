//! Every shipped scene script on the original data (`outdir/`, decision
//! O15) runs in the evaluator and passes both passes of the parser.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use neuron_client::scene_script::{BuildError, BuildState, Step, Value, Vm, build, load};

fn thn_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            thn_files(&path, out);
        } else if path
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("thn"))
        {
            out.push(path);
        }
    }
}

#[test]
fn every_shipped_scene_script_loads() {
    let data = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut files = Vec::new();
    thn_files(&data, &mut files);
    if files.is_empty() {
        println!("outdir/data has no .thn files: nothing to load");
        return;
    }
    files.sort();
    let (mut no_events, mut no_duration, mut entities, mut events) = (0, 0, 0u64, 0u64);
    for file in &files {
        let bytes = fs::read(file).unwrap();
        // A fresh state per file, so no script sees another's globals.
        let mut vm = Vm::new();
        let summary = load(&mut vm, &bytes).unwrap_or_else(|e| panic!("{}: {e}", file.display()));
        no_events += usize::from(vm.global("events").as_table().is_none());
        no_duration += usize::from(vm.global("duration").as_number().is_none());
        entities += u64::from(summary.entities);
        events += u64::from(summary.events);
        let unexpected: Vec<String> = vm
            .changed_globals()
            .map(|(name, _)| String::from_utf8_lossy(name).into_owned())
            .filter(|n| !matches!(n.as_str(), "entities" | "events" | "duration"))
            .collect();
        assert!(
            unexpected.is_empty(),
            "{}: sets {unexpected:?}",
            file.display()
        );
    }
    println!(
        "{} scripts: {entities} entities, {events} events; {no_events} without events, \
         {no_duration} without a numeric duration",
        files.len()
    );
    assert_eq!(files.len(), 1506);
}

/// Every shipped scene script passes the parser's second pass, in a fresh
/// module state per file, with a COM context, and in one module state for
/// all of them in file order, as the game keeps one for its lifetime.
#[test]
fn every_shipped_scene_script_builds() {
    let data = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut files = Vec::new();
    thn_files(&data, &mut files);
    if files.is_empty() {
        println!("outdir/data has no .thn files: nothing to build");
        return;
    }
    files.sort();
    let mut shared_vm = Vm::new();
    let mut shared_state = BuildState::new();
    let mut counts = [0usize; 4];
    let mut kinds = BTreeMap::new();
    let mut untyped = 0;
    let mut most_keys = 0;
    for file in &files {
        let bytes = fs::read(file).unwrap();
        let mut vm = Vm::new();
        let mut state = BuildState::new();
        let summary = load(&mut vm, &bytes).unwrap();
        let scene = build(&mut vm, &mut state, &bytes, summary, true)
            .unwrap_or_else(|e| panic!("{}: {e}", file.display()));
        // Entities without a type leak a Lua block in the reference (D241).
        if let Value::Table(id) = vm.global("entities") {
            untyped += vm
                .table(*id)
                .entries()
                .filter(|(_, e)| match e {
                    Value::Table(t) => vm.table(*t).field("type").is_nil(),
                    _ => false,
                })
                .count();
        }
        for step in &scene.steps {
            let at = match step {
                Step::AddEntity(_) => 0,
                Step::SetUserProp { .. } => 1,
                Step::UpdateEntity { .. } => 2,
                Step::AddEvent(e) => {
                    *kinds.entry(e.common.kind).or_insert(0) += 1;
                    let keys = e.common.param_curve.as_ref().map_or(0, |c| c.keys.len());
                    most_keys = most_keys.max(keys);
                    3
                }
            };
            counts[at] += 1;
        }
        assert_eq!(
            scene.entities().count(),
            summary.entities as usize,
            "{}: every entity of the shipped data is built",
            file.display()
        );
        let summary = load(&mut shared_vm, &bytes).unwrap();
        build(&mut shared_vm, &mut shared_state, &bytes, summary, true)
            .unwrap_or_else(|e| panic!("{} after the others: {e}", file.display()));
    }
    println!(
        "{} scripts: {} entities, {} user properties, {} entity updates, {} events by type \
         {kinds:?}; {untyped} entities without a type; at most {most_keys} curve keys",
        files.len(),
        counts[0],
        counts[1],
        counts[2],
        counts[3]
    );
    // The reference's Lua stack overflows from 1,172 keys in one curve (D243).
    assert_eq!(untyped, 0);
    assert!(most_keys < 1000);
}

/// The scripts on which the reference faults give the error the port
/// reports instead (divergence D240).
#[test]
fn faulting_scripts_report_errors() {
    let path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/scene_script_build_faults.list");
    let text = fs::read_to_string(&path).unwrap();
    let mut cases = 0;
    for line in text.lines().filter(|l| !l.starts_with('#')) {
        let (want, chunk) = line.split_once(' ').unwrap();
        let bytes: Vec<u8> = (0..chunk.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&chunk[i..i + 2], 16).unwrap())
            .collect();
        let mut vm = Vm::new();
        let mut state = BuildState::new();
        let summary = load(&mut vm, &bytes).unwrap();
        let got = match build(&mut vm, &mut state, &bytes, summary, true) {
            Err(BuildError::NullString(what)) => format!("null:{what}"),
            Err(BuildError::NoEntity(what)) => format!("entity:{what}"),
            other => format!("{other:?}"),
        };
        assert_eq!(got.replace(' ', "_"), want);
        cases += 1;
    }
    assert_eq!(cases, 11);
}
