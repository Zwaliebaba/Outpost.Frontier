//! Arenas with generational handles for renderer resources (record 0004,
//! point 2): a handle names a slot and the generation it was made in, so a
//! handle kept past `remove` is refused instead of reaching a new resource.

use std::fmt;
use std::marker::PhantomData;

/// A handle into an [`Arena<T>`].
pub struct Handle<T> {
    index: u32,
    generation: u32,
    kind: PhantomData<fn() -> T>,
}

impl<T> Handle<T> {
    /// The slot index, for diagnostics and stable trace ids.
    #[must_use]
    pub fn index(self) -> u32 {
        self.index
    }

    #[must_use]
    pub fn generation(self) -> u32 {
        self.generation
    }
}

impl<T> Clone for Handle<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for Handle<T> {}

impl<T> PartialEq for Handle<T> {
    fn eq(&self, other: &Self) -> bool {
        self.index == other.index && self.generation == other.generation
    }
}

impl<T> Eq for Handle<T> {}

impl<T> std::hash::Hash for Handle<T> {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.index.hash(state);
        self.generation.hash(state);
    }
}

impl<T> fmt::Debug for Handle<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Handle({}v{})", self.index, self.generation)
    }
}

/// A handle that names no live resource.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StaleHandle;

impl fmt::Display for StaleHandle {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("the handle names no live resource")
    }
}

impl std::error::Error for StaleHandle {}

struct Slot<T> {
    generation: u32,
    value: Option<T>,
}

/// Values of one kind, by handle.
pub struct Arena<T> {
    slots: Vec<Slot<T>>,
    free: Vec<u32>,
    len: usize,
}

impl<T> Default for Arena<T> {
    fn default() -> Self {
        Arena {
            slots: Vec::new(),
            free: Vec::new(),
            len: 0,
        }
    }
}

impl<T: fmt::Debug> fmt::Debug for Arena<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_map().entries(self.iter()).finish()
    }
}

impl<T> Arena<T> {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub fn insert(&mut self, value: T) -> Handle<T> {
        self.len += 1;
        let index = match self.free.pop() {
            Some(index) => {
                let slot = &mut self.slots[index as usize];
                slot.value = Some(value);
                index
            }
            None => {
                self.slots.push(Slot {
                    generation: 0,
                    value: Some(value),
                });
                u32::try_from(self.slots.len() - 1).expect("fewer than 2^32 resources")
            }
        };
        Handle {
            index,
            generation: self.slots[index as usize].generation,
            kind: PhantomData,
        }
    }

    pub fn get(&self, handle: Handle<T>) -> Result<&T, StaleHandle> {
        self.slots
            .get(handle.index as usize)
            .filter(|s| s.generation == handle.generation)
            .and_then(|s| s.value.as_ref())
            .ok_or(StaleHandle)
    }

    pub fn get_mut(&mut self, handle: Handle<T>) -> Result<&mut T, StaleHandle> {
        self.slots
            .get_mut(handle.index as usize)
            .filter(|s| s.generation == handle.generation)
            .and_then(|s| s.value.as_mut())
            .ok_or(StaleHandle)
    }

    /// Removes the value; the slot's next value gets a new generation.
    pub fn remove(&mut self, handle: Handle<T>) -> Result<T, StaleHandle> {
        let slot = self
            .slots
            .get_mut(handle.index as usize)
            .filter(|s| s.generation == handle.generation)
            .ok_or(StaleHandle)?;
        let value = slot.value.take().ok_or(StaleHandle)?;
        slot.generation = slot.generation.wrapping_add(1);
        self.free.push(handle.index);
        self.len -= 1;
        Ok(value)
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.len
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// The live values with their handles, by slot.
    pub fn iter(&self) -> impl Iterator<Item = (Handle<T>, &T)> {
        self.slots.iter().enumerate().filter_map(|(i, s)| {
            s.value.as_ref().map(|v| {
                (
                    Handle {
                        index: i as u32,
                        generation: s.generation,
                        kind: PhantomData,
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
    fn stale_handles_are_refused() {
        let mut a = Arena::new();
        let h = a.insert(5);
        assert_eq!(a.get(h), Ok(&5));
        assert_eq!(a.remove(h), Ok(5));
        assert_eq!(a.get(h), Err(StaleHandle));
        assert_eq!(a.remove(h), Err(StaleHandle));
        let g = a.insert(7);
        assert_eq!(g.index(), h.index());
        assert_ne!(g, h);
        assert_eq!(a.get(h), Err(StaleHandle));
        *a.get_mut(g).unwrap() += 1;
        assert_eq!(a.iter().map(|(_, v)| *v).collect::<Vec<_>>(), [8]);
        assert_eq!(a.len(), 1);
    }
}
