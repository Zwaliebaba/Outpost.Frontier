//! The `[MsnRandEnc]` sections (TU38, `MissionObject.cpp:4170-4229`) and
//! the random-encounter parameters their populators read (TU31,
//! `Content/RandomEncounters.cpp`: `FRandEncSPPopulator::Parse`,
//! `:1471-1590`, `FRandEncTLPopulator::Parse`, `:1142-1240`, and
//! `ParseRandEncFormationValues`, `:524-556`).
//!
//! The parse draws `rand()`: a `time` activation picks its start, and
//! `num_forms` the number of formations. The populators' run time (`Accept`,
//! `Spawn`) comes with the population (P3.6d).

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::msvcrt::atoi;
use neuron_core::num::trunc_to_i32;
use neuron_server::controller::ControllerId;

use super::{f32_at, nickname_id, tstring, value_tstring};
use crate::content::tables;
use crate::server::Cx;

/// `GRandEncDegToRad` (`RandomEncounters.cpp:392`).
const DEG_TO_RAD: f32 = 0.017_453_292;

/// `GIdMissionRandomEncounter`: `CreateID("Mission_Random_Encounter")`, the
/// label every mission encounter's ships get.
pub const MISSION_RANDOM_ENCOUNTER: &[u8] = b"Mission_Random_Encounter";

/// The two parameter classes (`CreateRandEncParams`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RandEncKind {
    /// `TLParams` with `TLPzr` (`TL_Attack`): type 3.
    TradeLane,
    /// `SPParams` with `SPPzr` (`SP_Attack`): type 2.
    Space,
}

impl RandEncKind {
    /// `GetType`.
    #[must_use]
    pub fn code(self) -> i32 {
        match self {
            Self::TradeLane => 3,
            Self::Space => 2,
        }
    }
}

/// One ship-class choice of a formation (`FRandEncShipChoice`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ShipChoice {
    /// `ShipClasses`: the members of the class.
    pub ship_classes: Vec<u32>,
    /// `Weight`.
    pub weight: f32,
}

/// One `formation` line (`FRandEncFormation`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RandEncFormation {
    /// `Formation`: the formation of value 0 (its key), none when unknown.
    pub formation: Option<u32>,
    /// `Nickname`: value 0's id.
    pub nickname: u32,
    /// `Field_0x08`: value 1.
    pub field_0x08: f32,
    /// `MinCount`: value 2.
    pub min_count: i32,
    /// `MaxCount`: value 3.
    pub max_count: i32,
    /// `Count` (run time).
    pub count: i32,
    /// `ShipChoices`: the class and weight pairs from value 4.
    pub ship_choices: Vec<ShipChoice>,
    /// `Weight`: value 1 again.
    pub weight: f32,
}

/// The parameters of one encounter (`FRandEncParams` with
/// `FRandEncSPParams` or `FRandEncTLParams`, `RandomEncounters.h:72-210`);
/// the fields of the other class keep their defaults and are not dumped.
#[derive(Clone, Debug, PartialEq)]
pub struct RandEncParams {
    /// The class.
    pub kind: RandEncKind,
    /// `System` (none after the parse).
    pub system: Option<u32>,
    /// `ControllerId`: the mission's.
    pub controller: u32,
    /// `Player`: the mission's player state (its player).
    pub player: Option<u32>,
    /// `AttackerRep` (-1).
    pub attacker_rep: i32,
    /// `PilotId`.
    pub pilot_id: u32,
    /// `Labels`: the section's `label` ids, then the encounter's id and
    /// [`MISSION_RANDOM_ENCOUNTER`].
    pub labels: Vec<u32>,
    /// `Positions`: `formation_position` and its weight.
    pub positions: Vec<(Vector, f32)>,
    /// `ShipsSpawned`.
    pub ships_spawned: i32,
    /// `FormationsToSpawn`: drawn by `num_forms`.
    pub formations_to_spawn: i32,
    /// `NumFormsMin`.
    pub num_forms_min: i32,
    /// `NumFormsMax`.
    pub num_forms_max: i32,
    /// `Formations`.
    pub formations: Vec<RandEncFormation>,
    /// `TargetShipName`.
    pub target_ship_name: Vec<u8>,
    /// `GotoTargetId`.
    pub goto_target_id: u32,
    /// `TargetObjectId`.
    pub target_object_id: u32,
    /// `DockTarget`: `arrival_obj`.
    pub dock_target: u32,
    /// `SpawnAngle` (pi / 2).
    pub spawn_angle: f32,
    /// `SpawnDistance` (2500).
    pub spawn_distance: f32,
    /// `EmptyPosRadius` (7).
    pub empty_pos_radius: i32,
    /// `ShipNameCounter`.
    pub ship_name_counter: i32,
    /// SP: `ActivationType` (0 time_cumulative, 1 location, 2 time).
    pub activation_type: i32,
    /// SP: `ActivationLocation`.
    pub activation_location: Vector,
    /// SP: `ActivationRange` (6250000).
    pub activation_range: i32,
    /// SP: `ActivationTime` (300, 180).
    pub activation_time: [f32; 2],
    /// SP: `CumulativeTime`, drawn by a `time` activation.
    pub cumulative_time: f32,
    /// TL: `TlAttackChance` (20).
    pub tl_attack_chance: i32,
    /// TL: `TlAttackChanceReadIn` (20).
    pub tl_attack_chance_read_in: i32,
    /// TL: `Direction` (1).
    pub direction: i32,
    /// TL: `EntryRing`.
    pub entry_ring: u32,
    /// TL: `ExitRing`.
    pub exit_ring: u32,
    /// TL: `TargetTradelane`.
    pub target_tradelane: u32,
}

impl RandEncParams {
    /// `CreateRandEncParams`: the constructor's values.
    #[must_use]
    pub fn new(kind: RandEncKind) -> Self {
        Self {
            kind,
            system: None,
            controller: 0,
            player: None,
            attacker_rep: -1,
            pilot_id: 0,
            labels: Vec::new(),
            positions: Vec::new(),
            ships_spawned: 0,
            formations_to_spawn: 0,
            num_forms_min: 0,
            num_forms_max: 0,
            formations: Vec::new(),
            target_ship_name: Vec::new(),
            goto_target_id: 0,
            target_object_id: 0,
            dock_target: 0,
            spawn_angle: 1.570_796_4,
            spawn_distance: 2500.0,
            empty_pos_radius: 7,
            ship_name_counter: 0,
            activation_type: 0,
            activation_location: Vector::ZERO,
            activation_range: 6_250_000,
            activation_time: [300.0, 180.0],
            cumulative_time: 0.0,
            tl_attack_chance: 20,
            tl_attack_chance_read_in: 20,
            direction: 1,
            entry_ring: 0,
            exit_ring: 0,
            target_tradelane: 0,
        }
    }
}

/// One `[MsnRandEnc]` (`FMissionRandEnc`, `MissionObject.h:228`).
#[derive(Clone, Debug, PartialEq)]
pub struct MissionRandEnc {
    /// `nickname` (`TString<0x20>`).
    pub nickname: Vec<u8>,
    /// `bEnabled` (0).
    pub enabled: bool,
    /// `NicknameId`: `CreateID` of `nickname`.
    pub nickname_id: u32,
    /// `FormationIndex` (run time).
    pub formation_index: u32,
    /// `Params` and the populator of `encounter_type`; none for another
    /// type or without one.
    pub params: Option<RandEncParams>,
    /// `NpcCount` (run time).
    pub npc_count: u32,
}

/// `rand() * (max - min + 1) / 32768 + min` in `int`.
fn pick(cx: &mut Cx<'_>, min: i32, max: i32) -> i32 {
    cx.world
        .rng()
        .rand()
        .wrapping_mul(max.wrapping_sub(min).wrapping_add(1))
        / 32768
        + min
}

/// `ParseRandEncFormationValues`: value 1 the weight, 2 and 3 the count
/// range, then class and weight pairs from value 4; a class without members
/// is skipped, as (lane rules) is one without a weight. The formation goes
/// to the parameters' list.
fn formation_values(
    cx: &Cx<'_>,
    reader: &mut IniReader,
    params: &mut RandEncParams,
    mut formation: RandEncFormation,
    lane: bool,
) {
    formation.field_0x08 = f32_at(reader, 1);
    formation.min_count = reader.value_int(2);
    formation.max_count = reader.value_int(3);
    let mut index = 4;
    while !reader.is_value_empty(index) {
        let class = if lane {
            create_id(&reader.value_string_at(index))
        } else {
            create_id(&value_tstring(reader, index, 0x40))
        };
        let members = cx
            .logic
            .content
            .tables
            .ship_class(class)
            .map(<[u32]>::to_vec);
        let skip = match &members {
            None => true,
            Some(m) => m.is_empty() || (lane && reader.is_value_empty(index + 1)),
        };
        if !skip {
            formation.ship_choices.push(ShipChoice {
                weight: f32_at(reader, index + 1),
                ship_classes: members.unwrap_or_default(),
            });
        }
        index += 2;
    }
    formation.weight = formation.field_0x08;
    params.formations.push(formation);
}

/// The `formation` of value 0 and its id.
fn formation_by_name(cx: &Cx<'_>, id: u32) -> RandEncFormation {
    RandEncFormation {
        formation: cx.logic.content.tables.formation(id).map(|_| id),
        nickname: id,
        ..RandEncFormation::default()
    }
}

/// The keys both populators read.
fn common_value(cx: &mut Cx<'_>, reader: &mut IniReader, params: &mut RandEncParams) -> bool {
    if reader.is_value(b"target_ship_name") {
        params.target_ship_name = value_tstring(reader, 0, 0x30);
    } else if reader.is_value(b"attacker_rep_name") {
        params.attacker_rep = tables::reputation_group(cx, &reader.value_string_at(0)) as i32;
    } else if reader.is_value(b"attacker_rep") {
        params.attacker_rep = reader.value_int(0);
    } else if reader.is_value(b"pilot") {
        params.pilot_id = create_id(&reader.value_string_at(0));
    } else if reader.is_value(b"pilot_id") {
        params.pilot_id = reader.value_int(0) as u32;
    } else if reader.is_value(b"orientation") {
        params.spawn_angle = f32_at(reader, 0) * DEG_TO_RAD;
        if !reader.is_value_empty(1) {
            params.spawn_distance = f32_at(reader, 1);
        }
    } else if reader.is_value(b"radius") {
        params.empty_pos_radius = trunc_to_i32(f64::from(f32_at(reader, 0)));
    } else if reader.is_value(b"label") {
        params.labels.push(create_id(&reader.value_string_at(0)));
    } else if reader.is_value(b"num_forms") {
        params.num_forms_min = reader.value_int(0);
        params.num_forms_max = reader.value_int(1);
        params.formations_to_spawn = pick(cx, params.num_forms_min, params.num_forms_max);
    } else {
        return false;
    }
    true
}

/// `FRandEncSPPopulator::Parse` / `FRandEncTLPopulator::Parse`: the values
/// after the current one, the reader's state saved first and put back.
pub(crate) fn populator_parse(cx: &mut Cx<'_>, reader: &mut IniReader, params: &mut RandEncParams) {
    let saved = reader.state();
    if (reader.is_header(b"RandomEncounter") || reader.is_header(b"MsnRandEnc"))
        && reader.read_value()
    {
        loop {
            match params.kind {
                RandEncKind::Space => space_value(cx, reader, params),
                RandEncKind::TradeLane => lane_value(cx, reader, params),
            }
            if !reader.read_value() {
                break;
            }
        }
    }
    reader.set_state(&saved);
}

/// One value of `FRandEncSPPopulator::Parse`.
fn space_value(cx: &mut Cx<'_>, reader: &mut IniReader, params: &mut RandEncParams) {
    if reader.is_value(b"label") {
        params.labels.push(create_id(&reader.value_string_at(0)));
    } else if reader.is_value(b"activation_type") {
        let kind = reader.value_string_at(0);
        if eq_ignore_case(c_str(&kind), b"location") {
            if params.activation_type == 0 {
                params.activation_location =
                    Vector::new(f32_at(reader, 1), f32_at(reader, 2), f32_at(reader, 3));
                let range = f64::from(f32_at(reader, 4));
                params.activation_range = trunc_to_i32(f64::from(f32_at(reader, 4)) * range);
                params.activation_type = 1;
            }
        } else if eq_ignore_case(c_str(&kind), b"time") && params.activation_type == 0 {
            params.activation_time[1] = f32_at(reader, 1);
            params.activation_time[0] = f32_at(reader, 2);
            let high = trunc_to_i32(f64::from(params.activation_time[0]));
            let low = trunc_to_i32(f64::from(params.activation_time[1]));
            let picked = pick(cx, low, high);
            params.activation_type = 2;
            // FILD of the qword {picked, 0}, stored as a float.
            params.cumulative_time = i64::from(picked as u32) as f32;
        }
    } else if reader.is_value(b"formation") {
        let formation = formation_by_name(cx, create_id(&reader.value_string_at(0)));
        formation_values(cx, reader, params, formation, false);
    } else if reader.is_value(b"formation_position") {
        let position = Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2));
        params.positions.push((position, 1.0));
    } else if reader.is_value(b"arrival_obj") {
        params.dock_target = if atoi(&reader.value_string_at(0)) != 0 {
            reader.value_int(0) as u32
        } else {
            nickname_id(&reader.value_string_at(0))
        };
    } else {
        let _ = common_value(cx, reader, params);
    }
}

/// One value of `FRandEncTLPopulator::Parse`.
fn lane_value(cx: &mut Cx<'_>, reader: &mut IniReader, params: &mut RandEncParams) {
    if reader.is_value(b"target_tradelane_name") {
        params.target_tradelane = nickname_id(&reader.value_string_at(0));
    } else if reader.is_value(b"target_tradelane") {
        params.target_tradelane = reader.value_int(0) as u32;
    } else if reader.is_value(b"tl_attack_chance") {
        params.tl_attack_chance_read_in = reader.value_int(0);
        params.tl_attack_chance = reader.value_int(0);
    } else if reader.is_value(b"tl_attack_chance_read_in") {
        params.tl_attack_chance_read_in = reader.value_int(0);
    } else if reader.is_value(b"formation") {
        let formation = formation_by_name(cx, create_id(&reader.value_string_at(0)));
        formation_values(cx, reader, params, formation, true);
    } else if reader.is_value(b"saved_formation") {
        let formation = formation_by_name(cx, reader.value_int(0) as u32);
        formation_values(cx, reader, params, formation, true);
    } else {
        let _ = common_value(cx, reader, params);
    }
}

/// One `[MsnRandEnc]` section: `nickname`, and `encounter_type`, which for
/// `TL_Attack` or `SP_Attack` (`_stricmp`) makes new parameters read by
/// their populator from the values after it. Every `encounter_type` line
/// then gives the parameters the encounter's id, the mission-encounter
/// label, the mission's controller and player; the C++ does so through
/// null for another type before a known one, the port skips it (D815).
pub fn load_record(
    cx: &mut Cx<'_>,
    reader: &mut IniReader,
    controller: ControllerId,
    player: Option<u32>,
) -> MissionRandEnc {
    let mut record = MissionRandEnc {
        nickname: Vec::new(),
        enabled: false,
        nickname_id: 0,
        formation_index: 0,
        params: None,
        npc_count: 0,
    };
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            record.nickname = tstring(&reader.value_string_at(0), 0x20);
            record.nickname_id = create_id(&reader.value_string_at(0));
        } else if reader.is_value(b"encounter_type") {
            let kind = reader.value_string_at(0);
            let kind = if eq_ignore_case(c_str(&kind), b"TL_Attack") {
                Some(RandEncKind::TradeLane)
            } else if eq_ignore_case(c_str(&kind), b"SP_Attack") {
                Some(RandEncKind::Space)
            } else {
                None
            };
            if let Some(kind) = kind {
                let mut params = RandEncParams::new(kind);
                populator_parse(cx, reader, &mut params);
                record.params = Some(params);
            }
            if let Some(params) = record.params.as_mut() {
                params.labels.push(record.nickname_id);
                params.labels.push(create_id(MISSION_RANDOM_ENCOUNTER));
                params.controller = controller.0;
                params.player = player;
            }
        }
    }
    record
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_parameters_start_with_the_constructors_values() {
        let p = RandEncParams::new(RandEncKind::Space);
        assert_eq!(p.spawn_angle.to_bits(), 0x3fc9_0fdb);
        assert_eq!(p.activation_range, 0x005f_5e10);
        assert_eq!(p.attacker_rep, -1);
        assert_eq!(RandEncKind::TradeLane.code(), 3);
    }
}
