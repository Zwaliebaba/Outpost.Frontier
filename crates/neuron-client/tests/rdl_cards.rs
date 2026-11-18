//! Every rich-text card of the game's text (`outdir/data/text/`, decision
//! T21) reads as RDLX markup, and the markup the writer makes of its nodes
//! reads back to the same nodes (P4.12). The string tables go through the
//! "markup, else plain text" rule of `LoadIdsIntoRdl`: only the entries
//! that hold `<RDL>` markup are taken as markup.

use std::fs;
use std::path::Path;

use neuron_client::text::rdl::{RenderDisplayList, load_ids, markup};
use neuron_core::catalog::{IdsText, TextCatalog, TextCatalogs};

/// The byte-order mark the catalog build puts before a card.
const CARD_BOM: [u8; 2] = [0xFF, 0xFE];

/// A module's catalog built from its TOML source as `cargo xtask catalog`
/// builds it.
fn catalog(text: &str) -> TextCatalog {
    let module: toml::Table = text.parse().unwrap();
    let mut catalog = TextCatalog::new();
    let table = |name: &str| {
        module
            .get(name)
            .and_then(toml::Value::as_table)
            .cloned()
            .unwrap_or_default()
    };
    for (id, value) in table("strings") {
        let units = value.as_str().unwrap().encode_utf16().collect();
        catalog.add_string(id.parse().unwrap(), units).unwrap();
    }
    for (id, value) in table("cards") {
        let mut bytes = CARD_BOM.to_vec();
        bytes.extend(
            value
                .as_str()
                .unwrap()
                .encode_utf16()
                .flat_map(u16::to_le_bytes),
        );
        catalog.add_card(id.parse().unwrap(), bytes).unwrap();
    }
    catalog
}

#[test]
fn every_card_reads_and_round_trips() {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data/text");
    let Ok(entries) = fs::read_dir(&dir) else {
        println!("{} is missing: no cards to read", dir.display());
        return;
    };
    let mut files: Vec<_> = entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|e| e == "toml"))
        .collect();
    files.sort();
    let mut catalogs = TextCatalogs::new();
    let mut ids = Vec::new();
    for file in &files {
        let catalog = catalog(&fs::read_to_string(file).unwrap());
        let cards: Vec<u16> = catalog.cards().map(|(id, _)| id).collect();
        let strings: Vec<u16> = catalog.strings().map(|(id, _)| id).collect();
        let base = catalogs.register(Some(catalog));
        ids.extend(cards.into_iter().map(|id| (base | u32::from(id), true)));
        ids.extend(strings.into_iter().map(|id| (base | u32::from(id), false)));
    }

    let (mut cards, mut same_text, mut markup_strings) = (0, 0, 0);
    for (ids, is_card) in ids {
        let text = catalogs.text(ids).unwrap();
        let units = text.units();
        let mut list = RenderDisplayList::new();
        let is_markup = markup::read(&mut list, &units);
        let source = String::from_utf16(&units).unwrap();
        if !is_card {
            // A card with the same ID hides a string from this lookup.
            if matches!(text, IdsText::String(_)) {
                assert_eq!(
                    is_markup,
                    source.starts_with("<RDL>"),
                    "{ids:#x}: {source:?}"
                );
                markup_strings += usize::from(is_markup);
            }
            continue;
        }
        assert!(is_markup, "{ids:#x}: {source}");
        assert!(!list.nodes.is_empty(), "{ids:#x}");
        cards += 1;
        let written = markup::write(&list).unwrap_or_else(|e| panic!("{ids:#x}: {e}"));
        let mut back = RenderDisplayList::new();
        assert!(markup::read_str(&mut back, &written), "{ids:#x}: {written}");
        assert_eq!(back, list, "{ids:#x}: {written}");
        same_text += usize::from(written == source);

        // The loader takes the same path.
        let mut loaded = RenderDisplayList::new();
        assert!(load_ids(&mut loaded, &catalogs, ids));
        assert_eq!(loaded, list, "{ids:#x}");
    }
    println!(
        "{cards} cards; {same_text} written back as their source text; {markup_strings} strings read as markup"
    );
    assert_eq!(cards, 5252);
    assert_eq!(same_text, cards);
}
