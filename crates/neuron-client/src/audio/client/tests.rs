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
