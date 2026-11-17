//! `cargo xtask catalog`: builds the text catalogs (work package P1.6,
//! decision T21) from the text sources in `resources/text/`. The source
//! layout is in that folder's README; the catalog format is documented in
//! `neuron_core::catalog`.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use neuron_core::catalog::{TextCatalog, catalog_file_name};
use toml::{Table, Value};

pub const USAGE: &str = "  catalog [--source <dir>] [--out <dir>]
                 build one text catalog per module from the text sources
                 <dir>/<Module>.toml (default resources/text) into
                 <dir>/<Module>.tcat (default target/catalogs)
";

/// The byte-order mark every card starts with in the catalog, as the
/// resource DLLs hold it. The sources leave it out.
const CARD_BOM: [u8; 2] = [0xFF, 0xFE];

/// The modules under `source`: every `<name>.toml`, sorted by name.
pub fn modules(source: &Path) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(source)
        .map(|dir| {
            dir.flatten()
                .filter_map(|entry| entry.file_name().into_string().ok())
                .filter_map(|file| file.strip_suffix(".toml").map(str::to_owned))
                .filter(|name| !name.is_empty())
                .collect()
        })
        .unwrap_or_default();
    names.sort();
    names
}

/// A key of `[strings]` or `[cards]`: a 16-bit ID in canonical decimal, so
/// that `7` and `07` cannot name the same entry twice.
fn parse_id(table: &str, key: &str) -> Result<u16, String> {
    key.parse::<u16>()
        .ok()
        .filter(|id| id.to_string() == key)
        .ok_or_else(|| format!("[{table}] key {key:?} is not a 16-bit ID in decimal"))
}

/// The entries of one of the module's tables, in ID order.
fn entries<'a>(module: &'a Table, table: &str) -> Result<Vec<(u16, &'a str)>, String> {
    let Some(value) = module.get(table) else {
        return Ok(Vec::new());
    };
    let Value::Table(entries) = value else {
        return Err(format!("[{table}] is not a table"));
    };
    let mut out = Vec::with_capacity(entries.len());
    for (key, value) in entries {
        let id = parse_id(table, key)?;
        let Value::String(text) = value else {
            return Err(format!("[{table}] {key} is not a string"));
        };
        out.push((id, text.as_str()));
    }
    out.sort_by_key(|entry| entry.0);
    Ok(out)
}

/// Builds a catalog from the text of a module's source.
///
/// The source holds a `[strings]` table and a `[cards]` table, each keyed
/// by the 16-bit ID inside the module. A string becomes its UTF-16 code
/// units; an empty string is an entry, because it makes its string block
/// exist. A card is `<RDL>` markup and becomes UTF-16LE bytes after a
/// byte-order mark, as the resource DLLs hold it.
pub fn build_from_str(text: &str) -> Result<TextCatalog, String> {
    let module: Table = text.parse().map_err(|e: toml::de::Error| e.to_string())?;
    if let Some(other) = module
        .keys()
        .find(|key| !matches!(key.as_str(), "strings" | "cards"))
    {
        return Err(format!("unknown table [{other}]"));
    }
    let mut catalog = TextCatalog::new();
    for (id, text) in entries(&module, "strings")? {
        catalog
            .add_string(id, text.encode_utf16().collect())
            .map_err(|e| format!("[strings] {id}: {e}"))?;
    }
    for (id, text) in entries(&module, "cards")? {
        let mut bytes = CARD_BOM.to_vec();
        bytes.extend(text.encode_utf16().flat_map(u16::to_le_bytes));
        catalog
            .add_card(id, bytes)
            .map_err(|e| format!("[cards] {id}: {e}"))?;
    }
    Ok(catalog)
}

/// Builds the catalog of module `name` from `<source>/<name>.toml`.
pub fn build_module(source: &Path, name: &str) -> Result<TextCatalog, String> {
    let path = source.join(format!("{name}.toml"));
    let text = fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    build_from_str(&text).map_err(|e| format!("{}: {e}", path.display()))
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
        eprintln!("catalog: no <Module>.toml under {}", source.display());
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
    fn strings_and_cards_build_a_catalog() {
        let source = r#"# a module
[strings]
32 = "hex id"
12 = ""
5 = "tab\tnew\nline é"
[cards]
1 = '''<RDL><TEXT>One "quoted" it's</TEXT></RDL>'''
"#;
        let built = build_from_str(source).unwrap();
        assert_eq!((built.string_count(), built.card_count()), (3, 1));
        assert_eq!(built.string(32), Some(units("hex id").as_slice()));
        assert_eq!(built.string_entry(12), Some(&[][..]));
        assert_eq!(
            built.string(5),
            Some(units("tab\tnew\nline \u{e9}").as_slice())
        );
        let mut card = CARD_BOM.to_vec();
        card.extend(
            units(r#"<RDL><TEXT>One "quoted" it's</TEXT></RDL>"#)
                .iter()
                .flat_map(|u| u.to_le_bytes()),
        );
        assert_eq!(built.card(1), Some(card.as_slice()));
        let bytes = built.to_bytes().unwrap();
        assert_eq!(TextCatalog::from_bytes(&bytes).unwrap(), built);
        assert_eq!(build_from_str("").unwrap(), TextCatalog::new());
    }

    #[test]
    fn malformed_sources_are_rejected() {
        let cases = [
            ("[strings]\n7 = \"a\"\n07 = \"b\"\n", "not a 16-bit ID"),
            ("[strings]\n65536 = \"a\"\n", "not a 16-bit ID"),
            ("[strings]\nname = \"a\"\n", "not a 16-bit ID"),
            ("[strings]\n1 = 2\n", "not a string"),
            ("strings = 1\n", "not a table"),
            ("[other]\n1 = \"a\"\n", "unknown table"),
            ("[strings]\n1 = \"a\"\n1 = \"b\"\n", "duplicate"),
        ];
        for (text, needle) in cases {
            let error = build_from_str(text).unwrap_err();
            assert!(error.contains(needle), "{text:?}: {error}");
        }
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
        }
        let ui = build_module(&source, "UiText").unwrap();
        assert_eq!(ui.string(0), Some(units("Object Unknown").as_slice()));
        assert_eq!(ui.string(5), Some(units("\n").as_slice()));
    }
}
