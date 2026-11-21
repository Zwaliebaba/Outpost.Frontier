# 0007 — The server world over the shared arena

Status: accepted by the owner on 2026-11-20 (P3.3); decision O43.
Builds on records 0001 (object model), 0002 (single player keeps one
world) and 0006 (controller bus, point 7: hooks and `MetaList` walks).

## Context

Paths are relative to `CppSource/`.

### The C++ server world

- **Star systems.** `SysDB::SysMap` (`include/server_sysdb.h:1750`) is a
  `std::map<uint, StarSystem>` with one entry for every Universe system,
  made at startup (`Server/IServerImpl.cpp:3398-3410`).
- **What a `StarSystem` holds** (`server_sysdb.h:744-859`):
  - the players, as embedded `Observer`s, one per player;
  - the dock queue (`FSystemDockEntry`, `:691-726`);
  - a BSP over the solars;
  - ten `MetaList<GameObject>` lists: ships, loot, solars, guided, beams,
    mines, counter-measures, asteroids, always-update, deferred;
  - a loaded flag.
- **What it does not hold.** Asteroid fields are a global map keyed by
  system with a reference count (`Server/server_classes.cpp:5227`, `5909-5974`).
  Timers are global (record 0006), and zones come from Universe.
- **Loading.** A system loads when its first player arrives (`add_player`
  → `load`, `server_classes.cpp:6770-6872`). Single player wraps the
  solars the client already built; multiplayer reads the system file's
  `[Object]` sections.
- **Stepping.** `Server_StepSystems` (`IServerImpl.cpp:4144-4199`) steps
  each distinct system of the active players; a loaded system with no
  player is not stepped. `StarSystem_Update` (`IServerImpl.cpp:3863-4121`)
  runs the sub-steps:
  - the object lists, in a fixed order;
  - the asteroid fields;
  - deleting the deferred list;
  - the dock queue;
  - in multiplayer, physics and collisions;
  - then the observers.
- **Unloading.** Single player unloads a system when the player leaves or
  lands. Multiplayer keeps every system loaded until shutdown.
- **Objects.** `GameObject` wraps the Common `CObject` (it holds a pointer;
  it does not inherit) and adds server state (`Server/server_gameobject.h`):
  - for every object: the players who marked it, invincibility, the
    destroyed latch, the pending `DamageList`, the last damage time;
  - for equipped objects: the dock request queue, cargo decay, equipment
    listeners and lit fuses;
  - for ships: the death sequence and jettison timers;
  - loot timers, and the phantom and seek state of mines and
    counter-measures.

  A wrapper's `update` drives its `CObject`. An object that is destroyed
  leaves its list on its next update and is deleted from the deferred
  list.
- **Lookup.** `Server_FindObject` (`server_classes.cpp:11941-12002`)
  searches, in order:
  1. a one-entry cache;
  2. the cached system;
  3. the in-transit objects (`GInTransitObjects`, objects relocated to
     system 0, with saved physics but no body);
  4. every system's lists in a fixed order.
- **Moving between systems.** `remove_ship` → `add_ship` → `arrive_ship`
  (`server_classes.cpp:10286-10417`), used by gate jumps, `JumpIn`,
  `Launch`, `InstantDock` and `Relocate`.
- **Replication** (`Server/server_observer.cpp`, `server_sysdb.h:240-457`).
  Each observer (one player in one system) keeps the objects it tracks.
  Each tracked object has a remote-physics simulation and a
  created-on-client flag. The observer also keeps pending damage ids and
  its own clock.
  - **Visibility.** Always visible in single player and to the owner.
    Otherwise in range: 2500 m for unowned objects, 5000 m for another
    group's, 10 000 m for the player's own group's.
  - **Single player** sends every create at once and skips the per-frame
    pass.
  - **Multiplayer** runs five phases per system frame: the created
    records' own work; new creates; destroys and sync checks; updates
    ordered by simulation time; deferred damage. Each phase stops once
    the link's saturation reaches 0.9.
  - **Fan-outs.** Equipment, cruise, thrusters, target, fire, fuses, comm,
    tradelanes, group changes and cargo updates go to the observers that
    created the object.
- **The `pub::` surface.** About 270 exports:
  - SpaceObj (56) and System (9) work on star systems;
  - Player (57) works on the player database;
  - GF (34) and Market (9) work on the base database;
  - Reputation (16) and Zone (16) work on Common;
  - AI (15) and Phantom (5);
  - Controller (4) is the bus;
  - Audio (3);
  - 47 top-level getters.

### The Rust world today

`game-sim::object::World` (records 0001 and 0002):
- **Arena and lists.** One arena with generational handles, one id → handle
  map (`World::find`), and per-system `SystemObjects` with the tick lists.
- **Systems.** `load_system` covers physics and the collision filter only;
  there is no unload. `step(system, dt)` runs the C++ sub-step order.
- **Spawning.** Spawners for every kind.
- **Damage and fields.** Damage, explosions, and asteroid fields with
  their mines.
- **Removal.** A dead flag and a reap instead of the deferred list.

It has no players, observers, dock queue, system transfer, in-transit
state or server object state. `game-logic` has the server context
(`Cx { world, logic }`, record 0006), the player database, accounts,
chat and groups (P3.2), and the markets (P3.3).

## Decision

1. **Server object state lives in `game-logic`, in side tables keyed by
   handle.** The game-sim object stays the one object both sides share
   (records 0001 point 5, 0002). `game-logic::objects` keeps a
   `ServerObjects` table: generational, keyed by `ObjHandle`, with one entry
   per object the server created or wrapped. An entry holds:
   - the common server state (marks, invincibility, the destroyed latch,
     the pending damage list, the last damage time);
   - an enum for the per-kind state (equipped and docking, ship, loot,
     mine, counter-measure).

   An entry is made with the object and removed at the reap.

2. **The wrapper's update is a hook in the world's walk.** `World::step`
   keeps the C++ sub-step order and its lists. Where a `GameObject`'s
   `update` runs around its `CObject`, the step calls
   `ServerHooks::object_update(handle, dt)` before the object's own update.
   The hook is the record 0006 trait, so it gets the context and may
   dispatch; game-logic's implementation works on the side table. In
   single player, where the client side also steps the world (record
   0002), the server hooks run only in the server's pass.

   *Amended by the owner on 2026-11-21 (P3.3's object part).* The C++
   wrappers do not all run before their `CObject`: `Ship::update` runs
   server logic on both sides of it and skips it during a death sequence,
   and `Loot`, `Mine` and `CounterMeasure` update the `CObject` first. So
   `object_update` answers whether the object's own update runs, and
   `object_after_update` runs after it. The damage and destruction paths
   inside game-sim get server seams too, passed the same way as the step's
   hooks (`World::step_with` and the public `_hooked` entry points):
   - `damage_state` gives the damage computation the object's
     invincibility for the list's inflictor, the difficulty scale and the
     death fuse;
   - `before_damage_list`, `after_damage_list` (which may cancel the
     destruction) and `damage_list_done` wrap `apply_damage_list`;
   - `destroy(kind, killer)` runs the server's `destroy` before the world
     marks the object dead, and may keep it.

3. **A `StarSystem` in `game-logic::world` holds what the world does
   not.** `ServerWorld { systems: BTreeMap<u32, StarSystem> }` in `Logic`
   has one entry per Universe system, made at startup as `SysDB_Initialize`
   makes them. A `StarSystem` holds:
   - the observers, one per player, in arrival order;
   - the dock queue;
   - the loaded flag.

   The object lists, the solar BSP and the asteroid fields stay in
   game-sim's per-system state. Record 0006 point 7 already turns those
   lists into `MetaList` walks.

4. **Lookup is the world's id map.** `Server_FindObject` becomes
   `World::find`. The C++ search order shows only when two live objects
   share an id: a static solar hash in two systems, or an in-transit
   object whose id is also in use. There the C++ answers the first in its
   search order and the port answers the id map's single entry. That
   difference is a divergence row. The cache and its hit counters are
   statistics, not ported.

5. **In-transit objects stay in the arena with no system.** `Relocate` to
   system 0 removes the object's body and keeps the physics parameters the
   C++ saves (`PhySys::CreateParms`) in the object. The object is in no
   system's lists and is found by id. Arriving in a system rebuilds the
   body from the saved parameters, as `arrive_ship` does. game-sim gains
   the transfer operations (`leave_system`, `enter_system` and the
   transit state); game-logic gains the server's `remove_ship`, `add_ship`
   and `arrive_ship` around them.

6. **Systems load, step and unload as in the C++.**
   - **Loading:** lazily, when a system's first player arrives, in both
     modes. `pub::System::LoadSystem` is `add_player(0)`.
   - **Stepping:** each tick steps the distinct systems of the active
     players, in session order, with the multiplayer 0.2 s cap and the
     single-player single step (record 0002 point 6).
   - **Unloading:** single player unloads a system when the player leaves
     or lands; multiplayer never does. game-sim gains `unload_system`.

7. **Replication is a game-logic module over the observers.**
   `game-logic::replication` holds the `Observer` of point 3:
   - the tracked objects, each with a
     `neuron_core::physics::RemotePhysicsSimulation` and its created flag;
   - the pending damage ids;
   - the observer's clock.

   The visibility rule, the five phases and the per-object fan-outs are
   ported in the C++ order. Single player keeps its shortcut: every create
   at once, no per-frame pass. The link-saturation gate reads the
   transport's backlog. Until P3.8 brings the network transport, the
   loopback reports 0 and no phase stops early.

8. **Single player finds instead of creating.** Where the C++ server
   wraps an object the client built (the player's ship, a system's
   solars, the asteroid fields), the port looks the object up in the
   shared world and adds its side-table entry. The multiplayer-only paths
   (reading the system file, server physics and collisions, observers)
   run only on a multiplayer server, as the C++'s `SinglePlayer()` and
   `IsMPServer()` branches decide.

9. **The `pub::` functions become functions on the context, grouped by
   namespace.** `game-logic::api::{space_obj, system, player, gf, market,
   reputation, zone, ai, phantom, audio}` hold free functions that take
   `&mut Cx`, as P3.2's player functions do. `pub::Market` already lives
   in `game-logic::bases`. Outputs are return values. A C++ result code
   becomes `Result<T, PubError>`, where `PubError` carries the C++ code
   (-1 to -4 and the others), because Content tests the codes. Functions
   that only read Common data (Universe, archetypes, reputation getters)
   become accessors on the world or its data and keep no wrapper.

10. **Nesting.** The nested `StarSystem_Update` of `ForceLand` → `Dock`
    (record 0006 point 7) is a nested call of the same step on `&mut Cx`.
    No borrow of a system or a side-table entry is held across a hook,
    a dispatch or a nested step; each access looks the entry up again.

11. **Verification.**
    - **System load:** a data dump of each system's objects after
      `StarSystem::load` (multiplayer path) from `Server.dll` against the
      port's (Tier A).
    - **Replication:** the oracle's `SimClientStub` records the packets
      each client receives per tick in a scripted multiplayer scenario;
      the port's are compared within the scenario envelopes of decision
      O20 (Tier B).
    - **Docking, tradelanes and transfer:** Gate G3's `dock_at_station`,
      `trade_lane` and `dock_trade_lane` scenarios through the Rust world.
    - **Invariants:** every object has exactly one side-table entry while
      the server knows it; no object is in two systems' lists; an
      in-transit object has no body.

## Consequences

- game-sim gains transfer, transit and unload operations and the
  `object_update` hook point, and nothing server-specific; the client
  side is unchanged.
- game-logic's P3.3 modules are `world` (systems, loading, stepping,
  transfer, dock queue), `objects` (side tables, fuses, loot, munition
  controllers, docking), `replication` (observers) and `bases` (the base
  database), with `api` for `pub::`.
- Every structure the C++ reaches by pointer is reached by handle or id
  and looked up again after a call that can dispatch, as record 0006
  requires.
- The divergence rows this produces are point 4's duplicate ids and the
  walks of record 0006 point 7. Each comes with the package that ports
  its site.

## Options rejected

- **Server state as game-sim components.** This would put server-only
  state in the crate the client uses and against record 0001 point 5. It
  gains nothing over a side table, because single player shares the
  object either way.
- **One game-sim world per star system.** This would break record 0002's
  shared single-player world and the global id map, and need a copy for
  every transfer.
- **Reproducing `Server_FindObject`'s search order** with a per-system
  search. It costs a scan per lookup to reproduce a difference that shows
  only with duplicate ids.
- **Replication at a fixed rate or by events.** The C++ sends once per
  system frame, gated by saturation. Another schedule changes what
  clients see and is not comparable with the oracle.
- **`pub::` as methods on `World`.** Most of these functions need the
  player database, the bases or the bus as well as the world, so they
  belong on the context.
