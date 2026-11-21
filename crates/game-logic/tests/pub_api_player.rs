//! P3.3's `pub::Player` and the top-level `pub::` getters over the
//! original data (`outdir/`): the getters' answers and result codes for
//! he01's bases, rooms, goods, ships and loadouts; a real character
//! (`newplayer.fl`, applied to a slot) through `pub::Player`: its money,
//! cargo, rank, base, system and ship, docked at he01_01_Base and in space
//! in he01, with the messages its client hears; the camera, mission and
//! group functions. The expected values follow the C++ logic of
//! `Server/server_pub.cpp`, `server_classes.cpp` and Common, and rpclocal's
//! marshalling.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::player::{self as pp, CargoLine, MissionObjective, RELLAN_COSTUME};
use game_logic::api::{PubError, misc};
use game_logic::character_file::{SaveData, SavedCharacter, load_save_record};
use game_logic::events::ControllerMessage;
use game_logic::server::{ClientSink, Cx, Logic};
use game_logic::{group, player, world};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::costume::Costume;
use game_sim::data::equip_desc::EquipDesc;
use game_sim::equip::EquipKind;
use game_sim::object::world::{Assets, ShipSpawn, WorldConfig};
use game_sim::object::{ObjHandle, World};
use game_sim::protocol::ToClient;
use game_sim::protocol::common::CommonMessage;
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::{create_id, make_location_id};
use neuron_core::math::{Matrix, Transform, Vector, matrix_to_quaternion};
use neuron_core::vfs::Host;
use neuron_server::save::read_save;

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

    fn server(&self) -> Vec<(u32, ServerMessage)> {
        self.take()
            .into_iter()
            .filter_map(|(c, m)| match m {
                ToClient::Server(s) => Some((c, s)),
                ToClient::Common(_) => None,
            })
            .collect()
    }
}

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// The data and `newplayer.fl`'s record.
fn load() -> Option<(Arc<GameData>, SavedCharacter)> {
    let config = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let bytes = std::fs::read(outdir().join("newplayer.fl")).ok()?;
    let text = read_save(&bytes).text()?.to_vec();
    let mut record = SavedCharacter::default();
    let mut reports = game_sim::data::Reports::default();
    let save = SaveData {
        universe: &data.universe,
        archetypes: &data.archetypes,
        reputation: &data.reputation,
        costumes: &data.costumes,
    };
    assert!(load_save_record(
        &text,
        "newplayer.fl",
        &mut record,
        save,
        &mut data.strings,
        &mut reports,
    ));
    Some((Arc::new(data), record))
}

fn new_world(data: Arc<GameData>, single_player: bool) -> World {
    let config = WorldConfig {
        single_player,
        ..WorldConfig::default()
    };
    World::new(data, config).with_assets(Assets::new(outdir().join("data")))
}

fn he01() -> u32 {
    create_id(b"he01")
}

fn he01_base() -> u32 {
    create_id(b"he01_01_Base")
}

fn reports(cx: &mut Cx<'_>) -> Vec<String> {
    std::mem::take(&mut cx.logic.reports.0)
        .into_iter()
        .map(|r| r.text)
        .collect()
}

#[test]
fn pub_player_and_the_getters_over_the_data() {
    let Some((data, record)) = load() else {
        println!("outdir/ is missing: no pub::Player");
        return;
    };
    getters(Arc::clone(&data));
    docked(Arc::clone(&data), &record);
    in_space(Arc::clone(&data), &record);
    messages_and_groups(data, &record);
}

fn getters(data: Arc<GameData>) {
    let mut world = new_world(Arc::clone(&data), false);
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 4, false);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let gold = create_id(b"commodity_gold");
    let hull = create_id(b"gf1_hull");
    let fighter = create_id(b"ge_fighter");
    let unknown = create_id(b"no_such_thing");

    // Goods.
    assert_eq!(misc::is_commodity(&cx, gold), Ok(true));
    assert_eq!(misc::is_commodity(&cx, hull), Ok(false));
    assert_eq!(misc::is_commodity(&cx, unknown), Err(PubError(-4)));
    assert_eq!(misc::good_id(&cx, b"commodity_gold"), Ok(gold));
    assert_eq!(misc::good_id(&cx, b"no_such_thing"), Err(PubError(-2)));
    assert_eq!(misc::good_id(&cx, b""), Err(PubError(-4)));
    // The gold good's equipment is the archetype of the same nickname; a
    // ship archetype is no good's.
    assert_eq!(misc::translate_arch_to_good(&cx, gold), Ok(gold));
    assert_eq!(
        misc::translate_arch_to_good(&cx, fighter),
        Err(PubError(-2))
    );
    assert_eq!(misc::translate_arch_to_good(&cx, 0), Err(PubError(-4)));
    let (volume, mass) = misc::good_properties(&cx, gold).unwrap();
    assert_eq!(volume, 1.0);
    assert_eq!(mass, data.archetypes.equipment[&gold].root.mass);
    assert_eq!(misc::good_properties(&cx, 0), Err(PubError(-4)));
    assert_eq!(misc::good_properties(&cx, unknown), Err(PubError(-2)));
    assert_eq!(
        misc::translate_good_to_msg_id_prefix(&cx, gold),
        Ok(b"gcs_gen_commodity_gold".to_vec())
    );
    assert_eq!(
        misc::translate_good_to_msg_id_prefix(&cx, unknown),
        Err(PubError(-4))
    );

    // Names to ids.
    assert_eq!(
        misc::equipment_id(b"commodity_gold"),
        Ok(create_id(b"commodity_gold"))
    );
    assert_eq!(misc::equipment_id(b""), Err(PubError(-4)));
    assert_eq!(misc::ship_id(b"ge_fighter"), Ok(fighter));
    assert_eq!(misc::ship_id(b"\0"), Err(PubError(-4)));
    assert_eq!(
        misc::voice_id(b"rellan_voice"),
        Ok(create_id(b"rellan_voice"))
    );
    assert_eq!(misc::voice_id(b""), Err(PubError(-4)));
    assert_eq!(misc::costume_id(&cx, b"rellan"), Ok(RELLAN_COSTUME as i32));
    assert_eq!(misc::costume_id(&cx, b"no_such_thing"), Err(PubError(-2)));
    assert_eq!(misc::costume_id(&cx, b""), Err(PubError(-4)));

    // Ship archetypes: `ge_fighter` (hold 20, 1100 hit points, name 237015).
    assert_eq!(misc::cargo_hold_size(&cx, fighter), Ok(20));
    assert_eq!(misc::cargo_hold_size(&cx, 0x4000_0000), Err(PubError(-4)));
    assert_eq!(misc::cargo_hold_size(&cx, unknown), Err(PubError(-2)));
    assert_eq!(misc::full_health(&cx, fighter), Ok(1100));
    assert_eq!(misc::full_health(&cx, unknown), Err(PubError(-2)));
    assert_eq!(misc::ship_arch_strid(&cx, fighter), Ok(237_015));
    assert_eq!(misc::ship_arch_strid(&cx, unknown), Err(PubError(-2)));
    assert_eq!(misc::ship_arch_strid(&cx, 0), Err(PubError(-4)));
    assert_eq!(
        misc::ship_type(&cx, fighter),
        Ok(data.archetypes.ships[&fighter].eq_obj.root.object_type)
    );
    assert_eq!(misc::ship_type(&cx, unknown), Err(PubError(-2)));
    assert_eq!(
        misc::translate_ship_to_msg_id_prefix(&cx, fighter),
        Ok(b"gcs_refer_shiparch_starflier".to_vec())
    );
    assert_eq!(
        misc::translate_ship_to_msg_id_prefix(&cx, unknown),
        Err(PubError(-4))
    );
    // Solar archetypes.
    let (&solar, arch) = data.archetypes.solars.iter().next().expect("a solar");
    assert_eq!(
        misc::solar_type(&cx, solar),
        Ok(arch.eq_obj.root.object_type)
    );
    assert_eq!(
        misc::max_hit_points(&cx, solar),
        Ok(arch.eq_obj.root.hit_pts as i64 as i32)
    );
    assert_eq!(misc::solar_type(&cx, fighter), Err(PubError(-2)));
    assert_eq!(misc::max_hit_points(&cx, 0), Err(PubError(-4)));

    // Hardpoints of the fighter's model.
    let hp = misc::find_hardpoint(&mut cx, b"HpWeapon01", fighter).expect("HpWeapon01");
    assert_eq!(
        Some(hp),
        cx.world.ship_archetype_hardpoint(fighter, b"HpWeapon01")
    );
    assert_eq!(
        misc::find_hardpoint(&mut cx, b"HpNoSuchPoint", fighter),
        Err(PubError(-3))
    );
    assert_eq!(
        misc::find_hardpoint(&mut cx, b"HpWeapon01", unknown),
        Err(PubError(-2))
    );

    // Loadouts.
    let loadout = create_id(b"li_p_he_fighter_loadout01");
    assert_eq!(
        misc::loadout(&cx, loadout),
        Ok(data.loadouts.get(loadout).unwrap().items.clone())
    );
    assert_eq!(misc::loadout(&cx, 0), Err(PubError(-4)));
    assert_eq!(misc::loadout(&cx, unknown), Err(PubError(-2)));
    assert_eq!(misc::loadout_id(b"li_p_he_fighter_loadout01"), Ok(loadout));
    assert_eq!(misc::loadout_id(b""), Err(PubError(-4)));
    // The buffer's first byte decides; the name is the id in hex.
    let mut buffer = [b'?'; 16];
    assert_eq!(misc::loadout_name(&cx, loadout, &mut buffer), Ok(()));
    let hex = format!("0x{loadout:x}");
    assert_eq!(&buffer[..hex.len()], hex.as_bytes());
    assert!(buffer[hex.len()..].iter().all(|&b| b == 0));
    let mut empty = [0u8; 16];
    assert_eq!(
        misc::loadout_name(&cx, loadout, &mut empty),
        Err(PubError(-2))
    );
    let mut short = [b'?'; 4];
    assert_eq!(misc::loadout_name(&cx, loadout, &mut short), Ok(()));
    assert_eq!(&short, &hex.as_bytes()[..4]);
    assert_eq!(
        misc::loadout_name(&cx, unknown, &mut buffer),
        Err(PubError(-2))
    );

    // The game time.
    cx.logic.clock.base = 120.0;
    cx.logic.clock.elapsed = 60.0;
    assert_eq!(misc::rw_time(&cx), 180.0);
    // `[Time] seconds_per_day = 1800`.
    assert_eq!(misc::time(&cx), 180.0 * (1.0 / 1800.0));

    // Bases.
    reports(&mut cx);
    let base = he01_base();
    assert_eq!(misc::base_id(&mut cx, b"he01_01_Base"), Ok(base));
    assert_eq!(misc::base_id(&mut cx, b""), Err(PubError(-4)));
    assert_eq!(misc::base_id(&mut cx, b"li01_01_base"), Err(PubError(-4)));
    assert_eq!(
        reports(&mut cx),
        ["*** WARNING: get_base(li01_01_base) failed."]
    );
    assert_eq!(misc::base_system(&mut cx, base), Ok(he01()));
    assert_eq!(misc::base_system(&mut cx, unknown), Err(PubError(-2)));
    assert_eq!(
        reports(&mut cx),
        [format!("*** WARNING: get_base(0X{unknown:X}) failed.")]
    );
    assert_eq!(misc::base_strid_name(&mut cx, base), Ok(196_766));
    assert_eq!(
        misc::base_nickname(&mut cx, 64, base),
        (0, Some(b"he01_01_Base".to_vec()))
    );
    assert_eq!(misc::base_nickname(&mut cx, 12, base), (13, None));
    assert_eq!(misc::base_nickname(&mut cx, 64, 0), (0, None));
    let source = Host::new(outdir().join("data"));
    let mut reader = misc::build_base_reader(&mut cx, &source, base).expect("the base file");
    assert!(reader.read_header() && reader.is_header(b"BaseInfo"));
    assert!(misc::build_base_reader(&mut cx, &source, unknown).is_err());
    reports(&mut cx);

    // The bases of he01 in id order, and the walk over every base.
    let mut expected: Vec<u32> = data
        .universe
        .bases
        .iter()
        .filter(|(_, b)| b.system == he01())
        .map(|(&id, _)| id)
        .collect();
    assert!(expected.contains(&base));
    assert_eq!(misc::bases(&cx, he01(), 1000), expected);
    expected.truncate(2);
    assert_eq!(misc::bases(&cx, he01(), 2), expected);
    let mut id = 0;
    let mut walked = Vec::new();
    while misc::next_base_id(&cx, &mut id) {
        walked.push(id);
    }
    assert_eq!(
        walked,
        data.universe.bases.keys().copied().collect::<Vec<_>>()
    );
    let mut unknown_id = unknown;
    assert!(!misc::next_base_id(&cx, &mut unknown_id));
    assert_eq!(unknown_id, unknown);

    // The rooms: he01_01_Base has Cityscape, Bar, Trader, ShipDealer and
    // Equipment, found in GetLocations' order; Deck and Planetside are
    // reported.
    let room = |name: &[u8]| make_location_id(base, name);
    assert_eq!(
        misc::locations(&mut cx, base, 7),
        [
            room(b"Bar"),
            room(b"Trader"),
            room(b"Equipment"),
            room(b"Cityscape"),
            room(b"ShipDealer")
        ]
    );
    assert_eq!(
        reports(&mut cx),
        [
            format!("*** ERROR: Could not find room {}", room(b"Deck")),
            format!("*** ERROR: Could not find room {}", room(b"Planetside")),
        ]
    );
    assert_eq!(
        misc::locations(&mut cx, base, 2),
        [room(b"Bar"), room(b"Trader")]
    );
    assert_eq!(reports(&mut cx).len(), 2);
    assert_eq!(misc::locations(&mut cx, unknown, 7), Vec::<u32>::new());
    assert_eq!(
        reports(&mut cx),
        [format!("*** ERROR: Could not find base {unknown}")]
    );
    assert_eq!(
        misc::location_nickname(&mut cx, 16, room(b"Bar")),
        (0, Some(b"Bar".to_vec()))
    );
    assert_eq!(misc::location_nickname(&mut cx, 3, room(b"Bar")), (4, None));
    assert_eq!(
        misc::location_nickname(&mut cx, 16, room(b"Deck")),
        (0, None)
    );
    assert_eq!(reports(&mut cx).len(), 1);

    // `DebugPrint`.
    misc::debug_print(&mut cx, b"one\0", 1);
    misc::debug_print(&mut cx, b"two", 2);
    misc::debug_print(&mut cx, b"three", 3);
    assert_eq!(
        reports(&mut cx),
        ["*** WARNING: pub: one", "*** ERROR: pub: two"]
    );

    // `ReportFreeTerminal` answers a launch's request through its message.
    let mut request = ControllerMessage::FreeTerminalRequest {
        player: 1,
        target: 2,
        reported: -1,
    };
    assert_eq!(misc::report_free_terminal(&mut request, 99, 3), 0);
    assert!(matches!(
        request,
        ControllerMessage::FreeTerminalRequest { reported: 3, .. }
    ));
    assert!(sink.take().is_empty());
}

/// A logic with the character in slot 1 (client 1) and an empty slot 2.
fn character_logic(
    world: &mut World,
    sink: &Shared,
    record: &SavedCharacter,
    multiplayer: bool,
) -> Logic {
    let mut logic = Logic::new(Box::new(sink.clone()), 4, multiplayer);
    logic.deliver_hook = Some(|_, _, _, _| {});
    logic.players.get_mut(1).unwrap().client_id = 1;
    let mut cx = Cx {
        world,
        logic: &mut logic,
    };
    player::apply_saved_character(&mut cx, 1, record, 1);
    logic
}

fn docked(data: Arc<GameData>, record: &SavedCharacter) {
    let mut world = new_world(Arc::clone(&data), true);
    let sink = Shared::default();
    let mut logic = character_logic(&mut world, &sink, record, false);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let fighter = create_id(b"ge_fighter");

    // The character as `newplayer.fl` has it.
    assert_eq!(pp::inspect_cash(&cx, 1), Ok(500));
    assert_eq!(pp::rank(&cx, 1), Ok(0));
    assert_eq!(pp::ship_id(&cx, 1), Ok(fighter));
    assert_eq!(pp::system(&cx, 1), Ok(create_id(b"relayseven_system")));
    assert_eq!(pp::base(&cx, 1), Ok(0));
    assert_eq!(pp::ship(&cx, 1), Ok(0));
    assert_eq!(pp::relative_health(&cx, 1), Ok(record.hull_status));
    let vibe = cx.logic.players.get(1).unwrap().rep_vibe;
    assert_eq!(pp::rep(&cx, 1), Ok(vibe));
    assert_eq!(
        pp::name(&mut cx, 1),
        Some("Rellan".encode_utf16().collect())
    );
    assert_eq!(pp::name(&mut cx, 2), None);

    // It lands at he01_01_Base in he01.
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.current_base = he01_base();
        slot.system_id = he01();
        slot.current_location = make_location_id(he01_base(), b"Bar");
        slot.field_3fc = 7;
    }
    assert_eq!(pp::base(&cx, 1), Ok(he01_base()));
    assert_eq!(pp::system(&cx, 1), Ok(he01()));
    assert_eq!(
        pp::location(&cx, 1),
        Ok(make_location_id(he01_base(), b"Bar"))
    );
    assert_eq!(pp::character(&cx, 1), Ok(7));

    // An invalid player: id 0, an empty slot, past the slots.
    for id in [0, 2, 5] {
        assert_eq!(pp::system(&cx, id), Err(PubError(-2)));
        assert_eq!(pp::inspect_cash(&cx, id), Err(PubError(-2)));
        assert_eq!(pp::num_kills(&cx, id), Err(PubError(-2)));
        assert_eq!(pp::adjust_cash(&mut cx, id, 5), Err(PubError(-2)));
        assert_eq!(pp::enumerate_cargo(&cx, id), Err(PubError(-2)));
        assert_eq!(pp::msn_id(&cx, id), None);
        assert_eq!(pp::send_nn_message(&mut cx, id, 9), Err(PubError(-4)));
    }
    assert!(sink.take().is_empty());

    // Money: the earned total grows, the cash is clamped, the client hears.
    pp::adjust_cash(&mut cx, 1, 1000).unwrap();
    assert_eq!(pp::inspect_cash(&cx, 1), Ok(1500));
    pp::adjust_cash(&mut cx, 1, -2000).unwrap();
    assert_eq!(pp::inspect_cash(&cx, 1), Ok(0));
    let p = cx.logic.players.get(1).unwrap();
    assert_eq!((p.cash, p.total_earned), (0, -1000));
    let cash: Vec<(u32, u32)> = sink
        .server()
        .into_iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::SetCash(s) => Some((c, s.value)),
            _ => None,
        })
        .collect();
    assert_eq!(cash, [(1, 1500), (1, 0)]);
    // A stored cash out of range is clamped on the way out only.
    cx.logic.players.get_mut(1).unwrap().cash = 2_000_000_000;
    assert_eq!(pp::inspect_cash(&cx, 1), Ok(999_999_999));
    cx.logic.players.get_mut(1).unwrap().cash = 500;

    // The rank and the money it needs.
    pp::set_rank(&mut cx, 1, 7).unwrap();
    assert_eq!(pp::rank(&cx, 1), Ok(7));
    let sent = sink.server();
    assert!(matches!(
        sent.as_slice(),
        [(1, ServerMessage::MiscObjUpdate(m))] if m.field_24 == 1 && m.field_2c == 7
    ));
    pp::set_money_needed_to_next_rank(&mut cx, 1, 25_000).unwrap();
    assert_eq!(pp::money_needed_to_next_rank(&cx, 1), Ok(25_000));

    // The counters: stored, sent to the slot's client.
    pp::set_num_kills(&mut cx, 1, 3).unwrap();
    pp::set_num_mission_successes(&mut cx, 1, 4).unwrap();
    pp::set_num_mission_failures(&mut cx, 1, 5).unwrap();
    assert_eq!(pp::num_kills(&cx, 1), Ok(3));
    assert_eq!(pp::num_mission_successes(&cx, 1), Ok(4));
    assert_eq!(pp::num_mission_failures(&cx, 1), Ok(5));
    let names: Vec<&str> = sink
        .server()
        .iter()
        .map(|(c, m)| {
            assert_eq!(*c, 1);
            match m {
                ServerMessage::SetNumKills(v) if v.value == 3 => "kills",
                ServerMessage::SetMissionSuccesses(v) if v.value == 4 => "successes",
                ServerMessage::SetMissionFailures(v) if v.value == 5 => "failures",
                _ => "other",
            }
        })
        .collect();
    assert_eq!(names, ["kills", "successes", "failures"]);

    // The cargo: the items not mounted, with their goods.
    let goods = &data.goods;
    let expected: Vec<CargoLine> = record
        .equipment
        .iter()
        .filter(|e| e.equipped == 0)
        .map(|e| CargoLine {
            good: goods.arch_to_good(e.arch_id),
            count: e.count,
            id: e.id,
        })
        .collect();
    assert_eq!(expected.len(), 3);
    assert_eq!(pp::enumerate_cargo(&cx, 1), Ok(expected));

    // Docked, a good joins the record's cargo bay and the client hears.
    let gold = create_id(b"commodity_gold");
    assert_eq!(
        pp::add_cargo(&mut cx, 1, gold, 5, 1.5, false),
        Err(PubError(-4))
    );
    assert_eq!(
        pp::add_cargo(&mut cx, 1, gold, 5, -0.5, false),
        Err(PubError(-4))
    );
    assert_eq!(
        pp::add_cargo(&mut cx, 1, gold, 5, f32::NAN, false),
        Err(PubError(-4))
    );
    assert_eq!(
        pp::add_cargo(&mut cx, 2, gold, 5, 1.0, false),
        Err(PubError(-2))
    );
    pp::add_cargo(&mut cx, 1, gold, 5, 1.0, false).unwrap();
    pp::add_cargo(&mut cx, 1, gold, 3, 1.0, false).unwrap();
    let lines = pp::enumerate_cargo(&cx, 1).unwrap();
    let gold_line = *lines.iter().find(|l| l.good == gold).expect("gold");
    assert_eq!(gold_line.count, 8);
    assert_eq!(lines.len(), 4);
    let adds: Vec<(u32, u32, i32)> = sink
        .server()
        .into_iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::SetAddItem(a) => Some((c, a.arch_id, a.count)),
            _ => None,
        })
        .collect();
    assert_eq!(adds, [(1, gold, 5), (1, gold, 3)]);
    // A mission good stays apart.
    pp::add_cargo(&mut cx, 1, gold, 2, 1.0, true).unwrap();
    assert_eq!(pp::enumerate_cargo(&cx, 1).unwrap().len(), 5);
    sink.take();
    // The hold: 20 less the cargo's volume (gold's 1 each, 10 units, and
    // the template's cargo).
    let strings = cx.world.strings();
    let mut occupied = 0.0f32;
    for item in &cx.logic.players.get(1).unwrap().equipment {
        let volume = data
            .archetypes
            .equipment
            .get(&item.arch_id)
            .map(|a| a.volume);
        occupied = (item.cargo_space_occupied(strings, volume) + f64::from(occupied)) as f32;
    }
    assert!(occupied >= 10.0);
    assert_eq!(
        pp::remaining_hold_size(&cx, 1),
        Ok((20.0 - f64::from(occupied)) as f32)
    );
    // Units leave, the client hears.
    pp::remove_cargo(&mut cx, 1, gold_line.id, 6).unwrap();
    let after = pp::enumerate_cargo(&cx, 1).unwrap();
    assert_eq!(
        after.iter().find(|l| l.id == gold_line.id).map(|l| l.count),
        Some(2)
    );
    let removes: Vec<(u32, u32, u32, u32)> = sink
        .server()
        .into_iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::SetRemoveItem(r) => Some((c, r.field_00, r.field_04, r.field_08)),
            _ => None,
        })
        .collect();
    assert_eq!(removes, [(1, u32::from(gold_line.id), 6, 1)]);

    // The asset value: 0.8 of the hull good's price (gf1_hull, 6000), then
    // each item: combinable goods at full price, others at 0.3 of
    // ftol(health * price) per unit.
    let equipment = cx.logic.players.get(1).unwrap().equipment.clone();
    let mut value = (f64::from(0.8f32) * 6000.0) as f32;
    for item in &equipment {
        let Some(good) = goods.find_by_archetype(item.arch_id) else {
            continue;
        };
        let term = if good.combinable {
            f64::from(item.count) * f64::from(good.price)
        } else {
            let unit = (f64::from(item.health) * f64::from(good.price)) as i64 as i32;
            f64::from(unit * item.count) * f64::from(0.3f32)
        };
        value = (f64::from(value) + term) as f32;
    }
    assert_eq!(pp::asset_value(&cx, 1), Ok(value));
    // The first gold line alone: 4800 for the hull and 425 per unit (the
    // docked "mission" line is not marked: `PlayerData_AddEquipment` only
    // keeps it apart).
    let only_gold: Vec<EquipDesc> = equipment
        .iter()
        .filter(|e| e.id == gold_line.id)
        .copied()
        .collect();
    assert_eq!(
        equipment
            .iter()
            .filter(|e| e.arch_id == gold && !e.mission)
            .count(),
        2
    );
    cx.logic.players.get_mut(1).unwrap().equipment = only_gold;
    assert_eq!(pp::asset_value(&cx, 1), Ok(4800.0 + 2.0 * 425.0));
    cx.logic.players.get_mut(1).unwrap().ship_archetype = 0;
    assert_eq!(pp::asset_value(&cx, 1), Err(PubError(-2)));

    // A new ship and loadout: every item is numbered afresh.
    let loadout = data
        .loadouts
        .get(create_id(b"li_p_he_fighter_loadout01"))
        .unwrap()
        .items
        .clone();
    let he_fighter = create_id(b"he_fighter");
    pp::set_ship_and_loadout(&mut cx, 1, he_fighter, &loadout).unwrap();
    assert_eq!(pp::ship_id(&cx, 1), Ok(he_fighter));
    let p = cx.logic.players.get(1).unwrap();
    assert_eq!(p.equipment.len(), loadout.len());
    for (have, given) in p.equipment.iter().zip(&loadout) {
        assert_eq!(have.arch_id, given.arch_id);
        assert_ne!(have.id, game_sim::data::equip_desc::SUB_OBJECT_ID_NONE);
    }
    let sent = sink.server();
    assert!(
        matches!(sent.first(), Some((1, ServerMessage::SetShipArch(s))) if s.value == he_fighter)
    );
    assert!(matches!(
        sent.last(),
        Some((1, ServerMessage::SetEquipment(_)))
    ));

    // The start position and orientation.
    pp::set_initial_pos(&mut cx, 1, Vector::new(1.0, 2.0, 3.0)).unwrap();
    let orient = Matrix {
        m: [[0.0, 1.0, 0.0], [-1.0, 0.0, 0.0], [0.0, 0.0, 1.0]],
    };
    pp::set_initial_orient(&mut cx, 1, &orient).unwrap();
    let p = cx.logic.players.get(1).unwrap();
    assert_eq!(p.initial_pos, [1.0, 2.0, 3.0]);
    assert_eq!(p.initial_orient, orient.m);

    // Costumes: nothing is sent.
    pp::set_monkey(&mut cx, 1).unwrap();
    assert_eq!(
        cx.logic.players.get(1).unwrap().costume.part_ids[0],
        0x88db_0044
    );
    pp::set_robot(&mut cx, 1).unwrap();
    assert_eq!(
        cx.logic.players.get(1).unwrap().costume.part_ids,
        [0, 0x8dff_c147, 0x9dc4_c883, 0x9358_2d4f]
    );
    pp::set_rellan(&mut cx, 1).unwrap();
    let rellan: Costume = data
        .costumes
        .get_costume(RELLAN_COSTUME, &mut game_sim::data::Reports::default());
    assert_ne!(rellan, Costume::default());
    assert_eq!(cx.logic.players.get(1).unwrap().costume, rellan);
    pp::set_costume(&mut cx, 1, 5);
    assert_eq!(pp::gender(&cx, 9), 1);
    assert_eq!(pp::body(&cx, 9), 0);
    assert_eq!(pp::set_monkey(&mut cx, 2), Err(PubError(-2)));
    assert!(sink.take().is_empty());

    // A multiplayer server: no cash change and no name before a character
    // is selected.
    let mut mp_world = new_world(data, false);
    let mp_sink = Shared::default();
    let mut mp_logic = character_logic(&mut mp_world, &mp_sink, record, true);
    let mut cx = Cx {
        world: &mut mp_world,
        logic: &mut mp_logic,
    };
    pp::adjust_cash(&mut cx, 1, 100).unwrap();
    let p = cx.logic.players.get(1).unwrap();
    assert_eq!((p.cash, p.total_earned), (500, 100));
    assert!(mp_sink.take().is_empty());
    assert_eq!(
        pp::name(&mut cx, 1),
        Some("<bad name>".encode_utf16().collect())
    );
}

/// The cargo update packets: recipient, object, equipment, count, mission.
fn cargo_updates(sent: &[(u32, ServerMessage)]) -> Vec<(u32, u32, u16, u16, u8)> {
    sent.iter()
        .filter_map(|(c, m)| match m {
            ServerMessage::ObjectCargoUpdate(u) => {
                Some((*c, u.object_id, u.equip_id, u.count, u.mission))
            }
            _ => None,
        })
        .collect()
}

/// The units of the ship's stack with the id.
fn stack_count(cx: &Cx<'_>, h: ObjHandle, id: u16) -> Option<u32> {
    let eq = cx.world.get(h)?.equipped.as_deref()?;
    match &eq.equipment.by_id(id)?.kind {
        EquipKind::Cargo(c) => Some(c.count),
        _ => None,
    }
}

fn in_space(data: Arc<GameData>, record: &SavedCharacter) {
    let mut world = new_world(Arc::clone(&data), false);
    let sink = Shared::default();
    let mut logic = character_logic(&mut world, &sink, record, false);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    // Players 1 and 2 in he01; player 1 flies a fighter there.
    cx.logic.players.get_mut(1).unwrap().system_id = he01();
    {
        let slot = cx.logic.players.get_mut(2).unwrap();
        slot.client_id = 2;
        slot.system_id = he01();
    }
    cx.world.load_system(he01());
    // The ship carries the record's items, as a player's ship does.
    let mut spawn = ShipSpawn::new(
        create_id(b"ge_fighter"),
        0,
        Vector::new(15000.0, 3000.0, -25000.0),
        Matrix::IDENTITY,
    );
    spawn.owner_player = 1;
    spawn.equipment = cx.logic.players.get(1).unwrap().equipment.clone();
    let h = cx
        .world
        .spawn_ship(he01(), &spawn)
        .expect("ge_fighter spawns");
    let ship_id = cx.world.get(h).unwrap().simple.id;
    cx.logic.players.get_mut(1).unwrap().ship_object_id = ship_id;
    world::add_player(&mut cx, he01(), 1);
    world::add_player(&mut cx, he01(), 2);
    // Player 2's client has not been told of the ship.
    for view in &mut cx
        .logic
        .server_world
        .systems
        .get_mut(&he01())
        .unwrap()
        .observers
    {
        for t in &mut view.tracked {
            if t.object == h && view.player == 2 {
                t.created_on_client = false;
            }
        }
    }
    sink.take();
    assert_eq!(pp::ship(&cx, 1), Ok(ship_id));

    // A new stack: the record numbers it, the ship holds it, the players
    // that see the ship hear of it; no SETADDITEM.
    let gold = create_id(b"commodity_gold");
    let before = cx.logic.players.get(1).unwrap().equipment.len();
    pp::add_cargo(&mut cx, 1, gold, 5, 1.0, false).unwrap();
    let p = cx.logic.players.get(1).unwrap();
    assert_eq!(p.equipment.len(), before + 1);
    let line = *p.equipment.last().unwrap();
    assert_eq!((line.arch_id, line.count), (gold, 5));
    assert_eq!(stack_count(&cx, h, line.id), Some(5));
    let sent = sink.server();
    assert!(
        !sent
            .iter()
            .any(|(_, m)| matches!(m, ServerMessage::SetAddItem(_)))
    );
    assert_eq!(cargo_updates(&sent), [(1, ship_id, line.id, 5, 0)]);

    // More gold joins the stack and the record's line.
    pp::add_cargo(&mut cx, 1, gold, 3, 1.0, false).unwrap();
    assert_eq!(stack_count(&cx, h, line.id), Some(8));
    let p = cx.logic.players.get(1).unwrap();
    assert_eq!(p.equipment.len(), before + 1);
    assert_eq!(p.equipment.last().unwrap().count, 8);
    assert_eq!(cargo_updates(&sink.server()), [(1, ship_id, line.id, 8, 0)]);

    // Mission gold is a stack of its own, marked.
    pp::add_cargo(&mut cx, 1, gold, 2, 1.0, true).unwrap();
    let mission = *cx.logic.players.get(1).unwrap().equipment.last().unwrap();
    assert_ne!(mission.id, line.id);
    assert_eq!(stack_count(&cx, h, mission.id), Some(2));
    assert_eq!(
        cargo_updates(&sink.server()),
        [(1, ship_id, mission.id, 2, 1)]
    );

    // Units leave the stack and the record; the rest travels.
    pp::remove_cargo(&mut cx, 1, line.id, 6).unwrap();
    assert_eq!(stack_count(&cx, h, line.id), Some(2));
    let p = cx.logic.players.get(1).unwrap();
    let left = p.equipment.iter().find(|e| e.id == line.id).unwrap();
    assert_eq!(left.count, 2);
    let sent = sink.server();
    assert!(
        !sent
            .iter()
            .any(|(_, m)| matches!(m, ServerMessage::SetRemoveItem(_)))
    );
    assert_eq!(cargo_updates(&sent), [(1, ship_id, line.id, 2, 0)]);
    // An item that is not a stack: nothing.
    let mounted = cx
        .world
        .get(h)
        .unwrap()
        .equipped
        .as_deref()
        .unwrap()
        .equipment
        .iter()
        .find(|e| !matches!(e.kind, EquipKind::Cargo(_)))
        .unwrap()
        .id;
    pp::remove_cargo(&mut cx, 1, mounted, 1).unwrap();
    assert!(cargo_updates(&sink.server()).is_empty());

    // A ship the world does not have answers -2.
    cx.logic.players.get_mut(1).unwrap().ship_object_id = 0x3fff_0000;
    assert_eq!(
        pp::add_cargo(&mut cx, 1, gold, 1, 1.0, false),
        Err(PubError(-2))
    );
    assert_eq!(pp::remove_cargo(&mut cx, 1, line.id, 1), Err(PubError(-2)));
    cx.logic.players.get_mut(1).unwrap().ship_object_id = ship_id;

    // Marking the ship for player 2: recorded, and MARKOBJ to player 2.
    pp::mark_obj(&mut cx, 2, ship_id, 1).unwrap();
    assert!(cx.logic.objects.get(h).unwrap().is_marked_by(2));
    let sent = sink.server();
    assert!(matches!(
        sent.as_slice(),
        [(2, ServerMessage::MarkObj(m))] if m.object_id == ship_id && m.mark_type == 1
    ));
    pp::mark_obj(&mut cx, 2, ship_id, 0).unwrap();
    assert!(!cx.logic.objects.get(h).unwrap().is_marked_by(2));
    assert_eq!(pp::mark_obj(&mut cx, 3, ship_id, 1), Err(PubError(-2)));
    assert_eq!(pp::mark_obj(&mut cx, 2, 0x3fff_0000, 1), Err(PubError(-3)));
    sink.take();

    // A forced landing at a base of the ship's own system queues it.
    pp::force_land(&mut cx, 1, he01_base());
    let queue = &cx.logic.server_world.system(he01()).unwrap().dock_queue;
    assert!(
        queue
            .iter()
            .any(|e| e.object == Some(h) && e.base_id == he01_base() && e.land)
    );
}

fn messages_and_groups(data: Arc<GameData>, record: &SavedCharacter) {
    let mut world = new_world(data, true);
    let sink = Shared::default();
    let mut logic = character_logic(&mut world, &sink, record, false);
    for session in [2, 3] {
        logic.players.get_mut(session).unwrap().client_id = session;
    }
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let text = FmtStr::new(1553);
    let flat = text.flatten().unwrap();

    // The interface notification and the story cue.
    pp::cfg_interface_notification(&mut cx, 1, 4, true, -1).unwrap();
    pp::set_story_cue(&mut cx, 1, 77).unwrap();
    assert_eq!(pp::set_story_cue(&mut cx, 4, 77), Err(PubError(-2)));
    let sent = sink.server();
    assert!(matches!(
        sent.as_slice(),
        [
            (1, ServerMessage::CfgInterfaceNotification(n)),
            (1, ServerMessage::SetStoryCue(s)),
        ] if n.notif_type == 4 && n.enable == 1 && n.param == u32::MAX && s.value == 77
    ));

    // The camera: the rotation as the math engine's quaternion.
    let transform = Transform::new(
        Matrix {
            m: [[0.0, 0.0, 1.0], [0.0, 1.0, 0.0], [-1.0, 0.0, 0.0]],
        },
        Vector::new(10.0, 20.0, 30.0),
    );
    pp::set_camera(&mut cx, 1, &transform, 0.5, 1.25).unwrap();
    pp::revert_camera(&mut cx, 1).unwrap();
    assert_eq!(
        pp::set_camera(&mut cx, 0, &transform, 0.5, 1.25),
        Err(PubError(-2))
    );
    let q = matrix_to_quaternion(&transform.rotation);
    let sent = sink.server();
    assert!(matches!(
        sent.as_slice(),
        [(1, ServerMessage::SetCamera(c)), (1, ServerMessage::RevertCamera(_))]
            if c.position == [10.0, 20.0, 30.0]
                && c.orientation == [q.w, q.x, q.y, q.z]
                && c.fov == 0.5
                && c.aspect == 1.25
    ));

    // The unchecked senders go to the id as given.
    pp::pop_up_dialog(&mut cx, 9, &text, &FmtStr::new(2), 3);
    pp::return_best_path(&mut cx, 9, &[1, 2, 3]);
    pp::return_player_stats(&mut cx, 9, &[4]);
    pp::load_hint(&mut cx, 9, 1, &[5, 6]);
    let sent = sink.take();
    assert_eq!(sent.len(), 4);
    assert!(sent.iter().all(|(c, _)| *c == 9));
    assert!(matches!(
        &sent[0].1,
        ToClient::Common(CommonMessage::PopUpDialog(d))
            if d.data_a == flat && d.field_10 == 3
    ));
    assert!(matches!(
        &sent[1].1,
        ToClient::Common(CommonMessage::RequestBestPath(b)) if b.data == [1, 2, 3]
    ));
    assert!(matches!(
        &sent[2].1,
        ToClient::Common(CommonMessage::RequestPlayerStats(b)) if b.data == [4]
    ));
    assert!(matches!(
        &sent[3].1,
        ToClient::Server(ServerMessage::LoadHint(h)) if h.kind == 1 && h.data == [5, 6]
    ));

    // The mission messages and objectives.
    let objective = MissionObjective {
        kind: 0x102,
        description: text.clone(),
    };
    pp::set_mission_objectives(
        &mut cx,
        1,
        42,
        std::slice::from_ref(&objective),
        &text,
        3,
        &FmtStr::default(),
    )
    .unwrap();
    pp::replace_mission_objective(&mut cx, 1, 42, 0x101, &objective).unwrap();
    pp::set_mission_objective_state(&mut cx, 1, 42, 0x105, 0x1_0002).unwrap();
    assert_eq!(pp::send_nn_message(&mut cx, 1, 0), Err(PubError(-4)));
    pp::send_nn_message(&mut cx, 1, 88).unwrap();
    let sent = sink.server();
    assert!(matches!(
        sent.as_slice(),
        [
            (1, ServerMessage::SetMissionObjectives(o)),
            (1, ServerMessage::ReplaceMissionObjective(r)),
            (1, ServerMessage::SetMissionObjectiveState(s)),
            (1, ServerMessage::SendNnMessage(n)),
        ] if o.field_00 == 42 && o.state == 3 && o.text_a == flat
            && o.objectives.len() == 1 && o.objectives[0].state == 2
            && r.field_00 == 42 && r.state == 1 && r.text == flat
            && s.field_00 == 42 && s.field_04 == 5 && s.field_08 == 2
            && n.value == 88
    ));
    // Type 0 skips the next autosave; without a group the player alone hears.
    pp::display_mission_message(&mut cx, 1, &text, 0, true).unwrap();
    assert!(cx.logic.players.get(1).unwrap().skip_next_autosave);
    let sent = sink.server();
    assert!(matches!(
        sent.as_slice(),
        [(1, ServerMessage::SetMissionMessage(m))] if m.kind == 0 && m.text == flat
    ));

    // Groups: players 1 and 2 together, 3 alone.
    assert_eq!(pp::group_size(&cx, 1), 1);
    assert_eq!(pp::group_members(&cx, 1), Ok(vec![1]));
    assert_eq!(pp::remove_from_group(&mut cx, 1), Err(PubError(-3)));
    let id = cx.logic.groups.create();
    group::add_member(&mut cx, id, 1);
    group::add_member(&mut cx, id, 2);
    sink.take();
    assert_eq!(pp::group_size(&cx, 1), 2);
    assert_eq!(pp::group_members(&cx, 2), Ok(vec![1, 2]));
    assert_eq!(pp::group_members(&cx, 4), Err(PubError(-2)));
    assert!(pp::is_group_member(&cx, 1, 2));
    assert!(!pp::is_group_member(&cx, 1, 3));
    assert!(!pp::is_group_member(&cx, 3, 1));
    assert!(pp::is_group_member(&cx, 3, 3));
    // A broadcast reaches the group; a message without one the player.
    pp::display_mission_message(&mut cx, 2, &text, 1, true).unwrap();
    pp::display_mission_message(&mut cx, 2, &text, 1, false).unwrap();
    let to: Vec<u32> = sink.server().iter().map(|(c, _)| *c).collect();
    assert_eq!(to, [1, 2, 2]);
    // The group's reward: each member's cash, counted as earned.
    let cash = |cx: &Cx<'_>, s: u32| cx.logic.players.get(s).unwrap().cash;
    let (one, two, three) = (cash(&cx, 1), cash(&cx, 2), cash(&cx, 3));
    pp::reward_group(&mut cx, 1, 100);
    pp::reward_group(&mut cx, 3, 50);
    pp::reward_group(&mut cx, 9, 50);
    assert_eq!(
        (cash(&cx, 1), cash(&cx, 2), cash(&cx, 3)),
        (one + 100, two + 100, three + 50)
    );
    sink.take();
    // The group mission.
    pp::set_msn_id(&mut cx, 1, 0x55, 2, true, 0x77);
    assert_eq!(pp::msn_id(&cx, 1), Some(0x55));
    let sent = sink.server();
    assert!(matches!(
        sent.as_slice(),
        [(1, ServerMessage::MissionSaveA(s)), (1, ServerMessage::MiscObjUpdate(m))]
            if s.value == 0x77 && m.field_38 == 0x55 && m.field_3c == 2
    ));
    pp::set_msn_id(&mut cx, 7, 0x55, 2, true, 0x77);
    assert!(sink.take().is_empty());
    // Leaving the group.
    pp::remove_from_group(&mut cx, 2).unwrap();
    assert_eq!(pp::group_members(&cx, 2), Ok(vec![2]));
    assert_eq!(pp::group_members(&cx, 1), Ok(vec![1]));
}
