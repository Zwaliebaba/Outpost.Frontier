//! The random missions: the `DestroyMission` controller
//! (`Content/DestroyMission.cpp`, `Content/RandomMissionDB.cpp` and the
//! `RandomMission*.cpp` files; record 0008 points 2 and 8).
//!
//! Its create parameter is the reference-counted `FRandomMissionData`
//! (`RandomMission.h:21-90`), which the port keeps in a table of Content's
//! state and names by [`RmDataKey`]. The mission data, the constructor
//! (its 1 s heartbeat, `RandomMissionDB.cpp:1041`, and the router
//! handlers) and the run time are P3.7's; until then a random mission
//! instance holds its key only.

use neuron_server::controller::ControllerId;

use super::Instance;
use crate::server::{CreateParams, Cx};

/// A key into Content's table of random-mission data (the C++'s
/// `FRandomMissionData*`, counted as `FContentRefObject` counts it; P3.7).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RmDataKey(pub u32);

/// A `DestroyMission` controller's own state (P3.7 adds the mission
/// object and its machines).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RandomMission {
    /// Its mission data; none for parameters of another form (D770).
    pub data: Option<RmDataKey>,
}

/// `CreateDestroyMission`: the instance with its mission data.
pub fn construct(_cx: &mut Cx<'_>, _id: ControllerId, params: &CreateParams) -> Instance {
    let data = match *params {
        CreateParams::RandomMission(key) => Some(key),
        _ => None,
    };
    Instance::RandomMission(RandomMission { data })
}
