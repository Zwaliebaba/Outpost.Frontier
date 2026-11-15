# 01 — Foundation libraries assessment

Paths are relative to `CppSource/`. Everything was read from source; **(guess)** marks anything unverified. No `.ini` files are checked in (`copy_dev_ini.py` copies them from a gitignored `bin\`), so INI *contents* are inferred from the reading code.

Not in this group although the headers were listed: `include/engine/sceneruntime.h` is the SceneRuntime `IEngine` contract; `include/network.h` is the RemoteClient/RemoteServer opcode table (161 opcodes, channel in the high word `0x1/0x2/0x3`). Binary INI ("BINI") lives in `Common/common_ini.cpp`, not here.

## Summary

| Library | Own LOC | Role | Rust destination | Verdict | Effort |
|---|---|---|---|---|---|
| neuroncomponents | 2,843 | Component manager, engine INI parser, name CRC, `FDUMP` log hook, RegExp | `neuron-core::{config, hash, diag}` | Delete the component model; port INI, CRC, stricmp | M |
| system | 628 | `SystemContainer` that instantiates `[System]` components | engine constructor in `neuron-client` | Delete | S |
| NeuronRuntime | 2,339 | Engine bring-up/teardown, DirectPlay8 client/server, GUN list-server wrapper, MD5 | core (`net` framing, MD5), client/server endpoints | Rewrite transport; delete GUN | L |
| zlib | 10 (+27.6k vendor) | DLL wrapper over zlib 1.3.1 + minizip | `flate2` | Delete | S |
| ReadFile | 3,519 | `FileSystem` (DOS / UTF / MEM) and `SearchPath` | `neuron-core::{vfs, utf}` | Port UTF reader; redesign the rest | M |
| luaprofile | 724 (+7.9k Lua) | `ILua` component exposing Lua globals as profile sections | — | Delete (no consumer) | S |
| x86math | 1,491 | `3DMathEngine`: vec/mat/quat kernels in x87 inline asm | `neuron-core::math` | Port bit-exact with golden vectors | M |
| MemoryDiagnostics | 1,160 | Standalone GUI around `umdh.exe`/`gflags.exe` | — | Delete | — |

## 1. neuroncomponents — the component model

One TU (`neuroncomponents.cpp`), `neuroncomponents.def`, `include/neuroncomponents.h`, `include/neuroncomponents_iids.h`.

**Exports:** `NEURONCOMPONENTS_Acquire`, `NEURONCOMPONENTS_GetDllVersion`, `NEURONCOMPONENTS_GetVersion`, data pointer `FDUMP`, `NEURONCOMPONENTS_CRC::{GetCRC32 ×2, GetContinuedCRC32 ×2, CompareStringsI}`, five `LogStream::*` functions with empty bodies (`:2749`), and `stricmp` — an ASCII-only fold returning -1/0/1 (`:2732`).

### How it works

- **Identity.** A COM look-alike with string IIDs: `INeuronComponent { QueryInterface(const char* iid, void**), AddRef, Release }`, `__stdcall`, returning `GENRESULT` (0 OK, -1 … -4 errors). About 96 baseline IIDs of the form `"1.11_IName"`, compared with `strcmp`.
- **Manager.** `NEURONCOMPONENTS_Acquire()` placement-constructs one `FCOManager` in static storage on first call and registers `TearDown` with `atexit` (`:2776`). Its `AddRef`/`Release` return 1. There are no locks.
- **Registration.** Each component DLL calls `RegisterComponent(factory, name, priority)` from `DllMain(DLL_PROCESS_ATTACH)`; 18 source files do so. The registry is `std::map<CRC(name), list<link>>`, each bucket ordered by descending unsigned priority, newest first among equals (`:1206`). Priorities seen: `0x80000000` (ProfileParser, UTF, MEM, SearchPath) and `0x40000000` (the rest).
- **Creation.** `CreateInstance(AGGDESC*, void**)` hashes `desc->interface_name` — a component name such as `"FileSystem"`, not an IID — and offers the descriptor to each factory in the bucket until one returns OK (`:1176`). Factories discriminate on `desc->size` and trailing fields, so the descriptor is a union tagged by size: 8 (`Engine`, `RegExp`, `SearchPath`), 0xC (`3DMathEngine`), 0x14 (aggregating: name, outer, inner-out, extra), 0x30/0x34 (`FileSystem` DOS/UTF), 0x40 (`FileSystem` MEM). `"FileSystem"` has three factories; UTF and MEM are asked before DOS.
- **Aggregation.** As in COM: the 0x14 descriptor carries the controlling outer, the object hands back a non-delegating inner unknown, and its public faces forward `QueryInterface/AddRef/Release` to the outer. The manager aggregates the INI parser this way (`:1148`), which is why `QueryInterface("1.11_IProfileParser")` on the manager works.
- **Lifetime.** Manual refcounts; at zero the object deletes itself in its own module. Several holders QI, immediately `Release`, and keep the borrowed pointer (`system.cpp:165`).
- **INI-driven discovery.** `SetINIConfig(path, flags)` parses the INI, then `LoadConfiguration()` (`:1465`) reads `[NEURONCOMPONENTS] DllPath` (absolute, or relative to the exe directory with `..\` climbing) and `IgnoreNEURONCOMPONENTSEnv`; the `NEURONCOMPONENTS` environment variable overrides `DllPath`. `LoadLibraries()` (`:1579`) passes the first word of each `[Libraries]` line to `AddLibrary` = `SearchPathA(DllPath)` + `LoadLibraryA`. The library name is held in a member buffer during the load so factories registered from that `DllMain` are tagged with it and `RemoveLibrary` can bulk-unregister them.

### Startup and shutdown order (client)

1. Import-time `DllMain`s register factories (neuroncomponents registers `ProfileParser`, `ProfileParser2`, `RegExp`).
2. `NeuronRuntime::Startup(hwnd, ini)` (`NeuronRuntime.cpp:98`): INI path is the argument, else `NEURONCOMPONENTS.INI` in cwd, else beside `NeuronRuntime.dll`. `SetINIConfig` loads `[Libraries]` in file order.
3. `CreateInstance("SystemContainer")` → `Initialize()` → `LoadSystemComponents()` (`system.cpp:334`): each `[System]` line `Name [= Extra]` is created aggregated under the container; then each child's `IAggregateComponent::Initialize()` runs in list order and failing children are silently dropped.
4. `QueryInterface` on the container falls through to each child in order, first OK wins; results fill 24 exported globals (`NeuronRuntime::Sound`, `Pipeline`, `TextureLib`, …).
5. Sound device start, streamer init with the sound manager's `IDirectSound*`; failure calls the host's `CriticalWarningFn(0xd3)`.
6. `CreateInstance("Engine")` → `LoadEngineComponents(container)` → more QIs; `CreateInstance("FxEffectLibrary")`. Then `OpenData(path)` creates the root `FileSystem`/`DOS`.

`Shutdown` (`NeuronRuntime.cpp:277`) releases in fixed order: FxEffectLibrary → Channel, Anim, Renderer, Hardpoint, Deform, Engine2, Engine → TextureLib, MaterialBatcher, MaterialLib → ParticleFx → FxRuntime → VertexBuffer → Streamer → Sound → RPDraw/IB/VB → Pipeline, then `ICOManager::Shutdown()` (drops parser and factory links, `FreeLibrary` in load order). `MatAnimLibrary` and `VMeshLibrary` are never released.

Only the client calls `NeuronRuntime::Startup` (`OutpostFrontier/Client/engine_init.cpp:673`); Server and Common reach the manager directly for `3DMathEngine` and file systems.

### Formats

**Engine INI** (`:333-743`). Tokens are `[`, `]`, `=`, newline and words split on space/tab/CR/LF/`[];=`; `;` comments to end of line; `@include <path>` resolves relative to the including file. Multi-word keys and values are re-joined with single spaces. A line without `=` is a key with a *null* value, distinct from empty. Sections are kept sorted case-insensitively, duplicates retained, lookup returns the first. A key before any section dereferences null. Readers address a section by 0-based line (`ReadLine` yields `"key = value"`) or by key (`GetString`, first case-insensitive match).

**Name CRC** (`:28-92`, `:2687-2723`). Table-driven reflected CRC, init `0xFFFFFFFF`, final NOT, input folded to ASCII lower case. The table is **not** IEEE 802.3 (entry 1 is `0x09073096`, not `0x77073096`), and bytes ≥ 0x80 are "folded" by reading the last 128 bytes of the table itself. On-disk mesh and material keys are these hashes; roughly 130 call sites across OutpostFrontier, SceneRuntime, Common, Shading, Deformable2. Embed the table verbatim.

### Legacy and platform

`SearchPathA`, `LoadLibraryA`, `GetFileVersionInfoA`, `MessageBoxA` (default `FDUMP` shows Abort/Retry/Ignore), `__debugbreak` on INI syntax errors, ANSI paths in `MAX_PATH` buffers. `FDUMP` is a mutable exported function pointer that `Common/common_errorhandler.cpp:391` and `OutpostServer/OutpostServerView.cpp:72` replace.

### Rust mapping

- Replace registry, aggregation and QI with static composition: an `Engine` struct in `neuron-client` whose constructor takes config and owns the subsystems; traits only where a second implementation really exists (render backend, file-system backend). The 24 globals become fields; drop order replaces `Shutdown`.
- Port: INI parser with its quirks under test, `name_crc` and `ascii_stricmp`, a diagnostics sink (`log`/`tracing`) replacing `FDUMP`.
- Delete: `RegExp` (Henry Spencer regexp, ~900 lines, no in-tree consumer), `LogStream`, DLL version probing, `ProfileParser2`.
- Risks: (1) priority-ordered multi-factory dispatch and "first child that answers" are implicit configuration — the real `[Libraries]`/`[System]` lists decide which implementation wins; (2) about a dozen modules read their own settings from the same INI (e.g. `DX8RENDERER.cpp:2607` reads `FPU_PRESERVE`), so its schema is scattered; (3) borrowed-after-Release pointers hide the true ownership graph.

## 2. system

One TU. Registers `"SystemContainer"`; implements `ISystemContainer` (`Initialize`, `Shutdown`, `LoadSystemComponents`, `UnloadSystemComponents`, `AddComponent`) and `IDAConnectionPointContainer` (fan-out to children). `Shutdown()` calls each child's optional `ISystemComponent::Shutdown` in load order. Depends only on neuroncomponents and the CRT. **Rust:** delete — it reduces to "construct the subsystems named in `[System]` in order, initialise, drop". Effort S; the risk is losing the init order.

## 3. NeuronRuntime

Eight TUs (`NeuronRuntime.cpp`, `CDP*.cpp`, `CGunWrapper.cpp`, `md5hash.cpp`); headers `NeuronRuntime.h`, `neuronruntime_core.h`, `neuronruntime_net.h`, `md5hash.h`. Links `neuroncomponents.lib`, `zlib.lib`, `winmm.lib`; needs the DirectX 8.1 SDK for `dplay8.h`.

**Public interface.** `NeuronRuntime::{Startup, OpenData, CloseData, Shutdown}`, the 24 globals, `FatalErrorFn`/`CriticalWarningFn` host hooks, and exported C++ classes `IDPMsgHandler`, `CDPMessage`, `CDPMsgList`, `CDPClient`, `CDPClientProxy`, `CDPServer`, `CGunWrapper`, `MD5Hash`. Class layouts are ABI (`sizeof(CDPMessage) == 0x20`), and header-defined members are re-exported from OutpostFrontier.exe, RemoteClient.dll and remoteserver.dll.

### DirectPlay8 wrapping

| Aspect | Behaviour | Source |
|---|---|---|
| Objects | Client/server only, no peer. `CDPClient` owns `IDirectPlay8Client` (`CoInitialize` + `CoCreateInstance`). `CDPServer` owns `IDirectPlay8Server` and is a process singleton. One `CDPClientProxy` per player, created in `INDICATE_CONNECT` through virtual `CreateClientProxy()` and stored as the player context. | `CDPClient.cpp:38`, `CDPServer.cpp:36`, `:372` |
| Addressing | `CLSID_DP8SP_TCPIP` (DirectPlay's own reliable protocol over UDP). Host is a DirectPlay URL or `x-directplay:/hostname=%s` plus a `port` component; optional client source port. | `CDPClient.cpp:131`, `:253` |
| Session | `DPN_APPLICATION_DESC`: flags 1, max players N+1, session name and password in 260-wchar buffers (unbounded `wcscpy`), password flag 0x80. Default GUID `{A690F026-26F0-4E57-ACA0-ECF868E48D21}`. | `CDPServer.cpp:36-67`, `:103`, `:119` |
| Send flags | Always `0x108` = `DPNSEND_GUARANTEED` (0x8) + 0x100 (**(guess)** `DPNSEND_COALESCE`). Everything is reliable and ordered; no unreliable channel. Asynchronous, completions ignored. | `CDPClient.cpp:188`, `CDPServer.cpp:234` |
| Callback threading | DirectPlay worker threads call the static handlers, which only allocate a `CDPMessage` and append to `IncomingMsgs` (critical section + manual-reset event). Receive buffers are kept zero-copy via `DPNSUCCESS_PENDING` and returned with `ReturnBuffer` after dispatch. The game thread's `DispatchMsgs()` drains the queue and calls `OnReceive/OnConnect/OnDisconnect/OnAddHost`. | `CDPClient.cpp:349`, `:502`; `CDPServer.cpp:327`, `:430` |
| Enumeration | `EnumerateHosts()` broadcasts without limit; `EnumerateHost(host, port, count, retry, timeout)` targets one host. The server answers with up to 0x400 bytes from `SetEnumResponse`. Port-only deviation: responders whose hostname contains `:` (IPv6) are dropped. | `CDPClient.cpp:206`, `:380`; `CDPServer.cpp:365` |
| Keep-alive | Client sends the single byte `0x01` once a second when no ping is outstanding; server replies with one byte `255 - (int)LinkSaturation`. Any 1-byte message is a ping, never data. | `CDPClient.cpp:509`, `CDPServer.cpp:445` |
| Server→client batching | `CDPClientProxy::Send` queues a copy; `SendMsgs` concatenates messages into a 0x400-byte batch with no inner framing and sends the zlib form when input ≥ 0x20 bytes and output ≤ input. An oversized message goes alone after a zero-length packet; ≥ 0x7FFF bytes goes raw. | `CDPClientProxy.cpp:25`, `:46` |
| Client receive | One-shot `inflate(Z_FINISH)` into a 0x8000 stack buffer; anything but `Z_STREAM_END` means "treat as raw". No flag distinguishes the cases. Client→server data is never compressed. | `CDPClient.cpp:530`, `CDPNetCodec.cpp` |
| Stats | `GetConnectionInfo`, `GetSendQueueInfo`, a 3-sample queue-byte history giving `LinkSaturation`. Ping statistics are unsynchronised statics written on the IO thread. | `CDPClientProxy.cpp:186` |

Upward, `CLoginMgr` (OutpostFrontier), `FRemoteServerClient` (RemoteServer), `CRemoteClientHost` and `CRemoteClientProxy` (RemoteClient) subclass these and see only byte buffers and four callbacks.

**GUN.** `CGunWrapper` loads the closed-source `gundll.dll`, calls its `DllGetClassObject` with hard-coded CLSID/IIDs, and drives `IGunConnection/IGunHost/IGunBrowser` (CD-key auth, list-server registration and browsing; several slots still unnamed). Teardown releases on a worker thread joined by `Shutdown()`.

**MD5.** Standard RFC 1321 transform with a non-standard text form, `"%08x-%08x-%08x-%08x"` of the four state words (`md5hash.cpp:219`). Used by Common (18 sites) and Server, **(guess)** for account or file identity.

**Other legacy:** `timeGetTime` everywhere, including static initialisers at DLL load; `IDirectSound*` passed between audio components.

**Rust mapping.** `neuron-core::net` holds message types, framing and the ping byte; `neuron-client::net::ClientEndpoint` and `neuron-server::net::ServerEndpoint` replace the classes, with callbacks becoming an event enum drained by the game loop. Transport candidates: `quinn` (QUIC), a reliable-UDP crate such as `renet`, or plain TCP — all satisfy "guaranteed and sequential". LAN discovery needs a separate UDP responder carrying the 0x400-byte blob. MD5 → `md-5` crate plus the custom formatter. GUN → delete or replace with an HTTP master server. Effort L. Risks: (1) wire compatibility with DirectPlay8 peers is not achievable without reimplementing its protocol; (2) "try inflate, else raw" and unframed batches push message-boundary knowledge into RemoteCommon — a new transport should add an explicit header; (3) higher layers throttle on send-queue depth.

## 4. zlib

`zlib/vendor` is **zlib 1.3.1** (`vendor/zlib.h:40`) plus minizip. It carries no project markers; I could not diff against upstream offline, so "unmodified" is a **(guess)**. Local code is one function: `zlibVersion` is exported as `OutpostFrontierZlibVersion` returning `"1.1.4"` to mimic the shipped 2002 DLL, whose ordinals `zlib.def` reproduces (including `unz*`/`zip*`). Built `ZLIB_WINAPI`.

The only consumer is `NeuronRuntime/CDPNetCodec.cpp`: level 6, zlib-wrapped (RFC 1950) streams, one-shot `Z_FINISH`. Nothing calls minizip. **Rust:** `flate2` inside `neuron-core::net`; byte-identical compressor output is not required.

## 5. ReadFile

One TU plus `include/readfile.h`. Registers `FileSystem` DOS (0x40000000), UTF and MEM (0x80000000), and `SearchPath`. `IFileSystem` has 38 methods shaped like Win32 file APIs (`OpenByDesc`, `Read`, `SeekFile`, `GetSize`, `FindFirst/Next/Close` with `WIN32_FIND_DATAA`, `CreateMapping/MapFileView`, `OpenNode`, `GetFileTimes`, …); 14 are stubs. File systems nest: a DOS directory opens a `.utf` file as a child UTF file system, UTF nodes clone into children, and `SearchPath` is an ordered list of DOS roots split on `;`.

**Backends.** DOS: `CreateFileA`, read-only `CreateFileMappingA`/`MapViewOfFile`, `FindFirstFileA`; writes rejected. UTF: read-only (`:2043`), whole archive memory-mapped, nodes read in place. MEM: growable buffer, the only writable backend, exposes `IDataBuffer`.

**UTF container** (little-endian; header `:266-292`, node access `:331-356`, `:757-806`):

| Header offset | Field |
|---|---|
| 0x00 | magic `0x20465455` (`"UTF "`) |
| 0x04 | version `0x101` |
| 0x08 | tree offset (root node) |
| 0x14 | node size, `0x2C` or `0x0C` |
| 0x18 | string table offset |
| 0x24 | data section offset |

Header is 0x38 bytes; the remaining dwords are not read.

| Node | Layout |
|---|---|
| `0x2C` | `+0x00` next-sibling offset from tree start (0 ends the chain); `+0x04` name offset; `+0x08` flags (0x10 = directory); `+0x10` first-child offset or file data offset; `+0x1C` size; `+0x20/24/28` create/access/write times as DOS date (low word) and time (high word) |
| `0x0C` | `+0x00` bit 31 = directory, low 31 bits = name offset (name at strings + offset + 4); `+0x04` first-child byte offset or data offset; `+0x08` child count or size; children contiguous |

File bytes start at `header.data_offset + node.data_offset`. Name lookup uses neuroncomponents' ASCII `stricmp`. Wildcard matching (`*`, `?`) folds `\` to `/` and, for bytes ≥ 0x80, indexes 128 bytes of unrelated read-only data reproduced as `G_UtfFoldBelowTable` (`:162`).

**Threading and handles.** One `CRITICAL_SECTION` per file system plus a root lock. Handles returned to callers are object addresses cast to `unsigned long` (`:502`) — 32-bit only.

**Rust mapping.** `neuron-core::utf`: bounds-checked zero-copy parser over `&[u8]` (`memmap2`), both node layouts. `neuron-core::vfs`: a small `FileSystem` trait with directory, UTF and memory backends and an ordered overlay replacing `SearchPath`, returning mapped slices or `Arc<[u8]>` instead of handles. Effort M. Risks: path semantics relied on by every loader (case-insensitive, either slash, `..` popping, `.utf`-as-directory traversal), non-ASCII matching quirks, and callers of `CreateMapping` that expect stable pointers into the archive.

## 6. luaprofile

**Purpose: configuration tooling, not game logic.** It registers a component named `"ILua"` whose object implements `ILua::InvokeGlobal` (call a Lua global with tagged arguments) and `IProfileParser` over a Lua state — global tables act as INI sections, string/number fields as `key=value` lines — so a Lua script could stand in for the engine INI. **Nothing in the tree creates `"ILua"`**; other projects include `luaprofile.h` only for the shared `IProfileParser` declaration. Gameplay scripting uses a separate copy in `SceneScript/lua32`.

**Version: PUC-Rio Lua 3.2.2** (`lua32/ORIGIN.txt`), `LUA_VERSION` patched to `"Lua 3.2"`, `double` numbers, base library only. One local patch: `lua32/src/lvm.c:83` formats numbers through `MsvcrtFormatLuaNumber` to reproduce VC6 `%.16g`. Lua 3.2 has a single process-global current state, swapped with `lua_setstate` around every call, so the wrapper is not thread-safe.

**Rust mapping:** delete. `mlua` does not apply: Lua 3.2 syntax and semantics (`nextvar`, tag methods, `%upvalue`) are incompatible with 5.x. `IProfileParser`'s role moves to the INI reader.

## 7. x86math

One TU; registers `"3DMathEngine"`. `I3DMathEngine` has 23 baseline methods plus one port-only (`x86math.h`): matrix inverse/scale/determinant, quaternion and transform multiply, vector transforms (single and list), matrix↔quaternion, `QuatSlerp` (really a normalised lerp with hemisphere test), `GeneralInverse`, `InvSqrt`, `Sqrt`. Types: `FVector` 3×f32, `FMatrix` 3×3 row-major, `FTransform` = matrix + translation, `FQuaternion` (W,X,Y,Z). Included by about 180 files (OutpostFrontier 80, Common 71, Content 15).

**Fidelity.** 37 `__asm` blocks transcribe the original x87 instruction order, including behaviour when `Dst` aliases an input. Only `+ - * /` and `fsqrt` occur, so results depend solely on operation order and the thread's x87 precision control. `Directory.Build.props:42-48` records that the client main thread runs at 24-bit precision after Direct3D 8 `CreateDevice`; the server creates no device, so **(guess)** it stays at the CRT default of 53 bits. The baseline client and server can therefore round the same call differently.

- `InvSqrt`/`Sqrt` use a 1024-byte table built at load (`:30`) plus two Newton steps; `Sqrt(x) = InvSqrt(x) * x`. Low bits differ from `f32::sqrt`.
- List transforms pick one of 27 dot kernels by classifying each matrix coefficient as 0, 1 or other (`:217`, `:642`), changing operation count and rounding.
- Singularity threshold `1e-8f`.

**Rust mapping.** `neuron-core::math`: `#[repr(C)]` structs and plain functions, no trait object. SSE `f32` arithmetic rounds each operation to 24 bits and so matches the client's mode except near overflow/underflow; 53-bit mode can be emulated by computing in `f64` and rounding at each original `fstp dword`. Avoid `glam` for ported call sites if parity matters, and forbid FMA contraction. Effort M. Risks: aliasing semantics, operation order, and choosing which precision mode is the reference.

**Related headers.**

- `x87_trig.h` — `FSIN/FCOS/FPTAN/FPATAN` wrappers (Common 22 includers, IVP 4). These instructions are not correctly rounded and match no libm; exact reproduction on x64 needs a software model, otherwise accept last-bit drift.
- `msvcrt_compat.h` — `atoi`/`_wtoi` with 32-bit wraparound instead of UCRT saturation; data files store hashes such as `2806013121` in decimal and rely on it. 35 includers; trivial and mandatory to port.
- `msvcrt_numeric_format.h` — VC6 `%.16g` and `%.Nf` digit generation (17 significant digits then zero padding, 3-digit exponents), used by both Lua copies and `OutpostFrontier/Client/font_string.cpp:1696`.

## 8. MemoryDiagnostics

A standalone Unicode Win32 GUI (Russian UI strings) that launches or attaches to the game, toggles user-mode stack traces with `gflags.exe`, and captures and compares heap snapshots with `umdh.exe`. Settings live in the `HKCU` registry. Nothing depends on it. **Rust:** do not port; the port-map row assigning it to NeuronCore/NeuronClient should be revisited.

## Cross-cutting

- **Threading.** Everything except the DirectPlay callback boundary and the GUN release threads assumes a single thread.
- **Globals.** `FCOManager`, `FDUMP`, the 24 `NeuronRuntime::*` pointers, `CDPServer::m_pServer`, client ping statics, a function-static `CDPNetCodec` shared by all proxies, `G_DosFactory`, `G_Engine`, Lua's current state.
- **Allocation across DLLs.** All modules share the DLL CRT, so ownership crosses freely (`ILua::InvokeGlobal` returns a `_strdup` string the caller frees). Components self-destruct in their own module through `Release`.
- **SEH, FPU control, registry.** None in the engine libraries of this group. FPU control-word writes exist elsewhere (`OutpostFrontier/Client/client_frame_tick.cpp:88`, RemoteCommon codecs, particlefx) and interact with §7.
- **Oracles.** `ReadFile/utf_walk_once.py` (independent UTF walker), layout `static_assert`s, measured `atoi` vectors in `msvcrt_compat.h`. `CppSource` has **no unit tests** and the Rust crates are empty stubs. The practical oracle is the 32-bit C++ build: generate golden vectors (name CRCs, INI parse dumps, UTF listings, math kernels over random and aliased inputs in both precision modes, MD5 strings, number formatting) and commit them as Rust test fixtures.

## Open questions for the owner

1. **Numeric reference.** Must Rust be bit-identical to the C++ build for simulation math? If so, at which x87 precision — the client's 24-bit or the server's 53-bit?
2. **Network compatibility.** Is interoperability with existing DirectPlay8 clients or servers ever required? If not, which transport, and may the framing change (explicit compression flag, length-prefixed batches)?
3. **Server discovery.** Replace GUN with a new master server, LAN broadcast only, or direct connect only? Is CD-key authentication dropped?
4. **Configuration.** Keep `NEURONCOMPONENTS.INI` with its section and key names, or define a new format with a converter? Can the real `[Libraries]` and `[System]` lists be provided?
5. **Plugins.** Is run-time DLL selection (for example the renderer) still required, or is compile-time composition acceptable?
6. **Removals.** Confirm `luaprofile`, `RegExp` and `MemoryDiagnostics` are dropped.
7. **Data compatibility.** Must unmodified original `.utf` archives and non-ASCII names load with the fold quirks, or may assets be re-exported?
8. **MD5 strings.** Are identifiers from `MD5Hash::AsString` persisted anywhere that must keep working?

## Suggested port order within this group

1. `neuron-core::hash` (name CRC, ASCII stricmp) and `msvcrt_compat` integer parsing — tiny, bit-exact, needed everywhere.
2. `neuron-core::config` — engine INI parser.
3. `neuron-core::math` — after question 1.
4. `neuron-core::utf` and `neuron-core::vfs` — unblocks every asset loader in other groups.
5. `neuron-core::diag` — sink replacing `FDUMP`, `FatalErrorFn`, `CriticalWarningFn`.
6. MD5 and `msvcrt_numeric_format` — when Common/Server and UI text need them.
7. `neuron-core::net` framing, then client and server endpoints — after questions 2–3.
8. Engine composition root in `neuron-client` (replacing neuroncomponents, system, `NeuronRuntime::Startup/Shutdown`) — last.
9. Drop `zlib`, `luaprofile`, `MemoryDiagnostics`, RegExp and GUN from the plan.
