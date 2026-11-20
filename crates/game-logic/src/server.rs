//! The server context of decision record 0006 (decision O40): one `Cx`
//! that borrows the world and owns-by-borrow everything else, passed by
//! `&mut` to every function that can dispatch a controller message.
//!
//! `Logic` holds the server's own state: the controller bus, the client
//! sink and (from the later P3.2 increments) the player database. The
//! controllers are the 23 kinds of Content's factory (`CreateInstance`,
//! `Content/Content.cpp:84-108`); their handlers arrive with P3.5 to P3.7,
//! and until then a delivery goes to the optional test hook.

use game_sim::object::World;
use game_sim::protocol::ToClient;
use neuron_core::hash::eq_ignore_case;
use neuron_core::ini::IniReader;
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
/// (`pub::Controller::CreateParms` and its derived forms).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CreateParams {
    /// No parameters (the `[Initial * DLLs]` rows).
    #[default]
    None,
    /// `FStoryCreateParms` and the mission forms: the player.
    Player(u32),
}

/// Where the server's calls to the client go (`IClient`, through the
/// transport; record 0002): queued in order, never answered synchronously.
pub trait ClientSink {
    /// One message to one client.
    fn send(&mut self, client: u32, message: ToClient);
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
    /// Deliveries go here until Content's handlers are ported.
    pub deliver_hook: Option<DeliverHook>,
    /// The session slots (`Players`, `PlayerDB::init(MaxPlayers, ...)`).
    pub players: neuron_server::session::SessionTable<crate::player::PlayerData>,
    /// `IsMPServer()`: a dedicated multiplayer server.
    pub multiplayer_server: bool,
    /// The server's game time (`GServerTimeBase`, `g_ElapsedTimeAccum`).
    pub clock: neuron_server::tick::ServerClock,
    /// The players' vibe ids (`g_VibeBitmap`).
    pub vibe_ids: VibeIds,
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
        if let Some(hook) = self.logic.deliver_hook {
            hook(self, id, kind, message);
        }
    }

    fn instantiate(
        &mut self,
        _id: ControllerId,
        _dll: &[u8],
        name: &[u8],
        _params: &CreateParams,
    ) -> Option<ControllerKind> {
        ControllerKind::from_instance_name(name)
    }

    fn destroy_instance(&mut self, _id: ControllerId, _kind: ControllerKind) {}

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
