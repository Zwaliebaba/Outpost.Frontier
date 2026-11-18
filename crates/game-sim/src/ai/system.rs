//! The system manager's combat parts together (`SystemManager`,
//! `include/common_systemmanager.h`, `Common/common_systemmanager.cpp`):
//! the target manager and its target list, the timer table, the gun
//! groups with fire control, and the formation-leader block, with
//! `InitSystemManager`, `UpdateSystemManager` and the personality refresh.
//!
//! The thrust manager and the personality live in
//! [`super::manager::ManagerState`] beside this. Not ported: the repair
//! manager (`RepairManager`), the loot helper at +0x1498 and the leader's
//! borrowed personality (`bLeaderMakesMeTougher`, which needs the
//! leader's own system manager; the ship's own personality is used, an
//! open item of the ledger's AI sub-row).

use super::fire_control::FireEnv;
use super::gun_convergence::{FormationState, LeaderNotification};
use super::gun_groups::GunGroups;
use super::personality::Personality;
use super::target_manager::{CombatEvent, TargetEnv, TargetList, TargetManager};
use super::timer_table::{TimerTable, slot};
use super::world::{AiRand, AiWorld, SpatialQueries};

/// The system manager's combat parts.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SystemManager {
    /// `Targeting` (+0x142C).
    pub targeting: TargetManager,
    /// `Timers.Targets` (+0x40): the AI target list.
    pub targets: TargetList,
    /// `Timers` (+0x34) without the list.
    pub timers: TimerTable,
    /// `GunGroups` (+0x12C).
    pub guns: GunGroups,
    /// `GunConvergenceStateBytes` (+0x1364).
    pub formation: FormationState,
    /// The content-callback reports, in order; the owner drains them.
    pub events: Vec<CombatEvent>,
}

impl SystemManager {
    /// The combat parts of `InitSystemManager(ship)`, in the reference's
    /// order: the timer table, the gun groups (their `rand()` draws: each
    /// gun's timing, then the scan phase), then the countermeasure
    /// droppers (switched on).
    pub fn init<W: AiWorld + AiRand + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
    ) {
        if world.inspect(ship).is_none() {
            return;
        }
        self.timers.arm(&*world, ship);
        self.guns.init(world, ship, p);
        self.guns.cm.bind(world, ship);
    }

    /// `NotifySystemManagerPersonalityChanged`: the gun groups (with their
    /// draws), the timer thresholds and the force-attack flag.
    pub fn personality_changed<W: AiWorld + AiRand + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
    ) {
        self.guns.refresh_from_personality(world, ship, p);
        self.timers.refresh_from_personality(p);
        self.formation.force_attack = p.job.force_attack_formation != 0;
    }

    /// `UpdateSystemManager` up to the thrust manager: the target list is
    /// dropped, the commit timer, the gun timers and the countermeasure
    /// droppers advance.
    pub fn update_before_thrust<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
        dt: f32,
    ) {
        self.targets.built = false;
        self.targets.ids.clear();
        self.targeting.update_commit_timer(dt);
        self.guns.update_timers(&*world, dt);
        self.guns.cm.update(world, ship, p, dt);
    }

    /// `UpdateSystemManager` after the thrust manager: the timer table and
    /// the formation block. Returns the notifications to queue for the
    /// leader.
    pub fn update_after_thrust<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
        dt: f32,
    ) -> Vec<LeaderNotification> {
        self.timers.update(&*world, ship, p, dt);
        self.formation.update(&*world, ship, p, &self.timers, dt)
    }

    /// `TargetManager::AttackState`.
    #[must_use]
    pub fn attack_state(&self) -> i32 {
        self.targeting.attack_state
    }

    /// `IsTimer3Active` (the engine-kill-and-face reaction).
    #[must_use]
    pub fn timer3_active(&self) -> bool {
        self.timers.active(slot::ENGINE_KILL_FACE)
    }

    /// `SystemManagerTimerTableHasMissileReactionTarget`.
    #[must_use]
    pub fn missile_reaction(&self, p: &Personality) -> bool {
        self.timers.has_missile_reaction_target(p)
    }

    /// `UpdateAttackTargetOverride` (the attack-override timer lapsed).
    pub fn update_attack_target_override<W: AiWorld + AiRand + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
        content_callback: bool,
        directive: [i32; 3],
    ) {
        let leader = self
            .formation
            .current_leader(&*world)
            .map(|id| (id, self.formation.leader_idle_formation));
        let mut env = TargetEnv {
            world,
            ship,
            personality: p,
            content_callback,
            directive,
            leader,
        };
        self.targeting
            .update_attack_target_override(&mut env, &mut self.targets, &mut self.events);
    }

    /// `GunGroups.RunFireControlTick(target)`.
    pub fn run_fire_control_tick<W: AiWorld + AiRand + SpatialQueries + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
        target: Option<u32>,
    ) {
        let mut env = FireEnv {
            world,
            ship,
            personality: p,
            timers: &self.timers,
            attack_state: self.targeting.attack_state,
            selection_priority: self.targeting.selection_priority,
        };
        self.guns
            .run_fire_control_tick(&mut env, &mut self.targets, target);
    }

    /// `ScanAutoTurretTargets(dt)` and `FireAutoTurrets` (an AI ship's).
    pub fn auto_turrets<W: AiWorld + AiRand + SpatialQueries + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        p: &Personality,
        content_callback: bool,
        directive: [i32; 3],
        dt: f32,
    ) {
        if let Some(state) =
            self.guns
                .scan_auto_turret_targets(world, ship, p, &mut self.targets, dt)
        {
            let mut env = TargetEnv {
                world: &mut *world,
                ship,
                personality: p,
                content_callback,
                directive,
                leader: None,
            };
            self.targeting
                .set_attack_state(&mut env, &mut self.events, state);
        }
        let mut env = FireEnv {
            world,
            ship,
            personality: p,
            timers: &self.timers,
            attack_state: self.targeting.attack_state,
            selection_priority: self.targeting.selection_priority,
        };
        self.guns.fire_auto_turrets(&mut env);
    }
}
