//! The server's fuses (work package P3.3, decision record 0007, point 1):
//! the fuse database and its eleven actions (`Server/server_fuse.cpp`),
//! and the fuse methods of the `EqObj` wrapper
//! (`Server/server_gameobject.cpp`): `check_fuses`, `light_fuse`,
//! `unlight_fuse`, `is_death_fuse_burning`, the light and unlight sweep of
//! `apply_root_entry` and `apply_group_entry`, and the burn in
//! `EqObj::update`.
//!
//! A fuse ([`ServerFuse`]) is Common's [`Fuse`] over the server's actions.
//! The database ([`FuseDb`], `ServerFuseDB`) holds one prototype per fuse
//! name, read from the files the game's `[Data]` list names; an object
//! lights a copy of a prototype, which then lives in its side-table entry
//! ([`crate::objects::ServerObject::fuses`]) until the object goes, as the
//! C++ list of `ServerFuse` does. Burning a fuse runs its actions' triggers
//! as the C++ `Fuse::Burn` reaches them; a trigger can light another fuse
//! of the object, which the same frame's burn then reaches (the C++ list
//! walk sees an element appended during it), or destroy the object.
//!
//! No entry or world object is borrowed across an action, a dispatch or a
//! nested damage list (record 0007 point 10): each step looks the object's
//! fuse up again by its index (fuses are only ever appended).
//!
//! Left to other parts: the players' fuse notices (`StarSystem::
//! broadcast_fuse`, replication); `Ship::light_fuse`'s word to the
//! formation and follow followers' behaviour managers and the death
//! branch's own behaviour event 7 (the behaviour manager's server link);
//! `check_fuses`'s `rebuild_server_equip` (the server's equipment
//! wrappers); the loot of a `destroy_hp_attachment` with the `loot` fate
//! (the server's `destroy_equip` loot branch).

use std::collections::BTreeMap;

use game_sim::data::fuse::{BurnWalk, Fuse, FuseAction, FuseActionBase, FuseFiles, UnburnWalk};
use game_sim::data::{DataSource, open_ini};
use game_sim::equip::{Fate, SUBOBJ_ID_ROOT, SubObjId, types};
use game_sim::object::damage::{self, DamageList, DamageTarget};
use game_sim::object::world::DamageState;
use game_sim::object::{Command, ObjHandle, ObjectKind, World};
use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::{IniReader, IniReport};
use neuron_core::math::Vector;
use neuron_server::controller::ControllerId;

use crate::events::{ControllerMessage, DeathFuseLit};
use crate::objects::Invincibility;
use crate::server::Cx;

const EQOBJ: &str = "Server\\Space\\EqObj.cpp";

/// The cause of every damage list a fuse action builds (`InitFuseDamageList`).
pub const FUSE_DAMAGE_CAUSE: damage::DamageCause = 0x15;

/// `DEATH_COMM_FUSE`: the fuse an NPC destroyed by damage lights when
/// Content gives it a death conversation (`EqObj::destroy`).
#[must_use]
pub fn death_comm_fuse() -> u32 {
    create_id(b"death_comm")
}

/// What a server fuse action does when a burn reaches it (the eleven
/// `ServerFuseAction` classes).
#[derive(Clone, Debug, PartialEq)]
pub enum FuseActionKind {
    /// `[ignite_fuse]`: lights the fuse `fuse` at `fuse_t` on the burn's
    /// sub-object.
    IgniteFuse {
        /// `fuse`.
        fuse: u32,
        /// `fuse_t`.
        fuse_t: f32,
    },
    /// `[destroy_root]`: destroys the object (`destroy(1, source)`).
    DestroyRoot,
    /// `[damage_root]`: `inflict_hull_damage`.
    DamageRoot {
        /// `hitpoints`.
        hit_points: i32,
        /// Not `damage_type = absolute` (compared without letter case).
        can_kill: bool,
    },
    /// `[destroy_group]`: `destroy_group_by_name`; `random` picks a living
    /// group.
    DestroyGroup {
        /// `group_name`.
        group: Vec<u8>,
        /// `fate`: 0, `debris` 2, `disappear` 3.
        fate: i32,
    },
    /// `[damage_group]`: `inflict_group_damage`.
    DamageGroup {
        /// `group_name`.
        group: Vec<u8>,
        /// `hitpoints`.
        hit_points: i32,
        /// Not `damage_type = absolute` (compared byte for byte).
        can_kill: bool,
    },
    /// `[destroy_hp_attachment]`: `destroy_equip_by_hardpoint`; `random`
    /// picks an attached item.
    DestroyHardpointAttachment {
        /// `hardpoint`.
        hardpoint: Vec<u8>,
        /// `fate`: 0, `debris` 2, `disappear` 3, `loot` 5.
        fate: i32,
    },
    /// `[damage_hp_attachment]`: `inflict_equip_damage`.
    DamageHardpointAttachment {
        /// `hardpoint`.
        hardpoint: Vec<u8>,
        /// `hitpoints`.
        hit_points: i32,
        /// Not `damage_type = absolute` (compared byte for byte).
        can_kill: bool,
    },
    /// `[impulse]`: an explosion at the hardpoint (`FuseImpulse`).
    Impulse {
        /// `hardpoint`.
        hardpoint: Vec<u8>,
        /// `force`: the explosion's impulse.
        force: f32,
        /// `radius`.
        radius: f32,
        /// `damage`: its hull damage.
        damage: f32,
        /// `pos_offset`, in the hardpoint's space.
        pos_offset: Vector,
    },
    /// `[tumble]`: an NPC ship without a behaviour manager spins.
    Tumble {
        /// `ang_drag_scale` (1).
        ang_drag_scale: f32,
        /// The `turn_throttle_*` lows.
        turn_throttle_low: Vector,
        /// The `turn_throttle_*` highs (1, 1, 1).
        turn_throttle_high: Vector,
        /// `throttle`'s low.
        throttle_low: f32,
        /// `throttle`'s high.
        throttle_high: f32,
        /// `engine_kill`.
        engine_kill: bool,
    },
    /// `[make_invincible]`: invincibility command type 4.
    MakeInvincible {
        /// `turn_on` (true).
        turn_on: bool,
    },
    /// `[dump_cargo]`: the cargo goes out as loot.
    DumpCargo {
        /// `origin_hardpoint`.
        origin_hardpoint: Vec<u8>,
    },
}

/// A `ServerFuseAction`: the Common base and the server's kind. The owner
/// (`ServerFuseAction::Owner`) is the object whose fuse list holds it.
#[derive(Clone, Debug, PartialEq)]
pub struct ServerFuseAction {
    /// The `FuseAction` part.
    pub base: FuseAction,
    /// What it does.
    pub kind: FuseActionKind,
}

impl FuseActionBase for ServerFuseAction {
    fn base(&self) -> &FuseAction {
        &self.base
    }

    fn base_mut(&mut self) -> &mut FuseAction {
        &mut self.base
    }
}

fn value_f32(reader: &mut IniReader, index: u32) -> f32 {
    reader.value_float(index) as f32
}

/// `TString<32>` of the whole value, compared byte for byte.
fn tstring32(reader: &IniReader) -> Vec<u8> {
    let mut text = neuron_core::hash::c_str(&reader.value_string()).to_vec();
    text.truncate(31);
    text
}

impl ServerFuseAction {
    /// An action's constructor.
    #[must_use]
    pub fn new(kind: FuseActionKind) -> Self {
        Self {
            base: FuseAction::default(),
            kind,
        }
    }

    /// The action a header names, with its constructor's values, or
    /// `None` for a header the server does not read.
    #[must_use]
    pub fn for_header(reader: &IniReader) -> Option<Self> {
        let kind = if reader.is_header(b"ignite_fuse") {
            FuseActionKind::IgniteFuse {
                fuse: 0,
                fuse_t: 0.0,
            }
        } else if reader.is_header(b"destroy_root") {
            FuseActionKind::DestroyRoot
        } else if reader.is_header(b"damage_root") {
            FuseActionKind::DamageRoot {
                hit_points: 0,
                can_kill: true,
            }
        } else if reader.is_header(b"destroy_group") {
            FuseActionKind::DestroyGroup {
                group: Vec::new(),
                fate: 0,
            }
        } else if reader.is_header(b"damage_group") {
            FuseActionKind::DamageGroup {
                group: Vec::new(),
                hit_points: 0,
                can_kill: true,
            }
        } else if reader.is_header(b"destroy_hp_attachment") {
            FuseActionKind::DestroyHardpointAttachment {
                hardpoint: Vec::new(),
                fate: 0,
            }
        } else if reader.is_header(b"damage_hp_attachment") {
            FuseActionKind::DamageHardpointAttachment {
                hardpoint: Vec::new(),
                hit_points: 0,
                can_kill: true,
            }
        } else if reader.is_header(b"impulse") {
            FuseActionKind::Impulse {
                hardpoint: Vec::new(),
                force: 0.0,
                radius: 0.0,
                damage: 0.0,
                pos_offset: Vector::ZERO,
            }
        } else if reader.is_header(b"tumble") {
            FuseActionKind::Tumble {
                ang_drag_scale: 1.0,
                turn_throttle_low: Vector::ZERO,
                turn_throttle_high: Vector::new(1.0, 1.0, 1.0),
                throttle_low: 0.0,
                throttle_high: 0.0,
                engine_kill: false,
            }
        } else if reader.is_header(b"make_invincible") {
            FuseActionKind::MakeInvincible { turn_on: true }
        } else if reader.is_header(b"dump_cargo") {
            FuseActionKind::DumpCargo {
                origin_hardpoint: Vec::new(),
            }
        } else {
            return None;
        };
        Some(Self::new(kind))
    }

    /// `Slot08`: the section's values. `at_t` is the base's; every action
    /// reads to the next header and succeeds.
    pub fn read(&mut self, reader: &mut IniReader) -> bool {
        let name_key: &[u8] = match self.kind {
            FuseActionKind::DamageGroup { .. } => b"group_name",
            _ => b"hardpoint",
        };
        while reader.read_value() {
            if self.base.read_value(reader) {
                continue;
            }
            match &mut self.kind {
                FuseActionKind::IgniteFuse { fuse, fuse_t } => {
                    if reader.is_value(b"fuse") {
                        *fuse = create_id(&reader.value_string());
                    } else if reader.is_value(b"fuse_t") {
                        *fuse_t = value_f32(reader, 0);
                    }
                }
                FuseActionKind::DestroyRoot => {}
                FuseActionKind::DamageRoot {
                    hit_points,
                    can_kill,
                } => {
                    if reader.is_value(b"damage_type") {
                        *can_kill = !eq_ignore_case(
                            neuron_core::hash::c_str(&reader.value_string()),
                            b"absolute",
                        );
                    } else if reader.is_value(b"hitpoints") {
                        *hit_points = reader.value_int(0);
                    }
                }
                FuseActionKind::DestroyGroup { group, fate } => {
                    if reader.is_value(b"group_name") {
                        *group = neuron_core::hash::c_str(&reader.value_string()).to_vec();
                    } else if reader.is_value(b"fate") {
                        match tstring32(reader).as_slice() {
                            b"debris" => *fate = 2,
                            b"disappear" => *fate = 3,
                            _ => {}
                        }
                    }
                }
                FuseActionKind::DamageGroup {
                    group: name,
                    hit_points,
                    can_kill,
                }
                | FuseActionKind::DamageHardpointAttachment {
                    hardpoint: name,
                    hit_points,
                    can_kill,
                } => {
                    if reader.is_value(name_key) {
                        *name = neuron_core::hash::c_str(&reader.value_string()).to_vec();
                    } else if reader.is_value(b"damage_type") {
                        *can_kill = tstring32(reader) != b"absolute";
                    } else if reader.is_value(b"hitpoints") {
                        *hit_points = reader.value_int(0);
                    }
                }
                FuseActionKind::DestroyHardpointAttachment { hardpoint, fate } => {
                    if reader.is_value(b"hardpoint") {
                        *hardpoint = neuron_core::hash::c_str(&reader.value_string()).to_vec();
                    } else if reader.is_value(b"fate") {
                        match tstring32(reader).as_slice() {
                            b"debris" => *fate = 2,
                            b"disappear" => *fate = 3,
                            b"loot" => *fate = 5,
                            _ => {}
                        }
                    }
                }
                FuseActionKind::Impulse {
                    hardpoint,
                    force,
                    radius,
                    damage,
                    pos_offset,
                } => {
                    if reader.is_value(b"hardpoint") {
                        *hardpoint = neuron_core::hash::c_str(&reader.value_string()).to_vec();
                    } else if reader.is_value(b"force") {
                        *force = value_f32(reader, 0);
                    } else if reader.is_value(b"radius") {
                        *radius = value_f32(reader, 0);
                    } else if reader.is_value(b"damage") {
                        *damage = value_f32(reader, 0);
                    } else if reader.is_value(b"pos_offset") {
                        let x = value_f32(reader, 0);
                        let y = value_f32(reader, 1);
                        let z = value_f32(reader, 2);
                        *pos_offset = Vector::new(x, y, z);
                    }
                }
                FuseActionKind::Tumble {
                    ang_drag_scale,
                    turn_throttle_low,
                    turn_throttle_high,
                    throttle_low,
                    throttle_high,
                    engine_kill,
                } => {
                    if reader.is_value(b"ang_drag_scale") {
                        *ang_drag_scale = value_f32(reader, 0);
                    } else if reader.is_value(b"turn_throttle_x") {
                        turn_throttle_low.x = value_f32(reader, 0);
                        turn_throttle_high.x = value_f32(reader, 1);
                    } else if reader.is_value(b"turn_throttle_y") {
                        turn_throttle_low.y = value_f32(reader, 0);
                        turn_throttle_high.y = value_f32(reader, 1);
                    } else if reader.is_value(b"turn_throttle_z") {
                        turn_throttle_low.z = value_f32(reader, 0);
                        turn_throttle_high.z = value_f32(reader, 1);
                    } else if reader.is_value(b"throttle") {
                        *throttle_low = value_f32(reader, 0);
                        *throttle_high = value_f32(reader, 1);
                    } else if reader.is_value(b"engine_kill") {
                        *engine_kill = reader.value_bool(0);
                    }
                }
                FuseActionKind::MakeInvincible { turn_on } => {
                    if reader.is_value(b"turn_on") {
                        *turn_on = reader.value_bool(0);
                    }
                }
                FuseActionKind::DumpCargo { origin_hardpoint } => {
                    if reader.is_value(b"origin_hardpoint") {
                        *origin_hardpoint = reader.value_string_at(0);
                    }
                }
            }
        }
        true
    }

    /// `Slot04`: the action's own copy. The base keeps its times only;
    /// a tumble's copy keeps the constructor's turn throttles (0, 0, 0)
    /// to (1, 1, 1), as the C++ copy does not copy them.
    #[must_use]
    pub fn copy(&self) -> Self {
        let kind = match &self.kind {
            FuseActionKind::Tumble {
                ang_drag_scale,
                throttle_low,
                throttle_high,
                engine_kill,
                ..
            } => FuseActionKind::Tumble {
                ang_drag_scale: *ang_drag_scale,
                turn_throttle_low: Vector::ZERO,
                turn_throttle_high: Vector::new(1.0, 1.0, 1.0),
                throttle_low: *throttle_low,
                throttle_high: *throttle_high,
                engine_kill: *engine_kill,
            },
            other => other.clone(),
        };
        Self {
            base: self.base.arch_copy(),
            kind,
        }
    }
}

/// `ServerFuse`: a fuse over the server's actions.
#[derive(Clone, Debug, PartialEq)]
pub struct ServerFuse {
    /// The Common fuse.
    pub fuse: Fuse<ServerFuseAction>,
    /// A `[destroy_root]` was read (`bHasDestroyRoot`).
    pub has_destroy_root: bool,
}

impl Default for ServerFuse {
    fn default() -> Self {
        Self::new()
    }
}

impl ServerFuse {
    /// `ServerFuse(CreateID(""), -1)`.
    #[must_use]
    pub fn new() -> Self {
        Self {
            fuse: Fuse::new(create_id(b""), -1.0),
            has_destroy_root: false,
        }
    }

    /// `ServerFuse::Read`: the `[fuse]` values, then each action section
    /// up to the next `[fuse]` or the end. A section the server does not
    /// read is skipped; an action that reads is inserted under its time.
    pub fn read(&mut self, reader: &mut IniReader) -> bool {
        if !self.fuse.read_values(reader) {
            return false;
        }
        if !reader.read_header() {
            return true;
        }
        loop {
            if reader.is_header(b"fuse") {
                return true;
            }
            if let Some(mut action) = ServerFuseAction::for_header(reader) {
                if reader.is_header(b"destroy_root") {
                    self.has_destroy_root = true;
                }
                if action.read(reader) {
                    let randomize = action.base.should_randomize();
                    self.fuse.add_action(action);
                    if randomize {
                        self.fuse.needs_randomize = true;
                    }
                }
            }
            if !reader.read_header() {
                return true;
            }
        }
    }

    /// `ServerFuse(const ServerFuse&)`: the copy an object lights, its
    /// actions copied by their own copies, no burns.
    #[must_use]
    pub fn lit_copy(&self) -> Self {
        Self {
            fuse: self.fuse.copy_with(ServerFuseAction::copy),
            has_destroy_root: self.has_destroy_root,
        }
    }
}

/// `ServerFuseDB`: the prototypes by fuse id, and what the files' reader
/// reported.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct FuseDb {
    /// `ServerFuseDB::Fuses`.
    pub fuses: BTreeMap<u32, ServerFuse>,
    /// The INI reader's reports (`FDUMP`) while the files were read.
    pub reports: Vec<IniReport>,
}

impl FuseDb {
    /// `ServerFuseDB::LoadAll`: every file of the `[Data]` list, in order.
    #[must_use]
    pub fn load_all(source: &dyn DataSource, files: &FuseFiles) -> Self {
        let mut db = Self::default();
        for path in &files.0 {
            db.load_file(source, path);
        }
        db
    }

    /// `ServerFuseDB::LoadFile`: each `[fuse]` section; a file that does
    /// not open is skipped silently, and a fuse whose name is already in
    /// the database is dropped (the first one stays).
    pub fn load_file(&mut self, source: &dyn DataSource, path: &[u8]) {
        let Some(mut reader) = open_ini(source, path) else {
            return;
        };
        let _ = reader.read_header();
        while !reader.is_end() {
            if reader.is_header(b"fuse") {
                let mut fuse = ServerFuse::new();
                let at = reader.tell();
                if fuse.read(&mut reader) {
                    self.fuses.entry(fuse.fuse.nickname).or_insert(fuse);
                }
                if reader.tell() == at && !reader.read_header() {
                    break;
                }
            } else if !reader.read_header() {
                break;
            }
        }
        self.reports.extend(reader.take_reports());
    }

    /// A prototype.
    #[must_use]
    pub fn get(&self, id: u32) -> Option<&ServerFuse> {
        self.fuses.get(&id)
    }
}

/// The fuse database, read on first use from the world's data root and
/// the `[Data]` list's fuse files (`ServerFuseDB::LoadAll`, at server
/// start in the C++); empty without a data root.
pub fn fuse_db<'c>(cx: &'c mut Cx<'_>) -> &'c mut FuseDb {
    if cx.logic.fuses.is_none() {
        let db = match cx.world.data_source() {
            Some(source) => FuseDb::load_all(source, &cx.world.data().fuse_files),
            None => FuseDb::default(),
        };
        cx.logic.fuses = Some(db);
    }
    cx.logic.fuses.get_or_insert_with(FuseDb::default)
}

fn fuse_mut<'c>(cx: &'c mut Cx<'_>, h: ObjHandle, index: usize) -> Option<&'c mut ServerFuse> {
    cx.logic.objects.entry_mut(h)?.fuses.get_mut(index)
}

fn is_eq_obj(world: &World, h: ObjHandle) -> bool {
    world.get(h).is_some_and(|o| {
        matches!(o.kind, ObjectKind::Ship | ObjectKind::Solar) && o.equipped.is_some()
    })
}

/// The `EqObj` archetype of a ship or a solar.
fn eq_obj_arch(world: &World, h: ObjHandle) -> Option<&game_sim::data::archetype::eq_obj::EqObj> {
    let o = world.get(h)?;
    let a = &world.data().archetypes;
    match o.kind {
        ObjectKind::Ship => a.ships.get(&o.archetype).map(|s| &s.eq_obj),
        ObjectKind::Solar => a.solars.get(&o.archetype).map(|s| &s.eq_obj),
        _ => None,
    }
}

/// The archetype's fuse list (`eqobjarch()->Fuses`).
fn arch_fuses(world: &World, h: ObjHandle) -> Vec<game_sim::data::archetype::eq_obj::GroupFuse> {
    eq_obj_arch(world, h).map_or_else(Vec::new, |a| a.fuses.clone())
}

/// A collision group's fuse ignitions (`FuseIgnitions`).
fn group_fuses(
    world: &World,
    h: ObjHandle,
    group: SubObjId,
) -> Vec<game_sim::data::archetype::eq_obj::GroupFuse> {
    eq_obj_arch(world, h)
        .and_then(|a| a.group_by_id(group))
        .map_or_else(Vec::new, |g| g.fuses.clone())
}

fn hull_hit_pts(world: &World, h: ObjHandle) -> Option<f32> {
    world.get(h).map(|o| o.simple.hit_pts)
}

fn group_hit_pts(world: &World, h: ObjHandle, group: SubObjId) -> Option<f32> {
    world
        .get(h)?
        .equipped
        .as_deref()?
        .arch_groups
        .iter()
        .find(|g| g.id == group)
        .map(|g| g.hit_pts)
}

/// `EqObj::is_death_fuse_burning`: a death fuse of the object is lit and
/// not spent.
#[must_use]
pub fn is_death_fuse_burning(objects: &crate::objects::ServerObjects, h: ObjHandle) -> bool {
    objects.get(h).is_some_and(|o| {
        o.fuses
            .iter()
            .any(|f| f.fuse.death_fuse && !f.fuse.is_spent())
    })
}

/// `FDeathFuseEvent` (controller message 0x38) for the object: Content's
/// answer, the death conversation's length (0 without one).
pub fn send_death_fuse_lit(cx: &mut Cx<'_>, h: ObjHandle) -> f32 {
    let Some(o) = cx.world.get(h) else {
        return 0.0;
    };
    let mut message = ControllerMessage::DeathFuseLit(DeathFuseLit {
        system: o.physical.system,
        object: o.simple.id,
        object_type: o.simple.type_bits,
        lifetime: 0.0,
    });
    let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
    match message {
        ControllerMessage::DeathFuseLit(e) => e.lifetime,
        _ => 0.0,
    }
}

/// `EqObj::check_fuses` (when a system's solars are wrapped or loaded, and
/// a ship's equipment state is set): every archetype fuse whose hit-point
/// threshold the hull is at or below lights on the root, then each
/// collision group's on the group. The server's equipment wrappers
/// (`rebuild_server_equip`) come with their part.
pub fn check_fuses(cx: &mut Cx<'_>, h: ObjHandle) {
    if !is_eq_obj(cx.world, h) {
        return;
    }
    for f in arch_fuses(cx.world, h) {
        if hull_hit_pts(cx.world, h).is_none_or(|hp| hp > f.value2) {
            continue;
        }
        light_fuse(cx, h, 0, f.fuse, SUBOBJ_ID_ROOT, f.value1, -1.0);
    }
    let groups: Vec<SubObjId> = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|e| e.arch_groups.iter().map(|g| g.id).collect())
        .unwrap_or_default();
    for group in groups {
        for f in group_fuses(cx.world, h, group) {
            if group_hit_pts(cx.world, h, group).is_none_or(|hp| hp > f.value2) {
                continue;
            }
            light_fuse(cx, h, 0, f.fuse, group, f.value1, -1.0);
        }
    }
}

/// `EqObj::light_fuse`: a copy of the fuse already in the object's list is
/// lit again at `at_t` on `sub_obj` (`lifetime` is then not applied);
/// otherwise the database's prototype is copied to the end of the list,
/// its ranged actions draw their times, and it is lit. The first fuse
/// with a `destroy_root` an object lights sends `DeathFuseLit` and burns
/// at least as long as Content's answer; another fuse takes `lifetime`
/// unless it is -1. An unknown fuse is reported. True when the fuse was
/// found. The players' notice is the replication part's; a ship's word to
/// its followers about `death_comm` is the behaviour manager's.
pub fn light_fuse(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    source: u32,
    fuse: u32,
    sub_obj: SubObjId,
    at_t: f32,
    lifetime: f32,
) -> bool {
    // Whether a burn was added, and the lifetime the C++ broadcasts (-1
    // for a fuse already in the list), go to the players with replication.
    let Some(o) = cx.logic.objects.entry(cx.world, h) else {
        return false;
    };
    if let Some(lit) = o.fuses.iter_mut().find(|f| f.fuse.nickname == fuse) {
        lit.fuse.ignite_at(source, sub_obj, at_t);
        return true;
    }
    let Some(mut lit) = fuse_db(cx).get(fuse).map(ServerFuse::lit_copy) else {
        cx.logic.reports.push(
            EQOBJ,
            0xc73,
            format!("*** WARNING: Fuse(0x{fuse:x}) not found in FuseDB. Aborting."),
        );
        return false;
    };
    lit.fuse.randomize_actions(cx.world.rng());
    let has_destroy_root = lit.has_destroy_root;
    let Some(o) = cx.logic.objects.entry(cx.world, h) else {
        return false;
    };
    o.fuses.push(lit);
    let index = o.fuses.len() - 1;
    let first_death = !o.death_fuse_lit && has_destroy_root;
    if first_death {
        o.death_fuse_lit = true;
        let answer = send_death_fuse_lit(cx, h);
        if let Some(f) = fuse_mut(cx, h, index)
            && f.fuse.lifetime < answer
        {
            f.fuse.override_lifetime(answer);
        }
    } else if lifetime != -1.0
        && let Some(f) = fuse_mut(cx, h, index)
    {
        f.fuse.override_lifetime(lifetime);
    }
    if let Some(f) = fuse_mut(cx, h, index) {
        f.fuse.ignite_at(source, sub_obj, at_t);
    }
    true
}

/// `EqObj::unlight_fuse`: the first copy of the fuse in the object's list
/// loses its burn on `sub_obj` lit at `at_t`; each action that burn had
/// triggered is untriggered, and an `ignite_fuse` among them unlights the
/// fuse it lit (and may trigger again). True when the fuse is in the list.
/// The players' notice is the replication part's.
pub fn unlight_fuse(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    fuse: u32,
    sub_obj: SubObjId,
    at_t: f32,
) -> bool {
    let Some(index) = cx
        .logic
        .objects
        .get(h)
        .and_then(|o| o.fuses.iter().position(|f| f.fuse.nickname == fuse))
    else {
        return false;
    };
    let Some(mut walk) = fuse_mut(cx, h, index).and_then(|f| f.fuse.begin_unburn(sub_obj, at_t))
    else {
        return true;
    };
    while let Some(ignite) = untrigger_next(cx, h, index, &mut walk) {
        if let Some((target, fuse_t)) = ignite {
            unlight_fuse(cx, h, target, walk.sub_obj, fuse_t);
        }
    }
    if let Some(f) = fuse_mut(cx, h, index) {
        f.fuse.finish_unburn(&walk);
    }
    true
}

/// One step of `UnBurn`: the next triggered action is untriggered for the
/// burn's sub-object (`UnTrigger`); an `ignite_fuse` also clears its flag
/// and answers the fuse it unlights. `None` at the end of the walk.
#[allow(clippy::option_option)]
fn untrigger_next(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    index: usize,
    walk: &mut UnburnWalk,
) -> Option<Option<(u32, f32)>> {
    let f = fuse_mut(cx, h, index)?;
    let action = f.fuse.next_untrigger(walk)?;
    let (_, a) = f.fuse.actions.get_mut(action)?;
    a.base.untrigger(walk.sub_obj);
    Some(match a.kind {
        FuseActionKind::IgniteFuse { fuse, fuse_t } => {
            a.base.triggered = false;
            Some((fuse, fuse_t))
        }
        _ => None,
    })
}

/// `EqObj::update`'s fuse pass: `Fuse::Burn(dt)` on every fuse of the
/// object's list in order, a fuse lit during the pass included.
pub fn burn_fuses(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) {
    let mut index = 0;
    while cx
        .logic
        .objects
        .get(h)
        .is_some_and(|o| index < o.fuses.len())
    {
        burn_fuse(cx, h, index, dt);
        index += 1;
    }
}

/// `Fuse::Burn` on one fuse: the windows widen, then each action a burn
/// reaches that is not triggered for its sub-object runs its trigger
/// (the action's work only the first time it is triggered at all) and is
/// marked.
fn burn_fuse(cx: &mut Cx<'_>, h: ObjHandle, index: usize, dt: f32) {
    if !fuse_mut(cx, h, index).is_some_and(|f| f.fuse.widen(dt)) {
        return;
    }
    let mut walk = BurnWalk::default();
    loop {
        let Some(f) = fuse_mut(cx, h, index) else {
            return;
        };
        let Some(t) = f.fuse.next_trigger(&mut walk) else {
            return;
        };
        let pending = f
            .fuse
            .actions
            .get(t.action)
            .filter(|(_, a)| !a.base.triggered)
            .map(|(_, a)| a.kind.clone());
        if let Some(kind) = pending {
            trigger(cx, h, &kind, t.source, t.sub_obj);
        }
        if let Some((_, a)) = fuse_mut(cx, h, index).and_then(|f| f.fuse.actions.get_mut(t.action))
        {
            a.base.trigger(t.sub_obj);
        }
    }
}

/// `InitFuseDamageList`: cause 0x15, inflicted by the burn's source, whose
/// player is read when the source is in the object's system.
fn fuse_damage_list(cx: &Cx<'_>, h: ObjHandle, source: u32) -> DamageList {
    let system = cx.world.get(h).map(|o| o.physical.system);
    let owner = cx
        .world
        .find(source)
        .and_then(|s| cx.world.get(s))
        .filter(|s| Some(s.physical.system) == system)
        .map_or(0, |s| s.simple.owner_player);
    DamageList::new(FUSE_DAMAGE_CAUSE, source, owner)
}

fn apply(cx: &mut Cx<'_>, h: ObjHandle, list: &DamageList) {
    let mut hooks = crate::world::Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, list, &mut hooks);
}

/// Runs a damage computation on the object with the server's state for
/// the list's inflictor.
fn compute(
    cx: &Cx<'_>,
    h: ObjHandle,
    list: &mut DamageList,
    f: impl FnOnce(
        &DamageTarget<'_>,
        &dyn Fn(&damage::ArchGroup, Option<neuron_core::scene::InstanceId>) -> bool,
        &dyn Fn(SubObjId) -> Option<neuron_core::scene::InstanceId>,
        &mut DamageList,
    ),
) {
    let state: DamageState =
        crate::objects::damage_state(&cx.logic.objects, h, list.inflictor_owner);
    cx.world
        .with_damage_target_state(h, state, |t, in_group, mount_of| {
            f(t, in_group, mount_of, list);
        });
}

/// The id of a living group by name (`CArchGroupManager::FindByName`, the
/// cache strings compared without letter case).
fn group_by_name(world: &World, h: ObjHandle, name: &[u8]) -> Option<(SubObjId, f32)> {
    world
        .get(h)?
        .equipped
        .as_deref()?
        .arch_groups
        .iter()
        .find(|g| eq_ignore_case(&g.name, name))
        .map(|g| (g.id, g.hit_pts))
}

/// `rand() * count / 0x8000` (a signed divide).
fn pick(cx: &mut Cx<'_>, count: i32) -> i32 {
    cx.world.rng().rand().wrapping_mul(count) / 0x8000
}

/// `DestroyGroupAction`'s `random`: the group whose living rank (from 1)
/// is the draw, else the name stays `random`; no living group, no name.
fn random_group(cx: &mut Cx<'_>, h: ObjHandle) -> Vec<u8> {
    let living: Vec<Vec<u8>> = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|e| {
            e.arch_groups
                .iter()
                .filter(|g| !g.is_dead())
                .map(|g| g.name.clone())
                .collect()
        })
        .unwrap_or_default();
    if living.is_empty() {
        return Vec::new();
    }
    let target = pick(cx, living.len() as i32);
    usize::try_from(target)
        .ok()
        .and_then(|t| t.checked_sub(1))
        .and_then(|t| living.get(t).cloned())
        .unwrap_or_else(|| b"random".to_vec())
}

/// `DestroyHardpointAttachmentAction`'s `random`: the attached item whose
/// rank (from 1) is the draw gives its hardpoint, else the name stays
/// `random`; with none attached, no name.
fn random_hardpoint(cx: &mut Cx<'_>, h: ObjHandle) -> Vec<u8> {
    let hardpoints: Vec<Vec<u8>> = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|e| {
            e.equipment
                .traverse(types::ATTACHED, false)
                .into_iter()
                .filter_map(|slot| e.equipment.get(slot))
                .map(|q| {
                    q.mount
                        .as_ref()
                        .map(|m| m.hardpoint.clone())
                        .unwrap_or_default()
                })
                .collect()
        })
        .unwrap_or_default();
    if hardpoints.is_empty() {
        return Vec::new();
    }
    let target = pick(cx, hardpoints.len() as i32);
    usize::try_from(target)
        .ok()
        .and_then(|t| t.checked_sub(1))
        .and_then(|t| hardpoints.get(t).cloned())
        .unwrap_or_else(|| b"random".to_vec())
}

/// `RandomBetween`: `rand() / 32768` of the way from `low` to `high`.
fn random_between(cx: &mut Cx<'_>, low: f32, high: f32) -> f32 {
    let roll = cx.world.rng().rand();
    let scaled = f64::from(roll) * (1.0 / 32768.0);
    (scaled * (f64::from(high) - f64::from(low)) + f64::from(low)) as f32
}

/// A server action's `Trigger` work, run the first time the action is
/// triggered (`if (!IsTriggered())`).
fn trigger(cx: &mut Cx<'_>, h: ObjHandle, kind: &FuseActionKind, source: u32, sub_obj: SubObjId) {
    match kind {
        FuseActionKind::IgniteFuse { fuse, fuse_t } => {
            light_fuse(cx, h, source, *fuse, sub_obj, *fuse_t, -1.0);
        }
        FuseActionKind::DestroyRoot => {
            crate::world::destroy_object(cx, h, 1, source);
        }
        FuseActionKind::DamageRoot {
            hit_points,
            can_kill,
        } => {
            let mut list = fuse_damage_list(cx, h, source);
            inflict_hull_damage(cx, h, *hit_points as f32, *can_kill, &mut list);
            apply(cx, h, &list);
        }
        FuseActionKind::DestroyGroup { group, fate } => {
            let name = if eq_ignore_case(group, b"random") {
                random_group(cx, h)
            } else {
                group.clone()
            };
            if !name.is_empty() {
                let mut list = fuse_damage_list(cx, h, source);
                destroy_group_by_name(cx, h, &name, Fate::from_value(*fate), &mut list);
                apply(cx, h, &list);
            }
        }
        FuseActionKind::DamageGroup {
            group,
            hit_points,
            can_kill,
        } => {
            let mut list = fuse_damage_list(cx, h, source);
            inflict_group_damage(cx, h, group, *hit_points as f32, *can_kill, &mut list);
            apply(cx, h, &list);
        }
        FuseActionKind::DestroyHardpointAttachment { hardpoint, fate } => {
            let name = if eq_ignore_case(hardpoint, b"random") {
                random_hardpoint(cx, h)
            } else {
                hardpoint.clone()
            };
            if !name.is_empty() {
                let mut list = fuse_damage_list(cx, h, source);
                destroy_equip_by_hardpoint(cx, h, &name, Fate::from_value(*fate), &mut list);
                apply(cx, h, &list);
            }
        }
        FuseActionKind::DamageHardpointAttachment {
            hardpoint,
            hit_points,
            can_kill,
        } => {
            let mut list = fuse_damage_list(cx, h, source);
            inflict_equip_damage(cx, h, hardpoint, *hit_points as f32, *can_kill, &mut list);
            apply(cx, h, &list);
        }
        FuseActionKind::Impulse {
            hardpoint,
            force,
            radius,
            damage,
            pos_offset,
        } => impulse(cx, h, hardpoint, *force, *radius, *damage, *pos_offset),
        FuseActionKind::Tumble {
            ang_drag_scale,
            turn_throttle_low,
            turn_throttle_high,
            throttle_low,
            throttle_high,
            engine_kill,
        } => {
            let Some(o) = cx.world.get(h) else {
                return;
            };
            if o.simple.owner_player != 0
                || o.kind != ObjectKind::Ship
                || cx.world.behavior_manager(h).is_some()
            {
                return;
            }
            cx.world
                .command(h, Command::SetAngularDragFactor(*ang_drag_scale));
            let x = random_between(cx, turn_throttle_low.x, turn_throttle_high.x);
            let y = random_between(cx, turn_throttle_low.y, turn_throttle_high.y);
            let z = random_between(cx, turn_throttle_low.z, turn_throttle_high.z);
            let Some(orientation) = cx.world.get(h).map(|o| o.engine.orientation) else {
                return;
            };
            let axis = neuron_core::math::transform_matrix(&orientation, Vector::new(x, y, z));
            cx.world.command(h, Command::SetAxisThrottle(axis));
            let throttle = random_between(cx, *throttle_low, *throttle_high);
            cx.world.command(h, Command::SetThrottle(throttle));
            if *engine_kill {
                deactivate_first_engine(cx, h);
            }
        }
        FuseActionKind::MakeInvincible { turn_on } => {
            if let Some(o) = cx.logic.objects.entry(cx.world, h) {
                o.set_invincible(Invincibility {
                    kind: 4,
                    protect: *turn_on,
                    from_players: *turn_on,
                    max_hp_loss: if *turn_on { 0.0 } else { 1.0 },
                });
            }
        }
        FuseActionKind::DumpCargo { origin_hardpoint } => {
            let mut list = fuse_damage_list(cx, h, source);
            dump_cargo(cx, h, origin_hardpoint, &mut list);
            apply(cx, h, &list);
        }
    }
}

/// `EqObj::inflict_hull_damage`: a killing hit of 0 or more, or a hit
/// that leaves the hull above 0.
fn inflict_hull_damage(
    cx: &Cx<'_>,
    h: ObjHandle,
    amount: f32,
    can_kill: bool,
    list: &mut DamageList,
) {
    let Some(hp) = hull_hit_pts(cx.world, h) else {
        return;
    };
    let go = if can_kill {
        amount >= 0.0
    } else {
        f64::from(hp) - f64::from(amount) > 0.0
    };
    if go {
        compute(cx, h, list, |t, _, _, list| {
            damage::damage_hull(t, amount, list);
        });
    }
}

/// `EqObj::destroy_group_by_name`.
fn destroy_group_by_name(
    cx: &Cx<'_>,
    h: ObjHandle,
    name: &[u8],
    fate: Fate,
    list: &mut DamageList,
) {
    let Some((id, _)) = group_by_name(cx.world, h, name) else {
        return;
    };
    compute(cx, h, list, |t, in_group, mount_of, list| {
        damage::destroy_group(t, id, fate, list, true, in_group, mount_of);
    });
}

/// `EqObj::inflict_group_damage`: with `can_kill` a hit of 0 or more;
/// otherwise the group's hit points less the hit, while above 0, is what
/// goes on as the damage (as the C++ passes it).
fn inflict_group_damage(
    cx: &Cx<'_>,
    h: ObjHandle,
    name: &[u8],
    amount: f32,
    can_kill: bool,
    list: &mut DamageList,
) {
    let Some((id, hp)) = group_by_name(cx.world, h, name) else {
        return;
    };
    let damage = if can_kill {
        amount
    } else {
        (f64::from(hp) - f64::from(amount)) as f32
    };
    let go = if can_kill {
        damage >= 0.0
    } else {
        damage > 0.0
    };
    if !go {
        return;
    }
    compute(cx, h, list, |t, in_group, mount_of, list| {
        damage::damage_group(t, id, damage, list, in_group, mount_of);
    });
}

fn equip_by_hardpoint(world: &World, h: ObjHandle, name: &[u8]) -> Option<(SubObjId, f32)> {
    let eq = world.get(h)?.equipped.as_deref()?;
    let slot = eq.equipment.find_by_hardpoint(name)?;
    eq.equipment.get(slot).map(|e| (e.id, e.hit_points()))
}

/// `EqObj::destroy_equip_by_hardpoint`.
fn destroy_equip_by_hardpoint(
    cx: &Cx<'_>,
    h: ObjHandle,
    name: &[u8],
    fate: Fate,
    list: &mut DamageList,
) {
    let Some((id, _)) = equip_by_hardpoint(cx.world, h, name) else {
        return;
    };
    compute(cx, h, list, |t, _, _, list| {
        damage::destroy_equip(t, id, fate, list);
    });
}

/// `EqObj::inflict_equip_damage`: with `can_kill` the hit as it is (no
/// sign test); otherwise the item's hit points less the hit, while above
/// 0, goes on as the damage.
fn inflict_equip_damage(
    cx: &Cx<'_>,
    h: ObjHandle,
    name: &[u8],
    amount: f32,
    can_kill: bool,
    list: &mut DamageList,
) {
    let Some((id, hp)) = equip_by_hardpoint(cx.world, h, name) else {
        return;
    };
    let damage = if can_kill {
        amount
    } else {
        (f64::from(hp) - f64::from(amount)) as f32
    };
    if !can_kill && damage.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return;
    }
    compute(cx, h, list, |t, _, _, list| {
        damage::damage_equip(t, id, damage, list);
    });
}

/// `ImpulseAction::Trigger`: for an object the server finds in a system,
/// an explosion `FuseImpulse` (radius, impulse and hull damage from the
/// action) at the hardpoint moved by `pos_offset` in its space, set off by
/// the object (cause 0x15): the damage pass skips it, the impulse pass
/// pushes it too. Nothing without the hardpoint.
fn impulse(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    hardpoint: &[u8],
    force: f32,
    radius: f32,
    damage_amount: f32,
    pos_offset: Vector,
) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let (id, system) = (o.simple.id, o.physical.system);
    if system == 0 || cx.world.find(id) != Some(h) {
        return;
    }
    let Some((position, orientation)) = cx.world.hardpoint_world(h, hardpoint) else {
        return;
    };
    let offset = neuron_core::math::transpose_transform(&orientation, pos_offset);
    let blast = damage::Blast {
        position: Vector::new(
            (f64::from(offset.x) + f64::from(position.x)) as f32,
            (f64::from(offset.y) + f64::from(position.y)) as f32,
            (f64::from(offset.z) + f64::from(position.z)) as f32,
        ),
        radius,
        hull_damage: damage_amount,
        energy_damage: 0.0,
    };
    let mut hooks = crate::world::Hooks {
        logic: &mut *cx.logic,
    };
    cx.world
        .explode_self_hooked(system, &blast, FUSE_DAMAGE_CAUSE, h, force, &mut hooks);
}

/// `TumbleAction`'s `engine_kill`: `slot_4c(engine, false)` on the first
/// engine. The switch is refused while the ship strafes or a thruster is
/// on; an engine already off stays as it is. A ship without an engine is
/// left alone (the C++ reads through null, D618).
fn deactivate_first_engine(cx: &mut Cx<'_>, h: ObjHandle) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let Some(eq) = o.equipped.as_deref() else {
        return;
    };
    let Some(engine) = eq.equipment.first(types::ENGINE) else {
        return;
    };
    let (id, active) = (engine.id, engine.is_active());
    let thrusting = eq
        .equipment
        .traverse(types::THRUSTER, false)
        .into_iter()
        .filter_map(|s| eq.equipment.get(s))
        .any(game_sim::equip::Equip::is_active);
    let strafing = cx
        .world
        .inspect(h)
        .and_then(|i| i.ship)
        .is_some_and(|s| s.strafe != game_sim::equip::flight::StrafeDir::None);
    if strafing || thrusting || !active {
        return;
    }
    cx.world.command(h, Command::ActivateEquip(id, false));
}

/// `EqObj::dump_cargo`: every cargo stack not held in a pod, with a
/// description, goes out as loot from `origin` (the object's position
/// when the object has no such hardpoint) and gets a destroyed entry. At a
/// solar a valid inflicting player first asks to queue the visit; a
/// refusal keeps the item, and a dumped one queues the visit.
fn dump_cargo(cx: &mut Cx<'_>, h: ObjHandle, origin: &[u8], list: &mut DamageList) {
    let ids: Vec<SubObjId> = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|e| {
            e.equipment
                .traverse(types::CARGO, false)
                .into_iter()
                .filter_map(|s| e.equipment.get(s))
                .filter(
                    |q| !matches!(&q.kind, game_sim::equip::EquipKind::Cargo(c) if c.pod.is_some()),
                )
                .map(|q| q.id)
                .collect()
        })
        .unwrap_or_default();
    for id in ids {
        let Some(mut desc) = cx.world.equip_desc(h, id) else {
            continue;
        };
        let solar = cx.world.get(h).and_then(|o| match &o.data {
            game_sim::object::KindData::Solar(s) => Some((o.simple.id, s.visit_value)),
            _ => None,
        });
        let inflictor = list.inflictor_owner;
        if let Some((key, flags)) = solar
            && let Some(slot) = cx.logic.players.get(inflictor)
            && !crate::player::should_queue_visit(slot, key, flags)
        {
            continue;
        }
        desc.hardpoint = cx.world.strings_mut().alloc(origin, false);
        let spawns = cx.world.loot_drops(h, std::slice::from_ref(&desc));
        if !spawns.is_empty() {
            let (system, source) = cx
                .world
                .get(h)
                .map_or((0, 0), |o| (o.physical.system, o.simple.id));
            crate::objects::create_loot(cx, system, source, &spawns);
        }
        list.add_damage_entry(id, 0.0, Fate::Destroyed);
        if let Some((key, flags)) = solar
            && let Some(slot) = cx.logic.players.get_mut(list.inflictor_owner)
        {
            crate::player::queue_visit(slot, key, flags);
        }
    }
}

/// `EqObj_SweepFuses` over one fuse list: a hit taking the sub-object's
/// hit points from above a threshold to it or below lights the fuse
/// there; healing from at or below a threshold to above it unlights it.
/// The hit points are read again for each fuse, since lighting can
/// change them.
fn sweep(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    fuses: &[game_sim::data::archetype::eq_obj::GroupFuse],
    read: impl Fn(&World) -> Option<f32>,
    after: f32,
    sub_obj: SubObjId,
    inflictor: u32,
) {
    for f in fuses {
        let Some(before) = read(cx.world) else {
            return;
        };
        if before > after {
            if read(cx.world).is_some_and(|hp| hp > f.value2) && f.value2 >= after {
                light_fuse(cx, h, inflictor, f.fuse, sub_obj, f.value1, -1.0);
            }
        } else {
            if f.value2 < after && read(cx.world).is_some_and(|hp| hp <= f.value2) {
                unlight_fuse(cx, h, f.fuse, sub_obj, f.value1);
            }
        }
    }
}

/// The fuse parts of `EqObj::apply_root_entry` and `apply_group_entry`, in
/// the list's order, before the entries are written: a root entry that
/// changes the hull's hit points sweeps the archetype's fuses, a group
/// entry that changes the group's sweeps the group's; then, while a death
/// fuse burns, a group entry of a cause below 0x10 loses its fate (the
/// group stays attached). A group entry naming a group the object lacks is
/// skipped (the C++ reads through null, D618).
pub fn sweep_damage_list(cx: &mut Cx<'_>, h: ObjHandle, list: &mut DamageList) {
    if !is_eq_obj(cx.world, h) || list.entries.is_empty() {
        return;
    }
    let inflictor = list.inflictor_id;
    for i in 0..list.entries.len() {
        let entry = list.entries[i];
        let id = entry.sub_obj_id;
        if id == SUBOBJ_ID_ROOT {
            if hull_hit_pts(cx.world, h).is_some_and(|hp| hp != entry.hit_pts) {
                let fuses = arch_fuses(cx.world, h);
                sweep(
                    cx,
                    h,
                    &fuses,
                    |w| hull_hit_pts(w, h),
                    entry.hit_pts,
                    SUBOBJ_ID_ROOT,
                    inflictor,
                );
            }
        } else if neuron_core::id::sub_object::is_arch_group_id(id) {
            let Some(hp) = group_hit_pts(cx.world, h, id) else {
                continue;
            };
            if hp != entry.hit_pts {
                let fuses = group_fuses(cx.world, h, id);
                sweep(
                    cx,
                    h,
                    &fuses,
                    |w| group_hit_pts(w, h, id),
                    entry.hit_pts,
                    id,
                    inflictor,
                );
            }
            if is_death_fuse_burning(&cx.logic.objects, h) && list.cause < 0x10 {
                list.entries[i].fate = Fate::None;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Text(&'static [u8]);

    impl DataSource for Text {
        fn read(&self, _path: &[u8]) -> Option<Vec<u8>> {
            Some(self.0.to_vec())
        }
    }

    const FILE: &[u8] = b"[fuse]
name = a
lifetime = 2
death_fuse = true

[start_effect]
at_t = 0
effect = x

[tumble]
at_t = 0.5
turn_throttle_x = 0.25, 0.5
throttle = 1, 1

[destroy_group]
at_t = 0.2, 0.4
group_name = random
fate = Debris

[ignite_fuse]
at_t = 0.1
fuse = b
fuse_t = 0.5

[fuse]
name = b
lifetime = 1

[destroy_root]
at_t = 1

[damage_root]
damage_type = ABSOLUTE
hitpoints = 12

[damage_group]
damage_type = ABSOLUTE
hitpoints = 3

[fuse]
name = a
lifetime = 9
";

    #[test]
    fn the_reader_builds_the_actions_by_time() {
        let mut files = FuseFiles::default();
        files.add(b"fx\\fuse.ini");
        let db = FuseDb::load_all(&Text(FILE), &files);
        assert_eq!(db.fuses.len(), 2);
        assert!(db.reports.is_empty());
        let a = db.get(create_id(b"a")).expect("a");
        assert_eq!(a.fuse.lifetime, 2.0, "the first `a` stays");
        assert!(a.fuse.death_fuse && !a.has_destroy_root);
        assert!(a.fuse.needs_randomize);
        let keys: Vec<f32> = a.fuse.actions.iter().map(|(k, _)| *k).collect();
        assert_eq!(keys, [0.1, 0.2, 0.5], "start_effect is the client's");
        match &a.fuse.actions[1].1.kind {
            FuseActionKind::DestroyGroup { group, fate } => {
                assert_eq!(group, b"random");
                assert_eq!(*fate, 0, "the fate is compared byte for byte");
            }
            other => panic!("{other:?}"),
        }
        let b = db.get(create_id(b"b")).expect("b");
        assert!(b.has_destroy_root);
        match (&b.fuse.actions[0].1.kind, &b.fuse.actions[1].1.kind) {
            (
                FuseActionKind::DamageRoot { can_kill: root, .. },
                FuseActionKind::DamageGroup {
                    can_kill: group, ..
                },
            ) => {
                assert!(!root, "damage_root compares without letter case");
                assert!(*group, "damage_group compares byte for byte");
            }
            other => panic!("{other:?}"),
        }
        // The copy an object lights keeps a tumble's default turn
        // throttles.
        let lit = a.lit_copy();
        match &lit.fuse.actions[2].1.kind {
            FuseActionKind::Tumble {
                turn_throttle_low,
                turn_throttle_high,
                throttle_low,
                ..
            } => {
                assert_eq!(*turn_throttle_low, Vector::ZERO);
                assert_eq!(*turn_throttle_high, Vector::new(1.0, 1.0, 1.0));
                assert_eq!(*throttle_low, 1.0);
            }
            other => panic!("{other:?}"),
        }
    }
}
