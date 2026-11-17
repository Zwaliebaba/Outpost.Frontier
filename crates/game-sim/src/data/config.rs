//! The `[Data]` list and the load of every database (`DataConfig::startup`),
//! ported from `CppSource/Common/common_dataconfig.cpp`.
//!
//! `DataConfig::startup` reads every `[Data]` section of the engine INI
//! (`OutpostFrontier.ini`) and, value by value, calls the loader the key
//! names. Several keys may repeat (`equipment`, `goods`, `loadouts`, ...);
//! the order matters, because later files override or extend earlier ones,
//! the string cache keeps the first spelling of a name, and the universe
//! looks up solar archetypes. [`DataConfig`] holds the list; [`GameData::load`]
//! runs it.
//!
//! The C++ reference count (a second `startup` loads nothing) is the
//! caller's business here: a [`GameData`] is loaded once. The loaders of
//! later work packages are listed in [`DataKind`] and skipped:
//! `bodyparts`, `costumes` (P5.9), `NewCharDB` (P3), `RtcSlider` (P5),
//! `PetalDB` and `WeaponModDB` (with the equipment in P2.4); the AI state
//! graph that `startup` reads after the list (`AI\state_graph.db`) belongs
//! to P2.5. Keys `DataConfig` does not dispatch (`fonts`, `sounds`,
//! `effects`, ...) are read by other subsystems and kept in the list.

use neuron_core::hash::eq_ignore_case;
use neuron_core::ini::IniReader;

use super::constants::Constants;
use super::goods::{GoodList, GoodsContext};
use super::loadout::LoadoutDb;
use super::reputation::Reputation;
use super::string_cache::StringCache;
use super::{DataSource, Reports, lossy, open_ini};

const SOURCE: &str = "Common\\DataConfig.cpp";

/// One value of a `[Data]` section: the key and its whole value text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DataEntry {
    /// The key, as written.
    pub key: Vec<u8>,
    /// The value text (`get_value_string()`).
    pub value: Vec<u8>,
}

/// The loader a `[Data]` key selects, in the order `DispatchDataValue`
/// tests the keys (case-insensitive).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DataKind {
    /// `universe`: `Universe::Startup`.
    Universe,
    /// `constants`: `ReadConstants`.
    Constants,
    /// `equipment`: `Archetype::LoadEquipment`.
    Equipment,
    /// `solar`: `Archetype::LoadSolar`.
    Solar,
    /// `ships`: `Archetype::LoadShips`.
    Ships,
    /// `asteroids`: `Archetype::LoadAsteroids`.
    Asteroids,
    /// `loadouts`: `Loadout::Load`.
    Loadouts,
    /// `bodyparts`: `LoadBodypartDescriptions` (not part of P2.1).
    Bodyparts,
    /// `costumes`: `LoadCostumeDescriptions` (not part of P2.1).
    Costumes,
    /// `bases`: `BaseDataList_load` (the value is not read).
    Bases,
    /// `goods`: `GoodList_load`.
    Goods,
    /// `markets`: `BaseDataList_load_market_data`.
    Markets,
    /// `PetalDB`: `PetalDB_load` (not part of P2.1).
    PetalDb,
    /// `Groups`: `Reputation::Startup`.
    Groups,
    /// `explosions`: `Archetype::LoadSimples`.
    Explosions,
    /// `fuses`: `FuseDB::AddFuseINIFile`.
    Fuses,
    /// `WeaponModDB`: `WeaponModDB_load` (not part of P2.1).
    WeaponModDb,
    /// `NewCharDB`: `NewChar::TheDB->Load` (not part of P2.1).
    NewCharDb,
    /// `RtcSlider`: `RtcSlider::load` (not part of P2.1).
    RtcSlider,
}

const KINDS: [(&[u8], DataKind); 19] = [
    (b"universe", DataKind::Universe),
    (b"constants", DataKind::Constants),
    (b"equipment", DataKind::Equipment),
    (b"solar", DataKind::Solar),
    (b"ships", DataKind::Ships),
    (b"asteroids", DataKind::Asteroids),
    (b"loadouts", DataKind::Loadouts),
    (b"bodyparts", DataKind::Bodyparts),
    (b"costumes", DataKind::Costumes),
    (b"bases", DataKind::Bases),
    (b"goods", DataKind::Goods),
    (b"markets", DataKind::Markets),
    (b"PetalDB", DataKind::PetalDb),
    (b"Groups", DataKind::Groups),
    (b"explosions", DataKind::Explosions),
    (b"fuses", DataKind::Fuses),
    (b"WeaponModDB", DataKind::WeaponModDb),
    (b"NewCharDB", DataKind::NewCharDb),
    (b"RtcSlider", DataKind::RtcSlider),
];

impl DataKind {
    /// The loader `key` selects, or `None` for a key `DataConfig` ignores.
    #[must_use]
    pub fn of(key: &[u8]) -> Option<Self> {
        KINDS
            .iter()
            .find(|(name, _)| eq_ignore_case(key, name))
            .map(|&(_, kind)| kind)
    }

    /// Whether this work package ports the loader.
    #[must_use]
    pub fn is_ported(self) -> bool {
        !matches!(
            self,
            Self::Bodyparts
                | Self::Costumes
                | Self::PetalDb
                | Self::WeaponModDb
                | Self::NewCharDb
                | Self::RtcSlider
        )
    }
}

/// Every value of every `[Data]` section, in file order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DataConfig {
    /// The values.
    pub entries: Vec<DataEntry>,
}

impl DataConfig {
    /// The `[Data]` values of an open reader.
    pub fn parse(reader: &mut IniReader) -> Self {
        let mut entries = Vec::new();
        while reader.read_header() {
            if !reader.is_header(b"Data") {
                continue;
            }
            while reader.read_value() {
                entries.push(DataEntry {
                    key: reader.name(),
                    value: reader.value_string(),
                });
            }
        }
        Self { entries }
    }

    /// The list of the engine INI at `path` (read through `source`), or
    /// `None` when it cannot be opened: `startup` then loads nothing.
    pub fn read(source: &dyn DataSource, path: &[u8]) -> Option<Self> {
        open_ini(source, path).map(|mut reader| Self::parse(&mut reader))
    }
}

/// Every database `DataConfig::startup` fills, owned in one place.
#[derive(Clone, Debug, Default)]
pub struct GameData {
    /// The string cache every loader interns through.
    pub strings: StringCache,
    /// `ReadConstants`.
    pub constants: Constants,
    /// `GoodList_load`.
    pub goods: GoodList,
    /// `Loadout::Load`.
    pub loadouts: LoadoutDb,
    /// `Reputation::Startup`.
    pub reputation: Reputation,
    /// `FDUMP` reports of the load.
    pub reports: Reports,
}

/// How a load ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LoadOutcome {
    /// Every value was dispatched (`startup` returns 0).
    Complete,
    /// The progress callback asked to stop (`startup` returns 1).
    Aborted,
}

impl GameData {
    /// The empty databases of a process that has not loaded anything.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `DataConfig::startup` over `config`: each value is dispatched in
    /// order. `progress` is `ProgressCB`: it sees each value text first and
    /// stops the load by returning `true`; that value is not dispatched.
    pub fn load(
        &mut self,
        source: &dyn DataSource,
        config: &DataConfig,
        mut progress: Option<&mut dyn FnMut(&[u8]) -> bool>,
    ) -> LoadOutcome {
        for entry in &config.entries {
            if let Some(callback) = progress.as_mut()
                && callback(&entry.value)
            {
                return LoadOutcome::Aborted;
            }
            if let Some(kind) = DataKind::of(&entry.key) {
                self.dispatch(source, kind, &entry.value);
            }
        }
        LoadOutcome::Complete
    }

    /// One `[Data]` value (`DispatchDataValue`).
    pub fn dispatch(&mut self, source: &dyn DataSource, kind: DataKind, value: &[u8]) {
        match kind {
            DataKind::Constants => {
                self.constants.read(source, value, &mut self.reports);
            }
            DataKind::Goods => {
                let mut cx = GoodsContext {
                    strings: &mut self.strings,
                    equipment: &(),
                    reports: &mut self.reports,
                };
                self.goods.load(source, value, &mut cx);
            }
            DataKind::Loadouts => {
                let loaded = self.loadouts.load(
                    source,
                    value,
                    &mut self.strings,
                    &self.goods,
                    &mut self.reports,
                );
                if loaded == 0 {
                    self.reports.push(
                        SOURCE,
                        0x8e,
                        format!(
                            "*** WARNING: LOADOUTS file '{}' contains no new definitions.",
                            lossy(value)
                        ),
                    );
                }
            }
            DataKind::Groups => {
                self.reputation.startup(source, value);
            }
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn data_sections_in_order() {
        let mut reader = IniReader::new();
        assert!(reader.open_memory(
            b"[Data]\nsolar = a.ini\nbases\n[Other]\nx = 1\n[data]\nGOODS = g.ini ; c\n"
        ));
        let config = DataConfig::parse(&mut reader);
        let keys: Vec<&[u8]> = config.entries.iter().map(|e| &e.key[..]).collect();
        assert_eq!(keys, [&b"solar"[..], b"bases", b"GOODS"]);
        assert_eq!(config.entries[2].value, b"g.ini");
        assert_eq!(DataKind::of(b"GOODS"), Some(DataKind::Goods));
        assert_eq!(DataKind::of(b"groups"), Some(DataKind::Groups));
        assert_eq!(DataKind::of(b"fonts"), None);
        assert!(!DataKind::Costumes.is_ported());
    }

    #[test]
    fn progress_can_stop_the_load() {
        struct Nothing;
        impl DataSource for Nothing {
            fn read(&self, _path: &[u8]) -> Option<Vec<u8>> {
                None
            }
        }
        let config = DataConfig {
            entries: vec![
                DataEntry {
                    key: b"constants".to_vec(),
                    value: b"a".to_vec(),
                },
                DataEntry {
                    key: b"constants".to_vec(),
                    value: b"b".to_vec(),
                },
            ],
        };
        let mut seen = Vec::new();
        let mut stop = |value: &[u8]| {
            seen.push(value.to_vec());
            value == b"b"
        };
        let mut data = GameData::new();
        assert_eq!(
            data.load(&Nothing, &config, Some(&mut stop)),
            LoadOutcome::Aborted
        );
        assert_eq!(seen.len(), 2);
    }
}
