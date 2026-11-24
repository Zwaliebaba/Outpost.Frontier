//! A script mission's part of the player save (TU21 and TU38; work package
//! P3.5b): the `PlayerSave` body of `FMissionScript::HandleMissionMessage`
//! (`Content/MissionScript.cpp:1053-1066`) with `SaveMissionState`
//! (`:1698-1748`) and the conditions' `Save` slots
//! (`Content/MissionCnd.cpp`), and the `PlayerLoad` body (`:1067-1089`) with
//! `LoadMissionState` (`:1753-1932`) and the conditions' `Load` slots; the
//! mission object's sections are [`super::object_save`].
//!
//! A save is, in this order:
//!
//! 1. `[MissionState]`: whether the player accepted the mission, Content's
//!    six clamps, the offer's and title's mad libs, the NN path when it is
//!    active, then either the one `[TriggerSave]` `Act_Save` asked for
//!    (and nothing else), or the unfinished cut scenes, the ambient scenes,
//!    the friendly-fire labels, the story cue and one `[TriggerSave]` per
//!    active trigger with its condition's values;
//! 2. unless a trigger save was pending, the mission object's sections;
//! 3. `[BStoryMissionDone]`.
//!
//! The restore reads `[MissionState]`, `[TriggerSave]` and `[NNObjective]`
//! up to `[BStoryMissionDone]`, then the mission object's sections from the
//! start of the reader. A restored trigger goes to the initially active
//! triggers, which the mission's start activates (P3.5c).
//!
//! **What is left to the run time** (P3.5c). The mission's handler does not
//! deliver `PlayerSave` and `PlayerLoad` yet. When the player has a ship
//! the save first takes the snapshot of the live objects
//! (`SnapshotMissionObjects`, with the NPC ships of P3.6c); the restore's
//! calls out — `JoinMissionAsGroup` with the story's event, the release of
//! started cut scenes, the objectives sent after they are cleared — are
//! returned as [`RestoreStep`]s, in the C++ order, for the run time to make;
//! and the handler's tail (the NN path's, the nags' and the state change's
//! handling of the message) is the run time's.

use std::io::{self, Write};

use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::msvcrt::format_f;
use neuron_core::num::trunc_to_i64;

use super::condition::{Condition, NpcSystemEntry};
use super::object_save;
use super::{MissionScript, f32_at, value_tstring};
use crate::api;
use crate::server::Cx;

/// One cut scene of the mission (`FStoryMissionListNode_0x2f4`):
/// `Act_AddRTC`'s, or a restored `rtc`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RtcScript {
    /// `Path` (at most 0x7f bytes).
    pub path: Vec<u8>,
    /// `Field_0x8c`: the bar scene's handle, -1 for none (run time).
    pub handle: i32,
    /// `Field_0x90`: the script was started (run time).
    pub started: bool,
    /// `Field_0x94`: finished; the save skips it.
    pub finished: bool,
    /// `Field_0x95`: the `rtc` line's second value.
    pub field_0x95: bool,
}

/// One ambient scene (`FMissionAmbientScene`): `Act_AddAmbient`'s, or a
/// restored `ambi_scene`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AmbientScene {
    /// `Path` (`TString<0x80>`).
    pub path: Vec<u8>,
    /// `Handle`, -1 for none (run time).
    pub handle: i32,
    /// `RoomId`.
    pub room_id: u32,
    /// `BaseId`.
    pub base_id: u32,
}

/// The NN path's saved state (`FMissionNnPath`, `MissionObject.h:560-590`).
/// Its objectives, legs and timer are the run time's (P3.5c).
#[derive(Clone, Debug, PartialEq)]
pub struct NnPath {
    /// `Position`.
    pub position: Vector,
    /// `TargetObjectId` (run time).
    pub target_object_id: u32,
    /// `TargetRadius` (run time).
    pub target_radius: f32,
    /// `Label` (`TString<0x30>`).
    pub label: Vec<u8>,
    /// `TravelIds` (1).
    pub travel_ids: i32,
    /// `TargetIds` (1).
    pub target_ids: i32,
    /// `SystemId`.
    pub system_id: u32,
    /// `bActive`.
    pub active: bool,
    /// `State` (0; 1 once started).
    pub state: i32,
}

impl Default for NnPath {
    /// The mission constructor's values.
    fn default() -> Self {
        Self {
            position: Vector::ZERO,
            target_object_id: 0,
            target_radius: 0.0,
            label: Vec::new(),
            travel_ids: 1,
            target_ids: 1,
            system_id: 0,
            active: false,
            state: 0,
        }
    }
}

/// A call out of the restore that the mission's run time makes (P3.5c), in
/// the order the C++ makes it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RestoreStep {
    /// `mission_accepted = true` with a player state:
    /// `JoinMissionAsGroup(ControllerId, 0, empty, false, 0)` for the
    /// player, then the story's `OnMissionEvent(1, CampaignSlotSucceed,
    /// ControllerId)` when the mission has one.
    JoinMission,
    /// `ReleaseRtcScripts` of a started cut scene: its bar scene removed
    /// from the player's base when it has a handle, the script freed.
    ReleaseRtcScript {
        /// The bar scene's handle.
        handle: i32,
    },
    /// `ClearObjectives`: every player record's objectives sent after they
    /// were emptied.
    SendObjectives,
}

fn f(value: f32) -> String {
    format_f(f64::from(value), 6)
}

/// `Cnd_NPCSystemEnter` / `Cnd_NPCSystemExit`'s save: the pending ships,
/// with no line end.
fn write_entries(out: &mut dyn Write, key: &[u8], entries: &[NpcSystemEntry]) -> io::Result<()> {
    out.write_all(key)?;
    out.write_all(b" = ")?;
    for entry in entries {
        out.write_all(&entry.label)?;
        write!(out, ",{}, ", entry.system_id)?;
    }
    Ok(())
}

impl Condition {
    /// The condition's `Save` slot (`Cnd::Save` writes nothing): the run
    /// time values of the twelve conditions that keep any. The two NPC
    /// system conditions end their line without a line end, as the C++
    /// does.
    ///
    /// # Errors
    ///
    /// The writer's.
    pub fn save(&self, out: &mut dyn Write) -> io::Result<()> {
        match self {
            Self::Destroyed(c) => writeln!(out, "Cnd_Destroyed = {}", c.count),
            Self::DistShip(c) => writeln!(
                out,
                "Cnd_DistShip = {}, {}",
                f(c.tick_timer),
                f(c.last_distance_sq)
            ),
            Self::DistVec(c) => writeln!(
                out,
                "Cnd_DistVec = {}, {}",
                f(c.tick_timer),
                f(c.last_distance_sq)
            ),
            Self::DistVecLbl(c) => writeln!(out, "Cnd_DistVecLbl = {}", f(c.tick_timer)),
            Self::LootAcquired(c) => writeln!(out, "Cnd_LootAcquired = {}", c.count),
            Self::NpcSystemEnter(c) => write_entries(out, b"Cnd_NPCSystemEnter", &c.pending),
            Self::NpcSystemExit(c) => write_entries(out, b"Cnd_NPCSystemExit", &c.pending),
            Self::ProjHit(c) => writeln!(out, "Cnd_ProjHit = {}", c.count),
            Self::ProjHitShipToLbl(c) => writeln!(out, "Cnd_ProjHitShipToLbl = {}", c.count),
            Self::TetherBroke(c) => writeln!(
                out,
                "Cnd_TetherBroke = {}, {}",
                f(c.field_0x90),
                f(c.field_0x88)
            ),
            Self::Timer(c) => writeln!(out, "Cnd_Timer = {}", f(c.elapsed)),
            _ => Ok(()),
        }
    }

    /// The condition's `Load` slot (`Cnd::Load` reads nothing): the rest of
    /// the `[TriggerSave]`'s values, the condition's own line taken.
    pub fn load(&mut self, reader: &mut IniReader) {
        match self {
            Self::Destroyed(c) => load_count(reader, b"Cnd_Destroyed", &mut c.count),
            Self::LootAcquired(c) => load_count(reader, b"Cnd_LootAcquired", &mut c.count),
            Self::ProjHit(c) => load_count(reader, b"Cnd_ProjHit", &mut c.count),
            Self::ProjHitShipToLbl(c) => {
                load_count(reader, b"Cnd_ProjHitShipToLbl", &mut c.count);
            }
            Self::DistShip(c) => {
                load_pair(
                    reader,
                    b"Cnd_DistShip",
                    &mut c.tick_timer,
                    &mut c.last_distance_sq,
                );
            }
            Self::DistVec(c) => {
                load_pair(
                    reader,
                    b"Cnd_DistVec",
                    &mut c.tick_timer,
                    &mut c.last_distance_sq,
                );
            }
            Self::TetherBroke(c) => {
                load_pair(
                    reader,
                    b"Cnd_TetherBroke",
                    &mut c.field_0x90,
                    &mut c.field_0x88,
                );
            }
            Self::DistVecLbl(c) => load_float(reader, b"Cnd_DistVecLbl", &mut c.tick_timer),
            Self::Timer(c) => load_float(reader, b"Cnd_Timer", &mut c.elapsed),
            Self::NpcSystemEnter(c) => {
                // Pairs of label and system (`get_value_int`) up to 0x32
                // values; a second system differing from the first clears
                // the same-system flag.
                let mut first = 0u32;
                c.loaded_same_system = true;
                while reader.read_value() {
                    if !reader.is_value(b"Cnd_NPCSystemEnter") {
                        continue;
                    }
                    c.pending.clear();
                    let mut slot = 0u32;
                    if !reader.is_value_empty(0) {
                        while slot < 0x32 {
                            let system = reader.value_int(slot + 1) as u32;
                            c.pending.push(NpcSystemEntry {
                                label: value_tstring(reader, slot, 0x30),
                                system_id: system,
                            });
                            if first & 0x3fff_ffff == 0 {
                                first = system;
                            }
                            if first != system {
                                c.loaded_same_system = false;
                            }
                            slot += 2;
                            if reader.is_value_empty(slot) {
                                break;
                            }
                        }
                    }
                    c.awaited.clone_from(&c.pending);
                }
            }
            Self::NpcSystemExit(c) => {
                // Pairs while the next label is not empty, up to 0x32
                // values; each system through `get_value_float` and the
                // 64-bit truncation, its low word kept.
                while reader.read_value() {
                    if !reader.is_value(b"Cnd_NPCSystemExit") {
                        continue;
                    }
                    c.pending.clear();
                    let mut slot = 0u32;
                    while !value_tstring(reader, slot, 0x40).is_empty() && slot < 0x32 {
                        let system = trunc_to_i64(f64::from(f32_at(reader, slot + 1)));
                        c.pending.push(NpcSystemEntry {
                            label: value_tstring(reader, slot, 0x30),
                            system_id: system as u32,
                        });
                        slot += 2;
                    }
                    c.awaited.clone_from(&c.pending);
                }
            }
            _ => {}
        }
    }
}

fn load_count(reader: &mut IniReader, key: &[u8], count: &mut i32) {
    while reader.read_value() {
        if reader.is_value(key) {
            *count = reader.value_int(0);
        }
    }
}

fn load_float(reader: &mut IniReader, key: &[u8], value: &mut f32) {
    while reader.read_value() {
        if reader.is_value(key) {
            *value = f32_at(reader, 0);
        }
    }
}

fn load_pair(reader: &mut IniReader, key: &[u8], a: &mut f32, b: &mut f32) {
    while reader.read_value() {
        if reader.is_value(key) {
            *a = f32_at(reader, 0);
            *b = f32_at(reader, 1);
        }
    }
}

/// Content's clamps as the missions save and restore them
/// (`GSpacePopAttackClamp`, `GSpacePopEncounterEnabled`, `GMissionScanClamp`,
/// `G_bContentGcsClampDisabled`, `GSpacePopHostileClamp`,
/// `GSpacePopPopulationEnabled`), each the byte the C++ keeps.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Clamps {
    /// `GSpacePopAttackClamp` (0).
    pub attack_clamp: u8,
    /// `GSpacePopEncounterEnabled` (1).
    pub encounter_enabled: u8,
    /// `GMissionScanClamp` (0).
    pub scan_clamp: u8,
    /// `G_bContentGcsClampDisabled` (1): the inverse of the GCS clamp.
    pub gcs_clamp_disabled: u8,
    /// `GSpacePopHostileClamp` (1).
    pub hostile_clamp: u8,
    /// `GSpacePopPopulationEnabled` (1).
    pub population_enabled: u8,
}

impl Default for Clamps {
    /// The values of a freshly loaded Content.dll.
    fn default() -> Self {
        Self {
            attack_clamp: 0,
            encounter_enabled: 1,
            scan_clamp: 0,
            gcs_clamp_disabled: 1,
            hostile_clamp: 1,
            population_enabled: 1,
        }
    }
}

impl Clamps {
    /// `ContentIsGcsClampEnabled`.
    #[must_use]
    pub fn gcs_clamp_enabled(&self) -> bool {
        self.gcs_clamp_disabled == 0
    }

    /// `ContentSetGcsClampEnabled` (`SpacePopSetGcsClampEnabled`).
    pub fn set_gcs_clamp_enabled(&mut self, enabled: u8) {
        self.gcs_clamp_disabled = u8::from(enabled == 0);
    }
}

fn flag(out: &mut dyn Write, key: &str, on: bool) -> io::Result<()> {
    writeln!(out, "{key} = {}", if on { "true" } else { "false" })
}

/// `SaveMissionState(File)` (`MissionScript.cpp:1698-1748`): the values of
/// `[MissionState]`, then the trigger saves. `hostile_clamp = false` ends
/// with a space before the line end, as in the C++. An active trigger
/// without a condition calls `Save` through null in the C++; the port
/// writes its `[TriggerSave]` without values (D832).
///
/// # Errors
///
/// The writer's.
pub fn write_mission_state(
    cx: &Cx<'_>,
    script: &MissionScript,
    out: &mut dyn Write,
) -> io::Result<()> {
    let object = &script.object;
    let live = cx
        .logic
        .content
        .state
        .players
        .get(&object.player)
        .map(|s| s.live_mission);
    writeln!(
        out,
        "mission_accepted = {}",
        i32::from(live == Some(object.controller.0))
    )?;
    let clamps = cx.logic.content.state.clamps;
    flag(out, "att_clamp", clamps.attack_clamp == 1)?;
    flag(out, "tradelane_attacks", clamps.encounter_enabled == 1)?;
    flag(out, "scan_clamp", clamps.scan_clamp == 1)?;
    flag(out, "gcs_clamp", clamps.gcs_clamp_enabled())?;
    out.write_all(if clamps.hostile_clamp == 1 {
        b"hostile_clamp = true\n"
    } else {
        b"hostile_clamp = false \n"
    })?;
    flag(out, "random_pop", clamps.population_enabled != 0)?;
    writeln!(out, "msn_offer = {}", object.offer as i32)?;
    writeln!(out, "msn_title = {}", object.title as i32)?;
    let nn = &script.nn_path;
    if nn.active {
        write!(
            out,
            "nnpath = {}, {}, {}, {}, {}, ",
            nn.travel_ids as u32,
            nn.target_ids as u32,
            f(nn.position.x),
            f(nn.position.y),
            f(nn.position.z)
        )?;
        out.write_all(&nn.label)?;
        writeln!(out, ", {}", nn.system_id)?;
    }
    if object.trigger_save_pending {
        out.write_all(b"\n[TriggerSave]\n")?;
        return writeln!(out, "trigger = {}", script.save_trigger_id);
    }
    for rtc in &script.rtc_scripts {
        if rtc.finished {
            continue;
        }
        out.write_all(b"rtc = ")?;
        out.write_all(&rtc.path)?;
        out.write_all(if rtc.field_0x95 {
            b", true\n"
        } else {
            b", false\n"
        })?;
    }
    for scene in &script.ambient_scenes {
        out.write_all(b"ambi_scene = ")?;
        out.write_all(&scene.path)?;
        writeln!(out, ", {}, {}", scene.room_id, scene.base_id)?;
    }
    for label in &script.friendly_fire_excused {
        out.write_all(b"friendly_fire_excused = ")?;
        out.write_all(label)?;
        out.write_all(b"\n")?;
    }
    writeln!(out, "story_cue = {}", script.story_cue)?;
    for id in &script.active_triggers {
        out.write_all(b"\n[TriggerSave]\n")?;
        writeln!(out, "trigger = {id}")?;
        if let Some(condition) = script.triggers.get(id).and_then(|t| t.condition.as_ref()) {
            condition.save(out)?;
        }
    }
    Ok(())
}

/// The `PlayerSave` body without the message's gates: `[MissionState]`,
/// the mission object's sections unless a trigger save is pending, then
/// `[BStoryMissionDone]`; the pending trigger save is cleared. The
/// snapshot of the live objects that the C++ takes first when the player
/// has a ship is the run time's (P3.5c).
///
/// # Errors
///
/// The writer's.
pub fn write_save(cx: &Cx<'_>, script: &mut MissionScript, out: &mut dyn Write) -> io::Result<()> {
    out.write_all(b"\n[MissionState]\n")?;
    write_mission_state(cx, script, out)?;
    if !script.object.trigger_save_pending {
        object_save::write(cx, out, &mut script.object)?;
    }
    out.write_all(b"\n[BStoryMissionDone]\n")?;
    script.object.trigger_save_pending = false;
    Ok(())
}

/// `PlayerSave {client, file}` to a script mission: nothing without the
/// player's state, for another player or in multiplayer; else
/// [`write_save`].
///
/// # Errors
///
/// The writer's.
pub fn player_save(
    cx: &Cx<'_>,
    script: &mut MissionScript,
    client: u32,
    out: &mut dyn Write,
) -> io::Result<()> {
    let player = script.object.player;
    if !cx.logic.content.state.players.contains_key(&player)
        || client != player
        || !api::misc::single_player(cx)
    {
        return Ok(());
    }
    write_save(cx, script, out)
}

/// `ReleaseRtcScripts`: every cut scene dropped, a started one as a step.
fn release_rtc_scripts(script: &mut MissionScript, steps: &mut Vec<RestoreStep>) {
    for rtc in &script.rtc_scripts {
        if rtc.started {
            steps.push(RestoreStep::ReleaseRtcScript { handle: rtc.handle });
        }
    }
    script.rtc_scripts.clear();
}

/// `FMissionNnPath::Load(Reader, Mission)`: values 0 and 1 the ids, the
/// floats read 4, 3, 2 into z, y, x, value 5 the label when not empty,
/// value 6 the system; the path starts (state 1, active).
fn load_nn_path(reader: &mut IniReader, nn: &mut NnPath) {
    nn.travel_ids = reader.value_int(0);
    nn.target_ids = reader.value_int(1);
    let z = f32_at(reader, 4);
    let y = f32_at(reader, 3);
    let x = f32_at(reader, 2);
    nn.position = Vector::new(x, y, z);
    if !reader.is_value_empty(5) {
        nn.label = value_tstring(reader, 5, 0x30);
    }
    nn.system_id = reader.value_int(6) as u32;
    nn.state = 1;
    nn.active = true;
}

/// One `[MissionState]` value of `LoadMissionState`.
fn load_state_value(
    cx: &mut Cx<'_>,
    reader: &mut IniReader,
    script: &mut MissionScript,
    rtc_released: &mut bool,
    steps: &mut Vec<RestoreStep>,
) {
    if reader.is_value(b"rtc") {
        // The first `rtc` releases the cut scenes; each one is appended
        // not started, its second value as `Field_0x95`.
        if !*rtc_released {
            release_rtc_scripts(script, steps);
        }
        let path = value_tstring(reader, 0, 0x80);
        let field_0x95 = reader.value_bool(1);
        script.rtc_scripts.push(RtcScript {
            path,
            handle: -1,
            started: false,
            finished: false,
            field_0x95,
        });
        *rtc_released = true;
        return;
    }
    if reader.is_value(b"ambi_scene") {
        let path = value_tstring(reader, 0, 0x80);
        let room_id = reader.value_int(1) as u32;
        let base_id = reader.value_int(2) as u32;
        script.ambient_scenes.push(AmbientScene {
            path,
            handle: -1,
            room_id,
            base_id,
        });
        return;
    }
    if reader.is_value(b"friendly_fire_excused") {
        script
            .friendly_fire_excused
            .push(value_tstring(reader, 0, 0x30));
        return;
    }
    if reader.is_value(b"mission_accepted") {
        if reader.value_bool(0)
            && cx
                .logic
                .content
                .state
                .players
                .contains_key(&script.object.player)
        {
            steps.push(RestoreStep::JoinMission);
        }
        return;
    }
    let clamps = &mut cx.logic.content.state.clamps;
    if reader.is_value(b"att_clamp") {
        clamps.attack_clamp = u8::from(reader.value_bool(0));
    } else if reader.is_value(b"tradelane_attacks") {
        clamps.encounter_enabled = u8::from(reader.value_bool(0));
    } else if reader.is_value(b"scan_clamp") {
        clamps.scan_clamp = u8::from(reader.value_bool(0));
    } else if reader.is_value(b"gcs_clamp") {
        let enabled = u8::from(reader.value_bool(0));
        clamps.set_gcs_clamp_enabled(enabled);
    } else if reader.is_value(b"random_pop") {
        clamps.population_enabled = u8::from(reader.value_bool(0));
    } else if reader.is_value(b"hostile_clamp") {
        clamps.hostile_clamp = u8::from(reader.value_bool(0));
    } else if reader.is_value(b"msn_offer") {
        // A non-zero id rebuilds the mad lib with that id.
        if reader.value_int(0) != 0 {
            script.object.offer = reader.value_int(0) as u32;
        }
    } else if reader.is_value(b"msn_title") {
        if reader.value_int(0) != 0 {
            script.object.title = reader.value_int(0) as u32;
        }
    } else if reader.is_value(b"nnpath") {
        // `HideAllObjectives`, then the path.
        for player in script.object.players.values_mut() {
            for objective in &mut player.objectives {
                objective.hidden = true;
            }
        }
        load_nn_path(reader, &mut script.nn_path);
    }
}

/// `LoadMissionState(Reader)` (`MissionScript.cpp:1753-1932`): the headers
/// from the reader's position until `[BStoryMissionDone]`.
///
/// - `[MissionState]` marks the state seen (`Field_0x2ec`) and reads its
///   values: the cut scenes (the first `rtc` releases the old ones), the
///   ambient scenes, the friendly-fire labels, the clamps, the mad libs of
///   non-zero ids and the NN path (which hides every objective first);
///   `mission_accepted = true` is a [`RestoreStep::JoinMission`].
/// - `[TriggerSave]`: the first `trigger` drops the active triggers, the
///   initially active ones and the pending activations; each `trigger`
///   becomes initially active (the first entry of an id stays) and its
///   trigger's condition reads the rest of the section. The C++ calls
///   `Load` through null for a trigger without a condition; the port
///   reads nothing for it (D832).
/// - `[NNObjective]`, once `[MissionState]` was seen: the first one clears
///   every player's objectives (a [`RestoreStep::SendObjectives`]); each
///   goes to the first player's objectives.
pub fn load_mission_state(
    cx: &mut Cx<'_>,
    reader: &mut IniReader,
    script: &mut MissionScript,
) -> Vec<RestoreStep> {
    let mut steps = Vec::new();
    let mut state_seen = false;
    let mut rtc_released = false;
    let mut triggers_dropped = false;
    let mut objectives_cleared = false;
    while reader.read_header() {
        if reader.is_header(b"MissionState") {
            script.field_0x2ec = true;
            state_seen = true;
            while reader.read_value() {
                load_state_value(cx, reader, script, &mut rtc_released, &mut steps);
            }
            continue;
        }
        if reader.is_header(b"TriggerSave") {
            while reader.read_value() {
                if !reader.is_value(b"trigger") {
                    continue;
                }
                if !triggers_dropped {
                    script.active_triggers.clear();
                    script.initially_active.clear();
                    script.pending_activations.clear();
                }
                let id = reader.value_int(0) as u32;
                script.initially_active.entry(id).or_insert(id);
                if let Some(condition) = script
                    .triggers
                    .get_mut(&id)
                    .and_then(|t| t.condition.as_mut())
                {
                    condition.load(reader);
                }
                triggers_dropped = true;
            }
            continue;
        }
        if reader.is_header(b"NNObjective") && state_seen {
            if !objectives_cleared {
                for player in script.object.players.values_mut() {
                    player.objectives.clear();
                }
                steps.push(RestoreStep::SendObjectives);
                objectives_cleared = true;
            }
            object_save::load_objective(cx, reader, &mut script.object);
            continue;
        }
        if reader.is_header(b"BStoryMissionDone") {
            break;
        }
    }
    steps
}

/// The `PlayerLoad` body without the message's gates: the reader's state
/// kept around [`load_mission_state`] from its start, then around the
/// mission object's sections ([`object_save::load`]).
pub fn restore(
    cx: &mut Cx<'_>,
    reader: &mut IniReader,
    script: &mut MissionScript,
) -> Vec<RestoreStep> {
    let saved = reader.state();
    reader.reset();
    let steps = load_mission_state(cx, reader, script);
    reader.set_state(&saved);
    let saved = reader.state();
    object_save::load(cx, reader, &mut script.object);
    reader.set_state(&saved);
    steps
}

/// `PlayerLoad {player, reader}` to a script mission: nothing without the
/// player's state or for another player; else [`restore`]. (The C++'s
/// branch for a missing state after its first test cannot be taken.)
pub fn player_load(
    cx: &mut Cx<'_>,
    script: &mut MissionScript,
    player: u32,
    reader: &mut IniReader,
) -> Vec<RestoreStep> {
    if !cx
        .logic
        .content
        .state
        .players
        .contains_key(&script.object.player)
        || player != script.object.player
    {
        return Vec::new();
    }
    restore(cx, reader, script)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::content::mission::condition::{NpcSystemExit, Timer};

    fn reader(text: &str) -> IniReader {
        let mut reader = IniReader::new();
        reader.open_memory(text.as_bytes());
        assert!(reader.read_header());
        assert!(reader.read_value());
        reader
    }

    #[test]
    fn a_timer_saves_and_loads_its_elapsed_time() {
        let mut out = Vec::new();
        let c = Condition::Timer(Timer {
            elapsed: 0.023_437_5,
            ..Timer::default()
        });
        c.save(&mut out).unwrap();
        assert_eq!(out, b"Cnd_Timer = 0.023438\n");
        let mut r = reader("[TriggerSave]\ntrigger = 1\nCnd_Timer = 0.023438\n");
        let mut loaded = Condition::Timer(Timer::default());
        loaded.load(&mut r);
        let Condition::Timer(t) = loaded else {
            panic!()
        };
        assert_eq!(t.elapsed, 0.023_438);
    }

    #[test]
    fn the_npc_system_exit_reads_its_systems_through_a_float() {
        let mut r =
            reader("[TriggerSave]\ntrigger = 1\nCnd_NPCSystemExit = a,16777217, b,4000000007, \n");
        let mut c = Condition::NpcSystemExit(NpcSystemExit::default());
        c.load(&mut r);
        let Condition::NpcSystemExit(c) = c else {
            panic!()
        };
        let systems: Vec<u32> = c.pending.iter().map(|e| e.system_id).collect();
        assert_eq!(systems, [16_777_216, 4_000_000_000]);
        assert_eq!(c.awaited, c.pending);
    }
}
