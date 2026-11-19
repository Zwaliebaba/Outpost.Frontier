//! What `imeui` draws (`ImeUiCore.cpp`'s `ImeUi_Render`,
//! `ImeUiToolbar.cpp`'s `ImeUi_RenderToolbar`) and the state it draws from:
//! the appearance (`IMEFocus::GetDefaultAppearance`, `ImeUi_SetAppearance`,
//! the composition backgrounds), the candidate page
//! (`ImeUi_UpdateCandidateList`, `FormatCandidate`) and the language's
//! orientation and symbol (`ImeUi_CheckLanguage`).
//!
//! The drawing goes through [`ImeDraw`]: `GDrawRect`, `GDrawFan` and the
//! fonts' text through their owner box, which the client implements
//! (`ui::ime`). Everything here is platform-free; the IME's own state (the
//! composition, the candidate list, the language, the tick count) comes
//! from the window's [`ImeContext`](super::ime::ImeContext) as an
//! [`ImeView`](super::ime::ImeView).

use neuron_core::num::trunc_to_i32;

use crate::text_input::ime_font::{FanVertex, ImeDraw, ImeFont};
use crate::window::Rect;

/// The support level the client sets (`IMEFocus`'s constructor calls
/// `ImeUi_SetSupportLevel(2)`): the game draws the composition and the
/// candidate list. `ImeUi_SetSupportLevel` makes it 3 for Korean (the
/// composition typed inline into the box by synthesised keys,
/// `text_input::ime_core`).
pub const SUPPORT_LEVEL: u32 = 2;

/// `LANG_CHINESE`, `LANG_JAPANESE`, `LANG_KOREAN`.
pub const LANG_CHINESE: u32 = 0x04;
pub const LANG_JAPANESE: u32 = 0x11;
pub const LANG_KOREAN: u32 = 0x12;

/// `ATTR_TARGET_CONVERTED`.
const ATTR_TARGET_CONVERTED: u8 = 1;

/// `_IMEUI_APPEARANCE` without its symbol font (the toolbar is handed the
/// font apart).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Appearance {
    pub symbol_on_color: u32,
    pub symbol_off_color: u32,
    pub symbol_background_color: u32,
    pub symbol_height: u8,
    pub symbol_alpha: u8,
    pub symbol_position: u8,
    pub candidate_text_color: u32,
    pub candidate_selected_text_color: u32,
    pub candidate_color: u32,
    pub composition_input_color: u32,
    pub composition_target_color: u32,
    pub composition_converted_color: u32,
    pub composition_target_converted_color: u32,
    pub composition_error_color: u32,
    pub composition_alpha: u8,
    pub composition_color: u32,
}

/// `ImeUi.dll`'s string table (`imeui.rc`, ids 1 to 16), which
/// `GetDefaultAppearance` parses. The client does not look for the DLL
/// (D283); the strings are its resources, compiled in (D551).
pub const APPEARANCE_STRINGS: [&str; 16] = [
    "0",
    "0x00404040",
    "0xff000000",
    "24",
    "0",
    "0xa0",
    "0xffffffff",
    "0xff000000",
    "0xff000000",
    "0x00ffff00",
    "0x000000ff",
    "0x0000ff00",
    "0x00ff0000",
    "0x00ff0000",
    "0x80",
    "0xff000000",
];

/// `ParseAppearanceNumber`: `0x` hex to the end of the text, where a
/// character that is not a hex digit repeats the digit before it, or
/// leading decimal digits. Wrapping, as the C++'s `unsigned`.
#[must_use]
pub fn parse_appearance_number(text: &[u16]) -> u32 {
    let text = &text[..text.iter().position(|&u| u == 0).unwrap_or(text.len())];
    let mut value = 0u32;
    if text.len() >= 2 && text[0] == u16::from(b'0') && (text[1] | 0x20) == u16::from(b'x') {
        let mut digit = 0u32;
        for &c in &text[2..] {
            match c {
                0x30..=0x39 => digit = u32::from(c - 0x30),
                0x41..=0x46 => digit = u32::from(c - 0x41) + 10,
                0x61..=0x66 => digit = u32::from(c - 0x61) + 10,
                _ => {}
            }
            value = (value << 4).wrapping_add(digit);
        }
    } else {
        for &c in text.iter().take_while(|&&c| (0x30..=0x39).contains(&c)) {
            value = value.wrapping_mul(10).wrapping_add(u32::from(c - 0x30));
        }
    }
    value
}

impl Appearance {
    /// `GetDefaultAppearance` over sixteen strings (ids 1 to 16; the byte
    /// fields keep the low byte).
    #[must_use]
    pub fn parse(strings: &[&[u16]; 16]) -> Appearance {
        let n = |id: usize| parse_appearance_number(strings[id - 1]);
        let b = |id: usize| n(id) as u8;
        Appearance {
            symbol_on_color: n(1),
            symbol_off_color: n(2),
            symbol_background_color: n(3),
            symbol_height: b(4),
            symbol_position: b(5),
            symbol_alpha: b(6),
            candidate_text_color: n(7),
            candidate_selected_text_color: n(8),
            candidate_color: n(9),
            composition_input_color: n(10),
            composition_target_color: n(11),
            composition_converted_color: n(12),
            composition_target_converted_color: n(13),
            composition_error_color: n(14),
            composition_alpha: b(15),
            composition_color: n(16),
        }
    }

    /// `GetDefaultAppearance` of `ImeUi.dll`'s strings.
    #[must_use]
    pub fn default_strings() -> Appearance {
        let units: Vec<Vec<u16>> = APPEARANCE_STRINGS
            .iter()
            .map(|s| s.encode_utf16().collect())
            .collect();
        let refs: [&[u16]; 16] = std::array::from_fn(|i| units[i].as_slice());
        Appearance::parse(&refs)
    }

    /// `ImeUi_SetAppearance`: the symbol and composition colours lose
    /// their alpha.
    #[must_use]
    pub fn set(self) -> Appearance {
        const RGB: u32 = 0x00FF_FFFF;
        Appearance {
            symbol_on_color: self.symbol_on_color & RGB,
            symbol_off_color: self.symbol_off_color & RGB,
            symbol_background_color: self.symbol_background_color & RGB,
            composition_input_color: self.composition_input_color & RGB,
            composition_target_color: self.composition_target_color & RGB,
            composition_converted_color: self.composition_converted_color & RGB,
            composition_target_converted_color: self.composition_target_converted_color & RGB,
            composition_error_color: self.composition_error_color & RGB,
            ..self
        }
    }

    /// The appearance the client's boxes set when they take the focus
    /// (`UpdateAppearance`: `ImeUi_SetAppearance(GetDefaultAppearance())`).
    #[must_use]
    pub fn client() -> Appearance {
        Appearance::default_strings().set()
    }

    /// `UpdateCompositionColors`: the backgrounds of attributes 0 to 4,
    /// opaque at support level 2.
    #[must_use]
    pub fn composition_backgrounds(&self, support_level: u32) -> [u32; 5] {
        let alpha = if support_level == 2 {
            0xFF00_0000
        } else {
            u32::from(self.composition_alpha) << 24
        };
        [
            self.composition_input_color | alpha,
            self.composition_target_color | alpha,
            self.composition_converted_color | alpha,
            self.composition_target_converted_color | alpha,
            self.composition_error_color | alpha,
        ]
    }

    /// `ImeUi_SetFontAppearance`'s colours: the candidates' and the
    /// composition's font colour, the appearance's unless 0, else `color`.
    #[must_use]
    pub fn font_colors(&self, color: u32) -> (u32, u32) {
        let pick = |c: u32| if c != 0 { c } else { color };
        (pick(self.candidate_color), pick(self.composition_color))
    }
}

/// A `CANDIDATELIST` as the IME gives it: the selection, the page size
/// and the strings.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CandidateList {
    pub selection: u32,
    pub page_size: u32,
    pub strings: Vec<Vec<u16>>,
}

/// The candidates on show (`GCandidates`, `GCandidateCount`,
/// `GCandidatePageSize`, `GCandidateSelection`) and the reading window's
/// state (`GbShowReading`, `GbHorizontalReading`, `GReading`,
/// `GReadingSelection`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CandidatePage {
    /// Ten entries, each formatted (`FormatCandidate`) or a reading's
    /// character; an entry is its text to its first zero
    /// ([`CandidatePage::entry`]), and what follows the zero is what the
    /// slot held before, as in the C++'s buffers.
    pub entries: Vec<Vec<u16>>,
    pub count: i32,
    pub page_size: i32,
    pub selection: i32,
    pub show_reading: bool,
    pub horizontal_reading: bool,
    pub reading: Vec<u16>,
    pub reading_selection: i32,
}

impl Default for CandidatePage {
    fn default() -> Self {
        CandidatePage {
            entries: vec![Vec::new(); 10],
            count: 0,
            page_size: 0,
            selection: 0,
            show_reading: false,
            horizontal_reading: false,
            reading: Vec::new(),
            reading_selection: 0,
        }
    }
}

/// `FormatCandidate`: the number's last digit, a space before the text in
/// a vertical list or after it in a horizontal one.
#[must_use]
pub fn format_candidate(number: i32, text: &[u16], vertical: bool) -> Vec<u16> {
    let mut out = Vec::with_capacity(text.len() + 2);
    out.push(u16::try_from(number.rem_euclid(10)).unwrap_or(0) + u16::from(b'0'));
    if vertical {
        out.push(u16::from(b' '));
    }
    out.extend(text.iter().take_while(|&&u| u != 0));
    if !vertical {
        out.push(u16::from(b' '));
    }
    out
}

impl CandidatePage {
    /// Entry `index` to its first zero (empty past the ten).
    #[must_use]
    pub fn entry(&self, index: usize) -> &[u16] {
        self.entries.get(index).map_or(&[][..], |e| {
            &e[..e.iter().position(|&u| u == 0).unwrap_or(e.len())]
        })
    }

    /// `memset(GCandidates, 0, ...)`.
    pub fn clear_entries(&mut self) {
        self.entries = vec![Vec::new(); 10];
    }

    /// `GCandidates[index][0] = 0` (nothing past the ten).
    pub fn clear_first_unit(&mut self, index: usize) {
        if let Some(e) = self.entries.get_mut(index)
            && let Some(first) = e.first_mut()
        {
            *first = 0;
        }
    }

    /// `GCandidates[index][0] = unit` alone (`MultiByteToWideChar` of one
    /// unit, no terminator).
    pub fn set_first_unit(&mut self, index: usize, unit: u16) {
        if let Some(e) = self.entries.get_mut(index) {
            match e.first_mut() {
                Some(first) => *first = unit,
                None => e.push(unit),
            }
        }
    }

    /// `GCandidates[index][0] = unit; GCandidates[index][1] = 0`.
    pub fn set_reading_unit(&mut self, index: usize, unit: u16) {
        if let Some(e) = self.entries.get_mut(index) {
            if e.len() < 2 {
                e.resize(2, 0);
            }
            e[0] = unit;
            e[1] = 0;
        }
    }

    /// `AssembleReading`: the reading window's text, the entries joined,
    /// with the selection at the selected entry's first unit.
    pub fn assemble_reading(&mut self) {
        self.reading_selection = -1;
        self.reading.clear();
        for index in 0..usize::try_from(self.count)
            .unwrap_or(0)
            .min(self.entries.len())
        {
            if self.selection == index as i32 {
                self.reading_selection = self.reading.len() as i32;
            }
            let text = self.entry(index).to_vec();
            self.reading.extend(text);
        }
    }

    /// `ImeUi_UpdateCandidateList` over a candidate list into a new page.
    #[must_use]
    pub fn update(
        language: u32,
        ime_id: u32,
        vertical: bool,
        list: &CandidateList,
    ) -> CandidatePage {
        let mut page = CandidatePage::default();
        page.apply_list(language, ime_id, vertical, list);
        page
    }

    /// `ImeUi_UpdateCandidateList` over a candidate list: the page that
    /// holds the selection (for the Simplified Chinese IME with an id, the
    /// candidates that fit 18 units), the selection within it, formatted.
    /// A page size of 0 starts at the first candidate (the C++ divides by
    /// it). The reading window's state stays.
    pub fn apply_list(&mut self, language: u32, ime_id: u32, vertical: bool, list: &CandidateList) {
        let low = language & 0xFFFF;
        let count = list.strings.len() as u32;
        let page = self;
        page.selection = list.selection as i32;
        page.count = count as i32;
        let mut start = 0u32;
        if low == 0x804 && ime_id != 0 {
            let mut accumulated = 0u32;
            let mut index = 0u32;
            while index < count {
                let text = &list.strings[index as usize];
                let length = text.iter().take_while(|&&u| u != 0).count() as u32 + 1;
                if accumulated + length > 18 {
                    if index > list.selection {
                        break;
                    }
                    start = index;
                    accumulated = length;
                } else {
                    accumulated += length;
                }
                index += 1;
            }
            page.page_size = (index - start) as i32;
        } else {
            let size = list.page_size.min(10);
            page.page_size = size as i32;
            start = list.selection.checked_div(size).map_or(0, |p| p * size);
        }
        if low == 0x804 && ime_id == 0 {
            page.selection = -1;
        } else {
            page.selection = page.selection.wrapping_sub(start as i32);
        }
        page.clear_entries();
        for (display, index) in (start..count).enumerate() {
            if display >= page.page_size.max(0) as usize || display >= page.entries.len() {
                break;
            }
            page.entries[display] =
                format_candidate(display as i32 + 1, &list.strings[index as usize], vertical);
        }
        if low & 0x3FF == LANG_KOREAN || (low == 0x404 && ime_id == 0) {
            page.selection = -1;
        }
    }
}

/// The language's candidate orientation and toolbar symbol
/// (`GbVerticalCandidates`, `GLanguageSymbol`), as `ImeUi_CheckLanguage`
/// keeps them: recomputed only when the layout changes, and a language
/// other than Chinese, Japanese and Korean leaves the orientation as it
/// was.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LanguageState {
    pub vertical: bool,
    pub symbol: Vec<u16>,
    previous: Option<u32>,
}

impl Default for LanguageState {
    fn default() -> Self {
        LanguageState {
            vertical: true,
            symbol: vec![u16::from(b'A')],
            previous: None,
        }
    }
}

impl LanguageState {
    /// `ImeUi_CheckLanguage` for the layout `language` (its low 32 bits);
    /// `ime_id` is `GetImeId(0)`, asked only for the layouts whose
    /// orientation depends on it.
    pub fn check(&mut self, language: u32, ime_id: impl FnOnce() -> u32) {
        if self.previous == Some(language) {
            return;
        }
        self.previous = Some(language);
        const ZHONG: u16 = 0x4E2D;
        let low = language & 0xFFFF;
        match low & 0x3FF {
            LANG_CHINESE => {
                let sub = low >> 10;
                self.vertical = true;
                self.symbol = vec![ZHONG];
                if sub != 1 {
                    self.symbol = vec![u16::from(b'A')];
                    if sub == 2 {
                        self.symbol = vec![ZHONG];
                        self.vertical = ime_id() == 0;
                    }
                }
            }
            LANG_JAPANESE => {
                self.vertical = true;
                self.symbol = vec![0x3042];
            }
            LANG_KOREAN => {
                self.vertical = false;
                self.symbol = vec![0xAC00];
            }
            _ => self.symbol = vec![u16::from(b'A')],
        }
    }
}

/// The render's own state between frames: the composition caret's blink
/// (`GLastCaretTick`, `GbCaretVisible`) and the toolbar's animation
/// (`GToolbarLastTick`, `GToolbarPhase`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RenderState {
    pub last_caret_tick: u32,
    pub caret_visible: bool,
    pub toolbar_last_tick: f64,
    pub toolbar_phase: u32,
}

/// What one `ImeUi_Render` reads.
pub struct RenderInput<'a> {
    pub support_level: u32,
    pub appearance: &'a Appearance,
    /// `Appearance.SymbolFont`.
    pub symbol_font: &'a dyn ImeFont,
    /// The symbol font's symbol, which the fan carries (`StaticDrawFan`).
    pub fan_text: &'a [u16],
    /// The symbol font's colour at the fan.
    pub fan_color: &'a dyn Fn() -> u32,
    /// `GCompositionFont`: the focused box's font.
    pub font: &'a dyn ImeFont,
    /// `GBounding`: the focused box's rectangle.
    pub bounding: Rect,
    /// `GPosition`: the focused box's caret.
    pub position: (i32, i32),
    /// `GScreenWidth`, `GScreenHeight`.
    pub screen: (i32, i32),
    /// `GImeState`.
    pub state: u32,
    /// `GLanguage`.
    pub language: u32,
    /// `GLanguageSymbol`.
    pub symbol: &'a [u16],
    /// `GetImeId(0)`.
    pub ime_id: u32,
    /// `GComposition` (to its NUL) and `GCompositionAttributes`.
    pub composition: &'a [u16],
    pub attributes: &'a [u8],
    /// `GClientCaret`, `GCompositionCursor`.
    pub client_caret: i32,
    pub composition_cursor: i32,
    pub candidates: &'a CandidatePage,
    /// `GbVerticalCandidates`.
    pub vertical: bool,
    /// `GetTickCount` and `GCaretBlinkInterval`.
    pub tick: u32,
    pub caret_blink: u32,
    /// `GCandidateColor`, `GCompositionColor`.
    pub candidate_color: u32,
    pub composition_color: u32,
    /// `GCompositionBackgrounds`.
    pub backgrounds: [u32; 5],
}

fn half(value: i32) -> i32 {
    (value.cast_unsigned() >> 1).cast_signed()
}

/// The toolbar's rainbow (`ImeUi_RenderToolbar`'s `Rainbow`).
const RAINBOW: [u32; 15] = [
    0xff0000, 0xff3000, 0xff6000, 0xff9000, 0xffc000, 0xffff00, 0xc0ff00, 0x90ff00, 0x60ff00,
    0x30c0ff, 0x00a0ff, 0x3090ff, 0x6060ff, 0x9030ff, 0xc000ff,
];

/// `ImeUi_RenderToolbar`: the symbol's disc, a fan of 16 vertices beside
/// the box (or in a corner), turning a step every 250 ms while the IME is
/// on, then the symbol in the symbol font.
pub fn render_toolbar(state: &mut RenderState, input: &RenderInput<'_>, out: &mut dyn ImeDraw) {
    let a = input.appearance;
    let bounding = input.bounding;
    let active = input.state != 0;
    let diameter = f32::from(a.symbol_height);
    let radius = f64::from(diameter) * 0.5;
    let mut v = [FanVertex::default(); 16];
    let width = f64::from(input.screen.0.cast_unsigned());
    let height = f64::from(input.screen.1.cast_unsigned());
    match a.symbol_position {
        0 => {
            v[0].x = if width >= f64::from(diameter) + f64::from(bounding.right) + 4.0 {
                ((f64::from(bounding.right) - radius) + f64::from(diameter) + 4.0) as f32
            } else {
                (f64::from(diameter) * -0.5 + f64::from(bounding.left) - 4.0) as f32
            };
            v[0].y = (f64::from(bounding.bottom) - f64::from(a.symbol_height >> 1)) as f32;
        }
        1 => {
            v[0].x = (radius + 4.0) as f32;
            v[0].y = (radius + 4.0) as f32;
        }
        2 => {
            v[0].x = (width - (radius + 4.0)) as f32;
            v[0].y = (radius + 4.0) as f32;
        }
        3 => {
            v[0].x = (width - (radius + 4.0)) as f32;
            v[0].y = (height - (radius + 4.0)) as f32;
        }
        4 => {
            v[0].x = (radius + 4.0) as f32;
            v[0].y = (height - (radius + 4.0)) as f32;
        }
        _ => {}
    }
    v[0].rhw = 1.0;
    if active {
        if f64::from(input.tick) - state.toolbar_last_tick > 250.0 {
            state.toolbar_phase += 1;
            state.toolbar_last_tick = f64::from(input.tick);
            if state.toolbar_phase > 13 {
                state.toolbar_phase = 0;
            }
        }
    } else {
        state.toolbar_phase = 0;
    }
    let angle_step = f64::from(f32::from_bits(0x3EE5_C8F9));
    let first = if active { state.toolbar_phase } else { 0 };
    let (center_x, center_y) = (f64::from(v[0].x), f64::from(v[0].y));
    for (step, vertex) in (first..).zip(v[1..].iter_mut()) {
        let angle = f64::from(step) * angle_step;
        vertex.x = (radius * angle.cos() + center_x) as f32;
        vertex.y = (radius * angle.sin() + center_y) as f32;
        vertex.rhw = 1.0;
    }
    let alpha = u32::from(a.symbol_alpha) << 24;
    v[0].color = alpha.wrapping_add(0x00FF_FFFF);
    for (i, vertex) in v.iter_mut().enumerate().skip(1) {
        let color = if a.symbol_on_color == 0 && active {
            RAINBOW[i - 1]
        } else if active {
            a.symbol_on_color
        } else {
            a.symbol_off_color
        };
        vertex.color = alpha.wrapping_add(color);
    }
    out.draw_fan(&v, input.fan_text, (input.fan_color)());
    let mut font_height = f64::from(a.symbol_height) * f64::from(0.625f32);
    if input.language & 0x3FF == LANG_KOREAN && active {
        font_height *= f64::from(0.8f32);
    }
    let font = input.symbol_font;
    font_height *= f64::from(1.333_333_4_f32);
    font.set_height(trunc_to_i32(font_height));
    font.set_color(alpha | a.symbol_background_color);
    let a_text = [u16::from(b'A')];
    let text: &[u16] = if input.state == 1 {
        input.symbol
    } else {
        &a_text
    };
    let (tw, th) = font.text_extent(text);
    let x = trunc_to_i32(f64::from(v[0].x)).wrapping_sub(half(tw));
    let y = trunc_to_i32(f64::from(v[0].y)).wrapping_sub(half(th));
    font.set_position(x, y);
    font.draw(text, out);
}

/// `ImeUi_Render` for the client's focused box (the caller tests that the
/// IME is enabled and has a font): the toolbar, the composition at level
/// 2 (each unit's background in its attribute's colour, the caret, the
/// text through the box) or level 3 (the backgrounds inside the box), and
/// the candidate list or reading window beside the conversion target.
pub fn render(
    state: &mut RenderState,
    input: &RenderInput<'_>,
    draw_composition: bool,
    show_toolbar: bool,
    out: &mut dyn ImeDraw,
) {
    if !draw_composition && !show_toolbar {
        return;
    }
    let level = input.support_level;
    if level == 2 && !show_toolbar {
        return;
    }
    if show_toolbar {
        render_toolbar(state, input, out);
    }
    if input.tick.wrapping_sub(state.last_caret_tick) > input.caret_blink {
        state.caret_visible = !state.caret_visible;
        state.last_caret_tick = input.tick;
    }
    let font = input.font;
    font.set_color(input.composition_color);
    let composition = input.composition;
    let length = composition.len();
    let (mut x, y) = input.position;
    let space = [u16::from(b' ')];
    let (_, mut font_height) = font.text_extent(&space);
    let caret_at = usize::try_from(input.client_caret).ok();
    if level == 3 && input.client_caret != 0 && length != 0 {
        let end = caret_at.unwrap_or(0).min(length);
        let (width, _) = font.text_extent(&composition[..end]);
        x = x.wrapping_sub(width);
    }
    let caret = |out: &mut dyn ImeDraw, state: &RenderState, x: i32, y: i32, h: i32| {
        if state.caret_visible {
            out.draw_rect(
                x,
                y.wrapping_add(1),
                x.wrapping_add(2),
                y.wrapping_add(h).wrapping_sub(1),
                input.composition_color,
            );
        }
    };
    let start_x = x;
    let (mut anchor_x, mut anchor_y) = (0i32, 0i32);
    let mut rendered: Vec<u16> = Vec::new();
    let mut index = 0usize;
    let chinese_simplified_id = input.ime_id & 0xFFFF == 0x804;
    if (input.language & 0xFFFF) & 0x3FF != LANG_KOREAN || state.caret_visible {
        while index < length {
            let previous_x = x;
            let (width, h) = font.text_extent(&composition[..=index]);
            font_height = h;
            x = width.wrapping_add(start_x);
            let mut background = 0;
            let mut target = false;
            let attribute = input.attributes.get(index).copied().unwrap_or(0);
            if attribute <= 4 {
                background = input.backgrounds[usize::from(attribute)];
                if attribute == ATTR_TARGET_CONVERTED && !chinese_simplified_id {
                    target = true;
                }
            }
            if level == 3 {
                if draw_composition
                    && previous_x >= input.bounding.left
                    && previous_x <= input.bounding.right
                {
                    out.draw_rect(previous_x, y, x, y.wrapping_add(font_height), background);
                }
            } else if level == 2 {
                out.draw_rect(previous_x, y, x, y.wrapping_add(font_height), background);
                if rendered.len() < 251 {
                    rendered.push(composition[index]);
                }
                if Some(index) == caret_at {
                    caret(out, state, previous_x, y, font_height);
                }
            }
            if (target && anchor_x == 0)
                || (chinese_simplified_id && index as i32 == input.composition_cursor)
            {
                anchor_x = previous_x;
                anchor_y = y;
            }
            index += 1;
        }
    }
    if level == 2 {
        if length != 0 && Some(index) == caret_at {
            caret(out, state, x, y, font_height);
        }
        if !rendered.is_empty() {
            font.set_position(input.position.0, input.position.1);
            font.draw(&rendered, out);
        }
    }
    let page = input.candidates;
    let first = !page.entry(0).is_empty();
    if !show_toolbar || page.count == 0 || !first {
        return;
    }
    if anchor_x == 0 {
        (anchor_x, anchor_y) = input.position;
    }
    let digit = [u16::from(b'0')];
    let (digit_width, _) = font.text_extent(&digit);
    let mut max_height;
    let space_width;
    (space_width, max_height) = font.text_extent(&space);
    let padding = half(space_width.wrapping_add(1));
    let mut widths = [0i32; 10];
    let mut total_width = 0i32;
    let mut max_width = 0i32;
    if page.show_reading && page.horizontal_reading {
        (max_width, max_height) = font.text_extent(&page.reading);
    } else {
        let mut ci = 0usize;
        while ci < widths.len() && !page.entry(ci).is_empty() && (ci as i32) < page.page_size {
            let (w, h) = font.text_extent(page.entry(ci));
            max_width = max_width.max(w);
            max_height = max_height.max(h);
            widths[ci] = w;
            total_width = total_width.wrapping_add(w);
            ci += 1;
        }
    }
    let count = if page.show_reading && page.count < page.page_size {
        page.count
    } else {
        page.page_size
    };
    let vertical = input.vertical;
    let outer_height = if vertical && (!page.show_reading || !page.horizontal_reading) {
        count.wrapping_mul(max_height)
    } else {
        max_height
    }
    .wrapping_add(2);
    if anchor_y
        .wrapping_add(font_height)
        .wrapping_add(outer_height)
        .cast_unsigned()
        > input.screen.1.cast_unsigned()
    {
        anchor_y = anchor_y.wrapping_sub(outer_height);
    } else {
        anchor_y = anchor_y.wrapping_add(font_height);
    }
    anchor_y = anchor_y.max(0);
    let outer_width = if !page.show_reading && chinese_simplified_id {
        total_width.wrapping_add(count.wrapping_mul(padding))
    } else if vertical || page.show_reading {
        max_width
            .wrapping_add(padding.wrapping_mul(2))
            .wrapping_add(2)
    } else {
        max_width
            .wrapping_add(1)
            .wrapping_mul(count)
            .wrapping_add(1)
    };
    anchor_x = anchor_x.max(0);
    let a = input.appearance;
    let mut right = anchor_x.wrapping_add(outer_width);
    let mut bottom = anchor_y.wrapping_add(outer_height);
    out.draw_rect(
        anchor_x,
        anchor_y,
        right,
        bottom,
        a.candidate_selected_text_color,
    );
    let mut inner_x = anchor_x.wrapping_add(1);
    let inner_y = anchor_y.wrapping_add(1);
    right = right.wrapping_sub(1);
    bottom = bottom.wrapping_sub(1);
    if !page.show_reading && !chinese_simplified_id {
        if vertical {
            let divider = inner_x
                .wrapping_add(half(space_width))
                .wrapping_add(padding)
                .wrapping_add(digit_width)
                .wrapping_sub(1);
            out.draw_rect(inner_x, inner_y, divider, bottom, a.candidate_text_color);
            out.draw_rect(
                divider.wrapping_add(1),
                inner_y,
                right,
                bottom,
                a.candidate_text_color,
            );
        } else {
            let mut cell_x = inner_x;
            for _ in 0..count.max(0) {
                out.draw_rect(
                    cell_x,
                    inner_y,
                    cell_x.wrapping_add(max_width),
                    bottom,
                    a.candidate_text_color,
                );
                cell_x = cell_x.wrapping_add(max_width).wrapping_add(1);
            }
        }
    } else {
        out.draw_rect(inner_x, inner_y, right, bottom, a.candidate_text_color);
    }
    let inverse = (!(input.candidate_color & 0x00FF_FFFF)).wrapping_sub(0x0100_0000);
    if page.show_reading && page.horizontal_reading {
        let text_x = inner_x.wrapping_add(padding);
        let reading = &page.reading[..page
            .reading
            .iter()
            .position(|&u| u == 0)
            .unwrap_or(page.reading.len())];
        let mut left_width = -1i32;
        let selection = usize::try_from(page.reading_selection).ok();
        if let Some(sel) = selection {
            let (right_width, _) = font.text_extent(&reading[..(sel + 1).min(reading.len())]);
            (left_width, _) = font.text_extent(&reading[..sel.min(reading.len())]);
            out.draw_rect(
                text_x.wrapping_add(left_width),
                inner_y,
                text_x.wrapping_add(right_width),
                inner_y.wrapping_add(max_height),
                a.candidate_selected_text_color,
            );
        }
        font.set_position(text_x, inner_y);
        font.set_color(input.candidate_color);
        font.draw(reading, out);
        if left_width >= 0 {
            font.set_position(text_x.wrapping_add(left_width), inner_y);
            font.set_color(inverse);
            let sel = selection.unwrap_or(0);
            let selected = reading.get(sel..=sel).unwrap_or(&[]);
            font.draw(selected, out);
        }
        return;
    }
    for ci in 0..page.page_size.max(0) {
        if ci >= page.count {
            return;
        }
        let entry = page.entry(ci as usize);
        let width = widths.get(ci as usize).copied().unwrap_or(0);
        if ci == page.selection {
            font.set_color(inverse);
            if !page.show_reading && !vertical {
                out.draw_rect(
                    inner_x,
                    inner_y,
                    inner_x.wrapping_add(width),
                    inner_y.wrapping_add(max_height),
                    a.candidate_selected_text_color,
                );
            } else {
                out.draw_rect(
                    inner_x,
                    inner_y.wrapping_add(max_height.wrapping_mul(ci)),
                    inner_x.wrapping_add(outer_width).wrapping_sub(1),
                    inner_y.wrapping_add(max_height.wrapping_mul(ci + 1)),
                    a.candidate_selected_text_color,
                );
            }
        } else {
            font.set_color(input.candidate_color);
        }
        if !entry.is_empty() {
            if !page.show_reading && !vertical {
                font.set_position(inner_x.wrapping_add(half(space_width)), inner_y);
            } else {
                font.set_position(
                    inner_x.wrapping_add(padding),
                    inner_y.wrapping_add(max_height.wrapping_mul(ci)),
                );
            }
            font.draw(entry, out);
        }
        if !page.show_reading && !vertical {
            inner_x = inner_x.wrapping_add(if chinese_simplified_id {
                width.wrapping_add(padding)
            } else {
                max_width.wrapping_add(1)
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    fn units(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    /// Eight pixels a unit, 16 high.
    #[derive(Default)]
    struct Fixed {
        color: Cell<u32>,
        at: Cell<(i32, i32)>,
    }

    impl ImeFont for Fixed {
        fn set_height(&self, _: i32) {}
        fn set_color(&self, color: u32) {
            self.color.set(color);
        }
        fn set_position(&self, x: i32, y: i32) {
            self.at.set((x, y));
        }
        fn text_extent(&self, text: &[u16]) -> (i32, i32) {
            let n = text.iter().take_while(|&&u| u != 0).count() as i32;
            if n == 0 { (0, 0) } else { (8 * n, 16) }
        }
        fn draw(&self, text: &[u16], out: &mut dyn ImeDraw) {
            let (x, y) = self.at.get();
            out.draw_string(x, y, self.color.get(), text);
        }
    }

    #[derive(Default)]
    struct Calls(Vec<String>);

    impl ImeDraw for Calls {
        fn draw_rect(&mut self, x1: i32, y1: i32, x2: i32, y2: i32, color: u32) {
            self.0.push(format!("rect {x1} {y1} {x2} {y2} {color:08x}"));
        }
        fn draw_fan(&mut self, v: &[FanVertex], _: &[u16], _: u32) {
            self.0.push(format!("fan {} {}", v[0].x, v[0].y));
        }
        fn draw_string(&mut self, x: i32, y: i32, color: u32, text: &[u16]) {
            self.0.push(format!(
                "text {x} {y} {color:08x} {}",
                String::from_utf16_lossy(text)
            ));
        }
    }

    fn input<'a>(
        appearance: &'a Appearance,
        font: &'a Fixed,
        page: &'a CandidatePage,
        fan_color: &'a dyn Fn() -> u32,
    ) -> RenderInput<'a> {
        RenderInput {
            support_level: SUPPORT_LEVEL,
            appearance,
            symbol_font: font,
            fan_text: &[],
            fan_color,
            font,
            bounding: Rect {
                left: 0,
                top: 0,
                right: 100,
                bottom: 20,
            },
            position: (10, 2),
            screen: (800, 600),
            state: 1,
            language: 0x0409_0409,
            symbol: &[0x41],
            ime_id: 0,
            composition: &[],
            attributes: &[],
            client_caret: 0,
            composition_cursor: 0,
            candidates: page,
            vertical: true,
            tick: 0,
            caret_blink: 500,
            candidate_color: 0xFF00_0000,
            composition_color: 0xFF00_0000,
            backgrounds: appearance.composition_backgrounds(2),
        }
    }

    /// The toolbar's corners (`SymbolPosition` 1 to 4; the client's
    /// appearance has 0, beside the box), and no place for another value.
    #[test]
    fn the_toolbar_goes_to_its_corner() {
        let font = Fixed::default();
        let page = CandidatePage::default();
        let white = || 0xFFFF_FFFF;
        for (position, x, y) in [
            (1, 16.0, 16.0),
            (2, 784.0, 16.0),
            (3, 784.0, 584.0),
            (4, 16.0, 584.0),
            (7, 0.0, 0.0),
        ] {
            let appearance = Appearance {
                symbol_position: position,
                ..Appearance::client()
            };
            let mut calls = Calls::default();
            render_toolbar(
                &mut RenderState::default(),
                &input(&appearance, &font, &page, &white),
                &mut calls,
            );
            assert_eq!(calls.0[0], format!("fan {x} {y}"), "position {position}");
        }
    }

    /// `ImeUi_UpdateCandidateList` for the Simplified Chinese IME with a
    /// known id: the candidates that fit 18 units around the selection.
    #[test]
    fn the_simplified_chinese_page_holds_18_units() {
        let list = CandidateList {
            selection: 4,
            page_size: 9,
            strings: ["aaaa", "bbbb", "cccc", "dddd", "e", "ffff"]
                .iter()
                .map(|s| units(s))
                .collect(),
        };
        let page = CandidatePage::update(0x0804_0804, 0x0402_0804, true, &list);
        // Three of five units fit; the fourth starts a page, which holds
        // the selection and runs to the end.
        assert_eq!(page.page_size, 3);
        assert_eq!(page.selection, 1);
        assert_eq!(page.entries[0], units("1 dddd"));
        assert_eq!(page.entries[2], units("3 ffff"));
        // A page size of 0 starts at the first candidate.
        let list = CandidateList {
            page_size: 0,
            ..list
        };
        let page = CandidatePage::update(0x0411_0411, 0, false, &list);
        assert_eq!((page.page_size, page.selection), (0, 4));
        assert!(page.entries.iter().all(Vec::is_empty));
        assert_eq!(format_candidate(12, &units("x"), false), units("2x "));
    }

    /// The reading window: the reading, its selected unit framed and drawn
    /// in the inverse colour.
    #[test]
    fn the_reading_window_frames_its_selection() {
        let font = Fixed::default();
        let page = CandidatePage {
            count: 1,
            page_size: 1,
            entries: vec![units("x"); 10],
            show_reading: true,
            horizontal_reading: true,
            reading: units("abc"),
            reading_selection: 1,
            ..CandidatePage::default()
        };
        let appearance = Appearance::client();
        let white = || 0xFFFF_FFFF;
        let mut calls = Calls::default();
        render(
            &mut RenderState::default(),
            &input(&appearance, &font, &page, &white),
            true,
            true,
            &mut calls,
        );
        let tail: Vec<&str> = calls.0.iter().map(String::as_str).skip(2).collect();
        assert_eq!(
            tail,
            [
                "rect 10 18 44 36 ff000000",
                "rect 11 19 43 35 ffffffff",
                "rect 23 19 31 35 ff000000",
                "text 15 19 ff000000 abc",
                "text 23 19 feffffff b",
            ]
        );
    }
}
