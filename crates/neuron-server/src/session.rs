//! Session slots (`PlayerDB`'s player array, `Server/server_classes.cpp`):
//! a fixed array of `MaxPlayers` slots addressed by 1-based session ids,
//! where a slot is in use while its client id is not 0.
//!
//! The slot's contents are the game side's (`PlayerData`, P3.2); this is
//! the addressing and the walk order the server relies on.

/// What a slot must tell the table.
pub trait SessionSlot: Default {
    /// The slot's client id; 0 for a free slot.
    fn client_id(&self) -> u32;
}

/// The slots of `PlayerDB::init(MaxPlayers, ...)`.
#[derive(Clone, Debug)]
pub struct SessionTable<T> {
    slots: Vec<T>,
}

impl<T: SessionSlot> SessionTable<T> {
    /// `max_players` default slots (`new PlayerData[MaxPlayers]`).
    #[must_use]
    pub fn new(max_players: usize) -> Self {
        Self {
            slots: (0..max_players).map(|_| T::default()).collect(),
        }
    }

    /// `MaxPlayers`.
    #[must_use]
    pub fn max_players(&self) -> usize {
        self.slots.len()
    }

    /// `PlayerDB::operator[]`: the slot of session `id` (1-based). The C++
    /// does not check the id; the port answers `None` outside the array.
    #[must_use]
    pub fn get(&self, id: u32) -> Option<&T> {
        let index = usize::try_from(id).ok()?.checked_sub(1)?;
        self.slots.get(index)
    }

    /// The slot of session `id`, mutably.
    pub fn get_mut(&mut self, id: u32) -> Option<&mut T> {
        let index = usize::try_from(id).ok()?.checked_sub(1)?;
        self.slots.get_mut(index)
    }

    /// `PlayerDB::is_valid`: `id` is not 0, within the array, and its slot
    /// has a client id.
    #[must_use]
    pub fn is_valid(&self, id: u32) -> bool {
        self.get(id).is_some_and(|slot| slot.client_id() != 0)
    }

    /// `PlayerDB::traverse_active` from the start: the session ids of the
    /// slots in use, in array order.
    pub fn active_ids(&self) -> impl Iterator<Item = u32> + '_ {
        self.slots
            .iter()
            .enumerate()
            .filter(|(_, slot)| slot.client_id() != 0)
            .map(|(index, _)| index as u32 + 1)
    }

    /// `traverse_active(current)`: the next slot in use after session
    /// `after` (from the start for `None`). A walk that calls out between
    /// steps reads the array again at each step, as the C++'s does.
    #[must_use]
    pub fn next_active(&self, after: Option<u32>) -> Option<u32> {
        let start = after.map_or(0, |id| id as usize);
        self.slots
            .iter()
            .enumerate()
            .skip(start)
            .find(|(_, slot)| slot.client_id() != 0)
            .map(|(index, _)| index as u32 + 1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Slot(u32);

    impl SessionSlot for Slot {
        fn client_id(&self) -> u32 {
            self.0
        }
    }

    #[test]
    fn ids_are_one_based_and_walks_skip_free_slots() {
        let mut table: SessionTable<Slot> = SessionTable::new(4);
        assert!(table.get(0).is_none() && table.get(5).is_none());
        table.get_mut(2).unwrap().0 = 2;
        table.get_mut(4).unwrap().0 = 4;
        assert!(!table.is_valid(1) && table.is_valid(2) && !table.is_valid(0));
        assert_eq!(table.active_ids().collect::<Vec<_>>(), [2, 4]);
        assert_eq!(table.next_active(None), Some(2));
        assert_eq!(table.next_active(Some(2)), Some(4));
        assert_eq!(table.next_active(Some(4)), None);
    }
}
