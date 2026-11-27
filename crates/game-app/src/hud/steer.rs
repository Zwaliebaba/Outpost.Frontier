//! `HUD_Steer` (`hud_element_list.cpp`, the `[Steer]` section of
//! `hud.ini`): the mouse-flight steering arrow. It is an element without a
//! window: its `Render` draws the arrow's segments towards the cursor.
//!
//! The port so far: the constructor and its section (`radius`, `range`).
//! `Init` is `HUDItem`'s (true). The mesh is kept by its path; `Render`
//! comes with the HUD's draw (divergence D1117).

use neuron_core::ini::IniReader;

use super::HudItem;

/// The factory's section.
pub const SECTION: &[u8] = b"Steer";
/// `HUDItem::Name`.
pub const NAME: &[u8] = b"Steer";
/// The steering arrow's mesh (`ClientMeshLoadFromPath(path, 0, 1)`).
pub const MESH: &[u8] = b"interface\\hud\\HUD_steeringarrow.3db";

/// The element (`HUD_Steer`, 0x74 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct HudSteer {
    /// `+0x38`: the constructor clears it; nothing sets it (slot 1).
    pub byte_38: bool,
    /// `+0x3c` "radius": the dead zone's radius in pixels at 800 wide.
    pub radius: f32,
    /// `+0x40` "range": the full deflection's radius.
    pub range: f32,
    /// `+0x44`: 0.2, read by nothing.
    pub field_44: f32,
    /// `+0x48`: cleared, read by nothing.
    pub byte_48: bool,
    /// The mesh holder's path (`+0x4c`).
    pub mesh: &'static [u8],
}

impl Default for HudSteer {
    fn default() -> Self {
        Self {
            byte_38: false,
            radius: 40.0,
            range: 200.0,
            field_44: 0.2,
            byte_48: false,
            mesh: MESH,
        }
    }
}

impl HudSteer {
    /// The constructor's section loop: `radius` and `range` (a float each,
    /// `get_value_float(0)`); every other key is skipped.
    pub fn read(reader: &mut IniReader) -> Self {
        let mut steer = Self::default();
        while reader.read_value() {
            if reader.is_value(b"radius") {
                steer.radius = reader.value_float(0) as f32;
            } else if reader.is_value(b"range") {
                steer.range = reader.value_float(0) as f32;
            }
        }
        steer
    }
}

impl HudItem for HudSteer {
    fn name(&self) -> &[u8] {
        NAME
    }
    fn type_name(&self) -> &'static str {
        "struct HUD_Steer"
    }
}

/// `HUD_SteerFactory::Create`: the constructor over the section.
pub fn create(
    reader: &mut IniReader,
    _ui: &mut neuron_client::ui::tree::Ui,
) -> Option<Box<dyn HudItem>> {
    Some(Box::new(HudSteer::read(reader)))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn read(text: &str) -> HudSteer {
        let mut reader = IniReader::new();
        assert!(reader.open(b"hud.ini", text.as_bytes().to_vec()));
        assert!(reader.read_header());
        HudSteer::read(&mut reader)
    }

    #[test]
    fn the_section_sets_the_radius_and_the_range() {
        let steer =
            read("[Steer]\nradius = 30\nrange = 250.5\ncolor = 255, 255, 0, 255\nsize = 6\n");
        assert_eq!(steer.radius, 30.0);
        assert_eq!(steer.range, 250.5);
        assert_eq!(steer.field_44, 0.2);
        assert!(!steer.byte_38 && !steer.byte_48);
        assert_eq!(steer.mesh, MESH);
        assert_eq!(steer.name(), b"Steer");
    }

    #[test]
    fn an_empty_section_keeps_the_defaults() {
        assert_eq!(read("[Steer]\n"), HudSteer::default());
    }
}
