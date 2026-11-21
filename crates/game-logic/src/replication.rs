//! Replication, the observers (work package P3.3, decision record 0007,
//! point 7): one [`Observer`] per player in each star system keeps the
//! objects that player's client is told about, and every system frame
//! decides what the client is sent. Ported from
//! `Server/server_observer.cpp` (`SystemPlayerView_Update`,
//! `StarSystem_DamageObject`) and the observer part of
//! `Server/server_classes.cpp` (`include/server_sysdb.h:240-457`).
//!
//! - **What an observer holds.** The tracked objects, each with its
//!   `CRemotePhysicsSimulation` and whether the client has been told it
//!   exists (seeded from single player, where the client already has
//!   everything); the ids of the objects whose damage waits until they are
//!   visible; the observer's clock; the position of the player's ship; and
//!   the latch that stops the creates once the player's ship has left.
//! - **Visibility** ([`should_be_visible`]). Always in single player and
//!   for the player's own objects. Otherwise the squared distance from the
//!   player's ship against 2500² for an object nobody owns, 10 000² for a
//!   player of the same group and 5000² for any other player's.
//! - **Single player** sends the creates at once ([`add_player`], the
//!   creators' fan-outs) and runs no per-frame pass.
//! - **Multiplayer** runs the five phases of [`update`] after each system
//!   frame: the created records' dead reckoning; the creates of the
//!   records now in range; the destroys of those out of range and the
//!   sync checks of the unowned rest; the updates, the record longest
//!   without one first; the deferred damage. Each phase stops once the
//!   client's link saturation reaches 0.9 ([`ClientSink::link_saturation`]),
//!   which the loopback reports as 0 until P3.8.
//! - **Creates** for ships, solars, loot, mines, counter-measures and
//!   guided munitions, from the object (`*CreateInfo_Fill`,
//!   `Server/server_createinfo.cpp`) or from the creator's record, each
//!   carried into the packet as rpclocal's marshaller does
//!   (`rpclocal/rpc_payload_ch2.cpp`, the create-object header of
//!   `rpc_containers.cpp`).
//! - **Destroys** ([`object_destroyed`]) and the damage fan-out
//!   ([`damage_object`]), whose deferred half the fifth phase retries.
//!
//! The per-object fan-outs beyond these (equipment, cruise, thrusters,
//! target, fire, fuses, comm, trade lanes, group changes, cargo, the
//! object updates a player's client sends, leaving the system) and the
//! solars' damage state and `SETREPUTATION` of `add_player` are left to
//! later increments.
//!
//! [`ClientSink::link_saturation`]: crate::server::ClientSink::link_saturation

use std::collections::BTreeSet;

use game_sim::data::costume::Costume;
use game_sim::data::equip_desc::EquipDesc;
use game_sim::equip::{EquipKind, Fate, SUBOBJ_ID_NONE, SUBOBJ_ID_ROOT, SubObjId, types};
use game_sim::object::damage::{DamageCause, DamageList};
use game_sim::object::{KindData, ObjHandle, ObjectKind, World, class};
use game_sim::protocol::ToClient;
use game_sim::protocol::common::{SetTarget, UpdateObject};
use game_sim::protocol::costume::CreateCostume;
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::quantise_orientation;
use game_sim::protocol::records::{
    CreateCollisionGroup, CreateEquip, CreateHardpoint, CreateObject,
    DamageEntry as WireDamageEntry, DamageList as WireDamageList,
};
use game_sim::protocol::to_client::{
    ActivateObject, CreateCounter, CreateGuided, CreateLoot, CreateMine, CreateShip,
    CreateShipOwner, CreateSolar, DamageObject, DestroyObject, LootRecord,
};
use neuron_core::id::sub_object::{is_arch_group_id, is_equip_id};
use neuron_core::math::{Matrix, Quaternion, Vector, matrix_to_quaternion};
use neuron_core::net::wire::quantise::{quantise_fraction, quantise_hull, quantise_unit};
use neuron_core::num::trunc_to_i32;
use neuron_core::physics::RemotePhysicsSimulation;

use crate::server::{Cx, Logic};

/// The link saturation at which a phase stops (`.rdata` double 0.9).
pub const LINK_SATURATION_CEILING: f64 = 0.9;
/// 2500²: an object nobody owns.
pub const VIEW_RADIUS_SQ_UNOWNED: f32 = 6.25e6;
/// 5000²: another player's object.
pub const VIEW_RADIUS_SQ_STRANGER: f32 = 2.5e7;
/// 10 000²: an object of a player in the same group.
pub const VIEW_RADIUS_SQ_GROUP_MATE: f32 = 1.0e8;
/// Every solar `OBJECT_TYPE` but `WAYPOINT`: objects a client has from the
/// system file, told of their destruction whether tracked or not.
pub const SOLAR_TYPE_BITS: u32 = 0xfdff;
/// `TRADELANE_RING`: a ring's damage reaches every client.
pub const TRADELANE_RING: u32 = 0x80;
/// The cause of the damage lists the server builds to describe an object
/// (`get_damage_list`'s callers write 0x19, which has no name).
pub const CAUSE_DAMAGE_STATE: DamageCause = 0x19;
/// `CREATELOOT`'s kind byte when the per-frame pass creates one container.
pub const LOOT_KIND_OBSERVED: u32 = 5;

const OBSERVER_CPP: &str = "Server\\space\\Observer.cpp";

/// One object an observer tracks (`SystemTrackedObject`).
#[derive(Clone, Debug, PartialEq)]
pub struct TrackedObject {
    /// The object (`Object`).
    pub object: ObjHandle,
    /// Its id, read when it was tracked.
    pub id: u32,
    /// The client's dead reckoning of it (`Simulation`).
    pub simulation: RemotePhysicsSimulation,
    /// The client has been told it exists (`bCreatedOnClient`).
    pub created_on_client: bool,
}

/// One player's view of a system (`Observer`, `server_sysdb.h:240`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Observer {
    /// The session (`PlayerId`).
    pub player: u32,
    /// The tracked objects, in the order they were tracked (`Tracked`).
    pub tracked: Vec<TrackedObject>,
    /// The ids whose damage waits until they are visible
    /// (`PendingDamageIds`).
    pub pending_damage: BTreeSet<u32>,
    /// The player's ship has left the system: no more creates
    /// (`bShipGone`).
    pub ship_gone: bool,
    /// The observer's clock, the frames it has run (`ElapsedTime`).
    pub elapsed_time: f64,
    /// The position of the player's ship at the last update (`Position`).
    pub position: Vector,
}

impl Observer {
    /// `Observer::Observer(PlayerId)`: everything else empty or zero.
    #[must_use]
    pub fn new(player: u32) -> Self {
        Self {
            player,
            ..Self::default()
        }
    }

    /// The record of an object (`SystemPlayerView_FindByPointer`).
    #[must_use]
    pub fn find(&self, object: ObjHandle) -> Option<&TrackedObject> {
        self.tracked.iter().find(|t| t.object == object)
    }

    fn position_of_id(&self, id: u32) -> Option<usize> {
        self.tracked.iter().position(|t| t.id == id)
    }
}

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn single_player(world: &World) -> bool {
    world.config().single_player
}

fn send_server(
    logic: &mut Logic,
    client: u32,
    message: impl Into<game_sim::protocol::to_client::ServerMessage>,
) {
    logic.client.send(client, ToClient::Server(message.into()));
}

fn send_common(
    logic: &mut Logic,
    client: u32,
    message: impl Into<game_sim::protocol::common::CommonMessage>,
) {
    logic.client.send(client, ToClient::Common(message.into()));
}

/// `HasBandwidth`: the client's link is below the ceiling.
fn has_bandwidth(logic: &Logic, player: u32) -> bool {
    logic.client.link_saturation(player) < LINK_SATURATION_CEILING
}

fn group_of(logic: &Logic, player: u32) -> Option<u32> {
    logic.players.get(player).and_then(|p| p.group)
}

fn rep_vibe(logic: &Logic, player: u32) -> i32 {
    logic.players.get(player).map_or(-1, |p| p.rep_vibe)
}

fn ship_object_id(logic: &Logic, player: u32) -> u32 {
    logic.players.get(player).map_or(0, |p| p.ship_object_id)
}

fn is_marked_by(logic: &Logic, h: ObjHandle, player: u32) -> bool {
    logic.objects.get(h).is_some_and(|o| o.is_marked_by(player))
}

/// `GameObject::is_destroyed`: the destroyed latch, or an object the world
/// already marked dead.
fn is_destroyed(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.logic.objects.get(h).is_some_and(|o| o.destroyed) || cx.world.get(h).is_none_or(|o| o.dead)
}

fn quat(m: &Matrix) -> Quaternion {
    matrix_to_quaternion(m)
}

fn wxyz(q: Quaternion) -> [f32; 4] {
    [q.w, q.x, q.y, q.z]
}

fn xyz(v: Vector) -> [f32; 3] {
    [v.x, v.y, v.z]
}

/// `SystemPlayerView_ShouldBeVisible`: whether an object the observer
/// does not track yet is visible. A missing object (the C++'s null) and
/// single player answer true, as does the player's own object; otherwise
/// the squared distance from the player's ship (a float) is compared
/// with the radius of the owner: [`VIEW_RADIUS_SQ_UNOWNED`] for none,
/// [`VIEW_RADIUS_SQ_GROUP_MATE`] for a player in the observer's group
/// (falling back to the stranger radius beyond it), and
/// [`VIEW_RADIUS_SQ_STRANGER`] otherwise. A NaN distance is not visible.
#[must_use]
pub fn should_be_visible(
    world: &World,
    logic: &Logic,
    player: u32,
    position: Vector,
    object: ObjHandle,
) -> bool {
    let Some(o) = world.get(object) else {
        return true;
    };
    if single_player(world) {
        return true;
    }
    let owner = o.simple.owner_player;
    if owner == player {
        return true;
    }
    let at = o.engine.position;
    // Each difference and the sum are stored as floats.
    let dx = e((e(at.x) - e(position.x)) as f32);
    let dy = e((e(at.y) - e(position.y)) as f32);
    let dz = e((e(at.z) - e(position.z)) as f32);
    let dist_sq = e((dx * dx + dy * dy + dz * dz) as f32);
    if owner == 0 {
        return dist_sq <= e(VIEW_RADIUS_SQ_UNOWNED);
    }
    let mine = group_of(logic, player);
    if mine.is_some() && mine == group_of(logic, owner) && dist_sq <= e(VIEW_RADIUS_SQ_GROUP_MATE) {
        return true;
    }
    dist_sq <= e(VIEW_RADIUS_SQ_STRANGER)
}

/// `SystemPlayerView_IsObjectVisible`: a missing object, single player and
/// the player's own object are visible; a tracked object is visible when
/// the client has been told it exists; any other goes by
/// [`should_be_visible`].
#[must_use]
pub fn is_object_visible(
    world: &World,
    logic: &Logic,
    view: &Observer,
    object: Option<ObjHandle>,
) -> bool {
    let Some(h) = object.filter(|&h| world.get(h).is_some()) else {
        return true;
    };
    if single_player(world) {
        return true;
    }
    if world
        .get(h)
        .is_some_and(|o| o.simple.owner_player == view.player)
    {
        return true;
    }
    if let Some(t) = view.find(h) {
        return t.created_on_client;
    }
    should_be_visible(world, logic, view.player, view.position, h)
}

/// `SystemPlayerView_IsObjectIdVisible`: single player answers true;
/// otherwise [`is_object_visible`] of the object with the id (none when
/// the lookup fails).
#[must_use]
pub fn is_object_id_visible(world: &World, logic: &Logic, view: &Observer, id: u32) -> bool {
    if single_player(world) {
        return true;
    }
    is_object_visible(world, logic, view, world.find(id))
}

/// `SystemPlayerView_AddTrackedObject`: a record for the object at the end
/// of the list, created on the client in single player; outside single
/// player its simulation takes the object's position and orientation at
/// the observer's clock. Returns the record's index.
pub fn add_tracked_object(world: &World, view: &mut Observer, object: ObjHandle) -> usize {
    let single = single_player(world);
    let (id, position, orientation) = world
        .get(object)
        .map_or((0, Vector::ZERO, Matrix::IDENTITY), |o| {
            (o.simple.id, o.engine.position, o.engine.orientation)
        });
    let mut record = TrackedObject {
        object,
        id,
        simulation: RemotePhysicsSimulation::default(),
        created_on_client: single,
    };
    if !single {
        record
            .simulation
            .remote_update(position, quat(&orientation), view.elapsed_time);
    }
    view.tracked.push(record);
    view.tracked.len() - 1
}

// ---------------------------------------------------------------------------
// The create records and their packets.

/// The shared part of `CShipCreateInfo` and `CSolarCreateInfo`
/// (`CEqObjCreateInfo`) that reaches the wire.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EqObjCreateInfo {
    /// `ObjId`.
    pub obj_id: u32,
    /// `ArchetypeNickname`.
    pub archetype: u32,
    /// `DockingBaseId`.
    pub docking_base: u32,
    /// `VibeGroupId`.
    pub vibe: i32,
    /// The costume.
    pub costume: Costume,
    /// `VoiceId`.
    pub voice: u32,
    /// `Position`.
    pub position: Vector,
    /// `Orientation`.
    pub orientation: Quaternion,
    /// `HitPts`, an integer.
    pub hit_pts: i32,
    /// `Field_0x6c`: the loadout of a creator's record, 0 from an object.
    pub field_6c: u32,
    /// `Equipment`.
    pub equipment: Vec<EquipDesc>,
    /// `CollisionGroups`: id and relative health.
    pub collision_groups: Vec<(SubObjId, f32)>,
}

/// `CShipCreateInfo` as far as it reaches the wire.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ShipCreateInfo {
    /// The shared part.
    pub base: EqObjCreateInfo,
    /// `bUsingTradelane`.
    pub using_tradelane: bool,
    /// `Velocity`.
    pub velocity: Vector,
    /// `Throttle`.
    pub throttle: f32,
    /// `OwnerPlayer`.
    pub owner_player: u32,
    /// +0xb0, the ship's `GroupNameId`.
    pub group_name_id: u32,
    /// `ReputationRank`.
    pub rank: u32,
    /// +0xd8, the ship's player group.
    pub group_id: u32,
    /// `VibeOtherGroupId`: the receiving player's vibe (-1 until set).
    pub vibe_other_group: i32,
    /// `bMarkedBy`.
    pub marked_by: bool,
}

/// `CSolarCreateInfo` as far as it reaches the wire.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SolarCreateInfo {
    /// The shared part.
    pub base: EqObjCreateInfo,
    /// +0xac, `bDynamic`.
    pub dynamic: bool,
    /// +0xad, `bDestructible`.
    pub destructible: bool,
    /// +0xb0, `VibeOtherGroupId`: the receiving player's vibe (-1 until
    /// set).
    pub vibe_other_group: i32,
    /// `bMarkedBy`.
    pub marked_by: bool,
}

/// `FServerCreateProjectileInfo`, the record of `CREATEMINE` and
/// `CREATECOUNTER`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ProjectileCreateInfo {
    /// `ObjId`.
    pub obj_id: u32,
    /// `ArchetypeNickname`.
    pub archetype: u32,
    /// `OwnerId`.
    pub owner_id: u32,
    /// `SubObjId`: the dropper's item, `SUBOBJ_ID_NONE` from an object.
    pub sub_obj_id: SubObjId,
    /// `Position`.
    pub position: Vector,
    /// `Orientation`.
    pub orientation: Quaternion,
    /// `Velocity`.
    pub velocity: Vector,
    /// `Field_0x38`: the creator's angular velocity, zero from an object.
    pub field_38: Vector,
    /// `Field_0x44`.
    pub field_44: u32,
}

/// `FServerCreateGuidedInfo`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GuidedCreateInfo {
    /// `ObjId`.
    pub obj_id: u32,
    /// `Position`.
    pub position: Vector,
    /// `Orientation`.
    pub orientation: Quaternion,
    /// `TargetId`.
    pub target_id: u32,
    /// `TargetSubObjId`.
    pub target_sub_obj_id: SubObjId,
    /// `ArchetypeNickname`.
    pub archetype: u32,
    /// `OwnerId`.
    pub owner_id: u32,
    /// `SubObjId`: the launcher, `SUBOBJ_ID_NONE` from an object.
    pub sub_obj_id: SubObjId,
}

/// `FServerLootDesc`, one container of `CREATELOOT`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LootDesc {
    /// `LootId`.
    pub loot_id: u32,
    /// `ContentsNickname`.
    pub contents: u32,
    /// `ContentsHitPts`.
    pub contents_hit_pts: f32,
    /// `Units`.
    pub units: u32,
    /// `ContainerNickname`.
    pub container: u32,
    /// `HitPts`.
    pub hit_pts: f32,
    /// `NameOverride`: the name when it is not the contents'.
    pub name_override: u32,
    /// `Position`.
    pub position: Vector,
    /// `Orientation`.
    pub orientation: Quaternion,
    /// `Velocity`.
    pub velocity: Vector,
    /// `bTemporary`.
    pub temporary: bool,
    /// `bMarkedBy`.
    pub marked_by: bool,
}

/// `FServerCreateLootRequest` as far as it reaches the wire.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LootCreateRequest {
    /// `OwnerId`.
    pub owner_id: u32,
    /// `Kind`, its low byte travels.
    pub kind: u32,
    /// `Items`.
    pub items: Vec<LootDesc>,
}

/// `CEqObj::get_equip_desc_list`: the description of every item that has
/// one (`GetEquipDesc`), destroyed items left out.
fn equip_desc_list(world: &mut World, h: ObjHandle) -> Vec<EquipDesc> {
    let ids: Vec<SubObjId> = world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|eq| {
            eq.equipment
                .traverse(u32::MAX, false)
                .into_iter()
                .filter_map(|slot| eq.equipment.get(slot).map(|e| e.id))
                .collect()
        })
        .unwrap_or_default();
    ids.into_iter()
        .filter_map(|id| world.equip_desc(h, id))
        .collect()
}

/// `CEqObj::get_collision_group_description`: each group by id, with its
/// hit points over the archetype's rounded to a float.
fn collision_group_description(world: &World, h: ObjHandle) -> Vec<(SubObjId, f32)> {
    let mut groups: Vec<(SubObjId, f32)> = world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|eq| {
            eq.arch_groups
                .iter()
                .map(|g| (g.id, g.relative_health() as f32))
                .collect()
        })
        .unwrap_or_default();
    groups.sort_by_key(|g| g.0);
    groups
}

/// The shared part of `CShipCreateInfo_Fill` and `CSolarCreateInfo_Fill`.
fn eq_obj_create_info(world: &mut World, h: ObjHandle) -> Option<EqObjCreateInfo> {
    let o = world.get(h)?;
    let eq = o.equipped.as_deref()?;
    let mut info = EqObjCreateInfo {
        obj_id: o.simple.id,
        archetype: o.archetype,
        docking_base: eq.docking_base,
        vibe: eq.vibe,
        costume: eq.space_costume.clone(),
        voice: eq.voice,
        position: o.engine.position,
        orientation: quat(&o.engine.orientation),
        hit_pts: trunc_to_i32(e(o.simple.hit_pts)),
        field_6c: 0,
        equipment: Vec::new(),
        collision_groups: collision_group_description(world, h),
    };
    info.equipment = equip_desc_list(world, h);
    Some(info)
}

/// `CShipCreateInfo_Fill`: the ship's record. The damage list the C++
/// builds and drops is not built.
pub fn ship_create_info(world: &mut World, h: ObjHandle) -> Option<ShipCreateInfo> {
    let o = world.get(h)?;
    let ship = o.as_ship()?;
    let (using_tradelane, throttle, group_name_id, group_id) = (
        ship.using_tradelane,
        ship.flight.throttle,
        ship.group_name_id,
        ship.group_id,
    );
    let owner_player = o.simple.owner_player;
    let base = eq_obj_create_info(world, h)?;
    let rank = world.vibes().0.rank(base.vibe).1;
    Some(ShipCreateInfo {
        velocity: world.velocity(h),
        using_tradelane,
        throttle,
        owner_player,
        group_name_id,
        rank,
        group_id,
        vibe_other_group: -1,
        marked_by: false,
        base,
    })
}

/// `CShipCreateInfo` copied from `StarSystem::create_ship`'s request: the
/// request's fields (its loadout as `Field_0x6c`, its equipment, its
/// collision groups), throttle and player group 0.
#[must_use]
pub fn ship_request_info(
    req: &crate::ship::ShipCreateRequest,
    groups: Option<&[crate::character_file::CollisionGroupDesc]>,
) -> ShipCreateInfo {
    ShipCreateInfo {
        base: EqObjCreateInfo {
            obj_id: req.id,
            archetype: req.archetype,
            docking_base: req.docking_base,
            vibe: req.vibe,
            costume: req.costume.clone(),
            voice: req.voice,
            position: req.position,
            orientation: req.orientation,
            hit_pts: req.hit_pts,
            field_6c: req.loadout,
            equipment: req.equipment.clone(),
            collision_groups: groups
                .map(|g| g.iter().map(|d| (d.id, d.health)).collect())
                .unwrap_or_default(),
        },
        using_tradelane: req.using_tradelane,
        velocity: req.velocity,
        throttle: 0.0,
        owner_player: req.owner_player,
        group_name_id: req.group_name_id,
        rank: req.rank,
        group_id: 0,
        vibe_other_group: -1,
        marked_by: false,
    }
}

/// `CSolarCreateInfo_Fill`: the solar's record.
pub fn solar_create_info(world: &mut World, h: ObjHandle) -> Option<SolarCreateInfo> {
    let (dynamic, destructible) = match &world.get(h)?.data {
        KindData::Solar(s) => (s.dynamic, s.destructible),
        _ => return None,
    };
    Some(SolarCreateInfo {
        base: eq_obj_create_info(world, h)?,
        dynamic,
        destructible,
        vibe_other_group: -1,
        marked_by: false,
    })
}

/// `ServerCreateProjectileInfo_Fill`: a mine's or counter-measure's
/// record from the object: no sub-object and no angular velocity.
#[must_use]
pub fn projectile_create_info(world: &World, h: ObjHandle) -> Option<ProjectileCreateInfo> {
    let o = world.get(h)?;
    let owner_id = match &o.data {
        KindData::Mine(m) => m.projectile.owner_id,
        KindData::CounterMeasure(c) => c.projectile.owner_id,
        KindData::Guided(g) => g.projectile.owner_id,
        _ => return None,
    };
    Some(ProjectileCreateInfo {
        obj_id: o.simple.id,
        archetype: o.archetype,
        owner_id,
        sub_obj_id: SUBOBJ_ID_NONE,
        position: o.engine.position,
        orientation: quat(&o.engine.orientation),
        velocity: world.velocity(h),
        field_38: Vector::ZERO,
        field_44: 0,
    })
}

/// The record `StarSystem::create_mine` and `create_counter_measure` build
/// from their launch: the dropper's item and the angular velocity drawn.
#[must_use]
pub fn launch_create_info(
    world: &World,
    h: ObjHandle,
    launch: &game_sim::object::world::ProjectileSpawn,
    equip: SubObjId,
    spin: Vector,
) -> ProjectileCreateInfo {
    ProjectileCreateInfo {
        obj_id: world.get(h).map_or(0, |o| o.simple.id),
        archetype: launch.archetype,
        owner_id: launch.owner,
        sub_obj_id: equip,
        position: launch.position,
        orientation: quat(&launch.orientation),
        velocity: launch.velocity,
        field_38: spin,
        field_44: 0,
    }
}

/// `ServerCreateGuidedInfo_Fill`: a guided munition's record, its target's
/// id (0 for none) and sub-object.
#[must_use]
pub fn guided_create_info(world: &World, h: ObjHandle) -> Option<GuidedCreateInfo> {
    let o = world.get(h)?;
    let KindData::Guided(g) = &o.data else {
        return None;
    };
    let target_id = g
        .target
        .and_then(|t| world.get(t))
        .map_or(0, |t| t.simple.id);
    Some(GuidedCreateInfo {
        obj_id: o.simple.id,
        position: o.engine.position,
        orientation: quat(&o.engine.orientation),
        target_id,
        target_sub_obj_id: g.sub_target,
        archetype: o.archetype,
        owner_id: g.projectile.owner_id,
        sub_obj_id: SUBOBJ_ID_NONE,
    })
}

/// `ServerLootDesc_Fill`: a container's record; the name travels when it
/// is not the contents' (`get_name`: the override, else the contents'
/// name). `bMarkedBy` is the caller's.
#[must_use]
pub fn loot_desc(world: &World, h: ObjHandle) -> Option<LootDesc> {
    let o = world.get(h)?;
    let KindData::Loot(l) = &o.data else {
        return None;
    };
    let contents_name = world
        .data()
        .archetypes
        .equipment
        .get(&l.contents_arch)
        .map_or(0, |a| a.root.ids_name as u32);
    let name = if l.name_id != 0 {
        l.name_id as u32
    } else {
        contents_name
    };
    Some(LootDesc {
        loot_id: o.simple.id,
        contents: l.contents_arch,
        contents_hit_pts: l.contents_hit_pts,
        units: l.units,
        container: o.archetype,
        hit_pts: o.simple.hit_pts,
        name_override: if name == contents_name { 0 } else { name },
        position: o.engine.position,
        orientation: quat(&o.engine.orientation),
        velocity: world.velocity(h),
        temporary: l.temporary,
        marked_by: false,
    })
}

/// `RpcCreateObjectHeaderWrite` and the header's encoder: the archetype as
/// its small id, the orientation and hull quantised, every item as a
/// record (the hardpoint left out for an internal one), the collision
/// groups' low id bytes and healths. The damage list the C++ sends is
/// always empty.
fn create_object(world: &World, info: &EqObjCreateInfo) -> CreateObject {
    let archetypes = &world.data().archetypes;
    let strings = world.strings();
    let equipment = info
        .equipment
        .iter()
        .map(|d| CreateEquip {
            equipped: d.equipped != 0,
            field_0f: false,
            count: d.count,
            health: CreateEquip::health_byte(d.health),
            archetype: archetypes.large_to_small(d.arch_id),
            id: d.id,
            hardpoint: if d.is_internal(strings) {
                CreateHardpoint::None
            } else {
                CreateHardpoint::from_name(strings.get(d.hardpoint))
            },
        })
        .collect();
    CreateObject {
        obj_id: info.obj_id,
        archetype: archetypes.large_to_small(info.archetype),
        docking_base_id: info.docking_base,
        vibe_group_id: info.vibe as u32,
        costume: CreateCostume {
            body: info.costume.part_ids[1],
            head: info.costume.part_ids[0],
            accessories: info.costume.accessory_ids.clone(),
        },
        voice_id: info.voice,
        position: xyz(info.position),
        orientation: quantise_orientation(wxyz(info.orientation)),
        hull: quantise_hull(info.hit_pts, archetypes.max_hit_pts(info.archetype)),
        field_6c: info.field_6c,
        equipment,
        collision_groups: info
            .collision_groups
            .iter()
            .map(|&(id, health)| CreateCollisionGroup {
                id: id.to_le_bytes()[0],
                health: quantise_fraction(health),
            })
            .collect(),
        damage: Vec::new(),
    }
}

/// `Vibe::GetName` with the C++'s empty names when the vibe is unknown.
fn vibe_names(world: &mut World, vibe: i32) -> (FmtStr, FmtStr, Vec<u16>) {
    let (a, b, display) = world.vibes().0.name(vibe).unwrap_or_default();
    let end = display
        .iter()
        .position(|&u| u == 0)
        .unwrap_or(display.len());
    (a, b, display[..end].to_vec())
}

fn flatten(name: &FmtStr) -> Vec<u8> {
    name.flatten().unwrap_or_default()
}

/// `GetAffiliation` and `AttitudeTowards(vibe, other)` as the packets carry
/// them: the affiliation and the attitude × 127.
fn vibe_relation(world: &mut World, vibe: i32, other: i32) -> (u32, u8) {
    let (vibes, rep) = world.vibes();
    let affiliation = vibes.affiliation(vibe, false).1;
    let attitude = vibes.attitude_towards(vibe, other, rep).1;
    (affiliation, quantise_unit(attitude))
}

/// rpclocal's `MarshalCreateShip` and the tail's encoder: the velocity,
/// the throttle × 127, the group name's and rank's low bits; an owned
/// ship carries the owner's low byte and its player group's low word.
/// Outside single player the vibe's display name goes with an owned ship
/// and its two names with an NPC (empty when it has a display name),
/// then the affiliation and the attitude towards the receiver.
pub fn create_ship_packet(world: &mut World, info: &ShipCreateInfo) -> CreateShip {
    let mut packet = CreateShip {
        object: create_object(world, &info.base),
        using_tradelane: info.using_tradelane,
        marked_by: info.marked_by,
        velocity: xyz(info.velocity),
        throttle: quantise_unit(info.throttle),
        field_c0: info.group_name_id as u16,
        reputation_rank: info.rank as u8,
        owner: (info.owner_player != 0).then(|| CreateShipOwner {
            player: info.owner_player as u8,
            field_c4: info.group_id as u16,
            display_name: Vec::new(),
        }),
        ..CreateShip::default()
    };
    if single_player(world) {
        return packet;
    }
    let (a, b, display) = vibe_names(world, info.base.vibe);
    if display.is_empty() {
        packet.names = (flatten(&a), flatten(&b));
    } else if let Some(owner) = &mut packet.owner {
        owner.display_name = display;
    }
    let (affiliation, attitude) = vibe_relation(world, info.base.vibe, info.vibe_other_group);
    packet.affiliation = affiliation;
    packet.attitude = attitude;
    packet
}

/// rpclocal's `MarshalCreateSolar`: the destructible flag and the mark;
/// outside single player the affiliation, the attitude towards the
/// receiver and the vibe's two names.
pub fn create_solar_packet(world: &mut World, info: &SolarCreateInfo) -> CreateSolar {
    let mut packet = CreateSolar {
        object: create_object(world, &info.base),
        field_9c: u8::from(info.destructible),
        marked_by: u8::from(info.marked_by),
        ..CreateSolar::default()
    };
    if single_player(world) {
        return packet;
    }
    let (affiliation, attitude) = vibe_relation(world, info.base.vibe, info.vibe_other_group);
    packet.affiliation = affiliation;
    packet.attitude = attitude;
    let (a, b, _) = vibe_names(world, info.base.vibe);
    packet.names = (flatten(&a), flatten(&b));
    packet
}

/// rpclocal's `MarshalCreateMine`.
#[must_use]
pub fn create_mine_packet(world: &World, info: &ProjectileCreateInfo) -> CreateMine {
    CreateMine {
        obj_id: info.obj_id,
        archetype: world.data().archetypes.large_to_small(info.archetype),
        owner_id: info.owner_id,
        sub_obj_id: info.sub_obj_id,
        position: xyz(info.position),
        orientation: wxyz(info.orientation),
        velocity: xyz(info.velocity),
        field_38: xyz(info.field_38),
    }
}

/// rpclocal's `MarshalCreateCounter`.
#[must_use]
pub fn create_counter_packet(info: &ProjectileCreateInfo) -> CreateCounter {
    CreateCounter {
        archetype: info.archetype,
        obj_id: info.obj_id,
        field_08: info.field_44,
        owner_id: info.owner_id,
        sub_obj_id: info.sub_obj_id,
        orientation: wxyz(info.orientation),
        position: xyz(info.position),
        velocity: xyz(info.velocity),
        field_3c: xyz(info.field_38),
    }
}

/// rpclocal's `MarshalCreateGuided`.
#[must_use]
pub fn create_guided_packet(world: &World, info: &GuidedCreateInfo) -> CreateGuided {
    CreateGuided {
        archetype: world.data().archetypes.large_to_small(info.archetype),
        owner_id: info.owner_id,
        sub_obj_id: info.sub_obj_id,
        target_id: info.target_id,
        target_sub_obj_id: info.target_sub_obj_id,
        obj_id: info.obj_id,
        position: xyz(info.position),
        orientation: quantise_orientation(wxyz(info.orientation)),
    }
}

/// rpclocal's `MarshalCreateLoot`: the kind's low byte and one record per
/// container. The orientation's `x` and `y` go in each other's places, as
/// the C++ marshaller writes them.
#[must_use]
pub fn create_loot_packet(world: &World, request: &LootCreateRequest) -> CreateLoot {
    let archetypes = &world.data().archetypes;
    CreateLoot {
        owner_id: request.owner_id,
        kind: request.kind as u8,
        loot: request
            .items
            .iter()
            .map(|d| LootRecord {
                loot_id: d.loot_id,
                contents: archetypes.large_to_small(d.contents),
                contents_hit_points: d.contents_hit_pts,
                units: d.units as u16,
                container: archetypes.large_to_small(d.container),
                hit_points: d.hit_pts,
                name_override: d.name_override,
                position: xyz(d.position),
                velocity: xyz(d.velocity),
                orientation: [
                    d.orientation.w,
                    d.orientation.y,
                    d.orientation.x,
                    d.orientation.z,
                ],
                marked_by: u8::from(d.marked_by),
                temporary: u8::from(d.temporary),
            })
            .collect(),
    }
}

fn wire_damage_list(list: &DamageList) -> WireDamageList {
    WireDamageList {
        destroyed: u8::from(list.destroyed),
        cause: list.cause as u32,
        inflictor_id: list.inflictor_id,
        entries: list
            .entries
            .iter()
            .map(|x| WireDamageEntry {
                sub_obj_id: x.sub_obj_id,
                fate: x.fate.value() as u8,
                hit_points: x.hit_pts,
            })
            .collect(),
    }
}

fn send_damage(logic: &mut Logic, client: u32, object_id: u32, list: &DamageList) {
    send_server(
        logic,
        client,
        DamageObject {
            object_id,
            damage: wire_damage_list(list),
        },
    );
}

fn send_destroy(logic: &mut Logic, client: u32, object_id: u32, kind: i32) {
    send_server(
        logic,
        client,
        DestroyObject {
            object_id,
            field_04: kind.to_le_bytes()[0],
        },
    );
}

fn send_set_target(
    logic: &mut Logic,
    client: u32,
    object_id: u32,
    sub_obj_id: SubObjId,
    target_id: u32,
    target_sub_obj_id: SubObjId,
) {
    send_common(
        logic,
        client,
        SetTarget {
            object_id,
            target_id,
            sub_obj_id,
            target_sub_obj_id,
        },
    );
}

/// `GameObject::get_damage_list` and the `EqObj` override: the hull when
/// its relative health is strictly below the ceiling; for a ship or solar
/// also every item (a destroyed one as hit points 0 and fate 3, any other
/// below the ceiling with its hit points) and every collision group below
/// the ceiling, by id.
pub fn get_damage_list(world: &World, h: ObjHandle, out: &mut DamageList, ceiling: f32) {
    let Some(o) = world.get(h) else {
        return;
    };
    if o.is(class::SIMPLE) && o.simple.relative_health() < e(ceiling) {
        out.add_damage_entry(SUBOBJ_ID_ROOT, o.simple.hit_pts, Fate::None);
    }
    if !matches!(o.kind, ObjectKind::Ship | ObjectKind::Solar) {
        return;
    }
    let Some(eq) = o.equipped.as_deref() else {
        return;
    };
    for slot in eq.equipment.traverse(u32::MAX, true) {
        let Some(item) = eq.equipment.get(slot) else {
            continue;
        };
        if item.is_destroyed() {
            out.add_damage_entry(item.id, 0.0, Fate::HealthProxy);
            continue;
        }
        if item.relative_health() < e(ceiling) {
            out.add_damage_entry(item.id, item.hit_points(), Fate::None);
        }
    }
    let mut groups: Vec<&game_sim::object::damage::ArchGroup> = eq.arch_groups.iter().collect();
    groups.sort_by_key(|g| g.id);
    for g in groups {
        if g.relative_health() < e(ceiling) {
            out.add_damage_entry(g.id, g.hit_pts, Fate::None);
        }
    }
}

/// A damage list describing an object's state: cause 0x19, nothing
/// inflicted.
fn damage_state_list(world: &World, h: ObjHandle, ceiling: f32) -> DamageList {
    let mut list = DamageList::new(CAUSE_DAMAGE_STATE, 0, 0);
    list.destroyed = false;
    get_damage_list(world, h, &mut list, ceiling);
    list
}

// ---------------------------------------------------------------------------
// The observers of a system.

fn observer_count(cx: &Cx<'_>, system: u32) -> usize {
    cx.logic
        .server_world
        .systems
        .get(&system)
        .map_or(0, |s| s.observers.len())
}

fn observer<'c>(cx: &'c Cx<'_>, system: u32, index: usize) -> Option<&'c Observer> {
    cx.logic
        .server_world
        .systems
        .get(&system)
        .and_then(|s| s.observers.get(index))
}

fn observer_mut<'c>(cx: &'c mut Cx<'_>, system: u32, index: usize) -> Option<&'c mut Observer> {
    cx.logic
        .server_world
        .systems
        .get_mut(&system)
        .and_then(|s| s.observers.get_mut(index))
}

/// Takes an observer out of its system for work that also needs the rest
/// of the context; [`put_observer`] puts it back. Nothing in between
/// dispatches, so the list does not change meanwhile.
fn take_observer(cx: &mut Cx<'_>, system: u32, index: usize) -> Option<Observer> {
    observer_mut(cx, system, index).map(std::mem::take)
}

fn put_observer(cx: &mut Cx<'_>, system: u32, index: usize, view: Observer) {
    if let Some(slot) = observer_mut(cx, system, index) {
        *slot = view;
    }
}

/// The system an object is in, for its fan-outs.
fn system_of(world: &World, h: ObjHandle) -> Option<u32> {
    world.get(h).map(|o| o.physical.system)
}

// ---------------------------------------------------------------------------
// The per-player creates (`SystemPlayerView_Create*ForPlayer`).

fn report_dead(cx: &mut Cx<'_>, line: u32, what: &str, level: &str, id: u32) {
    cx.logic.reports.push(
        OBSERVER_CPP,
        line,
        format!("*** {level}: {what}({id}) 'dead' before creation packet sent."),
    );
}

/// `SystemPlayerView_CreateShipForPlayer`: never the player's own ship,
/// nothing once the player's ship has gone, a destroyed ship reported and
/// left out; otherwise the ship is tracked, and in single player the
/// record goes to the client with the player's vibe. A multiplayer client
/// hears of it from the per-frame pass.
pub fn create_ship_for_player(
    cx: &mut Cx<'_>,
    system: u32,
    index: usize,
    info: &ShipCreateInfo,
    h: ObjHandle,
) {
    let Some(view) = observer(cx, system, index) else {
        return;
    };
    let player = view.player;
    let owner = cx.world.get(h).map_or(0, |o| o.simple.owner_player);
    if owner == player || view.ship_gone {
        return;
    }
    if is_destroyed(cx, h) {
        let id = cx.world.get(h).map_or(info.base.obj_id, |o| o.simple.id);
        report_dead(cx, 0x19b, "Ship", "WARNING", id);
        return;
    }
    if let Some(mut view) = take_observer(cx, system, index) {
        add_tracked_object(cx.world, &mut view, h);
        put_observer(cx, system, index, view);
    }
    if !single_player(cx.world) {
        return;
    }
    let mut info = info.clone();
    info.vibe_other_group = rep_vibe(cx.logic, player);
    let packet = create_ship_packet(cx.world, &info);
    send_server(cx.logic, player, packet);
}

/// `SystemPlayerView_CreateSolarForPlayer`: nothing once the player's ship
/// has gone; the record goes to the client when the solar is visible or
/// dynamic. A solar is not tracked.
pub fn create_solar_for_player(
    cx: &mut Cx<'_>,
    system: u32,
    index: usize,
    info: &SolarCreateInfo,
    h: ObjHandle,
) {
    let Some(view) = observer(cx, system, index) else {
        return;
    };
    let (player, position) = (view.player, view.position);
    if view.ship_gone {
        return;
    }
    if !should_be_visible(cx.world, cx.logic, player, position, h) && !info.dynamic {
        return;
    }
    let packet = create_solar_packet(cx.world, info);
    send_server(cx.logic, player, packet);
}

/// The shared body of `SystemPlayerView_CreateMineForPlayer` and
/// `CreateCounterForPlayer`: nothing once the player's ship has gone, a
/// destroyed object reported; otherwise tracked, and created on the
/// client at once in single player or when visible.
fn create_projectile_for_player(
    cx: &mut Cx<'_>,
    system: u32,
    index: usize,
    h: ObjHandle,
    report: (u32, &str),
) -> Option<u32> {
    let view = observer(cx, system, index)?;
    if view.ship_gone {
        return None;
    }
    if is_destroyed(cx, h) {
        let id = cx.world.get(h).map_or(0, |o| o.simple.id);
        report_dead(cx, report.0, report.1, "WARNING", id);
        return None;
    }
    let mut view = take_observer(cx, system, index)?;
    let at = add_tracked_object(cx.world, &mut view, h);
    let visible = single_player(cx.world)
        || should_be_visible(cx.world, cx.logic, view.player, view.position, h);
    if visible {
        view.tracked[at].created_on_client = true;
    }
    let player = view.player;
    put_observer(cx, system, index, view);
    visible.then_some(player)
}

/// `SystemPlayerView_CreateMineForPlayer`.
pub fn create_mine_for_player(
    cx: &mut Cx<'_>,
    system: u32,
    index: usize,
    info: &ProjectileCreateInfo,
    h: ObjHandle,
) {
    if let Some(player) = create_projectile_for_player(cx, system, index, h, (0x28a, "Mine")) {
        let packet = create_mine_packet(cx.world, info);
        send_server(cx.logic, player, packet);
    }
}

/// `SystemPlayerView_CreateCounterForPlayer`.
pub fn create_counter_for_player(
    cx: &mut Cx<'_>,
    system: u32,
    index: usize,
    info: &ProjectileCreateInfo,
    h: ObjHandle,
) {
    if let Some(player) =
        create_projectile_for_player(cx, system, index, h, (0x23c, "CounterMeasure"))
    {
        send_server(cx.logic, player, create_counter_packet(info));
    }
}

/// `SystemPlayerView_CreateGuidedForPlayer`: nothing once the player's
/// ship has gone; the first object is tracked (no destroyed test), and
/// created on the client at once in single player or when visible.
pub fn create_guided_for_player(
    cx: &mut Cx<'_>,
    system: u32,
    index: usize,
    info: &GuidedCreateInfo,
    objects: &[ObjHandle],
) {
    let Some(&h) = objects.first() else {
        return;
    };
    let Some(mut view) = take_observer(cx, system, index) else {
        return;
    };
    if view.ship_gone {
        put_observer(cx, system, index, view);
        return;
    }
    let at = add_tracked_object(cx.world, &mut view, h);
    let visible = single_player(cx.world)
        || should_be_visible(cx.world, cx.logic, view.player, view.position, h);
    if visible {
        view.tracked[at].created_on_client = true;
    }
    let player = view.player;
    put_observer(cx, system, index, view);
    if visible {
        let packet = create_guided_packet(cx.world, info);
        send_server(cx.logic, player, packet);
    }
}

/// `SystemPlayerView_CreateLootForPlayer`: nothing once the player's ship
/// has gone; each container is tracked, but a destroyed one abandons the
/// call (reported) before the packet; the request's mark is the last
/// container's. Only single player sends the packet.
pub fn create_loot_for_player(
    cx: &mut Cx<'_>,
    system: u32,
    index: usize,
    request: &LootCreateRequest,
    objects: &[ObjHandle],
) {
    let Some(mut view) = take_observer(cx, system, index) else {
        return;
    };
    let player = view.player;
    if view.ship_gone {
        put_observer(cx, system, index, view);
        return;
    }
    for &h in objects {
        if is_destroyed(cx, h) {
            put_observer(cx, system, index, view);
            let id = cx.world.get(h).map_or(0, |o| o.simple.id);
            report_dead(cx, 0x256, "Loot", "ERROR", id);
            return;
        }
        add_tracked_object(cx.world, &mut view, h);
    }
    put_observer(cx, system, index, view);
    if !single_player(cx.world) {
        return;
    }
    let packet = create_loot_packet(cx.world, request);
    send_server(cx.logic, player, packet);
}

// ---------------------------------------------------------------------------
// The creators' fan-outs.

/// `StarSystem::create_ship`'s fan-out: every player of the system but the
/// owner gets [`create_ship_for_player`] with the record of the request,
/// marked for that player.
pub fn ship_created(cx: &mut Cx<'_>, system: u32, h: ObjHandle, info: &ShipCreateInfo) {
    let mut index = 0;
    while index < observer_count(cx, system) {
        let player = observer(cx, system, index).map_or(0, |v| v.player);
        if player != info.owner_player {
            let mut copy = info.clone();
            copy.marked_by = is_marked_by(cx.logic, h, player);
            create_ship_for_player(cx, system, index, &copy, h);
        }
        index += 1;
    }
}

/// `StarSystem::add_ship`'s fan-out: every player of the system gets
/// [`create_ship_for_player`] with the ship's record, marked for that
/// player.
pub fn ship_arrived(cx: &mut Cx<'_>, system: u32, h: ObjHandle) {
    let info = ship_create_info(cx.world, h).unwrap_or_default();
    let mut index = 0;
    while index < observer_count(cx, system) {
        let player = observer(cx, system, index).map_or(0, |v| v.player);
        let mut copy = info.clone();
        copy.marked_by = is_marked_by(cx.logic, h, player);
        create_ship_for_player(cx, system, index, &copy, h);
        index += 1;
    }
}

/// `StarSystem::create_solar`'s fan-out: every player gets
/// [`create_solar_for_player`] with the record, marked for that player
/// and with that player's vibe.
pub fn solar_created(cx: &mut Cx<'_>, system: u32, h: ObjHandle, info: &SolarCreateInfo) {
    let mut index = 0;
    while index < observer_count(cx, system) {
        let player = observer(cx, system, index).map_or(0, |v| v.player);
        let mut copy = info.clone();
        copy.marked_by = is_marked_by(cx.logic, h, player);
        copy.vibe_other_group = rep_vibe(cx.logic, player);
        create_solar_for_player(cx, system, index, &copy, h);
        index += 1;
    }
}

/// `StarSystem::create_mine`'s fan-out.
pub fn mine_created(cx: &mut Cx<'_>, system: u32, h: ObjHandle, info: &ProjectileCreateInfo) {
    let mut index = 0;
    while index < observer_count(cx, system) {
        create_mine_for_player(cx, system, index, info, h);
        index += 1;
    }
}

/// `StarSystem::create_counter_measure`'s fan-out.
pub fn counter_created(cx: &mut Cx<'_>, system: u32, h: ObjHandle, info: &ProjectileCreateInfo) {
    let mut index = 0;
    while index < observer_count(cx, system) {
        create_counter_for_player(cx, system, index, info, h);
        index += 1;
    }
}

/// `StarSystem::create_guided`'s fan-out: the record from the missile and
/// its launcher.
pub fn guided_created(cx: &mut Cx<'_>, system: u32, h: ObjHandle, launcher: SubObjId) {
    let Some(mut info) = guided_create_info(cx.world, h) else {
        return;
    };
    info.sub_obj_id = launcher;
    let mut index = 0;
    while index < observer_count(cx, system) {
        create_guided_for_player(cx, system, index, &info, &[h]);
        index += 1;
    }
}

/// `StarSystem::create_loot`'s fan-out: the request (the source, the
/// mode and each container's record, unmarked) to every player.
pub fn loot_created(cx: &mut Cx<'_>, system: u32, source: u32, mode: u32, made: &[ObjHandle]) {
    let request = LootCreateRequest {
        owner_id: source,
        kind: mode,
        items: made
            .iter()
            .filter_map(|&h| loot_desc(cx.world, h))
            .collect(),
    };
    let mut index = 0;
    while index < observer_count(cx, system) {
        create_loot_for_player(cx, system, index, &request, made);
        index += 1;
    }
}

// ---------------------------------------------------------------------------
// A player's arrival (`StarSystem::add_player`'s creates).

fn objects_of(world: &World, system: u32, kind: ObjectKind) -> Vec<ObjHandle> {
    world
        .objects_in(system)
        .into_iter()
        .filter(|&h| world.get(h).is_some_and(|o| o.kind == kind))
        .collect()
}

/// The creates `StarSystem::add_player` sends the player whose observer is
/// at `index`, after the multiplayer system id:
/// - multiplayer only, each solar: a dynamic one's record
///   (`CREATESOLAR`); then the target of each working gun (`SETTARGET`)
///   and each active docking point (`ACTIVATEOBJECT`);
/// - each ship: in single player [`create_ship_for_player`] with its
///   record; in multiplayer it is tracked unless destroyed;
/// - the loot, when there is any: in single player one `CREATELOOT` of
///   every container (owner 0, kind 5, each marked for the player,
///   [`create_loot_for_player`]); in multiplayer each container is
///   tracked unless destroyed.
///
/// Left to a later increment: a static solar's damage state below full
/// health (`DAMAGEOBJECT` in place of the create) and every solar's
/// `SETREPUTATION` before its targets.
pub fn add_player(cx: &mut Cx<'_>, system: u32, index: usize) {
    let Some(session) = observer(cx, system, index).map(|v| v.player) else {
        return;
    };
    let single = single_player(cx.world);
    if !single {
        for h in objects_of(cx.world, system, ObjectKind::Solar) {
            add_player_solar(cx, session, h);
        }
    }
    for h in objects_of(cx.world, system, ObjectKind::Ship) {
        if single {
            if let Some(mut info) = ship_create_info(cx.world, h) {
                info.marked_by = is_marked_by(cx.logic, h, session);
                create_ship_for_player(cx, system, index, &info, h);
            }
        } else if !is_destroyed(cx, h)
            && let Some(mut view) = take_observer(cx, system, index)
        {
            add_tracked_object(cx.world, &mut view, h);
            put_observer(cx, system, index, view);
        }
    }
    let loot = objects_of(cx.world, system, ObjectKind::Loot);
    if loot.is_empty() {
        return;
    }
    if single {
        let items: Vec<LootDesc> = loot
            .iter()
            .filter_map(|&h| {
                let mut desc = loot_desc(cx.world, h)?;
                desc.marked_by = is_marked_by(cx.logic, h, session);
                Some(desc)
            })
            .collect();
        let request = LootCreateRequest {
            owner_id: 0,
            kind: LOOT_KIND_OBSERVED,
            items,
        };
        create_loot_for_player(cx, system, index, &request, &loot);
    } else {
        for h in loot {
            if !is_destroyed(cx, h)
                && let Some(mut view) = take_observer(cx, system, index)
            {
                add_tracked_object(cx.world, &mut view, h);
                put_observer(cx, system, index, view);
            }
        }
    }
}

fn add_player_solar(cx: &mut Cx<'_>, session: u32, h: ObjHandle) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let dynamic = matches!(&o.data, KindData::Solar(s) if s.dynamic);
    let id = o.simple.id;
    if dynamic && let Some(mut info) = solar_create_info(cx.world, h) {
        info.marked_by = is_marked_by(cx.logic, h, session);
        let packet = create_solar_packet(cx.world, &info);
        send_server(cx.logic, session, packet);
    }
    let targets: Vec<(SubObjId, u32, SubObjId)> = cx
        .world
        .get(h)
        .and_then(|o| o.equipped.as_deref())
        .map(|eq| {
            let offline = eq.weapons_offline;
            eq.equipment
                .traverse(types::GUN, false)
                .into_iter()
                .filter_map(|slot| eq.equipment.get(slot))
                .filter(|item| item.is_functioning(offline))
                .filter_map(|item| match &item.kind {
                    EquipKind::Gun(g) => gun_target(cx.world, g).map(|t| (item.id, t, g.lock_sub)),
                    _ => None,
                })
                .collect()
        })
        .unwrap_or_default();
    for (gun, target, sub) in targets {
        send_set_target(cx.logic, session, id, gun, target, sub);
    }
    let (states, any) = cx.world.docking_activate_state(h);
    if any {
        for (index, active) in states.into_iter().enumerate() {
            if active {
                send_server(
                    cx.logic,
                    session,
                    ActivateObject {
                        object_id: id,
                        activate: 1,
                        field_08: index as u32,
                    },
                );
            }
        }
    }
}

/// `CEGun::GetTarget`'s object, as its id.
fn gun_target(world: &World, gun: &game_sim::equip::launcher::Gun) -> Option<u32> {
    gun.target.and_then(|t| world.get(t)).map(|t| t.simple.id)
}

// ---------------------------------------------------------------------------
// Destruction and damage.

/// `SystemPlayerView_OnObjectDestroyed`: the destroy packet carries the id
/// and the kind's low byte. An object the observer does not track is
/// announced when it is the player's own or a solar of the system file
/// ([`SOLAR_TYPE_BITS`]); a tracked one when the client was told of it,
/// and its record goes.
pub fn observer_object_destroyed(
    cx: &mut Cx<'_>,
    system: u32,
    index: usize,
    h: ObjHandle,
    kind: i32,
) {
    let Some((id, owner, type_bits)) = cx
        .world
        .get(h)
        .map(|o| (o.simple.id, o.simple.owner_player, o.simple.type_bits))
    else {
        return;
    };
    let Some(view) = observer_mut(cx, system, index) else {
        return;
    };
    let player = view.player;
    let send = match view.position_of_id(id) {
        None => owner == player || type_bits & SOLAR_TYPE_BITS != 0,
        Some(at) => view.tracked.remove(at).created_on_client,
    };
    if send {
        send_destroy(cx.logic, player, id, kind);
    }
}

/// `StarSystem::on_object_destroyed`'s observer part: every player of the
/// object's system hears of it ([`observer_object_destroyed`]).
pub fn object_destroyed(cx: &mut Cx<'_>, h: ObjHandle, kind: i32) {
    let Some(system) = system_of(cx.world, h) else {
        return;
    };
    let mut index = 0;
    while index < observer_count(cx, system) {
        observer_object_destroyed(cx, system, index, h, kind);
        index += 1;
    }
}

/// `StarSystem::remove_ship`'s observer part: every player of the system
/// but the owner hears of the ship as destroyed with kind 0.
pub fn ship_removed(cx: &mut Cx<'_>, h: ObjHandle) {
    let Some((system, owner)) = cx
        .world
        .get(h)
        .map(|o| (o.physical.system, o.simple.owner_player))
    else {
        return;
    };
    let mut index = 0;
    while index < observer_count(cx, system) {
        if observer(cx, system, index).is_some_and(|v| v.player != owner) {
            observer_object_destroyed(cx, system, index, h, 0);
        }
        index += 1;
    }
}

/// `SystemPlayerView_DamageObject`: the damage of an object that is
/// visible, or of a trade-lane ring, goes to the client. Otherwise, in
/// multiplayer, a solar's reduced list goes when it has entries and its
/// id waits for the fifth phase when not; anything else is dropped.
fn observer_damage_object(
    cx: &mut Cx<'_>,
    system: u32,
    index: usize,
    id: u32,
    damage: &DamageList,
    reduced: &DamageList,
) {
    let Some(h) = cx.world.find(id) else {
        return;
    };
    let Some(view) = observer(cx, system, index) else {
        return;
    };
    let player = view.player;
    let solar = cx.world.get(h).filter(|o| o.is(class::SOLAR));
    let ring = solar.is_some_and(|o| o.simple.type_bits & TRADELANE_RING != 0);
    let is_solar = solar.is_some();
    let visible = is_object_visible(cx.world, cx.logic, view, Some(h));
    let list = if visible || ring {
        damage
    } else {
        if single_player(cx.world) || !is_solar {
            return;
        }
        if reduced.entries.is_empty() {
            if let Some(view) = observer_mut(cx, system, index) {
                view.pending_damage.insert(id);
            }
            return;
        }
        reduced
    };
    send_damage(cx.logic, player, id, list);
}

/// `StarSystem_DamageObject` (`StarSystem::damage_object`): with entries,
/// a player's current ship first mirrors them into the player's record
/// (an item still there is re-described, one gone is removed, a collision
/// group takes its relative health, the hull the ship's relative health),
/// then every player of the system hears of the damage; a solar's
/// players get the list reduced to its destroyed and separated entries
/// when they cannot see it.
pub fn damage_object(cx: &mut Cx<'_>, h: ObjHandle, damage: &DamageList) {
    if damage.entries.is_empty() {
        return;
    }
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let (id, owner, system) = (o.simple.id, o.simple.owner_player, o.physical.system);
    let eq_obj = o.is(class::EQ_OBJ) && o.equipped.is_some();
    let is_solar = o.is(class::SOLAR);
    let mirror = eq_obj
        && cx
            .logic
            .players
            .get(owner)
            .is_some_and(|p| p.ship_object_id == id);
    if mirror {
        mirror_damage(cx, h, owner, damage);
    }
    let mut reduced = DamageList::default();
    if is_solar {
        reduced.entries = damage
            .entries
            .iter()
            .filter(|x| x.fate != Fate::None)
            .copied()
            .collect();
    }
    let mut index = 0;
    while index < observer_count(cx, system) {
        observer_damage_object(cx, system, index, id, damage, &reduced);
        index += 1;
    }
}

/// `StarSystem_DamageObject`'s first part.
fn mirror_damage(cx: &mut Cx<'_>, h: ObjHandle, session: u32, damage: &DamageList) {
    for entry in &damage.entries {
        let sub = entry.sub_obj_id;
        if is_equip_id(sub) {
            if entry.fate == Fate::None {
                let present = cx
                    .world
                    .get(h)
                    .and_then(|o| o.equipped.as_deref())
                    .is_some_and(|eq| eq.equipment.find_by_id(sub).is_some());
                if !present {
                    continue;
                }
                let Some(desc) = cx.world.equip_desc(h, sub) else {
                    continue;
                };
                crate::player::modify_equipment(
                    cx,
                    session,
                    desc.id,
                    desc.hardpoint,
                    desc.count,
                    desc.health,
                    desc.equipped,
                );
            } else {
                crate::player::remove_equipment(cx, session, sub, -1, false);
            }
        } else if is_arch_group_id(sub) {
            let health = cx
                .world
                .get(h)
                .and_then(|o| o.equipped.as_deref())
                .and_then(|eq| eq.arch_groups.iter().find(|g| g.id == sub))
                .map(|g| g.relative_health() as f32);
            let Some(health) = health else {
                continue;
            };
            if let Some(slot) = cx.logic.players.get_mut(session)
                && let Some(group) = slot.collision_groups.iter_mut().find(|g| g.id == sub)
            {
                group.health = health;
            }
        } else if sub == SUBOBJ_ID_ROOT {
            let health = cx
                .world
                .get(h)
                .map_or(0.0, |o| o.simple.relative_health() as f32);
            crate::player::set_relative_health(cx, session, health, false);
        }
    }
}

// ---------------------------------------------------------------------------
// The per-frame pass (`SystemPlayerView_Update`).

/// `StarSystem_Update`'s last step: every observer of the system runs
/// [`update`] with the frame's time, when the system has players.
pub fn update_observers(cx: &mut Cx<'_>, system: u32, dt: f64) {
    let mut index = 0;
    while index < observer_count(cx, system) {
        update(cx, system, index, dt);
        index += 1;
    }
}

/// `SystemPlayerView_Update` (`Observer::update`): nothing in single
/// player (the clock does not run either). Otherwise the clock advances,
/// the observer takes the position of the player's ship, and the five
/// phases run:
/// 1. the records split into those the client has, whose simulations
///    advance by `dt`, and those it has not;
/// 2. each record not on the client that is now visible gets its create
///    (loot, ship with its targets, counter-measure, mine, guided munition
///    with its target, in that order of tests) and is created, its
///    simulation reset;
/// 3. each record on the client that is no longer visible is destroyed on
///    it (kind 0) and becomes uncreated; an unowned rest whose simulation
///    asks for a sync is queued by simulation time, the longest first;
/// 4. the queue's updates go out, each also fed to the record's
///    simulation;
/// 5. the waiting damage of each object now visible goes out (cause 0x19,
///    the state below 2.0, i.e. everything) when there is any, and its id
///    leaves the set either way.
///
/// The link-saturation gate is asked before each phase and each send.
pub fn update(cx: &mut Cx<'_>, system: u32, index: usize, dt: f64) {
    if single_player(cx.world) {
        return;
    }
    let Some(mut view) = take_observer(cx, system, index) else {
        return;
    };
    run_update(cx, &mut view, dt);
    put_observer(cx, system, index, view);
}

fn run_update(cx: &mut Cx<'_>, view: &mut Observer, dt: f64) {
    let player = view.player;
    view.elapsed_time += dt;
    if let Some(h) = cx.world.find(ship_object_id(cx.logic, player))
        && let Some(o) = cx.world.get(h)
    {
        view.position = o.engine.position;
    }

    // Phase 1. A record whose object the world no longer has goes first.
    view.tracked.retain(|t| cx.world.get(t.object).is_some());
    let mut uncreated = Vec::new();
    let mut created = Vec::new();
    for (at, record) in view.tracked.iter_mut().enumerate() {
        if record.created_on_client {
            created.push(at);
            record.simulation.doit(dt);
        } else {
            uncreated.push(at);
        }
    }

    // Phase 2.
    if !has_bandwidth(cx.logic, player) {
        return;
    }
    for at in uncreated {
        let h = view.tracked[at].object;
        if !should_be_visible(cx.world, cx.logic, player, view.position, h) {
            continue;
        }
        if !has_bandwidth(cx.logic, player) {
            return;
        }
        send_tracked_create(cx, player, h);
        let record = &mut view.tracked[at];
        record.created_on_client = true;
        created.push(at);
        record.simulation.reset();
    }

    // Phase 3.
    let mut pending: Vec<usize> = Vec::new();
    for at in created {
        if !has_bandwidth(cx.logic, player) {
            return;
        }
        let h = view.tracked[at].object;
        let Some(o) = cx.world.get(h) else {
            continue;
        };
        let (id, owner, position, orientation) = (
            o.simple.id,
            o.simple.owner_player,
            o.engine.position,
            o.engine.orientation,
        );
        if !should_be_visible(cx.world, cx.logic, player, view.position, h) {
            send_destroy(cx.logic, player, id, 0);
            view.tracked[at].created_on_client = false;
            continue;
        }
        let orient = quat(&orientation);
        if owner != 0 {
            continue;
        }
        if !view.tracked[at]
            .simulation
            .check_for_sync(view.position, position, orient)
        {
            continue;
        }
        let ours = view.tracked[at].simulation.simulation_time();
        let mut place = 0;
        while place < pending.len() {
            if view.tracked[pending[place]].simulation.simulation_time() <= ours {
                break;
            }
            place += 1;
        }
        pending.insert(place, at);
    }

    // Phase 4.
    for at in pending {
        if !has_bandwidth(cx.logic, player) {
            return;
        }
        let h = view.tracked[at].object;
        let Some(o) = cx.world.get(h) else {
            continue;
        };
        let position = o.engine.position;
        let orient = quat(&o.engine.orientation);
        let (throttle, behavior) = match (o.as_ship(), o.equipped.as_deref()) {
            (Some(ship), eq) => (
                ship.flight.throttle,
                eq.map_or(-1, |e| e.behavior_index).to_le_bytes()[0],
            ),
            (None, _) => (0.0, 0xff),
        };
        let message = UpdateObject {
            flags: behavior,
            object_id: o.simple.id,
            position: xyz(position),
            orientation: quantise_orientation(wxyz(orient)),
            extra: quantise_unit(throttle),
            update_time: view.elapsed_time as f32,
        };
        send_common(cx.logic, player, message);
        view.tracked[at]
            .simulation
            .remote_update(position, orient, view.elapsed_time);
    }

    // Phase 5.
    if !has_bandwidth(cx.logic, player) {
        return;
    }
    let retry: Vec<ObjHandle> = view
        .pending_damage
        .iter()
        .filter_map(|&id| cx.world.find(id))
        .filter(|&h| should_be_visible(cx.world, cx.logic, player, view.position, h))
        .collect();
    for h in retry {
        if !has_bandwidth(cx.logic, player) {
            return;
        }
        let id = cx.world.get(h).map_or(0, |o| o.simple.id);
        let list = damage_state_list(cx.world, h, 2.0);
        if !list.entries.is_empty() {
            send_damage(cx.logic, player, id, &list);
        }
        view.pending_damage.remove(&id);
    }
}

/// Phase 2's create of one tracked object, by the first class it has:
/// loot, ship, counter-measure, mine, guided munition. Anything else
/// sends nothing.
fn send_tracked_create(cx: &mut Cx<'_>, player: u32, h: ObjHandle) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    if o.is(class::LOOT) {
        let owner_id = match &o.data {
            KindData::Loot(l) => l.owner_id,
            _ => 0,
        };
        let Some(mut desc) = loot_desc(cx.world, h) else {
            return;
        };
        desc.marked_by = is_marked_by(cx.logic, h, player);
        let request = LootCreateRequest {
            owner_id,
            kind: LOOT_KIND_OBSERVED,
            items: vec![desc],
        };
        let packet = create_loot_packet(cx.world, &request);
        send_server(cx.logic, player, packet);
    } else if o.is(class::SHIP) {
        let Some(mut info) = ship_create_info(cx.world, h) else {
            return;
        };
        info.vibe_other_group = rep_vibe(cx.logic, player);
        info.marked_by = is_marked_by(cx.logic, h, player);
        let packet = create_ship_packet(cx.world, &info);
        send_server(cx.logic, player, packet);
        send_ship_targets(cx, player, h);
    } else if o.is(class::COUNTER_MEASURE) {
        if let Some(info) = projectile_create_info(cx.world, h) {
            send_server(cx.logic, player, create_counter_packet(&info));
        }
    } else if o.is(class::MINE) {
        if let Some(info) = projectile_create_info(cx.world, h) {
            let packet = create_mine_packet(cx.world, &info);
            send_server(cx.logic, player, packet);
        }
    } else if o.is(class::GUIDED) {
        let Some(info) = guided_create_info(cx.world, h) else {
            return;
        };
        let packet = create_guided_packet(cx.world, &info);
        send_server(cx.logic, player, packet);
        if info.target_id != 0 {
            send_set_target(
                cx.logic,
                player,
                info.obj_id,
                SUBOBJ_ID_NONE,
                info.target_id,
                info.target_sub_obj_id,
            );
        }
    }
}

/// `SendShipTargets`: after a ship's create, its target, then each gun's
/// (the walk ends at the first item that is not a gun).
fn send_ship_targets(cx: &mut Cx<'_>, player: u32, h: ObjHandle) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let ship_id = o.simple.id;
    let mut targets: Vec<(SubObjId, u32, SubObjId)> = Vec::new();
    if let Some(ship) = o.as_ship()
        && let Some(target) = ship.target.and_then(|t| cx.world.get(t))
    {
        targets.push((SUBOBJ_ID_NONE, target.simple.id, ship.sub_target));
    }
    if let Some(eq) = o.equipped.as_deref() {
        for slot in eq.equipment.traverse(types::GUN, false) {
            let Some(item) = eq.equipment.get(slot) else {
                break;
            };
            let EquipKind::Gun(g) = &item.kind else {
                break;
            };
            if let Some(target) = gun_target(cx.world, g) {
                targets.push((item.id, target, g.lock_sub));
            }
        }
    }
    for (sub, target, target_sub) in targets {
        send_set_target(cx.logic, player, ship_id, sub, target, target_sub);
    }
}
