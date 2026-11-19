//! Material animation (`FMaterialAnimation`, `FImaData` and `FImaInstance`
//! of `CppSource/Shading/MaterialAnimation.cpp`): animations by name CRC,
//! the groups each load returns, and the instances that play them into a
//! texture matrix.
//!
//! An animation is `count` keys, each with a duration and four rates
//! (`MADeltas`), and from the second key on four start values (`MAKeys`;
//! the first key starts from 0). An instance holds a key index and the
//! time spent in that key; ticking moves through the keys and wraps after
//! the last. Its texture matrix is the identity with `u` scale, `u` offset,
//! `v` scale and `v` offset taken from the key's start values plus its
//! rates times the time.
//!
//! The data side (`ParseMaKeys`, `LoadAnimationGroup`'s walk) is
//! `neuron_core::material_data::anim`; this module holds the run-time
//! side. Animation data is shared ([`Rc`]) between the map, the groups and
//! the instances, as the reference shares its reference-counted `FImaData`.
//!
//! The reference links every instance of the process into one list that
//! `TickAnimationInstances` walks; here each [`MaterialAnimations`] owns
//! the list of the instances it created (divergence D307). The arithmetic
//! is the reference's x87 order in `f64`, rounded to `f32` where it stores
//! a `float` (decision T11).

use std::collections::BTreeMap;
use std::rc::Rc;

use neuron_core::material_data::{MaDelta, MaKey, MaterialAnimation, load_material_animations};
use neuron_core::vfs::FileSystem;

use super::{GR_GENERIC, GR_OK};

/// One animation (`FImaData`): its name CRC and key block.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AnimationData {
    /// `NameCrc`.
    pub crc: u32,
    /// `MACount`: the number of keys.
    pub count: u32,
    /// `MAFlags`.
    pub flags: u32,
    /// The complete `MADeltas` records read.
    pub deltas: Vec<MaDelta>,
    /// The complete `MAKeys` records read.
    pub keys: Vec<MaKey>,
}

impl AnimationData {
    /// The key block `ParseMaKeys` leaves for an animation.
    #[must_use]
    pub fn from_parsed(animation: &MaterialAnimation) -> AnimationData {
        AnimationData {
            crc: animation.crc,
            count: animation.count,
            flags: animation.flags,
            deltas: animation.delta_records(),
            keys: animation.key_records(),
        }
    }

    /// Key `index`'s delta; a record the leaf did not hold reads as zeros
    /// (the reference reads its uninitialised buffer; divergence D307).
    fn delta(&self, index: u32) -> [f32; 5] {
        self.deltas.get(index as usize).map_or([0.0; 5], |d| {
            [
                f32::from_bits(d.duration),
                f32::from_bits(d.rates[0]),
                f32::from_bits(d.rates[1]),
                f32::from_bits(d.rates[2]),
                f32::from_bits(d.rates[3]),
            ]
        })
    }

    fn key(&self, index: u32) -> [f32; 4] {
        self.keys
            .get(index as usize)
            .map_or([0.0; 4], |k| k.values.map(f32::from_bits))
    }
}

/// An instance (`FImaInstance`).
#[derive(Clone, Debug, PartialEq)]
pub struct AnimationInstance {
    data: Option<Rc<AnimationData>>,
    /// `Time`: the time spent in the current key.
    pub time: f32,
    /// `Frame`: the current key.
    pub frame: u32,
    refs: u32,
}

impl AnimationInstance {
    /// A new instance playing `data` from its first key.
    #[must_use]
    pub fn new(data: Option<Rc<AnimationData>>) -> AnimationInstance {
        AnimationInstance {
            data,
            time: 0.0,
            frame: 0,
            refs: 1,
        }
    }

    /// The animation it plays.
    #[must_use]
    pub fn data(&self) -> Option<&Rc<AnimationData>> {
        self.data.as_ref()
    }

    /// `ResetTime`.
    pub fn reset_time(&mut self) {
        self.frame = 0;
        self.time = 0.0;
    }

    /// `Tick`: adds `dt` (only a positive one) and moves on while the time
    /// exceeds the current key's duration, wrapping after the last key.
    /// The first comparison uses the unrounded sum, as the x87 keeps it.
    /// A loop that would never end in the reference (the time stops
    /// changing for a whole cycle of keys) ends here (divergence D307).
    pub fn tick(&mut self, dt: f32) {
        let Some(data) = self.data.clone() else {
            return;
        };
        // `TEST AH,0x41`: not greater, NaN included, returns.
        if dt.partial_cmp(&0.0) != Some(core::cmp::Ordering::Greater) || data.count == 0 {
            return;
        }
        let mut comparison = f64::from(dt) + f64::from(self.time);
        self.time = comparison as f32;
        let mut stalled = 0u32;
        while comparison > f64::from(data.delta(self.frame)[0]) {
            let remaining = f64::from(self.time) - f64::from(data.delta(self.frame)[0]);
            let before = self.time;
            self.frame = self.frame.wrapping_add(1);
            self.time = remaining as f32;
            if self.frame >= data.count {
                self.frame = 0;
            }
            comparison = f64::from(self.time);
            if self.time.to_bits() == before.to_bits() {
                stalled += 1;
                if stalled >= data.count {
                    break;
                }
            } else {
                stalled = 0;
            }
        }
    }

    /// `FillUvMatrix`: the 4x4 texture matrix (row-major, as the reference
    /// writes its 16 floats). `None` without data (the reference
    /// dereferences null).
    #[must_use]
    pub fn uv_matrix(&self) -> Option<[f32; 16]> {
        let data = self.data.as_ref()?;
        let mut out = [0.0f32; 16];
        out[0] = 1.0;
        out[5] = 1.0;
        out[10] = 1.0;
        out[15] = 1.0;
        let key = if self.frame != 0 {
            data.key(self.frame - 1)
        } else {
            [0.0; 4]
        };
        let delta = data.delta(self.frame);
        let time = f64::from(self.time);
        let value_b = (f64::from(delta[2]) * time + f64::from(key[1])) as f32;
        let value_c = f64::from(delta[3]) * time + f64::from(key[2]);
        let value_d = (f64::from(delta[4]) * time + f64::from(key[3])) as f32;
        let a_product = f64::from(delta[1]) * time;
        out[6] = value_b;
        out[2] = (a_product + f64::from(key[0])) as f32;
        out[0] = (value_c + 1.0) as f32;
        out[5] = (f64::from(value_d) + 1.0) as f32;
        Some(out)
    }

    /// The reference count.
    #[must_use]
    pub fn refs(&self) -> u32 {
        self.refs
    }
}

/// An instance of a [`MaterialAnimations`]: a slot index.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct InstanceId(u32);

impl InstanceId {
    /// The slot index (creation order).
    #[must_use]
    pub fn index(self) -> u32 {
        self.0
    }
}

/// A loaded group (the reference's opaque `FMatAnimLoadGroup` pointer).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct GroupId(u32);

impl GroupId {
    /// The group's number (load order).
    #[must_use]
    pub fn index(self) -> u32 {
        self.0
    }
}

/// The animation library (`FMaterialAnimation`, `IMatAnimLibrary`).
#[derive(Clone, Debug, Default)]
pub struct MaterialAnimations {
    /// `CrcMap`: in insertion order, a CRC at most once after `StoreData`.
    map: Vec<(u32, Rc<AnimationData>)>,
    groups: BTreeMap<u32, Vec<(u32, Rc<AnimationData>)>>,
    next_group: u32,
    /// Instances by slot; the live ones in creation order are the tick
    /// list.
    instances: Vec<Option<AnimationInstance>>,
}

impl MaterialAnimations {
    /// An empty library.
    #[must_use]
    pub fn new() -> MaterialAnimations {
        MaterialAnimations::default()
    }

    /// The map: CRCs and their animations in map order.
    #[must_use]
    pub fn map(&self) -> &[(u32, Rc<AnimationData>)] {
        &self.map
    }

    /// A group's entries.
    #[must_use]
    pub fn group(&self, group: GroupId) -> Option<&[(u32, Rc<AnimationData>)]> {
        self.groups.get(&group.0).map(Vec::as_slice)
    }

    /// `FindData`.
    #[must_use]
    pub fn find(&self, crc: u32) -> Option<&Rc<AnimationData>> {
        self.map.iter().find(|(c, _)| *c == crc).map(|(_, d)| d)
    }

    /// `StoreData`: `crc` maps to `data`, replacing what it mapped to.
    fn store(&mut self, crc: u32, data: &Rc<AnimationData>) {
        match self.map.iter_mut().find(|(c, _)| *c == crc) {
            Some(entry) => entry.1 = Rc::clone(data),
            None => self.map.push((crc, Rc::clone(data))),
        }
    }

    /// `LoadAnimationGroup`: the animations below `MaterialAnim`, each
    /// entered in the map and in a new group. `None` when the node does
    /// not open or holds no animation.
    pub fn load_group(&mut self, fs: &mut dyn FileSystem) -> Option<GroupId> {
        let animations = load_material_animations(fs)?;
        let data: Vec<AnimationData> = animations.iter().map(AnimationData::from_parsed).collect();
        self.add_group(data)
    }

    /// The per-animation steps of `LoadAnimationGroup`: each animation
    /// entered in the map (replacing an earlier one of the same CRC) and
    /// in a new group, in order. `None` for no animations.
    pub fn add_group(&mut self, animations: Vec<AnimationData>) -> Option<GroupId> {
        if animations.is_empty() {
            return None;
        }
        let mut entries = Vec::with_capacity(animations.len());
        for animation in animations {
            let data = Rc::new(animation);
            self.store(data.crc, &data);
            entries.push((data.crc, data));
        }
        let id = self.next_group;
        self.next_group = self.next_group.wrapping_add(1);
        self.groups.insert(id, entries);
        Some(GroupId(id))
    }

    /// `UnloadAnimationGroup`: every map entry with a CRC of the group
    /// leaves the map (also one a later group stored), then the group's
    /// references go. An unknown group is a no-op.
    pub fn unload_group(&mut self, group: GroupId) -> i32 {
        let Some(entries) = self.groups.remove(&group.0) else {
            return GR_OK;
        };
        for (crc, _) in &entries {
            self.map.retain(|(c, _)| c != crc);
        }
        GR_OK
    }

    /// `CreateAnimationInstance`: a new instance of the animation of `crc`
    /// (`Query + 4`), last in the tick list. `GR_GENERIC` on a miss.
    pub fn create_instance(&mut self, crc: u32) -> Result<InstanceId, i32> {
        let data = self.find(crc).cloned().ok_or(GR_GENERIC)?;
        let id = InstanceId(self.instances.len() as u32);
        self.instances
            .push(Some(AnimationInstance::new(Some(data))));
        Ok(id)
    }

    /// An instance.
    #[must_use]
    pub fn instance(&self, id: InstanceId) -> Option<&AnimationInstance> {
        self.instances.get(id.0 as usize).and_then(Option::as_ref)
    }

    /// An instance, mutably.
    pub fn instance_mut(&mut self, id: InstanceId) -> Option<&mut AnimationInstance> {
        self.instances
            .get_mut(id.0 as usize)
            .and_then(Option::as_mut)
    }

    /// `AddRef` on an instance.
    pub fn add_ref(&mut self, id: InstanceId) -> u32 {
        self.instance_mut(id).map_or(0, |i| {
            i.refs = i.refs.wrapping_add(1);
            i.refs
        })
    }

    /// `Release` on an instance: the last one unlinks it and drops its
    /// animation reference.
    pub fn release(&mut self, id: InstanceId) -> u32 {
        let Some(instance) = self.instance_mut(id) else {
            return 0;
        };
        instance.refs = instance.refs.saturating_sub(1);
        let refs = instance.refs;
        if refs == 0 {
            self.instances[id.0 as usize] = None;
        }
        refs
    }

    /// `TickAnimationInstances`: with filter -1, every live instance in
    /// creation order; any other filter does nothing.
    pub fn tick_instances(&mut self, filter: i32, dt: f32) -> i32 {
        if filter == -1 {
            for instance in self.instances.iter_mut().flatten() {
                instance.tick(dt);
            }
        }
        GR_OK
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn data(durations: &[f32]) -> AnimationData {
        AnimationData {
            crc: 7,
            count: durations.len() as u32,
            flags: 0,
            deltas: durations
                .iter()
                .map(|d| MaDelta {
                    duration: d.to_bits(),
                    rates: [1.0f32.to_bits(), 0, 0, 0],
                })
                .collect(),
            keys: Vec::new(),
        }
    }

    #[test]
    fn ticks_wrap_after_the_last_key() {
        let mut instance = AnimationInstance::new(Some(Rc::new(data(&[1.0, 2.0]))));
        instance.tick(1.5);
        assert_eq!((instance.frame, instance.time), (1, 0.5));
        instance.tick(2.0);
        assert_eq!((instance.frame, instance.time), (0, 0.5));
        instance.tick(-1.0);
        assert_eq!((instance.frame, instance.time), (0, 0.5));
    }

    #[test]
    fn zero_durations_do_not_hang() {
        let mut instance = AnimationInstance::new(Some(Rc::new(data(&[0.0, 0.0]))));
        instance.tick(1.0);
        assert_eq!(instance.time, 1.0);
    }

    #[test]
    fn uv_matrix_of_the_first_key() {
        let mut instance = AnimationInstance::new(Some(Rc::new(data(&[4.0]))));
        instance.tick(2.0);
        let uv = instance.uv_matrix().unwrap_or_else(|| panic!("data"));
        assert_eq!(uv[2], 2.0);
        assert_eq!(uv[0], 1.0);
        assert_eq!(uv[5], 1.0);
    }

    #[test]
    fn unload_removes_its_crcs() {
        let mut library = MaterialAnimations::new();
        let first = library
            .add_group(vec![data(&[1.0])])
            .unwrap_or_else(|| panic!("group"));
        let _second = library.add_group(vec![data(&[2.0])]);
        assert_eq!(library.map().len(), 1);
        let instance = library
            .create_instance(7)
            .unwrap_or_else(|_| panic!("instance"));
        library.unload_group(first);
        assert!(library.map().is_empty());
        assert_eq!(library.create_instance(7), Err(GR_GENERIC));
        assert!(library.instance(instance).is_some());
    }
}
