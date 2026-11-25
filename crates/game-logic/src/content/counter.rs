//! Content's unique-id counter, `GContentNextUniqueId`
//! (`Content/Content.cpp:14`), and the five ids it hands out while
//! Content.dll loads (record 0008, "The id counter").
//!
//! The counter is a plain `unsigned int` that starts at 1 (constant
//! initialisation, so it is ready before any dynamic initialiser) and is
//! post-incremented by each user. Its values come back as the first word
//! of a `TimerExpired` (`Param1`), so they can be seen in the dispatch
//! trace (`dispatch.jsonl`, record 0006 point 10).
//!
//! Five dynamic initialisers take an id when the DLL loads:
//!
//! - the music timer (`GSpaceSupportMusicTimerId`, `Content/SpaceSupport.cpp:61`);
//! - the three faction-name release timers (`GFactionNameMsgPilot`,
//!   `GFactionNameMsgLargeShip`, `GFactionNameMsgFormation`,
//!   `Content/FactionProps.cpp:44-46`, in this order within the file);
//! - the dock-hail timer (`GGcsDockHailTimerId`, `Content/Gcs.cpp:64`).
//!
//! MSVC runs the dynamic initialisers of a module file by file in the
//! linker's object order, which is the order `Content.vcxproj` lists the
//! sources in: `SpaceSupport.cpp` (line 127), `FactionProps.cpp` (166),
//! `Gcs.cpp` (170). So the reference build hands out 1 to 5 in the order
//! above. The router's timer id, taken by GameSupport's constructor, is
//! the next value (6 when GameSupport is the first Content controller
//! since the load). At run time the counter serves the NPC ship timers
//! (`Content/NpcShip.cpp:1125`, `:1161`, `:1198`; P3.6), the mission nags
//! (`Content/MissionAct.cpp:1559`) and the NN path timers
//! (`Content/MissionObject.cpp:4296`; P3.5).

/// `GContentNextUniqueId`: the next id to hand out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct UniqueIds {
    next: u32,
}

impl Default for UniqueIds {
    /// The counter as the DLL is loaded: 1.
    fn default() -> Self {
        Self { next: 1 }
    }
}

impl UniqueIds {
    /// The value the next [`UniqueIds::take`] hands out.
    #[must_use]
    pub const fn peek(&self) -> u32 {
        self.next
    }

    /// `GContentNextUniqueId++`: the current value, the counter moved on
    /// (an `unsigned int`, so it wraps).
    pub fn take(&mut self) -> u32 {
        let id = self.next;
        self.next = id.wrapping_add(1);
        id
    }
}

/// The ids Content.dll's dynamic initialisers take when it loads, in the
/// reference build's initialiser order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LoadTimeIds {
    /// `GSpaceSupportMusicTimerId`: SpaceSupport's timer that cancels a
    /// player's music.
    pub music_timer: u32,
    /// `GFactionNameMsgPilot`: the release of a reserved pilot name.
    pub faction_name_pilot: u32,
    /// `GFactionNameMsgLargeShip`: the release of a reserved large-ship
    /// name.
    pub faction_name_large_ship: u32,
    /// `GFactionNameMsgFormation`: the release of a reserved formation
    /// designation.
    pub faction_name_formation: u32,
    /// `GGcsDockHailTimerId`: the comm system's recent-hail timer.
    pub dock_hail_timer: u32,
}

impl LoadTimeIds {
    /// Runs the five initialisers on `ids`: `SpaceSupport.cpp`'s, then
    /// `FactionProps.cpp`'s three, then `Gcs.cpp`'s.
    pub fn take(ids: &mut UniqueIds) -> Self {
        let music_timer = ids.take();
        let faction_name_pilot = ids.take();
        let faction_name_large_ship = ids.take();
        let faction_name_formation = ids.take();
        let dock_hail_timer = ids.take();
        Self {
            music_timer,
            faction_name_pilot,
            faction_name_large_ship,
            faction_name_formation,
            dock_hail_timer,
        }
    }

    /// The three faction-name timers, in the order SpaceSupport's
    /// `TimerExpired` case tests them (`Content/SpaceSupport.cpp:923-937`).
    #[must_use]
    pub const fn faction_names(&self) -> [u32; 3] {
        [
            self.faction_name_pilot,
            self.faction_name_large_ship,
            self.faction_name_formation,
        ]
    }
}
