//! The space object model (work package P2.4): `CObject` to `CShip`,
//! `CSolar`, the projectiles, loot and asteroids of
//! `CppSource/Common/common_c*.cpp`, as decision record 0001 lays it out.
//!
//! * [`arena`]: the arena with generational handles; a handle to a reaped
//!   object resolves to `None` (points 1 and 4).
//! * [`Object`]: one world object: its [`ObjectKind`] (the C++ class), the
//!   shared components ([`EngineObject`], [`Physical`], [`Simple`],
//!   [`Equipped`]) and the per-kind data ([`KindData`]). The class masks
//!   stay as constants ([`class`]) for the data and the wire.
//! * [`world`]: the [`World`]: every loaded system's objects, the id map,
//!   deferred destruction, the update loop in the server's list order, the
//!   physics controller hook, the headless scene, and the typed inspection
//!   and command APIs ([`inspect`]) that replace `IObjInspect` and
//!   `IObjAffect`.
//! * [`ship`], [`solar`], [`projectile`], [`loot`], [`asteroid`]: the
//!   per-kind state and updates.
//! * [`damage`]: hit points, `DamageList`, collision groups (`CArchGroup`)
//!   and the damage application of munition hits and collisions.
//! * [`csys`]: frame helpers (`LookMatrix`, `RandomVector`).
//!
//! The behaviour manager (AI, P2.5) plugs in through
//! [`world::BehaviorHook`], called where `CEqObj::update` calls
//! `UpdateBehaviorTick`.

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true exactly
// when the reference takes that side, which needs negated comparisons
// (as `neuron-core::physics` does).
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod arena;
pub mod asteroid;
pub mod csys;
pub mod damage;
pub mod inspect;
pub mod loot;
pub mod projectile;
pub mod ship;
pub mod solar;
pub mod world;

use std::collections::BTreeSet;

use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::{BodyHandle, ControllerHandle};
use neuron_core::scene::InstanceId;

use crate::equip::EquipManager;

pub use arena::{Arena, ObjHandle};
pub use damage::{ArchGroup, DamageCause, DamageEntry, DamageList};
pub use inspect::{Command, CommandResult, Inspect};
pub use world::{World, WorldEvent};

/// The class masks (`CObject::Class`, `include/common_cobject.h:165`). An
/// "is-a" test is `(class & mask) == mask`.
pub mod class {
    /// `ClassObject`.
    pub const OBJECT: u32 = 1;
    /// `ClassSimple`.
    pub const SIMPLE: u32 = 0x03;
    /// `ClassEqObj`.
    pub const EQ_OBJ: u32 = 0x103;
    /// `ClassMine`.
    pub const MINE: u32 = 0x0f;
    /// `ClassCounterMeasure`.
    pub const COUNTER_MEASURE: u32 = 0x17;
    /// `ClassBeam`.
    pub const BEAM: u32 = 0x67;
    /// `ClassGuided`.
    pub const GUIDED: u32 = 0xa7;
    /// `ClassSolar`.
    pub const SOLAR: u32 = 0x303;
    /// `ClassShip`.
    pub const SHIP: u32 = 0x503;
    /// `ClassLoot`.
    pub const LOOT: u32 = 0x803;
    /// `ClassAsteroid`.
    pub const ASTEROID: u32 = 0x1003;
    /// `ClassEquipmentObj`.
    pub const EQUIPMENT_OBJ: u32 = 0x2001;
    /// `ClassDynamicAsteroid`.
    pub const DYNAMIC_ASTEROID: u32 = 0x4001;
    /// The projectiles (`CProjectile`): the three low bits, which make the
    /// physics leave a body after its first contact.
    pub const PROJECTILE: u32 = 0x07;
}

/// The `update` flags (`CObject::UPDATE_DEFAULT` and friends).
pub mod update_flags {
    /// `NO_DYNAMICS`.
    pub const NO_DYNAMICS: u32 = 0x01;
    /// `NO_SCANNER`.
    pub const NO_SCANNER: u32 = 0x04;
    /// `NO_BEHAVIOR`.
    pub const NO_BEHAVIOR: u32 = 0x08;
    /// `NO_ACTIONS`.
    pub const NO_ACTIONS: u32 = 0x10;
    /// `NO_COUNTERMEASURE_AI`.
    pub const NO_COUNTERMEASURE_AI: u32 = 0x20;
    /// `UPDATE_DUMB`.
    pub const UPDATE_DUMB: u32 = 0x3d;
}

/// The concrete classes `CObject::Alloc` builds. `CMunition` and
/// `CProjectile` are abstract in the reference (no class value creates
/// them), so their shared state is [`projectile::Projectile`] inside the
/// kinds that derive from them.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ObjectKind {
    /// `CShip` (0x503).
    Ship,
    /// `CSolar` (0x303).
    Solar,
    /// `CLoot` (0x803).
    Loot,
    /// `CBeam` (0x67): unguided munitions, moved as rays.
    Beam,
    /// `CGuided` (0xa7): missiles.
    Guided,
    /// `CMine` (0x0f).
    Mine,
    /// `CCounterMeasure` (0x17).
    CounterMeasure,
    /// `CAsteroid` (0x1003).
    Asteroid,
    /// `CDynamicAsteroid` (0x4001).
    DynamicAsteroid,
    /// `CEquipmentObj` (0x2001): the body of physically attached equipment.
    EquipmentObject,
}

impl ObjectKind {
    /// The class mask.
    #[must_use]
    pub fn class(self) -> u32 {
        match self {
            Self::Ship => class::SHIP,
            Self::Solar => class::SOLAR,
            Self::Loot => class::LOOT,
            Self::Beam => class::BEAM,
            Self::Guided => class::GUIDED,
            Self::Mine => class::MINE,
            Self::CounterMeasure => class::COUNTER_MEASURE,
            Self::Asteroid => class::ASTEROID,
            Self::DynamicAsteroid => class::DYNAMIC_ASTEROID,
            Self::EquipmentObject => class::EQUIPMENT_OBJ,
        }
    }

    /// `(class & mask) == mask`.
    #[must_use]
    pub fn is(self, mask: u32) -> bool {
        self.class() & mask == mask
    }
}

/// `EngineObject` (0x4c bytes): the object's place on the engine instance
/// tree and its frame.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EngineObject {
    /// +0x04 the root instance (-1 is `None`).
    pub instance: Option<InstanceId>,
    /// +0x08 the orientation.
    pub orientation: Matrix,
    /// +0x2c the world position.
    pub position: Vector,
    /// +0x38 the centred radius.
    pub radius: f32,
    /// +0x3c its centre.
    pub radius_center: Vector,
    /// +0x48 the instance flags.
    pub instance_flags: u32,
}

impl Default for EngineObject {
    fn default() -> Self {
        Self {
            instance: None,
            orientation: Matrix::IDENTITY,
            position: Vector::ZERO,
            radius: 0.0,
            radius_center: Vector::ZERO,
            instance_flags: 0,
        }
    }
}

/// A `CObject::PartMap` entry: a part and its instance.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PartMapEntry {
    /// The part id (the instance name's CRC).
    pub part_id: u32,
    /// The instance.
    pub instance: Option<InstanceId>,
    /// The part is a shield's.
    pub shield_part: bool,
}

/// The physical part of `CObject` (+0x50..+0x84).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Physical {
    /// +0x50 the physics system id.
    pub system: u32,
    /// +0x54 the body, `None` when not physical.
    pub body: Option<BodyHandle>,
    /// +0x64 the part map.
    pub part_map: Vec<PartMapEntry>,
    /// +0x74 the control mode.
    pub control_mode: i32,
    /// The part ids the body's ledges carry (the surface manager's filter
    /// hides those the part map lacks).
    pub ledge_parts: Vec<u32>,
    /// The registered force controllers of the object and its equipment.
    pub controllers: Vec<(world::ControllerRef, ControllerHandle, u64)>,
}

/// `CSimple` (0x9c..0xe4).
#[derive(Clone, Debug, PartialEq)]
pub struct Simple {
    /// +0xa0 bit 0: targetable.
    pub targetable: bool,
    /// +0xa4 the smallest physical zone the object is in (its nickname).
    pub zone: Option<u32>,
    /// That zone's drag modifier (`IZone::GetDragModifier`).
    pub zone_drag: Option<f32>,
    /// +0xa8 the immersion in that zone.
    pub scanner_interference: f32,
    /// +0xac the system whose object database holds it.
    pub system: Option<u32>,
    /// +0xb0 the object id.
    pub id: u32,
    /// +0xb4 the owning player.
    pub owner_player: u32,
    /// +0xb8 the hit points.
    pub hit_pts: f32,
    /// The archetype's hit points (`get_max_hit_pts`).
    pub max_hit_pts: f32,
    /// +0xbc the attached damage objects by name (`AttachedDamageObjs`).
    pub attached_damage_objs: BTreeSet<Vec<u8>>,
    /// +0xd0 the cached centre of mass.
    pub cached_center: Vector,
    /// +0xdc the cached radius, -1 until cached.
    pub cached_radius: f32,
    /// +0xe0 the type bits (the archetype's `OBJECT_TYPE`).
    pub type_bits: u32,
}

impl Default for Simple {
    fn default() -> Self {
        Self {
            targetable: true,
            zone: None,
            zone_drag: None,
            scanner_interference: 0.0,
            system: None,
            id: 0,
            owner_player: 0,
            hit_pts: 0.0,
            max_hit_pts: 0.0,
            attached_damage_objs: BTreeSet::new(),
            cached_center: Vector::ZERO,
            cached_radius: -1.0,
            type_bits: 0,
        }
    }
}

impl Simple {
    /// `get_relative_health`, unrounded.
    #[must_use]
    pub fn relative_health(&self) -> f64 {
        f64::from(self.hit_pts) / f64::from(self.max_hit_pts)
    }
}

/// A docking point (`CEqObj::DockAnimInfo` with its archetype sphere).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DockingPoint {
    /// The sphere's hardpoint.
    pub hardpoint: Vec<u8>,
    /// Its radius.
    pub radius: f32,
    /// Its dock type.
    pub dock_type: u32,
    /// The animation's name.
    pub anim: Vec<u8>,
    /// The animation plays.
    pub active: bool,
}

/// `CEqObj` (0xe4..0x1a4).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Equipped {
    /// +0xe4 the equipment.
    pub equipment: EquipManager,
    /// +0x104 the reputation group (-1 none).
    pub vibe: i32,
    /// +0x13c the voice.
    pub voice: u32,
    /// +0x144 the collision groups.
    pub arch_groups: Vec<ArchGroup>,
    /// +0x15c the weapons are offline (`CELauncher::IsDisabled`).
    pub weapons_offline: bool,
    /// +0x160 the base the object docks in.
    pub docking_base: u32,
    /// +0x164 the base of its docking point.
    pub docking_point_base: u32,
    /// +0x180 the docking points.
    pub docking_points: Vec<DockingPoint>,
    /// +0x190 the control exclusion flags.
    pub control_exclusion: u32,
    /// +0x194 a behaviour manager is attached (P2.5 runs it through the
    /// world's hook).
    pub has_behavior: bool,
    /// +0x198 the power.
    pub power: f32,
    /// +0x19c the maximum power (the power plants' capacity).
    pub max_power: f32,
    /// +0x1a0 the behaviour index without a manager (-1).
    pub behavior_index: i32,
    /// +0x140 the cloak percentage.
    pub cloak_percent: f32,
    /// The explosion bounding sphere cache.
    pub explosion_sphere: Option<(f32, Vector)>,
}

/// The per-kind data.
#[derive(Clone, Debug, PartialEq)]
pub enum KindData {
    /// `CShip`.
    Ship(Box<ship::ShipData>),
    /// `CSolar`.
    Solar(Box<solar::SolarData>),
    /// `CLoot`.
    Loot(loot::LootData),
    /// `CBeam`.
    Beam(projectile::BeamData),
    /// `CGuided`.
    Guided(projectile::GuidedData),
    /// `CMine`.
    Mine(projectile::MineData),
    /// `CCounterMeasure`.
    CounterMeasure(projectile::CounterMeasureData),
    /// `CAsteroid`.
    Asteroid(asteroid::AsteroidData),
    /// `CDynamicAsteroid`.
    DynamicAsteroid(asteroid::DynamicAsteroidData),
    /// `CEquipmentObj`: the owner and the item.
    EquipmentObject {
        /// The equipped object the item hangs on.
        owner: Option<ObjHandle>,
        /// The item's id.
        equip: crate::equip::SubObjId,
    },
}

/// One world object.
#[derive(Clone, Debug, PartialEq)]
pub struct Object {
    /// The class.
    pub kind: ObjectKind,
    /// The archetype's nickname id (+0x88).
    pub archetype: u32,
    /// The archetype's mass (`get_mass` without a body).
    pub arch_mass: f32,
    /// `EngineObject`.
    pub engine: EngineObject,
    /// The physical part of `CObject`.
    pub physical: Physical,
    /// `CSimple`; every kind but the equipment object and the dynamic
    /// asteroid has one (theirs is unused).
    pub simple: Simple,
    /// `CEqObj` of ships and solars.
    pub equipped: Option<Box<Equipped>>,
    /// The kind's own data.
    pub data: KindData,
    /// Marked for the end-of-tick reap (decision record 0001, point 5).
    pub dead: bool,
}

impl Object {
    /// `(class & mask) == mask`.
    #[must_use]
    pub fn is(&self, mask: u32) -> bool {
        self.kind.is(mask)
    }

    /// The ship data, for a ship.
    #[must_use]
    pub fn as_ship(&self) -> Option<&ship::ShipData> {
        match &self.data {
            KindData::Ship(s) => Some(s),
            _ => None,
        }
    }

    /// The ship data, mutably.
    pub fn as_ship_mut(&mut self) -> Option<&mut ship::ShipData> {
        match &mut self.data {
            KindData::Ship(s) => Some(s),
            _ => None,
        }
    }

    /// The equipped part (`(class & 0x103) == 0x103`).
    #[must_use]
    pub fn as_equipped(&self) -> Option<&Equipped> {
        self.equipped.as_deref()
    }

    /// The equipped part, mutably.
    pub fn as_equipped_mut(&mut self) -> Option<&mut Equipped> {
        self.equipped.as_deref_mut()
    }

    /// The projectile part of the projectile kinds.
    #[must_use]
    pub fn as_projectile(&self) -> Option<&projectile::Projectile> {
        match &self.data {
            KindData::Beam(b) => Some(&b.projectile),
            KindData::Guided(g) => Some(&g.projectile),
            KindData::Mine(m) => Some(&m.projectile),
            KindData::CounterMeasure(c) => Some(&c.projectile),
            _ => None,
        }
    }

    /// The projectile part, mutably.
    pub fn as_projectile_mut(&mut self) -> Option<&mut projectile::Projectile> {
        match &mut self.data {
            KindData::Beam(b) => Some(&mut b.projectile),
            KindData::Guided(g) => Some(&mut g.projectile),
            KindData::Mine(m) => Some(&mut m.projectile),
            KindData::CounterMeasure(c) => Some(&mut c.projectile),
            _ => None,
        }
    }

    /// `inst_to_part`: the part of an instance, 0 when it has none.
    #[must_use]
    pub fn inst_to_part(&self, instance: Option<InstanceId>) -> u32 {
        self.physical
            .part_map
            .iter()
            .find(|p| p.instance == instance)
            .map_or(0, |p| p.part_id)
    }

    /// `part_to_inst`.
    #[must_use]
    pub fn part_to_inst(&self, part_id: u32) -> Option<InstanceId> {
        self.physical
            .part_map
            .iter()
            .find(|p| p.part_id == part_id)
            .and_then(|p| p.instance)
    }

    /// `is_shield_part`.
    #[must_use]
    pub fn is_shield_part(&self, part_id: u32) -> bool {
        self.physical
            .part_map
            .iter()
            .any(|p| p.part_id == part_id && p.shield_part)
    }

    /// `flag_part_as_shield`.
    pub fn flag_part_as_shield(&mut self, part_id: u32) -> bool {
        match self
            .physical
            .part_map
            .iter_mut()
            .find(|p| p.part_id == part_id)
        {
            Some(p) => {
                p.shield_part = true;
                true
            }
            None => false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn class_masks_nest() {
        assert!(ObjectKind::Ship.is(class::EQ_OBJ));
        assert!(ObjectKind::Solar.is(class::EQ_OBJ));
        assert!(!ObjectKind::Loot.is(class::EQ_OBJ));
        assert!(ObjectKind::Beam.is(class::SIMPLE));
        assert!(ObjectKind::Guided.is(class::PROJECTILE));
        assert!(ObjectKind::Mine.is(class::PROJECTILE));
        assert!(!ObjectKind::EquipmentObject.is(class::SIMPLE));
        assert!(ObjectKind::Asteroid.is(class::SIMPLE));
    }
}
