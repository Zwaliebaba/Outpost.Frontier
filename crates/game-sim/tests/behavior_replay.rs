//! Behaviour-tick replay (record 0003 point 7, work package P2.5): every
//! `IBehaviorManager::UpdateBehaviorTick` that `Oracle.exe sim` recorded
//! in `ai.jsonl` is run through the Rust behaviour manager
//! (`game_sim::ai::manager`) against a world rebuilt from the tick's
//! object snapshot, with the tick's `rand()` values fed back
//! (`RecordedRand`).
//!
//! Each object keeps one Rust manager for the whole run, so the state the
//! records do not hold (the behaviours' own fields, the thrust manager's
//! caches) carries over from the Rust side's previous tick; the recorded
//! fields (`ai_in`: behaviour, state graph, directive, mask, lock, dirty,
//! suppressed, the five timers, in-sights, f118, f11c, f120, the score
//! table, the target list and the personality) are set from the record
//! before each tick. After the tick the manager fields are compared with
//! `ai_out` bit for bit, the `rand()` draws by count, and the commands
//! with the `affect` records of `calls.jsonl` that carry the tick's `seq`
//! (the recorded slots 0 to 3, 13 and 18 to 21; the calls the behaviour
//! made, not the ones an `IObjAffect` body makes in turn; floats bit for
//! bit).
//!
//! The motion model the behaviours query (slots 50 to 64) is a pure
//! function of the ship's archetype and equipment, which the snapshot does
//! not hold: the replay spawns each scenario ship into a [`World`] over
//! `outdir/` at its scenario position and answers the motion queries of
//! the ship from it. The snapshot's own state (position, throttle, engine
//! and thruster status and so on) follows the commands of the tick as the
//! reference's `IObjAffect` bodies change it, nested calls included.
//!
//! A tick whose behaviour on entry or exit is not ported yet is counted as
//! skipped, by behaviour id, and so is a mismatching tick that polled a
//! behaviour that is not ported. `OUTPOST_ORACLE_DIR` names a folder above
//! the runs; without it the test returns with a notice. Its summary is
//! `rust-behavior-replay.txt` there.

mod support;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::Path;
use std::sync::Arc;

use game_sim::ai::manager::{BehaviorManager, ManagerState};
use game_sim::ai::personality::Personality;
use game_sim::ai::state_graph::StateGraphDb;
use game_sim::ai::world::{
    AiCommand, AiRand, AiWorld, MotionModel, ObjectView, RayContact, RecordedRand, SpatialQueries,
};
use game_sim::object::world::{ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::StepMode;
use support::ai_records::{
    Affect, AiTick, ManagerRecord, PathfindRecord, QueryRecord, read_affects, read_ai_ticks,
};
use support::records::{find_files, oracle_dir};

/// Mismatches listed in the summary.
const MAX_REPORTED: usize = 30;

/// The recorded `IObjAffect` slots (`design/sim-records.md`, `affect`).
const RECORDED_SLOTS: [u8; 9] = [0, 1, 2, 3, 13, 18, 19, 20, 21];

/// The scenario ships of one run in a world over `outdir/`, for their
/// motion models: the ship's scenario name to its handle.
struct MotionWorld {
    world: World,
    ships: BTreeMap<String, ObjHandle>,
}

impl MotionWorld {
    /// The ships of `CppSource/Oracle/scenarios/<scenario>.txt`, spawned
    /// where the scenario puts them.
    fn new(scenario: &str) -> Option<Self> {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../CppSource/Oracle/scenarios")
            .join(format!("{scenario}.txt"));
        let text = fs::read_to_string(path).ok()?;
        let mut world = support::world::world(WorldConfig {
            single_player: false,
            step_mode: StepMode::Multiplayer,
            server_physics: true,
            seed: 1,
        })?;
        let mut system = None;
        let mut ships = BTreeMap::new();
        for line in text.lines() {
            let t: Vec<&str> = line.split_whitespace().collect();
            match t.first().copied() {
                Some("system") if t.len() > 1 => {
                    let id = create_id(t[1].as_bytes());
                    world.load_system(id);
                    system = Some(id);
                }
                Some("ship") if t.len() >= 7 => {
                    let Some(system) = system else {
                        continue;
                    };
                    let num = |k: usize| t.get(k).and_then(|v| v.parse::<f32>().ok());
                    let (Some(x), Some(y), Some(z)) = (num(4), num(5), num(6)) else {
                        continue;
                    };
                    let spawn = ShipSpawn::new(
                        create_id(t[2].as_bytes()),
                        create_id(t[3].as_bytes()),
                        Vector::new(x, y, z),
                        Matrix::IDENTITY,
                    );
                    if let Ok(h) = world.spawn_ship(system, &spawn) {
                        ships.insert(t[1].to_owned(), h);
                    }
                }
                _ => {}
            }
        }
        Some(Self { world, ships })
    }
}

/// A world over one tick's snapshot: object ids are the handles.
struct ReplayWorld<'a> {
    objects: BTreeMap<u32, ObjectView>,
    attitudes: BTreeMap<u32, (f32, f32)>,
    queries: Vec<QueryRecord>,
    rand: RecordedRand,
    commands: Vec<AiCommand<u32>>,
    ship: u32,
    /// The ship's motion model: the motion world and the ship's handle.
    motion: Option<(&'a World, ObjHandle)>,
    /// `GAvoidIndexTableShuffled` of the run (the process).
    avoid_shuffled: bool,
    /// The ship's scan list: the target list the tick built, which the
    /// ship's next tick records on entry (`ai_in.targets`).
    scan: Vec<u32>,
}

impl<'a> ReplayWorld<'a> {
    fn new(
        t: &AiTick,
        scan: Vec<u32>,
        motion: Option<&'a MotionWorld>,
        avoid_shuffled: bool,
    ) -> Self {
        let motion = motion.and_then(|m| {
            let h = *m.ships.get(t.obj.as_deref()?)?;
            Some((&m.world, h))
        });
        Self {
            objects: t
                .objects
                .iter()
                .filter_map(|o| o.view.clone().map(|v| (o.id, v)))
                .collect(),
            attitudes: t
                .objects
                .iter()
                .filter_map(|o| o.attitude.map(|a| (o.id, a)))
                .collect(),
            queries: t.queries.clone(),
            rand: RecordedRand::new(t.rand.clone()),
            commands: Vec::new(),
            ship: t.id,
            motion,
            avoid_shuffled,
            scan,
        }
    }

    /// The ship's state after one of its `IObjAffect` calls, with the
    /// calls the reference's bodies make in turn (`Ship::set_strafe_dir`
    /// and `slot_54` switch the engine on, `slot_50` switches the
    /// thrusters off before engaging cruise; `slot_4c` will not switch an
    /// engine off while the ship strafes or thrusts).
    fn follow(v: &mut ObjectView, command: &AiCommand<u32>) {
        let engine_on = |v: &mut ObjectView| {
            if let Some(e) = &mut v.engine
                && !e.destroyed
            {
                e.active = true;
            }
        };
        match command {
            AiCommand::SetThrottle(t) => v.throttle = Some(*t),
            AiCommand::SetAxisThrottle(a) => v.axis_throttle = Some(*a),
            AiCommand::SetNudge(n) => v.nudge = Some(*n),
            AiCommand::SetStrafe(s) => {
                if v.strafe != Some(*s) {
                    engine_on(v);
                }
                v.strafe = Some(*s);
            }
            AiCommand::EngageCruise { engage, .. } => {
                let cruise = v.cruise.is_some_and(|c| c.0);
                if cruise != *engage && *engage {
                    v.thrusters = Some(false);
                }
                v.cruise = Some((*engage, v.cruise.is_some_and(|c| c.1)));
            }
            AiCommand::SetThrustersActive(on) => {
                if *on {
                    if v.cruise.is_some_and(|c| c.0) {
                        return;
                    }
                    engine_on(v);
                }
                v.thrusters = Some(*on);
            }
            AiCommand::ActivateEquip { equipment, on } => {
                let strafing = v.strafe.is_some_and(|s| s.value() != 0);
                let thrusting = v.thrusters.unwrap_or(false);
                if let Some(e) = &mut v.engine
                    && e.id == *equipment
                    && !e.destroyed
                    && (*on || !(strafing || thrusting))
                {
                    e.active = *on;
                }
            }
            AiCommand::SetTarget { target, subtarget } => {
                v.target = *target;
                v.subtarget = Some(*subtarget);
            }
            _ => {}
        }
    }

    fn query(&self, kind: &str, from: Vector, to: Vector, radius: f32) -> Option<&QueryRecord> {
        self.queries
            .iter()
            .find(|q| q.kind == kind && q.from == from && q.to == to && q.radius == radius)
    }

    fn contacts(q: Option<&QueryRecord>) -> Vec<RayContact> {
        q.map_or_else(Vec::new, |q| {
            q.hits
                .iter()
                .map(|&(object, point, normal)| RayContact {
                    object,
                    point,
                    normal,
                    distance: 0.0,
                })
                .collect()
        })
    }
}

impl AiWorld for ReplayWorld<'_> {
    type Handle = u32;

    fn inspect(&self, h: u32) -> Option<ObjectView> {
        self.objects.get(&h).cloned()
    }

    fn handle_of(&self, id: u32) -> Option<u32> {
        self.objects.contains_key(&id).then_some(id)
    }

    fn system_objects(&self, _system: u32, class_mask: u32) -> Vec<u32> {
        self.objects
            .values()
            .filter(|o| o.class & class_mask == class_mask)
            .map(|o| o.id)
            .collect()
    }

    fn attitude(&self, from: u32, to: u32) -> Option<f32> {
        if from != self.ship {
            return None;
        }
        self.attitudes.get(&to).map(|a| a.0)
    }

    fn attitude_symmetrical(&self, from: u32, to: u32) -> Option<(f32, f32)> {
        if from != self.ship {
            return None;
        }
        self.attitudes.get(&to).copied()
    }

    fn hardpoint(&self, _h: u32, _name: &[u8]) -> Option<(Vector, Matrix)> {
        None
    }

    fn motion(&self, h: u32) -> Option<Box<dyn MotionModel + '_>> {
        let (world, handle) = self.motion?;
        if h != self.ship {
            return None;
        }
        world.motion(handle)
    }

    fn scan_list(&self, h: u32) -> Option<Vec<u32>> {
        (h == self.ship).then(|| {
            self.scan
                .iter()
                .copied()
                .filter(|id| self.objects.contains_key(id))
                .collect()
        })
    }

    fn system_of(&self, h: u32) -> Option<u32> {
        self.objects.contains_key(&h).then_some(0)
    }

    fn command(&mut self, h: u32, command: AiCommand<u32>) -> i32 {
        // The ship's own state follows its commands, as the reference's
        // reads after a command see it.
        if let Some(v) = self.objects.get_mut(&h) {
            Self::follow(v, &command);
        }
        if h == self.ship {
            self.commands.push(command);
        }
        0
    }
}

impl SpatialQueries for ReplayWorld<'_> {
    fn find_ray_collisions(
        &self,
        _system: u32,
        from: Vector,
        to: Vector,
        _max: usize,
    ) -> Vec<RayContact> {
        Self::contacts(self.query("ray", from, to, 0.0))
    }

    fn find_ray_intersect(
        &self,
        _system: u32,
        _object: u32,
        from: Vector,
        to: Vector,
        _max: usize,
    ) -> Vec<RayContact> {
        Self::contacts(self.query("ray_object", from, to, 0.0))
    }

    fn find_sphere_collisions(
        &self,
        _system: u32,
        center: Vector,
        radius: f32,
        _max: usize,
    ) -> Vec<u32> {
        self.query("sphere", center, center, radius)
            .map_or_else(Vec::new, |q| q.hits.iter().map(|h| h.0).collect())
    }
}

impl AiRand for ReplayWorld<'_> {
    fn rand(&mut self) -> i32 {
        self.rand.rand()
    }

    fn avoid_table_shuffled(&self) -> bool {
        self.avoid_shuffled
    }

    fn set_avoid_table_shuffled(&mut self) {
        self.avoid_shuffled = true;
    }
}

fn int(v: u32) -> i32 {
    i32::from_ne_bytes(v.to_ne_bytes())
}

fn word(v: i32) -> u32 {
    u32::from_ne_bytes(v.to_ne_bytes())
}

/// Puts the recorded entry fields into the Rust manager.
fn restore(m: &mut ManagerState, r: &ManagerRecord, t: &AiTick, graphs: &StateGraphDb) {
    m.behavior = r.behavior;
    if m.state_graph_id() != r.state_graph {
        m.state_graph = graphs.get(r.state_graph).map(|g| Arc::new(g.clone()));
    }
    m.directive = [
        int(r.directive[0]),
        int(r.directive[1]),
        int(r.directive[2]),
    ];
    m.maneuver_mask = r.maneuver_mask;
    m.lock = r.lock;
    m.dirty = r.dirty;
    m.suppressed = r.suppressed;
    [
        m.timer_a,
        m.timer_b,
        m.timer_c,
        m.attack_override_timer,
        m.attack_override_period,
    ] = r.timers;
    m.in_sights = r.in_sights;
    m.f118 = r.f118;
    m.f11c = r.f11c;
    m.f120 = int(r.f120);
    m.scores = r.scores;
    m.sys.targets.ids.clone_from(&t.targets);
    if let Some(p) = &r.pathfind {
        let pf = &mut m.pathfind;
        [pf.timer_1e0, pf.timer_1e8, pf.timer_20c] = p.timers;
        [pf.period_1e4, pf.period_1ec, pf.period_210] = p.periods;
        pf.field_214 = p.record;
        pf.field_22c = p.goal;
        pf.field_23c = p.cap;
        pf.field_240 = p.kind;
        pf.cached_target_position = p.target;
        pf.target_id = p.target_id;
        pf.reaction.state = p.reaction;
        pf.avoidance.active = p.probe_active;
        pf.avoidance.has_contact = p.probe_contact;
    }
    if let Some(flags) = r.thrust {
        let th = &mut m.thrust;
        th.cruise_engaged = flags & 1 != 0;
        th.cruise_unobstructed = flags & 2 != 0;
        th.engine_active = flags & 4 != 0;
        th.auto_level = flags & 8 != 0;
        th.auto_avoidance = flags & 16 != 0;
    }
}

/// The pathfinder fields the records hold, from the Rust manager.
fn pathfind_snapshot(m: &ManagerState) -> PathfindRecord {
    let pf = &m.pathfind;
    PathfindRecord {
        timers: [pf.timer_1e0, pf.timer_1e8, pf.timer_20c],
        periods: [pf.period_1e4, pf.period_1ec, pf.period_210],
        record: pf.field_214,
        goal: pf.field_22c,
        cap: pf.field_23c,
        kind: pf.field_240,
        target: pf.cached_target_position,
        target_id: pf.target_id,
        reaction: pf.reaction.state,
        probe_active: pf.avoidance.active,
        probe_contact: pf.avoidance.has_contact,
    }
}

/// The thrust manager's flags as `thrust` writes them.
fn thrust_flags(m: &ManagerState) -> u32 {
    let th = &m.thrust;
    u32::from(th.cruise_engaged)
        | u32::from(th.cruise_unobstructed) << 1
        | u32::from(th.engine_active) << 2
        | u32::from(th.auto_level) << 3
        | u32::from(th.auto_avoidance) << 4
}

/// The fields of `ai_out` the Rust manager left.
fn snapshot(m: &ManagerState) -> ManagerRecord {
    ManagerRecord {
        behavior: m.behavior,
        state_graph: m.state_graph_id(),
        directive: [
            word(m.directive[0]),
            word(m.directive[1]),
            word(m.directive[2]),
        ],
        maneuver_mask: m.maneuver_mask,
        lock: m.lock,
        dirty: m.dirty,
        suppressed: m.suppressed,
        timers: m.timers(),
        in_sights: m.in_sights,
        f118: m.f118,
        f11c: m.f11c,
        f120: word(m.f120),
        scores: m.scores,
        pathfind: Some(pathfind_snapshot(m)),
        thrust: Some(thrust_flags(m)),
    }
}

fn vector_bits(v: Vector) -> [u32; 3] {
    [v.x.to_bits(), v.y.to_bits(), v.z.to_bits()]
}

/// The pathfinder fields bit for bit (only where the oracle wrote them).
fn pathfind_equal(ours: Option<&PathfindRecord>, theirs: Option<&PathfindRecord>) -> bool {
    let (Some(a), Some(b)) = (ours, theirs) else {
        return true;
    };
    a.timers.map(f32::to_bits) == b.timers.map(f32::to_bits)
        && a.periods.map(f32::to_bits) == b.periods.map(f32::to_bits)
        && a.record == b.record
        && vector_bits(a.goal) == vector_bits(b.goal)
        && a.cap.to_bits() == b.cap.to_bits()
        && a.kind == b.kind
        && vector_bits(a.target) == vector_bits(b.target)
        && a.target_id == b.target_id
        && a.reaction == b.reaction
        && a.probe_active == b.probe_active
        && a.probe_contact == b.probe_contact
}

fn bits_equal(a: &ManagerRecord, b: &ManagerRecord) -> bool {
    let timers = a
        .timers
        .iter()
        .zip(&b.timers)
        .all(|(x, y)| x.to_bits() == y.to_bits());
    let scores = a
        .scores
        .iter()
        .zip(&b.scores)
        .all(|(x, y)| x.weight.to_bits() == y.weight.to_bits() && x.behavior == y.behavior);
    a.behavior == b.behavior
        && a.state_graph == b.state_graph
        && a.directive == b.directive
        && a.maneuver_mask == b.maneuver_mask
        && a.lock == b.lock
        && a.dirty == b.dirty
        && a.suppressed == b.suppressed
        && timers
        && a.in_sights == b.in_sights
        && a.f118 == b.f118
        && a.f11c == b.f11c
        && a.f120 == b.f120
        && scores
        && pathfind_equal(a.pathfind.as_ref(), b.pathfind.as_ref())
        && (b.thrust.is_none() || a.thrust == b.thrust)
}

/// A command as the `affect` record writes it: slot, integer, float bits.
type AffectRow = (i32, i32, Option<Vec<u32>>);

fn as_affect(c: &AiCommand<u32>) -> Option<AffectRow> {
    if !RECORDED_SLOTS.contains(&c.slot()) {
        return None;
    }
    let v3 = |v: &Vector| Some(vec![v.x.to_bits(), v.y.to_bits(), v.z.to_bits()]);
    Some(match c {
        AiCommand::SetThrottle(t) => (0, 0, Some(vec![t.to_bits()])),
        AiCommand::SetAxisThrottle(v) => (1, 0, v3(v)),
        AiCommand::SetNudge(v) => (2, 0, v3(v)),
        AiCommand::SetStrafe(d) => (3, d.value(), None),
        AiCommand::FireGuns { equipment, target } => (
            13,
            i32::try_from(equipment.len()).unwrap_or(-1),
            target.as_ref().and_then(v3),
        ),
        AiCommand::SetTarget { target, subtarget } => (
            18,
            target.map_or(0, int),
            Some(vec![f32::from(*subtarget).to_bits()]),
        ),
        AiCommand::ActivateEquip { equipment, on } => (
            19,
            i32::from(*equipment),
            Some(vec![if *on { 1.0_f32 } else { 0.0 }.to_bits()]),
        ),
        AiCommand::EngageCruise { engage, instant } => (
            20,
            i32::from(*engage),
            Some(vec![if *instant { 1.0_f32 } else { 0.0 }.to_bits()]),
        ),
        AiCommand::SetThrustersActive(on) => (21, i32::from(*on), None),
        _ => return None,
    })
}

fn recorded_affect(a: &Affect) -> AffectRow {
    (
        a.slot,
        a.i,
        a.v.as_ref()
            .map(|v| v.iter().map(|x| x.to_bits()).collect()),
    )
}

fn describe(r: &ManagerRecord) -> String {
    format!(
        "behavior {} graph {} directive {:?} mask {} lock {} dirty {} suppressed {} timers {:08x?} in_sights {} f118 {} f11c {} f120 {}",
        r.behavior,
        r.state_graph,
        r.directive,
        r.maneuver_mask,
        r.lock,
        r.dirty,
        r.suppressed,
        r.timers.map(f32::to_bits),
        r.in_sights,
        r.f118,
        r.f11c,
        r.f120
    )
}

#[derive(Default)]
struct Totals {
    ticks: usize,
    compared: usize,
    matched: usize,
    matched_with_stand_ins: usize,
    mismatched: usize,
    skipped: BTreeMap<i32, usize>,
    skipped_candidate: BTreeMap<i32, usize>,
    rand_mismatch: usize,
    reports: Vec<String>,
    /// Each run's ship on its first tick, recorded and Rust.
    first_views: Vec<String>,
    /// Compared ticks by behaviour on entry: compared, matched.
    by_behavior: BTreeMap<i32, (usize, usize)>,
}

/// The first behaviour with a weight in the graph row of `current` that
/// is not ported.
fn unported_candidate(manager: &BehaviorManager<ReplayWorld<'_>>, current: i32) -> Option<i32> {
    let graph = manager.state.state_graph.as_ref()?;
    let row = graph.weights.get(usize::try_from(current).ok()?)?;
    row.iter()
        .enumerate()
        .filter(|(_, w)| **w != 0.0)
        .map(|(k, _)| i32::try_from(k).unwrap_or(-1))
        .find(|&k| !manager.is_ported(k))
}

fn report(
    path: &std::path::Path,
    t: &AiTick,
    out: &ManagerRecord,
    ours: &ManagerRecord,
    commands: (&[AffectRow], &[AffectRow]),
    world: &ReplayWorld<'_>,
) -> String {
    let mut r = format!(
        "{} seq {} tick {} id {} ({:?}):",
        path.display(),
        t.seq,
        t.tick,
        t.id,
        t.obj
    );
    if !bits_equal(ours, out) {
        let _ = write!(
            r,
            "\n  in     {}\n  oracle {}\n  rust   {}",
            describe(&t.input),
            describe(out),
            describe(ours)
        );
        if !pathfind_equal(ours.pathfind.as_ref(), out.pathfind.as_ref())
            || (out.thrust.is_some() && ours.thrust != out.thrust)
        {
            let _ = write!(
                r,
                "\n  pathfind in     {:?} thrust {:?}\n  pathfind oracle {:?} thrust {:?}\n  pathfind rust   {:?} thrust {:?}",
                t.input.pathfind,
                t.input.thrust,
                out.pathfind,
                out.thrust,
                ours.pathfind,
                ours.thrust
            );
        }
        if ours.scores != out.scores {
            let _ = write!(
                r,
                "\n  scores oracle {:?}\n  scores rust   {:?}",
                out.scores, ours.scores
            );
        }
    }
    let (theirs, ours_c) = commands;
    if theirs != ours_c {
        let _ = write!(
            r,
            "\n  commands oracle {theirs:08x?}\n  commands rust   {ours_c:08x?}"
        );
    }
    if !world.rand.exhausted_exactly() {
        let _ = write!(
            r,
            "\n  rand: {} recorded ({:?}), {} drawn, {} past the end",
            t.rand.len(),
            t.rand_mod,
            world.rand.consumed(),
            world.rand.overrun()
        );
    }
    r
}

fn replay_file(path: &std::path::Path, graphs: &StateGraphDb, totals: &mut Totals) {
    let ticks = read_ai_ticks(path);
    let affects = path
        .parent()
        .map(|p| read_affects(&p.join("calls.jsonl")))
        .unwrap_or_default();
    // `<oracle dir>/<scenario>/<run>/ai.jsonl`.
    let scenario = path
        .parent()
        .and_then(Path::parent)
        .and_then(Path::file_name)
        .and_then(|n| n.to_str())
        .unwrap_or("");
    let motion = MotionWorld::new(scenario);
    let mut avoid_shuffled = false;
    let mut managers: BTreeMap<u32, BehaviorManager<ReplayWorld<'_>>> = BTreeMap::new();
    let mut first_seen: BTreeMap<u32, ()> = BTreeMap::new();
    let mut personalities: BTreeMap<u32, Personality> = BTreeMap::new();
    for (index, t) in ticks.iter().enumerate() {
        // The list this tick builds is the one the ship's next tick
        // records on entry; the last tick falls back to its own entry.
        let scan = ticks[index + 1..]
            .iter()
            .find(|n| n.id == t.id)
            .map_or_else(|| t.targets.clone(), |n| n.targets.clone());
        let Some(out) = &t.output else {
            continue;
        };
        totals.ticks += 1;
        if let Some(p) = t.personality.as_deref().and_then(Personality::from_bytes) {
            personalities.insert(t.id, p);
        }
        let mut world = ReplayWorld::new(t, scan, motion.as_ref(), avoid_shuffled);
        let is_player = world.objects.get(&t.id).is_some_and(|v| v.is_player);
        if first_seen.insert(t.id, ()).is_none()
            && let Some(v) = world.objects.get(&t.id)
            && totals.first_views.len() < MAX_REPORTED
        {
            // What the recorder's `get_physical_radius` and box give for
            // the ship, against the Rust world's answer for the same ship.
            let rust = world
                .motion
                .and_then(|(w, h)| w.ai_view(h))
                .map(|r| (r.radius, r.center, r.position, r.extents));
            totals.first_views.push(format!(
                "{} id {} {:?}: timers {:08x?} pathfind {:?}; pos {:?} center {:?} radius {} extents {:?} engine {:?} thrusters {:?}; rust at spawn {:?}",
                path.display(),
                t.id,
                t.obj,
                t.input.timers.map(f32::to_bits),
                t.input.pathfind.as_ref().map(|p| p.timers.map(f32::to_bits)),
                v.position,
                v.center,
                v.radius,
                v.extents,
                v.engine,
                v.thrusters,
                rust
            ));
        }
        let manager = managers.entry(t.id).or_insert_with(|| {
            let mut m = BehaviorManager::from_state(ManagerState::new(is_player), Some(t.id));
            m.attach_all(&mut world, t.id);
            m.state.pathfind.owner_type = world.objects.get(&t.id).map_or(0, |v| v.type_flags);
            if let Some(v) = world.objects.get(&t.id) {
                m.state.pathfind.avoidance.attach_owner(v);
            }
            m.state.pathfind.avoidance.phys_system_id = 1;
            m
        });
        restore(&mut manager.state, &t.input, t, graphs);
        manager.state.stand_in_polls = 0;
        if let Some(p) = personalities.get(&t.id) {
            manager.state.personality = *p;
        }
        manager.update_behavior_tick(&mut world, t.id, t.dt);
        avoid_shuffled = world.avoid_shuffled;
        let ours = snapshot(&manager.state);
        if let Some(b) = [t.input.behavior, out.behavior]
            .into_iter()
            .find(|&b| b != -1 && !manager.is_ported(b))
        {
            *totals.skipped.entry(b).or_default() += 1;
            continue;
        }
        let our_commands: Vec<AffectRow> = world.commands.iter().filter_map(as_affect).collect();
        let their_commands: Vec<AffectRow> = affects
            .get(&t.seq)
            .map(|v| {
                v.iter()
                    .filter(|a| a.id == t.id && a.depth == 0)
                    .map(recorded_affect)
                    .collect()
            })
            .unwrap_or_default();
        let rand_ok = world.rand.exhausted_exactly();
        if bits_equal(&ours, out) && our_commands == their_commands && rand_ok {
            totals.compared += 1;
            totals.matched += 1;
            let e = totals.by_behavior.entry(t.input.behavior).or_default();
            e.0 += 1;
            e.1 += 1;
            if manager.state.stand_in_polls > 0 {
                totals.matched_with_stand_ins += 1;
            }
            continue;
        }
        // A mismatch after a poll of a behaviour that is not ported says
        // nothing about the manager.
        if manager.state.stand_in_polls > 0
            && let Some(c) = unported_candidate(manager, t.input.behavior)
        {
            *totals.skipped_candidate.entry(c).or_default() += 1;
            continue;
        }
        totals.compared += 1;
        totals.mismatched += 1;
        totals.by_behavior.entry(t.input.behavior).or_default().0 += 1;
        if !rand_ok {
            totals.rand_mismatch += 1;
        }
        if totals.reports.len() < MAX_REPORTED {
            totals.reports.push(report(
                path,
                t,
                out,
                &ours,
                (&their_commands, &our_commands),
                &world,
            ));
        }
    }
}

#[test]
fn behavior_ticks_against_the_oracle() {
    let Some(dir) = oracle_dir("behaviour ticks") else {
        return;
    };
    let Some(data) = support::world::game_data() else {
        println!("outdir/ is missing: no behaviour tick was replayed");
        return;
    };
    let mut totals = Totals::default();
    for file in find_files(&dir, "ai.jsonl") {
        replay_file(&file, &data.state_graphs, &mut totals);
    }
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "behaviour replay: {} ticks, {} compared, {} matched ({} of them with stand-in polls of unported candidates), {} mismatched ({} with a rand count mismatch); skipped by behaviour id {:?}; skipped after polling an unported behaviour {:?}",
        totals.ticks,
        totals.compared,
        totals.matched,
        totals.matched_with_stand_ins,
        totals.mismatched,
        totals.rand_mismatch,
        totals.skipped,
        totals.skipped_candidate
    );
    let _ = writeln!(
        summary,
        "compared ticks by behaviour on entry (compared, matched): {:?}",
        totals.by_behavior
    );
    for r in &totals.first_views {
        let _ = writeln!(summary, "first tick: {r}");
    }
    for r in &totals.reports {
        let _ = writeln!(summary, "{r}");
    }
    print!("{summary}");
    let _ = fs::write(dir.join("rust-behavior-replay.txt"), &summary);
    assert_eq!(totals.mismatched, 0, "{summary}");
}
