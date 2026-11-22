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
//!   main switch the port has the trade-lane cases (`:669-676`), whose
//!   handlers are in [`crate::tradelane`]; the rest of SpaceSupport is
//!   P3.6a's.

use neuron_server::controller::ControllerId;

use super::Instance;
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

/// `FSpaceSupport::HandleMessage`: the main switch's trade-lane cases,
/// `TradelaneRequest` (`HandleTradelaneRequest`) and `TradelaneRelease`
/// (`HandleTradelaneRelease`).
pub fn handle_message(cx: &mut Cx<'_>, _id: ControllerId, message: &mut ControllerMessage<'_>) {
    match *message {
        ControllerMessage::TradelaneRequest { .. } | ControllerMessage::TradelaneRelease { .. } => {
            crate::tradelane::space_support(cx, message);
        }
        _ => {}
    }
}
