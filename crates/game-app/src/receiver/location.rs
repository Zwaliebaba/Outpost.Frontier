//! The location controller's mirrors the `GF` slots write
//! (`menu_universe_base.cpp:691-870`, `LocationController.cpp`; work
//! package P5.9's seam, agreed with stream C).
//!
//! The C++ keeps them inside the LocationController's entries: each base
//! entry's runtime holds the mission offers (+0x00), the news (+0x18) and
//! the start location (+0x48); each room entry's state holds the
//! characters (+0x00), the script behaviours (+0x18) and the ambient
//! scripts (+0x30), each bucket with its `bComplete` flag. An IClient
//! forward writes only into an entry the controller has opened; an id the
//! controller does not know reports the reference's `FDUMP` and is
//! skipped, where the reference reads through null (D1401).
//!
//! The data is the receiver's ([`LocationDb`], `ClientReceiver::location`);
//! the controller (stream C's `base::location`) registers the ids, opens
//! and closes the runtimes and states, resets the flags when it requests,
//! waits on them and drains [`LocationDb::updated_characters`]
//! (`RoomControlUpdateCharacter`).

use std::collections::BTreeMap;

use game_sim::ai::script::ScriptBehavior;
use game_sim::protocol::ServerMessage;
use game_sim::protocol::base_props::{
    AmbientScriptProperties, CharacterProperties, MissionVendorAcceptance, MissionVendorProperties,
    NewsBroadcastProperties,
};
use game_sim::protocol::marshal::MarshalReader;
use neuron_core::diag::{self, DumpFlags};

/// The source the controller's reports name.
pub const LOCATION_CONTROLLER_CPP: &str = "Client\\Gf\\LocationController.cpp";

/// One `TMirrorPropertyBucket`: the records by key and the list's
/// `bComplete`.
#[derive(Clone, Debug)]
pub struct MirrorBucket<T> {
    /// The records by key.
    pub items: BTreeMap<u32, T>,
    /// The server sent the list's end.
    pub complete: bool,
}

impl<T> Default for MirrorBucket<T> {
    fn default() -> Self {
        Self {
            items: BTreeMap::new(),
            complete: false,
        }
    }
}

impl<T> MirrorBucket<T> {
    /// `Upsert`: the record under its key, replacing one there.
    pub fn upsert(&mut self, key: u32, item: T) {
        self.items.insert(key, item);
    }

    /// `Erase(Key)`.
    pub fn erase(&mut self, key: u32) {
        self.items.remove(&key);
    }

    /// The records gone and the flag down (a new request).
    pub fn reset(&mut self) {
        self.items.clear();
        self.complete = false;
    }
}

/// A base entry's runtime part the slots write.
#[derive(Clone, Debug, Default)]
pub struct BaseMirror {
    /// +0x00 `MissionVendorProperties`.
    pub mission_offers: MirrorBucket<MissionVendorProperties>,
    /// +0x18 `NewsBroadcastProperties`.
    pub news: MirrorBucket<NewsBroadcastProperties>,
    /// +0x48 `StartLocationId` (`SetClientLocationBaseStartLocation`).
    pub start_location: u32,
}

/// A room entry's state the slots write.
#[derive(Clone, Debug, Default)]
pub struct RoomMirror {
    /// +0x00 `CharacterProperties`, by key.
    pub characters: MirrorBucket<CharacterProperties>,
    /// +0x18 `ScriptBehavior`, by behaviour id.
    pub script_behaviors: MirrorBucket<ScriptBehavior>,
    /// +0x30 `AmbientScriptProperties`, by key.
    pub ambient_scripts: MirrorBucket<AmbientScriptProperties>,
}

/// A room entry: its state (`None` until the room is loaded) and whether
/// it has a runtime (a room control the character updates reach).
#[derive(Clone, Debug, Default)]
pub struct RoomEntry {
    /// +0x04 the state.
    pub state: Option<RoomMirror>,
    /// +0x08 a runtime exists.
    pub runtime: bool,
}

/// The mirrors, by base and room id.
#[derive(Clone, Debug, Default)]
pub struct LocationDb {
    /// `G_LocationBaseById`: every known base, its runtime part when
    /// opened.
    pub bases: BTreeMap<u32, Option<BaseMirror>>,
    /// `G_LocationRooms`: every known room.
    pub rooms: BTreeMap<u32, RoomEntry>,
    /// The last `GFMISSIONVENDORACCEPTANCE` (`NnMissionVendor::OnAcceptResult`
    /// takes it).
    pub vendor_acceptance: Option<MissionVendorAcceptance>,
    /// `G_MissionVendorNoMissionsReason` (`GFMISSIONVENDORWHYEMPTY`).
    pub vendor_why_empty: i32,
    /// The `RoomControlUpdateCharacter` calls, (room, key), in order, for
    /// the room control to take.
    pub updated_characters: Vec<(u32, u32)>,
    /// The forwards skipped because the controller does not know the base
    /// or room (each also reported; D1401).
    pub skipped: u32,
}

fn miss(line: u32, what: &str, id: u32) {
    diag::dump(
        DumpFlags::ERROR,
        &format!("{LOCATION_CONTROLLER_CPP}({line}) : *** ERROR: Could not find {what} id {id}\n"),
    );
}

impl LocationDb {
    /// The controller knows `base` (no runtime yet).
    pub fn register_base(&mut self, base: u32) {
        self.bases.entry(base).or_insert(None);
    }

    /// The controller knows `room` (no state or runtime yet).
    pub fn register_room(&mut self, room: u32) {
        self.rooms.entry(room).or_default();
    }

    /// The base's runtime made (empty buckets).
    pub fn open_base(&mut self, base: u32) {
        self.bases.insert(base, Some(BaseMirror::default()));
    }

    /// The base's runtime gone.
    pub fn close_base(&mut self, base: u32) {
        if let Some(entry) = self.bases.get_mut(&base) {
            *entry = None;
        }
    }

    /// The room's state made (empty buckets).
    pub fn open_room(&mut self, room: u32) {
        self.rooms.entry(room).or_default().state = Some(RoomMirror::default());
    }

    /// The room's state gone.
    pub fn close_room(&mut self, room: u32) {
        if let Some(entry) = self.rooms.get_mut(&room) {
            entry.state = None;
        }
    }

    /// The room's runtime made or gone (the character updates go to it).
    pub fn set_room_runtime(&mut self, room: u32, runtime: bool) {
        self.rooms.entry(room).or_default().runtime = runtime;
    }

    /// The base's runtime part, when opened.
    #[must_use]
    pub fn base(&self, base: u32) -> Option<&BaseMirror> {
        self.bases.get(&base)?.as_ref()
    }

    /// The base's runtime part, mutably.
    pub fn base_mut(&mut self, base: u32) -> Option<&mut BaseMirror> {
        self.bases.get_mut(&base)?.as_mut()
    }

    /// The room's state, when opened.
    #[must_use]
    pub fn room(&self, room: u32) -> Option<&RoomMirror> {
        self.rooms.get(&room)?.state.as_ref()
    }

    /// The room's state, mutably.
    pub fn room_mut(&mut self, room: u32) -> Option<&mut RoomMirror> {
        self.rooms.get_mut(&room)?.state.as_mut()
    }

    /// An id the controller does not know: counted and reported.
    fn miss(&mut self, line: u32, what: &str, id: u32) {
        self.skipped += 1;
        miss(line, what, id);
    }

    /// `FindLocationBaseEntry` then its runtime: `None` for a base not
    /// known (reported, line 0x100) or not opened.
    fn base_runtime(&mut self, base: u32) -> Option<&mut BaseMirror> {
        if !self.bases.contains_key(&base) {
            self.miss(0x100, "base", base);
            return None;
        }
        self.bases.get_mut(&base)?.as_mut()
    }

    /// `FindClientLocationRoomState`: `None` for a room not known
    /// (reported, line 0x110) or without a state.
    fn room_state(&mut self, room: u32) -> Option<&mut RoomMirror> {
        if !self.rooms.contains_key(&room) {
            self.miss(0x110, "location", room);
            return None;
        }
        self.rooms.get_mut(&room)?.state.as_mut()
    }

    /// `SetClientLocationBaseStartLocation`.
    pub fn set_base_start_location(&mut self, base: u32, location: u32) {
        if let Some(b) = self.base_runtime(base) {
            b.start_location = location;
        }
    }

    /// `UpsertClientLocationBaseMissionOffer`: under the offer's base.
    pub fn upsert_mission_offer(&mut self, offer: MissionVendorProperties) {
        if let Some(b) = self.base_runtime(offer.base_id) {
            b.mission_offers.upsert(offer.key as u32, offer);
        }
    }

    /// `EraseClientLocationBaseMissionOffer`.
    pub fn erase_mission_offer(&mut self, base: u32, key: u32) {
        if let Some(b) = self.base_runtime(base) {
            b.mission_offers.erase(key);
        }
    }

    /// `SetClientLocationBaseMissionListComplete`.
    pub fn set_mission_offers_complete(&mut self, base: u32) {
        if let Some(b) = self.base_runtime(base) {
            b.mission_offers.complete = true;
        }
    }

    /// `UpsertClientLocationBaseNews`: under the item's base.
    pub fn upsert_news(&mut self, news: NewsBroadcastProperties) {
        if let Some(b) = self.base_runtime(news.base_id) {
            b.news.upsert(news.key as u32, news);
        }
    }

    /// `EraseClientLocationBaseNews`.
    pub fn erase_news(&mut self, base: u32, key: u32) {
        if let Some(b) = self.base_runtime(base) {
            b.news.erase(key);
        }
    }

    /// `SetClientLocationBaseNewsListComplete`.
    pub fn set_news_complete(&mut self, base: u32) {
        if let Some(b) = self.base_runtime(base) {
            b.news.complete = true;
        }
    }

    /// `UpsertClientLocationRoomCharacter`: into the room's state, then
    /// (the room looked up again) to its runtime's room control.
    pub fn upsert_room_character(&mut self, character: CharacterProperties) {
        let (room, key) = (character.room_id, character.key);
        if let Some(s) = self.room_state(room) {
            s.characters.upsert(key, character);
        }
        match self.rooms.get(&room) {
            Some(entry) => {
                if entry.runtime {
                    self.updated_characters.push((room, key));
                }
            }
            None => self.miss(0x110, "location", room),
        }
    }

    /// `SetClientLocationRoomCharactersComplete`.
    pub fn set_room_characters_complete(&mut self, room: u32) {
        if let Some(s) = self.room_state(room) {
            s.characters.complete = true;
        }
    }

    /// `UpsertClientLocationRoomScriptBehavior`: the room is the
    /// behaviour's `+0x04`.
    pub fn upsert_room_script_behavior(&mut self, behavior: ScriptBehavior) {
        let (room, key) = (behavior.field_04, behavior.behavior_id as u32);
        if let Some(s) = self.room_state(room) {
            s.script_behaviors.upsert(key, behavior);
        }
    }

    /// `EraseClientLocationRoomScriptBehavior`.
    pub fn erase_room_script_behavior(&mut self, room: u32, key: u32) {
        if let Some(s) = self.room_state(room) {
            s.script_behaviors.erase(key);
        }
    }

    /// `SetClientLocationRoomScriptBehaviorsComplete`.
    pub fn set_room_script_behaviors_complete(&mut self, room: u32) {
        if let Some(s) = self.room_state(room) {
            s.script_behaviors.complete = true;
        }
    }

    /// `UpsertClientLocationRoomAmbientScript`.
    pub fn upsert_room_ambient_script(&mut self, script: AmbientScriptProperties) {
        let (room, key) = (script.room_id, script.key as u32);
        if let Some(s) = self.room_state(room) {
            s.ambient_scripts.upsert(key, script);
        }
    }

    /// `EraseClientLocationRoomAmbientScript`.
    pub fn erase_room_ambient_script(&mut self, room: u32, key: u32) {
        if let Some(s) = self.room_state(room) {
            s.ambient_scripts.erase(key);
        }
    }

    /// `SetClientLocationRoomAmbientScriptsComplete`.
    pub fn set_room_ambient_scripts_complete(&mut self, room: u32) {
        if let Some(s) = self.room_state(room) {
            s.ambient_scripts.complete = true;
        }
    }
}

impl super::ClientReceiver {
    /// The forwards of the `GF` slots 29 to 45 into the mirrors, after the
    /// slot's oracle line (`gameflow`). A record that does not decode is
    /// dropped (D101: the reference reads past the buffer).
    pub(super) fn location_forward(&mut self, m: &ServerMessage) {
        use ServerMessage as M;
        let db = &mut self.location;
        match m {
            M::GfDestroyCharacter(_) => {}
            M::GfDestroyScriptBehavior(p) => db.erase_room_script_behavior(p.field_00, p.field_04),
            M::GfDestroyAmbientScript(p) => db.erase_room_ambient_script(p.field_00, p.field_04),
            M::GfDestroyMissionComputer(p) => db.erase_mission_offer(p.field_00, p.field_04),
            M::GfDestroyNewsBroadcast(p) => db.erase_news(p.field_00, p.field_04),
            M::GfCompleteCharList(p) => db.set_room_characters_complete(p.value),
            M::GfCompleteScriptBehaviorList(p) => db.set_room_script_behaviors_complete(p.value),
            M::GfCompleteAmbientScriptList(p) => db.set_room_ambient_scripts_complete(p.value),
            M::GfCompleteMissionComputerList(p) => db.set_mission_offers_complete(p.value),
            M::GfCompleteNewsBroadcastList(p) => db.set_news_complete(p.value),
            M::GfUpdateChar(p) => {
                if let Ok(r) = CharacterProperties::unmarshal(&mut MarshalReader::new(&p.data)) {
                    db.upsert_room_character(r);
                }
            }
            M::GfScriptBehavior(p) => {
                if let Ok(r) = ScriptBehavior::unmarshal(&mut MarshalReader::new(&p.data)) {
                    db.upsert_room_script_behavior(r);
                }
            }
            M::GfUpdateAmbientScript(p) => {
                if let Ok(r) = AmbientScriptProperties::unmarshal(&mut MarshalReader::new(&p.data))
                {
                    db.upsert_room_ambient_script(r);
                }
            }
            M::GfUpdateMissionComputer(p) => {
                if let Ok(r) = MissionVendorProperties::unmarshal(&mut MarshalReader::new(&p.data))
                {
                    db.upsert_mission_offer(r);
                }
            }
            M::GfMissionVendorAcceptance(p) => {
                if let Ok(r) = MissionVendorAcceptance::unmarshal(&mut MarshalReader::new(&p.data))
                {
                    db.vendor_acceptance = Some(r);
                }
            }
            M::GfUpdateNewsBroadcast(p) => {
                if let Ok(r) = NewsBroadcastProperties::unmarshal(&mut MarshalReader::new(&p.data))
                {
                    db.upsert_news(r);
                }
            }
            M::GfMissionVendorWhyEmpty(p) => db.vendor_why_empty = i32::from(p.value),
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use game_sim::protocol::marshal::MarshalWriter;
    use game_sim::protocol::to_client::{
        GfCompleteCharList, GfCompleteMissionComputerList, GfDestroyMissionComputer,
        GfMissionVendorWhyEmpty, GfUpdateChar, GfUpdateMissionComputer, SetStartRoom,
    };
    use game_sim::protocol::{ServerMessage, ToClient};

    use super::*;
    use crate::receiver::ClientReceiver;

    fn deliver(r: &mut ClientReceiver, m: ServerMessage) {
        r.deliver(None, 1, ToClient::Server(m));
    }

    #[test]
    fn the_gf_slots_fill_the_opened_mirrors_only() {
        let mut r = ClientReceiver::new(true);
        r.location.register_base(7);
        r.location.register_room(70);
        r.location.register_room(71);

        // Not opened: the base and the room take nothing.
        let ch = CharacterProperties {
            key: 3,
            room_id: 70,
            ..CharacterProperties::default()
        };
        let mut w = MarshalWriter::new();
        ch.marshal(&mut w).unwrap();
        let char_bytes = w.into_bytes();
        deliver(
            &mut r,
            ServerMessage::GfUpdateChar(GfUpdateChar {
                data: char_bytes.clone(),
            }),
        );
        assert!(r.location.room(70).is_none());

        // Opened: the record lands under its key; the room's runtime gets
        // the update.
        r.location.open_base(7);
        r.location.open_room(70);
        r.location.set_room_runtime(70, true);
        deliver(
            &mut r,
            ServerMessage::GfUpdateChar(GfUpdateChar { data: char_bytes }),
        );
        deliver(
            &mut r,
            ServerMessage::GfCompleteCharList(GfCompleteCharList { value: 70 }),
        );
        let room = r.location.room(70).expect("open");
        assert!(room.characters.items.contains_key(&3) && room.characters.complete);
        assert_eq!(r.location.updated_characters, [(70, 3)]);
        assert!(!r.location.room(70).unwrap().script_behaviors.complete);

        // The base: start location, an offer, its erase, the list's end.
        deliver(
            &mut r,
            ServerMessage::SetStartRoom(SetStartRoom {
                field_00: 7,
                field_04: 71,
            }),
        );
        let offer = MissionVendorProperties {
            key: 9,
            base_id: 7,
            ..MissionVendorProperties::default()
        };
        let mut w = MarshalWriter::new();
        offer.marshal(&mut w).unwrap();
        deliver(
            &mut r,
            ServerMessage::GfUpdateMissionComputer(GfUpdateMissionComputer {
                data: w.into_bytes(),
            }),
        );
        assert!(
            r.location
                .base(7)
                .unwrap()
                .mission_offers
                .items
                .contains_key(&9)
        );
        deliver(
            &mut r,
            ServerMessage::GfDestroyMissionComputer(GfDestroyMissionComputer {
                field_00: 7,
                field_04: 9,
            }),
        );
        deliver(
            &mut r,
            ServerMessage::GfCompleteMissionComputerList(GfCompleteMissionComputerList {
                value: 7,
            }),
        );
        let base = r.location.base(7).unwrap();
        assert_eq!(base.start_location, 71);
        assert!(base.mission_offers.items.is_empty() && base.mission_offers.complete);

        // An unknown base is reported and skipped.
        deliver(
            &mut r,
            ServerMessage::GfCompleteMissionComputerList(GfCompleteMissionComputerList {
                value: 8,
            }),
        );
        assert!(!r.location.bases.contains_key(&8));
        assert_eq!(r.location.skipped, 1);
        deliver(
            &mut r,
            ServerMessage::GfMissionVendorWhyEmpty(GfMissionVendorWhyEmpty { value: 2 }),
        );
        assert_eq!(r.location.vendor_why_empty, 2);

        // Every slot was logged in the oracle's form.
        let lines = r.log.take().unwrap().take_lines();
        let slots: Vec<&str> = lines
            .iter()
            .filter_map(|l| l.split("\"m\":\"").nth(1)?.split('"').next())
            .collect();
        assert_eq!(
            slots,
            [
                "SendServerGFUpdateChar",
                "SendServerGFUpdateChar",
                "SendServerGFCompleteCharList",
                "SendServerSetStartRoom",
                "SendServerGFUpdateMissionComputer",
                "SendServerGFDestroyMissionComputer",
                "SendServerGFCompleteMissionComputerList",
                "SendServerGFCompleteMissionComputerList",
                "SendServerGFMissionVendorWhyEmpty",
            ]
        );
    }
}
