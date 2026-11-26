//! The server's save games and autosaves (work package P3.3):
//! `PlayerData_SaveGame`, `PlayerData_MissionSave`, `PlayerData_AutoSave`
//! and `PlayerData_SkipNextAutosave` (`Server/server_savegame.cpp:688-806`),
//! with the `IServer` entries that call them, `SaveGame`, `MissionSaveB`
//! (`Server/IServerImpl.cpp:1829`, `:1849`) and `CharacterSkipAutosave`
//! (`:782`). `pub::Save` (`Server/server_pub.cpp:2072`) is
//! [`crate::api::misc::save`].
//!
//! Every save goes through the writer of P3.2
//! ([`crate::save_writer::serialize_to_file`]), so a save holds what the C++
//! writes for the same slot. The C++'s autosave triggers call
//! [`auto_save`] in their place: a player's own ship landing on a
//! multiplayer server ([`crate::replication::object_landed`]), the end of a
//! player's gate jump ([`crate::transfer::system_switch_out_complete`]), the
//! character list's multiplayer prologue
//! ([`crate::login::build_character_set`]), `IServerImpl::BaseExit`
//! ([`crate::session::base_exit`]), a multiplayer `PlayerDB::logout`
//! ([`crate::login::logout`]) and `pub::Save` of type 1. The client's
//! `SAVEGAME` reaches [`server_save_game`] through the session
//! ([`crate::session::route`]), with the character id of its
//! single-player form.
//!
//! The server's save storage is [`Saves`] in [`crate::server::Logic`]: the
//! accounts folder (the C++'s `Server_GetAccountsRoot`), the local clock the
//! `tstamp` line, the multiplayer autosave's description and the cheat log
//! read (`GetLocalTime`, `_strdate`, `_strtime`), and the path of the last
//! file written (`GServerLastSavePath`). A server without an accounts folder
//! writes nothing (D752).
//!
//! The mission sections of a save are Content's (P3.5): the writer hands
//! the open file to every controller with the `PlayerSave` broadcast and,
//! in single player, with `SaveGame`, as the C++ does; until Content's
//! handlers exist no controller writes into it, which is what the C++
//! writes when no mission state exists.

use std::path::PathBuf;

use neuron_core::hash::c_str;
use neuron_server::account::AccountsRoot;

use crate::cheats::LocalTime;
use crate::save_writer::{SaveOutcome, serialize_to_file};
use crate::server::Cx;

/// The text id of the autosave's description (`0x65c`), which
/// `PlayerData_AutoSave` gives `PlayerData_SaveGame` and the character list
/// shows for `AutoSave.fl`.
pub const AUTOSAVE_STRID: u32 = 0x65c;

/// The name of the single-player autosave, without its extension.
pub const AUTOSAVE_NAME: &str = "AutoSave";

/// How many names `PlayerData_SaveGame` tries for an unnamed save
/// (`Attempt < 0xc8`).
const NAME_PROBES: i32 = 0xc8;

/// The fields of a Windows `SYSTEMTIME`, as `GetLocalTime` fills them.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SystemTime {
    /// The year.
    pub year: u16,
    /// The month, 1 to 12.
    pub month: u16,
    /// The day of the week, 0 (Sunday) to 6.
    pub day_of_week: u16,
    /// The day of the month, 1 to 31.
    pub day: u16,
    /// The hour, 0 to 23.
    pub hour: u16,
    /// The minute.
    pub minute: u16,
    /// The second.
    pub second: u16,
    /// The millisecond.
    pub milliseconds: u16,
}

/// Days from 1601-01-01 to 1970-01-01.
const DAYS_1601_TO_1970: i64 = 134_774;
/// `FILETIME` units (100 ns) in a second.
const TICKS_PER_SECOND: i64 = 10_000_000;
/// `FILETIME` units in a millisecond.
const TICKS_PER_MILLISECOND: i64 = 10_000;

/// The days from 1970-01-01 to a proleptic Gregorian date.
fn days_from_civil(year: i64, month: i64, day: i64) -> i64 {
    let y = if month <= 2 { year - 1 } else { year };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let mp = (month + 9) % 12;
    let doy = (153 * mp + 2) / 5 + day - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// The date of a day counted from 1970-01-01: year, month, day.
fn civil_from_days(days: i64) -> (i64, i64, i64) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    (year, month, day)
}

impl SystemTime {
    /// The time `millis` milliseconds after 1970-01-01 00:00:00.
    #[must_use]
    pub fn from_unix_millis(millis: i64) -> Self {
        let days = millis.div_euclid(86_400_000);
        let rest = millis.rem_euclid(86_400_000);
        let (year, month, day) = civil_from_days(days);
        let field = |v: i64| u16::try_from(v).unwrap_or(0);
        Self {
            year: field(year),
            month: field(month),
            // 1970-01-01 was a Thursday.
            day_of_week: field((days + 4).rem_euclid(7)),
            day: field(day),
            hour: field(rest / 3_600_000),
            minute: field(rest / 60_000 % 60),
            second: field(rest / 1000 % 60),
            milliseconds: field(rest % 1000),
        }
    }

    /// `SystemTimeToFileTime`: the 100 ns intervals since 1601-01-01 as
    /// `(dwHighDateTime, dwLowDateTime)`; the day of the week is not read.
    #[must_use]
    pub fn file_time(&self) -> (u32, u32) {
        let days = days_from_civil(
            i64::from(self.year),
            i64::from(self.month),
            i64::from(self.day),
        ) + DAYS_1601_TO_1970;
        let seconds = days * 86_400
            + i64::from(self.hour) * 3600
            + i64::from(self.minute) * 60
            + i64::from(self.second);
        let ticks =
            seconds * TICKS_PER_SECOND + i64::from(self.milliseconds) * TICKS_PER_MILLISECOND;
        let ticks = u64::try_from(ticks).unwrap_or(0);
        ((ticks >> 32) as u32, (ticks & 0xFFFF_FFFF) as u32)
    }

    /// `_strdate`: `MM/DD/YY`.
    #[must_use]
    pub fn strdate(&self) -> String {
        format!("{:02}/{:02}/{:02}", self.month, self.day, self.year % 100)
    }

    /// `_strtime`: `HH:MM:SS`.
    #[must_use]
    pub fn strtime(&self) -> String {
        format!("{:02}:{:02}:{:02}", self.hour, self.minute, self.second)
    }

    /// The fields `cheaters.log` prints.
    #[must_use]
    pub fn cheat_time(&self) -> LocalTime {
        LocalTime {
            year: self.year,
            month: self.month,
            day: self.day,
            hour: self.hour,
            minute: self.minute,
            second: self.second,
        }
    }
}

/// Where the server reads the local time (`GetLocalTime`, and `_tzset`
/// with `_strdate` and `_strtime`).
pub trait LocalClock {
    /// The local time now.
    fn local_time(&mut self) -> SystemTime;
}

/// The clock of a server whose host gives none: the system clock in UTC,
/// because the standard library knows no time zone: for tests and hosts
/// that give none. `OutpostServer` gives the machine's local time (P3.9).
#[derive(Clone, Copy, Debug, Default)]
pub struct UtcClock;

impl LocalClock for UtcClock {
    fn local_time(&mut self) -> SystemTime {
        let millis = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| i64::try_from(d.as_millis()).unwrap_or(0));
        SystemTime::from_unix_millis(millis)
    }
}

/// A clock that always answers the same time, for tests and the harness.
#[derive(Clone, Copy, Debug, Default)]
pub struct FixedClock(pub SystemTime);

impl LocalClock for FixedClock {
    fn local_time(&mut self) -> SystemTime {
        self.0
    }
}

/// The server's save storage.
pub struct Saves {
    /// The accounts folder (`Server_GetAccountsRoot`); a server without one
    /// writes no saves. The host sets it to the root its
    /// [`crate::login::ServerFiles`] name.
    pub accounts: Option<AccountsRoot>,
    /// The local clock.
    pub clock: Box<dyn LocalClock>,
    /// The file the writer last opened (`GServerLastSavePath`), which
    /// `PlayerData_MissionSave` copies.
    pub last_save_path: Option<PathBuf>,
}

impl Default for Saves {
    fn default() -> Self {
        Self {
            accounts: None,
            clock: Box::new(UtcClock),
            last_save_path: None,
        }
    }
}

/// `PlayerData_SerializeToFile` with the server's storage and clock: the
/// `tstamp` and the cheat log take the local time, and the path the writer
/// opened becomes the last save path. False when nothing was written,
/// also for a server without an accounts folder.
pub fn serialize(
    cx: &mut Cx<'_>,
    session: u32,
    file_name: &[u8],
    description: Option<&[u16]>,
    descrip_strid: u32,
) -> bool {
    let Some(accounts) = cx.logic.saves.accounts.clone() else {
        return false;
    };
    let now = cx.logic.saves.clock.local_time();
    let name = String::from_utf8_lossy(file_name).into_owned();
    let outcome = serialize_to_file(
        cx,
        session,
        &accounts,
        &name,
        description,
        descrip_strid,
        now.file_time(),
        now.cheat_time(),
    );
    match outcome {
        SaveOutcome::Written(path) => {
            cx.logic.saves.last_save_path = Some(path);
            true
        }
        // The C++ keeps the path once the file is open, before it writes.
        SaveOutcome::WriteFailed(path) => {
            cx.logic.saves.last_save_path = Some(path);
            false
        }
        SaveOutcome::NoPlayer | SaveOutcome::Refused | SaveOutcome::OpenFailed(_) => false,
    }
}

/// The name an unnamed save takes: from a `rand()` seed, `Save%04x` of the
/// seed's low 16 bits sign-extended (`MOVSX`, so a seed past 0x7fff prints
/// `ffff8000` and up), the seed counting up, the first of 200 that names no
/// file in the account's folder (`GetFileAttributesA` failing with
/// `ERROR_FILE_NOT_FOUND`); empty when all 200 are taken.
fn free_save_name(cx: &mut Cx<'_>, accounts: &AccountsRoot, account: &[u16]) -> Vec<u8> {
    let dir = accounts.account_dir(account);
    let mut seed = cx.world.rng().rand();
    let mut attempt = 1;
    loop {
        // `(int)(short)Seed`, printed by `%04x` as an unsigned int.
        let low = (seed & 0xFFFF) as u16 as i16;
        let candidate = format!("Save{:04x}", i32::from(low) as u32);
        let probe = dir.join(format!("{candidate}.fl"));
        // `ERROR_FILE_NOT_FOUND`, not `ERROR_PATH_NOT_FOUND`: the folder
        // must exist for the name to be free.
        let missing = std::fs::symlink_metadata(&probe)
            .is_err_and(|e| e.kind() == std::io::ErrorKind::NotFound);
        if missing && dir.is_dir() {
            return candidate.into_bytes();
        }
        seed = seed.wrapping_add(1);
        if attempt >= NAME_PROBES {
            return Vec::new();
        }
        attempt += 1;
    }
}

/// `PlayerData_SaveGame(slot, name, description, strid)`: a named save
/// keeps the name up to its first `.` (of at most 0x200 bytes); an unnamed
/// one takes a free `Save%04x` ([`free_save_name`]). The writer then saves
/// the slot under that name; on success the file is read back into the
/// slot's characters (`PlayerData_ReloadSavedCharacter`), otherwise the
/// slot counts a save failure.
pub fn save_game(
    cx: &mut Cx<'_>,
    session: u32,
    name: &[u8],
    description: Option<&[u16]>,
    descrip_strid: u32,
) -> bool {
    let Some(accounts) = cx.logic.saves.accounts.clone() else {
        return false;
    };
    let Some(account) = cx
        .logic
        .players
        .get(session)
        .map(|s| s.account_name.clone())
    else {
        return false;
    };
    let name = c_str(&name[..name.len().min(0x200)]);
    let file_name = if name.is_empty() {
        free_save_name(cx, &accounts, &account)
    } else {
        let end = name.iter().position(|&b| b == b'.').unwrap_or(name.len());
        name[..end].to_vec()
    };
    if serialize(cx, session, &file_name, description, descrip_strid) {
        crate::login::reload_saved_character_in(cx, &accounts, session, &file_name);
        return true;
    }
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.save_failure_count += 1;
    }
    false
}

/// `PlayerData_MissionSave(slot, strid)`: an unnamed save with the text id
/// ([`save_game`]), then the file written copied over the account's
/// `AutoSave.fl` (`CopyFileA`, overwriting); the copy's success.
pub fn mission_save(cx: &mut Cx<'_>, session: u32, descrip_strid: u32) -> bool {
    if !save_game(cx, session, b"", None, descrip_strid) {
        return false;
    }
    let (Some(accounts), Some(source)) = (
        cx.logic.saves.accounts.clone(),
        cx.logic.saves.last_save_path.clone(),
    ) else {
        return false;
    };
    let Some(account) = cx
        .logic
        .players
        .get(session)
        .map(|s| s.account_name.clone())
    else {
        return false;
    };
    let destination = accounts
        .account_dir(&account)
        .join(format!("{AUTOSAVE_NAME}.fl"));
    std::fs::copy(source, destination).is_ok()
}

/// `PlayerData_AutoSave(slot)`.
///
/// On a multiplayer server: nothing without a selected character or with
/// system 0; otherwise the selected character's own file is written with
/// the local `"<date> <time>"` (`_strdate`, `_strtime`) as its description
/// and no text id. The C++ looks the record up without testing the end
/// (character selection stores the id only once its record exists); the
/// port writes nothing without one (D753).
///
/// In single player: when the slot's base (its low 30 bits) forbids
/// autosaves (`autosave_forbidden`) or the skip latch is set, the latch is
/// cleared and nothing is written; otherwise [`save_game`] writes
/// `AutoSave` with the text id 0x65c. A base the universe does not have,
/// which the C++ reads through null, does not forbid it (D751).
pub fn auto_save(cx: &mut Cx<'_>, session: u32) -> bool {
    let Some(slot) = cx.logic.players.get(session) else {
        return false;
    };
    if cx.logic.multiplayer_server {
        if !slot.selected_character.is_valid() || slot.system_id == 0 {
            return false;
        }
        let Some(file_name) = slot
            .saved_characters
            .get(&slot.selected_character)
            .map(|r| r.file_name.clone())
        else {
            return false;
        };
        let now = cx.logic.saves.clock.local_time();
        let description: Vec<u16> = format!("{} {}", now.strdate(), now.strtime())
            .encode_utf16()
            .collect();
        return serialize(cx, session, &file_name, Some(&description), 0);
    }
    let base = slot.current_base;
    let skip = slot.skip_next_autosave;
    let allowed = base & 0x3fff_ffff == 0
        || !cx
            .world
            .data()
            .universe
            .bases
            .get(&base)
            .is_some_and(|b| b.autosave_forbidden);
    if !skip && allowed {
        return save_game(cx, session, AUTOSAVE_NAME.as_bytes(), None, AUTOSAVE_STRID);
    }
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.skip_next_autosave = false;
    }
    false
}

/// `PlayerData_SkipNextAutosave`: the next single-player autosave is
/// skipped.
pub fn skip_next_autosave(cx: &mut Cx<'_>, session: u32) {
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.skip_next_autosave = true;
    }
}

/// `IServerImpl::SaveGame(char_id, description, session)`: in single
/// player, for a valid session whose player is not in a ship (the low 30
/// bits of its ship id are 0), [`save_game`] under the character id's name
/// with the description and no text id.
pub fn server_save_game(
    cx: &mut Cx<'_>,
    character: &[u8],
    description: Option<&[u16]>,
    session: u32,
) {
    if !cx.world.config().single_player || !cx.logic.players.is_valid(session) {
        return;
    }
    if cx
        .logic
        .players
        .get(session)
        .is_none_or(|s| s.ship_object_id & 0x3fff_ffff != 0)
    {
        return;
    }
    let _ = save_game(cx, session, character, description, 0);
}

/// `IServerImpl::MissionSaveB(session, mission_id)`: in single player, for
/// a valid session, [`mission_save`] with the mission id as the text id.
pub fn mission_save_b(cx: &mut Cx<'_>, session: u32, mission_id: u32) {
    if !cx.world.config().single_player || !cx.logic.players.is_valid(session) {
        return;
    }
    let _ = mission_save(cx, session, mission_id);
}

/// `IServerImpl::CharacterSkipAutosave(session)`: for a valid session,
/// [`skip_next_autosave`].
pub fn character_skip_autosave(cx: &mut Cx<'_>, session: u32) {
    if cx.logic.players.is_valid(session) {
        skip_next_autosave(cx, session);
    }
}

/// `pub::Save(player, type)`: in single player, type 1 is
/// [`auto_save`], any other [`mission_save`] with the type as the text id.
/// The slot is the C++'s `Players[player]`, unchecked; the port does
/// nothing for a player outside the table (D753).
pub fn pub_save(cx: &mut Cx<'_>, player: u32, save_type: u32) {
    if !cx.world.config().single_player || cx.logic.players.get(player).is_none() {
        return;
    }
    if save_type == 1 {
        let _ = auto_save(cx, player);
    } else {
        let _ = mission_save(cx, player, save_type);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn file_time_counts_from_1601() {
        let t = SystemTime {
            year: 1601,
            month: 1,
            day: 1,
            ..SystemTime::default()
        };
        assert_eq!(t.file_time(), (0, 0));
        // 1970-01-01 is 116444736000000000 (0x019DB1DE_D53E8000).
        let t = SystemTime::from_unix_millis(0);
        assert_eq!((t.year, t.month, t.day, t.day_of_week), (1970, 1, 1, 4));
        assert_eq!(t.file_time(), (0x019D_B1DE, 0xD53E_8000));
        // 2026-11-22 13:05:09.250, a Sunday.
        let millis = (days_from_civil(2026, 11, 22) * 86_400 + 13 * 3600 + 5 * 60 + 9) * 1000 + 250;
        let t = SystemTime::from_unix_millis(millis);
        assert_eq!(
            (
                t.year,
                t.month,
                t.day,
                t.day_of_week,
                t.hour,
                t.minute,
                t.second,
                t.milliseconds
            ),
            (2026, 11, 22, 0, 13, 5, 9, 250)
        );
        let ticks = (u64::from(t.file_time().0) << 32) | u64::from(t.file_time().1);
        assert_eq!(
            ticks,
            u64::try_from(millis).unwrap() * 10_000 + 116_444_736_000_000_000
        );
        assert_eq!(t.strdate(), "11/22/26");
        assert_eq!(t.strtime(), "13:05:09");
    }
}
