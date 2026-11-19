//! `ImeUiCore.cpp`'s message state: what `ImeUi_ProcessMessage` keeps of
//! the IME's messages and does with them (the composition copy, the
//! candidate page, the reading window, the support level with its inline
//! composition at level 3), `ImeUi_Initialize`, `ImeUi_SetState`,
//! `ImeUi_SetSupportLevel`, `ImeUi_FinalizeString`, `ImeUi_SetInsertMode`,
//! `ImeUi_GetReadingString` and the IME's version word (`GetImeId`).
//!
//! The C++ reads the IME through IMM32 inside the window procedure. Here
//! an [`ImeContext`] hands the core what the IME did as [`ImeMessage`]s,
//! and answers the core's questions: the input context, the layout, the
//! IME's file and version resource ([`ImeFile`]), whether it exports
//! `GetReadingString`, and its reading ([`Reading`]). On Windows the
//! system draws the composition and the candidate list (decision T7), so
//! the window's context hands none of these messages (D283); a context
//! that hands them gets the C++'s behaviour, the inline Korean composition
//! of level 3 included.
//!
//! The level-3 composition is typed into the focused box as the C++'s
//! `GSendChar` and `GSendKey` type it: each unit through
//! `IMEFocus::OnChar`, each Left, Right and Backspace through
//! `IMEFocus::OnKeyDown`. Every such call goes through [`ImeFocus`] with a
//! [`CoreContext`], so what the box asks of the IME in turn (the buffer
//! full, `ImeUi_FinalizeString(false)`) reaches this state at once, as it
//! does in the C++.

use crate::input::keyboard::vk;
use crate::text_input::focus::ImeFocus;
use crate::text_input::ime::{CaretRect, ImeContext, ImeView};
use crate::text_input::ime_render::{CandidateList, CandidatePage, LANG_KOREAN, LanguageState};

/// `MAX_COMPSTRING_SIZE`: the units of the composition buffer.
pub const COMPOSITION_UNITS: usize = 256;

/// What `GetComposition` reads of a string: 256 bytes, 128 units.
const STRING_UNITS: usize = 128;

/// `WM_IME_COMPOSITION`'s composition string (`GCS_COMPSTR`) with its
/// attribute bytes (`GCS_COMPATTR`) and cursor (`GCS_CURSORPOS`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CompositionString {
    pub text: Vec<u16>,
    pub attributes: Vec<u8>,
    pub cursor: i32,
}

/// What an IME context hands the core: the messages `ImeUi_ProcessMessage`
/// handles.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ImeMessage {
    /// `WM_IME_STARTCOMPOSITION`.
    StartComposition,
    /// `WM_IME_COMPOSITION`: the result string (`GCS_RESULTSTR`), the
    /// composition (`GCS_COMPSTR`), or both.
    Composition {
        result: Option<Vec<u16>>,
        composition: Option<CompositionString>,
    },
    /// `WM_IME_ENDCOMPOSITION`.
    EndComposition,
    /// `IMN_OPENCANDIDATE` or `IMN_CHANGECANDIDATE` with the IME's list
    /// (`ImmGetCandidateListW`; `None` when it gave none).
    OpenCandidate(Option<CandidateList>),
    /// `IMN_CLOSECANDIDATE`.
    CloseCandidate,
    /// `IMN_SETOPENSTATUS` or `IMN_SETCONVERSIONMODE` (`ImeUi_GetState`).
    StateChanged,
    /// `WM_INPUTLANGCHANGE`.
    InputLanguageChanged,
    /// `IMN_PRIVATE` with its `lParam`.
    Private(u32),
}

/// The IME's file as `GetImeId` reads it: the name `ImmGetIMEFileNameA`
/// gives and the file version of its version resource (`dwFileVersionMS`,
/// `dwFileVersionLS`; `None` without one).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ImeFile {
    pub name: String,
    pub version: Option<(u32, u32)>,
}

/// A reading's text: UTF-16 units, or the bytes of the layout's ANSI code
/// page (the reading records of some IMEs).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ReadingText {
    Wide(Vec<u16>),
    Ansi(Vec<u8>),
}

impl Default for ReadingText {
    fn default() -> Self {
        ReadingText::Wide(Vec::new())
    }
}

/// The reading an IME hands the game at `IMN_PRIVATE`: its text, the index
/// of its first unit (or byte) in error, and, from `GetReadingString`,
/// whether it is vertical.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Reading {
    pub text: ReadingText,
    pub error: u32,
    pub vertical: bool,
}

/// The layouts whose IMEs `GetImeId` knows: the Traditional Chinese
/// phonetic and ChangJie, the Simplified Chinese MSPY.
const ID_LAYOUTS: [u32; 3] = [0xE008_0404, 0xE009_0404, 0xE00E_0804];

/// The IME files `GetImeId` knows by name.
const ID_FILES: [&str; 5] = [
    "TINTLGNT.IME",
    "CINTLGNT.IME",
    "MSTCIPHA.IME",
    "PINTLGNT.IME",
    "MSSCIPYA.IME",
];

/// `GetImeId`'s word for a layout and its IME file: the version's major
/// and minor numbers in the high bytes and the layout's language in the low
/// word, with the file version's low word; `None` for an IME it does not
/// know. An IME exporting `GetReadingString` (`reading_api`) is known
/// whatever its name and version.
#[must_use]
pub fn ime_ids(layout: u32, file: Option<&ImeFile>, reading_api: bool) -> Option<[u32; 2]> {
    if !ID_LAYOUTS.contains(&layout) {
        return None;
    }
    let file = file?;
    let known = reading_api || ID_FILES.iter().any(|n| n.eq_ignore_ascii_case(&file.name));
    if !known {
        return None;
    }
    let (ms, ls) = file.version?;
    let id = (((ms & 0xFF) << 8) | (ms & 0xFFFF_0000)).wrapping_mul(256);
    let language = layout & 0xFFFF;
    let version_known = reading_api
        || match language {
            0x404 => [
                0x0402_0000,
                0x0403_0000,
                0x0404_0000,
                0x0500_0000,
                0x0501_0000,
                0x0502_0000,
                0x0600_0000,
            ]
            .contains(&id),
            0x804 => [0x0401_0000, 0x0402_0000, 0x0503_0000].contains(&id),
            _ => false,
        };
    version_known.then_some([id | language, ls])
}

/// `GetImeId`'s cache (`GImeId`, `GImeIdLayout`): the words are worked
/// out again only when the layout changed. The first lookup for a layout
/// whose IME is known answers the first word whatever the index asked, as
/// the C++ does.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ImeIdCache {
    /// The layout the words are for (`NULL` at first).
    layout: u32,
    ids: [u32; 2],
}

impl ImeIdCache {
    /// `GetImeId(index)` for `layout` (`GKeyboardLayout`).
    pub fn get(
        &mut self,
        index: usize,
        layout: u32,
        reading_api: bool,
        context: &mut dyn ImeContext,
    ) -> u32 {
        if index > 1 {
            return 0;
        }
        if self.layout == layout {
            return self.ids[index];
        }
        self.layout = layout;
        let file = if ID_LAYOUTS.contains(&layout) {
            context.ime_file()
        } else {
            None
        };
        if let Some(ids) = ime_ids(layout, file.as_ref(), reading_api) {
            self.ids = ids;
            return ids[0];
        }
        self.ids = [0, 0];
        self.ids[index]
    }
}

/// `UpdateReadingOrientation`: whether the reading window is horizontal
/// for the layout and the IME's word. The Traditional Chinese IMEs read
/// their keyboard mapping (`mapping`, the low byte of the "keyboard
/// mapping" value under `HKCU\software\microsoft\windows\currentversion\`
/// and the key named, `TINTLGNT` or `MSTCIPH`).
pub fn reading_horizontal(layout: u32, id: u32, mapping: impl FnOnce(&str) -> Option<u8>) -> bool {
    if layout == 0xE00E_0804 || layout == 0xE009_0404 || id == 0 {
        return true;
    }
    if id & 0xFFFF != 0x404 {
        return false;
    }
    let version = id & 0xFFFF_0000;
    let key = if version < 0x0501_0000 {
        "TINTLGNT"
    } else {
        "MSTCIPH"
    };
    mapping(key).is_some_and(|value| {
        (version <= 0x0500_0000 && (value == 0x22 || value == 0x23))
            || ((version == 0x0501_0000 || version == 0x0502_0000)
                && (0x22..=0x24).contains(&value))
    })
}

/// `ImeUiCore.cpp`'s globals that the messages and the boxes change.
#[derive(Clone, Debug)]
pub struct ImeCore {
    /// `GbInitialized`.
    initialized: bool,
    /// `GbDisabled`: the window had no input context of its own.
    disabled: bool,
    /// `GbEnabled`: a box has the IME on (`ImeUi_SetState`).
    enabled: bool,
    /// `GSupportLevel`, `GRequestedSupportLevel`.
    support_level: u32,
    requested_level: u32,
    /// `GbInsertMode`.
    insert_mode: bool,
    /// `GComposition`, with whatever follows its terminator.
    composition: [u16; COMPOSITION_UNITS],
    /// `GCompositionAttributes`.
    attributes: [u8; COMPOSITION_UNITS],
    /// `GClientCaret`, `GCompositionCursor`.
    client_caret: i32,
    composition_cursor: i32,
    /// The candidates and the reading window.
    page: CandidatePage,
    /// `GbCandidateListOpen`.
    candidate_list_open: bool,
    /// `GbVerticalCandidates`, `GLanguageSymbol`.
    language: LanguageState,
    /// `GKeyboardLayout` (and `GLanguage`) as `ImeUi_CheckLanguage` last
    /// read it.
    layout: u32,
    ime_id: ImeIdCache,
    /// `GGetReadingString != NULL` (`SetupReadingApi`).
    reading_api: bool,
    /// `GbFinalizing`.
    finalizing: bool,
}

impl Default for ImeCore {
    fn default() -> Self {
        ImeCore {
            initialized: false,
            disabled: false,
            enabled: false,
            support_level: 0,
            requested_level: 0,
            insert_mode: true,
            composition: [0; COMPOSITION_UNITS],
            attributes: [0; COMPOSITION_UNITS],
            client_caret: 0,
            composition_cursor: 0,
            page: CandidatePage::default(),
            candidate_list_open: false,
            language: LanguageState::default(),
            layout: 0,
            ime_id: ImeIdCache::default(),
            reading_api: false,
            finalizing: false,
        }
    }
}

/// The units of `buffer` from `at` to its first zero.
fn c_len(buffer: &[u16], at: usize) -> usize {
    buffer
        .get(at..)
        .map_or(0, |b| b.iter().position(|&u| u == 0).unwrap_or(b.len()))
}

/// A string as `GetComposition` reads it into a 256-unit buffer: at most
/// 128 units, to its first zero.
fn read_string(text: &[u16]) -> &[u16] {
    let text = &text[..text.len().min(STRING_UNITS)];
    &text[..c_len(text, 0)]
}

impl ImeCore {
    /// `GSupportLevel`.
    #[must_use]
    pub fn support_level(&self) -> u32 {
        self.support_level
    }

    /// `GbEnabled`.
    #[must_use]
    pub fn enabled(&self) -> bool {
        self.enabled
    }

    /// `GbInsertMode`.
    #[must_use]
    pub fn insert_mode(&self) -> bool {
        self.insert_mode
    }

    /// `GComposition` to its terminator.
    #[must_use]
    pub fn composition(&self) -> &[u16] {
        &self.composition[..c_len(&self.composition, 0)]
    }

    /// `GCompositionAttributes`, the whole buffer.
    #[must_use]
    pub fn attributes(&self) -> &[u8] {
        &self.attributes
    }

    /// `GClientCaret`.
    #[must_use]
    pub fn client_caret(&self) -> i32 {
        self.client_caret
    }

    /// `GCompositionCursor`.
    #[must_use]
    pub fn composition_cursor(&self) -> i32 {
        self.composition_cursor
    }

    /// `ImeUi_IsCompositionEmpty`.
    #[must_use]
    pub fn is_composition_empty(&self) -> bool {
        !self.initialized || self.composition[0] == 0
    }

    /// The candidate page and reading window.
    #[must_use]
    pub fn page(&self) -> &CandidatePage {
        &self.page
    }

    /// The language's orientation and symbol.
    #[must_use]
    pub fn language(&self) -> &LanguageState {
        &self.language
    }

    /// `GKeyboardLayout` at the last language check.
    #[must_use]
    pub fn layout(&self) -> u32 {
        self.layout
    }

    /// `ImeUi_Initialize` for the client (its draw callbacks set): without
    /// an input context of its own the window stays disabled; otherwise the
    /// language is read, level 3 set, the state read, the IME closed and
    /// the reading API looked for.
    pub fn initialize(&mut self, focus: &mut ImeFocus, context: &mut dyn ImeContext) {
        self.initialized = true;
        if !context.has_context() {
            self.disabled = true;
            self.set_state(false, context);
            return;
        }
        self.check_language(context);
        self.set_support_level(3, focus, context);
        self.get_state(context);
        self.set_state(false, context);
        self.reading_api = context.reading_api();
    }

    /// `ImeUi_CheckLanguage` of the context's layout.
    pub fn check_language(&mut self, context: &mut dyn ImeContext) {
        let layout = context.layout();
        self.check_layout(layout, context);
    }

    /// `ImeUi_CheckLanguage` of `layout`.
    pub fn check_layout(&mut self, layout: u32, context: &mut dyn ImeContext) {
        self.layout = layout;
        let ImeCore {
            language,
            ime_id,
            reading_api,
            ..
        } = self;
        language.check(layout, || ime_id.get(0, layout, *reading_api, context));
    }

    /// `ImeUi_GetState`'s part here: the language checked.
    pub fn get_state(&mut self, context: &mut dyn ImeContext) {
        if self.initialized {
            self.check_language(context);
        }
    }

    /// `ImeUi_SetState`: the window's input context given or taken (never
    /// given while disabled); opening reads the state.
    pub fn set_state(&mut self, open: bool, context: &mut dyn ImeContext) {
        if !self.initialized {
            return;
        }
        let open = open && !self.disabled;
        context.set_open(open);
        self.enabled = open;
        if open {
            self.get_state(context);
        }
    }

    /// `ImeUi_SetInsertMode`.
    pub fn set_insert_mode(&mut self, insert: bool) {
        if self.initialized {
            self.insert_mode = insert;
        }
    }

    /// `GetImeId(index)` for the layout of the last check.
    pub fn ime_id(&mut self, index: usize, context: &mut dyn ImeContext) -> u32 {
        self.ime_id
            .get(index, self.layout, self.reading_api, context)
    }

    /// `ImeUi_SetSupportLevel`: levels 2 and 3 (Korean always 3), and the
    /// composition finalised (sent to the box at level 2).
    pub fn set_support_level(
        &mut self,
        level: u32,
        focus: &mut ImeFocus,
        context: &mut dyn ImeContext,
    ) {
        if !self.initialized {
            return;
        }
        self.requested_level = level;
        if (2..=3).contains(&level) {
            self.support_level = if self.layout & 0x3FF == LANG_KOREAN {
                3
            } else {
                level
            };
            self.finalize_string(true, focus, context);
        }
    }

    /// `ImeUi_FinalizeString` from within the core: the text
    /// [`ImeCore::finalize_text`] and the context give is typed into the
    /// focused box while the finalisation lasts (`SendComposition` under
    /// `GbFinalizing`).
    fn finalize_string(&mut self, send: bool, focus: &mut ImeFocus, context: &mut dyn ImeContext) {
        if self.finalizing {
            return;
        }
        let mut text = self.finalize_text(send, context);
        text.extend(context.finalize(send));
        self.finalizing = true;
        for unit in text {
            self.send_char(unit, focus, context);
        }
        self.finalizing = false;
    }

    /// `ImeUi_FinalizeString`'s part here: with the window's input context
    /// and outside another finalisation, the composition is cleared; at
    /// level 2 with `send` its text is returned for the box.
    pub fn finalize_text(&mut self, send: bool, context: &mut dyn ImeContext) -> Vec<u16> {
        if !self.initialized || !context.has_context() || self.finalizing {
            return Vec::new();
        }
        let text = if self.support_level == 2 && send {
            self.composition().to_vec()
        } else {
            Vec::new()
        };
        self.reset_composition();
        text
    }

    /// `ResetComposition`.
    fn reset_composition(&mut self) {
        self.composition = [0; COMPOSITION_UNITS];
        self.attributes = [0; COMPOSITION_UNITS];
        self.client_caret = 0;
        self.composition_cursor = 0;
    }

    /// `GSendChar`: `IMEFocus::OnChar`.
    fn send_char(&mut self, unit: u16, focus: &mut ImeFocus, context: &mut dyn ImeContext) {
        focus.on_char(
            unit,
            &mut CoreContext {
                core: self,
                context,
            },
        );
    }

    /// `SendKey` with `GSendKey` set: `IMEFocus::OnKeyDown`, `count` times.
    fn send_key(
        &mut self,
        key: u32,
        count: usize,
        focus: &mut ImeFocus,
        context: &mut dyn ImeContext,
    ) {
        for _ in 0..count {
            focus.on_key_down(
                key,
                &mut CoreContext {
                    core: self,
                    context,
                },
            );
        }
    }

    /// `SendComposition`: each unit of the composition to the box, read
    /// again after each (a full box clears it).
    fn send_composition(&mut self, focus: &mut ImeFocus, context: &mut dyn ImeContext) {
        let mut index = 0;
        while index < COMPOSITION_UNITS && self.composition[index] != 0 {
            let unit = self.composition[index];
            self.send_char(unit, focus, context);
            index += 1;
        }
    }

    /// `RemoveComposition` at level 3: the box's caret to the end of the
    /// inline composition, the composition backspaced (all of it in insert
    /// mode or when `all`, else what the new one is shorter by), and the
    /// caret back over what the new composition will overwrite.
    fn remove_composition(
        &mut self,
        all: bool,
        new_length: usize,
        focus: &mut ImeFocus,
        context: &mut dyn ImeContext,
    ) {
        if self.support_level != 3 {
            return;
        }
        let length = c_len(&self.composition, 0);
        let to_end =
            usize::try_from(length as i64 - i64::from(self.composition_cursor)).unwrap_or(0);
        self.send_key(vk::RIGHT, to_end, focus, context);
        let mut new_length = if all || self.insert_mode {
            0
        } else {
            new_length
        };
        if new_length < length {
            self.send_key(vk::BACK, length - new_length, focus, context);
        } else {
            new_length = length;
        }
        self.send_key(vk::LEFT, new_length, focus, context);
    }

    /// `ImeUi_ProcessMessage` for a message an IME context handed the
    /// game (the window procedure gives it only while a box is registered).
    pub fn process(
        &mut self,
        message: ImeMessage,
        focus: &mut ImeFocus,
        context: &mut dyn ImeContext,
    ) {
        if !self.initialized || self.disabled {
            return;
        }
        match message {
            ImeMessage::InputLanguageChanged => {
                let old = self.layout & 0x3FF;
                self.get_state(context);
                let new = self.layout & 0x3FF;
                if old != new {
                    let requested = self.requested_level;
                    let level = if new == LANG_KOREAN { 3 } else { requested };
                    self.set_support_level(level, focus, context);
                    self.requested_level = requested;
                }
                self.reading_api = context.reading_api();
                if context.has_context() {
                    context.show_reading_window(false);
                }
            }
            ImeMessage::StartComposition => self.reset_composition(),
            ImeMessage::EndComposition => {
                self.remove_composition(true, 0, focus, context);
                self.reset_composition();
            }
            ImeMessage::Composition {
                result,
                composition,
            } => {
                if !context.has_context() {
                    return;
                }
                if let Some(result) = result {
                    let text = read_string(&result).to_vec();
                    self.remove_composition(false, text.len(), focus, context);
                    self.copy_composition(&text);
                    self.send_composition(focus, context);
                    self.reset_composition();
                }
                if let Some(update) = composition {
                    self.composition_changed(&update, focus, context);
                }
            }
            ImeMessage::OpenCandidate(list) => {
                self.candidate_list_open = true;
                self.update_candidate_list(list.as_ref(), context);
            }
            ImeMessage::CloseCandidate => {
                self.candidate_list_open = false;
                if !self.page.show_reading {
                    self.page.count = 0;
                    self.page.clear_entries();
                }
            }
            ImeMessage::StateChanged => self.get_state(context),
            ImeMessage::Private(_) => {
                if !self.candidate_list_open {
                    self.get_reading_string(context);
                }
            }
        }
    }

    /// `wcscpy(GComposition, Text)`: the text and its terminator; what
    /// followed in the buffer stays.
    fn copy_composition(&mut self, text: &[u16]) {
        let n = text.len().min(COMPOSITION_UNITS - 1);
        self.composition[..n].copy_from_slice(&text[..n]);
        self.composition[n] = 0;
    }

    /// `WM_IME_COMPOSITION`'s `GCS_COMPSTR` part.
    fn composition_changed(
        &mut self,
        update: &CompositionString,
        focus: &mut ImeFocus,
        context: &mut dyn ImeContext,
    ) {
        let text = read_string(&update.text).to_vec();
        let length = text.len();
        self.remove_composition(false, length, focus, context);
        self.copy_composition(&text);
        let size = update.attributes.len().min(COMPOSITION_UNITS);
        self.attributes[..size].copy_from_slice(&update.attributes[..size]);
        if size < COMPOSITION_UNITS {
            self.attributes[size] = 0;
        }
        if self.layout & 0xFFFF == 0x404 && self.ime_id(0, context) == 0 {
            if length == 0 {
                self.page.count = 0;
            } else {
                self.page.count = 4;
                self.page.selection = -1;
                for index in (0..4).rev() {
                    if index < length {
                        self.page.set_reading_unit(index, self.composition[index]);
                    } else {
                        self.page.clear_first_unit(index);
                    }
                }
                self.composition[..4].fill(0);
                self.page.page_size = 10;
                self.page.show_reading = true;
                self.page.horizontal_reading =
                    reading_horizontal(self.layout, 0, |key| context.keyboard_mapping(key));
                if self.page.horizontal_reading {
                    self.page.assemble_reading();
                }
            }
        }
        self.client_caret = update.cursor;
        self.composition_cursor = update.cursor;
        if self.support_level == 3 {
            self.send_composition(focus, context);
            let after = usize::try_from(self.client_caret)
                .map_or(0, |caret| c_len(&self.composition, caret));
            self.send_key(vk::LEFT, after, focus, context);
        }
    }

    /// `ImeUi_UpdateCandidateList`.
    fn update_candidate_list(
        &mut self,
        list: Option<&CandidateList>,
        context: &mut dyn ImeContext,
    ) {
        if !context.has_context() {
            return;
        }
        self.page.show_reading = false;
        let Some(list) = list else {
            return;
        };
        let id = self.ime_id(0, context);
        self.page
            .apply_list(self.layout, id, self.language.vertical, list);
    }

    /// `ImeUi_GetReadingString`: the reading of a known IME, one character
    /// an entry, the selection at its first unit in error, joined into the
    /// reading window's text when it is horizontal.
    fn get_reading_string(&mut self, context: &mut dyn ImeContext) {
        let id = self.ime_id(0, context);
        if id == 0 || !context.has_context() {
            return;
        }
        let api = self.reading_api;
        let reading = context.reading(id, api).unwrap_or_default();
        let length = match &reading.text {
            ReadingText::Wide(units) => units.len(),
            ReadingText::Ansi(bytes) => bytes.len(),
        };
        let mut error = reading.error;
        if api {
            self.page.horizontal_reading = !reading.vertical;
        } else {
            if id == 0x0401_0804 && error as usize >= length {
                error = length as u32;
            }
            for index in 0..4 {
                self.page.clear_first_unit(index);
            }
        }
        let page = &mut self.page;
        page.selection = -1;
        page.count = length as i32;
        let mut display = 0usize;
        match &reading.text {
            ReadingText::Wide(units) => {
                for (index, &unit) in units.iter().enumerate() {
                    if error as usize <= index && page.selection == -1 {
                        page.selection = index as i32;
                    }
                    page.set_reading_unit(index, unit);
                }
                display = units.len();
            }
            ReadingText::Ansi(bytes) => {
                let mut byte = 0usize;
                while byte < bytes.len() {
                    if error as usize <= byte && page.selection == -1 {
                        page.selection = display as i32;
                    }
                    let lead = context.is_lead_byte(bytes[byte]);
                    let end = (byte + if lead { 2 } else { 1 }).min(bytes.len());
                    let unit = context.decode_ansi(&bytes[byte..end]);
                    page.set_first_unit(display, unit);
                    if lead {
                        byte += 1;
                    }
                    byte += 1;
                    display += 1;
                }
                page.count = display as i32;
            }
        }
        page.clear_first_unit(display);
        if !api {
            let layout = self.layout;
            self.page.horizontal_reading =
                reading_horizontal(layout, id, |key| context.keyboard_mapping(key));
        }
        self.page.show_reading = true;
        if self.page.horizontal_reading {
            self.page.assemble_reading();
        }
        self.page.page_size = 10;
    }
}

/// The context the focus manager and the boxes see: the window's context
/// with this state in front of it (`ImeUi_SetState`, `ImeUi_FinalizeString`
/// and `ImeUi_SetInsertMode` are the core's).
pub struct CoreContext<'a> {
    pub core: &'a mut ImeCore,
    pub context: &'a mut dyn ImeContext,
}

impl ImeContext for CoreContext<'_> {
    fn set_open(&mut self, open: bool) {
        self.core.set_state(open, self.context);
    }

    fn finalize(&mut self, send: bool) -> Vec<u16> {
        if self.core.finalizing {
            return Vec::new();
        }
        let mut text = self.core.finalize_text(send, self.context);
        text.extend(self.context.finalize(send));
        text
    }

    fn set_caret(&mut self, caret: CaretRect) {
        self.context.set_caret(caret);
    }

    fn set_boxes_registered(&mut self, registered: bool) {
        self.context.set_boxes_registered(registered);
    }

    fn set_insert_mode(&mut self, insert: bool) {
        self.core.set_insert_mode(insert);
        self.context.set_insert_mode(insert);
    }

    fn available(&self) -> bool {
        self.context.available()
    }

    fn view(&mut self) -> ImeView {
        self.context.view()
    }

    fn has_context(&mut self) -> bool {
        self.context.has_context()
    }

    fn layout(&mut self) -> u32 {
        self.context.layout()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::text_input::ime::NoIme;

    fn file(name: &str, ms: u32, ls: u32) -> ImeFile {
        ImeFile {
            name: name.to_owned(),
            version: Some((ms, ls)),
        }
    }

    /// `GetImeId`: the known IMEs of the three layouts, by name (any case)
    /// and version, or by exporting `GetReadingString`.
    #[test]
    fn version_words_come_from_known_imes() {
        let tintl = file("tintlgnt.ime", 0x0004_0002, 7);
        assert_eq!(
            ime_ids(0xE008_0404, Some(&tintl), false),
            Some([0x0402_0404, 7])
        );
        assert_eq!(ime_ids(0x0404_0404, Some(&tintl), false), None);
        assert_eq!(ime_ids(0xE008_0404, None, false), None);
        let odd = file("TINTLGNT.IME", 0x0005_0009, 1);
        assert_eq!(ime_ids(0xE008_0404, Some(&odd), false), None);
        assert_eq!(
            ime_ids(0xE008_0404, Some(&odd), true),
            Some([0x0509_0404, 1])
        );
        let other = file("OTHER.IME", 0x0004_0002, 1);
        assert_eq!(ime_ids(0xE009_0404, Some(&other), false), None);
        let unversioned = ImeFile {
            name: "MSTCIPHA.IME".to_owned(),
            version: None,
        };
        assert_eq!(ime_ids(0xE008_0404, Some(&unversioned), true), None);
        let pinyin = file("MSSCIPYA.IME", 0x0005_0003, 2);
        assert_eq!(
            ime_ids(0xE00E_0804, Some(&pinyin), false),
            Some([0x0503_0804, 2])
        );
    }

    /// The cache answers the first word at a layout's first lookup, then
    /// each word; a layout without a known IME answers 0.
    #[test]
    fn the_first_lookup_answers_the_first_word() {
        struct Files;
        impl ImeContext for Files {
            fn set_open(&mut self, _: bool) {}
            fn finalize(&mut self, _: bool) -> Vec<u16> {
                Vec::new()
            }
            fn set_caret(&mut self, _: CaretRect) {}
            fn set_boxes_registered(&mut self, _: bool) {}
            fn ime_file(&mut self) -> Option<ImeFile> {
                Some(file("MSTCIPHA.IME", 0x0005_0001, 3))
            }
        }
        let mut cache = ImeIdCache::default();
        assert_eq!(cache.get(1, 0xE008_0404, false, &mut Files), 0x0501_0404);
        assert_eq!(cache.get(1, 0xE008_0404, false, &mut Files), 3);
        assert_eq!(cache.get(2, 0xE008_0404, false, &mut Files), 0);
        assert_eq!(cache.get(0, 0x0409_0409, false, &mut Files), 0);
        assert_eq!(cache.get(0, 0, false, &mut NoIme), 0);
    }

    /// `UpdateReadingOrientation`: two layouts and no word are horizontal;
    /// the Traditional Chinese IMEs follow their keyboard mapping.
    #[test]
    fn the_reading_orientation_follows_the_keyboard_mapping() {
        assert!(reading_horizontal(0xE009_0404, 0x0402_0404, |_| None));
        assert!(reading_horizontal(0xE008_0404, 0, |_| None));
        assert!(!reading_horizontal(0xE008_0804, 0x0401_0804, |_| Some(
            0x22
        )));
        let old = |key: &str| (key == "TINTLGNT").then_some(0x23);
        assert!(reading_horizontal(0xE008_0404, 0x0402_0404, old));
        assert!(!reading_horizontal(0xE008_0404, 0x0402_0404, |_| Some(
            0x24
        )));
        let new = |key: &str| (key == "MSTCIPH").then_some(0x24);
        assert!(reading_horizontal(0xE008_0404, 0x0501_0404, new));
        assert!(!reading_horizontal(0xE008_0404, 0x0600_0404, new));
    }

    /// A reading of more than ten characters keeps ten entries and its
    /// count (D592).
    #[test]
    fn a_long_reading_keeps_ten_entries() {
        struct Long;
        impl ImeContext for Long {
            fn set_open(&mut self, _: bool) {}
            fn finalize(&mut self, _: bool) -> Vec<u16> {
                Vec::new()
            }
            fn set_caret(&mut self, _: CaretRect) {}
            fn set_boxes_registered(&mut self, _: bool) {}
            fn layout(&mut self) -> u32 {
                0xE008_0404
            }
            fn ime_file(&mut self) -> Option<ImeFile> {
                Some(file("TINTLGNT.IME", 0x0004_0002, 0))
            }
            fn reading(&mut self, _: u32, _: bool) -> Option<Reading> {
                Some(Reading {
                    text: ReadingText::Wide((0x61..0x6D).collect()),
                    error: 11,
                    vertical: false,
                })
            }
        }
        let mut focus = ImeFocus::default();
        let mut core = ImeCore::default();
        core.initialize(&mut focus, &mut Long);
        core.process(ImeMessage::Private(1), &mut focus, &mut Long);
        let page = core.page();
        assert_eq!(page.count, 12);
        assert_eq!(page.selection, 11);
        assert_eq!(page.entry(9), [0x6A]);
        assert!(page.show_reading && !page.horizontal_reading);
    }
}
