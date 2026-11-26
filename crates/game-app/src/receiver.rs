//! The client's event receiver (`IClientImpl`, `client_iclient.cpp`; work
//! package P5.3, record 0009 point 4): each message the server sends is one
//! call of an `IClient` slot, handled in delivery order. Where the C++
//! client's `+oracle` proxy logs the call (`GetClientInterface`), the port's
//! oracle mode logs it too ([`crate::oracle::iclient`]).
//!
//! This first increment handles the calls of a single-player start-up, as
//! the C++ client's IClient log of `menu_walk.txt` has them: `Startup`
//! (slot 10), `OnConnect` (11), `SendServerLoginResponse` (12),
//! `SendServerCharacterInfo` (13) and `Shutdown` (15). The other messages
//! are counted by name until their packages port them.

use std::collections::BTreeMap;

use game_sim::protocol::to_client::CharacterInfo;
use game_sim::protocol::{ServerMessage, ToClient, ToServer};
use neuron_core::net::transport::{ConnectionId, Transport, TransportEvent};

use crate::oracle::iclient::IClientLog;

/// `IClient` slot 10.
pub const SLOT_STARTUP: i32 = 10;
/// `IClient` slot 11.
pub const SLOT_ON_CONNECT: i32 = 11;
/// `IClient` slot 12.
pub const SLOT_LOGIN_RESPONSE: i32 = 12;
/// `IClient` slot 13.
pub const SLOT_CHARACTER_INFO: i32 = 13;
/// `IClient` slot 15.
pub const SLOT_SHUTDOWN: i32 = 15;

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
}

/// `IClientImpl`: the client's handlers of the server's calls.
#[derive(Debug, Default)]
pub struct ClientReceiver {
    /// The state the handlers write.
    pub login: LoginState,
    /// The oracle mode's IClient log, when the mode is on.
    pub log: Option<IClientLog>,
    /// Messages no handler takes yet, by name, with their count.
    pub unported: BTreeMap<&'static str, u32>,
}

impl ClientReceiver {
    /// A receiver; with `oracle`, it logs every call.
    #[must_use]
    pub fn new(oracle: bool) -> Self {
        Self {
            log: oracle.then(IClientLog::new),
            ..Self::default()
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
        if let Some(log) = self.log.as_mut() {
            log.call(SLOT_SHUTDOWN, "Shutdown", |_| {});
        }
    }

    /// One message from the server on `connection`, as the slot it calls.
    pub fn deliver(&mut self, connection: u32, message: ToClient) {
        match message {
            ToClient::Server(ServerMessage::ConnectResponse(m)) => self.on_connect(m.value),
            ToClient::Server(ServerMessage::LoginResponse(m)) => {
                self.login_response(connection, m.value);
            }
            ToClient::Server(ServerMessage::CharacterInfo(m)) => {
                self.character_info(connection, m);
            }
            ToClient::Server(m) => *self.unported.entry(m.name()).or_default() += 1,
            ToClient::Common(m) => *self.unported.entry(m.name()).or_default() += 1,
        }
    }

    /// `IClientImpl::OnConnect` (slot 11): `G_bServerConnected`, then
    /// `CLoginMgr::OnConnectResponse` (the connection id and `bConnected`).
    fn on_connect(&mut self, client_id: u32) {
        if let Some(log) = self.log.as_mut() {
            log.call(SLOT_ON_CONNECT, "OnConnect", |a| {
                a.uint("ClientId", client_id);
            });
        }
        self.login.server_connected = true;
        self.login.connection_id = client_id;
        self.login.connected = true;
    }

    /// `IClientImpl::SendServerLoginResponse` (slot 12):
    /// `CLoginMgr::OnLoginResponse`.
    fn login_response(&mut self, client_id: u32, status: u8) {
        if let Some(log) = self.log.as_mut() {
            log.call(SLOT_LOGIN_RESPONSE, "SendServerLoginResponse", |a| {
                a.uint("ClientId", client_id)
                    .uint("Status", u32::from(status));
            });
        }
        self.login.login_status = status;
        self.login.login_response = true;
    }

    /// `IClientImpl::SendServerCharacterInfo` (slot 13):
    /// `CharacterSelect_ReceiveCharacterInfo`, which sets
    /// `bCharacterInfoArrived` first and copies the records.
    fn character_info(&mut self, client_id: u32, info: CharacterInfo) {
        if let Some(log) = self.log.as_mut() {
            log.call(SLOT_CHARACTER_INFO, "SendServerCharacterInfo", |a| {
                a.uint("ClientId", client_id).str("Characters", "present");
            });
        }
        self.login.character_info_arrived = true;
        self.login.characters = Some(info);
    }
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
    pub fn deliver_all(&mut self) {
        while let Some(event) = self.transport.poll() {
            match event {
                TransportEvent::Connected(connection) => self.server = Some(connection),
                TransportEvent::Disconnected { .. } => {
                    self.receiver.login.server_disconnected = true;
                }
                TransportEvent::Message {
                    connection,
                    message,
                } => self.receiver.deliver(connection.get(), message),
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
        r.deliver(1, ToClient::Server(ConnectResponse { value: 1 }.into()));
        r.deliver(1, ToClient::Server(LoginResponse { value: 3 }.into()));
        r.deliver(1, ToClient::Server(CharacterInfo::default().into()));
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
