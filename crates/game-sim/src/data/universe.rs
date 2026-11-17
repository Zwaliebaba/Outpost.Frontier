//! The universe: star systems, bases, zones and jump connections
//! (`Universe::Startup`), ported from `CppSource/Common/common_universe.cpp`,
//! `common_universe_system.cpp` and `common_universe_base.cpp`.
//!
//! `universe.ini` lists `[System]` and `[Base]` blocks. A system's `file`
//! key reads that system's own INI on the spot, so its zones and
//! connections are loaded in the middle of the `[System]` block with the
//! nickname read so far. Two values carry from one block to the next
//! because the C++ never resets them: a system's position (a `[System]`
//! without `pos` keeps the previous one) and an `[Object]`'s nickname (an
//! object without one uses the previous object's). Both start at zero, as in
//! the C++ reference (the original started from whatever its stack held).
//!
//! The `[Object]` records of a system file (`GetSystemObject`) are read
//! again from the file on demand; [`Universe::system_objects`] does the
//! same. Each system file is first read for its reputation vibes
//! (`SolarReps::LoadObjectVibesFromSystemIni`, [`super::vibe`]) and then
//! again from the top; the vibes of the whole load form the static store.

use std::collections::BTreeMap;

use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;

use super::archetype::ArchetypeDb;
use super::goods::tstring;
use super::reputation::Reputation;
use super::string_cache::{CacheString, StringCache};
use super::vibe::{VibeCx, VibeDb};
use super::zone::Zone;
use super::{DataSource, Reports, lossy, open_ini, value_f32};

const SOURCE: &str = "Common\\Systems.cpp";

/// The mask `IdIsValid` tests.
const VALID_ID: u32 = 0x3FFF_FFFF;

/// Systems that need no `msg_id_prefix` (`kSystemsWithoutPrefix`).
const WITHOUT_PREFIX: [&[u8]; 11] = [
    b"Ep01",
    b"Ep02",
    b"Ep03",
    b"Ep02b",
    b"Ep02c",
    b"Ep03",
    b"Ep03b",
    b"Tut",
    b"Oe05",
    b"RELAYSEVEN_system",
    b"Oe06",
];

/// `ESolarGateType`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum GateType {
    /// Not a gate (0).
    #[default]
    None,
    /// `JUMP_GATE` (1).
    JumpGate,
    /// `JUMP_HOLE` (2).
    JumpHole,
    /// `AIRLOCK_GATE` (3).
    Airlock,
}

impl GateType {
    /// The C++ value.
    #[must_use]
    pub fn value(self) -> u32 {
        match self {
            Self::None => 0,
            Self::JumpGate => 1,
            Self::JumpHole => 2,
            Self::Airlock => 3,
        }
    }

    fn from_object_type(object_type: u32) -> Self {
        if object_type & 0x40 != 0 {
            Self::JumpGate
        } else if object_type & 0x800 != 0 {
            Self::JumpHole
        } else if object_type & 0x400 != 0 {
            Self::Airlock
        } else {
            Self::None
        }
    }
}

/// `FSystemConnection`: an `[Object]` with a `goto`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Connection {
    /// The target system.
    pub goto_system: u32,
    /// The object's own `nickname` in this section (0 when it has none).
    pub object: u32,
    /// The nickname carried from earlier objects when this one has none.
    pub object_id: u32,
    /// `pos` (-0.1 each when not given).
    pub pos: Vector,
    /// `visit`.
    pub visit: u8,
    /// From the object's solar archetype type.
    pub gate_type: GateType,
}

/// A system (`SYSTEM`).
#[derive(Clone, Debug, PartialEq)]
pub struct System {
    /// `nickname`: its id.
    pub nickname_id: u32,
    /// `nickname`.
    pub nickname: CacheString,
    /// `msg_id_prefix` (`TString<64>`).
    pub msg_id_prefix: Vec<u8>,
    /// `file`, relative to the universe directory.
    pub file: CacheString,
    /// `strid_name`.
    pub strid_name: i32,
    /// `ids_info`.
    pub ids_info: i32,
    /// `visit`.
    pub visit: u8,
    /// `NavMapScale` (1).
    pub nav_map_scale: f32,
    /// `pos` / `real_pos`.
    pub pos: Vector,
    /// The `[Object]`s with `goto`, in file order.
    pub connections: Vec<Connection>,
    /// The zones the zone map took, in file order (their ids).
    pub zones: Vec<u32>,
    /// `[Music] space`.
    pub space_music: u32,
}

impl System {
    fn new(pos: Vector) -> Self {
        Self {
            nickname_id: 0,
            nickname: CacheString::EMPTY,
            msg_id_prefix: Vec::new(),
            file: CacheString::EMPTY,
            strid_name: 0,
            ids_info: 0,
            visit: 0,
            nav_map_scale: 1.0,
            pos,
            connections: Vec::new(),
            zones: Vec::new(),
            space_music: 0,
        }
    }
}

/// A base (`BASE`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Base {
    /// `nickname`: its id.
    pub nickname_id: u32,
    /// `nickname`.
    pub nickname: CacheString,
    /// `strid_name`.
    pub strid_name: i32,
    /// `file`.
    pub file: CacheString,
    /// `system`.
    pub system: u32,
    /// `BGCS_base_run_by`.
    pub bgcs_base_run_by: Vec<u8>,
    /// The space object that docks here (set from a system's `[Object]`).
    pub space_object: u32,
    /// That object's `visit`.
    pub space_object_visit: u8,
    /// `ship_sml_01` to `ship_sml_03`.
    pub ship_sml: [u32; 3],
    /// `ship_mdm_01` to `ship_mdm_03`.
    pub ship_mdm: [u32; 3],
    /// `ship_lrg_01` to `ship_lrg_03`.
    pub ship_lrg: [u32; 3],
    /// `terrain_tiny`.
    pub terrain_tiny: u32,
    /// `terrain_sml`.
    pub terrain_sml: u32,
    /// `terrain_mdm`.
    pub terrain_mdm: u32,
    /// `terrain_lrg`.
    pub terrain_lrg: u32,
    /// `terrain_dyna_01`.
    pub terrain_dyna_01: u32,
    /// `terrain_dyna_02`.
    pub terrain_dyna_02: u32,
    /// `autosave_forbidden`.
    pub autosave_forbidden: bool,
}

/// `FSystemObject`: an `[Object]` of a system file as `GetSystemObject`
/// returns it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SystemObject {
    /// `nickname` (`TString<64>`).
    pub nickname: Vec<u8>,
    /// `pos`.
    pub pos: Vector,
    /// `archetype`.
    pub archetype: u32,
    /// `ids_name`.
    pub ids_name: i32,
    /// `ids_info` / `ids_info_card` / `infocard_ids`.
    pub ids_info: i32,
    /// `reputation` (`TString<32>`).
    pub reputation: Vec<u8>,
    /// `dock_with`.
    pub dock_with: u32,
    /// `goto`.
    pub goto: u32,
    /// `base`, 0 when it is not a base.
    pub base: u32,
    /// `next_ring` (`TString<64>`).
    pub next_ring: Vec<u8>,
    /// `prev_ring` (`TString<64>`).
    pub prev_ring: Vec<u8>,
    /// `visit`; 1 for a `star`.
    pub visit: u8,
}

/// The universe database.
#[derive(Clone, Debug)]
pub struct Universe {
    /// Systems by id.
    pub systems: BTreeMap<u32, System>,
    /// Bases by id.
    pub bases: BTreeMap<u32, Base>,
    /// Zones by id.
    pub zones: BTreeMap<u32, Zone>,
    /// `get_gate_system`: gate object id to target system.
    pub gate_systems: BTreeMap<u32, u32>,
    /// `DaysPerSecond` (`[Time] seconds_per_day`, 1/60 by default).
    pub days_per_second: f64,
    /// The directory of `universe.ini`, which system files are relative to.
    pub directory: Vec<u8>,
}

impl Default for Universe {
    fn default() -> Self {
        Self {
            systems: BTreeMap::new(),
            bases: BTreeMap::new(),
            zones: BTreeMap::new(),
            gate_systems: BTreeMap::new(),
            days_per_second: 1.0 / 60.0,
            directory: Vec::new(),
        }
    }
}

/// The loader's context.
pub struct UniverseCx<'a> {
    /// The string cache.
    pub strings: &'a mut StringCache,
    /// The solar archetypes (gate types, vibe ranks).
    pub archetypes: &'a ArchetypeDb,
    /// The factions loaded so far (vibe affiliations).
    pub reputation: &'a Reputation,
    /// The static vibe store the load refills.
    pub vibes: &'a mut VibeDb,
    /// Where messages go.
    pub reports: &'a mut Reports,
}

/// The values that carry between blocks (`FUniverseLoadCarry`).
#[derive(Clone, Copy, Default)]
struct Carry {
    system_pos: Vector,
    object_nickname: u32,
}

fn vector3(reader: &mut IniReader) -> Vector {
    Vector {
        x: value_f32(reader, 0),
        y: value_f32(reader, 1),
        z: value_f32(reader, 2),
    }
}

/// The directory part of a path, separator included (`_splitpath`).
fn directory_of(path: &[u8]) -> Vec<u8> {
    let path = neuron_core::hash::c_str(path);
    match path.iter().rposition(|&b| b == b'\\' || b == b'/') {
        Some(end) => path[..=end].to_vec(),
        None => Vec::new(),
    }
}

impl Universe {
    /// The path of a system file (`BuildSystemFilePath`).
    #[must_use]
    pub fn system_file_path(&self, file: &[u8]) -> Vec<u8> {
        let mut path = self.directory.clone();
        path.extend_from_slice(neuron_core::hash::c_str(file));
        path
    }

    /// `Universe::Startup(path)`: clears the universe and reads
    /// `universe.ini`. Reports when it holds no system.
    pub fn startup(&mut self, source: &dyn DataSource, path: &[u8], cx: &mut UniverseCx<'_>) {
        *self = Self {
            directory: directory_of(path),
            ..Self::default()
        };
        let mut carry = Carry::default();
        cx.vibes.free_static();
        if let Some(mut reader) = open_ini(source, path) {
            cx.vibes.begin_static_load();
            while reader.read_header() {
                if reader.is_header(b"Time") {
                    while reader.read_value() {
                        if reader.is_value(b"seconds_per_day") {
                            self.days_per_second = 1.0 / reader.value_float(0);
                            break;
                        }
                    }
                } else if reader.is_header(b"System") {
                    let mut system = System::new(carry.system_pos);
                    self.read_system(source, &mut reader, &mut system, &mut carry, cx);
                    carry.system_pos = system.pos;
                    self.insert_system(system, cx);
                } else if reader.is_header(b"Base") {
                    let base = read_base(&mut reader, cx.strings);
                    self.insert_base(base, cx);
                }
            }
            cx.vibes.end_static_load();
        }
        if self.systems.is_empty() {
            cx.reports.push(
                SOURCE,
                0xc2d,
                format!("*** ERROR: could not create universe '{}'", lossy(path)),
            );
        }
    }

    fn read_system(
        &mut self,
        source: &dyn DataSource,
        reader: &mut IniReader,
        system: &mut System,
        carry: &mut Carry,
        cx: &mut UniverseCx<'_>,
    ) {
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                let text = reader.value_string();
                system.nickname_id = create_id(&text);
                system.nickname = cx.strings.alloc(&text, false);
            } else if reader.is_value(b"msg_id_prefix") {
                system.msg_id_prefix = tstring(&reader.value_string(), 64);
            } else if reader.is_value(b"file") {
                system.file = cx.strings.alloc(&reader.value_string(), false);
                let path = self.system_file_path(cx.strings.get(system.file));
                self.load_system_file(source, &path, system, carry, cx);
            } else if reader.is_value(b"strid_name") {
                system.strid_name = reader.value_int(0);
            } else if reader.is_value(b"ids_info") {
                system.ids_info = reader.value_int(0);
            } else if reader.is_value(b"visit") {
                system.visit = reader.value_int(0).to_le_bytes()[0];
            } else if reader.is_value(b"NavMapScale") {
                system.nav_map_scale = value_f32(reader, 0);
            } else if reader.is_value(b"pos") {
                system.pos.x = value_f32(reader, 0);
                system.pos.y = value_f32(reader, 1);
                system.pos.z = if reader.is_value_empty(2) {
                    0.0
                } else {
                    value_f32(reader, 2)
                };
            } else if reader.is_value(b"real_pos") {
                system.pos = vector3(reader);
            }
        }
    }

    fn insert_system(&mut self, system: System, cx: &mut UniverseCx<'_>) {
        let name = cx.strings.get(system.nickname).to_vec();
        let no_prefix = system.msg_id_prefix.is_empty();
        if system.nickname_id & VALID_ID == 0 {
            cx.reports.push(
                SOURCE,
                0xb39,
                format!(
                    "*** ERROR: Failed to insert system \"{}\" into system DB due to invalid ID.",
                    lossy(&name)
                ),
            );
        } else if let std::collections::btree_map::Entry::Vacant(slot) =
            self.systems.entry(system.nickname_id)
        {
            slot.insert(system);
        } else {
            cx.reports.push(
                SOURCE,
                0xb34,
                "*** ERROR: Failed to insert new system into system DB due to ID collision.",
            );
        }
        if no_prefix && !WITHOUT_PREFIX.contains(&&name[..]) {
            cx.reports.push(
                SOURCE,
                0xb49,
                format!(
                    "*** ERROR: System \"{}\" has no msg_id_prefix information, which it will \
                     need to be referred to by space GCS - See TAYBRO",
                    lossy(&name)
                ),
            );
        }
    }

    fn insert_base(&mut self, base: Base, cx: &mut UniverseCx<'_>) {
        if base.nickname_id & VALID_ID == 0 {
            cx.reports.push(
                SOURCE,
                0xbc0,
                format!(
                    "*** ERROR: Failed to insert base \"{}\" into system DB due to invalid ID.",
                    cx.strings.lossy(base.nickname)
                ),
            );
        } else if let std::collections::btree_map::Entry::Vacant(slot) =
            self.bases.entry(base.nickname_id)
        {
            slot.insert(base);
        } else {
            cx.reports.push(
                SOURCE,
                0xbbb,
                "*** ERROR: Failed to insert new base into system DB due to ID collision.",
            );
        }
    }

    /// The system-file half of the loader: `[Zone]`, `[Music]` and the
    /// `[Object]` keys the universe needs. A missing file is skipped.
    fn load_system_file(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        system: &mut System,
        carry: &mut Carry,
        cx: &mut UniverseCx<'_>,
    ) {
        let Some(mut reader) = open_ini(source, path) else {
            return;
        };
        let mut vibe_cx = VibeCx {
            archetypes: cx.archetypes,
            reputation: cx.reputation,
            reports: cx.reports,
        };
        cx.vibes.load_object_vibes(&mut reader, &mut vibe_cx);
        reader.reset();
        while reader.read_header() {
            if reader.is_header(b"Zone") {
                let mut zone = Zone::new(system.nickname_id);
                zone.read(&mut reader, cx.strings, cx.reports);
                let id = zone.nickname;
                if id != 0 && !self.zones.contains_key(&id) {
                    self.zones.insert(id, zone);
                    system.zones.push(id);
                }
            } else if reader.is_header(b"Music") {
                while reader.read_value() {
                    if reader.is_value(b"space") {
                        system.space_music = create_id(&reader.value_string());
                    }
                }
            } else if reader.is_header(b"Object") {
                self.read_file_object(&mut reader, system, carry, cx);
            }
        }
    }

    /// `ReadSystemFileObject`.
    fn read_file_object(
        &mut self,
        reader: &mut IniReader,
        system: &mut System,
        carry: &mut Carry,
        cx: &mut UniverseCx<'_>,
    ) {
        let mut nickname = 0;
        let mut base = 0;
        let mut ids_name = 0;
        let mut archetype = 0;
        let mut goto = 0;
        let mut visit = 0u8;
        let mut saw_goto = false;
        let mut pos = Vector {
            x: -0.1,
            y: -0.1,
            z: -0.1,
        };
        if !reader.read_value() {
            return;
        }
        let mut carried = carry.object_nickname;
        loop {
            if reader.is_value(b"nickname") {
                nickname = create_id(&reader.value_string_at(0));
                carried = nickname;
            } else if reader.is_value(b"base") {
                base = self.lookup_base(&reader.value_string(), cx.reports);
            } else if reader.is_value(b"ids_name") {
                ids_name = reader.value_int(0);
            } else if reader.is_value(b"archetype") {
                archetype = create_id(&reader.value_string_at(0));
            } else if reader.is_value(b"pos") {
                pos = vector3(reader);
            } else if reader.is_value(b"visit") {
                visit = reader.value_int(0).to_le_bytes()[0];
            } else if reader.is_value(b"goto") {
                goto = create_id(&reader.value_string_at(0));
                self.gate_systems.entry(carried).or_insert(goto);
                saw_goto = true;
            }
            if !reader.read_value() {
                break;
            }
        }
        carry.object_nickname = carried;
        if saw_goto {
            let gate_type = match cx.archetypes.solars.get(&archetype) {
                Some(solar) => GateType::from_object_type(solar.eq_obj.root.object_type),
                None => {
                    cx.reports.push(
                        "Common\\Archetype.cpp",
                        0x49,
                        format!(
                            "*** ERROR: ArchDB::Get(0x{archetype:x}) failed. ID={}",
                            i32::from_ne_bytes(archetype.to_ne_bytes())
                        ),
                    );
                    GateType::None
                }
            };
            system.connections.push(Connection {
                goto_system: goto,
                object: nickname,
                object_id: carried,
                pos,
                visit,
                gate_type,
            });
        }
        if base == 0 || base & VALID_ID == 0 {
            return;
        }
        match self.bases.get_mut(&base) {
            Some(found) => {
                if found.strid_name == ids_name {
                    found.space_object = carried;
                    found.space_object_visit = visit;
                }
            }
            None => cx.reports.push(
                SOURCE,
                0xbf3,
                format!("*** WARNING: get_base({base:#X}) failed."),
            ),
        }
    }

    /// `LookupObjectBaseId` / `get_base_id`: the base's id, or 0 (reported).
    pub fn lookup_base(&self, nickname: &[u8], reports: &mut Reports) -> u32 {
        let id = create_id(nickname);
        if self.bases.contains_key(&id) {
            id
        } else {
            reports.push(
                SOURCE,
                0xbff,
                format!("*** WARNING: get_base({}) failed.", lossy(nickname)),
            );
            0
        }
    }

    /// `get_system_id`: the system's id, or 0 (reported).
    pub fn get_system_id(&self, nickname: &[u8], reports: &mut Reports) -> u32 {
        let id = create_id(nickname);
        if self.systems.contains_key(&id) {
            id
        } else {
            reports.push(
                SOURCE,
                0xbe4,
                format!("*** WARNING: get_system({}) failed.", lossy(nickname)),
            );
            0
        }
    }

    /// `get_gate_system`: the system a gate object leads to, or 0.
    #[must_use]
    pub fn gate_system(&self, object: u32) -> u32 {
        self.gate_systems.get(&object).copied().unwrap_or(0)
    }

    /// `first_zone` and `next_zone`: the zones of a system in file order.
    pub fn system_zones(&self, system: u32) -> impl Iterator<Item = &Zone> {
        self.systems
            .get(&system)
            .into_iter()
            .flat_map(|s| s.zones.iter())
            .filter_map(|id| self.zones.get(id))
    }

    /// `GetObjectCount` / `GetSystemObject`: every `[Object]` of the
    /// system's file, read from the file again.
    pub fn system_objects(
        &self,
        source: &dyn DataSource,
        system: &System,
        strings: &StringCache,
        reports: &mut Reports,
    ) -> Vec<SystemObject> {
        let path = self.system_file_path(strings.get(system.file));
        let Some(mut reader) = open_ini(source, &path) else {
            return Vec::new();
        };
        let mut objects = Vec::new();
        while reader.read_header() {
            if reader.is_header(b"Object") {
                objects.push(self.read_system_object(&mut reader, reports));
            }
        }
        objects
    }

    /// `ReadSystemObject`.
    fn read_system_object(&self, reader: &mut IniReader, reports: &mut Reports) -> SystemObject {
        let mut object = SystemObject::default();
        let mut star = false;
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                object.nickname = tstring(&reader.value_string(), 64);
            } else if reader.is_value(b"next_ring") {
                object.next_ring = tstring(&reader.value_string(), 64);
            } else if reader.is_value(b"prev_ring") {
                object.prev_ring = tstring(&reader.value_string(), 64);
            } else if reader.is_value(b"pos") {
                let y = value_f32(reader, 1);
                let x = value_f32(reader, 0);
                let z = value_f32(reader, 2);
                object.pos = Vector { x, y, z };
            } else if reader.is_value(b"archetype") {
                object.archetype = create_id(&reader.value_string());
            } else if reader.is_value(b"ids_name") {
                object.ids_name = reader.value_int(0);
            } else if reader.is_value(b"ids_info")
                || reader.is_value(b"ids_info_card")
                || reader.is_value(b"infocard_ids")
            {
                object.ids_info = reader.value_int(0);
            } else if reader.is_value(b"goto") {
                object.goto = create_id(&reader.value_string_at(0));
            } else if reader.is_value(b"dock_with") {
                object.dock_with = create_id(&reader.value_string_at(0));
            } else if reader.is_value(b"reputation") {
                object.reputation = tstring(&reader.value_string_at(0), 32);
            } else if reader.is_value(b"visit") {
                object.visit = reader.value_int(0).to_le_bytes()[0];
            } else if reader.is_value(b"base") {
                object.base = self.lookup_base(&reader.value_string(), reports);
            } else if reader.is_value(b"star") {
                star = true;
            }
        }
        if star {
            object.visit = 1;
        }
        object
    }
}

/// `ReadBaseValues`.
fn read_base(reader: &mut IniReader, strings: &mut StringCache) -> Base {
    let mut base = Base::default();
    while reader.read_value() {
        let id = |r: &IniReader| create_id(&r.value_string());
        if reader.is_value(b"nickname") {
            let text = reader.value_string();
            base.nickname_id = create_id(&text);
            base.nickname = strings.alloc(&text, false);
        } else if reader.is_value(b"strid_name") {
            base.strid_name = reader.value_int(0);
        } else if reader.is_value(b"file") {
            base.file = strings.alloc(&reader.value_string(), false);
        } else if reader.is_value(b"system") {
            base.system = id(reader);
        } else if reader.is_value(b"BGCS_base_run_by") {
            base.bgcs_base_run_by = neuron_core::hash::c_str(&reader.value_string()).to_vec();
        } else if reader.is_value(b"autosave_forbidden") {
            base.autosave_forbidden = reader.value_bool(0);
        } else {
            const SHIPS: [&[u8]; 9] = [
                b"ship_sml_01",
                b"ship_sml_02",
                b"ship_sml_03",
                b"ship_mdm_01",
                b"ship_mdm_02",
                b"ship_mdm_03",
                b"ship_lrg_01",
                b"ship_lrg_02",
                b"ship_lrg_03",
            ];
            if let Some(index) = SHIPS.iter().position(|n| reader.is_value(n)) {
                let value = id(reader);
                match index / 3 {
                    0 => base.ship_sml[index % 3] = value,
                    1 => base.ship_mdm[index % 3] = value,
                    _ => base.ship_lrg[index % 3] = value,
                }
                continue;
            }
            let terrain: [(&[u8], &mut u32); 6] = [
                (b"terrain_tiny", &mut base.terrain_tiny),
                (b"terrain_sml", &mut base.terrain_sml),
                (b"terrain_mdm", &mut base.terrain_mdm),
                (b"terrain_lrg", &mut base.terrain_lrg),
                (b"terrain_dyna_01", &mut base.terrain_dyna_01),
                (b"terrain_dyna_02", &mut base.terrain_dyna_02),
            ];
            if let Some((_, slot)) = terrain.into_iter().find(|(n, _)| reader.is_value(n)) {
                *slot = id(reader);
            }
        }
    }
    base
}
