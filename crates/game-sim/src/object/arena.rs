//! The object arena with generational handles (decision record 0001,
//! point 1): a handle to a destroyed object resolves to `None`.

/// A handle to a world object. `Copy`; resolves to `None` once the object
/// is reaped (decision record 0001, points 1 and 4).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ObjHandle {
    index: u32,
    generation: u32,
}

impl ObjHandle {
    /// The slot index.
    #[must_use]
    pub const fn index(self) -> u32 {
        self.index
    }

    /// The slot's generation when the handle was made.
    #[must_use]
    pub const fn generation(self) -> u32 {
        self.generation
    }

    /// The handle as one 64-bit key (the physics bodies' and controllers'
    /// user key).
    #[must_use]
    pub const fn to_key(self) -> u64 {
        ((self.generation as u64) << 32) | self.index as u64
    }

    /// The handle a key stands for.
    #[must_use]
    pub const fn from_key(key: u64) -> Self {
        Self {
            index: key as u32,
            generation: (key >> 32) as u32,
        }
    }
}

#[derive(Clone, Debug)]
struct Slot<T> {
    generation: u32,
    value: Option<T>,
}

/// Slots of `T` addressed by [`ObjHandle`]. Freed slots are reused with a
/// new generation; iteration order is slot order, so callers that need
/// the C++ insertion order keep their own lists.
#[derive(Clone, Debug)]
pub struct Arena<T> {
    slots: Vec<Slot<T>>,
    free: Vec<u32>,
    len: usize,
}

impl<T> Default for Arena<T> {
    fn default() -> Self {
        Self {
            slots: Vec::new(),
            free: Vec::new(),
            len: 0,
        }
    }
}

impl<T> Arena<T> {
    /// Stores a value; the handle stays valid until it is removed.
    pub fn insert(&mut self, value: T) -> ObjHandle {
        self.len += 1;
        if let Some(index) = self.free.pop() {
            let slot = &mut self.slots[index as usize];
            slot.value = Some(value);
            return ObjHandle {
                index,
                generation: slot.generation,
            };
        }
        let index = u32::try_from(self.slots.len()).unwrap_or(u32::MAX);
        self.slots.push(Slot {
            generation: 0,
            value: Some(value),
        });
        ObjHandle {
            index,
            generation: 0,
        }
    }

    /// Removes the value; the slot's generation moves on, so every handle
    /// to it reads as gone.
    pub fn remove(&mut self, handle: ObjHandle) -> Option<T> {
        let slot = self.slots.get_mut(handle.index as usize)?;
        if slot.generation != handle.generation {
            return None;
        }
        let value = slot.value.take()?;
        slot.generation = slot.generation.wrapping_add(1);
        self.free.push(handle.index);
        self.len -= 1;
        Some(value)
    }

    /// The value, if the handle is current.
    #[must_use]
    pub fn get(&self, handle: ObjHandle) -> Option<&T> {
        let slot = self.slots.get(handle.index as usize)?;
        if slot.generation != handle.generation {
            return None;
        }
        slot.value.as_ref()
    }

    /// The value, mutably.
    pub fn get_mut(&mut self, handle: ObjHandle) -> Option<&mut T> {
        let slot = self.slots.get_mut(handle.index as usize)?;
        if slot.generation != handle.generation {
            return None;
        }
        slot.value.as_mut()
    }

    /// Whether the handle is current.
    #[must_use]
    pub fn contains(&self, handle: ObjHandle) -> bool {
        self.get(handle).is_some()
    }

    /// The live values.
    #[must_use]
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether nothing is stored.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Every live handle and value, in slot order.
    pub fn iter(&self) -> impl Iterator<Item = (ObjHandle, &T)> {
        self.slots.iter().enumerate().filter_map(|(i, s)| {
            s.value.as_ref().map(|v| {
                (
                    ObjHandle {
                        index: i as u32,
                        generation: s.generation,
                    },
                    v,
                )
            })
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stale_handles_read_as_gone() {
        let mut a = Arena::default();
        let h = a.insert(1);
        assert_eq!(a.get(h), Some(&1));
        assert_eq!(a.remove(h), Some(1));
        assert_eq!(a.get(h), None);
        let h2 = a.insert(2);
        assert_eq!(h2.index(), h.index());
        assert_ne!(h2, h);
        assert_eq!(a.get(h), None);
        assert_eq!(a.get(h2), Some(&2));
        assert_eq!(a.remove(h), None);
        assert_eq!(a.len(), 1);
    }

    #[test]
    fn keys_round_trip() {
        let mut a = Arena::default();
        let first = a.insert(());
        let _ = a.remove(first);
        let h = a.insert(());
        assert_eq!(ObjHandle::from_key(h.to_key()), h);
        assert_eq!(h.generation(), 1);
    }
}
