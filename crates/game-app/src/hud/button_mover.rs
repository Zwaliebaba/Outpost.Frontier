//! `ButtonMover` and `ButtonScaler` (`button_mover.cpp`): the two timed
//! animators a window inherits to slide a set of controls between two
//! offsets, or to scale their mesh instances, over a fixed time. The
//! reference shares them between several windows (the NeuroNet main
//! button, the navigation bar, the HUD's target and status panels).
//!
//! The reference's `OnFinished` slot is the owner's: [`ButtonMover::update`]
//! and [`ButtonScaler::update`] answer true on the frame the animation
//! completes, and the owner then runs its handler. The `OnStep` slots the
//! target panel does not override (they answer false), so the default step
//! is applied here.

use neuron_client::ui::tree::{NodeId, Property, Ui};
use neuron_core::math::{Matrix, Transform, Vector};

/// A double expression stored to a `float` (T11).
#[allow(clippy::cast_possible_truncation)]
fn r(v: f64) -> f32 {
    v as f32
}

/// `pi` as the reference stores it (0x40490fdb).
const PI: f32 = std::f32::consts::PI;

/// The elapsed time advanced by the frame (`+0x14`, kept as a float); true
/// when the unrounded sum reached the duration, which then snaps.
fn advance(elapsed: &mut f32, duration: f32, frame_time: f64) -> bool {
    let sum = frame_time + f64::from(*elapsed);
    *elapsed = r(sum);
    if sum < f64::from(duration) {
        false
    } else {
        *elapsed = duration;
        true
    }
}

/// One slide (`FButtonMoveEntry`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MoveEntry {
    pub start: Vector,
    pub end: Vector,
    pub control: Option<NodeId>,
}

/// `ButtonMover`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ButtonMover {
    pub entries: Vec<MoveEntry>,
    pub elapsed: f32,
    pub active: bool,
    pub duration: f32,
}

impl ButtonMover {
    /// `Start`: the duration, active, the elapsed time zeroed, the list
    /// emptied.
    pub fn start(&mut self, seconds: f32) {
        self.duration = seconds;
        self.active = true;
        self.elapsed = 0.0;
        self.entries.clear();
    }

    /// `AddButton`.
    pub fn add_button(&mut self, control: Option<NodeId>, start: Vector, end: Vector) {
        self.entries.push(MoveEntry {
            start,
            end,
            control,
        });
    }

    /// `IsActive`.
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.active
    }

    /// `Update`: the time advanced by the frame, each control moved to
    /// `start + (end - start) * t` through its property 6; true when the
    /// move completed this frame (the caller's `OnMoveFinished`).
    pub fn update(&mut self, ui: &mut Ui, frame_time: f64) -> bool {
        if !self.active {
            return false;
        }
        if advance(&mut self.elapsed, self.duration, frame_time) {
            self.active = false;
        }
        let t = r(f64::from(self.elapsed) / f64::from(self.duration));
        let t64 = f64::from(t);
        for entry in self.entries.clone() {
            let (s, e) = (entry.start, entry.end);
            let dz = r(f64::from(e.z) - f64::from(s.z));
            let dx_t = r((f64::from(e.x) - f64::from(s.x)) * t64);
            let dy_t = r((f64::from(e.y) - f64::from(s.y)) * t64);
            let position = Vector::new(
                r(f64::from(dx_t) + f64::from(s.x)),
                r(f64::from(dy_t) + f64::from(s.y)),
                r(f64::from(dz) * t64 + f64::from(s.z)),
            );
            if let Some(control) = entry.control {
                ui.property(control, Property::Offset(position));
            }
        }
        !self.active
    }
}

/// One scale (`FButtonScaleEntry`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScaleEntry {
    pub from: f32,
    pub to: f32,
    pub control: Option<NodeId>,
}

/// `ButtonScaler`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ButtonScaler {
    pub entries: Vec<ScaleEntry>,
    pub elapsed: f32,
    pub active: bool,
    pub duration: f32,
}

impl ButtonScaler {
    /// `Start`.
    pub fn start(&mut self, seconds: f32) {
        self.duration = seconds;
        self.active = true;
        self.elapsed = 0.0;
        self.entries.clear();
    }

    /// `AddButton`.
    pub fn add_button(&mut self, control: Option<NodeId>, from: f32, to: f32) {
        self.entries.push(ScaleEntry { from, to, control });
    }

    /// `IsActive`.
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.active
    }

    /// The eased scale of the elapsed time, `1 - (cos(t pi) + 1) / 2`.
    #[must_use]
    pub fn ease(&self) -> f32 {
        let phase =
            f64::from(r(f64::from(self.elapsed) / f64::from(self.duration))) * f64::from(PI);
        r(1.0 - (phase.cos() + 1.0) * 0.5)
    }

    /// `Update`: the time advanced by the frame, each control's mesh
    /// instance given the identity rotation scaled by `from + (to - from) *
    /// s` at its current translation; true when the scale completed this
    /// frame (the caller's `OnScaleFinished`).
    pub fn update(&mut self, ui: &mut Ui, frame_time: f64) -> bool {
        if !self.active {
            return false;
        }
        if advance(&mut self.elapsed, self.duration, frame_time) {
            self.active = false;
        }
        let s = f64::from(self.ease());
        for entry in self.entries.clone() {
            let scale =
                r((f64::from(entry.to) - f64::from(entry.from)) * s + f64::from(entry.from));
            let Some(control) = entry.control else {
                continue;
            };
            let Some(mesh) = ui.win_control_mut(control).and_then(|c| c.mesh.as_mut()) else {
                continue;
            };
            let translation = mesh.transform().translation;
            let rotation = Matrix::from_rows(
                Matrix::IDENTITY
                    .m
                    .map(|row| row.map(|v| r(f64::from(v) * f64::from(scale)))),
            );
            mesh.set_transform(Transform {
                rotation,
                translation,
            });
        }
        !self.active
    }
}

#[cfg(test)]
mod tests {
    use neuron_client::ui::widescreen::Surface;

    use super::*;

    #[test]
    fn the_mover_snaps_to_the_end_when_the_time_is_up() {
        let mut ui = Ui::new(Surface::new(1024, 768));
        let mut mover = ButtonMover::default();
        mover.start(0.3);
        mover.add_button(
            None,
            Vector::new(-0.4105, -0.37, -1.0),
            Vector::new(-0.4105, -0.4, -1.0),
        );
        let mut frames = 0;
        while !mover.update(&mut ui, 1.0 / 30.0) {
            frames += 1;
            assert!(mover.is_active());
        }
        assert_eq!(frames, 8);
        assert!(!mover.is_active());
        assert!((mover.elapsed - 0.3).abs() < f32::EPSILON);
        assert!(!mover.update(&mut ui, 1.0 / 30.0));
    }

    #[test]
    fn the_scaler_eases_from_zero_to_one() {
        let mut scaler = ButtonScaler::default();
        scaler.start(0.3);
        assert!(scaler.ease().abs() < 1e-6);
        scaler.elapsed = 0.15;
        assert!((scaler.ease() - 0.5).abs() < 1e-6);
        scaler.elapsed = 0.3;
        assert!((scaler.ease() - 1.0).abs() < f32::EPSILON);
    }
}
