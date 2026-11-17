//! Text catalogs (work package P1.6, decision T21): the string tables and
//! rich-text cards of the eight C++ resource DLLs, looked up by numeric ID.
//!
//! `cargo xtask catalog` builds one catalog file per module from the
//! resource scripts in `resources/text/` (see its README). At run time
//! [`TextCatalogs`] registers the modules in the order the C++ loads the
//! DLLs, so every 32-bit text ID keeps its value: the high 16 bits are the
//! module index, the low 16 bits the module's local ID. For the client
//! that order is [`CLIENT_MODULES`] (fact F3).
//!
//! # Lookup semantics
//!
//! Ported from `CppSource/OutpostFrontier/Client/ids_strings.cpp`,
//! `ids_rdl.cpp`, `dataconfig_progress.cpp` and
//! `CppSource/OutpostServer/ResourceStrings.cpp`:
//!
//! - A module that fails to load keeps its index; every lookup in it
//!   misses ([`TextCatalogs::register`] with `None`).
//! - String-table lookups (`GetIdsStringW`, the server's `LoadIdsString`)
//!   read block `(id >> 4) + 1`, entry `id & 0xF` of `RT_STRING`. A block
//!   exists when any of its 16 IDs is defined; an undefined ID in an
//!   existing block is an empty string. The copy is truncated to the
//!   buffer ([`TextCatalogs::string_into`]).
//! - `GetIdsStringWWithRawFallback` falls back to the card with the same
//!   local ID, copied from its second UTF-16 unit
//!   ([`TextCatalogs::string_with_card_fallback_into`]).
//! - `LoadIdsIntoRdl` prefers the card and skips its byte-order mark, and
//!   otherwise takes the string-table entry ([`TextCatalogs::text`]).
//!
//! Strings stay UTF-16 (decision T15): the C++ hands them over as wide
//! strings. Cards stay the bytes of the resource, a UTF-16LE text that
//! starts with a byte-order mark.
//!
//! # File format, version 1
//!
//! All integers are little-endian.
//!
//! | Offset | Width | Field |
//! | --- | --- | --- |
//! | 0 | 4 bytes | magic `OFTC` |
//! | 4 | u16 | version, 1 |
//! | 6 | u16 | reserved, 0 |
//! | 8 | u32 | string count S |
//! | 12 | u32 | card count C |
//! | 16 | 12 × S | string index |
//! | 16 + 12 × S | 12 × C | card index |
//! | 16 + 12 × (S + C) | rest | data |
//!
//! An index entry is a u16 local ID, a u16 reserved (0), a u32 offset from
//! the start of the data and a u32 length in bytes. IDs are strictly
//! ascending within each index. A string's data is its UTF-16LE units
//! (even length, at most 65535 units, no terminator); an empty string is an
//! entry of length 0, because it makes its block exist. A card's data is
//! the resource's bytes unchanged. The writer stores the data in index
//! order, strings first, without padding, so the same catalog always gives
//! the same file. Catalog files are named `<module>.tcat`; they are build
//! output and ship beside the executables.

mod file;
mod lookup;

pub use file::{CatalogError, CatalogReadError, EXTENSION, EntryKind, MAGIC, TextCatalog, VERSION};
pub use lookup::{
    CLIENT_MODULES, IdsText, ModuleLoadFailure, SERVER_INTERFACE_MODULE, TextCatalogs,
    catalog_file_name, module_name_from_dll,
};
