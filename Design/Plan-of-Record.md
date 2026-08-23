# Plan of Record

**Status:** standing document, **revised in place** · opened 2026-08-22 · current as of
2026-08-23 (U3d-a, U3d-b, N2, N5, N4, N6's sizing half, I1, I2, N3, U3d-c, U5a, U6a, **07b's
device-free half and R30's conversion** built). **This is the only document that says what is built next.** The three build orders
say what a slice *contains* and record what happened when it landed; this one says which slice,
and when. Where it and an ADR disagree the ADR wins on *what*, which is the rule the build
orders already run under.

**It carries no date in its name on purpose.** A dated plan is a snapshot, and a snapshot
someone has to notice has gone stale is how a corpus ends up with four documents answering one
question — which is the condition this document was written to end. Revise it here; the
revision log at the foot is the record of what changed.

**What it is:** every item that is designed and not planned, planned and not designed, or built
and not recorded — each with a home. **What it is not:** a new design. Every decision below is
the owner's, or a correction to a document that already disagreed with the tree.

---

## The decisions this document records

1. **The interest/delta slice (ADR-022) is next**, before any further screen work.
2. **Touch is the primary input; the mouse is a development convenience.** This reverses
   [ADR-020](ADR/ADR-020-ui-architecture.md) D15.4 and is the largest item here.
3. **The command wheel is built**, not superseded — it follows from decision 2.
4. **The user layer lands now**; the settings screen becomes a scheduled slice of its own.
5. **The item-taxonomy ADR is written now**, ahead of E5. ✅ **Done** —
   [ADR-027](ADR/ADR-027-item-taxonomy.md), 2026-08-22.
6. **Fill order is content-declared order** — one ruling closing three (ADR-024 §5d, ADR-027 §2).
7. **Per-player state is device-local**, account service as the named reopen trigger — one ruling
   closing three more (ADR-012 §3).
8. **Stance is selection-only; Abilities keep a placement** — the input model's last guess
   replaced by a decision (ADR-020's amendment).
9. **The wing lifecycle is reversed: membership is in-space membership** — owner ruling from
   the 2026-08-23 fleet design review, recorded as
   [ADR-017 §6b](ADR/ADR-017-station-docking.md): the wing forms at the **undock**, a dock
   writes **memory**, and a wing with nothing in space ends, its row with it. Delivery is
   **T4** ([Station-Build-Order.md](Station-Build-Order.md)), sequenced below.

---

## 1. The input model — the phase nobody wrote down

**The finding.** `puck-and-wheel.png` is a **touch** design, and so is the corpus around it.
This repository built a mouse adaptation of one corner of it and never recorded that it had
done so. `OrderPuck.h:21` states the adaptation plainly — *"The print draws a touch gesture and
this is a mouse"* — but no build order carries the other corners, and D15.4 went further and
refused to prepare for them: *"No pointer abstraction beyond the mouse is built, reserved or
hinted at."*

That clause is now the thing standing between the design and the product, so it is reversed
below rather than eroded quietly.

**What the print already decided** (§1's modality table, §2, §3 — normative once lifted into
ADR-020 by this document's companion amendment):

| Toggle | long-press empty space | long-press an entity | long-press own selection |
|---|---|---|---|
| **MOVE** *(default)* | PUCK | PUCK — move to it | PUCK |
| **ATTACK** | nothing — invalid | WHEEL ▸ pre-armed | WHEEL |
| **ABILITY** | PUCK — targeted ground | WHEEL ▸ abilities | WHEEL ▸ abilities |
| **AUTO** *(hit-test fallback)* | PUCK | WHEEL | WHEEL |

- **Long-press opens the order surface**; the toggle decides which. **Auto is a setting and not
  the default**, and the print says why from a shipped failure: *"in a dense engagement, whether
  twelve squadrons received* move here *or* attack that *depended on where a finger landed."*
- **The puck is two-finger** — one places, the second twists arrival facing. The mouse sequences
  it as anchor-then-drag because a mouse has one pointer.
- **The wheel is eight sectors**, rotated so HOLD sits under the holding thumb, handedness from
  the settings screen, sub-wheels cascading outward with the parent dimmed and retained, sliding
  back inside cancelling without a lift, and **the hub carrying the pre-check's illegality reason
  while a finger rests on a greyed sector**.
- **A held second finger appends** to the queue; the chip teaches that queuing exists.
- **Pinch on a selected probe ring** steps its radius — the touch-native form of that ladder.
- **48 px target discipline** is already normative (ADR-020 §8) and is a touch floor.

**What the corpus never designed, on any input: selection.** ADR-006 §11 designs *picking* —
the plane mapping, the point-pick radius, the box parallelogram — and stops there. A grep for
"control group", "select all", "double-click", "marquee" and "band select" across all sixteen
sources returns nothing. The three affordances in the tree (point pick, box drag, roster-row
press) arrived inside slices as implementation decisions. **The gesture that starts every order
is the one thing with no design.**

**The selection model this plan adopts**, sized against the gesture budget the print has already
spent (long-press → orders, second finger → facing, held second finger → append, pinch → zoom and
probe radius):

1. **Tap selects, drag pans.** Tap a hull to take it, tap empty space to clear, drag anywhere to
   move the camera. Free, because long-press already means *command* — no modifier and no mode.
2. **The roster panel is the fleet picker.** A row press already takes the wing
   (`ClientApp.cpp:783`). What changes is that it **stops moving the camera**: framing becomes a
   double-tap, so a single tap can take a fleet without the plane moving under the order that
   follows. The unconditional `FocusOn` at `ClientApp.cpp:855` is the defect.
3. **Wings are the control groups.** `AssignWing` exists with emergent ids 1..255, wings are
   server-side, and they survive restart and reconnect for free — so this needs no user-layer
   storage, no keybindings, and no answer to the device-local-versus-account ruling. ~~**It is
   docked-scope today** (`Station.cpp:237`), and the code comment already anticipates the lift:
   *"in-space reassignment can arrive later without new machinery."* That lift is in scope
   here.~~ **Lifted 2026-08-23** — and the comment was right: `RosterView` grew a grid beside
   its station, `NamesShips` split into a second question, and no wire, verb or entity moved.
4. **Long-press on a roster row adds to the selection.** The world's long-press is spent; the
   roster's is not, and T3b's press-versus-hold reasoning already establishes the idiom where it
   collides with nothing.
5. **Box-select is dropped**, with two-finger drag reserved for it. It has no home once drag is
   the camera, the print corpus never drew one, and wings make a marquee largely redundant.
   Reserved rather than refused, because "we never drew it" is not the same as "it is wrong".

> **Rules 1, 2, 4 and 5 built 2026-08-23.** A tap takes what is under it and a tap on nothing
> clears; a drag that *began over the world* pans, with the same sign the middle-drag already
> used so a finger and a mouse cannot disagree about which way the plane goes; a roster tap takes
> the wing and **no longer moves the camera**, a second tap frames it, and a long-press on the row
> adds it to the selection. `Selection` lost its press/move/release state machine entirely --
> `Gesture.h` owns when a tap happened, so what is left is the set and one function.
> **`PickBox` was kept**: rule 5 reserves two-finger drag for it, and deleting the arithmetic
> would have made the difference between "dropped" and "refused" invisible.
>
> Two decisions the slice had to take. **Framing is the second tap of a double, and `tapped`
> fires on both** — so the first tap acts immediately and the second *adds* the framing, rather
> than every tap waiting out a double-tap window before doing anything. And the pan is gated on
> where the contact went **down** rather than where it is now, which is the rule the box-select
> drag already carried: a gesture may only begin over the world, and once begun it may leave
> freely.
>
> **And what it did not convert, named rather than left to be discovered.** The command row, the
> ability rack and the menu still read a raw left press through `ClaimPointerIn`, so on a touch
> display they are dead until I3 reaches them. That is the scope this slice was given — rules 1
> to 5 are about *selection* and the camera — but it means "I2 landed" must not be read as "the
> client is touch-driven": the world responds to a finger and the chrome does not yet.

**Keyboard bindings become an accelerator rather than the model.** They stay — the settings
screen's keybind capture (D15.3) is unaffected and a desk player will want them — but nothing in
the design may require a key, which is the rule D15.4's reversal has to carry or it will be
re-eroded.

### The slices this becomes

- ~~**I1 — The input seam.**~~ **Built 2026-08-23.** The pointer abstraction D15.4 refused:
  `InputFrame` now carries **contacts** — what is touching the screen, oldest first — and
  `Gesture.h` turns them into the five things the design spends: tap, drag, long-press, second
  finger, pinch. `Window` fills contact zero from the left button, which is the whole of "the
  mouse is expressed *through* it rather than beside it"; the recognizer contains no mouse case.
  `InputRouter` hands the gesture out behind the pointer claim, so a long-press that landed on
  chrome cannot also reach the world.

  **It changes no behaviour, and that is the shape of the slice.** Nothing consumes a gesture
  yet — selection is I2 and the order surfaces are I3 — so the seam lands without a retrofit and
  those become a change of consumer rather than a change of architecture.

  Three decisions it had to take rather than look up. **The dwell is 350 ms**, lifted from
  `puck-and-wheel.png`'s own step 1 rather than invented, and **`dwellProgress` is a level**
  because the print requires the ring to fill "from the first millisecond … so a player who did
  not mean to long-press knows to lift" — a recognizer reporting only the completed press could
  not draw it. **The slop is a quarter of the 48 px target floor**, derived rather than picked so
  the two numbers cannot drift apart. And **the frame carries five contacts while the recognizer
  follows two**: the design spends two, but a palm occupying one of exactly two slots would lock
  out the finger that has meaning, and dropping contacts at the window is how that becomes an
  unreproducible "sometimes the second finger does nothing".
- ~~**I2 — Selection.**~~ **Built 2026-08-23**, in two commits and deliberately: the five rules
  first, then the in-space `AssignWing` lift. Splitting them was worth it — the lift turned out
  to change what a *refusal says* as well as what the authority accepts, and it needed the
  fences (`Undock` and the transfer verbs still require a dock) and a parity gate of its own.
  What it did **not** need was new machinery, exactly as ADR-017 §6 predicted it would not.
- **I3 — The order surfaces.** §1's modality toggle, the two-finger puck, and the wheel with its
  handedness, cascade and reason-carrying hub. Its accept is a visual checkpoint and a person
  with a touch display — the R1 category, and the first slice in this corpus that cannot be
  accepted on the owner's desk alone.

**A named risk, R28's sibling and new with this decision:** no touch device is in the loop
anywhere — not in CI, not in the self test, not on the owner's desk as far as this document
knows. I1 and I2 are device-free and land green regardless; **I3's accept cannot be met without
one**, and that is a procurement question rather than an engineering one. It is named here so it
is a decision rather than a surprise.

---

## 2. Slices to add

| # | Slice | Why it has no home today | Size |
|---|---|---|---|
| ~~**N1**~~ | **Interest & delta** — now **[U3d](Universe-Build-Order.md)**, split a/b/c | ~~no build order absorbed it~~ **Homed 2026-08-22**; it is D6's implementation slice and takes A14's own "after U3c" as its number | large |
| ~~**N2**~~ | ~~**The user layer**~~ — **built 2026-08-22** | ~~ADR-012 §3 calls `Settings.json` "the only file the game writes" and nothing writes it~~ | small |
| ~~**N3**~~ | ~~**The settings screen**~~ — **built 2026-08-23** | ~~ADR-020 §8 names "the settings screen's first slice"; no build order contains it~~ **Keybind capture, which that clause named as the first slice, is the one part still owed** — the touch reversal demoted it | medium |
| ~~**I1**~~ · ~~**I2**~~ **both built 2026-08-23** · **I3** | **The input model** | §1 above | large |
| ~~**N4**~~ | ~~**D18, arrival contention**~~ — **built 2026-08-22** | ~~Baked, parsed, hashed, never read — fell between U1 and U3a~~ | small |
| ~~**N5**~~ | ~~**The viewer hold**~~ — **built 2026-08-22** | ~~`AddViewer`/`RemoveViewer` have no caller for a player's view; the "until U3b" deferral expired~~ | small |
| **N6** | **A20 — spike 3 + the S5 frame check** · ~~**and the upload-ring sizing**~~ **built 2026-08-23** | ~~Stated as "Before U5", no slice, no owner~~ **The measuring half still is**, and it needs a GPU | small |
| **N7** | **The map's RESOURCES overlay** | Drawn 2026-08-22 (ADR-024 §3d); U5's scope was written 2026-08-19 | medium |

**N1 — Interest & delta.** The largest and the one that unblocks the most. It carries
`SnapshotAck` against a ring of views **as sent**, keyframes on a reliable `Bulk` channel, the
relevance hook that ranks in the game and truncates in the engine, priority truncation with an
honest `culledCount`, the owned-and-selected guarantee, relationship bits in two spare
`statusBits`, `lastOrderSeqProcessed` out of the world hash — and **A11's remainder**, the
`EntityRecord` widening that "cannot widen until the delta cluster removes the full-fit
constraint". It retires **R19**, the register's only High/High row, and lifts the disjoint-grid
gate U3c is currently fenced behind. It is headless, which is what makes it the right thing to
do while the input decision above is still turning into screens.

~~**N4 — D18.**~~ **Built 2026-08-22.** `arrivalSpreadRadiusCm` was baked, parsed, folded into
the universe hash and read by nothing; `ApplyTransit` placed every crossing on the raw
`warpInPoint`. Two slices each believed the other had it — U1's note said *"the rule itself is
U3a's"*, U3a's said *"Still owed by U3a: nothing."* The offset is a slot on a ring of that
radius, at a bearing the crossing's own `TransferId` decides through `FixedAngle.h`'s integer
table: consecutive counters step 223.6° — the golden angle the long way round — so a burst filed
in one tick lands 2,228 m apart rather than on one point. **Undock is not covered and that is a
decision**: ADR-017 §6a.1 answered undock contention a day after D18 and differently, with a
clearance predicate and a timeout, and that gate is not built — so undock contention stays open
and belongs to the station phase.

~~**N5 — The viewer hold.**~~ **Built 2026-08-22.** It grew the teeth this line predicted, and
from a direction it did not: not culling, but **teardown**. A player watching a grid with no
ships on it was watching a world that `RankRelevance` spun up and the end-of-tick sweep tore back
down — a whole `World`, its authored occupants and a site's `BuildSiteField` layout, once per
tick, for as long as they looked. It never changed what they *saw*, because a rebuilt grid
resolves its field from the calendar; what the gap cost was the work, and a rule ADR-016 §7
states outright and nothing enforced. The seam is `Simulation::ViewerOpened`/`ViewerClosed` beside `MayView`, reporting the
whole answer rather than a delta so a missed release is not expressible; the hold goes with the
socket rather than with the commander, because a player inside the grace window still owns their
fleet but has no camera.

---

## 3. Design deliverables owed

- ~~**The item-taxonomy ADR**~~ — **delivered 2026-08-22:**
  [ADR-027](ADR/ADR-027-item-taxonomy.md). Drafted from the three citing prints by owner ruling,
  restating rather than referencing because the upstream numbering is not this corpus's. It fixes
  `ItemTypeId` as a `u16`, the `(item, units, litres)` triple with litres derived, families as
  display grouping with content-declared fill order, the fungible/instance split as the market's
  admission rule, three flat categories with a pre-authorised growth rule, and one container
  component whose kinds differ by chrome. **Two deliberate refusals, both in its §6:** the *six*
  container kinds are **not** restated — three are grounded here and normative, the other three
  are nowhere in this corpus and naming them would be inventing them — and nothing in it claims to
  report what the upstream document says. Every inferred decision is flagged, which is what makes
  a print-drafted ADR correctable rather than authoritative by accident.
- **The market ADR** (D-P4's four rulings are its inputs) and **the fleet-template ADR**
  (D-P5's four) — both further out, both named so the prints stop being orphans.
- **Session surfaces (07e) are blocked, legitimately.** ADR-023 says outright *"It does not
  design the account service"* and names it an external dependency. That reason has never been
  written next to the plate; it is written here.
- **P2, D4 and D-C3 — the three audio deliverables.** All were gated on "S15 gives audio a bank
  format"; S15 shipped. P2's text was updated to say so, D4's was not. None has a slice, and
  "deliberately last" is now a choice rather than a dependency.
- **D-C2, the site field's visual treatment** — in E5's scope already; recorded so it is not
  counted twice.

---

## 4. Documentation defects

**Fixed in the commit that adds this document:**

1. **Two risk rows numbered R27.** The epoch/presence row (E2, 2026-08-20) keeps R27 — the
   register's own header, ADR-024 §3d twice and README all point at it. The resume-token row
   (U3c-b, 2026-08-21) becomes **R28**.
2. **`MANIFEST-1.0.md` row 07g claimed "Built (OrderPuck/CommandWheel)".** The puck and the
   command *row* are built; the wheel is not, and under decision 3 it is now scheduled.
3. **`README.md`'s "what is not built is screen work… no exceptions"** omitted four screens:
   07e, 07g's wheel, 07h and 07f+.

**Owed, and not fixed here because each needs a sentence of design rather than a correction:**

4. **`alerts-and-toasts.png` §4's push-notification contradiction** is tracked nowhere — a grep
   for "notif" across the corpus returns four incidental hits and none of them is this. It is
   the only §3/§4 item in seventeen plates with no home. It wants a ruling: the platform
   allow-list has no notification row, and the refinery print's *"this tab never owes a
   notification"* sidesteps rather than settles it.
5. **D3, the name-root lists, is delivered and still listed open.** `UniverseGen.cpp:142` carries
   40 prefixes × 8 suffixes and the committed bake has real region names.
6. **Session Surfaces §3's schema-hash requirement is already met** — `Wire.h:375` ships
   `UpdateRequired{u64 serverSchemaHash, u64 serverContentHash}`. Satisfied, unrecorded.
7. **`OrderKindNamesDestination`'s conservative default** (`Orders.cpp:107`) has Stance and
   Abilities needing a gesture. A posture is not a placement; this wants deciding before combat
   writes content against it, not after.
8. **`MAX_ORDER_KINDS = 8` is justified circularly** — *"the wheel's eight sectors again"*. Under
   decision 3 the wheel is real, so the comment is now true; it should say so on purpose.

---

## 5. Sequence

**Now, in order:**

1. ~~**U3d — interest & delta**~~ — **built: U3d-a and U3d-b 2026-08-22, U3d-c 2026-08-23.** R19 is closed, the shared-grid gate (ADR-018 D3) is lifted, and A11's remainder landed with the `ShipId`/`EntityRecord::id` widening. ~~What is left is **U3d-c's counted chip** — `culledCount` reaches `ReplicatedView::CulledCount()` and nothing draws it yet~~ — **the counted chip landed 2026-08-23, and the rung it was to render through did not exist**: the density ladder is unbuilt, so `CountedChip.h` is that rung, and it could not have been that rung anyway because a culled entity has no position (ADR-022 §5d's note). **The ladder itself was built later the same day at 07b** (`IconDensity.h`), and the two stayed separate for exactly the reason U3d-c gave: one counts what the server did not send and has no position at all, the other counts what the client has and chose to draw as one. What is left of U3d is the **visual checkpoint** — a real count on a grid over budget — which is an R1 item rather than a slice. The client's ack, keyframe and delta-apply paths, which this plan listed under U3d-c, landed with U3d-b because the wire cannot be tested without a reader.

    Two decisions the slice had to take rather than find, both recorded with it: **tier 1 reads as "inside the camera's extent"** rather than ADR-022 §4's literal "a visible relationship *and* inside the extent", because the first conjunct has no producer until the combat phase; and **`ViewFocus` had to be invented** — §4's query needs a focus, an extent and a selection, and §5a's guarantee cannot be kept for a selection nobody told the server about, which amends ADR-016 §7's "the server has no business holding this".
2. ~~**N2 — the user layer.**~~ **Built 2026-08-22.** `Settings.json` is written — atomically,
    and carrying only what the player *changed* rather than a copy of the shipped values, because
    a file that captured the defaults would pin a player to them through a file they never
    edited. Wing names are its first family: a call sign the player composed comes back next
    session, a rename outranks the authored word without overwriting it, and a restored name is
    struck off the spare pool so it is never handed to a second wing. ADR-012 §A4's "backed up
    beside itself" stopped being aspirational with it. **🏁 H1 is not closed by this** — its
    clause needs the rename *control*, which is T3's remainder and is now a gesture rather than a
    dependency. Two things the slice had to decide rather than look up are recorded with it: the
    file records **changes and not state**, which §A3 did not say either way; and names are
    written at **shutdown** rather than at the keystroke, which is the right trade for call signs
    and the first thing N3 has to revisit.
3. ~~**The item-taxonomy ADR.**~~ **Done 2026-08-22** — [ADR-027](ADR/ADR-027-item-taxonomy.md).
4. ~~**N5**~~ and ~~**N4**~~ **built 2026-08-22**, and **N6 split on 2026-08-23**. A20 asks for two
    things and only one of them needs hardware. ~~**Its sizing half is built**~~: the upload ring is
    derived from what the renderer is built to draw and made config, which is the half that
    actually unblocks U5 — the 256 KiB constant it replaced covered the tactical view and left the
    map out, and a short ring drops a whole pass rather than part of one, so U5 would have opened
    on a blank screen. **What is left of N6 is the measuring half** — spike 3 and the S5 frame
    check — which cannot land on CI. N6 before U5 as A20 requires. **N5 moved ahead of N4 on 2026-08-22, and U3d is
    why.** This document listed the
    three as interchangeable and justified N5 as "small, and it grows teeth the moment N1 lands".
    N1 has landed — and U3d-b built the very thing N5 was missing: `ViewFocus` carries a `gridId`
    per session and `SnapshotSender` holds it, so the viewer the registry has no viewer for now
    exists on the wire and on the session with nothing connecting it to `AddViewer`. It got
    cheaper and more load-bearing in the same slice.

**Then the input model, which is its own phase:** ~~I1~~ → ~~I2~~ **both built 2026-08-23** → I3.
I1 and I2 were device-free and landed before a touch device exists; **I3 cannot be accepted
without one**, which makes it the first slice in this corpus blocked on hardware rather than on
work. What is buildable behind it is the screens, re-based on the input model — so the next
question is whether to procure a touch display or to take N3 and the screen slices first.

**Then the screens, unchanged in content but re-based on the input model:** ~~N3 (settings, which
I3 needs for handedness and the Auto toggle)~~ **built 2026-08-23 — handedness is settable, so
I3's wheel has its prerequisite**, ~~**U3d-c's counted chip**~~ **built 2026-08-23, which closes
U3d**, ~~U3b's remainder~~ **built 2026-08-23 — fleet markers, VIEW-on-presence and A16's
transit edge; what is left is A16's pinned-camera edge behind U6, and A15's timed run**,
~~U4's client half~~ **built 2026-08-23**, ~~U5 **including N7**~~ **U5a built 2026-08-23; the
visual checkpoint was then run the same day and 🔥 R15 is what it found — the map drew the
2,500-system bake as seven horizontal strips under 250 overlapping names, through one bake
defect and three of the screen's own. All four are fixed, the universe is re-baked, and the
checkable half of the checkpoint is now a headless gate over the committed file
(`RunStrategicMapGate`). U5b (ADD WAYPOINT, search, per-label de-confliction and the GPU half
of the frame budget) and N7's site layer remain**, ~~U6~~ **built 2026-08-23 — U6a's
seam and device-free half, with all four owner rulings answered the same day at
[ADR-016 §9b](ADR/ADR-016-procedural-universe-and-warp.md), and **U6b the same day**: the draw,
the breadcrumb door, a warp issued from the surface and all four focus-polish items. 🏁 W2 is
met; what is left is the plate check, owed upstream, and the transit view's own run**, E5.

**T4 joins that queue ahead of E5** *(decision 9, 2026-08-23)*: **T4a** is registry truth,
device-free, and can land any time; **T4b** reshapes the hangar P1 draws and the roster 07a
draws, so it lands **before further screens are built against a grouping the ruling has
already struck** — the same clause that put every screen behind the input model, one
decision along. Its print deltas (P1 — a major bump, a §2 call reversed; 07a — sized at
capture) travel inside T4b rather than as separate deliverables, and its visual checkpoint
joins the R1 backlog like every other.

> **U5's checkpoint, 2026-08-23 — the one that was owed, and what it cost.**
>
> The map opened on the committed bake as **seven full-width horizontal strips** of overlapping
> dots under ~250 constellation names stacked on one another. Four defects: the bake laid five
> constellations on a three-column lattice inside a *square* region cell, so the gap between two
> regions was smaller than the pitch inside one; the client had no density ladder at REGION and
> emitted group labels outside the label budget; the pinch thresholds and the zoom ceiling were
> fractions of total extent derived from the print's 48-system era, which put SYSTEM out of
> reach entirely; and the top bar named the first region in the bake beside whole-universe
> counts. A fifth fell out of reproducing it — the mouse wheel had never zoomed this map,
> because the zoom was gated on the *gesture's* position and a gesture has one only while
> something is touching the screen.
>
> **All 1,281 tests were green while it drew strips**, including the pixel-level clustering
> assertion below — because the property that broke was never the Voronoi one. That is the
> finding worth keeping: a legibility invariant has to be about *what the eye groups by*. The
> bake now asserts that a region's content is isotropic and that regions stand further apart
> than their own constellations do, on both axes, over the baked positions; the self test
> asserts the same property **after the projection**, at the print's own 1440×900, over the
> committed file. See [Universe-Build-Order](Universe-Build-Order.md)'s U1 and U5 and
> [ADR-016 §2/§9](ADR/ADR-016-procedural-universe-and-warp.md).

> **U5a, 2026-08-23 — the strategic map's seam and device-free half.**
>
> The largest screen in the corpus, built as ADR-018 D14 says a screen should be: `MapView.h`
> is the neutral graph, `MapScreen.h/.cpp` is the zones, the camera, the cull and six hit
> tests, and `ReplicatedWorldView` answers five seam calls from the committed bake. Thirty-eight
> tests, plus a device-free run of the whole seam over a **real 2,500-system universe** — which
> is where the useful result is: at the fit, **no two of the 250 constellation discs overlap on
> screen**. That is U1's clustering invariant, asserted in pixels rather than in metres, and it
> is the mechanical half of a checkpoint that was supposed to need a person looking at a
> screen.
>
> **It is the first surface with a camera**, and the first and only consumer of
> `GestureState::pinchScale` — I1 built the pinch and nothing had a use for a zoom until now.
>
> **Three findings, and two of them are corrections to documents rather than to code.**
> `strategic-map.png` predates the touch reversal by two weeks and draws mouse-sized rows, so
> the 48 px floor is what corrects the *print* rather than something the print satisfies.
> D14's "colours arrive as data" had to become "badge classes arrive as data" — a baked colour
> ignores the colour-vision palette, on the one screen whose whole subject is a coloured
> overlay. And the gate-link budget turned out to have no headroom at all: the corpus's bake
> produces **exactly** the 3,000 links the client is built for, so the number is now declared
> once and the builder says when it drops one.
>
> **What it did not build, named rather than left to be discovered:** search (`TextEditState`
> exists and is wired to no surface), SET DESTINATION (U4's route feeder — the map plans and
> the client feeds, and sending the first hop as a bare warp would be a different promise from
> the one the button makes), fleet markers and VIEW-on-presence (U3b's client half), and label
> de-confliction (a look decision that belongs with the visual checkpoint).

> **U4's client half, 2026-08-23 — the route feeder.**
>
> A fleet crosses a route the player drew. `RoutePlan` hands the authority one leg at a time and
> advances on `OrderProgress::finished` — a flag that already existed so a ghost could retire,
> and *"the ghost may retire"* and *"the fleet has arrived"* are one fact read twice.
>
> **The warp pre-check worked for the first time**, and the fix was one function asked twice
> rather than two that agree: `reachableAnchors` and `jumpAnchor` were the two `ValidationView`
> fields nothing on the client filled, so a client running the right code over an empty view
> refused everything — indistinguishable from a client with no validation at all. That is a
> second half to BounceParity worth naming: **the same inputs, not only the same code**
> (ADR-014 §3).
>
> **Two of the four owed items are reported rather than built.** The halt cannot go into
> ADR-018 D19's event record — that record is server-side, a route lives in one client's memory
> by ADR-016 §8's own *"no server work"*, and the halt worth logging is **the one the client is
> not there for**. Amended at ADR-016 §9a.1. ~~The STATIC icon is blocked twice: the icon system
> is unbuilt *and* the client does not know an entity's hull class at all.~~ **Both halves were
> answered by 07b on 2026-08-23**: the icon system is built to its device-free edge, and
> `SceneEntity` now carries a hull's icon slot, its standing and its silhouette — so the client
> knows what class an entity is for the first time. What the STATIC icon still waits on is the
> *atlas*, which is §7's art plus a GPU.
>
> **Three drawn-text defects found in passing**, and a guard so the class cannot recur: a marker
> glyph had double-encoded into the source and had been drawing as three Latin-1 boxes since it
> was written. R31 records it; the CI step found one more that a regex would have missed.

> **U3b's client half, 2026-08-23 — fleet markers, VIEW, and the presence edge.**
>
> Markers are **counts at places** and the fold is the game's: a summary row is an anchor and
> the map draws systems. Two counts rather than one — ships that are *there* and ships
> *crossing to* it — because one total would put a fleet in a system it has not reached.
>
> **A16's second presence edge was a gap inside a function that already existed**, which is why
> nobody had seen it. `FollowTarget` answers `NO_FOLLOW_TARGET` for two situations that look
> identical from outside and are opposite to a player: ships where you stand, and nothing here
> and nothing to go to. Its own edge case came from a **mutation test rather than reasoning** —
> removing the here-guard passed every test written, because the case it protects is a player
> watching the grid a fleet is *arriving at*.
>
> Most of this slice was already built, which is the third time in a row.

> **A15, 2026-08-23 — the latency shim, and the budget as a function.**
>
> **The old acceptance was not conservative, it was conditional.** *"Under half a second"* is
> true at loopback RTT and an unstated assumption about the network everywhere else, because a
> switch is a request, an answer and the settle. `ViewSwitchBudgetSeconds(roundTripMs)`
> separates the target from the guess.
>
> `DelayedTransport` is a **decorator**, so the shipping transport never learns latency
> injection exists, and its hook is a call site rather than a configuration key — a key is a way
> for a shipped client to be slow because somebody left a number in a file. Only the timed run
> is still owed.

> **U6a, 2026-08-23 — the system view's seam and device-free half.**
>
> `SystemView.h`, `SystemScreen.h/.cpp`, one new seam call, and `ReplicatedWorldView` filling it
> from the bake. Eighteen tests and a gate over a real bake; twenty-two mutations, all caught.
>
> **All four of the owner rulings landed on the game's side of the seam**, which is the result
> worth keeping: the client is handed a ring index and a slot, so §9b.1's capacity, §9b.2's site
> ring and §9b.4's far-side name are computed where an anchor is known to be an anchor, and the
> engine never learns that the outer ring is mining fields. A screen that had computed them
> would have had to know what a site *is*.
>
> **Two findings.** A ring is fanned by everything on it, anchors and scenery together — the
> first draft counted only the anchors and put every moon exactly under a planet, which says
> that what a ring's capacity rations is *angular room* rather than pressable things. And
> **ADR-020's 48 px floor does not scale while the rings do**: at 0.5 a full ring's neighbours
> are 29.8 px apart against a 48 px target, so *nearest, never first* is load-bearing on this
> screen rather than defensive — and the test that proves it now asserts the crowding first, so
> it cannot quietly stop testing the tie-break.
>
> Device-free again, which is the fifth screen slice in a row.

> **U6b, 2026-08-23 — the screen, the door, and the run that broke the streak.**
>
> The sixth screen slice was *not* device-free, and it is the first one where that mattered. The
> suites were green -- 1,235 unit tests, a self-test gate over a real bake, thirty-eight of them
> U6a's own on this screen -- and the built screen drew `Vesta-3 Anchorage` through `Halgren`
> with four fifths of its disc empty. The rings ran evenly *from* `firstRingRadius` outward,
> which is defensible arithmetic and is wrong for the shape the bake actually produces: six
> anchors on an inner ring, two fields outside it. **Nothing in the arithmetic can say "these two
> words occupy the same pixels"**, so nothing did. That is R1's category earning its name, and it
> is worth recording against this document's own repeated finding that the device-free share of a
> screen slice is larger than the plan assumes: it is, and the remainder is not decoration.
>
> **The door was the cheap part and one seam call was the whole of it.** A breadcrumb is a
> hierarchy; U5 made it one target because there was one screen above it. The system word now
> opens the system, the chevron still opens the map, and `WorldView::WatchedSystem` exists
> because the client held the *word* while `BuildSystemView` takes an **id**.
>
> **Two decisions the slice had to take rather than look up.** The command row's WARP stays
> greyed and its comment was corrected instead: it promised to *"lift by deletion the day a
> destination picker exists"*, and two now do -- so what was wrong was the scope of the sentence
> rather than the gate, because a warp is a verb spent on a **place** and the places are on the
> two surfaces that draw them. And the verbs' *words* being the game's forced `AnchorVerbs` to
> answer for `INVALID_SYSTEM_ANCHOR` too: the panel draws both buttons with nothing selected, and
> a client that filled that frame itself would have learned what a warp is.
>
> **The focus polish closed D16 and gave ADR-020 D15.5 its first consumer.** Camera pinning is
> the pinned state D16's first presence edge had nothing to test against; the transit view turns
> D16's second edge into a screen that frames the crossing and then hands the player back on
> arrival; `JUMP TO` is the toast action payload finally spent on something -- and building it
> moved the chip's *press* above the surface branch, where its *draw* already was.
>
> **And one thing the owner found by playing it, which no gate could have.** A crossing's summary
> row is keyed on its destination, so the ELSEWHERE block read `VESTA-3 / IN WARP` -- a station
> apparently in warp -- at exactly the moment the wing's own roster row had gone.
> `FleetSummary` now carries a wing and crossings group on it: a wire and schema change, taken
> deliberately ([ADR-016 §6](ADR/ADR-016-procedural-universe-and-warp.md)). The warp itself was
> retuned ×10 in the same sitting and for the same kind of reason -- a hop across one system read
> **2:01**, which is two minutes of a fleet nobody can command.

**What moved and why:** every screen slice now sits behind the input model rather than beside it.
Building a screen against the mouse adaptation and re-fitting it for touch afterwards is the
retrofit the corpus refuses everywhere else — the same clause U6 and T3 both carry about drawing
the print before the screen.

---

## 6. Rulings, consolidated

Fifteen were tracked in [README.md](README.md) and eight in
[Economy-Build-Order.md](Economy-Build-Order.md). Two of them were one ruling asked repeatedly,
and **both were answered on 2026-08-22, closing six questions with two answers**:

- ~~**Fill order**~~ — D-P2 #1, D-P6 #10 and ADR-024 §5d were the same question. **Answered:
  content-declared order** (F-C → AST → NEB, each later family's own order after it), recorded at
  [ADR-024 §5d](ADR/ADR-024-mining-economy.md) with the general form at
  [ADR-027 §2](ADR/ADR-027-item-taxonomy.md). Not fairness but stability: value density needs a
  price table that does not exist, and a rule computed from prices would re-order itself the day
  the market phase tunes one.
- ~~**Device-local or account-side**~~ — 07h §3 (settings), D-P5 #5 (templates), site-layer #15
  (the scouting journal). **Answered: device-local, with the account service as the named reopen
  trigger**, recorded at [ADR-012 §3](ADR/ADR-012-configuration-and-json.md). It is the only
  answer the corpus can honour, since ADR-023 states it does not design the account service —
  and decision 2 had already shrunk the problem, because **wings are the control groups and live
  on the shard**, so the grouping case never reaches the user layer at all.

**A third was answered with them, and it shapes the input model rather than a ruling register:**
`OrderKindNamesDestination`'s conservative default. **Stance becomes selection-only** — a posture
is not a placement — while **Abilities keep a placement**, because §1's modality table draws
ABILITY + long-press empty space as *"PUCK — targeted ground"*. Recorded in
[ADR-020](ADR/ADR-020-ui-architecture.md)'s amendment; it is one line in `Orders.cpp`'s exception
list plus its parity row, and it is ruled **now** because I1–I3 are about to be built against it.

**Still open and still unowned: the push-notification contradiction** (`alerts-and-toasts.png`
§4). The platform allow-list has no notification row, and the refinery print's *"this tab never
owes a notification"* sidesteps rather than settles it. It is the last §3/§4 item in seventeen
plates with no home.

---

## What this document does not close

**The visual checkpoints.** T2, T3a and T3b each recorded one as owed, and the R1 category is
unchanged: a screen nobody has looked at is a screen nobody has tested. Decision 2 widens that
debt rather than narrowing it — the checkpoints now need a touch display, not just a display.

**The combat phase.** Wrecks, PVP flags, the 50% destruction burn and three of the wheel's eight
sectors (HOLD, ORBIT, KEEP RANGE) all name a phase with no ADR and no build order. It is not a
loose end so much as the next horizon, recorded here so the first person to need it finds it
named.

---

## Revision log

- **2026-08-23 — the fleet design review: the wing lifecycle reversed.** The owner stated
  the fleet model in six clauses — docked ships belong to no fleet; an undock composes one
  automatically; a docked ship can be sent out into a fleet already flying; a docking ship
  leaves its fleet; a fleet fully docked or fully destroyed is over, its slot freed — and
  the review found the tree answering nearly every clause the other way, because the corpus
  has **two** concepts where the owner's model has one: the emergent *fleet* (ships sharing
  a location — `FleetSummary`, the map's markers, the location blocks) and the persistent
  *wing* (a numbered label that attached at **dock**, survived docking, and was never
  visibly freed). Three defects were confirmed in code before the reversal was ruled: a
  lone errand minted a one-ship wing and spent a call sign; a parked wing and a dead one
  drew the same permanent zero row; and a number freed by loss could resurrect its old call
  sign on an unrelated dock group.

  **The ruling is [ADR-017 §6b](ADR/ADR-017-station-docking.md)** — membership is in-space
  membership — and it is decision 9 above. What it keeps is deliberate: the round trip
  still spends nothing (a group undocking exactly what it docked gets its number and call
  sign back — the old dock-groups note's motive, kept without keeping the wing alive while
  parked), names stay client-side presentation, the emergent-fleet layer and D-P5's
  doctrine layer are untouched, and the wire moves not one byte — `Undock` starts reading
  the wing field it has carried since T2. What it deletes is the 2026-08-22 dock-groups
  rule, the hangar's ASSIGN pair (the wing chip becomes UNDOCK's second parameter), the
  permanent zero row, and `EnsureWingName`'s refusal spiral (the cap counts rows drawn, not
  names minted). Delivery is **T4** — T4a registry, T4b client, milestone **H2** — specified
  in [Station-Build-Order.md](Station-Build-Order.md) and sequenced ahead of E5 above.

  **One drafted call is flagged for the owner rather than settled by them:** on an undock
  left to the registry, a selection that is exactly one number's parked memory goes *back*
  to that number — a pit-stop subset rejoins its wing rather than minting a fresh one. The
  owner's second clause read literally ("always a free slot") would mint; the draft chose
  rejoin as the round-trip motive generalised. It is one clause in §6b.2 to delete if play
  or the owner disagrees.

- **2026-08-23 — 07b and R30: the icon system's device-free half, and the tactical chrome onto
  the gesture seam.** Two items that had been waiting on each other without saying so — the
  icon sheet needed the client to know an entity's hull class, and the chrome needed a gesture
  the roster was already reading. Three things came out of them that belong in a plan rather
  than in a slice note.

  **The chrome conversion found a defect worse than the one it was scheduled to fix.** I2
  rewrote the roster to read `tapped` and `longPressed` and left it standing behind
  `if (!m_router.Pressed(InputButton::Left)) return;` — and a press edge is the frame a contact
  goes *down* while a tap is the frame it comes *up*. Never the same frame. So every roster rule
  I2 built was correct, was tested, and could not fire: a tap could not take a wing, a second tap
  could not frame one, a long-press could not add. **Two right answers wired in series**, and
  neither half was wrong on its own — which is why no test caught it and why the one that catches
  it now drives the recogniser over the frames `Window` actually produces rather than asserting
  about either half alone. The general form is worth carrying: **a slice that converts a consumer
  and leaves its gate is a slice that has built dead code**, and the corpus has one more of these
  waiting wherever a surface was written before I1.

  **The engine may own a vocabulary; it may not own the game's taxonomy.** The icon sheet is
  per-class and eleven of the eleven classes are the game's, so `IconSlot` is an *index* and the
  glyph table is data the composition root supplies — the mesh table's own shape, and ADR-018
  D14's *"the game says which class, the engine owns what a class looks like"* one surface
  further along than U5a needed it. What the engine does name is the four **families**, which is
  the line `LampRig` already drew: a visual kind shared across hulls is vocabulary, and eleven
  hull names would be `GameLogic` spelled inside `NeuronClient`.

  **And a print can be stale in a second way.** U5a found `strategic-map.png` predated the touch
  reversal; this one predates `HullClass::Gate`, which ADR-016 §10 appended a fortnight after §7
  closed the taxonomy at eleven. So the plate has no row for a class the tree ships, and the row
  is written here from §2's own description and **flagged inferred**, in ADR-027's manner. That
  is now twice in three screen slices that a decision moved under a plate and nothing re-read the
  plate. It is worth a standing habit rather than a third finding.

- **2026-08-23 — T3's remainder: the rename control, the disband, and the defect the second
  found.** The control was owed one thing and the layer under it had already landed, so what it
  actually needed was two input channels — `InputAction::Confirm` and `TextEditKey` — and the
  station surface moved onto the gesture seam to make room for a second gesture on a header.
  That closes half of R30, and the way it closed is the part to remember: **the chrome converts
  when something asks it to, one surface at a time**, rather than in a sweep nobody scheduled.
  *(The other half converted the same day with 07b — and the sweep, when it finally came, was
  four `ClaimPointerIn` sites and two gates. What it cost was not the conversion but finding
  what the gates had been hiding; see the entry at the head of this log.)*

  **Two findings, and neither was in the slice's list.** Giving the strays a roster row meant
  asking who owns a ship — and `BuildRoster` had never asked, so a hostile fleet flying *their*
  wing 1 was being counted into this player's wing 1 and would have been selected by a press on
  it. A wing number is a byte every commander numbers from one; the bits that tell them apart
  arrived after the code that needed them, so the function that could not ask read like one that
  had decided not to. And ADR-020 §3's "fail soft to U+FFFD" was unmet twice over: the draw
  *skipped* an unpaintable glyph rather than substituting, so a name made of them drew as
  nothing at all — and U+FFFD was not in the bake, so a substitution would have drawn nothing
  either. Silence is the one outcome a fail-soft rule cannot have.

- **2026-08-23 — U5a: the strategic map's seam and device-free half.** The largest screen in the
  corpus, and the first with a camera rather than a zone table. Three things came out of it that
  belong in a plan rather than in a slice note.

  **A print can be stale in the same way a document can.** `strategic-map.png` was authored
  2026-08-08 and ADR-020 reversed D15.4 on 2026-08-22, so the print draws mouse-sized rows on a
  surface whose primary input is now a finger. The floor corrected them silently — which is the
  right outcome and the wrong *process*: nothing in this corpus re-reads a print when a decision
  under it moves. Worth watching, because six more screen slices are queued behind prints of the
  same vintage.

  **The seam's colour rule needed one word changed.** ADR-018 D14 says labels, badge classes and
  colours arrive as data; the map took the third and baked a security colour, which is a colour
  that ignores the player's colour-vision palette. It crosses as a class now. The general form is
  worth keeping: *the game says which class, the engine owns what a class looks like* — because a
  screen that needed the game to pick a pixel value would be a screen whose accessibility
  settings the game has to know about.

  **And a budget with no headroom is a budget nobody measured.** `MAX_MAP_LINKS` was 3,000 and
  the corpus's bake produces exactly 3,000. It was chosen from ADR-016 §3's "~2.4 gates per
  system" and it is right — but it was right by arithmetic rather than by measurement, and the
  first thing that measured it was this slice. It is declared once now, beside the GPU budget it
  has to agree with, and the builder is audible when it drops one.

- **2026-08-23 — a documentation sweep across the corpus, and it found a risk that had never
  reached the register.** Every slice this session updated its own documents as it landed; this
  is the pass that checks what *other* documents said about them.

  **The finding worth the sweep:** ADR-020's touch reversal named "no touch device is in the
  loop anywhere" as a risk in §1 of this plan on 2026-08-22 — *"named here so it is a
  decision rather than a surprise"* — and it never reached the Risk Register, which is the one
  document whose whole job is holding risks. It is **R29** now, with **R30** beside it for the
  two surfaces I2 left answering only a mouse. A risk that lives only in a plan is a risk
  nobody is watching, which is the failure the register exists to prevent.

  Seven other documents disagreed with the tree. ADR-022 §5d still said `culledCount` renders
  through the icon ladder's *existing* rung — wrong twice, since the rung did not exist and
  could not have served anyway (it also cited §5 for a §6 panel). ADR-012 §A3 still said the
  display and audio families are written "the moment something changes them, and today nothing
  does". ADR-011's submix clause still called the category gains "what the settings screen
  writes", which N3 could not make true for want of a live-mixer setter. ADR-016's U3d-b
  amendment, the README's two phase blocks, the Dependency Map's `HudPalette` entry and R18's
  A15 annotation were each a slice behind. All corrected in place, struck rather than deleted,
  so the record of what was believed survives beside what is true.

- **2026-08-23 — U3 closed as far as it closes, and the last two items are blocked on different
  things.** U3d-c's counted chip is built, which finishes U3d: `culledCount` had reached
  `ReplicatedView::CulledCount()` at U3d-b and stopped there.

  **The rung it was to render through did not exist.** ADR-022 §5d and the build order both say
  it renders through *"the icon ladder's existing counted-chip rung"*, and the density ladder
  (`tactical-icon-system.png` §6) is unbuilt — so `CountedChip.h` is that rung, built here.
  **And it could not have been that rung anyway:** a density merge knows where its group is and
  draws an extent; a culled entity is one the server did not send, so the client holds a number
  and nothing else. Putting it on the plane would invent the *"position the client cannot
  justify"* the same print sentence forbids, so the chip is a screen-space statement about the
  feed. Zero says nothing, and §5a's guarantee means it is never the player's own fleet.

  **U3b's remainder was one line and should have been two.** ~~A16's presence edges route to the
  map on both rules, and `SurfaceId::Map` is an enumerator nothing pushes or draws — blocked on
  U5/U6's surface, not on effort. A15 is an *acceptance procedure*: its settle half is built and
  named (`VIEW_SETTLE_SECONDS`, 200 ms), its shim half does not exist (`Transport.h` has no
  latency hook), and its accept is a timed observation of a real client either way.~~ What could
  be finalised was the **target**, now stated as **RTT + 200 ms** in the form A15 asks for
  rather than W0's flat "under half a second".

  > **Both struck sentences were false within hours of being written (2026-08-23).** The map
  > became a screen at U5a, A16's second edge landed with U3b's client half, and A15's shim is
  > `NeuronCore/DelayedTransport.h/.cpp`. The *split* this paragraph argued for was right and
  > is kept: they are two problems, and they now have two different remainders — A16 waits on
  > U6's camera pinning for its first edge, A15 on a person with a stopwatch. What the
  > paragraph got wrong is what U5a, U4's client half and U3b's client half all got wrong in
  > turn: that a screen-adjacent item needs a GPU and a person before anything of it can be
  > built.

  So what is left of U3 is one R1 visual checkpoint and two items waiting on a screen and a
  stopwatch — which puts the whole phase in the same queue as I3 and N6's measuring half.

- **2026-08-23 — N3 built, and the contrast rule it enforces turned out to be unsatisfiable as
  written.** The settings screen has its layout, its five sections, the two the print marks
  REQUIRED, and a contrast audit beside them. **Handedness is settable, which was I3's hard
  prerequisite** — `settings.png` §3 calls the command wheel blocked on this screen in as many
  words. `MENU_SETTINGS` stops being dead.

  **The finding, which is the part worth keeping.** `settings.png` §1 states a floor of
  *"4.5:1 on every glyph pair"*, and the audit's first run failed all eighteen glyph-vs-glyph
  pairs across all three palettes while every glyph-vs-void pair passed comfortably. That
  pattern is not a palette defect, it is the rule being impossible: contrast ratio measures
  **luminance**, so clearing 4.5:1 against a void at 0.0037 forces every glyph past luminance
  0.19, and clearing it again between two such glyphs forces the brighter one past 1.03 —
  brighter than white. Two glyphs cannot satisfy both, let alone four. The print's own four
  rows do not reproduce against the shipped tables either, so its numbers came from a mock.
  The print settles it one panel higher — *"colour is never the only carrier"* — so the floor
  is a glyph-against-**ground** rule and glyph-vs-glyph is geometry's job. Under that reading
  all three palettes pass, worst 5.27:1. **Recorded as an ADR-020 §8 correction rather than
  fixed in the palettes**, because the "fix" would have been darkening semantics until they
  separated by luminance, which damages the contrast that actually matters.

  Two more decisions the slice had to take. **AUDIO is drawn and refused**: `AudioDevice`
  takes its gains at creation and has no setter for a running mixer, so a volume slider would
  take effect at next start — which the screen's own CHANGES APPLY IMMEDIATELY forbids. And
  **the 48 px floor is enforced by a function**, not by discipline: the first draft put BACK
  at 36.8 px at 0.8× scale, which a test caught.

  What is still owed on this screen: **D15.3's keybind capture**, which ADR-020 §8 called its
  first slice before the touch reversal demoted keybindings to accelerators; the live
  preview's contents, which need I3's hulls; the two RESET buttons, which need a decision
  about what reset means against a layer that records changes rather than state; and the AUDIO
  section, which needs one setter.

- **2026-08-23 — I2's `AssignWing` lift built, and the input phase is done to the hardware
  wall.** A wing can now be formed in space: `RosterView` carries a grid beside its station,
  `RequiresDock` splits off `NamesShips`, and the registry writes `World::SetWing` for a ship
  that is flying. No wire change, no new verb, no wing table — ADR-017 §6's "without new
  machinery" held.

  Three things the build had to decide rather than look up. **The refusal changed**:
  `NotDocked` reads "not docked here", which is the wrong sentence for a player who selected
  ships in space, so an `AssignWing` naming a ship the view does not carry is refused
  `UnknownShip`. It is keyed on the **verb** rather than on which list came up empty, because
  the verb is a byte both machines have while the view is built from the registry's owner index
  on one side and the relationship bits on the other — a reason derived from the view would
  have forked the halves in the very case the lift adds. **Writing a grid between ticks is
  safe** for a narrower reason than the old comment's: nothing in `Tick` reads a wing, so it is
  carried rather than simulated. And **an assignment away from a station mints no roster** for
  the place, which needed a non-creating lookup beside `RosterFor`.

  One adjacent gap, seen and left: `BuildGroupMembers` selects by wing without filtering on the
  relationship bits, so a hostile hull in wing 3 would be selected by the client. It is a
  selection wart rather than an authority hole — the server refuses the order `NotOwned` — and
  it belongs to whichever slice gives the roster its second commander.

- **2026-08-23 — I2's selection rules built; its `AssignWing` lift is not.** Rules 1, 2, 4 and 5
  are in: tap selects, drag pans, the roster row stops moving the camera, a second tap frames, a
  long-press adds, and box select lost its binding while keeping its pick. The lift is deliberately
  **not** in the same commit — it changes what the authority accepts and touches the view both
  halves validate against, and that is a different kind of care from the client work beside it.
  It stays I2's, and it is the slice's remainder rather than a new item.

- **2026-08-23 — I1 built; the input phase has started.** Contacts on `InputFrame`, `Gesture.h`
  over them, the mouse filling slot zero, and the router handing gestures out behind the pointer
  claim. It consumes nothing: I2 and I3 are the consumers, and landing the seam first is what
  stops them being an architecture change. **Two bugs were caught by running the state machine
  rather than by reading it**, and both are the kind that would have been miserable on a device:
  ending a gesture cleared the `tapped` flag the same frame had just set, so every tap silently
  did nothing; and the pinch scale was computed a frame late, so a zoom jumped on its second
  frame. Ending and cancelling are now two operations, and the pinch's numbers are computed on
  the frame it begins.

- **2026-08-23 — N6's sizing half built, and N6 turned out to be two slices.** A20 asks for a
  measurement *and* a sizing rule; only the measurement needs a GPU, and the sizing is the half
  U5 is actually blocked on. `NeuronClient/UploadBudget.h` derives the per-frame segment from the
  ceilings the renderer is built for times the strides its input layouts declare — 1,345 KiB
  against the 256 KiB constant it replaced — with `client.renderer.uploadBytesPerFrame` as the
  override and zero meaning "derive". The old constant was not a bad guess: it is within a few
  KiB of the derived *floor*, because it was sized for the tactical view and the map had not been
  drawn yet. **One number was got wrong on the way**, and it is the reason the slice has a test
  rather than only arithmetic: the per-hull mark ceiling was counted by reading the producers and
  missed one that lives in a different function, a thousand marks a frame. It is measured now,
  and exact rather than generous, which is only safe because the test is what keeps it so.

- **2026-08-22 — N4 built, and the three small slices are two.** D18's offset is a slot on a ring
  of `arrivalSpreadRadiusCm`, bearing from the crossing's own `TransferId` through
  `FixedAngle.h`'s integer table — integer angles because this decides a position in the replay
  domain, and the id was already in the transfer hash so nothing new entered it. Two existing
  tests moved rather than broke: the slot-assignment test now checks the formation's *shape* in
  the frame it arrived in, which is what it was always about, and the near-anchor test's
  tolerance became a sum that names the reserved radius instead of a number that hid it.
  **Undock contention is deliberately not covered** — ADR-017 §6a.1 answers it with a clearance
  predicate rather than an offset, and that gate is unbuilt, so it is now the station phase's
  open item rather than something D18 quietly covers. **What is left of the plan's small slices
  is N6**, which needs a GPU and a person.

- **2026-08-22 — N5 built.** `Simulation` gained `ViewerOpened`/`ViewerClosed` beside `MayView`,
  and `ServerHost` calls them at the three moments the answer changes: a session opening on its
  grid, an accepted `RequestView`, and the socket going. The composition root keeps the
  viewer-to-grid table and `WorldRegistry` keeps its count, which is ADR-022 §1's rule — the sim
  tier has no viewers — applied to a hold. **What the slice found was worse than the gap it was
  scheduled to close**: presence gating meant the missing hold never showed on a grid with ships
  on it, and on a grid *without* ships the world was torn down and rebuilt every tick — a whole
  `World`, its authored occupants and a site's field layout, for as long as somebody watched. The
  picture stayed stable throughout, since a rebuilt grid resolves its field from the calendar, so
  what the gap cost was the work and a rule ADR-016 §7 states that nothing held up. One decision
  it had to take
  rather than look up: the hold is released on **transport loss** rather than at the end of D5's
  grace window, because a disconnected commander still owns their fleet but has no camera, and
  worlds forget by design.

- **2026-08-22 — N2 built, and N5 moved ahead of N4.** The user layer is written:
  `WriteUserLayer` beside `ApplyUserLayer` in `AppConfig.cpp` — pure, and so covered by
  `OutpostTests` — with the file handle, the temporary and the rename in `ConfigLoad.cpp`, which
  is the split drawn exactly where that suite's reach ends. Wing names are its first family and
  `ReplicatedWorldView` gained the invariants that go with them: the player's word outranks the
  authored one rather than overwriting it, a rename costs a word and not a roster row (the cap
  arithmetic added the two name lists and double-counted the overlap), and a restored call sign is
  struck off the spare pool so it is never handed to a second wing. Three documentation defects
  fell out that were older than the slice: ADR-012 §3 and README both cited **ADR-017 §6a.4** for
  a rule that is §6's — §6a.4 only cites it — and README still described the reorganisation room
  as drawn disabled, three days after it was built. What N2 did **not** do is named in ADR-012
  §A3: the display and audio families are written the moment something changes them and nothing
  does yet, and the F1 diagnostics strip has no way to tell the composition root it was pressed.
  That write-back is N3's.

- **2026-08-22 — opened.** From a gap analysis run across the corpus before the next slice
  started. Five owner decisions recorded; eight slices that had no home given one; the input
  model established as its own phase after ADR-020 D15.4 was reversed. Four documentation
  defects fixed in the same commit.
- **2026-08-22 — the planning documents de-duplicated.** Renamed from
  `Plan-of-Record-2026-08-22.md`, because a dated name makes a standing document look like a
  snapshot. The three build orders' status headers stopped sequencing and now point here;
  `prompt-hud-economy.md` folded into E5. One document answers "what is next" instead of four.
- **2026-08-22 — four rulings taken, and the taxonomy debt paid.** Decision 5 delivered as
  [ADR-027](ADR/ADR-027-item-taxonomy.md); fill order and per-player state each answered once,
  closing six questions between them; `OrderKindNamesDestination`'s default replaced by a
  decision before I1 builds against it. Six of the fifteen tracked rulings closed. What remains
  unowned is the push-notification contradiction, and the fleet-template ADR is now the whole of
  the upstream-citation debt.
- **2026-08-22 — U3d-a and U3d-b built; R19 closed.** The ranking and `lastOrderSeqProcessed`'s
  departure from the world hash, then the whole wire cluster: `SnapshotAck`, `DeltaHeader`, the
  keyframe on a new reliable `Bulk` channel, u32 ids across the sim and the wire together, the
  viewer-relative relationship bits, the ring of views **as sent**, and priority truncation with
  an honest `culledCount`. The replication cliff is removed rather than mitigated, so R19 closes
  and ADR-018 D3's shared-grid gate lifts. Two things the slice had to decide rather than look
  up are recorded in the build order beside it — tier 1's reading, and the `ViewFocus` message
  ADR-022 §4 needs but does not name, which amends ADR-016 §7. Three defects fell out that were
  older than the slice: a size helper that had stopped agreeing with its writer, a pick radius
  that was an id sentinel, and a QUIC stream the peer had never heard of because nobody had
  written to it. **What is left of U3d is the counted chip**, which is screen work and has moved
  behind the input model with the other screens.
- **2026-08-22 — N1 given a home and a specification.** It is **U3d** in
  [Universe-Build-Order.md](Universe-Build-Order.md), split a/b/c along the sim → wire → client
  seam, with acceptance criteria per sub-slice. The numbering takes ADR-018 A14 at its word
  ("slice after U3c") rather than inventing a phase for it, and D6's entry now records why a
  delivered deliverable left its own work homeless: the ADR *was* the deliverable, so delivering
  it struck the row through while the slice it schedules had nowhere to live.
