# Scaling-Readiness Review — five lenses over the MVP and the design corpus

**Status:** Review output 2026-08-19 · **advisory, not normative.** This document decides
nothing: the ADRs remain the authority, and every recommendation below is a proposal for the
owner to accept, amend, or decline. Commissioned by the owner as a five-expert review of the
code-complete MVP (S1–S14 plus ADR-015) and the full design corpus, asking one question: **is
the baseline structurally right, so that scaling does not later force structural changes whose
cost dwarfs fixing the baseline now?** Feature proposals were out of scope; so were code
changes — nothing in the tree was modified.

**Method.** Five independent reviews — user experience, MMO & network, C++ & DirectX, UI
architecture, game logic — each read the corpus against the tree and verified claims in code
before writing; their full reports, with file:line evidence, are Appendices A–E. The
load-bearing numeric claims (the dock geometry, the snapshot budget chain, per-world id
allocation, the world-hash fold of `lastOrderSeqProcessed`, the Debug/Release shader-model
fork) were re-verified independently during consolidation. Findings carry their lens prefix
(`UX- / NET- / CPP- / UI- / SIM-`) and are cited by that id throughout.

## Verdict

**The baseline holds. Nothing needs ripping out.** Zero findings are Critical; no reviewer
found a foundation that scaling would force out. The decisions the corpus is proudest of are
the ones the panel independently converged on keeping: the transport-only crossing that makes
the process split a packaging change, the fail-closed schema hash, quantised-input validation
parity, fixed-schema SoA with widen-by-append, the multi-world decomposition with a
between-ticks transfer bus, the Extract seam, the fixed pass list, and the presence-gated
one-view attention model. These are the right shapes at every scale the corpus gestures at.

What the panel found instead is **a cluster of missing decisions sitting exactly where the
next two phases are about to pour concrete** — 10 High findings, every one a one-page ADR
amendment today and a log-format, wire, replay-golden, or shipped-surface migration later:

- **The universe runtime's bookkeeping is under-designed** (SIM-1/2/3, CPP-1): ship-id
  allocation collides across worlds, the transfer bus's ordering key is written two different
  ways in ADR-016 and ADR-017 and neither reading is total, viewer behaviour leaks into the
  replay/hash domain through the teardown rule, and the world-isolation contract that keeps
  deterministic parallelism reachable is nowhere stated. U2/T1 build all of this next.
- **The multi-client baseline is a label, not a design** (NET-1/2, SIM-4/7, UX-2): there is
  no durable player identity anywhere — the seam hands the game a `clientId` and the game
  discards it — while U3b/T1 are about to key presence, rosters, and summaries on "the
  player"; and the replication growth path ("delta + interest, later") has no snapshot-ack,
  no keyframe path, no relevance hook, and a hard 43-record cliff whose designed failure is
  *no snapshot at all*, with a margin of one record over the MVP's own content.
- **The screens are designed but the screen machinery is undecided** (UI-1/2/5): four
  full-screen surfaces are printed and two are buildable today, with no surface/navigation
  model, no input-consumption rule, no text input path (no `WM_CHAR`, no focus concept —
  while U5 ships a search box and T3 ships renames), and no decided shape for screen-scale
  data crossing the ADR-014 seam.
- **One spec is arithmetically wrong** (UX-1): the dock rule — every member inside 5,000 m,
  together, instantly — cannot be satisfied by the game's own 41-ship starting fleet, whose
  Battleship-paced Line spans 19.2 km (spacing 480 m, `ShipClass.cpp:50`); and ADR-017 §5's
  "15 s covers a ~3 km parking flight" is actually ~1.2–1.6 km against the class table. T1
  pins this into shared validation and U1 bakes the related warp-in invariant into 2,500
  committed systems.

Beside those, two quieter structural threads: **the instruments the next decisions depend on
have never been run** (the 1,024-entity soak, the 1,024-instance draw, the Release build and
its replay-hash comparison, the S5 frame-time check — CPP-3/5, NET-5, SIM-6), so the universe
phase would otherwise take structural decisions like R17's content split on Debug-only
numbers; and **the compatibility story gates shape, not behaviour** (SIM-5): the schema hash
does not cover validation bounds or the check order, so the first post-remote-client balance
patch can produce systematic pre-check/authority disagreement that nothing detects.

And one question gates more of this review than any other, because the corpus never answers
it: **what scale is this game actually for?** (below).

## The gating question: target scale

The only concurrency number in the tree is `maxSessions = 8` (`ServerConfig.h:19`). No
document states how many commanders share a session or a grid, whether the 2,500-system
universe is one persistent shard or an instance per session, how long a server lives (the
corpus's RESUME card and away-log read like a persistent service), or a per-commander
envelope (ships owned, concurrent fleets). Four of the five reviewers independently hit this
wall (NET-3, and the questions of UX, CPP, SIM), because severities flip on the answer:

- **Co-op session (≤8 commanders, instanced universe):** the single-process topology is
  adequate essentially forever; NET-1 stays High but its build trigger relaxes; NET-3's
  sharding concerns dissolve; SIM-1's u16 policy is comfortable.
- **MMO shard (dozens-to-hundreds, one persistent universe):** the topology needs its ADR
  *before U2* (grid-to-host assignment, transfer-bus ordering authority, connection handoff);
  `ShipId = u16` becomes a shard-lifetime ceiling to widen before ids ship in rosters and
  logs; NET-1 is effectively Critical; persistence (SIM-2's "worlds forget" rule, NET-2's
  identity) stops being deferrable.

The panel's strong recommendation is not a particular answer — it is that the answer be
**written down** (a one-page ADR or a Risk-Register preamble) before U2 shapes the universe
runtime. The design's own data discipline (serialised transfer records, per-grid logs,
per-anchor seeding, anchors bounding grid count) is unusually shard-friendly, so the MMO
reading is *reachable* — but only if U2/T1 keep it so knowingly.

## Sound for scale — the panel's consensus keep-list

Decisions the reviews independently verified as right for every scaling axis. They are listed
so that no finding below is read as an invitation to churn them.

1. **Transport-only crossing + single-writer worlds** — the process split stays a packaging
   change by construction; verified no other channel exists (ADR-007; CPP, NET).
2. **QUIC-only behind a QUIC-shaped seam**, proven against two implementations, encryption
   live since S13, loss/jitter posture (idempotent snapshots, interp→extrap→STALE, slewed
   `t_est`) already real-network-grade (ADR-002/003; NET).
3. **The fail-closed dual schema/content hash covering quantisation constants** — every
   schema bump is a clustered refuse-at-the-door event (ADR-004; NET, SIM).
4. **Integer-quantised wire + one shared validation function on quantised inputs** — float
   parity is dead before a second machine exists; BounceParity is bought by construction
   (ADR-004/005; NET, SIM, UX).
5. **Same-binary determinism scope, enforced by CI bans rather than review** — the cheap
   determinism, with the expensive kind (cross-build) correctly refused (ADR-005 §6; CPP, SIM).
6. **Fixed-schema SoA with widen-by-append evolution, every field priced in ships** — gauges
   and statusBits already prove the mechanism (ADR-005, ADR-017 §5; SIM).
7. **The multi-world decomposition** — `World` untouched, transfers between ticks, per-anchor
   PCG32: worlds are tick-independent by construction, so deterministic parallelism and even
   sharding stay reachable (ADR-016 §4; SIM, NET, CPP).
8. **Anchors-only warp over a baked, hash-guarded universe, client-fed routes with the
   `systemIssued` seam** — bounds grid count, keeps the order schema narrow, leaves the AI
   commander a door (ADR-016 §§2–3, 8; SIM, NET).
9. **Extract as the future Game/Render thread seam; the fixed pass list with measured
   insertion cost; bounded drop-and-count crossings everywhere** (ADR-006/007; CPP).
10. **Emergent fleets and wings; the presence-gated single view + ~1 Hz summaries + the map
    as between-surface** — command-by-exception is the right attention architecture for many
    fleets, and emergent grouping deletes a class of shared-state desync before it exists
    (ADR-016 §7, ADR-017; UX, SIM).
11. **The order grammar is RTT-proof** — optimism about the order, never the ships; one
    reason home on two surfaces; honest ETAs (ADR-005 §4; UX).
12. **The UI's device-free text discipline, layout-and-hit-test-in-one-place, and seam-fed
    vocabulary** — the right substrate for every future screen (ADR-006 §§7–10, ADR-014 §2c;
    UI).

## The decision list, by deadline

The panel's 30 findings reduce to the following decisions, sequenced against the build
orders. Each entry names its findings; the appendices carry the full argument and evidence.

### Before U1 — the bake commits content

- **Fix the dock rule's arithmetic** (UX-1, High). Choose the fleet-scale dock semantic —
  waves (mirroring the undock clause already on the P1 print), a footprint-derived radius, or
  consume-on-entry with "together" restated per-order — and re-run ADR-017 §5's protection
  arithmetic against the class table. It must precede T1 (which pins the rule into shared
  validation) *and* U1 (which bakes warp-in-inside-dock-radius across 2,500 systems).
- **Decide the anchor-contention vocabulary** (UX-6, Low). One authored warp-in/undock point
  per anchor serialises hub traffic; if arrival offsets or point-rings are ever wanted, the
  anchor record should carry the fields from the first committed bake.
- **Name the measurement config and add the Release leg** (CPP-3, Medium). U1's acceptance
  measures parse+hash against a ~1 s threshold that triggers a structural content split —
  state whether that number is Debug or Release; add a Release|x64 compile + `selfTest` leg
  to CI; run standing spike 2 once; resolve the shader-model fork deliberately (Debug builds
  SM 6.7/dxc via per-file overrides, the never-built Release path says SM 5.1/fxc, the
  documents say fxc — verified in `Outpost.vcxproj`); pin the CI toolset.

### Before U2/T1 — the universe runtime and the transfer bus

- **Design universe-layer identity, both halves** (SIM-1 + NET-2, both High). Ship ids: a
  registry-owned allocator; authored occupants take deterministic ids derived from their
  anchor; `World::Spawn`'s self-allocation (today per-world from 0 — `World.h:361`) becomes
  injection; state the u16 headroom/reuse policy. Player id: mint a durable `PlayerId`
  distinct from `ConnectionId` (today identity is `m_nextClientId++`, erased on disconnect,
  discarded at the seam — `Main.cpp:207`); sessions survive disconnect for a grace window;
  every player-keyed thing U3b/T1 build — presence, view rights, summaries, rosters, order
  and transfer logs — keys on it, even while it has one value.
- **Make the transfer bus's order total** (SIM-3, High). ADR-016 §4 says "(arrival tick,
  order id)"; ADR-017 §9/T1 say "(apply tick, record order)"; order ids are per-world
  counters, so the first reading can tie and the second is undefined until registry iteration
  order is pinned. One sentence — a universe-layer monotonic `transferId` as the tie-break,
  anchor-id iteration order, in-flight transits folded into the registry hash — stated once,
  referenced from both ADRs, before T1 writes the log format.
- **Close the teardown/viewer/replay loop** (SIM-2, High). Name the empty-world quiescence
  invariant and test it; either exclude ship-less worlds from the hash/replay domain or log
  spin-up/teardown as transfer-log events; record "durable grid-local state lives at the
  universe layer, worlds forget" as the standing rule future `Site` content must obey — or
  redesign teardown now if that rule is not intended.
- **Write the world-isolation contract and price the tick** (CPP-1 High + SIM-6 Medium).
  Amend ADR-016 §4/ADR-007 §4: worlds share nothing during `Tick`, the bus and extract are
  the only crossings, world-level fan-out is the pre-approved first parallel consumer; build
  the owner-assert ADR-007 §7 already claims; add permuted-world-tick-order bit-identity to
  the registry suite; state the budget (target grids × entities per 50 ms) and run R10's
  1,024-entity soak now, in Release — the number decides whether a broadphase for the two
  O(n²) systems (`World.cpp:154, 900`) lands before or after U2.

### Before U3b/T2 — the wire widens

- **Design the replication growth path; build the per-client sender now** (NET-1 High +
  SIM-4 High + UX-2 Medium + SIM-7 Low). The full-snapshot budget closes at 43 records after
  T2 with the MVP's own content at 42; the designed over-cap behaviour is *no snapshot at
  all*; and every operative mechanism behind "delta + interest later" is undecided — no
  snapshot-ack message, no keyframe/join path (a view switch *is* a mid-session join), no
  relevance hook, `lastOrderSeqProcessed` world-global and folded into the world hash
  (`WorldHash.cpp:128`). Decide now: the ack and baseline owner, the initial-sync path (it
  touches the transport channel enum), the relevance hook and the graceful-degradation rule,
  the interest guarantee (owned + selected ships never culled; over-cap presence stated to
  the player via the icon ladder's counted chips), per-session state out of the world hash,
  ownership replication riding the delta slice rather than a 22nd byte. Sequencing: U3b
  builds `SnapshotSender` per-client from day one (per-grid snapshots and per-player
  summaries force it anyway); T2's `StationRoster` goes through it (ADR-017 §1 makes rosters
  per-viewer *by rule* — on the broadcast path it is a silent leak nothing tests); the
  delta/interest slice gets a named trigger (the first second-commander milestone) and, until
  it lands, bake/scenario invariants keep authored per-grid population under the cap with an
  over-cap behaviour test in T2's accept.
- **Reconcile the presence-gate's edges** (UX-3, Medium). The pinned-camera-after-departure
  contradiction, the all-fleets-in-transit state, and whether docked-presence viewing (a
  free, invulnerable observation post the moment other commanders exist) survives into
  multiplayer — three sentences in ADR-016 §7 before U3b/T2 implement the rules.
- **Parameterise the view-switch acceptances over RTT** (UX-5, Medium). The switch is the one
  UX pipeline whose budget is RTT-linear; state targets as RTT + settle, add a delay shim to
  the loopback, and name the between-surface for switches that exceed the settle — before
  U3b/U6 close green on loopback-only numbers.
- **Reserve the event record** (UX-4, Medium). Four printed surfaces (UNREAD, REVIEW LOSSES,
  the away-log, the strategic stream) need a per-commander append-only record no ADR owns,
  and its producer must sit inside the universe runtime as events occur — name it beside the
  transfer bus so U/T slices emit into it from their first events.
- **Give universe events their toast rows and the action payload** (UX-7 + UI-6, Low). At
  U3b, extend the alerts sheet's table (priority, sourceKey, action target) and the `Toast`
  type; the palette-refresh and user-layer-schema notes ride the same housekeeping.

### Before U5/T3 — the screens

- **Write the UI-architecture ADR** (UI-1 + UI-2, High; UI-5, Medium). One short ADR before
  either screen starts: the active-surface model (what a screen is, which passes run beneath
  it, what state persists across navigation), input routing as ordered consumption (active
  surface → cross-surface layers → world), the text-input reservation (focus owner,
  `TextEditState`, `WM_CHAR`/IME posture in `Window` — today no path exists while U5 ships a
  search box and T3 ships renames), one scrolling-list primitive, and the screen-data
  contract across the seam (static topology crosses once at boot as a neutral graph; live
  data as summary-keyed rows; search and route-solve as GameLogic pure functions), plus the
  proactive answer to ADR-014's fifth-project question. The fence R9 held moves up one level;
  it does not fall.
- **State the text and display envelopes** (UI-3 + UI-4, Medium). Player-text charset policy
  (atlas-baked set only, validated at input and on user-layer load) with the i18n reopen
  named; effective UI scale = DPI-derived default × user preference, `WM_DPICHANGED` as
  resize-plus-rescale, a stated minimum client area — before three screens hardcode zone
  tables against the undocumented envelope.
- **Size render budgets from the corpus caps and run the instruments** (CPP-5, Medium). The
  256 KiB upload ring cannot hold U5's ~1 MB strategic-map draw list, and the designed
  exhaustion behaviour drops the whole HUD; record the sizing rule (caps, not current
  content), run standing spike 3 and S5's frame-time check before U5 measures the map
  against MVP constants.

### Before the first remote client / second commander

- **The remote-play ADR** (NET-4, Medium). Server trust model (pinning vs PKI — the choice
  depends on the official-service vs player-hosted answer), the `Authenticate`/token slot in
  `Hello` (reserve it in NET-2's schema cluster), the transport config surface (bind address,
  credential source, validation on by default off-loopback), a first-order abuse posture; and
  amend ADR-008 §8's "no architectural work remains" to point at it.
- **Gate behaviour, not just shape** (SIM-5, Medium). Fold the verdict-affecting constants
  (`MAX_ORDER_LEGS`, the play-area bound, `DOCK_RADIUS_METRES`, the check order) — or a
  single `GAMEPLAY_VERSION` bumped by rule — into the schema text, and write where
  "retunable as table edits" is written that a retune of any validation bound is a
  compatibility event.
- **Decide device removal** (CPP-2, Medium). Name the recovery model (or explicitly accept
  relaunch-and-reconnect as shipped behaviour) and record the invariant that keeps the
  retrofit cheap — no session state may hold a device reference — before two more phases of
  device resources erode it unenforced.
- **Annotate R10's split validation** (NET-5, Low). The 1,024-entity figure currently has a
  sim-only validation path; the wire half is blocked on the NET-1 design. Say so in the
  register, and run the sim half now.

### Standing — the team axis

- **Pin the invariants a machine can hold** (CPP-4, Medium). A common props file (or CI
  guard) for `PlatformToolset`/`LanguageStandard`/`ConformanceMode`/`/fp:precise`/no-`/arch`
  — today the pins the determinism story cites are MSVC defaults, stated in prose only;
  derive build.yml's hand-spelled guard lists from the tree; add the clang-tidy step
  AGENTS.md already anticipates. Before the second contributor, not after.

## Findings index

| ID | Severity | Finding (one line) | Decide by |
|---|---|---|---|
| UX-1 | High | Fleet-scale docking geometrically unsatisfiable (19.2 km Line vs 5 km radius); §5 protection arithmetic ~2× off | Before U1/T1 |
| NET-1 | High | Replication growth path is a label: no ack, no keyframe, no relevance hook; 43-record cliff fails total | Design before U3b/T2; build by first 2-client milestone |
| NET-2 | High | No durable player identity; U/T about to key presence/rosters/summaries on connection-lifetime ids | Before U3b/T1 |
| CPP-1 | High | Multi-grid tick: no budget, no soak, world-parallel contract unwritten, owner-asserts unbuilt | Before U2 |
| UI-1 | High | No screen/surface model (navigation, input consumption, per-surface state) while two screens are buildable | Before U5/T3 |
| UI-2 | High | Text input does not exist (no WM_CHAR/IME/focus) while search, renames, binding capture are designed | Before U5 |
| SIM-1 | High | Ship-id allocation is per-world-from-zero; collides at first transfer; no session allocator designed | Before U2/T1 |
| SIM-2 | High | Teardown/recreate makes viewer behaviour a hidden sim/replay input; quiescence invariant unstated | Before U2 |
| SIM-3 | High | Transfer-bus ordering key specified two ways (ADR-016 vs ADR-017); neither total | Before T1 |
| SIM-4 | High | Per-grid population ungoverned while replication hard-stops at 43 with margin one | Before T2 |
| UX-2 | Medium | Shared-grid replication has no player-facing contract (ownership bytes, interest guarantee, over-cap honesty) | With the delta/interest design |
| UX-3 | Medium | Presence-gate edges unreconciled (pinned camera, all-in-transit, docked observation post) | Before U3b/T2 |
| UX-4 | Medium | Four printed surfaces need a per-commander event record no ADR owns | Before T1/U3a |
| UX-5 | Medium | View-switch budget is RTT-linear; every acceptance number is loopback-derived | At U3b |
| NET-3 | Medium | Target concurrency undocumented; decides whether single-process is a fact or a fence | Now (one page); topology ADR before U2 if MMO |
| NET-4 | Medium | Remote-network security undesigned (trust model, Authenticate, abuse posture) with no owning slice | Before first remote deployment |
| CPP-2 | Medium | Device-removed handling has no owner; remote clients make it mandatory | Decision now; slice by first remote milestone |
| CPP-3 | Medium | Debug-only CI on an unpinned toolchain; perf gates measured in a config that ships nothing; latent shader fork | At U1 |
| CPP-4 | Medium | Toolchain invariants (/fp, /arch, toolset) exist only as prose + per-project copies; guard lists hand-spelled | Before the second contributor |
| CPP-5 | Medium | Render budgets sized for MVP content (256 KiB ring vs ~1 MB map draw list); sizing instruments unrun | Before U5 |
| UI-3 | Medium | Player-authored text meets an ASCII-baked atlas; i18n posture has no owner or trigger | Before T3 |
| UI-4 | Medium | Resolution/DPI/aspect envelope undocumented; no DPI→scale rule, no WM_DPICHANGED | Before U5/T3 zone tables |
| UI-5 | Medium | Screen-scale data across the WorldView seam undecided; fifth-project trigger is reactive | Before U5, day one |
| SIM-5 | Medium | Schema hash gates shape only; validation-bound retunes silently break parity across build skew | Next schema cluster; before first post-remote patch |
| SIM-6 | Medium | Two O(n²) tick systems, budget stated per-world only, 1,024 soak never run, isolation invariant unstated | In U2's spec; soak now |
| UX-6 | Low | Single authored arrival/undock points serialise hub traffic; anchor schema commits at U1 | Before U1 |
| UX-7 | Low | Universe events have no toast rows; `Toast` cannot carry its promised action | At U3b |
| NET-5 | Low | The 1,024-entity cap has only a sim-side validation path; register overclaims | Now (annotate R10) |
| UI-6 | Low | Actionless toasts, boot-frozen palette, user-layer schema about to outgrow its charter | In respective slices |
| SIM-7 | Low | Ownership is a promised column that spends the last snapshot byte; must ride the delta slice | Recorded line now |

Severity totals: 0 Critical · 10 High · 15 Medium · 5 Low.

## Questions for the owner

The panel's questions, deduplicated. The first is the one that re-prices the rest.

1. **What scale is this game for?** (a) How many simultaneous commanders must one session
   hold — ≤8 co-op, dozens, hundreds? (b) How many commanders may share one *grid*? (c) Is
   the 2,500-system universe one persistent shard or an instance per session? (d) What is the
   persistence horizon — a long-lived server with the RESUME card taken literally, minting
   ships for months? (e) What per-commander envelope (owned ships, concurrent fleets) should
   surfaces and summaries be sized against? (f) Are 1,024 entities per viewed grid still the
   normative cap for the interest/delta design to meet? *Decides: NET-1/3, SIM-1/4, UX-2/3,
   CPP-1's budget numbers.*
2. **When is the first two-real-clients milestone** — before, during, or after the U/T
   phases? *Decides whether the per-client sender, snapshot-ack, and `PlayerId` must ride
   U3b/T2 (the panel's recommendation if "during or sooner") or may follow.*
3. **May T1/U3b key rosters, summaries, presence, and logs on a durable `PlayerId` now**
   (fixed to one value), reserving the `Hello` token/resume field in the same schema
   cluster? *NET-2; one bump now versus re-keying those systems later.*
4. **Dock semantics at fleet scale:** is "together, one moment" load-bearing (then the
   radius must derive from the solved footprint), or is the undock-waves model acceptable
   for docking too? *UX-1.*
5. **Durable grid-local state:** is "durable state lives at the universe layer, worlds
   forget" the permanent rule (mined-out fields and wrecks live as universe records), or
   should any future grid content persist through teardown — which changes the teardown
   design itself? *SIM-2.*
6. **May T1 introduce a universe-layer monotonic `transferId`** (new hashed state) as the
   transfer bus's tie-break, or should the key derive from existing identities (which then
   waits on the SIM-1 allocator)? *SIM-3.*
7. **Compatibility posture for tuning:** when a validation bound is retuned, is refusing
   stale clients at the door acceptable (fold the constants into the schema text — strict,
   simple), or is a compatibility window wanted (a behaviour version with server-side
   tolerance)? *SIM-5.*
8. **Operation model:** official hosted service only, or also player-hosted dedicated
   servers? *Picks NET-4's certificate trust model and whether accounts are a service
   dependency of the dedicated binary.*
9. **Which build configuration do the phase acceptance numbers mean** (R17's ~1 s parse
   threshold, U5's frame budget, the tick budget) — and is a Release CI leg approved so
   they can be measured there? *CPP-3.*
10. **Is the Debug shader build's SM 6.7/dxc setting deliberate** (superseding S7a's
    fxc/SM 5.1, in which case the docs and the Release path should follow) **or drift** (in
    which case 5.1 returns)? *CPP-3.*
11. **Is Windows 11 / Server 2022+ the recorded platform floor**, or provisional pending the
    msquic OpenSSL-flavour question flagged in Dependency-Map §7? *CPP's platform question.*
12. **Device removal:** is relaunch-and-reconnect acceptable shipped behaviour for the first
    remote-client milestone, or is mid-session recovery owed? *CPP-2's scope.*
13. **UI decisions bundle:** (a) does the settings screen own a keybind surface (two docs say
    yes, the print's own note says open)? (b) Is touch/pad input on any horizon, or is the
    48 px floor target-size discipline only? (c) What minimum client area / DPI-default
    should the screens be built against — are the prints' 1440×900 proportions the floor?
    (d) May "editable text accepts only the atlas's baked set; localisation is post-roadmap
    with a named reopen" be recorded as the standing posture? (e) When U5/T3 build, are
    screens engine surfaces fed neutral data through a wider seam, or does ADR-014's deferred
    fifth project (a game-side presentation home) get decided now? *UI-1…5; (e) is needed on
    U5's first day.*
14. **Is presence-gated viewing an intel rule or a convenience?** May a pinned camera outlive
    presence; does watch-from-dock survive into multiplayer, or are both placeholders the
    intel-overlay design may revoke? *UX-3.*

---

# Appendix — the five reviews in full

Each review was produced independently against the same mandate and severity scale, then
included here verbatim. Where an appendix and the synthesis differ in emphasis, the appendix
is the reviewer's own voice; the synthesis is the consolidation.

## Appendix A — User experience review

### Verdict
The player-facing baseline is unusually well-shaped for scale: the ghost/bounce/ETA grammar is latency-shaped by construction, the toast machine already implements the full five-priority taxonomy device-free, fleets and wings are emergent (nothing to desync, nothing to manage), and the one-viewed-grid + summaries + map-as-between-surface attention model is the right architecture for many fleets. The structural gaps cluster where rules written against a single commander on loopback meet arithmetic and multiplayer: the dock rule is geometrically unsatisfiable for the game's own canonical fleet, the presence-gate has unreconciled edges that a second commander turns into an intel economy, the replication/IFF contract for shared grids is unpriced, and four corpus surfaces demand a persistent event record no ADR owns. All are decisions cheap to take now and expensive to renegotiate after T1–T3/U3b harden them.

### Sound for scale — do not churn
- **The order grammar is RTT-proof by design**: client optimism about the *order* only, never the ships; one reason home on two surfaces whether verdict is local or remote; queued-leg ETAs honestly labeled prediction vs the authority's replicated per-leg figure (OrderGhost.h:12–41, Snapshot.h:91–116, ADR-005 §4/§4a, Architecture-Overview "one data flow"). Added RTT lengthens PENDING without changing any state's meaning.
- **The toast taxonomy is structurally sufficient at many-fleets scale and already built**: priority as the only axis, mandatory coalescing on (priority, sourceKey), 5-visible cap, queue-not-drop suppression keyed on a replicated combat flag, DroppedCount telemetry (ToastStack.h:49–70, 125–135, 144–166; alerts-and-toasts.png §1–§3).
- **Presence-gated single view + ~1 Hz summaries + the map as between-surface** bounds per-grid attention load (1,024 cap + the icon sheet's density ladder to counted chips) and makes command-by-exception the scaling posture (ADR-016 §6–§7, tactical-icon-system.png §5–§6).
- **Emergent fleets and wings** (the undock selection *is* the fleet; a wing exists iff a ship carries its id; remote hangar is a badge, not a mode) delete a whole class of multi-client shared-state UX before it can exist (ADR-017 §1/§3/§6, station-screen.png).
- **Session entry is structurally ready for accounts and remote clients**: fail-closed UpdateRequired as a designed main screen, honest queue (depth + drain rate demanded of the service), reconnect-under-fire with a structured away-log requirement (session-surfaces.png §1–§3).
- **The degradation grammar** — interp → extrap ≤ 250 ms → freeze + STALE per entity, grid-identity smear guard, the designed settle instead of a pretended instant — is the honest shape for real-network loss (ADR-002 §4, ADR-016 §6–§7).

### Findings

#### UX-1 — Fleet-scale docking is geometrically unsatisfiable as specified
- **Severity:** High
- **Refs:** ADR-017 §2 ("every member of the order inside `DOCK_RADIUS_METRES` (5,000 m)", "warp-arrive → dock chains without a crawl"), §5 ("Fifteen seconds at a Battleship's cruise covers a ~3 km parking flight"); Station-Build-Order T2 ("Move to perimeter → Dock when every member is in radius, surfaced as a DOCKING chip"); ShipClass.cpp:50 (Battleship: spacing 480 m, maxSpeed 105 m/s, accel 14 m/s²); Orders.h:166 (`MAX_SHIPS_PER_ORDER` 64); ADR-005 §3a (adjacent stations exactly one spacing apart; spacing from largest member); station-screen.png ("the >64 case is declared before commit as waves"; wave-2 trigger OPEN for undock only).
- **Design position:** A fleet docks together, instantly, once every member of the order is inside 5 km of the structure; trickle docking is explicitly rejected ("partial fleets are exactly what the together-instant rule exists to prevent"); the warp-in standoff sits inside the dock radius so arrive→dock chains. Undock got a footprint treatment (waves, clamp-and-heal); dock got none.
- **Scaling risk:** Axis B/D, and it fires at the phase's own milestone, not at MMO scale. A 41-ship order paced by a Battleship spans (41−1)×480 m = 19.2 km in Line (±9.6 km from anchor); Claw at 41 ships has R ≈ 480/(2·sin 1.5°) ≈ 9.2 km; a Wedge arm runs ~9.6 km. Any Battleship-paced selection above ~22 ships (Line) — including the canonical starting fleet — can never place every member inside 5 km in any formation the game has, so T2's DOCKING chip never fires and Dock bounces `NotAtStation` forever. The arrive→dock chaining claim is false for the same fleets. Secondary arithmetic error in the same section: a Battleship from rest covers ~1.18 km in 15 s (0.5·14·7.5² + 7.5·105), not the claimed ~3 km, so capital protection lapses mid-parking-flight once combat exists.
- **Why structural:** T1 pins the radius rule into the shared validation and parity matrix, U1 bakes warp-in-inside-dock-radius as an invariant across 2,500 committed systems, and T2 builds the chip's contract on it. Discovering this at H1 means re-opening a recorded owner rejection (trickle docking) under playtest pressure, re-writing the chip semantics, and re-running the bake — versus one clause now.
- **Recommendation:** Amend ADR-017 §2 before T1 with an explicit fleet-scale dock semantic — choose one: (a) dock waves as the declared semantic (mirror the undock-waves clause, with the same open trigger rule the P1 print already poses), (b) dock radius derived from the solved formation footprint, or (c) a consume-on-entry radius with the "together" guarantee restated per-order rather than per-fleet. Re-run §5's protection arithmetic against the class table in the same edit.
- **Confidence:** High

#### UX-2 — Shared-grid replication has no player-facing contract (ownership, interest, over-cap honesty)
- **Severity:** Medium
- **Refs:** EntityRecord.h (20 bytes; id/type/group/pos/vel/heading/gauges — no owner); Validate.h:42–44 ("When ownership arrives (multiplayer, post-MVP) this is where the owning player id joins"), Validate.cpp:87–88 (`NotOwned` unreachable, "ownership is a field, not a redesign"); Snapshot.h:74–76 (1,024-cap ⇒ ~20 KB, "delta encoding plus interest management is the designed growth path"), 171–176 (asserted floor 41, cap 43 after statusBits), 184–189 (an over-budget snapshot writes *nothing*); ADR-017 §5 (margin's named future consumers: per-grid header, gauges — ownership absent); tactical-icon-system.png §3 (colour carries relationship, flags Public, four standings rows).
- **Design position:** Multi-client is a reserved seam (session table, `mode: "client"`, `NotOwned` numbered, validation extension point named). The icon system fully designs four-party IFF. Delta+interest is the reserved wire growth path. No document prices what a *second commander on your grid* needs.
- **Scaling risk:** Axis A, on day one of two real clients: two 41-ship fleets plus a structure ≈ 83 entities > the 43-entity datagram cap, and today's write path then sends *no snapshot at all* — the viewed grid goes STALE wholesale. Independently, the relationship colour channel needs per-entity owner/standings data that costs wire bytes nobody has priced (a third consumer of a margin already at two ships). And once interest culling exists, a distance-first default can cull the player's *own or selected* ships — breaking pre-check parity (ValidationView is ids off the snapshot) and the whole bounce grammar for the command set.
- **Why structural:** The wire mechanism is the network reviewer's ground; the *contract* is not, and it shapes the mechanism: "owned + selected ships always replicate; culling is communicated" is one paragraph now, but retrofitting it after an interest scheme ships means reworking replication priority, pre-check inputs, and the icon system's honesty claim (F10: presentation is a pure function of replicated fields) together.
- **Recommendation:** Take the decision now as an ADR-004/016 amendment: (1) ownership/relationship is a named replication field with its budget cost on the page, (2) the interest guarantee — the player's owned and selected entities are never culled from their viewed grid, (3) the over-cap affordance — extend the density ladder's counted-chip rung to state unreplicated presence ("how many, roughly where" for what the client is *not* being sent).
- **Confidence:** High

#### UX-3 — The presence-gate's edges are unreconciled, and multiplayer monetizes them
- **Severity:** Medium
- **Refs:** ADR-016 §7 ("the view may point at any grid where the player has ships — and nowhere else"; "a grid stays alive while it has ships *or a viewer*, so a world is never torn down under the player's camera"; auto-follow "unless the camera is pinned"); ADR-017 §7 (docked ships count as presence; "you may watch the space outside a station you are fully docked at"; docked ships invisible to others).
- **Design position:** Presence-gated viewing is the stated placeholder posture until the intel overlay is designed; the docked amendment answered the dock-your-last-fleet case explicitly.
- **Scaling risk:** Axis A. Three edges are undefined or contradictory: (1) a *pinned* camera on a grid your last ship warps out of — the permission rule says the view must leave, the teardown rule says the world survives under your camera; nothing says where the view goes or when; (2) every fleet simultaneously in transit — presence exists nowhere (transit records are not grids) and the view has no legal target; (3) docked-counts-as-presence makes every hub a free, invulnerable, invisible observation post the moment other commanders' traffic is worth watching — a sharper giveaway than the placeholder posture intended, since docked is also perfectly safe and perfectly private.
- **Why structural:** U3b and T2 implement the view rules; whatever reading they harden becomes the multiplayer intel baseline. Re-gating vision *after* players have it is the classic MMO rework (interest/intel retrofits touch view subscription, summaries, and teardown), whereas each edge is one sentence today.
- **Recommendation:** Amend ADR-016 §7 with: the view fallback chain when presence is lost under a pinned camera (map, by rule); the all-in-transit state (map is the view); and an explicit statement of whether docked-presence viewing is intended to survive into multiplayer or is single-commander convenience the intel design may revoke.
- **Confidence:** High (the ambiguity); Medium (the multiplayer cost)

#### UX-4 — Four promised surfaces need a persistent per-commander event record no ADR owns
- **Severity:** Medium
- **Refs:** alerts-and-toasts.png §2 (context bar "3 UNREAD"); ToastStack.h:105–110, 125, 174 (dwell-then-gone, 5 visible, `Clear()` on disconnect); session-surfaces.png §3 ("A structured away-log from the AI commander — orders it executed and losses taken … Required from 06/04, or the screen cannot be built"; "Is REVIEW LOSSES a screen or a filtered journal? … no document has scoped"); strategic-map.png (history scrubber fed by "the strategic stream — the 4X layer's memory"); ADR-016 §8 / ADR-017 §2 (disconnect halts at gate / outside station — events with no witness); ADR-017 ("No persistence" is a stated non-goal).
- **Design position:** All in-session feedback is client-raised and transient; the away-log, killmail journal, and strategic stream are corpus requirements on the *service*; persistence is deliberately deferred, with the docked roster named as the save anchor.
- **Scaling risk:** Axis A/D. At 10 fleets across 50 systems the model is command-by-exception, but an exception that toasts while the player is heads-down for three minutes expires unseen and exists nowhere afterward — route halts, berth-exhaustion holds, refusals on unwatched grids become "why is my fleet parked at a gate?" archaeology. The moment remote links make disconnects routine, "what happened while the link was down" has no producer: toasts are cleared on disconnect by design, and summaries carry state, not history.
- **Why structural:** The consuming surfaces are already normative (the UNREAD counter is drawn on the print; the reconnect screen "cannot be built" without the log), and the producer must be fed from inside the sim/universe runtime as events occur. Reserving it now is the transfer-bus move — a named mechanism events emit into from birth (U/T phases generate arrivals, docks, halts from their first slice); retrofitting emission across World, the registry, and the order path after two phases is the expensive direction.
- **Recommendation:** One design decision, recorded as an ADR or an ADR-016 amendment: a per-commander, append-only event record at the universe layer (beside transit records and rosters) is the single producer behind the toast backlog/UNREAD, REVIEW LOSSES, the away-log, and the strategic stream; U/T slices emit into it from their first events even if the only consumer for now is the UNREAD count.
- **Confidence:** Medium

#### UX-5 — The view-switch is the one UX pipeline whose budget is RTT-linear, and its acceptance is loopback-only
- **Severity:** Medium
- **Refs:** Architecture-Overview:94–96 ("the same numbers hold over a real link plus RTT — nothing about the MVP flow assumes locality"); ADR-016 §7 (view switch = one interp-buffer refill, "designed ~200 ms settle"); Risk-Register R18 + Universe-Build-Order U3b accept ("roster-click to smooth motion in under half a second, **over the real loopback** in `selfTest`"); U6 (fleet cycling on a key; toast click jumps focus across systems); ADR-016 §6 (late datagrams from the previous view are dropped, never blended).
- **Design position:** The latency claim is made for the order flow, and there it genuinely holds — the ghost masks RTT and staleness bounces are the designed exception. The view switch is specified as a ~200 ms settle, validated on loopback.
- **Scaling risk:** Axis A. A switch is request → grid-switch notice → ≥2 snapshots of the new grid: RTT + ~100–150 ms + settle, i.e. 350–600 ms at real internet RTTs — and the smear guard rightly freezes the old grid the instant the request leaves, so the whole gap is dead screen unless something is designed to fill it. Fleet cycling serializes this per keypress; U6's "cycling visits every owned fleet" becomes a slideshow of settles at 10 fleets.
- **Why structural:** Not the mechanism — it degrades gracefully — but the *acceptance numbers* are design artifacts other slices build against, and every one is loopback-derived. If U3b/U6 close green on loopback, the first remote client re-opens closed slices; deciding the degraded-switch presentation (hold-last-frame vs map interstitial vs summary card) after the surfaces exist means re-touching the switch, cycling, auto-follow, and toast-jump paths together.
- **Recommendation:** Amend U3b/U6 accepts and R18 to parameterize the switch budget over injected RTT (a delay shim on the QUIC loopback), state the target as RTT + fixed settle rather than an absolute, and decide now what occupies the screen when a switch exceeds the settle — the map-as-between-surface is the natural answer and should be named for cross-system switches.
- **Confidence:** High

#### UX-6 — Single authored arrival/undock points serialize traffic at popular anchors
- **Severity:** Low
- **Refs:** ADR-016 §3 (one warp-in point + facing per anchor; "arrival is never random"; "separation guarantees clean water even when two fleets arrive the same tick"); ADR-017 §3 (one undock point per station, ~800 m off); ADR-015 (Separate heals overlap positionally); puck-and-wheel/ADR-005 §3a (obstructed footprint remains OPEN).
- **Design position:** Every arrival formation-solves centred on *the* warp-in point; every undock spawns at *the* undock point; separation is the floor.
- **Scaling risk:** Axis A/B, at hub traffic levels: continuous arrivals all centre on one point, so the common case at a trade hub becomes spawning into whoever is already sitting there — the known clamp-and-heal shove, promoted from edge to norm; gate warp-ins invite the same pile-up (and future campers sit exactly there).
- **Why structural:** Mostly it isn't — deterministic arrival offsets or multiple authored points are contained changes — but the anchor schema is committed across 2,500 baked systems at U1, so the vocabulary question (one point vs an arc/ring of points) is cheapest settled before the first bake.
- **Recommendation:** Record the intended contention answer in ADR-016 §3 (e.g. deterministic per-order offset around the warp-in bearing), and let U1's anchor record carry whatever fields it needs from the first bake.
- **Confidence:** Medium

#### UX-7 — Universe-phase events have no assigned priorities, and the toast type cannot carry its promised action
- **Severity:** Low
- **Refs:** ToastStack.h:81–111 (`Toast` = priority, sourceKey, count, head, detail — no action/target); alerts-and-toasts.png §1 (CRITICAL carries one action, "JUMP TO"); ADR-016 §7 ("warp arrivals feed the alerts rail, and the toast is clickable"); U6 accept ("a toast click jumps focus across systems").
- **Design position:** The taxonomy's qualifying rows are combat/economy-era examples; the machine is generic; U6 requires clickable focus-jumping toasts.
- **Scaling risk:** Axis D housekeeping: arrivals, route halts, berth holds, dock/undock completions have no assigned priority or sourceKey convention (10 concurrently routing fleets are 10 sources — deliberate coalescing keys are what keep the 5-slot stack from churning), and the action payload for JUMP TO does not exist on the type.
- **Why structural:** It barely is — all additive — but the mapping is a design decision the print's taxonomy should own, not something U3b improvises per call site.
- **Recommendation:** At U3b, extend the alerts sheet's §1 table with the universe/station event rows (priority, sourceKey convention, action target) and add the action field to `Toast` in the same slice.
- **Confidence:** High

### Questions for the owner
1. **Dock semantics at fleet scale (changes UX-1):** when the full 41-ship fleet docks, is "together, one moment" load-bearing — in which case the radius must scale with the solved footprint — or is the undock-waves model (already on the P1 print) the acceptable semantic for docking too?
2. **What is the design envelope per commander?** A target number of owned ships and concurrent fleets (e.g. "~200 ships, ~12 fleets") would size the roster blocks, summary bandwidth, fleet cycling, and toast coalescing decisions; today no document states one, and several surfaces (16 roster rows, 5 toast slots, cycle-on-a-key) are silently calibrated to single digits.
3. **What is the first multi-client target — two cooperating clients on one shard, or open concurrency?** The 43-entity snapshot margin means even two commanders meeting at one hub forces the delta/interest step (UX-2); knowing whether that step precedes or follows the universe phase decides when its UX contract must be written.
4. **Is presence-gated viewing an intel rule or a convenience (changes UX-3)?** Specifically: may a pinned camera outlive presence on a grid, and is watch-from-dock intended to survive into multiplayer, or should both be marked as placeholders the intel-overlay design is free to revoke?

## Appendix B — MMO & network review

### Verdict

The MVP's network baseline is unusually disciplined for its stage: a QUIC-shaped transport validated against two implementations, a fail-closed dual schema hash that even covers quantisation constants, an integer-quantised wire with one shared validation function, and a jitter-grade client clock — all real in code, not just in ADRs. Two structural gaps sit behind that quality, and both are about to be built around rather than through: the entire per-client replication apparatus (interest, baselines, keyframes, per-client emit) exists only as a reserved header field and a planned filename while the U/T phases spend the last bytes of the single-datagram budget, and there is no durable player identity anywhere — the seam hands the game a clientId and the game throws it away — while ADR-016/017 are about to build player-keyed presence, rosters, and summaries on top of that absence. Neither requires code now; both require decisions now, and both change urgency dramatically depending on a target concurrency the corpus never states.

### Sound for scale — do not churn

- **QUIC-only behind a QUIC-shaped seam, encryption live since S13** — the abstraction was validated against two implementations before the second became the only one; the swap touched two type names (ADR-003 §4; NeuronCore/QuicTransport.h; Risk R3 resolved with measured 0.305 ms min RTT).
- **Idempotent full snapshots + interp/extrap≤250 ms/STALE + slew-limited `t_est` (±2 %, snap past 10 ticks)** — the loss/jitter posture is real-network-grade already, tested device-free; nothing assumes loopback (ADR-002 §4; NeuronClient/SnapshotBuffer.h:41–55; R5).
- **Fail-closed dual schema hash covering field layout, quantisation constants, enum values, and caps** (NeuronCore/Wire.h:190–208; GameLogic/SchemaHash.h:44–58) — makes every D-axis schema bump a clustered, refuse-at-the-door event; exactly right for a no-cross-version-compat posture, and it scales with team growth (E).
- **Integer-quantised wire with one shared validation function consuming quantised inputs** (ADR-004/005; GameLogic/Orders.h:145–166, Validate.h) — eliminates cross-machine float-parity risk *before* a second machine exists, and yields delta-friendly integer fields for later.
- **Per-anchor independent `World`s, between-tick transfer records ordered by (arrival tick, order id), per-anchor PCG32 seeding** (ADR-016 §4; R16) — grid-level sim parallelism (ADR-007 §4's reserved seam) and even future cross-host sharding stay compatible with the replay contract, because transfers are already serialised, logged data rather than shared memory.
- **Server-only verdicts, ghost-only optimism, bounds-checked decode as the hostile boundary, presence-gated views, and the `systemIssued` order flag** (GameLogic/OrderMessages.h:44–53; ADR-016 §7; ADR-017 §4) — the cheat surface is structurally small, and `systemIssued` is precisely the seam the future server-side AI commander needs, so the client-fed-routes decision is not a dead end.

### Findings

#### NET-1 — The replication growth path is a label, not a design, and the designed phases are spending its margin

- **Severity:** High (becomes Critical the day a second concurrent client is promised)
- **Refs:** ADR-004 §6; ADR-016 §6; ADR-017 §5, §8; Dependency-Map.md:107–108 (*(planned)* `Session.h`, `SnapshotSender.h`); ADR-014 Consequences ("Interest management later needs a generic relevance hook on `Simulation`"); NeuronCore/Transport.h:29; GameLogic/Snapshot.h:78, 156–176; NeuronServer/ServerHost.h:100–103, ServerHost.cpp:93–129; GameLogic/World.cpp:450 + WorldHash.cpp:128; Outpost/Main.cpp:331–392; Risk R10, R18.
- **Design position:** Full snapshots every tick, one per datagram; `baselineTick` reserved so "delta-vs-acked-baseline slots in later without a format break"; "delta + interest management is the designed growth path, not larger datagrams" (ADR-004 §6). Today `ServerHost` serialises once and broadcasts, "correct while every client sees the whole world" (Dependency-Map:108); `Simulation::WriteSnapshot(tick, writer)` takes no client. ADR-016 §6 makes snapshots per-grid with "budgets unchanged"; ADR-017 §5 prices `statusBits` and names delta as the growth path.
- **Scaling risk:** The arithmetic closes exactly and then stops. Budget chain: 1,152 − 2 framing − 16 header − 224 reserved order area = 910 B → **45 ships × 20 B today, 43 × 21 B after T2**. MVP content is 40 fleet + 1 station = **41 records** (1,060 B of 1,150) — margin of two records for the entire U/T roadmap. One more byte per record (e.g. an owner id, which multi-client icons need — tactical-icon-system.png §3's OWN/ALLIED/NEUTRAL/HOSTILE is a replicated-relationship channel) gives 910/22 = **41, the asserted floor, margin zero**. Axis A first step: two 40-ship commanders on one grid — the starter station is the natural meeting point — is 81 records ≈ 1,861 B, 62 % over budget, and the designed failure mode is **total**: `WriteSnapshot` refuses, `BroadcastSnapshot` drops the tick for every viewer of that grid ("clients will see nothing move", ServerHost.cpp:116). At the corpus cap: 1,024 × 21 B = 21.5 KB ≈ 19 datagrams/tick ≈ 3.4 Mbps/client at 20 Hz. Axis B/C: rosters are unlimited and `Undock` names up to 64 ships (ADR-017 §3) — content growth past ~42 hulls per grid hits the same cliff.
- **Why structural:** The reserved seam is one header field plus a planned filename; every operative mechanism is undecided: (1) no client→server snapshot-ack exists in the message set (C→S is Hello/OrderSubmit/Ping/Goodbye only — Wire.h:26–64), and delta-against-acked-baseline cannot work without one; (2) no keyframe/join path — a first snapshot of a busy grid won't fit one datagram, and ADR-016 §7's view switch *is* a mid-session join, while the transport seam exposes exactly one reliable stream shared with orders (head-of-line) and no fragmentation story; (3) no interest authority — ADR-014 names the "relevance hook" in one sentence; (4) per-client state is already fused into shared structures: `lastOrderSeqProcessed` is world-global, written as a max across all submitters (World.cpp:450), **and folded into the world hash** (WorldHash.cpp:128), so making it per-client later edits the replay/determinism contract, not just the wire; (5) ADR-017 §1's privacy promise ("other commanders cannot see what is docked") makes `StationRoster` per-viewer content **by rule** — T2 landing it on the broadcast-shaped sender bakes in a silent information leak that nothing tests, because nothing on the roadmap ever runs two clients. R10's and R18's registered mitigations both presuppose this machinery ("growth path reserved in wire"; "summaries on the existing datagram budget") — the reservation is real, the mechanism is absent, which is why this clears ground rule 1.
- **Recommendation:** Take the design now, build later: amend ADR-004 (or write the ADR-018 it deserves) deciding (a) the snapshot-ack message and who tracks baselines, (b) the keyframe/initial-sync path for join and view-switch (second stream vs fragmented datagrams — this touches the `Transport` seam's channel enum and should be decided while it is cheap), (c) the relevance-hook shape on `Simulation` and the graceful-degradation rule when interest still exceeds budget (priority truncation is currently forbidden by the refuse-whole-snapshot posture), (d) that `lastOrderSeqProcessed` becomes per-session state outside the world hash. Amend U3b's spec to build `SnapshotSender` per-client from day one (per-grid snapshots and per-player summaries force per-client sends anyway), and require T2's `StationRoster` to go through it. Schedule the interest/delta slice against an explicit trigger (first two-client milestone), not "later".
- **Confidence:** High — arithmetic and code verified end to end.

#### NET-2 — No durable player identity exists, and the U/T phases are about to bake player-keyed state onto its absence

- **Severity:** High
- **Refs:** NeuronCore/Wire.h:77–113 (`Hello`/`Welcome`); NeuronServer/ServerHost.h:29–35 + ServerHost.cpp:183–188, 310–325 (session = connection, erased on disconnect, `m_nextClientId++`); Outpost/Main.cpp:207 (`ApplyOrderBytes(std::uint32_t /*discarded*/, …)`); GameLogic/Validate.h:38–53 ("when ownership arrives … this is where the owning player id joins"), Validate.cpp:87–89 ("NotOwned is unreachable"); ADR-016 §7 ("any grid where the player has ships"), §6 ("fleets the player owns"); ADR-017 §1, §8; Architecture-Overview.md:169 (persistence seam = "schema-hash handshake already speaks `UpdateRequired`"); ScreenPrints/session-surfaces.png (§1 `Authenticate { sessionToken, schemaHash }`, F1 standalone accounts, queue with drop-safe place-keeping, RESUME, RECONNECT UNDER FIRE).
- **Design position:** Persistence and accounts are deliberately omitted; the recorded seam is the `UpdateRequired` handshake path. `NotOwned` is a reserved reason code and `ValidationView` names where an owner id will go. The corpus, however, promises far more than the omissions table covers: accounts with credentials, a session token in the handshake, a capacity queue that holds your place across a connection drop, resume state, and a reconnect flow in which the server kept your fleet acting while you were gone.
- **Scaling risk:** Axes A and D together. Today identity is `clientId = m_nextClientId++`, born and destroyed with the transport connection; the game discards it at the seam, so `World` has no actor concept at all. ADR-016/017 now build presence indexes ("grids where the player has ships"), view-right checks, per-player fleet summaries, private station rosters, and order/transfer logs — all keyed on "the player" — in a codebase where "the player" can only be implemented as "the singleton" or "the current connection". Every one of the corpus's session promises (queue place-keeping, reconnect-under-fire, resume) additionally requires session lifetime ≠ connection lifetime, which `ServerHost` currently conflates (disconnect erases the session row).
- **Why structural:** Retrofitting a durable `PlayerId` after U3b/T1 means re-touching the universe runtime's presence index, the view-gating rule, summary and roster addressing, order attribution in the replay log (a reconnect changing `clientId` mid-session would change replay inputs), and the wire records — i.e. exactly the systems the next two phases create. Minting the id first is a type, one `Hello`/`Welcome` decision, and a rule ("player-keyed state keys on `PlayerId`, never `ConnectionId`") — days now versus a cross-cutting migration later. The wire half stays cheap either way (fail-closed hash makes the bump safe); the *keying* of new server-side state is the part that hardens. Note the interaction with NET-1: if ownership must ever be replicated per-entity, the byte budget is already spent (22 B/record = the asserted floor), so this decision and the delta decision constrain each other and should be taken together.
- **Recommendation:** Decide now, in an ADR-008/016 amendment: (1) `PlayerId` as a durable id distinct from `ConnectionId`/`clientId`, assigned by the composition root in the MVP (one player, id fixed) and by accounts later; (2) all player-keyed state introduced by U3b/T1 (presence, summaries, rosters, order/transfer log attribution) keys on it; (3) session objects survive transport disconnect for a grace window (the reconnect print's requirement), with `Hello` growing the reserved resume/token field the print already names; (4) explicitly record that per-entity *ownership* replication is deferred to the interest/delta design (NET-1) rather than a 22nd byte.
- **Confidence:** High — verified that clientId is discarded at the seam and that sessions are erased on disconnect; the corpus promises are on the print.

#### NET-3 — Target concurrency is undocumented, and it decides whether the single-process topology is a fact or a fence

- **Severity:** Medium (decision to take now; becomes High/structural under the MMO reading)
- **Refs:** NeuronServer/ServerConfig.h:19 (`maxSessions = 8` — the only concurrency number in the tree); ADR-007 §1–4 (one Sim thread; parallelism gated on "deterministic partitioning story + profile"); ADR-016 §4 (universe runtime, in-process registry and transfer bus); ADR-008 §8 (packaging split = client/server only); GameLogic/Ids.h:45 (`ShipId = u16`, session-global once transfers preserve ids — ADR-017 §9); Risk R10; panel brief ("MMO-scale ambitions").
- **Design position:** One process, one sim thread, M grids ticked serially, all fan-out on the tick thread; grid-parallelism is a reserved seam (independent single-writer worlds, between-tick transfers) and the client/server split is a packaging change. Nothing anywhere states how many concurrent commanders a session — or the persistent universe — must hold.
- **Scaling risk:** Under the co-op reading (≤8 clients/session, sessions are instances), the current topology is adequate essentially forever: ≤8 commanders × a few fleets ≈ tens of live grids, microseconds each, and egress ≈ 8 × 184 kbps. Under the MMO-shard reading (hundreds+ on one persistent universe), three ceilings appear that are *not* reserved seams: (1) one process/one machine per universe — grid-to-host assignment, a cross-host transfer bus, and client connection handoff on view-switch are undesigned, and the replay contract's total order "(arrival tick, order id)" currently assumes one process observing all transfers; (2) `ShipId = u16` becomes a ~65 k-hull ceiling for the whole shard once ids persist across grids and rosters; (3) "splitting server from client is a packaging change" stays true, but splitting server from *server* is not a packaging change and nothing prepares it. The good news, worth stating: the design's data discipline (serialised transfer records, per-grid logs, per-anchor seeding, anchors bounding grid count) is unusually shard-friendly — the gap is a missing statement of intent, not a hostile architecture.
- **Why structural:** U2 builds the universe runtime now. If MMO-shard is the ambition, the registry and transfer bus should be specified as location-transparent (records and logs, never cross-grid pointers; already their instinct) and the id-width question answered before 2,500 systems of content and the replay corpus calcify around `u16`. If co-op-session is the ambition, several of this panel's severities drop and the register should say so, so future reviewers stop re-litigating it.
- **Recommendation:** Owner documents the target (Question 1) in the Risk Register or a one-page ADR: players per session, sessions per universe, and whether the persistent universe is one shard or many instances. If the MMO reading holds, write the topology ADR (grid-to-host assignment model, transfer-bus ordering authority, connection handoff) *before* U2 lands, and revisit `ShipId` width as part of NET-1's schema cluster.
- **Confidence:** High that the ambiguity exists and is load-bearing; Medium on the MMO-side consequences (they depend on the answer).

#### NET-4 — Remote-network security has no decided design: cert trust, the Authenticate step, and abuse posture are all "later" with no slice

- **Severity:** Medium
- **Refs:** ADR-003 §3 ("`NO_CERTIFICATE_VALIDATION` on loopback; pinning comes with real deployment, out of MVP"); NeuronCore/QuicTransport.cpp:330–331 (client validation off unconditionally — no knob exists), 740–746 (listener hard-bound to 127.0.0.1 by design); NeuronClient/ClientConfig.h (no validation/token surface); NeuronServer/ServerHost.cpp:141–281 (no per-session rate/flood policy; pre-join `Ping` answered; duplicate `Hello` from one connection adds a second session row — sessions erase one-per-disconnect, so slots can be pinned until `ServerFull`); ADR-008 §8 ("real cert validation; no architectural work remains by construction"); session-surfaces.png ("Registration … its abuse posture is a security design, not a layout"; `Authenticate { sessionToken, schemaHash }`).
- **Design position:** R3 (msquic/Schannel) is resolved *for loopback*: in-memory self-signed cert, client told not to validate, listener loopback-only so nothing widens quietly. Encryption is genuinely on. Everything internet-facing — how a client decides it is talking to the real server, how a server decides who the client is, and what a hostile-but-well-formed client may do per second — is deferred, correctly for the MVP, but with no owning decision point on any roadmap: the U/T phases never touch it, and ADR-008 §8 presents the split as already architecture-complete.
- **Scaling risk:** Axis A (remote clients, dedicated server). The failure mode is not that the work is large — it is that the split is described as trivial, so the first remote deployment ships the current posture: validation off (MITM-able by construction), no authentication beyond a display name, and an order path where any post-handshake connection can submit at unbounded rate (harm is bounded — groups are capped by ship count and QUIC isolates per-connection streams — but validation CPU and ack traffic are attacker-priced). The trust-model *choice* also has architectural weight: pinned key distributed with content (fits the schema-hash "one build, one truth" philosophy, works for an official service) versus real PKI versus per-community certs for player-hosted servers — and the right answer depends on Question 3.
- **Why structural:** Mostly contained code when it comes — *except* the parts that other decisions bake in: the `Hello` message is where the token slot lives (cheap now, a migration once accounts state exists — pairs with NET-2), the `Transport` seam needs a config surface (cert source, validation policy, bind address) which is an interface change better made once, and the packaging-split claim in ADR-008 should stop asserting completeness so the split acquires an explicit security gate instead of inheriting loopback defaults.
- **Recommendation:** A short "remote play" ADR, scheduled before the first remote-client or dedicated-server milestone, deciding: server trust model (pinning vs PKI, per operation model), the `Authenticate`/token step's place in the handshake (reserve the field with NET-2's bump), transport config surface (bind address, credential source, client validation on-by-default off-loopback), and a first-order abuse posture (per-session order/message budget with a named refusal, echoing the corpus's own "abuse posture is a security design"). Amend ADR-008 §8's "no architectural work remains" to point at it.
- **Confidence:** High — code and config surfaces verified; the deferral is real and currently unowned.

#### NET-5 — The register's own scale validations cannot exercise the wire, so "1,024 entities" remains a sim-only claim

- **Severity:** Low (worth recording)
- **Refs:** Risk R10 ("Synthetic 1,024-entity headless soak once S6 lands (cheap, scriptable)" — no result recorded); Risk-Register standing spike 3 (1,024-instance draw — "still to run"); GameLogic/Snapshot.cpp:56 (`shipCount > MAX_SHIPS_PER_SNAPSHOT` ⇒ refuse); NeuronServer/Simulation.h:53–62.
- **Design position:** R10's mitigation row treats the 1,024-entity tick budget as instrumented (spans, `tickOverrun`) and the wire growth path as reserved; its early validation is a headless 1,024-entity soak.
- **Scaling risk:** The soak as specified can only validate tick CPU: any world past 45 hulls refuses to serialise, so the replication half of the 1,024 claim (fan-out cost, datagram scheduling, client apply/interpolate at 1,024) is untestable until NET-1's machinery exists. The register currently reads as if the cap has a validation path; it has half of one.
- **Why structural:** It isn't, by itself — but an owner steering by the register should know the 1,024 figure is unvalidated on every axis that involves the wire, and will stay so until the NET-1 design lands. Recording it prevents the mitigation column from quietly overclaiming for another two phases.
- **Recommendation:** Annotate R10: split the soak into "sim-only (runnable now)" and "wire (blocked on the delta/interest design)", and run the sim-only half — it is still cheap and scriptable, and it prices the M-grids-per-tick question from NET-3 for free.
- **Confidence:** High.

### Questions for the owner

1. **Target concurrency, both numbers:** how many simultaneous commanders must one session hold (≤8 co-op? dozens? hundreds?), and is the 2,500-system universe one shared persistent shard or a universe-per-session instance? NET-1's urgency and NET-3's severity both flip on this; `maxSessions = 8` is currently the only number in the tree.
2. **When is the first two-real-clients milestone intended** — before, during, or after the U/T phases? This decides whether the per-client sender, snapshot-ack, and `PlayerId` must ride U3b/T2 (my recommendation if "during or sooner") or may follow them.
3. **Operation model for servers:** official hosted service only (as session-surfaces' standalone-accounts design implies), or also player-hosted dedicated servers? The answer picks the certificate trust model (pinned key shipped with content vs PKI vs per-community) and whether accounts are a service dependency of the dedicated binary.
4. **Should durable identity be minted during U/T even with one commander** — i.e., may T1/U3b key rosters, summaries, presence, and order/transfer logs on a `PlayerId` (fixed to one value for now) and reserve the `Hello` token/resume field in the same schema cluster, accepting one bump now to avoid re-keying those systems later?
5. **Is 1,024 replicated entities per view still normative** for the interest/delta design to meet (≈21.5 KB full state, ≈3.4 Mbps uncompressed at 20 Hz), or is the intended per-grid population ceiling for the universe phase lower — and if lower, what number should the interest design and the bake's anchor density be sized against?

## Appendix C — C++ & DirectX review

### Verdict
From the engine-programming lens this baseline is structurally right for scale: the two-thread/transport-only model, the Extract seam, the SoA world behind the replay gate, and the fixed pass list with measured insertion cost are exactly the shapes that let players, entities, and features grow without rework. The gaps are not architecture but unwritten contracts and unrun instruments: the multi-grid phase (U2) is about to be built with no tick-cost measurement, no grid-count budget, and no recorded contract making world-level parallelism the drop-in it could be; and the toolchain is a single-config monoculture (Debug-only CI, unpinned "latest" compiler, a latent Debug/Release shader-compiler fork) whose numbers will drive structural decisions in the universe phase. Fix those at design level now — all are cheap — and the baseline holds.

### Sound for scale — do not churn
- **Transport-only crossing + single-writer worlds** — process split stays a packaging change by construction; verified: no other channel exists (ADR-007 §5–6; ServerHost.cpp:337–404; ClientApp.cpp:273–294).
- **Extract as the future Game/Render thread seam** — `RenderScene` is plain, device-free, pooled data filled through `WorldView::BuildScene` (RenderWorld.h:97–118, ClientApp.cpp:618–646; Dependency-Map "the future Game/Render thread seam"); a thread split is double-buffering, not redesign.
- **Fixed pass list with reserved slots, insertion cost measured** — Nebula cost one struct, one line, one PSO (ADR-006 §1a; GpuPasses.h:10–32, 188–222); GpuCull/DepthPre/Effects/Tonemap remain insertions.
- **Same-binary determinism scope + quantised-input validation parity** (ADR-005 §4, §6) — deliberately avoids the cross-build bit-exactness a server farm would otherwise owe; client/server verdict parity survives differing binaries because validation is integer-domain.
- **Bounded, drop-and-count crossings and allocation-free steady state** — SPSC/MPSC rings with drop counters (RingBuffer.h:38–50), upload ring that refuses growth (GpuUploadRing.h:21–24), pooled draw lists (UiDrawList.h:27–31); backpressure is visible, never blocking.
- **Engine/game inversion enforced by CI, seam already multi-client-shaped** — build.yml:144–176; `ApplyOrderBytes` carries `clientId`, `SnapshotSender.h` reserved for the per-client/per-grid emit path (Simulation.h:62–66; Dependency-Map NeuronServer table).

### Findings

#### CPP-1 — Multi-grid tick: no budget, no measurement, and the world-parallel contract is unwritten
- **Severity:** High
- **Refs:** ADR-016 §4; Universe-Build-Order U2/U3a accepts (determinism only — no cost measure); ADR-007 §4 ("first parallel consumer… must bring the deterministic partitioning story"); ADR-005 §5; Risk-Register R10 (1,024-entity soak "cheap, scriptable" — never run); Architecture-Overview ("the tick must fit 50 ms with 1,024 entities" — written for one world); World.cpp:154 and 901–909 (Steering avoidance and `Separate` are O(n²), 4 relaxation passes); ADR-007 §7 vs grep: `NEURON_ASSERT_OWNER` exists in no source file.
- **Design position:** One Sim thread runs the whole server; U2 puts a registry of M `World`s behind `Simulation::AdvanceTick`; transfers apply between ticks in fixed order; R10's mitigation is "sim is SoA + single-writer, parallelisable behind the replay gate."
- **Scaling risk:** Axis B, at U2–U4. Per-tick cost is M × O(n²) with both factors unbudgeted: nobody has measured one grid at 1,024 entities (in any config), ADR-016 caps neither concurrent grids nor per-grid population, and no U-slice measures tick cost. The between-ticks transfer bus makes worlds share-nothing *in principle* — but nothing obliges U2's registry to preserve that (a shared scratch buffer, cross-world iteration during tick, or an allocator shared across worlds would each be natural single-thread code), and the owner-assert machinery ADR-007 §7 claims ("mechanical, not aspirational") was never built, so nothing would catch it.
- **Why structural:** If U2 lands share-nothing with a stated budget, parallelising across worlds later is a TaskPool fan-out inside `AdvanceTick` behind the replay gate — a slot that drops in, invisible to the engine seam. If U2 lands without the contract, T1–T3 build the transfer seam on top of it and the parallel retrofit reopens both phases plus the replay harness. Writing three sentences into an ADR now versus re-plumbing the universe runtime after the station phase is the asymmetry.
- **Recommendation:** Amend ADR-016 §4 (or ADR-007 §4) to (a) record world-level fan-out as the pre-approved first parallel consumer, with its constraints: worlds share nothing during `Tick`, transfers apply single-threaded between ticks, per-world telemetry rides worker lanes; (b) make "no cross-world state touched during Tick" a U2 acceptance item and build the deferred owner-assert as its rail; (c) state the budget — target concurrent grids × entities the 50 ms must hold — and run R10's 1,024-entity soak (Release) before U2 shapes the registry, since its result decides whether a broadphase lands inside GameLogic first.
- **Confidence:** High

#### CPP-2 — Device-removed handling has no owner, and remote clients make it mandatory
- **Severity:** Medium
- **Refs:** AGENTS.md:278 ("Present logs and carries on until device-removed handling exists"); GpuSwapChain.cpp:264–278 (log-and-continue); Main.cpp:694 (composition-root catch → message box → exit); GlyphAtlas.h / GpuMeshes.h (no CPU-side retention after upload; both `Create`/`Destroy` re-runnable); ClientApp.h:89–90, 128–153 (single ordered device-init path); not in Risk-Register, no slice in any build order.
- **Design position:** Explicitly deferred, with a designed failure today: any post-removal `check_hresult` throws, is caught once at the root, and the process exits cleanly. No document says when the deferral ends or what the recovery is.
- **Scaling risk:** Axis A. On loopback with the owner's machine, exit-with-message-box is fine. The first remote/shipped client meets TDRs, driver updates, and iGPU/dGPU switches as routine events; today each one kills the session client-side (the server survives, so the floor is relaunch-and-reconnect).
- **Why structural:** The retrofit is currently *contained* precisely because the codebase keeps a clean device-free/device-owning split — every device object has a Create/Destroy pair, session state (connection, snapshots, camera, selection) holds no GPU references, and assets rebuild from disk/DWrite via the restartable TaskPool. That containment is a property nothing enforces; each new device resource added across two more phases can erode it, and retrofitting after erosion touches every pass input, the upload ring's persistent map, the atlas, and the swapchain's waitable machinery at once.
- **Recommendation:** Take the decision now, as an ADR-006 amendment plus a risk row: name the recovery model (minimum: detect `DXGI_ERROR_DEVICE_REMOVED/RESET` at Present and fence waits → destroy the device half → re-run the device section of `Initialise` → re-upload static content, session state surviving by construction — or explicitly accept relaunch+reconnect as shipped behaviour), and make "no session state may hold a device reference" the recorded invariant that keeps the retrofit cheap. Gate the slice on the first remote-client milestone.
- **Confidence:** High

#### CPP-3 — Single-config CI on an unpinned toolchain, with a latent Debug/Release shader-compiler fork
- **Severity:** Medium
- **Refs:** build.yml:3–7 (Debug|x64 only, by design), build.yml:17, 22–23, 294–296 (`windows-latest`, `setup-msbuild`, `vswhere -latest` — toolset floats with the CI image); AGENTS.md §5 (`/std:c++latest`); Risk-Register standing spike 2 (Debug/Release replay comparison — never run); R17 (~1 s **Debug** threshold triggers the per-region content split); Universe-Build-Order U5 accept ("full 2,500 render inside the frame budget"); Outpost.vcxproj:41/53/… (per-file `ShaderModel 6.7` **Debug-only** → dxc/DXIL) vs :192, 218 (project default `5.1` → fxc/DXBC, i.e. the never-built Release path) vs ADR-013 §1a / ADR-006 §12 / MVP-Build-Order S7a, which all say `fxc`, SM 5.1.
- **Design position:** "Add Release when optimised-only breakage becomes a real risk" (AGENTS.md §6); CI is "the only compiler this work has" (Design/README.md); determinism is same-binary by scope, so config divergence is documented as acceptable — but never observed.
- **Scaling risk:** Axes C and E, starting at U1. The universe phase crosses AGENTS.md's own line three ways: perf-gated acceptance criteria (R17's parse threshold, U5's frame budget, CPP-1's tick budget) will be measured in a config that ships nothing — Debug numbers are 5–20× off, and R17's fallback is a *content-pipeline restructure* triggered by them; a dedicated-server farm ships Release binaries CI has never compiled; and the shader build already forks per config — Debug compiles SM 6.7 through dxc while Release would compile SM 5.1 through fxc with warnings-as-errors, a path that has never once run and that the design documents do not describe. Separately, `c++latest` on a floating `windows-latest` toolset means a VS image update can break every contributor with zero repo change.
- **Why structural:** None of this is expensive to fix; all of it is expensive to discover late — a structural content-split adopted (or wrongly skipped) on Debug numbers, or a first Release build during the dedicated-server push that fails in shaders and NDEBUG-only wire paths simultaneously.
- **Recommendation:** Three decisions at U1: (1) add a Release|x64 compile + `selfTest` leg to CI and run spike 2 once, recording that the hashes differ by design; (2) state in the build orders which config every perf acceptance number is measured in (Release, with the Debug ratio noted); (3) resolve the shader fork deliberately — either commit both configs to dxc/SM 6.x (and amend ADR-006 §12/ADR-013 §1a, noting the reserved GpuCull slot will want SM6 anyway) or restore 5.1, but stop shipping a per-config compiler split nobody decided. Pin the CI toolset version and record the upgrade cadence.
- **Confidence:** High

#### CPP-4 — Solution-wide toolchain invariants exist only as per-project copies; the pins the determinism story cites are not pinned
- **Severity:** Medium
- **Refs:** AGENTS.md §5 ("`/arch` stays uniform across the solution"), R11 ("`/arch` pinned solution-wide"), World.h:31 ("float32 throughout at `/fp:precise`") — versus grep: no `FloatingPointModel` or `EnableEnhancedInstructionSet` element in any `.vcxproj` (both are only MSVC defaults), no `Directory.Build.props` in the repo, and 10 projects each hand-copying `PlatformToolset`/`LanguageStandard`/`ConformanceMode` blocks; AGENTS.md §1 ("Nothing runs either automatically yet" — `.clang-tidy` has no CI step); build.yml:165, 218 (guard lists of GameLogic headers and tick-code files are hand-spelled, not derived from the tree).
- **Design position:** The pins are normative in prose; enforcement is review. The CI guard suite is excellent but its file lists are maintained by hand, and R14's registry discipline already lives in two hand-maintained doc tables.
- **Scaling risk:** Axis E, plus the determinism axis it protects. At 10× projects and multiple contributors, one project quietly gaining `/arch:AVX2` or `/fp:fast` (a plausible "make Release faster" edit) violates the uniformity R11 depends on with no diagnostic; a new GameLogic header (U1's generator, T1's transfer bus) lands outside build.yml's hand-spelled grep lists and is silently unguarded — R16 already relies on those lists being extended by hand.
- **Why structural:** This repo's own history (R14 realised twice, the `INVALID_ENTITY_ID` C2371) shows that conventions here survive only when a machine holds them. The fix is one MSBuild-native file plus deriving guard lists from the tree; the failure mode it prevents is a determinism or codegen divergence found months later via an unexplained red replay on one machine.
- **Recommendation:** Adopt a common props file (compatible with ADR-013 — it changes no layout) that states `PlatformToolset`, `LanguageStandard`, `ConformanceMode`, explicit `/fp:precise`, and the absence of `/arch` overrides once, or add a CI guard failing any `.vcxproj` that overrides them; derive build.yml's GameLogic-header and tick-code lists from the tree (`Get-ChildItem GameLogic -Filter *.h`) instead of hand lists; add the clang-tidy CI step AGENTS.md §1 already anticipates before the second contributor arrives.
- **Confidence:** High

#### CPP-5 — The renderer's scale ceilings are compile-time constants sized for the MVP HUD, and the instruments that would size them are unrun
- **Severity:** Medium
- **Refs:** Risk-Register standing spike 3 (1,024-instance draw — never run; "the test is a scenario rather than a code change"); ClientApp.cpp:35 (`UPLOAD_BYTES_PER_FRAME = 256 KiB`), :199; UiDrawList.h:157 (`UiInstance` = 48 B); GpuPasses.cpp:302, 362 (upload-ring exhaustion drops the **entire** HUD for the frame, counted); Universe-Build-Order U5 accept ("the full 2,500 render inside the frame budget with the `Ui` span proving it"); ADR-016 §9 (strategic map is Ui-pass quads/segments/text).
- **Design position:** The upload ring deliberately refuses to grow ("a frame that asks for more than its segment holds is a frame with a bug in it" — GpuUploadRing.h:21–24), sized once at 256 KiB/frame; the pass architecture (instancing, one queue, 5 PSOs) is asserted adequate for the corpus's 1,024-entity cap but the spike validating that has needed only a GPU and a scenario since S5.
- **Scaling risk:** Axes B/C, at U5. Arithmetic available today: ~2,500 node quads + ~3,000 gate-link segments + ~15,000 label glyphs ≈ 20k `UiInstance` × 48 B ≈ 1 MB — four times the whole per-frame segment, before frame constants, overlay marks and instance streams. U5's acceptance as written will render a blank HUD (the exhaustion branch drops everything, by design) until someone bumps a constant mid-slice. The same class of unmeasured number sits under spike 3 (1,024 instances × up to ~1,800 faces through the opaque pass) and S5's still-open "frame time < 2 ms at 41 instances" check.
- **Why structural:** The mechanism is right — a linear per-frame ring scales to megabytes without redesign — so this is not rework risk but *decision* debt: budgets sized from MVP content rather than from the corpus caps the design already commits to (1,024 entities, 2,500 systems), and validation slices that will fire against tuning constants instead of architecture, muddying what U5/spike results actually measure.
- **Recommendation:** Record the sizing rule now (upload ring, and any future fixed GPU budget, is sized from the corpus caps — 1,024 entities, 2,500-node map — not from current content; make it config alongside `client.renderer.msaa` if retuning without rebuild is wanted); run spike 3 and the S5 frame-time check before U5 starts so the map slice measures the map, not the MVP's constants.
- **Confidence:** High

### Questions for the owner
1. **Concurrency targets for the budget in CPP-1:** at the horizon the universe phase must hold for, how many concurrent live grids per session, and how many entities per grid, should one server process sustain at 20 Hz? (A single pair of numbers turns the 50 ms budget from a slogan into an acceptance criterion for U2.)
2. **Which build configuration are the phase acceptance numbers measured in** — R17's ~1 s parse threshold, U5's frame budget, the tick budget? If Debug (CI's only config), is a structural fallback like the per-region file split really meant to trigger on Debug timings?
3. **Is the Debug shader build's SM 6.7/dxc setting (Outpost.vcxproj per-file overrides) a deliberate decision superseding S7a's fxc/SM 5.1, or drift?** The docs and the never-built Release path still say fxc/5.1; the answer decides which way CPP-3's reconciliation goes and matters for the GpuCull reserved slot.
4. **Is Windows 11 / Server 2022+ the recorded platform floor for shipped clients and servers**, or provisional pending the msquic OpenSSL-flavour sign-off flagged in Dependency-Map §7? (Determines whether R3's OS dependency is a product decision or an accident of the Schannel package.)
5. **For the first remote-client milestone, is relaunch-and-reconnect acceptable shipped behaviour for device removal**, or is mid-session recovery owed? (Sets CPP-2's scope: a risk row and an invariant, versus a scheduled slice.)

## Appendix D — UI architecture review

### Verdict

The MVP's UI layer is exactly what R9 prescribed and it held: pooled quads + text runs, a zone table, game-fed strings, one screen, no widgets — and the discipline around it (device-free HUD tests, layout-and-hit-test-in-one-place, seam-fed vocabulary) is the right substrate to scale on. But the designed roadmap crosses a category boundary the R9 fence was never built for: U5 + U6 + T3 + settings are three-to-five full-screen *surfaces* with navigation, text input, dropdowns, scrolling lists, and per-screen state — and no ADR owns what a screen mechanically *is*, how input routes to one, or how screen-scale data crosses the ADR-014 seam. Nothing in the baseline is wrong; what is missing is one UI-architecture ADR's worth of decisions, and the window to take them is now, because U5 is an open starting point today (Design/README.md:104–109) and T3's print prerequisite is already delivered (ADR-017 Consequences, "P1 delivered 2026-08-19"). Taken now, those decisions are cheap; taken after three screens hand-roll three answers, convergence is rework across shipped surfaces.

### Sound for scale — do not churn

- **Text stays text until the pass; the HUD's words are asserted device-free** — this is what lets 263 client tests cover every future screen's content (NeuronClient/UiDrawList.h:9–31; ADR-006 §10a; MVP-Build-Order.md:960–965).
- **Layout and hit-test in one place, chrome gets first refusal on the pointer** — the one convention that generalises to every widget (NeuronClient/CommandRow.h:22–29; ADR-006 §10b; ClientApp.cpp:361–387, 439–445).
- **One Ui instance stream, oriented quad added as a class not a feature** — the pass vocabulary already expresses the map's links, lanes and arcs with no new GPU work (ADR-006 §8c; UiDrawList.h:98–111, 142–158; R9's own close-out, Risk-Register.md:23).
- **The seam feeds all HUD vocabulary as data** — kinds, options, roster rows, reason text; the two-game-engine property survived six method additions and is the proven pattern for U/T screens (NeuronClient/WorldView.h; ADR-014 §2c; Dependency-Map.md:116).
- **UTF-8 at the text API with `char32_t` atlas keys and U+FFFD fallback** — the encoding half of the text-at-scale problem is already decided correctly (UiDrawList.h:176–189; GlyphAtlas.h:92; GlyphAtlas.cpp:27–47).
- **Pixel-based zones × a scale multiplier with an enforced 48 px touch floor, per-monitor-v2 DPI declared** — the right base for a resolution model (UiLayout.h:20–27; CommandRow.h:76–83; Window.cpp:37–40).

### Findings

#### UI-1 — No screen/surface model: navigation, input routing, and per-surface state are undecided while four surfaces are designed and two are about to build

- **Severity:** High
- **Refs:** ClientApp.h:88–107 (the frame is a fixed single-surface sequence: `UpdateHud → UpdateSelection → UpdateOrders → ExtractScene → BuildHud`); ClientApp.cpp:361–455 (routing is hand-ordered zone tests, no consumed-event concept); ADR-016 §7 (roster click switches view; auto-follow; "the map is the between-surface"), §9 (three surfaces); ADR-017 §6 ("a full-screen surface in the TACTICAL ⇄ MAP family"); Universe-Build-Order.md:103–126 (U5/U6); Station-Build-Order.md:68–81 (T3); station-screen.png §3 OPEN ("Does the composer persist? … clears on undock, survives navigation within a session"); ADR-017 Consequences ("print proposals awaiting review: … composer persistence"); debug-hud.png §1/§5 (the corpus's budget model: "every other screen replaces the one before it", per-screen Ui budget rows); settings.png ("◀ BACK | SETTINGS"); session-surfaces.png §1 (a six-screen pre-session flow); HudRoster.h:54–62 and ClientApp.cpp:916–917 (scrolling explicitly deferred as "a surface").
- **Design position:** ADR-016/017 *name* the surfaces and their handoffs (TACTICAL ⇄ MAP ⇄ STATION, ◀ BACK) and the prints draw them, but no document decides the mechanics: what owns the active-surface state, how input routes when a surface is up, which render passes run beneath a full-screen surface, which layers cross surfaces (toasts, the critical slot, the debug strip — debug-hud.png makes the strip the *only* composing surface), what per-surface widget state persists across navigation, or how a list scrolls. R9's fence ("layouts hardcoded to print zones… no widgets") governed primitives *within one screen* and closed successfully on those terms.
- **Scaling risk:** Axis C/E, immediate. U5 and T3 can start now and are deliberately parallelisable with the sim track (Universe-Build-Order.md:153–155), so they are exactly the slices likely to be built by different hands. Before either, U3b's "roster click switches view" and T2's "STATION button" make the roster interactive — the first click-consuming widget outside the command row, forcing the routing question mid-phase. The state-retention question is already being answered ad hoc in a print proposal with no framework to receive the answer. Session surfaces (axis A/D) later add screens that must exist *before* a WorldView has data, which rules out "screens as HUD modes" retrofits.
- **Why structural:** Each screen that ships pre-decision embeds its own answer to routing precedence, pass suspension, state lifetime, and scrolling; converging three shipped, print-checkpointed surfaces onto one model later means touching all of them plus their device-free test suites — the classic framework-retrofit cost R9 was registered to avoid, arriving one level up from where R9 watched. Taken now, the decision is one short ADR and near-zero code, because the corpus has already made the load-bearing calls (screens are mutually exclusive; one composing overlay; per-screen Ui budget rows; toasts never over the context bar).
- **Recommendation:** Write the UI-architecture ADR before U5/T3 start. Minimal content: (1) a client-owned active-surface enum/stack with declared entry/exit and which passes each surface runs (a full-screen map is Ui-pass-only over Clear — insertion-free in the fixed pass list); (2) input routing as ordered consumption — active surface → cross-surface layers → world — replacing per-consumer zone re-tests; (3) per-surface retained-state structs with declared lifetime (answering the composer question as a rule, not a per-print proposal); (4) the widget conventions generalised from CommandRow: laid out and hit-tested in one place, build order = draw order = z, device-free; (5) one scrolling-list primitive (offset + clip + wheel routing) shared by roster, route panel, and any hangar overflow. Explicitly *not* a widget framework: the fence moves up one level, it does not fall.
- **Confidence:** High

#### UI-2 — Text input does not exist and no seam is reserved for it: no WM_CHAR/IME path, no focus concept, no editable-field state, while search, renames, and binding capture are designed

- **Severity:** High
- **Refs:** Window.cpp:288–420 (handled messages: mouse, size, focus, VK key up/down only — no WM_CHAR, WM_UNICHAR, WM_IME_*); InputMap.h:57–77 (the entire key vocabulary is 14 hardcoded logical actions); Universe-Build-Order.md:106–108 (U5 ships "search"); strategic-map.png §1 (the search box); Station-Build-Order.md:72–74 (T3: "new wing" names and renames persisted to the user settings layer) and :73 ("keybinding in the settings screen"); ADR-016 §7 ("bindings live in the settings screen"); ADR-017 §6 ("name a wing" is the player experience); session-surfaces.png §2 (email/passphrase fields, commander-name field with live availability check); settings.png §3 OPEN ("Is there a keybind surface at all?").
- **Design position:** Nothing. The prints assume typing (search, renames, credentials); ADR-016/T3 assume a settings screen holding bindings; no ADR, build-order line, or code path reserves how a character reaches a string, how an editable field takes keyboard focus away from camera bindings ("W" must pan *or* type, never both), or how a binding-capture control swallows the next key.
- **Scaling risk:** Axis C first (U5's search is the first typed field), then T3 (renames — persisted player identity), then axis A/D (session surfaces make text entry the *first* thing a player ever does). Each surface that arrives without the machinery either hand-rolls a keyboard path or silently drops the print's feature (a search-less 2,500-system map fails its own purpose).
- **Why structural:** Text input is the one widget that cannot be built locally inside a screen: it reaches into Window (new message handling, IME posture), into the routing chain (a focused field must *preempt* every global key, which requires the consumption model of UI-1 to exist), and into the encoding/atlas policy (UI-3). Retrofitting focus-preemption after screens ship means revisiting every existing key consumer; the current InputFrame model has no place to even express "this key was eaten". Reserved now, it is: one focus-owner slot, one `TextEditState` (buffer/caret/selection), a WM_CHAR→UTF-8 path in Window, and a routing rule — all device-free and testable in the house style.
- **Recommendation:** Fold into the UI-1 ADR as its input-routing half: decide the focus model (at most one focused editable; keyboard order: focused widget → surface → global), the minimal edit-state machinery, Window's WM_CHAR/IME posture (IME can be explicitly deferred — but *say so*), and whether binding capture is in or out of the settings screen's first slice. Land it before U5's search field is written.
- **Confidence:** High

#### UI-5 — Screen-scale data across the WorldView seam: the method-by-method pattern is proven for panel rows but its shape for whole screens — and where screen composition itself lives — is undecided

- **Severity:** Medium
- **Refs:** WorldView.h (12 virtuals, grown one question at a time; ADR-014 §2c records each); ADR-014 Consequences ("watch for leakage… the fifth-project question reopens") and Alternatives ("a fifth 'GameClient' project… revisit only if the WorldView seam starts leaking game shapes"); ADR-016 §6 (fleet summaries), §8 (client-side Dijkstra route planner, avoid-list in user settings), §9 (map/system-view content: names, security bands, sovereignty stubs); ADR-017 §8 (`StationRoster` ~1 Hz), §6 (hangar tabs REPAIR/REFIT/CONSTRUCT/MARKET with disabled reasons); strategic-map.png (F15: complete client-held universe copy; "the band is a badge, the number is per-system"); Dependency-Map.md:127–147 (the exe adapter is where universe data and both halves meet); U5 accept ("the full 2,500 render inside the frame budget with the `Ui` span proving it").
- **Design position:** ADR-016 decides *what* the client holds (full baked topology, summaries, client-fed routes) and ADR-014 decides *who may know what* (NeuronClient: nothing game-shaped). Neither decides how 2,500 systems of static topology, search, route solving, or a wing-grouped hangar cross the seam — and ADR-014's revisit trigger for the fifth-project question is reactive ("if the seam starts leaking"), i.e. it fires only after code exists.
- **Scaling risk:** Axis C/E. Per-frame span-fill calls (the BuildRoster shape) are wrong for a static 2,500-node graph; one-method-per-question across three screens plausibly triples the interface (every method × NullWorldView × test double × exe adapter). The worse branch is semantic leakage: a map screen is 90 % game meaning (security colour rules, sovereignty owners, station-service tab names), and S11d proved the seam is held by judgement, not CI ("a string literal is neither" an include — ADR-014 §2c). Screens composed inside NeuronClient also erode the two-game property even with clean data: Warzone wants none of Frontier's hangar.
- **Why structural:** If the answer emerges by accretion, the endpoints are either engine-embedded Frontier screens (relocating them later moves files, tests, include entitlements, and both ADR-013 registries) or an interface wide enough that reshaping it touches engine + exe + game + four test suites. Decided now, it is a one-page ADR-014 amendment following existing precedent: static content crosses **once, at boot, as a neutral graph** (nodes/edges/labels/badges — the asked-once pattern `OrderKinds` already set); live per-node data crosses as summary-keyed neutral rows at summary rate; search and route-solve are GameLogic pure functions reached through seam calls (the `SolveFormation`/`ValidateOrder` precedent, which the client-side Dijkstra *must* follow anyway since NeuronClient cannot know the gate graph's meaning).
- **Recommendation:** Amend ADR-014 (or include in the UI ADR): the screen-data contract above, plus an explicit proactive answer to the fifth-project question for screens — either "screens are engine surfaces, data-fed, and the seam is allowed to grow to N-per-screen asked-once builders" or "U5 opens the game-side presentation home". Extend the leak test in words: no security/sovereignty/service semantics in engine code; labels, badge classes and colours arrive as data.
- **Confidence:** Medium

#### UI-3 — Player-authored text meets a boot-baked ASCII+markers atlas, and the i18n posture has no owner or trigger

- **Severity:** Medium
- **Refs:** GlyphAtlas.cpp:27–47 (baked set: 95 printable ASCII + box/block/marker glyphs, a hardcoded boot list); GlyphAtlas.h:91–92 (missing glyph → null, "the caller substitutes"); UiDrawList.h:176–189 (UTF-8 decode to U+FFFD — the pipeline is ready, the coverage is not); ADR-006 §9 and its closing consequence ("no shaping/i18n in MVP — accepted; revisit at localisation" — no scheduled trigger); ADR-017 §6 + Station-Build-Order.md:72–81 (wing renames persisted to the user settings layer, surviving restart — T3's own acceptance); Universe-Build-Order.md:108 (search); ADR-012 (user layer is the one file the game writes).
- **Design position:** The corpus's authored text is ASCII by construction (`ROOT-N` naming, ADR-016 §1 — a deliberate, good decision). Player-authored text has no stated charset: a wing named in Cyrillic or with an emoji renders as U+FFFD boxes *forever*, persisted in Settings.json, and search must define matching semantics over whatever it accepts. "Revisit at localisation" names a decision point with no date, while T3 and U5 build string paths before it.
- **Scaling risk:** Axis C at T3/U5 (first persisted player strings, first free-text query); axis A/D at session surfaces (account email, commander name — names checked server-side for availability, i.e. a charset policy becomes a *protocol* fact); axis E (every string path built pre-decision embeds an assumption someone later audits).
- **Why structural:** The expensive half is already right (UTF-8 API, `char32_t` keys, bake-list-as-data means coverage growth is content, not architecture). What remains is a policy that is nearly free now and compounding later: once names are persisted and searchable, changing the accepted charset means migrating stored settings, revalidating on load, and reconciling server-side name rules. This is the classic "decide at the boundary before data exists" case.
- **Recommendation:** One paragraph, amending ADR-006 §9 (or in the UI ADR): (1) editable fields accept only codepoints the atlas bakes, validated at the input widget *and* on user-layer load (fail-soft to substitution); (2) the bake list is the single source of that set; (3) i18n/localisation is declared post-roadmap and its trigger named (the localisation decision reopens ADR-006 §9 with per-locale bake lists and a shaping call — the D2D-interop rejection may be re-argued then, not before); (4) session-surface identity fields inherit the same policy until the accounts design says otherwise.
- **Confidence:** High

#### UI-4 — The resolution/DPI/aspect envelope is undocumented: one 0.8–1.6× knob carries every display, with no DPI→scale rule, no WM_DPICHANGED response, and no design minimum

- **Severity:** Medium
- **Refs:** Window.cpp:37–40 (per-monitor-v2 declared — real pixels, no bitmap scaling); Window.cpp:312–318 (min track 320×240 is a swapchain guard, not a design floor); no WM_DPICHANGED case (Window.cpp:288–420); UiLayout.h:32–68 (zone constants are px @ 1.0, print-derived); UiLayout.cpp/ClientApp.cpp:361 (scale comes from config only — ClientConfig.h:63–66); CommandRow.h:104–113 (narrow rows *drop verbs*); ADR-006 §10 ("UI scale multiplies pixels… clamps to the settings sheet's range"); settings.png §1 ("re-lays out the interface rather than scaling a bitmap", scale 0.8–1.6×, 48 px floor "enforced, not scaled"); prints authored 1440×900 (strategic-map.png §1, station-screen.png §1, session-surfaces "shown at 46% — landscape tablet").
- **Design position:** Pixel-zones-times-scale is decided and correct; per-monitor-v2 is in the tree. Undecided: what scale a player *starts* at on a 150 %/200 % display (default 1.0× = illegibly small chrome until they find a slider whose ceiling, 1.6×, is below 200 % DPI parity), what happens when the window crosses monitors (nothing — no WM_DPICHANGED), and the minimum client area at which each surface's print layout is owed (at 1280×720×1.6× the tactical chrome already eats most of the world rect; the map and hangar prints assume ~1440×900 proportions).
- **Scaling risk:** Axis A/E. One dev machine hides this; the first remote client on a 4K laptop meets it. Three new full-screen surfaces are about to hardcode zone tables against the same undocumented envelope — the command row's drop-verbs answer is right for a border-chrome HUD but has no stated equivalent for a full-screen composer or a 24-jump route panel.
- **Why structural:** Cheap now — three sentences and one multiplication (effective scale = DPI factor × user preference, floor/ceiling applied to the *product*; a stated minimum client area per surface; DPI-change = re-resolve zones, which `ResolveUiLayout` already supports per-frame). Late, it is a per-screen re-layout audit across every zone table shipped in between, and possibly a re-negotiation of print checkpoints ("visual checkpoint against the print" is an acceptance criterion at U5/T3 — against which resolution?).
- **Recommendation:** Amend ADR-006 §10: define effective scale (DPI-derived default × user 0.8–1.6× preference), handle WM_DPICHANGED as a resize-plus-rescale, and document the supported envelope (proposal: min client area 1280×720 at effective 1.0×; prints normative at 1440×900; per-surface overflow behaviour named — drop, scroll, or letterbox). State it before U5/T3 write their zone tables.
- **Confidence:** High

#### UI-6 — Small reserved-seam gaps the designed slices will hit: actionless toasts, boot-frozen palette, and a user-settings write path about to outgrow its charter

- **Severity:** Low
- **Refs:** ToastStack.h:81–111 (a `Toast` carries priority/source/text only — no action or target payload); Universe-Build-Order.md:117–126 (U6 accept: "a toast click jumps focus across systems"); alerts-and-toasts.png §2 (critical carries a JUMP TO action; "3 UNREAD" counter has no machinery); HudPalette.h:8–15 + ClientApp.h:226 (palette resolved once at boot; "swapping the table has to be a config edit") vs settings.png ("CHANGES APPLY IMMEDIATELY", live preview pane, live contrast audit); ADR-012 §3 (user layer "carries exactly the keys the settings screen owns… anything else is ignored with a warning") vs T3 (wing names) and ADR-016 §8 (route avoid-list) both assigning new data to that layer.
- **Design position:** Each is designed *around* but not *into*: toasts are complete as displays, the palette is runtime-selectable by name only at boot, the user layer's schema sentence predates the two phases that extend it.
- **Scaling risk:** Axis C/D, at U6 (clickable toasts), settings' first slice (runtime palette + preview compositing — which also needs UI-1's answer to "what renders beneath a surface"), and T1/T3 (first non-settings-screen keys written to Settings.json, colliding with "ignored with a warning" on older builds).
- **Why structural:** None individually — all additive (an opaque action id + target on `Toast`, mirroring `sourceKey`; a palette re-resolve on change; one sentence widening ADR-012 §3 with a versioning note for unknown keys). Recorded so each is a decision in its slice rather than a discovery, and so the toast-action payload is designed once game-side (the target of "jump focus" is a grid/anchor the engine must carry opaquely — the `groupId` pattern again).
- **Recommendation:** Note all three in the UI ADR's appendix or the respective build-order slices: toast action payloads (engine carries, game interprets), palette as re-resolvable state, ADR-012 §3 amended to enumerate the user layer's growing families (settings, wing names, avoid-list) with unknown-key tolerance stated as forward-compat, not error.
- **Confidence:** High

### Questions for the owner

1. **Keybinding:** ADR-016 §7 and Station-Build-Order T3 both say bindings live in the settings screen, but settings.png §3 lists "is there a keybind surface at all?" as open. Which statement governs? (Decides whether binding-capture machinery goes into the UI ADR now or is explicitly out of the U/T roadmap.)
2. **Touch and pad:** the prints repeatedly assume "mouse, keyboard, pad and touch drive one grammar" and enforce 48 px touch floors, but the client handles only Win32 mouse/keyboard. Is touch/pad input on any planned horizon (i.e. must the input-routing decision reserve a pointer abstraction beyond the mouse), or is 48 px purely target-size discipline for now?
3. **Resolution envelope:** what is the minimum supported client area, and should UI scale acquire a DPI-derived default (with 0.8–1.6× as the user preference on top), given the 1.6× ceiling is below 200 %-DPI parity? Are the prints' 1440×900 proportions the design minimum for the full-screen surfaces?
4. **Player-text charset / i18n:** may I record "editable text accepts only the atlas's baked set, localisation is post-roadmap with a named reopen of ADR-006 §9" as the standing posture — or do you want player-authored text to accept broader Unicode from T3 onward (which changes the atlas plan now)?
5. **Where screens live:** when U5/T3 build, do the strategic map and hangar follow the HUD precedent — engine-drawn surfaces fed neutral data through a wider WorldView — accepting that NeuronClient accumulates Frontier-shaped screen layouts, or is this the moment ADR-014's deferred "fifth project" (a game-side presentation home) gets decided? U5's builder needs this answer on day one.

## Appendix E — Game logic review

### Verdict
The single-world core is genuinely scale-shaped: dense SoA tables with id↔slot indirection, determinism bought by construction and enforced by CI, a widen-by-append schema discipline already proven twice (gauges, statusBits pricing), and a multi-world decomposition (between-ticks transfer bus, untouched `World`) that makes deterministic parallelism reachable rather than hoped-for. The structural risk is concentrated exactly where the next two phases are about to build: the universe layer's bookkeeping is under-designed — identity allocation across worlds collides with the design's own promises, the transfer bus's ordering key is specified two different ways and neither is total, and teardown/recreate quietly makes viewer behavior a sim input. All are one-paragraph ADR amendments today and log-format/replay-golden migrations later; U2/T1 being unstarted is precisely why they should be decided now. Two governance gaps (per-grid population vs. replication capacity; behavior parity across build skew) need owners rather than mechanisms.

### Sound for scale — do not churn
- **Fixed-schema SoA + id↔slot indirection, widen-by-append evolution** — gauges already exist in tables *and* on the wire (`World.h:370-371`, `EntityRecord.h:57-58`), and every added field is priced in ships (`Snapshot.h:130-142`, ADR-017 §5); "add array + EmitSnapshot + schema string" is a sound long-term mechanism under a closed taxonomy.
- **Determinism by construction, enforced not asserted** — bit-pattern `WorldHash` with RNG-in-state folded last (`WorldHash.cpp:27-31,134-135`), CI bans on `<chrono>`/`<random>`/`XM*Est` (Risk R2/R11), dense-order-only iteration; the desync-tooling foundation is right at any scale.
- **The multi-world decomposition** — `World` untouched, transfers applied between ticks, per-anchor PCG32, `UniversePos`-in-tick-code CI guard (ADR-016 §4, R8): worlds are tick-independent by construction, so across-world parallelism is genuinely reachable behind the replay gate, and spatial partitioning is a contained drop-in behind two pure functions (`SteerAroundTraffic`, `Separate` — `World.cpp:124,870`).
- **Anchors-only warp + baked, hash-guarded universe + client-fed routes** (ADR-016 §§2-3,8) — bounds grid count, keeps the order schema narrow (no server-side route state), and puts axis-C content under the existing fail-closed `contentHash`.
- **Emergent fleets and wings, rosters as universe-layer durable records** (ADR-016 §7, ADR-017 §§1,6) — zero new hashed state per social structure, and the roster is the correct persistence-shaped precedent for durable off-grid state.
- **Reserved-value and contract discipline** — `OrderKind`/`OrderReason`/hull appends, `ValidationView` as the two-sided intersection (`Validate.h:38-53`), the pinned check-order + parity matrix (ADR-005 §4a, ADR-017 §8): entity-targeted orders later fit the members-are-ids precedent (`OrderGroup` at `World.h:100-129`) without a leg-schema rework.

### Findings

#### SIM-1 — Session-scoped identity is undesigned for the multi-world runtime
- **Severity:** High
- **Refs:** `World.cpp:250-257` (`m_nextShipId` allocated per `World`, from 0), `World.cpp:220-242` (`Reset` clears it), `World.h:379` + `World.cpp:410-433` (`m_nextOrderId` per world), `Ids.h:44-47` ("order ids are never reused within a session"); ADR-016 §4 ("`World` itself is untouched"); ADR-017 §1 ("Ids persist through docking… every log, order, and roster row means the same ship"), §8 (`StationRoster` carries `shipId u16`); Station-Build-Order T1 ("transfer-in spawning **with** a given id"); Universe-Build-Order U2 (spin-up "spawns the anchor's authored occupants").
- **Design position:** The receiving half of identity is designed (transfer-in with a given id); the allocating half is not. Each `World` self-allocates ship ids from 0 and order ids from 1; no document names a session-level allocator, and U2's acceptance never mentions ids.
- **Scaling risk:** Axis B/D, at U2/T1/U3a — i.e., the next slices. Every spun-up world mints id 0 for its authored occupant, so the first cross-grid transfer collides an arriving fleet's ids with the destination's local ids; rosters and logs carry ambiguous `shipId`s the moment two stations exist; teardown/recreate resets both counters, so "never reused within a session" is already false at registry scope. Worse, a naive global counter fails U2's own accept test (recreate-bit-exact requires *reproducible* occupant ids), so this needs an actual design: deterministic authored-occupant ids (derivable from anchor) partitioned from session-allocated dynamic ids, plus a u16 exhaustion/reuse policy once a persistent server makes "65k ships ever" (`World.cpp:252`) a real scenario. The session-level ship→location index that order routing to unviewed grids, rosters, and summaries all imply also has no designed home — it is the natural owner of allocation.
- **Why structural:** ShipIds live on the wire, in the replay and transfer logs, in rosters, and in every test scenario. Deciding the scheme now is a paragraph in ADR-016 §4/U2; retrofitting it after T1/U3a build the logs and roster messages is a log-format migration plus a rewrite of every double-run scenario — the classic asymmetry.
- **Recommendation:** Amend ADR-016 §4 (and U2's accept) with a universe-layer identity design: registry-owned allocator; authored occupants take deterministic ids derived from their anchor; `World::Spawn`'s self-allocation becomes injection; state the u16 headroom/reuse policy (or widen before the id ships in persistent artifacts); name the ship-index as registry state.
- **Confidence:** High

#### SIM-2 — Teardown/recreate makes viewer behavior a hidden sim/replay input
- **Severity:** High
- **Refs:** ADR-016 §4 ("tear it down when the last ship leaves and nobody is watching"; "teardown and recreation reproduce bit-identically"), §7 ("a grid stays alive while it has ships *or a viewer*"), deliberate-omissions ("No grid persistence… a decision then, not an accident"); U2 accept (registry double-run + recreate-bit-exact; "viewer half lands with U3b"); `WorldHash.cpp:47` (per-world `m_tick` folded), `:134-135` (RNG folded); ADR-017 §9 (roster in registry hash); R16.
- **Design position:** A session replay is the per-grid order logs plus the transfer log; recreate is bit-exact from (session seed, anchor id). View subscriptions are not replay inputs and are not logged.
- **Scaling risk:** Axis B/D. The scheme is only closed under an *unstated* invariant: a world holding nothing but authored occupants never changes state and never draws RNG. Today that holds (verified: `Tick` mutates nothing for anchored hulls and draws no randomness). But a viewer-held empty world still ticks — its `m_tick` advances and is hashed — so the registry hash already depends on which worlds a viewer kept alive, which the replay log cannot reproduce; the U2/U3b double-run suite will either go flaky or silently never test it. And the first ambient feature (site depletion, wreck fields, patrols — the corpus's §2 promises Wreck/Site entities) turns hash-noise into true divergence keyed to unlogged client behavior: the worst debugging class the replay contract exists to prevent. The teardown rule also structurally forbids durable grid-local mutation — better to discover that as a stated rule now than mid-`Site`-feature.
- **Why structural:** Once U3b ships with view-held worlds hash-visible, fixing it means changing what the registry hash means and what the replay log contains — invalidating goldens and the shipped log format. Now it is three sentences and one test.
- **Recommendation:** Amend ADR-016 §4: (a) name the empty-world quiescence invariant and add a registry test (occupants-only world ticked N times hashes identically to its recreation); (b) define the registry hash/replay domain to exclude ship-less worlds *or* log spin-up/teardown as transfer-log events — pick one; (c) record the roster's pattern — durable grid-local state lives at the universe layer, worlds stay forgetful — as the standing rule future `Site` content must follow.
- **Confidence:** High

#### SIM-3 — The transfer bus's ordering key is ambiguous and, in one reading, not total
- **Severity:** High
- **Refs:** ADR-016 §4 and U3a: applied "in (arrival tick, **order id**) order"; ADR-017 §9 and T1: "in (apply tick, **record order**)"; `serverOrderId` is per-world and teardown-reset (`World.h:379`, `World.cpp:238`); R16 row (mitigation restates the ADR-016 key); registry hash coverage names rosters only (ADR-017 §9) while transit records are minutes-long state (ADR-016 §5).
- **Design position:** Tick-stamped records applied between ticks in a fixed order; the two ADRs spell the tie-break differently, and neither defines it precisely.
- **Scaling risk:** Axis B. Read as "(arrival tick, serverOrderId)", the key is non-total: order ids are per-world counters, so two same-tick arrivals from different source worlds can tie (both id 7), and teardown resets make ids repeat per anchor — a nondeterminism reachable exactly under the concurrent-opposite-warp scenarios U3a's accept names. Read as "record order" (filing order), it is total only if the registry's world-iteration order is itself pinned, which nothing specifies. Separately, in-flight transit records must join the registry-level hash — unlike the pending-order queue's one-tick window (`WorldHash.cpp:82-96`'s argument), a transit lives for minutes, so divergence there stays invisible until arrival.
- **Why structural:** T1 builds the bus and its log format next; whichever reading the builder picks becomes the replay contract. Correcting a non-total key later is a transfer-log format change plus re-goldening; correcting it now is one sentence stated once and referenced from both ADRs.
- **Recommendation:** Amend ADR-016 §4 (and note in ADR-017 §9/T1): a universe-layer monotonic `transferId` is stamped at filing and is the tie-break — `(applyTick, transferId)`; registry iteration order is anchor-id order, stated; in-flight transit records and the bus queue fold into the registry hash alongside rosters.
- **Confidence:** High

#### SIM-4 — Per-grid population has no governing decision while replication hard-stops at 43
- **Severity:** High
- **Refs:** `Snapshot.h:159-176` (cap = 45, derived; asserts), `Snapshot.cpp:53-61` (`WriteSnapshot` refuses entirely above the cap — correctly loud, but total); ADR-017 §5 (cap → 43, "margin of two"; "the delta encoding ADR-004 reserved is the growth path, not another byte"); ADR-004 §6 ("at the 1,024-entity cap… ~20 KB ⇒ delta + interest management"); corpus 1,024-entity cap (`tactical-icon-system.png` §3/§5, Architecture-Overview:188); ADR-017 §4 ("undocking is never refused for clutter"); U1–U6 and T1–T3: no slice delivers delta/interest.
- **Design position:** The sim caps nothing per grid (orders cap at 64 ships each, nothing caps a grid); the wire's growth path is named in three places; no slice schedules it and no rule bounds population until it lands.
- **Scaling risk:** Axis A/B, immediately after T2. Post-statusBits arithmetic: 43-record cap against 41 fleet + 1 station = 42 — margin one on the design's own numbers. The first second commander sharing a grid (the first real axis-A step), or any fleet growth past 42, or a 64-ship undock into an occupied grid, puts the population over the cap — and the designed behavior is *no snapshot at all* for that grid, a session-killing outage by construction. The sim cannot backstop it: undock-never-refused is a design commitment, and transfers-in have no refusal path. The growth path is adequate; what is missing is an owner and a trigger — the gap between "reserved" and "scheduled" is where this class of cliff ships.
- **Why structural:** Not a data-model rework — a sequencing decision that becomes an emergency retrofit (delta + interest under incident pressure, or a hasty population cap contradicting ADR-017 §4) if taken late. The wire mechanics belong to the network reviewer; the missing *decision* is joint and cheap now.
- **Recommendation:** Record the decision: delta + interest management is a named prerequisite slice gating (a) any second commander per grid and (b) any per-grid population above the full-snapshot cap; until it lands, the bake/scenario invariants keep authored-per-grid population under the cap, and the T2 accept gains an explicit over-cap behavior test (what the server does, loudly, when a grid exceeds capacity).
- **Confidence:** High

#### SIM-5 — Nothing gates behavior parity across build skew — the schema hash covers shape only
- **Severity:** Medium
- **Refs:** `SchemaHash.h:44-58` (covers layout, quantisation, enum values, `caps{shipsPerOrder,ordersPerSnapshot}` — not `MAX_ORDER_LEGS` (`Orders.h:155`), not `PLAY_AREA_HALF_EXTENT_CM` (`Validate.h:78`), not the check order, not `OrderReason` values); `Wire.h:77-119` (handshake = protocolVersion + schemaHash + contentHash, no gameplay version); ADR-005 §4a (check order "is part of the contract"); ADR-005 §6 (same-binary scope); ADR-017 §8 (mid-sequence `InvalidQueueMode` insertion), Consequences ("`DOCK_RADIUS_METRES`… table data, retunable as table edits"); `ShipClass.h:9-14` ("when balance wants to be data, it arrives with a hash" — the awareness exists for *content*, not for code constants).
- **Design position:** Fail-closed `UpdateRequired` on schema/content mismatch is the whole compatibility story; BounceParity is bought by "one implementation" — which is only one implementation when both ends run the same build.
- **Scaling risk:** Axis A/E, the first patch after remote clients exist. A server retune of `MAX_ORDER_LEGS`, the play-area bound, or (post-T1) `DOCK_RADIUS_METRES` — explicitly invited as "retunable table edits" — changes verdicts without touching a field, so a stale client passes the door and then systematically disagrees with the authority: pre-checks pass locally and bounce remotely (or refuse orders the server would take, the direction ADR-005 §4a calls worse), persistently rather than as designed staleness. Check-order edits and `OrderReason` appends are likewise invisible to the gate. Server-fleet operations under same-binary determinism (replays/goldens per build) are already accepted; this is the one *cheap* missing gate.
- **Why structural:** Not a rework — a widening window. Every ungated behavior change shipped before the gate exists is a silent-skew combination in the field that no tool detects, and BounceParity's debugging story ("same function") stops being true precisely when it is hardest to check.
- **Recommendation:** Amend ADR-004/ADR-005: fold the verdict-affecting constants and the check-order sequence (or a single `GAMEPLAY_VERSION` bumped by rule alongside them) into `GAME_SCHEMA_TEXT`, and write the rule where "retunable as table edits" is written: a retune of any validation bound is a compatibility event.
- **Confidence:** High

#### SIM-6 — Multi-world tick cost: the budget is per-world, the soak was never run, and world-isolation is not yet a stated invariant
- **Severity:** Medium
- **Refs:** `World.cpp:154-205` (Steering's traffic scan is all-ships per seeking ship — O(n²)/tick), `:900-967` (`Separate`: 4 Gauss-Seidel passes over all pairs); Architecture-Overview:140-145 ("the tick must fit 50 ms with 1,024 entities" — singular); ADR-016 ("the server ticks every grid with presence"); ADR-007 (two owning threads); R10 (mitigation includes "synthetic 1,024-entity headless soak once S6 lands" — no such test exists in `Tests/`, verified by search; "parallelisable behind the replay gate").
- **Design position:** R10 owns the single-world budget with telemetry in place; ADR-016 multiplies worlds without restating an aggregate budget or a threading posture; the registry ticks serially on the one Sim thread as designed.
- **Scaling risk:** Axis B. At 1,024 entities the two O(n²) systems are ~1M + up to ~2M pair evaluations per world-tick, ×20 Hz, ×M concurrent grids, serial. R10's claim of parallelism is *structurally true* — verified Steering writes only its own slot and worlds interact only between ticks — but it stays true only if U2's registry API keeps worlds share-nothing and tick-order-invariant; one convenient cross-world read written casually in U2/T1 (a berth scan peeking at another grid, a summary read mid-tick) forecloses it quietly. The spatial-partition fix for n² is contained (two pure functions, dense arrays are the friendly layout) — it needs a measured trigger, not a redesign.
- **Why structural:** Invariants are cheap to state before the code that could violate them exists and expensive to excavate after; and an unmeasured budget cannot schedule the partition or the thread-pool amendment before players feel `tickOverrun`.
- **Recommendation:** In U2's spec: state the world-isolation invariant (worlds share no mutable state; bus and extract are the only crossings) and add permuted-world-tick-order bit-identity to the registry double-run suite — this keeps parallelism reachable forever at the cost of one test. Run R10's 1,024 soak now (it has been runnable since S6) and record the number; note the planned ADR-007 amendment (sim worker pool with a bus barrier) as the reserved response.
- **Confidence:** High

#### SIM-7 — Ownership is a promised column that spends the last of the snapshot margin
- **Severity:** Low
- **Refs:** `Validate.cpp:87-90` ("ownership is a field, not a redesign"), `Validate.h:44-46` (where the owner joins the view), `Orders.h:126` (`NotOwned` reserved); no owner column in `World.h:363-371`, none in `EntityRecord.h`, `OrderGroup`, `OrderStateRecord`, or the roster row (ADR-017 §8); corpus relationship channel (OWN/ALLIED/NEUTRAL/HOSTILE) requires per-entity owner on the client (`tactical-icon-system.png` §3).
- **Design position:** Multi-commander ownership is deliberately deferred with named seams; the reserved reason and view-slot are real.
- **Scaling risk:** Axis A. The mechanism is genuinely add-a-column — but the *wire* column costs: an owner byte on `EntityRecord` after statusBits (22 bytes) puts the full-snapshot cap at 41, exactly the asserted floor with zero margin; groups, order records, and rosters each need the field too, and `lastOrderSeqProcessed`/`clientOrderSeq` become per-client quantities on a shared grid. None of this is rework; all of it lands on SIM-4's shrinking margin at the same moment.
- **Why structural:** Only via sequencing — ownership's snapshot cost is a second reason the delta decision (SIM-4) must precede axis A, and recording that dependency now prevents the owner byte being deferred into the icon system's relationship channel arriving with no data to read.
- **Recommendation:** One recorded line in the multi-client plan: ownership replication rides the delta/interest slice, not the full-snapshot format; the sim-side column (owner array, hash fold, `ValidationView` extension) is the cheap half and can land any time.
- **Confidence:** Medium

### Questions for the owner
1. **Axis-A target scale:** what is the intended ceiling for commanders per *session* and, more specifically, commanders sharing one *grid*? A handful changes SIM-4's trigger to "before the first shared-grid test"; MMO-scale concurrency additionally raises per-grid instancing questions the current design deliberately doesn't touch.
2. **Persistence horizon:** does the 2,500-system universe eventually run as a long-lived persistent server (the RESUME card taken literally), with construction/economy minting ships over months? Yes settles SIM-1's u16 reuse/width policy now (before ids ship in rosters, logs, and saves) and puts the u32 tick epoch (~6.8 years cumulative at 20 Hz) on the record.
3. **Durable grid-local state:** should any future grid content persist after the last ship leaves (mined-out fields, wreck fields), or is the rule permanently "durable state lives at the universe layer, worlds forget"? SIM-2's recommendation hardens the latter into an invariant; if you foresee the former, teardown itself needs a different design and cheaper now than at `Site` time.
4. **Transfer bus key:** may T1 introduce a universe-layer monotonic `transferId` as new hashed state to serve as the bus's tie-break (SIM-3), or do you prefer the key derived from existing identities (which then depends on SIM-1's allocator landing first)? One line settles both ADRs.
5. **Compatibility posture for tuning:** when a validation bound is retuned ("table data, retunable as table edits"), is refusing stale clients at the door acceptable (fold constants into the schema text — strict but simple), or do you want a compatibility window (a separate behavior version with server-side tolerance)? SIM-5's shape follows directly.
