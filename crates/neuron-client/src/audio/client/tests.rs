use std::collections::BTreeMap;
use std::sync::Arc;

use super::*;
use crate::audio::listener::Listener;
use crate::audio::mixer::{Emitter, Spatial, VoiceId};

/// A device that keeps what the mixer last set on each voice.
#[derive(Default)]
struct Device {
    voices: BTreeMap<VoiceId, (i32, i32, bool)>,
    samples: BTreeMap<ArchetypeId, usize>,
    streams: Option<Streams>,
}

impl VoiceBackend for Device {
    fn create(&mut self, voice: VoiceId, _: ArchetypeId, _: &Archetype) -> bool {
        self.voices.insert(voice, (0, 0, false));
        true
    }
    fn release(&mut self, voice: VoiceId) {
        self.voices.remove(&voice);
    }
    fn set_pan(&mut self, voice: VoiceId, pan: i32) {
        self.voices.get_mut(&voice).unwrap().0 = pan;
    }
    fn set_volume(&mut self, voice: VoiceId, volume: i32) {
        self.voices.get_mut(&voice).unwrap().1 = volume;
    }
    fn set_frequency(&mut self, _: VoiceId, _: u32) {}
    fn set_position(&mut self, _: VoiceId, _: u32) {}
    fn is_playing(&mut self, voice: VoiceId) -> bool {
        self.voices.get(&voice).is_some_and(|v| v.2)
    }
    fn play(&mut self, voice: VoiceId, _: bool) -> bool {
        self.voices.get_mut(&voice).unwrap().2 = true;
        true
    }
    fn set_emitter(&mut self, _: VoiceId, _: &Listener, _: &Emitter) {}
}

impl SoundDevice for Device {
    fn set_samples(&mut self, archetype: ArchetypeId, samples: Arc<[u8]>) {
        self.samples.insert(archetype, samples.len());
    }
    fn remove_samples(&mut self, archetype: ArchetypeId) {
        self.samples.remove(&archetype);
    }
    fn streamer(&mut self) -> Option<&mut dyn streamed::ClientStreamer> {
        self.streams
            .as_mut()
            .map(|s| s as &mut dyn streamed::ClientStreamer)
    }
}

/// Wavs of `frames` zero frames of 16-bit mono at 22050 Hz, by name
/// `f<frames>.wav`; an archive's leaves the same way.
struct Files;

impl WavFiles for Files {
    fn read(&self, file: &[u8], _: Option<&[u8]>) -> Option<Vec<u8>> {
        let name = std::str::from_utf8(file).ok()?;
        let frames: u32 = name.strip_prefix('f')?.strip_suffix(".wav")?.parse().ok()?;
        let data = frames * 2;
        let mut out = Vec::new();
        for w in [0x4646_4952, 36 + data, 0x4556_4157, 0x2074_6D66, 16] {
            out.extend_from_slice(&u32::to_le_bytes(w));
        }
        out.extend_from_slice(&[1, 0, 1, 0]);
        out.extend_from_slice(&22050u32.to_le_bytes());
        out.extend_from_slice(&44100u32.to_le_bytes());
        out.extend_from_slice(&[2, 0, 16, 0]);
        out.extend_from_slice(&0x6174_6164u32.to_le_bytes());
        out.extend_from_slice(&data.to_le_bytes());
        out.resize(out.len() + data as usize, 0);
        Some(out)
    }
}

const SOUNDS: &[u8] =
    b"[Sound]\nnickname = click\ntype = interface\nfile = f2205.wav\nis_2d = true\n\
    [Sound]\nnickname = hum\nfile = f22050.wav\nrange = 10, 200\n\
    [Sound]\nnickname = music\ntype = music\nfile = f22050.wav\nstreamer = true\n";

fn env<'a>(mixer: &'a mut Mixer<u32>, device: &'a mut Device, now: u32) -> SoundEnv<'a> {
    SoundEnv {
        mixer,
        device,
        files: &Files,
        options: SoundOptions::default(),
        now,
    }
}

#[test]
fn a_flat_one_shot_plays_through_the_mixer_and_retires() {
    let mut mixer = Mixer::new();
    mixer.max_channels = 4;
    let mut device = Device::default();
    let mut sound = ClientSound::new();
    sound.load_sounds(&mut env(&mut mixer, &mut device, 0), SOUNDS, false);
    let click = sound.sound(b"click").unwrap();
    let key = PlayKey {
        attenuation: -6.0,
        ..PlayKey::default()
    };
    let (ok, handle) = sound.play(
        &mut env(&mut mixer, &mut device, 1000),
        click,
        Some(&key),
        true,
    );
    assert!(ok);
    let handle = handle.unwrap();
    // The archetype was made on the play, 2D with pan control.
    assert_eq!(device.samples.len(), 1);
    assert!(mixer.archetype(1).is_some());
    sound.tick(
        &mut env(&mut mixer, &mut device, 1000),
        None,
        0.0,
        &NoAttachments,
    );
    assert_eq!(sound.ranked().collect::<Vec<_>>(), [handle]);
    let voice = mixer.instance(handle.0).unwrap().voice;
    assert_eq!(device.voices[&voice].1, -600);
    assert_eq!(sound.source(handle).unwrap().start_tick, 1000);
    // 100 ms later it has ended: retired, its voice and archetype released.
    sound.tick(
        &mut env(&mut mixer, &mut device, 1101),
        None,
        0.0,
        &NoAttachments,
    );
    assert!(sound.playing().next().is_none());
    assert!(mixer.instances().is_empty());
    assert!(device.samples.is_empty());
    assert_eq!(sound.alive(), 1);
    sound.release(&mut env(&mut mixer, &mut device, 1101), handle);
    assert_eq!(sound.alive(), 0);
}

#[test]
fn positioned_sources_rank_by_distance() {
    let mut mixer = Mixer::new();
    mixer.max_channels = 1;
    mixer.spatial = Spatial::Software;
    let mut device = Device::default();
    let mut sound = ClientSound::new();
    sound.load_sounds(&mut env(&mut mixer, &mut device, 0), SOUNDS, false);
    let hum = sound.sound(b"hum").unwrap();
    let far = PlayKey {
        flags: key_flags::SPATIAL | key_flags::LOOP,
        position: [0.0, 0.0, 150.0],
        ..PlayKey::default()
    };
    let near = PlayKey {
        position: [0.0, 0.0, 20.0],
        ..far
    };
    let out = PlayKey {
        position: [0.0, 0.0, 250.0],
        ..far
    };
    let (_, a) = sound.play(&mut env(&mut mixer, &mut device, 0), hum, Some(&far), true);
    let (_, b) = sound.play(&mut env(&mut mixer, &mut device, 0), hum, Some(&near), true);
    let (_, c) = sound.play(&mut env(&mut mixer, &mut device, 0), hum, Some(&out), true);
    sound.tick(
        &mut env(&mut mixer, &mut device, 10),
        None,
        0.0,
        &NoAttachments,
    );
    let (a, b, c) = (a.unwrap(), b.unwrap(), c.unwrap());
    assert_eq!(sound.ranked().collect::<Vec<_>>(), [b, a]);
    assert_eq!(sound.source(c).unwrap().mixer_score, source::NO_SCORE);
    // About 10 log10(10 / 20) for the near one (one Newton step of the
    // inverse square root).
    let score = sound.source(b).unwrap().mixer_score;
    assert!((score + 3.0103).abs() < 0.02, "{score}");
    // One channel: only the nearest has a voice.
    assert_eq!(mixer.instances().len(), 1);
    assert_eq!(mixer.instances()[0].key, b.0);
}

#[test]
fn streamed_archetypes_and_ducking() {
    let mut mixer = Mixer::new();
    let mut device = Device::default();
    let mut sound = ClientSound::new();
    sound.load_sounds(&mut env(&mut mixer, &mut device, 0), SOUNDS, false);
    let music = sound.sound(b"music").unwrap();
    assert_eq!(
        sound.play(&mut env(&mut mixer, &mut device, 0), music, None, true),
        (false, None)
    );
    sound.push_ducking();
    sound.tick(
        &mut env(&mut mixer, &mut device, 0),
        None,
        0.1,
        &NoAttachments,
    );
    let d = sound.ducking_db();
    assert!(d < 0.0 && d > -6.0, "{d}");
    sound.tick(
        &mut env(&mut mixer, &mut device, 0),
        None,
        0.2,
        &NoAttachments,
    );
    assert_eq!(sound.ducking_db(), -6.0);
    sound.pop_ducking();
    sound.tick(
        &mut env(&mut mixer, &mut device, 0),
        None,
        1.0,
        &NoAttachments,
    );
    assert_eq!(sound.ducking_db(), 0.0);
}

/// A streamer that keeps its streams' state.
#[derive(Default)]
struct Streams {
    next: u32,
    streams: BTreeMap<u32, Stream>,
}

/// A stream's bytes, looping, started, volume and destroyed.
#[derive(Clone, Debug, PartialEq)]
struct Stream {
    bytes: usize,
    looping: bool,
    started: bool,
    volume: i32,
    destroyed: bool,
}

impl streamed::ClientStreamer for Streams {
    fn create(&mut self, file: Vec<u8>, looping: bool) -> Option<u32> {
        self.next += 1;
        let stream = Stream {
            bytes: file.len(),
            looping,
            started: false,
            volume: 0,
            destroyed: false,
        };
        self.streams.insert(self.next, stream);
        Some(self.next)
    }
    fn destroy(&mut self, stream: u32) {
        self.streams.get_mut(&stream).unwrap().destroyed = true;
    }
    fn stop(&mut self, stream: u32) {
        self.streams.get_mut(&stream).unwrap().started = false;
    }
    fn start(&mut self, stream: u32) {
        self.streams.get_mut(&stream).unwrap().started = true;
    }
    fn set_volume(&mut self, stream: u32, volume: i32) {
        self.streams.get_mut(&stream).unwrap().volume = volume;
    }
    fn set_pan(&mut self, _: u32, _: i32) {}
    fn status(&mut self, stream: u32) -> u32 {
        let s = &self.streams[&stream];
        if s.destroyed {
            0
        } else if s.started {
            2
        } else {
            1
        }
    }
}

const STREAMS: &[u8] =
    b"[Sound]\nnickname = music_a\ntype = music\nfile = f100.wav\nstreamer = true\n\
    [Sound]\nnickname = music_b\ntype = music\nfile = f200.wav\nstreamer = true\n\
    [Sound]\nnickname = wind\nfile = f300.wav\n";

fn streaming() -> (Mixer<u32>, Device, ClientSound) {
    let mut mixer = Mixer::new();
    let mut device = Device {
        streams: Some(Streams::default()),
        ..Device::default()
    };
    let mut sound = ClientSound::new();
    sound.load_sounds(&mut env(&mut mixer, &mut device, 0), STREAMS, false);
    (mixer, device, sound)
}

fn stream(device: &Device, id: u32) -> Stream {
    device.streams.as_ref().unwrap().streams[&id].clone()
}

#[test]
fn background_music_fades_in_and_out_through_its_stream() {
    use streamed::Background::{Ambient, Music};
    let (mut mixer, mut device, mut sound) = streaming();
    let a = neuron_core::id::create_id(b"music_a");
    let b = neuron_core::id::create_id(b"music_b");
    // Looping, at the silence: the stream started at -96 dB.
    assert!(sound.set_background(&mut env(&mut mixer, &mut device, 0), Music, a, 1, -1.0));
    let first = sound.background(Music).unwrap();
    let s1 = stream(&device, 1);
    assert_eq!(
        (s1.bytes, s1.looping, s1.started, s1.volume),
        (244, true, true, -9600)
    );
    let s = sound.source(first).unwrap();
    assert_eq!(
        (s.category, s.flags & source::source_flags::MANAGED),
        (category::MUSIC, 0)
    );
    assert_eq!(s.streamed.unwrap().fade_time, 3.0);
    // The same id keeps it.
    assert!(sound.set_background(&mut env(&mut mixer, &mut device, 0), Music, a, 1, -1.0));
    assert_eq!(sound.background(Music), Some(first));
    assert_eq!(device.streams.as_ref().unwrap().streams.len(), 1);
    // Not ranked, no mixer archetype; part-way up after a tick, at 0 dB
    // after the fade.
    let tick = |sound: &mut ClientSound, mixer: &mut Mixer<u32>, device: &mut Device, dt| {
        sound.tick(&mut env(mixer, device, 0), None, dt, &NoAttachments);
    };
    tick(&mut sound, &mut mixer, &mut device, 1.5);
    assert_eq!(sound.ranked().count(), 0);
    assert!(device.samples.is_empty());
    let mid = sound.source(first).unwrap().attenuation_db;
    assert!(mid > -96.0 && mid < 0.0, "{mid}");
    assert_eq!(
        stream(&device, 1).volume,
        streamed::stream_volume(f64::from(mid))
    );
    tick(&mut sound, &mut mixer, &mut device, 1.5);
    assert_eq!(sound.source(first).unwrap().attenuation_db, 0.0);
    assert_eq!(stream(&device, 1).volume, 0);
    // Another id: the first fades out over 2 s and, let go, is retired
    // with its stream destroyed; the second fades in.
    assert!(sound.set_background(&mut env(&mut mixer, &mut device, 0), Music, b, 0, 2.0));
    let second = sound.background(Music).unwrap();
    assert_ne!(second, first);
    assert!(!stream(&device, 2).looping);
    tick(&mut sound, &mut mixer, &mut device, 1.0);
    assert!(!stream(&device, 1).destroyed);
    tick(&mut sound, &mut mixer, &mut device, 1.0);
    assert!(stream(&device, 1).destroyed, "faded out and retired");
    assert_eq!(sound.playing().collect::<Vec<_>>(), [second]);
    // An empty id lets the slot go and answers false.
    assert!(!sound.set_background(&mut env(&mut mixer, &mut device, 0), Music, 0, 0, -1.0));
    assert_eq!(sound.background(Music), None);
    // An ambience that is no streamer still streams (the key's bit 2).
    let wind = neuron_core::id::create_id(b"wind");
    assert!(sound.set_background(&mut env(&mut mixer, &mut device, 0), Ambient, wind, 1, 0.0));
    let ambient = sound.background(Ambient).unwrap();
    assert_eq!(sound.source(ambient).unwrap().category, category::AMBIENT);
    // Stop-all stops both slots' sources and destroys their streams.
    sound.stop_all_except(&mut env(&mut mixer, &mut device, 0), None);
    assert_eq!(
        (sound.background(Music), sound.background(Ambient)),
        (None, None)
    );
    let streams = &device.streams.as_ref().unwrap().streams;
    assert!(streams.values().all(|s| s.destroyed), "{streams:?}");
    assert_eq!(sound.alive(), 0);
}

#[test]
fn background_fades_and_no_streamer() {
    use streamed::Background::Music;
    let (mut mixer, mut device, mut sound) = streaming();
    let a = neuron_core::id::create_id(b"music_a");
    // Without a streamer nothing starts.
    let mut silent = Device::default();
    assert!(!sound.set_background(&mut env(&mut mixer, &mut silent, 0), Music, a, 1, -1.0));
    assert_eq!((sound.background(Music), sound.alive()), (None, 0));
    // A rate: the time is the distance from the total attenuation.
    assert!(sound.set_background(&mut env(&mut mixer, &mut device, 0), Music, a, 1, 0.0));
    let key = sound.background(Music).unwrap();
    sound.masters.music = -10.0;
    sound.fade_background(Music, -50.0, 20.0);
    let s = sound.source(key).unwrap().streamed.unwrap();
    // The total is the silence (-96 - 10, clamped): 46 / 20.
    assert_eq!((s.fade_target, s.fade_time), (-50.0, 2.3));
    // Above 1e8: the ducking's down-by and down-time.
    sound.fade_background(Music, 1e9, 1e9);
    let s = sound.source(key).unwrap().streamed.unwrap();
    assert_eq!((s.fade_target, s.fade_time), sound.config.ducking());
    // The volume's clamps.
    assert_eq!(streamed::stream_volume(-96.5), -9999);
    assert_eq!(streamed::stream_volume(0.5), 0);
    assert_eq!(streamed::stream_volume(-12.345), -1234);
}
