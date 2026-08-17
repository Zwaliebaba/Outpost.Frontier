# ADR-013 — Source Layout: Flat Project Directories, Grouping via VS Filters

**Status:** Accepted · 2026-08-17 (owner directive)
**Depends on:** the fixed project structure
**Supersedes:** every subdirectory path used illustratively in earlier documents
(`game/wire/Snapshot.h`, `wire/Order.h`, a possible `Public/` split) — those are rewritten
as flat names below.

## Context

Owner directive: **no subdirectories in the code; use Visual Studio filters to group files.**
Each project directory holds its `.h`/`.cpp` files directly; the `.vcxproj.filters` file
provides the tree the IDE shows. The template already works this way (`pch.h`, `pch.cpp` at
each project root, default `Source Files`/`Header Files` filters).

This is a real trade, not a formality: filters are IDE-only metadata, so grouping lives in a
file MSBuild does not use for compilation, and the flat namespace must be managed by naming.

## Decision

1. **Flat project directories.** All source for a project sits directly in that project's
   folder. No `Wire/`, no `Gpu/`, no `Public/`, no `Private/`. `pch.h`/`pch.cpp` stay at the
   root as the template has them.
2. **Grouping is `.vcxproj.filters` only** — virtual folders, maintained per project, and
   allowed to nest freely (`Wire`, `Transport`, `Gpu\Passes`). Filters cost nothing at build
   time and are the directive's intended mechanism. The template's extension-driven
   `Source Files`/`Header Files` filters are replaced by semantic ones, since a flat directory
   makes "all headers in one bucket" useless at this scale.
3. **File names are unique repo-wide** — with a flat layout and project roots on the include
   path, uniqueness is what keeps includes unambiguous. Achieved by area prefixes rather than
   by ceremony:

   | Project | Prefixes | Examples |
   |---|---|---|
   | NeuronCore | *(area)* | `Log.h` `Time.h` `Hash.h` `Random.h` `Arena.h` `RingBuffer.h` `TaskPool.h` `Telemetry.h` `ByteReader.h` `ByteWriter.h` `Json.h` `JsonWriter.h` `Transport.h` `UdpTransport.h` `QuicTransport.h` `WireCore.h` |
   | GameLogic | `Wire*`, `Universe*` | `Ids.h` `ShipClass.h` `World.h` `ReplicatedView.h` `Orders.h` `Validate.h` `Formation.h` `WorldHash.h` `WireSnapshot.h` `WireOrder.h` `WireSchemaHash.h` `Universe.h` `UniverseParse.h` |
   | NeuronServer | `Server*`, `Session*` | `ServerHost.h` `ServerConfig.h` `Session.h` `SnapshotSender.h` |
   | NeuronClient | `Gpu*`, `Hud*`, `Cam*`, `Audio*` | `ClientApp.h` `Window.h` `ClientConnection.h` `SnapshotBuffer.h` `RenderWorld.h` `GpuDevice.h` `GpuSwapChain.h` `GpuUploadRing.h` `GpuPasses.h` `GpuPipelines.h` `ObjMesh.h` `GlyphAtlas.h` `CamIso.h` `Picking.h` `InputMap.h` `HudLayout.h` `HudRoster.h` `OrderPuck.h` `AudioSystem.h` `AudioBank.h` `AudioListener.h` |
   | Outpost | — | `Main.cpp` `AppConfig.h` `AppConfig.cpp` `ConfigLoad.h` `ConfigLoad.cpp` |

   If a genuine collision ever appears, the **newer** file takes a library tag prefix; the
   table above is the registry to check first.
4. **Include style:** `$(SolutionDir)` is the single additional include root, so cross-project
   includes are project-qualified and unambiguous:
   `#include "NeuronCore/Json.h"`, `#include "GameLogic/WireSnapshot.h"`. Within a project,
   plain `#include "Json.h"`. Project folders are project roots, not code subdirectories — the
   directive is satisfied.
   *(Owner action: the vcxprojs currently list each project folder individually as an include
   directory, e.g. `$(SolutionDir)NeuronCore;$(SolutionDir)NeuronClient;…`. Switching to
   `$(SolutionDir)` alone gives the qualified form above and keeps a project's own headers
   reachable unqualified. Both work given rule 3; qualified is the recommendation.)*
5. **Filter taxonomy per project** (the intended tree — filters are owner-maintained along
   with the rest of the project files):
   - **NeuronCore:** Foundation · Memory · Tasking · Telemetry · Serialization · Json ·
     Transport · Wire
   - **GameLogic:** World · Orders · Formation · Universe · Wire · Test Support
   - **NeuronServer:** Host · Sessions · Replication
   - **NeuronClient:** App · Net · Extract · Gpu · Gpu\Passes · Assets · Camera · Input ·
     Hud · Audio
   - **Outpost:** Boot · Config
   Example (`NeuronCore.vcxproj.filters`):
   ```xml
   <ItemGroup>
     <Filter Include="Json"><UniqueIdentifier>{…}</UniqueIdentifier></Filter>
   </ItemGroup>
   <ItemGroup>
     <ClInclude Include="Json.h"><Filter>Json</Filter></ClInclude>
     <ClCompile Include="Json.cpp"><Filter>Json</Filter></ClCompile>
   </ItemGroup>
   ```
6. **Non-code trees are unaffected.** `GameData/` (meshes, universe, audio) and `Design/`
   keep their subdirectories — the directive is about source layout. `Tests/*` projects are
   their own project roots and are flat internally, same rules.
7. **Namespaces do not follow directories** (there are none) and remain as the Dependency Map
   defines: `neuron::core`, `neuron::server`, `neuron::client`, `game`.

## Alternatives rejected

- **Directory-per-subsystem** (the C++ default) — the directive forbids it; it also duplicates
  grouping information in two places once filters exist.
- **`Public/` + `Private/` split** — was floated in the Dependency Map as a future option;
  dropped. Public surface is now defined by the Dependency Map's tables and by what other
  projects include, not by folder membership.
- **Long compound names instead of filters** (`Neuron_Core_Json_Parser.h`) — filters exist
  precisely so names don't have to carry the tree. Rejected.
- **Per-project include roots with unqualified cross-project includes** — works only while
  names never collide, and fails confusingly when they do (silently picking the wrong header).
  Qualified includes are the belt to rule 3's braces.

## Consequences

- A project folder will hold on the order of 30–60 files at MVP scale; the IDE view stays
  organised, the Explorer view does not. Accepted — the directive's trade.
- Filters are hand-maintained: adding a file and forgetting its `<Filter>` entry drops it into
  the root of the IDE tree. Harmless, visible, fixed in seconds — and worth a line in review.
- Renaming for uniqueness is cheap now and expensive later; the registry in §3 is the place to
  add names *before* creating files.
- Any future move to CMake or another generator inherits a flat, unambiguous file set — one
  of the few upsides of this layout.
