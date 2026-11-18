# Decision register

Decisions behind the [migration plan](migration-plan.md). "Owner" decisions
were made by the project owner on 2026-11-15. "Technical" decisions follow
from the assessment and from the owner decisions; each can be reopened
through a decision record under `design/adr/`.

## Owner decisions

| ID | Decision | Alternatives rejected |
| --- | --- | --- |
| O1 | Parallel port. Rust targets x86_64 only and never shares a process with the C++. Each library is accepted against reference output from the C++ build | 32-bit Rust DLLs plugged into the C++ component system; a hybrid of the two |
| O2 | Fidelity is format-exact and behaviour-equivalent. Data files, saves, IDs and hashes are bit-exact. Simulation, physics and AI match within tolerances | Bit-exact simulation through x87 emulation; formats only with free redesign |
| O3 | The reference is a restored C++ build with an oracle harness, created in Phase 0. The C++ does not build today | Real data plus vectors transliterated from source; synthetic fixtures only |
| O4 | Shared simulation lives in a new crate, `game-sim`, used by `game-logic` and `game-app`. `OutpostFrontier.exe` links `game-logic` and keeps the in-process server for single player | Putting it in `neuron-core`; running single player against a separate server process |
| O5 | Amended by O19 on 2026-11-17. Physics is `parry3d` for collision geometry plus the project's own zero-gravity rigid-body solver that honours the original tuning constants | `rapier3d`; a faithful port of the IVP reconstruction |
| O6 | The transport is QUIC through `quinn` | `renet`; a purpose-built UDP layer; GameNetworkingSockets |
| O7 | All four port-only features carry over: widescreen, walk mode, volumetric nebula, tick profiler | Dropping any of them |
| O8 | Servers are found by direct connect only. No LAN discovery and no internet list | LAN discovery; a new master list service |
| O9 | The dedicated server is headless, with a config file and a command console | A terminal dashboard; a native GUI |
| O10 | `CppSource/` stays intact until the Rust build is complete and is deleted in one commit | Deletion per library from a tagged reference; deletion per phase |
| O11 | Decided 2026-11-16. The DirectX 8.1 SDK headers and libraries are committed in `CppSource/DX8/`, so the reference build works from a clone. They are deleted with the C++ in P7.3 | Keeping the SDK outside the repository in an ignored `dx8\` folder, as first planned |
| O12 | Decided 2026-11-16. Saves are written as plain-text INI. The C++ reference server writes character saves, new characters from the templates and the single-player restart file as text instead of the `FLS1` cipher container (commit `816f556a`); the Rust port does the same. Both builds still read `FLS1` files, so existing saves and the shipped templates load. This was the first change to the reference's behaviour beyond build fixes and oracle instrumentation; O14 is the second | Keeping the original `FLS1` writer in the reference and in the port; reverting the C++ change |
| O13 | Decided 2026-11-16. The game data is text only: every INI file in the master copy (`outdir\`) is text. The port reads INI files as text and has no binary INI reader; the C++ reference keeps its decoder, which the data never reaches (divergence D10) | Porting the binary INI reader and keeping its oracle suites |
| O14 | Decided 2026-11-17. A directory file system lists its entries. The reconstruction's `FDosFileSystem::FindFirst` (`ReadFile.cpp`) tested `JoinRoot` the wrong way round, so every pattern that joined failed with error 0xA1 and a loose directory could never be listed; it also compared the find handle with `nullptr` instead of `INVALID_HANDLE_VALUE`. Both are fixed in the C++ reference and in `neuron-core::vfs`. The port lists as `FindFirstFileA` does: Win32 wildcard rules and the `.` and `..` entries, but in byte order of the names and without 8.3 short names (divergence D37). Whether the original game had the same defect is not known; callers that enumerate a loose directory (`TextureLibrary`, `VMeshLibrary`, `SceneRuntime`, `Deformable2`) now see its entries | Keeping the defect in both builds; fixing only the port and recording a divergence |
| O15 | Decided 2026-11-17. The original game data is committed in `outdir/` (commit `dbe1abc6`): the master copy's INI files at its top and the `DATA` tree as `outdir/data/`. It is public with the repository; the owner accepts the legal exposure of publishing it. Tests and CI may read it. Dumps and vectors made from it are still not committed: they are produced from `outdir/` when a test runs. The text sources (T21) live in `outdir/data/text/` | Keeping the data outside the repository behind `OUTPOST_DATA_DIR`; making the repository private; removing the data again |
| O16 | Superseded by O17 on 2026-11-17. Decided 2026-11-17. The reference reads the particle node library as `AlchemyNodeLibrary` again. The rebrand commit `89eda18` had renamed the node name in `CppSource/particlefx/particlefx_common.cpp` to `ParticleFxNodeLibrary`, which no `.ale` file has (all 596 use `AlchemyNodeLibrary`), so the reference never loaded node libraries. The one-line fix is an approved change to the frozen reference, like O12 and O14 | Leaving the reference as built and recording the port's correct name as a divergence |
| O17 | Decided 2026-11-17. The data carries the reference's names (option 3 of [rebrand-names.md](rebrand-names.md)). The text files in `outdir/` already used the names of the rebrand commit `89eda18`, nickname hashes included; binary files and one hash compiled into the code still used the old ones. The binary data is renamed: in the 596 `.ale` files the node library `AlchemyNodeLibrary` is now `ParticleFxNodeLibrary` (the UTF string table rewritten in place), and in 851 compiled Lua 3.2 `.thn` cutscenes 1,468 string constants that name the player character's costume (`trent`), voice lines (`[VoiceProfile_][$B]GCS_Trent_*`) and story sounds (`dx_s*_Trent`) now say `Rellan` in the same case. Entity, camera and marker names inside the scripts, and file and directory names, keep the old spelling: nothing outside their own file matches them, or the data refers to them by that path. O16 is undone: the reference reads `ParticleFxNodeLibrary` again. `SetRellan` in `CppSource/Server/server_pub.cpp` asked for the costume `CreateID("trent")`; it now asks for `CreateID("rellan")`, an approved change to the frozen reference. Data dumps run 44056112434 on the renamed data equals the oracle in every kind. The port no longer loads an unmodified original installation; it never could once the text files were renamed | Restoring the old names in the reference (option 1); recording each mismatch as a divergence (option 2) |
| O18 | Decided 2026-11-17. The port loads `.thn` scene scripts as compiled Lua 3.2 chunks only, and only the instructions the shipped scripts use: the 15 opcodes in [facts.md](facts.md) F15, with numbers stored as text. A script in source form, or a chunk with any other instruction, is rejected (divergence D86). Source can be compiled first: `cargo xtask thn-decompile` holds a compiler for the subset its output uses. The decompiler writes the numeral the original source had when the stored text would not compile back to itself (3 constants in the data, F15) | Also loading source in that subset; a full Lua 3.2 evaluator with lexer, parser and VM, as T8 first planned |
| O19 | Decided 2026-11-17. Amends O5. Free flight follows IVP: the integrator, damping and the flight controllers are transliterated from the reference in its operation order on the T11 model, so a ship in free flight matches the oracle within Tier B bounds. Collision geometry is `parry3d` and contact response is the project's own solver, honouring the original tuning constants; collisions are compared with the oracle within scenario envelopes only (O20). The rest of IVP is not ported | O5 unchanged (own solver throughout); a faithful port of IVP |
| O20 | Decided 2026-11-17. Phase 2 is verified in two layers. Replay tests feed the oracle's recorded per-tick state into the Rust integrator, AI and damage code and compare their outputs with the oracle's, Tier B, without drift from earlier ticks. Closed-loop scenarios (fly to a point, dock, fire, collide) are then compared within envelopes; their bounds are proposed in a P2.0 decision record and approved by the owner before Gate G2 is judged. The reference's physics is not reproducible run to run, so a bound is never tighter than the oracle's own spread over repeated runs of the same scenario | Closed-loop scenarios against envelopes only |
| O21 | Decided 2026-11-17 (P2.0). Single player keeps one shared `game-sim` world, as the C++ does: the server side and the client side run their systems over it in turn, with the C++ split of work, and still exchange the message catalog by id over the loopback transport. Answers fact F14; details in [adr/0002](adr/0002-single-player-world.md) | Two worlds joined by loopback messages (the plan's earlier default) |
| O22 | Decided 2026-11-17 (P2.0). `game-sim` holds objects in its own arena with generational handles, an enum of kinds with shared component structs, and handles in place of watchers and raw pointers; details in [adr/0001](adr/0001-object-model.md) | The `slotmap` crate; an ECS library; `Rc<RefCell<…>>` objects |
| O23 | Decided 2026-11-17 (P2.0). The scenario and replay oracle is a headless `Server.dll` run inside `Oracle.exe` (`sim` mode) with a stub client DLL and inert-by-default hooks. The envelope method is fixed now (N = 20 oracle runs, bound = max(2 × spread, floor)); the numbers follow from harness runs and are approved before Gate G2. Details in [adr/0003](adr/0003-simulation-oracle.md) | A driver of its own over Common; deciding the method only after measurements |
| O24 | Decided 2026-11-18. `BuildOrientationBasis` (`Common/common_aisteer.cpp`) zero-fills its result matrix, as the reconstruction's own notes say the original does (`local_48`, zero-filled and copied out on every path). The reconstruction declared `Matrix Result{}`, which leaves it uninitialised because `Matrix` has a user-provided empty constructor, so a forward shorter than 0.01 returned stack contents; the `ai_steer` oracle suite showed it. Fixed in the C++ reference, a recorded exception to the `CppSource/` freeze like O12, O14 and O17; `game-sim::ai::steer` already returns the zero matrix. The other `Matrix X{}` sites in Common are left as they are: their values are overwritten before use, or (`common_faceidea.cpp`) the original is uninitialised too on an unreachable path | Leaving the C++ alone, dropping the degenerate inputs from the suite and recording a divergence; zero-filling every `Matrix X{}` site |
| O25 | Decided 2026-11-18 (P4.0). The renderer is one `render::Device` value over a platform-free state model and a `cfg(windows)` Direct3D 12 backend, with typed states and generational handles; renderer traces move to format version 1 with model replay on any host and image replay on Windows, compared by the metric of the record (starting numbers `t = 8`, `p = 0.5 %`, `m = 1.5`); shaders are DXBC from `fxc` at shader model 5.1 with fixed flags; all FFI lives under `neuron-client/src/platform/`. Up to sixteen lights per draw, BMP and PNG screenshots, two emulated texture stages. Details in [adr/0004](adr/0004-render-device-and-trace.md) | A higher-level renderer API first; raw state numbers in the API; `dxc` and shader model 6; recording resolved draws instead of interface calls |
| O26 | Decided 2026-11-18 (P4.10). Voices of 3D archetypes are spatialised by X3DAudio on the device side: the mixer hands each one its pose and the listener (`VoiceBackend::set_emitter`) and leaves distance attenuation, pan and Doppler to X3DAudio, in place of the reference's DirectSound 3D buffers; the reference's software path stays for 2D and pan voices and as an option (`Spatial::Software`) for the oracle. Environmental reverb (the EAX settings mapped to XAudio2's I3DL2 reverb) is applied whatever `useEAX` says. Audio uses the Windows default output device and its speaker layout; `deviceID` and `speakerConfiguration` are read without effect | A platform-free emulation of DirectSound 3D's inverse-distance law, Doppler and cones; the reference's software path for 3D voices (about 3 dB per doubling of distance instead of DirectSound's 6 dB, no Doppler); reverb only with `useEAX` on, as shipped; dropping reverb; honouring `deviceID` and `speakerConfiguration` |
| O27 | Decided 2026-11-18 (P4.1). The client window is per-monitor DPI aware (version 2): a windowed client area is the mode's size times the monitor's scale, and fullscreen is a borderless window over the monitor (no display-mode change, not topmost); in both the back buffer keeps the mode's size and the swap chain stretches it. Per-user state lives in TOML files under `Documents\My Games\OutpostFrontier`, written atomically; the first run without them imports the C++ client's `HKCU\Software\Microsoft\Microsoft Games\OutpostFrontier\1.0` once and never writes the registry. Saved passwords are sealed with the Windows user's DPAPI. A new install derives the multiplayer account name as the C++ does (MD5 of the Windows `DigitalProductId` and the user name, a UUID without a product id) | Hex-text passwords as the C++ kept them; not saving passwords; exclusive fullscreen through DXGI, or both modes; physical-pixel or window-sized back buffers; a random stored account identity |
| O28 | Decided 2026-11-18 (P4.11; proposed and accepted by the owner the same day). Raw Input stands in for the C++'s exclusive, foreground DirectInput mouse: while the game holds the mouse (the window has the focus and the cursor is on the surface), the mouse is registered without legacy messages and captured (`RIDEV_NOLEGACY`, `RIDEV_CAPTUREMOUSE`), the system cursor is hidden and the pointer is clipped to the client area; when the game lets the mouse go (focus lost, deactivation, or a windowed cursor leaving the surface) the registration is plain again, the clip is lifted and the pointer appears where the software cursor was, as in the C++. Cursor positions read from or written to the system pointer are converted between client and surface pixels (O27) | Leaving the hidden pointer free (clicks could reach other windows and `WM_MOUSEMOVE` would keep re-seating the cursor); recentring the pointer with `SetCursorPos` each frame; reading motion from `WM_MOUSEMOVE` (pointer ballistics, no motion at the screen edge) |
| O29 | Decided 2026-11-18 (P4.12; proposed and accepted by the owner the same day). The faces `fonts.ini` and `rich_fonts.ini` name are not all on a Windows machine: `Agency FB` and `Arial Unicode MS` came with Microsoft Office, `Vibrocentric` (the rich-text defaults) with nothing, and `FixedSys` is a raster face DirectWrite cannot draw. The C++ client drew whatever GDI's font mapper substituted. Decided: keep the names in the data; where DirectWrite lacks a face, substitute by a fixed table in the backend (`Agency FB` to `Bahnschrift SemiCondensed`, which ships with Windows 10 and later and is a condensed sans like it; `Arial Unicode MS` and `Vibrocentric` to `Arial`; `FixedSys` to `Consolas`), Arial for anything else; and load font files placed under `DATA/FONTS` as a private DirectWrite collection first (decision T3), so an installation that has licensed faces can use them. The autoscale search fits each font to the pangram's width share, so a substitute keeps the layout's widths; heights and glyph shapes change. Until the backend implements the table and the private collection, it substitutes Arial for every missing face (divergence D290) | Arial for every missing face (as implemented now); shipping `Agency FB` and `Arial Unicode MS` (their licences do not allow it); a free look-alike font committed to the data tree; GDI kept for text (decision T3 and the policy scan exclude it) |
| O30 | Decided 2026-11-18. Three string literals that the rebrand commit `89eda18` renamed name data it did not rename, so the reference could not find them: the default Veilborn texture `VeilbornRGB1_VeilbornAlpha1` (`surfacematerials/MaterialImpl.cpp`; `SHIPS/NOMAD/nomad_fx.txm` and `no_gunship.3db` hold `NomadRGB1_NomadAlpha1`, the client logged `texture verify failed: nomad surface`), the navmap mesh `zoomedmap_helion.3db` (`OutpostFrontier/Client/nn_navmap.cpp`; the file is `zoomedmap_liberty.3db`) and the HUD contact icon `HUD_contactveilborn.3db` (`hud_contact_list.cpp`; the file is `hud_contactnomad.3db`). The literals take back their spelling from before `89eda18`, an approved change to the frozen reference like O12, O14, O17 and O24. File names keep the old spelling (O17), and a texture name inside a binary model is matched against its own data. Found by the naming analysis ([naming-analysis.md](naming-analysis.md)) | Renaming the two files and the texture inside the `.txm` and `.3db` to the new spelling |
| O31 | Decided 2026-11-18 (owner commit `a0734fd`). `FVoiceProfile::ResolveScriptFile` (`OutpostFrontier/Client/voice_profiles.cpp`) stored a voice's archive as the full path `<data>\AUDIO\<nickname>.utf`, and `OpenScriptFileSystem` handed it to `NeuronRuntime::Data`, whose UTF factory joins it to the data root again (`ReadFile.cpp`, `_makepath(root, path)`); that resolves only when the data path is a single `..\NAME`, as the shipped `..\DATA` is. The reference now stores `audio\<nickname>.utf`, relative to the data root, which opens the same archive under any data path; an approved change to the frozen reference like O12, O14, O17, O24 and O30. The same commit gives system `ep01` in `UNIVERSE/universe.ini` its voice-message prefix (`msg_id_prefix = gcs_refer_system_ep01`, whose messages `AUDIO/voices_space_female.ini` and `voices_space_male.ini` hold) | Keeping the full path (voice archives open only under a `..\NAME` data path); joining in the factory only paths that are not already under the root |

## Technical decisions

### T1 — Direct3D 12 through the `windows` crate

Raw Direct3D 12, feature level 11_0, Windows 10 and later, flip-model swap
chain, borderless full screen, tearing when the sync interval is zero. The
scene renders to an intermediate target and a final blit applies the gamma
curve; this also serves screenshots, back-buffer reads and depth sampling.
HLSL is compiled ahead of time and embedded. Runtime compilation exists
only behind a development feature for the nebula shaders.

Amended 2026-11-17 (owner): a crate keeps its HLSL sources in
`<crate>/shaders/` and the compiled blobs in `<crate>/compiled_shaders/`,
both committed. The code embeds the blobs with `include_bytes!`, the Rust
counterpart of compiling to a header and including it, so building the
crate needs no shader compiler and every host can build it. A CI step on
Windows recompiles every source with the pinned compiler and flags and
fails when a committed blob differs. The compiler (`fxc` for DXBC or `dxc`
for DXIL), its version and the shader model are fixed in spike P4.0; the
flags strip debug data and reflection so the output depends only on the
source and the compiler version.

`wgpu` was rejected: the feature set is small and fixed, the owner asked for
Direct3D 12, and `wgpu` hides swap chain, pipeline-state and readback
control this port needs. See [05-rendering](assessment/05-rendering.md).

### T2 — Fixed-function device first

The Rust renderer first exposes the C++ renderer's state-machine shape as a
typed API: enums for states, generational handles, an explicit context. One
vertex shader and one pixel shader, driven by a per-draw constant buffer,
implement lighting, fog, texture coordinate generation, two texture stages
and alpha test. Pipeline states are created lazily from a key and cached.

A higher-level renderer API is deferred until parity is shown. Designing it
first would force a rewrite of 41 materials and about 650 state-setting
call sites before anything could be compared with the C++.

### T3 — Text through DirectWrite

Fonts are rasterised by DirectWrite in GDI-compatible measuring mode into
the existing atlas scheme. Font files found in the data tree load as a
private collection; otherwise installed faces are used, as today.

### T4 — Audio through XAudio2

The C++ computes attenuation, pan, pitch, volume clamps and the ranked
voice cap itself for software voices. That arithmetic is ported as plain
Rust and tested; XAudio2 source voices play the result. Native loop regions
replace the notify thread. EAX settings map to an XAudio2 reverb send.
Hardware-buffer placement, EAX passthrough and the 3D algorithm option are
dropped. `symphonia` decodes WAV and MP3.

### T5 — Movies through Media Foundation

The source reader decodes the existing ASF files; frames upload to a
texture and audio goes to a streaming voice. The certificate stub and the
Video for Windows path are not ported.

### T6 — Mouse through Raw Input

Keyboard handling stays on window messages. No controller support is added.

### T7 — System-drawn IME windows

The edit-box model is rebuilt in Rust. Composition and candidate windows
are drawn by the system and positioned by the game. This is sufficient
because exclusive full screen is not used (T1).

### T8 — Lua 3.2 as a Rust load-time evaluator

Scene scripts are Lua 3.2 chunks that define three tables and are never
called again. Revised 2026-11-17 by owner decision O18: every shipped
script is straight-line table construction (facts.md F15), so the Rust
side is a compiled-chunk reader and an evaluator for those 15 opcodes, a
few hundred lines instead of a lexer, parser, VM and base library. Number
constants are read from their stored text as `luaO_str2d` reads them. No
library is provided, so the I/O library, which lets scripts run commands,
cannot be reached. `mlua` cannot load Lua 3.2. The Lua copyright notice is
kept with the module. `luaprofile` has no consumer and is not ported.

### T9 — Static composition

The component manager, the system container and engine start-up become one
`Engine` value whose constructor builds the subsystems in the order the
real `[System]` list gives (fact from P0.3). Traits exist only where two
implementations exist (file-system backends, transports). Content
controllers are a fixed set registered at compile time. Server, content,
transport and client are connected by traits passed at start-up, which
removes the four run-time dependency cycles in the C++.

### T10 — Ownership

Explicit context values own all state. World objects, equipment, UI
controls and scene instances live in arenas and are referenced by typed
generational handles. Watcher chains and manual reference counts become
handle validity checks. Decision records from spikes P2.0, P3.0 and P4.0
fix the detail.

### T11 — Numeric policy

- One canonical arithmetic: the dedicated server's 53-bit x87 mode, which
  is also the mode the oracle runs in. Values are stored as `f32` where the
  C++ stores a `float`. An expression the C++ evaluates on the x87 stack is
  evaluated in `f64`, whether the source spells `float` or `double`, and
  rounded to `f32` at each point where the reference stores to a `float`:
  an assignment, a cast, a `float` argument or return, an `FSTP dword` in
  inline assembly. Reworded 2026-11-17: the first wording ("`f64` exactly
  where the C++ spells a double") would have computed `float` code in
  `f32` and missed the oracle by more than 1 ulp in cancellation cases.
  The client's 24-bit mode is not modelled (divergence D11, accepted by
  the owner on 2026-11-17).
- No fused multiply-add contraction and no fast-math anywhere.
- `neuron-core::num` supplies truncating and round-to-even float-to-int
  conversions with x86 overflow results. `neuron-core::msvcrt` supplies the
  wrapping `atoi` and `_wtoi`, the legacy `atof` and the Universal C
  Runtime `%g` the reference formats with. The VC6 number formatters of
  `msvcrt_numeric_format.h` are in `neuron-core::msvcrt` too
  (`format_lua_number`, `numeric_vsnprintf`), shared by the client and
  xtask's chunk compiler (reworded 2026-11-18, P4.13: "port with their
  users" would have given xtask, which may depend only on neuron-core, a
  second copy).
- The CRT random generator is an explicit value passed to its users, not a
  process global. Draw order is preserved where a saved seed rebuilds state
  (random missions).
- Trigonometry uses the Rust standard library. Results that pass through
  it are Tier B.
- No `asm!`. No x87 emulation.

### T12 — Errors and logging

Library crates define error enums with `thiserror`. The executables may use
`anyhow`. `tracing` replaces `FDUMP`; tick-profile sections are spans
behind a feature.

### T13 — Dependencies

Initial allow-list: `windows`, `quinn`, `rustls`, `rcgen`, `tokio`
(runtime, net and time features only), `parry3d` with `nalgebra`, `flate2`,
`symphonia`, `memmap2`, `md-5`, `quick-xml`, `thiserror`, `anyhow`,
`tracing`, `serde`, `toml`, `bitflags`, `png`; `proptest` for tests.
`cargo-deny` enforces licences and advisories. Additions need a line in
this register.

The permitted licences are listed in `deny.toml`. In use so far:

- `toml`, in `xtask` and `neuron-client` (`user_state`, the per-user state
  files of P4.1; `tests/rdl_cards.rs` reads the card sources, P4.12).
- `flate2` 1.1 (MIT or Apache-2.0; `rust_backend`, that is `miniz_oxide`):
  `neuron-core::net::frame`, compressed batches only (P1.9).
- `md-5` 0.10 (MIT or Apache-2.0; default features off):
  `neuron-core::util::md5` (P1.5). The C++ MD5 was checked against RFC 1321
  and is standard.
- `tracing` 0.1 (MIT; `std` feature only, no attribute macros):
  `neuron-core::diag` (P1.5). `tracing-subscriber` is not used; the
  channel subscriber is written in `diag`.
- `parry3d` 0.25.3 (Apache-2.0; default features off, `required-features`,
  `std`, `alloc`): `neuron-core::physics` (P2.3), collision geometry, shape
  casts, rays and intersection tests. It is the last release on `nalgebra`
  (0.26 and later moved to `glam`). `nalgebra` 0.34 (Apache-2.0) comes with
  it and is used through `parry3d::na`, in `physics::shape` only. Its
  build-time macro crate `paste` carries an "unmaintained" advisory
  (RUSTSEC-2024-0436), ignored in `deny.toml` with that reason. The owner
  confirmed both on 2026-11-17: the ignore stays, and `parry3d` stays pinned
  to 0.25.3 rather than moving to the `glam` releases.
- `proptest` 1 (MIT or Apache-2.0; default features off, `std` only):
  a dev-dependency of `game-sim`, the protocol property tests (P2.2).
- `windows` 0.62.2 (MIT or Apache-2.0; features `Win32_Foundation`,
  `Win32_Graphics_Direct3D`, `Win32_Graphics_Direct3D12`,
  `Win32_Graphics_DirectWrite`, `Win32_Graphics_Dxgi`, `Win32_Graphics_Dxgi_Common`, `Win32_Graphics_Gdi`,
  `Win32_Security`, `Win32_Security_Cryptography`, `Win32_System_Com`,
  `Win32_System_LibraryLoader`, `Win32_System_Registry`,
  `Win32_System_Threading`, `Win32_UI_HiDpi`, `Win32_UI_Shell`,
  `Win32_UI_WindowsAndMessaging`): `neuron-client::platform` only, behind
  `cfg(windows)` (P4.2, record 0004 point 19). Pinned when P4.2 started,
  2026-11-18. P4.1 added `Gdi` (monitor rectangles only, no GDI text),
  `HiDpi`, `Shell` and `Com` (the Documents folder), `Registry` (the
  one-time import) and `Security_Cryptography` (DPAPI). P4.10 added `Media_Audio` and
  `Media_Audio_XAudio2` for the XAudio2 device (`platform::audio`, proposed
  2026-11-18), and P4.15 `Media_MediaFoundation` for the startup movies'
  source reader (`platform::media`, proposed
  2026-11-18); X3DAudio, which the crate describes only by constants, is
  declared there and loaded with the XAudio2 library at run time.
  P4.11 added `Win32_UI_Input` (Raw Input), `Win32_UI_Input_KeyboardAndMouse`
  (key state, focus, keyboard layouts) and `Win32_UI_Input_Ime` (the
  documented IMM32 calls of T7; `platform::input` and `platform::ime`,
  proposed 2026-11-18); P4.12 added `Graphics_DirectWrite` for the font
  backend (`platform::dwrite`,
  proposed 2026-11-18).
- `symphonia` 0.6.1 (MPL-2.0; default features off, feature `mp3` only,
  which brings `symphonia-core`, `symphonia-bundle-mp3` and
  `symphonia-metadata`, with `bitflags`, `bytemuck`, `lazy_static`, `libm`,
  `log`, `num-complex`, `num-traits`, `regex-lite` and `smallvec`, all MIT,
  Apache-2.0 or Zlib): `neuron-client::audio::decode`, the MPEG Layer III
  data of the game's MP3-in-WAV sounds (P4.10, replacing the Audio
  Compression Manager). The shipped data holds only PCM and MPEG-2 and
  MPEG-2.5 Layer III, so no other codec or container is enabled; the WAV
  container is read by `audio::wav`. Proposed 2026-11-18, with
  `[profile.dev.package]` `opt-level = 3` for `symphonia-core` and
  `symphonia-bundle-mp3` so the data tests decode all shipped sounds in
  seconds.

Math types are the project's own (`neuron-core::math`) so operation order
stays under control; conversion to `nalgebra` happens at the `parry3d`
boundary only.

### T14 — Unsafe code

`unsafe` is forbidden in `game-sim`, `game-logic`, `game-app`,
`neuron-server` and the executables, and denied everywhere else. It may be
allowed in `neuron-client` platform modules and for memory mapping in
`neuron-core`, with a safety comment on every block. The directories that may
allow it are listed in `xtask/src/scan.rs`; the work package that adds an
FFI module adds its directory there.

### T15 — Strings

UTF-8 `String` inside. Nicknames stay byte strings where hashing depends on
bytes at or above 0x80. UTF-16 appears only in resources, the hex-encoded
names in saves, wide strings on the wire and Windows calls. Account-name
lower-casing follows the C++ result for the characters the oracle covers;
others are recorded as divergences.

### T16 — Identity and login

The account name scheme is kept so existing account directories load: the
machine-derived MD5 text is still the account name. It now travels inside
TLS. The server presents a self-signed certificate generated on first run;
the client pins it on first connect and warns on change. The session
password, which the C++ client never sends, is part of the handshake. The
registry values (`InstallKey`, `MPAccountName`, login history) are imported
once into a per-user file.

### T17 — Wire protocol

Kept from the C++: the 162 message types, their two-byte header, field
order, widths and quantisers. Added: a QUIC application protocol name with
a version, a handshake carrying protocol version, build, password and
account, a length prefix per message, and an explicit flag for compressed
batches. All game traffic uses one reliable stream per client, as today.
Moving object updates to unreliable datagrams is a later improvement and
not part of parity. The governor that stops object updates when the link
saturates reads the application-side backlog and the QUIC round-trip time.

Single player uses the loopback transport and passes typed messages without
encoding. A test option forces an encode and decode on every loopback
message.

With direct connect only (O8), the server-select page becomes an address
book. A row's status comes from a handshake without login.

Not reproduced: the decode-loop defects listed in
[03-network-stack](assessment/03-network-stack.md), the implicit "try to
inflate, else raw" rule, and the cross-process tick handshake through named
events.

### T18 — Saves

The save text schema is written and read exactly, so saves move between
the C++ and Rust builds in both directions until the C++ is deleted. Both
write plain text (decision O12); both read the `FLS1` container as well, so
saves written by the original game and the shipped templates still load.

### T19 — Time step

Variable time step as in the C++: one step per frame in single player, the
0.2-second sub-step loop on the dedicated server, physics at
`PHYSICAL_SIM_RATE`. A fixed step is a possible later change.

### T20 — User interface

Kept: the control tree of mesh-backed controls, factory lookup by class
name, the INI and page-text grammars, draw layers, text IDs and rich-text
markup. Rebuilt: typed messages in place of the untyped property call, an
arena tree, an explicit modal stack in place of nested message pumps.
Widescreen anchoring is part of the layout design from the start.

### T21 — Text resources

The `.rc` string tables and the 5,252 rich-text files move to
`outdir/data/text/`. An `xtask` step builds one catalog per module, keyed by
the local 16-bit ID. At run time catalogs load in the order the real
`[Resources]` list gives, so every 32-bit text ID keeps its value. Catalogs
ship as files beside the executable.

Amended 2026-11-17 (owner): the sources are one TOML file per module, with
a `[strings]` table and a `[cards]` table keyed by the local ID; a card's
`<RDL>` markup is kept as text. They were generated from the `.rc` scripts
and card files, and the catalogs built from them are byte-identical to the
`.rc` build's. The resource-script parser in `xtask` was removed with
them. Until P7.3 a text change has to be made in `CppSource/Resources/` as
well; CI's catalog comparison catches a copy that differs.

Open for P4.12: the cards use seven RDL tags (`RDL`, `TEXT`, `PARA`,
`PUSH`, `POP`, `TRA`, `JUST`). Once the RDL node model is ported and its
XML reader checked against the oracle, the cards may become structured
TOML (a list of nodes) instead of markup. That would remove the XML reader
only if the other XML inputs go too: two hard-coded strings in the client
(`nn_help.cpp`, `nn_navmap_input.cpp`), two string-table entries that hold
`<RDL>` markup, and the "try XML, else plain text" rule for string entries
(`ids_rdl.cpp`).

### T22 — Configuration files

Game configuration keeps its file names and key names
(`OutpostFrontier.ini`, `PerfOptions.ini`, `UserKeyMap.ini` and the data
INIs). The `[Libraries]` and `[System]` sections of the engine INI lose
their function under T9; per-subsystem settings in that file are still
read. Hardware-quirk keys are ignored. The dedicated server gets a new TOML
config file and imports the old binary config once.

### T23 — Removed without replacement

`luaprofile`, the regular-expression component, `MemoryDiagnostics`, the
zlib DLL and minizip, `qhull`, IVP, both legacy renderers, the GPU quirk
database, the list-server wrapper, the EULA gate, the crash-report
launcher, `Flasher.exe`, the network-alive probe, the `dxdiag` launch, the
unused ODBC link, the duplicate codecs in `rpclocal`, and the run-time
plug-in mechanism.
