//! The client's IME boxes and their draw (`fl_ime_input_box.cpp`,
//! `client_ime_focus.cpp`): `FLImeInputBox`, the box a rich-text edit
//! window takes keys through ([`ClientImeBox`], the window's
//! [`ImeBox`](super::services::ImeBox)), and `ClientIMEFocus`, the focus
//! manager whose `DrawRect` and `DrawTextW` draw the IME's rectangles and
//! the toolbar's fan ([`ImeBoxes`]).
//!
//! The C++ keeps one `IMEFocus` singleton that the boxes register in and
//! whose window procedure and draw reach them. Here [`ImeBoxes`] is a
//! shared handle (the engine keeps one, its services hand it to the edit
//! windows): it owns the focus manager (`text_input::focus::ImeFocus`),
//! the window's IME context, each box's font and font strings, and the
//! render state of `imeui` (`text_input::ime_render`). What a box hands
//! back through `UpdatedText` goes to its window: directly as the result
//! of the window's call, or, for another box (a focus move that finalises
//! the old box's composition), through [`ImeBoxes::take_owner_texts`],
//! which the engine applies after the call (divergence D553).
//!
//! The draw goes through [`ImeOverlay`], the immediate-mode layer, the
//! screen-quad batcher, the font atlas and the font strings of the
//! engine's IME pass (`ui::win3d_renderables::ime_pass_begin`).

use std::cell::RefCell;
use std::rc::Rc;

use neuron_core::num::trunc_to_i32;

use crate::text::font::quad::{ScreenQuadRecord, clip_1d};
use crate::text::font::string::FontStringChild;
use crate::text::font::{FontId, LogFont};
use crate::text_input::edit_box::EditBox;
use crate::text_input::focus::{BoxId, ImeFocus};
use crate::text_input::ime::{ImeContext, NoIme};
use crate::text_input::ime_font::{FanVertex, ImeDraw, ImeUiFont, SharedFontBackend, SymbolFont};
use crate::text_input::ime_render::{
    Appearance, CandidatePage, LanguageState, RenderInput, RenderState, SUPPORT_LEVEL, render,
};
use crate::ui::quad_batch::{DEFAULT_FRAME, Frame};
use crate::ui::services::{ImeBox, UiServices};
use crate::ui::tree::NodeId;
use crate::ui::win3d_immediate::primitive;
use crate::ui::win3d_renderables::{ime_background_key, ime_text_key};
use crate::window::Rect;

/// Which of the client's font strings.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StringRole {
    /// `ClientIMEFocus::ConfirmText`, the toolbar's text.
    Confirm,
    /// `FLImeInputBox::TextString`, the text `DrawString` paints.
    Text,
    /// `FLImeInputBox::CompositionString`, the box's text, measured for
    /// the left shift of every IME draw.
    Composition,
}

/// One of the client's font strings: a `FontStringChild` bound, on first
/// use, to "ConfirmPlayerText" without a shadow.
#[derive(Clone, Debug)]
pub struct ImeString {
    pub role: StringRole,
    pub child: FontStringChild,
    /// The font it is bound to (`ClientConfirmPlayerFont`).
    pub font: Option<FontId>,
    /// Bound yet (the C++ binds it in the constructor; here at the first
    /// use, when the font system is at hand).
    pub bound: bool,
}

impl ImeString {
    #[must_use]
    pub fn new(role: StringRole, font: Option<FontId>) -> ImeString {
        ImeString {
            role,
            child: FontStringChild::new(),
            font,
            bound: false,
        }
    }
}

/// What the IME pass draws with.
pub trait ImeOverlay {
    /// `Win3DBegin`.
    fn begin(&mut self, primitive: u32);
    /// `Win3DVertex3f`.
    fn vertex(&mut self, x: f32, y: f32, z: f32);
    /// `Win3DColor3ub`.
    fn color3ub(&mut self, r: u8, g: u8, b: u8);
    /// `Win3DEnd`.
    fn end(&mut self);
    /// `ScreenQuadSetMaterialKey`.
    fn set_quad_key(&mut self, key: u32);
    /// `ScreenQuadBeginBatchFrame`.
    fn begin_quad_frame(&mut self, frame: Frame, blend_mode: i32, find: bool);
    /// `ScreenQuadAppendRecord`.
    fn append_quad(&mut self, record: ScreenQuadRecord);
    /// `ScreenQuadFlush` of one material list.
    fn flush_quads(&mut self, key: u32);
    /// `FontAtlasMaterialKey`.
    fn atlas_key(&mut self) -> u32;
    /// `FontAtlasSetMaterialKey`.
    fn set_atlas_key(&mut self, key: u32);
    /// `FontAtlasSetDrawColor`.
    fn set_draw_color(&mut self, packed: u32);
    /// A font string's `SetText`.
    fn string_text(&mut self, string: &mut ImeString, text: &[u16]);
    /// A font string's `GetSize`.
    fn string_size(&mut self, string: &mut ImeString) -> (i32, i32);
    /// A font string's `Paint` at an origin, no trims.
    fn string_paint(&mut self, string: &mut ImeString, x: f32, y: f32);
    /// A font string's `Release`.
    fn string_release(&mut self, string: &mut ImeString);
    /// `G_RenderSurfaceWidth`, `G_RenderSurfaceHeight`.
    fn surface(&self) -> (i32, i32);
}

/// `PackDrawColorBytes`: `AARRGGBB` to the bytes R, G, B, A in memory.
#[must_use]
pub fn pack_draw_color(color: u32) -> u32 {
    let [b, g, r, a] = color.to_le_bytes();
    u32::from_le_bytes([r, g, b, a])
}

/// The value of an unsigned 32-bit sum as the C++'s `FILD` of a
/// zero-extended qword converts it.
fn unsigned_f32(value: u32) -> f32 {
    f64::from(value) as f32
}

/// `ClientIMEFocus::DrawRect`: the rectangle moved left by the focused
/// box's text width, on the "IMEbkground" list, clipped to the surface.
pub fn client_draw_rect(
    overlay: &mut dyn ImeOverlay,
    composition: &mut ImeString,
    rect: [i32; 4],
    color: u32,
) {
    let (width, _) = overlay.string_size(composition);
    let left = rect[0].wrapping_sub(width);
    let right = rect[2].wrapping_sub(width);
    overlay.set_quad_key(ime_background_key());
    let (mut x0, mut y0) = (left as f32, rect[1] as f32);
    let (mut x1, mut y1) = (right as f32, rect[3] as f32);
    let (mut u0, mut v0, mut u1, mut v1) = (0.0f32, 0.0f32, 1.0f32, 1.0f32);
    let span_x = (f64::from(x1) - f64::from(x0) + 1.0) as f32;
    let span_y = (f64::from(y1) - f64::from(y0) + 1.0) as f32;
    let (w, h) = overlay.surface();
    if clip_1d(
        &mut x0,
        &mut x1,
        &mut u0,
        &mut u1,
        0.0,
        w.wrapping_sub(1) as f32,
        span_x,
    ) == 0
    {
        return;
    }
    if clip_1d(
        &mut y0,
        &mut y1,
        &mut v0,
        &mut v1,
        0.0,
        h.wrapping_sub(1) as f32,
        span_y,
    ) == 0
    {
        return;
    }
    overlay.begin_quad_frame(DEFAULT_FRAME, 0, true);
    overlay.append_quad(ScreenQuadRecord {
        x0,
        y0,
        x1,
        y1,
        depth_a: 1.0,
        depth_b: 1.0,
        u0,
        v0,
        u1,
        v1,
        color,
    });
}

/// `ClientIMEFocus::DrawTextW`: a triangle fan from the first vertex over
/// each later vertex and the one after it (the last closing on the
/// second), each later vertex's colour first; then the text centred on
/// the first vertex in "ConfirmPlayerText" on the "IMEtext" list.
pub fn client_draw_text(
    overlay: &mut dyn ImeOverlay,
    confirm: &mut ImeString,
    text: &[u16],
    color: u32,
    vertices: &[FanVertex],
) {
    let Some(first) = vertices.first() else {
        return;
    };
    overlay.begin(primitive::TRIANGLE_FAN);
    overlay.vertex(first.x, first.y, 0.0);
    let count = vertices.len();
    for edi in 1..count {
        let [b, g, r, _] = vertices[edi].color.to_le_bytes();
        overlay.color3ub(r, g, b);
        overlay.vertex(vertices[edi].x, vertices[edi].y, 0.0);
        let next = if edi == count - 1 { 1 } else { edi + 1 };
        overlay.vertex(vertices[next].x, vertices[next].y, 0.0);
    }
    overlay.end();
    let previous = overlay.atlas_key();
    overlay.set_atlas_key(ime_text_key());
    let x = trunc_to_i32(f64::from(first.x));
    let y = trunc_to_i32(f64::from(first.y));
    overlay.set_draw_color(pack_draw_color(color));
    overlay.string_text(confirm, text);
    let (w, h) = overlay.string_size(confirm);
    let ox = x.cast_unsigned().wrapping_sub(w.cast_unsigned() >> 1);
    let oy = y.cast_unsigned().wrapping_sub(h.cast_unsigned() >> 1);
    overlay.string_paint(confirm, unsigned_f32(ox), unsigned_f32(oy));
    overlay.set_atlas_key(previous);
}

/// `FLImeInputBox::DrawString`: the text in the box's font string on the
/// "IMEtext" list, moved left by the box's text width and by the margins
/// (the sums converted unsigned), then both IME lists flushed.
pub fn box_draw_string(
    overlay: &mut dyn ImeOverlay,
    text_string: &mut ImeString,
    composition: &mut ImeString,
    margins: (u32, u32),
    at: (i32, i32),
    color: u32,
    text: &[u16],
) {
    let previous = overlay.atlas_key();
    overlay.set_atlas_key(ime_text_key());
    let (width, _) = overlay.string_size(composition);
    let left = at.0.wrapping_sub(width);
    overlay.set_draw_color(pack_draw_color(color));
    overlay.string_text(text_string, text);
    let x = margins.0.wrapping_add(left.cast_unsigned());
    let y = margins.1.wrapping_add(at.1.cast_unsigned());
    overlay.string_paint(text_string, unsigned_f32(x), unsigned_f32(y));
    overlay.set_atlas_key(previous);
    overlay.flush_quads(ime_background_key());
    overlay.flush_quads(ime_text_key());
}

/// A registered client box.
struct ClientBox {
    id: BoxId,
    owner: NodeId,
    font: Rc<ImeUiFont>,
    text: ImeString,
    composition: ImeString,
    /// The text `UpdatedText` handed the composition string last, not yet
    /// set into it.
    composed: Option<Vec<u16>>,
}

struct ImeHub {
    focus: ImeFocus,
    context: Box<dyn ImeContext>,
    backend: SharedFontBackend,
    /// "ConfirmPlayerText" and its `LOGFONTA` (`ClientConfirmPlayerFont`).
    confirm_font: Option<FontId>,
    log_font: Option<LogFont>,
    boxes: Vec<ClientBox>,
    confirm: ImeString,
    symbol: SymbolFont,
    language: LanguageState,
    render: RenderState,
    owner_texts: Vec<(NodeId, Vec<u16>)>,
    released: Vec<ImeString>,
}

impl ImeHub {
    fn client_box(&mut self, id: BoxId) -> Option<&mut ClientBox> {
        self.boxes.iter_mut().find(|b| b.id == id)
    }

    /// The boxes' `UpdatedText` since the last call: `direct`'s text is
    /// returned, every other box's is queued for its window.
    fn collect(&mut self, direct: Option<BoxId>) -> Option<Vec<u16>> {
        let mut result = None;
        for i in 0..self.boxes.len() {
            let id = self.boxes[i].id;
            let Some(text) = self.focus.take_updated_text(id) else {
                continue;
            };
            self.boxes[i].composed = Some(text.clone());
            if Some(id) == direct {
                result = Some(text);
            } else {
                self.owner_texts.push((self.boxes[i].owner, text));
            }
        }
        result
    }
}

/// The client's IME boxes: the `IMEFocus` singleton with its window and
/// boxes. Clones share it.
#[derive(Clone)]
pub struct ImeBoxes(Rc<RefCell<ImeHub>>);

impl std::fmt::Debug for ImeBoxes {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let hub = self.0.borrow();
        f.debug_struct("ImeBoxes")
            .field("boxes", &hub.focus.len())
            .field("focus", &hub.focus.focus())
            .finish_non_exhaustive()
    }
}

impl ImeBoxes {
    /// The focus manager on `context` (its input context closed), whose
    /// boxes measure with `backend`.
    #[must_use]
    pub fn new(mut context: Box<dyn ImeContext>, backend: SharedFontBackend) -> ImeBoxes {
        let focus = ImeFocus::new(context.as_mut());
        ImeBoxes(Rc::new(RefCell::new(ImeHub {
            focus,
            context,
            backend,
            confirm_font: None,
            log_font: None,
            boxes: Vec::new(),
            confirm: ImeString::new(StringRole::Confirm, None),
            symbol: SymbolFont::default(),
            language: LanguageState::default(),
            render: RenderState::default(),
            owner_texts: Vec::new(),
            released: Vec::new(),
        })))
    }

    /// A focus manager with no IME (`NoIme`) over `backend`.
    #[must_use]
    pub fn without_ime(backend: SharedFontBackend) -> ImeBoxes {
        ImeBoxes::new(Box::new(NoIme), backend)
    }

    /// The window's IME context: the window's input context is closed and
    /// told whether boxes are registered.
    pub fn set_context(&self, mut context: Box<dyn ImeContext>) {
        let mut hub = self.0.borrow_mut();
        context.set_open(false);
        context.set_boxes_registered(!hub.focus.is_empty());
        hub.context = context;
    }

    /// `ClientConfirmPlayerFont`: the font the strings bind and the
    /// `LOGFONTA` new boxes take (`GetLogFont`).
    pub fn set_confirm_font(&self, font: Option<FontId>, log_font: Option<LogFont>) {
        let mut hub = self.0.borrow_mut();
        hub.confirm_font = font;
        hub.log_font = log_font;
        hub.confirm.font = font;
    }

    /// `IsImeAvailable`: the C++ client has an `IMEFocus` only then.
    #[must_use]
    pub fn available(&self) -> bool {
        self.0.borrow().context.available()
    }

    /// `NumInputBoxes`.
    #[must_use]
    pub fn len(&self) -> usize {
        self.0.borrow().focus.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// `GetFocus`.
    #[must_use]
    pub fn focus(&self) -> Option<BoxId> {
        self.0.borrow().focus.focus()
    }

    /// Runs `f` on a registered box (read-only).
    pub fn with_box<R>(&self, id: BoxId, f: impl FnOnce(&EditBox) -> R) -> Option<R> {
        self.0.borrow().focus.get(id).map(f)
    }

    /// Runs `f` on a registered box; what it asks of the IME is applied
    /// and its `UpdatedText` queued for its window.
    pub fn with_box_mut<R>(&self, id: BoxId, f: impl FnOnce(&mut EditBox) -> R) -> Option<R> {
        let mut hub = self.0.borrow_mut();
        let ImeHub { focus, context, .. } = &mut *hub;
        let result = focus.edit(id, context.as_mut(), f);
        hub.collect(None);
        result
    }

    /// A box's font (`IMEInputBox::m_pFont`).
    #[must_use]
    pub fn font(&self, id: BoxId) -> Option<Rc<ImeUiFont>> {
        self.0
            .borrow_mut()
            .client_box(id)
            .map(|b| Rc::clone(&b.font))
    }

    /// `IMEFocus::SetFocus(box, force)`.
    pub fn set_focus(&self, id: Option<BoxId>, force: bool) {
        let mut hub = self.0.borrow_mut();
        let ImeHub { focus, context, .. } = &mut *hub;
        focus.set_focus(id, force, context.as_mut());
        hub.collect(None);
    }

    /// The window a box writes its text into.
    #[must_use]
    pub fn owner(&self, id: BoxId) -> Option<NodeId> {
        self.0.borrow_mut().client_box(id).map(|b| b.owner)
    }

    /// `new FLImeInputBox(owner)` while `IMEFocus::GetSingleton()` is set:
    /// `None` without an IME.
    #[must_use]
    pub fn create_box(&self, owner: NodeId) -> Option<Box<dyn ImeBox>> {
        if !self.available() {
            return None;
        }
        Some(Box::new(self.create_client_box(owner)))
    }

    /// `FLImeInputBox`'s constructor, whatever the IME: the box's font
    /// (`CImeUiFont`), the box registered (it takes the focus when none
    /// has it), the `LOGFONTA` of "ConfirmPlayerText" set (`SetFont`), the
    /// two font strings.
    #[must_use]
    pub fn create_client_box(&self, owner: NodeId) -> ClientImeBox {
        let mut hub = self.0.borrow_mut();
        let font = Rc::new(ImeUiFont::new(Box::new(hub.backend.clone())));
        let edit = EditBox::new(Box::new(Rc::clone(&font)));
        let ImeHub {
            focus,
            context,
            log_font,
            confirm_font,
            boxes,
            ..
        } = &mut *hub;
        let id = focus.register(edit, context.as_mut());
        // Without "ConfirmPlayerText" the C++ reads its LOGFONTA through a
        // null font; the box keeps its zeroed one (D554).
        if let Some(log_font) = *log_font {
            focus.edit(id, context.as_mut(), |b| {
                font.set_log_font(&log_font);
                b.reset_caret();
            });
        }
        boxes.push(ClientBox {
            id,
            owner,
            font,
            text: ImeString::new(StringRole::Text, *confirm_font),
            composition: ImeString::new(StringRole::Composition, *confirm_font),
            composed: None,
        });
        hub.collect(None);
        ClientImeBox {
            hub: Rc::clone(&self.0),
            id,
        }
    }

    /// `IMEFocus::SetFocus(nullptr, false)`.
    pub fn clear_focus(&self) {
        let mut hub = self.0.borrow_mut();
        let ImeHub { focus, context, .. } = &mut *hub;
        focus.set_focus(None, false, context.as_mut());
        hub.collect(None);
    }

    /// `IMEFocus::OnChar`: a unit of the IME's result string for the
    /// focused box.
    pub fn on_char(&self, unit: u16) -> bool {
        let mut hub = self.0.borrow_mut();
        let ImeHub { focus, context, .. } = &mut *hub;
        let taken = focus.on_char(unit, context.as_mut());
        hub.collect(None);
        taken
    }

    /// `IMEFocus::OnKeyDown`.
    pub fn on_key_down(&self, key: u32) -> bool {
        let mut hub = self.0.borrow_mut();
        let ImeHub { focus, context, .. } = &mut *hub;
        let taken = focus.on_key_down(key, context.as_mut());
        hub.collect(None);
        taken
    }

    /// `IMEFocus::OnSetFocus` (`gained`) and `OnKillFocus`.
    pub fn on_window_focus(&self, gained: bool) {
        let mut hub = self.0.borrow_mut();
        let ImeHub { focus, context, .. } = &mut *hub;
        if gained {
            focus.on_window_focus_gained(context.as_mut());
        } else {
            focus.on_window_focus_lost(context.as_mut());
        }
        hub.collect(None);
    }

    /// The texts boxes handed their windows outside the windows' own calls
    /// (`UpdatedText` into `Owner->SetText`), oldest first.
    #[must_use]
    pub fn take_owner_texts(&self) -> Vec<(NodeId, Vec<u16>)> {
        std::mem::take(&mut self.0.borrow_mut().owner_texts)
    }

    /// `IMEFocus::Draw(0)`: the boxes' own draw (empty in the client), then
    /// `ImeUi_Render(true, true)` while the focused box has the IME on:
    /// the toolbar, the composition and the candidate list the IME
    /// context hands the game (decision T7: on Windows the system draws
    /// the last two, D283). The IME's state, the layout's orientation and
    /// symbol are read from the context at the draw, the screen is the
    /// surface (D552).
    pub fn draw(&self, overlay: &mut dyn ImeOverlay) {
        let mut hub = self.0.borrow_mut();
        let hub = &mut *hub;
        for mut s in hub.released.drain(..) {
            overlay.string_release(&mut s);
        }
        for b in &mut hub.boxes {
            if let Some(text) = b.composed.take() {
                overlay.string_text(&mut b.composition, &text);
            }
        }
        let view = hub.context.view();
        hub.language.check(view.language, view.ime_id);
        let Some(id) = hub.focus.focus() else {
            return;
        };
        let Some((rect, position, margins, enabled)) = hub
            .focus
            .get(id)
            .map(|b| (b.rect(), b.caret().position(), b.margins(), b.has_focus()))
        else {
            return;
        };
        if !enabled {
            return;
        }
        let ImeHub {
            boxes,
            confirm,
            symbol,
            language,
            render: state,
            ..
        } = hub;
        let Some(client) = boxes.iter_mut().find(|b| b.id == id) else {
            return;
        };
        let appearance = Appearance::client();
        let (candidate_color, composition_color) = appearance.font_colors(0);
        let composition = view.composition.unwrap_or_default();
        let attributes: Vec<u8> = composition.attributes.iter().map(|a| a.byte()).collect();
        let candidates = view.candidates.map_or_else(CandidatePage::default, |list| {
            CandidatePage::update(view.language, view.ime_id, language.vertical, &list)
        });
        let fan_text = symbol.symbol();
        let symbol_color = || symbol.color();
        let font = Rc::clone(&client.font);
        let cursor = composition.cursor as i32;
        let input = RenderInput {
            support_level: SUPPORT_LEVEL,
            appearance: &appearance,
            symbol_font: symbol,
            fan_text: &fan_text,
            fan_color: &symbol_color,
            font: font.as_ref(),
            bounding: rect,
            position,
            screen: overlay.surface(),
            state: view.state,
            language: view.language,
            symbol: &language.symbol,
            ime_id: view.ime_id,
            composition: &composition.text,
            attributes: &attributes,
            client_caret: cursor,
            composition_cursor: cursor,
            candidates: &candidates,
            vertical: language.vertical,
            tick: view.tick,
            caret_blink: view.caret_blink,
            candidate_color,
            composition_color,
            backgrounds: appearance.composition_backgrounds(SUPPORT_LEVEL),
        };
        let mut draw = ClientDraw {
            overlay,
            confirm,
            text: &mut client.text,
            composition: &mut client.composition,
            margins,
        };
        render(state, &input, true, true, &mut draw);
    }
}

/// The services' IME calls over [`ImeBoxes`]: the engine's default
/// services, and what the game's services forward to.
#[derive(Clone, Debug)]
pub struct ImeServices(pub ImeBoxes);

impl UiServices for ImeServices {
    fn create_ime_box(&mut self, owner: NodeId) -> Option<Box<dyn ImeBox>> {
        self.0.create_box(owner)
    }

    fn clear_ime_focus(&mut self) {
        self.0.clear_focus();
    }
}

/// The client's `GDrawRect`, `GDrawFan` and `DrawString` for the focused
/// box.
struct ClientDraw<'a> {
    overlay: &'a mut dyn ImeOverlay,
    confirm: &'a mut ImeString,
    text: &'a mut ImeString,
    composition: &'a mut ImeString,
    margins: (u32, u32),
}

impl ImeDraw for ClientDraw<'_> {
    fn draw_rect(&mut self, x1: i32, y1: i32, x2: i32, y2: i32, color: u32) {
        client_draw_rect(self.overlay, self.composition, [x1, y1, x2, y2], color);
    }

    fn draw_fan(&mut self, vertices: &[FanVertex], text: &[u16], color: u32) {
        client_draw_text(self.overlay, self.confirm, text, color, vertices);
    }

    fn draw_string(&mut self, x: i32, y: i32, color: u32, text: &[u16]) {
        box_draw_string(
            self.overlay,
            self.text,
            self.composition,
            self.margins,
            (x, y),
            color,
            text,
        );
    }
}

/// `FLImeInputBox`: a rich-text edit window's IME box. Dropping it removes
/// it from the focus manager (`~IMEInputBox`).
pub struct ClientImeBox {
    hub: Rc<RefCell<ImeHub>>,
    id: BoxId,
}

impl std::fmt::Debug for ClientImeBox {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ClientImeBox")
            .field("id", &self.id)
            .finish_non_exhaustive()
    }
}

impl ClientImeBox {
    /// The box in the focus manager.
    #[must_use]
    pub fn id(&self) -> BoxId {
        self.id
    }

    /// Runs `f` on the box: its result, and the text its `UpdatedText`
    /// handed the window (other boxes' are queued).
    pub fn edit<R>(&mut self, f: impl FnOnce(&mut EditBox) -> R) -> (Option<R>, Option<Vec<u16>>) {
        let mut hub = self.hub.borrow_mut();
        let ImeHub { focus, context, .. } = &mut *hub;
        let result = focus.edit(self.id, context.as_mut(), f);
        let text = hub.collect(Some(self.id));
        (result, text)
    }
}

impl ImeBox for ClientImeBox {
    /// `CInputBox::HandleKey`; its `UpdatedText` is the window's new text.
    fn handle_key(&mut self, key: u32) -> Option<Vec<u16>> {
        self.edit(|b| b.handle_key(key)).1
    }

    /// `HandleWideChar` of the typed unit (the C++ hands the box the low
    /// byte through `HandleMultiByteChar`, D286).
    fn handle_char(&mut self, unit: u16) -> Option<Vec<u16>> {
        self.edit(|b| b.handle_wide_char(unit)).1
    }

    /// `SetInputBufferSize`, then `FLImeInputBox::Resize`: the box is the
    /// window's size at 0, 0.
    fn resize(&mut self, buffer_size: u32, rect: [i32; 4]) {
        self.edit(|b| {
            b.set_input_buffer_size(buffer_size);
            b.resize(Rect {
                left: 0,
                top: 0,
                right: rect[2],
                bottom: rect[3],
            });
        });
    }

    /// `IMEFocus::SetFocus(box, false)`.
    fn take_focus(&mut self) {
        let mut hub = self.hub.borrow_mut();
        let ImeHub { focus, context, .. } = &mut *hub;
        focus.set_focus(Some(self.id), false, context.as_mut());
        hub.collect(None);
    }
}

impl Drop for ClientImeBox {
    /// `~FLImeInputBox`: the strings released; `~IMEInputBox`: the box
    /// removed (with the focus, its composition finalised into it, which
    /// no longer reaches the window: the C++ object is an `IMEInputBox` by
    /// then).
    fn drop(&mut self) {
        let Ok(mut hub) = self.hub.try_borrow_mut() else {
            return;
        };
        let hub = &mut *hub;
        let ImeHub { focus, context, .. } = &mut *hub;
        let _ = focus.remove(self.id, context.as_mut());
        if let Some(i) = hub.boxes.iter().position(|b| b.id == self.id) {
            let b = hub.boxes.remove(i);
            hub.released.push(b.text);
            hub.released.push(b.composition);
        }
        hub.collect(None);
    }
}
