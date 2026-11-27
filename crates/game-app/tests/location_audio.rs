//! A room's audio on entry (`ApplyLocationAudio`, P5.5 and P5.9) on the
//! original data (`outdir/`, decision O15) against the C++ client's
//! `+oracle` file `audio.jsonl` (decision O53).
//!
//! `location_audio_equals_the_cpp_client` (`OUTPOST_CLIENT_ORACLE_DIR`):
//! for each `locaudio` record of the C++ run, the port reads the room of
//! that location from the base list and applies its audio over the
//! client's sound layer with the data's sound tables and `SoundCFG.ini`
//! and no streamer, as the hosted runner's client runs without sound; its
//! records equal the C++ records that follow, numbered on from the first.
//! Without the variable it prints a notice; it also lists the run's
//! `snd` records by method.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use game_app::audio::{Logged, SoundLayer};
use game_app::base::location::apply_location_audio;
use game_app::base::room_data::RoomData;
use game_app::oracle::audio::AudioLog;
use game_sim::data::Reports;
use game_sim::data::config::{DataConfig, GameData};
use neuron_client::audio::client::config::SoundOptions;
use neuron_client::audio::client::{ClientSound, SoundEnv, WavFiles};
use neuron_client::audio::mixer::Mixer;
use neuron_client::engine::sound::SilentDevice;
use neuron_client::scene_script::ScriptModule;
use neuron_core::hash::eq_ignore_case;
use neuron_core::vfs::Host;

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// No wavs: nothing here plays.
struct NoWavs;

impl WavFiles for NoWavs {
    fn read(&self, _: &[u8], _: Option<&[u8]>) -> Option<Vec<u8>> {
        None
    }
}

fn env<'a>(mixer: &'a mut Mixer<u32>, device: &'a mut SilentDevice) -> SoundEnv<'a> {
    SoundEnv {
        mixer,
        device,
        files: &NoWavs,
        options: SoundOptions::default(),
        now: 0,
    }
}

fn uint_field(line: &str, key: &str) -> Option<u32> {
    let start = line.find(&format!("\"{key}\":"))? + key.len() + 3;
    line[start..]
        .split([',', '}'])
        .next()
        .and_then(|v| v.parse().ok())
}

fn str_field<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let start = line.find(&format!("\"{key}\":\""))? + key.len() + 4;
    line[start..].split('"').next()
}

#[test]
fn location_audio_equals_the_cpp_client() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let root = outdir();
    let Some(config) = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini") else {
        println!("notice: outdir/ is missing: nothing was compared");
        return;
    };
    let host = Host::new(root.join("data"));
    let mut data = GameData::new();
    data.load(&host, &config, None);

    // The client's sound layer as the data load leaves it.
    let mut mixer = Mixer::new();
    let mut device = SilentDevice;
    let mut sound = ClientSound::new();
    for entry in &config.entries {
        if eq_ignore_case(&entry.key, b"sounds") {
            let file = host.read_file(&entry.value).expect("a sound table");
            sound.load_sounds(&mut env(&mut mixer, &mut device), &file.data, false);
        }
    }
    let cfg = host
        .read_file(b"audio\\SoundCFG.ini")
        .expect("SoundCFG.ini");
    sound.config.load(&cfg.data);

    let text = std::fs::read_to_string(dir.join("audio.jsonl")).expect("audio.jsonl");
    let cpp: Vec<&str> = text.lines().collect();
    let mut by_method: BTreeMap<&str, usize> = BTreeMap::new();
    for l in &cpp {
        if let Some(m) = str_field(l, "m") {
            *by_method.entry(m).or_default() += 1;
        }
    }
    println!(
        "audio.jsonl: {} records; snd by method: {by_method:?}",
        cpp.len()
    );

    let mut module = ScriptModule::new();
    let mut reports = Reports::default();
    let mut rooms = 0;
    for (i, head) in cpp.iter().enumerate() {
        if !head.contains("\"rec\":\"locaudio\"") {
            continue;
        }
        let loc = uint_field(head, "loc").expect("a location");
        let frame = uint_field(head, "frame").expect("a frame");
        let mut keep = head.contains("\"keep\":true");
        let room = data
            .bases
            .bases
            .iter()
            .flat_map(|b| b.rooms.iter().flatten())
            .find(|r| r.room_id == loc)
            .unwrap_or_else(|| panic!("location {loc} is not a room of the base list"));
        let room_data = RoomData::read(&host, &room.file, &mut module, &mut reports);
        let first = cpp
            .get(i + 1)
            .and_then(|l| uint_field(l, "seq"))
            .expect("a call");
        let mut log = AudioLog::after(first - 1);
        let mut layer = SoundLayer {
            sound: &mut sound,
            env: env(&mut mixer, &mut device),
        };
        let mut logged = Logged {
            inner: &mut layer,
            log: &mut log,
            frame,
        };
        apply_location_audio(&mut logged, &room_data, &mut keep);
        let expected = &cpp[i + 1..(i + 1 + log.lines.len()).min(cpp.len())];
        for (k, port) in log.lines.iter().enumerate() {
            assert_eq!(
                expected.get(k).copied(),
                Some(port.as_str()),
                "location {loc} ({}), record {}",
                String::from_utf8_lossy(&room.nickname),
                k + 1
            );
        }
        rooms += 1;
    }
    println!("audio.jsonl: {rooms} rooms' audio equal");
}
