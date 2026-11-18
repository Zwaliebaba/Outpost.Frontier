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
//! (the recorded slots 0 to 3 and 13; floats bit for bit).
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
use std::sync::Arc;

use game_sim::ai::manager::{BehaviorManager, ManagerState};
use game_sim::ai::personality::Personality;
use game_sim::ai::state_graph::StateGraphDb;
use game_sim::ai::world::{
    AiCommand, AiRand, AiWorld, MotionModel, ObjectView, RayContact, RecordedRand, SpatialQueries,
};
use neuron_core::math::{Matrix, Vector};
use support::ai_records::{
    Affect, AiTick, ManagerRecord, QueryRecord, read_affects, read_ai_ticks,
};
use support::records::{find_files, oracle_dir};

/// Mismatches listed in the summary.
const MAX_REPORTED: usize = 30;

/// The recorded `IObjAffect` slots (`design/sim-records.md`, `affect`).
const RECORDED_SLOTS: [u8; 5] = [0, 1, 2, 3, 13];

/// A world over one tick's snapshot: object ids are the handles.
struct ReplayWorld {
    objects: BTreeMap<u32, ObjectView>,
    attitudes: BTreeMap<u32, (f32, f32)>,
    queries: Vec<QueryRecord>,
    rand: RecordedRand,
    commands: Vec<AiCommand<u32>>,
    ship: u32,
}

impl ReplayWorld {
    fn new(t: &AiTick) -> Self {
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

impl AiWorld for ReplayWorld {
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

    fn motion(&self, _h: u32) -> Option<Box<dyn MotionModel + '_>> {
        None
    }

    fn system_of(&self, h: u32) -> Option<u32> {
        self.objects.contains_key(&h).then_some(0)
    }

    fn command(&mut self, h: u32, command: AiCommand<u32>) -> i32 {
        // The ship's own state follows its commands, as the reference's
        // reads after a command see it.
        if let Some(v) = self.objects.get_mut(&h) {
            match &command {
                AiCommand::SetThrottle(t) => v.throttle = Some(*t),
                AiCommand::SetAxisThrottle(a) => v.axis_throttle = Some(*a),
                AiCommand::SetNudge(n) => v.nudge = Some(*n),
                AiCommand::SetStrafe(s) => v.strafe = Some(*s),
                AiCommand::EngageCruise { engage, .. } => {
                    v.cruise = Some((*engage, v.cruise.is_some_and(|c| c.1)));
                }
                AiCommand::SetThrustersActive(on) => v.thrusters = Some(*on),
                _ => {}
            }
        }
        if h == self.ship {
            self.commands.push(command);
        }
        0
    }
}

impl SpatialQueries for ReplayWorld {
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

impl AiRand for ReplayWorld {
    fn rand(&mut self) -> i32 {
        self.rand.rand()
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
    m.sys.targets.clone_from(&t.targets);
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
    }
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
}

/// The first behaviour with a weight in the graph row of `current` that
/// is not ported.
fn unported_candidate(manager: &BehaviorManager<ReplayWorld>, current: i32) -> Option<i32> {
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
    world: &ReplayWorld,
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
    let mut managers: BTreeMap<u32, BehaviorManager<ReplayWorld>> = BTreeMap::new();
    let mut personalities: BTreeMap<u32, Personality> = BTreeMap::new();
    for t in &ticks {
        let Some(out) = &t.output else {
            continue;
        };
        totals.ticks += 1;
        if let Some(p) = t.personality.as_deref().and_then(Personality::from_bytes) {
            personalities.insert(t.id, p);
        }
        let mut world = ReplayWorld::new(t);
        let is_player = world.objects.get(&t.id).is_some_and(|v| v.is_player);
        let manager = managers.entry(t.id).or_insert_with(|| {
            BehaviorManager::from_state(ManagerState::new(is_player), Some(t.id))
        });
        restore(&mut manager.state, &t.input, t, graphs);
        if let Some(p) = personalities.get(&t.id) {
            manager.state.personality = *p;
        }
        manager.update_behavior_tick(&mut world, t.id, t.dt);
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
                    .filter(|a| a.id == t.id)
                    .map(recorded_affect)
                    .collect()
            })
            .unwrap_or_default();
        let rand_ok = world.rand.exhausted_exactly();
        if bits_equal(&ours, out) && our_commands == their_commands && rand_ok {
            totals.compared += 1;
            totals.matched += 1;
            if unported_candidate(manager, t.input.behavior).is_some() {
                totals.matched_with_stand_ins += 1;
            }
            continue;
        }
        // A mismatch after a poll of a behaviour that is not ported says
        // nothing about the manager.
        if let Some(c) = unported_candidate(manager, t.input.behavior) {
            *totals.skipped_candidate.entry(c).or_default() += 1;
            continue;
        }
        totals.compared += 1;
        totals.mismatched += 1;
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
    for r in &totals.reports {
        let _ = writeln!(summary, "{r}");
    }
    print!("{summary}");
    let _ = fs::write(dir.join("rust-behavior-replay.txt"), &summary);
    assert_eq!(totals.mismatched, 0, "{summary}");
}
