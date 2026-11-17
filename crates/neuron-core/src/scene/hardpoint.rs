//! Hardpoints: the `Hardpoint` engine component of
//! `CppSource/SceneRuntime/SceneRuntime.cpp` (`HardpointCreateArchetype`,
//! `HardpointGetHardpoint`, `HardpointAddHardpoint`,
//! `HardpointEnumerateHardpoints`, `HardpointFindHardpoint`,
//! `HardpointConnect`, `HardpointDuplicateArchetype`,
//! `HardpointDestroyArchetype`).
//!
//! An archetype's file system holds `Hardpoints\<type>\<name>\` with the
//! files `Position` (12 bytes), `Orientation` (36 bytes) and, for
//! `Revolute` and `Prismatic`, `Axis` (12), `Min` and `Max` (4 each). The
//! component keeps one list of records per archetype, in load order; a
//! record is the lower-cased name, its CRC and the 0x48-byte blob
//! [`HardpointData`]. Field reads are all-or-nothing: a file that is
//! missing or shorter than its field leaves the field zero.

use std::any::Any;
use std::collections::BTreeMap;

use crate::diag::{DumpFlags, dump};
use crate::hash::name_crc32;
use crate::math::{Matrix, Vector, mul_matrix, transform_matrix};
use crate::vfs::{FileSystem, FsDesc};

use super::engine::{ArchetypeId, Engine, EngineComponent, InstanceId};
use super::joint::JointPayload;
use super::{SceneError, find, open_node, soft};

/// Bytes of a hardpoint blob.
pub const HARDPOINT_DATA_SIZE: usize = 0x48;

/// A hardpoint's blob: +0x00 the joint type (0 fixed, 1 prismatic, 2
/// revolute), +0x04 the position, +0x10 the orientation (row-major),
/// +0x34 the axis, +0x40 the minimum, +0x44 the maximum.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct HardpointData(pub [u8; HARDPOINT_DATA_SIZE]);

impl Default for HardpointData {
    fn default() -> Self {
        Self([0; HARDPOINT_DATA_SIZE])
    }
}

impl HardpointData {
    fn word(&self, at: usize) -> u32 {
        let b = &self.0;
        u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
    }

    fn f(&self, at: usize) -> f32 {
        f32::from_bits(self.word(at))
    }

    fn vector(&self, at: usize) -> Vector {
        Vector::new(self.f(at), self.f(at + 4), self.f(at + 8))
    }

    /// +0x00: the joint type.
    #[must_use]
    pub fn kind(&self) -> u32 {
        self.word(0)
    }

    /// +0x04.
    #[must_use]
    pub fn position(&self) -> Vector {
        self.vector(4)
    }

    /// +0x10.
    #[must_use]
    pub fn orientation(&self) -> Matrix {
        Matrix::from_words(core::array::from_fn(|i| self.f(0x10 + 4 * i)))
    }

    /// +0x34.
    #[must_use]
    pub fn axis(&self) -> Vector {
        self.vector(0x34)
    }

    /// +0x40.
    #[must_use]
    pub fn min(&self) -> f32 {
        self.f(0x40)
    }

    /// +0x44.
    #[must_use]
    pub fn max(&self) -> f32 {
        self.f(0x44)
    }
}

/// One hardpoint (`FHardpointRecord`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HardpointRecord {
    /// The CRC of the name as found (case-folded, so of the lower-cased
    /// name too).
    pub crc: u32,
    /// The name, lower-cased in ASCII (`_strdup` + `_strlwr`).
    pub name: Vec<u8>,
    /// The blob.
    pub data: HardpointData,
}

impl HardpointRecord {
    fn new(crc: u32, name: &[u8]) -> Self {
        Self {
            crc,
            name: name.to_ascii_lowercase(),
            data: HardpointData::default(),
        }
    }
}

/// `ParseHardpointJointType`: "Fixed" 0, "Revolute" 2, "Prismatic" 1,
/// compared case-sensitively; anything else warns and is fixed.
fn joint_type(name: &[u8]) -> u32 {
    match name {
        b"Fixed" => 0,
        b"Revolute" => 2,
        b"Prismatic" => 1,
        _ => {
            dump(
                DumpFlags::WARNING,
                "HardPoint: unknown joint type, returning JT_FIXED",
            );
            0
        }
    }
}

/// `ReadHardpointField` and `ReadHardpointOrientation`: one read of
/// `dest.len()` bytes, copied only when it reports all of them.
fn read_field(fs: &dyn FileSystem, name: &[u8], dest: &mut [u8]) -> Result<(), SceneError> {
    let Some(mut file) = soft(fs.open_file(&FsDesc::read(name)))? else {
        return Ok(());
    };
    let mut temp = vec![0; dest.len()];
    if soft(file.read(&mut temp))? == Some(dest.len()) {
        dest.copy_from_slice(&temp);
    }
    Ok(())
}

/// `HardpointCreateArchetype`'s walk: the records under `Hardpoints` of
/// `fs`, in find order, and the current node it leaves `fs` at (it does
/// not go back up when `Hardpoints` has nothing to find).
///
/// # Errors
///
/// A malformed archive.
pub fn load_hardpoints(fs: &mut dyn FileSystem) -> Result<Vec<HardpointRecord>, SceneError> {
    let mut records = Vec::new();
    if !open_node(fs, b"Hardpoints")? {
        return Ok(records);
    }
    let outer = soft(fs.find(b"*"))?.unwrap_or_default();
    if outer.is_empty() {
        return Ok(records);
    }
    for kind_entry in outer {
        let kind_name = kind_entry.data.name;
        if !open_node(fs, &kind_name)? {
            dump(
                DumpFlags::WARNING,
                &format!(
                    "Hardpoint: create_archetype: '{}' has old hardpoint layout. If you see this message, mail pbleisch\n",
                    String::from_utf8_lossy(&kind_name)
                ),
            );
            continue;
        }
        for entry in find(fs, b"*.*")? {
            let name = entry.data.name;
            if !open_node(fs, &name)? {
                continue;
            }
            let mut record = HardpointRecord::new(name_crc32(&name), &name);
            let kind = joint_type(&kind_name);
            record.data.0[..4].copy_from_slice(&kind.to_le_bytes());
            let data = &mut record.data.0;
            if kind != 0 {
                read_field(fs, b"Axis", &mut data[0x34..0x40])?;
                read_field(fs, b"Min", &mut data[0x40..0x44])?;
                read_field(fs, b"Max", &mut data[0x44..0x48])?;
            }
            read_field(fs, b"Position", &mut data[0x04..0x10])?;
            read_field(fs, b"Orientation", &mut data[0x10..0x34])?;
            records.push(record);
            let _ = open_node(fs, b"..")?;
        }
        let _ = open_node(fs, b"..")?;
    }
    let _ = open_node(fs, b"..")?;
    Ok(records)
}

/// The `Hardpoint` component: hardpoint lists by archetype.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Hardpoints {
    archetypes: BTreeMap<ArchetypeId, Vec<HardpointRecord>>,
}

impl Hardpoints {
    /// An empty component.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The records of `archetype`, `None` when it has no list.
    #[must_use]
    pub fn records(&self, archetype: ArchetypeId) -> Option<&[HardpointRecord]> {
        self.archetypes.get(&archetype).map(Vec::as_slice)
    }

    /// `GetHardpoint(handle, key)`: the blob of the first record with the
    /// key's CRC.
    #[must_use]
    pub fn get(&self, archetype: ArchetypeId, crc: u32) -> Option<HardpointData> {
        self.records(archetype)?
            .iter()
            .find(|r| r.crc == crc)
            .map(|r| r.data)
    }

    /// `AddHardpoint(handle, key, data)`: replaces the blob of the first
    /// record with the key's CRC, or appends a record (named after the
    /// key, lower-cased). Always 1 in the reference.
    pub fn add(&mut self, archetype: ArchetypeId, name: &[u8], crc: u32, data: HardpointData) {
        let list = self.archetypes.entry(archetype).or_default();
        if let Some(record) = list.iter_mut().find(|r| r.crc == crc) {
            record.data = data;
            return;
        }
        let mut record = HardpointRecord::new(crc, name);
        record.data = data;
        list.push(record);
    }

    /// `EnumerateHardpoints`: the names in list order.
    pub fn names(&self, archetype: ArchetypeId) -> impl Iterator<Item = &[u8]> {
        self.records(archetype)
            .unwrap_or_default()
            .iter()
            .map(|r| r.name.as_slice())
    }

    /// `FindHardpoint(handle, crc)`: the name of the first record with the
    /// CRC.
    #[must_use]
    pub fn find(&self, archetype: ArchetypeId, crc: u32) -> Option<&[u8]> {
        self.records(archetype)?
            .iter()
            .find(|r| r.crc == crc)
            .map(|r| r.name.as_slice())
    }
}

impl EngineComponent for Hardpoints {
    /// `HardpointCreateArchetype`: loads the archetype's list when the
    /// walk finds at least one hardpoint directory. The reference's result
    /// (1 when it made a list) is not used by the engine.
    fn create_archetype(
        &mut self,
        archetype: ArchetypeId,
        fs: &mut dyn FileSystem,
    ) -> Result<(), SceneError> {
        let records = load_hardpoints(fs)?;
        if !records.is_empty() {
            // The reference appends to an existing list for the key.
            self.archetypes
                .entry(archetype)
                .or_default()
                .extend(records);
        }
        Ok(())
    }

    fn duplicate_archetype(&mut self, new: ArchetypeId, old: ArchetypeId) {
        if let Some(list) = self.archetypes.get(&old).cloned() {
            // `insert` keeps an existing list for the new key.
            self.archetypes.entry(new).or_insert(list);
        }
    }

    fn destroy_archetype(&mut self, archetype: ArchetypeId) {
        self.archetypes.remove(&archetype);
    }

    fn as_any(&self) -> &dyn Any {
        self
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

/// The joint payload `ConnectHardpoints` builds from hardpoint `a` of the
/// parent and hardpoint `b` of the child, whose type it takes: a fixed
/// joint puts `b` at `a`; a prismatic or revolute joint keeps both
/// positions, the combined orientation, `b`'s axis turned by it and `b`'s
/// limits; other types leave the payload zero.
#[must_use]
pub fn connect_payload(a: &HardpointData, b: &HardpointData) -> JointPayload {
    let kind = b.kind();
    let mut payload = JointPayload::zeroed(kind);
    let put = |p: &mut JointPayload, at: usize, v: f32| p.0[at] = v.to_bits();
    let put_matrix = |p: &mut JointPayload, m: &Matrix| {
        for (i, v) in m.to_words().into_iter().enumerate() {
            put(p, 7 + i, v);
        }
    };
    if kind == 0 {
        let m = mul_matrix(&a.orientation(), &b.orientation().transpose());
        let v = transform_matrix(&m, b.position());
        let pa = a.position();
        put(&mut payload, 4, (f64::from(pa.x) - f64::from(v.x)) as f32);
        put(&mut payload, 5, (f64::from(pa.y) - f64::from(v.y)) as f32);
        put(&mut payload, 6, (f64::from(pa.z) - f64::from(v.z)) as f32);
        put_matrix(&mut payload, &m);
    } else if kind == 1 || kind == 2 {
        for (i, v) in a.position().to_words().into_iter().enumerate() {
            put(&mut payload, 1 + i, v);
        }
        for (i, v) in b.position().to_words().into_iter().enumerate() {
            put(&mut payload, 4 + i, v);
        }
        let m = mul_matrix(&a.orientation(), &b.orientation());
        let axis = transform_matrix(&m, b.axis());
        put_matrix(&mut payload, &m);
        for (i, v) in axis.to_words().into_iter().enumerate() {
            put(&mut payload, 16 + i, v);
        }
        put(&mut payload, 19, b.min());
        put(&mut payload, 20, b.max());
    }
    payload
}

/// `ConnectHardpoints(a, key_a, b, key_b)`: joins instance `b` to instance
/// `a` at the named hardpoints. Returns 1 when `a`'s hardpoint is missing,
/// 2 when `b`'s is, 3 when `CreateJoint` refused, 0 on success.
pub fn connect_hardpoints(
    engine: &mut Engine,
    a: Option<InstanceId>,
    crc_a: u32,
    b: Option<InstanceId>,
    crc_b: u32,
) -> u32 {
    let lookup = |engine: &Engine, instance: Option<InstanceId>, crc| {
        let archetype = engine.instance_archetype(instance?)?;
        engine.component::<Hardpoints>()?.get(archetype, crc)
    };
    let Some(data_a) = lookup(engine, a, crc_a) else {
        return 1;
    };
    let Some(data_b) = lookup(engine, b, crc_b) else {
        return 2;
    };
    let payload = connect_payload(&data_a, &data_b);
    if engine.create_joint(a, b, &payload) {
        0
    } else {
        3
    }
}
