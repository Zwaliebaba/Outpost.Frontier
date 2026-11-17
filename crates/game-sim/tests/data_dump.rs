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

use support::data::{Report, Tolerance, data_dirs};
use support::game::{self, Loaded};
use support::render::Render;

use neuron_core::vfs::Host;

/// The fields compared within a bound rather than as text (Tier B): a
/// munition's seeker cone went through `FCOS`, a zone's orientation through
/// `FSIN` and `FCOS` and a few products after them.
const TOLERANCE: Tolerance = Tolerance {
    keys: &["fov_cos", "orient"],
    ulps: 4,
    absolute: 1.0e-7,
};

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
                report.check_within(file, &lines, &TOLERANCE);
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

#[test]
fn data_dump_goods() {
    run("goods", &["goods"], |loaded, _| {
        Render {
            strings: &loaded.data.strings,
        }
        .goods(&loaded.data.goods)
    });
}

#[test]
fn data_dump_loadouts() {
    run("loadouts", &["loadouts"], |loaded, _| {
        Render {
            strings: &loaded.data.strings,
        }
        .loadouts(&loaded.data.loadouts)
    });
}

#[test]
fn data_dump_reputation() {
    run("reputation", &["groups"], |loaded, _| {
        Render {
            strings: &loaded.data.strings,
        }
        .groups(&loaded.data.reputation)
    });
}

#[test]
fn data_dump_archetypes() {
    let files = [
        "small_ids",
        "simples",
        "ships",
        "solars",
        "equipment",
        "asteroids",
        "dynamic_asteroids",
        "motors",
        "explosions",
    ];
    run("archetypes", &files, |loaded, file| {
        let db = &loaded.data.archetypes;
        let r = Render {
            strings: &loaded.data.strings,
        };
        match file {
            "small_ids" => Render::small_ids(db),
            "simples" => r.roots(db.simples.values()),
            "ships" => r.ships(db),
            "solars" => r.solars(db),
            "equipment" => r.equipment(db),
            "asteroids" => r.asteroids(db),
            "dynamic_asteroids" => r.roots(db.dynamic_asteroids.values()),
            "motors" => Render::motors(db),
            _ => Render::explosions(db),
        }
    });
}

#[test]
fn data_dump_universe() {
    let files = ["systems", "objects", "bases", "zones"];
    run("universe", &files, |loaded, file| {
        let u = &loaded.data.universe;
        let r = Render {
            strings: &loaded.data.strings,
        };
        match file {
            "systems" => r.systems(u),
            "objects" => {
                let root = std::env::var_os("OUTPOST_DATA_DIR").unwrap_or_default();
                let source = Host::new(std::path::Path::new(&root).join("DATA"));
                r.objects(u, &source)
            }
            "bases" => r.bases(u),
            _ => r.zones(u),
        }
    });
}

#[test]
fn data_dump_bases() {
    run("bases", &["base_data"], |loaded, _| {
        Render::base_data(&loaded.data.bases)
    });
}
