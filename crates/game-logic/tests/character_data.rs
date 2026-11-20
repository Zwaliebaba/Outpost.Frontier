//! The character save reader on the original data (`outdir/`, decision
//! O15): the single-player start `newplayer.fl` and the multiplayer
//! template `mpnewcharacter.fl`.

use std::path::{Path, PathBuf};

use game_logic::character_file::{SaveData, SavedCharacter, load_save_record, read_character_name};
use game_sim::data::config::{DataConfig, GameData};
use neuron_core::id::create_id;
use neuron_core::vfs::Host;
use neuron_server::save::read_save;

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn game_data() -> Option<GameData> {
    let root = outdir();
    let config = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(root.join("data")), &config, None);
    Some(data)
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

fn load(data: &mut GameData, file: &str) -> (SavedCharacter, bool, Vec<String>) {
    let bytes = std::fs::read(outdir().join(file)).expect("template");
    let text = read_save(&bytes).text().expect("readable").to_vec();
    let mut record = SavedCharacter::default();
    let mut reports = game_sim::data::Reports::default();
    let save = SaveData {
        universe: &data.universe,
        archetypes: &data.archetypes,
        reputation: &data.reputation,
        costumes: &data.costumes,
    };
    let saw = load_save_record(
        &text,
        file,
        &mut record,
        save,
        &mut data.strings,
        &mut reports,
    );
    let texts = reports.0.iter().map(|r| r.text.clone()).collect();
    (record, saw, texts)
}

#[test]
fn the_single_player_start_loads() {
    let Some(mut data) = game_data() else {
        println!("outdir/ is missing: no characters");
        return;
    };
    let (record, saw, reports) = load(&mut data, "newplayer.fl");
    assert!(saw);
    assert!(reports.is_empty(), "{reports:#?}");
    assert_eq!(record.name, wide("Rellan"));
    assert_eq!(record.money, 500);
    assert_eq!(record.rank, 0);
    assert_eq!(record.voice, b"rellan_voice");
    assert_eq!(record.system_id, create_id(b"relayseven_system"));
    assert_eq!(record.base_id, 0);
    assert_eq!(record.position, [3216.0, 1314.0, -21775.0]);
    assert_eq!(record.ship_archetype, create_id(b"ge_fighter"));
    let ship = &data.archetypes.ships[&record.ship_archetype];
    assert_eq!(
        record.collision_groups.len(),
        ship.eq_obj.collision_groups.len()
    );
    // 16 `equip` lines and 3 `cargo` lines, numbered from 0x22.
    assert_eq!(record.equipment.len(), 19);
    let ids: Vec<u16> = record.equipment.iter().map(|e| e.id).collect();
    assert_eq!(ids, (0x22..0x22 + 19).collect::<Vec<u16>>());
    let ammo = &record.equipment[8];
    assert_eq!(ammo.arch_id, create_id(b"ge_s_cm_01_ammo"));
    assert_eq!((ammo.count, ammo.equipped), (20, 0));
    assert_eq!(record.relations.len(), 55);
    assert!(record.com_costume.part_ids.iter().any(|&p| p != 0));
    assert_eq!(record.com_costume, record.base_costume);
    assert_eq!(record.mission_log.len(), 1);
    assert_eq!(record.mission_log[0].mad_lib_id, 1553);
    assert_eq!(record.interface, 3);
    assert_eq!(
        read_character_name(&std::fs::read(outdir().join("newplayer.fl")).unwrap()),
        wide("Rellan")
    );
}

#[test]
fn the_multiplayer_template_reports_its_placeholders() {
    let Some(mut data) = game_data() else {
        println!("outdir/ is missing: no characters");
        return;
    };
    let (record, saw, reports) = load(&mut data, "mpnewcharacter.fl");
    assert!(saw);
    assert_eq!(record.name, wide("%%NAME%%"));
    // `system = %%HOME_SYSTEM%%` and `base = %%HOME_BASE%%` resolve to
    // nothing and are reported (with the universe's own warnings).
    assert_eq!((record.system_id, record.base_id), (0, 0));
    assert!(
        reports
            .iter()
            .any(|r| r.contains("invalid system name (%%HOME_SYSTEM%%)"))
    );
    assert!(
        reports
            .iter()
            .any(|r| r.contains("invalid base name (%%HOME_BASE%%)"))
    );
    assert!(record.relations.len() >= 40);
    assert!(
        record
            .relations
            .iter()
            .all(|r| (-1.0..=1.0).contains(&r.feeling))
    );
}
