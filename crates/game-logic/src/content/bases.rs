//! The `missions\mBases.ini` database of TU17 (`Content/BaseDB.cpp`) and the
//! GF_NPC record parsers of TU22 (`Content/BaseNpcRecords.cpp`): work
//! package P3.4c, decision record 0008 point 10.
//!
//! - [`build_content_bases`] (`BuildContentBases`, `:400-422`): one record
//!   per `pub::NextBaseID` base, then each `[mBase]` of the file with its
//!   values and `[BaseFaction]` sections ([`ContentBase::load_factions`]).
//! - [`ContentBase::load`] (`FContentBase::Load`, `:252-357`): once per
//!   base, the base's `[MVendor]`, `[GF_NPC]` and `[mRoom]` sections; the
//!   `[BaseFaction]` NPCs take their faction as affiliation.
//! - [`load_dyn_base_info`] (`LoadDynBaseInfo`, `:425-447`): the table
//!   rebuilt on `LoadGame`.
//!
//! The table is live state: GameSupport rebuilds it on every `LoadGame` and
//! BaseSupport loads a base's rooms when a player enters it, so it belongs
//! in [`ContentState`](super::ContentState). Its call sites are P3.4d's
//! (`LoadGame`) and P3.6e's (BaseSupport). The bar characters, room states,
//! bar scenes and offers of the same TU are BaseSupport's run time (P3.6e).
//!
//! A fixture points at its GF_NPC by the NPC's nickname id, the key of
//! [`ContentBase::npcs`], where the C++ holds a pointer into the map.

use std::collections::BTreeMap;

use game_sim::data::DataSource;
use neuron_core::hash::{c_str, stricmp};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::npc_desc::NpcCharacterDesc;
use super::tables::{open, reputation_group, stage_value};
use crate::api;
use crate::server::Cx;

/// `missions\mBases.ini`.
pub const FILE: &[u8] = b"missions\\mBases.ini";

/// `GBaseNpcRepThresholds`: the feelings the know and rumor levels need.
pub const REP_THRESHOLDS: [f32; 4] = [0.0, 0.2, 0.4, 0.6];

/// `GBaseMissionTypesInSinglePlayer`: 1 and never written.
pub const MISSION_TYPES_IN_SINGLE_PLAYER: bool = true;

/// `GBaseMissionTypesInMultiplayer`: 1 and never written.
pub const MISSION_TYPES_IN_MULTIPLAYER: bool = true;

/// A `TString<N>` assignment: at most `N - 1` bytes, up to the first NUL.
fn tstring(text: &[u8], size: usize) -> Vec<u8> {
    let mut text = c_str(text).to_vec();
    text.truncate(size - 1);
    text
}

/// `"misn = type, min, max"` (`FBaseNpcMission`).
#[derive(Clone, Debug, PartialEq)]
pub struct BaseNpcMission {
    /// Value 1, clamped to `[0, 100]`.
    pub min_difficulty: f32,
    /// Value 2, clamped to `[0, 100]`.
    pub max_difficulty: f32,
    /// Value 0 (a `TString<24>`).
    pub kind: Vec<u8>,
}

/// `"bribe = group, cost, text"` (`FBaseNpcBribe`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BaseNpcBribe {
    /// Value 1.
    pub cost: i32,
    /// Value 0, `GetReputationGroup`.
    pub group: u32,
    /// Value 2: the offer text's mad lib.
    pub text_ids: i32,
}

impl BaseNpcBribe {
    /// `FBaseNpcBribe::Parse`: `None` for an unknown group or a zero cost.
    #[must_use]
    pub fn parse(cx: &Cx<'_>, reader: &IniReader) -> Option<Self> {
        let group =
            api::reputation::reputation_group(cx, c_str(&reader.value_string_at(0))).ok()?;
        let cost = reader.value_int(1);
        if cost == 0 {
            return None;
        }
        Some(Self {
            cost,
            group,
            text_ids: reader.value_int(2),
        })
    }
}

/// `"know = v0, v1, price, level"` (`FBaseNpcKnow`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BaseNpcKnow {
    /// Value 2.
    pub price: i32,
    /// Value 1: the rumor record key and a mad lib string.
    pub field_0x04: i32,
    /// Value 0: a mad lib string.
    pub field_0x08: i32,
    /// Value 3, an index of [`REP_THRESHOLDS`].
    pub rep_level: u32,
}

impl BaseNpcKnow {
    /// `FBaseNpcKnow::Parse`: `None` unless the level is below 4.
    #[must_use]
    pub fn parse(reader: &IniReader) -> Option<Self> {
        let know = Self {
            field_0x08: reader.value_int(0),
            field_0x04: reader.value_int(1),
            price: reader.value_int(2),
            rep_level: reader.value_int(3) as u32,
        };
        (know.rep_level < 4).then_some(know)
    }
}

/// `"rumor"` / `"rumor_type2 = from, to, level, ids…"` (`FBaseNpcRumor`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BaseNpcRumor {
    /// Value 0, a stage name.
    pub from_stage: i32,
    /// Value 1, a stage name.
    pub to_stage: i32,
    /// Value 2, an index of [`REP_THRESHOLDS`].
    pub rep_level: i32,
    /// Values 3 on, up to the first empty one.
    pub ids: [i32; 8],
}

impl BaseNpcRumor {
    /// `FBaseNpcRumor::Parse` over a zeroed record: always a record.
    ///
    /// The C++ starts the stage walk from the bits of its `Reader` argument,
    /// so a first name missing from the stage table leaves a pointer value;
    /// the port starts from 0 (D790). The ids past the eighth are dropped
    /// where the C++ writes past the record (D791).
    #[must_use]
    pub fn parse(reader: &IniReader) -> Self {
        let mut rumor = Self::default();
        let mut stage = 0;
        stage_value(&reader.value_string_at(0), &mut stage);
        rumor.from_stage = stage;
        stage_value(&reader.value_string_at(1), &mut stage);
        rumor.to_stage = stage;
        rumor.rep_level = reader.value_int(2);
        let mut index = 3;
        while !reader.is_value_empty(index) {
            if let Some(slot) = rumor.ids.get_mut(index as usize - 3) {
                *slot = reader.value_int(index);
            }
            index += 1;
        }
        rumor
    }
}

/// One `[GF_NPC]` of a base (`FBaseNpc`, 0x94 bytes).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct BaseNpc {
    /// The character.
    pub desc: NpcCharacterDesc,
    /// `misn`.
    pub missions: Vec<BaseNpcMission>,
    /// `bribe`.
    pub bribes: Vec<BaseNpcBribe>,
    /// `know`.
    pub knows: Vec<BaseNpcKnow>,
    /// `rumor`, `rumor_type2`.
    pub rumors: Vec<BaseNpcRumor>,
    /// `room`, location ids.
    pub rooms: Vec<u32>,
}

/// The x87 clamp of a `misn` difficulty (`ClampMissionDifficulty`): a value
/// not below 0 (or a NaN) is kept, a negative one is 0, and a result not
/// below 100 (or a NaN) is 100.
#[must_use]
pub fn clamp_mission_difficulty(value: f32) -> f32 {
    let low = if value < 0.0 { 0.0 } else { value };
    match low.partial_cmp(&100.0) {
        Some(std::cmp::Ordering::Less) => low,
        _ => 100.0,
    }
}

/// `LoadBaseNpc` (`BaseDB.cpp:55-105`): every remaining value of the
/// section into `npc`; true when a nickname was read and the description
/// is complete (BaseSupport's caller ignores it).
pub fn load_base_npc(
    cx: &mut Cx<'_>,
    out_nickname: &mut u32,
    npc: &mut BaseNpc,
    reader: &mut IniReader,
    base: u32,
) -> bool {
    while reader.read_value() {
        if npc.desc.parse_value(cx, reader, out_nickname) {
            continue;
        }
        if reader.is_value(b"misn") {
            let kind = tstring(&reader.value_string_at(0), 24);
            let min = clamp_mission_difficulty(reader.value_float(1) as f32);
            let max = clamp_mission_difficulty(reader.value_float(2) as f32);
            npc.missions.push(BaseNpcMission {
                min_difficulty: min,
                max_difficulty: max,
                kind,
            });
        } else if reader.is_value(b"bribe") {
            if let Some(bribe) = BaseNpcBribe::parse(cx, reader) {
                npc.bribes.push(bribe);
            }
        } else if reader.is_value(b"know") {
            if let Some(know) = BaseNpcKnow::parse(reader) {
                npc.knows.push(know);
            }
        } else if reader.is_value(b"rumor") || reader.is_value(b"rumor_type2") {
            npc.rumors.push(BaseNpcRumor::parse(reader));
        } else if reader.is_value(b"room") {
            npc.rooms
                .push(api::gf::find_location(base, c_str(&reader.value_string())));
        }
    }
    *out_nickname != 0 && npc.desc.is_complete()
}

/// `"fixture = npc, placement, fidget, role"` of an `[mRoom]`
/// (`FBaseFixture`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BaseFixture {
    /// Value 0: the GF_NPC's nickname id, a key of [`ContentBase::npcs`].
    pub npc: u32,
    /// Value 1, `GetCharacterPlacementByName`.
    pub placement: i32,
    /// Value 2 (a `TString<64>`).
    pub fidget: Vec<u8>,
    /// Value 3 (a `TString<32>`).
    pub role: Vec<u8>,
}

/// One `[mRoom]` (`FBaseRoom`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BaseRoom {
    /// `nickname`, `pub::GF::FindLocation`.
    pub room_id: u32,
    /// `character_density`.
    pub character_density: i32,
    /// `fixture`.
    pub fixtures: Vec<BaseFixture>,
}

/// `"mission_type = type, min, max[, weight]"` of a `[BaseFaction]`
/// (`FBaseMissionType`); value 0 is not read.
#[derive(Clone, Debug, PartialEq)]
pub struct BaseMissionType {
    /// Value 1.
    pub min_difficulty: f32,
    /// Value 2.
    pub max_difficulty: f32,
    /// Always `DestroyMission`.
    pub kind: Vec<u8>,
    /// Value 3, 1.0 when empty.
    pub weight: f32,
}

/// One `[BaseFaction]` (`FBaseFaction`).
#[derive(Clone, Debug, PartialEq)]
pub struct BaseFaction {
    /// `faction` (`0xffffffff` by default).
    pub group: u32,
    /// `npc`, unique `CreateID` ids in first-seen order.
    pub npcs: Vec<u32>,
    /// `mission_type`.
    pub mission_types: Vec<BaseMissionType>,
    /// `weight`, -1.0 when absent.
    pub weight: f32,
}

/// `FContentBase` (0xb4 bytes): the parts the loaders fill. The room
/// states and the bar scenes are BaseSupport's run time (P3.6e).
#[derive(Clone, Debug, PartialEq)]
pub struct ContentBase {
    /// `BaseId`.
    pub base_id: u32,
    /// `msg_id_prefix` of the `[mBase]` (`CommReferrable`, a `TString<64>`).
    pub msg_id_prefix: Vec<u8>,
    /// `diff` (10 by default).
    pub diff: i32,
    /// [`ContentBase::load`]'s count.
    pub load_count: i32,
    /// `local_faction` (`0xffffffff` by default).
    pub local_faction: u32,
    /// `[BaseFaction]`.
    pub factions: Vec<BaseFaction>,
    /// `[MVendor] num_offers` value 0.
    pub vendor_offers_min: i32,
    /// `[MVendor] num_offers` value 1.
    pub vendor_offers_max: i32,
    /// `[GF_NPC]` by nickname id.
    pub npcs: BTreeMap<u32, BaseNpc>,
    /// `[mRoom]`.
    pub rooms: Vec<BaseRoom>,
}

impl ContentBase {
    /// `FContentBase::FContentBase(&BaseId)`.
    #[must_use]
    pub fn new(base_id: u32) -> Self {
        Self {
            base_id,
            msg_id_prefix: Vec::new(),
            diff: 10,
            load_count: 0,
            local_faction: u32::MAX,
            factions: Vec::new(),
            vendor_offers_min: 0,
            vendor_offers_max: 0,
            npcs: BTreeMap::new(),
            rooms: Vec::new(),
        }
    }

    /// `FContentBase::LoadFactions` (`:181-249`): the rest of the `[mBase]`
    /// section's values, then the `[BaseFaction]` sections up to the next
    /// `[mBase]`, whose header the reader is left on. `single_player` is
    /// `pub::SinglePlayer()`, which picks the flag a `mission_type` needs.
    pub fn load_factions(&mut self, cx: &mut Cx<'_>, reader: &mut IniReader, single_player: bool) {
        while reader.read_value() {
            if reader.is_value(b"local_faction") {
                self.local_faction = reputation_group(cx, &reader.value_string());
            } else if reader.is_value(b"diff") {
                self.diff = reader.value_int(0);
            } else if reader.is_value(b"msg_id_prefix") {
                // `read_comm_ini_data`.
                self.msg_id_prefix = tstring(&reader.value_string(), 64);
            }
        }
        if !reader.read_header() {
            return;
        }
        loop {
            if reader.is_header(b"BaseFaction") {
                let mut faction = BaseFaction {
                    group: u32::MAX,
                    npcs: Vec::new(),
                    mission_types: Vec::new(),
                    weight: 0.0,
                };
                let mut weight = -1.0_f32;
                while reader.read_value() {
                    if reader.is_value(b"faction") {
                        faction.group = reputation_group(cx, &reader.value_string());
                    } else if reader.is_value(b"weight") {
                        weight = reader.value_float(0) as f32;
                    } else if reader.is_value(b"mission_type") {
                        let min = reader.value_float(1) as f32;
                        let max = reader.value_float(2) as f32;
                        let mut type_weight = 1.0_f32;
                        if !reader.is_value_empty(3) {
                            type_weight = reader.value_float(3) as f32;
                        }
                        let enabled = if single_player {
                            MISSION_TYPES_IN_SINGLE_PLAYER
                        } else {
                            MISSION_TYPES_IN_MULTIPLAYER
                        };
                        if enabled {
                            faction.mission_types.push(BaseMissionType {
                                min_difficulty: min,
                                max_difficulty: max,
                                kind: b"DestroyMission".to_vec(),
                                weight: type_weight,
                            });
                        }
                    } else if reader.is_value(b"npc") {
                        let id = create_id(c_str(&reader.value_string()));
                        if !faction.npcs.contains(&id) {
                            faction.npcs.push(id);
                        }
                    }
                }
                faction.weight = weight;
                self.factions.push(faction);
            } else if reader.is_header(b"mBase") {
                return;
            }
            if !reader.read_header() {
                return;
            }
        }
    }

    /// `FContentBase::Load(Reader)` (`:252-357`): on the first load only,
    /// the base's `[MVendor]`, `[GF_NPC]` and `[mRoom]` sections up to the
    /// next `[mBase]`. Without a reader the base's `[MBase]` is looked up in
    /// `missions\mBases.ini` under `source` by its nickname (`_stricmp`), and
    /// the sections after it are read.
    pub fn load(
        &mut self,
        cx: &mut Cx<'_>,
        reader: Option<&mut IniReader>,
        source: &dyn DataSource,
    ) {
        let previous = self.load_count;
        self.load_count += 1;
        if previous != 0 {
            return;
        }
        let Some(reader) = reader else {
            let mut own = open(source, FILE);
            let (_, nickname) = api::misc::base_nickname(cx, 0x40, self.base_id);
            let nickname = nickname.unwrap_or_default();
            let mut found = false;
            while !found && own.read_header() {
                if !own.is_header(b"MBase") || !own.read_value() {
                    continue;
                }
                loop {
                    if own.is_value(b"nickname")
                        && stricmp(&nickname, c_str(&own.value_string())).is_eq()
                    {
                        found = true;
                        break;
                    }
                    if !own.read_value() {
                        break;
                    }
                }
            }
            self.load_sections(cx, &mut own);
            return;
        };
        self.load_sections(cx, reader);
    }

    fn load_sections(&mut self, cx: &mut Cx<'_>, reader: &mut IniReader) {
        let base = self.base_id;
        while reader.read_header() {
            if reader.is_header(b"MVendor") {
                while reader.read_value() {
                    if reader.is_value(b"num_offers") {
                        self.vendor_offers_min = reader.value_int(0);
                        self.vendor_offers_max = reader.value_int(1);
                    }
                }
            } else if reader.is_header(b"GF_NPC") {
                let mut npc = BaseNpc::default();
                let mut nickname = 0;
                load_base_npc(cx, &mut nickname, &mut npc, reader, base);
                self.npcs.entry(nickname).or_insert(npc);
            } else if reader.is_header(b"mRoom") {
                let mut room = BaseRoom::default();
                while reader.read_value() {
                    if reader.is_value(b"nickname") {
                        room.room_id = api::gf::find_location(base, c_str(&reader.value_string()));
                    } else if reader.is_value(b"character_density") {
                        room.character_density = reader.value_int(0);
                    } else if reader.is_value(b"fixture") {
                        let npc = create_id(c_str(&reader.value_string_at(0)));
                        if !self.npcs.contains_key(&npc) {
                            continue;
                        }
                        let placement = api::gf::character_placement_by_name(
                            cx,
                            room.room_id,
                            c_str(&reader.value_string_at(1)),
                        );
                        if let Ok(placement) = placement {
                            room.fixtures.push(BaseFixture {
                                npc,
                                placement: placement as i32,
                                fidget: tstring(&reader.value_string_at(2), 64),
                                role: tstring(&reader.value_string_at(3), 32),
                            });
                        }
                    }
                }
                if room.room_id & 0x3fff_ffff != 0 {
                    self.rooms.push(room);
                }
            } else if reader.is_header(b"mBase") {
                break;
            }
        }
        // Every `[BaseFaction]` NPC takes the faction as its affiliation. The
        // C++ writes a miss into the map's head node, which nothing reads.
        for faction in &self.factions {
            for id in &faction.npcs {
                if let Some(npc) = self.npcs.get_mut(id) {
                    npc.desc.affiliation = faction.group;
                }
            }
        }
    }

    /// `FContentBase::Unload`: the last of the loads drops the rooms and the
    /// GF_NPCs (and the room states, P3.6e).
    pub fn unload(&mut self) {
        self.load_count -= 1;
        if self.load_count != 0 {
            return;
        }
        self.rooms.clear();
        self.npcs.clear();
    }

    /// `FContentBase::FindFaction`: the first `[BaseFaction]` of `group`.
    #[must_use]
    pub fn find_faction(&self, group: u32) -> Option<&BaseFaction> {
        self.factions.iter().find(|f| f.group == group)
    }

    /// `FContentBase::GetRoomDensity`: a room's `character_density`, 0 when
    /// the base does not list it.
    #[must_use]
    pub fn room_density(&self, room: u32) -> i32 {
        self.rooms
            .iter()
            .find(|r| r.room_id == room)
            .map_or(0, |r| r.character_density)
    }
}

/// `GContentBases`: the records by base id.
pub type ContentBases = BTreeMap<u32, ContentBase>;

/// `BuildContentBases` (`:400-422`): `bases` gets one record per
/// `pub::NextBaseID` base (an existing record is kept), then the `[mBase]`
/// sections of `missions\mBases.ini` under `source`. The walk stops at the
/// first section that is not an `[mBase]` where one is expected: after the
/// last base, or after an `[mBase]` whose nickname is not a known base.
pub fn build_content_bases(cx: &mut Cx<'_>, bases: &mut ContentBases, source: &dyn DataSource) {
    let mut id = 0;
    while api::misc::next_base_id(cx, &mut id) {
        bases.entry(id).or_insert_with(|| ContentBase::new(id));
    }
    let single_player = api::misc::single_player(cx);
    let mut reader = open(source, FILE);
    reader.read_header();
    while !reader.is_end() {
        if !reader.is_header(b"mBase") {
            break;
        }
        reader.read_value();
        if let Ok(id) = api::misc::base_id(cx, c_str(&reader.value_string()))
            && let Some(base) = bases.get_mut(&id)
        {
            base.load_factions(cx, &mut reader, single_player);
            continue;
        }
        reader.read_header();
    }
}

/// `LoadDynBaseInfo` (`:425-447`), GameSupport's `LoadGame`: the table is
/// emptied and built again; the save's `[DynBaseInfo]` nicknames are
/// looked up and the result is dropped. Its call site is P3.4d's.
pub fn load_dyn_base_info(
    cx: &mut Cx<'_>,
    bases: &mut ContentBases,
    source: &dyn DataSource,
    save: &mut IniReader,
) {
    bases.clear();
    build_content_bases(cx, bases, source);
    while save.read_header() {
        if !save.is_header(b"DynBaseInfo") || !save.read_value() {
            continue;
        }
        loop {
            if save.is_value(b"nickname") {
                let _ = api::misc::base_id(cx, c_str(&save.value_string()));
                break;
            }
            if !save.read_value() {
                break;
            }
        }
    }
}
