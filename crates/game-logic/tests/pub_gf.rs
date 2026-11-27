//! `pub::GF` over the original data (`outdir/`): a base's position and its
//! one-entry cache, the placements of a base's start room, the setpoint
//! name round trip, the destroy wrappers and a costume's gender.

use std::path::Path;
use std::sync::Arc;

use game_logic::api::{PubError, gf};
use game_logic::bases;
use game_logic::server::{Cx, Logic, RecordingSink};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::protocol::base_props::NewsBroadcastProperties;
use neuron_core::id::create_id;
use neuron_core::vfs::Host;

#[test]
fn gf_over_the_bases() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no pub::GF");
        return;
    };
    let source = Host::new(root.join("data"));
    let mut data = GameData::new();
    data.load(&source, &config, None);
    let base = create_id(b"he01_01_base");
    let room = data.bases.base(base).expect("base").start_room;
    let costume_id = *data.costumes.costumes.keys().next().expect("a costume");
    let mut world = World::new(Arc::new(data), WorldConfig::default());
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let he01 = create_id(b"he01");

    assert_eq!(gf::find_base(b"he01_01_base"), base);
    assert_eq!(gf::spaceflight_location(), create_id(b"Spaceflight"));

    // The base's position, then the cache keyed by the base alone.
    let pos = gf::base_position(&mut cx, &source, he01, base).expect("a solar docks with it");
    assert_eq!(
        gf::base_position(&mut cx, &source, create_id(b"he02"), base),
        Ok(pos)
    );
    assert_eq!(
        gf::base_position(&mut cx, &source, he01, 0),
        Err(PubError::BAD_ID)
    );
    assert_eq!(
        gf::base_position(&mut cx, &source, he01, create_id(b"no_base")),
        Err(PubError::BAD_ID)
    );

    // The start room's placements.
    assert_eq!(
        gf::num_character_placements(&mut cx, room),
        0,
        "not loaded yet"
    );
    bases::load_base(&mut cx, &source, base);
    let n = gf::num_character_placements(&mut cx, room);
    assert!(n > 0);
    let name = gf::character_placement_name(&mut cx, room, 0);
    assert_eq!(gf::character_placement_by_name(&mut cx, room, &name), Ok(0));
    assert_eq!(
        gf::character_placement_by_name(&mut cx, room, b"no such setpoint"),
        Err(PubError::NOT_FOUND)
    );
    assert_eq!(
        gf::character_placement_by_name(&mut cx, 12345, &name),
        Err(PubError::BAD_ID)
    );
    assert!(gf::character_placement_posture(&mut cx, room, 0).is_ok());
    assert_eq!(
        gf::character_placement_posture(&mut cx, room, n),
        Err(PubError::BAD_ID)
    );
    let props = gf::character_placement_properties(&mut cx, room, 0).expect("valid name");
    assert_eq!(
        gf::is_character_placement_special(&mut cx, room, 0),
        props.zone_script
    );
    assert!(gf::is_character_placement_normal(&mut cx, 12345, 0));
    assert!(!gf::is_character_placement_special(&mut cx, 12345, 0));
    // The name formed from the properties reads back to them.
    let formed = gf::form_character_placement_name(&mut cx, &props).expect("formed");
    let back = game_sim::data::room::SetpointProps::from_name(&formed, &mut cx.logic.reports);
    assert_eq!(back, props, "{}", String::from_utf8_lossy(&formed));
    // The name part is `NameBuffer`, upper-cased, as `combine_to_name` writes it.
    let part = formed.split(|&b| b == b'/').nth(4).expect("the name part");
    assert_eq!(part, props.name_buffer.as_slice());
    assert!(!part.iter().any(u8::is_ascii_lowercase));

    // The destroy wrappers clear the handle.
    let news = NewsBroadcastProperties {
        base_id: base,
        ..NewsBroadcastProperties::default()
    };
    let mut handle = gf::news_broadcast_create(&mut cx, &news, 0);
    assert_ne!(handle, u32::MAX);
    gf::news_broadcast_destroy(&mut cx, &mut handle);
    assert_eq!(handle, u32::MAX);
    let mut character = 0;
    gf::character_destroy(&mut cx, &mut character);
    assert_eq!(character, 0);

    // A costume and its gender; unknown names are reported.
    let costume = gf::costume_by_id(&mut cx, costume_id as i32);
    assert!(costume.part_ids.iter().any(|&p| p != 0));
    let gender = gf::costume_skeleton_gender(&mut cx, &costume);
    println!("costume {costume_id:#x}: gender {gender}");
    let before = cx.logic.reports.0.len();
    assert_eq!(gf::body_part(&mut cx, b"no_such_part", 0), 0);
    assert_eq!(gf::accessory(&mut cx, b"no_such_accessory"), 0);
    assert_eq!(cx.logic.reports.0.len(), before + 2);
    assert_eq!(gf::rtc_performance_slider(&cx), 1.0, "a multiplayer server");
}
