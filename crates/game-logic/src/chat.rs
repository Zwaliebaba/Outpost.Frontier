//! The server's chat router (work package P3.2): `Server_SubmitChat`, its
//! slash commands, the group verbs and `Server_SendStyledChat`
//! (`Server/server_chat.cpp`).
//!
//! Messages are binary rich text ([`game_sim::protocol::rdl`]). A message
//! is prefixed with the sender's name and routed on its target chat id:
//! a player (byte 2 of the id is 0), the universe, the sender's system,
//! local space or the sender's group. Local chat reaches the players whose
//! ships are near the sender through the system database (P3.4); until
//! then only the sender gets it back.
//!
//! The commands are `std::wregex` patterns in the C++, tried from the last
//! to the first: `group`/`g`, `local`/`l`, `system`/`s`, `universe`/`u`
//! (each a word, at least one space, then the text up to a line break,
//! which re-targets the message), `leave`/`lv` (a prefix only), `join`/`j`
//! and `invite`/`i` (a character name), `echo`. The port matches the same
//! shapes directly; the space class is
//! [`neuron_server::chat_space::is_chat_space`].

use game_sim::protocol::rdl::{RdlRecord, extract_text, write_records};
use neuron_server::chat_space::is_chat_space;

use crate::group::{self, GROUP_CONTROL_CHAT_ID};
use crate::server::Cx;

/// The console (or nobody).
pub const CONSOLE: u32 = 0x0000_0000;
/// Every player.
pub const UNIVERSE: u32 = 0x0001_0000;
/// The sender's system.
pub const SYSTEM: u32 = 0x0001_0001;
/// Local space.
pub const LOCAL: u32 = 0x0001_0002;
/// The sender's group.
pub const GROUP: u32 = 0x0001_0003;

/// The largest encoded message.
const MAX_MESSAGE: usize = 0x400;
const BAD_NAME: &str = ">>>BAD   NAME<<<";
const CHAT_SERVER: &str = "Server\\FLChatServer.cpp";

/// The server's chat hooks (`g_pPrivateChatHook`, `g_pSystemChatHook`,
/// `g_pUniverseChatHook`): a host's view of the traffic, with the
/// message's text and a player or system.
#[derive(Clone, Copy, Debug, Default)]
pub struct ChatHooks {
    /// Private chat to or from the console, with the other player.
    pub private: Option<fn(&[u16], i32)>,
    /// System and local chat, with the system.
    pub system: Option<fn(&[u16], i32)>,
    /// Universe chat, with the sender.
    pub universe: Option<fn(&[u16], i32)>,
}

fn w(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

fn notice(cx: &mut Cx<'_>, line: u32, message: &str) {
    cx.logic
        .reports
        .push(CHAT_SERVER, line, format!("NOTICE:General:{message}"));
}

/// `PlayerData_HasSelectedCharacter`: always outside a multiplayer server.
fn has_selected_character(cx: &Cx<'_>, player: u32) -> bool {
    !cx.logic.multiplayer_server
        || cx
            .logic
            .players
            .get(player)
            .is_some_and(|s| s.selected_character.is_valid())
}

/// `PlayerData_GetCharacterName`: the vibe's display name.
fn character_name(cx: &mut Cx<'_>, player: u32) -> Vec<u16> {
    let vibe = cx.logic.players.get(player).map_or(-1, |s| s.rep_vibe);
    let (vibes, _) = cx.world.vibes();
    vibes
        .name(vibe)
        .map_or_else(|| w(BAD_NAME), |(_, _, display)| display)
}

/// `_wcsicmp(a, b) == 0` in the "C" locale.
fn wcsicmp_eq(a: &[u16], b: &[u16]) -> bool {
    let fold = |c: u16| {
        if (u16::from(b'A')..=u16::from(b'Z')).contains(&c) {
            c + 32
        } else {
            c
        }
    };
    a.len() == b.len() && a.iter().zip(b).all(|(&x, &y)| fold(x) == fold(y))
}

/// `Server_SendStyledChat`: a styled text from the console to `target`.
pub fn send_styled_chat(cx: &mut Cx<'_>, style: u16, target: u32, text: &[u16]) {
    let (data, ok) = write_records(
        &[RdlRecord::Style(style), RdlRecord::Text(text.to_vec())],
        MAX_MESSAGE,
    );
    if ok {
        cx.logic.chat.send(target, CONSOLE, data);
    }
}

/// `ChatGroup_Invite`: `invitee` is invited to `inviter`'s group (made for
/// it when it has none).
pub fn group_invite(cx: &mut Cx<'_>, inviter: u32, invitee: u32, name: Option<&[u16]>) {
    if !cx.logic.players.is_valid(invitee) || !has_selected_character(cx, invitee) {
        return;
    }
    if let Some(name) = name
        && !wcsicmp_eq(name, &character_name(cx, invitee))
    {
        return;
    }
    let group = match cx.logic.players.get(inviter).and_then(|s| s.group) {
        Some(id) => id,
        None => {
            let id = cx.logic.groups.create();
            group::add_member(cx, id, inviter);
            id
        }
    };
    let client = |cx: &Cx<'_>, p: u32| cx.logic.players.get(p).map_or(0, |s| s.client_id);
    let (inviter_client, invitee_client) = (client(cx, inviter), client(cx, invitee));
    if inviter_client == invitee_client {
        group::send_notice(cx, inviter, 0x9, inviter_client);
        return;
    }
    if cx
        .logic
        .players
        .get(invitee)
        .is_some_and(|s| s.group_mission_id != 0)
    {
        group::send_notice(cx, inviter_client, 0xf, invitee_client);
        return;
    }
    let (count, member) = cx.logic.groups.get(group).map_or((0, false), |g| {
        (g.members.len(), g.members.contains(&invitee_client))
    });
    if count >= group::MAX_MEMBERS {
        group::send_notice(cx, inviter, 0xa, inviter_client);
        return;
    }
    if member {
        group::send_notice(cx, inviter, 0xb, invitee_client);
        return;
    }
    if let Some(g) = cx.logic.groups.get_mut(group) {
        g.invites.insert(invitee_client);
    }
    group::send_notice(cx, inviter, group::notice::INVITED, invitee_client);
    group::send_notice(cx, invitee_client, group::notice::INVITED_YOU, inviter);
}

/// `ChatGroup_Join`: `joiner` joins `inviter`'s group when invited.
pub fn group_join(cx: &mut Cx<'_>, joiner: u32, inviter: u32, name: Option<&[u16]>) {
    if !cx.logic.players.is_valid(inviter) || !has_selected_character(cx, inviter) {
        return;
    }
    if let Some(name) = name
        && !wcsicmp_eq(name, &character_name(cx, inviter))
    {
        return;
    }
    let client = |cx: &Cx<'_>, p: u32| cx.logic.players.get(p).map_or(0, |s| s.client_id);
    let (joiner_client, inviter_client) = (client(cx, joiner), client(cx, inviter));
    let old_group = cx.logic.players.get(joiner).and_then(|s| s.group);
    let group = cx.logic.players.get(inviter).and_then(|s| s.group);
    if joiner_client == inviter_client {
        group::send_notice(cx, joiner, 0xc, joiner_client);
        return;
    }
    let Some(group) = group.filter(|&g| {
        cx.logic
            .groups
            .get(g)
            .is_some_and(|g| g.invites.contains(&joiner_client))
    }) else {
        group::send_notice(cx, joiner, 0xd, joiner_client);
        return;
    };
    let (count, member) = cx.logic.groups.get(group).map_or((0, false), |g| {
        (g.members.len(), g.members.contains(&joiner_client))
    });
    if member {
        group::send_notice(cx, joiner, 0xe, inviter_client);
        return;
    }
    if cx
        .logic
        .players
        .get(joiner)
        .is_some_and(|s| s.group_mission_id != 0)
    {
        group::send_notice(cx, joiner_client, 0xf, joiner_client);
        return;
    }
    if count >= group::MAX_MEMBERS {
        group::send_notice(cx, joiner, 0x10, inviter_client);
        return;
    }
    if let Some(old) = old_group {
        group::del_member(cx, old, joiner_client);
    }
    group::send_notice(cx, joiner_client, group::notice::JOINED, inviter_client);
    group::add_member(cx, group, joiner_client);
    if let Some(g) = cx.logic.groups.get_mut(group) {
        g.invites.remove(&joiner_client);
    }
}

/// `ChatGroup_Leave`: the player leaves its group, or hears it has none.
pub fn group_leave(cx: &mut Cx<'_>, player: u32) {
    let Some(slot) = cx.logic.players.get(player) else {
        return;
    };
    let client = slot.client_id;
    match slot.group {
        Some(group) => {
            group::del_member(cx, group, client);
        }
        None => group::send_notice(cx, client, 0x12, client),
    }
}

/// What a command leaves for the router.
struct Command {
    target: u32,
    data: Vec<u8>,
}

/// `^word<space>+([^\n]*)` with `match_continuous`: the text after the
/// spaces up to the first line break, for the first word that fits.
fn word_and_text<'t>(text: &'t [u16], words: &[&str]) -> Option<&'t [u16]> {
    for word in words {
        let word = w(word);
        let Some(rest) = text.strip_prefix(word.as_slice()) else {
            continue;
        };
        let spaces = rest.iter().take_while(|&&u| is_chat_space(u)).count();
        if spaces == 0 {
            continue;
        }
        let rest = &rest[spaces..];
        let end = rest
            .iter()
            .position(|&u| u == u16::from(b'\n'))
            .unwrap_or(rest.len());
        return Some(&rest[..end]);
    }
    None
}

/// `ChatCommand_Redirect`: the text re-encoded with a style for a channel.
fn redirect(cx: &mut Cx<'_>, text: &[u16], channel: u32, style: u16) -> Option<Command> {
    let (data, ok) = write_records(
        &[RdlRecord::Style(style), RdlRecord::Text(text.to_vec())],
        MAX_MESSAGE,
    );
    if ok {
        Some(Command {
            target: channel,
            data,
        })
    } else {
        notice(cx, 0x21e, "Failed to re-encode chat message.\n");
        None
    }
}

/// The commands, last to first; `Err(())` when a command took the message,
/// `Ok(None)` when none matched, `Ok(Some)` for a re-targeted message.
fn run_command(cx: &mut Cx<'_>, sender: u32, text: &[u16]) -> Result<Option<Command>, ()> {
    let player = sender & 0xFFFF;
    let is_player = (sender >> 16) & 0xFF == 0;
    for (words, channel, style) in [
        (&["group", "g"][..], GROUP, 0xba28),
        (&["local", "l"][..], LOCAL, 0x1d9d),
        (&["system", "s"][..], SYSTEM, 0xd801),
        (&["universe", "u"][..], crate::chat::UNIVERSE, 0xd681),
    ] {
        if let Some(rest) = word_and_text(text, words) {
            let rest = rest.to_vec();
            return match redirect(cx, &rest, channel, style) {
                Some(command) => Ok(Some(command)),
                None => Err(()),
            };
        }
    }
    if text.starts_with(&w("leave")) || text.starts_with(&w("lv")) {
        if is_player {
            group_leave(cx, player);
        }
        return Err(());
    }
    for (words, join) in [(&["join", "j"][..], true), (&["invite", "i"][..], false)] {
        if let Some(name) = word_and_text(text, words) {
            let name = name.to_vec();
            if is_player
                && let Some(account) = cx.logic.accounts.find_by_character_name(&name)
                && let Some(other) = cx
                    .logic
                    .accounts
                    .get(account)
                    .map(|a| a.client_id)
                    .filter(|&c| c != 0)
            {
                if join {
                    group_join(cx, player, other, Some(&name));
                } else {
                    group_invite(cx, player, other, Some(&name));
                }
            }
            return Err(());
        }
    }
    if let Some(rest) = word_and_text(text, &["echo"]) {
        let (data, ok) = write_records(
            &[RdlRecord::Style(0x3ea8), RdlRecord::Text(rest.to_vec())],
            MAX_MESSAGE,
        );
        if ok {
            cx.logic.chat.send(sender, sender, data);
        } else {
            notice(cx, 0x1ff, "Failed to encode echo message.\n");
        }
        return Err(());
    }
    Ok(None)
}

/// `Server_SubmitChat(sender, data, target, system)`: a message from a chat
/// id (a player, or the console) to a chat id; `system` names the
/// console's system for system chat (-1 for none).
pub fn submit_chat(cx: &mut Cx<'_>, sender: u32, data: &[u8], target: u32, system: i32) {
    let mut target = target;
    if target == GROUP_CONTROL_CHAT_ID {
        if data.len() != 8 {
            return;
        }
        let word = |i: usize| u32::from_le_bytes(data[i..i + 4].try_into().unwrap_or_default());
        let player = sender & 0xFFFF;
        match word(0) {
            0 => group_invite(cx, player, word(4), None),
            1 => group_leave(cx, player),
            2 => group_join(cx, player, word(4), None),
            _ => {}
        }
        return;
    }
    let mut payload = data.to_vec();
    let (text, _) = extract_text(data, MAX_MESSAGE);
    if text.first() == Some(&u16::from(b'/')) {
        match run_command(cx, sender, &text[1..]) {
            Err(()) => return,
            Ok(Some(command)) => {
                target = command.target;
                payload = command.data;
            }
            Ok(None) => {}
        }
    }
    let sender_word = sender & 0xFFFF;
    let sender_slot = cx.logic.players.is_valid(sender_word);
    let mut records = Vec::new();
    if sender_slot {
        records.push(RdlRecord::Style(0xd5ec));
        let name = if has_selected_character(cx, sender_word) {
            character_name(cx, sender_word)
        } else {
            w("<unnamed>")
        };
        records.push(RdlRecord::Text(name));
    } else {
        records.push(RdlRecord::Style(0x6c0e));
        records.push(RdlRecord::Text(w("CONSOLE")));
    }
    records.push(RdlRecord::Text(w(": ")));
    let (mut message, ok) = write_records(&records, MAX_MESSAGE);
    if !ok {
        notice(cx, 0x2d6, "Failed to encode chat sender name.\n");
        return;
    }
    if message.len() + payload.len() <= MAX_MESSAGE {
        message.extend_from_slice(&payload);
    } else {
        records.push(RdlRecord::Attributes {
            attrs: 0x1d1d_bf11,
            mask: 0xFFFF_FFFF,
        });
        records.push(RdlRecord::Text(w("<message too big>")));
        let (rewritten, ok) = write_records(&records, MAX_MESSAGE);
        if !ok {
            notice(
                cx,
                0x2e7,
                "Failed to encode \"too big chat\" error message.\n",
            );
            return;
        }
        message = rewritten;
    }
    let (shown, _) = extract_text(&message, MAX_MESSAGE);
    let hooks = cx.logic.chat_hooks;
    let active: Vec<u32> = cx.logic.players.active_ids().collect();

    if (target >> 16) & 0xFF == 0 {
        if let Some(hook) = hooks.private {
            if sender == CONSOLE {
                hook(&shown, (target & 0xFFFF) as i32);
            } else if target == CONSOLE {
                hook(&shown, sender_word as i32);
            }
        }
        if cx.logic.players.is_valid(target & 0xFFFF) {
            cx.logic.chat.send(target, sender, message.clone());
        }
        cx.logic.chat.send(sender, target, message);
        return;
    }
    if target == UNIVERSE {
        // A sender without a registered account is refused as one without
        // the rights, where the C++ reads through the null account (D608).
        let admin = cx
            .logic
            .players
            .get(sender_word)
            .and_then(|s| s.account)
            .and_then(|a| cx.logic.accounts.get(a))
            .is_some_and(|a| a.admin);
        if sender_slot && !admin {
            notice(
                cx,
                0x31e,
                "Universe chat attempted from an account without the proper rights.\n",
            );
            return;
        }
        if let Some(hook) = hooks.universe {
            hook(&shown, sender_word as i32);
        }
        for player in active {
            let client = cx.logic.players.get(player).map_or(0, |s| s.client_id);
            cx.logic.chat.send(client & 0xFFFF, target, message.clone());
        }
        return;
    }
    let sender_system = cx
        .logic
        .players
        .get(sender_word)
        .filter(|_| sender_slot)
        .map(|s| s.system_id);
    if target == SYSTEM {
        if sender_system.is_none() && system == -1 {
            notice(
                cx,
                0x340,
                "System chat attempted without a player or console system.\n",
            );
            return;
        }
        if let Some(hook) = hooks.system {
            hook(&shown, sender_system.map_or(system, |s| s as i32));
        }
        for player in active {
            let Some(slot) = cx.logic.players.get(player) else {
                continue;
            };
            if sender_system == Some(slot.system_id) || slot.system_id as i32 == system {
                let client = slot.client_id;
                cx.logic.chat.send(client & 0xFFFF, target, message.clone());
            }
        }
        return;
    }
    if target == LOCAL {
        let Some(system_id) = sender_system else {
            notice(cx, 0x36c, "Local chat attempted from console.\n");
            return;
        };
        if let Some(hook) = hooks.system {
            hook(&shown, system_id as i32);
        }
        cx.logic.chat.send(sender, target, message);
        return;
    }
    if target == GROUP {
        match cx
            .logic
            .players
            .get(sender_word)
            .filter(|_| sender_slot)
            .and_then(|s| s.group)
        {
            Some(group) => group::deliver_chat(cx, group, &message),
            None => notice(
                cx,
                0x37a,
                "Console group chat attempted. Feature is not implemented.\n",
            ),
        }
        return;
    }
    notice(
        cx,
        0x381,
        "Received message to unknown chat list. Ignoring.\n",
    );
}
