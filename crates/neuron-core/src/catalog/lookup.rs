//! The module table and the lookups of 32-bit text IDs.

use std::fmt;
use std::path::Path;

use super::file::{CatalogReadError, EXTENSION, TextCatalog};

/// The client's modules in the order of their index (fact F3): `UiText`,
/// which `WinMain` registers first, then the `[Resources]` DLLs of the
/// client INI in their order.
pub const CLIENT_MODULES: [&str; 7] = [
    "UiText",
    "FieldGuideText",
    "DialogueText",
    "NameCatalog",
    "EquipmentCatalog",
    "NegotiationText",
    "InterfaceExtras",
];

/// The module of the dedicated server's console strings. The C++ server
/// makes `ServerInterface.dll` its MFC resource handle; it has no index in
/// the text-ID table.
pub const SERVER_INTERFACE_MODULE: &str = "serverinterface";

/// The file name of a module's catalog: `<module>.tcat`.
pub fn catalog_file_name(module: &str) -> String {
    format!("{module}.{EXTENSION}")
}

/// The module name of a `[Resources]` DLL value: the name without a
/// trailing `.dll` in any case.
pub fn module_name_from_dll(dll: &str) -> &str {
    let split = dll.len().saturating_sub(4);
    match (dll.get(..split), dll.get(split..)) {
        (Some(stem), Some(ext)) if ext.eq_ignore_ascii_case(".dll") => stem,
        _ => dll,
    }
}

/// A module whose catalog did not load. It keeps its index.
#[derive(Debug)]
pub struct ModuleLoadFailure {
    /// The index the module was registered at.
    pub index: usize,
    /// The module name.
    pub module: String,
    /// Why it did not load.
    pub error: CatalogReadError,
}

impl fmt::Display for ModuleLoadFailure {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "text catalog {} (index {}): {}",
            self.module, self.index, self.error
        )
    }
}

/// The text resource a [`TextCatalogs::text`] lookup finds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IdsText<'a> {
    /// A rich-text card: the UTF-16LE bytes of its text, after a leading
    /// byte-order mark and without an odd last byte.
    Card(&'a [u8]),
    /// A string-table entry; empty when the ID is not defined but its
    /// block exists.
    String(&'a [u16]),
}

impl IdsText<'_> {
    /// The length in UTF-16 units.
    pub fn len(&self) -> usize {
        match self {
            Self::Card(bytes) => bytes.len() / 2,
            Self::String(units) => units.len(),
        }
    }

    /// Whether the text is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The UTF-16 units of the text.
    pub fn units(&self) -> Vec<u16> {
        match self {
            Self::Card(bytes) => bytes
                .as_chunks::<2>()
                .0
                .iter()
                .map(|&pair| u16::from_le_bytes(pair))
                .collect(),
            Self::String(units) => units.to_vec(),
        }
    }
}

/// The text-ID module table (`GIdsModules` of `ids_strings.cpp`,
/// `GResourceDlls` of the server's `ResourceStrings.cpp`). The high 16 bits
/// of a text ID index it; the low 16 bits are the module's local ID.
#[derive(Clone, Debug, Default)]
pub struct TextCatalogs {
    modules: Vec<Option<TextCatalog>>,
}

impl TextCatalogs {
    /// An empty table.
    pub fn new() -> Self {
        Self::default()
    }

    /// Appends a module and returns its ID base, the index shifted into the
    /// high 16 bits (`RegisterIdsResourceModule`, `AddResourceDll`). `None`
    /// is a module that failed to load: it keeps its index, and every
    /// lookup in it misses.
    pub fn register(&mut self, module: Option<TextCatalog>) -> u32 {
        let index = u32::try_from(self.modules.len()).unwrap_or(u32::MAX);
        self.modules.push(module);
        index << 16
    }

    /// Loads `<dir>/<module>.tcat` for each name, in order. A catalog that
    /// cannot be read is registered as missing and reported.
    pub fn load<S: AsRef<str>>(dir: &Path, modules: &[S]) -> (Self, Vec<ModuleLoadFailure>) {
        let mut catalogs = Self::new();
        let mut failures = Vec::new();
        for module in modules {
            let module = module.as_ref();
            let index = catalogs.modules.len();
            match TextCatalog::read(&dir.join(catalog_file_name(module))) {
                Ok(catalog) => {
                    catalogs.register(Some(catalog));
                }
                Err(error) => {
                    catalogs.register(None);
                    failures.push(ModuleLoadFailure {
                        index,
                        module: module.to_owned(),
                        error,
                    });
                }
            }
        }
        (catalogs, failures)
    }

    /// Loads the client's modules in the order of [`CLIENT_MODULES`].
    pub fn load_client(dir: &Path) -> (Self, Vec<ModuleLoadFailure>) {
        Self::load(dir, &CLIENT_MODULES)
    }

    /// The number of registered modules, missing ones included.
    pub fn len(&self) -> usize {
        self.modules.len()
    }

    /// Whether no module is registered.
    pub fn is_empty(&self) -> bool {
        self.modules.is_empty()
    }

    /// The module registered at `index`, `None` when it is missing or the
    /// index is past the table.
    pub fn module_at(&self, index: usize) -> Option<&TextCatalog> {
        self.modules.get(index).and_then(Option::as_ref)
    }

    /// The module the high 16 bits of `ids` name (`GetIdsResourceModule`).
    pub fn module(&self, ids: u32) -> Option<&TextCatalog> {
        self.resolve(ids).map(|(module, _)| module)
    }

    /// `TakeIdsModule`: the module and the local ID. For an index past the
    /// table, or a module that failed to load, the C++ hands a null module
    /// to `FindResourceExA`, which searches the executable instead; neither
    /// executable has a string table or a type-23 resource, so that lookup
    /// misses, as `None` does here.
    fn resolve(&self, ids: u32) -> Option<(&TextCatalog, u16)> {
        let index = usize::try_from(ids >> 16).ok()?;
        let module = self.module_at(index)?;
        Some((module, low_word(ids)))
    }

    /// The string-table entry for `ids`; empty when it is missing.
    pub fn string(&self, ids: u32) -> &[u16] {
        self.resolve(ids)
            .and_then(|(module, id)| module.string_entry(id))
            .unwrap_or_default()
    }

    /// `GetIdsStringW` (client) and `LoadIdsString` (server) with a null
    /// buffer: the length of the entry, 0 when it is missing.
    pub fn string_len(&self, ids: u32) -> usize {
        self.string(ids).len()
    }

    /// `GetIdsStringW` (client) and `LoadIdsString` (server) into `dest`,
    /// whose length is the buffer size in characters. Copies at most
    /// `dest.len() - 1` units as `wcsncpy` does (a NUL in the entry ends
    /// the copy and the rest of the count is zero-filled), terminates the
    /// text and returns the count. An empty `dest` returns 0 untouched; a
    /// missing entry writes an empty text and returns 0.
    pub fn string_into(&self, ids: u32, dest: &mut [u16]) -> usize {
        let room = buffer_chars(dest);
        if room < 1 {
            return 0;
        }
        let Some(entry) = self
            .resolve(ids)
            .and_then(|(module, id)| module.string_entry(id))
        else {
            dest[0] = 0;
            return 0;
        };
        let count = (room - 1).min(entry.len());
        let copied = entry[..count]
            .iter()
            .position(|&unit| unit == 0)
            .unwrap_or(count);
        dest[..copied].copy_from_slice(&entry[..copied]);
        dest[copied..=count].fill(0);
        count
    }

    /// `GetIdsStringWWithRawFallback`: [`TextCatalogs::string_into`], and
    /// when that copies nothing, the card with the same local ID from its
    /// second UTF-16 unit on, whatever the first unit is. Copies at most
    /// `dest.len() - 1` units, terminates the text and returns the count.
    ///
    /// The C++ writes past the buffer for a zero-length `dest` and reads
    /// past the resource for a card shorter than two bytes; here the first
    /// returns 0 and the second copies nothing (divergence register).
    pub fn string_with_card_fallback_into(&self, ids: u32, dest: &mut [u16]) -> usize {
        let copied = self.string_into(ids, dest);
        if copied != 0 {
            return copied;
        }
        let Some(card) = self.resolve(ids).and_then(|(module, id)| module.card(id)) else {
            return 0;
        };
        let room = buffer_chars(dest);
        if room < 1 {
            return 0;
        }
        let words = card.len() / 2;
        let count = words.saturating_sub(1).min(room - 1);
        for (slot, pair) in dest
            .iter_mut()
            .zip(card.as_chunks::<2>().0.iter().skip(1))
            .take(count)
        {
            *slot = u16::from_le_bytes(*pair);
        }
        dest[count] = 0;
        count
    }

    /// The resource `LoadIdsIntoRdl` reads for `ids`: the card with the
    /// local ID if there is one, otherwise the string-table entry. `None`
    /// when the module is missing or neither exists; an undefined ID in an
    /// existing string block is an empty string, not `None`.
    pub fn text(&self, ids: u32) -> Option<IdsText<'_>> {
        let (module, id) = self.resolve(ids)?;
        if let Some(card) = module.card(id) {
            let words = card.len() / 2;
            let skip = usize::from(words != 0 && card[0] == 0xFF && card[1] == 0xFE);
            return Some(IdsText::Card(&card[skip * 2..words * 2]));
        }
        module.string_entry(id).map(IdsText::String)
    }
}

fn low_word(ids: u32) -> u16 {
    u16::try_from(ids & 0xFFFF).unwrap_or_default()
}

/// The buffer size in characters as the C++ `int` sees it.
fn buffer_chars(dest: &[u16]) -> usize {
    dest.len().min(i32::MAX as usize)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn units(text: &str) -> Vec<u16> {
        text.encode_utf16().collect()
    }

    fn card(text: &str, bom: bool) -> Vec<u8> {
        let mut bytes = Vec::new();
        if bom {
            bytes.extend_from_slice(&[0xFF, 0xFE]);
        }
        bytes.extend(text.encode_utf16().flat_map(u16::to_le_bytes));
        bytes
    }

    /// Module 0: strings 1 "One", 2 "", 5 "A\0B", 0x20 "Hi". Module 1
    /// fails to load. Module 2: card 3 with a BOM, string 3 "S", card 4
    /// without a BOM, card 9 of one byte, string 0x40 "", card 0x40.
    fn table() -> TextCatalogs {
        let mut zero = TextCatalog::new();
        zero.add_string(1, units("One")).unwrap();
        zero.add_string(2, units("")).unwrap();
        zero.add_string(5, vec![0x41, 0, 0x42]).unwrap();
        zero.add_string(0x20, units("Hi")).unwrap();
        let mut two = TextCatalog::new();
        two.add_card(3, card("<RDL/>", true)).unwrap();
        two.add_string(3, units("S")).unwrap();
        two.add_card(4, card("xyz", false)).unwrap();
        two.add_card(9, vec![0x41]).unwrap();
        two.add_string(0x40, units("")).unwrap();
        two.add_card(0x40, card("C", true)).unwrap();
        let mut catalogs = TextCatalogs::new();
        assert_eq!(catalogs.register(Some(zero)), 0);
        assert_eq!(catalogs.register(None), 0x1_0000);
        assert_eq!(catalogs.register(Some(two)), 0x2_0000);
        catalogs
    }

    #[test]
    fn module_index_is_the_high_word() {
        let catalogs = table();
        assert_eq!(catalogs.len(), 3);
        assert!(catalogs.module(0x0000_1234).is_some());
        assert!(catalogs.module(0x0001_0001).is_none());
        assert!(catalogs.module(0x0002_0000).is_some());
        assert!(catalogs.module(0x0003_0001).is_none());
        assert_eq!(catalogs.string(0x0000_0001), units("One"));
        // Out of range: the C++ keeps the high word and searches the
        // executable, which has no strings.
        assert_eq!(catalogs.string(0x0003_0001), &[] as &[u16]);
        assert_eq!(catalogs.string(0x0001_0001), &[] as &[u16]);
    }

    #[test]
    fn string_into_truncates_like_the_cpp() {
        let catalogs = table();
        let mut buf = [0xAAAA_u16; 8];
        assert_eq!(catalogs.string_into(1, &mut buf), 3);
        assert_eq!(&buf[..4], &[0x4F, 0x6E, 0x65, 0]);
        let mut buf = [0xAAAA_u16; 3];
        assert_eq!(catalogs.string_into(1, &mut buf), 2);
        assert_eq!(buf, [0x4F, 0x6E, 0]);
        let mut buf = [0xAAAA_u16; 1];
        assert_eq!(catalogs.string_into(1, &mut buf), 0);
        assert_eq!(buf, [0]);
        let mut empty: [u16; 0] = [];
        assert_eq!(catalogs.string_into(1, &mut empty), 0);
        // Missing block and missing module write an empty text.
        let mut buf = [0xAAAA_u16; 4];
        assert_eq!(catalogs.string_into(0x10, &mut buf), 0);
        assert_eq!(buf[0], 0);
        assert_eq!(buf[1], 0xAAAA);
        buf[0] = 0xAAAA;
        assert_eq!(catalogs.string_into(0x1_0001, &mut buf), 0);
        assert_eq!(buf[0], 0);
        // An embedded NUL ends the copy; the count is still the length.
        let mut buf = [0xAAAA_u16; 8];
        assert_eq!(catalogs.string_into(5, &mut buf), 3);
        assert_eq!(&buf[..5], &[0x41, 0, 0, 0, 0xAAAA]);
        assert_eq!(catalogs.string_len(5), 3);
        assert_eq!(catalogs.string_len(2), 0);
        assert_eq!(catalogs.string_len(0x10), 0);
    }

    #[test]
    fn card_fallback_skips_the_first_unit() {
        let catalogs = table();
        let mut buf = [0xAAAA_u16; 16];
        // String 3 exists: no fallback.
        assert_eq!(
            catalogs.string_with_card_fallback_into(0x2_0003, &mut buf),
            1
        );
        assert_eq!(&buf[..2], &[0x53, 0]);
        // Empty string 0x40 falls back to card 0x40 after its BOM.
        assert_eq!(
            catalogs.string_with_card_fallback_into(0x2_0040, &mut buf),
            1
        );
        assert_eq!(&buf[..2], &[0x43, 0]);
        // Card 4 has no BOM: its first character is skipped all the same.
        assert_eq!(
            catalogs.string_with_card_fallback_into(0x2_0004, &mut buf),
            2
        );
        assert_eq!(&buf[..3], &[0x79, 0x7A, 0]);
        // Truncated to the buffer.
        let mut small = [0xAAAA_u16; 2];
        assert_eq!(
            catalogs.string_with_card_fallback_into(0x2_0004, &mut small),
            1
        );
        assert_eq!(small, [0x79, 0]);
        // A one-byte card copies nothing.
        assert_eq!(
            catalogs.string_with_card_fallback_into(0x2_0009, &mut buf),
            0
        );
        assert_eq!(buf[0], 0);
        // Missing everywhere.
        assert_eq!(
            catalogs.string_with_card_fallback_into(0x2_0100, &mut buf),
            0
        );
        assert_eq!(
            catalogs.string_with_card_fallback_into(0x1_0004, &mut buf),
            0
        );
        let mut empty: [u16; 0] = [];
        assert_eq!(
            catalogs.string_with_card_fallback_into(0x2_0004, &mut empty),
            0
        );
    }

    #[test]
    fn text_prefers_cards_and_strips_the_bom() {
        let catalogs = table();
        let text = catalogs.text(0x2_0003).unwrap();
        assert_eq!(text, IdsText::Card(&card("<RDL/>", false)));
        assert_eq!(text.units(), units("<RDL/>"));
        assert_eq!(text.len(), 6);
        assert_eq!(
            catalogs.text(0x2_0004),
            Some(IdsText::Card(&card("xyz", false)[..]))
        );
        assert_eq!(catalogs.text(0x2_0009), Some(IdsText::Card(&[][..])));
        assert_eq!(catalogs.text(1), Some(IdsText::String(&units("One"))));
        // Undefined ID in an existing block: an empty string.
        assert_eq!(catalogs.text(3), Some(IdsText::String(&[])));
        assert!(catalogs.text(3).unwrap().is_empty());
        assert_eq!(catalogs.text(0x10), None);
        assert_eq!(catalogs.text(0x1_0001), None);
        assert_eq!(catalogs.text(0x7_0001), None);
    }

    #[test]
    fn dll_names() {
        assert_eq!(module_name_from_dll("FieldGuideText.dll"), "FieldGuideText");
        assert_eq!(module_name_from_dll("UiText.DLL"), "UiText");
        assert_eq!(module_name_from_dll("dll"), "dll");
        assert_eq!(module_name_from_dll("Name"), "Name");
        assert_eq!(catalog_file_name("UiText"), "UiText.tcat");
    }

    #[test]
    fn load_keeps_the_index_of_a_missing_module() {
        let dir = std::env::temp_dir().join(format!(
            "neuron-core-catalog-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let mut a = TextCatalog::new();
        a.add_string(7, units("A")).unwrap();
        let mut c = TextCatalog::new();
        c.add_string(7, units("C")).unwrap();
        std::fs::write(dir.join("A.tcat"), a.to_bytes().unwrap()).unwrap();
        std::fs::write(dir.join("C.tcat"), c.to_bytes().unwrap()).unwrap();
        std::fs::write(dir.join("Bad.tcat"), b"junk").unwrap();
        let (catalogs, failures) = TextCatalogs::load(&dir, &["A", "B", "Bad", "C"]);
        std::fs::remove_dir_all(&dir).unwrap();
        assert_eq!(catalogs.len(), 4);
        assert_eq!(catalogs.string(7), units("A"));
        assert_eq!(catalogs.string(0x3_0007), units("C"));
        assert_eq!(catalogs.string(0x1_0007), &[] as &[u16]);
        let failed: Vec<(usize, &str)> = failures
            .iter()
            .map(|f| (f.index, f.module.as_str()))
            .collect();
        assert_eq!(failed, [(1, "B"), (2, "Bad")]);
        assert!(matches!(failures[0].error, CatalogReadError::Io(_)));
        assert!(matches!(failures[1].error, CatalogReadError::Format(_)));
    }
}
