use neuron_client::ui::tree::{ACTIVE, VISIBLE};
use neuron_client::ui::widescreen::Surface;

use super::*;

const FRAME: f64 = 1.0 / 30.0;

fn panel() -> (Ui, NodeId) {
    let mut ui = Ui::new(Surface::new(1024, 768));
    let mut reader = IniReader::new();
    let item = create(&mut reader, &mut ui).expect("the element");
    assert_eq!(item.name(), b"Target");
    assert_eq!(item.type_name(), "class HudTarget");
    let id = item.window().unwrap();
    (ui, id)
}

fn shown(ui: &Ui, id: Option<NodeId>) -> bool {
    ui.node(id.unwrap()).unwrap().flags & (ACTIVE | VISIBLE) == ACTIVE | VISIBLE
}

fn offset(ui: &Ui, id: Option<NodeId>) -> Vector {
    ui.win_control(id.unwrap()).unwrap().offset
}

#[test]
fn the_collapsed_panel_shows_its_bottom_row() {
    let (mut ui, id) = panel();
    let node = ui.node(id).unwrap();
    assert_eq!(node.name(), b"HUD_Target");
    assert_eq!(node.class_name, Some("HUD_Target"));
    assert_eq!(ui.children(id).len(), 18);
    let t = ui.win_control(id).unwrap();
    assert_eq!(t.half_extents, Vector::new(0.109, 0.02, 0.0));
    assert_eq!(t.hit_offset, Vector::new(-0.402, -0.371, 0.0));
    let c = ui.widget_mut::<HudTarget>(id).unwrap().c;
    for hidden in [
        c.view_panel,
        c.ship_name,
        c.subtarget_name,
        c.close_button,
        c.rank_text,
        c.previous_button,
        c.next_button,
        c.trade_button,
        c.formation_button,
    ] {
        assert!(!shown(&ui, hidden));
    }
    for visible in [
        c.minimized_frame,
        c.toggle_button,
        c.scan_button,
        c.contact_list_button,
        c.next_enemy_button,
    ] {
        assert!(shown(&ui, visible));
    }
}

#[test]
fn the_first_updates_open_the_panel_on_the_contact_list() {
    let (mut ui, id) = panel();
    ui.update_all(FRAME);
    {
        let t = ui.widget_mut::<HudTarget>(id).unwrap();
        assert!(t.mover.is_active());
        assert_eq!(t.get_mode(false), 3);
    }
    for _ in 0..29 {
        ui.update_all(FRAME);
    }
    let t = ui.widget_mut::<HudTarget>(id).unwrap();
    assert!(t.expanded);
    assert!(!t.mover.is_active() && !t.scaler.is_active());
    assert_eq!(t.get_mode(false), 0);
    let c = t.c;
    let w = ui.win_control(id).unwrap();
    assert_eq!(w.half_extents, Vector::new(0.131, 0.117, 0.0));
    assert_eq!(w.hit_offset, Vector::new(-0.379, -0.266, 0.0));
    for visible in [
        c.view_panel,
        c.close_button,
        c.contact_list_button,
        c.target_button,
        c.next_enemy_button,
    ] {
        assert!(shown(&ui, visible));
    }
    for hidden in [
        c.minimized_frame,
        c.toggle_button,
        c.ship_name,
        c.subtarget_name,
        c.previous_button,
        c.next_button,
        c.scan_button,
        c.tractor_button,
        c.comm_button,
        c.request_group_button,
        c.trade_button,
        c.formation_button,
        c.rank_text,
    ] {
        assert!(!shown(&ui, hidden));
    }
    assert_eq!(
        offset(&ui, c.toggle_button),
        Vector::new(-0.375, -0.27, 0.0)
    );
    assert_eq!(
        offset(&ui, c.minimized_frame),
        Vector::new(-0.4105, -0.4, -1.0)
    );
    assert_eq!(
        offset(&ui, c.next_enemy_button),
        Vector::new(-0.465, -0.171, 0.0)
    );
}

#[test]
fn the_close_button_collapses_the_open_panel() {
    let (mut ui, id) = panel();
    for _ in 0..30 {
        ui.update_all(FRAME);
    }
    let close = ui
        .widget_mut::<HudTarget>(id)
        .unwrap()
        .c
        .close_button
        .unwrap();
    let took = ui.property(
        id,
        Property::FromChild {
            msg: 0x26,
            from: close,
            value: 0,
        },
    );
    assert_eq!(took, 1);
    for _ in 0..30 {
        ui.update_all(FRAME);
    }
    let t = ui.widget_mut::<HudTarget>(id).unwrap();
    assert!(!t.expanded);
    let c = t.c;
    assert_eq!(
        ui.win_control(id).unwrap().half_extents,
        Vector::new(0.109, 0.02, 0.0)
    );
    assert_eq!(
        offset(&ui, c.toggle_button),
        Vector::new(-0.296, -0.367, 0.0)
    );
    assert_eq!(
        offset(&ui, c.next_enemy_button),
        Vector::new(-0.479, -0.37, 0.0)
    );
    assert!(shown(&ui, c.minimized_frame));
    assert!(!shown(&ui, c.view_panel));
}
