# Plan: the actions, in order

Two tracks run in parallel and meet at milestone M3:

- **Track A, the simulation:** ground truth from the original (phase 1), then the port (phase 3).
  It is verifiable on its own, headless, bit for bit, long before anything is drawn.
- **Track B, content and engine:** the asset pipeline (phase 2), engine foundations (phase 4),
  rendering parity (phase 5).

Then presentation, audio, UI, missions and multiplayer (phases 6–10) build on both, and phase 11
completes the game.

| Milestone | Means | After |
|---|---|---|
| **M1** | every golden replay runs bit-exact through the C++ simulation, headless | phase 3 |
| **M2** | every WorldArt scene loads and renders in a free camera, matching reference shots for placement | phase 4 |
| **M3** | a skirmish against the AI is playable on one map: the C++ simulation, near-parity visuals, basic HUD | phase 6.2 |
| **M4** | campaign mission M01 plays end to end: script, dialogue, cinematics, UI | phase 9.3 |
| **M5** | the whole campaign (Tutorial, M01–M13) | phase 9.3 |
| **M6** | LAN multiplayer and skirmish on every map, saves and replays | phase 10 |
| **M7** | complete: every original feature, every language, performance targets | phase 11 |

Each action: **deliverable** → **exit** (how it is known done). Sizes are relative (S < M < L < XL).

## Phase 0: foundations

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 0.1 | Rule on the open decisions D1–D14 ([Decisions.md](Decisions.md)) | rulings recorded → no phase-1/2 action waits on a decision. **Done 2026-11-14** | S |
| 0.2 | Solution and projects as in [Architecture.md §2](Architecture.md), empty; coding conventions (D10) | `Outpost.Frontier.sln`, `CONTRIBUTING.md` → builds in Debug and Release. **Done 2026-11-14:** 16 projects under `Source/`, warning level 4 as errors, `Tests.exe` checks that all 12 libraries link | S |
| 0.3 | CI on a Windows runner: build, unit tests | workflow → green on every push. CI never sees original code or data: golden-trace and content checks run locally. **Done 2026-11-14:** `.github/workflows/build.yml` (Debug and Release, tests, no platform headers in `Core`/`Data`/`Sim`, extraction scripts parse) | S |
| 0.4 | Local inputs configuration: paths of the install, `DoK-Decompiled`, `DoK-Assets`, cooked content, golden corpus | a git-ignored JSON file, read by every tool → each tool reports a clear error when an input is missing. **Done 2026-11-14:** `local.json` (`gameDir`, `decompiledDir`, `assetsDir`, `toolsDir`, `contentDir`, `corpusDir`); `Tools/Extract/Extract.ps1` reads it and rebuilds `DoK-Decompiled` and `DoK-Assets` from the install | S |
| 0.5 | Decompile the middleware the game's behaviour depends on, as specification: NGUI, `Fabric.*`, CinemaDirector, Cinemachine, PlayMaker, `uScriptRuntime`, LeanTween | C# projects beside the others → every runtime in phases 5–9 has its reference. **Done 2026-11-14:** 13 assemblies (also `UnityEngine.UI`, `UnityScript.Lang`, `Boo.Lang`), 872 files in `DoK-Decompiled\Middleware\` (`Extract.ps1 -Steps middleware`) | S |

## Phase 1: ground truth from the original (Track A)

The original is the specification. This phase instruments it once and records what everything
after it is checked against.

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 1.1 | **Recorder**: a BepInEx 5 + HarmonyX plugin for the original game (Mono, Unity 5.2), installed into a copy of the game folder: per tick, the commands applied and the checksum (`Checksum`, exported as JSON per state component); on demand, full state frames. Hook points: `CommandRouter.ApplyEndOfFrameState(currentFrame, stateCollectionList, simChecksum)`, called once per tick with the frame's state and checksum (the one the original's desync check compares), and `CommandBuffer.ScheduleCommandForFrame(command, frame)` | plugin + format spec → a recorded skirmish replays in the original twice with identical checksum streams. **Done 2026-11-14:** `Tools/Recorder`, installed in the game. A skirmish (MP_05-Teeth, 998 ticks) replayed twice: every tick's checksum identical to the live game and between the two playbacks, no `SyncError`. Found: a `.dokreplay` stores checksums only in frames with commands (11 of 998), so the dense per-tick checksums come from the Recorder | M |
| 1.2 | **Golden corpus**: replays with traces for every campaign mission (recorded playthroughs) and for every skirmish/MP map (AI-vs-AI skirmishes run unattended). Each entry is a pair: the game's `.dokreplay` (the full commands) and the Recorder's trace (every tick's checksum). The campaign saves no replay, so for it the Recorder must also write the full command data (each command's JSON, as the replay would hold it) | corpus on the owner's machine → every mission and map covered at least once | M |
| 1.3 | **Load captures**: every attribute's loaded `Fixed64` raw value; the simulation descriptors built from each design scene; the sim's height map and nav mesh after load | captures → the inputs of the C++ loaders can be compared value by value | S |
| 1.4 | **Presentation references**: screenshots at scripted camera positions per scene; the Fabric events fired per tick in golden replays; every UI screen per language | reference set → phases 5, 7 and 8 have something to compare against | M |

## Phase 2: the asset pipeline (Track B)

Per [Asset-Pipeline.md](Asset-Pipeline.md), stage 2a (reading `DoK-Assets`).

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 2.1 | `Cook` framework: inputs, stable IDs, cooked headers, the content layout, the check framework, reproducibility | tool → an empty cook of the whole install runs and is reproducible | M |
| 2.2 | Lossless converters: textures (DDS), meshes, string tables, fonts, sim height maps and nav meshes | converters + checks → all pass on the whole install | M |
| 2.3 | **Schema generator**: C++ structs and loaders from the decompiled C# class definitions of the component and attribute classes; then the scene and prefab converter | generator, `Cook scene` → every scene converts; counts match `manifest.json`; all references resolve | L |
| 2.4 | Materials, particle systems, UI atlases, skeletons, animation clips and controllers, audio events, cutscenes, PlayMaker FSMs | converters + **usage reports** (particle modules, Fabric component types, PlayMaker actions, component types per scene) → the runtimes' scope is measured, not guessed | L |
| 2.5 | Re-encoders: streamed audio to xWMA (D5, T2), video to H.264 MP4 (D6, T1) | converters → durations match; quality threshold met | S |
| 2.6 | **Mission script recovery**: the uScript graphs from the generated C# (D4) | `Cook script` + node-type report → all 82 graph components recover to `.graph` with every node type listed | L |
| 2.7 | Stage 2b (D3): `Cook` reads the installation itself | Unity 5.2 serialized-file reader → cooks from the install equal cooks from `DoK-Assets` | L |

## Phase 3: Core and the simulation (Track A)

The C# is the specification, ported namespace by namespace; each step extends the golden-trace
coverage that `SimRunner` checks.

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 3.1 | `Core`: `Fixed64`, `Vector2r`/`Vector3r`, `Random64`, `Checksum`, ID types, and the .NET-compatible pieces of [Architecture.md §4](Architecture.md) (Mono's Dictionary/HashSet, sort and string hash) | library + tests against values computed in the original (via the recorder) → every operation bit-equal, including overflow cases. **Started 2026-11-14:** `Fixed64` and `Checksum` ported (`Source/Core`); all 28,319 vectors of `Tools/Vectors` (the original's C#, run on .NET Framework x64) equal, and the tests shown to catch planted faults. Next: `Vector2r`/`Vector3r`, `Random64`, ID types, the Mono collections and hashes; cross-check the vectors inside the game via the Recorder | M |
| 3.2 | `Data`: attribute types (generated, 2.3) and their loaders | → loaded values equal the load captures (1.3) | M |
| 3.3 | `SimRunner`: replay a command log, compare per-tick checksums, and on divergence report the first tick and the first component whose checksum differs | tool → used by every step below | M |
| 3.4 | Port the entity/component model and the simulation core (`SimController`, state frames, events) | → empty-map replay ticks with matching checksums | L |
| 3.5 | Movement: maneuvering, nav mesh, height map, formations | → movement-only replays bit-exact | XL |
| 3.6 | Combat: weapons, projectiles, damage, sensors and detection | → combat replays bit-exact | XL |
| 3.7 | Abilities, buffs, production, research, resources and harvesting, the commander system | → skirmish replays bit-exact up to the first AI decision | XL |
| 3.8 | AI: the managers (economy, combat, intel, production, needs, targets) and jobs | → AI-vs-AI replays bit-exact | XL |
| 3.9 | Commands and router, replay, network protocol (lockstep messages) | → **M1**: the whole golden corpus bit-exact | L |

## Phase 4: engine foundations (Track B)

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 4.1 | `Platform`: window, input, timing, file system | → a window, input events, a frame clock | S |
| 4.2 | `Render` core: D3D12 device, swap chain, frame pacing, uploads, descriptors, the `dxc` build step | → debug layer clean; device-removed handled | M |
| 4.3 | `Scene` runtime: cooked scenes, hierarchies, prefab instancing, LOD groups | → every scene loads | M |
| 4.4 | Meshes and textures with one reference shader; a free camera with the original's projection | → **M2** | M |

## Phase 5: rendering parity (Track B)

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 5.1 | Shader inventory: the 217 shaders by material use; the property mapping per shader | table → the rewrite order is known | S |
| 5.2 | Rewrite the shaders in HLSL (SM 6) from their disassembly, most used first: terrain (`MixMap_Terrain`, `MixMap_Mesh_Blend_Detail`), units, environment, FX | shaders → per shader, reference-shot comparison within tolerance | XL |
| 5.3 | Lighting: real-time lights and shadows as the original; the replacement for Enlighten GI (R3, D2) | → WorldArt scenes match references within tolerance | L |
| 5.4 | Deferred decals | → decal-heavy scenes match | M |
| 5.5 | Particles: a Shuriken-compatible runtime for the measured modules, and Blackbird's FX layer over it | → FX in golden replays match references | XL |
| 5.6 | `Anim`: clips, controllers, skinning, `AnimatedSuspension` | → every animated model plays its clips as the FBX references | L |
| 5.7 | Post-processing (the image effects in use, Chromatica colour grading), layer sorting, the sensors view | → full-frame comparison within tolerance | L |

## Phase 6: the presentation (joins the tracks)

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 6.1 | `Game`: run `Sim` at 8 ticks/s; interpolate state frames; unit views (model, animation state, weapon FX); selection and commands from input | → a golden replay plays back visually | L |
| 6.2 | RTS camera, input mapping, basic HUD | → **M3** | M |
| 6.3 | The rest of `BBI.Unity.Game`: FX events, world, route components, scenarios, rendering helpers | → golden replays look like the original's references | XL |

## Phase 7: audio

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 7.1 | `Audio`: XAudio2 graph, the Fabric component runtime, 3D, streaming | → the Fabric event log of a golden replay matches the original's (1.4) | L |
| 7.2 | Dialogue, drama and dynamic-music controllers | → campaign audio matches on golden playthroughs | M |

## Phase 8: UI and localisation

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 8.1 | `Ui`: the NGUI model (atlases, sprites, labels, anchors, tweens), DirectWrite text, localisation | → a sample screen matches its reference | L |
| 8.2 | Every screen: front end, lobby, briefings, HUD, menus, options; 7 languages | → screen-by-screen comparison | XL |

## Phase 9: missions, scripting, cinematics

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 9.1 | `Script`: the uScript node runtime (from the node-type report), the PlayMaker actions in use, the bridge to the simulation (as `BBI.Unity.Game.Scripting`) | → each mission's scripts run without unknown nodes | L |
| 9.2 | Cutscenes (CinemaDirector runtime) and video (NIS) | → cinematics match the original's recordings | M |
| 9.3 | The campaign, mission by mission: Tutorial, M01 … M13; each played through against its golden playthrough (trace + video) | → **M4** after M01, **M5** after M13 | XL |

## Phase 10: multiplayer, platform, persistence

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 10.1 | Lockstep sessions over LAN (`Net`); Steam and GOG Galaxy are out of scope for now (D11) | → a 2-player and an 8-player LAN game | L |
| 10.2 | Save/load in our own format (D8) over the `Sim` state; replays | → saves round-trip bit-exact (the checksum after load equals the checksum at save); recorded replays play back | M |
| 10.3 | Platform services (achievements, stats, cloud saves): only if D11 is revisited | → as decided then | — |
| 10.4 | Every skirmish and multiplayer map | → **M6** | M |

## Phase 11: completion

| # | Action | Deliverable → exit | Size |
|---|---|---|---|
| 11.1 | DLC content (`BBI.Unity.Game.DLC`), every option, performance targets (D7), crash reporting | → **M7** | L |
| 11.2 | First run: find the owner's installation, cook it, start | → a clean machine with the original installed plays the game | M |

## Order of work, and why

1. Phase 0, then phases 1 and 2 at once: both read the original and neither needs the other.
2. Phase 3 starts when 1.1 records traces; phase 4 when 2.3 produces scenes.
3. The simulation goes first among the large pieces because it is the only part that can be
   proven exactly, and everything else (presentation, scripting, multiplayer) is built on it.
4. Rendering parity (phase 5) runs alongside phase 3; the tracks meet at 6.1.
5. Missions (phase 9) come after audio and UI because a mission is all of them at once.
