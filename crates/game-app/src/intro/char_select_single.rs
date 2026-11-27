//! The single-player Load Game page (`page_character_select_single.cpp`,
//! menu state 18): `PageCharacterSelectSingle` over the page file
//! `Interface\intro\PAGE_CharSelectSingle.page`, and `EnterMenuState`
//! case 18's chrome (`menu_enter_state.cpp`, `EnterCase18Chrome` with
//! `kCase18Statics`): the three "bImg" button meshes, "bigBox" and
//! "smallBox", made under the page before its file loads.
//!
//! The page slides in at 1.3 per second (the frame time capped at 0.5 s):
//! the meshes and the two boxes move, and once the slide passes 0 every
//! widget is shown, the meshes are hidden and the slide parks at 2. A click
//! on Load Game or Main Menu (or a double click on a row) hides the widgets
//! again and slides out at 1.5 per second; at its end Main Menu sets the
//! result 1 and Load Game asks the menu to leave the intro base, after which
//! the selected row's character becomes the menu's character and the result
//! 0xc.
//!
//! The roster (`G_LoginMgr`: `ReloadFactions`, `RequestCharacterInfo`,
//! `Characters`, `CharacterAt`, the records' `bIsRestart`) is the caller's:
//! it sets [`PageCharacterSelectSingle::roster`] before `Create` with the
//! status `RequestCharacterInfo` answered and one [`RosterRow`] per record,
//! its name as `NnLoadSaveEntryText` gives it, its time as
//! `filetime_to_wstring` of the record's time stamp, and the details
//! `ShowDetails` shows (the ship's name through `Archetype::GetShip` and its
//! `ids_name`, the system's through `Universe::get_system` and its
//! `strid_name`, "IDS?" when that text is missing). `ReloadFactions` is the
//! caller's too, before it asks for the roster.
//!
//! Not ported yet: Delete Game's confirmation (`NnConfirmOpenFormatted`, a
//! modal NeuroNet dialog) and with it the delete (`DestroyCharacter`,
//! `NnErrorRunModal`); the first press is answered as a declined dialog
//! (D1103).

use std::rc::Rc;

use neuron_client::text::rdl::{RdlNode, RenderDisplayList, append_plain_text, load_ids};
use neuron_client::ui::children::{StaticInit, init_static_control};
use neuron_client::ui::controls::list_box::UiListBox;
use neuron_client::ui::controls::scroll::UiScroll;
use neuron_client::ui::controls::ui_rich_text::{UiRichTextWin, UiTextMsgButton};
use neuron_client::ui::page::{MasterWin, PageDesc};
use neuron_client::ui::pulse::HUD_CHROME_RGBA;
use neuron_client::ui::tree::{
    ACTIVE, ChildMessage, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget,
};
use neuron_core::math::Vector;
use neuron_core::num::trunc_to_i32;

use super::type_select::BUTTON_MESH;

/// `kPageCharSelectSingle`.
pub const PAGE_FILE: &[u8] = b"Interface\\intro\\PAGE_CharSelectSingle.page";
/// `kShortOptionsBox`.
pub const SHORT_OPTIONS_BOX: &[u8] = b"Interface\\NeuroNet\\Inventory\\FRONT_shortoptions_box.cmp";
/// `kInfoWindowMesh`.
pub const INFO_WINDOW_MESH: &[u8] = b"Interface\\LoadSave\\FRONT_infowindow.cmp";

/// `kCase18Statics`: nickname, mesh, offset.
const CASE18_STATICS: [(&[u8], &[u8], [f32; 3]); 5] = [
    (b"bImg", BUTTON_MESH, [-0.348, 0.195, -1.0]),
    (b"bImg", BUTTON_MESH, [-0.349, 0.1105, -1.0]),
    (b"bImg", BUTTON_MESH, [-0.349, 0.03, -1.0]),
    (b"bigBox", SHORT_OPTIONS_BOX, [0.02, -0.02, -0.98]),
    (b"smallBox", INFO_WINDOW_MESH, [-0.35, -0.197, -1.0]),
];

/// The click message of the page's buttons.
pub const MSG_CLICK: u32 = 0x3004;
/// The confirm box's answer.
pub const MSG_CONFIRM_ANSWER: u32 = 0x3006;
/// Load Game's, Delete Game's and Main Menu's commands.
pub const CMD_LOAD: u32 = 0x3003;
pub const CMD_DELETE: u32 = 0x3002;
pub const CMD_MAIN_MENU: u32 = 0x300c;
/// The confirm box's "no".
const CMD_DECLINED: u32 = 0xf003;
/// `USER_CANCEL`'s HUD event.
pub const EVENT_CANCEL: u32 = 0xf;
/// `ui_motion_swish`'s UI sound slot.
const SWISH_SLOT: u32 = 0x1b;
/// The rows a list shows.
const VISIBLE_ROWS: i32 = 0x14;
/// The style of the labels and values, and of the title.
const VALUE_STYLE: u16 = 0x5724;
const TITLE_STYLE: u16 = 0x1df5;
/// The captions' string ids: money, rank, location, ship, the title.
const MONEY_IDS: u32 = 0x594;
const RANK_IDS: u32 = 0x5ec;
const LOCATION_IDS: u32 = 0x593;
const SHIP_IDS: u32 = 0x591;
const TITLE_IDS: u32 = 0x4f5;

/// What `ShowDetails` shows of a record.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RowDetails {
    /// `+0x234`.
    pub money: i32,
    /// `+0x270`.
    pub rank: i32,
    /// The ship's name, `None` without a ship archetype that resolves
    /// (empty when its `ids_name` has no text).
    pub ship_name: Option<Vec<u16>>,
    /// The system's name, `None` without a system ("IDS?" when its
    /// `strid_name` has no text).
    pub location_name: Option<Vec<u16>>,
}

/// One record of the roster (`CCharacterSet::CharacterInfo`, the parts
/// the page reads).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RosterRow {
    /// `NnLoadSaveEntryText(Info)`.
    pub name: Vec<u16>,
    /// `filetime_to_wstring` of the record's time stamp.
    pub time: Vec<u16>,
    /// The rows' data word (the reference's record pointer): not 0, and
    /// the caller's way back to the record.
    pub data: i32,
    /// `+0x200`: the restart save, left out of the lists.
    pub is_restart: bool,
    pub details: RowDetails,
}

/// The roster as `G_LoginMgr` gives it to `Create`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Roster {
    /// `RequestCharacterInfo`'s answer: only 1 fills the lists.
    pub status: i32,
    /// `Characters`, in order.
    pub rows: Vec<RosterRow>,
}

/// `PageCharacterSelectSingle`.
#[derive(Default)]
pub struct PageCharacterSelectSingle {
    /// The `UIPage::MasterWin` part, over the descriptor (`+0x8c`).
    pub master: MasterWin,
    /// `+0xb4`: the menu state next, 0 for none.
    pub result: i32,
    pub character_money: Option<NodeId>,
    pub character_rank: Option<NodeId>,
    pub character_location: Option<NodeId>,
    pub character_ship: Option<NodeId>,
    pub load_character: Option<NodeId>,
    pub delete_character: Option<NodeId>,
    pub main_menu: Option<NodeId>,
    pub char_scroll: Option<NodeId>,
    pub char_sel_desc: Option<NodeId>,
    pub char_sel_time: Option<NodeId>,
    pub money_label: Option<NodeId>,
    pub rank_label: Option<NodeId>,
    pub location_label: Option<NodeId>,
    pub ship_label: Option<NodeId>,
    pub screen_title: Option<NodeId>,
    /// `+0xf4`: the three "bImg" statics.
    pub button_images: [Option<NodeId>; 3],
    /// `+0x100`, `+0x104`.
    pub big_box: Option<NodeId>,
    pub small_box: Option<NodeId>,
    /// `+0x108`: 0 to 2 sliding in, 0 to -0.5 sliding out.
    pub slide_t: f64,
    /// `+0x110`.
    pub leaving: bool,
    /// `+0x114`: 1 load, 0 main menu.
    pub leave_direction: i32,
    /// `+0x118`: the chosen row's data word, for the menu.
    pub selected_character: i32,
    /// `+0x318`: the delete confirm box is open.
    pub delete_pending: bool,
    /// `+0x319`.
    pub character_chosen: bool,
    /// `+0x31c` to `+0x34c`: the captions.
    pub money_label_text: Rc<RenderDisplayList>,
    pub rank_label_text: Rc<RenderDisplayList>,
    pub location_label_text: Rc<RenderDisplayList>,
    pub ship_label_text: Rc<RenderDisplayList>,
    pub title_text: Rc<RenderDisplayList>,
    /// `+0x358`: the rows in the lists.
    pub entries: Vec<RosterRow>,
    /// `+0x368`: the menu's poll reads it.
    pub load_requested: bool,
    /// `+0x369`: the menu's poll sets it.
    pub intro_base_left: bool,
    /// `LoadingScreenSetClear(1)` and `LoadingScreenStart` asked for by the
    /// load; the menu's poll hands them on.
    pub loading_screen_requested: bool,
    /// The roster `Create` reads (`G_LoginMgr`'s, see the module).
    pub roster: Roster,
}

fn hide(ui: &mut Ui, id: Option<NodeId>) {
    if let Some(n) = id.and_then(|id| ui.node_mut(id)) {
        n.flags &= !(ACTIVE | VISIBLE);
    }
}

fn show(ui: &mut Ui, id: Option<NodeId>) {
    if let Some(n) = id.and_then(|id| ui.node_mut(id)) {
        n.flags |= ACTIVE | VISIBLE;
    }
}

fn clear_active(ui: &mut Ui, id: Option<NodeId>) {
    if let Some(n) = id.and_then(|id| ui.node_mut(id)) {
        n.flags &= !ACTIVE;
    }
}

fn set_active(ui: &mut Ui, id: Option<NodeId>) {
    if let Some(n) = id.and_then(|id| ui.node_mut(id)) {
        n.flags |= ACTIVE;
    }
}

fn rich<R>(ui: &mut Ui, id: Option<NodeId>, f: impl FnOnce(&mut UiRichTextWin) -> R) -> Option<R> {
    id.and_then(|id| ui.widget_mut::<UiRichTextWin>(id)).map(f)
}

fn button<R>(
    ui: &mut Ui,
    id: Option<NodeId>,
    f: impl FnOnce(&mut UiTextMsgButton) -> R,
) -> Option<R> {
    id.and_then(|id| ui.widget_mut::<UiTextMsgButton>(id))
        .map(f)
}

fn list<R>(ui: &mut Ui, id: Option<NodeId>, f: impl FnOnce(&mut UiListBox) -> R) -> Option<R> {
    id.and_then(|id| ui.widget_mut::<UiListBox>(id)).map(f)
}

fn scroll<R>(ui: &mut Ui, id: Option<NodeId>, f: impl FnOnce(&mut UiScroll) -> R) -> Option<R> {
    id.and_then(|id| ui.widget_mut::<UiScroll>(id)).map(f)
}

/// `ListBox::AddItem` on a list of the page.
fn add_item(ui: &mut Ui, id: Option<NodeId>, text: &[u16], data: i32) {
    let Some(id) = id else { return };
    ui.with_widget(id, (), |w, ui| {
        if let Some(l) = w.as_any_mut().downcast_mut::<UiListBox>() {
            l.list.add_item(ui, text, data);
        }
    });
}

fn set_interactive(ui: &mut Ui, id: Option<NodeId>, on: bool) {
    button(ui, id, |b| b.interactive = on);
}

/// A value window emptied, the style, then `text` (when given).
fn set_value(ui: &mut Ui, id: Option<NodeId>, text: Option<&RenderDisplayList>) {
    rich(ui, id, |w| {
        w.win.clear_content();
        if let Some(text) = text {
            w.win.append_node(&RdlNode::Style(VALUE_STYLE));
            w.win.append_content(text);
        }
    });
}

/// `AppendTextAsRdl` of `text` into a new list.
fn plain(text: &[u16]) -> RenderDisplayList {
    let mut list = RenderDisplayList::new();
    append_plain_text(&mut list, text, false);
    list
}

/// `L"-"` through `AppendTextMaybeXmlRdl`.
fn dash() -> RenderDisplayList {
    let mut list = RenderDisplayList::new();
    neuron_client::text::rdl::append_text_maybe_markup(&mut list, &[u16::from(b'-')], false);
    list
}

/// `IntroPageUtil::LoadIds`: a new list with the text of `ids`.
fn ids_list(ui: &Ui, ids: u32) -> Rc<RenderDisplayList> {
    let mut list = RenderDisplayList::new();
    if let Some(c) = ui.catalogs.clone() {
        load_ids(&mut list, &c, ids);
    }
    Rc::new(list)
}

/// `(int)` of a float by `ftol` (truncation).
fn ftol(value: f32) -> i32 {
    trunc_to_i32(f64::from(value))
}

/// `G_HudChromeColorRgba` read as a dword.
const CHROME_COLOR: u32 = u32::from_le_bytes(HUD_CHROME_RGBA);

/// `PlaceButtonImages`: image `i` at `{V, 0, 0}` with
/// `V = (2 - i) * 0.1 + slide`, or at the origin once `V` is above 0.
fn place_button_images(ui: &mut Ui, images: &[Option<NodeId>; 3], slide_t: f64) {
    for (index, image) in images.iter().enumerate() {
        let row = 2 - i32::try_from(index).unwrap_or(0);
        let v = f64::from(row) * 0.1 + slide_t;
        let position = if v > 0.0 {
            Vector::ZERO
        } else {
            Vector::new(v as f32, 0.0, 0.0)
        };
        if let Some(id) = image {
            ui.set_position(*id, position);
        }
    }
}

impl PageCharacterSelectSingle {
    /// The constructor: the slide at 1, no direction.
    #[must_use]
    pub fn new() -> Self {
        PageCharacterSelectSingle {
            slide_t: 1.0,
            leave_direction: -1,
            ..Self::default()
        }
    }

    /// The fifteen page widgets, in `Create`'s and `BeginLeave`'s order.
    fn widgets(&self) -> [Option<NodeId>; 15] {
        [
            self.character_money,
            self.character_rank,
            self.character_location,
            self.character_ship,
            self.load_character,
            self.delete_character,
            self.main_menu,
            self.char_scroll,
            self.char_sel_desc,
            self.char_sel_time,
            self.money_label,
            self.rank_label,
            self.location_label,
            self.ship_label,
            self.screen_title,
        ]
    }

    fn desc_count(&self, ui: &mut Ui) -> i32 {
        list(ui, self.char_sel_desc, |l| l.list.item_count).unwrap_or(0)
    }

    /// The row's entry by its data word (`GetItemData`: 0 for none).
    fn entry_of(&self, ui: &mut Ui, row: i32) -> Option<RosterRow> {
        let data = list(ui, self.char_sel_desc, |l| l.list.item_data(row)).unwrap_or(0);
        if data == 0 {
            return None;
        }
        self.entries.iter().find(|e| e.data == data).cloned()
    }

    /// `UpdateScrollVisibility`: the bar only with more than 20 rows.
    fn update_scroll_visibility(&self, ui: &mut Ui) {
        if self.desc_count(ui) - VISIBLE_ROWS >= 1 {
            show(ui, self.char_scroll);
        } else {
            hide(ui, self.char_scroll);
        }
    }

    /// Both lists from the bar's value.
    fn sync_first_visible(&self, ui: &mut Ui) {
        let value = scroll(ui, self.char_scroll, |s| s.value).unwrap_or(0.0);
        for l in [self.char_sel_desc, self.char_sel_time] {
            list(ui, l, |l| l.list.first_visible = ftol(value));
        }
    }

    /// `RefillLists` (also `Create`'s fill): both lists emptied, one row
    /// per record that is not the restart save, the bar's range and value.
    fn refill_lists(&mut self, ui: &mut Ui) {
        for l in [self.char_sel_desc, self.char_sel_time] {
            list(ui, l, |l| l.list.item_count = 0);
        }
        scroll(ui, self.char_scroll, |s| {
            s.set_page(20.0);
            s.set_step(1.0);
        });
        show(ui, self.char_scroll);
        let mut added = 0i32;
        self.entries.clear();
        let rows: Vec<RosterRow> = self
            .roster
            .rows
            .iter()
            .filter(|r| !r.is_restart)
            .cloned()
            .collect();
        for row in rows {
            let (name, time, data) = (row.name.clone(), row.time.clone(), row.data);
            self.entries.push(row);
            add_item(ui, self.char_sel_desc, &name, data);
            add_item(ui, self.char_sel_time, &time, data);
            added += 1;
        }
        #[allow(clippy::cast_precision_loss)]
        let top = added as f32;
        scroll(ui, self.char_scroll, |s| {
            s.set_range(0.0, top);
            s.set_page(20.0);
            s.set_step(1.0);
            s.set_value(0.0);
        });
        self.update_scroll_visibility(ui);
        self.sync_first_visible(ui);
    }

    /// `ShowDetails`: the four value windows from the record, or "-" with
    /// Load and Delete off.
    pub fn show_details(&self, ui: &mut Ui, row: Option<&RowDetails>) {
        let Some(row) = row else {
            let dash = dash();
            for w in [
                self.character_ship,
                self.character_money,
                self.character_rank,
                self.character_location,
            ] {
                set_value(ui, w, Some(&dash));
            }
            for b in [self.load_character, self.delete_character] {
                clear_active(ui, b);
            }
            for b in [self.load_character, self.delete_character] {
                set_interactive(ui, b, false);
            }
            return;
        };
        let money: Vec<u16> = row.money.to_string().encode_utf16().collect();
        set_value(ui, self.character_money, Some(&plain(&money)));
        let rank: Vec<u16> = row.rank.to_string().encode_utf16().collect();
        set_value(ui, self.character_rank, Some(&plain(&rank)));
        let ship = row.ship_name.as_deref().map(plain);
        set_value(ui, self.character_ship, ship.as_ref());
        let location = row.location_name.as_deref().map(plain);
        set_value(ui, self.character_location, location.as_ref());
        for b in [self.load_character, self.delete_character] {
            set_active(ui, b);
        }
        for b in [self.load_character, self.delete_character] {
            set_interactive(ui, b, true);
        }
    }

    /// `BeginLeave`: every widget hidden, the meshes shown, the lists not
    /// selectable, the buttons not interactive, the slide out from 0.
    fn begin_leave(&mut self, ui: &mut Ui, direction: i32) {
        for w in self.widgets() {
            hide(ui, w);
        }
        for i in self.button_images {
            show(ui, i);
        }
        for l in [self.char_sel_desc, self.char_sel_time] {
            list(ui, l, |l| l.list.selectable = false);
        }
        for b in [self.load_character, self.delete_character, self.main_menu] {
            set_interactive(ui, b, false);
        }
        self.leave_direction = direction;
        self.leaving = true;
        self.slide_t = 0.0;
        ui.services.play_ui_sound_slot(SWISH_SLOT);
    }

    /// The controls live again after the confirm box (`Slot23`'s two
    /// spellings).
    fn reactivate(&mut self, ui: &mut Ui) {
        self.delete_pending = false;
        for w in [
            self.char_scroll,
            self.char_sel_desc,
            self.char_sel_time,
            self.main_menu,
            self.load_character,
            self.delete_character,
        ] {
            set_active(ui, w);
        }
        for b in [self.main_menu, self.load_character, self.delete_character] {
            set_interactive(ui, b, true);
        }
    }

    /// `Slot23`: the buttons' clicks (0x3004), the confirm box's answer
    /// (0x3006), and through [`Widget::child_message`] the lists' picks
    /// (0xf001, 0xf002) and the bar's moves (0xf005).
    pub fn slot23(&mut self, ui: &mut Ui, msg: u32, a: u32, b: u32) -> bool {
        match msg {
            MSG_CLICK => {
                ui.services.play_or_rewind_ui_sound_slot(0);
                match a {
                    CMD_LOAD | CMD_MAIN_MENU => {
                        if !self.leaving {
                            self.begin_leave(ui, i32::from(a == CMD_LOAD));
                        }
                        true
                    }
                    CMD_DELETE => {
                        if self.delete_pending {
                            // The confirm box said yes: the delete
                            // (`DestroyCharacter`) comes with the dialog
                            // (D1103); the box is never opened yet.
                            self.reactivate(ui);
                            return true;
                        }
                        self.delete_pending = true;
                        for w in [
                            self.char_scroll,
                            self.char_sel_desc,
                            self.char_sel_time,
                            self.main_menu,
                            self.load_character,
                            self.delete_character,
                        ] {
                            clear_active(ui, w);
                        }
                        for b in [self.main_menu, self.load_character, self.delete_character] {
                            set_interactive(ui, b, false);
                        }
                        let selected =
                            list(ui, self.char_sel_desc, |l| l.list.selected_index).unwrap_or(-1);
                        if selected == -1 {
                            return true;
                        }
                        // `NnConfirmOpenFormatted` (a modal NeuroNet
                        // dialog, not ported yet): answered "no" (D1103).
                        self.slot23(ui, MSG_CLICK, CMD_DECLINED, 0);
                        true
                    }
                    CMD_DECLINED => {
                        if self.delete_pending {
                            self.reactivate(ui);
                        }
                        false
                    }
                    _ => false,
                }
            }
            0xf001 => {
                let row = b.cast_signed();
                for l in [self.char_sel_desc, self.char_sel_time] {
                    list(ui, l, |l| {
                        if row >= -1 && row < l.list.item_count {
                            l.list.selected_index = row;
                        }
                    });
                }
                let entry = self.entry_of(ui, row);
                self.show_details(ui, entry.as_ref().map(|e| &e.details));
                false
            }
            MSG_CONFIRM_ANSWER => {
                match a {
                    0 => {
                        self.slot23(ui, MSG_CLICK, CMD_DECLINED, 0);
                    }
                    1 => {
                        self.slot23(ui, MSG_CLICK, CMD_DELETE, 0);
                    }
                    _ => {}
                }
                true
            }
            0xf002 => {
                let selected =
                    list(ui, self.char_sel_desc, |l| l.list.selected_index).unwrap_or(-1);
                if selected != -1 && !self.leaving {
                    self.begin_leave(ui, 1);
                }
                true
            }
            0xf005 => {
                self.sync_first_visible(ui);
                true
            }
            _ => false,
        }
    }

    /// `OnEvent` (the `Event::Handler` face): `USER_CANCEL` outside a leave
    /// is state 1; anything else is `MasterWin`'s (false).
    pub fn on_event(&mut self, msg: u32) -> bool {
        if msg == EVENT_CANCEL && !self.leaving {
            self.result = 1;
            return true;
        }
        false
    }

    /// The two boxes at the slide's place.
    fn place_boxes(&self, ui: &mut Ui) {
        if let Some(b) = self.big_box {
            ui.set_position(b, Vector::new((self.slide_t * -1.5) as f32, 0.0, 0.0));
        }
        if let Some(s) = self.small_box {
            ui.set_position(s, Vector::new(0.0, self.slide_t as f32, 0.0));
        }
    }

    /// The slide's end: both boxes home, the widgets shown (the bar only
    /// when needed), the meshes hidden, the slide parked at 2.
    fn arrive(&mut self, ui: &mut Ui) {
        for b in [self.big_box, self.small_box].into_iter().flatten() {
            ui.set_position(b, Vector::ZERO);
        }
        for w in self.widgets() {
            if w == self.char_scroll {
                self.update_scroll_visibility(ui);
            } else {
                show(ui, w);
            }
        }
        for i in self.button_images {
            hide(ui, i);
        }
        self.slide_t = 2.0;
    }
}

impl Widget for PageCharacterSelectSingle {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }

    /// `Create`: `MasterWin::Create`, the fifteen widgets, the bar's
    /// settings, the lists from the roster, the labels' captions and the
    /// "-" values, everything hidden. False when a widget is missing (the
    /// reference would use a null pointer).
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        // Its result is not tested.
        self.master.create(ui, id, parent, &mut Payload::None);
        self.result = 0;
        let find = |name: &[u8]| self.master.page.as_ref().and_then(|p| p.find(name));
        self.character_ship = find(b"CharacterShip");
        self.load_character = find(b"LoadCharacter");
        self.delete_character = find(b"DeleteCharacter");
        self.main_menu = find(b"MainMenu");
        self.character_money = find(b"CharacterMoney");
        self.character_rank = find(b"CharacterRank");
        self.character_location = find(b"CharacterLocation");
        self.char_scroll = find(b"CharScroll");
        self.char_sel_desc = find(b"CharSelDesc");
        self.char_sel_time = find(b"CharSelTime");
        self.money_label = find(b"MoneyLabel");
        self.rank_label = find(b"RankLabel");
        self.location_label = find(b"LocationLabel");
        self.ship_label = find(b"ShipLabel");
        self.screen_title = find(b"ScreenTitle");
        if self.widgets().iter().any(Option::is_none) {
            return false;
        }
        ui.services.register_event(id, EVENT_CANCEL);

        scroll(ui, self.char_scroll, |s| {
            s.set_arrow_colors(CHROME_COLOR, CHROME_COLOR);
            s.set_bar_colors(CHROME_COLOR, CHROME_COLOR);
            s.set_page(20.0);
            s.set_step(1.0);
        });
        for b in [self.load_character, self.delete_character, self.main_menu] {
            button(ui, b, |b| b.win.base.draw_frame = false);
        }
        self.delete_pending = false;
        self.character_chosen = false;
        self.slide_t = -2.0;

        // `ReloadFactions` and `RequestCharacterInfo` are the caller's.
        if self.roster.status == 1 {
            self.refill_lists(ui);
        }

        rich(ui, self.character_ship, |w| w.win.clear_content());
        for b in [self.load_character, self.delete_character, self.main_menu] {
            show(ui, b);
        }
        for l in [self.char_sel_desc, self.char_sel_time] {
            list(ui, l, |l| {
                l.list.draw_frame = false;
                l.list.draw_row_rules = true;
            });
        }
        self.show_details(ui, None);
        for w in self.widgets() {
            hide(ui, w);
        }

        self.money_label_text = ids_list(ui, MONEY_IDS);
        self.rank_label_text = ids_list(ui, RANK_IDS);
        self.location_label_text = ids_list(ui, LOCATION_IDS);
        self.ship_label_text = ids_list(ui, SHIP_IDS);
        self.title_text = ids_list(ui, TITLE_IDS);
        for (label, style, text) in [
            (self.money_label, VALUE_STYLE, &self.money_label_text),
            (self.rank_label, VALUE_STYLE, &self.rank_label_text),
            (self.location_label, VALUE_STYLE, &self.location_label_text),
            (self.ship_label, VALUE_STYLE, &self.ship_label_text),
            (self.screen_title, TITLE_STYLE, &self.title_text),
        ] {
            let text = Rc::clone(text);
            rich(ui, label, |w| {
                w.win.clear_content();
                w.win.append_node(&RdlNode::Style(style));
                w.win.append_node(&RdlNode::Ref(text));
            });
        }

        let dash = dash();
        for w in [
            self.character_ship,
            self.character_money,
            self.character_rank,
            self.character_location,
        ] {
            set_value(ui, w, Some(&dash));
        }
        for l in [self.char_sel_desc, self.char_sel_time] {
            list(ui, l, |l| l.list.visible_rows = VISIBLE_ROWS);
        }
        true
    }

    /// `Destroy`: hidden, every child destroyed (the static controls
    /// deleted), `MasterWin::Destroy`, `USER_CANCEL` dropped, the widget
    /// pointers cleared.
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(n) = ui.node_mut(id) {
            n.flags &= !(ACTIVE | VISIBLE);
        }
        for child in ui.children(id) {
            let is_static = ui
                .node(child)
                .and_then(|n| n.class_name)
                .is_some_and(|c| c == "StaticControl");
            ui.destroy(child);
            if is_static {
                ui.delete(child);
            }
        }
        self.master.destroy(ui, id);
        ui.services.unregister_event(id, EVENT_CANCEL);
        self.character_ship = None;
        self.load_character = None;
        self.delete_character = None;
        self.main_menu = None;
        self.character_money = None;
        self.character_rank = None;
        self.character_location = None;
        self.char_scroll = None;
        self.char_sel_desc = None;
        self.char_sel_time = None;
        self.money_label = None;
        self.rank_label = None;
        self.location_label = None;
        self.ship_label = None;
        self.screen_title = None;
    }

    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.master.on_delete(ui, id);
    }

    /// `Update`: the children, the hover pulse in both lists, the slide in
    /// or out, then the load once the menu has left the intro base.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);

        let desc_hover = list(ui, self.char_sel_desc, |l| l.list.hover_index).unwrap_or(-1);
        let time_hover = list(ui, self.char_sel_time, |l| l.list.hover_index).unwrap_or(-1);
        let mut hover = -1;
        if desc_hover != -1 {
            hover = desc_hover;
        }
        if time_hover != -1 {
            hover = time_hover;
        }
        for l in [self.char_sel_desc, self.char_sel_time] {
            list(ui, l, |l| {
                if hover >= -1 && hover < l.list.item_count {
                    l.list.field_80 = hover;
                }
            });
        }

        let dt = ui.frame_time;
        if !self.leaving && self.slide_t < 1.0 {
            self.slide_t += dt.min(0.5) * 1.3;
            place_button_images(ui, &self.button_images, self.slide_t);
            if self.slide_t > 0.0 {
                self.arrive(ui);
            } else {
                self.place_boxes(ui);
            }
            if self.slide_t > -0.5 && self.slide_t < -0.1 {
                ui.services.play_ui_sound_slot(SWISH_SLOT);
            }
        } else if self.leaving {
            self.slide_t -= dt * 1.5;
            place_button_images(ui, &self.button_images, self.slide_t);
            if self.slide_t < -0.5 {
                for i in self.button_images {
                    hide(ui, i);
                }
                self.leaving = false;
                if self.leave_direction == 1 {
                    self.load_requested = true;
                } else {
                    self.result = 1;
                }
                self.slide_t = -2.0;
            } else {
                self.place_boxes(ui);
            }
        }

        if !self.intro_base_left {
            return;
        }
        self.intro_base_left = false;
        self.load_requested = false;
        if self.character_chosen {
            return;
        }
        self.loading_screen_requested = true;
        let selected = list(ui, self.char_sel_desc, |l| l.list.selected_index).unwrap_or(-1);
        let Some(chosen) = self.entry_of(ui, selected) else {
            return;
        };
        self.character_chosen = true;
        self.selected_character = chosen.data;
        self.result = 0xc;
    }

    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.master.draw(ui, id, host);
    }

    fn broadcast(&mut self, ui: &mut Ui, _id: NodeId, msg: u32, a: u32, b: u32) -> bool {
        self.slot23(ui, msg, a, b)
    }

    /// The lists' picks and the bar's moves, as their slot 23 messages.
    fn child_message(&mut self, ui: &mut Ui, _id: NodeId, message: &ChildMessage) -> bool {
        match message {
            ChildMessage::ListPick {
                double_click,
                index,
                ..
            } => {
                let msg = if *double_click { 0xf002 } else { 0xf001 };
                self.slot23(ui, msg, 0, index.cast_unsigned())
            }
            ChildMessage::ScrollChanged { .. } => self.slot23(ui, 0xf005, 0, 0),
            _ => false,
        }
    }

    fn property(&mut self, _ui: &mut Ui, _id: NodeId, _property: Property<'_>) -> isize {
        0
    }
}

/// The page's node: made once, without a class name, as the reference's
/// static `G_PageCharacterSelectSingle`.
pub fn make_page(ui: &mut Ui) -> NodeId {
    ui.alloc(Some(Box::new(PageCharacterSelectSingle::new())))
}

/// `EnterCase18Chrome`: the five statics made under the page (every entry
/// makes them again; the page's `Destroy` deletes the old ones).
pub fn enter_chrome(ui: &mut Ui, page: NodeId) {
    let mut made = [None; 5];
    for (slot, (nickname, mesh, offset)) in made.iter_mut().zip(CASE18_STATICS) {
        *slot = ui.create_by_class(
            Some(nickname),
            b"StaticControl",
            Some(page),
            &mut Payload::None,
        );
        let Some(id) = *slot else { continue };
        init_static_control(
            ui,
            id,
            Some(mesh),
            &StaticInit {
                offset: Vector::new(offset[0], offset[1], offset[2]),
                no_wire_render: true,
                no_coloring: true,
                no_clear_render: true,
                ..StaticInit::default()
            },
        );
    }
    if let Some(p) = ui.widget_mut::<PageCharacterSelectSingle>(page) {
        p.button_images = [made[0], made[1], made[2]];
        p.big_box = made[3];
        p.small_box = made[4];
    }
}

/// `EnterMenuState` case 18 after the active page's release: the chrome,
/// the page file (`ui.services.data_file`) loaded into the descriptor and,
/// when it loads, the page created with `roster`. False when the file does
/// not load (the slide is then reset to -2 and the menu goes to state 1).
pub fn enter_load_game_page(ui: &mut Ui, page: NodeId, roster: Roster) -> bool {
    enter_chrome(ui, page);
    let desc = ui
        .services
        .data_file(PAGE_FILE)
        .map(|bytes| PageDesc::load(&bytes));
    let Some(desc) = desc.filter(|d| d.ok) else {
        if let Some(p) = ui.widget_mut::<PageCharacterSelectSingle>(page) {
            p.slide_t = -2.0;
        }
        return false;
    };
    if let Some(p) = ui.widget_mut::<PageCharacterSelectSingle>(page) {
        p.master = MasterWin::new(&desc);
        p.roster = roster;
    }
    ui.with_widget(page, false, |w, ui| {
        w.create(ui, page, None, &mut Payload::None)
    });
    true
}
