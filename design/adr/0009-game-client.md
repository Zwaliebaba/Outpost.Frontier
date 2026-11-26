# 0009 — The game client: context, frame, session and screen oracle

Status: proposed, 2026-11-26 (Phase 5, before P5.1; decision O50). Builds on
records 0001 (object model), 0002 (single player keeps one shared world),
0006 (controller bus) and 0007 (server world), on Phase 4's `neuron-client`
as accepted at Gate G4, and on Phase 3's `game-logic::host::ServerHost`.
The questions at the end need the owner's answer before P5.1 starts.

## Context

Paths are relative to `CppSource/` unless they name a crate.

### What the client is

- **Size.** `OutpostFrontier/Client/` is 246 `.cpp` and 245 `.h` files,
  233,040 lines ([assessment 06](../assessment/06-client-app.md) section 1).
  Phase 4 took the engine half of it: window, pump, clock, user state,
  renderer, toolkit, fonts, rich text, input, IME, audio, scene scripts,
  movies and the `Engine` value (P4.1 to P4.16). What is left is the game:
  the state machine, the session with the server, the client's view of the
  objects, cameras, effects rules, HUD, front end, NeuroNet screens and
  bases, about 150,000 lines over sub-systems 2 to 9 and 11 of the
  assessment.
- **Global state.** About 1,000 `G_*` identifiers and 110 `g_*` names, and
  state singletons reached through getters (`GetSpaceState`,
  `GetGameflowState`, `GetHudStatus`, `G_LoginMgr`, `g_client`); factories
  and command handlers register from static constructors, and teardown is
  40-odd ordered `atexit` calls (`main.cpp:500-585`). Two order
  dependencies are documented: `vis_effect_arch.cpp` after
  `client_cliobj.cpp`, and gameflow's hotspot handler before Win3D's
  (`gameflow.cpp:52-64`).
- **What `game-app` holds today.** The Phase 4 pieces that belong to the
  game: the profile and its registry import, `PerfOptions.ini`, the render
  mode pick, the low-machine warning, login history, account keys, table
  layouts, the client clock (`timing`), the input router, the startup
  movies and a camera stub, and `run()`, which builds the engine and runs
  its frame until the window closes. It does not yet depend on `game-sim`;
  the policy graph (`xtask/src/graph.rs`) already allows it.

### The frame

`TickClientFrame` (`OutpostFrontier/Client/client_frame_tick.cpp:187-293`)
runs, in this order: the global time (skipped while the server tick is
paused); the multiplayer disconnect check; `Win3DUpdateAll(dt)`;
`TickLocalServer(dt)`, which is `G_Server->Update()` then
`G_Server->ElapseTime(dt)` (`server_connect.cpp:155-162`); and the current
state's `Update(dt)`, which returns the next state id. Each state draws and
presents inside its own `Update`; in space that is the client object
update, `AnimDB::Update`, `PhySys::Update`, beams, collisions, lanes,
lights, docking, camera, the sound mixer, the scene, `Win3DDrawAll` and the
present (`space_state.cpp:1017-1205`). State ids are in `client_state.h`
(1 quit, 2 space, 3 base, 6 intro, 7 load game, 8 mission briefing).

### How single player moves messages

`local_server = rpclocal.dll`. Both directions are queues:
`PushToClient` enqueues every server-to-client call
(`rpclocal/rpc_client_proxy.cpp:257-261`), and the client's
`G_Server->Update()` is rpclocal's `IServerImpl::Update`
(`rpclocal/rpclocal.cpp:536-550`):

1. `GiveServerRunPermission`;
2. drain the client-bound queue into the client (`HandlePacket`, which
   calls the real `IClient` slot);
3. `WaitForClientRunPermission`;
4. the client pump (`RpcClientPump`, `rpc_client_proxy.cpp:376-400`):
   drain the server-bound queue into the server (`RpcDispatchToServer`).

`ElapseTime` then runs the server's frame, which fills the client-bound
queue again. So a call the server makes in frame N reaches the client in
frame N+1's `TickLocalServer`, before the server hears what the client sent
during frame N's state update. Record 0002 keeps one world and keeps these
messages (point 3); this order is what the loopback must reproduce.

### Modal dialogs

`NnErrorRunModal` (`nn_error.cpp:221-252`) and three `nn_prompt` variants,
the server filter dialog and the connect handshake run nested loops: sleep
10 ms, pump messages, `G_Server->Update()` only when the caller asked for
it (else wait for the run permission), `Win3DUpdateAll`, draw the
interface, until the dialog is gone. The state's `Update` and the server's
`ElapseTime` do not run meanwhile. `neuron-client::ui` already has the
modal stack that replaces the capture count (P4.13).

### The client oracle

`+oracle` (P0.5, [reference-build.md](../reference-build.md) section 8)
runs a script at a fixed 1/30 s step and seed and writes `session.jsonl`,
`tree.jsonl` per dump, `keymap.jsonl`, `layout.jsonl`, `iclient.jsonl` (one
line per call into the client event interface, logged where
`GetClientInterface` hands it out) and screenshots. On the owner's machine
`menu_walk.txt` repeats byte for byte; `single_player.txt` repeats in its
session, trees, layouts and key map, but not in its IClient log or its
space screenshots, because the in-process server is not on the fixed step
and one payload carries a per-run double and uninitialised costume bytes.
Gate G5 asks that interface-tree dumps equal the oracle for every screen.

## Decision

1. **One owning value.** `game-app` holds the client's state in one
   `ClientContext` (the engine, the state machine, the session, the
   client's object view, cameras, HUD, screens, options and key map),
   built and dropped in the C++ order. No statics. Registrations that ran
   from static constructors become explicit tables built in `ClientContext`
   construction, with the two documented order dependencies written as
   order. Teardown is `Drop` in the order of `main.cpp:500-585`.

2. **The state machine.** `ClientState` is an enum with the C++ ids;
   `update(&mut ClientContext, dt) -> Option<ClientStateId>` per state, and
   `EnterClientState` as one function. The menu's states (1 to 19 of
   `menu_enter_state.cpp`) and the room changes are their own enums. A
   state draws and presents inside its update, as in the C++.

3. **The session seam.** `game-app` defines a trait `Session` that the
   executable implements; `game-app` never sees `game-logic`:

   ```rust
   pub trait Session {
       fn world(&mut self) -> &mut game_sim::object::World;
       fn update(&mut self);              // the server's pump: server-bound messages
       fn elapse_time(&mut self, dt: f32);
       fn single_player(&self) -> bool;
   }
   ```

   The client's own end of the transport (`ToServer` out, `ToClient` in)
   stays in `game-app`. `TickLocalServer` becomes: drain the client's
   inbox into the event receiver (step 2 above), `session.update()` (step
   4), `session.elapse_time(dt)`. In single player `apps/outpost-frontier`
   implements `Session` over `ServerHost<LoopbackTransport>` and its world
   (`ServerHost::world` is public, so no `game-logic` API changes); in
   multiplayer (P6.1) over the client's own world, with `update` and
   `elapse_time` as no-ops and the QUIC connection as the transport. The
   run permissions have no counterpart: both sides run on one thread.
   `ServerHost` was written for the dedicated server; P5.3 checks what a
   single-player host needs beside it (the loopback's connection event, the
   single-player `SAVEGAME`, which `ServerHost` leaves to its caller through
   `game_logic::saves::server_save_game`) and names any change to
   `game-logic` in its pull request.

4. **The event receiver.** `game-app::session::receiver` takes each
   `game_sim::protocol::ToClient` and calls one handler per `IClient` slot
   (126 slots, `include/rpc_iclient.h`), in delivery order. The client's
   view of the objects follows record 0002: in single player a create
   message finds the shared world's object by id
   (`client_iclient_space.cpp:445` and the other creates) and the client
   adds its own state in a side table, `ClientObjects`, keyed by
   `ObjHandle`, as record 0007 keeps `ServerObjects`; in multiplayer the
   same message creates the object in the client's world. The client's
   `SinglePlayer()` tests are ported as they are.

5. **Modal frames.** A modal dialog pushed on the toolkit's modal stack
   switches the frame to the nested loop's body: pump, `session.update()`
   only when the dialog asked for it, `Win3DUpdateAll`, the interface draw;
   no state update and no `elapse_time`. The 10 ms sleep is the frame
   pacing. The script runner of point 6 waits while a modal frame runs, as
   the C++ script stops.

6. **The port's oracle mode.** `OutpostFrontier.exe +oracle <script>
   <out>` exists in the Rust client too, always compiled, with the same
   script language and the same JSON Lines records, written by
   `game-app::oracle` at the hooks section 8 lists. The comparison:
   `tree.jsonl`, `layout.jsonl` and `keymap.jsonl` equal after leaving out
   `rtti` (port-specific by design) and the `start` record's paths;
   `session.jsonl` equal in its screen changes, dumps and frames;
   `iclient.jsonl` equal for the menu scripts, and for scripts with a
   session only once question 3 is settled; screenshots by the Tier C image
   metric of record 0004. Platform-free parts (the state machine, the
   receiver, the script runner over a recording toolkit, the dumps) are
   tested on every host; the Rust client's own runs need Windows.

7. **Order of work.** P5.1 (boot, config, loading screen, state machine)
   and P5.2 (front end, options, key map) first: they rest on Phase 4 only
   and give the first screen comparisons (`menu_walk.txt`). P5.3 (session,
   receiver, new and load game) next; it is where Phase 3's open packages
   can still change the protocol. P5.4 to P5.8 then, and P5.9 and P5.10
   last, after P3.5c4, P3.5c5 and P3.5d, which they show.

8. **Registers.** Divergence rows D1100 to D1999 (O50). A change to
   `game-sim`'s object model or protocol, or to any file Phase 3 owns, is
   named in its pull request.

## Consequences

- Single player runs the C++ order without a second world or a lock, and
  without a new API in `game-logic`. The cost is that `game-app` reaches the
  world only through `Session`, so a borrow of the world cannot span a call
  to `update` or `elapse_time`; the C++ has the same shape (the server runs
  between the client's passes).
- The loopback delivers a server message one frame later, as rpclocal
  does. A design that delivered synchronously would change what the client
  sees in a frame and is not taken.
- Gate G5's screen criterion becomes a mechanical comparison of the two
  oracle modes' files. Where the C++ mode cannot run in CI (the trial job
  `.github/workflows/client-oracle.yml`), the comparison runs on the
  owner's machine from a runbook, as Gate G4's did.
- Saves: the C++ client writes no save files (`nn_load_save.cpp:611`,
  `:814` call the server). G5's interchange criterion rests on Phase 3's
  writer; Phase 5 adds the client's raw state blobs the server stores
  (interface state, visited state, weapon groups: `game_options.cpp:167`,
  `character_select_equip.cpp:523`, `hud_weapon_groups.cpp:304`), pinned
  byte for byte.

## Options rejected

- **`ServerHost` taking `&mut World` per call, the world owned by the
  executable.** Cleaner ownership, but it changes Phase 3's host API and
  every caller for no behaviour the trait cannot give.
- **The client's frame split into before-server and after-server
  functions sequenced by the executable.** Keeps `game-app` unaware of the
  server, but the modal loop decides inside the client whether the server
  updates, and the split would leak that decision into the executable.
- **A synchronous loopback** (deliver into the receiver inside the server's
  send). Not what rpclocal does; changes the frame a client sees an event
  in.
- **The Rust oracle mode as a Cargo feature.** Saves little; a separate
  build would be the build compared instead of the one shipped.

## Questions for the owner

1. The `Session` trait of point 3, with single player over
   `ServerHost<LoopbackTransport>`. Recommended.
2. The Rust `+oracle` mode always compiled and switched on by the command
   line (point 6). Recommended.
3. The C++ in-process server on the oracle's fixed step: oracle
   instrumentation in `TickLocalServer`'s caller (allowed by P0.5) so that
   `single_player.txt`'s IClient log repeats, with the per-run double of
   `SendServerCharSelectVerified` and the costume's unset bytes masked in
   the comparison. Recommended, as P5.3's first step; without it the IClient
   log of a session is compared as a sequence of slots only.
4. The comparison rules of point 6 (`rtti` left out, `flags` bits 0 to 3
   as the dump already writes). Recommended.
5. Where Gate G5's comparisons run: in CI if the trial job shows the C++
   client runs on a hosted runner, else on the owner's machine from a
   runbook. The trial's result is added here before this record is
   decided.
