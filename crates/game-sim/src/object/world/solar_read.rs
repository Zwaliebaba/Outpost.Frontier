//! `CSolar::ReadObj` (`Common/common_csolar.cpp:694`): a system file's
//! `[Object]` section as the multiplayer server's `StarSystem::load_object`
//! reads it (decision record 0007, point 11), and the system's solars in
//! file order.

use neuron_core::ini::IniReader;
use neuron_core::math::Vector;

use crate::data::costume::nickname_id;
use crate::data::{Reports, lossy, open_ini, value_f32};

use super::{SolarSpawn, World};

const SOURCE: &str = "Common\\CSolar.cpp";

/// `MAX_ACCESSORIES`: the accessory slots of a `Costume`.
const MAX_ACCESSORIES: usize = 8;

impl World {
    /// `CSolar::ReadObj`: one `[Object]` section after its header. `None`
    /// when the C++ answers false: an unknown archetype (the rest of the
    /// section is not read), no archetype, no nickname, or a nickname the
    /// reputation database lacks. `file` names the system file in the
    /// reports.
    pub fn read_solar_obj(
        &mut self,
        reader: &mut IniReader,
        file: &[u8],
        reports: &mut Reports,
    ) -> Option<SolarSpawn> {
        let mut spawn = SolarSpawn::default();
        let mut arch: Option<crate::data::archetype::eq_obj::Solar> = None;
        let mut nickname: Vec<u8> = Vec::new();
        let mut arch_name: Vec<u8> = Vec::new();
        let mut loadout = 0u32;
        let mut loadout_name: Vec<u8> = Vec::new();
        let mut destructible_set = false;
        let mut atmosphere_set = false;
        while reader.read_value() {
            if reader.is_value(b"archetype") {
                let name = reader.value_string();
                // `GetSolar(GetAsteroidID(CreateID(name)))`: the asteroid
                // id is the name's id.
                let id = neuron_core::id::create_id(&name);
                match self.data.archetypes.solars.get(&id) {
                    Some(a) => {
                        arch_name = name;
                        spawn.archetype = id;
                        arch = Some(a.clone());
                    }
                    None => {
                        reports.push(
                            SOURCE,
                            0x1da,
                            format!(
                                "*** WARNING: Archetype \"{}\" not found while reading Solar object \"{}\"",
                                lossy(&name),
                                lossy(&nickname)
                            ),
                        );
                        return None;
                    }
                }
            } else if reader.is_value(b"nickname") {
                nickname = reader.value_string();
                spawn.nickname = neuron_core::id::create_id(&nickname);
                spawn.name = nickname.clone();
            } else if reader.is_value(b"pos") {
                spawn.position = Vector::new(
                    value_f32(reader, 0),
                    value_f32(reader, 1),
                    value_f32(reader, 2),
                );
            } else if reader.is_value(b"rotate") {
                spawn.orientation = crate::data::zone::euler_matrix(Vector::new(
                    value_f32(reader, 0),
                    value_f32(reader, 1),
                    value_f32(reader, 2),
                ));
            } else if reader.is_value(b"spin") {
                spawn.angular_velocity = Vector::new(
                    value_f32(reader, 0),
                    value_f32(reader, 1),
                    value_f32(reader, 2),
                );
            } else if reader.is_value(b"visit") {
                spawn.visit = reader.value_int(0) as u8;
            } else if reader.is_value(b"parent") {
                spawn.parent = neuron_core::id::create_id(&reader.value_string());
            } else if reader.is_value(b"goto") {
                let system_name = reader.value_string_at(0);
                let system = self.data.universe.get_system_id(&system_name, reports);
                spawn.dest_system = system;
                if system & 0x3fff_ffff == 0 {
                    reports.push(
                        SOURCE,
                        0x218,
                        format!(
                            "*** ERROR: Jumpgate has invalid destination: {}",
                            lossy(&system_name)
                        ),
                    );
                }
                spawn.dest_gate = neuron_core::id::create_id(&reader.value_string_at(1));
            } else if reader.is_value(b"base") {
                let name = reader.value_string();
                spawn.base = self.data.universe.lookup_base(&name, reports);
                if spawn.base == 0 {
                    reports.push(
                        SOURCE,
                        0x224,
                        format!(
                            "*** ERROR: Could not find base {} for solar object.",
                            lossy(&name)
                        ),
                    );
                }
            } else if reader.is_value(b"dock_with") {
                let name = reader.value_string();
                spawn.dock_with = self.data.universe.lookup_base(&name, reports);
                if spawn.dock_with == 0 {
                    reports.push(
                        SOURCE,
                        0x22c,
                        format!(
                            "*** ERROR: Could not find docking target {} for solar object.",
                            lossy(&name)
                        ),
                    );
                }
            } else if reader.is_value(b"loadout") {
                loadout_name = reader.value_string();
                loadout = neuron_core::id::create_id(&loadout_name);
            } else if reader.is_value(b"atmosphere_range") {
                spawn.atmosphere_range = value_f32(reader, 0);
                atmosphere_set = true;
            } else if reader.is_value(b"anim") {
                spawn.continual_anims.push(reader.value_string());
            } else if reader.is_value(b"prev_ring") {
                spawn.prev_ring = neuron_core::id::create_id(&reader.value_string());
            } else if reader.is_value(b"next_ring") {
                spawn.next_ring = neuron_core::id::create_id(&reader.value_string());
            } else if reader.is_value(b"tradelane_space_name") {
                spawn.tradelane_space_name = reader.value_int(0) as u32;
            } else if reader.is_value(b"destructible") {
                destructible_set = true;
                spawn.destructible = reader.value_bool(0);
            } else if reader.is_value(b"space_costume") {
                self.read_space_costume(reader, &mut spawn, reports);
            } else if reader.is_value(b"voice") {
                spawn.voice = neuron_core::id::create_id(&reader.value_string());
            } else if reader.is_value(b"comm_appr") {
                let id = nickname_id(&reader.value_string());
                let costume = self.data.costumes.get_costume(id, reports);
                spawn.costume = costume;
            }
        }
        let file_text = lossy(file);
        if atmosphere_set
            && let Some(a) = &arch
            && spawn.atmosphere_range < a.solar_radius
        {
            reports.push(
                SOURCE,
                0x289,
                format!(
                    "*** ERROR: Solar with Archetype({}) is larger than atmosphere {}f in INI File({file_text})!",
                    lossy(&arch_name),
                    neuron_core::num::trunc_to_i32(f64::from(spawn.atmosphere_range))
                ),
            );
        }
        let Some(arch) = arch else {
            reports.push(
                SOURCE,
                0x291,
                format!(
                    "*** ERROR: Solar with Nickname({}) missing archetype in INI File({file_text})!",
                    lossy(&nickname)
                ),
            );
            return None;
        };
        if spawn.nickname == 0 {
            reports.push(
                SOURCE,
                0x296,
                format!(
                    "*** ERROR: Solar with Archetype({}) missing nickname in INI File({file_text})!",
                    lossy(&arch_name)
                ),
            );
            return None;
        }
        let key = spawn.nickname as i32;
        if !self.vibes.verify(key) {
            reports.push(
                SOURCE,
                0x2a3,
                format!(
                    "*** ERROR: Failed to read reputation information for solar({})!",
                    lossy(&nickname)
                ),
            );
            return None;
        }
        spawn.vibe = key;
        if loadout == 0 {
            // The archetype's loadout; its name is not kept, so a missing
            // one is reported by id.
            loadout = arch.loadout;
            loadout_name = format!("{loadout}").into_bytes();
        }
        if loadout != 0 {
            if self.data.loadouts.get(loadout).is_some() {
                spawn.loadout = loadout;
            } else {
                reports.push(
                    SOURCE,
                    0x2c9,
                    format!(
                        "*** ERROR: {}: Unable to find [Loadout] {}.",
                        lossy(&nickname),
                        lossy(&loadout_name)
                    ),
                );
            }
        }
        if !destructible_set {
            spawn.destructible = arch.destructible;
        }
        Some(spawn)
    }

    /// `space_costume = head, body, accessory...`: a head (type 1) and a
    /// body (type 0) when their values are not empty, each reported when
    /// unknown, then the accessories; a costume with no body is reported.
    /// The C++ reads an unknown accessory through a null pointer and writes
    /// past the eighth slot; the port skips the first and stops at the
    /// eighth (D614).
    fn read_space_costume(
        &mut self,
        reader: &mut IniReader,
        spawn: &mut SolarSpawn,
        reports: &mut Reports,
    ) {
        let costumes = &self.data.costumes;
        let parts = [(0usize, 1usize, "head", 0x25e), (1, 0, "body", 0x26f)];
        for (index, kind, what, line) in parts {
            let name = reader.value_string_at(index as u32);
            if reader.is_value_empty(index as u32) || name.is_empty() {
                continue;
            }
            match costumes.find_bodypart(kind, nickname_id(&name)) {
                Some(part) => spawn.costume.part_ids[index] = part.id,
                None => reports.push(
                    SOURCE,
                    line,
                    format!(
                        "*** ERROR: space costume {what} {} does not exist",
                        lossy(&name)
                    ),
                ),
            }
        }
        let mut index = 2u32;
        while !reader.is_value_empty(index) {
            let name = reader.value_string_at(index);
            index += 1;
            let Some(accessory) = costumes.accessories.get(&nickname_id(&name)) else {
                continue;
            };
            if spawn.costume.accessory_ids.len() < MAX_ACCESSORIES {
                spawn.costume.accessory_ids.push(accessory.id);
            }
        }
        if spawn.costume.part_ids[1] == 0 {
            reports.push(
                SOURCE,
                0x27c,
                format!(
                    "*** ERROR: space costume malformed {}",
                    lossy(&reader.value_string())
                ),
            );
        }
    }

    /// The solars of a system file's `[Object]` sections in file order,
    /// read as `StarSystem::load` reads them ([`World::read_solar_obj`]),
    /// without making them; the reader's reports go to `reports`. Without
    /// assets or a known system there are none.
    pub fn system_solar_spawns_reported(
        &mut self,
        system: u32,
        reports: &mut Reports,
    ) -> Vec<SolarSpawn> {
        self.load_system(system);
        let Some(sys) = self.data.universe.systems.get(&system).cloned() else {
            return Vec::new();
        };
        let Some(assets) = self.assets.clone() else {
            return Vec::new();
        };
        let path = self
            .data
            .universe
            .system_file_path(self.strings.get(sys.file));
        let Some(mut reader) = open_ini(&assets.host, &path) else {
            return Vec::new();
        };
        let file = reader.file_name().to_vec();
        let mut out = Vec::new();
        while reader.read_header() {
            if !reader.is_header(b"Object") {
                continue;
            }
            if let Some(spawn) = self.read_solar_obj(&mut reader, &file, reports) {
                out.push(spawn);
            }
        }
        out
    }

    /// [`World::system_solar_spawns_reported`] without the reports.
    pub fn system_solar_spawns(&mut self, system: u32) -> Vec<SolarSpawn> {
        self.system_solar_spawns_reported(system, &mut Reports::default())
    }

    /// `StarSystem::load`'s `[Object]` loop: every solar of the system
    /// file made ([`World::spawn_solar`]), with the reader's reports and
    /// `CSolar::init`'s dockable check (a dockable type with neither a
    /// docking base nor a destination system). Returns the objects made.
    pub fn load_system_solars_reported(
        &mut self,
        system: u32,
        reports: &mut Reports,
    ) -> Vec<super::ObjHandle> {
        let mut out = Vec::new();
        for spawn in self.system_solar_spawns_reported(system, reports) {
            let Ok(h) = self.spawn_solar(system, &spawn) else {
                continue;
            };
            let bits = self.objects.get(h).map_or(0, |o| o.simple.type_bits);
            if bits & 0x60_0d60 != 0
                && bits & 2 == 0
                && spawn.dock_with & 0x3fff_ffff == 0
                && spawn.dest_system & 0x3fff_ffff == 0
            {
                let name = lossy(&spawn.name);
                reports.push(
                    SOURCE,
                    0x6d,
                    format!(
                        "*** ERROR: {name} is dockable, but doesn't have a valid base or system to go to "
                    ),
                );
            }
            out.push(h);
        }
        out
    }
}
