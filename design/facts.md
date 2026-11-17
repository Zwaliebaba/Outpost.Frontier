# Facts from the original installation

Facts the repository cannot answer, collected from the original
installation and the [reference build](reference-build.md) (work package
P0.3). Fill in **Value** and set **Status** to `known`. Quote file contents
exactly. Do not paste licensed content beyond the value itself.

Paths are relative to the repository root, laid out as in section 3 of the
reference-build guide.

Collected on 2026-11-16 from the master copy in `outdir\`: its INI files,
and the installation's `DATA` in `outdir\data\`. `EXE\` below means the
INI files in `outdir\`, `DATA\` means `outdir\data\`.

`cargo xtask ini <file or folder>` lists INI files through the ported
reader, and `--grep <text>` shows only the values that contain the text,
with their file and section.

| ID | Fact | Where to look | Needed by | Value | Status |
| --- | --- | --- | --- | --- | --- |
| F1 | `[Libraries]` list of the engine INI, in order | The engine INI in `EXE\` (the build reads it as `NEURONCOMPONENTS.INI`) | P4.16 | `ReadFile.dll`, `x86math.dll`, `SceneRuntime.dll`, `system.dll`, `DX8RENDERER.dll`, `SoundStreamer.dll`, `SoundManager.dll`, `Deformable2.dll`, `SceneScript.dll`, `Shading.dll`, `RendComp.dll`, `particlefx.dll`, `ximage.dll`, `.\surfacematerials.dll` (client, `NeuronComponents.ini`). Server (`NeuronComponentssrv.ini`): `readfile.dll`, `sceneruntime.dll`, `x86math.dll`, `rendcomp.dll`, `system.dll`, `SceneScript.dll`, `dx8renderer.dll`, `shading.dll`. Both files start with a commented-out `; @include FL_Dev.ini` | known |
| F2 | `[System]` list of the engine INI, in order | Same file | P4.16 | Client: `VMeshLibrary`, `DirectX8`, `SoundManager`, `SoundStreamer`, `TextureLibrary`, `MaterialAnimation`, `MaterialBatcher`, `ParticleFx`, `FxRuntime`. Server: `DirectX8`, `MaterialLibrary`, `TextureLibrary` | known |
| F3 | `[Resources]` DLL list, in order; it fixes the high 16 bits of every text ID | Client INI in `EXE\` | P1.6 | `FieldGuideText.dll`, `DialogueText.dll`, `NameCatalog.dll`, `EquipmentCatalog.dll`, `NegotiationText.dll`, `InterfaceExtras.dll`. The client registers `UiText.dll` before the list (`main.cpp`), so the high 16 bits are 0 `UiText`, 1 `FieldGuideText`, 2 `DialogueText`, 3 `NameCatalog`, 4 `EquipmentCatalog`, 5 `NegotiationText`, 6 `InterfaceExtras`. A DLL that fails to load keeps its index (`dataconfig_progress.cpp`) | known |
| F4 | `local_server` value in `[OutpostFrontier]` | Client INI | P3.8, P5.3 | `rpclocal.dll` | known |
| F5 | Is `PHYSICAL_SIM_RATE` set in the data? Value | `DATA\` INI files, section `[PhySys]` | P2.3 | Not set. `DATA\constants.ini` has `[PhySysConsts]` without the key and no other INI file names it, so the default 16.0 applies (`common_constants.cpp`) | known |
| F6 | Are `.thn` scene scripts text or compiled Lua chunks? | First byte of each `DATA\**\*.thn`: 27 (`ESC`) means compiled | P4.14 | Compiled: all 1506 `.thn` files start with byte 27 | known |
| F7 | `maxSoundChannels` in `[SoundManager]`, and `useEAX` in `[AudioProperties]` | Engine INI | P4.10 | `maxSoundChannels = 24` (comment: "was 30. changed for Beta"). No INI file has `[AudioProperties]` or `useEAX`, so the `SoundManager` default applies | known |
| F8 | Container and codec of each `[Startup] movie_file` | Client INI for the names; a media inspector for the files | P4.15 | Three ASF files: `mgs_logo_final.wmv` 640x480, `da_logo_final.wmv` and `fl_intro.wmv` 720x480. Video WMV2 (Windows Media Video 8); audio format tag 0x0161 (Windows Media Audio v2), stereo, 44.1 kHz, 64 kbit/s. Read from the ASF stream headers | known |
| F9 | Font faces named in `[TrueType]`, and whether their files are in the installation | Fonts INI named by `[Data] fonts` in the client INI | P4.12 | `Agency FB` and `Arial Unicode MS` in `fonts.ini`; `rich_fonts.ini` also names `FixedSys`. The installation has no `fonts\files\` folder (`fonts_dir`), so the faces come from Windows; `AGENCYR.TTF`, `AGENCYB.TTF` and `ARIALUNI.TTF` are installed on the owner's machine | known |
| F10 | DLL names in the INI copies in `bin\` before and after editing them for this build | All INI files copied to `bin\` | P0.2, P4.16 | The master in `outdir\` already holds this build's names: as F1 and F3, plus `local_server = rpclocal.dll` and `Content.dll` in every `[Initial SP DLLs]` and `[Initial MP DLLs]` row. The run folders copy them unchanged. The names before editing are not kept | known |
| F11 | DLLs named by an INI that this build does not produce | Same | P0.2 | None. Every DLL that the INI files in `outdir\` name is in the build output | known |
| F12 | Does `DATA_EXTRA\` exist, and does it hold the nebula HLSL sources? | `DATA_EXTRA\SHADERS` | P6.4 | No. There is no `DATA_EXTRA\` folder, and the C++ tree holds no copy of its shaders. P6.4 writes new ones (decided 2026-11-17) | known |
| F13 | Feature switches used for the reference build | The MSBuild command line | P0.4 | Defaults, confirmed from the Release build logs: widescreen on, walk mode on for the client, tick profile 0, beta label off, no `DX9RENDERER.dll` | known |
| F14 | Does single player keep one shared object world or two? | Observed with the oracle harness | P2.0 | Moved to the P2.0 design spike on single-player world ownership, which observes it with the oracle | moved to P2.0 |

The following commands help with F6 and F5 in PowerShell, from the
repository root:

```powershell
# F6: count .thn files by first byte (27 = compiled, anything else = text)
Get-ChildItem outdir\data -Recurse -Filter *.thn |
  ForEach-Object { [IO.File]::ReadAllBytes($_.FullName)[0] } | Group-Object

# F5: every INI file that sets the key
cargo xtask ini outdir\data --grep PHYSICAL_SIM_RATE
```
