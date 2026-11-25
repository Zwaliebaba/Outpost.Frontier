//! The NPC guides (TU37, `Content/Guides.cpp`, `GotoGuides.cpp`,
//! `PatrolGuides.cpp`; record 0008 point 7; work package P3.6c): the
//! long-running behaviour of an NPC ship (`FContentNpcShip +0x178`), which
//! gives the ship `[ObjList]` directives and hears its events.
//!
//! This part of P3.6c (P3.6c1) brings the guide's place in the NPC ship:
//! its description and its reference, and the calls the ship makes. The
//! guides themselves come with P3.6c2; until then no ship has one, and the
//! directives a ship runs are only those its parameter block gives it.

use game_sim::ai::content::FleeAnswer;

use super::directives::DirectiveRef;
use super::ships::ShipKey;
use crate::events::NpcShipKey;
use crate::server::Cx;

/// A guide description (`FGuideDescBase`): the guide type to build.
#[derive(Clone, Debug, PartialEq)]
pub struct GuideDesc {
    /// `Type` (`+0x08`): 0 none, 1 to 6 the guide kinds.
    pub kind: i32,
}

/// A guide in Content's arena (`FContentGuide`, counted).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct GuideRef(pub u32);

/// The guide part of `FContentNpcShip`'s constructor (`NpcShip.cpp:368-403`):
/// a description of type 0 drops the ship's guide; any other builds one
/// (`CreateGuide`) that starts when the ship has no directives (and, with a
/// guide target, when the target is a station, a planet or a jump object).
pub fn attach_from_params(cx: &mut Cx<'_>, ship: NpcShipKey, desc: &GuideDesc, target: u32) {
    let _ = (cx, ship, desc, target);
}

/// The ship's reference on its guide released.
pub fn release(cx: &mut Cx<'_>, guide: GuideRef) {
    let _ = (cx, guide);
}

/// `Execute(Ship)` (slot 3) of a directive: false when it finished at
/// once.
pub fn execute_directive(cx: &mut Cx<'_>, ship: ShipKey, directive: DirectiveRef) -> bool {
    let _ = (cx, ship, directive);
    false
}

/// The guide's `OnDirectivesDone(Word11c, Word118)` (slot 6).
pub fn on_directives_done(cx: &mut Cx<'_>, guide: GuideRef, result: (i32, i32)) {
    let _ = (cx, guide, result);
}

/// The guide's `OnScanStateChange(bScanning)` (slot 11).
pub fn on_scan_state_change(cx: &mut Cx<'_>, guide: GuideRef, scanning: bool) {
    let _ = (cx, guide, scanning);
}

/// The read-only part of the guide's `RequestFleeDestination` (slot 12).
#[must_use]
pub fn flee_answer(cx: &Cx<'_>, guide: GuideRef, ship: NpcShipKey) -> Option<FleeAnswer> {
    let _ = (cx, guide, ship);
    None
}

/// The guide's `RequestFleeDestination` with its effects, after the tick.
pub fn request_flee_destination(
    cx: &mut Cx<'_>,
    guide: GuideRef,
    ship: NpcShipKey,
) -> Option<FleeAnswer> {
    let _ = (cx, guide, ship);
    None
}
