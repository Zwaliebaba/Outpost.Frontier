# ADR-002 — Server Tick Model & Client Time

**Status:** Accepted · 2026-08-17
**Depends on:** ADR-001 (planar sim)
**Feeds:** ADR-003/004 (transport, wire), ADR-005 (determinism), ADR-007 (threading)

## Context

The server is authoritative and must simulate at a rate that (a) feels responsive for fleet
command, (b) leaves CPU headroom for the long-term 1,024-replicated-entities-per-client cap
(`overlay-pass.png`, `tactical-icon-system.png`), and (c) decouples cleanly from an
uncapped/vsynced client framerate. The corpus fixes two relevant numbers already: the STALE
marker caps **extrapolation at 250 ms** (`tactical-icon-system.png` §4), and the debug HUD
(`debug-hud.png`) displays snapshot *age* (~61 ms shown) and *drift in ticks* — so client-side
tick-offset tracking is an assumed capability.

## The argument

Genre anchors: EVE ticks at 1 Hz (fine for its combat, too coarse for continuous formation
steering); RTS engines sit at 10–24 Hz sim with interpolated rendering. Our motion model is
continuous steering (ADR-005), which needs enough servo rate for stable formation-keeping but
has no twitch input.

- **10 Hz:** cheapest; but 100 ms command quantisation is perceptible against the "ghost
  promotes on the next snapshot" feedback loop the corpus demands, and formation servo gains
  get touchy at 100 ms steps.
- **30–60 Hz:** feel gains are eaten by interpolation anyway (the player watches interpolated
  state, not ticks); 1.5–3× server CPU and snapshot bandwidth per client for nothing the MVP or
  an MMO shard wants.
- **20 Hz (50 ms):** stable steering, ghost promotion within ≤ 100 ms perceived, matches the
  ~61 ms snapshot-age figure the debug sheet was drawn with, and halves the cost of 40 Hz-class
  rates. **Self-challenge:** is 20 Hz enough if combat later needs projectile sim? Yes —
  hit-scan and guided weapons resolve fine at 50 ms with interpolated presentation; if a future
  weapon genuinely needs finer resolution, it sub-steps *inside* GameLogic without changing the
  tick, wire, or client contract.

## Decision

1. **Fixed timestep, 20 Hz.** `TickRate = 20`, `TickDt = 50 ms` exactly (`dt = 0.05f` in sim).
   Tick index is `uint32` (~6.8 years of uptime; wraps are a non-problem but comparisons use
   serial arithmetic anyway). Simulation time *is* the tick count; GameLogic never reads a
   clock (ADR-005).
2. **Server loop:** dedicated sim thread (ADR-007) drives `tick → snapshot → send` on a
   high-resolution waitable timer with an absolute schedule (`next += 50 ms`). If the loop
   falls behind by more than 5 ticks (250 ms), it **snaps forward** (drops the debt, logs, and
   increments a `tick_overrun` counter) rather than death-spiralling. Catch-up bursts execute
   at most 2 extra ticks per wake.
3. **Snapshot cadence = tick cadence (MVP).** One snapshot datagram per tick per client. The
   header carries the tick, so cadence can be decimated per zoom tier / per client later
   without a wire change (ADR-004).
4. **Client render is free-running** (vsync or uncapped) and never blocks on the network.
   Per frame the client:
   - estimates server time `t_est = serverTick_latest + age/TickDt`, smoothed with a slew
     limiter (no jumps; ±2 % rate correction), giving the *drift* readout the debug HUD shows;
   - renders at `t_render = t_est − InterpDelay`, with **InterpDelay = 2 ticks (100 ms)**;
   - **interpolates** ship transforms between the two snapshots bracketing `t_render`
     (positions lerp, headings slerp-on-circle);
   - if the newer bracket is missing (loss/hitch), **extrapolates** from last velocity for at
     most **250 ms**, then freezes the entity and flags it STALE (marker per icon sheet;
     debug-only surface in MVP).
5. **Order timing:** client→server orders are not tick-aligned; the server applies orders at
   the start of the next tick after arrival + validation. On loopback this yields ≤ 100 ms
   press-to-promotion worst case, hidden behind the client-side ghost (ADR-004).

## Alternatives rejected

- **1 Hz EVE-style tick** — incompatible with continuous formation steering and with the
  ghost-promotion feedback design; would force client-side movement authority to feel alive.
- **Variable timestep** — non-deterministic, unreplayable, tuning nightmare. Never.
- **Render-locked sim (single loop)** — couples the two things the brief explicitly requires
  decoupled, and dies the moment server and client split processes.
- **Client-side prediction in MVP** — unnecessary on loopback (RTT ≈ 0) and for order-based
  command (no per-frame input to predict). The seam is reserved: GameLogic is linkable by the
  client (fixed constraint) and snapshots carry the tick + last-processed-order ack needed for
  reconciliation later.

## Consequences

- Perceived command latency ≈ order transit + ≤ 1 tick + InterpDelay ≈ 100–150 ms on loopback,
  masked by instant ghost feedback. Acceptable for fleet command; revisit InterpDelay (drop to
  1 tick) only with evidence.
- Interpolation requires ≥ 2 buffered snapshots before the world first renders "live" —
  connection flow must tolerate a ~100 ms warm-up (join screen covers it).
- All sim constants tune against a fixed 50 ms dt; changing TickRate later is a *balancing*
  event, not a refactor (nothing outside GameLogic assumes 50 ms except via named constants).
- Server CPU budget per tick is 50 ms hard ceiling; the `tick_overrun` counter is a release
  telemetry field from day one (`debug-hud.png` Tier 1).
