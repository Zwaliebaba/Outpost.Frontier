//! A new game through the in-process server on the original data
//! (`outdir/`, decision O15), from the login to the player's ship in space
//! (P5.3, record 0009): the client's side of `single_player.txt` up to
//! frame 118, where the C++ client's `NEW GAME` enters space, and the
//! frame after it.
//!
//! The C++ client clicks `NEW GAME` at frame 92 and its menu enters states
//! 16, 12 and 20 and client state 2 in frame 118 (the main menu's column
//! animation takes the frames between, which are the screens' work); every
//! server call of the new game happens inside that frame's waits. The
//! test runs frames 1 to 118 as the frame does (the delivery, the pump and
//! the server's time step), then the new game in frame 118's state update:
//! the restart character selected (menu states 16 and 12), and the entry
//! into space (`InitSpaceState`), then frames 119 and 120.

use std::path::PathBuf;

use game_app::login::{
    LoginSettings, connect, find_restart_character, login_to_server, request_character_info,
    select_character_by_id,
};
use game_app::player_behavior::FlightFrame;
use game_app::receiver::{ClientLink, ClientReceiver};
use game_app::session::tick_local_server;
use game_app::space_camera::SpaceCameraMode;
use game_app::space_camera_third::ShipCameraTable;
use game_app::space_state::{SpaceApp, SpaceEntry, enter_space, update as space_update};
use game_logic::startup::LocalStartupInfo;
use game_sim::data::config::DataConfig;
use neuron_core::vfs::Host;
use outpost_frontier::local_server::{LocalSession, RealClock, start_local_server};

/// The frame the C++ client enters space in (`single_player.txt`).
const SPACE_FRAME: u32 = 118;

/// The frames whose server step the C++ client runs at the x87 FPU's
/// 24-bit precision on the hosted runner (D1400).
const SINGLE_PRECISION_FRAMES: u32 = 2;

/// The frame the port delivers the mission ships' running lights in: the
/// server's step after the entry is frame 119's, delivered in frame 120.
const LIGHTS_FRAME: u32 = SPACE_FRAME + 2;

fn outdir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// One run from a start-up to the frame after entering space; the IClient
/// lines.
fn run(info: &LocalStartupInfo) -> Vec<String> {
    let (mut session, client) =
        start_local_server(info, RealClock::Fixed).expect("the server starts on outdir");
    let mut link = ClientLink::new(client, ClientReceiver::new(true));
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
    let frame = |link: &mut ClientLink<_>, session: &mut LocalSession, n: u32| {
        if let Some(log) = link.receiver.log.as_mut() {
            log.set_frame(n);
        }
        // D1400: the C++ runner's frames 1 and 2 leave the FPU at 24 bits
        // for the server (its server.jsonl: 0, f32(1/30), 2·f32(1/30), then
        // double steps).
        session.single_precision = n <= SINGLE_PRECISION_FRAMES;
        tick_local_server(Some(session), false, 1.0 / 30.0, |s| {
            link.deliver_all(Some(game_app::session::Session::world(s)));
        });
    };
    for n in 1..=SPACE_FRAME {
        frame(&mut link, &mut session, n);
    }
    // Menu state 16: the restart character's id; state 12: selected.
    let restart = find_restart_character(&link.receiver)
        .map(|r| r.character_id.clone())
        .expect("the list has the new game's character");
    assert_eq!(restart, b"Restart.fl");
    assert!(select_character_by_id(
        &mut link,
        Some(&mut session),
        &restart,
        &mut now
    ));
    // Client state 2: InitSpaceState.
    assert_eq!(
        enter_space(&mut link, Some(&mut session), &mut now),
        SpaceEntry::Entered
    );
    assert!(link.receiver.objects.find(11, 1).is_some());
    // StarSysEnter made the system's solars (the server's load_solars wraps
    // them) and the spatial set.
    let solars = link.receiver.objects.solar_count();
    println!("{solars} solars");
    assert!(solars > 0, "the system's solars are registered");
    assert!(link.receiver.objects.spatial().is_some());
    assert_eq!(link.receiver.player.session_flags, 3);
    // UpdateSpaceState: 6 without a camera; with the player's camera the
    // frame stays in space (0).
    let mut app = SpaceApp::new(Default::default(), 0);
    let root = outdir();
    if let Some(config) = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini") {
        app.ship_cameras = ShipCameraTable::from_config(&Host::new(root.join("data")), &config);
    }
    assert!(!app.ship_cameras.is_empty(), "the ships' camera blocks");
    let flight = FlightFrame::default();
    assert_eq!(
        space_update(&mut app, &mut link, Some(&mut session), &flight, true, 0.0),
        6
    );
    app.cameras.switch_mode(b"player");
    assert_eq!(
        space_update(&mut app, &mut link, Some(&mut session), &flight, true, 0.0),
        0
    );
    // The camera had no target, so the frame put `player` back on the ship.
    let ship = link
        .receiver
        .objects
        .find(11, 1)
        .and_then(|(_, o)| o.handle);
    assert_eq!(app.cameras.active_camera().and_then(|c| c.watched), ship);
    // `player` is the chase camera. The shared world has no ship of the
    // player's id until `SpawnPlayerShip`'s `CShip::init` is ported, so the
    // object has no handle yet and the camera watches nothing.
    assert_eq!(app.cameras.active(), Some(SpaceCameraMode::ThirdPerson));
    frame(&mut link, &mut session, SPACE_FRAME + 1);
    frame(&mut link, &mut session, SPACE_FRAME + 2);
    link.receiver
        .log
        .take()
        .map(|mut l| l.take_lines())
        .unwrap_or_default()
}

fn field<'a>(line: &'a str, key: &str) -> &'a str {
    let at = line.find(&format!("\"{key}\":")).expect("the field") + key.len() + 3;
    let rest = &line[at..];
    let end = rest.find([',', '}']).unwrap_or(rest.len());
    &rest[..end]
}

/// The C++ client's lines up to `SPACE_FRAME`, as the port sends them:
/// D857 (an AI's equipment switch outside a behaviour tick reaches the
/// players after the server's next tick) moves the mission ships' running
/// lights (`SendCommonActivateEquip`, which the C++ sends inside the
/// launch, right after each ship's create) to [`LIGHTS_FRAME`], in their
/// order; the call numbers follow.
fn cpp_with_d857(cpp: &[&str]) -> Vec<String> {
    let (lights, rest): (Vec<&str>, Vec<&str>) = cpp
        .iter()
        .copied()
        .filter(|l| field(l, "frame").parse::<u32>().unwrap() <= SPACE_FRAME)
        .partition(|l| field(l, "frame") == SPACE_FRAME.to_string() && field(l, "slot") == "1");
    let moved = lights.iter().map(|l| {
        l.replacen(
            &format!("\"frame\":{SPACE_FRAME},"),
            &format!("\"frame\":{},", LIGHTS_FRAME),
            1,
        )
    });
    let mut seq = 0;
    let mut last = String::new();
    rest.iter()
        .map(|l| (*l).to_string())
        .chain(moved)
        .map(|l| {
            let old = field(&l, "seq").to_string();
            // An answer line keeps its call's number.
            if !l.contains("\"r\":") || old != last {
                seq += 1;
            }
            last = old.clone();
            l.replacen(&format!("\"seq\":{old},"), &format!("\"seq\":{seq},"), 1)
        })
        .collect()
}

#[test]
fn a_new_game_enters_space_as_the_cpp_client_does() {
    let user = std::env::temp_dir().join(format!("outpost-new-game-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&user);
    let info = LocalStartupInfo {
        install_dir: outdir(),
        user_data: user.clone(),
        seed: 0x4f52_4143,
    };
    // The first run makes the user folder (its login answers 2, then 3);
    // the second is the comparison's, as the C++ runs start from a folder.
    let _ = run(&info);
    let lines = run(&info);
    let _ = std::fs::remove_dir_all(user);

    let at = |frame: u32| {
        lines
            .iter()
            .filter(|l| field(l, "frame") == frame.to_string())
            .count()
    };
    // Start-up and login at frame 0; nothing in the menu's frames; the new
    // game's 47 calls in frame 118 and the nine running lights after it.
    assert_eq!(
        (
            at(0),
            at(SPACE_FRAME),
            at(SPACE_FRAME + 1),
            at(LIGHTS_FRAME)
        ),
        (5, 47, 0, 9)
    );
    assert_eq!(lines.len(), 5 + 47 + 9);
    let methods: Vec<&str> = lines.iter().map(|l| field(l, "m")).collect();
    assert_eq!(methods[5], "\"SendCommonSetVisitedState\"");
    assert_eq!(methods[10], "\"SendServerCharSelectVerified\"");
    assert_eq!(methods[13], "\"SendServerLaunch\"");
    assert_eq!(methods[51], "\"SendServerSetMissionObjectives\"");
    for line in &lines {
        println!("{line}");
    }

    // On the hosted runner, against the C++ client's iclient.jsonl of
    // single_player.txt (`OUTPOST_CLIENT_ORACLE_SP_DIR`, run s1).
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_SP_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_SP_DIR is not set; nothing was compared");
        return;
    };
    let text = std::fs::read_to_string(dir.join("iclient.jsonl")).expect("iclient.jsonl");
    let cpp: Vec<&str> = text.lines().collect();
    let expected = cpp_with_d857(&cpp);
    for (i, (port, cpp)) in lines.iter().zip(&expected).enumerate() {
        assert_eq!(port, cpp, "IClient line {}", i + 1);
    }
    assert_eq!(lines.len(), expected.len());
    println!(
        "the {} IClient lines up to frame {} equal the C++ client's (D857's moved lights included)",
        lines.len(),
        LIGHTS_FRAME
    );
}
