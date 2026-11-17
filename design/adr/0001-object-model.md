# 0001 — Object model: arena, handles and kinds

Status: accepted by the owner on 2026-11-17 (P2.0); decision O22.

## Context

The C++ object model is `Common`'s class tree, used by the client, the
server and the AI. Paths are relative to `CppSource/`.

- **Kinds.** `EngineObject` (0x4c) → `CObject` (0x9c) → `CSimple` (0xe4) →
  projectiles (`CMunition`, `CGuided`, `CBeam`, `CMine`,
  `CCounterMeasure`), `CLoot`, `CAsteroid`, and `CEqObj` (0x1a4) → `CShip`
  (0x2dc) and `CSolar` (0x1ec); beside them `CEquipmentObj` and
  `CDynamicAsteroid` (`include/common_cobject.h`). `CShip` also derives
  from `PhySys::Controller` (multiple inheritance at +0x1a4).
- **Type tests.** "Is-a" is a bitmask test on `CObject::ObjectClass`
  (`(class & mask) == mask`): about 359 sites (Client 152, Server 114,
  Common 87), mostly ship 0x503 (≈146), equipped object 0x103 (≈105) and
  solar 0x303 (≈60).
- **Identity.** Static ids are `CreateID` hashes; dynamic ids come from
  `AllocateObjectID`, which only the server calls (12 sites). Common keeps
  one global list per concrete class (`G_ClassLists[11]`,
  `Common/common_cobject.cpp:44`) with linear `Find`. There is no object
  database class: `IObjDB` is a 4-slot interface
  (`include/common_cobjdb.h:28-57`) implemented separately by the client
  (`FIObjDB`, ten lists and a BSP) and the server (`StarSystem`, ten lists
  and a deferred-delete queue, `include/server_sysdb.h:749`).
- **Ownership.** Objects come from size pools with a reference count
  starting at 1 (`common_cobject.cpp:268-327`). Wrappers own the reference:
  the server's `GameObject` family (`include/server_gameobject.h:251`) and
  the client's `FClientGameobj` family (`OutpostFrontier/Client/client_gameobj.h:171`).
  `CEqObj` owns its behaviour manager and equipment (`common_ceqobj.cpp:93-115`).
- **References.** One watcher type, `BaseWatcher`, is cleared when its
  target dies (`Common/common_watchable.cpp:33`); about 55 watcher fields
  exist (AI and Common about 39, client 13, server 3): formation leader and
  followers, targets, missile targets, gun locks, dock queues. Other links
  are raw pointers that are not cleared (object database, zone, asteroid
  field, equipment owner and notify targets, controllers, debris), and a few
  are ids looked up on use (projectile and loot owner, docking base).
- **Equipment.** `CEquipManager` holds internal and external lists of
  `CEquip`, each keyed by a 16-bit sub-object id from the loadout; external
  equipment attaches to a hardpoint, physically (a `CEquipmentObj` and an
  engine joint) or not (`Common/common_cattachedequip.cpp:135`).
- **Engine coupling.** Objects live on the engine instance tree on both
  sides; the dedicated server starts the engine too
  (`OutpostServer/OutpostServer.cpp:523`). The simulation needs transforms,
  the instance-to-object map, hierarchy walks, hardpoint transforms,
  extents, part maps and animation durations (bay, dock, gate), and no
  rendering.
- **Update order.** Object lists iterate in insertion order. Three places
  iterate by pointer value: the physics sync set
  (`std::set<CObject*>`, `Common/common_physys_internal.h:85`), intruder
  sets, and the attached-damage set. AI runs from `CEqObj::update` through
  `UpdateBehaviorTick` (`common_ceqobj.cpp:684-720`).
- **Inspection.** `IObjInspect` (85 slots) and `IObjAffect` (30 slots) are
  how the AI, client and server read and command objects
  (`include/common_iface.h`). Content never touches objects; it uses ids
  through 53 `pub::SpaceObj` functions.

## Decision

1. **World arena.** `game-sim` owns a `World` value per process side (one
   in single player, record 0002), holding every loaded star system. Objects live in an arena the crate implements itself, with
   generational handles (`ObjHandle { index: u32, generation: u32 }`). A
   handle to a destroyed object resolves to `None`. No new dependency.
2. **Ids beside handles.** The 32-bit object id (static hash or dynamic
   counter, bit-exact as in `neuron-core::id`) is kept for the wire, saves
   and Content-style lookups, with an id → handle map per world. Internal
   links use handles.
3. **Kinds as an enum, data as components.** `ObjectKind` is an enum
   (`Ship`, `Solar`, `Loot`, `Munition`, `Guided`, `Beam`, `Mine`,
   `CounterMeasure`, `Asteroid`, `DynamicAsteroid`, `EquipmentObject`).
   Shared state is grouped into plain structs the kinds share: `Simple`
   (id, owner player, hit points, zone), `Physical` (body handle, parts,
   control mode), `Equipped` (equipment manager, power, docking, behaviour
   manager), and per-kind data (`ShipData`, `SolarData` and so on). The
   C++ mask tests become `match` arms or accessor methods
   (`as_equipped()`); the mask values themselves are kept as constants where
   data or the wire carries them.
4. **References.** Every C++ watcher becomes an `Option<ObjHandle>` that
   reads as gone once the target is destroyed, matching the clearing
   semantics. Raw object pointers become handles too; links to non-object
   owners (asteroid field, object database) become indices into the world.
   Id links stay ids.
5. **Lifetime.** Destruction is deferred to the end of a tick, as the
   server's `Deferred` list does: an object marked dead stays readable until
   the reap, then its slot's generation increments. Wrapper layers do not
   exist; client presentation and server bookkeeping keep their own side
   tables keyed by handle (decision record 0002).
6. **Equipment** is a component of equipped objects: two ordered lists
   (internal, external) of `Equip` values keyed by `SubObjId(u16)`, with
   attachment data for external items. Equipment objects that need a body
   (`CEquipmentObj`) are world objects linked by handle.
7. **Engine coupling** goes through a headless instance tree provided by
   `neuron-core::scene` (P1.8), owned by the world: transforms,
   hierarchy, hardpoints, extents, part maps and animation durations. No
   renderer types appear in `game-sim`.
8. **Inspection** becomes typed read and command APIs on the world
   (`world.inspect(h)`, `world.command(h, Command::SetThrottle(..))`)
   rather than an 115-slot vtable. Commands from the AI go through the same
   API, so a replay test can record them (record 0003).
9. **Order.** Ticks iterate kinds and objects in insertion order, like the
   C++ lists. Where the C++ iterates by pointer (physics sync, intruder sets,
   attached damage), the port iterates by insertion order and records it as
   a divergence (no defined order exists to copy; decision O20 bounds the
   effect).

## Consequences

- Downcasts are checked by the compiler; the 359 mask sites port as
  matches.
- Use-after-free of the C++ raw links becomes a `None` the code must
  handle; each such site that the C++ leaves undefined gets a divergence
  row when ported.
- Handles are `Copy`, so AI and equipment code can hold many without
  borrowing the world; access goes through the world, which serialises
  mutation and keeps the tick order explicit.
- The world is the unit a test or the scenario harness creates, steps and
  inspects.

## Options rejected

- **`slotmap` crate.** The same design on an existing generational key
  type; rejected to avoid a dependency for about 150 lines of arena code.
- **An ECS library (`hecs`, `bevy_ecs`).** Flexible queries, but the C++
  has a fixed set of kinds and a strict update order that an ECS scheduler
  would obscure, and a large dependency.
- **`Rc<RefCell<…>>` objects.** Mirrors the C++ ownership but brings
  runtime borrow failures and cycles through watchers.
