use neuron_client::ui::tree::{ACTIVE, VISIBLE};
use neuron_client::ui::widescreen::Surface;

use super::*;

fn panel() -> (Ui, NodeId) {
    let mut ui = Ui::new(Surface::new(1024, 768));
    let item = create(&mut IniReader::new(), &mut ui).expect("the element");
    assert_eq!(item.name(), b"Test_Status");
    (ui, item.window().expect("a window"))
}

fn child(ui: &Ui, id: NodeId, name: &str) -> NodeId {
    ui.children(id)
        .into_iter()
        .find(|&c| ui.node(c).unwrap().name() == name.as_bytes())
        .unwrap_or_else(|| panic!("{name}"))
}

fn shown(ui: &Ui, id: NodeId) -> bool {
    ui.node(id).unwrap().flags & (ACTIVE | VISIBLE) == ACTIVE | VISIBLE
}

fn launcher(sub_id: u16, hardpoint: &[u8], ammo_count: i32) -> StatusLauncher {
    StatusLauncher {
        sub_id,
        hardpoint: hardpoint.to_vec(),
        ids_name: 0,
        ammo_count,
        active: true,
    }
}

fn fighter() -> StatusShip {
    let legal = |hp_type, names: &[&[u8]]| StatusLegalHps {
        hp_type,
        hardpoints: names.iter().map(|n| n.to_vec()).collect(),
    };
    StatusShip {
        // The countermeasure dropper first: the rows put it last.
        launchers: vec![
            launcher(3, b"HpCM01", 20),
            launcher(2, b"HpWeapon02", -1),
            launcher(1, b"HpWeapon01", -1),
        ],
        legal_hps: Some(vec![
            legal(0x1a, &[b"HpWeapon01", b"HpWeapon02"]),
            legal(0x19, &[b"HpWeapon01", b"HpWeapon02"]),
            legal(0xb, &[b"HpCM01"]),
        ]),
        ..StatusShip::default()
    }
}

#[test]
fn create_makes_the_panels_children_in_order_and_minimized() {
    let (ui, id) = panel();
    let names: Vec<String> = ui
        .children(id)
        .iter()
        .map(|&c| String::from_utf8_lossy(ui.node(c).unwrap().name()).into_owned())
        .collect();
    let mut expected: Vec<String> = ["StatusBackground", "StatusMinimizedFrame", "HudStatusOpen"]
        .map(String::from)
        .to_vec();
    for i in 0..10 {
        expected.push(format!("WeaponNumberText{i}"));
        expected.push(format!("WeaponText{i}"));
        expected.push(format!("AmmoText{i}"));
    }
    expected.extend(
        [
            "StatusCloseButton",
            "HudRepairButton",
            "RepairCountText",
            "UseBatteryButton",
            "BatteryCountText",
            "WeaponListScroll",
            "SpecialModeText",
            "SpecialModeTextCancel",
            "LootButton",
        ]
        .map(String::from),
    );
    assert_eq!(names, expected);
    let node = ui.node(id).unwrap();
    assert_eq!(node.name(), b"HUD_Status");
    let c = ui.win_control(id).unwrap();
    assert_eq!(c.half_extents, Vector::new(0.105, 0.02, 0.0));
    assert_eq!(c.offset, Vector::new(0.0, 0.0, -1.0));
    assert!(!shown(&ui, child(&ui, id, "StatusBackground")));
    assert!(!shown(&ui, child(&ui, id, "WeaponText0")));
    assert!(shown(&ui, child(&ui, id, "StatusMinimizedFrame")));
    assert!(!shown(&ui, child(&ui, id, "LootButton")));
}

#[test]
fn the_rows_number_column_steps_down_from_minus_0_199() {
    assert_eq!(row_y(0), (-0.199, -0.199));
    let (number, row) = row_y(1);
    assert_eq!(number.to_bits(), 0xbe5e_3540);
    assert_eq!(row.to_bits(), 0xbe5e_353f);
}

#[test]
fn the_weapon_list_is_ordered_by_row_then_hardpoint() {
    let ship = fighter();
    let mut cache = Vec::new();
    let rows = rebuild_weapon_list(&mut cache, Some(&ship));
    let order: Vec<&[u8]> = rows.iter().map(|w| &w.hardpoint[..]).collect();
    assert_eq!(order, [&b"HpWeapon01"[..], b"HpWeapon02", b"HpCM01"]);
    // hp_gun_special_2 (0x1a) is row 12, the countermeasure dropper's 0xb
    // row 24.
    assert_eq!(rows[0].sort_key, 12);
    assert_eq!(rows[2].sort_key, 24);
    assert_eq!(cache.len(), 3);
    assert_eq!(find_hardpoint_row(&mut cache, None, b"HpCM01"), 24);
    assert_eq!(find_hardpoint_row(&mut cache, None, b"HpMine01"), 0);
}

#[test]
fn an_open_session_flag_opens_the_panel_and_fills_the_rows() {
    let (mut ui, id) = panel();
    ui.frame_time = 1.0 / 30.0;
    set_input(
        &mut ui,
        id,
        StatusInput {
            ship: Some(fighter()),
            view_object: true,
            target: true,
            session_flags: 3,
            repair_count: Some(10),
            battery_count: Some(1234),
            ..StatusInput::default()
        },
    );
    for _ in 0..30 {
        ui.update_node(id);
    }
    let w = ui.widget::<HudStatus>(id).unwrap();
    assert!(w.state.expanded);
    assert_eq!(w.state.session_flag_2, Some(true));
    let c = ui.win_control(id).unwrap();
    assert_eq!(c.half_extents, Vector::new(0.131, 0.117, 0.0));
    assert_eq!(c.hit_offset, Vector::new(0.376, -0.267, 0.0));
    for i in 0..3 {
        assert!(shown(&ui, child(&ui, id, &format!("WeaponText{i}"))));
        assert!(shown(&ui, child(&ui, id, &format!("WeaponNumberText{i}"))));
    }
    assert!(!shown(&ui, child(&ui, id, "AmmoText0")));
    assert!(shown(&ui, child(&ui, id, "AmmoText2")));
    for i in 3..10 {
        assert!(!shown(&ui, child(&ui, id, &format!("WeaponText{i}"))));
    }
    assert!(!shown(&ui, child(&ui, id, "WeaponListScroll")));
    assert!(!shown(&ui, child(&ui, id, "HudStatusOpen")));
    assert!(!shown(&ui, child(&ui, id, "StatusMinimizedFrame")));
    assert!(shown(&ui, child(&ui, id, "StatusBackground")));
    let frame = child(&ui, id, "StatusMinimizedFrame");
    assert_eq!(
        ui.win_control(frame).unwrap().offset,
        Vector::new(0.4101, -0.4, -1.0)
    );
    let battery = child(&ui, id, "BatteryCountText");
    let text = ui
        .widget::<TextControlWidget>(battery)
        .unwrap()
        .text
        .text
        .text();
    assert_eq!(text.map(String::from_utf16_lossy), Some("999".to_owned()));
}

#[test]
fn the_key_name_goes_into_the_first_string_argument() {
    let line = format_key(&wide("PRESS %s TO RETURN 100%%"), &wide("F"));
    assert_eq!(String::from_utf16_lossy(&line), "PRESS F TO RETURN 100%");
}
