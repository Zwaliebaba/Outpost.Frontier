//! Loadouts (`Loadout::Load`, `ReadEquipLine`, `ReadCargoLine`), ported
//! from `CppSource/Common/common_loadout.cpp`.
//!
//! A `[Loadout]` block names a loadout and lists its `equip` and `cargo`
//! lines. A loadout named again in a later block or file is emptied and
//! refilled. Equipment lines get sub-object ids from the loadout's own
//! counter (0x22 upwards); combinable cargo in the cargo bay folds into an
//! earlier line of the same archetype.

use std::collections::BTreeMap;

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::id::sub_object::EquipmentIds;
use neuron_core::ini::IniReader;
use neuron_core::msvcrt::{atof, atoi};

use super::equip_desc::EquipDesc;
use super::goods::GoodList;
use super::string_cache::StringCache;
use super::{DataSource, Reports, lossy, open_ini};

const SOURCE: &str = "Common\\Loadouts.cpp";

/// `Loadout::Map`.
#[derive(Clone, Debug)]
pub struct Loadout {
    /// The nickname id.
    pub nickname_id: u32,
    /// The lines, in order.
    pub items: Vec<EquipDesc>,
    /// The sub-object id counter.
    pub id_maker: EquipmentIds,
}

/// The loadout table.
#[derive(Clone, Debug, Default)]
pub struct LoadoutDb {
    loadouts: BTreeMap<u32, Loadout>,
}

/// Why a cargo line could not be added.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LoadoutError {
    /// An `equip` or `cargo` line before any `nickname`: the reference
    /// dereferences null.
    LineBeforeNickname,
    /// A combinable cargo line whose first match differs in mission flag,
    /// owner or hardpoint: the reference loops for ever.
    CargoMatchLoops,
}

/// `ParseArchToken`: `atoi` of the token, or `CreateID` of it when that
/// is 0.
fn arch_token(text: &[u8]) -> u32 {
    let as_int = atoi(text);
    if as_int != 0 {
        u32::from_ne_bytes(as_int.to_ne_bytes())
    } else {
        create_id(text)
    }
}

/// `Loadout::ReadEquipLine`.
pub fn read_equip_line(reader: &mut IniReader, desc: &mut EquipDesc, strings: &mut StringCache) {
    desc.equipped = 1;
    desc.count = 1;
    desc.arch_id = arch_token(&reader.value_string_at(0));
    if reader.is_value_empty(1) {
        desc.make_internal(strings);
    } else {
        desc.hardpoint = strings.alloc(&reader.value_string_at(1), false);
    }
    if !reader.is_value_empty(2) {
        desc.health = reader.value_float(2) as f32;
    }
}

/// `Loadout::ReadCargoLine`. A hardpoint field that reads as a non-zero
/// number ends the line before health and mission flag.
pub fn read_cargo_line(reader: &mut IniReader, desc: &mut EquipDesc, strings: &mut StringCache) {
    desc.equipped = 0;
    desc.arch_id = arch_token(&reader.value_string_at(0));
    if !reader.is_value_empty(1) {
        desc.count = reader.value_int(1);
    }
    if reader.is_value_empty(2) {
        desc.make_internal(strings);
    } else {
        let field = reader.value_string_at(2);
        if atof(&field) != 0.0 {
            return;
        }
        desc.hardpoint = strings.alloc(&field, false);
    }
    if !reader.is_value_empty(3) {
        desc.health = reader.value_float(3) as f32;
    }
    if !reader.is_value_empty(4) {
        desc.mission = reader.value_bool(4);
    }
}

/// `EquipDescVector::add_equipment_item`: folds `desc` into the first line
/// of the same archetype when `combinable` (`Ok(true)`), else appends it
/// (`Ok(false)`).
///
/// # Errors
///
/// [`LoadoutError::CargoMatchLoops`] where the reference never returns.
pub fn add_equipment_item(
    items: &mut Vec<EquipDesc>,
    desc: &EquipDesc,
    combinable: bool,
    strings: &StringCache,
) -> Result<bool, LoadoutError> {
    if combinable && let Some(found) = items.iter_mut().find(|i| i.arch_id == desc.arch_id) {
        if found.mission != desc.mission
            || found.owner != desc.owner
            || !eq_ignore_case(strings.get(found.hardpoint), strings.get(desc.hardpoint))
        {
            return Err(LoadoutError::CargoMatchLoops);
        }
        if found.health >= 1.0 || found.health.is_nan() {
            found.count = found.count.wrapping_add(desc.count);
            found.health = desc.health;
        } else if desc.health >= 1.0 || desc.health.is_nan() {
            found.count = found.count.wrapping_add(desc.count);
        } else {
            let sum = f64::from(found.health) + f64::from(desc.health);
            let total = desc.count.wrapping_add(found.count);
            if sum >= 1.0 {
                found.count = total;
                found.health = (sum - 1.0) as f32;
            } else {
                found.health = sum as f32;
                found.count = total.wrapping_sub(1);
            }
        }
        return Ok(true);
    }
    items.push(*desc);
    Ok(false)
}

impl LoadoutDb {
    /// `Loadout::Get`.
    #[must_use]
    pub fn get(&self, id: u32) -> Option<&Loadout> {
        self.loadouts.get(&id)
    }

    /// Every loadout in id order.
    pub fn iter(&self) -> impl Iterator<Item = &Loadout> {
        self.loadouts.values()
    }

    /// How many loadouts there are.
    #[must_use]
    pub fn len(&self) -> usize {
        self.loadouts.len()
    }

    /// Whether there are none.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.loadouts.is_empty()
    }

    /// `Loadout::Free`.
    pub fn clear(&mut self) {
        self.loadouts.clear();
    }

    /// `Loadout::Load(path)`: the growth of the table. Lines the reference
    /// cannot survive (see [`LoadoutError`]) are reported and skipped.
    pub fn load(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        strings: &mut StringCache,
        goods: &GoodList,
        reports: &mut Reports,
    ) -> usize {
        let saved = self.loadouts.len();
        let Some(mut reader) = open_ini(source, path) else {
            reports.push(
                SOURCE,
                0xb3,
                format!("*** ERROR: unable to open Loadout file {}", lossy(path)),
            );
            return 0;
        };
        while reader.read_header() {
            if !reader.is_header(b"Loadout") {
                continue;
            }
            let mut current: Option<u32> = None;
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    if current.is_some() {
                        continue;
                    }
                    let id = create_id(&reader.value_string());
                    let entry = self.loadouts.entry(id).or_insert_with(|| Loadout {
                        nickname_id: 0,
                        items: Vec::new(),
                        id_maker: EquipmentIds::default(),
                    });
                    entry.nickname_id = id;
                    entry.items.clear();
                    entry.id_maker.reset();
                    current = Some(id);
                } else if reader.is_value(b"equip") || reader.is_value(b"cargo") {
                    let equip = reader.is_value(b"equip");
                    let Some(loadout) = current.and_then(|id| self.loadouts.get_mut(&id)) else {
                        report_error(reports, LoadoutError::LineBeforeNickname, &reader);
                        continue;
                    };
                    let mut desc = EquipDesc::new(strings);
                    if equip {
                        read_equip_line(&mut reader, &mut desc, strings);
                        desc.id = loadout.id_maker.create();
                        loadout.items.push(desc);
                    } else {
                        read_cargo_line(&mut reader, &mut desc, strings);
                        let combinable =
                            goods.arch_is_combinable(desc.arch_id) && desc.is_internal(strings);
                        match add_equipment_item(&mut loadout.items, &desc, combinable, strings) {
                            Ok(false) => {
                                let id = loadout.id_maker.create();
                                if let Some(last) = loadout.items.last_mut() {
                                    last.id = id;
                                }
                            }
                            Ok(true) => {}
                            Err(error) => report_error(reports, error, &reader),
                        }
                    }
                }
            }
        }
        self.loadouts.len() - saved
    }
}

fn report_error(reports: &mut Reports, error: LoadoutError, reader: &IniReader) {
    reports.push(
        SOURCE,
        0,
        format!(
            "{error:?} at {}: {}",
            lossy(reader.file_name()),
            lossy(&reader.line())
        ),
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cargo_folds() {
        let strings = StringCache::new();
        let mut items = Vec::new();
        let mut desc = EquipDesc::new(&strings);
        desc.arch_id = 5;
        desc.health = 0.75;
        assert_eq!(
            add_equipment_item(&mut items, &desc, true, &strings),
            Ok(false)
        );
        assert_eq!(
            add_equipment_item(&mut items, &desc, true, &strings),
            Ok(true)
        );
        assert_eq!(items[0].count, 2);
        assert_eq!(items[0].health, 0.5);
        desc.mission = true;
        assert_eq!(
            add_equipment_item(&mut items, &desc, true, &strings),
            Err(LoadoutError::CargoMatchLoops)
        );
    }

    #[test]
    fn loadouts_load() {
        struct One(&'static [u8]);
        impl DataSource for One {
            fn read(&self, _path: &[u8]) -> Option<Vec<u8>> {
                Some(self.0.to_vec())
            }
        }
        let text = b"[Loadout]\nnickname = a\nequip = gun, HpWeapon01\nequip = 12\n\
cargo = ammo, 5\ncargo = ammo, 3, 2.5\n[Loadout]\nnickname = a\nequip = x\n";
        let mut strings = StringCache::new();
        let mut reports = Reports::default();
        let mut db = LoadoutDb::default();
        let goods = GoodList::default();
        assert_eq!(
            db.load(&One(text), b"l.ini", &mut strings, &goods, &mut reports),
            1
        );
        let a = db.get(create_id(b"a")).unwrap();
        assert_eq!(a.items.len(), 1);
        assert_eq!(a.items[0].id, 0x22);
    }
}
