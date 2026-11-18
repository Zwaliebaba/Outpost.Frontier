//! The target manager (`TargetManager`, `include/common_iface.h`,
//! `Common/common_targetmanager.cpp`) and the AI target list it scores
//! (`SystemManagerTimerTable::Targets`, `BuildAiTargetList` in
//! `Common/common_iustategraph.cpp`).
//!
//! The target manager is the system manager's arbiter of whom the ship
//! attacks: once a second (`UpdateAttackTargetOverride`, from the
//! behaviour tick's attack-override timer) it weighs the hostiles on the
//! target list against the ship's personality (the job block: attack
//! preferences, proximity tier, field targeting, toughness thresholds) and
//! sets the ship's target through `IObjAffect::set_target`. Its attack
//! state (0 attacking, 1 outgunned, 2 idle) steers the auto turrets and
//! the behaviours.
//!
//! The reference reads each object through an `FAiTarget` record, a lazy
//! cache over one `IObjInspect`; nothing moves during a behaviour tick, so
//! the port reads [`ObjectView`]s instead: the cached position is the
//! view's `center` (what `get_physical_radius` answers), the flags its
//! `type_flags`, and the attitude pair the world's
//! `attitude_symmetrical` from the ship.
//!
//! The pure parts ([`classify_zone_level_tier`], [`zone_levels`],
//! [`attack_order_mask`], [`hostile_power_ratio`],
//! [`select_attack_candidate`], [`subtarget_value`]) take plain values so
//! the oracle can drive them.

use neuron_core::math::Vector;

use super::fp::{e, inline_fast_length, r};
use super::personality::{AttackPreference, Personality};
use super::raycast::clamped_dot3;
use super::world::{AiCommand, AiRand, AiWorld, ObjectView, class};

/// The type mask `BuildAiTargetList` scans and filters with (0x7fffff
/// less bit 0x200).
pub const SCAN_TYPE_MASK: u32 = 0x7f_fdff;

/// `GetHostilePowerRatio`'s type filter.
const POWER_TYPE_MASK: u32 = 0x7f_8110;

/// The job attack-order class masks (`GAttackOrderClassMasks`, Common's
/// `.rdata`), indexed by an attack preference's ship class; class 12 ends
/// a list. The fourteenth dword is a pointer in the image.
pub const ATTACK_ORDER_MASKS: [u32; 14] = [
    0x0001_0000,
    0x0002_0000,
    0x0010_0000,
    0x0004_0000,
    0x0008_0000,
    0x0020_0000,
    0x0000_0080,
    0x0000_0040,
    0x0000_1000,
    0x0000_2000,
    0x0000_cd3f,
    0xffff_ffff,
    0x0000_0000,
    0x063a_9df4,
];

/// The attack-preference class that ends a list.
pub const ATTACK_ORDER_END: i32 = 12;

/// The most forced targets (`MaxForcedTargets`).
pub const MAX_FORCED_TARGETS: usize = 8;

/// The most candidates `UpdateAttackTargetOverride` gathers.
const MAX_CANDIDATES: usize = 16;

/// `kTiers` of `ClassifyZoneLevelTier`.
const ZONE_TIERS: [[i32; 3]; 3] = [[0, 0, 0], [1, 0, 0], [2, 1, 1]];

/// `ClassifyZoneLevelTier`: the first tier that dominates the three zone
/// levels component-wise, 3 when none does.
#[must_use]
pub fn classify_zone_level_tier(levels: [i32; 3]) -> i32 {
    for (tier, bounds) in (0..).zip(ZONE_TIERS.iter()) {
        if bounds.iter().zip(levels.iter()).all(|(b, l)| b >= l) {
            return tier;
        }
    }
    3
}

/// The decoding half of `QueryZonePropertyLevels`: `None` when bit
/// 0x40000 of the zone-property mask is set, else the three levels (bits
/// 0x1/0x2/0x4, 0x8/0x10/0x20 and 0x8000; the highest bit of each group
/// wins).
#[must_use]
pub fn zone_levels(mask: u32) -> Option<[i32; 3]> {
    if mask & 0x4_0000 != 0 {
        return None;
    }
    let mut out = [0; 3];
    for (bit, value) in [(1, 1), (2, 2), (4, 3)] {
        if mask & bit != 0 {
            out[0] = value;
        }
    }
    for (bit, value) in [(8, 1), (0x10, 2), (0x20, 3)] {
        if mask & bit != 0 {
            out[1] = value;
        }
    }
    if mask & 0x8000 != 0 {
        out[2] = 1;
    }
    Some(out)
}

/// `GAttackOrderClassMasks[class]`; a class outside the table is 0 (D170).
#[must_use]
pub fn attack_order_mask(ship_class: i32) -> u32 {
    usize::try_from(ship_class)
        .ok()
        .and_then(|i| ATTACK_ORDER_MASKS.get(i))
        .copied()
        .unwrap_or(0)
}

/// The attack preferences up to the terminator (class 12); the list ends
/// after the thirteenth record when none is a terminator (D170).
#[must_use]
pub fn attack_preferences(p: &Personality) -> &[AttackPreference] {
    let prefs = &p.job.attack_preferences;
    let count = prefs
        .iter()
        .position(|a| a.ship_class == ATTACK_ORDER_END)
        .unwrap_or(prefs.len());
    &prefs[..count]
}

/// `MatchesAttackOrder`: whether an object's type flags meet any of the
/// attack preferences' class masks.
#[must_use]
pub fn matches_attack_order(p: &Personality, type_flags: u32) -> bool {
    attack_preferences(p)
        .iter()
        .any(|a| attack_order_mask(a.ship_class) & type_flags != 0)
}

/// One object of the target list as `GetHostilePowerRatio` reads it.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PowerEntry {
    /// `get_type`.
    pub type_flags: u32,
    /// The ship's attitude toward it.
    pub attitude: f32,
    /// Its attitude toward the ship.
    pub reverse: f32,
    /// `get_toughness`.
    pub toughness: i32,
}

/// `GetHostilePowerRatio`: the hostile toughness over the hostile and the
/// friendly toughness (the ship's own included), 0 when that total is at
/// most 0.01. Unrounded, as the reference returns it.
#[must_use]
pub fn hostile_power_ratio(own_toughness: i32, entries: &[PowerEntry]) -> f64 {
    let mut hostile = 0.0_f32;
    let mut own = r(f64::from(own_toughness));
    for entry in entries {
        if entry.type_flags & POWER_TYPE_MASK == 0 {
            continue;
        }
        if entry.reverse <= -0.6 || entry.attitude <= -0.6 {
            hostile = r(f64::from(entry.toughness) + e(hostile));
            continue;
        }
        if entry.reverse >= 0.6 {
            own = r(f64::from(entry.toughness) + e(own));
        }
    }
    let total = r(e(own) + e(hostile));
    if e(total) > 0.01 {
        e(hostile) / e(total)
    } else {
        0.0
    }
}

/// One candidate of `SelectAttackCandidate`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AttackCandidate {
    /// The object's id (the sticky-target test compares objects).
    pub id: u32,
    /// `get_type`.
    pub type_flags: u32,
    /// `is_player`.
    pub is_player: bool,
    /// The cached position (`get_physical_radius`'s centre).
    pub center: Vector,
    /// `CShip::get_targeted_enemy_count` when the object is a ship, else
    /// `None` (`cobject()` is not a `CShip`).
    pub enemy_count: Option<i32>,
}

/// The result of [`select_attack_candidate`].
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AttackSelection {
    /// The index of the pick in the candidates, `None` for none.
    pub index: Option<usize>,
    /// The weapon-type mask of the matching preference (15 when none).
    pub priority: i32,
    /// `bPlayerOverClampKept`: a lone player over the enemy clamp stays.
    pub over_clamp_kept: bool,
    /// `bPlayerOverClampDropped`: a player over the clamp was dropped.
    pub over_clamp_dropped: bool,
}

/// `TargetManager::SelectAttackCandidate`: the candidates of the first
/// attack preference that has any within its range (a player counts as
/// classes 0x30000), then a player under the enemy clamp, else the sticky
/// target, else a random one. Draws one `rand()` whenever a tier matched.
pub fn select_attack_candidate(
    prefs: &[AttackPreference],
    owner_pos: Vector,
    candidates: &[AttackCandidate],
    sticky: Option<u32>,
    clamp: (i32, i32),
    rand: &mut impl AiRand,
) -> AttackSelection {
    let mut out = AttackSelection {
        priority: 0xf,
        ..AttackSelection::default()
    };
    let mut matched: Vec<usize> = Vec::new();
    for pref in prefs {
        let mask = attack_order_mask(pref.ship_class);
        matched.clear();
        for (i, c) in candidates.iter().enumerate() {
            let flags = if c.is_player {
                mask & 0x3_0000
            } else {
                mask & c.type_flags
            };
            if flags == 0 {
                continue;
            }
            let dx = e(c.center.x) - e(owner_pos.x);
            let dy = e(c.center.y) - e(owner_pos.y);
            let dz = e(c.center.z) - e(owner_pos.z);
            let dist = inline_fast_length((dz * dz + dy * dy) + dx * dx);
            if dist <= e(pref.range) {
                matched.push(i);
                out.priority = i32::from_ne_bytes(pref.weapon_flags.to_ne_bytes());
            }
        }
        if !matched.is_empty() {
            break;
        }
    }
    if matched.is_empty() {
        return out;
    }
    let (min, max) = clamp;
    let mut best = None;
    let mut i = 0;
    while i < matched.len() {
        let c = &candidates[matched[i]];
        let Some(count) = c.enemy_count.filter(|_| c.is_player) else {
            i += 1;
            continue;
        };
        if count > max {
            if matched.len() < 2 {
                out.over_clamp_kept = true;
            } else {
                // `memmove` down; the loop then steps past the element
                // that moved into slot `i`, as the reference does.
                matched.remove(i);
                out.over_clamp_dropped = true;
            }
        }
        if count < min {
            best = Some(i);
            break;
        }
        i += 1;
    }
    if matched.is_empty() {
        return out;
    }
    let draw = rand.rand();
    let product = draw.wrapping_mul(i32::try_from(matched.len()).unwrap_or(i32::MAX));
    let scaled = (product + ((product >> 31) & 0x7fff)) >> 15;
    if let Some(b) = best {
        out.index = matched.get(b).copied();
        return out;
    }
    let mut pick = usize::try_from(scaled).ok();
    if let Some(sticky) = sticky
        && let Some(j) = matched.iter().position(|&m| candidates[m].id == sticky)
    {
        pick = Some(j);
    }
    out.index = pick.and_then(|p| matched.get(p)).copied();
    out
}

/// The value `TargetManager::enumerate_equip` scores a sub-target by: for
/// a gun (type 0x80) whose centre of mass lies on the near side of the
/// target (the offset from the target's position against the line from
/// the ship), the squared distance from the ship's centre to the target's
/// position; `f32::MAX` otherwise. Unrounded.
#[must_use]
pub fn subtarget_value(
    equip_type: u32,
    own_center: Vector,
    target_pos: Vector,
    sub_center: Vector,
) -> f64 {
    let mut value = e(f32::MAX);
    if equip_type == 0x80 {
        let to_target = super::fp::sub(target_pos, own_center);
        let offset = super::fp::sub(sub_center, target_pos);
        if clamped_dot3(offset, to_target) < 0.0 {
            let dx = e(target_pos.x) - e(own_center.x);
            let dy = e(target_pos.y) - e(own_center.y);
            let dz = e(target_pos.z) - e(own_center.z);
            value = (dz * dz + dy * dy) + dx * dx;
        }
    }
    value
}

/// A report to the content callback (`pub::AI::ContentCallback`), queued
/// for the manager's owner; only made while a callback is installed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CombatEvent {
    /// `combat_state_notification(kind, owner, target)`: kind 0 for
    /// attack state 0 (with the ship's target id or 0), 2 for state 1, 4
    /// for state 2.
    State {
        /// The `CSNType`.
        kind: i32,
        /// The ship's id.
        owner: u32,
        /// The target id, 0 for none.
        target: u32,
    },
    /// `combat_drift_event`: the ship drifted too far from its anchor.
    Drift,
    /// `ship_in_sights_event(target)`.
    InSights(u32),
}

/// The AI target list (`SystemManagerTimerTable::Targets` with its
/// `bBuilt` and `bKeepCache` flags): the ids of the objects of the ship's
/// scan list whose type meets [`SCAN_TYPE_MASK`].
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TargetList {
    /// `bBuilt`.
    pub built: bool,
    /// `bKeepCache`: set for the duration of a behaviour tick, so the list
    /// is built at most once per tick.
    pub keep_cache: bool,
    /// The ids, in scan order.
    pub ids: Vec<u32>,
}

impl TargetList {
    /// `ResetAiTargetList`.
    pub fn reset(&mut self) {
        self.built = false;
        self.ids.clear();
    }

    /// `BuildAiTargetList`: rebuilt unless a kept list was already built
    /// in this tick.
    pub fn build<W: AiWorld + ?Sized>(&mut self, world: &mut W, ship: W::Handle) -> &[u32] {
        if !self.keep_cache {
            self.ids.clear();
        }
        if self.built && self.keep_cache {
            return &self.ids;
        }
        self.built = true;
        if world.inspect(ship).is_none() {
            return &self.ids;
        }
        let Some(list) = world.scan_list(ship, self.keep_cache) else {
            return &self.ids;
        };
        for h in list {
            let Some(v) = world.inspect(h) else {
                continue;
            };
            if v.type_flags & SCAN_TYPE_MASK == 0 {
                continue;
            }
            self.ids.push(v.id);
        }
        &self.ids
    }

    /// The list (built if needed) as handles and views; objects the world
    /// no longer knows are left out.
    pub fn views<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
    ) -> Vec<(W::Handle, ObjectView)> {
        let ids = self.build(world, ship).to_vec();
        ids.iter()
            .filter_map(|&id| {
                let h = world.handle_of(id)?;
                Some((h, world.inspect(h)?))
            })
            .collect()
    }
}

/// The world, the ship and what of the manager the target manager reads.
pub struct TargetEnv<'a, W: AiWorld + ?Sized> {
    /// The world.
    pub world: &'a mut W,
    /// The ship.
    pub ship: W::Handle,
    /// The personality (`SystemManager::PersonalityData`).
    pub personality: &'a Personality,
    /// A content callback is installed (`IBehaviorManager+0x110`).
    pub content_callback: bool,
    /// The current directive (`get_current_directive`).
    pub directive: [i32; 3],
    /// The formation or follow leader (`GetCurrentLeader`) and whether it
    /// idles in formation (`bLeaderIdleFormation`).
    pub leader: Option<(u32, bool)>,
}

impl<W: AiWorld + ?Sized> TargetEnv<'_, W> {
    fn own(&self) -> Option<ObjectView> {
        self.world.inspect(self.ship)
    }

    fn attitudes(&self, h: W::Handle) -> (f32, f32) {
        self.world
            .attitude_symmetrical(self.ship, h)
            .unwrap_or((0.0, 0.0))
    }

    fn set_target(&mut self, target: Option<u32>) {
        let target = target.and_then(|id| self.world.handle_of(id));
        self.world.command(
            self.ship,
            AiCommand::SetTarget {
                target,
                subtarget: 0,
            },
        );
    }
}

/// `TargetManager` (`SystemManager+0x142C`).
#[derive(Clone, Debug, PartialEq)]
pub struct TargetManager {
    /// +0x08 the attack state: 0 attacking, 1 outgunned, 2 idle.
    pub attack_state: i32,
    /// +0x0C the last hostile power ratio.
    pub aggression_ratio: f32,
    /// +0x10 the best sub-target value `enumerate_equip` has seen.
    pub subtarget_distance_sq: f32,
    /// +0x14 the sub-target with that value.
    pub subtarget_id: u16,
    /// +0x18/+0x1C the forced targets (`submit_forced_target`).
    pub forced_targets: Vec<u32>,
    /// +0x3C the position cached when the state turned 0.
    pub anchor: Vector,
    /// +0x48 the drift commit (`bCommitted`), lapsing after 10 seconds.
    pub committed: bool,
    /// +0x4C its timer.
    pub commit_timer: f32,
    /// +0x50 the weapon-type mask of the last pick's preference.
    pub selection_priority: i32,
    /// +0x54 hostiles of the last pass at -0.6 or below.
    pub candidates_a: i32,
    /// +0x58 of them, those at -1 or below.
    pub candidates_b: i32,
    /// +0x5C the candidates gathered.
    pub selected: i32,
    /// +0x60 the pick is the player.
    pub selected_is_player: bool,
    /// +0x64 the last player's targeted-enemy count.
    pub targeted_enemy_count: i32,
    /// +0x68 a lone player over the enemy clamp was kept.
    pub over_clamp_kept: bool,
    /// +0x69 a player over the enemy clamp was dropped.
    pub over_clamp_dropped: bool,
    /// `set_player_enemy_clamp`'s bounds (process-wide in the reference,
    /// D172), both 2 until set.
    pub enemy_clamp: (i32, i32),
}

impl Default for TargetManager {
    /// `TargetManager::TargetManager` and `Attach` (the fields the
    /// constructor leaves as the allocation found them are 0, D171).
    fn default() -> Self {
        Self {
            attack_state: 2,
            aggression_ratio: 0.5,
            subtarget_distance_sq: 0.0,
            subtarget_id: 0,
            forced_targets: Vec::new(),
            anchor: Vector::ZERO,
            committed: false,
            commit_timer: 0.0,
            selection_priority: 0,
            candidates_a: 0,
            candidates_b: 0,
            selected: 0,
            selected_is_player: false,
            targeted_enemy_count: 0,
            over_clamp_kept: false,
            over_clamp_dropped: false,
            enemy_clamp: (2, 2),
        }
    }
}

impl TargetManager {
    /// `submit_forced_target`: 1 when added, 0 when the list is full or
    /// already has it.
    pub fn submit_forced_target(&mut self, id: u32) -> i32 {
        if self.forced_targets.len() >= MAX_FORCED_TARGETS || self.forced_targets.contains(&id) {
            return 0;
        }
        self.forced_targets.push(id);
        1
    }

    /// `remove_forced_target`: 1 when removed.
    pub fn remove_forced_target(&mut self, id: u32) -> i32 {
        match self.forced_targets.iter().position(|&t| t == id) {
            Some(i) => {
                self.forced_targets.remove(i);
                1
            }
            None => 0,
        }
    }

    /// `set_player_enemy_clamp`.
    pub fn set_player_enemy_clamp(&mut self, min: i32, max: i32) {
        self.enemy_clamp = (min, max);
    }

    /// `UpdateCommitTimer`: the drift commit lapses once its timer is
    /// strictly past 10 seconds.
    pub fn update_commit_timer(&mut self, dt: f32) {
        if !self.committed {
            return;
        }
        let sum = e(dt) + e(self.commit_timer);
        self.commit_timer = r(sum);
        if sum > 10.0 {
            self.commit_timer = 0.0;
            self.anchor = Vector::ZERO;
            self.committed = false;
        }
    }

    /// `enumerate_equip(id, type)` with the values it reads: keeps the
    /// sub-target of the smallest [`subtarget_value`].
    pub fn enumerate_equip(
        &mut self,
        id: u16,
        equip_type: u32,
        own_center: Vector,
        target_pos: Vector,
        sub_center: Vector,
    ) -> bool {
        let value = subtarget_value(equip_type, own_center, target_pos, sub_center);
        if value < e(self.subtarget_distance_sq) {
            self.subtarget_distance_sq = r(value);
            self.subtarget_id = id;
        }
        false
    }

    /// `SetAttackState`: a change is stored and, with a content callback,
    /// reported; state 0 also caches the ship's position as the anchor.
    pub fn set_attack_state<W: AiWorld + ?Sized>(
        &mut self,
        env: &mut TargetEnv<'_, W>,
        events: &mut Vec<CombatEvent>,
        state: i32,
    ) {
        if self.attack_state == state {
            return;
        }
        self.attack_state = state;
        let own = env.own();
        let owner = own.as_ref().map_or(0, |v| v.id);
        let kind = match state {
            0 => 0,
            1 => 2,
            2 => 4,
            _ => return,
        };
        if env.content_callback {
            let target = if state == 0 {
                own.as_ref().and_then(|v| v.target).unwrap_or(0)
            } else {
                0
            };
            events.push(CombatEvent::State {
                kind,
                owner,
                target,
            });
        }
        if state == 0 {
            self.anchor = own.map_or(Vector::ZERO, |v| v.position);
        }
    }

    /// `QueryZonePropertyLevels` at a position: the levels of the zones
    /// over a 20 m sphere, `None` when the mask has bit 0x40000.
    fn zone_levels_at<W: AiWorld + ?Sized>(
        env: &TargetEnv<'_, W>,
        view: &ObjectView,
    ) -> Option<[i32; 3]> {
        let system = env.world.system_of(env.ship).unwrap_or(0);
        zone_levels(
            env.world
                .sphere_zone_properties(system, view.position, 20.0),
        )
    }

    /// `GetHostilePowerRatio` over the target list.
    pub fn hostile_power_ratio<W: AiWorld + ?Sized>(
        env: &mut TargetEnv<'_, W>,
        list: &mut TargetList,
    ) -> f64 {
        let own = env.own().map_or(0, |v| v.toughness);
        let entries: Vec<PowerEntry> = list
            .views(&mut *env.world, env.ship)
            .into_iter()
            .map(|(h, v)| {
                let (attitude, reverse) = env.attitudes(h);
                PowerEntry {
                    type_flags: v.type_flags,
                    attitude,
                    reverse,
                    toughness: v.toughness,
                }
            })
            .collect();
        hostile_power_ratio(own, &entries)
    }

    /// `GetScanResponse`: 1 when the hostile power ratio is below the
    /// personality's loot-flee toughness threshold, else 0. Drops the
    /// target list first.
    pub fn scan_response<W: AiWorld + ?Sized>(
        env: &mut TargetEnv<'_, W>,
        list: &mut TargetList,
    ) -> i32 {
        list.reset();
        let ratio = Self::hostile_power_ratio(env, list);
        let threshold = Personality::toughness_ratio(env.personality.job.loot_flee_threshold);
        if ratio >= e(threshold) { 0 } else { 1 }
    }

    /// `RetargetOwner(target, sub)`: false when the ship already has that
    /// target object; true otherwise (set unless the ids match).
    pub fn retarget_owner<W: AiWorld + ?Sized>(
        env: &mut TargetEnv<'_, W>,
        target: Option<u32>,
        subtarget: u16,
    ) -> bool {
        let current = env.own().and_then(|v| v.target);
        if current == target {
            return false;
        }
        let target_h = target.and_then(|id| env.world.handle_of(id));
        env.world.command(
            env.ship,
            AiCommand::SetTarget {
                target: target_h,
                subtarget,
            },
        );
        true
    }

    /// `NotifyOverrideCommitTail`: clear the ship's target if it has one.
    fn commit_tail<W: AiWorld + ?Sized>(env: &mut TargetEnv<'_, W>) {
        if env.own().and_then(|v| v.target).is_some() {
            env.set_target(None);
        }
    }

    /// `UpdateAttackTargetOverride`: the once-a-second target choice.
    #[allow(clippy::too_many_lines)]
    pub fn update_attack_target_override<W: AiWorld + AiRand + ?Sized>(
        &mut self,
        env: &mut TargetEnv<'_, W>,
        list: &mut TargetList,
        events: &mut Vec<CombatEvent>,
    ) {
        let job = env.personality.job;
        let Some(own) = env.own() else {
            return;
        };
        self.candidates_a = 0;
        self.candidates_b = 0;
        self.selected = 0;
        self.selected_is_player = false;
        self.targeted_enemy_count = 0;
        self.over_clamp_kept = false;
        self.over_clamp_dropped = false;

        let leader = env.leader.filter(|&(id, _)| id != own.id);
        let mut skip_directive_gate = false;
        let mut probe_target: Option<u32> = None;
        let leader_view = leader
            .and_then(|(id, _)| env.world.handle_of(id))
            .and_then(|h| env.world.inspect(h));
        if let Some((_, idle_formation)) = leader {
            if idle_formation {
                Self::commit_tail(env);
                return;
            }
            probe_target = leader_view.as_ref().and_then(|v| v.target);
            if job.wait_for_leader_target == 1 && probe_target.is_none() {
                return;
            }
            skip_directive_gate = true;
        }
        if !skip_directive_gate {
            let [kind, arg, tag] = env.directive;
            if tag != 0x14 && tag != 6 && kind == 1 && arg == 0 {
                return;
            }
        }
        if self.committed {
            return;
        }
        let self_pos = own.position;
        if self.attack_state == 0 {
            let dx = r(e(self.anchor.x) - e(self_pos.x));
            let dy = r(e(self.anchor.y) - e(self_pos.y));
            let dz = r(e(self.anchor.z) - e(self_pos.z));
            let dist = inline_fast_length((e(dx) * e(dx) + e(dz) * e(dz)) + e(dy) * e(dy));
            if dist >= e(job.unknown_0c) {
                if env.content_callback {
                    events.push(CombatEvent::Drift);
                }
                self.committed = true;
                self.set_attack_state(env, events, 2);
                Self::commit_tail(env);
                return;
            }
        }
        let use_ambient_tier = Self::zone_levels_at(env, &own)
            .is_some_and(|levels| classify_zone_level_tier(levels) <= job.field_targeting);

        let proximity = env.personality.range_threshold_value();
        let targets = list.views(&mut *env.world, env.ship);
        let mut hostile_threshold = -0.6_f32;
        for (h, v) in &targets {
            let (attitude, _) = env.attitudes(*h);
            if !(attitude <= -0.6) {
                continue;
            }
            self.candidates_a += 1;
            if !(attitude <= -1.0) {
                continue;
            }
            self.candidates_b += 1;
            let dx = r(e(v.position.x) - e(self_pos.x));
            let dy = r(e(v.position.y) - e(self_pos.y));
            let dz = r(e(v.position.z) - e(self_pos.z));
            let dist = inline_fast_length((e(dx) * e(dx) + e(dz) * e(dz)) + e(dy) * e(dy));
            if dist < proximity {
                hostile_threshold = -1.0;
            }
        }

        let aggression_threshold =
            Personality::toughness_ratio(env.personality.job.scene_toughness_threshold);
        let ratio = Self::hostile_power_ratio(env, list);
        self.aggression_ratio = r(ratio);
        if ratio != 0.0 && e(aggression_threshold) < ratio {
            self.set_attack_state(env, events, 1);
            return;
        }
        // The user-zone containment test: the pathfinder is not ported, so
        // there is no user zone (`GetUserZoneApproachDirection` fails).
        let mut slots: Vec<(W::Handle, ObjectView)> = Vec::new();
        for (h, v) in &targets {
            if v.is_player && v.is(class::SHIP) {
                self.targeted_enemy_count = env.world.targeted_enemy_count(*h);
            }
            for &forced in &self.forced_targets {
                if v.id == forced && slots.len() < MAX_CANDIDATES {
                    slots.push((*h, v.clone()));
                }
            }
            if v.type_flags & 0xfdff != 0 && v.hit_points.0 == 0.0 {
                continue;
            }
            if job.allow_player_targeting == 0 && v.is_player {
                continue;
            }
            let (attitude, _) = env.attitudes(*h);
            if !(attitude <= hostile_threshold) {
                continue;
            }
            let dx = e(v.position.x) - e(self_pos.x);
            let dy = e(v.position.y) - e(self_pos.y);
            let dz = e(v.position.z) - e(self_pos.z);
            let dist = inline_fast_length((dz * dz + dy * dy) + dx * dx);
            if !(dist < proximity) {
                continue;
            }
            if let Some(lv) = &leader_view {
                let dx = e(v.position.x) - e(lv.position.x);
                let dy = e(v.position.y) - e(lv.position.y);
                let dz = e(v.position.z) - e(lv.position.z);
                let dist = inline_fast_length((dz * dz + dy * dy) + dx * dx);
                if dist > e(job.maximum_leader_target_distance) {
                    continue;
                }
            }
            if use_ambient_tier {
                match Self::zone_levels_at(env, v) {
                    Some(levels) if classify_zone_level_tier(levels) <= job.field_targeting => {}
                    _ => continue,
                }
            }
            if slots.len() < MAX_CANDIDATES {
                slots.push((*h, v.clone()));
            }
        }
        self.selected = i32::try_from(slots.len()).unwrap_or(0);
        if !slots.is_empty() {
            let sticky = own.target;
            let candidates: Vec<AttackCandidate> = slots
                .iter()
                .map(|(h, v)| AttackCandidate {
                    id: v.id,
                    type_flags: v.type_flags,
                    is_player: v.is_player,
                    center: v.center,
                    enemy_count: v
                        .is(class::SHIP)
                        .then(|| env.world.targeted_enemy_count(*h)),
                })
                .collect();
            let pick = select_attack_candidate(
                attack_preferences(env.personality),
                own.position,
                &candidates,
                sticky,
                self.enemy_clamp,
                &mut &mut *env.world,
            );
            self.over_clamp_kept |= pick.over_clamp_kept;
            self.over_clamp_dropped |= pick.over_clamp_dropped;
            if let Some(index) = pick.index {
                let chosen = &candidates[index];
                if chosen.is_player {
                    self.selected_is_player = true;
                }
                self.selection_priority = pick.priority;
                let current = env.own().and_then(|v| v.target);
                if current != Some(chosen.id) {
                    env.set_target(Some(chosen.id));
                }
                self.set_attack_state(env, events, 0);
                if job.force_attack_formation == 0 {
                    return;
                }
                if skip_directive_gate {
                    let current = env.own().and_then(|v| v.target);
                    if current == probe_target {
                        return;
                    }
                    env.set_target(probe_target);
                    return;
                }
                let followers = if own.is(class::SHIP) {
                    env.own().map(|v| v.formation_followers).unwrap_or_default()
                } else {
                    Vec::new()
                };
                for follower in followers.into_iter().take(0x20) {
                    let Some(fh) = env.world.handle_of(follower) else {
                        continue;
                    };
                    if env.world.inspect(fh).is_none_or(|f| f.is_player) {
                        continue;
                    }
                    let target = env.world.handle_of(chosen.id);
                    env.world.command(
                        fh,
                        AiCommand::SetTarget {
                            target,
                            subtarget: 0,
                        },
                    );
                }
                return;
            }
        }
        if env.own().and_then(|v| v.target).is_some() {
            env.set_target(None);
        }
        self.set_attack_state(env, events, 2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ai::world::RecordedRand;

    #[test]
    fn zone_tiers_and_levels() {
        assert_eq!(classify_zone_level_tier([0, 0, 0]), 0);
        assert_eq!(classify_zone_level_tier([1, 0, 0]), 1);
        assert_eq!(classify_zone_level_tier([0, 1, 0]), 2);
        assert_eq!(classify_zone_level_tier([3, 0, 0]), 3);
        assert_eq!(zone_levels(0x4_0001), None);
        assert_eq!(zone_levels(0x8000 | 0x10 | 0x5), Some([3, 2, 1]));
        assert_eq!(zone_levels(0), Some([0, 0, 0]));
    }

    #[test]
    fn attack_order_reads_up_to_the_terminator() {
        let mut p = Personality::default();
        assert_eq!(attack_preferences(&p).len(), 1);
        assert!(matches_attack_order(&p, 0x40));
        p.job.attack_preferences[0].ship_class = 6;
        assert!(matches_attack_order(&p, 0x80));
        assert!(!matches_attack_order(&p, 0x40));
        assert_eq!(attack_order_mask(99), 0);
        assert_eq!(attack_order_mask(-1), 0);
    }

    #[test]
    fn power_ratio() {
        let hostile = PowerEntry {
            type_flags: 0x1_0000,
            attitude: -0.7,
            reverse: 0.0,
            toughness: 3,
        };
        let friend = PowerEntry {
            type_flags: 0x1_0000,
            attitude: 0.0,
            reverse: 0.8,
            toughness: 2,
        };
        let ignored = PowerEntry {
            type_flags: 0x8,
            ..hostile
        };
        assert_eq!(hostile_power_ratio(1, &[hostile, friend, ignored]), 0.5);
        assert_eq!(hostile_power_ratio(0, &[]), 0.0);
    }

    fn cand(id: u32, x: f32, player: Option<i32>) -> AttackCandidate {
        AttackCandidate {
            id,
            type_flags: 0x1_0000,
            is_player: player.is_some(),
            center: Vector::new(x, 0.0, 0.0),
            enemy_count: player,
        }
    }

    #[test]
    fn selection_draws_and_prefers() {
        let p = Personality::default();
        let prefs = attack_preferences(&p);
        let c = [cand(1, 100.0, None), cand(2, 200.0, None)];
        let mut rand = RecordedRand::new(vec![20_000]);
        let s = select_attack_candidate(prefs, Vector::ZERO, &c, None, (2, 2), &mut rand);
        assert_eq!(s.index, Some(1));
        assert_eq!(s.priority, 0xf);
        assert!(rand.exhausted_exactly());
        // The sticky target wins over the draw, which is still made.
        let mut rand = RecordedRand::new(vec![20_000]);
        let s = select_attack_candidate(prefs, Vector::ZERO, &c, Some(1), (2, 2), &mut rand);
        assert_eq!(s.index, Some(0));
        assert!(rand.exhausted_exactly());
        // A player over the clamp is dropped; the loop then steps past the
        // one that moved into its slot, so the draw picks.
        let c = [cand(1, 100.0, Some(5)), cand(2, 100.0, Some(0))];
        let mut rand = RecordedRand::new(vec![0]);
        let s = select_attack_candidate(prefs, Vector::ZERO, &c, None, (2, 2), &mut rand);
        assert!(s.over_clamp_dropped);
        assert_eq!(s.index, Some(1));
        // Out of range: nothing, no draw.
        let c = [cand(1, 6000.0, None)];
        let mut rand = RecordedRand::new(vec![]);
        let s = select_attack_candidate(prefs, Vector::ZERO, &c, None, (2, 2), &mut rand);
        assert_eq!(s.index, None);
        assert_eq!(rand.consumed(), 0);
    }

    #[test]
    fn commit_timer_and_forced_targets() {
        let mut t = TargetManager {
            committed: true,
            anchor: Vector::new(1.0, 2.0, 3.0),
            ..TargetManager::default()
        };
        t.update_commit_timer(6.0);
        assert!(t.committed);
        t.update_commit_timer(4.0);
        assert!(t.committed);
        t.update_commit_timer(0.5);
        assert!(!t.committed);
        assert_eq!(t.anchor, Vector::ZERO);
        for id in 0..8 {
            assert_eq!(t.submit_forced_target(id), 1);
        }
        assert_eq!(t.submit_forced_target(9), 0);
        assert_eq!(t.remove_forced_target(3), 1);
        assert_eq!(t.remove_forced_target(3), 0);
        assert_eq!(t.forced_targets, vec![0, 1, 2, 4, 5, 6, 7]);
    }

    #[test]
    fn subtarget_scoring() {
        let own = Vector::ZERO;
        let target = Vector::new(0.0, 0.0, 100.0);
        assert_eq!(
            subtarget_value(0x80, own, target, Vector::new(0.0, 0.0, 99.0)),
            10_000.0
        );
        assert_eq!(
            subtarget_value(0x80, own, target, Vector::new(0.0, 0.0, 101.0)),
            e(f32::MAX)
        );
        let mut t = TargetManager {
            subtarget_distance_sq: f32::MAX,
            ..TargetManager::default()
        };
        t.enumerate_equip(7, 0x80, own, target, Vector::new(0.0, 0.0, 99.0));
        assert_eq!(t.subtarget_id, 7);
    }
}
