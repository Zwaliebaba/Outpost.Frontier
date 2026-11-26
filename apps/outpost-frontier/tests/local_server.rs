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
