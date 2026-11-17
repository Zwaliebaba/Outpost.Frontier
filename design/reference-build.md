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

