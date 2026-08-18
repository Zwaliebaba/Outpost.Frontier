# ADR-006 — Renderer: Fixed Forward Pass List, Orthographic 30°, Atlas Text

**Status:** Accepted · 2026-08-17
**Depends on:** ADR-001 (plane), ADR-002 (interpolation), ADR-005 (extract source)
**Feeds:** Build Order S1/S5/S8/S11

## Context

DX12, raw Win32 + DXGI flip model (fixed constraints). Look: Darwinia — flat-shaded low-poly,
strong silhouettes, emissive accents, sparse space. The corpus already draws a *target* frame:
`CullClear → GpuCull → DepthPre → Opaque → Effects → Nebula → Tonemap → Overlay → Ui → Present`
(`overlay-pass.png`), with the Overlay node post-tonemap reading depth as SRV, and an icon/
de-clutter system scaled for 1,024 entities. The MVP must be an honest subset of that frame,
not a different one. Assets: 9 OBJ meshes (per-face normals, triangulated, 5 shared materials
`hull/plate/glass/accent/thruster`, Y-up, forward = −Z, ~270–1,800 faces).

## Decision

### Frame structure
1. **No frame graph.** A frame graph schedules resource churn across many passes; MVP has four.
   Instead: a **fixed, named pass list** — `Clear → Opaque → OverlayWorld → Ui → Present` —
   each pass a struct with `Record(ctx)`, executed in order on one direct queue. The names and
   order are the corpus target list with unbuilt nodes absent; `GpuCull`, `DepthPre`,
   `Effects`, `Nebula`, `Tonemap` are **reserved slots** documented in code, so growth is
   insertion, not redesign. Revisit a real graph only when transient-resource management hurts.
   *`Nebula` is an ambient haze* — it sits after `Opaque` and before `Tonemap` because it
   composites over the scene, and it is the green field `tactical-hud.png` shows behind the
   fleet (hence `overlay-pass.png` §1 on "HDR drift between a bright nebula and empty space").
   It is **not** a celestial or skybox renderer, and no reserved slot here is: the prints draw
   no celestial body on either screen, and ADR-009 §9a settles that they are data rather than
   geometry.
2. **SDR first.** Swapchain `R8G8B8A8_UNORM` (flip model forbids `_SRGB` swapchain formats)
   with an `_SRGB` RTV; linear-space lighting; no tonemap node yet. The Darwinia look —
   near-black space, saturated emissives — survives SDR; HDR+Tonemap is a reserved node.
   D32 depth buffer, written by Opaque, read (test-only, small bias) by OverlayWorld.

### Camera
3. **Orthographic**, not narrow-FOV perspective. On a planar sim, perspective buys parallax
   depth cues we don't need and costs: zoom becomes dolly management, icon/ring screen-size
   varies across the view, and picking loses its uniform-direction ray. Ortho keeps scale
   uniform (tonnage stays readable — the icon sheet's size channel), makes zoom one number
   (ortho half-height), and matches the prints.
3a. **The tree is left-handed, and every handedness-parameterised entry point takes the `LH`
   form.** This is normative and it is a standing rule, not a per-call choice. Camera and
   picking use DirectXMath directly (ADR-010): `XMMatrixOrthographicOffCenterLH` +
   `XMMatrixLookAtLH` for view/projection, `XMPlaneIntersectLine` for the cursor-ray ∩
   ground-plane test, `XMVectorLerp` for snapshot interpolation. Matrices are stored as
   `XMFLOAT4X4`, instance positions as `XMFLOAT3`.

   **Why left-handed.** Two reasons, and the second is the load-bearing one:

   1. ADR-001 §3 fixes render space as `world = (sim.x, h_cosmetic, sim.y)` with `+Y` up — so
      `+X` is east, `+Y` is up, `+Z` is north. That triad *is* left-handed: geographic
      East-North-Up is right-handed, and putting **up** between the two plane axes swaps a
      pair. So the basis was never a choice this ADR got to make; it follows from ADR-001.
   2. Direct3D is itself documented as left-handed, and the SDKs we are committed to agree:
      every `LH`/`RH` pair in DirectXMath has an `LH` member, DirectXCollision's frustum
      defaults to left-handed, D3D12's default rasteriser state matches left-handed winding,
      and X3DAudio is left-handed with no option. Standardising on `LH` therefore means
      **taking every default and writing no conversion anywhere**.

   **The rule, concretely.** Wherever an SDK asks which handedness it is dealing with, the
   answer is *left*:

   | Where | The left-handed form |
   |---|---|
   | View / projection (DirectXMath) | `XMMatrixLookAtLH`, `XMMatrixOrthographicOffCenterLH` — and the `LH` member of every other pair, including the `Perspective*` ones if a second camera mode ever wants them |
   | Rasteriser winding (D3D12) | front faces come out clockwise in NDC ⇒ `FrontCounterClockwise = FALSE`, which is D3D12's own default |
   | Frustum culling (DirectXCollision, reserved) | `BoundingFrustum::CreateFromMatrix(…, rhcoords: false)` — also the default |
   | 3D audio (X3DAudio, ADR-011) | native. Right-handed would mean negating `.z` on every listener and emitter `Position`, `Velocity`, `OrientFront` and `OrientTop`, on every update, forever |
   | Derived geometry (mesh normals, overlay rings, formation ticks) | cross products follow the authored winding; `ObjMesh`'s missing-normal fallback is asserted against the corpus's authored normals rather than assumed |

   The audio row is the one that pays for the rest. A right-handed tree owes X3DAudio a
   negation pass on four fields of two structures every time the listener moves, and forgetting
   it on one field is a sound arriving from the wrong side of the player — a bug that is
   audible, intermittent, and almost impossible to attribute.

   **How it was found**, recorded because the failure mode is invisible by inspection. This ADR
   originally named the `RH` variants and nobody noticed, because an `RH` view matrix over a
   left-handed world does not fail — it *mirrors*. With the camera south of the focus looking
   north, east projected to the **left** of the screen: a map that reads backwards, in a game
   whose whole interaction model is reading a map. It surfaced in slice S5 only by projecting a
   viewport corner through the real matrices in a test. `NeuronClientTests`'
   `EastIsOnTheRightOfTheScreen` is the guard that keeps it surfaced, and the rasteriser
   winding above was measured against the shipped meshes rather than reasoned about, for the
   same reason. Nothing about depth changes: `LH` clips to `[0,1]` exactly as `RH` does.

4. **Elevation fixed at 30°**; this is normative because the corpus specifies selection rings
   as **2:1 plane ellipses** — an ortho ground circle projects at `sin(elevation)`, and
   `sin 30° = 0.5`. Yaw: free orbit about the focus point with 45° snap detents. Zoom:
   half-height 0.5–40 km (tactical tier; Operational/Strategic tiers are post-MVP camera
   *modes*, not new cameras). Pan: edge/drag on the plane. All camera state is client-only.

### Meshes & materials
5. **Hand-rolled OBJ/MTL loader** in NeuronClient at boot (no external lib; format is ours).
   Faces sorted by material into **submesh ranges**; one static VB/IB per class (positions +
   normals as authored — the exporter has already duplicated the corners that need it, so
   plain vertex normals shade flat with no shader work).
   *Measured, because the original wording claimed more than the content delivers:* the corpus
   is **mostly** flat-shaded, not entirely. 152 of `Structure.obj`'s 1,784 faces carry a
   different normal per corner, around a curved section, and a handful of faces in four other
   meshes do the same. Nothing needs to change for this — the loader keys a vertex on
   (position, normal), so a smooth corner simply becomes its own vertex and interpolates —
   but "every face is flat" is not a property anything may rely on.
6. **One opaque PSO.** Per-draw: instanced per ship class; per-instance data =
   `InstanceRecord{ XMFLOAT3 posWorld, float heading, u8 teamColorId, u8 selectionAndLodBias,
   u16 classId }` (field names deliberately match the corpus). Per-submesh root constants pick
   one of the **5 canonical materials** (albedo, emissive strength — `accent`/`thruster` carry
   emissive; `glass` is just dark). Lighting: one fixed directional + hemispherical ambient.
   Cosmetic banking/hover (ADR-001) computed in Extract from replicated velocity/heading only.
7. **Selection tint/rings never recolour hulls** — relationship colour and selection live in
   the Overlay pass (rings, bars), per the icon sheet's channel separation.

### Overlay & UI
8. **OverlayWorld pass** adopts the corpus two-mechanism split now:
   *(A)* per-entity instanced marks — selection ellipses (world-space circles on the plane,
   screen-space minimum radius clamp per the sheet's scaling law), hull/shield bars;
   *(B)* client-authored draw list — order puck, formation footprint ticks, dashed ghost
   polylines with per-leg ETA labels. Plane-lying geometry depth-tests against hulls
   (hard test + bias in MVP; soft SRV occlusion is the reserved DepthPre upgrade);
   screen-facing marks (bars, labels) never occlude.
9. **Text = DirectWrite-baked glyph atlas** at boot (ASCII + box glyphs, one monospace face,
   2–3 sizes), rendered as instanced quads in the Ui pass. This keeps one graphics API in the
   frame. **Rejected:** D3D11On12/D2D interop — a second device, wrapped-resource sync, and
   the largest boilerplate item in the codebase for richer typography than the prints use.
10. **Ui pass** (screen-space quads + text): fleet roster, context bar, ability rack (visual
    stubs), top status row, toasts. Layout constants from the prints; UI scale is a multiplier
    from day one (settings sheet makes 0.8–1.6× a requirement).

### Picking
11. Client-side, against the **interpolated render world**: cursor → ortho ray → plane point;
    point-pick = nearest ship within `max(class pickRadius, screen-floor-in-world)`;
    box-select = ship pos inside the screen rect's plane parallelogram. No GPU picking, no
    server round-trip. (Same plane point feeds the order puck — one code path.)

### Plumbing
12. Two frames in flight; waitable swapchain (latency 1 waitable, 2 buffers min 3 recommended
    — use 3 buffers); per-frame command allocator + one direct list; linear upload ring for
    constants/instances; one shader-visible CBV/SRV heap (atlas + per-frame tables); root
    signature: frame CBV, pass CBV, draw root constants, one SRV table. Debug layer on in dev;
    `tick/frame/extract` timings feed the Tier-1 counters strip (`debug-hud.png`). No MSAA in
    first slices; a 4× MSAA offscreen target + resolve is a listed polish slice (flat-shaded
    silhouettes alias hard, and it's cheap here).

## Alternatives rejected

- **Frame graph now** — machinery without churn to schedule; would be speculative API design.
  Reserved-slot pass list preserves the migration path.
- **Narrow-FOV perspective isometric** — non-uniform scale fights the tonnage/size channel and
  the 2:1 ring spec; adds nothing on a plane. Rejected.
- **Billboarded sprite ships** — cheaper, but silhouettes and heading readability *are* the
  art direction; meshes are already low-poly. Rejected.
- **Runtime D2D/DWrite HUD (D3D11On12)** — see §9.
- **Per-pixel GPU picking** — readback latency and plumbing for a problem 2D maths solves
  exactly. Rejected.

## Consequences

- The whole MVP renderer is ~4 PSOs (opaque, overlay-instanced, overlay-polyline, ui/text) and
  no compute — DX12 boilerplate risk is bounded to device/swapchain/upload plumbing (Risk R4).
- Extract (ADR-007) is the only bridge from net-thread world to GPU data; `InstanceRecord`
  is its output format, already shaped for the corpus's later GPU-culled path.
- Camera/selection/overlay all assume the plane; if ADR-001 ever reopens, this ADR reopens.
- Atlas text caps typography (no shaping/i18n in MVP) — accepted; revisit at localisation.
