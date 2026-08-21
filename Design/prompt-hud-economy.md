# Prompt — economy on the tactical HUD (post-E3)

For Claude Code, working in `Zwaliebaba/Outpost.Frontier@main`. The mining loop is built and green
through E3 (run 161): `OrderKind::Mine = 6` with a trailing `u8 oreFilter` on `OrderSubmit`,
`OrderReason` 17–19 (`NotAtSite`, `NoMinerInOrder`, `HoldFull`), and the three ~1 Hz summaries in
`GameLogic/EconomyMessages.h` (`SiteStatus` public, `CargoStatus`/`BayStatus` owner-only). The HUD
in `ClientApp::BuildHud` + `UiTuning` (`UiLayout.h`) + `UiDrawList` shows none of it. This prompt
adds the tactical-side economy surface, matching the design prints (`Design/ScreenPrints/*.png`;
interactive sources under `source/`). Station tabs (CARGO/REFINERY) are **out of scope** — E5
builds those to the D-P2/D-P3 prints.

## 0 — The decoder, first *(built 2026-08-21)*

Not in the original scope, and it had to come first: the client's `ApplySummary` was written
when the summary family had two kinds, and E3 added three. The frame carries **no length
prefix**, so a body the decoder does not read is not a record skipped — it is every record
after it misparsed. The client was dropping whole frames and losing T2's docked blocks and
dock/undock toasts with them.

**Latent, not firing:** the starting world has no site, no cargo and no Bay, so only the two
old kinds were ever sent — which is exactly why nothing noticed, and why items 3 and 4 below
would each have tripped it on their first frame.

Fixed with all five kinds handled, no `default:`, and `#pragma warning(1 : 4062)` scoped to the
switch — the compiler could not warn before because C4062 is level 4 and this tree builds at
/W3, so the next kind added to the family is now a build break in the file that must handle it.
Verified both ways: removing a case fails the build with C4062 naming it, and the pre-fix
decoder fails the new gate on all five checks.

`selfTest` gained **`RunSummaryFamilyGate`** — a five-kind frame written the way the server
writes one, through the real decoder. It lives there rather than in a unit test because
`ReplicatedWorldView` is in the executable and has no test project, which is the other half of
why this got through.

## Rails (unchanged from prompt-hud-migration.md)
Atlas quads only (R9). Palette via the `HudPalette` table keyed by `client.ui.palette` — no new
constants. 48 px verb floor. Zone metrics from `UiTuning`. Cargo/economy data comes ONLY from the
summaries — never from `EntityRecord` (ADR-024 §4d refused the cargo byte; the test asserts it).

## 0a — The validation view, which was empty *(built 2026-08-21)*

Also not in scope, also had to come first, and worse than the decoder: the client built its
`ValidationView` with **ids and nothing else**. No marks, no station, no site, no hold room. So
the shared validator refused every client-side Dock with `UnknownStation` — and T2's approach
chain waits on exactly that pre-check, by design, so *the DOCK verb never fired*. The chain
started, the fleet flew, the chip showed, and the order never went.

`ApproachChain`'s unit tests could not see it: they exercise the chain against a hand-made
verdict, which is the right scope for them and precisely why the bug was invisible — it lived
one layer below the seam they mock.

`MakeValidationView` now fills everything this side can know: ship marks and hull classes from
the scene, the station's anchor and position, the site anchor from the public `SiteStatus`
summary, and per-ship free hold litres with the ore unit volumes beside them. What it cannot
know it leaves empty, which the validator reads as "the authority decides" — the one thing it
must never do is leave empty a field it *could* have filled, because that turns a check the
authority passes into a refusal the client invents.

Gate: **`RunClientDockPreCheckGate`** — in range accepted, out of range `NotAtStation`, and the
bug's own signature (a station-less view answering `UnknownStation`) pinned so it cannot come
back quietly.

## 1 — MINE verb on the context bar
Appears when the current grid has a site field AND the selection holds ≥ 1 Miner; enablement runs
the shared pure validation (same seam as existing verbs — bounce parity), so a tappable MINE is a
promise. Disabled states carry their reason text, mapped from the shared checks that produce
`NotAtSite` / `NoMinerInOrder` / `HoldFull`. Submit is an ordinary `OrderSubmit` with `kind = 6`
and `oreFilter = Any (0)` — no ore picker in v1; the server assigns the richest matching cluster.
Per the puck-and-wheel print's 2026-08-21 proposal: MINE is a context-bar verb, NOT a ninth
command-wheel sector — the wheel stays eight.

**Built 2026-08-21, with E3 leading.** MINE stays on the command row exactly as E3 shipped it
(`OrderKindHasContent`); the print's ruling reads as being about the unbuilt radial wheel, and
DOCK already sets the precedent of a verb that is both a row kind and a context action. So this
was the gating, not a relocation.

`WorldView::OrderKinds` now **takes the selection and is asked every frame** — it used to be
asked once at boot, which cannot answer "is this verb offerable *now*". `OrderKindOption` gained
a `reasonCode`, so a greyed verb carries the game's own refusal code and the row draws
`ReasonText` on it: the same words the bounce toast would use, because it is the same code. The
availability answer comes from `ValidateOrder` rather than from three checks written on the
client, which is the whole of the parity claim.

Only kinds that **name no destination** are pre-judged this way, and that is a property rather
than a special case for Mine: a Move, a Warp or a Dock is judged partly on where it points, and
there is no point until the gesture happens.

Gate: **`RunMineAvailabilityGate`**, and it is an integration gate on purpose — a real snapshot
through `ApplySnapshot`, a real scene build, real summary frames — because both bugs above were
of the kind where the validator is right and the *view* is wrong, which a validator-level test
passes while the feature does nothing. It found one immediately: `oreUnitLitres` was unfilled,
so the hold check skipped itself and `HoldFull` could never fire.

## 2 — MINING chip on the group
A group whose Mine order is working (Underway with no leg left) shows a MINING chip where a flying
group shows its leg ETA — same `LegEtaSeconds` seam, which already reports the cluster ETA for a
working Mine order (earlier of cluster-dry and last-Miner-full; that arithmetic is server-side,
do not recompute it). Chip text: `MINING · <eta>`. A ship that exits `Hold` (hold full) keeps its
station per R5 — no client-side movement or regroup.

## 3 — HOLD FULL on the roster strip
From `CargoStatus` (owner-only, ~1 Hz, rows capped at `MAX_CARGO_STATUS_ROWS`): per-ship litres =
Σ `oreUnits[i] × unitVolumeLitres[i]` (from `EconomyDef` — the parsed content, never re-authored
constants) against the hull's `oreHoldLitres`. At 100 % the roster row gains a HOLD FULL tag.
Show fill only for hulls that mine (Miner); no cargo bar on combat hulls in the strip.

## 4 — Site fullness, minimal
When a site grid is focused, render `SiteStatus` per-cluster `clusterFullPct` as thin bars in the
context zone (0–100, saturating — the wire guarantees no wrap). Check `epoch` against the client's
own `SiteEpochIndex` result and tag a stale status `LAST EPOCH` rather than drawing yesterday's
rocks. The field's visual treatment (rocks, nebula pass) is D-C2 — out of scope here.

## Accept
- MINE enables/disables in lockstep with the server's refusal reasons (parity test over 17–19).
- A working Mine order shows MINING with a falling ETA; a completed one retires like any Done leg.
- A Miner at 120,000/120,000 L reads HOLD FULL within one summary interval of filling.
- No `EntityRecord` change, no new palette constants, no non-atlas draw call (R9 guard stays green).
