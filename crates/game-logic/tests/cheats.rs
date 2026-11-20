//! The market functions and the multiplayer cheat check over `outdir/`: a
//! character docked at its home base passes, equipment the base does not
//! sell is reported once and every later check refuses, and cash beyond
//! what was earned is reported as a worth finding.

use std::path::Path;
use std::sync::Arc;

use game_logic::bases;
use game_logic::cheats::{LocalTime, check_for_cheats, total_equipment_value};
use game_logic::login::{self, CreateCharacterInfo, LoginInfo, ServerFiles};
use game_logic::player::{self, CharacterId};
use game_logic::selection::select_character;
use game_logic::server::{Cx, Logic, RecordingSink};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use neuron_core::id::create_id;
use neuron_core::vfs::Host;
use neuron_server::account::{AccountsRoot, hashed_name};

fn w(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

#[test]
fn markets_and_the_cheat_check() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no cheats");
        return;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let ini = std::fs::read(outdir.join("OutpostFrontier.ini")).expect("ini");
    let user_data = Path::new(env!("CARGO_TARGET_TMPDIR")).join("cheats");
    let _ = std::fs::remove_dir_all(&user_data);
    let files = ServerFiles {
        exe_dir: outdir.clone(),
        data_dir: outdir.join("data"),
        initial_world: ServerFiles::read_initial_world(&ini),
        accounts: AccountsRoot::new(&user_data, true),
    };
    let mut world = World::new(
        Arc::new(data),
        WorldConfig {
            single_player: false,
            ..WorldConfig::default()
        },
    );
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };

    // The markets.
    let base = create_id(b"he01_01_base");
    let boron = create_id(b"commodity_boron");
    let gold = create_id(b"commodity_gold");
    assert!(bases::price(&mut cx, base, boron).unwrap() > 0.0);
    assert_eq!(bases::price(&mut cx, 0, boron), Err(bases::BAD_ID));
    assert_eq!(bases::is_good_in_demand(&mut cx, base, gold), Ok(true));
    let for_sale = bases::commodities_for_sale(&mut cx, base, 64).unwrap();
    assert!(for_sale.contains(&boron) && !for_sale.contains(&gold));
    assert!(
        bases::commodities_in_demand(&mut cx, base, 64)
            .unwrap()
            .contains(&gold)
    );
    assert_eq!(
        bases::commodities_for_sale(&mut cx, base, 1).unwrap().len(),
        1
    );
    assert!(bases::find_base(&mut cx, 12345).is_none());

    // A character at its home base.
    let info = LoginInfo {
        account_name: w("Trader"),
        create: true,
    };
    assert_eq!(login::login(&mut cx, &files, &info, 1), 3);
    let create = CreateCharacterInfo {
        name: w("Merchant"),
        faction: create_id(b"new_player"),
        base,
        package: create_id(b"ge_fighter"),
        pilot: create_id(b"rellan"),
    };
    assert!(login::create_character(&mut cx, &files, 1, &create));
    let id = CharacterId::new(format!("{}.fl", hashed_name(&create.name)).as_bytes());
    assert!(select_character(&mut cx, &files, 1, &id));
    assert_eq!(cx.logic.players.get(1).unwrap().current_base, base);
    player::snapshot_equipment(cx.logic.players.get_mut(1).unwrap());
    player::reset_total_earned(&mut cx, 1);
    let worth = total_equipment_value(&mut cx, 1);
    assert!(worth >= 2000, "the cash and the ship: {worth}");
    let now = LocalTime {
        year: 2026,
        month: 11,
        day: 20,
        hour: 9,
        minute: 5,
        second: 7,
    };
    assert!(!check_for_cheats(&mut cx, 1, &files.accounts, now));

    // Cash beyond what was earned: a worth finding.
    player::set_cash(&mut cx, 1, 500_000);
    assert!(check_for_cheats(&mut cx, 1, &files.accounts, now));
    let log = std::fs::read_to_string(files.accounts.cheaters_log_path()).expect("cheaters.log");
    assert_eq!(
        log,
        "11/20/2026 09:05:07 Possible cheating detected (Ship Related) by Merchant(Trader)\r\n"
    );
    // Reported once; every later check refuses without a second line.
    assert!(check_for_cheats(&mut cx, 1, &files.accounts, now));
    assert_eq!(
        std::fs::read_to_string(files.accounts.cheaters_log_path()).unwrap(),
        log
    );

    // Equipment the base does not sell, on a fresh character state.
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.cheat_reported = false;
        slot.cash = 0;
    }
    player::reset_total_earned(&mut cx, 1);
    let missile = create_id(b"missile01_mark02");
    let bay = cx.world.strings().cargo_bay();
    player::add_equipment(&mut cx, 1, missile, bay, 1, 1.0, 0, false, true);
    assert!(check_for_cheats(&mut cx, 1, &files.accounts, now));
    let log = std::fs::read_to_string(files.accounts.cheaters_log_path()).unwrap();
    assert!(
        log.ends_with("(Ship or Equipment not sold on base) by Merchant(Trader)\r\n"),
        "{log}"
    );

    // Single player never checks.
    cx.world.set_multiplayer(false);
    cx.logic.players.get_mut(1).unwrap().cheat_reported = false;
    assert!(!check_for_cheats(&mut cx, 1, &files.accounts, now));
}
