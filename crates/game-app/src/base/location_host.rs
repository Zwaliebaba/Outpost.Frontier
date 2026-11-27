//! The client's side of the location controller (`menu_universe_base.cpp`,
//! work package P5.9): what a controller step does to the interface, the
//! sound and the player, over the client's parts.
//!
//! [`ClientLocationHost`] borrows the parts a step touches; the gameflow
//! state makes one for each step. The player's base change and a failed
//! synchronisation are kept on the host for the state to apply, since the
//! link they change is the controller's server during the step.
//!
//! Not yet the host's: the base's solar and visit flags after its load
//! (`StoryStarForgetLastObjective` and the visit tail of
//! `SetPlayerBaseId`), the room's runtime (`CreateRoomRuntime`,
//! `DestroyRoomRuntime`) and its scenes (`RoomControlPlayScenes`), and the
//! navigation bar's rooms (`OnLocationEntered`). They are P5.9's next
//! increments; until then those calls do nothing.

use game_sim::data::{DataSource, Reports};
use neuron_client::scene_script::ScriptModule;
use neuron_client::ui::tree::{NodeId, Property, Ui};
use neuron_core::rng::CrtRand;

use super::location_controller::LocationHost;
use super::nav_bar;
use super::room_data::RoomData;
use crate::audio::SoundCalls;
use crate::oracle::script::OracleScript;

/// The inventory window `NotifyInventoryLocationLeft` tells.
const INVENTORY: &[u8] = b"NN_Inventory";

/// The property `NotifyInventoryLocationLeft` sets on it.
const INVENTORY_LOCATION_LEFT: i32 = 0x4b;

/// A base change's player state (`SetPlayerBaseId`,
/// `SetPlayerFactionScratch`, `SetGameflowLocationBase`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BaseChange {
    /// The new base (`G_PlayerBaseId`, `G_GameflowLocationBase`).
    pub base: u32,
    /// The location entered with it (`G_PlayerFactionScratch`).
    pub location: u32,
}

/// The controller's host over the client's parts.
pub struct ClientLocationHost<'a> {
    /// The data the rooms' files are read from.
    pub source: &'a dyn DataSource,
    /// The client's scene-script module, which the rooms' set scripts run in.
    pub scripts: &'a mut ScriptModule,
    /// The reports of the rooms' reads.
    pub reports: &'a mut Reports,
    /// The client's sound calls.
    pub sound: &'a mut dyn SoundCalls,
    /// The C runtime's `rand`.
    pub rand: &'a mut CrtRand,
    /// The interface, when the client has one.
    pub ui: Option<&'a mut Ui>,
    /// The oracle mode's script, which notes the room screens.
    pub oracle: Option<&'a mut OracleScript>,
    /// Single player (`SinglePlayer()`).
    pub single_player: bool,
    /// A synchronisation failed: single player asks to quit
    /// (`SetClientQuitRequested(true)`), multiplayer is disconnected
    /// (`G_bServerDisconnected`).
    pub sync_failed: bool,
    /// The last base change of the step.
    pub base_change: Option<BaseChange>,
}

/// The parts a host borrows that are the location's own until the client
/// keeps them: the scene-script module, the reads' reports and the `rand`
/// state.
#[derive(Default)]
pub struct LocationParts {
    /// The scene-script module the rooms' set scripts run in.
    pub scripts: ScriptModule,
    /// The reports of the rooms' reads.
    pub reports: Reports,
    /// The C runtime's `rand`.
    pub rand: CrtRand,
}

impl LocationParts {
    /// A host over these parts and the given ones, with no change kept yet.
    pub fn host<'a>(
        &'a mut self,
        source: &'a dyn DataSource,
        sound: &'a mut dyn SoundCalls,
        ui: Option<&'a mut Ui>,
        oracle: Option<&'a mut OracleScript>,
        single_player: bool,
    ) -> ClientLocationHost<'a> {
        ClientLocationHost {
            source,
            scripts: &mut self.scripts,
            reports: &mut self.reports,
            sound,
            rand: &mut self.rand,
            ui,
            oracle,
            single_player,
            sync_failed: false,
            base_change: None,
        }
    }
}

impl ClientLocationHost<'_> {
    /// The interface's window `name`, when there is one.
    fn find(&mut self, name: &[u8]) -> Option<(&mut Ui, NodeId)> {
        let ui = self.ui.as_deref_mut()?;
        let id = ui.find_by_name(name)?;
        Some((ui, id))
    }
}

impl LocationHost for ClientLocationHost<'_> {
    fn read_room(&mut self, file: &[u8]) -> RoomData {
        RoomData::read(self.source, file, self.scripts, self.reports)
    }
    fn sound(&mut self) -> &mut dyn SoundCalls {
        &mut *self.sound
    }
    fn rand(&mut self) -> i32 {
        self.rand.rand()
    }
    /// The base's solar and visit flags are not the host's yet (module
    /// documentation).
    fn base_loaded(&mut self, _base: u32) {}
    fn sync_failed(&mut self) {
        self.sync_failed = true;
    }
    /// The room's runtime is not the host's yet (module documentation).
    fn room_runtime(&mut self, _room: u32, _create: bool) {}
    /// `NavBarEnsure`'s window; the room's scenes and the bar's rooms are
    /// not the host's yet (module documentation).
    fn room_entered(&mut self, _room: u32) {
        if let Some(ui) = self.ui.as_deref_mut() {
            let _ = nav_bar::ensure(ui);
        }
    }
    /// `NavBarDestroy`: the "NavBar" window, if there is one, destroyed.
    fn nav_bar_destroy(&mut self) {
        if let Some((ui, id)) = self.find(nav_bar::NAME) {
            ui.destroy(id);
        }
    }
    /// `NnArrivalTextDestroy`: the arrival text comes with the space
    /// entry's NeuroNet windows (P5.7).
    fn arrival_text_destroy(&mut self) {}
    /// `NotifyInventoryLocationLeft`: property 0x4b on "NN_Inventory", when
    /// it is a control.
    fn inventory_location_left(&mut self) {
        if let Some((ui, id)) = self.find(INVENTORY)
            && ui.win_control(id).is_some()
        {
            let _ = ui.property(id, Property::Other(INVENTORY_LOCATION_LEFT, 0, 0));
        }
    }
    fn base_changed(&mut self, base: u32, location: u32) {
        self.base_change = Some(BaseChange { base, location });
    }
    fn screen_room(&mut self, location: u32) {
        if let Some(script) = self.oracle.as_deref_mut() {
            script.note_screen("room", location);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::Silent;

    /// No files.
    struct Nothing;

    impl DataSource for Nothing {
        fn read(&self, _: &[u8]) -> Option<Vec<u8>> {
            None
        }
    }

    #[test]
    fn the_host_keeps_the_player_state_for_the_gameflow_state() {
        let source = Nothing;
        let mut parts = LocationParts::default();
        let mut sound = Silent::default();
        let mut host = parts.host(&source, &mut sound, None, None, true);
        assert_eq!(host.rand(), 41, "the CRT's first rand");
        host.base_changed(7, 70);
        host.sync_failed();
        // Without an interface the window calls do nothing.
        host.nav_bar_destroy();
        host.inventory_location_left();
        host.room_entered(70);
        assert_eq!(
            host.base_change,
            Some(BaseChange {
                base: 7,
                location: 70
            })
        );
        assert!(host.sync_failed);
        assert_eq!(host.read_room(b"missing.ini"), RoomData::default());
    }
}
