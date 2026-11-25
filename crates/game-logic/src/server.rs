//! The server context of decision record 0006 (decision O40): one `Cx`
//! that borrows the world and owns-by-borrow everything else, passed by
//! `&mut` to every function that can dispatch a controller message.
//!
//! `Logic` holds the server's own state: the controller bus, the client
//! sink, the player database and Content's state ([`crate::content`],
//! decision record 0008). The controllers are the 23 kinds of Content's
//! factory (`CreateInstance`, `Content/Content.cpp:94-135`): the bus's
//! create runs their constructors ([`crate::content::instantiate`]), its
//! teardown their destructors ([`crate::content::destroy_instance`]), and a
//! delivery goes to Content's handler of the kind
//! ([`crate::content::deliver`]), then to the optional test hook.

use std::cell::RefCell;
use std::rc::Rc;

use game_sim::object::World;
use game_sim::protocol::ToClient;
use neuron_core::hash::eq_ignore_case;
use neuron_core::ini::IniReader;
use neuron_core::net::transport::{ConnectionId, Transport};
use neuron_server::controller::{
    self, BusHost, ControllerBus, ControllerId, read_initial_controllers,
};

use crate::events::ControllerMessage;

/// A Content controller kind: one name of `CreateInstance`'s factory.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ControllerKind {
    /// `GameSupport`.
    GameSupport,
    /// `SpaceSupport`.
    SpaceSupport,
    /// `BaseSupport`.
    BaseSupport,
    /// `DestroyMission`.
    DestroyMission,
    /// `SpacePop`.
    SpacePop,
    /// `BasePop`.
    BasePop,
    /// `StoryLauncher`.
    StoryLauncher,
    /// `Story`.
    Story,
    /// `Mission_01a`.
    Mission01a,
    /// `Mission_01b`.
    Mission01b,
    /// `Mission_02` to `Mission_13`.
    Mission(u8),
    /// `GenericMission`.
    GenericMission,
}

/// The factory's names in its `stricmp` order (first match wins).
const FACTORY: [(&str, ControllerKind); 23] = [
    ("GameSupport", ControllerKind::GameSupport),
    ("SpaceSupport", ControllerKind::SpaceSupport),
    ("BaseSupport", ControllerKind::BaseSupport),
    ("DestroyMission", ControllerKind::DestroyMission),
    ("SpacePop", ControllerKind::SpacePop),
    ("BasePop", ControllerKind::BasePop),
    ("StoryLauncher", ControllerKind::StoryLauncher),
    ("Story", ControllerKind::Story),
    ("Mission_01a", ControllerKind::Mission01a),
    ("Mission_01b", ControllerKind::Mission01b),
    ("Mission_02", ControllerKind::Mission(2)),
    ("Mission_03", ControllerKind::Mission(3)),
    ("Mission_04", ControllerKind::Mission(4)),
    ("Mission_05", ControllerKind::Mission(5)),
    ("Mission_06", ControllerKind::Mission(6)),
    ("Mission_07", ControllerKind::Mission(7)),
    ("Mission_08", ControllerKind::Mission(8)),
    ("Mission_09", ControllerKind::Mission(9)),
    ("Mission_10", ControllerKind::Mission(10)),
    ("Mission_11", ControllerKind::Mission(11)),
    ("Mission_12", ControllerKind::Mission(12)),
    ("Mission_13", ControllerKind::Mission(13)),
    ("GenericMission", ControllerKind::GenericMission),
];

impl ControllerKind {
    /// `CreateInstance`'s name lookup: case-insensitive, first match; `None`
    /// for a name the factory does not know (a null interface).
    #[must_use]
    pub fn from_instance_name(name: &[u8]) -> Option<Self> {
        FACTORY
            .iter()
            .find(|(n, _)| eq_ignore_case(n.as_bytes(), name))
            .map(|&(_, kind)| kind)
    }

    /// The factory name.
    #[must_use]
    pub fn name(self) -> &'static str {
        FACTORY
            .iter()
            .find(|(_, k)| *k == self)
            .map_or("?", |(n, _)| n)
    }
}

/// What `pub::Controller::Create` hands a new controller
/// (`pub::Controller::CreateParms` and its derived forms; record 0008
/// point 2). A kind whose constructor reads parameters of another form
/// reads zeros (D770).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CreateParams {
    /// No parameters (the `[Initial * DLLs]` rows; a null `Parms`).
    #[default]
    None,
    /// `FStoryCreateParms` (StoryLauncher's create of `Story`,
    /// `Content/StoryLauncher.cpp:64-77`): the player.
    Story {
        /// `PlayerId`.
        player: u32,
    },
    /// `FStoryMissionCreateParms` (Story's create of a chapter's mission,
    /// `Content/Story.cpp:162-169`): the player and the Story controller
    /// whose `IStoryMissionEvents` the mission calls.
    StoryMission {
        /// `PlayerId`.
        player: u32,
        /// The Story controller (the C++'s `IStoryMissionEvents*`).
        story: ControllerId,
    },
    /// `FStoryOnLoadCreateParms` for `GenericMission` (Story's restore,
    /// `Content/Story.cpp:360-380`): the story mission's parameters and the
    /// mission file, within the C++'s buffer (D773).
    GenericMission {
        /// `PlayerId`.
        player: u32,
        /// The Story controller.
        story: ControllerId,
        /// `GenericMissionPath`.
        path: crate::content::mission::MissionPath,
    },
    /// `FRandomMissionData` for `DestroyMission`: a key into Content's
    /// reference-counted mission data (P3.7).
    RandomMission(crate::content::random_mission::RmDataKey),
}

/// Where the server's calls to the client go (`IClient`, through the
/// transport; record 0002): queued in order, never answered synchronously.
pub trait ClientSink {
    /// One message to one client.
    fn send(&mut self, client: u32, message: ToClient);

    /// `IClient::GetLinkSaturation`: how loaded the link to a client is,
    /// from the transport's backlog (decision T17); the observers stop
    /// sending at 0.9 ([`crate::replication`]). rpclocal's answer, and the
    /// loopback's, is 0; [`TransportSink`] reports the QUIC host's
    /// governor.
    fn link_saturation(&self, client: u32) -> f64 {
        let _ = client;
        0.0
    }
}

/// Where the server's chat messages go (`IChatClient::SendCommonChatMsg`):
/// to a chat id, from a chat id, with the encoded message.
pub trait ChatSink {
    /// One chat message.
    fn send(&mut self, to: u32, from: u32, data: Vec<u8>);
}

/// A chat sink that keeps what it is sent.
#[derive(Debug, Default)]
pub struct RecordingChat {
    /// The messages in order: to, from, data.
    pub sent: Vec<(u32, u32, Vec<u8>)>,
}

impl ChatSink for RecordingChat {
    fn send(&mut self, to: u32, from: u32, data: Vec<u8>) {
        self.sent.push((to, from, data));
    }
}

/// A sink that keeps what it is sent, for tests and the harness.
#[derive(Debug, Default)]
pub struct RecordingSink {
    /// The messages in order, with their client.
    pub sent: Vec<(u32, ToClient)>,
}

impl ClientSink for RecordingSink {
    fn send(&mut self, client: u32, message: ToClient) {
        self.sent.push((client, message));
    }
}

/// The client sink over a transport (P3.8): a message for client `n` goes
/// to the transport's connection `n`, as the QUIC host numbers its
/// connections by client id (`CRemoteClientProxy::OnConnect`), and the
/// saturation is the transport's ([`Transport::link_saturation`]). The
/// server loop polls the same transport, hence the shared cell.
///
/// A message for a client without an open connection is dropped, as
/// `CClientImpl::PushToClient` drops one for a slot without a proxy.
#[derive(Debug)]
pub struct TransportSink<T> {
    transport: Rc<RefCell<T>>,
}

impl<T> TransportSink<T> {
    /// The sink over `transport`.
    #[must_use]
    pub const fn new(transport: Rc<RefCell<T>>) -> Self {
        Self { transport }
    }
}

impl<T> ClientSink for TransportSink<T>
where
    T: Transport,
    T::Outgoing: From<ToClient>,
{
    fn send(&mut self, client: u32, message: ToClient) {
        // Dropped for a client without an open connection.
        let _ = self
            .transport
            .borrow_mut()
            .send(ConnectionId::new(client), message.into());
    }

    fn link_saturation(&self, client: u32) -> f64 {
        self.transport
            .borrow()
            .link_saturation(ConnectionId::new(client))
    }
}

/// The chat sink over a transport (P3.9): `CClientImpl::SendCommonChatMsg`
/// (`RemoteClient/ClientImpl.cpp:360-381`), a `CHATMSG` with the target
/// and sender ids, pushed to the client the target id's low word names.
/// A message for a client without an open connection (the console's own
/// echo, to id 0) is dropped.
#[derive(Debug)]
pub struct TransportChat<T> {
    transport: Rc<RefCell<T>>,
}

impl<T> TransportChat<T> {
    /// The sink over `transport`.
    #[must_use]
    pub const fn new(transport: Rc<RefCell<T>>) -> Self {
        Self { transport }
    }
}

impl<T> ChatSink for TransportChat<T>
where
    T: Transport,
    T::Outgoing: From<ToClient>,
{
    fn send(&mut self, to: u32, from: u32, data: Vec<u8>) {
        let message = ToClient::Common(
            game_sim::protocol::common::ChatMsg {
                data,
                target_id: to,
                field_04: from,
            }
            .into(),
        );
        let _ = self
            .transport
            .borrow_mut()
            .send(ConnectionId::new(to & 0xFFFF), message.into());
    }
}

/// The server's counters (`GServerStats`, `ServerStats`,
/// `include/common_server_types.h:498`); the player count is counted when
/// asked (`IServerImpl::GetServerStats`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ServerStats {
    /// Logins answered 3 (`LoginSuccessCount`).
    pub logins: u32,
    /// `PlayerDB::logout` calls (`LogoutCount`).
    pub logouts: u32,
    /// Logins answered otherwise (`LoginFailCount`).
    pub failed_logins: u32,
    /// Players' ships destroyed on a multiplayer server (`Field_0x10`).
    pub player_deaths: u32,
    /// Missions aborted (`AbortMissionCount`).
    pub aborted_missions: u32,
}

/// A delivery handler for tests: called with the context, the controller
/// and the message. A function pointer, so the context can lend it out
/// without a borrow.
pub type DeliverHook = fn(&mut Cx<'_>, ControllerId, ControllerKind, &mut ControllerMessage<'_>);

/// The server's own state.
pub struct Logic {
    /// The controller registry.
    pub bus: ControllerBus<ControllerKind>,
    /// The client side of the transport.
    pub client: Box<dyn ClientSink>,
    /// A test seam: every delivery goes here after Content's handler
    /// (record 0008 point 2).
    pub deliver_hook: Option<DeliverHook>,
    /// The session slots (`Players`, `PlayerDB::init(MaxPlayers, ...)`).
    pub players: neuron_server::session::SessionTable<crate::player::PlayerData>,
    /// `IsMPServer()`: a dedicated multiplayer server.
    pub multiplayer_server: bool,
    /// The server's game time (`GServerTimeBase`, `g_ElapsedTimeAccum`).
    pub clock: neuron_server::tick::ServerClock,
    /// The players' vibe ids (`g_VibeBitmap`).
    pub vibe_ids: VibeIds,
    /// What the server reports (`FDUMP`), in order.
    pub reports: game_sim::data::Reports,
    /// The accounts a multiplayer server knows (`m_AccountNameMap`,
    /// `m_CharacterIDMap`); empty in single player.
    pub accounts: neuron_server::registry::AccountRegistry,
    /// New accounts may be made (`PlayerDB::init`'s `bAllowNewPlayers`).
    pub allow_new_players: bool,
    /// The chat side of the transport (`g_pChatClientInterface`).
    pub chat: Box<dyn ChatSink>,
    /// The player groups.
    pub groups: crate::group::Groups,
    /// A host's view of the chat traffic.
    pub chat_hooks: crate::chat::ChatHooks,
    /// The prefix of a cheat announcement (`GCheatMessage`).
    pub cheat_message: Vec<u16>,
    /// The base database, made from the world's bases on first use
    /// (`Server_SubsystemInit`).
    pub bases: Option<crate::bases::BaseDb>,
    /// The star systems (`SysDB::SysMap`, record 0007 point 3).
    pub server_world: crate::world::ServerWorld,
    /// `pub::GF::GetBasePosition`'s one-entry cache: the base and its
    /// position.
    pub gf_base_position: Option<(u32, neuron_core::math::Vector)>,
    /// The server's state of the world's objects (record 0007 point 1).
    pub objects: crate::objects::ServerObjects,
    /// The fuse database (`ServerFuseDB`), read on first use
    /// ([`crate::fuses::fuse_db`]).
    pub fuses: Option<crate::fuses::FuseDb>,
    /// The client's performance slider (`GRtcSliderValue`, 0 until set),
    /// which a single-player server reads.
    pub rtc_slider: f32,
    /// The ships `pub::SpaceObj::Create` made and their controllers.
    pub space_obj: crate::api::space_obj::SpaceObjState,
    /// The server's phantoms and the pump's latch (`pub::Phantom`).
    pub phantoms: crate::api::phantom::Phantoms,
    /// The loaded systems' solar BSPs (`StarSystem::Spatial`).
    pub solar_trees: crate::api::system::SolarTrees,
    /// The save storage: the accounts folder, the local clock and the last
    /// file written ([`crate::saves`]).
    pub saves: crate::saves::Saves,
    /// Content.dll's state (record 0008 point 1): its tables, its globals
    /// and each controller's own state.
    pub content: crate::content::Content,
    /// The message of the day (`PlayerDB::m_MOTD`).
    pub motd: Vec<u16>,
    /// The server's counters (`GServerStats`).
    pub stats: ServerStats,
}

/// The 4096-bit map of player vibe ids (`g_VibeBitmap`, `g_VibeBitmapHint`,
/// `Server/server_pub.cpp:5802`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VibeIds {
    bits: [u32; 128],
    hint: u32,
}

impl Default for VibeIds {
    fn default() -> Self {
        Self {
            bits: [0; 128],
            hint: 0,
        }
    }
}

impl VibeIds {
    /// The number of ids.
    pub const COUNT: u32 = 0x1000;

    fn taken(&self, index: u32) -> bool {
        self.bits[(index >> 5) as usize] & (1 << (index & 0x1F)) != 0
    }

    /// `Server_VibeID_Alloc`: the first free id after the last one given,
    /// wrapping at 4096; -1 when the walk comes back to that id. The walk
    /// passes 0 without the comparison, so with every id taken and the last
    /// one given 0 the C++ never returns; the port returns -1 there too
    /// (D604).
    pub fn alloc(&mut self) -> i32 {
        let mut index = self.hint + 1;
        if index >= Self::COUNT {
            index = 0;
        }
        for _ in 0..=Self::COUNT {
            if !self.taken(index) {
                self.bits[(index >> 5) as usize] |= 1 << (index & 0x1F);
                self.hint = index;
                return index as i32;
            }
            index += 1;
            if index >= Self::COUNT {
                index = 0;
            } else if index == self.hint {
                return -1;
            }
        }
        -1
    }

    /// `Server_VibeID_Free`; the C++ throws `out_of_range` for an id above
    /// 4095, which the port ignores.
    pub fn free(&mut self, id: i32) {
        let Ok(index) = u32::try_from(id) else {
            return;
        };
        if index < Self::COUNT {
            self.bits[(index >> 5) as usize] &= !(1 << (index & 0x1F));
        }
    }
}

impl Logic {
    /// A server with no controllers and `max_players` empty slots.
    #[must_use]
    pub fn new(client: Box<dyn ClientSink>, max_players: usize, multiplayer_server: bool) -> Self {
        Self {
            bus: ControllerBus::new(),
            client,
            deliver_hook: None,
            players: neuron_server::session::SessionTable::new(max_players),
            multiplayer_server,
            clock: neuron_server::tick::ServerClock::default(),
            vibe_ids: VibeIds::default(),
            reports: game_sim::data::Reports::default(),
            accounts: neuron_server::registry::AccountRegistry::new(),
            allow_new_players: true,
            chat: Box::new(RecordingChat::default()),
            groups: crate::group::Groups::default(),
            chat_hooks: crate::chat::ChatHooks::default(),
            cheat_message: crate::cheats::DEFAULT_MESSAGE.encode_utf16().collect(),
            bases: None,
            server_world: crate::world::ServerWorld::default(),
            gf_base_position: None,
            rtc_slider: 0.0,
            objects: crate::objects::ServerObjects::default(),
            fuses: None,
            space_obj: crate::api::space_obj::SpaceObjState::default(),
            phantoms: crate::api::phantom::Phantoms::default(),
            solar_trees: crate::api::system::SolarTrees::default(),
            saves: crate::saves::Saves::default(),
            content: crate::content::Content::new(),
            motd: Vec::new(),
            stats: ServerStats::default(),
        }
    }
}

/// The context every dispatching function takes (record 0006, point 2).
pub struct Cx<'a> {
    /// The world.
    pub world: &'a mut World,
    /// The server's state.
    pub logic: &'a mut Logic,
}

impl BusHost for Cx<'_> {
    type Kind = ControllerKind;
    type Message<'m> = ControllerMessage<'m>;
    type CreateParams = CreateParams;

    fn bus(&mut self) -> &mut ControllerBus<ControllerKind> {
        &mut self.logic.bus
    }

    fn deliver(
        &mut self,
        id: ControllerId,
        kind: ControllerKind,
        message: &mut ControllerMessage<'_>,
    ) {
        self.logic.content.count_delivery();
        crate::content::deliver(self, id, kind, message);
        if let Some(hook) = self.logic.deliver_hook {
            hook(self, id, kind, message);
        }
    }

    fn instantiate(
        &mut self,
        id: ControllerId,
        _dll: &[u8],
        name: &[u8],
        params: &CreateParams,
    ) -> Option<ControllerKind> {
        crate::content::instantiate(self, id, name, params)
    }

    fn destroy_instance(&mut self, id: ControllerId, kind: ControllerKind) {
        crate::content::destroy_instance(self, id, kind);
    }

    fn allocate_id(&mut self) -> u32 {
        self.world.allocate_id()
    }

    fn rand(&mut self) -> i32 {
        self.world.rng().rand()
    }

    fn heartbeat_message(elapsed: f32) -> ControllerMessage<'static> {
        ControllerMessage::Heartbeat { elapsed }
    }

    fn timer_expired_message(param1: u32, param2: u32) -> ControllerMessage<'static> {
        ControllerMessage::TimerExpired { param1, param2 }
    }

    fn controller_destroyed_message(id: ControllerId) -> ControllerMessage<'static> {
        ControllerMessage::ControllerDestroyed { controller: id }
    }
}

impl Cx<'_> {
    /// `Server_LoadInitialDlls`: the `[Initial SP DLLs]` (single player) or
    /// `[Initial MP DLLs]` rows of the server INI, each through
    /// `pub::Controller::Create`. Returns each row's id (0 for a name the
    /// factory does not know, whose id is used up all the same).
    pub fn load_initial_controllers(
        &mut self,
        ini: &mut IniReader,
        single_player: bool,
    ) -> Vec<ControllerId> {
        let rows = read_initial_controllers(ini, single_player);
        rows.rows
            .iter()
            .map(|row| {
                controller::create(
                    self,
                    &row.dll,
                    &row.instance,
                    &CreateParams::None,
                    row.priority,
                )
            })
            .collect()
    }

    /// `Server_ControllerDispatch` (`pub::Controller::_SendMessage`).
    ///
    /// # Errors
    ///
    /// No controller has the targeted id.
    pub fn dispatch(
        &mut self,
        target: ControllerId,
        message: &mut ControllerMessage<'_>,
    ) -> Result<(), controller::BusError> {
        controller::dispatch(self, target, message)
    }
}

#[cfg(test)]
mod transport_sink_tests {
    use std::cell::RefCell;
    use std::rc::Rc;

    use game_sim::protocol::to_client::ConnectResponse;
    use neuron_core::net::transport::{
        ConnectionId, DisconnectReason, LOOPBACK_CONNECTION, LinkStats, Transport, TransportError,
        TransportEvent, loopback,
    };

    use super::{ClientSink, ToClient, TransportSink};

    /// A transport that keeps what it is sent and reports a fixed
    /// saturation for connection 2.
    #[derive(Default)]
    struct Fake {
        sent: Vec<(ConnectionId, ToClient)>,
    }

    impl Transport for Fake {
        type Outgoing = ToClient;
        type Incoming = ();

        fn send(
            &mut self,
            connection: ConnectionId,
            message: ToClient,
        ) -> Result<(), TransportError> {
            if connection.get() == 9 {
                return Err(TransportError::UnknownConnection(connection));
            }
            self.sent.push((connection, message));
            Ok(())
        }

        fn poll(&mut self) -> Option<TransportEvent<()>> {
            None
        }

        fn disconnect(
            &mut self,
            _: ConnectionId,
            _: DisconnectReason,
        ) -> Result<(), TransportError> {
            Ok(())
        }

        fn link_stats(&self, _: ConnectionId) -> Option<LinkStats> {
            None
        }

        fn link_saturation(&self, connection: ConnectionId) -> f64 {
            if connection.get() == 2 { 0.95 } else { 0.0 }
        }
    }

    #[test]
    fn the_sink_addresses_connections_by_client_id() {
        let transport = Rc::new(RefCell::new(Fake::default()));
        let mut sink = TransportSink::new(Rc::clone(&transport));
        let message = ToClient::Server(ConnectResponse { value: 2 }.into());
        sink.send(2, message.clone());
        sink.send(9, message.clone());
        assert_eq!(transport.borrow().sent, [(ConnectionId::new(2), message)]);
        assert_eq!(sink.link_saturation(2), 0.95);
        assert_eq!(sink.link_saturation(1), 0.0);
    }

    #[test]
    fn the_loopback_reports_no_saturation() {
        let (_client, server) = loopback::<game_sim::protocol::ToServer, ToClient>();
        let server = Rc::new(RefCell::new(server));
        let mut sink = TransportSink::new(Rc::clone(&server));
        sink.send(1, ToClient::Server(ConnectResponse { value: 1 }.into()));
        assert_eq!(sink.link_saturation(1), 0.0);
        let stats = server.borrow().link_stats(LOOPBACK_CONNECTION).unwrap();
        assert_eq!(stats.queued_messages, 1);
    }
}
