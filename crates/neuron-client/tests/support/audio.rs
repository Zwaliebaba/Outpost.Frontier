//! The P4.10 audio suites: the inputs of `tests/fixtures/oracle/audio/`
//! rendered through the port in the line formats of
//! `CppSource/Oracle/oracle_audio.cpp`, and the Tier B comparison of those
//! lines.
//!
//! The logarithm of the attenuation and the power of the pitch multiplier
//! go through the Rust standard library where the reference uses `fyl2x`
//! and the CRT `pow` (decision T11), so the fields that depend on them
//! compare within a bound ([`Bound`]); everything else compares exactly.

use std::collections::{BTreeMap, HashMap};
use std::fmt::Write as _;
use std::fs;
use std::path::Path;

use neuron_client::audio::config::profile_float;
use neuron_client::audio::listener::{Listener, Pose};
use neuron_client::audio::mix::{quake_distance, software_attenuation};
use neuron_client::audio::mixer::{
    Archetype, ArchetypeId, Instance, MixState, Mixer, NO_ARCHETYPE, PcmFormat, SoundSource,
    SourceTable, StartTiming, VoiceBackend, VoiceCaps, VoiceId, software_settings, start_offset,
};
use neuron_client::audio::reverb::EaxPreview;
use neuron_client::render::ff::defaults::profile_number;

use super::{check_suite, input_lines, unhex};

/// How a field compares with the committed value.
#[derive(Clone, Copy, Debug)]
pub enum Bound {
    /// The text is equal.
    Exact,
    /// Concatenated f32 words within this many units in the last place.
    F32Ulps(u64),
    /// One f64 (16 hex digits) within this many units in the last place.
    F64Ulps(u64),
    /// A decimal integer within this distance.
    Int(i64),
}

fn pairs(line: &str) -> Vec<(String, String)> {
    let inner = line
        .strip_prefix("{\"")
        .and_then(|l| l.strip_suffix("\"}"))
        .unwrap_or_else(|| panic!("not a vector line: {line}"));
    inner
        .split("\",\"")
        .map(|pair| {
            let (key, value) = pair
                .split_once("\":\"")
                .unwrap_or_else(|| panic!("bad pair {pair}"));
            (key.to_owned(), value.to_owned())
        })
        .collect()
}

fn ordered(magnitude: u64, negative: bool) -> i128 {
    if negative {
        -i128::from(magnitude)
    } else {
        i128::from(magnitude)
    }
}

fn within(bound: Bound, want: &str, got: &str) -> bool {
    if want == got {
        return true;
    }
    if want == "-" || got == "-" {
        return false;
    }
    match bound {
        Bound::Exact => false,
        Bound::Int(limit) => match (want.parse::<i64>(), got.parse::<i64>()) {
            (Ok(w), Ok(g)) => (w - g).abs() <= limit,
            _ => false,
        },
        Bound::F32Ulps(limit) => {
            want.len() == got.len()
                && want.len().is_multiple_of(8)
                && (0..want.len()).step_by(8).all(|i| {
                    let w = u32::from_str_radix(&want[i..i + 8], 16).unwrap_or(0);
                    let g = u32::from_str_radix(&got[i..i + 8], 16).unwrap_or(0);
                    let key = |b: u32| ordered(u64::from(b & 0x7FFF_FFFF), b >> 31 != 0);
                    (key(w) - key(g)).unsigned_abs() <= u128::from(limit)
                })
        }
        Bound::F64Ulps(limit) => {
            let parse = |t: &str| u64::from_str_radix(t, 16).unwrap_or(0);
            let key = |b: u64| ordered(b & 0x7FFF_FFFF_FFFF_FFFF, b >> 63 != 0);
            (key(parse(want)) - key(parse(got))).unsigned_abs() <= u128::from(limit)
        }
    }
}

/// Compares `lines` with the committed suite field by field within
/// `bound(key)`, or writes the suite when `OUTPOST_BLESS_VECTORS` is set.
pub fn check_suite_within(name: &str, lines: &[String], bound: impl Fn(&str) -> Bound) {
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite(name, lines);
        return;
    }
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        let at = format!("{name} line {}", index + 1);
        let (want, got) = (pairs(want), pairs(got));
        let keys = |p: &[(String, String)]| p.iter().map(|(k, _)| k.clone()).collect::<Vec<_>>();
        assert_eq!(keys(&want), keys(&got), "{at}: fields");
        for ((key, w), (_, g)) in want.iter().zip(&got) {
            assert!(
                within(bound(key), w, g),
                "{at}: {key}: committed {w}, port {g}"
            );
        }
    }
}

fn hexf(token: &str) -> f32 {
    f32::from_bits(u32::from_str_radix(token, 16).unwrap_or_else(|e| panic!("{token}: {e}")))
}

fn opt_f(token: &str) -> Option<f32> {
    (token != "-").then(|| hexf(token))
}

fn vec3(token: &str) -> [f32; 3] {
    let parts: Vec<f32> = token.split(',').map(hexf).collect();
    [parts[0], parts[1], parts[2]]
}

fn opt_vec(token: &str) -> Option<[f32; 3]> {
    (token != "-").then(|| vec3(token))
}

fn dec<T: std::str::FromStr>(token: &str) -> T
where
    T::Err: std::fmt::Debug,
{
    token.parse().unwrap_or_else(|e| panic!("{token}: {e:?}"))
}

/// A float as the harness writes it: every NaN as `7fc00000`.
fn f(value: f32) -> String {
    let bits = if value.is_nan() {
        0x7FC0_0000
    } else {
        value.to_bits()
    };
    format!("{bits:08x}")
}

fn v(values: &[f32]) -> String {
    values.iter().map(|&x| f(x)).collect()
}

struct Line(String);

impl Line {
    fn new(input: &str) -> Line {
        Line(format!("{{\"in\":\"{input}\""))
    }

    fn field(mut self, key: &str, value: impl std::fmt::Display) -> Line {
        let _ = write!(self.0, ",\"{key}\":\"{value}\"");
        self
    }

    fn end(mut self) -> String {
        self.0.push('}');
        self.0
    }
}

fn or_dash<T: std::fmt::Display>(value: Option<T>) -> String {
    value.map_or_else(|| "-".to_owned(), |v| v.to_string())
}

/// The scripted play key of the harness (`FOracleSource`).
#[derive(Clone, Debug)]
pub struct Source {
    active: bool,
    spatial: bool,
    looping: bool,
    arch: ArchetypeId,
    start: u32,
    atten: f32,
    pitch: Option<f32>,
    pan: Option<i32>,
    pos: Option<[f32; 3]>,
    min: Option<f32>,
    max: Option<f32>,
    restamps: u32,
}

impl Default for Source {
    fn default() -> Self {
        Source {
            active: true,
            spatial: false,
            looping: false,
            arch: NO_ARCHETYPE,
            start: 0,
            atten: 0.0,
            pitch: None,
            pan: Some(0),
            pos: Some([0.0; 3]),
            min: None,
            max: None,
            restamps: 0,
        }
    }
}

impl SoundSource for Source {
    fn restamp(&mut self) {
        self.restamps += 1;
    }
    fn start_tick(&mut self) -> u32 {
        self.start
    }
    fn is_active(&self) -> bool {
        self.active
    }
    fn is_3d(&self) -> bool {
        self.spatial
    }
    fn is_looping(&self) -> bool {
        self.looping
    }
    fn archetype(&self) -> ArchetypeId {
        self.arch
    }
    fn attenuation(&self) -> f32 {
        self.atten
    }
    fn pitch(&self) -> Option<f32> {
        self.pitch
    }
    fn pan(&self) -> Option<i32> {
        self.pan
    }
    fn position(&self) -> Option<[f32; 3]> {
        self.pos
    }
    fn min_distance(&self) -> Option<f32> {
        self.min
    }
    fn max_distance(&self) -> Option<f32> {
        self.max
    }
}

/// What the harness's buffer records (`FOracleBuffer`).
#[derive(Clone, Debug, Default)]
struct Record {
    pan: Option<i32>,
    volume: Option<i32>,
    frequency: Option<u32>,
    position: Option<u32>,
    plays: u32,
    playing: bool,
}

#[derive(Default)]
struct Recorder(HashMap<VoiceId, Record>);

impl Recorder {
    fn at(&mut self, voice: VoiceId) -> &mut Record {
        self.0.entry(voice).or_default()
    }
}

impl VoiceBackend for Recorder {
    fn create(&mut self, voice: VoiceId, _: ArchetypeId, _: &Archetype) -> bool {
        self.0.insert(voice, Record::default());
        true
    }
    fn release(&mut self, voice: VoiceId) {
        self.0.remove(&voice);
    }
    fn set_pan(&mut self, voice: VoiceId, pan: i32) {
        self.at(voice).pan = Some(pan);
    }
    fn set_volume(&mut self, voice: VoiceId, volume: i32) {
        self.at(voice).volume = Some(volume);
    }
    fn set_frequency(&mut self, voice: VoiceId, frequency: u32) {
        self.at(voice).frequency = Some(frequency);
    }
    fn set_position(&mut self, voice: VoiceId, byte: u32) {
        self.at(voice).position = Some(byte);
    }
    fn is_playing(&mut self, voice: VoiceId) -> bool {
        self.at(voice).playing
    }
    fn play(&mut self, voice: VoiceId, _: bool) -> bool {
        let r = self.at(voice);
        r.playing = true;
        r.plays += 1;
        true
    }
}

/// The pan and volume a harness buffer holds before the mixer sets them.
const UNSET: i32 = 0x7FFF_FFFF;

fn archetype(rate: u16, block_align: u16) -> Archetype {
    Archetype {
        format: PcmFormat {
            channels: 1,
            bytes_per_sample: 0,
            rate,
            block_align,
        },
        loop_start: 0,
        loop_end: 0,
        frames: 0,
        duration_ms: 0,
        gain: 0.0,
        flags: 0,
        buffer_flags: 0,
    }
}

/// `audio_apply.jsonl`.
pub fn apply_lines() -> Vec<String> {
    input_lines("audio/apply.list")
        .iter()
        .map(|line| {
            let t: Vec<&str> = line.split(' ').collect();
            let mut listener = Listener {
                coord_scale: dec(t[0]),
                ..Listener::default()
            };
            listener.set_orientation(vec3(t[1]), vec3(t[2]));
            listener.set_position(vec3(t[3]));
            listener.distance_factor = hexf(t[4]);
            listener.rolloff_factor = hexf(t[5]);
            let mut arch = archetype(dec(t[8]), 2);
            arch.gain = hexf(t[7]);
            let state = MixState {
                listener: &listener,
                master_gain: hexf(t[6]),
                max_3d_pan: hexf(t[9]),
            };
            let source = Source {
                arch: 1,
                spatial: t[10] != "0",
                pos: opt_vec(t[11]),
                min: opt_f(t[12]),
                max: opt_f(t[13]),
                atten: hexf(t[14]),
                pitch: opt_f(t[15]),
                pan: (t[16] != "-").then(|| dec(t[16])),
                ..Source::default()
            };
            let mut pose = if source.spatial {
                listener.pose(source.pos, source.min, source.max)
            } else {
                Pose::default()
            };
            let dist = quake_distance(pose.dist_sq);
            let atten = (source.spatial && pose.dist_sq > pose.min_dist_sq).then(|| {
                f(software_attenuation(
                    pose.min_distance,
                    dist,
                    listener.distance_factor,
                    listener.rolloff_factor,
                    source.atten,
                ))
            });
            let dist_sq = pose.dist_sq;
            let settings = software_settings(
                &state,
                &source,
                Some(&arch),
                &mut pose,
                t[17] != "0",
                t[18] != "0",
            );
            Line::new(line)
                .field("dist_sq", f(dist_sq))
                .field("dist", f(dist))
                .field("atten", or_dash(atten))
                .field("dir", v(&pose.direction))
                .field("pan", or_dash(settings.pan.filter(|&p| p != UNSET)))
                .field("freq", or_dash(settings.frequency))
                .field("volume", or_dash(settings.volume.filter(|&v| v != UNSET)))
                .end()
        })
        .collect()
}

/// The bounds of `audio_apply.jsonl`: the attenuation (and the volume
/// from it) passes through the logarithm, the frequency through `pow`.
pub fn apply_bound(key: &str) -> Bound {
    match key {
        "atten" => Bound::F32Ulps(1),
        "volume" | "freq" => Bound::Int(1),
        _ => Bound::Exact,
    }
}

/// `audio_pitch.jsonl`.
pub fn pitch_lines() -> Vec<String> {
    use neuron_client::audio::mix::{pitch_to_multiplier, scaled_duration_ms};
    input_lines("audio/pitch.list")
        .iter()
        .map(|line| {
            let t: Vec<&str> = line.split(' ').collect();
            let pitch = opt_f(t[0]);
            let flags = u32::from_str_radix(t[2], 16).unwrap_or(0);
            let mult = pitch.map(|p| {
                let m = pitch_to_multiplier(p);
                let bits = if m.is_nan() {
                    0x7FF8_0000_0000_0000
                } else {
                    m.to_bits()
                };
                format!("{bits:016x}")
            });
            let duration = scaled_duration_ms(dec(t[1]), flags & 2 != 0, pitch);
            Line::new(line)
                .field("mult", or_dash(mult))
                .field("duration", duration)
                .end()
        })
        .collect()
}

/// The bounds of `audio_pitch.jsonl`.
pub fn pitch_bound(key: &str) -> Bound {
    match key {
        "mult" => Bound::F64Ulps(1),
        "duration" => Bound::Int(1),
        _ => Bound::Exact,
    }
}

/// `audio_start.jsonl`.
pub fn start_lines() -> Vec<String> {
    input_lines("audio/start.list")
        .iter()
        .map(|line| {
            let t: Vec<&str> = line.split(' ').collect();
            let mut source = Source {
                arch: 1,
                start: dec(t[1]),
                looping: t[5] != "0",
                pitch: opt_f(t[6]),
                ..Source::default()
            };
            let mut arch = archetype(dec(t[7]), dec(t[8]));
            arch.frames = dec(t[9]);
            arch.loop_start = dec(t[10]);
            arch.loop_end = dec(t[11]);
            let mut backend = Recorder::default();
            backend.at(1).playing = t[12] != "0";
            let mut instance = Instance {
                key: 0,
                voice: 1,
                caps: VoiceCaps::default(),
                frequency_control: t[4] != "0",
                started: t[3] != "0",
                pose: Pose::default(),
                loop_bytes: None,
                last_start_tick: dec(t[2]),
                rank: 0,
                pan: 0,
                volume: 0,
            };
            let timing = StartTiming {
                now: dec(t[0]),
                never_set_start_pos: t[13] != "0",
            };
            let lookup = |id: ArchetypeId| (id == 1).then_some(arch);
            let play = start_offset(timing, &lookup, &mut instance, &mut source, &mut backend);
            Line::new(line)
                .field("play", u8::from(play))
                .field("pos", or_dash(backend.at(1).position))
                .field("last", instance.last_start_tick)
                .field("freq", u8::from(instance.frequency_control))
                .field("restamps", source.restamps)
                .end()
        })
        .collect()
}

/// The bounds of `audio_start.jsonl`: a pitched start position passes
/// through `pow`, and one millisecond more is `rate / 1000` frames more.
pub fn start_bound(key: &str) -> Bound {
    match key {
        "pos" => Bound::Int(256),
        _ => Bound::Exact,
    }
}

/// `audio_listener.jsonl`.
pub fn listener_lines() -> Vec<String> {
    input_lines("audio/listener.list")
        .iter()
        .map(|line| {
            let t: Vec<&str> = line.split(' ').collect();
            let mut l = Listener {
                coord_scale: dec(t[1]),
                right_handed: t[2] != "0",
                ..Listener::default()
            };
            let next = if t[0] == "m" {
                let m: Vec<f32> = t[3].split(',').map(hexf).collect();
                let m: [f32; 9] = m.try_into().unwrap_or_else(|_| panic!("matrix: {line}"));
                l.set_orientation_from_matrix(&m);
                4
            } else {
                l.set_orientation(vec3(t[3]), vec3(t[4]));
                5
            };
            l.set_position(vec3(t[next]));
            l.set_velocity(vec3(t[next + 1]));
            let (front, up) = l.orientation_space();
            Line::new(line)
                .field("front", v(&l.front))
                .field("up", v(&l.up))
                .field("pos", v(&l.position))
                .field("vel", v(&l.velocity))
                .field("matrix", v(&l.orientation_matrix()))
                .field("space", v(&front) + &v(&up))
                .field("get_pos", v(&l.get_position()))
                .field("get_vel", v(&l.get_velocity()))
                .end()
        })
        .collect()
}

/// `audio_range.jsonl`.
pub fn range_lines() -> Vec<String> {
    input_lines("audio/range.list")
        .iter()
        .map(|line| {
            let t: Vec<&str> = line.split(' ').collect();
            let mut mixer = Mixer::<u32>::new();
            let mut arch = archetype(22050, 2);
            arch.flags = u32::from_str_radix(t[3], 16).unwrap_or(0);
            arch.duration_ms = dec(t[4]);
            let id = mixer.insert_archetype(arch);
            mixer.listener.coord_scale = dec(t[7]);
            mixer.listener.set_position(vec3(t[8]));
            let mut source = Source {
                arch: if t[11] != "0" { id } else { NO_ARCHETYPE },
                looping: t[1] != "0",
                spatial: t[2] != "0",
                pitch: opt_f(t[5]),
                start: dec(t[6]),
                pos: Some(vec3(t[9])),
                max: opt_f(t[10]),
                ..Source::default()
            };
            let allow = mixer.allow_create(&source);
            let expired = mixer.one_shot_expired(dec(t[0]), &mut source);
            Line::new(line)
                .field("allow", u8::from(allow))
                .field("expired", u8::from(expired))
                .end()
        })
        .collect()
}

/// The bounds of `audio_range.jsonl`: a pitched duration passes through
/// `pow`, which can move an expiry by a millisecond.
pub fn range_bound(_: &str) -> Bound {
    Bound::Exact
}

/// `audio_ini.jsonl`.
pub fn ini_lines() -> Vec<String> {
    input_lines("audio/ini.list")
        .iter()
        .map(|line| {
            let value = unhex(line);
            Line::new(line)
                .field("int", format!("{:08x}", profile_number(&value)))
                .field("float", f(profile_float(&value)))
                .end()
        })
        .collect()
}

/// `audio_eax.jsonl`.
pub fn eax_lines() -> Vec<String> {
    input_lines("audio/eax.list")
        .iter()
        .map(|line| {
            let t: Vec<&str> = line.split(' ').collect();
            let p = EaxPreview::clamped(dec(t[0]), hexf(t[1]), hexf(t[2]), hexf(t[3]));
            let mut arch = archetype(22050, 2);
            arch.set_gain(hexf(t[4]));
            Line::new(line)
                .field("env", p.environment)
                .field("vol", f(p.volume))
                .field("decay", f(p.decay_time))
                .field("damp", f(p.damping))
                .field("gain", f(arch.gain))
                .end()
        })
        .collect()
}

struct Table(BTreeMap<i64, Source>);

impl SourceTable<i64> for Table {
    fn source(&mut self, key: i64) -> &mut dyn SoundSource {
        self.0
            .get_mut(&key)
            .unwrap_or_else(|| panic!("no source {key}"))
    }
}

fn keys(token: &str) -> Vec<i64> {
    if token == "-" {
        Vec::new()
    } else {
        token.split(',').map(dec).collect()
    }
}

/// `audio_tick.jsonl`: the scripted sessions of `audio/tick.list` through
/// [`Mixer::tick`], with the harness's settings: frequency control forced
/// to software (`FORCE_FREQ_CONTROL_TO_SW` keeps its default without
/// `StartupDevice`) and no pan on the voices of 3D archetypes (3D buffers
/// without a 3D interface).
pub fn tick_lines() -> Vec<String> {
    let mut out = Vec::new();
    let mut case = -1i32;
    let mut mixer = Mixer::<i64>::new();
    let mut table = Table(BTreeMap::new());
    let mut backend = Recorder::default();
    let mut archetypes: Vec<ArchetypeId> = Vec::new();
    for line in input_lines("audio/tick.list") {
        let t: Vec<&str> = line.split(' ').collect();
        match t[0] {
            "case" => {
                case += 1;
                mixer = Mixer::new();
                mixer.pan_3d_voices = false;
                table = Table(BTreeMap::new());
                backend = Recorder::default();
                archetypes.clear();
                mixer.max_channels = dec(t[1]);
                mixer.master_gain = hexf(t[2]);
                mixer.listener.distance_factor = hexf(t[3]);
                mixer.listener.rolloff_factor = hexf(t[4]);
                mixer.listener.coord_scale = dec(t[5]);
                mixer.max_3d_pan = hexf(t[6]);
            }
            "listener" => {
                mixer.listener.set_orientation(vec3(t[1]), vec3(t[2]));
                mixer.listener.set_position(vec3(t[3]));
            }
            "arch" => {
                let flags = u32::from_str_radix(t[1], 16).unwrap_or(0);
                let format = PcmFormat {
                    channels: dec(t[2]),
                    bytes_per_sample: dec(t[3]),
                    rate: dec(t[4]),
                    block_align: dec(t[5]),
                };
                let frames: u32 = dec(t[6]);
                let byte_count = frames * u32::from(format.block_align);
                let mut arch =
                    Archetype::from_pcm(format, byte_count, dec(t[7]), dec(t[8]), flags, true)
                        .unwrap_or_else(|e| panic!("{line}: {e}"));
                arch.set_gain(hexf(t[9]));
                let id = mixer.insert_archetype(arch);
                archetypes.push(id);
                out.push(format!(
                    "{{\"case\":\"{case}\",\"arch\":\"{id}\",\"flags\":\"{:x}\",\"buffer\":\"{:x}\",\
                     \"frames\":\"{}\",\"duration\":\"{}\"}}",
                    arch.flags, arch.buffer_flags, arch.frames, arch.duration_ms
                ));
            }
            "src" => {
                let index: i64 = dec(t[4]);
                let source = Source {
                    spatial: t[2] != "0",
                    looping: t[3] != "0",
                    arch: usize::try_from(index).map_or(NO_ARCHETYPE, |i| archetypes[i]),
                    ..Source::default()
                };
                table.0.insert(dec(t[1]), source);
            }
            "set" => {
                let source = table
                    .0
                    .get_mut(&dec(t[1]))
                    .unwrap_or_else(|| panic!("{line}"));
                let value = t[3];
                match t[2] {
                    "active" => source.active = value != "0",
                    "start" => source.start = dec(value),
                    "looping" => source.looping = value != "0",
                    "atten" => source.atten = hexf(value),
                    "pitch" => source.pitch = opt_f(value),
                    "pan" => source.pan = (value != "-").then(|| dec(value)),
                    "pos" => source.pos = opt_vec(value),
                    "min" => source.min = opt_f(value),
                    "max" => source.max = opt_f(value),
                    other => panic!("unknown field {other}"),
                }
            }
            "tick" => {
                let now: u32 = dec(t[1]);
                for key in keys(t[3]) {
                    let voices: Vec<VoiceId> = mixer
                        .instances()
                        .iter()
                        .filter(|i| i.key == key)
                        .map(|i| i.voice)
                        .collect();
                    for voice in voices {
                        backend.at(voice).playing = false;
                    }
                }
                mixer.replace_attached(&keys(t[2]), &mut backend);
                mixer.tick(now, &mut table, &mut backend);
                out.push(format!(
                    "{{\"case\":\"{case}\",\"tick\":\"{now}\",\"instances\":\"{}\"}}",
                    mixer.instances().len()
                ));
                for instance in mixer.instances() {
                    let r = backend.0.get(&instance.voice).cloned().unwrap_or_default();
                    out.push(format!(
                        "{{\"case\":\"{case}\",\"tick\":\"{now}\",\"key\":\"{}\",\"rank\":\"{}\",\
                         \"pan\":\"{}\",\"volume\":\"{}\",\"freq\":\"{}\",\"pos\":\"{}\",\
                         \"plays\":\"{}\"}}",
                        instance.key,
                        instance.rank,
                        or_dash(r.pan),
                        or_dash(r.volume),
                        or_dash(r.frequency),
                        or_dash(r.position),
                        r.plays
                    ));
                }
            }
            other => panic!("unknown command {other}"),
        }
    }
    out
}

/// The bounds of `audio_tick.jsonl`.
pub fn tick_bound(key: &str) -> Bound {
    match key {
        "volume" | "freq" => Bound::Int(1),
        "pos" => Bound::Int(256),
        _ => Bound::Exact,
    }
}
