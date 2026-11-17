//! A small generational arena for bodies, cores, phantoms and controllers
//! (the pattern of decision record 0001; no dependency).

/// A generational index into an [`Arena`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Index {
    /// The slot.
    pub slot: u32,
    /// The slot's generation when the value was inserted.
    pub generation: u32,
}

#[derive(Clone, Debug)]
struct Slot<T> {
    generation: u32,
    value: Option<T>,
}

/// Values in slots; a freed slot is reused with the next generation.
/// Iteration is in slot order.
#[derive(Clone, Debug)]
pub struct Arena<T> {
    slots: Vec<Slot<T>>,
    free: Vec<u32>,
}

impl<T> Default for Arena<T> {
    fn default() -> Self {
        Self {
            slots: Vec::new(),
            free: Vec::new(),
        }
    }
}

impl<T> Arena<T> {
    /// Stores a value.
    pub fn insert(&mut self, value: T) -> Index {
        if let Some(slot) = self.free.pop() {
            let entry = &mut self.slots[slot as usize];
            entry.value = Some(value);
            return Index {
                slot,
                generation: entry.generation,
            };
        }
        let slot = self.slots.len() as u32;
        self.slots.push(Slot {
            generation: 0,
            value: Some(value),
        });
        Index {
            slot,
            generation: 0,
        }
    }

    /// Takes a value out; its index no longer resolves.
    pub fn remove(&mut self, index: Index) -> Option<T> {
        let entry = self.slots.get_mut(index.slot as usize)?;
        if entry.generation != index.generation {
            return None;
        }
        let value = entry.value.take()?;
        entry.generation = entry.generation.wrapping_add(1);
        self.free.push(index.slot);
        Some(value)
    }

    /// The value at an index, if it is still there.
    #[must_use]
    pub fn get(&self, index: Index) -> Option<&T> {
        let entry = self.slots.get(index.slot as usize)?;
        if entry.generation != index.generation {
            return None;
        }
        entry.value.as_ref()
    }

    /// The value at an index, mutably.
    pub fn get_mut(&mut self, index: Index) -> Option<&mut T> {
        let entry = self.slots.get_mut(index.slot as usize)?;
        if entry.generation != index.generation {
            return None;
        }
        entry.value.as_mut()
    }

    /// The live indices in slot order.
    #[must_use]
    pub fn indices(&self) -> Vec<Index> {
        self.slots
            .iter()
            .enumerate()
            .filter(|(_, s)| s.value.is_some())
            .map(|(i, s)| Index {
                slot: i as u32,
                generation: s.generation,
            })
            .collect()
    }

    /// The number of live values.
    #[cfg(test)]
    #[must_use]
    pub fn len(&self) -> usize {
        self.slots.iter().filter(|s| s.value.is_some()).count()
    }

    /// Whether no value is live.
    #[cfg(test)]
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generations_invalidate_old_indices() {
        let mut arena = Arena::default();
        let a = arena.insert(1);
        let b = arena.insert(2);
        assert_eq!(arena.remove(a), Some(1));
        assert_eq!(arena.get(a), None);
        let c = arena.insert(3);
        assert_eq!(c.slot, a.slot);
        assert_ne!(c.generation, a.generation);
        assert_eq!(arena.indices(), vec![c, b]);
        *arena.get_mut(b).unwrap() = 5;
        assert_eq!(arena.get(b), Some(&5));
        assert_eq!(arena.len(), 2);
        assert!(!arena.is_empty());
        assert_eq!(arena.remove(a), None);
    }
}
