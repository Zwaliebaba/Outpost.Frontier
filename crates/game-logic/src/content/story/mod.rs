//! StoryLauncher and Story (`Content/StoryLauncher.cpp`, `Content/Story.cpp`;
//! record 0008 point 2; work packages P3.5b and P3.5d).
//!
//! - **StoryLauncher** (`FStoryLauncher::FStoryLauncher`,
//!   `StoryLauncher.cpp:39-53`): a heartbeat of -1 (none; one `rand()`), then
//!   `missions\specific_npc.ini` into the specific NPCs
//!   ([`npc_desc::load_specific_npcs`], P3.4c). Its destructor empties them
//!   (`:60-63`). Its handler creates Story for the player of a
//!   `PlayerLogin` (`:68-80`, [`handle_launcher_message`]).
//! - **Story** (`FStory::FStory`, `Story.cpp:100-129`): the player's mission
//!   number 0, no money to the next rank and a 5 s heartbeat
//!   ([`construct_story`]). Its handler (`:438-474`, [`handle_message`])
//!   saves and restores `[StoryInfo]` ([`save`], [`load`], P3.5b), starts
//!   the next chapter's mission once the player's worth reaches the worth
//!   the last chapter set (`AdvanceMission`, `:206-267`) and lowers the
//!   player's standing with `gd_bh_grp` when he buys a ship between
//!   chapters 15 and 22 (`:159-167`).
//! - **The story's events** (`FStory::OnMissionEvent`, `:270-351`,
//!   [`on_mission_event`]): a story mission reports its acceptance (kind 1)
//!   and its success (kind 3) with its chapter. An acceptance sets the
//!   chapter; a success ends the mission (`pub::Controller::Destroy`), sets
//!   the next chapters and either starts the next mission at once or sets
//!   the worth that starts it. The C++'s `IStoryMissionEvents*` is the
//!   Story's controller id here; a mission without one reports to nothing.
//!
//! Story is called synchronously from inside a mission's handler, as the
//! C++ calls through its pointer: its state is looked up by its controller
//! id at each step and never held across a call that can dispatch (record
//! 0008 point 1). The Tier B story trace ([`trace`]) records its messages,
//! events, creates, destroys and states.

pub mod trace;

use std::io::{self, Write};
use std::sync::Arc;

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::ini::IniReader;
use neuron_core::msvcrt::format_f;
use neuron_core::num::trunc_to_i32;
use neuron_server::controller::{ControllerId, Priority};

use super::mission::MissionPath;
use super::{Instance, npc_desc, players};
use crate::api;
use crate::events::ControllerMessage;
use crate::server::{CreateParams, Cx};

/// The low 30 bits of a controller id.
const ID_MASK: u32 = 0x3fff_ffff;

/// StoryLauncher's heartbeat period: none (`StoryLauncher.cpp:42`).
pub const LAUNCHER_HEARTBEAT: f32 = -1.0;

/// `FStory`'s own state (`Story.cpp:58-97`, 0x3c bytes). Its save
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
    /// The constructor's values (`Story.cpp:100-129`) for `player`.
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

/// A call out of Story's restore, in the order the C++ makes it. The
/// detached [`load`] returns them; Story's own handler makes them as it
/// reads ([`handle_message`]).
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

/// `GStoryRankThresholds` (`Story.cpp:26`): the worth above the player's
/// that each chapter waits for, by mission number. Story's constructor
/// writes the thirteen entries of `:104-116` (six of them 0) before anything
/// reads them, so they are constants here; the rest stay 0.
pub const RANK_THRESHOLDS: [f32; 43] = {
    let mut t = [0.0; 43];
    t[5] = 3500.0;
    t[9] = 5200.0;
    t[12] = 7000.0;
    t[15] = 10000.0;
    t[18] = 16000.0;
    t[24] = 36000.0;
    t[27] = 30000.0;
    t
};

/// Story's heartbeat period (`Story.cpp:128`); its handler has no
/// heartbeat case.
pub const STORY_HEARTBEAT: f32 = 5.0;

/// The mission number of the story's end: neither the worth nor a mission
/// change starts a mission any more.
pub const LAST_CHAPTER: i32 = 42;

/// `FStory::OnSave(File)` (`Story.cpp:354-365`): `[StoryInfo]` with the
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

/// The Story a restore reads into: one held by the caller (the detached
/// [`load`], which only lists its calls out), or the instance of a
/// controller, whose calls out are made as the C++ makes them.
enum Target<'s> {
    Detached(&'s mut Story),
    Live(ControllerId),
}

impl Target<'_> {
    /// Runs `f` on the Story; `None` when the controller has none.
    fn with<R>(&mut self, cx: &mut Cx<'_>, f: impl FnOnce(&mut Story) -> R) -> Option<R> {
        match self {
            Target::Detached(story) => Some(f(story)),
            Target::Live(id) => story_mut(cx, *id).map(f),
        }
    }
}

/// `FStory::OnLoad(Reader)` (`Story.cpp:368-435`): the reader's state kept
/// around a read from its start. The mission number goes to 0 (and the
/// player's mission number with it), then every `[StoryInfo]` value:
///
/// - `Mission` takes value 0 as the mission's name (at most 31 bytes: the
///   C++ `strcpy` into its 0x20 bytes overruns for a longer one, D834) and,
///   unless it is `No_Mission`, creates the mission at `NORMAL`
///   ([`StoryLoadStep::CreateMission`]);
/// - `MissionNum` sets the number and the player's mission number (no rank
///   change), then sends `PlayerLoad` with the reader and `MissionStart` to
///   the mission Story created last, when there is one
///   ([`StoryLoadStep::LoadMission`]);
/// - `delta_worth` the worth and the player's money to the next rank (its
///   truncation);
/// - `ship_bought` the flag; `debug` the process-wide flag.
fn load_into(
    cx: &mut Cx<'_>,
    mut target: Target<'_>,
    reader: &mut IniReader,
) -> Vec<StoryLoadStep> {
    let saved = reader.state();
    reader.reset();
    let mut steps = Vec::new();
    let Some(player) = target.with(cx, |s| {
        s.mission_num = 0;
        s.player
    }) else {
        reader.set_state(&saved);
        return steps;
    };
    players::set_mission_number(cx, player, 0, false);
    while reader.read_header() {
        if !reader.is_header(b"StoryInfo") {
            continue;
        }
        while reader.read_value() {
            if reader.is_value(b"Mission") {
                let mut name = c_str(&reader.value_string_at(0)).to_vec();
                name.truncate(MISSION_NAME_BUFFER - 1);
                let _ = target.with(cx, |s| s.mission_name.clone_from(&name));
                if !eq_ignore_case(&name, NO_MISSION) {
                    let path = eq_ignore_case(&name, b"GenericMission")
                        .then(|| MissionPath::new(&reader.value_string_at(1)));
                    if let Target::Live(id) = target {
                        create_mission(cx, id, player, &name, path, Priority::Normal);
                    }
                    steps.push(StoryLoadStep::CreateMission { name, path });
                }
            } else if reader.is_value(b"MissionNum") {
                let number = reader.value_int(0);
                let _ = target.with(cx, |s| s.mission_num = number);
                players::set_mission_number(cx, player, number as u32, false);
                if let Target::Live(_) = target {
                    let mission = cx.logic.content.state.story_mission;
                    if mission.0 & ID_MASK != 0 {
                        let mut load = ControllerMessage::PlayerLoad {
                            player,
                            reader: &mut *reader,
                        };
                        let _ = api::controller::send_message(cx, mission, &mut load);
                        let mut start = ControllerMessage::MissionStart;
                        let _ = api::controller::send_message(cx, mission, &mut start);
                    }
                }
                steps.push(StoryLoadStep::LoadMission);
            } else if reader.is_value(b"delta_worth") {
                let worth = reader.value_float(0) as f32;
                let _ = target.with(cx, |s| s.delta_worth = worth);
                let money = trunc_to_i32(f64::from(worth));
                let _ = api::player::set_money_needed_to_next_rank(cx, player, money);
            } else if reader.is_value(b"ship_bought") {
                let bought = reader.value_bool(0);
                let _ = target.with(cx, |s| s.ship_bought = bought);
            } else if reader.is_value(b"debug") {
                cx.logic.content.state.story_debug = u8::from(reader.value_bool(0));
            }
        }
    }
    reader.set_state(&saved);
    steps
}

/// [`load_into`] a Story the caller holds: the restore's values, with its
/// calls out returned as [`StoryLoadStep`]s and not made (the save
/// comparisons of P3.5b).
pub fn load(cx: &mut Cx<'_>, story: &mut Story, reader: &mut IniReader) -> Vec<StoryLoadStep> {
    load_into(cx, Target::Detached(story), reader)
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

/// `PlayerLoad {player, reader}` to a Story the caller holds: [`load`] for
/// its own player.
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

/// The Story of controller `id`.
#[must_use]
pub fn story<'c>(cx: &'c Cx<'_>, id: ControllerId) -> Option<&'c Story> {
    match cx.logic.content.instance(id) {
        Some(Instance::Story(story)) => Some(story),
        _ => None,
    }
}

/// The Story of controller `id`, to change between call-outs.
pub fn story_mut<'c>(cx: &'c mut Cx<'_>, id: ControllerId) -> Option<&'c mut Story> {
    match cx.logic.content.instance_mut(id) {
        Some(Instance::Story(story)) => Some(story),
        _ => None,
    }
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

/// `FStoryLauncher::HandleMessage` (`StoryLauncher.cpp:68-80`): a
/// `PlayerLogin` creates `Story` for its player at `NORMAL`. The C++ names
/// Content.dll by its module's full path (`GetModuleFileNameA`), which
/// loads the same module; the port's bus does not read the name.
pub fn handle_launcher_message(
    cx: &mut Cx<'_>,
    _id: ControllerId,
    message: &mut ControllerMessage<'_>,
) {
    let ControllerMessage::PlayerLogin { player } = *message else {
        return;
    };
    let params = CreateParams::Story { player };
    let story = api::controller::create(cx, b"Content.dll", b"Story", &params, Priority::Normal);
    trace::create(cx, b"Story", story);
}

/// `CreateStory` and `FStory::FStory(&ControllerId, Parms)`
/// (`Story.cpp:100-129`, `:477-485`): the instance with the player of its
/// `FStoryCreateParms` (other parameters read as player 0, D770), mission
/// number 0 (the player's too, without a rank change), the worth -1 and no
/// mission; then no money to the next rank and a 5 s heartbeat (one
/// `rand()`).
pub fn construct_story(cx: &mut Cx<'_>, id: ControllerId, params: &CreateParams) -> Instance {
    let player = match *params {
        CreateParams::Story { player }
        | CreateParams::StoryMission { player, .. }
        | CreateParams::GenericMission { player, .. } => player,
        CreateParams::None | CreateParams::RandomMission(_) => 0,
    };
    players::set_mission_number(cx, player, 0, false);
    let _ = api::player::set_money_needed_to_next_rank(cx, player, 0);
    super::set_heartbeat(cx, id, STORY_HEARTBEAT);
    Instance::Story(Story::new(player))
}

/// `FStory::HandleMessage` (`Story.cpp:438-474`):
///
/// - `PlayerWorthChanged {player}`: with a worth set (not -1), before the
///   story's end, for its player, `AdvanceMission`;
/// - `PlayerSave` and `PlayerLoad` for its player: [`save`] and the
///   restore, whose calls out are made as it reads;
/// - `PlayerShipArchSet {player}`: with the player's state, in chapters 15
///   to 22, for its player, once: the ship-bought standing;
/// - `PlayerMissionChanged {mission, player}`: with a worth set, before
///   the story's end, for its player leaving his mission (an id whose low
///   30 bits are 0), `AdvanceMission`.
pub fn handle_message(cx: &mut Cx<'_>, id: ControllerId, message: &mut ControllerMessage<'_>) {
    use ControllerMessage as M;
    let Some(s) = story(cx, id) else { return };
    let (player, num, worth, bought) = (s.player, s.mission_num, s.delta_worth, s.ship_bought);
    trace::message(cx, message.id());
    let waiting = worth != -1.0 && num != LAST_CHAPTER;
    match message {
        M::PlayerWorthChanged { client } if waiting && *client == player => {
            advance_mission(cx, id);
        }
        M::PlayerSave { client, file } => {
            if *client == player
                && let Some(s) = story(cx, id)
            {
                // The C++ does not test `fprintf`.
                let _ = save(cx, s, &mut **file);
            }
        }
        M::PlayerLoad { player: p, reader } if *p == player => {
            let _ = load_into(cx, Target::Live(id), reader);
        }
        M::PlayerShipArchSet { client } => {
            let has_state = cx.logic.content.state.players.contains_key(&player);
            if has_state && (15..23).contains(&num) && player == *client && !bought {
                set_ship_bought_reputation(cx, id);
            }
        }
        M::PlayerMissionChanged { mission, player: p }
            if waiting && *p == player && mission.0 & ID_MASK == 0 =>
        {
            advance_mission(cx, id);
        }
        _ => {}
    }
    trace::state(cx, id);
}

/// `FStory::SetMissionChapter(Chapter, bAdvanceRank)` (`Story.cpp:135-141`):
/// the mission number, and the player's with its rank change when the
/// player has a state (`SetContentPlayerMissionNumber`).
fn set_mission_chapter(cx: &mut Cx<'_>, id: ControllerId, chapter: u32, advance_rank: bool) {
    let Some(s) = story_mut(cx, id) else { return };
    s.mission_num = chapter as i32;
    let player = s.player;
    if cx.logic.content.state.players.contains_key(&player) {
        players::set_mission_number(cx, player, chapter, advance_rank);
    }
}

/// The player's worth, `(double)cash + (double)asset value`, as the C++
/// leaves it on the FPU; each getter writes 0 before it fails.
fn worth(cx: &Cx<'_>, player: u32) -> f64 {
    let cash = api::player::inspect_cash(cx, player).unwrap_or(0);
    let asset = api::player::asset_value(cx, player).unwrap_or(0.0);
    f64::from(cash) + f64::from(asset)
}

/// `FStory::SetNextRankWorth(State, Chapter)` (`Story.cpp:146-156`): the
/// worth plus the chapter's threshold, stored as the worth that starts the
/// next mission, and its truncation (of the unrounded sum) as the player's
/// money to the next rank.
fn set_next_rank_worth(cx: &mut Cx<'_>, id: ControllerId, chapter: u32) {
    let Some(player) = story(cx, id).map(|s| s.player) else {
        return;
    };
    let threshold = RANK_THRESHOLDS
        .get(chapter as usize)
        .copied()
        .unwrap_or(0.0);
    let new_worth = worth(cx, player) + f64::from(threshold);
    if let Some(s) = story_mut(cx, id) {
        s.delta_worth = new_worth as f32;
    }
    let _ = api::player::set_money_needed_to_next_rank(cx, player, trunc_to_i32(new_worth));
}

/// `FStory::SetShipBoughtReputation` (`Story.cpp:159-167`): the flag, then
/// the player's standing with `gd_bh_grp` set to -0.2 (the vibe -1 and the
/// group `0xffffffff` when the getters fail).
fn set_ship_bought_reputation(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = story_mut(cx, id) else { return };
    s.ship_bought = true;
    let player = s.player;
    let vibe = api::player::rep(cx, player).unwrap_or(-1);
    let group = api::reputation::reputation_group(cx, b"gd_bh_grp").unwrap_or(u32::MAX);
    let _ = api::reputation::set_reputation(cx, vibe, group, -0.2);
}

/// `pub::Controller::Create("Content.dll", name, {player, this story,
/// path}, priority)` into `GStoryMissionController`.
fn create_mission(
    cx: &mut Cx<'_>,
    id: ControllerId,
    player: u32,
    name: &[u8],
    path: Option<MissionPath>,
    priority: Priority,
) -> ControllerId {
    let params = match path {
        Some(path) => CreateParams::GenericMission {
            player,
            story: id,
            path,
        },
        None => CreateParams::StoryMission { player, story: id },
    };
    let mission = api::controller::create(cx, b"Content.dll", name, &params, priority);
    cx.logic.content.state.story_mission = mission;
    trace::create(cx, name, mission);
    mission
}

/// The mission's create at `ABOVE_NORMAL` and `MissionStart` to it (to
/// every controller when the create gave 0, as the C++'s send to id 0
/// does).
fn create_and_start(cx: &mut Cx<'_>, id: ControllerId, name: &[u8]) {
    let Some(player) = story(cx, id).map(|s| s.player) else {
        return;
    };
    let mission = create_mission(cx, id, player, name, None, Priority::AboveNormal);
    let mut start = ControllerMessage::MissionStart;
    let _ = api::controller::send_message(cx, mission, &mut start);
}

/// `FStory::StartMission(Chapter, Name)` (`Story.cpp:170-184`): the
/// mission's name, the worth -1 and no money to the next rank, the
/// chapter (with its rank), then the mission created and started.
fn start_mission(cx: &mut Cx<'_>, id: ControllerId, chapter: u32, name: &[u8]) {
    let Some(s) = story_mut(cx, id) else { return };
    s.mission_name = name.to_vec();
    s.delta_worth = -1.0;
    let player = s.player;
    let _ = api::player::set_money_needed_to_next_rank(cx, player, 0);
    set_mission_chapter(cx, id, chapter, true);
    create_and_start(cx, id, name);
}

/// `pub::Controller::Destroy` of a mission, recorded first.
fn destroy(cx: &mut Cx<'_>, controller: u32) {
    trace::destroy(cx, controller);
    api::controller::destroy(cx, ControllerId(controller));
}

/// `FStory::FinishMission(Controller, NextChapter)` (`Story.cpp:187-203`):
/// no mission, the controller destroyed, the next chapter (with its rank);
/// then, with the player's state, the rank recalculated at the story's end
/// or the next chapter's worth.
fn finish_mission(cx: &mut Cx<'_>, id: ControllerId, controller: u32, next: u32) {
    let Some(s) = story_mut(cx, id) else { return };
    s.mission_name = NO_MISSION.to_vec();
    let player = s.player;
    destroy(cx, controller);
    set_mission_chapter(cx, id, next, true);
    let Some(state) = cx.logic.content.state.players.get(&player).cloned() else {
        return;
    };
    if next == LAST_CHAPTER as u32 {
        players::recalculate_rank(cx, &state);
        return;
    }
    set_next_rank_worth(cx, id, next);
}

/// `FStory::AdvanceMission` (`Story.cpp:206-267`): with the player's state,
/// a worth not below the one set, and no live mission, the chapter that
/// waits for it starts its mission.
fn advance_mission(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(player) = story(cx, id).map(|s| s.player) else {
        return;
    };
    if !cx.logic.content.state.players.contains_key(&player) {
        return;
    }
    let current = worth(cx, player);
    let Some(s) = story(cx, id) else { return };
    if current < f64::from(s.delta_worth) {
        return;
    }
    let live = cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map_or(0, |p| p.live_mission);
    if live & ID_MASK != 0 {
        return;
    }
    let next: Option<(u32, &[u8])> = match s.mission_num {
        5 => Some((7, b"Mission_02")),
        9 => Some((10, b"Mission_03")),
        12 => Some((13, b"Mission_04")),
        15 => Some((16, b"Mission_05")),
        18 => Some((19, b"Mission_06")),
        21 => Some((22, b"Mission_07")),
        24 => Some((25, b"Mission_08")),
        27 => Some((28, b"Mission_09")),
        30 => Some((31, b"Mission_10")),
        33 => Some((34, b"Mission_11")),
        36 => Some((37, b"Mission_12")),
        39 => Some((40, b"Mission_13")),
        _ => None,
    };
    if let Some((chapter, name)) = next {
        start_mission(cx, id, chapter, name);
    }
}

/// `FStory::OnMissionEvent(EventKind, Chapter, Controller)`
/// (`Story.cpp:270-351`), the story facet a mission calls; nothing for a
/// controller without a Story (the C++'s null `StoryEvents`, D1046).
///
/// - Kind 1, an acceptance: the chapter (with its rank); chapter 23 also
///   lowers the standing with `gd_bh_grp`.
/// - Kind 3, a success, by chapter: 2 (`Mission_01a`) destroys the mission,
///   sets chapters 3 and 4 and creates and starts `Mission_01b`; 4, 8, 11,
///   14 and 17 end the mission and set the next chapter's worth; 20, 29,
///   32, 35 and 38 finish the mission and start the next one at once; 23,
///   26 and 41 finish it only (41 recalculates the rank).
pub fn on_mission_event(
    cx: &mut Cx<'_>,
    id: ControllerId,
    kind: u32,
    chapter: u32,
    controller: u32,
) {
    if story(cx, id).is_none() {
        return;
    }
    trace::event(cx, kind, chapter, controller);
    match kind {
        1 => {
            set_mission_chapter(cx, id, chapter, true);
            if chapter == 23 {
                set_ship_bought_reputation(cx, id);
            }
        }
        3 => on_success(cx, id, chapter, controller),
        _ => {}
    }
    trace::state(cx, id);
}

/// The success arm of [`on_mission_event`] (`Story.cpp:286-350`).
fn on_success(cx: &mut Cx<'_>, id: ControllerId, chapter: u32, controller: u32) {
    match chapter {
        2 => {
            destroy(cx, controller);
            set_mission_chapter(cx, id, 3, true);
            set_mission_chapter(cx, id, 4, true);
            if let Some(s) = story_mut(cx, id) {
                s.mission_name = b"Mission_01b".to_vec();
            }
            create_and_start(cx, id, b"Mission_01b");
        }
        4 | 8 | 11 | 14 | 17 => {
            if let Some(s) = story_mut(cx, id) {
                s.mission_name = NO_MISSION.to_vec();
            }
            destroy(cx, controller);
            set_mission_chapter(cx, id, chapter + 1, true);
            let player = story(cx, id).map_or(0, |s| s.player);
            if cx.logic.content.state.players.contains_key(&player) {
                set_next_rank_worth(cx, id, chapter + 1);
            }
        }
        20 | 29 | 32 | 35 | 38 => {
            finish_mission(cx, id, controller, chapter + 1);
            let (next, name): (u32, &[u8]) = match chapter {
                20 => (22, b"Mission_07"),
                29 => (31, b"Mission_10"),
                32 => (34, b"Mission_11"),
                35 => (37, b"Mission_12"),
                _ => (40, b"Mission_13"),
            };
            start_mission(cx, id, next, name);
        }
        23 | 26 | 41 => finish_mission(cx, id, controller, chapter + 1),
        _ => {}
    }
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
