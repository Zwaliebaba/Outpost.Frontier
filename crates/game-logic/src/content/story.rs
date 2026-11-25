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
//!   (P3.5d); until then a Story instance holds its player and the
//!   constructor's values. Its `[StoryInfo]` writer and reader ([`save`],
//!   [`load`]) came with P3.5b; the restore's mission creation and load are
//!   returned as [`StoryLoadStep`]s for the shell to make.

use std::io::{self, Write};
use std::sync::Arc;

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::ini::IniReader;
use neuron_core::msvcrt::format_f;
use neuron_core::num::trunc_to_i32;
use neuron_server::controller::ControllerId;

use super::mission::MissionPath;
use super::{Instance, npc_desc, players};
use crate::api;
use crate::server::{CreateParams, Cx};

/// StoryLauncher's heartbeat period: none (`StoryLauncher.cpp:41`).
pub const LAUNCHER_HEARTBEAT: f32 = -1.0;

/// `FStory`'s own state (`Story.cpp:73-84`, 0x3c bytes). Its save
/// (`[StoryInfo]`) came with P3.5b; its handlers with P3.5d.
#[derive(Clone, Debug, PartialEq)]
pub struct Story {
    /// `PlayerId`.
    pub player: u32,
    /// `MissionNum` (0).
    pub mission_num: i32,
    /// `DeltaWorth` (-1): the worth that starts the next mission.
    pub delta_worth: f32,
    /// `bShipBought` (false).
    pub ship_bought: bool,
    /// `MissionName` (`char[0x20]`, `No_Mission`): at most 31 bytes.
    pub mission_name: Vec<u8>,
}

/// The mission name of a Story without a mission.
pub const NO_MISSION: &[u8] = b"No_Mission";

/// The size of `MissionName`, terminator included.
pub const MISSION_NAME_BUFFER: usize = 0x20;

impl Story {
    /// The constructor's values (`Story.cpp:88-116`) for `player`.
    #[must_use]
    pub fn new(player: u32) -> Self {
        Self {
            player,
            mission_num: 0,
            delta_worth: -1.0,
            ship_bought: false,
            mission_name: NO_MISSION.to_vec(),
        }
    }
}

impl Default for Story {
    fn default() -> Self {
        Self::new(0)
    }
}

/// A call out of Story's restore that the story shell makes (P3.5d), in
/// the order the C++ makes it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum StoryLoadStep {
    /// `Mission` names a mission: `pub::Controller::Create("Content.dll",
    /// name, {player, this story, path}, NORMAL)` into
    /// `GStoryMissionController`; the path only for `GenericMission`.
    CreateMission {
        /// The mission's name.
        name: Vec<u8>,
        /// `GenericMission`'s path (value 1, cut as D773 says).
        path: Option<MissionPath>,
    },
    /// `MissionNum`: when `GStoryMissionController` is a controller at
    /// that point, `PlayerLoad {player, reader}` then `MissionStart` to it.
    LoadMission,
}

/// `FStory::OnSave(File)` (`Story.cpp:333-344`): `[StoryInfo]` with the
/// ship flag, the mission, its number and the worth when there is a
/// mission or a number, and the process-wide debug flag.
///
/// # Errors
///
/// The writer's.
pub fn save(cx: &Cx<'_>, story: &Story, out: &mut dyn Write) -> io::Result<()> {
    out.write_all(b"[StoryInfo]\n")?;
    writeln!(
        out,
        "ship_bought = {}",
        if story.ship_bought { "true" } else { "false" }
    )?;
    if story.mission_num != 0 || !eq_ignore_case(&story.mission_name, NO_MISSION) {
        out.write_all(b"Mission = ")?;
        out.write_all(&story.mission_name)?;
        out.write_all(b"\n")?;
        writeln!(out, "MissionNum = {}", story.mission_num)?;
        writeln!(
            out,
            "delta_worth = {}",
            format_f(f64::from(story.delta_worth), 6)
        )?;
    }
    writeln!(
        out,
        "debug = {}",
        i32::from(cx.logic.content.state.story_debug != 0)
    )
}

/// `FStory::OnLoad(Reader)` (`Story.cpp:347-412`): the reader's state kept
/// around a read from its start. The mission number goes to 0 (and the
/// player's mission number with it), then every `[StoryInfo]` value:
///
/// - `Mission` takes value 0 as the mission's name (at most 31 bytes: the
///   C++ `strcpy` into its 0x20 bytes overruns for a longer one, D834) and,
///   unless it is `No_Mission`, makes a [`StoryLoadStep::CreateMission`];
/// - `MissionNum` sets the number, the player's mission number (no rank
///   change) and makes a [`StoryLoadStep::LoadMission`];
/// - `delta_worth` the worth and the player's money to the next rank (its
///   truncation);
/// - `ship_bought` the flag; `debug` the process-wide flag.
pub fn load(cx: &mut Cx<'_>, story: &mut Story, reader: &mut IniReader) -> Vec<StoryLoadStep> {
    let saved = reader.state();
    reader.reset();
    let mut steps = Vec::new();
    story.mission_num = 0;
    players::set_mission_number(cx, story.player, 0, false);
    while reader.read_header() {
        if !reader.is_header(b"StoryInfo") {
            continue;
        }
        while reader.read_value() {
            if reader.is_value(b"Mission") {
                let mut name = c_str(&reader.value_string_at(0)).to_vec();
                name.truncate(MISSION_NAME_BUFFER - 1);
                story.mission_name.clone_from(&name);
                if !eq_ignore_case(&name, NO_MISSION) {
                    let path = eq_ignore_case(&name, b"GenericMission")
                        .then(|| MissionPath::new(&reader.value_string_at(1)));
                    steps.push(StoryLoadStep::CreateMission { name, path });
                }
            } else if reader.is_value(b"MissionNum") {
                story.mission_num = reader.value_int(0);
                players::set_mission_number(cx, story.player, story.mission_num as u32, false);
                steps.push(StoryLoadStep::LoadMission);
            } else if reader.is_value(b"delta_worth") {
                story.delta_worth = reader.value_float(0) as f32;
                let money = trunc_to_i32(f64::from(story.delta_worth));
                let _ = api::player::set_money_needed_to_next_rank(cx, story.player, money);
            } else if reader.is_value(b"ship_bought") {
                story.ship_bought = reader.value_bool(0);
            } else if reader.is_value(b"debug") {
                cx.logic.content.state.story_debug = u8::from(reader.value_bool(0));
            }
        }
    }
    reader.set_state(&saved);
    steps
}

/// `PlayerSave {player, file}` to Story: [`save`] for its own player.
///
/// # Errors
///
/// The writer's.
pub fn player_save(cx: &Cx<'_>, story: &Story, player: u32, out: &mut dyn Write) -> io::Result<()> {
    if player != story.player {
        return Ok(());
    }
    save(cx, story, out)
}

/// `PlayerLoad {player, reader}` to Story: [`load`] for its own player.
pub fn player_load(
    cx: &mut Cx<'_>,
    story: &mut Story,
    player: u32,
    reader: &mut IniReader,
) -> Vec<StoryLoadStep> {
    if player != story.player {
        return Vec::new();
    }
    load(cx, story, reader)
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
    Instance::Story(Story::new(player))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_new_story_saves_no_mission() {
        assert_eq!(Story::new(3).mission_name, NO_MISSION);
        assert_eq!(Story::new(3).delta_worth, -1.0);
    }
}
