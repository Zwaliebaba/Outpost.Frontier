# Screen consistency audit — 2026-08-21

**Status: ARCHIVED, 2026-08-21 — the day it was written.** An audit is finished when its
findings have been acted on, and this one's were the same day: the four inconsistencies were
fixed in the prints, and everything it found that this corpus needed to *keep* has been lifted
into the documents that govern — the tab-row verdict into
[ADR-017 §6](../ADR/ADR-017-station-docking.md), the wheel's eight sectors and MINE as a
context verb into [ADR-024 §4a](../ADR/ADR-024-mining-economy.md), and the inventory of
surfaces with no print into [README.md](../README.md). One finding outlived the fix and is
recorded there rather than here: the item taxonomy this audit's source document cites has no
ADR in this corpus, so D-P2's "ore is the first family of many" is currently uncitable.

It stays readable because it is the evidence — the same reason
[Scaling-Readiness-Review.md](Scaling-Readiness-Review.md) does. Nothing owed lives only here.
The one live document it points at, [prompt-hud-economy.md](../prompt-hud-economy.md), is still
at the top level because its work is not done.

**A caveat for anyone reading it later.** The scope line below describes `main` as it stood in
the project this audit was written in; the branch state it names arrived in this repository
afterwards, so a reader checking the tree against it should check the date rather than assume.

---

Scope: all 12 design documents in this project, cross-checked against each other and against
`main` as built (E1a–E3 green, run 161; E4/E5 pending). Trigger: D-P2/D-P3 landing after eight
screens were already designed and the tactical HUD already implemented.

## Fixed in this pass

| Screen | Inconsistency | Fix |
|---|---|---|
| Station Screen (P1) | Tab row predated the economy: HANGAR/REPAIR/REFIT/CONSTRUCT/MARKET — no CARGO or REFINERY, though D-P2/D-P3 show both as siblings | Tab row now matches D-P2/D-P3 (7 tabs, CARGO + REFINERY T1); §2 tabs card names built vs unbuilt services; plate re-captured |
| Puck and Wheel | `OrderKind::Mine = 6` is real since E2; the print's eight-sector wheel and verb inventory were silent on where MINE lives | Proposal recorded (dated, marked ◻ for review): MINE is a context-bar verb like DOCK, the wheel stays eight, ore filter defaults to ANY |
| Session Surfaces | "WHILE YOU WERE AWAY" digest used an invented industry row ("8× Frigate hull" — no such system) | Row now names the real system: "Refine complete — 50× Ferrocite Plates into the Vesta-3 Bay" (E4's offline completion into D19) |
| Alerts and Toasts | Taxonomy's ROUTINE examples predated mining; HOLD FULL (R5's event-record entry) had no alert class | ROUTINE now includes "refine batch complete · hold full at the cluster (R5)" |

Consistent already, verified: palette/type identical across all 12 (single phosphor family,
Share Tech Mono); station chrome (status bar, clock, tab pattern) identical across P1/D-P2/D-P3;
D-P2 ↔ D-P3 agree on tier tag, Bay contents, privacy language; Alerts' WALL-TIME class already
covers "long industry job finished while away" (now the refinery, no change needed).

## Implemented-code impact → prompt-hud-economy.md
The tactical HUD is live C++ and shows nothing of the built economy. [prompt-hud-economy.md](../prompt-hud-economy.md)
(new) scopes the four additions for Claude Code: MINE context verb (reason parity 17–19),
MINING chip via the LegEtaSeconds seam, HOLD FULL on the roster strip from CargoStatus,
minimal SiteStatus cluster bars. Station tabs stay E5's, to the D-P2/D-P3 prints.

## Missing screens / deliverables (the honest inventory)

Designed, unpushed upstream:
- 07i debug HUD (answers F11) + plates session-surfaces.png, puck-and-wheel.png, debug-hud.png
- P1, D-P2, D-P3 plates — in the push/ package now

Undesigned, unblocked (in suggested order):
1. **Market (07 §4.3)** — unblocked by 08c; browser taxonomy still an open UI question; largest and least specified
2. **Fleet management (07 §4.5)** — unblocked by 02a (FleetTemplateRow, ≤16 squadrons, all-or-nothing deploy)
3. **Container surface** (no row in 07 §4's inventory) — D-P2 answers hull↔Bay only; wreck loot, escrow,
   Deploying and the other 08c container kinds still have a data model and no UI
4. **D-C1** — ore/alloy icons in the icon system's families + three archetype glyphs (feeds the maps and both economy tabs)
5. **Strategic/system map site layer** — sites are baked anchors and `SiteEpochPlacement` is client-callable,
   so the map can draw today's fields; 07f predates sites entirely (design change to an upstream doc — needs its own pass)
6. **Character / skills** — no data model yet; correctly not designable

Undesigned content (not screens): D-C2 site field visuals, D-C3 mining/refining audio.

## Plate status
- station-screen.png — re-captured this pass (tab row)
- cargo-tab.png, refinery-tab.png — current
- session-surfaces.png, alerts-and-toasts.png, puck-and-wheel.png — text-level edits above sit in
  doc/table regions; layouts unchanged. Re-export owed only if upstream treats those regions as spec.
