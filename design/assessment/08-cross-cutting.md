# 08 — Cross-cutting assessment of `CppSource/`

Scope: what cuts across all subsystems. Method: grep/regex census over 1,414 `.cpp/.h/.c` files
(822,457 lines; +30,659 lines of `.rc`), all 39 `.vcxproj`, and `git log`. Counts are regex hits,
so treat them as ±10 % indicators, not exact. Nothing was built. "Guess" marks unverified claims.

**Corrections to the brief.** The solution has **39** projects, not 35 (27 code DLLs, 3 EXEs,
1 static lib, 8 resource-only DLLs). `IVP/`, `RemoteCommon/` and `include/` are source groups
with no project of their own. The tree is a rebranded reconstruction of shipped binaries: the
rebrand commit `89eda18` renamed the modules and scrubbed many addresses to the phrases
"the routine" (22,476 hits) and "module storage" (7,674 hits). Save magics (`FLS1`, `FLSG`),
`Fl*` MSBuild properties and `flconfigdatabase.txt` keep the old prefix.

## 1. Build system

| Item | Finding |
|---|---|
| Toolset | `v145`, Win32 only, C++17, `CharacterSet=MultiByte`, `/W3 /sdl`, `/MD` or `/MDd` (`Directory.Build.props`) |
| FP | `/arch:IA32` + `/fp:precise` tree-wide. Exceptions: `particlefx_math.cpp` is `/fp:fast`; three TUs are SSE2 (`particlefx_float_forward.cpp` ×2, `SceneScriptControlCalls.cpp`) |
| Configs | `Release→..\bin`, `Debug→..\bin_debug`, `DebugASan→..\binasan` (all at repo root, git-ignored) |
| Layout pin | Release sets `_ITERATOR_DEBUG_LEVEL=1` so `std::` containers keep the VC6 size and the "baseline field offsets asserted across src/include hold" (`Directory.Build.targets`) |
| ASan | `/fsanitize=address`, needs VS 2022 17.7+; resource DLLs excluded; copies `clang_rt.asan*_dynamic-i386.dll` |
| Post-build | Debug/DebugASan client runs `python copy_dev_ini.py <outdir>`: copies `..\bin\*.ini` (rewriting absolute paths), `mpnewcharacter.fl`, `newplayer.fl`, `flconfigdatabase.txt`. **Returns 1 if `..\bin` is missing**, which fails the build |
| DX9 post-build | `expand.exe` pulls `d3dx9_43.dll` out of the June 2010 SDK redist cab |
| MFC | `OutpostServer` uses `UseOfMfc=Dynamic` (`afxwin/afxext/afxole/afxrich/afxcmn/afxdlgs`) |
| Other scripts | `Resources/import_windows_resources.py` (regenerates `.rc` from git-ignored `ResourceInputs/`), `Common/parse_exports.py` (hard-coded `D:\...\EXE\Common.dll` + `.map`), `ReadFile/utf_walk_once.py` (one-off UTF verifier) |

Feature flags (all "port-only", i.e. absent from the original):

| MSBuild property → define | Default | Gates | `#if` sites / files |
|---|---|---|---|
| `OutpostFrontierWidescreenSupport` → `OUTPOST_FRONTIER_WIDESCREEN_SUPPORT` | 1 | Hor+ cameras, HUD anchoring, wide modes | 68 / 31 |
| `OutpostFrontierDx9Renderer` → `OUTPOST_FRONTIER_DX9RENDERER` | 0 | builds `DX9RENDERER.dll` (else an empty Utility project) + client "Ex" hooks (volumetric nebula, HLSL in `DATA_EXTRA`) | 40 / 10 |
| `OutpostFrontierWalkMode` → `OUTPOST_FRONTIER_WALK_MODE` | **inconsistent**: 0 in `Directory.Build.props` and the `.xml` schema, **1** in `OutpostFrontier.Diagnostics.props` (which the client imports, so effectively on) | walking in base rooms | 38 / 13 |
| `OutpostFrontierBetaLabel` → `OUTPOST_FRONTIER_BETA_LABEL` | 0 | on-screen label | 8 / 2 |
| `OutpostFrontierEnableTickProfile` → `OUTPOST_FRONTIER_ENABLE_TICK_PROFILE=n` | 0 | QPC physics/tick profiler in Common + client | 14 / 5 |
| `FL_ASAN`, `FL_CLIENT`, `OUTPOST_SERVER`, `RP_DIRECT3D9` | per project | defined, but 0–1 `#if` uses | — |

No `_DEBUG`/`NDEBUG` conditionals exist outside vendor code, so Debug and Release behave alike.

**Can it be built from this repo alone? No.** Missing:

1. DirectX 8.1 SDK at repo-root `dx8\` (`d3d8.h`, `d3dx8tex.h`, `dplay8.h`, `dsound.h`, static
   `d3dx8.lib`). Every project gets `$(Dx8SdkDir)\include` appended.
2. DirectX SDK June 2010 (`DXSDK_DIR`) if the DX9 flag is on.
3. The MFC x86 component for `OutpostServer`; Python on `PATH` for Debug.
4. `..\bin\` dev INIs for Debug/ASan (see post-build above).
5. To *run*: the original `..\DATA` tree, `..\EXE` INIs (`NEURONCOMPONENTS.INI`,
   `outpostfrontier.ini`, …), `newplayer.fl`, `flconfigdatabase.txt`, and original closed binaries
   that were never reconstructed: `EBUEula.dll` (fatal if absent, `main.cpp:410`), `gundll.dll`,
   `dw15.exe`, `Flasher.exe`; optional `DATA_EXTRA\` (port shaders).
6. Docs the comments cite but the repo lacks: `deviations.md` (15 refs), the C++-era `AGENTS.md`
   (15), `TABLE.md`, `docs/walk_mode.md`, `scenescript_thn_format.md`, "implementation/tracker
   notes" (809 refs).

Build-order fragility: `Server`, `RemoteClient`, `RemoteServer`, `rpclocal`, `Content` link
`Common.lib`/`Server.lib`/`rpclocal.lib` by name from `$(OutDir)` without a matching
`ProjectReference`. Some projects hard-code `..\..\bin` as a library path in every configuration.

**C++ CI: none, ever, for this tree.** `git log -- .github`: the workflow present when the tree
landed (`ddb435c`) still built the deleted `Source\…` solution; `89eda18` replaced it with the
Rust-only workflow.

## 2. Dependency graph

Link = `ProjectReference` ∪ `AdditionalDependencies`. Runtime = `LoadLibrary`/INI/component lookups
(component consumption is grep-derived from name strings and interface headers).

| L | Project | Type | Link deps | Runtime-loaded / component deps |
|---|---|---|---|---|
| 0 | neuroncomponents | DLL (.def) | — | every DLL in `[Libraries]` of `NEURONCOMPONENTS.INI` via `SearchPathA`+`LoadLibraryA` (`neuroncomponents.cpp:1313`); provides `RegExp` |
| 0 | zlib | DLL (.def, `ZLIB_WINAPI`) | — | — |
| 0 | qhull | static lib | — | — |
| 0 | imeui | DLL | imm32 | `imm32.dll`, IME modules by name |
| 0 | movie | DLL | ole32, vfw32 | DirectShow + WM ASF reader via COM |
| 0 | MemoryDiagnostics | EXE (Unicode) | psapi… | launches the game; no code deps |
| 0 | UiText, FieldGuideText, DialogueText, NameCatalog, EquipmentCatalog, NegotiationText, InterfaceExtras, serverinterface | resource-only DLLs (`/NOENTRY`) | — | loaded by name from `[Resources]` |
| 1 | NeuronRuntime | DLL | neuroncomponents, zlib, winmm | boots the component manager from the INI; DirectPlay8 (COM); `gundll.dll` |
| 1 | ReadFile | DLL | neuroncomponents | provides `FileSystem` (DOS/UTF/Mem), `SearchPath` |
| 1 | x86math | DLL | neuroncomponents | provides `3DMathEngine` |
| 1 | luaprofile | DLL | neuroncomponents | provides `ILua`, `ProfileParser`, `ProfileParser2` |
| 1 | system | DLL | neuroncomponents | provides `SystemContainer`; uses ProfileParser |
| 1 | SceneRuntime | DLL | neuroncomponents | provides `Engine`, `Animation`, `Hardpoint`, `ChannelArchetype`; uses FileSystem, 3DMath, ProfileParser, SystemContainer |
| 1 | SceneScript | DLL | neuroncomponents | provides `IScriptEngine`; uses FileSystem, 3DMath |
| 1 | DX8RENDERER / DX9RENDERER | DLL | neuroncomponents, ddraw, d3d8+d3dx8 / d3d9+d3dx9+DxErr | provide `DirectX8` / `DirectX9`; use FileSystem, ProfileParser, 3DMath; `d3dx9_43.dll` |
| 1 | Shading | DLL | neuroncomponents | provides Texture/Material libraries, batcher, animation; uses pipeline, FileSystem, 3DMath, Engine |
| 1 | RendComp | DLL | neuroncomponents | provides `RenderManager`, `VMeshLibrary`, meshes; uses pipeline, Shading, FileSystem |
| 1 | surfacematerials, particlefx, Deformable2, ximage | DLL | neuroncomponents | provide materials/`FLAppearance`, `ParticleFx`, `Deformable`/`EyeMaterial`, `BigImage`; use pipeline, Shading, FileSystem, 3DMath (Deformable2 also RendComp) |
| 1 | SoundManager, SoundStreamer | DLL | neuroncomponents, dsound, msacm32, winmm | provide same-named components |
| 2 | Common (+IVP sources, +qhull) | DLL | neuroncomponents, NeuronRuntime, qhull | uses FileSystem, 3DMathEngine (71 files), Engine, IScriptEngine |
| 3 | Server | DLL | Common, NeuronRuntime, neuroncomponents | `LoadLibrary` of `[Initial SP/MP DLLs]` (**Content.dll**) and of the client module path (**rpclocal** or **RemoteClient**) |
| 3 | rpclocal | DLL | Common, neuroncomponents | `LoadLibraryA("server.dll")`; re-loads the real client module |
| 3 | OutpostFrontier | EXE **with exports** | neuroncomponents, NeuronRuntime, Common, imeui | `Server.dll` or the `local_server` INI value, **RemoteServer.dll**, `movie.dll`, `ImeUi.dll`, `UiText.dll` + `[Resources]`, `EBUEula.dll` |
| 4 | Content | DLL (.def) | Server, Common | — |
| 4 | RemoteClient (+RemoteCommon) | DLL | Server, rpclocal, Common, NeuronRuntime | — |
| 4 | RemoteServer (+RemoteCommon) | DLL | **OutpostFrontier.lib**, Server, rpclocal, Common, NeuronRuntime | `LoadLibrary` of the client module |
| 5 | OutpostServer | EXE (MFC) | Server, RemoteClient, Common, NeuronRuntime, serverinterface | `ServerInterface.dll`, `UiText.dll`, `RICHED20`, `EBUEula.dll`, `RemoteClient.dll` |

Link order is acyclic. **Cycles appear once runtime loads are counted:**
`OutpostFrontier.exe ⇄ RemoteServer` (the DLL links the EXE's import library; the EXE loads the
DLL), `Server ⇄ Content`, `Server ⇄ rpclocal`, `Server ⇄ RemoteClient`. All layer-1 DLLs form one
late-bound cluster through the component manager: the real edges live in an INI file that is not in
the repo. The Rust layering (core ← client/server ← game) cannot mirror this literally;
`Server ⇄ Content` and the EXE-exports case need trait inversion.

## 3. 32→64-bit hazard census

| Hazard | Count | Top offenders |
|---|---|---|
| `static_assert` total / `sizeof` / `offsetof` | 4,219 / 1,453 / 2,750 | `include/common_cobject.h` 190, `rpclocal/rpc_packet_registry.cpp` 164, `include/common_archetype.h` 144 |
| "+0xNN" offset comments | 45,762 lines | `Server/server_classes.cpp` 1,241 |
| `reinterpret_cast` | 4,622 | `Client/cli_nebula.cpp` 512, `star_sphere.cpp` 191, `particlefx_appearances.cpp` 118 |
| pointer → 32-bit int (`reinterpret_cast<unsigned>` + C casts) | 87 + 182 | `rpclocal/rpclocal.cpp` 45, `SceneRuntime.cpp` 33, `Deformable2.cpp` 23 |
| int/offset → pointer, `*(T*)(p+0x..)` | 168 + 102 | `movie/MovieRuntime.cpp` 42+20, `Server/server_classes.cpp` |
| `#pragma pack` | 57 | `IVP/ivp_physics/ivp_mindist.h` 12, `Client/cli_asteroid.h` 8 |
| `memcpy` / with `sizeof` | 2,293 / 1,699 | `SceneScriptScene.cpp` 249, `particlefx_appearances.cpp` 208 |
| `fread`/`fwrite` (with `sizeof`) | 62 (7) | `Server/server_classes.cpp`, `OutpostServer/ServerConfigDlg.cpp` (raw struct fields to `OutpostServer.cfg`) |
| `Get/SetWindowLong` (no `…Ptr` variant anywhere) | 16 | `OutpostServer.cpp` 8, `Client/window.cpp` 4 |
| `unsigned long` / bare `long` used as u32/i32 | 8,671 / 2,039 | `SceneRuntime.cpp` 674, `MaterialImpl.cpp` 471 |
| containers keyed by pointer | 26 | `RendComp/Plugins.cpp` 9, `Content/Gcs.cpp` 6 |
| `_M_IX86` guards | 2 files | `include/x87_trig.h` has a hard `#error` |

`time_t`, `size_t` and `%p` uses are few and almost all in vendor code. `uintptr_t`-style casts
(883) are already width-safe. Reading:

- The tree is pinned to ILP32 by ~4.2k layout assertions. A few say so (`sizeof(void*) != 4 ||`, or
  `static_assert(sizeof(FCliDynamicAsteroid*) == 4)`). **The C++ cannot be recompiled for x64 as a
  stepping stone.**
- In-memory layouts are reconstruction artefacts and should not be carried into Rust. Carry only
  layouts that are *formats*: UTF tree entries, BINI, `.sur` chunks "read verbatim", VMeshData
  headers, RIFF, wire payloads in `rpclocal/rpc_payloads.h` and `RemoteCommon/*codecs.cpp`.
- The local RPC smuggles pointers through `unsigned int` argument arrays
  (`rpclocal.cpp:331-351`, `reinterpret_cast<unsigned int>(&info)`; 48 sites). It needs a typed
  redesign, not a port.
- `long` stays 32-bit on Win64, but Rust code must spell `u32`/`i32`, never `c_ulong`.
- I found no hash-of-pointer in persisted data. `IVP/ivp_utility/ivp_vhash.cpp` hashes in memory
  only (guess: not checked line by line).

## 4. x87 / numeric census

| Marker | Total | Where |
|---|---|---|
| `__asm` blocks | 228 | SceneScript 115, x86math 37, RemoteCommon 17, vendored Lua 16 (modified `lvm.c`, `lmathlib.c`), include 12 (`x87_trig.h`), Shading 11, RemoteServer 7, Client 7 |
| `__declspec(naked)` | 31 | x86math 27, SceneScript 4 |
| control-word manipulation (`_control87`, `fldcw`, `fnstcw`, …) | 88 | SceneScript 33, RemoteCommon 21, Client 16, particlefx 4, OutpostServer 4, IVP 3 |
| `x87_trig.h` includers / `X87*` calls | 26 / 192 | Common 22 / 160, IVP 4 / 20 |
| `ftol`-style conversions | 528 | Client 255, Common 58, Content 49 |
| bit-pattern constants (`0x5F3759DF`, `0x3F800000`, 2^52 rounding) | 249 | Shading 81, Common 39 |
| `volatile float/double` (forces a store-round) | 45 | particlefx 23, SceneScript 22 |
| `#pragma optimize/float_control/…` | 78 | particlefx 67 |
| the word "baseline" | 11,028 | include 2,161, Common 2,117, Client 2,021, Server 1,235, IVP 885 |
| x87/precision/bit-exact vocabulary | 1,517 | Common 472, IVP 277 |

**"Baseline" means the original shipped VC6-built binaries** (`EXE\*.dll`, `OutpostFrontier.exe`),
read as disassembly listings. The commitment to match them is extreme and instruction-level:

1. `Directory.Build.props`: "x87 floating point (/arch:IA32), as the VC6-built baseline modules.
   baseline leaves FPU_PRESERVE off … so Direct3D 8 CreateDevice drops the main thread's x87
   precision control to 24-bit and never restores it. x87 code inherits that; SSE2 code ignores the
   control word and would keep 53-bit doubles where baseline has 24-bit."
2. `include/x87_trig.h`: "every sin, cos, tan, atan and atan2 is inlined as one FSIN / FCOS / FPTAN /
   FPATAN instruction … These helpers execute the instruction itself." and "Never write
   (float)X87Sin(A) for such a site: that rounds 80 -> 53 -> 24 bits where baseline rounds 80 -> 24
   once."
3. `include/common_weighted_vector.h:102-111`: "each FADD is rounded by the thread's x87 precision
   control. That differs by process: the single-player client runs Server/Content on its main
   thread … 24-bit precision; OutpostServer.exe sets 53-bit itself (_control87(0x9001f, 0xfffff))
   … A float variable would force 24-bit everywhere."
4. `IVP/ivp_physics/ivp_contact_point.cpp:644`: "x87 PRECISION (… all 557 insns re-traced): values
   baseline keeps on the FPU stack are doubles here and are rounded only at its FST/FSTP dword;
   every compare uses the unrounded operand baseline compares".
5. `Client/nn_navmap_draw.cpp:8`: "The port models the stack with double and rounds with
   static_cast<float> exactly where baseline has an FSTP dword".
6. `Client/menu_sound_arch.cpp:1451`: "A float inverse-sqrt intermediate or libm log10 changes
   baseline's final rounding." (followed by an `__asm` block).
7. `include/msvcrt_compat.h`: "baseline MSVCRT.DLL wraps mod 2^32, the UCRT saturates" (`atoi` on
   decimal hash ids such as `2806013121`).
8. `Client/vc6_sort.h`: "The current CRT's std::sort orders equal elements differently, so a port
   whose baseline call sorts with a comparator that has ties uses this to keep baseline's order."
9. `IVP/ivp_collision/ivp_listmanager_topology.cpp:236`: "BASELINE UB, made deterministic here
   (owner's ruling 2026-10-08)". There are ~52 "baseline UB/bug" notes and ~84 deviation notes.

**What an x86_64 SSE2 Rust port can reproduce:**

- Single `+ − × ÷ sqrt` with `f32` operands under 24-bit precision control: identical to `f32`
  arithmetic, except in overflow/denormal ranges (x87 keeps the extended exponent until the store).
- The "double on the stack, `FSTP dword` at the end" pattern under 53-bit control: `f64`
  intermediates and one cast, with the same exponent caveat.
- Float→int: both truncation (`_ftol`, `fldcw` chop) and round-to-even (`FISTP`). Needs helpers,
  because Rust `as i32` saturates where x86 yields `0x80000000`.
- CRT `rand()` (LCG; 519 call sites, Content 246), wrapping `atoi`, legacy `%g` formatting
  (`include/msvcrt_numeric_format.h`), VC6 sort tie order: all are pure algorithms.

**What it cannot reproduce "for free":**

- **Process-dependent precision.** The same Common/IVP/Server code runs at 24-bit in the client and
  53-bit in the dedicated server, and the C++ gets both from one source via the control word. Rust
  needs an explicit choice: a numeric-mode type parameter or one canonical mode.
- 24-bit ops on non-`f32` operands (double constants): `f64` op + cast double-rounds and can be 1 ulp
  off, rarely.
- `FSIN/FCOS/FPTAN/FPATAN` microcode results: no libm matches them. Options: `core::arch::asm!`
  x87 shims (x87 still exists on x86_64; costs `unsafe` and x86-only code) or accept ulp-level
  drift. The original is itself only reproducible per CPU family for these instructions (guess,
  from general x87 knowledge).
- CRT `asin/acos/pow/log10`: the header argues the x86 UCRT matches the baseline; nothing shows the
  x64 UCRT does (unverified).
- `particlefx` (`/fp:fast`, SSE2 TUs) is not bit-stable even in the C++.

Net: bit-exactness is practical for the deterministic simulation core (Common/IVP/Server/Content)
only with a dedicated `x87` emulation module and a declared precision mode. It is poor value for
rendering, UI and effects.

## 5. Reconstruction provenance

| Marker | Total | Client | include | Common | Content | IVP | Server | rpclocal |
|---|---|---|---|---|---|---|---|---|
| comment lines with a binary address (`06xxxxxx`, `0x005xxxxx`) | 18,364 | 1,021 | 1,934 | 2,412 | 3,226 | 4,719 | 790 | 2,447 |
| decompiler names (`param_N`, `local_xx`, `uVarN`, …) | 1,986 | 158 | 174 | 397 | 55 | 811 | 246 | 83 |
| `Unknown…`/`Pad…`/`Field…` members | 10,904 | 2,233 | 1,832 | 1,501 | 1,101 | 215 | 408 | 2,043 |
| "SlotN"/"formerly" (unnamed virtuals) | 4,083 | 779 | 1,420 | 336 | 146 | 123 | 68 | 0 |
| "port-only" | 203 | 133 | 9 | 6 | 1 | 13 | 2 | 0 |
| guess/unverified/TODO/stub | 1,051 | 141 | 313 | 118 | 157 | 122 | 17 | 36 |
| mangled names / export ordinals | 322 | 8 | 226 | 40 | 0 | 1 | 30 | 15 |

Also: `.text:xxxxxxxx` appears literally only 3 times (the rest were scrubbed); `Fl*`/`fl_`/`FL_`
identifiers are rare (15/7/16); the per-file author tag appears in 1,280 of 1,414 files;
RendComp has 578 "SlotN/formerly" hits; RemoteCommon/RemoteClient/RemoteServer have 300–400
`Unknown/Pad` fields each.

Implications:

- **Function boundaries are reliable** where an address and a calling-convention note exist: most of
  Common, IVP, Server, Content, rpclocal. Export names for Common/Server come from real mangled
  symbols (`Server/*_symbols.txt`, 542 lines), so those class and method names are authentic.
- **Semantics are weaker than boundaries.** ~11k unknown/padding fields, ~4k unnamed virtual slots
  and ~2k surviving decompiler variables mean many structures are understood by offset, not
  meaning. Worst: the network payloads (rpclocal, Remote*), RendComp vtables, IVP internals.
- `IVP/` is **not** vendor Ipion source. It is a disassembly reconstruction that borrows Ipion SDK
  names; it carries no licence text.
- The scrubbing ("the routine", "module storage") destroyed many cross-references. The surviving
  ~18k addresses are only useful to someone who holds the original binaries.
- "port-only" marks code with no original counterpart (widescreen, DX9, walk mode, profiler,
  determinism fixes). It can be ported freely or dropped.

## 6. Platform API census

Hits per project (blank = 0):

| API family | Total | Main users |
|---|---|---|
| Direct3D enums (`D3DRS_*` …) | 802 | Client 310, DX9 160, Shading 110, surfacematerials 69, DX8 62, Deformable2 43, particlefx 39 |
| D3D8 / D3D9 interfaces | 155 / 179 | DX8RENDERER 63, DX9RENDERER 77+173 |
| DirectSound / ACM | 84 / 25 | SoundManager 67+10, SoundStreamer 5+15 |
| DirectPlay 8 | 97 | NeuronRuntime 84 |
| DirectShow / WM reader | 23 | movie |
| COM plumbing | 228 | movie 43, NeuronRuntime 39, Common 32, SceneScript 24 |
| IMM / TSF | 58 | imeui 57 |
| winsock | 47 | Client 27 (`login_mgr.cpp`), Common 9 |
| winmm timers | 49 | NeuronRuntime 16, Common 13, Client 11 |
| registry | 64 | Client 24, Common 18, MemoryDiagnostics 11 |
| GDI | 157 | Client 44 (font atlas), OutpostServer 35, Common 31 |
| window/message loop | 169 | OutpostServer 92 (MFC), Client 33 |
| SEH / TLS / memory-mapped files | 45 / 5 / 16 | movie 28+4, Client 10 / ReadFile 7 |
| thread creation | 7 sites | SoundManager 1, SoundStreamer 1, NeuronRuntime 1 (GUN), OutpostServer 2, MemoryDiagnostics 1 |
| sync primitives | 282 | OutpostServer 51, NeuronRuntime 42, movie 41, Client 37, SoundManager 33 |
| MBCS helpers / MB↔WC / wide types | 6 / 62 / 2,261 | imeui / Server 18 / Client 993 |

Key facts: the simulation is single-threaded (no thread is created by the client EXE, Server,
Common, Content or rpclocal); the only game threads are audio workers, DirectPlay's own callbacks
and the dedicated server's host thread. Narrow strings dominate engine code (project is MBCS);
UI text is UTF-16 from resources. Networking is DirectPlay 8, a deprecated optional Windows
component with no Rust binding (guess on availability).

## 7. Language-feature census

| Feature | Count | Notes |
|---|---|---|
| class/struct definitions | ~3,565 | include 979, Client 709, Content 600 |
| `virtual` keyword | 4,414 | include 2,759: COM-like interfaces everywhere |
| multiple inheritance | ~103 | Client 43, include 26 |
| virtual inheritance | ~69 | `server_fuse.h` 10, `client_fuse.h` 5, IVP listeners 11, MFC 21 |
| `dynamic_cast` / `typeid` | 36 / 0 | `Server/server_pub.cpp` 23 |
| `try` / `catch` / `throw` | 7 / 7 / 34 | effectively exception-free |
| `setjmp/longjmp` | 20 | only qhull and Lua |
| `template<` | 155 | light; `rpc_packet_codec.h`, `particlefx_ramp.cpp` |
| `#define` (multi-line) | 1,302 (84) | 94 % vendor: almost no code-generating macros in first-party code |
| `goto` | 161 | movie 64, vendor 63, IVP 17 |
| `operator new/delete` mentions | 2,219 | 962 raw `operator new(0x…)` calls reproducing original allocation sizes; 6 class-level overrides (`CObject`, `CEquip`, …); no global replacement |
| placement new | 582 | manual construction in raw storage |
| calling-convention keywords | 3,333 | `__thiscall` 450, `__fastcall` 25: vtables emulated by hand |
| export macros | 7,464 | whole classes exported |
| `std::vector/list/map/set` | 4,890 | Content 2,152 |
| shared headers with `std::` members | 46 of 130 in `include/` | **STL containers cross DLL boundaries** (`common_goods.h`, `common_archetype.h`, `common_cobject.h`); hence the iterator-debug pin |
| mutable globals (`G_*`/`g_*`) | ~3,454 | Client 2,367 |
| `static_cast` | 22,274 | Client 12,704: mostly float/double rounding control |

For Rust: the hard parts are not exotic C++ (few exceptions, little MI, no RTTI reliance). They are
(a) ~3.4k mutable globals and pervasive aliasing through raw pointers, (b) interface/factory
late binding by string name, (c) hand-rolled object lifetimes (placement new, refcounts,
intrusive lists), and (d) arithmetic whose exact shape is semantically significant.

## 8. Runtime data and asset formats

Most asset paths come from INI data, so extension literals under-count real use.

| Format | Kind | Parser | Notes |
|---|---|---|---|
| `.ini` (107 literals: Content 55, Client 43) | text; or binary **BINI** (magic `0x494E4942`) | `Common/common_ini.cpp` | universal; decimal 32-bit hash ids need wrapping `atoi` |
| UTF container (magic `"UTF "` `0x20465455`, version `0x101`) | binary tree, memory-mapped | `ReadFile/ReadFile.cpp` (+ a private reader in `Deformable2.h`) | carrier for the next six rows |
| `.cmp`, `.3db` (92 + 221 literals, client UI) | UTF | SceneRuntime (compound hierarchy, hardpoints), RendComp (VMeshData, `.vms`, `.lod`) | |
| `.mat`, `.txm` | UTF | Shading (`TextureLibrary.cpp`: DXT1–5 plus custom FourCCs `DAOP/DAOT/DAA1/4/8`), surfacematerials | |
| `.anm`, `.anim` | UTF | SceneRuntime, Common animdb | |
| `.ale` | UTF | particlefx | |
| `.dfm` (no literal) | UTF | Deformable2 | inferred |
| `.sph`, `.utf` (voices) | UTF | Common; SoundManager/Streamer | inferred |
| `.sur` | binary, chunked; `surf` chunk "read verbatim" into IVP compact surfaces | `Common/common_phyarch.cpp` | layout = format |
| `.thn` | Lua 3.2 text or precompiled chunk | SceneScript (`lundump.c`/`lparser.c`) | spec doc missing |
| `.fl` saves, `newplayer.fl`, `mpnewcharacter.fl` | text INI, optionally enciphered; magics `FLS1`, `FLSG` | `Server/server_savegame.cpp`, `server_savefile.cpp`, `server_classes.cpp` | `%g` formatting matters |
| RIFF/WAVE (PCM or ACM-compressed) | binary | SoundManager (mmap), SoundStreamer | decode via Windows ACM |
| movies | ASF/WMV through DirectShow | movie | |
| `.tga .dds .bmp .jpg .png .dib .ppm` | binary images | D3DX inside DX8/DX9RENDERER | screenshots written as BMP |
| `flconfigdatabase.txt` | text | `DX8RENDERER/Mgsdb.cpp` | GPU quirk database |
| `OutpostServer.cfg`, ban/password/name files | **raw binary struct fields** | `OutpostServer/ServerConfigDlg.cpp`, `Server/server_classes.cpp` | |
| Windows resources | string tables + RCDATA type 23 (UTF-16 markup; 5,252 `.txt` files) | resource DLLs; `LoadString`/`FindResource` | numeric ids are the contract |
| wire packets | binary, zlib-compressed, over DirectPlay | RemoteCommon codecs, rpclocal payloads, NeuronRuntime | |

Per-user locations (`Common/common_misc.cpp:985-1018`): `Documents\My Games\<game>\`
(`Accts\SinglePlayer`, `Accts\MultiPlayer`, `PerfOptions.ini`, `UserKeyMap.ini`),
`%LOCALAPPDATA%\<game>\` (spew log), `Pictures\<game>Shots\`. Registry:
`Software\Microsoft\Microsoft Games\<game>\1.0`. Hard-coded debug paths: `c:\client.updates`,
`c:\OutpostServerTrace.txt`, `c:\OutpostServerErrors.log`. No INI, asset or save sample is in the
repo.

## 9. Third-party code and licences

| Component | Version / state | Licence carried? | Rust route |
|---|---|---|---|
| zlib + minizip | **1.3.1** vendored; a shim reports `"1.1.4"` to match the original DLL | yes (`zlib/vendor/LICENSE`) | `flate2`/`miniz_oxide`; inflate output is version-independent, deflate output is not (matters only if compressed bytes are compared) |
| qhull | 2002.1, unmodified C | yes (`COPYING.txt`: keep notices, ship the text, mark modifications) | no drop-in crate with identical facet order; port, or wrap C via `cc` |
| Lua | **3.2.2 in both copies**; SceneScript's is patched with x87 `__asm`, luaprofile's adds the parser; `ORIGIN.txt` forbids sharing state | yes (`COPYRIGHT`, TeCGraf 1994–1999) | no crate (`mlua` is 5.x); must be ported or embedded as C |
| IVP physics | reconstructed, ~50k lines, compiled into Common | **none** | no equivalent crate; behaviour-defining |
| Regex (`RegExp` component) | Spencer-style `regcomp/regexec` inside neuroncomponents | none seen | `regex` crate differs in syntax: check patterns |
| D3DX 8.1 (static) / D3DX9 | external SDKs | Microsoft SDK terms | `image`/`ddsfile` for decode; custom FourCCs by hand |
| DirectPlay, DirectShow, ACM, MFC | OS/SDK | — | redesign (transport, media, admin UI) |

Carry forward: zlib, qhull and Lua notices if their code or a derivative ships. Separately, the
whole first-party tree is derived from a commercial binary; its licensing status is a project-level
question outside this assessment.

## 10. Verification assets

| Asset | State | Oracle value |
|---|---|---|
| Automated tests in `CppSource/` | **none** (`Client/test_size.cpp` is a stray `main()` printing three sizes) | — |
| Layout assertions | 4,219 `static_assert`s | document original layouts; useful only for format structs |
| DebugASan configuration | present, unbuildable here | memory-safety check of the C++ reference |
| Tick profiler | QPC timings per physics section (`include/common_tick_profile.h`) | performance only, not state |
| `MemoryDiagnostics.exe` | Win32 launcher around external leak tools | none for behaviour |
| Spew log (`FDUMP`, 887 sites → `FLSpew.txt`) | present | coarse textual trace, comparable across builds |
| `tracef` (`Common/common_remotephysics.cpp`) | writes `c:\client.updates` / `c:\OutpostServerTrace.txt`; the code notes it has **no caller** | hook point for a state trace |
| `ReadFile/utf_walk_once.py` | Python UTF walker | independent second implementation for the UTF reader |
| Saves and INI round-trips | text formats | cheap golden files once samples exist |
| Original binaries | not in repo | the only true oracle, and legally and technically external |

Git history: the Recorder (per-tick checksums), `Tools/Vectors`, `Source/Tests` and `Fixed64.tsv`
in commits `eaea247…e906edb` (2026-11-14) belonged to an **unrelated earlier code base** (a C++23
rebuild of a different, Unity-based strategy game) and were deleted in `ddb435c`. The ~160 commits
before that are a third unrelated project. Nothing in history tests this tree.

So no oracle exists today. The plan must create one: instrument the C++ (state hashes per tick,
format dumps) on a machine that can build and run it, and commit the captured vectors.

## 11. Rust workspace readiness

Exists: 7 packages (5 libs with 3–12 lines each, 2 one-line bins), `Cargo.lock` with zero external
dependencies, edition 2024, resolver 3, default target `x86_64-pc-windows-msvc`, CI running
fmt/clippy `-D warnings`/test/release build on `windows-2025-vs2026`.

Gaps:

| Area | Gap |
|---|---|
| Toolchain | `channel = "stable"` is unpinned; no MSRV |
| Lints | no `[workspace.lints]`, `clippy.toml` or `rustfmt.toml`; no `unsafe_code` policy while §4 and §6 imply `unsafe` (x87 shims, Win32/D3D FFI) |
| Numerics | no numeric policy: `f32`/`f64` discipline, forbidden fast-math/FMA, a precision-mode type, float→int helpers |
| Errors | only `io::Result`; the C++ uses `GENRESULT` codes plus `FDUMP` logging and almost no exceptions; no convention or logging facade |
| FFI | no `windows`/`windows-sys` choice; no decision on graphics, audio, input, video, network back-ends |
| Dependencies | no `cargo-deny`/`cargo-audit`, no licence allow-list |
| Features | none; C++ has 5 port-only flags and two renderers |
| Tests | zero tests; no fixture strategy for non-committable assets (env-var data root, ignored tests, synthetic fixtures); CI cannot see game data |
| Architecture | no decision on what replaces the string-keyed component registry, DLL plug-ins (`Content`, renderers), resource DLLs, or the client/server split when both run in one process (the single-player case) |
| Docs | `AGENTS.md` asks for explicit byte order and widths: no helper crate/module yet |
| Platform | CI and target are Windows-only; nothing can be tested on the Linux agents doing the work unless core crates stay platform-neutral and are also checked on a host target |

## 12. Decisions the plan must make (ranked by cost of getting it wrong)

1. **Numeric fidelity tier per subsystem.** Bit-exact with the original, deterministic but
   self-consistent, or merely plausible. This fixes the type design of all math, physics and AI
   code, the x87-emulation module, and whether `asm!` is allowed. Retrofitting later means
   re-porting Common/IVP/Server/Content (~270k lines).
2. **Which precision mode is canonical** for shared simulation code (24-bit client-hosted vs
   53-bit dedicated server), or whether to support both generically.
3. **What the oracle is and who can run it.** Without a buildable C++ reference or original
   binaries, "bit-exact" is unverifiable; decide on capturing golden vectors from an instrumented
   C++ build, and where those live.
4. **Port structure vs port layout.** Declare that only on-disk/wire layouts are preserved and
   in-memory layouts, vtable slots and allocation sizes are dropped; otherwise 4.2k assertions and
   11k unknown fields are dragged into a 64-bit world where they are false.
5. **Replacement for the component/DLL architecture**, including the runtime cycles
   (`Server⇄Content`, `Server⇄rpclocal`, EXE⇄RemoteServer) that contradict the Rust layering.
6. **Network compatibility.** Wire-compatible with original clients/servers (requires DirectPlay
   framing) or a new transport carrying the same message semantics.
7. **Save/config/asset compatibility.** Must original `DATA`, `.fl` saves and resource ids load
   unchanged (AGENTS.md implies yes)? Then BINI/UTF/`.sur`/`.thn`/RIFF readers come first and need
   fixtures.
8. **Third-party strategy.** Port Lua 3.2.2, qhull and the reconstructed IVP to Rust, or keep C
   behind FFI. Lua and qhull use `longjmp`; IVP has no licence.
9. **Backend stack** for rendering (the D3D8 fixed-function pipeline has no direct modern
   equivalent), audio (DirectSound+ACM), video (DirectShow), input, IME; and which port-only
   features (widescreen, DX9 nebula, walk mode) are in scope.
10. **Unsafe/FFI/dependency policy and workspace lints**, set before the first real crate lands.
11. **Global-state policy.** ~3.4k mutable globals need an ownership model (context structs vs
    statics) before porting the client.
12. **Dedicated-server UI.** The MFC admin application needs a replacement approach.
13. **Text handling.** MBCS engine strings and UTF-16 resources: pick internal encodings and where
    conversion happens.
14. **Reference-tree hygiene.** Whether to restore the missing docs (`deviations.md`, notes) and
    unscrubbed addresses for porters, and how to resolve the walk-mode default inconsistency.
