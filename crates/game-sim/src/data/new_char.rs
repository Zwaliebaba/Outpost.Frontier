//! The new-character database (`NewChar::Database`), ported from
//! `CppSource/Common/common_newchar.cpp`.
//!
//! `[Data] NewCharDB` reads `NewCharacter.ini`: the factions a new player
//! can start in, the ship packages and the pilot looks. Each record goes in
//! under its nickname id unless the id is already there. A faction's `base`
//! lines also fill the extra table (base id to base nickname) that the
//! server reads for `%%Home_Base%%`. Nothing here interns strings.

use std::collections::{BTreeMap, BTreeSet};

use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32;

use super::{DataSource, Reports, lossy, open_ini};

const SOURCE: &str = "Common\\NewCharDB.cpp";

/// A `[Faction]` (`FNewCharFaction`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Faction {
    /// `nickname`, its id.
    pub nickname: u32,
    /// `rep_group`.
    pub rep_group: Vec<u8>,
    /// `base`, the ids.
    pub bases: BTreeSet<u32>,
    /// `package`, the ids.
    pub packages: BTreeSet<u32>,
    /// `pilot`, the ids.
    pub pilots: BTreeSet<u32>,
}

/// A `[Package]` (`FNewCharPackage`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Package {
    /// `nickname`, its id.
    pub nickname: u32,
    /// `strid_name`.
    pub strid_name: i32,
    /// `strid_desc`.
    pub strid_desc: i32,
    /// `ship`.
    pub ship: Vec<u8>,
    /// `loadout`.
    pub loadout: Vec<u8>,
    /// `money`.
    pub money: i32,
}

/// A `[Pilot]` (`FNewCharPilot`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Pilot {
    /// `nickname`, its id.
    pub nickname: u32,
    /// `body`.
    pub body: Vec<u8>,
    /// `body.anim`, every parameter up to the first empty one.
    pub body_anims: Vec<Vec<u8>>,
    /// `comm`.
    pub comm: Vec<u8>,
    /// `comm.anim`.
    pub comm_anims: Vec<Vec<u8>>,
    /// `voice`.
    pub voice: Vec<u8>,
    /// `thumb`.
    pub thumb: Vec<u8>,
}

/// `NewChar::Database`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NewCharDb {
    /// Factions by id.
    pub factions: BTreeMap<u32, Faction>,
    /// Packages by id.
    pub packages: BTreeMap<u32, Package>,
    /// Pilots by id.
    pub pilots: BTreeMap<u32, Pilot>,
    /// Base id to base nickname (`FNewCharExtra`).
    pub extras: BTreeMap<u32, Vec<u8>>,
}

/// Every parameter of the current value up to the first empty one.
fn parameters(reader: &IniReader) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    let mut index = 0;
    while !reader.is_value_empty(index) {
        out.push(reader.value_string_at(index));
        index += 1;
    }
    out
}

impl NewCharDb {
    /// `Load`: `false` (reported) when the file cannot be opened.
    ///
    /// A `[Package]` without `strid_name`, `strid_desc` or `money` keeps the
    /// previous package's value: the C++ never initialises those fields of
    /// its stack record. The first package starts from 0 (the original
    /// started from whatever its stack held).
    pub fn load(&mut self, source: &dyn DataSource, path: &[u8], reports: &mut Reports) -> bool {
        let Some(mut reader) = open_ini(source, path) else {
            reports.push(
                SOURCE,
                0x1cd,
                format!(
                    "*** ERROR: failed to open configuration file ({})",
                    lossy(path)
                ),
            );
            return false;
        };
        let mut carried = Package::default();
        while reader.read_header() {
            if reader.is_header(b"Faction") {
                let faction = self.read_faction(&mut reader);
                self.factions.entry(faction.nickname).or_insert(faction);
            } else if reader.is_header(b"Package") {
                let mut package = Package {
                    nickname: 0,
                    ship: Vec::new(),
                    loadout: Vec::new(),
                    ..carried.clone()
                };
                read_package(&mut reader, &mut package);
                carried = package.clone();
                self.packages.entry(package.nickname).or_insert(package);
            } else if reader.is_header(b"Pilot") {
                let pilot = read_pilot(&mut reader);
                self.pilots.entry(pilot.nickname).or_insert(pilot);
            }
        }
        true
    }

    fn read_faction(&mut self, reader: &mut IniReader) -> Faction {
        let mut faction = Faction::default();
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                faction.nickname = create_id(&reader.value_string());
            } else if reader.is_value(b"rep_group") {
                faction.rep_group = reader.value_string();
            } else if reader.is_value(b"base") {
                let text = reader.value_string();
                let id = create_id(&text);
                self.extras.entry(id).or_insert(text);
                faction.bases.insert(id);
            } else if reader.is_value(b"package") {
                faction.packages.insert(create_id(&reader.value_string()));
            } else if reader.is_value(b"pilot") {
                faction.pilots.insert(create_id(&reader.value_string()));
            }
        }
        faction
    }
}

fn read_package(reader: &mut IniReader, package: &mut Package) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            package.nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"strid_name") {
            package.strid_name = trunc_to_i32(reader.value_float(0));
        } else if reader.is_value(b"strid_desc") {
            package.strid_desc = trunc_to_i32(reader.value_float(0));
        } else if reader.is_value(b"ship") {
            package.ship = reader.value_string();
        } else if reader.is_value(b"loadout") {
            package.loadout = reader.value_string();
        } else if reader.is_value(b"money") {
            package.money = trunc_to_i32(reader.value_float(0));
        }
    }
}

fn read_pilot(reader: &mut IniReader) -> Pilot {
    let mut pilot = Pilot::default();
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            pilot.nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"body") {
            pilot.body = reader.value_string();
        } else if reader.is_value(b"body.anim") {
            pilot.body_anims = parameters(reader);
        } else if reader.is_value(b"comm") {
            pilot.comm = reader.value_string();
        } else if reader.is_value(b"comm.anim") {
            pilot.comm_anims = parameters(reader);
        } else if reader.is_value(b"voice") {
            pilot.voice = reader.value_string();
        } else if reader.is_value(b"thumb") {
            pilot.thumb = reader.value_string();
        }
    }
    pilot
}

#[cfg(test)]
mod tests {
    use super::*;

    struct One(&'static [u8]);

    impl DataSource for One {
        fn read(&self, _path: &[u8]) -> Option<Vec<u8>> {
            Some(self.0.to_vec())
        }
    }

    #[test]
    fn records_and_carried_package_fields() {
        let source = One(b"[Pilot]\nnickname = p\ncomm.anim = a, b\n\
            [Package]\nnickname = k1\nstrid_name = 5.9\nmoney = 100\n\
            [Package]\nnickname = k2\nship = s\n\
            [Faction]\nnickname = f\nbase = B1\nbase = B1\npackage = k1\n\
            [Faction]\nnickname = f\nrep_group = other\n");
        let mut db = NewCharDb::default();
        assert!(db.load(&source, b"x", &mut Reports::default()));
        let k2 = &db.packages[&create_id(b"k2")];
        assert_eq!((k2.strid_name, k2.money, &k2.ship[..]), (5, 100, &b"s"[..]));
        assert_eq!(db.pilots[&create_id(b"p")].comm_anims.len(), 2);
        let f = &db.factions[&create_id(b"f")];
        assert!(f.rep_group.is_empty());
        assert_eq!(f.bases.len(), 1);
        assert_eq!(db.extras[&create_id(b"B1")], b"B1");
    }
}
