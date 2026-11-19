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
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct MontageTile {
    /// The tile's index in the montage.
    pub index: usize,
    /// `Scale`: the icon's size; 0 sizes it by the mesh.
    pub scale: f32,
    /// `U`, `V`: the tile's upper left corner on its shape.
    pub u: f32,
    pub v: f32,
    /// `Step`: the tile's size in UV.
    pub step: f32,
    /// The `ButtonTextures.ini` shape the tile sits on, if any.
    pub shape: Option<super::shape::TexShape>,
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
/// reference stores the object's address; divergence D436), or one of a
/// control's own records (`UIFastTable`'s per column, row or cell records,
/// keyed by the table's address plus one plus their index; divergence
/// D476).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HoverKey {
    Ids(i32),
    Object(NodeId),
    Part(NodeId, u32),
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

/// `FHudCmdRecord`: the record the HUD command dispatcher hands the
/// handler of an event (`HudCommandRecord`): event 1 is a key, event 2 a
/// character.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HudRecord {
    /// `+0x04`: the key code of event 1.
    pub key: u32,
    /// `+0x08`: Shift 1, Ctrl 4, Alt 0x10.
    pub modifiers: u32,
    /// `+0x0C`: the raw window message, 0 for a posted command.
    pub message: u32,
    /// `+0x10`: the window message's `wParam` (event 2's character).
    pub wparam: u32,
    /// `+0x14`: its `lParam`.
    pub lparam: u32,
    /// `+0x18`: the key went down (event 1) or the command was posted.
    pub down: bool,
    /// `+0x19`: an auto-repeated key.
    pub repeat: bool,
}

/// What a rich-text edit box asks of its IME box (`FLImeInputBox`,
/// [`crate::ui::ime::ClientImeBox`]): the client makes one while it has an
/// IME ([`UiServices::create_ime_box`], [`crate::ui::ime::ImeBoxes`]). A key or character the box takes may
/// change its composed text, which it hands back for the owner's
/// `SetText` (the box's `UpdatedText`).
pub trait ImeBox {
    /// `HandleKey` (slot 4).
    fn handle_key(&mut self, key: u32) -> Option<Vec<u16>>;
    /// `HandleMultiByteChar` of the character's low byte in the C++; the
    /// UTF-16 unit here, which the box takes whole (`HandleWideChar`,
    /// divergence D286).
    fn handle_char(&mut self, unit: u16) -> Option<Vec<u16>>;
    /// `SetInputBufferSize` and `Resize` (slot 3) with the window's screen
    /// rectangle (left, top, right, bottom).
    fn resize(&mut self, buffer_size: u32, rect: [i32; 4]);
    /// `IMEFocus::SetFocus(box, false)`.
    fn take_focus(&mut self);
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
    /// `MouseSetCursor(name, false)`: one of `mouse.ini`'s cursors.
    fn mouse_set_cursor(&mut self, name: &[u8]) {}
    /// `HudGetInputFocus`: the handler that alone receives the HUD's
    /// events, `None` when every registered handler does.
    fn input_focus(&mut self) -> Option<NodeId> {
        None
    }
    /// `HudPushInputFocus`.
    fn push_input_focus(&mut self, node: NodeId) {}
    /// `HudPopInputFocus`.
    fn pop_input_focus(&mut self, node: NodeId) {}
    /// `Event::Handler::RegisterEvent`: the node receives HUD event `msg`.
    fn register_event(&mut self, node: NodeId, msg: u32) {}
    /// `Event::Handler::UnregisterEvent`.
    fn unregister_event(&mut self, node: NodeId, msg: u32) {}
    /// `new FLImeInputBox(owner)` while `IMEFocus::GetSingleton()` is set:
    /// `None` without an IME focus.
    fn create_ime_box(&mut self, owner: NodeId) -> Option<Box<dyn ImeBox>> {
        None
    }
    /// `IMEFocus::SetFocus(nullptr, false)`, when there is an IME focus.
    fn clear_ime_focus(&mut self) {}
}

/// [`UiServices`] with every default.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoServices;

impl UiServices for NoServices {}
