//! The client's event receiver (`IClientImpl`, `client_iclient.cpp` and
//! `client_iclient_space.cpp`; work package P5.3, record 0009 point 4):
//! each message the server sends is one call of an `IClient` slot, handled
//! in delivery order. Where the C++ client's `+oracle` proxy logs the call
//! (`GetClientInterface`), the port's oracle mode logs it too
//! ([`crate::oracle::iclient`], the arguments through [`args`]).
//!
//! The slots so far are those of a single-player start-up and of a new
//! game up to entering space, as the C++ client's IClient logs of
//! `menu_walk.txt` and `single_player.txt` have them: `Startup` (slot 10),
//! `OnConnect` (11), `SendServerLoginResponse` (12),
//! `SendServerCharacterInfo` (13), `SendServerCharSelectVerified` (14),
//! `Shutdown` (15), the creates (46, 47), `SendServerLaunch` (60),
//! `SendServerRequestCreateShipResp` (61), `SendServerRequestSpaceScript`
//! (62), `SendServerSetReputation` (66), `SendServerSetMissionObjectives`
//! (72), `SendServerLoadHint` (81), the common blocks (1, 91, 92, 97, 98),
//! the seven `MISCOBJUPDATE` slots (113 to 120), `SendServerSetMusic` (122)
//! and `SendServerPlaySoundEffect` (124). A slot whose effect belongs to a
//! sub-system not ported yet (the reputation vibes, the HUD's comm
//! costumes, the story star, the best path, the space-script camera, the
//! game objects' equipment) logs the call, does what is ported and counts
//! the rest in [`ClientReceiver::unported_effects`]; the other messages are
//! counted in [`ClientReceiver::unported`] until their packages port them.

pub mod args;

use std::collections::BTreeMap;

use game_sim::object::World;
use game_sim::protocol::common::{
    ActivateEquip, SetInterfaceState, SetMissionLog, SetVisitedState, SetWeaponGroup,
};
use game_sim::protocol::records::CreateObject;
use game_sim::protocol::to_client::{
    CharSelectVerified, CharacterInfo, CharacterRecord, Launch, LoadHint, MiscObjUpdate,
    RequestCreateShipResp, RequestSpaceScript, SetMissionObjectives, SetMusic, SetReputation,
};
use game_sim::protocol::{CommonMessage, ServerMessage, ToClient, ToServer};
use neuron_core::diag::{self, DumpFlags};
use neuron_core::net::transport::{ConnectionId, Transport, TransportEvent};

use crate::objects::{ClientObject, ClientObjectKind, ClientObjects};
use crate::oracle::iclient::IClientLog;
use crate::oracle::json::OracleJson;

/// `IClient` slot 1.
pub const SLOT_ACTIVATE_EQUIP: i32 = 1;
/// `IClient` slot 10.
pub const SLOT_STARTUP: i32 = 10;
/// `IClient` slot 11.
pub const SLOT_ON_CONNECT: i32 = 11;
/// `IClient` slot 12.
pub const SLOT_LOGIN_RESPONSE: i32 = 12;
/// `IClient` slot 13.
pub const SLOT_CHARACTER_INFO: i32 = 13;
/// `IClient` slot 14.
pub const SLOT_CHAR_SELECT_VERIFIED: i32 = 14;
/// `IClient` slot 15.
pub const SLOT_SHUTDOWN: i32 = 15;
/// `IClient` slot 46.
pub const SLOT_CREATE_SOLAR: i32 = 46;
/// `IClient` slot 47.
pub const SLOT_CREATE_SHIP: i32 = 47;
/// `IClient` slot 60.
pub const SLOT_LAUNCH: i32 = 60;
/// `IClient` slot 61.
pub const SLOT_CREATE_SHIP_RESP: i32 = 61;
/// `IClient` slot 62.
pub const SLOT_REQUEST_SPACE_SCRIPT: i32 = 62;
/// `IClient` slot 66.
pub const SLOT_SET_REPUTATION: i32 = 66;
/// `IClient` slot 72.
pub const SLOT_SET_MISSION_OBJECTIVES: i32 = 72;
/// `IClient` slot 81.
pub const SLOT_LOAD_HINT: i32 = 81;
/// `IClient` slot 91.
pub const SLOT_SET_WEAPON_GROUP: i32 = 91;
/// `IClient` slot 92.
pub const SLOT_SET_VISITED_STATE: i32 = 92;
/// `IClient` slot 97.
pub const SLOT_SET_MISSION_LOG: i32 = 97;
/// `IClient` slot 98.
pub const SLOT_SET_INTERFACE_STATE: i32 = 98;
/// `IClient` slot 122.
pub const SLOT_SET_MUSIC: i32 = 122;
/// `IClient` slot 124.
pub const SLOT_PLAY_SOUND_EFFECT: i32 = 124;

/// What the receiver's slots write: `CLoginMgr`'s connection and response
/// state (`G_LoginMgr`) and the frame's connection flags.
#[derive(Clone, Debug, Default)]
pub struct LoginState {
    /// `GetPlayerConnectionId()`: the client id `OnConnect` gave.
    pub connection_id: u32,
    /// `bConnected`: the connect response arrived.
    pub connected: bool,
    /// `G_bServerConnected`.
    pub server_connected: bool,
    /// `G_bServerDisconnected`: the server dropped the client.
    pub server_disconnected: bool,
    /// `bLoginResponse`.
    pub login_response: bool,
    /// `LoginStatus`: the last login's status byte (3 logged in, 2 no
    /// such account).
    pub login_status: u8,
    /// `bCharacterInfoArrived`.
    pub character_info_arrived: bool,
    /// The characters the last `CHARACTERINFO` listed
    /// (`CharacterSelect_ReceiveCharacterInfo`'s copy).
    pub characters: Option<CharacterInfo>,
    /// `Field_0x21c`: the character selection was verified
    /// (`OnCharacterSelectResponse`).
    pub char_select_verified: bool,
    /// The verification's record (`NotifyCharacterSelectComplete`'s
    /// argument: the player's vibe and the server time).
    pub char_select: Option<CharSelectVerified>,
    /// The selected character's record (`ApplyCharacterSelectEquipFields`:
    /// the player globals it fills).
    pub selected: Option<CharacterRecord>,
}

/// The player's state the space slots write (`player_spawn.cpp`'s globals,
/// the session flags and the blocks the server keeps for the client).
#[derive(Clone, Debug, Default)]
pub struct PlayerState {
    /// `G_bPlayerCreateShipAnswered`: `REQUESTCREATESHIPRESP` arrived.
    pub create_ship_answered: bool,
    /// `G_PlayerCreateShipResult`, the answer's byte.
    pub create_ship_result: u8,
    /// `G_PlayerCharacterId`: the player's ship id from the answer.
    pub character_id: u32,
    /// `G_bPlayerLaunchAcknowledged`: the player's ship launched
    /// (`SendServerLaunch` found it).
    pub launch_acknowledged: bool,
    /// The pose `SendServerLaunch` beams the ship to (a launch into open
    /// space; the dock's launch sequence is not ported): the ship's id, the
    /// position and the orientation. The receiver has no mutable world, so
    /// the owner applies it ([`crate::space_state::apply_launch_placement`])
    /// when the delivery returns.
    pub launch_placement: Option<(u32, neuron_core::math::Vector, neuron_core::math::Matrix)>,
    /// The game session flags (`AssignGameSessionFlags`, the interface
    /// state's first dword).
    pub session_flags: u32,
    /// The visit flags by id (`SetPlayerVisitFlags`).
    pub visited: BTreeMap<u32, u8>,
    /// The weapon groups' block (`G_WeaponGroups.ReadFromServer`'s input).
    pub weapon_groups: Vec<u8>,
    /// The mission log's block (`StoryStarLoadHistory`'s input).
    pub mission_log: Vec<u8>,
    /// The music override (`StarSysSetMusicOverride`; the music is P5.5's).
    pub music_override: Option<SetMusic>,
    /// The sound effects asked for, in order (`PlayClientSoundArch`; the
    /// sounds are P5.5's).
    pub sound_effects: Vec<u32>,
}

/// `IClientImpl`: the client's handlers of the server's calls.
#[derive(Debug)]
pub struct ClientReceiver {
    /// The state the login's handlers write.
    pub login: LoginState,
    /// The state the space handlers write.
    pub player: PlayerState,
    /// The client's game objects.
    pub objects: ClientObjects,
    /// `SinglePlayer()`.
    pub single_player: bool,
    /// The oracle mode's IClient log, when the mode is on.
    pub log: Option<IClientLog>,
    /// Messages no handler takes yet, by name, with their count.
    pub unported: BTreeMap<&'static str, u32>,
    /// Slots that ran but whose effect on a sub-system not ported yet was
    /// left out, by method, with their count.
    pub unported_effects: BTreeMap<&'static str, u32>,
}

impl Default for ClientReceiver {
    fn default() -> Self {
        Self::new(false)
    }
}

impl ClientReceiver {
    /// A single-player receiver; with `oracle`, it logs every call.
    #[must_use]
    pub fn new(oracle: bool) -> Self {
        Self {
            login: LoginState::default(),
            player: PlayerState::default(),
            objects: ClientObjects::default(),
            single_player: true,
            log: oracle.then(IClientLog::new),
            unported: BTreeMap::new(),
            unported_effects: BTreeMap::new(),
        }
    }

    /// `IClientImpl::Startup(MaxPlayers, Port)` (slot 10): `MOV AL,1`, the
    /// arguments not read. The server's start-up calls it
    /// (`IServerImpl::Startup`'s `g_pClientInterface->Startup`), so the
    /// executable does when it starts the in-process server.
    pub fn startup(&mut self, max_players: u32, port: u32) -> bool {
        let seq = self.log.as_mut().map(|log| {
            log.call(SLOT_STARTUP, "Startup", |a| {
                a.uint("MaxPlayers", max_players).uint("Port", port);
            })
        });
        let answer = true;
        if let (Some(log), Some(seq)) = (self.log.as_mut(), seq) {
            log.answer_bool(SLOT_STARTUP, "Startup", seq, answer);
        }
        answer
    }

    /// `IClientImpl::Shutdown` (slot 15): nothing.
    pub fn shutdown(&mut self) {
        self.call(SLOT_SHUTDOWN, "Shutdown", |_| {});
    }

    /// One message from the server on `connection`, as the slot it calls;
    /// `world` is the session's (the shared one in single player), which
    /// the creates look their objects up in.
    pub fn deliver(&mut self, world: Option<&World>, connection: u32, message: ToClient) {
        let c = connection;
        match message {
            ToClient::Server(m) => match m {
                ServerMessage::ConnectResponse(m) => self.on_connect(m.value),
                ServerMessage::LoginResponse(m) => self.login_response(c, m.value),
                ServerMessage::CharacterInfo(m) => self.character_info(c, m),
                ServerMessage::CharSelectVerified(m) => self.char_select_verified(c, m),
                ServerMessage::CreateSolar(m) => {
                    let marked = m.marked_by != 0;
                    self.create(world, c, &m.object, ClientObjectKind::Solar, marked);
                }
                ServerMessage::CreateShip(m) => {
                    let marked = m.marked_by;
                    self.create(world, c, &m.object, ClientObjectKind::Ship, marked);
                }
                ServerMessage::Launch(m) => self.launch(c, &m),
                ServerMessage::RequestCreateShipResp(m) => self.create_ship_resp(c, &m),
                ServerMessage::RequestSpaceScript(m) => self.request_space_script(c, &m),
                ServerMessage::SetReputation(m) => self.set_reputation(c, &m),
                ServerMessage::SetMissionObjectives(m) => self.set_mission_objectives(c, &m),
                ServerMessage::LoadHint(m) => self.load_hint(c, &m),
                ServerMessage::MiscObjUpdate(m) => self.misc_obj_update(c, &m),
                ServerMessage::SetMusic(m) => self.set_music(c, m),
                ServerMessage::PlaySoundEffect(m) => self.play_sound_effect(c, m.value),
                m => *self.unported.entry(m.name()).or_default() += 1,
            },
            ToClient::Common(m) => match m {
                CommonMessage::ActivateEquip(m) => self.activate_equip(c, &m),
                CommonMessage::SetWeaponGroup(m) => self.set_weapon_group(c, m),
                CommonMessage::SetVisitedState(m) => self.set_visited_state(c, &m),
                CommonMessage::SetMissionLog(m) => self.set_mission_log(c, m),
                CommonMessage::SetInterfaceState(m) => self.set_interface_state(c, &m),
                m => *self.unported.entry(m.name()).or_default() += 1,
            },
        }
    }

    /// The proxy's line of one call, when the mode is on.
    fn call(&mut self, slot: i32, method: &str, args: impl FnOnce(&mut OracleJson)) {
        if let Some(log) = self.log.as_mut() {
            log.call(slot, method, args);
        }
    }

    /// A slot's effect on a sub-system not ported yet.
    fn unported_effect(&mut self, method: &'static str) {
        *self.unported_effects.entry(method).or_default() += 1;
    }

    /// `IClientImpl::OnConnect` (slot 11): `G_bServerConnected`, then
    /// `CLoginMgr::OnConnectResponse` (the connection id and `bConnected`).
    fn on_connect(&mut self, client_id: u32) {
        self.call(SLOT_ON_CONNECT, "OnConnect", |a| {
            a.uint("ClientId", client_id);
        });
        self.login.server_connected = true;
        self.login.connection_id = client_id;
        self.login.connected = true;
    }

    /// `IClientImpl::SendServerLoginResponse` (slot 12):
    /// `CLoginMgr::OnLoginResponse`.
    fn login_response(&mut self, client_id: u32, status: u8) {
        self.call(SLOT_LOGIN_RESPONSE, "SendServerLoginResponse", |a| {
            a.uint("ClientId", client_id)
                .uint("Status", u32::from(status));
        });
        self.login.login_status = status;
        self.login.login_response = true;
    }

    /// `IClientImpl::SendServerCharacterInfo` (slot 13):
    /// `CharacterSelect_ReceiveCharacterInfo`, which sets
    /// `bCharacterInfoArrived` first and copies the records.
    fn character_info(&mut self, client_id: u32, info: CharacterInfo) {
        self.call(SLOT_CHARACTER_INFO, "SendServerCharacterInfo", |a| {
            a.uint("ClientId", client_id).str("Characters", "present");
        });
        self.login.character_info_arrived = true;
        self.login.characters = Some(info);
    }

    /// `IClientImpl::SendServerCharSelectVerified` (slot 14):
    /// `CLoginMgr::OnCharacterSelectResponse` (`Field_0x21c`), then
    /// `NotifyCharacterSelectComplete` with the record.
    fn char_select_verified(&mut self, client_id: u32, info: CharSelectVerified) {
        let bytes = args::char_select_verified(&info);
        self.call(
            SLOT_CHAR_SELECT_VERIFIED,
            "SendServerCharSelectVerified",
            |a| {
                a.uint("ClientId", client_id).bytes("Info", &bytes);
            },
        );
        self.login.char_select_verified = true;
        self.login.char_select = Some(info);
    }

    /// `IClientImpl::SendServerCreateSolar` (slot 46) and
    /// `SendServerCreateShip` (47): nothing while no system is entered
    /// (`PhySys::AnyActive`); in single player the shared world's object of
    /// the id (`Gameobj::find`, a warning and nothing when there is none),
    /// then the object registered (`ClientObjDb_RegisterGameobj`). The
    /// game object's own set-up (companions, materials, watch list,
    /// collisions) is P5.4's.
    fn create(
        &mut self,
        world: Option<&World>,
        client_id: u32,
        object: &CreateObject,
        kind: ClientObjectKind,
        marked_by: bool,
    ) {
        let archetype = world.map_or(0, |w| w.data().archetypes.small_to_large(object.archetype));
        let (slot, method, line, label) = if kind == ClientObjectKind::Solar {
            (SLOT_CREATE_SOLAR, "SendServerCreateSolar", 0x67, "Solar")
        } else {
            (SLOT_CREATE_SHIP, "SendServerCreateShip", 0xff, "Ship")
        };
        self.call(slot, method, |a| {
            a.uint("ClientId", client_id)
                .begin_object("Info")
                .uint("ObjId", object.obj_id)
                .uint("ArchetypeNickname", archetype)
                .end_object();
        });
        let _ = marked_by;
        if !self.objects.physics_active {
            return;
        }
        let handle = world.and_then(|w| w.find(object.obj_id));
        if self.single_player && handle.is_none() {
            diag::dump(
                DumpFlags::WARNING,
                &format!(
                    "{ICLIENT_S_CPP}({line}) : *** WARNING: {label}Create - CObject {} not found.",
                    object.obj_id
                ),
            );
            return;
        }
        let object = ClientObject {
            id: object.obj_id,
            kind,
            archetype,
            handle,
            remote_physics: false,
        };
        match world {
            Some(world) => self.objects.register(object, world),
            None => self.objects.register(object, &()),
        };
        self.unported_effect(method);
    }

    /// `IClientImpl::SendServerLaunch` (slot 60): the ship (bucket 1), a
    /// warning and nothing when there is none; otherwise it is placed (P5.4)
    /// and the launch acknowledged.
    fn launch(&mut self, client_id: u32, info: &Launch) {
        let bytes = args::launch(info);
        self.call(SLOT_LAUNCH, "SendServerLaunch", |a| {
            a.uint("ClientId", client_id).bytes("Info", &bytes);
        });
        if self.objects.find(info.field_00, 1).is_none() {
            diag::dump(
                DumpFlags::WARNING,
                &format!(
                    "{ICLIENT_S_CPP}(1476) : *** WARNING: Launch (): launching object {} not found.",
                    info.field_00
                ),
            );
            return;
        }
        // Outside a dock the ship is beamed to the pose; from a dock (in
        // single player the server's launch places it) the client's
        // `LaunchFromDock` sequence is not ported.
        if info.field_04 == 0 {
            let [w, x, y, z] = info.orientation;
            let orientation =
                neuron_core::math::quaternion_to_matrix(&neuron_core::math::Quaternion {
                    w,
                    x,
                    y,
                    z,
                });
            let [px, py, pz] = info.position;
            self.player.launch_placement = Some((
                info.field_00,
                neuron_core::math::Vector::new(px, py, pz),
                orientation,
            ));
        } else {
            self.unported_effect("SendServerLaunch");
        }
        self.player.launch_acknowledged = true;
    }

    /// `IClientImpl::SendServerRequestCreateShipResp` (slot 61):
    /// `SetPlayerCreateShipResponse` (answered, the byte, the ship id).
    fn create_ship_resp(&mut self, client_id: u32, info: &RequestCreateShipResp) {
        self.call(
            SLOT_CREATE_SHIP_RESP,
            "SendServerRequestCreateShipResp",
            |a| {
                a.uint("ClientId", client_id)
                    .bool("bSuccess", info.field_00 != 0)
                    .uint("ShipId", info.field_04);
            },
        );
        self.player.create_ship_answered = true;
        self.player.create_ship_result = u8::from(info.field_00 != 0);
        self.player.character_id = info.field_04;
    }

    /// `IClientImpl::SendServerRequestSpaceScript` (slot 62): nothing
    /// without the object; the space-script camera of mode 1 or 2 is P5.4's.
    fn request_space_script(&mut self, client_id: u32, info: &RequestSpaceScript) {
        let bytes = args::request_space_script(info);
        self.call(
            SLOT_REQUEST_SPACE_SCRIPT,
            "SendServerRequestSpaceScript",
            |a| {
                a.uint("ClientId", client_id).bytes("Info", &bytes);
            },
        );
        if self.objects.find(info.field_00, 0).is_some() {
            self.unported_effect("SendServerRequestSpaceScript");
        }
    }

    /// `IClientImpl::SendServerSetReputation` (slot 66): the vibe's
    /// attitude and affiliation (multiplayer) or the whole vibe (single
    /// player); the reputation vibes are P5.4's.
    fn set_reputation(&mut self, client_id: u32, info: &SetReputation) {
        let single_player = self.single_player;
        self.call(SLOT_SET_REPUTATION, "SendServerSetReputation", |a| {
            a.uint("ClientId", client_id);
            if single_player {
                // rpclocal's payload carries no attitude.
                let mut sp = info.clone();
                sp.attitude = 0.0;
                args::set_reputation(a, "Info", &sp);
            } else {
                args::set_reputation(a, "Info", info);
            }
        });
        self.unported_effect("SendServerSetReputation");
    }

    /// `IClientImpl::SendServerSetMissionObjectives` (slot 72):
    /// `BestPathSetObjectives`, P5.7's.
    fn set_mission_objectives(&mut self, client_id: u32, info: &SetMissionObjectives) {
        self.call(
            SLOT_SET_MISSION_OBJECTIVES,
            "SendServerSetMissionObjectives",
            |a| {
                a.uint("ClientId", client_id);
                args::set_mission_objectives(a, "Info", info);
            },
        );
        self.unported_effect("SendServerSetMissionObjectives");
    }

    /// `IClientImpl::SendServerLoadHint` (slot 81): a costume hint (kind 1)
    /// preloads the comm costume (`HudCommPreloadCostume`, P5.7's).
    fn load_hint(&mut self, client_id: u32, hint: &LoadHint) {
        self.call(SLOT_LOAD_HINT, "SendServerLoadHint", |a| {
            a.uint("ClientId", client_id)
                .begin_object("Hint")
                .uint("Kind", hint.kind)
                .end_object();
        });
        if hint.kind == 1 {
            self.unported_effect("SendServerLoadHint");
        }
    }

    /// `MISCOBJUPDATE`'s slots in the receiver's order
    /// (`rpc_server_channel_receive.cpp`, case 0x54): affiliation (116),
    /// player or object group (118, 113), mission (114), message of the day
    /// (115), rank (117), system (119), formation (120).
    fn misc_obj_update(&mut self, c: u32, m: &MiscObjUpdate) {
        let f = m.flags;
        if f & 0x20 != 0 {
            self.call(116, "SendServerMiscObjUpdateAffiliation", |a| {
                a.uint("ClientId", c)
                    .uint("ObjectId", m.field_28)
                    .uint("Affiliation", m.field_54);
            });
            // The object's vibe, else the player's when the id is the
            // player's ship: Reputation::Vibe::SetAffiliation (P5.4).
            self.unported_effect("SendServerMiscObjUpdateAffiliation");
        }
        if f & 0x01 != 0 {
            if f & 0x04 != 0 {
                self.call(118, "SendServerMiscObjUpdatePlayerGroup", |a| {
                    a.uint("ClientId", c)
                        .uint("PlayerId", m.field_24)
                        .uint("GroupId", m.field_34);
                });
                self.unported_effect("SendServerMiscObjUpdatePlayerGroup");
            } else {
                self.call(113, "SendServerMiscObjUpdateObjectGroup", |a| {
                    a.uint("ClientId", c)
                        .uint("ObjectId", m.field_28)
                        .uint("GroupId", m.field_34);
                });
                self.unported_effect("SendServerMiscObjUpdateObjectGroup");
            }
        }
        if f & 0x02 != 0 {
            self.call(114, "SendServerMiscObjUpdateMission", |a| {
                a.uint("ClientId", c)
                    .uint("MissionId", m.field_38)
                    .uint("MissionSetBy", m.field_3c);
            });
            self.unported_effect("SendServerMiscObjUpdateMission");
        }
        if f & 0x10 != 0 {
            self.call(115, "SendServerMiscObjUpdateMotd", |a| {
                a.uint("ClientId", c).wstr("Motd", &m.text);
            });
            self.unported_effect("SendServerMiscObjUpdateMotd");
        }
        if f & 0x40 != 0 {
            self.call(117, "SendServerMiscObjUpdateRank", |a| {
                a.uint("ClientId", c)
                    .uint("PlayerId", m.field_24)
                    .int("Rank", i32::from(m.field_2c));
            });
            self.unported_effect("SendServerMiscObjUpdateRank");
        }
        if f & 0x80 != 0 {
            self.call(119, "SendServerMiscObjUpdateSystem", |a| {
                a.uint("ClientId", c)
                    .uint("PlayerId", m.field_24)
                    .uint("SystemId", m.field_30);
            });
            self.unported_effect("SendServerMiscObjUpdateSystem");
        }
        if f & 0x100 != 0 {
            self.call(120, "SendServerMiscObjUpdateFormation", |a| {
                a.uint("ClientId", c).begin_array("MemberIds");
                for &id in &m.ids {
                    a.item_uint(id);
                }
                a.end_array();
            });
            self.unported_effect("SendServerMiscObjUpdateFormation");
        }
    }

    /// `IClientImpl::SendServerSetMusic` (slot 122): the system's music
    /// override, kept for the music (P5.5).
    fn set_music(&mut self, client_id: u32, m: SetMusic) {
        self.call(SLOT_SET_MUSIC, "SendServerSetMusic", |a| {
            a.uint("ClientId", client_id)
                .uint("SpaceMusic", m.field_20)
                .uint("DangerMusic", m.field_24)
                .uint("BattleMusic", m.field_28)
                .uint("OverrideMusic", m.field_2c)
                .bool("bFlag", m.field_30 != 0)
                .float("Fade", m.field_34);
        });
        self.player.music_override = Some(m);
    }

    /// `IClientImpl::SendServerPlaySoundEffect` (slot 124): the sound
    /// archetype played, queued for the sounds (P5.5).
    fn play_sound_effect(&mut self, client_id: u32, sound: u32) {
        self.call(SLOT_PLAY_SOUND_EFFECT, "SendServerPlaySoundEffect", |a| {
            a.uint("ClientId", client_id).uint("SoundId", sound);
        });
        self.player.sound_effects.push(sound);
    }

    /// `IClientImpl::SendCommonActivateEquip` (slot 1): the object's
    /// equipment switch (`IObjAffect` slot 19, P5.4 and P5.6), nothing
    /// without the object.
    fn activate_equip(&mut self, client_id: u32, info: &ActivateEquip) {
        let bytes = args::activate_equip(info);
        self.call(SLOT_ACTIVATE_EQUIP, "SendCommonActivateEquip", |a| {
            a.uint("ClientId", client_id).bytes("Info", &bytes);
        });
        if self.objects.find(info.object_id, 0).is_some() {
            self.unported_effect("SendCommonActivateEquip");
        }
    }

    /// `IClientImpl::SendCommonSetWeaponGroup` (slot 91): the weapon
    /// groups' block (`G_WeaponGroups.ReadFromServer`, P5.7's reader).
    fn set_weapon_group(&mut self, client_id: u32, m: SetWeaponGroup) {
        self.call(SLOT_SET_WEAPON_GROUP, "SendCommonSetWeaponGroup", |a| {
            a.uint("ClientId", client_id)
                .bytes("Data", &m.data)
                .uint("Size", block_size(&m.data));
        });
        self.player.weapon_groups = m.data;
    }

    /// `IClientImpl::SendCommonSetVisitedState` (slot 92): a count, then
    /// that many records of an id and a flag byte, each given to
    /// `SetPlayerVisitFlags`. A count past the block reads no further.
    fn set_visited_state(&mut self, client_id: u32, m: &SetVisitedState) {
        self.call(SLOT_SET_VISITED_STATE, "SendCommonSetVisitedState", |a| {
            a.uint("ClientId", client_id)
                .bytes("Data", &m.data)
                .uint("Size", block_size(&m.data));
        });
        let count = i32::from_le_bytes(dword(&m.data, 0));
        for index in 0..usize::try_from(count).unwrap_or(0) {
            let at = 4 + index * 5;
            let Some(&flags) = m.data.get(at + 4) else {
                break;
            };
            let id = u32::from_le_bytes(dword(&m.data, at));
            self.player.visited.insert(id, flags);
        }
    }

    /// `IClientImpl::SendCommonSetMissionLog` (slot 97): the story star's
    /// history (`StoryStarLoadHistory`, P5.7's reader).
    fn set_mission_log(&mut self, client_id: u32, m: SetMissionLog) {
        self.call(SLOT_SET_MISSION_LOG, "SendCommonSetMissionLog", |a| {
            a.uint("ClientId", client_id)
                .bytes("Data", &m.data)
                .uint("Size", block_size(&m.data));
        });
        self.player.mission_log = m.data;
    }

    /// `IClientImpl::SendCommonSetInterfaceState` (slot 98):
    /// `AssignGameSessionFlags` with the block's first dword.
    fn set_interface_state(&mut self, client_id: u32, m: &SetInterfaceState) {
        self.call(
            SLOT_SET_INTERFACE_STATE,
            "SendCommonSetInterfaceState",
            |a| {
                a.uint("ClientId", client_id)
                    .bytes("Data", &m.data)
                    .uint("Size", block_size(&m.data));
            },
        );
        self.player.session_flags = u32::from_le_bytes(dword(&m.data, 0));
    }
}

/// The source file the client's space warnings name.
const ICLIENT_S_CPP: &str = "Client\\space\\IClientS.cpp";

/// A block's size as the slot's `Size` argument.
fn block_size(data: &[u8]) -> u32 {
    u32::try_from(data.len()).unwrap_or(u32::MAX)
}

/// The four bytes at `at`, zeros past the block's end.
fn dword(data: &[u8], at: usize) -> [u8; 4] {
    let mut word = [0; 4];
    for (i, out) in word.iter_mut().enumerate() {
        *out = data.get(at + i).copied().unwrap_or(0);
    }
    word
}

/// The client's end of the transport with its receiver.
pub struct ClientLink<T> {
    /// The transport to the server.
    pub transport: T,
    /// The receiver of what arrives.
    pub receiver: ClientReceiver,
    server: Option<ConnectionId>,
}

impl<T> ClientLink<T>
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
{
    /// A link over `transport`.
    pub fn new(transport: T, receiver: ClientReceiver) -> Self {
        Self {
            transport,
            receiver,
            server: None,
        }
    }

    /// The server's half of `G_Server->Update()` that reaches the client:
    /// every event the transport holds, in order, to the receiver
    /// (rpclocal's client-bound drain, `rpclocal.cpp:536-550`). A closed
    /// connection is `G_bServerDisconnected`.
    pub fn deliver_all(&mut self, world: Option<&World>) {
        while let Some(event) = self.transport.poll() {
            match event {
                TransportEvent::Connected(connection) => self.server = Some(connection),
                TransportEvent::Disconnected { .. } => {
                    self.receiver.login.server_disconnected = true;
                }
                TransportEvent::Message {
                    connection,
                    message,
                } => self.receiver.deliver(world, connection.get(), message),
            }
        }
    }

    /// One message to the server; false without a connection or when the
    /// transport refuses it.
    pub fn send(&mut self, message: impl Into<ToServer>) -> bool {
        let Some(connection) = self.server else {
            return false;
        };
        self.transport.send(connection, message.into()).is_ok()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use game_sim::protocol::to_client::{ConnectResponse, LoginResponse};

    #[test]
    fn the_start_up_calls_are_logged_and_set_the_login_state() {
        let mut r = ClientReceiver::new(true);
        assert!(r.startup(1, 0x36d9));
        r.deliver(
            None,
            1,
            ToClient::Server(ConnectResponse { value: 1 }.into()),
        );
        r.deliver(None, 1, ToClient::Server(LoginResponse { value: 3 }.into()));
        r.deliver(None, 1, ToClient::Server(CharacterInfo::default().into()));
        r.shutdown();
        assert!(r.login.connected && r.login.server_connected);
        assert_eq!((r.login.connection_id, r.login.login_status), (1, 3));
        assert!(r.login.character_info_arrived && r.login.login_response);
        assert_eq!(
            r.log.as_ref().map(IClientLog::lines).unwrap_or_default(),
            [
                r#"{"frame":0,"seq":1,"slot":10,"m":"Startup","a":{"MaxPlayers":1,"Port":14041}}"#,
                r#"{"frame":0,"seq":1,"slot":10,"m":"Startup","r":true}"#,
                r#"{"frame":0,"seq":2,"slot":11,"m":"OnConnect","a":{"ClientId":1}}"#,
                r#"{"frame":0,"seq":3,"slot":12,"m":"SendServerLoginResponse","a":{"ClientId":1,"Status":3}}"#,
                r#"{"frame":0,"seq":4,"slot":13,"m":"SendServerCharacterInfo","a":{"ClientId":1,"Characters":"present"}}"#,
                r#"{"frame":0,"seq":5,"slot":15,"m":"Shutdown","a":{}}"#,
            ]
        );
    }
}
