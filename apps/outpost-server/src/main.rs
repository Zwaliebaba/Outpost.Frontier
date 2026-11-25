//! `OutpostServer.exe`: the dedicated server's entry point.
#![forbid(unsafe_code)]

use std::process::ExitCode;

fn main() -> ExitCode {
    let options = match outpost_server::parse_args(std::env::args().skip(1)) {
        Ok(options) => options,
        Err(usage) => {
            eprintln!("{usage}");
            return ExitCode::from(2);
        }
    };
    match outpost_server::run(&options, std::io::BufReader::new(std::io::stdin())) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("OutpostServer: {error}");
            ExitCode::FAILURE
        }
    }
}
