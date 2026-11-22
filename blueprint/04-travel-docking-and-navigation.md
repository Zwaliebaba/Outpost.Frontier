# 04 — Travel, docking and navigation

Status: reviewed
Depends on: 00 (glossary), 01 (solars, lanes, jump points, system paths),
02 (flight model, cruise, force controllers), 11 (authority, messages).
Refers forward to 05, 06, 08, 09 and 10.

## 1. Purpose and player experience

A system is far too large to cross at combat speed (chapter 01), and the
places that matter are indoors. Travel joins the two: it gets a ship from
open space into a base, out again, across a system and into the next one,
with as little hand flying as the player wants.

The player has four layers of help.

- **Cruise** (chapter 02): five seconds to engage, 300 m/s, no weapons.
- **Manoeuvres** are orders to the ship's manoeuvre layer: fly to the
  selected object, dock with it, join its formation, or return to free
  flight. NPCs are flown by the same layer (chapter 05).
- **Lanes** are the motorways. The player orders a dock with a lane ring
  and is carried from ring to ring at 2,500 m/s until the lane ends, the
  player leaves, or somebody shoots a ring out.
- **Routes** are planned by the game. The player picks a destination on
  the map and receives a chain of waypoints through lanes and jump points,
  across systems if need be.

Docking is a small ceremony, not a teleport. The player asks, the station
answers by voice (cleared, wait, refused), the ship flies an authored
approach into a bay, a ring or a jump point, the camera cuts outside, and
the screen changes to the base or to the next system. Launching plays it
backwards. The ceremony is where the world gets to say no: a hostile
station, a closed border, a mission that forbids leaving, a full pad.

## 2. Concepts

Glossary terms (solar, base, lane, lane ring, jump point, gate, hole,
connection, system path, authority, presentation, content controller,
force controller) are used as defined. This chapter adds:

| Concept | Definition |
| --- | --- |
| Manoeuvre | What the player orders from the interface: free flight, go to, dock, join formation. Each is issued as a directive to the manoeuvre layer of the player's ship, where it runs as a behaviour (chapter 05). One is active at a time. |
| Docking point | One authored place on a solar or capital ship where a ship can dock: a kind, a mount pose, two approach poses, a radius and an optional door animation. |
| Docking point kind | Berth (interior bay), mooring (the ship stays outside), ring pad (on a planetary docking ring), jump (the mouth of a jump point), airlock. The code knows three mooring sizes; the data uses medium and large only. |
| Docking class | A label on a ship archetype saying which kind of docking point it may use: berths, medium moorings or large moorings. No ship has the small-mooring class. |
| Docking list, waiting list | The authority's two lists: ships cleared and flying in, and ships told to wait for a free docking point. |
| Scripted mover | A force controller that takes a ship over completely: collisions off, pose set each tick. Lane travel, ring launch and NPC jump arrival are scripted movers. |
| Control exclusion | A mask on a ship that suspends the manoeuvre layer, the player's steering and the free camera while a mover or sequence owns the ship. |
| Lock | A marker that closes a jump point: for one player, for every player, or for ambient NPC traffic. |
| Route, leg | The planner's answer is a route, an ordered list of legs. A leg is "ride this lane from ring X to ring Y" or "fly free to this point". |
| Waypoint | A position in a named system, optionally tied to an object, held by the presentation. Up to 99. |

```text
player ── orders ──▶ MANOEUVRE (directive to the ship's manoeuvre layer)
                        │ go to: steer, throttle, cruise
                        │ dock ─── request ──▶ AUTHORITY: space support
                        │         ◀─ answer ──   permission ▸ docking point ▸ queue
                        ▼
solar ── has ──▶ DOCKING POINT ×n ── kind ──┬─ berth, ring pad ─▶ land: enter the base (10)
ship archetype ── docking class ── fits? ───┤─ mooring ─────────▶ moored outside
                                            ├─ jump ────────────▶ transfer to the paired jump point
lane ring ── next / previous ring ──────────┴─ (lane) ──────────▶ scripted mover along the chain

map ── route request ──▶ AUTHORITY: planner ── legs ──▶ WAYPOINTS ──▶ HUD marker (09)
           uses each system's lanes and the gates-only system path (01), and locks
```

## 3. How it works

### 3.1 The manoeuvres a player can order

The interface offers four manoeuvre buttons and a cruise switch (chapter
09). A button is enabled when the manoeuvre layer says the behaviour
could start now.

| Manoeuvre | Needs | What it does |
| --- | --- | --- |
| Free flight | — | Manual control (chapter 02). Selecting it cancels any other manoeuvre. |
| Go to | Optional target | With a target: fly to within a stand-off range of it. Without: fly straight ahead (to a point 100 km ahead, arrival range 500 m). |
| Dock | A dockable target | Sections 3.2 to 3.4. On a lane ring it means "enter the lane"; on a jump point, "jump". |
| Join formation | A ship as target | Asks the authority for a slot in that ship's formation (chapter 05). |

The go-to stand-off range is a multiple of the target's radius (5 for
fighters and freighters, down to 0.5 for stars and planets), at least
100 m, and 750 m for a lane ring. A player's go to refuses a target
moving faster than 5 m/s. It cruises only if the player has cruise
switched on, the distance exceeded 1,750 m when the manoeuvre began and
the way ahead is clear.

Dock is available when the target is a station, docking ring, lane ring,
jump point or capital ship with docking points, or a planet served by a
docking ring, in which case the ring is substituted. The target must be
functioning and standing still.

Manoeuvres of the player's ship run on the presentation, which owns that
ship's flight (chapter 02). Whatever needs a decision about the world is a
request to the authority.

### 3.2 The dock request

When the dock manoeuvre has brought the ship near, it sends a dock
request. A player whose target is more than 10,000 m away is told "too
far" and nothing is sent.

Space support, a content controller of the authority, answers. The checks
run in this order and the first failure decides.

| # | Check | Answer on failure |
| --- | --- | --- |
| 1 | A running mission has not forbidden this player to dock here (3.10). | Denied |
| 2 | The target is not a jump point locked for this ship (3.10). | Denied |
| 3 | The target's owner faction is not hostile: its attitude towards the ship is above −0.6 (chapter 07). Jump points and airlocks skip this check: borders stay open to enemies. | Denied, hostile |
| 4 | The target has docking points. | No dock |
| 5 | At least one docking point's kind fits the ship's docking class. Jump and airlock points fit every ship. | Cannot dock |
| 6 | One of the fitting points is not held by a ship in the docking list. | Wait |
| 7 | The target is at rest (linear speed at most 0.01 m/s, angular 0.01 rad/s). | Wait |
| — | All passed | Proceed, with a docking point |

Among free fitting points the one nearest the ship is chosen.

| Answer | Authority | Ship |
| --- | --- | --- |
| Proceed | Adds the ship to the docking list with its point and the time; queues it at that point of the target; switches on its docking lights. | Flies the approach (3.3). |
| Wait | Adds the ship to the waiting list. Players, ships in formation with a player and two classes of ambient NPC are placed ahead of other NPCs, behind players already waiting. | Holds until a later "proceed". |
| Denied, cannot dock, no dock | — | The manoeuvre fails. |

Every answer is spoken by the target to the ship (chapter 09).

**The queue.** Space support walks the docking list once per second, on
its heartbeat, and again after each finished docking. A cleared ship near
its docking point marks the point occupied, which opens its doors; they
close when no cleared ship is near. A ship that has not arrived within
90 s is thrown out: an NPC leader is told to cancel and dock again. When
a docking finishes, its point is freed and the waiting list is walked
from the front, each waiter passing through the whole check again.

**Formations.** When a leader is cleared at a jump point, every follower
is cleared for the same point and goes through with it. At a base the
formation dissolves: NPC followers copy the leader's orders and dock on
their own, players are released.

### 3.3 The approach

A docking point supplies three poses in the target's model: the *mount*,
where the ship ends up, and two *approach points* leading to it. The ship
has a mount hardpoint of its own, and the aim is to bring one onto the
other. The approach is a state machine inside the dock behaviour; this is
its normal path, simplified.

| State | What the ship does | Leaves when |
| --- | --- | --- |
| Far approach | Go-to behaviour to within 1,000 m of the target, cruise allowed. | In range → staging |
| Staging | Flies to within 64 m of a staging point. For a lane ring or gate: a spot about 140 m short of the entry, on the ship's own side and spread randomly by up to 128 m so that arrivals do not stack. | Arrived → request |
| Request | Sends the dock request (3.2) or lane request (3.6). | Sent → hold |
| Hold | Stops and waits. "Wait" keeps it here. | Proceed → align; refusal → failed |
| Align | Flies to the outer approach point at no more than 18 m/s and takes up its orientation. | Within 10 m → inner; straight-in docks and holes → final |
| Inner | Flies to the inner approach point, same speed cap. Cruise is locked out from here. | Within 15 m → final |
| Final | Flies its mount onto the dock's mount. The player's manoeuvre buttons are locked. | Within 0.6 × the point's radius → completing; at a mooring → done |
| Completing | Waits for the authority to confirm. | Confirmed → the ship is gone; 3 s without confirmation (10 s for a player in multiplayer) → start over |
| Done, failed | Ends the directive with a result that its issuer (interface, NPC script) reads. | — |

At a base the second and third rows swap: the ship asks as soon as it is
in range, because only the answer tells it which docking point to stage
at, and then stages at that point's outer approach point. A dock is
"straight-in" when the three poses lie on one line (within about 6°) and
face along it.

Guards run throughout. If the ship strays more than 1,050 m from the target
while staging or aligning, or overshoots a lane entry, the manoeuvre
starts again. While the target moves, the ship pauses. If the target is
destroyed or the player picks another manoeuvre, the manoeuvre ends and
the authority is sent a cancel, which removes the ship from both lists.

### 3.4 Completion

Completion is the authority's decision, by one test per queued ship per
tick:

```text
docked  ⇔  | ship's mount hardpoint − docking point's mount | < docking point's radius
```

The radius is authored: 2 to 10 m for berths and moorings, 40 m for ring
pads, 150 m for holes, 225 m for gates.

| Kind | Result |
| --- | --- |
| Berth, ring pad | **Landing.** Every presentation in the system is told the ship landed on that base. An NPC ship is removed. A player's session leaves the system and enters the base (chapters 10, 11); in single player the system is then shut down. |
| Mooring | A "docked, not landed" event is raised and the ship stays in space. |
| Jump, airlock | **Transfer** (3.7). "Dock complete" is raised; other pilots in the system are told the ship left through that object. |

A player's ship cannot be damaged while it passes through a jump point.
On a multiplayer authority an NPC that completed a dock loses its
collisions and, at a jump point, lingers 12 s before removal so that the
jump effect can play.

The presentation dresses this up with a fixed exterior camera that fades
to black as the ship reaches the door, a three-second glide into a
planetary ring, and a tunnel sequence at a jump point. None of it affects
the rules.

### 3.5 Launching

The authority decides where a launching ship appears:

1. Leaving a base: at the first solar in the system that docks to that
   base. On a docking ring, its pad. On a station, space support is asked
   for a free berth, so that a departing player does not appear in a bay
   another ship is flying into.
2. If the base has no such solar: 2.2 radii out from the base's solar,
   towards the system's centre.
3. A new or relocated character: an authored starting solar, or a stored
   position.

The ship is invincible while it is moved out by one of two means.

- **From a docking ring**, a scripted mover. At time `t` after launch the
  ship sits on the pad's axis `d` metres behind its release point:

  ```text
  t < 2.5 s:  d = 150 × f² + 80 × (2.5 − t) + 40      with f = (2.5 − t) / 2.5
  t ≥ 2.5 s:  d = 80 × (3.0 − t)
  ```

  It starts 390 m back at 200 m/s, eases to 80 m/s and at 3 s is released
  150 m in front of the pad with 80 m/s of velocity and full throttle, so
  the hand-over to the flight model is seamless. In multiplayer a
  player's ship is released on a random heading within ±22.5° in yaw and
  pitch, so that players launching together diverge.
- **From a station or capital ship**, a launch directive makes the
  manoeuvre layer fly the ship out of the berth while the player's
  controls are excluded.

The end of the launch lifts the exclusion, ends the invincibility and
raises "launch complete", which missions listen for.

NPC traffic is born the same way (chapter 06), at a free docking point or
out of a jump point, and so competes with the player for docking points.

### 3.6 Lanes

**Entry.** A lane is entered by docking with one of its rings, any ring.
The side from which the ship approaches decides the direction of travel,
and each direction has its own track on one side of the rings, so opposing
traffic does not meet. The lane request names the entry ring and the next
one. Space support refuses it only if a mission has restricted the
player's lane use (3.10); otherwise the ring is switched to its active
state and the ship cleared. On a ring whose shield is down the manoeuvre
refuses to start and says why.

Once aligned on the track the ship is handed to a scripted mover. The
manoeuvre layer and steering are suspended, collisions are off, weapons
refuse to fire.

**The ride.** Speed is `2,500 m/s × r`, with `r` a ramp value.

| Phase | Ramp | Ends |
| --- | --- | --- |
| Accelerate | Starts at entry speed / 2,500, rises by 0.125 per second. For the first 3 s the pose is blended from where the ship was onto the track. | `r = 1` → full speed |
| Full speed | `r = 1` | The ring ahead is the last → decelerate |
| Decelerate | `r = 1 − 0.84 × p`, `p` being progress along the last segment: 2,500 down to 400 m/s | Past the last ring → exit |
| Exit | `r` falls by `(r at exit start − 0.4 × r_end)` per second down to `r_end`, the ramp value at entry | Released |

Each tick the mover projects the ship onto the current ring-to-ring
segment, advances it and, on passing a ring, takes the next ring from the
chain. Formation followers ride beside their leader at their lateral
offset. On release, collisions and control return and the throttle is set
to full.

A median lane of 52 km (chapter 01), ridden end to end from 80 m/s,
takes about 28 s: 8 s and 10 km of acceleration, 14 s at full speed,
6.5 s across the last segment. At cruise it is nearly three minutes.

**What interrupts it.**

| Event | Effect |
| --- | --- |
| The player cancels the manoeuvre | Exit phase at once, with a yaw of 0.56 rad/s that swings the ship off the track. From full speed it is released after about a second. |
| The ring ahead is not functioning and the ship is past the middle of the segment | Forced exit with the same swerve; the player is told the lane is disrupted. |
| The last ring is reached | Deceleration and exit. |
| In multiplayer, a follower's formation leader is out of the lane and less than 500 m ahead | The follower exits. |

A ring functions while its shield generator is up. Its hull cannot be
destroyed, but its shield (1,000 points, regenerating 10 per second) can
be shot down, and a mission action can drain it. That is how lane
attackers (chapter 06) pull traffic into an ambush and how a mission
stops the player at a chosen spot.

NPCs use lanes through the same dock order, or through a shortcut
manoeuvre that carries a ship waiting at a ring to another named ring.

### 3.7 Transfer between systems

Chapter 01 defines connections; chapter 11 gives the messages. The rules:

1. Docking at a jump point passes through 3.2 like any dock, minus the
   hostility check. A jump point has one docking point, so while one ship
   or formation is cleared for it, others wait (inferred from check 6).
2. On completion the player's ship becomes invulnerable. The presentation
   plays the tunnel, loads the destination system and reports back.
3. The authority checks that the ship belongs to that session and left
   through that jump point, reads its destination system and paired jump
   point, and moves the ship. A system is loaded when its first player
   arrives (per design notes).
4. The ship arrives 350 m in front of the destination jump point, on a
   circle of radius 500 m around its axis at a random angle, facing along
   the axis.
5. Content is told "system entered" and the character is saved. When the
   presentation reports the arrival finished, the invulnerability ends
   and "jump-in complete" is raised.

An NPC is slid in by a scripted mover: over half a second it decelerates
along the axis from 1,000 m back to its arrival point.

### 3.8 Route planning

The authority plans routes on request, as chapter 01 outlines: for the
player's map, for mission waypoints, for NPC travel orders.

**Inside one system** the planner chains lanes greedily. It is a
heuristic, not a shortest-path search.

1. Find the ring nearest the origin. Its lane, ridden towards whichever
   end lies nearer the destination, is the first candidate. Every other
   lane with an end within 11 km of that lane's starting end is a
   candidate too.
2. Score each candidate by summing, over its lanes, the squared distances
   from both ends of the lane to the origin and to the destination. The
   lowest score wins; its last ring becomes the current ring.
3. Stop when the current ring is the lane end nearest the destination, or
   within 11 km of it, or when no candidates remain. Otherwise extend the
   winner by every unused lane with an end within 11 km of the current
   ring, and score again. A lane is used once.
4. The result is the winning chain plus a free leg to the destination.
   Its cost is travel time: free flight at 300 m/s (origin to first ring,
   and the last leg), lanes at 2,500 m/s. Hops between consecutive lanes
   are not counted.
5. If the cost exceeds the direct distance divided by 300 m/s, the route
   is one free leg. If the destination is nearer than the request's
   minimum range (500 m for a map request), the planner reports "no
   route".
6. The first lane is joined at the nearest ring lying between the origin
   and the lane's far end, not at its end ring.

Example: origin and destination 60 km apart; one lane of 45 km whose
near end is 4 km from the origin and whose far end is 12 km from the
destination. Direct: 60,000 / 300 = 200 s. By lane: 4,000 / 300 +
45,000 / 2,500 + 12,000 / 300 = 13 + 18 + 40 = 71 s. The answer is two
legs.

The 11 km is an authoring contract: lane ends meant to connect must lie
within that distance of each other (inferred).

**Across systems** the planner takes the gates-only system path and
plans in each system on it, from the origin or the jump point just
arrived through, to the next jump point or the destination. The legs are
concatenated into at most 99 waypoints, each tagged with its system and,
where there is one, the ring or jump point to use.

If a gate by which the route would enter a system is locked for the
requester or for everyone, the whole request fails with no waypoints. The
planner does not seek a detour, because the path tables are precomputed
without regard to locks. Holes are never routed through, so a route never
reveals one.

### 3.9 Waypoints

The presentation keeps the player's waypoint list. The map has two tools:
*set waypoint* appends the clicked place, *best path* replaces the list
with a planned route to it. If the planner fails, the map says so.

In flight a waypoint in the current system becomes a targetable marker
(chapter 09). Coming within 150 m of it counts as reaching it: it and every
waypoint before it are removed and the next becomes the marker. With no
live marker, passing within 300 m of any stored waypoint removes it and its
predecessors, so a lane ride clears the waypoints it skips.

Mission objectives arrive in the same form and take precedence: when a
mission sets objectives, the player's own waypoints are dropped, and
while a mission route is shown the waypoint tool is disabled (chapter 08).

### 3.10 Locks and restrictions

Travel is where the campaign fences the player in. Five switches exist,
all held by the authority and stored in the saved game (chapter 11).

| Switch | Scope | Set by |
| --- | --- | --- |
| Jump point locked for everyone | All players | Initial world state (27 jump points); mission actions |
| Jump point locked for one player | That player | Mission actions |
| Jump point closed to ambient NPCs | Population traffic; mission-spawned ships pass | Initial world state (36); mission actions |
| Player may dock: yes or no, plus exceptions | One player | Mission actions (about 120 uses in the campaign) |
| Player may use lanes: yes or no, plus exception stretches given as ring pairs | One player | Mission actions (about 50 uses) |

Unlocking a jump point "for the player" removes the per-player and the
everyone lock. The campaign opens the universe region by region this way.
A formation led through a gate sheds any player for whom it is locked.

Missions also bypass the ceremony when the story needs it: they can force
a landing, dock a ship instantly, relocate a ship, make an NPC jump in,
and give NPCs go-to orders flagged "use cruise" or "never cruise". The
authority has a service that engages a ship's cruise at once, skipping
the charge; neither this chapter's reading nor chapter 08's found content
that calls it.

### 3.11 Authority and presentation

| Authority | Presentation |
| --- | --- |
| Dock permission, choice of docking point, both lists, timeouts | The player's manoeuvres: go to, the approach, the own ship's lane ride |
| Completion test; landing; removal from the system | Docking camera, door and ring animation, lane and jump effects |
| Launch position; invincibility | The own ship's launch; "launch complete" report |
| Transfer between systems; arrival pose | Tunnel sequence; loading; "jump-in complete" report |
| Locks and restrictions; route planning | Button availability; waypoint list, map tools, marker |
| All of the left column and the manoeuvres, for NPCs | Told when another ship enters or leaves a lane |

For an NPC a request and its answer happen inside one tick. For a player
they are two messages, which is why the approach has a hold state.

## 4. Data that defines it

| Record | Principal fields | Values |
| --- | --- | --- |
| Docking point, repeatable on a solar archetype | kind; mount hardpoint; radius; optional door animation | 262 points on 55 archetypes: 106 medium moorings, 96 berths, 47 large moorings, no small ones; stations carry 2 to 18. Radius in m: berth and mooring 2 to 10, ring pad 40, hole 150, gate 225. |
| Solar archetype, other fields | docking camera yes or no; exit hardpoint and opening animation of a jump point | Docking camera on 42 archetypes |
| Model hardpoints | per docking point a mount and two approach points; on a lane ring one per direction; on every ship one mount | — |
| Ship archetype | docking class | Of 69 hulls: berths 42 (fighters, freighters), medium moorings 8 (mostly transports), large moorings 15 (gunboats and larger), none 4 |
| Lane equipment, mounted on every lane ring | effects for entering, travelling, leaving and disruption; delays before each (2 s, 0.2 s, 0.3 s); ring spin rate and acceleration; two activation distances (750 m, 500 m) | One record |
| Lane ring loadout | lane equipment, shield generator, unlimited power, two turrets, lights | Shield 1,000, regeneration 10 per second |
| Initial world state | jump points locked for everyone, and for NPC traffic | 27 and 36 |
| Mission actions (chapter 08) | lock or unlock a jump point; player may dock; player may use lanes; drain a lane ring; force a landing; relocate a ship; give the player a route | — |

The rule constants of section 3 (lane speeds and ramp, linking distance,
route cap, timeouts, hostility threshold, approach speed cap, launch
timings) are in code, not data.

Three anonymised docking point lists (radius in m in brackets):

| Archetype | Docking points |
| --- | --- |
| `example_docking_ring` | ring pad A (40, door animation), ring pad B (40, door animation), 2 medium moorings (5) |
| `example_small_station` | 3 medium moorings (5), 1 large mooring (5) |
| `example_gate` | 1 jump (225) |

## 5. Connections

Needs: solars, lane ring chains, connections and path tables (01); the
flight model, cruise and force controllers (02); the manoeuvre layer, in
which go to, dock, launch and formation are behaviours (05); the attitude
of a base's owner towards a ship (07); request and answer messages, the
event bus, system loading and saving (11).

Gives:

- 05, 06: the dock, launch and lane orders that NPC scripts and ambient
  traffic are built from; docking points as places where NPCs appear and
  leave; the NPC lock on jump points.
- 08: locks and restrictions as mission actions; "launch complete", "dock
  complete", "system entered", "jump-in complete" and lane entry and exit
  as trigger events; the planner for mission waypoints.
- 09: manoeuvre availability, dock answers and travel notices for voice
  and HUD; waypoints for the map and the marker.
- 10, 11: the moment of entering and leaving a base; the save points at
  landing and system arrival.

## 6. Design notes for the builder

**Essential.**

- *Docking as a request the world can refuse.* Reputation, campaign
  gating and mission pressure all act through one checkpoint.
- *Locks on jump points.* The pacing of the campaign rests on a list of
  closed doors that missions open.
- *A fast lane that can be broken.* Lanes make a large map tolerable and
  concentrate traffic; their vulnerability makes them the natural ambush
  site. A lane that could not be disrupted would be a loading screen.
- *Routes planned by the authority, with locks applied.* The map must not
  offer a route the player cannot fly.

**Feel.**

- The three-pose approach, door animations and docking camera.
- Spoken clearance, and the waiting queue, which matters only where
  traffic is dense enough to fill a base's docking points.

**Accidents of the era.**

- The presentation flies the player's approach and lane ride and the
  authority trusts the reported pose (chapter 11).
- The in-system planner is a greedy heuristic with an odd score. With a
  handful of lanes per system, an exact shortest-time search over rings
  is simpler and better.
- Cross-system routes fail outright on a locked gate. Searching the
  connections per request, with the requester's locks, removes that.

**Simplifications and their cost.**

- No docking classes or moorings: lost, large ships parked outside
  stations.
- No approach flight, docking completes inside a radius after clearance:
  lost, the sense of arrival and the exposure of a slow ship near a base
  under fire.
- Lanes as teleports between end rings: lost, interdiction, traffic seen
  in passing, leaving mid-lane.
- One lock scope instead of three: lost, multiplayer campaigns in which
  players stand at different points.

## 7. Open questions

1. **Moored ships.** What content does on "docked, not landed" (how long
   a moored NPC stays, how it leaves) was not traced.
2. **Airlocks.** Two archetypes have an airlock docking point, with an
   arrival rule of its own (the ship appears at the object, turned
   about). Permission treats them like jump points; what they are for was
   not established.
3. **Waypoints and lanes.** Whether go to on a marker that stands for a
   lane ring enters the lane, or the player must target the ring and
   order a dock, was not established.
4. **Initial locks in multiplayer.** Whether the 27 initial locks apply
   on a multiplayer host, where no campaign unlocks them, was not checked.
5. **Unread code.** The lane equipment values (activation distances,
   spin) were read in data only, and the station launch manoeuvre was not
   read in full.

## 8. Basis

Design documents: `design/adr/0007-server-world.md`;
`design/assessment/04-server-content.md` section 1;
`design/sim-records.md` (dock, launch and lane behaviour records);
`design/divergences.md` rows D622 to D628, D681 to D686 and D720 to D723,
as pointers into the code.

Data: `SOLAR/solararch.ini` (docking_sphere, docking_camera, jump_out_hp,
open_anim; tabulated by script), `SOLAR/loadouts.ini`,
`EQUIPMENT/select_equip.ini` (lane equipment, ring shield),
`SHIPS/shiparch.ini` and `MISSIONS/mshipprops.ini` (docking class),
`initialworld.ini` (locked gates), the mission files (counts of travel
actions). `constants.ini` holds nothing specific to travel.

Code, read in part. Common: `common_dockbehavior.cpp`,
`common_laneaction.cpp`, `common_launchaction.cpp`,
`common_gotobehavior.cpp`, `common_cruisebehavior.cpp`,
`common_iobjinspectimpl.cpp` (docking poses, lane start, ring status),
`common_cship.cpp`, `common_archetype*.cpp`. Content: `DockQueues.cpp`,
`TradelaneRequests.cpp`, `SpacePaths.cpp` (in-system planner),
`GameSupport.cpp` (route request), `SpaceSupport.cpp` (locks),
`PlayerState.cpp`, `MissionAct.cpp`, `RandomMissionDB.cpp` and
`ContentStaticIds.h` (docking class labels). Server:
`server_gameobject.cpp` (completion test, arrival pose),
`server_classes.cpp` (dock completion, launch position),
`server_pub.cpp`, `IServerImpl.cpp`. Client: `hud_maneuvers.cpp`,
`hud_waypoint.cpp`, `best_path.cpp`, `player_spawn.cpp`,
`nn_navmap_input.cpp`, `client_iclient_space.cpp`, `client_gameobj.cpp`;
headers of `space_camera_docking`, `client_land_action`, `jump_effect`.
The dock, lane, launch and fly-to scenarios in `Oracle/scenarios/`.

Inference: the approach table condenses a larger machine and names its
states, and its staging offsets are rounded; lane timings and the launch
start values are computed from the formulas; the purposes given for the
11 km linking distance, for traffic competing over docking points and
for lane disruption are reasoned from the rules. The presentation
sequences are described from header notes.
