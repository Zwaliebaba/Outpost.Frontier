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
use windows::Win32::UI::WindowsAndMessaging::{
    CS_HREDRAW, CS_VREDRAW, CW_USEDEFAULT, CreateWindowExW, DefWindowProcW, DestroyWindow,
    DispatchMessageW, GWL_STYLE, HTCLIENT, IDC_ARROW, LoadCursorW, MSG, PM_REMOVE, PeekMessageW,
    PostQuitMessage, RegisterClassExW, SC_CLOSE, SET_WINDOW_POS_FLAGS, SW_SHOWNORMAL,
    SWP_FRAMECHANGED, SWP_NOMOVE, SWP_NOZORDER, SetWindowLongPtrW, SetWindowPos, ShowWindow,
    TranslateMessage, WINDOW_EX_STYLE, WINDOW_STYLE, WM_ACTIVATEAPP, WM_CLOSE, WM_DESTROY,
    WM_DPICHANGED, WM_ERASEBKGND, WM_KILLFOCUS, WM_MOUSEMOVE, WM_NCHITTEST, WM_QUIT, WM_SETFOCUS,
    WM_SIZE, WM_SYSCOMMAND, WM_SYSKEYUP, WNDCLASSEXW, WS_CAPTION, WS_CLIPCHILDREN, WS_MINIMIZEBOX,
    WS_POPUP, WS_SYSMENU, WS_VISIBLE,
};
use windows::core::{HSTRING, PCWSTR, w};

use crate::window::{DisplayMode, Placement, Rect, WindowEvent, placement};

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
struct State {
    window: HWND,
    fullscreen: bool,
    events: VecDeque<WindowEvent>,
}

thread_local! {
    static STATE: RefCell<Option<State>> = const { RefCell::new(None) };
}

fn push(event: WindowEvent) {
    STATE.with_borrow_mut(|s| {
        if let Some(s) = s {
            s.events.push_back(event);
        }
    });
}

fn is_fullscreen() -> bool {
    STATE.with_borrow(|s| s.as_ref().is_some_and(|s| s.fullscreen))
}

fn low_word(v: isize) -> u32 {
    (v as usize & 0xffff) as u32
}

fn high_word(v: isize) -> u32 {
    ((v as usize >> 16) & 0xffff) as u32
}

extern "system" fn window_proc(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match msg {
        WM_SETFOCUS => push(WindowEvent::FocusGained),
        WM_KILLFOCUS => push(WindowEvent::FocusLost),
        // The renderer owns every pixel.
        WM_ERASEBKGND => return LRESULT(1),
        WM_ACTIVATEAPP => push(WindowEvent::Activated(wparam.0 == 1)),
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
            push(WindowEvent::MouseMoved { x, y });
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
            STATE.with_borrow_mut(|s| {
                if s.as_ref().is_some_and(|s| s.window == hwnd) {
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
        STATE.with_borrow_mut(|s| {
            *s = Some(State {
                window: hwnd,
                fullscreen: mode.fullscreen,
                events: VecDeque::new(),
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
        STATE.with_borrow_mut(|s| {
            if let Some(s) = s {
                s.fullscreen = mode.fullscreen;
            }
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
    /// (`PumpMessages` in the C++, without its input routing, which P4.11
    /// adds).
    pub fn pump(&mut self) -> Pump {
        pump()
    }

    /// Destroys the window; the next pump sees `WM_QUIT`.
    pub fn destroy(self) {
        drop(self);
    }
}

/// Drains the calling thread's message queue: [`Window::pump`], usable
/// after the window is gone to see `WM_QUIT`.
pub fn pump() -> Pump {
    let mut msg = MSG::default();
    let mut quit = None;
    // SAFETY: a valid message structure; messages of every window of
    // the thread.
    while unsafe { PeekMessageW(&mut msg, None, 0, 0, PM_REMOVE) }.as_bool() {
        if msg.message == WM_QUIT {
            quit = Some(msg.wParam.0 as i32);
            break;
        }
        // SAFETY: the message just removed from the queue.
        unsafe {
            let _ = TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    let events = STATE.with_borrow_mut(|s| {
        s.as_mut()
            .map(|s| s.events.drain(..).collect())
            .unwrap_or_default()
    });
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
}
