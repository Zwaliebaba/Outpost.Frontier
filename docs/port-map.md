# C++ to Rust port map

The Rust workspace uses five libraries and two thin executable packages.
The table assigns all 39 projects in
[the C++ solution](../CppSource/OutpostFrontier.sln) and its shared source
groups to their destination. These are implementation boundaries for future
ports. The current Rust entry points exercise the crate dependencies.

| C++ source group | Rust destination | Responsibility |
| --- | --- | --- |
| `neuroncomponents`, `system` | NeuronCore | Component startup and lifetime. Express dependencies through Rust modules, constructors, and traits. |
| `NeuronRuntime` | NeuronCore, NeuronClient, NeuronServer | Shared packet and system services in core. Keep rendering and audio handles in the client engine and host services in the server engine. |
| `Common` | NeuronCore, NeuronServer, NeuronClient, GameLogic, GameApp | Put shared configuration, IDs, data and simulation primitives in core. Put persistence and host services in the server engine, presentation helpers in the client engine, and game-specific rules and UI flow in the game crates. |
| `IVP`, `qhull`, `x86math` | NeuronCore | Collision queries, hull geometry, vectors, transforms, and physics. Capture numeric tolerances at the public interfaces. |
| `SceneRuntime` | NeuronCore, NeuronClient | Put shared model hierarchies, transforms, and hardpoints in core. Put scene playback and presentation adapters in the client engine. |
| `ReadFile`, `luaprofile`, `zlib` | NeuronCore | Filesystem and container readers, profile parsing, and compression. |
| `rpclocal`, `RemoteCommon` | NeuronCore, NeuronClient, NeuronServer | Share message definitions and codecs. Keep queue and connection ownership with the engine endpoint that uses it. |
| `RemoteServer` | NeuronClient | Client-side connection to a server and delivery of incoming events. |
| `RemoteClient` | NeuronServer | Server-side connection handling and delivery of events to connected clients. |
| `Server` | NeuronServer, GameLogic | Server lifecycle, sessions, and save storage in the engine. Authoritative world updates, character actions, and game rules in GameLogic. |
| `Content` | GameLogic | Missions, encounters, factions, NPC behavior, and rewards. |
| `particlefx`, `Deformable2`, `surfacematerials` | NeuronClient | Particles, skinned geometry, and surface materials. |
| `DX8RENDERER`, `DX9RENDERER`, `RendComp`, `Shading`, `ximage` | NeuronClient | Graphics device integration, rendering interfaces, draw batching, texture handling, and material animation. |
| `SceneScript`, `movie` | NeuronClient, GameApp | Scene script evaluation and media playback in the engine. Game-specific scene selection and transitions in GameApp. |
| `SoundManager`, `SoundStreamer`, `imeui` | NeuronClient | Audio playback, mixing and streaming, and platform text input. |
| `UiText`, `FieldGuideText`, `DialogueText`, `EquipmentCatalog`, `InterfaceExtras` | GameApp | Client UI, guide entries, dialogue, equipment names, and menu text. |
| `NameCatalog`, `NegotiationText` | GameLogic, GameApp | Game rule and content identifiers in GameLogic, with localized presentation in GameApp. |
| `serverinterface` | GameLogic | Server administration labels and resources. |
| `OutpostFrontier` | GameApp, NeuronClient, `outpost-frontier` | Game and menu flow in GameApp, platform and engine integration in NeuronClient, process startup in the executable package. |
| `OutpostServer` | GameLogic, NeuronServer, `outpost-server` | Administration and game configuration in GameLogic, host lifecycle in NeuronServer, process startup in the executable package. |
| `MemoryDiagnostics` | NeuronCore, NeuronClient | Shared diagnostic interfaces and client-specific instrumentation. |
| `include` | Owning Rust crate | Define shared types in core. Expose engine or game interfaces from the crate that owns them. |

C++ project files and resource import scripts describe build inputs.
Cargo, the Windows workflow, and the Rust crate manifests define the new build.

Keep numeric resource IDs and serialized field widths consistent when a port
consumes an existing format. Design Rust types around the fields and behavior
of that format so the same implementation works on Windows x64.

## Content names

The C++ resources and lookup keys use Helion (He), Vespera (Ve), Dornmark
(Dm), and Orsena (Or) for the major factions. The renamed setting uses
Corin Rellan and the Veilborn. Names stored in external configuration files,
asset nicknames, and DLL lists must agree with the corresponding names in
the C++ tree when running that version.
