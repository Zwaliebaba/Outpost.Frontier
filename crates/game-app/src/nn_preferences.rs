//! The preferences window (`NN_Preferences`, `nn_preferences.cpp`; work
//! package P5.2): the column of page buttons (general, controls,
//! performance, audio, credits, main menu), the fourteen option rows of the
//! general, performance and audio pages, the key map page's thirteen rows
//! with their group tabs and scroll bar, the video mode cycle, the credits
//! and the key dialog's frame, all made by `Create` as flat children of the
//! window.
//!
//! Opened from the main menu (menu state 10, [`crate::intro::options`]) the
//! column slides in at 1.5 per second (the frame time capped at 0.2 s) and
//! the big box with it; once the slide passes 0 the general page is shown.
//! A column button flips the box half a turn to its page, Main Menu slides
//! the column out at 1.5 per second (the frame time capped at 0.03 s) and
//! then marks the window closed for the menu's poll.
//!
//! What the window reads of the client is the caller's
//! ([`PreferencesContext`]): the key map, the keyboard layout's key names,
//! the performance options, the display mode and the device's modes and
//! abilities. The lists come from `keylist.ini` and `optlist.ini` through
//! [`crate::preferences`].
//!
//! Not ported yet (D1106): the option rows' edits (the sliders, the
//! toggles, the apply and defaults buttons, the graphics preset), the key
//! binding (capture, conflict and confirm dialogs, `KeyEvent`,
//! `MouseEvent`), the "apply changes?" dialog and the video mode change with
//! its countdown, and the tooltips of the rows; the window takes only the
//! clicks of the column, the key groups' tabs and the credits' restart. The
//! in-game opener (a null payload: the game paused, the black plane, the
//! close button) has no caller yet; its construction is here, its pause and
//! its close come with it.

use neuron_client::text::rdl::{RdlNode, RenderDisplayList, load_ids};
use neuron_client::ui::children::{
    ButtonInit, StaticInit, TextInit, create_button_control, create_rich_text_win,
    create_static_control, create_text_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::controls::button::{ButtonControl, ButtonWidget, ToggleButtonWidget};
use neuron_client::ui::controls::rich_text_win::RichTextWin;
use neuron_client::ui::controls::scroll::UiScroll;
use neuron_client::ui::controls::text_control::{TextControl, TextControlWidget};
use neuron_client::ui::controls::wire::{WireControl, WireWidget};
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::page::EntryPayload;
use neuron_client::ui::pulse::palette;
use neuron_client::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};
use neuron_core::math::{Matrix, Vector, mul_matrix};
use neuron_core::num::trunc_to_i32;

use crate::keymap::KeyMap;
use crate::keymap::names::{KeyNames, format_binding_name};
use crate::perf_options::PerfOptions;
use crate::preferences::{KEY_LIST_PATH, KeyRow, OPT_LIST_PATH, PrefOption, Preferences};

/// The factory class and the window's name.
pub const CLASS: &[u8] = b"NN_Preferences";

/// `kPrefVideoResolutionCount` (the widescreen build's, decision O7).
pub const RESOLUTION_COUNT: usize = 24;
/// `kPrefVideoModeCount`.
pub const MODE_COUNT: usize = RESOLUTION_COUNT * 2;

/// The option rows and the key rows.
const OPTION_ROWS: usize = 14;
const KEY_ROWS: usize = 13;

/// `ui_motion_swish`'s UI sound slot.
const SWISH_SLOT: u32 = 0x1b;
/// The meshes.
const BUTTON_MESH: &[u8] = b"Interface\\NeuroNet\\Inventory\\FRONT_button.cmp";
const BIG_BOX_MESH: &[u8] = b"Interface\\NeuroNet\\Inventory\\FRONT_shortoptions_box2.cmp";
const CHECK_MESH: &[u8] = b"Interface\\NeuroNet\\Inventory\\check.3db";
const CHECK_BOX_MESH: &[u8] = b"Interface\\NeuroNet\\Inventory\\repairblackbox.3db";
const SELECT_SOUND: &[u8] = b"ui_select_item";

/// The palette words the window writes.
const IDLE: u32 = palette::IDLE;
const AMBER: u32 = palette::AMBER;
const GREY: u32 = palette::GREY;

/// A `float` from its bits (`NnFloatBits`).
const fn fb(bits: u32) -> f32 {
    f32::from_bits(bits)
}

/// A double expression stored to a `float` (T11).
#[allow(clippy::cast_possible_truncation)]
fn r(v: f64) -> f32 {
    v as f32
}

fn v3(x: f32, y: f32, z: f32) -> Vector {
    Vector::new(x, y, z)
}

/// The display mode (`g_display`: the parts the window reads).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DisplayMode {
    pub width: i32,
    pub height: i32,
    pub color_bpp: i32,
}

/// One device display mode (`GetDisplayModeEnum`): its size and its pixel
/// format's bits per pixel; `unknown_format` for the format 1 the window
/// skips.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DeviceMode {
    pub width: i32,
    pub height: i32,
    pub bpp: i32,
    pub unknown_format: bool,
}

/// What the window reads of the client.
#[derive(Clone, Debug, Default)]
pub struct PreferencesContext {
    /// `G_KeyMap`.
    pub keymap: KeyMap,
    /// The keyboard layout's key names (`GetKeyNameTextW`).
    pub key_names: KeyNames,
    /// The performance options (`FindPerfOption`, `FindPerfOptionByIds`).
    pub perf: PerfOptions,
    /// `g_display`.
    pub display: DisplayMode,
    /// The device's display modes, in the renderer's order.
    pub device_modes: Vec<DeviceMode>,
    /// `WidescreenGetDesktopSize`.
    pub desktop: (i32, i32),
    /// `QueryDeviceAbility(3)`: the video memory in bytes.
    pub video_memory: u32,
    /// `QueryDeviceAbility(1)`: the gamma ramp.
    pub gamma_ability: u32,
    /// `QueryDeviceAbility(8)`: environment maps.
    pub environment_map_ability: u32,
    /// `SinglePlayer()`.
    pub single_player: bool,
    /// `GetMaxSubmittedSounds()`.
    pub max_submitted_sounds: i32,
}

/// `FPrefVideoMode`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VideoMode {
    pub width: i32,
    pub height: i32,
    pub bpp: i32,
}

/// The window's controls (`NnPreferences`'s pointers).
#[derive(Clone, Debug, Default)]
pub struct Controls {
    /// `+0x338`.
    pub big_box: Option<NodeId>,
    /// `+0x344`.
    pub title: Option<NodeId>,
    pub close_button: Option<NodeId>,
    /// Keymap, interface, video, audio, credits, main menu.
    pub page_buttons: [Option<NodeId>; 6],
    pub page_texts: [Option<NodeId>; 6],
    pub opt_title_text: [Option<NodeId>; OPTION_ROWS],
    pub opt_text: [Option<NodeId>; OPTION_ROWS],
    pub opt_toggle: [Option<NodeId>; OPTION_ROWS],
    pub opt_toggle_text: [Option<NodeId>; OPTION_ROWS],
    pub opt_scroll: [Option<NodeId>; OPTION_ROWS],
    pub small_text: [Option<NodeId>; OPTION_ROWS],
    pub scroll_text_left: [Option<NodeId>; OPTION_ROWS],
    pub scroll_text_right: [Option<NodeId>; OPTION_ROWS],
    pub small_button: [Option<NodeId>; OPTION_ROWS],
    pub small_button_text: [Option<NodeId>; OPTION_ROWS],
    pub static_bar: Option<NodeId>,
    pub cancel_button: Option<NodeId>,
    pub defaults_button: Option<NodeId>,
    pub cancel_text: Option<NodeId>,
    pub defaults_text: Option<NodeId>,
    pub video_mode_cycle: Option<NodeId>,
    pub video_box: Option<NodeId>,
    pub video_box2: Option<NodeId>,
    pub video_mode_text: Option<NodeId>,
    pub key_scroll: Option<NodeId>,
    pub group_buttons: [Option<NodeId>; 3],
    pub group_texts: [Option<NodeId>; 3],
    pub key_btn_rows: [Option<NodeId>; KEY_ROWS],
    pub key_alt_btn_rows: [Option<NodeId>; KEY_ROWS],
    pub command_rows: [Option<NodeId>; KEY_ROWS],
    pub key_rows: [Option<NodeId>; KEY_ROWS],
    pub key_alt_rows: [Option<NodeId>; KEY_ROWS],
    pub key_column1: Option<NodeId>,
    pub key_column2: Option<NodeId>,
    pub static_rows: [Option<NodeId>; OPTION_ROWS],
    pub credits_text: Option<NodeId>,
    pub set_key_frame: Option<NodeId>,
    pub dialog_text: [Option<NodeId>; 3],
    pub overwrite_button: Option<NodeId>,
    pub dialog_cancel_button: Option<NodeId>,
    pub dialog_button_text: [Option<NodeId>; 2],
}

/// The window's state apart from its `WinControl` part.
#[derive(Clone, Debug, Default)]
pub struct PreferencesState {
    pub ctx: PreferencesContext,
    pub c: Controls,
    /// The key groups and the option sections (`KeyGroups`, `OptionsA` to
    /// `OptionsC`, `bSeedMode`).
    pub lists: Preferences,
    /// `+0x32c`: closed; the menu's poll deletes the window.
    pub closed: bool,
    /// `+0x32d`: opened in game (a null payload).
    pub in_game: bool,
    pub cur_page: i32,
    pub req_page: i32,
    pub cur_group: i32,
    /// The slide, -2 to 1.
    pub anim: f64,
    pub dirty: bool,
    pub creating: bool,
    pub confirm_mode: bool,
    pub key_capture: bool,
    pub video_confirm: bool,
    pub conflict: bool,
    pub applying_preset: bool,
    pub flip_angle: f64,
    pub flip_speed: f64,
    pub flipping: bool,
    pub flip_half: bool,
    pub closing_anim: bool,
    pub credits_scroll: f64,
    pub credits_speed: f64,
    pub sel_width: i32,
    pub cur_width: i32,
    pub sel32: bool,
    pub cur32: bool,
    pub big_vram: bool,
    pub snapshot: [f32; OPTION_ROWS],
    pub modes: Vec<VideoMode>,
    pub mode_avail: Vec<bool>,
    pub cycle_index: Vec<i32>,
    pub mode_count: i32,
    /// `G_bNnScreenUp`.
    pub screen_up: bool,
    /// `SavePerfOptions(kPerfOptionsFile)` asked for; the client saves.
    pub save_perf_options_requested: bool,
}

/// `NnPreferences`.
#[derive(Debug, Default)]
pub struct NnPreferences {
    pub control: WinControl,
    pub state: PreferencesState,
}

// ---- small control helpers ------------------------------------------------

fn show(ui: &mut Ui, id: Option<NodeId>) {
    if let Some(n) = id.and_then(|id| ui.node_mut(id)) {
        n.flags |= 3;
    }
}

fn hide(ui: &mut Ui, id: Option<NodeId>) {
    if let Some(n) = id.and_then(|id| ui.node_mut(id)) {
        n.flags &= 0xfc;
    }
}

fn with_text(ui: &mut Ui, id: Option<NodeId>, f: impl FnOnce(&mut TextControl, &mut Ui, NodeId)) {
    let Some(id) = id else { return };
    ui.with_widget(id, (), |w, ui| {
        if let Some(t) = w.as_any_mut().downcast_mut::<TextControlWidget>() {
            f(&mut t.text, ui, id);
        }
    });
}

fn text_colour(ui: &mut Ui, id: Option<NodeId>, colour: u32) {
    with_text(ui, id, |t, _, _| t.text_colour = colour);
}

fn with_button(ui: &mut Ui, id: Option<NodeId>, f: impl FnOnce(&mut ButtonControl)) {
    let Some(w) = id
        .and_then(|id| ui.node_mut(id))
        .and_then(|n| n.widget_mut())
    else {
        return;
    };
    let any = w.as_any_mut();
    if any.is::<ButtonWidget>() {
        if let Some(b) = any.downcast_mut::<ButtonWidget>() {
            f(&mut b.button);
        }
    } else if let Some(b) = any.downcast_mut::<ToggleButtonWidget>() {
        f(&mut b.button);
    }
}

fn with_wire(ui: &mut Ui, id: Option<NodeId>, f: impl FnOnce(&mut WireControl)) {
    if let Some(w) = id.and_then(|id| ui.widget_mut::<WireWidget>(id)) {
        f(&mut w.wire);
    }
}

fn with_scroll<R>(
    ui: &mut Ui,
    id: Option<NodeId>,
    f: impl FnOnce(&mut UiScroll) -> R,
) -> Option<R> {
    id.and_then(|id| ui.widget_mut::<UiScroll>(id)).map(f)
}

/// The wire button's `ButtonState` (`+0x388`).
fn wire_state_bits(ui: &mut Ui, id: Option<NodeId>, set: bool, bits: u8) {
    with_wire(ui, id, |w| {
        if let Some(b) = w.button.as_mut() {
            if set {
                b.state |= bits;
            } else {
                b.state &= !bits;
            }
        }
    });
}

fn property(ui: &mut Ui, id: Option<NodeId>, p: Property<'_>) -> isize {
    id.map_or(0, |id| ui.property(id, p))
}

fn property_other(ui: &mut Ui, id: Option<NodeId>, msg: i32, a: usize, b: usize) -> isize {
    property(ui, id, Property::Other(msg, a, b))
}

fn set_text(ui: &mut Ui, id: Option<NodeId>, text: &[u16]) {
    property(ui, id, Property::SetText(Some(text)));
}

/// `GetIdsStringWWithRawFallback(ids, buffer, 0x40)`.
fn ids_text(ui: &Ui, ids: u32) -> Vec<u16> {
    let Some(c) = ui.catalogs.as_deref() else {
        return Vec::new();
    };
    let mut buffer = [0u16; 0x40];
    let n = c.string_with_card_fallback_into(ids, &mut buffer);
    buffer[..n].to_vec()
}

/// `SetTextIds`: the id's text (at most 63 units) through property 0x1d.
fn set_text_ids(ui: &mut Ui, id: Option<NodeId>, ids: u32) {
    let text = ids_text(ui, ids);
    set_text(ui, id, &text);
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().collect()
}

fn position3(ui: &mut Ui, id: Option<NodeId>, x: f32, y: f32, z: f32) {
    if let Some(id) = id {
        ui.set_position(id, v3(x, y, z));
    }
}

/// `RotateMatrix({x, 0, 0})`: the turn about x of an angle in degrees
/// (none at or below 0.01 degrees); the cosine of the unrounded radians,
/// the sine of the rounded ones.
#[allow(clippy::neg_cmp_op_on_partial_ord)]
fn rotate_matrix_x(degrees: f32) -> Matrix {
    if !(f64::from(degrees).abs() > 0.01) {
        return Matrix::IDENTITY;
    }
    let exact = f64::from(degrees) * f64::from(0.017_453_292_f32);
    let rounded = r(exact);
    let c = r(exact.cos());
    let s = r(f64::from(rounded).sin());
    let rot = Matrix::from_rows([[1.0, 0.0, 0.0], [0.0, c, -s], [0.0, s, c]]);
    mul_matrix(&Matrix::IDENTITY, &rot)
}

// ---- the child construction helpers ----------------------------------------
//
// `WinControl`'s helpers the toolkit has not ported yet
// (`wincontrol_children.cpp`): each makes the child through the factory
// and, when it is made, calls its `Init`.

fn make(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    class: &[u8],
    payload: &mut Payload<'_>,
) -> Option<NodeId> {
    ui.create_by_class(Some(name), class, Some(parent), payload)
}

/// `WinControl::CreateWireButtonControl`.
#[allow(clippy::too_many_arguments)]
fn create_wire_button(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    sound: &[u8],
    position: Vector,
    half: Vector,
    always_white: u8,
    no_z_enable: bool,
    tooltip: i32,
    command: i32,
    edge_mask: u32,
) -> Option<NodeId> {
    let id = make(ui, parent, name, b"WireButtonControl", &mut Payload::None)?;
    ui.with_widget(id, (), |w, ui| {
        if let Some(w) = w.as_any_mut().downcast_mut::<WireWidget>() {
            w.wire.init_button(
                ui,
                id,
                Some(sound),
                position,
                half,
                always_white,
                no_z_enable,
                tooltip,
                command,
                edge_mask,
            );
        }
    });
    Some(id)
}

/// `WinControl::CreateWireStaticControl`.
fn create_wire_static(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    position: Vector,
    half: Vector,
    edge_mask: u32,
) -> Option<NodeId> {
    let id = make(ui, parent, name, b"WireStaticControl", &mut Payload::None)?;
    ui.with_widget(id, (), |w, ui| {
        if let Some(w) = w.as_any_mut().downcast_mut::<WireWidget>() {
            w.wire
                .init_static(ui, id, position, half, 0, false, 0, edge_mask);
        }
    });
    Some(id)
}

/// `WinControl::CreateUIScroll` and `CreateUICycle`: the payload (the
/// sizes, the range and page as floats, a step of 1), then the scroll
/// offset, the value 0 and the direction.
#[allow(clippy::too_many_arguments, clippy::cast_precision_loss)]
fn create_scroll(
    ui: &mut Ui,
    parent: NodeId,
    class: &[u8],
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
    let id = make(ui, parent, name, class, &mut Payload::Page(&payload))?;
    ui.with_widget(id, (), |w, ui| {
        if let Some(s) = w.as_any_mut().downcast_mut::<UiScroll>() {
            s.set_scroll_offset(ui, id, position);
            s.set_value(0.0);
            s.horizontal = horizontal;
        }
    });
    Some(id)
}

/// `WinControl::CreateToggleButtonControl`.
fn create_toggle(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    position: Vector,
    half: Vector,
) -> Option<NodeId> {
    let id = make(ui, parent, name, b"ToggleButtonControl", &mut Payload::None)?;
    ui.with_widget(id, (), |w, ui| {
        if let Some(b) = w.as_any_mut().downcast_mut::<ToggleButtonWidget>() {
            b.button.init(
                ui,
                id,
                Some(CHECK_MESH),
                Some(CHECK_BOX_MESH),
                Some(b"ui_shipview_select"),
                None,
                position,
                None,
                half,
                0,
                0,
                0,
                false,
                0x65f,
                0x67a,
                0,
            );
        }
    });
    Some(id)
}

/// `CreateStaticControl` with the window's settings: no wire render, no
/// colouring, no clear render.
fn create_static(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    mesh: &[u8],
    offset: Vector,
) -> Option<NodeId> {
    create_static_control(
        ui,
        parent,
        name,
        Some(mesh),
        &StaticInit {
            offset,
            no_wire_render: true,
            no_coloring: true,
            no_clear_render: true,
            ..StaticInit::default()
        },
    )
}

/// `CreateButtonControl(name, mesh, 0, "ui_select_item", 0, position, 0,
/// half, 0, 0, 0, 0, tooltip, 0, 0)`.
fn create_button(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    mesh: &[u8],
    position: Vector,
    half: Vector,
    tooltip: i32,
) -> Option<NodeId> {
    create_button_control(
        ui,
        parent,
        name,
        Some(mesh),
        &ButtonInit {
            sound: Some(SELECT_SOUND),
            position,
            half_extents: half,
            tooltip,
            ..ButtonInit::default()
        },
    )
}

/// `CreateTextControl(name, style, position, 0, width, height, scale, ids,
/// justification, 0, corner_anchor)` (the scale is not read).
#[allow(clippy::too_many_arguments)]
fn create_text(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    style: &[u8],
    position: Vector,
    width: f32,
    height: f32,
    ids: u32,
    justification: i32,
    corner_anchor: bool,
) -> Option<NodeId> {
    create_text_control(
        ui,
        parent,
        name,
        &TextInit {
            style: Some(style),
            position,
            width,
            height,
            ids,
            justification,
            corner_anchor,
            ..TextInit::default()
        },
    )
}

fn row_name(prefix: &str, index: usize) -> Vec<u8> {
    format!("{prefix}{index}").into_bytes()
}

// ---- the window -------------------------------------------------------------

impl PreferencesState {
    /// The constructor's state with the client's context.
    #[must_use]
    pub fn new(ctx: PreferencesContext) -> Self {
        PreferencesState {
            ctx,
            modes: vec![VideoMode::default(); MODE_COUNT],
            mode_avail: vec![false; MODE_COUNT],
            cycle_index: vec![0; MODE_COUNT],
            ..PreferencesState::default()
        }
    }

    /// The column in `Update`'s order: interface, keymap, video, audio,
    /// credits (from the main menu only), main menu.
    fn column(&self) -> ([Option<NodeId>; 6], [Option<NodeId>; 6]) {
        let b = &self.c.page_buttons;
        let t = &self.c.page_texts;
        let credits = !self.in_game;
        (
            [
                b[1],
                b[0],
                b[2],
                b[3],
                if credits { b[4] } else { None },
                b[5],
            ],
            [
                t[1],
                t[0],
                t[2],
                t[3],
                if credits { t[4] } else { None },
                t[5],
            ],
        )
    }

    /// `BuildWidescreenModeTable`: every device mode of at least 800 x 600
    /// at least as wide as 4:3, one height per width (the one closest to
    /// the desktop's aspect), ascending by width, the last
    /// [`RESOLUTION_COUNT`] of them at 16 bpp, then the same at 32 bpp;
    /// width -1 for the unused entries.
    fn build_mode_table(&mut self) {
        let (dw, dh) = self.ctx.desktop;
        let desktop_aspect = f64::from(dw) / f64::from(dh);
        let mut picked: Vec<VideoMode> = Vec::new();
        for mode in &self.ctx.device_modes {
            if mode.unknown_format || mode.width < 0x320 || mode.height < 0x258 {
                continue;
            }
            if mode.width * 3 < mode.height * 4 {
                continue;
            }
            let aspect = f64::from(mode.width) / f64::from(mode.height);
            if let Some(slot) = picked.iter_mut().find(|p| p.width == mode.width) {
                let held = f64::from(slot.width) / f64::from(slot.height);
                if (aspect - desktop_aspect).abs() < (held - desktop_aspect).abs() {
                    slot.height = mode.height;
                }
            } else {
                picked.push(VideoMode {
                    width: mode.width,
                    height: mode.height,
                    bpp: 0x10,
                });
            }
        }
        // `std::sort` is not stable; the widths are distinct.
        picked.sort_by_key(|m| m.width);
        let first = picked.len().saturating_sub(RESOLUTION_COUNT);
        for index in 0..RESOLUTION_COUNT {
            let (width, height) = picked
                .get(first + index)
                .map_or((-1, -1), |m| (m.width, m.height));
            self.modes[index] = VideoMode {
                width,
                height,
                bpp: 0x10,
            };
            self.modes[index + RESOLUTION_COUNT] = VideoMode {
                width,
                height,
                bpp: 0x20,
            };
        }
    }

    /// `FindVideoMode`: whether a device mode of that width, height and bpp
    /// (each 0 for any; widescreen build: at least as wide as 4:3) exists.
    fn find_video_mode(&self, width: i32, height: i32, bpp: i32) -> bool {
        let display_bpp = self.ctx.display.color_bpp;
        self.ctx.device_modes.iter().any(|m| {
            !m.unknown_format
                && (width == 0 || width == m.width)
                && (height == 0 || height == m.height)
                && (if bpp > 0 {
                    bpp == m.bpp
                } else {
                    display_bpp == m.bpp
                })
                && m.width * 3 >= m.height * 4
                && m.width != 0
                && m.height != 0
        })
    }

    /// `ProbeModes`: the big-VRAM flag (more than 16 MB) and each mode's
    /// availability and cycle index (32-bit modes need the big VRAM).
    fn probe_modes(&mut self) {
        let megabytes = self.ctx.video_memory >> 20;
        self.big_vram = megabytes >= 0x11;
        self.mode_count = 0;
        for index in 0..MODE_COUNT {
            self.cycle_index[index] = -1;
            let m = self.modes[index];
            self.mode_avail[index] = self.find_video_mode(m.width, m.height, m.bpp);
            if !self.mode_avail[index] {
                continue;
            }
            if !self.big_vram && m.bpp != 0x10 {
                self.mode_avail[index] = false;
                continue;
            }
            self.cycle_index[index] = self.mode_count;
            self.mode_count += 1;
        }
    }

    /// The option row `index` of the current page (a blank row elsewhere).
    fn page_row(&self, index: usize) -> Option<PrefOption> {
        let list = match self.cur_page {
            1 => &self.lists.interface,
            2 => &self.lists.video,
            3 => &self.lists.audio,
            _ => return None,
        };
        // `LoadOptList` pads each section to 15 rows.
        Some(list.get(index).copied().unwrap_or_default())
    }

    fn perf_applied_by_ids(&self, ids: i32) -> Option<f32> {
        let ids = u32::try_from(ids).ok()?;
        PerfOptions::find_by_ids(ids).map(|i| self.ctx.perf.options[i].applied)
    }

    /// `GetCurGroupRows`.
    fn cur_group_rows(&self) -> &[KeyRow] {
        usize::try_from(self.cur_group)
            .ok()
            .and_then(|g| self.lists.key_groups.get(g))
            .map_or(&[], Vec::as_slice)
    }

    // ---- Create -------------------------------------------------------------

    /// `Create`'s children and their first state (everything but the
    /// window's own placement and `Win3D::Create`).
    #[allow(clippy::too_many_lines, clippy::needless_range_loop)]
    fn build(&mut self, ui: &mut Ui, id: NodeId) {
        self.creating = true;
        self.lists.seed_mode = false;
        self.flip_angle = 0.0;
        self.flip_speed = 360.0;
        self.flipping = false;
        self.flip_half = false;
        self.closing_anim = false;
        self.anim = -2.0;
        self.big_vram = false;
        self.key_capture = false;
        self.video_confirm = false;
        self.closed = false;
        self.applying_preset = false;
        self.dirty = false;
        self.conflict = false;
        self.confirm_mode = false;

        self.modes.resize(MODE_COUNT, VideoMode::default());
        self.mode_avail.resize(MODE_COUNT, false);
        self.cycle_index.resize(MODE_COUNT, 0);
        self.build_mode_table();
        for index in 0..MODE_COUNT {
            self.mode_avail[index] = false;
            self.cycle_index[index] = -1;
        }
        self.mode_count = 0;
        self.probe_modes();

        self.cur_group = 0;
        self.screen_up = true;
        // In game the game is paused here (`NnModalPauseGame`): comes with
        // the in-game opener.
        self.cur_page = 0;

        let zero = Vector::ZERO;
        if self.in_game {
            create_static(
                ui,
                id,
                b"Options_BlackPlane",
                b"Interface\\NeuroNet\\MISC_bigblackplane.3db",
                v3(0.0, 0.0, -1.0),
            );
            create_static(
                ui,
                id,
                b"OptionsBox",
                b"Interface\\NeuroNet\\Inventory\\FRONT_options_box2.cmp",
                v3(0.0, fb(0xbd99_999a), -1.0),
            );
        } else {
            self.c.big_box = create_static(
                ui,
                id,
                b"big box",
                BIG_BOX_MESH,
                v3(fb(0x3e19_999a), fb(0xbd50_e560), -1.0),
            );
        }

        self.sel_width = 0;
        self.cur_width = 0;
        self.cur32 = self.ctx.display.color_bpp == 0x20;
        self.sel32 = self.cur32;
        if !self.big_vram && self.cur32 {
            self.cur32 = false;
            self.sel32 = false;
        }

        let mut title_position = v3(fb(0xbdb8_51ec), 0.0, 0.0);
        if self.in_game {
            self.c.close_button = create_button(
                ui,
                id,
                b"PreferencesCloseButton",
                b"Interface\\NeuroNet\\Inventory\\X.3db",
                v3(fb(0x3edc_28f6), fb(0x3e6c_8b44), 0.0),
                v3(fb(0x3ca3_d70a), fb(0x3ca3_d70a), 0.0),
                0x409,
            );
            title_position.y = fb(0x3e7c_ed91);
        } else {
            title_position.y = fb(0x3e8e_d917);
        }
        self.c.title = create_text(
            ui,
            id,
            b"Screen Title",
            b"PageHeader",
            title_position,
            fb(0x3ef6_45a2),
            fb(0x3d75_c28f),
            0x4f8,
            1,
            true,
        );
        text_colour(ui, self.c.title, IDLE);

        let (mut column_x, mut column_y, mut step) =
            (fb(0xbe99_999a), fb(0x3e51_eb85), fb(0x3d9d_b22d));
        if !self.in_game {
            column_x = fb(0xbea8_f5c3);
            column_y = fb(0x3e57_0a3d);
            step = fb(0x3dae_147b);
        }
        self.build_column(ui, id, column_x, column_y, step);

        let row_y = if self.in_game {
            fb(0x3e47_ae14)
        } else {
            fb(0x3e51_eb85)
        };
        self.load_opt_list(ui);
        let scroll_row_y = r(f64::from(row_y) + f64::from(fb(0x3be5_6042)));
        for index in 0..OPTION_ROWS {
            self.build_option_row(ui, id, index, row_y, scroll_row_y);
        }

        self.c.static_bar = create_wire_static(
            ui,
            id,
            b"opt_static_bar",
            v3(fb(0x3e9e_b852), fb(0x3e9e_b852), -1.0),
            v3(fb(0x3f07_ae14), fb(0x3cf1_a9fc), 0.0),
            1,
        );

        let wire_half = v3(fb(0x3d8f_5c29), fb(0x3c75_c28f), 0.0);
        self.c.cancel_button = create_wire_button(
            ui,
            id,
            b"opt_cancel_btn",
            SELECT_SOUND,
            v3(fb(0x3e9a_1cac), fb(0xbe97_0a3d), 0.0),
            wire_half,
            0,
            false,
            0,
            0,
            0,
        );
        self.c.defaults_button = create_wire_button(
            ui,
            id,
            b"opt_set_defaults_btn",
            SELECT_SOUND,
            v3(fb(0xbc75_c28f), fb(0xbe97_0a3d), 0.0),
            wire_half,
            0,
            false,
            0,
            0,
            0,
        );
        for b in [self.c.cancel_button, self.c.defaults_button] {
            with_wire(ui, b, |w| w.colour = GREY.to_le_bytes());
        }
        self.c.cancel_text = create_text(
            ui,
            id,
            b"opt_cancel_txt",
            b"ItemName",
            v3(fb(0x3e9a_1cac), fb(0xbe98_1062), 0.0),
            fb(0x3e8a_3d71),
            fb(0x3cf5_c28f),
            0x2148,
            1,
            false,
        );
        self.c.defaults_text = create_text(
            ui,
            id,
            b"opt_set_defaults_txt",
            b"ItemName",
            v3(fb(0xbc75_c28f), fb(0xbe98_1062), 0.0),
            fb(0x3e8a_3d71),
            fb(0x3cf5_c28f),
            0x5e4,
            1,
            false,
        );

        self.c.video_mode_cycle = create_scroll(
            ui,
            id,
            b"UICycle",
            b"video_mode_cycle",
            v3(fb(0x3ed4_7ae1), fb(0x3e60_4189), -1.0),
            fb(0x3c44_9ba6),
            fb(0x3ccc_cccd),
            0,
            0,
            0,
            false,
        );
        self.c.video_box = create_wire_static(
            ui,
            id,
            b"video_mode_box",
            v3(fb(0x3f1a_9fbe), fb(0x3ed2_f1aa), -1.0),
            v3(fb(0x3e61_47ae), fb(0x3ccc_cccd), 0.0),
            0xf,
        );
        self.c.video_box2 = create_wire_static(
            ui,
            id,
            b"video_mode_box2",
            v3(fb(0x3f57_8d50), fb(0x3ed2_f1aa), -1.0),
            v3(fb(0x3c54_fdf4), fb(0x3ccc_cccd), 0.0),
            0xf,
        );
        self.c.video_mode_text = create_text(
            ui,
            id,
            b"video_mode_text",
            b"ItemName",
            v3(fb(0x3e9a_1cac), fb(0x3e4e_d917), 0.0),
            fb(0x3e60_4189),
            fb(0x3cf5_c28f),
            0,
            1,
            false,
        );
        with_scroll(ui, self.c.video_mode_cycle, |s| {
            s.step_mode = true;
            s.page = 1.0;
        });
        property_other(ui, self.c.video_mode_text, 0x24, 1, 0);

        self.dirty = false;
        text_colour(ui, self.c.cancel_text, GREY);
        with_wire(ui, self.c.cancel_button, |w| {
            w.use_colour = true;
            if let Some(b) = w.button.as_mut() {
                b.state &= 0xfb;
            }
        });

        self.leave_audio_page(ui);
        self.leave_video_page(ui);
        self.load_key_list(ui);

        #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
        let group0 = self.lists.key_groups[0].len() as i32;
        self.c.key_scroll = create_scroll(
            ui,
            id,
            b"UIScroll",
            b"keymap_scroll",
            v3(fb(0x3ed4_7ae1), fb(0x3e0a_3d71), -1.0),
            fb(0x3c75_c28f),
            fb(0x3ec7_ae14),
            0,
            group0,
            0xd,
            false,
        );

        let tab_half = v3(fb(0x3d8f_5c29), fb(0x3c75_c28f), 0.0);
        for group in 0..3 {
            let x = r(f64::from(u8::try_from(group).unwrap_or(0)) * 0.175 - 0.03);
            self.c.group_buttons[group] = create_wire_button(
                ui,
                id,
                &row_name("IGroup_Btn_Row", group),
                SELECT_SOUND,
                v3(x, fb(0x3e42_8f5c), 0.0),
                tab_half,
                0,
                false,
                0,
                0,
                0xf,
            );
            with_wire(ui, self.c.group_buttons[group], |w| w.wire_state &= 0xbf);
        }
        let tab_xs = [fb(0xbce5_6042), fb(0x3e17_8d50), fb(0x3ea5_e354)];
        for (group, x) in tab_xs.into_iter().enumerate() {
            self.c.group_texts[group] = create_text(
                ui,
                id,
                &row_name("IGroup_Txt_Row", group + 1),
                b"ItemName",
                v3(x, fb(0x3e40_0000), 0.0),
                fb(0x3e00_0000),
                fb(0x3cf1_a9fc),
                0x76e + u32::try_from(group).unwrap_or(0),
                1,
                false,
            );
        }
        for t in self.c.group_texts {
            property_other(ui, t, 0x24, 1, 0);
        }

        for row in 0..KEY_ROWS {
            self.build_key_row(ui, id, row);
        }

        self.c.key_column1 = create_text(
            ui,
            id,
            b"keymap_col1",
            b"ItemName",
            v3(fb(0x3e2e_147b), fb(0x3e1c_ac08), 0.0),
            fb(0x3e14_7ae1),
            fb(0x3c8b_4396),
            0x763,
            1,
            false,
        );
        self.c.key_column2 = create_text(
            ui,
            id,
            b"keymap_col2",
            b"ItemName",
            v3(fb(0x3ea1_cac1), fb(0x3e1c_ac08), 0.0),
            fb(0x3e14_7ae1),
            fb(0x3c8b_4396),
            0x764,
            1,
            false,
        );

        for row in 0..OPTION_ROWS {
            let y = r(0.1745 - (f64::from(u8::try_from(row).unwrap_or(0)) - 1.0) * 0.059);
            self.c.static_rows[row] = create_wire_static(
                ui,
                id,
                &row_name("Istatics_Row", row),
                v3(fb(0x3e92_f1aa), y, -1.0),
                v3(fb(0x3f07_ae14), fb(0x3cf1_a9fc), 0.0),
                1,
            );
        }
        with_wire(ui, self.c.static_rows[13], |w| w.wire_state2 |= 1);
        self.fill_key_rows(ui, 0);
        self.leave_key_map_page(ui);

        self.build_credits(ui, id);

        if !self.in_game {
            hide(ui, self.c.credits_text);
            text_colour(ui, self.c.page_texts[4], IDLE);
            set_text_ids(ui, self.c.defaults_text, 0x5e4);
            hide(ui, self.c.defaults_button);
            hide(ui, self.c.defaults_text);
            hide(ui, self.c.title);
        }

        self.c.set_key_frame = create_static_control(
            ui,
            id,
            b"SetKeyFrame",
            Some(b"Interface\\NeuroNet\\Options\\alt-f4window.cmp"),
            &StaticInit {
                offset: v3(0.0, 0.0, -1.0),
                half_extents: zero,
                no_wire_render: true,
                no_coloring: true,
                no_clear_render: true,
                ..StaticInit::default()
            },
        );
        self.build_dialog(ui, id);

        self.set_rows_hidden(ui, false);
        self.set_rows_hidden(ui, true);
        self.leave_interface_page(ui);
        if self.in_game {
            self.enter_interface_page(ui);
        }
    }

    /// The page column: six buttons (no credits in game) and their
    /// captions.
    #[allow(clippy::too_many_lines)]
    fn build_column(&mut self, ui: &mut Ui, id: NodeId, x: f32, y: f32, step: f32) {
        let half = v3(fb(0x3e05_1eb8), fb(0x3cd4_fdf4), 0.0);
        let keymap_half = v3(fb(0x3e05_1eb8), fb(0x3ccc_cccd), 0.0);
        let (y64, s64) = (f64::from(y), f64::from(step));
        let step2 = r(s64 + s64);
        let step3 = r(s64 * 3.0);
        let step6 = r(s64 * 6.0);
        // The credits row keeps `Step * 4.0` unrounded.
        let ys = [
            y,
            r(y64 - s64),
            r(y64 - f64::from(step2)),
            r(y64 - f64::from(step3)),
            r(y64 - s64 * 4.0),
            r(y64 - f64::from(step6)),
        ];
        let buttons: [(usize, &[u8], Vector, usize); 6] = [
            (1, b"interfaceButton", half, 0),
            (0, b"keymapButton", keymap_half, 1),
            (2, b"VideoButton", half, 2),
            (3, b"AudioButton", half, 3),
            (4, b"CreditsButton", half, 4),
            (5, b"MainMenuButton", half, 5),
        ];
        for (slot, name, extents, row) in buttons {
            if slot == 4 && self.in_game {
                continue;
            }
            let made = create_button(ui, id, name, BUTTON_MESH, v3(x, ys[row], 0.0), extents, 0);
            self.c.page_buttons[slot] = made;
            if slot != 4 {
                with_button(ui, made, |b| b.montage_mode = 1);
            }
        }

        let base = y64 + 0.015;
        let tx = r(f64::from(x) - 0.13);
        let text_ys = [
            r(base),
            r(base - s64),
            r(base - f64::from(step2)),
            r(base - f64::from(step3)),
            r(base - s64 * 4.0),
            r(base - f64::from(step6)),
        ];
        let main_menu_ids = if self.in_game { 0x58b } else { 0x533 };
        let texts: [(usize, &[u8], u32, usize); 6] = [
            (1, b"InterfaceText", 0x52e, 0),
            (0, b"keymapText", 0x52f, 1),
            (2, b"VideoText", 0x5e2, 2),
            (3, b"AudioText", 0x5e3, 3),
            (4, b"CreditsText", 0x532, 4),
            (5, b"mainMenuText", main_menu_ids, 5),
        ];
        for (slot, name, ids, row) in texts {
            if slot == 4 && self.in_game {
                continue;
            }
            let made = create_text(
                ui,
                id,
                name,
                b"ButtonText",
                v3(tx, text_ys[row], 0.0),
                fb(0x3e84_1893),
                fb(0x3cf5_c28f),
                ids,
                1,
                true,
            );
            property_other(ui, made, 0x24, 1, 0);
            self.c.page_texts[slot] = made;
        }
        text_colour(ui, self.c.page_texts[5], IDLE);
    }

    /// Option row `index`: its title, caption, toggle and toggle caption,
    /// slider, value, slider captions and small button with its caption.
    #[allow(clippy::too_many_lines)]
    fn build_option_row(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        index: usize,
        row_y: f32,
        scroll_row_y: f32,
    ) {
        let offset = r(f64::from(u8::try_from(index).unwrap_or(0)) * f64::from(fb(0x3d0f_5c29)));
        let ty = r(f64::from(row_y) - f64::from(offset));

        self.c.opt_title_text[index] = create_text(
            ui,
            id,
            &row_name("opt_titleText ", index),
            b"PageHeader",
            v3(fb(0xbcf5_c28f), ty, 0.0),
            fb(0x3e5c_28f6),
            fb(0x3cf5_c28f),
            0,
            0,
            false,
        );

        let text = create_text(
            ui,
            id,
            &row_name("opt_text ", index),
            b"SubHeader",
            v3(0.0, ty, 0.0),
            fb(0x3e5c_28f6),
            fb(0x3cf5_c28f),
            0,
            0,
            false,
        );
        property_other(ui, text, 0x24, 1, 0);
        with_text(ui, text, |t, _, _| t.pulse_on_hover = false);
        self.c.opt_text[index] = text;

        let toggle = create_toggle(
            ui,
            id,
            &row_name("opt_toggle ", index),
            v3(fb(0x3ed4_7ae1), ty, 0.0),
            v3(fb(0x3c75_c28f), fb(0x3c75_c28f), 0.0),
        );
        property_other(ui, toggle, 0x35, 1, 0);
        with_button(ui, toggle, |b| b.colour = GREY.to_le_bytes());
        self.c.opt_toggle[index] = toggle;

        let toggle_text_y = r(f64::from(ty) - f64::from(fb(0x3b44_9ba6)));
        self.c.opt_toggle_text[index] = create_text(
            ui,
            id,
            &row_name("opt_toggleText ", index),
            b"SubHeader",
            v3(fb(0x3e8f_5c29), toggle_text_y, 0.0),
            fb(0x3e61_47ae),
            fb(0x3cf5_c28f),
            0,
            2,
            false,
        );

        let scroll = create_scroll(
            ui,
            id,
            b"UIScroll",
            &row_name("opt_scroll ", index),
            v3(
                fb(0x3e42_8f5c),
                r(f64::from(scroll_row_y) - f64::from(offset)),
                -1.0,
            ),
            fb(0x3e75_c28f),
            fb(0x3c65_6042),
            0,
            100,
            1,
            true,
        );
        property(ui, scroll, Property::ScrollRange(0, 100));
        with_scroll(ui, scroll, |s| {
            s.step_mode = true;
            s.page = 1.0;
        });
        self.c.opt_scroll[index] = scroll;

        self.c.small_text[index] = create_text(
            ui,
            id,
            &row_name("opt_smallText ", index),
            b"HudSmall",
            v3(fb(0x3e08_3127), ty, 0.0),
            fb(0x3de1_47ae),
            fb(0x3ca3_d70a),
            0,
            2,
            false,
        );

        let scroll_text_y = r(f64::from(ty) + f64::from(fb(0x3c8b_4396)));
        self.c.scroll_text_left[index] = create_text(
            ui,
            id,
            &row_name("opt_scrollTextLeft ", index),
            b"ToolTip",
            v3(fb(0x3e85_1eb8), scroll_text_y, 0.0),
            fb(0x3de1_47ae),
            fb(0x3ca3_d70a),
            0,
            0,
            false,
        );
        self.c.scroll_text_right[index] = create_text(
            ui,
            id,
            &row_name("opt_scrollTextRight ", index),
            b"ToolTip",
            v3(fb(0x3eb7_ced9), scroll_text_y, 0.0),
            fb(0x3de1_47ae),
            fb(0x3ca3_d70a),
            0,
            2,
            false,
        );

        let small = create_wire_button(
            ui,
            id,
            &row_name("opt_smallBtn ", index),
            SELECT_SOUND,
            v3(fb(0x3ea3_d70a), ty, 0.0),
            v3(fb(0x3db8_51ec), fb(0x3c34_3958), 0.0),
            0,
            false,
            0,
            0,
            0,
        );
        with_wire(ui, small, |w| {
            w.colour = GREY.to_le_bytes();
            w.wire_state2 |= 1;
        });
        self.c.small_button[index] = small;

        self.c.small_button_text[index] = create_text(
            ui,
            id,
            &row_name("opt_smallBtnText ", index),
            b"ItemName",
            v3(fb(0x3ea3_d70a), toggle_text_y, 0.0),
            fb(0x3e38_51ec),
            fb(0x3cf5_c28f),
            0,
            1,
            false,
        );
    }

    /// Key row `row`: its two buttons, the command's caption and the two
    /// bindings' captions.
    fn build_key_row(&mut self, ui: &mut Ui, id: NodeId, row: usize) {
        let k = f64::from(u8::try_from(row).unwrap_or(0));
        let button_y = r(0.1745 - (k - 1.0) * 0.059);
        let half = v3(fb(0x3e19_999a), fb(0x3cf1_a9fc), 0.0);
        let primary = create_wire_button(
            ui,
            id,
            &row_name("IBtn_Row", row),
            SELECT_SOUND,
            v3(fb(0x3eae_147b), button_y, -1.0),
            half,
            0,
            false,
            0,
            0,
            0xa,
        );
        with_wire(ui, primary, |w| w.wire_state &= 0xbf);
        self.c.key_btn_rows[row] = primary;
        let alt = create_wire_button(
            ui,
            id,
            &row_name("IBtn_alt_Row", row),
            SELECT_SOUND,
            v3(fb(0x3f23_d70a), button_y, -1.0),
            half,
            0,
            false,
            0,
            0,
            0,
        );
        with_wire(ui, alt, |w| w.edge_mask = 0);
        self.c.key_alt_btn_rows[row] = alt;

        let caption_offset = (k + 0.5) * f64::from(fb(0x3cf1_a9fc));
        let command = create_text(
            ui,
            id,
            &row_name("ICommand_Row", row),
            b"ItemName",
            v3(
                fb(0xbc65_6042),
                r(f64::from(fb(0x3e03_126f)) - caption_offset),
                0.0,
            ),
            fb(0x3e48_b439),
            fb(0x3cf5_c28f),
            0,
            0,
            false,
        );
        property_other(ui, command, 0x24, 1, 0);
        with_text(ui, command, |t, _, _| t.pulse_on_hover = false);
        self.c.command_rows[row] = command;

        let key_y = r(f64::from(fb(0x3e08_3127)) - caption_offset);
        self.c.key_rows[row] = create_text(
            ui,
            id,
            &row_name("IKey_Row", row),
            b"ItemName",
            v3(fb(0x3e2e_147b), key_y, 0.0),
            fb(0x3e14_7ae1),
            fb(0x3c8b_4396),
            0,
            1,
            false,
        );
        self.c.key_alt_rows[row] = create_text(
            ui,
            id,
            &row_name("IKey_alt_Row", row),
            b"ItemName",
            v3(fb(0x3ea1_cac1), key_y, 0.0),
            fb(0x3e14_7ae1),
            fb(0x3c8b_4396),
            0,
            1,
            false,
        );
        property_other(ui, self.c.key_rows[row], 0x24, 1, 0);
        property_other(ui, self.c.key_alt_rows[row], 0x24, 1, 0);
    }

    /// The credits window: twenty paragraph breaks, then the texts
    /// 0x6003f to 0x60048 as one list.
    fn build_credits(&mut self, ui: &mut Ui, id: NodeId) {
        self.c.credits_text = create_rich_text_win(
            ui,
            id,
            b"CreditsMessageText",
            0.5,
            fb(0x3ee1_47ae),
            v3(fb(0xbde7_6c8b), fb(0x3e54_fdf4), -1.0),
            false,
        );
        let mut credits = RenderDisplayList::new();
        if let Some(c) = ui.catalogs.clone() {
            for ids in 0x6003f..=0x60048 {
                load_ids(&mut credits, &c, ids);
            }
        }
        self.credits_speed = 20.0;
        if let Some(w) = self
            .c
            .credits_text
            .and_then(|t| ui.widget_mut::<RichTextWin>(t))
        {
            w.clear_content();
            for _ in 0..0x14 {
                w.append_node(&RdlNode::Paragraph);
            }
            w.append_content(&credits);
        }
    }

    /// The key dialog's three lines, its two buttons and their captions,
    /// all hidden.
    fn build_dialog(&mut self, ui: &mut Ui, id: NodeId) {
        let (w, h) = (fb(0x3e8a_3d71), fb(0x3cf5_c28f));
        let x = fb(0xbc34_3958);
        let lines: [(&[u8], f32, u32); 3] = [
            (b"SetKeyFrameText1", fb(0x3da3_d70a), 0x5e6),
            (b"SetKeyFrameText2", fb(0x3d23_d70a), 0),
            (b"SetKeyFrameText3", fb(0xbbc4_9ba6), 0),
        ];
        for (slot, (name, y, ids)) in lines.into_iter().enumerate() {
            self.c.dialog_text[slot] = create_text(
                ui,
                id,
                name,
                b"ConfirmText",
                v3(x, y, 0.0),
                w,
                h,
                ids,
                1,
                false,
            );
        }
        let half = v3(fb(0x3dcc_cccd), fb(0x3cac_0831), 0.0);
        let left = v3(fb(0xbde5_6042), fb(0xbd8f_5c29), 0.0);
        let right = v3(fb(0x3de5_6042), fb(0xbd8f_5c29), 0.0);
        self.c.overwrite_button = create_wire_button(
            ui,
            id,
            b"SetKeyOverwriteBtn",
            SELECT_SOUND,
            left,
            half,
            0,
            false,
            0,
            0,
            0,
        );
        self.c.dialog_cancel_button = create_wire_button(
            ui,
            id,
            b"SetKeyCancelBtn",
            SELECT_SOUND,
            right,
            half,
            0,
            false,
            0,
            0,
            0,
        );
        with_wire(ui, self.c.dialog_cancel_button, |w| w.wire_state2 |= 1);
        self.c.dialog_button_text[0] = create_text(
            ui,
            id,
            b"keymap_keyBtnText1",
            b"ButtonText",
            left,
            w,
            h,
            0x5e8,
            1,
            false,
        );
        self.c.dialog_button_text[1] = create_text(
            ui,
            id,
            b"keymap_keyBtnText2",
            b"ButtonText",
            right,
            w,
            h,
            0x2148,
            1,
            false,
        );
        for t in self.c.dialog_text {
            hide(ui, t);
        }
        for t in self.c.dialog_button_text {
            hide(ui, t);
        }
        hide(ui, self.c.overwrite_button);
        hide(ui, self.c.dialog_cancel_button);
        hide(ui, self.c.set_key_frame);
    }

    /// `LoadOptList(kOptListPath)`, the file through the data services.
    fn load_opt_list(&mut self, ui: &mut Ui) {
        let rel = OPT_LIST_PATH.strip_prefix("data/").unwrap_or(OPT_LIST_PATH);
        let text = ui.services.data_file(rel.as_bytes());
        let mut seed = |_: i32, _: f32| {};
        self.lists
            .load_opt_list(text.as_deref(), OPT_LIST_PATH, &mut seed);
    }

    /// `LoadKeyList(kKeyListPath)`.
    fn load_key_list(&mut self, ui: &mut Ui) {
        let rel = KEY_LIST_PATH.strip_prefix("data/").unwrap_or(KEY_LIST_PATH);
        let text = ui.services.data_file(rel.as_bytes());
        self.lists
            .load_key_list(text.as_deref(), KEY_LIST_PATH, &self.ctx.keymap);
    }

    // ---- rows ----------------------------------------------------------------

    /// `FillKeyRows(first)`: the thirteen key rows from row `first` of the
    /// current group (nothing when `first` is past the last full page).
    fn fill_key_rows(&mut self, ui: &mut Ui, first: i32) {
        let rows = self.cur_group_rows().to_vec();
        #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
        let len = rows.len() as i32;
        if first.cast_unsigned() > len.wrapping_sub(13).cast_unsigned() {
            return;
        }
        let catalogs = ui.catalogs.clone();
        for index in 0..KEY_ROWS {
            #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
            let at = first + index as i32;
            if at.cast_unsigned() >= len.cast_unsigned() {
                continue;
            }
            let row = rows[usize::try_from(at).unwrap_or(0)];
            let key = self.c.key_rows[index];
            let alt = self.c.key_alt_rows[index];
            text_colour(ui, self.c.command_rows[index], IDLE);
            with_wire(ui, self.c.key_btn_rows[index], |w| w.edge_mask = 0xa);
            let colour = if row.editable { IDLE } else { GREY };
            text_colour(ui, key, colour);
            text_colour(ui, alt, colour);
            let name = |k: &crate::preferences::KeyRowKey| {
                format_binding_name(
                    k.virtual_key,
                    k.modifiers,
                    &self.ctx.key_names,
                    catalogs.as_deref(),
                )
            };
            let (key_text, alt_text) = match row.binding_kind {
                1 => {
                    let n = name(&row.primary);
                    let a = if row.editable { Vec::new() } else { n.clone() };
                    (n, a)
                }
                2 => (name(&row.primary), name(&row.alt)),
                _ => (Vec::new(), Vec::new()),
            };
            with_text(ui, key, |t, ui, id| t.set_text(ui, id, Some(&key_text)));
            with_text(ui, alt, |t, ui, id| t.set_text(ui, id, Some(&alt_text)));
            if row.name_ids > 0 {
                set_text_ids(ui, self.c.command_rows[index], row.name_ids.cast_unsigned());
            } else {
                set_text(ui, self.c.command_rows[index], &[]);
            }
        }
    }

    /// `SetRowsHidden(hide)`: the current page's rows hidden, or shown
    /// again (the option pages then laid out anew).
    fn set_rows_hidden(&mut self, ui: &mut Ui, hide_rows: bool) {
        let c = self.c.clone();
        match self.cur_page {
            0 => {
                for i in 0..KEY_ROWS {
                    if hide_rows {
                        hide(ui, c.key_btn_rows[i]);
                        hide(ui, c.key_alt_btn_rows[i]);
                        property_other(ui, c.key_rows[i], 0x24, 0, 0);
                        property_other(ui, c.key_alt_rows[i], 0x24, 0, 0);
                        hide(ui, c.command_rows[i]);
                        hide(ui, c.key_rows[i]);
                        hide(ui, c.key_alt_rows[i]);
                        hide(ui, c.static_rows[i]);
                    } else {
                        show(ui, c.key_btn_rows[i]);
                        show(ui, c.key_alt_btn_rows[i]);
                        show(ui, c.command_rows[i]);
                        show(ui, c.key_rows[i]);
                        show(ui, c.key_alt_rows[i]);
                        show(ui, c.static_rows[i]);
                        property_other(ui, c.key_rows[i], 0x24, 1, 0);
                        property_other(ui, c.key_alt_rows[i], 0x24, 1, 0);
                    }
                }
                if !hide_rows {
                    show(ui, c.key_scroll);
                    show(ui, c.static_rows[13]);
                    return;
                }
                hide(ui, c.key_scroll);
                hide(ui, c.static_rows[13]);
            }
            1..=3 => {
                for i in 0..OPTION_ROWS {
                    if hide_rows {
                        hide(ui, c.opt_toggle[i]);
                        hide(ui, c.opt_scroll[i]);
                        hide(ui, c.small_button[i]);
                        hide(ui, c.small_button_text[i]);
                        property_other(ui, c.opt_text[i], 0x24, 0, 0);
                    } else {
                        property_other(ui, c.opt_text[i], 0x24, 1, 0);
                    }
                }
                if !hide_rows {
                    self.refresh_rows(ui, -1);
                    return;
                }
            }
            _ => {}
        }
        if hide_rows {
            hide(ui, c.credits_text);
        }
    }

    /// `SetOptionRowGreyed`: a row's captions grey and its controls off, or
    /// back.
    fn set_option_row_greyed(&self, ui: &mut Ui, i: usize, greyed: bool) {
        let colour = if greyed { GREY } else { IDLE };
        text_colour(ui, self.c.opt_text[i], colour);
        text_colour(ui, self.c.small_button_text[i], colour);
        with_wire(ui, self.c.small_button[i], |w| {
            w.use_colour = greyed;
            if let Some(b) = w.button.as_mut() {
                if greyed {
                    b.state &= 0xfb;
                } else {
                    b.state |= 4;
                }
            }
        });
        text_colour(ui, self.c.opt_toggle_text[i], colour);
        with_button(ui, self.c.opt_toggle[i], |b| {
            b.use_colour = greyed;
            if greyed {
                b.bits2 &= 0xfd;
            } else {
                b.bits2 |= 2;
            }
        });
        text_colour(ui, self.c.scroll_text_left[i], colour);
        text_colour(ui, self.c.scroll_text_right[i], colour);
        text_colour(ui, self.c.small_text[i], colour);
    }

    /// `RefreshRows(id)`: the fourteen rows of the current page laid out
    /// from its section (`-1` every row, else the rows of that option).
    #[allow(clippy::too_many_lines)]
    fn refresh_rows(&mut self, ui: &mut Ui, only: i32) {
        let c = self.c.clone();
        if !self.lists.seed_mode {
            with_scroll(ui, c.opt_scroll[0], |s| {
                s.set_size(fb(0x3e75_c28f), fb(0x3c65_6042))
            });
            position3(ui, c.opt_scroll[0], 0.0, 0.0, 0.0);
            with_text(ui, c.opt_text[0], |t, ui, id| {
                t.set_font_by_name(ui, b"SubHeader");
                t.set_width(ui, id, fb(0x3e5c_28f6));
            });
        }
        let mut row = PrefOption::default();
        for index in 0..OPTION_ROWS {
            let mut disabled = false;
            if !self.lists.seed_mode {
                with_button(ui, c.opt_toggle[index], |b| b.bits2 |= 2);
            }
            row.id = -1;
            row.slider = false;
            row.byte15 = false;
            row.header = false;
            if let Some(page_row) = self.page_row(index) {
                row = page_row;
                disabled = match row.id {
                    0x534 => self.ctx.gamma_ability == 0,
                    0x657 => self.ctx.environment_map_ability == 0,
                    0x67f => self.ctx.display.color_bpp == 0x20,
                    0x57f => !self.ctx.single_player,
                    _ => false,
                };
            }
            let row_id = row.id;
            if only != -1 && only != row_id {
                continue;
            }

            property(ui, c.opt_scroll[index], Property::ScrollRange(0, 100));
            for w in [
                c.opt_toggle[index],
                c.opt_title_text[index],
                c.opt_scroll[index],
                c.small_text[index],
                c.scroll_text_left[index],
                c.scroll_text_right[index],
                c.small_button[index],
                c.small_button_text[index],
                c.opt_toggle_text[index],
            ] {
                hide(ui, w);
            }
            if row_id == -1 {
                hide(ui, c.opt_text[index]);
                continue;
            }
            let ids = row_id.cast_unsigned();
            if row.header {
                hide(ui, c.opt_text[index]);
                let text = ids_text(ui, ids);
                set_text(ui, c.opt_title_text[index], &text);
                show(ui, c.opt_title_text[index]);
                continue;
            }
            if disabled {
                continue;
            }

            self.set_option_row_greyed(ui, index, row.byte15 && self.in_game);
            show(ui, c.opt_text[index]);
            let text = ids_text(ui, ids);
            set_text(ui, c.opt_text[index], &text);

            let applied = self.perf_applied_by_ids(row_id);
            if let Some(applied) = applied {
                if row.slider {
                    let value = trunc_to_i32(f64::from(applied) * 100.0);
                    property(ui, c.opt_scroll[index], Property::ScrollValue(value));
                    show(ui, c.opt_scroll[index]);
                } else {
                    let on = f64::from(applied) > 0.999;
                    property_other(ui, c.opt_toggle[index], 0x30, usize::from(on), 0);
                    show(ui, c.opt_toggle[index]);
                    let text = ids_text(ui, if on { 0x65f } else { 0x67a });
                    set_text(ui, c.opt_toggle_text[index], &text);
                    show(ui, c.opt_toggle_text[index]);
                }
            }

            match row_id {
                0x534 => {
                    if let Some(applied) = applied {
                        let value = wide(&format!("{:3.2}", f64::from(applied)));
                        set_text(ui, c.small_text[index], &value);
                        show(ui, c.small_text[index]);
                    }
                }
                0x536 => self.place_video_mode(ui, index),
                0x57e => {
                    show(ui, c.small_button[index]);
                    show(ui, c.small_button_text[index]);
                    let text = ids_text(ui, 0x60b);
                    set_text(ui, c.small_button_text[index], &text);
                    let same = self.sel_width == self.cur_width && self.sel32 == self.cur32;
                    text_colour(
                        ui,
                        c.small_button_text[index],
                        if same { GREY } else { IDLE },
                    );
                    with_wire(ui, c.small_button[index], |w| {
                        w.use_colour = same;
                        if let Some(b) = w.button.as_mut() {
                            if same {
                                b.state &= 0xfb;
                            } else {
                                b.state |= 4;
                            }
                        }
                    });
                }
                0x57f => {
                    // The option is not null-tested in the reference.
                    let applied = applied.unwrap_or(0.0);
                    property(ui, c.opt_scroll[index], Property::ScrollRange(0, 3));
                    let level = trunc_to_i32(f64::from(applied) + f64::from(applied));
                    property(ui, c.opt_scroll[index], Property::ScrollValue(level));
                    show(ui, c.opt_scroll[index]);
                    let text = match u32::try_from(level) {
                        Ok(l) if l <= 3 => ids_text(ui, 0x702 + l),
                        _ => Vec::new(),
                    };
                    set_text(ui, c.small_text[index], &text);
                    show(ui, c.small_text[index]);
                }
                0x687 => {
                    if applied.is_some() {
                        property(ui, c.opt_scroll[index], Property::ScrollRange(8, 0x20));
                        #[allow(clippy::cast_precision_loss)]
                        let count = self.ctx.max_submitted_sounds as f32;
                        let value = trunc_to_i32(f64::from(count));
                        property(ui, c.opt_scroll[index], Property::ScrollValue(value));
                        show(ui, c.opt_scroll[index]);
                        let text = wide(&format!("{:1.0}", f64::from(count)));
                        set_text(ui, c.small_text[index], &text);
                        show(ui, c.small_text[index]);
                    }
                }
                0x767 => {
                    with_scroll(ui, c.opt_scroll[index], |s| {
                        s.set_size(fb(0x3eb4_bc6a), fb(0x3c93_74bc));
                    });
                    position3(ui, c.opt_scroll[index], fb(0xbde1_47ae), 0.0, 0.0);
                    if !self.applying_preset
                        && let Some(general) = self.ctx.perf.applied(b"general_graphics")
                    {
                        let value = trunc_to_i32(f64::from(general) * 100.0);
                        property(ui, c.opt_scroll[index], Property::ScrollValue(value));
                    }
                    show(ui, c.opt_scroll[index]);
                    with_text(ui, c.opt_text[index], |t, ui, id| {
                        t.set_width(ui, id, fb(0x3e3d_70a4));
                        t.set_font_by_name(ui, b"PageHeader");
                    });
                }
                _ => {
                    if applied.is_none() {
                        hide(ui, c.opt_text[index]);
                        let text = ids_text(ui, ids);
                        set_text(ui, c.opt_text[index], &text);
                        show(ui, c.opt_text[index]);
                    }
                }
            }
        }
    }

    /// `RefreshRows`' resolution row: the video mode cycle, its frames and
    /// caption moved to row `index`, the cycle at the current display
    /// mode's entry (the last match wins; none: the display's own size and
    /// the first entry).
    fn place_video_mode(&mut self, ui: &mut Ui, index: usize) {
        let c = self.c.clone();
        let y = r(-f64::from(u8::try_from(index).unwrap_or(0)) * f64::from(fb(0x3d0f_5c29)));
        let y2 = y + y;
        position3(ui, c.video_mode_cycle, 0.0, y, 0.0);
        position3(ui, c.video_box, 0.0, y2, 0.0);
        position3(ui, c.video_box2, 0.0, y2, 0.0);
        position3(ui, c.video_mode_text, 0.0, y, 0.0);
        property(
            ui,
            c.video_mode_cycle,
            Property::ScrollRange(0, self.mode_count - 1),
        );

        let display = self.ctx.display;
        let mut text = None;
        for mode in 0..MODE_COUNT {
            if !self.mode_avail[mode] {
                continue;
            }
            let m = self.modes[mode];
            if display.width != m.width || display.color_bpp != m.bpp {
                continue;
            }
            self.cur_width = m.width;
            self.sel_width = m.width;
            self.sel32 = m.bpp == 0x20;
            text = Some(format!("{} x {}", m.width, m.height));
            let at = self.cycle_index[mode];
            property(ui, c.video_mode_cycle, Property::ScrollValue(at));
        }
        let mut text = text.unwrap_or_else(|| {
            self.cur_width = display.width;
            self.sel_width = display.width;
            self.sel32 = display.color_bpp == 0x20;
            property(ui, c.video_mode_cycle, Property::ScrollValue(0));
            format!("{} x {}", display.width, display.height)
        });
        if self.big_vram {
            text = format!("{text} x {}", if self.sel32 { 32 } else { 16 });
        }
        set_text(ui, c.video_mode_text, &wide(&text));
        for w in [
            c.video_mode_cycle,
            c.video_box,
            c.video_box2,
            c.video_mode_text,
        ] {
            show(ui, w);
        }
    }

    /// `TakeSnapshot`: each row's value (headers -1, sliders their value,
    /// toggles 0 or 1); the three preview sounds stopped.
    fn take_snapshot(&mut self, ui: &mut Ui) {
        if self.lists.seed_mode {
            return;
        }
        let mut row = PrefOption::default();
        for index in 0..OPTION_ROWS {
            if let Some(page_row) = self.page_row(index) {
                row = page_row;
            }
            self.snapshot[index] = if row.header {
                -1.0
            } else if row.slider {
                let scroll = if row.id == 0x536 {
                    self.c.video_mode_cycle
                } else {
                    self.c.opt_scroll[index]
                };
                let value = property(ui, scroll, Property::GetScrollValue);
                #[allow(clippy::cast_possible_truncation, clippy::cast_precision_loss)]
                let v = i64::from(value as u32) as f32;
                v
            } else {
                let mut value = 0i32;
                property(
                    ui,
                    self.c.opt_toggle[index],
                    Property::GetToggleState(&mut value),
                );
                let v = f64::from(value.cast_unsigned());
                if v < 0.999 { 0.0 } else { 1.0 }
            };
        }
        for slot in [0x1e, 0x1f, 0x20] {
            ui.services.stop_ui_sound_slot(slot);
        }
    }

    // ---- pages -----------------------------------------------------------------

    /// `SelectGroup(group)`: the key group's rows, the bar when they need
    /// one, the tab's caption lit.
    fn select_group(&mut self, ui: &mut Ui, group: i32) {
        self.cur_group = group;
        let c = self.c.clone();
        property(ui, c.key_scroll, Property::ScrollValue(0));
        #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
        let count = self.cur_group_rows().len() as i32;
        property(ui, c.key_scroll, Property::ScrollRange(0, count - 1));
        for index in 0..KEY_ROWS {
            #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
            let has = (index as i32) < count;
            wire_state_bits(ui, c.key_btn_rows[index], has, 4);
            wire_state_bits(ui, c.key_alt_btn_rows[index], has, 4);
            for w in [
                c.command_rows[index],
                c.key_rows[index],
                c.key_alt_rows[index],
            ] {
                if has {
                    show(ui, w);
                } else {
                    hide(ui, w);
                }
            }
        }
        self.fill_key_rows(ui, 0);
        if self.cur_group_rows().len() > 13 {
            show(ui, c.key_scroll);
        } else {
            hide(ui, c.key_scroll);
        }
        for (index, t) in c.group_texts.iter().enumerate() {
            if usize::try_from(group).ok() != Some(index) {
                text_colour(ui, *t, IDLE);
            }
        }
        if let Some(t) = usize::try_from(group)
            .ok()
            .and_then(|g| c.group_texts.get(g))
        {
            text_colour(ui, *t, AMBER);
        }
    }

    /// The first call of an `Enter…Page` outside a flip: the page made
    /// current and the flip started. Answers whether the page is entered
    /// now (the flip's second call).
    fn begin_enter(&mut self, ui: &mut Ui, page: i32, text: usize) -> Option<bool> {
        if self.cur_page == page && !self.flipping {
            return None;
        }
        if !self.flipping {
            self.cur_page = page;
            self.flipping = true;
            text_colour(ui, self.c.page_texts[text], AMBER);
            return Some(false);
        }
        Some(true)
    }

    /// `ResetCancelGrey` and the page's common controls shown.
    fn show_page_common(&mut self, ui: &mut Ui, title_ids: u32) {
        self.dirty = false;
        let c = self.c.clone();
        text_colour(ui, c.cancel_text, GREY);
        with_wire(ui, c.cancel_button, |w| {
            w.use_colour = true;
            if let Some(b) = w.button.as_mut() {
                b.state &= 0xfb;
            }
        });
        for w in [
            c.cancel_button,
            c.cancel_text,
            c.defaults_button,
            c.defaults_text,
            c.title,
        ] {
            show(ui, w);
        }
        if title_ids == 0x68d {
            show(ui, c.static_bar);
        }
        set_text_ids(ui, c.title, title_ids);
    }

    /// `EnterKeyMapPage`.
    fn enter_key_map_page(&mut self, ui: &mut Ui) {
        if self.cur_page == 0 && !self.flipping {
            return;
        }
        if !self.flipping {
            self.cur_page = 0;
            self.flipping = true;
            // The caption's text is read into a buffer nobody uses.
            text_colour(ui, self.c.page_texts[0], AMBER);
            return;
        }
        self.select_group(ui, 0);
        let c = self.c.clone();
        for i in 0..KEY_ROWS {
            for w in [
                c.command_rows[i],
                c.key_rows[i],
                c.key_btn_rows[i],
                c.key_alt_rows[i],
                c.key_alt_btn_rows[i],
            ] {
                show(ui, w);
            }
        }
        for w in c.static_rows {
            show(ui, w);
        }
        for i in 0..3 {
            show(ui, c.group_texts[i]);
            show(ui, c.group_buttons[i]);
        }
        for w in [
            c.key_column1,
            c.key_column2,
            c.cancel_button,
            c.cancel_text,
            c.defaults_button,
            c.defaults_text,
            c.title,
        ] {
            show(ui, w);
        }
        set_text_ids(ui, c.title, 0x691);
        self.set_rows_hidden(ui, false);
        hide(ui, c.credits_text);
    }

    /// `EnterInterfacePage`.
    fn enter_interface_page(&mut self, ui: &mut Ui) {
        if self.begin_enter(ui, 1, 1) != Some(true) {
            return;
        }
        self.refresh_rows(ui, -1);
        self.take_snapshot(ui);
        self.show_page_common(ui, 0x68e);
        self.set_rows_hidden(ui, false);
        self.probe_modes();
    }

    /// `EnterVideoPage`.
    fn enter_video_page(&mut self, ui: &mut Ui) {
        if self.begin_enter(ui, 2, 2) != Some(true) {
            return;
        }
        self.refresh_rows(ui, -1);
        self.take_snapshot(ui);
        self.show_page_common(ui, 0x68d);
        self.set_rows_hidden(ui, false);
    }

    /// `EnterAudioPage`.
    fn enter_audio_page(&mut self, ui: &mut Ui) {
        if self.begin_enter(ui, 3, 3) != Some(true) {
            return;
        }
        self.refresh_rows(ui, -1);
        self.take_snapshot(ui);
        self.show_page_common(ui, 0x68f);
        self.set_rows_hidden(ui, false);
    }

    /// `EnterCreditsPage`.
    fn enter_credits_page(&mut self, ui: &mut Ui) {
        if self.begin_enter(ui, 4, 4) != Some(true) {
            return;
        }
        let c = self.c.clone();
        set_text_ids(ui, c.defaults_text, 0x765);
        self.credits_scroll = 0.0;
        for w in [c.credits_text, c.defaults_button, c.defaults_text, c.title] {
            show(ui, w);
        }
        set_text_ids(ui, c.title, 0x532);
        self.set_rows_hidden(ui, false);
    }

    fn enter_page(&mut self, ui: &mut Ui, page: i32) {
        match page {
            0 => self.enter_key_map_page(ui),
            1 => self.enter_interface_page(ui),
            2 => self.enter_video_page(ui),
            3 => self.enter_audio_page(ui),
            4 => self.enter_credits_page(ui),
            _ => {}
        }
    }

    /// The common tail of the three option pages' leave.
    fn hide_page_common(&self, ui: &mut Ui) {
        let c = &self.c;
        for w in [
            c.title,
            c.cancel_button,
            c.cancel_text,
            c.defaults_button,
            c.defaults_text,
        ] {
            hide(ui, w);
        }
    }

    /// `HideOptionRow`.
    fn hide_option_row(&self, ui: &mut Ui, i: usize) {
        let c = &self.c;
        for w in [
            c.opt_title_text[i],
            c.opt_text[i],
            c.opt_toggle[i],
            c.opt_scroll[i],
            c.small_text[i],
            c.scroll_text_left[i],
            c.scroll_text_right[i],
            c.small_button[i],
            c.small_button_text[i],
            c.opt_toggle_text[i],
        ] {
            hide(ui, w);
        }
    }

    /// `LeaveKeyMapPage` (a changed page asks first: `ShowConfirmDialog`,
    /// which comes with the option edits).
    fn leave_key_map_page(&mut self, ui: &mut Ui) {
        if self.dirty {
            return;
        }
        let c = self.c.clone();
        for i in 0..KEY_ROWS {
            for w in [
                c.command_rows[i],
                c.key_rows[i],
                c.key_alt_rows[i],
                c.key_btn_rows[i],
                c.key_alt_btn_rows[i],
            ] {
                hide(ui, w);
            }
        }
        for w in c.static_rows {
            hide(ui, w);
        }
        for i in 0..3 {
            hide(ui, c.group_texts[i]);
            hide(ui, c.group_buttons[i]);
        }
        for w in [
            c.key_column1,
            c.key_column2,
            c.key_scroll,
            c.cancel_button,
            c.cancel_text,
            c.defaults_button,
            c.defaults_text,
            c.title,
        ] {
            hide(ui, w);
        }
        text_colour(ui, c.page_texts[0], IDLE);
    }

    /// `LeaveInterfacePage`.
    fn leave_interface_page(&mut self, ui: &mut Ui) {
        if self.dirty {
            return;
        }
        for i in 0..OPTION_ROWS {
            self.hide_option_row(ui, i);
            with_button(ui, self.c.opt_toggle[i], |b| b.bits2 |= 2);
        }
        let c = &self.c;
        for w in [
            c.video_mode_cycle,
            c.video_box,
            c.video_box2,
            c.video_mode_text,
        ] {
            hide(ui, w);
        }
        self.hide_page_common(ui);
        text_colour(ui, self.c.page_texts[1], IDLE);
    }

    /// `LeaveVideoPage`.
    fn leave_video_page(&mut self, ui: &mut Ui) {
        if self.dirty {
            return;
        }
        for i in 0..OPTION_ROWS {
            self.hide_option_row(ui, i);
        }
        self.hide_page_common(ui);
        hide(ui, self.c.static_bar);
        text_colour(ui, self.c.page_texts[2], IDLE);
    }

    /// `LeaveAudioPage`.
    fn leave_audio_page(&mut self, ui: &mut Ui) {
        if self.dirty {
            return;
        }
        for i in 0..OPTION_ROWS {
            self.hide_option_row(ui, i);
        }
        self.hide_page_common(ui);
        text_colour(ui, self.c.page_texts[3], IDLE);
    }

    /// `LeaveCreditsPage`.
    fn leave_credits_page(&mut self, ui: &mut Ui) {
        let c = self.c.clone();
        hide(ui, c.credits_text);
        text_colour(ui, c.page_texts[4], IDLE);
        set_text_ids(ui, c.defaults_text, 0x5e4);
        hide(ui, c.defaults_button);
        hide(ui, c.defaults_text);
        hide(ui, c.title);
    }

    /// `LeaveCurrentPage`.
    fn leave_current_page(&mut self, ui: &mut Ui) {
        match self.cur_page {
            0 => self.leave_key_map_page(ui),
            1 => self.leave_interface_page(ui),
            2 => self.leave_video_page(ui),
            3 => self.leave_audio_page(ui),
            4 => self.leave_credits_page(ui),
            _ => {}
        }
    }

    /// `GotoRequestedPage`: the requested page entered; 5 (Main Menu)
    /// slides the column out from the menu, 6 closes at once. In game both
    /// close the window, which comes with the in-game opener.
    fn goto_requested_page(&mut self, ui: &mut Ui) {
        if self.req_page != 5 && self.req_page != 6 {
            ui.services.play_or_rewind_ui_sound_slot(2);
            ui.services.play_or_rewind_ui_sound_slot(5);
        }
        match self.req_page {
            0..=4 => self.enter_page(ui, self.req_page),
            5 => {
                if !self.in_game {
                    self.closing_anim = true;
                    self.anim = 0.0;
                    for slot in [0, 1, 2, 3, 5] {
                        with_button(ui, self.c.page_buttons[slot], |b| b.bits2 &= 0xfd);
                    }
                    self.leave_current_page(ui);
                    self.save_perf_options_requested = true;
                    return;
                }
                self.closed = true;
                self.screen_up = false;
                self.save_perf_options_requested = true;
            }
            6 => {
                self.closed = true;
                self.screen_up = false;
                if self.in_game {
                    self.save_perf_options_requested = true;
                }
            }
            _ => {}
        }
    }

    /// `SetProperty`: the clicks (0x26) of the column, the key groups' tabs
    /// and the credits' restart. The rest of the reference's messages (the
    /// rows' tooltips, the sliders, the toggles, the dialogs) are not ported
    /// yet (D1106).
    fn child_property(&mut self, ui: &mut Ui, msg: i32, from: NodeId) -> isize {
        if msg == 0x4f && self.anim < 0.0 {
            return 1;
        }
        if self.flipping {
            return 0;
        }
        if self.confirm_mode || self.video_confirm || self.key_capture {
            return 0;
        }
        if msg != 0x26 {
            return 0;
        }
        let source = Some(from);
        match self.cur_page {
            0 => {
                for group in 0..3 {
                    if source == self.c.group_buttons[group] {
                        self.select_group(ui, i32::try_from(group).unwrap_or(0));
                    }
                }
            }
            4 if source == self.c.defaults_button => {
                self.credits_scroll = 0.0;
            }
            _ => {}
        }
        for page in 0..5 {
            if source != self.c.page_buttons[page] {
                continue;
            }
            let page = i32::try_from(page).unwrap_or(0);
            if self.cur_page != page {
                self.req_page = page;
                self.leave_current_page(ui);
                if !self.dirty {
                    self.goto_requested_page(ui);
                }
            }
            break;
        }
        if source.is_some() && (source == self.c.close_button || source == self.c.page_buttons[5]) {
            if self.cur_page == 5 {
                return 0;
            }
            self.req_page = 5;
            self.leave_current_page(ui);
            if !self.dirty {
                self.goto_requested_page(ui);
            }
        }
        0
    }

    /// One step of the column's slide (`X = (6 - i) * 0.1 + anim`).
    fn place_column(&self, ui: &mut Ui) {
        let (buttons, texts) = self.column();
        for (index, step) in (1..=6).rev().enumerate() {
            let offset = f64::from(step) * 0.1;
            let x = offset + self.anim;
            if x > 0.0 {
                position3(ui, buttons[index], 0.0, 0.0, 0.0);
                position3(ui, texts[index], 0.0, 0.0, 0.0);
            } else {
                position3(ui, buttons[index], r(x), 0.0, 0.0);
                position3(ui, texts[index], r(offset + self.anim), 0.0, 0.0);
            }
        }
    }

    /// `Update` after the children's: the slides, the page flip, the
    /// credits' roll.
    fn update(&mut self, ui: &mut Ui) {
        let mut dt = ui.frame_time;
        if dt > 0.2 {
            dt = 0.2;
        }
        let mut closing = false;
        if self.anim < 1.0 && !self.in_game {
            if self.closing_anim {
                closing = true;
            } else {
                self.anim += dt * 1.5;
                self.place_column(ui);
                if self.anim > 0.0 {
                    self.enter_interface_page(ui);
                    self.enter_interface_page(ui);
                    position3(ui, self.c.big_box, 0.0, 0.0, 0.0);
                    self.flipping = false;
                    self.anim = 1.0;
                } else {
                    position3(ui, self.c.big_box, r(self.anim * -1.2), 0.0, 0.0);
                }
                if self.anim > -0.8 && self.anim < -0.1 {
                    ui.services.play_ui_sound_slot(SWISH_SLOT);
                }
            }
        } else if self.closing_anim && !self.in_game {
            closing = true;
        }

        if closing {
            if dt > 0.03 {
                dt = 0.03;
            }
            self.anim -= dt * 1.5;
            self.place_column(ui);
            if self.anim + 0.6 > 0.0 {
                ui.services.play_ui_sound_slot(SWISH_SLOT);
            }
            if self.anim < -1.0 {
                let (buttons, texts) = self.column();
                for index in 0..6 {
                    hide(ui, buttons[index]);
                    hide(ui, texts[index]);
                }
                self.closed = true;
                self.screen_up = false;
                return;
            }
            position3(ui, self.c.big_box, r(self.anim * -1.2), 0.0, 0.0);
        }

        if self.in_game {
            self.enter_page(ui, self.cur_page);
            self.flipping = false;
        } else if self.flipping {
            if dt > 0.03 {
                dt = 0.03;
            }
            self.flip_angle += dt * self.flip_speed;
            if self.flip_angle >= 180.0 {
                self.flip_angle = 0.0;
                self.enter_page(ui, self.cur_page);
                self.flip_half = !self.flip_half;
                self.flipping = false;
            }
            let mut angle = self.flip_angle;
            if self.flip_half {
                angle += 180.0;
            }
            let rotation = rotate_matrix_x(r(angle));
            if let Some(mesh) = self
                .c
                .big_box
                .and_then(|b| ui.win_control_mut(b))
                .and_then(|w| w.mesh.as_mut())
            {
                mesh.set_orientation(rotation);
            }
        }

        if self.cur_page == 4 && !self.flipping {
            self.credits_scroll += dt * self.credits_speed;
            let y = trunc_to_i32(self.credits_scroll);
            if let Some(t) = self.c.credits_text {
                ui.with_widget(t, (), |w, ui| {
                    if let Some(w) = w.as_any_mut().downcast_mut::<RichTextWin>() {
                        w.base.set_scroll_origin(ui, 0, y);
                    }
                });
            }
        }
        // The video mode's confirm countdown comes with the video mode
        // change (D1106).
    }
}

impl NnPreferences {
    /// The window as the main menu opens it (`WinFactoryCreate` with a
    /// non-null payload).
    #[must_use]
    pub fn from_menu(ctx: PreferencesContext) -> Self {
        NnPreferences {
            control: WinControl::new(),
            state: PreferencesState::new(ctx),
        }
    }
}

impl Widget for NnPreferences {
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

    /// `Create`: the children (made with the window in its node, so that
    /// they find it as their parent control), then the offset (0, 0, -1),
    /// the position, the draw layer 100 and sub-layer 6 and `Win3D::Create`.
    /// The window is made from the main menu (the payload the reference
    /// tests for null is [`NnPreferences::from_menu`]'s).
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        let mut state = std::mem::take(&mut self.state);
        ui.lend(id, self, |ui| state.build(ui, id));
        self.state = state;
        base::set_offset(ui, id, self, v3(0.0, 0.0, -1.0));
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        self.set_position(ui, id, position);
        if let Some(n) = ui.node_mut(id) {
            n.draw_layer = 0x64;
            n.draw_sub_layer = 6;
        }
        self.state.creating = false;
        let ok = ui.base_create(id, parent);
        if !ok {
            self.state.closed = true;
        }
        ok
    }

    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.control);
    }

    /// `Update`: the children, then the window's own.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        self.state.update(ui);
    }

    /// `Draw`: every visible child in order (the overlay batch flushes
    /// before the children and before the key dialog's frame are the
    /// renderer's, D1106).
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        for child in ui.children(id) {
            if ui.node(child).is_some_and(|n| n.flags & VISIBLE != 0) {
                ui.draw_node(child, host);
            }
        }
    }

    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        base::mouse_event(ui, id, self, event)
    }

    /// `Slot23`: nothing ("OnMessage").
    fn broadcast(&mut self, _ui: &mut Ui, _id: NodeId, _msg: u32, _a: u32, _b: u32) -> bool {
        false
    }

    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        // The reference's `SetProperty` answers every message itself; none
        // reaches `WinControl`'s.
        let _ = id;
        match property {
            Property::FromChild { msg, from, .. } => self.state.child_property(ui, msg, from),
            Property::ChildToolTip(_) => isize::from(self.state.anim < 0.0),
            _ => 0,
        }
    }
}
