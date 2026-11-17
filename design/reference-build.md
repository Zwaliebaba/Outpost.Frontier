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
SceneRuntime  SceneScript  Server  Shading  SoundManager  SoundStreamer
UiText  imeui  luaprofile  movie  neuroncomponents  particlefx  rpclocal
remoteserver  serverinterface  surfacematerials  system  x86math  ximage
zlib
```

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
| With `bDrawScripts = false` in `intro.ini`, the main menu is not drawn and has no mouse cursor | `MenuStateDriverUpdate` returned before it began the frame when `bDrawScripts` is clear, and the cursor was set only on the path that enters the intro base (`client_state.cpp`) | The menu frame is always drawn and only the intro scene work depends on `bDrawScripts`; the cursor is set before that test (commit `816f556a`). With `bDrawScripts = true` the menu behaves as before |

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
`user` phase of the key-map dump carries local bindings.

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
`single_player.txt` (New Game to the first base).

### Output

One file per kind. Every file is JSON Lines; floats are their bit pattern
in 8 lower-case hex digits, doubles in 16, byte blocks one hex string,
narrow strings byte by byte (bytes from 0x80 as `\u00XX`), wide strings by
UTF-16 unit. `frame` is the number of `TickClientFrame` calls so far; records
written during start-up have frame 0.

| File | Records (`rec`) |
| --- | --- |
| `session.jsonl` | `start` (version, script, out, lines, frame_seconds, seed, settle_frames); `display`; `seed`; `screen` (kind, id); `script` (line, text, ok, note; written after the line ran, so after the records it caused); `waitscreen` (line, result); `dump` (dump, cause `script` or `screen`, label, tree, shot: the file name, `""` when the screenshot failed, null when none was asked); `quit` (why) |
| `tree.jsonl` | per dump a `dump` head (dump, label, client, menu, room: the latest screen ids, -1 or 4294967295 before the first; shot; roots; nodes), then one line per node without `rec`: dump, i (pre-order index, roots in registry order), p (parent index, -1 for a root), d (depth), class (the factory class name or null), rtti (the C++ type name, port-specific), name, flags (Win3D +0x6c), vis (own and every ancestor's visible bit), layer, sub, hit, pos[3], orient[9]; `ctl` for a WinControl (offset, half, hitoff, tooltip, mesh, hp, nz); `text` and `tw` (scale, w, h) for a TextWin; `text` and `tc` (w, h) for a TextControl |
| `keymap.jsonl` | per phase (`base` after `KeyMap.ini`, `user` after `UserKeyMap.ini`): `load` (result, cmds, groups); `cmd` per KeyCmd in list order (i, nickname, id, ids_name, ids_info, state, keys: [vk, modifiers] pairs); `group` per [KeyMap] (i, nickname, cmds: indices into the cmd list); `active` (the active group nicknames, newest first) |
| `layout.jsonl` | `page` per PAGE file load (path, ok, aspect, entries) and `widget` per entry (i, type, name, rect[4], payload by type); `winini` per `WinControl::LoadIni` that found `[BaseFrame]` (path, nodes) and `winnode` per node of the control's subtree after the load (the tree node fields; `navbar.ini` and the HUD panels come this way); `keylist` (path, group_names) and `keyrow` per row (group, i, primary and alt: [nickname, vk, modifiers], name_ids, info_ids, cmd, kind, editable); `optlist` (path, opened, seed_mode) and `opt` per row of all three sections (section, i, id, info_ids, default, min, max, slider, byte15, header; the lists grow with every load, as in the client); `hud` (path, static_blend, items) and `huditem` per element (i, name, rtti, tooltip) |
| `iclient.jsonl` | one line per call into the client event interface, before it is forwarded: frame, seq (call number from 1), slot (the vtable slot in `rpc_iclient.h`), m (the method; `IChatClient::SendCommonChatMsg` for the chat slot), a (the arguments by name). A non-void slot adds a line after the call with the same seq and `r`, the answer |
| `shot_NNNN_<label>.png` | screenshots through `IRPTexture::PrintScreen`, NNNN from 0001 |

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

