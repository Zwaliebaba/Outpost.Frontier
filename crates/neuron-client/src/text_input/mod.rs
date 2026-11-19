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
//!
//! Everything here is platform-free: [`ime::ImeContext`] is what the boxes
//! ask of the IME, implemented on Windows by `platform::ime::WindowIme`
//! and by [`ime::NoIme`] elsewhere. Drawing a box's text is the UI
//! toolkit's (P4.13); it measures text for the caret through
//! [`edit_box::TextMeasure`].

pub mod edit_box;
pub mod focus;
pub mod ime;
