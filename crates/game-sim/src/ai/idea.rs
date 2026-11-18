//! The idea interface (`class BaseIdea`, `include/common_idea.h`,
//! `Common/common_idea.cpp`): the small steering units behaviours own and
//! tick (move, face, spline move, match throttle, dodge, break away), and
//! the simple ideas of `common_idea.cpp` (stop, afterburner, idle) with the
//! direction picker the dodge ideas share.
//!
//! `BaseIdea` is a six-slot vtable: `SetContext` (0), `Update` (1), an
//! unnamed tick hook (2), `GetKind` (3), `Commit` (4) and an unnamed query
//! (5, always false). [`Idea`] keeps 0, 1, 2, 3 and 4.

use super::behavior::{AiEnv, Cx};
use super::world::AiRand;

/// `BaseIdea`'s own fields (`+0x04..+0x10`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct IdeaBase {
    /// +0x04 the owner is bound (the reference's object pointer is
    /// non-null): the owner's id.
    pub owner: Option<u32>,
    /// +0x10 `SetContext` succeeded.
    pub attached: bool,
}

impl IdeaBase {
    /// `BaseIdea::SetContext`: refused without an owner (the system
    /// manager is always there in the port).
    pub fn bind(&mut self, owner: Option<u32>) -> bool {
        if owner.is_none() {
            return false;
        }
        self.owner = owner;
        self.attached = true;
        true
    }
}

/// One idea. The update answers 0 when done, 1 while working, 3 without
/// an owner (the reference's convention; each idea documents its own).
pub trait Idea<W: AiEnv + ?Sized> {
    /// The shared fields.
    fn base(&self) -> &IdeaBase;

    /// The shared fields, mutably.
    fn base_mut(&mut self) -> &mut IdeaBase;

    /// (0) `SetContext(Object, Handle, Context)`.
    fn bind(&mut self, owner: Option<u32>) -> bool {
        self.base_mut().bind(owner)
    }

    /// (1) `Update(DeltaTime)`.
    fn update(&mut self, cx: &mut Cx<'_, W>, dt: f32) -> i32;

    /// (2) the tick hook (a bare `RET 4` in every idea but match
    /// throttle).
    fn tick(&mut self, cx: &mut Cx<'_, W>, dt: f32) {
        let _ = (cx, dt);
    }

    /// (3) `GetKind`.
    fn kind(&self) -> i16;

    /// (4) `Commit`: what the owning behaviour calls when it lets go.
    fn commit(&mut self, cx: &mut Cx<'_, W>);
}

/// `BaseIdea::PublishTargetId`: the pathfinder's published target.
pub fn publish_target_id<W: AiEnv + ?Sized>(cx: &mut Cx<'_, W>, id: u32) {
    cx.state.pathfind.target_id = id;
}

/// `StopIdea`: binds without checks and does nothing.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StopIdea {
    base: IdeaBase,
}

impl<W: AiEnv + ?Sized> Idea<W> for StopIdea {
    fn base(&self) -> &IdeaBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut IdeaBase {
        &mut self.base
    }

    fn bind(&mut self, _owner: Option<u32>) -> bool {
        true
    }

    fn update(&mut self, _cx: &mut Cx<'_, W>, _dt: f32) -> i32 {
        0
    }

    fn kind(&self) -> i16 {
        2
    }

    fn commit(&mut self, _cx: &mut Cx<'_, W>) {}
}

/// `AfterburnerIdea`: thrusters on at full throttle; off on commit.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AfterburnerIdea {
    base: IdeaBase,
}

impl<W: AiEnv + ?Sized> Idea<W> for AfterburnerIdea {
    fn base(&self) -> &IdeaBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut IdeaBase {
        &mut self.base
    }

    fn update(&mut self, cx: &mut Cx<'_, W>, _dt: f32) -> i32 {
        if self.base.owner.is_none() {
            return 3;
        }
        cx.set_thrusters_active(true);
        cx.set_throttle(1.0, 0);
        0
    }

    fn kind(&self) -> i16 {
        2
    }

    fn commit(&mut self, cx: &mut Cx<'_, W>) {
        cx.set_thrusters_active(false);
    }
}

/// `IdleIdea`: stops the ship every update; its commit does nothing.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct IdleIdea {
    base: IdeaBase,
}

impl<W: AiEnv + ?Sized> Idea<W> for IdleIdea {
    fn base(&self) -> &IdeaBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut IdeaBase {
        &mut self.base
    }

    fn update(&mut self, cx: &mut Cx<'_, W>, _dt: f32) -> i32 {
        if self.base.owner.is_none() {
            return 3;
        }
        cx.stop();
        0
    }

    fn kind(&self) -> i16 {
        0x12
    }

    fn commit(&mut self, _cx: &mut Cx<'_, W>) {}
}

/// `FDirectionPicker`, the dodge ideas' shared block: the last strafe
/// direction picked, four weights and the watched object.
#[derive(Clone, Debug, PartialEq)]
pub struct DirectionPicker {
    /// +0x00 the last direction (1 left, 2 right, 3 up, 4 down; 0 none).
    pub last: i32,
    /// +0x04 left.
    pub weight_left: f32,
    /// +0x08 right.
    pub weight_right: f32,
    /// +0x0c up.
    pub weight_up: f32,
    /// +0x10 down.
    pub weight_down: f32,
    /// +0x14 the watched object's id.
    pub watched: Option<u32>,
    /// +0x1c set by `SetWatchObject`.
    pub flag_1c: u8,
}

impl Default for DirectionPicker {
    fn default() -> Self {
        Self {
            last: 0,
            weight_left: 1.0,
            weight_right: 1.0,
            weight_up: 1.0,
            weight_down: 1.0,
            watched: None,
            flag_1c: 0,
        }
    }
}

impl DirectionPicker {
    /// `SetWatchObject`.
    pub fn set_watch_object(&mut self, object: Option<u32>) {
        self.watched = object;
        self.flag_1c = 1;
    }

    /// `SetWeights`.
    pub fn set_weights(&mut self, left: f32, right: f32, up: f32, down: f32) {
        self.weight_left = left;
        self.weight_right = right;
        self.weight_up = up;
        self.weight_down = down;
    }

    /// `GenericWeightedPick`: a weighted draw among the four directions,
    /// the last one excluded; one `rand()`.
    pub fn pick(&mut self, rand: &mut (impl AiRand + ?Sized)) -> i32 {
        let tag = self.last;
        let w1 = if tag == 1 { 0.0 } else { self.weight_left };
        let w2 = if tag == 2 { 0.0 } else { self.weight_right };
        let w3 = if tag == 3 { 0.0 } else { self.weight_up };
        let w4 = if tag == 4 { 0.0 } else { self.weight_down };
        let (w1, w2, w3, w4) = (f64::from(w1), f64::from(w2), f64::from(w3), f64::from(w4));
        let draw = (f64::from(rand.rand()) * (((w4 + w3) + w2) + w1)) * (1.0 / 32768.0);
        let pick = if draw < w1 {
            1
        } else if draw < w2 + w1 {
            2
        } else if draw < (w3 + w2) + w1 {
            3
        } else {
            4
        };
        self.last = pick;
        pick
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ai::world::RecordedRand;

    #[test]
    fn pick_skips_the_last_direction() {
        let mut p = DirectionPicker::default();
        let mut r = RecordedRand::new(vec![0, 0, 32767]);
        assert_eq!(p.pick(&mut r), 1);
        assert_eq!(p.pick(&mut r), 2);
        assert_eq!(p.pick(&mut r), 4);
        p.set_weights(0.0, 0.0, 1.0, 0.0);
        p.last = 0;
        let mut r = RecordedRand::new(vec![100]);
        assert_eq!(p.pick(&mut r), 3);
    }
}
