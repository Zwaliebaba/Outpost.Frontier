//! The gate locks of TU53 (`Content/SpaceSupport.cpp:1125-1351`): work
//! package P3.4d, decision record 0008 point 10.
//!
//! Two process-wide tables in [`ContentState`](super::ContentState):
//!
//! - `GContentGateLocks` (`:70`), an ordered vector of `{gate, players}`:
//!   a gate locked for the players it lists, player 0 standing for every
//!   player;
//! - `GContentNpcGateLocks` (`:1184`), the gates locked for NPC traffic,
//!   unique, in insertion order.
//!
//! GameSupport reads `[locked_gates]` on `LoadGame` and writes it on
//! `SaveGame` ([`GateLocks::load`], [`GateLocks::save`]); a player's
//! `[mPlayer]` writes and reads the gates locked for that player
//! ([`super::players`]). The system paths' builder asks for player 0
//! ([`super::system_paths::SystemPaths::build`]). The mission actions that
//! lock and unlock gates are P3.5's, and `IsGateLockedForShip`'s NPC part
//! (a SpacePop ship) P3.6's.

use std::io::{self, Write};

use neuron_core::ini::IniReader;

/// One lock: the gate and the players it is locked for (0 for every
/// player).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GateLock {
    /// The gate's object id.
    pub gate: u32,
    /// The players, in the order they were added.
    pub players: Vec<u32>,
}

/// `GContentGateLocks` and `GContentNpcGateLocks`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GateLocks {
    /// The player locks, in insertion order.
    pub locks: Vec<GateLock>,
    /// The gates locked for NPCs, unique, in insertion order.
    pub npc: Vec<u32>,
}

impl GateLocks {
    /// `RegisterContentGateLock(PlayerId, GateId)` (`:1126-1146`): the
    /// player joins the first lock of the gate (once), or a new lock
    /// `{gate, [player]}` goes at the end.
    pub fn register(&mut self, player: u32, gate: u32) {
        if let Some(lock) = self.locks.iter_mut().find(|l| l.gate == gate) {
            if !lock.players.contains(&player) {
                lock.players.push(player);
            }
            return;
        }
        self.locks.push(GateLock {
            gate,
            players: vec![player],
        });
    }

    /// `CollectContentGateLocks(PlayerId, OutGates)` (`:1149-1160`): the
    /// gates of every lock that lists the player or player 0, in table
    /// order, each once.
    #[must_use]
    pub fn collect(&self, player: u32) -> Vec<u32> {
        let mut gates = Vec::new();
        for lock in &self.locks {
            if (lock.players.contains(&player) || lock.players.contains(&0))
                && !gates.contains(&lock.gate)
            {
                gates.push(lock.gate);
            }
        }
        gates
    }

    /// `IsContentGateLocked(&PlayerId, &GateId)` (`:1165-1177`): the first
    /// lock of the gate lists player 0 or the player; false without a lock.
    #[must_use]
    pub fn is_locked(&self, player: u32, gate: u32) -> bool {
        self.locks
            .iter()
            .find(|l| l.gate == gate)
            .is_some_and(|l| l.players.contains(&0) || l.players.contains(&player))
    }

    /// `RegisterContentNpcGateLock(&GateId)` (`:1188-1192`): appended unless
    /// listed.
    pub fn register_npc(&mut self, gate: u32) {
        if !self.npc.contains(&gate) {
            self.npc.push(gate);
        }
    }

    /// `IsContentNpcGateLocked(&GateId)` (`:1195-1198`).
    #[must_use]
    pub fn is_npc_locked(&self, gate: u32) -> bool {
        self.npc.contains(&gate)
    }

    /// `RemoveContentNpcGateLock(&GateId)` (`:1201-1212`): the first entry
    /// of the gate goes.
    pub fn remove_npc(&mut self, gate: u32) {
        if let Some(index) = self.npc.iter().position(|&g| g == gate) {
            self.npc.remove(index);
        }
    }

    /// `UnregisterContentGateLock(&PlayerId, &GateId)` (`:1243-1281`):
    /// player 0 drops the first lock of the gate; gate 0 drops the player
    /// from every lock; otherwise the player leaves the first lock of the
    /// gate. In single player the player-0 entry of a lock the player left
    /// goes too, and a lock left without players is removed.
    pub fn unregister(&mut self, player: u32, gate: u32, single_player: bool) {
        if player == 0 {
            if let Some(index) = self.locks.iter().position(|l| l.gate == gate) {
                self.locks.remove(index);
            }
            return;
        }
        let drop_player = |lock: &mut GateLock| {
            erase_first(&mut lock.players, player);
            if single_player && lock.players.contains(&0) {
                erase_first(&mut lock.players, 0);
            }
            lock.players.is_empty()
        };
        if gate == 0 {
            let mut index = 0;
            while index < self.locks.len() {
                if drop_player(&mut self.locks[index]) {
                    self.locks.remove(index);
                } else {
                    index += 1;
                }
            }
            return;
        }
        if let Some(index) = self.locks.iter().position(|l| l.gate == gate)
            && drop_player(&mut self.locks[index])
        {
            self.locks.remove(index);
        }
    }

    /// `LoadContentGateLocks(Reader)` (`:1314-1334`), GameSupport's
    /// `LoadGame`: every `[locked_gates]` section's `locked_gate` locks the
    /// gate for player 0 and `npc_locked_gate` for NPCs. Nothing is
    /// cleared first.
    pub fn load(&mut self, reader: &mut IniReader) {
        while reader.read_header() {
            if !reader.is_header(b"locked_gates") {
                continue;
            }
            while reader.read_value() {
                if reader.is_value(b"locked_gate") {
                    self.register(0, reader.value_int(0) as u32);
                } else if reader.is_value(b"npc_locked_gate") {
                    self.register_npc(reader.value_int(0) as u32);
                }
            }
        }
    }

    /// `SaveContentGateLocks(File)` (`:1337-1351`), GameSupport's
    /// `SaveGame`: `[locked_gates]` when any lock is there, the NPC locks,
    /// then the gates locked for player 0. The C++ writes through a
    /// text-mode `FILE`; `out` gets the `\n`s and the save path translates
    /// them, as P3.3's writer does.
    ///
    /// # Errors
    ///
    /// The writer's.
    pub fn save(&self, out: &mut dyn Write) -> io::Result<()> {
        let gates = self.collect(0);
        if !self.npc.is_empty() || !gates.is_empty() {
            out.write_all(b"[locked_gates]\n")?;
        }
        for gate in &self.npc {
            writeln!(out, "npc_locked_gate = {gate}")?;
        }
        for gate in gates {
            writeln!(out, "locked_gate = {gate}")?;
        }
        Ok(())
    }
}

/// The first `value` erased.
fn erase_first(ids: &mut Vec<u32>, value: u32) {
    if let Some(index) = ids.iter().position(|&v| v == value) {
        ids.remove(index);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn locks_for_a_player_and_for_everybody() {
        let mut locks = GateLocks::default();
        locks.register(3, 100);
        locks.register(3, 100);
        locks.register(0, 200);
        locks.register(4, 100);
        assert_eq!(locks.locks[0].players, [3, 4]);
        assert_eq!(locks.collect(3), [100, 200]);
        assert_eq!(locks.collect(5), [200]);
        assert!(locks.is_locked(4, 100));
        assert!(!locks.is_locked(5, 100));
        assert!(locks.is_locked(5, 200));
        locks.unregister(3, 0, false);
        assert_eq!(locks.locks[0].players, [4]);
        locks.unregister(4, 100, false);
        assert_eq!(locks.locks.len(), 1);
        locks.unregister(0, 200, false);
        assert!(locks.locks.is_empty());
    }

    #[test]
    fn single_player_drops_the_everybody_entry() {
        let mut locks = GateLocks::default();
        locks.register(0, 7);
        locks.register(1, 7);
        locks.unregister(1, 7, true);
        assert!(locks.locks.is_empty());
    }

    #[test]
    fn locked_gates_round_trip() {
        let mut locks = GateLocks::default();
        locks.register(0, 2_926_089_285);
        locks.register(9, 11);
        locks.register_npc(5);
        locks.register_npc(5);
        let mut out = Vec::new();
        locks.save(&mut out).expect("write");
        assert_eq!(
            String::from_utf8_lossy(&out),
            "[locked_gates]\nnpc_locked_gate = 5\nlocked_gate = 2926089285\n"
        );
        let mut reader = IniReader::default();
        assert!(reader.open_memory(&out));
        let mut back = GateLocks::default();
        back.load(&mut reader);
        assert_eq!(back.collect(0), [2_926_089_285]);
        assert_eq!(back.npc, [5]);
        // Nothing to write without locks.
        let mut empty = Vec::new();
        GateLocks::default().save(&mut empty).expect("write");
        assert!(empty.is_empty());
    }
}
