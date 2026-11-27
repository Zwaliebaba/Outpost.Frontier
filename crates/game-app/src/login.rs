//! The single-player connect and login (`CLoginMgr`, `login_mgr.cpp`;
//! `connect_flow.cpp`; `WaitForServerResponse`, `connect_handshake.cpp`;
//! work package P5.3, record 0009).
//!
//! Each step sends one request and then waits for its answer outside the
//! frame loop: the wait runs the server's pump (`G_Server->Update()`, the
//! client's delivery then the server's) until the receiver sets the
//! response flag, the server drops the client or the time runs out; it never
//! runs the server's frame. That is why the C++ client's IClient log has the
//! whole login at frame 0.

use game_sim::protocol::to_server::{Login, ReqDifficultyScale, RequestCharInfo};
use game_sim::protocol::{ClientMessage, ToClient, ToServer};
use neuron_core::diag::{self, DumpFlags};
use neuron_core::net::transport::Transport;

use game_sim::protocol::to_client::CharacterRecord;
use game_sim::protocol::to_server::SelectCharacter;

use crate::receiver::{ClientLink, ClientReceiver};
use crate::session::{Session, tick_modal_server};

/// The single-player wait's limit in seconds (`WaitForServerResponse`'s
/// `SpTimeout` from every caller here).
pub const SP_TIMEOUT: f64 = 60.0;

/// `G_LoginMgr.LoginName` in single player (`menu_enter_state.cpp:225`).
pub const SINGLE_PLAYER_NAME: &str = "SinglePlayer";

/// The login's settings (`CLoginMgr`'s fields the menus set).
#[derive(Clone, Debug, PartialEq)]
pub struct LoginSettings {
    /// `LoginName`.
    pub name: Vec<u16>,
    /// `LoginPassword`.
    pub password: Vec<u16>,
    /// `ReqDifficultyScale`'s value after a single-player login: 0 under
    /// `+G`, else performance option slot 6.
    pub difficulty_scale: f32,
}

impl LoginSettings {
    /// The single-player login (`MenuNavStartSinglePlayer`): `SinglePlayer`
    /// with an empty password.
    #[must_use]
    pub fn single_player(difficulty_scale: f32) -> Self {
        Self {
            name: SINGLE_PLAYER_NAME.encode_utf16().collect(),
            password: Vec::new(),
            difficulty_scale,
        }
    }
}

/// `WaitForServerResponse(bResponse, 5.0f, 60.0f)` in single player: the
/// server's pump until `done` holds, the server drops the client or the
/// session ends, or `now` has moved past the limit since the start (an equal
/// time keeps waiting, as the x87 test does). The single-player answer is
/// always 1.
pub fn wait_for_server_response<T, S>(
    link: &mut ClientLink<T>,
    mut session: Option<&mut S>,
    done: impl Fn(&ClientReceiver) -> bool,
    now: &mut dyn FnMut() -> f64,
) -> i32
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    let start = now();
    while !done(&link.receiver) && !link.receiver.login.server_disconnected {
        let Some(s) = session.as_deref_mut() else {
            break;
        };
        tick_modal_server(Some(s), true, |s| link.deliver_all(Some(s.world())));
        if SP_TIMEOUT < now() - start {
            break;
        }
    }
    1
}

/// `CLoginMgr::Connect(true)` in single player: the notice, then the wait
/// for `OnConnect` (rpclocal's `Connect` makes the connection; here the
/// loopback has it already). A connected client is not disconnected first:
/// the in-process server lives as long as its session.
pub fn connect<T, S>(
    link: &mut ClientLink<T>,
    session: Option<&mut S>,
    now: &mut dyn FnMut() -> f64,
) -> i32
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    link.receiver.login.connected = false;
    diag::dump(
        DumpFlags::NOTICE,
        "Client\\LoginMgr.cpp(351) : NOTICE:General:Starting single player game.\n",
    );
    wait_for_server_response(link, session, |r| r.login.connected, now)
}

/// `CLoginMgr::Login`: nothing without a connection; otherwise the login
/// with `create_account` as its create flag and the name and password
/// bounded to 39 and 15 units, then the wait. A single-player login that
/// answers 3 then sends `ReqDifficultyScale`. The answer is 1 when logged
/// in, else 0.
pub fn login<T, S>(
    link: &mut ClientLink<T>,
    mut session: Option<&mut S>,
    settings: &LoginSettings,
    create_account: bool,
    now: &mut dyn FnMut() -> f64,
) -> i32
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    if !link.receiver.login.connected {
        return 0;
    }
    link.receiver.login.login_response = false;
    let message = Login {
        create_flag: u8::from(create_account),
        account_name: bounded(&settings.name, 0x27),
        second: bounded(&settings.password, 0x0f),
    };
    link.send(ToServer::Client(ClientMessage::Login(message)));
    let result = wait_for_server_response(
        link,
        session.as_deref_mut(),
        |r| r.login.login_response,
        now,
    );
    if result != 1 || link.receiver.login.login_status != 3 {
        return 0;
    }
    if session.is_some() {
        link.send(ToServer::Client(ClientMessage::ReqDifficultyScale(
            ReqDifficultyScale {
                value: settings.difficulty_scale,
            },
        )));
    }
    result
}

/// `LoginToServer` in single player: a login without the create flag, and
/// when it answers status 2 (no user data yet) a second one with it. True
/// when logged in; otherwise the caller shows the error box the status
/// names (`NnErrorRunModal`, which the toolkit's screens bring) and shuts
/// the connection down.
pub fn login_to_server<T, S>(
    link: &mut ClientLink<T>,
    mut session: Option<&mut S>,
    settings: &LoginSettings,
    now: &mut dyn FnMut() -> f64,
) -> bool
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    if session.is_none() {
        return false;
    }
    let mut result = login(link, session.as_deref_mut(), settings, false, now);
    if result != 1 && link.receiver.login.login_status == 2 {
        result = login(link, session, settings, true, now);
    }
    result == 1
}

/// `CLoginMgr::RequestCharacterInfo`: the list asked for and waited for; 0
/// without a server.
pub fn request_character_info<T, S>(
    link: &mut ClientLink<T>,
    session: Option<&mut S>,
    now: &mut dyn FnMut() -> f64,
) -> i32
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    if session.is_none() {
        return 0;
    }
    link.receiver.login.character_info_arrived = false;
    link.receiver.login.characters = None;
    link.send(ToServer::Client(ClientMessage::RequestCharInfo(
        RequestCharInfo {},
    )));
    wait_for_server_response(link, session, |r| r.login.character_info_arrived, now)
}

/// `CLoginMgr::FindRestartCharacter`: the first listed character with
/// `bIsRestart` set (the new game's `Restart.fl`).
#[must_use]
pub fn find_restart_character(receiver: &ClientReceiver) -> Option<&CharacterRecord> {
    listed(receiver).find(|r| r.is_restart != 0)
}

/// `CLoginMgr::FindAutoSaveCharacter`: the first with `bIsAutoSave` set.
#[must_use]
pub fn find_auto_save_character(receiver: &ClientReceiver) -> Option<&CharacterRecord> {
    listed(receiver).find(|r| r.is_auto_save != 0)
}

fn listed(receiver: &ClientReceiver) -> impl Iterator<Item = &CharacterRecord> {
    receiver
        .login
        .characters
        .iter()
        .flat_map(|c| c.records.iter())
}

/// `CLoginMgr::SelectCharacterById`: the listed character with the id
/// selected; false when none has it.
pub fn select_character_by_id<T, S>(
    link: &mut ClientLink<T>,
    session: Option<&mut S>,
    id: &[u8],
    now: &mut dyn FnMut() -> f64,
) -> bool
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    let record = listed(&link.receiver)
        .find(|r| r.character_id == id)
        .cloned();
    select_character(link, session, record.as_ref(), now)
}

/// `CLoginMgr::SelectCharacter`: false without a record; otherwise
/// `Field_0x21c` cleared, `CharacterSelect` sent and waited for; false
/// unless the server verified it, else the record becomes the player's
/// (`ApplyCharacterSelectEquipFields`; its equipment and costume fields are
/// P5.4's) and the answer is true.
pub fn select_character<T, S>(
    link: &mut ClientLink<T>,
    session: Option<&mut S>,
    record: Option<&CharacterRecord>,
    now: &mut dyn FnMut() -> f64,
) -> bool
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    let Some(record) = record else {
        return false;
    };
    link.receiver.login.char_select_verified = false;
    link.send(ToServer::Client(ClientMessage::SelectCharacter(
        SelectCharacter {
            character_id: record.character_id.clone(),
        },
    )));
    wait_for_server_response(link, session, |r| r.login.char_select_verified, now);
    if !link.receiver.login.char_select_verified {
        return false;
    }
    link.receiver.login.selected = Some(record.clone());
    true
}

/// `wcsncpy(dst, src, n + 1)` then `dst[n] = 0`: at most `n` units, up to
/// the first NUL.
fn bounded(units: &[u16], n: usize) -> Vec<u16> {
    units
        .iter()
        .copied()
        .take_while(|&u| u != 0)
        .take(n)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_are_bounded_as_wcsncpy_leaves_them() {
        let long: Vec<u16> = (0..60).map(|i| 0x41 + (i % 26)).collect();
        assert_eq!(bounded(&long, 0x27).len(), 0x27);
        assert_eq!(bounded(&[0x41, 0, 0x42], 0x0f), [0x41]);
        let sp = LoginSettings::single_player(1.0);
        assert_eq!(String::from_utf16_lossy(&sp.name), "SinglePlayer");
    }
}
