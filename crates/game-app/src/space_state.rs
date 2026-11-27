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
use crate::space_camera::{GameCamera, SpaceCameraMode};
use crate::space_camera_third::{
    CameraShake, ChaseCx, ChaseWarning, ShakeHits, ShipCameraTable, ThirdPersonCamera,
};

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

    // StarSysEnter(GetPlayerStarSystemIdbaseline()): the system's physics
    // and its solars. Without a world (multiplayer, P5.10) only the
    // physics flag is raised.
    let system = player_system(link);
    match session.as_deref_mut() {
        Some(s) => {
            let _ = crate::star_system::enter_system(s.world(), &mut link.receiver.objects, system);
        }
        None => link.receiver.objects.physics_active = true,
    }

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

/// `StarSysUpdate`'s music: the zone music at the player's ship, the
/// danger scan over the ship's scanner list, and the machine's change left
/// in [`SpaceApp::music_change`].
fn star_sys_music(
    app: &mut SpaceApp,
    world: &World,
    ship: Option<game_sim::object::ObjHandle>,
    dt: f32,
) {
    use game_sim::ai::world::AiWorld;
    use game_sim::equip::{EquipKind, types};

    let system = app.system;
    let music = app.music.get_or_insert_with(|| {
        let tracks = crate::star_system::system_file_path(world, system)
            .and_then(|path| crate::star_system::StarSystemFile::load(world.data_source()?, &path))
            .map(|f| f.music)
            .unwrap_or_default();
        crate::star_system::SystemMusicState::enter(tracks)
    });
    let data = world.data();
    let position = ship.and_then(|h| world.get(h)).map(|o| o.engine.position);
    let zone_music = position
        .filter(|_| music.override_music.space == 0)
        .and_then(|p| {
            data.universe
                .smallest_zone_with_properties_at(system, p, 0x40_0000)
        })
        .and_then(|z| data.universe.zones.get(&z))
        .map(|z| z.music)
        .filter(|&m| m != 0);
    let frame = crate::star_system::MusicFrame {
        dt,
        background_finished: std::mem::take(&mut app.background_music_finished),
        zone_music,
        // The player's game object's jump state comes with its game object.
        player_jumping: false,
        cross_fade_delay: data.constants.general.music_cross_fade_delay,
    };
    // ScannerSeesHostile: a scanned object whose attitude to the player is
    // at most -0.6 (GetAttitudeClass -1).
    let hostile = || {
        let Some(h) = ship.filter(|&h| {
            world
                .get(h)
                .is_some_and(|o| o.kind == game_sim::object::ObjectKind::Ship)
        }) else {
            return false;
        };
        let Some(scanner) = world
            .get(h)
            .and_then(|o| o.as_equipped())
            .and_then(|e| e.equipment.first(types::SCANNER))
        else {
            return false;
        };
        let EquipKind::Scanner(s) = &scanner.kind else {
            return false;
        };
        s.scans
            .iter()
            .any(|&t| world.attitude(t, h).is_some_and(|a| a <= -0.6))
    };
    app.music_change = music.update(&frame, hostile);
}

/// `GetPlayerStarSystemIdbaseline`: the selected character's system, 0
/// when it is in a base (`ApplyCharacterSelectEquipFields`).
fn player_system<T>(link: &ClientLink<T>) -> u32 {
    link.receiver.login.selected.as_ref().map_or(0, |r| {
        if r.base_id & 0x3fff_ffff != 0 {
            0
        } else {
            r.system_id
        }
    })
}

fn no_answer<T>(link: &mut ClientLink<T>) -> SpaceEntry {
    if link.receiver.single_player {
        SpaceEntry::Quit
    } else {
        link.receiver.login.server_disconnected = true;
        SpaceEntry::Disconnected
    }
}

/// The in-flight controller's state that `UpdateSpaceState` reads and
/// writes (`SpaceApp`): the cameras, the mouse flight, the active system
/// and the leave and player-lost latches.
#[derive(Clone, Debug)]
pub struct SpaceApp {
    /// The eight cameras (+0x00c .. +0xba4).
    pub cameras: crate::space_camera::SpaceCameras,
    /// `player_behavior.cpp`'s module state.
    pub flight: crate::player_behavior::PlayerFlight,
    /// `StarSysGetActiveSystemId`.
    pub system: u32,
    /// `bLeaveRequested`: the next frame leaves space (state 6).
    pub leave_requested: bool,
    /// `bPlayerLostHandled`.
    pub player_lost_handled: bool,
    /// Raised once by [`update`] when the player's ship is gone: the caller
    /// posts `NnExitPostShipLostMessage` and clears it.
    pub post_ship_lost: bool,
    /// `star_sys.cpp`'s music state, made from the system file's `[Music]`
    /// on the first frame with a world (`StarSysEnter` reads the same file).
    pub music: Option<crate::star_system::SystemMusicState>,
    /// Set by the sound layer before [`update`]: the background music
    /// source has finished (`BindClientBackgroundMusic`, `IsFinished`).
    pub background_music_finished: bool,
    /// Left by [`update`] for the sound layer to play
    /// (`SetClientBackgroundMusic`); `None` when the track is unchanged.
    pub music_change: Option<crate::star_system::MusicChange>,
    /// The chase camera's own state (`FThirdPersonCamera` +0x100 ..).
    pub third_person: ThirdPersonCamera,
    /// `G_CameraShakeAmount`, shared by the HUD cameras.
    pub shake: CameraShake,
    /// The ship companions' camera blocks; the owner fills them from the
    /// data configuration ([`ShipCameraTable::from_config`]). Empty, the
    /// chase camera keeps its constructor's values.
    pub ship_cameras: ShipCameraTable,
    /// The client's `rand` for the camera shake (the caller's generator,
    /// as D513).
    pub rand: neuron_core::rng::CrtRand,
    /// The last frame's camera report (the reference's `FDUMP`), for the
    /// caller to log.
    pub camera_warning: Option<ChaseWarning>,
}

impl SpaceApp {
    /// The controller for a surface, in `system`, with no camera installed.
    #[must_use]
    pub fn new(surface: neuron_client::ui::widescreen::Surface, system: u32) -> Self {
        Self {
            cameras: crate::space_camera::SpaceCameras::new(surface),
            flight: crate::player_behavior::PlayerFlight::default(),
            system,
            leave_requested: false,
            player_lost_handled: false,
            post_ship_lost: false,
            music: None,
            background_music_finished: false,
            music_change: None,
            third_person: ThirdPersonCamera::default(),
            shake: CameraShake::default(),
            ship_cameras: ShipCameraTable::default(),
            rand: neuron_core::rng::CrtRand::default(),
            camera_warning: None,
        }
    }

    /// The active camera's update (slot 0x6c): the chase camera's own, or
    /// the base's.
    fn update_camera(&mut self, world: &mut World, dt: f32) {
        let Some(mode) = self.cameras.active() else {
            return;
        };
        if mode == SpaceCameraMode::ThirdPerson {
            let mut cx = ChaseCx {
                world,
                ships: &self.ship_cameras,
                shake: &mut self.shake,
                rand: &mut self.rand,
                hits: ShakeHits::default(),
            };
            let cam = self.cameras.camera_mut(mode);
            self.camera_warning = self.third_person.update(cam, &mut cx, dt).err();
        } else {
            self.cameras.camera_mut(mode).update(dt);
        }
    }

    /// The active camera's attach (slot 0x58): the chase camera's own, or
    /// the base's.
    pub fn attach_camera(
        &mut self,
        world: &mut World,
        object: Option<game_sim::object::ObjHandle>,
    ) {
        let Some(mode) = self.cameras.active() else {
            return;
        };
        if mode == SpaceCameraMode::ThirdPerson {
            let mut cx = ChaseCx {
                world,
                ships: &self.ship_cameras,
                shake: &mut self.shake,
                rand: &mut self.rand,
                hits: ShakeHits::default(),
            };
            let cam = self.cameras.camera_mut(mode);
            self.camera_warning = self.third_person.attach(cam, &mut cx, object).err();
        } else {
            self.cameras.camera_mut(mode).attach(object);
        }
    }
}

/// `UpdateSpaceState(dt)`: the in-space frame; the next client state (0
/// stays, 6 leaves space).
///
/// - No installed camera: 6 at once.
/// - Unless the local server's tick is held (`ShouldSkipLocalServerTick`):
///   the simulation half ([`crate::space_frame::simulate`]; single player).
/// - In the scene bracket: `UpdateMouseFlightDirection` on the player's
///   ship with `flight_frame`.
/// - A requested leave: 6.
///
/// The active camera's update is the chase camera's own solve when it is
/// `third_person`; the other cameras' motion, and every camera's draw,
/// are not here yet (P5.4b), nor the docking and lights passes, the sound
/// mixer, the fade, the reap, the pending system switch and the player's
/// last position.
pub fn update<T, S>(
    app: &mut SpaceApp,
    link: &mut ClientLink<T>,
    session: Option<&mut S>,
    flight_frame: &crate::player_behavior::FlightFrame,
    skip_local_server_tick: bool,
    dt: f32,
) -> i32
where
    S: Session + ?Sized,
{
    if app.cameras.active().is_none() {
        return 6;
    }
    let Some(session) = session else {
        // Multiplayer's own world comes with P5.10.
        return if app.leave_requested { 6 } else { 0 };
    };
    let world = session.world();
    let player = link.receiver.login.connection_id;
    if !skip_local_server_tick {
        crate::space_frame::simulate(world, &link.receiver.objects, app.system, player, dt);
    }
    let ship = link
        .receiver
        .objects
        .find(link.receiver.player.character_id, 1)
        .and_then(|(_, o)| o.handle);
    if !skip_local_server_tick {
        // The active camera's update (slot 0x6c).
        app.update_camera(world, dt);
        // StarSysUpdate's music part (its dust, nebulae, panels and
        // asteroids are the environment's, P5.8).
        star_sys_music(app, world, ship, dt);
    }
    let mut player_ship = crate::player_behavior::PlayerShip {
        world: &mut *world,
        ship,
    };
    app.flight
        .update_mouse_flight(&mut player_ship, flight_frame);
    // A finished camera (slot 0x60, no object watched): without a player
    // ship the ship-lost message once (`NnExitPostShipLostMessage`: the
    // caller posts it on `post_ship_lost`); otherwise back to `player`,
    // attached to the ship.
    if app
        .cameras
        .active_camera()
        .is_some_and(GameCamera::has_no_target)
    {
        match ship {
            None => {
                if !app.player_lost_handled {
                    app.post_ship_lost = true;
                    app.player_lost_handled = true;
                }
            }
            Some(h) => {
                app.cameras.switch_mode(b"player");
                app.attach_camera(world, Some(h));
            }
        }
    }
    if app.leave_requested { 6 } else { 0 }
}
