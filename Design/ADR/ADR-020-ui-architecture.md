# ADR-020 — UI Architecture: Surfaces, Input Consumption, and the Screen-Data Contract

**Status:** Accepted · 2026-08-19 (design deliverable ADR-018 A19)
**Depends on:** ADR-006 (fixed pass list, glyph atlas, Ui pass), ADR-012 (JSON and the user
layer), ADR-014 (the engine/game seam), ADR-016 (§7 focus and presence, §8 the client-fed
route, §9 the surfaces), ADR-017 (§6 the hangar), ADR-018 (**D14**, **D15**, A19)
**Amends:** ADR-006 **§9** — player-authored text takes the atlas charset, validated at the
widget and on user-layer load; the atlas re-bakes on an effective-scale change. ADR-006
**§10** — the display envelope: effective scale, `WM_DPICHANGED`, the 1280×720 floor,
per-surface overflow. ADR-014 **§2c** — the screen-data contract, and the fifth-project
question given a design-time tripwire in place of a reactive one.
**Delivery:** blocks **U5** (strategic map — Universe-Build-Order deliverable D7) and **T3**
(hangar screen — Station-Build-Order's stated gate). Closes review findings **UI-1…UI-6**.
This decides design only; no code is written here and none is assumed to exist.

## Context

R9 fenced the MVP HUD — "atlas ASCII + quads only, layouts hardcoded to print zones,
UI-scale multiplier the only flexibility" — and the fence held: the queue of things that
wanted to become widgets (the drag rectangle, the bounce toast, the dashed lane, the NET
readouts) emptied without one arriving, and the single primitive it cost was the oriented
quad, added as a class rather than a feature (ADR-006 §8c; Risk-Register R9). Nothing that
fence protects is reopened here. What changed is the level. R9 governed primitives *inside
one screen*; the roadmap is three to five full-screen surfaces — strategic map (U5), system
view (U6), hangar (T3), settings, six pre-session screens — with navigation, search,
renames, dropdowns, scrolling lists and per-screen state, and **no document says what a
screen mechanically is**. Today the frame is a fixed single-surface sequence
(`ClientApp.h:88–107`) and routing is hand-ordered zone re-tests with nowhere to say "this
was eaten" (`ClientApp.cpp:361–455`) — right for one screen and two consumers, and it does
not survive four.

The corpus has already made the load-bearing calls, which is why this ADR is short and its
code cost near zero. `debug-hud.png` §1 states the budget model — "every other screen in 04
replaces the one before it, which is what lets §1.5 say screens never stack" — and names the
diagnostics strip as the one entry that *composes*, with per-screen budget rows in §5. The
prints draw the navigation (`◀ TACTICAL |`, `◀ BACK |`) and `alerts-and-toasts.png` fixes
toast placement. What is missing is only the mechanism; ADR-018 D14/D15 already fixed the
policy, and this ADR implements them rather than re-arguing them.

## Decision

### 1. A surface is a value, and the client owns which one is live

**`SurfaceId` is a closed enumeration** — `Tactical`, `Map`, `System`, `Station`,
`Settings`, plus the pre-session `Login`, `UpdateRequired`, `CharacterCreate`, `Resume`,
`Queue`, `ReconnectUnderFire` (`session-surfaces.png` §1). `ClientApp` owns a fixed-capacity
stack (`MAX_SURFACE_DEPTH = 4`) whose base is the surface the session lands in. Pushing a
surface **already in the stack pops back to it** rather than pushing again: that is what
makes `◀ TACTICAL` from the hangar and `◀ BACK` from settings one mechanism, and what stops
a Tactical→Map→Tactical ladder growing under a player who navigates in circles.

**Screens are mutually exclusive; the debug strip is the only composing overlay.** The
corpus rule verbatim, and a budget rule before an aesthetic one — the per-screen frame
budget reconciles only because an active surface replaces rather than stacks, and the strip
is the one additive cost and the one surface permitted to occlude world (`debug-hud.png` §1,
§3, §5).

**Entry and exit are declared, and entry is where staleness dies.** `OnEnter` validates the
surface's retained state against the data it now holds (§5), requests anything asked-once it
lacks (§6), and claims no input. `OnExit` clears focus (§3) and cancels any in-flight drag,
and destroys no retained state — lifetime is §5's decision, not navigation's. Validating on
entry rather than exit is deliberate: a surface cannot know, as it leaves, which of the
things it names will still exist when it returns.

**Which passes run beneath a surface — an insertion-free use of ADR-006 §1's fixed list, not
a new pass.** The frame loop already records that list in two halves (`GpuPasses.h:195–215`):
`RecordWorld` is `Clear → Opaque → Nebula → OverlayWorld` into the MSAA target, the loop
resolves, then `RecordUi` draws on the back buffer (`ClientApp.cpp:1378`). A surface declares
whether it needs the world half; a full-screen surface does not, so the loop clears the back
buffer directly, skips the three world passes and the resolve, and records `Ui`. **No pass is
added, removed, reordered or branched** — the loop chooses between two calls it already
makes, and the resolve was never a pass ("barriers are not a pass", same header).

| Surface | World half | Ui half | Note |
|---|---|---|---|
| `Tactical` | records | HUD chrome | the world *is* the screen; the HUD is a border around it |
| `Map`, `System`, `Station`, `Settings` | skipped | the whole screen | Ui-over-Clear; skipping three passes and the resolve is where U5's 2,500-node budget comes from (A20) |
| pre-session surfaces | skipped | the whole screen | no `WorldView` data exists yet — which is why screens-as-HUD-modes was never available |

`settings.png`'s LIVE PREVIEW is not a counter-example and must not be built as one: it is Ui
content — the icon vocabulary as quads and glyphs inside a panel — not a suspended world
view. **No surface renders a partial world.**

**The network half of the frame runs on every surface; extract runs only when the world half
is recorded.** Snapshots keep arriving and keep the interpolation buffer full while the map
is up, so returning to tactical costs no refill: a *surface* switch is not a *view* switch,
and ADR-016 §7's ~200 ms settle is owed only by the latter. **Toasts are a cross-surface
layer**, drawn above the active surface on every in-session surface in the zones
`alerts-and-toasts.png` fixes — Critical centre-top, the rest bottom-right, neither over the
context bar. Pre-session surfaces carry no toast layer: there is no session to raise one, and
their equivalents (UPDATE REQUIRED, the queue position) are the screens themselves.

### 2. Input is claimed once, in one order, by one router

An `InputRouter` wraps the frame and carries claim flags; each stage in turn asks what
remains and claims what it takes. The consumed-event concept is a **claim over a latched
frame**, not an event queue, because `InputFrame` (`InputMap.h:79–111`) is what the camera,
selection and puck already read — converting it would touch every consumer to buy a property
claims already provide. Three channels claim independently — **pointer**, **wheel**,
**keyboard** — so a wheel notch over a scrolling list does not also cost the click still
pending in the same frame. The order for pointer and wheel: **focused widget → active
surface → cross-surface layers → world.**

Cross-surface layers sit after the active surface because the two **may not contend for a
pixel**: toast zones and the strip's clamp are already written so they never overlap a
surface's controls (`alerts-and-toasts.png` §2; `debug-hud.png` §3). One declared exception,
because the strip is draggable and can land over another surface's control — while being
dragged, and on its drag handle only, it claims ahead of the surface. Any layer needing a
wider exception is a defect in the zone tables, not a routing question.

**Keyboard order is the rule that has to be written down, because "W" must type or pan and
never both.** While `UiFocus` names an editable field, the field claims the text channel and
the editing keys, and **no action bound to a printable key survives to a later stage**;
actions on non-printable keys (F1's diagnostics toggle, `Escape`) still route past it. So
typing `W` into the map's search box does not pan the camera, and F1 still works while
typing — neither by accident. `Escape` goes to the focused field first (cancel the edit),
then the surface (back), then nothing. The decision lives in the router: `Window` keeps
translating virtual keys to logical actions and stays ignorant of listeners.

### 3. Focus, text input, and the charset

**At most one focus owner exists.** `UiFocus` names the surface and the widget, and is
cleared on surface exit, on a pointer claim outside the focused widget, and on window
deactivation. Two widget kinds may hold it: an editable field, and a binding-capture control
(§8), which claims the whole keyboard channel while armed rather than the text channel.
**`TextEditState` is the whole machinery**: a UTF-8 buffer, a caret byte offset, a selection
anchor, a codepoint cap — device-free and testable exactly like `CommandRow`.

**`Window` gains `WM_CHAR`,** appending to a bounded per-frame character buffer on
`InputFrame` that the router hands to the focus owner. Two details that are defects if
missed: `WM_CHAR` delivers UTF-16 code units, so a surrogate pair arrives as two messages and
must be assembled before it is a codepoint; and control characters (`\b`, `\r`, `\t`) arrive
here too and belong to the editing keys, not the buffer. **IME is explicitly deferred** — the
statement D15/UI-2 asked for. No `WM_IME_*` message is handled and the window makes no IME
arrangement of its own; anything reaching the buffer through a composition path meets the
same filter as everything else, so nothing unpaintable can enter. The reopen trigger is the
one already named: the localisation decision reopening ADR-006 §9 with per-locale bake lists
and a shaping call (ADR-018 D15.1).

**The charset is the atlas's baked set, and the atlas is the single source of it** (D15.1).
A codepoint is acceptable iff `GlyphAtlas::Find` answers for it at **every** baked size —
asking the atlas rather than keeping a second copy of `GlyphAtlas.cpp:27–47`'s list, because
two lists is how a wing name renders at one size and boxes at another. Validation runs in
**two** places: at the input widget, where an unacceptable codepoint is dropped as it is
typed, so nothing unpaintable is ever stored; and on **user-layer load**, where a name that
arrived some other way (a hand-edited `Settings.json`, an older build, a future account
service) fails soft to U+FFFD substitution rather than refusing the file — ADR-012's posture
toward every hand-edited artefact.

### 4. One scrolling list, and it scrolls by whole rows

`UiScrollState` is an offset in rows, a content count and a visible count, with a clamp. One
primitive serves the roster, the map's route panel (`strategic-map.png` draws eleven jumps;
ADR-016 §8 plans fourteen), the hangar's wing columns and the settings body. It closes
`HudRoster.h:54–62`, which deferred scrolling as "a surface" — it is not a surface, it is
this.

**Clipping is CPU-side and by whole rows**: a row outside the visible span emits nothing, and
a row straddling the edge is not emitted either. The reason is what the alternative costs —
either a clip rectangle on `UiInstance`, whose 48-byte stride ADR-006 §10a and a static
assert deliberately pin, or a scissor change mid-pass with a sort to group by it — bought for
a half-row the prints never draw: `tactical-hud.png` shows the roster's affordance as
`∨ 8/8`, a count and a chevron, and `strategic-map.png`'s route list is rows with a footer.
The primitive stays device-free and the pass stays one upload and one draw. **Wheel routing
is part of it**: a list under the cursor claims the wheel before the camera's zoom sees it,
through §2's chain and nothing bespoke.

### 5. Widget conventions, generalised from `CommandRow`

1. **Laid out and hit-tested in one place.** `BuildCommandRow` produces the rects,
   `HitCommandRow` tests them (`CommandRow.h:115–128`; ADR-006 §10b) — so the thing you press
   is the thing you see. Laying out in the build and hit-testing in the input handler is
   untestable by construction, because the two halves never meet.
2. **Build order is draw order is z.** The Ui pass has one pipeline and no sort (ADR-006
   §8c), so this is not a convention to maintain — it is what the pass does, and naming it is
   what stops someone adding a z field.
2a. **A slot's position is a property of the layout, never of the state in it** *(added
   2026-08-21, from a defect)*. `puck-and-wheel.png` §3 keeps the wheel's sectors in fixed
   positions "so the ring stays learnable as a shape rather than a lookup", and `CommandRow`'s
   own header repeats it for the row — but the parameter chip was placed *relative to the
   selected command*, held until "the next command with a parameter of its own". That
   reproduced the print only because Move was once the only early verb with a parameter; when
   Warp and Dock also came to vary by formation (ADR-018 §D7 makes Dock's radius a function of
   the solved one), the chip began landing after whichever of the three was armed and **every
   button to its right moved when the player clicked**.

   The rule that replaces it: the chip's slot is computed from the *kind list* alone — the door
   of the picker cluster — and is **always occupied**, so a command that varies by nothing gets
   a dash and nothing to press rather than an absent chip and a shifted row. What a slot
   *says* may follow the selection; where it sits may not. Two consequences worth stating: a
   test asserting "no parameter, no button" was deliberately reversed, and the dash is this
   HUD's existing word for an absent value (the roster draws one for an empty wing), so nothing
   new was invented to say it.

   It was found by looking at two screenshots side by side, which is the argument for the
   visual checkpoint in one line: every value involved was correct, and the row was still wrong.
3. **Device-free.** A widget is arithmetic over structs plus text runs; the atlas is reached
   only inside the pass (ADR-006 §10a). That is what lets the client suite cover a screen it
   cannot render, and it is not negotiable for anything built after this ADR.
4. **Per-surface retained state, with a lifetime declared from a closed set of three.**

| Lifetime | Cleared by | Examples |
|---|---|---|
| **Frame** | rebuilt every frame from replicated state | roster rows, order ghosts, the context bar (`ClientApp.h:214–220`) |
| **Session** | disconnect or session end; never by navigation | the hangar composer, scroll offsets, the map's pinch level and exclusive overlay, the tactical selection, the strip's dragged position (`debug-hud.png` §3) |
| **User layer** | the player, through settings | scale, palette, bindings, wing names, the route avoid-list (ADR-012 §3, widened by D15.5) |

**This answers `station-screen.png` §3's OPEN question as a rule rather than a proposal.** A
surface's selection state is **session-lifetime**: the composer survives navigation within a
session, is cleared by the action that consumes it (UNDOCK), and is cleared of anything its
data no longer contains — §1's entry validation, which is the half a per-print proposal could
not supply and which closes the stale-selection hazard the print worried about. The print's
proposal is adopted and generalised, so the map's selected system, the route panel's route
and the hangar's composer get one answer and nobody asks a third time.

5a. **Built so far (2026-08-19): the tactical HUD is the first surface to hold to §5 in
   full, and it did not need §1's surface stack to do it.** The command row rebuilt itself
   from the game's lists (`WorldView::OrderKinds`/`OrderOptions`), gained parameter chips with
   the print's `▾` caret, deferred them past the immediate verbs so **ATTACK stays second**,
   and grew the `▥ MENU` chip and its stub list. Four conventions above got their first real
   exercise, and one of them nearly broke:

   - **§5.1 held.** The MENU chip's rects are resolved in `UpdateHud` and drawn in `BuildHud`,
     one answer for the click and the quads — the same shape `BuildCommandRow`/`HitCommandRow`
     set.
   - **§5.2 held, and was the reason for a new pass.** "Build order is draw order is z" is
     exactly what *cannot* put the ghost's lane under the hulls, because the hulls are not in
     the Ui pass at all. The fix was a second `UiPass` instance recording into the world target
     before `Opaque` (ADR-006 §1c) — z by pass, not by field, which keeps the convention
     rather than bending it.
   - **§5.3 held.** Everything above is arithmetic over structs; all four tests that pin it
     (the row's order, the caret, the two gauge-banding cases) run with no device.
   - **§5.4 gained a real case:** `m_uiConsumedPress`, the frame's "this press landed on chrome
     above the world", which is **frame** lifetime and is what stops the selection box and the
     puck also acting on a click that opened the menu. It is the input-claim rule of §2 in one
     bool, on a surface that predates §2's router.

   **What this does not mean.** The surface stack (§1), the input router (§2), focus and text
   editing (§3) and the scrolling list (§4) are still unbuilt — the HUD is one surface, and a
   single live surface needs none of them. They arrive with the first screen that navigates,
   which is T3.

### 6. The screen-data contract across the ADR-014 seam

D14 decided screens are engine surfaces, data-fed. The shape of that data, fixed here so U5
and T3 do not each invent one:

| What crosses | Shape | Cadence | Precedent |
|---|---|---|---|
| Universe topology | a neutral graph: nodes (id, plane position, label, badge class, flags), edges (a, b, link class), and the class tables | **once, at boot** | `OrderKinds` — the asked-once pattern (ADR-014 §2c) |
| Live per-node data | summary-keyed neutral rows (counts, state, `etaSeconds`, badge values) | summary rate, ~1 Hz (ADR-016 §6) | `BuildRoster` |
| Docked roster | group rows plus ship-chip rows, keyed by station | on entry, and on roster change | `BuildRoster`, `StationRoster` (ADR-017 §8) |
| Search | a pure game-side function over the graph, reached through the seam | per keystroke | `ValidateOrder` parity |
| Route solve | a pure game-side function returning a path | on request | `SolveFormation` |
| Toast action | an opaque `(actionKind, actionTarget)` pair, carried and handed back unread | on press | `groupId` (ADR-014 §4) |

**A screen's seam budget is three shapes, not three methods**: one asked-once builder, one
summary-rate row builder, and pure query functions. A screen wanting something that is none
of the three is the tripwire below.

**The extended leak test, in words** (D14): *no security, sovereignty or station-service
semantics in engine code — labels, badge classes and colours arrive as data.* Concretely,
`NeuronClient` must never learn that a band is "security", that a colour means "low", that a
tab is "REFIT", or that a hull owner is a "pact". One sharpening the palette contract forces:
what crosses is a **badge class index and its label**, and the engine resolves that index to
a colour *and* a glyph through `HudPalette` and the icon tables. A literal colour crossing
the seam would be unauditable against `settings.png`'s 4.5:1 contrast floor and would survive
a deuteranopia palette swap unchanged — the one promise the engine cannot delegate. The class
index is the data; the palette is content (ADR-012). D14 is satisfied by the index, not the
literal.

**The fifth-project tripwire, made proactive.** ADR-014's revisit condition was reactive ("if
the seam starts leaking"), so it fires only after code exists. It is now a test applicable at
design time: **if a screen needs a game *rule* rather than game *data* to render, ADR-014's
deferred fifth-project question reopens** — not answered by widening the seam one more
method, and not by quietly re-adding the dependency. Game data is a label, a number, a class
index, an opaque id; a game rule is anything the engine would have to *evaluate* to decide
what to draw — which tab is legal, what a band means, how two badges combine, whether a route
is allowed. S11d is why this needs judgement rather than CI: the engine-references-game check
greps includes and project references, and a string literal is neither (ADR-014 §2c).

### 7. The display envelope (ADR-018 D15.2, amending ADR-006 §10)

**Effective scale = DPI-derived default × the user's preference**, the default being the
monitor's factor (`GetDpiForWindow / 96`) and the preference the settings sheet's 0.8–1.6×
slider. **The clamp applies to the preference, not the product** — clamping the product at
1.6 would cap a 200 % display below parity, which is exactly the defect UI-4 raised, so the
review's own wording is corrected here. A product guard (0.5–4.0) exists only so a degenerate
DPI report cannot produce a degenerate layout; it is a guard, not a design range.
**`WM_DPICHANGED` is a resize plus a rescale**: honour the suggested rectangle through
`SetWindowPos`, then take the existing resize path and re-resolve the zones —
`ResolveUiLayout` already runs per frame (`UiLayout.h:108`), so the rescale half is free.

**The glyph atlas re-bakes on an effective-scale change**, which is the part easily missed.
The atlas bakes 2–3 sizes in *pixels* at boot (ADR-006 §9); drawing those coverage bitmaps
into quads twice the size is bitmap scaling, the one thing `settings.png` §1 forbids in those
words ("re-lays out the interface rather than scaling a bitmap, so nothing blurs"). The
re-bake is the same boot operation on the same task pool, and every device resource already
owes a re-runnable Create/Destroy pair (ADR-018 D13) — so the mechanism exists, and the cost
is one rasterise and one upload on a change the player makes by hand.

**Minimum client area is 1280×720 at effective 1.0** — `MIN_CLIENT_WIDTH` /
`MIN_CLIENT_HEIGHT`, enforced at `WM_GETMINMAXINFO`. `ptMinTrackSize` is in *physical*
pixels, so the value is the design floor times the monitor's DPI factor, recomputed on
`WM_DPICHANGED`; `Window.cpp:312–318`'s 320×240 stays what it is — a swapchain guard against
a zero-sized client area, not a design floor. **The prints are normative at 1440×900**, which
is what "visual checkpoint against the print" means at U5 and T3. **Every zone declares one
overflow rule from a closed set of three** — the `CommandRow` drop-verbs precedent
(`CommandRow.h:104–113`) generalised rather than left as one control's habit. A surface with
no declared rule for a zone is not finished.

| Surface | Rule | Why |
|---|---|---|
| `Tactical` | **drop** | verbs drop rather than reflow; the world rect absorbs the rest, and reflow is a layout engine |
| `Map` | **scroll** the panels; the graph is a viewport | the graph already pans and zooms — that *is* its overflow answer |
| `Station` | **scroll** the wing columns; **letterbox** the parking diagram | the print calls the diagram a readout, not a control, so holding its aspect is the honest degradation |
| `Settings` | **scroll** the section body; **letterbox** the live preview | the preview is a proof about proportion and colour; distorting it breaks the proof |

### 8. The rest of the D15 package, folded in where it belongs

**Keybinding capture is the settings screen's first slice** (D15.3), settling `settings.png`
§3's "is there a keybind surface at all?" — ADR-016 §7 and T3 govern, and the answer is yes.
The capture control is `UiFocus`'s second client: while armed it claims the whole keyboard
channel, so the next press is captured rather than acted on. `InputMap.h:57–77`'s fourteen
hardcoded actions become a table `Window` reads, and bindings persist in the user layer
beside the other families (D15.5; ADR-012 §3).

**Touch and pad are off the roadmap** (D15.4). No pointer abstraction beyond the mouse is
built, reserved or hinted at — building one speculatively is what this clause refuses. **48 px
stays target-size discipline**, generalised from `CommandRow.h:76–83` to every interactive
widget: a floor in real pixels, enforced and not scaled, exactly as `settings.png` states it.

**Toasts gain an action payload** (D15.5): an opaque `(actionKind, actionTarget)` pair beside
`sourceKey`, plus an action label supplied by the game as `RosterRow::name` and `ReasonText`
already are. The engine carries the pair, draws the label, and hands it back unread when the
action is pressed — `alerts-and-toasts.png`'s `JUMP TO` works without `NeuronClient` learning
what a jump is. **The palette becomes re-resolvable state**: the settings screen re-runs the
resolve on change rather than only at boot (`ClientApp.cpp:82`), and "a packed colour literal
in `BuildHud` is a defect" (`HudPalette.h`) is what makes a swap total rather than partial.

**Surfaces are sized against ~200 owned ships and ~12 concurrent fleets per commander**
(D15.6). Two consequences fall out: `MAX_ROSTER_ROWS = 16` (`HudRoster.h:62`) does not hold
twelve fleets plus DOCKED blocks with headroom, so the roster is a scrolling list from its
next edit; and `station-screen.png`'s "sixty ships fit without a scrollbar" is a true
statement about 1440×900 and a typical station rather than a cap — 200 ships docked in one
place is 200 chips, so the hangar's wing columns scroll.

### What this deliberately does not do, so nobody mistakes it for covered

- **No retained-mode widget tree.** No parent/child graph, no invalidation, no layout solver.
  The fence moves up one level; it does not fall.
- **No external UI library.** AGENTS.md §5 forbids one without the owner's explicit approval,
  and no case is being presented here.
- **No data binding.** A widget reads the data it is handed, on the frame it is built.
- **No animation system.** Surfaces cut; they do not tween. The ~1 s dock and warp fades stay
  what ADR-016 §9 and ADR-017 §7 made them — overlay vocabulary, not a UI animator.
- **No i18n, shaping, or bidirectional text**, with the reopen trigger already named
  (ADR-018 D15.1 → ADR-006 §9).
- **No modal dialog system.** The corpus has no dialogs: UPDATE REQUIRED is a *screen*, and
  `session-surfaces.png` §2 argues that at length.
- **No clip rectangle on the Ui instance and no virtualised list** beyond §4's whole-row cull.
- **No gesture rebinding** (`settings.png` §2 kept it out; this does not put it back), **no
  portrait layout** (`strategic-map.png` §4 leaves it open), **no screen-reader path**.

## Alternatives rejected

- **Screens as HUD modes** — a flag on the tactical HUD that hides the world and draws
  something else. Cheapest by far, and it fails twice: the pre-session surfaces must exist
  before a `WorldView` has any data (`session-surfaces.png` §1), and the budget model requires
  screens to replace rather than stack (`debug-hud.png` §1.5). Rejected.
- **A render pass per surface** (`MapPass`, `StationPass`) — reads natural against ADR-006's
  reserved slots, but those slots are *world* nodes and every surface draws the same quads
  through the same PSO in the same space; it would multiply pipelines for content differing
  only in build order. Rejected — the surface chooses which half of the existing list runs.
- **Keeping per-consumer zone re-tests** — today's arrangement, extended. Each new consumer
  multiplies the pairs that must agree about precedence, and there is nowhere to express
  "this key was eaten" (UI-2). Rejected.
- **A real event queue instead of claim flags** — better if a surface ever needs ordering
  *within* a frame; today it buys nothing claims do not, at the cost of touching the camera,
  the selection and the puck, which all read the latched frame. Rejected now, named as the
  upgrade if intra-frame ordering ever matters.
- **Immediate mode with implicit ids** (the hand-rolled imgui idiom) — no retained state at
  all, which is the attraction. It hit-tests during the build, so the pointer is tested
  against the *previous* frame's layout, the opposite of ADR-006 §10b's call; and anonymous
  per-widget state is exactly what the composer question needed to name. Rejected.
- **Scissor-clipped pixel-offset scrolling** — smoother, and it costs either a clip rect on
  the 48-byte `UiInstance` or a scissor change mid-pass with a sort, for a half-row the
  prints never draw. Rejected (§4).
- **Clamping effective scale to 0.8–1.6** — the review's own wording, and wrong: it caps a
  200 % display below parity, the very defect UI-4 raised. Rejected in favour of clamping the
  preference and multiplying by the DPI factor (§7).
- **Baking a wider charset now** (Latin-1, or the BMP) — atlas area and boot cost for text
  nobody can enter until accounts exist, and it would decide the localisation question by
  accident. Rejected; the bake list is data and grows as content when the trigger fires.
- **Opening the fifth project now** — ADR-014's deferred "GameClient" is the honest home for
  Frontier-shaped screens, and taking it would spend a fixed structure decision before one
  screen has demonstrated the seam leaking; D14 already ruled the other way. Rejected, with
  §6's tripwire as the condition that reopens it rather than a feeling that it might be time.

## Consequences

- **U5 and T3 are unblocked.** Both build orders carry an explicit gate naming this document
  (Universe-Build-Order U5 / deliverable D7; Station-Build-Order T3); the gate is satisfied.
- **Amendment notes land with this ADR**: ADR-006 §§9–10 and ADR-014 §2c point here for the
  mechanism they were given, the README's decisions table grows a row, and U5's and T3's gate
  lines name this document. Where a note and this ADR ever disagree, this one is the decision
  and the note is the pointer.
- **The seam grows, and each addition is paid for three times**: a `NullWorldView` stub, a
  test double in each engine test project, and an adapter in `Outpost.exe` (ADR-014 §2a) —
  the argument for §6's three-shapes budget over one method per question.
- **`Window`'s message set grows for the first time since the input frame was fixed**:
  `WM_CHAR` and `WM_DPICHANGED`, plus a binding table behind `InputMap`'s logical actions.
  `Window` stays ignorant of who consumes what.
- **Device-free testability survives intact.** Every decision here is arithmetic over structs
  — surfaces, claims, edit state, scroll state, zone tables — so the client suite covers
  screens it cannot render. The single device-touching item is §7's atlas re-bake.
- **Full-screen surfaces are cheaper than the tactical HUD**, not dearer: three world passes
  and the MSAA resolve are skipped. That headroom is what U5's 2,500-node budget and
  `debug-hud.png` §5's `STRATEGIC MAP ACTIVE + TIER 1` row both assume.
- **R9 stays a fence, one level up.** Its new test has the old shape: anything wanting a
  parent pointer, an invalidation flag or a z field is the framework arriving, and that is a
  flag rather than a feature. The register row should say so.
- **Two print questions are answered** and belong to whoever reviews those prints:
  `station-screen.png` §3's composer persistence (§5 — session lifetime, cleared on undock,
  validated on entry) and `settings.png` §3's keybind surface (§8 — yes, in the settings
  screen's first slice).
