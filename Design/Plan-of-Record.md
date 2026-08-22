# Plan of Record

**Status:** standing document, **revised in place** · opened 2026-08-22 · current as of
2026-08-22. **This is the only document that says what is built next.** The three build orders
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

## The five decisions this document records

1. **The interest/delta slice (ADR-022) is next**, before any further screen work.
2. **Touch is the primary input; the mouse is a development convenience.** This reverses
   [ADR-020](ADR/ADR-020-ui-architecture.md) D15.4 and is the largest item here.
3. **The command wheel is built**, not superseded — it follows from decision 2.
4. **The user layer lands now**; the settings screen becomes a scheduled slice of its own.
5. **The item-taxonomy ADR is written now**, ahead of E5.

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
| **N1** | **Interest & delta (ADR-022)** | A14 scheduled it "after U3c"; U3c landed 2026-08-21 and no build order absorbed it | large |
| **N2** | **The user layer** | ADR-012 §3 calls `Settings.json` "the only file the game writes" and nothing writes it | small |
| **N3** | **The settings screen** | ADR-020 §8 names "the settings screen's first slice"; no build order contains it | medium |
| **I1–I3** | **The input model** | §1 above | large |
| **N4** | **D18, arrival contention** | Baked, parsed, hashed, never read — fell between U1 and U3a | small |
| **N5** | **The viewer hold** | `AddViewer`/`RemoveViewer` have no caller for a player's view; the "until U3b" deferral expired | small |
| **N6** | **A20 — spike 3 + the S5 frame check** | Stated as "Before U5", no slice, no owner | small |
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

**N4 — D18.** `Anchor::arrivalSpreadRadiusCm` is written by the bake
(`UniverseGen.cpp:638`), parsed, and folded into the universe hash — and read by nothing.
`WorldRegistry::ApplyTransit` places every arrival on the raw `warpInPoint`
(`WorldRegistry.cpp:1236`). U1's note said *"the rule itself is U3a's"*; U3a's note says
*"Still owed by U3a: nothing."* Two slices each believed the other had it. Today two fleets
warping to one anchor on one tick land on the same point and are pushed apart by ADR-015
separation, which is the stacking D18 exists to prevent.

**N5 — The viewer hold.** Small, and it grows teeth the moment N1 lands, because culling is a
property of a viewer and the registry currently has no viewer to be a property of.

---

## 3. Design deliverables owed

- **The item-taxonomy ADR** — *decision 5: written now.* Cited by three prints (D-P2's "ore is
  the first family of many", D-P4's admission rule, D-P6's six container kinds), and until it
  lands those plates cite a document this repository cannot follow. **Scope:** `ItemTypeId`, the
  fungible/instance split, the six container kinds, and the stack rules the cargo and market
  surfaces both assume. **Note for whoever writes it:** the numbering in the upstream source is
  not this corpus's, so the ADR has to restate rather than reference.
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

1. **N1 — interest & delta.** Headless, large, retires R19, lifts the shared-grid gate.
2. **N2 — the user layer.** Small, unblocks T3's reorganisation room and closes 🏁 H1.
3. **The item-taxonomy ADR.** Design, not code; can run beside 1 and 2.
4. **N4, N5, N6.** Three small slices, each closing a named gap; N6 before U5 as A20 requires.

**Then the input model, which is its own phase:** I1 → I2 → I3. I1 and I2 are device-free and
can land before a touch device exists; I3 cannot be accepted without one.

**Then the screens, unchanged in content but re-based on the input model:** N3 (settings, which
I3 needs for handedness and the Auto toggle), U3b's remainder, U4's client half, U5 **including
N7**, U6, E5.

**What moved and why:** every screen slice now sits behind the input model rather than beside it.
Building a screen against the mouse adaptation and re-fitting it for touch afterwards is the
retrofit the corpus refuses everywhere else — the same clause U6 and T3 both carry about drawing
the print before the screen.

---

## 6. Rulings, consolidated

Fifteen open rulings are tracked in [README.md](README.md) and seven more in
[Economy-Build-Order.md](Economy-Build-Order.md). Two of them are one ruling asked repeatedly,
and answering each once closes five:

- **Fill order** — D-P2 #1 (ore order on a fill), D-P6 #10 (partial-scoop order) and ADR-024 §5d
  are the same question. The ADR already says *"one ruling should close all three."*
- **Device-local or account-side** — 07h §3 (settings), D-P5 #5 (templates), site-layer #15 (the
  scouting journal). **Recommended: device-local, with the account service as the named reopen
  trigger**, because ADR-023 defers accounts and it is the only answer the corpus can currently
  honour. Decision 2 shrinks this further: under §1's selection model **wings are the control
  groups and live on the shard**, so the grouping case never reaches the user layer at all.

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

- **2026-08-22 — opened.** From a gap analysis run across the corpus before the next slice
  started. Five owner decisions recorded; eight slices that had no home given one; the input
  model established as its own phase after ADR-020 D15.4 was reversed. Four documentation
  defects fixed in the same commit.
- **2026-08-22 — the planning documents de-duplicated.** Renamed from
  `Plan-of-Record-2026-08-22.md`, because a dated name makes a standing document look like a
  snapshot. The three build orders' status headers stopped sequencing and now point here;
  `prompt-hud-economy.md` folded into E5. One document answers "what is next" instead of four.
