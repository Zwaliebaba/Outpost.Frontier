//! Oracle vectors for work package P1.7 (animation): channel archetypes
//! and animation scripts of `CppSource/SceneRuntime`.
//!
//! The inputs are in `tests/fixtures/oracle/anim/`, written by
//! `make_anim_fixtures.py`; the harness side is
//! `CppSource/Oracle/oracle_anim.cpp`, which loads the same archives
//! through `ReadFile`'s UTF file system into `SceneRuntime.dll`'s
//! `ChannelArchetype` and `Animation` components and walks the objects
//! they build.
//!
//! Every field is compared exactly except the frames of the axis-angle
//! channels (flags 0x80), whose unpacking calls `sin` (Tier B): each f32
//! word there must be within [`AXIS_ULPS`] ulps of the committed value, or
//! within [`AXIS_ABS`] of it. The absolute bound covers w components near
//! 0, where one ulp of the f32 sine moves `sqrt(1 - s * s)` by more ulps.

use std::fs;
use std::path::Path;

mod support;

use neuron_core::anim::{
    AnimScript, COMPRESS_MASK, ChannelError, FLAG_SCALAR_ONLY, LoadedChannel, load_channel,
};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem, VfsError};
use support::{check_suite, fixtures, hex, input_lines, read_fixture, unhex};

/// Tier B bound for axis-angle frame words, in ulps.
const AXIS_ULPS: u64 = 4;
/// Tier B bound for axis-angle frame words, absolute.
const AXIS_ABS: f32 = 1e-6;

fn host() -> Host {
    Host::new(fixtures())
}

fn genresult(error: VfsError) -> i32 {
    i32::from_ne_bytes(error.code().to_ne_bytes())
}

fn open_archive(host: &Host, path: &str) -> Result<UtfFileSystem, VfsError> {
    UtfFileSystem::open(
        host,
        &FsDesc::read(path.as_bytes()).with_implementation(b"UTF"),
    )
}

/// How a frame buffer is written: hex in the suites, a hash in the data
/// dump.
#[derive(Clone, Copy)]
enum Frames {
    Hex,
    Hash,
}

impl Frames {
    fn render(self, bytes: &[u8]) -> String {
        match self {
            Self::Hex => hex(bytes),
            Self::Hash => format!("fnv:{}", support::data::fnv64(bytes)),
        }
    }
}

fn channel_fields(channel: &LoadedChannel) -> String {
    format!(
        "\"count\":\"{}\",\"interval\":\"{:08x}\",\"flags\":\"{:x}\",\"stride\":\"{}\",\"duration\":\"{:08x}\"",
        channel.frame_count(),
        channel.interval().to_bits(),
        channel.flags(),
        channel.stride(),
        channel.duration().to_bits()
    )
}

/// The frame buffer an archetype in a script holds, as the harness writes
/// it without knowing the load result: `-` for none, `pool` while the
/// flags still have packed bits (the buffer is the shared unpacking pool),
/// `event` for an event channel (its file length is not kept), else the
/// frame count times stride bytes.
fn script_frames(channel: &LoadedChannel, mode: Frames) -> String {
    let flags = channel.flags();
    let held = match &channel.outcome {
        Ok(_) => true,
        // A failed read leaves the buffer allocated.
        Err(ChannelError::ShortFrames) => true,
        Err(_) => false,
    };
    if !held {
        return "-".to_owned();
    }
    if flags & COMPRESS_MASK != 0 {
        return "pool".to_owned();
    }
    if flags == FLAG_SCALAR_ONLY {
        return "event".to_owned();
    }
    match channel.channel() {
        Some(loaded) => mode.render(loaded.frames()),
        None if channel.frame_count().wrapping_mul(channel.stride()) == 0 => mode.render(&[]),
        // The rest of the buffer is uninitialised in the reference.
        None => "partial".to_owned(),
    }
}

fn name_field(name: Option<&neuron_core::anim::MapName>) -> String {
    name.map_or_else(|| "-".to_owned(), |n| hex(&n.name))
}

/// The entry and track lines of a script.
fn script_lines(script: &AnimScript, mode: Frames, lines: &mut Vec<String>) {
    for (crc, entry) in script.entries() {
        let height = entry.root_height.unwrap_or(0.0).to_bits();
        lines.push(format!(
            "{{\"entry\":\"{crc:08x}\",\"name\":\"{}\",\"tracks\":\"{}\",\"root\":\"{}\",\"height\":\"{height:08x}\",\"longest\":\"{}\"}}",
            hex(&entry.name),
            entry.tracks.len(),
            u8::from(entry.root_height.is_some()),
            entry.longest.map_or_else(|| "-".to_owned(), |i| i.to_string())
        ));
        for (index, track) in entry.tracks.iter().enumerate() {
            let mut line = format!(
                "{{\"track\":\"{index}\",\"parent\":\"{:08x}\",\"parent_name\":\"{}\",\"child\":\"{:08x}\",\"child_name\":\"{}\"",
                track.parent_crc(),
                name_field(track.parent.as_ref()),
                track.child_crc(),
                name_field(track.child.as_ref())
            );
            match &track.channel {
                None => line.push_str(",\"channel\":\"-\"}"),
                Some(channel) => line.push_str(&format!(
                    ",\"channel\":\"1\",{},\"frames\":\"{}\"}}",
                    channel_fields(channel),
                    script_frames(channel, mode)
                )),
            }
            lines.push(line);
        }
    }
}

/// Comma-separated hex names; `-` is a null name.
fn name_list(text: &str) -> Vec<Option<Vec<u8>>> {
    if text.is_empty() {
        return Vec::new();
    }
    text.split(',')
        .map(|n| (n != "-").then(|| unhex(n)))
        .collect()
}

fn channel_lines() -> Vec<String> {
    let host = host();
    let archive = open_archive(&host, "anim/channels.utf").expect("anim/channels.utf");
    input_lines("anim/channels.list")
        .iter()
        .map(|name| {
            match archive.create_child(&FsDesc::read(name.as_bytes())) {
                Err(e) => format!("{{\"channel\":\"{name}\",\"create\":\"{}\"}}", genresult(e)),
                Ok(fs) => {
                    let channel = load_channel(fs.as_ref()).expect("well-formed archive");
                    let dumps = channel.channel().map_or(0, |c| u8::from(c.fatal().is_some()));
                    let frames = channel.channel().map_or_else(|| "-".to_owned(), |c| hex(c.frames()));
                    format!(
                        "{{\"channel\":\"{name}\",\"result\":\"{:08x}\",{},\"dumps\":\"{dumps}\",\"frames\":\"{frames}\"}}",
                        channel.result_code(),
                        channel_fields(&channel)
                    )
                }
            }
        })
        .collect()
}

fn ulps(a: f32, b: f32) -> u64 {
    let ordered = |v: f32| {
        let magnitude = i64::from(v.to_bits() & 0x7FFF_FFFF);
        if v.is_sign_negative() {
            -magnitude
        } else {
            magnitude
        }
    };
    ordered(a).abs_diff(ordered(b))
}

fn field<'a>(line: &'a str, key: &str) -> &'a str {
    let tag = format!("\"{key}\":\"");
    let start = line
        .find(&tag)
        .map(|i| i + tag.len())
        .unwrap_or_else(|| panic!("{line}: no {key}"));
    let end = line[start..].find('"').expect("closing quote") + start;
    &line[start..end]
}

fn words(hex_text: &str) -> Vec<f32> {
    unhex(hex_text)
        .chunks(4)
        .map(|c| f32::from_le_bytes(c.try_into().expect("whole words")))
        .collect()
}

#[test]
fn anim_channels() {
    let lines = channel_lines();
    let name = "anim_channels.jsonl";
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite(name, &lines);
        return;
    }
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
    for (index, (want, got)) in committed.iter().zip(&lines).enumerate() {
        let at = format!("{name} line {}", index + 1);
        if !field(want, "channel").starts_with("axis") || want.contains("\"create\"") {
            assert_eq!(*want, got.as_str(), "{at}");
            continue;
        }
        let strip = |l: &str| l.replace(field(l, "frames"), "");
        assert_eq!(strip(want), strip(got), "{at}: fields other than frames");
        let (w, g) = (words(field(want, "frames")), words(field(got, "frames")));
        assert_eq!(w.len(), g.len(), "{at}: frame words");
        for (i, (w, g)) in w.iter().zip(&g).enumerate() {
            assert!(
                ulps(*w, *g) <= AXIS_ULPS || (w - g).abs() <= AXIS_ABS,
                "{at}: word {i}: committed {:08x}, port {:08x}",
                w.to_bits(),
                g.to_bits()
            );
        }
    }
}

#[test]
fn anim_scripts() {
    let host = host();
    let mut lines = Vec::new();
    for archive in input_lines("anim/scripts.list") {
        let fs = open_archive(&host, &archive);
        let Ok(mut fs) = fs else {
            lines.push(format!(
                "{{\"archive\":\"{archive}\",\"create\":\"{}\"}}",
                genresult(fs.unwrap_err())
            ));
            continue;
        };
        lines.push(format!("{{\"archive\":\"{archive}\",\"create\":\"0\"}}"));
        let mut enum_fs = fs.clone();
        let names = AnimScript::enum_fs_scripts(&mut enum_fs).expect("well-formed archive");
        lines.push(match names {
            None => "{\"enum\":\"-1\"}".to_owned(),
            Some(names) => format!(
                "{{\"enum\":\"{}\"}}",
                names.iter().map(|n| hex(n)).collect::<Vec<_>>().join(",")
            ),
        });
        match AnimScript::open(&mut fs).expect("well-formed archive") {
            None => lines.push("{\"script\":\"0\"}".to_owned()),
            Some(script) => {
                lines.push(format!(
                    "{{\"script\":\"1\",\"entries\":\"{}\"}}",
                    script.len()
                ));
                script_lines(&script, Frames::Hex, &mut lines);
            }
        }
    }
    check_suite("anim_scripts.jsonl", &lines);
}

#[test]
fn anim_add_names() {
    let host = host();
    let mut lines = Vec::new();
    for spec in input_lines("anim/add.list") {
        let mut parts = spec.split('|');
        let archive = parts.next().expect("archive");
        let mut fs = open_archive(&host, archive).expect("fixture archive");
        lines.push(format!("{{\"archive\":\"{archive}\"}}"));
        let mut script = AnimScript::new();
        for (call, names) in parts.enumerate() {
            let names = name_list(names);
            let refs: Vec<Option<&[u8]>> = names.iter().map(|n| n.as_deref()).collect();
            let (result, dumps) = match script
                .add_names(&mut fs, &refs)
                .expect("well-formed archive")
            {
                None => (0xFFFF_FFFF_u32, 0),
                Some(warnings) => (0, warnings.len()),
            };
            lines.push(format!(
                "{{\"call\":\"{call}\",\"result\":\"{result:08x}\",\"dumps\":\"{dumps}\"}}"
            ));
        }
        lines.push(format!("{{\"entries\":\"{}\"}}", script.len()));
        script_lines(&script, Frames::Hex, &mut lines);
    }
    check_suite("anim_add_names.jsonl", &lines);
}

/// Deterministic byte mutations of the fixture archives: loading never
/// panics, whatever the archive holds.
#[test]
fn anim_mutations_do_not_panic() {
    let mut state = 0x1234_5678_u32;
    let mut next = move || {
        state = state.wrapping_mul(1_103_515_245).wrapping_add(12_345);
        state >> 8
    };
    for archive in [
        "anim/channels.utf",
        "anim/script_full.utf",
        "anim/script_compact.utf",
    ] {
        let original = read_fixture(archive);
        for round in 0..600 {
            let mut bytes = original.clone();
            let flips = 1 + round % 8;
            for _ in 0..flips {
                let at = next() as usize % bytes.len();
                bytes[at] = match next() % 4 {
                    0 => 0,
                    1 => 0xFF,
                    _ => (next() & 0xFF) as u8,
                };
            }
            let Ok(mut fs) = UtfFileSystem::from_bytes(bytes, archive.as_bytes()) else {
                continue;
            };
            let _ = AnimScript::open(&mut fs.clone());
            let _ = AnimScript::enum_fs_scripts(&mut fs.clone());
            let names: [Option<&[u8]>; 3] = [Some(b"Walk"), Some(b"Open"), Some(b"Idle")];
            let _ = AnimScript::new().add_names(&mut fs, &names);
            if let Ok(found) = fs.find(b"*") {
                for item in found.iter().take(64) {
                    if let Ok(child) =
                        fs.create_child(&FsDesc::read(&item.data.name).with_found(item.token))
                    {
                        let _ = load_channel(child.as_ref());
                    }
                }
            }
        }
    }
}

#[test]
fn data_dump_anim() {
    let Some(dirs) = support::data::data_dirs("anim") else {
        return;
    };
    let host = Host::new(&dirs.root);
    let mut covered = Vec::new();
    let mut dumps = Vec::new();
    let mut errors = Vec::new();
    let (mut entries, mut tracks, mut channels, mut failed) = (0usize, 0usize, 0usize, 0usize);
    for file in support::data::list_files(&dirs.root) {
        if support::data::magic(&dirs.root, &file) != b"UTF " {
            continue;
        }
        let Ok(mut fs) = open_archive(&host, &file) else {
            continue;
        };
        let script = match AnimScript::open(&mut fs) {
            Ok(Some(script)) => script,
            Ok(None) => continue,
            Err(e) => {
                covered.push(file.clone());
                errors.push(format!("{file}: {e}"));
                dumps.push((file, vec!["{\"archive\":\"error\"}".to_owned()]));
                continue;
            }
        };
        covered.push(file.clone());
        entries += script.len();
        for (_, entry) in script.entries() {
            tracks += entry.tracks.len();
            for channel in entry.tracks.iter().filter_map(|t| t.channel.as_ref()) {
                channels += 1;
                if channel.outcome.is_err() {
                    failed += 1;
                }
            }
        }
        let mut lines = vec![format!(
            "{{\"archive\":\"{file}\",\"entries\":\"{}\"}}",
            script.len()
        )];
        script_lines(&script, Frames::Hash, &mut lines);
        dumps.push((file, lines));
    }
    let mut report = support::data::Report::new("anim", &dirs);
    report.file_list(&covered);
    for (file, lines) in &dumps {
        report.check(file, lines);
    }
    report.note(format!(
        "{} archives with an Animation node, {} parse; {entries} animations, {tracks} tracks, {channels} channels, {failed} channel loads failed",
        covered.len(),
        covered.len() - errors.len()
    ));
    for error in &errors {
        report.note(error.clone());
    }
    assert!(
        errors.is_empty(),
        "anim: {} archives do not parse",
        errors.len()
    );
    report.finish();
}
