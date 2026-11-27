//! P3.3's `pub::SpaceObj` over the original data (`outdir/`, the
//! rebranded he01): the getters' answers and result codes for he01's
//! planet he01_01, the station he01_14 and its shipyard, the gate
//! he01_to_he02, the trade-lane rings and an NPC fighter made through
//! `Create`; the setters and actions (comm, health, shields, fuses,
//! impulses, docking points, equipment, invincibility, jettison, loot,
//! solars, destruction, launch, space scripts) with the messages the
//! players hear; and the empty-position searches. The expected values
//! follow the C++ logic of `Server/server_pub.cpp`, `server_classes.cpp`,
//! `server_gameobject.cpp` and Common.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::PubError;
use game_logic::api::player::CargoLine;
use game_logic::api::space_obj::{
    self as so, CommRequest, EqInfo, InvincibleState, LootInfo, ShipInfo, SolarInfo, TerminalInfo,
};
use game_logic::events::ControllerMessage;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::equip::{EquipKind, SUBOBJ_ID_NONE, types};
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{Command, KindData, ObjHandle, World};
use game_sim::protocol::ToClient;
use game_sim::protocol::common::CommonMessage;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::{IdString, create_id};
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::num::{trunc_to_i32, trunc_to_i32_via_i64};
use neuron_core::vfs::Host;

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

impl Shared {
    fn take(&self) -> Vec<(u32, ToClient)> {
        std::mem::take(&mut *self.0.borrow_mut())
    }
}

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn setup(what: &str, sink: &Shared) -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(sink.clone()), 4, true);
    logic.deliver_hook = Some(|_, _, _, message| {
        let line = match message {
            ControllerMessage::LootSpawned {
                loot,
                source,
                good,
                count,
            } => format!("LootSpawned {loot} {source} {good} {count}"),
            ControllerMessage::ObjectDestroyed(e) => {
                format!("ObjectDestroyed {} {} {}", e.id, e.system, e.kind)
            }
            other => other.name().to_owned(),
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    Some((world, logic))
}

fn he01() -> u32 {
    create_id(b"he01")
}

/// A Content controller, which owns the NPC ships.
fn content(cx: &mut Cx<'_>) -> u32 {
    neuron_server::controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        neuron_server::controller::Priority::Highest,
    )
    .0
}

fn solar(cx: &Cx<'_>, nickname: &[u8]) -> ObjHandle {
    cx.world.find(create_id(nickname)).expect("the solar")
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).unwrap().simple.id
}

/// Player 1 in he01 with a fighter of its own at `at`; he01 loads.
fn player(cx: &mut Cx<'_>, at: Vector) -> ObjHandle {
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.client_id = 1;
        slot.system_id = he01();
    }
    world::add_player(cx, he01(), 1);
    let mut spawn = ShipSpawn::new(
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    spawn.owner_player = 1;
    let h = cx.world.spawn_ship(he01(), &spawn).expect("the ship");
    let ship_id = id(cx, h);
    cx.logic.players.get_mut(1).unwrap().ship_object_id = ship_id;
    let _ = cx.logic.objects.entry(cx.world, h);
    h
}

/// An NPC fighter made through `pub::SpaceObj::Create`.
fn fighter(cx: &mut Cx<'_>, controller: u32, at: Vector, velocity: Vector) -> ObjHandle {
    let mut info = ShipInfo::new(
        controller,
        he01(),
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    info.velocity = velocity;
    let ship = so::create(cx, &info).expect("the ship is made");
    cx.world.find(ship).unwrap()
}

/// `Server_PhysicsUpdate` and the global time.
fn tick(cx: &mut Cx<'_>, dt: f32) {
    world::physics_update(cx, dt);
    cx.world.update_global_time(dt);
}

fn server_messages(sink: &Shared) -> Vec<(u32, ServerMessage)> {
    sink.take()
        .into_iter()
        .filter_map(|(c, m)| match m {
            ToClient::Server(s) => Some((c, s)),
            ToClient::Common(_) => None,
        })
        .collect()
}

/// The equipped part's items of a type that are not destroyed.
fn items(cx: &Cx<'_>, h: ObjHandle, mask: u32) -> Vec<game_sim::equip::Equip> {
    let eq = cx.world.get(h).unwrap().equipped.as_deref().unwrap();
    eq.equipment
        .traverse(mask, false)
        .into_iter()
        .filter_map(|slot| eq.equipment.get(slot).cloned())
        .collect()
}

#[test]
fn spaceobj_getters_over_he01() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("pub::SpaceObj getters", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    player(&mut cx, Vector::new(20000.0, 0.0, 20000.0));
    let npc = fighter(
        &mut cx,
        controller,
        Vector::new(21000.0, 0.0, 20000.0),
        Vector::new(10.0, 0.0, 0.0),
    );
    let npc_id = id(&cx, npc);
    let planet = solar(&cx, b"he01_01");
    let station = solar(&cx, b"he01_14");
    let shipyard = solar(&cx, b"he01_14_shipyard_1");
    let gate = solar(&cx, b"he01_to_he02");
    let ring = solar(&cx, b"he01_Trade_Lane_Ring_17");
    let sun = solar(&cx, b"Sun_he01");
    let (planet_id, station_id, gate_id, ring_id) = (
        id(&cx, planet),
        id(&cx, station),
        id(&cx, gate),
        id(&cx, ring),
    );
    let unknown = create_id(b"no_such_object");

    // Location, system, type.
    let (at, orientation) = so::location(&cx, planet_id).unwrap();
    assert_eq!(at, Vector::new(-33270.0, 0.0, -33039.0));
    assert_eq!(
        orientation,
        cx.world.get(planet).unwrap().engine.orientation
    );
    assert_eq!(so::location(&cx, 0), Err(PubError::BAD_ARGUMENT));
    assert_eq!(so::location(&cx, 0x4000_0000), Err(PubError::BAD_ARGUMENT));
    assert_eq!(so::location(&cx, unknown), Err(PubError::BAD_ID));
    assert_eq!(so::system(&cx, planet_id), Ok(he01()));
    assert_eq!(so::system(&cx, npc_id), Ok(he01()));
    assert_eq!(so::system(&cx, unknown), Err(PubError::BAD_ID));
    assert_eq!(so::object_type(&cx, gate_id), Ok(0x40));
    assert_eq!(so::object_type(&cx, station_id), Ok(0x100));
    assert_eq!(so::object_type(&cx, ring_id), Ok(0x80));
    assert_eq!(so::object_type(&cx, planet_id), Ok(2));
    assert_eq!(so::object_type(&cx, npc_id), Ok(0x1_0000));
    assert_eq!(so::object_type(&cx, unknown), Err(PubError::BAD_ID));

    // Atmosphere: the planet's range less its radius; a station has none.
    let planet_radius = cx.world.get(planet).unwrap().engine.radius;
    let sun_radius = cx.world.get(sun).unwrap().engine.radius;
    assert_eq!(
        so::atmosphere_range(&cx, planet_id),
        Ok((4200.0f64 - f64::from(planet_radius)) as f32)
    );
    assert_eq!(
        so::atmosphere_range(&cx, id(&cx, sun)),
        Ok((11000.0f64 - f64::from(sun_radius)) as f32)
    );
    assert_eq!(so::atmosphere_range(&cx, station_id), Ok(0.0));
    assert_eq!(so::atmosphere_range(&cx, 0), Err(PubError::BAD_ARGUMENT));
    assert_eq!(so::burn_radius(&cx, planet_id), Ok((4200.0, at)));
    assert_eq!(
        so::burn_radius(&cx, station_id),
        Ok((0.0, Vector::new(5752.0, 0.0, -52385.0)))
    );

    // The gate's destination; anything but a gate, hole or airlock is -1.
    assert_eq!(
        so::jump_target(&cx, gate_id),
        Ok((create_id(b"he02"), create_id(b"he02_to_he01")))
    );
    assert_eq!(so::jump_target(&cx, planet_id), Err(PubError::FAILED));
    assert_eq!(so::jump_target(&cx, npc_id), Err(PubError::FAILED));
    assert_eq!(so::jump_target(&cx, unknown), Err(PubError::BAD_ID));

    // The rings, the parent, the docking target.
    assert_eq!(
        so::tradelane_next_and_prev(&cx, ring_id),
        Ok((
            create_id(b"he01_Trade_Lane_Ring_18"),
            create_id(b"he01_Trade_Lane_Ring_16")
        ))
    );
    assert_eq!(
        so::tradelane_next_and_prev(&cx, npc_id),
        Err(PubError::FAILED)
    );
    assert_eq!(
        so::solar_parent(&cx, id(&cx, shipyard)),
        Ok(create_id(b"he01_14"))
    );
    assert_eq!(so::solar_parent(&cx, station_id), Err(PubError::NOT_FOUND));
    assert_eq!(so::solar_parent(&cx, npc_id), Err(PubError::FAILED));
    assert_eq!(so::solar_parent(&cx, 0), Err(PubError::BAD_ARGUMENT));
    assert_eq!(
        so::docking_target(&cx, station_id),
        Ok(create_id(b"he01_14_base"))
    );
    // The planet docks with no point of its own: its base.
    assert_eq!(
        so::docking_target(&cx, planet_id),
        Ok(create_id(b"he01_01_Base"))
    );
    assert_eq!(so::docking_target(&cx, npc_id), Ok(0));

    // The station's docking spheres (space_factory01).
    assert_eq!(
        so::terminal_info(&cx, station_id, 0),
        Ok(TerminalInfo {
            name: b"hpdockmounta".to_vec(),
            value: 1
        })
    );
    assert_eq!(
        so::terminal_info(&cx, station_id, 3),
        Ok(TerminalInfo {
            name: b"hpdockmountd".to_vec(),
            value: 4
        })
    );
    assert_eq!(
        so::terminal_info(&cx, station_id, 5),
        Ok(TerminalInfo {
            name: b"hpdockmountf".to_vec(),
            value: 5
        })
    );
    assert_eq!(
        so::terminal_info(&cx, station_id, 6),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(
        so::terminal_info(&cx, station_id, -1),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(
        so::terminal_info(&cx, gate_id, 0),
        Ok(TerminalInfo {
            name: b"hpdockmounta".to_vec(),
            value: 7
        })
    );
    assert_eq!(so::terminal_info(&cx, 0, 0), Err(PubError::BAD_ID));

    // Reputation, archetype, voice.
    let station_vibe = cx
        .world
        .get(station)
        .unwrap()
        .equipped
        .as_deref()
        .unwrap()
        .vibe;
    assert_eq!(so::rep(&cx, station_id), Ok(station_vibe));
    assert_eq!(
        so::solar_rep(&mut cx, station_vibe as u32),
        Ok(station_vibe)
    );
    assert_eq!(so::solar_rep(&mut cx, unknown), Err(PubError::BAD_ID));
    assert_eq!(so::rep(&cx, 0), Err(PubError::BAD_ARGUMENT));
    assert_eq!(
        so::solar_archetype_id(&cx, station_id),
        Ok(create_id(b"space_factory01"))
    );
    assert_eq!(
        so::solar_archetype_id(&cx, npc_id),
        Ok(create_id(b"he_fighter"))
    );
    let factory = create_id(b"space_factory01");
    let name = IdString(factory).to_hex_string().into_bytes();
    let len = name.len() as i32;
    assert_eq!(
        so::solar_archetype_nickname(&cx, len + 1, factory),
        Ok(name.clone())
    );
    assert_eq!(
        so::solar_archetype_nickname(&cx, len, factory),
        Err(PubError::FAILED)
    );
    assert_eq!(
        so::solar_archetype_nickname(&cx, 0, factory),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::solar_archetype_nickname(&cx, 64, unknown),
        Err(PubError::BAD_ID)
    );
    let station_voice = cx
        .world
        .get(station)
        .unwrap()
        .equipped
        .as_deref()
        .unwrap()
        .voice;
    assert_ne!(station_voice, 0);
    assert_eq!(so::voice_id(&cx, station_id), Ok(station_voice));
    assert_eq!(so::voice_id(&cx, 0), Err(PubError::BAD_ARGUMENT));
    assert_eq!(so::voice_id(&cx, unknown), Err(PubError::BAD_ID));

    // Hardpoints, radius, mass, centre of mass.
    assert_eq!(
        so::hardpoint(&cx, station_id, b"HpDockMountA"),
        Ok(cx.world.hardpoint_world(station, b"HpDockMountA").unwrap())
    );
    assert_eq!(
        so::hardpoint(&cx, station_id, b"HpNoSuchPoint"),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(
        so::radius(&cx, station_id),
        Ok(cx.world.physical_radius_r(station))
    );
    assert_eq!(so::mass(&cx, npc_id), Ok(cx.world.mass(npc)));
    assert_eq!(
        so::center_of_mass(&cx, npc_id),
        Ok(cx.world.center_of_mass(npc))
    );
    assert_eq!(so::mass(&cx, unknown), Err(PubError::BAD_ID));

    // Health: the hull; a ring's relative health is its shields' state.
    let o = cx.world.get(station).unwrap();
    assert_eq!(
        so::health(&cx, station_id),
        Ok((o.simple.hit_pts, o.simple.max_hit_pts))
    );
    assert_eq!(so::relative_health(&cx, station_id), Ok(1.0));
    let ring_shields = items(&cx, ring, types::SHIELD);
    let expected_ring =
        if ring_shields.is_empty() || ring_shields.iter().any(|s| !s.is_functioning(true)) {
            0.0
        } else {
            1.0
        };
    assert_eq!(so::relative_health(&cx, ring_id), Ok(expected_ring));
    assert_eq!(so::relative_health(&cx, 0), Err(PubError::BAD_ARGUMENT));
    assert_eq!(so::relative_health(&cx, unknown), Err(PubError::BAD_ID));
    let shields = items(&cx, npc, types::SHIELD);
    assert_eq!(shields.len(), 1);
    let weapons_offline = cx
        .world
        .get(npc)
        .unwrap()
        .equipped
        .as_deref()
        .unwrap()
        .weapons_offline;
    let EquipKind::Shield(shield) = &shields[0].kind else {
        panic!("a shield");
    };
    assert!(shield.max_hit_pts > 0.0);
    assert_eq!(
        so::shield_health(&cx, npc_id),
        Ok(Some(so::ShieldHealth {
            current: shield.hit_pts,
            max: shield.max_hit_pts,
            active: shields[0].is_functioning(weapons_offline),
        }))
    );

    assert_eq!(so::exists_and_alive(&cx, station_id), Ok(()));
    assert_eq!(so::exists_and_alive(&cx, 0), Err(PubError::BAD_ID));
    assert_eq!(so::exists_and_alive(&cx, unknown), Err(PubError::BAD_ID));

    // Motion: the body's velocity and angular velocity.
    let inspect = cx.world.inspect(npc).unwrap();
    assert_eq!(
        so::motion(&cx, npc_id),
        Ok((inspect.velocity, inspect.angular_velocity))
    );
    assert_eq!(so::motion(&cx, unknown), Err(PubError::BAD_ID));

    // The target, set through the ship's command.
    assert_eq!(so::target(&cx, npc_id), Ok(0));
    cx.world
        .command(npc, Command::SetTarget(Some(station), SUBOBJ_ID_NONE));
    assert_eq!(so::target(&cx, npc_id), Ok(station_id));
    assert_eq!(so::target(&cx, station_id), Err(PubError::FAILED));

    // The scanner: its ranges less the interference, truncated.
    let scanner = items(&cx, npc, types::SCANNER).remove(0);
    let EquipKind::Scanner(s) = &scanner.kind else {
        panic!("a scanner");
    };
    let clear = 1.0 - f64::from(cx.world.get(npc).unwrap().simple.scanner_interference);
    assert_eq!(
        so::scanner_range(&cx, npc_id),
        Ok((
            trunc_to_i32(f64::from((clear * f64::from(s.range)) as f32)),
            trunc_to_i32(f64::from((clear * f64::from(s.cargo_scan_range)) as f32))
        ))
    );
    assert_eq!(so::scanner_range(&cx, station_id), Err(PubError::FAILED));

    // The hold and the cargo: the loadout's ammunition, then gold.
    let gold = create_id(b"commodity_gold");
    let gold_good = cx.world.data().goods.arch_to_good(gold);
    assert_eq!(so::archetype_id(&cx, gold_good), Ok(gold));
    assert_eq!(so::archetype_id(&cx, 0), Err(PubError::BAD_ID));
    let occupied = |cx: &Cx<'_>| {
        trunc_to_i32_via_i64(
            f64::from(cx.world.cargo_hold_size(npc).unwrap())
                - cx.world.cargo_hold_remaining(npc).unwrap(),
        ) as u32
    };
    assert_eq!(so::cargo_space_occupied(&cx, npc_id), Ok(occupied(&cx)));
    let before = so::enumerate_cargo(&mut cx, npc_id).unwrap();
    game_logic::cargo::add_cargo(&mut cx, npc, gold, 5, 1.0, false);
    let after = so::enumerate_cargo(&mut cx, npc_id).unwrap();
    assert_eq!(after.len(), before.len() + 1);
    let line: CargoLine = *after.last().unwrap();
    assert_eq!((line.good, line.count), (gold_good, 5));
    assert_eq!(so::cargo_space_occupied(&cx, npc_id), Ok(occupied(&cx)));
    assert_eq!(so::cargo_space_occupied(&cx, station_id), Ok(0));
    assert_eq!(so::enumerate_cargo(&mut cx, planet_id), Ok(Vec::new()));
    assert_eq!(so::enumerate_cargo(&mut cx, unknown), Err(PubError::BAD_ID));

    // Invincibility, as the commands add up.
    assert_eq!(
        so::invincible(&mut cx, npc_id),
        Ok(InvincibleState {
            from_players: false,
            protect: false,
            max_hp_loss: 1.0
        })
    );
    so::set_invincible(&mut cx, npc_id, true, false, 0.5).unwrap();
    assert_eq!(
        so::invincible(&mut cx, npc_id),
        Ok(InvincibleState {
            from_players: true,
            protect: true,
            max_hp_loss: 0.5
        })
    );
    so::set_invincible2(&mut cx, npc_id, true, false, 0.25).unwrap();
    assert_eq!(
        so::invincible(&mut cx, npc_id),
        Ok(InvincibleState {
            from_players: false,
            protect: true,
            max_hp_loss: 0.25
        })
    );
    so::set_invincible(&mut cx, npc_id, false, true, 0.5).unwrap();
    assert_eq!(
        so::invincible(&mut cx, npc_id),
        Ok(InvincibleState {
            from_players: false,
            protect: false,
            max_hp_loss: 1.0
        })
    );
    assert_eq!(
        so::set_invincible(&mut cx, 0, true, true, 1.0),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(so::invincible(&mut cx, unknown), Err(PubError::BAD_ID));
}

#[test]
fn spaceobj_actions_over_he01() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("pub::SpaceObj actions", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    let mine = player(&mut cx, Vector::new(20000.0, 0.0, 20000.0));
    let mine_id = id(&cx, mine);
    let npc = fighter(
        &mut cx,
        controller,
        Vector::new(20500.0, 0.0, 20000.0),
        Vector::ZERO,
    );
    let npc_id = id(&cx, npc);
    let station = solar(&cx, b"he01_14");
    let station_id = id(&cx, station);
    let unknown = create_id(b"no_such_object");
    // Player 1's client gets the NPC.
    tick(&mut cx, 0.01);
    let _ = (sink.take(), heard());

    // Comm: the NPC speaks to the player with its own voice and costume;
    // its comm type becomes 0.
    let npc_eq = cx
        .world
        .get(npc)
        .unwrap()
        .equipped
        .as_deref()
        .unwrap()
        .clone();
    let request = CommRequest {
        sender: npc_id,
        target: mine_id,
        comm_type: 7,
        text_ids: vec![11, 12],
        mood: 0x1_0002,
        duration: 1.0,
        ..CommRequest::default()
    };
    so::send_comm(&mut cx, &request).unwrap();
    let comms: Vec<_> = server_messages(&sink)
        .into_iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::SendComm(s) => Some((c, s)),
            _ => None,
        })
        .collect();
    assert_eq!(comms.len(), 1);
    let (client, comm) = &comms[0];
    assert_eq!(*client, 1);
    assert_eq!(
        (comm.field_20, comm.field_24, comm.field_28, comm.field_60),
        (npc_id, mine_id, npc_eq.voice, 0)
    );
    assert_eq!(comm.costume_flag, 1);
    assert_eq!(comm.costume.parts, npc_eq.space_costume.part_ids);
    assert_eq!((comm.text_ids.clone(), comm.mood), (vec![11, 12], 2));
    assert_eq!(
        so::send_comm(
            &mut cx,
            &CommRequest {
                text_ids: Vec::new(),
                ..request.clone()
            }
        ),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::send_comm(
            &mut cx,
            &CommRequest {
                sender: 0,
                target: unknown,
                ..request.clone()
            }
        ),
        Err(PubError::BAD_ARGUMENT)
    );
    // No speaker and no costume.
    assert_eq!(
        so::send_comm(
            &mut cx,
            &CommRequest {
                sender: 0,
                ..request.clone()
            }
        ),
        Err(PubError::BAD_ID)
    );

    // Health: half the hull.
    let max = cx.world.get(npc).unwrap().simple.max_hit_pts;
    so::set_relative_health(&mut cx, npc_id, 0.5).unwrap();
    assert_eq!(
        cx.world.get(npc).unwrap().simple.hit_pts,
        (f64::from(max) * 0.5) as f32
    );
    assert_eq!(so::relative_health(&cx, npc_id), Ok(0.5));
    assert_eq!(
        so::set_relative_health(&mut cx, npc_id, 1.5),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::set_relative_health(&mut cx, npc_id, f32::NAN),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::set_relative_health(&mut cx, 0, 0.5),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::set_relative_health(&mut cx, unknown, 0.5),
        Err(PubError::BAD_ID)
    );

    // Shields drained.
    so::drain_shields(&mut cx, npc_id).unwrap();
    let drained = so::shield_health(&cx, npc_id).unwrap().unwrap();
    assert_eq!(drained.current, 0.0);
    assert!(drained.max > 0.0);
    assert_eq!(so::drain_shields(&mut cx, 0), Err(PubError::BAD_ARGUMENT));
    assert_eq!(so::drain_shields(&mut cx, unknown), Err(PubError::BAD_ID));

    // Fuses.
    so::light_fuse(&mut cx, npc_id, b"intermed_damage_smallship01", 0.0).unwrap();
    let fuse = create_id(b"intermed_damage_smallship01");
    assert!(
        cx.logic
            .objects
            .get(npc)
            .unwrap()
            .fuses
            .iter()
            .any(|f| f.fuse.nickname == fuse)
    );
    assert_eq!(
        so::light_fuse(&mut cx, npc_id, b"no_such_fuse", 0.0),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::light_fuse(&mut cx, 0, b"intermed_damage_smallship01", 0.0),
        Err(PubError::BAD_ARGUMENT)
    );
    let _ = (sink.take(), heard());

    // An impulse pushes the body up.
    let (before, _) = so::motion(&cx, npc_id).unwrap();
    let at = cx.world.get(npc).unwrap().engine.position;
    so::add_impulse(&mut cx, npc_id, Vector::new(0.0, 100_000.0, 0.0), at).unwrap();
    // The physics applies it at its next step.
    tick(&mut cx, 0.01);
    let (after, _) = so::motion(&cx, npc_id).unwrap();
    assert!(after.y > before.y, "{before:?} {after:?}");
    assert_eq!(
        so::add_impulse(&mut cx, unknown, Vector::ZERO, Vector::ZERO),
        Err(PubError::BAD_ID)
    );

    // A docking point of the station: every player hears a change.
    so::activate(&mut cx, station_id, true, 0).unwrap();
    let activated: Vec<_> = server_messages(&sink)
        .into_iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::ActivateObject(a) => Some((c, a.object_id, a.activate, a.field_08)),
            _ => None,
        })
        .collect();
    assert_eq!(activated, [(1, station_id, 1, 0)]);
    so::activate(&mut cx, station_id, true, 0).unwrap();
    assert!(server_messages(&sink).is_empty());
    assert_eq!(
        so::activate(&mut cx, station_id, true, 6),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(
        so::activate(&mut cx, npc_id, true, 0),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(
        so::activate(&mut cx, 0, true, 0),
        Err(PubError::BAD_ARGUMENT)
    );

    // Equipment: cruise; no cloak; the docking lights.
    assert!(!game_logic::replication::cruise_active(cx.world, npc));
    assert_eq!(
        so::activate_equipment(
            &mut cx,
            npc_id,
            &EqInfo {
                kind: 0,
                count: 1,
                ..EqInfo::default()
            }
        ),
        Ok(Vec::new())
    );
    assert!(game_logic::replication::cruise_active(cx.world, npc));
    let cruise: Vec<_> = sink
        .take()
        .into_iter()
        .filter_map(|(c, m)| match m {
            ToClient::Common(CommonMessage::ActivateCruise(a)) => {
                Some((c, a.object_id, a.active, a.field_05))
            }
            _ => None,
        })
        .collect();
    assert_eq!(cruise, [(1, npc_id, 1, 1)]);
    let info = |kind, count| EqInfo {
        kind,
        count,
        ..EqInfo::default()
    };
    assert_eq!(
        so::activate_equipment(&mut cx, npc_id, &info(0, 2)),
        Err(PubError::FAILED)
    );
    assert_eq!(
        so::activate_equipment(&mut cx, npc_id, &info(2, 0)),
        Err(PubError::BAD_ID)
    );
    assert_eq!(
        so::activate_equipment(&mut cx, npc_id, &info(3, 0)),
        Ok(Vec::new())
    );
    assert_eq!(
        so::activate_equipment(&mut cx, npc_id, &info(4, 0)),
        Err(PubError::FAILED)
    );
    assert_eq!(
        so::activate_equipment(&mut cx, npc_id, &info(1, 0)),
        Err(PubError::FAILED)
    );
    // The station is far beyond the cargo scan range.
    assert_eq!(
        so::activate_equipment(
            &mut cx,
            npc_id,
            &EqInfo {
                kind: 1,
                target: station_id,
                ..EqInfo::default()
            }
        ),
        Err(PubError(-5))
    );
    // The player's ship is close: its cargo is the answer.
    let scanned = so::activate_equipment(
        &mut cx,
        npc_id,
        &EqInfo {
            kind: 1,
            target: mine_id,
            ..EqInfo::default()
        },
    );
    let list = scanned.expect("the scan completes");
    let ids: Vec<u16> = items(&cx, mine, u32::MAX).iter().map(|e| e.id).collect();
    assert!(!list.is_empty());
    assert!(list.iter().all(|item| ids.contains(&item.id)));
    assert_eq!(
        so::activate_equipment(&mut cx, 0, &info(0, 1)),
        Err(PubError(-5))
    );
    assert_eq!(
        so::activate_equipment(&mut cx, unknown, &info(0, 1)),
        Err(PubError::FAILED)
    );

    // Formation and lane answers.
    assert_eq!(so::formation_response(&mut cx, npc_id, 1), Ok(()));
    assert_eq!(so::lane_response(&mut cx, station_id, 1), Ok(()));
    assert_eq!(
        so::formation_response(&mut cx, 0, 1),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::lane_response(&mut cx, unknown, 1),
        Err(PubError::BAD_ID)
    );

    // Jettison: units leave a stack; an item that is no stack is -3.
    let gold = create_id(b"commodity_gold");
    game_logic::cargo::add_cargo(&mut cx, npc, gold, 5, 1.0, false);
    let gold_good = cx.world.data().goods.arch_to_good(gold);
    let line = *so::enumerate_cargo(&mut cx, npc_id)
        .unwrap()
        .iter()
        .find(|l| l.good == gold_good)
        .unwrap();
    so::jettison_equipment(&mut cx, npc_id, line.id, 2).unwrap();
    let left = so::enumerate_cargo(&mut cx, npc_id)
        .unwrap()
        .into_iter()
        .find(|l| l.id == line.id)
        .unwrap();
    assert_eq!(left.count, 3);
    assert_eq!(
        so::jettison_equipment(&mut cx, npc_id, 0x7fff, 1),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(so::jettison_equipment(&mut cx, station_id, 1, 1), Ok(()));
    assert_eq!(
        so::jettison_equipment(&mut cx, 0, 1, 1),
        Err(PubError::BAD_ID)
    );

    // A space script for the player's client.
    let _ = sink.take();
    so::request_space_script(&mut cx, station_id, Vector::ZERO, 1, 1, b"scene_station\0").unwrap();
    let scripts: Vec<_> = server_messages(&sink)
        .into_iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::RequestSpaceScript(s) => Some((c, s.field_00, s.name)),
            _ => None,
        })
        .collect();
    assert_eq!(scripts, [(1, station_id, b"scene_station\0".to_vec())]);
    // The position and the priority travel as rpclocal's payload carries
    // them: priority 2 is mode byte 0, any other priority 1.
    let at = Vector::new(1.5, -2.0, 3.0);
    for (priority, mode) in [(2, 0), (1, 1), (7, 1)] {
        so::request_space_script(&mut cx, station_id, at, priority, 1, b"s\0").unwrap();
        let sent: Vec<_> = server_messages(&sink)
            .into_iter()
            .filter_map(|(_, m)| match m {
                ServerMessage::RequestSpaceScript(s) => Some((s.position, s.mode, s.priority())),
                _ => None,
            })
            .collect();
        let bits = [1.5f32.to_bits(), (-2.0f32).to_bits(), 3.0f32.to_bits()];
        let priority = if priority == 2 { 2 } else { 1 };
        assert_eq!(sent, [(bits, mode, priority)]);
    }
    assert_eq!(
        so::request_space_script(&mut cx, 0, Vector::ZERO, 1, 1, b"x"),
        Err(PubError::BAD_ARGUMENT)
    );

    // Launch: the NPC gets its launch invincibility; a player's ship is
    // reported.
    so::launch(&mut cx, npc_id, station_id, 0).unwrap();
    assert_eq!(
        so::invincible(&mut cx, npc_id),
        Ok(InvincibleState {
            from_players: true,
            protect: true,
            max_hp_loss: 0.0
        })
    );
    cx.logic.reports.0.clear();
    so::launch(&mut cx, mine_id, station_id, 0).unwrap();
    assert!(
        cx.logic
            .reports
            .0
            .iter()
            .any(|r| r.text.contains("StarSystem::launch() called on a player!"))
    );
    assert_eq!(
        so::launch(&mut cx, 0, station_id, 0),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::launch(&mut cx, unknown, station_id, 0),
        Err(PubError::BAD_ID)
    );
    assert_eq!(
        so::launch(&mut cx, npc_id, unknown, 0),
        Err(PubError::NOT_FOUND)
    );

    // Destroy: Create's records go.
    assert!(cx.logic.space_obj.created_ships.contains(&npc_id));
    so::destroy(&mut cx, npc_id, 0).unwrap();
    assert!(!cx.logic.space_obj.created_ships.contains(&npc_id));
    assert!(!cx.logic.space_obj.ship_controllers.contains_key(&npc_id));
    assert_eq!(so::exists_and_alive(&cx, npc_id), Err(PubError::BAD_ID));
    assert_eq!(so::destroy(&mut cx, npc_id, 0), Err(PubError::BAD_ID));
    assert_eq!(so::destroy(&mut cx, 0, 0), Err(PubError::BAD_ARGUMENT));
}

#[test]
fn spaceobj_creation_and_positions_over_he01() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("pub::SpaceObj creation", &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    player(&mut cx, Vector::new(20000.0, 0.0, 20000.0));
    let _ = (sink.take(), heard());
    let gold = create_id(b"commodity_gold");
    let gold_good = cx.world.data().goods.arch_to_good(gold);

    // Loot: a container of gold, marked for player 1.
    let mut loot_info = LootInfo::new(he01(), gold, 7, Vector::new(20100.0, 0.0, 20000.0));
    loot_info.mark_player = 1;
    loot_info.owner = 0;
    let loot_id = so::create_loot(&mut cx, &loot_info).unwrap();
    let loot = cx.world.find(loot_id).expect("the container");
    assert_eq!(heard(), [format!("LootSpawned {loot_id} 0 {gold_good} 7")]);
    assert!(cx.logic.objects.get(loot).unwrap().is_marked_by(1));
    match &cx.world.get(loot).unwrap().data {
        KindData::Loot(l) => assert_eq!((l.contents_arch, l.units), (gold, 7)),
        _ => panic!("loot"),
    }
    // On a multiplayer server player 1's observer tracks it.
    assert!(
        cx.logic
            .server_world
            .system(he01())
            .unwrap()
            .observers
            .iter()
            .any(|o| o.player == 1 && o.find(loot).is_some())
    );
    assert_eq!(so::good_id(&cx, loot_id), Ok(gold_good));
    assert_eq!(so::rep(&cx, loot_id), Err(PubError::FAILED));
    assert_eq!(so::voice_id(&cx, loot_id), Err(PubError::NOT_FOUND));
    assert_eq!(so::shield_health(&cx, loot_id), Ok(None));
    assert_eq!(so::drain_shields(&mut cx, loot_id), Err(PubError::FAILED));
    assert_eq!(
        so::light_fuse(&mut cx, loot_id, b"intermed_damage_smallship01", 0.0),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::good_id(&cx, id(&cx, solar(&cx, b"he01_14"))),
        Err(PubError::FAILED)
    );
    assert_eq!(so::good_id(&cx, 0), Err(PubError::BAD_ID));
    assert_eq!(
        so::create_loot(&mut cx, &LootInfo::new(0, gold, 1, Vector::ZERO)),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::create_loot(
            &mut cx,
            &LootInfo::new(he01(), create_id(b"no_such_good"), 1, Vector::ZERO)
        ),
        Err(PubError::BAD_ARGUMENT)
    );

    // A solar made at run time: dynamic, every player gets it.
    let at = Vector::new(30000.0, 0.0, 30000.0);
    let mut solar_info =
        SolarInfo::new(he01(), create_id(b"space_factory01"), b"test_solar_01", at);
    solar_info.docking_base = create_id(b"he01_14_base");
    solar_info.mark_player = 1;
    let solar_id = so::create_solar(&mut cx, &solar_info).unwrap();
    assert_eq!(solar_id, create_id(b"test_solar_01"));
    let made = cx.world.find(solar_id).expect("the solar");
    assert_eq!(so::location(&cx, solar_id).unwrap().0, at);
    assert_eq!(
        so::solar_archetype_id(&cx, solar_id),
        Ok(create_id(b"space_factory01"))
    );
    assert_eq!(so::object_type(&cx, solar_id), Ok(0x100));
    assert_eq!(so::relative_health(&cx, solar_id), Ok(1.0));
    assert_eq!(
        so::docking_target(&cx, solar_id),
        Ok(create_id(b"he01_14_base"))
    );
    match &cx.world.get(made).unwrap().data {
        KindData::Solar(s) => assert!(s.dynamic && s.destructible),
        _ => panic!("a solar"),
    }
    assert!(cx.logic.objects.get(made).unwrap().is_marked_by(1));
    let created: Vec<_> = server_messages(&sink)
        .into_iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::CreateSolar(s) => Some((c, s.object.obj_id, s.marked_by)),
            _ => None,
        })
        .collect();
    assert_eq!(created, [(1, solar_id, 1)]);
    // The solar BSP has it: a scan around it finds it.
    let found =
        game_logic::api::system::scan_objects(&mut cx, he01(), 16, at, 10.0, 0x100).unwrap();
    assert!(found.contains(&solar_id));
    assert_eq!(
        so::create_solar(
            &mut cx,
            &SolarInfo::new(he01(), create_id(b"space_factory01"), b"", at)
        ),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::create_solar(
            &mut cx,
            &SolarInfo::new(0, create_id(b"space_factory01"), b"x", at)
        ),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::create_solar(
            &mut cx,
            &SolarInfo::new(he01(), create_id(b"no_such_solar"), b"test_solar_02", at)
        ),
        Err(PubError::FAILED)
    );
    so::destroy(&mut cx, solar_id, 0).unwrap();
    assert_eq!(so::exists_and_alive(&cx, solar_id), Err(PubError::BAD_ID));
    let _ = heard();

    // An NPC sent into transit and destroyed there: deleted at once with
    // the bare `ObjectDestroyed`; the C++ answers -2.
    let npc = fighter(
        &mut cx,
        controller,
        Vector::new(21000.0, 0.0, 20000.0),
        Vector::ZERO,
    );
    let npc_id = id(&cx, npc);
    so::relocate(&mut cx, npc_id, 0, Vector::ZERO, Matrix::IDENTITY).unwrap();
    assert_eq!(so::system(&cx, npc_id), Ok(0));
    assert_eq!(so::destroy(&mut cx, npc_id, 1), Err(PubError::BAD_ID));
    assert_eq!(heard(), [format!("ObjectDestroyed {npc_id} 0 0")]);
    assert!(cx.world.find(npc_id).is_none());
    assert!(!cx.logic.space_obj.created_ships.contains(&npc_id));

    // Empty positions. Far from everything the first point on the circle
    // is free: 100 m from the base in its local XY plane.
    let base = Transform::new(Matrix::IDENTITY, Vector::new(0.0, 60000.0, 0.0));
    let free = so::empty_pos(&mut cx, he01(), &base, 100.0, 10.0, 0).unwrap();
    let d = |a: Vector, b: Vector| {
        let (x, y, z) = (
            f64::from(a.x) - f64::from(b.x),
            f64::from(a.y) - f64::from(b.y),
            f64::from(a.z) - f64::from(b.z),
        );
        (x * x + y * y + z * z).sqrt()
    };
    assert!((d(free, base.translation) - 100.0).abs() < 0.01, "{free:?}");
    assert_eq!(free.z, 0.0);
    // Selection 1 is the local YZ plane.
    let yz = so::empty_pos(&mut cx, he01(), &base, 100.0, 10.0, 1).unwrap();
    assert_eq!(yz.x, 0.0);
    // At the planet's centre the circle grows past the atmosphere.
    let planet_at = Vector::new(-33270.0, 0.0, -33039.0);
    let near_planet = Transform::new(Matrix::IDENTITY, planet_at);
    let out = so::empty_pos(&mut cx, he01(), &near_planet, 10.0, 0.0, 0).unwrap();
    assert!(d(out, planet_at) > 4200.0, "{out:?}");
    assert_eq!(
        so::empty_pos(&mut cx, he01(), &base, 100.0, 10.0, 4),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::empty_pos(&mut cx, he01(), &base, -1.0, 10.0, 0),
        Err(PubError::BAD_ARGUMENT)
    );
    assert_eq!(
        so::empty_pos(&mut cx, he01(), &base, f32::NAN, 10.0, 0),
        Err(PubError::BAD_ARGUMENT)
    );

    // Is a point empty: nothing near and no planet's atmosphere; the sun
    // (type 4) is not a planet.
    assert_eq!(
        so::is_pos_empty(&mut cx, he01(), Vector::new(0.0, 60000.0, 0.0), 10.0),
        Ok(true)
    );
    assert_eq!(
        so::is_pos_empty(&mut cx, he01(), Vector::new(0.0, 5000.0, 0.0), 10.0),
        Ok(true)
    );
    assert_eq!(
        so::is_pos_empty(
            &mut cx,
            he01(),
            Vector::new(planet_at.x, 3000.0, planet_at.z),
            10.0
        ),
        Ok(false)
    );
    assert_eq!(
        so::is_pos_empty(&mut cx, he01(), planet_at, 10.0),
        Ok(false)
    );
    assert_eq!(
        so::is_pos_empty(&mut cx, 0, planet_at, 10.0),
        Err(PubError::FAILED)
    );
}
