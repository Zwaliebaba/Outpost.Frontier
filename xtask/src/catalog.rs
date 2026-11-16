//! `cargo xtask catalog`: builds the text catalogs (work package P1.6,
//! decision T21) from the resource scripts in `resources/text/`. The file
//! format is documented in `neuron_core::catalog`.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use neuron_core::catalog::{TextCatalog, catalog_file_name};

use crate::rc::{self, CodePage, RcOptions};

pub const USAGE: &str = "  catalog [--source <dir>] [--out <dir>]
                 build one text catalog per module from the resource
                 scripts in <dir>/<Module>/<Module>.rc (default
                 resources/text) into <dir>/<Module>.tcat (default
                 target/catalogs)
";

/// The code page of the resource DLL projects (`/c65001` in
/// `CppSource/Resources/ResourceDll.props`).
const CODE_PAGE: CodePage = CodePage::Utf8;

/// The modules under `source`: every folder `<name>` that holds
/// `<name>.rc`, sorted by name.
pub fn modules(source: &Path) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(source)
        .map(|dir| {
            dir.flatten()
                .filter_map(|entry| entry.file_name().into_string().ok())
                .filter(|name| source.join(name).join(format!("{name}.rc")).is_file())
                .collect()
        })
        .unwrap_or_default();
    names.sort();
    names
}

/// Builds the catalog of module `name` from `<source>/<name>/<name>.rc`.
///
/// Rejected, because the game's lookups would be ambiguous or `rc.exe`
/// rejects them: an ID defined twice, and a string block whose entries
/// have more than one language (`FindResourceEx` finds one block per
/// language, so some of its IDs would be unreachable).
pub fn build_module(source: &Path, name: &str) -> Result<TextCatalog, String> {
    let dir = source.join(name);
    let options = RcOptions {
        code_page: CODE_PAGE,
        include_dirs: vec![dir.clone()],
    };
    let script =
        rc::parse_file(&dir.join(format!("{name}.rc")), &options).map_err(|e| e.to_string())?;

    let mut catalog = TextCatalog::new();
    let mut block_languages: BTreeMap<u16, (u16, String)> = BTreeMap::new();
    let mut first_place: BTreeMap<u16, String> = BTreeMap::new();
    for string in &script.strings {
        let place = format!("{}:{}", string.loc.file.display(), string.loc.line);
        if let Some(first) = first_place.get(&string.id) {
            return Err(format!(
                "{place}: string {} is already defined at {first}",
                string.id
            ));
        }
        first_place.insert(string.id, place.clone());
        let block = string.id >> 4;
        match block_languages.get(&block) {
            Some((language, first)) if *language != string.language => {
                return Err(format!(
                    "{place}: string {} has language 0x{:04x}, but its block has 0x{language:04x} (at {first})",
                    string.id, string.language
                ));
            }
            Some(_) => {}
            None => {
                block_languages.insert(block, (string.language, place.clone()));
            }
        }
        catalog
            .add_string(string.id, string.units.clone())
            .map_err(|e| format!("{place}: {e}"))?;
    }

    let mut card_place: BTreeMap<u16, String> = BTreeMap::new();
    for card in &script.cards {
        let place = format!("{}:{}", card.loc.file.display(), card.loc.line);
        if let Some(first) = card_place.get(&card.id) {
            return Err(format!(
                "{place}: card {} is already defined at {first}",
                card.id
            ));
        }
        card_place.insert(card.id, place.clone());
        let bytes =
            fs::read(&card.path).map_err(|e| format!("{place}: {}: {e}", card.path.display()))?;
        catalog
            .add_card(card.id, bytes)
            .map_err(|e| format!("{place}: {e}"))?;
    }
    Ok(catalog)
}

pub fn run(args: &[String], root: &Path) -> ExitCode {
    let mut source = root.join("resources").join("text");
    let mut out = root.join("target").join("catalogs");
    let mut rest = args.iter();
    while let Some(arg) = rest.next() {
        let target = match arg.as_str() {
            "--source" => &mut source,
            "--out" => &mut out,
            _ => {
                eprintln!("usage: cargo xtask{USAGE}");
                return ExitCode::from(2);
            }
        };
        match rest.next() {
            Some(path) => *target = PathBuf::from(path),
            None => {
                eprintln!("catalog: {arg} needs a folder");
                return ExitCode::from(2);
            }
        }
    }

    let names = modules(&source);
    if names.is_empty() {
        eprintln!(
            "catalog: no <Module>/<Module>.rc under {}",
            source.display()
        );
        return ExitCode::FAILURE;
    }
    if let Err(error) = fs::create_dir_all(&out) {
        eprintln!("catalog: {}: {error}", out.display());
        return ExitCode::FAILURE;
    }
    let mut ok = true;
    for name in &names {
        let written = build_module(&source, name).and_then(|catalog| {
            let bytes = catalog.to_bytes().map_err(|e| e.to_string())?;
            let path = out.join(catalog_file_name(name));
            fs::write(&path, bytes).map_err(|e| format!("{}: {e}", path.display()))?;
            Ok((catalog.string_count(), catalog.card_count(), path))
        });
        match written {
            Ok((strings, cards, path)) => {
                println!(
                    "{name}: {strings} strings, {cards} cards -> {}",
                    path.display()
                );
            }
            Err(error) => {
                eprintln!("catalog: {name}: {error}");
                ok = false;
            }
        }
    }
    if ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn workspace_root() -> PathBuf {
        crate::workspace_root().to_path_buf()
    }

    fn units(text: &str) -> Vec<u16> {
        text.encode_utf16().collect()
    }

    #[test]
    fn fixture_catalog_round_trips() {
        let fixtures = workspace_root().join("xtask/tests/fixtures/rc");
        let dir = std::env::temp_dir().join(format!("xtask-catalog-{}", std::process::id()));
        let module = dir.join("Demo");
        fs::create_dir_all(module.join("sub")).unwrap();
        for file in ["main.rc", "ids.h", "card1.txt", "sub/card2.txt"] {
            let to = module.join(if file == "main.rc" { "Demo.rc" } else { file });
            fs::copy(fixtures.join(file), to).unwrap();
        }
        assert_eq!(modules(&dir), ["Demo"]);
        let built = build_module(&dir, "Demo");
        fs::remove_dir_all(&dir).unwrap();
        let built = built.unwrap();

        assert_eq!(built.string_count(), 15);
        assert_eq!(built.card_count(), 2);
        assert_eq!(built.string(0x20), Some(units("hex id").as_slice()));
        assert_eq!(built.string_entry(12), Some(&[][..]));
        assert_eq!(built.string_entry(0x10), None);
        let mut card1 = vec![0xFF, 0xFE];
        card1.extend(
            units("<RDL><TEXT>One</TEXT></RDL>")
                .iter()
                .flat_map(|u| u.to_le_bytes()),
        );
        assert_eq!(built.card(1), Some(card1.as_slice()));

        let bytes = built.to_bytes().unwrap();
        assert_eq!(TextCatalog::from_bytes(&bytes).unwrap(), built);
    }

    #[test]
    fn duplicates_and_mixed_blocks_are_rejected() {
        let dir = std::env::temp_dir().join(format!("xtask-catalog-dup-{}", std::process::id()));
        let cases = [
            (
                "Dup",
                "STRINGTABLE { 1, \"a\" }\nSTRINGTABLE { 1, \"b\" }\n",
                "already defined",
            ),
            (
                "Mixed",
                "STRINGTABLE { 1, \"a\" }\nSTRINGTABLE LANGUAGE 0, 0 { 2, \"b\" }\n",
                "language",
            ),
            (
                "Card",
                "1 23 \"c.txt\"\n1 HTML \"c.txt\"\n",
                "already defined",
            ),
        ];
        for (name, text, needle) in cases {
            let module = dir.join(name);
            fs::create_dir_all(&module).unwrap();
            fs::write(module.join(format!("{name}.rc")), text).unwrap();
            fs::write(module.join("c.txt"), b"\xff\xfe").unwrap();
            let error = build_module(&dir, name).unwrap_err();
            assert!(error.contains(needle), "{name}: {error}");
        }
        // Separate languages in separate blocks are fine.
        let module = dir.join("Split");
        fs::create_dir_all(&module).unwrap();
        fs::write(
            module.join("Split.rc"),
            "STRINGTABLE { 1, \"a\" }\nSTRINGTABLE LANGUAGE 0, 0 { 16, \"b\" }\n",
        )
        .unwrap();
        let split = build_module(&dir, "Split");
        fs::remove_dir_all(&dir).unwrap();
        assert_eq!(split.unwrap().string_count(), 2);
    }

    /// The real sources: the counts must equal the library ledger
    /// (design/library-ledger.md, rows 32 to 39).
    #[test]
    fn real_tree_matches_the_ledger() {
        let source = workspace_root().join("resources/text");
        let want: [(&str, usize, usize); 8] = [
            ("DialogueText", 0, 3087),
            ("EquipmentCatalog", 784, 784),
            ("FieldGuideText", 0, 886),
            ("InterfaceExtras", 0, 495),
            ("NameCatalog", 7139, 0),
            ("NegotiationText", 1270, 0),
            ("UiText", 3648, 0),
            ("serverinterface", 107, 0),
        ];
        let names = modules(&source);
        let expected: Vec<&str> = want.iter().map(|w| w.0).collect();
        assert_eq!(names, expected);
        for (name, strings, cards) in want {
            let catalog = build_module(&source, name).unwrap_or_else(|e| panic!("{e}"));
            assert_eq!(
                (catalog.string_count(), catalog.card_count()),
                (strings, cards),
                "{name}"
            );
            let bytes = catalog.to_bytes().unwrap();
            assert_eq!(TextCatalog::from_bytes(&bytes).unwrap(), catalog, "{name}");
            // Every card is UTF-16LE text that starts with a byte-order mark.
            for (id, card) in catalog.cards() {
                assert!(card.starts_with(&[0xFF, 0xFE]), "{name} card {id}");
                assert_eq!(card.len() % 2, 0, "{name} card {id}");
            }
        }
        let ui = build_module(&source, "UiText").unwrap();
        assert_eq!(ui.string(0), Some(units("Object Unknown").as_slice()));
        assert_eq!(ui.string(5), Some(units("\n").as_slice()));
    }
}
