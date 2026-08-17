# ADR-010 — Math: DirectXMath Used Natively, No Wrapper Layer

**Status:** Accepted · 2026-08-17 (owner directive)
**Depends on:** ADR-001 (planar sim), ADR-005 (determinism), ADR-006 (renderer)
**Supersedes:** the planned `NeuronCore/Math.h` (`float2/3/4`, `mat4`) in the Dependency Map —
that header is deleted from the design and is not to be written.

## Context

Owner directive: **maths is DirectXMath, used natively — no wrapper classes, no wrapper
functions.** DirectXMath ships in the Windows SDK, is header-only, SIMD-accelerated, and is
the native math vocabulary of D3D12 and X3DAudio, so it costs nothing under the library policy
and removes a hand-written layer the project would otherwise own, test, and debug.

The temptation this ADR forecloses: every C++ game codebase grows a `Vec2`/`Mat4` "convenience"
layer over its math library. It always starts as three operators and ends as a second math API
with its own bugs, its own conversion noise at every DirectXMath boundary, and documentation
nobody wrote. We are not doing that.

## Decision

1. **DirectXMath is the math API.** Include `<DirectXMath.h>` (and `<DirectXCollision.h>` when
   culling arrives) and call `XM*` functions at the use site. **No wrapper types, no wrapper
   functions, no aliases** — no `using float2 = XMFLOAT2`, no `operator*` of our own, no
   `Normalize(v)` forwarding to `XMVector2Normalize`. Types are called by their real names, so
   every DirectXMath sample, doc page, and Stack Overflow answer applies verbatim.
2. **NeuronCore has no math header.** The library keeps `Assert/Log/Time/Hash/Random`,
   containers, tasking, telemetry, byte IO, JSON, and transport — math is the toolchain's.
3. **Storage vs computation (normative):**
   - **Stored** in structs, arrays, constant buffers, and any long-lived state:
     `XMFLOAT2` (planar sim position/velocity), `XMFLOAT3`/`XMFLOAT4` (render/audio positions),
     `XMFLOAT4X4` (matrices), scalars for heading. These have defined layout and no alignment
     demands.
   - **Computed** with `XMVECTOR`/`XMMATRIX` as locals and parameters only — loaded via
     `XMLoadFloat2/3/4`, stored back via `XMStoreFloat2/3/4`. An `XMVECTOR` is **never** a
     member of a stored struct or an element of a container (16-byte alignment, ABI traps).
   - Functions taking vectors by value use `XM_CALLCONV` with the `FXMVECTOR`/`GXMVECTOR`/
     `HXMVECTOR`/`CXMVECTOR` conventions in the documented order. That *is* using DirectXMath
     natively; ignoring it is the usual source of x64 codegen surprises.
4. **Planar work uses the 2D entry points** — `XMVector2Length`, `XMVector2Normalize`,
   `XMVector2Dot`, `XMVector2AngleBetweenVectors` — on vectors loaded from `XMFLOAT2`
   (z/w zeroed by `XMLoadFloat2`). Heading stays a scalar radian (ADR-001); no quaternions in
   the sim. `XMScalarSinCos` for heading→direction, `XMScalarModAngle` for wrap.
5. **The renderer and camera use the library's own solutions** rather than hand-rolled
   equivalents: `XMMatrixOrthographicOffCenterRH` / `XMMatrixLookAtRH` for the iso camera
   (ADR-006), `XMMatrixAffineTransformation` or explicit `XMMatrixRotationY` × translation for
   instances, `XMPlaneIntersectLine` for cursor-ray ∩ ground-plane picking, `XMVectorLerp` for
   snapshot interpolation, `BoundingFrustum`/`BoundingBox` (DirectXCollision) when culling
   lands. If a genuinely missing operation appears, it is written as a **local free function in
   the one file that needs it**, with a comment saying why DirectXMath has no equivalent —
   never as a shared "math utils" header, which is the wrapper layer under another name.
6. **Determinism rules (binding with ADR-005):**
   - DirectXMath selects its implementation **at compile time** (`_XM_SSE_INTRINSICS_`,
     `_XM_AVX2_INTRINSICS_`, `_XM_NO_INTRINSICS_`), so a given binary has exactly one code
     path — consistent with the same-binary replay-determinism scope. Do not build GameLogic
     with a different `/arch` setting than the rest of the solution; leave the default x64
     SSE2 baseline unless a measured need changes it solution-wide.
   - **Estimate functions are forbidden in GameLogic** (`XMVector*Est`, `XMVectorReciprocalEst`,
     `XMVectorSinEst`, …): their accuracy is explicitly instruction-set dependent. They remain
     legal in NeuronClient presentation code, which nothing replays.
   - Trig/normalisation in the sim uses the exact variants; they are polynomial approximations,
     which is fine — deterministic is the requirement, not IEEE-perfect.
   - `XMVerifyCPUSupport()` is called once at startup and refuses to run on an unsupported CPU
     rather than producing quietly wrong math.
7. **DirectXMath types never cross the wire.** The wire stays quantised integers (ADR-004);
   conversion happens in the snapshot emit/apply functions. Same for config: JSON carries
   numbers, not vector types (ADR-012).

## Dependency ruling

`GameLogic depends only on NeuronCore` is a rule about **our libraries**. DirectXMath is a
header-only, OS-free part of the toolchain's SDK — it introduces no Windows coupling, no
linkage, and no game semantics. **GameLogic may include DirectXMath directly**, and does.
Recorded here so the rule is not silently reinterpreted later.

## Alternatives rejected

- **A thin wrapper layer** (`Vec2`, operators, `Length()`) — the directive forbids it, and it
  is the right call: two math vocabularies, conversion noise at every D3D12/X3DAudio boundary,
  and a bug surface nobody else has already debugged. Rejected.
- **Type aliases only** (`using float2 = XMFLOAT2`) — cosmetic, still a rename that breaks
  the "docs apply verbatim" property, and invites the operators to follow. Rejected.
- **Hand-rolled SoA float arrays for the sim** (x[], y[] separately) — better raw SIMD shape,
  but abandons DirectXMath's load/store idiom and readable call sites for a sim that is
  microseconds at MVP scale. Revisit only with a profile at the 1,024-entity cap, and behind
  the replay-determinism gate. Rejected for now.
- **`_XM_NO_INTRINSICS_` for "safer" determinism** — the scalar path is deterministic *and*
  slow; a single binary is already single-path. Pointless cost. Rejected.

## Consequences

- One math vocabulary across sim, renderer, and audio; D3D12 and X3DAudio structures take our
  stored types directly (`XMFLOAT3` into `X3DAUDIO_EMITTER.Position`, `XMFLOAT4X4` into
  constant buffers) with no adaptation layer.
- Call sites are more verbose than operator math (`XMStoreFloat2(&p, XMVectorAdd(XMLoadFloat2(&p), v))`).
  Accepted deliberately: verbosity at the call site buys the absence of a library we'd own.
  Where a routine does heavy vector work, it loads once, computes in `XMVECTOR`, stores once —
  which is also the fast shape.
- The storage/computation split must be reviewed for: no `XMVECTOR` members, no
  `std::vector<XMVECTOR>`. Cheap to check, expensive to discover late.
- `GameLogicTests` movement-envelope and replay suites now also pin DirectXMath usage: a change
  from an exact to an `Est` function breaks replay equality, which is exactly the alarm wanted.
