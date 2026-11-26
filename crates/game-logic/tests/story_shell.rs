//! The story shell (work package P3.5d): Story's handler and events and
//! the story missions' overrides of `game_logic::content::{story,
//! mission::story_missions}`, on a bare world (no data root: the missions'
//! files do not open, so their scripts are empty). The Tier B comparison
//! with the C++ is `story_session.rs`.

use std::sync::Arc;

use game_logic::content::Instance;
use game_logic::content::mission::engine::{self, MISSION_STATE_SUCCEED};
use game_logic::content::mission::runtime;
use game_logic::content::players::PlayerState;
use game_logic::content::story::{self, NO_MISSION};
use game_logic::events::ControllerMessage;
use game_logic::server::{ControllerKind, CreateParams, Cx, Logic, RecordingSink};
use game_sim::data::config::GameData;
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use neuron_core::ini::IniReader;
use neuron_server::controller::{self, ControllerId, Priority};

const PLAYER: u32 = 3;

fn world(single_player: bool) -> World {
    let config = WorldConfig {
        single_player,
        ..WorldConfig::default()
    };
    World::new(Arc::new(GameData::new()), config)
}

/// A server with player 3's Content state and a Story for him made by a
/// StoryLauncher on `PlayerLogin`; `run` gets the context and the Story.
fn with_story(single_player: bool, run: impl FnOnce(&mut Cx<'_>, ControllerId)) {
    let mut world = world(single_player);
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, !single_player);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let state = PlayerState::new(&cx, PLAYER);
    cx.logic.content.state.players.insert(PLAYER, state);
    let launcher = controller::create(
        &mut cx,
        b"Content.dll",
        b"StoryLauncher",
        &CreateParams::None,
        Priority::BelowNormal,
    );
    let mut login = ControllerMessage::PlayerLogin { player: PLAYER };
    controller::dispatch(&mut cx, launcher, &mut login).expect("the launcher");
    let story = cx
        .logic
        .content
        .instances
        .iter()
        .find(|(_, i)| matches!(i, Instance::Story(s) if s.player == PLAYER))
        .map(|(&id, _)| id)
        .expect("the launcher made Story");
    run(&mut cx, story);
}

fn story_of<'c>(cx: &'c Cx<'_>, id: ControllerId) -> &'c story::Story {
    story::story(cx, id).expect("a Story")
}

fn mission_kind(cx: &Cx<'_>, id: ControllerId) -> Option<ControllerKind> {
    runtime::script(cx, id).map(|s| s.kind)
}

fn load(cx: &mut Cx<'_>, to: ControllerId, text: &[u8]) {
    let mut reader = IniReader::new();
    reader.open_memory(text);
    let mut message = ControllerMessage::PlayerLoad {
        player: PLAYER,
        reader: &mut reader,
    };
    controller::dispatch(cx, to, &mut message).expect("a controller");
}

fn send(cx: &mut Cx<'_>, to: ControllerId, mut message: ControllerMessage<'_>) {
    controller::dispatch(cx, to, &mut message).expect("a controller");
}

fn succeed(cx: &mut Cx<'_>, mission: ControllerId) {
    runtime::script_mut(cx, mission)
        .expect("a mission")
        .mission_state = MISSION_STATE_SUCCEED;
    engine::apply_desired_state(cx, mission);
}

#[test]
fn a_new_story_waits_and_the_new_players_save_makes_mission_01a() {
    with_story(false, |cx, story| {
        let s = story_of(cx, story);
        assert_eq!((s.mission_num, s.delta_worth), (0, -1.0));
        assert_eq!(s.mission_name, NO_MISSION);
        load(
            cx,
            story,
            b"[StoryInfo]\nMission = Mission_01a\nMissionNum = 1\ndelta_worth = -1\n",
        );
        let mission = cx.logic.content.state.story_mission;
        assert_eq!(mission_kind(cx, mission), Some(ControllerKind::Mission01a));
        let script = runtime::script(cx, mission).unwrap();
        assert_eq!((script.player, script.story), (PLAYER, story));
        assert_eq!(story_of(cx, story).mission_num, 1);
        assert_eq!(cx.logic.content.state.players[&PLAYER].mission_number, 1);
    });
}

#[test]
fn mission_01a_reports_its_acceptance_and_its_success_makes_mission_01b() {
    with_story(false, |cx, story| {
        load(
            cx,
            story,
            b"[StoryInfo]\nMission = Mission_01a\nMissionNum = 1\n",
        );
        let m01a = cx.logic.content.state.story_mission;
        send(
            cx,
            m01a,
            ControllerMessage::MissionResponse {
                offer: m01a.0,
                kind: 0,
                accepted: true,
                player: PLAYER,
            },
        );
        assert_eq!(story_of(cx, story).mission_num, 2);
        succeed(cx, m01a);
        let m01b = cx.logic.content.state.story_mission;
        assert_ne!(m01b, m01a);
        assert_eq!(mission_kind(cx, m01b), Some(ControllerKind::Mission01b));
        assert_eq!(story_of(cx, story).mission_num, 4);
        assert_eq!(story_of(cx, story).mission_name, b"Mission_01b");
        // Mission_01a is queued for its teardown.
        controller::tick(cx, 0.0);
        assert!(mission_kind(cx, m01a).is_none());

        // Mission_01b's first base enter is its acceptance.
        send(
            cx,
            m01b,
            ControllerMessage::BaseEnter {
                base: 9,
                player: PLAYER,
            },
        );
        assert!(
            runtime::script(cx, m01b)
                .unwrap()
                .story_fields
                .base_enter_handled
        );
        assert_eq!(cx.logic.content.state.clamps.attack_clamp, 1);
        succeed(cx, m01b);
        let s = story_of(cx, story);
        assert_eq!(s.mission_num, 5);
        assert_eq!(s.mission_name, NO_MISSION);
        // Chapter 5's threshold over a worth of 0.
        assert_eq!(s.delta_worth, 3500.0);
    });
}

#[test]
fn the_worth_starts_the_waiting_chapter_unless_a_mission_is_live() {
    with_story(false, |cx, story| {
        load(
            cx,
            story,
            b"[StoryInfo]\nMission = No_Mission\nMissionNum = 21\ndelta_worth = 0\n",
        );
        let before = cx.logic.content.state.story_mission;
        // A live mission holds the chapter back.
        cx.logic
            .content
            .state
            .players
            .get_mut(&PLAYER)
            .unwrap()
            .live_mission = 0x55;
        send(
            cx,
            story,
            ControllerMessage::PlayerWorthChanged { client: PLAYER },
        );
        assert_eq!(cx.logic.content.state.story_mission, before);
        // Another player's mission change does nothing; his own, leaving
        // the mission, starts Mission_07.
        cx.logic
            .content
            .state
            .players
            .get_mut(&PLAYER)
            .unwrap()
            .live_mission = 0;
        send(
            cx,
            story,
            ControllerMessage::PlayerMissionChanged {
                mission: ControllerId(0),
                player: PLAYER + 1,
            },
        );
        assert_eq!(cx.logic.content.state.story_mission, before);
        send(
            cx,
            story,
            ControllerMessage::PlayerMissionChanged {
                mission: ControllerId(0),
                player: PLAYER,
            },
        );
        let m07 = cx.logic.content.state.story_mission;
        assert_eq!(mission_kind(cx, m07), Some(ControllerKind::Mission(7)));
        let s = story_of(cx, story);
        assert_eq!((s.mission_num, s.delta_worth), (22, -1.0));
        // The worth is -1 again: a second change starts nothing.
        send(
            cx,
            story,
            ControllerMessage::PlayerWorthChanged { client: PLAYER },
        );
        assert_eq!(cx.logic.content.state.story_mission, m07);
    });
}

#[test]
fn a_ship_bought_in_chapters_15_to_22_is_noted_once() {
    with_story(false, |cx, story| {
        load(cx, story, b"[StoryInfo]\nMissionNum = 14\n");
        send(
            cx,
            story,
            ControllerMessage::PlayerShipArchSet { client: PLAYER },
        );
        assert!(!story_of(cx, story).ship_bought);
        load(cx, story, b"[StoryInfo]\nMissionNum = 15\n");
        send(
            cx,
            story,
            ControllerMessage::PlayerShipArchSet { client: PLAYER + 1 },
        );
        assert!(!story_of(cx, story).ship_bought);
        send(
            cx,
            story,
            ControllerMessage::PlayerShipArchSet { client: PLAYER },
        );
        assert!(story_of(cx, story).ship_bought);
        // Mission_07's acceptance (chapter 23) notes it too.
        load(cx, story, b"[StoryInfo]\nMissionNum = 2\n");
        story::on_mission_event(cx, story, 1, 23, 0);
        let s = story_of(cx, story);
        assert_eq!((s.mission_num, s.ship_bought), (23, true));
    });
}

#[test]
fn each_success_arm_sets_its_chapters() {
    with_story(false, |cx, story| {
        // 20: finished at 21, Mission_07 started at 22.
        story::on_mission_event(cx, story, 3, 20, 0);
        let m07 = cx.logic.content.state.story_mission;
        assert_eq!(mission_kind(cx, m07), Some(ControllerKind::Mission(7)));
        assert_eq!(story_of(cx, story).mission_num, 22);
        // 23: finished at 24 with its threshold.
        story::on_mission_event(cx, story, 3, 23, m07.0);
        let s = story_of(cx, story);
        assert_eq!((s.mission_num, s.delta_worth), (24, 36000.0));
        assert_eq!(s.mission_name, NO_MISSION);
        // 38: Mission_13 at 40.
        story::on_mission_event(cx, story, 3, 38, 0);
        let m13 = cx.logic.content.state.story_mission;
        assert_eq!(mission_kind(cx, m13), Some(ControllerKind::Mission(13)));
        assert_eq!(story_of(cx, story).mission_num, 40);
        // 41: the story's end; the worth stays.
        story::on_mission_event(cx, story, 3, 41, m13.0);
        let s = story_of(cx, story);
        assert_eq!(s.mission_num, 42);
        assert_eq!(cx.logic.content.state.players[&PLAYER].mission_number, 42);
        // Neither the worth nor a mission change starts anything now.
        send(
            cx,
            story,
            ControllerMessage::PlayerWorthChanged { client: PLAYER },
        );
        assert_eq!(cx.logic.content.state.story_mission, m13);
        // Unknown kinds and chapters do nothing.
        story::on_mission_event(cx, story, 2, 4, 0);
        story::on_mission_event(cx, story, 3, 0, 0);
        assert_eq!(story_of(cx, story).mission_num, 42);
        // A controller without a Story hears nothing.
        story::on_mission_event(cx, ControllerId(0), 1, 9, 0);
    });
}

#[test]
fn mission_01a_saves_and_restores_its_state_in_single_player() {
    with_story(true, |cx, story| {
        load(
            cx,
            story,
            b"[StoryInfo]\nMission = Mission_01a\nMissionNum = 1\n",
        );
        let m01a = cx.logic.content.state.story_mission;
        let he0101 = runtime::script(cx, m01a)
            .unwrap()
            .story_fields
            .bases
            .unwrap()
            .0;
        let save = |cx: &mut Cx<'_>| {
            let mut out = Vec::new();
            send(
                cx,
                m01a,
                ControllerMessage::PlayerSave {
                    client: PLAYER,
                    file: &mut out,
                },
            );
            String::from_utf8(out).unwrap()
        };
        assert!(!save(cx).contains("[Mission01aSave]"));
        send(
            cx,
            m01a,
            ControllerMessage::BaseEnter {
                base: he0101,
                player: PLAYER,
            },
        );
        let text = save(cx);
        assert!(
            text.contains("\n[Mission01aSave]\nMissionStateNum = 3\n\n"),
            "{text}"
        );
        load(cx, m01a, b"[Mission01aSave]\nMissionStateNum = 5\n");
        let s = runtime::script(cx, m01a).unwrap();
        assert_eq!((s.previous_state, s.mission_state), (5, 5));
    });
}

#[test]
fn mission_01b_restores_its_acceptance() {
    with_story(true, |cx, story| {
        story::on_mission_event(cx, story, 3, 2, 0);
        let m01b = cx.logic.content.state.story_mission;
        assert_eq!(mission_kind(cx, m01b), Some(ControllerKind::Mission01b));
        load(cx, story, b"[StoryInfo]\nMissionNum = 3\n");
        load(cx, m01b, b"[Mission01bSave]\nMissionStateNum = 7\n");
        let s = runtime::script(cx, m01b).unwrap();
        assert!(s.story_fields.base_enter_handled);
        assert_eq!(s.mission_state, 7);
        assert_eq!(story_of(cx, story).mission_num, 4);
        assert_eq!(cx.logic.content.state.clamps.attack_clamp, 1);
        let mut out = Vec::new();
        send(
            cx,
            m01b,
            ControllerMessage::PlayerSave {
                client: PLAYER,
                file: &mut out,
            },
        );
        // The save's tail applied the restored desired state first.
        let text = String::from_utf8(out).unwrap();
        assert!(
            text.ends_with("\n[Mission01bSave]\nMissionStateNum = 7\n\n"),
            "{text}"
        );
    });
}
