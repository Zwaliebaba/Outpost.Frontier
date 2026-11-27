//! `HUD_ContactList` (`hud_contact_list.cpp`, the `[ContactList]` section
//! of `hud.ini`): the list the target panel shows open in its modes 0 (the
//! contacts) and 2 (the formation), up to eight rows of icon, range and
//! name with a scroll bar, five filter buttons and the formation list's
//! title.
//!
//! The port so far: the construction (`HUD_ContactList::HUD_ContactList`
//! and `Create`) and the window's update (`Update`). The list follows the
//! target panel's mode in the HUD's render (`Render`) and fills its rows
//! in the draw (`Draw`, `RebuildContacts`); those, the clicks
//! (`SetProperty`), the events (`OnEvent`) and the update while hidden
//! (`UpdateInactive`) need the client's objects and come with them
//! (divergence D1126). Until they run, the window shows no row, which is
//! the reference's list before its first draw.

use neuron_client::ui::children::{
    ButtonInit, TextInit, create_button_control, create_text_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::controls::scroll::UiScroll;
use neuron_client::ui::controls::text_control::TextControlWidget;
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::page::EntryPayload;
use neuron_client::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::HudItem;

/// The factory's section and the element's name.
pub const SECTION: &[u8] = b"ContactList";
/// The window's name and class name.
pub const NAME: &[u8] = b"HUD_ContactList";
/// The events the window registers: the next and the previous contact.
pub const EVENTS: [u32; 2] = [0x1d, 0x1e];
/// The rows.
pub const ROWS: usize = 8;

/// `kRebuildSeconds`.
const REBUILD_SECONDS: f32 = 3.0;
/// `kLabelScale`, the name text's width.
const LABEL_SCALE: f32 = 0.16;
/// `kRowStep`.
const ROW_STEP: f32 = 0.018;
/// `kRowTop` (0xbb031268).
const ROW_TOP: f32 = f32::from_bits(0xbb03_1268);
/// `kRangeRowOffset`, `kNameRowOffset`.
const RANGE_ROW_OFFSET: f32 = 0.0075;
const NAME_ROW_OFFSET: f32 = 0.015;
/// `kIconRowTop` (0x3a831270).
const ICON_ROW_TOP: f32 = f32::from_bits(0x3a83_1270);

/// A double expression stored to a `float` (T11).
#[allow(clippy::cast_possible_truncation)]
fn r(v: f64) -> f32 {
    v as f32
}

/// The filter buttons: name, mesh, x and tool tip, in `Create`'s order.
const FILTERS: [(&[u8], &[u8], f32, i32); 5] = [
    (
        b"ImportantFilterButton",
        b"interface\\HUD\\HUD_contactimportant.3db",
        -0.128,
        0x623,
    ),
    (
        b"ShipsFilterButton",
        b"interface\\HUD\\HUD_contactship.3db",
        -0.094,
        0x4fc,
    ),
    (
        b"SolarFilterButton",
        b"interface\\HUD\\HUD_contactstation.3db",
        -0.06,
        0x4fd,
    ),
    (
        b"LootFilterButton",
        b"interface\\HUD\\HUD_contactloot.3db",
        -0.026,
        0x520,
    ),
    (
        b"AllFilterButton",
        b"interface\\HUD\\HUD_contactall.3db",
        0.008,
        0x624,
    ),
];

/// The children `Create` makes.
#[derive(Clone, Copy, Debug, Default)]
pub struct Controls {
    /// "FormationTitle".
    pub formation_title: Option<NodeId>,
    /// "ContactListRangeText%d".
    pub range_texts: [Option<NodeId>; ROWS],
    /// "ContactListNameText%d".
    pub name_texts: [Option<NodeId>; ROWS],
    /// "ContactListIcon%d".
    pub icons: [Option<NodeId>; ROWS],
    /// The important, ships, solar, loot and all filter buttons.
    pub filters: [Option<NodeId>; 5],
    /// "ContactListScroll".
    pub scroll: Option<NodeId>,
}

/// The window (`HUD_ContactList`'s `WinControl` face).
#[derive(Default)]
pub struct HudContactList {
    pub control: WinControl,
    pub c: Controls,
    /// `ScrollPos`: the first visible row.
    pub scroll_pos: u32,
    /// `RebuildTimer`.
    pub rebuild_timer: f32,
    /// `FilterMode`: 0 important, 1 ships, 2 solars, 3 loot, 4 all.
    pub filter_mode: i32,
    /// `bDirty`.
    pub dirty: bool,
    /// `LastPanelMode`: the target panel's mode the list last followed.
    pub last_panel_mode: i32,
    /// `NameRefreshTimer`.
    pub name_refresh_timer: f32,
}

fn text_init(style: &'static [u8], position: Vector, width: f32, height: f32) -> TextInit<'static> {
    TextInit {
        style: Some(style),
        position,
        width,
        height,
        ..TextInit::default()
    }
}

/// `WinControl::CreateUIScroll`: the payload (the sizes, the range and
/// page as floats, a step of 1), then the scroll offset, the value 0 and
/// the direction.
#[allow(clippy::too_many_arguments, clippy::cast_precision_loss)]
fn create_ui_scroll(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    position: Vector,
    width: f32,
    height: f32,
    min: i32,
    max: i32,
    page: i32,
    horizontal: bool,
) -> Option<NodeId> {
    let payload = EntryPayload::ScrollCycle {
        right: width,
        bottom: height,
        extra: [min as f32, max as f32, page as f32, 1.0],
        flag: horizontal,
    };
    let id = ui.create_by_class(
        Some(name),
        b"UIScroll",
        Some(parent),
        &mut Payload::Page(&payload),
    )?;
    ui.with_widget(id, (), |w, ui| {
        if let Some(s) = w.as_any_mut().downcast_mut::<UiScroll>() {
            s.set_scroll_offset(ui, id, position);
            s.set_value(0.0);
            s.horizontal = horizontal;
        }
    });
    Some(id)
}

fn row_name(prefix: &str, index: usize) -> Vec<u8> {
    format!("{prefix}{index}").into_bytes()
}

impl Controls {
    /// `HUD_ContactList::Create`'s children, in its order.
    fn create(ui: &mut Ui, id: NodeId) -> Self {
        // The formation list's title, hidden.
        let mut c = Controls {
            formation_title: create_text_control(
                ui,
                id,
                b"FormationTitle",
                &TextInit {
                    ids: 0x60d,
                    justification: 1,
                    corner_anchor: true,
                    ..text_init(b"PageHeader", Vector::new(-0.189, 0.043, 0.0), 0.269, 0.015)
                },
            ),
            ..Controls::default()
        };
        if let Some(n) = c.formation_title.and_then(|t| ui.node_mut(t)) {
            n.flags &= 0xfc;
        }

        for (index, row) in (0u8..).zip(0..ROWS) {
            // FILD / FMUL / FSUBR: the row's top, kept unrounded for the
            // range text and stored as a float for the name text.
            let top = f64::from(ROW_TOP) - f64::from(index) * f64::from(ROW_STEP);
            let range_pos = Vector::new(-0.126, r(top + f64::from(RANGE_ROW_OFFSET)), 0.0);
            let range = create_text_control(
                ui,
                id,
                &row_name("ContactListRangeText", row),
                &TextInit {
                    justification: 1,
                    ..text_init(b"HudSmall", range_pos, 0.043, 0.015)
                },
            );
            if let Some(range) = range {
                ui.property(range, Property::Other(0x24, 1, 0));
            }
            c.range_texts[row] = range;

            let name_pos = Vector::new(
                -0.107,
                r(f64::from(r(top)) + f64::from(NAME_ROW_OFFSET)),
                0.0,
            );
            let name = create_text_control(
                ui,
                id,
                &row_name("ContactListNameText", row),
                &TextInit {
                    corner_anchor: true,
                    ..text_init(b"HudSmall", name_pos, LABEL_SCALE, ROW_STEP)
                },
            );
            if let Some(name) = name {
                if let Some(t) = ui.widget_mut::<TextControlWidget>(name) {
                    t.text.control.home_offset = name_pos;
                }
                ui.property(name, Property::Other(0x24, 1, 0));
                if let Some(t) = ui.widget_mut::<TextControlWidget>(name) {
                    t.text.text.set_max_lines(1);
                }
            }
            c.name_texts[row] = name;
        }

        let icon_half = Vector::new(0.006, 0.006, 0.0);
        for (index, row) in (0u8..).zip(0..ROWS) {
            let y = r(f64::from(ICON_ROW_TOP) - f64::from(index) * f64::from(ROW_STEP));
            c.icons[row] = create_button_control(
                ui,
                id,
                &row_name("ContactListIcon", row),
                Some(b"Interface\\hud\\HUD_contactwaypoint.3db"),
                &ButtonInit {
                    sound: Some(b"ui_select_item"),
                    position: Vector::new(-0.17, y, 0.0),
                    half_extents: icon_half,
                    action_button: 1,
                    tooltip: 1,
                    ..ButtonInit::default()
                },
            );
        }

        let filter_half = Vector::new(0.014, 0.014, 0.0);
        for (slot, (name, mesh, x, tooltip)) in c.filters.iter_mut().zip(FILTERS) {
            *slot = create_button_control(
                ui,
                id,
                name,
                Some(mesh),
                &ButtonInit {
                    sound: Some(b"ui_filter_select"),
                    position: Vector::new(x, 0.024, 0.0),
                    half_extents: filter_half,
                    action_button: 1,
                    tooltip,
                    ..ButtonInit::default()
                },
            );
        }
        // The "all" button is hidden in single player (`SinglePlayer()`, the
        // port's only client yet); `Create` hides every child after it
        // either way.
        if let Some(all) = c.filters[4] {
            ui.property(all, Property::Hidden(true));
        }

        c.scroll = create_ui_scroll(
            ui,
            id,
            b"ContactListScroll",
            Vector::new(-0.267, -0.234, -1.0),
            0.016,
            0.118,
            0,
            5,
            8,
            false,
        );
        c
    }
}

impl Widget for HudContactList {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.control)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        base::hit_test(ui, id, &self.control, ray)
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        base::set_position(ui, id, &mut self.control, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        base::set_orientation(ui, id, &mut self.control, orientation);
    }
    /// `HUD_ContactList::Create`: the icon meshes preloaded (the model
    /// cache's, which the port's mesh loader keeps itself), the title, the
    /// rows, the filter buttons and the scroll bar, the offset (-0.323,
    /// -0.241, -1), the position again, every child hidden, the two events,
    /// then `Win3D::Create`. The icons' montage tiles are the draw's.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        let mut made = Controls::default();
        ui.lend(id, self, |ui| made = Controls::create(ui, id));
        self.c = made;
        base::set_offset(ui, id, self, Vector::new(-0.323, -0.241, -1.0));
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        base::set_position(ui, id, &mut self.control, position);
        base::set_hidden_with_children(ui, id, true);
        for event in EVENTS {
            ui.services.register_event(id, event);
        }
        ui.base_create(id, parent)
    }
    /// `HUD_ContactList::Update`: `Win3D`'s child walk unless the mouse
    /// pick is blocked (`HudIsMousePickBlocked`, which reads the player's
    /// object and the NeuroNet screens: not blocked in the port yet,
    /// D1126).
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.control);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ui.draw_children(id, host);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        base::mouse_event(ui, id, self, event)
    }
    /// `HUD_ContactList::SetProperty` takes only the clicks, the double
    /// clicks and the scroll bar's moves (D1126); nothing reaches
    /// `WinControl`'s handler.
    fn property(&mut self, _ui: &mut Ui, _id: NodeId, _property: Property<'_>) -> isize {
        0
    }
}

/// The element (`HUD_ContactList`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudContactListItem {
    pub window: NodeId,
}

impl HudItem for HudContactListItem {
    fn name(&self) -> &[u8] {
        SECTION
    }
    fn type_name(&self) -> &'static str {
        "class HUD_ContactList"
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// `HUDFACTORY<HUD_ContactList>::Create`: the constructor, which makes the
/// window (`Create(0, 0)`), names it "HUD_ContactList" and sets the list's
/// state. The player ship's scanner and the two fonts it looks up are the
/// draw's.
pub fn create(_reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let id = ui.alloc(Some(Box::new(HudContactList::default())));
    ui.with_widget(id, false, |w, ui| {
        w.create(ui, id, None, &mut Payload::None)
    });
    if let Some(n) = ui.node_mut(id) {
        n.set_name(Some(NAME));
        n.class_name = Some("HUD_ContactList");
    }
    if let Some(list) = ui.widget_mut::<HudContactList>(id) {
        list.scroll_pos = 0;
        list.rebuild_timer = REBUILD_SECONDS;
        list.filter_mode = 0;
        list.dirty = false;
        list.last_panel_mode = -1;
        list.name_refresh_timer = 0.0;
    }
    Some(Box::new(HudContactListItem { window: id }))
}

#[cfg(test)]
mod tests {
    use neuron_client::ui::tree::{ACTIVE, VISIBLE};
    use neuron_client::ui::widescreen::Surface;

    use super::*;

    #[test]
    fn the_window_is_shown_with_every_child_hidden() {
        let mut ui = Ui::new(Surface::new(1024, 768));
        let mut reader = IniReader::new();
        let item = create(&mut reader, &mut ui).expect("the element");
        assert_eq!(item.name(), b"ContactList");
        let id = item.window().unwrap();
        let node = ui.node(id).unwrap();
        assert_eq!(node.name(), b"HUD_ContactList");
        assert_eq!(node.flags & (ACTIVE | VISIBLE), ACTIVE | VISIBLE);
        let children = ui.children(id);
        assert_eq!(children.len(), 1 + 2 * ROWS + ROWS + 5 + 1);
        for child in children {
            assert_eq!(ui.node(child).unwrap().flags & (ACTIVE | VISIBLE), 0);
        }
        let list = ui.widget_mut::<HudContactList>(id).unwrap();
        assert_eq!(list.last_panel_mode, -1);
        assert_eq!(list.rebuild_timer, 3.0);
        let names: Vec<Vec<u8>> = ui
            .children(id)
            .iter()
            .map(|&c| ui.node(c).unwrap().name().to_vec())
            .collect();
        assert_eq!(names[0], b"FormationTitle");
        assert_eq!(names[1], b"ContactListRangeText0");
        assert_eq!(names[2], b"ContactListNameText0");
        assert_eq!(names[17], b"ContactListIcon0");
        assert_eq!(names[25], b"ImportantFilterButton");
        assert_eq!(names[30], b"ContactListScroll");
    }

    #[test]
    fn the_rows_step_down_from_the_top() {
        let mut ui = Ui::new(Surface::new(1024, 768));
        let mut reader = IniReader::new();
        let id = create(&mut reader, &mut ui).unwrap().window().unwrap();
        let c = ui.widget_mut::<HudContactList>(id).unwrap().c;
        let offset = |ui: &Ui, n: Option<NodeId>| ui.win_control(n.unwrap()).unwrap().offset;
        // The dump's bits: 0x3bb4395b, 0xbc4cccca; 0x3c54fdf5; 0x3a831270.
        assert_eq!(offset(&ui, c.range_texts[0]).y.to_bits(), 0x3bb4_395b);
        assert_eq!(offset(&ui, c.range_texts[1]).y.to_bits(), 0xbc4c_ccca);
        assert_eq!(offset(&ui, c.name_texts[0]).y.to_bits(), 0x3c54_fdf5);
        assert_eq!(offset(&ui, c.icons[0]).y.to_bits(), 0x3a83_1270);
        assert_eq!(offset(&ui, c.icons[7]).y.to_bits(), 0xbdff_ffff);
    }
}
