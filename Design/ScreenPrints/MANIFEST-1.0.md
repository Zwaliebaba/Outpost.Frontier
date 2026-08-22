# ScreenPrints — MANIFEST 1.0

**Version: 1.0 · 2026-08-22 · the complete plate corpus.** Seventeen plates, sixteen sources,
one runtime (`source/support.js`). This manifest is the baseline every future delta is tracked
against: from here on, no plate changes without a version bump on its row and a line in the
CHANGELOG below.

## Versioning rules
- Each screen carries its own version, starting at **1.0** in this drop.
- **Minor bump (1.0 → 1.1):** any visible change to the plate — copy, layout, a new state,
  a chrome fix. Re-capture the PNG from the source in the same commit; a plate and its source
  must never disagree.
- **Major bump (1.x → 2.0):** the screen's design calls change — a §2 decision reversed, a
  zone moved, a rule replaced. Requires the corresponding ADR note in the same commit.
- A source edit with **no visible effect** (comment, tweak metadata) bumps nothing.
- The CHANGELOG in this file is append-only; the manifest table always states current versions.

## The corpus at 1.0

| # | Screen | Plate | Source | v | Status / grounding |
|---|---|---|---|---|---|
| 07a | Tactical HUD | tactical-hud.png | Tactical HUD.dc.html | 1.0 | Built (S11); code audited 2026-08-21 — ../Archive/prompt-hud-economy.md carries the deltas |
| 07b | Tactical Icon System | tactical-icon-system.png | Tactical Icon System.dc.html | 1.0 | Built (IconSystem); three-channel rule, 20 px floor |
| 07c | Overlay Pass | overlay-pass.png | Overlay Pass.dc.html | 1.0 | Built (OverlayPass); compositing rules D-P6 obeys |
| 07d | Alerts and Toasts | alerts-and-toasts.png | Alerts and Toasts.dc.html | 1.0 | Built (ToastStack); dwells verified against code 2026-08-21 |
| 07e | Session Surfaces | session-surfaces.png | Session Surfaces.dc.html | 1.0 | Designed; away-digest names real economy systems |
| 07f | Strategic Map | strategic-map.png | Strategic Map.dc.html §1 | 1.0 | Designed; plate re-captured this drop (post-consistency-pass state) |
| 07f+ | Strategic Map — site layer | strategic-map-sites.png | Strategic Map.dc.html §5 | 1.0 | **New**: fifth overlay RESOURCES (ADR-024 §3d); closes inventory item 5 |
| 07g | Puck and Wheel | puck-and-wheel.png | Puck and Wheel.dc.html | 1.0 | §2 built as a mouse adaptation (`OrderPuck`); **§1's modality table and §3's wheel are not built** and are scheduled as I3 ([Plan-of-Record](../Plan-of-Record.md)); MINE-placement ruling recorded (wheel stays eight) |
| 07i | Debug HUD | debug-hud.png | Debug HUD.dc.html | 1.0 | Built (DebugStrip); answers F11 |
| 07h | Settings | settings.png | Settings.dc.html | 1.0 | Designed; unimplemented (menu SETTINGS drawn dead) |
| P1 | Station Screen | station-screen.png | Station Screen.dc.html | 1.0 | Delivered; rulings folded into ADR-017 §6a; 7-tab row |
| D-P2 | Cargo Tab | cargo-tab.png | Cargo Tab.dc.html | 1.0 | Delivered (E5's spec); D-C1 glyphs adopted this drop |
| D-P3 | Refinery Tab | refinery-tab.png | Refinery Tab.dc.html | 1.0 | Delivered (E5's spec); D-C1 glyphs adopted this drop |
| D-C1 | Item Icon System | item-icon-system.png | Item Icon System.dc.html | 1.0 | **New**: 3 ores + 5 alloys + 3 archetypes; closes inventory item 4 |
| D-P4 | Market Tab | market-tab.png | Market Tab.dc.html | 1.0 | **New, forward design** — market phase has no ADR; closes inventory item 1 |
| D-P5 | Fleet Management | fleet-management.png | Fleet Management.dc.html | 1.0 | **New, forward design** — fleet-template design is upstream; closes inventory item 2 |
| D-P6 | Container Surface | container-surface.png | Container Surface.dc.html | 1.0 | **New, forward design** — wreck/escrow/Deploying; closes inventory item 3 |

Not in this corpus, correctly: **character/skills** (no data model — the one inventory item
that stays open), D-C2 (site field visual treatment), D-C3 (economy audio), D1 System View
(tracked upstream already, plate owed there).

## Conventions every plate obeys
- Authored 1440×900, captured at 1344×840 (0.9333 scale), PNG.
- The plate is the screen; the argument lives in the `.dc.html` beside it (§1 = the screen,
  §2 = design calls, §3 = absent/open). Do not read a plate's silence as the design's.
- Token values from 07-ui-tokens (phosphor ramp, semantic five); item hues from D-C1.
- Forward-design prints (D-P4/D-P5/D-P6) state their posture in an amber banner and list the
  owner rulings their future ADR needs — they are the UI half of ADRs not yet written.

## CHANGELOG
- **1.0 (2026-08-22)** — baseline. Seventeen plates: the twelve existing screens re-baselined,
  cargo/refinery re-captured with D-C1 glyph adoption, five new artefacts (site layer, D-C1,
  D-P4, D-P5, D-P6). Inventory items 1–5 closed; item 6 (character/skills) stays open.
