//! The HUD's element list (`hud_element_list.cpp`, the reference's
//! `Client\space\HUD.cpp`): the `HUDItem` base every HUD panel derives
//! from, the factory chain the panels register with (`HUDFACTORY<T>`), and
//! `LoadHudElementsFromIni`, which the cockpit's load runs on entering
//! space (`cockpit_load.cpp`).
//!
//! `hud.ini` (the `HUD` key of `[Data]`, `SetHudIniPath`) has a `[HUD]`
//! section (the shape database, the static texture and its blend) and one
//! section per element. Each section is offered to the factories in chain
//! order and the first one whose name matches and that makes an element
//! wins; the element is kept when its `Init` answers true. Most elements
//! are interface windows too: their constructor makes the window and its
//! children (`Create(0, 0)`) and names it, so the windows register in the
//! file's order.
//!
//! The port so far: the chain, the loader, the oracle's `hud` and
//! `huditem` records, and the elements in [`FACTORIES`]. The `[HUD]`
//! section's shape database and static texture are kept by name: the
//! HUD's draw, which reads them, comes with the elements' draws.

pub mod core;

use neuron_client::ui::tree::{NodeId, Ui};
use neuron_core::ini::IniReader;

use crate::oracle::json::OracleJson;

/// `HUDItem`: what the list keeps of an element.
pub trait HudItem {
    /// `HUDItem::Name` (+0x04), the name the element's constructor sets.
    fn name(&self) -> &[u8];
    /// `typeid(*Item).name()`, for the oracle's `huditem` record.
    fn type_name(&self) -> &'static str;
    /// `ToolTipIds` (+0x30).
    fn tooltip_ids(&self) -> i32 {
        0
    }
    /// Slot 3 `Init`: the element is kept when it answers true.
    fn init(&mut self, _ui: &mut Ui) -> bool {
        true
    }
    /// The interface window the element is, when it is one.
    fn window(&self) -> Option<NodeId> {
        None
    }
}

/// `HUDFACTORY<T>`: a section name and the element's constructor over the
/// section (`HUDFACTORY<T>::Create`, `operator new` and the constructor).
#[derive(Clone, Copy)]
pub struct HudFactory {
    /// `SectionName`.
    pub section: &'static [u8],
    /// `Create(Reader)`: `None` when the factory makes nothing.
    pub create: fn(&mut IniReader, &mut Ui) -> Option<Box<dyn HudItem>>,
}

impl std::fmt::Debug for HudFactory {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("HudFactory")
            .field("section", &String::from_utf8_lossy(self.section))
            .finish_non_exhaustive()
    }
}

/// The factories in chain order. The reference's chain is the order of
/// the static constructors that register them; the section names are all
/// different, so only the elements a section names are made, whatever the
/// order. The elements not ported yet are missing: their sections answer
/// "unknown INI entry".
pub const FACTORIES: &[HudFactory] = &[HudFactory {
    section: core::SECTION,
    create: core::create,
}];

/// `[HUD]`: what the section gave.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HudConfig {
    /// `shapes`: the shape database's file (`G_HudShapeDB.Load`).
    pub shapes: Option<Vec<u8>>,
    /// `static_txm`: the static texture (`TextureRefLoad`).
    pub static_txm: Option<Vec<u8>>,
    /// `static_blend` (`G_HudStaticBlend`, 2 until a file sets it).
    pub static_blend: i32,
}

impl Default for HudConfig {
    fn default() -> Self {
        Self {
            shapes: None,
            static_txm: None,
            static_blend: 2,
        }
    }
}

/// The element list (`G_HudItemHead` .. `G_HudItemTail`) and the `[HUD]`
/// section's values.
#[derive(Default)]
pub struct HudElements {
    /// The kept elements, in the file's order.
    pub items: Vec<Box<dyn HudItem>>,
    pub config: HudConfig,
}

impl std::fmt::Debug for HudElements {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let names: Vec<String> = self
            .items
            .iter()
            .map(|i| String::from_utf8_lossy(i.name()).into_owned())
            .collect();
        f.debug_struct("HudElements")
            .field("items", &names)
            .field("config", &self.config)
            .finish()
    }
}

/// `kHudCpp`, the warnings' file name.
const HUD_CPP: &str = r"Client\space\HUD.cpp";

/// `LoadHudElementsFromIni`: the file at `path` (`contents`, `None` when
/// it does not open) through `factories`; the warnings go to `warn` in the
/// reference's `FDUMP` text. The caller then resets the mouse flight
/// (`ResetMouseFlightState`, [`crate::player_behavior`]).
pub fn load_hud_elements(
    ui: &mut Ui,
    path: &[u8],
    contents: Option<Vec<u8>>,
    factories: &[HudFactory],
    warn: &mut dyn FnMut(String),
) -> HudElements {
    let mut out = HudElements::default();
    let mut reader = IniReader::new();
    if !contents.is_some_and(|c| reader.open(path, c)) {
        warn(format!(
            "{HUD_CPP}(317) : *** WARNING: hud file not found '{}'\n",
            String::from_utf8_lossy(path)
        ));
        return out;
    }
    if !reader.read_header() {
        return out;
    }
    loop {
        if reader.is_header(b"HUD") {
            while reader.read_value() {
                if reader.is_value(b"shapes") {
                    out.config.shapes = Some(reader.value_string());
                } else if reader.is_value(b"static_txm") {
                    out.config.static_txm = Some(reader.value_string());
                } else if reader.is_value(b"static_blend") {
                    // `get_value_float(0)` (a float), `ftol`.
                    let value = reader.value_float(0) as f32;
                    out.config.static_blend = neuron_core::num::trunc_to_i32(f64::from(value));
                }
            }
        } else {
            let mut item = None;
            for factory in factories {
                if !reader.is_header(factory.section) {
                    continue;
                }
                item = (factory.create)(&mut reader, ui);
                if item.is_some() {
                    break;
                }
            }
            match item {
                None => warn(format!(
                    "{HUD_CPP}(310) : *** WARNING: HUD - unknown INI entry '{}'\n",
                    String::from_utf8_lossy(&reader.header())
                )),
                Some(mut item) => {
                    if item.init(ui) {
                        out.items.push(item);
                    }
                }
            }
        }
        if !reader.read_header() {
            break;
        }
    }
    out
}

/// `OracleOnHudElementsLoaded`: `layout.jsonl`'s `hud` head and a
/// `huditem` per kept element (`rtti` null, decision O53 (4)).
#[must_use]
pub fn oracle_lines(frame: u32, path: &[u8], hud: &HudElements) -> Vec<String> {
    let mut lines = Vec::new();
    let mut j = OracleJson::new();
    j.uint("frame", frame)
        .str("rec", "hud")
        .str_bytes("path", path)
        .int("static_blend", hud.config.static_blend)
        .uint("items", u32::try_from(hud.items.len()).unwrap_or(u32::MAX));
    lines.push(j.finish());
    for (i, item) in (0u32..).zip(&hud.items) {
        let mut j = OracleJson::new();
        j.uint("frame", frame)
            .str("rec", "huditem")
            .uint("i", i)
            .str_bytes("name", item.name())
            .null("rtti")
            .int("tooltip", item.tooltip_ids());
        lines.push(j.finish());
    }
    lines
}

#[cfg(test)]
mod tests;
