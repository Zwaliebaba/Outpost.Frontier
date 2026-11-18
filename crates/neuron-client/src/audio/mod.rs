//! Audio (work package P4.10, decision T4): the platform-free part of
//! `CppSource/SoundManager` and `CppSource/SoundStreamer`.
//!
//! The reference mixes through DirectSound buffers. For voices without a
//! 3D buffer it computes attenuation, pan, pitch and volume itself, and it
//! decides every tick which of the client's ranked sources hold a voice.
//! That arithmetic and logic are ported here in the reference's operation
//! order (decision T11); XAudio2 source voices (`platform::audio`) play
//! the result through a [`mixer::VoiceBackend`].
//!
//! - [`mix`]: distance, attenuation, pitch, frequency, volume and pan, and
//!   their conversion to the linear factors XAudio2 takes.
//! - [`listener`]: the listener and the listener-relative pose of a source.
//! - [`mixer`]: archetypes, the attached list, the ranked voice cap and the
//!   per-tick voice settings (`MixerTick`).
//! - [`config`]: the `[AudioProperties]` and `[SoundManager]` INI keys.
//! - [`reverb`]: the EAX preview settings and their I3DL2 reverb mapping.
//! - [`stream`]: the streamer's buffer sizes, prefill, refill schedule and
//!   gapless trim, and the streamer itself over a voice queue.
//! - [`wav`]: the RIFF/WAVE walk of the two loaders.
//! - [`decode`]: archetype and stream decoding; MPEG Layer III through
//!   `symphonia` (decision T13, divergence D273).
//! - [`spatial`]: the X3DAudio inputs of 3D voices and the pan matrix of
//!   2D voices (owner decision O26, divergences D275 and D277).
//! - [`voice`]: how a device voice queues an archetype and measures a
//!   stream's queue.
//!
//! The XAudio2 device that plays all this is `platform::audio` (Windows
//! only).
//!
//! Not ported, per decision T4:
//!
//! - Hardware and software buffer placement (divergence D222): the
//!   channel budget from free hardware buffers, `MaybeMigrateHwSw`, the
//!   location flags of `Play` and their retries, `use2DHW`, `use3DHW`,
//!   `useNoHW`, `createAll2DInSoftware`, `ALLOW_HW_TO_SW`, `ALLOW_SW_TO_HW`
//!   and `FORCE_FREQ_CONTROL_TO_SW` (read, without effect).
//! - EAX passthrough (divergence D223): the property-set calls of
//!   `SetEaxPreview`, `MixerApplyEax` and the raw `EaxGet*`/`EaxSet*`
//!   slots. The settings map to a reverb ([`reverb`], divergence D224).
//! - The 3D algorithm option (`3D_SW_Algorithm`, read and kept,
//!   divergence D223) and the DirectSound 3D buffers it configured: voices
//!   of 3D archetypes hand their pose to the device, which spatialises
//!   them with X3DAudio ([`mixer::Spatial`], owner decision O26,
//!   divergence D220).
//! - Device selection (`deviceID`), the speaker table, the primary buffer,
//!   the notify thread (native loop regions replace it), the streamer's
//!   ring buffer (a voice queue replaces it, divergence D227), and the
//!   Audio Compression Manager (`symphonia` replaces it, divergence D273).

pub mod config;
pub mod decode;
pub mod listener;
pub mod mix;
pub mod mixer;
pub mod reverb;
pub mod spatial;
pub mod stream;
pub mod voice;
pub mod wav;
