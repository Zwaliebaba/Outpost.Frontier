use std::path::{Path, PathBuf};

use super::*;

fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/rc")
}

fn options() -> RcOptions {
    RcOptions {
        code_page: CodePage::Utf8,
        include_dirs: Vec::new(),
    }
}

fn units(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

/// Parses `text` as a script in a fresh folder.
fn parse_text(name: &str, text: &[u8]) -> Result<RcScript, RcError> {
    let dir = std::env::temp_dir().join(format!("xtask-rc-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("t.rc");
    std::fs::write(&path, text).unwrap();
    let result = parse_file(&path, &options());
    std::fs::remove_dir_all(&dir).unwrap();
    result
}

#[test]
fn fixture_strings() {
    let script = parse_file(&fixtures().join("main.rc"), &options()).unwrap();
    let got: Vec<(u16, u16, Vec<u16>)> = script
        .strings
        .iter()
        .map(|s| (s.id, s.language, s.units.clone()))
        .collect();
    let mut escapes = units("a\nb\tc\\d");
    escapes.push(0x08);
    escapes.extend(units("AA\\q"));
    let want: Vec<(u16, u16, Vec<u16>)> = vec![
        (0, 0x0409, units("Object Unknown")),
        (1, 0x0409, units("no comma")),
        (3, 0x0409, units("from #define")),
        (4, 0x0409, units("expression")),
        (5, 0x0409, units("say \"hi\" and \"quote\"")),
        (6, 0x0409, escapes),
        (7, 0x0409, units("split in three\u{263a}")),
        (8, 0x0409, units("caf\u{e9} \u{2019} \u{2014} \u{fc}")),
        (9, 0x0409, units("url http://example.com/*x*/")),
        (10, 0x0409, units("continued line")),
        (11, 0x0409, units("")),
        (0x20, 0x0409, units("hex id")),
        (0x21, 0x0409, units("wide  dropped")),
        (41, 0x0409, units("else branch")),
        (50, 0x0409, units("\u{2019}")),
    ];
    assert_eq!(got, want);
    // Line numbers survive the splice in string 10.
    let lines: Vec<(u16, u32)> = script.strings.iter().map(|s| (s.id, s.loc.line)).collect();
    assert!(lines.contains(&(10, 57)), "{lines:?}");
    assert!(lines.contains(&(11, 59)), "{lines:?}");
    assert!(lines.contains(&(0x20, 60)), "{lines:?}");
}

#[test]
fn fixture_cards() {
    let script = parse_file(&fixtures().join("main.rc"), &options()).unwrap();
    let got: Vec<(u16, u16, PathBuf)> = script
        .cards
        .iter()
        .map(|c| (c.id, c.language, c.path.clone()))
        .collect();
    assert_eq!(
        got,
        vec![
            (1, 0, fixtures().join("card1.txt")),
            (2, 0, fixtures().join("sub").join("card2.txt")),
        ]
    );
}

#[test]
fn include_dirs_are_searched() {
    let dir = std::env::temp_dir().join(format!("xtask-rc-{}-include", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("t.rc");
    std::fs::write(
        &path,
        "#include \"ids.h\"\n9 23 \"card1.txt\"\nSTRINGTABLE { IDS_BASE, \"x\" }\n",
    )
    .unwrap();
    let script = parse_file(
        &path,
        &RcOptions {
            code_page: CodePage::Utf8,
            include_dirs: vec![fixtures()],
        },
    );
    std::fs::remove_dir_all(&dir).unwrap();
    let script = script.unwrap();
    assert_eq!(script.cards[0].path, fixtures().join("card1.txt"));
    assert_eq!(script.strings[0].id, 3);
}

#[test]
fn utf16_scripts_are_read() {
    let utf16 = |text: &str| -> Vec<u8> {
        let mut bytes = vec![0xFF, 0xFE];
        bytes.extend(text.encode_utf16().flat_map(u16::to_le_bytes));
        bytes
    };
    let script = parse_text("utf16", &utf16("STRINGTABLE { 1, \"\u{2019}\" }\n")).unwrap();
    assert_eq!(script.strings[0].units, units("\u{2019}"));
    let error = parse_text("utf16pragma", &utf16("#pragma code_page(1252)\n")).unwrap_err();
    assert!(error.message.contains("UTF-16"), "{error}");
}

#[test]
fn errors_name_the_line() {
    let cases: &[(&str, &str, u32, &str)] = &[
        ("nostr", "STRINGTABLE\n{\n 1\n}\n", 3, "expected a string"),
        ("range", "STRINGTABLE { 70000, \"x\" }", 1, "outside"),
        ("inline", "\n1 23 { \"inline\" }", 2, "inline"),
        ("missing", "1 23 \"nope.txt\"", 1, "not found"),
        ("if", "#if 1\n#endif\n", 1, "#if"),
        ("fnmacro", "#define F(x) x\n", 1, "function-like"),
        ("zero", "STRINGTABLE { 010, \"x\" }", 1, "leading zero"),
        ("byte", "STRINGTABLE { 1, \"\\xe9\" }", 1, "UTF-8"),
        ("cp", "#pragma code_page(932)\n", 1, "code_page"),
        ("eof", "STRINGTABLE { 1, \"x\" ", 1, "without its end"),
        (
            "symbol",
            "STRINGTABLE { IDS_NOPE, \"x\" }",
            1,
            "unknown symbol",
        ),
        ("endif", "#ifdef X\n", 1, "#endif"),
        ("quote", "STRINGTABLE { 1, \"a\\\"b\" }", 1, "string"),
        ("utf8", "STRINGTABLE { 1, \"\u{e9}\" }\n", 0, ""),
    ];
    for &(name, text, line, needle) in cases {
        let result = parse_text(name, text.as_bytes());
        if needle.is_empty() {
            assert!(result.is_ok(), "{name}: {result:?}");
            continue;
        }
        let error = result.expect_err(name);
        assert_eq!(error.line, line, "{name}: {error}");
        assert!(error.message.contains(needle), "{name}: {error}");
    }
    let error = parse_text("badutf8", b"STRINGTABLE { 1, \"\x92\" }").unwrap_err();
    assert!(error.message.contains("UTF-8"), "{error}");
}

#[test]
fn defaults_and_language_options() {
    let script = parse_text(
        "lang",
        b"STRINGTABLE { 1, \"a\" }\nSTRINGTABLE LANGUAGE 7, 1 { 2, \"b\" }\nLANGUAGE 0, 0\nSTRINGTABLE { 3, \"c\" }\n",
    )
    .unwrap();
    let langs: Vec<(u16, u16)> = script.strings.iter().map(|s| (s.id, s.language)).collect();
    assert_eq!(langs, [(1, 0x0409), (2, 0x0407), (3, 0)]);
}
