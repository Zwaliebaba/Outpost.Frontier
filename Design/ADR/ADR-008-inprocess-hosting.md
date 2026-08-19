# ADR-008 — In-Process Hosting: Composition Root, Lifecycle, Headless Proof

**Status:** Accepted · 2026-08-17 · **§4–§5, §8 amended by ADR-012 (JSON config, no argv)**
· §8 further amended by [ADR-018](ADR-018-scaling-baseline.md) (2026-08-19): the packaging
split is architecture-complete *and* gated on the remote-play ADR (D10 — pinned-key trust,
validation on off-loopback, transport config surface, abuse posture); sessions survive
transport disconnect for a grace window keyed on `PlayerId` (D5)
**Depends on:** ADR-003 (transport), ADR-007 (threads/ownership), ADR-012 (configuration)
**Feeds:** Build Order S3/S4, future OutpostServer.exe

## Context

`Outpost.exe` hosts both halves in the MVP. The requirement: splitting server and client into
separate processes/machines later must be a packaging change. ADR-007 already guarantees the
data-path half of that (transport-only crossings); this ADR fixes boot, shutdown, and the
programmatic shape that a standalone server binary will reuse verbatim.

## Decision

### Shape
1. **`Neuron::ServerHost`** (NeuronServer) is a self-contained service object:
   `ServerHost(ServerConfig) → Start() → [Stop() → Join()]`. `Start` binds the listener
   transport, spawns the Sim thread (ADR-007), and returns once the server is *listening* —
   so a caller may connect immediately. It owns the authoritative `Game::World`, session/
   connection table, and tick loop. It has **zero knowledge of any client**, including the
   in-process one: a client disconnect returns the session to "empty server ticking along",
   never a shutdown (multi-client future depends on this posture).
2. **`Neuron::ClientApp`** (NeuronClient) is likewise self-contained:
   `ClientApp(ClientConfig) → Run()` — creates window + device, connects its transport to
   `ClientConfig.serverEndpoint` (always `127.0.0.1:<port>` in MVP), runs the Main-thread frame
   loop until quit, returns an exit code. It reaches the server **only** through that endpoint.
3. **`Outpost.exe` is a composition root and nothing else** (~150 lines): parse args → init
   logging/telemetry/lane registry → construct configs → `ServerHost.Start()` →
   `ClientApp.Run()` (blocks) → orderly shutdown. No game, net, or render logic lives in the
   exe. Libraries never read `argv`/env/registry — config structs are assembled here only.

### Configuration (MVP, complete) — *amended by ADR-012*
4. **There is no command line and no environment variable.** `wWinMain` ignores its arguments;
   every knob comes from `Outpost.json` (resolution order and full schema in ADR-012 §A/§B).
   The keys that drive this ADR:
   - `mode` — `"host"` (ServerHost + ClientApp, the MVP default), `"headless"` (ServerHost
     only, console logging, runs until Ctrl-C — **this mode is the standing proof that the
     server has no client dependency**; it is a build-order slice, not an afterthought), or
     `"client"` (skips hosting and connects to `client.connect`, so the *packaging change* is
     demonstrably trivial — only loopback is exercised in MVP).
   - `server.port` (0 = ephemeral; in `host` mode the client reads the bound port back from
     `ServerHost` in-process — a loopback-only convenience that dies with the split),
     `server.transport` = `"udp" | "quic"` (ADR-003).
   - `selfTest` — with `mode: "headless"`, runs wire round-trips, the schema-hash self-check,
     a transport handshake over loopback, and a GameLogic replay-determinism run, then exits
     (non-zero on failure). CI selects it by launching from a directory whose `Outpost.json`
     sets it — GPU-less and complementary to the `Tests/` unit-test projects.

### Lifecycle & shutdown ordering (normative)
5. **Boot:** load + merge config (fatal on missing/invalid base, ADR-012 §A4) → logging →
   lane registry → `ServerHost.Start()` → `ClientApp` ctor → `ClientApp.Run()`. A `Start()`
   failure (port bound, transport init) is a fatal, logged, user-visible exit — no client
   without a server unless `mode: "client"`. Config loading is the *first* thing that happens
   and the only file reading the composition root does on behalf of the libraries.
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
   dev, Windows service wrapper when needed): load its own `OutpostServer.json` →
   `Start() → WaitForStopSignal() → Stop() → Join()`. Because `mode: "headless"` runs exactly
   this code path inside `Outpost.exe` from the first server slice onward, the standalone
   binary is a new vcxproj + a `main()` — the promised packaging change. Client-side, the
   split is `mode: "client"` with a remote `client.connect` + QUIC transport (ADR-003) + real
   cert validation; no architectural work remains by construction.

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
