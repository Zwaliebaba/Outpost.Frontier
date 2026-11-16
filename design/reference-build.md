# C++ reference build

How to build and run the C++ code base in `CppSource/` as the reference for
the migration (work package P0.2 in the [migration plan](migration-plan.md)).
This needs a Windows machine, the DirectX 8.1 SDK and an original
installation of the game. None of these can be committed.

## 1. Requirements

| Item | Detail |
| --- | --- |
| Windows | 10 or 11, x64 |
| Visual Studio | 2022 or later, workload *Desktop development with C++*, with the individual components *MSVC v143 build tools (x86 and x64)*, *C++ MFC for v143 build tools (x86 and x64)* and a Windows 10 or 11 SDK. The solution sets toolset `v143` and builds `Win32` only |
| Python 3 | On `PATH`. Needed only by the Debug and DebugASan configurations |
| DirectX 8.1 SDK | Headers and libraries in `dx8\` (section 2) |
| DirectPlay | Windows optional feature (section 5) |
| Original installation | Its `DATA` and `EXE` folders (section 3) |

The current Windows SDK already provides DirectSound, DirectInput,
DirectDraw, DirectShow and `dxguid.lib`. The DirectX 8.1 SDK is needed only
for Direct3D 8, D3DX 8 and DirectPlay 8.

## 2. DirectX 8.1 SDK

Microsoft no longer distributes the SDK. The Internet Archive holds the
signed original as item
[`dx81sdk_full`](https://archive.org/details/dx81sdk_full)
(`dx81sdk_full.exe`, about 166 MB, dated 2001-11-03).

1. Download `dx81sdk_full.exe`.
2. Check the signature:

   ```powershell
   Get-AuthenticodeSignature .\dx81sdk_full.exe | Format-List Status, SignerCertificate
   ```

   The signer must be Microsoft. A certificate from 2001 can report as
   expired; `HashMismatch` or `NotSigned` means the file is not the
   original.
3. Extract it rather than install it. Try 7-Zip on the `.exe` first. If it
   cannot open the file, run the installer and take the files from the
   folder it installs to.
4. Copy the SDK's `include` and `lib` folders to `dx8\include` and
   `dx8\lib` at the repository root. `dx8\` is ignored by git.
5. Check that the needed files are present. The command prints nothing when
   they are all there:

   ```powershell
   'include\d3d8.h', 'include\d3d8types.h', 'include\d3d8caps.h',
   'include\d3dx8tex.h', 'include\dplay8.h', 'include\dpaddr.h',
   'lib\d3d8.lib', 'lib\d3dx8.lib' |
     Where-Object { -not (Test-Path (Join-Path dx8 $_)) }
   ```

Do not add `dx8\include` to a project's additional include directories: the
SDK's old `basetsd.h` breaks the current Windows headers. The projects
append it after the Windows SDK through `$(Dx8SdkDir)` in
`CppSource\Directory.Build.props`. To keep the SDK elsewhere, pass
`/p:Dx8SdkDir=<path>` to MSBuild.

`d3dx8.lib` is the reason the real SDK is needed. It is a static library of
texture code built with Visual C++ 6. The project files already work around
its `libci.lib` default-library reference and its missing SafeSEH tables.

## 3. Folder layout

All paths are relative to the repository root. Every folder below is
ignored by git.

```text
dx8\          DirectX 8.1 SDK: include\ and lib\
DATA\         the installation's DATA folder, unchanged
EXE\          the installation's EXE folder, unchanged
bin\          Release build output, plus the runtime files listed below
DATA_EXTRA\   optional: shaders and settings for the volumetric nebula
```

The client starts in `bin\` and expects `..\DATA`. It probes
`..\DATA\CHARACTERS\ANIMATIONS\FACIALMALE.ANM`; when that file is missing it
starts `OutpostServer.exe` instead and exits.

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
| — | — | — |

## 7. Then

Fill in [facts.md](facts.md) (work package P0.3). Gate G0 also needs the
oracle harness of work package P0.4.
