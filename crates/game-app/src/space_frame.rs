//! The in-space frame's simulation half (`UpdateSpaceState`,
//! `space_state.cpp:1017-1100`; work package P5.4b) in single player: the
//! client's object updates (`ClientObjDb_UpdateAll` and the game objects'
//! `update` slots), the joint animations (`AnimDB::Update`), the physics
//! step (`PhySys::Update`) and the beams (`ClientObjDb_MoveBeams`), over
//! the shared world (record 0002: in single player only the client steps
//! physics; game-logic's `steps_physics` answers false).
//!
//! In single player a game object's `update` runs its object's
//! `CObject::update(dt, UPDATE_DEFAULT)` only for the local player's own
//! objects (ships, guided missiles, mines, countermeasures, loot:
//! `FClientShipGameobj::update`, `GameobjOwnerUpdate`), every beam's always
//! (`FClientBeamGameobj::update`), and nothing for solars and asteroids
//! (theirs runs in multiplayer only). The game objects' own parts (actions,
//! effects, the jump, the bay countdown) and the collision pass
//! (`ClientObjDb_ProcessCollisions`, P5.6) are not here yet.
//!
//! The render half (`UpdateMouseFlightDirection` after the scene begins,
//! [`crate::player_behavior::PlayerFlight::update_mouse_flight`], the
//! camera's update and draw) is the frame's caller's.

use game_sim::object::World;
use game_sim::object::update_flags;

use crate::objects::{ClientObjectKind, ClientObjects};

/// `CObject::UPDATE_DEFAULT`.
pub const UPDATE_DEFAULT: u32 = 0;

/// The buckets `ClientObjDb_UpdateAll` walks before the solars.
const FIRST: [ClientObjectKind; 5] = [
    ClientObjectKind::Ship,
    ClientObjectKind::Beam,
    ClientObjectKind::Guided,
    ClientObjectKind::Mine,
    ClientObjectKind::CounterMeasure,
];

/// The buckets it walks after them (10, then 7, 8, 9).
const LAST: [ClientObjectKind; 4] = [
    ClientObjectKind::Object,
    ClientObjectKind::Loot,
    ClientObjectKind::Asteroid,
    ClientObjectKind::DynamicAsteroid,
];

/// The `CObject::update` flags a game object's `update` runs its object
/// with in single player; `None` for no update.
fn single_player_flags(kind: ClientObjectKind, owner: Option<u32>, player: u32) -> Option<u32> {
    // An object without a CSimple reads as the player's (`NarrowObject(.., 3)`
    // null gives `GetPlayerConnectionId()`).
    let own = owner.is_none_or(|o| o == player);
    match kind {
        ClientObjectKind::Beam => Some(UPDATE_DEFAULT),
        ClientObjectKind::Ship
        | ClientObjectKind::Guided
        | ClientObjectKind::Mine
        | ClientObjectKind::CounterMeasure
        | ClientObjectKind::Loot => own.then_some(UPDATE_DEFAULT),
        ClientObjectKind::Solar
        | ClientObjectKind::Asteroid
        | ClientObjectKind::DynamicAsteroid
        | ClientObjectKind::Object => None,
    }
}

/// `ClientObjDb_UpdateAll(dt)` in single player: buckets 1, 3, 4, 5 and 6,
/// the solars, then 10, 7, 8 and 9, each in link order. `player` is the
/// connection id (`GetPlayerConnectionId`).
pub fn update_all(world: &mut World, objects: &ClientObjects, player: u32, dt: f32) {
    let order = FIRST
        .into_iter()
        .chain([ClientObjectKind::Solar])
        .chain(LAST);
    for kind in order {
        let handles: Vec<_> = objects.bucket(kind).filter_map(|(_, o)| o.handle).collect();
        for h in handles {
            let owner = world
                .get(h)
                .filter(|o| o.kind != game_sim::object::ObjectKind::DynamicAsteroid)
                .map(|o| o.simple.owner_player);
            if let Some(flags) = single_player_flags(kind, owner, player) {
                world.update_object(h, dt, flags);
            }
        }
    }
}

/// The simulation half of `UpdateSpaceState` in single player: the object
/// updates, `AnimDB::Update`, `PhySys::Update` of the active system and
/// `ClientObjDb_MoveBeams`, in that order.
pub fn simulate(world: &mut World, objects: &ClientObjects, system: u32, player: u32, dt: f32) {
    update_all(world, objects, player, dt);
    world.update_anim(dt);
    world.step_physics(system, dt);
    world.move_beams(system, dt);
}

/// [`update_flags::UPDATE_DUMB`], the multiplayer update of foreign
/// objects (P5.10).
pub const UPDATE_DUMB: u32 = update_flags::UPDATE_DUMB;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn single_player_updates_only_the_players_own_and_beams() {
        let k = ClientObjectKind::Ship;
        assert_eq!(single_player_flags(k, Some(1), 1), Some(0));
        assert_eq!(single_player_flags(k, Some(0), 1), None);
        assert_eq!(single_player_flags(k, None, 1), Some(0));
        assert_eq!(
            single_player_flags(ClientObjectKind::Beam, Some(0), 1),
            Some(0)
        );
        assert_eq!(
            single_player_flags(ClientObjectKind::Solar, Some(1), 1),
            None
        );
        assert_eq!(
            single_player_flags(ClientObjectKind::Loot, Some(1), 1),
            Some(0)
        );
    }
}
