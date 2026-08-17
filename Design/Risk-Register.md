# Risk Register

**Status:** Session output 2026-08-17 · Top technical risks, each with mitigation already
wired into the ADRs/build order (referenced), plus its early-validation point. Review at each
milestone (M0/M1/MVP).

| # | Risk | Impact | Likelihood | Mitigation (designed-in) | Early validation |
|---|---|---|---|---|---|
| R1 | **Isometric readability** — flat-shaded fleets on a dark plane become unreadable blobs; depth/occlusion confusion sinks the core fantasy | High | Med | Planar sim kills depth ambiguity at the root (ADR-001); ortho 30° + world-space 2:1 rings + screen-floor clamps per corpus scaling law (ADR-006); icon channels/de-clutter ladder reserved | S5 visual checkpoint vs prints; S8 occlusion rules; playtest at min/max zoom each slice after |
| R2 | **Determinism erosion** — accidental clock/RNG/iteration-order leaks make replays lie and (later) prediction/desync work impossible | High | Med | Same-binary replay determinism is a *tested invariant*, not a goal: double-run hash suite from S6, `--selftest` gate in CI (ADR-005); single-writer + owner asserts (ADR-007); /fp rules; validation-on-quantised-inputs parity rule | S6 harness lands before first networked ship (S7); any red replay test blocks merge |
| R3 | **msquic integration friction** — Schannel-flavour cert provisioning (no PEM/PKCS12), callback threading, QUIC-TLS OS dependency (Win11/Server 2022+) | Med | Med | QUIC-shaped `ITransport` from day one so only the impl is at risk (ADR-003); known-good path pre-chosen (`CERTIFICATE_CONTEXT` + in-memory self-signed, ALPN `opf/1`, `NO_CERTIFICATE_VALIDATION` on loopback); UDP fallback is permanent; OpenSSL package flavour identified as the swap (flagged in Dependency Map §5 for owner sign-off) | S13 spike is *scheduled*, pre-MVP, with explicit exit criteria; datagram cap enforced from S4 so no payload surprises |
| R4 | **DX12 boilerplate burden** — device/swapchain/sync/upload plumbing consumes weeks before gameplay | Med | High | Scope fenced to 4 PSOs, one queue, no compute, no bindless, no MSAA initially (ADR-006); glyph atlas instead of D3D11On12/D2D interop (largest boilerplate item deleted); debug layer on from S1 | S1 and S5 are the first slices — burn-down is visible in week one, not month three |
| R5 | **Loopback UDP is not a function call** — socket-buffer drops/stalls under load or debugger pauses look like heisenbugs | Med | Med | Contract forbids assuming zero loss (ADR-003); full snapshots are idempotent (ADR-004); interp→extrap≤250 ms→STALE degrades gracefully (ADR-002); receive buffers sized at init; drop counters in telemetry | S7 induced 400 ms stall test; drop counters visible in S14 strip |
| R6 | **Formation/steering feel** — servo oscillation, station fighting, straggler deadlocks make the one MVP mechanic feel bad | Med | Med | Arrival-slowdown steering with class clamps, stations non-overlapping by construction, leg completion tolerance + straggler timeout (ADR-005); envelope tests pin turn radius/overshoot | S6 envelope suite; S10 "preview equals outcome" test; tuning constants isolated in `ShipClassTable` |
| R7 | **Scope creep from the corpus** — the prints describe a shipped game (strategic map, alerts taxonomy, abilities); building toward pictures instead of the MVP | Med | High | Every ADR carries an explicit MVP-subset + reserved-seam table; overview's omissions table is the contract; new surfaces require a slice + owner ack | Build order is the whip — anything not in S1–S14 is post-MVP by definition |
| R8 | **Float precision at world scale** — future galaxy-scale coordinates break float32 sim/render | Low (MVP) | High (later) | MVP grid bounded 40 km (≈5 mm resolution); growth path = per-grid local origins matching the corpus's system-graph universe (ADR-001 §3) — no continuous mega-space ever planned | Documented now; revisit at multi-system milestone |
| R9 | **Text/HUD scope** — HUD needs (roster, bars, toasts, ETAs) balloon into a UI framework | Med | Med | Atlas ASCII + quads only, layouts hardcoded to print zones, UI-scale multiplier the only flexibility (ADR-006); no i18n/shaping in MVP | S11 accept criteria; any widget beyond the prints' zones is a flag |
| R10 | **Tick budget at scale** — 20 Hz fine at 41 ships, unknown at 1,024 replicated + interest mgmt | Low (MVP) | Med (later) | `tick_overrun` + per-stage spans are release telemetry from S3 (ADR-002); snapshot growth path (delta+interest) reserved in wire (ADR-004); sim is SoA + single-writer, parallelisable behind the replay gate | Synthetic 1,024-entity headless soak once S6 lands (cheap, scriptable) |

## Standing spikes (cheap, do early, answer one question each)

1. **Loopback heartbeat under debugger** (S4): does breaking in the IDE wedge either side?
   Expected: no — transport tolerates stalls; verifies R5 posture.
2. **Replay-hash across Debug/Release** (S6): confirm hashes *differ* across configs is
   acceptable and documented (same-binary scope, ADR-005 §6) — prevents a false alarm later.
3. **1,024-instance draw** (after S5): 1,024 `InstanceRecord`s through the opaque pass —
   validates the corpus entity cap costs nothing on the render side (it shouldn't, instanced).
4. **Schannel QUIC availability** (before S13): one-liner msquic `QuicOpen` + listener on the
   dev/CI images — surfaces the OS-dependency question while there's slack, not during S13.
