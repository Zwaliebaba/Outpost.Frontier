# C++ to Rust port map

The Rust workspace uses six libraries and two thin executable packages.
The table assigns all 39 projects in
[the C++ solution](../CppSource/OutpostFrontier.sln) and its shared source
groups to their destination, following the
[migration plan](migration-plan.md). Per-project detail, work
packages and status are in the [library ledger](library-ledger.md).

| C++ source group | Rust destination | Responsibility |
| --- | --- | --- |
| `neuroncomponents`, `system` | NeuronCore, NeuronClient | INI parser, name CRC, ASCII comparison, FDUMP flags and the error-log channels (`diag`) in core. The component manager and system container are replaced by the client's `Engine` value, which builds the subsystems in a fixed order. |
| `NeuronRuntime` | NeuronCore, NeuronClient, NeuronServer | Wire framing, the transport interface and loopback (`net`), the MD5 text form (`util::md5`) and the host error callbacks (`diag::callbacks`) in core. The QUIC connection in the client and the QUIC host in the server. The DirectPlay and list-server wrappers are not ported. |
| `Common` | NeuronCore, GameSim, GameLogic, NeuronClient, GameApp | IDs, INI, utilities (`util`), timing (`time`), diagnostics (`diag`) and `.sur` reading in core. Game data (`data`: constants and the `[Data]` list, string cache, archetypes, loadouts, fuses, goods, factions and the static vibes, universe, zones with their queries and exclusion zones, asteroid ids and field files, the spatial partitions and the system zone tree, bases, costumes, the new-character database and the performance slider), the object model and world (`object`), equipment and the flight controllers (`equip`), flight AI (`ai`: steering, ray tests, intercepts, obstacle avoidance, the pathfinder, the movement ideas, the behaviour manager and the behaviours, state graphs, personality, the world interface the behaviours use) and wire types in GameSim. Trade and reputation rules in GameLogic. Text rendering in the client engine; client-only helpers in GameApp. |
| `IVP`, `qhull` | NeuronCore | Physics facade: free flight transliterated from IVP (decision O19), collision geometry from `parry3d`, contacts through the project's own solver (`neuron-core::physics`: `ivp` for free flight, `world` and `systems` for the facade, `shape` for the `parry3d` boundary, `golem`, `remote` and `dead_reckoning` for remote correction). Neither library is otherwise ported; the compact-surface layout is read as a file format. |
| `x86math` | NeuronCore | Vectors, matrices, quaternions and transforms in the original operation order (`math`). Common's vector and matrix helpers build on the same types. |
| `SceneRuntime` | NeuronCore | Engine, compound archetypes, `Cons` joints, hardpoints, instance trees, sampling, typed channels, mixer and playbacks (`scene`); channel and script data (`anim`). No renderer dependence. |
| `ReadFile`, `zlib` | NeuronCore | The UTF container (`utf`); the file-system trait, the directory, UTF and memory backends, the search path and the path rules (`vfs`). `flate2` replaces zlib for network batches (`net::frame`). |
| `luaprofile`, `MemoryDiagnostics` | — | Not ported. |
| `rpclocal`, `RemoteCommon` | NeuronCore, GameSim | The loopback transport, field primitives and quantisers (`net`) in core. The message catalog and its codecs in GameSim (`protocol`): one enum per channel, the single-player payload forms selected by a `WireContext`, and the `Common` encodings the messages carry (`MarshalBuffer`, `FmtStr`, costume, binary RDL). The codec copy in `rpclocal` is not ported. |
| `RemoteServer` | NeuronClient | Client-side connection to a server and delivery of incoming events. |
| `RemoteClient` | NeuronServer | Server-side connection handling and delivery of events to connected clients. |
| `Server` | NeuronServer, GameLogic | Server lifecycle, sessions, save storage, accounts and the controller bus in the engine. World updates, objects, players and replication in GameLogic. |
| `Content` | GameLogic | Missions, encounters, factions, NPC behavior, random missions and rewards. |
| `particlefx`, `Deformable2`, `surfacematerials` | NeuronCore, NeuronClient | Data parsing in core. Particles, skinning and surface materials in the client engine. Core modules: `fx_data` (effect and node libraries), `mesh_data::dfm`. Client modules: `render::material::surface` (the 19 surface materials, their schemas and factory, and the engine, instance-value and candidate calls they make); `FLAppearance` (backdrop, starsphere and shadow draws, `render::appearance`) is not ported yet. |
| `DX8RENDERER`, `DX9RENDERER`, `RendComp`, `Shading`, `ximage` | NeuronCore, NeuronClient | Image and mesh parsing in core. The Direct3D 12 renderer, fixed-function emulation, materials, batching and textures in the client engine. Neither legacy renderer is ported. Core modules: `image`, `material_data`, `mesh_data`; the C runtime's `qsort` the batcher sorts with in `msvcrt`. Client modules: `render::ff` (the device model), `render::texture` (texture library, playback, `BigImage`), `render::material` (material regular expressions, library, global records, animation, batcher, the 22 material kinds, `BatchedMaterial`, the factories and the services the materials draw with on the device model). |
| `SceneScript`, `movie` | NeuronClient, GameApp | Lua 3.2 evaluator, scene timeline and Media Foundation playback in the engine. Game-specific scene selection and transitions in GameApp. |
| `SoundManager`, `SoundStreamer`, `imeui` | NeuronClient | XAudio2 playback, mixing arithmetic, streaming, the edit-box model and IME positioning. |
| `UiText`, `FieldGuideText`, `DialogueText`, `EquipmentCatalog`, `InterfaceExtras` | NeuronCore, GameApp | Catalog loader and text-ID lookup in core (`catalog`). Client UI, guide entries, dialogue, equipment names and menu text as catalogs that `cargo xtask catalog` builds from `outdir/data/text/`. |
| `NameCatalog`, `NegotiationText` | NeuronCore, GameApp, GameLogic | Catalogs as above. GameLogic keeps the numeric text IDs and never resolves text. |
| `serverinterface` | `outpost-server` | Strings for the admin console. The MFC dialogs are dropped. |
| `OutpostFrontier` | GameApp, NeuronClient, `outpost-frontier` | Game and menu flow in GameApp, platform and engine integration in NeuronClient (the fonts, their pages and the string paints in `text::font`, rasterised by DirectWrite in `platform::dwrite`; the UI toolkit in `ui`: the Win3D control tree and manager, the Win3D camera, `WinControl`, the factory registry, page descriptors and pages, the screen-quad batcher, widescreen anchoring, the hover pulse, texture shapes, the software cursor and the generic controls; game screens, dialogs and game-specific controls register their factories from GameApp), process startup and in-process server wiring in the executable package. |
| `OutpostServer` | GameLogic, NeuronServer, `outpost-server` | Admin operations in GameLogic, host lifecycle in NeuronServer, config file and command console in the executable package. |
| `include` | Owning Rust crate | Each header follows its library. `x87_trig.h` is not ported. |

C++ project files and resource import scripts describe build inputs.
Cargo, CI, and the Rust crate manifests define the new build.

Keep numeric resource IDs and serialized field widths consistent when a port
consumes an existing format. Design Rust types around the fields and behavior
of that format so the same implementation works on Windows x64. Only layouts
that are formats (files, save text, wire payloads) are preserved; in-memory
layouts of the C++ are not.

## Content names

The C++ resources and lookup keys use Helion (He), Vespera (Ve), Dornmark
(Dm), and Orsena (Or) for the major factions. The renamed setting uses
Corin Rellan and the Veilborn. Names stored in external configuration files,
asset nicknames, and DLL lists must agree with the corresponding names in
the C++ tree when running that version.
