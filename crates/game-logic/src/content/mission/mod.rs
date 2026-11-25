//! The script missions: `Mission_01a` to `Mission_13` and `GenericMission`
//! (`Content/StoryMissions.cpp`, the mission object of
//! `Content/MissionObject.cpp` and the script of `Content/MissionScript.cpp`;
//! record 0008 points 2 and 6; work package P3.5a).
//!
//! A mission's constructor ([`construct`]) sets the mission object's
//! heartbeat (1/30 s, `MissionObject.cpp:101`, one `rand()`), takes its
//! player, sets its chapter and, for `Mission_01a`, the heartbeat again
//! (`StoryMissions.cpp:100`), then loads its mission file
//! (`LoadMissionIni`, [`load_mission`]):
//!
//! 1. the mission object's sections ([`object`]: ships, solars, loot,
//!    random encounters with their [`rand_enc`] parameters, object lists
//!    ([`obj_list`]), meta behaviours ([`meta_behavior`]), dialogs
//!    ([`dialog`]), `[Mission]` and the objectives of [`objective`]);
//! 2. then, from the top again, the script's `[Trigger]` ([`trigger`],
//!    with the [`condition`]s and [`action`]s), `[NPC]` and the `[Dialog]`
//!    speakers' costumes.
//!
//! The parse draws `rand()` from the world's generator in the C++ order
//! (the random encounters' activation times and formation counts, then
//! `Cnd_Timer`'s ranges) and calls the `pub::` getters of record 0008. The
//! saves came with P3.5b ([`save`] for the script's sections,
//! [`object_save`] for the mission object's); the run time is P3.5c's, the
//! story shell P3.5d's. The
//! mission object's `[ObjList]` directives ([`obj_list`]) and
//! `[MetaBehavior]` objects ([`meta_behavior`]) came with P3.5a2; they draw
//! nothing.

pub mod action;
pub mod condition;
pub mod dialog;
pub mod engine;
pub mod execute;
pub mod field;
pub mod meta_behavior;
pub mod obj_list;
pub mod object;
pub mod object_save;
pub mod objective;
pub mod rand_enc;
pub mod runtime;
pub mod save;
pub mod spawn;
pub mod trace;
pub mod trigger;

use std::collections::BTreeMap;

use game_sim::data::costume::Costume;
use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::ini::IniReader;
use neuron_server::controller::ControllerId;

use self::object::{MissionObject, MissionPlayer};
use self::trigger::Trigger;
use super::Instance;
use crate::api;
use crate::content::tables;
use crate::server::{ControllerKind, CreateParams, Cx};

/// The size of `FStoryOnLoadCreateParms::GenericMissionPath`
/// (`Content/Story.cpp:37-42`), terminator included.
pub const MISSION_PATH_BUFFER: usize = 0x3c;

/// A `GenericMission`'s script path as its create parameters carry it:
/// at most [`MISSION_PATH_BUFFER`] − 1 bytes, the rest of a longer path cut
/// off (D773).
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct MissionPath {
    len: u8,
    bytes: [u8; MISSION_PATH_BUFFER - 1],
}

impl MissionPath {
    /// The path, cut at its first NUL (as `strcpy` copies it) and to the
    /// buffer.
    #[must_use]
    pub fn new(path: &[u8]) -> Self {
        let path = path.split(|&b| b == 0).next().unwrap_or_default();
        let mut bytes = [0; MISSION_PATH_BUFFER - 1];
        let len = path.len().min(bytes.len());
        bytes[..len].copy_from_slice(&path[..len]);
        Self {
            len: u8::try_from(len).unwrap_or(u8::MAX),
            bytes,
        }
    }

    /// The path's bytes.
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes[..usize::from(self.len)]
    }
}

impl Default for MissionPath {
    fn default() -> Self {
        Self::new(b"")
    }
}

impl std::fmt::Debug for MissionPath {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "MissionPath({:?})",
            String::from_utf8_lossy(self.as_bytes())
        )
    }
}

/// `G_MissionStateNoState`: a new script's previous and desired states.
pub const MISSION_STATE_NO_STATE: i32 = 2;

/// One story mission's constructor values (`StoryMissions.cpp:85-437`):
/// `SetChapter`'s four values and the mission file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Chapter {
    /// `CampaignSlotCurrent`.
    pub slot_current: i32,
    /// `CampaignSlotSucceed`.
    pub slot_succeed: i32,
    /// `Field_0x2c0`.
    pub field_0x2c0: u32,
    /// `Field_0x2c4`.
    pub field_0x2c4: u32,
    /// The file `LoadMissionIni` opens, relative to the data root.
    pub file: &'static [u8],
}

/// The fourteen story missions' chapters, `Mission_01a`, `Mission_01b`,
/// then `Mission_02` to `Mission_13`.
pub const CHAPTERS: [(&str, Chapter); 14] = {
    const fn c(
        slot_current: i32,
        slot_succeed: i32,
        field_0x2c0: u32,
        field_0x2c4: u32,
        file: &'static [u8],
    ) -> Chapter {
        Chapter {
            slot_current,
            slot_succeed,
            field_0x2c0,
            field_0x2c4,
            file,
        }
    }
    [
        (
            "Mission_01a",
            c(1, 2, 0x5528, 0x7d00, b"Missions\\M01a\\m01a.ini"),
        ),
        (
            "Mission_01b",
            c(4, 4, 0x55f0, 0x7d64, b"Missions\\M01b\\m01b.ini"),
        ),
        (
            "Mission_02",
            c(7, 8, 0x56b8, 0x7dc8, b"Missions\\M02\\m02.ini"),
        ),
        (
            "Mission_03",
            c(10, 11, 0x5780, 0x7e2c, b"Missions\\M03\\m03.ini"),
        ),
        (
            "Mission_04",
            c(13, 14, 0x5848, 0x7e90, b"Missions\\M04\\m04.ini"),
        ),
        (
            "Mission_05",
            c(16, 17, 0x5910, 0x7ef4, b"Missions\\M05\\m05.ini"),
        ),
        (
            "Mission_06",
            c(19, 20, 0x59d8, 0x7f58, b"Missions\\M06\\m06.ini"),
        ),
        (
            "Mission_07",
            c(22, 23, 0x5aa0, 0x7fbc, b"Missions\\M07\\m07.ini"),
        ),
        (
            "Mission_08",
            c(25, 26, 0x5b68, 0x8020, b"Missions\\M08\\m08.ini"),
        ),
        (
            "Mission_09",
            c(28, 29, 0x5c30, 0x8084, b"Missions\\M09\\m09.ini"),
        ),
        (
            "Mission_10",
            c(31, 32, 0x5cf8, 0x80e8, b"Missions\\M10\\m10.ini"),
        ),
        (
            "Mission_11",
            c(34, 35, 0x5dc0, 0x814c, b"Missions\\M11\\m11.ini"),
        ),
        (
            "Mission_12",
            c(37, 38, 0x5e88, 0x81b0, b"Missions\\M12\\m12.ini"),
        ),
        (
            "Mission_13",
            c(40, 41, 0x5f50, 0x8214, b"Missions\\M13\\m13.ini"),
        ),
    ]
};

/// The chapter of a story mission's kind; none for `GenericMission`.
#[must_use]
pub fn chapter(kind: ControllerKind) -> Option<Chapter> {
    let index = match kind {
        ControllerKind::Mission01a => 0,
        ControllerKind::Mission01b => 1,
        ControllerKind::Mission(n @ 2..=13) => usize::from(n),
        _ => return None,
    };
    Some(CHAPTERS[index].1)
}

/// The fields only `Mission_01a` and `Mission_01b` add
/// (`StoryMissions.cpp:63-205`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StoryMissionFields {
    /// `bBaseEnterHandled` (+0x32c, 0).
    pub base_enter_handled: bool,
    /// `Mission_01a`: `Field_0x334` and `Field_0x3b8` (-1), `Field_0x43c`
    /// and `Field_0x43d` (0); none for the other missions.
    pub mission_01a: Option<(i32, i32, u8, u8)>,
    /// `Mission_01a`: `GHe0101BaseId` and `GHe0102BaseId`, the bases its
    /// constructor looks up after the file.
    pub bases: Option<(u32, u32)>,
}

/// A script mission's own state (`FMissionScript` over the mission object,
/// and its story-mission forms).
#[derive(Clone, Debug, PartialEq)]
pub struct MissionScript {
    /// The factory kind: which story mission, or `GenericMission`.
    pub kind: ControllerKind,
    /// `PlayerId`.
    pub player: u32,
    /// The Story controller the mission reports to (the C++'s
    /// `IStoryMissionEvents*`); 0 for none.
    pub story: ControllerId,
    /// `GenericMission`'s script path; empty for the story missions.
    pub path: MissionPath,
    /// The mission object (TU38).
    pub object: MissionObject,
    /// `TriggerMap`: every trigger by id.
    pub triggers: BTreeMap<u32, Trigger>,
    /// `InitiallyActive`: the `InitState = ACTIVE` triggers, `{id, id}`.
    pub initially_active: BTreeMap<u32, u32>,
    /// `PreviousMissionState` (2).
    pub previous_state: i32,
    /// `MissionState`, the desired state (2).
    pub mission_state: i32,
    /// `Field_0x2b0` (0).
    pub field_0x2b0: bool,
    /// `CampaignSlotCurrent`.
    pub campaign_slot_current: i32,
    /// `CampaignSlotSucceed`.
    pub campaign_slot_succeed: i32,
    /// `StoryCue` (0).
    pub story_cue: i32,
    /// `Field_0x2c0`.
    pub field_0x2c0: u32,
    /// `Field_0x2c4`.
    pub field_0x2c4: u32,
    /// `Field_0x2e8`: the mad lib shown with a state change (1).
    pub field_0x2e8: u32,
    /// `DialogCostumes`: the costumes of the speakers who talk to the
    /// player, each once (`AddDialogCostume`).
    pub dialog_costumes: Vec<Costume>,
    /// `ElapsedTime` (0).
    pub elapsed_time: f32,
    /// The story missions' own fields.
    pub story_fields: StoryMissionFields,
    /// `ActiveTriggers`: the active triggers by id, in activation order
    /// (record 0006 point 7's cursor walk; filled by the run time, P3.5c).
    pub active_triggers: Vec<u32>,
    /// `PendingActivations`: target id to source id (P3.5c).
    pub pending_activations: BTreeMap<u32, u32>,
    /// `NnPath` (+0x21c): `Act_NNPath`'s path, or a restored `nnpath`.
    pub nn_path: save::NnPath,
    /// `SaveTriggerId`: the trigger `Act_Save` saves (P3.5c).
    pub save_trigger_id: u32,
    /// `Field_0x2ec`: a restore read `[MissionState]`.
    pub field_0x2ec: bool,
    /// `List_0x2f4`: the cut scenes.
    pub rtc_scripts: Vec<save::RtcScript>,
    /// `FriendlyFireOffLabels` (+0x2fc).
    pub friendly_fire_excused: Vec<Vec<u8>>,
    /// `AmbientScenes` (+0x310).
    pub ambient_scenes: Vec<save::AmbientScene>,
    /// `Timers` (+0x1f8): the delayed actions, each by its trigger and its
    /// place there, with the seconds left (P3.5c1).
    pub timers: Vec<engine::MissionTimer>,
    /// `NagClamp` (+0x218).
    pub nag_clamp: bool,
    /// `Field_0x2c8` (+0x2c8): a static camera is up.
    pub field_0x2c8: bool,
    /// `bRepPopUpShown` (+0x2cc).
    pub rep_pop_up_shown: bool,
    /// `bRepPopUpArmed` (+0x2cd).
    pub rep_pop_up_armed: bool,
    /// `RepPopUpSystemId` (+0x2d0).
    pub rep_pop_up_system: u32,
    /// `RepPopUpGroups` (+0x2d4).
    pub rep_pop_up_groups: Vec<u32>,
    /// `CurrentLocationId` (+0x308): the player's last location on a base.
    pub current_location: u32,
    /// The places of the trigger walks in progress, innermost last (the
    /// cursor walk of [`engine::dispatch`]; not the C++'s, which walks with
    /// list iterators).
    pub walks: Vec<usize>,
    /// The Tier B trace ([`trace`]), off unless a session turns it on.
    pub trace: Option<trace::Trace>,
}

impl MissionScript {
    /// `GetMissionCostume` of the mission object.
    #[must_use]
    pub fn mission_costume(&self, nickname: &[u8]) -> Option<Costume> {
        self.object.costume(nickname)
    }

    /// `AddDialogCostume`: appended unless an equal costume is there
    /// (`IsSameCostume`: the parts, the count and the accessories).
    pub fn add_dialog_costume(&mut self, costume: &Costume) {
        if !self.dialog_costumes.contains(costume) {
            self.dialog_costumes.push(costume.clone());
        }
    }
}

/// A `TString<N>` assignment: at most `N - 1` bytes, up to the first NUL.
pub(crate) fn tstring(text: &[u8], size: usize) -> Vec<u8> {
    let mut text = c_str(text).to_vec();
    text.truncate(size - 1);
    text
}

/// `TString<N> = get_value_string(index)`.
pub(crate) fn value_tstring(reader: &IniReader, index: u32, size: usize) -> Vec<u8> {
    tstring(&reader.value_string_at(index), size)
}

/// `get_value_float(index)`: the parsed number stored as a float.
pub(crate) fn f32_at(reader: &mut IniReader, index: u32) -> f32 {
    reader.value_float(index) as f32
}

/// `pub::GetBaseID`: 0 first, then the base of a known nickname.
pub(crate) fn base_id_of(cx: &mut Cx<'_>, name: &[u8]) -> u32 {
    api::misc::base_id(cx, name).unwrap_or(0)
}

/// `pub::GetSystemID` into a slot, written only for a known system; true
/// then.
pub(crate) fn system_id_into(cx: &mut Cx<'_>, slot: &mut u32, name: &[u8]) -> bool {
    match api::misc::system_id(cx, name) {
        Ok(id) => {
            *slot = id;
            true
        }
        Err(_) => false,
    }
}

/// `pub::GF::FindLocation`.
pub(crate) fn find_location(base: u32, name: &[u8]) -> u32 {
    api::gf::find_location(base, c_str(name))
}

/// `pub::GetNicknameId`: `CreateID`.
pub(crate) fn nickname_id(name: &[u8]) -> u32 {
    api::misc::nickname_id(c_str(name))
}

/// `GetRepValueFromName` (`MissionAct.cpp:46-63`): the attitude of a
/// `REP_*` name (`_stricmp`), -2 for any other.
#[must_use]
pub fn rep_value_from_name(name: &[u8]) -> f32 {
    const NAMES: [(&[u8], f32); 7] = [
        (b"REP_HOSTILE_MAXIMUM", -1.0),
        (b"REP_HOSTILE_THRESHOLD", -0.6),
        (b"REP_NEUTRAL_HOSTILE", -0.2),
        (b"REP_NEUTRAL", 0.0),
        (b"REP_NEUTRAL_FRIENDLY", 0.2),
        (b"REP_FRIEND_THRESHOLD", 0.6),
        (b"REP_FRIEND_MAXIMUM", 1.0),
    ];
    NAMES
        .iter()
        .find(|(n, _)| eq_ignore_case(c_str(name), n))
        .map_or(-2.0, |&(_, v)| v)
}

/// `FMissionScript::LoadMission` (`MissionScript.cpp:956-1003`): the
/// mission object's sections, then from the top again every `[Trigger]`,
/// `[NPC]` (first nickname wins) and the `[Dialog]` lines spoken to
/// `Player`, whose speakers' costumes go to the dialog costumes.
pub fn load_mission(cx: &mut Cx<'_>, reader: &mut IniReader, script: &mut MissionScript) {
    object::load(cx, reader, &mut script.object);
    reader.reset();
    let mut triggers = std::mem::take(&mut script.triggers);
    let mut initially_active = std::mem::take(&mut script.initially_active);
    while reader.read_header() {
        if reader.is_header(b"Trigger") {
            trigger::load_section(cx, reader, script, &mut triggers, &mut initially_active);
            continue;
        }
        if reader.is_header(b"NPC") {
            let (nickname, npc) = object::load_npc(cx, reader);
            script.object.npcs.entry(nickname).or_insert(npc);
            continue;
        }
        if reader.is_header(b"Dialog") {
            while reader.read_value() {
                if !reader.is_value(b"line")
                    || !eq_ignore_case(c_str(&reader.value_string_at(1)), b"Player")
                {
                    continue;
                }
                let speaker = value_tstring(reader, 0, 0x30);
                if let Some(costume) = script.mission_costume(&speaker) {
                    script.add_dialog_costume(&costume);
                }
            }
        }
    }
    script.triggers = triggers;
    script.initially_active = initially_active;
}

/// `LoadMissionIni`: the file of the world's data root (a closed reader
/// when it does not open, as the C++ does not test the open), then
/// [`load_mission`].
pub fn load_mission_file(cx: &mut Cx<'_>, file: &[u8], script: &mut MissionScript) {
    let source = super::data_source(cx);
    let mut reader = tables::open(&*source, file);
    load_mission(cx, &mut reader, script);
}

/// The factory's mission constructors (`StoryMissions.cpp`,
/// `FMissionScript::FMissionScript`, `FMissionObject::FMissionObject`): the
/// mission object's heartbeat, the player (its state and mission record),
/// the chapter, `Mission_01a`'s second heartbeat, then the mission file and
/// `Mission_01a`'s two bases. A form of the create parameters without a
/// field reads it as 0 or empty (D770).
pub fn construct(
    cx: &mut Cx<'_>,
    id: ControllerId,
    kind: ControllerKind,
    params: &CreateParams,
) -> Instance {
    Instance::Mission(Box::new(construct_script(cx, id, kind, params)))
}

/// [`construct`]'s mission.
pub fn construct_script(
    cx: &mut Cx<'_>,
    id: ControllerId,
    kind: ControllerKind,
    params: &CreateParams,
) -> MissionScript {
    let (player, story, path) = match *params {
        CreateParams::Story { player } => (player, ControllerId::BROADCAST, MissionPath::default()),
        CreateParams::StoryMission { player, story } => (player, story, MissionPath::default()),
        CreateParams::GenericMission {
            player,
            story,
            path,
        } => (player, story, path),
        CreateParams::None | CreateParams::RandomMission(_) => {
            (0, ControllerId::BROADCAST, MissionPath::default())
        }
    };
    let path = if kind == ControllerKind::GenericMission {
        path
    } else {
        MissionPath::default()
    };
    // FMissionObject: its heartbeat.
    let mut object = MissionObject::new(id);
    super::set_heartbeat(cx, id, object.heartbeat_interval);
    // FMissionScript: SetPlayer, the story facet, GetOrCreatePlayer.
    object.player = player;
    let state = cx.logic.content.state.players.get(&player);
    object.player_state = state.is_some();
    // In multiplayer the player must be in a system for its record.
    let single_player = api::misc::single_player(cx);
    if let Some(state) = state
        && (single_player || state.system.is_some())
    {
        object.players.insert(
            player,
            MissionPlayer {
                player,
                objectives: Vec::new(),
            },
        );
    }
    let mut script = MissionScript {
        kind,
        player,
        story,
        path,
        object,
        triggers: BTreeMap::new(),
        initially_active: BTreeMap::new(),
        previous_state: MISSION_STATE_NO_STATE,
        mission_state: MISSION_STATE_NO_STATE,
        field_0x2b0: false,
        campaign_slot_current: 0,
        campaign_slot_succeed: 0,
        story_cue: 0,
        field_0x2c0: 0,
        field_0x2c4: 0,
        field_0x2e8: 1,
        dialog_costumes: Vec::new(),
        elapsed_time: 0.0,
        story_fields: StoryMissionFields::default(),
        active_triggers: Vec::new(),
        pending_activations: BTreeMap::new(),
        nn_path: save::NnPath::default(),
        save_trigger_id: 0,
        field_0x2ec: false,
        rtc_scripts: Vec::new(),
        friendly_fire_excused: Vec::new(),
        ambient_scenes: Vec::new(),
        timers: Vec::new(),
        nag_clamp: false,
        field_0x2c8: false,
        rep_pop_up_shown: false,
        rep_pop_up_armed: false,
        rep_pop_up_system: 0,
        rep_pop_up_groups: Vec::new(),
        current_location: 0,
        walks: Vec::new(),
        trace: None,
    };
    let Some(chapter) = chapter(kind) else {
        // GenericMission: the file its parameters name.
        load_mission_file(cx, path.as_bytes(), &mut script);
        return script;
    };
    script.campaign_slot_current = chapter.slot_current;
    script.campaign_slot_succeed = chapter.slot_succeed;
    script.field_0x2c0 = chapter.field_0x2c0;
    script.field_0x2c4 = chapter.field_0x2c4;
    match kind {
        ControllerKind::Mission01a => {
            script.story_fields.mission_01a = Some((-1, -1, 0, 0));
            script.object.heartbeat_interval = object::HEARTBEAT;
            super::set_heartbeat(cx, id, script.object.heartbeat_interval);
            script.mission_state = 3;
        }
        ControllerKind::Mission01b => {
            script.previous_state = 4;
            script.mission_state = 4;
        }
        _ => {}
    }
    load_mission_file(cx, chapter.file, &mut script);
    if kind == ControllerKind::Mission01a {
        let he0101 = base_id_of(cx, b"He01_01_Base");
        let he0102 = base_id_of(cx, b"He01_02_Base");
        script.story_fields.bases = Some((he0101, he0102));
    }
    script
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rep_names() {
        assert_eq!(rep_value_from_name(b"rep_neutral"), 0.0);
        assert_eq!(rep_value_from_name(b"REP_FRIEND_MAXIMUM"), 1.0);
        assert_eq!(rep_value_from_name(b"REP_FOE"), -2.0);
    }

    #[test]
    fn tstrings_are_cut() {
        assert_eq!(tstring(b"abcdef", 4), b"abc");
        assert_eq!(tstring(b"ab\0cd", 8), b"ab");
    }

    #[test]
    fn the_chapters_follow_the_kinds() {
        assert_eq!(chapter(ControllerKind::Mission01a).unwrap().slot_current, 1);
        assert_eq!(
            chapter(ControllerKind::Mission(13)).unwrap().slot_succeed,
            41
        );
        assert_eq!(
            chapter(ControllerKind::Mission(2)).unwrap().file,
            b"Missions\\M02\\m02.ini"
        );
        assert!(chapter(ControllerKind::GenericMission).is_none());
    }
}
