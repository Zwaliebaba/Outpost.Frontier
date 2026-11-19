//! `LootBehavior` (behaviour 16, `Common/common_lootbehavior.cpp`, class in
//! `include/common_lootbehavior.h`): an NPC with a tractor and a loot
//! preference picks the nearest loot container it may take, flies within
//! three quarters of the tractor's range, faces it and points the tractor
//! at it (the system manager's tractor record,
//! [`super::system::TractorRecord`]).
//!
//! What the server does with the tractor once it is pointed (the arm, the
//! pickup into the hold) is the object world's (`CETractor`); loot pickup
//! by players is Phase 3.

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp};
use super::behaviors::op_dword;
use super::face_idea::FaceIdea;
use super::fp::{e, r};
use super::move_idea::MoveIdea;
use super::system::TRACTOR_TYPE;
use super::world::{EQUIP_DESTROYED, LootView, class};

/// The type mask of the loot search (`GetOwnScanList(.., 0x10000000)`).
pub const LOOT_TYPE: u32 = 0x1000_0000;
/// The type mask of the claim test's ship search
/// (`GetOwnScanList(.., 0x7f0000)`).
pub const SHIP_TYPES: u32 = 0x007f_0000;

/// The contents' class of a commodity (`get_class_type`).
const CLASS_COMMODITY: u32 = 0xa;
/// The contents' class of a repair kit.
const CLASS_REPAIR_KIT: u32 = 0x1a;
/// The contents' class of a shield battery.
const CLASS_SHIELD_BATTERY: u32 = 0x1b;

/// `LootBehavior` (0x180 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct LootBehavior {
    base: BehaviorBase,
    /// +0x2c the move idea.
    pub move_idea: MoveIdea,
    /// +0xa4 the face idea.
    pub face: FaceIdea,
    /// +0x164 `SetContext`'s `find_equipment(.., 0x100000)` answered (it
    /// answers 0 for every equipped object, tractor or not).
    pub has_tractor: u8,
    /// +0x168 the tractor's range times 0.75 (0 without a tractor).
    pub tractor_reach: f32,
    /// +0x16c (1); slot 10's failure path copies the op's byte at +0x10.
    pub flag_16c: u8,
    /// +0x170 (13); no reader.
    pub field_170: f32,
    /// +0x174 the loot being collected (a watcher: an object that is gone
    /// reads as none).
    pub target: Option<u32>,
    /// +0x17c the target came with a directive.
    pub directed: u8,
}

impl LootBehavior {
    /// `LootBehavior::LootBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            move_idea: MoveIdea::new(),
            face: FaceIdea::new(),
            has_tractor: 0,
            tractor_reach: 0.0,
            flag_16c: 1,
            field_170: 13.0,
            target: None,
            directed: 0,
        }
    }

    /// The target while it lives.
    fn live<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>) -> Option<u32> {
        self.target.filter(|&id| cx.world.handle_of(id).is_some())
    }

    fn clear_target(&mut self) {
        self.target = None;
        self.directed = 0;
    }

    /// `IsLootUnclaimed(Loot)`: a container of more than one unit is never
    /// contested; otherwise false when another ship of the scan list is
    /// looting (current behaviour 16) with this container as its target.
    fn is_loot_unclaimed<W: AiEnv + ?Sized>(
        cx: &mut Cx<'_, W>,
        loot_id: u32,
        loot: &LootView,
    ) -> bool {
        if loot.units > 1 {
            return true;
        }
        let Some(list) = own_scan_list(cx, SHIP_TYPES) else {
            return false;
        };
        for other in list {
            if other == cx.ship {
                continue;
            }
            // An entry the world cannot inspect: the reference reads
            // through it (D185).
            let Some(view) = cx.world.inspect(other) else {
                continue;
            };
            if view.type_flags & SHIP_TYPES == 0 {
                continue;
            }
            if !class::is(view.class, class::SHIP) {
                continue;
            }
            let Some((behavior, _)) = cx.world.manager_of(other) else {
                continue;
            };
            if behavior != 0x10 {
                continue;
            }
            let chased = cx
                .world
                .loot_target_of(other)
                .filter(|&id| cx.world.handle_of(id).is_some());
            if chased == Some(loot_id) {
                return false;
            }
        }
        true
    }

    /// `AcquireLootTarget(&Target)`: with a loot preference and room in
    /// the hold, the nearest container of the scan list the preference
    /// allows that no other looter claims becomes the target (undirected).
    /// The walk stops at the first container AI ships may not tractor, of
    /// a kind the preference excludes, of a good the personality does not
    /// list, or empty.
    #[allow(clippy::too_many_lines)]
    fn acquire_loot_target<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) -> bool {
        let job = cx.state.personality.job;
        if job.loot_preference == 0 {
            return false;
        }
        if !own_hold_has_room(cx) {
            return false;
        }
        let own_pos = cx.own().map(|v| v.position).unwrap_or_default();
        let mut best: Option<u32> = None;
        let mut best_distance = f32::MAX;
        let Some(list) = own_scan_list(cx, LOOT_TYPE) else {
            return false;
        };
        for h in list {
            // An entry the world cannot inspect, or one that is not loot:
            // the reference reads through it (D185).
            let Some(view) = cx.world.inspect(h) else {
                continue;
            };
            if view.type_flags & LOOT_TYPE == 0 {
                continue;
            }
            let Some(loot) = cx.world.loot(h) else {
                continue;
            };
            if !loot.can_ai_tractor {
                break;
            }
            match loot.contents_class {
                CLASS_COMMODITY => {
                    if job.loot_preference & 1 == 0 {
                        break;
                    }
                    let count = usize::try_from(job.scan_for_cargo_count).unwrap_or(0);
                    if count > 0
                        && let Some(good) = loot.good_id
                        && !job.scan_for_cargo.iter().take(count).any(|&g| g == good)
                    {
                        break;
                    }
                }
                CLASS_REPAIR_KIT | CLASS_SHIELD_BATTERY if job.loot_preference & 4 == 0 => break,
                _ => {}
            }
            if loot.units == 0 {
                break;
            }
            if !Self::is_loot_unclaimed(cx, view.id, &loot) {
                continue;
            }
            let distance = fast_distance(view.position, own_pos);
            if distance < e(best_distance) {
                best_distance = r(distance);
                best = Some(view.id);
            }
        }
        self.target = best;
        self.directed = 0;
        best.is_some()
    }
}

/// `SystemManagerTimerTable::GetOwnScanList(List, Mask)`: none when the
/// scanner reports itself destroyed, else the ship's `get_scanlist(mask,
/// keep)` with the tick's cache flag.
fn own_scan_list<W: AiEnv + ?Sized>(cx: &mut Cx<'_, W>, mask: u32) -> Option<Vec<W::Handle>> {
    let destroyed = cx
        .state
        .sys
        .timers
        .scanner
        .and_then(|id| cx.world.equipment(cx.ship, id))
        .is_some_and(|i| i.flags & EQUIP_DESTROYED != 0);
    if destroyed {
        return None;
    }
    let keep = cx.state.sys.targets.keep_cache;
    cx.world.scan_list(cx.ship, mask, keep)
}

/// The cargo-hold test: the free fraction of the hold (0 for a hold of no
/// size) above 0.2. False for an owner that is not a ship (D185).
fn own_hold_has_room<W: AiEnv + ?Sized>(cx: &Cx<'_, W>) -> bool {
    let Some((remaining, hold)) = cx.world.cargo_hold(cx.ship) else {
        return false;
    };
    let fraction = if e(hold) > 0.0 {
        remaining / e(hold)
    } else {
        0.0
    };
    fraction > 0.2
}

/// The `0x5f3759df` distance: `(dz*dz + dx*dx) + dy*dy` on the stack, its
/// `float` bits for the guess, `DistSq * s * (1.5 - 0.5 * s*s*DistSq)`.
#[must_use]
pub fn fast_distance(a: Vector, b: Vector) -> f64 {
    let dx = e(a.x) - e(b.x);
    let dy = e(a.y) - e(b.y);
    let dz = e(a.z) - e(b.z);
    let dist_sq = (dz * dz + dx * dx) + dy * dy;
    let bits = i32::from_ne_bytes(r(dist_sq).to_bits().to_ne_bytes());
    let seed_bits = 0x5f37_59df_i32.wrapping_sub(bits >> 1);
    let seed = e(f32::from_bits(u32::from_ne_bytes(seed_bits.to_ne_bytes())));
    ((1.5 - ((seed * seed) * dist_sq) * 0.5) * seed) * dist_sq
}

impl<W: AiEnv + ?Sized> Behavior<W> for LootBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn as_any(&self) -> Option<&dyn core::any::Any> {
        Some(self)
    }

    /// Slot 1's world half: the ideas' contexts (when the binder took the
    /// owner), the target cleared, and the tractor: `find_equipment(..,
    /// 1, 0x100000)` answering sets the flag, and the found item's range
    /// (`get_equipment_val` 5) times 0.75 is the reach.
    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        if self.base.bound {
            self.move_idea.set_context(&*cx);
            self.face.set_context(&*cx);
        }
        self.clear_target();
        self.has_tractor = 0;
        self.tractor_reach = 0.0;
        if let Some(ids) = cx.world.find_equipment(cx.ship, TRACTOR_TYPE) {
            self.has_tractor = 1;
            let id = ids.first().copied().unwrap_or(0);
            if let Some(range) = cx.world.equipment(cx.ship, id).and_then(|i| i.range) {
                self.tractor_reach = r(e(range) * 0.75);
            }
        }
    }

    /// Slot 4: the target cleared, both ideas committed, the tractor
    /// released, then `Behavior::Slot4`.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.clear_target();
        self.move_idea.commit(cx);
        self.face.commit(cx);
        cx.state.sys.tractor.release(&mut *cx.world, cx.ship);
        self.base.disengage(cx.state);
    }

    /// Slot 5: with a tractor and the target manager in attack state 2,
    /// a live target (searched for when there is none) answers 0;
    /// otherwise 2 (3 while +0x29 is set).
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        if self.has_tractor != 0 && cx.state.sys.targeting.attack_state == 2 {
            if self.live(cx).is_none() {
                self.acquire_loot_target(cx);
            }
            if self.live(cx).is_some() {
                self.base.mode = if self.base.flag29 != 0 { 3 } else { 0 };
                return self.base.mode;
            }
        }
        self.base.mode = if self.base.flag29 != 0 { 3 } else { 2 };
        self.base.mode
    }

    /// Slot 7: without the target or room in the hold, done (2); else fly
    /// within the reach, then face the loot and point the tractor (2 once
    /// it took, with +0x10 not written), 1 meanwhile.
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        if !self.base.bound {
            self.base.result = 3;
            return 3;
        }
        let Some(loot) = self.live(cx) else {
            self.base.result = 2;
            return 2;
        };
        if !own_hold_has_room(cx) {
            self.clear_target();
            self.base.result = 2;
            return 2;
        }
        // The type refresh of the own target record (`get_type`, cached)
        // and a test of `(flags & 0x10000) | 0x20000`, which is never 0:
        // the branch behind it does not run.
        let dt = self.base.tick_delta;
        self.move_idea.set_watch_object(cx, Some(loot));
        self.move_idea.set_enabled(1);
        self.move_idea.set_range_3c(self.tractor_reach);
        if self.move_idea.update(cx, dt) == 1 {
            self.move_idea.commit(cx);
            self.face.set_watch_object(Some(loot));
            if self.face.update(cx) == 1
                && cx
                    .state
                    .sys
                    .tractor
                    .activate(&mut *cx.world, cx.ship, Some(loot))
            {
                return 2;
            }
        } else {
            self.face.set_watch_object(Some(loot));
            self.face.update(cx);
        }
        self.base.result = 1;
        1
    }

    /// Slot 10: the op's object (`FindVerifiedObject` of the dword at
    /// +0x0c) becomes the directed target; without one the directive flag
    /// is cleared and +0x16c takes the op's byte at +0x10.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        let id = op_dword(op, 0x0c);
        self.target = cx.view_of((id != 0).then_some(id)).map(|v| v.id);
        if self.target.is_some() {
            self.directed = 1;
            self.base.flags |= 0x2;
            return true;
        }
        self.base.flags &= 0xfd;
        self.flag_16c = (op_dword(op, 0x10) & 0xff) as u8;
        false
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use neuron_core::math::Matrix;
    use neuron_core::rng::CrtRand;

    use super::*;
    use crate::ai::manager::ManagerState;
    use crate::ai::world::{
        AiCommand, AiRand, AiWorld, EquipInfo, MotionModel, ObjectView, RayContact, SpatialQueries,
    };

    /// A looter (7) with a tractor (id 3, range 1000), loot containers and
    /// other ships.
    struct Field {
        objects: BTreeMap<u32, ObjectView>,
        loot: BTreeMap<u32, LootView>,
        managers: BTreeMap<u32, (i32, Option<u32>)>,
        hold: Option<(f64, f32)>,
        commands: Vec<AiCommand<u32>>,
        rand: CrtRand,
    }

    impl Field {
        fn new() -> Self {
            let mut objects = BTreeMap::new();
            objects.insert(
                7,
                ObjectView {
                    id: 7,
                    class: class::SHIP,
                    type_flags: 0x1_0000,
                    orientation: Matrix::IDENTITY,
                    ..ObjectView::default()
                },
            );
            Self {
                objects,
                loot: BTreeMap::new(),
                managers: BTreeMap::new(),
                hold: Some((50.0, 100.0)),
                commands: Vec::new(),
                rand: CrtRand::with_seed(1),
            }
        }

        fn add_loot(&mut self, id: u32, x: f32, view: LootView) {
            self.objects.insert(
                id,
                ObjectView {
                    id,
                    class: class::LOOT,
                    type_flags: LOOT_TYPE,
                    position: Vector::new(x, 0.0, 0.0),
                    ..ObjectView::default()
                },
            );
            self.loot.insert(id, view);
        }

        fn add_looter(&mut self, id: u32, target: u32) {
            self.objects.insert(
                id,
                ObjectView {
                    id,
                    class: class::SHIP,
                    type_flags: 0x1_0000,
                    ..ObjectView::default()
                },
            );
            self.managers.insert(id, (0x10, Some(target)));
        }
    }

    fn commodity(units: u32) -> LootView {
        LootView {
            units,
            can_ai_tractor: true,
            contents_class: 0xa,
            good_id: Some(99),
        }
    }

    impl AiWorld for Field {
        type Handle = u32;

        fn inspect(&self, h: u32) -> Option<ObjectView> {
            self.objects.get(&h).cloned()
        }

        fn handle_of(&self, id: u32) -> Option<u32> {
            self.objects.contains_key(&id).then_some(id)
        }

        fn system_objects(&self, _system: u32, _class_mask: u32) -> Vec<u32> {
            self.objects.keys().copied().collect()
        }

        fn attitude(&self, _from: u32, _to: u32) -> Option<f32> {
            None
        }

        fn attitude_symmetrical(&self, _from: u32, _to: u32) -> Option<(f32, f32)> {
            None
        }

        fn hardpoint(&self, _h: u32, _name: &[u8]) -> Option<(Vector, Matrix)> {
            None
        }

        fn motion(&self, _h: u32) -> Option<Box<dyn MotionModel + '_>> {
            None
        }

        fn system_of(&self, _h: u32) -> Option<u32> {
            Some(1)
        }

        fn command(&mut self, _h: u32, command: AiCommand<u32>) -> i32 {
            self.commands.push(command);
            0
        }

        fn scan_list(&mut self, h: u32, mask: u32, _keep: bool) -> Option<Vec<u32>> {
            (h == 7).then(|| {
                self.objects
                    .values()
                    .filter(|v| v.type_flags & mask != 0)
                    .map(|v| v.id)
                    .collect()
            })
        }

        fn find_equipment(&self, h: u32, type_mask: u32) -> Option<Vec<u16>> {
            (h == 7).then(|| {
                if type_mask == TRACTOR_TYPE {
                    vec![3]
                } else {
                    Vec::new()
                }
            })
        }

        fn equipment(&self, h: u32, id: u16) -> Option<EquipInfo> {
            (h == 7 && id == 3).then_some(EquipInfo {
                range: Some(1000.0),
                ..EquipInfo::default()
            })
        }

        fn manager_of(&self, h: u32) -> Option<(i32, [i32; 3])> {
            self.managers.get(&h).map(|m| (m.0, [0; 3]))
        }

        fn loot(&self, h: u32) -> Option<LootView> {
            self.loot.get(&h).copied()
        }

        fn cargo_hold(&self, h: u32) -> Option<(f64, f32)> {
            if h == 7 { self.hold } else { None }
        }

        fn loot_target_of(&self, h: u32) -> Option<u32> {
            self.managers.get(&h).and_then(|m| m.1)
        }
    }

    impl SpatialQueries for Field {
        fn find_ray_collisions(&self, _: u32, _: Vector, _: Vector, _: usize) -> Vec<RayContact> {
            Vec::new()
        }

        fn find_ray_intersect(
            &self,
            _: u32,
            _: u32,
            _: Vector,
            _: Vector,
            _: usize,
        ) -> Vec<RayContact> {
            Vec::new()
        }

        fn find_sphere_collisions(&self, _: u32, _: Vector, _: f32, _: usize) -> Vec<u32> {
            Vec::new()
        }
    }

    impl AiRand for Field {
        fn rand(&mut self) -> i32 {
            self.rand.rand()
        }
    }

    /// A bound loot behaviour and a manager state in attack state 2 with
    /// the given loot preference.
    fn setup(world: &mut Field, preference: i32) -> (LootBehavior, ManagerState) {
        let mut state = ManagerState::new(false);
        state.sys.targeting.attack_state = 2;
        state.personality.job.loot_preference = preference;
        state.sys.tractor.init(&*world, 7);
        let mut b = LootBehavior::new(ids_loot());
        Behavior::<Field>::bind(&mut b, Some(7));
        let mut cx = Cx::new(world, 7, &mut state);
        Behavior::<Field>::attach(&mut b, &mut cx);
        (b, state)
    }

    fn ids_loot() -> i32 {
        crate::ai::behavior::ids::LOOT
    }

    fn poll(b: &mut LootBehavior, world: &mut Field, state: &mut ManagerState) -> i32 {
        let mut cx = Cx::new(world, 7, state);
        Behavior::<Field>::poll(b, &mut cx, 0.0)
    }

    #[test]
    fn the_tractor_sets_the_reach() {
        let mut world = Field::new();
        let (b, state) = setup(&mut world, 1);
        assert_eq!(b.has_tractor, 1);
        assert_eq!(b.tractor_reach, 750.0);
        assert_eq!(state.sys.tractor.equip_id, 3);
    }

    #[test]
    fn without_a_preference_nothing_is_looted() {
        let mut world = Field::new();
        world.add_loot(20, 100.0, commodity(1));
        let (mut b, mut state) = setup(&mut world, 0);
        assert_eq!(poll(&mut b, &mut world, &mut state), 2);
        assert_eq!(b.target, None);
    }

    #[test]
    fn the_nearest_unclaimed_container_is_chosen() {
        let mut world = Field::new();
        world.add_loot(20, 300.0, commodity(1));
        world.add_loot(21, 100.0, commodity(1));
        world.add_loot(22, 200.0, commodity(3));
        // Another looter chases 21, a single unit: claimed.
        world.add_looter(30, 21);
        let (mut b, mut state) = setup(&mut world, 1);
        assert_eq!(poll(&mut b, &mut world, &mut state), 0);
        assert_eq!(b.target, Some(22));
        assert_eq!(b.directed, 0);
    }

    #[test]
    fn a_full_hold_or_another_attack_state_declines() {
        let mut world = Field::new();
        world.add_loot(20, 100.0, commodity(1));
        world.hold = Some((10.0, 100.0));
        let (mut b, mut state) = setup(&mut world, 1);
        assert_eq!(poll(&mut b, &mut world, &mut state), 2);
        world.hold = Some((50.0, 100.0));
        state.sys.targeting.attack_state = 1;
        assert_eq!(poll(&mut b, &mut world, &mut state), 2);
        assert_eq!(b.target, None);
    }

    #[test]
    fn the_walk_stops_at_an_excluded_kind() {
        let mut world = Field::new();
        // A repair kit first (the scan list is in id order here), which
        // the commodity preference excludes: the walk ends there.
        world.add_loot(
            20,
            300.0,
            LootView {
                contents_class: 0x1a,
                ..commodity(1)
            },
        );
        world.add_loot(21, 100.0, commodity(1));
        let (mut b, mut state) = setup(&mut world, 1);
        assert_eq!(poll(&mut b, &mut world, &mut state), 2);
        state.personality.job.loot_preference = 5;
        assert_eq!(poll(&mut b, &mut world, &mut state), 0);
        assert_eq!(b.target, Some(21));
    }

    #[test]
    fn the_update_ends_without_its_target_and_the_release_switches_the_tractor_off() {
        let mut world = Field::new();
        world.add_loot(20, 100.0, commodity(1));
        let (mut b, mut state) = setup(&mut world, 1);
        let mut cx = Cx::new(&mut world, 7, &mut state);
        assert_eq!(Behavior::<Field>::update(&mut b, &mut cx), 2);
        assert!(cx.state.sys.tractor.activate(&mut *cx.world, 7, Some(20)));
        // A second activation toward the same container is refused.
        assert!(!cx.state.sys.tractor.activate(&mut *cx.world, 7, Some(20)));
        Behavior::<Field>::disengage(&mut b, &mut cx);
        assert_eq!(cx.state.sys.tractor.watched, None);
        assert!(world.commands.contains(&AiCommand::Tractor {
            equipment: 3,
            target: 20
        }));
        assert!(world.commands.contains(&AiCommand::ActivateEquip {
            equipment: 3,
            on: false
        }));
    }

    #[test]
    fn the_fast_distance_is_close_to_the_distance() {
        let d = fast_distance(Vector::new(300.0, 400.0, 0.0), Vector::ZERO);
        assert!((d - 500.0).abs() < 1.0, "{d}");
    }
}
