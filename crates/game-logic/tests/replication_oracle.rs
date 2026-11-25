//! The replication comparison against the oracle (work package P3.3,
//! decision record 0007 point 11, Tier B): the packets each client
//! receives per tick in a scripted multiplayer scenario,
//! `CppSource/Oracle/replication.txt`, from the C++ server (`Oracle.exe
//! sim` with `dump replication`: SimClientStub's `replication.jsonl`)
//! against game-logic's server.
//!
//! The scenario runs with game-sim's driver (`game_sim::scenario::run_with`)
//! through a multiplayer [`Logic`]: the server loads the system as
//! `StarSystem::load` does ([`world::load`], every solar of the system
//! file), Content's SpaceSupport answers the trade lane and gives each
//! named solar with a `reputation` its faction on `SystemLoaded` (the
//! system records of P3.4d, after the oracle's start-up: GameSupport,
//! SpaceSupport and BaseSupport, then `Server_LoadWorldState` over
//! `InitialWorld.ini`), the AI's requests go to the server, and once the
//! objects are spawned each `player
//! <session> <ship>` of the file joins the system as the oracle's
//! `OracleSimAddPlayer` makes it join: the slot takes the system and the
//! ship, the client id stays 0, then [`world::add_player`]. After each
//! tick's step the observers update ([`replication::update_observers`],
//! then the changed affiliations), as `StarSystem_Update` ends. A
//! recording [`ClientSink`] keeps what each client is sent, by tick.
//!
//! Both records are reduced to the same [`Pkt`]: the method, the object,
//! the other objects it names, the discrete fields and the floats with
//! their tolerances. Objects are compared by label, not id: a scenario
//! object by its name, a solar by its id (`CreateID`, the same on both
//! sides), a munition or loot by its kind, its owner's label and its
//! place among that owner's creates; anything else is unknown (`?id`).
//!
//! The envelopes (decision O20):
//! - **Exact** where the record 0007 rules make the packets deterministic:
//!   while the objects a packet names and the client's own ship are bit
//!   for bit the oracle's (`same_state` on the per-tick states of both
//!   runs; a munition or loot also only up to the oracle's first contact or
//!   impact), each client's packets of each tick are the oracle's as a
//!   multiset: kind, labels and discrete fields equal, the floats within
//!   their tolerance. The order within a tick is reported, not judged: the
//!   AI's fire, requests and lanes reach the server after the behaviour
//!   tick (D622, D651, D730), and so do the players' notices of its target,
//!   cruise, thruster and equipment changes (D857), which are judged too.
//!   A difference here is a gross divergence and fails the test. A solar's
//!   affiliation in `SETREPUTATION` is judged (fact F17: Content gives it
//!   on `SystemLoaded`).
//! - **An object the port does not have** (a packet about an id that is
//!   neither a scenario object, a solar of the system nor made by one of
//!   them) fails the test: the harness's server runs no populator
//!   (`server.ini`), so none is expected.
//! - **Reported** for the rest of the run: per client and kind the packet
//!   counts, per object the ticks it was created and destroyed on each
//!   client, and the largest distance between the two sides' updates of
//!   the same object in the same tick.
//!
//! Also reported: whether the oracle's two runs wrote the same record, and
//! the trajectories as `scenario_hooks` judges them.
//!
//! `OUTPOST_DATA_DIR` names the dump root (`EXE`, `DATA`) and
//! `OUTPOST_ORACLE_DIR` the folder holding `replication/run_NN/`; without
//! either the test prints a notice and returns. The summary is written to
//! `rust-replication.txt` in the oracle folder.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use game_logic::replication;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::world::{self, Hooks};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{Assets, ServerHooks};
use game_sim::object::{ObjHandle, ObjectKind, World};
use game_sim::protocol::ToClient;
use game_sim::protocol::common::CommonMessage;
use game_sim::protocol::quantise_orientation;
use game_sim::protocol::to_client::ServerMessage;
use game_sim::scenario::records::{Json, find_files, read_lines};
use game_sim::scenario::{
    Run, Scenario, ScenarioServer, first_event, first_event_of, judge, oracle_ai_inputs,
    oracle_run, parse, run_with, same_state, world_config,
};
use neuron_core::net::wire::quantise::quantise_unit;
use neuron_core::vfs::Host;
use neuron_server::controller::Priority;

/// The scenario's name: the oracle's folder under `OUTPOST_ORACLE_DIR`.
const SCENARIO: &str = "replication";

/// Positions and velocities in the exact window, metres (and m/s): the
/// states are bit for bit equal there.
const EXACT_POS: f32 = 1e-3;
/// A quantised byte (orientation, throttle): one step either way, for a
/// quaternion an ulp apart.
const BYTE: f32 = 1.0;
/// The observer's clock.
const TIME: f32 = 1e-6;
/// Hit points.
const HP: f32 = 1e-3;
/// A `FIREWEAPON` aim component, which the wire carries in a byte or a
/// word scaled by 127/64 or 32767/8192.
const AIM: f32 = 1.0;

/// The kinds the exact window judges.
const JUDGED: [&str; 25] = [
    "SendServerCreateShip",
    "SendServerCreateSolar",
    "SendServerCreateLoot",
    "SendServerCreateMine",
    "SendServerCreateCounter",
    "SendServerCreateGuided",
    "SendServerSetGuidedTarget",
    "SendServerDestroyObject",
    "SendServerDamageObject",
    "SendServerActivateObject",
    "SendServerSetReputation",
    "SendServerBurnFuse",
    "SendServerSystemSwitchOut",
    "SendServerSystemSwitchIn",
    "SendServerLand",
    "SendServerLaunch",
    "SendCommonUpdateObject",
    "SendCommonFireWeapon",
    "SendCommonGoTradeLane",
    "SendCommonStopTradeLane",
    "SendCommonJettisonCargo",
    "SendCommonSetTarget",
    "SendCommonActivateCruise",
    "SendCommonActivateThrusters",
    "SendCommonActivateEquip",
];

/// One packet as the comparison sees it.
#[derive(Clone, Debug)]
struct Pkt {
    tick: u32,
    client: u32,
    /// The `IClient` method of the oracle's record.
    m: String,
    /// The object it is about (0: none).
    id: u32,
    /// The other objects it names (owner, target, inflictor, rings).
    refs: Vec<u32>,
    /// The discrete fields.
    ints: Vec<i64>,
    /// The floats and their tolerances.
    floats: Vec<(f32, f32)>,
}

impl Pkt {
    fn new(tick: u32, client: u32, m: &str, id: u32) -> Self {
        Self {
            tick,
            client,
            m: m.to_owned(),
            id,
            refs: Vec::new(),
            ints: Vec::new(),
            floats: Vec::new(),
        }
    }

    fn pos(&mut self, v: [f32; 3], tolerance: f32) {
        self.floats.extend(v.iter().map(|&x| (x, tolerance)));
    }

    /// Quantised bytes as signed values, so that one step across 0 stays
    /// one step.
    fn bytes(&mut self, b: &[u8]) {
        self.floats
            .extend(b.iter().map(|&x| (f32::from(x.cast_signed()), BYTE)));
    }

    /// A quaternion `w, x, y, z` as the wire's four bytes.
    fn quat(&mut self, wxyz: [f32; 4]) {
        self.bytes(&quantise_orientation(wxyz));
    }
}

// ---- the port's packets ----------------------------------------------------------------------

/// A client sink that keeps what it is sent until the driver takes it.
#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

/// One message as a [`Pkt`]; `None` for a kind the comparison does not
/// read. `small` turns an archetype id into the wire's small id, which
/// the oracle's record carries in full.
fn rust_pkt(tick: u32, client: u32, message: &ToClient) -> Option<Pkt> {
    let p = |m: &str, id: u32| Pkt::new(tick, client, m, id);
    match message {
        ToClient::Common(c) => match c {
            CommonMessage::UpdateObject(u) => {
                let mut k = p("SendCommonUpdateObject", u.object_id);
                k.ints.push(i64::from(u.flags));
                k.pos(u.position, EXACT_POS);
                k.bytes(&u.orientation);
                k.floats.push((u.update_time, TIME));
                k.bytes(&[u.extra]);
                Some(k)
            }
            CommonMessage::FireWeapon(f) => {
                let mut k = p("SendCommonFireWeapon", f.object_id);
                k.ints.extend(f.hardpoints.iter().map(|&h| i64::from(h)));
                k.pos(f.target.map(|c| c.value()), AIM);
                Some(k)
            }
            CommonMessage::ActivateEquip(a) => {
                let mut k = p("SendCommonActivateEquip", a.object_id);
                k.ints
                    .extend([i64::from(a.equip_id), i64::from(a.active != 0)]);
                Some(k)
            }
            CommonMessage::ActivateCruise(a) => {
                let mut k = p("SendCommonActivateCruise", a.object_id);
                k.ints
                    .extend([i64::from(a.active != 0), i64::from(a.field_05)]);
                Some(k)
            }
            CommonMessage::ActivateThrusters(a) => {
                let mut k = p("SendCommonActivateThrusters", a.object_id);
                k.ints.push(i64::from(a.active != 0));
                Some(k)
            }
            CommonMessage::SetTarget(t) => {
                let mut k = p("SendCommonSetTarget", t.object_id);
                k.refs.push(t.target_id);
                k.ints
                    .extend([i64::from(t.sub_obj_id), i64::from(t.target_sub_obj_id)]);
                Some(k)
            }
            CommonMessage::GoTradeLane(g) => {
                let mut k = p("SendCommonGoTradeLane", g.object_id);
                k.refs.extend([g.tradelane_ring1, g.tradelane_ring2]);
                k.ints.push(i64::from(g.field_0c != 0));
                Some(k)
            }
            CommonMessage::StopTradeLane(s) => {
                let mut k = p("SendCommonStopTradeLane", s.ship_id);
                k.refs.extend([s.tradelane_ring1, s.tradelane_ring2]);
                Some(k)
            }
            CommonMessage::JettisonCargo(j) => {
                let mut k = p("SendCommonJettisonCargo", j.object_id);
                k.ints.extend([i64::from(j.item_id), i64::from(j.count)]);
                Some(k)
            }
            _ => None,
        },
        ToClient::Server(s) => match s {
            ServerMessage::CreateShip(c) => {
                let o = &c.object;
                let mut k = p("SendServerCreateShip", o.obj_id);
                k.ints.extend([
                    i64::from(o.archetype),
                    o.equipment.len() as i64,
                    i64::from(c.using_tradelane),
                ]);
                k.pos(o.position, EXACT_POS);
                k.bytes(&o.orientation);
                k.pos(c.velocity, EXACT_POS);
                k.bytes(&[c.throttle]);
                Some(k)
            }
            ServerMessage::CreateSolar(c) => {
                let o = &c.object;
                let mut k = p("SendServerCreateSolar", o.obj_id);
                k.ints.push(i64::from(o.archetype));
                k.pos(o.position, EXACT_POS);
                k.bytes(&o.orientation);
                Some(k)
            }
            ServerMessage::CreateLoot(c) => {
                let first = c.loot.first();
                let mut k = p("SendServerCreateLoot", first.map_or(0, |l| l.loot_id));
                k.refs.push(c.owner_id);
                k.ints.extend([i64::from(c.kind), c.loot.len() as i64]);
                if let Some(l) = first {
                    k.ints.extend([
                        i64::from(l.contents),
                        i64::from(l.units),
                        i64::from(l.container),
                    ]);
                    k.pos(l.position, EXACT_POS);
                }
                Some(k)
            }
            ServerMessage::CreateMine(c) => {
                let mut k = p("SendServerCreateMine", c.obj_id);
                k.refs.push(c.owner_id);
                k.ints
                    .extend([i64::from(c.archetype), i64::from(c.sub_obj_id)]);
                k.pos(c.position, EXACT_POS);
                k.pos(c.velocity, EXACT_POS);
                Some(k)
            }
            ServerMessage::CreateCounter(c) => {
                let mut k = p("SendServerCreateCounter", c.obj_id);
                k.refs.push(c.owner_id);
                k.ints
                    .extend([i64::from(c.archetype), i64::from(c.sub_obj_id)]);
                k.pos(c.position, EXACT_POS);
                k.pos(c.velocity, EXACT_POS);
                Some(k)
            }
            ServerMessage::CreateGuided(c) => {
                let mut k = p("SendServerCreateGuided", c.obj_id);
                k.refs.extend([c.owner_id, c.target_id]);
                k.ints.extend([
                    i64::from(c.archetype),
                    i64::from(c.sub_obj_id),
                    i64::from(c.target_sub_obj_id),
                ]);
                k.pos(c.position, EXACT_POS);
                k.bytes(&c.orientation);
                Some(k)
            }
            ServerMessage::SetGuidedTarget(g) => {
                let mut k = p("SendServerSetGuidedTarget", g.field_20);
                k.refs.push(g.field_24);
                k.ints.push(i64::from(g.field_28));
                Some(k)
            }
            ServerMessage::DestroyObject(d) => {
                let mut k = p("SendServerDestroyObject", d.object_id);
                k.ints.push(i64::from(d.field_04));
                Some(k)
            }
            ServerMessage::DamageObject(d) => {
                let mut k = p("SendServerDamageObject", d.object_id);
                let l = &d.damage;
                k.refs.push(l.inflictor_id);
                k.ints.extend([
                    i64::from(l.destroyed != 0),
                    i64::from(l.cause),
                    l.entries.len() as i64,
                ]);
                for e in &l.entries {
                    k.ints.extend([i64::from(e.sub_obj_id), i64::from(e.fate)]);
                    k.floats.push((e.hit_points, HP));
                }
                Some(k)
            }
            ServerMessage::ActivateObject(a) => {
                let mut k = p("SendServerActivateObject", a.object_id);
                k.ints
                    .extend([i64::from(a.activate), i64::from(a.field_08)]);
                Some(k)
            }
            ServerMessage::SetReputation(r) => {
                let mut k = p("SendServerSetReputation", 0);
                k.ints.push(i64::from(r.target));
                k.ints.push(i64::from(r.affiliation));
                Some(k)
            }
            ServerMessage::BurnFuse(f) => {
                let mut k = p("SendServerBurnFuse", f.object_id);
                k.refs.push(f.source_id);
                k.ints.extend([
                    i64::from(f.fuse_id),
                    i64::from(f.sub_obj_id),
                    i64::from(f.lit),
                ]);
                k.floats.extend([(f.at_t, TIME), (f.lifetime, TIME)]);
                Some(k)
            }
            ServerMessage::SystemSwitchOut(o) => {
                let mut k = p("SendServerSystemSwitchOut", o.field_00);
                k.refs.push(o.field_04);
                Some(k)
            }
            ServerMessage::SystemSwitchIn(i) => {
                let mut k = p("SendServerSystemSwitchIn", i.field_00);
                k.ints.push(i64::from(i.field_04));
                k.pos(i.position, EXACT_POS);
                k.quat(i.orientation);
                Some(k)
            }
            ServerMessage::Land(l) => {
                let mut k = p("SendServerLand", l.field_00);
                k.refs.push(l.field_04);
                k.ints.push(i64::from(l.field_08));
                Some(k)
            }
            ServerMessage::Launch(l) => {
                let mut k = p("SendServerLaunch", l.field_00);
                k.refs.push(l.field_04);
                k.ints.push(i64::from(l.field_08));
                k.pos(l.position, EXACT_POS);
                k.quat(l.orientation);
                Some(k)
            }
            _ => None,
        },
    }
}

/// The port's server around the scenario run.
struct ReplicationServer<'l> {
    hooks: Hooks<'l>,
    sink: Shared,
    /// The `player` lines: session and ship.
    players: Vec<(u32, String)>,
    /// What each client was sent, with the tick.
    sent: Vec<(u32, u32, ToClient)>,
    /// The scenario objects' ids by name.
    ids: BTreeMap<String, u32>,
    /// The system's solars.
    solars: BTreeSet<u32>,
    /// The players that could not join, and why.
    refused: Vec<String>,
}

impl ReplicationServer<'_> {
    fn take(&mut self, tick: u32) {
        let sent = std::mem::take(&mut *self.sink.0.borrow_mut());
        self.sent
            .extend(sent.into_iter().map(|(client, m)| (tick, client, m)));
    }
}

impl ScenarioServer for ReplicationServer<'_> {
    fn hooks(&mut self) -> &mut dyn ServerHooks {
        &mut self.hooks
    }

    fn load_system(&mut self, world: &mut World, system: u32) -> bool {
        let mut cx = Cx {
            world,
            logic: &mut *self.hooks.logic,
        };
        world::load(&mut cx, system);
        true
    }

    fn spawned(
        &mut self,
        world: &mut World,
        system: u32,
        objects: &BTreeMap<String, Option<ObjHandle>>,
    ) {
        for (name, h) in objects {
            if let Some(o) = h.and_then(|h| world.get(h)) {
                self.ids.insert(name.clone(), o.simple.id);
            }
        }
        self.solars = world
            .objects_in(system)
            .into_iter()
            .filter_map(|h| world.get(h))
            .filter(|o| o.kind == ObjectKind::Solar)
            .map(|o| o.simple.id)
            .collect();
        let mut cx = Cx {
            world,
            logic: &mut *self.hooks.logic,
        };
        for (session, name) in &self.players {
            let Some(&ship) = self.ids.get(name) else {
                self.refused
                    .push(format!("player {session}: no ship {name}"));
                continue;
            };
            let Some(slot) = cx.logic.players.get_mut(*session) else {
                self.refused
                    .push(format!("player {session}: no slot for the session"));
                continue;
            };
            // `OracleSimAddPlayer`: the system and the ship, the client
            // id left at 0, then `StarSystem::add_player`.
            slot.system_id = system;
            slot.ship_object_id = ship;
            world::add_player(&mut cx, system, *session);
        }
        self.take(0);
    }

    fn after_step(&mut self, world: &mut World, system: u32, tick: u32, dt: f32) {
        let mut cx = Cx {
            world,
            logic: &mut *self.hooks.logic,
        };
        // The end of `StarSystem_Update` (`world::step_system`).
        replication::update_observers(&mut cx, system, f64::from(dt));
        replication::send_changed_affiliations(&mut cx, system);
        self.take(tick);
    }
}

// ---- the oracle's packets --------------------------------------------------------------------

fn floats(j: &Json, key: &str, n: usize) -> Option<Vec<f32>> {
    j.get(key)?.as_floats().filter(|v| v.len() == n)
}

fn arr3(v: &[f32]) -> [f32; 3] {
    [v[0], v[1], v[2]]
}

fn arr4(v: &[f32]) -> [f32; 4] {
    [v[0], v[1], v[2], v[3]]
}

/// One line of `replication.jsonl` as a [`Pkt`], the archetypes made
/// small where the wire makes them small; `None` for a line that does
/// not read.
fn oracle_pkt(j: &Json, data: &GameData) -> Option<Pkt> {
    let u = |k: &str| j.get(k).and_then(Json::as_u32);
    let i = |k: &str| j.get(k).and_then(Json::as_i32);
    let b = |k: &str| j.get(k).and_then(Json::as_bool).map(i64::from);
    let f = |k: &str| j.get(k).and_then(Json::as_f32);
    let small = |id: u32| i64::from(data.archetypes.large_to_small(id));
    let m = j.get("m")?.as_str()?;
    let mut k = Pkt::new(u("tick")?, u("client")?, m, u("id")?);
    match m {
        "SendCommonUpdateObject" => {
            k.ints.push(i64::from(u("behavior")? & 0xff));
            k.pos(arr3(&floats(j, "pos", 3)?), EXACT_POS);
            k.quat(arr4(&floats(j, "q", 4)?));
            k.floats.push((f("time")?, TIME));
            k.bytes(&[quantise_unit(f("throttle")?)]);
        }
        "SendCommonFireWeapon" => {
            let guns = j.get("guns")?.as_array()?;
            for g in guns {
                k.ints.push(i64::from(g.as_u32()?));
            }
            k.pos(arr3(&floats(j, "target", 3)?), AIM);
        }
        "SendCommonActivateEquip" => k.ints.extend([i64::from(u("equip")?), b("on")?]),
        "SendCommonActivateCruise" => k.ints.extend([b("on")?, i64::from(u("arg")?)]),
        "SendCommonActivateThrusters" => k.ints.push(b("on")?),
        "SendCommonSetTarget" => {
            k.refs.push(u("target")?);
            k.ints
                .extend([i64::from(u("sub")?), i64::from(u("target_sub")?)]);
        }
        "SendCommonGoTradeLane" => {
            k.refs.extend([u("ring1")?, u("ring2")?]);
            k.ints.push(b("flag")?);
        }
        "SendCommonStopTradeLane" => k.refs.extend([u("ring1")?, u("ring2")?]),
        "SendCommonJettisonCargo" => k
            .ints
            .extend([i64::from(u("item")?), i64::from(u("count")?)]),
        "SendServerCreateShip" => {
            k.ints.extend([
                small(u("arch")?),
                i64::from(i("equip")?),
                i64::from(u("lane")? != 0),
            ]);
            k.pos(arr3(&floats(j, "pos", 3)?), EXACT_POS);
            k.quat(arr4(&floats(j, "q", 4)?));
            k.pos(arr3(&floats(j, "vel", 3)?), EXACT_POS);
            k.bytes(&[quantise_unit(f("throttle")?)]);
        }
        "SendServerCreateSolar" => {
            k.ints.push(small(u("arch")?));
            k.pos(arr3(&floats(j, "pos", 3)?), EXACT_POS);
            k.quat(arr4(&floats(j, "q", 4)?));
        }
        "SendServerCreateLoot" => {
            let items = j.get("items")?.as_array()?;
            k.refs.push(u("owner")?);
            k.ints
                .extend([i64::from(u("kind")? & 0xff), items.len() as i64]);
            if let Some(first) = items.first() {
                let fu = |key: &str| first.get(key).and_then(Json::as_u32);
                k.ints.extend([
                    small(fu("contents")?),
                    i64::from(fu("units")? & 0xffff),
                    small(fu("container")?),
                ]);
                k.pos(
                    arr3(&first.get("pos")?.as_floats().filter(|v| v.len() == 3)?),
                    EXACT_POS,
                );
            }
        }
        "SendServerCreateMine" | "SendServerCreateCounter" => {
            k.refs.push(u("owner")?);
            // The counter-measure's packet carries the archetype in full.
            let arch = if m == "SendServerCreateMine" {
                small(u("arch")?)
            } else {
                i64::from(u("arch")?)
            };
            k.ints.extend([arch, i64::from(u("sub")?)]);
            k.pos(arr3(&floats(j, "pos", 3)?), EXACT_POS);
            k.pos(arr3(&floats(j, "vel", 3)?), EXACT_POS);
        }
        "SendServerCreateGuided" => {
            k.refs.extend([u("owner")?, u("target")?]);
            k.ints.extend([
                small(u("arch")?),
                i64::from(u("sub")?),
                i64::from(u("target_sub")?),
            ]);
            k.pos(arr3(&floats(j, "pos", 3)?), EXACT_POS);
            k.quat(arr4(&floats(j, "q", 4)?));
        }
        "SendServerSetGuidedTarget" => {
            k.refs.push(u("target")?);
            k.ints.push(i64::from(u("target_sub")?));
        }
        "SendServerDestroyObject" => k.ints.push(i64::from(u("kind")? & 0xff)),
        "SendServerDamageObject" => {
            let entries = j.get("entries")?.as_array()?;
            k.refs.push(u("inflictor")?);
            k.ints.extend([
                b("destroyed")?,
                i64::from(i("cause")?.cast_unsigned()),
                entries.len() as i64,
            ]);
            for e in entries {
                let e = e.as_array()?;
                k.ints.extend([
                    i64::from(e.first()?.as_u32()?),
                    i64::from(e.get(2)?.as_i32()?.cast_unsigned() & 0xff),
                ]);
                k.floats.push((e.get(1)?.as_f32()?, HP));
            }
        }
        "SendServerActivateObject" => k.ints.extend([i64::from(u("on")?), i64::from(u("index")?)]),
        "SendServerSetReputation" => {
            k.ints.push(i64::from(i("vibe")?.cast_unsigned()));
            k.ints.push(i64::from(u("affiliation")?));
        }
        "SendServerBurnFuse" => {
            k.refs.push(u("source")?);
            k.ints.extend([
                i64::from(u("fuse")?),
                i64::from(u("sub")?),
                i64::from(u("lit")?),
            ]);
            k.floats.extend([(f("at")?, TIME), (f("lifetime")?, TIME)]);
        }
        "SendServerSystemSwitchOut" => k.refs.push(u("gate")?),
        "SendServerSystemSwitchIn" => {
            k.ints.push(i64::from(u("bits")?));
            k.pos(arr3(&floats(j, "pos", 3)?), EXACT_POS);
            k.quat(arr4(&floats(j, "q", 4)?));
        }
        "SendServerLand" => {
            k.refs.push(u("dock")?);
            k.ints.push(i64::from(u("base")?));
        }
        "SendServerLaunch" => {
            k.refs.push(u("dock")?);
            k.ints.push(i64::from(u("index")?));
            k.pos(arr3(&floats(j, "pos", 3)?), EXACT_POS);
            k.quat(arr4(&floats(j, "q", 4)?));
        }
        _ => return None,
    }
    Some(k)
}

// ---- labels and windows ----------------------------------------------------------------------

/// The short name of a create that makes a numbered label.
fn derived_kind(m: &str) -> Option<&'static str> {
    match m {
        "SendServerCreateGuided" => Some("guided"),
        "SendServerCreateMine" => Some("mine"),
        "SendServerCreateCounter" => Some("counter"),
        "SendServerCreateLoot" => Some("loot"),
        _ => None,
    }
}

/// The labels of one side's ids.
struct Labels {
    named: BTreeMap<u32, String>,
    solars: BTreeSet<u32>,
    derived: BTreeMap<u32, String>,
}

impl Labels {
    fn new(names: &BTreeMap<String, u32>, solars: &BTreeSet<u32>, pkts: &[Pkt]) -> Self {
        let named: BTreeMap<u32, String> = names
            .iter()
            .filter(|(_, id)| **id != 0)
            .map(|(n, id)| (*id, n.clone()))
            .collect();
        let mut out = Self {
            named,
            solars: solars.clone(),
            derived: BTreeMap::new(),
        };
        let mut counts: BTreeMap<String, u32> = BTreeMap::new();
        for p in pkts {
            if p.id == 0 || out.named.contains_key(&p.id) || out.derived.contains_key(&p.id) {
                continue;
            }
            let base = if let Some(kind) = derived_kind(&p.m) {
                let owner = p.refs.first().copied().unwrap_or(0);
                format!("{kind}:{}", out.label(owner))
            } else if p.m == "SendServerCreateShip" {
                "?ship".to_owned()
            } else {
                continue;
            };
            let n = counts.entry(base.clone()).or_default();
            *n += 1;
            out.derived.insert(p.id, format!("{base}#{n}"));
        }
        out
    }

    fn label(&self, id: u32) -> String {
        if id == 0 {
            "none".to_owned()
        } else if let Some(n) = self.named.get(&id) {
            n.clone()
        } else if let Some(d) = self.derived.get(&id) {
            d.clone()
        } else if self.solars.contains(&id) {
            format!("solar:{id}")
        } else {
            format!("?{id}")
        }
    }
}

/// Up to which tick (exclusive) the packets about an object are judged
/// exactly: a scenario object while its state is the oracle's, a solar
/// and "none" always, a munition or loot while its owner's state is and
/// before the first contact or impact, an unknown object never.
fn window(label: &str, divergence: &BTreeMap<String, u32>, cutoff: u32) -> u32 {
    if label == "none" || label.starts_with("solar:") {
        u32::MAX
    } else if label.starts_with('?') {
        0
    } else if let Some((_, rest)) = label.split_once(':') {
        let owner = rest.split('#').next().unwrap_or("?");
        window(owner, divergence, cutoff).min(cutoff)
    } else {
        divergence.get(label).copied().unwrap_or(0)
    }
}

/// A packet's canonical text: kind, labels, discrete fields.
fn key(p: &Pkt, labels: &Labels) -> String {
    let refs: Vec<String> = p.refs.iter().map(|&r| labels.label(r)).collect();
    format!(
        "{} {} [{}] {:?}",
        p.m,
        labels.label(p.id),
        refs.join(","),
        p.ints
    )
}

/// The first tick each scenario object's state differs from the oracle's,
/// bit for bit (`same_state`); `u32::MAX` when it never does.
fn divergences(ours: &Run, theirs: &Run) -> BTreeMap<String, u32> {
    let mut out = BTreeMap::new();
    for (name, a) in &ours.states {
        let Some(b) = theirs.states.get(name) else {
            out.insert(name.clone(), 0);
            continue;
        };
        let n = a.len().min(b.len());
        let first = (0..n).find(|&t| !same_state(&a[t], &b[t])).unwrap_or(n);
        let tick = if first == n && a.len() == b.len() {
            u32::MAX
        } else {
            u32::try_from(first).unwrap_or(u32::MAX)
        };
        out.insert(name.clone(), tick);
    }
    out
}

// ---- the run ---------------------------------------------------------------------------------

fn scenario_path() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../CppSource/Oracle/replication.txt")
}

/// The `player <session> <ship>` lines, in file order.
fn player_lines(path: &Path) -> Vec<(u32, String)> {
    let text = fs::read_to_string(path).unwrap_or_default();
    text.lines()
        .filter_map(|l| {
            let t: Vec<&str> = l.split('#').next()?.split_whitespace().collect();
            match t.as_slice() {
                [k, session, ship] if k.eq_ignore_ascii_case("player") => {
                    Some((session.parse().ok()?, (*ship).to_owned()))
                }
                _ => None,
            }
        })
        .collect()
}

fn game_data(root: &Path) -> Option<Arc<GameData>> {
    let config = DataConfig::read(&Host::new(root.join("EXE")), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(root.join("DATA")), &config, None);
    Some(Arc::new(data))
}

/// The port's run: the `Run` of the driver, the packets, the scenario
/// objects' ids and the system's solars.
struct Ours {
    run: Run,
    pkts: Vec<Pkt>,
    ids: BTreeMap<String, u32>,
    solars: BTreeSet<u32>,
    refused: Vec<String>,
}

fn run_port(
    root: &Path,
    data: Arc<GameData>,
    s: &Scenario,
    ai: &game_sim::scenario::AiInputs,
) -> Ours {
    let mut world = World::new(data, world_config(s)).with_assets(Assets::new(root.join("DATA")));
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 16, true);
    // The oracle's start-up: its reduced [Initial MP DLLs] (Content's
    // GameSupport, SpaceSupport and BaseSupport), then
    // `Server_LoadWorldState` over `InitialWorld.ini`, whose `LoadGame`
    // builds the system records that SpaceSupport fully loads on
    // `SystemLoaded`.
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for (name, priority) in [
        (&b"GameSupport"[..], Priority::Highest),
        (&b"SpaceSupport"[..], Priority::Normal),
        (&b"BaseSupport"[..], Priority::Normal),
    ] {
        let id = neuron_server::controller::create(
            &mut cx,
            b"Content.dll",
            name,
            &CreateParams::None,
            priority,
        );
        assert_ne!(id.0, 0, "{}", String::from_utf8_lossy(name));
    }
    let initial_world =
        game_sim::data::DataSource::read(&Host::new(root.join("DATA")), b"InitialWorld.ini")
            .unwrap_or_default();
    let mut reader = neuron_core::ini::IniReader::new();
    reader.open_memory(&initial_world);
    game_logic::selection::load_world_state(&mut cx, &mut reader);
    let mut server = ReplicationServer {
        hooks: Hooks { logic: &mut logic },
        sink,
        players: player_lines(&scenario_path()),
        sent: Vec::new(),
        ids: BTreeMap::new(),
        solars: BTreeSet::new(),
        refused: Vec::new(),
    };
    let run = run_with(world, s, ai, &mut server);
    let pkts = server
        .sent
        .iter()
        .filter_map(|(tick, client, m)| rust_pkt(*tick, *client, m))
        .collect();
    Ours {
        run,
        pkts,
        ids: server.ids,
        solars: server.solars,
        refused: server.refused,
    }
}

/// The oracle's record of the first run, its spawned objects' ids, its
/// players, and how its second run's record compares.
struct Theirs {
    pkts: Vec<Pkt>,
    unread: usize,
    ids: BTreeMap<String, u32>,
    joined: Vec<(u32, String, bool)>,
    repeat: String,
}

fn read_oracle(dir: &Path, data: &GameData) -> Option<Theirs> {
    let files = find_files(&dir.join(SCENARIO), "replication.jsonl");
    let first = files.first()?;
    let lines = read_lines(first);
    let mut pkts = Vec::new();
    let mut unread = 0;
    for j in &lines {
        match oracle_pkt(j, data) {
            Some(p) => pkts.push(p),
            None => unread += 1,
        }
    }
    let mut ids = BTreeMap::new();
    let mut joined = Vec::new();
    if let Some(session) = first.parent().map(|d| d.join("session.jsonl"))
        && session.exists()
    {
        for j in read_lines(&session) {
            let rec = j.get("rec").and_then(Json::as_str);
            let obj = j.get("obj").and_then(Json::as_str).map(str::to_owned);
            let id = j.get("id").and_then(Json::as_u32);
            match (rec, obj, id) {
                (Some("spawn"), Some(obj), Some(id)) => {
                    ids.insert(obj, id);
                }
                (Some("player"), Some(obj), Some(_)) => joined.push((
                    j.get("session").and_then(Json::as_u32).unwrap_or(0),
                    obj,
                    j.get("joined").and_then(Json::as_bool).unwrap_or(false),
                )),
                _ => {}
            }
        }
    }
    let repeat = match files.get(1) {
        None => "the oracle wrote one run".to_owned(),
        Some(second) => {
            let a = fs::read_to_string(first).unwrap_or_default();
            let b = fs::read_to_string(second).unwrap_or_default();
            match a.lines().zip(b.lines()).position(|(x, y)| x != y) {
                None if a.lines().count() == b.lines().count() => {
                    format!(
                        "the oracle's two runs wrote the same {} lines",
                        a.lines().count()
                    )
                }
                None => format!(
                    "the oracle's runs differ in length: {} and {} lines",
                    a.lines().count(),
                    b.lines().count()
                ),
                Some(n) => format!("the oracle's runs first differ at line {}", n + 1),
            }
        }
    };
    Some(Theirs {
        pkts,
        unread,
        ids,
        joined,
        repeat,
    })
}

/// The per-object create and destroy ticks of one side, by client.
fn lifetimes(pkts: &[Pkt], labels: &Labels) -> BTreeMap<(u32, String), (Vec<u32>, Vec<u32>)> {
    let mut out: BTreeMap<(u32, String), (Vec<u32>, Vec<u32>)> = BTreeMap::new();
    for p in pkts {
        let label = labels.label(p.id);
        if label.starts_with("solar:") || label == "none" {
            continue;
        }
        let e = out.entry((p.client, label)).or_default();
        if p.m.starts_with("SendServerCreate") {
            e.0.push(p.tick);
        } else if p.m == "SendServerDestroyObject" {
            e.1.push(p.tick);
        }
    }
    out
}

/// One object's updates on one client, both sides.
#[derive(Default)]
struct UpdateError {
    /// The largest distance between the two sides' updates of a tick.
    max: f64,
    /// Its tick.
    tick: u32,
    /// The port's updates.
    ours: usize,
    /// The oracle's.
    theirs: usize,
    /// The ticks both sent one.
    both: usize,
}

fn fmt_ticks(t: &[u32]) -> String {
    if t.len() <= 6 {
        format!("{t:?}")
    } else {
        format!("{:?}.. ({} in all)", &t[..6], t.len())
    }
}

#[test]
fn replication_against_the_oracle() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the replication comparison was skipped");
        return;
    };
    let Some(dir) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_ORACLE_DIR is not set; the replication comparison was skipped");
        return;
    };
    let Some(data) = game_data(&root) else {
        panic!("no EXE/OutpostFrontier.ini under {}", root.display());
    };
    let path = scenario_path();
    let s = parse(&path).expect("CppSource/Oracle/replication.txt parses");
    let ai = oracle_ai_inputs(&dir, SCENARIO);
    let ours = run_port(&root, data.clone(), &s, &ai);

    let mut summary = String::new();
    let mut gross: Vec<String> = Vec::new();
    let players: BTreeMap<u32, String> = player_lines(&path).into_iter().collect();
    for r in &ours.refused {
        gross.push(format!("port: {r}"));
    }
    let _ = writeln!(
        summary,
        "{SCENARIO}: {} ticks, players {players:?}; the port sent {} packets of the compared kinds",
        s.ticks,
        ours.pkts.len()
    );

    // Without the oracle's record the port's side is still reported.
    let oracle = read_oracle(&dir, &data);
    let their_run = oracle_run(&dir, SCENARIO);
    let missing = oracle.is_none() || their_run.is_none();
    if missing {
        gross.push(format!(
            "the oracle's replication.jsonl or tick.jsonl is missing under {}",
            dir.join(SCENARIO).display()
        ));
    }
    let theirs = oracle.unwrap_or(Theirs {
        pkts: Vec::new(),
        unread: 0,
        ids: BTreeMap::new(),
        joined: Vec::new(),
        repeat: "no oracle run".to_owned(),
    });
    let their_run = their_run.unwrap_or_default();
    let _ = writeln!(
        summary,
        "oracle: {} packets ({} lines of other kinds or unread); {}",
        theirs.pkts.len(),
        theirs.unread,
        theirs.repeat
    );
    for (session, obj, ok) in &theirs.joined {
        if !ok {
            gross.push(format!("oracle: player {session} ({obj}) did not join"));
        }
    }
    if theirs.joined.len() != players.len() {
        gross.push(format!(
            "oracle: {} players joined, the scenario has {}",
            theirs.joined.len(),
            players.len()
        ));
    }

    // Labels, divergence ticks and the exact windows.
    let our_labels = Labels::new(&ours.ids, &ours.solars, &ours.pkts);
    let their_labels = Labels::new(&theirs.ids, &ours.solars, &theirs.pkts);
    let divergence = divergences(&ours.run, &their_run);
    let cutoff = [
        first_event(&dir, SCENARIO, &their_run),
        first_event_of(&ours.run, &ours.run.guided),
    ]
    .into_iter()
    .flatten()
    .min()
    .unwrap_or(u32::MAX);
    let _ = writeln!(
        summary,
        "first contact or impact: tick {}",
        if cutoff == u32::MAX {
            "none".to_owned()
        } else {
            cutoff.to_string()
        }
    );
    for (name, t) in &divergence {
        let _ = writeln!(
            summary,
            "{name}: bit for bit the oracle's {}",
            if *t == u32::MAX {
                "for the whole run".to_owned()
            } else {
                format!("up to tick {t} (exclusive)")
            }
        );
    }
    let viewer = |client: u32| {
        players
            .get(&client)
            .map_or(0, |n| window(n, &divergence, cutoff))
    };
    let exact_until = |p: &Pkt, labels: &Labels| {
        let mut w = viewer(p.client).min(window(&labels.label(p.id), &divergence, cutoff));
        for &r in &p.refs {
            w = w.min(window(&labels.label(r), &divergence, cutoff));
        }
        w
    };

    // Sanity: every player's client gets creates on both sides.
    for &client in players.keys() {
        for (side, pkts) in [("oracle", &theirs.pkts), ("port", &ours.pkts)] {
            if !pkts
                .iter()
                .any(|p| p.client == client && p.m.starts_with("SendServerCreate"))
            {
                gross.push(format!("{side}: client {client} was sent no create"));
            }
        }
    }

    // The exact window, per client and tick.
    type Group<'a> = BTreeMap<(u32, u32), Vec<&'a Pkt>>;
    let mut ours_g: Group = BTreeMap::new();
    let mut theirs_g: Group = BTreeMap::new();
    for p in &ours.pkts {
        if p.tick < exact_until(p, &our_labels) && JUDGED.contains(&p.m.as_str()) {
            ours_g.entry((p.client, p.tick)).or_default().push(p);
        }
    }
    for p in &theirs.pkts {
        if p.tick < exact_until(p, &their_labels) && JUDGED.contains(&p.m.as_str()) {
            theirs_g.entry((p.client, p.tick)).or_default().push(p);
        }
    }
    let unknown_in = |pkts: &[Pkt], labels: &Labels| -> Vec<String> {
        let mut ids: BTreeSet<String> = BTreeSet::new();
        for p in pkts {
            let label = labels.label(p.id);
            if label.starts_with('?') {
                ids.insert(format!("{label} ({})", p.m));
            }
        }
        ids.into_iter().collect()
    };
    let unknown = (
        unknown_in(&ours.pkts, &our_labels),
        unknown_in(&theirs.pkts, &their_labels),
    );
    let groups: BTreeSet<(u32, u32)> = ours_g.keys().chain(theirs_g.keys()).copied().collect();
    let (mut matched, mut mismatched, mut order_differs) = (0usize, Vec::new(), 0usize);
    for g in &groups {
        let a = ours_g.get(g).cloned().unwrap_or_default();
        let b = theirs_g.get(g).cloned().unwrap_or_default();
        let ka: Vec<String> = a.iter().map(|p| key(p, &our_labels)).collect();
        let kb: Vec<String> = b.iter().map(|p| key(p, &their_labels)).collect();
        if ka != kb {
            order_differs += 1;
        }
        let mut ia: Vec<usize> = (0..a.len()).collect();
        let mut ib: Vec<usize> = (0..b.len()).collect();
        ia.sort_by(|&x, &y| ka[x].cmp(&ka[y]));
        ib.sort_by(|&x, &y| kb[x].cmp(&kb[y]));
        let (mut x, mut y) = (0, 0);
        while x < ia.len() || y < ib.len() {
            let ord = match (ia.get(x), ib.get(y)) {
                (Some(&i), Some(&j)) => ka[i].cmp(&kb[j]),
                (Some(_), None) => std::cmp::Ordering::Less,
                _ => std::cmp::Ordering::Greater,
            };
            match ord {
                std::cmp::Ordering::Equal => {
                    let (pa, pb) = (a[ia[x]], b[ib[y]]);
                    let bad: Vec<String> = pa
                        .floats
                        .iter()
                        .zip(&pb.floats)
                        .enumerate()
                        .filter(|(_, ((va, ta), (vb, tb)))| {
                            !((va - vb).abs() <= ta.max(*tb) || va.to_bits() == vb.to_bits())
                        })
                        .map(|(n, ((va, _), (vb, _)))| format!("#{n} {va} vs {vb}"))
                        .collect();
                    if bad.is_empty() && pa.floats.len() == pb.floats.len() {
                        matched += 1;
                    } else {
                        mismatched.push(format!(
                            "client {} tick {}: {} differs: {}",
                            g.0,
                            g.1,
                            ka[ia[x]],
                            bad.join(", ")
                        ));
                    }
                    x += 1;
                    y += 1;
                }
                std::cmp::Ordering::Less => {
                    mismatched.push(format!(
                        "client {} tick {}: port only: {}",
                        g.0, g.1, ka[ia[x]]
                    ));
                    x += 1;
                }
                std::cmp::Ordering::Greater => {
                    mismatched.push(format!(
                        "client {} tick {}: oracle only: {}",
                        g.0, g.1, kb[ib[y]]
                    ));
                    y += 1;
                }
            }
        }
    }
    let _ = writeln!(
        summary,
        "exact window: {matched} packets equal, {} differ, {order_differs} of {} client ticks in another order (reported: D622, D651, D730, D857)",
        mismatched.len(),
        groups.len()
    );
    // The differences by side and packet kind, with the first few of each,
    // so one kind's many lines (the join's reputations) hide no other.
    let mut by_kind: std::collections::BTreeMap<(String, String), Vec<&String>> =
        std::collections::BTreeMap::new();
    for m in &mismatched {
        let (side, rest) = if let Some(r) = m.split_once(": port only: ") {
            ("port only", r.1)
        } else if let Some(r) = m.split_once(": oracle only: ") {
            ("oracle only", r.1)
        } else {
            ("differs", m.split_once(": ").map_or(m.as_str(), |r| r.1))
        };
        let kind = rest.split_whitespace().next().unwrap_or("?").to_owned();
        by_kind.entry((kind, side.to_owned())).or_default().push(m);
    }
    for ((kind, side), list) in &by_kind {
        let _ = writeln!(summary, "  {kind} {side}: {}", list.len());
        for m in list.iter().take(5) {
            let _ = writeln!(summary, "    {m}");
        }
    }
    if !mismatched.is_empty() {
        gross.push(format!(
            "{} packets differ in the exact window (first: {})",
            mismatched.len(),
            mismatched[0]
        ));
    }
    for (side, list) in [("port", &unknown.0), ("oracle", &unknown.1)] {
        if !list.is_empty() {
            let shown: Vec<&str> = list.iter().take(10).map(String::as_str).collect();
            gross.push(format!(
                "{side}: packets about {} objects of no known origin: {}",
                list.len(),
                shown.join(", ")
            ));
        }
    }

    // The whole run, reported: counts per client and kind.
    let mut counts: BTreeMap<(u32, String), (usize, usize)> = BTreeMap::new();
    for p in &ours.pkts {
        counts.entry((p.client, p.m.clone())).or_default().0 += 1;
    }
    for p in &theirs.pkts {
        counts.entry((p.client, p.m.clone())).or_default().1 += 1;
    }
    for ((client, m), (a, b)) in &counts {
        let _ = writeln!(summary, "client {client} {m}: port {a}, oracle {b}");
    }

    // The objects' creates and destroys on each client.
    let la = lifetimes(&ours.pkts, &our_labels);
    let lb = lifetimes(&theirs.pkts, &their_labels);
    let keys: BTreeSet<&(u32, String)> = la.keys().chain(lb.keys()).collect();
    for k in keys {
        let empty = (Vec::new(), Vec::new());
        let (a, b) = (la.get(k).unwrap_or(&empty), lb.get(k).unwrap_or(&empty));
        if a.0.is_empty() && b.0.is_empty() && a.1.is_empty() && b.1.is_empty() {
            continue;
        }
        let _ = writeln!(
            summary,
            "client {} {}: created {} vs {}, destroyed {} vs {}",
            k.0,
            k.1,
            fmt_ticks(&a.0),
            fmt_ticks(&b.0),
            fmt_ticks(&a.1),
            fmt_ticks(&b.1)
        );
    }

    // The updates of the same object in the same tick.
    let updates = |pkts: &[Pkt], labels: &Labels| -> BTreeMap<(u32, String, u32), [f32; 3]> {
        pkts.iter()
            .filter(|p| p.m == "SendCommonUpdateObject" && p.floats.len() >= 3)
            .map(|p| {
                (
                    (p.client, labels.label(p.id), p.tick),
                    [p.floats[0].0, p.floats[1].0, p.floats[2].0],
                )
            })
            .collect()
    };
    let (ua, ub) = (
        updates(&ours.pkts, &our_labels),
        updates(&theirs.pkts, &their_labels),
    );
    let mut errors: BTreeMap<(u32, String), UpdateError> = BTreeMap::new();
    for ((client, label, tick), pa) in &ua {
        let e = errors.entry((*client, label.clone())).or_default();
        e.ours += 1;
        if let Some(pb) = ub.get(&(*client, label.clone(), *tick)) {
            let d = pa
                .iter()
                .zip(pb)
                .map(|(x, y)| (f64::from(*x) - f64::from(*y)).powi(2))
                .sum::<f64>()
                .sqrt();
            e.both += 1;
            if d > e.max {
                e.max = d;
                e.tick = *tick;
            }
        }
    }
    for (client, label, _) in ub.keys() {
        errors.entry((*client, label.clone())).or_default().theirs += 1;
    }
    for ((client, label), e) in &errors {
        let _ = writeln!(
            summary,
            "client {client} {label} updates: port {}, oracle {}, {} in the same tick, largest distance {:.4} m (tick {})",
            e.ours, e.theirs, e.both, e.max, e.tick
        );
    }

    // The trajectories, as scenario_hooks judges them (reported here).
    let mut trajectory = Vec::new();
    if let Some((behavior, tick)) = ours.run.unported {
        let _ = writeln!(
            summary,
            "an AI ship runs behaviour {behavior} from tick {tick}, which is not ported"
        );
    }
    if !missing {
        judge(
            &mut summary,
            &mut trajectory,
            &dir,
            &s,
            &ai,
            &ours.run,
            &their_run,
        );
    }
    let _ = writeln!(
        summary,
        "trajectories (reported, scenario_hooks judges them): {}",
        if trajectory.is_empty() {
            "no gross divergence".to_owned()
        } else {
            trajectory.join("; ")
        }
    );
    let _ = writeln!(
        summary,
        "gross divergences: {}",
        if gross.is_empty() {
            "none".to_owned()
        } else {
            gross.join("; ")
        }
    );
    print!("{summary}");
    let _ = fs::write(dir.join("rust-replication.txt"), &summary);
    assert!(gross.is_empty(), "gross divergence: {gross:?}");
}

/// A float as `oracle_json.h` writes it.
fn hex(v: f32) -> String {
    format!("\"{:08x}\"", v.to_bits())
}

fn hexes(v: &[f32]) -> String {
    let parts: Vec<String> = v.iter().map(|&x| hex(x)).collect();
    format!("[{}]", parts.join(","))
}

/// The two readers agree: a line as `sim_client_stub_replication.cpp`
/// writes it and the port's packet of the same values give the same key
/// and floats. Needs no data (the empty archetype table makes every small
/// id 0 on both sides).
#[test]
fn the_record_and_the_port_read_alike() {
    use game_sim::protocol::common::{FireComponent, FireWeapon, GoTradeLane, UpdateObject};
    use game_sim::protocol::records::{CreateObject, DamageEntry, DamageList};
    use game_sim::protocol::to_client::{
        BurnFuse, CreateGuided, CreateShip, DamageObject, DestroyObject,
    };

    let data = GameData::new();
    let solars: BTreeSet<u32> = [500].into();
    let names: BTreeMap<String, u32> = [("police".to_owned(), 7), ("rogue".to_owned(), 8)].into();
    let q = [0.5f32, -0.5, 0.5, 0.5];
    let pos = [1.5f32, -2.25, 3e4];
    let line = |m: &str, id: u32, rest: &str| {
        format!(r#"{{"tick":4,"seq":11,"client":2,"m":"{m}","id":{id},{rest}}}"#)
    };
    let cases: Vec<(String, ToClient)> = vec![
        (
            line(
                "SendCommonUpdateObject",
                7,
                &format!(
                    r#""pos":{},"q":{},"time":{},"throttle":{},"behavior":3"#,
                    hexes(&pos),
                    hexes(&q),
                    hex(0.125),
                    hex(0.75)
                ),
            ),
            ToClient::Common(CommonMessage::UpdateObject(UpdateObject {
                flags: 3,
                object_id: 7,
                position: pos,
                orientation: quantise_orientation(q),
                extra: quantise_unit(0.75),
                update_time: 0.125,
            })),
        ),
        (
            line(
                "SendCommonFireWeapon",
                7,
                &format!(r#""target":{},"guns":[3,4]"#, hexes(&pos)),
            ),
            ToClient::Common(CommonMessage::FireWeapon(FireWeapon {
                object_id: 7,
                target: pos.map(FireComponent::Full),
                hardpoints: vec![3, 4],
            })),
        ),
        (
            line(
                "SendCommonGoTradeLane",
                8,
                r#""ring1":500,"ring2":501,"flag":false"#,
            ),
            ToClient::Common(CommonMessage::GoTradeLane(GoTradeLane {
                object_id: 8,
                tradelane_ring1: 500,
                tradelane_ring2: 501,
                field_0c: 0,
            })),
        ),
        (
            line(
                "SendServerCreateShip",
                8,
                &format!(
                    r#""arch":123,"pos":{},"q":{},"vel":{},"angvel":{},"hp":400,"throttle":{},"owner":0,"lane":0,"equip":5"#,
                    hexes(&pos),
                    hexes(&q),
                    hexes(&[0.0, 0.0, -80.0]),
                    hexes(&[0.0, 0.0, 0.0]),
                    hex(1.0)
                ),
            ),
            ToClient::Server(ServerMessage::CreateShip(CreateShip {
                object: CreateObject {
                    obj_id: 8,
                    position: pos,
                    orientation: quantise_orientation(q),
                    equipment: vec![Default::default(); 5],
                    ..CreateObject::default()
                },
                velocity: [0.0, 0.0, -80.0],
                throttle: quantise_unit(1.0),
                ..CreateShip::default()
            })),
        ),
        (
            line("SendServerDestroyObject", 8, r#""kind":1"#),
            ToClient::Server(ServerMessage::DestroyObject(DestroyObject {
                object_id: 8,
                field_04: 1,
            })),
        ),
        (
            line(
                "SendServerDamageObject",
                8,
                &format!(
                    r#""destroyed":true,"cause":5,"inflictor":7,"entries":[[1,{},1],[65521,{},0]]"#,
                    hex(0.0),
                    hex(120.5)
                ),
            ),
            ToClient::Server(ServerMessage::DamageObject(DamageObject {
                object_id: 8,
                damage: DamageList {
                    destroyed: 1,
                    cause: 5,
                    inflictor_id: 7,
                    entries: vec![
                        DamageEntry {
                            sub_obj_id: 1,
                            fate: 1,
                            hit_points: 0.0,
                        },
                        DamageEntry {
                            sub_obj_id: 65521,
                            fate: 0,
                            hit_points: 120.5,
                        },
                    ],
                },
            })),
        ),
        (
            line(
                "SendServerCreateGuided",
                9,
                &format!(
                    r#""arch":77,"owner":7,"sub":12,"target":8,"target_sub":1,"pos":{},"q":{}"#,
                    hexes(&pos),
                    hexes(&q)
                ),
            ),
            ToClient::Server(ServerMessage::CreateGuided(CreateGuided {
                archetype: 0,
                owner_id: 7,
                sub_obj_id: 12,
                target_id: 8,
                target_sub_obj_id: 1,
                obj_id: 9,
                position: pos,
                orientation: quantise_orientation(q),
            })),
        ),
        (
            line(
                "SendServerBurnFuse",
                8,
                &format!(
                    r#""source":7,"fuse":4242,"sub":65535,"at":{},"lifetime":{},"lit":1"#,
                    hex(0.0),
                    hex(-1.0)
                ),
            ),
            ToClient::Server(ServerMessage::BurnFuse(BurnFuse {
                source_id: 7,
                object_id: 8,
                fuse_id: 4242,
                sub_obj_id: 65535,
                at_t: 0.0,
                lifetime: -1.0,
                lit: 1,
            })),
        ),
    ];
    let read = |text: &str| {
        let j = game_sim::scenario::records::parse(text).unwrap();
        oracle_pkt(&j, &data).unwrap_or_else(|| panic!("the record line does not read: {text}"))
    };
    for (text, message) in &cases {
        let theirs = read(text);
        let ours = rust_pkt(4, 2, message).expect("a compared kind");
        let their_labels = Labels::new(&names, &solars, std::slice::from_ref(&theirs));
        let our_labels = Labels::new(&names, &solars, std::slice::from_ref(&ours));
        assert_eq!(
            key(&ours, &our_labels),
            key(&theirs, &their_labels),
            "{text}"
        );
        assert_eq!(ours.floats.len(), theirs.floats.len(), "{text}");
        for (a, b) in ours.floats.iter().zip(&theirs.floats) {
            assert!(
                (a.0 - b.0).abs() <= a.1.max(b.1),
                "{text}: {} vs {}",
                a.0,
                b.0
            );
        }
        assert_eq!((ours.tick, ours.client), (theirs.tick, theirs.client));
    }
    // A missile is numbered by its owner; an unknown id is unknown.
    let guided = read(&cases[6].0);
    let labels = Labels::new(&names, &solars, std::slice::from_ref(&guided));
    assert_eq!(labels.label(9), "guided:police#1");
    assert_eq!(labels.label(500), "solar:500");
    assert_eq!(labels.label(31), "?31");
    let div: BTreeMap<String, u32> = [("police".to_owned(), 200), ("rogue".to_owned(), 150)].into();
    assert_eq!(window("guided:police#1", &div, 180), 180);
    assert_eq!(window("rogue", &div, 180), 150);
    assert_eq!(window("solar:500", &div, 180), u32::MAX);
    assert_eq!(window("?31", &div, 180), 0);
}
