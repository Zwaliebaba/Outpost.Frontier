# Economy Build Order — the Mining and Refining Phase

**This document does not sequence** *(2026-08-22)*. It says what each E-slice contains, what its
accept is, and — in the **Built** lines, which are most of its length — what landed and what that
cost. **When a slice is built is [Plan-of-Record.md](Plan-of-Record.md)'s**, which sequences
across all three phases and the work that belongs to none of them. E5 also sits behind the input model as of
2026-08-22, for the reason the plan gives: a screen built against the mouse adaptation would be
retrofitted for touch afterwards.

**Status:** Session output 2026-08-20 · **E1a, E1b, E2, E3, E4a and E4b are built**, all six
green in CI (run 176, commit `c44724d`, 2026-08-21 — 795 tests, `self test: PASSED` with all
thirteen 🏁 G0 checks and 🏁 G1's mid-job restart); **E5, the two screens, is the last slice of
this phase and is not built.**
**Both prints that gate E5 landed 2026-08-21** —
[cargo-tab.png](ScreenPrints/cargo-tab.png) (D-P2) and
[refinery-tab.png](ScreenPrints/refinery-tab.png) (D-P3), sibling tabs of P1's hangar, with
their sources beside them. **Every design deliverable this phase tracks is now closed**; what
they leave behind is **eight owner rulings**, listed with each print below. Seven are owed
before E5. The eighth — whether a refine job can be cancelled — **was owed before E4b and was
answered 2026-08-21**: a queued job cancels whole, a running one cannot. The design this plan
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
waited for a GPU and a person. *(T2's client half has since landed — 2026-08-21 — and it landed
the same way this phase plans to: everything device-free built and tested, with the visual
checkpoint recorded as owed rather than assumed.)*

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

**Built 2026-08-21.** ADR-025 stopped being an accepted design and became three files, and
the shard stopped forgetting.

`GameLogic/DurableState.{h,cpp}` is the pure half — `WriteDurableState`, `ReadDurableState` and
`DurableHash` over `ByteWriter`/`ByteReader`, with a `PersistenceDiagnostic` list on malformed
input, never a path and never a throw. What is written is §1's list as far as this tree has it:
ships wherever they stand with their anchor, position, heading and hold; station rosters; Bays;
site ledgers with the epoch they belong to; the bus, because a fleet three seconds into a warp
is somewhere and the record is the only thing that knows where; the ship-id high-water mark; and
the shard tick. What is *not* written is order queues, steering, guidance, undock protection and
wrecks — so a fleet reloads at rest with an empty queue, and
`AReloadedFleetHoldsPositionWithAnEmptyQueue` is §1's line as a test rather than a sentence.

**`DurableHash` is a second hash and not a second opinion**, and this is the claim the slice
turns on. `WorldRegistry::Hash` folds the order queues §1 has just declared transient, so a
correct reload cannot reproduce it and a check written against it would either be wrong or
teach everyone to ignore a red test. `TheReplayHashAndTheReloadProofAnswerDifferentQuestions`
asserts the argument: an accepted order moves one number and not the other.

`NeuronServer/DurableStore.{h,cpp}` is the store, and **it never learns what it is storing** —
a record has a kind, a length and a checksum, and that one of those kinds is a Bay is a fact
for the composition root. The distinction the whole boot path turns on is **torn tail versus
corruption**: a bad frame at the end is the write that was in flight when the power went, so it
is truncated and logged as a byte count; a bad frame with good frames after it cannot be an
interrupted write, so it refuses and leaves both files untouched — truncating there would throw
away good state to make a bad file parse, and the shard would come up looking healthy.

**Two versions, not one.** `JOURNAL_FORMAT_VERSION` versions the frame and
`DURABLE_FORMAT_VERSION` the payload, because they move independently: E4b changes what a
payload contains and nothing about how a record is wrapped, and one number covering both would
refuse every existing shard for a change that could not affect it. ADR-025 §2 carries the note,
with the two signature adjustments §3's frame forced (`Append` takes the shard tick; the record
kind is the `u16` the frame actually holds).

**The wiring is the composition root's, and two of its consequences are the interesting part.**
The starting fleet is what a *new* shard is built with, so the load decides whether to spawn it
— spawning it on top of a reloaded shard would hand every commander a second fleet on every
restart. And the scripted patrol's ship list was *intention*, so a reloaded shard rebuilt it
from what was actually standing on the start grid rather than restoring it — the patrol has
since been removed, and the walk that fed it is now only a count in the log. `ServerHost` owns the
cadence on the Sim thread and nowhere else — a snapshot every `SNAPSHOT_INTERVAL_SECONDS`
counted in **ticks**, because the tick is the only clock (ADR-002 §1) and a wall-clock timer
there would be a second one to drift, and one on the way out before anything is torn down, which
is what makes "a clean stop loses nothing" true rather than aspirational.

**Two refusals rather than repairs**, both of them ADR-025 naming a trap in advance. A ship-id
mark that would go backwards is refused, because clamping it up is the failure the rule exists
to prevent wearing a repair's clothes: the shard would carry on and re-issue ids that the
rosters and transfers being loaded in the next few lines still name. And a load into a running
registry is refused, because there is no answer to what merging a save file into a running shard
would mean that is better than declining to have one.

**Three defects, all found by reading the code back rather than by a test.** `Replay` reopened
the journal over the handle `Open` had left — a leak on Linux and *refused outright* on the
platform that ships, so the shard would have persisted nothing while every other check passed.
`LoadDurable` could refuse on its last ship having already written the rosters, the Bays and the
ledgers, which is the half-built registry its own contract promises not to leave; everything is
judged against the content now before anything is written. And `m_journalHeaderBytes` was
written in three places and read in none. Each has a test beside it.

**The `selfTest` restart scenario is where R26's early-validation signal starts.** It runs in
two halves because they are two claims. `RunRestartLoop` proves the **format**: dock a loaded
Miner, commit ore to a Bay, write through a real store to a real directory, and have a second
registry and a second store read it back — two independent runtimes meeting only through two
files, which is what a restart is. The host section proves the **wiring**: after `Stop` and
`Join`, the snapshot is on disk, stamped with the tick the host stopped at, carrying the shard's
own reload proof. The second is the one a person would forget to make, and a store that works
and is never called is a shard that loses everything while passing every unit test.

**CI's verdict (run 172, commit `2e67966`, 2026-08-21).** Debug|x64, Release|x64 and Spike 2
all green. **773 tests pass on MSVC** with none failing, every source guard green, no clang-tidy
finding, and one warning in the whole build — the pre-existing `NeuronClient\Picking.cpp(51)`
C4723. `self test: PASSED`, including **seventeen new persistence checks**: the shard serialises
its durable state, a shard with no state opens fresh, the restarted shard finds it and reads it
back, **the reload reproduces the proof**, the roster, the hold and the Bay all survive, a load
into a running shard is refused, and a re-baked universe refuses to load an old one.

**The line worth quoting is from the host rather than from a test**, because it is the claim a
person would forget to make: `shard snapshot at tick 173: 1382 bytes, durable hash
c5dda30f194e6d2c`, immediately followed by `server host stopped after 173 ticks`. The shipping
binary wrote itself down on the way out, on the Sim thread, before anything was torn down —
which is "a clean stop loses nothing" as an observation instead of an intention.

**The replay hash did not move, and this time that is the prediction.** It is
`69c58e2751c0df22` (checkpoint `fa56d9f638cba0fe`), byte for byte E2's and E3's, and Spike 2
confirms Debug and Release agree on it. E3's Built line explains why in advance: the replay
scenario is six ships in a bare `World` hashed with `ComputeWorldHash`, and **this slice adds no
world state at all** — it reads existing state and writes it to a file. A slice that moved the
replay hash while claiming to add nothing to the simulation would be the thing to investigate.

The tick soak is inside its tripwire and worth one sentence rather than a celebration: a capped
grid costs **9.635 ms mean / 16.971 ms worst** against E3's 8.729 / 17.573, which is 19.3 % of
the tick and 5.2 capped grids per core against E3's 5.7 — run-to-run variance on a shared
runner rather than a trend, since nothing here runs per tick. Content is untouched and the
universe parses in **219 ms** on Release.

*What is deliberately not here, and is E4b's: the journal's **game** records. The store's
journal exists, is framed, CRC'd, recovered and tested, and nothing appends to it yet — so a
hard kill today loses back to the last snapshot rather than to the last second. The per-outcome
records that close ADR-025 §4's named window need a change-set at the registry's mutation
points, and they land with the state E4b is about to add rather than being retrofitted twice.
This is written here rather than left to be discovered, which is the E2 posture about ore not
surviving a crossing.*

### E4b — Refining, tiers, and the projects · 🏁 G1
Refine jobs `(recipe, batch)` submitted as station commands against a Bay — **at any
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

**Built 2026-08-21.** The station became industry, and the phase's last piece of simulation
landed.

`GameLogic/Refining.{h,cpp}` is the arithmetic — pure, integer-only, and with deliberately
nowhere to put an RNG, because batching is the deterministic economy's answer to yield variance
(§6b): the ME refund is **exact and floored per material**, not expected on average. Per
material matters: a refund computed on the total and split afterwards would have to decide which
ore eats the remainder, and there is no answer to that which is not an invention.

**A job is priced once, at submission, and carries its plan.** Not recomputed at completion, and
that is a ruling rather than an optimisation: D-P3's form shows this arithmetic *before* the
player commits, so the numbers they agreed to are the numbers they get — a station whose tier
rose mid-job does not retroactively improve it. It also makes the durable record
self-contained, which is why a reloaded job needs no tier table to know what it owes.

**The order of the arithmetic is a stated contract**, and stating it caught a slip in the ADR.
§6b's worked example says a 50-batch of Plates at T3 takes 22.5 minutes — the batch factor
applied and §6c's *tier* factor forgotten. Both are authored, both are parsed, and §6a's
units-per-slot rates are undiscounted, so both multiply and the honest number is 20.25 minutes.
The materials in that example were exact and are unchanged. **ADR-024 §6b carries the
correction**, and `TheAdrsWorkedExampleAddsUpExceptForTheTierDiscount` asserts both halves.

**The owner's ruling closed D-P3's one open question that was owed here** (2026-08-21): a
**queued** job cancels whole and its inputs return to the Bay untouched; a **running** job
cannot be cancelled, its inputs being spent. Two states with one rule each, and nothing is
created or destroyed by a cancel — what goes back is the same array that came out. ADR-024 §6b
was silent and now is not. **Three rulings remain and all three are E5's**, so nothing left in
this phase is gated on a decision.

**The check order forks, and that is §6b enforced rather than commented.** A refining verb names
no ships and requires no dock — "at a docked-or-remote station, focus never gates command" — so
the three selection checks would be asking about something that does not exist. The refining
order is `UnknownStation → RecipeLocked → RefineryBusy → InsufficientMaterials`, in the schema
hash beside the other two because it is behaviour both machines must match. `RecipeLocked`
precedes `RefineryBusy` because a locked recipe is a fact about the **station** and a full queue
is a fact about **you**: a player told their queue is full, when the real answer is that this
station will never cook Nova-Steel, goes and waits instead of going and flying.

**Jobs advance in `WorldRegistry::Tick` beside the transfer bus**, not inside a world's tick — a
refinery that needed a live grid would be a refinery nobody could walk away from, and walking
away is the feature. `AJobRunsWhileItsStationsGridIsNotEvenLive` asserts it with **zero worlds
spun up** for the job's whole duration.

**A project completes exactly once because the check lives at the contribution**, not on a
sweep. Two commanders pushing on the last unit in one tick: the first completes it and the
second finds nothing remaining, so it is refused before a single unit leaves their Bay. A sweep
would have had to decide what to do with units it had already taken, and there is no answer to
that which does not either keep somebody's property or invent a refund path.

**The wire cluster is the phase's second and last fail-closed bump.** `RefineStart`,
`RefineCancel` and `ProjectContribute` join the verb family; `StationCommand` grows an alloy
byte (**refused** rather than cast, on E3's ore byte's terms, because it indexes a recipe and a
Bay before validation could have an opinion) and a job sequence in its own field — a sequence is
an identity where the other two are quantities, and one field meaning both would eventually
cancel job 50 because somebody meant a batch of 50. `RefineryBusy = 21` and `RecipeLocked = 22`
stop being reserved. `SummaryKind` gains `RefineryStatus`, carrying the **completion tick**
rather than a duration, because wall-clock ETAs are the screen's job (D-P3) and a duration would
make the tab's arithmetic depend on when the frame arrived. `EventKind::RefineComplete` and
`ProjectComplete` stop being reserved: a refinery runs while the commander is offline, so the
away-log is the only thing that can say what finished.

*`BayStatus` grew the alloys rather than them getting a message of their own — a Bay is one
statement about one place, and a screen reading two sources for it would eventually show two
different answers. **That change found a defect in E3's code**: `StationBay::TotalUnits` counted
ore only, and the summary sender skips a Bay whose total is zero, so a commander who refined all
their ore would have watched their whole industrial estate vanish from the screen at exactly the
moment it became interesting.*

**The durable format goes to version 2.** Jobs, station tiers and project contributions join
ADR-025 §1's list, and the Bay grows its alloys. Tiers and projects are durable for a stronger
reason than jobs are — they are permanent and communal, so a completed project that did not
survive a restart would un-build something a dozen commanders paid for. **A station's tier is
derived, not baked**: the authored floor (T1, T2 in the starter system), raised by whatever has
been built, and **clamped by the band on the way out** rather than trusted on the way in, which
is what keeps "no High-Sec station can ever cook Nova-Steel" true against a project, a reload or
a content retune that lowered a cap under a station somebody had already upgraded.

**🏁 G1's claim is the completion tick, not the job.** `ARunningJobSurvivesARestartWithItsCompletionTickUnmoved`
and the `selfTest`'s mid-job restart both assert it, because a job restored with a *duration*
rather than a *deadline* would silently restart its own clock on every restart — a shard that
bounced twice an hour would never finish anything, and every individual test of it would still
pass.

**What was verified, and how.** All of GameLogic compiles clean under **clang 18 on Linux** and
the whole `GameLogicTests` suite compiles and **runs** there — **364 methods across eleven
files, 0 failures**, of which 23 are new in `RefiningTests.cpp` plus the durable additions —
with `NeuronServerTests`' store half at 14/14 beside it. clang-tidy reports nothing in the files
this slice touched; it caught one implicit widening in `RefineryStatusBytes` before CI could.
Two existing fixtures needed updating and both are the suite doing its job: the two hand-built
`StationCommand` payloads in `RegistryTests` name the wire layout and had to grow the two new
fields, and `DurableStateTests`' corrupt-cluster-count fixture had to learn that four families
now trail the ledgers rather than one.

**CI's verdict, run 176.** Debug|x64, Release|x64 and Spike 2 all green: **795 tests on MSVC
with none failing**, every source guard green, no clang-tidy finding, and one warning in the
whole build — the pre-existing `NeuronClient\Picking.cpp(51)` C4723. **`selfTest`: PASSED on
both configurations**, and it is the shipping binary that carries 🏁 G1's claim rather than a
test fixture:

```
self test: the restart scenario starts a refine job -- ok
self test: the job is running before the shard stops -- ok
self test: the refine job survives the restart -- ok
self test: with the job it was -- ok
self test: and its completion tick unmoved -- ok
self test: the reloaded job finishes into the Bay -- ok
```

**The snapshot grew by exactly twelve bytes** — 1382 at E4a to 1394 here, at the same tick 173 —
and the durable hash moved with it, `c5dda30f194e6d2c` to `705ab9f6f8831b6c`. Both numbers are
the *prediction*, and worth stating because a reader could mistake either for a surprise. The
host's shard has no refine job, no raised tier and no open project, so the twelve bytes are
three empty counts and nothing else; and the hash moved **because** the content did not, since
`DurableHash` folds each count before its contents. A format that added three collections
without moving the hash of a shard that has none of them would be the thing to investigate — it
would mean an empty list and an absent one hash alike, which is how a truncated snapshot passes
its own check.

**The replay hash did not move:** `69c58e2751c0df22`, byte for byte E2's, E3's and E4a's, with
Spike 2 confirming Debug and Release agree. Refining is registry state, and the replay scenario
is six ships in a bare World hashed with `ComputeWorldHash` — as at E4a, a slice that moved it
while adding nothing to *that* world would be the anomaly.

The Release tick soak reads 9.330 ms mean / 14.542 ms worst against E4a's 9.635 / 16.971 —
5.4 capped grids per core against 5.2, inside the tripwire — and `AdvanceRefining` runs per tick
now, which is the first thing in this phase that does. It is a walk of a job list that is empty
in this scenario, so the soak says nothing about it yet; the measurement that would is a shard
with jobs running, and E5 is where a screen exists to start them. Content is untouched and the
universe parses in **208 ms** on Release.

**One thing this run is owed a note for, and it is not E4b's.** The Debug leg went red first on
run 175 with 741 passed and nothing failed — a deadlock in `TaskPool::Stop`, in engine code this
slice never touches, which had been sitting in the tree since S2. It is R22's fifth entry, and
the reason it appears in E4b's Built line at all is that a reader comparing run 172's 773 tests
with run 176's 795 should not have to wonder what happened in between.

### E5 — The two screens
The station surface's **CARGO** and **REFINERY** tabs, built to their prints —
[cargo-tab.png](ScreenPrints/cargo-tab.png) and
[refinery-tab.png](ScreenPrints/refinery-tab.png), both landed 2026-08-21: the hull
manifest beside the Bay with stack-wise and TRANSFER ALL moves, the recipe list with its
locked rows explained rather than hidden, the batch picker, the slot and queue state, and the
project board with its contribution history. On the tactical side, the site field itself — the
Nebula pass parameter set for pockets, rock impostors for fields, clusters visibly hollowing as
`SiteStatus` reports them — plus the MINE context action and its MINING chip, HOLD FULL on the
roster strip, and the ore/alloy icons.

**Two of those four have landed, and E5's scope did not say so** *(recorded 2026-08-22)*. **MINE
on the context bar is built** (2026-08-21): `OrderKinds` takes the selection and is asked every
frame rather than once at boot, `OrderKindOption` carries a `reasonCode` so a greyed verb draws
the game's own words, and `RunMineAvailabilityGate` proves it through a real snapshot — which
found `oreUnitLitres` unfilled, so `HoldFull` could never fire. **The MINING chip is built**
(2026-08-22), and it took a word rather than a byte: `OrderWorkingName` answers `Mining` and null
for every other kind, rides in `OrderPreview::workingLabel`, and `OrderGhost::Working()` is the
predicate both draw passes share — so a working order draws no lane, no footprint and no station
ticks, because all three promise an arrival that already happened. No wire change.

**What is left of the tactical half, in the detail the annex carried:**

- **HOLD FULL on the roster strip.** Per-ship litres from `CargoStatus` (owner-only, ~1 Hz,
  capped at `MAX_CARGO_STATUS_ROWS`) as Σ `oreUnits[i] × unitVolumeLitres[i]` — from the parsed
  `EconomyDef`, never re-authored constants — against the hull's `oreHoldLitres`. The tag appears
  at 100 %. Fill shows only for hulls that mine; no cargo bar on a combat hull.
- **Site fullness, minimal.** `SiteStatus`'s per-cluster `clusterFullPct` as thin bars in the
  context zone when a site grid is focused (0–100, saturating — the wire guarantees no wrap), and
  `epoch` checked against the client's own `SiteEpochIndex` so a stale status is tagged
  `LAST EPOCH` rather than drawing yesterday's rocks. The field's *visual treatment* is D-C2.
- **Rails that stay:** atlas quads only (R9), palette through `HudPalette` and no new constants,
  the 48 px verb floor, zone metrics from `UiTuning`, and economy data from the summaries only —
  never `EntityRecord`, which ADR-024 §4d refused and a test asserts.

*The record of the four pieces above — including the two decoder and validation-view defects
found ahead of them — is [Archive/prompt-hud-economy.md](Archive/prompt-hud-economy.md), closed
and archived 2026-08-22 when its remaining scope moved here.*

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
  **Where it is *implemented* is E4a** (added 2026-08-21, **built the same day**): the ADR gated
  E2's design and E2 wrote its ledger to the shape ADR-025 named, but no journal, snapshot or
  store existed in the tree until E4a — a deliverable being accepted is not the same as its
  code being built, and this line says so where the next reader will look for it.
- ~~**D-P2 — The CARGO tab print.**~~ **Delivered 2026-08-21:**
  [cargo-tab.png](ScreenPrints/cargo-tab.png), with its source beside it. The hull-and-Bay
  transfer surface, drawn in the P1 pattern and against ADR-024 §5c's manual-transfer
  ruling: the docked hulls' manifests on the left, the Bay on the right, an ore filter and one
  primary verb between them. It **still gates E5**, and it now carries four questions of its
  own that want owner rulings first — listed below, the same way P1's four were.

  **What the print decided, which the ADR left to it.** Six calls, and the first is the one
  with the longest reach:

  - **The vocabulary is item stacks; ore is only the first family.** Every readout on the tab
    is an `(item, units, litres)` triple — the manifest columns, the Bay's rows, the MOVE
    chips, the fill preview — so a wider item taxonomy grows the screen **by rows and chips,
    never by redesign**, and no label says "ore" where "item" is meant. *(That taxonomy is
    named by the print and has no ADR in this corpus — see ADR-024 §5d's caveat. It blocks
    nothing here, since ore is the only family E1–E5 ships.)* One consequence is
    pre-authorised rather than met by surprise: per-item manifest columns hold to about five
    item types, past which they collapse to stacked chips per hull.
  - **Units are the number, litres are the cost**, and every stack reads twice — unit volumes
    differ per ore (300/200/250 L), so either number alone lies. The load bar is **property,
    not a gauge**: ADR-017's no-bars rule was about *damage*, and this is the amendment saying
    so out loud rather than by omission.
  - **Transfers move selections, not slots.** The gesture is the hangar's — tap hulls
    additively, pick an ore, one verb — and the only computed split is fill-to-capacity, by
    the same pure pre-check the server validates with. A slot grid would promise per-slot
    placement that the roster-order rule does not keep.
  - **Direction follows the selection**, and both verbs stay visible with the inactive one
    disabled and reasoned: they are one `MoveOre` with the sign flipped, and the screen says
    so rather than pretending they are two systems.
  - **The Bay draws no meter** — it has no capacity in the content and no epoch to go stale
    against, so a meter would invent a scarcity nobody designed. The same argument that kept
    the dock-capacity meter off P1.
  - **Pending is a mark, not a guess.** The command acks at once and the confirming numbers
    arrive on the next ~1 Hz `BayStatus`; in the gap the moved stack shows `◌` beside its
    **old** value. A screen that guesses a ledger invites the ledger to disagree with it, and
    this corpus refuses client-side truth about property everywhere else.

  **Empty holds collapse to one line.** Sixty ships dock and three hold ore, so combat hulls
  with nothing aboard are one summary row with a SHOW rather than sixty chips — the same
  economy that let P1 fit the hangar without a scrollbar. Industrials stay visible
  individually, because a Hauler reading READY is information: it is the fill target.

  **Open — four rulings owed before E5 builds** (the P1 pattern; unanswered questions in a
  print are how a screen gets built twice):
  1. **Ore order on a fill.** Drawn as ore-index order (F-C → AST → NEB); value-density
     order would favour the rare ores. It is player-visible arithmetic, so it wants a rule.
  2. **A dock shortcut.** §5c makes the transfer manual; "TRANSFER ALL from the fleet that
     just docked" as a one-tap toast action may still earn its convenience. The print proposes
     yes, as an explicit tap — still manual.
  3. **Where `CargoStatus` surfaces.** In-space holds (HOLD FULL) belong to the tactical roster
     strip and never to this tab. Proposed here so E5 does not double-home it.
  4. **Pending treatment.** The `◌`-beside-the-old-value above, versus applying optimistically
     and reconciling on `BayStatus` — which reads faster and lies occasionally.
- ~~**D-P3 — The REFINERY tab print.**~~ **Delivered 2026-08-21:**
  [refinery-tab.png](ScreenPrints/refinery-tab.png), with its source beside it. Recipes,
  batches, slots, queue and the project board — §6 on a screen, and the last artefact gating
  E5. It inherited a frame rather than starting from one: D-P2 had already fixed the tab row it
  joins and the item-stack vocabulary its recipes eat and produce.

  **What the print decided, which the ADR left to it.** Five calls, and the first two reach
  past the screen:

  - **Locked rows are the industrial map, so they are explained and never hidden.** Every
    recipe is always listed, and a locked row says **which lock and what changes it** — two
    different sentences. A *tier* lock points at the project board directly below it ("TIER 2 —
    THE PROJECT BELOW"): buildable, communal, this screen's own loop. A *band cap* points out
    of the station entirely ("NEVER IN HIGH-SEC — LOW / NULL ONLY"), in amber, because it is
    **geography rather than progress**. Hiding locked rows would hide the reason to fly
    anywhere.
  - **Offline is the feature, and the screen says so as permanent chrome.** The refinery is the
    game's first walk-away loop and a player who does not know it will watch a 22-minute bar.
    So: the offline line is chrome rather than help text, ETAs are **wall-clock, not
    session-clock**, and a job finishing while away lands in D19's event record for the away log
    — **this tab never owes a notification**, which keeps it honest at any staleness.
  - **The form is arithmetic, shown before commit** — four lines, always the same four: inputs
    now *with the Bay's after-state*, output at completion, time with the batch and tier
    discounts named rather than folded, and refund shown even at zero, because "TIER 2 WOULD
    RETURN 5" is the upgrade pitch in one line. The after-state earns its place: §6b debits at
    submission, so what remains is the number actually being decided with. No probability
    appears anywhere on the tab.
  - **Slots are yours, not the station's.** Per-player slots and a queue of ten mean no station
    traffic to browse and no queue-jumping UI to design, and `RefineryBusy` is always about
    your own ten — the same privacy the roster and the Bay already keep.
  - **The project board is communal and irreversible**, which is why contribution sits behind
    its own confirm while QUEUE does not, and why its preview **clamps at what remains**: the
    project completes exactly once, so the screen must never offer units it will not take.

  **Open — four rulings owed, and one of them lands earlier than the rest:**
  1. ~~**Job cancel**~~ — **answered 2026-08-21, and built with E4b.** The owner took the
     print's proposal: a **queued** job cancels whole and its inputs return to the Bay
     untouched; a **running** job cannot be cancelled, its inputs being spent. Two states with
     one rule each, and nothing is created or destroyed by a cancel. Recorded in
     [ADR-024 §6b](ADR/ADR-024-mining-economy.md), which was silent, and enforced by
     `AQueuedJobCancelsWholeAndARunningOneCannot`. **Three rulings remain, all E5's.**
  2. **What a job is on the wire.** Drawn as one batch per job with the queue holding many; if
     E4b spells `(recipe, batchCount)`, the composer gains a count and the queue rows collapse
     — same screen, one field. **E4b has since built it, and it did not take that branch:** a
     `RefineJob` carries one `batchUnits` — the batch's *size*, 1, 10 or 50, which is what
     multiplies the refine time — and the queue holds up to ten of them. So the composer gains
     nothing and the rows do not collapse: **the screen builds as drawn.** Recorded as what was
     built rather than as a ruling; the owner may still prefer the other reading, and changing
     it now would be a wire change rather than a screen one.
  3. **Contribution granularity.** Whole stacks with a clamp, as drawn, versus a partial-amount
     picker. The print proposes no: it would be the only quantity keypad either economy print
     has.
  4. **Where the away log lives.** Completions land in D19's record; the print proposes the
     alerts family owns telling the player (a toast on login) and this tab only ever shows
     current state.

**Deliverable status, 2026-08-21.** Of the four design deliverables this phase tracks,
**three are closed** — D-P1 (the persistence ADR, accepted 2026-08-20), D-P2 and D-P3 (both
prints, delivered today). The content deliverables D-C1–D-C3 remain, and they are art and audio
rather than design. **What is still owed against the prints is eight owner rulings, not
artefacts**, and they are listed with each print above: four from D-P2, all owed before E5; and
four from D-P3, of which **job cancel was owed before E4b and is answered** (2026-08-21). The
seven that remain can be answered any time before the screens build, which is the sequencing
consequence worth carrying forward: nothing left in this phase is gated on a ruling.
- ~~**D-C1 — Ore and alloy icons**~~, in the tactical icon system's families, plus the three
  archetype glyphs the strategic and system maps need. **Delivered 2026-08-22:**
  [item-icon-system.png](ScreenPrints/item-icon-system.png), with its source beside it — three
  ores, five alloys, three site archetypes, and **four form rules written for the item families
  that do not exist yet**: raw filled / refined outlined (the family channel, read before
  identity), silhouette carries identity so grayscale must pass, the 20 px floor with the
  `FC` · `AC` · `NB` letter codes below it, and **hue by majority input** — an alloy wears its
  majority ore's hue and a tie wears refined silver, so §6a's recipe table *is* the hue table
  and a new recipe needs arithmetic rather than a colour meeting. Both economy plates were
  re-captured the same day to adopt the glyphs, and the strategic map's new site layer takes
  the ore hues without a redesign. The ruleset is recorded in
  [ADR-024 §5d](ADR/ADR-024-mining-economy.md); it raises no ruling of its own.
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
