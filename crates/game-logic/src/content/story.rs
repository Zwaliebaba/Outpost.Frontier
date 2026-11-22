//! StoryLauncher and Story (`Content/StoryLauncher.cpp`, `Content/Story.cpp`;
//! record 0008 point 2). The story shell is P3.5d's.
//!
//! - **StoryLauncher** (`FStoryLauncher::FStoryLauncher`,
//!   `StoryLauncher.cpp:38-52`): a heartbeat of -1 (none; one `rand()`), then
//!   `missions\specific_npc.ini` into the specific NPCs (P3.4c). Its
//!   destructor empties them (`:57-60`). Its handler creates Story on
//!   `PlayerLogin` (P3.5d).
//! - **Story** (`FStory::FStory`, `Story.cpp:88-116`): the rank thresholds,
//!   the player's mission number, `pub::Player::SetMoneyNeededToNextRank`
//!   and a 5 s heartbeat. Its constructor comes with the story shell
//!   (P3.5d); until then a Story instance holds its create parameters only.

use neuron_server::controller::ControllerId;

use super::Instance;
use crate::server::{CreateParams, Cx};

/// StoryLauncher's heartbeat period: none (`StoryLauncher.cpp:41`).
pub const LAUNCHER_HEARTBEAT: f32 = -1.0;

/// `FStory`'s own state (`Story.cpp:73-79`, 0x3c bytes); P3.5d adds the
/// mission number, the worth, the ship flag and the mission name.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Story {
    /// `PlayerId`.
    pub player: u32,
}

/// `FStoryLauncher::FStoryLauncher(&ControllerId, Parms)`; `Parms` is not
/// read.
pub fn construct_launcher(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    super::set_heartbeat(cx, id, LAUNCHER_HEARTBEAT);
    Instance::StoryLauncher
}

/// `FStoryLauncher::~FStoryLauncher`: the specific NPCs are emptied, which
/// P3.4c ports with their loader.
pub fn destruct_launcher(_cx: &mut Cx<'_>, _id: ControllerId) {}

/// `CreateStory`: the instance with the player of its
/// `FStoryCreateParms`; other parameters read as player 0 (D770).
pub fn construct_story(_cx: &mut Cx<'_>, _id: ControllerId, params: &CreateParams) -> Instance {
    let player = match *params {
        CreateParams::Story { player }
        | CreateParams::StoryMission { player, .. }
        | CreateParams::GenericMission { player, .. } => player,
        CreateParams::None | CreateParams::RandomMission(_) => 0,
    };
    Instance::Story(Story { player })
}
