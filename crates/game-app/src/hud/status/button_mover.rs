//! `ButtonMover` and `ButtonScaler` (`button_mover.cpp`): the two timed
//! animators `HUD_Status` inherits, one sliding a set of controls, the
//! other scaling them, over a fixed time. The owner calls `update` from its
//! own update and runs its finished handler when `update` answers true
//! (the reference's slot 0, `OnMoveFinished` / `OnScaleFinished`). Neither
//! of `HUD_Status`'s faces overrides slot 1, so every step goes to the
//! controls.

use neuron_client::ui::tree::{NodeId, Property, Ui};
use neuron_core::math::Vector;

/// A mover entry: start, end, control.
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

fn r(v: f64) -> f32 {
    v as f32
}

/// `Elapsed += frame time`, snapped to the duration (and the animation
/// stopped) when the unrounded sum is not below it.
fn advance(elapsed: &mut f32, active: &mut bool, duration: f32, frame_time: f64) {
    let sum = frame_time + f64::from(*elapsed);
    *elapsed = r(sum);
    // The snap is taken on NaN too (`FCOMP` / `TEST AH,1`).
    if sum.partial_cmp(&f64::from(duration)) != Some(std::cmp::Ordering::Less) {
        *elapsed = duration;
        *active = false;
    }
}

impl ButtonMover {
    /// `Start`: the duration, active, elapsed 0, the list emptied.
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

    /// `Update`: the time advanced and every control put at its share of
    /// the way (`SetProperty(6)`); true when the move ended in this call
    /// (the caller's `OnMoveFinished`).
    pub fn update(&mut self, ui: &mut Ui, frame_time: f64) -> bool {
        if !self.active {
            return false;
        }
        advance(
            &mut self.elapsed,
            &mut self.active,
            self.duration,
            frame_time,
        );
        let t = f64::from(r(f64::from(self.elapsed) / f64::from(self.duration)));
        for e in &self.entries {
            let dz = r(f64::from(e.end.z) - f64::from(e.start.z));
            let dxt = r((f64::from(e.end.x) - f64::from(e.start.x)) * t);
            let dyt = r((f64::from(e.end.y) - f64::from(e.start.y)) * t);
            let position = Vector::new(
                r(f64::from(dxt) + f64::from(e.start.x)),
                r(f64::from(dyt) + f64::from(e.start.y)),
                r(f64::from(dz) * t + f64::from(e.start.z)),
            );
            if let Some(control) = e.control {
                ui.property(control, Property::Offset(position));
            }
        }
        !self.active
    }
}

/// A scaler entry: from, to, control.
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

    /// The scale of each entry at the current time: `1 - (cos(t pi) + 1) /
    /// 2` of the way from `from` to `to`.
    #[must_use]
    pub fn scales(&self) -> Vec<f32> {
        let phase = f64::from(r(f64::from(self.elapsed) / f64::from(self.duration)))
            * f64::from(std::f32::consts::PI);
        let s = r(1.0 - (phase.cos() + 1.0) * 0.5);
        self.entries
            .iter()
            .map(|e| r((f64::from(e.to) - f64::from(e.from)) * f64::from(s) + f64::from(e.from)))
            .collect()
    }

    /// `Update`: the time advanced; true when the scale ended in this call
    /// (the caller's `OnScaleFinished`). The scale goes to each control's
    /// engine instance in the reference (`SetInstanceMatrix`), which the
    /// interface tree does not hold: [`ButtonScaler::scales`] gives it to
    /// the HUD's draw.
    pub fn update(&mut self, frame_time: f64) -> bool {
        if !self.active {
            return false;
        }
        advance(
            &mut self.elapsed,
            &mut self.active,
            self.duration,
            frame_time,
        );
        !self.active
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_scaler_ends_on_the_frame_its_time_reaches_the_duration() {
        let mut s = ButtonScaler::default();
        s.start(0.3);
        s.add_button(None, 0.1, 1.0);
        let dt = 1.0 / 30.0;
        let mut frames = 0;
        while !s.update(dt) {
            frames += 1;
            assert!(frames < 20);
        }
        assert!(!s.active);
        assert_eq!(s.elapsed, 0.3);
        assert_eq!(s.scales(), [1.0]);
    }
}
