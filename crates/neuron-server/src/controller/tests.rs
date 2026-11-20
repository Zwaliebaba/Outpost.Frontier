//! The registry's rules on a small host. The C++'s defined behaviour is
//! checked against the oracle by `tests/controller_bus.rs`; these tests also
//! cover the cases where the C++ is undefined and the port defines it
//! (divergences D594 to D596).

use super::*;

#[derive(Clone, Copy, Debug, PartialEq)]
enum Msg {
    Test(u32),
    Heartbeat(f32),
    Timer(u32, u32),
    Destroyed(ControllerId),
}

/// What a controller does when it receives a message.
type Reaction = fn(&mut Host, ControllerId, &Msg);

#[derive(Default)]
struct Host {
    bus: ControllerBus<usize>,
    next_id: u32,
    rand: neuron_core::rng::CrtRand,
    log: Vec<(ControllerId, Msg)>,
    destroyed: Vec<ControllerId>,
    on_create: Option<Reaction>,
    on_message: Option<Reaction>,
    on_destroy: Option<Reaction>,
}

impl BusHost for Host {
    type Kind = usize;
    type Message<'m> = Msg;
    type CreateParams = ();

    fn bus(&mut self) -> &mut ControllerBus<usize> {
        &mut self.bus
    }

    fn deliver(&mut self, id: ControllerId, _kind: usize, message: &mut Msg) {
        self.log.push((id, *message));
        if let Some(react) = self.on_message {
            react(self, id, message);
        }
    }

    fn instantiate(
        &mut self,
        id: ControllerId,
        _dll: &[u8],
        name: &[u8],
        _params: &(),
    ) -> Option<usize> {
        if let Some(react) = self.on_create.take() {
            react(self, id, &Msg::Test(0));
        }
        (name != b"fail").then_some(0)
    }

    fn destroy_instance(&mut self, id: ControllerId, _kind: usize) {
        self.destroyed.push(id);
        if let Some(react) = self.on_destroy.take() {
            react(self, id, &Msg::Test(0));
        }
    }

    fn allocate_id(&mut self) -> u32 {
        self.next_id += 1;
        self.next_id & 0x3FFF_FFFF
    }

    fn rand(&mut self) -> i32 {
        self.rand.rand()
    }

    fn heartbeat_message(elapsed: f32) -> Msg {
        Msg::Heartbeat(elapsed)
    }

    fn timer_expired_message(param1: u32, param2: u32) -> Msg {
        Msg::Timer(param1, param2)
    }

    fn controller_destroyed_message(id: ControllerId) -> Msg {
        Msg::Destroyed(id)
    }
}

fn ids(nodes: &[ControllerNode<usize>]) -> Vec<u32> {
    nodes.iter().map(|n| n.id.0).collect()
}

#[test]
fn equal_priorities_put_the_newest_first() {
    let mut h = Host::default();
    for p in [
        Priority::Normal,
        Priority::Normal,
        Priority::Highest,
        Priority::Lowest,
    ] {
        create(&mut h, b"x", b"a", &(), p);
    }
    assert_eq!(ids(h.bus.active()), [3, 2, 1, 4]);
}

#[test]
fn a_failed_create_uses_up_its_id() {
    let mut h = Host::default();
    assert_eq!(
        create(&mut h, b"x", b"fail", &(), Priority::Normal),
        ControllerId(0)
    );
    assert_eq!(
        create(&mut h, b"x", b"a", &(), Priority::Normal),
        ControllerId(2)
    );
    assert_eq!(ids(h.bus.active()), [2]);
}

#[test]
fn an_allocator_at_zero_creates_nothing() {
    let mut h = Host {
        next_id: 0x3FFF_FFFF,
        ..Host::default()
    };
    assert_eq!(
        create(&mut h, b"x", b"a", &(), Priority::Normal),
        ControllerId(0)
    );
    assert!(h.bus.active().is_empty());
}

/// D594: a broadcast from a constructor that runs outside a walk reaches
/// the active list, the new node included, whose interface is still null in
/// the C++. The port skips that node.
#[test]
fn a_constructor_broadcast_skips_the_node_being_made() {
    let mut h = Host::default();
    create(&mut h, b"x", b"a", &(), Priority::Normal);
    h.on_create = Some(|h, _, _| broadcast(h, &mut Msg::Test(7)));
    create(&mut h, b"x", b"b", &(), Priority::Highest);
    assert_eq!(h.log, [(ControllerId(1), Msg::Test(7))]);
}

#[test]
fn creation_during_a_broadcast_waits_for_the_outermost_walk() {
    let mut h = Host::default();
    create(&mut h, b"x", b"a", &(), Priority::Normal);
    h.on_message = Some(|h, _, m| {
        if *m == Msg::Test(1) {
            let id = create(h, b"x", b"b", &(), Priority::Highest);
            assert_eq!(h.bus.deferred().len(), 1);
            // Addressable while deferred.
            assert!(send(h, id, &mut Msg::Test(2)).is_ok());
            broadcast(h, &mut Msg::Test(3));
            assert_eq!(h.bus.deferred().len(), 1, "the nested walk does not drain");
        }
    });
    broadcast(&mut h, &mut Msg::Test(1));
    assert_eq!(ids(h.bus.active()), [2, 1]);
    assert!(h.bus.deferred().is_empty());
    let got: Vec<_> = h.log.iter().map(|(id, m)| (id.0, *m)).collect();
    assert_eq!(
        got,
        [(1, Msg::Test(1)), (2, Msg::Test(2)), (1, Msg::Test(3))]
    );
}

#[test]
fn a_destroyed_controller_hears_messages_until_the_tick() {
    let mut h = Host::default();
    let a = create(&mut h, b"x", b"a", &(), Priority::Normal);
    destroy(&mut h, a);
    destroy(&mut h, a);
    assert_eq!(h.log, [(a, Msg::Destroyed(a))], "one broadcast");
    assert!(send(&mut h, a, &mut Msg::Test(1)).is_ok());
    tick(&mut h, 0.0);
    assert_eq!(h.destroyed, [a]);
    assert_eq!(
        send(&mut h, a, &mut Msg::Test(2)),
        Err(BusError::NoController)
    );
}

/// D595: a destructor runs after its node has left the list, so a broadcast
/// it sends does not reach it, and a controller it destroys is torn down in
/// the same pass.
#[test]
fn a_destructor_runs_after_its_node_left() {
    let mut h = Host::default();
    let a = create(&mut h, b"x", b"a", &(), Priority::Normal);
    let b = create(&mut h, b"x", b"b", &(), Priority::Normal);
    h.on_destroy = Some(|h, _, _| {
        broadcast(h, &mut Msg::Test(9));
        destroy(h, ControllerId(2));
    });
    destroy(&mut h, a);
    h.log.clear();
    tick(&mut h, 0.0);
    assert_eq!(h.destroyed, [a, b]);
    assert_eq!(
        h.log,
        [(b, Msg::Test(9)), (b, Msg::Destroyed(b))],
        "a is gone before its destructor broadcasts"
    );
}

#[test]
fn the_heartbeat_reads_its_accumulator_again() {
    let mut h = Host::default();
    let a = create(&mut h, b"x", b"a", &(), Priority::Normal);
    h.bus.active[0].heartbeat_period = 1.0;
    h.on_message = Some(|h, id, m| {
        if matches!(m, Msg::Heartbeat(_)) {
            h.bus.find_mut(id).unwrap().heartbeat_accum = 10.0;
        }
    });
    tick(&mut h, 2.5);
    assert_eq!(h.log, [(a, Msg::Heartbeat(2.0))]);
    assert_eq!(h.bus.active()[0].heartbeat_accum, 8.0);
}

/// D596: a zero period divides 0 by 0 on the first beat; the NaN is
/// delivered once and the accumulator stays NaN, so no beat follows. The
/// C++'s NaN carries the x87 sign; the port's is the host's.
#[test]
fn a_zero_period_beats_once_with_nan() {
    let mut h = Host::default();
    create(&mut h, b"x", b"a", &(), Priority::Normal);
    set_heartbeat(&mut h, ControllerId(1), 0.0).unwrap();
    tick(&mut h, 0.0);
    tick(&mut h, 1.0);
    assert_eq!(h.log.len(), 1);
    assert!(matches!(h.log[0].1, Msg::Heartbeat(e) if e.is_nan()));
}

#[test]
fn set_heartbeat_draws_only_for_a_known_id() {
    let mut h = Host::default();
    create(&mut h, b"x", b"a", &(), Priority::Normal);
    assert_eq!(
        set_heartbeat(&mut h, ControllerId(9), 1.0),
        Err(BusError::NoController)
    );
    set_heartbeat(&mut h, ControllerId(1), 2.0).unwrap();
    // The first rand() of seed 1 is 41.
    assert_eq!(
        h.bus.active()[0].heartbeat_accum,
        (41.0 / 32768.0 * 2.0) as f32
    );
}

#[test]
fn timers_fire_at_most_128_a_frame_and_new_ones_wait() {
    let mut h = Host::default();
    let a = create(&mut h, b"x", b"a", &(), Priority::Normal);
    for n in 0..130 {
        h.bus.set_timer(a, n, 0, 0.0).unwrap();
    }
    assert_eq!(
        h.bus.set_timer(a, 0, 0, -1.0),
        Err(BusError::NegativeTimeout)
    );
    h.on_message = Some(|h, id, m| {
        if *m == Msg::Timer(0, 0) {
            h.bus.set_timer(id, 500, 0, 0.0).unwrap();
        }
    });
    fire_timers(&mut h, 0.1);
    assert_eq!(h.log.len(), 128);
    assert_eq!(h.bus.timers().len(), 3);
    h.log.clear();
    fire_timers(&mut h, 0.1);
    let fired: Vec<_> = h.log.iter().map(|(_, m)| *m).collect();
    assert_eq!(
        fired,
        [Msg::Timer(128, 0), Msg::Timer(129, 0), Msg::Timer(500, 0)]
    );
}

#[test]
fn shutdown_destroys_by_descending_id() {
    let mut h = Host::default();
    create(&mut h, b"x", b"a", &(), Priority::Lowest);
    create(&mut h, b"x", b"b", &(), Priority::Highest);
    create(&mut h, b"x", b"c", &(), Priority::Normal);
    shutdown(&mut h);
    assert_eq!(
        h.destroyed,
        [ControllerId(3), ControllerId(2), ControllerId(1)]
    );
    assert!(h.bus.active().is_empty());
}

#[test]
fn priorities_from_names() {
    assert_eq!(
        Priority::from_name(b"below_normal"),
        Some(Priority::BelowNormal)
    );
    assert_eq!(Priority::from_name(b"NORMAL "), None);
    assert!(ControllerId(0x4000_0000).is_broadcast());
    assert!(!ControllerId(1).is_broadcast());
}
