//! Content's player states (`FContentPlayerState`, TU46
//! `Content/PlayerState.cpp`, with the `[mPlayer]` reader and the rank of
//! `Content/Support.cpp`): work package P3.4d, decision record 0008 point
//! 10.
//!
//! `GContentPlayerStates` (`PlayerState.cpp:37`) is a map by player id in
//! [`ContentState`](super::ContentState) ([`PlayerStates`]). GameSupport
//! keeps each state's ship, system, base and location records, writes
//! `[mPlayer]` on `PlayerSave` ([`PlayerState::save`]) and reads it on
//! `PlayerLoad` ([`load_player_state`]); SpaceSupport's lane answer asks
//! the route check ([`PlayerState::check_tradelane_route`]).
//!
//! Where the C++ holds a pointer, the state holds a key: the system record
//! by system id and the base record by base id, each `None` where the
//! C++'s lookup gave null. The player's ship object (`FContentPlayerShip`)
//! is the [`PlayerShip`] the state owns. The ship's hostile contacts
//! (SpaceSupport's heartbeat), its directives and the formations are
//! P3.6's; the mission and random-mission fields are kept here and set by
//! P3.5 and P3.7. P3.7c restores a save's `[RandomMission]` sections
//! ([`load_random_mission_section`]) and adds the group's mission
//! ([`group_live_mission`], [`join_mission_as_group`]).

use std::collections::BTreeMap;
use std::io::{self, Write};

use neuron_core::id::IdString;
use neuron_core::ini::IniReader;
use neuron_core::msvcrt::format_f;
use neuron_core::num::trunc_to_i32;

use super::gate_locks::GateLocks;
use crate::api;
use crate::server::Cx;

/// `[mPlayer]` `vnpc`: base, NPC and two values (`FVnpcRecord`).
pub type VnpcRecord = [u32; 4];

/// `[mPlayer]` `rumor`: the rumor and its count (`FRumorRecord`).
pub type RumorRecord = [u32; 2];

/// `[mPlayer]` `tlr_exception`: a pair of rings (`FTlrException`).
pub type TlrException = [u32; 2];

/// The statistics block (`FContentPlayerStats`, player `+0x48`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PlayerStats {
    /// `ship_type_killed`: kills by ship archetype.
    pub ship_types_killed: BTreeMap<u32, i32>,
    /// `rm_completed`: by random-mission type.
    pub random_missions_completed: BTreeMap<u8, i32>,
    /// `rm_aborted`.
    pub random_missions_aborted: BTreeMap<u8, i32>,
    /// `rm_failed`.
    pub random_missions_failed: BTreeMap<u8, i32>,
    /// `total_cash_earned`.
    pub total_cash_earned: f32,
    /// `total_time_played`: GameSupport's heartbeats add to it.
    pub total_time_played: f32,
    /// `sys_visited`, unique, in first-visit order.
    pub systems_visited: Vec<u32>,
    /// `base_visited`.
    pub bases_visited: Vec<u32>,
    /// `holes_visited`: the jump holes targeted and their exits.
    pub holes_visited: Vec<u32>,
}

/// The player's ship object (`FContentPlayerShip`, 0x190 bytes, over the
/// ship base of TU50 and the space object of TU20): made by GameSupport's
/// `SpaceEnter`, dropped on `SpaceExit` and when the ship is destroyed.
/// The fields the C++ reads in P3.4d's code; the directive list, the
/// attackers, the pending comms and the hostile contacts are P3.5's and
/// P3.6's.
#[derive(Clone, Debug, PartialEq)]
pub struct PlayerShip {
    /// `SpaceObjId` (TU20 `+0x04`): the ship.
    pub ship: u32,
    /// `ShipArchId`.
    pub archetype: u32,
    /// `ClassIds`: the ship's mission properties, then `fighter` or
    /// `freighter`.
    pub class_ids: Vec<u32>,
    /// `System` (TU20 `+0xdc`): the system record, by its id.
    pub system: Option<u32>,
    /// `FormationId` (TU50 `+0x154`, -1 for none).
    pub formation_id: u32,
    /// `FormationSlot` (TU50 `+0x14c`, 0 for the leader; -1 from the
    /// player ship's parameters).
    pub formation_slot: u32,
    /// `Formation` (TU50 `+0x150`): the formation the ship flies in, by its
    /// nickname id in [`ContentTables::formations`](super::ContentTables);
    /// `None` for the null pointer.
    pub formation: Option<u32>,
    /// `Damage` (TU50 `+0x13c`): the damage each attacker dealt
    /// (`AddDamage`), in the order they first hit.
    pub damage: Vec<(u32, f32)>,
    /// `+0x120`: the lane's entry ring while the ship travels a lane, else 0.
    pub lane_entry: u32,
    /// `+0x124`: the lane's end ring.
    pub lane_end: u32,
    /// `bInCombat` (`+0x11c`).
    pub in_combat: bool,
    /// `bScanTarget`.
    pub scan_target: bool,
    /// `TargetId` (`+0x174`): the last target (`Msg0x25`).
    pub target: u32,
    /// `CalmSince` (`+0x178`): -1 while hostiles are near.
    pub calm_since: f64,
    /// `HostileContactCount` (`+0x18c`).
    pub hostile_contact_count: i32,
    /// `HostileContacts` (TU46): the hostile NPC ships near the ship
    /// ([`super::combat_reputation::update_hostile_contacts`], P3.6c).
    pub hostile_contacts: Vec<super::combat_reputation::HostileContact>,
    /// `Attackers` (TU20 `+0x14`): the ship's hits on each victim
    /// ([`super::combat_reputation`], P3.6c).
    pub attackers: [super::combat_reputation::Attacker; 8],
    /// TU50 `Directives`, `CurrentDirective`, `Priority` and
    /// `ActiveDirective` (`+0x158` to `+0x16c`) of a player's ship object:
    /// a mission's `Act_GiveObjList` for `Player` fills it (P3.5c3). The
    /// player's ship has no directive callback, so only the directives that
    /// finish at once advance it. An NPC ship keeps its queue in
    /// [`NpcShip::directives`](super::npc_ships::NpcShip::directives) and
    /// leaves its base's empty.
    pub directives: super::directives::ShipDirectives,
    /// `PendingComms` (TU20 `+0x108`): the mission dialog lines the ship
    /// says when the line before ends (P3.5c4,
    /// [`super::systems::send_pending_comms`]).
    pub pending_comms: Vec<super::systems::PendingComm>,
}

impl PlayerShip {
    /// `FContentPlayerShip(Params)` as GameSupport's `SpaceEnter` makes it:
    /// the parameters' defaults (no formation: slot and id -1, a null
    /// formation), no damage, calm.
    #[must_use]
    pub fn new(ship: u32, archetype: u32, class_ids: Vec<u32>, system: Option<u32>) -> Self {
        Self {
            ship,
            archetype,
            class_ids,
            system,
            formation_id: u32::MAX,
            formation_slot: u32::MAX,
            formation: None,
            damage: Vec::new(),
            lane_entry: 0,
            lane_end: 0,
            in_combat: false,
            scan_target: false,
            target: 0,
            calm_since: -1.0,
            hostile_contact_count: 0,
            hostile_contacts: Vec::new(),
            attackers: [super::combat_reputation::Attacker::default(); 8],
            directives: super::directives::ShipDirectives::default(),
            pending_comms: Vec::new(),
        }
    }
}

/// One player's state (`FContentPlayerState`, 0x10c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct PlayerState {
    /// `PlayerId` (`+0x0c`).
    pub player: u32,
    /// `Reputation` (`+0x08`): the player's vibe (`pub::Player::GetRep`).
    pub reputation: i32,
    /// `ShipObject` (`+0x10`), owned.
    pub ship: Option<PlayerShip>,
    /// `LocationId` (`+0x14`).
    pub location: u32,
    /// `SystemRecord` (`+0x18`): the system record, by its id.
    pub system: Option<u32>,
    /// `BaseRecord` (`+0x1c`): the base record, by its id.
    pub base: Option<u32>,
    /// `bShipDestroyed` (`+0x20`).
    pub ship_destroyed: bool,
    /// `bCanDock` (`+0x21`, true).
    pub can_dock: bool,
    /// `DockExceptions` (`+0x24`).
    pub dock_exceptions: Vec<u32>,
    /// `bCanTradelane` (`+0x34`, true).
    pub can_tradelane: bool,
    /// `TradelaneExceptions` (`+0x38`).
    pub tradelane_exceptions: Vec<TlrException>,
    /// The statistics (`+0x48`).
    pub stats: PlayerStats,
    /// `+0xd0`: flags; bit 2 is cleared when the countdown runs out.
    pub flags: u32,
    /// `+0xd4`: a countdown, -1 when idle.
    pub countdown: f32,
    /// `+0xd8`: the live random mission's controller (P3.7).
    pub live_mission: u32,
    /// `MissionNumber` (`+0xdc`): the story's chapter (P3.5).
    pub mission_number: u32,
    /// `NextVendorOfferId` (`+0xe0`, 1).
    pub next_vendor_offer_id: u32,
    /// `+0xe4` (SpaceSupport's manoeuvre, P3.6).
    pub value_e4: u32,
    /// `+0xe8`.
    pub value_e8: u32,
    /// `Vnpcs` (`+0xec`).
    pub vnpcs: Vec<VnpcRecord>,
    /// `Rumors` (`+0xfc`).
    pub rumors: Vec<RumorRecord>,
}

/// `GContentPlayerStates`: the states by player id.
pub type PlayerStates = BTreeMap<u32, PlayerState>;

/// `"RELAYSEVEN_system"`, which `OnSystemChange` does not count as visited.
pub const RELAY_SEVEN_SYSTEM: &[u8] = b"RELAYSEVEN_system";

impl PlayerState {
    /// `FContentPlayerState(PlayerId)` (`:85-99`): the defaults, then the
    /// player's vibe (`pub::Player::GetRep`; -1 for an invalid player).
    #[must_use]
    pub fn new(cx: &Cx<'_>, player: u32) -> Self {
        Self {
            player,
            reputation: api::player::rep(cx, player).unwrap_or(-1),
            ship: None,
            location: 0,
            system: None,
            base: None,
            ship_destroyed: false,
            can_dock: true,
            dock_exceptions: Vec::new(),
            can_tradelane: true,
            tradelane_exceptions: Vec::new(),
            stats: PlayerStats::default(),
            flags: 0,
            countdown: -1.0,
            live_mission: 0,
            mission_number: 0,
            next_vendor_offer_id: 1,
            value_e4: 0,
            value_e8: 0,
            vnpcs: Vec::new(),
            rumors: Vec::new(),
        }
    }

    /// `GetShipId`: the ship object's ship, 0 without one.
    #[must_use]
    pub fn ship_id(&self) -> u32 {
        self.ship.as_ref().map_or(0, |s| s.ship)
    }

    /// `UpdateTime(DeltaTime)` (`:229-246`): the play time grows; a running
    /// countdown (not exactly -1, NaN included) runs down, and at 0 or
    /// below it stops and clears flag bit 2.
    pub fn update_time(&mut self, elapsed: f32) {
        self.stats.total_time_played =
            (f64::from(elapsed) + f64::from(self.stats.total_time_played)) as f32;
        if self.countdown == -1.0 {
            return;
        }
        self.countdown = (f64::from(self.countdown) - f64::from(elapsed)) as f32;
        if self.countdown <= 0.0 {
            self.countdown = -1.0;
            self.flags &= 0xffff_fffb;
        }
    }

    /// `SetDockAccess(bCan, Exceptions)` (`:500-508`).
    pub fn set_dock_access(&mut self, can: bool, exceptions: &[u32]) {
        self.can_dock = can;
        self.dock_exceptions = exceptions.to_vec();
    }

    /// `SetTradelaneAccess(bCan, Exceptions)` (`:511-519`).
    pub fn set_tradelane_access(&mut self, can: bool, exceptions: &[TlrException]) {
        self.can_tradelane = can;
        self.tradelane_exceptions = exceptions.to_vec();
    }

    /// `CanDockAt(DockTargetId)` (`:609-624`): `can_dock`, inverted for a
    /// target of the exception list.
    #[must_use]
    pub fn can_dock_at(&self, target: u32) -> bool {
        let listed = self.dock_exceptions.contains(&target);
        if self.can_dock { !listed } else { listed }
    }

    /// `CheckTradelaneRoute(RingA, RingB)` (`:528-563`): 0 allowed, 1 or 2
    /// refused. The default is 1 without `can_tl`, else 0. With a system
    /// record and exceptions, `lane` is the rings of `ring_a`'s lane
    /// (`GetTradeLaneRingsByRing`, empty for an unknown ring): for every
    /// ring of the lane and every exception pair whose first ring is that
    /// ring and `ring_a`, `ring_b` at either end of the pair, or a ring of
    /// the lane reached while the toggle (flipped at both ends of the pair,
    /// in lane order) is set, gives `can_tl ? 1 : 0`; a scan without such a
    /// hit gives 2. The last pair processed decides.
    #[must_use]
    pub fn check_tradelane_route(&self, lane: &[u32], ring_a: u32, ring_b: u32) -> i32 {
        let can = i32::from(self.can_tradelane);
        let mut result = i32::from(!self.can_tradelane);
        if self.system.is_none() || self.tradelane_exceptions.is_empty() {
            return result;
        }
        for &ring in lane {
            for pair in &self.tradelane_exceptions {
                if ring != pair[0] || ring_a != pair[0] {
                    continue;
                }
                if ring_b == pair[0] || ring_b == pair[1] {
                    result = can;
                    continue;
                }
                let mut inside = false;
                let mut hit = false;
                for &other in lane {
                    if other == pair[0] || other == pair[1] {
                        inside = !inside;
                    }
                    if other == ring_b && inside {
                        hit = true;
                        break;
                    }
                }
                result = if hit { can } else { 2 };
            }
        }
        result
    }

    /// `AddRumor(Id)` (`:383-398`).
    pub fn add_rumor(&mut self, id: u32) {
        match self.rumors.iter_mut().find(|r| r[0] == id) {
            Some(r) => r[1] = r[1].wrapping_add(1),
            None => self.rumors.push([id, 1]),
        }
    }

    /// `FindRumor(Id)`.
    #[must_use]
    pub fn find_rumor(&self, id: u32) -> u32 {
        self.rumors.iter().find(|r| r[0] == id).map_or(0, |r| r[1])
    }

    /// `RecordVnpcTalk(BaseId, NpcId)` (`:409-433`, BaseSupport's
    /// `GFObjSelect`): nothing for NPC 0; a new record `{base, npc, 1, 0}`
    /// when none is the NPC's or its count is 0; else value 3 is cleared
    /// unless it is 1, and the count grows.
    pub fn record_vnpc_talk(&mut self, base: u32, npc: u32) {
        if npc == 0 {
            return;
        }
        match self.vnpcs.iter_mut().find(|r| r[0] == base && r[1] == npc) {
            Some(record) if record[2] != 0 => {
                if record[3] != 1 {
                    record[3] = 0;
                }
                record[2] = record[2].wrapping_add(1);
            }
            _ => self.vnpcs.push([base, npc, 1, 0]),
        }
    }

    /// `GetVnpcValue2(BaseId, NpcId)` (`:908-916`): the talk count of the
    /// NPC's record, 0 without one.
    #[must_use]
    pub fn vnpc_value2(&self, base: u32, npc: u32) -> u32 {
        self.vnpcs
            .iter()
            .find(|r| r[0] == base && r[1] == npc)
            .map_or(0, |r| r[2])
    }

    /// `FindVnpcValue3(BaseId, &NpcId, &Value)` (`:893-905`): value 3 of
    /// the NPC's record, `None` without one.
    #[must_use]
    pub fn find_vnpc_value3(&self, base: u32, npc: u32) -> Option<u32> {
        self.vnpcs
            .iter()
            .find(|r| r[0] == base && r[1] == npc)
            .map(|r| r[3])
    }

    /// `CountShipTypeKilled(ShipArchId)`.
    pub fn count_ship_type_killed(&mut self, archetype: u32) {
        *self.stats.ship_types_killed.entry(archetype).or_insert(0) += 1;
    }

    /// `AllocateVendorOfferId`: the id, then one more.
    pub fn allocate_vendor_offer_id(&mut self) -> u32 {
        let id = self.next_vendor_offer_id;
        self.next_vendor_offer_id = id.wrapping_add(1);
        id
    }

    /// `FContentPlayerState::Save(File)` (`:249-287`): `[mPlayer]` with the
    /// gates locked for the player, the vnpc and rumor records, the dock and
    /// lane access, then the statistics. The C++ writes through a text-mode
    /// `FILE`; `out` gets the `\n`s and the save path translates them.
    ///
    /// A vnpc's base nickname is `pub::GetBaseNickname` into a 0x40 buffer;
    /// an unknown base leaves the buffer unset in the C++, and writes an
    /// empty name here (D800).
    ///
    /// # Errors
    ///
    /// The writer's.
    pub fn save(&self, cx: &mut Cx<'_>, locks: &GateLocks, out: &mut dyn Write) -> io::Result<()> {
        let gates = locks.collect(self.player);
        out.write_all(b"[mPlayer]\n")?;
        for gate in gates {
            writeln!(out, "locked_gate = {gate}")?;
        }
        for record in &self.vnpcs {
            let (_, name) = api::misc::base_nickname(cx, 0x40, record[0]);
            let name = name.unwrap_or_default();
            write!(
                out,
                "vnpc = {}, {}, {}, {} ; ",
                record[0], record[1], record[2], record[3]
            )?;
            out.write_all(&name)?;
            writeln!(out, ", {}", IdString(record[1]).to_hex_string())?;
        }
        for rumor in &self.rumors {
            writeln!(out, "rumor = {}, {}", rumor[0], rumor[1])?;
        }
        writeln!(out, "can_dock = {}", i32::from(self.can_dock))?;
        for target in &self.dock_exceptions {
            writeln!(out, "dock_exception = {target}")?;
        }
        writeln!(out, "can_tl = {}", i32::from(self.can_tradelane))?;
        for pair in &self.tradelane_exceptions {
            writeln!(out, "tlr_exception = {}, {}", pair[0], pair[1])?;
        }
        save_stats(&self.stats, out)
    }
}

/// `SaveContentPlayerStats(Stats, File)` (`:470-500`): the maps in key
/// order, the two totals as `%f` of their `double`, the visited lists.
///
/// # Errors
///
/// The writer's.
pub fn save_stats(stats: &PlayerStats, out: &mut dyn Write) -> io::Result<()> {
    for (ship, count) in &stats.ship_types_killed {
        writeln!(out, "ship_type_killed = {ship}, {count}")?;
    }
    for (kind, count) in &stats.random_missions_completed {
        writeln!(out, "rm_completed = {kind}, {count}")?;
    }
    for (kind, count) in &stats.random_missions_aborted {
        writeln!(out, "rm_aborted = {kind}, {count}")?;
    }
    for (kind, count) in &stats.random_missions_failed {
        writeln!(out, "rm_failed = {kind}, {count}")?;
    }
    writeln!(
        out,
        "total_cash_earned = {}",
        format_f(f64::from(stats.total_cash_earned), 6)
    )?;
    writeln!(
        out,
        "total_time_played = {}",
        format_f(f64::from(stats.total_time_played), 6)
    )?;
    for system in &stats.systems_visited {
        writeln!(out, "sys_visited = {system}")?;
    }
    for base in &stats.bases_visited {
        writeln!(out, "base_visited = {base}")?;
    }
    for hole in &stats.holes_visited {
        writeln!(out, "holes_visited = {hole}")?;
    }
    Ok(())
}

/// `AddVisitedUnique`.
fn add_unique(list: &mut Vec<u32>, id: u32) {
    if !list.contains(&id) {
        list.push(id);
    }
}

/// `LoadContentPlayerStatsValue` (`Support.cpp:75-133`): one value of
/// `[mPlayer]` that the state's own keys did not take. A count map keeps
/// the first line of a key (`emplace`).
fn load_stats_value(stats: &mut PlayerStats, reader: &mut IniReader) {
    let count_pair = |reader: &IniReader| (reader.value_int(1), reader.value_int(0));
    if reader.is_value(b"ship_type_killed") {
        let (count, ship) = count_pair(reader);
        stats.ship_types_killed.entry(ship as u32).or_insert(count);
    } else if reader.is_value(b"rm_completed") {
        let (count, kind) = count_pair(reader);
        stats
            .random_missions_completed
            .entry(kind as u8)
            .or_insert(count);
    } else if reader.is_value(b"rm_aborted") {
        let (count, kind) = count_pair(reader);
        stats
            .random_missions_aborted
            .entry(kind as u8)
            .or_insert(count);
    } else if reader.is_value(b"rm_failed") {
        let (count, kind) = count_pair(reader);
        stats
            .random_missions_failed
            .entry(kind as u8)
            .or_insert(count);
    } else if reader.is_value(b"total_cash_earned") {
        stats.total_cash_earned = reader.value_float(0) as f32;
    } else if reader.is_value(b"total_time_played") {
        stats.total_time_played = reader.value_float(0) as f32;
    } else if reader.is_value(b"sys_visited") {
        add_unique(&mut stats.systems_visited, reader.value_int(0) as u32);
    } else if reader.is_value(b"base_visited") {
        add_unique(&mut stats.bases_visited, reader.value_int(0) as u32);
    } else if reader.is_value(b"holes_visited") {
        add_unique(&mut stats.holes_visited, reader.value_int(0) as u32);
    }
}

/// `LoadContentPlayerState(State, Reader)` (`Support.cpp:299-405`) over a
/// fresh state of `player`: its vnpc and rumor records emptied, then from
/// the reader's position every `[mPlayer]` section: `locked_gate` locks the
/// gate for the player, the records, the access and the statistics. The
/// rank is computed again, and the reader is put back where it was.
///
/// A `[RandomMission]` section re-creates the player's random mission
/// ([`load_random_mission_section`], P3.7c); the state is in the table
/// while it does, as the C++'s is.
pub fn load_player_state(cx: &mut Cx<'_>, state: &mut PlayerState, reader: &mut IniReader) {
    state.vnpcs.clear();
    state.rumors.clear();
    let saved = reader.state();
    while reader.read_header() {
        if reader.is_header(b"RandomMission") {
            let player = state.player;
            let previous = cx.logic.content.state.players.insert(player, state.clone());
            load_random_mission_section(cx, player, reader);
            if let Some(back) = cx.logic.content.state.players.remove(&player) {
                *state = back;
            }
            if let Some(previous) = previous {
                cx.logic.content.state.players.insert(player, previous);
            }
            continue;
        }
        if !reader.is_header(b"mPlayer") {
            continue;
        }
        while reader.read_value() {
            if reader.is_value(b"locked_gate") {
                let gate = reader.value_int(0) as u32;
                cx.logic
                    .content
                    .state
                    .gate_locks
                    .register(state.player, gate);
            } else if reader.is_value(b"vnpc") {
                let record = [
                    reader.value_int(0) as u32,
                    reader.value_int(1) as u32,
                    reader.value_int(2) as u32,
                    reader.value_int(3) as u32,
                ];
                state.vnpcs.push(record);
            } else if reader.is_value(b"can_dock") {
                state.can_dock = reader.value_bool(0);
            } else if reader.is_value(b"dock_exception") {
                state.dock_exceptions.push(reader.value_int(0) as u32);
            } else if reader.is_value(b"can_tl") {
                state.can_tradelane = reader.value_bool(0);
            } else if reader.is_value(b"tlr_exception") {
                state
                    .tradelane_exceptions
                    .push([reader.value_int(0) as u32, reader.value_int(1) as u32]);
            } else if reader.is_value(b"rumor") {
                state
                    .rumors
                    .push([reader.value_int(0) as u32, reader.value_int(1) as u32]);
            } else {
                load_stats_value(&mut state.stats, reader);
            }
        }
    }
    recalculate_rank(cx, state);
    reader.set_state(&saved);
}

/// The `[RandomMission]` branch of `LoadContentPlayerState`
/// (`Support.cpp:361-397`): each `type` line (the rest are skipped) makes
/// a loader that loads the save's random mission ([`Loader::load`], which
/// reads every `[RandomMission]` section from the reader's start and puts
/// the reader back). A loaded mission becomes a controller of that type at
/// `ABOVE_NORMAL`, the player's live mission;
/// `PlayerMissionChanged {controller, player}` is broadcast and the
/// player's mission id set. The loader is released either way.
///
/// [`Loader::load`]: crate::content::random_mission::mission::Loader::load
pub fn load_random_mission_section(cx: &mut Cx<'_>, player: u32, reader: &mut IniReader) {
    use crate::content::random_mission::mission::Loader;
    use crate::events::ControllerMessage;
    use crate::server::CreateParams;
    use neuron_server::controller::{ControllerId, Priority};

    while reader.read_value() {
        if !reader.is_value(b"type") {
            continue;
        }
        // `strncpy` into 0x3c bytes; a name of 0x3c or more has no end there
        // and runs into the stack (D953).
        let mut kind = reader.value_string_at(0);
        kind.truncate(0x3b);
        let mut loader = Loader::new();
        let (loaded, mission) = loader.load(cx, reader, player);
        if loaded {
            let controller = api::controller::create(
                cx,
                b"content.dll",
                &kind,
                &CreateParams::RandomMission(mission),
                Priority::AboveNormal,
            );
            if let Some(state) = cx.logic.content.state.players.get_mut(&player) {
                state.live_mission = controller.0;
            }
            let mut message = ControllerMessage::PlayerMissionChanged {
                mission: controller,
                player,
            };
            let _ = api::controller::send_message(cx, ControllerId::BROADCAST, &mut message);
            api::player::set_msn_id(cx, player, controller.0, player, false, 0);
        }
        loader.release(cx);
    }
}

/// `GetGroupLiveMission` (`PlayerState.cpp:457-470`): the player's live
/// random mission, else the first group member's; 0 without one.
pub fn group_live_mission(cx: &Cx<'_>, player: u32) -> u32 {
    let players = &cx.logic.content.state.players;
    let Some(state) = players.get(&player) else {
        return 0;
    };
    if state.live_mission != 0 {
        return state.live_mission;
    }
    let members = api::player::group_members(cx, player).unwrap_or_default();
    members
        .iter()
        .filter_map(|m| players.get(m))
        .map(|m| m.live_mission)
        .find(|&m| m != 0)
        .unwrap_or(0)
}

/// `JoinMissionAsGroup(ControllerId, VnpcOwner, Vnpc, bNotify, NotifyArg)`
/// (`PlayerState.cpp:853-883`): the player's vnpc record `{owner, vnpc}`
/// marked (value 3 set to 1; none marked when none matches, D953); then
/// every group member with a state gets the mission as its live mission,
/// `PlayerMissionChanged {controller, member}` broadcast and its mission
/// id set (by this player, or by the member for player 0).
pub fn join_mission_as_group(
    cx: &mut Cx<'_>,
    player: u32,
    controller: u32,
    vnpc_owner: u32,
    vnpc: u32,
    notify: bool,
    notify_arg: u32,
) {
    use crate::events::ControllerMessage;
    use neuron_server::controller::ControllerId;

    if vnpc_owner != 0
        && vnpc != 0
        && let Some(state) = cx.logic.content.state.players.get_mut(&player)
        && let Some(record) = state
            .vnpcs
            .iter_mut()
            .find(|r| r[0] == vnpc_owner && r[1] == vnpc)
    {
        record[3] = 1;
    }
    let members = api::player::group_members(cx, player).unwrap_or_default();
    for member in members {
        let Some(state) = cx.logic.content.state.players.get_mut(&member) else {
            continue;
        };
        state.live_mission = controller;
        let mut message = ControllerMessage::PlayerMissionChanged {
            mission: ControllerId(controller),
            player: member,
        };
        let _ = api::controller::send_message(cx, ControllerId::BROADCAST, &mut message);
        let set_by = if player == 0 { member } else { player };
        api::player::set_msn_id(cx, member, controller, set_by, notify, notify_arg);
    }
}

/// `RecalculateContentPlayerRank(State)` (`Support.cpp:209-297`): the
/// player's worth (`(unsigned)(int)(cash + asset value)`) against the
/// toughness scale gives the rank of the last point at or below it, which
/// the player's vibe takes (`pub::Reputation::SetRank`). With the story's
/// last chapter (mission number 42) the next rank's cash goes to the player
/// (`SetMoneyNeededToNextRank`, at least 1, `0x7fffffff` when there is no
/// next point), and a table rank above the player's rank becomes it.
pub fn recalculate_rank(cx: &mut Cx<'_>, state: &PlayerState) {
    let cash = api::player::inspect_cash(cx, state.player).unwrap_or(0);
    let asset = api::player::asset_value(cx, state.player).unwrap_or(0.0);
    let worth = trunc_to_i32(f64::from(cash) + f64::from(asset)) as u32;
    let tables = std::sync::Arc::clone(&cx.logic.content.tables);
    let scale = &tables.toughness_scale;
    let mut next_threshold = 0.0_f32;
    let mut table_rank = 0u32;
    if !scale.is_empty() {
        let mut last_at_or_below = 0usize;
        let mut cursor = 0usize;
        while cursor < scale.len() {
            if scale[cursor].experience as u32 > worth {
                next_threshold = scale[cursor].experience as u32 as f32;
                break;
            }
            last_at_or_below = cursor;
            cursor += 1;
        }
        table_rank = if last_at_or_below == cursor {
            0
        } else {
            scale[last_at_or_below].toughness as u32
        };
    }
    let _ = api::reputation::set_rank(cx, state.reputation, table_rank as f32);
    if state.mission_number != 0x2a {
        return;
    }
    let player_rank = api::player::rank(cx, state.player).unwrap_or(0) as u32;
    let remainder = |threshold: f32| {
        let r = f64::from(threshold) - f64::from(worth);
        trunc_to_i32(if r < 1.0 { 1.0 } else { r })
    };
    let mut money = 0x7fff_ffff;
    if table_rank < player_rank {
        let wanted = player_rank.wrapping_add(1);
        if let Some(point) = scale.iter().find(|p| p.toughness as u32 == wanted) {
            next_threshold = point.experience as u32 as f32;
            if next_threshold != 0.0 {
                money = remainder(next_threshold);
            }
        }
    } else if next_threshold != 0.0 {
        money = remainder(next_threshold);
    }
    let _ = api::player::set_money_needed_to_next_rank(cx, state.player, money);
    if table_rank > player_rank {
        let _ = api::player::set_rank(cx, state.player, table_rank as i32);
    }
}

/// `SetContentPlayerMissionNumber(State, MissionNumber, bAdvanceRank)`
/// (`Support.cpp:408-445`): with the flag, the chapters that end a rank
/// raise the player's rank by one; then the number is stored.
pub fn set_mission_number(cx: &mut Cx<'_>, player: u32, mission: u32, advance_rank: bool) {
    if advance_rank
        && matches!(
            mission,
            3 | 5 | 7 | 9 | 10 | 12 | 13 | 15 | 16 | 18 | 19 | 22 | 24 | 25 | 27 | 28 | 31 | 42
        )
    {
        let rank = api::player::rank(cx, player).unwrap_or(0);
        let _ = api::player::set_rank(cx, player, rank.wrapping_add(1));
    }
    if let Some(state) = cx.logic.content.state.players.get_mut(&player) {
        state.mission_number = mission;
    }
}

/// `FindContentPlayerStateByShip(&ShipId)`: the first state in key order
/// whose ship object has the ship.
#[must_use]
pub fn find_by_ship(states: &PlayerStates, ship: u32) -> Option<u32> {
    states
        .values()
        .find(|s| s.ship.as_ref().is_some_and(|o| o.ship == ship))
        .map(|s| s.player)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state() -> PlayerState {
        PlayerState {
            player: 1,
            reputation: -1,
            ship: None,
            location: 0,
            system: Some(5),
            base: None,
            ship_destroyed: false,
            can_dock: true,
            dock_exceptions: Vec::new(),
            can_tradelane: true,
            tradelane_exceptions: Vec::new(),
            stats: PlayerStats::default(),
            flags: 4,
            countdown: -1.0,
            live_mission: 0,
            mission_number: 0,
            next_vendor_offer_id: 1,
            value_e4: 0,
            value_e8: 0,
            vnpcs: Vec::new(),
            rumors: Vec::new(),
        }
    }

    #[test]
    fn a_vnpc_talk_counts_and_clears_value_3_unless_it_is_1() {
        let mut s = state();
        s.record_vnpc_talk(5, 0);
        assert!(s.vnpcs.is_empty());
        s.record_vnpc_talk(5, 9);
        assert_eq!(s.vnpcs, vec![[5, 9, 1, 0]]);
        s.vnpcs[0][3] = 2;
        s.record_vnpc_talk(5, 9);
        assert_eq!(s.vnpcs, vec![[5, 9, 2, 0]]);
        s.vnpcs[0][3] = 1;
        s.record_vnpc_talk(5, 9);
        assert_eq!(s.vnpcs, vec![[5, 9, 3, 1]]);
        assert_eq!(s.vnpc_value2(5, 9), 3);
        assert_eq!(s.find_vnpc_value3(5, 9), Some(1));
        assert_eq!(s.find_vnpc_value3(5, 8), None);
        // A record whose count is 0 gets a new one after it.
        s.vnpcs[0][2] = 0;
        s.record_vnpc_talk(5, 9);
        assert_eq!(s.vnpcs.len(), 2);
    }

    #[test]
    fn the_route_check_follows_the_exception_pairs() {
        let mut s = state();
        let lane = [10, 11, 12, 13, 14];
        // No exceptions: the access alone.
        assert_eq!(s.check_tradelane_route(&lane, 10, 14), 0);
        s.can_tradelane = false;
        assert_eq!(s.check_tradelane_route(&lane, 10, 14), 1);
        // A pair 11..13 from ring 11: a ring inside answers the access,
        // one outside 2.
        s.tradelane_exceptions = vec![[11, 13]];
        assert_eq!(s.check_tradelane_route(&lane, 11, 12), 0);
        assert_eq!(s.check_tradelane_route(&lane, 11, 13), 0);
        assert_eq!(s.check_tradelane_route(&lane, 11, 14), 2);
        s.can_tradelane = true;
        assert_eq!(s.check_tradelane_route(&lane, 11, 12), 1);
        // Another first ring: the default.
        assert_eq!(s.check_tradelane_route(&lane, 10, 12), 0);
        // Without a system record: the default.
        s.system = None;
        assert_eq!(s.check_tradelane_route(&lane, 11, 14), 0);
    }

    #[test]
    fn the_countdown_clears_bit_two() {
        let mut s = state();
        s.update_time(0.5);
        assert_eq!(s.stats.total_time_played, 0.5);
        assert_eq!(s.flags, 4);
        s.countdown = 1.0;
        s.update_time(0.75);
        assert_eq!(s.countdown, 0.25);
        s.update_time(0.25);
        assert_eq!((s.countdown, s.flags), (-1.0, 0));
    }

    #[test]
    fn dock_access_and_rumors() {
        let mut s = state();
        s.set_dock_access(false, &[7]);
        assert!(s.can_dock_at(7));
        assert!(!s.can_dock_at(8));
        s.add_rumor(3);
        s.add_rumor(3);
        assert_eq!(s.find_rumor(3), 2);
        assert_eq!(s.allocate_vendor_offer_id(), 1);
        assert_eq!(s.allocate_vendor_offer_id(), 2);
    }

    #[test]
    fn stats_write_their_totals_as_f() {
        let mut stats = PlayerStats::default();
        stats.ship_types_killed.insert(9, 2);
        stats.random_missions_completed.insert(1, 3);
        stats.total_cash_earned = 1234.5;
        stats.total_time_played = 0.1;
        stats.systems_visited = vec![4, 5];
        let mut out = Vec::new();
        save_stats(&stats, &mut out).expect("write");
        assert_eq!(
            String::from_utf8_lossy(&out),
            "ship_type_killed = 9, 2\nrm_completed = 1, 3\n\
             total_cash_earned = 1234.500000\ntotal_time_played = 0.100000\n\
             sys_visited = 4\nsys_visited = 5\n"
        );
        let mut reader = IniReader::default();
        let mut text = b"[mPlayer]\n".to_vec();
        text.extend_from_slice(&out);
        assert!(reader.open_memory(&text));
        assert!(reader.read_header());
        let mut back = PlayerStats::default();
        while reader.read_value() {
            load_stats_value(&mut back, &mut reader);
        }
        assert_eq!(back, stats);
    }
}
