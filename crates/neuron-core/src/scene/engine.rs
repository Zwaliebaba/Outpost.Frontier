//! The `Engine` component of `CppSource/SceneRuntime/SceneRuntime.cpp`:
//! archetypes (compound models and their parts), instances in a
//! parent/child tree, joints and transforms.
//!
//! # Archetypes
//!
//! [`Engine::create_archetype_handle`] is `CreateArchetypeHandle`: an
//! archetype is named by the CRC of its name and shared by every caller
//! that names it again (with a reference count). Its file system either
//! holds a `Cmpnd` node, which makes it a compound, or is handed to every
//! [`EngineComponent`] (the hardpoints, the client's renderers).
//!
//! A `Cmpnd` node holds `Root` and `Part*` directories, each with an
//! `Object name` and a `File name` (at most 63 bytes each). The file name
//! is opened inside the compound's own file system, or else through the
//! engine's search path, and becomes an archetype of its own. `Cmpnd\Cons`
//! holds one file per joint type (`Fix`, `Pris`, `Rev`, `Cyl`, `Sphere`,
//! `Trans`, `Loose`) of fixed-size records: the parent's and the child's
//! object names (0x40 bytes each) and the type's data (see [`joint`]).
//!
//! # Instances
//!
//! An instance of a compound is a tree: one instance per part, connected
//! by the constraints; the root is the top of the last constraint's
//! parent. Each instance keeps its local transform (relative to its
//! parent, derived from its joint) and its world transform (the
//! reference's engine-owned `IInstanceTransform`).
//! [`Engine::update_instance`] composes world transforms down the tree.
//!
//! Instances and archetypes are arena slots named by ids where the
//! reference uses pointers. The root-instance map is keyed by the id, so
//! [`Engine::find_mapped_instance`] walks the roots in creation order
//! rather than by address (divergence register).
//!
//! [`joint`]: super::joint

use std::any::Any;
use std::collections::{BTreeMap, BTreeSet};

use crate::diag::{DumpFlags, dump};
use crate::hash::{c_str, name_crc32};
use crate::math::{Matrix, Transform, Vector, inverse_transform, mul_transform, transform_xform};
use crate::vfs::{FileSystem, FindToken, FsDesc, VfsError};

use super::joint::{JointKind, JointPayload, RECORD_HEADER_BYTES, RECORD_NAME_BYTES, joint_local};
use super::{SceneError, open_node, soft};

/// An archetype (the reference's `FArchetypeHandle*`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ArchetypeId(u32);

impl ArchetypeId {
    /// The arena index.
    #[must_use]
    pub const fn index(self) -> u32 {
        self.0
    }
}

/// An instance (the reference's `FEngineInstance*`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct InstanceId(u32);

impl InstanceId {
    /// The arena index.
    #[must_use]
    pub const fn index(self) -> u32 {
        self.0
    }

    /// The id with arena index `index`.
    #[must_use]
    pub const fn from_index(index: u32) -> Self {
        Self(index)
    }
}

/// A part of a compound (`FArchetypeChild`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PartChild {
    /// `Object name`, cut at its first NUL.
    pub name: Vec<u8>,
    /// The CRC of `name`.
    pub crc: u32,
    /// The part's archetype.
    pub archetype: ArchetypeId,
    /// `File name`, cut at its first NUL (not kept by the reference; the
    /// part's archetype is named after it).
    pub file_name: Vec<u8>,
}

/// A joint between two parts (`FConstraintRecord`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Constraint {
    /// The child part: an index into [`Archetype::children`].
    pub child: usize,
    /// The parent part.
    pub parent: usize,
    /// The joint.
    pub payload: JointPayload,
}

/// An archetype (`FArchetypeHandle`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Archetype {
    /// The name it was created under.
    pub name: Vec<u8>,
    /// The key it is shared by.
    pub crc: u32,
    /// The reference count.
    pub ref_count: u32,
    /// The parts, in `Cmpnd` find order (`Root` first).
    pub children: Vec<PartChild>,
    /// The joints, in `Cons` find and record order.
    pub constraints: Vec<Constraint>,
}

impl Archetype {
    /// `IsCompoundArchetype`.
    #[must_use]
    pub fn is_compound(&self) -> bool {
        !self.children.is_empty()
    }

    /// `FindArchetypeChildByCrc`: the first part whose name has `crc`.
    #[must_use]
    pub fn child_by_crc(&self, crc: u32) -> Option<usize> {
        self.children.iter().position(|c| c.crc == crc)
    }
}

/// A component the engine hands its archetypes and instances to
/// (`IEngineComponent`). The engine calls them in the order they were
/// added, as it walks its `[Engine]` list.
pub trait EngineComponent: core::fmt::Debug {
    /// `CreateArchetype(handle, fs)` for an archetype that is not a
    /// compound.
    ///
    /// # Errors
    ///
    /// A malformed archive.
    fn create_archetype(
        &mut self,
        archetype: ArchetypeId,
        fs: &mut dyn FileSystem,
    ) -> Result<(), SceneError>;

    /// `DuplicateArchetype(new, old)`.
    fn duplicate_archetype(&mut self, new: ArchetypeId, old: ArchetypeId) {
        let _ = (new, old);
    }

    /// `DestroyArchetype(handle)`.
    fn destroy_archetype(&mut self, archetype: ArchetypeId) {
        let _ = archetype;
    }

    /// `CreateInstance(instance, handle)`.
    fn create_instance(&mut self, instance: InstanceId, archetype: ArchetypeId) {
        let _ = (instance, archetype);
    }

    /// `DestroyInstance(instance)`.
    fn destroy_instance(&mut self, instance: InstanceId) {
        let _ = instance;
    }

    /// `UpdateInstance(instance, arg)`.
    fn update_instance(&mut self, instance: InstanceId) {
        let _ = instance;
    }

    /// `Notify(arg)`.
    fn notify(&mut self) {}

    /// The component, for [`Engine::component`].
    fn as_any(&self) -> &dyn Any;

    /// The component, for [`Engine::component_mut`].
    fn as_any_mut(&mut self) -> &mut dyn Any;
}

/// The search path a part file falls back on when the compound's own file
/// system does not hold it (`CreateFileSystem` with no factory).
pub type SearchPathFn = dyn Fn(&FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError>;

/// The engine-owned transform of an instance (`FInstanceTransform`): its
/// world transform, the extent it was given and a user word.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct InstanceTransform {
    /// The world transform.
    pub matrix: Transform,
    /// `Extra`: the extent radius `SetInstanceExtent(.., 1, ..)` stores.
    pub extra: f32,
    /// `ExtraVec`: its centre.
    pub extra_vec: Vector,
    /// `User`.
    pub user: u32,
}

impl Default for InstanceTransform {
    fn default() -> Self {
        Self {
            matrix: Transform::IDENTITY,
            extra: 0.0,
            extra_vec: Vector::ZERO,
            user: 0,
        }
    }
}

/// An instance (`FEngineInstance`).
#[derive(Clone, Debug, PartialEq)]
pub struct Instance {
    /// Its archetype.
    pub archetype: ArchetypeId,
    /// The compound it is a part of, and its index in the compound's parts.
    pub part: Option<(ArchetypeId, usize)>,
    /// The parent.
    pub parent: Option<InstanceId>,
    next_sibling: Option<InstanceId>,
    prev_sibling: Option<InstanceId>,
    first_child: Option<InstanceId>,
    last_child: Option<InstanceId>,
    /// The number of children.
    pub child_count: u32,
    /// `SetInstanceUserData`.
    pub user_data: usize,
    /// The local transform: from the joint, relative to the parent.
    pub local: Transform,
    /// The joint to the parent (`Relative`): type -1 for none.
    pub relative: JointPayload,
    /// The joint parameters (`JointState`).
    pub joint_state: [f32; 7],
    /// Whether the joint has to be applied again.
    pub world_dirty: bool,
    /// The cached extent radius; -1 when it is to be computed.
    pub extent: f32,
    /// The cached extent centre, local.
    pub extent_vec: Vector,
    /// The world transform and its companions.
    pub transform: InstanceTransform,
}

impl Instance {
    fn new(archetype: ArchetypeId) -> Self {
        Self {
            archetype,
            part: None,
            parent: None,
            next_sibling: None,
            prev_sibling: None,
            first_child: None,
            last_child: None,
            child_count: 0,
            user_data: 0,
            local: Transform::IDENTITY,
            relative: JointPayload::default(),
            joint_state: [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0],
            world_dirty: true,
            extent: -1.0,
            extent_vec: Vector::ZERO,
            transform: InstanceTransform::default(),
        }
    }

    /// The first child.
    #[must_use]
    pub fn first_child(&self) -> Option<InstanceId> {
        self.first_child
    }

    /// The next sibling.
    #[must_use]
    pub fn next_sibling(&self) -> Option<InstanceId> {
        self.next_sibling
    }
}

const ANONYMOUS_LIMIT: u32 = u32::MAX;

/// The engine.
#[derive(Default)]
pub struct Engine {
    archetypes: Vec<Option<Archetype>>,
    by_crc: BTreeMap<u32, ArchetypeId>,
    instances: Vec<Option<Instance>>,
    roots: BTreeSet<InstanceId>,
    components: Vec<Box<dyn EngineComponent>>,
    search_path: Option<Box<SearchPathFn>>,
    anonymous_index: u32,
}

impl core::fmt::Debug for Engine {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Engine")
            .field("archetypes", &self.archetypes.len())
            .field("instances", &self.instances.len())
            .field("components", &self.components)
            .finish_non_exhaustive()
    }
}

/// `ReadPartNameField`: at most 63 bytes, and the read must report them
/// all.
fn read_part_name(fs: &dyn FileSystem, field: &[u8]) -> Result<Option<Vec<u8>>, SceneError> {
    let Some(mut file) = soft(fs.open_file(&FsDesc::read(field)))? else {
        return Ok(None);
    };
    let size = soft(file.size())?.unwrap_or(u32::MAX).min(0x3F) as usize;
    let mut buf = vec![0; size];
    Ok(match soft(file.read(&mut buf))? {
        Some(got) if got == size => Some(c_str(&buf).to_vec()),
        _ => None,
    })
}

const PART_FAIL: &str =
    "Engine: create_compound_archetype: unable to create part archetype, ignoring...";

impl Engine {
    /// An engine without components or search path.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Adds a component at the end of the `[Engine]` list.
    pub fn add_component(&mut self, component: Box<dyn EngineComponent>) {
        self.components.push(component);
    }

    /// The first component of type `T`.
    #[must_use]
    pub fn component<T: 'static>(&self) -> Option<&T> {
        self.components
            .iter()
            .find_map(|c| c.as_any().downcast_ref::<T>())
    }

    /// The first component of type `T`.
    pub fn component_mut<T: 'static>(&mut self) -> Option<&mut T> {
        self.components
            .iter_mut()
            .find_map(|c| c.as_any_mut().downcast_mut::<T>())
    }

    /// `SetSearchPath` / `SetFilesystem`: where part files that the
    /// compound does not hold are opened.
    pub fn set_search_path(&mut self, search: Option<Box<SearchPathFn>>) {
        self.search_path = search;
    }

    // --- archetypes ---------------------------------------------------------------------

    /// Every archetype that exists, in id order.
    pub fn archetypes(&self) -> impl Iterator<Item = (ArchetypeId, &Archetype)> {
        self.archetypes
            .iter()
            .enumerate()
            .filter_map(|(i, a)| Some((ArchetypeId(i as u32), a.as_ref()?)))
    }

    /// Every instance that exists, in id order.
    pub fn instances(&self) -> impl Iterator<Item = (InstanceId, &Instance)> {
        self.instances
            .iter()
            .enumerate()
            .filter_map(|(i, a)| Some((InstanceId(i as u32), a.as_ref()?)))
    }

    /// The archetype `id`, while it exists.
    #[must_use]
    pub fn archetype(&self, id: ArchetypeId) -> Option<&Archetype> {
        self.archetypes.get(id.0 as usize)?.as_ref()
    }

    fn archetype_mut(&mut self, id: ArchetypeId) -> Option<&mut Archetype> {
        self.archetypes.get_mut(id.0 as usize)?.as_mut()
    }

    /// `FindArchetype` without its reference: the archetype with name CRC
    /// `crc` (`LookupArchetype`; 0 finds nothing).
    #[must_use]
    pub fn lookup_archetype(&self, crc: u32) -> Option<ArchetypeId> {
        if crc == 0 {
            return None;
        }
        self.by_crc.get(&crc).copied()
    }

    /// `FindArchetype`: as [`Engine::lookup_archetype`], taking a reference.
    pub fn find_archetype(&mut self, crc: u32) -> Option<ArchetypeId> {
        let id = self.lookup_archetype(crc)?;
        self.add_ref_handle(id);
        Some(id)
    }

    /// `AddRefHandle`: one more reference to the archetype and to each of
    /// its parts.
    pub fn add_ref_handle(&mut self, id: ArchetypeId) -> u32 {
        let Some(archetype) = self.archetype_mut(id) else {
            return 0;
        };
        archetype.ref_count = archetype.ref_count.wrapping_add(1);
        let count = archetype.ref_count;
        let parts: Vec<ArchetypeId> = archetype.children.iter().map(|c| c.archetype).collect();
        for part in parts {
            self.add_ref_handle(part);
        }
        count
    }

    /// `ReleaseHandle`: one reference less to the archetype and to each of
    /// its parts; an archetype with none left is destroyed (its components
    /// first).
    pub fn release_handle(&mut self, id: ArchetypeId) -> u32 {
        let Some(archetype) = self.archetype_mut(id) else {
            return 0;
        };
        archetype.ref_count = archetype.ref_count.wrapping_sub(1);
        let count = archetype.ref_count;
        let parts: Vec<ArchetypeId> = archetype.children.iter().map(|c| c.archetype).collect();
        for part in parts {
            self.release_handle(part);
        }
        if count != 0 {
            return count;
        }
        for component in &mut self.components {
            component.destroy_archetype(id);
        }
        if let Some(archetype) = self.archetypes[id.0 as usize].take() {
            if self.by_crc.get(&archetype.crc) == Some(&id) {
                self.by_crc.remove(&archetype.crc);
            } else {
                dump(
                    DumpFlags::ERROR,
                    &format!(
                        "Failed to find {} in archetype name map\n",
                        String::from_utf8_lossy(&archetype.name)
                    ),
                );
            }
        }
        0
    }

    /// `CreateCompoundArchetype(name, crc)`: a new, empty archetype with one
    /// reference. A CRC of 0 makes an `anonymous-<n>` name that no
    /// archetype has yet. An existing archetype of the same CRC stays in
    /// the name map (the reference's `insert` does not replace it).
    pub fn create_compound_archetype(&mut self, name: &[u8], crc: u32) -> ArchetypeId {
        let (name, crc) = if crc != 0 {
            (name.to_vec(), crc)
        } else {
            loop {
                let candidate = format!("anonymous-{}", self.anonymous_index).into_bytes();
                self.anonymous_index = self.anonymous_index.wrapping_add(1);
                let crc = name_crc32(&candidate);
                if self.lookup_archetype(crc).is_none() || self.anonymous_index == ANONYMOUS_LIMIT {
                    break (candidate, crc);
                }
            }
        };
        let id = ArchetypeId(u32::try_from(self.archetypes.len()).unwrap_or(u32::MAX));
        self.archetypes.push(Some(Archetype {
            name,
            crc,
            ref_count: 0,
            children: Vec::new(),
            constraints: Vec::new(),
        }));
        self.by_crc.entry(crc).or_insert(id);
        self.add_ref_handle(id);
        id
    }

    /// `CreateArchetypeHandle(name, fs)`: the archetype named `name`
    /// (shared and referenced again when it exists), loaded from `fs`.
    /// `None` (the reference's -1) for a name whose CRC is 0, and for a
    /// new name without a file system.
    ///
    /// # Errors
    ///
    /// A malformed archive.
    pub fn create_archetype_handle(
        &mut self,
        name: &[u8],
        fs: Option<&mut dyn FileSystem>,
    ) -> Result<Option<ArchetypeId>, SceneError> {
        let name = c_str(name);
        let crc = name_crc32(name);
        if crc == 0 {
            return Ok(None);
        }
        if let Some(existing) = self.lookup_archetype(crc) {
            self.add_ref_handle(existing);
            return Ok(Some(existing));
        }
        let Some(fs) = fs else {
            return Ok(None);
        };
        let id = self.create_compound_archetype(name, crc);
        let compound = soft(fs.create_child(&FsDesc::read(b"Cmpnd")))?;
        match compound {
            Some(mut cmpnd) => self.parse_cmpnd_children(id, cmpnd.as_mut(), fs)?,
            None => {
                for index in 0..self.components.len() {
                    self.components[index].create_archetype(id, fs)?;
                }
            }
        }
        Ok(Some(id))
    }

    /// `ParseCmpndChildren`.
    fn parse_cmpnd_children(
        &mut self,
        id: ArchetypeId,
        cmpnd: &mut dyn FileSystem,
        parent: &mut dyn FileSystem,
    ) -> Result<(), SceneError> {
        match self.link_part(b"Root", None, cmpnd, parent)? {
            Some(child) => self.push_child(id, child),
            None => dump(DumpFlags::WARNING, PART_FAIL),
        }
        for entry in super::find(cmpnd, b"Part*")? {
            match self.link_part(&entry.data.name, entry.token, cmpnd, parent)? {
                Some(child) => self.push_child(id, child),
                None => dump(DumpFlags::WARNING, PART_FAIL),
            }
        }
        self.parse_cons(id, cmpnd)
    }

    fn push_child(&mut self, id: ArchetypeId, child: PartChild) {
        if let Some(archetype) = self.archetype_mut(id) {
            archetype.children.push(child);
        }
    }

    /// `LinkCmpndPartHandle`.
    fn link_part(
        &mut self,
        name: &[u8],
        token: Option<FindToken>,
        cmpnd: &dyn FileSystem,
        parent: &mut dyn FileSystem,
    ) -> Result<Option<PartChild>, SceneError> {
        let desc = FsDesc::read(name).with_found(token);
        let Some(part) = soft(cmpnd.create_child(&desc))? else {
            return Ok(None);
        };
        let object = read_part_name(part.as_ref(), b"Object name")?;
        let file = read_part_name(part.as_ref(), b"File name")?;
        drop(part);
        let (Some(object), Some(file)) = (object, file) else {
            return Ok(None);
        };
        let desc = FsDesc::read(&file);
        let mut file_fs = match soft(parent.create_child(&desc))? {
            Some(fs) => fs,
            None => match &self.search_path {
                Some(search) => match soft(search(&desc))? {
                    Some(fs) => fs,
                    None => return Ok(None),
                },
                None => return Ok(None),
            },
        };
        let Some(archetype) = self.create_archetype_handle(&file, Some(file_fs.as_mut()))? else {
            return Ok(None);
        };
        Ok(Some(PartChild {
            crc: name_crc32(&object),
            name: object,
            archetype,
            file_name: file,
        }))
    }

    /// `ParseConsConstraints`.
    fn parse_cons(&mut self, id: ArchetypeId, fs: &mut dyn FileSystem) -> Result<(), SceneError> {
        if !open_node(fs, b"Cons")? {
            return Ok(());
        }
        for entry in super::find(fs, b"*")? {
            let name = entry.data.name;
            if name.first() == Some(&b'.') {
                continue;
            }
            let desc = FsDesc::read(&name).with_found(entry.token);
            let Some(mut file) = soft(fs.open_file(&desc))? else {
                continue;
            };
            let size = soft(file.size())?.unwrap_or(u32::MAX);
            let Some(kind) = JointKind::from_name(&name) else {
                // The reference then divides the size by a record size of 0.
                dump(
                    DumpFlags::WARNING,
                    "Engine: unknown joint type, probably random garbage",
                );
                continue;
            };
            let record_size = kind.record_size();
            for _ in 0..size as usize / record_size {
                let mut record = vec![0; record_size];
                if soft(file.read(&mut record))? != Some(record_size) {
                    continue;
                }
                let first = c_str(&record);
                let second = c_str(&record[RECORD_NAME_BYTES..]);
                let Some(archetype) = self.archetype(id) else {
                    return Ok(());
                };
                let parent = archetype.child_by_crc(name_crc32(first));
                let child = archetype.child_by_crc(name_crc32(second));
                match (parent, child) {
                    (Some(parent), Some(child)) => {
                        let payload =
                            JointPayload::from_record(kind, &record[RECORD_HEADER_BYTES..]);
                        if let Some(archetype) = self.archetype_mut(id) {
                            archetype.constraints.push(Constraint {
                                child,
                                parent,
                                payload,
                            });
                        }
                    }
                    _ => dump(
                        DumpFlags::WARNING,
                        &format!(
                            "Joint {}<->{} not found!",
                            String::from_utf8_lossy(first),
                            String::from_utf8_lossy(second)
                        ),
                    ),
                }
            }
        }
        let _ = open_node(fs, b"..")?;
        Ok(())
    }

    /// `DuplicateArchetype(handle, name)`: a new archetype under `name`
    /// that shares `source`'s parts and constraints; the components copy
    /// their data. `None` when `source` does not exist.
    pub fn duplicate_archetype(
        &mut self,
        source: ArchetypeId,
        name: &[u8],
        crc: u32,
    ) -> Option<ArchetypeId> {
        let (children, constraints) = {
            let s = self.archetype(source)?;
            (s.children.clone(), s.constraints.clone())
        };
        let id = self.create_compound_archetype(name, crc);
        let parts: Vec<ArchetypeId> = children.iter().map(|c| c.archetype).collect();
        if let Some(archetype) = self.archetype_mut(id) {
            archetype.children = children;
            archetype.constraints = constraints;
        }
        for part in parts {
            self.add_ref_handle(part);
        }
        for component in &mut self.components {
            component.duplicate_archetype(id, source);
        }
        Some(id)
    }

    // --- instances ----------------------------------------------------------------------

    /// The instance `id`, while it exists.
    #[must_use]
    pub fn instance(&self, id: InstanceId) -> Option<&Instance> {
        self.instances.get(id.0 as usize)?.as_ref()
    }

    /// The instance `id`, while it exists.
    pub fn instance_mut(&mut self, id: InstanceId) -> Option<&mut Instance> {
        self.instances.get_mut(id.0 as usize)?.as_mut()
    }

    fn get(&self, id: InstanceId) -> &Instance {
        self.instance(id)
            .unwrap_or_else(|| unreachable!("live instance"))
    }

    fn get_mut(&mut self, id: InstanceId) -> &mut Instance {
        self.instance_mut(id)
            .unwrap_or_else(|| unreachable!("live instance"))
    }

    fn alloc_instance(&mut self, archetype: ArchetypeId) -> InstanceId {
        let id = InstanceId(u32::try_from(self.instances.len()).unwrap_or(u32::MAX));
        self.instances.push(Some(Instance::new(archetype)));
        id
    }

    /// The roots of instance trees the engine keeps (`Instances`), in id
    /// order.
    pub fn roots(&self) -> impl Iterator<Item = InstanceId> + '_ {
        self.roots.iter().copied()
    }

    /// `CreateInstance(handle, null)`: an instance of a compound is the
    /// root of a tree of its parts; any other archetype makes one
    /// instance. `None` (the reference's -1) for a missing archetype and
    /// for a compound none of whose parts could be made.
    pub fn create_instance(&mut self, archetype: ArchetypeId) -> Option<InstanceId> {
        self.create_instance_guarded(archetype, &mut Vec::new())
    }

    fn create_instance_guarded(
        &mut self,
        archetype: ArchetypeId,
        open: &mut Vec<ArchetypeId>,
    ) -> Option<InstanceId> {
        let compound = self.archetype(archetype)?.is_compound();
        if !compound {
            self.add_ref_handle(archetype);
            let id = self.alloc_instance(archetype);
            for component in &mut self.components {
                component.create_instance(id, archetype);
            }
            return Some(id);
        }
        // A compound that holds itself would recurse for ever in the
        // reference; the port makes no instance of the inner one.
        if open.contains(&archetype) {
            return None;
        }
        open.push(archetype);
        let root = self.create_instance_tree(archetype, open);
        open.pop();
        root
    }

    /// `EngineCreateInstanceTree`.
    fn create_instance_tree(
        &mut self,
        archetype: ArchetypeId,
        open: &mut Vec<ArchetypeId>,
    ) -> Option<InstanceId> {
        let (children, constraints) = {
            let a = self.archetype(archetype)?;
            (a.children.clone(), a.constraints.clone())
        };
        let mut parts: BTreeMap<u32, InstanceId> = BTreeMap::new();
        for (index, child) in children.iter().enumerate() {
            if let Some(id) = self.create_instance_guarded(child.archetype, open) {
                self.get_mut(id).part = Some((archetype, index));
                parts.insert(child.crc, id);
                self.add_ref_handle(archetype);
            }
        }
        let mut root = None;
        let arch_name = self
            .archetype(archetype)
            .map(|a| String::from_utf8_lossy(&a.name).into_owned())
            .unwrap_or_default();
        let name = || arch_name.clone();
        let mut warnings = Vec::new();
        for constraint in &constraints {
            let child_part = &children[constraint.child];
            let parent_part = &children[constraint.parent];
            let Some(&child) = parts.get(&child_part.crc) else {
                warnings.push(format!(
                    "Unable to find child part '{}' in archetype '{}'",
                    String::from_utf8_lossy(&child_part.name),
                    name()
                ));
                continue;
            };
            let Some(&parent) = parts.get(&parent_part.crc) else {
                warnings.push(format!(
                    "Unable to find parent part '{}' in archetype '{}'",
                    String::from_utf8_lossy(&parent_part.name),
                    name()
                ));
                continue;
            };
            root = Some(parent);
            if !self.connect_child(parent, child, &constraint.payload) {
                warnings.push("Unable to connect child to parent".to_owned());
            }
        }
        for warning in warnings {
            dump(DumpFlags::WARNING, &warning);
        }
        let root = match root {
            Some(mut root) => {
                while let Some(parent) = self.get(root).parent {
                    root = parent;
                }
                root
            }
            None => *parts.values().next()?,
        };
        self.roots.insert(root);
        Some(root)
    }

    fn is_ancestor(&self, ancestor: InstanceId, mut of: InstanceId) -> bool {
        loop {
            if of == ancestor {
                return true;
            }
            match self.get(of).parent {
                Some(parent) => of = parent,
                None => return false,
            }
        }
    }

    /// `ConnectInstanceChild`: links `child` as the last child of `parent`
    /// with the joint `payload`. The reference links a child that already
    /// has a parent a second time, corrupting both lists, and loops for
    /// ever on a connection that closes a cycle; the port moves such a
    /// child and refuses a cycle (divergence register).
    fn connect_child(
        &mut self,
        parent: InstanceId,
        child: InstanceId,
        payload: &JointPayload,
    ) -> bool {
        if self.is_ancestor(child, parent) {
            return false;
        }
        if let Some(old) = self.get(child).parent {
            self.unlink_child(old, child);
        }
        let last = self.get(parent).last_child;
        {
            let c = self.get_mut(child);
            c.world_dirty = true;
            c.relative = *payload;
            c.next_sibling = None;
            c.prev_sibling = last;
            c.parent = Some(parent);
        }
        match last {
            Some(last) => self.get_mut(last).next_sibling = Some(child),
            None => self.get_mut(parent).first_child = Some(child),
        }
        let p = self.get_mut(parent);
        p.last_child = Some(child);
        p.child_count = p.child_count.wrapping_add(1);
        self.invalidate_extent_chain(parent, true);
        true
    }

    /// `UnlinkInstanceChild`.
    fn unlink_child(&mut self, parent: InstanceId, node: InstanceId) {
        let (prev, next) = {
            let n = self.get(node);
            (n.prev_sibling, n.next_sibling)
        };
        match prev {
            None => self.get_mut(parent).first_child = next,
            Some(prev) => self.get_mut(prev).next_sibling = next,
        }
        match next {
            None => self.get_mut(parent).last_child = prev,
            Some(next) => self.get_mut(next).prev_sibling = prev,
        }
        let p = self.get_mut(parent);
        p.child_count = p.child_count.wrapping_sub(1);
        let n = self.get_mut(node);
        n.parent = None;
        n.next_sibling = None;
        n.prev_sibling = None;
    }

    /// `EngineInvalidateExtentChain`: marks the extents of `id` (or of its
    /// parent first) and its ancestors stale, up to the first that already
    /// is.
    fn invalidate_extent_chain(&mut self, id: InstanceId, from_this: bool) {
        let mut cursor = if from_this {
            Some(id)
        } else {
            self.get(id).parent
        };
        while let Some(at) = cursor {
            let instance = self.get_mut(at);
            if instance.extent.to_bits() == 0xBF80_0000 {
                break;
            }
            instance.extent = -1.0;
            cursor = instance.parent;
        }
    }

    /// `DestroyInstance`: the instance and its subtree; each releases its
    /// archetype (and its compound).
    pub fn destroy_instance(&mut self, id: InstanceId) {
        if self.instance(id).is_none() {
            return;
        }
        while let Some(child) = self.get(id).first_child {
            self.destroy_instance(child);
        }
        if let Some(parent) = self.get(id).parent {
            self.unlink_child(parent, id);
        }
        for component in &mut self.components {
            component.destroy_instance(id);
        }
        self.roots.remove(&id);
        let Some(instance) = self.instances[id.0 as usize].take() else {
            return;
        };
        self.release_handle(instance.archetype);
        if let Some((compound, _)) = instance.part {
            self.release_handle(compound);
        }
    }

    /// `GetInstanceArchetype` without its reference.
    #[must_use]
    pub fn instance_archetype(&self, id: InstanceId) -> Option<ArchetypeId> {
        Some(self.instance(id)?.archetype)
    }

    /// `GetInstanceName`: the part's object name and its CRC, `None` and 0
    /// for an instance that is not a part.
    #[must_use]
    pub fn instance_name(&self, id: Option<InstanceId>) -> (Option<&[u8]>, u32) {
        let part = id
            .and_then(|id| self.instance(id))
            .and_then(|i| i.part)
            .and_then(|(compound, index)| self.archetype(compound)?.children.get(index));
        match part {
            Some(child) => (Some(child.name.as_slice()), child.crc),
            None => (None, 0),
        }
    }

    /// `GetInstanceParent`.
    #[must_use]
    pub fn instance_parent(&self, id: Option<InstanceId>) -> Option<InstanceId> {
        self.instance(id?)?.parent
    }

    /// `GetInstanceRoot`.
    #[must_use]
    pub fn instance_root(&self, id: Option<InstanceId>) -> Option<InstanceId> {
        let mut id = id?;
        self.instance(id)?;
        while let Some(parent) = self.get(id).parent {
            id = parent;
        }
        Some(id)
    }

    /// `GetRelatedInstance(from, flags, cursor)`: with no cursor the first
    /// child of `from`; with flag 1 the cursor's next sibling; else the
    /// next instance of `from`'s subtree in depth-first order.
    #[must_use]
    pub fn related_instance(
        &self,
        from: Option<InstanceId>,
        flags: u32,
        cursor: Option<InstanceId>,
    ) -> Option<InstanceId> {
        let root = from.filter(|&id| self.instance(id).is_some())?;
        let Some(mut node) = cursor.filter(|&id| self.instance(id).is_some()) else {
            return self.get(root).first_child;
        };
        if flags & 1 != 0 {
            return self.get(node).next_sibling;
        }
        if let Some(child) = self.get(node).first_child {
            return Some(child);
        }
        loop {
            if let Some(next) = self.get(node).next_sibling {
                return Some(next);
            }
            node = self.get(node).parent?;
            if node == root {
                return None;
            }
        }
    }

    /// `FindMappedInstance`: `None` gives the first root, a root the next
    /// one; anything else `None`.
    #[must_use]
    pub fn find_mapped_instance(&self, id: Option<InstanceId>) -> Option<InstanceId> {
        match id {
            None => self.roots.iter().next().copied(),
            Some(id) => {
                if !self.roots.contains(&id) {
                    return None;
                }
                self.roots
                    .range((core::ops::Bound::Excluded(id), core::ops::Bound::Unbounded))
                    .next()
                    .copied()
            }
        }
    }

    /// `FindArchetypeChild(handle, child)`: the first part of `archetype`
    /// whose archetype is `child`.
    #[must_use]
    pub fn find_archetype_child(
        &self,
        archetype: ArchetypeId,
        child: ArchetypeId,
    ) -> Option<usize> {
        self.archetype(archetype)?
            .children
            .iter()
            .position(|c| c.archetype == child)
    }

    // --- joints -------------------------------------------------------------------------

    /// `GetInstanceRelative`: the joint to the parent, `None` for a root.
    #[must_use]
    pub fn instance_relative(&self, id: Option<InstanceId>) -> Option<&JointPayload> {
        let instance = self.instance(id?)?;
        instance.parent?;
        Some(&instance.relative)
    }

    /// `CreateJoint(parent, child, payload)`: false when either is missing
    /// or the child has a parent.
    pub fn create_joint(
        &mut self,
        parent: Option<InstanceId>,
        child: Option<InstanceId>,
        payload: &JointPayload,
    ) -> bool {
        let Some(parent) = parent.filter(|&id| self.instance(id).is_some()) else {
            return false;
        };
        let Some(child) = child.filter(|&id| self.instance(id).is_some()) else {
            dump(
                DumpFlags::WARNING,
                "Engine: create_joint: Failing to connect, invalid instances.",
            );
            return false;
        };
        if self.get(child).parent.is_some() {
            dump(
                DumpFlags::WARNING,
                "Engine: create_joint: Failing to connect, child already has a parent.\n",
            );
            return false;
        }
        if self.get(child).child_count as i32 > 0 {
            self.roots.remove(&child);
        }
        if !self.connect_child(parent, child, payload) {
            return false;
        }
        let p = self.get(parent);
        if p.child_count == 1 && p.parent.is_none() {
            self.roots.insert(parent);
        }
        self.update_instance(Some(child), 0);
        true
    }

    /// `DestroyJoint(parent, child)`: detaches `child` from its parent.
    pub fn destroy_joint(&mut self, child: Option<InstanceId>) -> bool {
        let Some(child) = child.filter(|&id| self.instance(id).is_some()) else {
            dump(
                DumpFlags::WARNING,
                "Engine: destroy_joint: Failing to disconnect, child not found.",
            );
            return false;
        };
        let Some(old) = self.get(child).parent else {
            dump(
                DumpFlags::WARNING,
                "Engine: destroy_joint: Failing to disconnect, not connected.",
            );
            return false;
        };
        self.unlink_child(old, child);
        let o = self.get(old);
        if o.child_count == 0 && o.parent.is_none() {
            self.roots.remove(&old);
        }
        if self.get(child).child_count as i32 > 0 {
            self.roots.insert(child);
        }
        true
    }

    /// `SetJointParams(instance, index, values)`. With index 0 the joint's
    /// own parameters (a slide or angle is clamped to the joint's limits);
    /// any other index sets all seven words. False for an instance without
    /// a parent.
    pub fn set_joint_params(&mut self, id: Option<InstanceId>, index: u32, values: &[f32]) -> bool {
        let Some(instance) = id.and_then(|id| self.instance_mut(id)) else {
            return false;
        };
        if instance.parent.is_none() {
            return false;
        }
        let copy = |state: &mut [f32; 7], at: usize, n: usize| {
            for (i, v) in values.iter().take(n).enumerate() {
                state[at + i] = *v;
            }
        };
        if index != 0 {
            copy(&mut instance.joint_state, 0, 7);
            return true;
        }
        instance.world_dirty = true;
        match instance.relative.kind() {
            Some(JointKind::Pris | JointKind::Rev) => {
                let input = values.first().copied().unwrap_or(0.0);
                let (min, max) = (instance.relative.min(), instance.relative.max());
                instance.joint_state[0] = if min > input {
                    min
                } else if max < input {
                    max
                } else {
                    input
                };
            }
            Some(JointKind::Cyl) => copy(&mut instance.joint_state, 0, 2),
            Some(JointKind::Sphere) => copy(&mut instance.joint_state, 3, 4),
            Some(JointKind::Trans) => copy(&mut instance.joint_state, 0, 3),
            Some(JointKind::Loose) => copy(&mut instance.joint_state, 0, 7),
            Some(JointKind::Fix) | None => {}
        }
        true
    }

    /// `GetJointParams(instance, index)`: the words `SetJointParams` takes
    /// (all seven for a non-zero index); `None` for an instance without a
    /// parent.
    #[must_use]
    pub fn joint_params(&self, id: Option<InstanceId>, index: u32) -> Option<Vec<f32>> {
        let instance = self.instance(id?)?;
        instance.parent?;
        let s = &instance.joint_state;
        if index != 0 {
            return Some(s.to_vec());
        }
        Some(match instance.relative.kind() {
            Some(JointKind::Pris | JointKind::Rev) => s[..1].to_vec(),
            Some(JointKind::Cyl) => s[..2].to_vec(),
            Some(JointKind::Sphere) => s[3..7].to_vec(),
            Some(JointKind::Trans) => s[..3].to_vec(),
            Some(JointKind::Loose) => s.to_vec(),
            Some(JointKind::Fix) | None => Vec::new(),
        })
    }

    /// `ApplyJointToLocal(instance, force)`: recomputes the local
    /// transform from the joint when it is dirty; whether it did.
    fn apply_joint_to_local(&mut self, id: InstanceId, force: bool) -> bool {
        let instance = self.get_mut(id);
        if !instance.world_dirty && !force {
            return false;
        }
        instance.world_dirty = false;
        match joint_local(&instance.relative, &instance.joint_state) {
            Some(local) => {
                instance.local = local;
                true
            }
            None => false,
        }
    }

    /// `EngineComposeWorld`: world = parent world * local, after applying
    /// a dirty joint.
    fn compose_world(&mut self, id: InstanceId) {
        let Some(parent) = self.get(id).parent else {
            return;
        };
        let applied = self.apply_joint_to_local(id, false);
        let parent_world = self.get(parent).transform.matrix;
        let instance = self.get_mut(id);
        instance.transform.matrix = mul_transform(&parent_world, &instance.local);
        if applied {
            self.invalidate_extent_chain(id, true);
        }
    }

    fn compose_ancestors(&mut self, id: InstanceId) {
        if let Some(parent) = self.get(id).parent {
            self.compose_ancestors(parent);
            self.compose_world(id);
        }
    }

    fn update_tree(&mut self, id: InstanceId, flags: u32) {
        if flags & 2 == 2 {
            for component in &mut self.components {
                component.update_instance(id);
            }
        }
        if flags & 1 == 1 {
            return;
        }
        let mut child = self.get(id).first_child;
        while let Some(at) = child {
            self.compose_world(at);
            self.update_tree(at, flags);
            child = self.get(at).next_sibling;
        }
    }

    /// `UpdateInstance(instance, flags)`: unless flag 8, composes the
    /// instance's ancestors and itself; then, unless flag 1, its subtree;
    /// flag 2 also tells the components.
    pub fn update_instance(&mut self, id: Option<InstanceId>, flags: u32) {
        let Some(id) = id.filter(|&id| self.instance(id).is_some()) else {
            return;
        };
        if flags & 8 == 0 {
            self.compose_ancestors(id);
        }
        self.update_tree(id, flags);
    }

    /// `NotifyEngineComponents`: the components, then every root's tree.
    pub fn notify_engine_components(&mut self) {
        for component in &mut self.components {
            component.notify();
        }
        let roots: Vec<InstanceId> = self.roots.iter().copied().collect();
        for root in roots {
            self.update_tree(root, 0);
        }
    }

    // --- transforms ---------------------------------------------------------------------

    /// `GetInstanceTransform`: the world transform (the identity for a
    /// missing instance).
    #[must_use]
    pub fn instance_transform(&self, id: Option<InstanceId>) -> Transform {
        id.and_then(|id| self.instance(id))
            .map_or(Transform::IDENTITY, |i| i.transform.matrix)
    }

    /// `GetInstancePosition`.
    #[must_use]
    pub fn instance_position(&self, id: Option<InstanceId>) -> Vector {
        self.instance_transform(id).translation
    }

    /// `GetInstanceOrientation`.
    #[must_use]
    pub fn instance_orientation(&self, id: Option<InstanceId>) -> Matrix {
        self.instance_transform(id).rotation
    }

    /// `SetInstanceMatrix`.
    pub fn set_instance_matrix(&mut self, id: Option<InstanceId>, matrix: &Transform) {
        if let Some(i) = id.and_then(|id| self.instance_mut(id)) {
            i.transform.matrix = *matrix;
        }
    }

    /// `SetInstancePosition`.
    pub fn set_instance_position(&mut self, id: Option<InstanceId>, position: Vector) {
        if let Some(i) = id.and_then(|id| self.instance_mut(id)) {
            i.transform.matrix.translation = position;
        }
    }

    /// `SetInstanceOrientation`.
    pub fn set_instance_orientation(&mut self, id: Option<InstanceId>, rows: &Matrix) {
        if let Some(i) = id.and_then(|id| self.instance_mut(id)) {
            i.transform.matrix.rotation = *rows;
        }
    }

    /// `SetTransformUser`.
    pub fn set_transform_user(&mut self, id: Option<InstanceId>, user: u32) {
        if let Some(i) = id.and_then(|id| self.instance_mut(id)) {
            i.transform.user = user;
        }
    }

    /// `GetTransformUser`.
    #[must_use]
    pub fn transform_user(&self, id: Option<InstanceId>) -> u32 {
        id.and_then(|id| self.instance(id))
            .map_or(0, |i| i.transform.user)
    }

    /// `SetInstanceUserData`.
    pub fn set_instance_user_data(&mut self, id: Option<InstanceId>, value: usize) {
        if let Some(i) = id.and_then(|id| self.instance_mut(id)) {
            i.user_data = value;
        }
    }

    /// `GetInstanceUserData`.
    #[must_use]
    pub fn instance_user_data(&self, id: Option<InstanceId>) -> usize {
        id.and_then(|id| self.instance(id))
            .map_or(0, |i| i.user_data)
    }

    // --- extents ------------------------------------------------------------------------

    /// `SetInstanceExtent(instance, flags, extent, centre)`: with flag 1
    /// the transform's extent (the instance's own), else the cached
    /// extent of the subtree; either way the stale marks go up the tree.
    pub fn set_instance_extent(
        &mut self,
        id: Option<InstanceId>,
        flags: u32,
        extent: f32,
        centre: Vector,
    ) {
        let Some(id) = id.filter(|&id| self.instance(id).is_some()) else {
            return;
        };
        let instance = self.get_mut(id);
        if flags & 1 != 0 {
            instance.transform.extra = extent;
            instance.transform.extra_vec = centre;
            self.invalidate_extent_chain(id, true);
        } else {
            instance.extent = extent;
            instance.extent_vec = centre;
            self.invalidate_extent_chain(id, false);
        }
    }

    /// `GetInstanceExtent(instance, flags)`: with flag 1 the instance's
    /// own extent, else the sphere around its subtree (computed and cached
    /// when stale). A missing instance gives -1 and a zero centre.
    pub fn instance_extent(&mut self, id: Option<InstanceId>, flags: u32) -> (f32, Vector) {
        let Some(id) = id.filter(|&id| self.instance(id).is_some()) else {
            return (-1.0, Vector::ZERO);
        };
        self.compute_extent(id, flags & 1 == 0)
    }

    /// `EngineComputeInstanceExtent`.
    fn compute_extent(&mut self, id: InstanceId, recursive: bool) -> (f32, Vector) {
        let instance = self.get(id);
        if !recursive {
            return (instance.transform.extra, instance.transform.extra_vec);
        }
        if instance.extent > 0.0 {
            return (instance.extent, instance.extent_vec);
        }
        let world = instance.transform.matrix;
        let own_r = instance.transform.extra;
        let own_world = transform_xform(&world, instance.transform.extra_vec).to_words();
        let r = |v: f64| v as f32;
        let mut min_v: [f32; 3] =
            core::array::from_fn(|a| r(f64::from(own_world[a]) - f64::from(own_r)));
        let mut max_v: [f32; 3] =
            core::array::from_fn(|a| r(f64::from(own_world[a]) + f64::from(own_r)));
        let mut children = Vec::new();
        let mut child = self.get(id).first_child;
        while let Some(at) = child {
            children.push(at);
            child = self.get(at).next_sibling;
        }
        let mut child_spheres = Vec::with_capacity(children.len());
        for &at in &children {
            let child_xf = self.get(at).transform.matrix;
            let (child_r, child_local) = self.compute_extent(at, true);
            let child_world = transform_xform(&child_xf, child_local).to_words();
            for a in 0..3 {
                let lo = r(f64::from(child_world[a]) - f64::from(child_r));
                if lo < min_v[a] {
                    min_v[a] = lo;
                }
                let hi = r(f64::from(child_world[a]) + f64::from(child_r));
                if max_v[a] < hi {
                    max_v[a] = hi;
                }
            }
            child_spheres.push((child_r, child_world));
        }
        let centre: [f32; 3] =
            core::array::from_fn(|a| r((f64::from(max_v[a]) + f64::from(min_v[a])) * 0.5));
        let span = |a: usize| r(f64::from(max_v[a]) - f64::from(min_v[a]));
        let (span_z, span_y, span_x) = (span(2), span(1), span(0));
        let mut widest = if span_x < span_y { span_y } else { span_x };
        if widest < span_z {
            widest = span_z;
        }
        let mut radius = r(f64::from(widest) * 0.5);
        let own_sphere = distance(&centre, &own_world) + f64::from(own_r);
        if own_sphere > f64::from(radius) {
            radius = r(own_sphere);
        }
        // The reference computes every child's extent a second time here;
        // the cached result is the same.
        for &at in &children {
            let child_xf = self.get(at).transform.matrix;
            let (child_r, child_local) = self.compute_extent(at, true);
            let child_world = transform_xform(&child_xf, child_local).to_words();
            let child_sphere = r(distance(&child_world, &centre) + f64::from(child_r));
            if child_sphere > radius {
                radius = child_sphere;
            }
        }
        let local_centre = inverse_transform(&world, Vector::from_words(centre));
        let instance = self.get_mut(id);
        instance.extent = radius;
        instance.extent_vec = local_centre;
        (radius, local_centre)
    }
}

/// `ExtentDistance`: the differences stay unrounded, the squares summed z,
/// y, x.
fn distance(a: &[f32; 3], b: &[f32; 3]) -> f64 {
    let d: [f64; 3] = core::array::from_fn(|i| f64::from(a[i]) - f64::from(b[i]));
    (d[2] * d[2] + d[1] * d[1] + d[0] * d[0]).sqrt()
}
