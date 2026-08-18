# Outpost: Frontier — how to work here

Conformance rules for anyone, human or agent, writing code in this repository.

Two things to read before you start:

1. **This file** — naming, layout, build, and the working rules.
2. **[Design/](Design/)** — the architecture decisions. [Design/README.md](Design/README.md)
   indexes the ADRs; they are normative, and where this file and an ADR disagree, the ADR wins
   on *what* to build and this file wins on *how it is spelled*.

The naming convention in §1 is adopted verbatim in substance from the sibling repository
**Outpost.Warzone** (`AGENTS.md` §1), so that engine code can move between the two trees
without a rename pass. Where Frontier needs an addition rather than a change, it is marked
**F**.

**This is a greenfield tree. Nothing is grandfathered.** The sibling repository carries a 1998
C codebase and has to exempt it; here the rules apply to every line, from the first one.

---

## 1. Naming convention (normative — no exceptions)

| Kind | Convention | Example |
|---|---|---|
| Type (class, struct, enum, concept, alias) | `PascalCase` | `SnapshotBuffer` |
| Function, method | `PascalCase` | `SolveFormation()` |
| Member variable | `m_camelCase` | `m_deviceLost` |
| Static member | `sm_camelCase` | `sm_activeDevice` |
| Global | `g_camelCase` | `g_frameCount` |
| Parameter | `_camelCase` | `_shipId`, `_fileName` |
| Local | `camelCase` | `stationIndex` |
| Constant, enumerator | `PascalCase` | `TickRate`, `QueueFull` |
| Macro | `SCREAMING_SNAKE` | `NEURON_SPAN`, `NEURON_ASSERT_OWNER` |
| Namespace | `PascalCase` | `Neuron`, `Game` |
| File | `PascalCase.cpp` / `.h` | `Transport.cpp` |

### The rules behind the table

**R1 — The leading underscore on parameters is deliberate.** It is legal C++: the reserved
forms are `_Uppercase`, anything containing `__`, and `_lowercase` **at global scope**. A
parameter is never at global scope, so `_shipId` is safe. Never introduce a reserved form — no
`_Impl`, no `__helper`, no file-scope `_cache` (use `g_cache` in an anonymous namespace).

**R2 — A type name carries no prefix, and that includes abstract ones.** The transport
abstraction is `Transport`, **not `ITransport`**. A base class is not `BaseTransport` or
`AbstractTransport`. This also bans `CFoo`, `SFoo`, `EFoo`, `FooBase`, `FooImpl`, and `_t`
suffixes. Name the concept and let the concrete types say what they are:

```
Transport             ← the concept
├── UdpTransport      ← the loopback/plaintext one (MVP)
└── QuicTransport     ← msquic, the product path
```

Unlike the sibling tree, Frontier genuinely has two implementations from the outset
(ADR-003), so the hierarchy above is real rather than illustrative. Where there is only ever
one implementation, do not invent a base class for it.

**R3 — Compile-time constants are Constants.** `static constexpr` members and namespace-scope
`constexpr`/`inline constexpr` take PascalCase: `TickRate`, `TickDt`, `InterpDelayTicks`,
`MaxDatagramBytes`, `MaxOrderLegs`. `sm_` is reserved for *mutable* statics, which are rare
and must document their thread-safety.

**R4 — Acronyms capitalize as words**: `HudRoster`, `UdpTransport`, `GpuDevice`, `JsonWriter`
— never `HUDRoster` or `UDPTransport`. Identifiers from an external SDK keep that SDK's
spelling and are never renamed to fit: `XMFLOAT2`, `XMVECTOR`, `ID3D12Device`,
`IDXGISwapChain4`, `IXAudio2SourceVoice`, `X3DAUDIO_EMITTER`, `HRESULT`, `QUIC_STATUS`.

**R5 — Template parameters are PascalCase**: `T`, `Fn`, `BlockBytes`, `Ts...`.

**R6 — Units belong in names; types do not.** `posXCm`, `velXCmPerSec`, `headingTurns16`,
`etaTicks`, `zoomMetres`, `cooldownMs` are encouraged — this game measures a plane in metres,
a wire in centimetres, and time in ticks, so unit ambiguity is a real defect class. Never
encode the type: no `iCount`, `pShip`, `strName`, `dwFlags`.

**R7 — A file is named for its primary type**, PascalCase, `.h` / `.cpp` only. `.hpp`, `.cc`
and `.inl` are not used; template implementations live in the header. Two exceptions: the
per-project `pch.h`/`pch.cpp` keep the name MSBuild expects, and a file holding one message or
type *family* takes the family's name (`Snapshot.h`, `OrderMessages.h`). Formats get
hand-written parsers — there is no generated code in this tree and none is to be introduced.

**R8 — `m_` marks encapsulated state, not every field.** A `class` with invariants prefixes
private members `m_`. A public aggregate — a config struct, a wire record, a POD handed to the
renderer — uses plain `camelCase` fields so brace initialization reads naturally. So
`ShipRecord{ shipId, posXCm, … }` and `InstanceRecord{ posWorld, heading, … }` carry no
prefix, while `SnapshotBuffer` keeps `m_ring`, `m_latestTick`.

**F9 — Namespaces.** `Neuron` for the three engine libraries (NeuronCore, NeuronClient,
NeuronServer) — flat, matching the sibling repository so shared engine code moves without
edits — and `Game` for GameLogic. The library boundary and the dependency rules
([Design/Dependency-Map.md](Design/Dependency-Map.md)) do the separating; the namespace does
not need to repeat it. Do not nest further (`Neuron::Core::Detail`), and do not add a
`Detail`/`Internal` namespace where an anonymous namespace in a `.cpp` will do.

**F10 — Integer widths are spelled `std::uint32_t`.** The design documents use `u8`/`u16`/
`u32` as prose shorthand for wire layouts; code uses the standard names, and wire structs
state their width explicitly on every field.

### Worked example — this is the target style

```cpp
// NeuronCore/UdpTransport.h
#pragma once
#include <cstdint>

namespace Neuron
{

inline constexpr std::uint32_t MaxDatagramBytes = 1152;   // R3: constant → PascalCase

enum class ConnectionState : std::uint8_t { Connecting, Connected, Draining, Closed };

/// Loopback/plaintext transport. R2: no prefix on the type; R8: private state carries m_.
class UdpTransport
{
public:
  struct Desc                                             // R8: aggregate → plain fields
  {
    std::uint16_t port;
    std::uint32_t receiveBufferBytes;                     // R6: unit in the name
  };

  [[nodiscard]] static bool Create(const Desc& _desc,      // R1: _ on parameters
                                   UdpTransport& _outTransport) noexcept;

  void Poll() noexcept;                                   // ADR-003: delivery on the owning thread
  [[nodiscard]] ConnectionState State() const noexcept { return m_state; }

private:
  ConnectionState m_state = ConnectionState::Closed;
  std::uint64_t m_bytesSent = 0;
};

} // namespace Neuron
```

### Enforcement

[`.clang-tidy`](.clang-tidy) is the machine-readable statement of the table and is the
**single source of truth for the option values** — this document states the rules in prose and
does not repeat the settings, so there is nothing to drift. [`.clang-format`](.clang-format)
owns whitespace; the two never overlap.

**Nothing runs either automatically yet.** Until a CI step exists, §1 is enforced by review:
check your own diff against the table before handing it back. Run clang-tidy on the files you
wrote, not on the tree:

```
clang-tidy --quiet NeuronCore/YourNewFile.cpp -- -I . -D _WIN32 -D _DEBUG /std:c++latest
```

Two rules the config cannot express, and that a reviewer therefore has to carry:

- **R2 (type prefixes)** — clang-tidy can require an *absent* prefix but cannot ban a *present*
  suffix, so `FooBase` slips through. Grep declarations, `using` and `typedef` aliases
  included, for `\b(class|struct|using)\s+[ICSE][A-Z]` and for trailing
  `Base`/`Abstract`/`Impl`.
- **R7 (file naming)** — nothing checks that a new file is PascalCase and `.h`/`.cpp`. Look at
  the filename when you add one, and at the `.vcxproj` **and** `.filters` entries that must
  accompany it (§3).

---

## 2. Repository map

| Path | What it is |
|---|---|
| `NeuronCore/` | Engine primitives shared by client and server — zero game semantics. Foundation, containers, tasking, telemetry, byte IO, JSON, `Transport` (+ UDP/QUIC). |
| `GameLogic/` | The deterministic planar simulation. Depends on NeuronCore only. |
| `NeuronServer/` | `ServerHost` — sessions, tick-loop orchestration, snapshot fan-out. |
| `NeuronClient/` | `ClientApp` — window, D3D12, camera, picking, HUD, audio, interpolation. |
| `Outpost/` | The executable: composition root, config loading, boot/shutdown ordering. |
| `Tests/*Tests/` | VS CppUnitTestFramework suites, one per library. |
| `GameData/` | Content: `Meshes/` (OBJ/MTL), `Universe/` (JSON), `Audio/` (WAV + bank JSON). |
| `Design/` | ADRs and design documents. `Design/ScreenPrints/` is the UI reference corpus. |

Dependency rules are in [Design/Dependency-Map.md](Design/Dependency-Map.md) and are hard:
GameLogic depends only on NeuronCore; client and server never depend on each other; nothing
depends on the executable; and **the engine libraries never reference GameLogic** — `Neuron*`
is a shared engine (the sibling repository runs a different game on it), so the game is reached
through engine-declared interfaces that `Outpost.exe` injects (ADR-014).

---

## 3. Files, layout and filters

- **Flat project directories.** All of a project's `.h`/`.cpp` sit directly in its folder.
  No code subdirectories — grouping lives in `.vcxproj.filters` only (ADR-013).
- **File names are unique repo-wide**, and also unique against the CRT, the STL and the
  Windows SDK — **case-insensitively**. A header named `Time.h` or `Assert.h` shadows
  `<time.h>` or `<assert.h>` for every translation unit that can see this folder, and the
  errors land inside the STL with nothing pointing at you. CI fails the build on a collision.
  The tables in [Design/Dependency-Map.md](Design/Dependency-Map.md) are the registry; check
  it *before* creating a file, and add the name when you do.
- **Includes are unqualified**: `#include "Json.h"`. Each project lists the libraries it is
  entitled to as `$(SolutionDir)<Project>` include paths. Because several roots sit on the
  search path, a duplicate file name silently resolves to the wrong header — which is why the
  registry is checked *before* a file is created, not after.
- **The include path is not the dependency rule.** `NeuronClient` and `NeuronServer` must not
  list `GameLogic`: the engine libraries never depend on the game (ADR-014).
- **Every added, removed or renamed file updates both** the `.vcxproj` and the
  `.vcxproj.filters` of its project, in the same commit.

---

## 4. Layout and formatting

[`.clang-format`](.clang-format) is the authority; [`.editorconfig`](.editorconfig) repeats
only what an editor needs before the first save. The shape: **Allman braces, 2-space indent,
140 columns, no tabs, `namespace` contents not indented, pointer binds left**
(`ID3D12Device* _device`).

Include order is **not** sorted automatically and is grouped by hand: `pch.h` first, then
Windows headers (`windows.h` before any D3D/DXGI/XAudio2 header, with `WIN32_LEAN_AND_MEAN`
and `NOMINMAX` already defined), then SDK headers, then project headers, then the standard
library. A formatter reordering these behind your back is a correctness risk, not a style
preference.

Format the lines you write. Do not reformat files you are only passing through.

---

## 5. C++ rules for this codebase

- **C++ latest** (`/std:c++latest`), MSVC v145, x64 only, `ConformanceMode` on. Do not turn
  conformance off to make something compile.
- **Math is DirectXMath, used natively** — no wrapper types, functions, or aliases. Store
  `XMFLOAT2/3/4`, `XMFLOAT4X4`; compute in `XMVECTOR`/`XMMATRIX` as locals and parameters with
  the `XM_CALLCONV` conventions. Never a stored `XMVECTOR` or a `std::vector<XMVECTOR>`.
  (ADR-010.)
- **GameLogic is deterministic.** No wall clock, no OS entropy, no pointers as keys, no
  iteration order that isn't dense-array order, one seeded PCG32. `XM*Est` functions are
  **banned** in GameLogic, `/fp:fast` is banned there, and `/arch` stays uniform across the
  solution. The replay-equality suite is the gate. (ADR-005, ADR-010.)
- **No argv, no environment variables.** Configuration is JSON, loaded by the composition root
  only; libraries receive plain config structs and never read files or the registry.
  (ADR-012.)
- **The client and server halves communicate only over the transport** — a real socket, even
  in-process. No shared memory, no cross-half function calls, no singletons bridging them.
  (ADR-007.)
- **Single-writer state.** The authoritative world belongs to the Sim thread, render state to
  Main. Foreign threads (msquic workers, XAudio2 callbacks) enqueue to a ring and touch
  nothing else.
- **COM lifetimes are RAII, through `winrt::com_ptr`** (aliased as `Neuron::GpuPtr`), not
  `Microsoft::WRL::ComPtr` and never raw `Release()` calls. Two idioms: create with
  `Thing(__uuidof(IThing), thing.put_void())`, query with `thing.try_as<IOther>()`.
  `IID_PPV_ARGS` does **not** work — it needs `operator&`, which `com_ptr` deliberately lacks,
  and `put()` asserts the pointer is empty rather than silently releasing what was there.
  Include `<unknwn.h>` before `<winrt/base.h>` or classic COM interfaces are unsupported.
  This is the COM-helper sanction only — do not reach for the WinRT projection as a UI or
  async framework. A project that includes these headers needs the C++/WinRT package, so keep
  them out of headers that test projects consume.
- **No external libraries without the owner's explicit approval.** Pre-approved: the Windows
  SDK (Win32, Winsock2, D3D12/DXGI, DirectXMath, DirectWrite, XAudio2/X3DAudio), **msquic** via
  NuGet, and C++/WinRT as above. If you believe a third-party library is justified, present the
  case and **stop** — do not assume approval.
- **Errors that are the user's fault are diagnostics, not crashes.** Anything parsing content
  or config (JSON, OBJ, WAV, universe data) reports `(line, column, message)` and fails
  closed; it never throws on malformed input and never asserts.

---

## 6. Build and verify

```
msbuild Outpost.slnx /p:Configuration=Debug   /p:Platform=x64
msbuild Outpost.slnx /p:Configuration=Release /p:Platform=x64
vstest.console.exe Tests\GameLogicTests\x64\Debug\GameLogicTests.dll
```

**CI** ([`.github/workflows/build.yml`](.github/workflows/build.yml)) builds **Debug|x64 only**
on every push and runs all four test projects. Release is not built — x64 is the only platform
and, this early, a Release job would double the wall clock to catch almost nothing. Add it when
optimised-only breakage becomes a real risk.

Two things CI does that a local build does not, and that are easy to trip over:

- It builds the `.vcxproj` files directly (the NuGet CLI cannot read `.slnx`), so it passes
  **`/p:SolutionDir=`** explicitly. Every project's include paths are written as
  `$(SolutionDir)<Project>`, so without it no cross-project include resolves.
- **`Outpost.exe` is built only once it has an entry point.** It is a Windows-subsystem
  application, so until `Main.cpp` exists the link fails on `WinMain`; the step detects this
  and skips rather than leaving CI permanently red. It starts building itself when slice S1
  lands, with no edit to the workflow.

Headless checks (no GPU needed) run by launching the executable from a directory whose
`Outpost.json` sets `"mode": "headless"` and `"selfTest": true` — there are no flags to pass
(ADR-012).

**Report what you actually did.** "Builds clean, not run" and "builds and runs the fleet-move
slice" are different claims. Never imply the second when you only did the first.

---

## 7. Working rules

- Change the lines the task requires and no others. No drive-by reformatting, no opportunistic
  renames.
- New files follow §1's worked example: `#pragma once`, PascalCase filename, Allman braces,
  the right namespace.
- If a rule here blocks the task, say so in your report rather than quietly bending it.
- If a design decision needs to change, change the ADR — do not leave code and `Design/`
  disagreeing.

---

## 8. Before you hand work back

- [ ] Naming conforms to §1 — `_` on parameters, `m_` on class state, no `I`/`C`/`Base`
      prefixes, units in names.
- [ ] Files are PascalCase, flat, unique repo-wide, and registered in the Dependency Map.
- [ ] Every added/removed/moved file is in both the `.vcxproj` **and** the `.filters`.
- [ ] No `argv`, no environment reads, no `XMVECTOR` stored in a struct or container.
- [ ] GameLogic touched? The replay-determinism suite still passes.
- [ ] It builds — Debug at minimum — and you said which configurations you actually built.
- [ ] Tests for the layer you touched were run, and you said which.
- [ ] `Design/` updated if the change moved or contradicted a decision.
- [ ] Your report states plainly what you verified, what you assumed, and any rule you bent.
