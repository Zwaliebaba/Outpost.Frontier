//! The IME on the game window (work package P4.11), with documented IMM32
//! calls only.
//!
//! The C++ (`imeui`) draws the composition string and the candidate list
//! itself, reads reading strings out of IME internals and probes IME
//! DLLs. Here the system draws its composition and candidate windows (the
//! game draws the IME's toolbar, `ui::ime`, from this window's state; the
//! window hands the game none of the messages its own composition,
//! candidate list and reading window would be drawn from, D283) and
//! this module places them at the focused edit box's caret
//! (`ImmSetCompositionWindow`, `ImmSetCandidateWindow`) when a composition
//! starts or changes, a candidate list opens, or the caret moves. The
//! window keeps a copy of the composition (`ImmGetCompositionStringW`) for
//! the UI toolkit. The IME's result string reaches the window as
//! `WM_IME_CHAR`; the window queues each unit for the game's router, which
//! hands it to the focused box as the C++'s `GSendChar` did, instead of
//! letting it become a `WM_CHAR`.
//!
//! As in the C++, the IME acts only while an edit box is registered, and
//! the window has its input context only while a box has the focus
//! (`ImmAssociateContext`).

use std::collections::VecDeque;
use std::ffi::c_void;

use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Storage::FileSystem::{
    GetFileVersionInfoSizeW, GetFileVersionInfoW, VS_FIXEDFILEINFO, VerQueryValueW,
};
use windows::Win32::System::SystemInformation::GetTickCount;
use windows::Win32::UI::Input::Ime::{
    CANDIDATEFORM, CFS_EXCLUDE, CFS_POINT, COMPOSITIONFORM, CPS_CANCEL, GCS_COMPATTR, GCS_COMPSTR,
    GCS_CURSORPOS, GCS_RESULTSTR, HIMC, IME_COMPOSITION_STRING, IME_CONVERSION_MODE,
    IMN_CHANGECANDIDATE, IMN_OPENCANDIDATE, IMN_SETCONVERSIONMODE, IMN_SETOPENSTATUS,
    ImmAssociateContext, ImmGetCompositionStringW, ImmGetContext, ImmGetConversionStatus,
    ImmGetIMEFileNameW, ImmGetOpenStatus, ImmGetVirtualKey, ImmIsIME, ImmNotifyIME,
    ImmReleaseContext, ImmSetCandidateWindow, ImmSetCompositionWindow, NI_CLOSECANDIDATE,
    NI_COMPOSITIONSTR, NOTIFY_IME_INDEX,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetFocus, GetKeyboardLayout, GetKeyboardLayoutList, HKL,
};
use windows::Win32::UI::WindowsAndMessaging::{
    GetCaretBlinkTime, MSG, PostMessageW, WM_IME_CHAR, WM_IME_COMPOSITION, WM_IME_ENDCOMPOSITION,
    WM_IME_NOTIFY, WM_IME_STARTCOMPOSITION, WM_INPUTLANGCHANGE, WM_KEYDOWN, WM_KEYUP,
    WM_SYSKEYDOWN, WM_SYSKEYUP,
};
use windows::core::PCWSTR;

use crate::input::keyboard::vk;
use crate::platform::input::{client_mapping, key_down, key_down_now};
use crate::platform::window::{push, with_state};
use crate::text_input::ime::{
    CaretRect, Composition, HeldComposition, ImeContext, ImeView, ModifierRepair, ime_state,
    is_hot_key,
};
use crate::text_input::ime_core::ImeFile;
use crate::window::{ImeEvent, WindowEvent};

/// The window's IME state.
#[derive(Debug, Default)]
pub(crate) struct ImeWindowState {
    /// The window's own input context, taken when it was created; none
    /// when the thread has no IME support.
    context: HIMC,
    boxes: bool,
    open: bool,
    caret: Option<CaretRect>,
    composition: Composition,
    repair: ModifierRepair,
    /// The composition taken when the window lost the focus (D284).
    held: HeldComposition,
    /// Result-string units for the router (`WM_IME_CHAR`).
    pub chars: VecDeque<u16>,
}

impl ImeWindowState {
    /// The state for a new window: its default input context.
    pub fn capture(hwnd: HWND) -> ImeWindowState {
        // SAFETY: the live window; the context is released at once and
        // kept only as a value to associate later, as the C++ keeps it.
        let context = unsafe {
            let context = ImmGetContext(hwnd);
            if !context.is_invalid() {
                let _ = ImmReleaseContext(hwnd, context);
            }
            context
        };
        ImeWindowState {
            context,
            ..ImeWindowState::default()
        }
    }
}

/// A context of the window, released when dropped.
struct Context {
    hwnd: HWND,
    himc: HIMC,
}

impl Context {
    fn get(hwnd: HWND) -> Option<Context> {
        // SAFETY: a plain query of the window's context.
        let himc = unsafe { ImmGetContext(hwnd) };
        (!himc.is_invalid()).then_some(Context { hwnd, himc })
    }

    /// `ImmGetCompositionStringW` into a byte buffer.
    fn string(&self, kind: IME_COMPOSITION_STRING) -> Vec<u8> {
        // SAFETY: the size query passes no buffer.
        let size = unsafe { ImmGetCompositionStringW(self.himc, kind, None, 0) };
        let Ok(size) = usize::try_from(size) else {
            return Vec::new();
        };
        let mut bytes = vec![0u8; size];
        // SAFETY: the buffer has the size the query returned.
        let read = unsafe {
            ImmGetCompositionStringW(
                self.himc,
                kind,
                Some(bytes.as_mut_ptr().cast::<c_void>()),
                size as u32,
            )
        };
        bytes.truncate(usize::try_from(read).unwrap_or(0));
        bytes
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        // SAFETY: the context this value got from the window.
        let _ = unsafe { ImmReleaseContext(self.hwnd, self.himc) };
    }
}

/// Whether any installed keyboard layout is an IME (`CheckAnyIME`): the
/// C++ client builds its `IMEFocus` only then.
#[must_use]
pub fn ime_available() -> bool {
    // SAFETY: the count query passes no buffer.
    let count = unsafe { GetKeyboardLayoutList(None) };
    let Ok(count) = usize::try_from(count) else {
        return false;
    };
    let mut layouts = vec![HKL::default(); count];
    // SAFETY: a buffer of the size counted.
    let got = unsafe { GetKeyboardLayoutList(Some(&mut layouts)) };
    layouts.truncate(usize::try_from(got).unwrap_or(0));
    // SAFETY: plain queries of installed layouts.
    layouts.iter().any(|&l| unsafe { ImmIsIME(l) }.as_bool())
}

/// Places the system's composition window at the caret and its candidate
/// list below it, keeping the caret's line clear.
fn position_windows(hwnd: HWND) {
    let Some((Some(caret), mapping)) =
        with_state(|s| (s.ime.caret, client_mapping(s.window, s.mode)))
    else {
        return;
    };
    let Some(context) = Context::get(hwnd) else {
        return;
    };
    let (x, y) = mapping.to_client(caret.x, caret.y);
    let bottom = mapping
        .to_client(caret.x, caret.y.saturating_add(caret.height))
        .1;
    let composition = COMPOSITIONFORM {
        dwStyle: CFS_POINT,
        ptCurrentPos: POINT { x, y },
        rcArea: RECT::default(),
    };
    let candidate = CANDIDATEFORM {
        dwIndex: 0,
        dwStyle: CFS_EXCLUDE,
        ptCurrentPos: POINT { x, y: bottom },
        rcArea: RECT {
            left: x,
            top: y,
            right: x + 1,
            bottom,
        },
    };
    // SAFETY: the window's context and complete forms.
    unsafe {
        let _ = ImmSetCompositionWindow(context.himc, &composition);
        let _ = ImmSetCandidateWindow(context.himc, &candidate);
    }
}

/// Reads the composition after `WM_IME_COMPOSITION`. A result string
/// ends the composition; its units arrive as `WM_IME_CHAR`.
fn read_composition(hwnd: HWND, flags: u32) {
    let Some(context) = Context::get(hwnd) else {
        return;
    };
    let mut composition = None;
    if flags & GCS_COMPSTR.0 != 0 {
        let bytes = context.string(GCS_COMPSTR);
        let text: Vec<u16> = bytes
            .as_chunks::<2>()
            .0
            .iter()
            .map(|&b| u16::from_le_bytes(b))
            .collect();
        let attributes = context.string(GCS_COMPATTR);
        // SAFETY: the cursor query passes no buffer and returns the
        // position.
        let cursor = unsafe { ImmGetCompositionStringW(context.himc, GCS_CURSORPOS, None, 0) };
        composition = Some(Composition::new(text, &attributes, cursor));
    }
    drop(context);
    with_state(|s| {
        if flags & GCS_RESULTSTR.0 != 0 {
            s.ime.composition.clear();
        }
        if let Some(c) = composition {
            s.ime.composition = c;
        }
        s.events
            .push_back(WindowEvent::Ime(ImeEvent::CompositionChanged));
    });
}

/// The IME's first refusal on every message (`IMEFocus::WndProc`):
/// `Some` when it handled the message.
pub(crate) fn window_proc(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> Option<LRESULT> {
    if !with_state(|s| s.ime.boxes).unwrap_or(false) {
        return None;
    }
    match msg {
        WM_KEYDOWN | WM_SYSKEYDOWN | WM_KEYUP | WM_SYSKEYUP => {
            let post = with_state(|s| {
                s.ime
                    .repair
                    .on_key_message(msg, wparam.0 as u32, lparam.0 as u32, key_down_now)
            })
            .flatten();
            if let Some((key, lp)) = post {
                // SAFETY: plain calls; a null focus posts to the thread, as
                // the C++'s does.
                unsafe {
                    let _ = PostMessageW(
                        Some(GetFocus()),
                        WM_KEYUP,
                        WPARAM(key as usize),
                        LPARAM(lp as isize),
                    );
                }
            }
            None
        }
        WM_IME_STARTCOMPOSITION => {
            with_state(|s| {
                s.ime.composition.clear();
                s.events
                    .push_back(WindowEvent::Ime(ImeEvent::CompositionStarted));
            });
            position_windows(hwnd);
            None
        }
        WM_IME_COMPOSITION => {
            read_composition(hwnd, lparam.0 as u32);
            position_windows(hwnd);
            None
        }
        WM_IME_ENDCOMPOSITION => {
            with_state(|s| {
                s.ime.composition.clear();
                s.events
                    .push_back(WindowEvent::Ime(ImeEvent::CompositionEnded));
            });
            None
        }
        WM_IME_NOTIFY => {
            match wparam.0 as u32 {
                IMN_SETOPENSTATUS | IMN_SETCONVERSIONMODE => {
                    push(WindowEvent::Ime(ImeEvent::StateChanged));
                }
                IMN_OPENCANDIDATE | IMN_CHANGECANDIDATE => position_windows(hwnd),
                _ => {}
            }
            None
        }
        WM_INPUTLANGCHANGE => {
            push(WindowEvent::Ime(ImeEvent::StateChanged));
            None
        }
        WM_IME_CHAR => {
            with_state(|s| s.ime.chars.push_back(wparam.0 as u16));
            Some(LRESULT(0))
        }
        _ => None,
    }
}

/// `WM_KILLFOCUS` (D284): while a box has the focus, the composition is
/// held for the box and the IME's composition is cancelled at once, as
/// the C++'s `IMEFocus::OnKillFocus` commits and cancels it inside the
/// window procedure.
pub(crate) fn hold_on_kill_focus(hwnd: HWND) {
    let held = with_state(|s| {
        let open = s.ime.boxes && s.ime.open;
        s.ime.held.hold(open, &mut s.ime.composition)
    })
    .unwrap_or(false);
    if !held {
        return;
    }
    if let Some(context) = Context::get(hwnd) {
        // SAFETY: the window's context.
        unsafe {
            let _ = ImmNotifyIME(context.himc, NI_CLOSECANDIDATE, NOTIFY_IME_INDEX(4), 0);
            let _ = ImmNotifyIME(context.himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        }
    }
}

/// `IMEFocus::IgnoreHotKey`: with a box registered, a key an IME took that
/// is one of its hot keys is dropped before routing and translation.
pub(crate) fn ignore_hot_key(msg: &MSG) -> bool {
    if !matches!(msg.message, WM_KEYDOWN | WM_SYSKEYDOWN)
        || msg.wParam.0 as u32 != vk::PROCESSKEY
        || !with_state(|s| s.ime.boxes).unwrap_or(false)
    {
        return false;
    }
    // SAFETY: a plain query for the message's window.
    let key = unsafe { ImmGetVirtualKey(msg.hwnd) };
    is_hot_key(key, key_down(vk::CONTROL), key_down(vk::SHIFT))
}

/// [`ImeContext`] on the game window of this thread.
#[derive(Clone, Copy, Debug)]
pub struct WindowIme {
    hwnd: HWND,
}

impl WindowIme {
    pub(crate) fn new(hwnd: HWND) -> WindowIme {
        WindowIme { hwnd }
    }

    /// The composition in progress.
    #[must_use]
    pub fn composition(&self) -> Composition {
        with_state(|s| s.ime.composition.clone()).unwrap_or_default()
    }

    /// Whether the window has its input context (a box has the focus).
    #[must_use]
    pub fn is_open(&self) -> bool {
        with_state(|s| s.ime.open).unwrap_or(false)
    }

    /// `ImeUi_GetState`: 0 off, 1 on (native mode for Chinese), 2 for a
    /// Chinese IME in alphanumeric mode.
    #[must_use]
    pub fn state(&self) -> u32 {
        // SAFETY: plain queries of this thread's layout.
        let layout = unsafe { GetKeyboardLayout(0) };
        let language = (layout.0 as usize & 0xffff) as u16;
        // SAFETY: as above.
        let is_ime = unsafe { ImmIsIME(layout) }.as_bool();
        let Some(context) = Context::get(self.hwnd) else {
            return ime_state(false, is_ime, language, false, 0);
        };
        let mut conversion = IME_CONVERSION_MODE::default();
        // SAFETY: the window's context; an out mode, no sentence mode.
        let (open, _) = unsafe {
            (
                ImmGetOpenStatus(context.himc).as_bool(),
                ImmGetConversionStatus(context.himc, Some(&mut conversion), None),
            )
        };
        ime_state(true, is_ime, language, open, conversion.0)
    }
}

impl ImeContext for WindowIme {
    fn set_open(&mut self, open: bool) {
        let Some(context) = with_state(|s| s.ime.context) else {
            return;
        };
        // Without a context of its own the window cannot open.
        let open = open && !context.is_invalid();
        let associate = if open { context } else { HIMC::default() };
        // SAFETY: the live window and its own context, or none.
        unsafe { ImmAssociateContext(self.hwnd, associate) };
        with_state(|s| {
            s.ime.open = open;
            if open {
                s.ime.held.clear();
            }
        });
    }

    fn finalize(&mut self, send: bool) -> Vec<u16> {
        let Some(context) = Context::get(self.hwnd) else {
            return Vec::new();
        };
        let text = with_state(|s| {
            let ImeWindowState {
                held, composition, ..
            } = &mut s.ime;
            held.take(composition, send)
        })
        .unwrap_or_default();
        // SAFETY: the window's context.
        unsafe {
            let _ = ImmNotifyIME(context.himc, NI_CLOSECANDIDATE, NOTIFY_IME_INDEX(4), 0);
            let _ = ImmNotifyIME(context.himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        }
        text
    }

    fn set_caret(&mut self, caret: CaretRect) {
        let open = with_state(|s| {
            s.ime.caret = Some(caret);
            s.ime.open
        })
        .unwrap_or(false);
        if open {
            position_windows(self.hwnd);
        }
    }

    fn set_boxes_registered(&mut self, registered: bool) {
        with_state(|s| s.ime.boxes = registered);
    }

    fn available(&self) -> bool {
        ime_available()
    }

    /// The state, the layout and the clocks. The composition and the
    /// candidate list are the system's windows (decision T7, D283): the
    /// window hands the game no messages for them, so only the toolbar
    /// shows.
    fn view(&mut self) -> ImeView {
        // SAFETY: plain queries of this thread's layout and the clocks.
        let (layout, tick, caret_blink) =
            unsafe { (GetKeyboardLayout(0), GetTickCount(), GetCaretBlinkTime()) };
        ImeView {
            state: self.state(),
            language: layout.0 as usize as u32,
            tick,
            caret_blink,
        }
    }

    fn has_context(&mut self) -> bool {
        Context::get(self.hwnd).is_some()
    }

    fn layout(&mut self) -> u32 {
        // SAFETY: a plain query of this thread's layout.
        unsafe { GetKeyboardLayout(0) }.0 as usize as u32
    }

    /// The layout's IME file (`ImmGetIMEFileNameW`) and the file version
    /// of its version resource, for `GetImeId`. The IME itself is not
    /// loaded, so whether it exports `GetReadingString` is not known
    /// (D591).
    fn ime_file(&mut self) -> Option<ImeFile> {
        // SAFETY: a plain query of this thread's layout.
        let layout = unsafe { GetKeyboardLayout(0) };
        let mut name = [0u16; 1024];
        // SAFETY: the buffer and its length in units.
        let length = unsafe { ImmGetIMEFileNameW(layout, Some(&mut name[..1023])) };
        let length = usize::try_from(length)
            .ok()
            .filter(|&n| n > 0 && n < 1024)?;
        let name = &name[..length];
        let mut path = name.to_vec();
        path.push(0);
        let mut handle = 0u32;
        // SAFETY: a terminated name and an out handle.
        let size = unsafe { GetFileVersionInfoSizeW(PCWSTR(path.as_ptr()), Some(&mut handle)) };
        let version = (size != 0)
            .then(|| {
                let mut data = vec![0u8; size as usize];
                // SAFETY: a buffer of the size the query returned.
                unsafe {
                    GetFileVersionInfoW(
                        PCWSTR(path.as_ptr()),
                        Some(handle),
                        size,
                        data.as_mut_ptr().cast::<c_void>(),
                    )
                }
                .ok()?;
                let mut fixed: *mut c_void = std::ptr::null_mut();
                let mut fixed_size = 0u32;
                let root = [u16::from(b'\\'), 0];
                // SAFETY: the version data just read; the result points
                // into it and is read before it is dropped.
                let found = unsafe {
                    VerQueryValueW(
                        data.as_ptr().cast::<c_void>(),
                        PCWSTR(root.as_ptr()),
                        &mut fixed,
                        &mut fixed_size,
                    )
                }
                .as_bool();
                if !found
                    || fixed.is_null()
                    || (fixed_size as usize) < std::mem::size_of::<VS_FIXEDFILEINFO>()
                {
                    return None;
                }
                // SAFETY: a VS_FIXEDFILEINFO inside `data`, of the size
                // checked; read unaligned.
                let info = unsafe { std::ptr::read_unaligned(fixed.cast::<VS_FIXEDFILEINFO>()) };
                Some((info.dwFileVersionMS, info.dwFileVersionLS))
            })
            .flatten();
        Some(ImeFile {
            name: String::from_utf16_lossy(name),
            version,
        })
    }
}
