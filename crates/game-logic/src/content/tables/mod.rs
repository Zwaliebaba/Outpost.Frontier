//! Content's static tables: what `LoadContentGameTables`
//! (`Content/Encounters.cpp:497-591`) reads when GameSupport is constructed,
//! as typed data (decision record 0008 point 5, work package P3.4b).
//!
//! [`ContentTables::load`] runs the loaders in the C++ order, each a port of
//! its C++ loader over neuron-core's [`IniReader`] and the host's
//! [`DataSource`] (D690). A file that does not open reads as empty, as the
//! C++'s untested open does; a loader the C++ runs over a closed reader
//! still runs (the voice lines are sorted again). Nothing in the tables
//! changes after the load, so Content's state holds them in an `Arc`
//! (record 0008 point 1).
//!
//! `std::map` is a [`BTreeMap`] and `std::vector` a [`Vec`] in insertion
//! order; the tables the C++ sorts are sorted with the VC6 `std::sort`
//! ([`game_sim::data::vc6::vc6_sort`]), which fixes the order of equal
//! keys. Ids are `CreateID` values. The `pub::` getters the loaders call
//! are those of [`crate::api`]; their reports (a body part or base the data
//! lacks) go to the server's reports as the C++'s `FDUMP` lines do, and the
//! reader's own reports are kept in [`ContentTables::ini_reports`].

pub mod factions;
pub mod formations;
pub mod gcs;
pub mod loot;
pub mod news;
pub mod npc_ships;
pub mod pilots;
pub mod ranks;
pub mod ship_classes;
pub mod voices;

use std::collections::BTreeMap;

use game_sim::data::costume::{Costume, MAX_ACCESSORIES};
use game_sim::data::{DataSource, open_ini};
use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::{IniReader, IniReport};

use crate::api;
use crate::server::Cx;

pub use factions::{FactionEmpathy, FactionNames, FactionProps, Factions};
pub use formations::Formation;
pub use loot::{LootProps, PhantomLoot};
pub use news::NewsItem;
pub use npc_ships::NpcShipArch;
pub use pilots::{PilotRefs, Pilots};
pub use ranks::{RankDifficulty, ToughnessPoint};
pub use voices::{VoiceLine, VoiceProperties};

/// The files `LoadContentGameTables` opens, in its order, relative to the
/// data root.
pub mod files {
    /// `audio\voices_space_male.ini`.
    pub const VOICES_SPACE_MALE: &[u8] = b"audio\\voices_space_male.ini";
    /// `audio\voices_space_female.ini`.
    pub const VOICES_SPACE_FEMALE: &[u8] = b"audio\\voices_space_female.ini";
    /// `audio\voices_recognizable.ini`.
    pub const VOICES_RECOGNIZABLE: &[u8] = b"audio\\voices_recognizable.ini";
    /// `missions\empathy.ini`.
    pub const EMPATHY: &[u8] = b"missions\\empathy.ini";
    /// `missions\voice_properties.ini`.
    pub const VOICE_PROPERTIES: &[u8] = b"missions\\voice_properties.ini";
    /// `missions\formations.ini`.
    pub const FORMATIONS: &[u8] = b"missions\\formations.ini";
    /// `missions\pilots_population.ini`.
    pub const PILOTS_POPULATION: &[u8] = b"missions\\pilots_population.ini";
    /// `missions\pilots_story.ini`.
    pub const PILOTS_STORY: &[u8] = b"missions\\pilots_story.ini";
    /// `missions\npcships.ini`.
    pub const NPC_SHIPS: &[u8] = b"missions\\npcships.ini";
    /// `missions\npcships_test.ini`.
    pub const NPC_SHIPS_TEST: &[u8] = b"missions\\npcships_test.ini";
    /// `missions\faction_prop.ini`.
    pub const FACTION_PROPS: &[u8] = b"missions\\faction_prop.ini";
    /// `equipment\commodities_per_faction.ini`.
    pub const FACTION_GOODS: &[u8] = b"equipment\\commodities_per_faction.ini";
    /// `missions\shipclasses.ini`.
    pub const SHIP_CLASSES: &[u8] = b"missions\\shipclasses.ini";
    /// `ships\shiparch.ini`.
    pub const SHIP_ARCH: &[u8] = b"ships\\shiparch.ini";
    /// `missions\lootprops.ini`.
    pub const LOOT_PROPS: &[u8] = b"missions\\lootprops.ini";
    /// `scripts\GCS\GCSExcl.ini`.
    pub const GCS_EXCLUSIONS: &[u8] = b"scripts\\GCS\\GCSExcl.ini";
    /// `missions\PTough.ini`.
    pub const TOUGHNESS: &[u8] = b"missions\\PTough.ini";
    /// `missions\RankDiff.ini`.
    pub const RANK_DIFFICULTY: &[u8] = b"missions\\RankDiff.ini";
    /// `missions\news.ini`.
    pub const NEWS: &[u8] = b"missions\\news.ini";
}

/// Content's static tables (`LoadContentGameTables`), in its order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ContentTables {
    /// `GVoiceLines`: the `[Sound]` durations of the three voice files,
    /// sorted by voice and message.
    pub voice_lines: Vec<VoiceLine>,
    /// `GVoiceProperties`: the `[mVoiceProp]` records of the voice files and
    /// `missions\voice_properties.ini`.
    pub voice_properties: Vec<VoiceProperties>,
    /// `GFactionEmpathy`, by group.
    pub empathy: BTreeMap<u32, FactionEmpathy>,
    /// `GFormations`, by nickname id.
    pub formations: BTreeMap<u32, Formation>,
    /// The pilot personalities' seventeen maps.
    pub pilots: Pilots,
    /// `GNpcShipArchs`, by nickname id.
    pub npc_ships: BTreeMap<u32, NpcShipArch>,
    /// `GFactionProps`, `GFactionNames` and `GFactionGroups`.
    pub factions: Factions,
    /// `GRmShipClasses`: class id to member ids.
    pub ship_classes: BTreeMap<u32, Vec<u32>>,
    /// `GRmShipMissionProperties`: ship archetype to property ids.
    pub ship_mission_properties: BTreeMap<u32, Vec<u32>>,
    /// `GLootProps`, in file order.
    pub loot_props: Vec<LootProps>,
    /// `GPhantomLoots`, in file order.
    pub phantom_loot: Vec<PhantomLoot>,
    /// `GGcsExclusions`: voice id to the scripts it must not play.
    pub gcs_exclusions: BTreeMap<u32, Vec<Vec<u8>>>,
    /// `GPlayerToughnessScale`.
    pub toughness_scale: Vec<ToughnessPoint>,
    /// `GRmRankDifficulties`, sorted by stage.
    pub rank_difficulties: Vec<RankDifficulty>,
    /// `GNewsItems`, in file order.
    pub news: Vec<NewsItem>,
    /// The reports of the readers (`get_value_bool` and `ParseNumber`), in
    /// load order.
    pub ini_reports: Vec<IniReport>,
}

impl ContentTables {
    /// `LoadContentGameTables` over `source`, the host's data root.
    ///
    /// `cx` answers the `pub::` getters (reputation groups, goods, ships,
    /// costumes, body parts, bases, state graphs and `SinglePlayer`); the
    /// getters' reports go to `cx.logic.reports`.
    pub fn load(cx: &mut Cx<'_>, source: &dyn DataSource) -> Self {
        let mut t = Self::default();
        for path in [
            files::VOICES_SPACE_MALE,
            files::VOICES_SPACE_FEMALE,
            files::VOICES_RECOGNIZABLE,
        ] {
            let mut reader = open(source, path);
            voices::load_voice_lines(&mut t.voice_lines, &mut reader);
            reader.reset();
            voices::load_voice_properties(&mut t.voice_properties, &mut reader);
            t.keep(&mut reader);
        }
        // `LoadFactionEmpathy` opens the file itself.
        let mut reader = open(source, files::EMPATHY);
        factions::load_empathy(cx, &mut t.empathy, &mut reader);
        t.keep(&mut reader);

        let mut reader = open(source, files::VOICE_PROPERTIES);
        voices::load_voice_properties(&mut t.voice_properties, &mut reader);
        t.keep(&mut reader);

        let mut reader = open(source, files::FORMATIONS);
        while reader.read_header() {
            if reader.is_header(b"Formation") {
                formations::load_formation(&mut t.formations, &mut reader);
            }
        }
        t.keep(&mut reader);

        for path in [files::PILOTS_POPULATION, files::PILOTS_STORY] {
            let mut reader = open(source, path);
            pilots::load_pilot_personalities(&mut t.pilots, &mut reader);
            t.keep(&mut reader);
        }

        for path in [files::NPC_SHIPS, files::NPC_SHIPS_TEST] {
            let mut reader = open(source, path);
            npc_ships::load_npc_ship_archs(cx, &mut t.npc_ships, &mut reader);
            t.keep(&mut reader);
        }

        let mut reader = open(source, files::FACTION_PROPS);
        factions::load_faction_props(cx, &mut t.factions, &mut reader);
        t.keep(&mut reader);

        let mut reader = open(source, files::FACTION_GOODS);
        factions::load_faction_goods(cx, &mut t.factions, &mut reader);
        t.keep(&mut reader);

        let mut reader = open(source, files::SHIP_CLASSES);
        ship_classes::load_ship_classes(&mut t.ship_classes, &mut reader);
        t.keep(&mut reader);

        let mut reader = open(source, files::SHIP_ARCH);
        ship_classes::load_ship_mission_properties(&mut t.ship_mission_properties, &mut reader);
        t.keep(&mut reader);

        let mut reader = open(source, files::LOOT_PROPS);
        loot::load_loot_props(&mut t.loot_props, &mut reader);
        reader.reset();
        loot::load_phantom_loot(&mut t.phantom_loot, &mut reader);
        t.keep(&mut reader);

        // `LoadGcsExclusions` opens the file itself.
        let mut reader = open(source, files::GCS_EXCLUSIONS);
        gcs::load_gcs_exclusions(&mut t.gcs_exclusions, &mut reader);
        t.keep(&mut reader);

        let single_player = api::misc::single_player(cx);
        let mut reader = open(source, files::TOUGHNESS);
        ranks::load_toughness_scale(&mut t.toughness_scale, &mut reader, single_player);
        t.keep(&mut reader);

        let mut reader = open(source, files::RANK_DIFFICULTY);
        ranks::load_rank_difficulties(&mut t.rank_difficulties, &mut reader);
        t.keep(&mut reader);

        let mut reader = open(source, files::NEWS);
        news::load_news_items(cx, &mut t.news, &mut reader);
        t.keep(&mut reader);
        t
    }

    /// [`ContentTables::load`] over the world's data root; every file reads
    /// as empty when the world has none. The files are read before the
    /// loaders run, so the loaders can take the context.
    pub fn load_from_world(cx: &mut Cx<'_>) -> Self {
        let files = match cx.world.data_source() {
            Some(source) => Preread::read(source),
            None => Preread::default(),
        };
        Self::load(cx, &files)
    }

    fn keep(&mut self, reader: &mut IniReader) {
        self.ini_reports.extend(reader.take_reports());
    }

    /// `FindFormation`.
    #[must_use]
    pub fn formation(&self, id: u32) -> Option<&Formation> {
        self.formations.get(&id)
    }

    /// `FindNpcShipArch`.
    #[must_use]
    pub fn npc_ship(&self, id: u32) -> Option<&NpcShipArch> {
        self.npc_ships.get(&id)
    }

    /// `GetFactionProps`.
    #[must_use]
    pub fn faction_props(&self, group: u32) -> Option<&FactionProps> {
        self.factions.props.get(&group)
    }

    /// `GetFactionNames`.
    #[must_use]
    pub fn faction_names(&self, group: u32) -> Option<&FactionNames> {
        self.factions.names.get(&group)
    }

    /// `FindShipClass`.
    #[must_use]
    pub fn ship_class(&self, id: u32) -> Option<&[u32]> {
        self.ship_classes.get(&id).map(Vec::as_slice)
    }

    /// `GetFactionEventRate`: 0 for a group without empathy.
    #[must_use]
    pub fn faction_event_rate(&self, group: u32, event: usize) -> f32 {
        self.empathy
            .get(&group)
            .and_then(|e| e.event_rates.get(event))
            .copied()
            .unwrap_or(0.0)
    }
}

/// The files of [`files`] read into memory.
#[derive(Default)]
struct Preread(BTreeMap<&'static [u8], Vec<u8>>);

impl Preread {
    fn read(source: &dyn DataSource) -> Self {
        let all = [
            files::VOICES_SPACE_MALE,
            files::VOICES_SPACE_FEMALE,
            files::VOICES_RECOGNIZABLE,
            files::EMPATHY,
            files::VOICE_PROPERTIES,
            files::FORMATIONS,
            files::PILOTS_POPULATION,
            files::PILOTS_STORY,
            files::NPC_SHIPS,
            files::NPC_SHIPS_TEST,
            files::FACTION_PROPS,
            files::FACTION_GOODS,
            files::SHIP_CLASSES,
            files::SHIP_ARCH,
            files::LOOT_PROPS,
            files::GCS_EXCLUSIONS,
            files::TOUGHNESS,
            files::RANK_DIFFICULTY,
            files::NEWS,
        ];
        Self(
            all.into_iter()
                .filter_map(|path| source.read(path).map(|bytes| (path, bytes)))
                .collect(),
        )
    }
}

impl DataSource for Preread {
    fn read(&self, path: &[u8]) -> Option<Vec<u8>> {
        self.0.get(c_str(path)).cloned()
    }
}

/// `OpenContentDataIni`: the reader on the file, or a closed one, which
/// reads nothing.
pub(crate) fn open(source: &dyn DataSource, path: &[u8]) -> IniReader {
    open_ini(source, path).unwrap_or_default()
}

/// `pub::GetVoiceID` into a slot: `CreateID` of a non-empty name; an empty
/// one leaves the slot.
pub(crate) fn voice_id(slot: &mut u32, name: &[u8]) {
    if let Ok(id) = api::misc::voice_id(name) {
        *slot = id;
    }
}

/// `pub::GetShipID` into a slot: it writes the archetype id of any
/// non-empty name, valid or not.
fn ship_id(slot: &mut u32, name: &[u8]) {
    if !c_str(name).is_empty() {
        *slot = create_id(c_str(name));
    }
}

/// `pub::GetLoadoutID` into a slot: a non-zero id, else the slot is left.
fn loadout_id(slot: &mut u32, name: &[u8]) {
    if let Ok(id) = api::misc::loadout_id(name) {
        *slot = id;
    }
}

/// `pub::GetGoodID` into a slot: a known good's id (whatever its low bits),
/// else the slot is left.
fn good_id(cx: &Cx<'_>, slot: &mut u32, name: &[u8]) {
    let name = c_str(name);
    if name.is_empty() {
        return;
    }
    if let Some(good) = cx.world.data().goods.find_by_name(name) {
        *slot = good.id;
    }
}

/// `pub::SpaceObj::GetArchetypeID`: `Good2Arch`, written 0 or not.
fn archetype_id(cx: &Cx<'_>, good: u32) -> u32 {
    api::space_obj::archetype_id(cx, good).unwrap_or(0)
}

/// `pub::GetCostumeID` into a slot: written (0 for an unknown costume)
/// for a non-empty name.
fn costume_id(cx: &Cx<'_>, slot: &mut i32, name: &[u8]) {
    match api::misc::costume_id(cx, name) {
        Ok(id) => *slot = id,
        Err(api::PubError::BAD_ID) => *slot = 0,
        Err(_) => {}
    }
}

/// `pub::Reputation::GetReputationGroup`: `0xffffffff` for an unknown or
/// empty nickname, as the C++ writes it.
pub(crate) fn reputation_group(cx: &Cx<'_>, name: &[u8]) -> u32 {
    api::reputation::reputation_group(cx, c_str(name)).unwrap_or(u32::MAX)
}

/// `pub::Reputation::GetShortGroupName`: the name id, `0xffffffff` for
/// none, as the C++ writes it.
fn short_group_name(cx: &Cx<'_>, group: u32) -> u32 {
    api::reputation::short_group_name(cx, group).unwrap_or(u32::MAX)
}

/// `pub::GetBaseID`: 0 first, then the base of a known nickname.
fn base_id(cx: &mut Cx<'_>, name: &[u8]) -> u32 {
    api::misc::base_id(cx, name).unwrap_or(0)
}

/// `ParseSpaceCostume` (`NpcShips.cpp:144-156`): head (value 0) and body
/// (value 1) by `GetBodyPart`, then each further value's accessory
/// appended. True when both parts resolved. The C++ writes accessories past
/// the eight slots; the port keeps eight (D782).
fn space_costume(cx: &mut Cx<'_>, out: &mut Costume, reader: &IniReader) -> bool {
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
    out.part_ids[0] != 0 && out.part_ids[1] != 0
}

/// The story stage names and their values (`GContentStageNames`); stage 6
/// has no name.
pub const STAGE_NAMES: [(&[u8], i32); 42] = [
    (b"base_0_rank", 0x00),
    (b"mission_01a_loaded", 0x01),
    (b"mission_01a_accepted", 0x02),
    (b"mission_01b_loaded", 0x03),
    (b"mission_01b_accepted", 0x04),
    (b"freetime_01_02", 0x05),
    (b"mission_02_loaded", 0x07),
    (b"mission_02_accepted", 0x08),
    (b"freetime_02_03", 0x09),
    (b"mission_03_loaded", 0x0a),
    (b"mission_03_accepted", 0x0b),
    (b"freetime_03_04", 0x0c),
    (b"mission_04_loaded", 0x0d),
    (b"mission_04_accepted", 0x0e),
    (b"freetime_04_05", 0x0f),
    (b"mission_05_loaded", 0x10),
    (b"mission_05_accepted", 0x11),
    (b"freetime_05_06", 0x12),
    (b"mission_06_loaded", 0x13),
    (b"mission_06_accepted", 0x14),
    (b"freetime_06_07", 0x15),
    (b"mission_07_loaded", 0x16),
    (b"mission_07_accepted", 0x17),
    (b"freetime_07_08", 0x18),
    (b"mission_08_loaded", 0x19),
    (b"mission_08_accepted", 0x1a),
    (b"freetime_08_09", 0x1b),
    (b"mission_09_loaded", 0x1c),
    (b"mission_09_accepted", 0x1d),
    (b"freetime_09_10", 0x1e),
    (b"mission_10_loaded", 0x1f),
    (b"mission_10_accepted", 0x20),
    (b"freetime_10_11", 0x21),
    (b"mission_11_loaded", 0x22),
    (b"mission_11_accepted", 0x23),
    (b"freetime_11_12", 0x24),
    (b"mission_12_loaded", 0x25),
    (b"mission_12_accepted", 0x26),
    (b"freetime_12_13", 0x27),
    (b"mission_13_loaded", 0x28),
    (b"mission_13_accepted", 0x29),
    (b"mission_end", 0x2a),
];

/// The stage table walk: the first name equal without regard to case sets
/// `value`; a miss leaves it.
pub(crate) fn stage_value(name: &[u8], value: &mut i32) {
    if let Some((_, v)) = STAGE_NAMES.iter().find(|(n, _)| eq_ignore_case(name, n)) {
        *value = *v;
    }
}
