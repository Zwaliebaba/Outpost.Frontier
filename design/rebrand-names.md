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

## Second pass: other templates in the cutscenes (2026-11-18)

O17 checked only costume and sound templates naming the player character.
A scan of every template in all 1,506 `.thn` files (`outdir/data/SCRIPTS`
and `outdir/data/MISSIONS`) against the INI nicknames found 80 more old
names: PetalDB props (`li01_bldg_A`, `starsphere_Li01`), ships (`li_elite`,
`bw_freighter`), loadouts (`li_n_li_elite_loadout01`), effects
(`rtc_libertynewsicon`, `gf_nomadwormhole`), asteroids (`nomad01_asteroid10`)
and sounds (`engine_li_fighter_loop`, `music_bar_li04`, `DX_M13_*_TRENT`).
Each had exactly one renamed counterpart in the INI data, which is not
always a plain prefix swap (`li_n_li_elite_loadout01` is
`li_n_he_elite_loadout01`). 256 string constants in 150 files were
renamed to the INI spelling; every patched file still decompiles and
recompiles to itself.

Left as they are:

- `ring` (PSYS in `INTRO/intro_waterplanet.thn`): no effect of that name in
  either spelling, a gap in the data.
- `ku_elite` in `BASES/aa_test_all_ships.thn`: see the collision below.

Ku→or collided with the Order's own `or_` prefix: the text rename made the
Kusari elite `or_elite`, the Order elite's nickname, so `SHIPS/shiparch.ini`
had two `[Ship] or_elite` and two `[Simple] or_elite_dmg_port_wing_cap`, and
`EQUIPMENT/misc_equip.ini` two `[Power] or_elite_power01`. The first
definition wins, so every Order reference (missions M01A and M12–M13, the
Orillion and Quintaine cutscenes, `oe_hull`) got the Kusari ship. Decided by
the owner on 2026-11-18: the Kusari copies take back the `ku_` prefix, as
`ku_dragon`, `ku_gunboat` and `ku_destroyer` never lost it. Renamed: the
ship `ku_elite`, its five damage parts `ku_elite_dmg_*`, the power plant
`ku_elite_power01`, and the 34 lines that mean the Kusari ship — its own
`dmg_obj` lines, `ke_hull` and `ke_package` in `goods.ini`, the
`ku_n_or_elite_*` and `SECRET_ku_n_or_elite_*` loadouts and NPCs, the
Veilborn-held Orsena heavy fighters of M08 and M09 (`MSN0[89]_Veilborn_Orsena_Heavy_Fighter`),
`khf` in `npcships_test.ini`, the Kusari row of `mshipprops.ini`, the KU05
surprise wrecks' `dmg_obj` lines and the `[Archetype]` preload of KU07.
Loadout and NPC nicknames keep their `or` spelling; they are unique. Every
cutscene template naming a ship now resolves, `ku_elite` in
`aa_test_all_ships.thn` included.

## Voice archives (2026-11-18)

A voice's `.utf` archive in `outdir/data/AUDIO` names each line's entry
`0x%08X` of `CreateID(msg)`. The text rename changed the `msg` names in
`voices_*.ini` (`gcs_refer_base_br01_01_base-` became
`gcs_refer_base_ve01_01_base-`, `DX_M05_0400_TRENT` became
`DX_M05_0400_RELLAN`) but not the archives, so 5,906 lines in 35 archives
were silent, almost all of them base and system callouts. Each such entry
is renamed to the hash of the name the INI uses: it is the only entry in
the voice's own archive that matches an old spelling of the name (the
reverse of the code and word table above), and no line that resolved before
uses it. The entry names are fixed-width text in the UTF string table, so
the files keep their size and nothing outside the string table changes. Of
22,897 `msg` lines, 22,657 now find their entry (16,751 before). The other
240 are not rename effects within one archive: 239 have their entry only in
another voice's archive (12 under an old spelling) and
`DX_M04_1290_DORNMARKER1` has none.

`[BaseInfo] pe01_01_Base` is defined twice (`EW06/BASES/ew06_02_base.ini`,
`HI01/BASES/hi01_01_base.ini`) while `universe.ini` lists the EW06 base as
`oe06_02_Base`. The rename mapped Hi01→pe01 consistently, so the original
data most likely had the same slip; decided by the owner on 2026-11-18 to
leave it as shipped.

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
