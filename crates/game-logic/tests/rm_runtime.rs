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
//! - The mission object of TU38 in single player (P3.7d,
//!   `Content/MissionObject.cpp:1496-1925` through
//!   `FRandomMissionObject::Slot2`): the snapshot of `SpaceExit` and of a
//!   save, the respawn that `SpaceEnter` does not make, the solars of
//!   `PlayerLaunch`, the jump of the player's ship and the jumpers.
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

/// A server on `outdir/` started as the oracle starts it
/// (`rm_session.rs`), multiplayer but in single player when
/// `single_player` (P3.7d); `None` without the data.
fn started_with(single_player: bool) -> Option<(World, Logic)> {
    let root = outdir();
    let Some(config) = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini") else {
        println!("notice: outdir/OutpostFrontier.ini is missing; nothing was run");
        return None;
    };
    let data_dir = root.join("data");
    let mut data = GameData::new();
    data.load(&Host::new(&data_dir), &config, None);
    let world_config = WorldConfig {
        single_player,
        ..WorldConfig::default()
    };
    let mut world =
        World::new(Arc::new(data), world_config).with_assets(Assets::new(data_dir.clone()));
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
    with_battle_in(false, run);
}

/// [`with_battle`], in single player when `single_player` (P3.7d).
fn with_battle_in(single_player: bool, run: impl FnOnce(&mut Cx<'_>, ControllerId, u32)) {
    let Some((mut world, mut logic)) = started_with(single_player) else {
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
    let o = offer(&cx, mission);
    // In single player the same seeds give another offer, whose machine
    // waits for its main battle (state 10).
    let at = if single_player && state == 10 {
        assert!(o.ships.component.active && !o.ships.groups.is_empty());
        o.ships.position
    } else {
        assert_eq!(
            state, 8,
            "the first vendor offer waits for its target ships"
        );
        assert!(o.target_ships.component.active && !o.target_ships.groups.is_empty());
        o.target_ships.position
    };
    if single_player {
        // The client's side of a single-player game loads the system's
        // solars into the shared world first.
        cx.world.load_system(o.system);
        let _ = cx
            .world
            .load_system_solars_reported(o.system, &mut cx.logic.reports);
    }
    world::load(&mut cx, o.system);
    let ship = give_ship(&mut cx, o.system, at);
    let mut enter = ControllerMessage::SpaceEnter {
        system: o.system,
        player: PLAYER,
        launch_object: 0,
        launch_kind: -1,
    };
    let _ = api::controller::send_message(&mut cx, mission, &mut enter);
    tick(&mut cx, mission);
    // The main battle: the 7.5 km trigger spawns the solars and arms the
    // 2.5 km one, whose state 11 spawns the first group.
    for _ in 0..4 {
        if !single_player || !ship_records(&cx, mission).is_empty() {
            break;
        }
        tick(&mut cx, mission);
    }
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

/// P3.7d: in single player the mission's player's `SpaceExit` takes the
/// mission object's snapshot (`MissionObject.cpp:1688-1695`), which finds
/// nothing of a random mission: its ships are its ship machine's records,
/// never TU38's NPC objects (only `FMissionScript` registers those), so no
/// respawn record, solar or attitude is kept. `SpaceEnter` is answered by
/// `FRandomMissionObject::Slot2` (`RandomMissionDB.cpp:1645-1657`), which
/// refreshes the records and makes no respawn: the ships live on, as they
/// were. A save through the bus with a ship takes the snapshot again and
/// writes, after the `[RandomMission]` section, only the record's
/// objectives; `PlayerLaunch` spawns no solar.
#[test]
fn a_random_mission_is_snapshotted_but_not_respawned_in_single_player() {
    with_battle_in(true, |cx, mission, _| {
        assert!(api::misc::single_player(cx));
        let records = ship_records(cx, mission);
        assert!(!records.is_empty(), "the first group was spawned");
        let ships: Vec<u32> = records.iter().map(|r| r.object).collect();
        let npcs = cx.logic.content.state.npc_ships.len();
        let o = offer(cx, mission);
        // `NpcShipCreated` (`:1869-1913`): each ship's labels in the label
        // map; no `[MsnShip]` record makes it a jumper.
        {
            let r = instance(cx, mission);
            assert!(r.object.npc_objects.is_empty());
            assert!(r.object.jumpers.is_empty());
            for record in &records {
                let key = record.npc_ship.expect("a ship");
                for label in &cx.logic.content.state.npc_ships[&key].field_0x17c {
                    assert!(
                        r.object
                            .formation_members
                            .get(label)
                            .is_some_and(|m| m.contains(&record.object)),
                        "label {label:#x} of {}",
                        record.object
                    );
                }
            }
        }
        let mut exit = ControllerMessage::SpaceExit {
            system: o.system,
            player: PLAYER,
        };
        let _ = api::controller::send_message(cx, mission, &mut exit);
        {
            let r = instance(cx, mission);
            assert!(r.object.respawn_ships.is_empty());
            assert!(r.object.rand_enc_respawns.is_empty());
            assert!(r.object.respawn_solars.is_empty());
            assert!(r.object.vibe_infos.is_empty());
        }
        let mut enter = ControllerMessage::SpaceEnter {
            system: o.system,
            player: PLAYER,
            launch_object: 0,
            launch_kind: -1,
        };
        let _ = api::controller::send_message(cx, mission, &mut enter);
        assert_eq!(cx.logic.content.state.npc_ships.len(), npcs, "no respawn");
        let again: Vec<u32> = ship_records(cx, mission).iter().map(|r| r.object).collect();
        assert_eq!(again, ships, "the same ships");
        // A save with the ship: the snapshot, then the record's objectives.
        let mut save = Vec::new();
        let mut message = ControllerMessage::PlayerSave {
            client: PLAYER,
            file: &mut save,
        };
        let _ = api::controller::send_message(cx, mission, &mut message);
        let text = String::from_utf8_lossy(&save).into_owned();
        assert!(
            text.starts_with("[RandomMission]\ntype = DestroyMission\n"),
            "{text}"
        );
        let after = text
            .split_once("\n\n")
            .map_or("", |(_, rest)| rest)
            .to_owned();
        let mut want = Vec::new();
        if let Some(record) = instance(cx, mission).records.values().next() {
            game_logic::content::mission::object_save::write_objectives(
                &mut want,
                &record.objectives,
            )
            .expect("in memory");
        }
        assert_eq!(after, String::from_utf8_lossy(&want));
        assert!(!after.contains("[MsnShipSave]") && !after.contains("[MsnVibeInfo]"));
        // `PlayerLaunch` (`:1548-1561`): no solar to spawn.
        let mut launch = ControllerMessage::PlayerLaunch {
            player: PLAYER,
            base: 0,
        };
        let _ = api::controller::send_message(cx, mission, &mut launch);
        assert_eq!(cx.logic.content.state.npc_ships.len(), npcs);
    });
}

/// P3.7d: the jump of the mission's player's ship (`Msg0x25`,
/// `OnShipJump`, `MissionObject.cpp:2345-2385`) notes its gate's target
/// as the mission object's jump, and carries no jumper (a random mission
/// has none); the heartbeat's `UpdateJumpers` (`:2388-2422`) and
/// `JumpInComplete` (`:1643-1647`) find no pending jump. A mission ship's
/// own jump finds no NPC object.
#[test]
fn a_random_mission_follows_its_players_jump_without_jumpers() {
    with_battle_in(true, |cx, mission, player_ship| {
        let o = offer(cx, mission);
        let gate = cx
            .world
            .objects_in(o.system)
            .into_iter()
            .filter_map(|h| cx.world.get(h).map(|obj| obj.simple.id))
            .find(|&id| {
                api::space_obj::object_type(cx, id) == Ok(0x40)
                    && api::space_obj::jump_target(cx, id).is_ok()
            })
            .expect("a jump gate in the mission's system");
        let target = api::space_obj::jump_target(cx, gate).expect("its target");
        if let Some(ship) = cx
            .logic
            .content
            .state
            .players
            .get_mut(&PLAYER)
            .and_then(|s| s.ship.as_mut())
        {
            ship.target = gate;
        }
        let mut jump = ControllerMessage::Msg0x25 {
            ship: player_ship,
            death_sequence: gate,
            system: o.system,
        };
        let _ = api::controller::send_message(cx, mission, &mut jump);
        {
            let r = instance(cx, mission);
            assert_eq!((r.object.jump_system, r.object.jump_gate), target);
            assert!(r.object.jumpers.is_empty());
            assert!(!r.object.jump_pending);
        }
        // A mission ship's jump: no NPC object of TU38 holds it.
        let records = ship_records(cx, mission);
        let mut npc_jump = ControllerMessage::Msg0x25 {
            ship: records[0].object,
            death_sequence: gate,
            system: o.system,
        };
        let _ = api::controller::send_message(cx, mission, &mut npc_jump);
        let mut done = ControllerMessage::JumpInComplete {
            arg: o.system,
            ship: player_ship,
        };
        let _ = api::controller::send_message(cx, mission, &mut done);
        tick(cx, mission);
        let r = instance(cx, mission);
        assert!(r.object.jumpers.is_empty());
        assert!(!r.object.jump_pending);
        assert_eq!(r.object.jump_timer, -1.0);
        assert_eq!((r.object.jump_system, r.object.jump_gate), target);
    });
}

/// P3.7d: a mission ship's comm lines done (`Msg0x1006`) with its first
/// directive's word 0 fail the mission through the mission object's
/// `OnMissionFail` (slot 24, `MissionObject.cpp:1813-1817`, `:1950-1961`,
/// which TU47 and TU27 keep): the records cleared, `EndMission` (the
/// machines shut down), the live mission ended. A word other than 0 does
/// nothing.
#[test]
fn a_ships_last_comm_line_with_word_zero_ends_the_mission() {
    with_battle(|cx, mission, _| {
        let records = ship_records(cx, mission);
        let mut other = ControllerMessage::Msg0x1006 {
            ship: records[0].object,
            directive_word: 5,
            one: 1,
        };
        let _ = api::controller::send_message(cx, mission, &mut other);
        assert!(!instance(cx, mission).pending_destroy);
        let mut done = ControllerMessage::Msg0x1006 {
            ship: records[0].object,
            directive_word: 0,
            one: 1,
        };
        let _ = api::controller::send_message(cx, mission, &mut done);
        assert!(
            cx.logic
                .content
                .instances
                .get(&mission)
                .is_none_or(|i| match i {
                    Instance::RandomMission(r) => r.pending_destroy && r.records.is_empty(),
                    _ => false,
                })
        );
        assert_eq!(cx.logic.content.state.players[&PLAYER].live_mission, 0);
    });
}
