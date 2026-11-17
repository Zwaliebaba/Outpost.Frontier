//! The text catalogs against the C++ resource DLLs (work package P1.6).
//!
//! `OUTPOST_CATALOG_DIR` names the folder `cargo xtask catalog --out`
//! wrote, and `OUTPOST_ORACLE_DIR` the output of
//! `Oracle.exe catalog <bin> <dir>`, which loads the resource DLLs built
//! from `CppSource/Resources/` and dumps what the game's lookups read. The
//! test loads the catalogs through [`TextCatalogs`] in the same order,
//! renders the same lines, and requires every module's dump to equal the
//! oracle's line for line.
//!
//! Without both variables the test prints a notice and passes. The text is
//! game content: nothing it reads or writes is committed.

use std::fs;
use std::path::PathBuf;

mod support;

use neuron_core::catalog::{CLIENT_MODULES, SERVER_INTERFACE_MODULE, TextCatalogs};
use support::hex;

const MAX_REPORTED: usize = 10;

fn modules() -> Vec<&'static str> {
    let mut names = CLIENT_MODULES.to_vec();
    names.push(SERVER_INTERFACE_MODULE);
    names
}

/// The oracle's lines for module `index`: all 16 entries of every string
/// block in ID order, then every card in ID order.
fn render(catalogs: &TextCatalogs, index: usize) -> Vec<String> {
    let Some(module) = catalogs.module_at(index) else {
        return Vec::new();
    };
    let base = u32::try_from(index).unwrap() << 16;
    let mut blocks: Vec<u16> = module.strings().map(|(id, _)| id >> 4).collect();
    blocks.dedup();
    let mut lines = Vec::new();
    for block in blocks {
        for entry in 0..16u16 {
            let id = block * 16 + entry;
            let units = catalogs.string(base | u32::from(id));
            let bytes: Vec<u8> = units.iter().flat_map(|u| u.to_le_bytes()).collect();
            lines.push(format!(
                "{{\"string\":\"{id}\",\"len\":\"{}\",\"text\":\"{}\"}}",
                units.len(),
                hex(&bytes)
            ));
        }
    }
    for (id, card) in module.cards() {
        lines.push(format!(
            "{{\"card\":\"{id}\",\"size\":\"{}\",\"data\":\"{}\"}}",
            card.len(),
            hex(card)
        ));
    }
    lines
}

#[test]
fn catalogs_equal_the_resource_dlls() {
    let (Some(catalog_dir), Some(oracle_dir)) = (
        std::env::var_os("OUTPOST_CATALOG_DIR").map(PathBuf::from),
        std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from),
    ) else {
        println!(
            "catalog_dump: OUTPOST_CATALOG_DIR and OUTPOST_ORACLE_DIR are not both set; skipped"
        );
        return;
    };

    let names = modules();
    let (catalogs, failures) = TextCatalogs::load(&catalog_dir, &names);
    for failure in &failures {
        println!("{failure}");
    }
    assert!(
        failures.is_empty(),
        "{} catalogs did not load",
        failures.len()
    );

    let mut problems = Vec::new();
    for (index, name) in names.iter().enumerate() {
        let path = oracle_dir.join(format!("{name}.jsonl"));
        let Ok(text) = fs::read_to_string(&path) else {
            problems.push(format!("{}: cannot read", path.display()));
            continue;
        };
        let oracle: Vec<&str> = text.lines().collect();
        let rust = render(&catalogs, index);
        let mut reported = 0;
        for (line, (want, got)) in oracle.iter().zip(&rust).enumerate() {
            if *want != got && reported < MAX_REPORTED {
                let cut = |s: &str| s.chars().take(160).collect::<String>();
                problems.push(format!(
                    "{name} line {}:\n  oracle {}\n  rust   {}",
                    line + 1,
                    cut(want),
                    cut(got)
                ));
                reported += 1;
            }
        }
        if oracle.len() != rust.len() {
            problems.push(format!(
                "{name}: oracle {} lines, rust {} lines",
                oracle.len(),
                rust.len()
            ));
        }
        println!(
            "{name}: {} lines, {} strings, {} cards",
            rust.len(),
            catalogs.module_at(index).map_or(0, |m| m.string_count()),
            catalogs.module_at(index).map_or(0, |m| m.card_count())
        );
    }
    for problem in &problems {
        println!("{problem}");
    }
    assert!(problems.is_empty(), "{} differences", problems.len());
}
