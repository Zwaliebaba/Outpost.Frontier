//! State machines and their messages (`common_statemachine.cpp`).
//!
//! The C++ `StateMachine` is an abstract class whose third virtual slot
//! handles an event in a state. Here the machine's bookkeeping is
//! [`StateMachine`], and the behaviour is a [`StateHandler`] passed to
//! [`StateMachine::process`]. `StateMachineGroup` becomes
//! [`StateMachineGroup`], which owns its member machines and removes marked
//! ones at the points the C++ does.
//!
//! The message router the C++ reaches through `IMessageRouter` is the
//! [`MessageRouter`] trait; the server owns its implementation.

/// `STATE_MACHINE_EVENT_TYPE`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum EventType {
    /// 0, `SMET_None`.
    None,
    /// 1, `SMET_Tick`: sent by `start`, by `add_state_machine` and on entry
    /// to a new state.
    Tick,
    /// 2, `SMET_Message`: sent on leaving a state.
    Message,
    /// 3, `SMET_UserEvent`: a routed message (`SMMessage`).
    UserEvent,
    /// 4, `SMET_Controller`.
    Controller,
}

impl EventType {
    /// The C++ value.
    #[must_use]
    pub const fn value(self) -> i32 {
        match self {
            Self::None => 0,
            Self::Tick => 1,
            Self::Message => 2,
            Self::UserEvent => 3,
            Self::Controller => 4,
        }
    }

    /// `SMEventData::is_sm_event`: the value is below 4.
    #[must_use]
    pub const fn is_sm_event(self) -> bool {
        self.value() < 4
    }

    /// `SMEventData::is_user_event`: the value is 4 or more. A routed
    /// message ([`EventType::UserEvent`]) is not a user event by this test.
    #[must_use]
    pub const fn is_user_event(self) -> bool {
        self.value() >= 4
    }
}

/// An event offered to a state handler: `SMEventData`, `SMMessage` or
/// `SMControllerEvent`. `M` is the message type, `C` the controller data.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SmEvent<'a, M, C = ()> {
    /// An event with only a type.
    Plain(EventType),
    /// `SMMessage`: a routed message, type [`EventType::UserEvent`].
    Message(&'a M),
    /// `SMControllerEvent`, type [`EventType::Controller`].
    Controller {
        /// `ControllerId`.
        id: i32,
        /// What `DataPtr` points to.
        data: &'a C,
    },
}

impl<M, C> SmEvent<'_, M, C> {
    /// `get_event_type`.
    #[must_use]
    pub const fn event_type(&self) -> EventType {
        match self {
            Self::Plain(event_type) => *event_type,
            Self::Message(_) => EventType::UserEvent,
            Self::Controller { .. } => EventType::Controller,
        }
    }
}

/// The state value the C++ offers when the current state did not take an
/// event: "any state".
pub const ANY_STATE: i32 = -1;

/// A machine's behaviour: the C++ `StateMachine` slot 2.
pub trait StateHandler<M, C = ()> {
    /// Handles `event` in `state` (the current state or [`ANY_STATE`]).
    /// Returns whether the event was taken. The handler may call
    /// [`StateMachine::set_state`] on `machine`.
    ///
    /// The C++ passes the state by reference to the machine's current
    /// state; here it is a copy taken before the call.
    fn handle(&mut self, machine: &mut StateMachine, event: &SmEvent<'_, M, C>, state: i32)
    -> bool;
}

/// `StateMachine`: the current state and a pending change.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StateMachine {
    current: i32,
    pending: i32,
    pending_change: bool,
    processing: bool,
}

impl StateMachine {
    /// A machine in state 0.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            current: 0,
            pending: 0,
            pending_change: false,
            processing: false,
        }
    }

    /// `get_state`.
    #[must_use]
    pub const fn state(&self) -> i32 {
        self.current
    }

    /// Whether [`process`](Self::process) is running (`bProcessing`).
    #[must_use]
    pub const fn is_processing(&self) -> bool {
        self.processing
    }

    /// `reset`: the current and pending states become 0; a pending change
    /// stays armed.
    pub fn reset(&mut self) {
        self.current = 0;
        self.pending = 0;
    }

    /// `set_state`: arms a change to `state` unless it is the current one.
    pub fn set_state(&mut self, state: i32) {
        if state != self.current {
            self.pending = state;
            self.pending_change = true;
        }
    }

    /// `start`: processes a [`EventType::Tick`] event.
    pub fn start<M, C, H: StateHandler<M, C> + ?Sized>(&mut self, handler: &mut H) {
        self.process(handler, &SmEvent::Plain(EventType::Tick));
    }

    /// `process`: offers `event` to the current state, then to
    /// [`ANY_STATE`] if it was not taken, then applies pending changes.
    pub fn process<M, C, H: StateHandler<M, C> + ?Sized>(
        &mut self,
        handler: &mut H,
        event: &SmEvent<'_, M, C>,
    ) {
        self.processing = true;
        let state = self.current;
        if !handler.handle(self, event, state) {
            handler.handle(self, event, ANY_STATE);
        }
        self.handle_state_change(handler);
        self.processing = false;
    }

    /// `handle_state_change`: while a change is pending, offers
    /// [`EventType::Message`] to the old state, commits the change, then
    /// offers [`EventType::Tick`] to the new state. A handler that sets the
    /// state again runs the loop once more.
    pub fn handle_state_change<M, C, H: StateHandler<M, C> + ?Sized>(&mut self, handler: &mut H) {
        while self.pending_change {
            let state = self.current;
            handler.handle(self, &SmEvent::Plain(EventType::Message), state);
            self.current = self.pending;
            self.pending_change = false;
            let state = self.current;
            handler.handle(self, &SmEvent::Plain(EventType::Tick), state);
        }
    }
}

/// `SM_MESSAGE_SCOPE`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MessageScope {
    /// 0, `SMMS_Broadcast`.
    Broadcast,
    /// 1, `SMMS_Self`.
    ToSelf,
    /// 2, `SMMS_Target`.
    Target,
}

/// The message type id of `SMMChangeState` (`SMM_CHANGE_STATE`).
pub const SMM_CHANGE_STATE: u32 = 1;
/// The message type id `SMM_TIMEOUT`.
pub const SMM_TIMEOUT: u32 = 2;

/// `StateMachineMessage::GenerateUniqueMessageType`'s counter. The C++
/// draws [`SMM_CHANGE_STATE`] and [`SMM_TIMEOUT`] from it during static
/// initialisation, so the first id it hands out at run time is 3.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MessageTypeGenerator {
    last: u32,
}

impl Default for MessageTypeGenerator {
    fn default() -> Self {
        Self { last: SMM_TIMEOUT }
    }
}

impl MessageTypeGenerator {
    /// The counter after the two static ids.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The next id (pre-increment, wrapping).
    pub fn generate(&mut self) -> u32 {
        self.last = self.last.wrapping_add(1);
        self.last
    }
}

/// `StateMachineMessage`: a routed message's header. Message classes that
/// derive from it in the C++ embed it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StateMachineMessage {
    /// `MessageType`.
    pub message_type: u32,
    /// `Sender`, a handler id.
    pub sender: u32,
    /// `Receiver`, a handler id.
    pub receiver: u32,
    /// `DeliveryState`: the state the receiver must be in, or -1.
    pub delivery_state: i32,
    /// `DeliveryTime`.
    pub delivery_time: f32,
    /// `bDelivered`.
    pub delivered: bool,
}

impl Default for StateMachineMessage {
    fn default() -> Self {
        Self::new(0)
    }
}

impl StateMachineMessage {
    /// `StateMachineMessage(Type)`: no sender or receiver, delivery state
    /// -1, time 0.
    #[must_use]
    pub const fn new(message_type: u32) -> Self {
        Self {
            message_type,
            sender: 0,
            receiver: 0,
            delivery_state: -1,
            delivery_time: 0.0,
            delivered: false,
        }
    }

    /// `StateMachineMessage(Time, Type, Sender, Receiver, State)`; not
    /// delivered.
    #[must_use]
    pub const fn with_delivery(
        time: f32,
        message_type: u32,
        sender: u32,
        receiver: u32,
        state: i32,
    ) -> Self {
        Self {
            message_type,
            sender,
            receiver,
            delivery_state: state,
            delivery_time: time,
            delivered: false,
        }
    }

    /// `set_delivery_options`: also clears the delivered flag.
    pub fn set_delivery_options(&mut self, time: f32, sender: u32, receiver: u32, state: i32) {
        self.delivery_time = time;
        self.sender = sender;
        self.receiver = receiver;
        self.delivery_state = state;
        self.delivered = false;
    }
}

/// `SMMChangeState`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ChangeStateMessage {
    /// The header, type [`SMM_CHANGE_STATE`].
    pub header: StateMachineMessage,
    /// `NewState`.
    pub new_state: i32,
}

impl ChangeStateMessage {
    /// `SMMChangeState(NewState)`.
    #[must_use]
    pub const fn new(new_state: i32) -> Self {
        Self {
            header: StateMachineMessage::new(SMM_CHANGE_STATE),
            new_state,
        }
    }
}

/// `IMessageRouter`: delivers messages between handlers and hands out
/// handler ids.
pub trait MessageRouter<M> {
    /// Slot 0: routes `message` to `target` after `delay` seconds; `scope`
    /// is a state or -1.
    fn route_message(&mut self, message: M, delay: f32, target: u32, sender: u32, scope: i32);

    /// Slot 1: a new handler id.
    fn register_handler(&mut self) -> u32;

    /// Slot 2: forgets a handler id.
    fn unregister_handler(&mut self, id: u32);
}

/// `MessageHandler`: a registered id and the send helpers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MessageHandler {
    id: u32,
}

impl MessageHandler {
    /// `MessageHandler(Router)`: registers with `router`.
    pub fn register<M, R: MessageRouter<M> + ?Sized>(router: &mut R) -> Self {
        Self {
            id: router.register_handler(),
        }
    }

    /// `~MessageHandler`: unregisters.
    pub fn unregister<M, R: MessageRouter<M> + ?Sized>(self, router: &mut R) {
        router.unregister_handler(self.id);
    }

    /// `get_id`.
    #[must_use]
    pub const fn id(&self) -> u32 {
        self.id
    }

    /// `send_immediate_message`.
    pub fn send_immediate<M, R: MessageRouter<M> + ?Sized>(
        &self,
        router: &mut R,
        message: M,
        target: u32,
    ) {
        router.route_message(message, 0.0, target, self.id, ANY_STATE);
    }

    /// `send_delayed_message`.
    pub fn send_delayed<M, R: MessageRouter<M> + ?Sized>(
        &self,
        router: &mut R,
        message: M,
        delay: f32,
        target: u32,
    ) {
        router.route_message(message, delay, target, self.id, ANY_STATE);
    }

    /// `MessageHandler::send_delayed_message_to_me`.
    pub fn send_delayed_to_me<M, R: MessageRouter<M> + ?Sized>(
        &self,
        router: &mut R,
        message: M,
        delay: f32,
        scope: i32,
    ) {
        router.route_message(message, delay, self.id, self.id, scope);
    }

    /// `StateMachineMessageHandler::send_delayed_message_to_me`: the scope
    /// becomes the machine's current state, -1, or `target_state`.
    pub fn send_delayed_to_me_scoped<M, R: MessageRouter<M> + ?Sized>(
        &self,
        router: &mut R,
        machine: &StateMachine,
        message: M,
        delay: f32,
        scope: MessageScope,
        target_state: i32,
    ) {
        let scope = match scope {
            MessageScope::ToSelf => machine.state(),
            MessageScope::Broadcast => ANY_STATE,
            MessageScope::Target => target_state,
        };
        router.route_message(message, delay, self.id, self.id, scope);
    }
}

/// A member of a [`StateMachineGroup`]: a machine with its handler.
pub trait GroupMember {
    /// The routed message type.
    type Message;
    /// The controller event data.
    type Controller;

    /// The member's handler id, by which
    /// [`StateMachineGroup::remove_by_id`] finds it.
    fn id(&self) -> u32;

    /// Processes `event` (`StateMachine::process` on the member). `removals`
    /// lets the member mark machines for removal; they are removed after
    /// the pass.
    fn process(
        &mut self,
        event: &SmEvent<'_, Self::Message, Self::Controller>,
        removals: &mut Removals,
    );
}

/// The machines marked for removal (`DyingMachines`), by handler id.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Removals {
    ids: Vec<u32>,
}

impl Removals {
    /// Marks the machine with handler id `id`. Marking it twice has no
    /// further effect.
    pub fn mark(&mut self, id: u32) {
        if !self.ids.contains(&id) {
            self.ids.push(id);
        }
    }

    /// Whether nothing is marked.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.ids.is_empty()
    }
}

/// `StateMachineGroup`: a machine of its own plus member machines, which
/// see every event the group processes except routed messages.
#[derive(Debug)]
pub struct StateMachineGroup<T> {
    machine: StateMachine,
    members: Vec<T>,
    removals: Removals,
}

impl<T> Default for StateMachineGroup<T> {
    fn default() -> Self {
        Self {
            machine: StateMachine::new(),
            members: Vec::new(),
            removals: Removals::default(),
        }
    }
}

impl<T> StateMachineGroup<T> {
    /// An empty group.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The group's own machine.
    pub fn machine(&mut self) -> &mut StateMachine {
        &mut self.machine
    }

    /// The members, front to back.
    pub fn members(&self) -> &[T] {
        &self.members
    }

    /// `remove_state_machine`: marks the member with handler id `id`. It is
    /// removed at the next removal point and may still run until then.
    pub fn remove_by_id(&mut self, id: u32) {
        self.removals.mark(id);
    }
}

impl<T: GroupMember> StateMachineGroup<T> {
    /// `add_state_machine`: appends `member` and processes a
    /// [`EventType::Tick`] event through it.
    pub fn add(&mut self, mut member: T) {
        member.process(&SmEvent::Plain(EventType::Tick), &mut self.removals);
        self.members.push(member);
    }

    /// `remove_dying_machines`: drops each marked member, in marking order.
    pub fn remove_dying(&mut self) {
        for id in core::mem::take(&mut self.removals.ids) {
            if let Some(index) = self.members.iter().position(|m| m.id() == id) {
                self.members.remove(index);
            }
        }
    }

    /// `StateMachineGroup::process`: the group's own machine handles the
    /// event (with `handler`), marked members are removed, then, unless the
    /// event is a routed message, every member processes it front to back
    /// and marked members are removed again.
    pub fn process<H>(&mut self, handler: &mut H, event: &SmEvent<'_, T::Message, T::Controller>)
    where
        H: StateHandler<T::Message, T::Controller> + ?Sized,
    {
        self.machine.process(handler, event);
        self.remove_dying();
        if event.event_type() == EventType::UserEvent {
            return;
        }
        for member in &mut self.members {
            member.process(event, &mut self.removals);
        }
        self.remove_dying();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Recorder {
        calls: Vec<(EventType, i32)>,
        take_in: Option<i32>,
        go_to: Option<i32>,
    }

    impl StateHandler<u32> for Recorder {
        fn handle(
            &mut self,
            machine: &mut StateMachine,
            event: &SmEvent<'_, u32>,
            state: i32,
        ) -> bool {
            self.calls.push((event.event_type(), state));
            if event.event_type() == EventType::UserEvent
                && let Some(next) = self.go_to.take()
            {
                machine.set_state(next);
            }
            self.take_in == Some(state)
        }
    }

    #[test]
    fn untaken_events_go_to_any_state() {
        let mut machine = StateMachine::new();
        let mut handler = Recorder::default();
        machine.start(&mut handler);
        assert_eq!(
            handler.calls,
            [(EventType::Tick, 0), (EventType::Tick, ANY_STATE)]
        );
    }

    #[test]
    fn state_change_exits_then_enters() {
        let mut machine = StateMachine::new();
        let mut handler = Recorder {
            take_in: Some(0),
            go_to: Some(5),
            ..Recorder::default()
        };
        machine.process(&mut handler, &SmEvent::Message(&7));
        assert_eq!(
            handler.calls,
            [
                (EventType::UserEvent, 0),
                (EventType::Message, 0),
                (EventType::Tick, 5)
            ]
        );
        assert_eq!(machine.state(), 5);
        assert!(!machine.is_processing());
        machine.set_state(5);
        machine.reset();
        assert_eq!(machine.state(), 0);
    }

    #[test]
    fn event_type_tests() {
        assert!(EventType::UserEvent.is_sm_event());
        assert!(!EventType::UserEvent.is_user_event());
        assert!(EventType::Controller.is_user_event());
        let data = ();
        let event: SmEvent<'_, u32> = SmEvent::Controller { id: 3, data: &data };
        assert_eq!(event.event_type(), EventType::Controller);
    }

    #[test]
    fn message_types_start_after_the_static_ids() {
        let mut generator = MessageTypeGenerator::new();
        assert_eq!(generator.generate(), 3);
        assert_eq!(generator.generate(), 4);
        let change = ChangeStateMessage::new(9);
        assert_eq!(change.header.message_type, SMM_CHANGE_STATE);
        assert_eq!(change.header.delivery_state, -1);
        let mut message = StateMachineMessage::with_delivery(1.5, 7, 1, 2, 3);
        message.delivered = true;
        message.set_delivery_options(2.0, 4, 5, 6);
        assert!(!message.delivered);
        assert_eq!(
            (message.sender, message.receiver, message.delivery_state),
            (4, 5, 6)
        );
    }

    struct Member {
        id: u32,
        seen: Vec<EventType>,
        remove_on_tick: Option<u32>,
    }

    impl GroupMember for Member {
        type Message = u32;
        type Controller = ();

        fn id(&self) -> u32 {
            self.id
        }

        fn process(&mut self, event: &SmEvent<'_, u32>, removals: &mut Removals) {
            self.seen.push(event.event_type());
            if event.event_type() == EventType::Controller
                && let Some(id) = self.remove_on_tick
            {
                removals.mark(id);
            }
        }
    }

    #[test]
    fn group_skips_routed_messages_and_removes_after_the_pass() {
        let mut group = StateMachineGroup::new();
        let member = |id, remove_on_tick| Member {
            id,
            seen: Vec::new(),
            remove_on_tick,
        };
        group.add(member(1, Some(2)));
        group.add(member(2, None));
        let mut handler = Recorder::default();
        group.process(&mut handler, &SmEvent::Message(&0));
        let data = ();
        group.process(&mut handler, &SmEvent::Controller { id: 0, data: &data });
        assert_eq!(group.members().len(), 1);
        assert_eq!(
            group.members()[0].seen,
            [EventType::Tick, EventType::Controller]
        );
        group.remove_by_id(1);
        group.remove_dying();
        assert!(group.members().is_empty());
    }

    struct Router {
        next: u32,
        routed: Vec<(u32, f32, u32, u32, i32)>,
    }

    impl MessageRouter<u32> for Router {
        fn route_message(
            &mut self,
            message: u32,
            delay: f32,
            target: u32,
            sender: u32,
            scope: i32,
        ) {
            self.routed.push((message, delay, target, sender, scope));
        }

        fn register_handler(&mut self) -> u32 {
            self.next += 1;
            self.next
        }

        fn unregister_handler(&mut self, _id: u32) {}
    }

    #[test]
    fn handler_sends_through_the_router() {
        let mut router = Router {
            next: 10,
            routed: Vec::new(),
        };
        let handler = MessageHandler::register(&mut router);
        assert_eq!(handler.id(), 11);
        let mut machine = StateMachine::new();
        machine.set_state(4);
        let mut recorder = Recorder::default();
        machine.handle_state_change(&mut recorder);
        handler.send_immediate(&mut router, 1, 20);
        handler.send_delayed_to_me_scoped(&mut router, &machine, 2, 0.5, MessageScope::ToSelf, 9);
        handler.send_delayed_to_me_scoped(&mut router, &machine, 3, 0.5, MessageScope::Target, 9);
        assert_eq!(
            router.routed,
            [
                (1, 0.0, 20, 11, -1),
                (2, 0.5, 11, 11, 4),
                (3, 0.5, 11, 11, 9)
            ]
        );
        handler.unregister(&mut router);
    }
}
