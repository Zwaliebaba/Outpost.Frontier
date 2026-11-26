#![forbid(unsafe_code)]

use game_app::session::{LocalClientEnd, LocalServerRequest, Session};
use game_logic::startup::LocalStartupInfo;
use outpost_frontier::local_server::{RealClock, start_local_server};

/// `StartServer` in single player: `game-logic`'s in-process server on the
/// loopback (record 0009 point 3), on the oracle's clock in its mode.
fn start(request: &LocalServerRequest) -> Option<(Box<dyn Session>, LocalClientEnd)> {
    let info = LocalStartupInfo {
        install_dir: request.install_dir.clone(),
        user_data: request.user_data.clone(),
        seed: request.seed,
    };
    let clock = if request.oracle {
        RealClock::Fixed
    } else {
        RealClock::Wall(None)
    };
    match start_local_server(&info, clock) {
        Ok((session, client)) => Some((Box::new(session), client)),
        Err(e) => {
            eprintln!("the in-process server did not start: {e}");
            None
        }
    }
}

fn main() -> std::io::Result<()> {
    game_app::run(&mut start)
}
