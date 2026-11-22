# 04 — Travel, docking and navigation

Status: draft
Depends on: 00 (glossary), 01 (solars, lanes, jump points, system paths),
02 (flight model, cruise, force controllers), 11 (authority, messages).
Refers forward to 05, 06, 08, 09 and 10.

## 1. Purpose and player experience

A system is far too large to cross at combat speed (chapter 01), and the
places that matter are indoors. Travel is the part of the game that joins
the two: it gets a ship from open space into a base, out again, across a
system and into the next one, with as little hand flying as the player
wants.

The player has four layers of help.

- **Cruise** (chapter 02) is the plain fast mode: five seconds to engage,
  300 m/s, no weapons.
- **Manoeuvres** are orders to the ship's autopilot: fly to the selected
  object, dock with it, join its formation, or return to free flight. The
  autopilot is the same pilot layer that flies NPCs.
- **Lanes** are the motorways. The player orders a dock with a lane ring
  and is carried from ring to ring at 2,500 m/s, more than eight times
  cruise speed, until the lane ends, the player leaves, or somebody shoots
  a ring out.
- **Routes** are planned by the game. The player picks a destination on
  the map and receives a chain of waypoints through lanes and jump points,
  across systems if need be.

Docking is a small ceremony, not a teleport. The player asks, the station
answers by voice (cleared, wait, refused), the ship flies an authored
approach into a bay, a ring or a jump point, the camera cuts outside, and
the screen changes to the base or to the next system. Launching plays the
same thing backwards. The ceremony is where the world gets to say no: a
hostile station, a closed border, a mission that forbids leaving, a full
landing pad.

## 2. Concepts

Glossary terms (solar, base, lane, lane ring, jump point, gate, hole,
connection, system path, authority, presentation, content controller,
force controller) are used as defined. This chapter adds:

| Concept | Definition |
| --- | --- |
| Manoeuvre | An order the player gives the ship's pilot layer: free flight, go to, dock, join formation. One is active at a time. |
| Docking point | One authored place on a solar (or capital ship) where a ship can dock: a kind, a mount pose, two approach poses, a radius and an optional door animation. |
| Docking point kind | Berth (interior bay), mooring in three sizes (the ship stays outside), ring (the two pads of a planetary docking ring), jump (the mouth of a jump point), airlock. |
| Docking class | A label on a ship archetype saying which kinds of docking point it may use: berths, or one size of mooring. |
| Dock request | The question "may this ship dock at that object", put to the authority, with five possible answers. |
| Docking list, waiting list | The authority's two lists: ships cleared and flying in, and ships told to wait for a free docking point. |
| Scripted mover | A force controller that takes a ship over completely: collisions off, pilot and input suspended, pose set each tick. Lane travel, launch and jump arrival are scripted movers. |
| Control exclusion | A small mask on a ship that suspends the pilot layer, the player's steering and the free camera while a scripted mover or sequence owns the ship. |
| Lock | A marker on a jump point that closes it: for one player, for every player, or for ambient NPC traffic. |
| Route | The planner's answer: an ordered list of legs. |
| Leg | Either "ride this lane from ring X to ring Y" or "fly free to this point". |
| Waypoint | A position in a named system, optionally tied to an object, held by the presentation. Up to 99. |

```text
player ── orders ──▶ MANOEUVRE (pilot layer on the player's ship)
                        │ go to: steer, throttle, cruise
                        │ dock ─── dock request ──▶ AUTHORITY: space support
                        │            ◀─ answer ──     permission ▸ docking point ▸ queue
                        ▼
solar ── has ──▶ DOCKING POINT ×n ── kind ──┬─ berth, ring ──▶ land: enter the base (10)
ship archetype ── docking class ── fits? ───┤─ mooring ──────▶ moored outside
                                            ├─ jump ─────────▶ transfer to the paired jump point
lane ring ── next / previous ring ──────────┴─ (lane) ───────▶ scripted mover along the chain

map ── route request ──▶ AUTHORITY: planner ── legs ──▶ WAYPOINTS ──▶ HUD marker (09)
           uses lanes of each system (01), the gates-only system path (01), locks
```

## 3. How it works

### 3.1 The manoeuvres a player can order

The interface offers four manoeuvre buttons and a cruise switch (chapter
09). Each button is enabled when the pilot layer says the manoeuvre could
start now.

| Manoeuvre | Needs | What it does |
| --- | --- | --- |
| Free flight | — | Manual control (chapter 02). Selecting it cancels any other manoeuvre. |
| Go to | Optional target | With a target: fly to within a stand-off range of it and stop. Without: fly straight ahead (the destination is a point 100,000 ahead, arrival range 500). |
| Dock | A dockable target | The sequence of 3.2 to 3.4. On a lane ring it means "enter the lane"; on a jump point, "jump". |
| Join formation | A ship as target | Asks the authority for a slot in that ship's formation (chapter 05). |

The go-to stand-off range is a multiple of the target's radius, by class:
5 for fighters and freighters, 2 for gunboats and transports, 1 for larger
ships and for most solars, 0.5 for stars and planets; never less than 100;
750 for a lane ring; 100 for a waypoint marker. Go to refuses, for a
player, a target moving faster than 5 m/s. It uses cruise only if the
player has cruise switched on, the remaining distance exceeded 1,750 when
the manoeuvre began and the way ahead is clear. Arriving ends the
manoeuvre and returns the ship to free flight.

Dock is available when the target is a station, docking ring, lane ring,
jump point or capital ship with docking points; or a planet that a docking
ring serves, in which case the ring is silently substituted. The target
must be functioning and standing still.

Manoeuvres of the player's ship run on the presentation, which owns that
ship's flight (chapter 02). Whatever needs a decision about the world is a
request to the authority.

### 3.2 The dock request

When the dock manoeuvre has brought the ship near, it sends a dock
request. A player whose target is more than 10,000 away is told "too far"
and the manoeuvre ends; nothing is sent.

Space support, a content controller of the authority, answers. The checks
run in this order and the first failure decides.

| # | Check | Answer on failure |
| --- | --- | --- |
| 1 | The player is allowed to dock here. A running mission can forbid all docking with listed exceptions, or allow all with listed exceptions (3.10). | Denied |
| 2 | The target is not a jump point locked for this ship (3.10). | Denied |
| 3 | The target's owner faction does not regard the ship as hostile: its attitude towards the ship must be above −0.6 on the scale of chapter 07. Jump points and airlocks skip this check: borders stay open to enemies. | Denied, hostile |
| 4 | The target has docking points at all. | No dock |
| 5 | At least one docking point's kind fits the ship's docking class. Jump and airlock points fit every ship. | Cannot dock |
| 6 | One of the fitting points is not held by another ship in the docking list. | Wait |
| 7 | The target is at rest (linear and angular speed at most 0.01). | Wait |
| — | All passed | Proceed, with the chosen docking point |

Among free fitting points the one nearest the ship is chosen. A planetary
docking ring starts with its first pad.

The five answers and what follows:

| Answer | Authority | Ship |
| --- | --- | --- |
| Proceed | Adds the ship to the docking list with the chosen point and the time; switches the ship's docking lights on; queues the ship at that point of the target. | Flies the approach (3.3). |
| Wait | Adds the ship to the waiting list. Players, ships in a formation with a player and two classes of ambient NPC go ahead of other NPCs but behind players already waiting. | Holds position until a later "proceed". |
| Denied (hostile, locked or forbidden) | — | Manoeuvre fails; the player hears why. |
| Cannot dock, no dock | — | Manoeuvre fails. |

Every answer is spoken: the target is the speaker, the ship the listener
(chapter 09 for the voice system). The player's own ship computer repeats
the outcome.

**The queue.** Each tick of space support walks the docking list. A ship
within reach of its docking point marks that point occupied, which opens
its doors; the doors close when no cleared ship is near. A ship that has
not arrived within 90 s is thrown out: an NPC leader is told to cancel and
dock again, a vanished ship is simply removed. When a docking finishes,
its point is freed and the waiting list is walked from the front, each
waiter being put through the whole check again; so a waiter can now be
cleared, refused or left waiting.

**Formations.** When a leader is cleared at a jump point, every follower
is cleared for the same point at once and goes through with it. At a
base the formation dissolves instead: NPC followers copy the leader's
orders and dock on their own, players are released.

### 3.3 The approach

Each docking point supplies three poses in the target's model: the *mount*
(where the ship ends up) and two *approach points* leading to it. The ship
has a mount hardpoint of its own, and the aim is to bring the one onto the
other. The approach is a state machine in the pilot layer; this is its
normal path, simplified.

| State | What the ship does | Leaves when |
| --- | --- | --- |
| Far approach | Go-to manoeuvre towards the target, cruise allowed. | Within range → staging |
| Staging | Flies to within 64 of a staging point: the outer approach point for a base; for a lane ring or jump point a spot about 140 short of the entry, off to the ship's own side and spread randomly by up to 128, so that arrivals do not stack. | Arrived → request |
| Request | Sends the dock request (3.2) or lane request (3.6). | Sent → hold |
| Hold | Stops and waits for the answer. "Wait" keeps it here. | Proceed → align; any refusal → failed |
| Align | Flies to the outer approach point at no more than 18 m/s and takes up its orientation. | Within 10 → inner; straight-in docks skip to final; holes go straight to final |
| Inner | Flies to the inner approach point, same speed cap. Cruise is now locked out. | Within 15 → final |
| Final | Flies the mount hardpoint onto the dock's mount. The player's manoeuvre buttons are locked. | Within 0.6 × the docking point's radius → completing (a mooring: done) |
| Completing | Waits for the authority to confirm. | Confirmed → the ship is gone; 3 s without confirmation (10 s for a player in multiplayer) → start over |
| Done / failed | Ends the manoeuvre with a result that the orderer (player interface, NPC script) reads. | — |

A dock is "straight-in" when the three poses lie on one line (within about
6°) and face along it; the inner point is then redundant.

Guards run throughout. If the ship strays more than 1,050 from the target
during staging or alignment, or overshoots a lane entry, the manoeuvre
resets and starts again. If the target begins to move, the ship pauses.
If the target is destroyed, or the player picks another manoeuvre, the
manoeuvre ends and the authority is sent a cancel, which removes the ship
from both lists.

### 3.4 Completion

Completion is the authority's decision, by one geometric test per queued
ship per tick:

```text
docked  ⇔  | ship mount hardpoint − docking point mount | < docking point radius
```

The radius is authored per docking point: 2 to 10 for berths and
moorings, 40 for ring pads, 150 for holes, 225 for gates. What happens
next depends on the kind.

| Kind | Result |
| --- | --- |
| Berth, ring pad | **Landing.** Every presentation in the system is told the ship landed on that base. An NPC ship is removed. A player's session leaves the system and enters the base (chapters 10, 11); in multiplayer the character is saved; in single player the system is shut down. |
| Mooring | A "docked, not landed" event is raised and the ship stays in space. Used by ships too large for a berth. |
| Jump, airlock | **Transfer** (3.7). A "dock complete" event is raised; other pilots in the system are told the ship left through that object. |

While a player's ship passes through a jump point it cannot be damaged. An
NPC that completed a dock has its collisions switched off and, at a jump
point, lingers 12 s before removal so that the jump effect can play.

The presentation dresses this up: an exterior docking camera that holds
still and tracks the ship, timed to fade to black as the ship reaches the
door; a three-second glide into a planetary ring; a swing onto the jump
point's axis followed by a tunnel sequence. None of it affects the rules.

### 3.5 Launching

Launching reverses landing. The authority decides where the ship appears
(chapter 11 for the message exchange):

1. Leaving a base: the first solar in the system that docks to that base.
   If it is a docking ring, its pad. If it is a station, space support is
   asked for a free berth, so a departing player does not appear in a bay
   another ship is flying into.
2. If the base has no docking solar: a point 2.2 radii out from the base's
   solar, towards the system's centre, facing inwards.
3. A new or relocated character: an authored starting solar, or a stored
   position.

The ship is created invincible and then moved out by one of two means.

- **From a docking ring**, a scripted mover. With `t` the time since
  launch, the ship sits on the pad's axis at a distance behind its exit
  point of

  ```text
  t < 2.5 s:  d = 150 × f² + 80 × (2.5 − t) + 40      with f = (2.5 − t) / 2.5
  t ≥ 2.5 s:  d = 80 × (3.0 − t)
  ```

  It starts 390 back at 200 m/s, eases to 80 m/s and at 3 s is released
  150 in front of the pad with 80 m/s of forward velocity and full
  throttle, so the hand-over to the flight model is seamless. A ring
  whose archetype has no docking camera skips the glide and places the
  ship at the release point at once. In multiplayer a player's ship is released with a random heading within
  ±22.5° in yaw and pitch, so that players launching together diverge.
- **From a station or capital ship**, a launch manoeuvre of the pilot
  layer flies the ship out through the berth's approach points in reverse
  order while the player's controls are excluded.

In both cases the end of the launch lifts the exclusion, ends the
invincibility and raises "launch complete", which missions listen for.

NPC traffic is born the same way (chapter 06): a new ship is placed at a
free docking point of a base and given a launch manoeuvre, or appears out
of a jump point after a two-second delay. Ambient traffic therefore
competes with the player for the same docking points.

### 3.6 Lanes

**Entry.** A lane ring is entered by docking with it. The side from which
the ship approaches the ring decides the direction of travel, and each
direction has its own track on one side of the rings, so opposing traffic
passes without meeting. The request is a lane request naming the entry
ring and the next ring. Space support refuses it only if a mission has
restricted the player's lane use (3.10); otherwise the ring is switched
to its active state and the ship cleared. The manoeuvre refuses to start
at all, with a spoken reason, on a ring whose shield is down.

Once aligned on the track, the ship is handed to a scripted mover. Pilot
layer and player steering are suspended, collisions are off, weapons
refuse to fire.

**The ride.** Speed is `2,500 m/s × r`, where `r` is a ramp value.

| Phase | Ramp | Ends |
| --- | --- | --- |
| Accelerate | Starts at entry speed / 2,500 and rises by 0.125 per second. For the first 3 s the pose is blended from where the ship was onto the track. | `r = 1`: about 7.7 s from 80 m/s → cruise |
| Cruise | `r = 1` | The ring ahead is the last one → decelerate |
| Decelerate | `r = 1 − 0.84 × p`, `p` being progress along the last segment: 2,500 down to 400 m/s | Past the last ring → exit |
| Exit | `r` falls by `(r at exit start − 0.4 × r_end)` per second until it reaches `r_end`, the ramp value at entry | Released |

Each tick the mover projects the ship onto the current ring-to-ring
segment, advances it along the track and, on passing a ring, takes the
next ring from the chain. Followers of a formation ride beside their
leader at their lateral formation offset. On release, collisions and
control return and the throttle is set to full.

A median lane of 52,000 (chapter 01), ridden end to end, takes about
28 s: 8 s and 10,000 of acceleration, 14 s at full speed, 6.5 s across
the last segment. At cruise it is nearly three minutes.

**What interrupts it.**

| Event | Effect |
| --- | --- |
| The player cancels the manoeuvre | Exit phase at once, with a yaw of 0.56 rad/s that swings the ship off the track. From full speed it is released after about a second. |
| The ring ahead is not functioning and the ship is past the middle of the segment | Forced exit with the same swerve; the player is told the lane is disrupted. |
| The last ring is reached | Normal deceleration and exit. |
| In multiplayer, a formation leader has left the lane and the follower has drawn level | The follower exits. |

A ring "functions" while its shield generator is up. A ring's hull cannot
be destroyed, but its shield (1,000 points, regenerating 10 per second in
the data) can be shot down, and a mission action can drain it. This is
how outlaws pull lane traffic into an ambush, and how a mission stops the
player at a chosen spot.

NPCs use lanes through the same dock order, or through a shortcut
manoeuvre that takes a ship standing at a ring from one named ring to
another and releases it at a set speed.

### 3.7 Transfer between systems

Chapter 01 defines connections; chapter 11 gives the messages. The rules:

1. Docking at a jump point passes through 3.2 like any dock, without the
   hostility check. A jump point has one docking point, so it passes one
   ship, or one formation, at a time. Others wait.
2. On completion the ship becomes invulnerable. The presentation plays
   the tunnel, loads the destination system and reports back.
3. The authority checks that the ship belongs to that session and really
   left through that jump point, reads the destination system and the
   paired jump point, takes the ship out of the old system and puts it
   into the new one. A system is loaded when its first player arrives (per
   design notes).
4. The arrival pose is in front of the destination jump point: 350 ahead
   of it, on a circle of radius 500 around its axis at a random angle,
   facing along the axis. Several arrivals do not overlap.
5. Content is told "system entered", the character is saved, and when the
   presentation reports the arrival finished the invulnerability ends and
   "jump-in complete" is raised.

An NPC arriving through a jump point is slid in by a scripted mover: over
half a second it decelerates along the axis from 1,000 back to the arrival
point.

### 3.8 Route planning

The authority plans routes on request, as chapter 01 outlines: the
player's map asks, missions ask for their waypoints, NPC scripts ask for
travel orders.

**Inside one system** the planner chains lanes greedily. It is a
heuristic and not a shortest-path search.

1. Find the ring nearest the origin. Its lane is the first candidate
   route, taken towards whichever end lies nearer the destination. Every
   other lane with an end within 11,000 of the point where that lane is
   left also starts a candidate.
2. Score each candidate by summing, over its lanes, the squared distances
   from both ends of the lane to the origin and to the destination. The
   lowest score wins; its last ring becomes the current ring.
3. Stop when the current ring is the lane end nearest the destination or
   lies within 11,000 of it, or when no candidates remain. Otherwise
   extend the winning route by every unused lane that has an end within
   11,000 of the current ring, and score again. A lane is used once.
4. The result is the winning chain plus a free leg to the destination.
   Its cost is travel time: free flight at 300 m/s (origin to the first
   ring, and the final leg), lanes at 2,500 m/s. Hops between consecutive
   lanes are not counted.
5. If that cost exceeds the direct distance divided by 300, the lanes are
   discarded and the route is one free leg. If the destination is nearer
   than the request's minimum range (500 for a map request), there is no
   route at all.
6. The first lane is joined at the nearest ring that lies between the
   origin and the lane's far end, not at its end ring.

Example: origin and destination 60,000 apart, one lane of 45,000 whose
near end is 4,000 from the origin and whose far end is 12,000 from the
destination. Direct: 60,000 / 300 = 200 s. By lane: 4,000 / 300 +
45,000 / 2,500 + 12,000 / 300 = 13 + 18 + 40 = 71 s. The lane route is
returned as two legs.

The 11,000 figure is the designer's promise that lane ends meant to
connect sit within that distance of each other, typically around a
station or planet.

**Across systems** the planner takes the gates-only system path (chapter
01) and plans in each system on it, from the origin or the jump point
just arrived through to the next jump point or the destination. The legs
of all systems are concatenated into at most 99 waypoints, each tagged
with its system and, where it is one, the ring or jump point to use.

Locks are checked along the way: if a gate on the path is locked for the
requesting player, or for everyone, the whole request fails and no
waypoints are returned. The planner does not look for a detour, because
the path tables are precomputed without regard to per-player locks. Holes
are never routed through, so a route never reveals one.

### 3.9 Waypoints

The presentation keeps the player's waypoint list. The map has two tools:
*set waypoint* appends the clicked place to the list, and *best path*
replaces the list with a planned route to it. If the planner reports
failure the map says that no path exists.

In flight the first waypoint in the current system becomes a targetable
marker (chapter 09). Coming within 150 of it counts as reaching it: it and
every waypoint before it are removed and the next one becomes the marker.
With no live marker, passing within 300 of any stored waypoint removes it
and its predecessors, so riding a lane clears the waypoints it skips.

A mission's objectives arrive in the same form and take precedence: when a
mission sets objectives with positions, the player's own waypoints are
dropped, and while such a route is shown the map's waypoint tool is
disabled (chapter 08).

### 3.10 Locks and restrictions

Travel is where the campaign fences the player in. Five switches exist,
all held by the authority and all stored in the saved game (chapter 11).

| Switch | Scope | Set by |
| --- | --- | --- |
| Jump point locked for everyone | All players | The initial world state (27 jump points at the start); mission actions |
| Jump point locked for one player | That player | Mission actions |
| Jump point closed to ambient NPCs | Population traffic only; mission-spawned ships pass | The initial world state (36); mission actions |
| Player may dock: yes or no, plus a list of exceptions | One player | Mission actions (about 120 uses in the campaign) |
| Player may use lanes: yes or no, plus exception stretches given as ring pairs | One player | Mission actions (about 50 uses) |

A mission action that unlocks a jump point "for the player" removes both
the per-player and the everyone lock. In single player, removing a
player's lock also removes the everyone lock. The campaign opens the
universe region by region with these actions.

A formation led through a locked gate sheds any player for whom the gate
is locked.

Missions also bypass the ceremony when the story needs it: they can force
a landing on a base, dock a ship instantly, relocate a ship, make an NPC
jump in, and give NPCs go-to orders flagged "use cruise" or "never
cruise". The authority's service call for switching on a ship's equipment
has a variant that engages cruise at once, skipping the charge; no caller
of it was found in the content code read.

### 3.11 Authority and presentation

| Authority | Presentation |
| --- | --- |
| Dock permission, choice of docking point, both lists, timeouts | The player's manoeuvres: go to, the docking approach, the lane ride of the own ship |
| The completion test; landing; removal from the system | Docking camera, door and ring animation, lane and jump effects |
| Launch position and pose; launch invincibility | Launch sequence of the own ship; "launch complete" report |
| Transfer between systems; arrival pose | Tunnel sequence; loading; "jump-in complete" report |
| Locks, docking and lane restrictions | Enabling of manoeuvre buttons; spoken feedback |
| Route planning | Waypoint list, map tools, the waypoint marker and its advance |
| All of this for NPCs | Extrapolated NPCs; told when a ship enters or leaves a lane |

For an NPC the request and its answer happen inside one tick. For a
player they are two messages, and the approach must tolerate the delay,
which is why the manoeuvre has a hold state.

## 4. Data that defines it

| Record | Principal fields | Values |
| --- | --- | --- |
| Docking point, on a solar archetype (repeatable) | kind; mount hardpoint; radius; optional door animation | 262 points on 55 archetypes. Stations carry 2 to 18. Radius: berth 2 to 10, mooring 2 to 10, ring pad 40, hole 150, gate 225. |
| Solar archetype, other docking fields | docking camera yes or no; exit hardpoint of a jump point; opening animation | Docking camera on 42 archetypes |
| Model hardpoints | per docking point: one mount and two approach points sharing a letter; on lane rings one hardpoint per direction of travel; on every ship one mount hardpoint | — |
| Ship archetype | docking class | Of 69 hulls: berths 42 (fighters and freighters), medium mooring 8 (mostly transports), large mooring 15 (gunboats and larger), none 4 |
| Lane equipment, mounted on every lane ring | effects for entering, travelling, leaving and being disrupted, separately for the player; delays before each (2 s, 0.2 s, 0.3 s); ring spin rate and acceleration; two activation distances (750, 500) | One record |
| Lane ring loadout | lane equipment, shield generator, unlimited power, two turrets, lights | Shield 1,000, regeneration 10 per second |
| Solar instance (chapter 01) | base it docks to; destination system and jump point; previous and next ring | — |
| Initial world state | list of jump points locked for everyone and for NPC traffic | 27 and 36 |
| Mission actions (chapter 08) | lock or unlock a jump point for the player or for everyone; player may dock, with exceptions; player may use lanes, with exceptions; disable a lane ring; force a landing; relocate a ship; route for the player | — |
| System path tables (chapter 01) | per pair of systems, the systems to pass | Three tables; routes use the gates-only one |

Rule constants are in the code, not in data: lane speed 2,500 m/s, lane
exit speed 400 m/s, ramp rate 0.125 per second, lane-linking distance
11,000, route cap 99, dock timeout 90 s, hostility threshold −0.6,
maximum dock-order distance 10,000, approach speed cap 18 m/s, waypoint
radius 150, launch timings 2.5 s and 0.5 s at 80 m/s.

Three anonymised docking point lists:

| Archetype | Docking points |
| --- | --- |
| `example_docking_ring` | ring pad A (40, door animation), ring pad B (40, door animation), 2 medium moorings (5) |
| `example_small_station` | 3 medium moorings (5), 1 large mooring (5) |
| `example_gate` | 1 jump (225) |

## 5. Connections

Needs:

- 01: solars and their archetypes, lane ring chains, connections, path
  tables, the knowledge state the map draws from.
- 02: the flight model, cruise, force controllers, the rule that the
  owner of a ship integrates it.
- 05: the pilot layer in which go to, dock, launch and formation are
  behaviours; formations.
- 07: the attitude of a base's owner towards a ship.
- 11: request and answer messages, the event bus, system loading, saving.

Gives:

- 05, 06: the dock, launch and lane orders that NPC scripts and ambient
  traffic are built from; docking points as spawn and despawn places; the
  NPC lock on jump points.
- 08: locks and restrictions as mission actions; "launch complete",
  "dock complete", "system entered", "jump-in complete", lane enter and
  exit as trigger events; the planner for mission waypoints.
- 09: manoeuvre availability, dock answers and travel notices to voice
  and HUD; waypoints for the map and the marker.
- 10: the moment of entering and leaving a base.
- 11: the autosave points at landing and system arrival.

## 6. Design notes for the builder

**Essential.**

- *Docking as a request the world can refuse.* Reputation, campaign
  gating and mission pressure all act through one checkpoint. Without it
  each would need its own barrier.
- *Locks on jump points.* The whole pacing of the campaign rests on a list
  of closed doors that missions open.
- *A fast lane that can be broken.* Lanes make a large map tolerable and
  concentrate traffic, and their vulnerability turns them into the game's
  main ambush site. A lane that could not be disrupted would be a loading
  screen.
- *Route planning on the authority, with locks applied.* The map must not
  offer a route the player cannot fly.

**Feel.**

- The authored three-pose approach, door animations and docking camera.
  A fade at a trigger radius would follow the same rules.
- Spoken clearance and the waiting queue. The queue matters only where
  traffic is dense enough to fill a base's docking points.
- The launch curve that hands over at exactly 80 m/s, the three-second
  blend onto the lane, the random arrival angle.

**Accidents of the era.**

- The player's presentation flies its own docking approach and lane ride
  and the authority trusts the reported pose (chapter 11). A stricter
  authority would run them itself.
- Completion is retried by timeout (3 s or 10 s) because request and
  confirmation are separate messages with no acknowledgement.
- The in-system planner is a greedy heuristic with an odd score. With a
  handful of lanes per system an exact shortest-time search over rings is
  both simpler and better.
- Cross-system routes fail outright on a locked gate, because path tables
  are precomputed for nobody in particular. Computing the system path per
  request, with the requester's locks, removes the failure.

**Simplifications and their cost.**

- No docking classes and no moorings: every ship uses every docking
  point. Lost: large ships visibly parked outside stations.
- No approach flight: docking completes when the ship enters a radius
  after clearance. Lost: the sense of arrival, and the exposure of a slow
  ship near a base under fire.
- Lanes as plain teleports between end rings. Lost: interdiction, traffic
  seen in passing, leaving mid-lane.
- One "locked" flag per jump point instead of three scopes. Lost:
  multiplayer campaigns in which players are at different points.

## 7. Open questions

1. **Medium moorings.** Ship data labels one docking class with a shorter
   spelling than the label the reconstructed code hashes. If both are
   right, those 8 hulls fit no docking point at all. Not resolved.
2. **Moored ships.** What content does with a "docked, not landed" event
   (how long a moored NPC stays, how it leaves) was not traced.
3. **Airlocks.** Two archetypes have an airlock docking point and a
   matching arrival rule (the ship appears at the object, turned about).
   They are treated like jump points by the permission check; their use
   in the game was not established.
4. **Waypoints and lanes.** Whether go to on a waypoint marker that
   stands for a lane ring enters the lane on its own, or the player must
   target the ring and order a dock, was not established.
5. **Instant cruise.** The service variant exists on the authority and in
   the cruise message; its caller was not found.
6. **Initial locks in multiplayer.** Whether the 27 initially locked jump
   points apply on a multiplayer host, where no campaign unlocks them,
   was not checked.
7. **Lane activation distances** (750 and 500) and the ring spin values
   were read in data only; their exact use is presentation code not read.

## 8. Basis

Design documents: `design/adr/0007-server-world.md`,
`design/assessment/04-server-content.md` section 1,
`design/sim-records.md` (dock, launch and lane behaviour records, dock
events), `design/divergences.md` rows D622 to D628, D681 to D686, D703,
D720 to D723 as pointers into the code.

Data: `SOLAR/solararch.ini` (docking_sphere, docking_camera,
jump_out_hp, open_anim; tabulated by script), `SOLAR/loadouts.ini` (lane
ring loadout), `EQUIPMENT/select_equip.ini` (the lane equipment and ring
shield records), `SHIPS/shiparch.ini` and `MISSIONS/mshipprops.ini`
(docking class), `initialworld.ini` (locked gates), mission files (counts
of the travel actions), `constants.ini` (nothing specific to travel).

Code, read in part: `Common/common_dockbehavior.cpp` (approach state
machine), `common_laneaction.cpp` (lane ride), `common_launchaction.cpp`
(ring launch, NPC jump-in), `common_gotobehavior.cpp`,
`common_cruisebehavior.cpp`, `common_iobjinspectimpl.cpp` (docking point
poses, lane start and direction, ring status), `common_cship.cpp`,
`common_archetype.cpp`, `common_archetype_ships.cpp` (object types,
docking point kinds); `Content/DockQueues.cpp` (permission, choice,
lists, timeouts, formations), `TradelaneRequests.cpp`, `SpacePaths.cpp`
(in-system planner), `GameSupport.cpp` (route request), `SpaceSupport.cpp`
(locks), `PlayerState.cpp` (docking and lane restrictions),
`MissionAct.cpp`, `ContentStaticIds.h`; `Server/server_gameobject.cpp`
(completion test, requests, arrival pose, launch invincibility),
`server_classes.cpp` (dock completion, land queue, launch position, lane
notices), `server_pub.cpp` (dock and lane answers), `IServerImpl.cpp`
(land step, system switch); Client `hud_maneuvers.cpp`, `hud_waypoint.cpp`,
`best_path.cpp`, `player_spawn.cpp`, `nn_navmap_input.cpp`,
`client_iclient_space.cpp`, `client_gameobj.cpp`, and the headers of
`space_camera_docking`, `client_land_action`, `jump_effect`;
`Oracle/scenarios/` dock_at_station, dock_trade_lane, trade_lane,
launch_from_station, fly_to_point.

Inference: the approach state table condenses a larger machine and names
its states; the staging offsets are rounded; lane timings (7.7 s, 28 s,
one second to exit) and the launch start values are computed from the
formulas; the purpose of the 11,000 linking distance, of docking-point
competition by traffic and of lane disruption as an ambush tool is
reasoned from the rules. The station launch path "through the approach
points in reverse" was taken from the launch behaviour's record layout and
scenario description, not from a full reading of that behaviour. The
presentation sequences are described from header notes.
