//! The comm system, GCS (TU25, `Content/Gcs.cpp`, `Gcs.h`; record 0008
//! point 7; work package P3.6b): the radio chatter of the NPCs, the
//! stations and the docks.
//!
//! - **The handler set** (`GGcsHandlers`, `:3641-3710`): SpaceSupport hands
//!   it 15 messages and `SystemLoaded` and `SystemUnloaded`
//!   ([`handle_message`]). `Hail` starts a player hail; every other message
//!   goes to each handler in the order `InitGcs` added them: the dock hail
//!   handler (0, the stations' 2500 m phantoms), the combat handler (1)
//!   and the fidget handler (2).
//! - **The admission** (`IsGcsCommAllowed`, `RecordGcsComm`,
//!   `:4342-4488`): whether a comm of a category may be spoken (the GCS
//!   clamp, the speaker and listener categories, the interval since the
//!   last comm of the category within 10 km, and its chance, one `rand()`)
//!   and where one was spoken ([`is_comm_allowed`], [`record_comm`]).
//! - **The conversation manager** (`GGcsConversations`, `:3411-3635`): an
//!   event becomes a conversation ([`conversations`]) of lines
//!   ([`lines`]); the first line plays at once (`SendComm`, through
//!   [`crate::replication::send_comm`]). Outside single player each line
//!   queues its own "line done" (`CommComplete` to SpaceSupport after its
//!   voice durations and 1.25 s, counted by the heartbeat, [`update`]); a
//!   `CommComplete` of a line plays the conversation's next one
//!   ([`on_message`]).
//!
//! The state is process-wide in the C++ (TU25's and TU53's statics); here
//! it is [`Gcs`] in Content's live state, so it goes with the DLL's unload.
//! What the C++ holds by pointer (a line's system record, an event's
//! objects) is held by id and looked up again.
//!
//! **Not here.** `FGcsPlayerListLine` and `FGcsConversationManager::Add`,
//! the random missions' conversations (TU61), are P3.7b's (D939). The patrol
//! guide's scanner queues its events through [`queue_conversation`] with
//! [`Event::Scan`].

// The x87 comparisons keep their NaN outcome as written (`!(a > b)`).
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod conversations;
pub mod lines;
pub mod names;

use std::collections::{BTreeMap, BTreeSet, VecDeque};

use neuron_core::math::Vector;

use super::ships::{self, ShipKey};
use super::{game_support, players};
use crate::api;
use crate::events::{ControllerMessage, NpcShipKey, PhantomKey};
use crate::server::Cx;
use conversations::Conversation;

/// The handler types of `GetGcsHandler` (`:3641-3669`).
pub const HANDLER_DOCK_HAIL: u8 = 0;
/// The combat handler (`FGcsHandler1`).
pub const HANDLER_COMBAT: u8 = 1;
/// The fidget handler (`FGcsHandler2`).
pub const HANDLER_FIDGET: u8 = 2;

/// `GGcsFidgetRangeSquared` (`:4181`): 2500².
const FIDGET_RANGE_SQUARED: f32 = 6_250_000.0;

/// `FGcsDelayedMessage` (0x18 bytes): a line's "line done", sent once its
/// time is up.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DelayedMessage {
    /// `+0x00`: the system (message word 1).
    pub system: u32,
    /// `+0x04`: the speaker (word 2).
    pub speaker: u32,
    /// `+0x08`: the listener (word 0).
    pub listener: u32,
    /// `+0x0c`: the line's last text (word 3).
    pub text: u32,
    /// `Elapsed` (`+0x10`).
    pub elapsed: f32,
    /// `Delay` (`+0x14`).
    pub delay: f32,
}

/// The words of a `CommComplete` as the lines read them
/// (`{listener, system, speaker, last text}`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LineDone {
    /// Word 0.
    pub listener: u32,
    /// Word 1.
    pub system: u32,
    /// Word 2.
    pub speaker: u32,
    /// Word 3.
    pub text: u32,
}

/// `FGcsRecentComm` (0x20 bytes): a comm the admission remembers.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RecentComm {
    /// `System` (`+0x00`), by id; never read.
    pub system: Option<u32>,
    /// `Time` (`+0x04`): the game time, as a float.
    pub time: f32,
    /// `Position` (`+0x08`): the speaker's (else the listener's) position,
    /// -0.1 each when it has none.
    pub position: Vector,
    /// `Speaker` (`+0x14`).
    pub speaker: u32,
    /// `Listener` (`+0x18`).
    pub listener: u32,
}

/// `FGcsDockHailHandler` (0x24 bytes): the stations' phantoms and the
/// recent hails.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DockHailHandler {
    /// `Phantoms` (`+0x04`): system to phantom (by its key's number) to the
    /// object it surrounds.
    pub phantoms: BTreeMap<u32, BTreeMap<u32, u32>>,
    /// `RecentHails` (`+0x18`): `{phantom (None: any), ship}`, oldest first.
    pub recent_hails: VecDeque<(Option<PhantomKey>, u32)>,
}

/// The comm system's state: TU25's admission tables, the handler set and
/// the conversation manager.
#[derive(Clone, Debug, PartialEq)]
pub struct Gcs {
    /// `bEnabled` (`+0x04`, true; nothing clears it).
    pub enabled: bool,
    /// `Conversations` (`+0x08`), in queue order.
    pub conversations: Vec<Conversation>,
    /// `DelayedMessages` (`+0x18`).
    pub delayed: Vec<DelayedMessage>,
    /// `GGcsHandlers.Handlers`: the handler types in the order they were
    /// added (each `InitGcs` adds 0, 1, 2 again).
    pub handlers: Vec<u8>,
    /// The dock hail handler (`GGcsHandlerMap[0]`, made on first use).
    pub dock_hail: DockHailHandler,
    /// `GGcsRecentComms`: per category, newest first, at most 10.
    pub recent: BTreeMap<i32, VecDeque<RecentComm>>,
    /// `GGcsCategoryIntervals`.
    pub intervals: BTreeMap<i32, f32>,
    /// `GGcsCategoryChances`.
    pub chances: BTreeMap<i32, f32>,
    /// `GGcsSpeakerCategories`: the categories a speaker that is no
    /// SpacePop ship may use.
    pub speaker_categories: BTreeSet<i32>,
    /// `GGcsListenerCategories`: the same for listeners.
    pub listener_categories: BTreeSet<i32>,
    /// `GGcsDefaultInterval` (12, then `InitGcs`'s 15).
    pub default_interval: f64,
}

impl Default for Gcs {
    fn default() -> Self {
        Self {
            enabled: true,
            conversations: Vec::new(),
            delayed: Vec::new(),
            handlers: Vec::new(),
            dock_hail: DockHailHandler::default(),
            recent: BTreeMap::new(),
            intervals: BTreeMap::new(),
            chances: BTreeMap::new(),
            speaker_categories: BTreeSet::new(),
            listener_categories: BTreeSet::new(),
            default_interval: 12.0,
        }
    }
}

// ---- Events --------------------------------------------------------------------------------

/// Type 2, `FGcsDockGrantedEvent` (`DockQueues.h:72-85`): a ship at a
/// dock target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DockEvent {
    /// `System`, `None` without the record.
    pub system: Option<u32>,
    /// `Speaker`: the dock target.
    pub speaker: u32,
    /// `Listener`: the ship.
    pub listener: u32,
    /// `DockIndex` (-1).
    pub dock_index: i32,
    /// `DockType`: the terminal's type, or by the ship's classes.
    pub dock_type: u32,
    /// `Response`.
    pub response: i32,
    /// `bWaitedInQueue`.
    pub waited_in_queue: bool,
}

/// Type 3, `FGcsLaunchEvent` (`DockQueues.h:89-97`): a ship leaves a
/// terminal.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LaunchEvent {
    /// `System`.
    pub system: Option<u32>,
    /// `Speaker`: the dock.
    pub speaker: u32,
    /// `Listener`: the ship.
    pub listener: u32,
    /// `DockIndex`.
    pub dock_index: i32,
    /// `TerminalType`: the terminal's `TerminalInfo::Value`.
    pub terminal_type: u32,
}

/// Types 6 to 0x10, `FGcsCombatEvent` and its four classes with a value.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CombatEvent {
    /// `System`.
    pub system: Option<u32>,
    /// `Type` (`+0x10`).
    pub kind: i32,
    /// `ObjectA` (`+0x08`).
    pub a: u32,
    /// `ObjectB` (`+0x0c`).
    pub b: u32,
    /// Type 6's `Health` (`+0x14`).
    pub health: f32,
    /// Types 0xa, 0xb and 0xe's `Reason` (`+0x14`).
    pub reason: i32,
}

/// Types 0x11 to 0x18, `FGcsScanEvent` (`Gcs.h:1091-1102`), built by the
/// patrol guide's scanner (`FCargoScanRoute::QueueScanEvent`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScanEvent {
    /// `System`.
    pub system: Option<u32>,
    /// `Type` (`+0x08`).
    pub kind: i32,
    /// `Speaker` (`+0x0c`): the patrol.
    pub speaker: u32,
    /// `Listener` (`+0x10`): the scanned ship.
    pub listener: u32,
    /// `LootTakerId` (`+0x14`).
    pub loot_taker: u32,
    /// `CargoGoodId` (`+0x18`).
    pub good: u32,
    /// `ScanResponse` (`+0x1c`).
    pub response: u32,
}

/// An event handed to the conversation manager (`FGcsEvent`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Event {
    /// Type 1, `FGcsDockHailEvent`: the station hails the ship.
    DockHail {
        /// `System`.
        system: Option<u32>,
        /// `Speaker`: the station.
        speaker: u32,
        /// `Listener`: the ship.
        listener: u32,
    },
    /// Type 2.
    Dock(DockEvent),
    /// Type 3.
    Launch(LaunchEvent),
    /// Type 5, `FGcsPlayerObjectEvent`: a player hails an NPC.
    PlayerHail {
        /// `System`.
        system: Option<u32>,
        /// `Player`, by id.
        player: u32,
        /// `Object`: the hailed NPC ship.
        object: NpcShipKey,
    },
    /// Types 6 to 0x10.
    Combat(CombatEvent),
    /// Types 0x11 to 0x18.
    Scan(ScanEvent),
    /// Type 0x19, `FGcsFidgetEvent`: an NPC near a player chatters.
    Fidget {
        /// `System`.
        system: Option<u32>,
        /// `Ship`: the chattering formation leader.
        ship: NpcShipKey,
        /// `Kind`: 0 simple, 1 formation status, 2 rumour.
        kind: i32,
    },
}

impl Event {
    /// `GetType` (slot 0).
    #[must_use]
    pub fn type_id(&self) -> i32 {
        match self {
            Self::DockHail { .. } => 1,
            Self::Dock(_) => 2,
            Self::Launch(_) => 3,
            Self::PlayerHail { .. } => 5,
            Self::Combat(e) => e.kind,
            Self::Scan(e) => e.kind,
            Self::Fidget { .. } => 0x19,
        }
    }
}

// ---- The system record's objects -----------------------------------------------------------

/// What `FSystemInfo::FindShipOrSolar` finds: a ship, or a solar NPC of the
/// record (by its index).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpaceObjRef {
    /// A player's ship or an NPC ship.
    Ship(ShipKey),
    /// A solar NPC (`FSolarNpc`) of the record.
    Solar {
        /// The record.
        system: u32,
        /// Its index in `SolarNpcs`.
        at: usize,
    },
}

impl SpaceObjRef {
    /// The vibe of the object's reputation handle; `None` without one.
    #[must_use]
    pub fn reputation(&self, cx: &Cx<'_>) -> Option<i32> {
        let state = &cx.logic.content.state;
        match *self {
            Self::Ship(ShipKey::Player(player)) => state.players.get(&player).map(|p| p.reputation),
            Self::Ship(ShipKey::Npc(npc)) => state
                .npc_ships
                .get(&npc)
                .and_then(|s| s.reputation)
                .map(|h| state.rep_handles.vibe(Some(h))),
            Self::Solar { system, at } => state
                .systems
                .get(&system)
                .and_then(|r| r.solar_npcs.get(at))
                .and_then(|n| n.reputation),
        }
    }
}

/// `FSystemInfo::FindShip(Id)` on the record of `system`; nothing without
/// it, where the C++ reads through null.
#[must_use]
pub fn find_ship(cx: &Cx<'_>, system: Option<u32>, id: u32) -> Option<ShipKey> {
    ships::find_in_system(cx, system?, id)
}

/// `FSystemInfo::FindSpaceObj(Id)`: the record's NPC ship of the id.
#[must_use]
pub fn find_space_obj(cx: &Cx<'_>, system: Option<u32>, id: u32) -> Option<NpcShipKey> {
    ships::find_space_obj(&cx.logic.content.state, system?, id)
}

/// `FSystemInfo::FindShipOrSolar(Id)` (`SystemInfo.cpp:577-589`): the ship,
/// else the last solar NPC of the id.
#[must_use]
pub fn find_ship_or_solar(cx: &Cx<'_>, system: Option<u32>, id: u32) -> Option<SpaceObjRef> {
    let system = system?;
    if let Some(ship) = ships::find_in_system(cx, system, id) {
        return Some(SpaceObjRef::Ship(ship));
    }
    let record = cx.logic.content.state.systems.get(&system)?;
    record
        .solar_npcs
        .iter()
        .rposition(|n| n.object == id)
        .map(|at| SpaceObjRef::Solar { system, at })
}

/// `FContentNpcShip::IsSpacePopShip` of a ship as the C++ casts it: its
/// controller (`+0x170`, 0 for a player's ship) is SpacePop's.
#[must_use]
pub fn is_space_pop(cx: &Cx<'_>, ship: ShipKey) -> bool {
    let state = &cx.logic.content.state;
    ships::controller(state, ship) == state.space_pop.0
}

/// The record of `system` when there is one (`FindSystemInfo`).
fn record(cx: &Cx<'_>, system: u32) -> Option<u32> {
    cx.logic
        .content
        .state
        .systems
        .contains_key(&system)
        .then_some(system)
}

/// The guide of a ship, `None` for a ship that is no NPC's or has none.
fn ship_guide(cx: &Cx<'_>, ship: Option<ShipKey>) -> Option<super::guides::GuideRef> {
    let Some(ShipKey::Npc(npc)) = ship else {
        return None;
    };
    cx.logic
        .content
        .state
        .npc_ships
        .get(&npc)
        .and_then(|s| s.guide)
}

/// The trade guide's trade good and return good (`FTradeGuide` slots
/// `+0x4c` and `+0x50`, [`super::guides::trade_goods`]), read by the trade
/// lines of a ship whose guide type is 2. Any other guide, which the C++
/// casts unchecked, has none (D935).
#[must_use]
pub fn trade_goods(cx: &Cx<'_>, ship: Option<ShipKey>) -> (u32, u32) {
    ship_guide(cx, ship)
        .and_then(|g| super::guides::trade_goods(cx, g))
        .unwrap_or((0, 0))
}

/// The patrol guide's points to go (`FPatrolGuide::GetPathPointsToGo`,
/// slot `+0x4c`, [`super::guides::path_points_to_go`]), read by the patrol
/// lines of a ship whose guide type is 3. Any other guide, which the C++
/// casts unchecked, has none (D935).
#[must_use]
pub fn path_points_to_go(cx: &Cx<'_>, ship: Option<ShipKey>) -> i32 {
    ship_guide(cx, ship)
        .and_then(|g| super::guides::path_points_to_go(cx, g))
        .unwrap_or(0)
}

// ---- InitGcs and the admission -------------------------------------------------------------

/// `FSpaceSupport::InitGcs` (`SpaceSupport.cpp:315-347`): the handlers 0,
/// 1 and 2, the category intervals (an existing entry is kept), the
/// default interval 15, the chances and the categories that speakers and
/// listeners which are no SpacePop ships may use.
pub fn init(cx: &mut Cx<'_>) {
    let gcs = &mut cx.logic.content.state.gcs;
    gcs.handlers
        .extend([HANDLER_DOCK_HAIL, HANDLER_COMBAT, HANDLER_FIDGET]);
    let mut interval = |category: i32, value: f32| {
        gcs.intervals.entry(category).or_insert(value);
    };
    interval(2, 12.0);
    interval(0x19, 30.0);
    gcs.default_interval = f64::from(15.0_f32);
    gcs.chances.entry(7).or_insert(0.4);
    gcs.chances.entry(6).or_insert(0.05);
    for (category, value) in [
        (6, 6.0),
        (7, 0.0),
        (8, 6.0),
        (9, 3.0),
        (0xa, 3.0),
        (0xb, 3.0),
        (0xd, 3.0),
        (0xe, 3.0),
        (0xf, 3.0),
        (0x10, 3.0),
    ] {
        gcs.intervals.entry(category).or_insert(value);
    }
    for category in [6, 7, 8, 0xa, 0xb, 0xd, 0xe, 0xf, 0x10] {
        gcs.speaker_categories.insert(category);
        gcs.listener_categories.insert(category);
    }
}

/// `IsGcsCommAllowed(Category, System, Speaker, Listener)` (`:4372-4463`):
/// refused while the GCS clamp is on; a speaker or listener of the record's
/// NPC ships that is no SpacePop ship needs its category opened. Categories
/// 1 to 4 need the speaker (a ship or a solar NPC) to be no NPC ship, and
/// are always allowed for a player listener. Otherwise the speaker's (else
/// the listener's) position must be known; the newest comm of the category
/// within 10 km must be older than the category's interval (without one,
/// an interval of exactly -1 refuses), and one `rand()` must fall below the
/// category's chance (1 by default).
pub fn is_comm_allowed(
    cx: &mut Cx<'_>,
    category: i32,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) -> bool {
    if cx.logic.content.state.clamps.gcs_clamp_enabled() {
        return false;
    }
    let gcs = &cx.logic.content.state.gcs;
    if let Some(s) = find_space_obj(cx, system, speaker)
        && !is_space_pop(cx, ShipKey::Npc(s))
        && !gcs.speaker_categories.contains(&category)
    {
        return false;
    }
    if let Some(l) = find_space_obj(cx, system, listener)
        && !is_space_pop(cx, ShipKey::Npc(l))
        && !gcs.listener_categories.contains(&category)
    {
        return false;
    }
    if (1..=4).contains(&category) {
        let Some(object) = find_ship_or_solar(cx, system, speaker) else {
            return false;
        };
        if matches!(object, SpaceObjRef::Ship(ShipKey::Npc(_))) {
            return false;
        }
        if players::find_by_ship(&cx.logic.content.state.players, listener).is_some() {
            return true;
        }
    }
    let now = game_support::game_time(cx);
    let at = if speaker != 0 { speaker } else { listener };
    let Ok((position, _)) = api::space_obj::location(cx, at) else {
        return false;
    };
    let gcs = &mut cx.logic.content.state.gcs;
    let recent = gcs.recent.entry(category).or_default();
    let near = recent.iter().find(|r| {
        let dx = (f64::from(r.position.x) - f64::from(position.x)) as f32;
        let dy = (f64::from(r.position.y) - f64::from(position.y)) as f32;
        let dz = (f64::from(r.position.z) - f64::from(position.z)) as f32;
        let length_sq = f64::from(dz) * f64::from(dz)
            + f64::from(dy) * f64::from(dy)
            + f64::from(dx) * f64::from(dx);
        length_sq < 100_000_000.0
    });
    let interval = gcs.intervals.get(&category).copied();
    if let Some(near) = near {
        let min = interval.unwrap_or(gcs.default_interval as f32);
        let since = game_support::time_distance(now, f64::from(near.time));
        if f64::from(min) > since {
            return false;
        }
    } else {
        let min = interval.map_or(gcs.default_interval, f64::from);
        if min == -1.0 {
            return false;
        }
    }
    let chance = gcs.chances.get(&category).copied().unwrap_or(1.0);
    let roll = f64::from(cx.world.rng().rand()) * f64::from(1.0_f32 / 32768.0);
    roll < f64::from(chance)
}

/// `RecordGcsComm(Category, System, Speaker, Listener)` (`:4465-4488`):
/// the comm at the game time and the speaker's (else the listener's)
/// position, -0.1 each when it has none; at most 10 per category, the
/// newest first.
pub fn record_comm(
    cx: &mut Cx<'_>,
    category: i32,
    system: Option<u32>,
    speaker: u32,
    listener: u32,
) {
    let at = if speaker != 0 { speaker } else { listener };
    let position =
        api::space_obj::location(cx, at).map_or(Vector::new(-0.1, -0.1, -0.1), |(p, _)| p);
    let comm = RecentComm {
        system,
        time: game_support::game_time(cx) as f32,
        position,
        speaker,
        listener,
    };
    let recent = cx
        .logic
        .content
        .state
        .gcs
        .recent
        .entry(category)
        .or_default();
    if recent.len() == 10 {
        recent.pop_back();
    }
    recent.push_front(comm);
}

// ---- The conversation manager --------------------------------------------------------------

/// `FGcsConversationManager::QueueConversation(Event)` (`:3537-3635`):
/// the dock events (1 to 3) pass the admission first and are recorded (type
/// 2 with both objects, 1 and 3 with the speaker); the event's conversation
/// is built, dropped when invalid, else started (its first line plays) and
/// queued. 1 when one was queued (type 7 the first line's duration), else
/// 0.
pub fn queue_conversation(cx: &mut Cx<'_>, event: &Event) -> f32 {
    if !cx.logic.content.state.gcs.enabled {
        return 0.0;
    }
    let kind = event.type_id();
    match event {
        Event::DockHail {
            system,
            speaker,
            listener,
        } => {
            if !is_comm_allowed(cx, kind, *system, *speaker, *listener) {
                return 0.0;
            }
            record_comm(cx, kind, *system, *speaker, 0);
        }
        Event::Dock(e) => {
            if !is_comm_allowed(cx, kind, e.system, e.speaker, e.listener) {
                return 0.0;
            }
            record_comm(cx, kind, e.system, e.speaker, e.listener);
        }
        Event::Launch(e) => {
            if !is_comm_allowed(cx, kind, e.system, e.speaker, e.listener) {
                return 0.0;
            }
            record_comm(cx, kind, e.system, e.speaker, 0);
        }
        Event::Combat(e) if !(6..=0x10).contains(&e.kind) => return 0.0,
        Event::Scan(e) if !(0x11..=0x18).contains(&e.kind) => return 0.0,
        _ => {}
    }
    let conversation = conversations::build(cx, event);
    if !conversations::is_valid(cx, &conversation) {
        return 0.0;
    }
    // `Start`: the first line plays.
    if let Some(first) = conversation.lines.first().cloned() {
        lines::play(cx, &first);
    }
    let duration = if kind == 7 {
        conversation
            .lines
            .first()
            .map_or(0.0, |l| lines::duration(cx, l))
    } else {
        1.0
    };
    cx.logic.content.state.gcs.conversations.push(conversation);
    duration
}

/// `FGcsConversationManager::OnMessage(Parms)` (`:3481-3499`), SpaceSupport's
/// `CommComplete`: the first conversation that has the line drops it and
/// plays its next one when it can, else is finished; a finished one is
/// dropped.
pub fn on_message(cx: &mut Cx<'_>, done: &LineDone) {
    if !cx.logic.content.state.gcs.enabled {
        return;
    }
    let mut at = 0;
    while at < cx.logic.content.state.gcs.conversations.len() {
        let c = &cx.logic.content.state.gcs.conversations[at];
        let Some(line) = c.lines.iter().position(|l| lines::is_done(l, done)) else {
            at += 1;
            continue;
        };
        // `FGcsConversation::OnMessage` (`:2196-2215`).
        let c = &mut cx.logic.content.state.gcs.conversations[at];
        c.lines.remove(line);
        let next = c.lines.get(line).cloned();
        match next {
            Some(next) if lines::can_play(cx, &next) => lines::play(cx, &next),
            _ => cx.logic.content.state.gcs.conversations[at].finished = true,
        }
        if cx.logic.content.state.gcs.conversations[at].finished {
            cx.logic.content.state.gcs.conversations.remove(at);
        }
        return;
    }
}

/// `FGcsConversationManager::Update(DeltaTime)` (`:3501-3535`), on
/// SpaceSupport's heartbeat: outside single player, every delayed "line
/// done" whose time is up (the unrounded sum compared) is sent to
/// SpaceSupport as `CommComplete`, in queue order, once the list is walked.
pub fn update(cx: &mut Cx<'_>, dt: f32) {
    if api::misc::single_player(cx) {
        return;
    }
    let mut due = Vec::new();
    cx.logic.content.state.gcs.delayed.retain_mut(|m| {
        let sum = f64::from(dt) + f64::from(m.elapsed);
        m.elapsed = sum as f32;
        if sum < f64::from(m.delay) {
            return true;
        }
        due.push(*m);
        false
    });
    let to = cx.logic.content.state.space_support;
    for m in due {
        let _ = cx.dispatch(
            to,
            &mut ControllerMessage::CommComplete {
                listener: m.listener,
                system: m.system,
                speaker: m.speaker,
                line: m.text,
                result: crate::events::CommResult::Success,
            },
        );
    }
}

// ---- The handler set -----------------------------------------------------------------------

/// `FGcsHandlerSet::HandleMessage(MessageId, Parms)` (`:3682-3703`): `Hail`
/// (`{ship, object, system}`) starts a player hail when the ship is a
/// player's and the object an NPC ship of the record; every other message
/// goes to each handler in order.
pub fn handle_message(cx: &mut Cx<'_>, message: &mut ControllerMessage<'_>) {
    if let ControllerMessage::Hail { object, arg1, arg2 } = *message {
        let system = record(cx, arg2);
        let player = players::find_by_ship(&cx.logic.content.state.players, object);
        let hailed = find_space_obj(cx, system, arg1);
        if let (Some(player), Some(hailed)) = (player, hailed) {
            queue_conversation(
                cx,
                &Event::PlayerHail {
                    system,
                    player,
                    object: hailed,
                },
            );
        }
        return;
    }
    let handlers = cx.logic.content.state.gcs.handlers.clone();
    for handler in handlers {
        match handler {
            HANDLER_DOCK_HAIL => dock_hail_message(cx, message),
            HANDLER_COMBAT => combat_message(cx, message),
            HANDLER_FIDGET => fidget_message(cx, message),
            _ => {}
        }
    }
}

// ---- Handler 0, the dock hails -------------------------------------------------------------

/// `FGcsDockHailHandler::SuppressHail(&Ship, &Phantom, Delay)`
/// (`:3729-3738`): the ship is remembered as recently hailed by the phantom
/// (`None`: by every one), and a SpaceSupport timer drops the oldest such
/// entry after `delay` seconds. The timer's second word is the entry's
/// address in the C++; the port passes 0 (D934).
pub fn suppress_hail(cx: &mut Cx<'_>, ship: u32, phantom: Option<PhantomKey>, delay: i32) {
    cx.logic
        .content
        .state
        .gcs
        .dock_hail
        .recent_hails
        .push_back((phantom, ship));
    let timer = cx.logic.content.state.load_ids.dock_hail_timer;
    let to = cx.logic.content.state.space_support;
    let _ = api::controller::set_timer(cx, to, timer, 0, delay as f32);
}

/// `FGcsDockHailHandler::HandleMessage` (`:3740-3808`): its timer drops the
/// oldest recent hail; `SystemLoaded` and `SystemUnloaded` make and destroy
/// the stations' phantoms; a ship entering a phantom of the system that
/// has not been hailed by it (or by any) is remembered for 30 s and, a
/// SpacePop NPC ship outside a formation group, hailed by the phantom's
/// station (type 1).
fn dock_hail_message(cx: &mut Cx<'_>, message: &ControllerMessage<'_>) {
    match *message {
        ControllerMessage::TimerExpired { param1, .. } => {
            if param1 == cx.logic.content.state.load_ids.dock_hail_timer {
                // An empty list, which the C++ erases from anyway, keeps
                // nothing to drop (D937).
                cx.logic
                    .content
                    .state
                    .gcs
                    .dock_hail
                    .recent_hails
                    .pop_front();
            }
        }
        ControllerMessage::SystemLoaded { system } => on_system_loaded(cx, system),
        ControllerMessage::SystemUnloaded { system } => on_system_unloaded(cx, system),
        ControllerMessage::PhantomEnter {
            object,
            phantom,
            system,
        } => phantom_enter(cx, object, phantom, system),
        _ => {}
    }
}

/// The `PhantomEnter` case of the dock hail handler (`:3767-3808`).
fn phantom_enter(cx: &mut Cx<'_>, ship: u32, phantom: PhantomKey, system: u32) {
    let handler = &cx.logic.content.state.gcs.dock_hail;
    let Some(map) = handler.phantoms.get(&system) else {
        return;
    };
    if handler
        .recent_hails
        .iter()
        .any(|&(p, s)| (p == Some(phantom) || p.is_none()) && s == ship)
    {
        return;
    }
    let Some(&station) = map.get(&phantom.0) else {
        return;
    };
    suppress_hail(cx, ship, Some(phantom), 30);
    // The record is not tested in the C++ (D935).
    let system = record(cx, system);
    let Some(npc) = find_space_obj(cx, system, ship) else {
        return;
    };
    let key = ShipKey::Npc(npc);
    if ships::is_wingman(&cx.logic.content.state, key) || !is_space_pop(cx, key) {
        return;
    }
    queue_conversation(
        cx,
        &Event::DockHail {
            system,
            speaker: station,
            listener: ship,
        },
    );
}

/// `FGcsDockHailHandler::OnSystemLoaded(&SystemId)` (`:3810-3849`): every
/// station and docking object of the system (`ScanObjects`, 0x40 at most,
/// radius -1, mask `0x200100`) whose type has a bit of `0x200100` and not
/// `2`, and whose docking target's base record has voice data (a
/// `msg_id_prefix`), gets an active 2500 m phantom (flags `0x7f0000`) at its
/// position. A phantom the server refuses, which the C++ keeps as a null
/// entry, is not kept.
fn on_system_loaded(cx: &mut Cx<'_>, system: u32) {
    let objects = api::system::scan_objects(cx, system, 0x40, Vector::ZERO, -1.0, 0x20_0100)
        .unwrap_or_default();
    for object in objects {
        let object_type = api::space_obj::object_type(cx, object).unwrap_or(0);
        if object_type & 0x20_0100 == 0 || object_type & 2 != 0 {
            continue;
        }
        let base = api::space_obj::docking_target(cx, object).unwrap_or(0);
        if !cx
            .logic
            .content
            .state
            .bases
            .get(&base)
            .is_some_and(|b| !b.msg_id_prefix.is_empty())
        {
            continue;
        }
        let position = api::space_obj::location(cx, object).map_or(Vector::ZERO, |(p, _)| p);
        let Ok(phantom) = api::phantom::create_sphere(cx, system, 2500.0, position, 0x7f_0000)
        else {
            continue;
        };
        let _ = api::phantom::set_active(cx, phantom, true);
        cx.logic
            .content
            .state
            .gcs
            .dock_hail
            .phantoms
            .entry(system)
            .or_default()
            .insert(phantom.0, object);
    }
}

/// `FGcsDockHailHandler::OnSystemUnloaded(&SystemId)` (`:3851-3882`): the
/// system's phantoms are destroyed (its empty entry stays); system 0
/// destroys every system's and their entries.
pub fn on_system_unloaded(cx: &mut Cx<'_>, system: u32) {
    let handler = &mut cx.logic.content.state.gcs.dock_hail;
    let keys: Vec<u32> = if system != 0 {
        let Some(map) = handler.phantoms.get_mut(&system) else {
            return;
        };
        std::mem::take(map).into_keys().collect()
    } else {
        let all = std::mem::take(&mut handler.phantoms);
        all.into_values().flat_map(BTreeMap::into_keys).collect()
    };
    for key in keys {
        let _ = api::phantom::destroy(cx, PhantomKey(key));
    }
}

// ---- Handler 1, the combat messages --------------------------------------------------------

/// A combat event of `kind` between `a` and `b`, queued.
fn queue_combat(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    kind: i32,
    a: u32,
    b: u32,
    health: f32,
    reason: i32,
) -> f32 {
    queue_conversation(
        cx,
        &Event::Combat(CombatEvent {
            system,
            kind,
            a,
            b,
            health,
            reason,
        }),
    )
}

/// The common steps of the combat handler's cases: admitted, recorded and
/// queued.
fn admit_record_queue(
    cx: &mut Cx<'_>,
    system: Option<u32>,
    kind: i32,
    a: u32,
    b: u32,
    reason: i32,
) {
    if !is_comm_allowed(cx, kind, system, a, b) {
        return;
    }
    record_comm(cx, kind, system, a, b);
    queue_combat(cx, system, kind, a, b, 0.0, reason);
}

/// `FGcsHandler1::HandleMessage` (`:3888-4152`): the combat messages become
/// combat events once admitted, each recorded and queued:
/// - `DeathFuseLit`: a Content ship's death (7, the victim's side); the
///   conversation's result is the fuse's lifetime;
/// - `ObjectDamaged`: damage above 0 of kind 2 to 5 or 7 to an object with
///   a type, the victim's health below 1 (6);
/// - `ObjectDestroyed` of kind 1 by a Content ship: the killer brags (7),
///   unless a player killed an NPC whose faction he does not dislike;
/// - `CombatState`: engage (9), no engage (0xb, a ship), combat over (0xa);
/// - `CombatDrift` (0xd), `NpcTurnedHostile` (0xc), `ShipInSights` (8),
///   `FleeRequest` (0xe), an unprovoked `DamageReputation` (0xf), and a
///   SpacePop leader with wingmen taking the lead (0x10).
#[allow(clippy::too_many_lines)]
fn combat_message(cx: &mut Cx<'_>, message: &mut ControllerMessage<'_>) {
    match message {
        ControllerMessage::DeathFuseLit(e) => {
            let system = record(cx, e.system);
            if find_ship(cx, system, e.object).is_none() {
                return;
            }
            if !is_comm_allowed(cx, 7, system, 0, e.object) {
                return;
            }
            e.lifetime = queue_combat(cx, system, 7, 0, e.object, 0.0, 0);
            record_comm(cx, 7, system, 0, e.object);
        }
        ControllerMessage::ObjectDamaged(e) => {
            let e = *e;
            if !(e.damage > 0.0) {
                return;
            }
            if e.cause < 2 || (e.cause > 5 && e.cause != 7) {
                return;
            }
            let system = record(cx, e.system);
            if api::space_obj::object_type(cx, e.id).is_err() {
                return;
            }
            if !is_comm_allowed(cx, 6, system, e.inflictor, e.id) {
                return;
            }
            // A failed read leaves the parameter pointer's bits in the
            // C++'s local; the port reads 0 (D936).
            let health = api::space_obj::relative_health(cx, e.id).unwrap_or(0.0);
            if !(health < 1.0) {
                return;
            }
            record_comm(cx, 6, system, e.inflictor, e.id);
            queue_combat(cx, system, 6, e.inflictor, e.id, health, 0);
        }
        ControllerMessage::ObjectDestroyed(e) => {
            if e.kind != 1 {
                return;
            }
            let (victim_id, killer_id) = (e.id, e.killer);
            let system = record(cx, e.system);
            let Some(killer) = find_ship(cx, system, killer_id) else {
                return;
            };
            if let Some(victim) = find_ship(cx, system, victim_id) {
                let state = &cx.logic.content.state;
                let killer_ship = ships::id(state, killer);
                let victim_ship = ships::id(state, victim);
                if players::find_by_ship(&state.players, killer_ship).is_some()
                    && players::find_by_ship(&state.players, victim_ship).is_none()
                {
                    let group = ships::affiliation(cx, victim);
                    let rep = ships::reputation(&cx.logic.content.state, killer);
                    let feelings =
                        api::reputation::group_feelings_towards(cx, rep, group).unwrap_or(0.0);
                    if feelings >= -0.2 {
                        return;
                    }
                }
            }
            if !is_comm_allowed(cx, 7, system, killer_id, victim_id) {
                return;
            }
            queue_combat(cx, system, 7, killer_id, 0, 0.0, 0);
            record_comm(cx, 7, system, killer_id, victim_id);
        }
        ControllerMessage::CombatState {
            state,
            param1,
            param2,
            system,
        } => {
            let (state, a, b) = (*state, *param1, *param2);
            let system = record(cx, *system);
            match state {
                0 => admit_record_queue(cx, system, 9, a, b, 0),
                1 | 2 => {
                    if !is_comm_allowed(cx, 0xb, system, a, b) {
                        return;
                    }
                    // A failed `GetType` leaves the parameter pointer's bits
                    // in the C++'s local; the port reads 0 (D936).
                    let object_type = api::space_obj::object_type(cx, a).unwrap_or(0);
                    if object_type & 0x7f_0000 == 0 {
                        return;
                    }
                    record_comm(cx, 0xb, system, a, b);
                    queue_combat(cx, system, 0xb, a, b, 0.0, state as i32);
                }
                3 | 4 => {
                    if !is_comm_allowed(cx, 0xa, system, a, b) {
                        return;
                    }
                    record_comm(cx, 0xa, system, a, b);
                    queue_combat(cx, system, 0xa, a, b, 0.0, state as i32);
                }
                _ => {}
            }
        }
        ControllerMessage::CombatDrift { ship, system } => {
            let (ship, system) = (*ship, record(cx, *system));
            admit_record_queue(cx, system, 0xd, ship, 0, 0);
        }
        ControllerMessage::NpcTurnedHostile {
            object,
            other,
            system,
        } => {
            let (a, b, system) = (*object, *other, record(cx, *system));
            admit_record_queue(cx, system, 0xc, a, b, 0);
        }
        ControllerMessage::ShipInSights {
            ship,
            object,
            system,
        } => {
            let (a, b, system) = (*ship, *object, record(cx, *system));
            admit_record_queue(cx, system, 8, a, b, 0);
        }
        ControllerMessage::FleeRequest {
            ship,
            reason,
            system,
        } => {
            let (ship, reason, system) = (*ship, *reason as i32, record(cx, *system));
            admit_record_queue(cx, system, 0xe, ship, 0, reason);
        }
        ControllerMessage::DamageReputation {
            provoked,
            attacker,
            victim,
            system,
        } => {
            if *provoked {
                return;
            }
            let (a, b, system) = (*attacker, *victim, record(cx, *system));
            admit_record_queue(cx, system, 0xf, a, b, 0);
        }
        ControllerMessage::FormationLeaderChanged {
            new_leader,
            old_leader,
        } => {
            let (new_leader, old_leader) = (*new_leader, *old_leader);
            let state = &cx.logic.content.state;
            let system = state.npc_ships.get(&new_leader).and_then(|s| s.base.system);
            let new_key = ShipKey::Npc(new_leader);
            if !is_space_pop(cx, new_key) || !names::has_formation_wingmen(cx, Some(new_key)) {
                return;
            }
            let state = &cx.logic.content.state;
            let a = ships::id(state, new_key);
            let b = ships::id(state, ShipKey::Npc(old_leader));
            admit_record_queue(cx, system, 0x10, a, b, 0);
        }
        _ => {}
    }
}

// ---- Handler 2, the fidgets ----------------------------------------------------------------

/// `rand() * 2^-15` below `chance` (`IsRandBelow`).
fn rand_below(cx: &mut Cx<'_>, chance: f32) -> bool {
    f64::from(cx.world.rng().rand()) * (1.0 / 32768.0) < f64::from(chance)
}

/// The squared length the fidget handler sums: `z² + y² + x²` of a float
/// difference.
fn fidget_length_sq(a: Vector, b: Vector) -> f64 {
    let dx = (f64::from(a.x) - f64::from(b.x)) as f32;
    let dy = (f64::from(a.y) - f64::from(b.y)) as f32;
    let dz = (f64::from(a.z) - f64::from(b.z)) as f32;
    f64::from(dz) * f64::from(dz) + f64::from(dy) * f64::from(dy) + f64::from(dx) * f64::from(dx)
}

/// `FGcsHandler2::HandleMessage` (`:4183-4330`), on the heartbeat: each
/// player with a ship not handled yet may (1 in 120) get a chattering
/// SpacePop formation leader that is not busy, picked at random among those
/// of his system within 2500 m whose formation has nobody busy; the players
/// within 2500 m of it count as handled, and it speaks (0x19: kind 2 in 30,
/// else 1 in 5, else 0).
fn fidget_message(cx: &mut Cx<'_>, message: &ControllerMessage<'_>) {
    if !matches!(message, ControllerMessage::Heartbeat { .. }) {
        return;
    }
    let mut handled: Vec<u32> = Vec::new();
    let players: Vec<u32> = cx.logic.content.state.players.keys().copied().collect();
    for player in players {
        let Some(ship) = cx
            .logic
            .content
            .state
            .players
            .get(&player)
            .and_then(|p| p.ship.as_ref())
            .map(|s| (s.ship, s.system))
        else {
            continue;
        };
        if handled.contains(&player) {
            continue;
        }
        if !rand_below(cx, 0.008_333_334) {
            continue;
        }
        let (ship_id, system) = ship;
        if !is_comm_allowed(cx, 0x19, system, 0, ship_id) {
            continue;
        }
        let state = &cx.logic.content.state;
        let objects = system
            .and_then(|s| state.systems.get(&s))
            .map(|r| r.space_objs.clone())
            .unwrap_or_default();
        let mut candidates: Vec<NpcShipKey> = objects
            .into_iter()
            .filter(|&k| {
                let key = ShipKey::Npc(k);
                is_space_pop(cx, key)
                    && !ships::is_wingman(state, key)
                    && !ships::base(state, key).is_some_and(|s| s.in_combat)
            })
            .collect();
        let player_at = ships::position(cx, ShipKey::Player(player));
        let mut speaker = None;
        while !candidates.is_empty() {
            let count = candidates.len() as i32;
            let index = (cx.world.rng().rand() * count / 32768) as usize;
            let pick = candidates[index];
            let pick_at = ships::position(cx, ShipKey::Npc(pick));
            if fidget_length_sq(player_at, pick_at) < f64::from(FIDGET_RANGE_SQUARED) {
                let state = &cx.logic.content.state;
                let formation = ships::formation_id(state, ShipKey::Npc(pick));
                let busy = super::formations::collect_members(state, formation)
                    .iter()
                    .any(|&m| ships::base(state, m).is_some_and(|s| s.in_combat));
                if !busy {
                    speaker = Some(pick);
                    let others: Vec<u32> = cx.logic.content.state.players.keys().copied().collect();
                    for other in others {
                        if handled.contains(&other) {
                            continue;
                        }
                        if cx
                            .logic
                            .content
                            .state
                            .players
                            .get(&other)
                            .is_none_or(|p| p.ship.is_none())
                        {
                            continue;
                        }
                        let other_at = ships::position(cx, ShipKey::Player(other));
                        if !(fidget_length_sq(other_at, pick_at) < f64::from(FIDGET_RANGE_SQUARED))
                        {
                            continue;
                        }
                        handled.push(other);
                    }
                    break;
                }
            }
            candidates.remove(index);
        }
        let Some(speaker) = speaker else {
            continue;
        };
        let speaker_id = ships::id(&cx.logic.content.state, ShipKey::Npc(speaker));
        record_comm(cx, 0x19, system, speaker_id, ship_id);
        let kind = if rand_below(cx, 0.033_333_335) {
            2
        } else if rand_below(cx, 0.2) {
            1
        } else {
            0
        };
        queue_conversation(
            cx,
            &Event::Fidget {
                system,
                ship: speaker,
                kind,
            },
        );
    }
}
