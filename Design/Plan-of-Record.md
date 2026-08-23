# Plan of Record

**Status:** standing document, **revised in place** · opened 2026-08-22 · current as of
2026-08-23 (U3d-a, U3d-b, N2, N5, N4 and N6's sizing half built). **This is the only document that says what is built next.** The three build orders
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
   storage, no keybindings, and no answer to the device-local-versus-account ruling. **It is
   docked-scope today** (`Station.cpp:237`), and the code comment already anticipates the lift:
   *"in-space reassignment can arrive later without new machinery."* That lift is in scope here.
4. **Long-press on a roster row adds to the selection.** The world's long-press is spent; the
   roster's is not, and T3b's press-versus-hold reasoning already establishes the idiom where it
   collides with nothing.
5. **Box-select is dropped**, with two-finger drag reserved for it. It has no home once drag is
   the camera, the print corpus never drew one, and wings make a marquee largely redundant.
   Reserved rather than refused, because "we never drew it" is not the same as "it is wrong".

**Keyboard bindings become an accelerator rather than the model.** They stay — the settings
screen's keybind capture (D15.3) is unaffected and a desk player will want them — but nothing in
the design may require a key, which is the rule D15.4's reversal has to carry or it will be
re-eroded.

### The slices this becomes

- **I1 — The input seam.** The pointer abstraction D15.4 refused: a device-free gesture layer
  over `Window`'s messages carrying tap, drag, long-press, second-finger and pinch, with the
  mouse expressed *through* it rather than beside it. `InputRouter`'s three channels are the
  seam it extends. Device-free and testable in `NeuronClientTests`, which is what makes it the
  first slice rather than part of a screen.
- **I2 — Selection.** The five rules above, plus the in-space `AssignWing` lift (a validator
  scope change and its parity row, not new machinery).
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
| **N3** | **The settings screen** | ADR-020 §8 names "the settings screen's first slice"; no build order contains it | medium |
| **I1–I3** | **The input model** | §1 above | large |
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

1. ~~**U3d — interest & delta**~~ — **U3d-a and U3d-b built 2026-08-22.** R19 is closed, the shared-grid gate (ADR-018 D3) is lifted, and A11's remainder landed with the `ShipId`/`EntityRecord::id` widening. What is left is **U3d-c's counted chip** — `culledCount` reaches `ReplicatedView::CulledCount()` and nothing draws it yet — and that is screen work, so it moves down to sit behind the input model with the rest of the screens. The client's ack, keyframe and delta-apply paths, which this plan listed under U3d-c, landed with U3d-b because the wire cannot be tested without a reader.

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

**Then the input model, which is its own phase:** I1 → I2 → I3. I1 and I2 are device-free and
can land before a touch device exists; I3 cannot be accepted without one.

**Then the screens, unchanged in content but re-based on the input model:** N3 (settings, which
I3 needs for handedness and the Auto toggle), **U3d-c's counted chip**, U3b's remainder, U4's
client half, U5 **including N7**, U6, E5.

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
