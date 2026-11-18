//! The appearance pools (`FAppearancePool` in `particlefx_types.h`, the
//! pool functions of `particlefx_common.cpp`): one per appearance class,
//! shared by every node of that class. A pool's items are in a free list
//! and a used list of links, in allocation order; when a billboard pool is
//! full, a spawn evicts the oldest used item from its owner's list. Each
//! appearance threads its own particles through the items' `next` and
//! `prev` (its instance list).

use super::node::NodeId;

/// The pools, in the order `FreeAllAppearancePools` lists the globals'
/// declarations (`G_PoolBasic` first).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum PoolKind {
    /// `FxBasicAppearance`.
    Basic,
    /// `FxParticleAppearance`.
    Particle,
    /// `FxRectAppearance`.
    Rect,
    /// `FxPerpAppearance`.
    Perp,
    /// `FxMeshAppearance`.
    Mesh,
    /// `FxOrientedAppearance`.
    Oriented,
    /// `FLDustAppearance`.
    Dust,
    /// `FLBeamAppearance`.
    Beam,
}

impl PoolKind {
    /// Every pool.
    pub const ALL: [Self; 8] = [
        Self::Basic,
        Self::Particle,
        Self::Rect,
        Self::Perp,
        Self::Mesh,
        Self::Oriented,
        Self::Dust,
        Self::Beam,
    ];

    /// The `[ParticleFx]` key of its size (`<class>.poolSize`).
    #[must_use]
    pub const fn ini_key(self) -> &'static str {
        match self {
            Self::Basic => "FxBasicAppearance.poolSize",
            Self::Particle => "FxParticleAppearance.poolSize",
            Self::Rect => "FxRectAppearance.poolSize",
            Self::Perp => "FxPerpAppearance.poolSize",
            Self::Mesh => "FxMeshAppearance.poolSize",
            Self::Oriented => "FxOrientedAppearance.poolSize",
            Self::Dust => "FLDustAppearance.poolSize",
            Self::Beam => "FLBeamAppearance.poolSize",
        }
    }
}

/// The size every pool has when the INI names none (`kDefaultPoolSize`).
pub const DEFAULT_POOL_SIZE: u32 = 1000;

/// One particle: its owner's list links, the owner, position and velocity
/// in the owner's space, age, duration and its reciprocal, and the three
/// words after them (perp: the billboard axis; dust: the last camera-space
/// position; beam: a zero word).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Particle {
    /// The next particle of the owner's list.
    pub next: Option<u32>,
    /// The previous particle.
    pub prev: Option<u32>,
    /// The appearance whose list holds it.
    pub owner: Option<NodeId>,
    /// Position.
    pub pos: [f32; 3],
    /// Velocity.
    pub vel: [f32; 3],
    /// Age.
    pub age: f32,
    /// Duration.
    pub duration: f32,
    /// `1 / duration`.
    pub inv_duration: f32,
    /// The words at +0x30.
    pub extra: [f32; 3],
}

/// A link of the free and used lists.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Link {
    next: Option<u32>,
    prev: Option<u32>,
}

/// A doubly linked list of links: head and tail.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct List {
    head: Option<u32>,
    tail: Option<u32>,
}

/// One pool.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Pool {
    items: Vec<Particle>,
    links: Vec<Link>,
    free_count: u32,
    used: List,
    free: List,
}

impl Pool {
    /// `AllocateAppearancePool(count)`: every item free, in index order.
    #[must_use]
    pub fn new(count: u32) -> Self {
        let mut pool = Self {
            items: vec![Particle::default(); count as usize],
            links: vec![Link::default(); count as usize],
            free_count: count,
            ..Self::default()
        };
        for index in 0..count {
            pool.append(false, index);
        }
        pool
    }

    /// The capacity.
    #[must_use]
    pub fn capacity(&self) -> u32 {
        u32::try_from(self.items.len()).unwrap_or(u32::MAX)
    }

    /// The free count.
    #[must_use]
    pub const fn free_count(&self) -> u32 {
        self.free_count
    }

    /// The used items, oldest first.
    #[must_use]
    pub fn used(&self) -> Vec<u32> {
        let mut out = Vec::new();
        let mut cursor = self.used.head;
        while let Some(index) = cursor {
            out.push(index);
            cursor = self.links[index as usize].next;
        }
        out
    }

    /// The oldest used item.
    #[must_use]
    pub const fn oldest(&self) -> Option<u32> {
        self.used.head
    }

    /// An item.
    #[must_use]
    pub fn item(&self, index: u32) -> &Particle {
        &self.items[index as usize]
    }

    /// An item, mutable.
    pub fn item_mut(&mut self, index: u32) -> &mut Particle {
        &mut self.items[index as usize]
    }

    fn list(&mut self, used: bool) -> &mut List {
        if used { &mut self.used } else { &mut self.free }
    }

    fn append(&mut self, used: bool, index: u32) {
        let tail = self.list(used).tail;
        self.links[index as usize] = Link {
            next: None,
            prev: tail,
        };
        match tail {
            Some(t) => self.links[t as usize].next = Some(index),
            None => self.list(used).head = Some(index),
        }
        self.list(used).tail = Some(index);
    }

    fn unlink(&mut self, used: bool, index: u32) {
        let Link { next, prev } = self.links[index as usize];
        match prev {
            Some(p) => self.links[p as usize].next = next,
            None => self.list(used).head = next,
        }
        match next {
            Some(n) => self.links[n as usize].prev = prev,
            None => self.list(used).tail = prev,
        }
        self.links[index as usize] = Link::default();
    }

    /// `AllocPoolItem`: the first free item moves to the end of the used
    /// list; `None` when no item is free.
    pub fn alloc(&mut self) -> Option<u32> {
        let index = self.free.head?;
        self.free_count -= 1;
        self.unlink(false, index);
        self.append(true, index);
        Some(index)
    }

    /// `FreePoolItem`: the item moves from the used list to the end of the
    /// free list.
    pub fn free(&mut self, index: u32) {
        self.unlink(true, index);
        self.append(false, index);
        self.free_count += 1;
    }

    /// `UnlinkParticleNode` on an owner's instance list.
    pub fn unlink_particle(&mut self, head: &mut Option<u32>, tail: &mut Option<u32>, index: u32) {
        let Particle { next, prev, .. } = self.items[index as usize];
        match prev {
            Some(p) => self.items[p as usize].next = next,
            None => *head = next,
        }
        match next {
            Some(n) => self.items[n as usize].prev = prev,
            None => *tail = prev,
        }
        let item = &mut self.items[index as usize];
        item.next = None;
        item.prev = None;
    }

    /// `AppendParticleNode`.
    pub fn append_particle(&mut self, head: &mut Option<u32>, tail: &mut Option<u32>, index: u32) {
        let item = &mut self.items[index as usize];
        item.next = None;
        item.prev = *tail;
        match *tail {
            Some(t) => self.items[t as usize].next = Some(index),
            None => *head = Some(index),
        }
        *tail = Some(index);
    }

    /// `PrependParticleNode`.
    pub fn prepend_particle(&mut self, head: &mut Option<u32>, tail: &mut Option<u32>, index: u32) {
        let item = &mut self.items[index as usize];
        item.prev = None;
        item.next = *head;
        if let Some(h) = *head {
            self.items[h as usize].prev = Some(index);
        }
        *head = Some(index);
        if tail.is_none() {
            *tail = Some(index);
        }
    }
}

/// The eight pools.
#[derive(Clone, Debug, PartialEq)]
pub struct Pools {
    pools: Vec<Pool>,
}

impl Pools {
    /// Every pool with `DEFAULT_POOL_SIZE` items.
    #[must_use]
    pub fn new() -> Self {
        Self {
            pools: PoolKind::ALL
                .iter()
                .map(|_| Pool::new(DEFAULT_POOL_SIZE))
                .collect(),
        }
    }

    /// A pool.
    #[must_use]
    pub fn get(&self, kind: PoolKind) -> &Pool {
        &self.pools[kind as usize]
    }

    /// A pool, mutable.
    pub fn get_mut(&mut self, kind: PoolKind) -> &mut Pool {
        &mut self.pools[kind as usize]
    }

    /// `AllocateAppearancePool(pool, count)`: the pool starts over with
    /// `count` free items. Particles still in an appearance's list are not
    /// unlinked from it, as in the reference; callers reallocate between
    /// effects.
    pub fn reallocate(&mut self, kind: PoolKind, count: u32) {
        self.pools[kind as usize] = Pool::new(count);
    }
}

impl Default for Pools {
    fn default() -> Self {
        Self::new()
    }
}
