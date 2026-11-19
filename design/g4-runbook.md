# Gate G4: the owner's hardware runs

Gate G4 ([migration plan](migration-plan.md), Phase 4) needs three things
that CI's Windows runner cannot give: renderer traces of the C++ client on
the original data, the audio device on a machine with an audio endpoint,
and the movie decoder on a Windows with Media Foundation's Windows Media
decoders. This page is the procedure for the owner's Windows machine. Every
file it produces is made from the game data: keep traces, images and
reports outside the repository (AGENTS.md, test data), and share them only
in the conversation.

The fourth G4 item, the model viewer, comes with the engine composition
(P4.16) and is not covered here.

## 0. Requirements

- Windows 10 or 11 with a Direct3D 12 GPU (feature level 11_0), a working
  audio output device, and the Media Foundation components (on an N edition,
  the Media Feature Pack).
- The C++ reference build of [reference-build.md](reference-build.md),
  sections 1 to 5: the solution built `Release|Win32` (it builds
  `bin\RenderTrace.dll` too), DirectPlay enabled, and a run folder that
  plays no startup movies (section 3). Use a copy of the run folder for
  tracing: the trace settings below go into its `NeuronComponents.ini`,
  never into `outdir\`.
- The Rust toolchain of `rust-toolchain.toml` and a checkout of `main` at or
  after the commit that added this page.

## 1. Record the C++ renderer traces

The `+oracle` mode ([reference-build.md](reference-build.md) section 8)
paces the client at a fixed 1/30 s step and drives it with a script, so the
frame numbers below land on the same picture every run. RenderTrace
(section 9) records the selected frames and a front-buffer image of each.

1. In the tracing run folder's `NeuronComponents.ini`:

   ```ini
   [Libraries]
   DX8RENDERER.dll        ; stays loaded
   RenderTrace.dll

   [System]
   RenderTrace            ; in place of the DirectX8 line
   ```

2. Park the mouse pointer in a screen corner, keep the window in front and
   uncovered while it runs, and record one trace per scene, each into its
   own folder under one root (here `C:\traces`):

   ```powershell
   cd <tracing run folder>\bin
   mkdir C:\oracle-out -ErrorAction SilentlyContinue

   # The main menu with its background scene.
   $env:OUTPOST_RENDER_TRACE_DIR = 'C:\traces\menu'
   $env:OUTPOST_RENDER_TRACE_FRAMES = '200-201'
   .\OutpostFrontier.exe +oracle ..\..\CppSource\OutpostFrontier\oracle\menu_walk.txt C:\oracle-out\menu

   # Space after the start of a new game: ships, a station, effects.
   $env:OUTPOST_RENDER_TRACE_DIR = 'C:\traces\space'
   $env:OUTPOST_RENDER_TRACE_FRAMES = '400-401'
   .\OutpostFrontier.exe +oracle ..\..\CppSource\OutpostFrontier\oracle\single_player.txt C:\oracle-out\space

   # The first base after docking: the room scene, a skinned character.
   $env:OUTPOST_RENDER_TRACE_DIR = 'C:\traces\base'
   $env:OUTPOST_RENDER_TRACE_FRAMES = '2300-2301'
   .\OutpostFrontier.exe +oracle ..\..\CppSource\OutpostFrontier\oracle\single_player.txt C:\oracle-out\base
   ```

   Adjust the script paths to where the checkout is. The frame numbers come
   from the owner run of 2026-11-17 (section 8, "Owner run": the start in
   space at frame 118, docking at frame 2175); if a picture is still loading
   at that frame, move the range later. A trace writes every resource upload
   up to its last frame, so the base trace is large.

3. Each folder now holds `trace.jsonl` and `blobs\`. `FLSpew.txt`
   (`%LOCALAPPDATA%\OutpostFrontier`) says which file was opened or why
   tracing stayed off.

## 2. Replay them into the Rust renderer

From the repository root, on the same machine:

```powershell
$env:OUTPOST_RENDER_TRACES = 'C:\traces'
cargo test --locked -p neuron-client --test render_image_replay recorded_traces -- --nocapture
# Again on the GPU instead of WARP:
$env:OUTPOST_RENDER_ADAPTER = 'hardware'
cargo test --locked -p neuron-client --test render_image_replay recorded_traces -- --nocapture
```

The test (`recorded_traces_match_their_frame_images`) plays each trace into
the Direct3D 12 backend at the frame image's size, compares every frame with
the C++ image by the Tier C metric of record 0004 (point 14), and writes
`<trace>\replay\report.txt`, plus `frame-<n>-actual.bmp` and
`frame-<n>-difference.bmp` for each frame that fails. A failure is
expected at first: these are the first traces of real content, and the
report is the input for the next fixes.

Send back, in the conversation: the console output or the `report.txt`
files, and for failing frames the difference images. Traces stay on the
machine unless a specific frame needs a closer look.

## 3. Audio and movies on hardware

```powershell
cargo test --locked -p neuron-client --lib platform::audio -- --nocapture
cargo test --locked -p neuron-client --lib platform::media -- --nocapture
```

The audio tests open the default device, play archetype voices that move
and end, apply every reverb environment, stream to the end and loop shipped
music; you should hear short sounds and music. The media test decodes the
shipped logo movie. A test that finds no device or decoder prints
`nothing to test` and passes; that line in the output means the item is not
covered yet. Send back the output, and say whether the sounds played and
whether anything sounded wrong (crackle, wrong pitch, no stereo movement).

## 4. Then

The reports go into the P4.2, P4.10 and P4.15 rows of
[README.md](README.md), with any divergences they show; G4 also needs the
model viewer (P4.16).
