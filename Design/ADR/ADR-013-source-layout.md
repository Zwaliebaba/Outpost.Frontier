# ADR-013 — Source Layout: Flat Project Directories, Grouping via VS Filters

**Status:** Accepted · 2026-08-17 (owner directive) · amended 2026-08-18 (§1a, owner directive:
shaders under `Outpost/Shaders`, compiled into `Outpost/CompiledShaders`) · §1a further
amended by [ADR-018](ADR-018-scaling-baseline.md) (2026-08-19): the shader compiler is
dxc, SM 6.x, in both configurations (D12)
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
1a. **Two exceptions, both in `Outpost`, both owner directives** (2026-08-18): `Outpost/Shaders`
   holds the HLSL, and `Outpost/CompiledShaders` holds the byte-array headers the HLSL compiler
   generates from it — **`dxc` at SM 6.7 in both configurations** since ADR-018 D12
   (2026-08-19); it was `fxc` at SM 5.1 here and 6.7 in Debug, which is the fork that decision
   closed. Rule 1 is otherwise unchanged and applies to every other file in every project.

   The directive that produced rule 1 says *no subdirectories in the code*, and the reading
   that survives is the one where these are not that: `Shaders/` is HLSL rather than C++, and
   `CompiledShaders/` is build output rather than source — it is in `.gitignore`, and a fresh
   clone contains only the directory and the note explaining it. Neither is a subsystem folder
   of the kind rule 1 exists to prevent, and neither takes a `.cpp` out of the project root.

   Rules 3 and 3a still apply to the generated headers, which is not a formality:
   `CompiledShaders` is on `Outpost`'s include path, so `OpaqueVS.h` shares a flat namespace
   with every other header this project can reach. The names are the shader file names, so
   uniqueness is bought the same way it is everywhere else — by naming the thing well.

   The related move: shaders were `GameData/Shaders/*.hlsl`, compiled at boot by
   `D3DCompileFromFile` under §6's "assets are the client's own business". They are built now.
   §6 still holds for meshes, universe files and sound banks; shaders left the category.

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
   | NeuronCore | plain area names | `Debug.h` `Log.h` `Clock.h` `Hash.h` `Random.h` `OwnerThread.h` `OwnerThread.cpp` `Arena.h` `RingBuffer.h` `TaskPool.h` `Telemetry.h` `ByteReader.h` `ByteWriter.h` `Json.h` `JsonWriter.h` `EntityRecord.h` `OrderIntent.h` `Transport.h` `QuicTransport.h` `Wire.h` `FileSys.h` `FileSys.cpp` `NeuronHelper.h` |
   | GameLogic | type + family names | `Ids.h` `ShipClass.h` `World.h` `ReplicatedView.h` `Orders.h` `Validate.h` `Formation.h` `Eta.h` `WorldHash.h` `Snapshot.h` `OrderMessages.h` `SchemaHash.h` `Universe.h` `UniverseParse.h` `UniverseGen.h` `WorldRegistry.h` `Transfer.h` `Station.h` `StationMessages.h` `EventRecord.h` |
   | NeuronServer | type names | `Simulation.h` `ServerHost.h` `ServerConfig.h` `Session.h` `SnapshotSender.h` |
   | NeuronClient | type names (`Gpu`, `Hud`, `Audio` read as domain words, R4) | `ClientApp.h` `ClientConfig.h` `WorldView.h` `Window.h` `ClearColour.h` `ClientConnection.h` `SnapshotBuffer.h` `RenderWorld.h` `GpuCom.h` `GpuDevice.h` `GpuSwapChain.h` `GpuUploadRing.h` `GpuMeshes.h` `GpuNebula.h` `GpuPasses.h` `GpuPipelines.h` `ObjMesh.h` `NebulaField.h` `GlyphAtlas.h` `IsoCamera.h` `Picking.h` `Selection.h` `OverlayMark.h` `InputMap.h` `UiDrawList.h` `UiLayout.h` `ToastStack.h` `HudRoster.h` `HudPalette.h` `DebugStrip.h` `GhostLane.h` `CommandRow.h` `OrderPuck.h` `OrderGhost.h` `AudioSystem.h` `AudioBank.h` `AudioListener.h` |
   | Outpost | — | `Main.cpp` `AppConfig.h` `AppConfig.cpp` `ConfigLoad.h` `ConfigLoad.cpp` `UniverseLoad.h` `UniverseLoad.cpp` `ReplicatedWorldView.h` `ReplicatedWorldView.cpp` `ShaderTable.h` `ShaderTable.cpp` `SelfTest.h` `SelfTest.cpp` `TickSoak.h` `TickSoak.cpp` `UniverseBake.h` `UniverseBake.cpp` |
   | Outpost/Shaders | stage suffix on the pass name | `OpaqueVS.hlsl` `OpaquePS.hlsl` `NebulaVS.hlsl` `NebulaPS.hlsl` `OverlayVS.hlsl` `OverlayPS.hlsl` `UiVS.hlsl` `UiPS.hlsl` — plus `Opaque.hlsli` `Nebula.hlsli` `Overlay.hlsli` `Ui.hlsli` `FrameConstants.hlsli` `PassConstants.hlsli` for what stages share |
   | Outpost/CompiledShaders | generated; one per `.hlsl`, same stem | `OpaqueVS.h` `OpaquePS.h` `NebulaVS.h` `NebulaPS.h` `OverlayVS.h` `OverlayPS.h` `UiVS.h` `UiPS.h`, each defining `g_p<stem>` |

   If a genuine collision ever appears, the **newer** file is renamed to a more specific type name; the
   table above is the registry to check first.

   **Some rows reserve a name rather than describe a file**, which is the point of §4: the
   registry has to be written *before* the file exists or it cannot prevent anything. Not yet in
   the tree, as of S13 — `Session.h` and `SnapshotSender.h` (both still
   structures inside `ServerHost.cpp`) and `AudioSystem.h`, `AudioBank.h`, `AudioListener.h`
   (S15). Everything else listed is real. `QuicTransport.h`, reserved here since the start,
   became real at S13 — and `UdpTransport.h` left the registry with the file, deleted by the
   S13 owner directive (QUIC only, ADR-003 §4). `HudLayout.h` was reserved here for S11's roster
   half and is now struck from the row: S11a built `UiLayout.h` for the zones and S11b
   `HudRoster.h` for the rows, so the reserved name describes no file anyone intends to
   write. A reservation that outlives its plan is worse than no reservation -- it is a name
   the next author avoids for nothing. The
   Dependency Map's per-project tables carry the same names and mark the same rows *(planned)*;
   they are two views of one list, and updating one without the other is how both go stale.

3b. **The rule is about names, not only file names.** `Neuron::INVALID_ENTITY_ID` was declared
   in `NeuronCore/EntityRecord.h` as `u16` and again in `NeuronClient/RenderWorld.h` as `u32`;
   the two files have different names, so §3's check passed, and every translation unit that
   saw both failed with C2371. Namespace-scope constants share one flat namespace exactly the
   way file names share one flat include path, and for the same reason — so CI now fails the
   build on a constant declared in two engine headers. Class members are exempt: they are
   scoped by their class and may repeat freely.

3a. **Names must also be unique against the CRT, the STL and the Windows SDK — case-insensitively.**
   This is not hypothetical: `NeuronCore/Time.h` shadowed `<time.h>` the moment the folder
   joined an include path, and `<ctime>` produced two dozen errors deep inside the STL with
   nothing pointing at our file. `Assert.h` had the same latent collision with `<assert.h>`.
   They are now `Clock.h` and `Debug.h`, and a CI step fails the build on any header whose
   stem matches a standard one. Check a new header's bare name against the CRT before
   creating it — the sibling repository learned this the same way, with eight of its 87
   de-prefixed names colliding.
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
   - **Outpost:** Boot · Config · Shaders

   *The taxonomy is about what a file is for, not what it is named.* `OrderPuck` is **Input**
   (it turns pixels into a plane point) and `OrderGhost` is **Extract** (it decides what is on
   screen), even though both begin with the same word — grouping them together because of the
   prefix would put a gesture and a draw list in one folder and teach the wrong thing about
   where each belongs.

   **Filters are IDE metadata and the IDE rewrites them.** Visual Studio regenerated
   `Outpost.vcxproj.filters` during the shader move (§1a) and dropped every `<Filter Include>`
   definition, leaving four items referencing a `Shaders` filter that no longer existed — a
   file that still builds and vanishes from the tree. Worth a glance after opening the
   solution; it is not a set-once decision.
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
   keep their subdirectories — the directive is about source layout. Shaders were in this
   category and are not any more; see §1a. `Tests/*` projects are
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
- §1a's exception is a precedent, and the thing to watch. It is defensible because neither
  directory holds C++ the flat rule was written about, and because `CompiledShaders` is not in
  source control at all. A third subdirectory holding `.cpp` files would not be defensible on
  either ground, and should be refused on this line.
