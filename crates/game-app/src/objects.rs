//! The client's object database (`ClientObjDb_*`, `client_objdb.cpp`; work
//! package P5.4a, record 0009 point 4): the game objects the client knows,
//! in the ten buckets the C++ keeps by `ManagerKind`, each in link order.
//!
//! The interface the other streams read:
//!
//! - [`ClientObjects`]: registration ([`ClientObjects::register`],
//!   [`ClientObjects::unregister`]), the finders with the C++ search
//!   orders ([`ClientObjects::find`], [`ClientObjects::find_by_handle`],
//!   [`ClientObjects::find_eq_obj_by_vibe`]), the walks
//!   ([`ClientObjects::bucket`], [`ClientObjects::iter`],
//!   [`ClientObjects::large_solars`]), the counts and position queries, and
//!   the system's spatial set ([`ClientObjects::free_spatial`],
//!   [`ClientObjects::rebuild_spatial`], [`ClientObjects::spatial`]).
//! - [`ObjectView`]: where an object's pose, radius, type and vibe are
//!   read. In single player the shared [`World`] is the view (record 0002:
//!   the client adds its own state beside the server's object rather than a
//!   second object), through the handle the create message found. The
//!   multiplayer view (the client's own simulation) is P5.10's.
//!
//! The game objects' own state (engine instances, effects, the remote
//! physics simulation) is P5.4b's; [`ClientObject::remote_physics`] only
//! records that `ClientObjDb_RegisterGameobj` was asked for the block.

use game_sim::object::{ObjHandle, ObjectKind, World};
use neuron_core::math::{Matrix, Vector};

/// `kLargeSolarRadiusThreshold`: a solar above this radius is drawn and
/// updated as a large solar.
pub const LARGE_SOLAR_RADIUS: f32 = 200.0;

/// `kSpatialUpdateRadius`: the sphere around the listener camera within
/// which `ClientObjDb_UpdateAll` updates the partition's solars.
pub const SPATIAL_UPDATE_RADIUS: f32 = 10000.0;

/// The id bits an object id must have one of (`FindObject`'s gate).
const ID_MASK: u32 = 0x3fff_ffff;

/// The type bit that keeps a big solar out of the large-solar list.
const TYPE_NOT_LARGE: u32 = 0x80;

/// The type bits that put a solar in the spatial partition.
const TYPE_SPATIAL: u32 = 0xfdff;

/// `FClientGameobj::ManagerKind`: the object's bucket, 1 to 10.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ClientObjectKind {
    /// 1, `CShip`.
    Ship = 1,
    /// 2, `CSolar`.
    Solar = 2,
    /// 3, `CBeam`.
    Beam = 3,
    /// 4, `CGuided`.
    Guided = 4,
    /// 5, `CMine`.
    Mine = 5,
    /// 6, `CCounterMeasure`.
    CounterMeasure = 6,
    /// 7, `CLoot`.
    Loot = 7,
    /// 8, `CAsteroid`.
    Asteroid = 8,
    /// 9, `CDynamicAsteroid`.
    DynamicAsteroid = 9,
    /// 10, a plain `CObject`.
    Object = 10,
}

impl ClientObjectKind {
    /// The kinds in bucket order.
    pub const ALL: [Self; 10] = [
        Self::Ship,
        Self::Solar,
        Self::Beam,
        Self::Guided,
        Self::Mine,
        Self::CounterMeasure,
        Self::Loot,
        Self::Asteroid,
        Self::DynamicAsteroid,
        Self::Object,
    ];

    /// The bucket number.
    #[must_use]
    pub fn bucket(self) -> u32 {
        self as u32
    }

    /// The kind of bucket `bucket`; `None` outside 1 to 10.
    #[must_use]
    pub fn from_bucket(bucket: u32) -> Option<Self> {
        Self::ALL
            .get(usize::try_from(bucket.checked_sub(1)?).ok()?)
            .copied()
    }

    /// `ClientObjDb_FindByCObject`'s class table; `None` for the classes
    /// no game object wraps (the equipment object).
    #[must_use]
    pub fn of(kind: ObjectKind) -> Option<Self> {
        Some(match kind {
            ObjectKind::Ship => Self::Ship,
            ObjectKind::Solar => Self::Solar,
            ObjectKind::Beam => Self::Beam,
            ObjectKind::Guided => Self::Guided,
            ObjectKind::Mine => Self::Mine,
            ObjectKind::CounterMeasure => Self::CounterMeasure,
            ObjectKind::Loot => Self::Loot,
            ObjectKind::Asteroid => Self::Asteroid,
            ObjectKind::DynamicAsteroid => Self::DynamicAsteroid,
            ObjectKind::EquipmentObject => return None,
        })
    }

    fn index(self) -> usize {
        self as usize - 1
    }
}

/// A registered game object's key: the registration it came from. Two
/// registrations of one id are two keys, as the C++ links two game
/// objects.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct GameobjKey(u64);

/// One registered game object.
#[derive(Clone, Debug, PartialEq)]
pub struct ClientObject {
    /// The object's id (`get_id`).
    pub id: u32,
    /// Its bucket.
    pub kind: ClientObjectKind,
    /// The archetype's id (the nickname hash).
    pub archetype: u32,
    /// Single player: the shared world's object (`Gameobj::find`).
    pub handle: Option<ObjHandle>,
    /// Registered with a remote-physics block (multiplayer ships).
    pub remote_physics: bool,
}

/// What [`ObjectView`] reads of an object (`IObjInspect`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ObjectPose {
    /// `get_position` (`CObject` +0x2c).
    pub position: Vector,
    /// `get_orientation`.
    pub orientation: Matrix,
    /// The radius (`CObject` +0x38).
    pub radius: f32,
    /// `get_type`: the archetype's type bits; `None` where the C++ call
    /// fails (an object without a `CSimple`).
    pub type_bits: Option<u32>,
    /// `CEqObj` +0x104, the reputation group; `None` for an object that is
    /// no `CEqObj`.
    pub vibe: Option<i32>,
}

/// Where the client reads its objects' state.
pub trait ObjectView {
    /// The object's pose; `None` when it has no `CObject` (the C++ skips
    /// those in its position queries).
    fn pose(&self, object: &ClientObject) -> Option<ObjectPose>;
}

impl ObjectView for World {
    fn pose(&self, object: &ClientObject) -> Option<ObjectPose> {
        let o = self.get(object.handle?)?;
        let simple = !matches!(
            o.kind,
            ObjectKind::EquipmentObject | ObjectKind::DynamicAsteroid
        );
        Some(ObjectPose {
            position: o.engine.position,
            orientation: o.engine.orientation,
            radius: o.engine.radius,
            type_bits: simple.then_some(o.simple.type_bits),
            vibe: o.equipped.as_ref().map(|e| e.vibe),
        })
    }
}

/// A view with no objects: every query answers as for an object without a
/// `CObject`.
impl ObjectView for () {
    fn pose(&self, _: &ClientObject) -> Option<ObjectPose> {
        None
    }
}

/// An entry of the spatial partition (`FSpatialEntry`): the object, and
/// its position and radius when the partition was built.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpatialEntry {
    /// The game object.
    pub key: GameobjKey,
    /// `CSimple` +0x2c.
    pub position: Vector,
    /// `CSimple` +0x38.
    pub radius: f32,
}

/// `ClientObjDb` (`FIObjDB` and the large-solar vector).
#[derive(Clone, Debug, Default)]
pub struct ClientObjects {
    buckets: [Vec<(GameobjKey, ClientObject)>; 10],
    next_key: u64,
    large_solars: Vec<GameobjKey>,
    spatial: Option<Vec<SpatialEntry>>,
    /// `PhySys::AnyActive()`: a system is entered, so creates are taken
    /// (`StarSysEnter` makes it true, leaving the system false).
    pub physics_active: bool,
}

impl ClientObjects {
    /// `ClientObjDb_RegisterGameobj`: the object linked at the end of its
    /// bucket. A solar above [`LARGE_SOLAR_RADIUS`] whose type has bit 7
    /// clear also goes into the large-solar list; with the partition up, a
    /// solar whose type has a bit of 0xfdff goes into it.
    pub fn register(&mut self, object: ClientObject, view: &dyn ObjectView) -> GameobjKey {
        let key = GameobjKey(self.next_key);
        self.next_key += 1;
        if object.kind == ClientObjectKind::Solar {
            let pose = view.pose(&object);
            if let Some(p) = pose {
                // The out value is written on every path: a failed get_type
                // reads as 0.
                if p.radius > LARGE_SOLAR_RADIUS && p.type_bits.unwrap_or(0) & TYPE_NOT_LARGE == 0 {
                    self.large_solars.push(key);
                }
                if let Some(spatial) = self.spatial.as_mut()
                    && p.type_bits.unwrap_or(0) & TYPE_SPATIAL != 0
                {
                    spatial.push(SpatialEntry {
                        key,
                        position: p.position,
                        radius: p.radius,
                    });
                }
            }
        }
        self.buckets[object.kind.index()].push((key, object));
        key
    }

    /// `ClientObjDb_Unregister`: the object unlinked from its bucket, and a
    /// solar from the large-solar list and the partition. The object is
    /// returned (the C++ callers delete it).
    pub fn unregister(&mut self, key: GameobjKey) -> Option<ClientObject> {
        let (bucket, at) = self.locate(key)?;
        let (_, object) = self.buckets[bucket].remove(at);
        if object.kind == ClientObjectKind::Solar {
            if let Some(at) = self.large_solars.iter().position(|&k| k == key) {
                self.large_solars.remove(at);
            }
            if let Some(spatial) = self.spatial.as_mut() {
                spatial.retain(|e| e.key != key);
            }
        }
        Some(object)
    }

    /// The object registered under `key`.
    #[must_use]
    pub fn get(&self, key: GameobjKey) -> Option<&ClientObject> {
        let (bucket, at) = self.locate(key)?;
        Some(&self.buckets[bucket][at].1)
    }

    /// The object registered under `key`, mutably.
    pub fn get_mut(&mut self, key: GameobjKey) -> Option<&mut ClientObject> {
        let (bucket, at) = self.locate(key)?;
        Some(&mut self.buckets[bucket][at].1)
    }

    fn locate(&self, key: GameobjKey) -> Option<(usize, usize)> {
        self.buckets
            .iter()
            .enumerate()
            .find_map(|(b, list)| list.iter().position(|(k, _)| *k == key).map(|at| (b, at)))
    }

    /// `ClientObjDb_FindObject(id, bucket)`: none for an id without a bit
    /// under 0x3fffffff; bucket 0 searches buckets 1, 4, 5, 6, 7 and 2 in
    /// that order, any other bucket only itself. The first in link order
    /// wins.
    #[must_use]
    pub fn find(&self, id: u32, bucket: u32) -> Option<(GameobjKey, &ClientObject)> {
        if id & ID_MASK == 0 {
            return None;
        }
        if bucket != 0 {
            return self.find_in(ClientObjectKind::from_bucket(bucket)?, |o| o.id == id);
        }
        [
            ClientObjectKind::Ship,
            ClientObjectKind::Guided,
            ClientObjectKind::Mine,
            ClientObjectKind::CounterMeasure,
            ClientObjectKind::Loot,
            ClientObjectKind::Solar,
        ]
        .into_iter()
        .find_map(|kind| self.find_in(kind, |o| o.id == id))
    }

    /// `ClientObjDb_FindEqObjByVibeId(vibe, bucket)`: the same id gate;
    /// bucket 0 the ship, else the solar, whose `CEqObj` vibe is `vibe`;
    /// any other bucket that bucket's object whose id is `vibe`.
    #[must_use]
    pub fn find_eq_obj_by_vibe(
        &self,
        view: &dyn ObjectView,
        vibe: u32,
        bucket: u32,
    ) -> Option<(GameobjKey, &ClientObject)> {
        if vibe & ID_MASK == 0 {
            return None;
        }
        if bucket != 0 {
            return self.find_in(ClientObjectKind::from_bucket(bucket)?, |o| o.id == vibe);
        }
        let matches = |o: &ClientObject| {
            view.pose(o)
                .and_then(|p| p.vibe)
                .is_some_and(|v| u32::from_ne_bytes(v.to_ne_bytes()) == vibe)
        };
        self.find_in(ClientObjectKind::Ship, matches)
            .or_else(|| self.find_in(ClientObjectKind::Solar, matches))
    }

    /// `ClientObjDb_FindByCObject`: the game object wrapping the world's
    /// object, in the bucket its class selects.
    #[must_use]
    pub fn find_by_handle(
        &self,
        kind: ObjectKind,
        handle: ObjHandle,
    ) -> Option<(GameobjKey, &ClientObject)> {
        self.find_in(ClientObjectKind::of(kind)?, |o| o.handle == Some(handle))
    }

    fn find_in(
        &self,
        kind: ClientObjectKind,
        mut pred: impl FnMut(&ClientObject) -> bool,
    ) -> Option<(GameobjKey, &ClientObject)> {
        self.buckets[kind.index()]
            .iter()
            .find(|(_, o)| pred(o))
            .map(|(k, o)| (*k, o))
    }

    /// `ClientObjDb_GetKindHead` and the `ListNext` chain: one bucket in
    /// link order.
    pub fn bucket(
        &self,
        kind: ClientObjectKind,
    ) -> impl Iterator<Item = (GameobjKey, &ClientObject)> {
        self.buckets[kind.index()].iter().map(|(k, o)| (*k, o))
    }

    /// Every object, bucket 1 to 10, each in link order (the walk of the
    /// position queries).
    pub fn iter(&self) -> impl Iterator<Item = (GameobjKey, &ClientObject)> {
        self.buckets.iter().flatten().map(|(k, o)| (*k, o))
    }

    /// `ClientObjDb_GetRegisteredCount`.
    #[must_use]
    pub fn len(&self) -> usize {
        self.buckets.iter().map(Vec::len).sum()
    }

    /// True when none is registered.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// `ClientObjDb_GetSolarCount`.
    #[must_use]
    pub fn solar_count(&self) -> usize {
        self.buckets[ClientObjectKind::Solar.index()].len()
    }

    /// `ClientObjDb_TryGetWorldPosById`: the first object, bucket order,
    /// with a `CObject` and the id; none for id 0.
    #[must_use]
    pub fn world_pos_by_id(&self, view: &dyn ObjectView, id: u32) -> Option<Vector> {
        if id == 0 {
            return None;
        }
        self.iter()
            .filter(|(_, o)| o.id == id)
            .find_map(|(_, o)| view.pose(o))
            .map(|p| p.position)
    }

    /// `ClientObjDb_TryGetWorldPosByArchetypeId`: the same by archetype.
    #[must_use]
    pub fn world_pos_by_archetype(&self, view: &dyn ObjectView, archetype: u32) -> Option<Vector> {
        if archetype == 0 {
            return None;
        }
        self.iter()
            .filter(|(_, o)| o.archetype == archetype)
            .find_map(|(_, o)| view.pose(o))
            .map(|p| p.position)
    }

    /// `ClientObjDb_TryGetSolarWorldPosByIndex`: the `index`th solar with a
    /// `CObject`, in link order.
    #[must_use]
    pub fn solar_world_pos_by_index(&self, view: &dyn ObjectView, index: usize) -> Option<Vector> {
        self.bucket(ClientObjectKind::Solar)
            .filter_map(|(_, o)| view.pose(o))
            .nth(index)
            .map(|p| p.position)
    }

    /// The large-solar list (`ClientObjDb_LargeSolarFirst`/`Next`), in
    /// registration order.
    #[must_use]
    pub fn large_solars(&self) -> &[GameobjKey] {
        &self.large_solars
    }

    /// `ClientObjDb_IsLargeSolar`.
    #[must_use]
    pub fn is_large_solar(&self, key: GameobjKey) -> bool {
        self.large_solars.contains(&key)
    }

    /// The spatial partition's entries; `None` while there is none.
    #[must_use]
    pub fn spatial(&self) -> Option<&[SpatialEntry]> {
        self.spatial.as_deref()
    }

    /// `ClientObjDb_FreeSpatial` (`StarSysLoadIni`'s entry).
    pub fn free_spatial(&mut self) {
        self.spatial = None;
    }

    /// `ClientObjDb_RebuildSpatial` (`StarSysLoadIni`'s exit): with no
    /// solar the partition is only emptied; otherwise it is made and holds
    /// every solar whose `get_type` succeeds with a bit of 0xfdff, in link
    /// order.
    pub fn rebuild_spatial(&mut self, view: &dyn ObjectView) {
        if let Some(spatial) = self.spatial.as_mut() {
            spatial.clear();
        }
        if self.solar_count() == 0 {
            return;
        }
        let entries = self
            .bucket(ClientObjectKind::Solar)
            .filter_map(|(key, o)| {
                let p = view.pose(o)?;
                (p.type_bits? & TYPE_SPATIAL != 0).then_some(SpatialEntry {
                    key,
                    position: p.position,
                    radius: p.radius,
                })
            })
            .collect::<Vec<_>>();
        self.spatial.get_or_insert_with(Vec::new).extend(entries);
    }

    /// `ClientObjDb_ClearExcept(keep)`: every object not in `keep` removed,
    /// the large-solar list emptied and the partition freed.
    pub fn clear_except(&mut self, keep: &[GameobjKey]) {
        for list in &mut self.buckets {
            list.retain(|(k, _)| keep.contains(k));
        }
        self.large_solars.clear();
        self.free_spatial();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A view of fixed poses by id.
    struct Poses(Vec<(u32, ObjectPose)>);

    impl ObjectView for Poses {
        fn pose(&self, o: &ClientObject) -> Option<ObjectPose> {
            self.0.iter().find(|(id, _)| *id == o.id).map(|(_, p)| *p)
        }
    }

    fn pose(x: f32, radius: f32, type_bits: u32, vibe: i32) -> ObjectPose {
        ObjectPose {
            position: Vector { x, y: 0.0, z: 0.0 },
            orientation: Matrix::default(),
            radius,
            type_bits: Some(type_bits),
            vibe: Some(vibe),
        }
    }

    fn object(id: u32, kind: ClientObjectKind) -> ClientObject {
        ClientObject {
            id,
            kind,
            archetype: id + 1000,
            handle: None,
            remote_physics: false,
        }
    }

    #[test]
    fn the_finders_search_the_cpp_buckets_in_order() {
        let view = Poses(vec![
            (40, pose(4.0, 300.0, 1, 7)),
            (11, pose(1.0, 5.0, 1, 7)),
        ]);
        let mut db = ClientObjects::default();
        let solar = db.register(object(40, ClientObjectKind::Solar), &view);
        let loot = db.register(object(40, ClientObjectKind::Loot), &view);
        let ship = db.register(object(11, ClientObjectKind::Ship), &view);
        // Bucket 0: loot (7) is searched before solars (2).
        assert_eq!(db.find(40, 0).map(|(k, _)| k), Some(loot));
        assert_eq!(db.find(40, 2).map(|(k, _)| k), Some(solar));
        assert!(db.find(11, 2).is_none() && db.find(11, 11).is_none());
        assert!(db.find(0x4000_0000, 0).is_none());
        // The vibe: the ship before the solar; a bucket compares the id.
        assert_eq!(
            db.find_eq_obj_by_vibe(&view, 7, 0).map(|(k, _)| k),
            Some(ship)
        );
        assert_eq!(
            db.find_eq_obj_by_vibe(&view, 40, 2).map(|(k, _)| k),
            Some(solar)
        );
        assert_eq!(db.world_pos_by_id(&view, 40).map(|p| p.x), Some(4.0));
        assert_eq!(
            db.world_pos_by_archetype(&view, 1011).map(|p| p.x),
            Some(1.0)
        );
        assert_eq!(db.len(), 3);
        assert_eq!(db.iter().next().map(|(k, _)| k), Some(ship));
    }

    #[test]
    fn large_solars_and_the_partition_follow_the_load() {
        let view = Poses(vec![
            (1, pose(0.0, 300.0, 1, -1)),
            (2, pose(0.0, 300.0, 0x80, -1)),
            (3, pose(0.0, 100.0, 0x200, -1)),
        ]);
        let mut db = ClientObjects::default();
        // StarSysLoadIni: freed at the start, solars registered, rebuilt.
        db.free_spatial();
        let keys: Vec<_> = (1..=3)
            .map(|id| db.register(object(id, ClientObjectKind::Solar), &view))
            .collect();
        assert!(db.spatial().is_none());
        assert_eq!(db.large_solars(), &keys[..1]);
        db.rebuild_spatial(&view);
        // 0x200 alone is outside 0xfdff.
        let spatial: Vec<_> = db.spatial().unwrap().iter().map(|e| e.key).collect();
        assert_eq!(spatial, keys[..2]);
        assert_eq!(db.unregister(keys[0]).map(|o| o.id), Some(1));
        assert!(db.large_solars().is_empty());
        assert_eq!(db.spatial().unwrap().len(), 1);
        db.clear_except(&[keys[2]]);
        assert_eq!(db.len(), 1);
        assert!(db.spatial().is_none());
        assert_eq!(
            db.solar_world_pos_by_index(&view, 0).map(|p| p.x),
            Some(0.0)
        );
    }
}
