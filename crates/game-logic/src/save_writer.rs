//! The character save writer (work package P3.2):
//! `PlayerData_SerializeToFile` (`Server/server_pub.cpp:1179`) and the
//! helpers it calls (`Server_WriteCostume`, `Server_FormatMissionLogEntry`,
//! `Server_SaveSinglePlayerState`, `Reputation::Save`).
//!
//! The save is plain text (decision O12): the C++ opens it with `"wt"`, so
//! each `\n` it prints reaches the file as `\r\n`. [`save_text`] builds the
//! text as the C++ prints it, before that translation; [`serialize_to_file`]
//! writes it where the C++ does and returns what happened.
//!
//! Two gaps remain until later work packages: the position of a ship in space on a
//! multiplayer server is the object's own, because the remote physics
//! controller the C++ asks first belongs to P3.4; and `GetLocalTime` is the
//! caller's (the `tstamp` argument), so a test can fix it.

use std::io::Write as _;
use std::path::PathBuf;

use game_sim::data::costume::Costume;
use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::string_cache::StringCache;
use game_sim::protocol::fmtstr::{FmtStr, FmtValue};
use neuron_core::msvcrt::{format_g, text_mode_write};
use neuron_core::util::wide_hex::wide_to_hex;
use neuron_server::account::AccountsRoot;
use neuron_server::controller::ControllerId;

use crate::character_file::CollisionGroupDesc;
use crate::events::ControllerMessage;
use crate::player;
use crate::server::Cx;

/// `sprintf("%g", value)`.
fn g(value: f64) -> String {
    format_g(value, 6)
}

/// `fprintf(f, "%s = %s\n", key, value)`.
fn key(out: &mut Vec<u8>, name: &str, value: &[u8]) {
    out.extend_from_slice(name.as_bytes());
    out.extend_from_slice(b" = ");
    out.extend_from_slice(value);
    out.push(b'\n');
}

/// `Server_FormatMissionLogEntry`: `"id, count"`, then `", data, tag,
/// flags"` per value; a nested string follows its value after a bare `,`.
/// The C++ prints a nested string's or a navigation marker's payload
/// dword, which is the address of its heap block; the port prints 0 there
/// (D602).
#[must_use]
pub fn format_mission_log_entry(entry: &FmtStr) -> String {
    let mut out = format!("{}, {}", entry.mad_lib_id, entry.values.len());
    for value in &entry.values {
        let data = match value {
            FmtValue::Value { value, .. } => *value,
            FmtValue::Nested { .. } | FmtValue::NavMarker { .. } => 0,
        };
        out.push_str(&format!(", {data}, {}, {}", value.tag(), value.flags()));
        if let FmtValue::Nested { fmt, .. } = value {
            out.push(',');
            out.push_str(&format_mission_log_entry(fmt));
        }
    }
    out
}

/// `Server_WriteCostume`: the body, head, left and right hand parts that
/// are set and known, each as its decimal id (`bodypart::get_name`). The
/// accessories are not written.
fn write_costume(out: &mut Vec<u8>, cx: &Cx<'_>, prefix: &str, costume: &Costume) {
    const PARTS: [(usize, usize, &str); 4] = [
        (0, 1, "body"),
        (1, 0, "head"),
        (2, 2, "lefthand"),
        (3, 3, "righthand"),
    ];
    let costumes = &cx.world.data().costumes;
    for (kind, slot, name) in PARTS {
        let id = costume.part_ids[slot];
        if id == 0 {
            continue;
        }
        let Some(part) = costumes.find_bodypart(kind, id) else {
            continue;
        };
        key(
            out,
            &format!("{prefix}{name}"),
            part.id.to_string().as_bytes(),
        );
    }
}

/// One `equip` / `cargo` line (`base_equip` / `base_cargo` with `base`),
/// skipping items whose archetype is unknown.
fn write_equipment_item(out: &mut Vec<u8>, cx: &Cx<'_>, item: &EquipDesc, base: bool) {
    let Some(equipment) = cx.world.data().archetypes.equipment.get(&item.arch_id) else {
        return;
    };
    let nickname = equipment.root.nickname;
    let strings: &StringCache = cx.world.strings();
    let mission = i32::from(item.mission);
    if item.equipped == 0 {
        let line = if item.health == 1.0 {
            format!("{nickname}, {}, , , {mission}", item.count)
        } else {
            format!(
                "{nickname}, {}, , {}, {mission}",
                item.count,
                g(f64::from(item.health))
            )
        };
        key(
            out,
            if base { "base_cargo" } else { "cargo" },
            line.as_bytes(),
        );
    } else {
        let mut line = format!("{nickname}, ").into_bytes();
        if !item.is_internal(strings) {
            line.extend_from_slice(strings.get(item.hardpoint));
        }
        line.extend_from_slice(b", ");
        line.extend_from_slice(g(f64::from(item.health)).as_bytes());
        key(out, if base { "base_equip" } else { "equip" }, &line);
    }
}

/// The collision groups of a ship by name (`collision_group` /
/// `base_collision_group`); `damaged_only` skips groups at full health.
fn write_collision_groups(
    out: &mut Vec<u8>,
    cx: &Cx<'_>,
    ship: u32,
    groups: &[CollisionGroupDesc],
    name: &str,
    damaged_only: bool,
) {
    let Some(arch) = cx.world.data().archetypes.ships.get(&ship) else {
        return;
    };
    for desc in groups {
        // `!(health < 1)`: a NaN health is skipped too.
        if damaged_only && (desc.health >= 1.0 || desc.health.is_nan()) {
            continue;
        }
        let Some(group) = arch.eq_obj.group_by_id(desc.id) else {
            continue;
        };
        let mut line = cx.world.strings().get(group.obj).to_vec();
        line.extend_from_slice(b", ");
        line.extend_from_slice(g(f64::from(desc.health)).as_bytes());
        key(out, name, &line);
    }
}

/// The Euler angles `rotate` prints, in degrees, from an orientation's
/// rows, as the C++ evaluates them (T11: `f64`, with the stores it makes to
/// `float`).
fn rotate_angles(m: &[[f32; 3]; 3]) -> (f64, f64, f64) {
    let e = |i: usize| f64::from(m[i / 3][i % 3]);
    let deg_per_rad = f64::from(57.295_776_f32);
    let hypot_exact = (e(0) * e(0) + e(3) * e(3)).sqrt();
    let hypot = hypot_exact as f32;
    let (a, b, c) = if hypot_exact > f64::from(1.907_348_6e-6_f32) {
        (
            e(7).atan2(e(8)),
            (-e(6)).atan2(f64::from(hypot)),
            e(3).atan2(e(0)) as f32,
        )
    } else {
        ((-e(5)).atan2(e(4)), (-e(6)).atan2(f64::from(hypot)), 0.0)
    };
    let deg_a = (a * deg_per_rad) as f32;
    (
        f64::from(deg_a),
        b * deg_per_rad,
        f64::from(c) * deg_per_rad,
    )
}

/// What the writer leaves for the multiplayer record refresh: the position
/// and orientation it wrote (zero when it wrote none) and the rank.
#[derive(Clone, Debug, PartialEq)]
pub struct Written {
    /// The text as the C++ prints it, before the text-mode translation.
    pub text: Vec<u8>,
    /// The position `pos` printed, or zero.
    pub position: [f32; 3],
    /// The orientation `rotate` printed, or zero.
    pub orientation: [[f32; 3]; 3],
    /// The rank `rank` printed.
    pub rank: u32,
    /// The feeling overrides `house` printed.
    pub relations: Vec<(u32, f32)>,
}

/// The body of `PlayerData_SerializeToFile`, from the pending-visit flush
/// to the `PlayerSave` broadcast and the single-player state: the text it
/// prints for the slot of `session`, with `tstamp` as the `FILETIME` of the
/// local time (high, low). `None` for a session without a slot.
///
/// The slot's vibe must exist; for one that does not, the C++ hands an
/// uninitialised pointer to `WideToHex` and the port writes an empty name,
/// no rank, no `house` and no `rep_group` lines (D601).
pub fn save_text(
    cx: &mut Cx<'_>,
    session: u32,
    description: Option<&[u16]>,
    descrip_strid: u32,
    tstamp: (u32, u32),
) -> Option<Written> {
    prepare(cx, session)?;
    print(cx, session, description, descrip_strid, tstamp)
}

/// What the C++ does to the slot before it opens the file: a dead ship on
/// a multiplayer server gets its last-base loadout back, and the pending
/// visits are committed.
fn prepare(cx: &mut Cx<'_>, session: u32) -> Option<()> {
    let single_player = cx.world.config().single_player;
    if !single_player && cx.logic.players.get(session)?.relative_health <= 0.0 {
        player::reload_ship_data(cx, session);
    }
    player::flush_pending_visits(cx.logic.players.get_mut(session)?);
    Some(())
}

fn print(
    cx: &mut Cx<'_>,
    session: u32,
    description: Option<&[u16]>,
    descrip_strid: u32,
    tstamp: (u32, u32),
) -> Option<Written> {
    let single_player = cx.world.config().single_player;
    let mut out = Vec::new();
    out.extend_from_slice(b"\n[Player]\n");
    if let Some(description) = description {
        key(&mut out, "description", wide_to_hex(description).as_bytes());
    }
    if descrip_strid != 0 {
        key(
            &mut out,
            "descrip_strid",
            descrip_strid.to_string().as_bytes(),
        );
    }
    out.push(b'\n');
    key(
        &mut out,
        "tstamp",
        format!("{},{}", tstamp.0, tstamp.1).as_bytes(),
    );

    let rep_vibe = cx.logic.players.get(session)?.rep_vibe;
    let summary = {
        let (vibes, _) = cx.world.vibes();
        vibes.summary(rep_vibe)
    };
    let (affiliation, mut rank, relations, display_name) = match summary {
        Some(s) => (s.affiliation, s.rank, s.relations, s.display_name),
        None => (u32::MAX, 0, Vec::new(), Vec::new()),
    };
    key(&mut out, "name", wide_to_hex(&display_name).as_bytes());
    if single_player {
        rank = cx.logic.players.get(session)?.rank_level;
    }
    key(&mut out, "rank", rank.to_string().as_bytes());
    {
        let (_, reputation) = cx.world.vibes();
        for &(group, feeling) in &relations {
            let mut line = g(f64::from(feeling)).into_bytes();
            line.extend_from_slice(b", ");
            line.extend_from_slice(reputation.get_nickname(group).unwrap_or_default());
            key(&mut out, "house", &line);
        }
        if affiliation != u32::MAX {
            let nick = reputation.get_nickname(affiliation).unwrap_or_default();
            key(&mut out, "rep_group", nick);
        }
    }

    let slot = cx.logic.players.get(session)?;
    key(&mut out, "money", slot.cash.to_string().as_bytes());
    key(&mut out, "num_kills", slot.num_kills.to_string().as_bytes());
    key(
        &mut out,
        "num_misn_successes",
        slot.num_mission_successes.to_string().as_bytes(),
    );
    key(
        &mut out,
        "num_misn_failures",
        slot.num_mission_failures.to_string().as_bytes(),
    );
    out.push(b'\n');
    key(&mut out, "voice", &slot.voice);
    let (com_costume, costume) = (slot.com_costume.clone(), slot.costume.clone());
    write_costume(&mut out, cx, "com_", &com_costume);
    write_costume(&mut out, cx, "", &costume);
    out.push(b'\n');

    // `system`: the C++ dereferences a missing system (D601).
    let slot = cx.logic.players.get(session)?;
    let system = cx
        .world
        .data()
        .universe
        .systems
        .get(&slot.system_id)
        .map(|s| cx.world.strings().get(s.nickname).to_vec())
        .unwrap_or_default();
    key(&mut out, "system", &system);

    let mut saved_pos = [0.0f32; 3];
    let mut saved_orient = [[0.0f32; 3]; 3];
    {
        let slot = cx.logic.players.get_mut(session)?;
        if slot.ship_object_id == 0 && slot.current_base & 0x3FFF_FFFF == 0 {
            slot.current_base = slot.last_base_id;
        }
    }
    let slot = cx.logic.players.get(session)?;
    let base = cx
        .world
        .data()
        .universe
        .bases
        .get(&slot.current_base)
        .map(|b| cx.world.strings().get(b.nickname).to_vec());
    if let Some(base) = base {
        key(&mut out, "base", &base);
        out.push(b'\n');
        if !single_player {
            let slot = cx.logic.players.get_mut(session)?;
            let record = slot
                .saved_characters
                .entry(slot.selected_character.clone())
                .or_default();
            record.base_hull_status = slot.relative_health;
            record
                .base_collision_groups
                .clone_from(&slot.collision_groups);
            record.base_equipment.clone_from(&slot.equipment);
        }
    } else {
        let ship_object = slot.ship_object_id;
        let live = cx
            .world
            .find(ship_object)
            .and_then(|h| cx.world.get(h))
            .map(|o| (o.engine.position, o.engine.orientation));
        if let Some((position, orientation)) = live {
            saved_pos = [position.x, position.y, position.z];
            key(
                &mut out,
                "pos",
                format!(
                    "{},{},{}",
                    g(f64::from(saved_pos[0])),
                    g(f64::from(saved_pos[1])),
                    g(f64::from(saved_pos[2]))
                )
                .as_bytes(),
            );
            saved_orient = orientation.m;
            let (a, b, c) = rotate_angles(&saved_orient);
            key(
                &mut out,
                "rotate",
                format!("{},{},{}", g(a), g(b), g(c)).as_bytes(),
            );
            out.push(b'\n');
        }
    }

    let slot = cx.logic.players.get(session)?;
    if slot.current_location != 0 {
        key(
            &mut out,
            "location",
            slot.current_location.to_string().as_bytes(),
        );
    }
    if slot.ship_archetype != 0
        && let Some(ship) = cx.world.data().archetypes.ships.get(&slot.ship_archetype)
    {
        key(
            &mut out,
            "ship_archetype",
            ship.eq_obj.root.nickname.to_string().as_bytes(),
        );
        if slot.relative_health < 1.0 {
            key(
                &mut out,
                "hull_status",
                g(f64::from(slot.relative_health)).as_bytes(),
            );
        }
        write_collision_groups(
            &mut out,
            cx,
            slot.ship_archetype,
            &slot.collision_groups,
            "collision_group",
            true,
        );
    }
    out.push(b'\n');
    for item in &slot.equipment {
        if !single_player && item.mission {
            continue;
        }
        write_equipment_item(&mut out, cx, item, false);
    }
    out.push(b'\n');

    if !single_player {
        let last_base = cx
            .world
            .data()
            .universe
            .bases
            .get(&slot.last_base_id)
            .map(|b| cx.world.strings().get(b.nickname).to_vec());
        if let Some(last_base) = last_base {
            let record = slot
                .saved_characters
                .get(&slot.selected_character)
                .cloned()
                .unwrap_or_default();
            key(&mut out, "last_base", &last_base);
            out.push(b'\n');
            key(
                &mut out,
                "base_hull_status",
                g(f64::from(record.base_hull_status)).as_bytes(),
            );
            write_collision_groups(
                &mut out,
                cx,
                slot.ship_archetype,
                &record.base_collision_groups,
                "base_collision_group",
                false,
            );
            out.push(b'\n');
            for item in &record.base_equipment {
                if item.mission {
                    continue;
                }
                write_equipment_item(&mut out, cx, item, true);
            }
            out.push(b'\n');
        }
    }

    // The C++ hands the weapon groups to `fprintf` as its format; the port
    // writes them as they are (D603).
    out.extend_from_slice(&slot.weapon_groups);
    out.push(b'\n');
    for (system, flags) in &slot.visits {
        key(&mut out, "visit", format!("{system}, {flags}").as_bytes());
    }
    out.push(b'\n');
    for entry in &slot.mission_log {
        key(&mut out, "log", format_mission_log_entry(entry).as_bytes());
    }
    out.push(b'\n');
    key(
        &mut out,
        "interface",
        slot.interface_state.to_string().as_bytes(),
    );
    out.push(b'\n');

    let client = slot.client_id;
    {
        let mut message = ControllerMessage::PlayerSave {
            client,
            file: &mut out,
        };
        let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
    }
    if single_player {
        save_single_player_state(cx, &mut out);
    }
    Some(Written {
        text: out,
        position: saved_pos,
        orientation: saved_orient,
        rank,
        relations,
    })
}

/// `Server_SaveSinglePlayerState`: `[Time]` with the game time as a
/// `float`, `Reputation::Save`'s `[Group]` sections, then the `SaveGame`
/// broadcast.
fn save_single_player_state(cx: &mut Cx<'_>, out: &mut Vec<u8>) {
    out.extend_from_slice(b"\n[Time]\n");
    let seconds = cx.logic.clock.now() as f32;
    key(out, "seconds", g(f64::from(seconds)).as_bytes());
    {
        let (_, reputation) = cx.world.vibes();
        for (_, group) in reputation.groups() {
            out.extend_from_slice(b"\n[Group]\n");
            key(out, "nickname", &group.nickname);
            for (&other, &feeling) in &group.feelings {
                let mut line = g(f64::from(feeling)).into_bytes();
                line.extend_from_slice(b", ");
                line.extend_from_slice(
                    reputation
                        .group(other)
                        .map_or(&[][..], |o| o.nickname.as_slice()),
                );
                key(out, "rep", &line);
            }
        }
    }
    let mut message = ControllerMessage::SaveGame { file: out };
    let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
}

/// What [`serialize_to_file`] did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SaveOutcome {
    /// No slot has the session.
    NoPlayer,
    /// The cheat gate refused the save.
    Refused,
    /// The file could not be opened (`*** WARNING: Failed to open %s for
    /// writing`).
    OpenFailed(PathBuf),
    /// Writing failed; the file was removed.
    WriteFailed(PathBuf),
    /// The save was written at this path (the C++'s `GServerLastSavePath`).
    Written(PathBuf),
}

/// `PlayerData_SerializeToFile(slot, file_name, description, strid)`: the
/// save of `session`'s player in its account folder under `accounts`, with
/// the extension `.fl` in single player (a multiplayer name carries its
/// own); `now` is the local time a cheat report logs. A multiplayer server writes `temp.fl` in the accounts folder and
/// renames it over the save, then refreshes the selected character's
/// record from the slot.
#[allow(clippy::too_many_arguments)]
pub fn serialize_to_file(
    cx: &mut Cx<'_>,
    session: u32,
    accounts: &AccountsRoot,
    file_name: &str,
    description: Option<&[u16]>,
    descrip_strid: u32,
    tstamp: (u32, u32),
    now: crate::cheats::LocalTime,
) -> SaveOutcome {
    let single_player = cx.world.config().single_player;
    if cx.logic.players.get(session).is_none() {
        return SaveOutcome::NoPlayer;
    }
    if crate::cheats::check_for_cheats(cx, session, accounts, now) {
        return SaveOutcome::Refused;
    }
    if prepare(cx, session).is_none() {
        return SaveOutcome::NoPlayer;
    }
    let Some(slot) = cx.logic.players.get(session) else {
        return SaveOutcome::NoPlayer;
    };
    let account_dir = accounts.account_dir(&slot.account_name);
    let save_path = if cx.logic.multiplayer_server {
        account_dir.join(file_name)
    } else {
        account_dir.join(format!("{file_name}.fl"))
    };
    let temp_path = accounts.path().join("temp.fl");
    let write_path = if single_player {
        save_path.clone()
    } else {
        temp_path.clone()
    };
    let Ok(mut file) = std::fs::File::create(&write_path) else {
        return SaveOutcome::OpenFailed(write_path);
    };
    let Some(written) = print(cx, session, description, descrip_strid, tstamp) else {
        return SaveOutcome::NoPlayer;
    };
    let ok = file.write_all(&text_mode_write(&written.text)).is_ok() && file.flush().is_ok();
    drop(file);
    if !ok {
        let _ = std::fs::remove_file(&write_path);
        return SaveOutcome::WriteFailed(write_path);
    }
    if !single_player {
        let _ = std::fs::remove_file(&save_path);
        let _ = std::fs::rename(&temp_path, &save_path);
        player::refresh_saved_record(cx, session, &written);
    }
    SaveOutcome::Written(write_path)
}
