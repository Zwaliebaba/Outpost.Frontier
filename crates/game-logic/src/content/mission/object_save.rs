//! The mission object's save sections (TU38; work package P3.5b):
//! `SaveMission`, `SaveRandEncs`, `LoadMissionSave`, `LoadRandEncsSave` and
//! `LoadVibeInfos` (`Content/MissionSave.cpp:702-1222`), `SaveNNObjectives`
//! with the objectives' `Save` slots (`Content/MissionObject.cpp:410-930`)
//! and the encounter parameters' `Dump` (`Content/RandomEncounters.cpp:
//! 365-390`, `:560-570`, `:851-910`).
//!
//! The save writes, in this order: every respawn ship record (each ship's
//! guide line or `[MObjective]` directives, its `[MsnShipSave]`, then the
//! record's `[MsnWingSave]`), every random-encounter respawn record
//! (`[MsnRandEncSave]`), `[MsnSolarSave]`, `[MarkObj]`, `[Invulnerables]`,
//! `[Decloaked]`, `[MsnVibeInfo]`, the first player's `[NNObjective]`s and
//! the random encounters' `[RandomEncounter]`s. Floats go through `%f`
//! ([`format_f`]); the C++ writes through a text-mode `FILE`, `out` gets the
//! `\n`s and the save path translates them.
//!
//! The records the save writes come from the snapshot of the live ships
//! (`SnapshotMissionObjects`, which needs the NPC ships of P3.6c and runs
//! with the mission's run time, P3.5c) or from a restored save. The restore
//! makes the guide of a ship whose save has a `[MetaBehavior]` with no
//! ship (`CreateGuide`, [`MissionShip::guide_ref`], P3.5c4) and keeps the
//! object it was made from ([`MissionShip::guide`]).

use std::io::{self, Write};

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::{IdString, create_id};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::msvcrt::format_f;

use super::meta_behavior::{self, MetaBehavior};
use super::obj_list::{self, ObjListDirective};
use super::object::{MissionObject, MissionShip, ShipGroup};
use super::objective::{self, Objective};
use super::rand_enc::{self, MissionRandEnc, RandEncKind, RandEncParams, ShipChoice};
use super::{f32_at, tstring, value_tstring};
use crate::content::tables::ship_classes;
use crate::server::Cx;

/// `GIdLocalObjective`: `CreateID("LOCAL_OBJECTIVE")`, the objectives and
/// meta behaviour of a restored ship.
#[must_use]
pub fn local_objective() -> u32 {
    create_id(b"LOCAL_OBJECTIVE")
}

/// `GPlayerLabel`.
pub const PLAYER_LABEL: &[u8] = b"Player";

/// One `[Invulnerables]` row (`FMissionInvulnerable`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Invulnerable {
    /// `Nickname` (`TString<0x30>`).
    pub nickname: Vec<u8>,
    /// `Duration`.
    pub duration: f32,
    /// `bParam3`.
    pub param3: bool,
}

/// One attitude of `[MsnVibeInfo]` (`FMissionVibeInfo`): party A towards
/// party B, each named or, with an empty name, a reputation.
#[derive(Clone, Debug, PartialEq)]
pub struct VibeInfo {
    /// `ObjectA` (`TString<0x30>`).
    pub object_a: Vec<u8>,
    /// `ObjectB`.
    pub object_b: Vec<u8>,
    /// `RepA`: used when `ObjectA` is empty.
    pub rep_a: i32,
    /// `RepB`: used when `ObjectB` is empty.
    pub rep_b: i32,
    /// `Attitude`.
    pub attitude: f32,
}

impl Default for VibeInfo {
    /// The row `LoadVibeInfos` starts: no names, both reputations -1.
    fn default() -> Self {
        Self {
            object_a: Vec::new(),
            object_b: Vec::new(),
            rep_a: -1,
            rep_b: -1,
            attitude: 0.0,
        }
    }
}

/// One random-encounter formation to create again
/// (`FMissionRandEncRespawn`, `MissionObject.h:278-303`).
#[derive(Clone, Debug, PartialEq)]
pub struct RandEncRespawn {
    /// `EncounterId`: the `[MsnRandEnc]`'s id.
    pub encounter_id: u32,
    /// `FormationRef` (-1).
    pub formation_ref: u32,
    /// `Placement` (identity).
    pub placement: Transform,
    /// `Count`.
    pub count: i32,
    /// `Choices`: the ships' classes.
    pub choices: Vec<ShipChoice>,
    /// `Names` (`TString<0x30>` each).
    pub names: Vec<Vec<u8>>,
    /// `Directives`: the leader's directives.
    pub directives: Vec<ObjListDirective>,
    /// `Guide`: the leader's guide, kept as the `[MetaBehavior]` object it
    /// was made from (none after a restore, which makes no guide here).
    pub guide: Option<MetaBehavior>,
    /// `Priority` (unset in the C++ template; 0).
    pub priority: i32,
    /// `HitPoints` (-1).
    pub hit_points: i32,
}

impl Default for RandEncRespawn {
    fn default() -> Self {
        Self {
            encounter_id: 0,
            formation_ref: u32::MAX,
            placement: Transform::IDENTITY,
            count: 0,
            choices: Vec::new(),
            names: Vec::new(),
            directives: Vec::new(),
            guide: None,
            priority: 0,
            hit_points: -1,
        }
    }
}

fn f(value: f32) -> String {
    format_f(f64::from(value), 6)
}

/// `PrintOrientation`: the nine floats in memory order.
fn write_orientation(out: &mut dyn Write, m: &Matrix) -> io::Result<()> {
    let r = &m.m;
    writeln!(
        out,
        "orientation = {},{},{}, {},{},{}, {},{},{}",
        f(r[0][0]),
        f(r[0][1]),
        f(r[0][2]),
        f(r[1][0]),
        f(r[1][1]),
        f(r[1][2]),
        f(r[2][0]),
        f(r[2][1]),
        f(r[2][2])
    )
}

/// `SaveGuideOrDirectives`: a ship with a guide writes `[MetaBehavior]` and
/// the guide's line, which the C++ passes to `fprintf` as the format (D831:
/// the port writes it unchanged); a ship without one writes its directives
/// under `[MObjective]`, each its line (`FObjListDirective::Save`, nothing
/// for an empty one).
fn write_guide_or_directives(
    out: &mut dyn Write,
    guide_line: Option<&[u8]>,
    directives: &[ObjListDirective],
) -> io::Result<()> {
    if let Some(line) = guide_line {
        out.write_all(b"\n[MetaBehavior]\n")?;
        return out.write_all(line);
    }
    if directives.is_empty() {
        return Ok(());
    }
    out.write_all(b"\n[MObjective]\n")?;
    for directive in directives {
        if !directive.name.is_empty() {
            out.write_all(&directive.name)?;
            out.write_all(b"\n")?;
        }
    }
    Ok(())
}

/// The line of a ship slot's guide (`FLineGuide::Line`): its live
/// guide's, else the `[MetaBehavior]` object's it is kept as; `None`
/// without a guide. A live guide of another class has none (D1041).
fn ship_guide_line<'a>(cx: &'a Cx<'_>, ship: &'a MissionShip) -> Option<&'a [u8]> {
    if let Some(guide) = ship.guide_ref {
        return Some(crate::content::guides::line(cx, guide).unwrap_or_default());
    }
    ship.guide.as_ref().map(|m| m.guide().line.as_slice())
}

fn write_ship(cx: &Cx<'_>, out: &mut dyn Write, ship: &MissionShip) -> io::Result<()> {
    write_guide_or_directives(out, ship_guide_line(cx, ship), &ship.directives)?;
    out.write_all(b"\n[MsnShipSave]\n")?;
    out.write_all(b"nickname = ")?;
    out.write_all(&ship.nickname)?;
    out.write_all(b"\n")?;
    writeln!(out, "npc = {}", ship.npc_id)?;
    writeln!(out, "random_name = {}", i32::from(ship.random_name))?;
    writeln!(out, "ol_priority = {}", ship.priority)?;
    writeln!(out, "jumper = {}", i32::from(ship.jumper))?;
    writeln!(
        out,
        "original_jump_flag = {}",
        i32::from(ship.original_jump_flag)
    )?;
    writeln!(out, "hit_pts = {}", ship.hit_points)?;
    writeln!(out, "radius = {}", f(ship.radius))?;
    for label in &ship.label_ids {
        writeln!(out, "label = {label}")?;
    }
    Ok(())
}

fn write_wing(cx: &Cx<'_>, out: &mut dyn Write, group: &ShipGroup) -> io::Result<()> {
    for ship in &group.ships {
        write_ship(cx, out, ship)?;
    }
    out.write_all(b"\n[MsnWingSave]\n")?;
    out.write_all(b"nickname = ")?;
    out.write_all(&group.nickname)?;
    out.write_all(b"\n")?;
    if group.arrival_obj_id & 0x3fff_ffff == 0 {
        let p = group.position;
        writeln!(out, "position = {}, {}, {}", f(p.x), f(p.y), f(p.z))?;
        write_orientation(out, &group.orientation)?;
    } else {
        writeln!(out, "arrival_obj = {}", group.arrival_obj_id)?;
    }
    writeln!(out, "formation = {}", group.formation)?;
    for ship in &group.ships {
        out.write_all(b"ship = ")?;
        out.write_all(&ship.nickname)?;
        out.write_all(b"\n")?;
    }
    Ok(())
}

fn write_rand_enc_respawn(out: &mut dyn Write, record: &RandEncRespawn) -> io::Result<()> {
    let line = record.guide.as_ref().map(|m| m.guide().line.as_slice());
    write_guide_or_directives(out, line, &record.directives)?;
    out.write_all(b"\n[MsnRandEncSave]\n")?;
    writeln!(out, "rand_enc_owner = {}", record.encounter_id)?;
    let t = record.placement.translation;
    writeln!(out, "position = {}, {}, {}", f(t.x), f(t.y), f(t.z))?;
    write_orientation(out, &record.placement.rotation)?;
    writeln!(out, "num_ships = {}", record.count)?;
    writeln!(out, "ol_priority = {}", record.priority)?;
    writeln!(out, "hit_pts = {}", record.hit_points)?;
    for choice in &record.choices {
        out.write_all(b"ship_type = ")?;
        for class in &choice.ship_classes {
            write!(out, "{class}, ")?;
        }
        out.write_all(b"END\n")?;
    }
    out.write_all(b"names = ")?;
    for name in &record.names {
        out.write_all(name)?;
        out.write_all(b", ")?;
    }
    out.write_all(b"END\n")
}

fn write_names(
    out: &mut dyn Write,
    header: &[u8],
    key: &[u8],
    names: &[Vec<u8>],
) -> io::Result<()> {
    if !names.is_empty() {
        out.write_all(header)?;
    }
    for name in names {
        out.write_all(key)?;
        out.write_all(name)?;
        out.write_all(b"\n")?;
    }
    Ok(())
}

/// The name `[MsnVibeInfo]` writes for an empty party.
const EMPTY: &[u8] = b"empty";

fn write_vibes(out: &mut dyn Write, vibes: &[VibeInfo]) -> io::Result<()> {
    if vibes.is_empty() {
        return Ok(());
    }
    out.write_all(b"\n[MsnVibeInfo]\n")?;
    for vibe in vibes {
        let a: &[u8] = if vibe.object_a.is_empty() {
            EMPTY
        } else {
            &vibe.object_a
        };
        let b: &[u8] = if vibe.object_b.is_empty() {
            EMPTY
        } else {
            &vibe.object_b
        };
        out.write_all(b"vibe = ")?;
        out.write_all(a)?;
        out.write_all(b", ")?;
        out.write_all(b)?;
        writeln!(
            out,
            ", {}, {}, {}",
            vibe.rep_a as u32,
            vibe.rep_b as u32,
            f(vibe.attitude)
        )?;
    }
    Ok(())
}

/// The objective's `Save` slot: the rest of its `type` line.
fn write_objective_values(out: &mut dyn Write, o: &Objective) -> io::Result<()> {
    let x = f32::from_bits(o.marker[2]);
    let y = f32::from_bits(o.marker[3]);
    let z = f32::from_bits(o.marker[4]);
    match o.kind {
        1 => write!(out, "{}, {}", o.base_id, o.ids),
        2 => write!(
            out,
            "{}, {}, {}, {}, {}, {}",
            o.marker[0],
            o.ids,
            o.marker[1] as i32,
            f(x),
            f(y),
            f(z)
        ),
        3 => write!(out, "{}", o.ids),
        4 => {
            write!(
                out,
                "{}, {}, {}, {}, {}, {}, ",
                o.marker[0],
                o.ids,
                o.marker[1] as i32,
                f(x),
                f(y),
                f(z)
            )?;
            out.write_all(&o.label)
        }
        // `SaveType56`: the position as x, y, z; `ParseType56` reads it
        // back reversed, as the C++ does.
        5 | 6 => write!(
            out,
            "{}, {}, {}, {}, {}, {}, {}, {} END",
            o.ids,
            o.system_id,
            o.rep_instance as u32,
            o.marker[1] as i32,
            o.marker[0],
            f(x),
            f(y),
            f(z)
        ),
        _ => Ok(()),
    }
}

/// `SaveNNObjectives(File, List)` (`MissionObject.cpp:917-931`).
///
/// # Errors
///
/// The writer's.
pub fn write_objectives(out: &mut dyn Write, objectives: &[Objective]) -> io::Result<()> {
    for o in objectives {
        out.write_all(b"[NNObjective]\n")?;
        writeln!(out, "nickname = {}", o.nickname)?;
        write!(out, "type = {}, ", o.kind)?;
        write_objective_values(out, o)?;
        out.write_all(b"\n")?;
        writeln!(out, "state = {}", o.state as i32)?;
        writeln!(out, "hidden = {}", i32::from(o.hidden))?;
        out.write_all(b"\n")?;
    }
    Ok(())
}

/// The `formation` lines of both dumps (`DumpRandEncFormations`): the
/// nickname as `ID_String::get_string` writes it (`0x` and hex) for the
/// space attack, and for the lane attack the pointer the C++ passes to
/// `%u`, which the port replaces by the formation's id (D830); then each
/// choice's class (`FindShipClassByMembers`, 0 when none has these members)
/// and weight.
fn write_formations(cx: &Cx<'_>, out: &mut dyn Write, params: &RandEncParams) -> io::Result<()> {
    let classes = &cx.logic.content.tables.ship_classes;
    for formation in &params.formations {
        match params.kind {
            RandEncKind::TradeLane => write!(out, "saved_formation = {}", formation.nickname)?,
            RandEncKind::Space => write!(
                out,
                "formation = {}",
                IdString(formation.nickname).to_hex_string()
            )?,
        }
        write!(
            out,
            ", {}, {}, {}",
            f(formation.field_0x08),
            formation.min_count,
            formation.max_count
        )?;
        for choice in &formation.ship_choices {
            let class = ship_classes::find_class_by_members(classes, &choice.ship_classes);
            write!(out, ", {}, {}", class.unwrap_or(0), f(choice.weight))?;
        }
        out.write_all(b"\n")?;
    }
    Ok(())
}

/// `DumpRandEncNumForms`: only with a minimum; a zero maximum becomes the
/// number of formations first (the dump changes the parameters).
fn write_num_forms(out: &mut dyn Write, params: &mut RandEncParams) -> io::Result<()> {
    if params.num_forms_min == 0 {
        return Ok(());
    }
    if params.num_forms_max == 0 {
        params.num_forms_max = i32::try_from(params.formations.len()).unwrap_or(i32::MAX);
    }
    writeln!(
        out,
        "num_forms = {}, {}",
        params.num_forms_min, params.num_forms_max
    )
}

/// `FRandEncTLParams::Dump` / `FRandEncSPParams::Dump`.
///
/// # Errors
///
/// The writer's.
pub fn dump_params(cx: &Cx<'_>, out: &mut dyn Write, params: &mut RandEncParams) -> io::Result<()> {
    match params.kind {
        RandEncKind::TradeLane => {
            out.write_all(b"encounter_type = TL_Attack\n")?;
            writeln!(out, "tl_attack_chance = {}", params.tl_attack_chance)?;
            writeln!(
                out,
                "tl_attack_chance_read_in = {}",
                params.tl_attack_chance_read_in
            )?;
            if !params.formations.is_empty() {
                write_formations(cx, out, params)?;
            }
            write_num_forms(out, params)?;
            if params.target_tradelane != 0 {
                writeln!(out, "target_tradelane = {}", params.target_tradelane)?;
            }
            if !params.target_ship_name.is_empty() {
                out.write_all(b"target_ship_name = ")?;
                out.write_all(&params.target_ship_name)?;
                out.write_all(b"\n")?;
            }
        }
        RandEncKind::Space => {
            out.write_all(b"encounter_type = SP_Attack\n")?;
            match params.activation_type {
                2 => writeln!(
                    out,
                    "activation_type = time, {}, {}",
                    f(params.activation_time[1]),
                    f(params.activation_time[0])
                )?,
                1 => {
                    let l = params.activation_location;
                    writeln!(
                        out,
                        "activation_type = location, {}, {}, {}, {}",
                        f(l.x),
                        f(l.y),
                        f(l.z),
                        params.activation_range
                    )?;
                }
                _ => writeln!(
                    out,
                    "activation_type = time_cumulative, {}",
                    f(params.cumulative_time)
                )?,
            }
            if !params.target_ship_name.is_empty() {
                out.write_all(b"target_ship_name = ")?;
                out.write_all(&params.target_ship_name)?;
                out.write_all(b"\n")?;
            }
            if !params.formations.is_empty() {
                write_formations(cx, out, params)?;
            }
            write_num_forms(out, params)?;
            for (position, _) in &params.positions {
                writeln!(
                    out,
                    "formation_position = {}, {}, {}",
                    f(position.x),
                    f(position.y),
                    f(position.z)
                )?;
            }
        }
    }
    if params.attacker_rep != -1 {
        writeln!(out, "attacker_rep = {}", params.attacker_rep)?;
    }
    if params.pilot_id != 0 {
        writeln!(out, "pilot_id = {}", params.pilot_id)?;
    }
    Ok(())
}

/// `SaveRandEncs(File)` (`MissionSave.cpp:836-847`): each random
/// encounter's nickname, state and parameters. The C++ calls `Dump`
/// through null for an encounter without parameters; the port writes none
/// (D832).
///
/// # Errors
///
/// The writer's.
pub fn write_rand_encs(
    cx: &Cx<'_>,
    out: &mut dyn Write,
    rand_encs: &mut [MissionRandEnc],
) -> io::Result<()> {
    for record in rand_encs {
        out.write_all(b"[RandomEncounter]\n")?;
        out.write_all(b"nickname = ")?;
        out.write_all(&record.nickname)?;
        out.write_all(b"\n")?;
        writeln!(
            out,
            "enabled = {}, {}",
            i32::from(record.enabled),
            record.npc_count as i32
        )?;
        if let Some(params) = record.params.as_mut() {
            dump_params(cx, out, params)?;
        }
        out.write_all(b"\n")?;
    }
    Ok(())
}

/// `SaveMission(File)` (`MissionSave.cpp:702-833`), then `SaveRandEncs`:
/// the mission object's part of the `PlayerSave` body. The first player's
/// objectives are written through a record the C++ does not test; without
/// one the port writes none (D832). The parameters' dumps may set their
/// maximum formation count ([`dump_params`]).
///
/// # Errors
///
/// The writer's.
pub fn write(cx: &Cx<'_>, out: &mut dyn Write, object: &mut MissionObject) -> io::Result<()> {
    for group in &object.respawn_ships {
        write_wing(cx, out, group)?;
    }
    for record in &object.rand_enc_respawns {
        write_rand_enc_respawn(out, record)?;
    }
    write_names(
        out,
        b"\n[MsnSolarSave]\n",
        b"solar_name = ",
        &object.respawn_solars,
    )?;
    write_names(out, b"\n[MarkObj]\n", b"name = ", &object.mark_obj_names)?;
    if !object.invulnerables.is_empty() {
        out.write_all(b"\n[Invulnerables]\n")?;
    }
    for row in &object.invulnerables {
        out.write_all(b"invulnerable = ")?;
        out.write_all(&row.nickname)?;
        writeln!(out, ", {}, {}", f(row.duration), i32::from(row.param3))?;
    }
    write_names(out, b"\n[Decloaked]\n", b"decloaked= ", &object.decloaked)?;
    write_vibes(out, &object.vibe_infos)?;
    if let Some(player) = object.players.values().next() {
        write_objectives(out, &player.objectives)?;
    }
    write_rand_encs(cx, out, &mut object.rand_encs)
}

/// `ReadOrientation`: values 0 to 8 in memory order.
fn read_orientation(reader: &mut IniReader) -> Matrix {
    let mut m = Matrix::IDENTITY;
    for (index, slot) in m.m.iter_mut().flatten().enumerate() {
        *slot = f32_at(reader, u32::try_from(index).unwrap_or(0));
    }
    m
}

/// `IsEndMarker`: the value as a `TString<0x40>` is `END`.
fn is_end(reader: &IniReader, index: u32) -> bool {
    value_tstring(reader, index, 0x40) == b"END"
}

/// The `[MsnShipSave]` values into a new ship slot (the template's values).
fn read_ship(reader: &mut IniReader) -> MissionShip {
    let mut ship = MissionShip::default();
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            ship.nickname = value_tstring(reader, 0, 0x30);
        } else if reader.is_value(b"npc") {
            ship.npc_id = reader.value_int(0) as u32;
        } else if reader.is_value(b"random_name") {
            ship.random_name = reader.value_bool(0);
        } else if reader.is_value(b"jumper") {
            ship.jumper = reader.value_bool(0);
        } else if reader.is_value(b"original_jump_flag") {
            ship.original_jump_flag = reader.value_bool(0);
        } else if reader.is_value(b"label") {
            ship.label_ids.push(reader.value_int(0) as u32);
        } else if reader.is_value(b"ol_priority") {
            ship.priority = reader.value_int(0);
        } else if reader.is_value(b"hit_pts") {
            ship.hit_points = reader.value_int(0);
        } else if reader.is_value(b"radius") {
            ship.radius = f32_at(reader, 0);
        }
    }
    ship
}

/// The `[MsnRandEncSave]` values into a new record.
fn read_rand_enc_respawn(reader: &mut IniReader) -> RandEncRespawn {
    let mut record = RandEncRespawn::default();
    while reader.read_value() {
        if reader.is_value(b"rand_enc_owner") {
            record.encounter_id = reader.value_int(0) as u32;
        } else if reader.is_value(b"position") {
            record.placement.translation =
                Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2));
        } else if reader.is_value(b"ol_priority") {
            record.priority = reader.value_int(0);
        } else if reader.is_value(b"orientation") {
            record.placement.rotation = read_orientation(reader);
        } else if reader.is_value(b"hit_pts") {
            record.hit_points = reader.value_int(0);
        } else if reader.is_value(b"num_ships") {
            record.count = reader.value_int(0);
        } else if reader.is_value(b"ship_type") {
            // Up to 0x20 values until `END`, each class once; a missing
            // value reads as class 0.
            let mut classes = Vec::new();
            for index in 0..0x20 {
                if is_end(reader, index) {
                    break;
                }
                let class = reader.value_int(index) as u32;
                if !classes.contains(&class) {
                    classes.push(class);
                }
            }
            record.choices.push(ShipChoice {
                ship_classes: classes,
                weight: 1.0,
            });
        } else if reader.is_value(b"names") {
            for index in 0..0x20 {
                if is_end(reader, index) {
                    break;
                }
                record.names.push(value_tstring(reader, index, 0x30));
            }
        }
    }
    record
}

/// The `[MsnWingSave]` values into a new record (the parse's template).
fn read_wing(reader: &mut IniReader) -> ShipGroup {
    let mut group = ShipGroup::default();
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            group.nickname = value_tstring(reader, 0, 0x30);
        } else if reader.is_value(b"position") {
            group.position = Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2));
        } else if reader.is_value(b"orientation") {
            group.orientation = read_orientation(reader);
        } else if reader.is_value(b"arrival_obj") {
            group.arrival_obj_id = reader.value_int(0) as u32;
        } else if reader.is_value(b"formation") {
            group.formation = reader.value_int(0) as u32;
        }
    }
    group
}

/// `[Invulnerables]`: values 2, 1, then 0 read.
fn read_invulnerables(reader: &mut IniReader, rows: &mut Vec<Invulnerable>) {
    while reader.read_value() {
        if !reader.is_value(b"invulnerable") {
            continue;
        }
        let param3 = reader.value_bool(2);
        let duration = f32_at(reader, 1);
        rows.push(Invulnerable {
            nickname: value_tstring(reader, 0, 0x30),
            duration,
            param3,
        });
    }
}

/// The values of a name list section (`[MsnSolarSave]`, `[MarkObj]`,
/// `[Decloaked]`): each `key` line's value 0 as a `TString<0x30>`.
fn read_names(reader: &mut IniReader, key: &[u8], names: &mut Vec<Vec<u8>>) {
    while reader.read_value() {
        if reader.is_value(key) {
            names.push(value_tstring(reader, 0, 0x30));
        }
    }
}

/// `AddUniqueRep`.
fn add_unique_rep(reps: &mut Vec<i32>, rep: i32) {
    if !reps.contains(&rep) {
        reps.push(rep);
    }
}

/// `LoadVibeInfos(Reader)` (`MissionSave.cpp:1199-1222`): one row per
/// `vibe`; a name `empty` (`_stricmp`) leaves the party unnamed and adds
/// its reputation (value 2 or 3) to the solar reputations once.
pub fn load_vibe_infos(reader: &mut IniReader, object: &mut MissionObject) {
    while reader.read_value() {
        if !reader.is_value(b"vibe") {
            continue;
        }
        let mut row = VibeInfo::default();
        if eq_ignore_case(c_str(&reader.value_string_at(0)), EMPTY) {
            add_unique_rep(&mut object.solar_reps, reader.value_int(2));
        } else {
            row.object_a = value_tstring(reader, 0, 0x30);
        }
        if eq_ignore_case(c_str(&reader.value_string_at(1)), EMPTY) {
            add_unique_rep(&mut object.solar_reps, reader.value_int(3));
        } else {
            row.object_b = value_tstring(reader, 1, 0x30);
        }
        row.rep_a = reader.value_int(2);
        row.rep_b = reader.value_int(3);
        row.attitude = f32_at(reader, 4);
        object.vibe_infos.push(row);
    }
}

/// The nickname `LoadRandEncsSave` keeps between values: `strncpy` of 0x20
/// bytes, cut at the first NUL. A nickname of 0x20 bytes or more has no
/// terminator in the C++ (D833).
fn rand_enc_nickname(text: &[u8]) -> Vec<u8> {
    let mut text = c_str(text).to_vec();
    text.truncate(0x20);
    text
}

/// `LoadRandEncsSave(Reader)` (`MissionSave.cpp:1124-1195`): the values of
/// one `[RandomEncounter]` of the save.
///
/// - `nickname` names the encounter the next values are for.
/// - `enabled` (not for `RandomMission`) sets every encounter of that name
///   (`_stricmp`): enabled clears its ship count, disabled takes value 1
///   when there is one.
/// - `encounter_type` (only for `RandomMission`) appends a new disabled
///   encounter of that name; `TL_Attack` and `SP_Attack` (`strcmp`) get new
///   parameters read by their populator from the values after the line
///   (which may draw `rand()`), with the mission's controller and player.
///   The C++ sets those through null for another type; the port appends the
///   encounter without parameters (D832).
pub fn load_rand_encs(cx: &mut Cx<'_>, reader: &mut IniReader, object: &mut MissionObject) {
    let mut nickname: Vec<u8> = Vec::new();
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            nickname = rand_enc_nickname(&reader.value_string_at(0));
        } else if reader.is_value(b"enabled") {
            if eq_ignore_case(&nickname, b"RandomMission") {
                continue;
            }
            for record in &mut object.rand_encs {
                if !eq_ignore_case(&record.nickname, &nickname) {
                    continue;
                }
                record.enabled = reader.value_bool(0);
                if record.enabled {
                    record.npc_count = 0;
                } else if !reader.is_value_empty(1) {
                    record.npc_count = reader.value_int(1) as u32;
                }
            }
        } else if reader.is_value(b"encounter_type") {
            if !eq_ignore_case(&nickname, b"RandomMission") {
                continue;
            }
            let kind = reader.value_string_at(0);
            let kind = match c_str(&kind) {
                b"TL_Attack" => Some(RandEncKind::TradeLane),
                b"SP_Attack" => Some(RandEncKind::Space),
                _ => None,
            };
            let params = kind.map(|kind| {
                let mut params = RandEncParams::new(kind);
                rand_enc::populator_parse(cx, reader, &mut params);
                params.controller = object.controller.0;
                params.player = object.player_state.then_some(object.player);
                params
            });
            object.rand_encs.push(MissionRandEnc {
                nickname: tstring(&nickname, 0x20),
                enabled: false,
                nickname_id: 0,
                formation_index: 0,
                params,
                npc_count: 0,
                arms: Vec::new(),
            });
        }
    }
}

/// `LoadMissionSave(Reader)` (`MissionSave.cpp:891-1118`): the mission
/// object's sections of the save, from the reader's start.
///
/// - `[MObjective]` sections append their directives to a pending list
///   and `[MetaBehavior]` sections their objects to a second one (the
///   nicknames are dropped).
/// - `[MsnShipSave]` makes a ship slot that waits for the next
///   `[MsnWingSave]`: it takes the pending directives with the local
///   objective id, or, once any `[MetaBehavior]` was read, the first
///   object of that list as its guide (the list is never emptied, so every
///   later ship gets the same guide); the pending directives are dropped.
/// - `[MsnRandEncSave]` appends a record with the pending directives, which
///   are then dropped.
/// - `[MsnWingSave]` appends a ship record that takes the waiting slots.
/// - `[MsnSolarSave]`, `[MarkObj]`, `[Invulnerables]`, `[Decloaked]` and
///   `[MsnVibeInfo]` append to their lists; `[RandomEncounter]` is
///   [`load_rand_encs`].
pub fn load(cx: &mut Cx<'_>, reader: &mut IniReader, object: &mut MissionObject) {
    reader.reset();
    let mut pending_ships: Vec<MissionShip> = Vec::new();
    let mut objectives: Vec<ObjListDirective> = Vec::new();
    let mut metas: Vec<MetaBehavior> = Vec::new();
    while reader.read_header() {
        if reader.is_header(b"RandomEncounter") {
            load_rand_encs(cx, reader, object);
        } else if reader.is_header(b"MObjective") {
            let (_, list) = obj_list::load(cx, reader);
            objectives.extend(list);
        } else if reader.is_header(b"MetaBehavior") {
            let mut nickname = 0;
            metas.extend(meta_behavior::load(reader, &mut nickname));
        } else if reader.is_header(b"MsnShipSave") {
            let mut ship = read_ship(reader);
            ship.init_objectives = local_objective();
            ship.directives = std::mem::take(&mut objectives);
            if let Some(first) = metas.first() {
                ship.directives.clear();
                ship.init_meta_behavior = local_objective();
                ship.guide = Some(first.clone());
                // `CreateGuide(MetaList.front(), &Ship.Guide)`: a guide of
                // no ship, which the respawn gives the ship (P3.5c4).
                let desc = crate::content::guides::GuideDesc::from_meta(first.clone());
                ship.guide_ref = crate::content::guides::create_detached_guide(cx, &desc);
            }
            pending_ships.push(ship);
        } else if reader.is_header(b"MsnRandEncSave") {
            let mut record = read_rand_enc_respawn(reader);
            record.directives = std::mem::take(&mut objectives);
            object.rand_enc_respawns.push(record);
        } else if reader.is_header(b"MsnWingSave") {
            let mut group = read_wing(reader);
            group.ships = std::mem::take(&mut pending_ships);
            object.respawn_ships.push(group);
        } else if reader.is_header(b"MsnSolarSave") {
            read_names(reader, b"solar_name", &mut object.respawn_solars);
        } else if reader.is_header(b"MarkObj") {
            read_names(reader, b"name", &mut object.mark_obj_names);
        } else if reader.is_header(b"Invulnerables") {
            read_invulnerables(reader, &mut object.invulnerables);
        } else if reader.is_header(b"Decloaked") {
            read_names(reader, b"decloaked", &mut object.decloaked);
        } else if reader.is_header(b"MsnVibeInfo") {
            load_vibe_infos(reader, object);
        }
    }
}

/// `LoadNNObjective` into the first player's objectives, as
/// `LoadMissionState` calls it (none without a player record, D813).
pub fn load_objective(cx: &mut Cx<'_>, reader: &mut IniReader, object: &mut MissionObject) {
    let owner = cx
        .logic
        .content
        .state
        .players
        .contains_key(&object.player)
        .then_some(object.player);
    let parsed = objective::load(cx, reader);
    if let (Some(mut o), Some(player)) = (parsed, object.players.values_mut().next()) {
        o.owner = owner;
        player.objectives.push(o);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(f: impl FnOnce(&mut Vec<u8>) -> io::Result<()>) -> String {
        let mut out = Vec::new();
        f(&mut out).unwrap();
        String::from_utf8(out).unwrap()
    }

    #[test]
    fn objectives_write_their_type_lines() {
        let mut base = Objective::new(1);
        base.nickname = 7;
        base.base_id = 12;
        base.ids = -3;
        base.state = 10;
        let mut marker = Objective::new(5);
        marker.ids = 5;
        marker.system_id = 9;
        marker.marker = [
            1,
            2,
            1.5f32.to_bits(),
            0.0f32.to_bits(),
            (-2.0f32).to_bits(),
        ];
        marker.hidden = true;
        let out = text(|o| write_objectives(o, &[base, marker]));
        assert_eq!(
            out,
            "[NNObjective]\nnickname = 7\ntype = 1, 12, -3\nstate = 10\nhidden = 0\n\n\
             [NNObjective]\nnickname = 0\ntype = 5, 5, 9, 4294967295, 2, 1, 1.500000, \
             0.000000, -2.000000 END\nstate = 4\nhidden = 1\n\n"
        );
    }

    #[test]
    fn vibes_write_empty_names() {
        let vibes = [VibeInfo {
            object_b: b"Player".to_vec(),
            rep_a: 17,
            attitude: -0.6,
            ..VibeInfo::default()
        }];
        let out = text(|o| write_vibes(o, &vibes));
        assert_eq!(
            out,
            "\n[MsnVibeInfo]\nvibe = empty, Player, 17, 4294967295, -0.600000\n"
        );
    }

    #[test]
    fn a_rand_enc_respawn_lists_end_with_end() {
        let record = RandEncRespawn {
            encounter_id: 3,
            count: 2,
            choices: vec![
                ShipChoice {
                    ship_classes: vec![17, 34],
                    weight: 1.0,
                },
                ShipChoice::default(),
            ],
            names: vec![b"a".to_vec(), b"b".to_vec()],
            ..RandEncRespawn::default()
        };
        let out = text(|o| write_rand_enc_respawn(o, &record));
        assert_eq!(
            out,
            "\n[MsnRandEncSave]\nrand_enc_owner = 3\nposition = 0.000000, 0.000000, 0.000000\n\
             orientation = 1.000000,0.000000,0.000000, 0.000000,1.000000,0.000000, \
             0.000000,0.000000,1.000000\nnum_ships = 2\nol_priority = 0\nhit_pts = -1\n\
             ship_type = 17, 34, END\nship_type = END\nnames = a, b, END\n"
        );
    }
}
