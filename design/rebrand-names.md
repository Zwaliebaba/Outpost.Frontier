# Rebranded names in the data

Status: decided 2026-11-17, option 3 (decision O17). The first version of
this page, written earlier the same day, assumed the data still used the
old names throughout; it does not, and most of its rows were wrong.

## What the data holds

The commit that set up the Rust workspace and rebranded the C++ modules
(`89eda18`) renamed systems, bases, characters and modules in the code. The
data committed in `outdir/` (decision O15) had been renamed the same way,
but only in its text files:

- INI and `.fl` files use the new nicknames: every system and base (Li→he,
  Rh→dm, Br→ve, Ku→or, Iw→fc, Bw→fr, Hi→pe, St→ep, Ew→oe), `RELAYSEVEN_system`,
  `Or07_02`, `veilborn`, the costume and voice `rellan`, and the key
  `particlefx` in the effect lists. Nickname hashes in the text match: the
  `vnpc` lines in `newplayer.fl` hold `CreateID("ep01_01_base")`.
- File and directory names keep the old spelling
  (`systems\St01\St01.ini`, `Audio\Dialogue\Trent\…`). The text refers to
  them by those paths, so they resolve.
- Binary files were not renamed.

## Mismatches found, and what O17 did

| Where | Old name | Matched against | Done |
| --- | --- | --- | --- |
| 596 `.ale` files | node library `AlchemyNodeLibrary` | `kNodeLibName` in `particlefx/particlefx_common.cpp` | Renamed to `ParticleFxNodeLibrary` in the UTF string table (it fits the table's slack in every file); O16 undone |
| 851 `.thn` cutscenes (compiled Lua 3.2) | DEFORMABLE template `trent` | costume nickname in `CHARACTERS/costumes.ini` (`rellan`) | Renamed (698 templates) |
| same | SOUND templates `VoiceProfile_$BGCS_Trent_*`, `VoiceProfile_GCS_Trent_*` | voice messages of `rellan_voice` in `AUDIO/voices_recognizable.ini` | Renamed |
| same | SOUND templates `dx_s*_Trent` | sound nicknames `dx_s*_Rellan` in `AUDIO/story_sounds.ini` | Renamed |
| `Server/server_pub.cpp`, `SetRellan` | `CreateID("trent")` (`0xb8249f07`) | costume nickname `rellan` | Now `CreateID("rellan")` (`0x960dd804`) |

Before the rename none of the 916 costume and sound templates naming the
player character resolved; after it, 914 do. The two left
(`VoiceProfile_$BGCS_Rellan_F09aP01_5` in `gcs_mlstand_f09ap_010.thn` and
`gcs_mlsit_f09ap_010.thn`) name voice messages that exist in neither
spelling, a gap in the data.

Entity, camera and marker names inside the scripts (`Char_Trent`,
`Sc_dx_…_Trent`, `Mrk_TrentWalkAlign_0sec`) are matched only within their
own script and keep the old spelling. So do a few text nicknames the rename
missed but uses consistently (`MSN04_Trents_Friend`).

## How the lists were made

Strings: every Lua string constant in the `.thn` files that follows an entity
type was checked against the costume, sound and voice tables. Hashes: the
`CreateID` of 781 old names recoverable from the data (system and base file
names, directory names, and the names of the first version of this page) was
searched for as a hex or decimal literal in `CppSource/`; `SetRellan` was the
only hit, and the other four costume hashes beside it match current
nicknames. A name built at run time, or a hash of a name that appears
nowhere in the data, would not show, so later phases can still find one.
They are handled the same way: the data keeps the new name.
