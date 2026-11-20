//! The server frame (`IServerImpl::ElapseTime`, `Server/IServerImpl.cpp:295`)
//! and the server clock.
//!
//! One frame runs, in order: the clock gains the wall-clock time since the
//! last frame (not `dt`); the systems, the phantom pump and the dying ships
//! (`Server_PhysicsUpdate`, P3.3); the subsystems (`BaseDB::update`); the
//! controller heartbeat walk with its teardown; the timers.

use crate::controller::{self, BusHost};

/// The server clock: the base a loaded game or world file sets plus the
/// wall-clock time the frames have added (`g_ElapsedTimeAccum` and
/// `GServerTimeBase`, two doubles).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ServerClock {
    /// `GServerTimeBase`.
    pub base: f64,
    /// `g_ElapsedTimeAccum`: never reset.
    pub elapsed: f64,
}

impl ServerClock {
    /// The game time (`base + elapsed`).
    #[must_use]
    pub fn now(&self) -> f64 {
        self.base + self.elapsed
    }

    /// One frame's wall-clock delta (`Timing::Delta::read_dt`), negative
    /// values taken as 0.
    pub fn advance(&mut self, real_dt: f64) {
        self.elapsed += real_dt.max(0.0);
    }
}

/// The game side of a frame.
pub trait FrameHost: BusHost {
    /// The server clock.
    fn clock(&mut self) -> &mut ServerClock;
    /// `Server_PhysicsUpdate(dt)`: the systems, phantoms and dying ships.
    fn physics_update(&mut self, dt: f32);
    /// `Server_SubsystemUpdate(dt)`: `BaseDB::update`.
    fn subsystem_update(&mut self, dt: f32);
}

/// `IServerImpl::ElapseTime(dt)` with the frame's wall-clock delta.
pub fn elapse_time<H: FrameHost>(host: &mut H, dt: f32, real_dt: f64) {
    host.clock().advance(real_dt);
    host.physics_update(dt);
    host.subsystem_update(dt);
    controller::tick(host, dt);
    controller::fire_timers(host, dt);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::controller::{ControllerBus, ControllerId, Priority};

    #[derive(Default)]
    struct Host {
        bus: ControllerBus<()>,
        clock: ServerClock,
        log: Vec<String>,
        next: u32,
    }

    impl BusHost for Host {
        type Kind = ();
        type Message<'m> = &'static str;
        type CreateParams = ();
        fn bus(&mut self) -> &mut ControllerBus<()> {
            &mut self.bus
        }
        fn deliver(&mut self, _id: ControllerId, _kind: (), message: &mut &'static str) {
            self.log.push((*message).to_owned());
        }
        fn instantiate(&mut self, _: ControllerId, _: &[u8], _: &[u8], _: &()) -> Option<()> {
            Some(())
        }
        fn destroy_instance(&mut self, _: ControllerId, _: ()) {
            self.log.push("destroyed".into());
        }
        fn allocate_id(&mut self) -> u32 {
            self.next += 1;
            self.next
        }
        fn rand(&mut self) -> i32 {
            0
        }
        fn heartbeat_message(_: f32) -> &'static str {
            "heartbeat"
        }
        fn timer_expired_message(_: u32, _: u32) -> &'static str {
            "timer"
        }
        fn controller_destroyed_message(_: ControllerId) -> &'static str {
            "controller destroyed"
        }
    }

    impl FrameHost for Host {
        fn clock(&mut self) -> &mut ServerClock {
            &mut self.clock
        }
        fn physics_update(&mut self, _dt: f32) {
            self.log.push("physics".into());
        }
        fn subsystem_update(&mut self, _dt: f32) {
            self.log.push("subsystems".into());
        }
    }

    #[test]
    fn a_frame_runs_in_the_reference_order() {
        let mut h = Host::default();
        let id = controller::create(&mut h, b"", b"", &(), Priority::Normal);
        controller::set_heartbeat(&mut h, id, 0.5).unwrap();
        h.bus.set_timer(id, 1, 2, 0.1).unwrap();
        controller::destroy(&mut h, id);
        h.log.clear();
        elapse_time(&mut h, 1.0, -3.0);
        assert_eq!(h.log, ["physics", "subsystems", "destroyed"]);
        assert_eq!(h.clock.elapsed, 0.0);
        h.clock.base = 10.0;
        elapse_time(&mut h, 1.0, 0.25);
        assert_eq!(h.clock.now(), 10.25);
    }
}
