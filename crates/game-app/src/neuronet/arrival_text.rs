//! The arrival caption (`NnArrivalText`, `nn_modal.cpp`, the factory class
//! "NN_ArrivalText"): "<SYSTEM> SYSTEM." and its variants, typed out on
//! arrival and torn down after a hold time.
//!
//! The port so far: the construction (`NnArrivalText::NnArrivalText` and
//! `Create`), `EnsureNnArrivalText` ([`ensure`]), `ShowSystemArrival`
//! ([`show_system_arrival`]) and `NnArrivalTextShowSystemNow`
//! ([`show_system_now`]). The caption's `Update` (the letterbox wait, the
//! delay and hold countdowns, the typing, the teardown when a NeuroNet
//! panel, the mission-objective banner or the comm window is up) and the
//! other `Show*` entry points come with the NeuroNet panels (D1135).
//!
//! The player's system's `strid_name` (`PlayerSystemStridName`:
//! `Universe::get_system(GetPlayerStarSystemId())`'s, 0 without a system)
//! is the caller's argument.

use neuron_client::ui::children::{TextInit, create_text_control};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::factory::Factory;
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::pulse::palette;
use neuron_client::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};
use neuron_core::catalog::TextCatalogs;
use neuron_core::math::{Matrix, Vector};

/// The factory class and the window's name.
pub const CLASS: &[u8] = b"NN_ArrivalText";
/// The class name as the factory registers it.
const CLASS_NAME: &str = "NN_ArrivalText";

/// The formats: "%s, %s System." (a name and the system), "%s System."
/// and "%s System, %s Space." (the system and its region).
pub const IDS_NAMED_FORMAT: u32 = 0x55c;
pub const IDS_SYSTEM_FORMAT: u32 = 0x55d;
pub const IDS_REGION_FORMAT: u32 = 0x55e;

/// The formats' buffer, in characters (`kArrivalFormatChars`).
const FORMAT_CHARS: usize = 0x80;
/// `G_NnTextScratchCapacity`: each name gets half of scratch A.
const SCRATCH_CHARS: usize = 0x1000;

/// The window (`NnArrivalText`).
#[derive(Default)]
pub struct NnArrivalText {
    pub control: WinControl,
    /// `+0x32c` the "ArrivalText" caption.
    pub caption: Option<NodeId>,
    /// `+0x330`: the hold time once the caption is fully typed.
    pub hold_time: f32,
    /// `+0x334`: the delay before the caption shows.
    pub delay: f32,
    /// `+0x338`: the caption waits for the letterbox to go away.
    pub wait_for_letterbox: bool,
}

impl Widget for NnArrivalText {
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
    /// `NnArrivalText::Create`: the caption ("PageHeader" at (-0.45, 0.24,
    /// 0), 0.95 x 0.06, anchored at its corner) in the mint colour and
    /// hidden, the offset (0, 0, -1), the position again, the hold time
    /// 2 and the delay 4, then `Win3D::Create` under the window's current
    /// parent (not the argument).
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        _parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        let mut caption = None;
        ui.lend(id, self, |ui| {
            caption = create_text_control(
                ui,
                id,
                b"ArrivalText",
                &TextInit {
                    style: Some(b"PageHeader"),
                    position: Vector::new(
                        f32::from_bits(0xbee6_6666),
                        f32::from_bits(0x3e75_c28f),
                        0.0,
                    ),
                    width: f32::from_bits(0x3f73_3333),
                    height: f32::from_bits(0x3d75_c28f),
                    ids: 0,
                    justification: 0,
                    tooltip: 0,
                    corner_anchor: true,
                    ..TextInit::default()
                },
            );
        });
        self.caption = caption;
        if let Some(c) = caption {
            ui.property(c, Property::Colour(palette::MINT.to_le_bytes(), true));
            if let Some(n) = ui.node_mut(c) {
                n.flags &= !(ACTIVE | VISIBLE);
            }
        }
        base::set_offset(ui, id, self, Vector::new(0.0, 0.0, -1.0));
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        base::set_position(ui, id, &mut self.control, position);
        self.hold_time = 2.0;
        self.delay = 4.0;
        let parent = ui.parent(id);
        ui.base_create(id, parent)
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
    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
        base::broadcast(ui, id, self, msg)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        base::property(ui, id, self, property)
    }
}

fn new_window() -> Box<dyn Widget> {
    Box::new(NnArrivalText::default())
}

/// `WINFACTORY<NN_ArrivalText>`.
pub fn register(factory: &mut Factory) {
    factory.register(CLASS_NAME, new_window);
}

/// `EnsureNnArrivalText`: the window in `window` while it lives,
/// otherwise `WinFactoryCreate("NN_ArrivalText", "NN_ArrivalText", 0, 0)`
/// into `window`; `None` when the factory makes nothing.
pub fn ensure(ui: &mut Ui, window: &mut Option<NodeId>) -> Option<NodeId> {
    if let Some(id) = *window
        && ui.widget::<NnArrivalText>(id).is_some()
    {
        return Some(id);
    }
    *window = ui.create_by_class(Some(CLASS), CLASS, None, &mut Payload::None);
    *window
}

/// `NnArrivalText::DestroySelf`: each child's tree deleted, the window's
/// `Destroy`, then the window deleted (its destructor clears
/// `G_NnArrivalText`, here `window`).
fn destroy_self(ui: &mut Ui, window: &mut Option<NodeId>, id: NodeId) {
    for child in ui.children(id) {
        ui.delete_tree(child);
    }
    ui.destroy(id);
    ui.delete(id);
    *window = None;
}

/// `NnArrivalTextDestroy`: nothing without the window, else
/// `DestroySelf`. The location controller calls it on every exit.
pub fn destroy(ui: &mut Ui, window: &mut Option<NodeId>) {
    if let Some(id) = *window
        && ui.widget::<NnArrivalText>(id).is_some()
    {
        destroy_self(ui, window, id);
    }
}

/// `GetIdsStringWWithRawFallback(ids, buffer, chars)`: the text without
/// its terminator, empty without the catalogs.
fn ids_text(catalogs: Option<&TextCatalogs>, ids: u32, chars: usize) -> Vec<u16> {
    let Some(c) = catalogs else {
        return Vec::new();
    };
    let mut buffer = vec![0; chars];
    let count = c.string_with_card_fallback_into(ids, &mut buffer);
    buffer.truncate(count);
    // The copy stops at an embedded terminator; so does every reader.
    if let Some(end) = buffer.iter().position(|&u| u == 0) {
        buffer.truncate(end);
    }
    buffer
}

/// `_swprintf` of the formats these captions use: each `%s` takes the next
/// argument and `%%` is a percent sign; any other `%` is copied as text
/// (the three formats have no other conversion; D1136).
#[must_use]
pub fn format_strings(format: &[u16], args: &[&[u16]]) -> Vec<u16> {
    const PERCENT: u16 = b'%' as u16;
    const S: u16 = b's' as u16;
    let mut out = Vec::new();
    let mut args = args.iter();
    let mut i = 0;
    while i < format.len() {
        let unit = format[i];
        match (unit, format.get(i + 1).copied()) {
            (PERCENT, Some(S)) => {
                out.extend_from_slice(args.next().copied().unwrap_or_default());
                i += 2;
            }
            (PERCENT, Some(PERCENT)) => {
                out.push(PERCENT);
                i += 2;
            }
            _ => {
                out.push(unit);
                i += 1;
            }
        }
    }
    out
}

/// `CharUpperW`: each character's simple uppercase mapping when it is a
/// single character of the basic plane; surrogates and the characters
/// whose uppercase is longer stay (D1136).
#[must_use]
pub fn char_upper(text: &[u16]) -> Vec<u16> {
    text.iter()
        .map(|&unit| {
            let Some(ch) = char::from_u32(u32::from(unit)) else {
                return unit;
            };
            let mut upper = ch.to_uppercase();
            match (upper.next(), upper.next()) {
                (Some(u), None) => u16::try_from(u32::from(u)).unwrap_or(unit),
                _ => unit,
            }
        })
        .collect()
}

/// The region caption of a system's `strid_name` (`ShowSystemArrival`'s
/// switch over 0x30001..0x30018), 0 for none.
#[must_use]
pub fn region_ids(system_strid: i32) -> u32 {
    match system_strid.cast_unsigned().wrapping_sub(0x30001) {
        0..=4 => 0x4e1,
        5..=10 => 0x4e2,
        11..=16 => 0x4e4,
        17..=0x17 => 0x4e3,
        _ => 0,
    }
}

/// `NnArrivalText::ShowSystemArrival` on the window `id`: it waits for the
/// letterbox; with the system in a region, "%s System, %s Space." with the
/// system and the region, otherwise "%s System." with the system and the
/// delay 2; upper case, the caption's text. `reset_delay` sets the delay
/// to 2 either way. Nothing past the letterbox wait without a system.
pub fn show_system_arrival(ui: &mut Ui, id: NodeId, system_strid: i32, reset_delay: bool) {
    let Some(w) = ui.widget_mut::<NnArrivalText>(id) else {
        return;
    };
    w.wait_for_letterbox = true;
    let caption = w.caption;
    if system_strid == 0 {
        return;
    }
    let region = region_ids(system_strid);
    let catalogs = ui.catalogs.clone();
    let catalogs = catalogs.as_deref();
    let half = SCRATCH_CHARS / 2;
    let system = ids_text(catalogs, system_strid.cast_unsigned(), half);
    let text = if region != 0 {
        let format = ids_text(catalogs, IDS_REGION_FORMAT, FORMAT_CHARS);
        let region = ids_text(catalogs, region, half);
        format_strings(&format, &[&system, &region])
    } else {
        let format = ids_text(catalogs, IDS_SYSTEM_FORMAT, FORMAT_CHARS);
        format_strings(&format, &[&system])
    };
    let text = char_upper(&text);
    if let Some(c) = caption {
        ui.property(c, Property::SetText(Some(&text)));
    }
    if let Some(w) = ui.widget_mut::<NnArrivalText>(id)
        && (region == 0 || reset_delay)
    {
        w.delay = 2.0;
    }
}

/// `NnArrivalTextShowSystemNow`: [`ensure`], then
/// `ShowSystemArrival(true)` with the player's system's `strid_name`.
pub fn show_system_now(ui: &mut Ui, window: &mut Option<NodeId>, system_strid: i32) {
    if let Some(id) = ensure(ui, window) {
        show_system_arrival(ui, id, system_strid, true);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn w(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn the_format_takes_the_names_in_order() {
        assert_eq!(
            format_strings(&w("%s System, %s Space."), &[&w("Omicron"), &w("Edge")]),
            w("Omicron System, Edge Space.")
        );
        assert_eq!(format_strings(&w("100%% %d"), &[]), w("100% %d"));
        assert_eq!(format_strings(&w("%s."), &[]), w("."));
    }

    #[test]
    fn upper_case_maps_single_characters_only() {
        assert_eq!(char_upper(&w("Omicron major é ß")), w("OMICRON MAJOR É ß"));
    }

    #[test]
    fn the_regions_follow_the_strid_ranges() {
        assert_eq!(region_ids(0x30001), 0x4e1);
        assert_eq!(region_ids(0x30005), 0x4e1);
        assert_eq!(region_ids(0x30006), 0x4e2);
        assert_eq!(region_ids(0x3000c), 0x4e4);
        assert_eq!(region_ids(0x30012), 0x4e3);
        assert_eq!(region_ids(0x30018), 0x4e3);
        assert_eq!(region_ids(0x30019), 0);
        assert_eq!(region_ids(0x30031), 0);
        assert_eq!(region_ids(0), 0);
    }

    #[test]
    fn destroy_takes_the_window_and_its_caption_and_clears_the_field() {
        let mut ui = Ui::new(neuron_client::ui::widescreen::Surface::new(1024, 768));
        register(&mut ui.factory);
        let mut window = None;
        destroy(&mut ui, &mut window);
        assert!(window.is_none(), "nothing without a window");
        let id = ensure(&mut ui, &mut window).expect("the window");
        let children = ui.children(id);
        assert!(!children.is_empty());
        destroy(&mut ui, &mut window);
        assert!(window.is_none());
        assert!(!ui.contains(id));
        assert!(children.iter().all(|&c| !ui.contains(c)));
    }
}
