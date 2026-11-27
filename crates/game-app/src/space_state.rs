//! Entering space (`InitSpaceState`, `space_state.cpp:852-1016`, and
//! `SpawnPlayerShip`, `player_spawn.cpp:376-474`; work package P5.4): the
//! client's dialogue with the server on the way into a system.
//!
//! This part holds the server's side of the entry, in the C++ order: the
//! ship requested (`RequestCreateShip`) and waited for, the system entered,
//! the player's ship registered and launched (`PlayerLaunch`, the request
//! `REQUESTLAUNCH`), the server's pump (`IServer::Update`), and the wait
//! for the launch. The loading screen, the scene, the cockpit, the cameras
//! and the state's events between those steps are the rest of P5.4; none
//! of them talks to the server.

use game_sim::object::World;
use game_sim::protocol::to_server::{RequestCreateShip, RequestLaunch};
use game_sim::protocol::{ClientMessage, ToClient, ToServer};
use neuron_core::net::transport::Transport;

use crate::login::wait_for_server_response;
use crate::objects::{ClientObject, ClientObjectKind};
use crate::receiver::ClientLink;
use crate::session::{Session, tick_modal_server};

/// How the entry ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpaceEntry {
    /// The ship launched: `InitSpaceState` answers true.
    Entered,
    /// No answer in single player: `SetClientQuitRequested(true)`, the
    /// frame loop ends.
    Quit,
    /// No answer from a remote server: `G_bServerDisconnected`.
    Disconnected,
}

/// The server's part of `InitSpaceState` with the player's ship spawned
/// between its waits. Without an answer to the ship request the entry stops
/// there; without the launch it ends after the rest, as the C++ does.
pub fn enter_space<T, S>(
    link: &mut ClientLink<T>,
    mut session: Option<&mut S>,
    now: &mut dyn FnMut() -> f64,
) -> SpaceEntry
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    // IServer slot 64 RequestCreateShip, then ClearPlayerCreateShipResponse
    // and the wait on the answered byte.
    link.send(ToServer::Client(ClientMessage::RequestCreateShip(
        RequestCreateShip {},
    )));
    link.receiver.player.create_ship_answered = false;
    link.receiver.player.create_ship_result = 0;
    wait_for_server_response(
        link,
        session.as_deref_mut(),
        |r| r.player.create_ship_answered,
        now,
    );
    if !link.receiver.player.create_ship_answered {
        return no_answer(link);
    }

    // StarSysEnter: the system's physics is active, so creates are taken.
    link.receiver.objects.physics_active = true;

    spawn_player_ship(link, session.as_deref_mut());

    // IServer slot 11 Update: rpclocal's pump, the client's delivery first.
    tick_modal_server(session.as_deref_mut(), true, |s| {
        link.deliver_all(Some(s.world()));
    });

    wait_for_server_response(link, session, |r| r.player.launch_acknowledged, now);
    if link.receiver.player.launch_acknowledged {
        SpaceEntry::Entered
    } else {
        no_answer(link)
    }
}

/// `SpawnPlayerShip`'s registration and launch request: the player's ship
/// under the id the create answer gave (`G_PlayerCharacterId`), with the
/// selected character's ship archetype, registered in the object database
/// (in single player beside the shared world's ship of that id), the launch
/// acknowledgement cleared and `PlayerLaunch(ship, connection)` sent. The
/// game object's archetype, equipment and costume set-up is the rest of
/// P5.4.
pub fn spawn_player_ship<T, S>(link: &mut ClientLink<T>, session: Option<&mut S>)
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    let id = link.receiver.player.character_id;
    let archetype = link
        .receiver
        .login
        .selected
        .as_ref()
        .map_or(0, |r| r.ship_archetype);
    let world: Option<&World> = session.map(|s| &*s.world());
    let object = ClientObject {
        id,
        kind: ClientObjectKind::Ship,
        archetype,
        handle: world.and_then(|w| w.find(id)),
        remote_physics: false,
    };
    match world {
        Some(world) => link.receiver.objects.register(object, world),
        None => link.receiver.objects.register(object, &()),
    };
    link.receiver.player.launch_acknowledged = false;
    link.send(ToServer::Client(ClientMessage::RequestLaunch(
        RequestLaunch { value: id },
    )));
}

fn no_answer<T>(link: &mut ClientLink<T>) -> SpaceEntry {
    if link.receiver.single_player {
        SpaceEntry::Quit
    } else {
        link.receiver.login.server_disconnected = true;
        SpaceEntry::Disconnected
    }
}
