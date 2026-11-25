//! The patrol guide (type 3, `FPatrolGuide`, `Content/PatrolGuides.cpp`):
//! the ship flies its encounter's zone path point by point between its two
//! targets (`FPatrolRoute`), docks at the far target, waits, launches and
//! flies back to a random point of the path near the other target; on the
//! way its cargo scanner (`FCargoScanRoute`) picks a ship near it, trails
//! it, scans its cargo for the faction's `scan_for_cargo` goods and asks
//! for them, then tractors in the jettisoned cargo or turns hostile.
//!
//! The scanner's conversations are the comm system's events (types 0x11
//! to 0x18, `FGcsScanEvent` to `GGcsConversations.QueueConversation`,
//! [`super::super::gcs`]): the end of the line (`CommComplete` from the
//! speaker to the target) moves a scanner on from waiting for it.

// The C++'s `!(x < y)` tests send NaN to the failing side; the port spells
// them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use game_sim::ai::content::FleeAnswer;
use game_sim::ai::personality::Personality;
use neuron_core::math::Vector;

use super::super::directives::{self, DirectiveList};
use super::super::formations::collect_members;
use super::super::mission::obj_list::{DirectiveKind, Goto, TractorBeam, Trail};
use super::super::npc_ships::{self, set_goto_destination};
use super::super::patrol_paths::PatrolPath;
use super::super::ships::{self, ShipKey};
use super::super::space_paths;
use super::{GuideDesc, LANE_ENTRY_RANGE_SQUARED, PatrolPathRef, Targets, ship_position, target};
use crate::api;
use crate::api::space_obj::EqInfo;
use crate::events::{ControllerMessage, NpcShipKey};
use crate::server::Cx;

/// `GPatrolScanMinAttitude` (`Guides.cpp:35`).
const SCAN_MIN_ATTITUDE: f32 = -0.6;

/// `GPatrolScanMaxAttitude` (`Guides.cpp:36`).
const SCAN_MAX_ATTITUDE: f32 = 0.6;

/// The patrol path a route flies, while its system record has it.
fn path<'a>(cx: &'a Cx<'_>, path: Option<PatrolPathRef>) -> Option<&'a PatrolPath> {
    let r = path?;
    cx.logic
        .content
        .state
        .systems
        .get(&r.system)?
        .paths
        .get(r.index)
}

/// `FPatrolRoute` (0x1c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct PatrolRoute {
    /// `Path` (`+0x08`): the description's `Plan.Path`.
    pub path: Option<PatrolPathRef>,
    /// `Direction` (`+0x0c`): +1 or -1 along the path.
    pub direction: i32,
    /// `NextPoint` (`+0x10`).
    pub next_point: i32,
    /// `EndPoint` (`+0x14`).
    pub end_point: i32,
    /// `State` (`+0x18`): 1 new leg, 2 flying, 3 back from a flee, 4 at the
    /// end.
    pub state: i32,
}

impl PatrolRoute {
    /// `GetStatus` (`PatrolGuides.cpp:103-116`): 1 for states 1 and 2, 3 at
    /// the end, else 2.
    #[must_use]
    pub fn status(&self) -> i32 {
        match self.state {
            1 | 2 => 1,
            4 => 3,
            _ => 2,
        }
    }

    /// `Path->Points[NextPoint]`; the C++ indexes without a test, a point
    /// off the path is none here (D862).
    fn next(&self, cx: &Cx<'_>) -> Option<Vector> {
        let index = usize::try_from(self.next_point).ok()?;
        path(cx, self.path)?.points.get(index).copied()
    }

    /// `AdvancePoint` (`:245-275`): one step on, turning at an open path's
    /// ends and wrapping on a loop.
    fn advance_point(&mut self, cx: &Cx<'_>) {
        if self.next_point == self.end_point {
            return;
        }
        let Some(p) = path(cx, self.path) else {
            return;
        };
        let count = p.points.len() as i32;
        let looped = p.looped;
        if self.direction == -1 && self.next_point == 0 {
            if looped {
                self.next_point = count - 2;
                return;
            }
            self.direction = 1;
            self.next_point = 1;
            return;
        }
        if self.direction == 1 && self.next_point == count - 1 {
            if looped {
                self.next_point = 1;
                return;
            }
            self.next_point -= 1;
            self.direction = -1;
            return;
        }
        self.next_point += self.direction;
    }

    /// `GetPointsToGo` (`:279-294`): the points left to the end point (0
    /// unless the route is flying).
    #[must_use]
    pub fn points_to_go(&self, cx: &Cx<'_>) -> i32 {
        if self.state != 2 && self.state != 1 {
            return 0;
        }
        let count = path(cx, self.path).map_or(0, |p| p.points.len() as i32);
        if self.direction > 0 {
            if self.next_point <= self.end_point {
                return self.end_point - self.next_point;
            }
            return (self.end_point - self.next_point) + count + 1;
        }
        if self.next_point >= self.end_point {
            return self.next_point - self.end_point;
        }
        (self.next_point - self.end_point) + count + 1
    }

    /// A point goto to the next point (`SetDestination(&Point, 500, -1, 0,
    /// false, false)`, `Field_58` 1, priority 1) appended to `out`.
    fn append_goto(&self, cx: &mut Cx<'_>, out: &mut DirectiveList) {
        let Some(point) = self.next(cx) else {
            return;
        };
        let mut goto = Goto::default();
        set_goto_destination(&mut goto, point, 500.0, -1.0, 0, false, false);
        goto.op.field_58 = 1;
        goto.priority = 1;
        out.push(
            cx.logic
                .content
                .state
                .directives
                .create(DirectiveKind::GotoVec(goto)),
        );
    }

    /// `FPatrolRoute::Update(Out)` (`:128-175`): a goto to the next point
    /// (state 1, then 2); flying, the active goto's destination within 1
    /// per axis of the next point moves on (the end point ends the route),
    /// and the goto to the next point follows unless it ended; state 3 is
    /// state 2 again.
    fn update(
        &mut self,
        cx: &mut Cx<'_>,
        ship: Option<NpcShipKey>,
        out: &mut DirectiveList,
    ) -> i32 {
        match self.state {
            1 => {
                self.append_goto(cx, out);
                self.state = 2;
            }
            2 => {
                let destination = ship.and_then(|s| super::trade::active_goto_destination(cx, s));
                if let (Some(d), Some(p)) = (destination, self.next(cx)) {
                    let dx = f64::from(d.x) - f64::from(p.x);
                    let dy = f64::from(d.y) - f64::from(p.y);
                    let dz = f64::from(d.z) - f64::from(p.z);
                    if dx * dx <= 1.0 && dy * dy <= 1.0 && dz * dz <= 1.0 {
                        if self.next_point == self.end_point {
                            self.state = 4;
                        } else {
                            self.advance_point(cx);
                        }
                    }
                }
                if self.state != 4 {
                    self.append_goto(cx, out);
                }
            }
            3 => {
                self.state = 2;
                self.update(cx, ship, out);
            }
            _ => {}
        }
        self.status()
    }

    /// `FPatrolRoute::RequestFleeDestination` (`:184-232`): flying (states
    /// 1 to 3) the route goes to state 3 and answers a point (type 3) toward
    /// the next point: the point itself within 10000, else 10000 from the
    /// ship toward it. The radius is 500 either way. The C++ then makes a
    /// goto to the answer and releases it unused.
    fn flee(&mut self, cx: &Cx<'_>, ship: Option<NpcShipKey>, answer: &mut FleeAnswer) -> bool {
        answer.radius = 500.0;
        if !matches!(self.state, 1..=3) {
            return false;
        }
        self.state = 3;
        answer.kind = 3;
        let point = self.next(cx).unwrap_or(Vector::ZERO);
        let position = ship.map_or(Vector::ZERO, |s| ship_position(cx, s));
        let delta = Vector::new(
            (f64::from(point.x) - f64::from(position.x)) as f32,
            (f64::from(point.y) - f64::from(position.y)) as f32,
            (f64::from(point.z) - f64::from(position.z)) as f32,
        );
        let length_squared = (f64::from(delta.y) * f64::from(delta.y)
            + f64::from(delta.z) * f64::from(delta.z))
            + f64::from(delta.x) * f64::from(delta.x);
        if !(length_squared > f64::from(LANE_ENTRY_RANGE_SQUARED)) {
            answer.position = point;
            return true;
        }
        let inverse = 1.0 / length_squared.sqrt();
        let nx = (f64::from(delta.x) * inverse) as f32;
        let ny = (f64::from(delta.y) * inverse) as f32;
        let nz = (f64::from(delta.z) * inverse) as f32;
        let sz = (f64::from(nz) * 10000.0) as f32;
        let sy = (f64::from(ny) * 10000.0) as f32;
        let sx = (10000.0 * f64::from(nx)) as f32;
        answer.position = Vector::new(
            (f64::from(sx) + f64::from(position.x)) as f32,
            (f64::from(sy) + f64::from(position.y)) as f32,
            (f64::from(sz) + f64::from(position.z)) as f32,
        );
        true
    }
}

/// `FCargoScanRoute` (0x64 bytes), its states (`State`, `+0x14`): 0 off,
/// 1 idle, 2 closing in, 3 in range, 4 closing in after the "stand by"
/// line, 5 waiting for that line, 6 scanning, 7 waiting for the demand
/// line, 8 waiting for the player's answer, 0xa jettisoned, 0xb
/// tractoring, 0xc collecting, 0xd hostile.
#[derive(Clone, Debug, PartialEq)]
pub struct CargoScanRoute {
    /// `Directives` (`+0x08`): the directives `Update` hands out, one
    /// reference held on each.
    pub directives: DirectiveList,
    /// `State` (`+0x14`).
    pub state: i32,
    /// `ScanRange` (`+0x18`, -1): the cargo scan range, 500 once scanning.
    pub scan_range: i32,
    /// `CargoCount` (`+0x1c`, -1).
    pub cargo_count: i32,
    /// `TargetId` (`+0x20`): the scanned ship.
    pub target_id: u32,
    /// `CommSpeakerId` (`+0x24`): the speaker of the last queued event.
    pub comm_speaker: u32,
    /// `CargoGoodId` (`+0x28`).
    pub cargo_good: u32,
    /// `ScannedIds` (`+0x30`): not copied by `Clone`.
    pub scanned_ids: Vec<u32>,
    /// `LootIds` (`+0x40`): the jettisoned cargo; not copied by `Clone`.
    pub loot_ids: Vec<u32>,
    /// `AnswerTimer` (`+0x50`).
    pub answer_timer: f32,
    /// `+0x54`, 0; no reader.
    pub field_0x54: u32,
    /// `ScanChance` (`+0x58`): the pilot's or the faction's (0 until set,
    /// where the C++ leaves it unset).
    pub scan_chance: f32,
    /// `LootTimer` (`+0x5c`, -1 off).
    pub loot_timer: f32,
    /// `bScanEnabled` (`+0x60`, true).
    pub scan_enabled: bool,
    /// `bScanAnnounce` (`+0x61`, true).
    pub scan_announce: bool,
}

impl Default for CargoScanRoute {
    /// The members of `FCargoScanRoute(Ship)` before its `SetShip`.
    fn default() -> Self {
        Self {
            directives: DirectiveList::new(),
            state: 0,
            scan_range: -1,
            cargo_count: -1,
            target_id: 0,
            comm_speaker: 0,
            cargo_good: 0,
            scanned_ids: Vec::new(),
            loot_ids: Vec::new(),
            answer_timer: 0.0,
            field_0x54: 0,
            scan_chance: 0.0,
            loot_timer: -1.0,
            scan_enabled: true,
            scan_announce: true,
        }
    }
}

impl CargoScanRoute {
    /// `GetStatus` (`:350-357`): 2 off, 0 idle, else 1 (busy).
    #[must_use]
    pub fn status(&self) -> i32 {
        match self.state {
            0 => 2,
            1 => 0,
            _ => 1,
        }
    }

    /// The copy `Clone` makes (`:310-326`): the directives (each
    /// `AddRef`'ed) and the members, but not the two id lists; the loot
    /// timer, which the C++ leaves unset, off.
    pub(super) fn copy(&self, cx: &mut Cx<'_>) -> Self {
        for &d in &self.directives {
            cx.logic.content.state.directives.add_ref(d);
        }
        Self {
            scanned_ids: Vec::new(),
            loot_ids: Vec::new(),
            loot_timer: -1.0,
            ..self.clone()
        }
    }

    /// `~FCargoScanRoute` (`:329-338`): the scanned ship unmarked, then the
    /// directives released.
    pub(super) fn destroy(&mut self, cx: &mut Cx<'_>, ship: Option<NpcShipKey>) {
        self.unmark_target(cx, ship);
        directives::clear_list(&mut cx.logic.content.state.directives, &mut self.directives);
    }

    /// The scanned ship unmarked (`bScanTarget` 0, while the record knows
    /// it) and the target forgotten.
    fn unmark_target(&mut self, cx: &mut Cx<'_>, ship: Option<NpcShipKey>) {
        if self.target_id == 0 {
            return;
        }
        if let Some(key) = find_system_ship(cx, ship, self.target_id)
            && let Some(b) = ships::base_mut(&mut cx.logic.content.state, key)
        {
            b.scan_target = false;
        }
        self.target_id = 0;
    }

    /// `Reset` (slot 8, `:367-379`): the target unmarked, the loot and the
    /// directives dropped, the loot timer off, and idle unless off.
    fn reset(&mut self, cx: &mut Cx<'_>, ship: Option<NpcShipKey>) {
        self.unmark_target(cx, ship);
        self.loot_ids.clear();
        directives::clear_list(&mut cx.logic.content.state.directives, &mut self.directives);
        self.loot_timer = -1.0;
        if self.state != 0 {
            self.state = 1;
        }
    }

    /// `SetShip(Ship)` (slot 5, `:388-424`): a null ship resets and switches
    /// the scanner off. Scanning needs the ship's faction to have
    /// `scan_for_cargo` goods and a working scanner whose cargo range is
    /// above 500 (then 500); `scan_announce` comes from the faction, the
    /// scan chance from the pilot's job unless it is the default, else from
    /// the faction. A scanner that cannot scan resets and goes off.
    pub(super) fn set_ship(
        &mut self,
        cx: &mut Cx<'_>,
        old: Option<NpcShipKey>,
        ship: Option<NpcShipKey>,
    ) {
        if ship.is_none() {
            self.reset(cx, old);
        }
        let Some(key) = ship else {
            self.state = 0;
            return;
        };
        let affiliation = ships::affiliation(cx, ShipKey::Npc(key));
        let tables = cx.logic.content.tables.clone();
        // The C++ reads the faction's properties without a test (D862).
        let props = tables.faction_props(affiliation);
        if props.is_none_or(|p| p.scan_for_cargo.is_empty()) {
            if self.state != 0 {
                self.reset(cx, ship);
            }
            self.state = 0;
            return;
        }
        let props = props.expect("tested above");
        let id = npc_ships::space_obj(cx, key);
        let range = api::space_obj::scanner_range(cx, id);
        if let Ok((_, scan)) = range {
            self.scan_range = scan;
        }
        if !matches!(range, Ok((_, scan)) if scan > 500) {
            if self.state != 0 {
                self.reset(cx, ship);
            }
            self.state = 0;
            return;
        }
        self.scan_range = 500;
        self.scan_announce = props.scan_announce;
        let chance = npc_ships::get(cx, key).map_or(Personality::SCAN_CHANCE_DEFAULT, |s| {
            s.personality.job.scan_chance
        });
        self.scan_chance = if chance == Personality::SCAN_CHANCE_DEFAULT {
            props.scan_chance
        } else {
            chance
        };
        if self.state == 0 {
            self.state = 1;
        }
    }

    /// `Update(Out)` (`:465-481`): the scanner's directives handed over
    /// (each with a reference of the list's), its own list emptied.
    fn update(&mut self, cx: &mut Cx<'_>, out: &mut DirectiveList) -> i32 {
        directives::clear_list(&mut cx.logic.content.state.directives, out);
        *out = std::mem::take(&mut self.directives);
        self.status()
    }

    /// The comm system's scan event of `kind` from the ship to the target
    /// (`QueueScanEvent`, `:556-569`): in the ship's system, the ship is the
    /// last speaker, then the event goes to the conversation manager
    /// ([`super::super::gcs::queue_conversation`]; its result is dropped).
    fn queue_scan_event(
        &mut self,
        cx: &mut Cx<'_>,
        ship: Option<NpcShipKey>,
        kind: i32,
        loot_taker: u32,
        good: u32,
        response: u32,
    ) {
        let speaker = ship.map_or(0, |s| npc_ships::space_obj(cx, s));
        // `Ship->GetSystem()`: the ship's record (none without a ship).
        let system = ship
            .and_then(|s| cx.logic.content.state.npc_ships.get(&s))
            .and_then(|s| s.base.system);
        self.comm_speaker = speaker;
        let event = super::super::gcs::Event::Scan(super::super::gcs::ScanEvent {
            system,
            kind,
            speaker,
            listener: self.target_id,
            loot_taker,
            good,
            response,
        });
        super::super::gcs::queue_conversation(cx, &event);
    }

    /// `FindScanTarget` (`:488-537`): off while the missions clamp scans.
    /// The ships within 2500 of the scanner (`ScanObjects`, at most 16, mask
    /// `0x7f0000`) not yet scanned, not the scanner and known to the record:
    /// each one's formation (the ship alone outside one) is judged by its
    /// first member, skipped when that one is a scan target already or in a
    /// lane, else every member counts as scanned. The first member is taken
    /// when its reputation is known, the scanner's attitude toward it lies
    /// in `[-0.6, max]` (for a player 0.2 when the scanner's faction is
    /// unlawful, else 1; 0.6 for an NPC) and `rand() / 32768` is below the
    /// scan chance: it is marked and becomes the target.
    fn find_scan_target(&mut self, cx: &mut Cx<'_>, ship: NpcShipKey) -> bool {
        if cx.logic.content.state.clamps.scan_clamp != 0 {
            return false;
        }
        let system = npc_ships::system_id(cx, ship);
        let own = npc_ships::space_obj(cx, ship);
        let position = ship_position(cx, ship);
        let objects = api::system::scan_objects(cx, system, 16, position, 2500.0, 0x7f_0000)
            .unwrap_or_default();
        for object in objects {
            if self.scanned_ids.contains(&object) || object == own {
                continue;
            }
            let Some(found) = ships::find_in_system(cx, system, object) else {
                continue;
            };
            let state = &cx.logic.content.state;
            let mut members = collect_members(state, ships::formation_id(state, found));
            if members.is_empty() {
                members.push(found);
            }
            let first = members[0];
            let busy = ships::base(state, first).is_none_or(|b| b.scan_target || b.lane_entry != 0);
            if busy {
                continue;
            }
            let first_id = ships::id(state, first);
            for &m in &members {
                let id = ships::id(state, m);
                if !self.scanned_ids.contains(&id) {
                    self.scanned_ids.push(id);
                }
            }
            let Ok(rep) = api::space_obj::rep(cx, first_id) else {
                continue;
            };
            let own_rep = ships::reputation(&cx.logic.content.state, ShipKey::Npc(ship));
            // The C++ reads its unset local when `GetAttitude` fails; 0 here.
            let attitude = api::reputation::attitude(cx, own_rep, rep).unwrap_or(0.0);
            let mut max = SCAN_MAX_ATTITUDE;
            if super::super::players::find_by_ship(&cx.logic.content.state.players, first_id)
                .is_some()
            {
                let affiliation = ships::affiliation(cx, ShipKey::Npc(ship));
                let unlawful = cx
                    .logic
                    .content
                    .tables
                    .faction_props(affiliation)
                    .is_some_and(|p| {
                        p.legality == super::super::tables::factions::legality::UNLAWFUL
                    });
                max = if unlawful { 0.2 } else { 1.0 };
            }
            if !(attitude >= SCAN_MIN_ATTITUDE) || !(attitude <= max) {
                continue;
            }
            let roll = f64::from(cx.world.rng().rand()) * (1.0 / 32768.0);
            if !(roll < f64::from(self.scan_chance)) {
                continue;
            }
            if let Some(b) = ships::base_mut(&mut cx.logic.content.state, first) {
                b.scan_target = true;
            }
            self.target_id = first_id;
            break;
        }
        self.target_id != 0
    }

    /// The scanned ship gone or in a lane.
    fn target_gone(&self, cx: &Cx<'_>, ship: Option<NpcShipKey>) -> bool {
        find_system_ship(cx, ship, self.target_id)
            .and_then(|k| ships::base(&cx.logic.content.state, k))
            .is_none_or(|b| b.lane_entry != 0)
    }

    /// `HandleEvent(MsgId, Parms)` (slot 3, `:556-705`); every path answers
    /// the status.
    pub(super) fn handle_event(
        &mut self,
        cx: &mut Cx<'_>,
        ship: Option<NpcShipKey>,
        message: &ControllerMessage<'_>,
    ) -> i32 {
        let own = ship.map_or(0, |s| npc_ships::space_obj(cx, s));
        match message {
            ControllerMessage::CommComplete {
                listener, speaker, ..
            } => {
                if (self.state == 5 || self.state == 7)
                    && *speaker == self.comm_speaker
                    && *listener == self.target_id
                {
                    if self.state == 5 {
                        self.state = 4;
                    } else {
                        let _ = api::ai::scan_response(cx, own, self.target_id, self.cargo_good);
                        self.state = 8;
                    }
                }
            }
            ControllerMessage::LootAcquired {
                loot, ship: taker, ..
            } => {
                if (self.state == 0xb || self.state == 0xc) && self.loot_ids.contains(loot) {
                    if *taker == own {
                        if self.state == 0xb {
                            let ratio = hold_ratio(cx, ship);
                            if f64::from(ratio) > f64::from(0.7_f32) {
                                self.state = 0xc;
                            }
                        }
                    } else if !is_same_ship_or_formation(cx, ship, *taker) {
                        self.queue_scan_event(cx, ship, 0x18, *taker, 0, 1);
                        set_formation_attitudes(cx, ship, own, *taker);
                    }
                }
            }
            ControllerMessage::Heartbeat { elapsed } => self.handle_timer(cx, ship, *elapsed),
            ControllerMessage::CargoJettisoned {
                ship: jettisoned,
                good,
                count,
                ..
            } => {
                if self.state != 1 && self.state != 0 {
                    if self.target_gone(cx, ship) {
                        self.reset(cx, ship);
                    } else if self.target_id != 0
                        && *jettisoned == self.target_id
                        && super::super::players::find_by_ship(
                            &cx.logic.content.state.players,
                            self.target_id,
                        )
                        .is_some()
                        && self.cargo_good == *good
                        && *count == self.cargo_count
                    {
                        self.state = 0xa;
                        // The C++ calls its own slot 3 with the heartbeat's id
                        // and null parameters: the timer step reads no time.
                        self.handle_timer(cx, ship, 0.0);
                    }
                }
            }
            ControllerMessage::LootSpawned {
                loot, source, good, ..
            } => {
                if (self.state == 0xb || self.state == 0xc)
                    && *source == self.target_id
                    && *good == self.cargo_good
                {
                    if !self.loot_ids.contains(loot) {
                        self.loot_ids.push(*loot);
                    }
                    if self.state == 0xb {
                        let fill = hold_fill(cx, ship);
                        if fill > f64::from(0.7_f32) {
                            self.state = 0xc;
                            if self.loot_timer < 0.0 {
                                self.loot_timer = 3.1;
                            }
                        } else if self.loot_ids.len() == 1 && self.loot_timer < 0.0 {
                            self.loot_timer = 3.1;
                        }
                    }
                }
            }
            ControllerMessage::ObjectDestroyed(event) => {
                if self.state != 1 && self.state != 0 {
                    self.object_destroyed(cx, ship, own, event.id, event.killer, event.kind);
                }
            }
            ControllerMessage::SystemExit { player, .. } => {
                if self.state != 1 && self.state != 0 {
                    let ship_id = cx
                        .logic
                        .content
                        .state
                        .players
                        .get(player)
                        .and_then(|p| p.ship.as_ref())
                        .map(|s| s.ship);
                    if ship_id.is_some_and(|id| id == self.target_id) {
                        self.reset(cx, ship);
                    }
                }
            }
            ControllerMessage::CombatState { state, param1, .. }
                if *param1 == own && *state == 0 && (2..=7).contains(&self.state) =>
            {
                self.reset(cx, ship);
            }
            _ => {}
        }
        self.status()
    }

    /// `ObjectDestroyed {id, killer, .., kind}` (`:657-685`): the target
    /// gone resets; a recorded loot gone is dropped (the last one resets;
    /// otherwise the loot timer restarts while tractoring or collecting),
    /// and a taker outside the scanner's formation gets event 0x18 (kinds 0
    /// to 2) and hostility.
    fn object_destroyed(
        &mut self,
        cx: &mut Cx<'_>,
        ship: Option<NpcShipKey>,
        own: u32,
        id: u32,
        taker: u32,
        kind: i32,
    ) {
        if id == self.target_id {
            self.reset(cx, ship);
            return;
        }
        let Some(at) = self.loot_ids.iter().position(|&l| l == id) else {
            return;
        };
        self.loot_ids.remove(at);
        if self.loot_ids.is_empty() {
            self.reset(cx, ship);
        } else if (self.state == 0xb || self.state == 0xc) && self.loot_timer < 0.0 {
            self.loot_timer = 3.1;
        }
        if taker == 0 || is_same_ship_or_formation(cx, ship, taker) {
            return;
        }
        if (0..=2).contains(&kind) {
            self.queue_scan_event(cx, ship, 0x18, taker, 0, 1);
        }
        set_formation_attitudes(cx, ship, own, taker);
    }

    /// The heartbeat (`HandleTimer`, `:720-891`): the states run on while
    /// one moves on (`bStop` cleared), and stop at the first that does not.
    fn handle_timer(&mut self, cx: &mut Cx<'_>, ship: Option<NpcShipKey>, dt: f32) {
        let Some(key) = ship else {
            return;
        };
        let own = npc_ships::space_obj(cx, key);
        let mut stop = true;
        loop {
            match self.state {
                1 => {
                    if !self.scan_enabled {
                        // Falls to the loop's test with `bStop` as it is.
                    } else {
                        if self.scanned_ids.is_empty() {
                            let state = &cx.logic.content.state;
                            let members = collect_members(
                                state,
                                ships::formation_id(state, ShipKey::Npc(key)),
                            );
                            for m in members {
                                let id = ships::id(state, m);
                                if !self.scanned_ids.contains(&id) {
                                    self.scanned_ids.push(id);
                                }
                            }
                        }
                        if !self.find_scan_target(cx, key) {
                            return;
                        }
                        self.state = 2;
                        stop = false;
                        let mut trail = Trail {
                            priority: 1,
                            ..Trail::default()
                        };
                        trail.op.target = self.target_id;
                        trail.op.distance = self.scan_range as f32;
                        let d = cx
                            .logic
                            .content
                            .state
                            .directives
                            .create(DirectiveKind::Trail(trail));
                        self.directives.push(d);
                        continue;
                    }
                }
                2 | 4 => {
                    let target = api::space_obj::location(cx, self.target_id)
                        .map_or(Vector::ZERO, |(p, _)| p);
                    let position = ship_position(cx, key);
                    let dx = f64::from(position.x) - f64::from(target.x);
                    let dy = f64::from(position.y) - f64::from(target.y);
                    let dz = f64::from(position.z) - f64::from(target.z);
                    let distance = ((dx * dx + dz * dz) + dy * dy).sqrt();
                    if !(f64::from(self.scan_range) > distance) {
                        return;
                    }
                    self.state = if self.state == 2 { 3 } else { 6 };
                    stop = false;
                    continue;
                }
                3 => {
                    if self.scan_announce {
                        self.queue_scan_event(cx, ship, 0x11, 0, 0, 1);
                        self.state = 5;
                        return;
                    }
                    self.state = 4;
                    stop = false;
                    continue;
                }
                6 => {
                    if self.target_gone(cx, ship) {
                        self.reset(cx, ship);
                        return;
                    }
                    if !self.scan(cx, ship, own, &mut stop) {
                        return;
                    }
                }
                8 => {
                    let sum = f64::from(self.answer_timer) + f64::from(dt);
                    self.answer_timer = sum as f32;
                    let whole = neuron_core::num::trunc_to_i32_via_i64(sum);
                    if f64::from(whole) < 20.0 {
                        return;
                    }
                    self.state = 0xd;
                    stop = false;
                    continue;
                }
                0xa => {
                    if self.target_gone(cx, ship) {
                        self.reset(cx, ship);
                        return;
                    }
                    self.queue_scan_event(cx, ship, 0x14, 0, 0, 1);
                    self.state = 0xb;
                    return;
                }
                0xb | 0xc => {
                    if self.loot_timer > 0.0 {
                        let left = f64::from(self.loot_timer) - f64::from(dt);
                        self.loot_timer = left as f32;
                        if left <= 0.0 {
                            self.loot_timer = -1.0;
                            if let Some(&loot) = self.loot_ids.first() {
                                if self.state == 0xb {
                                    let mut tractor = TractorBeam::default();
                                    tractor.op.target = loot;
                                    tractor.op.field_10 = 0;
                                    tractor.priority = 0;
                                    let d = cx
                                        .logic
                                        .content
                                        .state
                                        .directives
                                        .create(DirectiveKind::TractorBeam(tractor));
                                    self.directives.push(d);
                                } else {
                                    let mut goto = Goto::default();
                                    goto.op.kind = 0;
                                    goto.op.field_1c = loot as i32;
                                    goto.op.field_58 = 0;
                                    goto.op.range = 50.0;
                                    goto.op.field_54 = -1.0;
                                    goto.priority = 0;
                                    let d = cx
                                        .logic
                                        .content
                                        .state
                                        .directives
                                        .create(DirectiveKind::GotoShip(goto));
                                    self.directives.push(d);
                                    let _ = api::ai::submit_forced_target(cx, own, loot);
                                }
                            }
                        }
                    }
                }
                0xd => {
                    if self.target_gone(cx, ship) {
                        self.reset(cx, ship);
                        return;
                    }
                    self.queue_scan_event(cx, ship, 0x16, 0, 0, 1);
                    let target = self.target_id;
                    set_formation_attitudes(cx, ship, own, target);
                    self.reset(cx, ship);
                    return;
                }
                // 0, 5, 7, 9 and above 0xd.
                _ => {}
            }
            if stop {
                return;
            }
        }
    }

    /// State 6 (`:775-858`): the target's cargo scanned (`ActivateEquipment`
    /// type 1); the wanted good of the highest level found in it (on a tie
    /// the one taking more cargo space). None: the "nothing found" line
    /// (when announced) and a reset. An NPC answers through its AI (mission
    /// cargo answers 3 without a line or a draw, which changes nothing and
    /// runs the state again: the C++ spins there, the port stops, D862): 0
    /// jettisons the cargo (state 0xa), 1 and 2 refuse (state 0xd). A player's
    /// mission cargo turns the scanner hostile; any other cargo is asked for
    /// (state 7). False to leave the heartbeat.
    fn scan(
        &mut self,
        cx: &mut Cx<'_>,
        ship: Option<NpcShipKey>,
        own: u32,
        stop: &mut bool,
    ) -> bool {
        let info = EqInfo {
            kind: 1,
            on: true,
            count: 0,
            target: self.target_id,
        };
        let cargo = api::space_obj::activate_equipment(cx, own, &info).unwrap_or_default();
        let affiliation = ship.map_or(u32::MAX, |s| ships::affiliation(cx, ShipKey::Npc(s)));
        let tables = cx.logic.content.tables.clone();
        let wanted = tables
            .faction_props(affiliation)
            .map(|p| p.scan_for_cargo.clone())
            .unwrap_or_default();
        let data = cx.world.data();
        let mut best: Option<usize> = None;
        let mut best_level = 0;
        let mut best_space = -1.0_f32;
        for w in &wanted {
            let Some(at) = cargo.iter().position(|c| c.arch_id == w.archetype) else {
                continue;
            };
            let c = &cargo[at];
            let volume = data
                .archetypes
                .equipment
                .get(&c.arch_id)
                .map_or(0.0, |a| a.volume);
            let space = if c.hardpoint.is_none() && c.arch_id != 0 {
                (f64::from(c.count) * f64::from(volume)) as f32
            } else {
                0.0
            };
            if w.level > best_level || (w.level == best_level && space > best_space) {
                best_space = space;
                best_level = w.level;
                best = Some(at);
            }
        }
        let Some(at) = best else {
            if self.scan_announce {
                self.queue_scan_event(cx, ship, 0x12, 0, 0, 1);
            }
            self.reset(cx, ship);
            *stop = true;
            return true;
        };
        let found = cargo[at].clone();
        if let Ok(good) = api::misc::translate_arch_to_good(cx, found.arch_id) {
            self.cargo_good = good;
        }
        self.cargo_count = found.count;
        if super::super::players::find_by_ship(&cx.logic.content.state.players, self.target_id)
            .is_none()
        {
            if found.mission {
                // Response 3 changes nothing and the C++ runs state 6 again
                // at once, without end (D862).
                return false;
            }
            self.queue_scan_event(cx, ship, 0x13, 0, self.cargo_good, 1);
            let response = api::ai::scan_response(cx, own, self.target_id, self.cargo_good);
            *stop = false;
            match response {
                0 => {
                    let _ = api::space_obj::jettison_equipment(
                        cx,
                        self.target_id,
                        found.id,
                        self.cargo_count,
                    );
                    self.state = 0xa;
                }
                1 | 2 => {
                    self.queue_scan_event(cx, ship, 0x15, 0, 0, response as u32);
                    self.state = 0xd;
                }
                // Any other answer runs state 6 again in the C++, without
                // end; the port stops (D862).
                _ => return false,
            }
        } else if found.mission {
            self.state = 0xd;
        } else {
            self.answer_timer = 0.0;
            self.queue_scan_event(cx, ship, 0x13, 0, self.cargo_good, 1);
            self.state = 7;
            *stop = true;
        }
        true
    }
}

/// The occupied part of the hold as `LootAcquired` computes it: the
/// integer quotient (`DIV`) of the occupied space by the hold; a hold of 0,
/// a division by zero in the C++, is 0 (D862).
fn hold_ratio(cx: &Cx<'_>, ship: Option<NpcShipKey>) -> u32 {
    let (occupied, hold) = hold(cx, ship);
    occupied.checked_div(hold).unwrap_or(0)
}

/// `LootSpawned`'s fill: the occupied space stored as a float over the
/// hold, both as doubles; a hold of 0 is a fill of 0.
fn hold_fill(cx: &Cx<'_>, ship: Option<NpcShipKey>) -> f64 {
    let (occupied, hold) = hold(cx, ship);
    if hold == 0 {
        return 0.0;
    }
    f64::from(f64::from(occupied) as f32) / f64::from(hold)
}

/// `GetCargoSpaceOccupied` and `GetCargoHoldSize` of the ship (0 when they
/// fail).
fn hold(cx: &Cx<'_>, ship: Option<NpcShipKey>) -> (u32, u32) {
    let Some(key) = ship else {
        return (0, 0);
    };
    let id = npc_ships::space_obj(cx, key);
    let arch = npc_ships::get(cx, key).map_or(0, |s| s.base.archetype);
    let occupied = api::space_obj::cargo_space_occupied(cx, id).unwrap_or(0);
    let hold = api::misc::cargo_hold_size(cx, arch).unwrap_or(0);
    (occupied, hold)
}

/// `Ship->GetSystem()->FindShip(Id)`; none without a system record.
fn find_system_ship(cx: &Cx<'_>, ship: Option<NpcShipKey>, id: u32) -> Option<ShipKey> {
    let system = npc_ships::system_id(cx, ship?);
    ships::find_in_system(cx, system, id)
}

/// `IsSameShipOrFormation(Ship, OtherId)` (`SpacePaths.cpp:903-912`): the
/// other ship of the ship's system flies in the ship's formation, or is
/// the ship; false when the record does not know it.
fn is_same_ship_or_formation(cx: &Cx<'_>, ship: Option<NpcShipKey>, other: u32) -> bool {
    let Some(key) = ship else {
        return false;
    };
    let Some(found) = find_system_ship(cx, ship, other) else {
        return false;
    };
    let state = &cx.logic.content.state;
    let formation = ships::formation_id(state, found);
    if formation != u32::MAX && formation == ships::formation_id(state, ShipKey::Npc(key)) {
        return true;
    }
    ships::id(state, ShipKey::Npc(key)) == ships::id(state, found)
}

/// `SetFormationAttitudes(System, ShipId, OtherId, true, true, -1)`
/// ([`space_paths::set_formation_attitudes`]) in the ship's system.
fn set_formation_attitudes(cx: &mut Cx<'_>, ship: Option<NpcShipKey>, own: u32, other: u32) {
    let system = ship.map_or(0, |s| npc_ships::system_id(cx, s));
    space_paths::set_formation_attitudes(cx, system, own, other, true, true, -1.0);
}

/// `FPatrolGuide` (type 3, 0x34 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct PatrolGuide {
    /// The target guide's two targets.
    pub targets: Targets,
    /// `bUndocked` (`+0x24`, true): false from the dock to the launch.
    pub undocked: bool,
    /// `bScanState` (`+0x25`, true): the ship's `scan_state_change`.
    pub scan_state: bool,
    /// `State` (`+0x28`): 0 to 5.
    pub state: i32,
    /// `Route` (`+0x2c`).
    pub route: PatrolRoute,
    /// `Scanner` (`+0x30`).
    pub scanner: CargoScanRoute,
}

impl PatrolGuide {
    /// `FPatrolGuide(Desc)` (`:900-915`): the route from the description's
    /// plan (its path, direction, target point and end point, state 1),
    /// the scanner with its `SetShip`; the ship joins the guided ships.
    pub(super) fn new(
        cx: &mut Cx<'_>,
        desc: &GuideDesc,
        targets: Targets,
        ship: Option<NpcShipKey>,
    ) -> Self {
        let mut scanner = CargoScanRoute::default();
        scanner.set_ship(cx, ship, ship);
        if let Some(s) = ship {
            super::super::space_support::register_guided_ship(cx, s);
        }
        Self {
            targets,
            undocked: true,
            scan_state: true,
            state: 0,
            route: PatrolRoute {
                path: desc.plan.path,
                direction: desc.plan.path_direction,
                next_point: desc.plan.path_target_point,
                end_point: desc.plan.path_point,
                state: 1,
            },
            scanner,
        }
    }
}

/// `FPatrolGuide::Start` (`:959-1030`): 0 goes to 1; 1 gives the route's
/// directives to the ship until the route ends (2); 2 docks at the second
/// target with scanning off; 3, docked, waits 20 to 30 s (else the targets
/// swap and the route goes on); 4 launches, swaps the targets, ends the
/// route at a random path point of the new second target, reverses an open
/// path and scans again as the ship last asked; 5 gives the scanner's
/// directives while it is busy, else back to the route.
pub(super) fn start(cx: &mut Cx<'_>, ship: Option<NpcShipKey>, g: &mut PatrolGuide) {
    loop {
        match g.state {
            0 => g.state = 1,
            1 => {
                let mut next = DirectiveList::new();
                if g.route.update(cx, ship, &mut next) == 3 {
                    directives::clear_list(&mut cx.logic.content.state.directives, &mut next);
                    g.state = 2;
                    continue;
                }
                if let Some(s) = ship {
                    npc_ships::set_directives(cx, ShipKey::Npc(s), &next, 0);
                }
                directives::clear_list(&mut cx.logic.content.state.directives, &mut next);
                return;
            }
            2 => {
                g.undocked = false;
                g.scanner.scan_enabled = false;
                target::issue_dock_directive(cx, ship, g.targets.id2);
                g.state = 3;
                return;
            }
            3 => {
                let id = ship.map_or(0, |s| npc_ships::space_obj(cx, s));
                if super::super::dock_queues::ship_docking_target(cx, id).is_some() {
                    target::issue_delay_directive(cx, ship);
                    g.state = 4;
                    return;
                }
                g.targets.swap();
                g.state = 1;
            }
            4 => {
                target::issue_launch_directive(cx, ship);
                g.targets.swap();
                let points = path(cx, g.route.path)
                    .map(|p| p.object_point_indices(g.targets.id2))
                    .unwrap_or_default();
                let count = points.len() as i32;
                let pick = cx.world.rng().rand().wrapping_mul(count) / 32768;
                // The C++ indexes the list without a test (D862).
                if let Some(&point) = usize::try_from(pick).ok().and_then(|i| points.get(i)) {
                    g.route.end_point = point;
                }
                g.route.state = 1;
                if !path(cx, g.route.path).is_some_and(|p| p.looped) {
                    g.route.direction = -g.route.direction;
                }
                g.state = 1;
                g.undocked = true;
                g.scanner.scan_enabled = g.scan_state;
                return;
            }
            5 => {
                let mut next = DirectiveList::new();
                if g.scanner.update(cx, &mut next) == 1 {
                    if !next.is_empty()
                        && let Some(s) = ship
                    {
                        npc_ships::set_directives(cx, ShipKey::Npc(s), &next, 0);
                    }
                    directives::clear_list(&mut cx.logic.content.state.directives, &mut next);
                    return;
                }
                directives::clear_list(&mut cx.logic.content.state.directives, &mut next);
                g.state = 1;
            }
            // The C++ spins above 5; no state reaches it.
            _ => return,
        }
    }
}

/// `FPatrolGuide::HandleEvent` (`:1062-1077`): every message goes to the
/// scanner; busy after it, the guide gives the scanner's directives (state
/// 5); a status that changed sends the guide back to its route.
pub(super) fn handle_event(
    cx: &mut Cx<'_>,
    ship: Option<NpcShipKey>,
    g: &mut PatrolGuide,
    message: &ControllerMessage<'_>,
) {
    let before = g.scanner.status();
    let after = g.scanner.handle_event(cx, ship, message);
    if after == 1 {
        g.state = 5;
        start(cx, ship, g);
        return;
    }
    if before != after {
        g.state = 1;
        start(cx, ship, g);
    }
}

/// `FPatrolGuide::SetShip` (`:1080-1089`): the guided ship replaced; the
/// route and the scanner take the new ship.
pub(super) fn set_ship(
    cx: &mut Cx<'_>,
    old: Option<NpcShipKey>,
    ship: Option<NpcShipKey>,
    g: &mut PatrolGuide,
) {
    if let Some(s) = old {
        super::super::space_support::unregister_guided_ship(cx, s);
    }
    g.scanner.set_ship(cx, old, ship);
    if let Some(s) = ship {
        super::super::space_support::register_guided_ship(cx, s);
    }
}

/// `FPatrolGuide::RequestFleeDestination` (`:1101-1115`): the route's
/// answer, else a dock at the second target; always an answer, radius 500
/// (the route writes it first). The C++'s unused locals make nothing.
pub(super) fn flee(
    cx: &Cx<'_>,
    ship: Option<NpcShipKey>,
    g: &mut PatrolGuide,
) -> Option<FleeAnswer> {
    let mut answer = FleeAnswer::default();
    if !g.route.flee(cx, ship, &mut answer) {
        answer.kind = 2;
        answer.param = g.targets.id2;
    }
    Some(answer)
}
