//! Raw Input and the system cursor for the game window (work package
//! P4.11), in place of the C++ client's DirectInput 8 system mouse.
//!
//! The window registers the mouse for Raw Input when it is created. While
//! the model holds the mouse exclusively and acquired, the registration
//! drops legacy mouse messages and captures the mouse
//! (`RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE`) and the pointer is clipped to
//! the client area: no `WM_MOUSEMOVE` reaches the window and no click
//! reaches another window, as with an exclusive, foreground DirectInput
//! device (decision O28, proposed). Unacquiring registers the mouse plainly
//! again and frees the pointer. Without `RIDEV_INPUTSINK`, reports arrive
//! only while the window is in front.

use std::ffi::c_void;

use windows::Win32::Foundation::{HWND, LPARAM, POINT, RECT};
use windows::Win32::Graphics::Gdi::{ClientToScreen, ScreenToClient};
use windows::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, GetFocus, GetKeyState};
use windows::Win32::UI::Input::{
    GetRawInputData, HRAWINPUT, RAWINPUT, RAWINPUTDEVICE, RAWINPUTDEVICE_FLAGS, RAWINPUTHEADER,
    RID_INPUT, RIDEV_CAPTUREMOUSE, RIDEV_NOLEGACY, RIM_TYPEMOUSE, RegisterRawInputDevices,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CURSOR_SHOWING, CURSORINFO, ClipCursor, GetClientRect, GetCursorInfo, GetCursorPos,
    GetForegroundWindow, GetSystemMetrics, SM_CXSCREEN, SM_CXVIRTUALSCREEN, SM_CYSCREEN,
    SM_CYVIRTUALSCREEN, SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SetCursorPos, ShowCursor,
};

use crate::input::keyboard::{Modifiers, vk};
use crate::input::mouse::{MouseInput, MousePlatform};
use crate::input::raw::{RawMouse, RawMouseDecoder};
use crate::window::{ClientMapping, DisplayMode, Rect};

/// HID usage page and usage of a mouse.
const USAGE_PAGE_GENERIC: u16 = 0x01;
const USAGE_MOUSE: u16 = 0x02;

/// A bound on the `ShowCursor` loops (each call moves the count by one).
const SHOW_CURSOR_LIMIT: usize = 4096;

/// The window's Raw Input state.
#[derive(Debug, Default)]
pub(crate) struct RawInputState {
    pub decoder: RawMouseDecoder,
    /// The cooperative level asked for: exclusive.
    exclusive: bool,
    /// Registered without legacy messages, pointer clipped.
    acquired: bool,
}

impl RawInputState {
    /// Frees the pointer when the window goes away while acquired.
    pub fn release(&mut self) {
        if self.acquired {
            // SAFETY: a plain call; no clip rectangle.
            let _ = unsafe { ClipCursor(None) };
            self.acquired = false;
        }
    }
}

/// Registers the mouse for Raw Input on `hwnd` with `flags`.
pub(crate) fn register_mouse(hwnd: HWND, flags: RAWINPUTDEVICE_FLAGS) -> bool {
    let device = RAWINPUTDEVICE {
        usUsagePage: USAGE_PAGE_GENERIC,
        usUsage: USAGE_MOUSE,
        dwFlags: flags,
        hwndTarget: hwnd,
    };
    // SAFETY: one complete description of a device; the size is its own.
    unsafe { RegisterRawInputDevices(&[device], size_of::<RAWINPUTDEVICE>() as u32) }.is_ok()
}

/// The mouse report of a `WM_INPUT` message, or `None` for another device
/// or a failed read.
pub(crate) fn read_raw_mouse(lparam: LPARAM) -> Option<RawMouse> {
    let mut raw = RAWINPUT::default();
    let mut size = size_of::<RAWINPUT>() as u32;
    // SAFETY: the handle is the message's; the buffer is a RAWINPUT of the
    // size given, which holds any mouse report.
    let read = unsafe {
        GetRawInputData(
            HRAWINPUT(lparam.0 as *mut c_void),
            RID_INPUT,
            Some((&raw mut raw).cast::<c_void>()),
            &mut size,
            size_of::<RAWINPUTHEADER>() as u32,
        )
    };
    if read == u32::MAX || read == 0 || raw.header.dwType != RIM_TYPEMOUSE.0 {
        return None;
    }
    // SAFETY: the header says the union holds a mouse report, and both
    // views of the button word are plain integers.
    let (mouse, buttons) = unsafe { (raw.data.mouse, raw.data.mouse.Anonymous.Anonymous) };
    Some(RawMouse {
        flags: mouse.usFlags.0,
        button_flags: buttons.usButtonFlags,
        button_data: buttons.usButtonData,
        last_x: mouse.lLastX,
        last_y: mouse.lLastY,
    })
}

fn metric(index: windows::Win32::UI::WindowsAndMessaging::SYSTEM_METRICS_INDEX) -> i32 {
    // SAFETY: a plain query.
    unsafe { GetSystemMetrics(index) }
}

/// Decodes a report against the primary monitor and the virtual desktop.
pub(crate) fn decode(decoder: &mut RawMouseDecoder, raw: RawMouse, time: u32) -> MouseInput {
    let primary = Rect {
        left: 0,
        top: 0,
        right: metric(SM_CXSCREEN),
        bottom: metric(SM_CYSCREEN),
    };
    let (x, y) = (metric(SM_XVIRTUALSCREEN), metric(SM_YVIRTUALSCREEN));
    let desktop = Rect {
        left: x,
        top: y,
        right: x + metric(SM_CXVIRTUALSCREEN),
        bottom: y + metric(SM_CYVIRTUALSCREEN),
    };
    decoder.decode(raw, primary, desktop, time)
}

/// The modifier state as the key map reads it (`GetKeyState`'s high bit of
/// Shift, Ctrl and Alt).
#[must_use]
pub fn modifiers() -> Modifiers {
    Modifiers::from_keys(
        key_down(vk::SHIFT),
        key_down(vk::CONTROL),
        key_down(vk::MENU),
    )
}

/// Whether a key is down in the thread's key state (`GetKeyState < 0`).
pub(crate) fn key_down(key: u32) -> bool {
    // SAFETY: a plain query.
    unsafe { GetKeyState(key as i32) < 0 }
}

/// Whether a key is down now (`GetAsyncKeyState < 0`).
pub(crate) fn key_down_now(key: u32) -> bool {
    // SAFETY: a plain query.
    unsafe { GetAsyncKeyState(key as i32) < 0 }
}

/// The client area against the surface of `mode`.
pub(crate) fn client_mapping(hwnd: HWND, mode: DisplayMode) -> ClientMapping {
    let mut r = RECT::default();
    // SAFETY: a valid out rectangle; a failure leaves it empty, which maps
    // positions unchanged.
    let _ = unsafe { GetClientRect(hwnd, &mut r) };
    ClientMapping {
        client: (r.right - r.left, r.bottom - r.top),
        surface: mode.back_buffer(),
    }
}

/// [`MousePlatform`] on the game window.
pub(crate) struct WinMouse<'a> {
    hwnd: HWND,
    mapping: ClientMapping,
    raw: &'a mut RawInputState,
}

impl<'a> WinMouse<'a> {
    pub fn new(hwnd: HWND, mode: DisplayMode, raw: &'a mut RawInputState) -> WinMouse<'a> {
        WinMouse {
            hwnd,
            mapping: client_mapping(hwnd, mode),
            raw,
        }
    }

    fn clip_to_client(&self) -> bool {
        let mut r = RECT::default();
        // SAFETY: the live window and valid out structures.
        unsafe {
            if GetClientRect(self.hwnd, &mut r).is_err() {
                return false;
            }
            let mut top_left = POINT {
                x: r.left,
                y: r.top,
            };
            let mut bottom_right = POINT {
                x: r.right,
                y: r.bottom,
            };
            if !ClientToScreen(self.hwnd, &mut top_left).as_bool()
                || !ClientToScreen(self.hwnd, &mut bottom_right).as_bool()
            {
                return false;
            }
            let screen = RECT {
                left: top_left.x,
                top: top_left.y,
                right: bottom_right.x,
                bottom: bottom_right.y,
            };
            ClipCursor(Some(&screen)).is_ok()
        }
    }
}

impl MousePlatform for WinMouse<'_> {
    fn cursor_position(&self) -> Option<(i32, i32)> {
        let mut p = POINT::default();
        // SAFETY: a valid out point; the live window.
        unsafe {
            GetCursorPos(&mut p).ok()?;
            if !ScreenToClient(self.hwnd, &mut p).as_bool() {
                return None;
            }
        }
        Some(self.mapping.to_surface(p.x, p.y))
    }

    fn set_cursor_position(&mut self, x: i32, y: i32) {
        let (x, y) = self.mapping.to_client(x, y);
        let mut p = POINT { x, y };
        // SAFETY: a valid in-out point; the live window.
        unsafe {
            if ClientToScreen(self.hwnd, &mut p).as_bool() {
                let _ = SetCursorPos(p.x, p.y);
            }
        }
    }

    fn system_cursor_showing(&self) -> bool {
        let mut info = CURSORINFO {
            cbSize: size_of::<CURSORINFO>() as u32,
            ..Default::default()
        };
        // SAFETY: a sized out structure. A failed query counts as showing,
        // so the pointer is left where it is.
        unsafe { GetCursorInfo(&mut info) }.map_or(true, |()| info.flags.0 & CURSOR_SHOWING.0 != 0)
    }

    fn show_system_cursor(&mut self, show: bool) {
        for _ in 0..SHOW_CURSOR_LIMIT {
            // SAFETY: plain calls on the display count.
            let count = unsafe { ShowCursor(show) };
            if (show && count > 0) || (!show && count < 0) {
                break;
            }
        }
    }

    fn has_focus(&self) -> bool {
        // SAFETY: a plain query.
        unsafe { GetFocus() == self.hwnd }
    }

    fn is_foreground(&self) -> bool {
        // SAFETY: a plain query.
        unsafe { GetForegroundWindow() == self.hwnd }
    }

    fn set_exclusive(&mut self, exclusive: bool) -> bool {
        self.raw.exclusive = exclusive;
        true
    }

    fn acquire(&mut self) -> bool {
        if !self.raw.exclusive {
            self.raw.acquired = register_mouse(self.hwnd, RAWINPUTDEVICE_FLAGS(0));
            return self.raw.acquired;
        }
        let ok = register_mouse(
            self.hwnd,
            RAWINPUTDEVICE_FLAGS(RIDEV_NOLEGACY.0 | RIDEV_CAPTUREMOUSE.0),
        );
        if ok {
            self.raw.acquired = true;
            self.clip_to_client();
        }
        ok
    }

    fn unacquire(&mut self) {
        if self.raw.acquired {
            register_mouse(self.hwnd, RAWINPUTDEVICE_FLAGS(0));
            self.raw.release();
        }
    }

    fn buttons_down(&self) -> u8 {
        // Left, right, middle, extra 1, extra 2 (physical buttons, as the
        // reports give them).
        const KEYS: [u32; 5] = [0x01, 0x02, 0x04, 0x05, 0x06];
        KEYS.iter()
            .enumerate()
            .filter(|&(_, &k)| key_down_now(k))
            .fold(0, |bits, (i, _)| bits | (1 << i))
    }
}
