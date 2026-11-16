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
| O5 | Physics is `parry3d` for collision geometry plus the project's own zero-gravity rigid-body solver that honours the original tuning constants | `rapier3d`; a faithful port of the IVP reconstruction |
| O6 | The transport is QUIC through `quinn` | `renet`; a purpose-built UDP layer; GameNetworkingSockets |
| O7 | All four port-only features carry over: widescreen, walk mode, volumetric nebula, tick profiler | Dropping any of them |
| O8 | Servers are found by direct connect only. No LAN discovery and no internet list | LAN discovery; a new master list service |
| O9 | The dedicated server is headless, with a config file and a command console | A terminal dashboard; a native GUI |
| O10 | `CppSource/` stays intact until the Rust build is complete and is deleted in one commit | Deletion per library from a tagged reference; deletion per phase |

## Technical decisions

### T1 — Direct3D 12 through the `windows` crate

Raw Direct3D 12, feature level 11_0, Windows 10 and later, flip-model swap
chain, borderless full screen, tearing when the sync interval is zero. The
scene renders to an intermediate target and a final blit applies the gamma
curve; this also serves screenshots, back-buffer reads and depth sampling.
HLSL is compiled at build time with the Windows SDK compiler and embedded.
Runtime compilation exists only behind a development feature for the
nebula shaders.

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
called again. A Rust evaluator (lexer, parser, compiled-chunk loader, VM,
base library subset) loads source and compiled forms unchanged. The I/O
library, which lets scripts run commands, is not provided. `mlua` cannot
load Lua 3.2. The Lua copyright notice is kept with the module.
`luaprofile` has no consumer and is not ported.

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

- One canonical arithmetic: `f32` storage, with `f64` intermediates exactly
  where the C++ source spells a double. This corresponds to the dedicated
  server's 53-bit mode and is the mode the oracle uses for simulation
  vectors.
- No fused multiply-add contraction and no fast-math anywhere.
- `neuron-core::num` supplies truncating and round-to-even float-to-int
  conversions with x86 overflow results, the wrapping `atoi`, the legacy
  `atof` and the VC6 `%g` formatter.
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

The permitted licences are listed in `deny.toml`. In use so far: `toml`,
in `xtask` only.

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

The save container and text schema are written and read exactly, so saves
move between the C++ and Rust builds in both directions until the C++ is
deleted.

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
`resources/text/`. An `xtask` step builds one catalog per module, keyed by
the local 16-bit ID. At run time catalogs load in the order the real
`[Resources]` list gives, so every 32-bit text ID keeps its value. Catalogs
ship as files beside the executable.

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
