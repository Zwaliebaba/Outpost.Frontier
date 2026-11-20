//! Chat and groups on a multiplayer server over `outdir/`: two players
//! with characters invite and join through the group-control messages, a
//! group message, the slash commands, private chat, the universe's rights
//! check and leaving the group.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::chat::{self, GROUP, UNIVERSE};
use game_logic::group::{GROUP_CHAT_ID, GROUP_CONTROL_CHAT_ID};
use game_logic::login::{self, CreateCharacterInfo, LoginInfo, ServerFiles};
use game_logic::player::CharacterId;
use game_logic::selection::select_character;
use game_logic::server::{ChatSink, Cx, Logic, RecordingSink};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::protocol::rdl::{RdlRecord, extract_text, write_records};
use neuron_core::id::create_id;
use neuron_core::vfs::Host;
use neuron_server::account::{AccountsRoot, hashed_name};

/// One chat message: to, from, data.
type Sent = (u32, u32, Vec<u8>);

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<Sent>>>);

impl ChatSink for Shared {
    fn send(&mut self, to: u32, from: u32, data: Vec<u8>) {
        self.0.borrow_mut().push((to, from, data));
    }
}

fn w(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

fn message(text: &str) -> Vec<u8> {
    write_records(&[RdlRecord::Text(w(text))], 0x400).0
}

fn notice(kind: u32, param: u32) -> Vec<u8> {
    [kind.to_le_bytes(), param.to_le_bytes()].concat()
}

fn text_of(data: &[u8]) -> String {
    String::from_utf16_lossy(&extract_text(data, 0x400).0)
}

#[test]
fn players_group_and_chat() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no chat");
        return;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let ini = std::fs::read(outdir.join("OutpostFrontier.ini")).expect("ini");
    let user_data = Path::new(env!("CARGO_TARGET_TMPDIR")).join("chat-groups");
    let _ = std::fs::remove_dir_all(&user_data);
    let files = ServerFiles {
        exe_dir: outdir.clone(),
        data_dir: outdir.join("data"),
        initial_world: ServerFiles::read_initial_world(&ini),
        accounts: AccountsRoot::new(&user_data, true),
    };
    let mut world = World::new(
        Arc::new(data),
        WorldConfig {
            single_player: false,
            ..WorldConfig::default()
        },
    );
    let chat = Shared::default();
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 3, true);
    logic.chat = Box::new(chat.clone());
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for (session, (account, pilot)) in [("Lead", "Hawk"), ("Wing", "Dove")].into_iter().enumerate()
    {
        let session = session as u32 + 1;
        let info = LoginInfo {
            account_name: w(account),
            create: true,
        };
        assert_eq!(login::login(&mut cx, &files, &info, session), 3);
        let create = CreateCharacterInfo {
            name: w(pilot),
            faction: create_id(b"new_player"),
            base: create_id(b"he01_01_base"),
            package: create_id(b"ge_fighter"),
            pilot: create_id(b"rellan"),
        };
        assert!(login::create_character(&mut cx, &files, session, &create));
        let id = CharacterId::new(format!("{}.fl", hashed_name(&create.name)).as_bytes());
        assert!(select_character(&mut cx, &files, session, &id));
    }
    let take = || std::mem::take(&mut *chat.0.borrow_mut());

    // 1 invites 2: a group is made for 1; 1 hears it invited 2, 2 that 1
    // invited it.
    take();
    chat::submit_chat(&mut cx, 1, &notice(0, 2), GROUP_CONTROL_CHAT_ID, -1);
    let group = cx
        .logic
        .players
        .get(1)
        .unwrap()
        .group
        .expect("1 has a group");
    assert_eq!(
        take(),
        [
            (1, GROUP_CONTROL_CHAT_ID, notice(3, 2)),
            (2, GROUP_CONTROL_CHAT_ID, notice(4, 1)),
        ]
    );
    // 2 joins: it hears it joined, 1 that 2 joined, 2 that 1 is a member.
    chat::submit_chat(&mut cx, 2, &notice(2, 1), GROUP_CONTROL_CHAT_ID, -1);
    assert_eq!(cx.logic.players.get(2).unwrap().group, Some(group));
    assert_eq!(
        take(),
        [
            (2, GROUP_CONTROL_CHAT_ID, notice(5, 1)),
            (1, GROUP_CONTROL_CHAT_ID, notice(6, 2)),
            (2, GROUP_CONTROL_CHAT_ID, notice(6, 1)),
        ]
    );

    // Group chat: every member gets "Hawk: hi" from the group chat id.
    chat::submit_chat(&mut cx, 1, &message("hi"), GROUP, -1);
    let sent = take();
    assert_eq!(
        sent.iter()
            .map(|(to, from, _)| (*to, *from))
            .collect::<Vec<_>>(),
        [(1, GROUP_CHAT_ID), (2, GROUP_CHAT_ID)]
    );
    assert_eq!(text_of(&sent[0].2), "Hawk: hi");

    // `/g` re-targets a private message to the group.
    chat::submit_chat(&mut cx, 2, &message("/g  over here"), 1, -1);
    let sent = take();
    assert_eq!(sent.len(), 2);
    assert_eq!(text_of(&sent[1].2), "Dove: over here");

    // `/echo` answers the sender only, and the message goes no further.
    chat::submit_chat(&mut cx, 2, &message("/echo ping"), 1, -1);
    let sent = take();
    assert_eq!(sent.len(), 1);
    assert_eq!(
        (sent[0].0, sent[0].1, text_of(&sent[0].2).as_str()),
        (2, 2, "ping")
    );

    // Private chat: the target and the sender each get it.
    chat::submit_chat(&mut cx, 1, &message("psst"), 2, -1);
    let sent = take();
    assert_eq!(
        sent.iter()
            .map(|(to, from, _)| (*to, *from))
            .collect::<Vec<_>>(),
        [(2, 1), (1, 2)]
    );

    // Universe chat needs the account's admin right.
    chat::submit_chat(&mut cx, 1, &message("all"), UNIVERSE, -1);
    assert!(take().is_empty());
    assert!(
        cx.logic
            .accounts
            .set_admin_rights(&files.accounts, &w("Lead"), true)
    );
    chat::submit_chat(&mut cx, 1, &message("all"), UNIVERSE, -1);
    assert_eq!(take().len(), 2);

    // `/leave`: 2 leaves; 1 hears it, 2 that it was removed.
    chat::submit_chat(&mut cx, 2, &message("/leave"), 1, -1);
    assert_eq!(cx.logic.players.get(2).unwrap().group, None);
    assert_eq!(
        take(),
        [
            (1, GROUP_CONTROL_CHAT_ID, notice(8, 2)),
            (2, GROUP_CONTROL_CHAT_ID, notice(7, 2)),
        ]
    );
    // Leaving again: 2 hears it has no group.
    chat::submit_chat(&mut cx, 2, &notice(1, 0), GROUP_CONTROL_CHAT_ID, -1);
    assert_eq!(take(), [(2, GROUP_CONTROL_CHAT_ID, notice(0x12, 2))]);
}
