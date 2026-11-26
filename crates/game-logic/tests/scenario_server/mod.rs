//! The oracle scenarios' server around game-sim's driver, shared by
//! `scenario_hooks.rs` and `content_startup.rs`: the oracle's start-up
//! (`Oracle.exe sim`'s reduced `[Initial MP DLLs]`, the harness's own
//! controller and `Server_LoadWorldState`), the server's hooks with the
//! physics of every sub-step on, the populated scenario's player and
//! SpacePop (`population <ship>`, P3.6d), the ships the population spawns
//! (`npc<k>` in spawn order, as the oracle's `tick.jsonl` names them) and
//! the bus's heartbeats of each frame.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use game_logic::content::npc_ships::{self, NpcShipParams};
use game_logic::events::ControllerMessage;
use game_logic::server::{CreateParams, Cx, Logic};
use game_logic::world::Hooks;
use game_sim::object::damage::DamageList;
use game_sim::object::world::{DamageState, ServerHooks, ServerRequest};
use game_sim::object::{ObjHandle, World};
use game_sim::scenario::ScenarioServer;
use neuron_core::ini::IniReader;
use neuron_server::controller::Priority;

/// A controller as the bus lists it when frame 1 starts: id, priority,
/// heartbeat period and accumulator.
pub type Listed = (u32, u32, f32, f32);

/// The server's hooks with the physics of every sub-step on, as the
/// oracle's harness forces it for the scenario's system: every other
/// method is the server's.
pub struct ScenarioHooks<'l> {
    pub server: Hooks<'l>,
    /// The AI's requests go to the server (a scenario marked `# rust:
    /// server`); the other scenarios run without, as they were judged
    /// before.
    pub requests: bool,
    /// The populated scenario's ship (`population <ship>`, P3.6d): it
    /// becomes player 1's after the spawns, SpacePop hears its launch, and
    /// each frame ends with the bus's heartbeats and timers as the
    /// oracle's `ElapseTime` runs them.
    pub population: Option<String>,
    /// The scenario's system, once spawned.
    pub system: u32,
    /// The ships the population spawned, in the order they were first
    /// found in the system's record (`SpaceObjs`): name, id, archetype.
    pub population_ships: Vec<(String, u32, u32)>,
    /// The bus's controllers when frame 1 starts (the populated scenario).
    pub start_listing: Vec<Listed>,
    /// With `Some`, each frame's heartbeats: tick, controller, elapsed time
    /// (bits). [`heartbeat_hook`] must be the logic's delivery hook.
    pub beats: Option<Vec<(u32, u32, u32)>>,
}

impl<'l> ScenarioHooks<'l> {
    /// The hooks over `logic` for a scenario.
    pub fn new(logic: &'l mut Logic, requests: bool, population: Option<String>) -> Self {
        Self {
            server: Hooks { logic },
            requests,
            population,
            system: 0,
            population_ships: Vec::new(),
            start_listing: Vec::new(),
            beats: None,
        }
    }
}

thread_local! {
    static HEARTBEATS: RefCell<Vec<(u32, u32)>> = const { RefCell::new(Vec::new()) };
}

/// A delivery hook that notes each `Heartbeat` (controller, elapsed time
/// bits) for [`ScenarioHooks::beats`] (`content_startup` uses it,
/// `scenario_hooks` does not).
#[allow(dead_code)]
pub fn heartbeat_hook(
    _: &mut Cx<'_>,
    id: neuron_server::controller::ControllerId,
    _: game_logic::server::ControllerKind,
    message: &mut ControllerMessage<'_>,
) {
    if let ControllerMessage::Heartbeat { elapsed } = *message {
        HEARTBEATS.with(|h| h.borrow_mut().push((id.0, elapsed.to_bits())));
    }
}

impl ServerHooks for ScenarioHooks<'_> {
    fn object_update(&mut self, world: &mut World, object: ObjHandle, dt: f32) -> bool {
        self.server.object_update(world, object, dt)
    }

    fn object_update_flags(&mut self, world: &World, object: ObjHandle) -> u32 {
        self.server.object_update_flags(world, object)
    }

    fn object_after_update(&mut self, world: &mut World, object: ObjHandle, dt: f32) {
        self.server.object_after_update(world, object, dt);
    }

    fn damage_state(
        &mut self,
        world: &World,
        object: ObjHandle,
        inflictor_owner: u32,
    ) -> DamageState {
        self.server.damage_state(world, object, inflictor_owner)
    }

    fn before_damage_list(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        list: &mut DamageList,
    ) -> bool {
        self.server.before_damage_list(world, object, list)
    }

    fn damage_entry(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        list: &mut DamageList,
        index: usize,
    ) {
        self.server.damage_entry(world, object, list, index);
    }

    fn after_damage_list(&mut self, world: &mut World, object: ObjHandle, list: &mut DamageList) {
        self.server.after_damage_list(world, object, list);
    }

    fn damage_list_done(&mut self, world: &mut World, object: ObjHandle) {
        self.server.damage_list_done(world, object);
    }

    fn destroy(&mut self, world: &mut World, object: ObjHandle, kind: i32, killer: u32) -> bool {
        self.server.destroy(world, object, kind, killer)
    }

    fn after_reap(&mut self, world: &mut World, system: u32) {
        self.server.after_reap(world, system);
    }

    fn server_requests(&mut self, world: &mut World, requests: Vec<ServerRequest>) {
        if self.requests {
            self.server.server_requests(world, requests);
        }
    }

    fn steps_physics(&mut self, _world: &World, _system: u32) -> bool {
        true
    }

    fn content_answer(
        &mut self,
        world: &mut World,
        ship: ObjHandle,
        handle: u32,
    ) -> Option<game_sim::ai::content::FleeAnswer> {
        self.server.content_answer(world, ship, handle)
    }

    fn directive_complete(
        &mut self,
        world: &mut World,
        ship: ObjHandle,
        handle: u32,
        result: (i32, i32),
    ) {
        self.server.directive_complete(world, ship, handle, result);
    }

    /// The end of a `LaunchAction` (`ring_launch`): `Ship::slot_70` lifts
    /// the launch invincibility with `LaunchComplete`.
    fn launch_complete(&mut self, world: &mut World, object: ObjHandle, base: u32) {
        self.server.launch_complete(world, object, base);
    }
}

impl ScenarioServer for ScenarioHooks<'_> {
    fn hooks(&mut self) -> &mut dyn ServerHooks {
        self
    }

    /// A `faction`'s vibe as the oracle's harness makes it
    /// (`AllocFactionVibe`): `pub::Reputation::GetReputationGroup`, then
    /// `Alloc` with empty names (the server's vibe id) and
    /// `SetAffiliation`.
    fn faction_vibe(&mut self, world: &mut World, faction: &str) -> Option<i32> {
        let mut cx = Cx {
            world,
            logic: self.server.logic,
        };
        faction_vibe(&mut cx, faction)
    }

    /// `spaceobj_launch` (`ring_launch`): `pub::SpaceObj::Launch(ship, dock,
    /// type)`, the server's `StarSystem::launch`.
    fn launch(&mut self, world: &mut World, ship: ObjHandle, dock: ObjHandle, kind: i32) -> bool {
        let (Some(ship), Some(dock)) = (
            world.get(ship).map(|o| o.simple.id),
            world.get(dock).map(|o| o.simple.id),
        ) else {
            return false;
        };
        let mut cx = Cx {
            world,
            logic: self.server.logic,
        };
        let _ = game_logic::api::space_obj::launch(&mut cx, ship, dock, kind);
        true
    }

    /// The populated scenario loads its system as the server does
    /// (`StarSystem::load`: every solar and field of the system file, then
    /// `SystemLoaded` to the controllers, from which SpaceSupport builds the
    /// system record's zones, lanes and dock targets that the population
    /// reads), as the oracle's `OracleSimLoadSystem` does. The other
    /// scenarios keep the driver's load of the solars they need.
    fn load_system(&mut self, world: &mut World, system: u32) -> bool {
        if self.population.is_none() {
            return false;
        }
        let mut cx = Cx {
            world,
            logic: self.server.logic,
        };
        game_logic::world::load(&mut cx, system);
        true
    }

    /// The populated scenario (`OracleSimPopulate`): player 1 with the
    /// ship, then SpacePop and its launch; the bus's list as frame 1 finds
    /// it.
    fn spawned(
        &mut self,
        world: &mut World,
        system: u32,
        objects: &BTreeMap<String, Option<ObjHandle>>,
    ) {
        self.system = system;
        let Some(name) = &self.population else {
            return;
        };
        let Some(h) = objects.get(name).copied().flatten() else {
            return;
        };
        let (ship, archetype) = world.get(h).map_or((0, 0), |o| (o.simple.id, o.archetype));
        let mut cx = Cx {
            world,
            logic: self.server.logic,
        };
        game_logic::content::population::oracle_populate(&mut cx, 1, system, ship, archetype);
        self.start_listing = cx
            .logic
            .bus
            .active()
            .iter()
            .map(|n| {
                (
                    n.id.0,
                    n.priority.value(),
                    n.heartbeat_period,
                    n.heartbeat_accum,
                )
            })
            .collect();
        HEARTBEATS.with(|h| h.borrow_mut().clear());
    }

    /// The end of the oracle's `ElapseTime` in the populated scenario: the
    /// controllers' heartbeats, then the timers.
    fn after_step(&mut self, world: &mut World, _system: u32, tick: u32, dt: f32) {
        if self.population.is_none() {
            return;
        }
        let mut cx = Cx {
            world,
            logic: self.server.logic,
        };
        neuron_server::controller::tick(&mut cx, dt);
        neuron_server::controller::fire_timers(&mut cx, dt);
        let beats = HEARTBEATS.with(|h| std::mem::take(&mut *h.borrow_mut()));
        if let Some(out) = &mut self.beats {
            out.extend(beats.into_iter().map(|(id, elapsed)| (tick, id, elapsed)));
        }
    }

    /// The ships of the system's record (`SpaceObjs`) not yet named, in
    /// list order, as the oracle's harness finds them
    /// (`OracleSimPopulationShips`): `npc<k>`.
    fn recorded_objects(&mut self, world: &World) -> Vec<(String, ObjHandle)> {
        if self.population.is_none() {
            return Vec::new();
        }
        let state = &self.server.logic.content.state;
        let Some(record) = state.systems.get(&self.system) else {
            return Vec::new();
        };
        let mut out = Vec::new();
        for key in &record.space_objs {
            let Some(id) = state.npc_ships.get(key).map(|s| s.base.ship) else {
                continue;
            };
            if id == 0 || self.population_ships.iter().any(|p| p.1 == id) {
                continue;
            }
            let Some(h) = world.find(id) else {
                continue;
            };
            let archetype = world.get(h).map_or(0, |o| o.archetype);
            let name = format!("npc{}", self.population_ships.len() + 1);
            self.population_ships.push((name.clone(), id, archetype));
            out.push((name, h));
        }
        out
    }

    /// A `content` ship (P3.6c): Content's NPC ship in the system's record
    /// (`FSystemInfo::CreateNpcShip`), as the oracle's
    /// `OracleSimCreateNpcShip` makes it: the archetype, loadout, place,
    /// hit points, state graph (`LEADER`) and personality (the default, or
    /// the pilot's), owned by SpaceSupport, with a reputation handle of its
    /// `faction`'s vibe (`FOracleNpcReputation`, which frees nothing) and
    /// the `flee` position.
    fn spawn_content_ship(
        &mut self,
        world: &mut World,
        system: u32,
        object: &game_sim::scenario::ObjDesc,
        personality: Option<game_sim::ai::personality::Personality>,
        vibe: i32,
    ) -> Option<Option<ObjHandle>> {
        let mut cx = Cx {
            world,
            logic: self.server.logic,
        };
        let state_graph = object.ai.as_ref().map_or(-1, |g| {
            cx.world
                .data()
                .state_graphs
                .find(g.as_bytes(), game_sim::ai::state_graph::GraphType::Leader)
        });
        let reputation = cx
            .logic
            .content
            .state
            .rep_handles
            .create(vibe, game_logic::content::rep_handles::RepKind::Plain);
        let params = NpcShipParams {
            reputation: Some(reputation),
            archetype: neuron_core::id::create_id(object.archetype.as_bytes()),
            loadout: if object.loadout.is_empty() {
                0
            } else {
                neuron_core::id::create_id(object.loadout.as_bytes())
            },
            position: object.pos,
            orientation: object.orient,
            hit_points: object.hp as i32,
            state_graph,
            personality: personality.unwrap_or_default(),
            controller: cx.logic.content.state.space_support.0,
            ..NpcShipParams::default()
        };
        let (_, key) = npc_ships::create_in_system(&mut cx, system, &params, 0);
        npc_ships::release_params(&mut cx, params);
        let key = key?;
        if let Some((point, distance)) = object.flee {
            npc_ships::set_flee_position(&mut cx, key, point, distance);
        }
        let id = npc_ships::space_obj(&cx, key);
        Some(cx.world.find(id))
    }
}

/// `AllocFactionVibe` (`oracle_sim.cpp:2079-2098`) through the server's
/// `pub::Reputation`: `None` for an unknown group or a failed call.
fn faction_vibe(cx: &mut Cx<'_>, faction: &str) -> Option<i32> {
    let group = game_logic::api::reputation::reputation_group(cx, faction.as_bytes()).ok()?;
    let name = game_sim::protocol::fmtstr::FmtStr::default();
    let vibe = game_logic::api::reputation::alloc(cx, &name, &name).ok()?;
    game_logic::api::reputation::set_affiliation(cx, vibe, group).ok()?;
    Some(vibe)
}

/// The `population <ship>` line of a scenario file (P3.6d; the scenario
/// parser leaves it to the server).
pub fn population_ship(path: &Path) -> Option<String> {
    let text = fs::read_to_string(path).ok()?;
    text.lines().find_map(|l| {
        let l = l.split('#').next().unwrap_or("").trim();
        let mut t = l.split_whitespace();
        match (t.next(), t.next()) {
            (Some(k), Some(ship)) if k.eq_ignore_ascii_case("population") => Some(ship.to_owned()),
            _ => None,
        }
    })
}

/// The oracle's start-up (`Oracle.exe sim`, `WriteServerIni` and
/// `Server_LoadWorldState`): Content's GameSupport, SpaceSupport and
/// BaseSupport, the id of the harness's own controller (`OracleSim`,
/// created in load_rand's `controller` phase from the same counter as the
/// objects, so the scenario's objects get the oracle's ids), then
/// `LoadGame` over `outdir`'s `InitialWorld.ini`, which builds the system
/// records. The constructors' heartbeat draws come before the driver's
/// seeding point, as the oracle's do.
pub fn content_start_up(cx: &mut Cx<'_>, outdir: &Path) {
    for (name, priority) in [
        (&b"GameSupport"[..], Priority::Highest),
        (&b"SpaceSupport"[..], Priority::Normal),
        (&b"BaseSupport"[..], Priority::Normal),
    ] {
        let id = neuron_server::controller::create(
            cx,
            b"Content.dll",
            name,
            &CreateParams::None,
            priority,
        );
        assert_ne!(id.0, 0, "{}", String::from_utf8_lossy(name));
    }
    let _harness = cx.world.allocate_id();
    let text = fs::read(outdir.join("data/initialworld.ini")).unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    game_logic::selection::load_world_state(cx, &mut reader);
}
