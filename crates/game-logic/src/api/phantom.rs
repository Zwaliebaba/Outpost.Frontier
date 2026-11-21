//! `pub::Phantom` (`Server/server_pub.cpp:7545-7676`) and the server's
//! phantom records (`ServerPhantom`, `:2180`): a box or ball phantom in a
//! system's physics world that lists the objects of its types that enter
//! and leave it, and the pump that tells the controllers of them
//! (`Server_PumpPhantomEvents`, `:2236`, after the systems' step in
//! `Server_PhysicsUpdate`, [`crate::world::physics_update`]).
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `Create(system, size, pos, orient, types)` | 7561 | [`create_box`] |
//! | `Create(system, radius, center, types)` | 7596 | [`create_sphere`] |
//! | `Destroy` | 7627 | [`destroy`] |
//! | `SetActive` | 7640 | [`set_active`] |
//! | `Attach` | 7652 | [`attach`] |
//! | `Detach` | 7668 | [`detach`] |
//!
//! The handle Content gets is the record's pointer in the C++ and a
//! [`PhantomKey`] here, numbered from 1 in creation order (D674); it is what
//! `PhantomEnter` and `PhantomExit` carry. Every export answers -5 while
//! the pump delivers. The phantom's enter and leave events come from the
//! world's physics step through [`crate::munitions::phantom_event`] (the
//! events no mine claims), so they arrive where the server steps the
//! physics (a multiplayer server).

// The C++'s `!(x > 0.0f)` tests send NaN to the failing side; the port
// spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use game_sim::object::{ObjHandle, class};
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::{Geometry, PhantomDesc, PhantomEvent, PhantomHandle};
use neuron_server::controller::ControllerId;

use super::PubError;
use crate::events::{ControllerMessage, PhantomKey};
use crate::server::Cx;

/// One server phantom (`ServerPhantom`, 0x28 bytes).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServerPhantom {
    /// The handle Content holds.
    pub key: PhantomKey,
    /// +0x04: the ids that entered since the last pump.
    pub entered: Vec<u32>,
    /// +0x10: the ids that left since the last pump.
    pub left: Vec<u32>,
    /// +0x1c: the type bits an object must share to count.
    pub type_mask: u32,
    /// +0x20: the physics phantom; none when the system had no physics
    /// world (D671).
    pub phantom: Option<PhantomHandle>,
    /// +0x24.
    pub system: u32,
}

/// `GPhantoms` and `GbPhantomPumpBusy`: the live phantoms in creation
/// order and the pump's latch.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Phantoms {
    /// The live records.
    pub list: Vec<ServerPhantom>,
    /// The pump is delivering.
    pub pump_busy: bool,
    next_key: u32,
}

impl Phantoms {
    /// `FindPhantom`: the record of a handle.
    fn position(&self, key: PhantomKey) -> Option<usize> {
        self.list.iter().position(|p| p.key == key)
    }
}

fn busy(cx: &Cx<'_>) -> Result<(), PubError> {
    if cx.logic.phantoms.pump_busy {
        Err(PubError::BUSY)
    } else {
        Ok(())
    }
}

/// The record and its physics phantom (`PhantomParms`: the system, a
/// listener, no collision group).
fn make(
    cx: &mut Cx<'_>,
    system: u32,
    geometry: Geometry,
    position: Vector,
    orientation: Matrix,
    type_mask: u32,
) -> PhantomKey {
    let phantom = cx.world.create_free_phantom(
        system,
        PhantomDesc {
            geometry,
            position,
            orientation,
            name: Vec::new(),
            listener: true,
        },
    );
    let phantoms = &mut cx.logic.phantoms;
    phantoms.next_key = phantoms.next_key.wrapping_add(1).max(1);
    let key = PhantomKey(phantoms.next_key);
    phantoms.list.push(ServerPhantom {
        key,
        entered: Vec::new(),
        left: Vec::new(),
        type_mask,
        phantom,
        system,
    });
    key
}

/// `Create` (box): a box of half extents `size` at `position` and
/// `orientation`.
///
/// # Errors
///
/// -5 while the pump delivers; -4 for a size component that is not
/// positive (NaN included) or a system id whose low 30 bits are 0.
pub fn create_box(
    cx: &mut Cx<'_>,
    system: u32,
    size: Vector,
    position: Vector,
    orientation: Matrix,
    type_mask: u32,
) -> Result<PhantomKey, PubError> {
    busy(cx)?;
    if !(size.x > 0.0) || !(size.y > 0.0) || !(size.z > 0.0) || system & 0x3fff_ffff == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let min = Vector {
        x: -size.x,
        y: -size.y,
        z: -size.z,
    };
    let geometry = Geometry::cuboid(min, size);
    Ok(make(cx, system, geometry, position, orientation, type_mask))
}

/// `Create` (sphere): a ball of `radius` at `center`, identity
/// orientation.
///
/// # Errors
///
/// -5 while the pump delivers; -4 for a radius that is not positive (NaN
/// included) or a system id whose low 30 bits are 0.
pub fn create_sphere(
    cx: &mut Cx<'_>,
    system: u32,
    radius: f32,
    center: Vector,
    type_mask: u32,
) -> Result<PhantomKey, PubError> {
    busy(cx)?;
    if !(radius > 0.0) || system & 0x3fff_ffff == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let geometry = Geometry::ball(radius);
    Ok(make(
        cx,
        system,
        geometry,
        center,
        Matrix::IDENTITY,
        type_mask,
    ))
}

/// `Destroy`: the physics phantom goes (no leave events), then the record.
///
/// # Errors
///
/// -5 while the pump delivers; -2 for an unknown handle.
pub fn destroy(cx: &mut Cx<'_>, key: PhantomKey) -> Result<(), PubError> {
    busy(cx)?;
    let at = cx.logic.phantoms.position(key).ok_or(PubError::BAD_ID)?;
    let record = cx.logic.phantoms.list.remove(at);
    if let Some(phantom) = record.phantom {
        cx.world.destroy_free_phantom(record.system, phantom);
    }
    Ok(())
}

/// `SetActive`: the phantom's collision detection on or off.
///
/// # Errors
///
/// -5 while the pump delivers; -2 for an unknown handle.
pub fn set_active(cx: &mut Cx<'_>, key: PhantomKey, active: bool) -> Result<(), PubError> {
    busy(cx)?;
    let at = cx.logic.phantoms.position(key).ok_or(PubError::BAD_ID)?;
    let (system, phantom) = {
        let p = &cx.logic.phantoms.list[at];
        (p.system, p.phantom)
    };
    if let Some(phantom) = phantom {
        cx.world
            .set_free_phantom_collisions(system, phantom, active);
    }
    Ok(())
}

/// `Attach`: the phantom follows the live object's body
/// (`PhySys::AttachPhantom`).
///
/// # Errors
///
/// -5 while the pump delivers; -2 when no live object has the id; -3 for
/// an unknown handle; -1 when the physics refuses: the object has no body
/// in the phantom's system, or the phantom has no physics phantom or is
/// already attached, where the C++ reads through null or merges again
/// (D672).
pub fn attach(cx: &mut Cx<'_>, object: u32, key: PhantomKey) -> Result<(), PubError> {
    busy(cx)?;
    let h = crate::dock::lookup_live(cx, object).ok_or(PubError::BAD_ID)?;
    let at = cx.logic.phantoms.position(key).ok_or(PubError::NOT_FOUND)?;
    let (system, phantom) = {
        let p = &cx.logic.phantoms.list[at];
        (p.system, p.phantom)
    };
    match phantom {
        Some(phantom) if cx.world.attach_free_phantom(system, phantom, h) => Ok(()),
        _ => Err(PubError::FAILED),
    }
}

/// `Detach`: the phantom stays where it is (`PhySys::DetachPhantom`). A
/// phantom that is not attached stays as it is and answers 0, where the
/// C++ reads its null owner (D673).
///
/// # Errors
///
/// -5 while the pump delivers; -2 for an unknown handle; -1 when the
/// physics phantom is gone.
pub fn detach(cx: &mut Cx<'_>, key: PhantomKey) -> Result<(), PubError> {
    busy(cx)?;
    let at = cx.logic.phantoms.position(key).ok_or(PubError::BAD_ID)?;
    let (system, phantom) = {
        let p = &cx.logic.phantoms.list[at];
        (p.system, p.phantom)
    };
    match phantom {
        Some(phantom) if cx.world.detach_free_phantom(system, phantom) => Ok(()),
        Some(_) => Err(PubError::FAILED),
        None => Ok(()),
    }
}

/// `ServerPhantom::added` and `deleted`: an event of one of the server's
/// phantoms. A `CSimple` (`(class & 3) == 3`) whose type bits share a bit
/// with the phantom's mask has its id put on the entered or left list.
/// False when no server phantom of the system has the physics phantom.
pub fn phantom_event(cx: &mut Cx<'_>, system: u32, event: &PhantomEvent) -> bool {
    let Some(at) = cx
        .logic
        .phantoms
        .list
        .iter()
        .position(|p| p.system == system && p.phantom == Some(event.phantom))
    else {
        return false;
    };
    let mask = cx.logic.phantoms.list[at].type_mask;
    let id = cx
        .world
        .get(ObjHandle::from_key(event.user_key))
        .filter(|o| o.is(class::SIMPLE) && o.simple.type_bits & mask != 0)
        .map(|o| o.simple.id);
    if let Some(id) = id {
        let record = &mut cx.logic.phantoms.list[at];
        if event.entered {
            record.entered.push(id);
        } else {
            record.left.push(id);
        }
    }
    true
}

/// `Server_PumpPhantomEvents`: with the latch set, for every phantom in
/// order, each entered id goes to every controller as `PhantomEnter` (the
/// id, the phantom, its system), then the entered list is emptied; the
/// same for the left ids with `PhantomExit`.
pub fn pump_events(cx: &mut Cx<'_>) {
    cx.logic.phantoms.pump_busy = true;
    let mut index = 0;
    while index < cx.logic.phantoms.list.len() {
        let (key, system, entered) = {
            let p = &cx.logic.phantoms.list[index];
            (p.key, p.system, p.entered.clone())
        };
        for object in entered {
            let _ = cx.dispatch(
                ControllerId::BROADCAST,
                &mut ControllerMessage::PhantomEnter {
                    object,
                    phantom: key,
                    system,
                },
            );
        }
        let left = match cx.logic.phantoms.list.get_mut(index) {
            Some(p) => {
                p.entered.clear();
                p.left.clone()
            }
            None => break,
        };
        for object in left {
            let _ = cx.dispatch(
                ControllerId::BROADCAST,
                &mut ControllerMessage::PhantomExit {
                    object,
                    phantom: key,
                    system,
                },
            );
        }
        if let Some(p) = cx.logic.phantoms.list.get_mut(index) {
            p.left.clear();
        }
        index += 1;
    }
    cx.logic.phantoms.pump_busy = false;
}
