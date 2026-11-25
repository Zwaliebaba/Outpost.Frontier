//! The dedicated server's admin commands (work package P3.9): the
//! server-side effects of the MFC admin application's dialogs, which a
//! console replaces (decision O9). Each [`Command`] runs on the server
//! thread with the context, as the dialogs ran under `LockServer`.
//!
//! | Command | C++ |
//! | --- | --- |
//! | [`Command::Accounts`] | `CAccountsDlg::RefreshAccountList` (`OutpostServer/AccountsDlg.cpp:80-110`) |
//! | [`Command::CreateAccount`] | `CCreateAccountDlg::OnOK` (`CreateAccountDlg.cpp:31-67`) |
//! | [`Command::DeleteAccount`] | `CAccountsDlg::OnDeleteAccount` (`AccountsDlg.cpp:241-258`), `PlayerDB::DeleteAccount` |
//! | [`Command::DeleteCharacter`] | `CAccountsDlg::OnDeleteCharacter` (`AccountsDlg.cpp:205-238`) |
//! | [`Command::Ban`] | `CEditAccountDlg::OnOK` (`EditAccountDlg.cpp:105-118`) |
//! | [`Command::Kick`] | `CEditAccountDlg::OnKickAccount` (`EditAccountDlg.cpp:121-134`): `LockAccountAccess` disconnects the account's client |
//! | [`Command::KickClient`] | `IClient::DisconnectClient` of a connection with no account |
//! | [`Command::SetAdmin`] | `PlayerDB::SetAccountAdminRights` (no dialog; the `admin` file) |
//! | [`Command::Players`] | `CChoosePlayerDlg::OnInitDialog` (`ChoosePlayerDlg.cpp:46-60`) |
//! | [`Command::ChatUniverse`] | `CUniverseChatDlg::OnMsgFilterInput` (`UniverseChatDlg.cpp:98-147`) |
//! | [`Command::ChatSystem`] | `CChatSystemDlg::OnMsgFilterInput` (`ChatSystemDlg.cpp`) |
//! | [`Command::ChatPlayer`] | `CChoosePlayerDlg::OnOK` and `CChatAccountDlg::OnMsgFilterInput` (`ChatAccountDlg.cpp:104-160`) |
//! | [`Command::News`], [`Command::SetNews`] | `CServerNewsDlg` (`ServerNewsDlg.cpp`), `SaveMotd`, `PlayerDB::SetMOTD` |
//! | [`Command::Status`] | `CFLSummaryForm::Refresh` (`FLSummaryForm.cpp:128-214`), `IServerImpl::GetServerStats` |
//!
//! The chat a player sends to the console, or to the universe or a
//! system, reaches the console through the chat hooks
//! (`g_pPrivateChatHook` and the others): [`console_chat_hooks`].

use std::fmt::Write as _;

use game_sim::protocol::rdl::{RdlRecord, write_records};
use neuron_server::admin_files::{MOTD_EDIT_CHARS, MOTD_FILE, encode_motd};
use neuron_server::registry::AccountRegistry;

use crate::chat::{CONSOLE, ChatHooks, SYSTEM, UNIVERSE};
use crate::server::Cx;

/// The universe chat's style (`ChatUniverseStyleId`).
pub const UNIVERSE_STYLE: u16 = 0xd681;
/// The system chat's style (`ChatSystemStyleId`).
pub const SYSTEM_STYLE: u16 = 0xd801;
/// The private chat's style (`ChatAccountStyleId`).
pub const PRIVATE_STYLE: u16 = 0x8283;
/// The dialogs' encode buffer (`GChatUniverseBuffer` and the others).
const CHAT_BUFFER: usize = 0x800;
/// The dialogs' input limit (`EM_EXLIMITTEXT` 0x200).
const CHAT_INPUT_CHARS: usize = 0x200;
/// The account dialog's name and password buffers (0x40 characters with
/// the terminator).
const ACCOUNT_TEXT_CHARS: usize = 0x3f;

/// One admin command.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Command {
    /// The accounts with their characters and flags.
    Accounts,
    /// A new account, lower-cased, with a password.
    CreateAccount {
        /// The name.
        name: String,
        /// The password.
        password: String,
    },
    /// An account and its folder deleted; its client is disconnected.
    DeleteAccount {
        /// The account.
        name: String,
    },
    /// A character deleted from its account.
    DeleteCharacter {
        /// The character's name.
        name: String,
    },
    /// The account's ban set or cleared. A change disconnects the
    /// account's client (`LockAccountAccess`).
    Ban {
        /// The account.
        name: String,
        /// Banned.
        ban: bool,
    },
    /// The account's client disconnected (`LockAccountAccess`).
    Kick {
        /// The account.
        name: String,
    },
    /// A connection closed by its client id.
    KickClient {
        /// The client id.
        client: u32,
    },
    /// The account's admin right (universe chat from a player).
    SetAdmin {
        /// The account.
        name: String,
        /// The right.
        admin: bool,
    },
    /// The players with a selected character.
    Players,
    /// Universe chat from the console.
    ChatUniverse {
        /// The text.
        text: String,
    },
    /// System chat from the console to the players in a system.
    ChatSystem {
        /// The system's nickname.
        system: String,
        /// The text.
        text: String,
    },
    /// Private chat from the console to a character's account.
    ChatPlayer {
        /// The character.
        character: String,
        /// The text.
        text: String,
    },
    /// The message of the day.
    News,
    /// A new message of the day, saved and sent to every player.
    SetNews {
        /// The text.
        text: String,
    },
    /// The server's counters and the host's state.
    Status,
}

/// What the admin commands ask of the host: its transport and its own
/// state.
pub trait HostControl {
    /// Closes the connection of `client` as a kick; false when none is
    /// open.
    fn close(&mut self, client: u32) -> bool;

    /// Connections open now.
    fn connection_count(&self) -> usize;

    /// The connection's address, for the player list.
    fn peer(&self, client: u32) -> Option<String>;

    /// The host's lines of the status (name, address, load, uptime).
    fn status_lines(&self) -> Vec<String>;
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

fn narrow(text: &[u16]) -> String {
    String::from_utf16_lossy(text)
}

/// `_wcslwr` in the C locale.
fn lower(text: &[u16]) -> Vec<u16> {
    text.iter()
        .map(|&c| {
            if (u16::from(b'A')..=u16::from(b'Z')).contains(&c) {
                c + 32
            } else {
                c
            }
        })
        .collect()
}

fn cut(mut text: Vec<u16>, chars: usize) -> Vec<u16> {
    text.truncate(chars);
    text
}

/// `IClient::DisconnectClient`: the connection closes, then the game's
/// disconnect runs at once (`IServerImpl::DisConnect`), as the C++ calls
/// it inside the admin's action; the transport's own disconnect event
/// that follows finds the slot free.
pub fn disconnect_client(cx: &mut Cx<'_>, host: &mut dyn HostControl, client: u32) -> bool {
    let closed = host.close(client);
    crate::session::disconnect(cx, client);
    closed
}

/// `LockAccountAccess`: the account's client, if any, is disconnected and
/// logins are refused until [`AccountRegistry::unlock`].
fn lock(cx: &mut Cx<'_>, host: &mut dyn HostControl, name: &[u16]) -> bool {
    match cx.logic.accounts.lock(name) {
        Some(client) => disconnect_client(cx, host, client),
        None => false,
    }
}

/// The chat records the dialogs encode: the style, then the text.
fn styled(style: u16, text: &str) -> Option<Vec<u8>> {
    let text = cut(wide(text), CHAT_INPUT_CHARS);
    let (data, ok) = write_records(
        &[RdlRecord::Style(style), RdlRecord::Text(text)],
        CHAT_BUFFER,
    );
    ok.then_some(data)
}

/// The accounts root of the server, when it has one.
fn accounts_root(cx: &Cx<'_>) -> Option<neuron_server::account::AccountsRoot> {
    cx.logic.saves.accounts.clone()
}

/// The selected character's name of a session (`GetActiveCharacterName`).
fn active_character_name(cx: &Cx<'_>, session: u32) -> Option<Vec<u16>> {
    let slot = cx.logic.players.get(session).filter(|s| s.client_id != 0)?;
    slot.saved_characters
        .get(&slot.selected_character)
        .map(|record| record.name.clone())
}

/// Runs `command`; the lines are the console's answer.
pub fn execute(cx: &mut Cx<'_>, host: &mut dyn HostControl, command: &Command) -> Vec<String> {
    let mut out = Vec::new();
    match command {
        Command::Accounts => accounts(&cx.logic.accounts, &mut out),
        Command::CreateAccount { name, password } => {
            let Some(root) = accounts_root(cx) else {
                out.push("The server has no accounts folder.".into());
                return out;
            };
            let name = lower(&cut(wide(name), ACCOUNT_TEXT_CHARS));
            if name.is_empty() {
                return out;
            }
            let password = cut(wide(password), ACCOUNT_TEXT_CHARS);
            if cx.logic.accounts.create_account(&root, &name) {
                cx.logic.reports.push(
                    "",
                    0,
                    format!(
                        "LOG: Create account {} \"{}\"",
                        narrow(&name),
                        root.account_dir(&name).display()
                    ),
                );
                AccountRegistry::set_account_password(&root, &name, &password);
                out.push(format!("Account {} created.", narrow(&name)));
            } else {
                out.push(format!("Account {} could not be created.", narrow(&name)));
            }
        }
        Command::DeleteAccount { name } => {
            let Some(root) = accounts_root(cx) else {
                return out;
            };
            let name = wide(name);
            let Some(id) = cx.logic.accounts.find_by_name(&name) else {
                out.push(format!("No account {}.", narrow(&name)));
                return out;
            };
            let stored = cx
                .logic
                .accounts
                .get(id)
                .map(|a| a.name.clone())
                .unwrap_or(name);
            if let Some(client) = cx
                .logic
                .accounts
                .get(id)
                .map(|a| a.client_id)
                .filter(|&c| c != 0)
            {
                disconnect_client(cx, host, client);
            }
            cx.logic.reports.push(
                "",
                0,
                format!(
                    "LOG: Delete account {} \"{}\"",
                    narrow(&stored),
                    root.account_dir(&stored).display()
                ),
            );
            cx.logic.accounts.delete_account(&root, &stored);
            out.push(format!("Account {} deleted.", narrow(&stored)));
        }
        Command::DeleteCharacter { name } => {
            let Some(root) = accounts_root(cx) else {
                return out;
            };
            let name = wide(name);
            let Some(account) = cx
                .logic
                .accounts
                .find_by_character_name(&name)
                .and_then(|id| cx.logic.accounts.get(id))
                .map(|a| a.name.clone())
            else {
                out.push(format!("No character {}.", narrow(&name)));
                return out;
            };
            lock(cx, host, &account);
            let id = neuron_server::registry::character_id(&name);
            cx.logic.accounts.delete_character_from_id(&root, &id);
            cx.logic.accounts.unlock(&account);
            out.push(format!("Character {} deleted.", narrow(&name)));
        }
        Command::Ban { name, ban } => {
            let Some(root) = accounts_root(cx) else {
                return out;
            };
            let name = wide(name);
            let Some(not_banned) = cx
                .logic
                .accounts
                .find_by_name(&name)
                .and_then(|id| cx.logic.accounts.get(id))
                .map(|a| a.not_banned)
            else {
                out.push(format!("No account {}.", narrow(&name)));
                return out;
            };
            // The dialog applies the box only when it differs.
            if not_banned == *ban {
                lock(cx, host, &name);
                cx.logic.accounts.ban_account(&root, &name, *ban);
                cx.logic.accounts.unlock(&name);
            }
            out.push(format!(
                "Account {} is {}.",
                narrow(&name),
                if *ban { "banned" } else { "not banned" }
            ));
        }
        Command::Kick { name } => {
            let name = wide(name);
            if cx.logic.accounts.find_by_name(&name).is_none() {
                out.push(format!("No account {}.", narrow(&name)));
                return out;
            }
            let kicked = lock(cx, host, &name);
            cx.logic.accounts.unlock(&name);
            out.push(if kicked {
                format!("Account {} kicked.", narrow(&name))
            } else {
                format!("Account {} is not logged in.", narrow(&name))
            });
        }
        Command::KickClient { client } => {
            out.push(if disconnect_client(cx, host, *client) {
                format!("Client {client} kicked.")
            } else {
                format!("No client {client}.")
            });
        }
        Command::SetAdmin { name, admin } => {
            let Some(root) = accounts_root(cx) else {
                return out;
            };
            let name = wide(name);
            out.push(
                if cx.logic.accounts.set_admin_rights(&root, &name, *admin) {
                    format!(
                        "Account {} {} the admin right.",
                        narrow(&name),
                        if *admin { "has" } else { "lost" }
                    )
                } else {
                    format!("No account {}.", narrow(&name))
                },
            );
        }
        Command::Players => {
            let max = cx.logic.players.max_players() as u32;
            for session in 1..=max {
                let Some(name) = active_character_name(cx, session) else {
                    continue;
                };
                let account = cx
                    .logic
                    .players
                    .get(session)
                    .map(|s| narrow(&s.account_name))
                    .unwrap_or_default();
                let peer = host.peer(session).unwrap_or_default();
                out.push(format!("{session:3} {} ({account}) {peer}", narrow(&name)));
            }
            if out.is_empty() {
                out.push("No players.".into());
            }
        }
        Command::ChatUniverse { text } => {
            if let Some(data) = styled(UNIVERSE_STYLE, text) {
                crate::chat::submit_chat(cx, CONSOLE, &data, UNIVERSE, -1);
            }
        }
        Command::ChatSystem { system, text } => {
            let id = neuron_core::id::create_id(system.as_bytes());
            if !cx.world.data().universe.systems.contains_key(&id) {
                out.push(format!("No system {system}."));
                return out;
            }
            if let Some(data) = styled(SYSTEM_STYLE, text) {
                crate::chat::submit_chat(cx, CONSOLE, &data, SYSTEM, id as i32);
            }
        }
        Command::ChatPlayer { character, text } => {
            let name = wide(character);
            let Some(id) = cx.logic.accounts.find_by_character_name(&name) else {
                out.push(format!("No character {character}."));
                return out;
            };
            // The target is the low word of the account's client id, 0
            // when it is not logged in.
            let target = cx
                .logic
                .accounts
                .get(id)
                .map_or(0, |a| a.client_id & 0xFFFF);
            if let Some(data) = styled(PRIVATE_STYLE, text) {
                crate::chat::submit_chat(cx, CONSOLE, &data, target, -1);
            }
        }
        Command::News => {
            out.push(if cx.logic.motd.is_empty() {
                "No news.".into()
            } else {
                narrow(&cx.logic.motd)
            });
        }
        Command::SetNews { text } => {
            let text = cut(wide(text), MOTD_EDIT_CHARS);
            if let Some(root) = accounts_root(cx) {
                let _ = std::fs::create_dir_all(root.path());
                if std::fs::write(root.local_user_path(MOTD_FILE), encode_motd(&text)).is_err() {
                    out.push("The news could not be saved.".into());
                }
            }
            crate::session::set_motd(cx, &text);
            out.push("News set.".into());
        }
        Command::Status => status(cx, host, &mut out),
    }
    out
}

/// The account list: each account, its flags, then its characters
/// indented, as the dialog's rows.
fn accounts(registry: &AccountRegistry, out: &mut Vec<String>) {
    for (_, account) in registry.iter() {
        let mut line = narrow(&account.name);
        if !account.not_banned {
            line.push_str(" [banned]");
        }
        if account.admin {
            line.push_str(" [admin]");
        }
        if account.client_id != 0 {
            let _ = write!(line, " [client {}]", account.client_id);
        }
        out.push(line);
        for character in &account.characters {
            out.push(format!("    {}", narrow(character)));
        }
    }
    if out.is_empty() {
        out.push("No accounts.".into());
    }
}

/// The summary form's rows: the counters, then the host's lines.
fn status(cx: &mut Cx<'_>, host: &dyn HostControl, out: &mut Vec<String>) {
    let players = cx.logic.players.active_ids().count();
    let stats = cx.logic.stats;
    out.extend(host.status_lines());
    out.push(format!("Connections: {}", host.connection_count()));
    out.push(format!("Player Count: {players}"));
    out.push(format!("Total Logins: {}", stats.logins));
    out.push(format!("Total Logouts: {}", stats.logouts));
    out.push(format!("Failed Logins: {}", stats.failed_logins));
    out.push(format!("Aborted Missions: {}", stats.aborted_missions));
    out.push(format!("Player deaths: {}", stats.player_deaths));
    let loaded = cx
        .logic
        .server_world
        .systems
        .values()
        .filter(|s| s.loaded)
        .count();
    out.push(format!("Active Systems: {loaded}"));
    out.push(format!("Game time: {:.1} s", cx.logic.clock.now()));
}

/// The console's view of the chat (the dialogs' hooks): each message is
/// logged with its player or system.
#[must_use]
pub fn console_chat_hooks() -> ChatHooks {
    fn log(kind: &str, text: &[u16], id: i32) {
        neuron_core::diag::dump(
            neuron_core::diag::DumpFlags::NOTICE,
            &format!("CHAT {kind} {id}: {}", narrow(text)),
        );
    }
    ChatHooks {
        private: Some(|text, id| log("private", text, id)),
        system: Some(|text, id| log("system", text, id)),
        universe: Some(|text, id| log("universe", text, id)),
    }
}
