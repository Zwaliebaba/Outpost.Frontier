//! StoryLauncher and Story (`Content/StoryLauncher.cpp`, `Content/Story.cpp`;
//! record 0008 point 2). The story shell is P3.5d's.
//!
//! - **StoryLauncher** (`FStoryLauncher::FStoryLauncher`,
//!   `StoryLauncher.cpp:38-52`): a heartbeat of -1 (none; one `rand()`), then
//!   `missions\specific_npc.ini` into the specific NPCs
//!   ([`npc_desc::load_specific_npcs`], P3.4c). Its destructor empties them
//!   (`:57-60`). Its handler creates Story on `PlayerLogin` (P3.5d).
//! - **Story** (`FStory::FStory`, `Story.cpp:88-116`): the rank thresholds,
//!   the player's mission number, `pub::Player::SetMoneyNeededToNextRank`
//!   and a 5 s heartbeat. Its constructor comes with the story shell
//!   (P3.5d); until then a Story instance holds its create parameters only.

use std::sync::Arc;

use neuron_server::controller::ControllerId;

use super::{Instance, npc_desc};
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
/// read. The heartbeat is switched off, then `missions\specific_npc.ini`
/// of the world's data root (none without one; the open is not tested) is
/// read into the specific NPCs.
pub fn construct_launcher(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    super::set_heartbeat(cx, id, LAUNCHER_HEARTBEAT);
    let bytes = cx
        .world
        .data_source()
        .and_then(|source| source.read(npc_desc::SPECIFIC_NPC_FILE));
    let mut reader = neuron_core::ini::IniReader::default();
    if let Some(bytes) = bytes {
        reader.open(npc_desc::SPECIFIC_NPC_FILE, bytes);
    }
    let npcs = npc_desc::load_specific_npcs(cx, &mut reader);
    // A second StoryLauncher inserts into the same table, keeping what is
    // there.
    let mut table = (*cx.logic.content.state.specific_npcs).clone();
    for (key, desc) in npcs {
        table.entry(key).or_insert(desc);
    }
    cx.logic.content.state.specific_npcs = Arc::new(table);
    Instance::StoryLauncher
}

/// `FStoryLauncher::~FStoryLauncher`: the specific NPCs are emptied.
pub fn destruct_launcher(cx: &mut Cx<'_>, _id: ControllerId) {
    cx.logic.content.state.specific_npcs = Arc::default();
}

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
