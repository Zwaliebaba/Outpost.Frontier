# Economy Build Order — the Mining and Refining Phase

**Status:** Session output 2026-08-20 · **E1a, E1b, E2 and E3 are built and green in CI**
(run 161, commit `93956dc`, 2026-08-21 — 717 tests, `self test: PASSED` with all thirteen
🏁 G0 checks); **E4a is the next slice**, and the rest is not built. The design this plan
delivers is [ADR-024](ADR/ADR-024-mining-economy.md), accepted 2026-08-20 with nine owner
rulings, and [ADR-025](ADR/ADR-025-persistence.md) for the durable half; where this document
and those disagree, the **ADRs win on *what*** and this one on
***when***. Three refinements of the ADR's own delivery sketch are recorded in the sequencing
rationale below rather than left as a silent divergence: **E1 splits** into a content half
and a bake half, **the screens leave E4** to become E5, and **E4 splits** into the durable
store (E4a) and the refining runtime (E4b).

**Where it sits:** after the universe and station phases, and it does not interleave with
them. It consumes everything both built — the bake and its anchor table (U1), the world
registry and its spin-up/teardown (U2), warp as the way a fleet reaches a site (U3a/U4), the
transfer bus and the station roster (T1), the summary family's frame and the per-client
sender (T2/A13). Nothing in this phase is blocked on the screen work those phases still owe:
E1a–E4b are headless-provable, which is the same split that let T1 land while T2's client half
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
`"economy": { "definition": "GameData/Economy/Economy.json" }`; the post-build content copy
takes the whole tree, so the new folder arrives with it and a fresh clone still presses F5.

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

**CI has reported, and E1a's accept is met (run 150, 2026-08-20).** This slice was written
with no Windows toolchain in the environment, so its first record here said plainly that MSVC
had not built it and the suite had not run. **Both legs are green now**, and the two items
that were owed are closed:

- **The suite runs and passes.** 650 tests across the four assemblies, Debug and Release
  alike, against 593 before the economy phase — `EconomyParseTests` among them, so the CRLF
  and narrow-`AreEqual` defects this slice fixed by prediction were fixed correctly rather
  than plausibly.
- **The economy parses in 0 ms**, which is the honest number for a 12 KB file and the reason
  §7's custody split costs the boot nothing.

And the three-hash boot line works exactly as designed, which was the whole argument for
mixing rather than adding a wire field:

```
content: universe ad9555dd776008a6, economy 0b07707ec843431d, mixed 1965b853a23a5115
```

A mismatch is refused by machinery that already existed, and the log still names *which* file
drifted.

*(E1a's own first CI run, 145, shows as **cancelled** rather than green, and the cause is worth
recording because it repeated: this workflow runs one build per branch, so pushing the next
commit kills the run in flight. A green tick for a slice therefore has to be waited for, not
assumed from the absence of a red one.)*


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

**What CI found, and what it has not said yet.** The first run refused E1b **before a line
was compiled**, and the guard was right to: `FixedAngle.h`, `SiteEpoch.h` and `SiteEpoch.cpp`
all name `UniversePos`, and the determinism guard treats every GameLogic file as per-tick code
unless its job *is* the universe (ADR-009 §2). The three are placement files -- `SiteEpoch`
computes where a field's grid sits on the plane, which is the arithmetic `UniverseGen` already
does at bake time -- so they joined the owned list rather than earning an exception. Because
widening a gating guard costs the protection it exists for, a **narrower rule replaced it**:
`World.h`, `World.cpp` and `WorldOrders.cpp` may no longer name `SiteEpoch` at all, which turns
ADR-024 §3d's "an epoch never applies under a live grid" from a promise into a check.

**With that fixed, run 150 is green on both legs and E1b's accept is met.** Every number the
accept asked for, measured rather than extrapolated:

- **650 tests pass**, Debug and Release alike, `UniverseSiteTests`' twelve among them. The
  cross-build mirror had said the same thing; the gate now agrees.
- **The universe parses and hashes in 183 ms in Release** -- for a file a third larger than
  U1's, against U1's 167 ms for 14.2 MB. That is 33 % more content for 10 % more time, well
  inside R17's ~1,000 ms threshold, so **no per-region content split is owed**. The
  cross-build guess had been 185-220 ms, which is close enough to be worth recording and not
  close enough to have been trusted.
- **The self test passes end to end**, including the gate-crossing checks that walk the baked
  anchors -- so site anchors did not disturb the topology U4 built on.
- **`universeHash ad9555dd776008a6`** is what the shipping binary reads out of the committed
  file, which closes the loop between the Linux bake and the Windows boot.
- **The replay hash matches across configurations** (`909bf3b4962d0b6a`, checkpoint
  `24b96ed08459db12`): standing spike 2 compares Debug against Release and passed, so the
  re-bake did not cost determinism.
- **One unique warning in the whole build**, and it is not this slice's:
  `NeuronClient\Picking.cpp(51): warning C4723`, which predates the economy phase.

The tick soak came back at **7.000 ms mean / 8.644 ms worst for a 1,024-ship grid, 14.0 % of
the tick, 7.1 capped grids per core** -- unchanged in character from A4's 7.728 ms, which is
the answer to the question this slice quietly raised: 6,223 more anchors in the content cost
the tick nothing, because a site is not a world until somebody warps to it.

*One defect was found by reading while CI ran*, and it is the kind two toolchains hide:
`UniverseGen.cpp` calls `std::begin`/`std::end` on the grade table without including
`<iterator>`. Both MSVC and libstdc++ pull it in through `<algorithm>`, so it would have
survived until a third toolchain refused it.


### E2 — Mining in the sim, and the site ledger
**Its gate is cleared:** [ADR-025](ADR/ADR-025-persistence.md) is accepted, so the site
ledger has a shape to be written in rather than a decision to wait on.

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

**Built (E2, 2026-08-20).** Mining is in the tick and the ledger is at the universe layer.
`GameLogic/SiteField.h/.cpp` is the field as the tick sees it — clusters, the ore filter, the
integer hazard arithmetic, and `SiteLedger`, the durable twin the registry keeps;
`GameLogic/WorldMining.cpp` is the third translation unit of `World` and runs the `Mining`
step; `SiteEpoch` gained `ResolveSiteEpoch`, which is what lets the registry resolve a site's
epoch at spin-up without naming a universe coordinate on the path that builds worlds;
`WorldRegistry` gained the ledgers, the `MineYield` apply point and the epoch at spin-up; and
`Tests/GameLogicTests/MiningTests.cpp` is the suite, **38 methods across four classes**.

**The tick's step order gained a sixth name, at the end.** `IngestOrders → GroupAdvance →
Steering → Integrate → Separate → **Mining**`, and last is the decision rather than the
leftover slot: a cycle is judged against where a ship *finished* the tick, so "inside the
field" is a fact about the finished tick rather than about a position two systems were still
arguing over. A group this step sends to `Done` is retired by the next tick's `GroupAdvance`,
which is exactly how a completed leg already behaves.

**A Mine order does not end when its leg does — that is when it begins.** Ingest gives the
order the richest matching cluster and one leg to it; when the fleet arrives, `GroupAdvance`
leaves the group `Underway` with no leg left instead of completing it, and `Mining` drives it
from there. That is what made a guard necessary that had been *implied* for fourteen slices:
the stale-solve pass now skips a group whose `legIndex` has passed its `legCount`, because a
working Mine order is the first group in this tree that outlives its own plan, and `ApplyLeg`
would have read past the plan to solve it. It also settles what a casualty does to a mining
wing — the survivors keep the stations the cluster put them in rather than closing the hole,
which is "no movement the player did not order" (ruling R5) applied to a fleet already parked.

**No new `OrderState`, and that was a choice with a price attached.** A fourth value would be
a wire value, a client branch and a schema bump for a distinction `etaSeconds` already draws
(§4d): a working Mine order reports the *cluster* where a flying one reports the leg, through
the same `LegEtaSeconds` seam. The ETA is the earlier of the cluster running dry and the
**last** Miner filling — `max` over the Miners, not `min`, because one full Miner does not end
the order.

**One number the design did not name had to be settled: `TICKS_PER_SECOND`.** Every duration
in this phase is authored in seconds and consumed in ticks, and `40 / 0.05f` is the arithmetic
`GATE_JUMP_TICKS` already exists to avoid — a mining cycle one tick short on one machine is a
hold that fills a tick early on it. So the integer rate lives beside the field model, and
`World.h` static-asserts that it agrees with `TICK_SECONDS`, which is what keeps two spellings
of one rate from drifting.

**Three hazard readings the ADR left to the implementation**, each written down beside the
code that makes it true. Radiation's slow is computed over the **total** stack rather than
accumulated per stack, because `3% × hazardScale` floors to 1 % on a grade-I belt and a
per-stack floor would make the gentlest field bite hardest per stack. Nebula heat sheds **only
while the laser is off**, which is what makes the authored numbers mean what §3b says: at 34 a
cycle against a threshold of 100, the third cycle locks the laser out and sixty seconds at two
a second clears it — and it is also what leaves room for the pacing toggle §3b describes,
which is E5's screen and not this slice's. Sensor damping is *not* scaled a second time by the
grade, because the content's five numbers already are the five grades.

**The wire moved, once.** `OrderKind::Mine = 6`, `OrderReason` gains 17–19 (and numbers 20–22
reserved for E3/E4 so that cluster is one bump and not three), `OrderSubmit` grows a trailing
`u8 oreFilter` beside the anchor it already wrote for every kind, and the check-order contract
in `GAME_SCHEMA_TEXT` extends with the parity matrix. The filter is the **one field the
decoder refuses on** where `kind` and `formation` are cast through: those two have reason codes
a player reads, and an ore filter has none, because every value it defines is legal everywhere
— so a byte outside it is a schema disagreement rather than a refusal owed to anyone.

**What this slice deliberately leaves broken, and says so out loud:** ore does not survive a
crossing. `TransferMember` carries an id, a hull and a wing, so a Miner that docks or warps
arrives with empty holds. E3 is the slice that gives a crossing a manifest and a station a
Bay, and half of that arrangement — cargo on the record with nowhere at the station to put it
— would be worse than none. It is written into `WorldMining.cpp`'s own file comment so nobody
finds it by surprise.

**It merged with ADR-026's placement work, and the interaction is a gift rather than a
collision.** "Solve, then slide" landed on `main` while this slice was in flight: a leg whose
solved formation lands in something now slides the *whole* shape to the nearest free anchor. A
Mine order's leg is the cluster it was sent to, so a wing ordered onto rocks another fleet is
already working forms up beside them instead of inside them — for free, with nothing in this
slice aware of it. The line worth writing down is *why* it is free: the Mine branch puts the
cluster in `legs[0]` and lets the ordinary leg machinery have it, rather than writing guidance
directly, and that is what left room for a rule nobody had written yet.

**What was verified, and how.** All of GameLogic compiles clean under **clang 18 on Linux** —
the cross-build route ADR-015's collision work was first proven on — with **clang-tidy clean**
against the repository config, and the seven pre-compile source guards (the clock, the RNG,
the `XM*Est` ban, `UniversePos`, `UniverseDef`, R2's prefixes and suffixes, repo-wide file-name
uniqueness, and both project registries) run by hand and green. **The whole `GameLogicTests`
suite was compiled and run** through a `CppUnitTest` shim and the DirectXMath subset GameLogic
names — **301 methods across eight files, 0 failures**, including the 38 new ones and
ADR-026's own, re-run after merging `main`. Three
existing `OrderTests` methods needed updating and every one of them is the suite doing its job:
the kind count is seven, a built kind refused on a world with no field says `NotAtSite`, and
the check-order string in the schema text moved because two checks were inserted into it.

**What CI said (run 155, commit `97a537a`, 2026-08-20).** Debug|x64, Release|x64 and Spike 2
all green on MSVC, in ten minutes. `self test: PASSED`, every named check `-- ok`, and the
step that matters for a slice this size — `nothing recorded: no gate wrote a report` — means
clang-tidy found nothing and no test failed. One unique warning in Release,
`NeuronClient\Picking.cpp(51): warning C4723`, which is the same pre-existing one E1b saw.
Content is untouched, as designed: `content: universe ad9555dd776008a6, economy
0b07707ec843431d, mixed 1965b853a23a5115`, all three unchanged from E1b. `universe: read,
parsed and hashed in 213 ms` against R17's ~1000 ms threshold. The replay hash moved to
**`69c58e2751c0df22` (checkpoint `fa56d9f638cba0fe`)** — it had to, because the ledger and the
cargo arrays joined the world hash — and Spike 2 confirms Debug and Release agree on it, which
is the property the number exists to prove.

**The soak is the one figure worth arguing with.** At the capped rung it reads `1024 ships --
167 ticks, mean 9.020 ms, worst 16.538 ms, 18.0 %`, against E1b's `200 ticks, mean 7.000 ms,
worst 8.644 ms, 14.0 %`; headroom drops from 7.1 capped grids per core to 5.5, and the rung
stopped at 167 ticks because it spent `RUNG_BUDGET_MS` rather than because it finished. Some
of that is a shared runner and some of it is real: `Mining` is a sixth named step over every
ship, and the soak's population is one in eight Miners. It is comfortably inside the 100 ms
tripwire and well inside the 50 ms budget, so nothing is owed now — but the *trend* is what
R10 exists to watch, and E3 adds a per-ship manifest to the same loop. If the capped mean
crosses ~15 ms the honest next move is to make `Mining` skip a grid with no field in one
branch rather than per ship.

**Runs 154, 156 and 157 hung for 45 minutes each, and run 158 caught the culprit in five.**
`main`'s head `a6dd412` ("Station progress") hung `Outpost.exe --selfTest` in **both**
configurations with zero diagnostics — the CI step only read the log after the process exited,
and the process never exited. Reading everything reachable found no unbounded wait, so instead
of a fourth guess the branch grew instrumentation: `Log::SetFlushEveryLine` (on in self-test
mode) and a 300-second watchdog in the CI step that kills the process and prints the complete
log. Run 158's log then named the line: `economy definition not found` — **the hang was at
boot, not in the self test.** Two defects, compounding:

- **`a6dd412` replaced the `CopyGameData` MSBuild target with `PostBuildEvent` xcopy lines,
  both broken on a fresh checkout.** Release's `/EXCLUDE:` names a wildcard where xcopy wants
  a file *listing* patterns, so the copy aborts having copied nothing — and the second
  command's exit 0 keeps the build green. Debug's variant drops the `GameData\` prefix, so
  content lands where the loader never looks. A stale `x64\<config>\GameData\` tree from
  weeks of the old target hid both on the author's machine; CI's fresh clone had no such
  shield, so the exe booted without its content.
- **`ReportFatal` raised a modal `MessageBoxW` unconditionally**, so a startup failure on a
  headless runner blocked forever on an OK nobody was there to click. That is what turned a
  misconfiguration into a silent 45-minute hang, three runs over.

Both are fixed on this branch: the `CopyGameData` target is restored (with the xcopy
post-mortem written into its comment), and the fatal dialog is gated to attended, windowed
launches — headless, bake and self-test runs report to the log and stderr and **exit**. The
watchdog and the per-line flush stay, because the next hang should also cost five minutes and
name its own line.

**And the copy is a post-build event again, on the owner's call — this time built so that the
same failure cannot recur silently (2026-08-21).** `Outpost/CopyGameData.cmd` is one script
called once per configuration, and every defect of the xcopy version is answered by its shape
rather than by care. One script instead of a command per configuration, so the destination is
written in a single place and the two configurations cannot drift apart. **robocopy** instead
of xcopy, because `/XF` takes patterns and paths directly — the flag that broke the last
attempt does not exist here. A single command in the event plus an explicit `exit /b`, so
MSBuild sees the script's exit code and not the last echo's; robocopy's bitmask is translated
at the one place that can get it right (`GEQ 8` is failure, `1` merely means files moved, and a
caller that treated non-zero as failure would fail every build that had anything to do).
Failures are printed in MSBuild's canonical `origin : error : text` form, so they land in the
error list rather than in the scrollback.

**None of that is what makes it safe, though.** What the last failure actually lacked was
anybody checking the result, and the self test could not: it runs from a scratch directory
whose `Outpost.json` points back at the repo's content, so a build that shipped no content at
all still reaches the end of the job. So the build gained a step that names files —
`Outpost.json` beside the exe, `Frontier.json`, `Economy.json`, `SoundBank.json` and
`Miner.obj` under `GameData\`, no second config inside the tree and no copied log. A count
would only say the copy ran; a name says the loader will find what it asks for, which is the
thing that failed. That step is the guard, and the mechanism above it is now free to be
whichever one reads best.

*And it cost a run to land, which is the entry that earns its place here. Run 162 went red in
both configurations on `robocopy exited 16`, and the reason is one line that had been correct
and was then tidied: the trailing backslash `$(TargetDir)` always carries was being stripped by
comparison, and that was replaced with `for %%I in ("%DEST%") do set "DEST=%%~fI"` on the
belief that `%~f` trims one. **It does not** — it fully qualifies a path and keeps the trailing
backslash exactly as given. So the second copy ran with `"D:\...\Release\"`, and while **cmd**
has no backslash escape and saw a balanced token, **robocopy parses its own command line with
the C runtime's rules, where `\"` is an escaped quote** — the closing quote stopped closing, and
`ERROR 123 Accessing Destination Directory D:\...\Release" Outpost.json /NJH` is the four
switches swallowed into the path. The comparison is back, the result is now asserted rather
than assumed, and the excludes are spelled as names rather than paths so that nothing depends
on agreeing with whatever robocopy canonicalised the source to. The error path is the half that
worked: the script printed `CopyGameData.cmd : error : ...`, MSBuild raised it as an error and
failed the build with MSB3073 — which is exactly the failure mode the xcopy version did not
have.*

**Green on the second try (run 163, commit `84e276b`).** The copy reports `27 files, 25 copied,
2 skipped, 21.47 m` — the two skipped being the tracked `Outpost.json` and `Outpost.log`, which
is the exclusion doing real work rather than a formality: a stale log **is** in the repository,
and without `/XF` it would ship. `Outpost.json` then lands beside the executable on its own, and
the check step says so in the one line a reader wants: `content beside the executable: 25 files,
21.5 MB, and Outpost.json in x64\Release`. Debug|x64, Release|x64 and Spike 2 all pass in ten
and a half minutes, **717 tests** with none failing, `self test: PASSED`, content hashes and the
replay hash `69c58e2751c0df22` unchanged, one pre-existing Release warning, no clang-tidy
finding. The soak reads 9.265 ms mean / 16.312 ms worst on a capped grid — run to run against
161's 8.729 / 17.573, which is runner variance and not a trend.

**And the merge head is green (run 159, commit `59d4a20`, 2026-08-21).** Debug|x64,
Release|x64 and Spike 2 all pass in ten minutes; the self test is back to twelve seconds and
`PASSED`, now including `main`'s own approach-disconnect scenario — three clients over the
loopback, a fleet abandoned mid-approach that never docks. **694 tests pass on MSVC** in both
configurations (650 before the merge window; E2's 38 and ADR-026's own among the growth), with
no clang-tidy finding and the one pre-existing Release warning. Content unchanged —
`universe ad9555dd776008a6, economy 0b07707ec843431d, mixed 1965b853a23a5115`, parsed in
214 ms — and the replay hash is **`69c58e2751c0df22` (checkpoint `fa56d9f638cba0fe`)**, byte
for byte what run 155 measured on E2 alone, with Spike 2 confirming Debug and Release agree:
the merge, the ADR-026 placement work and the CI ordeal between the two runs moved the
simulation not at all. The capped-grid soak reads 8.179 ms mean / 12.932 ms worst over 184
ticks (6.1 grids per core), between run 155's 9.020 and E1b's 7.000 — consistent with runner
noise on top of a real E2 cost, which is R10's trend to keep reading.

### E3 — Cargo, the Bay, and the wire cluster · 🏁 G0
Ships carry manifests; the station roster's record grows one (ADR-017 §1 as amended — cargo is
not damage, so repair-by-absence is untouched); the **Station Bay** joins the universe layer as
a per-`(PlayerId, station)` ledger in the registry hash. `TransferToBay` and `TransferToShip`
join the `StationCommand` family on the acked stream, validated by the shared pure function
over the RosterView plus the Bay — **manual, both directions** (ruling from ADR-024 §5c: the
transfer is the risk decision and the commitment of ore to industry). The wire cluster lands
in one fail-closed bump: the two verbs, reasons `InsufficientMaterials = 20`,
`RefineryBusy = 21`, `RecipeLocked = 22` (the last two numbered now and returned by E4b), and
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

**Built 2026-08-21.** Ore stopped evaporating at boundaries, and the station grew its second
resident.

`TransferMember` and `RosterEntry` each gained a three-integer manifest, which is the whole of
"cargo survives a crossing": a Miner that docks parks its hold on the roster, a ship that
undocks flies out with it, and a fleet that warps arrives holding what it left with.
ADR-017 §1's "three fields, deliberately" is amended rather than contradicted — that argument
was about **gauges**, and cargo is not one. Nothing regenerates it and nothing repairs it; it
is the player's property rather than the ship's condition, so **repair-by-absence is untouched**
and the sentence saying so now lives beside the field.

The **Station Bay** joins the ledgers and the rosters as the third resident of "worlds forget,
the universe layer does not": `StationBay` is per-`(owner, station)`, sorted on that pair,
created only once something has been stored in it, and folded into `WorldRegistry::Hash` on the
rosters' terms — but **with no currency rule**, because a Bay has no epoch to go stale against.
What is in it was put there by a command and stays until another command moves it, which is the
difference between committed property and a pool the shard refills on a calendar. The privacy
rule lives in the **key**: two commanders docked at one station have two Bays, and a Bay is not
even addressable without saying whose.

`TransferToBay` and `TransferToShip` join the verb family, applied **on the spot beside
`AssignWing`** rather than filed on the bus — both ends of the move are universe-layer state on
one host, and the bus exists to stop one grid reading another mid-tick, which this does not do.
One `MoveOre` serves both directions because they are one move with its sign flipped, and two
would be two places for the conservation rule to go wrong; the suite asserts conservation rather
than trusting it. Ore is drained and filled in **roster order**, so the outcome is a function of
the world rather than of the order the client listed its ships in.

**The command seam grew a `PlayerId`**, which is the one change outside the economy's own
files. `Simulation::ApplyOrderBytes` now takes both the player and the client id, because they
are different questions -- which socket said it, and whose property it is about -- and they
coincide today only because there is one player each. A registry that had to guess whose Bay a
transfer filled would be guessing about property. It is the command half of what `WriteSnapshot`
and `WriteSummaries` already do outbound, and ADR-018 D5's note applies unchanged: with one
player it filters nothing, and the shape does not change when there are two.

**The wire cluster landed as one fail-closed bump.** `StationCommand` gained an ore byte and a
u32 count; `StationRoster`'s row went 6 bytes to 18; `SummaryKind` gained `SiteStatus = 2`,
`CargoStatus = 3` and `BayStatus = 4` with bodies in a new `EconomyMessages.h`; the station
check-order string gained `InsufficientMaterials`; and `OreId` joined the schema text beside
`OreFilter`, because they are different bytes with different rules — `Any` is not a quantity, so
the transfer decoder refuses zero where the order decoder accepts it. The ore byte is **the one
field of the command the decoder refuses on**, where the verb and the formation are cast
through: an out-of-range ore would index the manifest arrays before validation got an opinion,
and there is no sentence to show a player whose client believes in a fourth ore.

**`EntityRecord` is untouched, and a test asserts the arithmetic** rather than the intention:
21 bytes, 43 ships per datagram, and one added byte would make it 22 and 41 — landing exactly
on `Snapshot.h`'s asserted floor with nothing left over. That is the calculation ADR-024 §4d
refused in advance, and the test recomputes it so the next person tempted by a cargo byte finds
a failing assertion instead of a mystery about the ship cap.

**What running the accept found.** The G0 scenario was written to prove the loop composes, and
it immediately proved it did not: `ApplyTransit` spawned arrivals with empty holds, so a fleet
that warped anywhere lost its cargo in silence. Dock had a test, undock had a test, and transit
had neither — so ore survived both boundaries anybody had thought to check and evaporated on the
one nobody had. Fixed, and `AWarpCarriesTheHoldToTheFarSide` is the unit test that should have
caught it first. This is the second slice running where the end-to-end check earned its cost by
finding something the unit suite could not.

**What was verified, and how.** All of GameLogic compiles clean under **clang 18 on Linux**,
clang-tidy reports nothing in the files this slice touched, and the source guards run by hand
are green. **The whole `GameLogicTests` suite compiles and runs** through the `CppUnitTest`
shim — **324 methods across nine files, 0 failures**, including 23 new ones in `CargoTests.cpp`.
Three existing tests needed updating and each is the suite doing its job: two hand-built
`StationCommand` payloads in `RegistryTests` name the wire layout and had to grow the two new
fields, and every `SubmitStationCommand` call site had to say who was calling. The G0 scenario
itself was extracted and **run** against the real registry rather than only compiled, which is
how the transit defect surfaced: mine 24 units, warp ~2,400 ticks, dock, and find all 24 on the
roster.

*The harness earned a fix too. `runall.sh` never re-copied the sources, so a GameLogic edit made
after the last manual sync was silently not compiled — twice it reported on code that no longer
existed, once as a phantom test failure and once as a real one that would not go away. It syncs
unconditionally now.*

**CI's verdict (run 161, commit `93956dc`, 2026-08-21).** Debug|x64, Release|x64 and Spike 2
all green, in eleven minutes. **717 tests pass on MSVC** with none failing, the whole suite in
20.8 s on Release, and `self test: PASSED` — including all thirteen new 🏁 G0 checks: mining
puts ore in the hold, the field's status shows it measurably emptier with the worked cluster
reading below full, a loaded Miner warps and docks, the Miner reaches the roster with every
unit it was carrying, the commander commits it to the Bay, the Bay holds exactly what was
committed and belongs to nobody else, and it outlives the grid it was filled at. No clang-tidy
finding, no failing test, and one warning in the whole build — the pre-existing
`NeuronClient\Picking.cpp(51)` C4723; Debug has none. Content is untouched, as expected:
`universe ad9555dd776008a6, economy 0b07707ec843431d, mixed 1965b853a23a5115`, read, parsed and
hashed in **212 ms** (4,630 ms on Debug). The tick soak is flat against E2 rather than up: a
capped grid costs **8.729 ms mean / 17.573 ms worst** against E2's 9.020 / 16.538, which is
17.5 % of the budget and 5.7 capped grids per core.

**The replay hash did not move, and the paragraph this one replaces said it would.** It is
`69c58e2751c0df22` (checkpoint `fa56d9f638cba0fe`), byte for byte what E2 measured, and Spike 2
confirms Debug and Release agree on it. The prediction was wrong in a way worth keeping written
down: `RunScriptedReplay` is six ships in a bare `World` hashed with `ComputeWorldHash`, and
everything this slice added — the Bay, the roster manifests, the transfer manifest — is
`WorldRegistry` state that the replay never touches. The one piece that *is* world state,
`ShipCargo`, E2 had already folded in. **The replay hash is a world hash and not a shard hash**,
so an unmoved number here says nothing either way about the universe layer; what covers that is
`WorldRegistry::Hash` and the G0 scenario above. `GameSchemaHash` did move, which is the point
of the slice.

### E4a — The durable store
[ADR-025](ADR/ADR-025-persistence.md) stops being an accepted design and becomes three files.
**No new game state lands here**: the slice takes the durable state E2 and E3 already built —
rosters, Bays, site ledgers, ship manifests, the ships themselves and where they stand — and
gives it somewhere to survive a process exit.

**GameLogic stays pure.** `GameLogic/DurableState.{h,cpp}` is `WriteDurableState`,
`ReadDurableState` and `DurableHash` over `Neuron::ByteWriter`/`ByteReader` — bytes in, bytes
out, a `PersistenceDiagnostic` list on malformed input, never a path and never a throw. That is
the `ParseUniverse`/`ParseEconomy` posture applied to a binary format, and it is what keeps the
round-trip test fixture-free. The durable line is ADR-025 §1's exactly: ships
`(ShipId, HullClass, WingId, PlayerId)` with their anchor and, for a hull standing on a grid,
its position and heading; station rosters; Bays; cargo manifests; site ledgers with the epoch
index they were last re-formed at; in-flight transfer records; **the ship-id high-water mark**
(§1a's first trap — a restore that would *lower* it is a refusal, not a clamp); and the shard
tick every record is stamped against. Order queues, steering, ETAs, undock-protection windows
and wrecks are not written, so **a fleet reloads at rest with an empty queue**: intention is the
player's to restate.

**`DurableHash()` is a second hash, not a second opinion.** `WorldRegistry::Hash()` folds the
order queues §1 has just declared transient, so a reload cannot reproduce it and a check written
against it would either be wrong or teach everyone to ignore a red test. `DurableHash()` folds
exactly §1's list, in anchor-id then id order; it is what a snapshot records and what a
checkpoint verifies. The two answer different questions and both keep their own.

**NeuronServer gets the store, which never learns what it is storing.**
`NeuronServer/DurableStore.{h,cpp}` owns files, framing, checksums, flushing, snapshot rotation
and torn-tail recovery behind `Append(recordKind, payload)`, `Replay(handler)` and
`WriteSnapshot(state, durableHash)`. A frame is ADR-025 §3's table — `magic`, `payloadBytes`,
`recordKind`, `shardTick`, payload, `crc32` — written with `ByteWriter` so there is not a second
endianness convention in the tree, and the file header carries `JOURNAL_FORMAT_VERSION`, the
`universeHash`, the `economyHash`, the `hostId` and the tick of the last snapshot. The store
knows a record has a kind, a length and a checksum; it does not know that one of those kinds is
a Bay. **Records are outcomes, not commands** — "these ships are now docked at anchor 412",
never "a Dock order arrived" — which is what keeps the journal from becoming a second replay
engine obliged to agree with the first forever.

**The tick never waits on a disk.** Records are serialised on Sim at the between-ticks apply
point where the transfer bus already runs, pushed into a lock-free SPSC ring, and drained by a
**journal lane** registered like every other lane — ADR-007 §7's sanctioned mechanism, run in
the other direction. The lane flushes on a watermark or every `JOURNAL_FLUSH_MILLISECONDS`
(1,000), whichever comes first, and always on a clean shutdown before ADR-008's ordering
releases Sim: **a clean stop loses nothing; a hard kill loses at most the last second.**
Snapshots go every `SNAPSHOT_INTERVAL_SECONDS` (300) through §5's three-step rotation — write
the `.tmp`, rename it over, then truncate the journal and rewrite its header — so there is no
crash window in which both files are needed and one is missing. Checkpoint records carry a
`DurableHash()` every `CHECKPOINT_INTERVAL_TICKS` (1,200). All four numbers join the envelope
suite's guardianship as table data.

**Boot is §6 in order, and nothing about it is silent.** A wrong magic or a
`JOURNAL_FORMAT_VERSION` this build does not know refuses to start and names both versions.
**`universeHash` is the only fatal content guard** — a re-bake renumbers anchors, so a roster
keyed by one is nonsense against it — while `economyHash` is recorded, compared and *survivable*:
a hold that shrank under a retune clamps per ship and logs, because the numbers moved under the
content and that is a content decision rather than a corruption. Then the snapshot, verified by
its hash; then journal records newer than it, verified at each checkpoint; a bad frame at the
**tail** is the write that was in flight when the power went, truncated and logged as a count of
records recovered and bytes discarded, while a bad frame **in the middle** is corruption and
refuses with both files left untouched for whoever has to look.

**`Outpost.exe` is the wiring** — construct the store, call the pure functions, hand the results
across — thin enough that there is nothing to test, which is the standing price of ADR-014 §2a
and the reason the interfaces get stubbed in the engine test projects instead. `Outpost.json`
grows `"persistence": { "directory": "ShardState", "enabled": true }`, `AppConfig` grows with
it, and the path resolves through `ResolveContentPath`'s writable sibling rather than the
LocalAppData user layer: that layer is one player's settings on one machine, and this is a
service's state.

**One wrinkle to settle rather than inherit.** ADR-025 §7 makes `"enabled": false` the headless
and `selfTest` posture, and §8 asks `selfTest` to prove a restart — which needs it true. The
configuration-by-directory mechanism CI already uses is the answer: the restart scenario runs
from its own directory with persistence enabled against a scratch path that the run creates and
removes, and every other headless run keeps the default and persists nothing. Writing that down
here is cheaper than discovering it as a contradiction at the accept.

**Accept:** `GameLogicTests` — a registry built in code with rosters, Bays, ledgers, manifests
and ships both docked and in space, written, read into a second registry, and the two
`DurableHash()` values equal; the high-water mark surviving a round trip and a restore that
would lower it refused; a truncated buffer producing diagnostics rather than a half-built
registry (the `ParseEconomy` posture, tested the way `ParseEconomy` is); a reloaded fleet
holding position with an empty queue, which is §1's line as a test rather than a sentence.
`NeuronServerTests` — the store with no game in it: framing and CRC, a deliberately torn tail
recovering to the last good record, a mid-file corruption refusing, snapshot rotation
interrupted at each of its three steps, and a journal older than its snapshot skipped by tick.
`selfTest` — dock a fleet, move ore into a Bay, **stop the host and start it again**, and find
the roster, the ore and the site ledger still there with `DurableHash()` reproduced; and a
deliberate `universeHash` mismatch refusing to start while naming both numbers. The replay suite
is untouched and must stay so — the journal is not the replay log — and `WorldRegistry::Hash()`
does not move.

**What it deliberately does not do:** no refine jobs, tiers or projects — E4b appends its
records to the format this slice defines, which is an addition to §1's durable list and not a
change to the frame; no migration across a re-bake (§9, and the trigger for designing one is
named there); no SQL. **R26 is the register row this slice exists to answer**, and the
early-validation signal that row names — the restart scenario running on every push — starts
here rather than at G1.

### E4b — Refining, tiers, and the projects · 🏁 G1
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

The wire half is the phase's second and last fail-closed bump: `RefineStart`, `RefineCancel`
and `ProjectContribute` join the `StationVerb` family E3 left at four, `RefineryBusy = 21` and
`RecipeLocked = 22` stop being reserved and join the station check-order string, the summary
family gains **`RefineryStatus`** on `BayStatus`'s cadence and framing, and the event record
gains **refine-complete** and **project-complete** beside E2's two. Every number these read is
already authored and hashed — E1a parsed the recipes, the batch factors, the tier table with
its `slotsPerPlayer` and `recipeTierCap`, the band caps and the upgrade projects — so this
slice adds no content and moves neither `universeHash` nor `economyHash`.

**Accept 🏁 G1:** `GameLogicTests`: a batch's arithmetic exact at every batch size, refund
floors included, with a property test that inputs consumed minus refund always equals the
recipe's rate — the books balance or the test fails; band caps refusing Nova-Steel at a
High-Sec station with `RecipeLocked`; slot exhaustion returning `RefineryBusy` and the queue
draining in order; a project completing exactly once when two commanders contribute its last
units in the same tick; and jobs, tiers and project contributions **joining E4a's durable list**
— the round-trip test grows to cover them and the two `DurableHash()` values still agree.
`selfTest` gains the half E4a's restart scenario could not have: start a batch, **stop the host
mid-job and start it again**, and find the job still running with its completion tick unmoved.
That is G1's real claim — a refinery that stops when the shard restarts is the demo this phase
exists not to be — and it is a claim about this slice's records in a format that already
proved itself.

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

- ~~**D-P1 — The persistence ADR.**~~ **Delivered and accepted 2026-08-20:** [ADR-025](ADR/ADR-025-persistence.md). Journal format, snapshot cadence, crash
  recovery and the reload proof, for the engine-owned journal ADR-024 §7a ruled on. **It blocks
  E2**, not E1 — ADR-024 §7a said "blocks E1", corrected there and here: E1a is a read-only
  content file and E1b is authored content, so neither has durable state to persist; the first
  durable thing in this phase is E2's site ledger. Three of its rulings reach into E2 directly:
  the durable line is **identity and location, never intention** (so a reloaded fleet holds
  position with an empty queue); the ledger's proof is a separate **`DurableHash()`**, not
  `WorldRegistry::Hash()`, which folds the order queues E2 is about to write; and journal
  records are **outcomes**, so E2 files "this pool is now N" rather than "a cycle completed".
  **Where it is *implemented* is E4a** (added 2026-08-21): the ADR gated E2's design and E2
  wrote its ledger to the shape ADR-025 named, but no journal, snapshot or store exists in the
  tree — a deliverable being accepted is not the same as its code being built, and this line
  says so where the next reader will look for it.
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
- **E4 splits, and the durable store goes first** *(added 2026-08-21, the same call E1 got)*.
  E4's accept was written as "the whole registry — rosters, Bays, ledgers, jobs, projects —
  round-tripping through the persistence layer", which reads as one clause and is a second
  slice: ADR-025 lands three new files across three projects, a config block, a lane, a boot
  path and three test surfaces of its own, and **none of it is in the tree** — the only trace
  of the journal in the code today is three comments pointing forward to it. Left as one
  slice, the first exercise of a brand-new file format would be against brand-new job state,
  with two unproven things debugging each other. Split, **E4a's subject already exists and is
  already hash-proven**: rosters, Bays, ledgers and manifests have been in `WorldRegistry` and
  in its hash since E3, so the round-trip has something real to bite on the day it is written,
  and E4b appends its records to a format that has already survived a torn tail. The blast
  radii differ the way E1's did — E4a touches the engine, the composition root and
  `Outpost.json`; E4b touches the economy's own files and one wire bump — and the G1 milestone
  stays with E4b, where the first alloy is.
  *This is also the ordering the E3 accept argues for: the G0 scenario found a defect
  (`ApplyTransit` spawning arrivals with empty holds) that every unit test had missed, because
  it was the one boundary nobody had composed. A restart is the same kind of boundary, and it
  is worth crossing once with state that is already understood.*
- **G0 at E3 rather than E2**, the same call H0 made at T2: a loop is a milestone when it runs
  over the real transport end to end, not when its sim half is correct in a test.
- **Nothing interleaves with the universe or station phases.** Both are past their sim halves
  and what they owe is screen work; this phase consumes their sim and adds no new claim on
  their client halves.
