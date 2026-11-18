//! The mouse model of the C++ client (`input_manager.cpp`, the reference's
//! `Client\cmn\mouse.cpp`): the software cursor driven by relative device
//! motion, the button bits, the wheel, the queue of move, button and wheel
//! events, double clicks, and taking and releasing the mouse as the window
//! gains and loses the focus.
//!
//! The C++ reads a DirectInput 8 system mouse in buffered mode, exclusive
//! and foreground, once a frame (`MouseFrameUpdate`). Here Raw Input
//! reports arrive between frames as `WM_INPUT` and are kept by the model as
//! that device kept them: button transitions in a buffer of 128 records
//! with their message time (`GetDeviceData`), motion and the wheel summed
//! (`GetDeviceState`), and the buttons' current state. [`Mouse::frame_update`]
//! then reads them in the C++'s order. Taking the mouse "exclusively" is
//! the platform's business ([`MousePlatform`]): on Windows Raw Input
//! without legacy messages, with the pointer captured and clipped to the
//! client area (decision O28, proposed).
//!
//! Cursor positions are in surface pixels (the back buffer's size, the
//! C++'s `G_RenderSurfaceWidth` and `G_RenderSurfaceHeight`). Since
//! decision O27 the client area can be larger than the surface; the
//! platform converts with [`crate::window::ClientMapping`].
//!
//! The C++'s absolute axis mode (flags bit 0x02) is never set in the
//! reference and is not modelled; its buffered-mode flag is always set, so
//! the buffered path is the only one.

use std::collections::VecDeque;

use neuron_core::num::trunc_to_i32;

use crate::window::DisplayMode;

/// Buttons the device reports (`DIMOUSESTATE2`): left, right, middle, the
/// two extra buttons and three more Raw Input never reports.
pub const BUTTON_COUNT: u32 = 8;
/// The wheel is queued as "button" 8.
pub const WHEEL_BUTTON: u32 = 8;
/// A second press within this many pixels on each axis can be a double
/// click.
pub const DOUBLE_CLICK_SLOP: i32 = 4;
/// The device buffer's size in records (`DIPROP_BUFFERSIZE` 0x80).
pub const BUFFER_ENTRIES: usize = 0x80;
/// The axis scale per surface pixel of width (.rdata double 0.00125): a
/// surface 800 pixels wide moves the cursor one pixel per count at speed 1.
pub const AXIS_SCALE: f64 = 0.00125;
/// The wheel scale (.rdata float 1/120): one notch per `WHEEL_DELTA`.
pub const WHEEL_SCALE: f32 = 1.0 / 120.0;
/// The last-press position before any press (.data -777, -777).
pub const NO_CLICK: (i32, i32) = (-777, -777);

/// One queued input event (`FMouseEvent`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MouseEvent {
    /// The cursor moved this frame (the game posts HUD command 5).
    Moved,
    /// A button changed state, or the wheel turned one notch (index
    /// [`WHEEL_BUTTON`], `down` when it turned towards the user).
    Button {
        index: u32,
        down: bool,
        double_click: bool,
    },
}

impl MouseEvent {
    /// The key the C++ event record carries: -1 - index (-9 for the
    /// wheel), 0 for a move.
    #[must_use]
    pub fn key(self) -> i32 {
        match self {
            MouseEvent::Moved => 0,
            MouseEvent::Button { index, .. } => -1 - index as i32,
        }
    }

    /// A wheel notch.
    #[must_use]
    pub fn is_wheel(self) -> bool {
        matches!(self, MouseEvent::Button { index, .. } if index == WHEEL_BUTTON)
    }
}

/// One Raw Input mouse report, decoded by [`crate::input::raw`]: relative
/// motion in device counts, the wheel in `WHEEL_DELTA` units (positive away
/// from the user), the `RI_MOUSE_*` button transition flags, and the
/// message time in milliseconds.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MouseInput {
    pub dx: i32,
    pub dy: i32,
    pub wheel: i32,
    pub button_flags: u16,
    pub time: u32,
}

/// What the model asks of the platform: the system cursor, the focus and
/// the device. Positions are in surface pixels.
pub trait MousePlatform {
    /// The system pointer's position (`GetCursorPos`, `ScreenToClient`),
    /// or `None` when either call fails.
    fn cursor_position(&self) -> Option<(i32, i32)>;
    /// Moves the system pointer (`ClientToScreen`, `SetCursorPos`).
    fn set_cursor_position(&mut self, x: i32, y: i32);
    /// Whether the system cursor is showing (`GetCursorInfo`).
    fn system_cursor_showing(&self) -> bool;
    /// Shows the system cursor (`ShowCursor(TRUE)` until the count is
    /// positive) or hides it (`ShowCursor(FALSE)` until it is negative).
    fn show_system_cursor(&mut self, show: bool);
    /// Whether the game window has the keyboard focus (`GetFocus`).
    fn has_focus(&self) -> bool;
    /// Whether the game window is the foreground window.
    fn is_foreground(&self) -> bool;
    /// Asks for the mouse exclusively (`DISCL_EXCLUSIVE |
    /// DISCL_FOREGROUND`) or gives it back (`DISCL_NONEXCLUSIVE |
    /// DISCL_BACKGROUND`); returns whether the level was set.
    fn set_exclusive(&mut self, exclusive: bool) -> bool;
    /// Starts delivering reports in the cooperative level set (`Acquire`).
    fn acquire(&mut self) -> bool;
    /// Stops delivering reports and gives the pointer back (`Unacquire`).
    fn unacquire(&mut self);
    /// The physical buttons now down, bit n for button n (the immediate
    /// state at acquisition).
    fn buttons_down(&self) -> u8;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ButtonRecord {
    index: u32,
    down: bool,
    time: u32,
}

/// The device's data between two reads.
#[derive(Clone, Debug, Default)]
struct Device {
    acquired: bool,
    exclusive: bool,
    dx: i32,
    dy: i32,
    dz: i32,
    buttons: u8,
    records: VecDeque<ButtonRecord>,
}

impl Device {
    fn flush(&mut self) {
        self.dx = 0;
        self.dy = 0;
        self.dz = 0;
        self.records.clear();
    }
}

/// The C++'s mouse state: the device object, the software cursor, the
/// button bits, the wheel and the event queue.
#[derive(Clone, Debug)]
pub struct Mouse {
    device: Device,
    /// Flags bit 0x01: the device is read and owns the cursor.
    active: bool,
    /// Flags bit 0x10: clamp the cursor to the surface instead of
    /// releasing the mouse when it leaves (fullscreen).
    clamp_to_surface: bool,
    double_click_time: u32,
    last_press_time: [u32; BUTTON_COUNT as usize],
    remainder: [f32; 3],
    moved: bool,
    events: VecDeque<MouseEvent>,
    input_active: bool,
    x: i32,
    y: i32,
    z: i32,
    wheel: i32,
    buttons: u32,
    speed: f32,
    last_click: (i32, i32),
    mode: DisplayMode,
}

impl Mouse {
    /// The state at start-up, with the system double-click time the C++
    /// latches when it creates the device (`GetDoubleClickTime`) and the
    /// display mode (the surface is 640 by 480 until a mode is set).
    #[must_use]
    pub fn new(double_click_time: u32, mode: DisplayMode) -> Mouse {
        Mouse {
            device: Device::default(),
            active: false,
            clamp_to_surface: false,
            double_click_time,
            last_press_time: [0; BUTTON_COUNT as usize],
            remainder: [0.0; 3],
            moved: false,
            events: VecDeque::new(),
            input_active: false,
            x: 0,
            y: 0,
            z: 0,
            wheel: 0,
            buttons: 0,
            speed: 1.0,
            last_click: NO_CLICK,
            mode,
        }
    }

    /// The display mode: the surface size and whether it is fullscreen
    /// (`ApplyGameWindowLayout` records them).
    pub fn set_display(&mut self, mode: DisplayMode) {
        self.mode = mode;
    }

    #[must_use]
    pub fn display(&self) -> DisplayMode {
        self.mode
    }

    fn surface(&self) -> (i32, i32) {
        let (w, h) = self.mode.back_buffer();
        (
            i32::try_from(w).unwrap_or(i32::MAX),
            i32::try_from(h).unwrap_or(i32::MAX),
        )
    }

    /// `WM_MOUSEMOVE` (`ClientInputOnMouseMove`): with input active, take
    /// the mouse again; the cursor jumps to the system pointer.
    pub fn on_mouse_move(&mut self, platform: &mut dyn MousePlatform) {
        if !self.input_active {
            return;
        }
        self.set_active(true, platform);
    }

    /// `WM_SETFOCUS` (`ClientInputOnFocusGained`).
    pub fn on_focus_gained(&mut self, platform: &mut dyn MousePlatform) {
        self.set_active(true, platform);
        self.input_active = true;
    }

    /// `WM_KILLFOCUS` (`ClientInputOnFocusLost`).
    pub fn on_focus_lost(&mut self, platform: &mut dyn MousePlatform) {
        self.set_active(false, platform);
        self.input_active = false;
    }

    /// `MouseSetEnabled`: `WM_ACTIVATEAPP` and the end of a display reset.
    /// A windowed session takes the mouse only while the window has the
    /// focus; fullscreen clamps the cursor instead of releasing the mouse.
    pub fn set_enabled(&mut self, enable: bool, platform: &mut dyn MousePlatform) {
        if !enable {
            self.set_active(false, platform);
            self.input_active = false;
            return;
        }
        let fullscreen = self.mode.fullscreen;
        if !fullscreen && !platform.has_focus() {
            return;
        }
        self.clamp_to_surface = fullscreen;
        self.set_active(true, platform);
        self.input_active = true;
    }

    /// `ClientInputIsActive`.
    #[must_use]
    pub fn input_active(&self) -> bool {
        self.input_active
    }

    /// Whether the device owns the cursor (flags bit 0x01).
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.active
    }

    /// Whether the device is acquired.
    #[must_use]
    pub fn is_acquired(&self) -> bool {
        self.device.acquired
    }

    /// The prologue the C++'s `SetActive(0)`, `SetExclusive` and
    /// `SetBufferSize` share: unacquire, drop the acquired and exclusive
    /// bits, empty the queue.
    fn unacquire_and_flush(&mut self, platform: &mut dyn MousePlatform) {
        platform.unacquire();
        self.device.acquired = false;
        self.device.exclusive = false;
        self.device.flush();
        self.events.clear();
    }

    /// `SetActive`.
    fn set_active(&mut self, active: bool, platform: &mut dyn MousePlatform) {
        if active {
            self.active = true;
            self.remainder[1] = 0.0;
            self.remainder[0] = 0.0;
            // A windowed session picks the cursor up where the system
            // pointer is.
            if self.mode.width != 0
                && self.mode.height != 0
                && !self.mode.fullscreen
                && let Some((x, y)) = platform.cursor_position()
            {
                self.x = x;
                self.y = y;
                self.z = 0;
            }
            let (w, h) = self.surface();
            if self.x < 0 {
                self.x = 0;
            } else if self.x >= w {
                self.x = w - 1;
            }
            if self.y < 0 {
                self.y = 0;
            } else if self.y >= h {
                self.y = h - 1;
            }
            platform.show_system_cursor(false);
            return;
        }
        self.active = false;
        self.unacquire_and_flush(platform);
        platform.set_exclusive(false);
        self.device.exclusive = false;
        if platform.system_cursor_showing() {
            return;
        }
        platform.set_cursor_position(self.x, self.y);
        platform.show_system_cursor(true);
    }

    /// `SetExclusive`: take the device exclusive and foreground once.
    fn take_exclusive(&mut self, platform: &mut dyn MousePlatform) -> bool {
        if !self.device.exclusive {
            self.unacquire_and_flush(platform);
            self.device.exclusive = platform.set_exclusive(true);
        }
        self.device.exclusive
    }

    /// `Acquire`: while the game window is in front. A fresh acquisition
    /// starts with empty data and the buttons as they are.
    fn acquire(&mut self, platform: &mut dyn MousePlatform) -> bool {
        if !self.device.acquired && platform.is_foreground() {
            self.device.acquired = platform.acquire();
            if self.device.acquired {
                self.device.flush();
                self.device.buttons = platform.buttons_down();
            }
        }
        self.device.acquired
    }

    /// A Raw Input report. Kept only while the device is acquired, as a
    /// DirectInput device keeps data only then; the buffer holds 128
    /// button records and drops the ones after.
    pub fn feed(&mut self, input: &MouseInput) {
        if !self.device.acquired {
            return;
        }
        let d = &mut self.device;
        d.dx = d.dx.saturating_add(input.dx);
        d.dy = d.dy.saturating_add(input.dy);
        d.dz = d.dz.saturating_add(input.wheel);
        for index in 0..5u32 {
            let down_flag = 1u16 << (2 * index);
            let up_flag = down_flag << 1;
            for (flag, down) in [(down_flag, true), (up_flag, false)] {
                if input.button_flags & flag == 0 {
                    continue;
                }
                let bit = 1u8 << index;
                if down {
                    d.buttons |= bit;
                } else {
                    d.buttons &= !bit;
                }
                if d.records.len() < BUFFER_ENTRIES {
                    d.records.push_back(ButtonRecord {
                        index,
                        down,
                        time: input.time,
                    });
                }
            }
        }
    }

    /// `FILD width / FMUL 0.00125 / FMUL speed`, unrounded.
    fn axis_scale(&self) -> f64 {
        f64::from(self.surface().0) * AXIS_SCALE * f64::from(self.speed)
    }

    /// The press bookkeeping of the button reads: a press within the
    /// double-click time of the previous press of this button, and within
    /// four pixels of the previous press of any button on each axis, is a
    /// double click; the button's time then moves back one double-click
    /// time so a third press does not pair with the second.
    fn detect_double_click(&mut self, index: usize, timestamp: u32) -> bool {
        let previous = self.last_press_time[index];
        let elapsed = timestamp.saturating_sub(previous);
        let dx = self.x.wrapping_sub(self.last_click.0);
        let dy = self.y.wrapping_sub(self.last_click.1);
        self.last_click = (self.x, self.y);
        if elapsed < self.double_click_time
            && dx.unsigned_abs() <= DOUBLE_CLICK_SLOP as u32
            && dy.unsigned_abs() <= DOUBLE_CLICK_SLOP as u32
        {
            self.last_press_time[index] = timestamp.wrapping_sub(self.double_click_time);
            return true;
        }
        self.last_press_time[index] = timestamp;
        false
    }

    fn on_button_change(&mut self, index: u32, down: bool, timestamp: u32) {
        let bit = 1u32 << index;
        if down {
            self.buttons |= bit;
        } else {
            self.buttons &= !bit;
        }
        let double_click = down && self.detect_double_click(index as usize, timestamp);
        self.events.push_back(MouseEvent::Button {
            index,
            down,
            double_click,
        });
    }

    /// `ClampOrRelease`: keep a moved cursor on the surface. Without the
    /// clamp (windowed) leaving the surface asks for the mouse to be
    /// released instead.
    fn clamp_or_release(&mut self, dx: i32, dy: i32) -> bool {
        let may_release = !self.clamp_to_surface;
        let (w, h) = self.surface();
        let mut release = false;
        let mut axis = |pos: &mut i32, size: i32| {
            if *pos < 0 {
                if may_release {
                    release = true;
                } else {
                    *pos = 0;
                }
            } else if *pos >= size {
                if may_release {
                    release = true;
                } else {
                    *pos = size - 1;
                }
            }
        };
        if dx != 0 {
            axis(&mut self.x, w);
        }
        if dy != 0 {
            axis(&mut self.y, h);
        }
        release
    }

    /// The input half of `MouseFrameUpdate` (the cursor drawing is the
    /// UI's): with the mouse active, take it exclusively and acquire it,
    /// read the buffered button records with their own times, then the
    /// summed motion and wheel and any button whose state disagrees,
    /// stamped `frame_ms` (`GetClientFrameTimestampMs`); queue a move when
    /// the cursor moved (releasing the mouse when a windowed cursor left
    /// the surface) and one event per wheel notch.
    ///
    /// The C++ does nothing at all here until the cursors are loaded
    /// (`MouseInitCursors`); the caller keeps that gate.
    pub fn frame_update(&mut self, platform: &mut dyn MousePlatform, frame_ms: u32) {
        if !(self.active && self.take_exclusive(platform) && self.acquire(platform)) {
            return;
        }
        let (start_x, start_y, start_z) = (self.x, self.y, self.z);

        // The buffer: buttons only (the axes come from the state below).
        while let Some(record) = self.device.records.pop_front() {
            self.on_button_change(record.index, record.down, record.time);
        }

        // The immediate state: the three axes, then the buttons.
        let (dx, dy, dz) = (self.device.dx, self.device.dy, self.device.dz);
        self.device.dx = 0;
        self.device.dy = 0;
        self.device.dz = 0;
        let scale = self.axis_scale();
        self.x = apply_axis(dx, scale, &mut self.remainder[0], self.x);
        self.y = apply_axis(dy, scale, &mut self.remainder[1], self.y);
        self.z = apply_axis(dz, f64::from(WHEEL_SCALE), &mut self.remainder[2], self.z);
        for index in 0..BUTTON_COUNT {
            let down = self.device.buttons & (1 << index) != 0;
            let held = self.buttons & (1 << index) != 0;
            if down != held {
                self.on_button_change(index, down, frame_ms);
            }
        }

        let dx = self.x.wrapping_sub(start_x);
        let dy = self.y.wrapping_sub(start_y);
        let mut wheel = self.z.wrapping_sub(start_z);
        if dx == 0 && dy == 0 {
            self.moved = false;
        } else {
            self.moved = true;
            if self.clamp_or_release(dx, dy) {
                self.set_active(false, platform);
            }
            self.events.push_back(MouseEvent::Moved);
        }

        // The wheel delta is published and queued one notch at a time.
        self.wheel = wheel;
        wheel = wheel.wrapping_abs();
        while wheel > 0 {
            self.events.push_back(MouseEvent::Button {
                index: WHEEL_BUTTON,
                down: self.wheel < 0,
                double_click: false,
            });
            wheel -= 1;
        }
    }

    /// Takes the queued events, oldest first (`MouseDispatchEvents` hands
    /// each to the game's router).
    pub fn drain_events(&mut self) -> Vec<MouseEvent> {
        self.events.drain(..).collect()
    }

    /// The queued events, oldest first.
    #[must_use]
    pub fn events(&self) -> &VecDeque<MouseEvent> {
        &self.events
    }

    /// The software cursor (`GetClientSoftwareCursor`): x, y and the wheel
    /// accumulator.
    #[must_use]
    pub fn cursor(&self) -> (i32, i32, i32) {
        (self.x, self.y, self.z)
    }

    /// `SetClientSoftwareCursor`.
    pub fn set_cursor(&mut self, x: i32, y: i32) {
        self.x = x;
        self.y = y;
    }

    /// The wheel delta of the last frame update (`GetClientMouseWheel`).
    #[must_use]
    pub fn wheel(&self) -> i32 {
        self.wheel
    }

    /// The button bits, bit n for button n held (`GetClientMouseButtons`).
    #[must_use]
    pub fn buttons(&self) -> u32 {
        self.buttons
    }

    /// Whether the last frame update moved the cursor.
    #[must_use]
    pub fn moved(&self) -> bool {
        self.moved
    }

    /// The perf option "mouse_speed" (`ApplyMouseSpeedOption`): the value
    /// clamped to 0..1, times 1.75, plus 0.25, rounded once to a float. A
    /// NaN passes through.
    pub fn apply_speed_option(&mut self, value: f32) {
        self.speed = speed_from_option(value);
    }

    /// The axis multiplier (`GetClientMouseSpeed`).
    #[must_use]
    pub fn speed(&self) -> f32 {
        self.speed
    }

    /// `GetClientDoubleClickTime`.
    #[must_use]
    pub fn double_click_time(&self) -> u32 {
        self.double_click_time
    }
}

/// `ApplyMouseSpeedOption`'s arithmetic.
#[must_use]
pub fn speed_from_option(value: f32) -> f32 {
    // A NaN fails both of the C++'s compares and stays NaN, as with clamp.
    (f64::from(value.clamp(0.0, 1.0)) * 1.75 + 0.25) as f32
}

/// One relative axis step (`ApplyAxis`): the count times the scale plus the
/// carried fraction; the whole part (truncated) moves the position and the
/// rest is stored back as a float.
fn apply_axis(value: i32, scale: f64, remainder: &mut f32, pos: i32) -> i32 {
    let moved = scale * f64::from(value) + f64::from(*remainder);
    let whole = trunc_to_i32(moved);
    *remainder = (moved - f64::from(whole)) as f32;
    pos.wrapping_add(whole)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A platform that records what the model asked of it.
    #[derive(Debug, Default)]
    struct Fake {
        pointer: Option<(i32, i32)>,
        showing: bool,
        focus: bool,
        foreground: bool,
        exclusive: bool,
        acquired: bool,
        buttons: u8,
        moves: Vec<(i32, i32)>,
        unacquires: usize,
    }

    impl MousePlatform for Fake {
        fn cursor_position(&self) -> Option<(i32, i32)> {
            self.pointer
        }
        fn set_cursor_position(&mut self, x: i32, y: i32) {
            self.moves.push((x, y));
            self.pointer = Some((x, y));
        }
        fn system_cursor_showing(&self) -> bool {
            self.showing
        }
        fn show_system_cursor(&mut self, show: bool) {
            self.showing = show;
        }
        fn has_focus(&self) -> bool {
            self.focus
        }
        fn is_foreground(&self) -> bool {
            self.foreground
        }
        fn set_exclusive(&mut self, exclusive: bool) -> bool {
            self.exclusive = exclusive;
            true
        }
        fn acquire(&mut self) -> bool {
            self.acquired = true;
            true
        }
        fn unacquire(&mut self) {
            self.acquired = false;
            self.unacquires += 1;
        }
        fn buttons_down(&self) -> u8 {
            self.buttons
        }
    }

    fn windowed(width: u32, height: u32) -> DisplayMode {
        DisplayMode {
            width,
            height,
            fullscreen: false,
        }
    }

    fn focused() -> Fake {
        Fake {
            pointer: Some((100, 50)),
            showing: true,
            focus: true,
            foreground: true,
            ..Fake::default()
        }
    }

    /// A mouse that has the focus and has been read once (acquired).
    fn acquired(mode: DisplayMode, platform: &mut Fake) -> Mouse {
        let mut mouse = Mouse::new(500, mode);
        mouse.on_focus_gained(platform);
        mouse.frame_update(platform, 0);
        assert!(mouse.is_acquired());
        mouse
    }

    fn input(dx: i32, dy: i32, wheel: i32, button_flags: u16, time: u32) -> MouseInput {
        MouseInput {
            dx,
            dy,
            wheel,
            button_flags,
            time,
        }
    }

    #[test]
    fn the_speed_option_maps_zero_to_one_onto_a_quarter_to_two() {
        assert_eq!(speed_from_option(0.0), 0.25);
        assert_eq!(speed_from_option(1.0), 2.0);
        assert_eq!(speed_from_option(0.5), 1.125);
        assert_eq!(speed_from_option(-3.0), 0.25);
        assert_eq!(speed_from_option(7.0), 2.0);
        assert!(speed_from_option(f32::NAN).is_nan());
        // 0.3 * 1.75 + 0.25 = 0.775, rounded once.
        assert_eq!(
            speed_from_option(0.3),
            (f64::from(0.3f32) * 1.75 + 0.25) as f32
        );
        assert_eq!(WHEEL_SCALE.to_bits(), 0x3c08_8889);
    }

    #[test]
    fn focus_takes_the_pointer_position_clamped_and_hides_the_cursor() {
        let mut p = focused();
        p.pointer = Some((900, -5));
        let mut mouse = Mouse::new(500, windowed(800, 600));
        mouse.on_focus_gained(&mut p);
        assert!(mouse.is_active() && mouse.input_active());
        assert_eq!(mouse.cursor(), (799, 0, 0));
        assert!(!p.showing);
        // Fullscreen keeps the software cursor where it was.
        let mut mouse = Mouse::new(
            500,
            DisplayMode {
                fullscreen: true,
                ..windowed(800, 600)
            },
        );
        mouse.set_cursor(10, 20);
        mouse.on_focus_gained(&mut p);
        assert_eq!(mouse.cursor(), (10, 20, 0));
    }

    #[test]
    fn mouse_moves_reactivate_only_while_input_is_active() {
        let mut p = focused();
        let mut mouse = Mouse::new(500, windowed(800, 600));
        mouse.on_mouse_move(&mut p);
        assert!(!mouse.is_active());
        mouse.on_focus_gained(&mut p);
        mouse.on_focus_lost(&mut p);
        assert!(!mouse.input_active());
        mouse.on_mouse_move(&mut p);
        assert!(!mouse.is_active());
    }

    #[test]
    fn a_windowed_session_without_the_focus_is_not_enabled() {
        let mut p = focused();
        p.focus = false;
        let mut mouse = Mouse::new(500, windowed(800, 600));
        mouse.set_enabled(true, &mut p);
        assert!(!mouse.is_active());
        p.focus = true;
        mouse.set_enabled(true, &mut p);
        assert!(mouse.is_active() && mouse.input_active());
        mouse.set_enabled(false, &mut p);
        assert!(!mouse.is_active() && !mouse.input_active());
    }

    #[test]
    fn reports_count_only_while_acquired_and_foreground() {
        let mut p = focused();
        let mut mouse = Mouse::new(500, windowed(800, 600));
        mouse.feed(&input(10, 0, 0, 0, 0));
        mouse.on_focus_gained(&mut p);
        p.foreground = false;
        mouse.frame_update(&mut p, 0);
        assert!(!mouse.is_acquired());
        mouse.feed(&input(10, 0, 0, 0, 0));
        p.foreground = true;
        mouse.frame_update(&mut p, 0);
        assert!(mouse.is_acquired() && p.exclusive && p.acquired);
        assert_eq!(mouse.cursor(), (100, 50, 0));
        assert!(mouse.drain_events().is_empty());
    }

    #[test]
    fn motion_scales_with_the_surface_width_and_carries_fractions() {
        let mut p = focused();
        // 1024 wide: 1.28 pixels per count.
        let mut mouse = acquired(windowed(1024, 768), &mut p);
        let mut xs = Vec::new();
        for _ in 0..4 {
            mouse.feed(&input(1, 0, 0, 0, 0));
            mouse.frame_update(&mut p, 0);
            xs.push(mouse.cursor().0);
        }
        // 1.28, 2.56, 3.84, 5.12 (each fraction rounded to a float).
        assert_eq!(xs, [101, 102, 103, 105]);
        assert_eq!(
            mouse.drain_events(),
            vec![MouseEvent::Moved; 4],
            "one move per frame that moved"
        );
        // Negative counts truncate toward zero.
        mouse.apply_speed_option(0.0);
        mouse.feed(&input(-3, -3, 0, 0, 0));
        mouse.frame_update(&mut p, 0);
        // 1024 * 0.00125 * 0.25 = 0.32 per count: -0.96 plus the carried
        // x fraction.
        let (x, y, _) = mouse.cursor();
        assert_eq!((x, y), (105, 50));
        assert!(!mouse.moved());
        assert!(mouse.drain_events().is_empty());
    }

    #[test]
    fn buffered_presses_detect_double_clicks() {
        let mut p = focused();
        let mut mouse = acquired(windowed(800, 600), &mut p);
        // Left down, up, down 300 ms later, up, then a third press.
        mouse.feed(&input(0, 0, 0, 0x1, 1000));
        mouse.feed(&input(0, 0, 0, 0x2, 1100));
        mouse.feed(&input(0, 0, 0, 0x1, 1300));
        mouse.feed(&input(0, 0, 0, 0x2, 1350));
        mouse.feed(&input(0, 0, 0, 0x1, 1400));
        mouse.frame_update(&mut p, 9999);
        let ev = mouse.drain_events();
        let b = |down, double_click| MouseEvent::Button {
            index: 0,
            down,
            double_click,
        };
        assert_eq!(
            ev,
            [
                b(true, false),
                b(false, false),
                b(true, true),
                b(false, false),
                b(true, false)
            ]
        );
        assert_eq!(mouse.buttons(), 1);
        assert_eq!(ev[0].key(), -1);
        // Right button (raw button 2) is index 1, key -2; the slop is per
        // axis from the previous press of any button.
        mouse.feed(&input(0, 0, 0, 0x4, 1500));
        mouse.feed(&input(0, 0, 0, 0x8, 1510));
        mouse.frame_update(&mut p, 0);
        assert_eq!(mouse.drain_events()[0].key(), -2);
        mouse.set_cursor(105, 50);
        mouse.feed(&input(0, 0, 0, 0x4, 1520));
        mouse.frame_update(&mut p, 0);
        let ev = mouse.drain_events();
        assert_eq!(
            ev[0],
            MouseEvent::Button {
                index: 1,
                down: true,
                double_click: false
            },
            "five pixels from the last press is too far"
        );
    }

    #[test]
    fn the_wheel_queues_one_event_per_notch() {
        let mut p = focused();
        let mut mouse = acquired(windowed(800, 600), &mut p);
        mouse.feed(&input(0, 0, 240, 0x400, 0));
        mouse.frame_update(&mut p, 0);
        assert_eq!(mouse.wheel(), 2);
        let up = MouseEvent::Button {
            index: WHEEL_BUTTON,
            down: false,
            double_click: false,
        };
        assert_eq!(mouse.drain_events(), [up, up]);
        assert!(up.is_wheel() && up.key() == -9);
        // The carried fraction is the float scale's: 240 counts left
        // +1e-7, so two half notches back are -0.99999993, not a notch.
        mouse.feed(&input(0, 0, -60, 0x400, 0));
        mouse.feed(&input(0, 0, -60, 0x400, 0));
        mouse.frame_update(&mut p, 0);
        assert_eq!(mouse.wheel(), 0);
        // From no fraction, half a notch carries over.
        let mut mouse = acquired(windowed(800, 600), &mut p);
        mouse.feed(&input(0, 0, -60, 0x400, 0));
        mouse.frame_update(&mut p, 0);
        assert_eq!(mouse.wheel(), 0);
        mouse.feed(&input(0, 0, -60, 0x400, 0));
        mouse.frame_update(&mut p, 0);
        assert_eq!(mouse.wheel(), -1);
        assert_eq!(
            mouse.drain_events(),
            [MouseEvent::Button {
                index: WHEEL_BUTTON,
                down: true,
                double_click: false
            }]
        );
        assert_eq!(mouse.cursor().2, -1);
    }

    #[test]
    fn a_windowed_cursor_leaving_the_surface_releases_the_mouse() {
        let mut p = focused();
        p.pointer = Some((795, 300));
        let mut mouse = acquired(windowed(800, 600), &mut p);
        mouse.feed(&input(0, 0, 0, 0x1, 10));
        mouse.feed(&input(10, 0, 0, 0, 0));
        mouse.frame_update(&mut p, 0);
        // The release flushed the press; the move is queued after it.
        assert_eq!(mouse.drain_events(), [MouseEvent::Moved]);
        assert!(!mouse.is_active() && !mouse.is_acquired());
        assert!(mouse.input_active(), "input stays active for the next move");
        assert!(!p.acquired && !p.exclusive);
        // The system pointer appears where the software cursor went.
        assert_eq!(p.moves, [(805, 300)]);
        assert!(p.showing);
        // The pointer coming back takes the mouse again.
        p.pointer = Some((790, 310));
        mouse.on_mouse_move(&mut p);
        assert!(mouse.is_active());
        assert_eq!(mouse.cursor().0, 790);
    }

    #[test]
    fn fullscreen_clamps_instead() {
        let mut p = focused();
        let mode = DisplayMode {
            width: 800,
            height: 600,
            fullscreen: true,
        };
        let mut mouse = Mouse::new(500, mode);
        mouse.set_enabled(true, &mut p);
        mouse.frame_update(&mut p, 0);
        mouse.set_cursor(795, 2);
        mouse.feed(&input(10, -10, 0, 0, 0));
        mouse.frame_update(&mut p, 0);
        assert_eq!(mouse.cursor(), (799, 0, 0));
        assert!(mouse.is_active());
        assert!(p.moves.is_empty());
    }

    #[test]
    fn losing_the_focus_shows_the_pointer_at_the_cursor_unless_it_shows() {
        let mut p = focused();
        let mut mouse = acquired(windowed(800, 600), &mut p);
        mouse.set_cursor(321, 123);
        mouse.on_focus_lost(&mut p);
        assert_eq!(p.moves, [(321, 123)]);
        assert!(p.showing && !p.acquired);
        let mut mouse = acquired(windowed(800, 600), &mut p);
        p.showing = true;
        p.moves.clear();
        mouse.on_focus_lost(&mut p);
        assert!(p.moves.is_empty());
    }

    #[test]
    fn acquisition_reads_the_buttons_and_reports_changes_made_meanwhile() {
        let mut p = focused();
        p.buttons = 0b100;
        let mut mouse = Mouse::new(500, windowed(800, 600));
        mouse.on_focus_gained(&mut p);
        mouse.frame_update(&mut p, 77);
        assert_eq!(
            mouse.drain_events(),
            [MouseEvent::Button {
                index: 2,
                down: true,
                double_click: false
            }]
        );
        assert_eq!(mouse.buttons(), 0b100);
        // Released while away: the button bits keep the press, and the
        // next acquisition reports the release.
        mouse.on_focus_lost(&mut p);
        p.buttons = 0;
        mouse.on_focus_gained(&mut p);
        mouse.frame_update(&mut p, 78);
        assert_eq!(
            mouse.drain_events(),
            [MouseEvent::Button {
                index: 2,
                down: false,
                double_click: false
            }]
        );
    }

    #[test]
    fn the_buffer_keeps_128_records() {
        let mut p = focused();
        let mut mouse = acquired(windowed(800, 600), &mut p);
        for i in 0..100u32 {
            mouse.feed(&input(0, 0, 0, 0x1 | 0x2, i));
        }
        mouse.frame_update(&mut p, 0);
        assert_eq!(mouse.drain_events().len(), BUFFER_ENTRIES);
    }
}
