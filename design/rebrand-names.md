# Rebranded names the original data still uses

Status: open, found 2026-11-17 during Phase 1. Needs an owner decision
before the packages that use these names (Phases 2, 3 and 5).

The commit that set up the Rust workspace and rebranded the C++ modules
(`89eda18`) also changed string literals that the game data matches. The
original data in `outdir/` (decision O15) still uses the old spellings, so
wherever the reference compares with one of these strings it no longer
finds what the original game found. One case is fixed already: the
particle node-library name (decision O16).

## How the list was made

Every string literal of three or more characters that `89eda18` removed
from `CppSource/**/*.{cpp,h}` and did not add back (360 literals) was
searched for, ignoring case, in `outdir/data/` and the INI files at the top
of `outdir/`. 20 of them occur in the data. Each was then traced to the
code that now holds its replacement. The scan only sees literals the
commit removed whole; a rename that kept part of a literal, a name built
at run time, or a hash of a name would not show, so the list is a lower
bound.

## Found

| Data spelling | Reference now | Where | Effect in the reference | Phase |
| --- | --- | --- | --- | --- |
| `AlchemyNodeLibrary` | was `ParticleFxNodeLibrary` | `particlefx/particlefx_common.cpp` | Fixed by O16 | P1.7 |
| INI key `alchemy` | `particlefx` | `OutpostFrontier/Client/vis_effect_arch.cpp:759` | The effect files named in `FX/EQUIPMENT/equipment_ale.ini` (19 lines), `FX/WEAPONS/weapons_ale.ini` (575) and `FX/ENGINES/engines_ale.ini` (106) are not read | P5.6 |
| System list `St01`, `St02`, `St03`, `St02b`, `St02c`, `St03b`, `Ew05`, `Ew06`, `FP7_system` | `Ep01`, … `Oe05` | `Common/common_universe.cpp:265` | The systems this list singles out are not matched | P2.1 |
| `FP7_system` | `RELAYSEVEN_system` | `Content/PlayerState.cpp:39`, `:218` | The system ID the story code looks up is not found | P3.5 |
| Object `Ku07_02` | `Or07_02` | `OutpostFrontier/Client/hud_status_core.cpp` (`G_ShieldlessShipId`) | The ship the HUD treats as shieldless is not recognised | P5.7 |
| Archetype type value `nomad` | `veilborn` | `Common/common_archetype_ships.cpp:799` | Ships of type `nomad` are not given that type | P2.1 |
| Costume `trent` | `rellan` | `OutpostFrontier/Client/base_walk.cpp:247` | The walk-mode character costume is not found | P6.3 |
| Template prefix `$BGCS_Trent_` | renamed | `OutpostFrontier/Client/gf_scenescript_sound.cpp` | Voice-line templates for the player character are not matched | P5.9 |

## Options

1. Restore the original spellings in the reference, one owner decision
   (like O16) covering this list, and port them as the data has them.
2. Leave the reference as built, port the original spellings, and record
   each place as a divergence. The oracle then cannot check those paths.
3. Rename the data to the new spellings. This changes the original data,
   which decision O15 keeps as the master copy, and the port would no
   longer load an unmodified installation.

Option 1 keeps the oracle meaningful and the data untouched.
