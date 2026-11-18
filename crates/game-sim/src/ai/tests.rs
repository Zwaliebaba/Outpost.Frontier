//! The behaviour manager and the simple behaviours over a one-ship world.

use neuron_core::math::{Matrix, Vector};
use neuron_core::rng::CrtRand;

use super::behavior::{DirectiveOp, DrasticEvadeOp, ids};
use super::manager::BehaviorManager;
use super::state_graph::{BEHAVIOR_COUNT, GraphType, StateGraph, StateGraphDb};
use super::world::{
    AiCommand, AiRand, AiWorld, MotionModel, ObjectView, RayContact, SpatialQueries, StrafeDir,
    class,
};

/// One ship that follows its commands.
struct OneShip {
    ship: ObjectView,
    rand: CrtRand,
    commands: Vec<AiCommand<u32>>,
}

impl OneShip {
    fn new() -> Self {
        Self {
            ship: ObjectView {
                id: 7,
                class: class::SHIP,
                type_flags: 0x1_0000,
                orientation: Matrix::IDENTITY,
                throttle: Some(0.0),
                axis_throttle: Some(Vector::ZERO),
                nudge: Some(Vector::ZERO),
                strafe: Some(StrafeDir::None),
                cruise: Some((false, false)),
                thrusters: Some(false),
                ..ObjectView::default()
            },
            rand: CrtRand::with_seed(1),
            commands: Vec::new(),
        }
    }
}

impl AiWorld for OneShip {
    type Handle = u32;

    fn inspect(&self, h: u32) -> Option<ObjectView> {
        (h == self.ship.id).then(|| self.ship.clone())
    }

    fn handle_of(&self, id: u32) -> Option<u32> {
        (id == self.ship.id).then_some(id)
    }

    fn system_objects(&self, _system: u32, _class_mask: u32) -> Vec<u32> {
        vec![self.ship.id]
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
        match &command {
            AiCommand::SetThrottle(t) => self.ship.throttle = Some(*t),
            AiCommand::SetNudge(n) => self.ship.nudge = Some(*n),
            AiCommand::EngageCruise { engage, .. } => self.ship.cruise = Some((*engage, false)),
            _ => {}
        }
        self.commands.push(command);
        0
    }
}

impl SpatialQueries for OneShip {
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

impl AiRand for OneShip {
    fn rand(&mut self) -> i32 {
        self.rand.rand()
    }
}

/// Graph 0 empty; graph 1 `FIGHTER` with idle → delay at weight 1.
fn graphs() -> StateGraphDb {
    let empty = StateGraph {
        id: 0,
        kind: GraphType::Leader,
        name: b"NOTHING".to_vec(),
        weights: [[0.0; BEHAVIOR_COUNT]; BEHAVIOR_COUNT],
    };
    let mut fighter = StateGraph {
        id: 1,
        name: b"FIGHTER".to_vec(),
        ..empty.clone()
    };
    fighter.weights[6][20] = 0.5;
    StateGraphDb {
        graphs: vec![empty, fighter],
    }
}

fn manager(world: &mut OneShip) -> BehaviorManager<OneShip> {
    let mut m = BehaviorManager::new(world, 7);
    let db = graphs();
    m.set_state_graph(world, 7, &db, 0, false);
    m.set_state_graph(world, 7, &db, 1, false);
    m
}

#[test]
fn construction_draws_the_scan_phase_then_three_phases_and_falls_back_to_idle() {
    let mut world = OneShip::new();
    let mut rand = CrtRand::with_seed(1);
    // `InitSystemManager` draws first: no guns, so only the scan phase.
    let scan = rand.rand();
    let (a, b, c) = (rand.rand(), rand.rand(), rand.rand());
    let m = manager(&mut world);
    let phase =
        |r: i32, p: f32| (f64::from(r as f32) * f64::from(1.0f32 / 32768.0) * f64::from(p)) as f32;
    assert_eq!(m.state.sys.guns.scan_timer, phase(scan, 1.0));
    assert_eq!(m.state.timer_a, phase(a, 3.0));
    assert_eq!(m.state.timer_b, phase(b, 0.2));
    assert_eq!(m.state.attack_override_timer, phase(c, 1.0));
    assert_eq!(m.state.behavior, ids::IDLE);
    assert_eq!(m.state.state_graph_id(), 1);
    assert_eq!(m.state.directive, [0, 0, 0]);
    assert_eq!(m.state.f120, ids::IDLE);
}

#[test]
fn idle_stops_the_ship_every_tick() {
    let mut world = OneShip::new();
    let mut m = manager(&mut world);
    world.commands.clear();
    m.update_behavior_tick(&mut world, 7, 1.0 / 30.0);
    // Idle resets timer A and re-evaluates every tick: the delay's poll
    // (1) keeps its weight; one draw picks it.
    assert_eq!(m.state.timer_a, 0.0);
    assert_eq!(m.state.scores[0].behavior, ids::DELAY);
    // A delay of 0 finishes in its first tick, back to idle.
    assert_eq!(m.state.behavior, ids::IDLE);
    assert_eq!(m.state.completed, vec![(ids::DELAY, 0)]);
    assert!(
        world
            .commands
            .iter()
            .all(|c| matches!(c, AiCommand::SetThrottle(t) if *t == 0.0)),
        "{:?}",
        world.commands
    );
}

#[test]
fn a_delay_directive_finishes() {
    let mut world = OneShip::new();
    let mut m = manager(&mut world);
    assert_eq!(
        m.submit_directive(&mut world, 7, &DirectiveOp::Delay(0.5), 0),
        1
    );
    assert_eq!(m.state.behavior, ids::DELAY);
    assert_eq!(m.state.directive, [1, 0, ids::DELAY]);
    assert_eq!(m.submit_directive(&mut world, 7, &DirectiveOp::Idle, 0), 2);
    for _ in 0..60 {
        m.update_behavior_tick(&mut world, 7, 1.0 / 30.0);
        if m.state.directive[0] == 0 {
            break;
        }
    }
    assert_eq!(m.state.directive[0], 0);
    assert_eq!(m.state.completed, vec![(ids::DELAY, 0)]);
    assert_eq!(m.state.f11c, -1);
}

#[test]
fn drastic_evade_nudges_for_its_duration() {
    let mut world = OneShip::new();
    let mut m = manager(&mut world);
    let op = DirectiveOp::DrasticEvade(DrasticEvadeOp {
        threat: 0,
        words_10: [1.0f32.to_bits(), (-1.0f32).to_bits(), 0],
        duration: 0.3,
        words_20: [0; 4],
    });
    assert_eq!(m.submit_directive(&mut world, 7, &op, 0), 1);
    world.commands.clear();
    for _ in 0..30 {
        m.update_behavior_tick(&mut world, 7, 0.1);
    }
    assert!(
        world
            .commands
            .contains(&AiCommand::SetNudge(Vector::new(1.0, -1.0, 0.0)))
    );
    assert!(world.commands.contains(&AiCommand::SetNudge(Vector::ZERO)));
}

#[test]
fn cancel_returns_to_the_fallback() {
    let mut world = OneShip::new();
    let mut m = manager(&mut world);
    assert_eq!(
        m.submit_directive(&mut world, 7, &DirectiveOp::Other(14), 0),
        1
    );
    assert_eq!(m.state.behavior, ids::LAUNCH);
    assert!(!m.is_ported(ids::LAUNCH));
    assert_eq!(
        m.submit_directive(&mut world, 7, &DirectiveOp::Cancel, 0),
        1
    );
    assert_eq!(m.state.behavior, ids::IDLE);
    assert_eq!(m.state.directive, [0, 0, 0]);
}

#[test]
fn maneuvers_and_sensitivity() {
    let mut world = OneShip::new();
    let mut m = manager(&mut world);
    assert!(!m.enable_maneuver(-1, false));
    assert!(m.enable_maneuver(3, false));
    assert_eq!(m.state.maneuver_mask, 1 << 4);
    // `2 << 31` is 0 in 32 bits: manoeuvre 31 has no bit.
    assert!(m.enable_maneuver(31, false));
    assert_eq!(m.state.maneuver_mask, 1 << 4);
    m.set_turn_sensitivity(2.0);
    assert_eq!(
        m.state.turn_sensitivity,
        (1.0f64 - f64::from(0.9f32)) as f32
    );
    assert_eq!(m.set_directive_priority(1), 3);
}
