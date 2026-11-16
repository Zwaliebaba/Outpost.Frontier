# Facts from the original installation

Facts the repository cannot answer, collected from the original
installation and the [reference build](reference-build.md) (work package
P0.3). Fill in **Value** and set **Status** to `known`. Quote file contents
exactly. Do not paste licensed content beyond the value itself.

Paths are relative to the repository root, laid out as in section 3 of the
reference-build guide.

The data INI files may be binary (they then start with the bytes `BINI`).
Text search does not work on those. `cargo xtask ini <file or folder>`
lists text and BINI files alike through the ported reader, and
`--grep <text>` shows only the values that contain the text, with their
file and section.

| ID | Fact | Where to look | Needed by | Value | Status |
| --- | --- | --- | --- | --- | --- |
| F1 | `[Libraries]` list of the engine INI, in order | The engine INI in `EXE\` (the build reads it as `NEURONCOMPONENTS.INI`) | P4.16 | | open |
| F2 | `[System]` list of the engine INI, in order | Same file | P4.16 | | open |
| F3 | `[Resources]` DLL list, in order; it fixes the high 16 bits of every text ID | Client INI in `EXE\` | P1.6 | | open |
| F4 | `local_server` value in `[OutpostFrontier]` | Client INI | P3.8, P5.3 | | open |
| F5 | Is `PHYSICAL_SIM_RATE` set in the data? Value | `DATA\` INI files, section `[PhySys]` | P2.3 | | open |
| F6 | Are `.thn` scene scripts text or compiled Lua chunks? | First byte of each `DATA\**\*.thn`: 27 (`ESC`) means compiled | P4.14 | | open |
| F7 | `maxSoundChannels` in `[SoundManager]`, and `useEAX` in `[AudioProperties]` | Engine INI | P4.10 | | open |
| F8 | Container and codec of each `[Startup] movie_file` | Client INI for the names; a media inspector for the files | P4.15 | | open |
| F9 | Font faces named in `[TrueType]`, and whether their files are in the installation | Fonts INI named by `[Data] fonts` in the client INI | P4.12 | | open |
| F10 | DLL names in the INI copies in `bin\` before and after editing them for this build | All INI files copied to `bin\` | P0.2, P4.16 | | open |
| F11 | DLLs named by an INI that this build does not produce | Same | P0.2 | | open |
| F12 | Does `DATA_EXTRA\` exist, and does it hold the nebula HLSL sources? | `DATA_EXTRA\SHADERS` | P6.4 | | open |
| F13 | Feature switches used for the reference build | The MSBuild command line | P0.4 | | open |
| F14 | Does single player keep one shared object world or two? | Observed with the oracle harness | P2.0 | | open |

The following commands help with F6 and F5 in PowerShell, from the
repository root:

```powershell
# F6: count .thn files by first byte (27 = compiled, anything else = text)
Get-ChildItem DATA -Recurse -Filter *.thn |
  ForEach-Object { [IO.File]::ReadAllBytes($_.FullName)[0] } | Group-Object

# F5: every INI file, text or BINI, that sets the key
cargo xtask ini DATA --grep PHYSICAL_SIM_RATE
```
