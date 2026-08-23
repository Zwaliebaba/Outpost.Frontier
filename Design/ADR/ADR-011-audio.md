# ADR-011 — Audio: XAudio2 Graph with X3DAudio Spatialisation

**Status:** Accepted · 2026-08-17 (owner directive)
**Depends on:** ADR-001 (planar sim, 3D presentation), ADR-006 (camera), ADR-007 (threads),
ADR-010 (DirectXMath), ADR-012 (config)
**Feeds:** NeuronClient charter, Build Order S15

## Context

Owner directive: **audio is XAudio2 with 3D audio.** Both are Windows SDK components —
XAudio2 2.9 is inbox on Windows 10+ (`xaudio2_9.dll`, no redistributable), and X3DAudio is the
spatialisation helper that computes DSP settings (per-channel volume matrix, LPF coefficients,
reverb send) for a source given an emitter and a listener. Nothing here needs a third-party
library.

The interesting problem is not the API — it is that **the simulation is planar and the camera
is orthographic**, so "where is the listener?" has no obvious answer. An ortho camera has no
eye position; it is a direction and a viewing volume. Getting this wrong produces the classic
RTS audio failure: sounds that pan wrongly, or that ignore zoom entirely.

## Decision

### Graph
1. **One `IXAudio2` instance, one mastering voice**, created at client startup on the Main
   thread (ADR-007), owned by NeuronClient. Sample rate follows the device default; the
   mastering voice's `XAUDIO2_VOICE_DETAILS` + channel mask drive `X3DAudioInitialize`.
2. **Five submix voices** feeding master, matching the categories the settings sheet already
   assumes: `World3D`, `Ambience`, `UI`, `Music`, `Alerts`. Category gains come from the JSON
   config (ADR-012) and are what the settings screen writes. Only `World3D` is spatialised;
   the rest are 2D. The **duck matrix** (alerts ducking world/music) that the settings print
   describes is post-MVP, but it is a per-submix volume ramp on this existing graph — no
   restructuring.

   > **"What the settings screen writes" was not achievable when the settings screen arrived**
   > (N3, 2026-08-23), and the missing piece is one function on this side.
   >
   > `AudioDevice::Create` takes an `AudioSettings` and there is **no setter for a live mixer**.
   > So a volume slider could record a value and could not apply one until the next launch —
   > which `settings.png`'s own header forbids in three words (CHANGES APPLY IMMEDIATELY). N3
   > therefore draws the AUDIO section, **refuses it, and puts that sentence on screen** rather
   > than shipping six sliders that quietly do nothing until you restart.
   >
   > It is a small gap and worth naming precisely so it is not mistaken for a design question:
   > the graph exists, the gains are already config, and what is owed is a
   > `SetGains(const AudioSettings&)` that walks the five submixes and the master and calls
   > `SetVolume` on each. The section goes live the day it lands and needs nothing else — which
   > is also why the print calls this section "free". The duck-strength control the print draws
   > beside the volumes is still post-MVP and still the ramp described above.
3. **Pooled source voices**, not one voice per entity: a fixed pool per source format
   (mono 3D, stereo 2D), MVP caps ~32 concurrent 3D voices and ~16 2D. Allocation is
   priority-then-distance; exhaustion steals the lowest-priority farthest voice. This is
   mandatory, not an optimisation: the entity cap is 1,024 (ADR-004) and no mixer survives
   that many voices, so the policy is designed in from the start.

### Spatialisation — the listener
4. **The listener sits at the camera's focus point on the plane, raised by an audio elevation
   derived from zoom**: `listener.Position = (focus.x, k · orthoHalfHeight, focus.z)` with
   `k ≈ 1`, `OrientFront` = the camera's forward projected onto the plane, `OrientTop` = world
   up. Consequences, all of them wanted: panning the camera moves the audio frame with it;
   **zooming out raises the listener and attenuates everything** (the whole battle recedes);
   left/right panning maps to screen left/right because the listener's basis is the camera's.
   *Rejected alternative:* placing the listener at the ortho camera's nominal eye — it is an
   arbitrary distance away, so attenuation and zoom stop correlating.

   **Positions and orientations go to X3DAudio unmodified.** X3DAudio's coordinate system is
   left-handed, and so is this tree's (ADR-006 §3a) — so `Position`, `Velocity`, `OrientFront`
   and `OrientTop` are the render-space values as they stand. A right-handed tree would have to
   negate `.z` on all four fields of both structures on every update; that conversion does not
   exist here, and no code should be written that reintroduces it. `OrientFront` and
   `OrientTop` must still be orthonormal to within 1e-5, which X3DAudio checks and which the
   camera's basis satisfies by construction.
5. **Emitters are the render positions** (plane position + cosmetic hover height, ADR-001),
   `ChannelCount = 1` — **all spatialised assets are mono**, which X3DAudio requires for
   meaningful positioning. `CurveDistanceScaler` is set in metres to match world units
   (ADR-009), so falloff is authored in real distances.
6. **Doppler is disabled in the MVP** (`DopplerScaler = 0`). Ships move at hundreds of m/s
   while the listener is a detached camera; pitch-shifting everything that flies past reads as
   a bug, not as physics. Revisit with combat/projectile audio, per-emitter.
7. **Only `Calculate` flags we consume are enabled**: matrix + LPF (+ reverb later). Per-frame,
   the client calls `X3DAudioCalculate` once per active 3D voice and applies
   `SetOutputMatrix`/`SetFilterParameters`. Positions come from the same interpolated render
   state the renderer uses, so audio and visuals never disagree (F10 holds for audio too).

### Threading (binds ADR-007)
8. XAudio2 owns its own audio thread(s). **No game or render state is touched from an XAudio2
   callback.** `IXAudio2VoiceCallback` implementations do one thing: push a small event
   (voice finished, buffer end) into a NeuronCore SPSC ring, drained by Main. XAudio2's threads
   register as **external lanes** so they appear in the lane registry and telemetry.
9. Audio work happens in one Main-thread stage per frame, **`AudioUpdate`, immediately after
   Extract**: retire finished voices, start/stop loops, update 3D parameters, apply category
   gains. It is a fifth budget row beside `GAME/EXTRACT/RENDER/UI`.

### Content & events
10. **Assets: RIFF WAV, 16-bit PCM** — mono for anything spatialised, stereo for music/UI —
    under `GameData/Audio/`. A hand-rolled RIFF chunk reader (a few dozen lines; the format is
    four-byte tags and sizes) loads them at boot into memory; streaming is post-MVP and only
    music would need it. No external decoder, no compressed formats in MVP.
11. **A JSON sound bank** (`GameData/Audio/SoundBank.json`, parsed by the ADR-012 parser) maps
    `soundId → { file, category, gain, pitchVariance, maxInstances, cooldownMs, minDistance,
    maxDistance }`. Design tuning is data, not code.
12. **Events are client-side and derived, not replicated.** Two sources only:
    (a) local UI/HUD actions (order issued, order rejected — the bounce already has a visual,
    it gets the audio too); (b) **deltas in replicated state** (ship arrives, engine loop
    intensity from velocity, selection changes). The server sends no audio messages in the
    MVP; when combat lands, discrete events ride the existing snapshot/event path rather than
    a new channel.

### Scope
13. **Audio is not in the MVP playable definition** and must not displace it. Build Order S15
    lands the thin slice — device + graph + category gains from config + one 2D UI cue + one
    3D engine loop with the listener model above — proving the architecture. Sound design,
    ducking, reverb, and streaming are post-MVP.

## Alternatives rejected

- **WASAPI directly** — we would write the mixer, the resampler, and the spatialiser that
  XAudio2/X3DAudio already provide. Rejected.
- **Media Foundation** — playback-oriented (media pipelines, longer latency), not a game mixer.
  Rejected.
- **XACT / third-party middleware (Wwise, FMOD)** — XACT is legacy and its tooling is dead;
  middleware violates the library policy and is enormous for a fleet RTS's needs. Rejected.
- **Per-entity source voices** — dies at the entity cap (§3). Rejected.
- **Stereo 3D assets** — X3DAudio positions channels, not sources; stereo "3D" sounds smear.
  Rejected by making mono a content rule.

## Consequences

- The client gains a small COM-lifetime surface (`IXAudio2`, voices) and one more startup
  failure mode: **no audio device is not fatal** — the client logs, disables the audio system,
  and runs silently.
- Audio correctness depends on the camera focus/zoom, so camera changes (new tiers,
  strategic-map zoom) must update the listener model deliberately; the coupling is documented
  rather than hidden.
- Mono-only spatialised content is a hard authoring constraint, worth stating in any future
  asset pipeline doc.
- `NeuronClientTests` can cover the listener/emitter math and bank parsing headlessly;
  actually hearing it stays a manual checkpoint (no audio device in CI).
