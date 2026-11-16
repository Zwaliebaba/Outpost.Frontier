# 07 — Media, scene scripts, text input and resources

Scope: `CppSource/SceneScript` (+ `lua32`), `movie`, `SoundManager`, `SoundStreamer`, `imeui`,
`Resources/*`, the matching headers in `CppSource/include`, and how the client consumes them.
Paths are relative to `CppSource/`. "Guess" marks anything not verified in code.

## Summary

| Library | Real size | What it is | Legacy tech to remove | Rust home | Effort |
| --- | --- | --- | --- | --- | --- |
| SceneScript | ~18k own + ~6.6k Lua C | Cutscene/room-scene timeline engine; `.thn` scripts evaluated by Lua 3.2 | x87 inline asm, Lua 3.2 C VM, component vtables | `neuron-client::scene_script` | XL |
| movie | ~1.7k player; ~13k is a WM DRM certificate stub + CRT remnants | Plays up to three startup movies | DirectShow, WM ASF Reader, VfW `MCIWnd` | `neuron-client::media` | S |
| SoundManager | 4.4k | Voice mixer over DirectSound 3D + EAX 2.0 | DirectSound, EAX, ACM | `neuron-client::audio` | L |
| SoundStreamer | 2.1k | Streamed WAV/MP3 (music, voice) | DirectSound, ACM | `neuron-client::audio::stream` | M |
| imeui | 2.7k | Text-edit model + custom IME composition/candidate UI | IMM32 private APIs, per-IME DLL probing | `neuron-client::text_input` | M |
| Resources (8) | 12,948 strings, 5,252 cards | String tables, RDL info cards, server GUI dialogs | Resource-only DLLs, `FindResourceEx` | `game-app` catalogs, loader in `neuron-core` | M |
| DirectInput (client) | 1 file | Mouse only | DirectInput 8 | `neuron-client::input` | S |

No tests exist for any of these. Verification hooks are `static_assert` layout checks, `FDUMP` log lines with original file/line
numbers, and the SceneScript text save path.

## 1. SceneScript

### What it is

A timeline engine. One `IScriptEngine` (45 slots, `include/scenescript_iface.h:540-587`) holds
entities, events and a millisecond clock. `Slot26Tick(DtMs)` advances the clock and drives the
world through nine small control interfaces that the client implements per entity
(`include/scenescript_controls.h:36-47`: compound, sound, IK, geo-transform, camera, D3D light,
particle system, fog, reverb). The library never touches the renderer or audio device itself.

- Entity types: 1 COMPOUND, 2 DEFORMABLE, 3 CAMERA, 4 MONITOR, 5 LIGHT, 6 SOUND, 7 MARKER,
  8 HARDPOINT, 9 SCENE, 10 SUB_SCENE, 11 MOTION_PATH, 13 PSYS (`scenescript_iface.h:46-57`).
- Event types 1–19: USER_EVENT, SET_CAMERA, START_SOUND, light/camera/spatial/psys/audio/fog/reverb
  property animations, PATH animation, ATTACH_ENTITY, CONNECT_HARDPOINTS, START_MOTION, START_IK,
  START_SUB_SCENE, START_PSYS, FLOOR_HEIGHT, SUBTITLE (`scenescript_iface.h:60-80`).
- Helper components registered by name: `CatmullRomPCurve`, `LinearPCurve`, `SmoothPCurve`,
  `StepPCurve` and SceneScript aliases (`SceneScript/SceneScript.cpp:906-936`), plus spline motion
  paths (`SplinePath.cpp`, 2.4k lines) and parameter curves (`ParamCurve.cpp`, 1.4k lines).

### Lua: version, patches, API surface

| Question | Finding |
| --- | --- |
| Version | Lua 3.2.2 (1999), reporting `"Lua 3.2"` (`lua32/include/lua.h:15`, `lua32/ORIGIN.txt`) |
| Number type | `double`, unchanged (`lua32/src/lobject.h:31-36`) |
| Stock or patched | Patched. `ORIGIN.txt` says "copied unmodified", but `lapi.c`, `lvm.c`, `lobject.c`, `ltable.c`, `lstring.c`, `lbuiltin.c`, `lundump.c`, `llex.c`, `lmathlib.c` carry edits. All are x87-parity edits (truncating number→int, FSIN range, operand order) or ownership rewrites, not language changes |
| Parser | Shared from `luaprofile/lua32/src/lparser.c` via `SceneScript/lua32/lparser_stub.c` |
| Libraries opened | baselib, mathlib, **iolib** (`SceneScriptLua.cpp:2194-2198`). iolib exposes `execute`, `remove`, `rename`, `getenv` to scripts (`lua32/src/lib/liolib.c:529-551`) |
| C API used | 24 functions of the pre-4.0 API: `lua_dobuffer`, `lua_getglobal`/`setglobal`, `lua_gettable`, `lua_pushobject`/`pushnumber`/`pushcfunction`, `lua_callfunction`, `lua_lua2C`, `lua_isnil`/`istable`/`isstring`/`isnumber`, `lua_getstring`/`getnumber`, `lua_beginblock`/`endblock`, `lua_setstate`, `lua_seterrormethod` |
| Engine functions exposed to scripts | None. Only numeric constants are registered (`SceneScriptLua.cpp:398-489`): entity and event names, axis names, flag bits (`LOOP`, `SPATIAL`, `STREAM`, `REFERENCE`, `LOOK_AT`, …), fog and light modes, `Y`/`N` |

### Script format and data flow

- A script is a Lua chunk that defines three globals: `duration`, `entities`, `events`. The loader
  runs the chunk once with `lua_dobuffer` (`SceneScriptLua.cpp:2286`), then walks the two tables with
  the Lua `foreach` builtin and C callbacks (`:2295-2341`). After load, Lua is not used again;
  playback runs on C++ records.
- Property tables read per entity/event (about 100 keys): `spatialprops{pos,orient}`,
  `cameraprops{fovh,hvaspect,nearplane,farplane}`, `lightprops{…}`, `audioprops{attenuation,pan,dmin,dmax,ain,aout,atout,rmix}`,
  `fogprops`, `reverbprops{env,vol,decay,damping}`, `pathprops`, `psysprops{sparam}`, `userprops`,
  `param_curve{CLSID,points}`, `path_data`, `animation`, `strid`, `user_event_string`.
- `lua_dobuffer` accepts source text and precompiled chunks (first byte `ESC`, then `"Lua"`, version
  `0x32`; `lua32/src/lundump.h:20-23`, `lundump.c:230-234`). **No `.thn` files are in the repository,
  so which form ships is unverified.** Guess: the original assets are precompiled.
- A precompiled chunk pins the Lua 3.2 opcode set but not a 32-bit layout: sizes are explicit
  big-endian words, numbers are either text or a native 8-byte `double` checked against a test
  constant (`lundump.c:40-82`, `:186-216`). It loads unchanged on x64.
- The engine can also write a scene back as Lua text (`SceneScriptScene.cpp:8687`, default save
  parser `SceneScript.cpp:143-165`). No client caller of `Slot44SavePath` was found.
- Consumers: `Common/common_roomdata.cpp:177-222` (`SceneProgramLoad`, creates the engine and loads
  the file) and the client script layer (`gf_script*.cpp`, `gf_scenescript_*.cpp`,
  `gf_character_object.cpp`, `space_camera_script.cpp:542-646`). `SceneProgram*` callers are all in
  the client; the server does not run scenes.
- Lua elsewhere: `luaprofile` is a second, private Lua 3.2.2 VM (baselib only) behind
  `ILua`/`IProfileParser` for Lua-syntax profile files (`luaprofile/luaprofile.cpp:291-405`). It
  must not share state with SceneScript (`luaprofile/lua32/ORIGIN.txt`). The two share one parser source file.

### Options for Lua

| Option | For | Against |
| --- | --- | --- |
| A. `mlua` (Lua 5.4, vendored C) | Mature; little code | Cannot load 3.2 bytecode. 3.2 differs in syntax and library (`%upvalue`, `$if` pragmas, tag methods, global `foreach`, no `for`). Adds a C build dependency |
| B. Port a Lua 3.2 subset to Rust | Loads existing text and bytecode files unmodified; one implementation also serves `luaprofile`; iolib can be dropped | About 3–4k lines of Rust (undump, VM, tables, strings, lexer/parser, baselib subset). x87 parity edits do not carry to x64 |
| C. Convert scripts offline to a data format | Smallest runtime; no interpreter in the client | Needs a 3.2 evaluator for the conversion anyway (the C++ build with its text writer can serve). Breaks loading of unmodified or third-party `.thn` files |

Recommendation: B, limited to what a load-time evaluator needs (no iolib, no `dofile`). It is the
only option that keeps the existing file format, which the port map asks for. Choose C instead if
the owner confirms assets will be re-exported. Reject A.

### Port notes

- Dependencies: `neuroncomponents` (factory, `FDUMP`), `IFileSystem` (ReadFile), `I3DMathEngine` (x86math).
- Threading: none; single-threaded, called from the client frame.
- Globals: one process-wide `lua_State`, load-time cursor globals (`SceneScriptLua.cpp:32-53`), a
  wrapping event-id counter (`SceneScriptScene.cpp:26`).
- Pointer-size hazards: descriptor structs asserted only when `sizeof(void*) == 4`
  (`scenescript_iface.h:163, 216, 230`), an event descriptor whose `Key` field is the event's own
  address (`:339`), `IntPoint` pointer hashing in Lua (`lobject.h:44`).
- Numeric hazard: 141 lines reference x87 behaviour, with `__asm` in `SplinePath.cpp`, `SplineMath.h`,
  `ParamCurve.cpp`, `SceneScriptLua.cpp`, `SceneScriptScene.cpp`. Curve and path evaluation will
  differ in low bits on SSE2; tolerances must be set at the control-interface boundary.
- Risk: the 9.3k-line `SceneScriptScene.cpp` holds the per-event tick semantics and is the bulk of the work.

## 2. movie

- Actual player: `MediaPlayer.cpp` (388), `MoviePlay.cpp` (803), `MovieObject.cpp` (521). The rest is
  a statically linked Windows Media certificate stub (big-number math, SHA-1, MD4, RC4:
  `MovieField*.cpp`, `MovieMod.cpp`, `MovieExp.cpp`, `MovieD4.cpp`, `MovieSha.cpp`, `MovieAce.cpp`,
  `MovieCca.cpp`, `MovieWindow.cpp`, `MovieGraph.cpp`) and CRT heap/lock code (`MovieRuntime.cpp`).
  It exists to answer `IWMAuthorizer` (`MoviePlay.cpp:71-110`, four embedded certificates). None of
  it needs porting.
- Graph construction, all in `BuildAndRunWmAsfGraph` (`MoviePlay.cpp:470-720`):
  `CoCreateInstance(CLSID_FilterGraph → IGraphBuilder)` `:504`; `IObjectWithSite::SetSite` with the
  authorizer service provider `:514-518`; `CLSID_WMAsfReader` as `IBaseFilter` `:530`, `AddFilter`
  `:536`, `IFileSourceFilter::Load` `:550`; `IGraphBuilder::Render` on every output pin `:558-590`;
  `IMediaControl`, `IVideoWindow`, `IBasicVideo` `:603-605`; `put_Owner(game HWND)` `:610`;
  `Pause` then a message-pumping wait `:613-655`; centre at ideal size `:679-699`;
  `IMediaEventEx::SetNotifyWindow(hwnd, 0x8001)` `:702-703`; `Run` `:706`.
- Container/codec: ASF only (the WM ASF Reader is hard-wired), so WMV video and WMA audio. Codec
  versions are not visible in code.
- Frames never reach the renderer. The DirectShow video renderer paints a child window over the
  game window, and the game window is subclassed for the duration (`:501`). Audio goes through the
  graph's default audio renderer, bypassing SoundManager. There is no volume control.
- A second path, "approach 2", uses VfW `MCIWndCreateA` (`MovieObject.cpp:263`). The client always
  selects approach 0 (`OutpostFrontier/Client/engine_init.cpp:305`).
- Only consumer: `PlayStartupMovies` (`engine_init.cpp:270-334`), up to three `[Startup] movie_file`
  entries (`startup_config.cpp:106-118`), bound by `GetProcAddress` on mangled names. Shift or
  Escape, end of stream, or user abort stop playback (`MoviePlay.cpp:754-786`).
- Hazards: `SetWindowLongA(hwnd, -4, (LONG)proc)` truncates a pointer (`:501`); process-global COM
  pointers (`MediaPlayer.cpp:17-30`).

| Replacement | Notes |
| --- | --- |
| Media Foundation via `windows` (`IMFMediaEngine` or `IMFSourceReader`) | Decodes ASF/WMV/WMA in-box; frames can go to a D3D11 texture, audio to the new mixer. Recommended if the files stay as they are |
| Rust decoder | No maintained pure-Rust WMV decoder. Viable only if movies are transcoded (for example to AV1/Opus or H.264/AAC); then MF still works, or a Rust demux/decoder stack |
| Drop | Three intro clips; lowest-risk option if the owner agrees |

Formats to support if kept: ASF container, WMV (version unverified), WMA.

## 3. SoundManager and SoundStreamer

### DirectSound usage in SoundManager

| Area | Detail (file `SoundManager/SoundManager.cpp`) |
| --- | --- |
| Device | `CoCreateInstance(CLSID_DirectSound)` for base `IDirectSound`, not `IDirectSound8` `:3070`; `DSSCL_PRIORITY` `:3093`; primary buffer `PRIMARYBUFFER\|CTRL3D` `:3112`, kept playing looped `:3183`; `IDirectSound3DListener` from the primary `:3169` |
| Hardware probing | `DSCAPS` free hardware buffers set the channel budget (minimum 8) `:3095-3107`; hardware and hardware-3D probe buffers `:3144-3161`; per-voice `LOCHARDWARE`/`LOCSOFTWARE` play with fallback `:3790-3840`; voice migration between the two (`MaybeMigrateHwSw` `:349`) |
| Buffers | One static buffer per archetype `:1198-1222`; `DuplicateSoundBuffer` per playing instance `:2667`. Flags chosen from archetype bits: 3D or pan (exclusive), frequency control, mute-at-max-distance, position notify |
| 3D buffers | `SetMode` (normal, head-relative, disable), cone angles/orientation/outside volume, min/max distance, velocity, all deferred and committed once per tick `:2332-2394`, `:3781` |
| Listener | Orientation, position, velocity, distance/Doppler/rolloff factors, deferred `:1707-1717`. Doppler is left to DirectSound; factor default 1.0 |
| 3D algorithm | ini `3D_SW_Algorithm` = FULL, LIGHT, BASIC, USER, NONE mapping to HRTF full/light/no-virtualization GUIDs `:3350-3392` |
| EAX | EAX 2.0 listener and buffer property sets through `IKsPropertySet` (`SoundManagerInternal.h:46-52`): environment, room volume, decay, damping, per-source environment `:2269-2280`; raw get/set slots 27–30 |
| Software 3D path | For non-hardware-3D voices the manager computes attenuation and pan itself: `dB = 10·log10(Min / (Min + Rolloff·(Dist·DistFactor − Min))) + source dB`, only beyond min distance, in x87 `fyl2x` `:726-750`; distance from a fast inverse square root `:711-720`; pan = dot(direction, listener right) × 10000 `:2408-2431` |
| Volume / pitch | Volume = (archetype gain + master + attenuation) × 100, clamped to [−10000, 0] `:2493-2505`. Frequency = rate × 2^(pitch/100), pitch clamped ±100, result clamped 100–100000 Hz `:2466-2488`, `:757-763` |
| Voice management | The client ranks sources by score and submits an ordered list; the manager plays the first `MaxChannels` that are active, started and in range, and stops the rest (`MixerTick` `:3649-3775`). Client side: `menu_sound_arch.cpp:3426-3491` (budget 8–128 from `max_submitted_sounds`) |
| Looping | A notify thread waits on per-voice position events and rewinds loop ranges with `SetCurrentPosition` `:919-984`, `:3505-3540` |
| Formats | RIFF WAV with `fmt `, `data`, `fact` and a custom `trim` chunk `:640-690`. PCM is used directly; anything else is decoded whole through ACM (`acmStreamConvert`) `:520-601`. The ACM driver is found by display name, "Fraunhofer IIS MPEG Layer-3 Codec (advanced)" or "(decode only)" `:441-447`, `:468`. No ADPCM-specific code |
| ini keys | `[AudioProperties]` useEAX, reverbBaseEnvironment, reverbMasterVolume, reverbDecayTime, reverbDamping, reverbMix; `[SoundManager]` deviceID, 3D_SW_Algorithm, useNoHW, rightHandedCoordinates, createAll2DInSoftware, speakerConfiguration, maxSoundChannels, use2DHW `:3213-3460` |

Interface: `ISoundManager` (56 slots), `ISoundArchetype`, and `ISoundSource`, a 43-slot "play key"
the client implements and the mixer polls each tick (`include/soundmanager.h`). `ISoundSource` mixes
two calling conventions. `ISoundListenerPose` is declared but both client call sites pass null
(`menu_sound_arch.cpp:3325`, `:3491`); the client sets the listener directly (`:3408-3417`).

### SoundStreamer

- `IStreamer2` (12 slots, `include/soundstreamer.h`): create/destroy/start/stop stream, volume, pan, status.
  It borrows the manager's `IDirectSound` (`NeuronRuntime/NeuronRuntime.cpp:195-211`).
- One looping secondary buffer per stream, flags `CTRLPAN|CTRLVOLUME|GETCURRENTPOSITION2`
  (`SoundStreamer.cpp:1601-1606`), refilled by a worker thread at highest priority that polls every
  40 ms and tops up the emptiest stream (`:1161-1172`, `:1440-1468`).
- Input is RIFF WAV holding PCM or MP3 (format tag `0x55`), decoded incrementally through the same
  Fraunhofer ACM driver (`:75-114`, `:530`, `:1314-1344`). The `trim` chunk supplies encoder delay.
  Streams can loop (`:1671`).
- Consumers: music and voice lines via `StreamedSource` (`menu_sound_arch.cpp:1909-2011`); effect
  archetypes come from WAVs inside `.utf` containers (`:3521-3546`).

### Port notes and replacement

- Threading: main-thread mixer tick, one notify thread (manager), one worker thread (streamer),
  one critical section each. Globals: instance list, notify thread handle, ACM driver handles.
- Hazards: object layouts asserted at 32-bit offsets (`SoundManagerInternal.h:358`), inline x87 asm,
  bit-cast float tricks (`SoundManager.cpp:711-720`).
- **The channel budget is taken from free hardware buffers; when there are none, `MaxChannels` stays 0
  unless the ini sets `maxSoundChannels`** (`:3100-3107`, `:3456-3457`). Current Windows emulates
  DirectSound without hardware buffers (platform knowledge, not from code), so the hardware and EAX
  branches are effectively dead there.

| Candidate | For | Against |
| --- | --- | --- |
| XAudio2 + X3DAudio + XAudio2 reverb (`windows` crate) | In-box; source voices map one-to-one to instance buffers; native loop regions remove the notify thread; frequency ratio and output matrix cover pitch and pan; reverb can take EAX-style presets | Windows-only API surface; Doppler/HRTF behaviour differs from DirectSound |
| WASAPI direct (`windows`) or `cpal` + own mixer | Mixer is pure Rust and unit-testable; exact control of curves | Must write resampling, reverb and device handling |
| `kira` / `rodio` | Fast to adopt | Voice budget, ranking and curves are still ours; less control over reverb and timing |
| Decode: `symphonia` | Pure-Rust WAV and MP3; replaces ACM and the codec-name lookup | Verify gapless handling against the `trim` chunk |

Recommendation: port the legacy "software 3D" arithmetic (attenuation, pan, pitch, volume clamps,
range cull, ranked voice cap) as pure Rust with tests, and drive XAudio2 source voices with the
results. Parity-relevant behaviours: the `10·log10` rolloff law and min/max distances, pan scale,
±1 octave pitch clamp, hundredths-of-dB volume clamp, ranked cut-off at the channel budget, start
tick and one-shot expiry rules, loop ranges, 40 ms stream refill. Hardware/software placement, EAX
property passthrough and `3D_SW_Algorithm` can be dropped or mapped to a reverb send.

## 4. `AIL_` symbols

Not Miles Sound System. A tree-wide search finds `AIL_` only inside `DETAIL_LEVELS`
(`OutpostFrontier/Client/game_options.cpp:103`, `game_options.h:110`) and `GLOBAL_FAIL_*` mission
comm names (`Content/RandomMissionRuntime.cpp:103-105`, `RandomMissionMachine.h:345-347`,
`RandomMissionVignette.cpp`). No `mss32`, Miles, Bink or Smacker references exist. Nothing to port.

## 5. imeui

- Two things in one DLL: (a) the game's text-edit model — `CCaret`, `CInputBox` (UTF-16 buffer,
  caret, insert mode, char filter), `IMEInputBox`, and `IMEFocus`, a singleton that routes window
  messages to the focused box (`include/imeui.h`); (b) an IMM32 composition/candidate/reading-window
  renderer derived from the DirectX SDK ImeUi sample (`imeui/ImeUiCore.cpp`, 1.3k lines) that draws
  through two client callbacks (`DrawRect`, `DrawTextW`).
- Legacy techniques: `imm32.dll` bound at run time including undocumented `ImmLockIMC`/`ImmLockIMCC`
  (`ImeUiCore.cpp:122-133`); `ImmDisableTextFrameService(-1)` `:133`; probing specific IME files by
  name and version (`TINTLGNT.IME`, `CINTLGNT.IME`, `MSTCIPHA.IME`, `PINTLGNT.IME`, `MSSCIPYA.IME`)
  and calling their private `GetReadingString` export `:1047-1100`; hiding the language bar with
  `ITfLangBarMgr` (`IMEFocus.cpp:138-156`); `GetVersionEx`; GDI text metrics. ANSI and wide IMM paths both exist.
- Consumers: `client_ime_focus.cpp` (drawing callbacks), `fl_ime_input_box.cpp` (the client box,
  built from the font's `LOGFONTA`, see `include/font.h` `GetLogFont`), `rich_text_win.cpp:1146-1737`
  (edit controls), the message pumps (`message_pump.cpp:28`, `engine_init.cpp:87`),
  `window.cpp:214` and `main.cpp`. All in-game text entry goes through it, not just IME input.
- Hazards: `static_assert(sizeof(void*) == 4)` in `imeui.h` and `font.h`; client subclasses exported
  C++ classes across the DLL boundary; singleton and static scratch buffers.

Replacement: keep (a) as a small Rust edit-box model fed by `WM_CHAR`/`WM_KEYDOWN` (UTF-16 in,
`String` inside). For (b), either let the system IME draw its own composition and candidate windows
and only position them (`ImmSetCompositionWindow`/`ImmSetCandidateWindow` via `windows`), or use
Text Services Framework with UI-less mode to keep in-game rendering. The first is far smaller and
drops every private API; the second is needed only if exclusive fullscreen must show candidates
in-engine. `IFont` is GDI-based (`font.h`); its replacement belongs to the UI/renderer assessment.

## 6. Resources

| DLL | String-table entries (local ID range) | RDL cards, type 23 (ID range) | Other | Side |
| --- | --- | --- | --- | --- |
| UiText | 3,648 (0–60252) | — | 1 dialog | client; also loaded by the server GUI |
| FieldGuideText | — | 886 (3–1079) | — | client |
| DialogueText | — | 3,087 (56–3869) | — | client |
| NameCatalog | 7,139 (1–65186) | — | — | client display; IDs also in GameLogic per port map |
| EquipmentCatalog | 784 (1002–3611) | 784 (2002–4611) | — | client |
| NegotiationText | 1,270 (0–4344) | — | — | client display; IDs also in GameLogic per port map |
| InterfaceExtras | — | 495 (1–496) | — | client |
| serverinterface | 107 (100–61485) | — | 20 dialogs, 2 menus, 2 accelerators, 2 bitmaps, 2 icons, 1 toolbar blob | server GUI only |

- Encoding: `.rc` files are UTF-8 and compiled with `/c65001` (`Resources/ResourceDll.props`). All
  5,252 `.txt` files are UTF-16LE with BOM, one line each, 3.4 MB total, largest 6,870 bytes. Content
  is XML-like markup: `<RDL><PUSH/><TEXT>…</TEXT><PARA/><POP/></RDL>` with `JUST` and `TRA` tags.
  Each is referenced as `<id> 23 "HTML<id>_1.txt"`.
- ID scheme: a 32-bit id is `(module_index << 16) | local_id` (`OutpostFrontier/Client/ids_strings.cpp:29-36`,
  `:68-76`). `module_index` is load order: `UiText.dll` is always 0 (`main.cpp:741-742`), then each
  `DLL` line of the ini `[Resources]` section in file order; a DLL that fails to load still takes an
  index (`dataconfig_progress.cpp:196-221`). The ini is not in the repository. Guess, from the GUID
  suffixes in `import_windows_resources.py:53-62`: FieldGuideText 1, DialogueText 2, NameCatalog 3,
  EquipmentCatalog 4, NegotiationText 5, InterfaceExtras 6.
- Lookup: strings are read by hand from `RT_STRING` block `(id >> 4) + 1`, entry `id & 15`
  (`ids_strings.cpp:91-125`). Rich text tries type 23 first, strips a BOM, then falls back to the
  string table; the result is parsed as XML or treated as plain text (`ids_rdl.cpp:233-310`). String
  and card ID spaces are separate and overlap numerically in EquipmentCatalog.
- Server side: the server executable repeats the same scheme (`OutpostServer/ResourceStrings.cpp:13-31`,
  `OutpostServer.cpp:487`, `:811`). `Server` and `Content` contain no resource lookups; they carry
  numeric IDs only. `serverinterface` is the MFC admin GUI's dialogs.
- Build today: `Resources/import_windows_resources.py` converts exported resource scripts from an
  external `ResourceInputs/` folder into the checked-in `.rc`/`.txt` files and project files; MSBuild
  then links resource-only DLLs with `/NOENTRY`. The other scripts (`copy_dev_ini.py`,
  `Common/parse_exports.py`, `ReadFile/utf_walk_once.py`) do not touch resources.

Proposed Rust representation:

- A build step (a workspace tool or `build.rs` in `game-app`) parses each `.rc` `STRINGTABLE` and the
  `.txt` cards and emits one catalog per module: two sorted tables (`strings`, `cards`) of
  `u16 local_id → UTF-8 text`, written as a little-endian binary with explicit field widths.
- Runtime: `Catalogs` holds modules in ini order (UiText first, missing modules keep their slot);
  `string(Ids(u32))` and `rich_text(Ids)` reproduce the split and the card-then-string fallback.
  Catalogs load from files beside the executable so text stays replaceable; embedding with
  `include_bytes!` is an option for UiText.
- The loader type lives in `neuron-core` (both executables need it); catalog contents and the RDL
  parser live in `game-app`. `game-logic` keeps IDs as plain `u32` and never resolves text.
  `serverinterface` dialogs are replaced by whatever admin UI the server port chooses.
- Tests: golden counts above, round-trip of every id against the `.rc`/`.txt` sources, block/entry arithmetic.

## 7. DirectInput in the client

Mouse only. `OutpostFrontier/Client/input_manager.cpp`: `DirectInput8Create` `:308`,
`CreateDevice(GUID_SysMouse)` `:310`, `SetDataFormat(c_dfDIMouse2)` `:311`, cooperative level
non-exclusive/background `:370` or exclusive/foreground `:398`, `Acquire` `:416`, buffered
`GetDeviceData` `:502`, immediate `GetDeviceState` `:425`, `:559`, `Poll` `:1060`. Position is
integrated from deltas and scaled by surface width; the cursor is drawn in software
(`input_manager.h:11-32`). Keyboard input uses window messages and `GetKeyState`
(`menu_keymap.cpp:470-474`, `:626-664`). No joystick or gamepad code exists.

Replacement: Raw Input (`WM_INPUT`, `RegisterRawInputDevices`) for relative motion and buttons,
`ClipCursor`/capture for the exclusive mode, keyboard unchanged. GameInput, XInput or `gilrs` would
add controller support that does not exist today.

## 8. Suggested destinations and crates

| Library | Rust module | Depends on (C++ side) | Candidate crates |
| --- | --- | --- | --- |
| SceneScript | `neuron-client::scene_script::{thn, timeline, curve, path}`; scene selection in `game-app` | neuroncomponents, ReadFile, x86math | none required (own Lua 3.2 subset); `glam` if adopted workspace-wide |
| luaprofile (relation) | `neuron-core::profile` reusing the same evaluator | neuroncomponents | — |
| movie | `neuron-client::media::movie` | Win32, COM | `windows` (Media Foundation) |
| SoundManager | `neuron-client::audio::{device, mixer, spatial, archetype}` | neuroncomponents, x86math, profile parser | `windows` (XAudio2), `symphonia`, `hound` (optional) |
| SoundStreamer | `neuron-client::audio::stream` | SoundManager device, ReadFile | `symphonia` |
| imeui | `neuron-client::text_input` | Win32 | `windows` (IMM/TSF) |
| Resources | `neuron-core::catalog` + `game-app::text` | ini reader | none |
| DirectInput | `neuron-client::input::mouse` | Win32 | `windows` (Raw Input) |

## Open questions for the owner

1. Are the shipped `.thn` scripts Lua source or precompiled Lua 3.2 chunks, and must the Rust client
   load them (and third-party ones) unmodified? This decides between a Rust Lua 3.2 subset and
   offline conversion.
2. Should scripts keep access to `execute`, `remove`, `rename` and file I/O? Proposed: no.
3. Is bit-level parity required for curve/path evaluation and sound attenuation, or is a tolerance acceptable on x64?
4. Movies: keep the WMV/ASF files (Media Foundation), transcode, or drop the three startup clips?
5. Audio: is XAudio2 acceptable as the device API? Is reverb (former EAX) required, and is HRTF wanted?
6. What `maxSoundChannels` value does the reference configuration use, given that the hardware-derived default is 0 today?
7. IME: is system-drawn composition/candidate UI acceptable, or must it be rendered in-engine?
8. Please supply the `[Resources]` DLL order from the reference ini so module indices can be fixed in tests.
9. Should text catalogs stay as replaceable files beside the executable, or be embedded? Is more than one language planned?
10. What replaces the `serverinterface` MFC dialogs in `outpost-server`?
11. The rename that produced these resource names also altered identifiers (for example
    `FieldGuideTexttats` in `OutpostFrontier/Client/nn_inventory.h:257`). Should the port use corrected names?

## Suggested port order within this group

1. Resources: catalog format, build step and lookup. No dependencies, unblocks all UI text, easy to test exhaustively.
2. Mouse input via Raw Input. Small and self-contained.
3. Text input: edit-box model first, IME second.
4. Audio decode (WAV/MP3 with `trim`) and the pure mixer arithmetic with tests, then the XAudio2 device, then streaming.
5. Lua 3.2 subset evaluator (shared with `luaprofile`), then the THN loader, curves and paths, then the timeline and event handlers.
6. Movie playback last, or drop it.
