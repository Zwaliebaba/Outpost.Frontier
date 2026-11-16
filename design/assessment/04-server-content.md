# 04 — Server and Content assessment

Scope: `CppSource/Server` (15 `.cpp`, 39.5k LOC), `CppSource/Content` (110 files, 78.5k LOC), `include/server_*.h`, `content.h`, `common_server*.h`. Read-only review; nothing was built or run. Paths are relative to `CppSource/`. Line numbers are for key claims; anything not read directly is marked **guess**.

Context: the tree is a function-by-function reconstruction of a shipped 2003 binary. Behaviour, including bugs and x87 rounding, is deliberately preserved; containers are modern `std::`, not VC6 layouts.

## 1. Server decomposition

### Lifecycle

| Phase | What happens | Where |
| --- | --- | --- |
| Startup | Zero time base, `srand(timeGetTime())`, `CreateID` self-test, `Players.init`, `LoadLibraryA(ModulePath)` + `GetClientInterface`/`GetChatClientInterface`, `DataConfig::startup(IniPath)`, read `[OutpostFrontier] initial_world`, load controller DLLs from `[Initial SP DLLs]`/`[Initial MP DLLs]`, MP-only world-state load, `BaseDB::init`, `SysDB_Initialize` (one `StarSystem` per universe system), fuse DB load, `IClient::Startup(MaxPlayers, Port)` | `Server/IServerImpl.cpp:204-252`, `:2827`, `:2924`, `:3337` |
| Tick | Host calls `Update()` (pumps `IClient`) then `ElapseTime(dt)`: add wall-clock delta to the server clock, step systems, pump phantoms and dying ships, `BaseDB::update`, controller heartbeats, one-shot timers | `IServerImpl.cpp:270-284` |
| Shutdown | Log out all, tear down fuses and SysDB, `CObject::ReleaseAll`, BaseDB, destroy controllers in descending id order, `DataConfig::shutdown`, client DLL free, `Players.free` | `IServerImpl.cpp:254-268`, `:4166` |

### Tick rate and time stepping

- **No fixed step.** The dedicated host targets 30 Hz (`GServerFramePeriod = 0.0333333351f`, `OutpostServer/OutpostServer.cpp:64`), measures real elapsed time with `Timing::read_ticks` (QPC), sleeps the remainder, and passes the measured seconds as `dt` (`:371-391`). Single player passes the client frame delta (`OutpostFrontier/Client/server_connect.cpp:155-163`, `MaxPlayers = 1` at `:138`).
- Only systems containing an active player are stepped, each once per frame (`Server_StepSystems`, `IServerImpl.cpp:4081`).
- `StarSystem_Update` (`:3802`): multiplayer clamps the sub-step to 0.2 s and loops until `dt` is consumed (double accumulator, float step); single player takes one step of `dt`. Per step: tick seven object lists, ship-proximity pass, asteroid fields, deferred deletes, dock/land queue; multiplayer only adds beam moves, `PhySys::Update` and the collision pass. Afterwards each player's `Observer` is updated and changed affiliations are sent.
- **Three clocks**: the `dt` argument; the server clock `g_ElapsedTimeAccum + GServerTimeBase`, which accumulates *wall-clock* `Timing::Delta::read_dt()` and not `dt` (`IServerImpl.cpp:276-279`); and Content's own clock, the sum of GameSupport's 1 s heartbeat payloads (`Content/GameSupport.cpp:598`, `:780`).
- Heartbeat: `accum += dt`; when `accum >= period` the controller gets one message carrying `floor(accum/period)*period`. The initial phase is `rand()/32768 * interval` (`IServerImpl.cpp:3195`). Timers: at most 128 fire per frame (`:4293`).

### World, objects, ownership

`SysDB::SysMap` (`std::map<uint, StarSystem>`, exported) owns every system. A `StarSystem` owns its objects through `MetaList` blocks, plus a deferred-delete list, a dock queue and a per-player `Observer` list. Server objects are thin `GameObject`/`EqObj`/`Ship`/`Solar`/`Loot`/`Mine`/`Guided`/`Bullet`/`CounterMeasure` wrappers (`IObjRW`) around Common's `CObject` family. Content drives spawning through `pub::SpaceObj::Create*`. Objects changing system sit in an in-transit list (`server_classes.cpp:11839-11943`).

### Sessions, players, accounts

`Players` (exported `PlayerDB` global) holds a `MaxPlayers`-sized array of `PlayerData`, indexed by 1-based session id. `PlayerData` carries the wide account name, every saved-character record of the account, and live cash, equipment, costume, reputation, visit map, mission log and trade state. Login returns a status byte (3 = success, `IServerImpl.cpp:517-545`) after enumerating `*.fl` in the account directory (`server_classes.cpp:3824`). `CPlayerGroup` and the chat router (`server_chat.cpp`) sit on top.

**Economy is not simulated.** Prices are static per-base data from Common (`pub::Market::GetPrice`, `server_pub.cpp:7786`). Buy and sell are client-driven (`ReqAddItem`, `ReqChangeCash`, …); the server forwards `GFGoodBuy`/`GFGoodSell` as controller events (`IServerImpl.cpp:1139-1154`) and polices the result with a worth-based cheat check (`PlayerData_TotalEquipmentValue`, `server_classes.cpp:1122`; `PlayerData_CheckForCheats`, `:1214`). Server `BaseDB` (`server_basedb.cpp`) is the per-base/per-room broadcast store for bar NPCs, ambient scripts, mission-vendor offers and news; Content fills it.

### Save and character files

| Item | Finding |
| --- | --- |
| Root | `SHGetFolderPath(CSIDL_PERSONAL)` + `My Games\OutpostFrontier` (`Common/common_misc.cpp:1002`), then `Accts` (SP) or `Accts\MultiPlayer` (`server_pub.cpp:796`) |
| SP layout | `Accts\SinglePlayer\{Restart,AutoSave,Save%04x,<name>}.fl`. The directory is `wcstombs(account name)`; the client logs in as `L"SinglePlayer"` (`OutpostFrontier/Client/login_mgr.cpp:287`). `Restart.fl` is created from `NewPlayer.fl` beside the exe (`server_classes.cpp:3489-3508`). Unnamed saves probe `Save%04x` from `rand()`, up to 200 tries (`server_savegame.cpp:704`). Mission saves are copied to `AutoSave.fl` (`:733-752`) |
| MP layout | `Accts\MultiPlayer\<acct>\<char>.fl`, both names `sprintf("%02x-%08x", wcslen, crc)`. Per account: `name`, `password`, `banned` marker. Root: `accounts.cfg` (raw `u32 salt, u32 server id, char[0x24], char[0x24]`, `server_classes.cpp:387-446`). Written to `temp.fl`, then unlink + rename (`server_pub.cpp:1976-1978`) |
| Format | Text INI, LF-only. `[Player]` keys include `description`, `tstamp`, `name` (wide chars as `%04x` hex), `rank`, `house`, `rep_group`, `money`, `num_kills`, costume, `system`, `base`, `pos`, `rotate`, `ship_archetype`, `equip`, `cargo`, `base_*`, `wg`, `visit`, `log`, `interface` (`server_pub.cpp:1168-1900`; reader `server_savegame.cpp:346-620`) |
| SP-only tail | `[Time] seconds`, reputation feelings (Common), then controller event `SaveGame` (`server_pub.cpp:1083-1088`) |
| Content sections | Appended through the raw `FILE*` in event `PlayerSave`: `[StoryInfo]`, `[MissionState]`, `[TriggerSave]`, `[MsnShipSave]`, `[MsnSolarSave]`, `[RandomMission]`, `[mPlayer]`, `[Mission01aSave]`, … |
| Templates | `NewCharacter.fl` / `MPNewCharacter.fl` beside the exe, expanded with `%%Name%%`, `%%Package%%`, `%%Faction%%`, … (`server_savegame.cpp:1084-1270`) |

**Container ("FLS1"), exact algorithm** (`Server/server_savefile.cpp:32-49`, `:135`, `:150-213`):

1. Write plaintext INI with `fopen(path, "w+t")`, read it back through the same text-mode handle (so CRLF collapses to LF).
2. For plaintext index `i` from 0: `c[i] = p[i] ^ (((key[i & 3] + (i & 0xFF)) & 0xFF) | 0x80)` with `key = "Gene"` (`47 65 6E 65`). XOR, so the same routine decodes.
3. Reopen `"wb"`, write the four bytes `FLS1`, then the ciphertext. The index excludes the magic.

It applies to **all** saves, SP and MP (`server_pub.cpp:1936-1953`). Readers accept `FLS1` (decode from offset 4), reject `FLSG`, and treat anything else as plain INI (`server_savegame.cpp:369`, `:998`).

**Account hash, exact** (`server_pub.cpp:895-932`): table-driven reflected CRC, width 31, polynomial `0x8005`, init 0, xor-out 0; per byte `crc = T[(crc ^ b) & 0xFF] ^ (crc >> 8)`; result `byteswap32(crc & 0x7FFFFFFF)`. Input is the **lowercased UTF-16LE** name, two bytes per code unit. `password` is the wide password XORed with the low 16 bits of its own CRC (a verification token, not reversible; `server_classes.cpp:4169-4207`). `name` is XORed with the low 16 bits of the CRC of `L"AKL!8167fGBA0712"` (`:3228`).

### Sub-modules

| Sub-module | Files | LOC (approx.) |
| --- | --- | --- |
| Lifecycle, `IServer` handlers, controller registry, timers, system step | `IServerImpl.cpp` | 4,380 |
| Player DB, sessions, accounts, cheat/worth checks | `server_classes.cpp:1-4440`, `:4970-5225` | 4,700 |
| Groups and chat | `server_classes.cpp:4439-4970`, `server_chat.cpp` | 1,330 |
| Save container, serializer, loader, templates | `server_savefile.cpp`, `server_savegame.cpp`, `server_pub.cpp:770-2070` | 2,860 |
| StarSystem, per-player Observer, create records | `server_classes.cpp:6050-12211`, `server_observer.cpp`, `server_createinfo.cpp` | 7,700 |
| Server asteroid fields | `server_classes.cpp:5225-6050` | 830 |
| Object wrappers and munitions | `server_gameobject.cpp`, `server_loot.cpp`, `server_mine.cpp`, `server_guided.cpp`, `server_countermeasure.cpp` | 8,320 |
| Fuse DB and fuse actions | `server_fuse.cpp` | 1,270 |
| Base/room broadcast DB | `server_basedb.cpp` | 1,360 |
| `pub::` API façade | `server_pub.cpp` (rest) | 6,750 |

## 2. Server.dll public interface

`Server/server_export_symbols.txt` (dumpbin of the baseline DLL): **524 exports**. Rough split by name: `pub::*` about 293, `IServerImpl` 100, `PlayerDB` 49, `CPlayerGroup` 30, `CAccount` 9, others about 43 (`SrvAsteroid`, `CDeadReckonedVector`, `CRemotePhysicsSimulation`, `MD5Hash`, `SysDB::SysMap`, three chat hook pointers, `GetServerInterface`, `GetChatServerInterface`, data export `Server`).

- **`IServer`**: 92 pure virtuals (`include/server_iserver.h`), plus `PushToServer` and `IChatServer::SubmitChat`: object commands, lifecycle, login/characters, client-pushed ship state (`ReqShipArch`, `ReqEquipment`, `ReqSetCash`, …), base/market, launch/jump/dock, SP-only collision and update reports (`SPObjUpdate`, `SPMunitionCollision`, …), save, trade, opaque state blobs. The `SP*` and `Req*` methods show how client-authoritative single player is.
- **Content hook**: `[Initial SP|MP DLLs]` rows `DLL = <dll>, <instance>, <priority>` → `LoadLibrary` → `CreateInstance(id, name, parms)` → `IDLL` with one virtual, `HandleMessage(EMessage, void const*)` (`include/content.h`, `IServerImpl.cpp:3233`). Broadcast in priority order, or to one controller. Creation during dispatch is deferred (`g_ReentrancyGuard`); destruction is queued to the next heartbeat walk.
- **Messages**: 63 server-sent ids in `0x00–0x41` and 18 Content-sent ids in `0x1000–0x1011`, all with untyped `void const*` payloads (`include/common_server_controller.h`).
- **`pub::`** symbol counts by namespace (mangled names, approximate): SpaceObj 62, Player 57, GF 34, AI 25, Reputation 20, Zone 16, System 10, Market 9, Phantom 6, Controller 4, Audio 3. Content references about 244 distinct `pub::` names.

## 3. Content decomposition

`CreateInstance` knows 23 controller names (`Content/Content.cpp:84-108`): GameSupport, SpaceSupport, BaseSupport, DestroyMission, SpacePop, BasePop, StoryLauncher, Story, Mission_01a, Mission_01b, Mission_02…13, GenericMission.

| Sub-module | Main files | LOC |
| --- | --- | --- |
| Entry, factory, state-machine message router, data path | `Content.cpp`, `ContentMessageRouter.*`, `ContentDataPath.cpp`, small headers | 840 |
| Mission script engine (triggers, conditions, actions) | `MissionScript.*`, `MissionCnd.*`, `MissionAct.*` | 11,920 |
| Mission object, `[ObjList]` directives, save, dialogs, RTC | `MissionObject.*`, `MissionSave.cpp`, `MissionObjList.*`, `MissionDialogs.*`, `RtcScripts.*` | 9,360 |
| Story progression and per-mission classes | `Story.cpp`, `StoryLauncher.cpp`, `StoryMissions.cpp` | 1,080 |
| Random missions (DB, offer generator, vignette tree, runtime machines, DestroyMission) | `RandomMission*`, `DestroyMission.cpp`, `RandomEncounters.*` | 16,130 |
| Encounters and population (zones, densities, spawn placement, patrol paths, shortest paths) | `Encounters.*`, `EncounterParams.*`, `Populators.*`, `SpawnPlacement.cpp`, `SystemInfo.*`, `Zones.cpp`, `PatrolPaths.*`, `SystemConnections.*` | 9,210 |
| NPC ships, pilots, names, AI guides | `Npc*`, `PilotPersonalities.*`, `BehaviorCmds.*`, `Guides*`, `GotoGuides.cpp`, `PatrolGuides.cpp`, `SpacePaths.*` | 10,250 |
| Space support, dock queues, tradelanes | `SpaceSupport.*`, `DockQueues.*`, `TradelaneRequests.*` | 4,090 |
| Factions, combat reputation, cargo, loot | `FactionProps.*`, `CombatReputation.*`, `Cargo.*`, `LootProps.*` | 3,360 |
| Bases: bar NPCs, bribes, rumors, news, generic bar scripts | `BaseDB.*`, `BaseNpcRecords.cpp`, `BaseSupport.*`, `GenericScripts.*` | 3,640 |
| GCS (NPC comm chatter) and voice-line durations | `Gcs.*`, `Voices.*` | 5,940 |
| Per-player content state, GameSupport, stats | `PlayerState.cpp`, `ContentPlayerState.h`, `GameSupport.cpp`, `Support.cpp` | 2,690 |

### How missions are authored

**INI-driven and interpreted at runtime, with a thin hard-coded shell.**

- Each story mission class sets chapter numbers and a heartbeat, then loads `Missions\M0x\m0x.ini` (`StoryMissions.cpp:100-106`). `GenericMission` takes its INI path from the create parms (`:55-59`).
- `[Trigger]` sections hold one `Cnd_*` plus any number of `Act_*` keys, `InitState`, `repeatable` (`MissionScript.cpp:240-300`). Dispatch is two name→factory tables: **44 condition types** (`:88-133`) and **85 action types** (`:145-231`). Each condition declares the message ids it listens to; 14 of them are polled on `Heartbeat`.
- Other mission sections: `[Mission]`, `[NPC]`, `[MsnSolar]`, `[MsnFormation]`, `[MsnLoot]`, `[MsnRandEnc]`, `[ObjList]` (AI directive lists), `[Dialog]`, `[NNObjective]`, `[MetaBehavior]`.
- Hard-coded C++: the mission order in `Story.cpp:214-247` (`StartMission(7, "Mission_02")`, …), per-mission overrides (for example Mission_01a's first-base-enter latch, `StoryMissions.cpp:108-140`), and all of the random-mission runtime state machine (`RandomMissionVignette.cpp`, one baseline function of about 5,100 instructions split by state).
- Random missions are generated from an INI decision tree (`RandomMissions\VignetteParams.ini`) plus difficulty/reward tables, then run by C++ machines.

## 4. Coupling

| Direction | Mechanism |
| --- | --- |
| Server → Content | Runtime only: `LoadLibrary` + `CreateInstance` + `IDLL::HandleMessage`. No link dependency. |
| Content → Server | Link-time imports of `pub::*` (`Content.vcxproj` links `Server.lib;Common.lib`). Content does not touch `Players`, `SysDB` or `GetServerInterface` (grep-verified); it includes `server_cobject.h`/`server_gameobject.h` for types. |
| Content → Common | `INI_Reader`, `CreateID`, reputation, goods, `pub::AI` parameter structs, math engine via the component manager. |
| Common → Content | Callback objects: Content NPC classes implement `pub::AI::ContentCallback` / `DirectiveCallback` / `CommReferrable`, registered through `SetPersonalityParams`; Common's behaviour manager calls back (`Content/NpcShip.h:96-99`, `include/common_pub.h:1163`). |
| Server → transport | `LoadLibraryA(ModulePath)` → `IClient` (126 slots), held in the global `g_pClientInterface`. |
| Common → Server | Header only (`ISave` in `server_savefile.h`). |

**Circular at runtime, not at link time**: Server dispatch → Content handler → `pub::` call → Server state change → nested dispatch → Content. The only guards are the re-entrancy counter and deferred-create list (`IServerImpl.cpp:3155-3180`). Content keeps process globals too: `GMessageRouter`, `GStoryMissionController`, `GSpaceSupportControllerId`, about 60 global STL tables.

**Borrowed handles across the boundary**: `PlayerSave`/`SaveGame` pass a CRT `FILE*`; `PlayerLoad`/`LoadGame` pass an `INI_Reader*` (`server_pub.cpp:1898-1908`).

## 5. "AIL_" references in Content

**There are none.** `grep -rn "\bAIL_"` over all of `CppSource` returns nothing. The only `AIL_` substrings in Content are inside three random-mission comm identifiers: `GLOBAL_FAIL_COMBAT_RANGE`, `GLOBAL_FAIL_DESTROYED_CRITICAL_LOOT`, `GLOBAL_FAIL_OTHER` (nine hits across `RandomMissionRuntime.cpp:103-105`, `RandomMissionMachine.h:345-347`, `RandomMissionVignette.cpp`). No Miles, `mss32` or winmm audio code exists in Server or Content. Audio in this group is server-side requests only (`pub::Audio`, `Act_PlayMusic`, `Act_PlaySoundEffect`) and voice-line duration tables used for comm timing (`Voices.cpp`).

## 6. Data files consumed

All paths resolve against the DATA file system (`pub::GetDataPath()->GetFileSysPath`, `Content/ContentDataPath.cpp:19`). **No data file is in the repository**; `copy_dev_ini.py` expects sibling `bin\`, `..\DATA`, `..\DATA_EXTRA`, `..\EXE` from an original install.

| Consumer | Files |
| --- | --- |
| Server | Main INI (`[OutpostFrontier] initial_world`, `[Initial SP DLLs]`, `[Initial MP DLLs]`), everything `DataConfig::startup` reads (Common), the `initial_world` file (MP), `NewPlayer.fl`, `NewCharacter.fl`, `MPNewCharacter.fl` beside the exe, `scripts\extras\player_fidget.thn`, `motd.dat` (host) |
| Content tables (mostly `Encounters.cpp:497+`) | `audio\voices_space_male.ini`, `voices_space_female.ini`, `voices_recognizable.ini`; `missions\empathy.ini`, `voice_properties.ini`, `formations.ini`, `pilots_population.ini`, `pilots_story.ini`, `faction_prop.ini`, `npcships.ini`, `shipclasses.ini`, `lootprops.ini`, `news.ini`, `PTough.ini`, `RankDiff.ini`, `mBases.ini`, `specific_npc.ini`; `equipment\commodities_per_faction.ini`; `ships\shiparch.ini`; `scripts\gcs\genericScripts.ini`, `GCSExcl.ini` |
| Random missions | `RandomMissions\{VignetteParams,Diff2Money,NpcRankToDiff,KillableSolars,RMLootInfo,SolarFormations,VignetteCriticalLoot}.ini` |
| Story | `Missions\M01a\m01a.ini` … `Missions\M13\m13.ini`, plus RTC and dialog INIs they reference |
| Universe | `Universe\universe.ini`, per-system INIs whose `[EncounterParameters]` rows name further encounter INIs (`SystemInfo.cpp:86-103`), `Universe\systems_shortest_path.ini`, `shortest_legal_path.ini`, `shortest_illegal_path.ini` |

Content **writes** the three shortest-path files to the CWD-relative `..\data\Universe\` when the first is missing (`SystemConnections.cpp:386-389`, `:482-507`).

## 7. Determinism

- **RNG**: one process-global CRT `rand()`, seeded with `srand(timeGetTime())` in server startup (`IServerImpl.cpp:207`), in the dedicated host loop and in the client. About 230 lines in Content mention `rand()` (76 in `Gcs.cpp`) and about 50 in Server, nearly all in the forms `rand() * N / 0x8000` or `rand() * (1/32768)`. Server, Content, Common and (in SP) the client share the stream.
- **One reseed that matters**: a random mission stores `random_seed`, written to the save (`DestroyMission.cpp:575`, `:610`), and the offer is *rebuilt* on load after `srand(seed)` (`:642`, `:1051-1052`). The Rust generator must be the MSVCRT LCG with the same draw order for old saves to restore the same mission. (LCG constants are not in this tree; take them from the CRT — **guess** until verified.)
- **Time**: QPC with `GetTickCount` fallback for the server clock; `GetLocalTime` → `FILETIME` for `tstamp` (high dword first, `server_pub.cpp:1296-1321`); variable `dt`.
- **Float rules**: money and worth use truncating float→int casts with exact constants (`0.3f * price + 0.4999989867f`, `0.8f * hull`, `server_classes.cpp:1136-1187`); stored prices are `(float)(int)(price * mult)`; reward interpolation runs in double and truncates (`RandomMissionGen.cpp:196-229`). The code spells out x87 intermediates as `double` with explicit `float` stores, and several compare polarities encode unordered (NaN) behaviour. `sin`/`cos` feed spawn placement.
- **Ordering**: controller dispatch order (priority, then insertion), `std::map` iteration by hashed id, `_findfirst` directory order at login, and the 128-timers-per-frame cap are all observable.

## 8. Hazards

| Hazard | Evidence | Rust consequence |
| --- | --- | --- |
| Globals and singletons | `Server`, `Players`, `SysDB::SysMap`, `g_pClientInterface`, controller/timer lists, `GServerLastSavePath`, static path buffers returned by pointer (`server_pub.cpp:796`) | One owned context passed by `&mut` |
| Static-init order | Every Content TU runs 16 `CreateID(...)` initializers against Common (`Content/ContentStaticIds.h`); about 60 global containers | `const` hashes or lazily built tables |
| Re-entrancy | Nested dispatch (§4); `Deferred` list deletes objects mid-step | Does not fit `&mut self` handlers; needs an event queue or split borrows |
| Pointer size | `LoadMissionIni((char const*)Parms + 0xc)` (`StoryMissions.cpp:58`); pointers truncated into `u32` event slots (`server_pub.cpp:2257`, `:2266`; `DockQueues.cpp:1701-1711`; `MissionObject.cpp:3223`; `BaseDB.cpp:722`); `*(unsigned int*)data` on wire blobs (`IServerImpl.cpp:2181`); payloads as raw `u32[]` | Typed message enum; no layout carried over |
| Raw structs on disk | Only `accounts.cfg`, `password`, `name`, `motd.dat`; fixed-width little-endian, no pointers | Explicit LE readers |
| Wide strings | Account/character hashing, XOR files, `%04x` hex names all assume **16-bit `wchar_t`**; `towlower` and `wcstombs` are locale-dependent | Use `u16` units; ASCII-only lowercase needs a decision |
| STL across DLLs | `IServer` takes `std::list`, `EquipDescList`, `std::wstring`; exported `std::map`; `FILE*`/`INI_Reader*` in events | Disappears in one crate graph |
| Threads | None in Server or Content. The dedicated host serialises with a mutex | Keep single-threaded |
| SEH / exceptions | No `__try`; two `throw std::out_of_range` (bitset emulation) | None |
| Preserved UB | Null derefs kept for fidelity (`TargetEq->DockingBaseId`, `IServerImpl.cpp:3917`), unterminated `wcstombs`, format-string writes (`fprintf(File, Text)`), unchecked `fopen` | Decide per case: `Option` handling or documented divergence |
| Reconstruction gaps | About 100 `TODO`s and 800 `Field_0x..` uses in Content, 280 in Server | Port as named-unknown fields; do not invent meaning |

## 9. Legacy Windows APIs

| API | Use | Port |
| --- | --- | --- |
| `SHGetFolderPathA` | Save root (Common) | Known-folder lookup behind a path provider |
| `LoadLibraryA`, `GetProcAddress`, `GetModuleFileNameA` | Transport DLL, controller DLLs, exe-side templates | Static linking, traits |
| `timeGetTime` (winmm) | RNG seed only | Injected seed |
| `GetLocalTime`, `SystemTimeToFileTime` | `tstamp` | `FILETIME` arithmetic |
| `CopyFileA`, `GetFileAttributesA`, `_findfirst`, `_access`, `_unlink`, `_rmdir`, `rename`, `_makepath` | Save management | `std::fs` |
| Text-mode CRT `fopen("w+t")` | Save newline semantics | Emit LF directly |
| **ODBC** | `Server.vcxproj` links `odbc32.lib;odbccp32.lib`; **no ODBC call exists** in Server or Content | Drop |
| Registry, COM | None in this group (`CoInitialize` is in the host) | — |

## 10. Tests and golden-vector points

No tests, fixtures or self-test hooks exist for Server or Content, and the Rust crates are stubs. Candidate capture points, each needing a small C++ harness:

1. **FLS1 codec** — pure function; round-trip real `.fl` files and both templates.
2. **Account CRC and directory names** — `(name) → "%02x-%08x"`, including mixed case and non-ASCII.
3. **Save round-trip** — parse → `FSavedCharacter` → serialize; byte-compare the decoded text (pin `tstamp`).
4. **New-character expansion** — template + `SCreateCharacterInfo` → output text.
5. **Mission script parse** — dump every `Missions\*\*.ini` as (trigger id, condition, args, actions).
6. **Trigger save/restore** — `[TriggerSave]` / `[MissionState]` text.
7. **Economy** — `GetPrice`, `PlayerData_TotalEquipmentValue`, `RmDifficultyToMoney`, `RoundRmMoney` over the data tables.
8. **Random mission offers** — `srand(seed)` + query → offer; the strongest check on RNG draw order.
9. **Shortest-path tables** — Content already writes them as INI.
10. **Heartbeat/timer scheduler** — scripted `dt` sequence → message trace.

## 11. Rust mapping

### Split

`game-logic` depends on `neuron-server` (AGENTS.md), so `neuron-server` must stay game-agnostic.

| `neuron-server` (infrastructure) | `game-logic` (rules) |
| --- | --- |
| `host` — startup/shutdown/tick driver, clocks | `world` — systems, object model, in-transit, asteroid fields |
| `session` — slot table, login status | `objects` — ship/solar/loot/munitions, damage, `fuse` |
| `accounts` — CRC names, name/password/banned, `accounts.cfg` | `player` — character state, cash/equipment, worth and cheat checks, trade, groups |
| `savefile` — FLS1 codec, INI writer, atomic replace, paths | `character_file` — `[Player]` schema, templates, visit/log |
| `controller` — registry, priorities, heartbeats, timers, deferred create/destroy (generic over message type) | `events` — typed message enum replacing the 80 ids |
| `client_sink` — trait standing in for `IClient` | `replication` — observer visibility and create records |
| `chat` transport plumbing | `bases` — room records, bar NPCs, rumors, news |
| | `mission::{script, cnd, act, object, objlist, save, dialog}`, `story` |
| | `random_mission::{db, gen, vignette, machine}` |
| | `population::{encounters, placement, zones, paths}`, `npc::{ships, pilots, guides, gcs}` |
| | `factions`, `reputation_combat`, `loot`, `space_support`, `dock` |

The `pub::` façade does not survive as an API: it becomes methods on the game-logic world/context.

### Mission scripts: redesign the representation, keep the semantics

Do not port the `new T(Mission)` + virtual `Parse` shape. Parse each INI into `enum Condition` (44 variants) and `enum Action` (85 variants) with typed fields, evaluated by `match`. Keep unchanged: key names and argument order, per-condition event subscriptions, trigger states and `repeatable`, the rule that a later `Cnd_*` replaces the earlier one, action order, and the save text. Parsing then becomes a pure function (golden 5), and the pointer tangle between trigger, mission and controller goes away. The 15 per-mission classes become small hook implementations. Port the random-mission machines 1:1 as explicit state enums.

### Effort and order

| Sub-module | Effort | Depends on |
| --- | --- | --- |
| FLS1 + account CRC + paths | S | — |
| Controller bus, heartbeats, timers | S–M | — |
| Session/account store, character file read/write, templates | L | INI reader, reputation, goods (Common) |
| Typed events + `client_sink` trait | M | RPC codecs (other assessor) |
| Content static tables (factions, pilots, npcships, formations, loot, voices, mbases) | L | INI reader, IDs |
| World, objects, fuses, munitions | XL | Common `CObject`, physics, archetypes |
| Observer / replication | L | World, client sink |
| Bases, GameSupport, BaseSupport, player state | L | Tables, world |
| Mission object, objlist, script engine | XL | World, AI params |
| Population, placement, SpaceSupport, dock queues | XL | World, AI |
| NPC ships, guides, GCS | L–XL | Common AI state graphs |
| Random missions | XL | Population, mission object |
| Story shell | S | Script engine |

### Top risks

1. **Re-entrant dispatch vs. Rust borrowing** — the central design decision; a wrong choice changes event ordering.
2. **Shared `rand()` stream and seed-rebuilt missions** — draw-order fidelity is required for save compatibility.
3. **x87 semantics in rules** — truncations and compare polarities must be reproduced with explicit `f64` intermediates and `as f32` stores.
4. **Client-authoritative single player** — the server trusts `SP*`/`Req*` calls; the Rust server/client boundary must keep or deliberately change that.
5. **No data, no tests** — nothing here can be validated without the original data directory and a C++ harness.
6. **Dependency depth** — most of Content cannot run until Common's AI, reputation and object model exist.

## Open questions for the owner

1. Must Rust-written saves and accounts be readable by the C++ build, and vice versa (FLS1, hashed names, password token, `random_seed` rebuild)? Or only C++→Rust import?
2. Is the original data directory available to the port team for golden capture, and may fixtures derived from it be committed?
3. Can a C++ harness be built to emit golden vectors (points 1–10), given the 32-bit-only toolchain?
4. Keep runtime-selectable controllers (the `[Initial * DLLs]` INI rows), or fix the set at compile time?
5. Keep the single-player trust model, or make the server authoritative for both modes?
6. Preserve bug-for-bug behaviour (null derefs, format-string writes, the 128-timer cap) or fix with a logged divergence list?
7. Non-ASCII account/character names: reproduce CRT `towlower`/`wcstombs` locale behaviour, or define new rules?
8. Should Content keep writing the shortest-path cache into the data tree?
9. Fixed-step simulation for the Rust server, or keep variable `dt` with the 0.2 s MP sub-step?

## Suggested port order within this group

1. `neuron-server::savefile` (FLS1, INI writer) and `accounts` (CRC, names) — with goldens 1–2.
2. `neuron-server::controller` (registry, heartbeat, timers) — golden 10.
3. `game-logic::events` (typed messages) and the `client_sink` trait.
4. `game-logic::character_file` and `neuron-server::session` — goldens 3–4.
5. Content static tables and `factions`/`loot`/`cargo`; `SystemConnections` — goldens 7, 9.
6. Mission script parser (AST only) — golden 5.
7. `world` + `objects` + `fuse` (after Common's object model lands), then `replication`.
8. `player` rules, `bases`, GameSupport/BaseSupport equivalents.
9. Mission runtime (object, objlist, conditions, actions, save) — golden 6; then `story`.
10. `population`, `space_support`, `dock`, `npc` (guides, GCS).
11. `random_mission` — golden 8.
