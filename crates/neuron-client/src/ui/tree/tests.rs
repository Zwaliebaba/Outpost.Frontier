use std::cell::RefCell;
use std::rc::Rc;

use super::*;
use crate::ui::control::{PlainControl, base, load_ini, ray_hits_box};
use crate::ui::message::{ButtonAction, MSG_HIDE, MSG_SHOW};
use crate::ui::page::{MasterWin, PageDesc, page_frame};

type Log = Rc<RefCell<Vec<String>>>;

/// A widget that logs its calls and is hit when the ray's x is inside
/// `[lo, hi]`.
struct Probe {
    tag: &'static str,
    log: Log,
    hit: (f32, f32),
    takes_mouse: bool,
    takes_keys: bool,
}

impl Probe {
    fn boxed(tag: &'static str, log: &Log, hit: (f32, f32)) -> Box<Probe> {
        Box::new(Probe {
            tag,
            log: Rc::clone(log),
            hit,
            takes_mouse: true,
            takes_keys: false,
        })
    }

    fn note(&self, what: &str) {
        self.log.borrow_mut().push(format!("{}:{what}", self.tag));
    }
}

impl Widget for Probe {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn hit_test(&mut self, _ui: &mut Ui, _id: NodeId, ray: Vector) -> bool {
        ray.x >= self.hit.0 && ray.x <= self.hit.1
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.note("update");
        ui.base_update(id);
    }
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        self.note("inactive");
        ui.base_update_inactive(id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.note("draw");
        ui.draw_children(id, host);
    }
    fn mouse_enter(&mut self, _ui: &mut Ui, _id: NodeId, _ray: Vector) {
        self.note("enter");
    }
    fn mouse_leave(&mut self, _ui: &mut Ui, _id: NodeId, _ray: Vector) {
        self.note("leave");
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        self.note(&format!("button{}", event.field1()));
        if self.takes_mouse {
            true
        } else {
            ui.base_mouse_event(id, event)
        }
    }
    fn key_event(&mut self, _ui: &mut Ui, _id: NodeId, key: u32, mods: &mut KeyMods) -> bool {
        self.note(&format!("key{key:x}/{:x}", mods.0));
        self.takes_keys
    }
    fn wheel(&mut self, _ui: &mut Ui, _id: NodeId, delta: i32) -> bool {
        self.note(&format!("wheel{delta}"));
        true
    }
}

fn ui() -> Ui {
    Ui::new(Surface::new(800, 600))
}

fn make(ui: &mut Ui, w: Box<dyn Widget>, parent: Option<NodeId>) -> NodeId {
    ui.create(w, None, parent, &mut Payload::None).unwrap()
}

fn taken(log: &Log) -> Vec<String> {
    std::mem::take(&mut *log.borrow_mut())
}

#[test]
fn creation_registers_in_order_and_links_children() {
    let mut ui = ui();
    let log = Log::default();
    let root = make(&mut ui, Probe::boxed("root", &log, (0.0, 0.0)), None);
    let a = make(&mut ui, Probe::boxed("a", &log, (0.0, 0.0)), Some(root));
    let b = make(&mut ui, Probe::boxed("b", &log, (0.0, 0.0)), Some(root));
    assert_eq!(ui.registered().collect::<Vec<_>>(), vec![root, a, b]);
    assert_eq!(ui.children(root), vec![a, b]);
    assert_eq!(ui.node(a).unwrap().flags, PICKABLE | ACTIVE | VISIBLE);
    assert!(ui.is_descendant_of(b, root));
    ui.reparent(a, Some(b));
    assert_eq!(ui.children(root), vec![b]);
    assert_eq!(ui.root_of(a), root);
    ui.node_mut(a)
        .unwrap()
        .set_name(Some(b"Some.Name.That.Is.Longer.Than.31"));
    assert_eq!(
        ui.node(a).unwrap().name(),
        b"Some.Name.That.Is.Longer.Than.3"
    );
    assert_eq!(ui.find_by_name(b"some.name.that.is.longer.than.3"), Some(a));
}

#[test]
fn destroy_unregisters_and_the_update_drains() {
    let mut ui = ui();
    let log = Log::default();
    let root = make(&mut ui, Probe::boxed("root", &log, (0.0, 0.0)), None);
    let child = make(&mut ui, Probe::boxed("child", &log, (0.0, 0.0)), Some(root));
    ui.set_key_focus(Some(child));
    ui.destroy(root);
    assert_eq!(ui.flags(root) & (ACTIVE | VISIBLE), 0);
    assert_ne!(ui.flags(child) & UNREGISTERED, 0);
    assert_eq!(ui.key_focus(), None);
    assert_eq!(ui.parent(child), None);
    // Nulled until the next update.
    assert_eq!(ui.registry.len(), 2);
    ui.update_all(0.0);
    assert_eq!(ui.registry.len(), 0);
    // Both stay allocated until deleted.
    assert_eq!(ui.live_count(), 2);
    ui.delete(child);
    ui.delete(root);
    assert_eq!(ui.live_count(), 0);
    assert!(!ui.contains(root));
}

#[test]
fn picks_prefer_depth_then_priority_then_z() {
    let mut ui = ui();
    let log = Log::default();
    let root = make(&mut ui, Probe::boxed("root", &log, (-1.0, 1.0)), None);
    let child = make(
        &mut ui,
        Probe::boxed("child", &log, (-1.0, 0.0)),
        Some(root),
    );
    let ray_left = Vector::new(-0.5, 0.0, -1.0);
    let ray_right = Vector::new(0.5, 0.0, -1.0);
    assert_eq!(ui.pick(ray_left), Some(child));
    assert_eq!(ui.pick(ray_right), Some(root));
    // An explicit priority wins over depth.
    ui.node_mut(root).unwrap().hit_priority = 5;
    assert_eq!(ui.pick(ray_left), Some(root));
    ui.node_mut(root).unwrap().hit_priority = 0;
    // A sibling of equal depth wins only with a larger z.
    let sibling = make(&mut ui, Probe::boxed("sib", &log, (-1.0, 0.0)), Some(root));
    assert_eq!(ui.pick(ray_left), Some(child));
    ui.node_mut(sibling).unwrap().position.z = 0.5;
    assert_eq!(ui.pick(ray_left), Some(sibling));
    // An invisible ancestor hides the subtree; inactive objects are skipped.
    ui.node_mut(root).unwrap().flags &= !VISIBLE;
    assert_eq!(ui.pick(ray_left), None);
    ui.node_mut(root).unwrap().flags |= VISIBLE;
    ui.node_mut(sibling).unwrap().flags &= !ACTIVE;
    assert_eq!(ui.pick(ray_left), Some(child));
    assert_eq!(ui.pick_within(child, ray_right), None);
    assert_eq!(ui.pick_within(root, ray_right), Some(root));
}

#[test]
fn hover_moves_with_the_mouse() {
    let mut ui = ui();
    let log = Log::default();
    let left = make(&mut ui, Probe::boxed("left", &log, (-1.0, -0.01)), None);
    let _right = make(&mut ui, Probe::boxed("right", &log, (0.01, 1.0)), None);
    assert!(ui.handle(Input::Move { x: 100, y: 300 }));
    assert_eq!(ui.hover(), Some(left));
    assert_eq!(taken(&log), vec!["left:enter"]);
    assert!(ui.stored_mouse_ray().x < 0.0);
    ui.handle(Input::Move { x: 700, y: 300 });
    assert_eq!(taken(&log), vec!["left:leave", "right:enter"]);
    // The frame's pick runs only while the cursor is drawn.
    ui.cursor_drawn = false;
    ui.update_all(0.0);
    assert_eq!(ui.hover(), None);
    assert_eq!(
        taken(&log),
        vec!["right:leave", "left:update", "right:update"]
    );
}

#[test]
fn buttons_go_down_the_capture_stack_then_to_the_pick() {
    let mut ui = ui();
    let log = Log::default();
    let dialog = make(&mut ui, Probe::boxed("dialog", &log, (-0.2, 0.2)), None);
    let ok = make(&mut ui, Probe::boxed("ok", &log, (-0.1, 0.0)), Some(dialog));
    let behind = make(&mut ui, Probe::boxed("behind", &log, (-1.0, 1.0)), None);
    ui.handle(Input::Move { x: 380, y: 300 });
    taken(&log);
    ui.push_capture(dialog);
    let down = Input::Button(MouseButton::LEFT_DOWN);
    assert!(ui.handle(down));
    assert_eq!(taken(&log), vec!["ok:button2"]);
    // A miss inside the capture goes to the capture itself.
    ui.handle(Input::Move { x: 700, y: 300 });
    taken(&log);
    assert!(ui.handle(down));
    assert_eq!(taken(&log), vec!["dialog:button2"]);
    // Without the capture the pick (the deepest hit) takes it.
    ui.remove_capture(dialog);
    ui.handle(Input::Move { x: 380, y: 300 });
    taken(&log);
    ui.handle(down);
    assert_eq!(taken(&log), vec!["ok:button2"]);
    // An object that declines hands the event to its parent.
    ui.widget_mut::<Probe>(ok).unwrap().takes_mouse = false;
    ui.handle(Input::Button(MouseButton {
        index: 0,
        action: ButtonAction::DoubleClick,
    }));
    assert_eq!(taken(&log), vec!["ok:button3", "dialog:button3"]);
    let _ = behind;
}

#[test]
fn the_capture_stack_moves_an_object_to_the_head() {
    let mut ui = ui();
    let a = ui.create_plain(None);
    let b = ui.create_plain(None);
    ui.push_capture(a);
    ui.push_capture(b);
    ui.push_capture(a);
    assert_eq!(ui.capture_stack(), &[a, b]);
    ui.remove_capture(a);
    assert_eq!(ui.capture(), Some(b));
    // Unregistering the head drops the whole stack, as in the reference.
    ui.push_capture(a);
    ui.destroy(a);
    assert!(ui.capture_stack().is_empty());
}

#[test]
fn keys_go_to_the_focus_or_in_registry_order() {
    let mut ui = ui();
    let log = Log::default();
    let a = make(&mut ui, Probe::boxed("a", &log, (0.0, 0.0)), None);
    let b = make(&mut ui, Probe::boxed("b", &log, (0.0, 0.0)), None);
    let c = make(&mut ui, Probe::boxed("c", &log, (0.0, 0.0)), None);
    ui.widget_mut::<Probe>(b).unwrap().takes_keys = true;
    let mods = KeyMods::from_record(true, 1);
    assert!(ui.handle(Input::Key { key: 0x41, mods }));
    assert_eq!(taken(&log), vec!["a:key41/6", "b:key41/6"]);
    ui.set_key_focus(Some(c));
    assert!(!ui.handle(Input::Key { key: 0x41, mods }));
    assert_eq!(taken(&log), vec!["c:key41/6"]);
    let _ = a;
}

#[test]
fn the_wheel_goes_to_the_capture_or_the_pick() {
    let mut ui = ui();
    let log = Log::default();
    let a = make(&mut ui, Probe::boxed("a", &log, (-1.0, 1.0)), None);
    let b = make(&mut ui, Probe::boxed("b", &log, (5.0, 5.0)), None);
    ui.handle(Input::Move { x: 400, y: 300 });
    taken(&log);
    assert!(ui.handle(Input::Wheel(-2)));
    ui.push_capture(b);
    ui.handle(Input::Wheel(3));
    assert_eq!(taken(&log), vec!["a:wheel-2", "b:wheel3"]);
    let _ = a;
}

#[test]
fn the_modal_count_updates_only_the_captured_control() {
    let mut ui = ui();
    let log = Log::default();
    let _a = make(&mut ui, Probe::boxed("a", &log, (0.0, 0.0)), None);
    let dialog = make(&mut ui, Box::new(PlainControl::default()), None);
    ui.cursor_drawn = false;
    ui.update_all(0.0);
    assert_eq!(taken(&log), vec!["a:update"]);
    ui.with_widget(dialog, (), |w, ui| {
        base::capture_mouse(ui, dialog, w.win_control_mut().unwrap());
    });
    assert!(ui.any_captured());
    ui.modal_capture_count = 1;
    ui.update_all(0.0);
    assert!(taken(&log).is_empty());
    ui.modal_capture_count = 0;
    ui.update_all(0.0);
    assert_eq!(taken(&log), vec!["a:update"]);
}

#[test]
fn inactive_children_update_inactive() {
    let mut ui = ui();
    let log = Log::default();
    let root = make(&mut ui, Probe::boxed("root", &log, (0.0, 0.0)), None);
    let a = make(&mut ui, Probe::boxed("a", &log, (0.0, 0.0)), Some(root));
    let _b = make(&mut ui, Probe::boxed("b", &log, (0.0, 0.0)), Some(root));
    ui.node_mut(a).unwrap().flags &= !ACTIVE;
    ui.cursor_drawn = false;
    ui.update_all(0.0);
    assert_eq!(taken(&log), vec!["root:update", "a:inactive", "b:update"]);
}

#[test]
fn roots_draw_by_layer_z_and_sub_layer() {
    let mut ui = ui();
    let log = Log::default();
    let setup = |ui: &mut Ui, tag, layer, z, sub| {
        let id = make(ui, Probe::boxed(tag, &log, (0.0, 0.0)), None);
        let n = ui.node_mut(id).unwrap();
        n.draw_layer = layer;
        n.position.z = z;
        n.draw_sub_layer = sub;
        id
    };
    setup(&mut ui, "late", 3, 0.0, 0);
    setup(&mut ui, "now", 0, 0.0, 5);
    setup(&mut ui, "far", 2, -1.0, 0);
    setup(&mut ui, "near", 2, 1.0, 0);
    setup(&mut ui, "sub1", 1, 0.0, 1);
    let hidden = setup(&mut ui, "hidden", 1, 0.0, 0);
    ui.node_mut(hidden).unwrap().flags &= !VISIBLE;
    let mut host = QuadHost::default();
    ui.draw_all(&mut host);
    assert_eq!(
        taken(&log),
        vec![
            "now:draw",
            "far:draw",
            "near:draw",
            "late:draw",
            "sub1:draw"
        ]
    );
}

#[test]
fn broadcasts_show_and_hide_controls() {
    let mut ui = ui();
    let parent = make(&mut ui, Box::new(PlainControl::default()), None);
    let child = make(&mut ui, Box::new(PlainControl::default()), Some(parent));
    ui.set_position(parent, Vector::new(1.0, 2.0, 3.0));
    assert_eq!(ui.node(child).unwrap().position, Vector::new(1.0, 2.0, 3.0));
    // The parent takes the broadcast first.
    assert!(ui.broadcast(MSG_HIDE, 0, 0));
    assert_eq!(ui.flags(parent) & VISIBLE, 0);
    assert_ne!(ui.flags(child) & VISIBLE, 0);
    assert!(ui.send_broadcast(child, MSG_HIDE, 0, 0));
    assert!(ui.send_broadcast(parent, MSG_SHOW, 0, 0));
    assert_ne!(ui.flags(parent) & VISIBLE, 0);
    assert!(!ui.broadcast(0x1234, 0, 0));
}

#[test]
fn control_hit_boxes_and_tooltips() {
    let half = Vector::new(0.1, 0.05, 0.0);
    let at = Transform::new(Matrix::IDENTITY, Vector::new(0.0, 0.0, -1.0));
    assert!(ray_hits_box(half, &at, Vector::new(0.05, 0.0, -1.0)));
    assert!(!ray_hits_box(half, &at, Vector::new(0.2, 0.0, -1.0)));
    // Strict inside: the edge misses.
    assert!(!ray_hits_box(half, &at, Vector::new(0.1, 0.0, -1.0)));
    assert!(!ray_hits_box(
        Vector::ZERO,
        &at,
        Vector::new(0.0, 0.0, -1.0)
    ));

    let mut ui = ui();
    let id = make(&mut ui, Box::new(PlainControl::default()), None);
    {
        let c = ui.win_control_mut(id).unwrap();
        c.half_extents = half;
        c.offset = Vector::new(0.0, 0.0, -1.0);
        c.tooltip_ids = 77;
    }
    // The centre pixel looks along -z.
    ui.handle(Input::Move { x: 400, y: 300 });
    assert_eq!(ui.hover(), Some(id));
    let requests = ui.take_tooltip_requests();
    assert_eq!(requests, vec![ToolTipRequest { owner: id, ids: 77 }]);
    assert!(ui.handle(Input::Button(MouseButton::LEFT_DOWN)));
}

#[test]
fn base_frame_ini_builds_children_through_the_factory() {
    let ini = b"[BaseFrame]\nbehavior = \nmesh = \noffset = 0, 0.349999994, -1\n\
mouse_size = 0.1, 0.2, 0\nno_z_enable = 1\nz_order = 3\ntooltip = 12\nsubcontrol = A\n\n\
[NavBar]\nEquipment = x.3db\n\n[ChildOne]\nclass_name = WinControl\n\n\
[ChildTwo]\nfoo = 1\nclass_name = WinControl\n\n[Unknown]\nclass_name = NoSuchClass\n";
    let mut ui = ui();
    let id = make(&mut ui, Box::new(PlainControl::default()), None);
    let mut reader = neuron_core::ini::IniReader::new();
    assert!(reader.open_memory(ini));
    load_ini(&mut ui, id, &mut reader);
    let c = ui.win_control(id).unwrap();
    assert_eq!(
        c.offset,
        Vector::new(0.0, f32::from_bits(0x3EB3_3333), -1.0)
    );
    assert_eq!(c.half_extents, Vector::new(0.1, 0.2, 0.0));
    assert!(c.no_z_enable);
    assert_eq!(c.tooltip_ids, 12);
    assert_eq!(ui.node(id).unwrap().draw_layer, 3);
    let names: Vec<Vec<u8>> = ui
        .children(id)
        .iter()
        .map(|&c| ui.node(c).unwrap().name().to_vec())
        .collect();
    assert_eq!(names, vec![b"ChildOne".to_vec(), b"ChildTwo".to_vec()]);
    let child = ui.children(id)[0];
    assert_eq!(ui.node(child).unwrap().class_name, Some("WinControl"));
    assert_eq!(
        crate::ui::control::find_child(&ui, id, b"ChildTwo"),
        Some(ui.children(id)[1])
    );
    // Deleting the control deletes its children.
    ui.delete(id);
    assert_eq!(ui.live_count(), 0);
}

#[test]
fn a_master_window_places_its_page_in_the_height_frame() {
    let desc = PageDesc::load(
        b"UIStaticText Title -0.5 0.375 1.0 0.05 \"Hello\"\nUIStaticText Other 0 0 0.5 0.1\n",
    );
    assert!(desc.ok);
    let mut ui = Ui::new(Surface::new(1024, 768));
    let frame = page_frame(&ui);
    let id = MasterWin::open(&mut ui, &desc).unwrap();
    let master = ui.widget::<MasterWin>(id).unwrap();
    let page = master.page.as_ref().unwrap();
    assert_eq!(page.count(), 2);
    assert_eq!(page.widget_name(0), Some(&b"Other"[..]));
    let title = page.find(b"Title").unwrap();
    // The frame scales by the visible height at the near plane.
    let h = frame.rotation.m[0][0];
    assert!(h > 0.0 && frame.rotation.m[1][1] == h);
    assert!((frame.translation.z + 0.1).abs() < 1e-6);
    let n = ui.node(title).unwrap();
    assert_eq!(n.position.x, (-0.5f64 * f64::from(h)) as f32);
    assert_eq!(n.parent(), Some(id));
    let text = ui
        .widget::<crate::ui::controls::text_win::UiStaticText>(title)
        .unwrap();
    assert_eq!(
        text.text_win.text.text(),
        Some(&"Hello".encode_utf16().collect::<Vec<_>>()[..])
    );
    assert_eq!(text.text_win.width, (f64::from(h) * 1.0) as f32);
    // The page's top edge is the top of the surface: y 0.375 * h is near
    // the top pixel row of a 4:3 surface (h / 2 above the centre).
    let (_, y, _) = ui.camera.project(n.position).unwrap();
    assert!(y > 0.0 && y < 100.0);
    // Destroying the window deletes the page's widgets.
    ui.destroy(id);
    ui.delete(id);
    assert_eq!(ui.live_count(), 0);
}

#[test]
fn edge_anchors_shift_the_hit_tests_of_their_windows() {
    let mut ui = Ui::new(Surface::new(1366, 768));
    let id = make(&mut ui, Box::new(PlainControl::default()), None);
    {
        let c = ui.win_control_mut(id).unwrap();
        c.half_extents = Vector::new(0.005, 0.005, 0.0);
        c.offset = Vector::new(0.0, 0.0, -1.0);
    }
    // Without an anchor the control sits in the middle.
    ui.handle(Input::Move { x: 683, y: 384 });
    assert_eq!(ui.hover(), Some(id));
    // Anchored to the right edge it is drawn 171 pixels to the right and
    // picked there.
    ui.node_mut(id).unwrap().anchor = Anchor::Edge(1.0);
    assert_eq!(ui.shift_pixels_for(id), 171.0);
    ui.handle(Input::Move { x: 683, y: 384 });
    assert_eq!(ui.hover(), None);
    ui.handle(Input::Move {
        x: 683 + 171,
        y: 384,
    });
    assert_eq!(ui.hover(), Some(id));
    // A centred child of an anchored window stays in the middle.
    let child = make(&mut ui, Box::new(PlainControl::default()), Some(id));
    ui.node_mut(child).unwrap().anchor = Anchor::Centre;
    assert_eq!(ui.shift_pixels_for(child), 0.0);
    assert_eq!(ui.current_shift(), 0.0);
}
