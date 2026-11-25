//! The NPC name builder (TU42, `Content/NpcNames.cpp`; work package P3.6c,
//! moved from P3.4c) over the faction-name pool (TU48, `FFactionNames`).
//!
//! P3.6c1 brings the reputation handle's destructor as far as the ships
//! need it; the builder and the pool come with P3.6c4.

use crate::api;
use crate::server::Cx;

/// `~FNpcNameReputation` (`NpcNames.cpp:52-120`): the name's pool entries
/// go back to the faction's tables and the vibe is freed.
pub fn release_name_reputation(cx: &mut Cx<'_>, vibe: i32) {
    let _ = api::reputation::free(cx, vibe);
}
