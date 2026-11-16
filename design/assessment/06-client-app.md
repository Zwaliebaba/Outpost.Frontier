# 06 — Client application (`CppSource/OutpostFrontier`)

Scope: `CppSource/OutpostFrontier/Client/*` (246 `.cpp`, 245 `.h`, 1 `.inc`; 233,040 lines, of which
~42k are comment lines and ~22k blank), `resources/` (icon 103 + VERSIONINFO only), and
`CppSource/include/OutpostFrontierClient.h`. Read-only assessment; paths are relative to
`CppSource/OutpostFrontier/Client/` unless prefixed. Line numbers are from the current tree.
Anything not read directly is marked **(guess)**.

The project links only `neuroncomponents.lib; NeuronRuntime.lib; Common.lib; imeui.lib` plus
`ole32, gdi32, user32, advapi32, shell32, winmm` (`OutpostFrontier.vcxproj`), and pulls
`dinput8.lib`/`dxguid.lib` (`input_manager.cpp:46-47`) and `ws2_32.lib` (`login_mgr.cpp:29`) by
pragma. Every other engine service (renderer, sound, scene runtime, particle FX, deformables) is
reached through COM-like interfaces published as `NeuronRuntime::*` globals. `Server.dll`,
`RemoteServer.dll`, `movie.dll`, `EBUEula.dll`, `sensapi.dll`, `shdocvw.dll` and the resource DLLs
are loaded at runtime by name.

## 1. Sub-system decomposition

Clusters were built by file-stem pattern and verified by reading the central files of each. LOC =
`.cpp` + `.h` lines. Total 492 files / 233,040 lines.

| # | Sub-system | File patterns | Files | LOC | Depends on (engine) | Rust destination | Effort |
|---|---|---|---:|---:|---|---|---|
| 1 | Startup, window, message pump, frame tick, timing, engine init, loading screen | `main`, `window`, `message_pump`, `client_frame_tick`, `client_timing`, `startup_config`, `engine_init`, `globals`, `loading_screen`, `dataconfig_progress`, `main_command_handler`, `misc_util`, `pixel_format` | 21 | 5,048 | NeuronRuntime (Startup, Pipeline), Common (INI, Timing, DataConfig, ErrorHandler), movie.dll | `apps/outpost-frontier` (entry, crash/EULA policy), neuron-client (window, pump, render-mode, clock), game-app (boot sequence) | M |
| 2 | Client state machine and flow | `client_state`, `client_environment`, `gameflow`, `space_state`, `space_app`, `mission_briefing`, `menu_enter_state`, `intro_*`, `fade_to_black`, `space_letterbox`, `client_land_action`, `player_spawn` | 26 | 6,542 | Common (PhySys, AnimDB, IServer) | game-app | M |
| 3 | Networking glue: `IClientImpl`, login/server list, connect flow, exports | `client_iclient*` (2,756), `client_exports`, `login_mgr` (1,889), `server_connect`, `connect_*`, `server_filter*`, `reg_key_stack`, `nn_network_operation`, `client_base_market` | 25 | 6,717 | rpc_iclient.h, NeuronRuntime net (`CDPClient`, `CGunWrapper` — DirectPlay 8), Server.dll / RemoteServer.dll | game-app (event receiver), neuron-client (transport, server browser) | L |
| 4 | Client game objects and object DB | `client_gameobj` (5,370+1,070), `client_cliobj`, `client_objdb`, `client_mesh_load`, `client_fuse`, `player_behavior`, `star_sys`, `part_cache`, `best_path`, `cockpit_load` | 31 | 19,798 | Common (`CObject`/`CEqObj`, PhySys, Archetype, Universe), SceneRuntime, RendComp | game-app (on neuron-core object model) | XL |
| 5 | Space rendering glue and FX | `cli_*` (asteroids, nebula, projectiles, tractor), `nebula_volume`, `star_sphere`, `space_scene*`, `asteroid_billboard`, `texture_panels`, `texture_shapes`, `engine_light`, `tradelane_fx`, `gate_tunnel`, `jump_effect`, `beam_effect`, `client_explosion`, `client_debris`, `client_equip_fx`, `particle_effect`, `vis_effect_arch`, `effect_*`, `fl_fx_mesh`, `space_dust`, `render_ex` | 76 | 46,653 | IRenderPipeline (D3D8-style state), Shading, RendComp, particlefx, x86math, Common zones | neuron-client (generic effect/batch layers), game-app (game-specific FX rules) | XL |
| 6 | Cameras | `space_camera*` (10 modes), `ship_camera_params`, `room_camera` | 24 | 8,562 | x86math, SceneRuntime, `cameras.ini` | game-app | M–L |
| 7 | HUD and in-space UI | `hud_*` (targeting + contacts = 12,228), `wire_controls`, `chat_*` (3,422), `button_mover`, `tooltip_hover` | 41 | 28,283 | toolkit (#10), Common objects, IRenderPipeline | game-app | XL |
| 8 | NeuroNet windows, base UI, shared dialogs | `nn_*` (nav map 9,815; inventory/dealer/trader/trade/repair/scanner/news/mission-vendor ≈ 21,000; `nn_*_control` + modal/tooltip/rollover ≈ 10,500 are really toolkit; `nn_load_save` 1,294; `nn_preferences` 3,882), `nav_bar`, `commodity_by_base`, `ship_view_control`, `character_info`, `knowledge_map`, `infocard_map` | 95 | 53,095 | toolkit, IServer, Common goods/reputation | game-app | XL |
| 9 | Front-end pages, options model | `page_*`, `menu_ui*`, `menu_universe_base`, `type_select`, `perf_options`, `game_options`, `menu_fonts_load` | 29 | 9,628 | toolkit, Common INI | game-app | L |
| 10 | UI toolkit (Win3D) | `win3d*`, `win_factory`, `win_object`, `wincontrol_*`, `ui_*`, `list_box`, `scroll_control`, `text_win`, `rich_text_win`, `font_*`, `image`, `screen_quad_state`, `event_handler` | 63 | 23,138 | IRenderPipeline, TextureLib, **GDI** | neuron-client | XL (L if internals re-platformed) |
| 11 | Base rooms, scene-script glue, characters, walk mode | `gf_*` (script objects, 9 `gf_scenescript_*` component adapters), `room_control*`, `engine_character`, `base_walk*` | 38 | 16,072 | SceneScript, Deformable2, SceneRuntime | game-app (scene selection, hotspots) + neuron-client (script component hosts) | L |
| 12 | Input, key map, IME | `input_manager` (1,274), `menu_keymap*`, `client_ime_focus`, `fl_ime_input_box` | 9 | 3,314 | DirectInput 8, imeui | neuron-client (devices, IME), game-app (command table, bindings) | M |
| 13 | Audio glue | `menu_sound_arch` (3,729), `sound_config`, `voice_profiles`, `sound_archetype` | 7 | 4,815 | SoundManager, SoundStreamer | neuron-client (archetypes, mixer tick), game-app (music/voice triggers) | M |
| 14 | Localisation | `ids_strings`, `ids_rdl` | 4 | 898 | Win32 resources, Common `RenderDisplayList` | neuron-core (string/RDL store), game-app (IDs) | S |
| 15 | Diagnostics | `client_tick_profile` | 2 | 416 | QPC, fonts | neuron-client (feature flag) | S |
| 16 | Widescreen helpers | `widescreen.h`, `hud_widescreen` (+92 `#ifdef` sites) | 1 | 61 | — | folded into camera/UI design | S |

Notes on specific items requested:

- **Audio / "AIL_"**: there is no `AIL_*` (Miles) identifier anywhere under `CppSource` (the only
  grep hit is the option string `DETAIL_LEVELS`). Audio is the engine's own `ISoundManager` /
  streamer interfaces (`NeuronRuntime::Sound` 42 uses, `NeuronRuntime::Streamer` 19).
  `menu_sound_arch.cpp` is baseline `Client\cmn\SoundArch.cpp`: sound archetype INI, 2D/3D sources,
  background music/ambient binding and fades, ducking, five volume buses, the per-frame mixer tick
  (`:3364`). The similarly named `ALELib` (`vis_effect_arch.cpp:51-68`) is the `.ale` visual-effect
  library, not audio.
- **Save/load UI**: `nn_load_save.cpp` is UI only; saving is `G_Server->SaveGame(...)` (`:611`,
  `:814`). The client writes no save files itself.
- **Exported surface**: `client_exports.cpp` defines the data export `Client` (an `IClientImpl`,
  12 bytes: `IClient` vptr, `IChatClient` vptr, one unknown dword) plus `GetClientInterface` /
  `GetChatClientInterface`. The exe also exports `IClient` special members and `MD5Hash` members to
  reproduce the baseline 69-name export table (vcxproj comment, `OUTPOST_FRONTIER_EXPORTS`).
  Back-reference: in single player `Server.dll`/`rpclocal` call
  `LoadLibraryA(SStartupInfo.ModulePath)` (the exe itself) + `GetProcAddress("GetClientInterface")`
  (`rpclocal/rpc_client_proxy.cpp:266-291`, `Server/IServerImpl.cpp:2788-2792`); in multiplayer
  `RemoteServer.dll` imports the `Client` data symbol at link time, which is why
  `RemoteServer.vcxproj:62` links `OutpostFrontier.lib` and why the exe has an import library at
  all (`RemoteServer/RemoteServer_main.cpp:30-46`, dispatch at `:504`). In Rust this collapses to a
  `ClientEvents` trait object handed to the server/transport at startup; no dynamic back-link.

## 2. Main loop and frame anatomy

`WinMain` (`main.cpp:682`): optional single-instance mutex (`:695`, compiled out) → chdir to exe
folder → probe `..\DATA\CHARACTERS\ANIMATIONS\FACIALMALE.ANM`, and if absent spawn
`OutpostServer.exe` and exit (`:713-726`) → EULA gate via `EBUEula.dll` (`:731`) → install fatal /
warning callbacks → `CoInitialize`, `atexit(ClientAtExitTeardown)` (`:738-739`; **all teardown runs
from atexit**, 40+ ordered calls at `:500-585`) → `LoadLibraryA("UiText.dll")` → command line,
`OutpostFrontier.ini`, spew log → `RegisterClassA` / `CreateGameWindow` (`:785-787`) →
`InitializeEngine` (`engine_init.cpp:671-953`: NeuronRuntime startup, pipeline, `PerfOptions.ini`,
render mode, startup movies, splash, `DataConfig::startup`, client INI data, Win3D manager, fonts,
NeuroNet) → enter initial state → loop:

```
for (;;) {                       // main.cpp:799-807
  UpdateClientTiming();          // sleep-spin fps cap, measure dt
  dt = GetClientFrameTimeCapped();
  if (!PumpMessages(&exit)) break;   // DI mouse queue, then Peek/Get/Translate/Dispatch
  if (ClientPresentAllowed()) TickClientFrame(dt);
}
```

`TickClientFrame` (`client_frame_tick.cpp:187-293`), in order:

1. `Timing::UpdateGlobalTime(dt)` unless the server tick is paused (`:192-193`).
2. Multiplayer disconnect check → modal error (`:207-220`).
3. **UI update**: `Win3DUpdateAll(dt)` for all registered window roots (`:225`).
4. **In-process server**: `TickLocalServer` → `IServer::Update()` then `IServer::ElapseTime(dt)`
   (`server_connect.cpp:154-162`), on the main thread, same variable dt. In multiplayer the same
   two calls go to `RemoteServer.dll`'s proxy.
5. **State update**: `current_state->Update(dt)` (`:261`) returns the next state id (0 = stay);
   `EnterClientState` switches (ids: 1 quit, 2 space, 3 base, 6 intro, 7 load game, 8 mission
   briefing — `client_state.h`).
6. No state left → `PostQuitMessage`.

Each state renders and presents inside its own `Update`. In space (`space_state.cpp:1017-1205`):
client object update (`:1043`) → `AnimDB::Update` → `PhySys::Update(system, dt)` (`:1054`) → beams
→ collisions → trade-lane/lights/docking/camera/star-system → sound mixer (`:1101`) →
`BeginScene` (`:1105`) → camera `DrawScene` → flush immediate batches → `Win3DDrawAll` (HUD/UI,
`:1130`) → fade → `EndScene` → reap objects, remote-physics interpolation, pending system switch →
`SwapBuffers` (`:1199`).

Timestep: **fully variable**, one sim step per rendered frame, client and local server alike. dt
is capped at 1.0 s (`client_timing.cpp:40,64`); the minimum frame time is enforced by a
`Sleep(1)` spin against an fps cap that defaults to 200 and is rewritten from
`Common::RtcSlider` when entering/leaving a base (`:37,55`, `SetClientFpsCap`). Single player
accumulates capped dt, multiplayer raw dt (`:98-108`). Pause is a depth counter that zeroes dt
getters and skips the server tick (`ShouldSkipLocalServerTick`).

Threads: client logic is single-threaded. `Common::Async` is synchronous
(`include/common_async.h:16`). The only lock in the client is a critical section around the
material cache (`client_cliobj.cpp:769-1138`). Other threads come from the engine: SoundManager
notify thread, SoundStreamer worker, `CGunWrapper` release thread, and DirectPlay's own workers
**(guess: callbacks are queued and drained by `IClient::Update`)**.

Re-entrancy: modal dialogs run **nested pump loops** that call `PumpMessages` and the current
state's `Update` from inside a UI handler (`nn_error.cpp:221-233`, `nn_prompt` ×3,
`server_filter_dialog`, `connect_handshake.cpp:82`; `CallClientStateUpdateSlot`,
`client_state.cpp:514`). `engine_init.cpp` pumps messages 10 times during boot.

Floating point: frame begin/end toggle x87 precision control and an optional FP-exception trap
(`client_frame_tick.cpp:79-93,153-166`, `+X` switch).

## 3. Port-only feature flags

Defined in `CppSource/Directory.Build.targets`; `OutpostFrontier.Diagnostics.props` overrides the
defaults (it currently sets **WalkMode=1**, while `Directory.Build.props` defaults it to 0).

| Flag | Client footprint | Invasiveness |
|---|---|---|
| `OUTPOST_FRONTIER_WIDESCREEN_SUPPORT` (on) | 92 sites / 31 files. Core in `widescreen.h`, `hud_widescreen.*`; 20 sites in `win3d.cpp` (every draw/update/hit-test wrapped in a camera-shift scope, `:223-311,450-474,705`), 7 each in `space_camera.cpp` / `room_camera.cpp`, the rest in HUD, nav map, options page, fonts, cursor, star sphere | High, cross-cutting. Design in from the start (Hor+ FOV, HUD corner anchoring) rather than port as `cfg` |
| `OUTPOST_FRONTIER_WALK_MODE` | 40 sites / 13 files. `base_walk.cpp` (681) + hooks in `gameflow`, `gf_character_object`, `input_manager`, `menu_keymap`, `nav_bar`, `room_control`, `win3d*`, `screen_quad_state` | Medium; isolated module plus input/camera hooks |
| `OUTPOST_FRONTIER_DX9RENDERER` (off) | 46 sites / 10 files. `render_ex.*`, `nebula_volume.*` (≈4,400 LOC), hooks in `cli_asteroid` (12), `space_scene_obj_draw` (12), `cli_nebula`, `star_sphere`, `star_sys`, `client_gameobj`; reads `DATA_EXTRA/VOLUMETRIC/volumetric.ini` and HLSL from `DATA_EXTRA/SHADERS` with hot reload | Medium; cleanly gated behind `RenderEx::IsAvailable()` |
| `OUTPOST_FRONTIER_BETA_LABEL` (off) | `client_tick_profile.*` only | Trivial |
| `OUTPOST_FRONTIER_ENABLE_TICK_PROFILE` (off) | `client_tick_profile.*`; RAII scopes at ~20 call sites in `client_frame_tick.cpp`, `space_state.cpp`, `client_objdb.cpp` | Trivial; maps to a tracing span feature |

## 4. Platform coupling inventory

110 distinct Win32/CRT platform functions are called directly; `<windows.h>` is included by 90
files. There is **no direct D3D, DirectSound or DirectPlay COM call** in the client — but see the
renderer row.

| Family | Where (file:line) |
|---|---|
| Window class / window / styles | `main.cpp:771-787`; `window.cpp:31-56` (create), `:62-104` (fullscreen ↔ windowed restyle, topmost), `:214-289` (WndProc: focus, `WM_ACTIVATEAPP`, `WM_NCHITTEST`, `WM_SYSCOMMAND`, close) |
| Message pump | `message_pump.cpp:15-38`; boot pump `engine_init.cpp:78`; nested pumps `nn_error.cpp:233`, `connect_handshake.cpp:82` |
| **DirectInput 8 — mouse only** | `input_manager.cpp:305-311` (`DirectInput8Create`, `GUID_SysMouse`, `c_dfDIMouse2`), `:288` (buffer size 0x80), `:370-371` (non-exclusive/background), `:398-399` (exclusive/foreground), `:416-425` (Acquire, GetDeviceState), `:502` (GetDeviceData), `:559`, `:1060` (Poll), `:1135` (Unacquire). Software cursors from `mouse.ini` (`:1122-1123`). **No joystick or gamepad code exists** (zero matches for `joy`), and no DirectInput keyboard |
| Keyboard | `WM_KEYDOWN/KEYUP/SYSKEY*/CHAR` routed by `KeyMapRouteInputMessage` (`menu_keymap.cpp:626-679`); `GetKeyState` 25 sites (16 in `menu_keymap.cpp`, also `gameflow.cpp:435`, `hud_command_dispatch`, `mission_briefing`, `nn_preferences`, `space_camera_game`); key names via `GetKeyNameTextW`/`MapVirtualKeyA` in `nn_preferences.cpp` |
| Cursor | `ShowCursor` 13 sites (`main.cpp`, `engine_init.cpp:126,778-794`), `SetCursor`/`LoadImageA` system cursors `ui_table.cpp:120-122`, `SetCursorPos`/`GetCursorPos`/`ClientToScreen` in `input_manager.cpp` |
| **GDI text and images** | `font_imp.cpp:117` (`CreateFontIndirectA`), `:127`, `:352`, `:472` (`GetTextExtentPointW`), `GetCharWidthW`; `font_atlas.cpp:169-207` (DIB scratch), `:438` (`ExtTextOutW`), `:452` (`GetDIBits`); `rich_text_win.cpp:56-59,307,356-357` (DIB surface, brush, `GetDIBits`); `image.cpp:276` (`LoadImageA` for `.bmp`), `:333` |
| Registry (HKCU/HKLM `Software\Microsoft\Microsoft Games\OutpostFrontier\1.0`) | `reg_key_stack.cpp:8-92`; saved accounts/passwords (hex-encoded) `login_mgr.cpp:54-64,178-274`; table column/row edges as `REG_BINARY` floats `ui_fast_table.cpp:486-519`; `LangID` for crash report `main.cpp:185-195` |
| Winsock | only `inet_addr`/`ntohl`: `login_mgr.cpp:410,572,999` |
| DirectPlay 8 (via NeuronRuntime classes) | `CLoginMgr : IGunQueueReader, CDPClient` (`login_mgr.h`); `CDPClient::SetSourcePort` `main.cpp:477`; `CDPServer::CrashCleanup` `main.cpp:130`; stall detection `client_timing.cpp:85`; `hud_targetable_objects.cpp` (4 sites) |
| Resources / localisation | `LoadLibraryA` of resource DLLs (`main.cpp:741`, `dataconfig_progress.cpp:212`); `FindResourceExA`/`LoadResource`/`LockResource` `ids_strings.cpp:49-102` (types 6 and 0x17), `ids_rdl.cpp:240-274` |
| Shell / process / misc | `CreateProcessA` (`dxdiag`, `dw15.exe`, `OutpostServer.exe`: `main.cpp:96,223,720`), `_spawnl("Flasher.exe")` `menu_enter_state.cpp:618`, `sensapi.dll!IsNetworkAlive` and `shdocvw.dll` `type_select.cpp:218-283`, crash shared-memory block `main.cpp:137-270`, `SetUnhandledExceptionFilter` `:291`, SEH `__try` ×3 |
| IME | through `imeui` (`client_ime_focus.cpp`, `fl_ime_input_box.cpp`; first refusal on every message in `window.cpp:220-226`, hot-key filter `message_pump.cpp:28`). No direct `Imm*`. No clipboard use at all |
| Timers | `timeGetTime` 13 sites, `QueryPerformanceCounter` 4 (profile), `GetTickCount` 4, `Sleep` 7 |
| Display modes | `EnumDisplaySettingsA` `widescreen.h:52`; `GetDesktopWindow`/`GetWindowRect` `window.cpp:73`; `GlobalMemoryStatus`, `GetVersionExA` (low-machine warning, `engine_init.cpp:373-452`) |
| **Renderer access** | Always through `IRenderPipeline`, but that interface *is* D3D8 fixed-function: 457 pipeline references in 56 files, 304 `SetRenderState`, 120 `SetTextureStageState`, 333 `D3DRS_*/D3DTSS_*`-style constants in 51 files, 38 draw-primitive calls. `win3d_immediate.cpp` is an OpenGL-immediate-mode emulator (`glBegin/glVertex/glEnd`) on top of it. `QueryInterface` to `RPVertexBuffer/RPIndexBuffer/RPDraw` (`nn_navmap.cpp:318-324`), `GammaControl` (`engine_init.cpp:486`), `RPTexture::PrintScreen` (`main_command_handler.cpp:120`), Ex interfaces (`render_ex.cpp:316-325`) |
| Movie | `movie.dll` by `GetProcAddress` on mangled `MediaPlayer::*` names (`engine_init.cpp:270-335`), up to three `[Startup] movie_file` entries |
| Sound | Only through `ISoundManager`/streamer interfaces; no DirectSound headers |

## 5. UI toolkit depth

- **Model**: `Win3D` (`win3d.h`, 0x7c bytes, 28 virtual slots) is a scene-graph node
  (`LinkTree` parent/sibling/child) with a 3×3 orientation and a 3D position. The interface is
  **3D geometry drawn in front of a dedicated "WinCamera"** (configured in `cameras.ini`,
  `space_state.cpp:567`); widgets are `.cmp/.3db` meshes (220 distinct mesh paths are hard-coded in
  the client), hit-testing is a mouse ray against half-extents.
- **Classes**: 62 `WINFACTORY<T>` registrations (name → constructor) plus 12 HUD item factories;
  ≈85 classes derive (directly or transitively) from `Win3D`. Communication between windows is an untyped
  `SetProperty(int Msg, uintptr_t A, uintptr_t B)` slot (e.g. `nn_navmap_input.cpp`,
  `nn_inventory.cpp`, each a 400+ line switch) and a global command registry (`EventHandler`,
  203 `USER_*` command nicknames in `menu_keymap_nicknames.inc`).
- **Layout specification — three mechanisms**:
  1. INI: `WinControl::LoadIni` (`wincontrol_base.cpp:338-413`) reads `[BaseFrame]` (`mesh`,
     `offset`, `mouse_size`, `z_order`, `tooltip`) and one section per child whose `class_name`
     selects a factory. `hud.ini` sections are matched to HUD factories
     (`hud_element_list.cpp:446-500`).
  2. Text pages: `PAGE_*.txt` parsed by `sscanf` into 12 widget types (`UIStaticText`,
     `UITextMsgButton`, `UITextEdit`, `UIListBox`, `UIWinObject`, `UIWinDeform`, `UIRichTextWin`,
     `UIShipViewControl`, `UITable`, `UIFastTable`, `UIScroll`, `UICycle`) with fractional bounds
     (`ui_page_desc.cpp`).
  3. Code: most NeuroNet screens build children procedurally (`NnPreferences::Create` 657 lines,
     `NeuroNetNavMap::Create` 487, `NnShipTrader::Create` 381) with literal offsets.
- **Text path**: font faces come from INI (`[TrueType] nickname/font/fixed_height`,
  `font_imp.cpp:591-632`) and are resolved by **GDI by face name** — no `AddFontResource` exists
  in the tree, so the faces must be installed in the OS **(guess: by the installer)**. Sizes are
  auto-scaled by measuring a pangram against 1/800 of the surface width (`font_imp.cpp:27-30`).
  Glyph runs are drawn with `ExtTextOutW` into a DIB, read back with `GetDIBits` and uploaded to
  atlas pages. Rich text (`rich_text_win.cpp`, 1,928 lines) renders through a GDI surface into an
  engine `BigImage`; content is UTF-16 string tables and RDL/XML-like markup in resource type 0x17.
- **Resolution/aspect**: positions are in camera space, so layout is resolution independent;
  baseline is 4:3 only. The port's Hor+ mode shifts HUD corner windows per object (section 3).

**Judgement.** Do not port 1:1 and do not swap in an off-the-shelf Rust GUI. Keep the *data-level
contract* — scene-graph of mesh-backed controls, factory lookup by class name, the INI and
`PAGE_*.txt` grammars, draw layers, IDS/RDL text — because ≈90k lines of screens (#7, #8, #9) and
the shipped data are written against it. Re-platform the *internals* in neuron-client: typed
messages instead of `SetProperty(int, uintptr_t, uintptr_t)`; arena/handle tree instead of intrusive
raw pointers; explicit modal stack instead of nested pump loops; a Rust glyph rasteriser instead of
GDI. The last item is the main fidelity cost: line breaks and autoscale depend on GDI metrics, so
either keep a GDI backend behind a trait for comparison runs or accept text-metric drift and
re-baseline.

## 6. Hazards

| Hazard | Evidence | Consequence for the port |
|---|---|---|
| Global state | ≈1,000 distinct `G_*` identifiers plus ≈110 `g_*/GG*` names; state singletons via getters (`GetSpaceState`, `GetGameflowState`, `GetHudStatus`, `G_LoginMgr`, `g_client`, `g_display`) | Needs an explicit `ClientContext` ownership design before screens are ported |
| Static-init order | 474 comments reference static init / atexit. Factories and command handlers self-register from static constructors. Two documented order dependencies: `vis_effect_arch.cpp` must link after `client_cliobj.cpp` (vcxproj comment), and `gameflow.cpp`'s hotspot handler must register before Win3D's so Win3D sees mouse events first (`gameflow.cpp:52-64`) | Replace with explicit registration order; encode handler priority |
| atexit teardown | `main.cpp:500-585` | Becomes ordered `Drop`/shutdown; order is load-bearing |
| Reconstructed-binary idioms | 1,086 `static_assert`s (358 `sizeof`, 713 `offsetof`), 4,428 baseline address comments, 2,529 `Field_0xNN`/`SlotNN` placeholders, 78 TODOs, 2,451 `reinterpret_cast`, 824 raw `ptr + 0xNN` accesses (e.g. `hud_element_list.cpp:62-70` reads `CEqObj*` at `IObjRW + 0x10`) | Many names/meanings are unrecovered; behaviour must be captured, not inferred. Offsets into *other modules'* objects need those modules' Rust types first |
| ABI layout coupling | `_ITERATOR_DEBUG_LEVEL=1` is forced in Release so `std::` containers keep VC6 sizes and cross-DLL field offsets hold (`Directory.Build.targets`) | Irrelevant once all sides are Rust, fatal for any mixed C++/Rust phase that shares structs |
| Pointer size | 742 `uintptr_t`, 1,917 `void*`; pointers travel through message parameters and `unsigned` fields; `sizeof == 0x..` asserts assume 4-byte pointers (`OutpostFrontierClient.h` asserts `sizeof(IClientImpl)==0x0c`) | x64 target requires typed handles; do not transliterate layouts |
| x87 semantics | 422 references; per-frame precision-control switching; comments note 64-bit mantissa evaluation (`input_manager.cpp:7-9`); `msvcrt_compat.h` wrappers for `atoi` wrap-around | Numeric results (camera, layout rounding, `ftol` truncation) will differ on x64 SSE2; define tolerances |
| Strings | Project is MBCS (`CharacterSet=MultiByte`), ANSI Win32 calls, 508 `wchar_t` uses, UTF-16 UI text, byte→wchar sign-extension widening (`ui_page_desc.cpp:78-92`), `CP_ACP` and `CP_UTF8` conversions mixed | Decide one internal encoding; keep UTF-16 only at resource boundaries |
| Raw structs to storage | Registry `REG_BINARY` float arrays (`ui_fast_table.cpp:486`); interface/visited/weapon-group state sent to the server as raw byte buffers (`game_options.cpp:167`, `character_select_equip.cpp:523`, `hud_weapon_groups.cpp:304`) which the server persists | Field widths and order must be pinned with the server assessor |
| Giant functions | 36 functions ≥ 300 lines, 171 ≥ 150 (largest: `HUD_ContactList::RebuildContacts` 675, `NnPreferences::Create` 657, `HUD_ContactList::Draw` 583, `NeuroNetNavMap::Update` 542) | Port with characterisation tests, then split |
| Macro generation | Negligible (3 `#define`s); one X-macro-like `.inc` table | None |
| Hidden client simulation | The client runs `PhySys::Update` and a full object DB mirror itself (`space_state.cpp:1054`, `client_objdb.cpp`) | game-app depends on the same neuron-core physics/object code as the server; ordering constraint |

## 7. External data dependence

Nothing under `DATA` or `EXE` is in the repository (`.gitignore` excludes `/bin/`); the client
cannot boot without the original install tree.

- Working directory is the exe folder; data root defaults to `..\Data`
  (`[OutpostFrontier] data path`). Startup probe file: `..\DATA\CHARACTERS\ANIMATIONS\FACIALMALE.ANM`.
- `OutpostFrontier.ini` beside the exe: `[OutpostFrontier]` (`data path`, `local_server`, AppGUID),
  `[Startup] movie_file` ×3, `[Data]` (`gate_tunnels`, `jump_effect`, `fonts`, `rich_fonts`,
  `sounds`, `voices`, `effects`, `effect_shapes`, `debris`, `intro`, `HUD`, `igraph`),
  `[Resources] DLL=` list of string DLLs whose list index is the high word of every IDS number
  (`dataconfig_progress.cpp:179-232`, `ids_strings.cpp:22-37`), `[ListServer]`, `[Login]`.
- Fixed-name INIs: `mouse.ini`, `cameras.ini`, `interface/KeyMap.ini`, `audio\SoundCFG.ini`,
  `cockpits\cockpit.ini`, `fx\lightanim.ini`, `solar\stararch.ini`, `interface\rollover.ini`,
  `interface\buttontextures.ini`, `interface\buttonmontage.ini`, `interface\UI\UITextures.ini`,
  `interface\baseside\navbar.ini`, `interface\infocardmap.ini`, `interface\knowledgemap.ini`,
  nav-map textures INI, `universe\missioncreatedsolars.ini`, `../Data/interface/keylist.ini`,
  `optlist.ini`; port-only `DATA_EXTRA/VOLUMETRIC/volumetric.ini` and shaders.
- Hard-coded assets: 170 `.3db`, 50 `.cmp`, 15 `.bmp`, 5 `.txm`, splash `.tga`, one `.vms`.
- DLLs by name: `UiText.dll`, `Server.dll`, `RemoteServer.dll`, `movie.dll`, `EBUEula.dll`,
  `ImeUi.dll`, `..\EXE\gundll.dll`; executables `OutpostServer.exe`, `dw15.exe`, `Flasher.exe`.
- Per-user: `Documents\My Games\OutpostFrontier\PerfOptions.ini` and `UserKeyMap.ini`
  (`perf_options.cpp:500-641`, `menu_keymap.cpp:409-419`), `%LOCALAPPDATA%\OutpostFrontier\FLSpew.txt`,
  `Pictures\OutpostFrontierShots\screenN.BMP`, plus the registry keys in section 4.

## 8. Tests and verification hooks

There are no client tests in the C++ tree (`test_size.cpp` is a stray layout probe) and the Rust
crates are 40-line stubs. Usable hooks that already exist:

- Screenshot to BMP through the pipeline (`TakeClientScreenShot`, `main_command_handler.cpp:98-125`).
- `FDUMP` spew log with original file/line tags; the tick profiler sections; debug options in
  `game_options.cpp` (ship IDs, interface timing, physics wires, draw-control toggles).
- `+Q` quick flag, `+F/+B/+M` display overrides, `+S host:port` direct connect, `[Login]` auto-login.
- 1,086 layout `static_assert`s (useful only as documentation for Rust).

Capture-and-compare ideas, cheapest first:

1. **Parser goldens**: dump parsed `KeyMap.ini`, `hud.ini`, `[BaseFrame]` INIs, `PAGE_*.txt`,
   `cameras.ini`, fonts and sound-arch INIs from C++ as JSON; compare with Rust parsers.
2. **UI tree dump**: after each screen's `Create`, walk the `Win3D` tree and emit name, class,
   position, half-extents, layer, flags. Deterministic and independent of rendering.
3. **IDS/RDL text**: dump every string and display list by numeric ID.
4. **Camera/HUD math vectors**: record inputs/outputs of `ThirdPersonCameraSolve`, projection of
   targets to screen, mouse-flight direction, with a tolerance (x87 vs SSE2).
5. **Input replay**: record the `FMouseEvent` queue and key messages with frame dt; replay with a
   fixed-dt clock (the loop is otherwise non-deterministic).
6. **IClient trace**: log every `IClientImpl` call (127 entry points) in a scripted single-player
   session; replay into the Rust receiver and compare resulting object-DB dumps.
7. **Screenshot diff** at fixed resolution with fixed dt, for menus first (text metrics will differ
   unless GDI is retained).

## 9. Rust mapping

neuron-client (platform and engine integration):

```
neuron_client::
  platform::{window, message_loop, clock, crash, user_paths, registry_store}
  input::{mouse (raw input replacing DirectInput), keyboard, cursor, ime}
  ui::{tree, control, factory, layout_ini, page_txt, draw_layers, modal,
       text::{font, atlas, rich, rdl}, widgets::{button, text, edit, list, table, scroll, ship_view}}
  render_glue::{frame (begin/end/present), immediate, screen_quad, batch}
  fx::{particle, beam, effect_lib, lights}       audio::{sound_arch, mixer, music, voice}
  net::{transport, server_browser}               diag::tick_profile
```

game-app (game and menu flow):

```
game_app::
  boot (engine init sequence, config, loading screen)
  flow::{state_machine, intro, space, base, mission_briefing, load_game}
  session::{client_events (IClient receiver), login, connect, player}
  world::{object_db, game_objects, equipment_fx, fuses, star_system, zones::{asteroid, nebula}}
  camera::{first, third, turret, rear, death, docking, script, room}
  hud::{core, status, target, contacts, targetables, maneuvers, waypoint, comm, chat, weapon_groups}
  neuronet::{nav_map, inventory, dealer, ship_trader, trade, repair, news, mission_vendor,
             player_info, load_save, preferences, dialogs}
  frontend::{pages, server_select, character_select, options}
  base::{rooms, hotspots, scene_script_host, characters, walk}
  options::{game_options, perf_options, keymap}      text::ids
```

`apps/outpost-frontier`: `main`, command line, single-instance, data-root probe, fatal dialogs.
Drop or stub: Dr. Watson (`dw15.exe`) block, dxdiag, EULA DLL, `Flasher.exe`, sensapi.

Suggested vertical slice order and effort (relative; XL ≈ several times L):

| Step | Slice | Needs from other groups | Effort |
|---|---|---|---|
| 1 | Boot: config, window, pump, clock, renderer bring-up, clear + present, screenshot | renderer, INI reader | M |
| 2 | Toolkit core: tree, mesh control, factories, INI/PAGE loaders, fonts, IDS; show main menu | mesh/texture loaders, resource DLL content | XL |
| 3 | State machine + in-process server session + `ClientEvents` + load a system (solars, star sphere) | server, Common object model, universe data | XL |
| 4 | Fly: player ship object, PhySys step, cameras, mouse flight, key map | physics | L |
| 5 | HUD: core, status, target, contact list, waypoints, weapon groups; projectiles/beams/explosions | FX, sound | XL |
| 6 | Space environment: asteroids, nebulae, trade lanes, jump tunnel, dust | — | XL |
| 7 | Bases: rooms, scene scripts, characters, nav bar, NeuroNet commerce, news, missions, load/save | SceneScript, Deformable | XL |
| 8 | Nav map, options/preferences, chat, mission briefing | — | L |
| 9 | Multiplayer: transport, server browser, account/character pages, player trade | RemoteServer/transport | L |
| 10 | Port-only: walk mode, DX9 volumetrics, tick profile | — | M |

Top risks: (1) the toolkit decision blocks ≈110k lines of UI; (2) the client embeds the shared
object model and physics, so it cannot run before neuron-core's `CObject`/PhySys ports exist;
(3) the renderer contract is D3D8 fixed-function state scattered over 56 files; (4) GDI text
metrics; (5) nested modal loops and global mutable state vs Rust borrowing; (6) unrecovered field
meanings (2,529 placeholders) inside the largest screens; (7) no runtime data in the repository.

## Open questions for the owner

1. Fidelity target for the client: behavioural equivalence with tolerances, or pixel/bit parity
   with the x87 baseline? This decides GDI retention and float handling.
2. Is a mixed C++/Rust phase planned (Rust client against C++ `Server.dll`, or the reverse)? If so,
   the `Client` export, `IClient` vtable order and VC6 container layouts become hard ABI
   constraints on x86 only.
3. Renderer contract for the Rust client: keep an `IRenderPipeline`-shaped fixed-function state API
   or define a higher-level draw API (and rewrite 300+ render-state call sites)?
4. Which port-only features are in scope: widescreen (assumed yes), walk mode (property sheets
   disagree on the default), DX9 volumetric nebula, tick profile, beta label?
5. Text: may fonts be bundled and rasterised in Rust, or must OS-installed faces via GDI remain?
6. Multiplayer transport: DirectPlay 8 wire compatibility with existing servers, or a new
   protocol? Is the global server list (`gundll.dll`) in scope?
7. Per-user state: keep registry keys (saved accounts, table layouts) or move to files? Is
   migration of existing `PerfOptions.ini` / `UserKeyMap.ini` required?
8. Drop the EULA gate, Dr. Watson reporter, `Flasher.exe`, sensapi probe and startup movies?
9. Joystick/gamepad: the baseline has none; is adding one in scope?
10. Where will the original `DATA` tree and resource DLL contents come from for CI and tests?

## Suggested port order within this group

1. Config, paths, command line, clock, window, pump (sub-system 1).
2. IDS/RDL text and INI-driven key map (14, part of 12) — small, testable with goldens.
3. Toolkit core and fonts (10), then the generic `nn_*_control` widgets from 8.
4. Front-end pages and options model (9) — first visible screen.
5. State machine, server connect, `ClientEvents` (2, 3 single-player path).
6. Object DB and game objects (4), cameras (6), mouse input (12).
7. Audio glue (13) and core FX (5: projectiles, beams, explosions, engines, lights).
8. HUD (7).
9. Environment FX (5: asteroids, nebulae, trade lanes, tunnels).
10. Base rooms and scene-script glue (11), NeuroNet commerce, nav map, load/save (8).
11. Multiplayer: login manager, server browser, character pages, chat, player trade (3, 7, 9).
12. Port-only extras: walk mode, DX9 Ex hooks, tick profile (11, 5, 15).
