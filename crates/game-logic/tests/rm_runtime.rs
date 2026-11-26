//! The random missions' ship-group spawns and comm sequences on the
//! original data (`outdir/`; the P3.7b follow-up, record 0008 point 10).
//! Expected values follow the C++ (`Content/RandomMissionMachine.cpp`,
//! `RandomMissionVignette.cpp`, `Gcs.cpp`).
//!
//! The fixture accepts the first vendor mission of `he01_01_Base` as
//! `rm_session.rs` does (player 7, no ship, joined to the mission), runs
//! three ticks (the runtime machine reaches its pre-battle state 8: the
//! shipped data's first offer there is an assassination), loads the
//! mission's system, gives player 7 a ship at the target ships' position
//! (`SpaceEnter` to the mission) and runs one more tick: the proximity
//! triggers fire and state 9 spawns the first target group.
//!
//! - A ship group: every ship made through the mission ships' spawn path,
//!   with its reputation (a target ship would carry the offer's target
//!   name; this offer, machine type 3, has none), its record, the
//!   formation and the attitude towards the player.
//! - A comm sequence: prepared on the offer's line, admitted by the comm
//!   system's manager (`FGcsConversationManager::Add`), its line spoken to
//!   player 7's ship and ended by the line's `CommComplete`.
//! - A player-list line (`FGcsPlayerListLine`) on its own: who decides
//!   `CanPlay`, who hears it, which player's ship ends it.
//!
//! Each test returns early without `outdir/`. Nothing these tests write is
//! committed.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::api;
use game_logic::api::space_obj::ShipInfo;
use game_logic::bases as base_db;
use game_logic::content::gcs::conversations::Kind;
use game_logic::content::gcs::{self, LineDone, lines::Line};
use game_logic::content::players::{PlayerShip, PlayerState};
use game_logic::content::random_mission::offer::Offer;
use game_logic::content::random_mission::runtime::{MachineKind, RandomMission, SpawnRecord};
use game_logic::content::ships::{self, ShipKey};
use game_logic::content::{Instance, base_support, game_support};
use game_logic::events::{CommResult, ControllerMessage};
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::{selection, world};
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;
use neuron_server::controller::{self, ControllerId, Priority};

/// The oracle session's player.
const PLAYER: u32 = 7;
/// The base whose first vendor mission is accepted.
const BASE: &[u8] = b"he01_01_Base";

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// A multiplayer server on `outdir/` started as the oracle starts it
/// (`rm_session.rs`); `None` without the data.
fn started() -> Option<(World, Logic)> {
    let root = outdir();
    let Some(config) = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini") else {
        println!("notice: outdir/OutpostFrontier.ini is missing; nothing was run");
        return None;
    };
    let data_dir = root.join("data");
    let mut data = GameData::new();
    data.load(&Host::new(&data_dir), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(data_dir.clone()));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for (name, priority) in [
        ("GameSupport", Priority::Highest),
        ("SpaceSupport", Priority::Normal),
        ("BaseSupport", Priority::Normal),
    ] {
        let id = controller::create(
            &mut cx,
            b"Content.dll",
            name.as_bytes(),
            &CreateParams::None,
            priority,
        );
        assert_ne!(id.0, 0, "{name}");
    }
    let text = Host::new(&data_dir)
        .read(b"initialworld.ini")
        .unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    selection::load_world_state(&mut cx, &mut reader);
    Some((world, logic))
}

fn instance<'c>(cx: &'c Cx<'_>, id: ControllerId) -> &'c RandomMission {
    match cx.logic.content.instances.get(&id) {
        Some(Instance::RandomMission(r)) => r,
        _ => panic!("the mission is gone"),
    }
}

/// The first vendor mission of the base accepted by player 7 (no ship) and
/// joined; its controller.
fn accept(cx: &mut Cx<'_>) -> ControllerId {
    let find = |cx: &Cx<'_>, want: fn(&Instance) -> bool| {
        cx.logic
            .content
            .instances
            .iter()
            .find(|(_, i)| want(i))
            .map_or(ControllerId(0), |(&id, _)| id)
    };
    let game = find(cx, |i| matches!(i, Instance::GameSupport));
    let support = find(cx, |i| matches!(i, Instance::BaseSupport));
    let base = api::misc::base_id(cx, BASE).unwrap_or(0);
    let system = api::misc::base_system(cx, base).ok();
    let mut state = PlayerState::new(cx, PLAYER);
    state.system = system.filter(|s| cx.logic.content.state.systems.contains_key(s));
    cx.logic.content.state.players.insert(PLAYER, state);
    if let Some(host) = cx.world.data_host() {
        base_db::load_base(cx, &host, base);
    }
    let mut enter = ControllerMessage::BaseEnter {
        base,
        player: PLAYER,
    };
    game_support::handle_message(cx, game, &mut enter);
    base_support::handle_message(cx, support, &mut enter);
    base_support::refresh_mission_vendor_offers(cx, PLAYER);
    let offer = cx
        .logic
        .content
        .state
        .base_visits
        .get(&PLAYER)
        .and_then(|v| v.vendor_offers.first())
        .map_or(0, |o| o.offer_id);
    let mut response = ControllerMessage::MissionResponse {
        offer,
        kind: 2,
        accepted: true,
        player: PLAYER,
    };
    game_support::handle_message(cx, game, &mut response);
    base_support::handle_message(cx, support, &mut response);
    let mission = find(cx, |i| matches!(i, Instance::RandomMission(_)));
    assert_ne!(mission.0, 0, "a vendor mission was accepted");
    game_logic::content::random_mission::runtime::join_player(cx, mission, PLAYER);
    mission
}

/// One second: the game time, the mission's heartbeat, the bus's timers.
fn tick(cx: &mut Cx<'_>, mission: ControllerId) {
    cx.logic.content.state.game_time += 1.0;
    let mut heartbeat = ControllerMessage::Heartbeat { elapsed: 1.0 };
    let _ = api::controller::send_message(cx, mission, &mut heartbeat);
    controller::fire_timers(cx, 1.0);
}

/// The vignette machine of the mission: its key, state and offer index.
fn vignette(cx: &Cx<'_>, mission: ControllerId) -> (u32, i32, usize) {
    instance(cx, mission)
        .machines
        .iter()
        .find_map(|(&k, m)| match &m.kind {
            MachineKind::Vignette(v) => Some((k, m.sm.state(), v.offer)),
            _ => None,
        })
        .expect("a vignette machine")
}

/// The mission's offer (a copy).
fn offer(cx: &Cx<'_>, mission: ControllerId) -> Offer {
    let (_, _, index) = vignette(cx, mission);
    let data = instance(cx, mission).data.expect("the mission data");
    cx.logic
        .content
        .state
        .rm
        .missions
        .get(data)
        .expect("data")
        .offers[index]
        .clone()
}

/// The ship machine's records (copies).
fn ship_records(cx: &Cx<'_>, mission: ControllerId) -> Vec<SpawnRecord> {
    instance(cx, mission)
        .machines
        .values()
        .find_map(|m| match &m.kind {
            MachineKind::Ships(s) => Some(s.records.clone()),
            _ => None,
        })
        .unwrap_or_default()
}

/// Player 7's ship of `he_fighter` at `at` in `system`, as GameSupport's
/// `OnPlayerLaunch` makes the player's ship object, and a vibe of its own
/// (the synthetic player has none); the ship's id.
fn give_ship(cx: &mut Cx<'_>, system: u32, at: Vector) -> u32 {
    let owner = cx.logic.content.state.space_support.0;
    let arch = create_id(b"he_fighter");
    let info = ShipInfo::new(
        owner,
        system,
        arch,
        create_id(b"li_p_he_fighter_loadout01"),
        at,
        Matrix::IDENTITY,
    );
    let ship = api::space_obj::create(cx, &info).expect("the player's ship");
    let vibe = api::reputation::alloc(cx, &FmtStr::new(0), &FmtStr::new(0)).expect("a vibe");
    let state = cx
        .logic
        .content
        .state
        .players
        .get_mut(&PLAYER)
        .expect("player 7's state");
    state.system = Some(system);
    state.reputation = vibe;
    state.ship = Some(PlayerShip::new(ship, arch, Vec::new(), Some(system)));
    if let Some(r) = cx.logic.content.state.systems.get_mut(&system) {
        r.add_player(PLAYER);
    }
    ship
}

/// The fixture: the mission accepted and in its pre-battle state, its
/// system loaded, player 7's ship at the target position, one tick run.
/// `run` gets the context, the mission and the ship.
fn with_battle(run: impl FnOnce(&mut Cx<'_>, ControllerId, u32)) {
    let Some((mut world, mut logic)) = started() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let mission = accept(&mut cx);
    for _ in 0..3 {
        tick(&mut cx, mission);
    }
    let (_, state, _) = vignette(&cx, mission);
    assert_eq!(
        state, 8,
        "the first vendor offer waits for its target ships"
    );
    let o = offer(&cx, mission);
    assert!(o.target_ships.component.active && !o.target_ships.groups.is_empty());
    world::load(&mut cx, o.system);
    let ship = give_ship(&mut cx, o.system, o.target_ships.position);
    let mut enter = ControllerMessage::SpaceEnter {
        system: o.system,
        player: PLAYER,
        launch_object: 0,
        launch_kind: -1,
    };
    let _ = api::controller::send_message(&mut cx, mission, &mut enter);
    tick(&mut cx, mission);
    run(&mut cx, mission, ship);
}

#[test]
fn a_target_group_spawns_with_its_reputations_and_formation() {
    with_battle(|cx, mission, player_ship| {
        let (_, state, _) = vignette(cx, mission);
        assert_eq!(state, 9, "the player reached the target ships");
        let o = offer(cx, mission);
        let group = &o.target_ships.groups[0];
        assert_eq!(group.field_0x01, 1, "the first target group is spawned");
        let records = ship_records(cx, mission);
        assert_eq!(records.len(), group.ships.len(), "{records:#?}");
        let player_vibe = cx.logic.content.state.players[&PLAYER].reputation;
        let mut formation_ids = Vec::new();
        for (slot, (record, spawn)) in records.iter().zip(&group.ships).enumerate() {
            // The record (`:1813-1821`).
            assert_eq!(record.name, spawn.nickname);
            assert_eq!(record.state, 2);
            assert_eq!(record.group, group.serial);
            assert_eq!(record.flags, spawn.field_0xd8 as u32);
            assert_eq!(record.ship_kind, spawn.field_0xdc);
            assert_eq!(record.attitude, group.field_0x40);
            assert_eq!(record.counted, spawn.affiliation == o.hostile_group);
            assert_eq!(record.loot, spawn.loot);
            // The ship of the block.
            let key = record.npc_ship.expect("the record's ship");
            let npc = cx.logic.content.state.npc_ships[&key].clone();
            assert_eq!(npc.base.ship, record.object);
            assert_ne!(record.object, 0);
            assert_eq!(npc.nickname, spawn.nickname);
            assert_eq!(npc.controller, mission.0, "the mission's ship");
            assert_eq!(npc.creation_kind, 1);
            assert_eq!(npc.base.system, Some(o.system));
            // Its reputation: a named handle of the ship's faction.
            let vibe = ships::reputation(&cx.logic.content.state, ShipKey::Npc(key));
            assert_ne!(vibe, -1);
            assert_eq!(
                api::reputation::affiliation(cx, vibe),
                Ok(spawn.affiliation)
            );
            if spawn.target_name {
                let (first, second) = api::reputation::name(cx, vibe).expect("a named vibe");
                assert_eq!(first, o.target_name.first, "the offer's target name");
                assert_eq!(second, o.target_name.second);
            }
            // The formation: one new formation, a slot each.
            assert_eq!(npc.base.formation_slot, slot as u32);
            assert_eq!(npc.base.formation, Some(group.formation));
            formation_ids.push(npc.base.formation_id);
            // The attitude towards the mission's player (`:1898-1906`), as
            // the reputation store keeps it.
            let attitude = api::reputation::attitude(cx, vibe, player_vibe).expect("an attitude");
            assert!((attitude - group.field_0x40).abs() < 1e-6, "{attitude}");
        }
        assert!(
            formation_ids
                .iter()
                .all(|&f| f == formation_ids[0] && f != u32::MAX)
        );
        // The first ship goes to the group's position: its queue or its
        // active directive holds the `GotoVec` (500).
        let first = records[0].npc_ship.expect("a ship");
        let queue = &cx.logic.content.state.npc_ships[&first].directives;
        let arena = &cx.logic.content.state.directives;
        let held: Vec<i32> = queue
            .list
            .iter()
            .chain(queue.active.iter())
            .map(|&d| arena.type_code(d))
            .collect();
        assert!(!held.is_empty(), "the first ship has its directives");
        assert!(held.contains(&5), "{held:?}");
        let _ = player_ship;
    });
}

#[test]
fn a_comm_sequence_is_admitted_and_spoken() {
    with_battle(|cx, mission, ship| {
        // `PLAYER_IN_PRE_BATTLE_SPAWN_RANGE` (trigger 3), the base speaking
        // to the players in range, after its delay.
        let o = offer(cx, mission);
        let index = o
            .general
            .conversations
            .iter()
            .position(|c| c.trigger == 3)
            .expect("the offer's pre-battle comm sequence");
        let delay = o.general.conversations[index].delay;
        // Prepared on the offer's own line (`:389-416`): ready, the base's
        // voice and costume, comm type 0x32d7, player 7 in range, one
        // voice line of the text, mood 0x4a39.
        let c = &o.general.conversations[index];
        assert!(c.ready);
        let line = &c.lines[0];
        assert_eq!(line.speaker, 1, "BASE");
        assert_eq!(line.target, 8, "PLAYERS_IN_RANGE");
        assert_eq!(line.line.speaker, 0);
        assert_eq!(line.line.voice, o.voice);
        assert_eq!(line.line.costume, o.mc_costume);
        assert_eq!(line.line.comm_type, 0x32d7);
        assert_eq!(line.line.players.as_deref(), Some(&[PLAYER][..]));
        assert_eq!(line.line.mood, 0x4a39);
        assert!(line.line.prepared);
        assert_eq!(line.line.texts.len(), 1);
        let text = line.line.texts[0];
        assert_ne!(
            text,
            0,
            "a voice line of `{:?}`",
            String::from_utf8_lossy(&line.text)
        );

        // Delivered after its delay, admitted and started.
        for _ in 0..=(delay.ceil() as u32) {
            if cx
                .logic
                .content
                .state
                .gcs
                .conversations
                .iter()
                .any(|c| c.kind == Kind::RandomMission && c.lines[0].texts == [text])
            {
                break;
            }
            tick(cx, mission);
        }
        let gcs = &cx.logic.content.state.gcs;
        let queued = gcs
            .conversations
            .iter()
            .find(|c| c.kind == Kind::RandomMission && c.lines[0].texts == [text])
            .expect("the comm system queued the conversation");
        assert_eq!(queued.lines.len(), 1);
        // Spoken: the line's end is queued for player 7's ship, after its
        // duration and 1.25 s.
        let duration = gcs::lines::duration(cx, &queued.lines[0]);
        let done = gcs
            .delayed
            .iter()
            .find(|d| d.text == text)
            .copied()
            .expect("the line's end");
        assert_eq!((done.listener, done.speaker), (ship, 0));
        assert_eq!(done.system, o.system);
        assert_eq!(done.delay, (f64::from(duration) + 1.25) as f32);

        // Its `CommComplete` ends the conversation (`OnMessage`).
        let count = cx.logic.content.state.gcs.conversations.len();
        let space_support = cx.logic.content.state.space_support;
        let _ = cx.dispatch(
            space_support,
            &mut ControllerMessage::CommComplete {
                listener: ship,
                system: o.system,
                speaker: 0,
                line: text,
                result: CommResult::Success,
            },
        );
        let left = &cx.logic.content.state.gcs.conversations;
        assert_eq!(left.len(), count - 1);
        assert!(
            !left
                .iter()
                .any(|c| c.kind == Kind::RandomMission && c.lines[0].texts == [text])
        );
    });
}

#[test]
fn a_player_list_line_speaks_to_each_player_with_a_ship() {
    with_battle(|cx, _, ship| {
        let system = cx.logic.content.state.players[&PLAYER].system;
        // Player 8 has a state and no ship; player 9 none.
        let state = PlayerState::new(cx, 8);
        cx.logic.content.state.players.insert(8, state);
        let mut line = Line::player_list(None);
        line.mood = 0x4a39;
        line.players = Some(vec![9, 8]);
        // Nobody with a ship: it cannot play.
        line.texts = vec![create_id(b"rmb_inrangeships_01-")];
        assert!(!gcs::lines::can_play(cx, &line));
        // Player 7 decides: with a text and no speaker it plays.
        line.players = Some(vec![9, 8, PLAYER]);
        assert!(gcs::lines::can_play(cx, &line));
        // A speaker that is gone refuses.
        line.speaker = 0x7fff_0000;
        assert!(!gcs::lines::can_play(cx, &line));
        line.speaker = 0;
        // A random mission's line also needs to be prepared.
        line.prepared = false;
        assert!(!gcs::lines::can_play(cx, &line));
        line.prepared = true;

        // Played: the line's end for the last listed player's ship, in its
        // system.
        let before = cx.logic.content.state.gcs.delayed.len();
        gcs::lines::play(cx, &line);
        let delayed = &cx.logic.content.state.gcs.delayed;
        assert_eq!(delayed.len(), before + 1);
        let done = delayed[before];
        assert_eq!(done.listener, ship);
        assert_eq!(Some(done.system), system);
        // When the last listed player has no ship, no end is queued.
        line.players = Some(vec![PLAYER, 8]);
        gcs::lines::play(cx, &line);
        assert_eq!(cx.logic.content.state.gcs.delayed.len(), before + 1);

        // Ended by a message whose listener is a listed player's ship, with
        // its speaker and last text; the system is not compared.
        let message = |listener, speaker, text| LineDone {
            listener,
            system: 0,
            speaker,
            text,
        };
        let text = line.texts[0];
        assert!(gcs::lines::is_done(cx, &line, &message(ship, 0, text)));
        assert!(!gcs::lines::is_done(cx, &line, &message(ship, 5, text)));
        assert!(!gcs::lines::is_done(cx, &line, &message(ship, 0, text + 1)));
        line.players = Some(vec![8]);
        assert!(!gcs::lines::is_done(cx, &line, &message(ship, 0, text)));
    });
}

#[test]
fn an_aborted_mission_lets_its_ships_wander() {
    with_battle(|cx, mission, _| {
        let records = ship_records(cx, mission);
        assert!(records.len() >= 2, "{records:#?}");
        let mut abort = ControllerMessage::AbortMission {
            player: PLAYER,
            mission: mission.0,
        };
        let _ = api::controller::send_message(cx, mission, &mut abort);
        // `Shutdown` (`RandomMissionMachine.cpp:2147-2163`): every alive
        // ship's life 24 and its zone cleared; the leader (slot 0), no
        // formation wingman, gets a wander guide (type 4); a wingman keeps
        // what it had.
        for record in &records {
            let key = record.npc_ship.expect("a ship");
            let npc = &cx.logic.content.state.npc_ships[&key];
            assert_eq!(npc.life_time, 24.0);
            let guide = npc
                .guide
                .map(|g| game_logic::content::guides::guide_type(cx, g));
            if npc.base.formation_slot == 0 {
                assert_eq!(guide, Some(4), "the leader wanders");
            } else {
                assert_ne!(guide, Some(4), "a wingman follows its leader");
            }
        }
    });
}
