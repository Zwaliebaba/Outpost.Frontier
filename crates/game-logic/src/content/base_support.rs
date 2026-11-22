//! The BaseSupport controller (`Content/BaseSupport.cpp`; record 0008
//! point 2).
//!
//! - **Constructor** (`FBaseSupport::FBaseSupport`, `:210-215`): no
//!   heartbeat, so no `rand()`; it reads the GCS generic scripts
//!   (`LoadGenericScripts("scripts\\gcs\\genericScripts.ini")`, P3.6b).
//! - **Destructor** (`:217-220`): `UnloadGenericScripts` (P3.6b).
//! - **Handler**: the bar, the rooms' characters, the news and the mission
//!   offers (P3.6e).

use neuron_server::controller::ControllerId;

use super::Instance;
use crate::server::Cx;

/// `FBaseSupport::FBaseSupport(ControllerId)`: nothing the bus sees.
pub fn construct(_cx: &mut Cx<'_>, _id: ControllerId) -> Instance {
    Instance::BaseSupport
}

/// `FBaseSupport::~FBaseSupport`: the generic scripts are unloaded, which
/// P3.6b ports with their loader.
pub fn destruct(_cx: &mut Cx<'_>, _id: ControllerId) {}
