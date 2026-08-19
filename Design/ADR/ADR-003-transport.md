# ADR-003 — Transport: QUIC-Shaped Abstraction, UDP Loopback First

**Status:** Accepted · 2026-08-17 · Amended at S13 (2026-08-18): owner directive — QUIC only,
`UdpTransport` deleted; §3's transport flag and §4's permanent-fallback decision are
superseded below · Amended 2026-08-19 by **[ADR-021](ADR-021-interest-and-delta.md)** (§1's
"exactly one reliable ordered channel" becomes two — `Bulk` carries keyframes so they never
queue behind orders) and **[ADR-022](ADR-022-remote-play.md)** (§1's `Listen`/`Connect` take
descriptors, and §3's unconditional `NO_CERTIFICATE_VALIDATION` becomes a policy that is
legal only against loopback)
**Depends on:** ADR-002 (tick/snapshot cadence)
**Feeds:** ADR-004 (wire protocol), ADR-007 (threading), ADR-008 (hosting)

## Context

MVP traffic is UDP on 127.0.0.1 between the in-process server and client — a real socket, no
shared-memory shortcut (fixed constraint). The target protocol is QUIC via msquic; the template
already references the **`Microsoft.Native.Quic.MsQuic.Schannel` 2.6.0** NuGet package in every
project. The question is whether the MVP speaks msquic from day one or raw UDP behind an
abstraction with msquic slotted in later — decided with the multi-client, internet-facing
future in mind.

## The argument

**msquic day one:** no re-integration risk, the protocol surface (streams + datagrams,
connection lifecycle) is exercised from the first heartbeat, encryption posture is real.
Cost: the very first vertical slice ("heartbeat crosses the loopback") acquires TLS cert
provisioning, ALPN, msquic's callback/threading model, and debugging-through-encryption —
days of friction before the game exists, in exactly the phase where iteration speed on the
*game* matters most.

**Raw UDP forever, abstract later:** fastest start, but the abstraction then congeals around
UDP's shape (connectionless, message = packet, one channel) and msquic becomes a redesign —
the classic abstraction-validated-against-one-implementation trap. This is the actual failure
mode the brief warns about.

**Decision-relevant asymmetry:** the risk is not *which library*, it is *whose semantics the
interface encodes*. If the interface is QUIC-shaped — connections, one reliable ordered stream,
unreliable datagrams with an MTU-ish size cap — then a UDP loopback implementation is trivial
to write beneath it, and msquic drops in without touching callers. The reverse is not true.
**Self-challenge:** does faking "reliable stream" over UDP smuggle in real work (ack/retransmit
= half of TCP)? On loopback, loss is rare (buffer overflow only) but *possible*, so the UDP
implementation gets a deliberately dumb reliability layer: sequence + cumulative ack +
stop-and-wait resend on a 50 ms timer, for the control channel only. That is ~a hundred lines,
is throwaway by design, and is honest about the contract instead of pretending loopback is a
function call.

## Decision

1. **`Transport` lives in NeuronCore** (per the fixed library charter) and is **QUIC-shaped**:
   - `Listener` (server): accepts `Connection`s.
   - `Connection`: exactly **one reliable-ordered bidirectional message channel** (maps to a
     QUIC stream; carries handshake, orders, acks) and **unreliable unordered datagrams**
     (map to QUIC DATAGRAM frames; carry snapshots and pings).
   - Message-oriented API on both channels (the transport owns stream re-framing); payload cap
     for datagrams: **1,152 bytes** (safe under QUIC datagram MTU after overheads). The cap is
     enforced on loopback too, so nothing ever depends on 64 KB loopback datagrams.
   - Explicit connection lifecycle: `Connecting → Connected → Draining → Closed`, with reason
     codes. No API implies zero loss, zero reorder, or zero latency — loopback is treated as a
     network.
   - **Threading contract (binds ADR-007):** implementations may enqueue completions from any
     internal thread; delivery to the application happens only when the owning thread calls
     `Poll()`, which drains an internal MPSC queue and invokes handlers inline. Sends are
     callable from the owning thread only. This contract fits both a polled non-blocking
     socket and msquic's worker-thread callbacks.
   - `Stats()`: RTT, loss %, jitter, bytes/s — feeds the HUD NET readout (`tactical-hud.png`)
     and debug Tier 1.
2. **MVP implementation: `UdpTransport`** (Winsock2, non-blocking, single socket per side) with
   the minimal control-channel reliability described above and a 3-way hello for "connection".
3. **msquic is validated by an early spike, not deferred to the end.** Build-order slice S13
   (before MVP-complete, after the protocol stabilises) brings up `QuicTransport` behind the
   same interface: ALPN **`opf/1`**, self-signed server cert created
   in-memory via `CertCreateSelfSignCertificate` + `QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT`
   (the Schannel-flavour package cannot load PEM/PKCS12 files), client using
   `NO_CERTIFICATE_VALIDATION` on loopback (pinning comes with real deployment, out of MVP).
   Datagrams enabled via `DatagramReceiveEnabled`; the reliable channel is stream 0 opened by
   the client. The spike's exit criterion: the full MVP protocol runs unmodified over QUIC.
   *(As originally accepted this read "over both transports, toggled by flag" — superseded by
   the S13 amendment below.)*
4. **QUIC is the only transport** *(S13 amendment, owner directive — as accepted this
   paragraph shipped both transports permanently with UDP as the plaintext fallback)*.
   `UdpTransport` was the S4–S12 scaffolding and was deleted when `QuicTransport` landed;
   there is no transport config knob. What the UDP era bought stands: the seam was validated
   against two implementations before the second became the only one, which is why the swap
   touched one type name in `ServerHost` and one in `ClientConnection` and nothing else.
   CI-style self-test (`selfTest`) runs the full handshake+order+snapshot loop over QUIC.

## Alternatives rejected

- **msquic from day one** — front-loads cert/ALPN/callback friction into the first slices,
  slows game iteration, and adds nothing on loopback the abstraction doesn't preserve.
  Rejected on sequencing, not on merit; the spike lands well before MVP-complete.
- **Raw UDP with ad-hoc reliability as the product path** — re-implements QUIC badly over
  time (congestion control, PMTU, encryption, NAT keep-alive). Rejected; msquic is the target.
- **Shared-memory / in-proc function-call channel** — forbidden by the brief; also erases the
  packaging-change guarantee. Not considered.
- **Adopting the OS inbox msquic.dll** — Schannel-only *and* OS-versioned with no app-servicing
  story (brief already rules it out). NuGet package stands.

## Prior art worth reading first

The sibling repository **Outpost.Warzone** already ships msquic behind a `Neuron::Transport`
seam, with `NeuronCore/HostCertificate.{h,cpp}` doing exactly the self-signed-certificate job
§3 describes (no CA, no name to certify, clients told not to validate) and `tools/stubs/`
carrying msquic header stubs for non-Windows checking. Read those before writing
`QuicTransport` — the naming convention is shared (AGENTS.md §1), so the code is close to
liftable. It lowers R3 from "integration unknown" to "port a working integration"; the spike
(S13) stays scheduled, because their transport is peer-to-peer session-oriented and ours is
client/server with datagram state, so the *shape* differs even where the plumbing does not.

## Consequences

- First heartbeat lands in days (UDP), and the QUIC semantics are locked from day one, so the
  later swap is mechanical. The one place UDP is *more* permissive (no handshake crypto cost)
  is invisible to callers.
- Snapshots must fit 1,152-byte datagrams or fragment at the protocol layer — drives ADR-004's
  size budget and, at the 1,024-entity cap, forces delta+interest (planned there).
- Schannel-flavour msquic requires the QUIC TLS stack in Windows (Win11/Server 2022+). Dev
  targets satisfy this; if a shipping constraint ever demands older Windows, the OpenSSL
  flavour of the *same* msquic NuGet family is the swap — flagged in the risk register, and
  treated as within the existing msquic approval **pending owner confirmation**.
- The dumb UDP reliability layer is bounded, documented throwaway; no feature may grow on it.
