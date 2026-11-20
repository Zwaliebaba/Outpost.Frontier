//! The walks of the registry: dispatch, create and destroy, the heartbeat
//! walk and timer firing, shutdown.

use neuron_core::diag::DumpFlags;

use super::{BusError, ControllerBus, ControllerId, ControllerNode, Priority};

/// The side that owns the bus and the controllers (decision record 0006,
/// point 4). `game-logic` implements it for its server context; tests
/// implement it with scripted controllers.
pub trait BusHost {
    /// What a controller is; copied out of the node for each delivery.
    type Kind: Copy;
    /// The message type; a lifetime lets a message lend a resource.
    type Message<'m>;
    /// What `pub::Controller::Create` hands the new controller.
    type CreateParams: ?Sized;

    /// The registry.
    fn bus(&mut self) -> &mut ControllerBus<Self::Kind>;

    /// `IDLL::HandleMessage` of controller `id`. The handler may dispatch
    /// again through `self`.
    fn deliver(&mut self, id: ControllerId, kind: Self::Kind, message: &mut Self::Message<'_>);

    /// `CreateInstance(id, name, params)`: makes the controller's state and
    /// returns its kind, or `None` as a null interface does. It runs while the
    /// node is already in a list, so it may address the new controller.
    fn instantiate(
        &mut self,
        id: ControllerId,
        dll: &[u8],
        name: &[u8],
        params: &Self::CreateParams,
    ) -> Option<Self::Kind>;

    /// `DestroyInstance`: drops the controller's state. The node has left
    /// the registry already.
    fn destroy_instance(&mut self, id: ControllerId, kind: Self::Kind);

    /// `CFLIDMaker::AllocateObjectID`, the counter the world's objects share.
    fn allocate_id(&mut self) -> u32;

    /// The C runtime's `rand()` of the server thread.
    fn rand(&mut self) -> i32;

    /// The `Heartbeat` message carrying the elapsed time.
    fn heartbeat_message(elapsed: f32) -> Self::Message<'static>;

    /// The `TimerExpired` message carrying the timer's two words.
    fn timer_expired_message(param1: u32, param2: u32) -> Self::Message<'static>;

    /// The `ControllerDestroyed` message carrying the id.
    fn controller_destroyed_message(id: ControllerId) -> Self::Message<'static>;

    /// `RefreshLoadingProgress`, called by dispatch and create.
    fn refresh_loading_progress(&mut self) {}

    /// An `FDUMP` report.
    fn dump(&mut self, flags: DumpFlags, text: &str) {
        neuron_core::diag::dump(flags, text);
    }
}

/// Delivers to the node at `index` of the active list, if it has a kind.
/// Returns `false` past the end of the list.
fn deliver_active<H: BusHost>(host: &mut H, index: usize, message: &mut H::Message<'_>) -> bool {
    let Some(node) = host.bus().active.get(index) else {
        return false;
    };
    let (id, kind) = (node.id, node.kind);
    // A node whose constructor is still running has no interface; the C++
    // calls through null there. The port skips it.
    if let Some(kind) = kind {
        host.deliver(id, kind, message);
    }
    true
}

/// `Server_ControllerDispatch` (`pub::Controller::_SendMessage`): a broadcast
/// for an id whose low 30 bits are 0, else a targeted send.
///
/// # Errors
///
/// [`BusError::NoController`] for a targeted send to an unknown id.
pub fn dispatch<H: BusHost>(
    host: &mut H,
    target: ControllerId,
    message: &mut H::Message<'_>,
) -> Result<(), BusError> {
    if target.is_broadcast() {
        broadcast(host, message);
        Ok(())
    } else {
        send(host, target, message)
    }
}

/// The broadcast arm: every active controller in order, under the guard;
/// the deferred list drains when the guard falls back to 0.
pub fn broadcast<H: BusHost>(host: &mut H, message: &mut H::Message<'_>) {
    host.refresh_loading_progress();
    let bus = host.bus();
    bus.guard += 1;
    let len = bus.active.len();
    let mut index = 0;
    while deliver_active(host, index, message) {
        // The active list cannot change while the guard is raised.
        debug_assert_eq!(host.bus().active.len(), len);
        index += 1;
    }
    host.bus().end_walk();
}

/// The targeted arm: the controller with `target` in the active list, else
/// in the deferred list. The guard is not raised.
///
/// # Errors
///
/// [`BusError::NoController`] when no controller has the id.
pub fn send<H: BusHost>(
    host: &mut H,
    target: ControllerId,
    message: &mut H::Message<'_>,
) -> Result<(), BusError> {
    host.refresh_loading_progress();
    let node = host.bus().find(target).ok_or(BusError::NoController)?;
    let (id, kind) = (node.id, node.kind);
    if let Some(kind) = kind {
        host.deliver(id, kind, message);
    }
    Ok(())
}

/// `pub::Controller::Create` (`Server_ControllerCreate`). Returns the new
/// id, or [`ControllerId::BROADCAST`] (0) when the allocator gave 0 or the
/// instance could not be made; in the second case the id is used up.
pub fn create<H: BusHost>(
    host: &mut H,
    dll: &[u8],
    name: &[u8],
    params: &H::CreateParams,
    priority: Priority,
) -> ControllerId {
    let id = host.allocate_id();
    if id == 0 {
        return ControllerId::BROADCAST;
    }
    let id = ControllerId(id);
    let node = ControllerNode {
        id,
        priority,
        kind: None,
        heartbeat_period: -1.0,
        heartbeat_accum: 0.0,
    };
    let bus = host.bus();
    if bus.guard != 0 {
        bus.deferred.push(node);
    } else {
        bus.insert_active(node);
    }
    host.refresh_loading_progress();
    match host.instantiate(id, dll, name, params) {
        Some(kind) => {
            if let Some(node) = host.bus().find_mut(id) {
                node.kind = Some(kind);
            }
            id
        }
        None => {
            // `DestroyInstance(nullptr)` does nothing; the node leaves the
            // list it is in.
            let bus = host.bus();
            bus.remove(id);
            bus.destroy_pending.retain(|&p| p != id);
            ControllerId::BROADCAST
        }
    }
}

/// `pub::Controller::Destroy` (`Server_ControllerDestroy`): ignored for an
/// id already queued; otherwise an active controller with the id is queued
/// and `ControllerDestroyed` is broadcast, to it too. Deferred controllers
/// are not searched.
pub fn destroy<H: BusHost>(host: &mut H, id: ControllerId) {
    let bus = host.bus();
    if bus.destroy_pending.contains(&id) {
        return;
    }
    if bus.active.iter().any(|n| n.id == id) {
        bus.destroy_pending.push_back(id);
        let mut message = H::controller_destroyed_message(id);
        broadcast(host, &mut message);
    }
}

/// `pub::Controller::SetHeartbeatInterval` (`Server_ControllerSetHeartbeat`):
/// the period, and an accumulator of `rand()/32768` of a period so that
/// controllers do not all beat on the same frame. No `rand()` is drawn for an
/// unknown id.
///
/// # Errors
///
/// [`BusError::NoController`] when no controller has the id.
pub fn set_heartbeat<H: BusHost>(
    host: &mut H,
    id: ControllerId,
    interval: f32,
) -> Result<(), BusError> {
    if host.bus().find(id).is_none() {
        return Err(BusError::NoController);
    }
    let r = host.rand();
    let node = host.bus().find_mut(id).ok_or(BusError::NoController)?;
    node.heartbeat_period = interval;
    node.heartbeat_accum = (f64::from(r) * (1.0 / 32768.0) * f64::from(interval)) as f32;
    Ok(())
}

/// The heartbeat walk (`Server_TimerTick`), each server frame after the
/// systems: first every queued controller is torn down, then each active
/// controller with a period gets `Heartbeat` when its accumulator reaches
/// the period.
///
/// Teardown takes the queue front to back, so a controller a destructor
/// destroys is torn down in the same pass, as the C++'s walk of the list it
/// appends to does. The node leaves the list before its destructor runs.
pub fn tick<H: BusHost>(host: &mut H, dt: f32) {
    while let Some(id) = host.bus().destroy_pending.pop_front() {
        let bus = host.bus();
        let Some(at) = bus.active.iter().position(|n| n.id == id) else {
            continue;
        };
        let node = bus.active.remove(at);
        if let Some(kind) = node.kind {
            host.destroy_instance(id, kind);
        }
    }

    host.bus().guard += 1;
    let mut index = 0;
    loop {
        let bus = host.bus();
        let Some(node) = bus.active.get_mut(index) else {
            break;
        };
        if node.heartbeat_period >= 0.0 {
            // The compare and the division use the unrounded sum; only the
            // store is rounded (T11).
            let sum = f64::from(dt) + f64::from(node.heartbeat_accum);
            node.heartbeat_accum = sum as f32;
            let period = f64::from(node.heartbeat_period);
            if sum >= period {
                let wrap = ((sum / period).floor() * period) as f32;
                let (id, kind) = (node.id, node.kind);
                if let Some(kind) = kind {
                    let mut message = H::heartbeat_message(wrap);
                    host.deliver(id, kind, &mut message);
                }
                // The accumulator is read again: the handler may have set a
                // new heartbeat.
                let node = &mut host.bus().active[index];
                node.heartbeat_accum = (f64::from(node.heartbeat_accum) - f64::from(wrap)) as f32;
            }
        }
        index += 1;
    }
    host.bus().end_walk();
}

/// The most timers that fire in one frame.
pub const MAX_TIMERS_PER_FRAME: usize = 128;

/// `Server_TimerFiring`, after the heartbeat walk: every timer loses `dt`;
/// up to 128 whose time is below 0 are collected in list order, then each
/// gets `TimerExpired` (a broadcast when its target's low 30 bits are 0)
/// and is removed after its handler returns. Timers set meanwhile wait for
/// the next frame. The result of each send is ignored.
pub fn fire_timers<H: BusHost>(host: &mut H, dt: f32) {
    let mut expired = Vec::new();
    let mut warn = false;
    let bus = host.bus();
    for timer in &mut bus.timers {
        timer.remaining = (f64::from(timer.remaining) - f64::from(dt)) as f32;
        if expired.len() < MAX_TIMERS_PER_FRAME && timer.remaining < 0.0 {
            expired.push(timer.serial);
            if expired.len() == MAX_TIMERS_PER_FRAME {
                warn = true;
            }
        }
    }
    if warn {
        let count = bus.timers.len();
        let mut text = format!(
            "{count:x} timers are trying to fire on the same frame.  Only 64 will fire now.  \
             The next ones will fire on the next frame."
        );
        // `TString<256>`: at most 255 characters.
        text.truncate(0xFF);
        host.dump(
            DumpFlags::WARNING,
            &format!("Server\\pubMisc.cpp(60) : *** WARNING: {text}"),
        );
    }
    for serial in expired {
        let bus = host.bus();
        let Some(timer) = bus.timers.iter().find(|t| t.serial == serial).copied() else {
            continue;
        };
        let mut message = H::timer_expired_message(timer.param1, timer.param2);
        let _ = dispatch(host, timer.target, &mut message);
        let bus = host.bus();
        if let Some(at) = bus.timers.iter().position(|t| t.serial == serial) {
            bus.timers.remove(at);
        }
    }
}

/// `Server_ControllerCleanup`, at shutdown: the active list sorted by id
/// (stable), then each controller destroyed from the last to the first, the
/// list's head read again after each, then one heartbeat walk with 0 that
/// tears them down. The deferred list is left alone.
pub fn shutdown<H: BusHost>(host: &mut H) {
    let bus = host.bus();
    bus.active.sort_by_key(|n| n.id);
    if bus.active.is_empty() {
        tick(host, 0.0);
        return;
    }
    let mut current: Option<ControllerId> = None;
    loop {
        let bus = host.bus();
        let target = match current {
            None => bus.active[bus.active.len() - 1].id,
            Some(c) => {
                let at = bus
                    .active
                    .iter()
                    .position(|n| n.id == c)
                    .expect("nodes stay listed until the teardown");
                bus.active[at - 1].id
            }
        };
        destroy(host, target);
        current = Some(target);
        if host.bus().active.first().map(|n| n.id) == Some(target) {
            break;
        }
    }
    tick(host, 0.0);
}
