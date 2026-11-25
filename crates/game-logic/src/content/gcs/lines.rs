//! The conversation lines of TU25 (`FGcsLine` and its 61 classes,
//! `Content/Gcs.cpp:415-548`, `:717-2172`).
//!
//! Every line class shares the base's five slots (`CanPlay`, `Play`,
//! `IsDone`, `GetDuration`); the classes differ only in their constructor,
//! which builds the texts. A line is therefore one [`Line`] value, and each
//! class is a constructor function here, named after it. A constructor
//! runs the C++'s steps in its order, with every `rand()` it draws: the
//! voice test (the speaker's voice and its roles, [`Line::has_voice_roles`]),
//! the `rand() / 32768` test that is always 0, the mood (`+0x28`) and the
//! texts, each voice line through `PickVoiceLine` ([`voice_line`]).
//!
//! The ships a constructor looks up (`System->FindShip`) are not
//! null-tested; a ship that is gone gives no names (D935).

// The x87 comparisons keep their NaN outcome as written (`!(a > b)`).
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use super::names::{
    dock_approach, flee_reason, formation_voice_line, good_prefix, guide_targets,
    has_formation_wingmen, location_name, number_names, reference_name, ship_prefix, short_faction,
    system_prefix, t64, voice_line,
};
use super::{find_ship, find_ship_or_solar};
use crate::api;
use crate::api::space_obj::CommRequest;
use crate::content::ships::ShipKey;
use crate::server::Cx;

/// `FGcsLine` (0x30 bytes): one line of a conversation.
#[derive(Clone, Debug, PartialEq)]
pub struct Line {
    /// `Speaker` (`+0x04`).
    pub speaker: u32,
    /// `Listener` (`+0x08`).
    pub listener: u32,
    /// `VoiceId` (`+0x0c`): 0 for the speaker's.
    pub voice: u32,
    /// `CostumeId` (`+0x10`): 0 for none (TU25's lines set none).
    pub costume: i32,
    /// `TextIds` (`+0x18`), nickname ids.
    pub texts: Vec<u32>,
    /// `System` (`+0x24`), by its record's id; `None` makes the line
    /// unplayable.
    pub system: Option<u32>,
    /// `+0x28`: `SendComm`'s mood; 0 makes the line unplayable.
    pub mood: u32,
    /// `+0x2c`: `SendComm`'s comm type.
    pub comm_type: u32,
}

impl Line {
    /// The base part of every constructor: no voice, costume, mood, comm
    /// type or texts.
    #[must_use]
    pub fn new(system: Option<u32>, speaker: u32, listener: u32) -> Self {
        Self {
            speaker,
            listener,
            voice: 0,
            costume: 0,
            texts: Vec::new(),
            system,
            mood: 0,
            comm_type: 0,
        }
    }

    /// `HasVoiceRoles(Roles)`: the speaker's voice (`GetVoiceID` into
    /// `+0x0c`); without its voice properties or every bit of `roles` the
    /// line loses its system.
    pub fn has_voice_roles(&mut self, cx: &mut Cx<'_>, roles: u32) -> bool {
        if let Ok(voice) = api::space_obj::voice_id(cx, self.speaker) {
            self.voice = voice;
        }
        let tables = &cx.logic.content.tables;
        let ok = crate::content::tables::voices::find_voice_properties(
            &tables.voice_properties,
            self.voice,
        )
        .is_some_and(|p| p.supports_roles(roles));
        if !ok {
            self.system = None;
        }
        ok
    }

    /// `PushText(Name)` and `AddText(Name)`: the name's nickname id.
    pub fn push(&mut self, name: &[u8]) {
        self.texts.push(api::misc::nickname_id(name));
    }

    /// The voice line of `name` with `suffix` in the line's voice.
    fn voiced(&mut self, cx: &mut Cx<'_>, name: &[u8], suffix: &[u8]) {
        let line = voice_line(cx, name, suffix, self.voice);
        self.push(&line);
    }

    /// The voice line of the formation form of `name` (`_p` / `_s`).
    fn voiced_formation(
        &mut self,
        cx: &mut Cx<'_>,
        ship: Option<ShipKey>,
        name: &[u8],
        suffix: &[u8],
    ) {
        let line = formation_voice_line(cx, ship, name, suffix, self.voice);
        self.push(&line);
    }

    /// The line's ship of `id` in its system.
    fn ship(&self, cx: &Cx<'_>, id: u32) -> Option<ShipKey> {
        find_ship(cx, self.system, id)
    }
}

/// `rand() / 32768 != 0`, the test every constructor but a few makes: the
/// CRT's 15-bit `rand()` makes it 0, so it only draws.
fn rand_skips(cx: &mut Cx<'_>) -> bool {
    cx.world.rng().rand() / 32768 != 0
}

/// `(rand() * 2) / 32768`: 0 or 1.
pub fn rand_pick2(cx: &mut Cx<'_>) -> i32 {
    cx.world.rng().rand() * 2 / 32768
}

/// `BuildSingleVoiceLine(Roles, Name)` (`:535-548`), the body of the
/// one-name lines: the voice test, the `rand()` test, mood `0x4a39` and the
/// voice line of `name` with a dash.
fn single(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    roles: u32,
    name: &[u8],
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, roles) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a39;
    line.voiced(cx, name, b"-");
    line
}

/// `FGcsLine::CanPlay` (`:431-436`): a mood, a system, the speaker found
/// in it (`FindShipOrSolar`) and at least one text.
#[must_use]
pub fn can_play(cx: &Cx<'_>, line: &Line) -> bool {
    line.mood != 0
        && line.system.is_some()
        && find_ship_or_solar(cx, line.system, line.speaker).is_some()
        && !line.texts.is_empty()
}

/// `FGcsLine::Play` (`:438-481`): the comm (`SendSpaceComm` with the
/// line's voice, comm type, costume and mood, 0.5 s, interrupting);
/// outside single player the manager's delayed "line done" message after
/// the line's duration and 1.25 s.
pub fn play(cx: &mut Cx<'_>, line: &Line) {
    let costume = (line.costume != 0).then(|| api::gf::costume_by_id(cx, line.costume));
    let request = CommRequest {
        sender: line.speaker,
        target: line.listener,
        voice: line.voice,
        costume,
        comm_type: line.comm_type,
        text_ids: line.texts.clone(),
        mood: line.mood,
        duration: 0.5,
        interrupt: true,
    };
    let _ = api::space_obj::send_comm(cx, &request);
    if api::misc::single_player(cx) {
        return;
    }
    let delay = (f64::from(duration(cx, line)) + 1.25) as f32;
    let entry = super::DelayedMessage {
        system: line.system.unwrap_or(0),
        speaker: line.speaker,
        listener: line.listener,
        text: line.texts.last().copied().unwrap_or(0),
        elapsed: 0.0,
        delay,
    };
    cx.logic.content.state.gcs.delayed.push(entry);
}

/// `FGcsLine::IsDone(Parms)` (`:483-489`): the line-done message is this
/// line's (its listener, system, speaker and last text).
#[must_use]
pub fn is_done(line: &Line, done: &super::LineDone) -> bool {
    line.system == Some(done.system)
        && line.listener == done.listener
        && line.speaker == done.speaker
        && line.texts.last().copied() == Some(done.text)
}

/// `FGcsLine::GetDuration` (`:491-508`): the summed durations of the texts
/// in the line's voice (the speaker's when it has none), 5 when one is
/// unknown.
#[must_use]
pub fn duration(cx: &Cx<'_>, line: &Line) -> f32 {
    let mut voice = line.voice;
    if voice == 0 {
        voice = api::space_obj::voice_id(cx, line.speaker).unwrap_or(0);
    }
    let table = &cx.logic.content.tables.voice_lines;
    let mut sum = 0.0_f32;
    for &text in &line.texts {
        let d = crate::content::tables::voices::voice_line_duration(table, voice, text);
        if d == -1.0 {
            return 5.0;
        }
        sum = (f64::from(d) + f64::from(sum)) as f32;
    }
    sum
}

// ---- The dock and launch lines -------------------------------------------------------------

/// `FGcsLineLaunchClearance` (`:717-770`): the dock (role `0x40`) clears
/// the launching ship: its faction, designation, number, dash, number, "this
/// is", the dock's place, the clearance for the terminal type (1 berth, 3
/// to 5 moor, 6 ring) and the well-wish.
pub fn launch_clearance(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    terminal_type: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x40) {
        return line;
    }
    let ship = line.ship(cx, listener);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a41;
    let faction = short_faction(cx, ship);
    line.push(&faction);
    for kind in 1..=2 {
        let name = reference_name(cx, kind, ship, None);
        line.push(&name);
    }
    line.push(b"gcs_misc_dash");
    let name = reference_name(cx, 3, ship, None);
    line.push(&name);
    line.push(b"gcs_misc_thisis+");
    let place = location_name(cx, speaker);
    line.push(&place);
    let clearance: &[u8] = match terminal_type {
        1 => b"gcs_docklaunch_clear_berth",
        3..=5 => b"gcs_docklaunch_clear_moor",
        6 => b"gcs_docklaunch_clear_ring",
        _ => b"",
    };
    line.voiced(cx, clearance, b"-");
    line.voiced(cx, b"gcs_misc_wellwish", b"-");
    line
}

/// `FGcsLineAck` (`:772-785`): `gcs_misc_ack` (role `0x200`).
pub fn ack(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x200) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a39;
    line.voiced(cx, b"gcs_misc_ack", b"-");
    line
}

/// `FGcsLineDockRequest` (`:787-816`): the ship (role `0x80`) asks the
/// dock: "this is", its faction, designation, number, dash, number, "to",
/// the dock's place, the request and the approach.
pub fn dock_request(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    dock_type: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x80) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a42;
    line.push(b"gcs_misc_thisis+");
    let faction = short_faction(cx, ship);
    line.push(&faction);
    for kind in [1, 2] {
        let name = reference_name(cx, kind, ship, None);
        line.push(&name);
    }
    line.push(b"gcs_misc_dash");
    // Kind 2 again, not 3.
    let name = reference_name(cx, 2, ship, None);
    line.push(&name);
    line.push(b"gcs_misc_to");
    let place = location_name(cx, listener);
    line.push(&place);
    line.voiced(cx, b"gcs_dockrequest_request", b"+");
    line.push(&dock_approach(dock_type, b"-"));
    line
}

/// A one-voice-line dock answer (role `0x40`, the `rand()` test, mood
/// `0x4a39`).
fn dock_one(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    name: &[u8],
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x40) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a39;
    line.voiced(cx, name, b"-");
    line
}

/// `FGcsLineDockHostile` (`:818-831`).
pub fn dock_hostile(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    dock_one(
        cx,
        system,
        speaker,
        listener,
        b"gcs_dockrequest_responsehostile",
    )
}

/// `FGcsLineDockGranted` (`:833-888`): the dock (role `0x40`) grants the
/// request, without the `rand()` test: landing (dock type 6), a terminal up
/// to 20 (its number at the end) or above (its number digit by digit).
pub fn dock_granted(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    dock_type: u32,
    index: i32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x40) {
        return line;
    }
    if dock_type == 6 {
        line.mood = 0x4a3e;
        line.voiced(cx, b"gcs_misc_ack", b"-");
        line.push(b"gcs_dockrequest_yourrequest+");
        line.push(b"gcs_dockrequest_toland");
        line.voiced(cx, b"gcs_dockrequest_granted", b"-");
        line.voiced(cx, b"gcs_dockrequest_proceed", b"+");
        line.push(b"gcs_dockrequest_toland-");
        return line;
    }
    if index <= 0x14 {
        line.mood = 0x4a3f;
        line.voiced(cx, b"gcs_misc_ack", b"-");
        line.push(b"gcs_dockrequest_yourrequest+");
        line.push(&dock_approach(dock_type, b""));
        line.voiced(cx, b"gcs_dockrequest_granted", b"-");
        line.voiced(cx, b"gcs_dockrequest_proceed", b"+");
        line.push(&dock_approach(dock_type, b"_number"));
        let number = t64(format!("gcs_misc_number_{}-", index.wrapping_add(1)).into_bytes());
        line.push(&number);
        return line;
    }
    line.mood = 0x4a40;
    line.voiced(cx, b"gcs_misc_ack", b"-");
    line.push(b"gcs_dockrequest_yourrequest+");
    line.push(&dock_approach(dock_type, b""));
    line.voiced(cx, b"gcs_dockrequest_granted", b"-");
    line.voiced(cx, b"gcs_dockrequest_proceed", b"+");
    line.push(&dock_approach(dock_type, b"_number"));
    // The first two names are read without a size test; a terminal above
    // 20 has two digits or more.
    let digits = number_names(index.wrapping_add(1), b"-");
    for name in digits.iter().take(2) {
        line.push(name);
    }
    line
}

/// `FGcsLineDockStandby` (`:890-905`): "stand by" (response 3).
pub fn dock_standby(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x40) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3c;
    for name in [
        &b"gcs_misc_ack"[..],
        b"gcs_dockrequest_standby",
        b"gcs_dockrequest_delayedreason",
        b"gcs_dockrequest_willbecleared",
    ] {
        line.voiced(cx, name, b"-");
    }
    line
}

/// `FGcsLineDockDenied` (`:907-923`): "denied, no fit" (response 2).
pub fn dock_denied(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    dock_type: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x40) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3d;
    line.voiced(cx, b"gcs_misc_ack", b"-");
    line.push(b"gcs_dockrequest_yourrequest+");
    line.push(&dock_approach(dock_type, b""));
    line.voiced(cx, b"gcs_dockrequest_denied", b"-");
    line.voiced(cx, b"gcs_dockrequest_nofit", b"-");
    line
}

/// `FGcsLineDockNowCleared` (`:925-944`): the dock calls a ship that
/// waited: its faction, designation, number, dash, number, "now cleared"
/// and the approach.
pub fn dock_now_cleared(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    dock_type: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x40) {
        return line;
    }
    let ship = line.ship(cx, listener);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3f;
    let faction = short_faction(cx, ship);
    line.push(&faction);
    for kind in [1, 2] {
        let name = reference_name(cx, kind, ship, None);
        line.push(&name);
    }
    line.push(b"gcs_misc_dash");
    let name = reference_name(cx, 3, ship, None);
    line.push(&name);
    line.voiced(cx, b"gcs_dockrequest_nowcleared", b"+");
    line.push(&dock_approach(dock_type, b"-"));
    line
}

/// `FGcsLineJumpGateOpen` (`:946-960`): a formation leader at a gate or a
/// hole (role `0x80`): "jump gate open", "going to", the target system.
pub fn jump_gate_open(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    target_system: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x80) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3b;
    line.voiced(cx, b"gcs_dockrequest_jumpgateopen", b"-");
    line.voiced(cx, b"gcs_dockrequest_goingto", b"+");
    let prefix = system_prefix(cx, target_system);
    line.push(&prefix);
    line
}

/// `FGcsLineJumpGateDelayed` (`:962-976`): "delayed", the target system.
pub fn jump_gate_delayed(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    target_system: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x80) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3a;
    line.voiced(cx, b"gcs_dockrequest_jumpgatedelayed", b"-");
    let prefix = system_prefix(cx, target_system);
    line.push(&prefix);
    line
}

/// `FGcsLineJumpGateOpenAfterDelay` (`:978-986`).
pub fn jump_gate_open_after_delay(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x80,
        b"gcs_dockrequest_jumpgateopenafterdelay",
    )
}

/// `FGcsLineTradeLaneStart` (`:988-995`).
pub fn trade_lane_start(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x80,
        b"gcs_dockrequest_tradelanestart",
    )
}

/// `FGcsLineThisOurStop` (`:997-1010`): "this is our stop", "thanks for
/// the help".
pub fn this_our_stop(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x80) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3a;
    line.voiced(cx, b"gcs_dockrequest_thisourstop", b"-");
    line.voiced(cx, b"gcs_dockrequest_thankshelp", b"-");
    line
}

// ---- The dock hail lines -------------------------------------------------------------------

/// The ship's archetype prefix of the hails.
fn hailed_ship_prefix(cx: &Cx<'_>, ship: Option<ShipKey>) -> Vec<u8> {
    let archetype = ship
        .and_then(|k| crate::content::ships::base(&cx.logic.content.state, k))
        .map_or(0, |s| s.archetype);
    ship_prefix(cx, archetype)
}

/// `FGcsLineDockHail` (`:1012-1074`): the station (role `0x20`) hails the
/// ship. An attitude at or below -0.6 (or NaN) refuses without the
/// `rand()` test; otherwise one of two identification requests by
/// `(rand() * 2) / 32768`.
pub fn dock_hail(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    attitude: f32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x20) {
        return line;
    }
    let ship = line.ship(cx, listener);
    if !(attitude > -0.6) {
        line.mood = 0x4a3c;
        line.push(b"gcs_dockhail_dock_incoming+");
        line.push(&hailed_ship_prefix(cx, ship));
        let business = reference_name(cx, 0, ship, None);
        line.push(&business);
        line.voiced(cx, b"gcs_dhail_nopermission", b"-");
        return line;
    }
    match rand_pick2(cx) {
        1 => {
            line.mood = 0x4a3f;
            line.push(b"gcs_dockhail_dock_incoming+");
            line.push(&hailed_ship_prefix(cx, ship));
            let business = reference_name(cx, 0, ship, None);
            line.push(&business);
            line.push(b"gcs_misc_thisis+");
            let place = location_name(cx, speaker);
            line.push(&place);
            line.voiced(cx, b"gcs_dockhail_dock_hailmethod", b"-");
            line.voiced(cx, b"gcs_dockhail_dock_idreq", b"-");
        }
        0 => {
            line.mood = 0x4a3e;
            line.push(b"gcs_misc_thisis+");
            let place = location_name(cx, speaker);
            line.push(&place);
            line.push(b"gcs_misc_to");
            line.push(&hailed_ship_prefix(cx, ship));
            let business = reference_name(cx, 0, ship, None);
            line.push(&business);
            line.voiced(cx, b"gcs_dockhail_dock_idreq", b"-");
        }
        _ => {}
    }
    line
}

/// `FGcsLineDockHailReceived` (`:1076-1091`): the station acknowledges and
/// asks where the ship is headed (no `rand()` test).
pub fn dock_hail_received(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x20) {
        return line;
    }
    let ship = line.ship(cx, listener);
    line.mood = 0x4a3d;
    line.voiced(cx, b"gcs_dockhail_dock_transreceived", b"+");
    let faction = short_faction(cx, ship);
    line.push(&faction);
    let name = reference_name(cx, 1, ship, None);
    line.push(&name);
    let name = reference_name(cx, 2, ship, Some(b"-"));
    line.push(&name);
    line.voiced(cx, b"gcs_dockhail_dock_whereheaded", b"+");
    line
}

/// "This is" the speaker's faction, designation and number (with a dash),
/// the shape of the hailee's data and the player hail's identification.
fn identify(cx: &mut Cx<'_>, line: &mut Line, ship: Option<ShipKey>) {
    line.push(b"gcs_misc_thisis+");
    let faction = short_faction(cx, ship);
    line.push(&faction);
    let name = reference_name(cx, 1, ship, None);
    line.push(&name);
    let name = reference_name(cx, 2, ship, Some(b"-"));
    line.push(&name);
}

/// `FGcsLineHaileeDataSubmit` (`:1093-1112`): the ship (role `0x10`)
/// identifies itself and submits its data.
pub fn hailee_data_submit(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x10) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3d;
    identify(cx, &mut line, ship);
    line.voiced(cx, b"gcs_dockhail_hailee_datasubmit", b"-");
    line
}

/// `FGcsLineHaileeAngry` (`:1114-1121`).
pub fn hailee_angry(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x10,
        b"gcs_dhail_angry_response",
    )
}

/// A guided line of the hailee (role `0x10`, its ship, the `rand()` test,
/// mood `0x4a3a`): the formation voice line of `name`, then the place of
/// the guide's first or second target.
fn guided(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    name: &[u8],
    second: bool,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x10) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3a;
    line.voiced_formation(cx, ship, name, b"+");
    let targets = guide_targets(cx, ship);
    let place = location_name(cx, if second { targets.id2 } else { targets.id1 });
    line.push(&place);
    line
}

/// `FGcsLineGoingTo` (`:1123-1143`): where the guide goes (its second
/// target).
pub fn going_to(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    guided(cx, system, speaker, listener, b"gcs_misc_goingto", true)
}

/// `FGcsLinePatrolIdHome` (`:1145-1165`): the patrol's home (the guide's
/// first target).
pub fn patrol_id_home(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    guided(cx, system, speaker, listener, b"gcs_patrol_idhome", false)
}

/// The patrol's progress of `FGcsLinePatrolStatus` and
/// `FGcsLineFidgetPatrolStatus`: 0 points to go is "finished", any other
/// count "(i|we) have (only)", the number and "more waypoint(s) to go".
fn patrol_progress(
    cx: &mut Cx<'_>,
    line: &mut Line,
    ship: Option<ShipKey>,
    points: i32,
    fidget: bool,
) {
    if points == 0 {
        line.mood = 0x4a39;
        line.voiced_formation(cx, ship, b"gcs_patrol_finished", b"-");
        return;
    }
    if fidget && rand_skips(cx) {
        return;
    }
    line.mood = 0x4a3b;
    let we = fidget || has_formation_wingmen(cx, ship);
    let have: &[u8] = match (we, points == 1) {
        (true, true) => b"gcs_patrol_wehaveonly+",
        (true, false) => b"gcs_patrol_wehave+",
        (false, true) => b"gcs_patrol_ihaveonly+",
        (false, false) => b"gcs_patrol_ihave+",
    };
    line.push(have);
    line.push(&t64(format!("gcs_misc_number_{points}").into_bytes()));
    line.push(if points == 1 {
        b"gcs_patrol_morewptogo-"
    } else {
        b"gcs_patrol_morewpstogo-"
    });
}

/// `FGcsLinePatrolStatus` (`:1167-1208`): the patrol guide's points to go
/// (slot `+0x4c`), without a `rand()` test.
pub fn patrol_status(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x10) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    let points = super::path_points_to_go(cx, ship);
    patrol_progress(cx, &mut line, ship, points, false);
    line
}

/// `FGcsLineTradeStatus` (`:1210-1263`): what the trader carries (its
/// trade good, "to") or wants (its return good, "from"; none loses the
/// system), then the place of the guide's second target.
pub fn trade_status(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x10) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3c;
    let (trade, ret) = super::trade_goods(cx, ship);
    let preposition: &[u8] = if trade != 0 {
        line.voiced_formation(cx, ship, b"gcs_trade_carrying", b"+");
        line.push(&good_prefix(cx, trade));
        b"gcs_misc_to"
    } else {
        if ret == 0 {
            line.system = None;
            return line;
        }
        line.voiced_formation(cx, ship, b"gcs_trade_wanttoget", b"+");
        line.push(&good_prefix(cx, ret));
        b"gcs_misc_from"
    };
    line.push(preposition);
    let targets = guide_targets(cx, ship);
    let place = location_name(cx, targets.id2);
    line.push(&place);
    line
}

/// `FGcsLineTradeIdHome` (`:1265-1285`): the trader's home (the guide's
/// first target).
pub fn trade_id_home(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    guided(cx, system, speaker, listener, b"gcs_trade_idhome", false)
}

// ---- The player hail lines -----------------------------------------------------------------

/// `FGcsLinePlayerHailBlowOff` (`:1287-1303`): the same player hailed
/// again within 15 s.
pub fn player_hail_blow_off(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(cx, system, speaker, listener, 0x10, b"gcs_phail_blowoff")
}

/// `FGcsLinePlayerHailCombatBlowOff` (`:1305-1319`): the hailed ship is
/// busy.
pub fn player_hail_combat_blow_off(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x10,
        b"gcs_phail_combatblowoff",
    )
}

/// `FGcsLinePlayerHailIdentify` (`:1321-1346`): the hailed ship names
/// itself.
pub fn player_hail_identify(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x10) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3c;
    identify(cx, &mut line, ship);
    line
}

// ---- The combat lines ----------------------------------------------------------------------

/// `HasCombatVoiceRoles` (`:1358-1371`): role `0x4000` for a ship (type
/// `0x7f0000`), `0x800` for anything else; a failed `GetType` reads 0.
fn has_combat_voice_roles(cx: &mut Cx<'_>, line: &mut Line) -> bool {
    if let Ok(voice) = api::space_obj::voice_id(cx, line.speaker) {
        line.voice = voice;
    }
    let object_type = api::space_obj::object_type(cx, line.speaker).unwrap_or(0);
    let roles = if object_type & 0x7f_0000 != 0 {
        0x4000
    } else {
        0x800
    };
    let tables = &cx.logic.content.tables;
    let ok =
        crate::content::tables::voices::find_voice_properties(&tables.voice_properties, line.voice)
            .is_some_and(|p| p.supports_roles(roles));
    if !ok {
        line.system = None;
    }
    ok
}

/// `FGcsLineCombatDeathSympathy` (`:1374-1383`): the listener is not kept.
pub fn combat_death_sympathy(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32) -> Line {
    single(cx, system, speaker, 0, 0x4000, b"gcs_combat_deathsympathy")
}

/// "This is" the ship's designation, number, dash and number.
fn designation(cx: &mut Cx<'_>, line: &mut Line, ship: Option<ShipKey>) {
    line.push(b"gcs_misc_thisis+");
    for kind in [1, 2] {
        let name = reference_name(cx, kind, ship, None);
        line.push(&name);
    }
    line.push(b"gcs_misc_dash");
    let name = reference_name(cx, 3, ship, None);
    line.push(&name);
}

/// `FGcsLineCombatTakingLead` (`:1385-1408`): the listener is not kept.
pub fn combat_taking_lead(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32) -> Line {
    let mut line = Line::new(system, speaker, 0);
    if !line.has_voice_roles(cx, 0x4) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3e;
    designation(cx, &mut line, ship);
    line.voiced(cx, b"gcs_combat_takinglead", b"-");
    line
}

/// `FGcsLineCombatScream` (`:1410-1423`): the role by the speaker's type.
pub fn combat_scream(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !has_combat_voice_roles(cx, &mut line) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a39;
    line.voiced(cx, b"gcs_combat_scream", b"-");
    line
}

/// `FGcsLineCombatBrag` (`:1425-1431`).
pub fn combat_brag(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    single(cx, system, speaker, listener, 0x4000, b"gcs_combat_brag")
}

/// `FGcsLineCombatTakingDamage` (`:1433-1451`): `_worst` below 0.49 of the
/// health; the role by the speaker's type.
pub fn combat_taking_damage(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    health: f32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !has_combat_voice_roles(cx, &mut line) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a39;
    let name: &[u8] = if f64::from(health) < 0.49 {
        b"gcs_combat_taking_damage_worst"
    } else {
        b"gcs_combat_taking_damage"
    };
    line.voiced(cx, name, b"-");
    line
}

/// `FGcsLineCombatInflictingDamage` (`:1453-1472`): `_worst` below 0.33 of
/// the victim's health.
pub fn combat_inflicting_damage(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    health: f32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x4000) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a39;
    let name: &[u8] = if f64::from(health) < 0.33 {
        b"gcs_combat_inflicting_damage_worst"
    } else {
        b"gcs_combat_inflicting_damage"
    };
    line.voiced(cx, name, b"-");
    line
}

/// `FGcsLineCombatInSights` (`:1474-1480`).
pub fn combat_in_sights(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x4000,
        b"gcs_combat_insights",
    )
}

/// `FGcsLineCombatHasInSights` (`:1482-1489`).
pub fn combat_has_in_sights(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x4000,
        b"gcs_combat_hasinsights",
    )
}

/// `FGcsLineCombatRejoiningForm` (`:1491-1498`).
pub fn combat_rejoining_form(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x4000,
        b"gcs_combat_rejoiningform",
    )
}

/// `FGcsLineCombatOrderDisengage` (`:1500-1517`): "disengage", "all
/// clear".
pub fn combat_order_disengage(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x4000) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3a;
    line.voiced(cx, b"gcs_combat_order_disengage", b"-");
    line.voiced(cx, b"gcs_combat_announce_allclear", b"-");
    line
}

/// `FGcsLineCombatAllClear` (`:1519-1525`).
pub fn combat_all_clear(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x4000,
        b"gcs_combat_announce_allclear",
    )
}

/// `FGcsLineCombatResumeTrade` (`:1527-1553`): role `0xc004`; "resume
/// trade", "to", the place of the guide's first target.
pub fn combat_resume_trade(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0xc004) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3b;
    line.voiced(cx, b"gcs_combat_resume_trade", b"+");
    line.push(b"gcs_misc_to");
    let targets = guide_targets(cx, ship);
    let place = location_name(cx, targets.id1);
    line.push(&place);
    line
}

/// `FGcsLineCombatResumePatrol` (`:1555-1562`).
pub fn combat_resume_patrol(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x4,
        b"gcs_combat_resume_patrol",
    )
}

/// `FGcsLineCombatEngaging` (`:1564-1609`): role `0x4`; "engaging" and the
/// target's faction, designation and number. The target is not the
/// listener (0): the comm is wing chatter. No target ship, or a text id of
/// 0, loses the system.
pub fn combat_engaging(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, target: u32) -> Line {
    let mut line = Line::new(system, speaker, 0);
    if !line.has_voice_roles(cx, 0x4) {
        return line;
    }
    let Some(target_ship) = line.ship(cx, target) else {
        line.system = None;
        return line;
    };
    let target_ship = Some(target_ship);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3c;
    line.voiced(cx, b"gcs_combat_engaging", b"+");
    let faction = short_faction(cx, target_ship);
    line.push(&faction);
    if line.texts.last() == Some(&0) {
        line.system = None;
        return line;
    }
    let name = reference_name(cx, 1, target_ship, None);
    line.push(&name);
    if line.texts.last() == Some(&0) {
        line.system = None;
        return line;
    }
    let name = reference_name(cx, 2, target_ship, Some(b"-"));
    line.push(&name);
    if line.texts.last() == Some(&0) {
        line.system = None;
    }
    line
}

/// The reason of the no-engage lines: 2 too tough, any other too far.
fn no_engage_reason(reason: i32) -> &'static [u8] {
    if reason == 2 {
        b"gcs_combat_noengagereason_tootough_01-"
    } else {
        b"gcs_combat_noengagereason_toofar_01-"
    }
}

/// `FGcsLineCombatNoEngaging` (`:1611-1628`) and
/// `FGcsLineCombatOrderNoEngage` (`:1647-1665`): role `0x4`; the name with
/// `suffix`, then the reason.
fn no_engage(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    reason: i32,
    name: &[u8],
    suffix: &[u8],
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x4) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3a;
    line.voiced(cx, name, suffix);
    line.push(no_engage_reason(reason));
    line
}

/// `FGcsLineCombatNoEngaging` (`:1611-1628`).
pub fn combat_no_engaging(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    reason: i32,
) -> Line {
    no_engage(
        cx,
        system,
        speaker,
        listener,
        reason,
        b"gcs_combat_noengaging",
        b"+",
    )
}

/// `FGcsLineCombatAskEngage` (`:1630-1636`).
pub fn combat_ask_engage(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(cx, system, speaker, listener, 0x4, b"gcs_combat_askengage")
}

/// `FGcsLineCombatOrderEngage` (`:1638-1645`).
pub fn combat_order_engage(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x4000,
        b"gcs_combat_order_engage",
    )
}

/// `FGcsLineCombatOrderNoEngage` (`:1647-1665`).
pub fn combat_order_no_engage(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    reason: i32,
) -> Line {
    no_engage(
        cx,
        system,
        speaker,
        listener,
        reason,
        b"gcs_combat_order_noengage",
        b"-",
    )
}

/// `FGcsLineCombatEnemySighted` (`:1667-1674`).
pub fn combat_enemy_sighted(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x4000,
        b"gcs_combat_announce_enemysighted",
    )
}

/// `FGcsLineCombatAskForHelp` (`:1676-1698`): role `0x4`; the listener's
/// faction, designation and number, "ask for help".
pub fn combat_ask_for_help(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x4) {
        return line;
    }
    let ship = line.ship(cx, listener);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3c;
    let faction = short_faction(cx, ship);
    line.push(&faction);
    let name = reference_name(cx, 1, ship, None);
    line.push(&name);
    let name = reference_name(cx, 2, ship, Some(b"-"));
    line.push(&name);
    line.voiced(cx, b"gcs_combat_askforhelp", b"-");
    line
}

/// `FGcsLineCombatComingIn` (`:1700-1718`): role `0x4`; "ack", "coming in".
pub fn combat_coming_in(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x4) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3a;
    line.voiced(cx, b"gcs_misc_ack", b"-");
    line.voiced_formation(cx, ship, b"gcs_combat_comingin", b"-");
    line
}

/// `FGcsLineCombatOfferHelp` (`:1720-1746`): role `0x4`; "this is" the
/// speaker "to" the listener, "offer help".
pub fn combat_offer_help(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x4) {
        return line;
    }
    let speaker_ship = line.ship(cx, speaker);
    let listener_ship = line.ship(cx, listener);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a41;
    line.push(b"gcs_misc_thisis+");
    let faction = short_faction(cx, speaker_ship);
    line.push(&faction);
    for kind in [1, 2] {
        let name = reference_name(cx, kind, speaker_ship, None);
        line.push(&name);
    }
    line.push(b"gcs_misc_to");
    let faction = short_faction(cx, listener_ship);
    line.push(&faction);
    let name = reference_name(cx, 1, listener_ship, None);
    line.push(&name);
    let name = reference_name(cx, 2, listener_ship, Some(b"-"));
    line.push(&name);
    line.voiced(cx, b"gcs_combat_offerhelp", b"-");
    line
}

/// `FGcsLineCombatRetreatTowards` (`:1748-1777`): role `0xc004`;
/// "attention all wingmen", "retreat towards", the place of the guide's
/// first target, the flee reason.
pub fn combat_retreat_towards(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    reason: i32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0xc004) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3c;
    line.voiced(cx, b"gcs_misc_attentionallwingmen", b"+");
    line.voiced(cx, b"gcs_combat_order_retreattowards", b"+");
    let targets = guide_targets(cx, ship);
    let place = location_name(cx, targets.id1);
    line.push(&place);
    let reason_name = flee_reason(cx, reason, line.voice);
    line.push(&reason_name);
    line
}

/// `FGcsLineCombatAnnounceFlee` (`:1779-1796`): role `0x4`; "flee", the
/// reason.
pub fn combat_announce_flee(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    reason: i32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x4) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3a;
    line.voiced(cx, b"gcs_combat_announce_flee", b"-");
    let reason_name = flee_reason(cx, reason, line.voice);
    line.push(&reason_name);
    line
}

/// `FGcsLineCombatFriendlyFire` (`:1798-1805`).
pub fn combat_friendly_fire(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x4000,
        b"gcs_combat_announce_ff",
    )
}

/// `FGcsLineCombatDrift` (`:1807-1813`).
pub fn combat_drift(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    single(
        cx,
        system,
        speaker,
        listener,
        0x4000,
        b"gcs_combat_announce_combatdrift",
    )
}

/// `FGcsLineStationAllClear` (`:1815-1835`): a station (role `0x800`):
/// "attention all units", "this is", its place, "all clear".
pub fn station_all_clear(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x800) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3c;
    line.voiced(cx, b"gcs_combat_attentionallunits", b"-");
    line.push(b"gcs_misc_thisis+");
    let place = location_name(cx, speaker);
    line.push(&place);
    line.voiced(cx, b"gcs_combat_announce_allclear", b"-");
    line
}

/// `FGcsLineStationEngage` (`:1837-1870`): a station under attack (role
/// `0x800`): without a place nothing (the system kept); else on
/// `(rand() * 2) / 32768` the engage announcement or "man the turrets".
pub fn station_engage(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x800) {
        return line;
    }
    let place = location_name(cx, speaker);
    if place.is_empty() {
        return line;
    }
    match rand_pick2(cx) {
        1 => {
            line.mood = 0x4a3c;
            line.voiced(cx, b"gcs_combat_attentionallunits", b"-");
            line.push(b"gcs_misc_thisis+");
            line.push(&place);
            line.voiced(cx, b"gcs_combat_announce_engage", b"-");
        }
        0 => {
            line.mood = 0x4a39;
            line.voiced(cx, b"gcs_combat_manturrets", b"-");
        }
        _ => {}
    }
    line
}

// ---- The scan lines ------------------------------------------------------------------------

/// `FGcsLineScanAnnounce` (`:1876-1905`): role `0x8`; "this is" the patrol
/// (faction, designation, number) "to" the scanned ship (faction,
/// designation, number, dash, number), "scan announce".
pub fn scan_announce(cx: &mut Cx<'_>, system: Option<u32>, speaker: u32, listener: u32) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x8) {
        return line;
    }
    let speaker_ship = line.ship(cx, speaker);
    let listener_ship = line.ship(cx, listener);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a43;
    line.push(b"gcs_misc_thisis+");
    let faction = short_faction(cx, speaker_ship);
    line.push(&faction);
    for kind in [1, 2] {
        let name = reference_name(cx, kind, speaker_ship, None);
        line.push(&name);
    }
    line.push(b"gcs_misc_to");
    let faction = short_faction(cx, listener_ship);
    line.push(&faction);
    for kind in [1, 2] {
        let name = reference_name(cx, kind, listener_ship, None);
        line.push(&name);
    }
    line.push(b"gcs_misc_dash");
    let name = reference_name(cx, 3, listener_ship, None);
    line.push(&name);
    line.voiced(cx, b"gcs_scan_announce", b"-");
    line
}

/// The one-name scan lines (role `0x8`): `FGcsLineScanNothingFound`,
/// `OrderDrop`, `Refuse`, `Comply`, `Angry`, `Happy` and `LootDestroyed`
/// (`:1907-1963`).
pub fn scan_single(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    name: &[u8],
) -> Line {
    single(cx, system, speaker, listener, 0x8, name)
}

// ---- The fidget lines ----------------------------------------------------------------------

/// The one-name fidget lines (role `0x400`): `FGcsLineFidgetTradeRumor`,
/// `TradeSimple`, `PatrolRumor` and `PatrolSimple` (`:1969-2099`).
pub fn fidget_single(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    name: &[u8],
) -> Line {
    single(cx, system, speaker, listener, 0x400, name)
}

/// `FGcsLineFidgetTradeIHear` (`:1978-2000`): what the trader heard about
/// the good.
pub fn fidget_trade_i_hear(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
    good: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x400) || rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3b;
    line.voiced(cx, b"gcs_fidget_trade_ihear", b"+");
    line.push(&good_prefix(cx, good));
    line.voiced(cx, b"gcs_fidget_talkcargo", b"-");
    line
}

/// `FGcsLineFidgetStatusQuery` (`:2011-2039`): the leader asks its
/// wingmen for their status.
pub fn fidget_status_query(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x400) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a40;
    line.voiced(cx, b"gcs_misc_attentionallwingmen", b"+");
    line.push(b"gcs_misc_thisis+");
    let faction = short_faction(cx, ship);
    line.push(&faction);
    for kind in [1, 2] {
        let name = reference_name(cx, kind, ship, None);
        line.push(&name);
    }
    line.push(b"gcs_misc_dash");
    let name = reference_name(cx, 3, ship, None);
    line.push(&name);
    line.voiced(cx, b"gcs_fidget_whatstatus", b"-");
    line
}

/// `FGcsLineFidgetStatusReport` (`:2041-2081`): a wingman's designation and
/// its damage by its relative health (below 0.3 large, 0.75 medium, 1
/// small, else none). A failed health read, which leaves the ship
/// pointer's bits in the C++'s local, reads 0 (D936).
pub fn fidget_status_report(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x400) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3e;
    designation(cx, &mut line, ship);
    let id = ship.map_or(0, |k| crate::content::ships::id(&cx.logic.content.state, k));
    let health = api::space_obj::relative_health(cx, id).unwrap_or(0.0);
    let name: &[u8] = if f64::from(health) < 0.3 {
        b"gcs_fidget_saystatus_damagelarge"
    } else if f64::from(health) < 0.75 {
        b"gcs_fidget_saystatus_damagemedium"
    } else if f64::from(health) < 1.0 {
        b"gcs_fidget_saystatus_damagesmall"
    } else {
        b"gcs_fidget_saystatus_damagenone"
    };
    line.voiced(cx, name, b"-");
    line
}

/// `FGcsLineFidgetWaypointQuery` (`:2101-2129`): a wingman asks its leader
/// how many waypoints are left.
pub fn fidget_waypoint_query(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x400) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    if rand_skips(cx) {
        return line;
    }
    line.mood = 0x4a3f;
    line.push(b"gcs_misc_thisis+");
    let faction = short_faction(cx, ship);
    line.push(&faction);
    for kind in [1, 2] {
        let name = reference_name(cx, kind, ship, None);
        line.push(&name);
    }
    line.push(b"gcs_misc_dash");
    let name = reference_name(cx, 3, ship, None);
    line.push(&name);
    line.voiced(cx, b"gcs_fidget_patrol_wpremainquery", b"-");
    line
}

/// `FGcsLineFidgetPatrolStatus` (`:2131-2172`): the leader answers with its
/// patrol's points to go; always "we", with a `rand()` test when points
/// are left.
pub fn fidget_patrol_status(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> Line {
    let mut line = Line::new(system, speaker, listener);
    if !line.has_voice_roles(cx, 0x400) {
        return line;
    }
    let ship = line.ship(cx, speaker);
    let points = super::path_points_to_go(cx, ship);
    patrol_progress(cx, &mut line, ship, points, true);
    line
}
