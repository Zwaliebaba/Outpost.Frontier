//! `pub::SpaceObj` (`Server/server_pub.cpp`, `Server\space\pubSpace.cpp`):
//! the creation of a ship (`Create`, `:2456`) and its transport loadout
//! (`SpaceObj_BuildTransportLoadout`, `Server\SrvMisc.cpp`). The other
//! `SpaceObj` functions arrive with the parts that own their state.

use std::collections::{BTreeMap, BTreeSet};

use game_sim::data::archetype::equipment::EquipmentKind;
use game_sim::data::costume::Costume;
use game_sim::data::equip_desc::EquipDesc;
use neuron_core::id::IdString;
use neuron_core::id::sub_object::EquipmentIds;
use neuron_core::math::{Matrix, Vector, matrix_to_quaternion};
use neuron_server::controller::ControllerId;

use super::PubError;
use crate::server::Cx;
use crate::ship::ShipCreateRequest;

const SRV_MISC: &str = "Server\\SrvMisc.cpp";

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
