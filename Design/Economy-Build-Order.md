# Economy Build Order — the Mining and Refining Phase

**Status:** Session output 2026-08-20 · **E1a and E1b are built** (2026-08-20); the rest is not. The design this plan delivers
is [ADR-024](ADR/ADR-024-mining-economy.md), accepted 2026-08-20 with nine owner rulings;
where this document and that one disagree, the **ADR wins on *what*** and this one on
***when***. Two refinements of the ADR's own delivery sketch are recorded in the sequencing
rationale below rather than left as a silent divergence: **E1 splits** into a content half
and a bake half, and **the screens leave E4** to become E5.

**Where it sits:** after the universe and station phases, and it does not interleave with
them. It consumes everything both built — the bake and its anchor table (U1), the world
registry and its spin-up/teardown (U2), warp as the way a fleet reaches a site (U3a/U4), the
transfer bus and the station roster (T1), the summary family's frame and the per-client
sender (T2/A13). Nothing in this phase is blocked on the screen work those phases still owe:
E1a–E4 are headless-provable, which is the same split that let T1 land while T2's client half
waited for a GPU and a person.

**What rides on this plan.** [ADR-018](ADR/ADR-018-scaling-baseline.md)'s baseline still
governs: durable state lives at the universe layer and worlds forget (D2) — the site ledger
and the Bay are that rule's newest residents; the transfer bus's `(applyTick, transferId)`
order (D17) carries mining's ledger writes; the event record (D19) gains four kinds; and the
economy's numbers join the fail-closed content posture rather than the compiled table
(ADR-012 §D13, cashed in by E1a). **One new design deliverable blocks this phase** — the
persistence ADR — and it blocks **E2**, not E1: see the deliverables section for why the
ADR's own "blocks E1" was corrected.

The rules are the MVP build order's, unchanged: each slice is independently testable, lands
green (`Tests/` + `selfTest` where applicable), is sized at "a few days" or less, and later
slices assume earlier ones. Landed slices carry a **Built** line naming what is in the tree
and what is still owed. Test placement follows the Dependency Map: content, sim and ledger
truth in `GameLogicTests`, wire in `NeuronCoreTests`/`NeuronServerTests`, screens in
`NeuronClientTests`, and anything needing the real loopback in `selfTest`.

Milestones: **G0** — *the headless mining loop* (warp to a site, mine until the hold fills,
dock, transfer to the Bay — over the real loopback) · **G1** — *the first alloy* (a refine
job runs to completion against a Bay and survives a restart) · **G2** — *the loop on screen*
(watch a field hollow out, compose a haul, queue a batch).

*(**G** for goods. Every letter this corpus already spends is taken — S the MVP slices, U the
universe, T the station, W and H and M the milestones, R the risks, A the action register,
D the decisions, P the prints, F the corpus figures — so the phase's slices take **E** and
its milestones take **G**, and neither collides with anything a reader might follow.)*

---

### E1a — The economy content layer
The numbers stop being prose and become authored, hash-guarded content.
`GameData/Economy/Economy.json` is written to ADR-024 §7's shape — ores, alloys, per-hull
cargo, mining, sites (grades, archetypes, distribution), refining (batches, tiers, band
caps, upgrade projects) — **integers throughout**, since litres, seconds and whole
percentages are what the parser's exact-`int64` guarantee exists for. `GameLogic/EconomyDef.h`
holds the parsed structures; `GameLogic/EconomyParse.{h,cpp}` is the pure
`bytes → EconomyDef` function in `ParseUniverse`'s exact shape — takes bytes rather than a
path so GameLogic stays free of the OS, never throws, never asserts, and reports **every**
problem with a line and a JSON path rather than the first. `ComputeEconomyHash` canonicalises
the parsed content the way `ComputeUniverseHash` does, so comments, whitespace and key order
never move it and a changed number always does. `Outpost/EconomyLoad.{h,cpp}` opens the file
and computes the hash, mirroring `UniverseLoad`; `Outpost.json` gains
`"economy": { "definition": "GameData/Economy/Economy.json" }`; `CopyGameData` picks up the
new folder so a fresh clone still presses F5.

**The handshake takes one number, not two.** `ServerDesc::contentHash` is mixed —
`contentHash = Mix(universeHash, economyHash)` — so an economy mismatch is refused by the
machinery that already refuses a universe mismatch, with **no schema bump and no wire field**.
The cost is that a mixed hash cannot say *which* file differs, so both numbers are logged
separately at boot; that is a log line against a wire field, which is the trade this tree
makes every time it is offered.

**Accept:** `GameLogicTests` green on a suite that runs **against the committed file**, not a
synthesised one (U1's lesson, and the only reason its two invisible bugs were caught):
referential integrity — every recipe input names a real ore, every `cargo` key names a real
`HullClass`, and no class with `hasContent == false` carries cargo; sums — every archetype's
per-band composition and every distribution weight totals 100; ranges — grade ids I–V, band
floors ordered, tier caps inside the tier list. Then the **shape assertions ADR-024 §7
promised**, written as shapes so retuning is not rewriting tests: rarer ore is slower to mine
and denser in value; every alloy compresses (output litres < input litres); higher refinery
tiers strictly dominate on slots, refund and time; no batch refund ever reaches the batch that
earns it. Hash stability: reordered keys, added comments and reflowed whitespace produce the
same `economyHash`, and a single changed integer does not. Diagnostics: a malformed corpus
reports *all* of its faults in one pass, each with a line and a path. Parse + hash of the
committed file **measured in Release and recorded here** (ADR-018 D11), and a headless boot
logs both hashes.

**Built (E1a, 2026-08-20).** The economy stopped being prose.
`GameData/Economy/Economy.json` is the authored content; `GameLogic/EconomyDef.h/.cpp`
holds what it parses into plus `ComputeEconomyHash` and `MixContentHashes`;
`GameLogic/EconomyParse.h/.cpp` is the pure `bytes → EconomyDef` function in
`ParseUniverse`'s shape; `Outpost/EconomyLoad.h/.cpp` opens the file the way
`UniverseLoad` does; and `Tests/GameLogicTests/EconomyParseTests.cpp` is the suite.
**`economyHash 0b07707ec843431d`.** *(E1a measured `162c9e8874ee3435`; E1b corrected the field-radius range against the real grid bound, and a changed litre moving the hash is the property the hash exists to have.)* The handshake carries
`Mix(universeHash, economyHash)` through the existing `contentHash` — no wire field, no
schema bump — and the boot log states all three numbers, because a mixed hash cannot say
which file drifted.

**One design decision paid for itself immediately.** Everything parses into storage
**indexed by its taxonomy** — ore, alloy, archetype, grade and tier are enum-indexed arrays,
recipe inputs and project costs are arrays indexed by ore and alloy with zero meaning
"not required" — so hash order-independence is a property of the *shape* rather than of
remembering to sort. Reordering the ore list, stripping every comment and reflowing the
whitespace all leave the hash where it was; changing one litre moves it. The one place a
list survives is `refining.batches`, and it is sorted at parse time for the same reason.

**Two defects were caught before the gating toolchain saw them**, both by looking rather
than by luck. `.gitattributes` says `* text=auto`, so the committed file is LF here and CRLF
in a Windows working tree — and the suite's fixtures search for **multi-line** snippets, so
it would have passed where it was written and failed where it gates. The test reader
normalises line endings out, and the mirror below exercises the CRLF path. And
`Assert::AreEqual` on a `std::uint8_t` is a compile error under CppUnitTest in a corpus with
no `ToString` specialisation for narrow types; that assertion is widened to `unsigned`, with
the reason written beside it.

**What was verified, and how.** GameLogic's economy files compile clean under **clang 18 on
Linux** — the same cross-build route ADR-015's collision work was first proven on — with
**clang-tidy clean** against the repository config, and the source guards (R2 prefixes and
suffixes, repo-wide file-name uniqueness, no clock or unseeded randomness in GameLogic,
no namespace-scope constant declared twice) run by hand and green. Two scratch harnesses ran
the real content through the real parser: the **invariants harness** (parse, hash, and every
ADR-024 §7 shape assertion) and a **mirror of the MSVC suite** (all seven refusal fixtures,
the three-faults-in-one-pass case, and the hash properties including the CRLF variant), both
**green, 0 failures**. Every new file is registered in the `.vcxproj` *and* the `.filters`,
and in both file-name registries.

**What is still owed, and is not a small print.** **MSVC has not built this and
`GameLogicTests` has not run** — there is no Windows toolchain in the environment this was
written in, so CI's Debug and Release legs are the first real gate, exactly as AGENTS.md §6
means it. `Outpost/Main.cpp`, `AppConfig` and `EconomyLoad` are **not compile-verified at
all**: they are Windows-only translation units, reviewed by reading the diff and no more than
that. And the Accept's **Release parse-and-hash measurement is not taken** — it needs a
Release boot on Windows — so that one acceptance item stays open until CI or an owner run
reports the number.


### E1b — Sites in the bake, and the epoch that moves them
`AnchorKind::Site` stops being reserved — it already holds value 3 in `Universe.h`, so this
cashes in a comment rather than renumbering a wire value. `GenerateUniverse` reads
`sites.distribution` from `EconomyDef` and bakes 2–3 site anchors per system: archetype and
grade rolled per band, an authored **orbit ring** radius between the planet orbits, field
radius, layout seed, and per-ore starting pools. **The bearing is not baked** (ADR-024 §3a,
ruling R7): `SiteEpochPlacement(anchorId, epochIndex)` is a pure, integer-only GameLogic
function — PCG32 seeded from the pair, no floats, the placement arithmetic the bake already
uses — returning the site's origin on its ring, its warp-in point on the field edge, and its
facing. Both halves call it and get the same answer, so the map can draw today's field
without asking the server where it is. Sites author a **wide `arrivalSpreadRadiusCm`** derived
from the field radius (the arc ADR-024 §3a asks for, against the 1,200 m point that station
and gate anchors carry), which is the half of the anti-camp design that actually bites.
The 2,500-system file is **re-baked and committed**.

**Accept:** same seed + config ⇒ byte-identical file, run twice in CI, exactly as U1's does.
The invariants suite extends and runs against the *committed* file: 2–3 sites per system with
the band's count distribution inside tolerance; every High-Sec system's first site a ferrous
field; at most two of an archetype per system in High and Low, triples permitted only in Null;
every region covering all three ores at grade ≥ II; **every High-Sec region holding at least
one faded nebula pocket** (ruling R2's guarantee, made measurable); starter systems carrying
their hand-authored pair. Two budget invariants, because U4's occupant-block refusal is the
precedent: the **anchor id total stays inside the u16 window** with the number recorded here
(~18,618 + ~6,250), and **sites author zero occupants**, so the 32,767-id occupant window is
untouched — rocks are not entities and this is the invariant that says so. And the one that
only exists because the bearing moves: **field radius plus warp-in standoff sits inside the
40 km grid bound for every epoch bearing**, proven over a sweep of epoch indices rather than
for the bearing that happened to bake. `SiteEpochPlacement` is asserted deterministic across
two runs and identical between a GameLogic call and the client's own. Parse + hash of the
grown file **re-measured in Release** against D11's ~1 s ceiling. `UniverseGenTests`'
expected `universeHash` is updated in the same commit — this is a **fail-closed content
event**, and both halves take it together.

**Built (E1b, 2026-08-20).** `AnchorKind::Site` stopped being reserved, and the universe
grew **6,223 mining fields**. `GameLogic/SiteEpoch.h/.cpp` is where a field is *today* --
`SiteEpochIndex` and `SiteEpochPlacement`, pure and integer-only and called by both halves;
`FixedAngle.h` carries the fixed-point sine table `UniverseGen.cpp` kept privately until a
second, runtime consumer arrived; `Anchor` grew a `SiteSpec`; the writer, the parser and
`ComputeUniverseHash` all learned it; and `Tests/GameLogicTests/UniverseGenTests.cpp` gained
a `UniverseSiteTests` class of twelve.
**`universeHash ad9555dd776008a6`**, 18.93 MB, 24,841 anchors (3,356 station, 9,262 planet,
6,000 gate, 6,223 site) with the top anchor id at 24,841 of the u16 window's 65,535.

**The re-bake is purely additive, which is the part that made it reviewable.** `git` reports
**180,467 lines added and zero removed**: not one station, planet or gate anchor moved, no
occupant block shifted, and no name, position, security value or gate edge changed. Two
decisions bought that. Sites are appended **after** every other anchor already has its id --
the same argument ADR-016 §10 used for appending `HullClass::Gate`, run against a 15 MB file
-- and every site roll comes from a **per-system stream of its own**, so the main sequence
that draws names and orbits is never advanced by a site. A site also **authors no occupants**,
so ~6,250 anchors joined without going near the 32,767-id window U4 measured into refusal.

**Three things the implementation found, each now corrected at its source.** The ADR's
field-radius range **did not fit the grid**: it said 8-15 km against "the 40 km grid bound",
but a grid is 40 km *across* and `GRID_HALF_EXTENT_METRES` is 20,000, so at 15 km the field,
its standoff and the wide arrival arc came to 23 km. The range is now 5-12 km, worst case
18,800 m, and the arithmetic sits beside the numbers in `Economy.json`. Two authored
**guarantees contradicted each other**: coverage asks every region for all three ores at
grade >= II, while ruling 1b caps a High-Sec nebula pocket at grade I -- and in High-Sec only
a pocket carries Nebulite, so an all-High region could never satisfy both. The archetype's cap
now beats the coverage floor, and the bake **checks** coverage and refuses rather than shipping
a region that cannot supply an ore. And the faded-pocket repair **ate the new-player floor**:
converting the first convertible site in a region took High-Sec systems' first site, which
§3c reserves for the hazard-free archetype. Six regions lost it before the repair learned to
skip slot one; a test now pins it.

Two smaller things fell out. The universe file crossed `JsonLimits`' **16 MB default**, so
`ParseUniverse` now passes its own 64 MB cap -- which is what ADR-012 §C9 meant by content
setting its own limit, arriving the first time it mattered. And `GenerateUniverse` gained a
second parameter, the economy's site block, with a **guard that bakes no sites when no site
content is supplied** -- which is what lets `RegistryTests` and `selfTest` pass `SitesInfo{}`
instead of carrying an economy they have no use for.

**What was verified, and how.** The whole GameLogic path compiles clean under **clang 18 on
Linux** and is **clang-tidy clean**; the four source guards that apply (repo-wide file-name
uniqueness, R2 prefixes and suffixes, no clock or unseeded randomness in GameLogic, no
constant declared in two headers) were run by hand and are green; and every new file is in the
`.vcxproj`, the `.filters` and both file-name registries. The real `UniverseGenTests.cpp` was
compiled against a CppUnitTest shim and run: **32 test methods, 0 failures** -- the twelve new
ones plus the twenty that already existed, which now run against universes that contain sites.
A separate bake harness ran the committed recipe end to end: bake, write, parse back, hash,
and every ADR-024 §3 guarantee checked against the *generated* content, all green, and the
same recipe twice produced byte-identical output.

**What is still owed.** **MSVC has not built this and `GameLogicTests` has not run** -- CI's
Debug and Release legs are the first real gate, as AGENTS.md §6 means it. `Outpost/UniverseBake.cpp`
and `Outpost/SelfTest.cpp` are Windows-only translation units, reviewed by reading the diff and
no more. And the parse figure the accept wants is a **Release MSVC** number: what exists is
**185-220 ms on Linux clang -O2** for the 18.93 MB file, against U1's 167 ms Release for
14.2 MB, so the ~1 s ceiling (ADR-018 D11) looks comfortable and **no per-region content split
is owed** -- but that sentence needs CI's number before it is a measurement rather than an
extrapolation.


### E2 — Mining in the sim, and the site ledger
**Gated on the persistence ADR** (below): the site ledger is this tree's first durable state,
and its shape is that ADR's to decide.

`OrderKind::Mine = 6` joins the vocabulary with the **ore filter** as its parameter, offered
through the existing `OrderKinds`/`OrderOptions` seam so the command surface names it without
the engine knowing what an ore is (ADR-014 §2b). Shared validation appends
`NotAtSite = 17`, `NoMinerInOrder = 18`, `HoldFull = 19`, and the check-order contract extends
with its parity matrix. A site's pool resolves into 6–12 **clusters** from the layout seed; an
accepted order assigns its Miners to the richest matching cluster and runs **40-second cycles**
yielding 12/8/6 units — `min(cycleYield, cluster remaining)`, deterministic, **no RNG draw**.
Hazards land in their Phase-1 halves only: radiation stacks stretching cycle time and decaying
outside the field, the nebula's sensor-dampening factor as a replicated number the relevance
seam reads, and the heat lockout. Ore credits the hold and debits the **site ledger** at the
universe layer through the transfer bus's own apply point, so per-tick code still never touches
a `UniversePos` and the ledger folds into `WorldRegistry::Hash` beside the roster. The three
exits are per-ship and visible: hold full → `Hold` at the cluster with an event-record entry
(ruling R5), cluster exhausted → `Done` with the linger a client ghost needs, all Miners full →
the order completes. Escorts in a mixed order take formation stations around the worked cluster
and hold (ruling R4) — the existing solve, no new machinery. The **epoch** applies at world
spin-up: refill, re-bearing, reshuffle, never under a live grid.

**Accept:** `GameLogicTests`: double-run bit-identity over a scenario mixing cycles, a hold
filling, a cluster exhausting and an epoch crossing; the ledger in the registry hash **across a
teardown and recreate of the site's grid**, which is "worlds forget, ledgers do not" as a test
rather than a sentence; hold-full exact at the litre, with the ore whose unit volume does not
divide the hold proving the boundary; yield deterministic with the RNG untouched (a draw here
would desync a replay, so the test asserts the generator's state is unmoved); the validation
parity matrix over all three new reasons; **a watched site does not re-form** — a grid with a
viewer crosses an epoch boundary and its rocks and pools are unchanged until presence leaves;
and the replay contract extended over the mining records in the transfer log.

### E3 — Cargo, the Bay, and the wire cluster · 🏁 G0
Ships carry manifests; the station roster's record grows one (ADR-017 §1 as amended — cargo is
not damage, so repair-by-absence is untouched); the **Station Bay** joins the universe layer as
a per-`(PlayerId, station)` ledger in the registry hash. `TransferToBay` and `TransferToShip`
join the `StationCommand` family on the acked stream, validated by the shared pure function
over the RosterView plus the Bay — **manual, both directions** (ruling from ADR-024 §5c: the
transfer is the risk decision and the commitment of ore to industry). The wire cluster lands
in one fail-closed bump: the two verbs, reasons `InsufficientMaterials = 20`,
`RefineryBusy = 21`, `RecipeLocked = 22` (the last two numbered now and returned by E4), and
the summary family gains **`SiteStatus`** (per viewed site, ~1 Hz — per-ore remaining and
per-cluster fractions), **`CargoStatus`** (owner-only, ~1 Hz) and **`BayStatus`**, all through
T2's per-client sender. **`EntityRecord` is not touched**, and a test asserts it: a per-tick
cargo byte would take the record 21 → 22 and the ship cap 43 → 41, onto `Snapshot.h`'s asserted
floor exactly, which is the arithmetic ADR-024 §4d refused in advance.

**Accept 🏁 G0:** `selfTest` drives the whole loop headless over real QUIC loopback — a Miner
wing warps to a site, mines until the hold fills and stops on its own, warps to the station,
docks, and its ore moves into the Bay by command; the Bay survives the station grid's teardown;
`SiteStatus` shows the field measurably emptier than it started; a second commander's
`CargoStatus` and `BayStatus` never reach the first (privacy as a testable property, the way
T2's roster privacy was); the schema hash refuses a client built against the previous cluster.

### E4 — Refining, tiers, and the projects · 🏁 G1
Refine jobs `(recipe, batchCount)` submitted as station commands against a Bay — **at any
station holding your ore, viewed or not**, because focus never gates command. Inputs debit at
submission, outputs and the deterministic **ME refund** credit at completion, floored per
material: no crit-crafts, no probabilistic bonus units, the ledger always adding up. Jobs run
on the shard-global tick at the universe layer and **continue while the commander is offline**,
completing into the event record so the away-log can say what finished. Per-player slots and a
queue of ten; station tiers with their band caps, so **no High-Sec station can ever cook
Nova-Steel**; and communal **upgrade projects** — alloys contributed, ledgered, consumed, the
tier rising permanently for everyone with D19 recording who built it. `fuelPerJob` is read and
honoured, and is zero in the content until a market exists to sell Ionized Slurry.

**Accept 🏁 G1:** `GameLogicTests`: a batch's arithmetic exact at every batch size, refund
floors included, with a property test that inputs consumed minus refund always equals the
recipe's rate — the books balance or the test fails; band caps refusing Nova-Steel at a
High-Sec station with `RecipeLocked`; slot exhaustion returning `RefineryBusy` and the queue
draining in order; a project completing exactly once when two commanders contribute its last
units in the same tick; the whole registry — rosters, Bays, ledgers, jobs, projects —
**round-tripping through the persistence layer and reproducing its hash**, which is G1's real
claim: a refinery that stops when the shard restarts is the demo this phase exists not to be.

### E5 — The two screens
The station surface's **CARGO** and **REFINERY** tabs, built to their prints (below): the hull
manifest beside the Bay with stack-wise and TRANSFER ALL moves, the recipe list with its
locked rows explained rather than hidden, the batch picker, the slot and queue state, and the
project board with its contribution history. On the tactical side, the site field itself — the
Nebula pass parameter set for pockets, rock impostors for fields, clusters visibly hollowing as
`SiteStatus` reports them — plus the MINE context action and its MINING chip, HOLD FULL on the
roster strip, and the ore/alloy icons.

**Accept 🏁 G2:** the owner's loop in one sitting — pick a field off the system view, mine it
until a hold fills, watch the cluster hollow, haul home, dock, move ore into the Bay, queue a
batch of Plates, and come back to it finished; visual checkpoint against both prints; a Null
pocket's dampening legible as a thing happening to *you* rather than a number in a strip.

---

## Content & design deliverables (not slices — tracked so they cannot be quietly dropped)

- ~~**D-P1 — The persistence ADR.**~~ **Delivered 2026-08-20 (proposed, awaiting owner
  review):** [ADR-025](ADR/ADR-025-persistence.md). Journal format, snapshot cadence, crash
  recovery and the reload proof, for the engine-owned journal ADR-024 §7a ruled on. **It blocks
  E2**, not E1 — ADR-024 §7a said "blocks E1", corrected there and here: E1a is a read-only
  content file and E1b is authored content, so neither has durable state to persist; the first
  durable thing in this phase is E2's site ledger. Three of its rulings reach into E2 directly:
  the durable line is **identity and location, never intention** (so a reloaded fleet holds
  position with an empty queue); the ledger's proof is a separate **`DurableHash()`**, not
  `WorldRegistry::Hash()`, which folds the order queues E2 is about to write; and journal
  records are **outcomes**, so E2 files "this pool is now N" rather than "a cycle completed".
- **D-P2 — The CARGO tab print.** The hull-and-Bay transfer surface, in the P1 pattern:
  designed and agreed **before E5 builds**, because retrofitting a screen design after the
  screen exists is how the corpus stops being the governing artefact.
- **D-P3 — The REFINERY tab print.** Recipes, batches, slots, queue and the project board.
  Same clause.
- **D-C1 — Ore and alloy icons**, in the tactical icon system's families, plus the three
  archetype glyphs the strategic and system maps need.
- **D-C2 — The site field's visual treatment**: the Nebula pass parameter set for pockets, and
  rock meshes or impostors for fields — the first content since `Stargate.obj`, and the one
  that has to look like a place rather than a number.
- **D-C3 — Mining and refining audio**, after P2 gives the dock cues their bank shape.
  Deliberately last, like P2 and D4 before it.

## Sequencing rationale

- **E1 splits, and the content half goes first.** The ADR sketched one slice; the two halves
  have a hard dependency direction and very different blast radii. The bake *consumes*
  `sites.distribution`, so the parse layer has to exist before the bake can read it — and E1b
  rewrites ~15 MB of committed content and moves `universeHash`, which every client takes at
  once. E1a is a few new files that nothing else can proceed without. Prove the road, then
  drive the heavy load over it.
- **The screens leave E4 to become E5**, matching the sim/wire/screen seam this repo already
  respects and the wall S5, U3b and T2 are all standing at: everything device-free lands and
  proves itself while the screen work waits for a GPU and a person. It also keeps G1 — the
  claim that an economy survives a restart — from being gated on a tab existing.
- **Persistence before E2, not before E1.** Stated above and corrected in the ADR; the point
  is that the gate is the *first durable state*, and that is the site ledger.
- **G0 at E3 rather than E2**, the same call H0 made at T2: a loop is a milestone when it runs
  over the real transport end to end, not when its sim half is correct in a test.
- **Nothing interleaves with the universe or station phases.** Both are past their sim halves
  and what they owe is screen work; this phase consumes their sim and adds no new claim on
  their client halves.
