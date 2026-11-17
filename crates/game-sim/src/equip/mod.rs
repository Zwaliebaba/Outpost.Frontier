//! Equipment (work package P2.4): `CEquip` and every `CE*` kind of
//! `CppSource/Common/common_cequip.cpp` and `common_ce*.cpp`, and the
//! equipment manager (`CEquipManager`, `CEquipTraverser`).
//!
//! Equipment is a component of equipped objects (decision record 0001,
//! point 6): an [`EquipManager`] holds two ordered lists of [`Equip`]
//! values, keyed by their 16-bit sub-object id. The C++ class tree
//! (`CEquip` → `CInternalEquip` / `CExternalEquip` → `CAttachedEquip` →
//! `CELauncher` → `CEGun`, …) becomes one struct with the shared fields
//! and an [`EquipKind`] enum with the per-kind state; the virtual calls
//! become `match` arms.
//!
//! * [`flight`]: the force controllers (`CEEngine`, `CEThruster`,
//!   `CSteering`, `CNudgeEngine`, `CStrafeEngine`) and the power plant
//!   (`CEPower`). The controllers are functions of plain state and a
//!   [`flight::Pusher`], so the replay test runs them from recorded
//!   inputs.
//! * [`launcher`]: launchers, guns and the mine and counter-measure
//!   droppers (`CELauncher`, `CEGun`, `CEMineDropper`,
//!   `CECounterMeasureDropper`).
//! * [`shield`]: the shield and its generators (`CEShield`,
//!   `CEShieldGenerator`).
//! * [`misc`]: the remaining kinds: cargo, cargo pods, armor, scanner,
//!   tractor, cloak, lights, effects, repair droids.
//!
//! The archetype values each kind reads are copied from
//! [`crate::data::archetype`] when the equipment is made; archetypes do not
//! change after loading, so this reads what the C++ reads through its
//! archetype pointer.

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true exactly
// when the reference takes that side, which needs negated comparisons
// (as `neuron-core::physics` does).
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod flight;
pub mod launcher;
pub mod misc;
pub mod shield;

use neuron_core::math::{Matrix, Vector};
use neuron_core::scene::InstanceId;

use crate::data::archetype::equipment::{Equipment, EquipmentKind};

pub use flight::{
    EngineArch, EngineState, Nudge, Pusher, ShipFlightArch, Steering, Strafe, StrafeDir,
};

/// A sub-object id (`SubObjectID`): 1 the root, 2 the power, 3 to 0x20 the
/// collision groups, from 0x21 the equipment, from 0xFFF0 the shields.
pub type SubObjId = u16;

/// `SUBOBJ_ID_NONE` (`common_misc.cpp:431`).
pub const SUBOBJ_ID_NONE: SubObjId = 0;
/// `SUBOBJ_ID_ROOT`.
pub const SUBOBJ_ID_ROOT: SubObjId = 1;
/// `SUBOBJ_ID_POWER`.
pub const SUBOBJ_ID_POWER: SubObjId = 2;

/// The equipment type bits (`EEquipType`, `include/common_cequip.h:87`).
pub mod types {
    /// `CELightEquip`.
    pub const LIGHT: u32 = 1;
    /// `CEAttachedFX`.
    pub const ATTACHED_FX: u32 = 2;
    /// `CEMineDropper`.
    pub const MINE_DROPPER: u32 = 0x20;
    /// `CECounterMeasureDropper`.
    pub const CM_DROPPER: u32 = 0x40;
    /// `CEGun`.
    pub const GUN: u32 = 0x80;
    /// `CEShield`.
    pub const SHIELD: u32 = 0x100;
    /// `CEShieldGenerator`.
    pub const SHIELD_GEN: u32 = 0x200;
    /// `CEThruster`.
    pub const THRUSTER: u32 = 0x400;
    /// `CECargoPod`.
    pub const CARGO_POD: u32 = 0x800;
    /// `CECloakingDevice`.
    pub const CLOAK: u32 = 0x1000;
    /// `CECargo`.
    pub const CARGO: u32 = 0x1_0000;
    /// `CEEngine`.
    pub const ENGINE: u32 = 0x2_0000;
    /// `CEPower`.
    pub const POWER: u32 = 0x4_0000;
    /// `CEScanner`.
    pub const SCANNER: u32 = 0x8_0000;
    /// `CETractor`.
    pub const TRACTOR: u32 = 0x10_0000;
    /// `CERepairDroid`.
    pub const REPAIR_DROID: u32 = 0x20_0000;
    /// `CEInternalFX`.
    pub const INTERNAL_FX: u32 = 0x40_0000;
    /// `CETradeLaneEquip`.
    pub const TRADE_LANE: u32 = 0x80_0000;
    /// `CEArmor`.
    pub const ARMOR: u32 = 0x100_0000;

    /// The types `CEquipManager::AddNewEquipment` files in its second list
    /// (`kInternalListMask`, `common_cequip.cpp:38`).
    pub const INTERNAL_LIST: u32 = 0x00C1_0003;
    /// The types the first list holds (`kExternalListMask`).
    pub const EXTERNAL_LIST: u32 = 0xFF3E_FFFC;
    /// `CExternalEquip::cast` (`kExternalEquipMask`).
    pub const EXTERNAL: u32 = 0x1FE3;
    /// `CInternalEquip::cast` (`kInternalEquipMask`).
    pub const INTERNAL: u32 = 0x1FF_0000;
    /// `CAttachedEquip::cast` (`kAttachedEquipMask`).
    pub const ATTACHED: u32 = 0x1FE0;
    /// The launchers (`CELauncher::cast`).
    pub const LAUNCHER: u32 = GUN | MINE_DROPPER | CM_DROPPER;
    /// Every type (`CEquipTraverser()`).
    pub const ALL: u32 = 0xFFFF_FFFF;
}

/// The archetype classes `CEqObj::alloc_equip` and `CShip::alloc_equip`
/// switch on (`Archetype::AClassType`; the section numbers of
/// [`crate::data::archetype::equipment::SECTIONS`]).
pub mod classes {
    /// `CargoPod`.
    pub const CARGO_POD: u32 = 9;
    /// `Commodity`.
    pub const COMMODITY: u32 = 0xA;
    /// `Power`.
    pub const POWER: u32 = 0xB;
    /// `Engine`.
    pub const ENGINE: u32 = 0xC;
    /// `Shield`.
    pub const SHIELD: u32 = 0xD;
    /// `ShieldGenerator`.
    pub const SHIELD_GENERATOR: u32 = 0xE;
    /// `Thruster`.
    pub const THRUSTER: u32 = 0xF;
    /// `Gun`.
    pub const GUN: u32 = 0x11;
    /// `MineDropper`.
    pub const MINE_DROPPER: u32 = 0x12;
    /// `CounterMeasureDropper`.
    pub const CM_DROPPER: u32 = 0x13;
    /// `Scanner`.
    pub const SCANNER: u32 = 0x14;
    /// `Light`.
    pub const LIGHT: u32 = 0x15;
    /// `Tractor`.
    pub const TRACTOR: u32 = 0x16;
    /// `AttachedFX`.
    pub const ATTACHED_FX: u32 = 0x17;
    /// `InternalFX`.
    pub const INTERNAL_FX: u32 = 0x18;
    /// `RepairDroid`.
    pub const REPAIR_DROID: u32 = 0x19;
    /// `CloakingDevice`.
    pub const CLOAK: u32 = 0x1C;
    /// `Armor`.
    pub const ARMOR: u32 = 0x22;
}

/// An event an equipment item tells its listeners (`INotify::Event`,
/// `include/common_cequip.h:158`), queued on the owner for the server and
/// client sides to read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EquipEvent {
    /// 0xa: `CEquip::Destroy`.
    Destroying,
    /// 0xb: `Activate(true)` on a 0 → 1 edge.
    Activating,
    /// 0xc: `Activate(false)` on a 1 → 0 edge.
    Deactivating,
    /// 0xd: an exhaust nozzle's collision group was destroyed.
    NozzleLost(SubObjId),
    /// 0xe: the engine's cruise state changed (the code: 0 normal
    /// disengage, 1 instant disengage, 2 charging, 3 charged).
    CruiseStateChanged(i32),
    /// 0xf: a thruster could not draw its power.
    ThrusterStarved,
    /// 0x10: the shield's dominant generator archetype changed.
    ShieldGeneratorsChanged,
    /// 0x11: the shield's offline flag changed.
    ShieldOfflineChanged(bool),
    /// 0x12: the shield dropped to its offline threshold.
    ShieldWentOffline,
    /// 0x13: the shield can come back.
    ShieldCanReactivate,
}

/// Where an external item is mounted: its hardpoint and the engine
/// instance that carries it (`CExternalEquip::HardpointName`,
/// `ConnectionIndex`; -1 is `None`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Mount {
    /// The hardpoint name.
    pub hardpoint: Vec<u8>,
    /// The hardpoint was found (`ConnectionIndex` is not -1).
    pub connected: bool,
    /// The instance the hardpoint was found on.
    pub instance: Option<InstanceId>,
    /// The hardpoint's position in that instance's space.
    pub position: Vector,
    /// Its orientation.
    pub orientation: Matrix,
}

/// How an attached item hangs on its owner (`CAttachmentObj`,
/// `common_cattachment.cpp`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Attachment {
    /// `CNonPhysAttachment`: the item's model is a child instance of the
    /// owner's tree, without a body of its own; the owner's part list
    /// names the hardpoint.
    NonPhysical {
        /// The item's root instance (`EngineInstanceId`, -1 is `None`).
        instance: Option<InstanceId>,
        /// The hardpoint's name CRC.
        hp_crc: u32,
        /// Whether its part takes part in collisions.
        collisions: bool,
    },
    /// `CPhysAttachment`: a `CEquipmentObj` world object with a body merged
    /// into the owner's core.
    Physical {
        /// The equipment object.
        object: crate::object::ObjHandle,
        /// Its root instance.
        instance: Option<InstanceId>,
    },
}

impl Attachment {
    /// `GetRootIndex`: the item's root instance.
    #[must_use]
    pub fn root_instance(&self) -> Option<InstanceId> {
        match *self {
            Self::NonPhysical { instance, .. } | Self::Physical { instance, .. } => instance,
        }
    }
}

/// `DamageEntry::SubObjFate` (`include/common_misc.h:278`), and the
/// attached equipment's fate values 3 to 5 the server uses.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Fate {
    /// 0: stays (no fate).
    #[default]
    None,
    /// 1: destroyed with its parent.
    Destroyed,
    /// 2: separated as debris.
    Separated,
    /// 3: the health proxy.
    HealthProxy,
    /// 4: dropped as loot with an event.
    Loot,
    /// 5: dropped as loot.
    LootSilent,
}

impl Fate {
    /// The wire and record value.
    #[must_use]
    pub fn value(self) -> i32 {
        match self {
            Self::None => 0,
            Self::Destroyed => 1,
            Self::Separated => 2,
            Self::HealthProxy => 3,
            Self::Loot => 4,
            Self::LootSilent => 5,
        }
    }

    /// The fate of a record value (unknown values read as `None`).
    #[must_use]
    pub fn from_value(value: i32) -> Self {
        match value {
            1 => Self::Destroyed,
            2 => Self::Separated,
            3 => Self::HealthProxy,
            4 => Self::Loot,
            5 => Self::LootSilent,
            _ => Self::None,
        }
    }
}

/// The state of an attached item (`CAttachedEquip`, 0x4c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct AttachedState {
    /// +0x28 how it is attached; `None` before `Connect`.
    pub attachment: Option<Attachment>,
    /// +0x2c the debris object a separation left, not yet retrieved.
    pub debris: Option<crate::object::ObjHandle>,
    /// +0x30 the hit points (the archetype's at construction).
    pub hit_pts: f32,
    /// +0x34 the fate (1 at construction).
    pub fate: Fate,
    /// +0x38..+0x4b the bounding sphere cache.
    pub bounds: Option<(f32, Vector)>,
    /// `CNonPhysAttachment::CachedShieldFlag` (+0x0d): the part's shield
    /// flag kept while its collisions are off.
    pub cached_shield_flag: bool,
}

/// One equipment item: the shared `CEquip` fields and the kind's state.
#[derive(Clone, Debug, PartialEq)]
pub struct Equip {
    /// +0x08 the sub-object id.
    pub id: SubObjId,
    /// The archetype's nickname id.
    pub arch_id: u32,
    /// +0x0c the archetype's common fields (hit points, mass, nickname).
    pub arch: crate::data::archetype::root::Root,
    /// `lootable` of the archetype.
    pub lootable: bool,
    /// +0x10 `bActive`.
    pub active: bool,
    /// +0x11 `bDestroyed`.
    pub destroyed: bool,
    /// +0x12 `bMission`.
    pub mission: bool,
    /// +0x14 the type bits.
    pub type_bits: u32,
    /// The mount of an external item (`None` for internal equipment).
    pub mount: Option<Mount>,
    /// The attached state of a `CAttachedEquip`.
    pub attached: Option<AttachedState>,
    /// The per-kind state.
    pub kind: EquipKind,
    /// Events for the listeners (`NotifyA`, `NotifyB`), oldest first.
    pub events: Vec<EquipEvent>,
}

/// The per-kind state of an item.
#[derive(Clone, Debug, PartialEq)]
pub enum EquipKind {
    /// `CEEngine`.
    Engine(flight::EngineState),
    /// `CEThruster`.
    Thruster(flight::Thruster),
    /// `CEPower`.
    Power(flight::PowerPlant),
    /// `CEGun`.
    Gun(launcher::Gun),
    /// `CEMineDropper`.
    MineDropper(launcher::Launcher),
    /// `CECounterMeasureDropper`.
    CmDropper(launcher::Launcher),
    /// `CEShield`.
    Shield(shield::Shield),
    /// `CEShieldGenerator`.
    ShieldGenerator(shield::ShieldGenerator),
    /// `CECargo`.
    Cargo(misc::Cargo),
    /// `CECargoPod`.
    CargoPod(misc::CargoPod),
    /// `CEArmor`.
    Armor(misc::Armor),
    /// `CEScanner`.
    Scanner(misc::Scanner),
    /// `CETractor`.
    Tractor(misc::Tractor),
    /// `CECloakingDevice`.
    Cloak(misc::Cloak),
    /// `CELightEquip`.
    Light(misc::Light),
    /// `CEAttachedFX`.
    AttachedFx,
    /// `CEInternalFX`.
    InternalFx,
    /// `CERepairDroid`.
    RepairDroid(misc::RepairDroid),
}

impl Equip {
    /// `CEquip::CEquip`: inactive, not destroyed.
    #[must_use]
    pub fn new(
        id: SubObjId,
        arch: &Equipment,
        type_bits: u32,
        kind: EquipKind,
        mission: bool,
    ) -> Self {
        let external = type_bits & types::EXTERNAL != 0;
        let attached = (type_bits & types::ATTACHED != 0).then_some(AttachedState {
            attachment: None,
            debris: None,
            hit_pts: arch.root.hit_pts,
            fate: Fate::Destroyed,
            bounds: None,
            cached_shield_flag: false,
        });
        Self {
            id,
            arch_id: arch.root.nickname,
            arch: arch.root,
            lootable: arch.lootable,
            active: false,
            destroyed: false,
            mission,
            type_bits,
            mount: external.then(Mount::default),
            attached,
            kind,
            events: Vec::new(),
        }
    }

    /// `CEqObj::alloc_equip` and `CShip::alloc_equip`: the item an
    /// archetype makes, or `None` for a class that cannot be equipped
    /// (the reference's warning). `ship` selects the ship-only kinds
    /// (engine, thruster, tractor). The constructors' own activations
    /// (engine, power plant) are applied; registering controllers is the
    /// world's (`ControlObject` needs the owner's body).
    #[must_use]
    pub fn alloc(
        id: SubObjId,
        arch: &Equipment,
        mission: bool,
        ship: bool,
        projectile: Option<&Equipment>,
    ) -> Option<Self> {
        let class = arch.class();
        let (type_bits, kind) = match (&arch.kind, class) {
            (EquipmentKind::Engine { .. }, _) if ship => (
                types::ENGINE,
                EquipKind::Engine(flight::EngineState::new(flight::EngineArch::of(arch)?)),
            ),
            (EquipmentKind::Thruster { .. }, _) if ship => (
                types::THRUSTER,
                EquipKind::Thruster(flight::Thruster::of(arch)?),
            ),
            (
                EquipmentKind::Tractor {
                    max_length,
                    reach_speed,
                },
                _,
            ) if ship => (
                types::TRACTOR,
                EquipKind::Tractor(misc::Tractor::new(*max_length, *reach_speed)),
            ),
            (EquipmentKind::CargoPod(_), _) => (
                types::CARGO_POD,
                EquipKind::CargoPod(misc::CargoPod::default()),
            ),
            (EquipmentKind::Power { .. }, _) => (
                types::POWER,
                EquipKind::Power(flight::PowerPlant::of(arch)?),
            ),
            (EquipmentKind::ShieldGenerator(g), _) => (
                types::SHIELD_GEN,
                EquipKind::ShieldGenerator(shield::ShieldGenerator::new(g)),
            ),
            (EquipmentKind::Gun(g), _) => (
                types::GUN,
                EquipKind::Gun(launcher::Gun::new(g, projectile)),
            ),
            (EquipmentKind::MineDropper(l), _) => (
                types::MINE_DROPPER,
                EquipKind::MineDropper(launcher::Launcher::new(l, projectile)),
            ),
            (EquipmentKind::CounterMeasureDropper { launcher, .. }, _) => (
                types::CM_DROPPER,
                EquipKind::CmDropper(launcher::Launcher::new(launcher, projectile)),
            ),
            (
                EquipmentKind::Scanner {
                    power_usage,
                    range,
                    cargo_scan_range,
                },
                _,
            ) => (
                types::SCANNER,
                EquipKind::Scanner(misc::Scanner::new(*power_usage, *range, *cargo_scan_range)),
            ),
            (
                EquipmentKind::Light {
                    always_on,
                    docking_light,
                },
                _,
            ) => (
                types::LIGHT,
                EquipKind::Light(misc::Light {
                    always_on: *always_on,
                    docking_light: *docking_light,
                    on: false,
                }),
            ),
            (EquipmentKind::AttachedFx, _) => (types::ATTACHED_FX, EquipKind::AttachedFx),
            (EquipmentKind::InternalFx { .. }, _) => (types::INTERNAL_FX, EquipKind::InternalFx),
            (EquipmentKind::RepairDroid { repair_rate }, _) => (
                types::REPAIR_DROID,
                EquipKind::RepairDroid(misc::RepairDroid {
                    repair_rate: *repair_rate,
                }),
            ),
            (
                EquipmentKind::CloakingDevice {
                    power_usage,
                    cloakin_time,
                    cloakout_time,
                    ..
                },
                _,
            ) => (
                types::CLOAK,
                EquipKind::Cloak(misc::Cloak::new(
                    *power_usage,
                    *cloakin_time,
                    *cloakout_time,
                )),
            ),
            (EquipmentKind::Armor { hit_pts_scale }, _) => (
                types::ARMOR,
                EquipKind::Armor(misc::Armor {
                    hit_pts_scale: *hit_pts_scale,
                }),
            ),
            _ => return None,
        };
        let mut equip = Self::new(id, arch, type_bits, kind, mission);
        match equip.kind {
            // `CEEngine::CEEngine` and `CEPower::CEPower` call
            // `CEquip::Activate(true)`.
            EquipKind::Engine(_) | EquipKind::Power(_) => {
                equip.activate_base(true);
            }
            _ => {}
        }
        Some(equip)
    }

    /// `CECargo`: a cargo stack (`CEqObj::add_cargo_item`), not active.
    #[must_use]
    pub fn cargo(id: SubObjId, arch: &Equipment, mission: bool, health: f32, count: u32) -> Self {
        let decay = match arch.kind {
            EquipmentKind::Commodity {
                decay_per_second, ..
            } => decay_per_second,
            _ => 0.0,
        };
        let mut cargo = misc::Cargo::new(decay);
        cargo.init(health, count, arch.root.hit_pts);
        Self::new(id, arch, types::CARGO, EquipKind::Cargo(cargo), mission)
    }

    /// `CEquip::Notify`.
    pub fn notify(&mut self, event: EquipEvent) {
        self.events.push(event);
    }

    /// `IsActive`.
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.active
    }

    /// `IsConnected`: an external item has a mount instance; an attached
    /// one also an attachment with a root instance.
    #[must_use]
    pub fn is_connected(&self) -> bool {
        let Some(mount) = &self.mount else {
            return false;
        };
        if !mount.connected {
            return false;
        }
        match &self.attached {
            Some(a) => a.attachment.is_some(),
            None => true,
        }
    }

    /// `IsDestroyed`: an unconnected external item reads as destroyed
    /// (`CExternalEquip::IsDestroyed`); a cargo stack is destroyed when
    /// empty (`CECargo::IsDestroyed`).
    #[must_use]
    pub fn is_destroyed(&self) -> bool {
        if let EquipKind::Cargo(cargo) = &self.kind {
            return cargo.is_destroyed(self.destroyed);
        }
        if self.mount.is_some() && !self.is_connected() {
            return true;
        }
        self.destroyed
    }

    /// `IsDisabled`: launchers of a weapons-offline owner with no health
    /// left (`CELauncher::IsDisabled`); everything else is never disabled.
    #[must_use]
    pub fn is_disabled(&self, weapons_offline: bool) -> bool {
        if self.type_bits & types::LAUNCHER != 0 && weapons_offline {
            return self.relative_health() <= 0.0;
        }
        false
    }

    /// `IsFunctioning`: active, not disabled, not destroyed; the engine
    /// also needs its controller and a nozzle or an indestructible
    /// archetype (`CEEngine::IsFunctioning`).
    #[must_use]
    pub fn is_functioning(&self, weapons_offline: bool) -> bool {
        let base = self.is_active() && !self.is_disabled(weapons_offline) && !self.is_destroyed();
        match &self.kind {
            EquipKind::Engine(engine) => {
                base && engine.controller_enabled && engine.has_nozzle_or_indestructible()
            }
            _ => base,
        }
    }

    /// `CEquip::Activate`: the edge events, then the flag.
    pub fn activate_base(&mut self, on: bool) -> bool {
        if on {
            if !self.active {
                self.notify(EquipEvent::Activating);
            }
            self.active = true;
        } else {
            if self.active {
                self.notify(EquipEvent::Deactivating);
            }
            self.active = false;
        }
        self.active
    }

    /// `GetMaxHitPoints`: the archetype's.
    #[must_use]
    pub fn max_hit_points(&self) -> f32 {
        match &self.kind {
            EquipKind::Cargo(c) => c.max_hit_points(self.arch.hit_pts),
            _ => self.arch.hit_pts,
        }
    }

    /// `GetHitPoints`: the attached item's own; cargo its stack's; any
    /// other item its maximum (`CEquip::GetHitPoints` tail-calls
    /// `GetMaxHitPoints`).
    #[must_use]
    pub fn hit_points(&self) -> f32 {
        if let Some(a) = &self.attached {
            return a.hit_pts;
        }
        match &self.kind {
            EquipKind::Cargo(c) => c.hit_pts,
            EquipKind::Shield(s) => s.hit_pts,
            _ => self.max_hit_points(),
        }
    }

    /// `SetHitPoints`: ignored by internal items without hit points.
    pub fn set_hit_points(&mut self, hit: f32) {
        if let Some(a) = &mut self.attached {
            a.hit_pts = hit;
            return;
        }
        match &mut self.kind {
            EquipKind::Cargo(c) => c.hit_pts = hit,
            EquipKind::Shield(s) => s.hit_pts = hit,
            _ => {}
        }
    }

    /// `GetRelativeHealth`: hit points over the maximum, unrounded; 0 for
    /// a maximum of exactly 0.
    #[must_use]
    pub fn relative_health(&self) -> f64 {
        let max = self.max_hit_points();
        if max == 0.0 {
            return 0.0;
        }
        f64::from(self.hit_points()) / f64::from(max)
    }

    /// `CEquip::Destroy` (and `CExternalEquip::Destroy`'s disconnect).
    pub fn destroy(&mut self) {
        self.destroyed = true;
        self.notify(EquipEvent::Destroying);
        if let Some(mount) = &mut self.mount {
            mount.connected = false;
        }
    }

    /// `CEquip::IsTemporary`.
    #[must_use]
    pub fn is_temporary(&self) -> bool {
        self.mission
    }

    /// `IsInternal`: in the second list (`CEquipManager::AddNewEquipment`).
    #[must_use]
    pub fn in_internal_list(&self) -> bool {
        self.type_bits & types::INTERNAL_LIST != 0
    }
}

/// `CEquipManager`: the two equipment lists of an equipped object.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EquipManager {
    /// +0x08 the first list (`External`): every type outside
    /// [`types::INTERNAL_LIST`].
    pub external: Vec<Equip>,
    /// +0x14 the second list (`Internal`): lights, attached and internal
    /// effects, cargo, the trade-lane item.
    pub internal: Vec<Equip>,
    /// +0x1c the decaying cargo stacks.
    pub decaying_cargo: i32,
}

/// Which list and index an item is at.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct EquipSlot {
    /// In the second list.
    pub internal: bool,
    /// The index in it.
    pub index: usize,
}

impl EquipManager {
    /// `AddNewEquipment`.
    pub fn add(&mut self, equip: Equip) -> EquipSlot {
        if let EquipKind::Cargo(c) = &equip.kind
            && c.decay_per_second != 0.0
        {
            self.decaying_cargo += 1;
        }
        if equip.in_internal_list() {
            self.internal.push(equip);
            EquipSlot {
                internal: true,
                index: self.internal.len() - 1,
            }
        } else {
            self.external.push(equip);
            EquipSlot {
                internal: false,
                index: self.external.len() - 1,
            }
        }
    }

    /// `Clear`.
    pub fn clear(&mut self) {
        self.external.clear();
        self.internal.clear();
        self.decaying_cargo = 0;
    }

    /// `Size`.
    #[must_use]
    pub fn len(&self) -> usize {
        self.external.len() + self.internal.len()
    }

    /// Whether there is no equipment.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The item at a slot.
    #[must_use]
    pub fn get(&self, slot: EquipSlot) -> Option<&Equip> {
        if slot.internal {
            self.internal.get(slot.index)
        } else {
            self.external.get(slot.index)
        }
    }

    /// The item at a slot, mutably.
    pub fn get_mut(&mut self, slot: EquipSlot) -> Option<&mut Equip> {
        if slot.internal {
            self.internal.get_mut(slot.index)
        } else {
            self.external.get_mut(slot.index)
        }
    }

    /// `Traverse` with a fresh `CEquipTraverser(mask, include_destroyed)`:
    /// the first list when the mask meets [`types::EXTERNAL_LIST`], then
    /// the second when it meets [`types::INTERNAL_LIST`], each in order,
    /// items of the mask only, destroyed ones only when asked.
    #[must_use]
    pub fn traverse(&self, mask: u32, include_destroyed: bool) -> Vec<EquipSlot> {
        let mut out = Vec::new();
        if self.external.is_empty() && self.internal.is_empty() {
            return out;
        }
        let pass = |list: &[Equip], internal: bool, out: &mut Vec<EquipSlot>| {
            for (index, equip) in list.iter().enumerate() {
                if mask & equip.type_bits == 0 {
                    continue;
                }
                if !include_destroyed && equip.is_destroyed() {
                    continue;
                }
                out.push(EquipSlot { internal, index });
            }
        };
        if !self.external.is_empty() && mask & types::EXTERNAL_LIST != 0 {
            pass(&self.external, false, &mut out);
        }
        if !self.internal.is_empty() && mask & types::INTERNAL_LIST != 0 {
            pass(&self.internal, true, &mut out);
        }
        out
    }

    /// `FindFirst(mask)`: the first item `traverse` gives.
    #[must_use]
    pub fn find_first(&self, mask: u32) -> Option<EquipSlot> {
        self.traverse(mask, false).first().copied()
    }

    /// The first item of a type, read only.
    #[must_use]
    pub fn first(&self, mask: u32) -> Option<&Equip> {
        self.find_first(mask).and_then(|s| self.get(s))
    }

    /// `FindByID`: the first list, then the second, destroyed items too.
    #[must_use]
    pub fn find_by_id(&self, id: SubObjId) -> Option<EquipSlot> {
        if let Some(index) = self.external.iter().position(|e| e.id == id) {
            return Some(EquipSlot {
                internal: false,
                index,
            });
        }
        self.internal
            .iter()
            .position(|e| e.id == id)
            .map(|index| EquipSlot {
                internal: true,
                index,
            })
    }

    /// The item with an id.
    #[must_use]
    pub fn by_id(&self, id: SubObjId) -> Option<&Equip> {
        self.find_by_id(id).and_then(|s| self.get(s))
    }

    /// The item with an id, mutably.
    pub fn by_id_mut(&mut self, id: SubObjId) -> Option<&mut Equip> {
        let slot = self.find_by_id(id)?;
        self.get_mut(slot)
    }

    /// `FindByHardpoint`: the first external item mounted on `hardpoint`
    /// (compared as the reference compares cache strings, by content).
    #[must_use]
    pub fn find_by_hardpoint(&self, hardpoint: &[u8]) -> Option<EquipSlot> {
        self.traverse(types::EXTERNAL, true)
            .into_iter()
            .find(|&slot| {
                self.get(slot)
                    .and_then(|e| e.mount.as_ref())
                    .is_some_and(|m| neuron_core::hash::eq_ignore_case(&m.hardpoint, hardpoint))
            })
    }

    /// `InstToSubObjID`: the attached item whose root instance is
    /// `instance`, 0 for none.
    #[must_use]
    pub fn inst_to_sub_obj_id(&self, instance: Option<InstanceId>) -> SubObjId {
        let Some(instance) = instance else {
            return 0;
        };
        for slot in self.traverse(types::ATTACHED, false) {
            if let Some(e) = self.get(slot)
                && e.attached
                    .as_ref()
                    .and_then(|a| a.attachment)
                    .and_then(|a| a.root_instance())
                    == Some(instance)
            {
                return e.id;
            }
        }
        0
    }

    /// `CleanUp`: removes every item `CanDelete` allows (destroyed, and
    /// not on a solar), the second list first; returns how many went.
    pub fn clean_up(&mut self, owner_is_solar: bool) -> usize {
        if owner_is_solar {
            return 0;
        }
        let mut removed = 0;
        for list in [&mut self.internal, &mut self.external] {
            let before = list.len();
            let mut decaying = 0;
            list.retain(|e| {
                let gone = e.is_destroyed();
                if gone && matches!(&e.kind, EquipKind::Cargo(c) if c.decay_per_second != 0.0) {
                    decaying += 1;
                }
                !gone
            });
            self.decaying_cargo -= decaying;
            removed += before - list.len();
        }
        removed
    }

    /// Every item, the first list then the second.
    pub fn iter(&self) -> impl Iterator<Item = &Equip> {
        self.external.iter().chain(self.internal.iter())
    }

    /// Every item, mutably.
    pub fn iter_mut(&mut self) -> impl Iterator<Item = &mut Equip> {
        self.external.iter_mut().chain(self.internal.iter_mut())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::archetype::equipment::Equipment;

    fn arch(class: u32, nickname: u32) -> Equipment {
        let mut e = Equipment::new(class);
        e.root.nickname = nickname;
        e.root.hit_pts = 50.0;
        e
    }

    #[test]
    fn lists_follow_the_type_masks() {
        let mut m = EquipManager::default();
        let power = Equip::alloc(0x22, &arch(classes::POWER, 1), false, true, None).unwrap();
        let light = Equip::alloc(0x23, &arch(classes::LIGHT, 2), false, true, None).unwrap();
        let engine = Equip::alloc(0x24, &arch(classes::ENGINE, 3), false, true, None).unwrap();
        assert_eq!(
            m.add(power),
            EquipSlot {
                internal: false,
                index: 0
            }
        );
        assert_eq!(
            m.add(light),
            EquipSlot {
                internal: true,
                index: 0
            }
        );
        m.add(engine);
        // The update traverser (0xff3efffc) never reaches the second list.
        let update: Vec<_> = m.traverse(types::EXTERNAL_LIST, false);
        assert_eq!(update.len(), 2);
        // The light is unconnected, so it reads as destroyed.
        assert!(m.find_first(types::LIGHT).is_none());
        assert!(m.traverse(types::LIGHT, true).len() == 1);
        assert_eq!(m.by_id(0x24).map(|e| e.type_bits), Some(types::ENGINE));
        assert!(m.by_id(0x24).unwrap().is_active());
        assert!(m.by_id(0x22).unwrap().is_active());
    }

    #[test]
    fn engine_is_ship_only() {
        assert!(Equip::alloc(0x22, &arch(classes::ENGINE, 3), false, false, None).is_none());
        assert!(Equip::alloc(0x22, &arch(classes::COMMODITY, 3), false, true, None).is_none());
    }

    #[test]
    fn activate_reports_edges() {
        let mut e = Equip::alloc(0x22, &arch(classes::SCANNER, 1), false, true, None).unwrap();
        e.activate_base(true);
        e.activate_base(true);
        e.activate_base(false);
        assert_eq!(
            e.events,
            vec![EquipEvent::Activating, EquipEvent::Deactivating]
        );
    }

    #[test]
    fn relative_health_of_zero_max_is_zero() {
        let mut a = arch(classes::GUN, 1);
        a.root.hit_pts = 0.0;
        let e = Equip::alloc(0x22, &a, false, true, None).unwrap();
        assert_eq!(e.relative_health(), 0.0);
    }
}
