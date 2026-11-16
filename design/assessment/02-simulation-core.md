# 02 — Simulation core assessment (Common, IVP, qhull)

Scope: `Common` (175 files, 104.9k lines), `include/common_*.h` (68 headers, 37.7k), `IVP` (100 files, 49.6k), `qhull/vendor` (24.9k). Nothing was built or run. "Guess" marks anything not read in code. Paths are relative to `CppSource/`.

## Summary

- Common is one DLL holding data loaders, the space object model, equipment, the flight AI and the physics facade. AI is 36% of it (51k lines).
- The client and the server both link almost all of it. The Rust graph (`game-app → neuron-client → neuron-core`, `game-logic → neuron-server → neuron-core`) gives them only `neuron-core` in common, so ~78k lines of shared game simulation (object model, equipment, AI) have no home today (Open question 1).
- Physics is **state-synchronised, not lockstep**. Each side runs its own IVP world; the owning client reports position/orientation and peers apply "remote correction". Bit-exact physics is not a network requirement.
- The original is not deterministic run to run (pointer-keyed hashes, three x87 precisions), so a faithful IVP port cannot be validated bit-for-bit.
- qhull runs at runtime only for boxes and one pyramid. All model hulls are precompiled in `.sur` files.
- Three string→ID hashes and one procedural ID packer must be bit-exact. All are small and specified below.

## 1. Common decomposition

LOC = `Common/` sources + matching headers. Consumers from `#include` and symbol greps: `OutpostFrontier` (C), `Server` (S), `Content` (Ct), `OutpostServer` (OS), `rpclocal`/`Remote*` (R).

| Sub-module | Key files (`common_*`) | LOC | Consumers | Class |
| --- | --- | ---: | --- | --- |
| IDs, string cache | `id`, `stringcache`, `subobjectid`, `tstring.h` | 0.8k | C 53 files, Ct 30, S 10 | shared |
| INI, files, config | `ini`, `textfile`, `file`, `msvcrt_atof`, `dataconfig`, `constants` | 3.7k | C 75, Ct 45, S 5, OS, R | shared |
| Utilities | `misc`, `heap`, `timing`, `rect`, `crspline`, `watchable`, `basewatcher`, `statemachine`, `errorhandler`, `async`, `mpcritsec`, `metalist.h`, `weighted_vector.h` | 4.9k | all; `StateMachine` is Ct-only (197 refs); `ErrorHandler`/`Async` C-only | shared, with client-only and Content-only pockets |
| Archetypes, loadouts | `archetype*` (6), `carchgroup`, `loadout`, `equipdesc`, `hardpointsummary`, `hierarchy`, `fuses`, `damagelist` | 12.2k | C 51, S 4, R; `HardpointSummary` C-only (47 refs, S 0) | shared |
| Universe, zones, asteroids, bases | `universe*` (3), `zone`, `exclusionzone`, `casteroid*` (3), `spatial`, `basedata`, `roomdata`, `roominfo`, `baseproperties` | 13.2k | C 49, S 5, OS 1; zones heavy in Ct via `pub::` | shared |
| Goods, reputation, trade | `goodinfo`, `reputation`, `vibe`, `tradeoffer`, `repairmanager`, `goods.h` | 5.3k | C 63, Ct 9, S 6, R; `TradeOffer` S+R only | shared; trade server-only |
| Object model | `cobject`, `engineobject`, `cobjdb`, `csimple`, `cship`, `csolar`, `cloot`, `cmine`, `cprojectile`, `cattachment`, `iobjinspectimpl`, `csys`, `tractorarm`, `thrustmanager`, `gunconvergence`, `iface.h` | 15.3k | C 49 files (`IObjRW` 509 refs), S (`CShip` 159) | shared |
| Equipment | `cequip`, `ce*` (13), `ceqobj`, `csteering` | 11.3k | C 23, S 5 | shared |
| Physics glue | `physys*` (4), `physicalsystem`, `phyarch`, `phycollisionstate`, `phycontroller`, `phantom`, `controllergolem`, `remotephysics`, `deadreckonedvector`, `sphereintruder`, `ivp_*.h` | 10.4k | C 13 (`PhySys::` 118 refs), S 6 (123); phantoms S 104 / Ct 64 / C 1 | shared; phantoms server-side |
| AI | 23 behaviours, 10 ideas, `behaviormanager`, `aisteer`, `aiintercept`, `aifirecontrol`, `airaycast`, `obstacleavoidance`, `pathfind`, `targetmanager`, `systemmanager*`, `laneaction`, `launchaction`, `pub`, `*stategraph` | 51.2k | Behaviour headers included only inside Common. `IBehaviorManager` C 67 / S 56 refs; `pub::AI` Ct 168 | shared (NPC AI on server, autopilot on client) |
| Text | `fmtstr`, `rdlio`, `rdlnodes`, `renderdisplaylist`, `textrender` | 6.5k | `FmtStr` everywhere incl. R 156; RDL/TextRender C 445, S 21 | `FmtStr` shared; rendering client |
| Costume, animation, new character | `costume`, `animdb`, `actiondb`, `newchar` | 2.7k | Costume all (on the wire); `AnimDB` C 17 / S 3; `ActionDB` C-only | shared |
| Net and server interface types | `marshalbuffer`, `playeraccount`, `net.h`, `server.h`, `server_types.h`, `server_controller.h`, `server_playerdb.h` | 4.9k | `server.h` Ct 57 / C 43; `server_playerdb.h` S-only; `server_controller.h` Ct 23 | wire types shared; `SERVER_API` items belong to Server |

Outgoing dependencies: `neuroncomponents.h` (80 includes: CRC, `stricmp`, `FDUMP`), `x86math.h` (71), `NeuronRuntime.h` (61; `NeuronRuntime::Engine` 165 uses, `Anim` 47, `Data` 28), `x87_trig.h` (22), `readfile.h` (10), `md5hash.h`, `scenescript_iface.h`. The object model sits directly on the engine instance tree: `CObject : EngineObject : IEngineInstance` (`include/common_cobject.h:83,161`), on the server as well.

## 2. Public interface of Common.dll

**Export mechanism.** `__declspec(dllexport)` through `COMMON_API` (`include/common_exports.h`): 977 uses, 250 of them class-level. No `.def`. `SHARED_INLINE_API` (22 uses) marks inline members the baseline emitted separately into Common, Server and the exe. The highest baseline ordinal cited in headers is 3858, so the export table has at least ~3.9k names (lower bound; the exact count needs a build). `Common/parse_exports.py` diffs against the baseline DLL's export table. Whole STL instantiations are exported (`extern template class COMMON_API std::vector<EquipDesc>`, `std::list<DamageEntry>`, `std::vector<IObjInspect*>`), and functions pass `std::map&`/`std::wstring` across DLLs. None of this ABI carries over; the Rust crates need designed interfaces.

**Hierarchies** (no virtual inheritance anywhere; RTTI is used: `dynamic_cast` Common 3, Server 25, client 10):

| Tree | Shape | Hazard |
| --- | --- | --- |
| World objects | `IEngineInstance → EngineObject → CObject → {CSimple → {CProjectile → {CMunition → {CGuided, CBeam}, CMine, CCounterMeasure}, CLoot, CAsteroid, CEqObj → {CShip, CSolar}}, CEquipmentObj, CDynamicAsteroid}` | `CShip : CEqObj, PhySys::Controller` (MI). `CObject::Class` is a bitmask; "is-a" is `(class & mask) == mask` (`common_cobject.h:165-181`: Simple 0x03, EqObj 0x103, Ship 0x503, Solar 0x303, Loot 0x803, Beam 0x67). Server downcasts by mask, unchecked |
| Equipment | `CEquip → {CInternalEquip, CExternalEquip → CAttachedEquip → {CELauncher → {CEGun, droppers}, CEShield, CEThruster, …}}` | MI: `CEEngine : CInternalEquip, CPhysControllerEquip`; `CEThruster`; `CEScanner : …, ScanList::Filter` |
| Inspection interfaces | `IObjRW : IObjInspect, IObjAffect, Watchable`; `IObjInspectImpl : IObjRW, CBase` | Three-base MI. `common_iustategraph.cpp` recovers objects with a hard-coded "−8" this-adjustment. No virtual destructor |
| Archetypes | `Archetype::Root → {EqObj → {Ship (+`CommReferrable`), Solar}, Equipment → 25 leaves, Asteroid, DynamicAsteroid}` | Plain data; maps to a Rust enum |
| AI | `Behavior` → 23 behaviours; `BaseIdea` → 10 ideas; `DockBehavior : …, IObjAffect::RequestCB`; `LaunchAction : IAction, PhySys::Controller, pub::AI::DirectiveCallback` | Callback MI; fixed-offset state blocks |
| Physics | `PhySys::Controller : IVP_Controller_Dependent`; `CRemotePhysicsController : IVP_Controller_Golem, CRemotePhysicsSimulation` | IVP types leak into game classes, Server and the client |

Lifetime is manual: intrusive watcher chains (`Watchable`/`BaseWatcher`), raw pointers, refcounts.

**ID schemes** (`Common/common_id.cpp`):

| Function | Width | Algorithm |
| --- | --- | --- |
| `CreateID(name)` → `CFLIDMaker::CreateStaticID` (`:258`) | 32 | Copy ≤255 bytes, fold ASCII `A–Z` to lower (bytes ≥0x80 untouched). Table (`:205`): CRC width 30, poly 0x8005, reflected; equivalent closed form `x=i; 8×: x = (x&1) ? (x>>1) ^ (0xA001<<14) : x>>1; T[i] = x & 0x3FFFFFFF` (I transliterated both to Python and the tables match). `h=0; h = T[(h&0xFF)^c] ^ (h>>8)`. Finish (`:136`): `(bswap32(h & 0x3FFFFFFF) >> 2) \| 0x80000000`. Null → 0; empty → 0x80000000 |
| `MakeId(name)` (`:405`) | 16 | Copy ≤127 bytes, same fold. Table is CRC-16 poly 0x1021 built MSB-first (`w=i<<8; 8×: w = (w&0x8000) ? (w<<1)^0x1021 : w<<1`), but the update is LSB-style: `crc=0xFFFF; crc = T[(crc&0xFF)^c] ^ (crc>>8)`. A hybrid, not a catalogued CRC-16. Faction ids; collisions occur (`common_reputation.cpp:241`) |
| `MakeLocationID(base, name)` (`:393`) | 32 | `CreateID(sprintf("%X_%s", base, name))`, upper-case hex, 256-byte buffer |
| `AllocateObjectID()` (`:294`) | 30 | Counter from 1, `& 0x3FFFFFFF`. High bit clear = dynamic, set = static hash |
| `NEURONCOMPONENTS_CRC::GetCRC32` (`neuroncomponents/neuroncomponents.cpp:28-84,2687`; used at 17 Common sites for hardpoint and archetype path keys) | 32 | Init `0xFFFFFFFF`, `crc = T[fold(c) ^ (crc&0xFF)] ^ (crc>>8)`, final `~`. `T` is a 256-entry **non-IEEE** table to copy verbatim. `fold` lowercases ASCII; bytes ≥0x80 map to the last 128 bytes of the table itself (signed-char quirk) |
| `compute_cube_id(pos)` (`common_casteroid.cpp:51`) | 32 | Per axis `t = trunc(v>0 ? v+0.1f : v−0.1f)` evaluated in double; `g = \|t\| >> 7`; reject `gx,gz ≥ 0x400`, `gy ≥ 0x200`; sign bits 0x400/0x200/0x400; id = `gz<<10 \| gy \| gx<<21`. Asteroid orientation uses a local MSVCRT LCG (`seed*214013+2531011`, `>>16 & 0x7fff`) |
| `SubObjectID` (`common_subobjectid.cpp:75-95`) | 16 | Ranges: 0–2 reserved, 3–0x20 collision groups, 0x21–0xfffe equipment, 0xfff0–0xfffe shields |

Test vectors from my Python transliteration of the C++ (not from the original binary): `CreateID("li01") = 0x83A78E4F`, `CreateID("a") = 0xA00C1407`, `MakeId("li_n_grp") = 0x224A`, `MakeId("") = 0xFFFF`.

`CHARACTER_ID` is a char buffer compared with `stricmp`.

## 3. Data formats parsed here

| Format | Where | Layout |
| --- | --- | --- |
| Text INI | `common_ini.cpp:661-780`, `common_textfile.cpp` | Memory-mapped. `[section]`; `key = v0, v1 ; comment`; section names beginning with `;` are skipped. Name and value buffers are 0x400 bytes; values split on `,`. `ParseNumber` (`:214`) accepts `0x…`, suffixes `d`/`b`/`h`, `'c'` char literals, fractional hex. The free `ValueNum` (`:328`) calls MSVCRT `atof` directly, so `0x10` reads as 0 there. `msvcrt_atof.cpp` reimplements the old CRT `atof` from its power tables |
| BINI | `common_ini.h:343-384`, `common_ini.cpp:44,388` | Little-endian, `pack(1)`. Header 12 bytes: `u32 'BINI' (0x494E4942)`, `u32 version` (not checked), `u32 string_table_offset` (must be < file size). Then sections `{u16 name_off, u16 value_count}`, each value `{u16 name_off, u8 param_count}`, each param 5 bytes `{u8 type, 4-byte payload}`; type 0 byte, 1 i32, 2 f32, 3 string offset. String table runs to EOF, NUL-terminated. Detected by magic on `open`; `open_memory` never recognises BINI (`:570`) |
| `.sur` collision | `common_phyarch.cpp:139-292` | `"vers"` tag + `f32` 2.0 (anything else is rejected). Then until EOF: `u32 part_id`, `i32 chunk_count`, chunks by 4-char tag: `surf` (`u32 size` + raw `IVP_Compact_Surface` blob, 16-byte aligned), `exts` (2×3 `f32`), `!fxd` (no payload), `hpid` (`i32 n` + n×`u32`). Unknown tag aborts. Several reads are unchecked |
| IVP compact surface | `IVP/ivp_collision/ivp_compact_surface.h`, `ivp_compact_builder/ivp_compact_ledge.h` | Self-relative offsets, no pointers. Surface header 0x24 (`mass_center[3]`, `rotation_inertia[3]`, `upper_limit_radius`, `u32 size<<8 \| max_deviation`, `i32 offset_ledgetree_root`). Tree node 0x1c (`offset_right_node`, `offset_compact_ledge`, `center[3]`, `radius`, `u8 box_sizes[3]`). Ledge header 16 (`point_offset`, client data = part id, `i16 n_triangles`); triangle 16; edge 4; points 16-byte `float[4]`. Loads unchanged on x64 |
| Binary RDL | `common_rdlio.cpp:654-784` | Records `{i32 type, u32 length}` + payload. Types: 1 attributes, 2 UTF-16 text, 4 position, 5 paragraph, 6 style |
| `FmtStr`, `MarshalBuffer` | `common_fmtstr.cpp:281`, `common_marshalbuffer.cpp` | Network and mission-log encoding: tag + flags + payload tree; raw host-order copies of 1/2/4-byte scalars |
| RDLX / XML | `common_rdlio.cpp:1011-1300` | Parsed through MSXML SAX2 COM |
| `AI\state_graph.db` | `common_pubstategraph.cpp:152` | AI state-graph table (layout not read; guess: text) |

UTF/tree containers are read through `IFileSystem` (ReadFile; see 01-foundation), not parsed here.

## 4. IVP

**Flavour.** Ipion Virtual Physics (pre-Havok), float build, compiled straight into Common.dll (`Common.vcxproj`; no IVP project, no `IVP_*` exports). It is a reconstruction with 32-bit layouts asserted throughout; some fields are still "no reader traced".

**Subsystems present** (only what Common linked): environment and PSI event queue; cores and real objects (`IVP_Polygon` over compact surfaces, `IVP_Ball`); OV-tree broadphase; mindist closest-feature tracking with continuous time-of-impact (14k lines); impact and friction solvers with the "great matrix" LCP; controllers (motion, golem); phantoms; ray solver; sphere query; listeners; anomaly limits; one material; merge/split of objects into a core. **Absent:** constraints, springs, actuators, vehicles, buoyancy.

**How Common wraps it.**
- `PhySys` namespace: ~60 free functions keyed by `CObject*` or system id (`include/common_physys.h:446-614`).
- One `PhysicalSystem` per star system owns an `IVP_Environment` (`common_physicalsystem.cpp:286-343`): zero gravity, look-ahead and anomaly limits from `[PhySys]` constants.
- Bodies come from `.sur` surfaces via `PhySurfaceManager` (`common_physys_build.cpp:83`), which hides ledges whose part id is no longer attached. `Attach`/`Detach` merge and split cores.
- Controllers subclass `PhySys::Controller` and apply force/torque per PSI in `do_simulation_controller(IVP_Event_Sim*, …)`: `CShip`, `CEEngine`, `CEThruster`, `CSteering`, nudge/strafe engines, lane/launch/jump-in actions, the golem remote-correction controller, and Server's mines and guided missiles.
- A collision listener fills a per-tick `CollisionEvent` vector; filtering is by collision-group string.
- Queries: `FindRayCollisions`, `FindRayIntersect`, `FindSphereCollisions`, phantoms (sphere, box, part), `GenerateCollisions(CBeam)` (beams are rays, not bodies).
- IVP types leak: Server and the client implement controllers and touch `IVP_Core`/`IVP_Real_Object` (7 files).

**Rate.** PSI = `1 / PHYSICAL_SIM_RATE`, default 16 Hz (`common_constants.cpp:244`, INI-overridable; IVP stock is 66 Hz). `PhysicalSystem::Update` (`common_physicalsystem.cpp:346-394`): single-player clamps `Dt` to 0.0999 and calls `simulate_dtime` once; multiplayer accumulates in a global double and steps in 0.0999 chunks. IVP runs continuous collision events between PSIs.

**Where.** The client always simulates its active system (`OutpostFrontier/Client/space_state.cpp:1054`). The server simulates in multiplayer only (`Server/IServerImpl.cpp:4030`); in single-player it skips (`:3827`). My reading: Server runs inside the client process there and shares the client's Common instance and world.

**Authority.** State sync. The owning client sends `SSPObjUpdateInfo` (id, quaternion, position, timestamp; `include/common_server_types.h:254`); the server applies `PhySys::RCSet` (`Server/server_classes.cpp:6564`) and relays; receivers drive remote objects through `RCBegin/RCSet/RCEnd` (`OutpostFrontier/Client/client_iclient_space.cpp:630,1041,1231`) and `CRemotePhysicsSimulation` dead reckoning. I found no cross-machine comparison of simulation state.

**Determinism.** Not present even in the original:
- Hash keys are object addresses (`IVP/ivp_physics/ivp_ov_element.cpp:221`); `MovingObjects` is `std::set<CObject*>` (`Common/common_physys_internal.h:85`). Pair and sync order follow heap layout.
- x87 precision differs by process and moment: the dedicated server sets 53-bit (`OutpostServer/OutpostServer.cpp:331`); the client main thread is left at 24-bit by Direct3D 8 (`Directory.Build.props` comment); `convert_pointsoup_to_compact_ledge` sets 64-bit and never restores it (`IVP/ivp_compact_builder/ivp_surface_builder_pointsoup.cpp:687`).
- `FSIN/FCOS/FPTAN/FPATAN` via `x87_trig.h` (148 uses in scope).
- Process-wide `rand()` shared by AI and effects.

What does need cross-machine agreement: the ID hashes, `compute_cube_id`, the asteroid LCG and INI number parsing.

**Options.**

| Option | Cost | What breaks or is at risk |
| --- | --- | --- |
| A. Faithful port of the linked subset | XL (~50k lines of pointer-graph code; arenas or heavy `unsafe`; 32-bit layouts to unwind) | Cannot be proven faithful: no deterministic baseline to diff against, and x64 SSE cannot reproduce three x87 precisions |
| B. `rapier3d` behind a `PhySys`-shaped trait | L | Flight feel needs retuning: IVP damping (linear + per-axis angular), speed clamps, one friction/elasticity material. Collision damage (`CalcCollisionDamage`: relative velocity and normal) shifts with different contact normals. 16 Hz PSI with IVP's in-between motion has no direct equivalent. Controllers become pre-step force callbacks; the golem controller is ported by hand. String collision groups need a filter hook. Attach/detach becomes compound-collider rebuild; per-ledge part ids become collider user data |
| C. `parry3d` for geometry + a small custom integrator and impulse solver | L | Same tuning risk as B, but PSI semantics, damping and clamps are ours, so original constants carry over. Zero-g needs no stacking or resting contact. More code to own; fast small bodies need explicit continuous collision |

**Assessment.** Do not port IVP. Port the seam: the `PhySys` API, `[PhySys]` constants, the `.sur`/compact-surface reader (for hull vertices and part ids), the golem and remote-correction controllers, and dead reckoning. I lean to C over B: the game's behaviour is expressed in IVP-specific knobs (PSI rate, damping vectors, speed clamps) that C can honour literally. B is the faster route to something flying. Either way physics is validated by scenario tolerances, not bit equality.

## 5. qhull

- Version 2002.1 (`qhull/vendor/README.txt`), C, static library, global `qh` state, `setjmp/longjmp` error exits.
- One call site: `qh_new_qhull(3, n, coords, False, "qhull Qs Pp C-0 W1e-14 E1.0e-18", …)` with a joggle retry (`QJ`, 1e-12 growing ×1.2 to 0.02) at `IVP/ivp_compact_builder/ivp_surface_builder_pointsoup.cpp:494,511`.
- Reached at runtime from exactly two places in Common: the default part's 5-point pyramid, once (`common_physys.cpp:437-463`), and each box phantom's 8 corners (`common_phantom.cpp:219-266`; mission trigger volumes from Server/Content). `FLedgeConvexHullBuilder` would also call it for multi-ledge soups, but Common only ever inserts one ledge (guess that no other path exists; the two call sites are all I found).
- Model hulls are precomputed in `.sur`.

Do not port qhull. Under option B/C a box phantom is a cuboid sensor and the default part a fixed five-vertex hull; `parry3d` has a general hull if one is ever wanted.

## 6. Hazards

- **Globals and singletons.** `CFLIDMaker::m_pTheMaker`, `G_PhysicalSystems`, `G_PhySysSimAccumulator` (one accumulator for all systems), the string cache, `GNewCharDatabase`, `GGoodInfoList`, archetype and universe maps, static scratch buffers (`ID_String::get_string`, INI `GStringIndexScratch`, `MarshalBuffer`'s 0x4000 default). `SinglePlayer()` and `IsMPServer()` are each consulted 26 times inside Common.
- **Static init order.** Load-bearing in `common_stringcache.cpp:7`, `common_equipdesc.cpp:7`, `common_constants.cpp:7`, `common_goodinfo.cpp:8`.
- **Threading.** None created here. `Async::AddWork` runs the work synchronously (`common_async.cpp:61`). Everything assumes one thread; qhull and IVP globals enforce it.
- **Allocators.** `Heap` free-list pool (`common_heap.cpp`); `ivp_malloc_aligned` keeps the raw pointer at `aligned[-1]`; VC6 "new returns null" semantics relied upon.
- **Inline asm / x87.** `x87_trig.h` is `__asm` and `#error`s off x86. The sources already spell out which intermediates the baseline kept at double versus stored as float; that is a usable `f64`/`f32` spec for Rust at 53-bit, but the client's 24-bit mode and the transcendentals will still differ in the last bits.
- **Pointer size.** 2,208 layout `static_assert`s (1,805 Common, 403 IVP); pointers cast to `uint32_t` as hash keys (10 sites); engine handles are `long` in Common and `void*` in `IEngine` (`common_hierarchy.cpp`, `common_archetype.cpp:629`).
- **Structs copied raw.** `.sur` extents and surfaces, BINI views, RDL records, RPC structs in `common_server_types.h` (padding bytes never written).
- **Baseline UB reproduced on purpose.** Unchecked `.sur` reads, `Part::operator=` double free, a `PhySys_FindSystem` miss writing into the map head node. Rust must pick defined behaviour for each.
- **Strings.** MBCS build; `char` nicknames; UTF-16 `wchar_t` in `FmtStr`, RDL and player names; `tolower` on sign-extended chars.
- **STL across DLLs.** See section 2.

## 7. Legacy Windows APIs used

File mapping (`CreateFileA`, `CreateFileMappingA`, `MapViewOfFile`); timing (`timeGetTime` ×13, `QueryPerformanceCounter`, `GetTickCount`, an RDTSC CPU-speed probe in `Timing::GetMachineSpeed`); registry (`RegOpenKeyExA`/`RegQueryValueExA`/`RegSetValueExA`/`RegCreateKeyExA`) plus `GetUserNameA` and MD5 for the machine/account hash (`common_playeraccount.cpp`); `SHGetFolderPathA`; named events `Microsoft…ClientCanRunEvent`/`…ServerCanRunEvent` (`common_misc.cpp:118`); `MessageBoxA`, `OutputDebugStringA`, `FormatMessageA`; GDI text metrics (`CreateFontIndirectA`, `GetDC`, `CreateCompatibleDC`, `SelectObject`, `GetTextExtentPoint32W`, `GetVersionExA`) in `common_textrender.cpp`; COM + MSXML SAX2 (`CoCreateInstance`, BSTR) in `common_rdlio.cpp`; `CRITICAL_SECTION` (`MPCritSec`); `_control87`.

## 8. Tests and verification hooks

Existing: no unit tests in the C++ tree. Layout `static_assert`s, `parse_exports.py`, a `DebugASan` configuration, tick profilers (`common_tick_profile.h`), the `FDUMP` log sink, `CRemotePhysicsSimulation::tracef`.

Golden-vector capture points (pure, deterministic I/O), in value order:

1. `CreateID`, `MakeId`, `MakeLocationID`, `GetCRC32` over every nickname in the shipped data plus edge cases (empty, >127/>255 bytes, bytes ≥0x80, mixed case).
2. `INI_Reader` as an event stream (section, key, typed values) over every shipped `.ini`, text and BINI; `ParseNumber` and `MsvcrtAtof` as string → f64 bit patterns.
3. `PhyArch::LoadSurfaces`: part ids, extents, hardpoint ids, surface blob hash, for every `.sur`.
4. Archetype, loadout, goods, universe, zone, base and reputation databases dumped canonically after load.
5. `compute_cube_id`, `compute_asteroid_id`, `compute_cube_orientation(seed, …)`.
6. `MarshalBuffer` and `FmtStr::flatten/unflatten` and binary RDL round trips.
7. Zone shape tests and distances (`common_zone.cpp`), spatial partition queries, `CRSpline`, `Rect`.
8. `CDeadReckonedVector` / `CRemotePhysicsSimulation` (sample sequence → predicted state).
9. `CalcCollisionDamage`, reputation and price formulas; `random_*` helpers under a seeded MSVCRT LCG.
10. Physics and AI: recorded scenarios through the `PhySys` facade with tolerance envelopes (trajectory, time to dock), not bit equality.

Capture 2, 5 and 7 at 53-bit precision (the dedicated-server setting) and record that choice.

## 9. Rust mapping

| Sub-module | Crate / module | Effort | Notes |
| --- | --- | :---: | --- |
| IDs, string cache | `neuron-core::id` | S | Newtypes `NicknameId(u32)`, `FactionId(u16)`, `SubObjId(u16)` |
| INI, BINI, atof, constants, data config | `neuron-core::ini`, `::config` | M | Quirk-exact parser; mmap → `Vec<u8>` |
| Utilities | `neuron-core::util`, `::time` | S | Drop `Heap`, `Async`, `MPCritSec`; MSVCRT LCG as an explicit RNG value |
| Archetypes, loadouts, fuses | `neuron-core::archetype` | L | Enum of kinds; engine handles behind a trait |
| Universe, zones, asteroids, spatial, bases | `neuron-core::universe`, `::zone`, `::spatial`, `::asteroid` | L | Zone and asteroid math is float-sensitive |
| Goods, market data | `neuron-core::goods`; rules in `game-logic` | M | |
| Reputation, vibe | data in `neuron-core`, rules in `game-logic` | M | |
| `FmtStr`, marshal, costume, wire structs | `neuron-core::wire` | M | Explicit little-endian field codecs |
| RDL, text render | `neuron-client::text` | M | Replace GDI and MSXML |
| `.sur`, `PhySys` facade, remote correction, dead reckoning | `neuron-core::physics` | L | Backend per section 4 |
| Object model | **undecided** (see question 1) | XL | Class bitmask → enum + component data; watcher chains → ids/handles |
| Equipment | with the object model | L | |
| AI | with the object model; `pub::AI` surface in `game-logic` | XL | 51k lines; heavy float and `rand()` use |
| Phantoms, trade offers, `StateMachine`, player DB types | `game-logic` / `neuron-server` | S–M | |
| `HardpointSummary`, `ActionDB`, `ErrorHandler`, RTC slider | `game-app` / `neuron-client` | S | |

Top risks:

1. Crate graph cannot host shared simulation code (question 1).
2. AI volume and fidelity: no deterministic oracle, 51k lines.
3. Physics replacement changes flight feel and collision damage.
4. Object model rewrite: MI, mask downcasts, intrusive lifetimes, engine-instance coupling.
5. Float fidelity where it does matter (asteroid ids, zone membership, INI numbers).

## Open questions for the owner

1. **Where does shared simulation live?** Client and server both need `CObject`…`CShip`, equipment and the behaviour manager (~78k lines), and single-player runs Server and Content inside the client process (`include/common_weighted_vector.h:104-106`, `OutpostFrontier/Client/client_objdb.cpp:926`). The Rust graph has no edge between `game-app` and `game-logic`. Options: put it in `neuron-core` (engine crate absorbs game rules), add a shared `game-sim` crate, or let `game-app` depend on `game-logic`.
2. **Is single-player in-process hosting kept**, or does single-player become a loopback dedicated server? This decides whether one physics world is shared (as today) or two exist.
3. **Physics fidelity target:** "feels the same within tolerance" (options B/C) or a faithful IVP port (A)? Is there recorded reference footage or telemetry to tune against?
4. **Numeric reference:** which precision is canonical for golden vectors, the server's 53-bit or the client's 24-bit?
5. **Network compatibility:** must the Rust build interoperate with C++ clients/servers (wire-exact `FmtStr`, `MarshalBuffer`, RPC structs), or only read the same data files and saves?
6. **Baseline bugs:** reproduce observable quirks (unchecked `.sur` reads, 16-bit faction id collisions, INI oddities) or fix them where data does not depend on them?
7. **Account identity:** keep the registry + machine-hash scheme in `common_playeraccount.cpp`, or replace it?
8. **Constants:** is `PHYSICAL_SIM_RATE` overridden in the shipped `constants.ini`?

## Suggested port order within this group

Leaf first; each step depends only on earlier ones and on 01-foundation (`math`, name CRC, filesystem).

1. `id` (hashes, sub-object ranges) with vectors.
2. `ini` + BINI + `atof` + constants + data config.
3. Utilities: RNG, timing, rect, spline, string cache replacement.
4. Wire codecs: `MarshalBuffer`, `FmtStr`, costume, RPC structs (needed early by the network group).
5. `.sur` loader and compact-surface decoder.
6. Archetypes, loadouts, fuses (need 1, 2, 5 and an engine-handle trait).
7. Universe, zones, exclusion zones, spatial, asteroid ids, bases and rooms.
8. Goods, market, reputation data.
9. Physics facade and chosen backend; dead reckoning and remote correction; phantoms.
10. Object model core (`CObject`, `CSimple`, attachments, `CEqObj`), then equipment, then `CShip`/`CSolar`/projectiles.
11. AI bottom-up: steering and ideas → obstacle avoidance and pathfinding → target/fire control → behaviours (idle, goto, face first; dock, formation, trade-lane last) → `pub::AI` and state graphs.
12. Client-side text (RDL, text render), `AnimDB`, hardpoint summaries.

Steps 1–8 are data and pure functions with golden-vector coverage and can start before questions 1–3 are answered. Steps 9–11 are blocked on them.
