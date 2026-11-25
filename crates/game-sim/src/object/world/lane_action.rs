//! A ship's transit of a trade lane: `CShip::go_tradelane` with its
//! `LaneAction` (`Common/common_cship.cpp:989`, `Common/common_laneaction.cpp`),
//! `CShip::stop_tradelane` and `request_exit_tradelane`
//! (`common_cship.cpp:2198`), and the ring queries the AI asks before
//! (`IObjInspect::get_lane_start`, `get_lane_direction`, `traverse_rings`,
//! `Common/common_iobjinspectimpl.cpp:1565-1722`).
//!
//! - **The start.** `get_lane_start(Ring, Lane)` asked of a ship: the ring
//!   decides the direction (the ring's centre against the ship's frame, or
//!   the named lane ring against the ring's), its neighbour that way (the
//!   other way when there is none) is the lane ring, its `HpRightLane`
//!   (forward) or `HpLeftLane` the start point, and the unit vector from
//!   the ring to the lane ring the direction.
//! - **The action.** `go_tradelane(Entrance, Exit, Obj, bFastExit, Speed)`
//!   marks the ship in a lane and makes the action. The lane runs forward
//!   (the rings' `next_ring`) when the ring after the entrance ring is the
//!   exit ring. Each ring's frame is its `HpRightLane` hardpoint, or, the
//!   other way, its `HpLeftLane` position with the right lane's orientation
//!   turned half a turn about Y. A fast entry starts at full speed (state 2,
//!   or 3 when the exit ring is the last), any other at the ship's speed
//!   over 2500 m/s (state 1). The ship's collisions go off, its control
//!   exclusion is 3 (no behaviour tick) and the controller is registered.
//! - **The controller**, each PSI: the ship projected onto the current leg
//!   (or, leaving, its own pose); a damaged exit ring past half way, or a
//!   multiplayer formation leader out of the lane and less than 500 m
//!   behind, makes the ship leave (state 4); past the exit ring the leg
//!   moves on to the next ring (no ring left: state 4); a formation or
//!   follow offset (without its z) is added in the lane's frame; the speed
//!   fraction rises by 0.125/s in state 1, is 1 in state 2 and falls to
//!   0.16 along the last leg; the pose advances `2500 · fraction · dt`
//!   along its backward axis; in state 1 for the first 3 s the pose blends
//!   from where the ship started; the ship's lane speed is set and the
//!   ship beamed there. Leaving, the pose turns at -0.56 rad/s when the
//!   exit was asked (`bExitFlag`) and the fraction falls until it reaches
//!   its target: the action stops (`stop(0)`).
//! - **The stop.** The controller is unregistered, the control exclusion
//!   lifted, the collisions back on, `stop_tradelane` called on the
//!   traveller (the server's `Ship::stop_tradelane`, which tells the
//!   players and the bus) unless the ship itself stopped the lane, and the
//!   throttle set to 1. The ship's next update removes the action. A stop
//!   in the physics step finishes after the step (D731).
//!
//! The controller runs inside the physics step, where it sees only its own
//! ship: what it reads of the rings and the leaders is gathered before each
//! step (D732).

use neuron_core::math::{
    Matrix, Quaternion, Transform, Vector, matrix_to_quaternion, mul_matrix, quat_slerp,
    quaternion_to_matrix, rotate,
};
use neuron_core::physics::ControllerContext;

use crate::ai::behavior::LaneEvent;
use crate::ai::fp::{e, normalize3, r, sub};
use crate::ai::world::LaneStart;
use crate::equip::types;
use crate::object::ship::{Lane, LaneLeader, LaneRing, LaneRingInput};
use crate::object::{Command, KindData, ObjHandle, Object, class};

use super::{ControllerRef, ServerRequest, TickList, World};

/// The lane's top speed (`2500.0f`).
pub const LANE_SPEED: f32 = 2500.0;

/// The rings ahead the controller's inputs follow at most.
const RING_LOOKAHEAD: usize = 64;

/// `π` through a float, as the double the reference turns the left lane by.
const HALF_TURN: f64 = core::f32::consts::PI as f64;

impl World {
    /// `CSolar::is_lane_ring`: a solar with the type bit 0x80.
    #[must_use]
    pub fn is_lane_ring(&self, h: ObjHandle) -> bool {
        self.get(h)
            .is_some_and(|o| o.is(class::SOLAR) && o.simple.type_bits & 0x80 != 0)
    }

    /// `IObjInspect::traverse_rings(OutRing, bForward)`: a lane ring's next
    /// ring (forward) or previous one, the id as the ring stores it (the
    /// reference answers -2 when its low 30 bits are 0, and writes it all
    /// the same); `None` (-4, nothing written) for any other object.
    #[must_use]
    pub fn traverse_rings(&self, h: ObjHandle, forward: bool) -> Option<u32> {
        if !self.is_lane_ring(h) {
            return None;
        }
        let KindData::Solar(s) = &self.get(h)?.data else {
            return None;
        };
        Some(if forward { s.next_ring } else { s.prev_ring })
    }

    /// `IObjInspect::get_lane_direction(Xform, &OutDir)` of a lane ring:
    /// the ring's centre (`get_physical_radius`) less the frame's
    /// position, each difference stored as a float, against the ring's
    /// third column; true when the dot product is negative. `None` (-4) for
    /// an object that is not a lane ring.
    #[must_use]
    pub fn lane_direction(&self, ring: ObjHandle, at: Vector) -> Option<bool> {
        if !self.is_lane_ring(ring) {
            return None;
        }
        let (_, center) = self.physical_sphere(ring);
        let d = sub(center, at);
        let m = self.get(ring)?.engine.orientation.m;
        let dot = (e(d.z) * e(m[2][2]) + e(m[1][2]) * e(d.y)) + e(m[0][2]) * e(d.x);
        Some(dot < 0.0)
    }

    /// `IObjInspect::get_lane_start(Ring, OutLane, OutStart, OutDir)` asked
    /// of the ship `h` (`ResolveLaneStart`): -4 without the ring or when the
    /// direction cannot be read (from `lane` against the ring's frame when
    /// given, else from the ring against the ship's); the lane ring is the
    /// ring's neighbour that way, or the other way round when that one is
    /// not found; -5 when the ring has no `HpRightLane` (forward) or
    /// `HpLeftLane`; else 0 with the hardpoint as the start and the unit
    /// direction from the ring to the lane ring. A ring with no neighbour
    /// either way, which the reference reads through null, answers -4
    /// (D733).
    pub fn get_lane_start(
        &self,
        h: ObjHandle,
        ring: Option<ObjHandle>,
        lane: Option<ObjHandle>,
    ) -> LaneStart<ObjHandle> {
        let fail = |result, lane| LaneStart {
            result,
            lane,
            start: None,
            direction: None,
        };
        let (Some(ship), Some(ring)) = (self.get(h), ring.filter(|&x| self.get(x).is_some()))
        else {
            return fail(-4, lane);
        };
        let direction = match lane.filter(|&l| self.get(l).is_some()) {
            Some(l) => self.lane_direction(
                l,
                self.get(ring).map_or(Vector::ZERO, |o| o.engine.position),
            ),
            None => self.lane_direction(ring, ship.engine.position),
        };
        let Some(mut forward) = direction else {
            return fail(-4, lane);
        };
        let mut ring_id = self.traverse_rings(ring, forward).unwrap_or(0);
        let mut found = self.find(ring_id);
        if found.is_none() {
            forward = !forward;
            if let Some(id) = self.traverse_rings(ring, forward) {
                ring_id = id;
            }
            found = self.find(ring_id);
        }
        let name: &[u8] = if forward {
            b"HpRightLane"
        } else {
            b"HpLeftLane"
        };
        let Some((start, _)) = self.hardpoint_world(ring, name) else {
            return fail(-5, found);
        };
        let Some(lane_ring) = found else {
            return LaneStart {
                result: -4,
                lane: None,
                start: Some(start),
                direction: None,
            };
        };
        let from = self.get(ring).map_or(Vector::ZERO, |o| o.engine.position);
        let to = self
            .get(lane_ring)
            .map_or(Vector::ZERO, |o| o.engine.position);
        LaneStart {
            result: 0,
            lane: Some(lane_ring),
            start: Some(start),
            direction: Some(normalize3(sub(to, from))),
        }
    }

    /// `LaneAction::BuildRingTransform`: the ring's `HpRightLane` hardpoint
    /// for a forward lane; otherwise the `HpLeftLane` position with the
    /// right lane's orientation turned by `RotY(π)` (`cos` and `sin` of the
    /// float π stored as floats). A missing hardpoint, which the reference
    /// leaves unwritten, reads as the ring's own pose.
    fn lane_ring_frame(&self, ring: ObjHandle, forward: bool) -> LaneRing {
        let Some(o) = self.get(ring) else {
            return LaneRing::default();
        };
        let own = (o.engine.position, o.engine.orientation);
        let (right_pos, right_orient) = self.hardpoint_world(ring, b"HpRightLane").unwrap_or(own);
        if forward {
            return LaneRing {
                ring: Some(o.simple.id),
                orientation: right_orient,
                position: right_pos,
            };
        }
        let left_pos = self
            .hardpoint_world(ring, b"HpLeftLane")
            .map_or(right_pos, |(p, _)| p);
        let (cos, sin) = (HALF_TURN.cos() as f32, HALF_TURN.sin() as f32);
        let turn = Matrix::from_rows([[cos, 0.0, sin], [0.0, 1.0, 0.0], [-sin, 0.0, cos]]);
        LaneRing {
            ring: Some(o.simple.id),
            orientation: mul_matrix(&turn, &right_orient),
            position: left_pos,
        }
    }

    /// `get_status(Health)` of a ring as the lane reads it: an object of
    /// the type bits 0x80 reads 0 when one of its shields is not working,
    /// 1 otherwise (1, the lane's own value, without shields: the
    /// reference writes nothing then); any other its hit points over the
    /// maximum, stored as a float, negative as 0.
    fn ring_health(&self, ring: ObjHandle) -> f32 {
        let Some(o) = self.get(ring) else {
            return 1.0;
        };
        if o.simple.type_bits == 0x80 {
            let Some(eq) = o.as_equipped() else {
                return 1.0;
            };
            let mut health = 1.0;
            for slot in eq.equipment.traverse(types::SHIELD, false) {
                let Some(shield) = eq.equipment.get(slot) else {
                    continue;
                };
                if !shield.is_functioning(eq.weapons_offline) {
                    return 0.0;
                }
                health = 1.0;
            }
            return health;
        }
        let relative = o.simple.relative_health();
        if relative < 0.0 { 0.0 } else { r(relative) }
    }

    /// `CShip::go_tradelane(Entrance, Exit, Obj, bFastExit, Speed)` with the
    /// ship as the traveller: the ship is in a lane and its `LaneAction`
    /// starts. 0, or -5 when the ship or a ring is missing (the reference
    /// reads a missing ring through null, D733). A ship already in a lane
    /// ends that action first (the reference keeps both, D736).
    pub fn go_tradelane(
        &mut self,
        h: ObjHandle,
        entrance: Option<ObjHandle>,
        exit: Option<ObjHandle>,
        fast_exit: bool,
        speed: f32,
    ) -> i32 {
        if self.ship_data(h).is_none() {
            return -5;
        }
        let (Some(entrance), Some(exit)) = (
            entrance.filter(|&x| self.get(x).is_some()),
            exit.filter(|&x| self.get(x).is_some()),
        ) else {
            return -5;
        };
        if self.lane_state(h).is_some() {
            self.end_lane(h);
        }
        if let Some(ship) = self.ship_data_mut(h) {
            ship.using_tradelane = true;
        }
        let entrance_id = self.ring_id(entrance);
        let exit_id = self.ring_id(exit);
        let forward = self.traverse_rings(entrance, true).unwrap_or(0) == exit_id;
        let mut lane = Lane {
            entrance_ring: entrance_id,
            exit_ring: exit_id,
            entrance: self.lane_ring_frame(entrance, forward),
            exit: self.lane_ring_frame(exit, forward),
            exit_state: 0,
            exit_flag: false,
            forward,
            fast_exit,
            speed,
            pose: Transform::IDENTITY,
            captured_position: Vector::ZERO,
            captured_orientation: Quaternion::from_words([0.0; 4]),
            elapsed: 0.0,
            ramp: 0.0,
            ramp_target: 0.0,
            exit_ramp_start: 0.0,
            rings: Vec::new(),
            in_system: true,
            leader: LaneLeader::default(),
            single_player: self.config.single_player,
            stopped_in_step: false,
        };
        // `InitializeApproach`.
        if fast_exit {
            lane.exit_state = 2;
            if self.traverse_rings(exit, forward).unwrap_or(0) == 0 {
                lane.exit_state = 3;
            }
            lane.ramp = 1.0;
            lane.elapsed = 3.0;
            lane.ramp_target = r(80.0 / e(LANE_SPEED));
        } else {
            lane.exit_state = 1;
            let v = self.velocity(h);
            let ramp = speed_fraction(v);
            lane.ramp_target = ramp;
            lane.ramp = ramp;
        }
        self.set_collisions(h, false);
        self.set_control_exclusion(h, 3);
        if let Some(o) = self.get(h) {
            lane.captured_orientation = matrix_to_quaternion(&o.engine.orientation);
            lane.captured_position = o.engine.position;
        }
        if let Some(ship) = self.ship_data_mut(h) {
            ship.lane = Some(Box::new(lane));
        }
        self.register_controller(h, ControllerRef::Lane);
        0
    }

    /// The ship's trade-lane action, while it has one.
    #[must_use]
    pub fn lane_state(&self, h: ObjHandle) -> Option<&Lane> {
        self.ship_data(h).and_then(|s| s.lane.as_deref())
    }

    fn lane_mut(&mut self, h: ObjHandle) -> Option<&mut Lane> {
        self.ship_data_mut(h).and_then(|s| s.lane.as_deref_mut())
    }

    /// `CShip::is_using_tradelane`.
    #[must_use]
    pub fn is_using_tradelane(&self, h: ObjHandle) -> bool {
        self.ship_data(h).is_some_and(|s| s.using_tradelane)
    }

    /// `CShip::get_tradelane_speed`: the lane speed while the ship is in a
    /// lane, else 0.
    #[must_use]
    pub fn tradelane_speed(&self, h: ObjHandle) -> f32 {
        self.ship_data(h)
            .filter(|s| s.using_tradelane)
            .map_or(0.0, |s| s.tradelane_speed)
    }

    /// `CShip::get_tradelane_entrance_ring`: the action's entrance ring, 0
    /// without one.
    #[must_use]
    pub fn tradelane_entrance_ring(&self, h: ObjHandle) -> u32 {
        self.lane_state(h).map_or(0, |l| l.entrance_ring)
    }

    /// `CShip::get_tradelane_exit_ring`: the reference reads the current
    /// leg's first ring (`EntranceRingXform.Ring`, not `ExitRing`), 0
    /// without an action.
    #[must_use]
    pub fn tradelane_exit_ring(&self, h: ObjHandle) -> u32 {
        self.lane_state(h)
            .and_then(|l| l.entrance.ring)
            .unwrap_or(0)
    }

    /// `CShip::stop_tradelane`: with an action, the ship is out of the lane
    /// and the action stops without calling `stop_tradelane` back
    /// (`LaneAction::stop(1)`).
    pub fn stop_tradelane(&mut self, h: ObjHandle) {
        if self.lane_state(h).is_none() {
            return;
        }
        if let Some(ship) = self.ship_data_mut(h) {
            ship.using_tradelane = false;
        }
        self.stop_lane(h, true);
    }

    /// `LaneAction::stop(bSkipStopTradelane)` outside the physics step: a
    /// running action (state other than 0) goes to 0, its controller is
    /// unregistered, the control exclusion lifted, the collisions back on,
    /// the traveller's `stop_tradelane` called unless skipped
    /// ([`World::traveller_stop_tradelane`]) and the throttle set to 1.
    fn stop_lane(&mut self, h: ObjHandle, skip_stop_tradelane: bool) {
        let Some(l) = self.lane_mut(h) else {
            return;
        };
        if l.exit_state == 0 {
            return;
        }
        l.exit_state = 0;
        self.unregister(h, ControllerRef::Lane);
        self.set_control_exclusion(h, 0);
        self.set_collisions(h, true);
        if !skip_stop_tradelane {
            self.traveller_stop_tradelane(h);
        }
        let _ = self.command(h, Command::SetThrottle(1.0));
    }

    /// `IObjAffect::stop_tradelane` on the traveller, the ship's own
    /// wrapper: `CShip::stop_tradelane`, then the rings it reports, and the
    /// server's part as a [`ServerRequest::StopTradeLane`] (D730).
    pub(crate) fn traveller_stop_tradelane(&mut self, h: ObjHandle) {
        self.stop_tradelane(h);
        let exit = self.tradelane_exit_ring(h);
        let entrance = self.tradelane_entrance_ring(h);
        self.push_server_request(ServerRequest::StopTradeLane {
            ship: h,
            entrance,
            exit,
        });
    }

    /// `IObjAffect::go_tradelane` (slot 25) from the AI on its ship's
    /// wrapper (`Ship::go_tradelane`): `CShip::go_tradelane`, -5 when it
    /// fails, else 0 and the server's part as a
    /// [`ServerRequest::GoTradeLane`] (D730).
    pub(crate) fn ai_go_tradelane(
        &mut self,
        h: ObjHandle,
        entrance: Option<ObjHandle>,
        exit: Option<ObjHandle>,
        fast_exit: bool,
        speed: f32,
    ) -> i32 {
        if self.go_tradelane(h, entrance, exit, fast_exit, speed) != 0 {
            return -5;
        }
        let entrance = entrance.map_or(0, |x| self.ring_id(x));
        let exit = exit.map_or(0, |x| self.ring_id(x));
        self.push_server_request(ServerRequest::GoTradeLane {
            ship: h,
            entrance,
            exit,
            fast_exit,
            speed,
        });
        0
    }

    /// `CShip::request_exit_tradelane`: the action leaves the lane with the
    /// turn (`LaneAction::request_exit`: `bExitFlag`, state 4), and each
    /// follow follower with a behaviour manager hears the type-4 event of
    /// the leader asking out (`IBehaviorManager::Slot00`; the reference's
    /// `FDUMP` warning is not kept, D737). Nothing without an action.
    pub fn request_exit_tradelane(&mut self, h: ObjHandle) {
        let Some(l) = self.lane_mut(h) else {
            return;
        };
        l.exit_flag = true;
        l.exit_state = 4;
        let ship = self.ring_id(h);
        let followers: Vec<ObjHandle> = self
            .ship_data(h)
            .map(|s| s.follow_followers.iter().flatten().copied().collect())
            .unwrap_or_default();
        let event = LaneEvent {
            exit_requested: true,
            ship,
            ..LaneEvent::default()
        };
        for f in followers {
            self.lane_event(f, &event);
        }
    }

    /// The manager's type-4 event for the object `h`
    /// (`IBehaviorManager::Slot00({4, &Payload})`): nothing for an object
    /// without a manager (`get_behavior_interface` answers null).
    pub fn lane_event(&mut self, h: ObjHandle, event: &LaneEvent) {
        if self.get(h).is_none() || !self.behavior_managers.contains_key(&h) {
            return;
        }
        let _ = self.with_behavior_manager(h, |m, w| m.lane_event(w, h, event));
    }

    /// `ActionDB::update` for the lane (`LaneAction::update`): the action
    /// stays while it runs (state other than 0); a stopped one is removed.
    pub(crate) fn update_lane(&mut self, h: ObjHandle) {
        if self.lane_state(h).is_some_and(|l| l.exit_state == 0) {
            self.end_lane(h);
        }
    }

    /// `~LaneAction`: the controller unregistered, the action gone.
    fn end_lane(&mut self, h: ObjHandle) {
        if self.ship_data_mut(h).and_then(|s| s.lane.take()).is_none() {
            return;
        }
        self.unregister(h, ControllerRef::Lane);
    }

    /// Before a physics step: each running lane of the system's ships gets
    /// what its controller reads of the world (D732): the rings from the
    /// exit ring on (each one's frame, health and next ring in the lane's
    /// direction, while the next is found), whether the ship is in a
    /// system, and its formation and follow leaders.
    pub(crate) fn prepare_lanes(&mut self, system: u32) {
        for h in self.list(system, TickList::Ships) {
            let Some(l) = self.lane_state(h).filter(|l| l.exit_state != 0) else {
                continue;
            };
            let forward = l.forward;
            let mut rings = Vec::new();
            let mut next = l.exit.ring.and_then(|id| self.find(id));
            while let Some(ring) = next {
                if rings.len() >= RING_LOOKAHEAD {
                    break;
                }
                let frame = self.lane_ring_frame(ring, forward);
                if rings
                    .iter()
                    .any(|x: &LaneRingInput| x.frame.ring == frame.ring)
                {
                    break;
                }
                let after = self.traverse_rings(ring, forward);
                rings.push(LaneRingInput {
                    frame,
                    health: self.ring_health(ring),
                    next: after,
                });
                next = after.filter(|&id| id != 0).and_then(|id| self.find(id));
            }
            let leader = self.lane_leader(h);
            let in_system = self.get(h).is_some_and(|o| o.simple.system.is_some());
            if let Some(l) = self.lane_mut(h) {
                l.rings = rings;
                l.leader = leader;
                l.in_system = in_system;
                l.stopped_in_step = false;
            }
        }
    }

    /// The traveller's leaders as the lane's controller reads them.
    fn lane_leader(&self, h: ObjHandle) -> LaneLeader {
        let Some(ship) = self.ship_data(h) else {
            return LaneLeader::default();
        };
        let formation = ship
            .formation_leader
            .filter(|&l| l != h && self.get(l).is_some());
        let follow = !ship.is_player && ship.follow_leader.is_some_and(|l| self.get(l).is_some());
        LaneLeader {
            formation: formation.is_some(),
            follow,
            in_lane: formation.is_some_and(|l| self.is_using_tradelane(l)),
            position: formation
                .and_then(|l| self.get(l))
                .map_or(Vector::ZERO, |o| o.engine.position),
        }
    }

    /// After a physics step: each lane its controller stopped in the step
    /// (`stop(0)`) finishes: the controller unregistered, the collisions
    /// back on, the first engine activated when it is not
    /// (`set_throttle(1)`'s activation), and the traveller's
    /// `stop_tradelane` (D731).
    pub(crate) fn finish_lanes(&mut self, system: u32) {
        for h in self.list(system, TickList::Ships) {
            let stopped = self.lane_mut(h).is_some_and(|l| {
                let s = l.stopped_in_step;
                l.stopped_in_step = false;
                s
            });
            if !stopped {
                continue;
            }
            self.unregister(h, ControllerRef::Lane);
            self.set_collisions(h, true);
            let _ = self.command(h, Command::SetThrottle(1.0));
            self.traveller_stop_tradelane(h);
        }
    }

    /// An object's id, 0 when it is gone.
    fn ring_id(&self, h: ObjHandle) -> u32 {
        self.get(h).map_or(0, |o| o.simple.id)
    }
}

/// `InitializeApproach`'s speed fraction: the speed through the inlined
/// fast inverse square root over 2500. The squared speed stays on the
/// stack; its float image seeds the guess; one rounding at the end.
fn speed_fraction(v: Vector) -> f32 {
    let sq = (e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z);
    let image = r(sq);
    let bits = i32::from_ne_bytes(image.to_bits().to_ne_bytes());
    let guess_bits = 0x5f37_59df_i32.wrapping_sub(bits >> 1);
    let guess = e(f32::from_bits(u32::from_ne_bytes(guess_bits.to_ne_bytes())));
    let refine = 1.5 - ((guess * guess) * sq) * 0.5;
    r(((sq / e(LANE_SPEED)) * refine) * guess)
}

/// `LaneAction::ProjectOntoSegment(A, B)`: the ship's position projected
/// onto the line from `a` to `b`, its progress along it (a fraction of the
/// leg) and `a`'s orientation; a leg of 1e-5 m or less gives progress 0
/// at `a`.
fn project_onto_segment(a: &LaneRing, b: &LaneRing, at: Vector) -> (f32, Transform) {
    let delta = sub(b.position, a.position);
    let len =
        ((e(delta.z) * e(delta.z) + e(delta.y) * e(delta.y)) + e(delta.x) * e(delta.x)).sqrt();
    let seg = r(len);
    if !(len > 1e-5) {
        return (0.0, Transform::new(a.orientation, a.position));
    }
    let inv = 1.0 / len;
    let dir = Vector::new(
        r(e(delta.x) * inv),
        r(e(delta.y) * inv),
        r(e(delta.z) * inv),
    );
    let rel = sub(at, a.position);
    let dot = (e(dir.z) * e(rel.z) + e(dir.y) * e(rel.y)) + e(rel.x) * e(dir.x);
    let dir_sq = (e(dir.z) * e(dir.z) + e(dir.y) * e(dir.y)) + e(dir.x) * e(dir.x);
    let along = dot / dir_sq;
    let progress = r(along / e(seg));
    let scaled = Vector::new(
        r(e(dir.x) * along),
        r(e(dir.y) * along),
        r(e(dir.z) * along),
    );
    let translation = Vector::new(
        r(e(scaled.x) + e(a.position.x)),
        r(e(scaled.y) + e(a.position.y)),
        r(e(scaled.z) + e(a.position.z)),
    );
    (progress, Transform::new(a.orientation, translation))
}

/// `LaneAction::stop(0)` inside the physics step: the state goes to 0, the
/// control exclusion is lifted and the throttle set to 1 at once; the
/// rest waits for the step's end ([`World::finish_lanes`], D731).
fn stop_in_step(lane: &mut Lane, exclusion: &mut Option<&mut u32>, throttle: &mut f32) {
    if lane.exit_state == 0 {
        return;
    }
    lane.exit_state = 0;
    lane.stopped_in_step = true;
    if let Some(x) = exclusion.as_deref_mut() {
        *x = 0;
    }
    *throttle = 1.0;
}

/// `LaneAction::do_simulation_controller`, as the module comment
/// describes. The traveller's position and transform are the object's own
/// (synced after each step, as the reference's `WorldPosition`).
pub(crate) fn lane_controller(obj: &mut Object, dt: f32, context: &mut ControllerContext<'_>) {
    let position = obj.engine.position;
    let orientation = obj.engine.orientation;
    let Object {
        equipped,
        data,
        engine,
        ..
    } = obj;
    let mut exclusion = equipped.as_deref_mut().map(|eq| &mut eq.control_exclusion);
    let KindData::Ship(ship) = data else {
        return;
    };
    let formation_offset = ship.formation_offset;
    let follow_offset = ship.follow_offset;
    let is_player = ship.is_player;
    let Some(l) = ship.lane.as_deref_mut() else {
        return;
    };
    if l.exit_state == 0 {
        return;
    }
    if l.exit_state == 5 {
        stop_in_step(l, &mut exclusion, &mut ship.flight.throttle);
        return;
    }
    l.elapsed = r(e(l.elapsed) + e(dt));
    let mut progress = 0.0f32;
    if l.exit_state == 4 {
        l.pose = Transform::new(orientation, position);
        if l.exit_ramp_start == 0.0 {
            l.exit_ramp_start = l.ramp;
        }
        if l.exit_flag {
            let angle = e(dt) * -0.56;
            let cos = r(angle.cos());
            let sin = angle.sin();
            let mut turn = Matrix::IDENTITY;
            for row in &mut turn.m {
                let (c0, c2) = (e(row[0]), e(row[2]));
                row[0] = r(e(cos) * c0 - sin * c2);
                row[2] = r(c0 * sin + e(cos) * c2);
            }
            l.pose.rotation = mul_matrix(&turn, &l.pose.rotation);
        }
        let ramp = e(l.ramp) - (e(l.exit_ramp_start) - e(l.ramp_target) * 0.4) * e(dt);
        l.ramp = r(ramp);
        if ramp <= e(l.ramp_target) {
            l.ramp = l.ramp_target;
            l.exit_state = 5;
            stop_in_step(l, &mut exclusion, &mut ship.flight.throttle);
            return;
        }
    } else {
        let (p, pose) = project_onto_segment(&l.entrance, &l.exit, position);
        progress = p;
        l.pose = pose;
    }
    // A damaged exit ring past half way: out of the lane. A player's ship
    // would hear `IObjAffect::slot_6c(1, 0)`, which the server's `Ship`
    // leaves to `GameObject`'s -3.
    if let Some(exit) = l.exit.ring {
        let health = l.ring_input(exit).map_or(1.0, |x| x.health);
        if health != 1.0 && e(progress) >= 0.5 {
            let _ = is_player;
            l.exit_flag = true;
            l.exit_state = 4;
            return;
        }
    }
    if matches!(l.exit_state, 1..=3) {
        if progress >= 1.0 && l.in_system {
            let next = l
                .exit
                .ring
                .and_then(|id| l.ring_input(id))
                .and_then(|x| x.next)
                .unwrap_or(0);
            l.entrance = l.exit;
            let next_input = (next != 0).then(|| l.ring_input(next).copied()).flatten();
            if let Some(input) = next_input {
                l.exit = input.frame;
                progress = r(e(progress) % 1.0);
                if l.exit_state != 3 && input.next.unwrap_or(next) == 0 {
                    l.exit_state = 3;
                }
            } else {
                // No ring left (or, D733, one the system does not have).
                l.exit.ring = None;
                l.exit_state = 4;
            }
        }
        let offset = if l.leader.formation {
            Some(formation_offset)
        } else if l.leader.follow {
            Some(follow_offset)
        } else {
            None
        };
        if let Some(mut offset) = offset {
            offset.z = 0.0;
            let turned = rotate(&l.pose, offset);
            let t = &mut l.pose.translation;
            *t = Vector::new(
                r(e(turned.x) + e(t.x)),
                r(e(turned.y) + e(t.y)),
                r(e(turned.z) + e(t.z)),
            );
        }
        if !l.single_player && l.leader.formation && !l.leader.in_lane {
            let m = &orientation.m;
            let back = Vector::new(-m[0][2], -m[1][2], -m[2][2]);
            let inv = 1.0
                / ((e(back.x) * e(back.x) + e(back.y) * e(back.y)) + e(back.z) * e(back.z)).sqrt();
            let back = Vector::new(r(e(back.x) * inv), r(e(back.y) * inv), r(e(back.z) * inv));
            let lp = l.leader.position;
            let leader_along = (e(back.y) * e(lp.y) + e(back.z) * e(lp.z)) + e(back.x) * e(lp.x);
            let self_along =
                (e(position.x) * e(back.x) + e(back.z) * e(position.z)) + e(back.y) * e(position.y);
            if self_along + -leader_along > -500.0 {
                l.exit_state = 4;
                return;
            }
        }
    }
    match l.exit_state {
        1 => {
            let ramped = e(dt) * 0.125 + e(l.ramp);
            l.ramp = r(ramped);
            if ramped >= 1.0 {
                l.exit_state = 2;
                l.ramp = 1.0;
            }
        }
        2 => l.ramp = 1.0,
        3 => l.ramp = r(1.0 - (1.0 - 400.0 / e(LANE_SPEED)) * e(progress)),
        _ => {}
    }
    let lane_speed = r(e(LANE_SPEED) * e(l.ramp));
    let advance = e(lane_speed) * e(dt);
    let m = l.pose.rotation.m;
    let dx = r(-e(m[0][2]) * advance);
    let dy = r(-e(m[1][2]) * advance);
    let dz = -e(m[2][2]) * advance;
    let t = l.pose.translation;
    l.pose.translation = Vector::new(r(e(dx) + e(t.x)), r(e(dy) + e(t.y)), r(dz + e(t.z)));
    let c = l.captured_position;
    l.captured_position = Vector::new(r(e(dx) + e(c.x)), r(e(dy) + e(c.y)), r(dz + e(c.z)));
    if l.exit_state == 1 && e(l.elapsed) < 3.0 {
        let lane_quat = matrix_to_quaternion(&l.pose.rotation);
        let blend = r(e(l.elapsed) * (1.0 / 3.0));
        let blended = quat_slerp(&l.captured_orientation, &lane_quat, blend);
        l.pose.rotation = quaternion_to_matrix(&blended);
        let keep = 1.0 - e(blend);
        let t = l.pose.translation;
        let c = l.captured_position;
        let lane_x = e(blend) * e(t.x);
        let lane_y = e(blend) * e(t.y);
        let lane_z = r(e(blend) * e(t.z));
        let keep_y = r(keep * e(c.y));
        let keep_z = r(keep * e(c.z));
        l.pose.translation = Vector::new(
            r(keep * e(c.x) + lane_x),
            r(e(keep_y) + lane_y),
            r(e(keep_z) + e(lane_z)),
        );
    }
    ship.tradelane_speed = lane_speed;
    context.beam_optimized(l.pose.translation, l.pose.rotation);
    // `PhySys::BeamR` writes the object's `Orientation` and
    // `WorldPosition` at once (`PhySys_BeamObject`), so the controllers
    // that run after this one in the PSI (the engine's push along the
    // third column) read the beamed pose, not the pose of the last step.
    engine.orientation = l.pose.rotation;
    engine.position = l.pose.translation;
}
