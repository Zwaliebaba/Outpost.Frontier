# ADR-016 — Procedural Universe, Anchors, and Warp

**Status:** Accepted · 2026-08-19 (owner design session) · amended by
[ADR-017](ADR-017-station-docking.md) (2026-08-19): station anchors gain an undock point
and facing (§3); the transfer bus lands with the station phase, before U3a (§4);
`StationRoster` becomes the summary family's first resident (§6); presence includes docked
ships (§7) · further amended by [ADR-018](ADR-018-scaling-baseline.md) (2026-08-19): the
target is an MMO shard and a topology ADR blocks U2 (D1); §3 arrival contention answered by
deterministic per-order offsets (D18); §4 gains registry-owned ship-id allocation (D6a),
the `(applyTick, transferId)` total order (D17), the empty-world quiescence invariant and
hash-domain rule (D8), and the world-isolation invariant (D1a); §7's presence edges defined
and marked revocable (D16); per-commander event record joins the universe layer (D19) · **extended 2026-08-20 by §9a**
(owner rulings on `strategic-map.png` §4's decision list: §8 confirmed, the history scrubber
reserved as a rail, intel-ping provenance deferred with a trigger, landscape-only stated as an
envelope property)
· §4 further amended by [ADR-019](ADR-019-shard-topology.md) (2026-08-19): the registry
becomes host-aware (`HostForAnchor`) and **the world's tick becomes shard-global** rather
than a per-world counter; §5's transit durations gain a `TRANSFER_FLOOR_TICKS` floor ·
**further amended 2026-08-20 by [ADR-024](ADR-024-mining-economy.md)**: §3's reserved
`Site` kind is cashed in — sites bake with an authored orbit ring and an epoch-derived
bearing — and the deliberate-gaps list's "mined-out fields" and "wrecks" questions are
answered (a durable site ledger at the universe layer; bounded, non-durable wrecks)
**Depends on:** ADR-001 (plane, grids), ADR-002 (tick), ADR-004 (wire), ADR-005 (orders,
determinism), ADR-009 (universe model), ADR-012 (JSON), ADR-014 (seam), ADR-015 (contact)
**Supersedes:** the corpus scale figure F13 (~300 systems across ~6 regions → **2,500 across
~50**); ADR-009 §9's "no gates traversal in MVP" fence (the MVP is over); ADR-009 §6's
"closed at eleven" hull roster (**`Gate` is appended as the twelfth value**); ADR-001 §3's
"one play area per session" (a session now hosts many grids; the client views one at a time).
**Delivery plan:** [Universe-Build-Order.md](../Universe-Build-Order.md) (U1–U6, milestones
W0–W2).

## Context

Owner directive at the close of the MVP: the universe is **procedurally generated, 2,500
solar systems to start**; every system has a name, a sun, multiple planets, and a station
close to one or two of them; fleets **warp** between systems and, within a system, between
planets; mining and PVE locations join the warp destinations later; the UI is part of the
design, not an afterthought.

Most of the ground was prepared. ADR-009 built the universe model *for* this: the int64-metre
plane, `GridAnchor`, exact anchor↔local conversion, systems/celestials/stations/gates parsed
and content-hash-guarded, and the sentence this ADR now cashes in — *"warp/gate arrival later
= anchor swap + local re-origin — exact and deterministic by construction."* The corpus fixed
more than the code did: `strategic-map.png` designs the whole universe-map screen, names the
generation pipeline ("**the bake**"), states its layout requirement (clustered constellations
with clear gaps), fixes the naming scheme (proper names for regions and constellations,
`ROOT-N` for systems), and confronts the one execution question this ADR must answer (§8).
`session-surfaces.png` requires authored starter systems. What no document had decided:
how 2,500 systems get made, what a warp *is* mechanically, how one session simulates many
grids without giving up the replay contract, and where the player's eyes go when their fleets
are in three places at once.

Decisions taken in the session, recorded here with their reasons: **timed warp** (not
instant); **fleets split across systems from day one** (the server ticks every grid with
presence); **the client feeds routes one jump at a time** (the map print's own lean); **~50
regions of ~50 systems** (the region screen keeps working as printed); **no avatar ship**
(the commander stays disembodied); **anchors, never coordinates, as warp destinations**.

## Decision

### 1. Scale, organisation, names, security

2,500 systems in **~50 regions of ~50 systems**, each region **~4 constellations** laid out
as clusters with clear gaps — the strategic map's stated legibility requirement, so a whole
region stays one legible screen exactly as printed (F13's ~300/~6 is superseded; the map's
*design* is not). Constellations remain UI grouping with zero mechanics (ADR-009 §4).
Naming follows the corpus: regions and constellations take proper names from curated root
lists; systems are **`ROOT-N`** of their constellation (`VEI-4`, `TALIS-7`) — the print warns
that long proper names break its label strategy, so "every system has a name" is satisfied
the way the map can draw. Security is bake-authored content: a band per region, a value per
system (the route planner and the character-create screen both already display exactly this),
and **~3 designated starter systems** close the "starter systems are unauthored" gap.

### 2. The bake

Generation is **offline, and the artifact is the truth**. `GenerateUniverse(seed, config)` is
a pure GameLogic function — integer arithmetic and NeuronCore's PCG32 only, no floats in
universe placement (ADR-009's rule holds through generation) — invoked by a **bake mode of
the executable** (ADR-012 machinery: a config directory selects it; the JSON writer emits the
canonical file). The output is **committed** to `GameData/Universe/`, hash-guarded end to end
exactly as authored content is today, and remains hand-editable: the generator supports
**curated inserts**, and the start system stays hand-authored (Vesta-3 keeps its polish, the
generated galaxy grows around it).

Why baked and not seeded at runtime: the committed file is reviewable, diffable, curatable,
and covered by the existing fail-closed `universeHash` — while a runtime generator would make
"the universe" a property of generator version and floating toolchains, exactly the class of
cross-build promise ADR-005 §6 refuses to make. The corpus already used the word "bake";
this ADR agrees with it.

Bake-time validation is part of the bake, not a hope: gate graph connected, gate pairs
symmetric, ids and names unique, every station orbiting a planet, 1–2 stations per system,
security in range, cluster separation (the map's requirement made measurable), starter
systems valid, every anchor's warp-in point inside its grid bound. A `GameLogicTests` suite
holds all of it against the committed file. Scale costs are measured, not guessed: ~2,500
systems ≈ 3–5 MB of JSON parsed at boot by both halves; U1's acceptance records the number, *(and the estimate was low twice over: U1 measured **14.2 MB**, and E1b's site anchors took the committed file to **18.93 MB** — still inside the threshold, which is why the fallback is still reserved rather than spent)*,
and per-region files are the reserved fallback if it disappoints.

### 3. Anchors — warp destinations are authored, never coordinates

A **grid anchor** becomes a first-class authored record:
`{ kind : Station | Planet | Gate | Site *(reserved then; baked by E1b)*, owner id, grid origin : UniversePos,
warp-in point : local offset, warp-in facing }`. Ships warp **to anchors only** — never to
arbitrary space. That single rule bounds the number of grids a system can ever host, makes
"where can I go?" a finite, pickable list for the UI, and gives future content its extension
point: mining fields and PVE encounters are new `Site` anchor rows, zero new architecture.
*(Cashed in by [ADR-024](ADR-024-mining-economy.md), 2026-08-20 — with one property this
sentence did not anticipate: a site is the one anchor whose position the bake does not pin;
its bearing on an authored orbit ring is re-derived each daily epoch.)*

Placement rules: a station's grid **doubles as its planet's primary anchor** — "warp to
Kessler" and "warp to the Anchorage" land on the same grid, warp-in at a few kilometres'
standoff from the structure, so the busy place stays one place. A planet without a station
gets a bare anchor of its own (empty space for now — ADR-009 §9a's "celestials are data, not
geometry" stands, and the cost is re-stated below). A gate's warp-in point sits inside its
own jump radius, so route hops chain without a crawl. Arrival is **never random**: the
formation solve centres on the warp-in point with the authored facing, and ADR-015's
separation guarantees clean water even when two fleets arrive the same tick.

### 4. Many worlds, one universe runtime

A session hosts **many concurrent tactical grids** — one `World` per anchor with presence —
under a **universe runtime** that owns the registry: spin a world up when ships (or a viewer,
§7) arrive, spawn its authored occupants (station, gate), tear it down when the last ship
leaves and nobody is watching. `World` itself is untouched: per-grid determinism, orders,
formations, contact — everything ADR-005 and ADR-015 built carries over verbatim, and each
world seeds its PCG32 from (session seed, anchor id) so teardown and recreation reproduce
bit-identically.

Ships move between worlds on the **transfer bus**: a warp departure removes the ships from
their world and files a tick-stamped transit record at the universe layer; arrivals apply
**between ticks**, in a fixed order — (arrival tick, then order id) — which keeps the CI
guard "no `UniversePos` in per-tick code" true by construction. The replay contract extends
accordingly: a session replay is the per-grid order logs **plus the transfer log**, and the
double-run suite covers both. In-warp fleets have no positions and are not simulated — the
map animates them along their route line, presentation only, which is the "distances between
systems are map fiction" rule applied to time as well as space.

### 5. The warp order

Warp is an order like any other: **`OrderKind::Warp`**, appended after `Abilities` (the enum
grows by append, value 4 — a schema bump the fail-closed hash turns into an `UpdateRequired`,
never a silent skew). One group, the existing 64-ship cap, validated by **the same function
on both halves** — the client pre-checks, the server decides, bounce parity holds (ADR-005
§4). New refusal reasons arrive with it (`UnknownAnchor`, and `NotAtGate` for jumps).

Three phases, all in ticks, because the tick is the only clock:

- **Spool** — a per-class charge time (capitals slower; two new class-table columns:
  `warpSpeedMetresPerSec`, `spoolSeconds`). Cancellable: a replacing order during spool
  simply wins.
- **Transit** — committed. In-system: `base + universe distance / warp speed`, governed by
  the **slowest member** (the same philosophy as `GroupTravelSeconds`: the arrival that
  matters is the last one — a fleet warps together and arrives together). Gate jumps: a
  fixed short duration, because inter-system distance is map fiction (ADR-009 §3) and must
  not price travel.
- **Arrival** — formation solve at the warp-in point, authored facing, ADR-015 separation as
  the floor. The replicated `etaSeconds` that already exists on order records now also
  reports warp, so the HUD's number is the authority's own.

**Gate jumps** additionally require every member within a jump radius of the gate structure
on the gate's own grid — warp-in lands inside that radius, so the common case chains
seamlessly, and the gate keeps a physical presence that later gameplay (camping, interdiction)
can hang off. The timed model is not negotiable ornament: the route planner print prices an
11-jump route at 4m 10s, ~23 seconds a hop, and this ADR is what makes that arithmetic true.

### 6. The wire

*(Amended 2026-08-19 by [ADR-022](ADR-022-interest-and-delta.md): the per-grid stream becomes
a per-**viewer** stream. "ADR-004's budgets are unchanged and per-grid" below was true while a
grid's snapshot was one datagram for everyone watching it; under interest culling the viewed
grid's update is sized by a per-tick byte budget, packed into as many datagrams as it takes,
and each viewer gets a different subset. Summaries are untouched — they were already per-player
and already cheap. §7's presence rules gain one affordance: `culledCount` states how many
entities the player is **not** being sent, through the icon ladder's counted-chip rung.
**Built 2026-08-23 (U3d-c)** — and as a *screen-space* chip rather than a mark on the plane,
because a culled entity has no position the client can justify; see ADR-022 §5d's note.)*

*(Amended again 2026-08-22 by U3d-b, and this one contradicts §7 rather than extending it.
**§7 says "everything else about a view — where the camera is, what is selected, how far it is
zoomed — is client state the server has no business holding".** ADR-022 §1 requires the
opposite: relevance is a property of a viewer, so §4's ranking takes a focus, an extent and a
selection, and §5a's guarantee — that a commander's owned **and selected** ships are never
culled — cannot be kept by a server that does not know the selection. The client therefore
sends a `ViewFocus` message when its camera or selection changes, and the session holds the
latest. What §7's sentence still buys, and what the implementation keeps, is that none of it
reaches the **simulation**: the focus lives on the session's `SnapshotSender` and `World` gains
nothing, which is ADR-022 §1's "the sim tier has no viewers" stated as a data flow.)*

Snapshots become **per-grid**: the header carries the grid's identity, and a client is
subscribed to exactly one grid's stream — its **view** (§7). Three additions, all under the
existing schema hash: a **view request** (client → server: point my view at this grid of
mine), a **grid-switch notice** (server → client: your view now streams grid X, with its
anchor — the `Welcome` fields, generalised to mid-session), and **fleet summaries** — a
low-rate (~1 Hz) record per fleet the player owns: where (anchor or transit), how many ships,
what state, `etaSeconds`. Summaries are what the roster and the maps read for everything the
player is *not* watching. ADR-004's budgets are unchanged and per-grid: the viewed grid's
snapshot obeys the same datagram cap as today, and summaries are a few dozen bytes per fleet.
The snapshot's grid identity is also the smear guard: a late datagram from the previous view
names the wrong grid and is dropped, never blended.

### 7. The player, fleets, and focus

The player remains what the corpus made them: a **disembodied commander** (F3 — one
character; the reconnect print's "station commander held your standing orders"). **No avatar
or active ship is introduced**, and none is needed: presence in a system *is* the player's
ships there, and separately the player's **view** is a subscription to one grid.

A **fleet is emergent, not authored state**: your ships sharing a location (a grid, or one
transit record). Derived on both halves from replicated state, so there is nothing new to
desync; wings (TALON, ANVIL, …) stay what they are — grouping and naming *within* a location.
The roster becomes location-grouped blocks and is the primary focus switch; the full set:

1. **Roster click** — jump the view to that fleet's grid, camera centred on it.
2. **Fleet cycling** on a key (bindings live in the settings screen).
3. **The maps** — fleet markers and systems-with-presence carry a VIEW action.
4. **Toasts** — warp arrivals feed the alerts rail (`alerts-and-toasts.png`), and the toast
   is clickable.

Rules: the view may point at **any grid where the player has ships** — and nowhere else.

> **Built and enforced per viewer at U3c-a, 2026-08-21.** `MayView` had answered this rule for
> the composition root's scripted patrol list rather than for the asking commander, so it
> returned the same answer to everyone: the second commander to connect could have watched the
> first one's grid. Presence is a question about the ship→location index, so it moved into the
> registry as `WorldRegistry::HasPresence(owner, anchor)` — on the grid or docked at its
> station, which §7's own sentence and ADR-017 §7 already folded into one answer — and the
> `selfTest` asserts the refusal with two real clients over a socket.

> **The viewer half of that rule was built on 2026-08-22 (N5), two slices after it was
> scheduled.** `WorldRegistry::AddViewer` shipped with U2 and its header named U3b as what
> would start calling it for a player's view. U3b landed, and every caller in the tree was
> still the composition root holding its own start grid — so *"a viewer"* meant a grid chosen
> at boot rather than the grid anybody was looking at. **Presence gating hid it**: a grid you
> may watch is one your ships are standing on, and ships keep a grid alive on their own. What
> it did not hide is the case that has no ships — a station whose grid you have docked
> everything at, a site you are scouting after the field is chewed out. There **the sentence
> above was simply false**: `RankRelevance` borrowed the grid, which spun it up; the sweep at
> the end of the tick found it empty and unwatched and tore it down; and the next tick built it
> again. A whole `World`, its authored occupants and — on a site — a `BuildSiteField` layout,
> once per tick, for as long as somebody looked.
>
> **What it did not do is change what the player saw**, and that is worth stating so the fix is
> not credited with more than it did. A rebuilt grid resolves its field from the calendar —
> `ResolveField` reads the shard tick, not the instance that went away — so the picture was
> stable, and the defect was the work plus a stated rule that was not true. The one place the
> distinction was already written down is `LedgerIsCurrent`, which gives a shipless viewer-held
> grid the *calendar's* epoch rather than its own ([ADR-018](ADR-018-scaling-baseline.md) D8, so
> that what a viewer holds alive cannot change what the session hashes). That branch was written
> for holds which did not yet exist; it has callers now.
>
> The seam is `Simulation::ViewerOpened`/`ViewerClosed`, beside `MayView` and doing the other
> half of its job: `MayView` says whether a view is legal, these say it happened.
> **`ViewerOpened` reports the whole answer rather than a delta** — which grid this viewer is
> watching now — so a missed release is not expressible; the composition root keeps the
> viewer-to-grid map and the registry keeps a count, which is ADR-022 §1's split (the sim tier
> has no viewers) applied to a hold.
>
> **The hold goes with the socket, not with the commander.** A player inside D5's grace window
> still owns their fleet, their Bay and their refine jobs, all keyed on the player at the
> universe layer — but they have no camera, and a hold is about a camera. Worlds forget by
> design (ADR-018 D2), so a grid that empties while they are away is rebuilt from content on
> the tick they resume onto it.

Seeing without presence is the intel overlay's territory (`strategic-map.png`'s INTEL PINGS:
"the shape of your ignorance"), deliberately not given away for free now. Within a grid the
camera is exactly as free as today. **Auto-follow**: a watched fleet that warps takes the
view with it — the map is the between-surface, tactical resumes on arrival — unless the
camera is pinned. A grid stays alive while it has ships *or a viewer*, so a world is never
torn down under the player's camera. A view switch costs one interpolation-buffer refill
(~100 ms, ADR-002), so the transition is a designed ~200 ms settle rather than a pretended
instant with one janky frame. And **focus never gates command**: queued orders run
everywhere, selection and input apply to the viewed grid, and the client's route feeding
(§8) continues for fleets nobody is watching.

### 8. Route execution — the client feeds

The strategic map print poses this as its one real open question: the order queue holds four
legs and a route can be fourteen jumps. Decided: **the map plans, the client feeds** — the
route lives client-side (Dijkstra over the gate graph; fastest first, safest and
avoid-hostile after; the avoid-list persists in the user settings layer, ADR-012), and the
client submits **one warp/jump order per completed hop** into the existing queue. No schema
change, no server-side planner to keep in agreement with the client's. The named, accepted
cost, exactly as the print prices it: a disconnected player's fleet halts at the next gate —
the future AI-commander feature (the reconnect print already assumes one) is where that gap
closes, not here.

### 9. UI surfaces

- **Strategic map** — built to `strategic-map.png`, first slice deliberately a subset:
  region / constellation / system pinch levels, gate links, `ROOT-N` labels, the **security**
  overlay (its content exists from the bake) with the region band badge, search, the
  selected-system panel, fleet markers from summaries, route line with SET DESTINATION /
  ADD WAYPOINT, VIEW on systems with presence, and the TACTICAL ⇄ MAP handoff. Sovereignty,
  activity heat, intel pings and the history scrubber remain **stubs** — their content does
  not exist yet, which the print itself anticipates ("depends on content that does not
  exist").

  > **Built 2026-08-23 as U5a — the seam and the device-free half.**
  >
  > `MapView.h` is D14's neutral graph and the rows the panels print; `MapScreen.h/.cpp` is the
  > screen — zones, camera, cull, layout and six hit tests; `ReplicatedWorldView` answers all
  > five seam calls from the committed bake. The subset landed as this section scopes it, with
  > the stubs *visible* rather than hidden: four of the five overlays are drawn, disabled, and
  > carry the game's own word for why, and the history rail is drawn, inert and labelled per
  > §9a.2. What is deferred is named on the slice rather than discovered — search (no
  > text-entry surface), ~~SET DESTINATION (U4's feeder)~~, fleet markers and VIEW-on-presence
  > (U3b's client half).
  >
  > **SET DESTINATION landed 2026-08-23 with U4's client half**, and the deferral list was one
  > entry short. **ADD WAYPOINT** was not named, and it should have been: a waypoint's legs are
  > planned from the *previous waypoint* rather than from where the fleet is standing, so
  > serving it means the client hands the game a list of systems to string together — a change
  > to both `SolveMapRoute` and `BuildRoutePlan`, neither of which takes an origin, because the
  > origin of a fleet's route is a fact the game owns. That is U5's remaining route work rather
  > than U4's feeder, and the button is drawn permanently dead until it lands.
  >
  > **Search stopped being blocked on the day T3 landed**, which is worth writing down because
  > nothing re-reads a deferral when the thing it waited for arrives. "No text-entry surface"
  > was true when this note was written; the wing-rename control built one — `TextEditState`,
  > the `Confirm` action, the caret and the charset filter — so what search now needs is a
  > field on this screen rather than a mechanism anywhere.
  >
  > **One thing in this list turned out to be stated slightly wrong, and it is worth correcting
  > here rather than in a build order.** *"The security overlay (its content exists from the
  > bake)"* is true of the content, and the first draft took it to mean the *colour* could be
  > baked too — the game computing a gradient and handing over a packed value, on D14's
  > "colours arrive as data". A packed colour is a colour that ignores the player's
  > colour-vision palette, on the one screen whose whole subject is a coloured overlay. So what
  > crosses is D14's other word — a **badge class** — and the client resolves it through its own
  > palette. The gradient becomes three bands, the exact per-system number still reaches the
  > player in the badge beside it, and both halves of §4's *"two security values that must stay
  > distinguishable"* survive: the band is a badge and the number is per-system.
- **System view** — the screen the corpus names (pinch level: SYSTEM) **and drew on
  2026-08-21** as D1 in the build order. *(Its source is no longer in `ScreenPrints/`; D1 is
  tracked upstream, where the plate is still owed. That costs this section nothing, because the
  calls it produced were lifted here the day it was drawn — which is what "the calls end up in
  the ADRs" is for, and this list is now their home rather than their copy.)* Six calls came out
  of drawing it, and three of them are decisions this section had left implicit:

  **Anchors are targets and everything else is backdrop.** §3 already ruled that ships warp to
  anchors and never to coordinates; the print makes that visible rather than teaching it by
  refusal — an anchor is drawn with a reticle and a label, a star or a moon is drawn dim,
  unlabelled and inert. The player never clicks something and is told no.

  **Sites are a fourth anchor kind, and they are the one that moves.** This section predates
  mining; `AnchorKind::Site` arrived with E1b. The print draws fields beside planets, stations
  and gates, and does it **without a message**: `SiteEpochPlacement` is pure and
  client-callable, so the screen computes today's field from the bake and the epoch index, and
  a view whose epoch has rolled draws the new place rather than yesterday's rocks.

  **The layout cannot say distance, so it says time.** Rings are ordered by bake order and
  evenly spaced — "layout is legibility" applied inside a system, as this section asks — and
  the print takes the consequence seriously: **no scale bar, ever**, because a ruler over a
  non-linear layout is a lie with authority. What a trip costs is stated as an ETA from the
  same spool-and-transit arithmetic the tactical ghost prints, so the two surfaces cannot
  disagree about one warp.

  The other three are the ones this section already implied and the print pins: warp is the
  tactical grammar unchanged (one pre-check, one ghost, one bounce — no second validation);
  fleet markers are **counts at places** from the summary family at ~1 Hz, with an `InTransit`
  row sliding by presentation-only interpolation and never past an arrival the summary has not
  reported; and a warp issued here is a **single hop**, with multi-hop planning left to the
  strategic map so `UniverseRoute` keeps one caller.

  **Four questions are open for owner ruling before U6 builds**, the way P1's four were before
  T3: ring spacing past eight anchors, whether sites sit on their own outer ring or among the
  planets by real orbit, one marker or two at an anchor holding both docked and on-grid ships,
  and whether a gate shows the far side's name. They are tracked with the deliverable in
  [Universe-Build-Order.md](../Universe-Build-Order.md).
  Contents: the sun, orbit rings at **presentation scale** (real orbital distances are not
  linearly renderable — "layout is legibility" applies inside a system too), anchor icons
  (planets, stations, gates), fleet markers, in-warp fleets sliding along route lines. This
  is where "warp to that planet" is clicked, through the existing grammar: pending ghost,
  ETA label, bounce on refusal — the whole S9 pipeline, reused.
- **Tactical HUD** — roster location blocks with IN WARP / off-grid states and ETAs; a ~1 s
  departure/arrival treatment (no new render pass — the Ui/overlay vocabulary suffices);
  warp ghosts and refusal toasts through the existing surfaces.

### 9a. The rest of the print's OPEN list *(owner rulings, 2026-08-20)*

`strategic-map.png` §4 marks four items **NEEDS A DECISION**. §8 answered the first when this
ADR was written; the other three were still open, and U5 was about to lay out a screen against
them. They are answered here, beside §9, so the slice builds against decisions rather than
around gaps.

**9a.1 — Route execution: §8 stands, and one thing has changed under it.** The print's "one
real question" is already decided — the map plans, the client feeds, one order per completed
hop — and re-reading it changed nothing. What has changed is where its accepted cost gets
*reported*. §8 priced "a disconnected player's fleet halts at the next gate" and pointed at a
future AI commander to close the gap; since then ADR-018 D19's per-commander **event record**
landed, and the reconnect away-log is one of its four designed consumers. So the halt is now a
thing the game can *say* — "your fleet stopped at KIL-7 while you were away" — rather than
something the player discovers by looking. That does not close the gap, and it is not meant
to: it makes the gap honest, which is the difference between a designed limitation and a bug.
~~U4 emits the halt into the record when it builds the route feeder.~~ **The print's §3 note is
answered by §8; nothing in the tree needs to change to make it true.**

> **Amended 2026-08-23 (U4's client half): the struck sentence cannot be carried out, and the
> reason is §8's own ruling read one step further.**
>
> D19's record is per-commander at the **universe layer** — server-side, beside the transfer
> bus and the rosters. A route lives in one client's memory, because §8 chose the option with
> "no schema change, no server work". So emitting the halt into that record means either a
> client→server message — the server work §8 refused — or a second, client-side record.
>
> The second is worse than it looks, and this is the finding: **the halt worth logging is the
> one the client is not there for.** The cost §8 priced is *"a disconnected player's fleet
> halts at the next gate"*, and the client that would write that entry is the client that went
> away. Every other halt is one the player watches happen — the pre-check refuses, a toast
> carries the game's own reason, and the HUD's route chip stays red until they plan another
> route. An away-log line about an event the player witnessed is not an away-log line.
>
> What the tree already records is the **arrival**: `EventKind::Arrived` fires wherever the
> fleet stops, so the away-log can say "your fleet reached KIL-7" today. What it cannot carry
> is the *intent* — "of fourteen" — and intent is client-side by construction. Closing that
> needs something server-side that holds the route, which is precisely the AI commander §8
> already named as where this gap closes. So this clause is not dropped; it is **re-pointed at
> the same future feature the rest of §8's cost is pointed at**, and U4 reports it rather than
> half-building it.
>
> What U4 did build in its place is the client-side half a present player is owed, and it is
> more than the toast: the route chip on the context bar carries `ROUTE 2/5` through every
> surface, states `HALTED` in the alarm colour and **stays** stated until another route is
> planned — because a player who was in a hangar when the toast dwelled out would otherwise
> have a fleet parked at a gate and nothing on screen saying so.

**9a.2 — The history scrubber is a reserved rail, not a built feature and not a cut one.**
§9 already lists it among the stubs; what §4 asked, and §9 did not answer, is whether the rail
keeps its space. It does. The reasoning is that the irreversible thing here is the **layout**,
not the feature: the print makes the scrubber a permanent bottom rail and calls it the
screen's second axis, so adding one to a finished screen re-lays the screen out, while
removing a reserved one reclaims its space cleanly. Reserving it is therefore the cheap
direction and cutting it is the expensive one, which is the opposite of how a cut list reads.

R5's tier-4 estimate is also, read carefully, an estimate for **content rather than for the
rail**: the scrubber replays a strategic stream of sovereignty and activity history, and none
of that exists or is designed. Committing to build the scrubber now would be committing to
build that. So the rail is drawn, inert and labelled, and **build-or-cut is decided when the
strategic stream exists** — not before, and not by U5.

**9a.3 — Intel ping provenance is deferred, with a named trigger.** Who saw a ping, how stale
it is, and whether an ally can spoof one are questions about a scanning system, an intel
system and an alliance system, none of which exist. Deciding a trust model before there is
anything to trust is inventing a security model to go with it — the reasoning ADR-018 D5 used
to leave `resumeToken` reserved and zero, and the reasoning §6 used to defer the summary
family's paging. Nothing is blocked either way: U5 ships the overlay as a visible stub.

The trigger, so this is a deferral and not a silence: **the first information one commander
can see because another commander reported it.** That is the moment a ping acquires a source,
and the moment provenance stops being hypothetical. Until then the overlay shows nothing and
promises nothing.

**9a.4 — Landscape only. Portrait is not a per-screen property.** The print is right that the
map is the one screen that reads like a document rather than a viewport, and it is still no.
The tactical view is an ortho camera at 30° against ADR-006's 2:1 ring spec — a
landscape-shaped frame — and ADR-020 makes a surface a value on a stack the player pushes and
pops. One portrait-capable surface in that stack is a navigation model whose screens disagree
about aspect, and the disagreement lands exactly on the transition the player makes most.

So the ruling is about *where the decision lives* as much as what it is: aspect is a property
of the **display envelope**, not of a screen, and if a couch or handheld target ever arrives it
is answered once for every surface. It belongs with the DPI-to-scale rule and the stated
minimum client area that the scaling review (UI-4) already records as undocumented — the same
envelope U5 would otherwise be laying zone tables against blind.

### 10. The gate hull

Gates on their grids follow the one pattern the tree has for static presence: **a ship-table
entry that never moves** — stations proved it ("one path instead of two" is written where
`Structure` spawns). So **`HullClass::Gate` joins as value 11**, the twelfth entry. ADR-009
§6's closure exists so wire values, icons and palettes never *renumber* — appending renumbers
nothing, and that clause is amended, not violated. The class takes a contact radius in
ADR-015's table, a STATIC-family icon, zero speed, and eventually **`Gate.obj`** — a
radially symmetric ring/portal in the shared five-material palette. Until the art lands the
Structure mesh stands in: the Fighter/Cruiser precedent, run in reverse — a named content
gap, not a design gap.

**Amended 2026-08-20 (U4): the art landed, as `Stargate.obj`.** The stand-in was never used —
the mesh arrived with the slice — so the name is the only thing that moved, and it moved
because the artist's file is what the content list loads. A ring/portal as specified: 1,888
vertices, 1,144 triangles, 168 m of silhouette on the plane against the station's 253, which
is where the class's pick and contact radii now come from. The class's numbers were set from
the mesh rather than the other way round, which is the order that keeps art from having to
live with a guess.

**Amended again 2026-08-23 (U4's client half): the STATIC-family icon is blocked twice, and
the second block is the one worth recording.** The clause above lists "a STATIC-family icon"
among the things the class takes, as though it were a table row. It is not: the icon *system*
(`tactical-icon-system.png`) is unbuilt — U3d-c established this when the counted chip turned
out to have no ladder rung to render through — so there is no family for a gate to join. But
the client also **does not know an entity's hull class at all**. `SceneEntity` carries an id, a
plane position, a pick radius, two gauges and a status byte; nothing on the wire says what
shape a thing is, because until now nothing needed to. So the icon wants a replicated field as
well as a system, which makes it a slice rather than a line. Recorded on U6, and named here so
the next reader of this section does not cost themselves the same afternoon.

It cost one conformance edit, and it is the case the five-material rule exists to catch. The
export carried a *sixth* material, `aperture`, for the two faces of the portal disc — with the
accent colour to the last digit and a `d 0.2` this renderer does not read (ADR-006 §5 shades
albedo plus a light term). `ParseObjMesh` refuses an unknown material by design, so the mesh
would have failed at boot with a diagnostic rather than drawn wrong; the two faces were
authored onto `accent` instead, which is the same pixels. **The icon is still owed** — the
STATIC-family tactical icon and the map glyph are U4's client half, and the mesh landing does
not close them.

### What this deliberately does not do, so nobody mistakes it for covered

- **No grid persistence.** A torn-down world forgets everything but its authored occupants.
  The policy question (wrecks, mined-out fields) arrives with the content that needs it —
  `Site` anchors — and is named here so it is a decision then, not an accident. *(It is a
  decision now: [ADR-024](ADR-024-mining-economy.md) — mined-out state lives in a durable
  site ledger at the universe layer, and wrecks are bounded and non-durable.)*
- **No intel / fog of war.** Presence-gated viewing is the placeholder posture; the INTEL
  PINGS overlay owns the real design later.
- **No combat interaction with warp.** Interdiction, scramble, in-warp damage — all waiting
  on combat existing at all. The spool/transit split was shaped so a hold-down mechanic has
  somewhere to attach.
- **Celestials stay undrawn** (ADR-009 §9a stands). A bare planet anchor is visually empty
  space, and warping there will feel like it. Accepted, with the same eyes-open note as 9a:
  the authored content already contains the hard case when someone reopens rendering.
- **No AI commander.** The disconnect-halts-at-gate cost stands until that feature exists.

## Alternatives rejected

- **Runtime generation from a seed** — makes the universe a property of generator version and
  toolchain, weakens the hash guard to a promise, and produces content nobody can review or
  curate. The bake keeps every existing guarantee. Rejected.
- **One grid per solar system** — a system is hundreds of millions of kilometres wide;
  float32 at metre resolution dies six orders of magnitude earlier. This is precisely the
  precision layering ADR-009 §2 exists for. Rejected without regret.
- **Free-space warp (arbitrary coordinates)** — unbounded grid population, an unpickable
  "where to?" UI, and it forfeits the anchor list as the content extension point. Rejected.
- **Server-owned route execution** — survives disconnects, but widens the order schema and
  replication, and demands a server-side planner kept in perfect agreement with the client's
  presentation of it. The print leaned client-feeds; so does the architecture. Rejected,
  revisitable when the AI commander lands.
- **An avatar / active ship** — re-opens "what happens to *you* when that hull dies
  mid-warp", contradicts F3's commander identity, and buys nothing command needs. A
  *flagship designation* remains available later as gameplay, not attachment. Rejected.
- **Instant warp with cooldown** — cheaper, but it deletes the travel weight the route
  planner's own arithmetic promises, and it leaves nothing for interdiction to hold onto.
  Rejected.

## Consequences

- The delivery plan is **[Universe-Build-Order.md](../Universe-Build-Order.md)**: U1–U6 with
  milestones W0 (first warp), W1 (first crossing), W2 (the universe on screen). U5 depends
  only on U1 and runs in parallel.
- **Schema bumps, enumerated once** so they cluster per slice instead of dribbling:
  `OrderKind` +Warp; order-state values for spool/transit; per-grid snapshot header; view
  request / grid-switch / fleet-summary messages; `hull{11→12 classes}`. Every one rides the
  existing fail-closed hash.
- The **replay contract grows the transfer log**; the harness and `selfTest` extend with it
  (Risk R16). Two new class-table columns (warp speed, spool) join the envelope suite.
- The **content pipeline joins the tree**: bake mode, curated inserts, invariants suite, and
  a committed multi-megabyte content file whose parse time is measured at U1 (Risk R17).
- **Named content deliverables**: the system-view print (blocks U6), `Gate.obj` + icons
  (stand-in until landed), curated name-root lists (part of U1).
- ADR-001, ADR-009 and the corpus's F13 carry amendment notes pointing here; the README's
  supersession list grows one line. `Ids.h`'s scale comment is corrected with the code that
  lands U1.
