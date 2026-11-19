# Naming in the game data: analysis and a path to consistency

Status: analysis, 2026-11-18. Nothing here is decided; the options at the
end are for the owner. Companion to [rebrand-names.md](rebrand-names.md)
(decision O17).

## Where a name lives

A name in `outdir/` is bound in up to four places, and a rename is only
safe when every place that holds it changes together:

| Layer | Examples | How it refers | Renamed by the rebrand |
| --- | --- | --- | --- |
| 1. INI text | `nickname = he_elite`, `ship_archetype = or_elite`, `msg = gcs_refer_base_ve01_01_base-` | the string, hashed by `CreateID` at load | yes, by token substitution, unevenly (below) |
| 2. Binary data | `.thn` template strings, `.utf` voice archives (entries named `0x%08X` of `CreateID(msg)`), texture and material names inside `.3db`/`.cmp`/`.txm`, `.ale` node names | string, or the hash of the string | `.ale` and the player character in `.thn` (O17); the rest of `.thn` (second pass, 2026-11-18); nothing else |
| 3. Code | literals and `CreateID("…")` in `CppSource/` and the Rust crates | string or hash | yes, in `89eda18`, without checking the data |
| 4. Paths | `Universe\Systems\Li01\li01.ini`, `bases\liberty\li01_bldg_A.3db` | file system | no, by decision (O17) |

The player never sees a nickname. Names on screen come from the text
resources (`ids_name`, `outdir/data/text/`). The rebrand's visible effect
does not depend on internal identifiers.

## What the data holds today

Counts are over the 25,483 distinct INI nicknames unless stated.

### System codes: renamed throughout

No `li01`-style code survives in a nickname (Li→he 2,209 names, Br→ve
2,247, Ku→or 1,649, Rh→dm 1,580, Bw→fr 1,918, Iw→fc 914, Hi→pe 245,
St→ep 141, Ew→oe 455). Paths keep the old codes: 1,045 files and folders
are named `Li01…`, `Br05…` and so on; none use the new codes. The base-room
style `li_01_cityscape` kept the old code: 559 distinct tokens, none
renamed (`petaldb.ini`, `mbases.ini`, the `li_01_*.thn` scripts).

### House tokens: mostly not renamed

| Code | Old `xx_` names left | New `xx_` names | Examples of old |
| --- | --- | --- | --- |
| li→he | 478 | 68 | `li_lsf_grp`, `engine_li_cruise_loop`, `li_n_he_elite_d4` |
| br→ve | 475 | 62 | `br_m_grp`, `engine_br_cruise_start` |
| ku→or | 471 | 104 (some are the Order's own) | `engine_ku_small`, `ku_dragon`, `ku_gunboat` |
| rh→dm | 767 | 67 | `rh_m_grp`, `fc_rh_grp` |
| bw→fr | 68 | 130 | `engine_bw_cruise_loop` |
| iw→fc | 9 | 449 (most are the `fc_` faction prefix) | `music_iw_space` |
| st→ep | 12 | 0 | `rtcprop_st_br_thruster` |
| ew→oe | 6 | 9 | `ew_dmg_portwing01_cap` |

The substitution renamed a house token in some names and left it in
others, often in the same name: 102 nicknames mix an old code with its new
one (`li_n_he_elite_d4`, `li_p_he_fighter_d3`, `ku_n_or_elite_d9`).
Factions show the same split: `he_n_grp`, `he_p_grp`, `or_n_grp` and
`dm_p_grp` were renamed; `li_lsf_grp`, `br_m_grp`, `rh_m_grp` and
`fc_rh_grp` were not.

### Words

`nomad` → `veilborn` (0 left / 465), `liberty` → `helion` (0 / 61),
`kusari` → `orsena` (0 / 12) are complete in nicknames. `trent` → `rellan`
leaves 5 (`MSN04_Trents_Friend`, `rtc_trentripple`, `com_0140trent`, plus
`ol_dock_Trenton`, which is a real word). Bretonia and Rheinland have no new
word yet.

### Collisions the new codes made

- **or**: Kusari (Ku→or) against the Order (`or_elite`, `or_osiris`,
  `fc_or_grp`). The `or_elite` clash is fixed (`ku_elite`, decided
  2026-11-18); the code still means two houses.
- **fc**: Iw systems (`fc01`) against the original faction prefix
  (`fc_c_grp`, `fc_j_gun01_mark01`). No nickname clashes today, but `fc_`
  can no longer say which of the two is meant.
- **pe01_01_Base**: also the nickname inside `EW06/BASES/ew06_02_base.ini`;
  probably an original slip, left as shipped.

### Bindings broken in binary data and code

1. **Voice lines (layer 2), fixed 2026-11-18** (rebrand-names.md, voice
   archives). As found: of 22,897 `msg` lines in `AUDIO/voices_*.ini`,
   16,751 find their entry in the voice's `.utf` archive. 5,906 find it only
   under the *old* spelling of the name (`gcs_refer_base_ve01_01_base-`
   is stored as the hash of `gcs_refer_base_br01_01_base-`), and 12 only in
   another archive under the old spelling. The 5,906 were silent: base and system callouts by traffic control and pilots
   (`voices_space_*`, `voices_base_*`, about 5,900) and mission lines
   naming Rellan or Helion. The remaining 228 are not rename effects (227
   are found in another voice's archive, as in the original; one is
   missing).
2. **Code literals (layer 3), fixed 2026-11-18 (decision O30)**: the
   literals take back their spelling from before the rebrand. As found,
   `89eda18` renamed string literals that name files or contents the data
   does not rename:
   - `VeilbornRGB1_VeilbornAlpha1` (`surfacematerials/MaterialImpl.cpp`),
     while `SHIPS/NOMAD/nomad_fx.txm` and `no_gunship.3db` hold
     `NomadRGB1_NomadAlpha1`. This is the `texture verify failed: nomad
     surface` line in the client log.
   - `Interface\NeuroNet\NavMap\NewNavMap\zoomedmap_helion.3db`
     (`nn_navmap.cpp`); the file is `zoomedmap_liberty.3db`.
   - `interface\HUD\HUD_contactveilborn.3db` (`hud_contact_list.cpp`); the
     file is `hud_contactnomad.3db`.
   Other renamed literals checked (`transport_helion`,
   `nav_terrain_veilbornast`, `rellan`) resolve. A name built at run time, or
   a literal in the Rust crates, was not checked.
3. **Cutscene templates (layer 2).** Fixed: 80 names in 150 `.thn` files
   (rebrand-names.md, second pass).

## Why it drifted

The rebrand was a text substitution over INI files and code, without a
table of entities and without a check that each reference still resolves.
Token rules (`li` → `he` where it looked like a code) cannot tell a house
code from a faction prefix (`fc_`, `or_`), or a system code from a word.
Every binary format that stores a string or its hash kept the old one.
Nothing in CI resolves references across the layers, so each break shows up
only as a log line at run time.

## Paths to consistency

Each option needs the same two tools before any bulk change, so they come
first:

- **A name table**, committed under `design/` or `outdir/`: one row per
  token family (house, system, character, word) giving the old spelling,
  the new one, and the scope where it applies. It replaces the regexes
  and is what every rename reads.
- **`cargo xtask names check`**, run in CI: it loads the INI data and
  resolves every reference. That covers INI-to-INI references, `.thn`
  templates and loadouts, voice `msg` names against the `.utf` entry
  hashes, the INI paths against files on disk, and code literals that name
  files. It fails on anything new that is unresolved and keeps a list of the
  known gaps in the original data (`ring`, `pe01_01_Base`,
  `DX_M04_1290_DORNMARKER1`). The scans behind this page are a prototype of
  it.

### Option 1: finish the rebrand (data carries the new names everywhere)

Rename every remaining old token from the table across all four layers:
house tokens (about 2,300 names plus references), faction groups, the
`li_01_*` room family, the five `trent` names. Rewrite the voice archives'
entry names with the new hashes; the entries are fixed-width text, so they
rewrite in place. Rename paths and files, or exclude paths from the scheme
explicitly. Collisions need new codes first: Kusari cannot be `or` while
the Order is, and Iw cannot be `fc` while factions are `fc_`. The
`ku_elite` fix would become a third spelling to undo.

- For: one scheme everywhere; matches what O17 started.
- Against: the largest change by far; touches `.cmp`/`.3db` internals and
  1,045 paths; every name change also needs a C++ reference change
  (frozen, an O-record each), or the oracle sees different data from the
  port.

### Option 2: internal identifiers stay original, the rebrand is display only

Keep nicknames, hashes and paths in the original scheme and put the rebrand
only in what the player sees (text resources, UI art). Revert layer 1 and
the code literals of `89eda18` that name data.

- For: the binary data, file names and hashes are consistent already;
  nothing in layer 2 or 4 changes; collisions disappear.
- Against: undoes O17 and part of `89eda18`. The original INI files are not
  in the repository, so the reversal has to be derived from the rename
  table, and it is ambiguous where the substitution merged names (`or_`
  meant two houses). The internal names then carry the old brand.

### Option 3: one table, stable identifiers, finish only where it is cheap and safe (recommended)

Treat nicknames as identifiers, not branding. Freeze the current scheme as
the baseline and make it self-consistent where that is mechanical and
local, without new collisions:

1. Fix the bindings broken in binary data and code now. They are bugs
   whichever way names go:
   - rewrite the voice entry hashes to the names the INI uses (done:
     5,906 entries; the 12 found only in another archive stay as in the
     original);
   - point the three code literals at the names the data has (done, O30);
   - the `.thn` pass is done.
2. Stop renaming house tokens and the `li_01_*` family. Record in the table
   that house prefixes keep the original codes (`li_`, `br_`, `ku_`, `rh_`,
   `bw_`, `iw_`, `st_`, `ew_`, as `ku_dragon` and `li_lsf_grp` already
   do). System codes (`he01`) and the words (`veilborn`, `rellan`,
   `helion`, `orsena`) stay new. This removes the `or`/`fc` ambiguity for
   houses without renaming anything that works today.
3. Normalise the 102 mixed names to that rule as `names check` flags
   them (`li_n_he_elite_*` → `li_n_li_elite_*`), one family per change,
   with the C++ reference and the oracle moving together.
4. Paths stay as they are; the table says so, and `names check` resolves
   them.

- For: fixes every audible and visible break first; small, reviewable
  steps; one written rule that the tool enforces; no new collisions.
- Against: two code systems remain by design (system codes new, house
  prefixes old), which must be written down so nobody "completes" it
  again by regex.

## Next steps if Option 3 is chosen

1. Record the rule as a decision (O33) and the table beside
   rebrand-names.md.
2. Build `cargo xtask names check` from the scans used here, with the
   known-gap list, and add it to CI.
3. Rewrite the voice archive hashes. Done 2026-11-18.
4. Fix the three code literals in `CppSource/`. Done 2026-11-18 (O30); the
   Rust crates do not use them yet.
5. Normalise the mixed names family by family.
