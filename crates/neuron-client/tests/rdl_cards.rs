//! Every rich-text card of the game's text (`outdir/data/text/`, decision
//! T21) reads as RDLX markup, and the markup the writer makes of its nodes
//! reads back to the same nodes (P4.12). The string tables go through the
//! "markup, else plain text" rule of `LoadIdsIntoRdl`: only the entries
//! that hold `<RDL>` markup are taken as markup.
//!
//! Every card also lays out (`LayoutRichText`) with the styles of
//! `rich_fonts.ini`, against the fake metrics of the rich-text oracle suite
//! (`support::rich_text`), wrapped at three widths and on one line; the
//! laid list keeps the card's text in order and executes.

mod support;

use std::fs;
use std::path::Path;

use neuron_client::text::layout::list::LayoutParams;
use neuron_client::text::layout::style::{apply_rich_styles, style_id};
use neuron_client::text::layout::{StyleCollection, TextRenderContext, layout_rich_text};
use neuron_client::text::rdl::{RdlNode, RenderDisplayList, TextAttributes, load_ids, markup};
use neuron_core::catalog::{IdsText, TextCatalog, TextCatalogs};

use support::rich_text::RecordingTrc;

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

/// Every module of `outdir/data/text` registered, and the IDs of their
/// cards (true) and strings (false); `None` when the folder is missing.
fn load_text() -> Option<(TextCatalogs, Vec<(u32, bool)>)> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data/text");
    let Ok(entries) = fs::read_dir(&dir) else {
        println!("{} is missing: no cards to read", dir.display());
        return None;
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
    Some((catalogs, ids))
}

/// The text units a list shows, in order: its runs, with styles and
/// referenced lists in their places.
fn shown_text(list: &RenderDisplayList, styles: &StyleCollection, out: &mut Vec<u16>) {
    for node in &list.nodes {
        match node {
            RdlNode::Text(t) | RdlNode::TextPtr(t) => out.extend_from_slice(t),
            RdlNode::Style(id) => shown_text(styles.get(*id), styles, out),
            RdlNode::Ref(inner) => shown_text(inner, styles, out),
            _ => {}
        }
    }
}

#[test]
fn every_card_lays_out() {
    let Some((catalogs, ids)) = load_text() else {
        return;
    };
    let ini = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data/FONTS/rich_fonts.ini");
    let mut styles = StyleCollection::new();
    apply_rich_styles(&fs::read(&ini).unwrap(), &mut styles);
    let style_count = styles.ids().count();
    assert_eq!(
        styles.get(style_id(b"STYLE_DIALOG")).nodes,
        [RdlNode::Attributes(TextAttributes::new(
            0xE0C3_8708,
            0xFFFF_FFFF,
            0
        ))]
    );

    let (mut cards, mut laid_nodes, mut lines, mut broken, mut cut) = (0, 0, 0, 0, 0);
    for (ids, is_card) in ids {
        if !is_card {
            continue;
        }
        let mut source = RenderDisplayList::new();
        assert!(load_ids(&mut source, &catalogs, ids));
        let mut want = Vec::new();
        shown_text(&source, &styles, &mut want);
        cards += 1;
        for (width, single_line) in [(400, false), (180, false), (60, false), (180, true)] {
            let mut ctx = RecordingTrc::default();
            ctx.begin(0xFFFF_FF00);
            let mut laid = RenderDisplayList::new();
            let result = layout_rich_text(
                &mut ctx,
                &styles,
                &source,
                &mut laid,
                LayoutParams {
                    width,
                    origin_h: 2,
                    origin_v: 0,
                    single_line,
                    wrap: true,
                },
            );
            assert!(result.height >= 0, "{ids:#x} at {width}");
            assert!(ctx.trc().state_stack.is_empty(), "{ids:#x}");
            let mut got = Vec::new();
            shown_text(&laid, &styles, &mut got);
            if single_line {
                // One line, cut with a "." marker when it was broken.
                if result.broken {
                    assert_eq!(got.last(), Some(&0x2E), "{ids:#x}");
                    cut += 1;
                }
            } else {
                assert_eq!(got, want, "{ids:#x} at {width}");
                broken += usize::from(result.broken);
                lines += laid
                    .nodes
                    .iter()
                    .filter(|n| matches!(n, RdlNode::Position(_)))
                    .count();
            }
            laid_nodes += laid.nodes.len();
            ctx.reset_to_default();
            ctx.set_pos(0, 0);
            laid.execute(&mut ctx, &styles, true);
        }
    }
    println!(
        "{cards} cards laid out at three widths and on one line with {style_count} styles: \
         {laid_nodes} laid nodes, {lines} placed runs, {broken} wrapped layouts, {cut} cut lines"
    );
    assert_eq!(cards, 5252);
}

#[test]
fn every_card_reads_and_round_trips() {
    let Some((catalogs, ids)) = load_text() else {
        return;
    };
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
