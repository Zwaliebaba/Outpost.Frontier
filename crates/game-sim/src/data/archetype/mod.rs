//! Archetypes: the plain data of every kind of object the loaders read,
//! ported from `CppSource/Common/common_archetype*.cpp`.
//!
//! [`ArchetypeDb`] holds the seven C++ databases (simples, ships, solars,
//! equipment, asteroids, dynamic asteroids, motors), the explosions, and the
//! small-id table that maps archetype ids to the 16-bit ids the wire uses:
//! every record a database accepts is numbered from 1 in load order, the
//! same id only once.
//!
//! Left out on purpose (decision record 0001, point 7): the client
//! companion objects (`ICliObj`, `[LOD]`; divergence D94), and everything `Root::load`
//! creates later (mesh and script handles, collision surfaces, the
//! `BEAM_ARCH` compound). `DataConfig` always loads with `bReload` false,
//! so the reload branches (redefining an equipment, ship or simple record
//! in place) are not ported; a solar or motor named twice is redefined
//! without that flag, and that is ported.

pub mod eq_obj;
pub mod equipment;
pub mod misc;
pub mod object_type;
pub mod root;

use std::collections::BTreeMap;

use neuron_core::id::create_id;
use neuron_core::id::sub_object::CollisionGroupIds;
use neuron_core::ini::IniReader;

use self::eq_obj::{CollisionGroup, Ship, Solar};
use self::equipment::{Equipment, ReadCx, SECTIONS};
use self::misc::{Asteroid, Explosion, Motor, dynamic_asteroid};
use self::root::Root;
use super::goods::{EquipmentGoodFields, EquipmentLookup};
use super::string_cache::StringCache;
use super::{DataSource, Reports, open_ini};

/// The C++ source the archetype messages name.
pub(crate) const SOURCE: &str = "Common\\Archetype.cpp";

/// Every archetype database.
#[derive(Clone, Debug, Default)]
pub struct ArchetypeDb {
    /// `[Simple]` records.
    pub simples: BTreeMap<u32, Root>,
    /// `[Ship]` records.
    pub ships: BTreeMap<u32, Ship>,
    /// `[Solar]` records.
    pub solars: BTreeMap<u32, Solar>,
    /// Equipment records.
    pub equipment: BTreeMap<u32, Equipment>,
    /// `[Asteroid]` and `[AsteroidMine]` records.
    pub asteroids: BTreeMap<u32, Asteroid>,
    /// `[DynamicAsteroid]` records.
    pub dynamic_asteroids: BTreeMap<u32, Root>,
    /// `[Motor]` records.
    pub motors: BTreeMap<u32, Motor>,
    /// `[Explosion]` records.
    pub explosions: BTreeMap<u32, Explosion>,
    small_to_large: Vec<u32>,
    large_to_small: BTreeMap<u32, u16>,
}

/// The loader's context.
pub struct LoadCx<'a> {
    /// The string cache.
    pub strings: &'a mut StringCache,
    /// Where messages go.
    pub reports: &'a mut Reports,
}

fn no_nickname(reports: &mut Reports, kind: &str) {
    reports.push(
        SOURCE,
        0x84,
        format!("*** ERROR: Loaded '{kind}' has no nickname .. not adding to database."),
    );
}

fn get_miss(reports: &mut Reports, id: u32) {
    reports.push(SOURCE, 0x3c, format!("*** ERROR: ArchDB::Get({id}) failed"));
}

fn get_miss_by_name(reports: &mut Reports, id: u32) {
    reports.push(
        SOURCE,
        0x49,
        format!(
            "*** ERROR: ArchDB::Get(0x{id:x}) failed. ID={}",
            i32::from_ne_bytes(id.to_ne_bytes())
        ),
    );
}

impl ArchetypeDb {
    /// `RegisterLargeId`.
    fn register(&mut self, id: u32) {
        if self.large_to_small.contains_key(&id) {
            return;
        }
        self.small_to_large.push(id);
        let small = u16::try_from(self.small_to_large.len() & 0xFFFF).unwrap_or(0);
        self.large_to_small.insert(id, small);
    }

    /// `ArchDB::Insert`: `false` for nickname 0 (reported) or a taken id.
    fn insert<T>(
        map: &mut BTreeMap<u32, T>,
        id: u32,
        record: T,
        kind: &str,
        reports: &mut Reports,
    ) -> Result<(), T> {
        if id == 0 {
            no_nickname(reports, kind);
            return Err(record);
        }
        if map.contains_key(&id) {
            return Err(record);
        }
        map.insert(id, record);
        Ok(())
    }

    /// `LargeIDToSmallID`: 0 for an id that is not an archetype.
    #[must_use]
    pub fn large_to_small(&self, id: u32) -> u16 {
        self.large_to_small.get(&id).copied().unwrap_or(0)
    }

    /// `SmallIDToLargeID`: 0 for 0 or an id past the table.
    #[must_use]
    pub fn small_to_large(&self, small: u16) -> u32 {
        if small == 0 {
            return 0;
        }
        self.small_to_large
            .get(usize::from(small) - 1)
            .copied()
            .unwrap_or(0)
    }

    /// The small-id table: large ids by small id from 1.
    #[must_use]
    pub fn small_ids(&self) -> &[u32] {
        &self.small_to_large
    }

    /// `Archetype::LoadEquipment(path)`: the growth of the equipment and
    /// motor databases.
    pub fn load_equipment(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        cx: &mut LoadCx<'_>,
    ) -> usize {
        let saved = self.equipment.len() + self.motors.len();
        let Some(mut reader) = open_ini(source, path) else {
            return 0;
        };
        while reader.read_header() {
            if reader.is_header(b"Motor") {
                self.load_motor(&mut reader, cx.reports);
            } else if reader.is_header(b"Explosion") {
                self.load_explosion(&mut reader);
            } else if let Some(&(_, class)) = SECTIONS.iter().find(|(n, _)| reader.is_header(n)) {
                let mut record = Equipment::new(class);
                while reader.read_value() {
                    let mut read = ReadCx {
                        strings: cx.strings,
                        equipment: &self.equipment,
                        reports: cx.reports,
                    };
                    record.read(&mut reader, &mut read);
                }
                let id = record.root.nickname;
                if Self::insert(&mut self.equipment, id, record, "Equipment", cx.reports).is_ok() {
                    self.register(id);
                }
            }
        }
        self.equipment.len() + self.motors.len() - saved
    }

    /// `[Motor]`: a repeated nickname redefines the loaded motor. A motor
    /// without a nickname is reported and dropped (the reference calls
    /// through null there, divergence D89).
    fn load_motor(&mut self, reader: &mut IniReader, reports: &mut Reports) {
        let mut record = Motor::default();
        while reader.read_value() {
            record.read(reader);
        }
        let id = record.nickname;
        match Self::insert(&mut self.motors, id, record, "Motor", reports) {
            Ok(()) => self.register(id),
            Err(record) => match self.motors.get_mut(&id) {
                Some(live) => live.redefine(&record),
                None => get_miss_by_name(reports, id),
            },
        }
    }

    /// `Archetype::LoadExplosion`: the first `nickname` makes or finds the
    /// record; later keys of the section go to it, also for a nickname
    /// loaded before.
    pub fn load_explosion(&mut self, reader: &mut IniReader) -> bool {
        let mut live: Option<u32> = None;
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                if live.is_some() {
                    continue;
                }
                let id = create_id(&reader.value_string());
                self.explosions
                    .entry(id)
                    .or_insert_with(|| Explosion::new(id));
                live = Some(id);
            } else if let Some(record) = live.and_then(|id| self.explosions.get_mut(&id)) {
                record.read(reader);
            }
        }
        live.is_some()
    }

    /// `Archetype::LoadSolar(path)`: the growth of the solar database. A
    /// solar named again redefines the loaded one; one without a nickname
    /// is reported and dropped (the reference calls through null there,
    /// divergence D89).
    pub fn load_solar(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        cx: &mut LoadCx<'_>,
    ) -> usize {
        let saved = self.solars.len();
        let Some(mut reader) = open_ini(source, path) else {
            return 0;
        };
        let mut group_ids = CollisionGroupIds::default();
        let mut current: Option<u32> = None;
        while reader.read_header() {
            if reader.is_header(b"Solar") {
                let mut record = Solar::default();
                group_ids.reset();
                while reader.read_value() {
                    if !reader.is_value(b"inherit") {
                        record.read(&mut reader, cx.strings, cx.reports);
                        continue;
                    }
                    let id = create_id(&reader.value_string());
                    match self.solars.get(&id) {
                        Some(donor) => record.inherit(donor),
                        None => get_miss_by_name(cx.reports, id),
                    }
                }
                let id = record.eq_obj.root.nickname;
                match Self::insert(&mut self.solars, id, record, "Solar", cx.reports) {
                    Ok(()) => {
                        self.register(id);
                        current = Some(id);
                    }
                    Err(record) => {
                        match self.solars.get_mut(&id) {
                            Some(live) => live.redefine(&record),
                            None => get_miss_by_name(cx.reports, id),
                        }
                        current = None;
                    }
                }
            } else if reader.is_header(b"CollisionGroup") {
                match current.and_then(|id| self.solars.get_mut(&id)) {
                    Some(solar) => {
                        let group = CollisionGroup::read(
                            &mut reader,
                            group_ids.create(),
                            cx.strings,
                            cx.reports,
                        );
                        solar.eq_obj.collision_groups.push(group);
                    }
                    None => cx.reports.push(
                        SOURCE,
                        0x780,
                        "*** ERROR: Attempt to load a solar group into a solar that wasn't loaded.",
                    ),
                }
            } else if reader.is_header(b"Explosion") {
                self.load_explosion(&mut reader);
            }
        }
        self.solars.len() - saved
    }

    /// `Archetype::LoadShips(path)`: the growth of the ship database. A ship
    /// named again is reported and dropped.
    pub fn load_ships(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        cx: &mut LoadCx<'_>,
    ) -> usize {
        let saved = self.ships.len();
        let Some(mut reader) = open_ini(source, path) else {
            return 0;
        };
        let mut group_ids = CollisionGroupIds::default();
        let mut current: Option<u32> = None;
        while reader.read_header() {
            if reader.is_header(b"Ship") {
                let mut record = Ship::new(cx.strings);
                group_ids.reset();
                while reader.read_value() {
                    if !reader.is_value(b"inherit") {
                        record.read(&mut reader, cx.strings, cx.reports);
                        continue;
                    }
                    let id = create_id(&reader.value_string());
                    match self.ships.get(&id) {
                        Some(donor) => record.inherit(donor),
                        None => {
                            get_miss(cx.reports, id);
                            cx.reports.push(
                                SOURCE,
                                0x649,
                                format!("*** WARNING: 'inherit' failed. couldn't find 0x{id:x}"),
                            );
                        }
                    }
                }
                let id = record.eq_obj.root.nickname;
                if Self::insert(&mut self.ships, id, record, "Ship", cx.reports).is_ok() {
                    self.register(id);
                    current = Some(id);
                } else {
                    cx.reports.push(
                        SOURCE,
                        0x66b,
                        format!(
                            "*** WARNING: [Ship] nickname = 0x{id:x} value is not unique. \
                             Aborted second version."
                        ),
                    );
                    current = None;
                }
            } else if reader.is_header(b"CollisionGroup") {
                if let Some(ship) = current.and_then(|id| self.ships.get_mut(&id)) {
                    let group_id = group_ids.create();
                    if group_id == 0 {
                        cx.reports.push(
                            SOURCE,
                            0x67d,
                            format!(
                                "*** WARNING: [Ship] nickname = 0x{:x}: Exceeds maximum collision group",
                                ship.eq_obj.root.nickname
                            ),
                        );
                    } else {
                        let group =
                            CollisionGroup::read(&mut reader, group_id, cx.strings, cx.reports);
                        ship.eq_obj.collision_groups.push(group);
                    }
                }
            } else if reader.is_header(b"Simple") {
                self.load_simple(&mut reader, cx);
            } else if reader.is_header(b"Explosion") {
                self.load_explosion(&mut reader);
            }
        }
        self.ships.len() - saved
    }

    /// `[Simple]`: a simple named again is reported and dropped.
    fn load_simple(&mut self, reader: &mut IniReader, cx: &mut LoadCx<'_>) -> bool {
        let mut record = Root::default();
        while reader.read_value() {
            record.read(reader, cx.strings, cx.reports);
        }
        let id = record.nickname;
        if Self::insert(&mut self.simples, id, record, "Simple", cx.reports).is_ok() {
            self.register(id);
            return true;
        }
        cx.reports.push(
            SOURCE,
            0x364,
            format!(
                "*** WARNING: [Simple] nickname = 0x{id:x} value is not unique. \
                 Aborted second version."
            ),
        );
        false
    }

    /// `Archetype::LoadSimples(path)`: the growth of the simple database.
    pub fn load_simples(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        cx: &mut LoadCx<'_>,
    ) -> usize {
        let saved = self.simples.len();
        let Some(mut reader) = open_ini(source, path) else {
            return 0;
        };
        while reader.read_header() {
            if reader.is_header(b"Simple") {
                self.load_simple(&mut reader, cx);
            } else if reader.is_header(b"Explosion") {
                self.load_explosion(&mut reader);
            }
        }
        self.simples.len() - saved
    }

    /// `Archetype::LoadAsteroids(path)`: the growth of both asteroid
    /// databases.
    pub fn load_asteroids(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        cx: &mut LoadCx<'_>,
    ) -> usize {
        let saved = self.asteroids.len() + self.dynamic_asteroids.len();
        let Some(mut reader) = open_ini(source, path) else {
            return 0;
        };
        while reader.read_header() {
            if reader.is_header(b"Asteroid") || reader.is_header(b"AsteroidMine") {
                let mut record = Asteroid::default();
                while reader.read_value() {
                    if !reader.is_value(b"inherit") {
                        record.read(&mut reader, cx.strings, cx.reports);
                        continue;
                    }
                    let id = create_id(&reader.value_string());
                    match self.asteroids.get(&id) {
                        Some(donor) => record.inherit(donor),
                        None => {
                            get_miss(cx.reports, id);
                            cx.reports.push(
                                SOURCE,
                                0x125b,
                                format!("*** WARNING: 'inherit' failed. couldn't find 0x{id:x}"),
                            );
                        }
                    }
                }
                let id = record.root.nickname;
                if Self::insert(&mut self.asteroids, id, record, "Asteroid", cx.reports).is_ok() {
                    self.register(id);
                }
            } else if reader.is_header(b"DynamicAsteroid") {
                let mut record = dynamic_asteroid();
                while reader.read_value() {
                    record.read(&mut reader, cx.strings, cx.reports);
                }
                let id = record.nickname;
                if Self::insert(
                    &mut self.dynamic_asteroids,
                    id,
                    record,
                    "DynamicAsteroid",
                    cx.reports,
                )
                .is_ok()
                {
                    self.register(id);
                }
            }
        }
        self.asteroids.len() + self.dynamic_asteroids.len() - saved
    }

    /// `GetArchMaxHitPts`: ships, solars, then equipment; 1 for none.
    #[must_use]
    pub fn max_hit_pts(&self, id: u32) -> i32 {
        let hit_pts = self
            .ships
            .get(&id)
            .map(|s| s.eq_obj.root.hit_pts)
            .or_else(|| self.solars.get(&id).map(|s| s.eq_obj.root.hit_pts))
            .or_else(|| self.equipment.get(&id).map(|e| e.root.hit_pts));
        hit_pts.map_or(1, |h| neuron_core::num::trunc_to_i32(f64::from(h)))
    }
}

impl EquipmentLookup for ArchetypeDb {
    fn good_fields(&self, arch_id: u32) -> Option<EquipmentGoodFields> {
        let equipment = self.equipment.get(&arch_id)?;
        Some(EquipmentGoodFields {
            ids_name: equipment.root.ids_name,
            ids_info: equipment.root.ids_info,
            attached: equipment
                .attached()
                .map(|a| (equipment.root.da_archetype, a.hp_child)),
        })
    }
}
