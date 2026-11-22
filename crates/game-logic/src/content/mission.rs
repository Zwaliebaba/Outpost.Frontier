//! The script missions: `Mission_01a` to `Mission_13` and `GenericMission`
//! (`Content/StoryMissions.cpp`, the mission object of
//! `Content/MissionObject.cpp` and the script of `Content/MissionScript.cpp`;
//! record 0008 points 2 and 6).
//!
//! A mission's constructor sets its heartbeat twice (1/30 s,
//! `MissionObject.cpp:101` and `StoryMissions.cpp:100`), then parses its
//! script, which can draw `rand()` (`Cnd_Timer`) and calls `pub::` getters.
//! The constructors, the parser and the run time are P3.5's; until then a
//! mission instance holds its create parameters only.

use neuron_server::controller::ControllerId;

use super::Instance;
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

/// A script mission's own state (`FMissionScript` and its story-mission
/// forms; P3.5 adds the triggers, the mission object and the overrides).
#[derive(Clone, Debug, PartialEq, Eq)]
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
}

/// The factory's mission constructors: the instance with what its create
/// parameters carry; a form without a field reads it as 0 or empty (D770).
pub fn construct(
    _cx: &mut Cx<'_>,
    _id: ControllerId,
    kind: ControllerKind,
    params: &CreateParams,
) -> Instance {
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
    Instance::Mission(MissionScript {
        kind,
        player,
        story,
        path,
    })
}
