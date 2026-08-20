# ADR-024 — The Mining Economy: Ores, Sites, Cargo, and Refining

**Status:** Proposed · 2026-08-20 (economy design session — **for owner review**; this is a
design document, no code rides with it, and every number in it is a reference tuning the
envelope suites will assert the *shape* of, not the value) · **the six questions the review
was owed are ruled** (owner rulings, 2026-08-20 — the final section records each with its
reason; every proposal stood, so no body number moved). Acceptance of the whole waits on the
owner's full read-through, and these rulings are normative the day it flips.
**Depends on:** ADR-002 (tick), ADR-005 (orders, group validation, determinism), ADR-009
(universe model), ADR-012 (JSON, content hashing, the §D13 balance-becomes-data hook),
ADR-014 (seam), ADR-015 (contact), ADR-016 (anchors, the reserved `Site` kind, the universe
runtime, summaries, presence), ADR-017 (docking, the roster, station commands, the transfer
bus), ADR-018 (persistent service; worlds forget — durable state lives at the universe
layer; D19 event record), ADR-019 (shard-global tick), ADR-022 (per-viewer interest, the
summary family)
**Amends, if accepted:** ADR-016 §3 — `AnchorKind::Site` stops being reserved and the bake
starts baking them; ADR-016's deliberate-gaps list — "mined-out fields" gets its policy (the
site ledger, §3) and "wrecks" gets its policy (bounded and non-durable, §5); ADR-017 §1 —
the station roster's "and nothing else" grows a per-ship cargo manifest; ADR-017 §6 — the
station surface's tab family activates **CARGO** and **REFINERY** beside HANGAR;
ADR-012 §D13 — the "when balance wants to be data" hook is cashed in: the economy tables are
the first hash-guarded balance content.

## Context

Owner brief for the session, in full: miners travel to asteroid areas — **every solar system
contains exactly 2 to 3 distinct areas**; ore is mined into the ship's hull, whose size is
per-hull configuration, and **mining stops automatically when the hull is full**; ore drops
as loot when a ship dies in PvP space; docking players **manually transfer** hull contents
into a persistent per-station **Station Bay**; inside the station, raw ore refines
**directly into alloys** (Phase-2 blueprint crafting is out of scope). Three ores are given —
**Astracite** (crystalline, high-radiation belts), **Ferro-Chroma** (dense, shattered
planetary crusts), **Nebulite** (volatile, gas-infused, nebulae) — and five alloy recipes
across three tiers: Ferrocite Plates (2 FC), Astra-Glass (1 AC + 1 NB), Chromite Conduit
(1 FC + 2 NB), Quantum Matrix (2 AC + 1 FC), Nova-Steel (2 AC + 2 FC + 2 NB). Asked for:
spawn distribution across security bands with environmental hazards, crafting ratios and
times, a station refining design, and the hull-capacity configuration model.

The tree is better prepared for this than the brief could know, and this ADR is written
against that preparation rather than beside it:

- **The extension point already has a name.** ADR-016 §3 reserved `AnchorKind::Site` with
  the sentence *"mining fields and PVE encounters are new `Site` anchor rows, zero new
  architecture"* — and `Universe.h` carries the reserved value today. A mining area is a
  Site anchor: a warp destination, a grid, a world when someone is there. Nothing below
  invents a second way for a place to exist.
- **Security is already content.** Every system carries `security` 0–100, baked inside its
  region's band (`securityFloor`/`securityCeiling`), and the bake's three region archetypes
  — 60–95, 25–70, 0–35 — are exactly the brief's High-Sec / Low-Sec / Null-Sec. The map
  already draws the badge; this ADR keys the economy on the number that exists.
- **Storage and station verbs have a home.** ADR-017 built the station roster at the
  universe layer, the `StationCommand` family, and the transfer bus; ADR-018 D2 ruled that
  durable state lives at the universe layer because worlds forget. The Station Bay is a
  second universe-layer ledger beside the roster, and cargo verbs are station commands.
- **The hulls exist.** `HullClass::Miner = 9` and `HullClass::Hauler = 8` have meshes,
  class rows, and no job. This ADR is their job arriving.
- **The balance-as-data hook is waiting.** The class table is compiled-in on the stated
  ground that "when balance wants to be data (post-MVP), it arrives with a hash, not
  before" (ADR-012 §D13). An economy is the moment balance wants to be data — ores,
  recipes, hold sizes and site archetypes are content the way the universe is content —
  so it arrives exactly as §D13 says: same parser, same fail-closed hash posture.

What no document had decided: where each ore lives and what it costs to go get it; what a
mining order *is* in a game whose player is a disembodied commander of 64-ship fleets; how
a depleted field survives a world teardown; what refining costs and what a station can
become; and where hull capacities are authored. Those are this ADR's sections.

## Decision

### 1. The brief's contradictions, ruled on first

Four things in the brief do not survive contact with itself or with the tree. Each gets a
ruling here rather than a silent workaround, and each ruling is one the review can reverse
cheaply — they are parameters and postures, not architecture.

**1a — Nebulite's lore says "deep space nebulae"; the loop says all mining happens in a
system's 2–3 asteroid areas.** Ruled: site *archetypes*. One of the three archetypes is a
**Nebula Pocket** — an asteroid field embedded in a dense nebula bank inside the system —
and Nebulite spawns there. The lore keeps its word ("gas-infused" is true of the rocks
because of where they sit), the loop keeps its shape (all mining is at Site anchors), and
the client keeps a gift: ADR-006's Nebula pass already draws art-directed nebula fields, so
the one biome that needs atmosphere has a renderer node waiting.

**1b — Astra-Glass is Tier 1 ("entry-level canopies") but requires Nebulite, the most
dangerous ore.** As given, a new player cannot craft half of Tier 1 without leaving
High-Sec. Ruled: **faded nebula pockets exist in High-Sec** — grade-capped at I, thin
yields, the hazard still on — and the bake guarantees at least one faded-pocket system per
High-Sec region. A High-Sec player can taste Nebulite at bad rates a few jumps from home;
doing it *well* means Low-Sec or the future market. The Tier-1 recipe stays craftable
everywhere, and the pull toward danger stays real. (The alternative — reclassifying
Astra-Glass to Tier 2 — changes the owner's recipe sheet, which this session does not do.)

**1c — "Exactly 2 to 3 areas" times three ores means most systems cannot feed themselves.**
Not a flaw — **the design leans into it**. With 2–3 sites per system and archetypes
weighted by band, mono-system self-sufficiency is rare by construction, common only in
Null-Sec triples (§3), and that is the trade network: Ferro-Chroma flows out of High-Sec,
Nebulite flows out of the dark, and haulers exist because no one system holds the whole
recipe book. The bake guarantees every *region* covers all three ores; no *system* is owed
that.

**1d — "Ore drops as loot on destruction" at 100% makes ganking strictly better than
mining.** If the whole hold drops, a destroyed hauler is a free hauler-load and piracy
out-earns industry forever. Ruled: **half drops, half is destroyed** — per stack, floor —
into a wreck (§5). The victim always loses more than the attacker gains, which makes
destruction the economy's sink rather than its shortcut. This rule is *forward design* in
the exact sense ADR-017 §5's damage immunity is: combat does not exist, nothing can be
destroyed today, and the rule is written down now so the loot table never has to be
invented in the middle of the combat ADR.

One more tension is named rather than ruled, because the answer is staging, not design:
**hazards that deal damage need damage to exist.** Every hazard below states its Phase-1
component (active with this phase: cycle slowdown, sensor dampening, overheat lockouts)
and its combat-era component (radiation burn, pocket detonations, debris wear), reserved
with numbers the way `CombatEngaged` is reserved with a value.

### 2. The ores

Cargo and recipes count in **integer units**; a unit of each ore has an **integer volume in
litres**. Integer litres for the same reason positions are integer metres and the parser
keeps exact `int64` (ADR-012 §C7): cargo arithmetic must be exact, hashable, and identical
on both halves — a hold is full when `usedLitres + unitVolume > capacityLitres`, and no
float ever rounds two clients into different answers. Prose below uses m³ where it reads
better; the data is litres (1 m³ = 1,000 L).

| Ore | Id | Unit volume | Home archetype | Where it spawns | Value index |
|---|---|---|---|---|---|
| Ferro-Chroma | `ferroChroma` | 300 L | Ferrous Crust Field | every band, abundant | 1.0 |
| Astracite | `astracite` | 200 L | Irradiated Belt | every band; High-Sec grade-capped | 2.2 |
| Nebulite | `nebulite` | 250 L | Nebula Pocket | Low/Null; faded pockets in High | 3.0 |

The **value index** is not a price — no currency exists — it is the tuning anchor future
pricing hangs off, and it is *derived*, not felt: inverse effective mining rate (§4 gives
1,080 / 720 / 540 units per hour, so 1.0 / 1.5 / 2.0) times an access-risk multiplier for
how much of the supply sits in shootable space (×1.0 / ×1.45 / ×1.5). Ferro-Chroma is
deliberately the cheap, bulky workhorse: it appears in four of the five recipes (7 of the
15 recipe-units), it is the newbie's first ore, and at 300 L/unit it is the least
value-dense cargo in the game — hauling it in bulk is the Hauler's reason to exist, while a
Nebulite run packs triple the value into five-sixths of the space, which is what makes the
dangerous run worth escorting.

**Recipe demand, checked against the sheet:** across the five recipes Ferro-Chroma is
needed 7 units, Astracite 5, Nebulite 5. Supply is tuned to match: FC common everywhere,
AC and NB equal in aggregate but gated by hazard and band. The brief's recipe sheet is
internally consistent on this point and is adopted verbatim.

### 3. Sites — where ore lives

#### 3a. A site is an anchor

`AnchorKind::Site` stops being reserved. A site is a baked anchor row like any other —
origin on the system disc between the planet orbits, warp-in point at the field's edge
(2 km standoff), authored facing, zero occupants — plus a **site block**:

- `archetype` — `ferrousField` | `irradiatedBelt` | `nebulaPocket`
- `grade` — I–V (§3d): how rich, how hazardous
- `fieldRadiusCm` — the rock field's extent (8–15 km; bake invariant: field plus warp-in
  standoff sits inside the 40 km grid bound, the same invariant warp-in points obey)
- `layoutSeed` — the deterministic rock-field layout (§4c)
- per-ore starting pool, in units, from the archetype × grade tables below

**The id price, stated:** 2,500 systems × 2–3 sites ≈ **6,250 new `AnchorId`s** on top of
today's 18,618, against the u16 window `Ids.h` prices at ~20,000 used of 65,535. Sites fit
with room to spare *because rocks are not entities* (§4c) — the design that would have put
each asteroid in the occupant window is rejected there, and U4's occupant-block lesson
(74,848 authored ids refused by the bake) is why this ADR checks the arithmetic before
proposing the content.

#### 3b. Three archetypes, and what each does to you

Every hazard has a Phase-1 half (lands with this economy, no damage model needed) and a
combat-era half (reserved, numbered, activated when damage exists). Grades scale both.

| Archetype | The place | Phase-1 hazard (active) | Combat-era hazard (reserved) |
|---|---|---|---|
| **Ferrous Crust Field** (`ferrousField`) | Shattered planetary crust; dense, dark debris | None — this is the tutorial biome | Micro-debris wear: slow armor tick while inside |
| **Irradiated Belt** (`irradiatedBelt`) | Glowing belts around hard stars | **Radiation exposure**: +1 stack per completed mining cycle in-field; each stack +3% cycle time; stacks decay 1 per 30 s outside the field | Past 15 stacks, hull burn per second |
| **Nebula Pocket** (`nebulaPocket`) | Asteroid cluster inside a dense nebula bank | **Sensor dampening**: detection/target range factor by grade (I ×0.7 … V ×0.2), applied through ADR-022's relevance seam — it dampens *everyone*, so the ganker closes unseen but also hunts blind. **Volatile heat**: each cycle adds heat; over threshold the laser locks out for 60 s; pacing cycles ×1.25 slower sheds it | Heat threshold instead triggers a gas-pocket detonation: AoE damage around the rock |

Two things worth saying about this table. First, **the dangerous ore hides where you
cannot see the hunter coming** — the sensor-dampening nebula holding the highest-value ore
is the deliberate centre of the risk design, and it is symmetric on purpose: escorts inside
the pocket are ambushers too. Second, a symmetry the owner's own recipe sheet creates:
**each biome's hazard is countered by the alloy made from its ore** — Quantum Matrix
(Astracite) is the shield/CPU alloy that will harden ships against radiation, Chromite
Conduit (Nebulite) is the cooling alloy that will tame nebula heat, Ferrocite Plates
(Ferro-Chroma) armor against debris. Phase 2's module crafting inherits a ready-made
progression loop — *survive a biome crudely to farm the material that masters it, or buy
mastery from someone who did* — and it costs this phase nothing to preserve the symmetry.
Named here so Phase 2 finds it on purpose rather than by luck.

#### 3c. Distribution across the bands

Band is read off the system's baked `security` value: **High ≥ 60, Low 25–59, Null ≤ 24**
— thresholds chosen to match the three region archetypes the bake already emits (60–95,
25–70, 0–35). A 60+ system inside a Low-band region is possible and *stays High-behaving*:
a safe pocket in rough space is flavor the map's gradient already draws, not a bug, and the
per-system number is the one the hash guards.

Site count and archetype are rolled at bake time (PCG32 in `GenerateUniverse`, like every
other roll — the artifact is the truth, and re-rolling means re-baking):

| Band | 2 sites : 3 sites | `ferrousField` | `irradiatedBelt` | `nebulaPocket` | Grade range |
|---|---|---|---|---|---|
| High (≥ 60) | 70 : 30 | 65 | 25 | 10 (faded) | I–II |
| Low (25–59) | 50 : 50 | 40 | 35 | 25 | II–IV |
| Null (≤ 24) | 30 : 70 | 25 | 40 | 35 | III–V |

Rolls are per-slot with these constraints, each a **bake invariant** in the existing
suite's style:

- Every High-Sec system's first site is a `ferrousField` — the new-player floor.
- At most two sites of one archetype per system in High and Low. **Null may roll triples**:
  a triple-nebula system is a strategic prize, and prizes belong where the shooting is.
- Every High-Sec **region** contains ≥ 1 faded `nebulaPocket` system (ruling 1b).
- Every region's site set covers all three ores at grade ≥ II somewhere.
- Starter systems author their sites by hand, like everything else about them: Vesta-3
  gets `ferrousField` grade II and `irradiatedBelt` grade I — both cycles teachable at
  home, Nebulite deliberately a short trip away.

**Composition** — a site is mostly its archetype's ore, never purely, so a field is worth
working even when it is not what you came for (share of pool units, FC/AC/NB):

| Archetype | High | Low | Null |
|---|---|---|---|
| `ferrousField` | 90 / 10 / 0 | 80 / 15 / 5 | 70 / 20 / 10 |
| `irradiatedBelt` | 25 / 75 / 0 | 15 / 75 / 10 | 10 / 75 / 15 |
| `nebulaPocket` | 15 / 10 / 75 | 5 / 15 / 80 | 5 / 10 / 85 |

High-Sec Nebulite and Astracite exist only at the bottom grades — the taste, not the meal.

#### 3d. Grades, pools, and the regeneration epoch

| Grade | Pool (total units) | Hazard scaling | Found in |
|---|---|---|---|
| I | 4,000 | ×0.5 | High |
| II | 8,000 | ×1.0 | High, Low |
| III | 16,000 | ×1.5 | Low, Null |
| IV | 32,000 | ×2.0 | Low, Null |
| V | 64,000 | ×3.0 | Null |

A solo Miner (§4) eats a grade-II pool in about seven hours; a five-Miner op drains it in
under two — depletion is meant to be *felt*, because a mined-out field is what pushes
fleets outward and staggers the shard across systems.

**Depletion is durable; the world is not.** ADR-016 named "mined-out fields" as the policy
question `Site` anchors would force, and this is the policy: the bake's pools are the
pristine truth, and a **site ledger** at the universe layer — beside ADR-017's station
roster, hashed at the registry level the same way — holds what has been taken since the
last epoch: `(siteAnchorId, orePool remaining)`. A world spinning up on a site reads
bake + ledger and reconstructs the field mid-eaten; a teardown forgets nothing that
matters, because the world never owned the number. Worlds forget, ledgers do not
(ADR-018 D2, applied).

**Regeneration is an epoch, not a trickle.** Every site refills to its full pool on a
fixed cycle — `SITE_REGEN_SECONDS = 86,400`, one day — at a per-site offset staggered
deterministically from its anchor id, so the shard's fields do not all reset at one
minute and the refill is a single ledger write rather than a per-tick drip. Between
epochs, what is gone is gone: "this system is chewed out until tomorrow" is information
a mining corp plans logistics around, and a roaming-anomaly layer (fields that *move*)
is left to the future content system that would own it.

**Visibility:** archetype and grade are bake content — the strategic map and system view
may show them to everyone, like security. The *remaining pool* is presence-gated like
everything live: you learn how eaten a field is by having a ship there (§4d's site
status), which gives scouting a job and keeps ADR-016 §7's "seeing without presence is
intel's territory" intact.

### 4. Mining — a fleet order, because this is a fleet game

#### 4a. The order

The commander is disembodied and commands up to 64 ships at a time; mining here is **not
click-a-rock**, it is ordering a mining wing to work a field. **`OrderKind::Mine = 6`**
appends after `Dock` — same acked stream, same shared validation both halves, same bounce
parity. Its parameter (through the existing `OrderKindParameterName` / `OrderOptions`
machinery, the way Move carries FORMATION) is the **ore filter**: `Any` (default, zero) |
`FerroChroma` | `Astracite` | `Nebulite`.

Validation, in the house grammar:

- The grid's anchor must be a Site — else **`NotAtSite = 17`** ("there is no field here"),
  the `NotAtStation` sentence one noun along. Getting there is the client's job: the MINE
  context action on a site (system view or tactical) feeds Warp, then Mine — the DOCKING
  chip pattern, shown as a MINING chip.
- At least one named ship must be a Miner — else **`NoMinerInOrder = 18`**. Mixed groups
  are *legal and intended*: non-Miner hulls in the order take formation stations around
  the worked cluster and hold — **escorted mining is the null-sec fantasy and it falls
  out of the existing formation solve for free.** Only Miners cycle.
- At least one named Miner must have room — else **`HoldFull = 19`**.
- The site must have ore matching the filter — else the order completes immediately as
  Done (an empty field is not an error, it is news; the toast says so).

#### 4b. Working the field

A site's rocks group into **clusters** (6–12 per field, from the layout seed). An accepted
Mine order assigns its Miners to the richest matching cluster; each Miner runs **mining
cycles** against it: `MINING_CYCLE_SECONDS = 40` (800 ticks), yielding per completed cycle
**12 units of Ferro-Chroma, 8 of Astracite, or 6 of Nebulite** — hardness, not RNG: yield
is deterministic, `min(cycleYield, cluster remaining)`, no draw. Hazards stretch or pause
cycles per §3b. Ore lands in the Miner's hold at cycle completion, and the same tick's
universe-layer apply debits the site ledger — mining writes travel the transfer-bus
apply-point discipline, so per-tick code still never touches a `UniversePos` and the
replay contract extends by one record family instead of forking.

Three exits, all per-ship and all visible:

- **Hold full → that ship stops mining.** The brief's rule, verbatim. The ship goes to
  `Hold` ("stay where you were put"), its roster row and chip read HOLD FULL, and D19's
  event record gets an entry — the away-log's "your Miners filled up at VEI-4 II" is the
  reconnect print's promise kept for industry.
- **Cluster exhausted → the order completes** (Done, with the linger the client needs).
  The *client* feeds the next Mine at the next cluster — route-feeding applied to
  industry, same accepted cost: a disconnected commander's wing stops at the empty
  cluster, and the future AI commander closes that gap for mining the day it closes it
  for routes.
- **All Miners full → the order completes.** The escorts hold with them; hauling home is
  the player's next decision, not an automation.

#### 4c. Rocks are presentation; the site is the state

**No asteroid is a ship-table entity.** The sim tracks the site at
(site, ore, cluster) granularity; the client derives the visible rock field — positions,
sizes, how cracked-open each cluster looks — as a pure function of
`(layoutSeed, pool remaining)`, the same both-halves-derive discipline as
`SolveFormation`. Rocks do not collide (a field is a place you fly *into*, and ADR-015
stays a ship-vs-ship and ship-vs-station rule), do not tick, and cost **zero snapshot
bytes and zero ids**.

This is the load-bearing frugality of the whole phase, so the prices it avoids are worth
stating: per-rock entities would have spent the u16 occupant window U4 already measured
into refusal (30 rocks × 6,250 sites ≈ 187,000 ids against 32,767), pressured the 43-ship
snapshot cap with entries that never move, and put every pebble in the world hash. A
commander orders a wing at a field; the game does not need the pebble to be real, it
needs the *number* to be real.

#### 4d. What replicates

- **`SiteStatus`** joins the summary family (~1 Hz, to viewers of that grid): per-ore
  remaining units, per-cluster remaining fraction (`clusterCount` bytes). Tens of bytes;
  the field visibly hollows out as it is eaten.
- **`CargoStatus`** joins beside it (~1 Hz, owner-only): per owned ship, used litres —
  the HUD's fill bars and the hangar's manifest read this. Owner-only is cargo privacy:
  scanning a stranger's hold is future intel gameplay, not a free byte.
- **The snapshot is untouched.** A per-tick cargo byte on `EntityRecord` would take
  21 → 22 bytes and the ship cap 43 → 41 — onto the asserted floor exactly — for a number
  that changes every 800 ticks. Priced and refused; the summary family exists for
  precisely this cadence (the ADR-017 §5 discipline, applied to the next byte that asked).

Mining state (MINING / HOLD FULL / cluster ETA) rides the existing order-state records —
`etaSeconds` already reports what an order is doing, and a cycle is an order doing it.

### 5. Cargo, hauling, and the wreck rule

#### 5a. Holds are per-hull configuration

Every hull gets a **general hold**; the Miner alone gets an **ore hold** — bigger, ore-only,
the reason a Hauler still matters (a Miner hauls ore badly *by configuration*: its general
hold is a glovebox). Litres, integers, authored in the economy content file (§7):

| Hull | Ore hold | General hold | Fills with FC / AC / NB in |
|---|---|---|---|
| Interceptor | — | 1,500 L | escort, not freight |
| Bomber | — | 3,000 L | |
| Corvette | — | 5,000 L | |
| Frigate | — | 8,000 L | |
| Battleship | — | 24,000 L | |
| Carrier | — | 90,000 L | fleet support, not a freighter |
| **Hauler** | — | **400,000 L** | 1,333 FC / 2,000 AC / 1,600 NB units |
| **Miner** | **120,000 L** | 6,000 L | **22 / 50 / 53 minutes** of cycling |
| Structure, Gate | — | — | stations store in Bays, not hulls |
| Fighter, Cruiser | — | — | reserved, `hasContent` false |

The Miner's fill times are the session heartbeat and they are *derived*, not decorative:
120,000 L ÷ 300 L ÷ 12 units/cycle × 40 s = 22.2 minutes of Ferro-Chroma — a coffee-length
High-Sec loop — stretching to ~50 minutes on the rare ores before hazards, so the valuable
run is also the long exposure. One Hauler carries 3⅓ Miner-loads, which is the convoy
ratio: a three-Miner op fills one Hauler per rotation.

Two deliberate non-couplings, stated so nobody builds them by accident: **cargo never
changes the movement envelope** (a laden Hauler flies like an empty one — mass-feel is
combat-era tuning if it is ever wanted, and the envelope suite's per-class assertions stay
per-class), and **capacity never varies per ship instance** (no rigs, no expanders —
per-ship divergence from class is Phase 2's refit system, and the REFIT tab is already
stubbed waiting for it).

#### 5b. Risk: the wreck rule (forward design — activates with combat)

When combat exists and a ship dies with cargo aboard: **each stack drops 50% (floor) into
a wreck; the rest is destroyed.** The destroyed half is the point — it is the economy's
sink, and it keeps piracy profitable but never *more* profitable than the industry it
preys on (ruling 1d). The wreck is an on-grid scoopable container, finders-keepers, SCOOP
as a context action (the DOCK pattern on a sadder target), `WRECK_LIFETIME_SECONDS = 900`,
and **wrecks are not durable**: a world teardown takes them (ADR-016's "wrecks" question,
answered: worlds forget, and a wreck is the world's). In practice a fresh kill site holds
the killer's ships, so the grid outlives the loot window that matters.

Band changes the *odds of dying*, not the arithmetic of it: High-Sec safety will be the
future security-response design (that is combat's ADR to write), and the drop fraction
stays flat so the loot table never needs a map. Ore in a **docked** hull, like everything
docked, is absolutely safe; ore in a Bay is safer than that.

#### 5c. Docking, the Bay, and the manual transfer

The **Station Bay** is a per-`(PlayerId, station)` ledger at the universe layer — beside
the roster, in the registry hash, durable, private, uncapped. Uncapped for the hangar's
own stated reason: a cap is a strategic knob with no economy pressure behind it yet, and
it is named here so a future scarcity design is a decision rather than a discovery.

**Transfer is manual, and stays manual — that is a ruling, not an omission.** Docking
moves the *ship* into the roster with its manifest intact (ADR-017 §1's record grows the
manifest; repair-by-absence is untouched — cargo is not damage). Moving ore between hull
and Bay is the player's act, on the **CARGO tab** (the station surface's tab family was
built for this — HANGAR's siblings stop being all stubs), stack-wise or TRANSFER ALL.
Three reasons the friction earns its keep: what is aboard when you undock is the *risk
decision* of §5b, made by hand; what is in the Bay is what the refinery may consume (§6),
so the transfer is the commitment of ore to industry; and a docked-with-full-hold Miner
undocking straight back to the field — never touching the Bay — is a legitimate loop the
automation would have foreclosed. The verbs are station commands
(`TransferToBay` / `TransferToShip`), validated against the RosterView + Bay the same
shared way `Undock` is.

### 6. Refining — the station becomes industry

#### 6a. The recipes, priced

Recipes consume Bay ore, produce Bay alloys, on the recipe sheet exactly as briefed —
output 1 unit each. Times are seconds (ticks derive; per-tick tables would rebalance if
the tick changed — the class-table rule):

| Alloy | Tier | Inputs | Time/unit | Units/slot·hr | Ore in → alloy out (L) | Input value | Feeds |
|---|---|---|---|---|---|---|---|
| Ferrocite Plates | T1 | 2 FC | 30 s | 120 | 600 → 400 | 2.0 | hulls, bulkheads |
| Astra-Glass | T1 | 1 AC + 1 NB | 45 s | 80 | 450 → 300 | 5.2 | canopies, lenses |
| Chromite Conduit | T2 | 1 FC + 2 NB | 90 s | 40 | 800 → 550 | 7.0 | thrusters, cooling |
| Quantum Matrix | T2 | 2 AC + 1 FC | 120 s | 30 | 700 → 450 | 5.4 | CPUs, shields |
| Nova-Steel | T3 | 2 AC + 2 FC + 2 NB | 300 s | 12 | 1,500 → 1,000 | 12.4 | capitals, superweapons |

Two derived properties are doing quiet work in that table. **Compression:** every alloy
packs its ore's value into ~⅔ of the volume (1.45–1.56× everywhere — checked, not
approximate), so refining *near the source* and hauling alloys home is always the
logistics-efficient move — which plants industry in dangerous space on purpose, and makes
the alloy convoy the richest gank target per litre in the game. **Throughput:** one
cycling Miner out-supplies one refining slot 4.5:1 on Ferro-Chroma — slots, not ore, are
the bottleneck resource, which is what makes station tiers (below) worth fighting over
and surplus ore worth selling when a market exists. Suggested value premia over input for
future pricing: ×1.35 T1, ×1.5 T2, ×2 T3 (slot-time is what the premium is paying for).

**Alloys are fungible, and that is load-bearing:** no per-unit quality, no instance
state — a Ferrocite Plate is a Ferrocite Plate or the future market has no order book.
Quality variance belongs to Phase 2's blueprints if it belongs anywhere.

#### 6b. Jobs, batches, and the material-efficiency refund

A **refine job** is `(recipe, batchCount)` submitted as a station command against your Bay
at a docked-or-remote station (remote works — focus never gates command, and the hangar
already opens remotely). Inputs debit up front; outputs and the **ME refund** credit at
completion. Jobs run on the shard-global tick at the universe layer — **they run while
you are offline**, because the service is persistent and a refinery that stopped when you
slept would just be a tax on time zones. Completion writes a D19 event-record entry: the
away-log learns to say "1,000 Plates finished at Vesta-3."

Batching is the deterministic economy's answer to yield RNG — **no crit-crafts, no
probabilistic bonus units; the refund is exact and the ledger always adds up:**

| Batch size | Time factor | ME refund (of inputs, floor) |
|---|---|---|
| 1 | ×1.00 | tier ME × 1 unit rounds to 0 — singles waste nothing but earn nothing |
| 10 | ×0.95 | tier ME, floored per material |
| 50 | ×0.90 | tier ME, floored per material |

At a T3 station (ME 10%), a 50-batch of Plates consumes 100 FC, refunds 10 FC at
completion, and takes 22.5 minutes: big batches are capital-efficient and
calendar-cheap, at the cost of locking inputs longer — a real planning choice, all of it
integer.

#### 6c. Station tiers, and who upgrades them

Refineries are **per-station public infrastructure with a tier**, authored T1 by the bake
everywhere (starter-system stations T2, so the on-ramp reaches Quantum Matrix at home).
Per **player** at a station: `slots(tier)` concurrent jobs and a queue of 10 — per-player
rather than shared, because a shared slot pool on a hundreds-of-commanders shard is a
griefable queue, and contention is meant to live at the *field and the trade lane*, not
the job form.

| Tier | Slots/player | Recipes | ME | Job time | Band cap |
|---|---|---|---|---|---|
| T1 | 1 | Tier 1 | 0% | ×1.00 | — |
| T2 | 2 | + Tier 2 | 5% | ×0.95 | **High-Sec stations stop here** |
| T3 | 3 | + Nova-Steel | 10% | ×0.90 | Low, Null |
| T3-C "frontier calibration" | 3 | all | 15% | ×0.85 | **Null only** |

The band caps are the industrial map: **Nova-Steel is physically born in dangerous
space** — no High-Sec station can ever cook it — so endgame industry runs ore *into* the
dark and alloys *out of* it, both legs escortable, both legs interdictable when combat
arrives. Null's calibration ceiling makes the lawless refinery the best refinery, which
is the standing invitation to go live there.

**Upgrades are communal projects, and they are the phase's economic sink.** No player
owns a station (CONSTRUCT is a stubbed tab for a future age); instead any commander
invests alloys into a station's public refinery — contributions ledgered at the universe
layer, project completes, tier rises **for everyone, permanently**:

| Project | Costs (alloys, consumed) | In ore-hours, roughly |
|---|---|---|
| T1 → T2 | 2,500 Ferrocite Plates + 800 Astra-Glass | ~7 Miner-hours + ~31 slot-hours |
| T2 → T3 | 2,000 Chromite Conduit + 1,200 Quantum Matrix | ~14 Miner-hours + ~86 slot-hours |
| T3 → T3-C | 500 Nova-Steel | needs the T3 you just built |

The economy eats its own output before a single gun exists: alloys leave the world
through infrastructure, upgraded stations become regional industry hubs worth defending,
and D19 records who built what — the hook future reputation or sovereignty systems will
be glad someone left. (Pre-combat, this is the **only** sink, and that is stated
honestly in §9's ledger rather than papered over.)

#### 6d. Fuel: designed, named, and deliberately not yet

The brief asks whether refining should burn a specialized station fuel. The answer is
**yes, later, and here is its name so the schema never moves**: `fuelPerJob`, authored
`0` for every recipe today, and **Ionized Slurry** — a vendor/market commodity, hauled
in hulls like anything else — the day a market exists to sell it. Fuel is the *right*
long-term sink and logistics driver (a Null refinery that must be fed makes supply
convoys a standing gameplay loop), but fuel before currency is a resource with no
faucet: there is nothing to buy it with and nowhere to buy it. Reserved-with-a-value is
the house pattern for exactly this (`CombatEngaged`, `resumeToken`), and it costs one
integer field to make the market phase a data patch instead of a schema bump.

### 7. The configuration architecture — balance becomes data, with a hash

Everything above is numbers, and the numbers live in **one authored content file**,
`GameData/Economy/Economy.json`, parsed by the NeuronCore parser inside a pure GameLogic
`bytes → EconomyDef` function (the `UniverseParse` shape: diagnostics with line and
column, fail closed, strict unknown-key rejection), canonicalised into an **`economyHash`**
that joins `universeHash` in the fail-closed handshake — both halves refuse to disagree
about a litre the same way they refuse to disagree about an anchor. This is ADR-012 §D13's
sentence — *"when balance wants to be data (post-MVP), it arrives with a hash, not
before"* — arriving. The class table stays compiled-in; movement is not economy, and the
two tables answer to different suites. **All integers**: litres, seconds, whole
percentages, weights — the parser's exact-int64 guarantee is wasted on nothing.

The shape, in full for one entry of each family (ellipses mark repetition, not gaps —
the real file enumerates all three ores, five alloys, three archetypes, ten hulls):

```jsonc
// Outpost: Frontier -- the economy definition (ADR-024).
// AUTHORED content, hash-guarded like the universe: both halves parse this and
// the handshake refuses a mismatch. Integers only -- litres, seconds, percent.
{
  "ores": [
    { "id": "ferroChroma", "name": "Ferro-Chroma", "unitVolumeLitres": 300, "valueIndexPct": 100 },
    { "id": "astracite",   "name": "Astracite",    "unitVolumeLitres": 200, "valueIndexPct": 220 },
    { "id": "nebulite",    "name": "Nebulite",     "unitVolumeLitres": 250, "valueIndexPct": 300 }
  ],

  "alloys": [
    {
      "id": "ferrocitePlates", "name": "Ferrocite Plates", "tier": 1,
      "unitVolumeLitres": 400, "refineSeconds": 30, "fuelPerJob": 0,
      "inputs": [ { "ore": "ferroChroma", "units": 2 } ]
    },
    {
      "id": "novaSteel", "name": "Nova-Steel", "tier": 3,
      "unitVolumeLitres": 1000, "refineSeconds": 300, "fuelPerJob": 0,
      "inputs": [ { "ore": "astracite", "units": 2 },
                  { "ore": "ferroChroma", "units": 2 },
                  { "ore": "nebulite", "units": 2 } ]
    }
    // ... astraGlass, chromiteConduit, quantumMatrix
  ],

  // Keyed by HullClass name. A hull absent here has no cargo (Structure, Gate,
  // and the two reserved ids). "oreHold" exists only where a hull mines.
  "cargo": {
    "Miner":   { "oreHoldLitres": 120000, "generalHoldLitres": 6000 },
    "Hauler":  { "generalHoldLitres": 400000 },
    "Carrier": { "generalHoldLitres": 90000 }
    // ... Battleship 24000, Frigate 8000, Corvette 5000, Bomber 3000, Interceptor 1500
  },

  "mining": {
    "cycleSeconds": 40,
    "unitsPerCycle": { "ferroChroma": 12, "astracite": 8, "nebulite": 6 }
  },

  "sites": {
    "regenSeconds": 86400,
    "grades": [
      { "grade": 1, "poolUnits": 4000,  "hazardScalePct": 50 },
      { "grade": 2, "poolUnits": 8000,  "hazardScalePct": 100 },
      { "grade": 3, "poolUnits": 16000, "hazardScalePct": 150 },
      { "grade": 4, "poolUnits": 32000, "hazardScalePct": 200 },
      { "grade": 5, "poolUnits": 64000, "hazardScalePct": 300 }
    ],
    "archetypes": [
      {
        "id": "irradiatedBelt", "name": "Irradiated Belt",
        "hazard": { "kind": "radiation", "cycleSlowPerStackPct": 3, "stackDecaySeconds": 30 },
        // FC/AC/NB shares per band, in percent, summing to 100.
        "compositionPct": { "high": [25, 75, 0], "low": [15, 75, 10], "null": [10, 75, 15] }
      }
      // ... ferrousField { hazard none }, nebulaPocket { hazard: sensor + heat }
    ],
    // The bake reads this block; everything else above is runtime balance.
    "distribution": {
      "bandSecurityFloors": { "high": 60, "low": 25 },      // null is the remainder
      "siteCountWeights":   { "high": [70, 30], "low": [50, 50], "null": [30, 70] }, // 2 : 3
      "archetypeWeights":   { "high": [65, 25, 10], "low": [40, 35, 25], "null": [25, 40, 35] },
      "gradeRange":         { "high": [1, 2], "low": [2, 4], "null": [3, 5] }
    }
  },

  "refining": {
    "batches": [ { "units": 1,  "timePct": 100 },
                 { "units": 10, "timePct": 95 },
                 { "units": 50, "timePct": 90 } ],
    "queueDepthPerPlayer": 10,
    "tiers": [
      { "tier": 1, "slotsPerPlayer": 1, "recipeTierCap": 1, "refundPct": 0,  "timePct": 100 },
      { "tier": 2, "slotsPerPlayer": 2, "recipeTierCap": 2, "refundPct": 5,  "timePct": 95 },
      { "tier": 3, "slotsPerPlayer": 3, "recipeTierCap": 3, "refundPct": 10, "timePct": 90 },
      { "tier": 4, "slotsPerPlayer": 3, "recipeTierCap": 3, "refundPct": 15, "timePct": 85,
        "nullSecOnly": true }
    ],
    "bandTierCaps": { "high": 2, "low": 3, "null": 4 },
    "upgradeProjects": [
      { "toTier": 2, "costs": [ { "alloy": "ferrocitePlates", "units": 2500 },
                                { "alloy": "astraGlass", "units": 800 } ] },
      { "toTier": 3, "costs": [ { "alloy": "chromiteConduit", "units": 2000 },
                                { "alloy": "quantumMatrix", "units": 1200 } ] },
      { "toTier": 4, "costs": [ { "alloy": "novaSteel", "units": 500 } ] }
    ]
  }
}
```

Division of custody, so two files never answer one question: the **bake** consumes
`sites.distribution` and emits site anchors into `Frontier.json` (archetype, grade,
field radius, layout seed, starting pools — content, hash-guarded, invariant-checked);
the **runtime** reads everything else. Hull *movement* stays in the class table; hull
*capacity* lives here — the seam between them is the seam between "how it flies" and
"what it is worth," and each side's suite guards its own. The envelope suite grows
economy assertions in the class-table style — shapes, not values: rarer ore is slower to
mine and denser in value, every alloy compresses, higher tiers strictly dominate, refund
never reaches the batch it refunds.

### 8. The wire and the schema, enumerated once

One clustered fail-closed bump when the phase's wire half lands, in the ADR-017 §8
manner (exact final numbering settles at the implementing slice — appends only, and the
enum comments will say which phase landed first, as they already do for warp):

- `OrderKind`: **`Mine = 6`**, with `ORE FILTER` as its parameter through the existing
  options machinery.
- `OrderReason` appends: **`NotAtSite = 17`, `NoMinerInOrder = 18`, `HoldFull = 19`**,
  and the station-command side **`InsufficientMaterials = 20`, `RefineryBusy = 21`,
  `RecipeLocked = 22`**.
- `StationCommand` verbs: **`TransferToBay`, `TransferToShip`, `RefineStart`,
  `RefineCancel`, `ProjectContribute`** — the ADR-017 §3 family, same seq/ack/reasons.
- Summary family: **`SiteStatus`** (per viewed site, ~1 Hz), **`CargoStatus`** (owned
  ships, ~1 Hz), **`BayStatus` / `RefineryStatus`** (per station holding your ore or
  jobs — the `StationRoster` cadence and framing, per-viewer per ADR-022).
- `AnchorKind::Site` baked content arrives under the existing `universeHash`;
  `Economy.json` arrives under the new **`economyHash`** beside it in the handshake.
- **`EntityRecord` is untouched** — no new per-tick byte anywhere in this phase (§4d).
- D19 event kinds: hold-full, site-exhausted, refine-complete, project-complete.

### 9. The arithmetic, checked against itself

The numbers above were derived together; this section is the cross-check, in the open,
so retuning starts from the invariants rather than the values.

- **The session heartbeat.** High-Sec FC loop: warp in, 22 minutes to full, warp home,
  dock, transfer — a ~30-minute casual loop yielding 400 FC → 200 Plates → 6.7 slot-hours
  of T1 refining. The rare-ore loops run ~50 minutes before hazards: risk *and* patience
  price the good ore.
- **Mining out-runs refining 4.5:1** (1,080 FC/h vs 240 FC/slot·h), so slot capacity is
  the scarce industrial resource — tiers, queues and upgrades stay meaningful, surplus
  ore stays liquid for the future market.
- **A T2→T3 project** costs 2,000 Conduit + 1,200 Matrix = 2,000 FC + 4,000 NB + 2,400 AC
  + 1,200 FC in ore ≈ 3 + 7.4 + 3.3 Miner-hours mining plus ~86 T2-slot-hours refining —
  a first real corp project, a week of evenings for a handful of commanders.
- **A capital-scale anchor** (illustrative, for the future CONSTRUCT tab): 8,000
  Nova-Steel + 20,000 Plates + 6,000 Conduit ≈ 62k FC + 16k AC + 28k NB ≈ 131 Miner-hours
  of Null-grade mining plus ~980 slot-hours — call it 28 wall-clock hours across a
  ten-commander wing running full T3-C slots. Weeks-scale as a shared op, which is what
  "capital ship" should cost.
- **Faucets and sinks, phase-honest.** Phase 1 faucets: site regeneration. Phase 1
  sinks: upgrade projects — *only*. The loop is deliberately faucet-heavy while the
  shard bootstraps its industrial base; the real engines of consumption are named and
  staged: destruction + the destroyed-half rule (combat), fuel and fees (market),
  blueprints consuming alloys and ships consuming blueprints (Phase 2). An economy
  review at each of those phases re-runs this ledger.

## What this deliberately does not do, so nobody mistakes it for covered

- **No currency, no market, no NPC vendors.** Value indices anchor future pricing;
  nothing trades yet. Fuel is reserved-at-zero until this lands (§6d).
- **No combat activation.** Wreck rules, drop fractions, hazard damage — forward design,
  written now, inert until damage exists (§1, §5b).
- **No Phase-2 crafting.** Alloys are the last stop; blueprints, modules, and the
  hazard-counter progression (§3b) are the next design session's inheritance.
- **No per-rock entities, no rock collision** (§4c) — and no mining from non-Miner
  hulls: one hull mines, by configuration, until refit exists.
- **No Bay caps and no hangar caps** — the same named future scarcity knob (§5c).
- **No auto-haul and no AI commander.** Full Miners hold; disconnected wings stop at the
  empty cluster — the same accepted gap as route-halt, closed by the same future feature.
- **No player-owned stations and no sovereignty.** Upgrade projects are communal;
  ownership arrives with whatever CONSTRUCT becomes.
- **No jettison, no ship-to-ship transfer in space, no ore compression module.** Named
  so their absence is a decision; each is a small design with a real abuse surface
  (free floating containers, combat resupply, value-density collapse) deserving its own
  paragraph when wanted.
- **No roaming anomalies / moving sites.** The 2–3 baked sites are the whole map of a
  system's ore until a content system owns wandering riches (§3d).

## Alternatives rejected

- **Asteroids as ship-table entities** — spends the id window U4 already measured into
  refusal, pressures the snapshot cap with immobile entries, and hashes pebbles. The
  site-granular state + derived-presentation split keeps every guarantee at a fraction
  of the cost. Rejected without regret (§4c).
- **Runtime-random ore spawns** — the universe's own argument (ADR-016 §2): unreviewable,
  uncuratable, and a weaker hash story. Baked sites + a durable ledger + a deterministic
  epoch give live-feeling scarcity out of committed content. Rejected.
- **Per-click / per-rock mining** — the commander is disembodied and fleets are the
  unit of intent; a fleet order with deterministic cluster work is this game's grammar.
  Rejected as genre habit, not design.
- **Probabilistic refining yield (crit crafts)** — RNG in an economy ledger means two
  replays of the same jobs disagree with every intuition an integer economy builds. The
  deterministic ME refund gives the same *feeling* (better stations pay better) with
  exact books. Rejected.
- **Shared station slot pools** — first-come queues on a shard are a griefing surface
  and a support ticket. Per-player slots scale with the population by construction.
  Rejected.
- **100% loot drop** (the brief's letter) — makes destruction a transfer instead of a
  sink and piracy strictly dominant; ruled to 50/50 in §1d. Rejected with the arithmetic
  on the page.
- **Auto-transfer on dock** — erases the risk decision, the industry commitment, and
  the dock-and-return loop the manual step preserves (§5c). Rejected; TRANSFER ALL keeps
  the convenience without deleting the choice.
- **Mobile refineries (refine aboard ship)** — collapses the haul-in/haul-out loop that
  plants industry and its defense in space, and unemploys the Hauler the compression
  ratio just hired. Revisitable someday as a capital-class facility with worse ME;
  rejected as the baseline.
- **Fees/fuel as launch-day sinks** — sinks that charge a currency that does not exist.
  Staged behind the market instead (§6d). Rejected for now.

## Consequences

- **Delivery, sketched** (a real build order is its own document if this is accepted —
  the ADR-016/017 pattern): **E1** bake sites + `Economy.json` + `EconomyDef` + hashes +
  invariants; **E2** the Mine order, cycles, the site ledger, replay coverage; **E3**
  cargo, the Bay, transfers, `SiteStatus`/`CargoStatus`, the wire cluster; **E4** refine
  jobs, tiers, projects, and the two screens. E1/E2 are headless-provable end to end in
  the `selfTest` manner before any screen exists.
- **Named deliverables:** the **CARGO** and **REFINERY** tab prints (the P1 pattern —
  designed and agreed before their slices build); ore/alloy icons; the site field's
  visual treatment (the Nebula pass parameter set for pockets; rock meshes or impostors
  for fields).
- **New constants join the envelope suite's guardianship** as table data: cycle seconds,
  unit volumes, hold litres, pool sizes, regen epoch, refund percentages — with
  shape-assertions per §7.
- **Proposed risk-register rows** (for the owner to promote): *faucet-without-sink
  inflation* if combat/market phases slip far behind this one (mitigation: the upgrade
  projects were sized to absorb early surplus; re-run §9's ledger at each phase); *site
  contention griefing* in High-Sec (fields are finite and shared; mitigation: epoch
  regen plus grade caps bound the damage — watch it in play).
- **Two file-registry entries** when implementation starts (`EconomyDef.h`,
  `EconomyParse.{h,cpp}` or folded per ADR-013's judgment), recorded in both registries
  per the standing rule.
- ADR-016 §3/§4-gaps, ADR-017 §1/§6 and ADR-012 §D13 take amendment notes pointing here
  **if accepted**; the README's supersession list grows its line at the same moment.

## The six the review left open, answered *(owner rulings, 2026-08-20)*

The questions were put to the owner the same day, each with a recommendation, and all six
were ruled. Every proposal stood, so no number in the body moved — what changed is that each
of these is now a **decision with its reason recorded** rather than a default that survived
by silence, which is the difference the P1 §3 → ADR-017 §6a pattern exists to make.

**R1 — The wreck split is a flat 50/50, in every band** (§1d, §5b stand as written). One
number, no banded loot table; the victim always loses more than the attacker gains, so
destruction is a sink everywhere. Null-drops-more was declined because it would weaken the
sink exactly where the most valuable cargo dies; victim-lighter 40/60 was declined because
piracy should stay a profession, just never a dominant one.

**R2 — High-Sec keeps its faded Nebula Pockets** (§1b, §3c stand). Tier 1 stays craftable
in every band from day one, which is load-bearing precisely while no market exists to buy
Nebulite from; doing Nebulite *well* still means Low-Sec. Cutting them — Astra-Glass as the
first forced trade route — is the named revisit for when the market phase makes "buy it
instead" a sentence a new player can actually act on.

**R3 — Regeneration is daily, staggered, in every band** (§3d stands). One number;
"chewed out until tomorrow" is logistics a corp can plan around. The faster Null cadence
(12 h) was declined for now and is the named revisit when war consumption exists to feed;
the continuous trickle was declined for blurring the one clean signal depletion sends.

**R4 — Mine orders take mixed fleets** (§4a stands). Escorts hold formation around the
worked cluster through the existing solve, for free. Miners-only was declined: the escorted
op is the picture this economy is drawing, and requiring two orders to draw it is ceremony.

**R5 — A full Miner holds at the cluster** (§4b stands). No movement the player did not
order — and no parking ships at the warp-in point, the field's most predictable ambush
spot. Auto-withdraw was declined for splitting the wing to move ships somewhere *less*
safe.

**R6 — The upgrade project costs stand as sized** (§6c stands). ~31 slot-hours to a T2 and
~86 to a T3 is a first real corp project — reachable, not trivial. Halving (faster
bootstrap) and doubling (rarer T3s) were both declined; the costs are table data in
`Economy.json` either way, so retuning after real play is an edit, not a redesign.
