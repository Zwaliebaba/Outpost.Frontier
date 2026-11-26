//! The run time on a synthetic mission: the router's order and timers, a
//! destroy-solars machine driven to its success and to its failure.

use std::sync::Arc;

use game_sim::data::config::GameData;
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use neuron_core::math::Vector;
use neuron_server::controller::{self, ControllerId, Priority};

use super::message::{Body, MessageType, RmMessage};
use super::sub::SpawnRecord;
use super::{CookieSite, MachineKind, cookie, join_player, rm, rm_mut, trace};
use crate::api;
use crate::content::Instance;
use crate::content::players::PlayerState;
use crate::content::random_mission::RmDataKey;
use crate::content::random_mission::offer::{Objective, Offer};
use crate::content::router;
use crate::events::{ControllerMessage, ObjectDestroyed};
use crate::server::{CreateParams, Cx, Logic, RecordingSink};

const PLAYER: u32 = 7;
const SYSTEM: u32 = 0x1234;

/// A server with GameSupport and SpaceSupport (the router and its timer's
/// target), player 7 with a state in a system, and a mission of one
/// destroy-solars offer; `run` gets the context and the mission's key.
fn with_mission(run: impl FnOnce(&mut Cx<'_>, RmDataKey)) {
    let mut world = World::new(Arc::new(GameData::new()), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for name in [&b"GameSupport"[..], b"SpaceSupport"] {
        let id = controller::create(
            &mut cx,
            b"Content.dll",
            name,
            &CreateParams::None,
            Priority::Normal,
        );
        assert_ne!(id.0, 0);
    }
    let mut state = PlayerState::new(&cx, PLAYER);
    state.system = Some(SYSTEM);
    cx.logic.content.state.players.insert(PLAYER, state);
    let key = cx.logic.content.state.rm.missions.create();
    let mut offer = Offer::new(1);
    offer.register_handlers(&mut cx);
    offer.kind = 1;
    offer.system = SYSTEM;
    offer.general.active = true;
    offer.solars.component.active = true;
    offer.solars.position = Vector::new(100.0, 0.0, 0.0);
    let objective = |kind| Objective {
        built: game_sim::protocol::fmtstr::FmtStr::new(0),
        kind,
        text: Default::default(),
        marker_system: SYSTEM,
        marker_value: 0x44d,
        marker_position: Vector::ZERO,
        targets: Vec::new(),
    };
    offer.general.objectives.insert(2, objective(2));
    offer.general.objectives.insert(9, objective(2));
    offer.solars.component.objectives.insert(4, objective(3));
    offer.solars.component.objectives.insert(5, objective(3));
    if let Some(d) = cx.logic.content.state.rm.missions.get_mut(key) {
        d.player = PLAYER;
        d.dest_sys = SYSTEM;
        d.reward = 1000;
        d.offers.push(offer);
    }
    run(&mut cx, key);
}

/// One second of the session: the game time, the heartbeat, the timers.
fn tick(cx: &mut Cx<'_>, mission: ControllerId) {
    cx.logic.content.state.game_time += 1.0;
    let mut heartbeat = ControllerMessage::Heartbeat { elapsed: 1.0 };
    let _ = api::controller::send_message(cx, mission, &mut heartbeat);
    controller::fire_timers(cx, 1.0);
}

/// The mission's controller, made and joined by player 7.
fn start(cx: &mut Cx<'_>, key: RmDataKey) -> ControllerId {
    let mission = api::controller::create(
        cx,
        b"content.dll",
        b"DestroyMission",
        &CreateParams::RandomMission(key),
        Priority::AboveNormal,
    );
    assert_ne!(mission.0, 0);
    join_player(cx, mission, PLAYER);
    mission
}

/// The mission's vignette machine and its state.
fn vignette(cx: &Cx<'_>, mission: ControllerId) -> Option<(u32, i32)> {
    rm(cx, mission)?
        .machines
        .iter()
        .find_map(|(&id, m)| match m.kind {
            MachineKind::Vignette(_) => Some((id, m.sm.state())),
            _ => None,
        })
}

/// A machine's sub-machine of `pick`.
fn solar_machine(cx: &Cx<'_>, mission: ControllerId) -> u32 {
    rm(cx, mission)
        .and_then(|r| {
            r.machines.iter().find_map(|(&id, m)| match m.kind {
                MachineKind::Solars(_) => Some(id),
                _ => None,
            })
        })
        .expect("a solar machine")
}

/// A trigger's fired message to the machine, at once.
fn fire(cx: &mut Cx<'_>, machine: u32, site: CookieSite) {
    let c = cookie(cx, site);
    router::route(
        cx,
        RmMessage::new(
            MessageType::TriggerFired,
            Body::TriggerFired {
                cookie: c,
                last: false,
                activator: 0,
            },
        ),
        0.0,
        machine,
        0,
        -1,
    );
}

#[test]
fn the_router_queues_by_time_and_fires_its_head() {
    with_mission(|cx, _| {
        trace::start(cx);
        let first = router::register(cx, router::HandlerOwner::Component);
        let second = router::register(cx, router::HandlerOwner::Component);
        assert_eq!(second, first + 1);
        router::route(
            cx,
            RmMessage::plain(MessageType::Timeout),
            2.0,
            first,
            second,
            -1,
        );
        router::route(
            cx,
            RmMessage::plain(MessageType::Cookie),
            1.0,
            first,
            second,
            -1,
        );
        router::route(
            cx,
            RmMessage::plain(MessageType::CountdownTick),
            1.0,
            first,
            second,
            -1,
        );
        let pending: Vec<_> = cx
            .logic
            .content
            .state
            .router
            .as_ref()
            .expect("router")
            .pending()
            .iter()
            .map(|m| m.kind())
            .collect();
        // A later time goes after every message of its time or earlier.
        assert_eq!(
            pending,
            [
                Some(MessageType::Cookie),
                Some(MessageType::CountdownTick),
                Some(MessageType::Timeout)
            ]
        );
        // Three timers on SpaceSupport; the first to expire delivers the
        // head, whichever message armed it.
        controller::fire_timers(cx, 1.5);
        let left = cx
            .logic
            .content
            .state
            .router
            .as_ref()
            .expect("router")
            .pending()
            .len();
        assert_eq!(left, 1);
        router::unregister(cx, first);
        controller::fire_timers(cx, 1.0);
        let lines = trace::stop(cx);
        assert!(
            lines
                .iter()
                .any(|l| l.contains("\"type\":\"Timeout\"") && l.contains("\"found\":0")),
            "{lines:#?}"
        );
        // An immediate message to no handler is dropped.
        router::route(cx, RmMessage::plain(MessageType::Timeout), 0.0, 999, 0, -1);
    });
}

#[test]
fn a_destroy_solars_machine_succeeds() {
    with_mission(|cx, key| {
        let mission = start(cx, key);
        // The brief's timeout after 1.1 s, the start 0.01 s later.
        tick(cx, mission);
        assert!(vignette(cx, mission).is_none());
        tick(cx, mission);
        let (machine, state) = vignette(cx, mission).expect("a machine");
        assert_eq!(state, 1);
        tick(cx, mission);
        // No target ships: the approach to the main battle.
        assert_eq!(vignette(cx, mission), Some((machine, 10)));
        assert_eq!(rm(cx, mission).map(|r| r.sm.state()), Some(2));
        // A solar to kill, as the solars' spawn would record it.
        let solars = solar_machine(cx, mission);
        if let Some(r) = rm_mut(cx, mission)
            && let Some(MachineKind::Solars(s)) = r.machines.get_mut(&solars).map(|m| &mut m.kind)
        {
            s.records.push(SpawnRecord {
                object: 0x55,
                flags: 2,
                solar_type: 0,
                ..SpawnRecord::default()
            });
        }
        fire(cx, machine, CookieSite::MainShips);
        fire(cx, machine, CookieSite::MainNear);
        assert_eq!(vignette(cx, mission), Some((machine, 11)));
        // The main battle marks the solar (objective 4); its destruction
        // ends the battle.
        let mut destroyed = ControllerMessage::ObjectDestroyed(ObjectDestroyed {
            id: 0x55,
            killer: 0,
            system: SYSTEM,
            owner_player: 0,
            kind: 1,
            position: Vector::ZERO,
            dropped_equip: Vec::new(),
        });
        let _ = api::controller::send_message(cx, mission, &mut destroyed);
        assert_eq!(vignette(cx, mission), Some((machine, 5)));
        // 5 s later the machine succeeded; the mission succeeds and ends.
        for _ in 0..8 {
            tick(cx, mission);
        }
        let state = &cx.logic.content.state.players[&PLAYER];
        assert_eq!(state.stats.random_missions_completed.get(&1), Some(&1));
        assert!(rm(cx, mission).is_none_or(|r| r.pending_destroy));
        controller::tick(cx, 0.0);
        assert!(!cx.logic.content.instances.contains_key(&mission));
        assert!(matches!(
            cx.logic.content.instances.values().next(),
            Some(Instance::GameSupport | Instance::SpaceSupport)
        ));
    });
}

#[test]
fn a_mission_fails_when_its_last_player_dies_in_the_area() {
    with_mission(|cx, key| {
        let mission = start(cx, key);
        for _ in 0..3 {
            tick(cx, mission);
        }
        let (machine, _) = vignette(cx, mission).expect("a machine");
        let mut destroyed = ControllerMessage::ObjectDestroyed(ObjectDestroyed {
            id: 0x77,
            killer: 0,
            system: SYSTEM,
            owner_player: PLAYER,
            kind: 1,
            position: Vector::new(100.0, 0.0, 0.0),
            dropped_equip: Vec::new(),
        });
        let _ = api::controller::send_message(cx, mission, &mut destroyed);
        assert_eq!(vignette(cx, mission), Some((machine, 6)));
        for _ in 0..8 {
            tick(cx, mission);
        }
        // Reason 3 outside single player.
        let state = &cx.logic.content.state.players[&PLAYER];
        assert_eq!(state.stats.random_missions_failed.get(&1), Some(&1));
        assert!(rm(cx, mission).is_none_or(|r| r.pending_destroy && r.fail_reason == 3));
    });
}
