# ADR-007 — Threading: Two Owning Threads, Transport-Only Crossings

**Status:** Accepted · 2026-08-17
**Depends on:** ADR-002 (tick loop), ADR-003 (Poll contract), ADR-005 (single-writer worlds)
**Feeds:** ADR-008 (lifecycle), telemetry design

## Context

One process hosts an authoritative server and a rendering client that must later split across
processes/machines as a packaging change. The corpus fixes the long-term shape: the client owns
**8 thread lanes** (Main, Game, Render + five workers; `debug-hud.png` — "fails at startup if
it asks for a ninth"), the server 15+1; per-thread SPSC telemetry rings are drained by the
owning thread; client frame budgets are reported as `GAME / EXTRACT / RENDER / UI`. The MVP
must not paint over those seams — but also must not build 23 threads for a demo of 41 ships.

## Decision

### Threads (MVP)
1. **Main thread** — Win32 message pump + the *entire* client:
   `Pump → Poll(clientTransport) → BufferSnapshots → Extract → AudioUpdate → Record → Present`.
   Extract interpolates the replicated view (ADR-002) and emits `InstanceRecord`s + overlay
   draw lists (ADR-006); the stage names exist from day one so the four budget rows the debug
   sheet shows are measured, not retrofitted. Rendering on the pump thread is correct for a
   game window (modal drag/size stalls are accepted MVP behaviour; noted, not solved).
2. **Sim thread** — the entire server: waitable-timer loop,
   `Poll(listenerTransport) → IngestOrders → Tick → EmitSnapshots → Send` (ADR-002 §2).
   Network I/O is polled on the sim thread — no separate net thread in MVP; on loopback with
   one client there is nothing for it to do but add a handoff.
3. **Transport-internal threads** — none for `UdpTransport`; msquic later brings its own
   worker pool, already absorbed by the ADR-003 rule: *completions enqueue internally
   (MPSC), surface only via `Poll()` on the owning thread.* No game or render code ever runs
   on a transport thread.
3a. **XAudio2 threads** — owned by XAudio2, registered as external lanes. Voice callbacks
   push events into an SPSC ring and touch nothing else; all audio work happens in the
   Main-thread `AudioUpdate` stage (ADR-011 §8–9). Same rule as transport: foreign threads
   enqueue, owning threads act.
4. **Boot task pool** — NeuronCore ships a minimal fixed pool (`Submit`, `WaitGroup`), used at
   startup for mesh/atlas bakes only. Sim tick and render frame are **single-threaded by
   rule** in MVP; the first parallel consumer inside a frame/tick must bring the deterministic
   partitioning story (ADR-005 §5) and a profile justifying it.

### Ownership & crossing rules (normative, all builds)
5. **Single-writer worlds.** The authoritative `game::World` is owned by the Sim thread; the
   client's replicated view + render state by the Main thread. There is *no shared world*, no
   "read the server's arrays from the client while it isn't ticking".
6. **The only data path between client and server halves is the transport** — a real loopback
   socket (fixed constraint), even in-process. The composition root may hand both sides
   *immutable* config at boot and nothing else. This makes process separation a packaging
   change by construction: there is no in-proc channel to replace.
7. **Enforcement is mechanical, not aspirational:** worlds carry an owner-thread id in debug;
   mutation entry points assert `NEURON_ASSERT_OWNER(world)`. Telemetry and logging are the
   only cross-thread sinks, both lock-free SPSC rings drained by the owner (log flush on a
   drain by whichever thread owns the sink file — Main in MVP).
8. **Lane registry from day one.** Threads register named lanes at startup
   (`Main`, `Sim`, boot workers, transport externals via `RegisterExternalThread`); the
   registry is capped (client-side cap 8 honoured when the split happens) and each lane gets
   its telemetry ring. MVP cost: an array and some macros (`NEURON_SPAN`, `NEURON_COUNTER`);
   payoff: the debug HUD and server `/metrics` land on existing rails.

### The future split (documented seam, not built)
9. Client `Game` thread (prediction, order pre-check, de-clutter prep) separates from Main at
   the **Extract boundary** — Extract is written as "read replicated view, write render
   structs" precisely so it can become the cross-thread copy point later. Render-worker
   parallel recording separates inside Record. Server net thread separates at the existing
   `Poll` seam. None of these change any interface defined above.

## Alternatives rejected

- **Dedicated net thread(s) in MVP** — a handoff queue serving zero contention; msquic will
  impose its own threads later anyway, and the Poll contract already absorbs them. Rejected.
- **Shared-memory snapshot handoff in-process** ("it's the same process, why serialize?") —
  explicitly forbidden by the brief; would silently fork the code paths the split depends on.
  Rejected on principle and by constraint.
- **Render thread separate from message pump now** — solves modal-loop stalls the MVP can
  live with, at the cost of input/present marshalling machinery. Deferred to the client
  Game/Render split, which has a real driver (prediction).
- **Job-system-first design (task-graph everything)** — determinism risk and scheduler
  machinery with no workload to justify it at 41 ships / ~2 ms frames. Rejected for MVP.

## Consequences

- Data races between sim and render are impossible by construction rather than by locking —
  there is no shared mutable state to race on.
- The loopback hop costs one serialize/deserialize per tick (~1 KB) — microseconds; accepted
  as the price of the packaging-change guarantee.
- Modal drag stalls freeze presentation but not the server (sim thread keeps ticking; client
  interpolation catches up on release — and the STALE path gets exercised for free).
- The 8/16-lane budgets stay honest: MVP registers 2 owned lanes + boot workers, leaving the
  corpus's headroom intact instead of quietly spending it.
