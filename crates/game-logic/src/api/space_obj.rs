//! `pub::SpaceObj` (`Server/server_pub.cpp:2264-4196`, `Server\space\
//! pubSpace.cpp`): Content's view of the objects in space, by object id,
//! on the world ([`game_sim::object::World`]) and the server's state of
//! its objects ([`crate::objects`]), with the C++ result codes.
//!
//! The C++ finds an object four ways, and each function names its own:
//! `Server_LookupGameObject` (the id's object, destroyed or not),
//! `Server_LookupLiveObject` (not destroyed, [`crate::dock::lookup_live`]),
//! `Server_FindObject` (destroyed or not, with its system: none while in
//! transit) and `Server_FindLiveObject` ([`crate::transfer::find_live`]).
//! Most functions first refuse an id whose low 30 bits are 0 (-4); the
//! comment of each says when it does not. Where the C++ writes an
//! out-parameter before a failure (zeros, mostly), the port answers the
//! failure alone; where it leaves an out-parameter untouched on success,
//! the port answers `None`.
//!
//! The 56 exports, in the C++ order:
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `Create` | 2456 | [`create`] |
//! | `CreateLoot` | 2528 | [`create_loot`] |
//! | `Destroy` | 2587 | [`destroy`] |
//! | `DrainShields` | 2627 | [`drain_shields`] |
//! | `EnumerateCargo` | 2643 | [`enumerate_cargo`] |
//! | `FormationResponse` | 2680 | [`formation_response`] |
//! | `LaneResponse` | 2694 | [`lane_response`] |
//! | `DockRequest` | 2710 | [`dock_request`] |
//! | `Dock` | 2749 | [`dock`] |
//! | `InstantDock` | 2836 | [`instant_dock`] |
//! | `Launch` | 2904 | [`launch`], without `CShip::launch` (its `LaunchAction` is not ported; D704) |
//! | `JumpIn` | 2949 | [`jump_in`] |
//! | `Relocate` | 2993 | [`relocate`] |
//! | `SendComm` | 3050 | [`send_comm`] |
//! | `JettisonEquipment` | 3099 | [`jettison_equipment`] |
//! | `GetArchetypeID` | 3114 | [`archetype_id`] |
//! | `GetAtmosphereRange` | 3126 | [`atmosphere_range`] |
//! | `GetScannerRange` | 3148 | [`scanner_range`] |
//! | `GetType` | 3174 | [`object_type`] |
//! | `Activate` | 3197 | [`activate`] |
//! | `ActivateEquipment` | 3229 | [`activate_equipment`] |
//! | `SetInvincible` | 3296 | [`set_invincible`] |
//! | `AddImpulse` | 3325 | [`add_impulse`] |
//! | `GetLocation` | 3342 | [`location`] |
//! | `GetSystem` | 3366 | [`system`] |
//! | `GetHealth` | 3382 | [`health`] |
//! | `GetRelativeHealth` | 3396 | [`relative_health`] |
//! | `ExistsAndAlive` | 3409 | [`exists_and_alive`] |
//! | `GetTarget` | 3420 | [`target`] |
//! | `CreateSolar` | 3467 | [`create_solar`] |
//! | `GetMotion` | 3513 | [`motion`] |
//! | `GetRep` | 3532 | [`rep`] |
//! | `GetSolarRep` | 3546 | [`solar_rep`] |
//! | `GetSolarArchetypeID` | 3557 | [`solar_archetype_id`] |
//! | `GetSolarArchetypeNickname` | 3574 | [`solar_archetype_nickname`] |
//! | `GetSolarParent` | 3595 | [`solar_parent`] |
//! | `GetHardpoint` | 3620 | [`hardpoint`] |
//! | `GetRadius` | 3634 | [`radius`] |
//! | `GetBurnRadius` | 3648 | [`burn_radius`] |
//! | `GetMass` | 3664 | [`mass`] |
//! | `GetCenterOfMass` | 3675 | [`center_of_mass`] |
//! | `GetVoiceID` | 3698 | [`voice_id`] |
//! | `GetInvincible` | 3713 | [`invincible`] |
//! | `GetCargoSpaceOccupied` | 3737 | [`cargo_space_occupied`] |
//! | `GetDockingTarget` | 3754 | [`docking_target`] |
//! | `SetRelativeHealth` | 3784 | [`set_relative_health`] |
//! | `GetShieldHealth` | 3796 | [`shield_health`] |
//! | `GetTradelaneNextAndPrev` | 3809 | [`tradelane_next_and_prev`] |
//! | `LightFuse` | 3832 | [`light_fuse`] |
//! | `GetTerminalInfo` | 3857 | [`terminal_info`] |
//! | `GetGoodID` | 3876 | [`good_id`] |
//! | `GetJumpTarget` | 3894 | [`jump_target`] |
//! | `SetInvincible2` | 3919 | [`set_invincible2`] |
//! | `RequestSpaceScript` | 3939 | [`request_space_script`] |
//! | `GetEmptyPos` | 3999 | [`empty_pos`] |
//! | `IsPosEmpty` | 4141 | [`is_pos_empty`] |
//!
//! The transport loadout of `Create` is `SpaceObj_BuildTransportLoadout`
//! (`Server\SrvMisc.cpp`, [`build_transport_loadout`]); the dock functions
//! work on [`crate::dock`], the transfers (`JumpIn`, `Relocate`, `Launch`)
//! on [`crate::world`]'s `remove_ship`, `add_ship` and `arrive_ship`. The
//! `StarSystem` methods that only these functions call (`create_solar`,
//! `activate_object`, `launch`, `Server_DestroyInTransit`) are here too.

use std::collections::{BTreeMap, BTreeSet};

use game_sim::data::archetype::equipment::EquipmentKind;
use game_sim::data::costume::Costume;
use game_sim::data::equip_desc::EquipDesc;
use game_sim::equip::{EquipKind, SUBOBJ_ID_NONE, types};
use game_sim::object::damage::{self, DamageList};
use game_sim::object::world::{ActivateResult, LootSpawn, ScanResult, ScannedEquip, SolarSpawn};
use game_sim::object::{KindData, ObjHandle, ObjectKind, class};
use game_sim::protocol::ToClient;
use game_sim::protocol::to_client::RequestSpaceScript;
use neuron_core::hash::c_str;
use neuron_core::id::sub_object::EquipmentIds;
use neuron_core::id::{IdString, create_id};
use neuron_core::math::{
    Matrix, Transform, Vector, matrix_to_quaternion, quaternion_to_matrix, transform_xform,
};
use neuron_core::num::{trunc_to_i32, trunc_to_i32_via_i64};
use neuron_server::controller::ControllerId;

use super::PubError;
use super::player::CargoLine;
use crate::events::{ControllerMessage, ObjectDestroyed};
use crate::objects::Invincibility;
use crate::replication::{Comm, EqObjCreateInfo, SolarCreateInfo};
use crate::server::Cx;
use crate::ship::ShipCreateRequest;

const SRV_MISC: &str = "Server\\SrvMisc.cpp";
const STAR_SYS: &str = "Server\\space\\StarSys.cpp";
const CSOLAR: &str = "Common\\CSolar.cpp";

/// The cause of `DrainShields`'s damage list (no `DamageCause` names it).
const DRAIN_SHIELDS_CAUSE: game_sim::object::damage::DamageCause = 0x17;

/// `rand() * 2^-15`'s factor, as a double (`GetEmptyPos`).
const RAND_STEP: f64 = 3.051_757_812_5e-05;

/// The float `2 pi` `GetEmptyPos` scales its angle by.
const TWO_PI: f64 = 6.283_185_482_025_146_5;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// An id whose low 30 bits are 0, which most functions refuse.
fn no_id(id: u32) -> bool {
    id & 0x3fff_ffff == 0
}

/// `Server_LookupGameObject`: the id's object, destroyed or not.
fn lookup(cx: &Cx<'_>, id: u32) -> Option<ObjHandle> {
    cx.world.find(id)
}

/// `Server_FindObject`: the id's object, destroyed or not, and its system
/// (`None` while it is in transit).
fn find_object(cx: &Cx<'_>, id: u32) -> Option<(ObjHandle, Option<u32>)> {
    let h = cx.world.find(id)?;
    let system = (!cx.world.in_transit(h)).then(|| cx.world.get(h).map(|o| o.physical.system));
    Some((h, system.flatten()))
}

/// The narrowing to `CSimple` (`& 3`).
fn is_simple(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world.get(h).is_some_and(|o| o.is(class::SIMPLE))
}

/// The narrowing to `CEqObj` (`& 0x103`): a ship or solar with equipment.
fn is_eq_obj(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world
        .get(h)
        .is_some_and(|o| o.is(class::EQ_OBJ) && o.equipped.is_some())
}

/// The narrowing to `CShip` (`& 0x503`).
fn is_ship(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.world
        .get(h)
        .is_some_and(|o| o.kind == ObjectKind::Ship && o.equipped.is_some())
}

/// `IObjInspect::get_atmosphere_range`: a solar's range; `None` (the
/// out-parameter untouched) for anything else.
fn atmosphere_of(cx: &Cx<'_>, h: ObjHandle) -> Option<f32> {
    match &cx.world.get(h)?.data {
        KindData::Solar(s) => Some(s.atmosphere_range),
        _ => None,
    }
}

/// `get_loot_appearance`: a commodity's loot appearance, else the item.
fn loot_appearance(cx: &Cx<'_>, contents: u32) -> u32 {
    match cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&contents)
        .map(|a| &a.kind)
    {
        Some(EquipmentKind::Commodity {
            loot_appearance: Some(id),
            ..
        }) if cx.world.data().archetypes.equipment.contains_key(id) => *id,
        _ => contents,
    }
}

/// `HpTypeCargoPod`: the hardpoint type a transport's cargo pods hang on.
pub const HP_TYPE_CARGO_POD: u32 = 0x0e;

/// One cargo line of a ship to create (`CargoDesc`): a good and how many.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CargoDesc {
    /// `GoodId`.
    pub good: u32,
    /// `Count`.
    pub count: u32,
}

/// `pub::SpaceObj::ShipInfo`: what Content asks a ship to be made from.
#[derive(Clone, Debug, PartialEq)]
pub struct ShipInfo {
    /// `ControllerId`: the controller that owns the ship; it must exist.
    pub controller: u32,
    /// `SystemID`.
    pub system: u32,
    /// `ShipArchID`.
    pub archetype: u32,
    /// `Position`.
    pub position: Vector,
    /// `AngularVelocity`.
    pub angular_velocity: Vector,
    /// `Velocity`.
    pub velocity: Vector,
    /// `Orientation`.
    pub orientation: Matrix,
    /// +0x54: the ship's group name (`GroupNameId`).
    pub group_name_id: u32,
    /// `LoadoutID` (0 none).
    pub loadout: u32,
    /// `Cargo`: a transport's cargo.
    pub cargo: Vec<CargoDesc>,
    /// The costume (head, body, hands, accessories).
    pub costume: Costume,
    /// `VibeGroupId`.
    pub vibe: i32,
    /// `VoiceId`.
    pub voice: u32,
    /// `DockingBaseId`.
    pub docking_base: u32,
    /// `HitPts`, an integer (-1 for the archetype's).
    pub hit_pts: i32,
    /// +0xb8: the player who marks the ship (0 none).
    pub mark_player: u32,
    /// `ReputationRank`.
    pub rank: u32,
}

impl ShipInfo {
    /// A ship of `archetype` with `loadout` at `position`, for
    /// `controller`, the rest empty: no cargo, costume, group, voice, base
    /// or mark, full health, at rest.
    #[must_use]
    pub fn new(
        controller: u32,
        system: u32,
        archetype: u32,
        loadout: u32,
        position: Vector,
        orientation: Matrix,
    ) -> Self {
        Self {
            controller,
            system,
            archetype,
            position,
            angular_velocity: Vector::ZERO,
            velocity: Vector::ZERO,
            orientation,
            group_name_id: 0,
            loadout,
            cargo: Vec::new(),
            costume: Costume::default(),
            vibe: -1,
            voice: 0,
            docking_base: 0,
            hit_pts: -1,
            mark_player: 0,
            rank: 0,
        }
    }
}

/// The two file-static trees of `SpaceObj.cpp`: the ships `Create` made and
/// the controller each was made for (`Destroy` erases both).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SpaceObjState {
    /// The ids of the ships `Create` made.
    pub created_ships: BTreeSet<u32>,
    /// Each such ship's controller.
    pub ship_controllers: BTreeMap<u32, u32>,
}

/// `SpaceObj_BuildTransportLoadout`: a ship with cargo-pod hardpoints gets,
/// for each of its first two cargo lines, a pod (the good's
/// `pod_appearance`) and a share of the count (the count over the number
/// of hardpoints, the last one taking the rest) on every hardpoint; a line
/// whose good has no pod appearance is reported and ends the loadout.
/// Without cargo-pod hardpoints every line goes into the hold. Each item
/// takes the next id of `ids`. A good or equipment that does not exist
/// ends the loadout too (the C++ reads through the null; D620).
pub fn build_transport_loadout(
    cx: &mut Cx<'_>,
    archetype: u32,
    ids: &mut EquipmentIds,
    cargo: &[CargoDesc],
    out: &mut Vec<EquipDesc>,
) {
    let data = cx.world.data_arc();
    let hps: Vec<_> = data
        .archetypes
        .ships
        .get(&archetype)
        .and_then(|a| a.legal_hps(HP_TYPE_CARGO_POD))
        .map(<[_]>::to_vec)
        .unwrap_or_default();
    let blank = EquipDesc::new(cx.world.strings());
    if hps.is_empty() {
        for line in cargo {
            let mut desc = blank;
            desc.id = ids.create();
            desc.arch_id = data.goods.good_to_arch(line.good);
            desc.count = line.count as i32;
            desc.health = 1.0;
            desc.make_internal(cx.world.strings());
            desc.equipped = 0;
            out.push(desc);
        }
        return;
    }
    let hp_count = hps.len() as i32;
    for (index, line) in cargo.iter().enumerate() {
        let Some(arch_id) = data.goods.find_by_id(line.good).map(|g| g.arch_id) else {
            return;
        };
        let Some(equipment) = data.archetypes.equipment.get(&arch_id) else {
            return;
        };
        let pod = match &equipment.kind {
            EquipmentKind::Commodity { pod_appearance, .. } => *pod_appearance,
            _ => None,
        };
        let Some(pod) = pod else {
            cx.logic.reports.push(
                SRV_MISC,
                0x3a,
                format!(
                    "*** WARNING: Equipment({}) has no pod_appearance.  Transport loadout aborted.",
                    IdString(equipment.root.nickname).to_hex_string()
                ),
            );
            return;
        };
        // `ftol(count / hardpoints)`: both are exact in a double and the
        // quotient truncates, as the integer division does.
        let per_hp = (line.count / hp_count as u32) as i32;
        let mut left = line.count as i32;
        for hp in &hps {
            let mut pod_desc = blank;
            pod_desc.id = ids.create();
            pod_desc.arch_id = pod;
            pod_desc.count = 1;
            pod_desc.health = 1.0;
            pod_desc.hardpoint = *hp;
            pod_desc.equipped = 1;
            out.push(pod_desc);
            let mut goods = blank;
            goods.id = ids.create();
            goods.arch_id = arch_id;
            goods.health = 1.0;
            goods.hardpoint = *hp;
            goods.equipped = 0;
            goods.count = per_hp;
            out.push(goods);
            left = left.wrapping_sub(per_hp);
        }
        if let Some(last) = out.last_mut() {
            last.count = left.wrapping_add(per_hp);
        }
        // `++Index > 1`: two lines at most.
        if index >= 1 {
            return;
        }
    }
}

/// `pub::SpaceObj::Create`: a ship made for Content in a system
/// ([`crate::ship::create_ship`]) from the info: its orientation through
/// a quaternion (`MatrixToQuaternion`, then the ship takes the matrix of
/// it), its loadout's equipment id maker continued by the transport
/// loadout ([`build_transport_loadout`]), a new object id. The ship and
/// its controller are recorded. Returns the ship's id.
///
/// # Errors
///
/// -1 for a system id whose low 30 bits are 0 or that has no entry, and
/// for an unknown ship archetype or loadout or a ship that cannot be made
/// (an unloaded system), where the C++ reads through a null (D619); -4
/// when the controller does not exist.
pub fn create(cx: &mut Cx<'_>, info: &ShipInfo) -> Result<u32, PubError> {
    crate::world::initialize(cx);
    if info.system & 0x3fff_ffff == 0 || !cx.logic.server_world.systems.contains_key(&info.system) {
        return Err(PubError::FAILED);
    }
    if !cx
        .world
        .data()
        .archetypes
        .ships
        .contains_key(&info.archetype)
    {
        return Err(PubError::FAILED);
    }
    let mut ids = EquipmentIds::default();
    if info.loadout != 0 {
        let Some(loadout) = cx.world.data().loadouts.get(info.loadout) else {
            return Err(PubError::FAILED);
        };
        ids = loadout.id_maker.clone();
    }
    let mut equipment = Vec::new();
    build_transport_loadout(cx, info.archetype, &mut ids, &info.cargo, &mut equipment);
    if !cx.logic.bus.exists(ControllerId(info.controller)) {
        return Err(PubError::NO_CONTROLLER);
    }
    let id = cx.world.allocate_id();
    let request = ShipCreateRequest {
        id,
        archetype: info.archetype,
        loadout: info.loadout,
        position: info.position,
        orientation: matrix_to_quaternion(&info.orientation),
        velocity: info.velocity,
        angular_velocity: info.angular_velocity,
        owner_player: 0,
        hit_pts: info.hit_pts,
        equipment,
        vibe: info.vibe,
        costume: info.costume.clone(),
        voice: info.voice,
        docking_base: info.docking_base,
        group_name_id: info.group_name_id,
        rank: info.rank,
        using_tradelane: false,
        mark_player: info.mark_player,
        equip_ids: ids,
        equip_state_data: 0,
    };
    if crate::ship::create_ship(cx, info.system, &request).is_none() || id & 0x3fff_ffff == 0 {
        return Err(PubError::FAILED);
    }
    cx.logic.space_obj.created_ships.insert(id);
    cx.logic
        .space_obj
        .ship_controllers
        .insert(id, info.controller);
    Ok(id)
}

/// `pub::SpaceObj::LootInfo`: what Content asks a loot container to be
/// made from.
#[derive(Clone, Debug, PartialEq)]
pub struct LootInfo {
    /// `SystemID`.
    pub system: u32,
    /// `EquipArchID`: the contents.
    pub contents: u32,
    /// `NameId`: the container's name.
    pub name_id: u32,
    /// `OwnerObjId`: the source object (`create_loot`'s source).
    pub owner: u32,
    /// `Velocity`.
    pub velocity: Vector,
    /// `Position`.
    pub position: Vector,
    /// `Orientation`.
    pub orientation: Matrix,
    /// `AngularVelocity`.
    pub angular_velocity: Vector,
    /// `Units`.
    pub units: u32,
    /// `HitPtsScale`: times the loot appearance's hit points.
    pub hit_pts_scale: f32,
    /// `bCanAiTractor`.
    pub can_ai_tractor: bool,
    /// `bMission`: temporary loot.
    pub mission: bool,
    /// `MarkForPlayer` (0 none).
    pub mark_player: u32,
}

impl LootInfo {
    /// `units` of `contents` at `position` in `system`, from no object, at
    /// rest, full health, tractorable by the AI, unmarked.
    #[must_use]
    pub fn new(system: u32, contents: u32, units: u32, position: Vector) -> Self {
        Self {
            system,
            contents,
            name_id: 0,
            owner: 0,
            velocity: Vector::ZERO,
            position,
            orientation: Matrix::IDENTITY,
            angular_velocity: Vector::ZERO,
            units,
            hit_pts_scale: 1.0,
            can_ai_tractor: true,
            mission: false,
            mark_player: 0,
        }
    }
}

/// `pub::SpaceObj::CreateLoot`: one container (`StarSystem::create_loot`
/// in mode 4 with the info's owner as the source,
/// [`crate::objects::create_loot_marked`]): a new object id; the contents'
/// archetype with its hit points and the units; the contents' loot
/// appearance as the crate with its hit points times the scale (a float);
/// the name, the motion, the pose, the AI tractor and mission flags and
/// the player to mark it for. `LootSpawned` reaches the bus and every
/// player of the system gets the create. Returns the container's id. A
/// system that is not loaded makes no container (as D619).
///
/// # Errors
///
/// -4 for a system id whose low 30 bits are 0 or that has no entry, and
/// for contents that are no equipment, which the C++ reads through null
/// (D700; the id is allocated first, as in the C++).
pub fn create_loot(cx: &mut Cx<'_>, info: &LootInfo) -> Result<u32, PubError> {
    let Some(system) = crate::transfer::lookup_system(cx, info.system) else {
        return Err(PubError::BAD_ARGUMENT);
    };
    let id = cx.world.allocate_id();
    let data = cx.world.data_arc();
    let Some(contents) = data.archetypes.equipment.get(&info.contents) else {
        return Err(PubError::BAD_ARGUMENT);
    };
    let appearance = loot_appearance(cx, info.contents);
    let appearance_hit_pts = data
        .archetypes
        .equipment
        .get(&appearance)
        .map_or(0.0, |a| a.root.hit_pts);
    let spawn = LootSpawn {
        archetype: appearance,
        contents: info.contents,
        units: info.units,
        contents_hit_pts: contents.root.hit_pts,
        owner: info.owner,
        position: info.position,
        velocity: info.velocity,
        id: Some(id),
        hit_pts: r(e(appearance_hit_pts) * e(info.hit_pts_scale)),
        orientation: info.orientation,
        angular_velocity: info.angular_velocity,
        name_id: info.name_id as i32,
        temporary: info.mission,
        can_ai_tractor: info.can_ai_tractor,
    };
    crate::objects::create_loot_marked(cx, system, info.owner, 4, &[spawn], &[info.mark_player]);
    Ok(id)
}

/// `pub::SpaceObj::Destroy(Object, Type)`: an object in a system is
/// destroyed (`destroy(Type, 0)`, [`crate::world::destroy_object`]); one
/// in transit is deleted at once (`Server_DestroyInTransit`:
/// `ObjectDestroyed` to the bus with only the id, then the object goes).
/// Either way `Create`'s records of the id are erased.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the object is not found
/// or destroyed, and after deleting an object in transit, as the C++
/// answers.
pub fn destroy(cx: &mut Cx<'_>, object: u32, kind: i32) -> Result<(), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let Some((h, system)) = crate::transfer::find_live(cx, object) else {
        return Err(PubError::BAD_ID);
    };
    let result = if system.is_some() {
        crate::world::destroy_object(cx, h, kind, 0);
        Ok(())
    } else {
        destroy_in_transit(cx, h);
        Err(PubError::BAD_ID)
    };
    cx.logic.space_obj.created_ships.remove(&object);
    cx.logic.space_obj.ship_controllers.remove(&object);
    result
}

/// `Server_DestroyInTransit`: an object in transit leaves the transit list
/// and the lookup cache, `ObjectDestroyed` reaches the bus with its id and
/// everything else 0, and the object is deleted. False for an object that
/// is not in transit.
fn destroy_in_transit(cx: &mut Cx<'_>, h: ObjHandle) -> bool {
    if !cx.world.in_transit(h) {
        return false;
    }
    let id = cx.world.get(h).map_or(0, |o| o.simple.id);
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::ObjectDestroyed(ObjectDestroyed {
            id,
            killer: 0,
            system: 0,
            owner_player: 0,
            kind: 0,
            position: Vector::ZERO,
            dropped_equip: Vec::new(),
        }),
    );
    cx.world.delete_in_transit(h)
}

/// `pub::SpaceObj::DrainShields`: every shield of an equipped object
/// (`EqObj::drain_shields`: `damage_shield` with -1, which empties it) in
/// a damage list of cause 0x17 from nobody, applied to the object
/// (`apply_damage_list` with the server's hooks).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the object is not found
/// or destroyed; -1 for an object without equipment.
pub fn drain_shields(cx: &mut Cx<'_>, object: u32) -> Result<(), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = crate::dock::lookup_live(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_eq_obj(cx, h) {
        return Err(PubError::FAILED);
    }
    let shields: Vec<u16> = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|eq| {
            eq.equipment
                .traverse(types::SHIELD, false)
                .into_iter()
                .filter_map(|slot| eq.equipment.get(slot).map(|s| s.id))
                .collect()
        })
        .unwrap_or_default();
    let mut list = DamageList::new(DRAIN_SHIELDS_CAUSE, 0, 0);
    crate::fuses::compute(cx, h, &mut list, |t, _, _, list| {
        for id in shields {
            damage::damage_shield(t, id, -1.0, list);
        }
    });
    let mut hooks = crate::world::Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
    Ok(())
}

/// `pub::SpaceObj::EnumerateCargo`: the cargo stacks of an equipped object
/// (`CEquipTraverser(0x10000)`, not destroyed), each whose description
/// reads and whose archetype has a good, with the good, its count and its
/// id; an object without equipment has none. The C++ calls the enumerator
/// for each; the port answers the lines (D701).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (`Server_LookupGameObject`, destroyed or not).
pub fn enumerate_cargo(cx: &mut Cx<'_>, object: u32) -> Result<Vec<CargoLine>, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_eq_obj(cx, h) {
        return Ok(Vec::new());
    }
    let ids: Vec<u16> = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|eq| {
            eq.equipment
                .traverse(types::CARGO, false)
                .into_iter()
                .filter_map(|slot| eq.equipment.get(slot).map(|s| s.id))
                .collect()
        })
        .unwrap_or_default();
    let mut out = Vec::new();
    for id in ids {
        let Some(desc) = cx.world.equip_desc(h, id) else {
            continue;
        };
        let good = cx.world.data().goods.arch_to_good(desc.arch_id);
        if good != 0 {
            out.push(CargoLine {
                good,
                count: desc.count,
                id: desc.id,
            });
        }
    }
    Ok(out)
}

/// `pub::SpaceObj::FormationResponse`: Content's answer to a ship's
/// formation request (`Ship::request_returned(1, Response, 0)`,
/// [`crate::dock::request_returned`]). An object that is not a ship, which
/// the C++ casts unchecked, hears nothing (D702).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the object is not found
/// or destroyed.
pub fn formation_response(cx: &mut Cx<'_>, ship: u32, response: i32) -> Result<(), PubError> {
    request_response(cx, ship, 1, response)
}

/// `pub::SpaceObj::LaneResponse`: the same for a trade-lane request
/// (`Ship::request_returned(2, Param, 0)`; D702).
///
/// # Errors
///
/// As [`formation_response`].
pub fn lane_response(cx: &mut Cx<'_>, ship: u32, param: i32) -> Result<(), PubError> {
    request_response(cx, ship, 2, param)
}

fn request_response(cx: &mut Cx<'_>, ship: u32, kind: i32, response: i32) -> Result<(), PubError> {
    if no_id(ship) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = crate::dock::lookup_live(cx, ship).ok_or(PubError::BAD_ID)?;
    if is_ship(cx, h) {
        crate::dock::request_returned(cx, h, kind, response, 0);
    }
    Ok(())
}

/// `pub::SpaceObj::DockRequest(Ship, Target)`: the object's request of a
/// dock without a callback (`slot_5c(0, Target, 0, 1)`; the owner's client
/// hears the answer). 0 for an object that is not found or destroyed, and
/// the request's answer otherwise (-3 for an object that is not a ship).
pub fn dock_request(cx: &mut Cx<'_>, ship: u32, target: u32) -> i32 {
    let Some(h) = crate::dock::lookup_live(cx, ship) else {
        return 0;
    };
    crate::dock::request(cx, h, 0, target, 0, 1, false)
}

/// `pub::SpaceObj::Dock(Ship, Target, Port, Response)`, Content's answer
/// to a dock request. For a live ship: a ship whose request is in progress
/// gets the answer (`request_returned(0, Response, Port)`, [`crate::dock::
/// request_returned`]); its docking lights go on for responses 3 and 4 and
/// off otherwise (`ActivateEquipment` with type 3). A waiting answer (3)
/// ends there. Otherwise a live host queues the ship at `port`, or with a
/// negative port drops the ship's request (any port). The behaviour event
/// 5 to the ship's followers comes with the behaviour manager's server
/// link.
///
/// # Errors
///
/// -4 for a ship id whose low 30 bits are 0; -2 for a waiting answer to a
/// ship that is not found, or that has no request in progress; -3 when the
/// host is not found. A host without equipment, which the C++ casts
/// unchecked, queues nothing (D625).
pub fn dock(
    cx: &mut Cx<'_>,
    ship: u32,
    target: u32,
    port: i32,
    response: i32,
) -> Result<(), PubError> {
    if ship & 0x3fff_ffff == 0 {
        return Err(PubError(-4));
    }
    let mut result = Err(PubError::BAD_ID);
    if let Some(h) = crate::dock::lookup_live(cx, ship) {
        let busy = crate::ship::ship_state(cx, h).is_some_and(|s| s.request_busy);
        if busy {
            crate::dock::request_returned(cx, h, 0, response, port);
            result = Ok(());
        }
        if let Some(lit) = cx.world.find(ship) {
            crate::dock::activate_docking_lights(
                cx,
                lit,
                response == crate::dock::RESPONSE_PROCEED || response == crate::dock::RESPONSE_WAIT,
            );
        }
    }
    if response == crate::dock::RESPONSE_WAIT {
        return result;
    }
    let Some(host) = crate::dock::lookup_live(cx, target) else {
        return Err(PubError::NOT_FOUND);
    };
    if cx.world.get(host).and_then(|o| o.as_equipped()).is_none() {
        return Ok(());
    }
    if port >= 0 {
        crate::dock::queue_dock_request(cx, host, port, ship);
    } else {
        crate::dock::dequeue_dock_request(cx, host, port, ship);
    }
    Ok(())
}

/// `pub::SpaceObj::InstantDock(Ship, Target, Port)`: the ship docks at
/// once, without the approach. A host whose type bits are a subset of
/// `0xc40` (a gate, a jump hole, an airlock) completes the dock in its
/// system ([`crate::dock::complete_dock`]); any other joins its system's
/// landing queue with the host's docking-point base, landing unless the
/// port's docking sphere is a mooring point. A port without a sphere,
/// which the C++ reads past the archetype's array, lands (D623).
///
/// # Errors
///
/// -4 for a ship or target id whose low 30 bits are 0; otherwise always
/// -2, as the C++ answers, including when the ship or a live equipped host
/// is not found.
pub fn instant_dock(cx: &mut Cx<'_>, ship: u32, target: u32, port: i32) -> Result<(), PubError> {
    if ship & 0x3fff_ffff == 0 || target & 0x3fff_ffff == 0 {
        return Err(PubError(-4));
    }
    let Some(ship_h) = crate::dock::lookup_live(cx, ship) else {
        return Err(PubError::BAD_ID);
    };
    let Some(host) = crate::dock::lookup_live(cx, target)
        .filter(|&h| cx.world.get(h).and_then(|o| o.as_equipped()).is_some())
    else {
        return Err(PubError::BAD_ID);
    };
    let Some(o) = cx.world.get(host) else {
        return Err(PubError::BAD_ID);
    };
    let system = o.physical.system;
    let bits = o.simple.type_bits;
    let eq = o.as_equipped();
    let dock_type = eq
        .and_then(|e| {
            usize::try_from(port)
                .ok()
                .and_then(|i| e.docking_points.get(i))
        })
        .map_or(0, |d| d.dock_type);
    let base = eq.map_or(0, |e| e.docking_point_base);
    if bits == bits & 0xc40 {
        crate::dock::complete_dock(cx, system, ship_h, host);
        return Err(PubError::BAD_ID);
    }
    let land = !matches!(dock_type, 3..=5);
    crate::world::queue_dock(cx, system, Some(ship_h), Some(host), base, land);
    Err(PubError::BAD_ID)
}

/// `pub::SpaceObj::Launch(Ship, Dock, Type)`: a ship launches from a dock
/// object. A ship in another system than the dock leaves its system
/// ([`crate::world::remove_ship`]) and enters the dock's at the origin
/// with the identity ([`crate::world::add_ship`]); then the dock's system
/// launches it (`StarSystem::launch`, [`launch_from`]). A ship in transit
/// only enters, and a dock in transit launches nothing, where the C++
/// calls the null system (D703).
///
/// # Errors
///
/// -4 for a ship id whose low 30 bits are 0; -2 when the ship is not found
/// or destroyed; -3 when the dock is not found or destroyed.
pub fn launch(cx: &mut Cx<'_>, ship: u32, dock: u32, launch_type: i32) -> Result<(), PubError> {
    if no_id(ship) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let Some((h, own)) = crate::transfer::find_live(cx, ship) else {
        return Err(PubError::BAD_ID);
    };
    let Some((dock_h, dock_system)) = crate::transfer::find_live(cx, dock) else {
        return Err(PubError::NOT_FOUND);
    };
    let Some(dock_system) = dock_system else {
        return Ok(());
    };
    if own != Some(dock_system) {
        if own.is_some() {
            let _ = crate::world::remove_ship(cx, h);
        }
        if !crate::world::add_ship(cx, dock_system, h, Vector::ZERO, Matrix::IDENTITY) {
            // A system that is not loaded keeps the ship in transit (D685).
            return Ok(());
        }
    }
    launch_from(cx, h, dock_h, launch_type);
    Ok(())
}

/// `StarSystem::launch(Ship, Dock, Type)`: a player's ship is reported and
/// left; an NPC ship gets its launch invincibility
/// ([`crate::ship::apply_launch_invincibility`]). `CShip::launch`, which
/// starts the ship's `LaunchAction` out of the dock's port, is not ported
/// (D704); an object that is not a ship, which the C++ reads through the
/// null narrowing, does nothing (D703).
fn launch_from(cx: &mut Cx<'_>, ship: ObjHandle, dock: ObjHandle, launch_type: i32) {
    let _ = (dock, launch_type);
    if cx
        .world
        .get(ship)
        .is_some_and(|o| o.simple.owner_player != 0)
    {
        cx.logic.reports.push(
            STAR_SYS,
            0x9f2,
            "*** ERROR: StarSystem::launch() called on a player!",
        );
        return;
    }
    if !is_ship(cx, ship) {
        return;
    }
    crate::ship::apply_launch_invincibility(cx, ship);
}

/// `pub::SpaceObj::JumpIn(Ship, Gate)`: the ship arrives through the gate
/// (a jump gate, a jump hole or an airlock). A ship in another system
/// leaves it ([`crate::world::remove_ship`]); one in transit leaves
/// transit; the gate chooses the arrival pose
/// ([`crate::transfer::choose_system_arrival_location`]); a ship from
/// elsewhere enters the gate's system ([`crate::world::add_ship`]); then the
/// ship arrives there through the gate ([`crate::world::arrive_ship`]: an
/// NPC's jumps in, the players who see it get the switch-in). A ship
/// already in the gate's system only arrives.
///
/// # Errors
///
/// -4 for a ship id whose low 30 bits are 0; -2 when the ship is not found
/// or destroyed; -3 when the gate is not found or destroyed, and, where the
/// C++ calls `Solar`'s method on another object or adds the ship to the
/// null system of a gate in transit, for a gate that is not a solar or is
/// in transit (D684).
pub fn jump_in(cx: &mut Cx<'_>, ship: u32, gate: u32) -> Result<(), PubError> {
    if ship & 0x3fff_ffff == 0 {
        return Err(PubError(-4));
    }
    let Some((ship_h, ship_system)) = crate::transfer::find_live(cx, ship) else {
        return Err(PubError::BAD_ID);
    };
    let Some((gate_h, gate_system)) = crate::transfer::find_live(cx, gate) else {
        return Err(PubError::NOT_FOUND);
    };
    let solar = cx
        .world
        .get(gate_h)
        .is_some_and(|o| o.kind == game_sim::object::ObjectKind::Solar);
    let Some(gate_system) = gate_system.filter(|_| solar) else {
        return Err(PubError::NOT_FOUND);
    };
    // `Server_TakeInTransit` for a ship in transit: the world keeps its
    // parameters until it enters a system.
    if ship_system.is_some_and(|s| s != gate_system) {
        let _ = crate::world::remove_ship(cx, ship_h);
    }
    let (position, orientation) = crate::transfer::choose_system_arrival_location(cx, gate_h);
    if ship_system != Some(gate_system)
        && !crate::world::add_ship(cx, gate_system, ship_h, position, orientation)
    {
        // A system that is not loaded keeps the ship in transit (D685).
        return Ok(());
    }
    crate::world::arrive_ship(cx, gate_system, ship_h, Some(gate_h), position, orientation);
    Ok(())
}

/// `pub::SpaceObj::Relocate(Object, System, Pos, Orient)`: the object moves
/// to `position` and `orientation` in `system`, or into transit for a
/// system id whose low 30 bits are 0 or that has no entry. An object in
/// another system leaves it ([`crate::world::remove_ship`]); into transit
/// it stays there, keeping its parameters (`Server_PushInTransit`); in its
/// own system it is beamed to the pose (`beam_object`); otherwise it
/// enters the system ([`crate::world::add_ship`]) and a ship's control
/// exclusion is cleared. A system that is not loaded keeps the object in
/// transit (D685); an object that is not a ship leaves and enters its own
/// kind's list (D686).
///
/// # Errors
///
/// -4 for an object id whose low 30 bits are 0; -2 when the object is not
/// found or destroyed.
pub fn relocate(
    cx: &mut Cx<'_>,
    object: u32,
    system: u32,
    position: Vector,
    orientation: Matrix,
) -> Result<(), PubError> {
    if object & 0x3fff_ffff == 0 {
        return Err(PubError(-4));
    }
    let Some((h, own)) = crate::transfer::find_live(cx, object) else {
        return Err(PubError::BAD_ID);
    };
    let target = crate::transfer::lookup_system(cx, system);
    // `Server_TakeInTransit` for an object in transit.
    if own.is_some() && target != own {
        let _ = crate::world::remove_ship(cx, h);
    }
    let Some(target) = target else {
        return Ok(());
    };
    if own == Some(target) {
        cx.world.beam_object(h, position, orientation);
        return Ok(());
    }
    if !crate::world::add_ship(cx, target, h, position, orientation) {
        return Ok(());
    }
    if let Some(o) = cx
        .world
        .get_mut(h)
        .filter(|o| o.kind == game_sim::object::ObjectKind::Ship)
        && let Some(eq) = o.equipped.as_deref_mut()
    {
        eq.control_exclusion = 0;
    }
    Ok(())
}

/// What `pub::SpaceObj::SendComm` is given.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CommRequest {
    /// The speaker (0 none).
    pub sender: u32,
    /// The addressee (0 none).
    pub target: u32,
    /// The voice (0: the speaker's).
    pub voice: u32,
    /// The costume (`None`: the speaker's).
    pub costume: Option<Costume>,
    /// `CommType`.
    pub comm_type: u32,
    /// The text ids (`TextIds`, `TextCount`).
    pub text_ids: Vec<u32>,
    /// `Mood`.
    pub mood: u32,
    /// `Duration`.
    pub duration: f32,
    /// `bInterrupt`: the comm is broadcast (`StarSystem::send_comm`'s
    /// `bBroadcast`).
    pub interrupt: bool,
}

/// `pub::SpaceObj::SendComm`: the system is the addressee's
/// (`Server_FindObject`, destroyed or not; none in transit), else the
/// speaker's. A live speaker with equipment gives its voice when none is
/// given and its space costume when none is given, and the comm type
/// becomes 0. Then the system sends the comm (`StarSystem::send_comm`,
/// [`crate::replication::send_comm`]).
///
/// # Errors
///
/// -4 without text ids, or when neither the addressee nor the speaker (an
/// id whose low 30 bits are 0 for the speaker) is in a system; -2 without
/// a costume.
pub fn send_comm(cx: &mut Cx<'_>, request: &CommRequest) -> Result<(), PubError> {
    if request.text_ids.is_empty() {
        return Err(PubError::BAD_ARGUMENT);
    }
    let mut system = None;
    if !no_id(request.target) {
        system = find_object(cx, request.target).and_then(|(_, s)| s);
    }
    if system.is_none() {
        if no_id(request.sender) {
            return Err(PubError::BAD_ARGUMENT);
        }
        system = find_object(cx, request.sender).and_then(|(_, s)| s);
    }
    let Some(system) = system else {
        return Err(PubError::BAD_ARGUMENT);
    };
    let mut voice = request.voice;
    let mut costume = request.costume.clone();
    let mut comm_type = request.comm_type;
    if !no_id(request.sender)
        && let Some(h) = crate::dock::lookup_live(cx, request.sender)
        && let Some(eq) = cx.world.get(h).and_then(|o| o.equipped.as_deref())
        && is_eq_obj(cx, h)
    {
        if voice == 0 {
            voice = eq.voice;
        }
        if costume.is_none() {
            costume = Some(eq.space_costume.clone());
        }
        comm_type = 0;
    }
    let Some(costume) = costume else {
        return Err(PubError::BAD_ID);
    };
    let comm = Comm {
        sender: request.sender,
        receiver: request.target,
        voice,
        costume,
        comm_type,
        text_ids: request.text_ids.clone(),
        mood: request.mood,
        duration: request.duration,
    };
    crate::replication::send_comm(cx, system, &comm, request.interrupt);
    Ok(())
}

/// `pub::SpaceObj::JettisonEquipment`: a ship's jettison of `quantity` of
/// a cargo stack (`Ship::slot_38`, [`crate::cargo::jettison`]); any other
/// object refuses with -3 (`GameObject::slot_38`), which reads as success.
///
/// # Errors
///
/// -2 when no object has the id (no id mask test; destroyed objects are
/// found); -3 when the ship has no such stack (the jettison's -2).
pub fn jettison_equipment(
    cx: &mut Cx<'_>,
    object: u32,
    equip: u16,
    quantity: i32,
) -> Result<(), PubError> {
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_ship(cx, h) {
        return Ok(());
    }
    match crate::cargo::jettison(cx, h, equip, quantity, 0) {
        -2 => Err(PubError::NOT_FOUND),
        _ => Ok(()),
    }
}

/// `pub::SpaceObj::GetArchetypeID`: `Good2Arch` of a good.
///
/// # Errors
///
/// -2 when the good has no archetype (the C++ writes the 0).
pub fn archetype_id(cx: &Cx<'_>, good: u32) -> Result<u32, PubError> {
    match cx.world.data().goods.good_to_arch(good) {
        0 => Err(PubError::BAD_ID),
        arch => Ok(arch),
    }
}

/// `pub::SpaceObj::GetAtmosphereRange`: a solar's atmosphere range, less
/// its radius when it is positive; 0 for any other object.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not).
pub fn atmosphere_range(cx: &Cx<'_>, object: u32) -> Result<f32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    let Some(range) = atmosphere_of(cx, h) else {
        return Ok(0.0);
    };
    if range > 0.0 {
        let radius = cx.world.get(h).map_or(0.0, |o| o.engine.radius);
        return Ok(r(e(range) - e(radius)));
    }
    Ok(range)
}

/// `pub::SpaceObj::GetScannerRange`: a ship's first scanner's radar and
/// cargo scan ranges (`GetRadarRange`, `GetCargoScanRange`: the range
/// less the ship's scanner interference, a float), each truncated to an
/// integer.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not) or the ship has no working scanner; -1 for an object
/// that is not a ship; -3 for a destroyed scanner.
pub fn scanner_range(cx: &Cx<'_>, object: u32) -> Result<(i32, i32), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_ship(cx, h) {
        return Err(PubError::FAILED);
    }
    let Some(o) = cx.world.get(h) else {
        return Err(PubError::FAILED);
    };
    let Some(scanner) = o
        .equipped
        .as_deref()
        .and_then(|eq| eq.equipment.first(types::SCANNER))
    else {
        return Err(PubError::BAD_ID);
    };
    let EquipKind::Scanner(s) = &scanner.kind else {
        return Err(PubError::BAD_ID);
    };
    if scanner.is_destroyed() {
        return Err(PubError::NOT_FOUND);
    }
    let clear = 1.0 - e(o.simple.scanner_interference);
    let radar = r(clear * e(s.range));
    let cargo = r(clear * e(s.cargo_scan_range));
    Ok((trunc_to_i32(e(radar)), trunc_to_i32(e(cargo))))
}

/// `pub::SpaceObj::GetType`: the object's type bits (`get_type`; 0 for an
/// object that is not a `CSimple`).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not). The C++ writes 0 first.
pub fn object_type(cx: &Cx<'_>, object: u32) -> Result<u32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    Ok(type_bits(cx, h))
}

/// `IObjInspect::get_type`: the type bits of a `CSimple`, else 0.
fn type_bits(cx: &Cx<'_>, h: ObjHandle) -> u32 {
    cx.world
        .get(h)
        .filter(|o| o.is(class::SIMPLE))
        .map_or(0, |o| o.simple.type_bits)
}

/// `pub::SpaceObj::Activate(Object, Activate, Index)`: an airlock (type
/// bit 0x400) is never deactivated (0). Otherwise the object's system
/// activates the docking point (`StarSystem::activate_object`,
/// `CEqObj::activate`): a change tells every player of the system for a
/// solar, else those who can see the object
/// ([`crate::replication::object_activated`]). An object in transit, whose
/// null system the C++ calls, changes its point and tells nobody (D705).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the object is not found
/// or destroyed; -3 for an object without equipment or an index that is
/// not one of its docking points.
pub fn activate(cx: &mut Cx<'_>, object: u32, on: bool, index: i32) -> Result<(), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let Some((h, system)) = crate::transfer::find_live(cx, object) else {
        return Err(PubError::BAD_ID);
    };
    if !on && type_bits(cx, h) & 0x400 != 0 {
        return Ok(());
    }
    if activate_object(cx, system, h, on, u32::from_ne_bytes(index.to_ne_bytes())) {
        Ok(())
    } else {
        Err(PubError::NOT_FOUND)
    }
}

/// `StarSystem::activate_object(Obj, On, Index)`: false for an object
/// without equipment or when `CEqObj::activate` fails; a change is told to
/// the players ([`crate::replication::object_activated`]).
fn activate_object(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    h: ObjHandle,
    on: bool,
    index: u32,
) -> bool {
    if !is_eq_obj(cx, h) {
        return false;
    }
    let Ok(at) = usize::try_from(index) else {
        return false;
    };
    match cx.world.activate_docking_point(h, on, at) {
        ActivateResult::Failed => false,
        ActivateResult::NoChange => true,
        ActivateResult::Changed => {
            if let Some(system) = system {
                crate::replication::object_activated(cx, system, h, on, index);
            }
            true
        }
    }
}

/// `pub::SpaceObj::EQInfo`: what `ActivateEquipment` is asked.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EqInfo {
    /// `Type`: 0 cruise, 1 a cargo scan, 2 the cloak, 3 the docking
    /// lights.
    pub kind: u32,
    /// The low byte of `EquipId`: on (non-zero) or off.
    pub on: bool,
    /// `Count`: type 0 needs 1.
    pub count: u32,
    /// `Field_0x0c`: type 1's target.
    pub target: u32,
}

/// `pub::SpaceObj::ActivateEquipment` (`Server_FindObject`: destroyed
/// objects too):
/// - type 0, count 1: the ship engages cruise (`Ship::slot_50(true, true,
///   0)`, [`crate::equip_fire::engage_cruise`]); its refusal (any object
///   that is not a ship among them) is -5;
/// - type 1: the object's first scanner scans the live target's cargo
///   (`IObjInspect::scan_cargo`); the scanned items are the answer, and
///   any failure is -5;
/// - type 2: an equipped object's first cloak is switched (`EqObj::
///   slot_4c` from the server, [`crate::equip_fire::activate_equip`]);
/// - type 3: an equipped object's docking lights are switched
///   ([`crate::dock::activate_docking_lights`]; none in transit, where
///   `slot_4c` refuses).
///
/// The answer is empty but for type 1.
///
/// # Errors
///
/// -5 for an id whose low 30 bits are 0; -1 when no object has the id, for
/// a type above 3, a count other than 1 (type 0), no target (type 1) or an
/// object without equipment (types 2 and 3); -5 as above; -2 when there is
/// no cloak.
pub fn activate_equipment(
    cx: &mut Cx<'_>,
    object: u32,
    info: &EqInfo,
) -> Result<Vec<ScannedEquip>, PubError> {
    const BAD_TYPE: PubError = PubError(-5);
    if no_id(object) {
        return Err(BAD_TYPE);
    }
    let Some((h, system)) = find_object(cx, object) else {
        return Err(PubError::FAILED);
    };
    match info.kind {
        0 => {
            if info.count != 1 {
                return Err(PubError::FAILED);
            }
            if crate::equip_fire::engage_cruise(cx, h, true, true, 0) == 0 {
                Ok(Vec::new())
            } else {
                Err(BAD_TYPE)
            }
        }
        1 => {
            if info.target == 0 {
                return Err(PubError::FAILED);
            }
            let Some(other) = crate::dock::lookup_live(cx, info.target) else {
                return Err(BAD_TYPE);
            };
            scan_cargo(cx, h, other).ok_or(BAD_TYPE)
        }
        2 => {
            if !is_eq_obj(cx, h) {
                return Err(PubError::FAILED);
            }
            let Some(cloak) = cx
                .world
                .get(h)
                .and_then(|o| o.equipped.as_deref())
                .and_then(|eq| eq.equipment.first(types::CLOAK))
                .map(|c| c.id)
            else {
                return Err(PubError::BAD_ID);
            };
            crate::equip_fire::activate_equip(cx, h, cloak, info.on, 0);
            Ok(Vec::new())
        }
        3 => {
            if !is_eq_obj(cx, h) {
                return Err(PubError::FAILED);
            }
            // `slot_4c` refuses each light of an object in transit (-5).
            if system.is_some() {
                crate::dock::activate_docking_lights(cx, h, info.on);
            }
            Ok(Vec::new())
        }
        _ => Err(PubError::FAILED),
    }
}

/// `IObjInspect::scan_cargo(Target, Out)`: the object's first scanner
/// scans an equipped target (`CEScanner::ScanCargo`); `None` (-3) for an
/// object without equipment or a scanner, a target without equipment, or
/// a scan that does not complete.
fn scan_cargo(cx: &mut Cx<'_>, h: ObjHandle, target: ObjHandle) -> Option<Vec<ScannedEquip>> {
    if !is_eq_obj(cx, h) || !is_eq_obj(cx, target) {
        return None;
    }
    let scanner = cx
        .world
        .get(h)?
        .equipped
        .as_deref()?
        .equipment
        .first(types::SCANNER)?
        .id;
    match cx.world.scan_cargo(h, scanner, Some(target)) {
        (ScanResult::Complete, list) => Some(list),
        _ => None,
    }
}

/// The invincibility command of `SetInvincible` and `SetInvincible2`
/// (`GameObject::set_invincible`) on a live object in a system.
fn command_invincibility(
    cx: &mut Cx<'_>,
    object: u32,
    command: Invincibility,
) -> Result<(), PubError> {
    if object == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let Some((h, Some(_))) = crate::transfer::find_live(cx, object) else {
        return Err(PubError::BAD_ID);
    };
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.set_invincible(command);
    }
    Ok(())
}

/// `pub::SpaceObj::SetInvincible(Object, Invincible, Param3, MaxHpLoss)`:
/// an invincibility of type 3, protecting from everyone, from players
/// unless `param3`, down to `max_hp_loss`; or none of type 3 (unprotected,
/// all loss allowed).
///
/// # Errors
///
/// -4 for the id 0 (the whole id, no mask); -2 when the object is not
/// found, destroyed or in transit.
pub fn set_invincible(
    cx: &mut Cx<'_>,
    object: u32,
    invincible: bool,
    param3: bool,
    max_hp_loss: f32,
) -> Result<(), PubError> {
    let command = if invincible {
        Invincibility {
            kind: 3,
            protect: true,
            from_players: !param3,
            max_hp_loss,
        }
    } else {
        Invincibility {
            kind: 3,
            protect: false,
            from_players: false,
            max_hp_loss: 1.0,
        }
    };
    command_invincibility(cx, object, command)
}

/// `pub::SpaceObj::AddImpulse`: an impulse at a world point on the
/// object's body (`PhySys::AddImpulseAtPoint`); nothing for an object
/// that is not a `CSimple` or has no body.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the object is not found
/// or destroyed.
pub fn add_impulse(
    cx: &mut Cx<'_>,
    object: u32,
    impulse: Vector,
    point: Vector,
) -> Result<(), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = crate::dock::lookup_live(cx, object).ok_or(PubError::BAD_ID)?;
    if is_simple(cx, h) {
        cx.world.add_impulse_at_point(h, impulse, point);
    }
    Ok(())
}

/// `pub::SpaceObj::GetLocation`: the object's position and orientation.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not). The C++ zeroes the position first.
pub fn location(cx: &Cx<'_>, object: u32) -> Result<(Vector, Matrix), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    let o = cx.world.get(h).ok_or(PubError::BAD_ID)?;
    Ok((o.engine.position, o.engine.orientation))
}

/// `pub::SpaceObj::GetSystem`: the system the object is in; 0 in transit.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the object is not found
/// or destroyed. The C++ writes 0 first.
pub fn system(cx: &Cx<'_>, object: u32) -> Result<u32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let (_, system) = crate::transfer::find_live(cx, object).ok_or(PubError::BAD_ID)?;
    Ok(system.unwrap_or(0))
}

/// `pub::SpaceObj::GetHealth`: the hit points and the maximum
/// (`get_status`; 0 and 0 for an object that is not a `CSimple`).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not). The C++ zeroes both first.
pub fn health(cx: &Cx<'_>, object: u32) -> Result<(f32, f32), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    Ok(cx
        .world
        .get(h)
        .filter(|o| o.is(class::SIMPLE))
        .map_or((0.0, 0.0), |o| (o.simple.hit_pts, o.simple.max_hit_pts)))
}

/// `pub::SpaceObj::GetRelativeHealth` (`get_status(Health)`): an object of
/// the type bits 0x80 with equipment reads 0 when one of its shields is
/// not working and 1 otherwise (0 without shields); any other `CSimple`
/// its hit points over the maximum, stored as a float, negative as 0. An
/// object that is not a `CSimple`, or of type 0x80 without equipment,
/// which the C++ reads through null, reads 0 (D706).
///
/// # Errors
///
/// -4 for the id 0 (the whole id, no mask); -2 when no object has the id
/// (destroyed or not). The C++ writes 0 first.
pub fn relative_health(cx: &Cx<'_>, object: u32) -> Result<f32, PubError> {
    if object == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    let Some(o) = cx.world.get(h).filter(|o| o.is(class::SIMPLE)) else {
        return Ok(0.0);
    };
    if o.simple.type_bits == 0x80 {
        let Some(eq) = o.equipped.as_deref() else {
            return Ok(0.0);
        };
        let shields = eq.equipment.traverse(types::SHIELD, false);
        let mut health = 0.0;
        for slot in shields {
            let Some(shield) = eq.equipment.get(slot) else {
                continue;
            };
            if !shield.is_functioning(eq.weapons_offline) {
                return Ok(0.0);
            }
            health = 1.0;
        }
        return Ok(health);
    }
    let relative = o.simple.relative_health();
    Ok(if relative < 0.0 { 0.0 } else { r(relative) })
}

/// `pub::SpaceObj::ExistsAndAlive`.
///
/// # Errors
///
/// -2 for an id whose low 30 bits are 0, or when the object is not found
/// or destroyed.
pub fn exists_and_alive(cx: &Cx<'_>, object: u32) -> Result<(), PubError> {
    if no_id(object) || crate::dock::lookup_live(cx, object).is_none() {
        return Err(PubError::BAD_ID);
    }
    Ok(())
}

/// `pub::SpaceObj::GetTarget`: a ship's target's id (0 for none).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not); -1 for an object that is not a ship. The C++
/// writes 0 first.
pub fn target(cx: &Cx<'_>, object: u32) -> Result<u32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_ship(cx, h) {
        return Err(PubError::FAILED);
    }
    Ok(cx
        .world
        .get(h)
        .and_then(|o| o.as_ship())
        .and_then(|s| s.target)
        .and_then(|t| cx.world.get(t))
        .map_or(0, |t| t.simple.id))
}

/// `pub::SpaceObj::SolarInfo`: what Content asks a solar to be made from.
#[derive(Clone, Debug, PartialEq)]
pub struct SolarInfo {
    /// `SolarArchID`.
    pub archetype: u32,
    /// `SystemID`.
    pub system: u32,
    /// `Position`.
    pub position: Vector,
    /// `Orientation`.
    pub orientation: Matrix,
    /// `LoadoutID` (0 none).
    pub loadout: u32,
    /// The costume (head, body, left hand, right hand, the accessories).
    pub costume: Costume,
    /// `VibeGroupId`.
    pub vibe: i32,
    /// `VoiceId`.
    pub voice: u32,
    /// `DockingBaseId`: the base the solar is on and docks with.
    pub docking_base: u32,
    /// +0x80, the visit value.
    pub visit: u8,
    /// `HitPts`, an integer (-1 for the archetype's).
    pub hit_pts: i32,
    /// `Nickname`: the solar's id is `CreateID` of it.
    pub nickname: Vec<u8>,
    /// +0xcc, the player who marks the solar (0 none).
    pub mark_player: u32,
}

impl SolarInfo {
    /// A solar `nickname` of `archetype` at `position` in `system`, the
    /// rest empty: no loadout, costume, voice or base, no reputation (-1),
    /// full health, unmarked.
    #[must_use]
    pub fn new(system: u32, archetype: u32, nickname: &[u8], position: Vector) -> Self {
        Self {
            archetype,
            system,
            position,
            orientation: Matrix::IDENTITY,
            loadout: 0,
            costume: Costume::default(),
            vibe: -1,
            voice: 0,
            docking_base: 0,
            visit: 0,
            hit_pts: -1,
            nickname: nickname.to_vec(),
            mark_player: 0,
        }
    }
}

/// `pub::SpaceObj::CreateSolar`: a solar made at run time
/// (`StarSystem::create_solar`, [`make_solar`]) whose id is `CreateID` of
/// the nickname, from the info: its orientation through a quaternion, the
/// loadout's equipment, every collision group undamaged, destructible and
/// dynamic. Returns the id.
///
/// # Errors
///
/// -4 for a system id whose low 30 bits are 0 or that has no entry, or an
/// empty nickname; -1 when no solar is made: an unknown solar archetype
/// or loadout, or a system that is not loaded, where the C++ reads
/// through null (D707). The C++ writes the id before that failure.
pub fn create_solar(cx: &mut Cx<'_>, info: &SolarInfo) -> Result<u32, PubError> {
    let Some(system) = crate::transfer::lookup_system(cx, info.system) else {
        return Err(PubError::BAD_ARGUMENT);
    };
    let name = c_str(&info.nickname);
    if name.is_empty() {
        return Err(PubError::BAD_ARGUMENT);
    }
    let id = create_id(name);
    make_solar(cx, system, id, info).ok_or(PubError::FAILED)?;
    Ok(id)
}

/// `StarSystem::create_solar` with `CreateSolar`'s request: the solar
/// (`Solar::create`, `CSolar::init` with the request's hit points, base,
/// reputation, costume, voice and visit value, destructible and dynamic),
/// its server entry, the mark of the request's player, the `CSolar::init`
/// report of a dockable solar without a base, `check_fuses`, its place in
/// the system's spatial partition
/// ([`crate::api::system::add_to_solar_tree`]), and every player of the
/// system gets the request's record ([`crate::replication::solar_created`]).
fn make_solar(cx: &mut Cx<'_>, system: u32, id: u32, info: &SolarInfo) -> Option<ObjHandle> {
    let data = cx.world.data_arc();
    let arch = data.archetypes.solars.get(&info.archetype)?;
    if info.loadout != 0 && data.loadouts.get(info.loadout).is_none() {
        return None;
    }
    let orientation = matrix_to_quaternion(&info.orientation);
    let nickname = IdString(id).to_hex_string();
    let spawn = SolarSpawn {
        archetype: info.archetype,
        loadout: info.loadout,
        nickname: id,
        name: nickname.as_bytes().to_vec(),
        position: info.position,
        orientation: quaternion_to_matrix(&orientation),
        destructible: true,
        base: info.docking_base,
        dock_with: info.docking_base,
        visit: info.visit,
        voice: info.voice,
        costume: info.costume.clone(),
        vibe: info.vibe,
        hit_pts: info.hit_pts as f32,
        dynamic: true,
        ..SolarSpawn::default()
    };
    let h = cx.world.spawn_solar(system, &spawn).ok()?;
    let entry = cx.logic.objects.entry(cx.world, h);
    if !no_id(info.mark_player)
        && let Some(entry) = entry
    {
        entry.mark_for_player(info.mark_player, 1);
    }
    let bits = cx.world.get(h).map_or(0, |o| o.simple.type_bits);
    if bits & 0x60_0d60 != 0 && bits & 2 == 0 && no_id(info.docking_base) {
        cx.logic.reports.push(
            CSOLAR,
            0x6d,
            format!(
                "*** ERROR: {nickname} is dockable, but doesn't have a valid base or system to go to "
            ),
        );
    }
    crate::fuses::check_fuses(cx, h);
    super::system::add_to_solar_tree(cx, system, h);
    let record = SolarCreateInfo {
        base: EqObjCreateInfo {
            obj_id: id,
            archetype: info.archetype,
            docking_base: info.docking_base,
            vibe: info.vibe,
            costume: info.costume.clone(),
            voice: info.voice,
            position: info.position,
            orientation,
            hit_pts: info.hit_pts,
            field_6c: info.loadout,
            equipment: Vec::new(),
            collision_groups: arch
                .eq_obj
                .collision_groups
                .iter()
                .map(|g| (g.id, 1.0))
                .collect(),
        },
        dynamic: true,
        destructible: true,
        vibe_other_group: -1,
        marked_by: false,
    };
    crate::replication::solar_created(cx, system, h, &record);
    Some(h)
}

/// `pub::SpaceObj::GetMotion`: the object's velocity and angular velocity
/// (world space).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not). The C++ zeroes both first.
pub fn motion(cx: &Cx<'_>, object: u32) -> Result<(Vector, Vector), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    let i = cx.world.inspect(h).ok_or(PubError::BAD_ID)?;
    Ok((i.velocity, i.angular_velocity))
}

/// `pub::SpaceObj::GetRep`: an equipped object's reputation (`VibeId`).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not); -1 for an object without equipment. The C++ leaves
/// the out-parameter untouched on each.
pub fn rep(cx: &Cx<'_>, object: u32) -> Result<i32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_eq_obj(cx, h) {
        return Err(PubError::FAILED);
    }
    cx.world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|eq| eq.vibe)
        .ok_or(PubError::FAILED)
}

/// `pub::SpaceObj::GetSolarRep`: the id itself when a vibe has it
/// (`Vibe::Verify`).
///
/// # Errors
///
/// -2 when no vibe has it.
pub fn solar_rep(cx: &mut Cx<'_>, vibe: u32) -> Result<i32, PubError> {
    let key = vibe as i32;
    if cx.world.vibes().0.verify(key) {
        Ok(key)
    } else {
        Err(PubError::BAD_ID)
    }
}

/// `pub::SpaceObj::GetSolarArchetypeID`: an equipped object's archetype
/// (a ship's too).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not); -1 for an object without equipment. The C++ writes
/// 0 first.
pub fn solar_archetype_id(cx: &Cx<'_>, object: u32) -> Result<u32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_eq_obj(cx, h) {
        return Err(PubError::FAILED);
    }
    Ok(cx.world.get(h).map_or(0, |o| o.archetype))
}

/// `pub::SpaceObj::GetSolarArchetypeNickname`: a solar archetype's
/// nickname as `ID_String::get_string` spells it (`0x` and the id in
/// lower-case hex), when it is shorter than `max_len`.
///
/// # Errors
///
/// -4 for a `max_len` that is not positive; -2 for an unknown solar
/// archetype; -1 when the name does not fit.
pub fn solar_archetype_nickname(
    cx: &Cx<'_>,
    max_len: i32,
    archetype: u32,
) -> Result<Vec<u8>, PubError> {
    if max_len <= 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let solar = cx
        .world
        .data()
        .archetypes
        .solars
        .get(&archetype)
        .ok_or(PubError::BAD_ID)?;
    let name = IdString(solar.eq_obj.root.nickname)
        .to_hex_string()
        .into_bytes();
    if name.len() < max_len as usize {
        Ok(name)
    } else {
        Err(PubError::FAILED)
    }
}

/// `pub::SpaceObj::GetSolarParent`: the id of a solar's parent
/// (`CSolar::GetParentNickname`, found by its id).
///
/// # Errors
///
/// -4 for the id 0 (the whole id, no mask); -2 when no object has the id
/// (destroyed or not); -1 for an object that is not a solar; -3 when the
/// solar has no parent or no object has the parent's id. The C++ writes 0
/// first.
pub fn solar_parent(cx: &Cx<'_>, object: u32) -> Result<u32, PubError> {
    if object == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    let parent = match cx.world.get(h).map(|o| &o.data) {
        Some(KindData::Solar(s)) => s.parent_nickname,
        _ => return Err(PubError::FAILED),
    };
    if parent == 0 {
        return Err(PubError::NOT_FOUND);
    }
    let p = lookup(cx, parent).ok_or(PubError::NOT_FOUND)?;
    Ok(cx.world.get(p).map_or(0, |o| o.simple.id))
}

/// `pub::SpaceObj::GetHardpoint`: a hardpoint of the object's instance
/// tree in world space (`HardpointPosition`).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not); -3 when the object has no such hardpoint.
pub fn hardpoint(cx: &Cx<'_>, object: u32, name: &[u8]) -> Result<(Vector, Matrix), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    cx.world
        .hardpoint_world(h, c_str(name))
        .ok_or(PubError::NOT_FOUND)
}

/// `pub::SpaceObj::GetRadius`: the object's physical radius and its
/// centre (`get_physical_radius_r`).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not). The C++ zeroes both first.
pub fn radius(cx: &Cx<'_>, object: u32) -> Result<(f32, Vector), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    Ok(cx.world.physical_radius_r(h))
}

/// `pub::SpaceObj::GetBurnRadius`: a solar's atmosphere range (0 for any
/// other object) and the object's position.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not). The C++ zeroes both first.
pub fn burn_radius(cx: &Cx<'_>, object: u32) -> Result<(f32, Vector), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    let position = cx.world.get(h).map_or(Vector::ZERO, |o| o.engine.position);
    Ok((atmosphere_of(cx, h).unwrap_or(0.0), position))
}

/// `pub::SpaceObj::GetMass`.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not).
pub fn mass(cx: &Cx<'_>, object: u32) -> Result<f32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    Ok(cx.world.mass(h))
}

/// `pub::SpaceObj::GetCenterOfMass`: a `CSimple`'s centre of mass in world
/// space (`CObject::get_center_of_mass`).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not) or it is not a `CSimple`. The C++ zeroes it first.
pub fn center_of_mass(cx: &Cx<'_>, object: u32) -> Result<Vector, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_simple(cx, h) {
        return Err(PubError::BAD_ID);
    }
    Ok(cx.world.center_of_mass(h))
}

/// `pub::SpaceObj::GetVoiceID`: an equipped object's voice.
///
/// # Errors
///
/// -4 for the id 0 (the whole id, no mask); -2 when no object has the id
/// (destroyed or not); -3 for an object without equipment.
pub fn voice_id(cx: &Cx<'_>, object: u32) -> Result<u32, PubError> {
    if object == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_eq_obj(cx, h) {
        return Err(PubError::NOT_FOUND);
    }
    cx.world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|eq| eq.voice)
        .ok_or(PubError::NOT_FOUND)
}

/// What `GetInvincible` reads: the object's invincibility as its commands
/// add up.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct InvincibleState {
    /// `bInvincibleFlag`: protected from players.
    pub from_players: bool,
    /// `bInvincible`: protected from everyone.
    pub protect: bool,
    /// `InvincibleTimer`: the largest fraction of hit points it may lose.
    pub max_hp_loss: f32,
}

/// `pub::SpaceObj::GetInvincible`.
///
/// # Errors
///
/// -4 for the id 0 (the whole id, no mask); -2 when the object is not
/// found, destroyed or in transit.
pub fn invincible(cx: &mut Cx<'_>, object: u32) -> Result<InvincibleState, PubError> {
    if object == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let Some((h, Some(_))) = crate::transfer::find_live(cx, object) else {
        return Err(PubError::BAD_ID);
    };
    let o = cx
        .logic
        .objects
        .entry(cx.world, h)
        .ok_or(PubError::BAD_ID)?;
    Ok(InvincibleState {
        from_players: o.invincible_flag,
        protect: o.invincible,
        max_hp_loss: o.invincible_timer,
    })
}

/// `pub::SpaceObj::GetCargoSpaceOccupied`: a ship's hold size less what
/// is left of it (`get_cargo_hold_remaining`, unrounded), truncated to an
/// integer. An object that is not a ship, which the C++ reads through the
/// null narrowing, occupies 0 (D706).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the object is not found
/// or destroyed. The C++ writes 0 first.
pub fn cargo_space_occupied(cx: &Cx<'_>, object: u32) -> Result<u32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = crate::dock::lookup_live(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_ship(cx, h) {
        return Ok(0);
    }
    let (Some(hold), Some(remaining)) = (
        cx.world.cargo_hold_size(h),
        cx.world.cargo_hold_remaining(h),
    ) else {
        return Ok(0);
    };
    Ok(trunc_to_i32_via_i64(e(hold) - remaining) as u32)
}

/// `pub::SpaceObj::GetDockingTarget`: an equipped object's docking point
/// base when its low 30 bits are not 0, else its base; 0 for any other
/// object.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not). The C++ writes 0 first.
pub fn docking_target(cx: &Cx<'_>, object: u32) -> Result<u32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_eq_obj(cx, h) {
        return Ok(0);
    }
    Ok(cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map_or(0, |eq| {
            if no_id(eq.docking_point_base) {
                eq.docking_base
            } else {
                eq.docking_point_base
            }
        }))
}

/// `pub::SpaceObj::SetRelativeHealth`: a root entry at that fraction of
/// the maximum hit points (destroying at 0) is queued and, outside a
/// damage callback, applied at once (`GameObject::set_relative_health`,
/// [`crate::objects::set_relative_health`], then `apply_damage_list` with
/// the server's hooks).
///
/// # Errors
///
/// -4 for a ratio below 0, above 1 or NaN, or the id 0 (the whole id, no
/// mask); -2 when the object is not found or destroyed.
pub fn set_relative_health(cx: &mut Cx<'_>, object: u32, ratio: f32) -> Result<(), PubError> {
    if !(0.0..=1.0).contains(&ratio) || object == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = crate::dock::lookup_live(cx, object).ok_or(PubError::BAD_ID)?;
    if let Some(list) = crate::objects::set_relative_health(cx, h, ratio) {
        let mut hooks = crate::world::Hooks {
            logic: &mut *cx.logic,
        };
        cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
    }
    Ok(())
}

/// What `GetShieldHealth` reads of an equipped object's shields.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ShieldHealth {
    /// The shields' hit points, summed as floats.
    pub current: f32,
    /// Their maximum hit points, summed as floats.
    pub max: f32,
    /// Whether the last shield works (`IsFunctioning`); false without one.
    pub active: bool,
}

/// `pub::SpaceObj::GetShieldHealth` (`get_shield_status`): an equipped
/// object's shields (`CEquipTraverser(0x100)`, not destroyed); `None` for
/// any other object, whose out-parameters the C++ leaves untouched.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not).
pub fn shield_health(cx: &Cx<'_>, object: u32) -> Result<Option<ShieldHealth>, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_eq_obj(cx, h) {
        return Ok(None);
    }
    let Some(eq) = cx.world.get(h).and_then(|o| o.equipped.as_deref()) else {
        return Ok(None);
    };
    let mut out = ShieldHealth {
        current: 0.0,
        max: 0.0,
        active: false,
    };
    for slot in eq.equipment.traverse(types::SHIELD, false) {
        let Some(shield) = eq.equipment.get(slot) else {
            continue;
        };
        // `CEShield::GetHitPoints` and `GetMaxHitPoints`: the shield's own.
        let (current, max) = match &shield.kind {
            EquipKind::Shield(s) => (s.hit_pts, s.max_hit_pts),
            _ => (shield.hit_points(), shield.max_hit_points()),
        };
        out.current = r(e(out.current) + e(current));
        out.max = r(e(out.max) + e(max));
        out.active = shield.is_functioning(eq.weapons_offline);
    }
    Ok(Some(out))
}

/// `pub::SpaceObj::GetTradelaneNextAndPrev`: a solar's next and previous
/// trade-lane rings.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when no object has the id
/// (destroyed or not); -1 for an object that is not a solar.
pub fn tradelane_next_and_prev(cx: &Cx<'_>, object: u32) -> Result<(u32, u32), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = lookup(cx, object).ok_or(PubError::BAD_ID)?;
    match cx.world.get(h).map(|o| &o.data) {
        Some(KindData::Solar(s)) => Ok((s.next_ring, s.prev_ring)),
        _ => Err(PubError::FAILED),
    }
}

/// `pub::SpaceObj::LightFuse`: the fuse `CreateID(name)` lit on the object
/// by nobody, on no sub-object, at `delay`, with its own lifetime
/// (`EqObj::light_fuse(0, Fuse, SUBOBJ_ID_NONE, Delay, -1)`,
/// [`crate::fuses::light_fuse`]). An object without equipment, which the
/// C++ casts unchecked, lights nothing (D702).
///
/// # Errors
///
/// -4 for the id 0 (the whole id, no mask), or when the fuse is not found
/// (or the object has no equipment); -2 when the object is not found or
/// destroyed.
pub fn light_fuse(cx: &mut Cx<'_>, object: u32, name: &[u8], delay: f32) -> Result<(), PubError> {
    if object == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = crate::dock::lookup_live(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_eq_obj(cx, h) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let fuse = create_id(c_str(name));
    if crate::fuses::light_fuse(cx, h, 0, fuse, SUBOBJ_ID_NONE, delay, -1.0) {
        Ok(())
    } else {
        Err(PubError::BAD_ARGUMENT)
    }
}

/// What `GetTerminalInfo` reads of a docking sphere (`TerminalInfo`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TerminalInfo {
    /// `Name`: the sphere's hardpoint, at most 31 bytes (`safe_strcpy` into
    /// 0x20).
    pub name: Vec<u8>,
    /// `Value`: its dock type.
    pub value: u32,
}

/// `pub::SpaceObj::GetTerminalInfo`: an equipped object's docking sphere
/// `index` of its archetype: its hardpoint and its type.
///
/// # Errors
///
/// -2 for an id whose low 30 bits are 0, an object that is not found,
/// destroyed or without equipment; -3 for an index that is not one of the
/// spheres.
pub fn terminal_info(cx: &Cx<'_>, object: u32, index: i32) -> Result<TerminalInfo, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ID);
    }
    let h = crate::dock::lookup_live(cx, object).ok_or(PubError::BAD_ID)?;
    if !is_eq_obj(cx, h) {
        return Err(PubError::BAD_ID);
    }
    let point = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .and_then(|eq| {
            usize::try_from(u32::from_ne_bytes(index.to_ne_bytes()))
                .ok()
                .and_then(|i| eq.docking_points.get(i))
        })
        .ok_or(PubError::NOT_FOUND)?;
    let name = c_str(&point.hardpoint);
    Ok(TerminalInfo {
        name: name[..name.len().min(0x1f)].to_vec(),
        value: point.dock_type,
    })
}

/// `pub::SpaceObj::GetGoodID`: the good of a loot container's contents.
///
/// # Errors
///
/// -2 for an id whose low 30 bits are 0, or when the object is not found
/// or destroyed; -1 for an object that is not loot, or contents that are
/// no equipment.
pub fn good_id(cx: &Cx<'_>, object: u32) -> Result<u32, PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ID);
    }
    let h = crate::dock::lookup_live(cx, object).ok_or(PubError::BAD_ID)?;
    let contents = match cx.world.get(h).map(|o| &o.data) {
        Some(KindData::Loot(l)) => l.contents_arch,
        _ => return Err(PubError::FAILED),
    };
    let data = cx.world.data();
    if !data.archetypes.equipment.contains_key(&contents) {
        return Err(PubError::FAILED);
    }
    Ok(data.goods.arch_to_good(contents))
}

/// `pub::SpaceObj::GetJumpTarget`: a gate's, jump hole's or airlock's
/// (type bits within 0xc40) destination system and gate.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the object is not found
/// or destroyed; -1 for an object that is not a solar or has other type
/// bits.
pub fn jump_target(cx: &Cx<'_>, object: u32) -> Result<(u32, u32), PubError> {
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let h = crate::dock::lookup_live(cx, object).ok_or(PubError::BAD_ID)?;
    let o = cx.world.get(h).ok_or(PubError::BAD_ID)?;
    let KindData::Solar(s) = &o.data else {
        return Err(PubError::FAILED);
    };
    let bits = o.simple.type_bits;
    if bits & 0xc40 != bits {
        return Err(PubError::FAILED);
    }
    Ok((s.dest_system, s.dest_gate))
}

/// `pub::SpaceObj::SetInvincible2(Object, Protect, FromPlayers,
/// MaxHpLoss)`: an invincibility of type 3 with the flags as given (the
/// second not inverted). The C++ stores the two bytes as given and
/// `GetInvincible` answers them; the port keeps flags (D709).
///
/// # Errors
///
/// As [`set_invincible`].
pub fn set_invincible2(
    cx: &mut Cx<'_>,
    object: u32,
    protect: bool,
    from_players: bool,
    max_hp_loss: f32,
) -> Result<(), PubError> {
    command_invincibility(
        cx,
        object,
        Invincibility {
            kind: 3,
            protect,
            from_players,
            max_hp_loss,
        },
    )
}

/// `pub::SpaceObj::RequestSpaceScript`: `REQUESTSPACESCRIPT` to the client
/// `client` with the object and the script's name (`safe_strcpy` into
/// 0x104 bytes; with its terminator, nothing for an empty name). The
/// position and the priority are in the C++'s record but rpclocal's
/// encoder does not send them. Neither the client nor the name is tested.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0.
pub fn request_space_script(
    cx: &mut Cx<'_>,
    object: u32,
    position: Vector,
    priority: i32,
    client: u32,
    name: &[u8],
) -> Result<(), PubError> {
    let _ = (position, priority);
    if no_id(object) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let name = c_str(name);
    let name = &name[..name.len().min(0x103)];
    let mut wire = Vec::new();
    if !name.is_empty() {
        wire.extend_from_slice(name);
        wire.push(0);
    }
    let message = RequestSpaceScript {
        field_00: object,
        name: wire,
    };
    cx.logic
        .client
        .send(client, ToClient::Server(message.into()));
    Ok(())
}

/// `StarSystem::scan_objects` of a system `SysDB_LookupSystem` finds (no
/// ids for one it does not), at most 0x400.
fn scan(cx: &mut Cx<'_>, system: u32, position: Vector, radius: f32, mask: u32) -> Vec<u32> {
    if crate::transfer::lookup_system(cx, system).is_none() {
        return Vec::new();
    }
    super::system::scan_objects(cx, system, 0x400, position, radius, mask).unwrap_or_default()
}

/// The point and radius `GetEmptyPos` keeps clear of an object found by
/// id: none (the origin, 0) unless it is live; a planet's (type bit 2)
/// position and atmosphere range, else its physical radius and centre.
fn clearance_of(cx: &Cx<'_>, id: u32, use_bits: bool) -> (Vector, f32) {
    if no_id(id) {
        return (Vector::ZERO, 0.0);
    }
    let Some(h) = crate::dock::lookup_live(cx, id) else {
        return (Vector::ZERO, 0.0);
    };
    let position = cx.world.get(h).map_or(Vector::ZERO, |o| o.engine.position);
    if !use_bits || type_bits(cx, h) & 2 != 0 {
        return (position, atmosphere_of(cx, h).unwrap_or(0.0));
    }
    let (radius, center) = cx.world.physical_radius_r(h);
    (center, radius)
}

/// `sqrt((dz^2 + dy^2) + dx^2) - radius` in double.
fn gap(at: Vector, other: Vector, radius: f32) -> f64 {
    let dx = e(at.x) - e(other.x);
    let dy = e(at.y) - e(other.y);
    let dz = e(at.z) - e(other.z);
    ((dz * dz + dy * dy) + dx * dx).sqrt() - e(radius)
}

/// `pub::SpaceObj::GetEmptyPos`: a point near the transform's that keeps
/// `clearance` from the objects there. Each round scans the system for
/// everything within the radius (or, finding nothing, for its planets
/// anywhere) and tries up to 10 points (`rand()`) on the circle of the
/// radius in the transform's local XY (selection 0), YZ (1) or XZ (2)
/// plane, or in the local box of side the radius (3), each through the
/// transform (`TransformXform`): the first whose distance from every found
/// object, less that object's radius (a planet's atmosphere range, else
/// its physical radius) and the clearance, is positive is the answer;
/// otherwise the radius doubles (a float) and the next round starts. An
/// unknown system has nothing to avoid.
///
/// # Errors
///
/// -4 for a start radius or clearance below 0 or NaN, or a selection above
/// 3.
pub fn empty_pos(
    cx: &mut Cx<'_>,
    system: u32,
    base: &Transform,
    start_radius: f32,
    clearance: f32,
    selection: u32,
) -> Result<Vector, PubError> {
    // `FCOMP 0.0f` with the unordered case taken as below.
    let below = |v: f32| v.is_nan() || v < 0.0;
    if below(start_radius) || below(clearance) || selection > 3 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let mut radius = start_radius;
    loop {
        let mut ids = scan(cx, system, base.translation, radius, u32::MAX);
        if ids.is_empty() {
            ids = scan(cx, system, base.translation, -1.0, 2);
        }
        let mut attempts = 0;
        let mut found = None;
        while found.is_none() {
            let offset = if selection == 3 {
                let u1 = f64::from(cx.world.rng().rand()) * RAND_STEP;
                let u2 = f64::from(cx.world.rng().rand()) * RAND_STEP;
                let x = f64::from(cx.world.rng().rand()) * RAND_STEP * e(radius);
                Vector::new(r(x), r(e(radius) * u2), r(e(radius) * u1))
            } else {
                let angle = f64::from(cx.world.rng().rand()) * RAND_STEP * TWO_PI;
                let c = r(angle.cos() * e(radius));
                let s = r(angle.sin() * e(radius));
                match selection {
                    0 => Vector::new(c, s, 0.0),
                    1 => Vector::new(0.0, c, s),
                    _ => Vector::new(c, 0.0, s),
                }
            };
            let candidate = transform_xform(base, offset);
            let collided = ids.iter().any(|&id| {
                let (other, other_radius) = clearance_of(cx, id, true);
                gap(candidate, other, other_radius) - e(clearance) <= 0.0
            });
            if !collided {
                found = Some(candidate);
                break;
            }
            attempts += 1;
            if attempts >= 10 {
                break;
            }
        }
        radius = r(e(radius) + e(radius));
        if let Some(candidate) = found {
            return Ok(candidate);
        }
    }
}

/// `pub::SpaceObj::IsPosEmpty`: false when anything of the system is
/// within `radius` of the point, or, when nothing is, when the point is
/// inside the atmosphere range of one of the system's planets (type bit 2,
/// a distance less the range that is not positive).
///
/// # Errors
///
/// -1 for a system id whose low 30 bits are 0 or that has no entry (the
/// C++ writes true first).
pub fn is_pos_empty(
    cx: &mut Cx<'_>,
    system: u32,
    position: Vector,
    radius: f32,
) -> Result<bool, PubError> {
    if crate::transfer::lookup_system(cx, system).is_none() {
        return Err(PubError::FAILED);
    }
    if !scan(cx, system, position, radius, u32::MAX).is_empty() {
        return Ok(false);
    }
    let hit = scan(cx, system, position, -1.0, 2).into_iter().any(|id| {
        let (other, other_radius) = clearance_of(cx, id, false);
        gap(position, other, other_radius) <= 0.0
    });
    Ok(!hit)
}
