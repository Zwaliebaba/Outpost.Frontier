//! The in-process single-player server on the original data (`outdir/`,
//! decision O15) behind the client's session (P5.1, record 0009).

use std::path::PathBuf;

use game_app::session::{Session, tick_local_server};
use game_logic::startup::LocalStartupInfo;
use game_sim::protocol::ToClient;
use neuron_core::net::transport::{Transport, TransportEvent};
use outpost_frontier::local_server::{ClientEnd, RealClock, start_local_server};

fn outdir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn names(client: &mut ClientEnd) -> Vec<String> {
    let mut out = Vec::new();
    while let Some(event) = client.poll() {
        out.push(match event {
            TransportEvent::Connected(_) => "connected".to_string(),
            TransportEvent::Disconnected { .. } => "disconnected".to_string(),
            TransportEvent::Message { message, .. } => match message {
                ToClient::Common(m) => m.name().to_string(),
                ToClient::Server(m) => m.name().to_string(),
            },
            #[allow(unreachable_patterns)]
            _ => "other".to_string(),
        });
    }
    out
}

#[test]
fn the_local_server_starts_on_outdir_and_answers_the_connection() {
    let user = std::env::temp_dir().join(format!("outpost-local-{}", std::process::id()));
    let info = LocalStartupInfo {
        install_dir: outdir(),
        user_data: user.clone(),
        seed: 0x4f52_4143,
    };
    let (mut session, mut client) =
        start_local_server(&info, RealClock::Fixed).expect("the server starts on outdir");
    assert!(session.single_player());

    let mut frames = Vec::new();
    for _ in 0..3 {
        let mut delivered = Vec::new();
        tick_local_server(Some(&mut session), false, 1.0 / 30.0, |_| {
            delivered = names(&mut client);
        });
        frames.push(delivered);
    }
    println!("{frames:?}");
    // Frame 1 delivers the client's own connection; the server hears it in
    // the same frame's pump and answers, which the client reads in frame 2:
    // the client id alone, without the multiplayer server's message of the
    // day, as the C++ client's IClient log of menu_walk.txt has it (Startup,
    // OnConnect, then the login's answers; reference-build.md section 8).
    assert_eq!(frames[0], ["connected"]);
    assert_eq!(frames[1], ["FLPACKET_SERVER_CONNECTRESPONSE"]);
    assert!(frames[2].is_empty());
    let _ = std::fs::remove_dir_all(user);
}

/// The single-player login on a fresh user folder and then on the folder it
/// made, against the C++ client's IClient log of `menu_walk.txt` in CI
/// (run 45826847849, runs w and a; reference-build.md section 8, "Hosted
/// runner"): a first start logs in twice (status 2, then 3 with the create
/// flag), a later one once.
#[test]
fn the_single_player_login_reads_as_the_cpp_iclient_log() {
    use game_app::login::{LoginSettings, connect, login_to_server, request_character_info};
    use game_app::receiver::{ClientLink, ClientReceiver};

    let user = std::env::temp_dir().join(format!("outpost-login-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&user);
    let info = LocalStartupInfo {
        install_dir: outdir(),
        user_data: user.clone(),
        seed: 0x4f52_4143,
    };
    let mut runs = Vec::new();
    for _ in 0..2 {
        let (mut session, client) =
            start_local_server(&info, RealClock::Fixed).expect("the server starts on outdir");
        let mut link = ClientLink::new(client, ClientReceiver::new(true));
        // IServerImpl::Startup ends with g_pClientInterface->Startup(1, 0x36d9).
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
        assert!(link.receiver.login.characters.is_some());
        runs.push(
            link.receiver
                .log
                .take()
                .map(|mut l| l.take_lines())
                .unwrap_or_default(),
        );
    }
    let start = [
        r#"{"frame":0,"seq":1,"slot":10,"m":"Startup","a":{"MaxPlayers":1,"Port":14041}}"#,
        r#"{"frame":0,"seq":1,"slot":10,"m":"Startup","r":true}"#,
        r#"{"frame":0,"seq":2,"slot":11,"m":"OnConnect","a":{"ClientId":1}}"#,
    ];
    let fresh: Vec<&str> = start
        .iter()
        .copied()
        .chain([
            r#"{"frame":0,"seq":3,"slot":12,"m":"SendServerLoginResponse","a":{"ClientId":1,"Status":2}}"#,
            r#"{"frame":0,"seq":4,"slot":12,"m":"SendServerLoginResponse","a":{"ClientId":1,"Status":3}}"#,
            r#"{"frame":0,"seq":5,"slot":13,"m":"SendServerCharacterInfo","a":{"ClientId":1,"Characters":"present"}}"#,
        ])
        .collect();
    let again: Vec<&str> = start
        .iter()
        .copied()
        .chain([
            r#"{"frame":0,"seq":3,"slot":12,"m":"SendServerLoginResponse","a":{"ClientId":1,"Status":3}}"#,
            r#"{"frame":0,"seq":4,"slot":13,"m":"SendServerCharacterInfo","a":{"ClientId":1,"Characters":"present"}}"#,
        ])
        .collect();
    assert_eq!(runs[0], fresh);
    assert_eq!(runs[1], again);
    let _ = std::fs::remove_dir_all(user);

    // In CI, against the C++ client's own files: the first lines of
    // `iclient.jsonl` of a run on a fresh user folder
    // (`OUTPOST_CLIENT_ORACLE_FRESH_DIR`) and of one on the folder an earlier
    // run made (`OUTPOST_CLIENT_ORACLE_DIR`).
    for (var, run) in [
        ("OUTPOST_CLIENT_ORACLE_FRESH_DIR", &runs[0]),
        ("OUTPOST_CLIENT_ORACLE_DIR", &runs[1]),
    ] {
        let Some(dir) = std::env::var_os(var).map(PathBuf::from) else {
            println!("notice: {var} is not set; nothing was compared");
            continue;
        };
        let text = std::fs::read_to_string(dir.join("iclient.jsonl")).expect("iclient.jsonl");
        let cpp: Vec<&str> = text.lines().take(run.len()).collect();
        assert_eq!(cpp, *run, "{var}");
        println!("{var}: the first {} IClient lines are equal", run.len());
    }
}
