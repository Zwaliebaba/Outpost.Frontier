# 0002 — Single player keeps one shared world

Status: proposed, 2026-11-17 (P2.0). Direction chosen by the owner on
2026-11-17 (decision O21); answers fact F14. The record waits for the
owner's approval.

## Context

The plan's default for P2.0 was "two worlds joined by typed loopback
messages, as the C++ `SP*` calls suggest". Reading the C++ shows otherwise
(paths relative to `CppSource/`):

- **One object per id.** Client and server share `Common.dll` in one
  process and its per-class object lists
  (`Common/common_cobject.cpp:433-455`). The server creates every object
  except the player's ship (`Server/server_gameobject.cpp:865`, `:6328`,
  `server_loot.cpp:35`, …). In single player the client does not create its
  own: it wraps the server's object found by id
  (`OutpostFrontier/Client/client_iclient_space.cpp:445`, `:645`, `:738`,
  `:820`, `:921`). The client creates the player's ship
  (`player_spawn.cpp`), and the server finds it
  (`Server/server_classes.cpp:7173-7180`).
- **One physics world.** Only the client steps physics in single player
  (`OutpostFrontier/Client/space_state.cpp:1054`); the server's beam,
  physics and collision pass is skipped (`Server/IServerImpl.cpp:4007-4033`).
  Single player steps once per frame with the step clamped to 0.0999 s;
  multiplayer accumulates and steps in 0.0999 s chunks
  (`Common/common_physicalsystem.cpp:354-388`).
- **Split of work.** The server creates objects, runs NPC AI on the shared
  objects (`Server/server_gameobject.cpp:5284-5297`), applies damage, runs
  missions and writes saves from the live objects
  (`Server/server_pub.cpp:1580-1616`). The client integrates physics,
  detects every collision (only its own in multiplayer,
  `client_gameobj.cpp:588-596`), moves beams and updates the player's ship.
  Each side skips in single player what the other already applies to the
  shared object (hit points, fire resources, cargo, solar and NPC updates):
  `SinglePlayer()` is tested 129 times in the client, 121 in Server, 62 in
  Content, 25 in rpclocal and 17 in Common.
- **Messages still flow.** `local_server = rpclocal.dll`
  (`outdir/OutpostFrontier.ini:7`) turns every call into a typed packet by
  id, but single-player payloads leave out fields the receiver can read
  from the shared object (`rpclocal/rpc_payload_ch2.cpp:2646-2652`).
- **The `SP*` calls are not single-player calls.** They are `IServer`
  slots (`include/server_iserver.h:219-259`) used in multiplayer too; "SP"
  appears to mean space. `SPObjUpdate` is used only in multiplayer.

## Decision

1. In single player the executable owns **one `game-sim` world**. The
   server side (`game-logic`) and the client side (`game-app`) each run
   their systems over it, in the C++ order, within one frame: the server's
   update when the local server ticks (`client_frame_tick.cpp:233`), the
   client's space update (object updates, physics, beams, collisions) in
   the client's frame.
2. **Ownership of work follows the C++ split**: object creation (except the
   player's ship), NPC AI, damage, missions and saves on the server side;
   physics, collision detection, beams and the player's ship on the client
   side. Each side's single-player skips are ported as they are.
3. **The message catalog still carries every call** between the two sides,
   by id, over the transport (`neuron-core::net` loopback), so the
   multiplayer and single-player code paths stay the same and the
   single-player payload omissions are kept.
4. **Crate graph unchanged.** `game-app` and `game-logic` both depend on
   `game-sim`; neither depends on the other. `apps/outpost-frontier`
   creates the world and the loopback transport and passes the world to
   both by reference, one after the other. The server side never holds a
   reference to client state.
5. **Multiplayer** keeps one world per process, as the C++ does: the
   dedicated server's world and each client's world, joined only by the
   transport with remote correction and dead reckoning.
6. Physics stepping keeps both C++ modes: the single clamped step in single
   player and the 0.0999 s accumulator in multiplayer.

## Consequences

- Single-player behaviour matches the C++ without remote correction, delay
  or a second physics world; saves read live positions as before.
- The world must be shareable between the two sides in turn, which is
  ordinary `&mut World` borrowing in the executable's frame loop; no
  locking is needed because the C++ runs both sides on one thread.
- Fact F14 is answered from the code; the scenario harness (record 0003)
  can confirm it at run time.
- The plan's P2.0 default and its reading of the `SP*` calls are corrected.

## Options rejected

- **Two worlds joined by loopback** (single player as a local multiplayer
  session). Simpler model, but a player would see remote correction on
  NPCs, damage and cargo effects a frame late, multiplayer stepping and
  saves of the last reported position. Each would be a divergence.
