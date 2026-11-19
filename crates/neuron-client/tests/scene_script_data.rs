//! Every shipped scene script on the original data (`outdir/`, decision
//! O15) runs in the evaluator, passes both passes of the parser, plays to
//! its end, and saves to source that reloads to the same scene.

mod support;

use std::cell::Cell;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use neuron_client::scene_script::curve::Curve;
use neuron_client::scene_script::entity::EntityTail;
use neuron_client::scene_script::playback::{EntityControls, EventRef, Player, state};
use neuron_client::scene_script::spline::SplinePath;
use neuron_client::scene_script::{
    BuildError, BuildState, CREATE_ALWAYS, ScriptEngine, ScriptModule, Step, Value, Vm, build, load,
};
use neuron_core::vfs::{GENERIC_READ, GENERIC_WRITE, MemFileSystem, MemSource, OPEN_EXISTING};

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

/// An engine object that answers every interface, counts the calls and
/// starts whatever it is asked to.
struct Counting(Rc<Cell<u64>>);

impl EntityControls for Counting {
    fn interfaces(&self) -> u32 {
        0x3fff
    }
    fn set_position(&mut self, _: [f32; 3]) {
        self.0.set(self.0.get() + 1);
    }
    fn set_orientation(&mut self, _: [f32; 9]) {
        self.0.set(self.0.get() + 1);
    }
    fn start_motion(&mut self, _: EventRef<'_>) -> i32 {
        0
    }
    fn start_sound(&mut self, _: EventRef<'_>, _: i32) -> i32 {
        0
    }
    fn start_ik(&mut self, _: EventRef<'_>, _: [f32; 3], _: [f32; 9]) -> i32 {
        0
    }
    fn connect_hardpoints(&mut self, _: EventRef<'_>) -> i32 {
        0
    }
    fn psys_start(&mut self) -> i32 {
        0
    }
    fn user_event_start(&mut self, _: EventRef<'_>) -> i32 {
        0
    }
    fn script_start(&mut self, _: i32) -> i32 {
        0
    }
}

/// Every shipped scene script plays to its end: once with nothing attached
/// (its own motion paths drive the path events) and once with an engine
/// object on every entity. Each curve and each motion path of the data
/// evaluates to finite values.
#[test]
fn every_shipped_scene_script_plays() {
    let data = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut files = Vec::new();
    thn_files(&data, &mut files);
    if files.is_empty() {
        println!("outdir/data has no .thn files: nothing to play");
        return;
    }
    files.sort();
    let (mut curves, mut paths, calls) = (0, 0, Rc::new(Cell::new(0)));
    for file in &files {
        let bytes = fs::read(file).unwrap();
        let mut vm = Vm::new();
        let mut state = BuildState::new();
        let summary = load(&mut vm, &bytes).unwrap();
        let scene = build(&mut vm, &mut state, &bytes, summary, true).unwrap();
        for desc in scene.events() {
            if let Some(curve) = desc.common.param_curve.as_ref().and_then(Curve::from_desc) {
                curves += 1;
                for i in 0..=16 {
                    let v = curve.evaluate(i as f32 / 16.0);
                    assert!(v.is_finite(), "{}: curve at {i}/16", file.display());
                }
            }
        }
        for desc in scene.entities() {
            if let EntityTail::MotionPath(m) = &desc.tail
                && let Some(object) = &m.path
            {
                let path = SplinePath::from_desc(object);
                paths += 1;
                for i in 0..=16 {
                    let t = i as f32 / 16.0;
                    let (mut p, mut q) = ([0.0; 3], [0.0; 4]);
                    path.evaluate(t, &mut p);
                    path.orientation_at(t, &mut q);
                    assert!(
                        p.iter().chain(&q).all(|v| v.is_finite()),
                        "{}: path at {t}",
                        file.display()
                    );
                }
            }
        }
        for attach in [false, true] {
            let mut player = Player::new();
            player.load(&scene, summary.duration_ms);
            if attach {
                let handles: Vec<i32> = player.entity_handles().collect();
                for handle in handles {
                    player.attach(handle, Box::new(Counting(calls.clone())));
                }
            }
            player.start(0);
            let step = (player.duration() / 50).max(50);
            let mut ticks = 0;
            while player.state() != state::DONE {
                player.tick(step);
                ticks += 1;
                assert!(ticks < 1000, "{}: does not end", file.display());
            }
        }
    }
    println!(
        "{} scripts played; {curves} curves, {paths} motion paths; {} pose calls",
        files.len(),
        calls.get()
    );
}

/// `bytes` loaded into a fresh script engine and module through a file
/// system (`Slot4Load`), and the text its save writes to another
/// (`Slot5Save` with the default save parser); `None` for a scene that does
/// not load.
fn load_and_save(bytes: &[u8]) -> (ScriptEngine, Option<Vec<u8>>) {
    let mut module = ScriptModule::new();
    let mut engine = ScriptEngine::new();
    let mut source = MemFileSystem::new(
        None,
        GENERIC_READ,
        OPEN_EXISTING,
        MemSource::Copy(Some(bytes)),
    );
    if engine.load(&mut module, Some(&mut source)) != 0 {
        return (engine, None);
    }
    let mut target = MemFileSystem::new(
        None,
        GENERIC_READ | GENERIC_WRITE,
        CREATE_ALWAYS,
        MemSource::Copy(None),
    );
    assert_eq!(engine.save(Some(&mut target)), 0);
    let text = target.buffer()[..target.logical_size() as usize].to_vec();
    (engine, Some(text))
}

/// The lines of a saved scene, without their line ends and the commas
/// that end all but the last entity and event, with each `q_orient` value
/// blanked; and how many lines held one.
fn normalised_lines(text: &[u8]) -> (Vec<Vec<u8>>, usize) {
    let mut q_orients = 0;
    let lines = text
        .split(|&c| c == b'\n')
        .map(|line| {
            let mut line = line.to_vec();
            while matches!(line.last(), Some(b'\r' | b',')) {
                line.pop();
            }
            if let Some(at) = line.windows(12).position(|w| w == b"q_orient = {") {
                let end = line[at..]
                    .iter()
                    .position(|&c| c == b'}')
                    .map_or(line.len(), |e| at + e);
                line.splice(at + 12..end, *b"..");
                q_orients += 1;
            }
            line
        })
        .collect();
    (lines, q_orients)
}

/// Two saved lines that differ only in numbers, each within a millionth of
/// its size (or of 1): what the reference's conversions lose on the way
/// through the text (light angles between radians and degrees).
fn close_lines(x: &[u8], y: &[u8]) -> bool {
    let tokens = |l: &[u8]| -> Vec<String> {
        String::from_utf8_lossy(l)
            .split([' ', ',', '{', '}', '='])
            .filter(|t| !t.is_empty())
            .map(str::to_owned)
            .collect()
    };
    let (a, b) = (tokens(x), tokens(y));
    a.len() == b.len()
        && a.iter().zip(&b).all(|(s, t)| {
            s == t
                || matches!((s.parse::<f64>(), t.parse::<f64>()),
                    (Ok(u), Ok(v)) if (u - v).abs() <= 1e-6 * u.abs().max(1.0))
        })
}

/// Every shipped scene script, loaded and saved, reloads to the same scene:
/// the saved source compiles (with the subset compiler of `cargo xtask
/// thn-decompile`, since the port loads compiled chunks only, decision
/// O18), loads, has the same entities, user properties and events, and
/// saves to the same text. Two things differ as they do in the reference:
/// events with equal start times may come back in another order (the
/// master sort is not stable), and a spatial event's `q_orient` holds four
/// words of the quaternion's matrix instead of the quaternion, so it
/// reloads as another rotation and saves differently.
#[test]
fn every_shipped_scene_script_saves_and_reloads() {
    let data = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut files = Vec::new();
    thn_files(&data, &mut files);
    if files.is_empty() {
        println!("outdir/data has no .thn files: nothing to save");
        return;
    }
    files.sort();
    let (mut bytes_saved, mut same_text, mut q_orient_events) = (0, 0, 0);
    let mut drifted = BTreeMap::new();
    for file in &files {
        let name = file.display();
        let (first, text) = load_and_save(&fs::read(file).unwrap());
        let text = text.unwrap_or_else(|| panic!("{name}: does not load"));
        bytes_saved += text.len();
        // The port loads compiled chunks only (decision O18, divergence
        // D86), so the saved source is compiled first, as a modded script
        // must be; the compiler accepts the language the writer writes.
        let chunk = lua32_compiler::compile(&text)
            .unwrap_or_else(|e| panic!("{name}: the saved source does not compile: {e}"))
            .write();
        let (second, again) = load_and_save(&chunk);
        let again = again.unwrap_or_else(|| panic!("{name}: the saved source does not load"));
        let (a, b) = (first.player(), second.player());
        assert_eq!(a.live_entity_count(), b.live_entity_count(), "{name}");
        assert_eq!(a.duration(), b.duration(), "{name}");
        for handle in a.entity_handles() {
            assert_eq!(
                a.entity_owner_type(handle),
                b.entity_owner_type(handle),
                "{name}: entity {handle}"
            );
            assert_eq!(a.user_props(handle), b.user_props(handle), "{name}");
        }
        let events = |p: &Player| {
            let mut all: Vec<_> = p
                .events()
                .map(|(_, e)| {
                    let c = &e.common;
                    (
                        c.start_ms,
                        c.kind,
                        c.flags,
                        c.event_flags,
                        c.duration_ms,
                        c.primary,
                        c.secondary,
                        c.pcurve_period,
                    )
                })
                .collect();
            all.sort_unstable();
            all
        };
        let (old_events, new_events) = (events(a), events(b));
        assert_eq!(old_events.len(), new_events.len(), "{name}");
        // Times go through a `float` of seconds printed to the millisecond,
        // which keeps every millisecond only below about 4,000 seconds.
        let near = |a: i32, b: i32| a.abs_diff(b) <= 1 + a.unsigned_abs() / (1 << 23);
        for (x, y) in old_events.iter().zip(&new_events) {
            assert!(
                near(x.0, y.0)
                    && near(x.4, y.4)
                    && (x.1, x.2, x.3, x.5, x.6, x.7) == (y.1, y.2, y.3, y.5, y.6, y.7),
                "{name}: event {x:?} became {y:?}"
            );
        }
        if text == again {
            same_text += 1;
            continue;
        }
        let (mut old, q_orients) = normalised_lines(&text);
        let (mut new, _) = normalised_lines(&again);
        q_orient_events += q_orients;
        let events_at = old
            .iter()
            .position(|l| l == b"events =")
            .unwrap_or_else(|| panic!("{name}: no events"));
        let mut same = |old: &[Vec<u8>], new: &[Vec<u8>], what: &str| {
            assert_eq!(old.len(), new.len(), "{name}: {what}");
            for (x, y) in old.iter().zip(new) {
                if x == y {
                    continue;
                }
                assert!(
                    close_lines(x, y),
                    "{name}: {what}: {} became {}",
                    String::from_utf8_lossy(x),
                    String::from_utf8_lossy(y)
                );
                let line = String::from_utf8_lossy(x);
                let key = if line.starts_with("\t{") {
                    line.split(", ").nth(1).unwrap_or_default().to_owned()
                } else {
                    line.split('=').next().unwrap_or_default().trim().to_owned()
                };
                *drifted.entry(key).or_insert(0) += 1;
            }
        };
        same(&old[..events_at], &new[..events_at], "the entities");
        old[events_at..].sort_unstable();
        new[events_at..].sort_unstable();
        same(&old, &new, "the events");
    }
    println!(
        "{} scripts saved ({bytes_saved} bytes) and reloaded; {same_text} save to the same text, \
         the others to the same events in another order or with another q_orient \
         ({q_orient_events} spatial events with q_orient); lines with numbers within a \
         millionth: {drifted:?}",
        files.len()
    );
}
