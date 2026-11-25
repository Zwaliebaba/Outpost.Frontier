//! The `[ObjList]` directive objects a Content ship runs
//! (`FObjListDirective`, `Content/ObjListDirective.h`) and the ship base's
//! directive queue (`FContentShipBase::Directives`, `CurrentDirective`,
//! `ActiveDirective`, `Content/BehaviorCmds.cpp:228-397`; work package
//! P3.6c).
//!
//! **Sharing.** The C++ counts references to each directive object: the
//! queue holds one on each entry, the active slot one, a guide's own list
//! one, and `FollowDirectivesOf` gives a follower the leader's very objects.
//! Running a directive (`Execute`) writes into the object: the default
//! priority, the targets it resolves by name, and `FollowDirectivesOf`'s
//! dock port, so every holder sees the write. How the mission's `[ObjList]`
//! map shares its directives with the ships it gives them to is the open
//! question of P3.5c. This module keeps P3.6c's directives (those the
//! guides and the flee answer make) behind an opaque [`DirectiveRef`] in an
//! arena of counted objects ([`DirectiveArena`]), so the callers do not
//! depend on the representation: the reference count is `FContentRefObject`'s
//! and a write through one holder is seen by all, as in the C++.

use std::collections::BTreeMap;

use super::mission::obj_list::{DirectiveKind, ObjListDirective};

/// A directive object in Content's arena.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct DirectiveRef(u32);

/// One counted directive object.
#[derive(Clone, Debug, PartialEq)]
struct Entry {
    refs: u32,
    directive: ObjListDirective,
}

/// The arena (`ContentState::directives`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DirectiveArena {
    next: u32,
    entries: BTreeMap<DirectiveRef, Entry>,
}

impl DirectiveArena {
    /// `CreateObjListDirective(Type)` into a holder: the object with one
    /// reference (the creator's local handle).
    pub fn create(&mut self, kind: DirectiveKind) -> DirectiveRef {
        self.next += 1;
        let key = DirectiveRef(self.next);
        self.entries.insert(
            key,
            Entry {
                refs: 1,
                directive: ObjListDirective {
                    name: Vec::new(),
                    kind,
                },
            },
        );
        key
    }

    /// `AddRef` (slot 0).
    pub fn add_ref(&mut self, key: DirectiveRef) {
        if let Some(e) = self.entries.get_mut(&key) {
            e.refs += 1;
        }
    }

    /// `Release` (slot 1): the last one deletes the object.
    pub fn release(&mut self, key: DirectiveRef) {
        let Some(e) = self.entries.get_mut(&key) else {
            return;
        };
        e.refs = e.refs.saturating_sub(1);
        if e.refs == 0 {
            self.entries.remove(&key);
        }
    }

    /// The directive.
    #[must_use]
    pub fn get(&self, key: DirectiveRef) -> Option<&ObjListDirective> {
        self.entries.get(&key).map(|e| &e.directive)
    }

    /// The directive, to write (every holder sees the write).
    pub fn get_mut(&mut self, key: DirectiveRef) -> Option<&mut ObjListDirective> {
        self.entries.get_mut(&key).map(|e| &mut e.directive)
    }

    /// `GetType` (slot 5), -1 for an object that is gone.
    #[must_use]
    pub fn type_code(&self, key: DirectiveRef) -> i32 {
        self.get(key).map_or(-1, |d| d.kind.type_code())
    }

    /// The reference count (for tests).
    #[must_use]
    pub fn refs(&self, key: DirectiveRef) -> u32 {
        self.entries.get(&key).map_or(0, |e| e.refs)
    }

    /// The number of live objects (for tests).
    #[must_use]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// No live object.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

/// A list of directives that holds one reference on each
/// (`FMissionDirectiveList`): a guide's list, a parameter block's.
pub type DirectiveList = Vec<DirectiveRef>;

/// The ship base's queue (`+0x158` to `+0x16c`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ShipDirectives {
    /// `Directives` (`+0x158`), one reference on each.
    pub list: DirectiveList,
    /// `CurrentDirective` (`+0x164`): the index of the running entry,
    /// `None` for `end()`.
    pub current: Option<usize>,
    /// `Priority` (`+0x168`).
    pub priority: i32,
    /// `ActiveDirective` (`+0x16c`), one reference.
    pub active: Option<DirectiveRef>,
}

/// Every element of `list` released, the list emptied
/// (`FMissionDirectiveList::Clear`).
pub fn clear_list(arena: &mut DirectiveArena, list: &mut DirectiveList) {
    for &d in list.iter() {
        arena.release(d);
    }
    list.clear();
}

/// A copy of `list`, each element `AddRef`'ed (`CopyDirectives`, the list
/// assignment).
#[must_use]
pub fn copy_list(arena: &mut DirectiveArena, list: &[DirectiveRef]) -> DirectiveList {
    for &d in list {
        arena.add_ref(d);
    }
    list.to_vec()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::content::mission::obj_list::Delay;

    #[test]
    fn the_last_release_deletes() {
        let mut arena = DirectiveArena::default();
        let d = arena.create(DirectiveKind::Delay(Delay::default()));
        let mut list = copy_list(&mut arena, &[d]);
        assert_eq!(arena.refs(d), 2);
        arena.release(d);
        assert_eq!(arena.refs(d), 1);
        clear_list(&mut arena, &mut list);
        assert!(arena.is_empty());
    }
}
