//! `VMeshWire` (`VMeshWire.cpp`): the engine component that keeps a
//! wireframe (`VWireData`) per archetype and draws it as a line list over
//! the vertices of the library mesh it names.

use std::collections::BTreeMap;
use std::rc::Rc;

use neuron_core::mesh_data::part::{self, WireError};
use neuron_core::mesh_data::vmesh::VWireData;
use neuron_core::vfs::FileSystem;

use super::services::MeshServices;
use super::vmesh::VMeshRegistry;
use super::{Archetype, Instance, MeshIid, dump, file};

/// The scratch index buffer's handle (`(void*)-1`).
pub const SCRATCH_IB: u32 = u32::MAX;

/// The engine component: one shared wireframe per archetype.
#[derive(Clone, Debug, Default)]
pub struct VMeshWire {
    archetypes: BTreeMap<Archetype, Rc<VWireData>>,
}

impl VMeshWire {
    /// No archetypes.
    #[must_use]
    pub fn new() -> VMeshWire {
        VMeshWire::default()
    }

    /// The wireframe of an archetype.
    #[must_use]
    pub fn wire(&self, archetype: Archetype) -> Option<&Rc<VWireData>> {
        self.archetypes.get(&archetype)
    }

    /// `CreateArchetype(archetype, fs)`: 0 without a `VMeshWire` node or
    /// when its `VWireData` does not read (an error notice); else the
    /// wireframe replaces the archetype's and the result is `OpenNode`'s,
    /// 1.
    pub fn create_archetype<S: MeshServices + ?Sized>(
        &mut self,
        services: &mut S,
        archetype: Archetype,
        fs: &mut dyn FileSystem,
    ) -> i32 {
        let Ok(Some(loaded)) = part::load_wire(fs) else {
            return 0;
        };
        match loaded {
            Ok(wire) => {
                self.archetypes.insert(archetype, Rc::new(wire));
                1
            }
            Err(WireError::ReadFailed | WireError::Parse(_)) => {
                services.dump(
                    dump::ERROR,
                    file::VMESH_WIRE,
                    0x87,
                    "couldnt load vmeshwire persistant wire_data; bad version?",
                );
                0
            }
        }
    }

    /// `DuplicateArchetype(new, old)`: the new archetype shares the old
    /// one's wireframe. 0.
    pub fn duplicate_archetype(&mut self, new: Archetype, old: Archetype) -> i32 {
        if let Some(shared) = self.archetypes.get(&old).cloned() {
            self.archetypes.insert(new, shared);
        }
        0
    }

    /// `DestroyArchetype(archetype)`. 0.
    pub fn destroy_archetype(&mut self, archetype: Archetype) -> i32 {
        self.archetypes.remove(&archetype);
        0
    }

    /// `QueryArchetype(archetype, iid)`: only `IID_IVMeshWire` of a known
    /// archetype.
    #[must_use]
    pub fn query_archetype(&self, archetype: Archetype, iid: MeshIid) -> Option<Rc<VWireData>> {
        if iid != MeshIid::VMeshWire {
            return None;
        }
        self.archetypes.get(&archetype).cloned()
    }

    /// `QueryInstance(instance, iid)`: the instance's archetype's.
    pub fn query_instance<S: MeshServices + ?Sized>(
        &self,
        services: &mut S,
        instance: Instance,
        iid: MeshIid,
    ) -> Option<Rc<VWireData>> {
        let archetype = services.instance_archetype(instance);
        if archetype == -1 {
            return None;
        }
        services.release_handle(archetype);
        self.query_archetype(archetype, iid)
    }
}

/// `IVMeshWire::Draw`: the mesh of the wireframe's CRC (-8 without one),
/// its group's vertex buffer made current (-1 without a group or when that
/// fails), the indices copied to the scratch index buffer and drawn as a
/// line list from the mesh's base vertex plus the wireframe's bias.
pub fn draw_wire<S: MeshServices + ?Sized>(
    wire: &VWireData,
    registry: &mut VMeshRegistry,
    services: &mut S,
) -> i32 {
    let Some(id) = registry.find(wire.mesh_crc) else {
        return -8;
    };
    let Some(group) = registry.entry(id).and_then(|e| e.group) else {
        return -1;
    };
    if registry.ensure_vb(services, group) < 0 {
        return -1;
    }
    let Some(entry) = registry.entry(id) else {
        return -1;
    };
    let base = entry.base_vertex;
    let count = usize::from(wire.index_count).min(wire.indices.len());
    let start = services.copy_scratch_indices(&wire.indices[..count]);
    services.select_ib(SCRATCH_IB, u32::from(wire.index_bias).wrapping_add(base));
    services.draw_indexed(
        2,
        0,
        u32::from(wire.vertex_count),
        start,
        u32::from(wire.index_count),
    )
}

/// `IVMeshWire::GetMeshData`: the mesh of the wireframe's CRC.
#[must_use]
pub fn wire_mesh(wire: &VWireData, registry: &VMeshRegistry) -> Option<usize> {
    registry.find(wire.mesh_crc)
}
