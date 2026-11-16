# Library ledger

Every project in `CppSource/OutpostFrontier.sln` and each shared source
group, with its disposition under the [migration plan](migration-plan.md).
The solution has 39 projects: 31 code projects and 8 resource projects.
`IVP/`, `RemoteCommon/` and `include/` are source groups without a project.

Status values: `not started`, `in progress`, `accepted` (its gate tests
pass and the row was reviewed). All C++ is deleted together in P7.3.

Line counts are C++ source and header lines from the assessment, rounded.

## Code projects

| # | C++ project | Lines | Disposition | Rust destination | Work packages | Status |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `neuroncomponents` | 2.8k | Port the INI parser, name CRC and `stricmp`. Delete the component manager and the regular-expression component | `neuron-core::{ini, hash, diag}`; `neuron-client::engine` | P1.1, P1.2, P1.5, P4.16 | in progress: name CRC, `stricmp` and the INI parser ported; oracle vectors confirmed in CI. `GetCRC32(begin, end)` is not ported: it never returns for a non-empty range and has no caller. FDUMP flags and the `ErrorHandler` channels in `neuron-core::diag` (P1.5); `LogStream`, empty in the shipping build, is not ported |
| 2 | `system` | 0.6k | Delete; start-up order moves into the engine constructor | `neuron-client::engine` | P4.16 | not started |
| 3 | `NeuronRuntime` | 2.3k | Rewrite the transport on QUIC. Port the MD5 text form. Delete the DirectPlay wrapper and the list-server wrapper | `neuron-core::net`; `neuron-server::net`; `neuron-client::{net, engine}` | P1.5, P1.9, P3.8, P4.16 | in progress: MD5 text form in `neuron-core::util::md5` and the host error callbacks in `neuron-core::diag::callbacks` (P1.5); framing, handshake, `Transport` trait and loopback in `neuron-core::net` (P1.9). QUIC host and connection follow in P3.8 |
| 4 | `zlib` | 27k vendor | Delete; `flate2` for network batches | `neuron-core::net` | P1.9 | in progress: `flate2` compresses batches in `neuron-core::net::frame` (P1.9) |
| 5 | `ReadFile` | 3.5k | Port the UTF reader; redesign the file-system interface | `neuron-core::{vfs, utf}` | P1.3 | not started |
| 6 | `luaprofile` | 0.7k + 7.9k Lua | Delete; no consumer. Its Lua parser source informs P4.14 | — | — | not started |
| 7 | `x86math` | 1.5k | Port kernels in original operation order | `neuron-core::math` | P1.4 | in progress: every `I3DMathEngine` method ported in `neuron-core::math` (P1.4); oracle vectors equal the C++ output in CI |
| 8 | `MemoryDiagnostics` | 1.2k | Delete; standalone tool | — | — | not started |
| 9 | `qhull` | 24k vendor | Delete; box and pyramid shapes come from `parry3d` | `neuron-core::physics` | P2.3 | not started |
| 10 | `Common` | 105k + 38k headers | Split by sub-module, see below | `neuron-core`, `game-sim`, `game-logic`, `neuron-client`, `game-app` | P1.1, P1.2, P1.5, P1.7, P2.1–P2.5, P4.12 | in progress: IDs, INI and `atof` (P1.1, P1.2); timing, rect, spline, state machine, error handler (P1.5) |
| 11 | `SceneRuntime` | 10k | Port whole; it makes no renderer calls | `neuron-core::{scene, anim}` | P1.8 | not started |
| 12 | `SceneScript` | 18k + 6.6k Lua | Port the timeline engine; replace the Lua C interpreter with a Rust evaluator | `neuron-client::scene_script` | P4.14 | not started |
| 13 | `particlefx` | 25k | Port; data parsing to core, simulation and drawing to client | `neuron-core::fx_data`; `neuron-client::render::particles` | P1.7, P4.8 | not started |
| 14 | `Deformable2` | 8.8k | Port; palette skinning moves into the vertex shader | `neuron-core::mesh_data`; `neuron-client::render::deformable` | P1.7, P4.7 | not started |
| 15 | `surfacematerials` | 5.8k | Port the 19 material kinds and the appearance draws | `neuron-client::render::{material, appearance}` | P4.5 | not started |
| 16 | `Shading` | 11k | Port texture and material libraries, animation, batcher | `neuron-core::image`; `neuron-client::render::{texture, material}` | P1.7, P4.4, P4.5 | not started |
| 17 | `RendComp` | 9.8k | Port render manager and mesh kinds | `neuron-core::mesh_data`; `neuron-client::render::mesh` | P1.7, P4.6 | not started |
| 18 | `DX8RENDERER` | 8.5k | Replace with the Direct3D 12 renderer; its behaviour is the reference for the fixed-function layer. Delete the quirk database | `neuron-client::render::{d3d12, ff}` | P4.2, P4.3 | not started |
| 19 | `DX9RENDERER` | 10.6k | Replace; its extension interface informs the programmable-pass API | `neuron-client::render::pass` | P4.9, P6.4 | not started |
| 20 | `ximage` | 1.0k | Port the large-image tiler | `neuron-client::render::texture` | P4.4 | not started |
| 21 | `SoundManager` | 4.4k | Port the mixer arithmetic and voice ranking; replace DirectSound, EAX and ACM | `neuron-client::audio` | P4.10 | not started |
| 22 | `SoundStreamer` | 2.1k | Port streaming on XAudio2 and `symphonia` | `neuron-client::audio::stream` | P4.10 | not started |
| 23 | `movie` | 1.7k live, 13k stub | Replace with Media Foundation; delete the certificate stub | `neuron-client::media` | P4.15 | not started |
| 24 | `imeui` | 2.7k | Port the edit-box model; replace the custom IME UI | `neuron-client::text_input` | P4.11 | not started |
| 25 | `rpclocal` | 28k | Delete the duplicate codecs; the in-process path becomes the loopback transport | `neuron-core::net` | P1.9 | in progress: loopback transport in `neuron-core::net::transport` (P1.9); the duplicate codecs are not ported |
| 26 | `RemoteClient` (+ `RemoteCommon`) | 3.5k + 5.8k | Port the codecs once; rewrite the server-side connection handling | `game-sim::protocol`; `neuron-server::net` | P2.2, P3.8 | in progress: field primitives and quantisers in `neuron-core::net::wire` (P1.9), oracle vectors confirmed in CI; message codecs follow in P2.2 |
| 27 | `RemoteServer` (+ `RemoteCommon`) | 3.2k | Rewrite the client-side connection handling | `neuron-client::net` | P3.8 | not started |
| 28 | `Server` | 39k | Port; infrastructure and rules are separated | `neuron-server`; `game-logic` | P3.1–P3.3 | not started |
| 29 | `Content` | 78k | Port; mission scripts become enums, random-mission machines are ported as state enums | `game-logic` | P3.4–P3.7 | not started |
| 30 | `OutpostFrontier` | 233k | Port by sub-system, see below | `game-app`; `neuron-client`; `apps/outpost-frontier` | P4.1, P4.11–P4.13, P5.1–P5.10, P6.1, P6.3–P6.5 | not started |
| 31 | `OutpostServer` | 6.3k | Replace the MFC application with a headless server | `apps/outpost-server`; `game-logic::admin` | P3.9 | not started |

## Resource projects

All eight become text catalogs built from sources copied to
`resources/text/` (P1.6; decided 2026-11-16: copied, not moved, so the
reference build is untouched; the copies in `CppSource/Resources/` go with
the C++ in P7.3). Numeric IDs keep their values. Until then both copies
must stay byte-identical: CI compares the DLLs built from `CppSource` with
the catalogs built from `resources/text`.

| # | C++ project | Content | Catalog use | Status |
| --- | --- | --- | --- | --- |
| 32 | `UiText` | 3,648 strings | Client and server | in progress: catalog built from `resources/text/` by `cargo xtask catalog`; equal to the resource DLL in CI (P1.6) |
| 33 | `FieldGuideText` | 886 rich-text cards | Client | in progress: catalog built from `resources/text/` by `cargo xtask catalog`; equal to the resource DLL in CI (P1.6) |
| 34 | `DialogueText` | 3,087 rich-text cards | Client | in progress: catalog built from `resources/text/` by `cargo xtask catalog`; equal to the resource DLL in CI (P1.6) |
| 35 | `NameCatalog` | 7,139 strings | Client display; IDs referenced by game logic | in progress: catalog built from `resources/text/` by `cargo xtask catalog`; equal to the resource DLL in CI (P1.6) |
| 36 | `EquipmentCatalog` | 784 strings, 784 cards | Client | in progress: catalog built from `resources/text/` by `cargo xtask catalog`; equal to the resource DLL in CI (P1.6) |
| 37 | `NegotiationText` | 1,270 strings | Client display; IDs referenced by game logic | in progress: catalog built from `resources/text/` by `cargo xtask catalog`; equal to the resource DLL in CI (P1.6) |
| 38 | `InterfaceExtras` | 495 rich-text cards | Client | in progress: catalog built from `resources/text/` by `cargo xtask catalog`; equal to the resource DLL in CI (P1.6) |
| 39 | `serverinterface` | 107 strings; dialogs, menus, bitmaps | Strings feed the admin console; dialogs are dropped | in progress: strings only, built as for the others and equal to the DLL in CI (P1.6); dialogs, menus and bitmaps dropped |

## Source groups

| Group | Lines | Disposition | Work packages |
| --- | --- | --- | --- |
| `IVP/` (compiled into `Common`) | 50k | Delete. Only the compact-surface layout is kept, as a file format | P1.7, P2.3 |
| `RemoteCommon/` | 5.8k | Port once as the protocol codecs | P2.2 |
| `include/` | 57k | Each header follows its owning library. `msvcrt_compat.h` ports in P1.1; `msvcrt_numeric_format.h` ports with its users in P4.13 and P4.14; `x87_trig.h` is not ported (decision T11) | per owner |
| Build files (`Directory.Build.*`, `*.vcxproj`, `*.props`, Python scripts) | — | Not ported. `import_windows_resources.py` is superseded by `cargo xtask catalog` | P1.6 |

## `Common` by sub-module

| Sub-module | Lines | Rust destination | Work package |
| --- | --- | --- | --- |
| IDs | 0.7k | `neuron-core::id`. Ported; oracle vectors confirmed in CI | P1.1 |
| String cache | 0.1k | `game-sim::data`. Moved from P1.1 on 2026-11-16: all its users arrive in P2.1 | P2.1 |
| INI, `atof`, text files | 3.7k | `neuron-core::ini`, `neuron-core::msvcrt`. Ported, text INI only (decision O13); oracle vectors confirmed in CI, and the dump of the master's INI files in `outdir\` equals the oracle (2026-11-16) | P1.2 |
| Utilities (timing, rect, spline, heap, async, state machine) | 4.9k | `neuron-core::{time, util, diag}`. Ported: `Timing`, `Delta`, rect, Catmull-Rom spline, state machine, `ErrorHandler`; oracle vectors confirmed in CI. The pool allocator, async shim and critical-section wrapper are deleted: nothing they do is observable | P1.5 |
| Constants, data config | (in above) | `game-sim::data` | P2.1 |
| Archetypes, loadouts, fuses | 12.2k | `game-sim::data` | P2.1 |
| Universe, zones, asteroids, bases, spatial | 13.2k | `game-sim::data` | P2.1 |
| Goods, reputation, trade | 5.3k | `game-sim::data`; rules in `game-logic` | P2.1, P3.2 |
| Wire types, marshal buffer, format strings, costume | 4.9k | `game-sim::protocol` | P2.2 |
| `.sur` loader | (in physics glue) | `neuron-core::sur` | P1.7 |
| Physics glue | 10.4k | `neuron-core::physics` | P2.3 |
| Object model | 15.3k | `game-sim::object` | P2.4 |
| Equipment | 11.3k | `game-sim::equip` | P2.4 |
| AI | 51.2k | `game-sim::ai` | P2.5 |
| Rich-text I/O and text rendering | 6.5k | binary form in `game-sim::protocol`; rendering in `neuron-client::text` | P2.2, P4.12 |
| Costume, animation and action databases, new character | 2.7k | `game-sim::data`; client-only parts in `game-app` | P2.1, P5.9 |
| Player account identity | (in net types) | `neuron-client::platform` | P4.1 |

## `OutpostFrontier` by sub-system

| Sub-system | Lines | Rust destination | Work package |
| --- | --- | --- | --- |
| Start-up, window, message pump, timing, engine init, loading screen | 5.0k | `apps/outpost-frontier`; `neuron-client::platform`; `game-app::boot` | P4.1, P5.1 |
| Client state machine and flow | 6.5k | `game-app::flow` | P5.1 |
| Networking glue, login, connect flow | 6.7k | `game-app::session`; `neuron-client::net` | P5.3, P6.1 |
| Client game objects and object database | 19.8k | `game-app::world` | P5.4 |
| Space rendering glue and effects | 46.7k | `neuron-client::render` (generic layers); `game-app::world` (game rules) | P5.6, P5.8, P6.4 |
| Cameras | 8.6k | `game-app::camera` | P5.4 |
| HUD and chat | 28.3k | `game-app::hud` | P5.7 |
| Base screens and shared windows | 53.1k | `game-app::neuronet`; generic controls to `neuron-client::ui` | P4.13, P5.10 |
| Front-end pages, options | 9.6k | `game-app::frontend`, `::options` | P5.2 |
| UI toolkit, fonts | 23.1k | `neuron-client::{ui, text}` | P4.12, P4.13 |
| Base rooms, scene-script glue, characters, walk mode | 16.1k | `game-app::base` | P5.9, P6.3 |
| Input, key map, IME glue | 3.3k | `neuron-client::{input, text_input}`; `game-app::options` | P4.11, P5.2 |
| Audio glue | 4.8k | `neuron-client::audio`; `game-app` triggers | P5.5 |
| Text ID lookup | 0.9k | `neuron-core::catalog`. Ported: the module table and the resource resolution of `GetIdsStringW`, `LoadIdsString`, `GetIdsStringWWithRawFallback` and `LoadIdsIntoRdl`. `GetIdsStringA` (ANSI code page) goes to client platform code, the mad-lib formatter to P2.2 or P5, the rich-text node building to P4.12 | P1.6 |
| Tick profile, widescreen helpers | 0.5k | `neuron-core::diag::tick_profile` behind feature `tick-profile` (sections and accumulator, P1.5); the overlay and widescreen helpers in `neuron-client::ui` | P1.5, P4.13, P6.5 |
