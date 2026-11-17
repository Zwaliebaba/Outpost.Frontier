//! Data dumps of the game databases (work package P2.1) against the C++
//! oracle: `Oracle.exe dump` loads the `[Data]` list of
//! `EXE/OutpostFrontier.ini` through Common's loaders and writes one dump
//! per database (`CppSource/Oracle/oracle_game_data.cpp`); each
//! `data_dump_<kind>` test loads the same list through
//! [`game_sim::data::config::GameData`] and renders the same lines.
//!
//! Without `OUTPOST_DATA_DIR` a test prints a notice and passes; see
//! `support/data.rs`.

mod support;

use support::data::{Report, data_dirs};
use support::game::{self, Loaded};

/// Runs one kind: `files` are the pseudo-file names of its databases, each
/// rendered by `render`.
fn run(kind: &str, files: &[&str], render: impl Fn(&Loaded, &str) -> Vec<String>) {
    let Some(dirs) = data_dirs(kind) else {
        return;
    };
    let mut report = Report::new(kind, &dirs);
    match game::load(&dirs.root) {
        Some(loaded) => {
            let names: Vec<String> = files.iter().map(|f| (*f).to_owned()).collect();
            report.file_list(&names);
            for file in files {
                let lines = render(loaded, file);
                report.check(file, &lines);
            }
        }
        None => {
            report.note(format!("no {} under the dump root", game::ENGINE_INI));
            report.file_list(&[]);
        }
    }
    report.finish();
}

#[test]
fn data_dump_constants() {
    run("constants", &["constants"], |loaded, _| {
        game::constants_lines(&loaded.data.constants)
    });
}
