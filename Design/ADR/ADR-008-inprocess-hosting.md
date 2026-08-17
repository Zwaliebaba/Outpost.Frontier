# ADR-008 — In-Process Hosting: Composition Root, Lifecycle, Headless Proof

**Status:** Accepted · 2026-08-17
**Depends on:** ADR-003 (transport), ADR-007 (threads/ownership)
**Feeds:** Build Order S3/S4, future OutpostServer.exe

## Context

`Outpost.exe` hosts both halves in the MVP. The requirement: splitting server and client into
separate processes/machines later must be a packaging change. ADR-007 already guarantees the
data-path half of that (transport-only crossings); this ADR fixes boot, shutdown, and the
programmatic shape that a standalone server binary will reuse verbatim.

## Decision

### Shape
1. **`neuron::server::ServerHost`** (NeuronServer) is a self-contained service object:
   `ServerHost(ServerConfig) → Start() → [Stop() → Join()]`. `Start` binds the listener
   transport, spawns the Sim thread (ADR-007), and returns once the server is *listening* —
   so a caller may connect immediately. It owns the authoritative `game::World`, session/
   connection table, and tick loop. It has **zero knowledge of any client**, including the
   in-process one: a client disconnect returns the session to "empty server ticking along",
   never a shutdown (multi-client future depends on this posture).
2. **`neuron::client::ClientApp`** (NeuronClient) is likewise self-contained:
   `ClientApp(ClientConfig) → Run()` — creates window + device, connects its transport to
   `ClientConfig.serverEndpoint` (always `127.0.0.1:<port>` in MVP), runs the Main-thread frame
   loop until quit, returns an exit code. It reaches the server **only** through that endpoint.
3. **`Outpost.exe` is a composition root and nothing else** (~150 lines): parse args → init
   logging/telemetry/lane registry → construct configs → `ServerHost.Start()` →
   `ClientApp.Run()` (blocks) → orderly shutdown. No game, net, or render logic lives in the
   exe. Libraries never read `argv`/env/registry — config structs are assembled here only.

### Command line (MVP, complete)
4. `--port=7777` (default; 0 = ephemeral, client reads the bound port from ServerHost —
   loopback-only convenience that dies with the split),
   `--headless` (ServerHost only, console logging, runs until Ctrl-C — **this flag is the
   standing proof that the server has no client dependency**; it is a build-order slice, not
   an afterthought),
   `--transport=udp|quic` (ADR-003),
   `--selftest` (headless: wire round-trips, schema-hash self-check, transport handshake
   over loopback, GameLogic replay-determinism run; exit code ≠ 0 on failure — CI-able on a
   GPU-less machine, complementing the `Tests/` unit-test projects),
   `--connect=<host:port>` (client-only mode, skips hosting; exists so the *packaging change*
   is demonstrably trivial, but only loopback is exercised/supported in MVP).

### Lifecycle & shutdown ordering (normative)
5. **Boot:** logging → lane registry → `ServerHost.Start()` → `ClientApp` ctor →
   `ClientApp.Run()`. A `Start()` failure (port bound, transport init) is a fatal, logged,
   user-visible exit — no client without a server unless `--connect`.
6. **Orderly shutdown (window close / quit):**
   1. ClientApp leaves its frame loop, sends `Goodbye`, gives the transport ≤ 250 ms to drain,
      then closes its connection;
   2. ClientApp flushes GPU (fence to idle), destroys device objects, destroys window;
   3. Composition root calls `ServerHost.Stop()`: stop accepting, send `Goodbye` to remaining
      sessions, run one final tick, signal the Sim thread, `Join()`;
   4. Telemetry/log flush, process exit. Server-before-client teardown is **forbidden** —
      the client must never render against a vanished server (it would exercise the STALE
      path during every exit and mask real bugs).
7. **Failure containment (MVP posture):** a Sim-thread fatal error logs, flags the host as
   failed, and requests process exit; the client shows "connection lost" (its normal handling
   of a dead transport — one code path for local crash and future remote loss). No in-process
   restart machinery in MVP.

### The future standalone server
8. `OutpostServer.exe` = the same `ServerHost` behind a service `main()` (console/Ctrl-C in
   dev, Windows service wrapper when needed): construct config from args/file →
   `Start() → WaitForStopSignal() → Stop() → Join()`. Because `--headless` runs exactly this
   code path inside `Outpost.exe` from the first server slice onward, the standalone binary
   is a new vcxproj + a `main()` — the promised packaging change. Client-side, the split is
   `--connect=<remote>` + QUIC transport (ADR-003) + real cert validation; no architectural
   work remains by construction.

## Alternatives rejected

- **Client-owns-server** (server as a member of the client app) — inverts the authority
  relationship, couples server lifetime to the window, and makes multi-client structurally
  awkward. Rejected.
- **Two processes from day one** (`Outpost.exe` spawning `OutpostServer.exe`) — pays process
  management, debugging friction, and installer complexity now to prove a property the
  transport-only rule + `--headless` already prove more cheaply. Rejected for MVP; it is the
  *first* post-MVP packaging exercise.
- **In-proc service locator / shared singletons between halves** — reintroduces the hidden
  coupling everything above exists to prevent. Forbidden.

## Consequences

- The MVP binary demonstrably contains a complete dedicated server; `--headless` keeps it
  honest in CI from the first networking slice.
- Composition-root-only config means test code can host `ServerHost` + a raw transport client
  with no exe involved (NeuronServerTests does exactly this).
- One shutdown ordering to reason about; the "client always outlives nothing" rule gives a
  single disconnect code path shared by local crash, remote loss, and future server restarts.
- Cost accepted: exe start requires a successful port bind even for pure-client experiments
  (worked around with `--connect`).
