# ADR-022 — Remote Play: Pinned Trust, the Transport's Config Surface, and a First Abuse Posture

**Status:** Accepted · 2026-08-19 (design deliverable [ADR-018](ADR-018-scaling-baseline.md)
A22 — **blocks the first remote deployment**)
**Depends on:** ADR-003 (transport, msquic/Schannel, the loopback posture), ADR-004
(handshake, `Hello`/`Welcome`, the fail-closed schema hash), ADR-007 (single-writer, the
transport as the only crossing), ADR-008 §8 (the packaging split), ADR-012 (JSON
configuration and the user layer), ADR-014 (the engine/game seam), ADR-018 (**D5** durable
identity and the reserved token/resume field, **D10** this ADR's parameters), ADR-019 §5
(the session front door)
**Amends:** ADR-003 §1 (the seam grows a **config surface** — `Listen`/`Connect` take
descriptors, §3) and §3 (client validation stops being unconditionally off — §2c);
ADR-008 §8 ("no architectural work remains by construction" acquires this gate — §7);
ADR-012 §3 (the config schema grows the server credential and client validation families —
§3b)
**Feeds:** the first remote-client milestone; the account service (which this ADR consumes
and does not design); R20 (device removal, whose "relaunch and reconnect" posture assumes
§5's reconnect is first-class)

## Context

Everything internet-facing was deferred, correctly, for the MVP. ADR-003 §3 resolved
encryption *for loopback*: an in-memory self-signed certificate, the client told not to
validate, the listener hard-bound to 127.0.0.1 so nothing widens by accident. Encryption is
genuinely on and has been since S13.

What the review found (NET-4) is not that the remaining work is large — it is that **ADR-008
§8 describes the split as already complete**, so the first remote deployment would inherit the
loopback defaults by default rather than by decision: validation off (MITM-able by
construction), no authentication beyond a display name, and an order path where any
post-handshake connection may submit at whatever rate it likes. Three concrete artefacts of
that posture are in the tree today and are named here so the fixes are checkable:
`QuicTransport` turns client validation off *unconditionally* — there is no knob to turn it
back on; a pre-join `Ping` is answered; and a second `Hello` on one connection adds a second
session row, so a single client can pin slots until the server reports full.

ADR-018 D10 fixed this ADR's inputs before it was written: **hosted servers only** for the
foreseeable roadmap, trust by **pinned key shipped with the build**, client validation **on by
default off-loopback**, a transport config surface, a first-order abuse posture, and the
platform floor recorded as **Windows 11 / Server 2022+**. What remains is to turn those into
shapes.

**Scope discipline.** This is a *remote play* ADR, not a security programme. It decides how a
client knows it is talking to the real shard, how the shard knows who a client is, what a
well-formed but hostile client may do per second, and where each of those lives. It does not
design the account service, and it is not an anti-cheat document — the authoritative-server
property (ADR-005, ADR-004 §7) is the anti-cheat story and it already holds.

## Decision

### 1. Operation model: hosted service only, and what that lets us not build

**Hosted servers only** (D10). Player-hosted dedicated servers are a **future decision, not a
latent promise** — nothing in this ADR is shaped to keep them cheap, and anyone reopening them
should expect to reopen §2 with it.

That single answer is what makes the rest small. Because every server is one we run:

- Trust can be a **pinned key** (§2) rather than a PKI, because the build and the server ship
  from the same place — exactly the relationship the schema hash already assumes.
- Accounts are a **service dependency**, not a thing the binary must work without.
- There is no "community server" identity problem, no certificate provisioning story for
  third parties, and no trust-on-first-use dialogue to design.

### 2. Trust: the client pins the shard's key

**2a. The pin is a build constant, not configuration.** The client ships with the SPKI hash
(SHA-256 over the certificate's `SubjectPublicKeyInfo`) of the shard's key, compiled in beside
the schema hash and treated the same way: *the build knows what it speaks and who it speaks
to.* It is deliberately **not** a JSON key. A trust anchor a user can edit is not a trust
anchor, and ADR-012's user layer is explicitly a place for preferences — putting the pin there
would make "point me at a different server" a settings change.

**2b. Two pins, always.** The client carries a **current** and a **next** pin and accepts
either. Key rotation then does not require the whole population to update in the same hour:
publish clients carrying (current, next), rotate the server to `next`, and the following
release rolls the pair forward. A single-pin design turns every rotation into an outage, which
is how pinning gets abandoned.

**2c. Validation policy, and where "off" is still legal.** `ValidationPolicy` is an explicit
enum on the client's connect descriptor (§3):

| Policy | Meaning | Where it is legal |
|---|---|---|
| `PinnedKey` | Presented cert's SPKI hash must match a pin | **The default. Required off-loopback.** |
| `LoopbackOnly` | No validation | Only when the peer address is loopback |

The rule is mechanical rather than advisory: `Connect` **refuses** `LoopbackOnly` against a
non-loopback address. That keeps `selfTest` and the in-process host exactly as they are — they
connect to 127.0.0.1 and nothing about their setup changes — while making the dangerous
combination unrepresentable rather than merely discouraged. This is what turns ADR-003 §3's
"pinning comes with real deployment" from a note into a property.

**2d. A pin mismatch is a fail-closed refusal with a name.** It joins `UpdateRequired` on the
existing philosophy: a client that cannot verify the shard does not connect degraded, it
**stops** and says why. `Refuse{ServerNotTrusted}`. The player-facing text is the session
surfaces' business; the decision here is that there is no "connect anyway".

### 3. The transport's config surface — the interface change made once

**3a. `Listen`/`Connect` take descriptors.** Today the seam is `Listen(u16 port)` and
`Connect(host, port)`, and every remote concern — bind address, credential source, validation
policy — has nowhere to go. NET-4's point is that this is an *interface* change and interface
changes are cheapest taken once, so it happens here rather than dribbling in over three
slices:

```
struct ListenDesc
{
  std::string bindAddress = "127.0.0.1";   // the safe default stays the default
  std::uint16_t port = 0;
  CredentialSource credential = CredentialSource::SelfSignedInMemory;
  std::string credentialSubject;           // store lookup, when the source is the store
};

struct ConnectDesc
{
  std::string host;
  std::uint16_t port = 0;
  ValidationPolicy validation = ValidationPolicy::PinnedKey;
};
```

The defaults are the current behaviour: bind loopback, self-signed in memory. **A deployment
widens the bind address deliberately**, and the widening is one field in one file that a
person had to write.

**3b. Configuration is JSON, per ADR-012 — and the pin is not in it.** The config schema grows
two families:

```
"server": { "bindAddress": "0.0.0.0", "port": 7777,
            "credential": { "source": "store", "subject": "shard.example" } }
"client": { "serverHost": "shard.example", "serverPort": 7777 }
```

`ValidationPolicy` is not a config key at all: it is derived — loopback host ⇒ loopback is
legal, anything else ⇒ pinned. There is nothing to misconfigure because there is nothing to
configure.

**3c. The Schannel flavour stands.** Dependency-Map §7 flags the OpenSSL-flavour swap
(older-Windows support, certificate *file* loading) as owner-sign-off work. D10's platform
floor (§6) removes the older-Windows motive, and `CredentialSource::Store` removes the
file-loading one — a hosted server takes its certificate from the Windows certificate store,
which is where a real deployment's certificate lives anyway. The swap stays a listed
possibility, not a plan.

### 4. `Authenticate`: the token's place in the handshake

**4a. It rides D5's field, and D5 already reserved it.** `Hello` grows
`{ token, resumeToken }` in the T2 identity cluster (ADR-018 D5/A12); this ADR says what fills
them. The token is an **opaque bearer credential** obtained out of band from the account
service. The session front door (ADR-019 §5) validates it *before* `Welcome`, resolves it to a
`PlayerId`, and only then does a session exist.

**4b. Authentication is engine, not game.** It is about connections and identity, not about
the simulation, so it lives in NeuronServer's session front door and **the game never sees a
token**. GameLogic learns a `PlayerId` and nothing else — which is the ADR-014 seam working
exactly as designed, and the reason this ADR adds no GameLogic surface at all.

**4c. Refusals are named, and they are transport-layer refusals.**
`Refuse{AuthRequired}` (no token where one is needed), `Refuse{AuthFailed}` (token rejected),
`Refuse{ServerNotTrusted}` (§2d, client-side). These are `Neuron` reason codes on the
handshake path, deliberately **not** members of the game's `OrderReason` enum: a game verdict
is a statement about a game rule, and "your token expired" is not one. Keeping the two
vocabularies apart is what stops the wire's reason codes from becoming a junk drawer.

**4d. Resume is the same door.** A reconnect inside D5's grace window presents the same token
plus the `resumeToken` naming the session to reattach to. On success the session host rebinds
the existing `PlayerId` session — subscriptions, order high-water, view — to the new
connection; on failure it is an ordinary join. This is the reconnect print's RESUME made
concrete, and it is also what R20's "relaunch and reconnect" device-removal posture spends.

### 5. Abuse posture: budgets, named refusals, and one session per player

Not a threat model — a first-order posture, sized to the corpus's own phrase that
"registration's abuse posture is a security design, not a layout". Four rules, each fixing
something that exists in the tree today:

**5a. The pre-join budget.** An accepted connection has **5 seconds and 8 messages** to
complete the handshake, and may send *nothing but* `Hello` before it does. This closes the
answered pre-join `Ping`: an unauthenticated peer gets no work from the server beyond parsing
one message.

**5b. One session per connection.** A second `Hello` on a live connection is a protocol
error — `Goodbye{ProtocolError}` and close — not a second session row. That is the
slot-pinning bug, removed by making the duplicate meaningless rather than by counting it.

**5c. Sessions key on `PlayerId`, not on connection** (D5). A new authenticated connection for
a `PlayerId` that already has a session **takes over** that session (§4d's resume path);
it never adds a second one. So opening N connections cannot consume N slots, and
`maxSessions` finally counts what its name says: players.

**5d. A per-session message budget, with a named refusal.** Orders are click-driven and the
tick is 20 Hz, so **32 control messages per second per session** is generous by an order of
magnitude against real play and still bounds the validation CPU and ack traffic an attacker
can price. Over budget, the session is refused further control messages for the remainder of
the second and the event is counted; sustained abuse ends the session with
`Goodbye{RateLimited}`. Datagrams are not budgeted — they are unreliable, unacked and cheap to
drop, and QUIC's own flow control is a better answer than one we would write.

The counters (`preJoinTimeouts`, `duplicateHellos`, `rateLimited`, `sessionTakeovers`) are
ordinary telemetry, on the existing rails. A posture nobody can see the effects of is a
posture nobody can tune.

### 6. The platform floor, recorded

**Windows 11 / Windows Server 2022 or later** (D10). Schannel-flavour msquic requires it for
the QUIC support this design assumes, and recording the floor is what makes §3c's "the
OpenSSL swap is not a plan" an argument rather than an assertion. It is a *floor*, not a
target: nothing here needs a newer API than the tree already uses.

### 7. ADR-008 §8's claim, corrected

ADR-008 §8 says the client-side split is "`mode: "client"` with a remote `client.connect` +
QUIC transport + real cert validation; **no architectural work remains by construction**."

The first three clauses are true. The last one was true of the *packaging* and was read as
true of the deployment. It is amended to: **the packaging split is architecture-complete and
gated on this ADR.** Concretely, the first remote deployment owes the descriptor surface
(§3a), the pin and its check (§2), the token step (§4), and the four budget rules (§5) — none
of them large, all of them absent, and none of them things to discover on the day a server
first gets a public address.

### 8. What this deliberately does not do

- **It does not design the account service.** Credential storage, registration, password
  policy and the token's own format are that service's business; this ADR consumes an opaque
  token and resolves it to a `PlayerId`.
- **It is not a DDoS posture.** Volumetric defence is infrastructure — that is one of the
  things "hosted only" buys, and putting a rate limiter in `ServerHost` would not change it.
- **It is not anti-cheat.** The server is authoritative and validates every order with the
  same function the client pre-checks with; a lying client changes nothing but its own
  display. Wallhack-class information leakage is a real and separate topic, and ADR-021's
  interest culling is where it will be argued, because that is where the server decides what a
  client is told.
- **It does not do certificate transparency, OCSP, or revocation.** Pinning with a rotation
  pair is the whole trust design; revocation under pinning is a client update.
- **It does not open player-hosted servers** (§1).

## Alternatives rejected

- **Real PKI (a public CA chain).** Correct for a system with many independent operators, and
  pure cost for one with a single operator: it adds a renewal dependency, a chain-validation
  failure mode, and trust in a third party, to solve a problem — "which of the many servers is
  this?" — that D10's answer means we do not have.
- **Trust on first use.** Requires a decision from the player at exactly the moment they have
  the least information, and the corpus has no surface for it. Pinning moves the decision to
  the build, where it belongs.
- **The pin as a config key.** Would make "connect to something else" a supported operation by
  accident, and would let anything that can write the user layer silently retarget the client.
- **Validation policy as a config key.** Same failure with a smaller blast radius; §3b derives
  it from the address instead, so the insecure combination cannot be spelled.
- **Authentication inside GameLogic**, so the simulation could reason about accounts. Breaks
  ADR-014 for no gain: the game needs a `PlayerId`, and a token is not one.
- **A single reusable reason enum for game and transport refusals.** Cheap today, and it would
  put "your session was rate limited" next to "that target is out of bounds" in a taxonomy the
  client renders as gameplay feedback (ADR-004 §7's bounce grammar).
- **Budgeting datagrams alongside control messages** (§5d). QUIC already governs them, they
  are droppable by definition, and a budget there would mostly add a way to throttle a
  legitimate client on a bad link.

## Consequences

- **`Transport` changes shape once**: `Listen(const ListenDesc&)` and
  `Connect(const ConnectDesc&)`, plus `ValidationPolicy` and `CredentialSource`. Both
  implementations and every call site move together, and the defaults keep today's behaviour
  exactly.
- **`QuicTransport` gains** the pin check on the client credential path (replacing the
  unconditional `NO_CERTIFICATE_VALIDATION`) and store-sourced credentials on the listener.
  The self-signed in-memory path stays for loopback.
- **The handshake grows** `token`/`resumeToken` in `Hello` — already reserved by D5 and
  already scheduled in T2's cluster, so this ADR adds a *meaning*, not a schema bump.
- **`ServerHost`'s session table re-keys onto `PlayerId`** (§5c), which is D5's rule arriving
  where it bites: `maxSessions` starts counting players.
- **ADR-008 §8 no longer claims completeness** (§7), and the first remote milestone has a
  named list of what it owes.
- **`ClientConfig`/`ServerConfig` and the JSON schema** grow §3b's families, under ADR-012's
  unknown-key tolerance.
- **The account service becomes a named external dependency** of the first remote milestone —
  the first thing in this project that is, which is worth noticing before it is on the
  critical path.
