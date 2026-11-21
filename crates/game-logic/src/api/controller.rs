//! `pub::Controller` (`Server/server_pub.cpp:6324-6355`), the bus of
//! decision record 0006, and `pub::SetTimer` (`:6364`), which the C++
//! defines beside it: thin wrappers over [`neuron_server::controller`]
//! through the context's [`BusHost`](neuron_server::controller::BusHost).
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `Create` | 6326 | [`create`] |
//! | `Destroy` | 6331 | [`destroy`] |
//! | `SetHeartbeatInterval` | 6343 | [`set_heartbeat_interval`] |
//! | `_SendMessage` | 6351 | [`send_message`] |
//! | `pub::SetTimer` | 6364 | [`set_timer`] |
//!
//! The message is the typed [`ControllerMessage`] (record 0006): its id and
//! data travel together, so `_SendMessage` takes one argument for both.

use neuron_server::controller::{self, BusError, ControllerId, Priority};

use super::PubError;
use crate::events::ControllerMessage;
use crate::server::{CreateParams, Cx};

fn code(error: BusError) -> PubError {
    PubError(error.code())
}

/// `Create` (`Server_ControllerCreate`): a controller of the factory's
/// kind `name` at `priority`; 0 when the id allocator gave 0 or the name
/// is not the factory's (its id is used up then).
pub fn create(
    cx: &mut Cx<'_>,
    dll: &[u8],
    name: &[u8],
    params: &CreateParams,
    priority: Priority,
) -> ControllerId {
    controller::create(cx, dll, name, params, priority)
}

/// `Destroy` (`Server_ControllerDestroy`): an active controller is queued
/// for teardown and `ControllerDestroyed` is broadcast.
pub fn destroy(cx: &mut Cx<'_>, id: ControllerId) {
    controller::destroy(cx, id);
}

/// `SetHeartbeatInterval` (`Server_ControllerSetHeartbeat`): the period and
/// an accumulator of `rand()/32768` of it.
///
/// # Errors
///
/// -2 when no controller has the id.
pub fn set_heartbeat_interval(
    cx: &mut Cx<'_>,
    id: ControllerId,
    interval: f32,
) -> Result<(), PubError> {
    controller::set_heartbeat(cx, id, interval).map_err(code)
}

/// `_SendMessage` (`Server_ControllerDispatch`): a broadcast for an id
/// whose low 30 bits are 0, else to that controller.
///
/// # Errors
///
/// -2 for a targeted send to an unknown id.
pub fn send_message(
    cx: &mut Cx<'_>,
    id: ControllerId,
    message: &mut ControllerMessage<'_>,
) -> Result<(), PubError> {
    cx.dispatch(id, message).map_err(code)
}

/// `pub::SetTimer` (`Server_ControllerSetTimer`): `TimerExpired` with the
/// two parameters goes to the controller once `timeout` seconds have
/// passed. The id is not checked.
///
/// # Errors
///
/// -4 for a negative timeout.
pub fn set_timer(
    cx: &mut Cx<'_>,
    id: ControllerId,
    param1: u32,
    param2: u32,
    timeout: f32,
) -> Result<(), PubError> {
    cx.logic
        .bus
        .set_timer(id, param1, param2, timeout)
        .map_err(code)
}
