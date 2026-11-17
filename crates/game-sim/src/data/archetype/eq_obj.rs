//! Equipped objects: `EqObj`, `Solar`, `Ship` and their collision groups
//! (`common_archetype_ships.cpp`).

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::SOURCE;
use super::root::{Root, lookup_hp_type};
use crate::data::goods::tstring;
use crate::data::string_cache::{CacheString, StringCache};
use crate::data::{Reports, value_f32};

/// `docking_sphere` types (the berth table; no match is 0).
const DOCK_TYPES: [(&[u8], u32); 8] = [
    (b"berth", 1),
    (b"moor_small", 3),
    (b"moor_medium", 4),
    (b"moor_large", 5),
    (b"ring", 6),
    (b"pad", 6),
    (b"jump", 7),
    (b"airlock", 8),
];

/// Collision group `type` names (no match is 1).
const GROUP_TYPES: [(&[u8], u32); 19] = [
    (b"Fin", 2),
    (b"Top_Fin", 4),
    (b"Port_Fin", 8),
    (b"Starboard_Fin", 0x10),
    (b"Port_Wing", 0x20),
    (b"Starboard_Wing", 0x40),
    (b"Middle_Wing", 0x80),
    (b"Engine", 0x100),
    (b"Port_Engine", 0x200),
    (b"Middle_Engine", 0x400),
    (b"Starboard_Engine", 0x800),
    (b"Top_Engine", 0x1000),
    (b"Bottom_Engine", 0x2000),
    (b"Spoiler", 0x4000),
    (b"Tail", 0x8000),
    (b"Port_Side_Panel", 0x1_0000),
    (b"Starboard_Side_Panel", 0x2_0000),
    (b"Port_Arm", 0x4_0000),
    (b"Starboard_Arm", 0x8_0000),
];

/// `linked_equip` names, `EEquipType` bits (no match is 0).
const LINKED_EQUIP_TYPES: [(&[u8], u32); 14] = [
    (b"scanner", 0x8_0000),
    (b"engine", 0x2_0000),
    (b"weapon", 0x80),
    (b"shield", 0x100),
    (b"thruster", 0x400),
    (b"power", 0x4_0000),
    (b"light", 1),
    (b"tractor", 0x10_0000),
    (b"attached_fx", 2),
    (b"internal_fx", 0x40_0000),
    (b"tradelane", 0x80_0000),
    (b"repairdroid", 0x20_0000),
    (b"cloaking_device", 0x1000),
    (b"armor", 0x100_0000),
];

fn lookup(name: &[u8], table: &[(&[u8], u32)], default: u32) -> u32 {
    table
        .iter()
        .find(|(n, _)| eq_ignore_case(name, n))
        .map_or(default, |&(_, v)| v)
}

/// `FGroupFuse`: a fuse with its two values.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GroupFuse {
    /// The fuse's id.
    pub fuse: u32,
    /// Value 1.
    pub value1: f32,
    /// Value 2.
    pub value2: f32,
}

fn read_fuse(reader: &mut IniReader) -> GroupFuse {
    GroupFuse {
        fuse: create_id(&reader.value_string_at(0)),
        value1: value_f32(reader, 1),
        value2: value_f32(reader, 2),
    }
}

/// `DamageObjInfo`: a hardpoint and the archetype mounted on it when the
/// group is damaged.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DamageObj {
    /// The hardpoint.
    pub hardpoint: CacheString,
    /// The archetype.
    pub arch: u32,
}

impl DamageObj {
    /// The entry lacks its hardpoint or its archetype (`IsValid`'s test).
    #[must_use]
    pub fn is_unmatched(&self) -> bool {
        self.hardpoint.is_empty() || self.arch & 0x3FFF_FFFF == 0
    }
}

fn fill_or_push_hp(list: &mut Vec<DamageObj>, hardpoint: CacheString) {
    match list.last_mut() {
        Some(last) if last.hardpoint.is_empty() => last.hardpoint = hardpoint,
        _ => list.push(DamageObj { hardpoint, arch: 0 }),
    }
}

fn fill_or_push_obj(list: &mut Vec<DamageObj>, arch: u32) {
    match list.last_mut() {
        Some(last) if last.arch == 0 => last.arch = arch,
        _ => list.push(DamageObj {
            hardpoint: CacheString::EMPTY,
            arch,
        }),
    }
}

/// `CollisionGroup`.
#[derive(Clone, Debug, PartialEq)]
pub struct CollisionGroup {
    /// The sub-object id (`CollGroupIdMaker`).
    pub id: u16,
    /// `obj`: the part name.
    pub obj: CacheString,
    /// `type` (1).
    pub group_type: u32,
    /// `hit_pts`.
    pub hit_pts: i32,
    /// `separable`.
    pub separable: bool,
    /// `destroy_parent`.
    pub destroy_parent: bool,
    /// `root_health_proxy`.
    pub root_health_proxy: bool,
    /// `parent_impulse`.
    pub parent_impulse: f32,
    /// `child_impulse`.
    pub child_impulse: f32,
    /// `mass` (1).
    pub mass: f32,
    /// Whether `rotation_inertia` was given.
    pub has_rotation_inertia: bool,
    /// `rotation_inertia`.
    pub rotation_inertia: [f32; 3],
    /// `separation_explosion`.
    pub separation_explosion: u32,
    /// `debris_type`.
    pub debris_type: u32,
    /// `explosion_resistance` (1).
    pub explosion_resistance: f32,
    /// `dmg_hp` / `dmg_obj` pairs.
    pub dmg_objs: Vec<DamageObj>,
    /// `group_dmg_hp` / `group_dmg_obj` pairs.
    pub group_dmg_objs: Vec<DamageObj>,
    /// `fuse` lines.
    pub fuses: Vec<GroupFuse>,
    /// `linked_equip`: type.
    pub linked_equip_type: u32,
    /// `linked_equip`: health.
    pub linked_equip_health: f32,
}

impl CollisionGroup {
    pub fn new(id: u16) -> Self {
        Self {
            id,
            obj: CacheString::EMPTY,
            group_type: 1,
            hit_pts: 0,
            separable: false,
            destroy_parent: false,
            root_health_proxy: false,
            parent_impulse: 0.0,
            child_impulse: 0.0,
            mass: 1.0,
            has_rotation_inertia: false,
            rotation_inertia: [0.0; 3],
            separation_explosion: 0,
            debris_type: 0,
            explosion_resistance: 1.0,
            dmg_objs: Vec::new(),
            group_dmg_objs: Vec::new(),
            fuses: Vec::new(),
            linked_equip_type: 0,
            linked_equip_health: 0.0,
        }
    }

    /// `ReadCollisionGroup`: the values of a `[CollisionGroup]` section. A
    /// damage list with an unmatched entry is reported and emptied.
    pub fn read(
        reader: &mut IniReader,
        id: u16,
        strings: &mut StringCache,
        reports: &mut Reports,
    ) -> Self {
        let mut group = Self::new(id);
        while reader.read_value() {
            if reader.is_value(b"obj") {
                group.obj = strings.alloc(&reader.value_string(), false);
            } else if reader.is_value(b"type") {
                group.group_type = lookup(&reader.value_string(), &GROUP_TYPES, 1);
            } else if reader.is_value(b"hit_pts") {
                group.hit_pts = reader.value_int(0);
            } else if reader.is_value(b"separable") {
                group.separable = true;
            } else if reader.is_value(b"destroy_parent") {
                group.destroy_parent = reader.is_value_empty(0) || reader.value_bool(0);
            } else if reader.is_value(b"root_health_proxy") {
                group.root_health_proxy = reader.value_bool(0);
            } else if reader.is_value(b"parent_impulse") {
                group.parent_impulse = value_f32(reader, 0);
            } else if reader.is_value(b"child_impulse") {
                group.child_impulse = value_f32(reader, 0);
            } else if reader.is_value(b"mass") {
                group.mass = value_f32(reader, 0);
            } else if reader.is_value(b"rotation_inertia") {
                group.rotation_inertia = [
                    value_f32(reader, 0),
                    value_f32(reader, 1),
                    value_f32(reader, 2),
                ];
                group.has_rotation_inertia = true;
            } else if reader.is_value(b"separation_explosion") {
                group.separation_explosion = create_id(&reader.value_string());
            } else if reader.is_value(b"debris_type") {
                group.debris_type = create_id(&reader.value_string());
            } else if reader.is_value(b"dmg_hp") {
                let hp = strings.alloc(&reader.value_string(), false);
                fill_or_push_hp(&mut group.dmg_objs, hp);
            } else if reader.is_value(b"dmg_obj") {
                fill_or_push_obj(&mut group.dmg_objs, create_id(&reader.value_string()));
            } else if reader.is_value(b"group_dmg_hp") {
                let hp = strings.alloc(&reader.value_string(), false);
                fill_or_push_hp(&mut group.group_dmg_objs, hp);
            } else if reader.is_value(b"group_dmg_obj") {
                fill_or_push_obj(&mut group.group_dmg_objs, create_id(&reader.value_string()));
            } else if reader.is_value(b"explosion_resistance") {
                group.explosion_resistance = value_f32(reader, 0);
            } else if reader.is_value(b"fuse") {
                group.fuses.push(read_fuse(reader));
            } else if reader.is_value(b"linked_equip") {
                group.linked_equip_type =
                    lookup(&reader.value_string_at(0), &LINKED_EQUIP_TYPES, 0);
                group.linked_equip_health = value_f32(reader, 1);
            }
        }
        for (list, kind, line) in [
            (&mut group.dmg_objs, "dmg_obj or dmg_hp", 0x11de),
            (
                &mut group.group_dmg_objs,
                "group_dmg_obj or group_dmg_hp",
                0x11e3,
            ),
        ] {
            if list.iter().any(DamageObj::is_unmatched) {
                reports.push(
                    SOURCE,
                    line,
                    format!(
                        "*** WARNING: Unmatched {kind} values for Group[{}].  No damage objects will be attached.",
                        strings.lossy(group.obj)
                    ),
                );
                list.clear();
            }
        }
        group
    }
}

/// `ShieldLink`: a shield with its hardpoint and generator hardpoints.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ShieldLink {
    /// The shield archetype.
    pub shield_arch: u32,
    /// The shield's sub-object id (0xFFF1 upwards).
    pub equip_id: u16,
    /// The shield's hardpoint.
    pub hardpoint: CacheString,
    /// The generator hardpoints.
    pub generator_hps: Vec<CacheString>,
}

/// `EqObj::DockingSphere`. The names are copies (`std::string`), taken
/// when the line was read.
#[derive(Clone, Debug, PartialEq)]
pub struct DockingSphere {
    /// Value 2.
    pub radius: f32,
    /// Value 1, lower-cased.
    pub hardpoint: Vec<u8>,
    /// Value 3, the dock animation script.
    pub anim: Vec<u8>,
    /// Value 0 through the berth table.
    pub dock_type: u32,
}

/// `EqObj`: what solars and ships share.
#[derive(Clone, Debug, PartialEq)]
pub struct EqObj {
    /// The common fields.
    pub root: Root,
    /// `[CollisionGroup]` sections after the record, in file order.
    pub collision_groups: Vec<CollisionGroup>,
    /// The `ShieldIdMaker` counter (0xFFF0).
    pub shield_id_next: u16,
    /// `shield_link` lines.
    pub shield_links: Vec<ShieldLink>,
    /// `fuse` lines.
    pub fuses: Vec<GroupFuse>,
    /// `veilborn`.
    pub veilborn: bool,
    /// `docking_camera` (true).
    pub docking_camera: bool,
    /// `docking_sphere` lines.
    pub docking_spheres: Vec<DockingSphere>,
}

impl EqObj {
    fn new(root: Root) -> Self {
        Self {
            root,
            collision_groups: Vec::new(),
            shield_id_next: 0xFFF0,
            shield_links: Vec::new(),
            fuses: Vec::new(),
            veilborn: false,
            docking_camera: true,
            docking_spheres: Vec::new(),
        }
    }

    /// `EqObj::read`.
    pub fn read(
        &mut self,
        reader: &mut IniReader,
        strings: &mut StringCache,
        reports: &mut Reports,
    ) -> bool {
        if self.root.read(reader, strings, reports) {
            return true;
        }
        if reader.is_value(b"docking_camera") {
            self.docking_camera = reader.value_bool(0);
        } else if reader.is_value(b"docking_sphere") {
            let dock_type = lookup(&reader.value_string_at(0), &DOCK_TYPES, 0);
            let pooled = strings.alloc(&reader.value_string_at(1), false);
            strings.lowercase_in_place(pooled);
            let hardpoint = strings.get(pooled).to_vec();
            let radius = value_f32(reader, 2);
            let anim = if reader.is_value_empty(3) {
                Vec::new()
            } else {
                let pooled = strings.alloc(&reader.value_string_at(3), false);
                strings.get(pooled).to_vec()
            };
            self.docking_spheres.push(DockingSphere {
                radius,
                hardpoint,
                anim,
                dock_type,
            });
        } else if reader.is_value(b"fuse") {
            self.fuses.push(read_fuse(reader));
        } else if reader.is_value(b"shield_link") {
            let shield_arch = create_id(&reader.value_string_at(0));
            self.shield_id_next = self.shield_id_next.wrapping_add(1);
            let hardpoint = strings.alloc(&reader.value_string_at(1), false);
            let mut generator_hps = Vec::new();
            let mut index = 2;
            while !reader.is_value_empty(index) {
                generator_hps.push(strings.alloc(&reader.value_string_at(index), false));
                index += 1;
            }
            self.shield_links.push(ShieldLink {
                shield_arch,
                equip_id: self.shield_id_next,
                hardpoint,
                generator_hps,
            });
        } else if reader.is_value(b"veilborn") {
            self.veilborn = reader.value_bool(0);
        } else {
            return false;
        }
        true
    }

    /// `EqObj::operator=` as `inherit` uses it: Root (without the
    /// nickname), docking spheres and collision groups; shield links,
    /// fuses and the flags stay.
    pub fn inherit(&mut self, donor: &Self) {
        self.root.inherit(&donor.root);
        self.docking_spheres.clone_from(&donor.docking_spheres);
        self.collision_groups.clone_from(&donor.collision_groups);
    }

    /// `get_group_by_id`.
    #[must_use]
    pub fn group_by_id(&self, id: u16) -> Option<&CollisionGroup> {
        self.collision_groups.iter().find(|g| g.id == id)
    }

    /// `get_group_by_name`: the pooled copies are compared, not the text.
    #[must_use]
    pub fn group_by_name(&self, name: CacheString) -> Option<&CollisionGroup> {
        self.collision_groups.iter().find(|g| g.obj == name)
    }
}

/// `Archetype::Solar`.
#[derive(Clone, Debug, PartialEq)]
pub struct Solar {
    /// The equipped-object fields.
    pub eq_obj: EqObj,
    /// `destructible`.
    pub destructible: bool,
    /// `solar_radius` (500).
    pub solar_radius: f32,
    /// `toughness`.
    pub toughness: i32,
    /// `open_anim`.
    pub open_anim: CacheString,
    /// `jump_out_hp`.
    pub jump_out_hp: CacheString,
    /// `loadout`.
    pub loadout: u32,
}

impl Default for Solar {
    fn default() -> Self {
        Self {
            eq_obj: EqObj::new(Root::default()),
            destructible: false,
            solar_radius: 500.0,
            toughness: 0,
            open_anim: CacheString::EMPTY,
            jump_out_hp: CacheString::EMPTY,
            loadout: 0,
        }
    }
}

impl Solar {
    /// `Solar::read`.
    pub fn read(
        &mut self,
        reader: &mut IniReader,
        strings: &mut StringCache,
        reports: &mut Reports,
    ) -> bool {
        if self.eq_obj.read(reader, strings, reports) {
            return true;
        }
        if reader.is_value(b"toughness") {
            self.toughness = reader.value_int(0);
        } else if reader.is_value(b"destructible") {
            self.destructible = reader.value_bool(0);
        } else if reader.is_value(b"open_anim") {
            self.open_anim = strings.alloc(&reader.value_string(), false);
        } else if reader.is_value(b"loadout") {
            self.loadout = create_id(&reader.value_string());
        } else if reader.is_value(b"jump_out_hp") {
            self.jump_out_hp = strings.alloc(&reader.value_string(), false);
        } else if reader.is_value(b"solar_radius") {
            self.solar_radius = value_f32(reader, 0);
        } else {
            return false;
        }
        true
    }

    /// The `inherit` copy of `LoadSolar`.
    pub fn inherit(&mut self, donor: &Self) {
        self.eq_obj.inherit(&donor.eq_obj);
        self.destructible = donor.destructible;
        self.solar_radius = donor.solar_radius;
        self.toughness = donor.toughness;
        self.open_anim = donor.open_anim;
        self.jump_out_hp = donor.jump_out_hp;
        self.loadout = donor.loadout;
    }

    /// `Solar::redefine`.
    pub fn redefine(&mut self, other: &Self) {
        self.eq_obj.root.redefine(&other.eq_obj.root);
        self.toughness = other.toughness;
    }
}

/// `LegalHpTypes`: the hardpoints a ship offers for one attachment type.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LegalHps {
    /// The `HpAttachmentType`.
    pub hp_type: u32,
    /// The hardpoints.
    pub hardpoints: Vec<CacheString>,
}

/// `Archetype::Ship`.
#[derive(Clone, Debug, PartialEq)]
pub struct Ship {
    /// The equipped-object fields.
    pub eq_obj: EqObj,
    /// `msg_id_prefix` (`TString<64>`).
    pub msg_id_prefix: Vec<u8>,
    /// `bay_door_anim`.
    pub bay_door_anim: CacheString,
    /// `HP_bay_surface`.
    pub hp_bay_surface: CacheString,
    /// `HP_bay_external`.
    pub hp_bay_external: CacheString,
    /// `HP_tractor_source` (`HpHeadlight`).
    pub hp_tractor_source: CacheString,
    /// `ids_info1`.
    pub ids_info1: i32,
    /// `ids_info2`.
    pub ids_info2: i32,
    /// `ids_info3`.
    pub ids_info3: i32,
    /// `ship_class`.
    pub ship_class: i32,
    /// `num_exhaust_nozzles` (9).
    pub num_exhaust_nozzles: i32,
    /// `hold_size`.
    pub hold_size: f32,
    /// `linear_drag` (100).
    pub linear_drag: f32,
    /// `angular_drag` (0.75 each).
    pub angular_drag: [f32; 3],
    /// `steering_torque` (2000 each).
    pub steering_torque: [f32; 3],
    /// `nudge_force`.
    pub nudge_force: f32,
    /// `strafe_force`.
    pub strafe_force: f32,
    /// `strafe_power_usage`.
    pub strafe_power_usage: f32,
    /// `max_bank_angle`, in radians.
    pub max_bank_angle: f32,
    /// `hp_type` lines.
    pub legal_hps: Vec<LegalHps>,
    /// `nanobot_limit` (-1).
    pub nanobot_limit: i32,
    /// `shield_battery_limit` (-1).
    pub shield_battery_limit: i32,
}

/// `0.017453292f`, degrees to radians as the C++ multiplies.
pub(crate) fn degrees() -> f64 {
    f64::from(0.017_453_292_f32)
}

impl Ship {
    /// `Ship::Ship`: interns `HpHeadlight` for the tractor source.
    pub fn new(strings: &mut StringCache) -> Self {
        Self {
            eq_obj: EqObj::new(Root::default()),
            msg_id_prefix: Vec::new(),
            bay_door_anim: CacheString::EMPTY,
            hp_bay_surface: CacheString::EMPTY,
            hp_bay_external: CacheString::EMPTY,
            hp_tractor_source: strings.alloc(b"HpHeadlight", false),
            ids_info1: 0,
            ids_info2: 0,
            ids_info3: 0,
            ship_class: 0,
            num_exhaust_nozzles: 9,
            hold_size: 0.0,
            linear_drag: 100.0,
            angular_drag: [0.75; 3],
            steering_torque: [2000.0; 3],
            nudge_force: 0.0,
            strafe_force: 0.0,
            strafe_power_usage: 0.0,
            max_bank_angle: 0.0,
            legal_hps: Vec::new(),
            nanobot_limit: -1,
            shield_battery_limit: -1,
        }
    }

    /// `Ship::get_legal_hps`.
    #[must_use]
    pub fn legal_hps(&self, hp_type: u32) -> Option<&[CacheString]> {
        self.legal_hps
            .iter()
            .find(|l| l.hp_type == hp_type)
            .map(|l| &l.hardpoints[..])
    }

    /// `Ship::read`.
    pub fn read(
        &mut self,
        reader: &mut IniReader,
        strings: &mut StringCache,
        reports: &mut Reports,
    ) -> bool {
        if self.eq_obj.read(reader, strings, reports) {
            return true;
        }
        let vector = |reader: &mut IniReader| {
            [
                value_f32(reader, 0),
                value_f32(reader, 1),
                value_f32(reader, 2),
            ]
        };
        if reader.is_value(b"bay_door_anim") {
            self.bay_door_anim = strings.alloc(&reader.value_string(), false);
        } else if reader.is_value(b"HP_bay_surface") {
            self.hp_bay_surface = strings.alloc(&reader.value_string(), false);
        } else if reader.is_value(b"HP_bay_external") {
            self.hp_bay_external = strings.alloc(&reader.value_string(), false);
        } else if reader.is_value(b"HP_tractor_source") {
            self.hp_tractor_source = strings.alloc(&reader.value_string(), false);
        } else if reader.is_value(b"num_exhaust_nozzles") {
            self.num_exhaust_nozzles = reader.value_int(0);
        } else if reader.is_value(b"hold_size") {
            self.hold_size = value_f32(reader, 0);
        } else if reader.is_value(b"linear_drag") {
            self.linear_drag = value_f32(reader, 0);
        } else if reader.is_value(b"angular_drag") {
            self.angular_drag = vector(reader);
        } else if reader.is_value(b"steering_torque") {
            self.steering_torque = vector(reader);
        } else if reader.is_value(b"nudge_force") {
            self.nudge_force = value_f32(reader, 0);
        } else if reader.is_value(b"strafe_force") {
            self.strafe_force = value_f32(reader, 0);
        } else if reader.is_value(b"strafe_power_usage") {
            self.strafe_power_usage = value_f32(reader, 0);
        } else if reader.is_value(b"ids_info1") {
            self.ids_info1 = reader.value_int(0);
        } else if reader.is_value(b"ids_info2") {
            self.ids_info2 = reader.value_int(0);
        } else if reader.is_value(b"ids_info3") {
            self.ids_info3 = reader.value_int(0);
        } else if reader.is_value(b"ship_class") {
            self.ship_class = reader.value_int(0);
        } else if reader.is_value(b"nanobot_limit") {
            self.nanobot_limit = reader.value_int(0);
        } else if reader.is_value(b"shield_battery_limit") {
            self.shield_battery_limit = reader.value_int(0);
        } else if reader.is_value(b"max_bank_angle") {
            let mut deg = reader.value_float(0);
            if deg > 180.0 {
                deg = 180.0;
            }
            self.max_bank_angle = (deg * degrees()) as f32;
        } else if reader.is_value(b"msg_id_prefix") {
            self.msg_id_prefix = tstring(&reader.value_string(), 64);
        } else if reader.is_value(b"hp_type") {
            let Some(hp_type) = lookup_hp_type(&reader.value_string_at(0)).filter(|&v| v != 0)
            else {
                return true;
            };
            let mut hardpoints = Vec::new();
            let mut index = 1;
            while !reader.is_value_empty(index) {
                hardpoints.push(strings.alloc(&reader.value_string_at(index), false));
                index += 1;
            }
            self.legal_hps.push(LegalHps {
                hp_type,
                hardpoints,
            });
        } else {
            return false;
        }
        true
    }

    /// The `inherit` copy of `LoadShips`.
    pub fn inherit(&mut self, donor: &Self) {
        self.eq_obj.inherit(&donor.eq_obj);
        let eq_obj = core::mem::replace(&mut self.eq_obj, EqObj::new(Root::default()));
        *self = donor.clone();
        self.eq_obj = eq_obj;
    }
}
