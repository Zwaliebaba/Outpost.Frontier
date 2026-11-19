//! The startup movies (`PlayStartupMovies` in `Client/engine_init.cpp`;
//! work package P4.15): each of the configured movies plays in turn in the
//! game window before the game starts.
//!
//! The C++ hands each file to the movie library, which plays it through a
//! DirectShow video window and polls every 300 ms (`UpdateMovie`) while the
//! client pumps messages. Here the frames go through the renderer: each due
//! frame is copied into a texture drawn at the movie's placement in the
//! back buffer, and the sound is decoded whole and streamed by the audio
//! device (D330). Shift or Escape stops a movie and every other key-down is
//! swallowed, as the movie's window procedure does; a closed window stops
//! the movies and quits, as `WM_QUIT` does in the C++.

use std::path::PathBuf;
use std::time::Instant;

use neuron_client::input::keyboard::KeyEvent;
use neuron_client::media::{FramePacer, placement, stops_movie, wav_file};
use neuron_client::platform::audio::AudioDevice;
use neuron_client::platform::d3d12::backend::D3d12Backend;
use neuron_client::platform::media::MovieReader;
use neuron_client::platform::window::{InputRouter, Pump, Window};
use neuron_client::render::ff::device::{ClearFlags, Device};
use neuron_client::window::WindowEvent;
use neuron_core::image::PixelFormat;

use crate::startup_config::{MOVIE_SLOTS, StartupConfig};

/// `WM_KEYDOWN`.
const WM_KEYDOWN: u32 = 0x100;

/// The movie's key handling (`MoviePlaybackWndProc`): every key-down is
/// swallowed, and Shift or Escape stops the movie.
#[derive(Default)]
struct SkipKeys {
    stop: bool,
}

impl InputRouter for SkipKeys {
    fn key_event(&mut self, event: &KeyEvent) -> bool {
        if event.message != WM_KEYDOWN {
            return false;
        }
        if stops_movie(event.key) {
            self.stop = true;
        }
        true
    }
}

/// How the movies ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MoviesEnd {
    /// All played (or could not be opened, which skips them).
    Played,
    /// The window closed: the client quits.
    Quit,
}

/// A path from the configuration's bytes (the INI is read as the ANSI code
/// page; the shipped paths are ASCII).
fn path(bytes: &[u8]) -> PathBuf {
    PathBuf::from(String::from_utf8_lossy(bytes).into_owned())
}

/// The vertices of a pre-transformed textured quad (`XYZRHW | TEX1`) over
/// the rectangle, as a triangle strip.
fn quad(left: f32, top: f32, right: f32, bottom: f32) -> Vec<u8> {
    let mut v = Vec::with_capacity(4 * 24);
    for (x, y, u, t) in [
        (left, top, 0.0, 0.0),
        (right, top, 1.0, 0.0),
        (left, bottom, 0.0, 1.0),
        (right, bottom, 1.0, 1.0),
    ] {
        for f in [x, y, 0.5f32, 1.0, u, t] {
            v.extend_from_slice(&f.to_le_bytes());
        }
    }
    v
}

/// Plays the configured movies into `window` through `device`.
pub fn play(
    config: &StartupConfig,
    window: &mut Window,
    device: &mut Device<D3d12Backend>,
    audio: Option<&AudioDevice>,
) -> MoviesEnd {
    for index in 0..MOVIE_SLOTS {
        let Some(file) = config.movie_path(index) else {
            continue;
        };
        if play_one(&path(&file), window, device, audio) == MoviesEnd::Quit {
            return MoviesEnd::Quit;
        }
    }
    MoviesEnd::Played
}

fn play_one(
    file: &std::path::Path,
    window: &mut Window,
    device: &mut Device<D3d12Backend>,
    audio: Option<&AudioDevice>,
) -> MoviesEnd {
    let Ok(mut movie) = MovieReader::open(file) else {
        // The C++ goes on when a movie cannot be played.
        return MoviesEnd::Played;
    };
    let (width, height) = movie.size();
    let Ok(texture) = device
        .textures
        .create(width, height, PixelFormat::A8R8G8B8, 1, 0)
    else {
        return MoviesEnd::Played;
    };
    let stream = audio.and_then(|a| {
        let (format, pcm) = movie.read_audio().ok()??;
        a.streams()
            .create(wav_file(format, &pcm).into(), false, true)
            .ok()
    });
    let viewport = device.viewport();
    let (left, top, w, h) = placement(
        (width as i32, height as i32),
        (viewport.width as i32, viewport.height as i32),
    );
    let vertices = quad(left as f32, top as f32, (left + w) as f32, (top + h) as f32);
    let mut pacer = FramePacer::new();
    let start = Instant::now();
    let mut ended = false;
    let mut result = MoviesEnd::Played;
    let mut keys = SkipKeys::default();
    while !ended && !keys.stop {
        match window.pump_routed(&mut keys) {
            Pump::Quit(..) => {
                result = MoviesEnd::Quit;
                break;
            }
            Pump::Events(events) => {
                if events.contains(&WindowEvent::CloseRequested) {
                    result = MoviesEnd::Quit;
                    break;
                }
            }
        }
        // 100-nanosecond units, as the frames are stamped.
        let now = i64::try_from(start.elapsed().as_nanos() / 100).unwrap_or(i64::MAX);
        let frame = pacer.due(now, || match movie.next_frame() {
            Ok(Some(f)) => Some(f),
            _ => {
                ended = true;
                None
            }
        });
        if let Some(frame) = frame
            && let Ok((layout, bytes)) = device.textures.lock(texture, 0)
        {
            let row = width as usize * 4;
            for y in 0..height as usize {
                let at = y * layout.pitch as usize;
                if let (Some(dst), Some(src)) = (
                    bytes.get_mut(at..at + row),
                    frame.bgra.get(y * row..(y + 1) * row),
                ) {
                    dst.copy_from_slice(src);
                }
            }
            let _ = device.textures.unlock(texture, 0);
        }
        device.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        device.set_stage_texture(0, Some(texture));
        let _ = device.draw_up(5, neuron_client::render::ff::fvf::Fvf(0x104), &vertices, 4);
        device.set_stage_texture(0, None);
        device.present();
    }
    if let (Some(a), Some(id)) = (audio, stream) {
        a.streams().destroy(id);
    }
    let _ = device.textures.destroy(texture);
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(message: u32, key: u32) -> KeyEvent {
        KeyEvent {
            kind: neuron_client::input::keyboard::KeyEventKind::Key,
            key,
            down: message == WM_KEYDOWN,
            repeat: false,
            modifiers: neuron_client::input::keyboard::Modifiers(0),
            message,
            wparam: key,
            lparam: 0,
        }
    }

    #[test]
    fn key_downs_are_swallowed_and_shift_or_escape_stop() {
        let mut k = SkipKeys::default();
        assert!(k.key_event(&key(WM_KEYDOWN, 0x41)));
        assert!(!k.stop);
        // Key-ups pass through.
        assert!(!k.key_event(&key(0x101, 0x1b)));
        assert!(!k.stop);
        assert!(k.key_event(&key(WM_KEYDOWN, 0x1b)));
        assert!(k.stop);
        let mut k = SkipKeys::default();
        k.key_event(&key(WM_KEYDOWN, 0x10));
        assert!(k.stop);
    }

    #[test]
    fn the_quad_is_a_strip_over_the_rectangle() {
        let v = quad(10.0, 20.0, 110.0, 70.0);
        assert_eq!(v.len(), 4 * 24);
        let f = |i: usize| f32::from_le_bytes(v[i * 4..i * 4 + 4].try_into().unwrap());
        // Vertex 3: right, bottom, z, rhw, u = 1, v = 1.
        assert_eq!(
            [f(18), f(19), f(20), f(21), f(22), f(23)],
            [110.0, 70.0, 0.5, 1.0, 1.0, 1.0]
        );
    }
}
