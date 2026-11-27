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
| O23 | Decided 2026-11-17 (P2.0). The scenario and replay oracle is a headless `Server.dll` run inside `Oracle.exe` (`sim` mode) with a stub client DLL and inert-by-default hooks. The envelope method is fixed now (N = 20 oracle runs, bound = max(2 × spread, floor)); the numbers follow from harness runs and are approved before Gate G2. Amended 2026-11-19 (owner): the runs proved identical, so the method is a seed ensemble (20 fixed seeds at the seeding point on both sides, each judged exactly to its first contact or impact, the outcome statistics of the seeds compared after it; `ram_ship`'s first-contact damage within 5 %). Details in [adr/0003](adr/0003-simulation-oracle.md) | A driver of its own over Common; deciding the method only after measurements |
| O24 | Decided 2026-11-18. `BuildOrientationBasis` (`Common/common_aisteer.cpp`) zero-fills its result matrix, as the reconstruction's own notes say the original does (`local_48`, zero-filled and copied out on every path). The reconstruction declared `Matrix Result{}`, which leaves it uninitialised because `Matrix` has a user-provided empty constructor, so a forward shorter than 0.01 returned stack contents; the `ai_steer` oracle suite showed it. Fixed in the C++ reference, a recorded exception to the `CppSource/` freeze like O12, O14 and O17; `game-sim::ai::steer` already returns the zero matrix. The other `Matrix X{}` sites in Common are left as they are: their values are overwritten before use, or (`common_faceidea.cpp`) the original is uninitialised too on an unreachable path | Leaving the C++ alone, dropping the degenerate inputs from the suite and recording a divergence; zero-filling every `Matrix X{}` site |
| O25 | Decided 2026-11-18 (P4.0). The renderer is one `render::Device` value over a platform-free state model and a `cfg(windows)` Direct3D 12 backend, with typed states and generational handles; renderer traces move to format version 1 with model replay on any host and image replay on Windows, compared by the metric of the record (starting numbers `t = 8`, `p = 0.5 %`, `m = 1.5`); shaders are DXBC from `fxc` at shader model 5.1 with fixed flags; all FFI lives under `neuron-client/src/platform/`. Up to sixteen lights per draw, BMP and PNG screenshots, two emulated texture stages. Details in [adr/0004](adr/0004-render-device-and-trace.md) | A higher-level renderer API first; raw state numbers in the API; `dxc` and shader model 6; recording resolved draws instead of interface calls |
| O26 | Decided 2026-11-18 (P4.10). Voices of 3D archetypes are spatialised by X3DAudio on the device side: the mixer hands each one its pose and the listener (`VoiceBackend::set_emitter`) and leaves distance attenuation, pan and Doppler to X3DAudio, in place of the reference's DirectSound 3D buffers; the reference's software path stays for 2D and pan voices and as an option (`Spatial::Software`) for the oracle. Environmental reverb (the EAX settings mapped to XAudio2's I3DL2 reverb) is applied whatever `useEAX` says. Audio uses the Windows default output device and its speaker layout; `deviceID` and `speakerConfiguration` are read without effect | A platform-free emulation of DirectSound 3D's inverse-distance law, Doppler and cones; the reference's software path for 3D voices (about 3 dB per doubling of distance instead of DirectSound's 6 dB, no Doppler); reverb only with `useEAX` on, as shipped; dropping reverb; honouring `deviceID` and `speakerConfiguration` |
| O27 | Decided 2026-11-18 (P4.1). The client window is per-monitor DPI aware (version 2): a windowed client area is the mode's size times the monitor's scale, and fullscreen is a borderless window over the monitor (no display-mode change, not topmost); in both the back buffer keeps the mode's size and the swap chain stretches it. Per-user state lives in TOML files under `Documents\My Games\OutpostFrontier`, written atomically; the first run without them imports the C++ client's `HKCU\Software\Microsoft\Microsoft Games\OutpostFrontier\1.0` once and never writes the registry. Saved passwords are sealed with the Windows user's DPAPI. A new install derives the multiplayer account name as the C++ does (MD5 of the Windows `DigitalProductId` and the user name, a UUID without a product id) | Hex-text passwords as the C++ kept them; not saving passwords; exclusive fullscreen through DXGI, or both modes; physical-pixel or window-sized back buffers; a random stored account identity |
| O28 | Decided 2026-11-18 (P4.11; proposed and accepted by the owner the same day). Raw Input stands in for the C++'s exclusive, foreground DirectInput mouse: while the game holds the mouse (the window has the focus and the cursor is on the surface), the mouse is registered without legacy messages and captured (`RIDEV_NOLEGACY`, `RIDEV_CAPTUREMOUSE`), the system cursor is hidden and the pointer is clipped to the client area; when the game lets the mouse go (focus lost, deactivation, or a windowed cursor leaving the surface) the registration is plain again, the clip is lifted and the pointer appears where the software cursor was, as in the C++. Cursor positions read from or written to the system pointer are converted between client and surface pixels (O27) | Leaving the hidden pointer free (clicks could reach other windows and `WM_MOUSEMOVE` would keep re-seating the cursor); recentring the pointer with `SetCursorPos` each frame; reading motion from `WM_MOUSEMOVE` (pointer ballistics, no motion at the screen edge) |
| O29 | Decided 2026-11-18 (P4.12; proposed and accepted by the owner the same day). The faces `fonts.ini` and `rich_fonts.ini` name are not all on a Windows machine: `Agency FB` and `Arial Unicode MS` came with Microsoft Office, `Vibrocentric` (the rich-text defaults) with nothing, and `FixedSys` is a raster face DirectWrite cannot draw. The C++ client drew whatever GDI's font mapper substituted. Decided: keep the names in the data; where DirectWrite lacks a face, substitute by a fixed table in the backend (`Agency FB` to `Bahnschrift SemiCondensed`, which ships with Windows 10 and later and is a condensed sans like it; `Arial Unicode MS` and `Vibrocentric` to `Arial`; `FixedSys` to `Consolas`), Arial for anything else; and load font files placed under `DATA/FONTS` as a private DirectWrite collection first (decision T3), so an installation that has licensed faces can use them. The autoscale search fits each font to the pangram's width share, so a substitute keeps the layout's widths; heights and glyph shapes change. `platform::dwrite` implements it (`text::font::substitute`, `DWriteBackend::with_font_dir`; divergence D290) | Arial for every missing face (as implemented now); shipping `Agency FB` and `Arial Unicode MS` (their licences do not allow it); a free look-alike font committed to the data tree; GDI kept for text (decision T3 and the policy scan exclude it) |
| O30 | Decided 2026-11-18. Three string literals that the rebrand commit `89eda18` renamed name data it did not rename, so the reference could not find them: the default Veilborn texture `VeilbornRGB1_VeilbornAlpha1` (`surfacematerials/MaterialImpl.cpp`; `SHIPS/NOMAD/nomad_fx.txm` and `no_gunship.3db` hold `NomadRGB1_NomadAlpha1`, the client logged `texture verify failed: nomad surface`), the navmap mesh `zoomedmap_helion.3db` (`OutpostFrontier/Client/nn_navmap.cpp`; the file is `zoomedmap_liberty.3db`) and the HUD contact icon `HUD_contactveilborn.3db` (`hud_contact_list.cpp`; the file is `hud_contactnomad.3db`). The literals take back their spelling from before `89eda18`, an approved change to the frozen reference like O12, O14, O17 and O24. File names keep the old spelling (O17), and a texture name inside a binary model is matched against its own data. Found by the naming analysis ([naming-analysis.md](naming-analysis.md)) | Renaming the two files and the texture inside the `.txm` and `.3db` to the new spelling |
| O31 | Decided 2026-11-18. `FVoiceProfile::ResolveScriptFile` (`OutpostFrontier/Client/voice_profiles.cpp`) stores the voice archive's path relative to the data root (`audio\<nickname>.utf`) instead of the full path it tested with `_access`. `OpenScriptFileSystem` hands that path to `NeuronRuntime::Data`, whose UTF factory joins it to the data root a second time (`ReadFile.cpp`, `_makepath(root, path)`); the full path resolves only when the data path is a single `..\NAME`, as the shipped `..\DATA` was. With `data path= .\data` no archive opened and every voice line failed with `Sound filename "0x…" not found`. Shown with `Oracle.exe`'s UTF script: `.\data\audio\FP7_setscene.utf` fails, `audio\FP7_setscene.utf` opens and its entry `0x9F48A900` reads as a WAV. An approved change to the frozen reference like O30 | A data path of the form `..\data` beside `bin` (no code change, but one layout only); joining paths correctly in the UTF factory (changes `ReadFile` behaviour the vfs vectors pin) |
| O32 | Decided 2026-11-18. `UNIVERSE/universe.ini` gives system `ep01` the `msg_id_prefix = gcs_refer_system_ep01` it lacked, as the shipped data did; the voices hold the same 17 `gcs_refer_system_ep01-` lines as for `he01`. The other six systems without a prefix (`ep02`, `ep03`, `ep03b`, `ep02c`, `oe05`, `oe06`) have no such lines and stay as shipped, so the client still logs `has no msg_id_prefix information` for them | Leaving `ep01` as shipped |
| O40 | Decided 2026-11-20 (P3.0). Controller messages are dispatched synchronously and depth first, as the C++ does: one server context passed by `&mut`, in which no borrow outlives a call that can dispatch; the controller registry, heartbeats and timers ported exactly into `neuron-server::controller`, generic over a host trait; a closed set of controller kinds selected by the `[Initial SP/MP DLLs]` rows; typed messages whose out-parameters the sender reads back; the world calling the server through hooks at the C++ dispatch points; walks over lists a nested call can change follow `MetaList` and `std::list`, continuing from a removed current node's old successor where the C++ reads freed memory (a divergence row per site). Verified by the `controller_bus` scheduler vectors and the `sim` mode's dispatch trace. Details in [adr/0006](adr/0006-controller-bus.md). Number chosen by the owner after the O33 to O39 cited elsewhere | A FIFO event queue; a queue drained at each send; moving the controller out of its slot for the call; `Rc<RefCell<..>>`; actors or async tasks; dynamically loaded controllers; `WorldEvent` as the only path from world to server |
| O41 | Decided 2026-11-20 (P3.1). Three working rules for Phase 3. (1) The platform-free core of `CPlayerAccount` (the MD5 account texts, `GenerateAccount`, `LoadAccountFromStrings`, `VerifyAccount`, `GetServerSignature`) moves from `game-app::account_keys` to `neuron-core::account`, so the multiplayer server's `accounts.cfg` account and the client share one port; `game-app` keeps the registry import and the machine queries. (2) `neuron-server` takes the accounts root as a parameter; on Windows the executables give it the C++ location, `Documents\\My Games\\OutpostFrontier\\Accts` (`\\MultiPlayer` for a dedicated server), so saves interchange with the C++ build, and the dedicated server's config (P3.9) can override it. (3) As in Phase 2, Claude opens one or more pull requests per work package and merges a Phase 3 pull request once its CI is green; new decision records and divergence rows still wait for the owner | A second copy of the account code in the server; `game-sim` as its home; a root beside the executable or one given only by configuration; the owner merging every pull request |
| O42 | Decided 2026-11-20 (P3.1). The dedicated server answers the machine queries of its `accounts.cfg` account (`neuron_core::account::MachineIdentity`: the `DigitalProductId`, a UUID, the user name, a millisecond count) with its own `cfg(windows)` implementation in `apps/outpost-server`, which mirrors `neuron-client::platform::machine::WinMachine`; the executable may call Windows APIs, and the crate graph stays as it is. Amended 2026-11-25 (owner, P3.9): T14 stands, so the executable stays free of unsafe code and reaches the platform through small safe wrapper crates (T13): the local time through `jiff`, the interrupt handler through `ctrlc`, the Documents folder through `dirs`. The `DigitalProductId` is not read: `neuron-server::machine::PortableMachine` answers the queries without it (D914) | An `outpost-server` dependency on `neuron-client`; a new platform crate shared by both executables |
| O43 | Decided 2026-11-20 (P3.3). Record [0007](adr/0007-server-world.md) is accepted as written: server object state in `game-logic` side tables keyed by handle; the wrapper update as a world hook; a `game-logic` `StarSystem` holding the observers and the dock queue over game-sim's per-system lists; lookup through the world's id map; in-transit objects kept in the arena without a system; lazy loading, with single-player unloading; replication as per-player observers; single player finding the client's objects; `pub::` as functions on the context returning the C++ codes. P3.3 is split into several pull requests, one per coherent part (base database; systems, loading and transfer; object state, fuses, loot and munitions; docking; replication), each merged by Claude on green under O41 | One pull request for the whole work package; server state as game-sim components; one world per star system |
| O44 | Decided 2026-11-20 (P3.3). The platform-free part of the scene-script port moves from `neuron-client::scene_script` to `neuron-core::scene_script`: the chunk reader, the Lua 3.2 evaluator, the parser's two passes (`load`, `build`), the entity and event descriptors, the param curves and the splines, with the UCRT `atof` they parse with (`neuron-core::msvcrt::ucrt_atof`). The scene's playback and the script engine stay in `neuron-client`, which re-exports the moved modules at their old paths. The server's room data (`RoomData::read_set_script`, in game-sim) reads a room's set script through it | A new shared crate for the scene scripts; porting the base database without character placements until Content needs them |
| O45 | Decided 2026-11-21 (P3.3, the object part). The scenario replay's driver moves from `crates/game-sim/tests/` into the game-sim library as a public module that takes a `&mut dyn ServerHooks`. game-sim's own replay keeps `NoServerHooks`; a game-logic test runs the same scenarios through the server's hooks, and the data run compares it with the oracle too, so the server's object work is checked against the C++ scenarios. No new crate. |
| O46 | Decided 2026-11-21 (P3.3, the object part). The C++ wrappers' per-kind damage overrides (`Solar::damage_sub_obj`, `munition_hit` and `explosion_hit_hull` with their `ObjectDamaged` and behaviour events; the immunity of the asteroid fields' rocks and mines; counter-measures and mines destroyed by any hit; `Ship::apply_damage_list`'s cause 6) are server behaviour: game-sim's damage computation offers a per-kind hit seam in `ServerHooks`, and game-logic implements the overrides (record 0007 point 1). The bare world keeps its behaviour. |
| O47 | Decided 2026-11-22 (P3.4 to P3.7; the record's questions 1 to 3 and 5 to 10 as recommended, question 4 on NPC looting answered 2026-11-24: the reconstruction's case-sensitive test is kept). Record [0008](adr/0008-content.md) proposes how Content.dll is ported into `game-logic`. Content's state is one `Content` value in `Logic` (record 0006 point 3), in three parts: the tables the C++ never changes after loading, as an immutable `Arc<ContentTables>` (the random-mission tables and vignette tree, which a static object loads with the DLL, as `Arc<RmData>` in `Content::rm`; amended 2026-11-25 with P3.7a); the live globals as `ContentState`; and each controller's own state by controller id. A borrow ends before any call that can dispatch, and an item is taken out of its slot only for a call that cannot dispatch. The 23 factory names map onto per-kind modules and instance variants, and `CreateParams` gains the story, mission and random-mission forms. The router is ported as it is: immediate delivery is a nested call, delayed delivery uses the sorted queue and a bus timer on SpaceSupport's id, and the state machines are driven through a context-passing host. Content's callback objects get handles from 1 that are never reused and never `u32::MAX` (D710, D722). Content callbacks raised in the behaviour tick are queued with the AI's server requests, and the two that answer are answered read-only just before the tick (amended 2026-11-25 with P3.6c); shared directives live in a counted arena behind an opaque `DirectiveRef` (owner, 2026-11-25). The static tables are typed data from `outdir/`, compared with oracle dumps (Tier A). Conditions (44) and actions (85) are enums, with Tier A parse dumps and save round trips and a Tier B run time. Population and NPCs are Tier B. Random missions are state enums on the one CRT generator, with offers rebuilt from a seed (Tier A); the state outside the seed (three counters, the faction name reservations, `GRmDestSystems` and `GRmObjectiveZones`) lives in `ContentState`, and the dump compares the counters as outputs (amended 2026-11-25 with P3.7a). The increments are P3.4a to P3.7c. Ten questions are open for the owner (the record's "Consequences and open questions") | A separate content crate; all state per controller; Content as a third field of `Cx`; static globals; trait objects for conditions and actions; scripts parsed on use; router delivery driven by the heartbeat; a separate generator for random missions; content callbacks as trait objects in game-sim; `Rc<RefCell<..>>` for NPC ships |
| O48 | Decided 2026-11-25 (Phase 3 process, after the pace fell to a few merges a day). (1) Packages that share no unmerged work run on their own branches and pull requests at the same time: P3.8 and P3.9 beside Content. Each branch takes its own range of divergence numbers, given when it starts. (2) Gate G3's criteria come first: P3.7a, P3.7c, P3.8, P3.9 and the gate's scenarios, before P3.5c, P3.5d, P3.6b, P3.6c's remaining guides, name builder and `GenerateCargo`, P3.6d, P3.6e and P3.7b. The milestone is reached when the gate's criteria pass; those packages finish Phase 3 after it. (3) A standing rule for divergence rows. A row is accepted without asking first when either (a) the C++ has undefined behaviour there (a freed, null or out-of-bounds access it never tests, or an `int3` it executes; the `int3` added by the owner on 2026-11-25 with D1002; a C++ hang, an infinite loop that the shipped data never reaches where the port terminates, added by the owner on 2026-11-26, as D115 was accepted) and the port's choice makes no difference on the shipped data, or (b) it is temporary and names the package that removes it. The pull request lists such rows, the owner reviews them after the merge, and the owner may reopen any. Every other row, and every change to a record, still goes to the owner first. (4) The Phase 2 seed ensemble's oracle outputs get a cache of their own, keyed on the sources the ensemble runs. The nightly data run rebuilds them without it. | Content's packages first on one branch; every row asked before its merge; the ensemble rerun on every C++ change; the ensemble run only on `main` |
| O49 | Decided 2026-11-25 (P3.9). The client messages no package ported (D918) are given two new packages. P3.10a, before Gate G3: the flight and combat handlers of P3.3's scope (`FireWeapon`, `ActivateEquip`, `ActivateCruise`, `ActivateThrusters`, `SetTarget`, `TractorObjects`, `GoTradelane`, `StopTradelane`, `JettisonCargo`, the client's collision reports, `MineAsteroid`, `SPRequestUseItem`, `SPRequestInvincibility`, `SetManeuver`, `Dock`), so the milestone's server takes a player's fire, cruise, dock and lane requests. P3.10b, after the milestone: the player-state requests of P3.2's scope and the player trades (`PLAYER_TRADE`, `TRADERESPONSE`), which no Phase 3 package held. | All handlers before the gate; all after it, the gate met with the inputs dropped |
| O50 | Decided 2026-11-25 (after Gate G3), amended the same day by the owner. Phase 5 (the game client) runs beside the rest of Phase 3, in a **separate session that the owner runs**; the session that carries Phase 3 does no Phase 5 work. The Phase 5 session owns `game-app`, `apps/outpost-frontier`, the client parts of `neuron-client` it needs, its design record (the game client's record, decided by the owner before P5.1) and the check whether the C++ client's `+oracle` mode can run in CI. The Phase 3 session owns `game-logic`, `neuron-server`, `apps/outpost-server`, the server and Content parts of `game-sim`, and Content's oracle instrumentation. Both sessions follow O48 (own branches and pull requests, never the other's branch; the standing rule for divergence rows). To keep the shared registers apart: Phase 3 takes divergence numbers D850 to D1099 and Phase 5 D1100 to D1999; decision numbers are taken at merge time, the next free O-number, and a pull request renumbers its own unmerged entry on a conflict; a change to a crate or file the other session owns, or to `game-sim`'s shared object model or protocol, is named in the pull request so the other session can merge it in. P5.1 to P5.8 depend only on Phase 4 and on server parts already merged; P5.9 and P5.10 wait for P3.6e and P3.7b on their server side, and Gate G5 for the whole of Phase 3. | Phase 5 only after Phase 3; Phase 5 after P3.5c; Phase 5 in the same session as Phase 3, capped at two packages; the client oracle runs only on the owner's machine |
| O51 | Decided 2026-11-26 (P3.6d; proposed and accepted by the owner the same day). The dev profile builds `neuron-core` and `game-sim` at `opt-level = 2` (`Cargo.toml`). With Content's NPC population the real server starts SpacePop, and the unoptimised debug server fell behind its frames: each longer frame stepped more (the C++ dedicated server caps nothing either, `OutpostServer.cpp`'s loop hands `ElapseTime` the raw elapsed time), so Gate G3's `g3_client` timed out in the debug `cargo test`. No result changes: Rust contracts no float operations at any optimisation level, and debug assertions and overflow checks stay on. Not chosen: running `g3_client` in optimised builds only, and a frame-time cap (a divergence from the C++ server). Corrected 2026-11-26 by the owner, who keeps the setting: the hot spot was neuron-core's ray query, not `game-logic` (`PhysicsWorld::find_ray_collisions` ran `ivp_ray_hits` on bodies its sphere test had culled), fixed on the P3.6d branch; with the fix the debug `g3_client` passes in about 4 s with this setting and in 9 to 11 s without, and the setting stays for the margin on the Windows runner. |
| O52 | Decided 2026-11-26 (Phase 3 closure; proposed and accepted by the owner the same day). (1) A new package, **P3.11 "server closure"**, runs on its own branch beside the Content work (O48 (1)). It holds the scope of P3.2 and P3.3 that Gate G3 did not need and that is still unported: local chat through the system database; a multiplayer re-selection leaving the old system and base, and `PlayerData_DeselectCharacter`; the save writer's gap of a multiplayer ship's position from its remote physics controller; the remote correction of the other objects a player owns (loot, mines, counter-measures, guided munitions); the rest of the character list's multiplayer prologue; the routing of the `SAVEGAME` packet; the removal of D125 and D126, which the plan gave P3.3; a review of D158, D165, D179, D180, D188 and D197, each removed, narrowed, or given an explicit later remover; and the oracle scenario of a ring launch, built as a test (the plan named it for Gate G3, which was signed off without it). (2) Phase 3 is closed when every Phase 3 package meets the definition of done ([verification.md](verification.md)), the progress table of the [README](README.md) and the [ledger](library-ledger.md) say so, and no temporary row of the [divergence register](divergences.md) names a Phase 3 package as its remover. | Amended by the owner on 2026-11-26 after P3.11: a follow-up package, **P3.11b**, takes what P3.11 leaves, the equipment's loot and debris (D126's rest), the attitudes from the reputation groups (D165's rest), the scan list's solar order (D180) and the pathfinder's bucket order (D197), judged with the fights against the oracle; Phase 3 does not close without it. Amended again on 2026-11-26 after P3.5c5: a package **P3.7d** takes the random missions' single-player saves and load, respawns and jumpers (D1024's rest); Phase 3 does not close without it. Amended again on 2026-11-26 before closing Phase 3: a package **P3.11c** fixes the guided munitions' impact and explosion order found by P3.11b; Phase 3 does not close without it. Amended again on 2026-11-26 after the Phase 3 closing audit: a package **P3.11d** closes the gaps the audit confirmed in code, the tractored loot (`StarSystem::tractor_loot`), the field mine's explosion fan-out, the behaviour manager's server link (the events 3, 5 and 7), an explosion's damage to objects without equipment, `FORMATION_UPDATE`, P3.4a's exact check of the `TimerExpired` deliveries against the oracle and the beam diagnostics that grow on the server; Phase 3 does not close without it.
| O53 | Decided 2026-11-26 (Phase 5, before P5.1; proposed in record [0009](adr/0009-game-client.md) and accepted by the owner the same day). (1) `game-app` holds the client's state in one `ClientContext` and reaches the in-process server through a `Session` trait it defines and `apps/outpost-frontier` implements over `game-logic`'s `ServerHost` and the loopback, in rpclocal's frame order (the client's inbox, the server's pump, the server's time step); a modal dialog runs the pump without the time step. (2) The Rust client has its own `+oracle` mode, always compiled and switched on by the command line, with the C++ mode's script language and files. (3) The C++ client's `+oracle` mode sets the scenario oracle's fixed-clock hooks (`GOracleSimHooks`), so the in-process server's time, seed and damage clock follow the oracle (oracle instrumentation, `oracle_mode.cpp`). (4) Trees, layouts and key maps are compared leaving out `rtti` and the start record's paths, `flags` bits 0 to 3; IClient payloads with the costume's accessory slots beyond `AccessoryCount` and `FServerBurnFuseInfo`'s padding masked; screenshots by record 0004's Tier C metric; any other mask (owner, 2026-11-27) only for an open temporary divergence row, whose ID it names in the code, over exact records and fields (no wildcard over a dump), with the fields it hid counted per row in the comparison's summary, and removed in the pull request that removes the row. (5) Gate G5's screen comparisons run in CI (`.github/workflows/client-oracle.yml`), from a user folder a warm-up run made; the owner compares one CI artifact with a local run once. | `ServerHost` taking `&mut World` per call; the client frame split into functions the executable sequences; a synchronous loopback; the Rust oracle mode as a Cargo feature or without masks; in-game sessions compared by call names only; comparisons on the owner's machine only |
| O54 | Decided 2026-11-26 by the owner (Phase 5, after Phases 3 and 4 closed; amends O50). (1) Phase 5 runs as three parallel sessions, each on its own branches and pull requests: **A**, front end and screens (the rest of P5.1, P5.2, then P5.10), divergence rows D1100 to D1399; **B**, space (the rest of P5.3, P5.4, then P5.6, P5.7 and P5.8), D1400 to D1699; **C**, audio and bases (P5.5, P5.9), D1700 to D1999. A change to a module another stream owns, to `game-sim` or to `game-logic` is named in its pull request; decision numbers stay the next free O-number at merge time (O50). (2) Claude merges a Phase 5 pull request once Rust CI, the data run and the client-oracle job are green; new decision records and decisions still wait for the owner, divergence rows follow O48 (3), and the owner reviews after the merge. A pull request is one increment. (3) Sessions may download the client-oracle job's artifacts (the environment allows GitHub's artifact storage hosts) and compare the C++ client's files locally; the files are made from the game data and are never committed (decision O15). (4) The client-oracle job caches the C++ runs' outputs keyed on `CppSource/` and the oracle scripts. | One session (O50); two streams; the owner merging every pull request; comparisons in CI only |
| O55 | Decided 2026-11-27 by the owner (Phase 5, amends O54). (1) The first stream A session conducts the three streams: each stream sends its questions for the owner to it, it puts them to the owner and relays the answers quoting the owner verbatim, and it checks the streams every hour and asks an idle stream with work left to continue. (2) Only main's runs and the nightly run save the data run's oracle and seed-ensemble outputs; a pull request restores what main saved and saves none of its own. The C++ leaves some recorded fields as the heap held them (D178), so two fresh oracle runs can disagree, and a pull request's own save shadowed main's entry for the same key in its later runs (#190's data run, 2026-11-27). | Each stream asks the owner itself; pull requests save their own oracle outputs |
| O56 | Decided 2026-11-27 by the owner (Phase 5, amends O54 (1)). The streams are rebalanced, because B held about two thirds of Phase 5 (P5.4, P5.6, P5.7 and P5.8 are all XL). **A**: P5.1, P5.2, P5.10, then P5.7 (the HUD), starting with its window trees. **B**: the rest of P5.3, then P5.4 in two parts, P5.4a first (the client object database, the star system's load and the cameras: the interface the other streams read) as its own early pull request, then P5.4b (game objects, flight controls), then P5.6. **C**: P5.5, P5.9, then P5.8 (the environment), starting with its data readers before P5.4a is on main. The divergence ranges stay with the streams, so a row takes the range of the stream that writes it. A module P5.4a owns is changed by the other streams only through a pull request that names it (O54 (1)). Planned end: 10 to 12 days from 2026-11-27, against two to three weeks with B carrying P5.7 and P5.8. | P5.7 and P5.8 stay with B; only P5.8 moves to C |
| O57 | Decided 2026-11-27 by the owner (Phase 5 CI throughput; amends O54 (2)). (1) Rust CI runs the workspace's tests with cargo-nextest (`--profile ci`, `.config/nextest.toml`), which runs the test binaries in parallel; the tests themselves are unchanged, and `cargo test` stays the local check. (2) On a pull request the data run runs only when the pull request changes the crates its tests build (neuron-core, game-sim, game-logic, neuron-server, neuron-client, lua32-compiler), the C++, the data, the build's settings or `data.yml`; otherwise its job is skipped, which counts as passed. Pushes to main, the nightly run and manual runs always run it. No Phase 1 to 4 suite is removed. (3) The streams' pull requests leave the progress rows of `design/README.md` alone and say their progress in the pull request; the conductor (O55) updates the rows once a day, because every merge made the other open pull requests conflict there and rerun the whole of CI. (4) A stream pushes to an open pull request once per increment, after its local checks, and opens increments that are ready together as one pull request with a commit each. | Removing the Phase 1 to 4 suites from pull requests; sharding the Windows tests across runners (measure nextest first) |

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

Amended 2026-11-19 (owner): the compiler is `dxc` and the shaders are
DXIL of shader model 6.1 (record 0004, the amendment after point 18),
pinned to the `Microsoft.Direct3D.DXC` NuGet package 1.9.2609.5.

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
  xtask's chunk compiler (the `lua32-compiler` crate since 2026-11-19;
  reworded 2026-11-18, P4.13: "port with their
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
  files of P4.1; `tests/rdl_cards.rs` reads the card sources, P4.12), and
  `apps/outpost-server` (the dedicated server's configuration, P3.9).
- `flate2` 1.1 (MIT or Apache-2.0; `rust_backend`, that is `miniz_oxide`):
  `neuron-core::net::frame`, compressed batches only (P1.9).
- `md-5` 0.10 (MIT or Apache-2.0; default features off):
  `neuron-core::util::md5` (P1.5). The C++ MD5 was checked against RFC 1321
  and is standard.
- `tracing` 0.1 (MIT; `std` feature only, no attribute macros):
  `neuron-core::diag` (P1.5), and the transports' notices in
  `neuron-server::net` and `neuron-client::net` (P3.8), and the dedicated
  server's log (`neuron-server::log::ServerLogSubscriber`, its set-up in
  `apps/outpost-server`, P3.9).
  `tracing-subscriber` is not used; the channel subscriber is written in
  `diag`.
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
- `quinn` 0.11.12 (MIT or Apache-2.0; default features off,
  `runtime-tokio` and `rustls-ring`), with `quinn-proto` and `quinn-udp`:
  `neuron-server::net` and `neuron-client::net`, the QUIC transport (P3.8).
  `tokio` 1.54.1 (MIT; default features off, `rt`, `net`, `time`: one
  current-thread runtime on each transport's I/O thread; the hand-over to
  the polling thread is `neuron-core::net::wake`, so `sync` is not
  enabled). `rustls` 0.23.46 (Apache-2.0, ISC or MIT; default features off,
  `std` and `ring`: TLS 1.3 only, the `ring` provider, no TLS 1.2, no root
  store; the client's verifier pins the server's certificate, T16).
  `rcgen` 0.14.10 (MIT or Apache-2.0; default features off, `crypto` and
  `ring`): the server's self-signed certificate, and a dev-dependency of
  `neuron-client` for its tests' stand-in server. The `ring` 0.17 provider
  (Apache-2.0 and ISC) needs a C compiler and no assembler; `aws-lc-rs`,
  which needs CMake and NASM on Windows, is not in the tree. Accepted by
  the owner on 2026-11-25 (P3.8); `deny.toml` needed no change.
- `jiff` 1.0.3 (Unlicense or MIT; default features off, `std`,
  `tz-system`, `tzdb-bundle-platform`, `tzdb-zoneinfo`), with `jiff-core`,
  `jiff-tzdb-platform` and `jiff-tzdb` (the bundled time-zone database,
  on Windows only; Unlicense or MIT) and `windows-link`: the dedicated
  server's local clock (`apps/outpost-server::clock`, `GetLocalTime` for
  the saves; decision O42 as amended). The system zone is read without
  the environment races `time`'s `local-offset` guards against, so it
  works in a process with threads. Accepted by the owner on 2026-11-25 (P3.9).
- `ctrlc` 3.5.3 (MIT or Apache-2.0; default features off, `termination`),
  with `nix` 0.32 (MIT) on Unix and `windows-sys`: the dedicated server's
  interrupt handler, which sends `quit` on the console's channel (Ctrl-C,
  Ctrl-Break, the console's close, SIGTERM). Accepted by the owner on 2026-11-25 (P3.9).
- `dirs` 7.0.0 (MIT or Apache-2.0; no features), with `dirs-sys` (MIT or
  Apache-2.0), `option-ext` (MPL-2.0) and, on Redox only, `redox_users`
  and `libredox` (MIT): the Documents folder of the dedicated server's
  default user-data folder (the known folder on Windows, decision O41).
  Accepted by the owner on 2026-11-25 (P3.9). `deny.toml` needed no change for the three.

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
