//! The system manager's timer table (`SystemManagerTimerTable`,
//! `Common/common_sysmgr_timertable.cpp`, the timer half; its target list
//! is [`super::target_manager::TargetList`]).
//!
//! Eleven sustained-threshold timers latch while the damage level the
//! ship took recently exceeds a personality threshold (the damage
//! reactions: evade, brake, engine kill and face, afterburner, roll, fire
//! guns, drop mines, fire missiles, break formation, break apart); the
//! level decays at half the ship's hull and shield capacity per second.
//! The missile list tracks incoming missiles; its scan keeps the shortest
//! estimated time to impact, which the missile reactions compare with
//! their times.

use neuron_core::math::Vector;

use super::fp::{e, normalize3, r};
use super::personality::Personality;
use super::raycast::clamped_dot3;
use super::world::AiWorld;

/// `SustainedThresholdTimer` (0x10 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SustainedThresholdTimer {
    /// +0x00 latched once the level exceeds the threshold.
    pub active: bool,
    /// +0x04 the trigger threshold.
    pub threshold: f32,
    /// +0x08 how long the latch holds.
    pub duration: f32,
    /// +0x0C the time spent latched.
    pub accumulator: f32,
}

impl Default for SustainedThresholdTimer {
    /// `ConstructSystemManagerTimerTable`'s values.
    fn default() -> Self {
        Self {
            active: false,
            threshold: 100.0,
            duration: 1.0,
            accumulator: 0.0,
        }
    }
}

impl SustainedThresholdTimer {
    /// `SustainedThresholdTimer::Update(dt, level)`: latch on a positive
    /// level above the threshold, then hold for `duration`; true while
    /// latched.
    pub fn update(&mut self, dt: f32, level: f32) -> bool {
        if level > 0.0 && level > self.threshold {
            self.active = true;
        } else if !self.active {
            return false;
        }
        let sum = e(dt) + e(self.accumulator);
        self.accumulator = r(sum);
        if sum > e(self.duration) {
            self.accumulator = 0.0;
            self.active = false;
            return false;
        }
        true
    }
}

/// The timer slots by reaction.
pub mod slot {
    /// Evade break.
    pub const EVADE_BREAK: usize = 0;
    /// Evade dodge more.
    pub const EVADE_DODGE_MORE: usize = 1;
    /// Brake reverse.
    pub const BRAKE_REVERSE: usize = 2;
    /// Engine kill and face (`IsTimer3Active`).
    pub const ENGINE_KILL_FACE: usize = 3;
    /// Afterburner.
    pub const AFTERBURNER: usize = 4;
    /// Roll.
    pub const ROLL: usize = 5;
    /// Fire guns (fires every gun regardless of its timing).
    pub const FIRE_GUNS: usize = 6;
    /// Drop mines.
    pub const DROP_MINES: usize = 7;
    /// Fire missiles.
    pub const FIRE_MISSILES: usize = 8;
    /// Break formation.
    pub const BREAK_FORMATION: usize = 9;
    /// Break apart formation.
    pub const BREAK_APART: usize = 10;
}

/// `SystemManagerTimerTable` without the target list.
#[derive(Clone, Debug, PartialEq)]
pub struct TimerTable {
    /// +0x08 the scanner's id (`find_equipment(.., 0x80000)`).
    pub scanner: Option<u16>,
    /// +0x20 the ship's hull and shield capacity (`DecayRate`).
    pub decay_rate: f32,
    /// +0x24 the recent damage level.
    pub level: f32,
    /// +0x28 the eleven timers.
    pub timers: [SustainedThresholdTimer; 11],
    /// +0xD8 the decay scale (0.5).
    pub decay_scale: f32,
    /// +0xE0 the incoming missiles' ids, in arrival order.
    pub missiles: Vec<u32>,
    /// +0xE8 the missile the last scan kept.
    pub best_missile: Option<u32>,
    /// +0xF4 the shortest time to impact (`f32::MAX` for none).
    pub best_time: f32,
}

impl Default for TimerTable {
    /// `ConstructSystemManagerTimerTable`.
    fn default() -> Self {
        Self {
            scanner: None,
            decay_rate: 0.0,
            level: 0.0,
            timers: [SustainedThresholdTimer::default(); 11],
            decay_scale: 0.5,
            missiles: Vec::new(),
            best_missile: None,
            best_time: f32::MAX,
        }
    }
}

impl TimerTable {
    /// `ArmSystemManagerTimerTable`'s reads: the scanner and the decay
    /// rate (maximum shield plus maximum hit points). False when the ship
    /// has no scanner.
    pub fn arm<W: AiWorld + ?Sized>(&mut self, world: &W, ship: W::Handle) -> bool {
        let Some(view) = world.inspect(ship) else {
            return false;
        };
        let Some(scanner) = world
            .find_equipment(ship, 0x8_0000)
            .and_then(|ids| ids.first().copied())
        else {
            return false;
        };
        self.scanner = Some(scanner);
        let max_shield = view.shield.map_or(0.0, |s| s.1);
        self.decay_rate = r(e(max_shield) + e(view.hit_points.1));
        true
    }

    /// `RefreshSystemManagerTimerThresholdsFromPersonality`.
    pub fn refresh_from_personality(&mut self, p: &Personality) {
        let d = &p.damage_reaction;
        let f = &p.formation;
        let scale = e(self.decay_rate);
        let rows: [(f32, f32); 11] = [
            (d.evade_break_trigger_percent, 1.0),
            (d.evade_dodge_more_trigger_percent, 1.0),
            (d.brake_reverse_trigger_percent, 1.0),
            (
                d.engine_kill_face_trigger_percent,
                d.engine_kill_face_trigger_time,
            ),
            (d.afterburner_trigger_percent, d.afterburner_trigger_time),
            (d.roll_trigger_percent, d.roll_trigger_time),
            (d.fire_guns_trigger_percent, d.fire_guns_trigger_time),
            (d.drop_mines_trigger_percent, d.drop_mines_trigger_time),
            (
                d.fire_missiles_trigger_percent,
                d.fire_missiles_trigger_time,
            ),
            (f.break_damage_trigger_percent, f.break_damage_trigger_time),
            (
                f.break_apart_damage_trigger_percent,
                f.break_apart_damage_trigger_time,
            ),
        ];
        for (timer, (percent, time)) in self.timers.iter_mut().zip(rows) {
            timer.threshold = r(e(percent) * scale);
            timer.duration = time;
        }
    }

    /// Whether a timer is latched (`IsTimer0Active` to `IsTimer5Active`
    /// and the direct reads of the others).
    #[must_use]
    pub fn active(&self, slot: usize) -> bool {
        self.timers.get(slot).is_some_and(|t| t.active)
    }

    /// `AccumulateSystemManagerTimerLevel`: a damage record (`target` id,
    /// state, value) adds its value when it hits the ship in states 2 to
    /// 7.
    pub fn accumulate_level(&mut self, own_id: u32, target: u32, state: i32, value: f32) {
        if target == own_id && 1 < state && state < 8 {
            self.level = r(e(self.level) + e(value));
        }
    }

    /// `AppendSystemManagerTimerListEntry`: an incoming missile.
    pub fn add_missile(&mut self, id: u32) {
        self.missiles.push(id);
    }

    /// `SystemManagerTimerTableHasMissileReactionTarget`: the shortest time
    /// to impact is within one of the three missile-reaction times.
    #[must_use]
    pub fn has_missile_reaction_target(&self, p: &Personality) -> bool {
        let best = self.best_time;
        if !(best > 0.0) {
            return false;
        }
        let m = &p.missile_reaction;
        best <= m.evade_break_time || best <= m.evade_slide_time || best <= m.evade_afterburn_time
    }

    /// `UpdateSystemManagerCandidateScan`: drop the missiles that are gone,
    /// keep the nearest closing one within the evade distance, and lower
    /// the time to impact to its estimate.
    pub fn scan_missiles<W: AiWorld + ?Sized>(
        &mut self,
        world: &W,
        ship: W::Handle,
        p: &Personality,
    ) {
        self.best_missile = None;
        let views: Vec<_> = self
            .missiles
            .iter()
            .map(|&id| world.handle_of(id).and_then(|h| world.inspect(h)))
            .collect();
        let mut kept = Vec::with_capacity(self.missiles.len());
        let Some(own) = world.inspect(ship) else {
            return;
        };
        let mut found = false;
        let mut best_distance = f32::MAX;
        let mut best_velocity = Vector::ZERO;
        for (id, view) in self.missiles.iter().zip(views) {
            let Some(m) = view else {
                continue;
            };
            kept.push(*id);
            let delta = Vector::new(
                r(e(own.center.x) - e(m.position.x)),
                r(e(own.center.y) - e(m.position.y)),
                r(e(own.center.z) - e(m.position.z)),
            );
            let v = m.velocity;
            let along = (e(delta.z) * e(v.z) + e(delta.y) * e(v.y)) + e(delta.x) * e(v.x);
            if along > 0.0 {
                let distance = ((e(delta.z) * e(delta.z) + e(delta.y) * e(delta.y))
                    + e(delta.x) * e(delta.x))
                .sqrt();
                if distance < e(p.missile_reaction.evade_distance) && distance < e(best_distance) {
                    best_distance = r(distance);
                    best_velocity = v;
                    self.best_missile = Some(*id);
                    found = true;
                }
            }
        }
        self.missiles = kept;
        if !found {
            self.best_time = f32::MAX;
            return;
        }
        let own_dir = normalize3(own.velocity);
        let missile_dir = normalize3(best_velocity);
        let dot = r(clamped_dot3(own_dir, missile_dir));
        let bv = best_velocity;
        let ov = own.velocity;
        let missile_speed = ((e(bv.y) * e(bv.y) + e(bv.z) * e(bv.z)) + e(bv.x) * e(bv.x)).sqrt();
        let own_speed = ((e(ov.z) * e(ov.z) + e(ov.y) * e(ov.y)) + e(ov.x) * e(ov.x)).sqrt();
        let closing = missile_speed - own_speed * e(dot);
        let estimate = e(best_distance) / closing;
        if estimate < e(self.best_time) && estimate > 0.0 {
            self.best_time = r(estimate);
        }
    }

    /// `UpdateSystemManagerTimerTable(dt)`: the missile scan, the eleven
    /// timers against the level, then the level's decay.
    pub fn update<W: AiWorld + ?Sized>(
        &mut self,
        world: &W,
        ship: W::Handle,
        p: &Personality,
        dt: f32,
    ) {
        if self.missiles.is_empty() {
            self.best_missile = None;
            self.best_time = f32::MAX;
        } else {
            self.scan_missiles(world, ship, p);
        }
        let level = self.level;
        for timer in &mut self.timers {
            timer.update(dt, level);
        }
        if self.level > 0.0 {
            let level = e(self.level) - e(self.decay_scale) * e(self.decay_rate) * e(dt);
            self.level = r(level);
            if level < 0.0 {
                self.level = 0.0;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timer_latches_and_holds() {
        let mut t = SustainedThresholdTimer {
            threshold: 10.0,
            duration: 1.0,
            ..SustainedThresholdTimer::default()
        };
        assert!(!t.update(0.5, 5.0));
        assert!(t.update(0.5, 20.0));
        assert!(t.update(0.4, 0.0));
        assert!(!t.update(0.2, 0.0));
        assert!(!t.active);
        assert_eq!(t.accumulator, 0.0);
    }

    #[test]
    fn level_decays_and_thresholds_scale() {
        let mut table = TimerTable {
            decay_rate: 100.0,
            ..TimerTable::default()
        };
        let p = Personality::default();
        table.refresh_from_personality(&p);
        assert_eq!(
            table.timers[slot::EVADE_DODGE_MORE].threshold,
            r(e(0.2) * 100.0)
        );
        assert_eq!(table.timers[slot::ENGINE_KILL_FACE].duration, 2.0);
        table.accumulate_level(7, 7, 3, 30.0);
        table.accumulate_level(7, 8, 3, 30.0);
        table.accumulate_level(7, 7, 8, 30.0);
        assert_eq!(table.level, 30.0);
        assert!(!table.has_missile_reaction_target(&p));
    }
}
