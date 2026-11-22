# C++ reference build

How to build and run the C++ code base in `CppSource/` as the reference for
the migration (work package P0.2 in the [migration plan](migration-plan.md)).
This needs a Windows machine and an original installation of the game. The
installation must not be committed. The DirectX 8.1 SDK headers and
libraries are committed in `CppSource\DX8\` (decision O11).

## 1. Requirements

| Item | Detail |
| --- | --- |
| Windows | 10 or 11, x64 |
| Visual Studio | 2026, workload *Desktop development with C++*, with the individual components *MSVC v145 build tools (x86 and x64)*, *C++ MFC for v145 build tools (x86 and x64)* and a Windows 10 or 11 SDK. The solution sets toolset `v145` and builds `Win32` only |
| Python 3 | On `PATH`. Needed only by the Debug and DebugASan configurations |
| DirectX 8.1 SDK | Headers and libraries, committed in `CppSource\DX8\` (section 2) |
| DirectPlay | Windows optional feature (section 5) |
| Original installation | Its game files, kept as the master copy in `outdir\` (section 3) |

The current Windows SDK already provides DirectSound, DirectInput,
DirectDraw, DirectShow and `dxguid.lib`. The DirectX 8.1 SDK is needed only
for Direct3D 8, D3DX 8 and DirectPlay 8.

## 2. DirectX 8.1 SDK

The SDK's `include` and `lib` folders are committed in `CppSource\DX8\`
(commit `fd5f6c1`). A clone needs no further step.

The build takes the SDK from the first of these that applies:

1. the path passed to MSBuild as `/p:Dx8SdkDir=<path>`;
2. `dx8\` at the repository root, when `dx8\include` exists. The folder is
   ignored by git;
3. `CppSource\DX8\`.

Do not add the SDK's `include` folder to a project's additional include
directories: the SDK's old `basetsd.h` breaks the current Windows headers.
The projects append it after the Windows SDK through `$(Dx8SdkDir)` in
`CppSource\Directory.Build.props`.

Microsoft no longer distributes the SDK. To check or replace the committed
files, the Internet Archive holds the signed original as item
[`dx81sdk_full`](https://archive.org/details/dx81sdk_full)
(`dx81sdk_full.exe`, about 166 MB, dated 2001-11-03). Check its signature
before use:

```powershell
Get-AuthenticodeSignature .\dx81sdk_full.exe | Format-List Status, SignerCertificate
```

The signer must be Microsoft. A certificate from 2001 can report as
expired; `HashMismatch` or `NotSigned` means the file is not the original.

`d3dx8.lib` is the reason the real SDK is needed. It is a static library of
texture code built with Visual C++ 6. The project files already work around
its `libci.lib` default-library reference and its missing SafeSEH tables.

## 3. Folder layout

All paths are relative to the repository root. `outdir\` is committed
(decision O15); every other folder below is ignored by git.

```text
dx8\          optional: a local DirectX 8.1 SDK that overrides CppSource\DX8\
outdir\       the master copy of the game files, committed: data\ (the
              installation's DATA folder, plus data\text\, the TOML text
              sources), the INI files, the .fl templates and
              flconfigdatabase.txt
bin\          Release build output, plus the runtime files listed below
bin_debug\    Debug build output; copy_dev_ini.py fills it from bin\
DATA_EXTRA\   optional: shaders and settings for the volumetric nebula
```

`outdir\` is the reference for every game file. The run folders are copies
of it and may carry local edits; the oracle reads `outdir\`.

The client switches to its own folder and expects the game data in `data\`
there, beside the executable (the default `data path` is `.\data`; the
original looked in `..\DATA`). It probes
`data\CHARACTERS\ANIMATIONS\FACIALMALE.ANM`; when that file is missing it
starts `OutpostServer.exe` instead and exits. Run the binaries from a folder
that holds `data\` (a copy of `outdir\data\`, or a junction to it) and the
INI files. `DATA_EXTRA\` then also sits beside the executable. The client and
server no longer show the original licence screen, so they need neither
`EBUEula.dll` nor `EULA.rtf` and `Warranty.rtf`.

A copied `data\` goes stale when a commit changes `outdir\data`: after
decision O17 an old copy loads no particle node library, leaves the main
menu's background scene empty and crashes on docking (section 6). Compare it with `diff -rq` after pulling data
commits, or use a junction where no local edit is needed.

On the owner's machine the run folders differ from `outdir\` in three
values: the client INI comments out the three `[Startup] movie_file` lines
and names `intro` by an absolute path into the run folder, and
`data\INTERFACE\intro.ini` sets `bDrawScripts = false` (the master has
`true`), so the main menu shows no intro scene.

Copy these files from `outdir\` to `bin\`, where present:

| Files | Why |
| --- | --- |
| `*.ini` | Engine, client, server and resource configuration |
| `NewPlayer.fl`, `NewCharacter.fl`, `MPNewCharacter.fl` | Templates for new single-player games and characters |
| `flconfigdatabase.txt` | Read by the Direct3D 8 renderer |

Do not copy the original game DLLs. The build produces its own.

The INI files name DLLs. In `outdir\` and its copies, every DLL name must be one
that this build produces: the engine INI's `[Libraries]` list, the client
INI's `[Resources]` list and `local_server` key, and the server's
`[Initial SP DLLs]` and `[Initial MP DLLs]` rows. The build produces these
DLLs:

```text
Common  Content  DX8RENDERER  DX9RENDERER  Deformable2  DialogueText
EquipmentCatalog  FieldGuideText  InterfaceExtras  NameCatalog
NegotiationText  NeuronRuntime  ReadFile  RemoteClient  RendComp
RenderTrace  SceneRuntime  SceneScript  Server  Shading  SoundManager
SoundStreamer  UiText  imeui  luaprofile  movie  neuroncomponents
particlefx  rpclocal  remoteserver  serverinterface  surfacematerials
system  x86math  ximage  zlib
```

`RenderTrace.dll` is the renderer logging proxy of work package P0.5
(section 7, "Renderer traces"); the master INI does not name it.

`DX9RENDERER.dll` is built only with `/p:OutpostFrontierDx9Renderer=1`. An
INI entry that names a DLL not in this list is a fact to record in
[facts.md](facts.md) (row F11).

## 4. Build

From a *Developer PowerShell for VS* at the repository root:

```powershell
msbuild CppSource\OutpostFrontier.sln /m /p:Configuration=Release /p:Platform=Win32
```

Start with Release. Debug and DebugASan run `CppSource\copy_dev_ini.py`
after linking the client, which copies the runtime files from `bin\` into
`bin_debug\` or `binasan\` and fails when `bin\` does not exist yet.

Feature switches, passed as `/p:<name>=0|1`:

| Property | Default | Effect |
| --- | --- | --- |
| `OutpostFrontierWidescreenSupport` | 1 | Hor+ cameras and HUD anchoring |
| `OutpostFrontierWalkMode` | 0, but 1 for the client through `OutpostFrontier.Diagnostics.props` | Walking in base rooms |
| `OutpostFrontierDx9Renderer` | 0 | Builds `DX9RENDERER.dll` and the volumetric nebula hooks; needs the DirectX SDK of June 2010 |
| `OutpostFrontierEnableTickProfile` | 0 | Physics and frame timing overlay |
| `OutpostFrontierBetaLabel` | 0 | On-screen label |

For reference output, build with the defaults and record any switch you
change in [facts.md](facts.md).

## 5. Run

1. Enable DirectPlay once, from an elevated prompt. Multiplayer needs it,
   and the client's login manager uses it, so enable it even for single
   player:

   ```powershell
   DISM /Online /Enable-Feature /FeatureName:DirectPlay /All
   ```

2. Start `bin\OutpostFrontier.exe` with `bin\` as the working directory.
3. Single player: start a new game and fly. This is the Gate G0 check;
   it passed on 2026-11-16 with commit `816f556a`.
4. Dedicated server: start `bin\OutpostServer.exe`; connect a client to
   `127.0.0.1`.

Per-user files go to `Documents\My Games\OutpostFrontier`, the log to
`%LOCALAPPDATA%\OutpostFrontier\FLSpew.txt`. The client also writes registry
values under `HKCU\Software\Microsoft\Microsoft Games\OutpostFrontier\1.0`.

## 6. When it does not work

Record every problem and its fix in this section, with the commit that
fixed it. The only changes allowed in `CppSource/` are build fixes and the
oracle instrumentation of work packages P0.4 and P0.5. The one exception is
the plain-text save writer of commit `816f556a`, recorded as decision O12.

| Symptom | Cause | Fix |
| --- | --- | --- |
| `MSB8041` (MFC libraries are required) when building `OutpostServer` with toolset v143 | The Visual Studio 2026 installation has MFC only for v145 | The solution uses toolset v145 (commit `a86a199`) |
| `LNK1181` (cannot open input file) for `Common.lib`, `Server.lib`, `rpclocal.lib` or `NeuronRuntime.lib` in a parallel build (`/m`) | `Server`, `RemoteServer` and `RemoteClient` linked these libraries without project references, so MSBuild could build them too late | Project references added (commit `a86a199`) |
| `LNK1181` for `neuroncomponents.lib` in `Deformable2` in a parallel build (`/m`), first seen in CI's Reference build job | `Deformable2.vcxproj` set `BuildProjectReferences` to `false`, so its reference did not order the build; `Content` and `RemoteClient` linked `neuroncomponents.lib`, `Common.lib` and `NeuronRuntime.lib` without references | The setting removed and the references added (commit `ddf4475`) |
| With `bDrawScripts = false` in `intro.ini`, the main menu is not drawn and has no mouse cursor | `MenuStateDriverUpdate` returned before it began the frame when `bDrawScripts` is clear, and the cursor was set only on the path that enters the intro base (`client_state.cpp`) | The menu frame is always drawn and only the intro scene work depends on `bDrawScripts`; the cursor is set before that test (commit `816f556a`). With `bDrawScripts = true` the menu behaves as before |
| `+oracle` writes no screenshots: every `dump` record has `"shot":""`, and `FLSpew.txt` shows `print_screen_save_surface: D3DXERR_INVALIDDATA` | The mode asked `PrintScreen` for `.png`; D3DX 8.1 does not save PNG | The mode writes `.bmp`, as the client's own screenshot key does (P0.5 owner run, 2026-11-17) |
| Tree dumps of two identical `+oracle` runs differ in `flags` | The Win3D constructor keeps bits 4 to 7 of the flag byte from the heap (`Flags & 0xf8`), as the original does | The dump writes bits 0 to 3 only (P0.5 owner run, 2026-11-17) |
| Every particle effect fails with `couldn't create instace of node` (engine fire, trails, contrails, explosions), and the client ends with `0xC0000005` in `particlefx.dll` at offset `0x7ecd` when a new single-player game's opening scene docks at the first base. The main menu's 3D background scene stays empty | The run folder's `data\` was copied before decision O17 renamed the node library in the 596 `.ale` files to `ParticleFxNodeLibrary`. The code looks for that name, so `OpenNode` failed in every file, no node library loaded, and every effect was created without nodes. `FFxEffectObject::Slot4` then reads `Nodes_[0]` without a count check, as the original does | Refresh the run folder's `data\` from `outdir\data` after any data commit (the 596 `.ale` and 851 `.thn` files of O17), keeping the local `intro.ini` value. No code change (P0.5 owner run, 2026-11-17) |

## 7. Then

Fill in [facts.md](facts.md) (work package P0.3; done on 2026-11-16). Gate
G0 also needs the oracle harness of work package P0.4.

The harness is the `Oracle` project in the solution's `Tools` folder. Its
synthetic suites need no game data. To run them by hand after a Release
build:

```powershell
New-Item -ItemType Directory -Force oracle-out
.\bin\Oracle.exe "$PWD\crates\neuron-core\tests\fixtures\oracle" "$PWD\oracle-out"
git diff --no-index crates\neuron-core\tests\vectors oracle-out
```

The suites cover P1.1 to P1.5 and P1.9: hashing and IDs, numbers, the
random generator, both INI readers (`oracle_main.cpp`), the math kernels
(`oracle_math.cpp`, which loads `x86math.dll`), the utilities
(`oracle_util*.cpp`), the UTF container and file systems (`oracle_vfs.cpp`)
and the wire primitives (`oracle_net*.cpp`). Helpers shared by all of them
are in `oracle_support.h`.

### Text catalogs

`Oracle.exe catalog <dll folder> <output folder>` dumps every string and
rich-text card of the eight resource DLLs as the game reads them (work
package P1.6). The output is game text and must not be committed. CI's
oracle job builds the DLLs, runs the dump and compares it with the catalogs
that `cargo xtask catalog` builds from `outdir/data/text/`. By hand, after
building the `ResourceDLLs` projects:

```powershell
.\bin\Oracle.exe catalog "$PWD\bin" "$env:TEMP\catalog-oracle"
cargo xtask catalog --out "$env:TEMP\catalogs"
$env:OUTPOST_CATALOG_DIR = "$env:TEMP\catalogs"; $env:OUTPOST_ORACLE_DIR = "$env:TEMP\catalog-oracle"
cargo test -p neuron-core --test catalog_dump -- --nocapture
```

### Data dump

The data dump runs both INI readers over the master in `outdir\` and
compares the Rust port with the C++ (work package P1.2, Gate G0). Both
programs read a folder that holds `DATA` and `EXE`: every `.ini` file under
them through the game reader, and the files directly in `EXE` through the
engine parser as well. Make that folder outside the repository from a
junction to `outdir\data` and a copy of the master's INI files. The dump
writes one file per INI file, about the size of the INI files themselves or
larger. None of it may be committed. From the repository root, after a
Release build:

```powershell
$root = "$env:TEMP\outpost-data"
New-Item -ItemType Directory -Force "$root\EXE" | Out-Null
New-Item -ItemType Junction -Path "$root\DATA" -Target "$PWD\outdir\data" | Out-Null
Copy-Item outdir\*.ini "$root\EXE\"
.\bin\Oracle.exe dump $root "$env:TEMP\outpost-oracle"
```

```powershell
$env:OUTPOST_DATA_DIR = $root; $env:OUTPOST_ORACLE_DIR = "$env:TEMP\outpost-oracle"
cargo test --release -p neuron-core --test data_dump -- --nocapture
```

The test prints a summary and writes it to `rust-result.txt` in the oracle
folder: how many files opened, which could not be opened, and the first
differences, if any. Attach that file to the pull request that closes P1.2.
CI runs the same pipeline on a small synthetic tree, so a failure here
comes from the data.

Result on 2026-11-16, from `outdir\`: 1256 files, all opened; 3 also
through the engine parser; 1259 dumps compared, 0 differ.

To read values for [facts.md](facts.md), `cargo xtask ini` lists files or
folders through the same reader:

```powershell
cargo xtask ini outdir --grep Libraries
```

`cargo xtask thn-decompile` turns the compiled `.thn` scene scripts back
into Lua 3.2 source ([facts.md](facts.md) F15). Each result is compiled
again and must equal the input byte for byte, or it is reported and not
written. The output is made from the game data: keep it outside the
repository. The round-trip test reads every `.thn` file in `outdir/data`
and runs with the workspace tests.

```powershell
cargo xtask thn-decompile outdir\data "$env:TEMP\thn-source"
cargo test --release -p xtask thn::tests::original_data_round_trips -- --nocapture
```

## 8. Client oracle mode (`+oracle`)

Work package P0.5, first half: the client instrumentation that
[verification.md](verification.md) lists as the `+oracle` row of the
oracle. The renderer call trace is the other half (`CppSource/RenderTrace/`).
The code is in `CppSource/OutpostFrontier/Client/oracle_*` and
`make_oracle_iclient.py`; the example scripts are in
`CppSource/OutpostFrontier/oracle/`. Without `+oracle` every hook returns at
once and the client behaves as before.

### Command line

```powershell
.\bin\OutpostFrontier.exe +oracle <script file> <output folder>
```

`+oracle` is matched as a whole token in any case; the other command-line
loops read it as the letter `O`, which none of them handles. The two paths
are the next two tokens (double quotes allowed). Relative paths are taken
from `bin\`, because the client makes its own folder the current directory.
The output folder is created if missing (its parent must exist); files in it
are overwritten. A missing argument, an unreadable script or a folder that
cannot be created ends the process with exit code 3 and a line on the
debugger output.

The mode forces three things and nothing else:

| What | How |
| --- | --- |
| Display | Windowed, 1024x768, 32-bit colour: InitializeEngine's render mode after `ApplyDisplayCmdlineOverrides`, the same fields `+W` and `+M1024` set. `SavePerfOptions` then writes that mode to `PerfOptions.ini`, so run the oracle from its own copy of the run folder |
| Time step | `TickFrameTimer` paces frames at 1/30 s of real time and reports exactly 1/30 s as the measured and capped frame time; every caller of `GetClientFrameTimeCapped`, the local server tick and `Timing::UpdateGlobalTime` see it |
| Random seed | `srand(0x4f524143)` right after InitializeEngine's `srand(timeGetTime())` |

The window must stay in front and uncovered: a windowed Direct3D 8 screenshot
copies the screen area of the window, and the client skips frames while it is
not the active application. Use a run folder whose INI files play no startup
movies (section 3). Leave out `UserKeyMap.ini` in the user folder, or the
`user` phase of the key-map dump carries local bindings. Park the mouse
pointer outside the window (a screen corner) before the start: the menu's
software cursor follows the real pointer and is drawn into the screenshots.

Known nondeterminism that the mode does not remove, because these files read
the real clock directly: `type_select.cpp` (the main-menu column animation),
`login_mgr.cpp`, `menu_sound_arch.cpp` (sound tick counts), `engine_init.cpp`
(only the seed the mode replaces), `render_ex.cpp` and `nebula_volume.cpp`
(Direct3D 9 build only), `client_tick_profile.cpp` (overlay off by default),
and the frame timestamp of `client_timing.cpp` (`GetClientFrameTimestampMs`).
The DLLs (Server, Common, sound, renderer) are not audited. Pacing at 30 fps
keeps the real clock close to game time, so these drift rather than diverge.

### Script

One command per line; blank lines and lines starting with `#` are skipped.
The script runs at the start of each frame, line after line, until a line
makes it wait. The end of the script acts as `quit`. Numbers are decimal, or
hex after `0x`.

| Command | Effect |
| --- | --- |
| `wait <frames>` | continue that many frames later |
| `waitscreen <kind> <id> <frames>` | continue once the latest screen change of `client`, `menu` or `room` has that id (at once if it already has); after `<frames>` frames continue anyway with result `timeout` |
| `dump [label]` | interface tree and screenshot now |
| `tree [label]`, `shot [label]` | only one of the two |
| `command <USER_NICKNAME>` | dispatch the key-map command as a matching key press does: the event record filled from the command's first active binding (else its first binding), `HudDispatchCommand(id, 1)` on key down if its state has `keydown`, again on key up if it has `keyup`. The raw-key dispatch that precedes a real key and the held-key list are skipped |
| `key <vk>` | post `WM_KEYDOWN` and `WM_KEYUP` to the window; the pump routes them as real keys. Shift, Ctrl and Alt come from the real keyboard |
| `click <target>` | left press `{-1, 2}` then release `{-1, 1}` straight to the target's `MouseEvent` (Win3D slot 19). Target: a widget name, `#<n>` (node n of a tree dump taken now) or `text:<caption>` (first TextWin or TextControl showing that text, ASCII case ignored, visible nodes first) |
| `state <id>` | `G_PendingClientStateId = id`; the frame enters that client state (6 is the main menu) |
| `log <text>` | a marker in `session.jsonl` |
| `quit` | client state 1, the client's own quit path |

`click` bypasses the pick, hover and capture walk. That works for the
message buttons (`TextMsgButton` in the main-menu column, `UITextMsgButton`
in the PAGE screens), which act on the event alone. It does not work for
NeuroNet buttons (`NnButtonControl`), which test the cursor ray on release;
moving the software cursor over a widget needs its screen projection, which
the mode does not compute. Reach those screens through `command` or `key`.
A command that opens a modal dialog with its own message loop
(`NnErrorRunModal`, `NnPromptRunYesNo`, the multiplayer notice) stops the
script until the dialog closes, because `TickClientFrame` does not run
meanwhile.

Every screen change is logged and, 30 frames later (one second of game
time), dumped as tree and screenshot with the label `<kind>-<id>`; a change
within those 30 frames replaces the pending dump. Screen changes are
`EnterClientState` (kind `client`, ids in `client_state.h`), `EnterMenuState`
(kind `menu`, states 1 to 19 of `menu_enter_state.cpp`) and
`ChangeClientLocation` (kind `room`, the location id).

Examples: `menu_walk.txt` (main menu, Load Game, back, Options) and
`single_player.txt` (New Game, space, then the first base).

### Output

One file per kind. Every file is JSON Lines; floats are their bit pattern
in 8 lower-case hex digits, doubles in 16, byte blocks one hex string,
narrow strings byte by byte (bytes from 0x80 as `\u00XX`), wide strings by
UTF-16 unit. `frame` is the number of `TickClientFrame` calls so far; records
written during start-up have frame 0.

| File | Records (`rec`) |
| --- | --- |
| `session.jsonl` | `start` (version, script, out, lines, frame_seconds, seed, settle_frames); `display`; `seed`; `screen` (kind, id); `script` (line, text, ok, note; written after the line ran, so after the records it caused); `waitscreen` (line, result); `dump` (dump, cause `script` or `screen`, label, tree, shot: the file name, `""` when the screenshot failed, null when none was asked); `quit` (why) |
| `tree.jsonl` | per dump a `dump` head (dump, label, client, menu, room: the latest screen ids, -1 or 4294967295 before the first; shot; roots; nodes), then one line per node without `rec`: dump, i (pre-order index, roots in registry order), p (parent index, -1 for a root), d (depth), class (the factory class name or null), rtti (the C++ type name, port-specific), name, flags (Win3D +0x6c, bits 0 to 3; bits 4 to 7 are left from the heap), vis (own and every ancestor's visible bit), layer, sub, hit, pos[3], orient[9]; `ctl` for a WinControl (offset, half, hitoff, tooltip, mesh, hp, nz); `text` and `tw` (scale, w, h) for a TextWin; `text` and `tc` (w, h) for a TextControl |
| `keymap.jsonl` | per phase (`base` after `KeyMap.ini`, `user` after `UserKeyMap.ini`): `load` (result, cmds, groups); `cmd` per KeyCmd in list order (i, nickname, id, ids_name, ids_info, state, keys: [vk, modifiers] pairs); `group` per [KeyMap] (i, nickname, cmds: indices into the cmd list); `active` (the active group nicknames, newest first) |
| `layout.jsonl` | `page` per PAGE file load (path, ok, aspect, entries) and `widget` per entry (i, type, name, rect[4], payload by type); `winini` per `WinControl::LoadIni` that found `[BaseFrame]` (path, nodes) and `winnode` per node of the control's subtree after the load (the tree node fields; `navbar.ini` and the HUD panels come this way); `keylist` (path, group_names) and `keyrow` per row (group, i, primary and alt: [nickname, vk, modifiers], name_ids, info_ids, cmd, kind, editable); `optlist` (path, opened, seed_mode) and `opt` per row of all three sections (section, i, id, info_ids, default, min, max, slider, byte15, header; the lists grow with every load, as in the client); `hud` (path, static_blend, items) and `huditem` per element (i, name, rtti, tooltip) |
| `iclient.jsonl` | one line per call into the client event interface, before it is forwarded: frame, seq (call number from 1), slot (the vtable slot in `rpc_iclient.h`), m (the method; `IChatClient::SendCommonChatMsg` for the chat slot), a (the arguments by name). A non-void slot adds a line after the call with the same seq and `r`, the answer |
| `shot_NNNN_<label>.bmp` | screenshots through `IRPTexture::PrintScreen`, NNNN from 0001: the window's client area, 1024x768, 24-bit (D3DX 8.1 saves no PNG) |

IClient arguments: scalars by value; `const unsigned int&` dereferenced;
byte blocks as hex of `Size` bytes; `TextIds` as an array of `TextCount`;
strings as strings; plain structs as hex of `sizeof(T)`, padding included as
the sender left it (mask it when comparing); structs holding containers or
pointers as objects of their readable fields (`EquipDesc`,
`EquipDescVector`, `DamageList`, the create-info ids, loot items,
reputation and group-feeling relations, mission-objective states); `FmtStr`,
`CCharacterSet` and `CDAPacket` as `"present"` (null for a null pointer).
The proxy is installed where `GetClientInterface` and
`GetChatClientInterface` hand out the client, so single player (rpclocal)
and multiplayer (RemoteServer) both go through it. Writes are serialised, so
calls from another thread are safe; their order is the seq order.

`oracle_iclient_proxy.cpp` is generated. After a change to `rpc_iclient.h`:

```powershell
python CppSource\OutpostFrontier\Client\make_oracle_iclient.py
python CppSource\OutpostFrontier\Client\make_oracle_iclient.py --check
```

The script stops at an argument type it has no recorder for; add an
`OracleArg` overload in `oracle_iclient_args.h` and `.cpp` and list the
type in the script.

### Owner run

Run on 2026-11-17 on the original data, Release build, from a copy of
`bin\` with `data\` as a junction (the run-folder values of section 3).
The figures are from the second run, after the run folder's data was
refreshed (section 6); the first run's menu figures were made without the
menu's background scene and are void.

| Script | Result |
| --- | --- |
| `menu_walk.txt` | Exit code 0 after 398 frames. The background scene enters room `3118686538` at frame 0. Every `waitscreen` matched; 9 dumps (4 caused by screen changes, 5 by the script), each with tree and screenshot; key map phases `base` (122 commands, 5 groups) and `user` (no file, result 0); 176 layout records (the `navbar.ini` window tree with 15 nodes at frame 0, one PAGE file with 15 widgets, 93 key rows, 49 option rows); 6 IClient calls. Two runs with the pointer parked gave byte-identical files, screenshots included; only the output path in the `start` record differs |
| `single_player.txt` | Exit code 0 after 2626 frames. 7 dumps: main menu, the start in space (client state 2 at frame 118), and the first base after the opening scene docks at frame 2175 (client state 3, room `2695128518`). No particle errors in `FLSpew.txt`. The first attempts crashed on docking, because the run folder's data predated decision O17 (section 6). Three runs agree on the session (every screen change and dump at the same frame), the trees, the layouts and the key map. They differ in the IClient log and in the screenshots taken in space (dumps 3, 4 and 6): the `Info` of `SendServerCharSelectVerified` carries a double that differs every run (4.29, 4.41, 4.47 at frame 118); `CommCostume` in `SendServerSendComm` carries bytes the costume does not set (heap contents, in one run the text `_Resource.p2293760__Cube_DIM`); and the server's events in space (targets, cruise, damage, fuses) come a few frames earlier or later and in a different order. The in-process server is not on the oracle's fixed step, so this script's IClient log and space screenshots are not yet comparable byte for byte |

The outputs are made from the game data and stay outside the repository.

### Hooks in existing files

Each is one or two lines marked `Oracle instrumentation (P0.5)`: `main.cpp`
(parse), `engine_init.cpp` (display, seed), `client_timing.cpp` (time step),
`client_frame_tick.cpp` (per-frame tick), `client_state.cpp`,
`menu_enter_state.cpp` and `menu_universe_base.cpp` (screen changes),
`client_exports.cpp` (the proxy), `menu_keymap.cpp` and
`menu_keymap_config.h` (key-map dump and read-only views),
`ui_page_desc.cpp`, `wincontrol_base.cpp`, `nn_preferences.cpp` and
`hud_element_list.cpp` (layout dumps), `win3d.cpp` and `win3d.h`
(`Win3DForEachRegistered`, a read-only registry walk).

## 9. Renderer traces (`RenderTrace`)

The `RenderTrace` project in the solution's `Tools` folder builds
`RenderTrace.dll`, a logging proxy for the Direct3D 8 renderer (work package
P0.5). Its component `RenderTrace` creates the real `DirectX8` component
aggregated under itself, answers the renderer's seven interfaces, forwards
every call unchanged and records it. Nothing changes until the run folder's
`NeuronComponents.ini` names it; to trace, edit that copy, never `outdir\`:

```ini
[Libraries]
DX8RENDERER.dll        ; stays loaded
RenderTrace.dll

[System]
RenderTrace            ; in place of the DirectX8 line

[RenderTrace]
output = C:\traces\menu
frames = 300-302
```

`OUTPOST_RENDER_TRACE_DIR` and `OUTPOST_RENDER_TRACE_FRAMES` override the two
keys. Without both an output folder and a frame list the proxy only
forwards. The output folder receives `trace.jsonl`, one JSON object per call
with floats as 8-hex-digit bit patterns and handles as small stable ids, and
`blobs\`, the buffer, texture and image data named by the FNV-1a 64 hash of
its bytes. Every selected frame starts with a snapshot of the pipeline state
the proxy has seen; resource creation and uploads are written in every frame
up to the last selected one, so a selected frame can be replayed. The format
is version 1 (decision record 0004, section 2): a defaults line after each
`CreateBuffers`, a front-buffer image after each selected frame (the window
must stay unoccluded) and `LightData` lines before each light flush. The
format, the replay
contract and the regeneration step (`make_render_trace.py`, run after any
change to `include\render\IRenderPipeline.h`) are in
`CppSource\RenderTrace\README.md`. A trace of the original data is game
data: keep it outside the repository.

Owner run on 2026-11-17: `menu_walk.txt` under `+oracle` with the proxy in
place of `DirectX8` and `frames = 300-302`, on the refreshed data (section
6). Exit code 0; `trace.jsonl` has 46,145 lines: frames 300, 301 and 302
(3,159, 3,233 and 3,388 records) each start with a 66-line state snapshot,
after the resource creation and uploads of frames 0 to 299; `blobs\` holds
6,600 files (31 MB). The proxy
changes nothing the client oracle sees: the session, key map, IClient and
layout files equal those of a run without it, and so do the trees once the
flag bits of section 6 are masked.

### Replay into DX8RENDERER (`Oracle.exe render-replay`)

Work package P4.3, decision record 0004 point 15:
`Oracle.exe render-replay <vectors folder> <output folder>` plays every
`<name>\trace.jsonl` under the vectors folder into the real renderer and
writes the image of each selected frame to `<output>\<name>\expected.bmp`
(`frame-<N>.bmp` for any later selected frame). The traces it is made for
are the synthetic ones in `crates\neuron-client\tests\vectors\render\`,
written by `crates\neuron-client\tests\fixtures\render\make_render_traces.py`
(`--check` fails when they are stale); their `expected.bmp` files are this
program's output, committed from CI's log (below), never made from Rust.

```powershell
msbuild CppSource\OutpostFrontier.sln /t:Tools\Oracle /p:Configuration=Release /p:Platform=Win32
.\bin\Oracle.exe render-replay "$PWD\crates\neuron-client\tests\vectors\render" "$env:TEMP\render-replay"
```

Building `Tools\Oracle` also builds `DX8RENDERER.dll` and `x86math.dll`
(the renderer's `3DMathEngine`), which the mode loads from `bin\`; it needs
`d3d8.dll` and `ddraw.dll` from the system. For each trace the player
creates `DirectX8` through neuroncomponents (not aggregated) and a window of
the trace's back-buffer size, then plays every line in order through the
seven interfaces: trace ids map to the real handles, light ids to a
`D3DLIGHT8` of the player's (filled by the `AddLight`, `UpdateLight` and
`LightData` lines), unlock data goes into the memory the real lock returned
at the recorded first vertex or index, and the methods record 0004 point 13
answers from the trace (mode enumeration, device information, render
targets, `BlitTexture`, `PrintScreen`) are not called. The defaults line is
compared with the renderer's getters, not applied. The per-method decoding
of the 80 methods `RenderTrace` records generically is generated from
`make_render_trace.py`'s table by `CppSource\Oracle\make_render_replay.py`
(`--check` in CI's reference job); an unknown method stops the trace.
`CopyVertices`, `CopyVertexData`, `LoadTexture`, `LoadCubemap` and a
snapshot selection of a never-returned pooled buffer stop it with a message
too: no synthetic trace uses them yet.

The image is the back buffer, not the front buffer `PrintScreen` copies: a
front-buffer copy of a window is the screen, which on a CI runner may be
covered or off screen. Before forwarding the selected frame's `SwapBuffers`
the player reads the back buffer through the renderer's own `LockBuffer` (a
`CopyRects` into a system-memory surface) and writes it as a 24-bit
bottom-up BMP. `LockBuffer` needs `LOCKABLE_BACKBUFFER`, so the player first
loads an engine INI of its own, `[RenderPipeline]` with only
`LOCKABLE_BACKBUFFER = true`, into neuroncomponents' profile parser; the key
sets no render state, so the defaults are the built-in tables the synthetic
traces' defaults lines hold. With the windowed `COPY` swap effect the
presented image is this back buffer, and no gamma ramp applies to it (record
0004 points 8 and 13 compare before the ramp).

Every call that returns a negative result is printed with its name and
result, beside DX8RENDERER's own `FDUMP` lines. A failure where the trace
recorded success fails the trace (gamma calls, getters and `VerifyState`,
which cannot change the image, only warn); a failed `CreateBuffers` ends the
run with a message that no Direct3D 8 HAL device could be created
(DX8RENDERER only creates `D3DDEVTYPE_HAL`). The exit code is 0 when every
trace played and wrote its image, 1 otherwise, 2 for a usage or set-up
error.

CI's oracle job runs the mode after the other comparisons. A committed
`expected.bmp` must equal the new one byte for byte; for a missing or
different one the log holds the new file's base64 between
`=== expected.bmp <name> base64 begin ===` and `... end ===`, and the
output folder is in the job's `oracle-vectors` artifact. To commit one,
decode the log line (`[IO.File]::WriteAllBytes(path,
[Convert]::FromBase64String(text))`) or take it from the artifact.

## 10. Simulation oracle (`Oracle.exe sim`)

Work package P2.3, decision O23, [adr/0003](adr/0003-simulation-oracle.md):
a headless multiplayer server inside `Oracle.exe` runs a scripted scenario
and records the physics, AI and damage the Rust replay and scenario tests
compare with. The record layouts are in [sim-records.md](sim-records.md).
The output is made from the game data: keep it outside the repository.

### Build and run

The `Oracle` project links `Server.lib` and builds `SimClientStub.dll`
(`CppSource\Oracle\SimClientStub\`) through its references. Both are
needed by this mode and by `Oracle.exe neuron-server` (below): `Server.dll` is delay-loaded (`DelayLoadDLLs`
with `delayimp.lib`), and the harness loads it and looks up its one data
export, `Server`, by name at the start of a run, so the vector suites,
`dump` and `catalog` never load it; `SimClientStub.dll` is loaded with
`LoadLibrary` by the run. Every other import from `Server.dll` must be a
function (a data import fails the link with `LNK1194`). A run also
loads `Content.dll` and the engine DLLs `NEURONCOMPONENTSsrv.ini` names;
build the whole solution, or `Tools\Oracle` with `DLLs\Content`,
`DLLs\ReadFile`, `DLLs\RendComp`, `DLLs\system`, `DLLs\SceneScript`,
`DLLs\DX8RENDERER` and `DLLs\Shading` as CI's data job does. Then, from the
repository root:

```powershell
.\bin\Oracle.exe sim "$PWD\outdir" CppSource\Oracle\scenarios\free_flight.txt "$env:TEMP\sim\free_flight"
.\bin\Oracle.exe sim "$PWD\outdir" CppSource\Oracle\scenarios\free_flight.txt "$env:TEMP\sim\free_flight" --runs 20
```

The data root holds the master's INI files and `data\`, as `outdir\` does.
The output folder is created; files in it are overwritten. Exit code 0 is a
completed run, 2 a usage or scenario error, 3 a start-up failure (the
reason is in `session.jsonl` and on stderr), 4 a fault in a frame (the
`fault` record, below). With `--runs N` (1 to 99) the
program starts itself N times, each run in a fresh process writing to
`<out>\run_NN\`, and writes `spread.jsonl` and `spread.txt` into `<out>`:
the largest difference between any two runs per object, metric and event
(the determinism check of record 0003's envelope method). It exits 1
when a run did not complete.

`--seed S` (decimal or `0x` hex, 32 bits; Gate G2's seed ensemble,
record 0003 point 9, owner 2026-11-19) replaces the seed of the seeding
point (start-up step 5) with `S`; the first `srand`, before the load,
keeps the scenario's `seed`, so the load-time draws stay as they are. The
run records `S` as `reseed` in `session.jsonl`'s `start` and `server`
records and in `load_rand.jsonl`'s `reseed` record. With `--runs N` each
child gets the same `--seed`. The options come after the three paths, in
any order. The data job runs the ensemble of
`crates/game-sim/tests/fixtures/g2_ensemble.txt` this way:

```powershell
.\bin\Oracle.exe sim "$PWD\outdir" CppSource\Oracle\scenarios\dogfight.txt "$env:TEMP\ens\seed_3985226492\dogfight" --seed 3985226492
```

### Start-up

As `OutpostServer.exe` (`OutpostServer.cpp:437`, `:523`, `:731-745`,
`:338-391`), on one thread:

1. `<out>\server.ini` is written from `<data root>\OutpostFrontier.ini` with
   `[Initial MP DLLs]` reduced to Content.dll's `GameSupport`,
   `SpaceSupport` and `BaseSupport` (the populators `SpacePop` and
   `BasePop` and the `AISandbox` and `TestAutomation` controllers would add
   ships of their own) and its `path` set to the folder of `Oracle.exe`;
   every other line is copied.
2. The current directory becomes the data root; `FDUMP` writes to
   `fdump.txt`; `SetIsMPServer(true)`; the x87 and SSE control words are set
   as the server thread sets them (`_control87(0x9001f, 0xfffff)`: 53-bit
   precision, every exception masked); `NeuronRuntime::Startup(NULL,
   "NEURONCOMPONENTSsrv.ini")`; `NeuronRuntime::OpenData` with
   `[OutpostFrontier] data path` made absolute (`GetFullPathNameA`, P2.4):
   the server's field load (`SrvAsteroidSystem::load`) roots the system
   file's path, which `Universe::get_filename` has already rooted, at the
   data path again, and `GetFileSysPath` leaves only absolute paths alone,
   so with the INI's relative `.\data` it opens
   `.\data\.\data\universe\…` and no asteroid field loads (the first
   `asteroids.jsonl` probes showed it). This is the reference's behaviour,
   not a reconstruction error (investigated 2026-11-19): the traced calls
   are `StarSystem::load` passing `Universe::get_filename`'s path (the
   universe directory file system's root, made by `Universe::Startup` from
   `GetFileSysPath("universe\universe.ini", Data)`, joined to the system's
   `file`) and `SrvAsteroidSystem::load` (ordinal 498) passing it through
   `GetFileSysPath` again; `GetFileSysPath` (Common `0xa2df0`) copies only a
   path that starts with `\` or has a drive colon, and the DOS file system
   keeps its root as given (ReadFile, no `_fullpath` on a directory root),
   as `OutpostServer.exe` passes the INI's value to `OpenData`. The other
   server loaders take paths relative to the data root
   (`FSrvAsteroidField::load`'s field files, `pub::BuildBaseReader`'s
   bases) or open the rooted path directly (`StarSystem::load`'s
   `[Object]` pass), so only the field load fails. A dedicated server loads
   its fields only with an absolute data path; the oracle runs that way,
   and the Rust world (`World::load_asteroid_fields`) loads them as such a
   server does, whatever form its data path has (divergence D198, owner
   2026-11-19).
3. `IServerImpl::Startup` with `SStartupInfo` {module path
   `SimClientStub.dll`, INI path `server.ini`, 16 players, port 0}. It runs
   `DataConfig::startup`, the world INI, `Server_LoadInitialDlls` (Content's
   controllers) and loads the stub as its client interface. Its `srand`
   takes the scenario's `seed` instead of `timeGetTime()` (the start-up
   seed, below under "Determinism"), and again after `PlayerDB::init`.
   With `comms off` the harness then sets Content's comm clamp
   (`OracleSimSetGcsClamp`).
4. The load-time rand() recorder starts (`load_rand.jsonl`, below);
   `srand(seed)`; `pub::Controller::Create("SimClientStub.dll",
   "OracleSim", NULL, LOWEST)`, the controller every scenario ship names;
   `OracleSimLoadSystem(CreateID(system))`, `StarSystem::load` as a
   player's arrival would run it.
5. The seeding point (owner, 2026-11-18): `srand(seed)` again, after the
   system and Content's population of it (the `SystemLoaded` handlers) have
   drawn their values. The scenario is compared from here as recorded: the
   Rust world, which has no population yet (P3.6), seeds its generator at
   the same point, and the population's draws are checked at Gate G3.
   With `--seed S` the seed here is `S` (the seed ensemble).
6. The objects are spawned (`pub::SpaceObj::Create`, then
   `pub::AI::SubmitState` with the state graph for `ai`) or looked up
   (solars, `CreateID(nickname)`); a missing object ends the run.
7. The recorders start (the hooks below, `ai.jsonl`), the load-time
   recorder stops and writes its records; every controller's heartbeat
   interval and phase is written (`heartbeats`); tick 0 is recorded.
8. Every frame *t* = 1..`ticks`: the control words are read (recorded) and
   set again; the fixed clock moves to `clock + t × frame`; the commands of
   tick *t* run; then the server thread's frame: `Server.Update()`,
   `Timing::UpdateGlobalTime(frame)`, `Server.ElapseTime((float)frame)`;
   then the per-tick records.

The server is not shut down: the run closes its files and ends the process.

### Hooks in existing files

Each is a few lines marked `Oracle instrumentation (P2.3 sim)`, inert while
`GOracleSimHooks` (`include\oracle_sim_hooks.h`, `Common\oracle_sim_hooks.cpp`)
is zero, which only `Oracle.exe sim` changes:

| File | Hook |
| --- | --- |
| `Server\IServerImpl.cpp` | `Server_StepSystems` steps `StepSystemId` and no other system; `StarSystem_Update` runs the multiplayer physics and collision pass for that system although it has no player; `IServerImpl::ElapseTime` takes the frame time as its real dt (`OracleSimFixElapsed`); `OracleSimLoadSystem`, a new export; `IServerImpl::Startup`'s `srand` takes `StartupSeed` instead of `timeGetTime()` under the fixed clock, and seeds again after `PlayerDB::init` (2026-11-19); `OracleSimControllerHeartbeats`, a new export (each controller's heartbeat interval and phase); `OracleSimAddPlayer`, a new export (P3.3, a scenario's `player`: the session's slot takes the system and the ship, then `StarSystem::add_player`; the client id stays 0) |
| `Server\server_gameobject.cpp` | the three timer stamps (`EqObj::update` repair, `apply_damage_list`, `apply_equip_entry`) read `OracleSimTimingRead()`, which is `Timing::read()` unless the clock is fixed; `GameObject::on_collision` reports `CalcCollisionDamage`; `Ship::set_throttle`, `set_axis_throttle`, `set_nudge_vec`, `set_strafe_dir` and `EqObj::slot_34` report the `IObjAffect` call |
| `Server\server_classes.cpp` | `StarSystem::munition_collision` reports the impact and its damage list; `StarSystem_MpCollisionFilter` takes its single-player branch when asked (`collisions sp`) |
| `Common\common_physicalsystem.cpp` | `PhysicalSystem::Update` takes the single-player branch when asked |
| `Common\common_behaviormanager.cpp` | `UpdateBehaviorTick` reports its entry and every exit (an RAII guard) |
| `Common\common_ceqobj.cpp` | `CEqObj::activate` reports each answer (`OnDockActivate`); `update_docking_animations` reports each point with a channel before processing it (`OnDockAnim`), P2.4 |
| `IVP\ivp_physics\ivp_environment.cpp` | `IVP_Environment_Simulate_Psi` reports before the simulation units and after the next-PSI matrices |
| `IVP\ivp_physics\ivp_simulation_unit.cpp` | `IVP_Simulation_Unit_Process_One_Psi_Step` reports its entry and its end with the movement check |
| `IVP\ivp_physics\ivp_core.cpp` | `IVP_Core::push_core_linear`, `push_core_angular_ws` and `push_core_angular_cs` report the push |
| `Common\common_iobjinspectimpl.cpp` | `get_dock_hardpoints` and `get_lane_start` report their answers (`GOracleAiHooks.OnAnswer`, P2.5 dock) |
| `Common\common_universe_system.cpp` | `SYSTEM::GetSphereProperties` reports its answer (the AI's zone probes, P2.5) |
| `Server\server_gameobject.cpp` (P2.5) | `Ship::slot_5c`, `slot_60`, `go_tradelane`, `stop_tradelane` and `EqObj::slot_44` report the `IObjAffect` call; `Ship::request_returned`, `go_tradelane` and `slot_44` report their answers; `request_returned` runs the pending callback at affect depth 0 |
| `Content\PilotPersonalities.cpp` | `OracleSimPilotPersonality`, a new export (P2.5) that only the `pilot` option calls: `GetPilotPersonality` of a pilot nickname |
| `Content\Content.cpp` | `OracleSimSetGcsClamp`, a new export (2026-11-19) that only the `comms off` option calls: `ContentSetGcsClampEnabled`, the clamp a mission's `Act_GcsClamp` sets |

`srand`, the frame time and the control words need no hook: the harness
runs the frame loop itself.

### Stepping modes

The server always runs in multiplayer mode (physics, collisions and NPC AI
run on the server only there). `stepping mp` keeps the dedicated server's
path: `StarSystem_Update` splits the frame into steps of at most 0.2 s and
each step calls `PhysicalSystem::Update`, which accumulates and simulates
in 0.0999 s chunks plus the remainder. `stepping sp` keeps everything else
and switches only `PhysicalSystem::Update` to its single-player branch: one
`IVP_Environment_SimulateDTime` per call with the step clamped to 0.0999 s,
which is how the client steps physics once per frame in single player
(`space_state.cpp:1054`). The differences from a real single-player frame
are recorded, not removed: the step runs inside the server's system update
instead of the client's frame, collisions go through the server's
multiplayer collision pass, and every other `SinglePlayer()` test sees
multiplayer. With a frame time of at most 0.2 s each frame is one step.

### Collisions

The multiplayer server's collision filter (`StarSystem_MpCollisionFilter`,
`Server/server_classes.cpp:11409`) lets a pair collide only when one side
is a `CBeam` (class 0x67) or a `CGuided` (0xa7), or has no client object;
the clients report every other contact. Neither munition gives an
end-to-end check of collision damage: a bullet has no object id, so the
other side's `GameObject::on_collision` returns before
`CalcCollisionDamage`, and `Bullet::on_collision` goes to
`munition_collision`; a missile's side is `Guided::on_collision`
(`munition_collision` too), and its flight, motor, seeker and proximity
fuse are the server's `Guided` controller (ported into P2.4's remainder,
owner 2026-11-18; its fuse timing is D177). No
ship, solar or asteroid target collides there. `collisions sp` therefore
makes the filter take its first branch (`if (SinglePlayer()) return
TRUE`) while the rest of the server stays in multiplayer mode: every pair
collides and each contact goes through the multiplayer collision pass
(`StarSystem_ProcessCollisions`: both sides' `on_collision`,
`CalcCollisionDamage`, `collision_damage`). As with `stepping sp`, the
difference from a single-player game is recorded, not removed: in single
player the contacts are found by the client's physics and reported to the
server. `ram_ship` uses it; the data job checks that every run of a
scenario marked `# ci: expect collision` records collision events with
their `CalcCollisionDamage` calls and that the runs record the same ones.

### Scenario format

A text file; `#` starts a comment, blank lines are skipped, tokens are
separated by spaces, tabs or commas. Examples (one per kind of check of
record 0003) are in `CppSource\Oracle\scenarios\`: `free_flight`,
`turn_to_heading`, `fly_to_point`, `dock_at_station`, `fire_at_target`,
`ram_asteroid`, `dogfight`, `skirmish`, `strafe`, and `ram_ship`, Gate G2's collide scenario in
place of `ram_asteroid` (owner, 2026-11-18); the escort scenarios of P2.5,
`formation`, `follow`, `guide` and `large_ship`; and the dock scenarios of P2.5,
`dock_trade_lane`, `launch_from_station` and `trade_lane`. A line
`# rust: skip <reason>` tells `scenario_replay` that the Rust world cannot
run the scenario yet; a line `# rust: server <reason>` that the scenario
needs the server's answers, so only `game-logic`'s `scenario_hooks` runs
it, with Content's SpaceSupport and the AI's requests to the server (P3.3,
the trade lanes).

| Line | Meaning |
| --- | --- |
| `system <nickname>` | the star system (required) |
| `ticks <n>` | the number of frames (required) |
| `frame <seconds>` | frame time; default 1/30, the server thread's period |
| `stepping sp\|mp` | default `mp` |
| `collisions sp\|mp` | default `mp`; `sp` makes the server's collision filter take its single-player branch (below) |
| `comms on\|off` | default `on`; `off` sets Content's comm clamp before the load, so `IsGcsCommAllowed` refuses every comm before its `rand()` roll (owner, 2026-11-19; record 0003: the G2 fights run with comms off) |
| `seed <n>` | `srand` after start-up; default `0x4f524143`; also the start-up seed |
| `clock <seconds>` | the fixed clock at tick 0; default 1000 |
| `ship <name> <archetype> <loadout\|-> <x> <y> <z> [options]` | spawn a ship; options: `heading <degrees>` (about +Y; 0 faces −Z, 90 faces −X), `orient <9 numbers>` (row-major), `vel <x y z>`, `angvel <x y z>`, `ai <state graph>` (e.g. `FIGHTER`; without it the ship has no behaviour and keeps the commands it is given), `escort` (with `ai`: the graph's escort variant, `get_state_graph(name, ESCORT)`, as Content's `ResetFormationState` gives a wingman; without it the leader variant), `pilot <nickname>` (with `ai`: the personality of a `[Pilot]` of `missions\pilots_population.ini` or `pilots_story.ini`, as Content gives an NPC ship of an `npcships.ini` entry, without a job; without it the default personality), `faction <group>` (a reputation affiliated with a group of `initialworld.ini`, e.g. `he_p_grp`; without it none), `hp <n>` (default −1, the archetype's), `psi` (record its PSIs) |
| `solar <name> <nickname> [psi]` | a system object of the loaded system |
| `at <ticks> <name> <command> [arguments]` | `<ticks>` is `t`, `a..b` or `a..b/step` (ticks from 1) |
| `player <session> <ship>` | (P3.3, decision record 0007 point 11) after the objects are spawned, session 1 to 16 joins the system with the ship, named before the line, as its own (`OracleSimAddPlayer`): its observer follows the ship and its packets are tick 0's; no login, no client id, so no other session or controller hears of it |
| `dump replication` | (P3.3) SimClientStub also writes the packets of the replication comparison each client receives to `replication.jsonl`, from the players' join on (`SimClientStubRecordReplication`) |

Commands. The first five call the ship's `IObjAffect` as the AI does; the
directives go through `pub::AI::SubmitDirective`, each after a
`DirectiveCancelOp` (a current directive blocks a new one), and need a
ship with `ai`:

| Command | Call |
| --- | --- |
| `throttle <t>` | `set_throttle` |
| `axis <x> <y> <z>` | `set_axis_throttle` |
| `nudge <x> <y> <z>` | `set_nudge_vec` |
| `strafe none\|left\|right\|up\|down` | `set_strafe_dir` |
| `fire [<target>]` | `slot_34` with every unguided `CEGun`, aimed at the target's position or 1000 m ahead |
| `target <name>\|none` | `set_target` |
| `goto <x> <y> <z> [range <r>] [cruise]` | `DirectiveGotoOp` to a point (`Field_0C` 1), arrival range default 100, no cruise unless asked |
| `face <x> <y> <z>` | `DirectiveFaceOp` mode 2 (a point) |
| `dock <target>` | `DirectiveDockOp` |
| `launch <target> <index> <terminal>` | `DirectiveLaunchOp` {the base, the docking sphere, its terminal type}, as Content's `LaunchQueuedShips` gives a ship it places at a dock (P2.5) |
| `tradelane <entrance> <exit>\|none [speed]` | `DirectiveInstantTradelaneOp` {the entrance ring, the exit ring (0 for none), the speed as a float in the third word, default 0} (P2.5) |
| `ram <target>` | `DirectiveRamOp` |
| `follow <target> [distance <d>] [offset <x> <y> <z>] [range <r>]` | `DirectiveFollowOp` (P2.5 escort): the target, the follow distance (default 150), the offset in the target's frame (default 0), the range (`Field_20`, default 400) |
| `guide <ship> <x> <y> <z> [range <r>] [distance <d>] [offset <x> <y> <z>]` | `DirectiveGuideOp` (P2.5 escort): its goto part as `goto`'s (a point, arrival range default 100, no cruise) with the guided ship in `GotoParams.Field_5C`; its follow part on the guided ship (distance default 150, offset default 0); the other fields keep their defaults |
| `idle`, `cancel` | `DirectiveIdleOp`, `DirectiveCancelOp` |
| `dock_request <target>` | `pub::SpaceObj::DockRequest` |
| `formation <leader>\|none [<x> <y> <z>]` | `pub::AI::update_formation_state(ship, leader, offset)` (P2.5 escort; offset default 0): as `ResetFormationState` makes a formation, the leader first with itself as leader, then each wingman with its slot |

Ships are created as Content creates NPC ships, without a costume or
voice. A ship without `faction` has no reputation (`VibeGroupId` −1); with
it, the harness allocates one as Content does for an NPC
(`pub::Reputation::Alloc` with empty names, `SetAffiliation` to the group
`GetReputationGroup` names), so its attitude towards another ship is the
groups' feeling and the target manager treats a ship it feels −0.6 or less
towards as hostile. A ship has the default personality unless `pilot`
names one, which `Oracle.exe sim` reads through Content.dll's
`OracleSimPilotPersonality` export and submits with the state graph
(`bSetPersonality` 1).

`dogfight` gives two AI fighters no command at all: the target manager
picks the hostile ship and the state graph the combat behaviours, as when
two hostile NPCs meet. There is no attack directive in the game; the
`target` command (`set_target`) is not used there. `skirmish` is the same
meeting with the rogue at 400 hull points: the police kills it at tick 880,
after the rogue's trail and flee, which the dogfight does not reach, and
the run goes on to tick 1500. (Until the fault recorder showed it, the run
stopped at tick 881: the access violation at tick 882 was the behaviour-tick
recorder's own, reading the previous tick's target list, which still named
the destroyed rogue; the game empties that list before it reads it.)

`strafe` records the strafe behaviour (13), which neither fight reaches.
Its poll (`StrafeBehavior::Slot5`, `common_strafebehavior.cpp`) offers it
only when the own ship is a fighter, freighter or gunboat (type
`& 0x70000`), the target is none of those and moves at most 20 m/s; only
then does the state graph's weight count. So the dogfight's police fighter
meets a hostile transport (`ge_transport`, `fc_lr_grp`) with no behaviour,
which stays where it was spawned, and strafes it for the whole run. The run
stops at tick 1500, before the police destroys the transport at tick 1536.
A gunboat (`GUNBOAT` graph) against the same transport and the fighter
against a `li_cruiser` strafe as well (1799 and 1775 of 1800 ticks in a
trial run).

The dock scenarios (P2.5). In `dock_at_station` the fighter's dock
behaviour (7) flies the approach through the goto behaviour to within
1000 m of `he01_11` (ticks 1 to 349), asks for a port at tick 354
(`slot_5c` type 0) and is refused (response 5, port -1: the ship has no
reputation), so the directive ends with result 2 and the ship idles; no
docking point is reached. `dock_trade_lane` has the fighter dock with
trade lane ring 16 as NPC traffic enters a lane: the lane start
(`get_lane_start`), the approach, the lane request at tick 222 (type 2,
granted at once), the ring's spline, the lane at tick 588 and the lane's
end at tick 1188. `launch_from_station` launches the fighter from the
station's first berth (behaviour 8, ticks 1 to 277). `trade_lane` puts the
fighter into the lane from ring 16 to ring 18 with the instant trade lane
(behaviour 9, `go_tradelane` at tick 12). The lane scenarios record no
PSIs: in the lane the ship moves by `CShip`'s `LaneAction`. Since P3.3's
trade lanes both run through the server's hooks (`# rust: server`): the
lane request is SpaceSupport's to answer, and the server's
`notify_stop_tradelane` ends the instant trade lane's directive.

The escort scenarios (P2.5) reach the behaviours no fight does.
`formation` is a leader and two wingmen of `formations.ini`'s
`fighter_basic` slots (the police pilot, no faction): the wingmen get the
FIGHTER ESCORT graph and `update_formation_state` makes the formation, the
leader flies a goto and the escort graph picks the formation behaviour
(10). In `follow` a fighter follows another that flies a goto
(`DirectiveFollowOp` with a non-zero offset, so the leader's
`generate_follow_offset` is not asked). In `guide` a fighter guides a
ship without behaviour to a point (`DirectiveGuideOp`): it follows the
ship, and as the ship makes no progress it leads it through the goto
behaviour every other sampling period. `large_ship` is a police cruiser
(the loadout, graph and pilot of `npcships.ini`'s cruiser entries) that
flies a goto and then, with a hostile fighter ahead, the large-ship
behaviour (11).

### Faults

`oracle_sim_fault.cpp` runs each frame (the tick's commands, the server
frame and the per-tick records) inside `__try`. A structured exception
there is written to `session.jsonl` as a `fault` record (sim-records.md):
the code, the faulting address as module + offset, the registers with the
memory they point at, and a stack walked with `StackWalk64` and resolved
with dbghelp from the PDB files beside `Oracle.exe`. The run then ends
with exit code 4. From the first frame on, a vectored handler also records
the first eight first-chance hardware exceptions (access violations and
the like) the same way, `first_chance` true, before any handler in the
game code sees them; a fault the game catches itself shows only there.
The data job's step log prints the `fault` records with the errors and
notes.

### Outputs

`session.jsonl`, `tick.jsonl`, `events.jsonl`, `psi.jsonl`,
`calls.jsonl`, since P2.5 the behaviour-tick snapshots `ai.jsonl`, and the
load-time draws `load_rand.jsonl` with its summary `load_rand.txt`, and
the simulation units of the first PSIs, `units.jsonl`, since P2.4's
remainder the asteroid cubes `asteroids.jsonl` and the docking points
`dock.jsonl` (`oracle_sim_fields.cpp`; sim-records.md); the stub's `iclient.jsonl` and `controller.jsonl`,
and with `dump replication` its `replication.jsonl` (P3.3: `CppSource\Oracle\replication.txt`,
which the data job runs twice into `outpost-replication\replication\run_NN`);
`server.ini`, `fdump.txt`, `metrics.txt`. The rand() recorder
(`oracle_sim_rand.cpp`) replaces the `rand` import of every module bound
to the universal CRT's `rand` (`Common.dll`, `Server.dll`, `Content.dll`
and any engine DLL that draws) with a wrapper that calls the CRT's `rand`
and counts, so the random sequence of a run is unchanged. From the
harness's `srand` to the first frame it keeps every draw with its phase,
caller and stack; it starts the symbol handler (dbghelp, the PDB files
beside `Oracle.exe` in `bin\`) for the stacks and names. The AI recorder
keeps the draws of each behaviour tick through it.

### Determinism

Fixed by the harness: the frame time, the clock of the three timer stamps
and of `ElapseTime`'s real dt, `Timing`'s global time, the `rand` seed (one
thread), the start-up seed (below), the control words before every frame
(IVP's point-soup builder sets 64-bit precision and never restores it; the
`frame` record shows what it found). Not fixed, as record 0003 says: the
pointer-keyed physics sync set and IVP's pointer-XOR pair hash, so runs can
differ; that is the spread `--runs` measures. Not audited: the other
`timeGetTime` and `GetTickCount` reads in Content.dll and the engine DLLs.

The start-up seed (owner, 2026-11-19; record 0003 point 11).
`IServerImpl::Startup` seeds the generator with `timeGetTime()` before
Content's controllers start, and each controller's
`pub::Controller::SetHeartbeatInterval` draws its heartbeat's starting
phase from it (`Server_ControllerSetHeartbeat`: `rand() / 32768 ×
interval`). GameSupport's heartbeat (1 s) is Content's game time
(`GetContentGameTime`, `GameSupport.cpp`), which moves in whole heartbeats,
so the phase decides at which tick a game-time test passes. Content's comm
system tests one on every damage message: `IsGcsCommAllowed` refuses a
comm within its category's interval (default 12 s) of the last one nearby
and otherwise rolls `rand()` (`Gcs.cpp`). In two data jobs and three probes
(runs 44494560931, 44500705507, 44523076483, 44526581221, 44530545092)
that roll came a few ticks earlier or later in separate processes of one
build, on any machine, CPU or toolset, with or without ASLR; every later
`rand()` of the AI moved with it, and 5 of 120 seed-ensemble runs ended
differently after their first contact (never before it). The harness now
gives `Startup`'s `srand` the scenario's `seed`
(`GOracleSimHooks.StartupSeed`) and seeds again after `PlayerDB::init`,
whose first start on a machine (no `accounts.cfg` yet) seeds from the
clock once more and draws six values. `session.jsonl` records the seed
(`server`, `startup_seed`) and each controller's heartbeat (`heartbeats`).
The G2 fights also run with `comms off` (record 0003 point 12). The data
job's step "Check that the oracle repeats itself" runs the five seeds that
split three times each and fails when their records differ (`run_rand.jsonl`
by each draw's tick, index, value and caller: dbghelp's walk of the frames
beyond the caller differed once between identical runs), when a
comms-off run draws a comm roll, or when a fight with comms on differs from
comms off before its first contact.

The CI builds whose output is compared use the MSVC toolset pinned in
`.github/msvc-toolset.txt` (`.github/scripts/msvc-pin.ps1`): build.yml's
oracle job (`Tools\Oracle`, compared with the committed vectors, and the
resource DLLs, compared with the text catalogs) and data.yml's oracle. The
windows-2025-vs2026 images change their VS 2026 toolset without notice
(14.51.36231 and 14.52.37025 were on different images on 2026-11-19); the
pin is v143 14.44.35207, which every image in use has. None of these builds
links MFC (v143's MFC is not on the images). The plain reference build
(the whole solution, OutpostServer's MFC included) uses the image's default
toolset and logs its version and the compiler's banner. The `env` record's
`msc_full_ver` shows what a run was built with.

The load-time draws (`load_rand.jsonl`; first recorded 2026-11-18, the same
in every run): `StarSystem::load` draws nothing itself; its `SystemLoaded`
message reaches Content's `FSpaceSupport`, whose `FSystemInfo::LoadFull`
draws them all, 4,837 in he01 and 2,390 in ve03. In he01: 1,082 in
`FZonePopulation::Validate` (`FEncounterParameters::Refresh`,
`EncounterParams.cpp:720`, and `PickFaction` through Common's
`weighted_vector::Pick`), 78 in `RebuildTradeLanes`
(`CollectSpacePopZones`, `Populators.cpp:707`), then 3,677 in Common's AI
for the 112 solar NPCs that `FSolarNpc::SubmitPersonalityState` gives a
state graph through `pub::AI::SubmitState`: the `IBehaviorManager`
constructor (the behaviour timers, `PathfindManager`'s phases,
`InitSystemManager`'s scan timer and gun-group slot timing; 2,196) and
`set_personality` (`SystemManagerGunGroupManager::RefreshFromPersonality`;
1,481). The encounter, trade-lane and solar-NPC code is Content's (P3.6)
and `SubmitState` the server's (P3.3); the Rust world makes none of these
draws yet (`scenario_replay` reports both counts). Before the harness
seeded again after the load (start-up step 5), a scenario ship with `ai`
drew its first value at index 4,837 in he01 and the Rust world's at 0;
now both draw it at index 0 of the sequence of the seed (21 draws of the
same constructor while it is spawned).

### Not covered yet

Single-player stepping is emulated as above, not run through the client.
Field asteroids (`CmnAsteroid`) are not scenario objects; `ram_asteroid`
uses a static asteroid solar. The server fills the fields around every
ship of the system (`CmnAsteroid::Update` walks the whole ship list, so
no player is needed): in `ram_asteroid` the fighter starts in the
Keswick ice field, and its cubes and rocks are recorded
(`asteroids.jsonl`). The multiplayer server's collision filter
(`StarSystem_MpCollisionFilter`, `Server/server_classes.cpp:11409`) lets a
pair collide only when one side is a bullet or a missile, so ship–solar and
ship–ship contact is not simulated on the server (the clients report it):
in `ram_asteroid` the ship flies through the rock and no collision is
recorded. `collisions sp` (above) emulates the single-player branch
instead; missiles against ships, the only collision of the multiplayer
path with collision damage, wait for the server's `Guided` controller
(P3.3). Ships without `faction` have no reputation,
so their AI targeting is neutral. There is no world snapshot for behaviours that read other objects
(record 0003 point 7, P2.5).


### The controller registry suite (`Oracle.exe neuron-server`)

Added by P3.1 (decision record 0006, point 10). `Oracle.exe neuron-server
<fixture directory> <output directory>` drives `Server.dll`'s controller
registry without starting a server: the harness (`oracle_bus.cpp`) loads
`SimClientStub.dll`, sets its script callback (`SimClientStubSetScript`) and
reads `crates\neuron-server\tests\fixtures\oracle\controller_bus.list`.
Each `create` goes through `pub::Controller::Create` with the instance
name `Scripted:<name>`, which the stub makes into a scripted controller
whose constructor, messages and destructor come back to the harness; the
other actions use `pub::Controller::Destroy`, `SetHeartbeatInterval`,
`_SendMessage` and `pub::SetTimer`. Two exports added to `Server.dll` for
it, `OracleBusTick` (the heartbeat walk, then timer firing, as `ElapseTime`
runs them) and `OracleBusShutdown` (`Server_ControllerCleanup`), drive the
rest. It writes `controller_bus.jsonl`, which CI's oracle job compares with
`crates\neuron-server\tests\vectors\`. It needs no game data.
