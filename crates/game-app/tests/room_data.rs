//! Rooms' files on the original data (`outdir/`, decision O15) and against
//! the C++ client's `+oracle` file (P5.9, decision O53).
//!
//! - `every_room_reads`: `RoomData::load` of every room of every base reads
//!   its set script; the bars have character placements and the rooms
//!   hotspots.
//! - `room_records_equal_the_cpp_client` (`OUTPOST_CLIENT_ORACLE_DIR`): for
//!   each `room` record of the C++ run's `layout.jsonl` (a room
//!   `LoadLocationBase` loaded), the port reads the room the base list names
//!   and writes its records with the C++ record's frame; every `room*`
//!   record equals the C++'s, in order. The rooms load in the C++ order on
//!   one scene-script module, as the client's. Without the variable it
//!   prints a notice.

use std::path::{Path, PathBuf};

use game_app::base::room_data::RoomData;
use game_app::oracle::room::{RoomName, room_lines};
use game_sim::data::Reports;
use game_sim::data::config::{DataConfig, GameData};
use neuron_client::scene_script::ScriptModule;
use neuron_core::vfs::Host;

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn game_data() -> Option<(Host, GameData)> {
    let root = outdir();
    let config = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini")?;
    let data_host = Host::new(root.join("data"));
    let mut data = GameData::new();
    data.load(&data_host, &config, None);
    Some((data_host, data))
}

#[test]
fn every_room_reads() {
    let Some((host, data)) = game_data() else {
        println!("notice: outdir/ is missing: no rooms");
        return;
    };
    let mut module = ScriptModule::new();
    let mut reports = Reports::default();
    let (mut rooms, mut props, mut chars, mut hotspots, mut scenes) = (0, 0, 0, 0, 0);
    for base in &data.bases.bases {
        for room in base.rooms.iter().flatten() {
            let r = RoomData::read(&host, &room.file, &mut module, &mut reports);
            rooms += 1;
            props += r.props.len();
            chars += r.characters.len();
            hotspots += r.hotspots.len();
            scenes += r.scenes.len();
            for p in &r.characters {
                assert!((0..=3).contains(&p.posture), "{p:?}");
            }
        }
    }
    println!(
        "{rooms} rooms: {props} props, {chars} placements, {hotspots} hotspots, {scenes} scenes, {} reports",
        reports.0.len()
    );
    assert!(rooms > 100);
    assert!(props > rooms, "every room has its mesh");
    assert!(chars > 20 && hotspots > rooms);
}

/// The unsigned number after `"key":`.
fn uint_field(line: &str, key: &str) -> Option<u32> {
    let start = line.find(&format!("\"{key}\":"))? + key.len() + 3;
    line[start..]
        .split([',', '}'])
        .next()
        .and_then(|v| v.parse().ok())
}

#[test]
fn room_records_equal_the_cpp_client() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let Some((host, data)) = game_data() else {
        println!("notice: outdir/ is missing: nothing was compared");
        return;
    };
    let text = std::fs::read_to_string(dir.join("layout.jsonl")).expect("layout.jsonl");
    let cpp: Vec<&str> = text
        .lines()
        .filter(|l| l.contains("\"rec\":\"room"))
        .collect();
    let mut module = ScriptModule::new();
    let mut reports = Reports::default();
    let mut port = Vec::new();
    let mut loaded = 0;
    for head in cpp.iter().filter(|l| l.contains("\"rec\":\"room\"")) {
        let id = uint_field(head, "room").expect("a room id");
        let frame = uint_field(head, "frame").expect("a frame");
        let room = data
            .bases
            .bases
            .iter()
            .flat_map(|b| b.rooms.iter().flatten())
            .find(|r| r.room_id == id)
            .unwrap_or_else(|| panic!("room {id} is not in the base list"));
        let r = RoomData::read(&host, &room.file, &mut module, &mut reports);
        port.extend(room_lines(
            &r,
            RoomName {
                room: room.room_id,
                base: room.base_id,
                nickname: Some(&room.nickname),
                file: Some(&room.file),
            },
            frame,
        ));
        loaded += 1;
    }
    if let Some(i) = cpp.iter().zip(&port).position(|(c, p)| *c != p.as_str()) {
        panic!(
            "record {} differs:\n  C++:  {}\n  port: {}",
            i + 1,
            cpp[i],
            port[i]
        );
    }
    assert_eq!(cpp.len(), port.len(), "the record counts differ");
    assert!(loaded > 0, "the run loaded no room");
    println!(
        "layout.jsonl: {loaded} rooms, {} room records equal",
        port.len()
    );
}
