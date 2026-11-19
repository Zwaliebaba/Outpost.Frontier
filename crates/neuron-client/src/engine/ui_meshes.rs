//! The interface's control meshes through the engine (`ClientMeshLoadFromPath`
//! of `client_mesh_load.cpp` and the mesh holder's instance calls): a
//! `.cmp` or `.3db` file of the data becomes a scene archetype (its
//! libraries loaded first, as [`super::Engine::create_model`] loads them)
//! and an instance tree the control owns; the engine's UI draw submits it
//! through the render manager (`engine::ui_host`).

use std::cell::RefCell;
use std::rc::Rc;

use neuron_core::hash::name_crc32;
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::scene::{ArchetypeId, Hardpoints, InstanceId};
use neuron_core::vfs::{FsDesc, Host, UtfFileSystem};

use crate::render::ff::device::Backend;
use crate::render::mesh::vmesh::SharedRegistry;
use crate::ui::control::{ControlMesh, MeshLoader};

use super::{Loaded, RenderSide, SharedLinks, SharedScene, apply_extents, load_libraries};

/// [`MeshLoader`] over the engine's scene, render side and data.
pub struct EngineMeshLoader<B: Backend + 'static> {
    pub(super) data: Host,
    pub(super) registry: SharedRegistry,
    pub(super) render: Rc<RefCell<RenderSide<B>>>,
    pub(super) scene: SharedScene,
    pub(super) links: SharedLinks,
    pub(super) loaded: Rc<RefCell<Loaded>>,
    /// The meshes made so far.
    pub(super) made: Rc<std::cell::Cell<usize>>,
}

impl<B: Backend + 'static> EngineMeshLoader<B> {
    fn open(&self, path: &[u8]) -> Option<UtfFileSystem> {
        UtfFileSystem::open(&self.data, &FsDesc::read(path).with_implementation(b"UTF")).ok()
    }

    fn libraries(&self, path: &[u8]) {
        if let Some(fs) = self.open(path) {
            load_libraries(
                &self.render,
                &self.registry,
                &mut self.loaded.borrow_mut(),
                fs,
            );
        }
    }
}

impl<B: Backend + 'static> MeshLoader for EngineMeshLoader<B> {
    /// `ClientMeshLoadFromPath`: an archetype of the same name is bound
    /// again; otherwise the file's libraries (and the material path's)
    /// load and the scene engine makes the archetype. `None` when the file
    /// gives no instance.
    fn load(&mut self, path: &[u8], material_path: &[u8]) -> Option<Box<dyn ControlMesh>> {
        if path.is_empty() {
            return None;
        }
        let name = path
            .rsplit(|&c| c == b'\\' || c == b'/')
            .next()
            .unwrap_or(path)
            .to_vec();
        let found = self.scene.borrow_mut().find_archetype(name_crc32(&name));
        let archetype = match found {
            Some(a) => a,
            None => {
                if !material_path.is_empty() {
                    self.libraries(material_path);
                }
                self.libraries(path);
                let mut fs = self.open(path)?;
                let made = self
                    .scene
                    .borrow_mut()
                    .create_archetype_handle(&name, Some(&mut fs))
                    .ok()
                    .flatten()?;
                self.links.borrow_mut().take_made();
                made
            }
        };
        let root = self.scene.borrow_mut().create_instance(archetype);
        self.links.borrow_mut().take_made();
        apply_extents(&self.scene, &self.links);
        let Some(root) = root else {
            self.scene.borrow_mut().release_handle(archetype);
            return None;
        };
        self.made.set(self.made.get() + 1);
        Some(Box::new(EngineMesh {
            scene: Rc::clone(&self.scene),
            root,
            archetype,
        }))
    }
}

/// A control's instance tree in the engine's scene (the mesh holder); the
/// instance is destroyed and the archetype released when it drops.
pub struct EngineMesh {
    scene: SharedScene,
    root: InstanceId,
    archetype: ArchetypeId,
}

impl EngineMesh {
    fn update(&self) {
        if let Ok(mut scene) = self.scene.try_borrow_mut() {
            scene.update_instance(Some(self.root), 0);
        }
    }
}

impl ControlMesh for EngineMesh {
    fn transform(&self) -> Transform {
        self.scene.try_borrow().map_or(Transform::IDENTITY, |s| {
            s.instance_transform(Some(self.root))
        })
    }

    fn set_position(&mut self, position: Vector) {
        self.put_position(position);
        self.update();
    }

    fn set_orientation(&mut self, orientation: Matrix) {
        self.put_orientation(orientation);
        self.update();
    }

    fn set_transform(&mut self, transform: Transform) {
        if let Ok(mut scene) = self.scene.try_borrow_mut() {
            scene.set_instance_matrix(Some(self.root), &transform);
        }
        self.update();
    }

    fn hardpoint(&self, name: &[u8]) -> Option<Transform> {
        let scene = self.scene.try_borrow().ok()?;
        let data = scene
            .component::<Hardpoints>()?
            .get(self.archetype, name_crc32(name))?;
        Some(Transform::new(data.orientation(), data.position()))
    }

    fn radius(&self, flags: u32) -> f32 {
        self.scene
            .try_borrow_mut()
            .map_or(0.0, |mut s| s.instance_extent(Some(self.root), flags).0)
    }

    fn put_orientation(&mut self, orientation: Matrix) {
        if let Ok(mut scene) = self.scene.try_borrow_mut() {
            scene.set_instance_orientation(Some(self.root), &orientation);
        }
    }

    fn put_position(&mut self, position: Vector) {
        if let Ok(mut scene) = self.scene.try_borrow_mut() {
            scene.set_instance_position(Some(self.root), position);
        }
    }

    fn engine_instance(&self) -> Option<u32> {
        Some(self.root.index())
    }
}

impl Drop for EngineMesh {
    fn drop(&mut self) {
        if let Ok(mut scene) = self.scene.try_borrow_mut() {
            scene.destroy_instance(self.root);
            scene.release_handle(self.archetype);
        }
    }
}
