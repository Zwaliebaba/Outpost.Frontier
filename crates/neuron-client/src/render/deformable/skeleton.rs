//! The skeleton and the bone runtimes: `FSkeleton` (the bones' engine
//! archetypes and `Bone to root` transforms), `FBoneRuntime` (the
//! `IInstanceTransform` the engine writes each bone's pose into, with its
//! dirty bits and the skinning matrix) and `FLodHolder` (a skeleton bound
//! to the instances of one engine instance tree).

use neuron_core::vfs::{FileSystem, FsDesc};

use super::math::{IDENTITY12, mul_transform12};
use super::services::{BoneLink, DeformHost};
use super::{EngineHandle, INSTANCE_NONE, Instance};

/// `G_MaxBoneCount`.
pub const MAX_BONES: usize = 150;

/// One bone of a skeleton (`FBoneSlot`).
#[derive(Clone, Debug, PartialEq)]
pub struct BoneSlot {
    /// The bone's engine archetype (`Parent` in the reference's layout:
    /// the slot holds the handle `CreateArchetypeHandle` answered).
    pub archetype: EngineHandle,
    /// `Bone to root`.
    pub bone_to_root: [f32; 12],
}

/// `FSkeleton`.
#[derive(Clone, Debug, PartialEq)]
pub struct Skeleton {
    /// `Scale` (1.0; never read).
    pub scale: f32,
    /// The bones loaded, at most [`MAX_BONES`].
    pub bones: Vec<BoneSlot>,
    /// The skeleton's own (compound) archetype; -1 until made.
    pub archetype: EngineHandle,
}

impl Default for Skeleton {
    /// `ConstructSkeleton`.
    fn default() -> Self {
        Self {
            scale: 1.0,
            bones: Vec::new(),
            archetype: INSTANCE_NONE,
        }
    }
}

impl Skeleton {
    /// `LoadSkeleton`: every `*.3db` entry of the root whose `Bone to root`
    /// reads as 0x30 bytes becomes a bone, its archetype made from the
    /// entry; then the skeleton's archetype `name` is made from the root.
    /// `false` when that archetype is -1.
    pub fn load(
        &mut self,
        name: &[u8],
        fs: &mut dyn FileSystem,
        services: &mut dyn DeformHost,
    ) -> bool {
        let entries = fs.find(b"*.3db").unwrap_or_default();
        for entry in entries {
            let desc = FsDesc::read(&entry.data.name).with_found(entry.token);
            let Ok(mut child) = fs.create_child(&desc) else {
                continue;
            };
            let skeleton_node = child.open_node(b"DFMSkeleton").is_ok();
            let mut matrix = None;
            if let Ok(mut file) = child.open_file(&FsDesc::read(b"Bone to root")) {
                let mut buf = [0u8; 0x30];
                if file.read(&mut buf).ok() == Some(0x30) {
                    matrix = Some(core::array::from_fn(|k| {
                        f32::from_le_bytes([
                            buf[k * 4],
                            buf[k * 4 + 1],
                            buf[k * 4 + 2],
                            buf[k * 4 + 3],
                        ])
                    }));
                }
            }
            if skeleton_node {
                let _ = fs.open_node(b"..");
            }
            if let Some(bone_to_root) = matrix {
                // The reference writes past its 150 slots (divergence D394).
                if self.bones.len() == MAX_BONES {
                    break;
                }
                let archetype = services.create_archetype(&entry.data.name, child.as_mut());
                self.bones.push(BoneSlot {
                    archetype,
                    bone_to_root,
                });
            }
        }
        self.archetype = services.create_skeleton_archetype(name, fs);
        self.archetype != INSTANCE_NONE
    }

    /// `DestroySkeletonHandles`: `ReleaseHandle` on every bone's archetype,
    /// then on the skeleton's (-1 included).
    pub fn release_handles(&self, services: &mut dyn DeformHost) {
        for bone in &self.bones {
            services.release_handle(bone.archetype);
        }
        services.release_handle(self.archetype);
    }
}

/// `FBoneRuntime`: the transform the engine keeps a bone instance's pose
/// in.
#[derive(Clone, Debug, PartialEq)]
pub struct BoneRuntime {
    /// The engine instance (`Init`); -1 until bound.
    pub instance: Instance,
    /// The skeleton bone it stands for (`Source`).
    pub source: Option<usize>,
    /// The pose the engine sets (`LocalTransform`: rotation, position).
    pub local: [f32; 12],
    /// Bit 0: the bone is a root of its tree (its moves mark it dirty);
    /// bit 1: the pose changed since the last skin.
    pub flags: u8,
    /// The skinning matrix (`Matrix`).
    pub matrix: [f32; 12],
}

impl Default for BoneRuntime {
    /// The construct loop's body: no instance, a zero pose, an identity
    /// matrix.
    fn default() -> Self {
        Self {
            instance: INSTANCE_NONE,
            source: None,
            local: [0.0; 12],
            flags: 0,
            matrix: IDENTITY12,
        }
    }
}

impl BoneRuntime {
    fn mark_if_root(&mut self, flags: u8) {
        if flags & 1 != 0 {
            self.flags = flags | 2;
        }
    }

    /// `SetPosition` (slot 3).
    pub fn set_position(&mut self, position: &[f32; 3]) {
        self.local[9..12].copy_from_slice(position);
        self.mark_if_root(self.flags);
    }

    /// `SetOrientation` (slot 5): the nine words, always.
    pub fn set_orientation(&mut self, rows: &[f32; 9]) {
        let flags = self.flags;
        self.local[..9].copy_from_slice(rows);
        self.mark_if_root(flags);
    }

    /// `SetMatrix` (slot 7): the twelve words, always.
    pub fn set_matrix(&mut self, matrix: &[f32; 12]) {
        let flags = self.flags;
        self.local = *matrix;
        self.mark_if_root(flags);
    }

    /// `MarkWorldDirty` (slot 13): 0 when already dirty; else the dirty
    /// bit is set and the result is whether the bone is no root.
    pub fn mark_world_dirty(&mut self) -> u32 {
        if self.flags & 2 != 0 {
            return 0;
        }
        self.flags |= 2;
        u32::from(!self.flags & 1)
    }

    /// `ComposeBoneWorldMatrix`: `inverse_root * (pose * bone_to_root)`.
    pub fn compose(&mut self, bone_to_root: &[f32; 12], inverse_root: &[f32; 12]) {
        let local = mul_transform12(&self.local, bone_to_root);
        self.matrix = mul_transform12(inverse_root, &local);
    }
}

/// `FLodHolder`: a skeleton and the bone runtimes of its instance tree.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LodHolder {
    /// The skeleton.
    pub skeleton: Skeleton,
    /// One runtime per skeleton bone.
    pub bones: Vec<BoneRuntime>,
    /// The link walk's cursor: after the bind, the number of bones the
    /// engine's tree linked (the bound bone count).
    pub bind_cursor: i32,
}

impl LodHolder {
    /// The bound bones (`BindCursor` of them).
    #[must_use]
    pub fn bound(&self) -> usize {
        usize::try_from(self.bind_cursor)
            .unwrap_or(0)
            .min(self.bones.len())
    }

    /// `BindOneBoneRuntime`: bit 0 becomes "bone 0", the source is the
    /// skeleton's bone, and the runtime becomes the instance's transform
    /// (the engine calls `Init`, then copies its matrix in through
    /// `SetMatrix`).
    pub fn bind_one(&mut self, services: &mut dyn DeformHost, index: usize, instance: Instance) {
        let Some(slot) = self.bones.get_mut(index) else {
            return;
        };
        slot.flags = (slot.flags & !1) | u8::from(index == 0);
        slot.source = Some(index);
        if let Some(matrix) = services.set_instance_transform(instance, index) {
            slot.instance = instance;
            slot.set_matrix(&matrix);
        }
    }

    /// `LinkBoneByParent`: from the cursor, the first bone whose archetype
    /// is `archetype` is bound to `instance`.
    fn link_bone(
        &mut self,
        services: &mut dyn DeformHost,
        instance: Instance,
        archetype: EngineHandle,
    ) -> bool {
        let count = self.skeleton.bones.len();
        let mut cursor = usize::try_from(self.bind_cursor).unwrap_or(usize::MAX);
        if cursor >= count {
            return false;
        }
        while self.skeleton.bones[cursor].archetype != archetype {
            cursor += 1;
            if cursor >= count {
                return false;
            }
        }
        self.bind_cursor = i32::try_from(cursor).unwrap_or(i32::MAX);
        self.bind_one(services, cursor, instance);
        self.bind_cursor += 1;
        true
    }

    /// `AttachSkeleton`: one runtime per bone, the skeleton's instance made
    /// with the bone link registered, then an instance of its own for
    /// every bone the tree did not link (those become roots). Always
    /// `true`.
    pub fn attach_skeleton(&mut self, skeleton: Skeleton, services: &mut dyn DeformHost) -> bool {
        self.skeleton = skeleton;
        self.bones = vec![BoneRuntime::default(); self.skeleton.bones.len()];
        let handle = self.skeleton.archetype;
        let root = services.create_instance(handle, Some(self));
        if root == INSTANCE_NONE {
            return true;
        }
        for index in 0..self.skeleton.bones.len() {
            if self.bones[index].instance == INSTANCE_NONE {
                let archetype = self.skeleton.bones[index].archetype;
                let instance = services.create_instance(archetype, None);
                self.bind_one(services, index, instance);
                self.bones[index].flags |= 1;
            }
        }
        true
    }

    /// The holder's destruction: the skeleton's handles released.
    pub fn destroy(&self, services: &mut dyn DeformHost) {
        self.skeleton.release_handles(services);
    }
}

impl BoneLink for LodHolder {
    fn link(
        &mut self,
        services: &mut dyn DeformHost,
        instance: Instance,
        archetype: EngineHandle,
    ) -> bool {
        self.link_bone(services, instance, archetype)
    }
}
