//! A system's solar partition (`StarSystem::Spatial`,
//! `Server/server_classes.cpp:6823-6859`): the BSP `StarSystem::load`
//! builds over every solar of the system's list (`NewSpatial("BSP")`,
//! `AddArray` of `{solar, position, radius}` in list order), to which
//! `create_solar` adds and from which `remove_solar` removes. It is the
//! bucket the system's object database answers for any mask
//! (`StarSystem::GetBucket`, `:11594`, which ignores the mask), so Common's
//! queries reach it through the world: the scanner's solars
//! (`ScanList::add_in_range`, mask `0xfdff`), the pathfinder's planets,
//! moons and suns (`ClassifyObstaclePhase`, `GetBucket(7)`), the server's
//! near-ship pass (`StarSystem_UpdateNearShips`) and
//! `pub::System::ScanObjects`. A system without one (not loaded by the
//! server, and no driver built it) answers no bucket (`GetBucket` null).

use neuron_core::math::Vector;

use super::World;
use crate::data::spatial::{BspPartition, QueryResult, SpatialEntry, SpatialPartition};
use crate::data::zone::query::Sphere;
use crate::object::{ObjHandle, ObjectKind};

/// A system's solar BSP: the partition, whose handles are indices into
/// `solars` plus one.
#[derive(Clone, Debug)]
pub struct SolarPartition {
    tree: SpatialPartition,
    solars: Vec<ObjHandle>,
}

impl SolarPartition {
    /// The partition's sphere query (`QuerySphere`, +0x1c): the partition
    /// handles of the solars whose spheres the sphere overlaps, appended
    /// to `found` while it has room, in the tree's order
    /// ([`SolarPartition::solar`] maps them back).
    pub fn query_sphere(&self, sphere: &Sphere, found: &mut QueryResult) {
        self.tree.query_sphere(sphere, found);
    }

    /// The solar of a partition handle.
    #[must_use]
    pub fn solar(&self, item: u32) -> Option<ObjHandle> {
        let index = usize::try_from(item).ok()?.checked_sub(1)?;
        self.solars.get(index).copied()
    }

    /// The partition handle of a solar the tree holds (0 for none).
    fn handle_of(&self, h: ObjHandle) -> u32 {
        self.solars
            .iter()
            .position(|&s| s == h)
            .and_then(|i| u32::try_from(i + 1).ok())
            .unwrap_or(0)
    }
}

impl World {
    /// `StarSystem::load`'s spatial partition: a BSP over every solar of
    /// the system's list, each `{solar, position, radius}` (`AddArray` in
    /// list order). It replaces the system's earlier one.
    pub fn build_solar_partition(&mut self, system: u32) {
        let solars: Vec<ObjHandle> = self
            .objects_in(system)
            .into_iter()
            .filter(|&h| self.get(h).is_some_and(|o| o.kind == ObjectKind::Solar))
            .collect();
        let entries: Vec<SpatialEntry> = solars
            .iter()
            .enumerate()
            .filter_map(|(index, &h)| {
                let o = self.get(h)?;
                Some(SpatialEntry {
                    object: u32::try_from(index + 1).unwrap_or(u32::MAX),
                    position: o.engine.position,
                    radius: o.engine.radius,
                })
            })
            .collect();
        let mut tree = SpatialPartition::Bsp(BspPartition::default());
        tree.add_array(&entries);
        self.solar_partitions
            .insert(system, SolarPartition { tree, solars });
    }

    /// `StarSystem::create_solar`'s single `Add`: the solar `{solar,
    /// position, radius}` joins the system's partition (none for a system
    /// without one).
    pub fn add_to_solar_partition(&mut self, system: u32, h: ObjHandle) {
        let Some((position, radius)) = self.get(h).map(|o| (o.engine.position, o.engine.radius))
        else {
            return;
        };
        let Some(partition) = self.solar_partitions.get_mut(&system) else {
            return;
        };
        partition.solars.push(h);
        partition.tree.add(SpatialEntry {
            object: u32::try_from(partition.solars.len()).unwrap_or(u32::MAX),
            position,
            radius,
        });
    }

    /// `StarSystem::remove_solar`'s `Remove` (`Solar::destroy`): the solar
    /// leaves its system's partition, whose node moves its last used slot
    /// into the solar's place. Nothing for a solar the partition does not
    /// hold or a system without one.
    pub fn remove_from_solar_partition(&mut self, system: u32, h: ObjHandle) {
        let Some(partition) = self.solar_partitions.get_mut(&system) else {
            return;
        };
        let item = partition.handle_of(h);
        if item != 0 {
            partition.tree.remove(item);
        }
    }

    /// `StarSystem::unload`'s `FreeSpatial`: the system's partition goes.
    pub fn free_solar_partition(&mut self, system: u32) {
        self.solar_partitions.remove(&system);
    }

    /// The system's partition (`GetBucket`), if one was built.
    #[must_use]
    pub fn solar_partition(&self, system: u32) -> Option<&SolarPartition> {
        self.solar_partitions.get(&system)
    }

    /// `GetBucket(mask)->QuerySphere({center, radius})` into a result of
    /// [`RESULT_CAPACITY`](crate::data::spatial::RESULT_CAPACITY): the
    /// solars whose spheres the sphere overlaps, in the partition's order;
    /// `None` when the system has no partition (`GetBucket` null).
    #[must_use]
    pub fn solars_in_sphere(
        &self,
        system: u32,
        center: Vector,
        radius: f32,
    ) -> Option<Vec<ObjHandle>> {
        let partition = self.solar_partitions.get(&system)?;
        let mut found = QueryResult::default();
        partition.query_sphere(&Sphere { center, radius }, &mut found);
        Some(
            found
                .items
                .into_iter()
                .filter_map(|item| partition.solar(item))
                .collect(),
        )
    }
}
