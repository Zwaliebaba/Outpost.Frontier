# Text resources

The sources of the game's text, one folder per module (work package P1.6,
decision T21). They are copies of the eight resource projects in
`CppSource/Resources/`; those copies go with the C++ in P7.3, and from then
on these are the only ones.

`cargo xtask catalog` builds one catalog per module from them, by default
into `target/catalogs/<Module>.tcat`. Catalogs are build output: they are
not committed and ship beside the executables. The catalog format and the
lookup rules are documented in `crates/neuron-core/src/catalog/mod.rs`.

## Layout

| Folder | Content | Index of its text IDs |
| --- | --- | --- |
| `UiText/` | `UiText.rc`: 3,648 strings | 0 |
| `FieldGuideText/` | `FieldGuideText.rc` and 886 cards | 1 |
| `DialogueText/` | `DialogueText.rc` and 3,087 cards | 2 |
| `NameCatalog/` | `NameCatalog.rc`: 7,139 strings | 3 |
| `EquipmentCatalog/` | `EquipmentCatalog.rc`: 784 strings, and 784 cards | 4 |
| `NegotiationText/` | `NegotiationText.rc`: 1,270 strings | 5 |
| `InterfaceExtras/` | `InterfaceExtras.rc` and 495 cards | 6 |
| `serverinterface/` | `serverinterface.rc`: 107 strings for the server console | none |

The index is the high 16 bits of a text ID (fact F3); the low 16 bits are
the ID inside the module. `serverinterface` is the server's resource module
and has no index.

- `<Module>.rc` is a resource script in UTF-8 without a byte-order mark,
  read as code page 65001 (the C++ projects pass `rc /c65001`). Its
  `STRINGTABLE` blocks hold the strings. A line `56 23 "HTML56_1.txt"`
  makes the file `HTML56_1.txt` resource 56 of type 23 (`RT_HTML`).
- `HTML<id>_1.txt` is a rich-text card: UTF-16LE text with a byte-order
  mark, `<RDL>` markup. The catalog stores its bytes unchanged.
- The dialogs, menus, accelerators and version information in the scripts
  are skipped by the catalog build. The bitmaps and icons that
  `serverinterface.rc` names were not copied: the server's dialogs are
  dropped (library ledger row 39).

Until P7.3 the CI `oracle` job builds the resource DLLs from
`CppSource/Resources/`, dumps them with `Oracle.exe catalog` and compares
the dump with these catalogs (`crates/neuron-core/tests/catalog_dump.rs`),
so the two copies must stay identical. The `.gitattributes` entry keeps
the bytes of these files as they are.
