//! The populators, SpacePop and BasePop (`Content/Populators.cpp`; record
//! 0008 point 2). Their handlers, the encounters and the spawn placement
//! are P3.6d's.
//!
//! - **SpacePop** (`FSpacePop::FSpacePop`, `:237-241`): the controller's id
//!   into `GSpacePopControllerId`, then a 3 s heartbeat (one `rand()`). No
//!   destructor.
//! - **BasePop** (`FBasePop::FBasePop`, `:970-977`): a heartbeat of -1 (none;
//!   the bus draws its `rand()` all the same), then the instance into the
//!   process-wide `GBasePopInstance`. No destructor: the pointer is never
//!   cleared (D772). A second BasePop executes `int3` in the C++ (D771).

use std::collections::BTreeMap;

use neuron_server::controller::ControllerId;

use super::Instance;
use crate::server::Cx;

/// SpacePop's heartbeat period in seconds (`:240`).
pub const SPACE_POP_HEARTBEAT: f32 = 3.0;

/// BasePop's heartbeat period: none (`:973`).
pub const BASE_POP_HEARTBEAT: f32 = -1.0;

/// `FBasePop`'s own state (`:796-807`, 0x1c bytes): the room characters
/// it placed for each player (`PlayerNpcs`), which its handler fills
/// (P3.6d).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BasePop {
    /// `PlayerNpcs`: by player id, the keys of the bar characters placed
    /// for them (the C++ keeps iterators into the base's bar list).
    pub player_npcs: BTreeMap<u32, Vec<u32>>,
}

/// `FSpacePop::FSpacePop(&ControllerId)`.
pub fn construct_space_pop(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    cx.logic.content.state.space_pop = id;
    super::set_heartbeat(cx, id, SPACE_POP_HEARTBEAT);
    Instance::SpacePop
}

/// `FBasePop::FBasePop(ControllerId)`. A second BasePop becomes the
/// instance, as the C++ continues after its breakpoint (D771).
pub fn construct_base_pop(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    super::set_heartbeat(cx, id, BASE_POP_HEARTBEAT);
    cx.logic.content.state.base_pop = Some(id);
    Instance::BasePop(BasePop::default())
}

/// `GBasePopInstance` as the C++ reads it (`BasePopHasPlayer`,
/// `:1043-1049`): the BasePop created last, when its controller still has
/// an instance; a torn-down BasePop reads as none (D772).
#[must_use]
pub fn base_pop<'a>(cx: &'a Cx<'_>) -> Option<&'a BasePop> {
    let content = &cx.logic.content;
    match content.instance(content.state.base_pop?) {
        Some(Instance::BasePop(pop)) => Some(pop),
        _ => None,
    }
}

/// `BasePopHasPlayer(PlayerId)` (`:1043-1049`): the BasePop instance
/// exists and tracks room characters for the player.
#[must_use]
pub fn base_pop_has_player(cx: &Cx<'_>, player: u32) -> bool {
    base_pop(cx).is_some_and(|pop| pop.player_npcs.contains_key(&player))
}
