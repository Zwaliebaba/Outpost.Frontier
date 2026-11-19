//! Text input and the IME (work package P4.11): the edit-box model of
//! `imeui.dll` (`CInputBox`, `IMEInputBox`, `CCaret`), its focus manager
//! (`IMEFocus`), and the IME state the window procedure keeps.
//!
//! The C++ draws the IME's composition string and candidate list itself
//! (support level 2, inline with synthesised keys at level 3 for Korean)
//! and reaches into IME internals for reading strings (`ImmLockIMC`,
//! per-IME DLL probing). Here the system draws its composition and
//! candidate windows, and `platform::ime` places them at the focused box's
//! caret with documented IMM32 calls (`ImmSetCompositionWindow`,
//! `ImmSetCandidateWindow`). The window keeps a copy of the composition
//! ([`ime::Composition`]: text, attributes, cursor) for the UI toolkit,
//! which may hide its own caret while one is in progress, as `CCaret` does.
//! What `imeui` does with the IME's messages, levels 2 and 3, the
//! candidate page, the reading window and the IME's version word, is
//! [`ime_core`], for the messages an IME context hands it.
//!
//! Everything here is platform-free: [`ime::ImeContext`] is what the boxes
//! ask of the IME, implemented on Windows by `platform::ime::WindowIme`
//! and by [`ime::NoIme`] elsewhere. A box measures text for the caret
//! through [`edit_box::TextMeasure`]; the client's boxes measure with their
//! own font ([`ime_font::ImeUiFont`], `CImeUiFont`, over the client's font
//! backend). What `imeui` draws, the toolbar, the composition and the
//! candidate list, is [`ime_render`]; the client's boxes and their draw
//! through the engine's IME pass are the UI toolkit's (`ui::ime`).

pub mod edit_box;
pub mod focus;
pub mod ime;
pub mod ime_core;
pub mod ime_font;
pub mod ime_render;
