//! Every shipped scene script on the original data (`outdir/`, decision
//! O15) runs in the evaluator and passes the parser's first pass.

use std::fs;
use std::path::{Path, PathBuf};

use neuron_client::scene_script::{Vm, load};

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
