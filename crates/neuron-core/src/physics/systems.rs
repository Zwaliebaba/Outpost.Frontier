//! The `PhySys` facade's system-level functions
//! (`CppSource/include/common_physys.h:446-451`, `:598-604`): one
//! [`PhysicsWorld`] per star system id, started and stopped by reference
//! count, and `PhysicalSystem::Update`'s two stepping modes
//! (`common_physicalsystem.cpp:346-394`).

use std::collections::BTreeMap;

use super::PhySysConstants;
use super::ivp::IvpRandom;
use super::ivp::math::IvpTime;
use super::world::{ControllerHost, PhysicsWorld};

/// How `PhysicalSystem::Update` steps (record 0002 point 6).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StepMode {
    /// One step per frame, clamped to 0.0999 s.
    SinglePlayer,
    /// A time accumulator stepped in 0.0999 s chunks, with the moving
    /// objects synced after each chunk.
    Multiplayer,
}

/// Every system's physics, the shared multiplayer accumulator and IVP's
/// process-wide random seed.
#[derive(Clone, Debug)]
pub struct PhysicsSystems {
    constants: PhySysConstants,
    systems: BTreeMap<u32, (PhysicsWorld, u32)>,
    accumulator: f64,
    random: IvpRandom,
}

impl PhysicsSystems {
    /// No systems yet; `constants` as read from `[PhySysConsts]`.
    #[must_use]
    pub fn new(constants: PhySysConstants) -> Self {
        Self {
            constants,
            systems: BTreeMap::new(),
            accumulator: 0.0,
            random: IvpRandom::default(),
        }
    }

    /// The constants.
    #[must_use]
    pub fn constants(&self) -> &PhySysConstants {
        &self.constants
    }

    /// `PhySys::Startup`: the system's reference count goes up; the world
    /// is built on the first.
    pub fn startup(&mut self, system_id: u32) {
        let constants = &self.constants;
        let entry = self
            .systems
            .entry(system_id)
            .or_insert_with(|| (PhysicsWorld::new(system_id, constants), 0));
        entry.1 += 1;
    }

    /// `PhySys::Shutdown`: the world goes with the last reference.
    pub fn shutdown(&mut self, system_id: u32) {
        let remove = match self.systems.get_mut(&system_id) {
            Some(entry) => {
                entry.1 = entry.1.saturating_sub(1);
                entry.1 == 0
            }
            None => false,
        };
        if remove {
            self.systems.remove(&system_id);
        }
    }

    /// A system's world.
    #[must_use]
    pub fn world(&self, system_id: u32) -> Option<&PhysicsWorld> {
        self.systems.get(&system_id).map(|(w, _)| w)
    }

    /// A system's world, mutably.
    pub fn world_mut(&mut self, system_id: u32) -> Option<&mut PhysicsWorld> {
        self.systems.get_mut(&system_id).map(|(w, _)| w)
    }

    /// `PhySys::AnyActive`: whether any system is started.
    #[must_use]
    pub fn any_active(&self) -> bool {
        !self.systems.is_empty()
    }

    /// `PhySys::MovingObjectCount`.
    #[must_use]
    pub fn moving_object_count(&self, system_id: u32) -> usize {
        self.world(system_id)
            .map_or(0, PhysicsWorld::moving_object_count)
    }

    /// `PhySys::GetCurrentTime`. The reference reads through the map's head
    /// node for an unknown system; the port answers zero.
    #[must_use]
    pub fn current_time(&self, system_id: u32) -> IvpTime {
        self.world(system_id)
            .map_or(IvpTime::default(), PhysicsWorld::current_time)
    }

    /// IVP's process-wide random seed (`IVP_FastRandomFloat`), which the
    /// movement check draws from.
    #[must_use]
    pub fn ivp_seed(&self) -> u32 {
        self.random.seed
    }

    /// Sets IVP's random seed: the reference's process has drawn from it
    /// before a harness starts comparing.
    pub fn set_ivp_seed(&mut self, seed: u32) {
        self.random.seed = seed;
    }

    /// The multiplayer accumulator (one for all systems, as in the
    /// reference).
    #[must_use]
    pub fn accumulator(&self) -> f64 {
        self.accumulator
    }

    /// `PhySys::Update` / `PhysicalSystem::Update`: clear the system's
    /// collisions, step it in `mode`, and call `sync` where the reference
    /// copies the moving bodies into their objects (after every
    /// multiplayer chunk and at the end).
    pub fn update(
        &mut self,
        system_id: u32,
        dt: f32,
        mode: StepMode,
        host: &mut dyn ControllerHost,
        sync: &mut dyn FnMut(&PhysicsWorld),
    ) {
        let Some((world, _)) = self.systems.get_mut(&system_id) else {
            return;
        };
        world.clear_collisions();
        match mode {
            StepMode::SinglePlayer => {
                let step = if f64::from(dt) > 0.0999 {
                    0.0999f64 as f32
                } else {
                    dt
                };
                world.simulate_dtime(step, host, &mut self.random);
            }
            StepMode::Multiplayer => {
                self.accumulator += f64::from(dt);
                if self.accumulator > 0.0051 {
                    while self.accumulator > 0.0999 {
                        world.simulate_dtime(0.0999f64 as f32, host, &mut self.random);
                        self.accumulator -= 0.0999;
                        sync(world);
                    }
                    if self.accumulator > f64::from(1.0e-5f32) {
                        world.simulate_dtime(self.accumulator as f32, host, &mut self.random);
                    }
                    self.accumulator = 0.0;
                }
            }
        }
        sync(world);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::physics::world::NoControllers;

    #[test]
    fn startup_is_reference_counted() {
        let mut systems = PhysicsSystems::new(PhySysConstants::default());
        assert!(!systems.any_active());
        systems.startup(7);
        systems.startup(7);
        systems.shutdown(7);
        assert!(systems.world(7).is_some());
        systems.shutdown(7);
        assert!(systems.world(7).is_none());
        systems.shutdown(7);
        assert_eq!(systems.moving_object_count(7), 0);
        assert_eq!(systems.current_time(7), IvpTime::default());
    }

    #[test]
    fn single_player_steps_once_per_frame() {
        let mut systems = PhysicsSystems::new(PhySysConstants::default());
        systems.startup(1);
        let mut syncs = 0;
        systems.update(
            1,
            0.5,
            StepMode::SinglePlayer,
            &mut NoControllers,
            &mut |_| syncs += 1,
        );
        assert_eq!(syncs, 1);
        let world = systems.world(1).unwrap();
        assert_eq!(world.clock().delta_time, 0.0999f64 as f32);
        systems.update(
            1,
            0.02,
            StepMode::SinglePlayer,
            &mut NoControllers,
            &mut |_| {},
        );
        assert_eq!(systems.world(1).unwrap().clock().delta_time, 0.02);
    }

    #[test]
    fn multiplayer_accumulates_in_chunks() {
        let mut systems = PhysicsSystems::new(PhySysConstants::default());
        systems.startup(1);
        let mut syncs = 0;
        systems.update(
            1,
            0.004,
            StepMode::Multiplayer,
            &mut NoControllers,
            &mut |_| syncs += 1,
        );
        assert_eq!(syncs, 1, "below 0.0051 nothing steps");
        assert_eq!(systems.accumulator(), f64::from(0.004f32));
        assert_eq!(systems.current_time(1), IvpTime::default());
        syncs = 0;
        systems.update(
            1,
            0.25,
            StepMode::Multiplayer,
            &mut NoControllers,
            &mut |_| syncs += 1,
        );
        // 0.254: two chunks of 0.0999 (each synced), the remainder, the final sync.
        assert_eq!(syncs, 3);
        assert_eq!(systems.accumulator(), 0.0);
        let world = systems.world(1).unwrap();
        let rest = (f64::from(0.004f32) + f64::from(0.25f32) - 0.0999 - 0.0999) as f32;
        assert_eq!(world.clock().delta_time, rest);
    }
}
