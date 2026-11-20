# 0006 — Controller bus and re-entrant dispatch

Status: accepted by the owner on 2026-11-20 (P3.0); decision O40. Answers the risk "nested
message dispatch between server and content does not fit Rust borrowing"
(migration plan section 8) and the assessment's open question 4
(runtime-selectable controllers, [04-server-content](../assessment/04-server-content.md)).

## Context

Paths are relative to `CppSource/`.

### The registry

- **Entry point.** Every server-to-content message goes through
  `Server_ControllerDispatch(Flid, EMessage, void const*)`
  (`Server/IServerImpl.cpp:3176`), the body of `pub::Controller::_SendMessage`.
  `Flid & 0x3fffffff == 0` broadcasts: it walks the active controller list
  and calls `IDLL::HandleMessage` on each node, reading `cur->Next` after
  the call. Any other `Flid` is a targeted send to one controller, looked
  up in the active list and then in the deferred list; an unknown `Flid`
  returns -2. Only `pub::Controller::_SendMessage` hands the result back
  to Content (`Server/server_pub.cpp:6341`); every internal caller drops it.
- **The call is synchronous.** `HandleMessage` runs to completion before
  dispatch returns. Content handlers call `pub::` functions (about 244
  distinct names) that change server state and dispatch again, so dispatch
  nests depth first. The client boundary does not nest: rpclocal queues
  packets in both directions and drains them in its pumps
  (`rpclocal/rpc_client_proxy.cpp:255-262`, `:376-415`), as record 0002
  already assumes.
- **Order of the list.** `Server_ControllerCreate` (`:3285`) inserts a new
  controller *before* the first node whose priority is greater than or
  equal to its own (signed compare, `:3299`); the deferred drain does the
  same (`:4305`). Within one priority, the newest controller therefore
  comes first. With the shipped `[Initial SP DLLs]`
  (`outdir/OutpostFrontier.ini:49-61`) the order is GameSupport (HIGHEST),
  BaseSupport and SpaceSupport (NORMAL, in that order), StoryLauncher
  (BELOW_NORMAL), BasePop and SpacePop (LOWEST). The rows `AISandbox` and
  `TestAutomation` name no factory (`Content/Content.cpp:84-108`):
  `CreateInstance` returns null and `Create` returns 0, but the controller
  id has already been taken from `CFLIDMaker` (`:3287`), so the two rows
  still use up ids. An unknown priority word keeps the previous row's value
  (`:2992-2997`).
- **The re-entrancy guard.** Broadcasts and the heartbeat walk increment
  `g_ReentrancyGuard`; targeted sends do not. A controller created while
  the guard is non-zero goes to the deferred list. Targeted sends and
  `SetHeartbeatInterval` can already reach it there (`Server_ControllerFind`,
  `:3155`), but no broadcast or heartbeat visits it. The deferred list is
  merged into the active list when the guard falls back to 0, at the end
  of the outermost walk (`:3187`, `:4292`). As a result **the active list
  never changes while a walk is running**.
- **Destruction is queued.** `Server_ControllerDestroy` (`:3335`) ignores
  a controller id already queued and searches only the active list (so it
  cannot destroy a deferred controller). It queues the node and broadcasts
  `ControllerDestroyed` at once, the dying controller included. The node
  keeps receiving messages until the top of the next heartbeat walk, which
  calls `DestroyInstance`, unlinks the node and frees it (`:4259-4272`).
  Shutdown sorts the active list by id and destroys from the last
  (`:4224`).
- **Heartbeats.** Each node has a period (-1 for none) and an accumulator.
  `SetHeartbeatInterval` seeds the accumulator with `rand()/32768 × period`
  (`:3215`), which is one of the load-time draws in `load_rand.jsonl`. The
  walk (`:4274-4290`) adds `dt` in x87 precision, compares the unrounded
  sum, delivers `floor(sum/period)·period` as `Heartbeat`, and only then
  subtracts that value from the accumulator it reads again. A handler that
  resets its own heartbeat during the call therefore changes the result.
- **Timers.** `pub::SetTimer` appends to a list (`:3245`); no call
  cancels a timer. Each frame `Server_TimerFiring` (`:4335`) subtracts `dt`
  from every timer and collects at most 128 expired ones into an array
  (the warning text says 64). It then sends each one `TimerExpired`,
  targeted at its own controller, and frees the timer after that handler
  returns. A timer set while timers are firing is appended and loses no
  time until the next frame. The order of one server frame is
  `ElapseTime` (`:295`): systems and physics, subsystems, the heartbeat
  walk (destroy queue first), then timers.

### Payloads

- The payloads are untyped (`include/common_server_controller.h`):
  63 server ids (`0x00..0x41`) and 18 Content ids (`0x1000..0x1011`). Some
  are *out-parameters*: after `EquipDropLoot` the server copies
  `Event.Desc` back (`Server/server_gameobject.cpp:3310`, `:3424`), and
  SpaceSupport rewrites it (`Content/SpaceSupport.cpp:654-667`).
  `ObjectDestroyed` carries the dropped equipment, which SpaceSupport
  sorts, merges, recounts, extends with phantom loot or clears
  (`PrepareLoot`, `SpaceSupport.cpp:351-511`, `:1030`, `:1036`). The
  server then drops what is left (`server_gameobject.cpp:1175-1180`).
  Missions, which come before SpaceSupport, see the list unchanged.
  `DeathFuseLit` hands back `Ev.Lifetime`, which the conversation code
  writes (`Content/Gcs.cpp:3915-3917`; `server_gameobject.cpp:1105`,
  `:1300`). `FreeTerminalRequest` answers through the global
  `GReportedFreeTerminal` (`server_classes.cpp:12149`). Others lend a
  resource: `PlayerSave` and `SaveGame` lend a `FILE*`, `PlayerLoad` and
  `LoadGame` an `INI_Reader*`, `EncounterLaunched` a `std::list<uint>`,
  `NpcShipCreated` an object by address.

### Content

- **One shared state.** The 23 controller kinds of `CreateInstance` are
  facades over one body of process globals: `GMessageRouter`, the player
  states, the system infos, the dock queues, the conversation queue and
  about 60 STL tables. Content's state-machine router
  (`Content/ContentMessageRouter.cpp`) delivers a message either at once,
  as a nested call, or after a delay. A delayed message goes into a list
  sorted by delivery time and arms one server timer on SpaceSupport's
  controller id (`:85-117`).
- **Content's own sends.** Content sends through `_SendMessage` from about
  32 sites, creates controllers and destroys them, its own controller
  included (`MissionObject.cpp:140`, `MissionScript.cpp:1468`,
  `Story.cpp:176`). Missions are created at ABOVE_NORMAL
  (`Story.cpp:168`, `:280`, `BaseSupport.cpp:429`, `BaseDB.cpp:792`,
  `Support.cpp:369`). Story is created at NORMAL by StoryLauncher
  (`StoryLauncher.cpp:76`), and a story mission restored from a save also
  at NORMAL (`Story.cpp:374`). A running game therefore broadcasts in this
  order: GameSupport; the missions, newest first; Story (or a restored
  mission ahead of it); BaseSupport; SpaceSupport; StoryLauncher; BasePop;
  SpacePop.
- **Content depends on that order.** Three examples:
  - GameSupport creates the player's ship record on `SpaceEnter` and sets
    the system, base and location records on `SystemEnter`, `BaseEnter`
    and `LocationEnter`. The controllers after it use those records
    without testing them (`GameSupport.cpp:409-447`,
    `Populators.cpp:530-540`).
  - On `ObjectDestroyed` the missions read the dying NPC
    (`MissionObject.cpp:1705`, `:1714`) before SpaceSupport deletes it
    (`SpaceSupport.cpp:1075`).
  - A nested broadcast from SpaceSupport (`FormationLeaderChanged`,
    `DamageReputation`, `NpcShipCreated`) reaches the controllers after
    SpaceSupport before the outer message does.
- **Mission triggers** (`Content/MissionScript.cpp:436-486`). One pass
  walks the `ActiveTriggers` list. A trigger that matches is set to
  COMPLETE *before* its actions run, so a nested pass on the same mission
  skips it and the outer pass can still erase it. Nested passes erase
  other completed triggers and add new ones, and the outer pass sees
  those additions. `Act_ActTrig` goes into a map keyed by trigger id,
  which is drained at the end of every pass, nested passes included.
  `Act_ChangeState` takes effect at the end of `HandleMissionMessage`. A
  nested call on the same mission therefore ends the mission while the
  outer pass is still running its actions. A mission destroys itself
  after its handler returns (`MissionObject.cpp:129-142`). It is freed at
  the next heartbeat walk, and until then it ignores everything.

### Re-entrance hazards

The P3.0 survey looked at all 98 dispatch call sites in `Server/` and the
Content handlers; its result is in [Appendix A](#appendix-a-survey-of-the-dispatch-sites).
In summary:

- **A nested world update.** `pub::Player::ForceLand` calls
  `IServerImpl::Dock` (`server_pub.cpp:5177`). When the base is in another
  system, `Dock` runs a whole `StarSystem_Update(System, 0.2f)` from inside
  the handler (`IServerImpl.cpp:2700`). That nested update ticks the lists,
  drains the deferred-delete list, processes and erases the dock queue
  and, in single player, can shut the system down.
- **Walks the handlers can change.** The `MetaList` walk
  (`include/common_metalist.h:162`) reads `Node->Next` when it steps, so
  an object appended during the walk is updated in the same pass. When the
  walker removes its own node, `remove` leaves the cursor on the previous
  node. When a nested handler removes the walker's current node (for
  example `pub::SpaceObj::Relocate` → `remove_ship` on the ship being
  updated), the cursor is left on freed memory. The same holds for the dock
  queue walk (`IServerImpl.cpp:3934-4020`), the tradelane notifications
  inside the ship walk (`server_classes.cpp:10480-10589`),
  `GDyingShips` (`IServerImpl.cpp:4199`), `EqObj::process_dock_queue`
  (`server_gameobject.cpp:2527-2622`) and `CPlayerGroup::DelMember`
  (`server_classes.cpp:4620-4626`). None of these walks has a guard. Most
  of them are undefined behaviour only when a handler happens to take that
  path.
- **Pointers held across a call.** Objects, equipment and trade offers
  are used after the call without being looked up again. A destroy nested
  inside the same object's destroy runs `ObjectDestroyed` and
  `on_object_destroyed` twice (`server_gameobject.cpp:1167`,
  `server_loot.cpp:156`).
- **Other synchronous call-outs.** The heartbeat walk calls Content
  directly. Server also calls Content through the enumerator callbacks
  (`pub::Player::EnumerateCargo`, `pub::SpaceObj::EnumerateCargo`,
  `pub::System::EnumerateObjects`, `pub::Reputation::EnumerateGroups`),
  through `CreateInstance`/`DestroyInstance`, through the request callback
  of `Ship::request_returned` (`server_gameobject.cpp:5532`) and through
  the behaviour manager's directive callbacks that `pub::AI::SubmitState`
  installs (`server_pub.cpp:6990`).

### The Rust world today

`game-sim::World` queues what happens during a step in
`events: Vec<WorldEvent>` for the caller to take afterwards
(`crates/game-sim/src/object/world.rs:329`, `:493`). Its tick walks a copy
of each list (`World::list`, `:944`; `step.rs` `tick_list`), so an object
created during the walk is not updated until the next frame. No divergence
row records that difference. `BehaviorHook` (`world.rs:96`) is the one
synchronous call-out. It is moved out of the world for the call and put
back afterwards (`take_behavior`, `put_behavior`), and it receives
`&mut World`.

## Decision

1. **Dispatch stays synchronous and depth first.** No queue stands
   between the server and the controllers. A message reaches every
   recipient, nested sends included, before the sender's next statement
   runs, as in the C++. Any queue changes the order of messages, `rand()`
   draws and id allocation, and Gate G3 compares all three through saves,
   mission dumps and `load_rand.jsonl`. The only queues are the ones the
   C++ already has: rpclocal's packet queues, the controller destroy
   queue, the deferred-create list, the timers, Content's delayed
   messages, and the world's deferred-delete list.

2. **One context, passed by `&mut`, borrowed only for a short time.** The
   server side is one context made of two parts: the world, and everything
   else (players, systems, bases, the controller bus, the timers and
   Content's state). It is passed as

   ```rust
   pub struct Cx<'a> { pub world: &'a mut World, pub logic: &'a mut Logic }
   ```

   Every function that can dispatch takes `&mut Cx`. That includes the
   `pub::` functions, which become methods (P3.3). A nested dispatch is
   then an ordinary recursive call with a reborrowed `&mut Cx`. No
   `RefCell`, `Rc`, lock or `unsafe` is involved.

   The borrow checker enforces the one rule that matters: **no borrow of
   context state outlives a call that can dispatch.** Where the C++ keeps
   a pointer or an iterator across such a call, the port keeps a handle,
   key or index and looks the item up again afterwards. Where the C++
   result depends on whether the item survived, the lookup decides, as
   point 7 sets out. In single player the executable owns the world
   (record 0002), so `Cx` borrows it there. On the dedicated server it
   borrows it from the server's own field.

3. **Controller state stays in its slot; handlers get the id.** Content's
   state is one `Content` value inside `Logic`, holding the globals and,
   per controller, its instance state keyed by controller id. A handler is
   `fn(cx: &mut Cx, me: ControllerId, msg: &mut ControllerMessage<'_>)`.
   It borrows `cx.logic.content.instance_mut(me)` only between call-outs.
   The instance is never moved out of the registry for the duration of a
   call (rejected option C), so a nested message to the same controller
   finds it there. Same-instance re-entrance does happen in the C++
   (Appendix A).

4. **The bus in `neuron-server`, generic and exact.**
   `neuron-server::controller` ports the registry as data and walk
   primitives, generic over the controller kind and the message type. It
   covers:

   - priority insertion ahead of equals, signed for create and as the drain
     has it;
   - the guard counter, the deferred list and the drain point;
   - lookup over the active list, then the deferred list;
   - `NoController` in place of -2;
   - the destroy queue, its duplicate test and active-list-only search,
     the `ControllerDestroyed` broadcast, and teardown at the top of the
     heartbeat walk;
   - the heartbeat arithmetic on the T11 model, with the accumulator read
     again after the call;
   - the random phase drawn from the CRT generator in the context;
   - the timer list with its 128 cap and its warning text;
   - the shutdown order;
   - id allocation through the same `CFLIDMaker` counter, so that a failed
     create still uses up an id.

   The walk algorithm (`broadcast`, `send`, `heartbeat`, `fire_timers`) is
   generic over a host trait:

   ```rust
   pub trait BusHost {
       type Kind: Copy;
       type Msg<'m>;
       fn bus(&mut self) -> &mut ControllerBus<Self::Kind>;
       fn deliver(&mut self, id: ControllerId, kind: Self::Kind, msg: &mut Self::Msg<'_>);
   }
   ```

   `game-logic` implements the trait for `Cx`. Because the active list
   cannot change during a walk (Context), a walk iterates the active
   vector by index. Before each delivery it copies `(id, kind)` and drops
   the borrow. A debug assertion checks that the length is unchanged.
   Teardown calls the kind's destroy hook (Content's destructors) after
   the node has left the queue that `Destroy` searches. The C++ reads freed
   nodes there (Appendix A, server item 10); the port's defined behaviour
   is a divergence row.

5. **A closed set of controllers, chosen in the INI.**
   `[Initial SP DLLs]` and `[Initial MP DLLs]` keep their syntax and are
   read as `Server_LoadInitialDlls` reads them. The DLL name is kept for
   the record, but the instance name selects a variant of
   `ControllerKind`: the 23 factory names, compared case-insensitively,
   first match wins. An unknown name fails as a null `CreateInstance`
   does, after the id has been taken. The oracle's stub and the test
   controllers are further variants, compiled only for tests. Rust has no
   `LoadLibrary`.

6. **Typed messages, carried by `&mut`.** `ControllerMessage<'a>` has one
   variant per id: the 80 ids that are sent and `NpcSystemEnter`, which is
   used only as a trigger event (81 in all; corrected by the P3.2 survey). Each variant keeps its numeric id
   (`id()`) for traces. Out-parameters (`EquipDropLoot.desc`,
   `ObjectDestroyed.dropped_equip`, `DeathFuseLit.lifetime`) are fields that a handler may change and the
   sender reads back. Lent resources are borrows: `&mut dyn Write` for the
   save writers, `&mut IniReader`, `&[u32]`. Truncated pointers become
   handles. Answers given through a global (`GReportedFreeTerminal`) become
   reply fields of their message. The names follow
   `common_server_controller.h`; the three `Msg0x..` ids get names when
   P3.2 settles their meaning.

7. **The world calls out where the C++ does, and its walks follow
   `MetaList`.**

   - **Hooks.** The world gets a `ServerHooks` trait, passed into
     `World::step` and called synchronously at the C++ dispatch points
     inside the update: object destroyed, death fuse lit, damage, loot
     spawned and acquired, dock complete, tradelane enter and exit, the
     dock-queue messages, phantoms and dying ships. Like `BehaviorHook`,
     a hook method receives `&mut World` and the rest of the context, and
     builds the `Cx` it dispatches with. `WorldEvent` stays the
     client-side and test record and is still filled in. The server no
     longer depends on it for ordering.
   - **Walks.** P3.3 turns the world's object lists into linked lists over
     the arena, with generational node keys. A walk then behaves as the
     C++ walk does:
     - an append during the walk is visited in the same pass;
     - removing a node other than the current one takes effect at once;
     - when the walker removes its own node, the cursor goes back to the
       previous node.

     The same walk type replaces Content's `std::list` walks that a nested
     call can change: a mission's active triggers, the jumpers, the guided
     ships, the NPC event list and the dock launch queues. A `std::list`
     walk behaves the same way when another node is erased. The trigger
     pass is the case that matters for the record: there the C++ is well
     defined and its result depends on these rules. Index walks over a
     vector that read the length again on each step (`DestroyMission.cpp:392`)
     stay index walks.

     When a nested handler removes the walker's current node, the C++ reads
     freed memory. The port's defined behaviour is to continue from the
     successor that node had when it was removed, which is what the freed
     node still held at the time. That rule applies to every walk listed in
     Appendix A: world lists, dock queues, `GDyingShips`, group members,
     trade items and cargo enumerations. Each walk gets a divergence row
     when its package ports it.
   - **Held pointers.** Where the C++ uses a held pointer after a call, the
     port looks the handle up again. If the object is gone (the C++ would
     use freed memory), the port takes the C++'s path as though the object
     were still there only when the code after the call does not touch it.
     Otherwise the rest of the work is skipped. Each such site gets a
     divergence row naming the rule it took.
   - **Destroy twice.** A destroy nested in the same object's destroy runs
     twice, as in the C++. That is defined behaviour and observable, so
     the port keeps it.
   - **`ForceLand`.** The nested `StarSystem_Update` is ported as a
     nested call of the same method.

8. **Content's router and timers.** The router is ported as it is:
   immediate delivery is a nested call, delayed delivery goes through the
   sorted list and a bus timer on SpaceSupport's id, and the router's
   timer id comes from Content's counter. The enumerator callbacks become
   closures or `FnMut` arguments called inside the walk, under the same
   cursor rule.

9. **Recursion depth.** The server thread is spawned with an explicit
   stack size (16 MiB, which P3.1 can tune). A frame in the Rust handlers
   is not the size of the C++ frame, and the default stack sizes differ.
   The single-player executable runs the server on the thread that runs
   it today (record 0002), and gives the main thread the same size through
   the linker.

10. **Verification.**

    - **Scheduler vectors (Tier A).** A new Oracle suite `controller_bus`
      drives the real `Server.dll` registry with scripted controllers.
      `SimClientStub.dll` gains a `ScriptedController` factory whose
      handlers carry out a script on given messages: send, broadcast,
      create (with priority), destroy (self or other), set a heartbeat,
      set a timer, nest to depth N. Each delivery is recorded as
      `{seq, depth, flid, msg, guard, list}`. Synthetic scripts cover:
      - ties in priority;
      - a create during a broadcast, during a targeted send and during a
        heartbeat;
      - a destroy of self during a broadcast;
      - a destroy of a deferred controller;
      - a destroy twice;
      - a heartbeat reset during its own call;
      - more than 128 timers;
      - a timer to a missing id;
      - shutdown.

      `neuron-server` runs the same scripts on `ControllerBus` and must
      produce the same records. The vectors go to
      `crates/neuron-server/tests/vectors/`.
    - **Dispatch trace (Tier A to the first difference).** The `sim` mode
      records every dispatch with its depth and recipients into
      `dispatch.jsonl`, which widens `controller.jsonl` beyond the stub's
      view (sim-records.md). From P3.3 on, `scenario_replay` compares the
      Rust world's trace with it. The order must be equal; the payloads
      are compared where they are decoded. Gate G3's `dock_at_station`,
      `trade_lane` and `dock_trade_lane` scenarios carry the trace.
    - **Divergence rows** for every undefined-behaviour site that points 4
      and 7 define, added by the package that ports the site.

## Consequences

- The observable order is the C++ order by construction, and a scheduler
  vector checks it before any Content code is written.
- Handler code is more verbose than the C++: it looks things up again
  after every call that can dispatch. The verbosity is the price of the
  borrow checker proving re-entrance safety. In the C++ the same
  discipline was missing in about 20 places (Appendix A).
- Any function that can dispatch has `&mut Cx` in its signature, so a
  reviewer can see which calls might re-enter.
- `neuron-server` stays free of game rules; the bus is tested there with
  its own vectors.
- P3.3 changes `game-sim`: the hooks in `World::step` and list walks with
  cursors in place of copies. The P2.4 scenarios are unaffected while no
  hook changes the world, because the walk over an unchanged list visits
  the same objects in the same order. The copy-walk difference closes
  without a divergence row.
- A controller kind cannot be added without a rebuild. The original data
  needs none beyond the 23 kinds; the two unknown rows keep their
  id-consuming failure.

## Options rejected

- **A. One FIFO event queue (breadth first).** It changes the order of
  delivery, `rand()` draws and id allocation. It also cannot express
  out-parameters (`EquipDropLoot`, `ObjectDestroyed`, `DeathFuseLit`),
  whose sender reads the answer on the next line. And it breaks Content's
  dependence on order: a mission would read a dying NPC after
  SpaceSupport has removed it.
- **B. A queue drained at each send (a stack emulating depth first).**
  This is equivalent to synchronous dispatch only if the sender waits for
  the drain. That is synchronous dispatch with extra machinery, and the
  borrow problem stays.
- **C. Take the controller out of its slot for the call (`Option::take`,
  as `BehaviorHook` does).** A nested message to the same controller would
  then find an empty slot. Same-instance re-entrance happens (Appendix A),
  so this option would drop messages or need a queue for them, which is
  option A again for those messages.
- **D. `Rc<RefCell<..>>` per controller or per table.** The same failure
  as C, but as a panic at run time instead of a compile error. It also
  hides which calls re-enter.
- **E. Actors, channels or async tasks.** They lose the synchronous return
  and the out-parameters, and add scheduling that is not deterministic or
  not the C++'s.
- **F. Plug-in controllers through dynamic loading.** The data names one
  DLL. A stable Rust ABI would need `unsafe` in a crate that forbids it,
  and decision O1 rules out sharing a process with C++ DLLs.
- **G. Keep `WorldEvent` as the only path from world to server.** The
  server would see every event of a step after the step. Content's
  answers to `ObjectDestroyed`, `DockComplete` and the rest would land a
  whole step late and in a different order relative to the update.

## Appendix A: survey of the dispatch sites

### Server (98 call sites of `Server_ControllerDispatch`)

Ten matches are comments and one is the definition. Of the 98 sites, 37
are in `IServerImpl.cpp`, 33 in `server_classes.cpp`, 20 in
`server_gameobject.cpp`, 6 in `server_pub.cpp`, 1 in `server_loot.cpp`
and 1 in `server_savegame.cpp`. Each site was put into one of four
classes:

- **plain**: nothing held, nothing read back;
- **stable storage held**: only a `PlayerDB` slot, a `SysMap` system or a
  Universe base is held across the call;
- **readback**: the sender reads the payload or server state afterwards;
- **walk** or **held pointer**: a walk or a pointer that a handler can
  invalidate.

The ten most exposed sites:

1. **A nested world update.** `ForceLand` → `Dock` →
   `StarSystem_Update(System, 0.2f)` (`server_pub.cpp:5177`,
   `IServerImpl.cpp:2700`).
2. **The dock-queue walk** in `StarSystem_Update` (`IServerImpl.cpp:3934-4020`,
   `Msg0x27` at `:3986` and `:4013`). The next pointer is read after the
   call, and a nested update erases the queue. `Obj` is destroyed with no
   second check.
3. **`StarSystem_TickObjectList`** (`server_observer.cpp:1044`) with
   `remove_ship` from `pub::SpaceObj::Relocate`, `JumpIn` and `Launch`
   (`server_pub.cpp:2911`, `:2954`, `:2999`). The node is deleted, because
   `SysMap` copies have `bUseHeap = 0`.
4. **The tradelane notifications** inside the ship walk
   (`server_classes.cpp:10493`, `:10584`).
5. **`EqObj::process_dock_queue` → `complete_dock`**
   (`server_gameobject.cpp:2527-2622`, `server_classes.cpp:10232`).
   `pub::SpaceObj::Dock` → `dequeue_dock_request` erases the walker's node.
6. **`CPlayerGroup::DelMember`** (`server_classes.cpp:4620-4626`): an
   iterator is held across `GroupMemberLeft`. A nested `RemoveFromGroup`
   makes the erase happen twice.
7. **`PlayerWorthChanged` inside walks.** `CPlayerGroup::RewardMembers`
   (`server_classes.cpp:4873`) walks the members. `AcceptTrade`
   (`IServerImpl.cpp:2426-2487`) holds offers and item lists, which
   `TerminateTrade` deletes.
8. **`GDyingShips`** (`IServerImpl.cpp:4199-4207`): `~Ship` →
   `Ship_ForgetDying` erases nodes.
9. **`StarSystem::remove_player`'s ship walk**
   (`server_classes.cpp:6140-6152`), whose destroys broadcast
   `ObjectDestroyed` and `SpaceExit`.
10. **Controller teardown** (`IServerImpl.cpp:4259-4272`): nodes are freed
    while they are still in `g_DestroyPending`. A `Destroy` called from a
    destructor reads freed nodes.

Other exposed sites:

- **Readbacks:** `EquipDropLoot` (`server_gameobject.cpp:3310`, `:3424`),
  `DeathFuseLit` (`:1105`, `:1300`), `FreeTerminalRequest`
  (`server_classes.cpp:12149`), and `PlayerSave`'s file error flag
  (`server_pub.cpp:1908`).
- **Held pointers:**
  - `SystemSwitchOutComplete`'s ship (`IServerImpl.cpp:1245`);
  - `tractor_loot` (`server_classes.cpp:6400`);
  - `create_loot` (`:9396`);
  - the equipment in the damage entries (`server_gameobject.cpp:1551-1743`);
  - `CargoJettisoned` (`:5759`);
  - the character record in `PlayerData_SelectCharacter`
    (`server_classes.cpp:2883-2914`).
- **Destroy twice:** `EqObj::destroy` (`server_gameobject.cpp:1167`) and
  `Loot::destroy` (`server_loot.cpp:156`) have no latch against a nested
  destroy of the same object.
- **Guarded walks:**
  - the phantom pump (`server_pub.cpp:2236`, `GbPhantomPumpBusy` makes
    `pub::Phantom` return -5);
  - timer firing (a copied array; no call cancels a timer);
  - the controller broadcast (the guard).

The survey shows what the code allows, not which paths the shipped Content
takes. The dispatch trace (point 10) shows the paths taken.

### Content

**The same instance re-entered** while its handler is still running:

1. **SpaceSupport.**
   - `Msg0x25` → `pub::SpaceObj::Destroy` (`SpaceSupport.cpp:756`,
     `server_pub.cpp:2585`) → `ObjectDestroyed`
     (`server_gameobject.cpp:1167`) → SpaceSupport.
   - `ObjectDamaged` → `SendDamageReputation` (`CombatReputation.cpp:83-93`)
     → `DamageReputation` → SpaceSupport (`SpaceSupport.cpp:939`).
   - `OnObjectDestroyed` → `PromoteFormationLeader` → `FormationLeaderChanged`
     (`:287-298`, `:1042`).
   - Its heartbeat sends targeted `CommComplete` and `NpcTurnedHostile`
     messages to itself (`Gcs.cpp:3533`, `CombatReputation.cpp:552`).
   - Every nested call runs SpaceSupport's tail again (`:967-983`).
2. **A mission.**
   - `Act_SpawnShip` → `CreateNpcShip` → `NpcShipCreated`
     (`SystemInfo.cpp:779-783`) → the same mission's trigger pass
     (`MissionScript.cpp:1246-1256`), then `EncounterLaunched` to itself
     (`MissionObject.cpp:2759`).
   - `Act_Destroy` → `ObjectDestroyed` → `OnObjectDestroyed`
     (`MissionScript.cpp:1525-1546`).
   - `Act_AdjAcct` → `PlayerWorthChanged`.
   - The mission's end → `LeaveLiveMission` → `PlayerMissionChanged`
     (`PlayerState.cpp:707-714`) → the same mission. Then
     `FStory::OnMissionEvent` destroys it and starts the next mission
     while the old mission's frames are still on the stack
     (`Story.cpp:255-330`).
3. **BaseSupport.** `MissionResponse` → `JoinMissionAsGroup` →
   `PlayerMissionChanged` → BaseSupport (`BaseSupport.cpp:384-480`,
   `PlayerState.cpp:853-891`).
4. **GameSupport.** `PlayerLoad` → `[RandomMission]` → `PlayerMissionChanged`
   (`Support.cpp:366-381`). Every later controller receives the nested
   message before `PlayerLoad`.

**Walks that a nested call changes**, and how they are protected:

| Walk | Site | C++ protection |
| --- | --- | --- |
| a mission's active triggers | `MissionScript.cpp:436-473` | the current trigger is set COMPLETE before its actions run; other nodes may be added or erased (well defined) |
| guided ships | `SpaceSupport.cpp:970-983` | removal only nulls the entry, but a nested SpaceSupport call runs its own erase pass under the outer iterator (undefined) |
| NPC events | `NpcShip.cpp:1057-1090` | none: a nested `ObjectDestroyed` erases the entry, and the outer `It->TimerId = 0` also clears a timer the call just armed |
| jumpers | `MissionObject.cpp:2360-2462` | none |
| dock launch queues | `DockQueues.cpp:2016-2156` | none; a `Params&` is held across `NpcShipCreated` |
| docking ships, terminals, delayed comms | `DockQueues.cpp:585`, `:1749`, `Gcs.cpp:3505` | ids collected first, then sent |
| mission objects | `SystemInfo.cpp:448-534`, `:906-924` | ids copied, each looked up again |
| ships whose life expired | `SystemInfo.cpp:955-1030` | copied, but as raw pointers |

**Pointers held across calls.** Player states held by missions and
system records are recreated on `PlayerLoad` and deleted on
`PlayerLogout` (`PlayerState.cpp:640-672`). They are safe only because
GameSupport runs first and the missions cache them again. NPC ships are
deleted by SpaceSupport's `OnObjectDestroyed` (`SystemInfo.cpp:544-556`).

**The router.** A timer pops the *head* of the sorted queue, not the
message that armed it (`ContentMessageRouter.cpp:137-147`). Delivery
times come from Content's game time, which moves only on GameSupport's
1 s heartbeat, so the order of the queue and the order of the timers can
differ. The faction-name timers (`FactionProps.cpp:698-722`) and the
dock-hail timers (`Gcs.cpp:3746-3752`) make the same assumption. The port
keeps it.

**Not settled by the survey.** It is not known whether
`pub::AI::SubmitDirective` reaches `DockCancel` synchronously: the only
evidence is the comment at `PatrolGuides.cpp:1160-1167`, and the
behaviour manager is in Common. It is also not known whether Common's
state-machine groups defer the deletion of machines. Both are settled by
the dispatch trace, or by P3.6 when it reads the code.
