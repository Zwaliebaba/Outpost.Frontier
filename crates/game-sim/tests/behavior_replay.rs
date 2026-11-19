//! Behaviour-tick replay (record 0003 point 7, work package P2.5): every
//! `IBehaviorManager::UpdateBehaviorTick` that `Oracle.exe sim` recorded
//! in `ai.jsonl` is run through the Rust behaviour manager
//! (`game_sim::ai::manager`) against a world rebuilt from the tick's
//! object snapshot, with the tick's `rand()` values fed back
//! (`RecordedRand`).
//!
//! Each object keeps one Rust manager for the whole run, so the state the
//! records do not hold (the behaviours' own fields but the face
//! behaviour's, the thrust manager's caches) carries over from the Rust
//! side's previous tick; the recorded fields (`ai_in`: behaviour, state
//! graph, directive, mask, lock, dirty, suppressed, the five timers,
//! in-sights, f118, f11c, f120, the score table, the pathfinder and thrust
//! fields, `flags_1c8`, the system manager's combat state, the face
//! behaviour's fields, the target list and the personality) are set from
//! the record before each tick. The ship's equipment, its lead fire
//! positions and the ships' targeted-enemy counts come from the snapshot. After the tick the manager fields are compared with
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

use game_sim::ai::behavior::{Cx, DirectiveOp};
use game_sim::ai::dock::DockBehavior;
use game_sim::ai::face::FaceBehavior;
use game_sim::ai::face_idea::FaceIdea;
use game_sim::ai::fire_control::GUN_POINTING_ANGLE;
use game_sim::ai::itl::InstantTradeLaneBehavior;
use game_sim::ai::launch::LaunchBehavior;
use game_sim::ai::manager::{BehaviorManager, ManagerState};
use game_sim::ai::personality::Personality;
use game_sim::ai::state_graph::StateGraphDb;
use game_sim::ai::steer::{
    build_orientation_basis, build_perpendicular_frame, steering_frame_from_relative_dir,
};
use game_sim::ai::world::{
    AI_SCAN_MASK, AiCommand, AiRand, AiWorld, DockHardpoints, EquipInfo, LaneStart, MotionModel,
    ObjectView, RayContact, RecordedRand, RequestAnswer, SpatialQueries,
};
use game_sim::object::world::{ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::physics::StepMode;
use support::ai_records::{
    Affect, AiTick, AnswerRecord, BehRecord, EquipRecord, FaceRecord, ManagerRecord,
    PathfindRecord, QueryRecord, SysRecord, read_affects, read_ai_ticks, read_names,
};
use support::records::{find_files, oracle_dir};

/// Mismatches listed in the summary.
const MAX_REPORTED: usize = 30;

/// The recorded `IObjAffect` slots (`design/sim-records.md`, `affect`).
const RECORDED_SLOTS: [u8; 15] = [0, 1, 2, 3, 13, 16, 17, 18, 19, 20, 21, 23, 24, 25, 26];

/// The scenario ships of one run in a world over `outdir/`, for their
/// motion models: the ship's scenario name to its handle.
struct MotionWorld {
    world: World,
    ships: BTreeMap<String, ObjHandle>,
    /// The scenario's commands: tick, object name, arguments.
    commands: Vec<(u32, String, Vec<String>)>,
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
        let commands = support::directives::commands(&text);
        Some(Self {
            world,
            ships,
            commands,
        })
    }
}

/// A world over one tick's snapshot: object ids are the handles.
struct ReplayWorld<'a> {
    objects: BTreeMap<u32, ObjectView>,
    attitudes: BTreeMap<u32, (f32, f32)>,
    queries: Vec<QueryRecord>,
    rand: RecordedRand,
    /// The DLL that drew each recorded value (`ai_out.rand_mod`).
    rand_mod: Vec<String>,
    commands: Vec<AiCommand<u32>>,
    ship: u32,
    /// The ship's motion model: the motion world and the ship's handle.
    motion: Option<(&'a World, ObjHandle)>,
    /// The other scenario ships' handles in the motion world, by id (the
    /// dodge idea asks the watched ship's speed).
    others: BTreeMap<u32, ObjHandle>,
    /// `GAvoidIndexTableShuffled` of the run (the process).
    avoid_shuffled: bool,
    /// The ship's scan list: the target list the tick built, which the
    /// ship's next tick records on entry (`ai_in.targets`).
    scan: Vec<u32>,
    /// The ship's items as the snapshot read them (`ai_obj.equip`).
    equip: Option<Vec<EquipRecord>>,
    /// The ship's `CShip::get_tgt_lead_fire_pos` (`ai_obj.lead`).
    lead: Option<Option<Vector>>,
    /// `CShip::get_targeted_enemy_count` of the snapshot's ships.
    targeted: BTreeMap<u32, i32>,
    /// The angles the snapshot asked `is_pointing_at` with: the gun angle,
    /// the gun groups' wide and missile cone angles.
    pointing_angles: [f32; 3],
    /// The equipment queries of the tick the snapshot cannot answer.
    unanswered: Vec<String>,
    /// The world answers of the tick (`ai_answer`), each used once.
    answers: Vec<AnswerRecord>,
    /// Which of them have been used.
    used: Vec<bool>,
    /// The ship's `get_hardpoint("hpmount")` (`ai_obj.hpmount`).
    hpmount: Option<(Vector, Matrix)>,
    /// The other AI ships' managers as their last recorded exit left
    /// them: the current behaviour and the directive (`CEqObj::
    /// get_behavior_interface`; a formation leader's directive is read by
    /// its wingmen).
    managers: BTreeMap<u32, (i32, [i32; 3])>,
}

impl<'a> ReplayWorld<'a> {
    fn new(
        t: &AiTick,
        scan: Vec<u32>,
        motion: Option<&'a MotionWorld>,
        avoid_shuffled: bool,
        names: &BTreeMap<u32, String>,
    ) -> Self {
        // The other scenario ships: the AI ships by their records, and the
        // snapshot's named objects (a ship without a manager, such as the
        // guided ship of a guide directive).
        let others = motion.map_or_else(BTreeMap::new, |m| {
            names
                .iter()
                .map(|(id, name)| (*id, name.as_str()))
                .chain(
                    t.objects
                        .iter()
                        .filter_map(|o| Some((o.id, o.name.as_deref()?))),
                )
                .filter(|(id, _)| *id != t.id)
                .filter_map(|(id, name)| m.ships.get(name).map(|h| (id, *h)))
                .collect()
        });
        let motion = motion.and_then(|m| {
            let h = *m.ships.get(t.obj.as_deref()?)?;
            Some((&m.world, h))
        });
        let own = t.objects.iter().find(|o| o.id == t.id);
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
            rand_mod: t.rand_mod.clone(),
            commands: Vec::new(),
            ship: t.id,
            motion,
            others,
            avoid_shuffled,
            scan,
            equip: own.and_then(|o| o.equip.clone()),
            lead: own.and_then(|o| o.lead),
            targeted: t
                .objects
                .iter()
                .filter_map(|o| o.targeted_enemies.map(|n| (o.id, n)))
                .collect(),
            answers: t.answers.clone(),
            used: vec![false; t.answers.len()],
            hpmount: own.and_then(|o| o.hpmount),
            pointing_angles: [
                GUN_POINTING_ANGLE,
                t.input.sys.as_ref().map_or(0.0, |s| s.guns.wide_cone_angle),
                t.input
                    .sys
                    .as_ref()
                    .map_or(0.0, |s| s.guns.missile_launch_cone_angle),
            ],
            unanswered: Vec::new(),
            managers: BTreeMap::new(),
        }
    }

    /// The first unused recorded answer of a kind that `pick` accepts,
    /// marked used.
    fn take_answer(
        &mut self,
        kind: &str,
        pick: impl Fn(&AnswerRecord) -> bool,
    ) -> Option<AnswerRecord> {
        let k = self
            .answers
            .iter()
            .enumerate()
            .position(|(k, a)| !self.used[k] && a.kind == kind && pick(a))?;
        self.used[k] = true;
        Some(self.answers[k].clone())
    }

    /// The answers of a batch outside a tick, in place of the tick's.
    fn set_answers(&mut self, answers: Vec<AnswerRecord>) -> (Vec<AnswerRecord>, Vec<bool>) {
        let used = vec![false; answers.len()];
        (
            std::mem::replace(&mut self.answers, answers),
            std::mem::replace(&mut self.used, used),
        )
    }

    /// The ship's recorded item with an id.
    fn item(&self, h: u32, id: u16) -> Option<&EquipRecord> {
        if h != self.ship {
            return None;
        }
        self.equip.as_ref()?.iter().find(|e| e.id == id)
    }

    /// The ship's state after one of its `IObjAffect` calls, with the
    /// calls the reference's bodies make in turn (`CShip::set_throttle`,
    /// `Ship::set_strafe_dir` and `slot_54` switch the engine on, `slot_50` switches the
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
            AiCommand::SetThrottle(t) => {
                // `CShip::set_throttle` activates an inactive first engine.
                engine_on(v);
                v.throttle = Some(*t);
            }
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

    /// The ship's `hpmount` as the snapshot read it.
    fn hardpoint(&self, h: u32, name: &[u8]) -> Option<(Vector, Matrix)> {
        if h != self.ship || name != b"hpmount" {
            return None;
        }
        self.hpmount
    }

    /// The recorded answer of `get_dock_hardpoints` for the object and
    /// sphere (the caller's buffers as they were after the call).
    fn dock_hardpoints(&mut self, h: u32, index: i32) -> DockHardpoints {
        let Some(a) = self.take_answer("dock_hardpoints", |a| {
            a.id == h && a.ints.first() == Some(&index)
        }) else {
            self.unanswered
                .push(format!("dock_hardpoints({h}, {index})"));
            return DockHardpoints::failed(-5);
        };
        let flag = |k: usize| a.ints.get(k).is_some_and(|&v| v != 0);
        let xform = |k: usize| {
            let f = a.floats.get(k * 12..k * 12 + 12)?;
            Some(Transform::new(
                Matrix::from_rows([[f[0], f[1], f[2]], [f[3], f[4], f[5]], [f[6], f[7], f[8]]]),
                Vector::new(f[9], f[10], f[11]),
            ))
        };
        DockHardpoints {
            result: a.ints.get(1).copied().unwrap_or(-5),
            terminal: a.ints.get(2).copied(),
            mount: if flag(3) { xform(0) } else { None },
            point1: if flag(4) { xform(1) } else { None },
            point2: if flag(5) { xform(2) } else { None },
            radius: if flag(6) {
                a.floats.get(36).copied()
            } else {
                None
            },
        }
    }

    /// The recorded answer of `get_lane_start` the ship got for the ring.
    fn lane_start(&mut self, h: u32, ring: Option<u32>, lane: Option<u32>) -> LaneStart<u32> {
        let ring_id = ring.map_or(0, int);
        let Some(a) = self.take_answer("lane_start", |a| {
            a.id == h && a.ints.first() == Some(&ring_id)
        }) else {
            self.unanswered.push(format!("lane_start({h}, {ring:?})"));
            return LaneStart {
                result: -4,
                lane,
                start: None,
                direction: None,
            };
        };
        let v = |k: usize| {
            let f = a.floats.get(k..k + 3)?;
            Some(Vector::new(f[0], f[1], f[2]))
        };
        LaneStart {
            result: a.ints.get(3).copied().unwrap_or(-4),
            lane: a.ints.get(2).map(|&id| word(id)).filter(|&id| id != 0),
            start: v(0),
            direction: v(3),
        }
    }

    /// The zone properties the oracle's probe of the same sphere found
    /// (`zone_props`); 0 when the tick made no such probe.
    fn sphere_zone_properties(&self, _system: u32, center: Vector, radius: f32) -> u32 {
        self.answers
            .iter()
            .find(|a| {
                a.kind == "zone_props"
                    && a.floats.len() == 4
                    && vector_bits(Vector::new(a.floats[0], a.floats[1], a.floats[2]))
                        == vector_bits(center)
                    && a.floats[3].to_bits() == radius.to_bits()
            })
            .and_then(|a| a.ints.first())
            .map_or(0, |&v| word(v))
    }

    /// The server's answers to the ship's request during the tick.
    fn take_request_answers(&mut self, h: u32) -> Vec<RequestAnswer> {
        let mut out = Vec::new();
        while let Some(a) = self.take_answer("request", |a| a.id == h) {
            out.push(RequestAnswer {
                kind: a.ints.first().copied().unwrap_or(0),
                response: a.ints.get(1).copied().unwrap_or(0),
                arg: a.ints.get(2).copied().unwrap_or(-1),
            });
        }
        out
    }

    fn motion(&self, h: u32) -> Option<Box<dyn MotionModel + '_>> {
        let (world, handle) = self.motion?;
        if h != self.ship {
            return world.motion(*self.others.get(&h)?);
        }
        world.motion(handle)
    }

    /// The target list's mask answers the list the tick built; another
    /// mask (the obstacle search) answers every object of the snapshot but
    /// the ship that is targetable and not dying, by id: the scanner's
    /// frame scan covers every type within the radar range, and the
    /// snapshot holds every ship, solar and loot within 5 km.
    fn scan_list(&mut self, h: u32, mask: u32, _keep: bool) -> Option<Vec<u32>> {
        if h != self.ship {
            return None;
        }
        if mask != AI_SCAN_MASK {
            return Some(
                self.objects
                    .values()
                    .filter(|v| v.id != self.ship && v.is_targetable && !v.is_dying)
                    .map(|v| v.id)
                    .collect(),
            );
        }
        Some(
            self.scan
                .iter()
                .copied()
                .filter(|id| self.objects.contains_key(id))
                .collect(),
        )
    }

    fn system_of(&self, h: u32) -> Option<u32> {
        self.objects.contains_key(&h).then_some(0)
    }

    /// The ids the snapshot's `find_equipment` of the mask found.
    fn find_equipment(&self, h: u32, type_mask: u32) -> Option<Vec<u16>> {
        if h != self.ship {
            return None;
        }
        let equip = self.equip.as_ref()?;
        Some(
            equip
                .iter()
                .filter(|e| e.type_mask == type_mask)
                .map(|e| e.id)
                .collect(),
        )
    }

    /// The snapshot's status and values of the item; `None` when its
    /// status call failed.
    fn equipment(&self, h: u32, id: u16) -> Option<EquipInfo> {
        let e = self.item(h, id)?;
        let (flags, equip_flags) = e.status?;
        let [munition, auto_turret, seeker, range, target] = e.vals;
        Some(EquipInfo {
            flags,
            equip_flags: (equip_flags & 0xff) as u8,
            munition_kind: munition.map(int),
            auto_turret: auto_turret.map(|w| w & 0xff != 0),
            seeker: seeker.map(|w| (w & 0xff) as u8),
            range: range.map(f32::from_bits),
            target_id: target,
            barrel: e.barrel,
        })
    }

    /// The snapshot's `can_point_at` of the item toward a candidate of the
    /// target list.
    fn can_point_at(&self, h: u32, id: u16, point: Vector, angle: f32) -> bool {
        let candidate = self
            .objects
            .values()
            .find(|v| vector_bits(v.position) == vector_bits(point))
            .map(|v| v.id);
        let answer = self.item(h, id).and_then(|e| {
            let c = candidate?;
            e.can_point.iter().find(|(k, _)| *k == c).map(|(_, a)| *a)
        });
        match answer {
            Some(a) if angle.to_bits() == 0.3_f32.to_bits() => a,
            _ => false,
        }
    }

    /// The snapshot's `is_pointing_at` of the item at one of the angles it
    /// asked with.
    fn is_pointing_at(&mut self, h: u32, id: u16, angle: f32) -> bool {
        let k = self
            .pointing_angles
            .iter()
            .position(|a| a.to_bits() == angle.to_bits());
        match (self.item(h, id), k) {
            (Some(e), Some(k)) => e.pointing[k],
            _ => {
                self.unanswered
                    .push(format!("is_pointing_at({id}, {:08x})", angle.to_bits()));
                false
            }
        }
    }

    /// The ship's own lead (`CShip::get_tgt_lead_fire_pos`), or an item's
    /// (`get_tgt_lead_fire_pos(id)`), as the snapshot read them.
    fn lead_fire_position(&mut self, h: u32, subtarget: Option<u16>) -> Option<Vector> {
        if h != self.ship {
            self.unanswered
                .push(format!("lead_fire_position({h}, {subtarget:?})"));
            return None;
        }
        match subtarget {
            None => self.lead.flatten(),
            Some(id) => {
                if self.item(h, id).is_none() {
                    self.unanswered
                        .push(format!("lead_fire_position({h}, Some({id}))"));
                }
                self.item(h, id).and_then(|e| e.lead)
            }
        }
    }

    fn targeted_enemy_count(&self, h: u32) -> i32 {
        self.targeted.get(&h).copied().unwrap_or(0)
    }

    fn manager_of(&self, h: u32) -> Option<(i32, [i32; 3])> {
        if h == self.ship {
            return None;
        }
        self.managers.get(&h).copied()
    }

    fn command(&mut self, h: u32, command: AiCommand<u32>) -> i32 {
        // The ship's own state follows its commands, as the reference's
        // reads after a command see it.
        if let Some(v) = self.objects.get_mut(&h) {
            Self::follow(v, &command);
        }
        // The server's side of a request (Content's dock queue and its
        // conversations) draws before it answers: those values are not the
        // AI's.
        if let AiCommand::Request { .. } = &command {
            while self
                .rand_mod
                .get(self.rand.consumed())
                .is_some_and(|m| m != "Common.dll")
            {
                self.rand.rand();
            }
        }
        let result = if let AiCommand::GoTradeLane { .. } = &command {
            self.take_answer("go_tradelane", |a| a.id == h)
                .and_then(|a| a.ints.get(3).copied())
                .unwrap_or(0)
        } else {
            0
        };
        if h == self.ship {
            self.commands.push(command);
        }
        result
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
        if let Some([a, b, c]) = p.locks {
            pf.flag_245 = a;
            pf.field_24c = b;
            pf.field_254 = c;
        }
    }
    if let Some(flags) = r.thrust {
        let th = &mut m.thrust;
        th.cruise_engaged = flags & 1 != 0;
        th.cruise_unobstructed = flags & 2 != 0;
        th.engine_active = flags & 4 != 0;
        th.auto_level = flags & 8 != 0;
        th.auto_avoidance = flags & 16 != 0;
    }
    if let Some(flags) = r.flags_1c8 {
        m.flags_1c8 = flags;
    }
    if let Some(sys) = &r.sys {
        let clamp = m.sys.targeting.enemy_clamp;
        m.sys.targeting = sys.targeting.clone();
        m.sys.targeting.enemy_clamp = clamp;
        m.sys.timers = sys.timers.clone();
        let player = m.sys.guns.cm.player;
        m.sys.guns = sys.guns.clone();
        m.sys.guns.cm.player = player;
    }
}

/// Puts the recorded face fields into the Rust face behaviour.
fn restore_face(manager: &mut BehaviorManager<ReplayWorld<'_>>, r: &FaceRecord) {
    let Some(face) = manager
        .behavior_mut(15)
        .and_then(|b| b.as_any_mut())
        .and_then(|a| a.downcast_mut::<FaceBehavior>())
    else {
        return;
    };
    {
        let base = game_sim::ai::behavior::Behavior::<ReplayWorld<'_>>::base_mut(face);
        base.mode = r.mode;
        base.result = r.result;
        base.tick_delta = r.dt;
        base.flags = r.flags;
        base.flag29 = r.flag29;
    }
    face.kind = r.kind;
    face.watcher_30 = (r.watched != 0).then_some(r.watched);
    face.orient_38 = r.orient;
    face.target_5c = r.target;
    face.flag_1b4 = r.flag_1b4;
    face.steer = r.steer;
    face.flag_1c4 = r.flag_1c4;
    face.flag_1c5 = r.flag_1c5;
    face.field_1c8 = r.field_1c8;
    face.field_1cc = r.field_1cc;
    face.field_1d0 = r.field_1d0;
    face.flag_1d4 = r.flag_1d4;
    face.field_1d8 = r.field_1d8;
}

/// Whether a matrix's rows are unit vectors (within 1e-3): a rotation, as
/// every orientation the face behaviour writes is.
fn is_rotation(m: &neuron_core::math::Matrix) -> bool {
    m.m.iter().all(|r| {
        let n = (f64::from(r[0]) * f64::from(r[0])
            + f64::from(r[1]) * f64::from(r[1])
            + f64::from(r[2]) * f64::from(r[2]))
        .sqrt();
        (n - 1.0).abs() < 1e-3
    })
}

/// The Rust face behaviour's fields as `face` writes them.
fn face_snapshot(manager: &mut BehaviorManager<ReplayWorld<'_>>) -> Option<FaceRecord> {
    let face = manager
        .behavior_mut(15)?
        .as_any_mut()?
        .downcast_mut::<FaceBehavior>()?;
    let base = game_sim::ai::behavior::Behavior::<ReplayWorld<'_>>::base(face).clone();
    Some(FaceRecord {
        mode: base.mode,
        result: base.result,
        dt: base.tick_delta,
        flags: base.flags,
        flag29: base.flag29,
        kind: face.kind,
        watched: face.watcher_30.unwrap_or(0),
        orient: face.orient_38,
        target: face.target_5c,
        flag_1b4: face.flag_1b4,
        steer: face.steer,
        flag_1c4: face.flag_1c4,
        flag_1c5: face.flag_1c5,
        field_1c8: face.field_1c8,
        field_1cc: face.field_1cc,
        field_1d0: face.field_1d0,
        flag_1d4: face.flag_1d4,
        field_1d8: face.field_1d8,
    })
}

/// The recorded combat state the Rust manager left, as differences:
/// `flags_1c8`, the target manager, the timer table and the gun groups
/// (floats by their bits, through `Debug`), and the face behaviour's
/// fields while it was current on exit.
fn state_diff(m: &ManagerState, face: Option<&FaceRecord>, theirs: &ManagerRecord) -> Vec<String> {
    let mut out = Vec::new();
    if let Some(flags) = theirs.flags_1c8
        && flags != m.flags_1c8
    {
        out.push(format!("flags_1c8 oracle {flags} rust {}", m.flags_1c8));
    }
    if let Some(sys) = &theirs.sys {
        let mut targeting = m.sys.targeting.clone();
        targeting.enemy_clamp = sys.targeting.enemy_clamp;
        let mut guns = m.sys.guns.clone();
        guns.cm.player = sys.guns.cm.player;
        let ours = SysRecord {
            targeting,
            timers: m.sys.timers.clone(),
            guns,
        };
        for (name, a, b) in [
            (
                "targeting",
                format!("{:?}", sys.targeting),
                format!("{:?}", ours.targeting),
            ),
            (
                "timers",
                format!("{:?}", sys.timers),
                format!("{:?}", ours.timers),
            ),
            (
                "guns",
                format!("{:?}", sys.guns),
                format!("{:?}", ours.guns),
            ),
        ] {
            if a != b {
                out.push(format!("{name}\n    oracle {a}\n    rust   {b}"));
            }
        }
    }
    if let (Some(a), Some(b)) = (&theirs.face, face) {
        let (a, b) = (format!("{a:?}"), format!("{b:?}"));
        if a != b {
            out.push(format!("face\n    oracle {a}\n    rust   {b}"));
        }
    }
    out
}

/// The dock, launch or trade-lane behaviour's fields in the layout of
/// `beh7`, `beh8` and `beh9` (`design/sim-records.md`).
fn beh_snapshot(manager: &mut BehaviorManager<ReplayWorld<'_>>, kind: i32) -> Option<BehRecord> {
    let b = manager.behavior_mut(kind)?;
    let base = b.base().clone();
    let head = vec![
        base.mode,
        base.result,
        i32::from(base.flags),
        i32::from(base.flag29),
    ];
    let any = b.as_any_mut()?;
    let id = |v: Option<u32>| v.map_or(0, int);
    let vec3 = |v: Vector| [v.x, v.y, v.z];
    let mat = |m: &Matrix| m.m.concat();
    let mut ints = head;
    let mut floats = Vec::new();
    if let Some(d) = any.downcast_mut::<DockBehavior>() {
        ints.extend([
            d.mode,
            int(d.type_flags),
            d.field_330,
            i32::from(d.cleared_334),
            d.field_43c,
            d.sub_440,
            i32::from(d.abort_398),
            i32::from(d.flag_399),
            i32::from(d.flag_39a),
            i32::from(d.flag_39b),
            i32::from(d.combat_344),
            id(d.watched),
            id(d.target_ship),
            id(d.prox.watched),
            int(d.prox.zone_flags),
            int(d.prox.saved_flags),
            i32::from(d.pilot),
        ]);
        floats.extend([
            d.field_338,
            d.field_33c,
            d.timer_340,
            d.field_408,
            d.max_speed_46c,
            d.prox.near,
            d.prox.far,
            d.prox.ratio,
        ]);
        for v in [
            d.facing,
            d.intercept,
            d.dest,
            d.saved,
            d.offset_430,
            d.field_444,
        ] {
            floats.extend(vec3(v));
        }
        for m in [&d.orient_3a8, &d.orient_3d8, &d.orient_40c] {
            floats.extend(mat(m));
        }
    } else if let Some(l) = any.downcast_mut::<LaunchBehavior>() {
        ints.extend([
            l.mode,
            id(l.watched),
            int(l.system),
            int(l.target_type),
            l.index,
            i32::from(l.straight),
            i32::from(l.berth),
            i32::from(l.turn_about),
        ]);
        floats.extend([
            base.tick_delta,
            l.wait,
            l.waited,
            l.elapsed,
            l.limit,
            l.throttle_22c,
        ]);
        for v in [l.mount_offset, l.point2, l.point1, l.mount] {
            floats.extend(vec3(v));
        }
        for m in [&l.launch_basis, &l.basis2, &l.basis3] {
            floats.extend(mat(m));
        }
    } else {
        let t = any.downcast_mut::<InstantTradeLaneBehavior>()?;
        ints.extend([
            t.state,
            id(t.entrance),
            id(t.lane),
            i32::from(t.in_lane),
            i32::from(t.flag_55),
            i32::from(t.flag_56),
        ]);
        floats.push(t.speed);
        floats.extend(vec3(t.lane_start));
        floats.extend(vec3(t.lane_end));
    }
    Some(BehRecord { kind, ints, floats })
}

/// The fields of a `beh` record the replay cannot compare: the launch
/// behaviour's system (the replay world has no system ids).
fn beh_skipped(kind: i32, k: usize) -> bool {
    kind == 8 && k == 6
}

/// The differences between the recorded and the Rust behaviour fields.
fn beh_diff(theirs: &BehRecord, ours: Option<&BehRecord>) -> Option<String> {
    let Some(ours) = ours else {
        return Some(format!(
            "beh{} oracle {:?}, rust none",
            theirs.kind, theirs.ints
        ));
    };
    let ints: Vec<usize> = (0..theirs.ints.len().max(ours.ints.len()))
        .filter(|&k| !beh_skipped(theirs.kind, k) && theirs.ints.get(k) != ours.ints.get(k))
        .collect();
    let floats: Vec<usize> = (0..theirs.floats.len().max(ours.floats.len()))
        .filter(|&k| {
            theirs.floats.get(k).map(|v| v.to_bits()) != ours.floats.get(k).map(|v| v.to_bits())
        })
        .collect();
    if ints.is_empty() && floats.is_empty() {
        return None;
    }
    Some(format!(
        "beh{}: ints {:?} oracle {:?} rust {:?}; floats {:?} oracle {:08x?} rust {:08x?}",
        theirs.kind,
        ints,
        theirs.ints,
        ours.ints,
        floats,
        floats
            .iter()
            .filter_map(|&k| theirs.floats.get(k).map(|v| v.to_bits()))
            .collect::<Vec<_>>(),
        floats
            .iter()
            .filter_map(|&k| ours.floats.get(k).map(|v| v.to_bits()))
            .collect::<Vec<_>>()
    ))
}

/// Puts the recorded dock fields into the Rust dock behaviour (as the
/// face behaviour's): its constructor leaves most of them as the heap
/// held them, and `SetContext` computed the `hpmount` offset before the
/// first tick.
fn restore_dock(manager: &mut BehaviorManager<ReplayWorld<'_>>, r: &BehRecord) {
    if r.kind != 7 || r.ints.len() < 21 || r.floats.len() < 53 {
        return;
    }
    let Some(d) = manager
        .behavior_mut(7)
        .and_then(|b| b.as_any_mut())
        .and_then(|a| a.downcast_mut::<DockBehavior>())
    else {
        return;
    };
    let i = &r.ints;
    let f = &r.floats;
    {
        let base = game_sim::ai::behavior::Behavior::<ReplayWorld<'_>>::base_mut(d);
        base.mode = i[0];
        base.result = i[1];
        base.flags = u8::try_from(i[2] & 0xff).unwrap_or(0);
        base.flag29 = u8::try_from(i[3] & 0xff).unwrap_or(0);
    }
    let byte = |v: i32| u8::try_from(v & 0xff).unwrap_or(0);
    let id = |v: i32| (v != 0).then(|| word(v));
    d.mode = i[4];
    d.type_flags = word(i[5]);
    d.field_330 = i[6];
    d.cleared_334 = byte(i[7]);
    d.field_43c = i[8];
    d.sub_440 = i[9];
    d.abort_398 = byte(i[10]);
    d.flag_399 = byte(i[11]);
    d.flag_39a = byte(i[12]);
    d.flag_39b = byte(i[13]);
    d.combat_344 = byte(i[14]);
    d.watched = id(i[15]);
    d.target_ship = id(i[16]);
    d.prox.watched = id(i[17]);
    d.prox.zone_flags = word(i[18]);
    d.prox.saved_flags = word(i[19]);
    d.pilot = i[20] != 0;
    d.field_338 = f[0];
    d.field_33c = f[1];
    d.timer_340 = f[2];
    d.field_408 = f[3];
    d.max_speed_46c = f[4];
    d.prox.near = f[5];
    d.prox.far = f[6];
    d.prox.ratio = f[7];
    let v = |k: usize| Vector::new(f[k], f[k + 1], f[k + 2]);
    let m = |k: usize| {
        Matrix::from_rows([
            [f[k], f[k + 1], f[k + 2]],
            [f[k + 3], f[k + 4], f[k + 5]],
            [f[k + 6], f[k + 7], f[k + 8]],
        ])
    };
    d.facing = v(8);
    d.intercept = v(11);
    d.dest = v(14);
    d.saved = v(17);
    d.offset_430 = v(20);
    d.field_444 = v(23);
    d.orient_3a8 = m(26);
    d.orient_3d8 = m(35);
    d.orient_40c = m(44);
}

/// Puts the recorded trade-lane fields into the Rust behaviour: its
/// flag byte carries the heap's bits above bits 0 and 1 (D178).
fn restore_trade_lane(manager: &mut BehaviorManager<ReplayWorld<'_>>, r: &BehRecord) {
    if r.kind != 9 || r.ints.len() < 10 || r.floats.len() < 7 {
        return;
    }
    let Some(t) = manager
        .behavior_mut(9)
        .and_then(|b| b.as_any_mut())
        .and_then(|a| a.downcast_mut::<InstantTradeLaneBehavior>())
    else {
        return;
    };
    let i = &r.ints;
    let f = &r.floats;
    let byte = |v: i32| u8::try_from(v & 0xff).unwrap_or(0);
    {
        let base = game_sim::ai::behavior::Behavior::<ReplayWorld<'_>>::base_mut(t);
        base.mode = i[0];
        base.result = i[1];
        base.flags = byte(i[2]);
        base.flag29 = byte(i[3]);
    }
    t.state = i[4];
    t.entrance = (i[5] != 0).then(|| word(i[5]));
    t.lane = (i[6] != 0).then(|| word(i[6]));
    t.in_lane = byte(i[7]);
    t.flag_55 = byte(i[8]);
    t.flag_56 = byte(i[9]);
    t.speed = f[0];
    t.lane_start = Vector::new(f[1], f[2], f[3]);
    t.lane_end = Vector::new(f[4], f[5], f[6]);
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
        locks: Some([pf.flag_245, pf.field_24c, pf.field_254]),
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
        // Compared apart (`state_diff`).
        flags_1c8: None,
        sys: None,
        face: None,
        beh: None,
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
        && (a.locks.is_none() || b.locks.is_none() || a.locks == b.locks)
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
        // The target's id travels as the bits of the float.
        AiCommand::Tractor { equipment, target } => {
            (16, i32::from(*equipment), Some(vec![*target]))
        }
        AiCommand::SetGunTarget {
            equipment,
            target,
            subtarget,
        } => (
            17,
            i32::from(*equipment),
            Some(vec![target.unwrap_or(0), f32::from(*subtarget).to_bits()]),
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
        // The ids travel as the bits of the floats.
        AiCommand::Request {
            kind,
            target,
            second,
            flag,
        } => (23, *kind, Some(vec![*target, *second, word(*flag)])),
        AiCommand::CancelRequest { kind, target, arg } => {
            (24, *kind, Some(vec![*target, word(*arg)]))
        }
        AiCommand::GoTradeLane {
            entrance,
            exit,
            fast_exit,
            speed,
        } => (
            25,
            i32::from(*fast_exit),
            Some(vec![
                entrance.unwrap_or(0),
                exit.unwrap_or(0),
                speed.to_bits(),
            ]),
        ),
        AiCommand::StopTradeLane => (26, 0, None),
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
    /// Ticks whose exit holds the combat state (`sys`), and those whose
    /// state the Rust manager left differently.
    state_compared: usize,
    state_mismatched: usize,
    state_reports: Vec<String>,
    /// Ticks that asked an equipment query the snapshot cannot answer.
    unanswered: usize,
    unanswered_reports: Vec<String>,
    /// Ticks whose exit holds the dock, launch or trade-lane behaviour's
    /// fields, and those the Rust behaviour left differently.
    beh_compared: usize,
    beh_mismatched: usize,
    beh_reports: Vec<String>,
    /// The oracle's zone probes (`zone_props`) against the Rust world's
    /// zones (`World::sphere_zone_properties`, P2.1's zones).
    zone_compared: usize,
    zone_mismatched: usize,
    zone_reports: Vec<String>,
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
    // The combat state the tick started from, and the ship's items.
    if let Some(sys) = &t.input.sys {
        let g = &sys.guns;
        let _ = write!(
            r,
            "\n  entry: flags_1c8 {:?} priority {} attack_state {} gun target {:?} aim timer {:08x} enabled {} slots {:?} timers active {:?}",
            t.input.flags_1c8,
            sys.targeting.selection_priority,
            sys.targeting.attack_state,
            g.target,
            g.aim_refresh_timer.to_bits(),
            g.enabled,
            g.guns
                .iter()
                .map(|s| (s.id, s.kind, s.auto_turret, s.fire_tick, s.phase))
                .collect::<Vec<_>>(),
            sys.timers.timers.map(|t| t.active)
        );
    }
    if let Some(f) = &t.input.face {
        let _ = write!(r, "\n  entry face {f:?}");
    }
    if let Some(equip) = &world.equip {
        let _ = write!(
            r,
            "\n  equip {:?} lead {:?}",
            equip
                .iter()
                .map(|e| (e.id, e.type_mask, e.status, e.vals, e.pointing))
                .collect::<Vec<_>>(),
            world.lead
        );
    }
    r
}

/// A diagnostic for a mismatching face tick: the commands a fresh face
/// idea gives on the tick's entry state for each frame the face behaviour
/// could have built toward `point` (`BuildOrientationBasis`,
/// `BuildPerpendicularFrame`, `SteeringFrameFromRelativeDir`).
fn face_variants(
    t: &AiTick,
    motion: Option<&MotionWorld>,
    entry: &ManagerState,
    point: Vector,
) -> String {
    let Some(own) = t
        .objects
        .iter()
        .find(|o| o.id == t.id)
        .and_then(|o| o.view.clone())
    else {
        return String::new();
    };
    let reference = own.orientation;
    let diff = Vector::new(
        (f64::from(point.x) - f64::from(own.center.x)) as f32,
        (f64::from(point.y) - f64::from(own.center.y)) as f32,
        (f64::from(point.z) - f64::from(own.center.z)) as f32,
    );
    let frames = [
        (
            "orientation basis",
            Some(build_orientation_basis(&reference, diff, None)),
        ),
        (
            "perpendicular frame",
            build_perpendicular_frame(diff, 0.999, None),
        ),
        (
            "steering frame",
            Some(steering_frame_from_relative_dir(
                &reference,
                diff,
                Vector::new(0.0, 1.0, 0.0),
            )),
        ),
    ];
    let mut out = String::new();
    for (name, frame) in frames {
        let Some(frame) = frame else {
            let _ = write!(out, "\n  variant {name}: none");
            continue;
        };
        let mut w = ReplayWorld::new(t, Vec::new(), motion, true, &BTreeMap::new());
        let mut st = entry.clone();
        let mut cx = Cx::new(&mut w, t.id, &mut st);
        let mut idea = FaceIdea::new();
        idea.set_context(&cx);
        idea.set_basis(&frame);
        idea.update(&mut cx);
        let commands: Vec<AffectRow> = w.commands.iter().filter_map(as_affect).collect();
        let _ = write!(
            out,
            "\n  variant {name}: frame {:08x?} commands {commands:08x?}",
            frame.to_words().map(f32::to_bits)
        );
    }
    out
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
    // The scenario's names of the objects (the directives name them).
    let object_ids = read_names(path);
    let resolve = |name: &str| object_ids.get(name).copied();
    // The answers outside a tick, waiting for their ship's next tick.
    let mut queued: BTreeMap<u32, Vec<AnswerRecord>> = BTreeMap::new();
    let mut avoid_shuffled = false;
    let mut managers: BTreeMap<u32, BehaviorManager<ReplayWorld<'_>>> = BTreeMap::new();
    let mut first_seen: BTreeMap<u32, ()> = BTreeMap::new();
    // The scenario directives submitted so far, and each ship's last
    // recorded exit (the state a directive between ticks finds).
    // Each command once, by its index (another ship's tick in the same
    // frame must not consume it).
    let mut submitted: Vec<bool> = motion
        .as_ref()
        .map_or_else(Vec::new, |m| vec![false; m.commands.len()]);
    // Mismatches reported per ship.
    let mut reported: BTreeMap<u32, usize> = BTreeMap::new();
    let mut last_out: BTreeMap<u32, ManagerRecord> = BTreeMap::new();
    let mut face_point: Option<Vector> = None;
    let mut personalities: BTreeMap<u32, Personality> = BTreeMap::new();
    let names: BTreeMap<u32, String> = ticks
        .iter()
        .filter_map(|t| Some((t.id, t.obj.clone()?)))
        .collect();
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
        let mut world = ReplayWorld::new(t, scan, motion.as_ref(), avoid_shuffled, &names);
        world.managers = last_out
            .iter()
            .filter(|(id, _)| **id != t.id)
            .map(|(id, m)| {
                (
                    *id,
                    (
                        m.behavior,
                        m.directive.map(|w| i32::from_ne_bytes(w.to_ne_bytes())),
                    ),
                )
            })
            .collect();
        let is_player = world.objects.get(&t.id).is_some_and(|v| v.is_player);
        if first_seen.insert(t.id, ()).is_none()
            && let Some(v) = world.objects.get(&t.id)
            && totals.first_views.len() < MAX_REPORTED
            && path
                .parent()
                .and_then(Path::file_name)
                .is_some_and(|n| n == "run_01")
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
            // The reference binds the behaviours when it builds the manager,
            // before any tick: what their `SetContext` commands (a move
            // idea leaving engine mode cuts the throttle) is no part of the
            // tick, so they run against a world of their own.
            let mut binding =
                ReplayWorld::new(t, Vec::new(), motion.as_ref(), avoid_shuffled, &names);
            m.attach_all(&mut binding, t.id);
            // `InitSystemManager`'s tractor record (the ship's first
            // tractor, from the snapshot's items).
            m.state.sys.tractor.init(&binding, t.id);
            m.state.pathfind.owner_type = world.objects.get(&t.id).map_or(0, |v| v.type_flags);
            if let Some(v) = world.objects.get(&t.id) {
                m.state.pathfind.avoidance.attach_owner(v);
            }
            m.state.pathfind.avoidance.phys_system_id = 1;
            m
        });
        // The harness submits the frame's directives before the step, so
        // between the ship's last tick and this one, outside any behaviour
        // tick: run them on the state the last tick left (no record holds
        // what they do to the behaviours), then take the entry fields.
        let pending: Vec<DirectiveOp> = motion
            .as_ref()
            .map(|m| {
                let mut ops = Vec::new();
                for (index, (tick, name, args)) in m.commands.iter().enumerate() {
                    if *tick > t.tick || submitted[index] || t.obj.as_deref() != Some(name.as_str())
                    {
                        continue;
                    }
                    submitted[index] = true;
                    // The objects a follow or guide names, by the
                    // snapshot's scenario names.
                    let id_of = |n: &str| {
                        t.objects
                            .iter()
                            .find(|o| o.name.as_deref() == Some(n))
                            .map(|o| o.id)
                    };
                    if t.obj.as_deref() == Some(name.as_str())
                        && let Some(op) = support::directives::directive_with(args, &|n| {
                            id_of(n).or_else(|| resolve(n))
                        })
                    {
                        ops.push(op);
                    }
                }
                ops
            })
            .unwrap_or_default();
        // The world's answers outside the ticks since the ship's last one
        // (what the directives asked, and the server's later answer to a
        // request), in their order.
        for a in &t.before {
            queued.entry(a.id).or_default().push(a.clone());
        }
        let batch = queued.remove(&t.id).unwrap_or_default();
        let saved_answers = world.set_answers(batch.clone());
        if !pending.is_empty() {
            let before = last_out.get(&t.id).cloned().unwrap_or_else(|| {
                let mut r = t.input.clone();
                r.behavior = int(r.f120);
                r.directive = [0, 0, word(-1)];
                r
            });
            restore(&mut manager.state, &before, t, graphs);
            if let Some(f) = &before.face {
                restore_face(manager, f);
            }
            let recorded = std::mem::take(&mut world.rand);
            for op in pending {
                if let DirectiveOp::Face(f) = &op {
                    face_point = Some(f.point);
                }
                if !matches!(op, DirectiveOp::Cancel) {
                    manager.submit_directive(&mut world, t.id, &DirectiveOp::Cancel, 0);
                }
                manager.submit_directive(&mut world, t.id, &op, 0);
            }
            world.rand = recorded;
            world.commands.clear();
        }
        // The server's later answers to a request, with what the callbacks
        // asked meanwhile.
        if !batch.is_empty() {
            let recorded = std::mem::take(&mut world.rand);
            for a in batch.iter().filter(|a| a.kind == "request") {
                let k = world
                    .answers
                    .iter()
                    .zip(world.used.iter())
                    .position(|(b, u)| !u && b == a);
                if let Some(k) = k {
                    world.used[k] = true;
                }
                manager.request_returned(
                    &mut world,
                    t.id,
                    a.ints.first().copied().unwrap_or(0),
                    a.ints.get(1).copied().unwrap_or(0),
                    a.ints.get(2).copied().unwrap_or(-1),
                );
            }
            world.rand = recorded;
            world.commands.clear();
        }
        (world.answers, world.used) = saved_answers;
        restore(&mut manager.state, &t.input, t, graphs);
        if let Some(f) = &t.input.face {
            restore_face(manager, f);
        }
        if let Some(b) = &t.input.beh {
            restore_dock(manager, b);
            restore_trade_lane(manager, b);
        }
        manager.state.stand_in_polls = 0;
        let entry_state = manager.state.clone();
        if let Some(p) = personalities.get(&t.id) {
            manager.state.personality = *p;
        }
        let orient_before = face_snapshot(manager).map(|f| f.orient);
        manager.update_behavior_tick(&mut world, t.id, t.dt);
        avoid_shuffled = world.avoid_shuffled;
        last_out.insert(t.id, out.clone());
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
        let mut our_face = if out.face.is_some() {
            face_snapshot(manager)
        } else {
            None
        };
        // `FaceBehavior`'s matrix at +0x38 is left uninitialised by its
        // constructor (D166): while the tick did not write it (the Rust
        // value is the one before the tick) and the recorded one is no
        // rotation, the recorded value is whatever the memory held.
        if let (Some(ours), Some(theirs), Some(before)) =
            (our_face.as_mut(), out.face.as_ref(), orient_before)
            && ours
                .orient
                .m
                .as_flattened()
                .iter()
                .map(|x| x.to_bits())
                .eq(before.m.as_flattened().iter().map(|x| x.to_bits()))
            && !is_rotation(&theirs.orient)
        {
            ours.orient = theirs.orient;
        }
        // The oracle's zone probes against the Rust world's zones.
        if let Some(m) = motion.as_ref() {
            for a in t
                .answers
                .iter()
                .chain(&t.before)
                .filter(|a| a.kind == "zone_props" && a.floats.len() == 4)
            {
                let ours = m.world.sphere_zone_properties(
                    a.id,
                    Vector::new(a.floats[0], a.floats[1], a.floats[2]),
                    a.floats[3],
                );
                let theirs = a.ints.first().map_or(0, |&v| word(v));
                totals.zone_compared += 1;
                if ours != theirs {
                    totals.zone_mismatched += 1;
                    if totals.zone_reports.len() < MAX_REPORTED {
                        totals.zone_reports.push(format!(
                            "{} tick {} system {}: sphere {:?} oracle {theirs:#x} rust {ours:#x}",
                            path.display(),
                            a.tick,
                            a.id,
                            a.floats
                        ));
                    }
                }
            }
        }
        if let Some(theirs) = &out.beh {
            totals.beh_compared += 1;
            let ours = beh_snapshot(manager, theirs.kind);
            if let Some(d) = beh_diff(theirs, ours.as_ref()) {
                totals.beh_mismatched += 1;
                if totals.beh_reports.len() < MAX_REPORTED {
                    totals.beh_reports.push(format!(
                        "{} seq {} tick {} id {} behaviour {} -> {}: {d}",
                        path.display(),
                        t.seq,
                        t.tick,
                        t.id,
                        t.input.behavior,
                        out.behavior
                    ));
                }
            }
        }
        let state = state_diff(&manager.state, our_face.as_ref(), out);
        if out.sys.is_some() {
            totals.state_compared += 1;
            if !state.is_empty() {
                totals.state_mismatched += 1;
                if totals.state_reports.len() < MAX_REPORTED {
                    totals.state_reports.push(format!(
                        "{} seq {} tick {} id {} behaviour {} -> {}: {}",
                        path.display(),
                        t.seq,
                        t.tick,
                        t.id,
                        t.input.behavior,
                        out.behavior,
                        state.join("\n  ")
                    ));
                }
            }
        }
        if !world.unanswered.is_empty() {
            totals.unanswered += 1;
            if totals.unanswered_reports.len() < MAX_REPORTED {
                totals.unanswered_reports.push(format!(
                    "{} seq {} tick {} id {}: {:?}",
                    path.display(),
                    t.seq,
                    t.tick,
                    t.id,
                    world.unanswered
                ));
            }
        }
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
        let per_ship = reported.entry(t.id).or_default();
        *per_ship += 1;
        if totals.reports.len() < MAX_REPORTED && *per_ship <= 3 {
            let mut r = report(
                path,
                t,
                out,
                &ours,
                (&their_commands, &our_commands),
                &world,
            );
            if t.input.behavior == 15
                && let Some(point) = face_point
            {
                r.push_str(&face_variants(t, motion.as_ref(), &entry_state, point));
            }
            totals.reports.push(r);
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
    let _ = writeln!(
        summary,
        "combat state on exit: {} ticks compared, {} with a difference; {} ticks asked an equipment query the snapshot cannot answer",
        totals.state_compared, totals.state_mismatched, totals.unanswered
    );
    for r in totals
        .state_reports
        .iter()
        .chain(&totals.unanswered_reports)
    {
        let _ = writeln!(summary, "{r}");
    }
    let _ = writeln!(
        summary,
        "dock, launch and trade-lane fields on exit: {} ticks compared, {} with a difference",
        totals.beh_compared, totals.beh_mismatched
    );
    for r in &totals.beh_reports {
        let _ = writeln!(summary, "{r}");
    }
    let _ = writeln!(
        summary,
        "zone probes: {} compared with the Rust world's zones, {} with a difference",
        totals.zone_compared, totals.zone_mismatched
    );
    for r in &totals.zone_reports {
        let _ = writeln!(summary, "{r}");
    }
    print!("{summary}");
    let _ = fs::write(dir.join("rust-behavior-replay.txt"), &summary);
    assert_eq!(totals.mismatched, 0, "{summary}");
    assert_eq!(totals.state_mismatched, 0, "{summary}");
    assert_eq!(totals.unanswered, 0, "{summary}");
    assert_eq!(totals.beh_mismatched, 0, "{summary}");
    assert_eq!(totals.zone_mismatched, 0, "{summary}");
}
