//! The free phantoms the server makes for Content (`pub::Phantom`,
//! `Server/server_pub.cpp:7545`) in a system's physics world: a box or a
//! ball with a listener, its collision state, and its attachment to an
//! object's body (`PhySys::CreatePhantom`, `DestroyPhantom`,
//! `SetCollisionStatePhantom`, `AttachPhantom`, `DetachPhantom`). Which
//! phantoms exist and what their events mean is the server's
//! (`game-logic::api::phantom`); the events reach it through
//! [`super::ServerHooks::phantom_event`].

use neuron_core::physics::{PhantomDesc, PhantomHandle};

use crate::object::ObjHandle;

use super::World;

impl World {
    /// `PhySys::CreatePhantom` in `system`'s physics world; `None` when the
    /// system has no physics world (the C++ builds into the system map's
    /// head node there).
    pub fn create_free_phantom(&mut self, system: u32, desc: PhantomDesc) -> Option<PhantomHandle> {
        self.physics
            .world_mut(system)
            .map(|world| world.create_phantom(desc))
    }

    /// `PhySys::DestroyPhantom`: no leave events.
    pub fn destroy_free_phantom(&mut self, system: u32, phantom: PhantomHandle) {
        if let Some(world) = self.physics.world_mut(system) {
            world.destroy_phantom(phantom);
        }
    }

    /// `PhySys::SetCollisionStatePhantom`.
    pub fn set_free_phantom_collisions(&mut self, system: u32, phantom: PhantomHandle, on: bool) {
        if let Some(world) = self.physics.world_mut(system) {
            world.set_phantom_collision_state(phantom, on);
        }
    }

    /// `PhySys::AttachPhantom(object, phantom)`: the phantom joins the
    /// object's body at its current pose, with the distance from the
    /// object's world position. False when the object has no body in
    /// `system` or the phantom is already attached or gone.
    pub fn attach_free_phantom(
        &mut self,
        system: u32,
        phantom: PhantomHandle,
        object: ObjHandle,
    ) -> bool {
        let Some(position) = self.objects.get(object).map(|o| o.engine.position) else {
            return false;
        };
        let Some((body_system, body)) = self.body(object) else {
            return false;
        };
        if body_system != system {
            return false;
        }
        self.physics
            .world_mut(system)
            .is_some_and(|world| world.attach_phantom(phantom, body, position))
    }

    /// `PhySys::DetachPhantom`: the phantom stays where it is. False when
    /// the phantom is gone.
    pub fn detach_free_phantom(&mut self, system: u32, phantom: PhantomHandle) -> bool {
        self.physics
            .world_mut(system)
            .is_some_and(|world| world.detach_phantom(phantom))
    }
}
