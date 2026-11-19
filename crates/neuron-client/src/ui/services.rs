//! What the generic controls ask of the rest of the client: the UI sounds,
//! the key-map commands, the button montage, the performance options, the
//! system cursor, the key bindings and a few game states.
//!
//! The reference's controls call these as free functions of the client
//! (`menu_ui_init.cpp`, `menu_sound_arch.cpp`, `hud_command_dispatch.cpp`,
//! `nn_montage_draw.cpp`, `perf_options.cpp`, `key_map.cpp`, the Win32
//! `SetCursor`). Here they are the methods of one [`UiServices`] value the
//! game installs on the [`super::tree::Ui`] (decision T10: no process
//! globals); every method has the answer the client gives before the game
//! has loaded anything.

use neuron_core::math::Vector;

use super::tree::NodeId;

/// A button montage tile (`FButtonMontageTile`): an icon of
/// `ButtonMontage.ini` drawn in place of a button's mesh.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MontageTile {
    /// The tile's index in the montage.
    pub index: usize,
    /// `Scale`: the icon's size; 0 sizes it by the mesh.
    pub scale: f32,
}

/// The three cursors `UITable` sets (`LoadImageA` of `IDC_SIZENS` and
/// `IDC_SIZEWE`, and the cursor current when the table was made).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SystemCursor {
    RowResize,
    ColumnResize,
    Default,
}

/// The key of a tooltip hover record (`+0x00`): a string id, or the
/// control itself when `WinControl::ShowToolTip` builds the record (the
/// reference stores the object's address; divergence D436).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HoverKey {
    Ids(i32),
    Object(NodeId),
}

impl Default for HoverKey {
    fn default() -> Self {
        HoverKey::Ids(0)
    }
}

/// `FToolTipHoverInfo`: what a control hands the tooltip and the rollover.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ToolTipHover {
    /// `+0x00`.
    pub key: HoverKey,
    /// `+0x04`: the string id shown.
    pub ids_alt: i32,
    /// `+0x08`: a caption in place of the id (`None` resolves the id).
    pub caption: Option<Vec<u16>>,
    /// `+0x44`: the key-map command whose binding is appended.
    pub aux: i32,
    /// `+0x48`.
    pub world_pos: Vector,
    /// `+0x54`.
    pub half_extents: Vector,
    /// `+0x60`: the record anchors the tip at its world box.
    pub live: bool,
}

impl ToolTipHover {
    /// `ToolTipHoverReset`: key -1, no id, no command, no caption, not
    /// live; the position and extents stay.
    pub fn reset(&mut self) {
        self.ids_alt = 0;
        self.key = HoverKey::Ids(-1);
        self.aux = 0;
        if let Some(c) = self.caption.as_mut() {
            c.clear();
        }
        self.live = false;
    }

    /// A reset record keyed and shown by `ids` (`BuildIdsRecord`).
    #[must_use]
    pub fn for_ids(ids: i32) -> ToolTipHover {
        let mut r = ToolTipHover::default();
        r.reset();
        r.key = HoverKey::Ids(ids);
        r.ids_alt = ids;
        r
    }
}

/// The client's services the controls call. Every default is the answer
/// of a client with nothing loaded.
#[allow(unused_variables)]
pub trait UiServices {
    /// `FindUiSoundSlot`: the slot of a UI sound by name, -1 when the name
    /// is not one.
    fn find_ui_sound_slot(&mut self, name: &[u8]) -> i32 {
        -1
    }
    /// `PlayOrRewindUiSoundSlot`.
    fn play_or_rewind_ui_sound_slot(&mut self, slot: u32) {}
    /// `PlayUiSoundSlotFlag1`.
    fn play_ui_sound_slot_flag1(&mut self, slot: u32) {}
    /// `StopUiSoundSlot`.
    fn stop_ui_sound_slot(&mut self, slot: u32) {}
    /// `PlayUiRolloverSound`.
    fn play_ui_rollover_sound(&mut self) {}
    /// `LookupClientSoundArch` of `CreateID(name)` and
    /// `PlayClientSoundArchIfIdle`.
    fn play_sound_arch(&mut self, name: &[u8]) {}
    /// `HudPostCommand(command, 0)`.
    fn post_command(&mut self, command: u32) {}
    /// The button montage tile of a mesh path (`stricmp`), the montage
    /// loaded on first use.
    fn montage_tile(&mut self, path: &[u8]) -> Option<MontageTile> {
        None
    }
    /// `FindPerfOption(name)->Applied`: 1 for an option not loaded.
    fn perf_option(&mut self, name: &[u8]) -> f32 {
        1.0
    }
    /// `KeyMapFindActiveBinding` and `KeyMapFormatBindingName`: the name of
    /// the key bound to a command, `None` without a binding.
    fn key_binding_name(&mut self, command: i32) -> Option<Vec<u16>> {
        None
    }
    /// `SetCursor` with one of `UITable`'s cursors.
    fn set_system_cursor(&mut self, cursor: SystemCursor) {}
    /// `ShouldSkipLocalServerTick`.
    fn skip_local_server_tick(&mut self) -> bool {
        false
    }
    /// `GetClientMouseButtons`: bit 0 left, bit 1 right.
    fn mouse_buttons(&mut self) -> u32 {
        0
    }
    /// `IsLetterboxVisible`.
    fn letterbox_visible(&mut self) -> bool {
        false
    }
    /// `IsClientIntroEnvironment`: the intro menu is the current state.
    fn intro_environment(&mut self) -> bool {
        false
    }
    /// `KnowledgeMapReveal`.
    fn knowledge_map_reveal(&mut self, ids: u32) {}
    /// A file of the data tree (`GetFileSysPath` and `INI_Reader::open`).
    fn data_file(&mut self, path: &[u8]) -> Option<Vec<u8>> {
        None
    }
    /// `GetClientElapsedTime`: the seconds since start.
    fn elapsed_time(&mut self) -> f64 {
        0.0
    }
}

/// [`UiServices`] with every default.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoServices;

impl UiServices for NoServices {}
