//! The random missions: the `DestroyMission` controller
//! (`Content/DestroyMission.cpp`, `Content/RandomMissionDB.cpp` and the
//! `RandomMission*.cpp` files; record 0008 points 2 and 8).
//!
//! Its create parameter is the reference-counted `FRandomMissionData`
//! (`RandomMission.h:21-90`), which the port keeps in a table of Content's
//! state ([`mission::RmMissions`]) and names by [`RmDataKey`]. P3.7c ports
//! the mission data with its `[RandomMission]` save section, the loader
//! (`CreateMission`, `Load`, the offer description and `CanAccept`;
//! [`mission`]) and the bar and vendor offers ([`vendor`]). The
//! constructor's 1 s heartbeat (`RandomMissionDB.cpp:1041`), the router
//! handlers and the run time are P3.7b's; until then a random mission
//! instance holds its key and one reference.
//!
//! P3.7a ports what builds an offer from a seed: the generator's tables
//! ([`tables`]), the vignette tree ([`vignette`]), the offers
//! ([`offer`]) and the generator ([`generator`]). The tables and the
//! tree are what the C++'s static generator object loads when Content.dll
//! loads (`GRandomMissionGeneratorInstance`, `RandomMissionGen.cpp:1090`);
//! they never change afterwards, so they are [`RmData`] behind an `Arc` in
//! [`Content`](super::Content). The counters and lists the generator
//! changes are [`RmState`] in Content's state.

pub mod generator;
pub mod mission;
pub mod offer;
pub mod tables;
pub mod vendor;
pub mod vignette;

use game_sim::data::DataSource;
use neuron_server::controller::ControllerId;

use super::Instance;
use super::tables::open;
use crate::server::{CreateParams, Cx};

/// A key into Content's table of random-mission data (the C++'s
/// `FRandomMissionData*`, counted as `FContentRefObject` counts it; P3.7).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RmDataKey(pub u32);

/// A `DestroyMission` controller's own state (P3.7 adds the mission
/// object and its machines).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RandomMission {
    /// Its mission data; none for parameters of another form (D770).
    pub data: Option<RmDataKey>,
}

/// `CreateDestroyMission`: the instance with its mission data, which it
/// holds one reference to (`FDestroyMission`'s constructor AddRefs it; the
/// rest of the constructor is P3.7b's, D955).
pub fn construct(cx: &mut Cx<'_>, _id: ControllerId, params: &CreateParams) -> Instance {
    let data = match *params {
        CreateParams::RandomMission(key) => Some(key),
        _ => None,
    };
    if let Some(key) = data {
        cx.logic.content.state.rm.missions.add_ref(key);
    }
    Instance::RandomMission(RandomMission { data })
}

/// The `DestroyMission` destructor's release of its mission data (the
/// rest of `~FDestroyMission` is P3.7b's, D955).
pub fn destruct(cx: &mut Cx<'_>, id: ControllerId) {
    let key = match cx.logic.content.instance(id) {
        Some(Instance::RandomMission(r)) => r.data,
        _ => None,
    };
    if let Some(key) = key {
        mission::release(cx, key);
    }
}

/// The files the generator's constructor opens, in its order
/// (`RandomMissionGen.cpp:320-373`).
pub mod files {
    /// `RandomMissions\NpcRankToDiff.ini`.
    pub const NPC_RANK_TO_DIFF: &[u8] = b"RandomMissions\\NpcRankToDiff.ini";
    /// `RandomMissions\Diff2Money.ini`.
    pub const DIFF2MONEY: &[u8] = b"RandomMissions\\Diff2Money.ini";
    /// `RandomMissions\RMLootInfo.ini`.
    pub const RM_LOOT_INFO: &[u8] = b"RandomMissions\\RMLootInfo.ini";
    /// `RandomMissions\SolarFormations.ini`.
    pub const SOLAR_FORMATIONS: &[u8] = b"RandomMissions\\SolarFormations.ini";
    /// `RandomMissions\KillableSolars.ini`.
    pub const KILLABLE_SOLARS: &[u8] = b"RandomMissions\\KillableSolars.ini";
    /// `RandomMissions\VignetteCriticalLoot.ini`.
    pub const VIGNETTE_CRITICAL_LOOT: &[u8] = b"RandomMissions\\VignetteCriticalLoot.ini";
    /// `RandomMissions\VignetteParams.ini`.
    pub const VIGNETTE_PARAMS: &[u8] = b"RandomMissions\\VignetteParams.ini";
}

/// What the static generator object loads (`FRandomMissionGenerator`'s
/// constructor): its tables and its vignette tree.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RmData {
    /// The tables.
    pub tables: tables::RmTables,
    /// The vignette tree.
    pub tree: vignette::VignetteTree,
}

impl RmData {
    /// The generator's constructor (`RandomMissionGen.cpp:313-374`): each
    /// file is read only when it opens.
    pub fn load(cx: &Cx<'_>, source: &dyn DataSource) -> Self {
        let mut d = Self::default();
        let mut reader = open(source, files::NPC_RANK_TO_DIFF);
        tables::load_npc_rank_to_difficulty(&mut d.tables.npc_rank_to_difficulty, &mut reader);
        let mut reader = open(source, files::DIFF2MONEY);
        tables::load_diff2money(&mut d.tables.diff2money, &mut reader);
        let mut reader = open(source, files::RM_LOOT_INFO);
        tables::load_rm_loot_info(cx, &mut d.tables, &mut reader);
        let mut reader = open(source, files::SOLAR_FORMATIONS);
        tables::load_solar_formations(&mut d.tables.solar_formations, &mut reader);
        let mut reader = open(source, files::KILLABLE_SOLARS);
        tables::load_killable_solars(cx, &mut d.tables.killable_solars, &mut reader);
        let mut reader = open(source, files::VIGNETTE_CRITICAL_LOOT);
        tables::load_vignette_critical_loot(cx, &mut d.tables.critical_loot, &mut reader);
        // `Load` runs only when the file opens; a closed reader would still
        // link an empty tree, which the C++ does not.
        if let Some(mut reader) = game_sim::data::open_ini(source, files::VIGNETTE_PARAMS) {
            d.tree.load(cx, &mut reader);
        }
        d
    }
}

/// The random-mission globals the generator changes (record 0008 point 8,
/// "state outside the seed"), and the live missions.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RmState {
    /// The counter of the `Vignette_ship_%X` nicknames
    /// (`RandomMissionGen.cpp:1998`).
    pub ship_counter: u32,
    /// The counter of the `Vignette_solar_%X` nicknames (`:1857`).
    pub solar_counter: u32,
    /// `GRmShipGroupSerial` (`RandomMissionRuntime.cpp:199`): the serial of
    /// the last ship group built.
    pub ship_group_serial: u32,
    /// `GRmDestSystems` (`RandomMissionDB.cpp:395`): the systems loaded for
    /// random missions, each holding one load until they are released.
    pub dest_systems: Vec<u32>,
    /// `GRmObjectiveZones` (`DestroyMission.cpp:43`): the zones the live
    /// missions hold as objectives (P3.7c).
    pub objective_zones: Vec<u32>,
    /// The live missions (`FRandomMissionData`), by key (P3.7c).
    pub missions: mission::RmMissions,
}

/// `RegisterRmDestSystem` (`RandomMissionDB.cpp:400-410`): a system not
/// listed yet is appended and its record loaded without encounters.
pub fn register_dest_system(cx: &mut Cx<'_>, system: u32) {
    let listed = &mut cx.logic.content.state.rm.dest_systems;
    if listed.contains(&system) {
        return;
    }
    listed.push(system);
    super::systems::load(cx, system, false);
}
