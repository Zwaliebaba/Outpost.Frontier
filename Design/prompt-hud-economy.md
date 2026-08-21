# Prompt — economy on the tactical HUD (post-E3)

For Claude Code, working in `Zwaliebaba/Outpost.Frontier@main`. The mining loop is built and green
through E3 (run 161): `OrderKind::Mine = 6` with a trailing `u8 oreFilter` on `OrderSubmit`,
`OrderReason` 17–19 (`NotAtSite`, `NoMinerInOrder`, `HoldFull`), and the three ~1 Hz summaries in
`GameLogic/EconomyMessages.h` (`SiteStatus` public, `CargoStatus`/`BayStatus` owner-only). The HUD
in `ClientApp::BuildHud` + `UiTuning` (`UiLayout.h`) + `UiDrawList` shows none of it. This prompt
adds the tactical-side economy surface, matching the design prints (`Design/ScreenPrints/*.png`;
interactive sources under `source/`). Station tabs (CARGO/REFINERY) are **out of scope** — E5
builds those to the D-P2/D-P3 prints.

## Rails (unchanged from prompt-hud-migration.md)
Atlas quads only (R9). Palette via the `HudPalette` table keyed by `client.ui.palette` — no new
constants. 48 px verb floor. Zone metrics from `UiTuning`. Cargo/economy data comes ONLY from the
summaries — never from `EntityRecord` (ADR-024 §4d refused the cargo byte; the test asserts it).

## 1 — MINE verb on the context bar
Appears when the current grid has a site field AND the selection holds ≥ 1 Miner; enablement runs
the shared pure validation (same seam as existing verbs — bounce parity), so a tappable MINE is a
promise. Disabled states carry their reason text, mapped from the shared checks that produce
`NotAtSite` / `NoMinerInOrder` / `HoldFull`. Submit is an ordinary `OrderSubmit` with `kind = 6`
and `oreFilter = Any (0)` — no ore picker in v1; the server assigns the richest matching cluster.
Per the puck-and-wheel print's 2026-08-21 proposal: MINE is a context-bar verb, NOT a ninth
command-wheel sector — the wheel stays eight.

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
