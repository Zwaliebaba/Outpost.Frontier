//! The system manager's formation-leader block (`FGunConvergenceStateBlock`,
//! `SystemManager+0x1364`, `Common/common_gunconvergence.cpp`; the
//! reference's name for it is historical: it holds the formation and
//! follow leaders, not gun convergence).
//!
//! It mirrors the ship's formation leader every tick, times the
//! force-attack cycle and the break-formation state from the damage and
//! missile timers, tells the leader when the ship breaks apart, and keeps
//! the leader's trade-lane order and two parked values for the formation
//! behaviours.

use super::fp::{e, r};
use super::personality::Personality;
use super::timer_table::{TimerTable, slot};
use super::world::{AiCommand, AiWorld};

/// The `IObjAffect` slot `SyncFormationLeader` leaves formation with
/// (`slot_6c(5, 0)`).
pub const LEAVE_FORMATION_SLOT: u8 = 27;
/// The `IObjAffect` slot `RequestRelease` calls (`slot_60(1, &id, -1)`).
pub const REQUEST_RELEASE_SLOT: u8 = 24;

/// A notification `Update` asks the manager to queue for the leader
/// (`QueueNotification(leader, extra, duration)`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LeaderNotification {
    /// The leader's id.
    pub leader: u32,
    /// The code (1 break apart, 2 regroup).
    pub extra: u32,
    /// The delay.
    pub duration: f32,
}

/// The trade-lane order a leader passes on (`SetDirectiveOrder`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DirectiveOrder {
    /// +0xBD pending; cleared once the leader leaves the lane.
    pub pending: bool,
    /// +0xA8 the first object's id.
    pub object: u32,
    /// +0xAC the second object's id.
    pub second_object: u32,
    /// +0xC2 commit directly.
    pub commit_directly: bool,
    /// +0xB8 `go_tradelane`'s fourth argument (float bits).
    pub lane_param: u32,
}

/// `FGunConvergenceStateBlock`.
#[derive(Clone, Debug, PartialEq)]
pub struct FormationState {
    /// +0x00 the force-attack cycle.
    pub force_attack_cycle_timer: f32,
    /// +0x04 time spent breaking formation.
    pub break_formation_timer: f32,
    /// +0x0C the formation leader (`SetFormationLeader`).
    pub formation_leader: Option<u32>,
    /// +0x14 the follow leader (`SetFollowLeader`).
    pub follow_leader: Option<u32>,
    /// +0x9C the narrow formation angle (0.1).
    pub narrow_angle: f32,
    /// +0xA0 the wide formation angle (0.2).
    pub wide_angle: f32,
    /// +0xB0 a parked value and +0xBE its flag.
    pub value_b0: Option<u32>,
    /// +0xB4 a parked value and +0xBF its flag.
    pub value_b4: (u32, bool),
    /// The trade-lane order.
    pub order: DirectiveOrder,
    /// +0xBC only ever cleared.
    pub field_bc: bool,
    /// +0xC0 breaking formation.
    pub breaking: bool,
    /// +0xC1 the leader idles in formation (its directive is `{1, 0}`).
    pub leader_idle_formation: bool,
    /// +0xC3 the force-attack flag (the job's `force_attack_formation`).
    pub force_attack: bool,
    /// +0xC4 the break-apart notification was sent.
    pub break_apart_notified: bool,
    /// +0xC5 both leaders are hidden.
    pub leader_suppressed: bool,
    /// +0xC6 a release was requested.
    pub release_requested: bool,
}

impl Default for FormationState {
    /// `FGunConvergenceStateBlock::FGunConvergenceStateBlock`.
    fn default() -> Self {
        Self {
            force_attack_cycle_timer: 0.0,
            break_formation_timer: 0.0,
            formation_leader: None,
            follow_leader: None,
            narrow_angle: 0.1,
            wide_angle: 0.2,
            value_b0: None,
            value_b4: (0, false),
            order: DirectiveOrder::default(),
            field_bc: false,
            breaking: false,
            leader_idle_formation: false,
            force_attack: false,
            break_apart_notified: false,
            leader_suppressed: false,
            release_requested: false,
        }
    }
}

/// The id of a watched object while it lives.
fn live<W: AiWorld + ?Sized>(world: &W, id: Option<u32>) -> Option<u32> {
    id.filter(|&id| world.handle_of(id).is_some())
}

impl FormationState {
    /// `GetCurrentLeader`: the follow leader, else the formation leader;
    /// none while suppressed.
    #[must_use]
    pub fn current_leader<W: AiWorld + ?Sized>(&self, world: &W) -> Option<u32> {
        if self.leader_suppressed {
            return None;
        }
        live(world, self.follow_leader).or_else(|| live(world, self.formation_leader))
    }

    /// `GetFormationLeader`.
    #[must_use]
    pub fn formation_leader<W: AiWorld + ?Sized>(&self, world: &W) -> Option<u32> {
        if self.leader_suppressed {
            return None;
        }
        live(world, self.formation_leader)
    }

    /// `GetFollowLeader`.
    #[must_use]
    pub fn follow_leader<W: AiWorld + ?Sized>(&self, world: &W) -> Option<u32> {
        if self.leader_suppressed {
            return None;
        }
        live(world, self.follow_leader)
    }

    /// `ClearLeaderStateBytes`.
    fn clear_leader_state(&mut self) {
        self.field_bc = false;
        self.order.pending = false;
        self.value_b0 = None;
        self.value_b4.1 = false;
        self.breaking = false;
        self.leader_idle_formation = false;
        self.order.commit_directly = false;
        self.force_attack = false;
        self.break_apart_notified = false;
    }

    /// `SetFormationLeader(target)`: a new leader (not the ship itself)
    /// clears the leader state; none lifts the suppression.
    pub fn set_formation_leader<W: AiWorld + ?Sized>(
        &mut self,
        world: &W,
        own_id: u32,
        target: Option<u32>,
    ) {
        if let Some(t) = target {
            if t == own_id {
                return;
            }
            let watched = live(world, self.formation_leader);
            if watched.is_some_and(|w| w != t) {
                self.leader_suppressed = false;
            }
            if watched != Some(t) {
                self.clear_leader_state();
                self.leader_suppressed = false;
            }
        } else {
            self.leader_suppressed = false;
        }
        self.formation_leader = target;
    }

    /// `SetFollowLeader(target)`, passed on to the ship
    /// (`set_follow_leader`).
    pub fn set_follow_leader<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        own_id: u32,
        target: Option<u32>,
    ) {
        match target {
            Some(t) if t == own_id => self.follow_leader = None,
            Some(t) => {
                let watched = live(&*world, self.follow_leader);
                if watched.is_some_and(|w| w != t) {
                    self.leader_suppressed = false;
                }
                if watched != Some(t) {
                    self.clear_leader_state();
                    self.leader_suppressed = false;
                }
                self.follow_leader = Some(t);
            }
            None => {
                self.leader_suppressed = false;
                self.follow_leader = None;
            }
        }
        let leader = live(&*world, self.follow_leader).and_then(|id| world.handle_of(id));
        world.command(ship, AiCommand::SetFollowLeader(leader));
    }

    /// `SyncFormationLeader`: mirror the ship's formation leader; a dying
    /// leader, or a lost one that was not released, makes the ship leave
    /// formation (`slot_6c(5, 0)`).
    pub fn sync_formation_leader<W: AiWorld + ?Sized>(&mut self, world: &mut W, ship: W::Handle) {
        let Some(own) = world.inspect(ship) else {
            self.set_formation_leader(&*world, 0, None);
            self.release_requested = false;
            return;
        };
        let leader = own
            .formation_leader
            .and_then(|id| world.handle_of(id))
            .and_then(|h| Some((h, world.inspect(h)?)));
        if let Some((lh, lv)) = leader {
            if lv.id != own.id && !lv.is_dying {
                self.set_formation_leader(&*world, own.id, Some(lv.id));
                self.leader_idle_formation = false;
                self.release_requested = false;
                if let Some((_, directive)) = world.manager_of(lh) {
                    self.leader_idle_formation = directive[0] == 1 && directive[1] == 0;
                }
                return;
            }
            if lv.is_dying {
                Self::leave_formation(world, ship);
                self.set_formation_leader(&*world, own.id, None);
                self.release_requested = false;
                return;
            }
        }
        if !self.release_requested
            && !self.leader_suppressed
            && live(&*world, self.formation_leader).is_some()
            && self.value_b0.is_none()
        {
            Self::leave_formation(world, ship);
        }
        self.set_formation_leader(&*world, own.id, None);
        self.release_requested = false;
    }

    fn leave_formation<W: AiWorld + ?Sized>(world: &mut W, ship: W::Handle) {
        world.command(
            ship,
            AiCommand::Slot {
                slot: LEAVE_FORMATION_SLOT,
                ints: vec![5, 0],
                floats: Vec::new(),
            },
        );
    }

    /// `Update(dt)`: the pending order, the force-attack cycle, the
    /// break-formation and break-apart reactions to the damage and missile
    /// timers. Returns the notifications to queue for the leader.
    pub fn update<W: AiWorld + ?Sized>(
        &mut self,
        world: &W,
        ship: W::Handle,
        p: &Personality,
        timers: &TimerTable,
        dt: f32,
    ) -> Vec<LeaderNotification> {
        let f = &p.formation;
        let mut out = Vec::new();
        if self.order.pending && !self.leader_suppressed {
            let leader =
                live(world, self.follow_leader).or_else(|| live(world, self.formation_leader));
            if let Some(v) = leader
                .and_then(|id| world.handle_of(id))
                .and_then(|h| world.inspect(h))
                && !v.cruise.is_some_and(|c| c.1)
            {
                self.order.pending = false;
            }
        }
        let active = self.force_attack;
        let watching = |s: &Self| !s.leader_suppressed && live(world, s.formation_leader).is_some();
        if active {
            let sum = e(dt) + e(self.force_attack_cycle_timer);
            self.force_attack_cycle_timer = r(sum);
            let limit = e(f.force_attack_unactive_time) + e(f.force_attack_active_time);
            if sum > limit {
                self.force_attack_cycle_timer = 0.0;
            }
            if watching(self) {
                self.breaking = false;
            }
        }
        if timers.active(slot::BREAK_FORMATION) && active && watching(self) {
            self.breaking = true;
        }
        let missile = timers.best_time;
        if missile > 0.0 && missile <= f.break_missile_reaction_time && active && watching(self) {
            self.breaking = true;
        }
        let mut notify = |s: &mut Self, duration: f32| {
            s.break_apart_notified = true;
            if let Some(leader) = world.inspect(ship).and_then(|v| v.formation_leader) {
                out.push(LeaderNotification {
                    leader,
                    extra: 1,
                    duration: 0.0,
                });
                if duration > 0.0 {
                    out.push(LeaderNotification {
                        leader,
                        extra: 2,
                        duration,
                    });
                }
            }
        };
        if timers.active(slot::BREAK_APART) {
            if !self.break_apart_notified {
                notify(self, f.break_damage_trigger_time);
            }
        } else {
            let pulse = missile > 0.0 && missile <= f.break_apart_missile_reaction_time;
            if !pulse {
                self.break_apart_notified = false;
            } else if !self.break_apart_notified {
                notify(self, f.break_apart_missile_reaction_time);
            }
        }
        self.break_formation_timer = if self.breaking {
            r(e(dt) + e(self.break_formation_timer))
        } else {
            0.0
        };
        out
    }

    /// `FollowerDirectiveGateB`: an AI ship breaking formation within the
    /// formation exit time.
    #[must_use]
    pub fn gate_b(&self, is_player: bool, p: &Personality) -> bool {
        !is_player && self.breaking && self.break_formation_timer < p.formation.exit_max_time
    }

    /// `FollowerDirectiveGateA(current behaviour, leader's behaviour)`.
    #[must_use]
    pub fn gate_a<W: AiWorld + ?Sized>(
        &self,
        world: &W,
        is_player: bool,
        current_behavior: i32,
        p: &Personality,
    ) -> bool {
        if is_player || p.job.force_attack_formation == 0 || self.leader_suppressed {
            return false;
        }
        let Some(leader) = live(world, self.formation_leader) else {
            return false;
        };
        if !self.breaking && self.force_attack {
            let leader_behavior = world
                .handle_of(leader)
                .and_then(|h| world.manager_of(h))
                .map(|m| m.0);
            if leader_behavior == Some(0xf) {
                return false;
            }
            return self.force_attack_cycle_timer <= p.formation.force_attack_active_time;
        }
        current_behavior == 0xa && self.gate_b(is_player, p)
    }

    /// `IsIdAmongOwnerFormationFollowers`.
    #[must_use]
    pub fn is_follower<W: AiWorld + ?Sized>(world: &W, ship: W::Handle, id: u32) -> bool {
        world
            .inspect(ship)
            .is_some_and(|v| v.formation_followers.iter().take(0x20).any(|&f| f == id))
    }

    /// `RequestRelease`: `slot_60(1, &own id, -1)`.
    pub fn request_release<W: AiWorld + ?Sized>(&mut self, world: &mut W, ship: W::Handle) -> i32 {
        self.release_requested = true;
        let id = world.inspect(ship).map_or(0, |v| v.id);
        world.command(
            ship,
            AiCommand::Slot {
                slot: REQUEST_RELEASE_SLOT,
                ints: vec![1, i64::from(id)],
                floats: Vec::new(),
            },
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_and_gate_b() {
        let s = FormationState::default();
        assert_eq!(s.narrow_angle, 0.1);
        let mut p = Personality::default();
        p.formation.exit_max_time = 2.0;
        let breaking = FormationState {
            breaking: true,
            break_formation_timer: 1.0,
            ..FormationState::default()
        };
        assert!(breaking.gate_b(false, &p));
        assert!(!breaking.gate_b(true, &p));
    }
}
