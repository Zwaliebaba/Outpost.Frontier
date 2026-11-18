//! The system manager's combat parts together (`SystemManager`,
//! `include/common_systemmanager.h`, `Common/common_systemmanager.cpp`):
//! the target manager and its target list, the timer table, the gun
//! groups with fire control, and the formation-leader block, with
//! `InitSystemManager`, `UpdateSystemManager` and the personality refresh.
//!
//! The thrust manager and the personality live in
//! [`super::manager::ManagerState`] beside this. The tractor record at
//! +0x1498 is [`TractorRecord`]. Not ported: the repair manager
//! (`RepairManager`) and the leader's
//! borrowed personality (`bLeaderMakesMeTougher`, which needs the
//! leader's own system manager; the ship's own personality is used, an
//! open item of the ledger's AI sub-row).

use super::fire_control::FireEnv;
use super::gun_convergence::{FormationState, LeaderNotification};
use super::gun_groups::GunGroups;
use super::personality::Personality;
use super::target_manager::{CombatEvent, TargetEnv, TargetList, TargetManager};
use super::timer_table::{TimerTable, slot};
use super::world::{AiCommand, AiRand, AiWorld, SpatialQueries};

/// The tractor type mask of `find_equipment` (`CETractor`).
pub const TRACTOR_TYPE: u32 = 0x10_0000;

/// The tractor record at `SystemManager+0x1498` (`Owner1498`,
/// `Watcher149C`, `Unknown14A4`): the ship's first tractor and the loot
/// it was last pointed at. The loot behaviour drives it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TractorRecord {
    /// +0x149C the loot the tractor was pointed at (a watcher: an object
    /// that is gone reads as none).
    pub watched: Option<u32>,
    /// +0x14A4 the tractor's sub-object id (`SUBOBJ_ID_NONE`, 0, without
    /// one).
    pub equip_id: u16,
}

impl TractorRecord {
    /// `InitSystemManager`'s part: `find_equipment(&Id, 1, 0x100000)`,
    /// the first tractor or none.
    pub fn init<W: AiWorld + ?Sized>(&mut self, world: &W, ship: W::Handle) {
        self.equip_id = world
            .find_equipment(ship, TRACTOR_TYPE)
            .and_then(|ids| ids.first().copied())
            .unwrap_or(0);
    }

    /// The watched loot while it lives.
    fn live<W: AiWorld + ?Sized>(&self, world: &W) -> Option<u32> {
        self.watched.filter(|&id| world.handle_of(id).is_some())
    }

    /// The tractor's activation (module storage-062ece39): refused
    /// without a target or a tractor, or when the tractor already watches
    /// it; otherwise it watches the target and the owner's `slot_40(&Id,
    /// target, -1)` points it there. True when that answered 0.
    pub fn activate<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        target: Option<u32>,
    ) -> bool {
        let Some(target) = target else {
            return false;
        };
        if self.equip_id == 0 {
            return false;
        }
        if self.live(&*world) == Some(target) {
            return false;
        }
        self.watched = Some(target);
        let Some(h) = world.handle_of(target) else {
            // The reference passes the null the watcher reads; `slot_40`
            // answers -4.
            return false;
        };
        let equipment = self.equip_id;
        world.command(
            ship,
            AiCommand::Tractor {
                equipment,
                target: h,
            },
        ) == 0
    }

    /// The tractor's release (module storage-062ecea3): nothing without a
    /// live watched loot or a tractor; otherwise the watcher is cleared
    /// and the owner's `slot_4c(&Id, false, -1)` switches the tractor
    /// off. True when that answered 0.
    pub fn release<W: AiWorld + ?Sized>(&mut self, world: &mut W, ship: W::Handle) -> bool {
        if self.live(&*world).is_none() {
            return false;
        }
        if self.equip_id == 0 {
            return false;
        }
        self.watched = None;
        let equipment = self.equip_id;
        world.command(
            ship,
            AiCommand::ActivateEquip {
                equipment,
                on: false,
            },
        ) == 0
    }
}

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
    /// The tractor record (+0x1498).
    pub tractor: TractorRecord,
}

impl SystemManager {
    /// The combat parts of `InitSystemManager(ship)`, in the reference's
    /// order: the timer table, the gun groups (their `rand()` draws: each
    /// gun's timing, then the scan phase), then the countermeasure
    /// droppers (switched on) and the tractor record.
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
        self.tractor.init(&*world, ship);
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
