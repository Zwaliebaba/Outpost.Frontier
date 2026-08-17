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
   path, uniqueness is what keeps includes unambiguous. Identifier and file *naming* is
   governed by [AGENTS.md](../../AGENTS.md) §1 (PascalCase, named for the primary type or type
   family — R7); this ADR adds only the uniqueness requirement and the registry. Names are
   made unique by naming the type well, not by decorating the file: `GpuDevice`, `HudRoster`
   and `AudioSystem` are genuine type names that happen to disambiguate.

   | Project | Naming | Files |
   |---|---|---|
   | NeuronCore | plain area names | `Log.h` `Time.h` `Hash.h` `Random.h` `Arena.h` `RingBuffer.h` `TaskPool.h` `Telemetry.h` `ByteReader.h` `ByteWriter.h` `Json.h` `JsonWriter.h` `Transport.h` `UdpTransport.h` `QuicTransport.h` `Wire.h` |
   | GameLogic | type + family names | `Ids.h` `ShipClass.h` `World.h` `ReplicatedView.h` `Orders.h` `Validate.h` `Formation.h` `WorldHash.h` `Snapshot.h` `OrderMessages.h` `SchemaHash.h` `Universe.h` `UniverseParse.h` |
   | NeuronServer | type names | `ServerHost.h` `ServerConfig.h` `Session.h` `SnapshotSender.h` |
   | NeuronClient | type names (`Gpu`, `Hud`, `Audio` read as domain words, R4) | `ClientApp.h` `Window.h` `ClientConnection.h` `SnapshotBuffer.h` `RenderWorld.h` `GpuDevice.h` `GpuSwapChain.h` `GpuUploadRing.h` `GpuPasses.h` `GpuPipelines.h` `ObjMesh.h` `GlyphAtlas.h` `IsoCamera.h` `Picking.h` `InputMap.h` `HudLayout.h` `HudRoster.h` `OrderPuck.h` `AudioSystem.h` `AudioBank.h` `AudioListener.h` |
   | Outpost | — | `Main.cpp` `AppConfig.h` `AppConfig.cpp` `ConfigLoad.h` `ConfigLoad.cpp` |

   If a genuine collision ever appears, the **newer** file is renamed to a more specific type name; the
   table above is the registry to check first.
4. **Include style — per-project include roots, unqualified includes** (owner decision, and
   already how the projects are configured): each `.vcxproj` lists the libraries it is
   entitled to as `$(SolutionDir)<Project>`, so `#include "Json.h"` reaches NeuronCore and
   `#include "Transport.h"` reaches it too. Project-qualified includes
   (`#include "NeuronCore/Json.h"`) were considered and **not** adopted.
   The trade this accepts: **rule 3 carries the whole weight.** With several roots on the
   search path, a duplicate file name resolves to whichever root comes first, silently and
   with no diagnostic. Adding a name to the registry before creating the file is therefore not
   bookkeeping — it is the only thing preventing a wrong-header build.
   Note this makes `$(SolutionDir)` load-bearing for any build that is not launched from the
   solution: CI passes `/p:SolutionDir=` explicitly for exactly this reason.
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
   defines: `Neuron` (engine libraries) and `Game` (GameLogic).

## Alternatives rejected

- **Directory-per-subsystem** (the C++ default) — the directive forbids it; it also duplicates
  grouping information in two places once filters exist.
- **`Public/` + `Private/` split** — was floated in the Dependency Map as a future option;
  dropped. Public surface is now defined by the Dependency Map's tables and by what other
  projects include, not by folder membership.
- **Long compound names instead of filters** (`Neuron_Core_Json_Parser.h`) — filters exist
  precisely so names don't have to carry the tree. Rejected.
- **Project-qualified cross-project includes** (`#include "NeuronCore/Json.h"` off a single
  `$(SolutionDir)` root) — proof against name collisions, and rejected by the owner: the
  projects already carry per-library include paths and the qualification buys little once
  rule 3 is enforced. The residual risk is recorded in §4 rather than argued again.

## Consequences

- A project folder will hold on the order of 30–60 files at MVP scale; the IDE view stays
  organised, the Explorer view does not. Accepted — the directive's trade.
- Filters are hand-maintained: adding a file and forgetting its `<Filter>` entry drops it into
  the root of the IDE tree. Harmless, visible, fixed in seconds — and worth a line in review.
- Renaming for uniqueness is cheap now and expensive later; the registry in §3 is the place to
  add names *before* creating files.
- Any future move to CMake or another generator inherits a flat, unambiguous file set — one
  of the few upsides of this layout.
