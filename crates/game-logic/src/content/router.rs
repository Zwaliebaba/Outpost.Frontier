//! Content's message router, `FContentMessageRouter`
//! (`Content/ContentMessageRouter.h:37-63`, `Content/ContentMessageRouter.cpp`;
//! record 0008 point 3).
//!
//! GameSupport's constructor makes it (`new FContentMessageRouter`, which
//! publishes itself in `GMessageRouter`) and its destructor deletes it, so
//! the router's timer id is drawn from Content's counter at the point the
//! C++ draws it. The pending messages, the handlers and the routing come
//! with the random missions (P3.7b); until then the router holds what its
//! constructor sets.

use super::counter::UniqueIds;

/// `FContentMessageRouter`'s state as its constructor leaves it
/// (`Content/ContentMessageRouter.cpp:24-30`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Router {
    timer_id: u32,
    last_handler_id: u32,
}

impl Router {
    /// The constructor: `TimerId(GContentNextUniqueId++)`,
    /// `LastHandlerId(0)`, both lists empty.
    pub fn new(ids: &mut UniqueIds) -> Self {
        Self {
            timer_id: ids.take(),
            last_handler_id: 0,
        }
    }

    /// `TimerId`: the first word of the timers the router sets on
    /// SpaceSupport, and the only one its `OnTimerExpired` handles.
    #[must_use]
    pub const fn timer_id(&self) -> u32 {
        self.timer_id
    }

    /// `LastHandlerId`: the last handler id handed out (0 for none).
    #[must_use]
    pub const fn last_handler_id(&self) -> u32 {
        self.last_handler_id
    }
}
