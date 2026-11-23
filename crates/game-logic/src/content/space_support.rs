//! The SpaceSupport controller (`Content/SpaceSupport.cpp`, TU53; record
//! 0008 point 2).
//!
//! - **Constructor** (`FSpaceSupport::FSpaceSupport`, `:303-308`): the
//!   controller's id into `GSpaceSupportControllerId`, a 1 s heartbeat
//!   (`GSpaceSupportHeartbeat`; one `rand()`), then `InitGcs`, the comm
//!   system's handlers and intervals (P3.6b). It has no destructor.
//! - **Handler** (`FSpaceSupport::HandleMessage`, `:560-984`), four steps
//!   over every message: the comm system's handler set (P3.6b), the NPC
//!   ship events (P3.6c), the main switch, and the tail (the comm system on
//!   load and unload, the dock terminals, the guided ships; P3.6). Of the
//!   main switch the port has:
//!   - the trade-lane cases (`:669-676`), whose handlers are in
//!     [`crate::tradelane`];
//!   - `SystemLoaded` and `SystemUnloaded` (`:914-921`), the system
//!     record's full load and unload ([`super::systems::load_full`],
//!     [`super::systems::unload_full`]; P3.4d). The dock terminals the
//!     unload forgets are P3.6a's;
//!   - the system record's part of `SystemEnter` and `SystemExit`
//!     (`:877-912`): the player joins and leaves the record's players, and
//!     an entering player's ship is no scan target and calm (P3.4d). The
//!     docking list's entries of both cases are P3.6a's.
//!
//!   The rest of SpaceSupport is P3.6a's.

use neuron_server::controller::ControllerId;

use super::{Instance, systems};
use crate::events::ControllerMessage;
use crate::server::Cx;

/// `GSpaceSupportHeartbeat` (`:54-55`): the heartbeat period in seconds.
pub const HEARTBEAT: f32 = 1.0;

/// `FSpaceSupport::FSpaceSupport(ControllerId)`.
pub fn construct(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    cx.logic.content.state.space_support = id;
    super::set_heartbeat(cx, id, HEARTBEAT);
    Instance::SpaceSupport
}

/// `FSpaceSupport::HandleMessage`: the main switch's cases the port has.
pub fn handle_message(cx: &mut Cx<'_>, _id: ControllerId, message: &mut ControllerMessage<'_>) {
    match *message {
        ControllerMessage::TradelaneRequest { .. } | ControllerMessage::TradelaneRelease { .. } => {
            crate::tradelane::space_support(cx, message);
        }
        ControllerMessage::SystemExit { system, player, .. } => {
            // A player without a state is dereferenced by the C++ (D801).
            if cx.logic.content.state.players.contains_key(&player)
                && let Some(record) = cx.logic.content.state.systems.get_mut(&system)
            {
                record.remove_player(player);
            }
        }
        ControllerMessage::SystemEnter { system, player, .. } => {
            let state = &mut cx.logic.content.state;
            let Some(record) = state.systems.get_mut(&system) else {
                return;
            };
            // A player without a state is dereferenced by the C++ (D801).
            let Some(player_state) = state.players.get_mut(&player) else {
                return;
            };
            record.add_player(player);
            if let Some(ship) = player_state.ship.as_mut() {
                ship.scan_target = false;
                // `ResetHostileState`.
                ship.in_combat = false;
                ship.calm_since = f64::from(-1.0_f32);
                ship.hostile_contact_count = 0;
            }
        }
        ControllerMessage::SystemLoaded { system } => systems::load_full(cx, system),
        ControllerMessage::SystemUnloaded { system } => systems::unload_full(cx, system),
        _ => {}
    }
}
