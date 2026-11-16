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
| Original installation | Its `DATA` and `EXE` folders (section 3) |

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

All paths are relative to the repository root. Every folder below is
ignored by git.

```text
dx8\          optional: a local DirectX 8.1 SDK that overrides CppSource\DX8\
DATA\         the installation's DATA folder, unchanged
EXE\          the installation's EXE folder, unchanged
bin\          Release build output, plus the runtime files listed below
DATA_EXTRA\   optional: shaders and settings for the volumetric nebula
```

The client switches to its own folder and expects the game data in `data\`
there, beside the executable (the default `data path` is `.\data`; the
original looked in `..\DATA`). It probes
`data\CHARACTERS\ANIMATIONS\FACIALMALE.ANM`; when that file is missing it
starts `OutpostServer.exe` instead and exits. Run the binaries from a folder
that holds `data\` (a copy of `DATA\`, or a junction to it), the INI files and
`EBUEula.dll`. `DATA_EXTRA\` then also sits beside the executable.

Copy these files from `EXE\` to `bin\`, where present:

| Files | Why |
| --- | --- |
| `*.ini` | Engine, client, server and resource configuration |
| `NewPlayer.fl`, `NewCharacter.fl`, `MPNewCharacter.fl` | Templates for new single-player games and characters |
| `flconfigdatabase.txt` | Read by the Direct3D 8 renderer |
| `EBUEula.dll` | The client exits with a fatal error without it. It is an original binary that was not reconstructed |

Do not copy the original game DLLs. The build produces its own.

The INI files name DLLs. In the copies in `bin\`, every DLL name must be one
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
3. Single player: start a new game and fly. This is the Gate G0 check.
4. Dedicated server: start `bin\OutpostServer.exe`; connect a client to
   `127.0.0.1`.

Per-user files go to `Documents\My Games\OutpostFrontier`, the log to
`%LOCALAPPDATA%\OutpostFrontier\FLSpew.txt`. The client also writes registry
values under `HKCU\Software\Microsoft\Microsoft Games\OutpostFrontier\1.0`.

## 6. When it does not work

Record every problem and its fix in this section, with the commit that
fixed it. The only changes allowed in `CppSource/` are build fixes and the
oracle instrumentation of work package P0.4.

| Symptom | Cause | Fix |
| --- | --- | --- |
| `MSB8041` (MFC libraries are required) when building `OutpostServer` with toolset v143 | The Visual Studio 2026 installation has MFC only for v145 | The solution uses toolset v145 (commit `a86a199`) |
| `LNK1181` (cannot open input file) for `Common.lib`, `Server.lib`, `rpclocal.lib` or `NeuronRuntime.lib` in a parallel build (`/m`) | `Server`, `RemoteServer` and `RemoteClient` linked these libraries without project references, so MSBuild could build them too late | Project references added (commit `a86a199`) |

## 7. Then

Fill in [facts.md](facts.md) (work package P0.3). Gate G0 also needs the
oracle harness of work package P0.4.

The harness is the `Oracle` project in the solution's `Tools` folder. Its
synthetic suites need no game data. To run them by hand after a Release
build:

```powershell
New-Item -ItemType Directory -Force oracle-out
.\bin\Oracle.exe "$PWD\crates\neuron-core\tests\fixtures\oracle" "$PWD\oracle-out"
git diff --no-index crates\neuron-core\tests\vectors oracle-out
```

### Data dump

The data dump runs both INI readers over the installation and compares the
Rust port with the C++ (work package P1.2, Gate G0). It reads every `.ini`
file under `DATA\` and `EXE\`. It writes one dump per file, about the size
of the INI files themselves or larger, into a folder outside the
repository. None of it may be committed. From the repository root, after a
Release build:

```powershell
.\bin\Oracle.exe dump "$PWD" "$env:TEMP\outpost-oracle"
```

```powershell
$env:OUTPOST_DATA_DIR = "$PWD"; $env:OUTPOST_ORACLE_DIR = "$env:TEMP\outpost-oracle"
cargo test --release -p neuron-core --test data_dump -- --nocapture
```

The test prints a summary and writes it to `rust-result.txt` in the oracle
folder: how many files are text and BINI, which could not be opened, and
the first differences, if any. Attach that file to the pull request that
closes P1.2. CI runs the same pipeline on a small synthetic tree, so a
failure here comes from the data.

To read values for [facts.md](facts.md), including from BINI files,
`cargo xtask ini` lists files or folders through the same reader:

```powershell
cargo xtask ini EXE --grep Libraries
```

