//! The archetype voices: one XAudio2 source voice per mixer voice,
//! playing the archetype's samples in place.
//!
//! A released voice is stopped, flushed and kept in a pool by format, so
//! the mixer's frequent releases and creations do not wait for
//! `DestroyVoice`; a pooled voice is reused only once it reports no queued
//! buffer, and until then it keeps the samples it last played alive. The
//! pool is bounded; voices beyond it are destroyed.

use std::collections::HashMap;
use std::sync::Arc;

use super::{Core, SourceVoice};
use crate::audio::listener::Listener;
use crate::audio::mix::{frequency_ratio, volume_to_amplitude};
use crate::audio::mixer::{Archetype, ArchetypeId, Emitter, PcmFormat, VoiceCaps, VoiceId};
use crate::audio::spatial::{
    emitter_inputs, listener_inputs, pan_matrix, rolloff_gain, voice_ratio, volume_curve,
};
use crate::audio::voice::buffer_plan;

/// Pooled voices kept at most.
const POOL_LIMIT: usize = 48;

/// What a voice can be reused for: the format and whether it sends to the
/// reverb (voices of 3D archetypes do, as EAX acted on 3D buffers).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Key {
    format: PcmFormat,
    spatial: bool,
}

/// A voice the mixer holds. The voice is declared before the samples, so
/// it is destroyed before they are released.
struct Live {
    voice: SourceVoice,
    samples: Arc<[u8]>,
    key: Key,
    archetype: Archetype,
    caps: VoiceCaps,
    /// The byte the next play starts at.
    position: u32,
    /// A buffer was submitted and not flushed since.
    submitted: bool,
    /// Whether that buffer loops.
    looping: bool,
    /// `Start` was called (and not `Stop`).
    started: bool,
    pitch_ratio: f32,
    doppler: f32,
    max_ratio: f32,
    pan: i32,
    /// The volume last set, as an amplitude.
    amplitude: f32,
    /// The output matrix last set (empty: XAudio2's default).
    matrix: Vec<f32>,
}

/// A voice in the pool, with the samples it may still read.
struct Pooled {
    voice: SourceVoice,
    _samples: Arc<[u8]>,
    key: Key,
}

/// The archetype voices of the device.
pub(crate) struct ArchetypeVoices {
    core: Arc<Core>,
    samples: HashMap<ArchetypeId, Arc<[u8]>>,
    live: HashMap<VoiceId, Live>,
    pool: Vec<Pooled>,
}

impl ArchetypeVoices {
    pub(crate) fn new(core: &Arc<Core>) -> ArchetypeVoices {
        ArchetypeVoices {
            core: core.clone(),
            samples: HashMap::new(),
            live: HashMap::new(),
            pool: Vec::new(),
        }
    }

    pub(crate) fn set_samples(&mut self, archetype: ArchetypeId, samples: Arc<[u8]>) {
        self.samples.insert(archetype, samples);
    }

    pub(crate) fn remove_samples(&mut self, archetype: ArchetypeId) {
        self.samples.remove(&archetype);
    }

    pub(crate) fn live(&self) -> usize {
        self.live.len()
    }

    /// A stopped voice for `key`: an idle pooled one, else a new one.
    fn voice_for(&mut self, key: Key) -> Option<SourceVoice> {
        if let Some(i) = self
            .pool
            .iter()
            .position(|p| p.key == key && p.voice.state().0 == 0)
        {
            return Some(self.pool.swap_remove(i).voice);
        }
        SourceVoice::new(&self.core, key.format, key.spatial).ok()
    }

    pub(crate) fn create(&mut self, id: VoiceId, archetype: ArchetypeId, data: &Archetype) -> bool {
        let Some(samples) = self.samples.get(&archetype).cloned() else {
            return false;
        };
        let bytes = u64::from(data.frames) * u64::from(data.format.block_align);
        if (samples.len() as u64) < bytes || bytes == 0 {
            return false;
        }
        let caps = VoiceCaps::from_buffer_flags(data.buffer_flags);
        let key = Key {
            format: data.format,
            spatial: caps.spatial,
        };
        let Some(voice) = self.voice_for(key) else {
            return false;
        };
        let mut live = Live {
            voice,
            samples,
            key,
            archetype: *data,
            caps,
            position: 0,
            submitted: false,
            looping: false,
            started: false,
            pitch_ratio: 1.0,
            doppler: 1.0,
            max_ratio: super::max_ratio(u32::from(data.format.rate)),
            pan: 0,
            amplitude: 1.0,
            matrix: Vec::new(),
        };
        // A new buffer plays at full volume, its own rate and the centre.
        live.voice.set_volume(1.0);
        live.voice.set_ratio(1.0);
        if !caps.spatial {
            Self::apply_pan(&self.core, &mut live);
        }
        if let Some(old) = self.live.insert(id, live) {
            self.retire(old);
        }
        true
    }

    /// Stops and flushes a voice and pools it (or destroys it when the
    /// pool is full).
    fn retire(&mut self, live: Live) {
        live.voice.stop();
        live.voice.flush();
        if self.pool.len() < POOL_LIMIT {
            self.pool.push(Pooled {
                voice: live.voice,
                _samples: live.samples,
                key: live.key,
            });
        }
    }

    pub(crate) fn release(&mut self, id: VoiceId) {
        if let Some(live) = self.live.remove(&id) {
            self.retire(live);
        }
    }

    fn apply_pan(core: &Core, live: &mut Live) {
        let matrix = pan_matrix(
            u32::from(live.key.format.channels),
            core.output_channels,
            core.channel_mask,
            live.pan,
        );
        Self::apply_matrix(live, matrix);
    }

    fn apply_matrix(live: &mut Live, matrix: Vec<f32>) {
        live.voice.set_matrix(&matrix);
        live.matrix = matrix;
    }

    /// Stops the voice and empties its queue. When the flush has not
    /// emptied it yet (XAudio2 may keep the buffer it is playing until its
    /// next pass), the voice is replaced by a new one with the same
    /// settings, so the old buffer cannot play on; the old voice is
    /// destroyed, which waits for the audio thread.
    fn clear_queue(core: &Arc<Core>, live: &mut Live) -> bool {
        live.voice.stop();
        live.voice.flush();
        live.submitted = false;
        live.started = false;
        if live.voice.state().0 == 0 {
            return true;
        }
        let Ok(voice) = SourceVoice::new(core, live.key.format, live.key.spatial) else {
            return false;
        };
        live.voice = voice;
        live.voice.set_volume(live.amplitude);
        Self::apply_ratio(live);
        if !live.matrix.is_empty() {
            live.voice.set_matrix(&live.matrix);
        }
        true
    }

    pub(crate) fn set_pan(&mut self, id: VoiceId, pan: i32) {
        if let Some(live) = self.live.get_mut(&id) {
            live.pan = pan;
            if !live.caps.spatial {
                Self::apply_pan(&self.core, live);
            }
        }
    }

    pub(crate) fn set_volume(&mut self, id: VoiceId, volume: i32) {
        if let Some(live) = self.live.get_mut(&id) {
            live.amplitude = volume_to_amplitude(volume);
            live.voice.set_volume(live.amplitude);
        }
    }

    fn apply_ratio(live: &Live) {
        live.voice
            .set_ratio(voice_ratio(live.pitch_ratio, live.doppler, live.max_ratio));
    }

    pub(crate) fn set_frequency(&mut self, id: VoiceId, frequency: u32) {
        if let Some(live) = self.live.get_mut(&id) {
            live.pitch_ratio = frequency_ratio(frequency, live.archetype.format.rate);
            Self::apply_ratio(live);
        }
    }

    /// Queues the samples from the voice's position.
    fn submit(live: &mut Live, looping: bool) -> bool {
        let Some(plan) = buffer_plan(&live.archetype, live.caps, live.position, looping) else {
            return false;
        };
        let bytes = live.archetype.frames as usize * usize::from(live.archetype.format.block_align);
        let Some(data) = live.samples.get(..bytes) else {
            return false;
        };
        // SAFETY: `data` is part of `live.samples`, which the voice's owner
        // keeps until the voice is destroyed or, pooled, reports no queued
        // buffer.
        let ok = unsafe { live.voice.submit(data, plan.play_begin, plan.loop_region) }.is_ok();
        live.submitted = ok;
        live.looping = looping;
        ok
    }

    /// `SetCurrentPosition`: the next play starts at `byte`; a voice that
    /// is playing moves there at once.
    pub(crate) fn set_position(&mut self, id: VoiceId, byte: u32) {
        let Some(live) = self.live.get_mut(&id) else {
            return;
        };
        live.position = byte;
        if !live.submitted {
            return;
        }
        let playing = live.started && live.voice.state().0 > 0;
        let looping = live.looping;
        if Self::clear_queue(&self.core, live) && playing && Self::submit(live, looping) {
            live.started = live.voice.start().is_ok();
        }
    }

    pub(crate) fn is_playing(&mut self, id: VoiceId) -> bool {
        self.live
            .get(&id)
            .is_some_and(|live| live.started && live.voice.state().0 > 0)
    }

    /// `Play`: a voice with nothing queued, or queued with the other loop
    /// mode, gets its buffer from its position; then it starts.
    pub(crate) fn play(&mut self, id: VoiceId, looping: bool) -> bool {
        let Some(live) = self.live.get_mut(&id) else {
            return false;
        };
        let queued = live.voice.state().0;
        if !live.submitted || queued == 0 || live.looping != looping {
            if queued > 0 && !Self::clear_queue(&self.core, live) {
                return false;
            }
            if !Self::submit(live, looping) {
                live.started = false;
                return false;
            }
        }
        live.started = live.voice.start().is_ok();
        live.started
    }

    /// The X3DAudio matrix and Doppler factor of a 3D voice; without
    /// X3DAudio, the centre with the distance curve's gain.
    pub(crate) fn set_emitter(&mut self, id: VoiceId, listener: &Listener, emitter: &Emitter) {
        let Some(live) = self.live.get_mut(&id) else {
            return;
        };
        let channels = u32::from(live.key.format.channels);
        let (scaler, curve) = volume_curve(
            emitter.min_distance,
            emitter.max_distance,
            listener.rolloff_factor,
        );
        let core = &self.core;
        match &core.x3d {
            Some(x3d) => {
                let calculated = x3d.calculate(
                    &listener_inputs(listener),
                    &emitter_inputs(listener, emitter),
                    channels,
                    scaler,
                    &curve,
                );
                live.doppler = calculated.doppler;
                Self::apply_matrix(live, calculated.matrix);
            }
            None => {
                let offset: [f64; 3] = core::array::from_fn(|i| {
                    f64::from(emitter.position[i]) - f64::from(listener.position[i])
                });
                let distance =
                    (offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]).sqrt();
                let gain = rolloff_gain(
                    distance as f32,
                    crate::audio::spatial::clamp_distance(emitter.min_distance),
                    crate::audio::spatial::clamp_distance(emitter.max_distance),
                    listener.rolloff_factor,
                );
                let matrix: Vec<f32> =
                    pan_matrix(channels, core.output_channels, core.channel_mask, 0)
                        .into_iter()
                        .map(|g| g * gain)
                        .collect();
                live.doppler = 1.0;
                Self::apply_matrix(live, matrix);
            }
        }
        Self::apply_ratio(live);
    }
}
