//! Content's message router, `FContentMessageRouter`
//! (`Content/ContentMessageRouter.h:37-63`, `Content/ContentMessageRouter.cpp`;
//! record 0008 point 3; P3.7b).
//!
//! GameSupport's constructor makes it (`new FContentMessageRouter`, which
//! publishes itself in `GMessageRouter`) and its destructor deletes it, so
//! the router's timer id is drawn from Content's counter at the point the
//! C++ draws it.
//!
//! - **Handlers** ([`register`], [`unregister`], [`lookup`]): every
//!   `MessageHandler` registers on construction and takes the next id
//!   (`++LastHandlerId`, never reused); the router keeps them in
//!   registration order. A handler here names its owner by keys
//!   ([`HandlerOwner`]), and a delivery looks the owner up again.
//! - **Routing** ([`route`], `RouteMessage`, `:49-89`): a delay of 0 or
//!   less delivers at once, as a nested call to the first handler with the
//!   receiver's id; any other delay (NaN included) queues the message at
//!   `float(game time + delay)`, before the first queued message with a
//!   later time, and arms a server timer on SpaceSupport's id with
//!   `{TimerId, 0}`.
//! - **Firing** ([`on_timer_expired`], `OnTimerExpired`, `:131-147`):
//!   SpaceSupport's `TimerExpired` hands its timer here; the router's own
//!   timer delivers and drops the head of the queue, whichever message
//!   armed it. The C++ reads an empty list's sentinel there; every queued
//!   message arms exactly one timer, so no path of the shipped game reaches
//!   it, and the port does nothing (D1020).

use neuron_core::util::state_machine::StateMachineMessage;
use neuron_server::controller::ControllerId;

use super::counter::UniqueIds;
use super::random_mission::runtime::{self, message::RmMessage, trace};
use crate::api;
use crate::server::Cx;

/// Who owns a registered handler (record 0008 point 3: a handler names its
/// owner by keys, and delivery looks it up again).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HandlerOwner {
    /// A `DestroyMission` controller's own state-machine group, by its
    /// controller id.
    Mission(ControllerId),
    /// A machine of a `DestroyMission` controller (a runtime machine, one
    /// of its sub-machines or a trigger), by the controller; the machine is
    /// the handler id itself.
    Machine(ControllerId),
    /// An offer component (`FRmOfferComponent`): its `receive_message` has
    /// no body.
    Component,
}

/// `FContentMessageRouter`.
#[derive(Clone, Debug, PartialEq)]
pub struct Router {
    timer_id: u32,
    last_handler_id: u32,
    /// `PendingMessages`: the delayed messages in delivery order.
    pending: Vec<RmMessage>,
    /// `Handlers`: the registered handlers in registration order.
    handlers: Vec<(u32, HandlerOwner)>,
}

impl Router {
    /// The constructor: `TimerId(GContentNextUniqueId++)`,
    /// `LastHandlerId(0)`, both lists empty.
    pub fn new(ids: &mut UniqueIds) -> Self {
        Self {
            timer_id: ids.take(),
            last_handler_id: 0,
            pending: Vec::new(),
            handlers: Vec::new(),
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

    /// The queued messages, in delivery order.
    #[must_use]
    pub fn pending(&self) -> &[RmMessage] {
        &self.pending
    }

    /// The registered handlers' ids, in registration order.
    pub fn handler_ids(&self) -> impl Iterator<Item = u32> + '_ {
        self.handlers.iter().map(|&(id, _)| id)
    }

    /// `LookupHandlerById`: the first handler with the id.
    #[must_use]
    pub fn lookup(&self, id: u32) -> Option<HandlerOwner> {
        self.handlers
            .iter()
            .find(|&&(h, _)| h == id)
            .map(|&(_, owner)| owner)
    }
}

/// `MessageHandler::MessageHandler(GMessageRouter)`: `RegisterHandler`
/// appends the handler and returns `++LastHandlerId`. Without a router
/// (no GameSupport) the C++ calls through a null pointer; no path of the
/// shipped game makes a handler then, and the port answers 0 (D1020).
pub fn register(cx: &mut Cx<'_>, owner: HandlerOwner) -> u32 {
    let Some(router) = cx.logic.content.state.router.as_mut() else {
        return 0;
    };
    router.last_handler_id = router.last_handler_id.wrapping_add(1);
    let id = router.last_handler_id;
    router.handlers.push((id, owner));
    trace::handler(cx, "register", id);
    id
}

/// `~MessageHandler`: `UnregisterHandler` erases the first entry of the
/// handler, if any.
pub fn unregister(cx: &mut Cx<'_>, id: u32) {
    let Some(router) = cx.logic.content.state.router.as_mut() else {
        return;
    };
    if let Some(at) = router.handlers.iter().position(|&(h, _)| h == id) {
        router.handlers.remove(at);
        trace::handler(cx, "unregister", id);
    }
}

/// `LookupHandlerById` of the live router.
#[must_use]
pub fn lookup(cx: &Cx<'_>, id: u32) -> Option<HandlerOwner> {
    cx.logic.content.state.router.as_ref()?.lookup(id)
}

/// `RouteMessage(Msg, Delay, Target, Sender, Scope)` (`:49-89`).
pub fn route(
    cx: &mut Cx<'_>,
    mut message: RmMessage,
    delay: f32,
    target: u32,
    sender: u32,
    scope: i32,
) {
    if cx.logic.content.state.router.is_none() {
        return;
    }
    if delay <= 0.0 {
        message
            .header
            .set_delivery_options(0.0, sender, target, scope);
        trace::route(cx, &message.header);
        deliver(cx, message);
        return;
    }
    // `GetContentGameTime() + Delay`, stored as a float.
    let time = (cx.logic.content.state.game_time + f64::from(delay)) as f32;
    message
        .header
        .set_delivery_options(time, sender, target, scope);
    trace::route(cx, &message.header);
    let Some(router) = cx.logic.content.state.router.as_mut() else {
        return;
    };
    // The first queued message whose time is greater; the end without one.
    let at = router
        .pending
        .iter()
        .position(|m| m.header.delivery_time > time)
        .unwrap_or(router.pending.len());
    router.pending.insert(at, message);
    let timer = router.timer_id;
    let space_support = cx.logic.content.state.space_support;
    let _ = api::controller::set_timer(cx, space_support, timer, 0, delay);
}

/// A delivery: the first handler with the receiver's id gets the message
/// (`bDelivered = 1`, `receive_message`); an unknown receiver drops it.
fn deliver(cx: &mut Cx<'_>, mut message: RmMessage) {
    let owner = lookup(cx, message.header.receiver);
    trace::deliver(cx, &message.header, owner.is_some());
    let Some(owner) = owner else {
        return;
    };
    message.header.delivered = true;
    runtime::receive_message(cx, owner, &message);
}

/// `OnTimerExpired(Timer)` (`:131-147`): only the router's own timer; the
/// head of the queue is delivered and dropped. An empty queue does nothing
/// (D1020).
pub fn on_timer_expired(cx: &mut Cx<'_>, param1: u32) {
    let Some(router) = cx.logic.content.state.router.as_mut() else {
        return;
    };
    if param1 != router.timer_id || router.pending.is_empty() {
        return;
    }
    let message = router.pending.remove(0);
    deliver(cx, message);
}

/// The header a routed message starts with (`StateMachineMessage(Type)`).
#[must_use]
pub const fn header(message_type: u32) -> StateMachineMessage {
    StateMachineMessage::new(message_type)
}
