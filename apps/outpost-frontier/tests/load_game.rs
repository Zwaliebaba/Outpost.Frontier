//! A saved game loaded from the main menu through the in-process server on
//! the original data (`outdir/`, decision O15), against the C++ client's
//! `load_game.txt` (P5.3, record 0009).
//!
//! The C++ client opens the single-player character page (menu state 18)
//! at frame 118, whose creation asks for the character list; the script
//! selects row 0 (the newest save, `AutoSave.fl`, which `single_player.txt`
//! left in the first base) and loads it, and the page's state 12 selects
//! the save in frame 191. The client then enters the save's base (client
//! state 3), whose base entry (`BaseEnter`, `LocationEnter`, frame 192) is
//! the bases' (P5.9); this test stops at the selection.
//!
//! After the comparison the test enters the save's base as the C++ GF
//! state's `Init(1)` does (`EnterClientLocationBase`), through the location
//! controller against the same server, whose maps were built at the
//! connection as the client builds them (D1702): every base and room
//! message the server sends then finds its entry, so none is skipped.
//!
//! The saves come from the client-oracle job: `OUTPOST_CLIENT_ORACLE_ROOT`
//! names its output folder, whose `s2/user` is the user folder
//! `single_player.txt`'s second run left and `l1` the first run of
//! `load_game.txt` from it. Without the variable the test prints a notice
//! and returns, as the oracle data tests do.

use std::path::{Path, PathBuf};

use game_app::audio::Silent;
use game_app::base::location_controller::{LinkServer, LocationController};
use game_app::base::location_host::LocationParts;
use game_app::login::{
    LoginSettings, connect, login_to_server, request_character_info, select_character_by_id,
};
use game_app::receiver::{ClientLink, ClientReceiver};
use game_app::session::{Session, tick_local_server};
use game_logic::startup::LocalStartupInfo;
use outpost_frontier::local_server::{LocalSession, RealClock, start_local_server};

/// The frame the C++ client's character page asks for the list in.
const PAGE_FRAME: u32 = 118;

/// The frame the C++ client selects the save in.
const SELECT_FRAME: u32 = 191;

/// The frames whose server step the C++ runs at 24-bit precision (D1400).
const SINGLE_PRECISION_FRAMES: u32 = 2;

fn outdir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn copy_dir(from: &Path, to: &Path) {
    std::fs::create_dir_all(to).expect("the user folder copy");
    for entry in std::fs::read_dir(from).expect("the user folder") {
        let entry = entry.expect("an entry");
        let target = to.join(entry.file_name());
        if entry.file_type().expect("its type").is_dir() {
            copy_dir(&entry.path(), &target);
        } else {
            std::fs::copy(entry.path(), target).expect("a file copy");
        }
    }
}

fn frame(
    link: &mut ClientLink<outpost_frontier::local_server::ClientEnd>,
    session: &mut LocalSession,
    n: u32,
) {
    if let Some(log) = link.receiver.log.as_mut() {
        log.set_frame(n);
    }
    session.single_precision = n <= SINGLE_PRECISION_FRAMES;
    tick_local_server(Some(session), false, 1.0 / 30.0, |s| {
        link.deliver_all(Some(s.world()));
    });
}

#[test]
fn a_saved_game_loads_as_the_cpp_client_loads_it() {
    let Some(root) = std::env::var_os("OUTPOST_CLIENT_ORACLE_ROOT").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_ROOT is not set; nothing was compared");
        return;
    };
    let user = std::env::temp_dir().join(format!("outpost-load-game-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&user);
    copy_dir(&root.join("s2").join("user"), &user);
    let info = LocalStartupInfo {
        install_dir: outdir(),
        user_data: user.clone(),
        seed: 0x4f52_4143,
    };
    let (mut session, client) =
        start_local_server(&info, RealClock::Fixed).expect("the server starts on outdir");
    let mut link = ClientLink::new(client, ClientReceiver::new(true));
    // The location maps, as the client builds them at the connection.
    let data = session.world().data_arc();
    let mut location = LocationController::new();
    location.build_maps(&data.bases, &data.goods, &mut link.receiver.location);
    assert!(link.receiver.startup(1, 0x36d9));
    let mut t = 0.0;
    let mut now = || {
        t += 0.001;
        t
    };
    let settings = LoginSettings::single_player(1.0);
    assert_eq!(connect(&mut link, Some(&mut session), &mut now), 1);
    assert!(login_to_server(
        &mut link,
        Some(&mut session),
        &settings,
        &mut now
    ));
    assert_eq!(
        request_character_info(&mut link, Some(&mut session), &mut now),
        1
    );
    for n in 1..=SELECT_FRAME {
        frame(&mut link, &mut session, n);
        if n == PAGE_FRAME {
            // PageCharacterSelectSingle's creation: the list again.
            assert_eq!(
                request_character_info(&mut link, Some(&mut session), &mut now),
                1
            );
        }
    }
    // Row 0 of the page: the first listed character that is not the
    // restart file (RefillLists).
    let save = link
        .receiver
        .login
        .characters
        .iter()
        .flat_map(|c| c.records.iter())
        .find(|r| r.is_restart == 0)
        .map(|r| r.character_id.clone())
        .expect("a save to load");
    assert_eq!(save, b"AutoSave.fl");
    assert!(select_character_by_id(
        &mut link,
        Some(&mut session),
        &save,
        &mut now
    ));
    // The save is in a base, so the client enters state 3 (GF Init(1)).
    let base = link
        .receiver
        .login
        .selected
        .as_ref()
        .map_or(0, |r| r.base_id);
    assert_eq!(base, 2_882_678_595);
    let lines = link
        .receiver
        .log
        .take()
        .map(|mut l| l.take_lines())
        .unwrap_or_default();

    // The save's base entered through the controller (GF Init(1)).
    let source = session.world().data_host().expect("the server's data host");
    let mut parts = LocationParts::default();
    let mut sound = Silent::default();
    {
        let mut host = parts.host(&source, &mut sound, None, None, true);
        let mut server = LinkServer {
            link: &mut link,
            session: Some(&mut session),
            now: &mut now,
        };
        location.enter_base(base, &mut server, &mut host);
        assert!(!host.sync_failed, "the base's synchronisation failed");
    }
    for n in SELECT_FRAME + 1..=SELECT_FRAME + 30 {
        frame(&mut link, &mut session, n);
    }
    let db = &link.receiver.location;
    assert_eq!(db.skipped, 0, "a base or room message the maps do not know");
    assert_eq!(location.current_base, base);
    assert_ne!(
        location.current_room, 0,
        "the base's start location entered"
    );
    assert!(db.base(base).is_some(), "the base's runtime opened");
    assert!(
        db.room(location.current_room).is_some(),
        "the start room's state opened"
    );
    println!(
        "the base {base} entered in room {}; no base or room message skipped",
        location.current_room
    );
    let _ = std::fs::remove_dir_all(user);

    let text =
        std::fs::read_to_string(root.join("l1").join("iclient.jsonl")).expect("iclient.jsonl");
    let cpp: Vec<&str> = text
        .lines()
        .filter(|l| {
            let at = l.find("\"frame\":").map_or(0, |i| i + 8);
            l[at..]
                .split(',')
                .next()
                .and_then(|f| f.parse::<u32>().ok())
                .is_some_and(|f| f <= SELECT_FRAME)
        })
        .collect();
    for (i, (port, cpp)) in lines.iter().zip(&cpp).enumerate() {
        assert_eq!(port, cpp, "IClient line {}", i + 1);
    }
    assert_eq!(lines.len(), cpp.len());
    println!(
        "the {} IClient lines up to frame {SELECT_FRAME} equal the C++ client's load_game.txt",
        lines.len()
    );
}
