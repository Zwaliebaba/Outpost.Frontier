//! The controller registry of `Server.dll` (`Server/IServerImpl.cpp:3016-3360`,
//! `:4220-4400`): the active and deferred lists, the re-entrancy guard, the
//! destroy queue, heartbeats and one-shot timers, ported as decision record
//! 0006 (decision O40) fixes.
//!
//! The registry is data plus walk primitives, generic over the controller
//! kind `K` that the game side defines. Dispatch runs through a
//! [`BusHost`]: the host owns the bus and the controllers' state, and every
//! function that can deliver a message takes the host by `&mut`, so a nested
//! dispatch is an ordinary recursive call and no borrow of the bus survives
//! a delivery.
//!
//! The observable rules the port keeps:
//!
//! - A controller is inserted before the first one whose priority is greater
//!   than or equal to its own, so within one priority the newest comes first.
//! - Broadcasts and the heartbeat walk raise the guard; a controller created
//!   while it is raised waits in the deferred list, where targeted sends and
//!   [`set_heartbeat`] already find it, and joins the active list when the
//!   guard falls back to 0. So the active list never changes during a walk.
//! - [`destroy`] queues the controller and broadcasts `ControllerDestroyed`;
//!   the controller keeps receiving messages until the next [`tick`] tears
//!   it down.
//! - Ids come from the host's object-id allocator (`CFLIDMaker`), shared
//!   with the world, so a failed create still uses one up.

mod dispatch;
mod initial;

pub use dispatch::{
    BusHost, broadcast, create, destroy, dispatch, fire_timers, send, set_heartbeat, shutdown, tick,
};
pub use initial::{InitialController, InitialControllers, read_initial_controllers};

use std::collections::VecDeque;
use std::fmt;

use neuron_core::hash::eq_ignore_case;

/// A controller id (`FLID`). Ids whose low 30 bits are 0 address every
/// controller.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ControllerId(pub u32);

impl ControllerId {
    /// The broadcast address.
    pub const BROADCAST: Self = Self(0);

    /// `(Flid & 0x3fffffff) == 0`: the id addresses every controller.
    #[must_use]
    pub const fn is_broadcast(self) -> bool {
        self.0 & 0x3FFF_FFFF == 0
    }
}

/// `pub::Controller::PRIORITY`, the rows of `Server.dll`'s priority table
/// (`Server_LoadInitialDlls`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(u32)]
pub enum Priority {
    /// `HIGHEST`, 0.
    Highest = 0,
    /// `ABOVE_NORMAL`, 1.
    AboveNormal = 1,
    /// `NORMAL`, 2.
    Normal = 2,
    /// `BELOW_NORMAL`, 3.
    BelowNormal = 3,
    /// `LOWEST`, 4.
    Lowest = 4,
}

impl Priority {
    /// The table's rows in order.
    pub const ALL: [Self; 5] = [
        Self::Highest,
        Self::AboveNormal,
        Self::Normal,
        Self::BelowNormal,
        Self::Lowest,
    ];

    /// The value the list is ordered by.
    #[must_use]
    pub const fn value(self) -> u32 {
        self as u32
    }

    /// The row's name.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Highest => "HIGHEST",
            Self::AboveNormal => "ABOVE_NORMAL",
            Self::Normal => "NORMAL",
            Self::BelowNormal => "BELOW_NORMAL",
            Self::Lowest => "LOWEST",
        }
    }

    /// The row whose name equals `word` ignoring case (`_stricmp`), first
    /// match.
    #[must_use]
    pub fn from_name(word: &[u8]) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|p| eq_ignore_case(p.name().as_bytes(), word))
    }
}

/// The failures the registry reports with the C++'s codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BusError {
    /// No controller has the id (-2).
    NoController,
    /// `pub::SetTimer` with a negative timeout (-4).
    NegativeTimeout,
}

impl BusError {
    /// The C++ return value.
    #[must_use]
    pub const fn code(self) -> i32 {
        match self {
            Self::NoController => -2,
            Self::NegativeTimeout => -4,
        }
    }
}

impl fmt::Display for BusError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NoController => f.write_str("no controller has that id"),
            Self::NegativeTimeout => f.write_str("negative timer timeout"),
        }
    }
}

impl std::error::Error for BusError {}

/// One controller's node (`FControllerNode`).
#[derive(Clone, Debug, PartialEq)]
pub struct ControllerNode<K> {
    /// Its id.
    pub id: ControllerId,
    /// Its priority.
    pub priority: Priority,
    /// Its kind; `None` while its constructor runs (the C++'s null interface
    /// pointer, which a delivery would call through).
    pub kind: Option<K>,
    /// The heartbeat period in seconds; negative for none (initially -1).
    pub heartbeat_period: f32,
    /// The heartbeat accumulator.
    pub heartbeat_accum: f32,
}

/// One pending timer (`FTimerNode`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Timer {
    /// The controller it fires at.
    pub target: ControllerId,
    /// `TimerExpired::Param1`.
    pub param1: u32,
    /// `TimerExpired::Param2`.
    pub param2: u32,
    /// The time left in seconds.
    pub remaining: f32,
    serial: u64,
}

/// The registry's state.
#[derive(Clone, Debug)]
pub struct ControllerBus<K> {
    active: Vec<ControllerNode<K>>,
    deferred: Vec<ControllerNode<K>>,
    destroy_pending: VecDeque<ControllerId>,
    guard: u32,
    timers: Vec<Timer>,
    next_serial: u64,
}

impl<K> Default for ControllerBus<K> {
    fn default() -> Self {
        Self {
            active: Vec::new(),
            deferred: Vec::new(),
            destroy_pending: VecDeque::new(),
            guard: 0,
            timers: Vec::new(),
            next_serial: 0,
        }
    }
}

impl<K: Copy> ControllerBus<K> {
    /// An empty registry.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The active list in dispatch order.
    #[must_use]
    pub fn active(&self) -> &[ControllerNode<K>] {
        &self.active
    }

    /// The deferred-create list in creation order.
    #[must_use]
    pub fn deferred(&self) -> &[ControllerNode<K>] {
        &self.deferred
    }

    /// The ids queued for teardown, in queue order.
    pub fn destroy_pending(&self) -> impl Iterator<Item = ControllerId> + '_ {
        self.destroy_pending.iter().copied()
    }

    /// The re-entrancy guard's count.
    #[must_use]
    pub fn guard(&self) -> u32 {
        self.guard
    }

    /// The pending timers in list order.
    #[must_use]
    pub fn timers(&self) -> &[Timer] {
        &self.timers
    }

    /// `Server_ControllerFind`: the node with `id` in the active list, else
    /// in the deferred list.
    #[must_use]
    pub fn find(&self, id: ControllerId) -> Option<&ControllerNode<K>> {
        self.active
            .iter()
            .chain(self.deferred.iter())
            .find(|n| n.id == id)
    }

    fn find_mut(&mut self, id: ControllerId) -> Option<&mut ControllerNode<K>> {
        self.active
            .iter_mut()
            .chain(self.deferred.iter_mut())
            .find(|n| n.id == id)
    }

    /// `Server_ControllerExists`: a live or pending controller has `id`.
    #[must_use]
    pub fn exists(&self, id: ControllerId) -> bool {
        self.find(id).is_some()
    }

    /// `pub::SetTimer` (`Server_ControllerSetTimer`): appends a timer. Only a
    /// negative timeout is refused; the target is not checked.
    ///
    /// # Errors
    ///
    /// [`BusError::NegativeTimeout`] when `timeout < 0`.
    pub fn set_timer(
        &mut self,
        target: ControllerId,
        param1: u32,
        param2: u32,
        timeout: f32,
    ) -> Result<(), BusError> {
        if timeout < 0.0 {
            return Err(BusError::NegativeTimeout);
        }
        let serial = self.next_serial;
        self.next_serial += 1;
        self.timers.push(Timer {
            target,
            param1,
            param2,
            remaining: timeout,
            serial,
        });
        Ok(())
    }

    /// Inserts before the first active node whose priority is greater than
    /// or equal to `node`'s (`Server_ControllerCreate`, the deferred drain).
    fn insert_active(&mut self, node: ControllerNode<K>) {
        let at = self
            .active
            .iter()
            .position(|n| n.priority.value() >= node.priority.value())
            .unwrap_or(self.active.len());
        self.active.insert(at, node);
    }

    /// `Server_ControllerDrainDeferred`: every deferred node, in order, into
    /// the active list.
    fn drain_deferred(&mut self) {
        for node in std::mem::take(&mut self.deferred) {
            self.insert_active(node);
        }
    }

    /// Lowers the guard after a walk and drains at 0.
    fn end_walk(&mut self) {
        self.guard -= 1;
        if self.guard == 0 {
            self.drain_deferred();
        }
    }

    /// Removes the node with `id` from whichever list holds it.
    fn remove(&mut self, id: ControllerId) -> Option<ControllerNode<K>> {
        if let Some(at) = self.active.iter().position(|n| n.id == id) {
            return Some(self.active.remove(at));
        }
        let at = self.deferred.iter().position(|n| n.id == id)?;
        Some(self.deferred.remove(at))
    }
}

#[cfg(test)]
mod tests;
