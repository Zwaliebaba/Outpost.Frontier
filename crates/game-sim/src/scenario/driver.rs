//! The run driver: a scenario's objects spawned in the world, its commands
//! applied and its system stepped through the server's hooks, and what the
//! run records.

use std::collections::BTreeMap;

use neuron_core::id::create_id;
use neuron_core::math::Vector;
use neuron_core::physics::StepMode;
use neuron_core::physics::replay::CoreRecord;

use super::directives::directive_with;
use super::file::{Scenario, num, spawn_orientation, vec3};
use super::oracle::AiInputs;
use crate::ai::behavior::DirectiveOp;
use crate::ai::state_graph::GraphType;
use crate::equip::flight::StrafeDir;
use crate::equip::{EquipKind, SUBOBJ_ID_ROOT, types};
use crate::object::world::{ServerHooks, ShipSpawn, WorldConfig};
use crate::object::{Command, DamageList, KindData, ObjHandle, ObjectKind, World, WorldEvent};

/// An equipped object's first shield.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ShieldState {
    /// The shield's sub-object id.
    pub id: u16,
    /// Its hit points.
    pub hp: f32,
    /// Its maximum hit points.
    pub max: f32,
    /// Whether it is online.
    pub online: bool,
}

/// An object after a tick (the oracle's `object` record of `tick.jsonl`).
#[derive(Clone, Copy, Debug, Default)]
pub struct State {
    /// In the world and not dead.
    pub alive: bool,
    /// The position.
    pub pos: Vector,
    /// The velocity.
    pub vel: Vector,
    /// The orientation's rows.
    pub orient: [f32; 9],
    /// The angular velocity.
    pub angvel: Vector,
    /// A ship's commanded throttle and axis throttle (`None` for others).
    pub throttle: Option<f32>,
    /// A ship's commanded axis throttle (`None` for others).
    pub axis: Option<Vector>,
    /// A ship's commanded nudge (`get_nudge_vec`; `None` for others).
    pub nudge: Option<Vector>,
    /// The hull's hit points.
    pub hp: f32,
    /// `None` when the record does not carry it (an older oracle record,
    /// or an object without equipment).
    pub power: Option<f32>,
    /// `None` without a shield, or when the record does not carry it.
    pub shield: Option<ShieldState>,
    /// The current behaviour (`CurrentBehaviorIndex`), -1 without a
    /// manager.
    pub behavior: i32,
    /// The target's scenario name as an id (`create_id`), 0 for none.
    pub target: u32,
}

/// One munition hit: the target, the sub-object, the hull and shield
/// before and after.
#[derive(Clone, Debug)]
pub struct Hit {
    /// The tick.
    pub tick: u32,
    /// The target's scenario name, or `#<id>`.
    pub target: String,
    /// The hit sub-object.
    pub sub: u16,
    /// The oracle's `sub_kind`; on the Rust side from the shield's id.
    pub kind: String,
    /// The hull before and after.
    pub hull: (f32, f32),
    /// The shield before and after (a hit on the shield).
    pub shield: Option<(f32, f32)>,
    /// The damage list's entries.
    pub entries: usize,
}

/// One collision event: the reporting object, the other, the relative
/// speed (`None` where the record does not carry it) and the damage.
#[derive(Clone, Debug)]
pub struct Contact {
    /// The tick.
    pub tick: u32,
    /// The reporting object's scenario name, or `#<id>`.
    pub a: String,
    /// The other object's.
    pub b: String,
    /// The relative speed.
    pub speed: Option<f32>,
    /// The collision damage.
    pub damage: f32,
}

/// A guided munition after a tick (the oracle's `guided` record).
#[derive(Clone, Debug, Default)]
pub struct GuidedState {
    /// The tick.
    pub tick: u32,
    /// Still flying (false once, the first tick it is gone).
    pub alive: bool,
    /// The position.
    pub pos: Vector,
    /// The velocity.
    pub vel: Vector,
    /// The orientation's rows.
    pub orient: [f32; 9],
    /// The angular velocity.
    pub angvel: Vector,
    /// The projectile's lifetime.
    pub lifetime: f32,
    /// The seeker's elapsed time.
    pub seeker_elapsed: f32,
    /// The target's scenario name, or `#<id>`.
    pub target: String,
}

/// What a run produced: the per-tick states, the munition hits and the
/// collisions.
#[derive(Default)]
pub struct Run {
    /// Each scenario object's state before the first tick and after each.
    pub states: BTreeMap<String, Vec<State>>,
    /// The munition hits.
    pub hits: Vec<Hit>,
    /// The collision events.
    pub contacts: Vec<Contact>,
    /// The behaviour managers' clocks after construction (the phases the
    /// constructors draw), for comparison with the first `ai_in` record.
    pub managers: Vec<String>,
    /// The first manager's pathfinder phases (its first three draws).
    pub pathfind_phases: Option<[f32; 3]>,
    /// The first behaviour the port does not have that an AI ship ran, and
    /// the tick (the state graph can pick one without a directive).
    pub unported: Option<(i32, u32)>,
    /// The shots fired (`WorldEvent::Fired`).
    pub fired: usize,
    /// The guided munitions launched: (tick, shooter's name, gun).
    pub missiles: Vec<(u32, String, u16)>,
    /// Each guided munition's states by id, from its first tick to the
    /// first tick it is gone.
    pub guided: BTreeMap<u32, Vec<GuidedState>>,
    /// Each ship's guns' turret joints per tick, as text (diagnostic).
    pub guns: BTreeMap<String, Vec<String>>,
    /// The AI ships' turret probes after each tick (0: before the first
    /// step), by tick and ship.
    pub probes: BTreeMap<u32, BTreeMap<String, Vec<TurretProbe>>>,
    /// The first seeker steps of each guided munition (diagnostic).
    pub guided_trace: Vec<(u32, crate::object::world::GuidedTrace)>,
    /// What the launcher looked like when each guided munition went out.
    pub launch_notes: Vec<String>,
    /// Each guided munition's core after each tick (the oracle's `out`).
    pub guided_cores: BTreeMap<u32, Vec<(u32, CoreRecord)>>,
    /// Every fire call: (tick, shooter's name, each gun and its result).
    pub fires: Vec<(u32, String, String)>,
    /// Per object and tick, the body's core after the frame (diagnostic).
    pub cores: BTreeMap<String, Vec<String>>,
    /// The simulation units of the first [`UNIT_PSIS`] PSIs (`units.jsonl`).
    pub units: Vec<Vec<Unit>>,
    /// Each unit's core at the start and the end of its PSI, for the first
    /// PSIs (`unit_cores.jsonl`).
    pub unit_cores: Vec<Vec<neuron_core::physics::world::UnitCore>>,
    /// The bullet rays that started inside an object, which they then do
    /// not hit (as in IVP): the tick and the object's name.
    pub starts_inside: Vec<(u32, String)>,
    /// The objects a bullet's ray crossed that IVP's OV sphere test kept it
    /// from hitting: the tick and the object's name.
    pub ov_culled: Vec<(u32, String)>,
}

/// A core's state as the bit diagnostic prints it.
fn fmt_core(c: &neuron_core::physics::ivp::core::Core) -> String {
    let h = |v: &[f32]| {
        v.iter()
            .map(|x| format!("{:08x}", x.to_bits()))
            .collect::<Vec<_>>()
            .join(" ")
    };
    let q = |q: &neuron_core::physics::ivp::math::IvpQuat| h(&[q.x, q.y, q.z, q.w]);
    let t = |t: &neuron_core::physics::ivp::math::IvpTime| h(&[t.base, t.offset]);
    format!(
        "flags {:x} movement {:x} rot_speed [{}] q_next [{}] q_last_motion [{}] pos_last_motion [{}] time_of_last_motion [{}] time_of_last_long_motion [{}] time_of_last_psi [{}]",
        c.flags,
        c.movement_state,
        h(&c.rot_speed),
        q(&c.q_world_f_core_next_psi),
        q(&c.q_last_motion),
        h(&c.pos_last_motion),
        t(&c.time_of_last_motion),
        t(&c.time_of_last_long_motion),
        t(&c.time_of_last_psi),
    )
}

fn record_cores(
    world: &World,
    out: &mut BTreeMap<String, Vec<String>>,
    handles: &BTreeMap<String, Option<ObjHandle>>,
) {
    for (name, h) in handles {
        let text = h
            .and_then(|h| world.get(h))
            .and_then(|o| {
                let body = o.physical.body?;
                world.physics(o.physical.system)?.core(body).map(fmt_core)
            })
            .unwrap_or_else(|| "-".to_owned());
        out.entry(name.clone()).or_default().push(text);
    }
}

fn shield_of(world: &World, h: ObjHandle) -> (Option<f32>, Option<ShieldState>) {
    let Some(eq) = world.get(h).and_then(|o| o.as_equipped()) else {
        return (None, None);
    };
    let shield = eq
        .equipment
        .first(types::SHIELD)
        .and_then(|e| match &e.kind {
            EquipKind::Shield(s) => Some(ShieldState {
                id: e.id,
                hp: s.hit_pts,
                max: s.max_hit_pts,
                online: s.online,
            }),
            _ => None,
        });
    (Some(eq.power), shield)
}

/// The Rust side of a hit: the target's hull and shield as the world had
/// them before the hit (so shield regeneration earlier in the frame is in
/// "before", as in the oracle's pre-hit state), then the damage list's
/// entries applied, in order.
fn rust_hit(
    tick: u32,
    target: &str,
    sub: u16,
    list: &DamageList,
    before: (f32, Option<(u16, f32)>),
    running: &mut State,
) -> Hit {
    running.hp = before.0;
    if let (Some(sh), Some((id, hp))) = (running.shield.as_mut(), before.1)
        && sh.id == id
    {
        sh.hp = hp;
    }
    let hull_before = running.hp;
    let shield_before = running.shield;
    for e in &list.entries {
        if e.sub_obj_id == SUBOBJ_ID_ROOT {
            running.hp = e.hit_pts;
        } else if let Some(sh) = running.shield.as_mut()
            && sh.id == e.sub_obj_id
        {
            sh.hp = e.hit_pts.min(sh.max);
        }
    }
    let kind = match shield_before {
        Some(sh) if sh.id == sub => "shield",
        _ if sub == SUBOBJ_ID_ROOT => "root",
        _ => "other",
    };
    Hit {
        tick,
        target: target.to_owned(),
        sub,
        kind: kind.to_owned(),
        hull: (hull_before, running.hp),
        shield: shield_before.zip(running.shield).map(|(a, b)| (a.hp, b.hp)),
        entries: list.entries.len(),
    }
}

/// The world configuration a scenario runs in: outside single player
/// (the server's paths), the physics in the scenario's stepping mode
/// (`stepping sp`), the server's physics, the scenario's seed.
#[must_use]
pub fn world_config(s: &Scenario) -> WorldConfig {
    WorldConfig {
        single_player: false,
        step_mode: if s.sp {
            StepMode::SinglePlayer
        } else {
            StepMode::Multiplayer
        },
        server_physics: true,
        seed: s.seed,
    }
}

/// A server around a scenario run (P3.3's replication comparison, record
/// 0007 point 11): the hooks of each frame, and what the server does at
/// three points of the run, each a no-op by default.
pub trait ScenarioServer {
    /// The server's hooks of a frame.
    fn hooks(&mut self) -> &mut dyn ServerHooks;

    /// Loads the scenario's system as the server loads it (every solar of
    /// the system file, as the oracle's `StarSystem::load`); the named
    /// solars are then found by their nicknames. False (the default) lets
    /// the driver load the system and spawn the solars it needs, as without
    /// a server.
    fn load_system(&mut self, world: &mut World, system: u32) -> bool {
        let _ = (world, system);
        false
    }

    /// After the objects are spawned and the attitudes set, before tick 0
    /// is recorded: the oracle's players join here (`player`).
    fn spawned(
        &mut self,
        world: &mut World,
        system: u32,
        objects: &BTreeMap<String, Option<ObjHandle>>,
    ) {
        let _ = (world, system, objects);
    }

    /// A `content` ship (P3.6c): the server makes it as Content's NPC ship
    /// (`FSystemInfo::CreateNpcShip`, which submits its state graph,
    /// personality and callbacks) and answers its handle; `None` (the
    /// default, without a server) makes it as any other ship.
    fn spawn_content_ship(
        &mut self,
        world: &mut World,
        system: u32,
        object: &super::file::ObjDesc,
        personality: Option<crate::ai::personality::Personality>,
    ) -> Option<Option<ObjHandle>> {
        let _ = (world, system, object, personality);
        None
    }

    /// After tick `tick`'s step of `dt` seconds, before the global time
    /// advances: the server's per-frame work after the system's step (the
    /// observers' updates of `StarSystem_Update`).
    fn after_step(&mut self, world: &mut World, system: u32, tick: u32, dt: f32) {
        let _ = (world, system, tick, dt);
    }
}

/// The plain hooks of [`run`], as a [`ScenarioServer`] that does nothing
/// more.
struct HooksOnly<'h>(&'h mut dyn ServerHooks);

impl ScenarioServer for HooksOnly<'_> {
    fn hooks(&mut self) -> &mut dyn ServerHooks {
        &mut *self.0
    }
}

/// Runs a scenario in `world` (made with [`world_config`] over the game
/// data): loads the system and the solars it needs, spawns the objects
/// (the AI ships with their state graphs, personalities and attitudes from
/// `ai`), and for each tick applies the tick's commands, steps the system
/// through `hooks` (`World::step_with`, then the global time) and records
/// what [`Run`] holds. With [`NoServerHooks`](crate::object::world::NoServerHooks)
/// each tick is [`World::step`].
pub fn run(world: World, s: &Scenario, ai: &AiInputs, hooks: &mut dyn ServerHooks) -> Run {
    run_with(world, s, ai, &mut HooksOnly(hooks))
}

/// [`run`] with a [`ScenarioServer`]: its hooks step the system, and it
/// loads the system, hears of the spawned objects and runs after each
/// tick's step where it says so.
pub fn run_with(
    mut world: World,
    s: &Scenario,
    ai: &AiInputs,
    server: &mut dyn ScenarioServer,
) -> Run {
    let system = create_id(s.system.as_bytes());
    let server_loaded = server.load_system(&mut world, system);
    if !server_loaded {
        world.load_system(system);
        // `StarSystem::load` outside single player loads the system's
        // asteroid fields; the scenario ships fill those they fly in, as in
        // the oracle.
        world.load_asteroid_fields(system);
    }
    let mut handles: BTreeMap<String, Option<ObjHandle>> = BTreeMap::new();
    let mut managers = Vec::new();
    let mut pathfind_phases = None;
    // The solars the scenario names, and every solar whose body or parts
    // are simulation units: the spinning ones and those with free (`!fxd`)
    // parts, which `BuildIVP_Real` makes bodies of their own held by
    // golems. The oracle's `StarSystem::load` builds them all, and each
    // unit takes its turn at the environment's movement-check countdown
    // (that decides when a scenario ship's motion is classified), so the
    // Rust world steps the oracle's units in the oracle's order. The other
    // solars are fixed and far from the scenario objects, and the contact
    // pass over every solar is slow in a debug build.
    let named: Vec<u32> = s
        .objects
        .iter()
        .filter(|o| !o.ship)
        .map(|o| create_id(o.nickname.as_bytes()))
        .collect();
    if !server_loaded {
        for sp in world.system_solar_spawns(system) {
            if named.contains(&sp.nickname)
                || sp.angular_velocity != Vector::ZERO
                || world.solar_has_free_parts(sp.archetype)
            {
                let _ = world.spawn_solar(system, &sp);
            }
        }
    }
    world.set_unit_trace(system, true);
    world.set_fire_log(true);
    world.set_guided_trace(true);
    if s.sp_collisions {
        world.set_server_collision_filter(system, false);
    }
    if s.comms_off {
        println!(
            "{}: comms off: the oracle's comm system is clamped (no comm rolls); the server's is clamped the same way, the world alone has none",
            s.name
        );
    }
    // The seeding point (owner, 2026-11-18): the oracle's harness calls
    // `srand(seed)` again once the system and its population have loaded.
    // The seed ensemble replaces the seed here only (owner, 2026-11-19).
    world.rng().seed(s.reseed.unwrap_or(s.seed));
    for o in &s.objects {
        let personality_of = |o: &super::file::ObjDesc| {
            o.pilot
                .as_ref()
                .and_then(|_| ai.personalities.get(&o.name).copied())
        };
        let content = if o.ship && o.content {
            server.spawn_content_ship(&mut world, system, o, personality_of(o))
        } else {
            None
        };
        let h = if let Some(h) = content {
            if let Some(m) = h.and_then(|h| world.behavior_manager(h)) {
                let pf = &m.state.pathfind;
                pathfind_phases.get_or_insert([pf.timer_1e0, pf.timer_1e8, pf.timer_20c]);
                managers.push(format!(
                    "{}: manager timers {:08x?} pathfind {:08x?}",
                    o.name,
                    m.state.timers().map(f32::to_bits),
                    [pf.timer_1e0, pf.timer_1e8, pf.timer_20c].map(f32::to_bits)
                ));
            }
            h
        } else if o.ship {
            let mut spawn = ShipSpawn::new(
                create_id(o.archetype.as_bytes()),
                create_id(o.loadout.as_bytes()),
                o.pos,
                spawn_orientation(&o.orient),
            );
            spawn.velocity = o.vel;
            spawn.angular_velocity = o.angvel;
            spawn.hit_pts = o.hp;
            let h = world.spawn_ship(system, &spawn).ok();
            if let (Some(h), Some(graph)) = (h, &o.ai) {
                let kind = if o.escort {
                    GraphType::Escort
                } else {
                    GraphType::Leader
                };
                let id = world.data().state_graphs.find(graph.as_bytes(), kind);
                // The harness submits a personality only with `pilot`
                // (`bSetPersonality`); without one the manager keeps the
                // default and the gun groups draw nothing more.
                let personality = o
                    .pilot
                    .as_ref()
                    .and_then(|_| ai.personalities.get(&o.name).copied());
                world.submit_state(h, id, personality);
                if let Some(m) = world.behavior_manager(h) {
                    let pf = &m.state.pathfind;
                    pathfind_phases.get_or_insert([pf.timer_1e0, pf.timer_1e8, pf.timer_20c]);
                    managers.push(format!(
                        "{}: manager timers {:08x?} pathfind {:08x?}",
                        o.name,
                        m.state.timers().map(f32::to_bits),
                        [pf.timer_1e0, pf.timer_1e8, pf.timer_20c].map(f32::to_bits)
                    ));
                }
            }
            h
        } else {
            world.find(create_id(o.nickname.as_bytes()))
        };
        handles.insert(o.name.clone(), h);
    }
    for ((from, to), value) in &ai.attitudes {
        if let (Some(Some(a)), Some(Some(b))) = (handles.get(from), handles.get(to)) {
            world.set_attitude(*a, *b, *value);
        }
    }
    server.spawned(&mut world, system, &handles);
    let names: BTreeMap<u32, String> = handles
        .iter()
        .filter_map(|(n, h)| {
            h.and_then(|h| world.get(h))
                .map(|o| (o.simple.id, n.clone()))
        })
        .filter(|(id, _)| *id != 0)
        .collect();
    let name_of = |id: u32| names.get(&id).cloned().unwrap_or_else(|| format!("#{id}"));
    let mut out = Run {
        managers,
        pathfind_phases,
        ..Run::default()
    };
    let record = |world: &World,
                  out: &mut BTreeMap<String, Vec<State>>,
                  handles: &BTreeMap<String, Option<ObjHandle>>| {
        let target_of = |h: ObjHandle| {
            let id = world.ai_view(h)?.target?;
            names.get(&id).map(|n| create_id(n.as_bytes()))
        };
        for (name, h) in handles {
            let st = h
                .and_then(|h| world.inspect(h).map(|i| (h, i)))
                .filter(|(_, i)| !i.dead)
                .map_or(State::default(), |(h, i)| {
                    let (power, shield) = shield_of(world, h);
                    State {
                        alive: true,
                        pos: i.position,
                        vel: i.velocity,
                        orient: i.orientation.to_words(),
                        angvel: i.angular_velocity,
                        throttle: i.ship.as_ref().map(|s| s.throttle),
                        axis: i.ship.as_ref().map(|s| s.axis_throttle),
                        nudge: i.ship.as_ref().map(|s| s.nudge),
                        hp: i.hit_pts,
                        power,
                        shield,
                        behavior: world.behavior_manager(h).map_or(-1, |m| m.state.behavior),
                        target: target_of(h).unwrap_or(0),
                    }
                });
            out.entry(name.clone()).or_default().push(st);
        }
    };
    record(&world, &mut out.states, &handles);
    record_cores(&world, &mut out.cores, &handles);
    out.probes.insert(0, turret_probes(&world, &handles));
    let mut next = 0;
    for tick in 1..=s.ticks {
        while next < s.commands.len() && s.commands[next].tick == tick {
            let c = &s.commands[next];
            next += 1;
            let Some(Some(h)) = handles.get(&c.object).copied() else {
                continue;
            };
            let a = &c.args;
            // `formation <leader|none> [x y z]`: `pub::AI::
            // update_formation_state`.
            if a[0] == "formation" {
                let leader = match a.get(1).map(String::as_str) {
                    Some("none") | None => None,
                    Some(n) => handles.get(n).copied().flatten(),
                };
                let offset = vec3(a, 2).unwrap_or(Vector::ZERO);
                world.update_formation_state(h, leader, offset);
                continue;
            }
            let id_of = |n: &str| {
                let h = handles.get(n).copied().flatten()?;
                world.get(h).map(|o| o.simple.id)
            };
            if let Some(op) = directive_with(a, &id_of) {
                // A current directive blocks a new one, so each scenario
                // directive cancels the one before.
                if !matches!(op, DirectiveOp::Cancel) {
                    world.submit_directive(h, &DirectiveOp::Cancel);
                }
                world.submit_directive(h, &op);
                continue;
            }
            let command = match a[0].as_str() {
                "throttle" => a
                    .get(1)
                    .and_then(|v| num(v))
                    .map(|v| Command::SetThrottle(v as f32)),
                "axis" => vec3(a, 1).map(Command::SetAxisThrottle),
                "nudge" => vec3(a, 1).map(Command::SetNudgeVec),
                "strafe" => {
                    let names = ["none", "left", "right", "up", "down"];
                    names
                        .iter()
                        .position(|n| a.get(1).is_some_and(|x| x == n))
                        .map(|i| Command::SetStrafeDir(StrafeDir::from_value(i as i32)))
                }
                "fire" => {
                    let target = match a.get(1) {
                        Some(t) => handles
                            .get(t)
                            .copied()
                            .flatten()
                            .and_then(|h| world.inspect(h))
                            .map(|i| i.position),
                        None => world.inspect(h).map(|i| {
                            let m = i.orientation.m;
                            Vector::new(
                                (f64::from(i.position.x) - 1000.0 * f64::from(m[0][2])) as f32,
                                (f64::from(i.position.y) - 1000.0 * f64::from(m[1][2])) as f32,
                                (f64::from(i.position.z) - 1000.0 * f64::from(m[2][2])) as f32,
                            )
                        }),
                    };
                    target.map(|t| Command::Fire {
                        guns: world.unguided_guns(h),
                        target: t,
                    })
                }
                _ => None,
            };
            if let Some(command) = command {
                world.command(h, command);
            }
        }
        world.step_with(system, s.frame as f32, server.hooks());
        server.after_step(&mut world, system, tick, s.frame as f32);
        world.update_global_time(s.frame as f32);
        for h in world.take_beam_starts_inside(system) {
            let name = world
                .get(h)
                .map_or_else(|| "?".to_owned(), |o| name_of(o.simple.id));
            out.starts_inside.push((tick, name));
        }
        for h in world.take_beam_ov_culled(system) {
            let name = world
                .get(h)
                .map_or_else(|| "?".to_owned(), |o| name_of(o.simple.id));
            out.ov_culled.push((tick, name));
        }
        if out.unported.is_none() {
            out.unported = handles.values().flatten().find_map(|&h| {
                let m = world.behavior_manager(h)?;
                let b = m.state.behavior;
                (b != -1 && !m.is_ported(b)).then_some((b, tick))
            });
        }
        let mut running: BTreeMap<String, State> = out
            .states
            .iter()
            .filter_map(|(n, v)| v.last().map(|st| (n.clone(), *st)))
            .collect();
        for ev in world.take_events() {
            match ev {
                WorldEvent::MunitionHit {
                    target,
                    sub,
                    damage,
                    hull_before,
                    shield_before,
                    ..
                } => {
                    let name = name_of(target);
                    let st = running.entry(name.clone()).or_default();
                    let before = (hull_before, shield_before);
                    out.hits
                        .push(rust_hit(tick, &name, sub, &damage, before, st));
                }
                WorldEvent::Fired {
                    shooter,
                    equip,
                    bullet,
                } => {
                    out.fired += 1;
                    if bullet != 0 {
                        out.missiles.push((tick, name_of(shooter), equip));
                        out.launch_notes
                            .push(launch_note(&world, tick, shooter, equip));
                    }
                }
                WorldEvent::Collision {
                    a,
                    b,
                    speed,
                    damage,
                } => out.contacts.push(Contact {
                    tick,
                    a: name_of(a),
                    b: name_of(b),
                    speed: Some(speed),
                    damage,
                }),
                _ => {}
            }
        }
        record(&world, &mut out.states, &handles);
        record_cores(&world, &mut out.cores, &handles);
        record_guided(&world, system, tick, &names, &mut out);
        out.probes.insert(tick, turret_probes(&world, &handles));
        for (name, h) in &handles {
            let text = h
                .and_then(|h| world.get(h))
                .and_then(|o| o.as_equipped())
                .map(|eq| {
                    eq.equipment
                        .traverse(types::GUN, false)
                        .into_iter()
                        .filter_map(|slot| eq.equipment.get(slot))
                        .filter_map(|e| match &e.kind {
                            EquipKind::Gun(g) => Some(gun_text(
                                u32::from(e.id),
                                [
                                    g.yaw.current,
                                    g.yaw.desired,
                                    g.pitch.current,
                                    g.pitch.desired,
                                    g.target_point_age,
                                ],
                                &format!(
                                    "b{}{} t{}",
                                    u8::from(g.yaw.is_bound()),
                                    u8::from(g.pitch.is_bound()),
                                    g.target
                                        .and_then(|t| world.get(t))
                                        .map_or_else(|| "-".to_owned(), |o| name_of(o.simple.id))
                                ),
                            )),
                            _ => None,
                        })
                        .collect::<Vec<_>>()
                        .join(" ")
                })
                .unwrap_or_default();
            out.guns.entry(name.clone()).or_default().push(text);
        }
        for t in world.take_guided_trace() {
            let n = out
                .guided_trace
                .iter()
                .filter(|(_, x)| x.id == t.id)
                .count();
            if n < 3 {
                out.guided_trace.push((tick, t));
            }
        }
        for (shooter, results) in world.take_fire_log() {
            out.fires
                .push((tick, name_of(shooter), format!("{results:?}")));
        }
        // The units of the PSIs this frame ran, named while their objects
        // still exist.
        if out.units.len() < UNIT_PSIS {
            let trace = world.unit_trace(system);
            for psi in trace
                .into_iter()
                .skip(out.units.len())
                .take(UNIT_PSIS - out.units.len())
            {
                out.units.push(
                    psi.into_iter()
                        .map(|(id, class, check)| Unit { id, class, check })
                        .collect(),
                );
            }
        }
    }
    out.unit_cores = world.unit_core_trace(system);
    out
}

/// The shooter's frame and the launcher's turret when a guided munition
/// goes out (diagnostic).
fn launch_note(world: &World, tick: u32, shooter: u32, equip: u16) -> String {
    let Some(h) = world.find(shooter) else {
        return format!("tick {tick}: shooter #{shooter} gone");
    };
    let Some(o) = world.get(h) else {
        return String::new();
    };
    let m = o.engine.orientation.m;
    let hex = |v: &[f32]| {
        v.iter()
            .map(|x| format!("{:08x}", x.to_bits()))
            .collect::<Vec<_>>()
            .join(" ")
    };
    let gun = o
        .as_equipped()
        .and_then(|eq| eq.equipment.by_id(equip))
        .and_then(|e| match &e.kind {
            EquipKind::Gun(g) => Some(g),
            _ => None,
        })
        .map_or_else(String::new, |g| {
            format!(
                "yaw desired {} current {} bound {} limited {}; pitch desired {} current {} bound {} limited {}; target point [{}] age {}; lock {:?} sub {} elapsed {}",
                g.yaw.desired,
                g.yaw.current,
                g.yaw.instance.is_some(),
                g.yaw.limited,
                g.pitch.desired,
                g.pitch.current,
                g.pitch.instance.is_some(),
                g.pitch.limited,
                hex(&[g.target_point.x, g.target_point.y, g.target_point.z]),
                g.target_point_age,
                g.target.and_then(|t| world.get(t)).map(|t| t.simple.id),
                g.lock_sub,
                g.lock_elapsed
            )
        });
    format!(
        "tick {tick}: shooter #{shooter} orient [{}] pos [{}]; gun {equip}: {gun}",
        hex(&[
            m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]
        ]),
        hex(&[
            o.engine.position.x,
            o.engine.position.y,
            o.engine.position.z
        ])
    )
}

/// A gun's turret joints as the report prints them: the id, the words'
/// bits (a signed zero as zero) and `extra`.
#[must_use]
pub fn gun_text(id: u32, v: [f32; 5], extra: &str) -> String {
    format!(
        "{id}:[{} {extra}]",
        v.iter()
            // A signed zero compares as zero (the joints' rest value).
            .map(|x| format!("{:08x}", (x + 0.0).to_bits()))
            .collect::<Vec<_>>()
            .join(" ")
    )
}

/// One gun of an AI ship as the auto-turret scan sees it: barrel 0's
/// position and `can_point_at(position, 0.3)` toward each other named
/// object.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TurretProbe {
    /// The gun's sub-object id.
    pub gun: u16,
    /// Barrel 0's position.
    pub barrel: Option<Vector>,
    /// The turret geometry as the oracle's `turret` record lays it out.
    pub turret: Option<Vec<f32>>,
    /// `can_point_at` toward each other named object.
    pub can_point: BTreeMap<String, bool>,
}

/// The turret probes of every AI ship (diagnostic).
fn turret_probes(
    world: &World,
    handles: &BTreeMap<String, Option<ObjHandle>>,
) -> BTreeMap<String, Vec<TurretProbe>> {
    use crate::ai::world::AiWorld;
    let mut out = BTreeMap::new();
    for (name, h) in handles {
        let Some(h) = *h else {
            continue;
        };
        if world.behavior_manager(h).is_none() {
            continue;
        }
        let Some(eq) = world.get(h).and_then(|o| o.as_equipped()) else {
            continue;
        };
        let guns: Vec<u16> = eq
            .equipment
            .traverse(types::GUN, false)
            .into_iter()
            .filter_map(|slot| eq.equipment.get(slot))
            .map(|e| e.id)
            .collect();
        let mut probes = Vec::new();
        for gun in guns {
            let barrel = <World as AiWorld>::equipment(world, h, gun)
                .and_then(|i| i.barrel)
                .map(|(p, _)| p);
            let mut can_point = BTreeMap::new();
            for (other, oh) in handles {
                let Some(oh) = *oh else {
                    continue;
                };
                if oh == h {
                    continue;
                }
                let Some(v) = <World as AiWorld>::inspect(world, oh) else {
                    continue;
                };
                can_point.insert(
                    other.clone(),
                    <World as AiWorld>::can_point_at(world, h, gun, v.position, 0.3),
                );
            }
            let turret = world.turret_geometry(h, gun).map(|t| {
                let joint = |j: Option<(f32, f32)>| match j {
                    Some((min, max)) => [1.0, 0.0, min, max],
                    None => [0.0; 4],
                };
                let mut v = Vec::with_capacity(29);
                v.extend(joint(t.yaw));
                v.extend(joint(t.pitch));
                v.extend([t.origin.x, t.origin.y, t.origin.z]);
                v.extend(t.frame.m.iter().flatten());
                v.extend(t.connector.m.iter().flatten());
                v
            });
            probes.push(TurretProbe {
                gun,
                barrel,
                turret,
                can_point,
            });
        }
        out.insert(name.clone(), probes);
    }
    out
}

/// The guided munitions after a tick: each live one's state and core,
/// and `alive` false once for each one gone.
fn record_guided(
    world: &World,
    system: u32,
    tick: u32,
    names: &BTreeMap<u32, String>,
    out: &mut Run,
) {
    let mut live = Vec::new();
    for h in world.objects_in(system) {
        let Some(o) = world.get(h) else {
            continue;
        };
        if o.kind != ObjectKind::Guided || o.dead {
            continue;
        }
        let KindData::Guided(g) = &o.data else {
            continue;
        };
        let Some(i) = world.inspect(h) else {
            continue;
        };
        let id = o.simple.id;
        live.push(id);
        let target = g
            .target
            .and_then(|t| world.get(t))
            .map(|t| {
                names
                    .get(&t.simple.id)
                    .cloned()
                    .unwrap_or_else(|| format!("#{}", t.simple.id))
            })
            .unwrap_or_default();
        out.guided.entry(id).or_default().push(GuidedState {
            tick,
            alive: true,
            pos: i.position,
            vel: i.velocity,
            orient: i.orientation.to_words(),
            angvel: i.angular_velocity,
            lifetime: g.projectile.lifetime,
            seeker_elapsed: g.seeker_elapsed,
            target,
        });
        if let Some(core) = o
            .physical
            .body
            .and_then(|b| world.physics(o.physical.system)?.core(b).cloned())
        {
            out.guided_cores.entry(id).or_default().push((
                tick,
                CoreRecord {
                    core,
                    unit_state: 0,
                },
            ));
        }
    }
    for (id, states) in &mut out.guided {
        if !live.contains(id) && states.last().is_some_and(|s| s.alive) {
            states.push(GuidedState {
                tick,
                ..GuidedState::default()
            });
        }
    }
}

/// The PSIs `units.jsonl` records (`kUnitPsis`, oracle_sim.cpp).
pub const UNIT_PSIS: usize = 120;

/// A simulation unit of a PSI: its (first) core's object as `units.jsonl`
/// names it, and whether the movement check came to it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Unit {
    /// The object's id.
    pub id: u32,
    /// Its class.
    pub class: u32,
    /// The movement check came to it.
    pub check: bool,
}
