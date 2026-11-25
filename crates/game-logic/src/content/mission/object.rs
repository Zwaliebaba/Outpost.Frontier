//! The mission object's sections (TU38, `Content/MissionObject.cpp`
//! `FMissionObject::LoadMission`, `:3502-4283` with its records'
//! constructors): `[MsnLoot]`, `[MsnSolar]`, `[MsnShip]` and
//! `[MsnFormation]`, `[MsnRandEnc]` ([`super::rand_enc`]), `[Dialog]`
//! ([`super::dialog`]), `[Mission]` and `[NNObjective]`
//! ([`super::objective`]), and the script's `[NPC]` records
//! (`LoadMsnNpc`, `Content/NpcShips.cpp:358-410`).
//!
//! `[ObjList]` ([`super::obj_list`]) and `[MetaBehavior]`
//! ([`super::meta_behavior`]) go into the mission object's two maps (P3.5a2):
//! their parse draws no `rand()` and changes nothing the other sections
//! read. The C++ keeps each list as a `std::list` and each map as a
//! `std::map`; the port keeps them as `Vec` in insertion order and
//! `BTreeMap`.

use std::collections::BTreeMap;

use game_sim::ai::personality::Personality;
use game_sim::data::costume::{Costume, MAX_ACCESSORIES};
use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Quaternion, Vector, quaternion_to_matrix};
use neuron_core::num::trunc_to_i32;
use neuron_server::controller::ControllerId;

use super::dialog::{self, MissionDialog};
use super::field::record;
use super::meta_behavior::{self, MetaBehavior};
use super::obj_list::{self, ObjListDirective};
use super::object_save::{Invulnerable, RandEncRespawn, VibeInfo};
use super::objective::{self, Objective};
use super::rand_enc::{self, MissionRandEnc};
use super::{base_id_of, f32_at, nickname_id, system_id_into, tstring, value_tstring};
use crate::api;
use crate::content::tables::{self, NpcShipArch};
use crate::server::Cx;

/// The heartbeat of a mission object (`MissionObject.cpp:88`): 1/30 s.
pub const HEARTBEAT: f32 = 0.033_333_335;

/// The formation of a new `[MsnShip]` record (`CreateID("transport_helion")`).
pub const DEFAULT_FORMATION: &[u8] = b"transport_helion";

/// `pi / 180` as the `rel_pos` parse stores it (`0x3c8efa35`).
const DEG_TO_RAD: f32 = 0.017_453_292;

record! {
    /// One `[NPC]` record (`FMsnNpc`, `NpcShips.h:154`).
    MsnNpc {
        /// `nickname`, `CreateID` of the whole value.
        nickname: u32 = 0,
        /// `affiliation`, `GetReputationGroup` of the whole value.
        affiliation: u32 = u32::MAX,
        /// `individual_name`, i0.
        individual_name: i32 = 0,
        /// `npc_ship_arch`, `CreateID` of the whole value.
        npc_ship_arch: u32 = 0,
        /// +0x0c, not parsed.
        field_0x0c: u32 = 0,
        /// `space_costume`: head, body, accessories.
        appearance: Costume = Costume::default(),
        /// `voice`, `GetVoiceID` of the whole value (at most 0x4f bytes).
        voice: u32 = 0,
    }
}

record! {
    /// One ship slot of a `[MsnShip]` / `[MsnFormation]` record
    /// (`FMissionShip`, `MissionObject.h:73`). Its directives and guide are
    /// empty after the parse and are not dumped.
    MissionShip {
        /// The record's `nickname`.
        nickname: Vec<u8> = Vec::new(),
        /// `npc`, `CreateID`.
        npc_id: u32 = 0,
        /// `CreateID` of the nickname and of each `label`.
        label_ids: Vec<u32> = Vec::new(),
        /// `jumper`.
        jumper: bool = false,
        /// `jumper` as well.
        original_jump_flag: bool = false,
        /// The `[ObjList]` priority (0).
        priority: i32 = 0,
        /// `random_name`.
        random_name: bool = false,
        /// `init_objectives`, while `init_metabehavior` is unset.
        init_objectives: u32 = 0,
        /// `init_metabehavior`, while `init_objectives` is unset.
        init_meta_behavior: u32 = 0,
        /// `cargo`: good and count, each line appended.
        cargo: Vec<(u32, i32)> = Vec::new(),
        /// `hit_points` (-1).
        hit_points: i32 = -1,
        /// `hit_points` as well (-1).
        field_0x80: i32 = -1,
        /// `radius` (7); `arrival_obj` zeroes every slot's.
        radius: f32 = 7.0,
    }
    kept {
        /// `Directives` (+0x58): empty after the parse; a respawn record's
        /// copy of its ship's directives, or a restored `[MObjective]`
        /// (P3.5b), written back as `[MObjective]`.
        directives: Vec<ObjListDirective> = Vec::new(),
        /// `Guide` (+0x68): none after the parse; a restored ship's guide,
        /// kept as the `[MetaBehavior]` object `CreateGuide` made it from
        /// (P3.5b; the guide objects are P3.6c's). The save writes its line.
        guide: Option<MetaBehavior> = None,
    }
}

record! {
    /// One `[MsnShip]` or `[MsnFormation]` section (`FMissionShipGroup`,
    /// `MissionObject.h:121`).
    ShipGroup {
        /// `nickname`.
        nickname: Vec<u8> = Vec::new(),
        /// `system`: the system record's id when the record exists, else 0.
        system: u32 = 0,
        /// `orientation`.
        orientation: Matrix = Matrix::IDENTITY,
        /// `position`.
        position: Vector = Vector::ZERO,
        /// `rel_pos` value 0, in radians.
        rel_pos_heading: f32 = 0.0,
        /// `rel_pos` value 1.
        rel_pos_obj: Vec<u8> = Vec::new(),
        /// `rel_pos` value 2 (2500).
        rel_pos_range: f32 = 2500.0,
        /// Set by `rel_pos`.
        rel_pos: bool = false,
        /// Set by `orientation`.
        has_orientation: bool = false,
        /// `arrival_obj` value 0.
        arrival_obj: Vec<u8> = Vec::new(),
        /// The object the ships launch from (run time).
        arrival_obj_id: u32 = 0,
        /// `arrival_obj` value 1 (-1).
        field_0xe0: i32 = -1,
        /// `formation`, `CreateID` (`transport_helion`).
        formation: u32 = create_id(DEFAULT_FORMATION),
    }
    kept {
        /// The ship slots.
        ships: Vec<MissionShip> = Vec::new(),
    }
}

record! {
    /// One `[MsnLoot]` section (`FMissionLoot`, `MissionObject.h:151`), its
    /// `pub::SpaceObj::LootInfo` flattened.
    MissionLoot {
        /// `nickname`.
        nickname: Vec<u8> = Vec::new(),
        /// The spawned loot (run time).
        space_obj_id: u32 = 0,
        /// The spawn state (run time).
        spawned: u32 = 0,
        /// `LootInfo::SystemID`.
        system_id: u32 = 0,
        /// `archetype`, `GetEquipmentID`.
        equip_arch_id: u32 = 0,
        /// `string_id`.
        name_id: u32 = 0,
        /// `LootInfo::OwnerObjId`.
        owner_obj_id: u32 = 0,
        /// `velocity`.
        velocity: Vector = Vector::ZERO,
        /// `position`.
        position: Vector = Vector::ZERO,
        /// `orientation`.
        orientation: Matrix = Matrix::IDENTITY,
        /// `angular_velocity`.
        angular_velocity: Vector = Vector::ZERO,
        /// `equip_amount`.
        units: u32 = 0,
        /// `health`.
        hit_pts_scale: f32 = 0.0,
        /// `LootInfo::bCanAiTractor` (1).
        can_ai_tractor: bool = true,
        /// `LootInfo::bMission`.
        mission: bool = false,
        /// `LootInfo::MarkForPlayer`.
        mark_for_player: u32 = 0,
        /// `rel_pos_obj`.
        rel_pos_obj: Vec<u8> = Vec::new(),
        /// `rel_pos_offset`.
        rel_pos_offset: Vector = Vector::ZERO,
        /// Set by `rel_pos_obj`.
        rel_pos: bool = false,
        /// +0xe8 (0).
        field_0xe8: u32 = 0,
        /// `CreateID` of the nickname and of each `label`.
        label_ids: Vec<u32> = Vec::new(),
        /// `SUBOBJ_ID_NONE`.
        sub_obj_id: u32 = 0,
        /// `can_jettison`, inverted.
        cannot_jettison: bool = false,
    }
}

record! {
    /// One `[MsnSolar]` section (`FMissionSolar`, `MissionObject.h:181`).
    MissionSolar {
        /// The spawned solar (run time).
        space_obj_id: u32 = 0,
        /// The spawn state (run time).
        spawned: u32 = 0,
        /// `pilot` found.
        pilot: bool = false,
        /// `radius` (150).
        radius: f32 = 150.0,
        /// `nickname`.
        nickname: Vec<u8> = Vec::new(),
        /// `orientation`.
        orientation: Matrix = Matrix::IDENTITY,
        /// `position`.
        position: Vector = Vector::ZERO,
        /// `archetype`, `GetNicknameId` of the whole value.
        archetype_id: u32 = 0,
        /// `system`, `GetSystemID`.
        system_id: u32 = 0,
        /// `loadout`, `GetLoadoutID` of the whole value.
        loadout_id: u32 = 0,
        /// +0x458 (-1).
        field_0x458: i32 = -1,
        /// `faction`, `GetReputationGroup` (-1).
        faction: u32 = u32::MAX,
        /// `base`, `GetBaseID` of the whole value.
        base_id: u32 = 0,
        /// `voice`, `GetVoiceID`.
        voice_id: u32 = 0,
        /// +0x468 (0).
        field_0x468: u32 = 0,
        /// `CreateID` of the nickname and of each `label`.
        label_ids: Vec<u32> = Vec::new(),
        /// `costume`.
        space_costume: Costume = Costume::default(),
        /// `string_id`: the name's mad lib.
        name: u32 = 0,
        /// `visit = unknowable` gives 0x80.
        visit: u8 = 0,
    }
    kept {
        /// `pilot`: the personality `GetPilotPersonality` wrote (compared
        /// with the pilots table, P3.4b).
        personality: Personality = Personality::default(),
    }
}

/// A mission's record of one player (`FMissionPlayer`,
/// `MissionObject.h:596`): the `[NNObjective]` sections go into the first
/// one's objectives.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MissionPlayer {
    /// `PlayerId`.
    pub player: u32,
    /// `Objectives`.
    pub objectives: Vec<Objective>,
}

/// The mission object (`FMissionObject`, TU38): what its constructor and
/// `LoadMission` set.
#[derive(Clone, Debug, PartialEq)]
pub struct MissionObject {
    /// `ControllerId`.
    pub controller: ControllerId,
    /// `PlayerId`.
    pub player: u32,
    /// `PlayerState`: whether the player had a state when the mission was
    /// made.
    pub player_state: bool,
    /// `[Mission] reward`.
    pub reward: u32,
    /// `HeartbeatInterval`.
    pub heartbeat_interval: f32,
    /// `Field_0x50`: `[Mission] mission_title`'s mad lib.
    pub title: u32,
    /// `Field_0x68`: `[Mission] mission_offer`'s mad lib.
    pub offer: u32,
    /// `[Mission] npc_ship_file`'s archetypes.
    pub npc_ship_archs: BTreeMap<u32, NpcShipArch>,
    /// `MissionShips` (+0xb8).
    pub ships: Vec<ShipGroup>,
    /// `MissionSolars` (+0xc4).
    pub solars: Vec<MissionSolar>,
    /// `MissionLoots` (+0xd0).
    pub loot: Vec<MissionLoot>,
    /// `RandEncs` (+0x98).
    pub rand_encs: Vec<MissionRandEnc>,
    /// `MissionNpcs` (+0x184), the script's `[NPC]` records.
    pub npcs: BTreeMap<u32, MsnNpc>,
    /// `Players` (+0x3c).
    pub players: BTreeMap<u32, MissionPlayer>,
    /// `ObjLists` (+0x100): each `[ObjList]`'s directives by its nickname's
    /// id (the first list of an id stays).
    pub obj_lists: BTreeMap<u32, Vec<ObjListDirective>>,
    /// `MetaBehaviors` (+0x114): each `[MetaBehavior]`'s objects by its
    /// nickname's id (the first list of an id stays).
    pub meta_behaviors: BTreeMap<u32, Vec<MetaBehavior>>,
    /// `bTriggerSavePending` (+0x18): `Act_Save` asked for a save of one
    /// trigger (P3.5c); the save then writes only its `[TriggerSave]`.
    pub trigger_save_pending: bool,
    /// `bPendingDestroy` (+0x19).
    pub pending_destroy: bool,
    /// `RespawnShips` (+0x128): the `[MsnShip]` records to create again,
    /// taken by the snapshot (P3.5c) or restored from `[MsnWingSave]`.
    pub respawn_ships: Vec<ShipGroup>,
    /// `RandEncRespawns` (+0x8c): the random-encounter formations to create
    /// again, from the snapshot or `[MsnRandEncSave]`.
    pub rand_enc_respawns: Vec<RandEncRespawn>,
    /// `RespawnSolars` (+0x134): the `[MsnSolar]` names to create again.
    pub respawn_solars: Vec<Vec<u8>>,
    /// `MarkObjNames` (+0x140): `[MarkObj]`.
    pub mark_obj_names: Vec<Vec<u8>>,
    /// `Invulnerables` (+0x14c): `[Invulnerables]`.
    pub invulnerables: Vec<Invulnerable>,
    /// `DecloakedNames` (+0x158): `[Decloaked]`.
    pub decloaked: Vec<Vec<u8>>,
    /// `VibeInfos` (+0x164): the attitudes the snapshot took, `[MsnVibeInfo]`.
    pub vibe_infos: Vec<VibeInfo>,
    /// `SolarReps` (+0x174): the reputations of the unnamed vibe parties,
    /// each once.
    pub solar_reps: Vec<i32>,
}

impl MissionObject {
    /// `FMissionObject::FMissionObject` (`MissionObject.cpp:83-102`) without
    /// its heartbeat, which the caller sets.
    #[must_use]
    pub fn new(controller: ControllerId) -> Self {
        Self {
            controller,
            player: 0,
            player_state: false,
            reward: 0,
            heartbeat_interval: HEARTBEAT,
            title: 0,
            offer: 0,
            npc_ship_archs: BTreeMap::new(),
            ships: Vec::new(),
            solars: Vec::new(),
            loot: Vec::new(),
            rand_encs: Vec::new(),
            npcs: BTreeMap::new(),
            players: BTreeMap::new(),
            obj_lists: BTreeMap::new(),
            meta_behaviors: BTreeMap::new(),
            trigger_save_pending: false,
            pending_destroy: false,
            respawn_ships: Vec::new(),
            rand_enc_respawns: Vec::new(),
            respawn_solars: Vec::new(),
            mark_obj_names: Vec::new(),
            invulnerables: Vec::new(),
            decloaked: Vec::new(),
            vibe_infos: Vec::new(),
            solar_reps: Vec::new(),
        }
    }

    /// `FindMissionShip`: the first ship slot of any record named
    /// `nickname` (`_stricmp`).
    #[must_use]
    pub fn find_ship(&self, nickname: &[u8]) -> Option<&MissionShip> {
        self.ships
            .iter()
            .flat_map(|g| &g.ships)
            .find(|s| eq_ignore_case(&s.nickname, c_str(nickname)))
    }

    /// `GetMissionCostume` (`MissionObject.cpp:2197-2214`): a ship slot's
    /// `[NPC]` costume (none when the slot names no record), else the first
    /// solar's of that nickname.
    #[must_use]
    pub fn costume(&self, nickname: &[u8]) -> Option<Costume> {
        if let Some(ship) = self.find_ship(nickname) {
            return self.npcs.get(&ship.npc_id).map(|n| n.appearance.clone());
        }
        self.solars
            .iter()
            .find(|s| eq_ignore_case(&s.nickname, c_str(nickname)))
            .map(|s| s.space_costume.clone())
    }
}

/// `ReadMissionOrientation` (`[MsnLoot]`): values 2, 1, 0, 3 read; the
/// length `sqrt(W² + X² + Y² + Z²)` and its inverse in the x87's 53 bits,
/// each part times the inverse stored as a float.
fn loot_orientation(reader: &mut IniReader) -> Matrix {
    let y = f32_at(reader, 2);
    let x = f32_at(reader, 1);
    let w = f32_at(reader, 0);
    let z = f32_at(reader, 3);
    let (w, x, y, z) = (f64::from(w), f64::from(x), f64::from(y), f64::from(z));
    let inverse = 1.0 / (w * w + x * x + y * y + z * z).sqrt();
    quaternion_to_matrix(&Quaternion::new(
        (w * inverse) as f32,
        (x * inverse) as f32,
        (y * inverse) as f32,
        (z * inverse) as f32,
    ))
}

/// `ReadMissionSolarOrientation` (`[MsnSolar]`, `[MsnShip]`): values 2, 1,
/// 0, 3 read; the sum `W² + Z² + Y² + X²`, `1 / sqrt` of it stored as a
/// float, and each part times that float stored as a float.
fn solar_orientation(reader: &mut IniReader) -> Matrix {
    let y = f32_at(reader, 2);
    let x = f32_at(reader, 1);
    let w = f32_at(reader, 0);
    let z = f32_at(reader, 3);
    let (w, x, y, z) = (f64::from(w), f64::from(x), f64::from(y), f64::from(z));
    let inverse = f64::from((1.0 / (w * w + z * z + y * y + x * x).sqrt()) as f32);
    quaternion_to_matrix(&Quaternion::new(
        (w * inverse) as f32,
        (inverse * x) as f32,
        (inverse * y) as f32,
        (inverse * z) as f32,
    ))
}

/// `LoadMissionLootValues`: the values of one `[MsnLoot]`.
fn load_loot(reader: &mut IniReader, loot: &mut MissionLoot) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            loot.nickname = value_tstring(reader, 0, 0x30);
            loot.label_ids.push(create_id(&loot.nickname));
        } else if reader.is_value(b"archetype") {
            // `pub::GetEquipmentID` writes the id of any non-empty name.
            let name = value_tstring(reader, 0, 0x40);
            if !name.is_empty() {
                loot.equip_arch_id = create_id(&name);
            }
        } else if reader.is_value(b"string_id") {
            loot.name_id = reader.value_int(0) as u32;
        } else if reader.is_value(b"position") {
            loot.position = Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2));
        } else if reader.is_value(b"rel_pos_obj") {
            loot.rel_pos_obj = value_tstring(reader, 0, 0x30);
            loot.rel_pos = true;
        } else if reader.is_value(b"rel_pos_offset") {
            loot.rel_pos_offset =
                Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2));
        } else if reader.is_value(b"orientation") {
            loot.orientation = loot_orientation(reader);
        } else if reader.is_value(b"velocity") {
            loot.velocity = Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2));
        } else if reader.is_value(b"angular_velocity") {
            loot.angular_velocity =
                Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2));
        } else if reader.is_value(b"equip_amount") {
            loot.units = reader.value_int(0) as u32;
        } else if reader.is_value(b"health") {
            loot.hit_pts_scale = f32_at(reader, 0);
        } else if reader.is_value(b"label") {
            loot.label_ids.push(create_id(&reader.value_string_at(0)));
        } else if reader.is_value(b"can_jettison") {
            loot.cannot_jettison = !reader.value_bool(0);
        }
    }
}

/// `LoadMissionSolarValues`: the values of one `[MsnSolar]`.
fn load_solar(cx: &mut Cx<'_>, reader: &mut IniReader, solar: &mut MissionSolar) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            solar.nickname = tstring(&reader.value_string(), 0x30);
            solar.label_ids.push(create_id(&solar.nickname));
        } else if reader.is_value(b"position") {
            let y = f32_at(reader, 1);
            let x = f32_at(reader, 0);
            let z = f32_at(reader, 2);
            solar.position = Vector::new(x, y, z);
        } else if reader.is_value(b"orientation") {
            solar.orientation = solar_orientation(reader);
        } else if reader.is_value(b"faction") {
            solar.faction = tables::reputation_group(cx, &reader.value_string());
        } else if reader.is_value(b"string_id") {
            solar.name = reader.value_int(0) as u32;
        } else if reader.is_value(b"pilot") {
            let pilot = create_id(&reader.value_string_at(0));
            let pilots = cx.logic.content.tables.clone();
            if pilots.pilots.personality(pilot, &mut solar.personality) {
                solar.pilot = true;
            }
        } else if reader.is_value(b"archetype") {
            solar.archetype_id = nickname_id(&reader.value_string());
        } else if reader.is_value(b"loadout") {
            if let Ok(id) = api::misc::loadout_id(&reader.value_string()) {
                solar.loadout_id = id;
            }
        } else if reader.is_value(b"system") {
            system_id_into(cx, &mut solar.system_id, &reader.value_string_at(0));
        } else if reader.is_value(b"base") {
            solar.base_id = base_id_of(cx, &reader.value_string());
        } else if reader.is_value(b"voice") {
            tables::voice_id(&mut solar.voice_id, &reader.value_string_at(0));
        } else if reader.is_value(b"radius") {
            solar.radius = f32_at(reader, 0);
        } else if reader.is_value(b"label") {
            solar.label_ids.push(create_id(&reader.value_string()));
        } else if reader.is_value(b"costume") {
            space_costume(cx, reader, &mut solar.space_costume);
        } else if reader.is_value(b"visit")
            && eq_ignore_case(c_str(&reader.value_string_at(0)), b"unknowable")
        {
            solar.visit = 0x80;
        }
    }
}

/// `ParseSpaceCostume` and its inlined copy in `LoadMsnNpc`: head (value
/// 0), body (value 1), then each further value's accessory appended after
/// those the costume has; the port keeps eight (D782).
fn space_costume(cx: &mut Cx<'_>, reader: &IniReader, out: &mut Costume) {
    out.part_ids[0] = api::gf::body_part(cx, &reader.value_string_at(0), 1) as u32;
    out.part_ids[1] = api::gf::body_part(cx, &reader.value_string_at(1), 0) as u32;
    let mut index = 2;
    while !reader.is_value_empty(index) {
        let accessory = api::gf::accessory(cx, &reader.value_string_at(index)) as u32;
        if out.accessory_ids.len() < MAX_ACCESSORIES {
            out.accessory_ids.push(accessory);
        }
        index += 1;
    }
}

/// `LoadMissionHeaderValues`: the values of `[Mission]`; `npc_ship_file`
/// reads that file's archetypes into the mission's (the open not tested).
fn load_header(cx: &mut Cx<'_>, reader: &mut IniReader, object: &mut MissionObject) {
    while reader.read_value() {
        if reader.is_value(b"mission_title") {
            object.title = reader.value_int(0) as u32;
        } else if reader.is_value(b"mission_offer") {
            object.offer = reader.value_int(0) as u32;
        } else if reader.is_value(b"reward") {
            object.reward = reader.value_int(0) as u32;
        } else if reader.is_value(b"npc_ship_file") {
            let source = crate::content::data_source(cx);
            let mut file = tables::open(&*source, &reader.value_string_at(0));
            tables::npc_ships::load_npc_ship_archs(cx, &mut object.npc_ship_archs, &mut file);
        }
    }
}

/// What became of the ship slot the keys of a `[MsnShip]` write.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Slot {
    /// The template slot, the group's last.
    Current,
    /// Dropped by `formation` or `ship`: the C++ writes through null after
    /// this (D811).
    Dropped,
}

/// `LoadMissionShipValues` (`MissionObject.cpp:3766-3935`): the values of
/// one `[MsnShip]` / `[MsnFormation]` into the record at `group`, the
/// list's last, whose template slot was appended first. `ship` moves the
/// first slot of the first record of that name into this one and drops
/// that record.
fn load_ship_group(cx: &mut Cx<'_>, reader: &mut IniReader, groups: &mut Vec<ShipGroup>) {
    let mut group = groups.len() - 1;
    groups[group].ships.push(MissionShip::default());
    let mut slot = Slot::Current;
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            groups[group].nickname = tstring(&reader.value_string(), 0x30);
            if slot == Slot::Current {
                let id = create_id(&reader.value_string());
                let g = &mut groups[group];
                let nickname = g.nickname.clone();
                let ship = g.ships.last_mut().expect("the template slot");
                ship.nickname = nickname;
                ship.label_ids.push(id);
            }
            continue;
        }
        if reader.is_value(b"formation") {
            if slot == Slot::Current {
                groups[group].ships.pop();
                slot = Slot::Dropped;
            }
            groups[group].formation = create_id(&reader.value_string_at(0));
            continue;
        }
        if reader.is_value(b"ship") {
            if slot == Slot::Current {
                groups[group].ships.pop();
                slot = Slot::Dropped;
            }
            let name = value_tstring(reader, 0, 0x30);
            let Some(found) = groups.iter().position(|g| g.nickname == name) else {
                continue;
            };
            // A record naming itself (or one without slots) reads freed or
            // missing memory in the C++; the port moves nothing (D812).
            if found == group || groups[found].ships.is_empty() {
                continue;
            }
            let copy = groups[found].ships[0].clone();
            groups[group].ships.push(copy);
            groups.remove(found);
            if found < group {
                group -= 1;
            }
            continue;
        }
        if reader.is_value(b"system") {
            if let Ok(system) = api::misc::system_id(cx, &reader.value_string_at(0))
                && cx.logic.content.state.systems.contains_key(&system)
            {
                groups[group].system = system;
            }
            continue;
        }
        if reader.is_value(b"position") {
            let y = f32_at(reader, 1);
            let x = f32_at(reader, 0);
            let z = f32_at(reader, 2);
            groups[group].position = Vector::new(x, y, z);
            continue;
        }
        if reader.is_value(b"rel_pos") {
            let g = &mut groups[group];
            g.rel_pos = true;
            g.rel_pos_heading = f32_at(reader, 0) * DEG_TO_RAD;
            g.rel_pos_obj = value_tstring(reader, 1, 0x30);
            if !reader.is_value_empty(2) {
                g.rel_pos_range = f32_at(reader, 2);
            }
            continue;
        }
        if reader.is_value(b"orientation") {
            groups[group].has_orientation = true;
            groups[group].orientation = solar_orientation(reader);
            continue;
        }
        if reader.is_value(b"arrival_obj") {
            let g = &mut groups[group];
            for ship in &mut g.ships {
                ship.radius = 0.0;
            }
            g.arrival_obj = value_tstring(reader, 0, 0x30);
            if !reader.is_value_empty(1) {
                g.field_0xe0 = reader.value_int(1);
            }
            continue;
        }
        // The keys that write the template slot: nothing once it is gone
        // (the C++ writes through null, D811).
        if slot != Slot::Current {
            continue;
        }
        let ship = groups[group]
            .ships
            .last_mut()
            .expect("the template slot is the last");
        if reader.is_value(b"npc") {
            ship.npc_id = create_id(&reader.value_string());
        } else if reader.is_value(b"random_name") {
            ship.random_name = reader.value_bool(0);
        } else if reader.is_value(b"label") {
            ship.label_ids.push(create_id(&reader.value_string()));
        } else if reader.is_value(b"init_objectives") {
            if ship.init_meta_behavior == 0 {
                ship.init_objectives = create_id(&reader.value_string_at(0));
            }
        } else if reader.is_value(b"init_metabehavior") {
            if ship.init_objectives == 0 {
                ship.init_meta_behavior = create_id(&reader.value_string_at(0));
            }
        } else if reader.is_value(b"jumper") {
            let jumper = reader.value_bool(0);
            ship.jumper = jumper;
            ship.original_jump_flag = jumper;
        } else if reader.is_value(b"cargo") {
            if let Ok(good) = api::misc::good_id(cx, &reader.value_string_at(0)) {
                let count = trunc_to_i32(f64::from(f32_at(reader, 1)));
                ship.cargo.push((good, count));
            }
        } else if reader.is_value(b"hit_points") {
            let hit_points = reader.value_int(0);
            ship.hit_points = hit_points;
            ship.field_0x80 = hit_points;
        } else if reader.is_value(b"radius") {
            ship.radius = f32_at(reader, 0);
        }
    }
}

/// `LoadMsnNpc`: the values of one `[NPC]` into a record, and its
/// nickname id (0 without one).
pub fn load_npc(cx: &mut Cx<'_>, reader: &mut IniReader) -> (u32, MsnNpc) {
    let mut npc = MsnNpc::default();
    let mut nickname = 0;
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            nickname = create_id(&reader.value_string());
            npc.nickname = nickname;
        } else if reader.is_value(b"space_costume") {
            space_costume(cx, reader, &mut npc.appearance);
        } else if reader.is_value(b"voice") {
            let name = tstring(&reader.value_string(), 0x50);
            tables::voice_id(&mut npc.voice, &name);
        } else if reader.is_value(b"npc_ship_arch") {
            npc.npc_ship_arch = create_id(&reader.value_string());
        } else if reader.is_value(b"affiliation") {
            npc.affiliation = tables::reputation_group(cx, &reader.value_string());
        } else if reader.is_value(b"individual_name") {
            npc.individual_name = reader.value_int(0);
        }
    }
    (nickname, npc)
}

/// `FMissionObject::LoadMission` (`MissionObject.cpp:4141-4283`): the
/// reader reset, both mad libs cleared, then every header in order.
/// `[ObjList]` and `[MetaBehavior]` go into their maps (first id wins; a
/// list's nickname starts as `CreateID("")`, a meta behaviour's as 0).
/// `[Dialog]` sections go into Content's mission dialogs (`GMissionDialogs`,
/// first id wins), `[NNObjective]` into the first player's objectives
/// (none without a player record: the C++ writes through null, D813).
pub fn load(cx: &mut Cx<'_>, reader: &mut IniReader, object: &mut MissionObject) {
    reader.reset();
    object.title = 0;
    object.offer = 0;
    while reader.read_header() {
        if reader.is_header(b"MsnLoot") {
            let mut loot = MissionLoot::default();
            load_loot(reader, &mut loot);
            object.loot.push(loot);
        } else if reader.is_header(b"MsnSolar") {
            let mut solar = MissionSolar::default();
            load_solar(cx, reader, &mut solar);
            object.solars.push(solar);
        } else if reader.is_header(b"MsnShip") || reader.is_header(b"MsnFormation") {
            object.ships.push(ShipGroup::default());
            load_ship_group(cx, reader, &mut object.ships);
        } else if reader.is_header(b"MsnRandEnc") {
            let player = object.player_state.then_some(object.player);
            let record = rand_enc::load_record(cx, reader, object.controller, player);
            object.rand_encs.push(record);
        } else if reader.is_header(b"ObjList") {
            let (nickname, list) = obj_list::load(cx, reader);
            object.obj_lists.entry(nickname).or_insert(list);
        } else if reader.is_header(b"MetaBehavior") {
            let mut nickname = 0;
            let list = meta_behavior::load(reader, &mut nickname);
            object.meta_behaviors.entry(nickname).or_insert(list);
        } else if reader.is_header(b"Dialog") {
            reader.read_value();
            if reader.is_value(b"nickname") {
                let id = create_id(&reader.value_string_at(0));
                let mut mission_dialog = MissionDialog::new(object.controller);
                dialog::load(cx, reader, &mut mission_dialog);
                cx.logic
                    .content
                    .state
                    .mission_dialogs
                    .entry(id)
                    .or_insert(mission_dialog);
            }
        } else if reader.is_header(b"Mission") {
            load_header(cx, reader, object);
        } else if reader.is_header(b"NNObjective") {
            let parsed = objective::load(cx, reader);
            if let (Some(objective), Some(player)) = (parsed, object.players.values_mut().next()) {
                player.objectives.push(objective);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::content::mission::field::Field;

    #[test]
    fn a_new_ship_group_has_the_helion_formation() {
        let group = ShipGroup::default();
        assert_eq!(group.formation, create_id(b"transport_helion"));
        assert_eq!(group.rel_pos_range, 2500.0);
        assert_eq!(group.field_0xe0, -1);
        assert!(group.fields().iter().all(|(name, _)| *name != "ships"));
    }

    #[test]
    fn the_rel_pos_heading_is_in_radians() {
        assert_eq!(DEG_TO_RAD.to_bits(), 0x3c8e_fa35);
    }

    #[test]
    fn a_solar_field_list_starts_with_its_ids() {
        let solar = MissionSolar::default();
        let names: Vec<&str> = solar.fields().iter().map(|(n, _)| *n).collect();
        assert_eq!(names[..3], ["space_obj_id", "spawned", "pilot"]);
        assert_eq!(solar.fields()[11].1, Field::U32(u32::MAX));
    }
}
