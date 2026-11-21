//! `pub::System` (`Server/server_pub.cpp:6624-6935`): loading a star
//! system, its solars by nickname and in a sphere (over the server's
//! systems, [`crate::world`], and the shared world's object lists), and
//! the universe's view of it (its name, connections, `[Object]` records,
//! zones and zone queries).
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `LoadSystem` | 6666 | [`load_system`] |
//! | `Find` | 6685 | [`find`] |
//! | `GetName` | 6699 | [`name`] |
//! | `GetNestedProperties` | 6712 | [`nested_properties`] |
//! | `EnumerateConnections` | 6732 | [`enumerate_connections`] |
//! | `EnumerateObjects` | 6769 | [`enumerate_objects`] |
//! | `EnumerateZones` | 6814 | [`enumerate_zones`] |
//! | `ScanObjects` | 6848 | [`scan_objects`] |
//! | `InZones(position, radius)` | 6868 | [`in_zones`] |
//! | `InZones(transform, angles, distance)` | 6906 | [`in_zones_frustum`] |
//!
//! `ScanObjects` reads the solar BSP `StarSystem::load` builds
//! (`server_classes.cpp:6823-6859`, `NewSpatial("BSP")` over every solar of
//! the list, never updated afterwards): [`build_solar_tree`] makes it when
//! a system loads and keeps it in [`SolarTrees`]. The C++ builds the zone
//! tree once per system; the port builds it again for each query
//! ([`game_sim::data::universe::Universe::zone_tree`]), which gives the
//! same tree.

// The C++'s `!(x > 0.0f)` tests send NaN to the failing side; the port
// spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use std::collections::BTreeMap;

use game_sim::data::DataSource;
use game_sim::data::spatial::{BspPartition, QueryResult, SpatialEntry, SpatialPartition};
use game_sim::data::zone::query::{Frustum, Sphere};
use game_sim::object::{ObjHandle, ObjectKind};
use neuron_core::id::create_id;
use neuron_core::ini::{IniReader, IniState};
use neuron_core::math::{Transform, Vector};

use super::PubError;
use super::misc::get_system;
use crate::server::Cx;

/// The solar BSP of a loaded system (`StarSystem::Spatial`): the
/// partition, whose handles are indices into `solars` plus one.
#[derive(Clone, Debug)]
pub struct SolarTree {
    tree: SpatialPartition,
    solars: Vec<ObjHandle>,
}

/// The loaded systems' solar BSPs, by system.
#[derive(Clone, Debug, Default)]
pub struct SolarTrees {
    trees: BTreeMap<u32, SolarTree>,
}

impl SolarTrees {
    /// The tree of a system, if it was built.
    #[must_use]
    pub fn get(&self, system: u32) -> Option<&SolarTree> {
        self.trees.get(&system)
    }

    /// `StarSystem::unload`'s `FreeSpatial`: the system's tree goes.
    pub fn remove(&mut self, system: u32) {
        self.trees.remove(&system);
    }
}

/// The objects of one kind in a system, in their list's order.
fn list_of(cx: &Cx<'_>, system: u32, kind: ObjectKind) -> Vec<ObjHandle> {
    cx.world
        .objects_in(system)
        .into_iter()
        .filter(|&h| cx.world.get(h).is_some_and(|o| o.kind == kind))
        .collect()
}

/// `StarSystem::load`'s spatial partition: a BSP over every solar of the
/// system's list, each `{solar, position, radius}` (`AddArray` in list
/// order). It replaces the system's earlier tree.
pub fn build_solar_tree(cx: &mut Cx<'_>, system: u32) {
    let solars = list_of(cx, system, ObjectKind::Solar);
    let entries: Vec<SpatialEntry> = solars
        .iter()
        .enumerate()
        .filter_map(|(index, &h)| {
            let o = cx.world.get(h)?;
            Some(SpatialEntry {
                object: u32::try_from(index + 1).unwrap_or(u32::MAX),
                position: o.engine.position,
                radius: o.engine.radius,
            })
        })
        .collect();
    let mut tree = SpatialPartition::Bsp(BspPartition::default());
    tree.add_array(&entries);
    cx.logic
        .solar_trees
        .trees
        .insert(system, SolarTree { tree, solars });
}

/// `StarSystem::create_solar`'s single `Add` to the spatial partition: the
/// solar `{solar, position, radius}` joins the system's tree (none for a
/// system without one).
pub fn add_to_solar_tree(cx: &mut Cx<'_>, system: u32, h: ObjHandle) {
    let Some((position, radius)) = cx
        .world
        .get(h)
        .map(|o| (o.engine.position, o.engine.radius))
    else {
        return;
    };
    let Some(tree) = cx.logic.solar_trees.trees.get_mut(&system) else {
        return;
    };
    tree.solars.push(h);
    tree.tree.add(SpatialEntry {
        object: u32::try_from(tree.solars.len()).unwrap_or(u32::MAX),
        position,
        radius,
    });
}

/// `LoadSystem`: the system loads (`StarSystem::add_player(0)`, which
/// loads it and stops at the session id 0).
///
/// # Errors
///
/// -1 for a system the server does not have.
pub fn load_system(cx: &mut Cx<'_>, system: u32) -> Result<(), PubError> {
    crate::world::initialize(cx);
    if !cx.logic.server_world.systems.contains_key(&system) {
        return Err(PubError::FAILED);
    }
    crate::world::add_player(cx, system, 0);
    Ok(())
}

/// `Find`: the id of the system's solar whose nickname is `name`
/// (`StarSystem::find_solar(CreateID(name))`, the first in the list).
///
/// # Errors
///
/// -4 for an empty name or a system id whose low 30 bits are 0; -2 for a
/// system the server does not have; -3 when no solar has the nickname (a
/// system that is not loaded has none).
pub fn find(cx: &mut Cx<'_>, system: u32, name: &[u8]) -> Result<u32, PubError> {
    if name.first().is_none_or(|&b| b == 0) || system & 0x3fff_ffff == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    crate::world::initialize(cx);
    if !cx.logic.server_world.systems.contains_key(&system) {
        return Err(PubError::BAD_ID);
    }
    let solar = crate::dock::find_solar(cx, system, create_id(name)).ok_or(PubError::NOT_FOUND)?;
    cx.world
        .get(solar)
        .map(|o| o.simple.id)
        .ok_or(PubError::NOT_FOUND)
}

/// `GetName`: the system's `strid_name`.
///
/// # Errors
///
/// -2 for an unknown system (reported; the C++ writes 0 first).
pub fn name(cx: &mut Cx<'_>, system: u32) -> Result<u32, PubError> {
    let data = cx.world.data_arc();
    get_system(&data, &mut cx.logic.reports, system)
        .map(|s| s.strid_name as u32)
        .ok_or(PubError::BAD_ID)
}

/// `GetNestedProperties`: the OR of the property flags of every zone the
/// point (a sphere of radius 0) is in (`ISystem::GetNestedProperties`).
///
/// # Errors
///
/// -2 for an unknown system.
pub fn nested_properties(cx: &mut Cx<'_>, system: u32, position: Vector) -> Result<u32, PubError> {
    let data = cx.world.data_arc();
    get_system(&data, &mut cx.logic.reports, system).ok_or(PubError::BAD_ID)?;
    let sphere = Sphere {
        center: position,
        radius: 0.0,
    };
    Ok(data.universe.nested_properties(system, &sphere))
}

/// One entry of `ConnectionEnumerator`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ConnectionRecord {
    /// `GotoSystemId`.
    pub goto_system: u32,
    /// `Object`: the object's own nickname (0 without one).
    pub object: u32,
    /// `ObjectId`: the nickname, carried from an earlier object when it
    /// has none.
    pub object_id: u32,
    /// `Pos`.
    pub pos: Vector,
}

/// `EnumerateConnections`: the system's jump connections in file order,
/// all of them for type 0, only jump gates for 1, only jump holes for 2,
/// none for any other type. The C++ appends to the caller's array without
/// a bound.
///
/// # Errors
///
/// -2 for an unknown system.
pub fn enumerate_connections(
    cx: &mut Cx<'_>,
    system: u32,
    connection_type: u32,
) -> Result<Vec<ConnectionRecord>, PubError> {
    let data = cx.world.data_arc();
    let sys = get_system(&data, &mut cx.logic.reports, system).ok_or(PubError::BAD_ID)?;
    Ok(sys
        .connections
        .iter()
        .filter(|c| match connection_type {
            0 => true,
            1 | 2 => c.gate_type.value() == connection_type,
            _ => false,
        })
        .map(|c| ConnectionRecord {
            goto_system: c.goto_system,
            object: c.object,
            object_id: c.object_id,
            pos: c.pos,
        })
        .collect())
}

/// `FSysObj`: what `EnumerateObjects` hands the enumerator for each
/// `[Object]` of the system's file.
#[derive(Clone, Debug, PartialEq)]
pub struct SysObj {
    /// `Nickname`: `strncpy` of 32 bytes, so a nickname of 32 bytes or more
    /// is cut there and has no terminator.
    pub nickname: Vec<u8>,
    /// `Pos`.
    pub pos: Vector,
    /// `ArchetypeId`.
    pub archetype: u32,
    /// `IdsName`.
    pub ids_name: i32,
    /// `IdsInfo`.
    pub ids_info: i32,
    /// `Reputation`, at most 31 bytes.
    pub reputation: Vec<u8>,
    /// `DockWith`.
    pub dock_with: u32,
    /// `Goto`.
    pub goto: u32,
    /// `SystemId`.
    pub system: u32,
}

/// `EnumerateObjects`: each `[Object]` of the system's file
/// (`ISystem::GetSystemObject`, read from the file again) until the
/// enumerator answers false. A file that does not open has no objects.
///
/// # Errors
///
/// -2 for an unknown system.
pub fn enumerate_objects(
    cx: &mut Cx<'_>,
    source: &dyn DataSource,
    system: u32,
    mut enumerator: impl FnMut(&SysObj) -> bool,
) -> Result<(), PubError> {
    let data = cx.world.data_arc();
    let sys = get_system(&data, &mut cx.logic.reports, system).ok_or(PubError::BAD_ID)?;
    let objects =
        data.universe
            .system_objects(source, sys, cx.world.strings(), &mut cx.logic.reports);
    for record in objects {
        let object = SysObj {
            nickname: record.nickname.iter().copied().take(0x20).collect(),
            pos: record.pos,
            archetype: record.archetype,
            ids_name: record.ids_name,
            ids_info: record.ids_info,
            reputation: record.reputation.iter().copied().take(0x20).collect(),
            dock_with: record.dock_with,
            goto: record.goto,
            system,
        };
        if !enumerator(&object) {
            break;
        }
    }
    Ok(())
}

/// The `[Zone]` sections of a system file that gave the system its zones,
/// in file order: the reader's state at each header and the zone's id.
/// A section counts when its id (its last `nickname` or `name`) is the
/// next of the system's zone list, as the loader inserted only those.
fn zone_sections(reader: &mut IniReader, zones: &[u32]) -> Vec<(IniState, u32)> {
    let mut out = Vec::new();
    let mut next = zones.iter().copied().peekable();
    while reader.read_header() {
        if !reader.is_header(b"Zone") {
            continue;
        }
        let state = reader.state();
        let mut id = 0;
        while reader.read_value() {
            if reader.is_value(b"nickname") || reader.is_value(b"name") {
                id = create_id(&reader.value_string());
            }
        }
        if id != 0 && next.peek() == Some(&id) {
            next.next();
            out.push((state, id));
        }
    }
    out
}

/// `EnumerateZones`: the system's file opened, then for each of its zones
/// in file order the reader put at the zone's `[Zone]` header and the
/// enumerator called with the zone's id and the reader (its answer is not
/// read).
///
/// # Errors
///
/// -2 for an unknown system or a file that does not open.
pub fn enumerate_zones(
    cx: &mut Cx<'_>,
    source: &dyn DataSource,
    system: u32,
    mut enumerator: impl FnMut(u32, &mut IniReader),
) -> Result<(), PubError> {
    let data = cx.world.data_arc();
    let sys = get_system(&data, &mut cx.logic.reports, system).ok_or(PubError::BAD_ID)?;
    let path = data
        .universe
        .system_file_path(cx.world.strings().get(sys.file));
    let mut reader = game_sim::data::open_ini(source, &path).ok_or(PubError::BAD_ID)?;
    let sections = zone_sections(&mut reader, &sys.zones);
    for (state, id) in sections {
        reader.set_state(&state);
        enumerator(id, &mut reader);
    }
    Ok(())
}

/// `GameObject::is_destroyed`: the server's latch, or an object the world
/// marked dead.
fn is_destroyed(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.logic.objects.get(h).is_some_and(|o| o.destroyed) || cx.world.get(h).is_none_or(|o| o.dead)
}

/// `ObjectList::collect_in_sphere`: the ids of the list's objects of a type
/// in `type_mask` (not destroyed, when the mask asks for ships) whose world
/// position is within `radius` (every one for a negative radius), at most
/// `max`.
fn collect_in_sphere(
    cx: &Cx<'_>,
    list: &[ObjHandle],
    max: usize,
    position: Vector,
    radius: f32,
    type_mask: u32,
) -> Vec<u32> {
    let radius_sq = (f64::from(radius) * f64::from(radius)) as f32;
    let mut out = Vec::new();
    if max == 0 {
        return out;
    }
    for &h in list {
        let Some(o) = cx.world.get(h) else {
            continue;
        };
        if !o.is(game_sim::object::class::SIMPLE) || o.simple.type_bits & type_mask == 0 {
            continue;
        }
        if type_mask & 0x7f_0000 != 0 && is_destroyed(cx, h) {
            continue;
        }
        let keep = if f64::from(radius) < 0.0 {
            true
        } else {
            let p = o.engine.position;
            let dx = f64::from(position.x) - f64::from(p.x);
            let dy = f64::from(position.y) - f64::from(p.y);
            let dz = f64::from(position.z) - f64::from(p.z);
            (dx * dx + dz * dz) + dy * dy <= f64::from(radius_sq)
        };
        if keep {
            out.push(o.simple.id);
            if out.len() >= max {
                break;
            }
        }
    }
    out
}

/// `ScanObjects` (`StarSystem::scan_objects`): at most `max` ids. Ships
/// first when the mask has a ship bit (`0x7f0000`); then, when it has a
/// bit of `0xfdff`, the solars: for a radius that is not positive the
/// solar list itself, otherwise the solar BSP's answers for the sphere
/// whose type is in the mask. A system that is not loaded has no BSP. A
/// solar the BSP holds but the world no longer has is skipped, where the
/// C++ reads its freed wrapper (D675).
///
/// # Errors
///
/// -2 for a system the server does not have (no id mask test).
pub fn scan_objects(
    cx: &mut Cx<'_>,
    system: u32,
    max: u32,
    position: Vector,
    radius: f32,
    type_mask: u32,
) -> Result<Vec<u32>, PubError> {
    crate::world::initialize(cx);
    if !cx.logic.server_world.systems.contains_key(&system) {
        return Err(PubError::BAD_ID);
    }
    let max = max as usize;
    let mut out = Vec::new();
    if type_mask & 0x7f_0000 != 0 {
        let ships = list_of(cx, system, ObjectKind::Ship);
        out = collect_in_sphere(cx, &ships, max, position, radius, type_mask);
    }
    if type_mask & 0xfdff == 0 {
        return Ok(out);
    }
    if !(radius > 0.0) {
        let solars = list_of(cx, system, ObjectKind::Solar);
        let left = max - out.len();
        out.extend(collect_in_sphere(
            cx, &solars, left, position, radius, type_mask,
        ));
        return Ok(out);
    }
    let Some(tree) = cx.logic.solar_trees.get(system) else {
        return Ok(out);
    };
    let sphere = Sphere {
        center: position,
        radius,
    };
    let mut found = QueryResult::default();
    tree.tree.query_sphere(&sphere, &mut found);
    for item in found.items {
        if out.len() >= max {
            break;
        }
        let Some(&h) = usize::try_from(item)
            .ok()
            .and_then(|i| tree.solars.get(i.checked_sub(1)?))
        else {
            continue;
        };
        let Some(o) = cx.world.get(h) else {
            continue;
        };
        if o.simple.type_bits & type_mask == 0 {
            continue;
        }
        out.push(o.simple.id);
    }
    Ok(out)
}

/// The zones of the system's zone tree that `query` finds and `culled`
/// keeps, at most `max`, as their ids.
fn zones_found(
    cx: &Cx<'_>,
    system: u32,
    max: u32,
    query: impl FnOnce(&SpatialPartition, &mut QueryResult),
    culled: impl Fn(&game_sim::data::zone::Zone) -> bool,
) -> Vec<u32> {
    let data = cx.world.data();
    let (tree, zones) = data.universe.zone_tree(system);
    let mut hits = QueryResult::default();
    query(&tree, &mut hits);
    let mut out = Vec::new();
    for item in hits.items {
        if out.len() >= max as usize {
            break;
        }
        let Some(zone) = usize::try_from(item)
            .ok()
            .and_then(|i| zones.get(i.checked_sub(1)?))
        else {
            continue;
        };
        if culled(zone) {
            continue;
        }
        out.push(zone.nickname);
    }
    out
}

/// `InZones(position, radius)`: the zones the sphere reaches (the zone
/// tree's answers that `SphereCull` keeps), at most `max`, in the tree's
/// order.
///
/// # Errors
///
/// -4 for a negative radius (a NaN radius passes); -2 for an unknown
/// system.
pub fn in_zones(
    cx: &mut Cx<'_>,
    system: u32,
    position: Vector,
    radius: f32,
    max: u32,
) -> Result<Vec<u32>, PubError> {
    if radius < 0.0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let data = cx.world.data_arc();
    get_system(&data, &mut cx.logic.reports, system).ok_or(PubError::BAD_ID)?;
    let sphere = Sphere {
        center: position,
        radius,
    };
    Ok(zones_found(
        cx,
        system,
        max,
        |tree, hits| {
            tree.query_sphere(&sphere, hits);
        },
        |zone| zone.sphere_cull(&sphere),
    ))
}

/// `InZones(transform, angle_x, angle_y, distance)`: the zones the frustum
/// reaches (the zone tree's answers for its bounding sphere that
/// `FrustumCull` keeps), at most `max`. No argument is checked.
///
/// # Errors
///
/// -2 for an unknown system.
pub fn in_zones_frustum(
    cx: &mut Cx<'_>,
    system: u32,
    transform: Transform,
    angle_x: f32,
    angle_y: f32,
    distance: f32,
    max: u32,
) -> Result<Vec<u32>, PubError> {
    let data = cx.world.data_arc();
    get_system(&data, &mut cx.logic.reports, system).ok_or(PubError::BAD_ID)?;
    let frustum = Frustum {
        xform: transform,
        angle_x,
        angle_y,
        distance,
    };
    Ok(zones_found(
        cx,
        system,
        max,
        |tree, hits| {
            tree.query_frustum(&frustum, hits);
        },
        |zone| zone.frustum_cull(&frustum),
    ))
}
