//! The game window and its message pump (work package P4.1; decision O27;
//! `window.cpp` and `message_pump.cpp` of the C++ client).
//!
//! The process is made per-monitor DPI aware (version 2). The window is
//! placed by [`crate::window::placement`]: a captioned window with the
//! C++'s style whose client area is the mode's size times the monitor's
//! scale, or a borderless popup over the monitor. The window procedure
//! keeps the C++'s dispositions that do not belong to the game: the
//! background is never erased, a fullscreen window answers `HTCLIENT`
//! everywhere, `WM_SYSKEYUP` and the screen saver command are swallowed,
//! and the close command becomes [`WindowEvent::CloseRequested`] instead of
//! closing. Everything it sees for the game goes into a queue that
//! [`Window::pump`] drains. One game window per thread.
//!
//! Input (P4.11) follows the C++'s window procedure and pump. The IME gets
//! first refusal on every message (`platform::ime`). Focus, activation,
//! `WM_MOUSEMOVE` and `WM_INPUT` drive the window's mouse model
//! ([`crate::input::mouse::Mouse`]) as `ClientInputOnFocusGained`,
//! `ClientInputOnFocusLost`, `MouseSetEnabled` and `ClientInputOnMouseMove`
//! do, and are still reported. [`Window::pump_routed`] first hands the
//! queued mouse events to the game's [`InputRouter`]
//! (`MouseDispatchEvents`), then for each message drops the IME's hot keys,
//! offers key messages to the router before `TranslateMessage`
//! (`KeyMapRouteInputMessage`; a consumed message is neither translated
//! nor dispatched), and after each dispatch hands it the IME's result
//! characters.

use std::cell::RefCell;
use std::collections::VecDeque;

use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    GetMonitorInfoW, MONITOR_DEFAULTTONEAREST, MONITORINFO, MonitorFromWindow, SC_SCREENSAVE,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::HiDpi::{
    AdjustWindowRectExForDpi, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, GetDpiForWindow,
    SetProcessDpiAwarenessContext,
};
use windows::Win32::UI::Input::KeyboardAndMouse::GetDoubleClickTime;
use windows::Win32::UI::Input::RAWINPUTDEVICE_FLAGS;
use windows::Win32::UI::WindowsAndMessaging::{
    CS_HREDRAW, CS_VREDRAW, CW_USEDEFAULT, CreateWindowExW, DefWindowProcW, DestroyWindow,
    DispatchMessageW, GWL_STYLE, GetMessageTime, HTCLIENT, IDC_ARROW, LoadCursorW, MSG, PM_REMOVE,
    PeekMessageW, PostQuitMessage, RegisterClassExW, SC_CLOSE, SET_WINDOW_POS_FLAGS, SW_SHOWNORMAL,
    SWP_FRAMECHANGED, SWP_NOMOVE, SWP_NOZORDER, SetWindowLongPtrW, SetWindowPos, ShowWindow,
    TranslateMessage, WINDOW_EX_STYLE, WINDOW_STYLE, WM_ACTIVATEAPP, WM_CLOSE, WM_DESTROY,
    WM_DPICHANGED, WM_ERASEBKGND, WM_INPUT, WM_KILLFOCUS, WM_MOUSEMOVE, WM_NCHITTEST, WM_QUIT,
    WM_SETFOCUS, WM_SIZE, WM_SYSCOMMAND, WM_SYSKEYUP, WNDCLASSEXW, WS_CAPTION, WS_CLIPCHILDREN,
    WS_MINIMIZEBOX, WS_POPUP, WS_SYSMENU, WS_VISIBLE,
};
use windows::core::{HSTRING, PCWSTR, w};

use crate::input::keyboard::{self, KeyEvent};
use crate::input::mouse::{Mouse, MouseEvent, MousePlatform};
use crate::platform::ime::{self, ImeWindowState, WindowIme};
use crate::platform::input::{self as platform_input, RawInputState, WinMouse};
use crate::text_input::ime::Composition;
use crate::window::{ClientMapping, DisplayMode, Placement, Rect, WindowEvent, placement};

/// The C++'s windowed style, `0x02CA0000`: clip children, caption, system
/// menu, minimise box.
pub const WINDOWED_STYLE: WINDOW_STYLE =
    WINDOW_STYLE(WS_CLIPCHILDREN.0 | WS_CAPTION.0 | WS_SYSMENU.0 | WS_MINIMIZEBOX.0);

const CLASS_NAME: PCWSTR = w!("OutpostFrontierWindow");

/// Why the window could not be made or placed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WindowError {
    pub call: &'static str,
    pub code: i32,
}

impl std::fmt::Display for WindowError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} failed (0x{:08x})", self.call, self.code)
    }
}

impl std::error::Error for WindowError {}

trait At<T> {
    fn at(self, call: &'static str) -> Result<T, WindowError>;
}

impl<T> At<T> for windows::core::Result<T> {
    fn at(self, call: &'static str) -> Result<T, WindowError> {
        self.map_err(|e| WindowError {
            call,
            code: e.code().0,
        })
    }
}

/// What the window procedure needs to know about the window.
pub(crate) struct State {
    pub window: HWND,
    pub mode: DisplayMode,
    pub events: VecDeque<WindowEvent>,
    pub mouse: Mouse,
    pub raw: RawInputState,
    pub ime: ImeWindowState,
    /// Whether the window's own Raw Input registration succeeded.
    pub raw_registered: bool,
}

thread_local! {
    static STATE: RefCell<Option<State>> = const { RefCell::new(None) };
}

/// Runs `f` on the window's state; `None` when there is no window or the
/// state is in use further up the stack (a message sent while the state
/// is borrowed is then left to the default processing).
pub(crate) fn with_state<R>(f: impl FnOnce(&mut State) -> R) -> Option<R> {
    STATE
        .try_with(|cell| {
            let mut state = cell.try_borrow_mut().ok()?;
            state.as_mut().map(f)
        })
        .ok()
        .flatten()
}

pub(crate) fn push(event: WindowEvent) {
    with_state(|s| s.events.push_back(event));
}

fn is_fullscreen() -> bool {
    with_state(|s| s.mode.fullscreen).unwrap_or(false)
}

/// Runs `f` on the mouse model with the window as its platform.
fn with_mouse<R>(f: impl FnOnce(&mut Mouse, &mut dyn MousePlatform) -> R) -> Option<R> {
    with_state(|s| {
        let mut platform = WinMouse::new(s.window, s.mode, &mut s.raw);
        f(&mut s.mouse, &mut platform)
    })
}

fn low_word(v: isize) -> u32 {
    (v as usize & 0xffff) as u32
}

fn high_word(v: isize) -> u32 {
    ((v as usize >> 16) & 0xffff) as u32
}

extern "system" fn window_proc(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    // The IME gets first refusal on every message.
    if let Some(result) = ime::window_proc(hwnd, msg, wparam, lparam) {
        return result;
    }
    match msg {
        WM_SETFOCUS => {
            with_mouse(|m, p| m.on_focus_gained(p));
            push(WindowEvent::FocusGained);
        }
        WM_KILLFOCUS => {
            ime::hold_on_kill_focus(hwnd);
            with_mouse(|m, p| m.on_focus_lost(p));
            push(WindowEvent::FocusLost);
        }
        // The renderer owns every pixel.
        WM_ERASEBKGND => return LRESULT(1),
        WM_ACTIVATEAPP => {
            let active = wparam.0 == 1;
            // `OnActivateApp`: the mouse is taken or released; the present
            // gate and the device release are the game's.
            with_mouse(|m, p| m.set_enabled(active, p));
            push(WindowEvent::Activated(active));
        }
        WM_NCHITTEST if is_fullscreen() => return LRESULT(HTCLIENT as isize),
        // Alt releases never reach the menu handling.
        WM_SYSKEYUP => return LRESULT(0),
        WM_SYSCOMMAND => {
            // The low four bits of the command are used by the system.
            let command = (wparam.0 & 0xfff0) as u32;
            if command == SC_CLOSE {
                push(WindowEvent::CloseRequested);
                return LRESULT(0);
            }
            if command == SC_SCREENSAVE {
                return LRESULT(0);
            }
        }
        WM_MOUSEMOVE => {
            // Signed client coordinates in the two low words.
            let x = i32::from(low_word(lparam.0) as u16 as i16);
            let y = i32::from(high_word(lparam.0) as u16 as i16);
            with_mouse(|m, p| m.on_mouse_move(p));
            push(WindowEvent::MouseMoved { x, y });
        }
        WM_INPUT => {
            if let Some(raw) = platform_input::read_raw_mouse(lparam) {
                // SAFETY: a plain query of the message being dispatched.
                let time = unsafe { GetMessageTime() } as u32;
                with_state(|s| {
                    let input = platform_input::decode(&mut s.raw.decoder, raw, time);
                    s.mouse.feed(&input);
                });
            }
        }
        WM_SIZE => push(WindowEvent::Resized {
            width: low_word(lparam.0),
            height: high_word(lparam.0),
        }),
        WM_DPICHANGED => {
            let dpi = low_word(wparam.0 as isize);
            if !is_fullscreen() {
                // The suggested window rectangle keeps the client area's
                // physical size proportional to the new scale.
                // SAFETY: Windows passes a valid RECT pointer with this
                // message for the duration of the call.
                let r = unsafe { *(lparam.0 as *const RECT) };
                // SAFETY: `hwnd` is the window being dispatched.
                let _ = unsafe {
                    SetWindowPos(
                        hwnd,
                        None,
                        r.left,
                        r.top,
                        r.right - r.left,
                        r.bottom - r.top,
                        SWP_NOZORDER,
                    )
                };
            }
            push(WindowEvent::DpiChanged(dpi));
            return LRESULT(0);
        }
        WM_CLOSE => {
            // SAFETY: `hwnd` is the window being dispatched.
            let _ = unsafe { DestroyWindow(hwnd) };
            return LRESULT(0);
        }
        WM_DESTROY => {
            let _ = STATE.try_with(|cell| {
                if let Ok(mut s) = cell.try_borrow_mut()
                    && let Some(state) = s.as_mut().filter(|state| state.window == hwnd)
                {
                    state.raw.release();
                    *s = None;
                }
            });
            // SAFETY: a plain call on the window's thread.
            unsafe { PostQuitMessage(0) };
            return LRESULT(0);
        }
        _ => {}
    }
    // SAFETY: the arguments are the ones Windows dispatched.
    unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) }
}

/// What a pump found.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Pump {
    /// The queue is empty; the events since the last pump.
    Events(Vec<WindowEvent>),
    /// `WM_QUIT` arrived, with its exit code, after these events.
    Quit(i32, Vec<WindowEvent>),
}

/// The game window.
pub struct Window {
    hwnd: HWND,
    mode: DisplayMode,
}

impl Window {
    /// Makes the process per-monitor DPI aware (once; later calls fail
    /// harmlessly) and creates the window for `mode`, shown.
    pub fn new(title: &str, mode: DisplayMode) -> Result<Window, WindowError> {
        // SAFETY: plain process-wide setting; it fails when already set,
        // which is not an error here.
        let _ =
            unsafe { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) };
        // SAFETY: a plain query of this module's handle.
        let instance = unsafe { GetModuleHandleW(None) }.at("GetModuleHandleW")?;
        // SAFETY: the predefined arrow cursor.
        let cursor = unsafe { LoadCursorW(None, IDC_ARROW) }.at("LoadCursorW")?;
        let class = WNDCLASSEXW {
            cbSize: size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(window_proc),
            hInstance: instance.into(),
            hCursor: cursor,
            lpszClassName: CLASS_NAME,
            ..Default::default()
        };
        // SAFETY: the description is complete; registering the class a
        // second time fails, which leaves the first registration in use.
        unsafe { RegisterClassExW(&class) };
        let title = HSTRING::from(title);
        // SAFETY: the class is registered above; the window starts hidden
        // and is placed below, before it is shown.
        let hwnd = unsafe {
            CreateWindowExW(
                WINDOW_EX_STYLE(0),
                CLASS_NAME,
                &title,
                WINDOWED_STYLE,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                None,
                None,
                Some(instance.into()),
                None,
            )
        }
        .at("CreateWindowExW")?;
        // The mouse for Raw Input, plainly until the model acquires it.
        let raw_registered = platform_input::register_mouse(hwnd, RAWINPUTDEVICE_FLAGS(0));
        // SAFETY: a plain query; the C++ latches it when it creates the
        // mouse device.
        let double_click_time = unsafe { GetDoubleClickTime() };
        let ime = ImeWindowState::capture(hwnd);
        STATE.with_borrow_mut(|s| {
            *s = Some(State {
                window: hwnd,
                mode,
                events: VecDeque::new(),
                mouse: Mouse::new(double_click_time, mode),
                raw: RawInputState::default(),
                ime,
                raw_registered,
            });
        });
        let mut window = Window { hwnd, mode };
        window.apply(mode)?;
        Ok(window)
    }

    #[must_use]
    pub fn hwnd(&self) -> HWND {
        self.hwnd
    }

    #[must_use]
    pub fn mode(&self) -> DisplayMode {
        self.mode
    }

    /// The window's DPI.
    #[must_use]
    pub fn dpi(&self) -> u32 {
        // SAFETY: a plain query of the live window.
        unsafe { GetDpiForWindow(self.hwnd) }
    }

    /// The rectangle of the monitor the window is on, in physical pixels.
    pub fn monitor(&self) -> Result<Rect, WindowError> {
        // SAFETY: plain queries of the live window's monitor, with a sized
        // out structure.
        unsafe {
            let monitor = MonitorFromWindow(self.hwnd, MONITOR_DEFAULTTONEAREST);
            let mut info = MONITORINFO {
                cbSize: size_of::<MONITORINFO>() as u32,
                ..Default::default()
            };
            if !GetMonitorInfoW(monitor, &mut info).as_bool() {
                return Err(WindowError {
                    call: "GetMonitorInfoW",
                    code: 0,
                });
            }
            let r = info.rcMonitor;
            Ok(Rect {
                left: r.left,
                top: r.top,
                right: r.right,
                bottom: r.bottom,
            })
        }
    }

    /// Restyles and places the window for `mode` and shows it
    /// (`ApplyGameWindowLayout` in the C++, with decision O27's placement).
    pub fn apply(&mut self, mode: DisplayMode) -> Result<(), WindowError> {
        self.mode = mode;
        with_state(|s| {
            s.mode = mode;
            s.mouse.set_display(mode);
        });
        let dpi = self.dpi();
        let monitor = self.monitor()?;
        let set_style = |style: WINDOW_STYLE| {
            // SAFETY: the live window; the style word only.
            unsafe { SetWindowLongPtrW(self.hwnd, GWL_STYLE, (style.0 | WS_VISIBLE.0) as isize) };
        };
        match placement(mode, dpi, monitor) {
            Placement::Borderless { rect } => {
                set_style(WS_POPUP);
                // SAFETY: the live window.
                unsafe {
                    SetWindowPos(
                        self.hwnd,
                        None,
                        rect.left,
                        rect.top,
                        rect.width(),
                        rect.height(),
                        SWP_NOZORDER | SWP_FRAMECHANGED,
                    )
                }
                .at("SetWindowPos")?;
            }
            Placement::Windowed { client } => {
                set_style(WINDOWED_STYLE);
                let mut frame = RECT {
                    left: 0,
                    top: 0,
                    right: client.0,
                    bottom: client.1,
                };
                // SAFETY: a valid rectangle; the style the window now has.
                unsafe {
                    AdjustWindowRectExForDpi(
                        &mut frame,
                        WINDOWED_STYLE,
                        false,
                        WINDOW_EX_STYLE(0),
                        dpi,
                    )
                }
                .at("AdjustWindowRectExForDpi")?;
                // SAFETY: the live window; its position is kept.
                unsafe {
                    SetWindowPos(
                        self.hwnd,
                        None,
                        0,
                        0,
                        frame.right - frame.left,
                        frame.bottom - frame.top,
                        SET_WINDOW_POS_FLAGS(SWP_NOMOVE.0 | SWP_NOZORDER.0 | SWP_FRAMECHANGED.0),
                    )
                }
                .at("SetWindowPos")?;
            }
        }
        // SAFETY: the live window.
        let _ = unsafe { ShowWindow(self.hwnd, SW_SHOWNORMAL) };
        Ok(())
    }

    /// Drains the thread's message queue and returns the window's events
    /// (`PumpMessages` in the C++) with the queued mouse events first and
    /// the IME's result characters among them; no key message is consumed.
    pub fn pump(&mut self) -> Pump {
        pump()
    }

    /// [`Window::pump`] with the game's input router (`PumpMessages`).
    /// Whether the window registered the mouse for Raw Input when it was
    /// made.
    #[must_use]
    pub fn raw_input_registered(&self) -> bool {
        with_state(|s| s.raw_registered).unwrap_or(false)
    }

    pub fn pump_routed(&mut self, router: &mut dyn InputRouter) -> Pump {
        pump_routed(router)
    }

    /// Runs `f` on the mouse model, with the window as its platform.
    pub fn with_mouse<R>(
        &self,
        f: impl FnOnce(&mut Mouse, &mut dyn MousePlatform) -> R,
    ) -> Option<R> {
        with_mouse(f)
    }

    /// The input half of `MouseFrameUpdate`, at the end of a frame, with
    /// the frame's millisecond stamp. The C++ runs it only once the cursors
    /// are loaded.
    pub fn mouse_frame(&self, frame_ms: u32) {
        with_mouse(|m, p| m.frame_update(p, frame_ms));
    }

    /// `MouseSetEnabled` from the game (after a display reset).
    pub fn set_mouse_enabled(&self, enable: bool) {
        with_mouse(|m, p| m.set_enabled(enable, p));
    }

    /// The IME on this window, for the edit boxes' focus manager.
    #[must_use]
    pub fn ime(&self) -> WindowIme {
        WindowIme::new(self.hwnd)
    }

    /// The IME's composition in progress.
    #[must_use]
    pub fn composition(&self) -> Composition {
        self.ime().composition()
    }

    /// How the client area maps onto the surface now.
    #[must_use]
    pub fn client_mapping(&self) -> ClientMapping {
        platform_input::client_mapping(self.hwnd, self.mode)
    }

    /// Destroys the window; the next pump sees `WM_QUIT`.
    pub fn destroy(self) {
        drop(self);
    }
}

/// A warning the player answers with Yes or No before the game goes on
/// (the low-machine warning of `InitializeEngine`, P4.1): a system
/// message box with the system cursor showing. Whether the answer was
/// Yes.
#[must_use]
pub fn ask_yes_no(owner: Option<HWND>, caption: &[u16], text: &[u16]) -> bool {
    use windows::Win32::UI::WindowsAndMessaging::{
        IDYES, MB_ICONWARNING, MB_YESNO, MessageBoxW, ShowCursor,
    };
    let terminated = |units: &[u16]| -> Vec<u16> {
        let mut out: Vec<u16> = units.iter().copied().take_while(|&u| u != 0).collect();
        out.push(0);
        out
    };
    let caption = terminated(caption);
    let text = terminated(text);
    // The C++ shows the cursor until its count is not negative, and puts
    // the count back afterwards.
    let mut extra = 0;
    // SAFETY: plain calls on this thread's cursor count.
    while unsafe { ShowCursor(true) } < 0 {
        extra += 1;
    }
    // SAFETY: two NUL-terminated strings that outlive the call.
    let answer = unsafe {
        MessageBoxW(
            owner,
            PCWSTR(text.as_ptr()),
            PCWSTR(caption.as_ptr()),
            MB_YESNO | MB_ICONWARNING,
        )
    };
    for _ in 0..=extra {
        // SAFETY: as above.
        unsafe { ShowCursor(false) };
    }
    answer == IDYES
}

/// The window's mouse model as the engine frame's mouse: the input half of
/// `MouseFrameUpdate` after the interface's draw, and the cursor there.
impl crate::engine::FrameMouse for Window {
    fn frame_update(&mut self, frame_ms: u32) {
        self.mouse_frame(frame_ms);
    }

    fn cursor(&self) -> (i32, i32) {
        with_state(|s| {
            let (x, y, _) = s.mouse.cursor();
            (x, y)
        })
        .unwrap_or_default()
    }
}

/// The game's input routing during a pump.
pub trait InputRouter {
    /// A queued mouse event, before the messages (`MouseDispatchEvents`
    /// hands each to `KeyMapRouteMouseEvent`).
    fn mouse_event(&mut self, event: MouseEvent) {
        let _ = event;
    }
    /// A key message before translation (`KeyMapRouteInputMessage`);
    /// `true` consumes it: it is neither translated nor dispatched.
    fn key_event(&mut self, event: &KeyEvent) -> bool {
        let _ = event;
        false
    }
    /// A unit of the IME's result string, for the focused edit box
    /// (`IMEFocus::OnChar`).
    fn ime_char(&mut self, unit: u16) {
        let _ = unit;
    }
}

/// The router of a plain pump: mouse events and result characters become
/// window events.
struct Report;

impl InputRouter for Report {
    fn mouse_event(&mut self, event: MouseEvent) {
        push(WindowEvent::Mouse(event));
    }
    fn ime_char(&mut self, unit: u16) {
        push(WindowEvent::ImeChar(unit));
    }
}

/// Hands the IME's queued result characters to the router.
fn flush_ime_chars(router: &mut dyn InputRouter) {
    while let Some(unit) = with_state(|s| s.ime.chars.pop_front()).flatten() {
        router.ime_char(unit);
    }
}

/// Drains the calling thread's message queue: [`Window::pump`], usable
/// after the window is gone to see `WM_QUIT`.
pub fn pump() -> Pump {
    pump_routed(&mut Report)
}

/// [`Window::pump_routed`] for the calling thread.
pub fn pump_routed(router: &mut dyn InputRouter) -> Pump {
    for event in with_state(|s| s.mouse.drain_events()).unwrap_or_default() {
        router.mouse_event(event);
    }
    let mut msg = MSG::default();
    let mut quit = None;
    // SAFETY: a valid message structure; messages of every window of
    // the thread.
    while unsafe { PeekMessageW(&mut msg, None, 0, 0, PM_REMOVE) }.as_bool() {
        if msg.message == WM_QUIT {
            quit = Some(msg.wParam.0 as i32);
            break;
        }
        if ime::ignore_hot_key(&msg) {
            continue;
        }
        if keyboard::is_routed(msg.message) {
            let event = KeyEvent::from_message(
                msg.message,
                msg.wParam.0 as u32,
                msg.lParam.0 as u32,
                platform_input::modifiers(),
            );
            if event.is_some_and(|e| router.key_event(&e)) {
                continue;
            }
        }
        // SAFETY: the message just removed from the queue.
        unsafe {
            let _ = TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        flush_ime_chars(router);
    }
    let events = with_state(|s| s.events.drain(..).collect()).unwrap_or_default();
    match quit {
        Some(code) => Pump::Quit(code, events),
        None => Pump::Events(events),
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        // SAFETY: the window this value made; destroying it twice fails
        // harmlessly.
        let _ = unsafe { DestroyWindow(self.hwnd) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The window opens, takes its placement, reports a close request
    /// without closing, and quits once destroyed. CI's runner has a desktop
    /// session; without one, creation fails and the test says so.
    #[test]
    fn the_window_opens_reports_and_quits() {
        let mode = DisplayMode {
            width: 320,
            height: 240,
            fullscreen: false,
        };
        let mut window = match Window::new("outpost-test", mode) {
            Ok(w) => w,
            Err(e) => {
                println!("no window here ({e}); nothing to test");
                return;
            }
        };
        let dpi = window.dpi();
        let mut client = RECT::default();
        // SAFETY: the live window and a valid out rectangle.
        unsafe {
            windows::Win32::UI::WindowsAndMessaging::GetClientRect(window.hwnd(), &mut client)
        }
        .unwrap();
        assert_eq!(
            (client.right, client.bottom),
            (
                crate::window::scale(320, dpi),
                crate::window::scale(240, dpi)
            ),
            "dpi {dpi}"
        );
        let _ = window.pump();
        // SAFETY: posting to the live window.
        unsafe {
            windows::Win32::UI::WindowsAndMessaging::PostMessageW(
                Some(window.hwnd()),
                WM_SYSCOMMAND,
                WPARAM(SC_CLOSE as usize),
                LPARAM(0),
            )
        }
        .unwrap();
        let Pump::Events(events) = window.pump() else {
            panic!("the close command must not quit");
        };
        assert!(events.contains(&WindowEvent::CloseRequested), "{events:?}");
        window
            .apply(DisplayMode {
                fullscreen: true,
                ..mode
            })
            .unwrap();
        let monitor = window.monitor().unwrap();
        // SAFETY: as above.
        unsafe {
            windows::Win32::UI::WindowsAndMessaging::GetClientRect(window.hwnd(), &mut client)
        }
        .unwrap();
        assert_eq!(
            (client.right, client.bottom),
            (monitor.width(), monitor.height())
        );
        window.destroy();
        // The destroyed window's WM_DESTROY posted WM_QUIT.
        assert!(matches!(pump(), Pump::Quit(0, _)));
    }

    #[derive(Debug, Default)]
    struct Recorder {
        keys: Vec<KeyEvent>,
        chars: Vec<u16>,
        consume_key_downs: bool,
    }

    impl InputRouter for Recorder {
        fn key_event(&mut self, event: &KeyEvent) -> bool {
            self.keys.push(*event);
            self.consume_key_downs && event.message == keyboard::WM_KEYDOWN
        }
        fn ime_char(&mut self, unit: u16) {
            self.chars.push(unit);
        }
    }

    /// The mouse is registered for Raw Input; key messages reach the router
    /// before translation, and a consumed key-down is not translated; the
    /// IME's result characters reach the router while a box is registered;
    /// the input context comes and goes with the IME's open state, and the
    /// composition window follows the caret.
    #[test]
    fn input_is_routed_and_the_ime_follows_the_caret() {
        use windows::Win32::UI::Input::Ime::{
            COMPOSITIONFORM, ImmGetCompositionWindow, ImmGetContext, ImmReleaseContext,
        };
        use windows::Win32::UI::WindowsAndMessaging::{PostMessageW, WM_IME_CHAR, WM_KEYDOWN};

        use crate::input::keyboard::KeyEventKind;
        use crate::text_input::ime::{CaretRect, ImeContext};

        let mode = DisplayMode {
            width: 320,
            height: 240,
            fullscreen: false,
        };
        let mut window = match Window::new("outpost-input-test", mode) {
            Ok(w) => w,
            Err(e) => {
                println!("no window here ({e}); nothing to test");
                return;
            }
        };
        let _ = window.pump();
        let hwnd = window.hwnd();
        let post = |msg: u32, w: usize, l: isize| {
            // SAFETY: posting to the live window.
            unsafe { PostMessageW(Some(hwnd), msg, WPARAM(w), LPARAM(l)) }.unwrap();
        };
        assert_eq!(window.with_mouse(|m, _| m.display()), Some(mode));

        // The window registered the mouse for Raw Input. (The registration
        // is process-wide; the other window tests running in parallel
        // register and destroy their own, so the window's own result is
        // what is checked, not the process's current list.)
        assert!(window.raw_input_registered());

        let mut r = Recorder {
            consume_key_downs: true,
            ..Recorder::default()
        };
        post(WM_KEYDOWN, 0x41, 0x001e_0001);
        let _ = window.pump_routed(&mut r);
        assert_eq!(r.keys.len(), 1, "{:?}", r.keys);
        assert_eq!(
            (r.keys[0].kind, r.keys[0].key, r.keys[0].down),
            (KeyEventKind::Key, 0x41, true)
        );
        r.consume_key_downs = false;
        r.keys.clear();
        post(WM_KEYDOWN, 0x41, 0x001e_0001);
        let _ = window.pump_routed(&mut r);
        // Translation queued a character, which the same pump routed.
        assert!(
            r.keys.iter().any(|k| k.kind == KeyEventKind::Char),
            "{:?}",
            r.keys
        );

        let mut ime = window.ime();
        ime.set_boxes_registered(true);
        post(WM_IME_CHAR, 0x3042, 1);
        let _ = window.pump_routed(&mut r);
        assert_eq!(r.chars, [0x3042]);
        post(WM_IME_CHAR, 0x3044, 1);
        let Pump::Events(events) = window.pump() else {
            panic!("no quit expected");
        };
        assert!(events.contains(&WindowEvent::ImeChar(0x3044)), "{events:?}");

        ime.set_open(false);
        assert!(!ime.is_open());
        // SAFETY: a plain query of the live window.
        let closed = unsafe { ImmGetContext(hwnd) };
        assert!(closed.is_invalid(), "no context while the IME is closed");
        ime.set_open(true);
        // SAFETY: as above.
        let himc = unsafe { ImmGetContext(hwnd) };
        if himc.is_invalid() {
            println!("this desktop gives windows no input context; IME placement not tested");
        } else {
            ime.set_caret(CaretRect {
                x: 10,
                y: 20,
                height: 16,
            });
            let mut form = COMPOSITIONFORM::default();
            // SAFETY: the window's context and an out form.
            let read = unsafe { ImmGetCompositionWindow(himc, &mut form) }.as_bool();
            // SAFETY: the context taken above.
            let _ = unsafe { ImmReleaseContext(hwnd, himc) };
            assert!(read);
            let (x, y) = window.client_mapping().to_client(10, 20);
            assert_eq!((form.ptCurrentPos.x, form.ptCurrentPos.y), (x, y));
            assert!(ime.finalize(true).is_empty(), "nothing was composed");
        }
        ime.set_boxes_registered(false);
        window.destroy();
        assert!(matches!(pump(), Pump::Quit(0, _)));
    }
}
