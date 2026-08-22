# ADR-006 — Renderer: Fixed Forward Pass List, Orthographic 30°, Atlas Text

**Status:** Accepted · 2026-08-17 · amended by [ADR-018](ADR-018-scaling-baseline.md)
(2026-08-19): §9 player-text charset + named i18n reopen (D15.1), §10 DPI-derived effective
scale + 1280×720 floor (D15.2), §12 shader toolchain → dxc/SM 6.x (D12), device-removed
posture = relaunch + the no-device-refs-in-session-state invariant (D13) · §§9–10 given
their mechanism by [ADR-020](ADR-020-ui-architecture.md) (2026-08-19): the effective-scale
formula (the 0.8–1.6 clamp applies to the *preference*, not the product), `WM_DPICHANGED`,
the atlas re-bake on a scale change, and the rule that a full-screen surface **skips** the
world passes rather than adding one
**Depends on:** ADR-001 (plane), ADR-002 (interpolation), ADR-005 (extract source)
**Feeds:** Build Order S1/S5/S8/S11

## Context

DX12, raw Win32 + DXGI flip model (fixed constraints). Look: Darwinia — flat-shaded low-poly,
strong silhouettes, emissive accents, sparse space. The corpus already draws a *target* frame:
`CullClear → GpuCull → DepthPre → Opaque → Effects → Nebula → Tonemap → Overlay → Ui → Present`
(`overlay-pass.png`), with the Overlay node post-tonemap reading depth as SRV, and an icon/
de-clutter system scaled for 1,024 entities. The MVP must be an honest subset of that frame,
not a different one. Assets: 9 OBJ meshes (per-face normals, triangulated, 5 shared materials
`hull/plate/glass/accent/thruster`, Y-up, forward = −Z, ~270–1,800 faces) — **10 since U4
added `Stargate.obj`** (1,144 faces), on the same five materials and the same palette; the
sixth its export carried was authored onto `accent`, whose colour it already was, because the
loader refuses a material outside the five and this shading model reads albedo only.

## Decision

### Frame structure
1. **No frame graph.** A frame graph schedules resource churn across many passes; MVP had five (seven since §1c
   and §6a).
   Instead: a **fixed, named pass list** — `Clear → UiWorld → Opaque → Nebula → Lamps →
   OverlayWorld → Ui → Present` — each pass a struct with `Record(ctx)`, executed in order on one
   direct queue. (`UiWorld` arrived after the MVP, §1c; `Lamps` with §6a, and it landed as the
   third insertion into the reserved list: one struct, one line in `RecordWorld`, and nothing
   else in the file moved.) The
   names and order are the corpus target list with unbuilt nodes absent; `GpuCull`, `DepthPre`,
   `Effects`, `Tonemap` are **reserved slots** documented in code, so growth is insertion, not
   redesign. Revisit a real graph only when transient-resource management hurts.

1a. **`Nebula` is built, and it was the reserved list's first test.** The claim in §1 — that a
   new node is an insertion rather than a redesign — was untested until something needed
   inserting. Adding it cost one struct in `GpuPasses.h`, one line in `GpuPassList::Record`,
   one PSO, and nothing else in the frame moved: no pass was reordered, no existing pass
   changed, and the root signature grew by one SRV and one sampler. The claim now has a
   measurement behind it.

   *What the node is:* an ambient field, sampled from a CPU-baked periodic tile
   (`NebulaField.h`) and added over the scene. It sits after `Opaque` and before `Tonemap`
   because it composites *over* the geometry — hulls read as being inside the cloud rather
   than pasted onto a backdrop — which is also why `overlay-pass.png` §1 worries about "HDR
   drift between a bright nebula and empty space". Additive and deliberately dim: the ceiling
   on how far it may wash out a hull is the readability budget §7 protects.

   *What it is not:* a celestial or skybox renderer, and no reserved slot here is one. Neither
   print draws a celestial body, and ADR-009 §9a settles that celestials are data rather than
   geometry. Reading `Nebula` as their scheduled home is a mistake this tree has already made
   once.

   *Why the field is baked on the CPU rather than evaluated in the shader:* so it can be
   tested. A procedural shader would put the only copy of the function where no test in this
   tree can reach it, and a second copy in C++ to test against would be two implementations
   free to disagree. `GlyphAtlas` set the pattern — bake at boot, upload once, let the shader
   sample — and `NeuronClientTests` now asserts the field's determinism, sparseness,
   smoothness and seamless wrap without a device.

   *Why the tile is periodic:* the pass covers an unbounded plane. Sizing one tile to the play
   area looks right until someone zooms out — `MAX_ZOOM_METRES` is a 40 km ortho half-height
   against a 40 km grid, so the camera sees well past the play area and a clamped tile would
   smear its edge texels over everything beyond. Periodicity removes the failure instead of
   sizing around it, and gives a test something to check.

   *Why it is world-anchored:* the pass reconstructs the plane position of each pixel from
   `IsoCamera::PlaneMappingForNdc` — an affine NDC→plane map, exact because the projection is
   orthographic — so panning moves the field through the world and orbiting turns it. Sampling
   in screen space would make it a smudge on the monitor, which is the one thing a background
   must never be. That mapping is round-tripped against the real view-projection in
   `NeuronClientTests`, for the same reason §3a's handedness defect needed a round trip: the
   failure is invisible to review.

1c. **`UiWorld` was the list's second insertion, and it tested a different claim.** `Nebula`
   proved a *new* node costs a struct and a line. `UiWorld` proved something the list had never
   been asked: that a node can be an **existing pass type recorded twice, into a different
   target, at a different point in the order**. It is a second `UiPass` instance — same struct,
   same shaders, its own instance buffer — recording into the world target before `Opaque`
   against a multisampled variant of the Ui pipeline.

   *Why the frame needed it:* the order ghost's lane is screen-space geometry that belongs
   **under** the hulls. Drawn in the `Ui` pass it crosses every ship it passes; drawn here, a
   ship standing on a lane covers it with its own silhouette. The alternative was a
   world-space lane in `OverlayWorld` sized by a radius somebody had to guess — §8's own
   argument for why the two-mechanism split exists, applied one node earlier.

   *What it cost:* one pipeline (`GpuPipelines::UiWorld`, multisampled where the screen-space
   `Ui` pipeline is single-sampled, because it draws into the MSAA world target §12 resolves),
   one member, one line in `RecordWorld`, and a `bool _worldLayer` on `UiPass::Record`. No
   pass was reordered and no existing pass changed — the same result `Nebula` got, from a
   different direction.

1b. **The clear colour is static.** `AnimatedClearColour` breathed between two blues so slice
   S1 could prove the loop ran with nothing else on screen. From this node onwards there is
   something else on screen, and a background that changes on its own competes with it. It is
   replaced by `SpaceClearColour` — near-black, faintly blue, linear values behind the `_SRGB`
   RTV — and is deliberately *not* configurable: what reads as the colour of space is the
   nebula, and that is configurable.
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
   `XMMatrixLookAtLH` for view/projection, and `XMVectorLerp` for snapshot interpolation.
   Matrices are stored as `XMFLOAT4X4`, instance positions as `XMFLOAT3`.

   *This section originally named `XMPlaneIntersectLine` for a cursor-ray ∩ ground-plane test.
   **S8 built picking and there is no ray** — see §11a. An orthographic projection over a plane
   makes the mapping affine, so a pixel becomes a plane point through three `float2`s and the
   intersection never has to be computed. The handedness rule is unaffected; one of its example
   call sites simply turned out not to exist.*

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
   u16 classId, f32 bank }` (field names deliberately match the corpus; `bank` was appended by
   S14 — appended, so every earlier field keeps the offset the input layout declares, the same
   discipline `UiInstance::axis` set). Per-submesh root constants pick
   one of the **5 canonical materials** (albedo, emissive strength — `accent`/`thruster` carry
   emissive; `glass` is just dark). Lighting: one fixed directional + hemispherical ambient —
   superseded by the three-term rig in the second amendment below.
   Cosmetic banking/hover (ADR-001) computed in Extract from replicated quantities only — and
   S14 recorded what "only" turned out to mean: the sim's velocity is always along its heading
   (the no-strafing rule), so a slip angle is identically zero and the bank derives from the
   **heading rate the client measures between its two bracketing snapshots**, normalised by the
   class's own turn-rate and speed limits (`Game::CosmeticBankRadians`). Hover is a per-class
   constant in the class table, cosmetic like `pickRadiusMetres`; the entity keeps the plane
   point, so rings and picking never learn about either.
7. **Selection tint/rings never recolour hulls** — relationship colour and selection live in
   the Overlay pass (rings, bars), per the icon sheet's channel separation.

> **Amendment, 2026-08-22 — a mesh is drawn at the size the class table says it is, and
> nothing used to make that true.**
>
> The owner reported that the station and the fleet read as very small, and asked for a camera
> that gave big ships presence. **It was not the camera.** Measured against
> `Game::SilhouetteRadiusMetres`, `Structure.obj` and `Stargate.obj` were within 1% of their
> rows — §10 of [ADR-016](ADR-016-procedural-universe-and-warp.md) wrote those rows *from*
> those two meshes — and **every flyable hull was between a quarter and a twelfth of the size
> its own row describes**. An Interceptor was a 3.8 m silhouette sitting inside a 53 m
> selection ring. No zoom fixes that: at `MIN_ZOOM_METRES` it is still six pixels, and the
> config had already spent the camera lever once (`zoomMetres` went 8,000 → 2,667 because "the
> fleet parked around the station read as a scatter of specks").
>
> **The rule.** A hull's drawn silhouette radius on the plane is its **contact radius × 1.25**.
> That ratio is measured rather than chosen: it is what the station (253 over 200) and the
> stargate (168 over 135) were authored at, so the fleet is now scaled to the two pieces of
> content this corpus already treats as correct. It has to be roughly that: contact is a circle
> *inside* the hull, and formation spacing is four contact radii, so a wider silhouette would
> put neighbouring ships in a formation through each other. At 1.25 the gap between two hulls at
> their own spacing is 3/8 of it, whatever the class.
>
> **Applied at load, not baked into the files.** `FitObjMeshToPlaneRadius` scales each mesh
> about its origin after parse and before upload, so a re-export at any scale still lands
> correctly and the rule is *stated* rather than being a property somebody has to remember not
> to break. It is the last moment the geometry is on the CPU and the cheapest place to do it:
> the scale is a per-class constant, so it costs no instance field, no input-layout change and
> nothing per frame. §6's `InstanceRecord` is untouched.
>
> **The seam holds.** `GpuMeshTable::Create` takes a span of plane radii in file order and has
> no idea which index is a Carrier (ADR-014); the composition root fills it from the class
> table. A zero means "as authored", which is what a file with no hull gets.
>
> **Chrome does not scale, and that is the point.** Selection rings come off `pickRadiusMetres`
> (`pickRadius + ringPadMetres`), lanes, pucks and station ticks are screen-space, and none of
> them reads this. `pickRadius` is deliberately generous for small hulls — an Interceptor's is
> 2.6 contact radii against a Battleship's 1.5 — so the ring stays outside the silhouette by
> the widest margin exactly where the scale factor is largest. A GameLogicTests case asserts
> both ceilings (clear in formation, inside its own ring) for every class.
>
> **`client.renderer.hullScale` is the dial**, defaulting to 1.0 — honest, and the size the sim
> already treats a hull as having. It is art direction, so it is content, the way the nebula
> block is. Above about 2 a formation starts overlapping itself, which is where the config
> comment and the parse range stop.
>
> Gated by `RunHullScaleGate` in the self test, which is the only place that can see both a mesh
> loader and a class table. The ratio itself is a `static_assert` against the two authored
> silhouettes: moving it fails the build rather than a test.

> **Amendment, 2026-08-22 — the rig is three terms, not two, because flat shading needs it to
> be.**
>
> §6 said "one fixed directional + hemispherical ambient", and that is what shipped. Against
> the adopted low-poly direction it produced hulls whose facets were nearly the same tone as
> each other and whose shadow sides were the same colour as the background — which is the
> failure mode flat shading has and smooth shading does not. A faceted hull is only
> three-dimensional if adjacent faces are lit *differently*; there is no gradient across a face
> to do the work.
>
> **Three terms, and each answers one of those.** All three are pass constants in
> `ClientApp::BuildFrameConstants`, quoted as fractions of the key so they are read together:
>
> - **Key** — one hard directional, warm white (1.0, 0.96, 0.90) at full intensity, from ~55°
>   of elevation. Cheated *above* the camera's 30° on purpose: under a top-down view the top
>   facets are most of what a hull shows, and a key nearer the horizon leaves them dark and the
>   scene reading as backlit. Fixed in world space, not attached to the camera — a hull's lit
>   side being a property of the *hull's* facing is what makes orbiting worth doing.
> - **Fill** — the hemispherical ambient §6 already had, retinted and raised to 28% of the key:
>   sky (0.87, 0.96, 0.75) above, near-black ground (0.06, 0.11, 0.06) below, lerped by the
>   normal's Y. Two constants and a lerp; it is what keeps an unkeyed face a shape.
> - **Bounce** — new. A weak directional at the accent hue (0.61, 0.94, 0.12), 17% of the key,
>   arriving from the camera's own side (`ScreenUpOnPlane` plus the camera's elevation). It is
>   the one term that moves when the view orbits, and it exists so a hull's shadow side has an
>   edge against a background that is nearly the same colour. Weak on purpose: louder and the
>   hulls read as painted green rather than as dark hulls with green markings.
>
> **A visible sun disc was proposed with this and rejected.** The sun exists as the key's
> direction and colour and as nothing else. Nor is it content: an earlier draft put a
> per-system sun direction and colour in the universe definition, which was dropped — the
> universe model carries no float by design (ADR-009), and light that nothing in `Tick` reads
> has no business in the file the content hash is taken over. The only art direction that stays
> content is the nebula block, which is the field the fleet sits inside rather than what lands
> on a hull.
>
> **Emissive materials take no key.** `accent` at 0.6 × albedo and `thruster` at 1.3 × albedo
> (down from 1.6 and 2.4, because the fill they sit against is now four times what it was), and
> the key term is switched off for any material with a non-zero emissive strength. A stripe lit
> *and* glowing would be brightest exactly where the hull already is, so the markings would
> come and go as a ship turned; unlit, they hold their colour at every heading, which is what
> makes them read as markings. Fill and bounce still land on them, so an engine bell keeps its
> own facets.
>
> Emissive strength stays a renderer decision and never enters the `.mtl` (§5's
> `MeshMaterialPalette` note). The committed palette is authored in **linear** floats — the RTV
> is `_SRGB`, so a `Kd` reaches the target unconverted — and
> `TheCorpusAuthorsTheCanonicalPaletteAndNotSomeOtherGreen` in `NeuronClientTests` pins the five
> values so that "correcting" them to sRGB fails a test rather than a review.
>
> **What this does not fix, and is not the renderer's to fix.** The corpus assigns most of a
> hull's faces to `plate` (412 of the Battleship's 664), whose design colour #5c6b55 is the
> light one of the five. So a capital reads sage-green overall, with `hull` #27332b appearing as
> the recesses and understructure rather than as the body. That is a material-assignment
> decision in the art, not a lighting one: measured off a capture, a fully keyed `plate` facet
> lands at #617354 against its #5c6b55 albedo, which is the material doing exactly what it says.
> Making a fleet read darker means moving faces from `plate` to `hull` in the meshes.

> **Amendment, 2026-08-22 — §6a, animated signal lamps.**
>
> Navigation lights on every hull, landing and hazard sequences on the station and
> the gate. Presentation only: nothing is replicated, nothing reaches `Tick`, no wire or schema
> field moved, and a lamp is invisible to picking and to selection-ring sizing because both read
> `SceneEntity`, which knows nothing about them.
>
> **Four colours, four modes.** Red (255, 60, 60), green (120, 255, 80), white (235, 255, 220),
> amber (255, 190, 70) — authored in sRGB in the design and converted **once**, in
> `SignalLamp.h`, to the linear floats the `_SRGB` target wants. Steady, strobe (8% duty), pulse
> (sinusoidal, floor 45%), chase (18% duty, floor 38%, phase = index/count so the flash travels).
> Brightness is opacity 0.18–0.80 plus a ±28% size swell; the floor is not zero, because an
> unlit lamp is still a fitting and one that vanishes reads as a hole in the hull.
>
> **A new pass, after Nebula and before OverlayWorld.** After the haze because a lamp is the
> brightest thing in the frame and nothing atmospheric may composite in front of it (the
> proposal's "fog-exempt"); before the overlay because the overlay is a readout. Additive
> ONE/ONE, depth-tested and never depth-writing, so a hull occludes its own far-side lamps while
> no lamp occludes another and none need sorting. The glow is **procedural** — a radial falloff
> with a blown-out core, two `pow`s in `LampPS` — rather than the sprite atlas the proposal
> sketched: a texture for it would be a bake to maintain, a descriptor to bind and a resolution
> to be wrong at some zoom, and the atlas earns its place the day a lamp stops being radially
> symmetric.
>
> **The cost argument, which is the reason for the shape.** The lamp table is *static* and
> hull-local — 59 lamps across the ten classes, one root CBV at `b3` written once at boot. The
> transform a lamp needs is the ship's, and the frame has already uploaded that: `LampPass` binds
> `OpaquePass::InstanceStream()` a second time rather than composing anything of its own. So the
> pass loops over **classes × lamps** and never over instances — one `DrawInstanced` per (class,
> lamp) pair, 49 draws for the shipped corpus whether the fleet is forty hulls or four hundred,
> and not one byte of new per-frame data. Measured at the shipped fleet, Release, vsync off: 0.45
> ms/frame without the pass and 0.46 ms with it, which is run-to-run noise.
>
> `InstanceRecord` grew a fourth appended field for this, `lampPhaseTurns` (24 → 28 bytes), and
> the growth is load-bearing rather than convenient: the table is per *class*, so without a
> per-hull offset every ship of a class would strobe on the same frame and a parked wing would
> flash as one object. It is hashed from the entity id — stable for a ship's whole life however
> the scene is sorted, and unlike a hash of position it does not drift as the ship moves.
> `FrameConstants` likewise gained `g_frameTime`, an **unsynced** wall clock: two clients showing
> the same fleet blinking differently is correct, and a replicated one would be a per-frame float
> on the wire for no gameplay at all.
>
> **Placement is derived, never authored — and the proposal's rule for it was wrong.** Every
> anchor is a fraction of something the loader measured: the hull's bounding box, or its
> silhouette radius on the plane (`GpuMesh::planeRadiusMetres`, carried separately from the
> sphere radius because the stargate's sphere is its spire and half again its footprint). The
> proposal put the navigation lamps "just above bbox top". On this corpus that is wrong by a wide
> margin — a `Battleship`'s box has its ceiling 100 m up because a thin antenna reaches there,
> while the deck out at the beam where the lamp goes is 37 m — so the wing lamps hung four
> lamp-widths clear of the hull in open space. Visibly wrong from every angle, and *invisible* to
> a "never inside geometry" check, which it passes with room to spare. So the nav lamps anchor to
> the deck measured **locally at that beam** (`LocalTopMetres`, sampled at load and carried on
> `GpuMesh::lampBounds` because that is the last moment the vertices exist on this side). Still
> derived, so a re-export still moves the lamps with the art; it is the same rule the proposal's
> own acceptance states ("above the deck line") rather than the one its implementation note did.
>
> **The acceptance that matters is a test, not a screenshot.** A lamp whose centre is inside a
> hull is either half-buried, which reads as an explosion, or gone entirely, because the depth
> test discards it against the hull it is bolted to — and neither is visible from the one angle
> somebody happened to capture. `NoLampOnAnyShippedHullSitsInsideItsGeometry` ray-casts every
> lamp of every shipped class against that mesh's own triangles, at the *drawn* scale, and votes
> parity over six directions. That is a claim about three-dimensional space, so it holds at every
> camera angle at once, which is strictly more than orbiting and looking would establish.

### Overlay & UI
8. **OverlayWorld pass** adopts the corpus two-mechanism split now:
   *(A)* per-entity instanced marks — selection ellipses (world-space circles on the plane,
   screen-space minimum radius clamp per the sheet's scaling law), hull/shield bars;
   *(B)* client-authored draw list — order puck, formation footprint ticks, dashed ghost
   polylines with per-leg ETA labels. Plane-lying geometry depth-tests against hulls
   (hard test + bias in MVP; soft SRV occlusion is the reserved DepthPre upgrade);
   screen-facing marks (bars, labels) never occlude.

   **8a. The split ran where the geometry does, not where the feature does** (S9). The order
   footprint and its station ticks are circles on the plane, which is exactly what (A) draws —
   so they are (A) kinds, and the ghost got its ring and its one-tick-per-ship without a draw
   list. What is genuinely (B) is the *polyline*: a dashed lane between two points is not a
   quad around one, and a per-leg ETA label is text. Both go with the Ui pass (§10).

   The mark kinds are numbered plane-lying-first with `FIRST_SCREEN_FACING` as the boundary,
   because the pass already draws the array in two contiguous halves with different depth
   state. Two orderings that had to agree became one that cannot disagree — and a ghost mark
   appended past the split would be drawn in the half that never depth-tests, where a
   footprint lying under a Carrier would refuse to be occluded by it.

   **8b. A mark's size and a mark's line width are both screen quantities, and a "floor" on
   either is usually a size in disguise** (S9, from the first frame anyone looked at).

   Two defects, one shape. The overlay ring's thickness was
   `max(fwidth(distance) * 1.2, 0.03)`: `fwidth` of a normalised distance is about
   `1 / radiusInPixels`, so the first term is a constant *screen* thickness and the second is a
   constant *fraction of the radius*. Past about forty pixels the floor wins and the ring's
   thickness grows linearly with what it encloses — a 700-pixel footprint drew as a forty-pixel
   band. And the order puck was `max(formationExtent, minimum)`, where the extent of a Line is
   half its length, so an eleven-ship order drew a circle spanning the viewport and touching
   the fleet at two points.

   The rules that fall out, and they apply to every mark this pass gains:

   - **A line's width comes from `fwidth` and nothing else.** A floor in normalised units is a
     fraction of the radius; the only floor a line width needs is an epsilon against a
     derivative of zero. (A *filled* shape's soft rim is a different case and may keep a
     normalised floor, because a disc does not grow a band as it grows.)
   - **A mark that means "here" is sized in pixels; a mark that means "this area" is sized in
     metres.** The puck means *where the player pointed* and is 22 px. The station ticks mean
     *where each ship is going* and are plane positions. Nothing on this pass is sized from a
     bounding radius, because a circle around a formation is an outline only for formations
     that happen to be round — the honest enclosing shape is a polyline, which is draw list
     *(B)*.

   Neither defect is visible to a device-free test: the mark builder's arithmetic was right,
   the shader's arithmetic was right, and the product of the two was wrong. That is the whole
   argument for §8's manual acceptance criteria being criteria rather than polish.

   **8c. (B) arrived in S11c, and the sentence above turned out to be a requirement rather
   than a description.** "A dashed lane between two points is not a quad around one" is exactly
   what the Ui pass could not draw: its instance is a top-left and a size, and a lane at 45°
   is neither. So the pass grew an **oriented quad** — `UI_FLAG_ORIENTED`, under which `rect`
   means centre and (length, thickness) and the corners sweep along a unit axis. The
   axis-aligned branch is left byte-identical rather than expressed as a special case of the
   sweep, because every panel and every glyph in the HUD goes through it and "the general form
   reduces to the old one" is a claim no test on a machine without a GPU can check.

   It is one primitive for a class, not a special case for a feature: `overlay-pass.png`'s
   mechanism-B list is waypoint polylines, engagement arcs and off-screen indicators, and
   every one of them is oriented. The alternative considered and rejected was square dots
   along the line, which needs no GPU change at all and reads as a dotted route rather than a
   dashed one — cheaper, and a different picture from the one the sheet draws.

   **The lane is screen-space and therefore never occluded**, which is the sheet's decision and
   not a shortcut: `overlay-pass.png`'s retirement matrix carries the order ghost as
   `MECH B · UIDRAWLIST` with no OCCLUDES badge, unlike the formation footprint listed beside
   it. A route that the hull you are flying around can hide is a route you cannot follow. It
   is also why the lane is built *before* the HUD's panels — the pass has one pipeline and no
   sort, so build order is draw order, and §1's "panels and toasts always composite over
   world-space marks" is implemented by nothing more than that ordering.
9. **Text = DirectWrite-baked glyph atlas** at boot (ASCII + box glyphs, one monospace face,
   2–3 sizes), rendered as instanced quads in the Ui pass. This keeps one graphics API in the
   frame. **Rejected:** D3D11On12/D2D interop — a second device, wrapped-resource sync, and
   the largest boilerplate item in the codebase for richer typography than the prints use.
10. **Ui pass** (screen-space quads + text): fleet roster, context bar, command row, ability
    rack (visual stubs), top status row, toasts. Layout constants from the prints; UI scale is
    a multiplier from day one (settings sheet makes 0.8–1.6× a requirement).

    **10b. A control is laid out and hit-tested in one place** (S11d). The command row's
    buttons are built in the frame's *update* and only drawn in the HUD build, so the rect a
    click is tested against and the rect a quad is emitted for come from one call. The
    alternative — laying out in the renderer and hit-testing in the input handler — is the HUD
    bug where the thing you press is not the thing you see, and it is untestable by
    construction because the two halves never meet.

    It also made a bug from S11b visible: a press anywhere outside the world started a box
    selection across the fleet *under* the panel. A drag may now only **begin** in the `world`
    rect, which is the one place the zone table was already reporting and nothing was reading.

    **10a. One instance stream, and text stays text until the pass** (S11a). Panels and glyphs
    are the same quad in the same space blended the same way, so they differ by a flag on the
    instance rather than by a pipeline — two pipelines would mean two draws over one upload
    and a sort to separate them, for a HUD whose natural build order is panel, text, panel,
    text. The atlas is R8 coverage (§9), so a glyph is its run's colour with the coverage as
    alpha, which is what lets one bake serve every colour of text on the HUD.

    The draw list carries **text runs** rather than glyph quads, and expanding them is the only
    step that needs the atlas. Everything upstream — what the HUD says, where its zones are,
    which notifications are showing — is therefore device-free and asserted without a GPU, and
    a test checks the words rather than the quads. The face being monospace (§9) is what makes
    that affordable: a run's width is its length times the cell, so layout needs no measuring
    pass and no per-glyph metrics.

    **UI scale multiplies pixels, not fractions of the viewport.** A zone written as a fraction
    would shrink its own text on a small screen, which is the opposite of what a scale control
    is for. The scale clamps to the settings sheet's range; a viewport too small for its own
    chrome collapses the world rect to nothing rather than producing a negative size, which is
    reachable by dragging a window small and would otherwise put a flipped-winding quad on
    screen.

    **No depth buffer is bound at all.** The HUD is last and composites over everything, so a
    depth test could only remove a pixel the player is meant to see. The pipeline declares
    `DSVFormat = UNKNOWN` rather than merely disabling the test, because a pipeline with a
    format and no test still expects a bound buffer.

### Picking
11. Client-side, against the **interpolated render world**: cursor → ortho ray → plane point;
    point-pick = nearest ship within `max(class pickRadius, screen-floor-in-world)`;
    box-select = ship pos inside the screen rect's plane parallelogram. No GPU picking, no
    server round-trip. (Same plane point feeds the order puck — one code path.)

    **11a. There is no ray** (S8, and it is the ortho camera's dividend rather than a shortcut).
    The projection is orthographic and the world is a plane, so `PlaneMappingForNdc` is affine
    and three `float2`s carry all of it: a pixel becomes a plane point by
    `PixelsToNdc` then `NdcToPlane`, and picking is a distance test against circles. §2 fixed
    the camera as ortho partly because "picking loses its uniform-direction ray" is what a
    perspective camera costs; this is that cost never being paid.

    "One code path" is now literal rather than aspirational — those two functions live in
    `Picking.h` and box-select, point-pick and the order puck all call them. They were file-local
    copies in two translation units first, which is exactly the arrangement where one of them
    later gets a sign wrong.

    Box-select needs the *inverse*, `PlaneToNdc`, because a drag rectangle is axis-aligned in
    screen space and an arbitrary parallelogram on the plane — far easier to map ships into
    screen space than four corners onto the plane. It is a hand-derived 2×2 inverse, which is
    the kind of thing that reads correctly with one term's sign wrong, so it is asserted to
    round-trip through the real view-projection rather than reviewed.

### Plumbing
12. Two frames in flight; waitable swapchain (latency 1 waitable, 2 buffers min 3 recommended
    — use 3 buffers); per-frame command allocator + one direct list; linear upload ring for
    constants/instances; one shader-visible CBV/SRV heap (atlas + per-frame tables); root
    signature: frame CBV, pass CBV, draw root constants, one SRV table.
    **Shaders are compiled into the executable**, not loaded: the HLSL compiler builds
    `Outpost/Shaders/*.hlsl` as part of `Outpost.vcxproj` into byte arrays, and the composition
    root hands them to `GpuPipelines` as spans (owner directive, 2026-08-18; ADR-013 §1a).
    **The compiler is `dxc` at shader model 6.7, in both configurations** (ADR-018 D12,
    2026-08-19): Debug had been compiling SM 6.7 through dxc via per-file overrides while the
    never-built Release path said SM 5.1 through fxc — a per-config compiler fork nobody
    decided, and one the reserved `GpuCull` slot would have had to unpick anyway, since a
    compute pass wants SM6. The setting now lives once per configuration and says the same
    thing in both. This
    project builds pipeline states and has no opinion about which shaders go in them — the
    engine ships around a second game, and a shader is exactly what the two differ on
    (ADR-014). One vertex and one pixel file per pass, with what both stages must agree about
    in a shared `.hlsli`, because that agreement is the link between them. Debug layer on in dev;
    `tick/frame/extract` timings feed the Tier-1 counters strip (`debug-hud.png`).
    **MSAA landed with S14 as the listed polish slice**: a 4× offscreen colour target and a
    multisampled depth buffer, owned by `GpuSwapChain` beside the back buffers whose size they
    share; the world passes (opaque, nebula, overlay) render there and the frame loop resolves
    into the back buffer before the Ui pass draws on it single-sampled. The resolve's format is
    the back buffer's own UNORM — the MSAA resource is typeless so its RTV can be the sRGB view,
    but a resolve into a typed destination must state that destination's format exactly, so the
    averaging happens in gamma space; on a near-black scene that is a subtler wrong than the
    debug-layer error the sRGB spelling risks. An unsupported sample count falls back to 1 with
    a log line rather than refusing to start, and `client.renderer.msaa` (shipped 4) is the knob
    that was in the config unread since S2b.

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

- The whole MVP renderer is ~5 PSOs (opaque, nebula, overlay-instanced, overlay-polyline,
  ui/text) and no compute — DX12 boilerplate risk is bounded to device/swapchain/upload
  plumbing (Risk R4). **Six as of §1c**, the sixth being the multisampled variant of the
  ui/text pipeline that draws into the world target; still no compute, still one queue.
- The nebula's parameters are content (ADR-012), so retuning the look is a config edit and a
  restart. Tint and intensity apply at draw time; resolution, octaves, coverage, contrast and
  seed change the field and are re-baked at boot.
- A `nebula` block that describes no field costs the frame its haze and nothing else: the pass
  checks and draws nothing. A client that will not start because the art direction is
  mistyped would be the wrong failure.
- Extract (ADR-007) is the only bridge from net-thread world to GPU data; `InstanceRecord`
  is its output format, already shaped for the corpus's later GPU-culled path.
- Camera/selection/overlay all assume the plane; if ADR-001 ever reopens, this ADR reopens.
- Atlas text caps typography (no shaping/i18n in MVP) — accepted; revisit at localisation.
