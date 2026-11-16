# 03 — Network stack assessment (RPC, DirectPlay8, dedicated server)

Paths are relative to `CppSource/`. Read-only pass; nothing was built or run. `dplay8.h` is **not in the
repo** (external DX8 SDK), so decoded flag names come from the SDK constants as I remember them and are
marked *(recalled)*.

Scope size: rpclocal 28.2k lines, RemoteCommon 5.8k, RemoteClient 3.6k, RemoteServer 3.2k, OutpostServer
6.6k, NeuronRuntime net 1.9k, headers ~7k.

## 1. Layering

```
 OutpostFrontier.exe (game thread)                      Server.dll (authoritative sim)
 IClientImpl : IClient  (126 slots, receives)           IServerImpl : IServer (92 slots, receives)
 calls G_Server -> IServer*                             calls g_pClientInterface -> IClient*
        |                                                        |
========|==================== SINGLE PLAYER =====================|=========================
        |  (a) local_server = Server.dll : direct virtual calls, no packets at all
        |  (b) local_server = rpclocal   : rpclocal.dll, in-process
        v                                                        v
   rpclocal IServerImpl --alloc FPacketBase--> FRpcConnection <--alloc-- rpclocal IClient proxy
        (4 std::deque<FPacketBase*>, pointer hand-off, NO byte encode, no locks)
        IServerImpl::Update pops client-bound -> HandlePacket -> real IClient
        RpcClientPump       pops server-bound -> RpcDispatchToServer -> real IServer
==================================== MULTIPLAYER ==========================================
 client process                                          server process (OutpostServer.exe)
 RemoteServer.dll                                        RemoteClient.dll
  FRemoteServerImpl : IServer (proxy)                     CClientImpl : IClient (proxy), ClientProxies[128]
  slot -> FRpc*Packet -> Encode(0x2800)                   slot -> FRpc*Packet -> Encode(0x2800 | 0x19000)
  RpcDispatchMsgs -> Decode -> HandlePacket -> IClient    CRemoteClientProxy::OnReceive -> Decode -> IServer
        |      RemoteCommon: 162 packet classes + codecs (same .cpp compiled into both DLLs)      |
        v                                                        v
  FRemoteServerClient : CDPClient                         CRemoteClientHost : CDPServer
  (1 Hz 1-byte ping, inflate on receive)                  + CRemoteClientProxy : CDPClientProxy
        |                                                 (send queue, 1 KiB batch, deflate, saturation)
        v                    NeuronRuntime.dll                    v
  IDirectPlay8Client  <====== DirectPlay8 TCP/IP SP (UDP) ======> IDirectPlay8Server
  CLoginMgr : CDPClient (EnumHosts browser)   CGunWrapper -> gundll.dll (list server, opaque COM)
```

- **Single player.** The server is in-process, same thread. `StartServer` loads `G_LocalServerDll`
  (default `"Server.dll"`, overridable by `[OutpostFrontier] local_server`) in SP and `RemoteServer.dll` in MP
  (`OutpostFrontier/Client/server_connect.cpp:106-143`, `login_mgr.cpp:1530-1545`). No `.ini` ships in the repo,
  so which of (a)/(b) the game uses is **unverified** (guess: `rpclocal`). In (b) every call becomes a packet
  *object* in a pointer queue; `Encode`/`Decode` are never called (the only call sites are
  `RemoteClient_transport.cpp:158,168,314`, `RemoteServer_main.cpp:157`, `RemoteServer/IServerImpl.cpp:80`).
  Five slots bypass packets entirely: Startup, Shutdown, ElapseTime, Connect, DisConnect
  (`rpclocal/rpclocal.cpp:12-19`). SP address is the literal `"localpass"` (`login_mgr.cpp:712`).
- **Multiplayer client.** `G_Server` is RemoteServer's proxy; `Update()` pumps DirectPlay
  (`RemoteServer_main.cpp:142-148`). `ElapseTime` does not simulate locally.
- **Dedicated server.** `OutpostServer.exe` links `Server.lib` + `RemoteClient.lib`, sets
  `SStartupInfo.ModulePath = "RemoteClient.dll"` (`OutpostServer.cpp:537`), and Server.dll resolves
  `GetClientInterface` from it (`Server/IServerImpl.cpp:2786-2790`). There is no listen-server mode: a
  hosting player runs both processes, coordinated by the run-permission events (section 7).
- **Duplication.** rpclocal carries its own full packet/codec implementation (162 registrations,
  `rpclocal/rpc_packet_registry.cpp`) parallel to RemoteCommon's (162 registrations,
  `RemoteCommon/remote_server_channel_codecs.cpp`). Rust needs one.

## 2. DirectPlay8 usage (exhaustive)

Interfaces: `IDirectPlay8Client`, `IDirectPlay8Server`, `IDirectPlay8Address` only. **Not used:**
`IDirectPlay8Peer`, lobby, voice, NAT-help, thread-pool, groups, player info, connect data.

| Call | Site | Arguments / notes |
| --- | --- | --- |
| `CoCreateInstance(CLSID_DirectPlay8Client)` + `Initialize(this, HandleClientMsg, 0)` | `NeuronRuntime/CDPClient.cpp:45-53` | preceded by `CoInitialize` (`:42`) |
| `SetCaps` (client) | `CDPClient.cpp:55-57` | all-zero `DPN_CAPS`; effect unverified |
| `Client->Connect` | `CDPClient.cpp:163` | async, no connect data, no player context; app desc carries only the GUID |
| `Client->Send` | `CDPClient.cpp:195` (data), `:519` (ping) | timeout 0, flags `0x108` |
| `Client->EnumHosts` (broadcast) | `CDPClient.cpp:245` | count `0xffffffff`, retry 0, timeout 0, no user data, flags 0 |
| `Client->EnumHosts` (one host) | `CDPClient.cpp:284` | URL `x-directplay:/hostname=%s` + `port`; caller passes count 2, retry 0, timeout 0 (`login_mgr.cpp:1214`); flags 2 = NOBROADCASTFALLBACK *(recalled)* |
| `CancelAsyncOperation(0, 0x8000)` / `(0, 2)` | `CDPClient.cpp:68,98,175` / `:184` | all operations / enums *(recalled)* |
| `GetConnectionInfo`, `GetSendQueueInfo`, `ReturnBuffer`, `Close`, `Release` | `CDPClient.cpp:457,342,540,69-70` | |
| `CoCreateInstance(CLSID_DirectPlay8Server)` + `Initialize(this, HandleDPServerMsg, 0)` | `CDPServer.cpp:40-48` | |
| `SetApplicationDesc` | `CDPServer.cpp:56,59,92` | on every name/password/max-players change |
| `SetCaps` (server) | `CDPServer.cpp:65-67` | all-zero |
| `Server->Host` | `CDPServer.cpp:203` | one TCPIP device address, optional `port` |
| `GetLocalHostAddresses` (64 max), `GetURLA`, `GetComponentByName("port")` | `CDPServer.cpp:144,154,228` | |
| `Server->SendTo(dpnid, …, 0x108)` | `CDPServer.cpp:240`, `:454` (ping echo) | per player; never a group or all-players send |
| `DestroyClient` | `CDPServer.cpp:248` | kick, no reason data |
| `GetConnectionInfo`, `GetSendQueueInfo(…,1)`, `ReturnBuffer`, `Close` | `CDPServer.cpp:259,270,279,290,461,79,217,322` | |
| `IDirectPlay8Address`: `SetSP(CLSID_DP8SP_TCPIP)`, `AddComponent("port")`, `BuildFromURLW`, `GetComponentByName("hostname"/"port")` | `CDPClient.cpp:152-156,228-234,275-281,397`; `CDPServer.cpp:198-200`; `login_mgr.cpp:566,576` | |

**Send flags.** One value everywhere: `0x108` = `DPNSEND_GUARANTEED | DPNSEND_COALESCE` *(recalled)*. No
SYNC, NOLOOPBACK, NONSEQUENTIAL, PRIORITY_*, NOCOMPLETE, and send timeout is always 0. **Every byte the game
sends is reliable and ordered on one logical stream**, position updates included.

**Callback and threading.** Static `__stdcall` handlers run on DirectPlay worker threads and only enqueue
(section 7).

| Side | `DPN_MSGID_*` handled | Action |
| --- | --- | --- |
| Client (`CDPClient.cpp:349-499`) | ASYNC_OP_COMPLETE, CONNECT_COMPLETE, ENUM_HOSTS_RESPONSE, RECEIVE, TERMINATE_SESSION | deep-copies enum responses; drops responders whose hostname contains `:` (IPv6, `:386-401`); a 1-byte receive is a ping reply |
| Server (`CDPServer.cpp:327-427`) | CREATE_PLAYER, DESTROY_PLAYER, ENUM_HOSTS_QUERY, INDICATE_CONNECT, INDICATED_CONNECT_ABORTED, SEND_COMPLETE, RECEIVE | INDICATE_CONNECT always accepts and allocates the proxy; receive buffers are kept (`DPNSUCCESS_PENDING`) and returned on the game thread |

**Session.** Application GUID `{A690F026-26F0-4E57-ACA0-ECF868E48D21}` hard-coded three times
(`CDPServer.cpp:62`, `RemoteServer_main.cpp:213`, `login_mgr.cpp:1061`), overridable by `AppGUID` in the ini.
`dwFlags = 1` (CLIENT_SERVER; NODPNSVR not set, so DPNSVR/UDP 6073 serves LAN discovery — *recalled*).
Session name default `"DPServer Generic"` then `"OutpostFrontier Default Session Name"`, then the admin's server
name (`RemoteClient_transport.cpp:107`, `OutpostServer.cpp:747`). `dwMaxPlayers = MaxPlayers + 1`
(`CDPServer.cpp:113`); the browser subtracts 1 again (`login_mgr.cpp:543-551`). Password sets flag `0x80`
(`CDPServer.cpp:119-133`); the client maps `0x80158410` and `0x80158610` to distinct error dialogs
(`connect_handshake.cpp:121-131`; INVALIDPASSWORD and SESSIONFULL, *recalled*).

**Finding:** the client never supplies the password. `FRemoteServerImpl::Connect` puts it in the *port* slot
of `AddConnectAttempt` (`RemoteServer_main.cpp:228,233`) and `ConnectToServer` ignores that parameter
(`CDPClient.cpp:131`). Either a reconstruction gap or dead original behaviour; password-protected servers
look unjoinable as written.

**Ports.** Server: `SStartupInfo.Port`, 0 = DirectPlay picks, `/P<n>` overrides (`OutpostServerCmdLine.cpp`).
Client source port: `+P<port>` (`main.cpp:473-478`). Client supports several candidate ports per host, tried
in sequence (`RemoteServer_main.cpp:211-237`, `CDPClient.cpp:551-558`). Address syntax from the UI is
`"tcpip, <host>:<p1>,<p2>…"` (`login_mgr.cpp:721-743`).

**IDs.** DPNID is internal. The game ID is `ClientId` 1..128, the first free `ClientProxies[]` slot
(`RemoteClient_transport.cpp:271-285`), sent to the client as the first message, `SERVER_CONNECTRESPONSE`.
Max players: dialog allows 1..128 (`ServerConfigDlg.cpp:142`), default 16; the admin bandwidth view stops
at 16 (`MainFrm.cpp:216`).

**Enumeration payload.** Reply data ≤ 0x400 bytes (`CDPServer.cpp:295-309`): ASCII
`"<pvp>:<newPlayers>:<version>:<serverId>:<serverSig>:"` followed by a UTF-16 description
(`RemoteClient_transport.cpp:176-187`; parser `login_mgr.cpp:582-628`). Session name, player counts, password
flag and RTT come from DirectPlay's own response. The browser pings LAN by broadcast and direct/list entries
five at a time, 1 s per try, 5 tries, ages entries at 5 s / 30 s (`login_mgr.cpp:1116-1226`).

**Banning.** None at transport level. Kick = `DestroyClient` (`ForceClientLogout`). Bans are per account, a
`banned` marker file checked at login (`Server/server_classes.cpp:3834,4216`).

## 3. Non-DirectPlay networking

| Mechanism | Where | Purpose |
| --- | --- | --- |
| "GUN" list server via `..\exe\gundll.dll` | `NeuronRuntime/CGunWrapper.cpp`, `include/neuronruntime_net.h:325-426` | Global server list. Opaque third-party COM DLL, **not in the repo**; only vtable shapes are known. Client: `SendRequest7104(0x65, page…)` and reply type `0x7204` (`login_mgr.cpp:363-528`). Server: publishes name, `GameOpts` = `"ports:max:pw:pvp:new:sig:id"`, `GameDesc` (`OutpostServer.cpp:823-861`). Host `[ListServer] hostname`/`port`, defaults `FLListServer2`:`0x708a` client, `localhost` server. Carries a `"CD-KEY"` slot, always `L"0"` or the version text. |
| winsock | `login_mgr.cpp:29,410,572,999` | `inet_addr`/`ntohl` only, for sort keys. No sockets, no `WSAStartup`. |
| HTTP / version check / LAN broadcast of its own | — | none found |

## 4. RPC message catalog

**Interfaces.** `IClient`: 126 slots (`include/rpc_iclient.h:5-10`), 117 of them senders
(`RemoteClient/ClientImpl.h`), plus `IChatClient::SendCommonChatMsg`. `IServer`: 92 slots, 81 serialising
(`rpclocal/rpclocal.cpp:12`), plus `IChatServer::SubmitChat`. Wire types: **162 packet classes / 161 opcodes**
— channel 1 common (21), channel 2 server→client (81), channel 3 client→server (60) (`include/network.h:19-680`).

**Framing.**
- Header: one LE `u16`, low byte opcode, high byte channel (`include/rpc_packet_codec.h:83-91`).
- **No length field.** Packets are self-delimiting by codec. A batch is plain concatenation.
- Fields are raw x86 little-endian `memcpy`: `u8/u16/u32/f32`, `Vector` as 3×`f32`, blobs as `{u32 size, bytes}`,
  wide strings as `{u8|u16 count, UTF-16}`. No bit-packing.
- Quantisation: orientation quaternion 4×`i8` × 127; percentages `u8` × 255; one field `u8` × 25.5; FIREWEAPON
  has a 16-bit tier (`rpclocal/rpc_transport.h:104-125`). Positions are **full `f32`**. UPDATEOBJECT is 28 bytes
  (`RemoteCommon/remote_packet_codecs.cpp:1503-1537`).
- A fixed 42-entry hardpoint-name dictionary compresses CREATESHIP/CREATESOLAR (`rpclocal/rpc_containers.cpp:1931-2010`).
- Sticky error codes `0x16` encode overflow / `0x17` decode overrun.

**Batching and compression (server→client only).** `CDPClientProxy::SendMsgs` concatenates queued packets into
≤ 0x400-byte batches and deflates each (zlib level 6) when ≥ 0x20 bytes and not larger
(`NeuronRuntime/CDPClientProxy.cpp:46-153`, `CDPNetCodec.cpp`). Oversized packets go alone, deflated below
0x7fff bytes, raw above. The client tries `inflate` into a 0x8000 buffer and falls back to raw on failure
(`CDPClient.cpp:530-537`) — the compressed/raw distinction is implicit. Client→server is never batched or
compressed.

**Sizes.** Encode buffers: 0x2800 (10 240) bytes both ways, server retry at 0x19000 (102 400)
(`RemoteClient_transport.cpp:155-171`, `RemoteServer_main.cpp:153`). So the largest message is ~100 KiB
server→client and 10 KiB client→server.

**Version, integrity, crypto.** No in-band protocol version; the build version travels only in the enum
reply and gates the browser row (`page_server_select.cpp:833,899`, `server_filter.cpp:66`). No CRC, no
encryption, no obfuscation in scope (grep-verified). Server-side cheat detection exists outside scope
(`Server/server_classes.cpp:545`, `cheaters.log`).

**Rates.** Server loop 30 Hz, `/H` 1–60 (`OutpostServer.cpp:64,379`, `ServerControl.cpp:242`). Client sends
its ship state only when dead-reckoning diverges (`client_objdb.cpp:880-926`). Server sends per-observer
UPDATEOBJECTs oldest-first until `GetLinkSaturation() >= 0.9` (`Server/server_observer.cpp:70-76,653-698`);
saturation = mean of three 1 s samples of queued bytes × 0.0005 (`CDPClientProxy.cpp:186-202`), i.e. ~1800
queued bytes closes the gate. That governor is the only congestion control above DirectPlay.

| Family (channel) | Examples | Direction | Reliability today | Frequency | Could be unreliable? |
| --- | --- | --- | --- | --- | --- |
| Transport ping (no header, 1 byte) | `0x01` / quality byte | both | guaranteed | 1 Hz | yes |
| Object state (1) | UPDATEOBJECT | both | guaranteed | high, saturation-gated | yes (has timestamp) |
| Combat/controls (1) | FIREWEAPON, ACTIVATEEQUIP/CRUISE/THRUSTERS, SETTARGET, TRADELANE, TRACTOR, JETTISON | both | guaranteed | bursty | no |
| Collisions/damage (3→, 2←) | MUNCOLLISION, OBJCOLLISION, DAMAGEOBJECT, DESTROYOBJECT | mixed | guaranteed | bursty | no |
| Spawn/despawn (2) | CREATESHIP/SOLAR/LOOT/MINE/GUIDED/COUNTER, LAUNCH, LAND, SYSTEM_SWITCH_* | S→C | guaranteed | medium | no |
| Session/login (2,3) | CONNECTRESPONSE, LOGIN/LOGINRESPONSE, CHARACTERINFO, SELECTCHARACTER, CREATENEWCHAR | both | guaranteed | once | no |
| Inventory/economy (2,3) | SETEQUIPMENT, SETCARGO, SETCASH, GFGOODBUY/SELL, REQ* | both | guaranteed | low | no |
| Base/room ("GF") lists (2) | GFUPDATE*/GFCOMPLETE*/GFDESTROY* | S→C | guaranteed | on dock, large | no |
| Missions/story/comm (2) | SETMISSION*, SENDCOMM, SET_STORY_CUE, SET_CAMERA, music | S→C | guaranteed | low | no |
| Social (1,2) | CHATMSG, PLAYER_TRADE, PLAYERLIST, MISCOBJUPDATE, group positions | both | guaranteed | low | no |

**Robustness defects to design out, not port:** an unknown opcode or failed decode on the client leaves the
loop reading mid-payload (`RemoteServer/IServerImpl.cpp:65-85`); the server's receive loop never advances its
cursor past a decoded body (`RemoteClient_transport.cpp:320-324`, harmless only because clients send one
packet per message); a 0-byte "empty batch" is counted as a ping reply (`CDPClientProxy.cpp:110-113`,
`CDPClient.cpp:474-479`); PLAYER_TRADE's length depends on `IsMPServer()` asymmetrically
(`rpclocal/rpc_transport.h:247-256`).

## 5. Authentication and identity

- No account server, no CD-key check. The GUN `"CD-KEY"` slot is the only remnant.
- Client identity = MD5 text derived from the Windows `DigitalProductId` + user name (fallback `UuidCreate`),
  stored in `HKCU\Software\Microsoft\Microsoft Games\OutpostFrontier\1.0` as `InstallKey`, `MPAccountName`,
  `MPAccountNameSig` (`Common/common_playeraccount.cpp:26,49-93,152-200,258-295`).
- At login the client sends `GetServerSignature(<server sig>)` as the account name with an empty password
  and `CreateFlag = 1` (`menu_enter_state.cpp:372-391`, `login_mgr.cpp:806-822`). The name is lower-cased on
  the wire (`remote_packet_codecs.cpp:1588-1596`). **The name is a bearer token in clear text**; any holder
  can impersonate the account.
- Server: rejects duplicates by case-insensitive name, loads or creates an account directory under
  `<UserData>\Accts\MultiPlayer`, checks `locked`/`banned` (`Server/server_classes.cpp:2337-2355,3824-3836`).
  Server identity (ID + signature) is its own `CPlayerAccount`.
- Login history (account + password, hex) lives in `HKCU\…\MP\Logins\LoginN` (`login_mgr.h:77-113`).

## 6. OutpostServer

MFC SDI application (`CWinApp`, doc/view, rich-edit console view + `CFormView` summary), not a console.

- **Startup:** EULA DLL gate, resource DLLs, two ini files (`[OutpostFrontier] Data path|AppGUID`,
  `[ListServer]`, `[Server]`, `[Resources]`), then the configuration dialog (`OutpostServer.cpp:417-560,761-815`).
- **Config sources:** binary config file v3 in the user-data dir (name, description, password, allow new
  players, internet-visible, max players, PvP — `ServerConfigDlg.cpp:124-173`); command line `/P` port,
  `/L` log file, `/S` log level, `/M` `/T` console limits, `/H` tick rate, `/E` log category, `/X` FPU traps,
  `/B` bandwidth log, `/C` accept saved config (`OutpostServerCmdLine.cpp`); MFC registry profile; `motd.dat`.
- **Admin UI:** accounts list, edit/ban/kick/delete account and character, create account, chat to
  player/system/universe, server news (MOTD), server info, set password, status summary (load ms, players,
  login/logout counters), uptime, console/summary toggle, per-client bandwidth CSV from
  `DPN_CONNECTION_INFO` (`MainFrm.cpp:205-260`), crash handler calling `CDPServer::CrashCleanup`.
- **Hosting:** startup thread runs `Server.Startup`; then `SetRemoteClientSessionName/Password/ResponseData`
  (`OutpostServer.cpp:724-757`); the server thread ticks `Server.Update()` + `ElapseTime` at 30 Hz.

## 7. Threading and reentrancy

| Thread | Does | Sync |
| --- | --- | --- |
| DirectPlay workers | run the static handler; enqueue `CDPMessage`; write ping statics (plain globals); server creates the proxy on INDICATE_CONNECT | `CDPMsgList` = `std::list` + `CRITICAL_SECTION` + manual-reset event; `g_ClientProxiesCs` |
| Client game thread | `IServer::Update` → `DispatchMsgs` → decode → `IClient` handlers, all synchronous; handlers may send | none above the list lock |
| OutpostServer server thread | `Server.Update` → `CClientImpl::Update` → `CDPServer::DispatchMsgs` (`RemoteClient_transport.cpp:96-100`) | `GServerMutex` |
| OutpostServer UI thread | dialogs call into `Players`/`Server`, `ForceClientLogout` | `GServerMutex`, account locks |
| Gun thread (1 Hz), startup/shutdown threads, gun release threads | list-server pump | `GAppMutex`; `CoInitialize` per thread |

- Server `Send` enqueues and immediately flushes inside the send-queue lock (`CDPClientProxy.cpp:25-43`), so
  the DirectPlay send happens on whichever thread called the `IClient` slot.
- `WaitForServerResponse` is a nested blocking pump inside UI code: spin `G_Server->Update()` for 5 s, then a
  modal dialog loop up to 60 s (`connect_handshake.cpp:33-116`). Login, character list, create/delete
  character all block on it.
- Run-permission handshake: two **named** manual-reset events
  (`"Microsoft…ClientCanRunEvent"`/`"…ServerCanRunEvent"`), 5000 ms / 500 ms timeouts, alternate client and
  server ticks across processes on one machine (`Common/common_misc.cpp:112-152`; used at
  `rpclocal.cpp:539-547`, `RemoteServer_main.cpp:144-146`, `OutpostServer.cpp:384-394`).
- rpclocal queues are single-threaded and unlocked; its `CRITICAL_SECTION` is never entered
  (`rpclocal/rpc_transport.h:437-441`).
- Liveness: client flags "stalled" after 1000 ms without any message (`client_timing.cpp:85`); HUD lag icons at
  ping > 600 / 1200 ms (`hud_targetable_objects.cpp:4093`). `LastActivityTime` on the server proxy is written
  and never read — **no application idle timeout**; disconnects come only from DirectPlay.

## 8. Legacy Windows APIs in scope

DirectPlay8 COM (`dpnet.dll`, deprecated optional Windows feature) · per-thread `CoInitialize` ·
`LoadLibraryA`/`GetProcAddress` wiring between exe, `Server.dll`, `rpclocal.dll`, `RemoteServer.dll`,
`RemoteClient.dll`, `gundll.dll`, including a DLL calling the exe's exported `HandleConnectError`
(`RemoteServer_main.cpp:184`) · `timeGetTime` (winmm) · `CRITICAL_SECTION`, events,
mutexes, `CreateThread`, `SleepEx` · registry (`HKCU`, `HKLM DigitalProductId`), `UuidCreate`, `GetUserNameA`
· MFC 14 + RICHED20 + `AfxOleInit` · `SetUnhandledExceptionFilter` · `_control87` FPU setup · VC6 CRT
semantics (`MsvcrtAtoi`, `_wcslwr`, `wcstombs`) · **29 inline x87 `__asm` blocks in the codecs**
(`RemoteCommon`, `include/rpc_packet_codec.h`, `rpclocal/rpc_wire_fields.cpp`) — 32-bit only.

## 9. Existing tests and verification hooks

No tests in scope (the Rust crates are 3–12 line stubs). Existing hooks: `DumpPacketStats(name)` writes a per-packet-type CSV (count/max/avg/total, in and out;
`rpclocal/rpc_transport.h:306-345`); twelve packet types have an `ostream` dump
(`rpclocal/rpc_packet_dump.cpp`); the `/B` bandwidth log; `OutputDebugString` of hosting URLs. The two
independent codec implementations are an unused cross-check oracle.

## 10. Replacement transport

### Requirements

| # | Requirement | Source |
| --- | --- | --- |
| R1 | Client/server only, 1 server : ≤128 clients, connection-oriented, async connect with result codes (refused, full, bad password, timeout) | §2 |
| R2 | One **reliable, ordered** message channel per connection carrying all 162 types — the baseline semantic | §2 flags |
| R3 | Message boundaries preserved, or an explicit length prefix added; messages 2 bytes … ≥100 KiB (S→C), ≥10 KiB (C→S); transparent fragmentation | §4 |
| R4 | Optional unreliable/sequenced channel for UPDATEOBJECT and ping (improvement, not parity) | §4 |
| R5 | Per-connection send-backlog bytes and RTT, cheap to poll each tick (drives the 0.9 saturation governor, HUD ping, admin stats) | §4, §6 |
| R6 | Keepalive and idle timeout owned by the transport; app sees connect / disconnect(reason) events | §7 |
| R7 | Server-initiated disconnect (kick), optional session password, connection cap | §2 |
| R8 | Discovery: LAN query without knowing the address; unicast query of host:port returning ≤1 KiB info + RTT **without a full connection**; master list is a separate service | §2, §3 |
| R9 | Poll-driven from the game/server tick: non-blocking `poll()` yielding events, send callable from the tick thread; no callbacks into game code from foreign threads | §7 |
| R10 | In-process loopback implementation of the same interface for single player | §1 |
| R11 | Encryption + server authentication, since account names are bearer tokens | §5 |
| R12 | IPv4 and IPv6; configurable port, multiple candidate ports; NAT: client-initiated UDP is enough for dedicated servers, hole-punching only if players host at home | §2 |
| R13 | Version/protocol handshake before any game packet | §4 |
| R14 | Builds on `x86_64-pc-windows-msvc` with `--locked`, no system-component install | AGENTS.md |

### Candidates

| | (a) QUIC / `quinn` | (b) own UDP layer | (c) `renet` / `laminar` | (d) GameNetworkingSockets FFI |
| --- | --- | --- | --- | --- |
| R2/R3 reliable ordered, large msgs | streams; add length prefix | must build fragmentation, ACK, resend, congestion | renet: reliable channels with fragmentation, size limits configurable | yes, 512 KiB messages |
| R4 unreliable | datagrams (~1.2 KiB) | yes | yes | yes |
| R5 backlog/RTT | RTT + cwnd stats; backlog tracked app-side | whatever we expose | RTT, loss, bytes/s; backlog partly app-side | best: pending reliable bytes per connection |
| R9 poll model | async (tokio) — needs an I/O thread + channels | native fit | native fit (`update(dt)`) | native fit |
| R11 crypto | TLS 1.3 mandatory; needs cert pinning/TOFU for self-hosted servers | DIY — high risk | netcode-style connect tokens + AEAD | built in |
| R8 discovery | none; separate UDP responder | can share the socket | none | none (P2P signalling only) |
| R12 NAT | no punching | DIY | no | ICE available |
| R14 build | pure Rust | pure Rust | pure Rust | C++/CMake + OpenSSL/protobuf, `unsafe` FFI |
| Maintenance (from memory, not checked) | active, widely used | ours forever | renet active, small team; **laminar dormant — exclude** | active upstream; Rust bindings thin |
| Effort | M | XL | S–M | M + build/CI cost |

Trade-offs: **(a)** strongest security and longevity, but its async model must sit behind a tick-polled
facade; one stream has the same head-of-line blocking DirectPlay had (parity). **(b)** exact fit, but
re-implements congestion control and crypto — the two parts most likely to be wrong. **(c)** closest to the
existing shape (channels, poll, client slots) and least code; risk is a small maintainer base. **(d)** most
proven, and the only one with an exact `GetSendQueueInfo` analogue, at the cost of a C++ toolchain in a
project whose purpose is leaving C++.

Whatever is chosen, R8 and R10 are ours to write, and the plan should put a narrow `Transport` trait in
neuron-core so the choice is reversible.

### Dropping wire compatibility

Lost: interop with original clients/servers (already impossible without `dpnet.dll`), DPNSVR LAN discovery,
the DirectPlay session password and error codes, `DPN_CONNECTION_INFO` counters, and the GUN list server
(closed DLL; no replacement possible from this repo).

The RPC payload encoding **can** sit unchanged above any transport: header and codecs never touch
DirectPlay. Recommendation: keep field order, widths and quantisers (they define gameplay precision and give
golden vectors from the C++ codecs), but (1) add a length prefix per packet, (2) add a versioned handshake,
(3) make the batch/compression flag explicit or drop zlib, (4) do not reproduce the defects listed in §4.
Bit-exact x87 behaviour is needed only for quantiser rounding (`f64` multiply, truncate toward zero), not for
the instruction-order quirks.

## 11. Rust mapping

| Piece | Crate | Effort | Notes |
| --- | --- | --- | --- |
| Packet IDs, wire reader/writer, quantisers, hardpoint dictionary, 162 message structs + codecs | neuron-core | **L** | port RemoteCommon's set only; rpclocal's 28k lines are not ported |
| `Transport` trait, events, loopback impl, protocol version, discovery record | neuron-core | S–M | replaces rpclocal + `CDPMsgList` |
| Client connection (multi-port attempts, ping/RTT/quality, stall flag), discovery client, list client | neuron-client | M | replaces RemoteServer + `CDPClient` |
| Host, per-client proxy, batching, saturation metric, slot table, kick, stats, discovery responder | neuron-server | M | replaces RemoteClient + `CDPServer`/`CDPClientProxy` |
| Dispatch of ch1/ch3 into simulation; accounts, bans, admin operations | game-logic | M | `IServer` becomes a trait or message enum |
| Dispatch of ch1/ch2 into client state; login/character flow; server-select page | game-app | M | replace the nested blocking pump with a state machine |
| Admin front end | `outpost-server` + game-logic | M–L | MFC cannot be ported; decide console/TUI/GUI |
| Master server list | new service or dropped | S (drop) – L (build) | GUN is unrecoverable |
| Account identity | neuron-core / neuron-client | S | MD5 kept only if account directories must stay compatible |

**Order:** (1) wire primitives + quantisers with golden tests; (2) codecs ch1 → ch3 → ch2, round-trip and
cross-checked against C++ dumps; (3) `Transport` trait + loopback, single player end to end; (4) real transport
+ handshake/auth; (5) server host and governor; (6) client connection; (7) discovery/browser; (8) admin
front end; (9) master list decision.

**Risks:** no tests and no captured traffic; variable-length codecs (CREATESHIP, CHARACTERINFO, GF blobs)
embed formats owned by other subsystems (`FmtStr`, RDL, `EquipDesc`); the saturation constants are tuned to
DirectPlay's queue and need retuning; the blocking-wait UI flow and cross-process run-permission events have
no idiomatic Rust equivalent; changing the insecure login model touches account persistence.

## Open questions for the owner

1. Which candidate — is a C++ dependency (d) acceptable at all, and is an async runtime (a) acceptable inside
   the engine crates?
2. Must existing multiplayer account directories and saves stay loadable (keeps the MD5 account-name scheme)?
3. Is the unauthenticated bearer-name login to be preserved, or replaced with real authentication?
4. Is an internet server list in scope? If so, who operates it?
5. Should single player keep a packet boundary (loopback transport, one code path) or call the simulation
   directly (mode (a) today)? Which `local_server` value does the shipped ini use?
6. Is the client-side password omission (§2) an original behaviour or a reconstruction gap?
7. Keep all traffic reliable-ordered for parity first, or move UPDATEOBJECT to unreliable immediately?
8. What replaces the MFC admin UI: headless + config file, TUI, or a GUI?
9. Is same-machine client + dedicated server (run-permission events) a supported scenario?
10. Target player cap: 128 as the dialog allows, or 16 as the admin tooling assumes?
