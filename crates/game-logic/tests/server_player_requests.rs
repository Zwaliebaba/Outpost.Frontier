//! The client's player-state requests and the player trades on the server
//! (work package P3.10b) over the original data (`outdir/`), without a
//! network: each decoded message goes through the host's routing
//! ([`session::route`], [`session::route_common`]) as `IServerImpl`'s
//! handlers take it (`Server/IServerImpl.cpp:857-935`, `1219`,
//! `2071-2330`, `2346-2730`). The item requests change the slot only in a
//! base; the kept state is the slot's; the questions are answered to the
//! asking client; a trade moves money and cargo between two players once
//! both accept, and ends with its reason on both clients otherwise.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::events::ControllerMessage;
use game_logic::login::ServerFiles;
use game_logic::player::CharacterId;
use game_logic::replication;
use game_logic::server::{ClientSink, ControllerKind, CreateParams, Cx, Logic};
use game_logic::session;
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::equip_desc::EquipDesc;
use game_sim::equip::{EquipKind, SubObjId, types};
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::common::{
    CommonMessage, FireWeapon, JettisonCargo, PlayerTrade, PopUpDialog, RequestBestPath,
    RequestGroupPositions, RequestRankLevel, SetInterfaceState, SetMissionLog, SetVisitedState,
};
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::marshal::{GoodDesc, MarshalWriter};
use game_sim::protocol::records::{CollisionGroup, EquipRecord};
use game_sim::protocol::to_client::ServerMessage;
use game_sim::protocol::to_server::{
    ReqAddItem, ReqCollisionGroups, ReqDifficultyScale, ReqEquipment, ReqModifyItem, ReqRemoveItem,
    TradeResponse,
};
use game_sim::protocol::{ClientMessage, ToClient, ToServer};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::vfs::Host;
use neuron_server::account::AccountsRoot;
use neuron_server::controller::{self, Priority};

#[derive(Clone, Default)]
struct Shared {
    sent: Rc<RefCell<Vec<(u32, ToClient)>>>,
}

impl Shared {
    fn take(&self) -> Vec<(u32, ToClient)> {
        std::mem::take(&mut *self.sent.borrow_mut())
    }
}

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.sent.borrow_mut().push((client, message));
    }
}

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn setup(what: &str, single_player: bool, sink: &Shared) -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {what}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let config = WorldConfig {
        single_player,
        ..WorldConfig::default()
    };
    let world = World::new(Arc::new(data), config).with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(sink.clone()), 4, !single_player);
    logic.deliver_hook = Some(|_, _, kind, message| {
        if kind != ControllerKind::GameSupport {
            return;
        }
        let line = match message {
            ControllerMessage::RequestBestPath { player, words } => {
                format!("RequestBestPath {player} {words:?}")
            }
            ControllerMessage::PopUpDialog { player, button } => {
                format!("PopUpDialog {player} {button}")
            }
            ControllerMessage::TradeResponse {
                field_00,
                field_04,
                flag_08,
                offered,
                requested,
                player,
            } => format!(
                "TradeResponse {field_00} {field_04} {flag_08} {offered:?} {requested:?} {player}"
            ),
            _ => return,
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    Some((world, logic))
}

/// The server's files; none of these requests reads them.
fn files() -> ServerFiles {
    let unused = Path::new(env!("CARGO_TARGET_TMPDIR")).join("player-requests");
    ServerFiles {
        exe_dir: outdir(),
        data_dir: outdir().join("data"),
        initial_world: Vec::new(),
        accounts: AccountsRoot::new(&unused, true),
    }
}

fn he01() -> u32 {
    create_id(b"he01")
}

fn id(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world.get(h).unwrap().simple.id
}

/// The routing of the host for one message of `session`.
fn deliver(cx: &mut Cx<'_>, session: u32, message: ToServer) {
    assert!(
        session::route(cx, &files(), session, &message)
            || session::route_common(cx, session, &message)
            || game_logic::receive::deliver(cx, session, &message),
        "{message:?} is routed"
    );
}

fn common(cx: &mut Cx<'_>, session: u32, message: impl Into<CommonMessage>) {
    deliver(cx, session, ToServer::Common(message.into()));
}

fn client(cx: &mut Cx<'_>, session: u32, message: impl Into<ClientMessage>) {
    deliver(cx, session, ToServer::Client(message.into()));
}

fn trades(sent: &[(u32, ToClient)]) -> Vec<(u32, PlayerTrade)> {
    sent.iter()
        .filter_map(|(c, m)| match m {
            ToClient::Common(CommonMessage::PlayerTrade(t)) => Some((*c, t.clone())),
            _ => None,
        })
        .collect()
}

fn trade(action: u8) -> PlayerTrade {
    PlayerTrade {
        action,
        ..PlayerTrade::default()
    }
}

fn terminated(sent: &[(u32, ToClient)]) -> Vec<(u32, i8)> {
    trades(sent)
        .into_iter()
        .filter(|(_, t)| t.action == 0x02)
        .map(|(c, t)| (c, t.reason))
        .collect()
}

fn slot_items(cx: &Cx<'_>, session: u32) -> Vec<EquipDesc> {
    cx.logic.players.get(session).unwrap().equipment.clone()
}

/// The count of the slot's lines of `arch`.
fn record_count(cx: &Cx<'_>, session: u32, arch: u32) -> i32 {
    slot_items(cx, session)
        .iter()
        .filter(|e| e.arch_id == arch)
        .map(|e| e.count)
        .sum()
}

/// The count of the ship's live stacks of `arch`.
fn hold_count(cx: &Cx<'_>, h: ObjHandle, arch: u32) -> u32 {
    let eq = cx.world.get(h).unwrap().equipped.as_deref().unwrap();
    eq.equipment
        .traverse(types::CARGO, false)
        .into_iter()
        .filter_map(|slot| eq.equipment.get(slot))
        .filter(|e| e.arch_id == arch)
        .map(|e| match &e.kind {
            EquipKind::Cargo(c) => c.count,
            _ => 0,
        })
        .sum()
}

fn cash(cx: &Cx<'_>, session: u32) -> i32 {
    cx.logic.players.get(session).unwrap().cash
}

fn gold() -> u32 {
    create_id(b"commodity_gold")
}

/// Two players trading in he01, each in its own he_fighter, and a third
/// that sees both; player 1 carries ten gold. Each slot's record is its
/// ship's equipment, its equipment ids past the ship's.
struct Traders {
    ships: [ObjHandle; 2],
    gold_stack: SubObjId,
}

fn traders(cx: &mut Cx<'_>) -> Traders {
    cx.world.load_system(he01());
    let fighter = create_id(b"he_fighter");
    let mut ships = Vec::new();
    for (session, x) in [(1, 15000.0), (2, 15400.0)] {
        let spawn = ShipSpawn::new(
            fighter,
            create_id(b"li_p_he_fighter_loadout01"),
            Vector::new(x, 3000.0, -25000.0),
            Matrix::IDENTITY,
        );
        let h = cx.world.spawn_ship(he01(), &spawn).expect("he_fighter");
        game_logic::ship::rebuild_server_equip(cx, h);
        let _ = session;
        ships.push(h);
    }
    // Ten gold in the first hold, then the ships are the players'.
    // An id past the loadout's, as a record would number it.
    let gold_stack: SubObjId = 0x100;
    let mut desc = EquipDesc::new(cx.world.strings());
    desc.id = gold_stack;
    desc.arch_id = gold();
    desc.count = 10;
    desc.make_internal(cx.world.strings());
    cx.world.add_equip_item(ships[0], &desc);
    for session in 1..=3u32 {
        let slot = cx.logic.players.get_mut(session).unwrap();
        slot.client_id = session;
        slot.system_id = he01();
        slot.selected_character = CharacterId::new(format!("p{session}.fl").as_bytes());
    }
    for (session, &h) in (1..=2u32).zip(&ships) {
        let ship_id = id(cx, h);
        cx.world.get_mut(h).unwrap().simple.owner_player = session;
        let ids: Vec<SubObjId> = {
            let eq = cx.world.get(h).unwrap().equipped.as_deref().unwrap();
            eq.equipment
                .traverse(u32::MAX, false)
                .into_iter()
                .filter_map(|s| eq.equipment.get(s).map(|e| e.id))
                .collect()
        };
        let equipment: Vec<EquipDesc> = ids
            .iter()
            .filter_map(|&i| cx.world.equip_desc(h, i))
            .collect();
        let top = ids.iter().copied().max().unwrap_or(0);
        let slot = cx.logic.players.get_mut(session).unwrap();
        slot.ship_object_id = ship_id;
        slot.ship_archetype = fighter;
        slot.relative_health = 1.0;
        slot.equipment = equipment;
        while slot.equip_ids.create() <= top {}
    }
    cx.logic.players.get_mut(1).unwrap().cash = 1000;
    cx.logic.players.get_mut(2).unwrap().cash = 5000;
    for session in 1..=3 {
        world::add_player(cx, he01(), session);
    }
    for session in 1..=3u32 {
        for &h in &ships {
            let world: &World = cx.world;
            let v = cx
                .logic
                .server_world
                .systems
                .get_mut(&he01())
                .unwrap()
                .observers
                .iter_mut()
                .find(|o| o.player == session)
                .unwrap();
            let at = match v.tracked.iter().position(|t| t.object == h) {
                Some(at) => at,
                None => replication::add_tracked_object(world, v, h),
            };
            v.tracked[at].created_on_client = true;
        }
    }
    Traders {
        ships: [ships[0], ships[1]],
        gold_stack,
    }
}

/// Player 1 asks player 2, who starts the trade.
fn open_trade(cx: &mut Cx<'_>, sink: &Shared, ships: [u32; 2]) {
    common(
        cx,
        1,
        PlayerTrade {
            target_player: 2,
            ..trade(0x20)
        },
    );
    let got = trades(&sink.take());
    assert_eq!(
        got,
        [(
            2,
            PlayerTrade {
                target_player: ships[0],
                ..trade(0x20)
            }
        )]
    );
    assert_eq!(cx.logic.players.get(1).unwrap().trade_request_partner, 2);
    assert_eq!(cx.logic.players.get(2).unwrap().inbound_trade_requests, [1]);

    common(
        cx,
        2,
        PlayerTrade {
            target_player: 1,
            ..trade(0x01)
        },
    );
    let got = trades(&sink.take());
    assert_eq!(
        got,
        [
            // Player 1's request ends: reason 0 to it, the withdrawal to 2.
            (
                1,
                PlayerTrade {
                    reason: 0,
                    ..trade(0x02)
                }
            ),
            (
                2,
                PlayerTrade {
                    target_player: ships[0],
                    ..trade(0x80)
                }
            ),
            (
                2,
                PlayerTrade {
                    target_player: ships[0],
                    ..trade(0x01)
                }
            ),
            (
                1,
                PlayerTrade {
                    target_player: ships[1],
                    ..trade(0x01)
                }
            ),
        ]
    );
    for (session, other) in [(1, 2), (2, 1)] {
        let slot = cx.logic.players.get(session).unwrap();
        let offer = slot.trade_offer.as_ref().expect("each side");
        assert_eq!((offer.source, offer.target), (session, other));
        assert_eq!(slot.trade_request_partner, 0);
        assert!(slot.inbound_trade_requests.is_empty());
    }
}

fn money(cx: &mut Cx<'_>, session: u32, amount: i32) {
    common(
        cx,
        session,
        PlayerTrade {
            money: amount,
            ..trade(0x04)
        },
    );
}

fn accept(cx: &mut Cx<'_>, session: u32, accepted: bool) {
    common(
        cx,
        session,
        PlayerTrade {
            accepted,
            ..trade(0x40)
        },
    );
}

fn offer_gold(cx: &mut Cx<'_>, stack: SubObjId, count: i32) {
    common(
        cx,
        1,
        PlayerTrade {
            equip_arch_id: gold(),
            equip_count: count,
            equip_health: 1.0,
            equip_id: stack,
            ..trade(0x08)
        },
    );
}

#[test]
fn a_full_trade_moves_money_and_cargo_both_ways() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("a trade", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Traders { ships, gold_stack } = traders(&mut cx);
    let ship_ids = [id(&cx, ships[0]), id(&cx, ships[1])];
    let _ = sink.take();
    open_trade(&mut cx, &sink, ship_ids);

    // Player 1 offers four gold and 300; player 2 offers 1200.
    offer_gold(&mut cx, gold_stack, 4);
    let added = PlayerTrade {
        equip_arch_id: gold(),
        equip_count: 4,
        equip_health: 1.0,
        equip_id: gold_stack,
        target_player: ship_ids[0],
        ..trade(0x08)
    };
    assert_eq!(
        trades(&sink.take()),
        [(1, added.clone()), (2, added)],
        "both clients hear the item with the offering ship"
    );
    money(&mut cx, 1, 300);
    money(&mut cx, 2, 1200);
    let got = trades(&sink.take());
    assert_eq!(
        got.iter()
            .map(|(c, t)| (*c, t.money, t.target_player))
            .collect::<Vec<_>>(),
        [
            (1, 300, ship_ids[0]),
            (2, 300, ship_ids[0]),
            (2, 1200, ship_ids[1]),
            (1, 1200, ship_ids[1]),
        ]
    );
    let offer = cx
        .logic
        .players
        .get(1)
        .unwrap()
        .trade_offer
        .clone()
        .unwrap();
    assert_eq!(offer.money, 300);
    assert_eq!(
        offer
            .items
            .iter()
            .map(|e| (e.arch_id, e.count))
            .collect::<Vec<_>>(),
        [(gold(), 4)]
    );

    // Player 1 accepts; a change by player 2 clears it.
    accept(&mut cx, 1, true);
    assert_eq!(
        trades(&sink.take()),
        [(
            2,
            PlayerTrade {
                accepted: true,
                target_player: ship_ids[0],
                ..trade(0x40)
            }
        )]
    );
    money(&mut cx, 2, 1000);
    let _ = sink.take();
    let accepted = |cx: &Cx<'_>, s: u32| {
        cx.logic
            .players
            .get(s)
            .unwrap()
            .trade_offer
            .as_ref()
            .unwrap()
            .accepted
    };
    assert!(!accepted(&cx, 1));

    // Both accept: the trade settles.
    accept(&mut cx, 2, true);
    assert!(accepted(&cx, 2));
    assert_eq!(cash(&cx, 1), 1000);
    accept(&mut cx, 1, true);
    let sent = sink.take();
    assert_eq!(cash(&cx, 1), 1000 - 300 + 1000);
    assert_eq!(cash(&cx, 2), 5000 - 1000 + 300);
    assert_eq!(record_count(&cx, 1, gold()), 6);
    assert_eq!(record_count(&cx, 2, gold()), 4);
    assert_eq!(hold_count(&cx, ships[0], gold()), 6);
    assert_eq!(hold_count(&cx, ships[1], gold()), 4, "a new stack");
    // The new line's id is the receiver's, and its ship's stack has it.
    let line = slot_items(&cx, 2)
        .into_iter()
        .find(|e| e.arch_id == gold())
        .unwrap();
    assert!(
        cx.world
            .get(ships[1])
            .unwrap()
            .equipped
            .as_deref()
            .unwrap()
            .equipment
            .by_id(line.id)
            .is_some()
    );
    // Both clients hear their cash and cargo, the others the two holds.
    let server: Vec<(u32, &ServerMessage)> = sent
        .iter()
        .filter_map(|(c, m)| match m {
            ToClient::Server(s) => Some((*c, s)),
            ToClient::Common(_) => None,
        })
        .collect();
    let cash_to: Vec<(u32, u32)> = server
        .iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::SetCash(s) => Some((*c, s.value)),
            _ => None,
        })
        .collect();
    assert_eq!(cash_to, [(1, 1700), (2, 4300)]);
    assert!(
        server
            .iter()
            .any(|(c, m)| *c == 1 && matches!(m, ServerMessage::SetRemoveItem(_)))
    );
    assert!(
        server
            .iter()
            .any(|(c, m)| *c == 2 && matches!(m, ServerMessage::SetAddItem(_)))
    );
    let remote: Vec<(u32, u32)> = server
        .iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::RemoteObjectCargoUpdate(u) => Some((*c, u.object_id)),
            _ => None,
        })
        .collect();
    assert!(remote.contains(&(2, ship_ids[0])) && remote.contains(&(3, ship_ids[0])));
    assert!(remote.contains(&(1, ship_ids[1])) && remote.contains(&(3, ship_ids[1])));
    assert!(
        !remote.contains(&(1, ship_ids[0])),
        "not to the ship's owner"
    );
    // The trade is over: reason 1 to both.
    assert_eq!(terminated(&sent), [(2, 1), (1, 1)]);
    assert!(cx.logic.players.get(1).unwrap().trade_offer.is_none());
    assert!(cx.logic.players.get(2).unwrap().trade_offer.is_none());
}

#[test]
fn a_cancelled_trade_and_the_refusals_change_nothing() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the trade refusals", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let Traders { ships, gold_stack } = traders(&mut cx);
    let ship_ids = [id(&cx, ships[0]), id(&cx, ships[1])];
    let _ = sink.take();
    let unchanged = |cx: &Cx<'_>| {
        assert_eq!((cash(cx, 1), cash(cx, 2)), (1000, 5000));
        assert_eq!(record_count(cx, 1, gold()), 10);
        assert_eq!(record_count(cx, 2, gold()), 0);
        assert_eq!(hold_count(cx, ships[0], gold()), 10);
    };

    // Starting a trade nobody asked for, or with oneself: reason 2 to the
    // source alone.
    common(
        &mut cx,
        2,
        PlayerTrade {
            target_player: 1,
            ..trade(0x01)
        },
    );
    assert_eq!(terminated(&sink.take()), [(2, 2)]);
    common(
        &mut cx,
        1,
        PlayerTrade {
            target_player: 1,
            ..trade(0x01)
        },
    );
    assert_eq!(terminated(&sink.take()), [(1, 2)]);
    // A change without a trade: reason 2.
    money(&mut cx, 1, 10);
    assert_eq!(terminated(&sink.take()), [(1, 2)]);
    // Accepting without a trade: nothing.
    accept(&mut cx, 1, true);
    assert!(sink.take().is_empty());

    // A withdrawn request: the asker hears reason 0, the asked the
    // withdrawal; the packet's target names the session.
    common(
        &mut cx,
        1,
        PlayerTrade {
            target_player: 2,
            ..trade(0x20)
        },
    );
    let _ = sink.take();
    common(
        &mut cx,
        1,
        PlayerTrade {
            target_player: 1,
            ..trade(0x80)
        },
    );
    assert_eq!(
        trades(&sink.take()),
        [
            (
                1,
                PlayerTrade {
                    reason: 0,
                    ..trade(0x02)
                }
            ),
            (
                2,
                PlayerTrade {
                    target_player: ship_ids[0],
                    ..trade(0x80)
                }
            ),
        ]
    );
    assert_eq!(cx.logic.players.get(1).unwrap().trade_request_partner, 0);
    assert!(
        cx.logic
            .players
            .get(2)
            .unwrap()
            .inbound_trade_requests
            .is_empty()
    );

    // A trade one side cancels: the reason to both, nothing moves.
    open_trade(&mut cx, &sink, ship_ids);
    offer_gold(&mut cx, gold_stack, 4);
    money(&mut cx, 2, 500);
    // A request while trading is ignored.
    common(
        &mut cx,
        1,
        PlayerTrade {
            target_player: 2,
            ..trade(0x20)
        },
    );
    let _ = sink.take();
    common(
        &mut cx,
        2,
        PlayerTrade {
            reason: 7,
            ..trade(0x02)
        },
    );
    assert_eq!(terminated(&sink.take()), [(1, 7), (2, 7)]);
    assert!(cx.logic.players.get(1).unwrap().trade_offer.is_none());
    assert!(cx.logic.players.get(2).unwrap().trade_offer.is_none());
    unchanged(&cx);

    // Firing ends the trade with reason 0.
    open_trade(&mut cx, &sink, ship_ids);
    common(
        &mut cx,
        1,
        FireWeapon {
            object_id: ship_ids[0],
            target: FireWeapon::target_from([15000.0, 3000.0, -24000.0]),
            hardpoints: Vec::new(),
        },
    );
    assert_eq!(terminated(&sink.take()), [(2, 0), (1, 0)]);
    assert!(cx.logic.players.get(2).unwrap().trade_offer.is_none());

    // More money than the player has: reason 3 once both accept.
    open_trade(&mut cx, &sink, ship_ids);
    money(&mut cx, 1, 1001);
    accept(&mut cx, 1, true);
    accept(&mut cx, 2, true);
    assert_eq!(terminated(&sink.take()), [(1, 3), (2, 3)]);
    unchanged(&cx);

    // More gold than the player holds: reason 3.
    open_trade(&mut cx, &sink, ship_ids);
    offer_gold(&mut cx, gold_stack, 11);
    accept(&mut cx, 2, true);
    accept(&mut cx, 1, true);
    assert_eq!(terminated(&sink.take()), [(2, 3), (1, 3)]);
    unchanged(&cx);

    // More than the other hold takes: reason 5.
    for item in &mut cx.logic.players.get_mut(1).unwrap().equipment {
        if item.arch_id == gold() {
            item.count = 40;
        }
    }
    open_trade(&mut cx, &sink, ship_ids);
    offer_gold(&mut cx, gold_stack, 40);
    accept(&mut cx, 1, true);
    accept(&mut cx, 2, true);
    assert_eq!(terminated(&sink.take()), [(1, 5), (2, 5)]);
    assert_eq!(record_count(&cx, 1, gold()), 40);
    assert_eq!(record_count(&cx, 2, gold()), 0);
    assert_eq!((cash(&cx, 1), cash(&cx, 2)), (1000, 5000));

    // A jettison ends the trade first, even of an item the ship lacks.
    open_trade(&mut cx, &sink, ship_ids);
    common(
        &mut cx,
        2,
        JettisonCargo {
            object_id: ship_ids[1],
            item_id: 0x999,
            count: 1,
        },
    );
    assert_eq!(terminated(&sink.take()), [(1, 0), (2, 0)]);

    // A destroyed ship ends its trade.
    open_trade(&mut cx, &sink, ship_ids);
    game_logic::objects::base_destroy(&mut cx, ships[1], 1);
    let sent = sink.take();
    assert_eq!(terminated(&sent)[..2], [(1, 0), (2, 0)]);
    assert!(cx.logic.players.get(1).unwrap().trade_offer.is_none());
}

#[test]
fn the_item_requests_change_the_slot_only_in_a_base() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the item requests", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let slot = cx.logic.players.get_mut(1).unwrap();
    slot.client_id = 1;
    slot.selected_character = CharacterId::new(b"p1.fl");
    let gun = create_id(b"li_gun01_mark01");
    let equipment = ReqEquipment {
        records: vec![
            EquipRecord {
                count: 1,
                health: 1.0,
                arch_id: gun,
                id: 0x30,
                equipped: 1,
                field_0f: 9,
                hardpoint: b"HpWeapon01\0".to_vec(),
            },
            EquipRecord {
                count: 7,
                health: 1.0,
                arch_id: gold(),
                id: 0x31,
                equipped: 0,
                field_0f: 0,
                hardpoint: Vec::new(),
            },
        ],
    };
    let groups = ReqCollisionGroups {
        groups: vec![CollisionGroup { id: 3, health: 0.5 }],
    };

    // Outside a base every request is refused.
    client(&mut cx, 1, equipment.clone());
    client(&mut cx, 1, groups.clone());
    client(
        &mut cx,
        1,
        ReqAddItem {
            arch_id: gold(),
            count: 2,
            health: 1.0,
            mounted: 0,
            hardpoint: b"BAY\0".to_vec(),
        },
    );
    assert!(slot_items(&cx, 1).is_empty());
    assert!(cx.logic.players.get(1).unwrap().collision_groups.is_empty());
    assert!(sink.take().is_empty());

    cx.logic.players.get_mut(1).unwrap().entered_base_id = create_id(b"he01_01_Base");
    client(&mut cx, 1, equipment);
    let items = slot_items(&cx, 1);
    assert_eq!(
        items
            .iter()
            .map(|e| (e.arch_id, e.id, e.count, e.equipped))
            .collect::<Vec<_>>(),
        [(gun, 0x30, 1, 1), (gold(), 0x31, 7, 0)]
    );
    assert_eq!(cx.world.strings().get(items[0].hardpoint), b"HpWeapon01");
    assert!(items[1].is_internal(cx.world.strings()));
    assert!(
        sink.take()
            .iter()
            .any(|(c, m)| *c == 1 && matches!(m, ToClient::Server(ServerMessage::SetEquipment(_))))
    );

    // Five more gold join the line, and the client hears it.
    client(
        &mut cx,
        1,
        ReqAddItem {
            arch_id: gold(),
            count: 5,
            health: 1.0,
            mounted: 0,
            hardpoint: b"BAY\0".to_vec(),
        },
    );
    assert_eq!(record_count(&cx, 1, gold()), 12);
    assert!(
        sink.take()
            .iter()
            .any(|(c, m)| *c == 1 && matches!(m, ToClient::Server(ServerMessage::SetAddItem(_))))
    );
    // Three leave it; the client is not told.
    client(
        &mut cx,
        1,
        ReqRemoveItem {
            equip_id: 0x1_0031,
            count: 3,
        },
    );
    assert_eq!(record_count(&cx, 1, gold()), 9);
    assert!(
        !sink
            .take()
            .iter()
            .any(|(_, m)| matches!(m, ToClient::Server(ServerMessage::SetRemoveItem(_))))
    );
    // The gun moves to another hardpoint at half health.
    client(
        &mut cx,
        1,
        ReqModifyItem {
            equip_id: 0x30,
            count: 1,
            health: 0.5,
            mounted: 1,
            hardpoint: b"HpWeapon02\0".to_vec(),
        },
    );
    let gun_line = slot_items(&cx, 1)[0];
    assert_eq!(gun_line.health, 0.5);
    assert_eq!(cx.world.strings().get(gun_line.hardpoint), b"HpWeapon02");
    // The groups are the slot's and go back to the client.
    client(&mut cx, 1, groups);
    let slot = cx.logic.players.get(1).unwrap();
    assert_eq!(
        slot.collision_groups
            .iter()
            .map(|g| (g.id, g.health))
            .collect::<Vec<_>>(),
        [(3, 0.5)]
    );
    assert!(sink.take().iter().any(
        |(c, m)| *c == 1 && matches!(m, ToClient::Server(ServerMessage::SetCollisionGroups(_)))
    ));
}

fn fmt(id: u32) -> FmtStr {
    let mut f = FmtStr::new(id);
    f.append_int(id + 1);
    f
}

#[test]
fn the_kept_state_and_the_questions() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("the player state", false, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    controller::create(
        &mut cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    let _ = heard();
    for session in 1..=2 {
        let slot = cx.logic.players.get_mut(session).unwrap();
        slot.client_id = session;
        slot.rank_level = 7;
    }

    // Visits: ORed into the slot's or added.
    cx.logic.players.get_mut(1).unwrap().visits.insert(10, 0x01);
    let mut block = 2u32.to_le_bytes().to_vec();
    block.extend_from_slice(&10u32.to_le_bytes());
    block.push(0x10);
    block.extend_from_slice(&11u32.to_le_bytes());
    block.push(0x08);
    common(&mut cx, 1, SetVisitedState { data: block });
    let visits = &cx.logic.players.get(1).unwrap().visits;
    assert_eq!(visits.get(&10), Some(&0x11));
    assert_eq!(visits.get(&11), Some(&0x08));

    // The mission log: the strings at the front, the last first; 0 clears.
    let mut block = 1u32.to_le_bytes().to_vec();
    block.extend_from_slice(&2u32.to_le_bytes());
    block.extend_from_slice(&fmt(100).flatten().unwrap());
    block.extend_from_slice(&fmt(200).flatten().unwrap());
    common(&mut cx, 1, SetMissionLog { data: block });
    assert_eq!(
        cx.logic.players.get(1).unwrap().mission_log,
        [fmt(200), fmt(100)]
    );
    common(
        &mut cx,
        1,
        SetMissionLog {
            data: 2u32.to_le_bytes().to_vec(),
        },
    );
    assert_eq!(cx.logic.players.get(1).unwrap().mission_log.len(), 2);
    common(
        &mut cx,
        1,
        SetMissionLog {
            data: 0u32.to_le_bytes().to_vec(),
        },
    );
    assert!(cx.logic.players.get(1).unwrap().mission_log.is_empty());

    // The interface state; a session without a client is refused.
    common(
        &mut cx,
        1,
        SetInterfaceState {
            data: 0x55u32.to_le_bytes().to_vec(),
        },
    );
    common(
        &mut cx,
        3,
        SetInterfaceState {
            data: 0x66u32.to_le_bytes().to_vec(),
        },
    );
    assert_eq!(cx.logic.players.get(1).unwrap().interface_state, 0x55);
    assert_eq!(cx.logic.players.get(3).unwrap().interface_state, 0);

    // The rank is hidden on a multiplayer server.
    let _ = sink.take();
    common(&mut cx, 1, RequestRankLevel { data: Vec::new() });
    assert_eq!(
        sink.take(),
        [(
            1,
            ToClient::Common(CommonMessage::RequestRankLevel(RequestRankLevel {
                data: vec![0xFF; 4]
            }))
        )]
    );

    // The group's other members: player 2 is in a base.
    common(&mut cx, 1, RequestGroupPositions { data: Vec::new() });
    assert_eq!(
        sink.take(),
        [(
            1,
            ToClient::Common(CommonMessage::RequestGroupPositions(
                RequestGroupPositions { data: vec![0; 4] }
            ))
        )]
    );
    let group = cx.logic.groups.create();
    cx.logic
        .groups
        .get_mut(group)
        .unwrap()
        .members
        .extend([1, 2]);
    for session in 1..=2 {
        cx.logic.players.get_mut(session).unwrap().group = Some(group);
    }
    let base = create_id(b"he01_01_Base");
    let slot = cx.logic.players.get_mut(2).unwrap();
    slot.current_base = base;
    slot.system_id = he01();
    slot.rep_vibe = 5;
    common(&mut cx, 1, RequestGroupPositions { data: Vec::new() });
    let mut expected = 1u32.to_le_bytes().to_vec();
    expected.extend_from_slice(&[0; 12]);
    expected.extend_from_slice(&base.to_le_bytes());
    expected.extend_from_slice(&he01().to_le_bytes());
    expected.extend_from_slice(&5u32.to_le_bytes());
    assert_eq!(
        sink.take(),
        [(
            1,
            ToClient::Common(CommonMessage::RequestGroupPositions(
                RequestGroupPositions { data: expected }
            ))
        )]
    );

    // The best path and the pop-up dialog go to the bus.
    let mut block = vec![0u8; 0x34];
    for (at, value) in [(0x00, 1u32), (0x1c, 2), (0x0c, 3), (0x30, 9)] {
        block[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    common(&mut cx, 1, RequestBestPath { data: block });
    common(
        &mut cx,
        1,
        RequestBestPath {
            data: vec![0; 0x20],
        },
    );
    common(
        &mut cx,
        2,
        PopUpDialog {
            field_10: 4,
            ..PopUpDialog::default()
        },
    );
    let bus = heard();
    assert_eq!(
        bus.iter()
            .filter(|m| m.starts_with("RequestBestPath"))
            .collect::<Vec<_>>(),
        ["RequestBestPath 1 [1, 2, 3, 0, 0, 0, 0, 0, 9]"],
        "a short block is not read"
    );
    assert!(bus.contains(&"PopUpDialog 2 4".to_owned()));

    // A trade response goes to the bus with the session.
    let mut writer = MarshalWriter::new();
    writer.put_int(3).unwrap();
    writer.put_int(-1).unwrap();
    writer.put_bool(true).unwrap();
    writer
        .put_good_desc_list(&[GoodDesc {
            good_id: gold(),
            count: 2,
            price: 1.5,
            field_0c: 0.0,
        }])
        .unwrap();
    writer.put_good_desc_list(&[]).unwrap();
    client(
        &mut cx,
        2,
        TradeResponse {
            data: writer.into_bytes(),
        },
    );
    let bus = heard();
    assert!(
        bus.iter().any(|m| m
            == &format!(
                "TradeResponse 3 -1 true [[{}, 2, {}, 0]] [] 2",
                gold(),
                1.5f32.to_bits()
            )),
        "{bus:?}"
    );

    // The difficulty is single player's.
    client(&mut cx, 1, ReqDifficultyScale { value: 1.5 });
    assert_eq!(cx.logic.players.get(1).unwrap().difficulty_scale, 1.0);
}

#[test]
fn single_player_answers_the_rank_and_takes_the_difficulty() {
    let sink = Shared::default();
    let Some((mut world, mut logic)) = setup("single player's state", true, &sink) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let slot = cx.logic.players.get_mut(1).unwrap();
    slot.client_id = 1;
    slot.rank_level = 7;
    common(&mut cx, 1, RequestRankLevel { data: Vec::new() });
    assert_eq!(
        sink.take(),
        [(
            1,
            ToClient::Common(CommonMessage::RequestRankLevel(RequestRankLevel {
                data: 7u32.to_le_bytes().to_vec()
            }))
        )]
    );
    for (value, kept) in [(1.5, 1.5), (3.0, 2.0), (-1.0, 0.0), (f32::NAN, 0.0)] {
        client(&mut cx, 1, ReqDifficultyScale { value });
        assert_eq!(cx.logic.players.get(1).unwrap().difficulty_scale, kept);
    }
    // In single player the item requests need no base.
    client(
        &mut cx,
        1,
        ReqAddItem {
            arch_id: gold(),
            count: 2,
            health: 1.0,
            mounted: 0,
            hardpoint: b"BAY\0".to_vec(),
        },
    );
    assert_eq!(record_count(&cx, 1, gold()), 2);
}
