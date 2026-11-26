use neuron_client::ui::controls::text_control::TextControlWidget;
use neuron_client::ui::tree::{ACTIVE, PICKABLE, Payload};
use neuron_client::ui::widescreen::Surface;
use neuron_core::math::Vector;

use super::*;

fn units(s: &str) -> Vec<u16> {
    s.encode_utf16().collect()
}

/// Two roots: a plain hidden root with a visible child and its grandchild,
/// and a visible text control.
fn sample() -> (Ui, [NodeId; 4]) {
    let mut ui = Ui::new(Surface::new(1024, 768));
    let root = ui.create_plain(None);
    let child = ui.create_plain(Some(root));
    let grandchild = ui.create_plain(Some(child));
    let text = ui
        .create(
            Box::new(TextControlWidget::default()),
            Some("TextControl"),
            None,
            &mut Payload::None,
        )
        .expect("the text control is created");
    for (id, flags) in [
        (root, PICKABLE),
        (child, ACTIVE | VISIBLE | PICKABLE | 0x30),
        (grandchild, VISIBLE),
        (text, ACTIVE | VISIBLE | PICKABLE),
    ] {
        ui.node_mut(id).unwrap().flags = flags;
    }
    ui.node_mut(child).unwrap().set_name(Some(b"Child"));
    ui.node_mut(child).unwrap().position = Vector {
        x: 1.0,
        y: -2.0,
        z: 0.5,
    };
    ui.node_mut(text).unwrap().draw_layer = 10;
    ui.node_mut(text).unwrap().draw_sub_layer = 7;
    (ui, [root, child, grandchild, text])
}

#[test]
fn the_tree_flattens_in_pre_order_with_inherited_visibility() {
    let (ui, [root, child, grandchild, text]) = sample();
    let nodes = flatten_all(&ui);
    let got: Vec<_> = nodes
        .iter()
        .map(|n| (n.id, n.parent, n.depth, n.visible))
        .collect();
    assert_eq!(
        got,
        [
            (root, -1, 0, false),
            (child, 0, 1, false),
            (grandchild, 1, 2, false),
            (text, -1, 0, true),
        ]
    );
    let mut sub = Vec::new();
    flatten_from(&ui, child, &mut sub);
    assert_eq!(sub.len(), 2);
    assert!(sub[0].visible && sub[1].visible);
    assert_eq!(sub[1].parent, 0);
}

#[test]
fn an_unregistered_or_parented_object_is_no_root() {
    let (mut ui, [root, ..]) = sample();
    ui.node_mut(root).unwrap().flags |= UNREGISTERED;
    let nodes = flatten_all(&ui);
    assert_eq!(nodes.len(), 1);
}

#[test]
fn node_lines_read_as_the_cpp_ones() {
    let (ui, _) = sample();
    let dump = dump_nodes(&ui, 3);
    assert_eq!(dump.roots, 2);
    assert_eq!(dump.nodes.len(), 4);
    let identity = r#""orient":["3f800000","00000000","00000000","00000000","3f800000","00000000","00000000","00000000","3f800000"]"#;
    assert_eq!(
        dump.nodes[1],
        format!(
            r#"{{"dump":3,"i":1,"p":0,"d":1,"class":null,"rtti":null,"name":"Child","flags":11,"vis":false,"layer":0,"sub":0,"hit":0,"pos":["3f800000","c0000000","3f000000"],{identity}}}"#
        )
    );
    let text = &dump.nodes[3];
    assert!(
        text.starts_with(
            r#"{"dump":3,"i":3,"p":-1,"d":0,"class":"TextControl","rtti":null,"name":"","flags":11,"vis":true,"layer":10,"sub":7,"hit":0,"#
        ),
        "{text}"
    );
    assert!(text.contains(r#","ctl":{"offset":["#), "{text}");
    assert!(text.contains(r#","tc":{"w":"#), "{text}");
}

#[test]
fn a_window_file_gives_its_head_and_one_record_per_node() {
    let (ui, [_, child, ..]) = sample();
    let lines = winini_lines(&ui, 0, b"INTERFACE\\NAVBAR.INI", child);
    assert_eq!(lines.len(), 3);
    assert_eq!(
        lines[0],
        r#"{"frame":0,"rec":"winini","path":"INTERFACE\\NAVBAR.INI","nodes":2}"#
    );
    assert!(
        lines[2].starts_with(
            r#"{"frame":0,"rec":"winnode","path":"INTERFACE\\NAVBAR.INI","i":1,"p":0,"d":1,"#
        ),
        "{}",
        lines[2]
    );
}

#[test]
fn captions_compare_as_the_cpp_does() {
    assert!(caption_equals(Some(&units("Load Game")), "LOAD GAME"));
    assert!(caption_equals(Some(&units("load game\0junk")), "LOAD GAME"));
    assert!(!caption_equals(Some(&units("LOAD GAMES")), "LOAD GAME"));
    assert!(!caption_equals(Some(&units("LOAD GAM\u{e9}")), "LOAD GAME"));
    assert!(!caption_equals(None, ""));
    assert!(caption_equals(Some(&[]), ""));
}

/// A widget that takes the press only and records what reached it.
struct Clickable(Vec<i32>);

impl neuron_client::ui::tree::Widget for Clickable {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
    fn mouse_event(&mut self, _: &mut Ui, _: NodeId, event: MouseButton) -> bool {
        self.0.push(event.field0() * 10 + event.field1());
        event.field1() == 2
    }
}

#[test]
fn clicks_find_their_target_visible_ones_first() {
    let (mut ui, [root, child, ..]) = sample();
    let hidden = ui
        .create(
            Box::new(Clickable(Vec::new())),
            None,
            Some(root),
            &mut Payload::None,
        )
        .unwrap();
    let shown = ui
        .create(
            Box::new(Clickable(Vec::new())),
            None,
            None,
            &mut Payload::None,
        )
        .unwrap();
    for id in [hidden, shown] {
        ui.node_mut(id).unwrap().set_name(Some(b"MainMenu"));
    }
    ui.node_mut(shown).unwrap().flags |= VISIBLE;
    assert_eq!(resolve_target(&ui, "mainmenu"), Ok(shown));
    assert_eq!(resolve_target(&ui, "#1"), Ok(child));
    assert_eq!(resolve_target(&ui, "#99"), Err("no such node".into()));
    assert_eq!(
        resolve_target(&ui, "text:LOAD GAME"),
        Err("not found".into())
    );
    ui.node_mut(shown).unwrap().flags &= !VISIBLE;
    assert_eq!(resolve_target(&ui, "MAINMENU"), Ok(hidden));

    assert_eq!(click(&mut ui, "MainMenu"), Ok("press=1 release=0".into()));
    assert_eq!(ui.widget::<Clickable>(hidden).unwrap().0, [-8, -9]);
}
