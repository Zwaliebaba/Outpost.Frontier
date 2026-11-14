# Architecture: the target

## 1. Shape: keep the original's split

The original already separates a deterministic simulation from its presentation, and the rebuild
keeps that seam exactly where Blackbird put it:

```
                 commands (the only input)               state frames + events (the only output)
 Input/UI/AI ───────────────────────────────▶  Sim  ───────────────────────────────────────▶ Presentation
 Network     ───────────────────────────────▶ (8 ticks/s, Fixed64, checksummed)               (60+ fps, float,
                                                                                               interpolated)
```

- **Sim** is the port of `BBI.Game`, `BBI.Core` and `BBI.Game.Data`: a library with no Windows,
  rendering or audio dependencies, advanced only by commands, observable only through its state
  frames, events and checksums. It is what lockstep multiplayer, replays and saves run on, and
  what [Verification.md](Verification.md) proves bit-exact.
- **Presentation** is the rewrite of `BBI.Unity.Game` and friends on our engine: it reads state
  frames and events, interpolates between ticks, and draws, plays and shows them. Nothing it does
  can change the simulation; desync is impossible from this side by construction.

## 2. Projects

| Project | Kind | Ports / replaces | Depends on |
|---|---|---|---|
| `Core` | lib | the non-Unity half of `BBI.Core`: `Fixed64`, `Vector2r`/`Vector3r`, `Random64`, `Checksum`, ID types, the .NET-compatible collections (§4), events, JSON, logging, file I/O | C++ std |
| `Sim` | lib | `BBI.Game` (simulation, AI, commands, network protocol, replay, save/load) | Core, Data |
| `Data` | lib | `BBI.Game.Data` attribute types, generated from a schema; loaders for cooked data | Core |
| `Platform` | lib | Win32 window, input (keyboard, mouse, gamepad via Windows SDK), timing, file system | Windows SDK |
| `Render` | lib | Unity's renderer for this game: D3D12 device, frame, materials and the rewritten shaders, meshes, skinning, terrain, deferred decals, particles, post-processing, lighting | Platform, DirectXMath, `dxc` |
| `Audio` | lib | Fabric: event-driven component trees over XAudio2, with X3DAudio for 3D | Platform, Windows SDK |
| `Video` | lib | MovieTexture: video playback into a texture via Media Foundation | Render, Windows SDK |
| `Anim` | lib | Mecanim for what the game uses: clips, the 56 state machines, skinning poses, `AnimatedSuspension` | Core |
| `Ui` | lib | NGUI: atlases, sprites, labels, widgets, buttons, localisation hooks; text through DirectWrite | Render, Platform |
| `Scene` | lib | the runtime scene: cooked hierarchies, components, prefab instancing, LOD, the scene entities the sim consumes | Render, Anim, Audio |
| `Script` | lib | uScript and PlayMaker runtimes for the mission graphs and FSMs; CinemaDirector cutscene runtime | Scene, Sim (commands, queries) |
| `Net` | lib | lockstep transport and sessions: Steam, GOG Galaxy, LAN | Sim, Steamworks/Galaxy SDKs |
| `Game` | exe | the port of `BBI.Unity.Game`: unit views, FX layer, HUD, front end, camera, input mapping, audio controllers, scenario flow | everything above |
| `Cook` | exe | the asset pipeline: original install → native content ([Asset-Pipeline.md](Asset-Pipeline.md)) | Core, Data |
| `SimRunner` | exe | headless: runs replays and command logs through `Sim`, compares checksums with golden traces | Sim |
| `Tests` | exe | unit tests per library; the golden-trace suite | all libs |

Rule: **`Sim`, `Core` and `Data` compile without `<windows.h>`**, so the simulation can never
reach a platform API by accident, and `SimRunner` runs anywhere.

## 3. Runtime data flow

1. `Cook` reads the owner's installation and writes native content (per mission group, as the
   original loads it) into a local content folder outside the repository.
2. The game loads a mission group: its scenes' cooked hierarchies (`Scene`), their assets
   (`Render`, `Audio`, `Anim`), and the simulation inputs: attributes, height map, nav mesh, and
   the scene entities (`ConvexPolygonObstacle`, `SceneResourceEntity`, `TriggerCircleEntity`,
   `SceneAIHintEntity`, ...) converted to simulation descriptors exactly as `BBI.Unity.Game`
   does at load (`ShipbreakersMain`, `EntityComponentExtensions`).
3. Each sim tick (125 ms) consumes the commands for that tick and produces a state frame, events
   and a checksum. The presentation interpolates frames and reacts to events (FX, audio, UI).

## 4. Determinism: what the C++ must reproduce exactly

Measured in `BBI.Game` and `BBI.Core` (2026-11-14). Each item is ported once, in `Core`, and the
simulation code is not allowed to use the C++ standard equivalent where order or bits can differ.

| C# behaviour | Where | In C++ |
|---|---|---|
| `Fixed64` arithmetic, including overflow (C# `long` wraps silently; no `checked` anywhere in the sim) | everywhere | port `Fixed64.cs` line for line; do wrapping arithmetic on `uint64_t` (or `_mul128` / `std::int128` emulation for the 64×64 multiply) and `std::bit_cast` back. Signed overflow is never allowed to happen in C++ |
| float → `Fixed64` (`UnsafeFromFloat`: `(long)(value * 2^32)`, saturating) | data load | the same expression in `double`; tested on every authored value against the original's loaded values |
| `Dictionary<K,V>` / `HashSet<T>` iteration order (622 uses, 42 enumerations) | sim and AI | a port of **Mono's** `Dictionary`/`HashSet` (buckets, entry array, free list), not `std::unordered_map`, wherever a collection is enumerated |
| `List<T>.Sort` / `Array.Sort` (21 calls), unstable: ties come out in implementation order | sim and core | a port of Mono's sort algorithm with the same comparer calls |
| `GetHashCode()` in checksums (118 uses: `string`, `bool`, `int`, enums, `ID<T>`) | checksums | Mono's implementations, including **Mono's string hash**, in `Core::Hash` |
| `Checksum.Combine` | checksums | port exactly; it defines what "bit-exact" is checked against |
| `Random64` (the sim's RNG); `System.Random` (3 uses in `BBI.Core`) | AI, dynamics | port `Random64`; `System.Random` is Knuth's subtractive generator, ported if any use reaches the sim |
| `Math.Round` on `float` (`EconomyManager`): halves to even | AI | `std::nearbyint` in the default rounding mode; `/fp:precise`, never `/fp:fast`; SSE2 only |
| LINQ (26 uses) | sim | rewritten as loops with the same order of evaluation |
| `foreach` over a collection that the loop body modifies (throws in .NET) | | must not exist; assert in debug builds |

## 5. Presentation: what is rebuilt, in Unity terms

The presentation layer is large (`BBI.Unity.Game` alone is 115k lines) because it leans on
Unity's systems. The engine provides the subset of them that the content measurably uses
([Inventory.md §3](Inventory.md)), not Unity in general:

- **Rendering:** forward or deferred is decided by the shaders' rewrite (Decisions D2); deferred
  decals (28,742 instances) are a first-class pass either way. Materials are the 217 shaders'
  properties as cooked parameter blocks; LOD groups; layer sorting (`FX_SCRP_LayerSorting`).
- **Particles:** a Shuriken-compatible runtime for the modules the 18,748 systems actually use
  (measured during conversion), plus Blackbird's FX layer over it (`FXElement`, `FXLOD*`,
  `FXNLIPS*`, despawn timers, proxy volumes).
- **Lighting:** real-time lights and the WorldArt scenes' global illumination (Risks R3).
- **Animation:** clips, the 56 Animator state machines, skinning, suspension.
- **Audio:** Fabric's model (events → component trees: random, blend, sequence, 3D sources),
  with the dialogue, drama and dynamic-music controllers on top.
- **UI:** NGUI's model (atlas sprites, labels, anchors, tweens, localisation); the front end, HUD
  and menus are rebuilt screen by screen against the original.
- **Scripting and cinematics:** the uScript graphs and PlayMaker FSMs as data driving the
  `Script` runtime; CinemaDirector cutscenes; the 27 videos.

## 6. C++23 use

Modules are not used (tooling); `std::expected` for fallible loading, `std::span`/`std::mdspan`
for buffers, `std::flat_map` only where order does not matter, `std::print`/`std::format` for
logging. No exceptions across library boundaries; no RTTI in `Sim`. Naming and layout
conventions are in Decisions D10.
