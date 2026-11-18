//! The game window's platform-free part (work package P4.1; decision O27):
//! where the window goes for a display mode, and the events the window
//! procedure reports. `platform::window` creates the window and runs the
//! message pump on Windows.
//!
//! The C++ (`window.cpp`, `CreateGameWindow` and `ApplyGameWindowLayout`)
//! is DPI-unaware: a windowed client area of the mode's size that Windows
//! bitmap-scales, or a topmost popup the size of the desktop over a
//! Direct3D 8 fullscreen mode change. Here (O27) the process is per-monitor
//! DPI aware: a windowed client area is the mode's size times the monitor's
//! scale, and fullscreen is a borderless window over the monitor. In both
//! the back buffer keeps the mode's size and the swap chain stretches it.
//! [`ClientMapping`] converts positions between the two (P4.11).

use crate::input::mouse::MouseEvent;

/// A rectangle in physical pixels: left, top, right, bottom.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Rect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

impl Rect {
    #[must_use]
    pub fn width(&self) -> i32 {
        self.right - self.left
    }

    #[must_use]
    pub fn height(&self) -> i32 {
        self.bottom - self.top
    }
}

/// The DPI Windows treats as 100 % scale.
pub const BASE_DPI: u32 = 96;

/// The size the C++ puts in for a zero width or height (640 by 480).
pub const FALLBACK_SIZE: (u32, u32) = (640, 480);

/// The display settings the window follows: the mode's size and whether it
/// is fullscreen (`DisplaySettings` in the C++).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DisplayMode {
    pub width: u32,
    pub height: u32,
    pub fullscreen: bool,
}

impl DisplayMode {
    /// The back-buffer size: the mode's, with the C++'s 640 by 480 for a
    /// zero width or height.
    #[must_use]
    pub fn back_buffer(&self) -> (u32, u32) {
        (
            if self.width == 0 {
                FALLBACK_SIZE.0
            } else {
                self.width
            },
            if self.height == 0 {
                FALLBACK_SIZE.1
            } else {
                self.height
            },
        )
    }
}

/// Where the window goes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Placement {
    /// A captioned window whose client area has this size in physical
    /// pixels; the platform adds the frame for the window's DPI and keeps
    /// the window's position.
    Windowed { client: (i32, i32) },
    /// A borderless popup covering this monitor rectangle.
    Borderless { rect: Rect },
}

/// `value` scaled from 96 DPI to `dpi`, rounded half up as `MulDiv` rounds
/// positive values.
#[must_use]
pub fn scale(value: u32, dpi: u32) -> i32 {
    let scaled =
        (u64::from(value) * u64::from(dpi) + u64::from(BASE_DPI / 2)) / u64::from(BASE_DPI);
    i32::try_from(scaled).unwrap_or(i32::MAX)
}

/// The placement for `mode` on a monitor of `dpi` covering `monitor`.
#[must_use]
pub fn placement(mode: DisplayMode, dpi: u32, monitor: Rect) -> Placement {
    if mode.fullscreen {
        return Placement::Borderless { rect: monitor };
    }
    let (w, h) = mode.back_buffer();
    Placement::Windowed {
        client: (scale(w, dpi), scale(h, dpi)),
    }
}

/// How client-area pixels and surface (back-buffer) pixels correspond:
/// the swap chain stretches the surface over the client area. Each pixel
/// maps to the pixel holding its centre; with equal sizes both ways are the
/// identity.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClientMapping {
    /// The client area's size in physical pixels.
    pub client: (i32, i32),
    /// The surface's size (the back buffer, [`DisplayMode::back_buffer`]).
    pub surface: (u32, u32),
}

impl ClientMapping {
    /// The surface pixel under a client pixel.
    #[must_use]
    pub fn to_surface(&self, x: i32, y: i32) -> (i32, i32) {
        (
            map_axis(x, i64::from(self.surface.0), i64::from(self.client.0)),
            map_axis(y, i64::from(self.surface.1), i64::from(self.client.1)),
        )
    }

    /// The client pixel showing a surface pixel's centre.
    #[must_use]
    pub fn to_client(&self, x: i32, y: i32) -> (i32, i32) {
        (
            map_axis(x, i64::from(self.client.0), i64::from(self.surface.0)),
            map_axis(y, i64::from(self.client.1), i64::from(self.surface.1)),
        )
    }
}

/// `floor((v + 0.5) * to / from)`; unchanged when either size is empty.
fn map_axis(v: i32, to: i64, from: i64) -> i32 {
    if from <= 0 || to <= 0 {
        return v;
    }
    let mapped = ((2 * i64::from(v) + 1) * to).div_euclid(2 * from);
    i32::try_from(mapped).unwrap_or(if mapped < 0 { i32::MIN } else { i32::MAX })
}

/// What the IME did, for the UI toolkit (the composition itself is read
/// from the window).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImeEvent {
    /// `WM_IME_STARTCOMPOSITION`.
    CompositionStarted,
    /// `WM_IME_COMPOSITION` changed the composition string.
    CompositionChanged,
    /// `WM_IME_ENDCOMPOSITION`.
    CompositionEnded,
    /// The IME was opened or closed, its conversion mode or the input
    /// language changed (`IMN_SETOPENSTATUS`, `IMN_SETCONVERSIONMODE`,
    /// `WM_INPUTLANGCHANGE`).
    StateChanged,
}

/// What the window procedure reports to the game, in the order the
/// messages arrived.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WindowEvent {
    /// `WM_ACTIVATEAPP`: the application became active or inactive.
    Activated(bool),
    /// `WM_SETFOCUS`.
    FocusGained,
    /// `WM_KILLFOCUS`.
    FocusLost,
    /// `WM_MOUSEMOVE`, with the client position in physical pixels.
    MouseMoved { x: i32, y: i32 },
    /// The close command (`SC_CLOSE`: the close box, Alt+F4). The window
    /// stays; the game asks the player and calls `destroy` to quit, as
    /// the C++ raises its exit dialog.
    CloseRequested,
    /// The window's monitor changed its DPI (`WM_DPICHANGED`); a windowed
    /// window has already taken its new size.
    DpiChanged(u32),
    /// The client area's new size in physical pixels (`WM_SIZE`); zero
    /// when minimised.
    Resized { width: u32, height: u32 },
    /// A queued mouse event, reported first by a pump without an input
    /// router (`MouseDispatchEvents` runs before the messages).
    Mouse(MouseEvent),
    /// A UTF-16 unit of the IME's result string (`WM_IME_CHAR`), reported
    /// by a pump without an input router; the game hands it to the focused
    /// edit box (`IMEFocus::OnChar`).
    ImeChar(u16),
    /// The IME's composition or state changed.
    Ime(ImeEvent),
}

#[cfg(test)]
mod tests {
    use super::*;

    const MONITOR: Rect = Rect {
        left: 1920,
        top: 0,
        right: 1920 + 2560,
        bottom: 1440,
    };

    #[test]
    fn windowed_client_areas_scale_with_the_monitor() {
        let mode = DisplayMode {
            width: 1024,
            height: 768,
            fullscreen: false,
        };
        assert_eq!(
            placement(mode, 96, MONITOR),
            Placement::Windowed {
                client: (1024, 768)
            }
        );
        // 150 %.
        assert_eq!(
            placement(mode, 144, MONITOR),
            Placement::Windowed {
                client: (1536, 1152)
            }
        );
        // 125 % of 1366 is 1707.5: rounded up.
        assert_eq!(scale(1366, 120), 1708);
    }

    #[test]
    fn zero_sizes_fall_back_to_640_by_480() {
        let mode = DisplayMode {
            width: 0,
            height: 600,
            fullscreen: false,
        };
        assert_eq!(mode.back_buffer(), (640, 600));
        assert_eq!(
            placement(mode, 96, MONITOR),
            Placement::Windowed { client: (640, 600) }
        );
    }

    #[test]
    fn client_and_surface_pixels_map_through_their_centres() {
        let same = ClientMapping {
            client: (800, 600),
            surface: (800, 600),
        };
        for v in [-5, 0, 1, 799, 900] {
            assert_eq!(same.to_surface(v, v), (v, v));
            assert_eq!(same.to_client(v, v), (v, v));
        }
        // 150 %: a 1200-pixel client area shows an 800-pixel surface.
        let m = ClientMapping {
            client: (1200, 900),
            surface: (800, 600),
        };
        assert_eq!(m.to_surface(0, 0), (0, 0));
        assert_eq!(m.to_surface(1199, 899), (799, 599));
        assert_eq!(m.to_surface(-1, -2), (-1, -1));
        assert_eq!(m.to_client(1, 1), (2, 2));
        for v in 0..800 {
            let (c, _) = m.to_client(v, 0);
            assert_eq!(m.to_surface(c, 0).0, v);
        }
        // A minimised window has no client area: positions pass through.
        let none = ClientMapping {
            client: (0, 0),
            surface: (800, 600),
        };
        assert_eq!(none.to_surface(5, 6), (5, 6));
    }

    #[test]
    fn fullscreen_covers_the_monitor_whatever_the_mode() {
        let mode = DisplayMode {
            width: 800,
            height: 600,
            fullscreen: true,
        };
        let p = placement(mode, 144, MONITOR);
        assert_eq!(p, Placement::Borderless { rect: MONITOR });
        assert_eq!(MONITOR.width(), 2560);
        // The back buffer stays at the mode's size.
        assert_eq!(mode.back_buffer(), (800, 600));
    }
}
