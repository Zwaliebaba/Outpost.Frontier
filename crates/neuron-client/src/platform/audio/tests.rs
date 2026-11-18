//! The device on the machine's default output. A build machine may have no
//! audio endpoint (or no XAudio2): then `CreateMasteringVoice` (or loading
//! the library) fails and each test prints a notice and returns.

use std::sync::Arc;
use std::time::{Duration, Instant};

use super::*;
use crate::audio::mixer::{Archetype, PcmFormat, archetype_flags};
use crate::audio::reverb::{ENVIRONMENT_COUNT, EaxPreview, reverb_parameters};

fn device() -> Option<AudioDevice> {
    match AudioDevice::new(1.0) {
        Ok(device) => Some(device),
        Err(e) if e.is_missing_device() || e.is_missing_library() => {
            println!("no audio output on this machine ({e}): the device test is skipped");
            None
        }
        Err(e) => panic!("the audio device failed: {e}"),
    }
}

/// `seconds` of a 440 Hz tone, 16-bit mono at 22050 Hz.
fn tone(seconds: f64) -> (PcmFormat, Vec<u8>) {
    let rate = 22050u16;
    let frames = (seconds * f64::from(rate)) as usize;
    let mut samples = Vec::with_capacity(frames * 2);
    for i in 0..frames {
        let t = i as f64 / f64::from(rate);
        let s = (t * 440.0 * std::f64::consts::TAU).sin() * 8000.0;
        samples.extend_from_slice(&(s as i16).to_le_bytes());
    }
    let format = PcmFormat {
        channels: 1,
        bytes_per_sample: 2,
        rate,
        block_align: 2,
    };
    (format, samples)
}

fn wait_until(limit: Duration, mut done: impl FnMut() -> bool) -> bool {
    let start = Instant::now();
    while start.elapsed() < limit {
        if done() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    done()
}

#[test]
fn the_default_device_opens() {
    let Some(device) = device() else { return };
    assert!(device.output_channels() >= 1);
    assert!(device.sample_rate() > 0);
    println!(
        "{device:?}, X3DAudio {}, reverb {}",
        device.has_x3daudio(),
        device.has_reverb()
    );
}

#[test]
fn archetype_voices_play_move_and_end() {
    let Some(mut device) = device() else { return };
    let (format, samples) = tone(0.3);
    let byte_count = samples.len() as u32;
    let frames = byte_count / 2;
    let flat = Archetype::from_pcm(
        format,
        byte_count,
        0,
        frames,
        archetype_flags::PAN | archetype_flags::FREQUENCY,
        true,
    )
    .unwrap();
    let spatial = Archetype::from_pcm(
        format,
        byte_count,
        0,
        frames,
        archetype_flags::SPATIAL,
        true,
    )
    .unwrap();
    let samples: Arc<[u8]> = samples.into();
    device.set_samples(1, samples.clone());
    device.set_samples(2, samples);

    // A pan voice: settings, a start offset, play once, end.
    assert!(device.create(1, 1, &flat));
    device.set_volume(1, -600);
    device.set_pan(1, 2000);
    device.set_frequency(1, 30000);
    device.set_position(1, 2 * 1000);
    assert!(device.play(1, false));
    assert!(device.is_playing(1));
    assert!(wait_until(Duration::from_secs(2), || !device.is_playing(1)));
    // Played again from the start after it ended.
    device.set_position(1, 0);
    assert!(device.play(1, false));
    assert!(device.is_playing(1));
    device.release(1);
    assert!(!device.is_playing(1));

    // A looping 3D voice through X3DAudio (or the fallback).
    assert!(device.create(2, 2, &spatial));
    let mut listener = Listener::default();
    listener.set_velocity([0.0, 0.0, 10.0]);
    let emitter = Emitter {
        position: [30.0, 0.0, 5.0],
        velocity: [0.0, 0.0, -5.0],
        min_distance: 10.0,
        max_distance: 500.0,
        mute_at_max_distance: true,
    };
    device.set_emitter(2, &listener, &emitter);
    assert!(device.play(2, true));
    std::thread::sleep(Duration::from_millis(400));
    assert!(device.is_playing(2), "a looping voice keeps playing");
    // Moving a playing voice keeps it playing.
    device.set_position(2, 2 * 500);
    assert!(device.is_playing(2));
    device.release(2);

    // Released voices are pooled and reused.
    assert_eq!(device.live_voices(), 0);
    assert!(device.create(3, 1, &flat));
    assert!(!device.create(4, 99, &flat), "no samples for the archetype");
    device.release(3);
}

#[test]
fn every_reverb_environment_is_accepted() {
    let Some(device) = device() else { return };
    if !device.has_reverb() {
        println!("no reverb effect: skipped");
        return;
    }
    for environment in 0..ENVIRONMENT_COUNT as i32 {
        let preview = EaxPreview::clamped(environment, 0.7, 2.5, 0.8);
        device
            .set_reverb(Some(&reverb_parameters(&preview)))
            .unwrap_or_else(|e| panic!("environment {environment}: {e}"));
    }
    let extreme = EaxPreview::clamped(10, 1.0, 100.0, 2.0);
    device
        .set_reverb(Some(&reverb_parameters(&extreme)))
        .unwrap();
    device.set_reverb(None).unwrap();
}

/// A PCM WAV file of `samples`.
fn pcm_wav(format: PcmFormat, samples: &[u8]) -> Vec<u8> {
    let rate = u32::from(format.rate);
    let block = u32::from(format.block_align);
    let mut fmt = Vec::new();
    fmt.extend_from_slice(&1u16.to_le_bytes());
    fmt.extend_from_slice(&format.channels.to_le_bytes());
    fmt.extend_from_slice(&rate.to_le_bytes());
    fmt.extend_from_slice(&(rate * block).to_le_bytes());
    fmt.extend_from_slice(&format.block_align.to_le_bytes());
    fmt.extend_from_slice(&(format.bytes_per_sample * 8).to_le_bytes());
    let mut body = b"WAVE".to_vec();
    for (id, payload) in [(b"fmt ", &fmt[..]), (b"data", samples)] {
        body.extend_from_slice(id);
        body.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        body.extend_from_slice(payload);
    }
    let mut file = b"RIFF".to_vec();
    file.extend_from_slice(&(body.len() as u32).to_le_bytes());
    file.extend_from_slice(&body);
    file
}

#[test]
fn streams_play_to_their_end() {
    let Some(device) = device() else { return };
    let (format, samples) = tone(0.6);
    let streams = device.streams();
    let id = streams
        .create(pcm_wav(format, &samples).into(), false, true)
        .unwrap();
    streams.set_volume(id, -1200);
    streams.set_pan(id, -3000);
    assert!(
        wait_until(Duration::from_secs(1), || streams.status(id) == 1),
        "the stream starts playing"
    );
    assert!(
        wait_until(Duration::from_secs(5), || streams.status(id) == 4),
        "the stream finishes"
    );
    assert!(streams.destroy(id));
    assert_eq!(streams.status(id), 0);
}

#[test]
fn shipped_music_streams_loop() {
    let Some(device) = device() else { return };
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../outdir/data/AUDIO/MUSIC/music_newtokyo.wav");
    let Ok(bytes) = std::fs::read(&path) else {
        println!("{} is missing: skipped", path.display());
        return;
    };
    let streams = device.streams();
    let id = streams.create(bytes.into(), true, false).unwrap();
    assert_eq!(streams.status(id), 2);
    assert!(streams.start(id));
    std::thread::sleep(Duration::from_millis(500));
    assert_eq!(streams.status(id), 1);
    assert!(streams.stop(id));
    assert_eq!(streams.status(id), 2);
    assert!(streams.start(id));
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(streams.status(id), 1);
    assert!(streams.destroy(id));
}
