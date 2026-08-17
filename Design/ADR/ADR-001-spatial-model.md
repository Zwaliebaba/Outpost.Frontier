# ADR-001 — Spatial Model: Planar Simulation, 3D Presentation

**Status:** Accepted · 2026-08-17
**Depends on:** — (root decision)
**Feeds:** every other ADR · extended by ADR-009 (universe coordinates)

## Context

The game must read like Darwinia (fixed-ish isometric camera, flat-shaded 3D objects), command
like Homeworld (fleet orders, formations), and grow into an EVE-like persistent universe. The
first structural choice is what space the *authoritative simulation* lives in: a 2D plane or a
3D volume. Everything inherits from this: pathing, formation solve, picking, camera, wire size,
HUD overlay geometry.

Evidence already in the design corpus (Design/ScreenPrints):

- `puck-and-wheel.png` §2: *"Planar movement is what made this cheap … now a screen point maps
  to exactly one world position"* — the entire order-puck interaction (drag on the plane, second
  finger spent on **arrival facing**, not altitude) assumes a plane.
- `tactical-hud.png` / `tactical-icon-system.png`: selection rings are *"2:1 ellipse **on the
  plane**, not billboarded"*; formation footprints are station ticks on the plane; the movement
  puck is a ground ellipse.
- `overlay-pass.png`: occlusion rule is *"plane-lying classes only"*.

The corpus, in other words, has already spent its interaction budget on planar assumptions. This
ADR either ratifies that or overturns it — silently doing both is the one wrong answer.

## The argument

**For a 3D volume (Homeworld):** true z-axis tactics — altitude splits, sphere formations,
attacking from below. It is the most distinctive thing Homeworld did.

**Against it, under *this* camera:** Homeworld made 3D work with a freely pitching camera plus a
dedicated movement disc UI (plane + altitude handle). We have a fixed-elevation isometric camera
as an identity constraint. Under isometric projection, altitude and ground position are
degenerate: a ship at `(x, y, h)` projects onto the same pixel as a ship at a different `(x', y')`
with `h = 0`. Disambiguating requires drop-lines, shadows, or camera pitch — i.e. giving up the
visual identity or re-adding the UI machinery the corpus explicitly deleted ("a reference plane,
an altitude handle and a drop-line"). Selection, box-select, and click-to-move each acquire a
depth-ambiguity problem that has sunk isometric-3D games before. Netcode and interest management
also pay: 3 position + 3 velocity axes, orientation as quaternion vs a single heading.

**Does 2D foreclose the long-term vision?** Checked against PVP/PVE/MMO: EVE's combat is
effectively planar — its tactics come from range control, transversal speed, positioning and
timing, none of which need z. Formation play (line, wedge, claw, echelon — all in the corpus
sub-wheel) is planar. PVE encounter design on a plane is standard RTS territory. Multi-client
servers are unaffected. Nothing structural is foreclosed; what is genuinely lost is z-tactics as
a mechanic, and that loss is permanent — accepted with eyes open.

**Self-challenge:** could we keep "2.5D bands" (a few discrete altitude layers) as a hedge?
Rejected: bands re-introduce per-layer picking and readability cost for a mechanic nobody has
designed, and they can be retrofitted later as *separate planes* (grids/instances, EVE-style)
without touching the core model. Do not pre-pay.

## Decision

1. **The authoritative simulation is 2D.** GameLogic state is positions/velocities in a plane
   (`float2 pos`, `float2 vel`, `float heading`). There is no simulated altitude.
2. **Presentation is 3D.** The client renders true 3D meshes on that plane and may add
   *cosmetic* vertical offsets (per-class hover height, banking roll into turns, idle bob).
   Cosmetic z is derived client-side from replicated planar state and is never replicated,
   never simulated, never pickable.
3. **Coordinate conventions (normative):**
   - Sim space: right-handed 2D, `x` east, `y` north, metres, `heading` in radians CCW from +x.
   - Render space: `world = (sim.x, h_cosmetic, sim.y)`, +Y up. Mesh forward axis is −Z (as
     authored in `GameData/Meshes`); model yaw maps heading onto −Z.
   - One play area ("grid") per session for MVP: 40 km × 40 km centred on origin. `float32`
     gives ~5 mm resolution at that extent — ample. Galaxy-scale later = many grids, each
     anchored at an exact `int64` universe position; see **ADR-009**, which makes this
     concrete (universe plane in integer metres, systems as graph nodes). Planar float32
     grids therefore do not block MMO scale.
4. **Everything that targets space targets the plane:** move orders are plane points + arrival
   facing; picking is a cursor-ray ∩ plane point followed by 2D proximity tests; formation
   stations are 2D offsets; AoE later is 2D discs.

## Alternatives rejected

- **Full 3D volume** — depth-ambiguous under a fixed isometric camera; re-adds the
  altitude-handle UI the corpus deleted; doubles state and solve complexity; z-tactics deliver
  nothing in an MVP with no combat. Rejected for readability and cost, not feasibility.
- **3D sim constrained to a plane by gameplay code** (sim in 3D, z clamped) — pays 3D costs
  (quaternions, 3D steering, 3D wire format) to get 2D behaviour; worst of both. Rejected.
- **Discrete altitude bands** — speculative mechanic, permanent UI tax; retrofittable as
  separate planes if ever wanted. Rejected for MVP and not reserved for.

## Consequences

- Pathing, steering, and formation solve are 2D — cheap, robust, testable.
- Picking/selection is exact and unambiguous; no depth-cue UI needed. The isometric-readability
  risk (Risk R1) shrinks to icon/de-clutter design.
- Snapshot per-entity kinematic state is 2 × pos + 2 × vel + heading (see ADR-004 for
  quantisation) — roughly half the 3D payload at the 1,024-entity replication cap.
- Homeworld-style z-tactics are permanently out. The fleet-command identity must come from
  formations, stances, and positioning on the plane.
- Cosmetic z must stay cosmetic: any future feature that makes hover height matter to gameplay
  violates this ADR and must reopen it.
