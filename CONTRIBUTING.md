# How code is written here

For anyone, human or agent, writing code in this repository. [Design/](Design/README.md) says
*what* to build and *when*; this file says *how it is spelled*. Where they disagree, the design
wins on what and this file on how.

## 1. Naming (Decisions D10)

| Kind | Convention | Example |
|---|---|---|
| Type (class, struct, enum, concept, alias) | `PascalCase` | `SimController`, `Fixed64` |
| Function, method | `PascalCase` | `AdvanceTick()`, `GetChecksum()` |
| Member variable | `m_camelCase` | `m_currentFrame` |
| Static member (mutable; rare, and documents its thread safety) | `sm_camelCase` | `sm_registry` |
| Global (in an anonymous namespace) | `g_camelCase` | `g_frameCount` |
| Parameter | `_camelCase` | `_commands`, `_fileName` |
| Local | `camelCase` | `unitIndex` |
| Constant (`constexpr`, `static constexpr`) | `UPPER_CASE` | `TICKS_PER_SECOND` |
| Enumerator | `PascalCase` | `SyncError` |
| Macro | `SCREAMING_SNAKE`, prefixed `OUTPOST_` | `OUTPOST_ASSERT` |
| Namespace | `PascalCase`, one per library | `Core`, `Sim`, `Render` |
| File | `PascalCase.cpp` / `.h`, one class per pair | `SimController.cpp` |

- **Acronyms capitalise as words**: `NavMeshData`, `UiAtlas`, `GpuDevice`, never `GPUDevice`.
- **Names from an external SDK keep that SDK's spelling** and are never wrapped or renamed:
  `ID3D12Device`, `XMVECTOR`, `IXAudio2SourceVoice`, `HRESULT`.
- **Names ported from the original keep their meaning**: `SimController`, `SimStateFrame`,
  `CommandRouter`, `Fixed64`, so the C# stays the readable specification beside the C++. The
  original's `m` prefix becomes `m_` (`mCurrentSimFrame` → `m_currentSimFrame`); its `k`
  constants become `UPPER_CASE` (`kSimTicksPerSecond` → `SIM_TICKS_PER_SECOND`).
- **Parameters start with `_` followed by a lower-case letter**, which is legal at parameter
  scope. Never `_Upper`, never `__` anywhere: those are reserved.

## 2. Layout

- Each library is a folder under `Source/` with its own `.vcxproj`; files sit flat in it.
- Includes are rooted at `Source/`: `#include "Core/Fixed64.h"`.
- 4 spaces, no tabs; braces on their own line for types and functions; lines up to 110 columns.

## 3. Rules

1. **Libraries:** the C++ standard library and the Windows SDK only (Decisions S5). A new
   library is an owner decision recorded in [Decisions.md](Design/Decisions.md) first.
2. **DirectXMath natively** in the presentation: no wrapper types, aliases or forwarding functions.
3. **`Core`, `Data` and `Sim` never include `<windows.h>`** or any platform header; CI checks it.
4. **Determinism in `Sim`** ([Architecture.md §4](Design/Architecture.md)): no `float` or
   `double` in simulation state; `Fixed64` arithmetic only through `Core`; no iteration over
   `std::unordered_map`/`std::unordered_set`, and no `std::sort` where equal keys can occur: use
   the .NET-compatible containers and sort in `Core`; no signed overflow (wrap explicitly on
   unsigned types); `/fp:precise` everywhere, never `/fp:fast`.
5. **Errors:** no exceptions across library boundaries. Fallible operations return
   `std::expected`; programming errors are assertions.
6. **Warnings are errors** at level 4, with conformance mode on (`Source/Outpost.props`).
7. **Tests:** every library change comes with tests in `Source/Tests` (the minimal runner in
   `Test.h`: `TEST(Name) { CHECK(...); }`). Test vectors computed in the original may be committed
   (Decisions D14); the original's code and data may not.

## 4. Building

Visual Studio 2022 (toolset v143) or Visual Studio 18 (v145) with the Windows SDK 10.0.26100.0;
`Directory.Build.props` picks the toolset.

```powershell
msbuild Outpost.Frontier.sln /m /p:Configuration=Release /p:Platform=x64
Build\Release\Tests.exe
```

The original's code and assets are never in this repository: [Tools/Extract](Tools/Extract/README.md)
rebuilds them locally from an installation.
