# Outpost.Frontier: a native C++23 rebuild of Homeworld: Deserts of Kharak

**Goal.** Rebuild Deserts of Kharak (Blackbird Interactive, 2016; Unity 5.2.2f1, build
"DisjointedFalafel" of 2018-05-25) as a native C++23 game: the same simulation, missions,
content and behaviour, with no Unity, no Mono and no Unity asset formats at runtime.

**Measure of done.** Every campaign mission, skirmish map and multiplayer map plays as in the
original, and the simulation is *bit-exact*: replaying the original's command streams through the
C++ simulation gives the original's per-tick checksums (see [Verification.md](Verification.md)).

## The inputs

| What | Where (default) | Made by |
|---|---|---|
| The original game | anywhere: `gameDir` in `local.json` | the GOG release |
| Decompiled game code, one C# project per assembly; the middleware in `Middleware\` | `..\DoK-Decompiled\` | `Tools\Extract\Extract.ps1` (ILSpy 11.2) |
| Every asset and object, arranged per scene, with `manifest.json` | `..\DoK-Assets\` | `Tools\Extract\Extract.ps1` (AssetStudioModCLI 0.20.0 and our scripts) |
| Animated models (FBX with skeletons and clips) | `..\DoK-Assets\Models\` | the same |

None of these go into this repository or into CI: they are the original's code and data. The
repository holds our code, our converters and our documents. On any machine with the game
installed, [`Tools/Extract`](../Tools/Extract/README.md) rebuilds the decompiled code and the
extracted assets in about 30 minutes; converted content is produced locally from them (Plan,
phase 2).

## Standards (owner-confirmed, 2026-11-14)

- **C++23**, MSVC, Windows x64.
- **Direct3D 12 on raw Win32 and DXGI.** No windowing or rendering framework.
- **DirectXMath, used natively**: no wrapper types or functions over it.
- **Shaders in HLSL, compiled with `dxc` at Shader Model 6.**
- **Strict library policy**: the C++ standard library and the Windows SDK by default. Any other
  library is an exception that is decided and recorded in [Decisions.md](Decisions.md), with
  the reason it cannot be avoided. There are none today; cook-time tools (ffmpeg, an xWMA
  encoder) are listed there too.

## Documents

| Read | For |
|---|---|
| [Inventory.md](Inventory.md) | what the original consists of, measured: code, subsystems, assets, middleware |
| [Architecture.md](Architecture.md) | the target: modules, the simulation/presentation split, determinism rules |
| [Asset-Pipeline.md](Asset-Pipeline.md) | every asset type: source, native format, converter, how it is checked |
| [Plan.md](Plan.md) | **the phases and actions, in order, with deliverables and exit criteria** |
| [Verification.md](Verification.md) | how "the same as the original" is proved, per layer |
| [Risks.md](Risks.md) | what can sink or stretch the rebuild, and the mitigation for each |
| [Decisions.md](Decisions.md) | the owner's rulings (standards and D1–D14, 2026-11-14) and the cook-time tools |

Order of authority: Decisions over Architecture and Asset-Pipeline on *what*; Plan on *when*.
How code is written: [CONTRIBUTING.md](../CONTRIBUTING.md).

## Legal position

The rebuild is a derivative of Blackbird Interactive's and Gearbox's work, made from a purchased
copy. It is private study until the owner decides otherwise: no original code or data is
committed or distributed, and the game runs only from the owner's own installation (the asset
pipeline reads the install; it never ships content).
