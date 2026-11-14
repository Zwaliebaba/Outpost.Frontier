# Verification: proving "the same as the original"

The original game is the specification and the test oracle. Phase 1 of the [Plan](Plan.md)
instruments it once (the **Recorder**, a BepInEx + HarmonyX plugin) and records the golden corpus;
every check below compares against what the Recorder captured. The corpus stays on the owner's
machine; CI runs only what needs no original data.

## 1. The simulation: bit-exact, tick by tick

The strongest check, and the one the rest of the game stands on.

- **Golden traces.** For each recorded game: the command stream (what was applied at which tick)
  and the checksum after every tick, as the original computes it (`Checksum`, combined from every
  state object's `GetChecksum()`), broken down per state component.
- **`SimRunner`** feeds the command stream to the C++ simulation and compares the checksum at
  every tick. **Pass means equal at every tick of every trace.** There is no tolerance.
- **On a divergence** it reports the first tick and the first component whose checksum differs;
  the Recorder can then dump the original's full state frame at that tick, and `SimRunner` diffs
  it against its own, field by field.
- **Coverage.** The C++ simulation is built with coverage instrumentation in a test configuration;
  functions the corpus never reaches are listed, and the corpus grows (a recorded game that
  exercises them) until the list is empty or each remaining entry is explained.
- **Inputs.** Before any tick runs, the loaded attributes, height maps, nav meshes and scene
  descriptors are compared with the load captures, value by value (`Fixed64` raw values).
- **Core arithmetic.** `Fixed64`, the vector types, `Random64`, the hashes and the .NET-compatible
  collections are unit-tested against vectors the Recorder computes inside the original's Mono
  runtime, including overflow and rounding edge cases.

## 2. Content

The checks in [Asset-Pipeline.md §4](Asset-Pipeline.md), on the whole content set, on every cook:
lossless converters compare bytes or hashes; the others compare counts, durations and
references; a cook fails when content needs something the runtime does not implement (a particle
module, a script node, a component type), and lists it.

## 3. Rendering

- **Reference shots.** The Recorder places the camera at scripted positions in every scene, with
  simulation and animation time frozen, at a fixed resolution and quality setting, and saves the
  frame. The rebuild renders the same positions and times.
- **Comparison.** Per shot, a perceptual difference (SSIM) and a per-pixel difference image.
  Thresholds are set per category (geometry and placement: tight; lighting and GI: looser, R3;
  particles: by eye, since Shuriken's randomness is seeded per instance).
- **Per shader.** During the rewrite (Plan 5.2), each shader is checked on a test scene that uses
  only that shader, so a difference has one cause.

## 4. Animation

Each animated model plays each of its clips and is compared, pose by pose, with the FBX references
in `DoK-Assets\Models` (bone transforms within a tolerance), and visually in reference shots.

## 5. Audio

Not waveforms: **event logs.** The Recorder logs every Fabric event fired (name, tick, emitter
position, parameters) during golden replays; the rebuild logs the same and the two must match.
Mixing and 3D attenuation are checked by ear against recordings.

## 6. UI

Every screen, in every language, as a reference screenshot from the original at a fixed
resolution, compared with the rebuild's; text layout differences (DirectWrite against Unity's
font rendering) are reviewed by eye, layout and content must match.

## 7. Missions

- **Playthrough traces**: a recorded playthrough of each mission replays bit-exact (§1), which
  also proves the mission scripts issue the same commands at the same ticks.
- **Script event logs**: the Recorder hooks the uScript runtime and PlayMaker and logs node and
  state activations per tick; the rebuild's `Script` runtime logs the same.
- **Cinematics and dialogue**: compared with video recordings of the original by eye.

## 8. What runs where

| Check | CI (no original data) | Local (with the corpus and the install) |
|---|---|---|
| unit tests of `Core`, `Data`, `Sim` with synthetic inputs | yes | yes |
| Core arithmetic against the Recorder's vectors | yes: the vectors are committed (Decisions D14) | yes |
| golden traces, load captures, content checks, reference shots, event logs | no | yes, per milestone and before merging work on the area |
