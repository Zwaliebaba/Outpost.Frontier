//! Raw Input mouse reports (`RAWMOUSE`) turned into the relative motion,
//! wheel and button transitions the DirectInput 8 system mouse gave the
//! C++ client.
//!
//! A mouse reports relative counts without pointer ballistics, as
//! DirectInput did. Some devices (pens, touch screens, remote sessions)
//! report absolute positions instead; their motion is the change of the
//! position in screen pixels since the last report, which is what
//! DirectInput's system mouse reported for them.

use crate::input::mouse::MouseInput;
use crate::window::Rect;

/// `usFlags`: the position is absolute (0..65535 across the screen).
pub const MOUSE_MOVE_ABSOLUTE: u16 = 0x01;
/// `usFlags`: an absolute position spans the virtual desktop.
pub const MOUSE_VIRTUAL_DESKTOP: u16 = 0x02;
/// `usButtonFlags`: the wheel turned by `usButtonData` (signed).
pub const RI_MOUSE_WHEEL: u16 = 0x0400;
/// `usButtonFlags`: the five button transitions, down then up for each of
/// left, right, middle, extra 1 and extra 2.
pub const RI_MOUSE_BUTTONS: u16 = 0x03ff;

/// The fields of one `RAWMOUSE` the model reads.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RawMouse {
    pub flags: u16,
    pub button_flags: u16,
    pub button_data: u16,
    pub last_x: i32,
    pub last_y: i32,
}

/// Turns reports into [`MouseInput`], keeping the last absolute position.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RawMouseDecoder {
    last_absolute: Option<(i32, i32)>,
}

impl RawMouseDecoder {
    /// Decodes one report. `primary` and `virtual_desktop` are the
    /// screen rectangles an absolute position spans (the primary monitor,
    /// or the whole desktop with [`MOUSE_VIRTUAL_DESKTOP`]); `time` is the
    /// message time in milliseconds.
    pub fn decode(
        &mut self,
        raw: RawMouse,
        primary: Rect,
        virtual_desktop: Rect,
        time: u32,
    ) -> MouseInput {
        let (dx, dy) = if raw.flags & MOUSE_MOVE_ABSOLUTE != 0 {
            let area = if raw.flags & MOUSE_VIRTUAL_DESKTOP != 0 {
                virtual_desktop
            } else {
                primary
            };
            let x = absolute_to_pixel(raw.last_x, area.left, area.width());
            let y = absolute_to_pixel(raw.last_y, area.top, area.height());
            let delta = self
                .last_absolute
                .map_or((0, 0), |(px, py)| (x.wrapping_sub(px), y.wrapping_sub(py)));
            self.last_absolute = Some((x, y));
            delta
        } else {
            self.last_absolute = None;
            (raw.last_x, raw.last_y)
        };
        let wheel = if raw.button_flags & RI_MOUSE_WHEEL != 0 {
            i32::from(raw.button_data as i16)
        } else {
            0
        };
        MouseInput {
            dx,
            dy,
            wheel,
            button_flags: raw.button_flags & RI_MOUSE_BUTTONS,
            time,
        }
    }
}

/// A normalised coordinate (0..65535) as a pixel of the area.
fn absolute_to_pixel(value: i32, origin: i32, size: i32) -> i32 {
    let pixel = i64::from(value) * i64::from(size) / 65535;
    i32::try_from(pixel).unwrap_or(0).wrapping_add(origin)
}

#[cfg(test)]
mod tests {
    use super::*;

    const PRIMARY: Rect = Rect {
        left: 0,
        top: 0,
        right: 1920,
        bottom: 1080,
    };
    const DESKTOP: Rect = Rect {
        left: -1280,
        top: 0,
        right: 1920,
        bottom: 1080,
    };

    #[test]
    fn relative_reports_pass_through_with_the_wheel_and_buttons() {
        let mut d = RawMouseDecoder::default();
        let raw = RawMouse {
            flags: 0,
            button_flags: 0x0001 | 0x0400 | 0x0800,
            button_data: (-120i16) as u16,
            last_x: -3,
            last_y: 7,
        };
        let input = d.decode(raw, PRIMARY, DESKTOP, 1234);
        assert_eq!(
            input,
            MouseInput {
                dx: -3,
                dy: 7,
                wheel: -120,
                button_flags: 0x0001,
                time: 1234
            }
        );
        // The data word is a wheel only with the wheel flag.
        let raw = RawMouse {
            button_flags: 0x0800,
            button_data: 120,
            ..raw
        };
        assert_eq!(d.decode(raw, PRIMARY, DESKTOP, 0).wheel, 0);
    }

    #[test]
    fn absolute_reports_become_pixel_deltas() {
        let mut d = RawMouseDecoder::default();
        let at = |x: i32, y: i32, flags: u16| RawMouse {
            flags: MOUSE_MOVE_ABSOLUTE | flags,
            last_x: x,
            last_y: y,
            ..RawMouse::default()
        };
        // The first absolute report only sets the position.
        assert_eq!(d.decode(at(0, 0, 0), PRIMARY, DESKTOP, 0).dx, 0);
        let input = d.decode(at(65535, 32768, 0), PRIMARY, DESKTOP, 0);
        assert_eq!((input.dx, input.dy), (1920, 540));
        // Across the virtual desktop, from its left edge.
        let mut d = RawMouseDecoder::default();
        d.decode(at(0, 0, MOUSE_VIRTUAL_DESKTOP), PRIMARY, DESKTOP, 0);
        let input = d.decode(at(65535, 0, MOUSE_VIRTUAL_DESKTOP), PRIMARY, DESKTOP, 0);
        assert_eq!(input.dx, 3200);
        // A relative report forgets the position.
        d.decode(RawMouse::default(), PRIMARY, DESKTOP, 0);
        assert_eq!(d.decode(at(100, 100, 0), PRIMARY, DESKTOP, 0).dx, 0);
    }
}
