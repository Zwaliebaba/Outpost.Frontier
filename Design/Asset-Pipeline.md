# Asset pipeline: from Unity's formats to native ones

`Cook` (a tool in this repository) turns the owner's installation into native content that the
game loads with no Unity format involved. Its output lives outside the repository; the repository
holds only the tool.

## 1. Principles

1. **Lossless wherever the original was already final.** DXT blocks, mesh vertex data, height
   maps, nav meshes and string tables are copied, not re-encoded. Re-encoding happens only where
   no Windows-SDK runtime can read the original (Vorbis audio, Theora video, compiled shaders).
2. **Keep the original's loading model.** An asset is stored once and shared; a mission group
   (the scenes loaded together, `manifest.json` → `groups`) has a cooked manifest of what it needs,
   as each scene's `requires` list does today.
3. **Stable IDs.** Every original object is identified by (source file, PathID); cooked content
   addresses it by a 64-bit ID derived from that pair, so a reference in a scene, a prefab or a
   script resolves the same way it did in Unity (`m_FileID` through the file's externals table,
   then `m_PathID`).
4. **Every converter has a check** (§4) that runs on the whole content set, not samples.

## 2. Input: two stages

| Stage | Input | Status |
|---|---|---|
| **2a** (now) | `..\DoK-Assets`: AssetStudioModCLI's exports and JSON dumps, `manifest.json`, `Models\` | done, 2026-11-14 |
| **2b** (Decisions D3: after M2) | the installation directly: a Unity 5.2 serialized-file reader in `Cook` (headers are already read by `DoK-Assets\tools\unity_externals.py`) | later, so a cook does not depend on a third-party tool |

Stage 2a is enough to start every converter. Raw bulk data (texture blocks, audio, mesh streams)
is always read from the installation by offset, using the offsets the dumps give
(`"image data": {Offset, Size}`), so no converter depends on a decoded intermediate.

## 3. Per asset type

| Asset | Count | Original | Native format | Converter | Lossless |
|---|---|---|---|---|---|
| Textures | 2,582 | DXT5 1,684, DXT1 719, uncompressed formats 179; 3.0 GB, with mips | **DDS**: BC3/BC1 blocks copied as is; uncompressed as `R8G8B8A8`, `B5G6R5`, `B4G4R4A4`, `A8` (all DXGI formats). Sampler state (filter, wrap, aniso) moves to the material | `Cook tex` | yes |
| Meshes | 2,124 | vertex arrays (positions, normals, tangents, colours, UV0–7), sub-meshes, bind poses, bone hashes, skin, blend shapes | **`.mesh`** (ours): vertex streams in a D3D12-ready layout, one index range per sub-mesh, bounds, skin and blend-shape blocks | `Cook mesh` | yes |
| Skeletons, animation clips, controllers | 443 clips, 56 controllers, avatars | Mecanim: muscle clips (streamed, dense, constant), avatars, state machines | **`.skel`**, **`.anim`** (keyframes per bone channel), **`.animctl`** (states, transitions, parameters) | `Cook anim`; the FBX models in `DoK-Assets\Models` are the visual reference | clips: up to Unity's own sampling (Risks R5) |
| Materials | 1,179 | shader + properties + textures | **`.mat`**: shader ID, constant block, texture IDs, sampler states | `Cook mat` | yes |
| Shaders | 217 | compiled only: D3D9 SM3 assembly, D3D11 SM4 bytecode | **HLSL source, compiled with `dxc` at SM 6**, in this repository; one per original shader actually used, behind a property-mapping table | by hand, from the disassembly, ordered by how many materials use each (Plan 5.3) | rewritten |
| Scenes and prefabs | 81 scenes, 128,029 GameObjects | GameObject/Transform hierarchies with components | **`.scene`**: entities, transforms, and one record per component type the rebuild supports; references as stable IDs | `Cook scene` | yes |
| Component data (MonoBehaviour) | 91,123 of 2,468 script classes | Unity serialization of C# fields | typed records: **C++ structs and loaders generated from the decompiled C# class definitions** (field order and types), for the classes the rebuild uses | `Cook schema` + `Cook scene` | yes |
| Particle systems | 18,748 | Shuriken modules (curves, gradients, emission, shape, ...) | **`.fx`**: module records for the modules used | `Cook fx` (it also reports module usage, which scopes the runtime) | yes |
| Audio | 1,632 clips (270 streamed); 6.0 GB as PCM, ~1 GB as Vorbis | Vorbis (FSB5) | **WAV PCM** for short clips (XAudio2 plays it natively); streamed music and dialogue as **xWMA**, re-encoded offline (Decisions D5, tool T2) | `Cook audio` | short clips yes; streamed clips re-encoded once |
| Audio events | Fabric components (~13,000) | Fabric component trees in scenes and prefabs | **`.audio`**: event → component tree (random, blend, sequence, switch, 3D source, parameters) | `Cook audio-events` | yes |
| Video | 27 | Theora + Vorbis (`.ogv`), 0.5 GB | **MP4: H.264 + AAC**, which Media Foundation plays natively; transcoded offline with ffmpeg (Decisions D6, tool T1) | `Cook video` (calls the offline transcoder) | re-encoded |
| String tables | 7 languages | CSV `NumberId,StringId,Text,CharacterId,FabricEventString,FabricFilePath` | **UTF-8 table per language**, keyed by `StringId`, with the dialogue columns kept | `Cook text` | yes |
| Fonts | 4 | Unity Font with embedded font data | **TTF/OTF** as embedded; rendered through DirectWrite into atlases at runtime | `Cook font` | yes |
| UI atlases | NGUI `UIAtlas` and sprites | atlas texture + sprite rects, borders, padding | **`.atlas`** + its DDS | `Cook ui` | yes |
| Simulation height maps, nav meshes | 1 per map each (`HeightMapBytesFile_*`, `*_navmesh`) | Blackbird binary formats | **copied byte for byte**; read by the port of `NavMeshData` / `HeightMapAttributesData` | `Cook sim` | yes |
| Simulation attributes | `BBI.Game.Data` types (units, weapons, abilities, buffs, research, ...) in data assets (e.g. `BuffSetAttributesAsset`, 583) | ScriptableObject-style MonoBehaviour assets | **`.attr`**: the attribute records, `Fixed64` values converted exactly as `UnsafeFromFloat` does at load | `Cook sim` | yes (the conversion is the original's) |
| Scene entities for the sim | `ConvexPolygonObstacle` 3,687, `SceneResourceEntity` 677, `TriggerCircleEntity` 664, `SceneAIHintEntity` 713, ... | components in the design scenes | simulation descriptors, as `BBI.Unity.Game` builds them at load | `Cook sim` | yes |
| Mission scripts | 82 uScript graph components; 1,431 generated classes | **generated C#** (the graphs did not ship) | **`.graph`**: nodes, parameters, connections, variables, recovered from the generated code (Decisions D4) | `Cook script` (reads `DoK-Decompiled`) | yes, if recovery is complete |
| PlayMaker FSMs | 87 | FSM data in the component (states, transitions, actions and their parameters) | **`.fsm`** | `Cook script` | yes |
| Cutscenes | CinemaDirector (~4,500 track and item components) | timelines in scenes | **`.cutscene`**: tracks, items, curves | `Cook cutscene` | yes |
| Global illumination | WorldArt scenes' `GI\` folders; 1 baked lightmap | Enlighten real-time GI data | **not portable**; replaced by our own baked lighting (Risks R3) | `Cook light` (a bake) | no |

Native formats marked "ours" get a versioned header (magic, version, content hash of the inputs),
so a stale cook is detected at load.

## 4. Checks

| Converter | Check, over the whole content set |
|---|---|
| tex | every DDS's block data equals the original's bytes; dimensions, format and mip count match |
| mesh | vertex and index counts, bounds, and a hash of each stream match the dump |
| scene | object and component counts per scene match `manifest.json`; every reference resolves |
| schema | every field of every used script class is read; a round trip (cooked record → JSON) equals the dump |
| fx | every module in use is supported, or the cook fails and lists it |
| audio | duration and sample count per clip; for compressed streams, a decoded RMS difference under a threshold |
| text | row counts per language; every `StringId` the UI references exists |
| sim | height maps and nav meshes byte-equal; attribute values equal the original's loaded values (captured by the harness, [Verification.md](Verification.md)) |
| script | every node type in every graph is implemented, or the cook fails and lists it |
| all | the cook is reproducible: two cooks of the same install give identical output |

## 5. Content layout (cooked)

```
<content>\
  manifest.json                     groups → scenes → required assets (by ID)
  pool\<type>\<id>.<ext>            every asset once
  groups\<group>\<scene>.scene      e.g. Campaign\M04\M04-WorldArt.scene
  sim\<map>\                        height map, nav mesh, scene entities, attributes
  text\<language>.strings
  video\<name>.mp4
```
