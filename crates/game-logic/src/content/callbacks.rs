//! Content's callback objects as handles (record 0008 point 4, decision
//! O47; D710, D722; work package P3.6c).
//!
//! The C++ hands `pub::AI` its objects by pointer: an NPC ship
//! (`FContentNpcShip`) as both the directive callback (`+0x118`) and the
//! content callback (`+0x174`), a solar NPC (`FSolarNpc`) as a content
//! callback (`+0x160`). The port gives each such object one
//! [`CallbackHandle`]: one allocator hands them out from 1, increasing,
//! never reused, never 0 (null) and never `u32::MAX`
//! (`LAUNCH_ACTION_CALLBACK`, the launch action's, D722); it is separate
//! from `GContentNextUniqueId`, whose values are observable.
//!
//! game-sim keeps the handles in the behaviour managers and calls back
//! after the behaviour tick: a directive result
//! (`ServerHooks::directive_complete`, [`directive_complete`]), a report
//! (`ServerRequest::Content`, [`content_event`]) and, before the tick, the
//! flee answer (`ServerHooks::content_answer`, [`flee_answer`]). A handle
//! whose object is gone answers nothing (D850).

use std::collections::BTreeMap;

use game_sim::ai::content::{CombatEvent, FleeAnswer};
use game_sim::object::ObjHandle;

use crate::events::NpcShipKey;
use crate::server::Cx;

/// A handle of one of Content's callback objects (not 0, not `u32::MAX`).
pub type CallbackHandle = u32;

/// The object a handle names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallbackOwner {
    /// An NPC ship (directive and content callback).
    NpcShip(NpcShipKey),
    /// A solar NPC of a system record, by the record and the solar's
    /// object id (content callback).
    Solar {
        /// The system record.
        system: u32,
        /// The solar's object id.
        object: u32,
    },
}

/// The allocator and the live handles.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CallbackHandles {
    /// The next handle to give out.
    next: u32,
    /// The live handles.
    owners: BTreeMap<CallbackHandle, CallbackOwner>,
}

impl Default for CallbackHandles {
    fn default() -> Self {
        Self {
            next: 1,
            owners: BTreeMap::new(),
        }
    }
}

impl CallbackHandles {
    /// A new handle for `owner`: the next value from 1, never one given
    /// before, never `u32::MAX`.
    pub fn allocate(&mut self, owner: CallbackOwner) -> CallbackHandle {
        let handle = self.next;
        debug_assert!(
            handle != 0 && handle != u32::MAX,
            "Content's callback handles are used up"
        );
        self.next = self.next.saturating_add(1);
        self.owners.insert(handle, owner);
        handle
    }

    /// The object is gone: its handle names nothing from now on (it is not
    /// given out again).
    pub fn release(&mut self, handle: CallbackHandle) {
        self.owners.remove(&handle);
    }

    /// The object `handle` names.
    #[must_use]
    pub fn owner(&self, handle: CallbackHandle) -> Option<CallbackOwner> {
        self.owners.get(&handle).copied()
    }

    /// The next handle the allocator gives (for tests).
    #[must_use]
    pub fn peek_next(&self) -> u32 {
        self.next
    }
}

/// `get_creation_type()` of the content callback `handle`, which
/// `set_content_callback` asks: an NPC ship maps its creation kind, a solar
/// NPC answers the base's 3.
#[must_use]
pub fn creation_type(cx: &Cx<'_>, handle: CallbackHandle) -> u32 {
    match cx.logic.content.state.callbacks.owner(handle) {
        Some(CallbackOwner::NpcShip(key)) => cx
            .logic
            .content
            .state
            .npc_ships
            .get(&key)
            .map_or(3, super::npc_ships::NpcShip::creation_type),
        Some(CallbackOwner::Solar { .. }) | None => 3,
    }
}

/// `NotifyDirectiveCallbacks`' call of the directive callback `handle`
/// (`OnDirectiveComplete(Word11c, Word118)`), after the behaviour tick of
/// `ship` that reported the result (D194). A solar NPC is no directive
/// callback; a handle whose object is gone does nothing (D850).
pub fn directive_complete(
    cx: &mut Cx<'_>,
    _ship: ObjHandle,
    handle: CallbackHandle,
    result: (i32, i32),
) {
    if let Some(CallbackOwner::NpcShip(key)) = cx.logic.content.state.callbacks.owner(handle) {
        super::npc_ships::on_directive_complete(cx, key, result);
    }
}

/// A report of `ship`'s manager to the content callback `handle`, in the
/// order the tick made it among the AI's requests of the server. A handle
/// whose object is gone does nothing (D850).
pub fn content_event(
    cx: &mut Cx<'_>,
    _ship: ObjHandle,
    handle: CallbackHandle,
    event: CombatEvent,
) {
    match cx.logic.content.state.callbacks.owner(handle) {
        Some(CallbackOwner::NpcShip(key)) => super::npc_ships::content_event(cx, key, event),
        Some(CallbackOwner::Solar { system, object }) => {
            super::solar_npcs::content_event(cx, system, object, event);
        }
        None => {}
    }
}

/// The content callback `handle`'s answer to `request_flee_destination`,
/// asked before the behaviour tick of the ship (record 0008 point 4): read
/// only; the request's effects come after the tick
/// ([`CombatEvent::FleeRequested`]). A solar NPC answers the base's false.
#[must_use]
pub fn flee_answer(cx: &Cx<'_>, handle: CallbackHandle) -> Option<FleeAnswer> {
    match cx.logic.content.state.callbacks.owner(handle) {
        Some(CallbackOwner::NpcShip(key)) => super::npc_ships::flee_answer(cx, key),
        Some(CallbackOwner::Solar { .. }) | None => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn handles_start_at_one_and_are_never_reused() {
        let mut h = CallbackHandles::default();
        let a = h.allocate(CallbackOwner::NpcShip(NpcShipKey(1)));
        let b = h.allocate(CallbackOwner::NpcShip(NpcShipKey(2)));
        assert_eq!((a, b), (1, 2));
        h.release(a);
        assert_eq!(h.owner(a), None);
        let c = h.allocate(CallbackOwner::Solar {
            system: 3,
            object: 4,
        });
        assert_eq!(c, 3);
        assert_eq!(h.owner(b), Some(CallbackOwner::NpcShip(NpcShipKey(2))));
    }
}
