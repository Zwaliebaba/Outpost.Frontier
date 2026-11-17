# Text resources

The sources of the game's text, one TOML file per module (work package
P1.6, decision T21). They live in the game data tree, `outdir/data/text/`,
since 2026-11-17 (they were in `resources/text/` before). `cargo xtask catalog` builds one catalog per module
from them, by default into `target/catalogs/<Module>.tcat`. Catalogs are
build output: they are not committed and ship beside the executables. The
catalog format and the lookup rules are documented in
`crates/neuron-core/src/catalog/mod.rs`.

The files were generated on 2026-11-17 from the eight resource projects in
`CppSource/Resources/` (string tables in `.rc` scripts, rich-text cards in
`HTML<id>_1.txt` files). The catalogs built from them are byte-identical
to the ones the `.rc` build made before. The C++ copies go with the C++ in
P7.3; from then on these files are the only source.

## Layout

| File | Content | Index of its text IDs |
| --- | --- | --- |
| `UiText.toml` | 3,648 strings | 0 |
| `FieldGuideText.toml` | 886 cards | 1 |
| `DialogueText.toml` | 3,087 cards | 2 |
| `NameCatalog.toml` | 7,139 strings | 3 |
| `EquipmentCatalog.toml` | 784 strings and 784 cards | 4 |
| `NegotiationText.toml` | 1,270 strings | 5 |
| `InterfaceExtras.toml` | 495 cards | 6 |
| `serverinterface.toml` | 107 strings for the server console | none |

The index is the high 16 bits of a text ID (fact F3); the low 16 bits are
the key in the file. `serverinterface` is the server's resource module and
has no index.

## Format

```toml
[strings]
0 = "Object Unknown"
5 = "\n"
12 = ""

[cards]
1000 = '''<RDL><PUSH/><TEXT>All the Fertilizer produced here…</TEXT>…</RDL>'''
```

- Keys are the 16-bit IDs inside the module, in decimal without leading
  zeros. An ID appears once per table.
- A string is any text. An empty string is a real entry: it makes its
  string block exist, which changes what the game's lookup of a missing
  neighbour returns.
- A card is `<RDL>` rich-text markup, kept as text in a literal string so
  its quotes need no escaping; it must not contain `'''`. The catalog
  build stores it as UTF-16LE after a byte-order mark, as the resource
  DLLs hold it.
- Other tables are rejected.

## Keeping the two copies in step

Until P7.3 the C++ reference still builds its resource DLLs from
`CppSource/Resources/`. CI's `oracle` job dumps those DLLs with
`Oracle.exe catalog` and compares the dump with the catalogs built from
these files (`crates/neuron-core/tests/catalog_dump.rs`). A text change
made here must be made there too, or that job fails.
