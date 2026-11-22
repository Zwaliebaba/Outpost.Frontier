//! The GameSupport controller (`Content/GameSupport.cpp`, TU33; record 0008
//! point 2) and Content's game time.
//!
//! - **Constructor** (`FGameSupport::FGameSupport`, `:450-458`): a 1 s
//!   heartbeat (one `rand()` for its phase), the message router (its timer
//!   id from Content's counter), then `LoadContentGameTables`.
//! - **Destructor** (`:461-468`): `UnloadContentGameTables`, then the router
//!   is deleted.
//! - **Heartbeat** (`HandleMessage`'s `Heartbeat` case, `:594-599`): the
//!   elapsed time is added to the game time (`GGameSupportElapsed`, a
//!   `double`), then to every player state's play time
//!   (`UpdateAllContentPlayerTimes`; the player states are P3.4d's).
//! - **Game time** (`GetContentGameTime`, `GetContentTimeDistance`,
//!   `:782-797`): what the router's delivery times and the comm system's
//!   intervals read.
//!
//! The rest of the handler (the player states' ship, system, base and
//! location records, `[mPlayer]`, `LoadGame` and `SaveGame`, the trade
//! lanes of the system records, the best path and the statistics) is
//! P3.4d's.

use std::sync::Arc;

use neuron_server::controller::ControllerId;

use super::{ContentTables, Instance, router::Router};
use crate::events::ControllerMessage;
use crate::server::Cx;

/// GameSupport's heartbeat period in seconds (`:453`).
pub const HEARTBEAT: f32 = 1.0;

/// The game time's "no time" value (`-1.0`, compared as its bits).
pub const NO_TIME: f64 = -1.0;

/// `FGameSupport::FGameSupport(ControllerId)`.
pub fn construct(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    super::set_heartbeat(cx, id, HEARTBEAT);
    let router = Router::new(&mut cx.logic.content.state.ids);
    // A second GameSupport replaces the router; the C++ leaks the first.
    cx.logic.content.state.router = Some(router);
    let tables = ContentTables::load(cx);
    cx.logic.content.tables = Arc::new(tables);
    Instance::GameSupport
}

/// `FGameSupport::~FGameSupport`: the tables unloaded, the router deleted
/// (`GMessageRouter` cleared by the router's base destructor).
pub fn destruct(cx: &mut Cx<'_>, _id: ControllerId) {
    cx.logic.content.tables = Arc::new(ContentTables::default());
    cx.logic.content.state.router = None;
}

/// `FGameSupport::HandleMessage`: the cases P3.4a ports.
pub fn handle_message(cx: &mut Cx<'_>, _id: ControllerId, message: &mut ControllerMessage<'_>) {
    if let ControllerMessage::Heartbeat { elapsed } = *message {
        heartbeat(cx, elapsed);
    }
}

/// The `Heartbeat` case: `GGameSupportElapsed = GGameSupportElapsed +
/// DeltaTime`, a `double` plus a `float`, exact in `f64` (T11); then the
/// player states' play time, which has no player state to update before
/// P3.4d.
pub fn heartbeat(cx: &mut Cx<'_>, elapsed: f32) {
    let state = &mut cx.logic.content.state;
    state.game_time += f64::from(elapsed);
}

/// `GetContentGameTime`: the time GameSupport's heartbeats have added up.
#[must_use]
pub fn game_time(cx: &Cx<'_>) -> f64 {
    cx.logic.content.state.game_time
}

/// `GetContentTimeDistance(A, B)`: [`NO_TIME`] when either time is
/// [`NO_TIME`] (the C++ compares both dwords of each `double`), else
/// `|a - b|`.
#[must_use]
pub fn time_distance(a: f64, b: f64) -> f64 {
    if a.to_bits() == NO_TIME.to_bits() || b.to_bits() == NO_TIME.to_bits() {
        return NO_TIME;
    }
    (a - b).abs()
}
