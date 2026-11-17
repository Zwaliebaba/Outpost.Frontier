//! Oracle vectors and data dumps for work package P1.8,
//! `neuron-core::scene` against `SceneRuntime.dll`.
//!
//! The synthetic inputs come from
//! `tests/fixtures/oracle/make_scene_fixtures.py`; the C++ harness
//! (`CppSource/Oracle/oracle_scene.cpp`) renders the same lines through the
//! reference's Engine, Hardpoint and Animation components.
//!
//! * `scene_load` (Tier A, exact): compound archetypes, their parts,
//!   constraints and hardpoints, and the instance trees made from them.
//! * `scene_sample` (Tier A, exact): channel sampling.
//! * `scene_eval` (Tier B): joint transforms from joint parameters, world
//!   transforms, and scripts played through the mixer. Float fields are
//!   compared within [`EVAL_TOLERANCE`]; everything else exactly.
//!
//! The data dumps over `outdir/` are kinds `scene` (as `scene_load`) and
//! `scene_eval` (as the script part of `scene_eval`).

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;
use std::sync::Arc;

mod support;

use support::data::Tolerance;
use support::{check_suite, hex, input_lines};

use neuron_core::anim::{AnimScript, Channel, load_channel};
use neuron_core::hash::name_crc32;
use neuron_core::scene::typed::TypedClass;
use neuron_core::scene::{
    Animation, ArchetypeId, Engine, Hardpoints, InstanceId, JointPayload, MixerKey,
    connect_hardpoints, sample,
};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem, VfsError, create_file_system};

/// The bound of the Tier B comparisons: joint angles go through `sin` and
/// `cos` (the x87's `FSIN`/`FCOS` and the Rust library may differ by an
/// ulp in double precision, rarely visible after the `float` store), and
/// such a difference grows through the matrix products down a tree. A
/// value matches within 16 ulps, or within 2^-20 of the oracle for values
/// that cancel to near zero.
const EVAL_TOLERANCE: Tolerance = Tolerance {
    keys: &["state", "local", "world", "values"],
    ulps: 16,
    absolute: 9.536_743e-7,
};

/// The time steps of the two passes a script is played in: forward from
/// the start without looping, and backwards from the end, looping.
const PASSES: [(u32, f32, &[f32]); 2] = [
    (0, 0.0, &[0.0, 0.25, 0.5, 1.0, 2.5]),
    (3, -1.0, &[0.0, 0.4, 1.3, 3.7]),
];

fn open_archive(host: &Host, path: &str) -> Result<UtfFileSystem, VfsError> {
    UtfFileSystem::open(
        host,
        &FsDesc::read(path.as_bytes()).with_implementation(b"UTF"),
    )
}

/// An engine with the Hardpoint component and a search path relative to
/// `root` (the component manager's `FileSystem` request).
fn new_engine(root: &Path) -> Engine {
    let mut engine = Engine::new();
    engine.add_component(Box::new(Hardpoints::new()));
    let host = Host::new(root);
    engine.set_search_path(Some(Box::new(move |desc: &FsDesc<'_>| {
        create_file_system(&host, desc)
    })));
    engine
}

fn bits(value: f32) -> String {
    let b = if value.is_nan() {
        0x7FC0_0000
    } else {
        value.to_bits()
    };
    format!("{b:08x}")
}

fn float_list(values: &[f32]) -> String {
    values
        .iter()
        .map(|&v| bits(v))
        .collect::<Vec<_>>()
        .join(",")
}

fn payload_hex(payload: &JointPayload) -> String {
    payload.0.iter().fold(String::new(), |mut out, w| {
        let _ = write!(out, "{w:08x}");
        out
    })
}

/// The archetypes reachable from `top`, numbered in pre-order.
fn number_archetypes(engine: &Engine, top: ArchetypeId) -> Vec<ArchetypeId> {
    fn visit(engine: &Engine, id: ArchetypeId, out: &mut Vec<ArchetypeId>) {
        if out.contains(&id) {
            return;
        }
        out.push(id);
        if let Some(a) = engine.archetype(id) {
            for child in &a.children {
                visit(engine, child.archetype, out);
            }
        }
    }
    let mut out = Vec::new();
    visit(engine, top, &mut out);
    out
}

fn index_of<T: PartialEq>(list: &[T], item: &T) -> String {
    list.iter()
        .position(|x| x == item)
        .map_or("-".to_owned(), |i| i.to_string())
}

/// The instances of the tree under `root`, depth first.
fn instance_order(engine: &Engine, root: InstanceId) -> Vec<InstanceId> {
    let mut out = vec![root];
    let mut cursor = engine.related_instance(Some(root), 0, None);
    while let Some(at) = cursor {
        out.push(at);
        cursor = engine.related_instance(Some(root), 0, Some(at));
    }
    out
}

/// The `scene_load` / `scene` lines of an archetype: its archetypes with
/// their parts, constraints and hardpoints, then the instance tree.
fn archetype_lines(engine: &mut Engine, top: ArchetypeId, lines: &mut Vec<String>) {
    let order = number_archetypes(engine, top);
    for (index, &id) in order.iter().enumerate() {
        let Some(a) = engine.archetype(id) else {
            continue;
        };
        let records = engine
            .component::<Hardpoints>()
            .and_then(|h| h.records(id))
            .map(<[_]>::to_vec);
        lines.push(format!(
            "{{\"archetype\":\"{index}\",\"name\":\"{}\",\"crc\":\"{:08x}\",\"refs\":\"{}\",\"parts\":\"{}\",\"constraints\":\"{}\",\"hardpoints\":\"{}\"}}",
            hex(&a.name),
            a.crc,
            a.ref_count,
            a.children.len(),
            a.constraints.len(),
            records.as_ref().map_or("-".to_owned(), |r| r.len().to_string())
        ));
        for (i, child) in a.children.iter().enumerate() {
            lines.push(format!(
                "{{\"part\":\"{i}\",\"name\":\"{}\",\"crc\":\"{:08x}\",\"archetype\":\"{}\"}}",
                hex(&child.name),
                child.crc,
                index_of(&order, &child.archetype)
            ));
        }
        for (i, c) in a.constraints.iter().enumerate() {
            lines.push(format!(
                "{{\"constraint\":\"{i}\",\"parent\":\"{}\",\"child\":\"{}\",\"payload\":\"{}\"}}",
                c.parent,
                c.child,
                payload_hex(&c.payload)
            ));
        }
        for (i, r) in records.unwrap_or_default().iter().enumerate() {
            lines.push(format!(
                "{{\"hardpoint\":\"{i}\",\"name\":\"{}\",\"crc\":\"{:08x}\",\"data\":\"{}\"}}",
                hex(&r.name),
                r.crc,
                hex(&r.data.0)
            ));
        }
    }
    let Some(root) = engine.create_instance(top) else {
        lines.push("{\"instances\":\"0\"}".to_owned());
        return;
    };
    let instances = instance_order(engine, root);
    lines.push(format!("{{\"instances\":\"{}\"}}", instances.len()));
    for (i, &id) in instances.iter().enumerate() {
        let Some(inst) = engine.instance(id) else {
            continue;
        };
        let parent = inst
            .parent
            .map_or("-".to_owned(), |p| index_of(&instances, &p));
        lines.push(format!(
            "{{\"instance\":\"{i}\",\"parent\":\"{parent}\",\"archetype\":\"{}\",\"part\":\"{}\",\"joint\":\"{}\"}}",
            index_of(&order, &inst.archetype),
            inst.part.map_or("-".to_owned(), |p| p.1.to_string()),
            payload_hex(&inst.relative)
        ));
    }
    engine.destroy_instance(root);
}

/// The transforms of every instance under `root`, depth first.
fn pose_lines(engine: &Engine, root: InstanceId, prefix: &str, lines: &mut Vec<String>) {
    for (i, id) in instance_order(engine, root).into_iter().enumerate() {
        let Some(inst) = engine.instance(id) else {
            continue;
        };
        lines.push(format!(
            "{{{prefix}\"instance\":\"{i}\",\"state\":\"{}\",\"local\":\"{}\",\"world\":\"{}\"}}",
            float_list(&inst.joint_state),
            float_list(&inst.local.to_words()),
            float_list(&inst.transform.matrix.to_words())
        ));
    }
}

/// Whether a script entry is played: every track has a loaded channel with
/// frames whose flags name a class. Anything else makes the reference
/// dereference null.
fn playable(entry: &neuron_core::anim::ScriptEntry) -> bool {
    entry.tracks.iter().all(|t| {
        t.channel
            .as_ref()
            .and_then(|c| c.channel())
            .is_some_and(|c| c.frame_count() != 0 && TypedClass::from_type(c.flags()).is_some())
    })
}

/// The script part of `scene_eval`: every animation of `script`, played on
/// a fresh instance of `top` in the two [`PASSES`].
fn script_lines(
    engine: &mut Engine,
    top: ArchetypeId,
    script: &Arc<AnimScript>,
    lines: &mut Vec<String>,
) {
    let mut animation = Animation::new();
    for (crc, entry) in script.entries() {
        let head = format!("{{\"anim\":\"{crc:08x}\",\"name\":\"{}\"", hex(&entry.name));
        if !playable(entry) {
            lines.push(format!("{head},\"status\":\"skip\"}}"));
            continue;
        }
        lines.push(format!("{head},\"status\":\"play\"}}"));
        for (pass, (flags, start, steps)) in PASSES.iter().enumerate() {
            let Some(root) = engine.create_instance(top) else {
                lines.push(format!("{{\"pass\":\"{pass}\",\"root\":\"-\"}}"));
                continue;
            };
            engine.update_instance(Some(root), 0);
            let id = animation.play_script(engine, script, Some(root), &entry.name, 0, 0);
            let Some(id) = id else {
                lines.push(format!("{{\"pass\":\"{pass}\",\"id\":\"-\"}}"));
                engine.destroy_instance(root);
                continue;
            };
            let bound = animation.objects(id).map_or(0, <[_]>::len);
            let eval =
                animation.eval_playback(engine, id, *flags, *start, 1.0, 0.0, 1.0, 1.0, None);
            lines.push(format!(
                "{{\"pass\":\"{pass}\",\"bound\":\"{bound}\",\"eval\":\"{eval:08x}\"}}"
            ));
            for (step, &dt) in steps.iter().enumerate() {
                animation.mixer_walk(engine, MixerKey::of(Some(root)), dt);
                engine.update_instance(Some(root), 0);
                pose_lines(
                    engine,
                    root,
                    &format!("\"pass\":\"{pass}\",\"step\":\"{step}\","),
                    lines,
                );
            }
            animation.destroy_script(engine, id);
            engine.destroy_instance(root);
        }
    }
}

fn scene_file(path: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    [".cmp", ".3db", ".dfm"].iter().any(|e| lower.ends_with(e))
}

#[test]
fn data_dump_scene() {
    let Some(dirs) = support::data::data_dirs("scene") else {
        return;
    };
    let host = Host::new(&dirs.root);
    let mut covered = Vec::new();
    let mut dumps = Vec::new();
    let mut errors = Vec::new();
    let mut counts: BTreeMap<&str, usize> = BTreeMap::new();
    let mut engine = new_engine(&dirs.root);
    let mut leaks = 0usize;
    for file in support::data::list_files(&dirs.root) {
        if !scene_file(&file) || support::data::magic(&dirs.root, &file) != b"UTF " {
            continue;
        }
        let Ok(mut fs) = open_archive(&host, &file) else {
            continue;
        };
        covered.push(file.clone());
        let mut lines = Vec::new();
        match engine.create_archetype_handle(file.as_bytes(), Some(&mut fs)) {
            Ok(Some(top)) => {
                lines.push(format!("{{\"file\":\"{file}\",\"archetype\":\"1\"}}"));
                let a = engine
                    .archetype(top)
                    .map(|a| (a.children.len(), a.constraints.len()));
                if let Some((parts, constraints)) = a {
                    *counts.entry("compounds").or_default() += usize::from(parts != 0);
                    *counts.entry("parts").or_default() += parts;
                    *counts.entry("constraints").or_default() += constraints;
                }
                archetype_lines(&mut engine, top, &mut lines);
                engine.release_handle(top);
                leaks += engine.archetypes().count();
            }
            Ok(None) => lines.push(format!("{{\"file\":\"{file}\",\"archetype\":\"0\"}}")),
            Err(e) => {
                errors.push(format!("{file}: {e}"));
                lines.push(format!("{{\"file\":\"{file}\",\"archetype\":\"error\"}}"));
            }
        }
        dumps.push((file, lines));
    }
    let mut report = support::data::Report::new("scene", &dirs);
    report.file_list(&covered);
    for (file, lines) in &dumps {
        report.check(file, lines);
    }
    report.note(format!(
        "{} files, {} load in rust; {counts:?}",
        covered.len(),
        covered.len() - errors.len()
    ));
    for e in &errors {
        report.note(format!("error: {e}"));
    }
    assert!(errors.is_empty(), "{errors:?}");
    assert_eq!(leaks, 0, "archetypes left after their release");
    report.finish();
}

#[test]
fn data_dump_scene_eval() {
    let Some(dirs) = support::data::data_dirs("scene_eval") else {
        return;
    };
    let host = Host::new(&dirs.root);
    let mut covered = Vec::new();
    let mut dumps = Vec::new();
    let mut errors = Vec::new();
    let (mut anims, mut skipped) = (0usize, 0usize);
    let mut engine = new_engine(&dirs.root);
    let mut leaks = 0usize;
    for file in support::data::list_files(&dirs.root) {
        if !scene_file(&file) || support::data::magic(&dirs.root, &file) != b"UTF " {
            continue;
        }
        let Ok(mut script_fs) = open_archive(&host, &file) else {
            continue;
        };
        let script = match AnimScript::open(&mut script_fs) {
            Ok(Some(script)) => Arc::new(script),
            Ok(None) => continue,
            Err(e) => {
                errors.push(format!("{file}: {e}"));
                continue;
            }
        };
        let Ok(mut fs) = open_archive(&host, &file) else {
            continue;
        };
        let top = match engine.create_archetype_handle(file.as_bytes(), Some(&mut fs)) {
            Ok(Some(top)) => top,
            Ok(None) => continue,
            Err(e) => {
                errors.push(format!("{file}: {e}"));
                continue;
            }
        };
        covered.push(file.clone());
        let mut lines = vec![format!(
            "{{\"file\":\"{file}\",\"entries\":\"{}\"}}",
            script.len()
        )];
        anims += script.len();
        skipped += script.entries().filter(|(_, e)| !playable(e)).count();
        script_lines(&mut engine, top, &script, &mut lines);
        engine.release_handle(top);
        leaks += engine.archetypes().count() + engine.instances().count();
        dumps.push((file, lines));
    }
    let mut report = support::data::Report::new("scene_eval", &dirs);
    report.file_list(&covered);
    for (file, lines) in &dumps {
        report.check_within(file, lines, &EVAL_TOLERANCE);
    }
    report.note(format!(
        "{} files, {anims} animations, {skipped} not played (a track without a playable channel)",
        covered.len()
    ));
    assert!(errors.is_empty(), "{errors:?}");
    assert_eq!(leaks, 0, "archetypes or instances left after their release");
    report.finish();
}

// --- synthetic suites ----------------------------------------------------------------------

fn hex_f32(text: &str) -> f32 {
    f32::from_bits(u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}")))
}

/// Compares `lines` with the committed suite, float fields within the
/// tolerance, or writes it when `OUTPOST_BLESS_VECTORS` is set.
fn check_suite_within(name: &str, lines: &[String], tolerance: &Tolerance) {
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite(name, lines);
        return;
    }
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let committed =
        std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        if let Err(why) = tolerance.compare_line(want, got) {
            panic!(
                "{name} line {}: {why}\n  committed: {want}\n  port:      {got}",
                index + 1
            );
        }
    }
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
}

fn fixture_engine() -> Engine {
    new_engine(&support::fixtures())
}

/// Loads `archive` (relative to the fixtures) into `engine`.
fn load_fixture(engine: &mut Engine, archive: &str) -> Option<ArchetypeId> {
    let host = Host::new(support::fixtures());
    let mut fs = open_archive(&host, archive).ok()?;
    engine
        .create_archetype_handle(archive.as_bytes(), Some(&mut fs))
        .unwrap_or_else(|e| panic!("{archive}: {e}"))
}

#[test]
fn scene_load_suite() {
    let mut engine = fixture_engine();
    let host = Host::new(support::fixtures());
    let mut lines = Vec::new();
    for archive in input_lines("scene/load.list") {
        let Ok(mut fs) = open_archive(&host, &archive) else {
            lines.push(format!("{{\"file\":\"{archive}\",\"archetype\":\"-\"}}"));
            continue;
        };
        match engine.create_archetype_handle(archive.as_bytes(), Some(&mut fs)) {
            Ok(Some(top)) => {
                lines.push(format!("{{\"file\":\"{archive}\",\"archetype\":\"1\"}}"));
                archetype_lines(&mut engine, top, &mut lines);
                engine.release_handle(top);
            }
            Ok(None) => lines.push(format!("{{\"file\":\"{archive}\",\"archetype\":\"0\"}}")),
            Err(e) => panic!("{archive}: {e}"),
        }
        lines.push(format!("{{\"live\":\"{}\"}}", engine.archetypes().count()));
    }
    check_suite("scene_load.jsonl", &lines);
}

/// Loads channel `name` of `scene/sample.utf`.
fn sample_channel(name: &str) -> Channel {
    let host = Host::new(support::fixtures());
    let fs = open_archive(&host, "scene/sample.utf").unwrap_or_else(|e| panic!("{e:?}"));
    let mut nested = FileSystem::create_child(&fs, &FsDesc::read(name.as_bytes()))
        .unwrap_or_else(|e| panic!("{name}: {e:?}"));
    let loaded = load_channel(nested.as_mut()).unwrap_or_else(|e| panic!("{e}"));
    loaded.outcome.unwrap_or_else(|e| panic!("{name}: {e}"))
}

fn keys_text(keys: &[sample::EventKey]) -> String {
    keys.iter()
        .map(|k| format!("{}:{:x}", bits(k.time), k.offset))
        .collect::<Vec<_>>()
        .join(",")
}

#[test]
fn scene_sample_suite() {
    let mut lines = Vec::new();
    for line in input_lines("scene/sample.list") {
        let (name, time) = line.split_once('|').unwrap_or_else(|| panic!("{line}"));
        let channel = sample_channel(name);
        let t = hex_f32(time);
        let mut out = format!("{{\"channel\":\"{name}\",\"time\":\"{time}\"");
        match sample::sample(&channel, t) {
            Ok(s) => {
                let _ = write!(
                    out,
                    ",\"a\":\"{}\",\"b\":\"{}\",\"alpha\":\"{}\"",
                    s.a,
                    s.b,
                    bits(s.alpha)
                );
            }
            Err(code) => {
                let _ = write!(out, ",\"sample\":\"{code:08x}\"");
            }
        }
        match sample::sample_pair(&channel, t) {
            Ok(p) => {
                let _ = write!(
                    out,
                    ",\"lo\":\"{}\",\"hi\":\"{}\",\"time_lo\":\"{}\",\"time_hi\":\"{}\",\"pair_alpha\":\"{}\"",
                    p.sample.a,
                    p.sample.b,
                    bits(p.time_lo),
                    bits(p.time_hi),
                    bits(p.sample.alpha)
                );
            }
            Err(code) => {
                let _ = write!(out, ",\"pair\":\"{code:08x}\"");
            }
        }
        let frame = |last| sample::frame(&channel, last).map_or("-".to_owned(), |i| i.to_string());
        let _ = write!(
            out,
            ",\"first\":\"{}\",\"last\":\"{}\"}}",
            frame(false),
            frame(true)
        );
        lines.push(out);
    }
    for line in input_lines("scene/range.list") {
        let f: Vec<&str> = line.split('|').collect();
        let channel = sample_channel(f[0]);
        let cap: u32 = f[3].parse().unwrap_or_else(|e| panic!("{e}"));
        let c = sample::collect_range(&channel, hex_f32(f[1]), hex_f32(f[2]), cap);
        lines.push(format!(
            "{{\"range\":\"{line}\",\"result\":\"{:08x}\",\"count\":\"{}\",\"keys\":\"{}\"}}",
            c.result,
            c.count,
            keys_text(&c.keys)
        ));
    }
    for line in input_lines("scene/at.list") {
        let f: Vec<&str> = line.split('|').collect();
        let channel = sample_channel(f[0]);
        let cap: u32 = f[2].parse().unwrap_or_else(|e| panic!("{e}"));
        let c = sample::collect_at(&channel, hex_f32(f[1]), cap);
        lines.push(format!(
            "{{\"at\":\"{line}\",\"result\":\"{:08x}\",\"count\":\"{}\",\"keys\":\"{}\"}}",
            c.result,
            c.count,
            keys_text(&c.keys)
        ));
    }
    check_suite("scene_sample.jsonl", &lines);
}

/// The `values` field: a joint's parameters as `GetJointParams(.., 0, ..)`
/// gives them, `-` for an instance without a parent.
fn params_text(engine: &Engine, id: InstanceId) -> String {
    engine
        .joint_params(Some(id), 0)
        .map_or("-".to_owned(), |v| float_list(&v))
}

/// The pose of every instance of the tree, with its parameters.
fn scenario_pose(engine: &Engine, root: InstanceId, op: usize, lines: &mut Vec<String>) {
    for (i, id) in instance_order(engine, root).into_iter().enumerate() {
        let Some(inst) = engine.instance(id) else {
            continue;
        };
        lines.push(format!(
            "{{\"op\":\"{op}\",\"instance\":\"{i}\",\"state\":\"{}\",\"local\":\"{}\",\"world\":\"{}\",\"values\":\"{}\"}}",
            float_list(&inst.joint_state),
            float_list(&inst.local.to_words()),
            float_list(&inst.transform.matrix.to_words()),
            params_text(engine, id)
        ));
    }
}

/// One scenario of `scene/play.list` on a fresh instance of `top`.
fn play_scenario(
    engine: &mut Engine,
    animation: &mut Animation,
    script: &Arc<AnimScript>,
    top: Option<ArchetypeId>,
    line: &str,
    lines: &mut Vec<String>,
) {
    lines.push(format!("{{\"play\":\"{line}\"}}"));
    let Some(root) = top.and_then(|t| engine.create_instance(t)) else {
        lines.push("{\"root\":\"-\"}".to_owned());
        return;
    };
    engine.update_instance(Some(root), 0);
    let mut playbacks: Vec<Option<u32>> = Vec::new();
    for (op, token) in line.split(' ').enumerate() {
        let f: Vec<&str> = token.split(':').collect();
        match f[0] {
            "p" => {
                let name = support::unhex(f[1]);
                let flags = u32::from_str_radix(f[2], 16).unwrap_or_else(|e| panic!("{e}"));
                let [start, speed, blend, scale, factor] =
                    [f[3], f[4], f[5], f[6], f[7]].map(hex_f32);
                let id = animation.play_script(engine, script, Some(root), &name, 0, 0);
                playbacks.push(id);
                let Some(id) = id else {
                    lines.push(format!("{{\"op\":\"{op}\",\"id\":\"-\"}}"));
                    continue;
                };
                let bound = animation.objects(id).map_or(0, <[_]>::len);
                let eval = animation
                    .eval_playback(engine, id, flags, start, speed, blend, scale, factor, None);
                lines.push(format!(
                    "{{\"op\":\"{op}\",\"id\":\"{id:x}\",\"bound\":\"{bound}\",\"eval\":\"{eval:08x}\"}}"
                ));
            }
            "w" | "a" => {
                let dt = hex_f32(f[1]);
                let events = if f[0] == "w" {
                    animation.mixer_walk(engine, MixerKey::of(Some(root)), dt)
                } else {
                    animation.mixer_walk_all(engine, dt)
                };
                assert!(events.is_empty(), "{events:?}");
                engine.update_instance(Some(root), 0);
                scenario_pose(engine, root, op, lines);
            }
            "s" | "d" => {
                let n: usize = f[1].parse().unwrap_or_else(|e| panic!("{e}"));
                if let Some(Some(id)) = playbacks.get(n) {
                    if f[0] == "s" {
                        animation.stop_playback(engine, *id);
                    } else {
                        animation.destroy_script(engine, *id);
                    }
                }
                lines.push(format!("{{\"op\":\"{op}\",\"done\":\"{token}\"}}"));
            }
            other => panic!("unknown op {other}"),
        }
    }
    for id in playbacks.into_iter().flatten() {
        animation.destroy_script(engine, id);
    }
    engine.destroy_instance(root);
}

/// The hardpoint connections of `scene_eval`: parent archive and
/// hardpoint, child archive and hardpoint.
const CONNECTIONS: [(&str, &str, &str, &str); 6] = [
    ("scene/ship.cmp", "HpMount", "scene/simple.3db", "HpA"),
    ("scene/ship.cmp", "HpTurret01", "scene/simple.3db", "HpB"),
    ("scene/simple.3db", "HpB", "scene/ship.cmp", "HpSlide"),
    ("scene/simple.3db", "HpA", "scene/ship.cmp", "HpOdd"),
    ("scene/simple.3db", "HpZ", "scene/simple.3db", "HpA"),
    ("scene/simple.3db", "HpA", "scene/plain.3db", "HpA"),
];

#[test]
fn scene_eval_suite() {
    let mut engine = fixture_engine();
    let mut lines = Vec::new();
    let mut tops: BTreeMap<String, Option<ArchetypeId>> = BTreeMap::new();
    for line in input_lines("scene/joints.list") {
        let f: Vec<&str> = line.split('|').collect();
        let top = *tops
            .entry(f[0].to_owned())
            .or_insert_with(|| load_fixture(&mut engine, f[0]));
        let index: u32 = f[1].parse().unwrap_or_else(|e| panic!("{e}"));
        let values: Vec<f32> = f[2].split(',').map(hex_f32).collect();
        lines.push(format!("{{\"joints\":\"{line}\"}}"));
        let Some(root) = top.and_then(|t| engine.create_instance(t)) else {
            lines.push("{\"root\":\"-\"}".to_owned());
            continue;
        };
        engine.update_instance(Some(root), 0);
        scenario_pose(&engine, root, 0, &mut lines);
        let order = instance_order(&engine, root);
        let set: Vec<String> = order
            .iter()
            .map(|&id| u8::from(engine.set_joint_params(Some(id), index, &values)).to_string())
            .collect();
        lines.push(format!("{{\"set\":\"{}\"}}", set.join(",")));
        engine.update_instance(Some(root), 0);
        scenario_pose(&engine, root, 1, &mut lines);
        engine.destroy_instance(root);
    }

    let anim_top = *tops
        .entry("scene/anim.cmp".to_owned())
        .or_insert_with(|| load_fixture(&mut engine, "scene/anim.cmp"));
    let host = Host::new(support::fixtures());
    let mut script_fs = open_archive(&host, "scene/anim.cmp").unwrap_or_else(|e| panic!("{e:?}"));
    let script = Arc::new(
        AnimScript::open(&mut script_fs)
            .unwrap_or_else(|e| panic!("{e}"))
            .unwrap_or_else(|| panic!("scene/anim.cmp has no script")),
    );
    let mut animation = Animation::new();
    for line in input_lines("scene/play.list") {
        play_scenario(
            &mut engine,
            &mut animation,
            &script,
            anim_top,
            &line,
            &mut lines,
        );
    }

    for (a_file, a_hp, b_file, b_hp) in CONNECTIONS {
        let a_top = *tops
            .entry(a_file.to_owned())
            .or_insert_with(|| load_fixture(&mut engine, a_file));
        let b_top = *tops
            .entry(b_file.to_owned())
            .or_insert_with(|| load_fixture(&mut engine, b_file));
        let a = a_top.and_then(|t| engine.create_instance(t));
        let b = b_top.and_then(|t| engine.create_instance(t));
        let crc = |name: &str| name_crc32(name.as_bytes());
        let status = connect_hardpoints(&mut engine, a, crc(a_hp), b, crc(b_hp));
        let relative = engine.instance_relative(b).copied();
        let payload = relative.map_or("-".to_owned(), |p| {
            float_list(
                &p.0[1..]
                    .iter()
                    .map(|w| f32::from_bits(*w))
                    .collect::<Vec<_>>(),
            )
        });
        lines.push(format!(
            "{{\"connect\":\"{a_file}|{a_hp}|{b_file}|{b_hp}\",\"status\":\"{status}\",\"type\":\"{}\",\"values\":\"{payload}\",\"world\":\"{}\"}}",
            relative.map_or("-".to_owned(), |p| format!("{:08x}", p.code())),
            float_list(&engine.instance_transform(b).to_words())
        ));
        if let Some(b) = b {
            engine.destroy_joint(Some(b));
            engine.destroy_instance(b);
        }
        if let Some(a) = a {
            engine.destroy_instance(a);
        }
    }
    check_suite_within("scene_eval.jsonl", &lines, &EVAL_TOLERANCE);
}
