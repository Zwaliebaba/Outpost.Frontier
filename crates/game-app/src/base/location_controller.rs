//! The client's location controller (`menu_universe_base.cpp`, work
//! package P5.9): the bases and rooms of the base list with their use
//! counts, the current base and room, entering and leaving them, the
//! queued location change, and what each step asks the server for.
//!
//! The server's answers land in the receiver's mirrors
//! ([`LocationDb`], stream B); the controller opens and closes their
//! entries where the reference makes and frees a base's runtime and a
//! room's state, resets their completion flags when it asks, and waits on
//! them. What a step does to the interface or the player (the navigation
//! bar, the arrival text, the room's runtime and scenes, the market, the
//! visit flags) goes to the [`LocationHost`].
//!
//! A base or room id the maps do not hold is reported and the step skipped,
//! where the reference reads through the null record it finds (divergence
//! D1701).

use std::cmp::Ordering;
use std::collections::BTreeMap;

use game_sim::data::base::{BaseDataList, MarketGood};
use game_sim::data::goods::GoodList;
use game_sim::protocol::to_server::{
    ClientMessage, EnterBase, EnterLocation, ExitBase, ExitLocation, RequestBaseInfo,
    RequestLocationInfo,
};

use game_sim::protocol::{ToClient, ToServer};
use neuron_core::net::transport::Transport;

use super::location::apply_location_audio;
use super::market::{self, ClientMarket, ListGood};
use super::room_data::RoomData;
use neuron_core::diag::{self, DumpFlags};

use crate::audio::SoundCalls;
use crate::login::wait_for_server_response;
use crate::receiver::ClientLink;
use crate::receiver::location::{LOCATION_CONTROLLER_CPP, LocationDb};
use crate::session::Session;

/// An `FDUMP` of the controller's source at `line`.
fn dump(flags: DumpFlags, line: u32, text: &str) {
    diag::dump(
        flags,
        &format!("{LOCATION_CONTROLLER_CPP}({line}) : {text}\n"),
    );
}

/// The low 30 bits of an id, as the reference tests them.
const ID_MASK: u32 = 0x3FFF_FFFF;

/// What the controller asks of the server: the messages, and the waits
/// on the receiver's mirrors.
pub trait LocationServer {
    /// Whether there is a server to talk to (`GetLocalServerInterface`).
    fn has_server(&self) -> bool;
    /// Sends a message to the server.
    fn send(&mut self, message: ClientMessage);
    /// The receiver's mirrors.
    fn db(&mut self) -> &mut LocationDb;
    /// `WaitForServerResponse(flag, 5.0f, 60.0f)`: the server pumped until
    /// `done` holds or the wait ends; whether `done` holds then.
    fn wait(&mut self, done: &dyn Fn(&LocationDb) -> bool) -> bool;
}

/// What the controller's steps do outside it.
pub trait LocationHost {
    /// `RoomData::load` of a room's file.
    fn read_room(&mut self, file: &[u8]) -> RoomData;
    /// The client's sound calls (`ApplyLocationAudio`).
    fn sound(&mut self) -> &mut dyn SoundCalls;
    /// The C runtime's `rand` (the base's news item).
    fn rand(&mut self) -> i32;
    /// After a base's rooms, mirrors and market: the base's solar and its
    /// visit flags, `StoryStarForgetLastObjective`.
    fn base_loaded(&mut self, base: u32);
    /// A synchronisation failed: single player quits, multiplayer is
    /// disconnected.
    fn sync_failed(&mut self);
    /// `CreateRoomRuntime` (true) or `DestroyRoomRuntime` (false).
    fn room_runtime(&mut self, room: u32, create: bool);
    /// `RoomControlPlayScenes`, then `NavBarEnsure`.
    fn room_entered(&mut self, room: u32);
    /// `NavBarDestroy`.
    fn nav_bar_destroy(&mut self);
    /// `NnArrivalTextDestroy`.
    fn arrival_text_destroy(&mut self);
    /// `NotifyInventoryLocationLeft`.
    fn inventory_location_left(&mut self);
    /// A base change's player state: `SetPlayerBaseId`,
    /// `SetPlayerFactionScratch` (with the location) and
    /// `SetGameflowLocationBase`.
    fn base_changed(&mut self, base: u32, location: u32);
    /// `OracleNoteScreen("room", location)` at the start of a change.
    fn screen_room(&mut self, location: u32);
}

/// The controller's server over the client's link: the messages through
/// `ClientLink::send`, the mirrors at `receiver.location`, and the waits
/// through `login::wait_for_server_response` (the session's pump in single
/// player).
pub struct LinkServer<'a, T, S: ?Sized> {
    /// The link.
    pub link: &'a mut ClientLink<T>,
    /// The in-process session, in single player.
    pub session: Option<&'a mut S>,
    /// The waits' clock in seconds.
    pub now: &'a mut dyn FnMut() -> f64,
}

impl<T, S> LocationServer for LinkServer<'_, T, S>
where
    T: Transport<Outgoing = ToServer, Incoming = ToClient>,
    S: Session + ?Sized,
{
    /// The link's server interface while the client is not disconnected.
    fn has_server(&self) -> bool {
        !self.link.receiver.login.server_disconnected
    }
    fn send(&mut self, message: ClientMessage) {
        self.link.send(ToServer::Client(message));
    }
    fn db(&mut self) -> &mut LocationDb {
        &mut self.link.receiver.location
    }
    fn wait(&mut self, done: &dyn Fn(&LocationDb) -> bool) -> bool {
        let session = self.session.as_deref_mut();
        wait_for_server_response(self.link, session, |r| done(&r.location), self.now);
        done(&self.link.receiver.location)
    }
}

/// A base of the maps (`FLocationBaseEntry`).
#[derive(Clone, Debug, Default)]
struct BaseEntry {
    start_room: u32,
    price_variance: f32,
    base_market: BTreeMap<u32, MarketGood>,
    market: ClientMarket,
    rooms: Vec<u32>,
    ref_count: i32,
    loaded: bool,
    news_id: u32,
}

/// A room of the maps (`FLocationRoomEntry`).
#[derive(Clone, Debug, Default)]
struct RoomEntry {
    base: u32,
    file: Vec<u8>,
    ref_count: i32,
    data: Option<RoomData>,
}

/// The location controller.
#[derive(Clone, Debug, Default)]
pub struct LocationController {
    bases: BTreeMap<u32, BaseEntry>,
    goods: Vec<ListGood>,
    rooms: BTreeMap<u32, RoomEntry>,
    /// `+0x34`.
    pub current_base: u32,
    /// `+0x38`.
    pub current_room: u32,
    pending_room: u32,
    pending_audio: bool,
    pending_elapsed: f32,
    pending_delay: f32,
    /// `+0x59`.
    pub in_transition: bool,
    /// `+0x58`: the next room's audio keeps a non-looping track.
    pub keep_audio: bool,
    /// `+0x5a`: no server calls and no waits (`SetClientLocationNoServerNotify`).
    pub no_server_notify: bool,
}

impl LocationController {
    /// No bases.
    #[must_use]
    pub fn new() -> LocationController {
        LocationController::default()
    }

    /// `BuildClientLocationMaps`: every base of the list and every room of
    /// each, in list order, into the maps and the receiver's mirrors (an
    /// existing key is kept).
    pub fn build_maps(&mut self, list: &BaseDataList, goods: &GoodList, db: &mut LocationDb) {
        self.goods = goods
            .goods()
            .iter()
            .map(|g| ListGood {
                id: g.id,
                price: g.price,
            })
            .collect();
        for base in &list.bases {
            let rooms: Vec<u32> = base.rooms.iter().flatten().map(|r| r.room_id).collect();
            self.bases.entry(base.base_id).or_insert(BaseEntry {
                start_room: base.start_room,
                price_variance: base.price_variance,
                base_market: base.market.clone(),
                market: ClientMarket::new(),
                rooms,
                ref_count: 0,
                loaded: false,
                news_id: u32::MAX,
            });
            db.register_base(base.base_id);
            for room in base.rooms.iter().flatten() {
                self.rooms.entry(room.room_id).or_insert(RoomEntry {
                    base: room.base_id,
                    file: room.file.clone(),
                    ref_count: 0,
                    data: None,
                });
                db.register_room(room.room_id);
            }
        }
    }

    /// `ResetClientLocationController`.
    pub fn reset(&mut self) {
        self.current_base = 0;
        self.current_room = 0;
        self.pending_room = 0;
        self.pending_audio = false;
        self.pending_elapsed = 0.0;
        self.pending_delay = 0.0;
        self.in_transition = false;
    }

    /// The room data of a loaded room (`LookupClientLocationRoom` once its
    /// base is loaded).
    #[must_use]
    pub fn room(&self, location: u32) -> Option<&RoomData> {
        self.rooms.get(&location).and_then(|r| r.data.as_ref())
    }

    /// A loaded base's market (`GetClientLocationBaseMarket`).
    #[must_use]
    pub fn market(&self, base: u32) -> Option<&ClientMarket> {
        self.bases
            .get(&base)
            .filter(|b| b.loaded)
            .map(|b| &b.market)
    }

    /// A loaded base's news item (`+0x44`), `u32::MAX` for none.
    #[must_use]
    pub fn news_id(&self, base: u32) -> Option<u32> {
        self.bases
            .get(&base)
            .filter(|b| b.loaded)
            .map(|b| b.news_id)
    }

    /// The use counts of a base and a room, for the tests.
    #[must_use]
    pub fn ref_counts(&self, base: u32, room: u32) -> (i32, i32) {
        (
            self.bases.get(&base).map_or(0, |b| b.ref_count),
            self.rooms.get(&room).map_or(0, |r| r.ref_count),
        )
    }

    fn server_on(&self, server: &dyn LocationServer) -> bool {
        !self.no_server_notify && server.has_server()
    }

    /// `FindLocationBaseEntry`'s miss (`%d` of the id).
    fn missing_base(base: u32) {
        let id = base.cast_signed();
        dump(
            DumpFlags::ERROR,
            0x100,
            &format!("*** ERROR: Could not find base id {id}"),
        );
    }

    /// `FindLocationRoomEntry`'s miss.
    fn missing_room(room: u32) {
        let id = room.cast_signed();
        dump(
            DumpFlags::ERROR,
            0x110,
            &format!("*** ERROR: Could not find location id {id}"),
        );
    }

    /// The flag waits of a sync: each in turn, the first not set fails the
    /// sync (quit or disconnect) and ends it.
    fn sync(
        server: &mut dyn LocationServer,
        host: &mut dyn LocationHost,
        flags: &[&dyn Fn(&LocationDb) -> bool],
    ) -> bool {
        for done in flags {
            if !server.wait(*done) {
                dump(
                    DumpFlags::WARNING,
                    0x108,
                    "*** WARNING: Synchronization failed",
                );
                host.sync_failed();
                return false;
            }
        }
        true
    }

    /// `LoadLocationBase`: the base's runtime opened, every room read, and
    /// with the server the base info asked for, its mission offers and news
    /// waited on, the news item picked, and the rest of the load.
    fn load_base(
        &mut self,
        base: u32,
        server: &mut dyn LocationServer,
        host: &mut dyn LocationHost,
    ) {
        let Some(entry) = self.bases.get_mut(&base) else {
            Self::missing_base(base);
            return;
        };
        entry.loaded = true;
        entry.news_id = u32::MAX;
        server.db().open_base(base);
        for room in entry.rooms.clone() {
            if let Some(r) = self.rooms.get_mut(&room) {
                r.data = Some(host.read_room(&r.file));
            }
        }
        if self.no_server_notify {
            return;
        }
        if let Some(b) = server.db().base_mut(base) {
            b.mission_offers.reset();
            b.news.reset();
        }
        if server.has_server() {
            server.send(ClientMessage::RequestBaseInfo(RequestBaseInfo {
                id: base,
                full: 1,
            }));
        }
        let offers =
            move |db: &LocationDb| db.base(base).is_some_and(|b| b.mission_offers.complete);
        let news = move |db: &LocationDb| db.base(base).is_some_and(|b| b.news.complete);
        if !Self::sync(server, host, &[&offers, &news]) {
            return;
        }
        let keys: Vec<u32> = server
            .db()
            .base(base)
            .map(|b| b.news.items.keys().copied().collect())
            .unwrap_or_default();
        let news_id = if keys.is_empty() {
            u32::MAX
        } else {
            // `(rand() * count) / 32768`, signed, over the bucket in key
            // order (the mirror's traversal).
            let pick = (host.rand() * i32::try_from(keys.len()).unwrap_or(i32::MAX)) / 32768;
            keys[usize::try_from(pick).unwrap_or(0).min(keys.len() - 1)]
        };
        // `ClientMarketFill(&runtime->Market, PriceVariance, nullptr)`: the
        // current base's market, which is the base being loaded.
        let goods = &self.goods;
        if let Some(entry) = self.bases.get_mut(&base) {
            entry.news_id = news_id;
            let BaseEntry {
                market,
                price_variance,
                base_market,
                ..
            } = entry;
            market::fill(market, *price_variance, base_market, goods, &mut || {
                host.rand()
            });
        }
        host.base_loaded(base);
    }

    /// `UnloadLocationBase`: the base info released, every room unloaded,
    /// the runtime closed.
    fn unload_base(&mut self, base: u32, server: &mut dyn LocationServer) {
        let on = self.server_on(server);
        let Some(entry) = self.bases.get_mut(&base) else {
            return;
        };
        if on {
            server.send(ClientMessage::RequestBaseInfo(RequestBaseInfo {
                id: base,
                full: 0,
            }));
        }
        entry.loaded = false;
        for room in entry.rooms.clone() {
            if let Some(r) = self.rooms.get_mut(&room) {
                r.data = None;
            }
        }
        server.db().close_base(base);
    }

    /// The base's use count up, loading it at zero.
    fn acquire_base(
        &mut self,
        base: u32,
        server: &mut dyn LocationServer,
        host: &mut dyn LocationHost,
    ) {
        let Some(entry) = self.bases.get(&base) else {
            Self::missing_base(base);
            return;
        };
        if entry.ref_count == 0 {
            self.load_base(base, server, host);
        }
        if let Some(entry) = self.bases.get_mut(&base) {
            entry.ref_count += 1;
        }
    }

    /// `EnsureRoomRuntime`: the room's state opened, the location info asked
    /// for, the runtime made, and with the server the three mirrors waited
    /// on; then the scenes and the navigation bar.
    fn ensure_room(
        &mut self,
        room: u32,
        server: &mut dyn LocationServer,
        host: &mut dyn LocationHost,
    ) {
        server.db().open_room(room);
        if self.server_on(server) {
            server.send(ClientMessage::RequestLocationInfo(RequestLocationInfo {
                id: room,
                full: 1,
            }));
        }
        host.room_runtime(room, true);
        server.db().set_room_runtime(room, true);
        if !self.no_server_notify {
            let characters =
                move |db: &LocationDb| db.room(room).is_some_and(|r| r.characters.complete);
            let behaviors =
                move |db: &LocationDb| db.room(room).is_some_and(|r| r.script_behaviors.complete);
            let ambient =
                move |db: &LocationDb| db.room(room).is_some_and(|r| r.ambient_scripts.complete);
            if !Self::sync(server, host, &[&characters, &behaviors, &ambient]) {
                return;
            }
        }
        host.room_entered(room);
    }

    /// The room's state freed (`UnloadClientLocationRoomState`, the tail of
    /// `LeaveClientLocationRoom` and `ReleaseClientLocationRoom`).
    fn free_room(
        &mut self,
        room: u32,
        server: &mut dyn LocationServer,
        host: &mut dyn LocationHost,
    ) {
        if self.server_on(server) {
            server.send(ClientMessage::RequestLocationInfo(RequestLocationInfo {
                id: room,
                full: 0,
            }));
        }
        host.room_runtime(room, false);
        server.db().set_room_runtime(room, false);
        server.db().close_room(room);
    }

    /// The room's use count down, freed at zero.
    fn release_room(
        &mut self,
        room: u32,
        server: &mut dyn LocationServer,
        host: &mut dyn LocationHost,
    ) {
        let Some(entry) = self.rooms.get_mut(&room) else {
            Self::missing_room(room);
            return;
        };
        entry.ref_count -= 1;
        if entry.ref_count == 0 {
            self.free_room(room, server, host);
        }
    }

    /// `LocationBaseOf`: the base a location belongs to, 0 for none.
    fn base_of(&self, location: u32) -> u32 {
        if location == 0 {
            return 0;
        }
        match self.rooms.get(&location) {
            Some(r) if self.bases.contains_key(&r.base) => r.base,
            Some(_) => 0,
            None => {
                let id = location.cast_signed();
                dump(
                    DumpFlags::ERROR,
                    0x120,
                    &format!("*** ERROR: Could not find location {id}"),
                );
                0
            }
        }
    }

    /// The current location left: the arrival text, the room released, the
    /// server told, the inventory told.
    fn leave_current_room(&mut self, server: &mut dyn LocationServer, host: &mut dyn LocationHost) {
        let current = self.current_room;
        host.arrival_text_destroy();
        self.release_room(current, server, host);
        if self.server_on(server) {
            server.send(ClientMessage::ExitLocation(ExitLocation { value: current }));
        }
        self.current_room = 0;
        host.inventory_location_left();
    }

    /// `EnterClientLocationBase`: the base entered and acquired, then its
    /// start location (the server's, or the base's own with a warning) gets
    /// its audio and becomes the location.
    pub fn enter_base(
        &mut self,
        base: u32,
        server: &mut dyn LocationServer,
        host: &mut dyn LocationHost,
    ) {
        self.current_base = base;
        if self.server_on(server) {
            server.send(ClientMessage::EnterBase(EnterBase { value: base }));
        }
        self.acquire_base(base, server, host);
        let Some(entry) = self.bases.get(&base) else {
            return;
        };
        let mut start = server.db().base(base).map_or(0, |b| b.start_location);
        if start == 0 {
            if server.has_server() {
                dump(
                    DumpFlags::WARNING,
                    0x405,
                    "*** WARNING: Failed to get start location",
                );
            }
            start = entry.start_room;
        }
        self.apply_audio(start, host);
        self.change_location(start, server, host);
    }

    /// `SetClientLocationBase`: the base half of [`Self::enter_base`].
    pub fn set_base(
        &mut self,
        base: u32,
        server: &mut dyn LocationServer,
        host: &mut dyn LocationHost,
    ) {
        self.current_base = base;
        if self.server_on(server) {
            server.send(ClientMessage::EnterBase(EnterBase { value: base }));
        }
        self.acquire_base(base, server, host);
    }

    /// `ApplyLocationAudio` on a location's room.
    pub fn apply_audio(&mut self, location: u32, host: &mut dyn LocationHost) {
        let Some(room) = self.rooms.get(&location).and_then(|r| r.data.clone()) else {
            Self::missing_room(location);
            return;
        };
        let mut keep = self.keep_audio;
        apply_location_audio(host.sound(), &room, &mut keep);
        self.keep_audio = keep;
    }

    /// `ChangeClientLocation`: a base change (the old room left, the old
    /// base released and exited, the navigation bar destroyed, the new base
    /// entered and acquired, the player's base set) or, within the base,
    /// the current location left; then the new location entered and its
    /// room acquired.
    pub fn change_location(
        &mut self,
        location: u32,
        server: &mut dyn LocationServer,
        host: &mut dyn LocationHost,
    ) {
        host.screen_room(location);
        self.in_transition = true;
        let old_base = self.base_of(self.current_room);
        let new_base = self.base_of(location);
        if old_base != 0 && new_base != 0 && old_base != new_base {
            if self.current_room & ID_MASK != 0 {
                self.leave_current_room(server, host);
            }
            if let Some(entry) = self.bases.get_mut(&old_base) {
                entry.ref_count -= 1;
                if entry.ref_count == 0 {
                    self.unload_base(old_base, server);
                }
            }
            if self.server_on(server) {
                server.send(ClientMessage::ExitBase(ExitBase { value: old_base }));
            }
            self.current_base = 0;
            host.nav_bar_destroy();
            self.current_base = new_base;
            if self.server_on(server) {
                server.send(ClientMessage::EnterBase(EnterBase { value: new_base }));
            }
            self.acquire_base(new_base, server, host);
            host.base_changed(new_base, location);
        } else if self.current_room & ID_MASK != 0 {
            let current = self.current_room;
            host.arrival_text_destroy();
            self.release_room(current, server, host);
            if self.server_on(server) {
                server.send(ClientMessage::ExitLocation(ExitLocation { value: current }));
            }
            self.current_room = 0;
            host.inventory_location_left();
        }
        self.current_room = location;
        self.pending_elapsed = 0.0;
        self.pending_delay = 0.0;
        if self.server_on(server) {
            server.send(ClientMessage::EnterLocation(EnterLocation {
                value: location,
            }));
        }
        match self.rooms.get(&location).map(|r| r.ref_count) {
            None => Self::missing_room(location),
            Some(count) => {
                if count == 0 {
                    self.ensure_room(location, server, host);
                }
                if let Some(r) = self.rooms.get_mut(&location) {
                    r.ref_count += 1;
                }
            }
        }
        self.in_transition = false;
    }

    /// `LeaveClientLocationBase`: the current location left, the base
    /// released and exited, the navigation bar destroyed.
    pub fn leave_base(&mut self, server: &mut dyn LocationServer, host: &mut dyn LocationHost) {
        let base = self.current_base;
        if base & ID_MASK == 0 {
            return;
        }
        if self.current_room & ID_MASK != 0 {
            self.leave_current_room(server, host);
        }
        match self.bases.get_mut(&base) {
            None => Self::missing_base(base),
            Some(entry) => {
                entry.ref_count -= 1;
                if entry.ref_count == 0 {
                    self.unload_base(base, server);
                }
            }
        }
        if self.server_on(server) {
            server.send(ClientMessage::ExitBase(ExitBase { value: base }));
        }
        self.current_base = 0;
        host.nav_bar_destroy();
    }

    /// `QueueClientLocationChange`.
    pub fn queue_change(&mut self, location: u32, delay: f32, with_audio: bool) {
        self.pending_room = location;
        self.pending_audio = with_audio;
        self.pending_elapsed = 0.0;
        self.pending_delay = delay;
    }

    /// `TickClientLocationController`: a queued change runs once its delay
    /// has passed (a NaN on either side keeps it waiting), with its audio
    /// when asked.
    pub fn tick(&mut self, dt: f32, server: &mut dyn LocationServer, host: &mut dyn LocationHost) {
        let pending = self.pending_room;
        if pending & ID_MASK == 0 {
            return;
        }
        self.pending_elapsed = (f64::from(self.pending_elapsed) + f64::from(dt)) as f32;
        // Only "at or past" falls through the x87 test.
        let ordering = self.pending_elapsed.partial_cmp(&self.pending_delay);
        if !matches!(ordering, Some(Ordering::Greater | Ordering::Equal)) {
            return;
        }
        self.change_location(pending, server, host);
        if self.pending_audio {
            self.apply_audio(self.pending_room, host);
        }
        self.pending_room = 0;
        self.pending_audio = false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::Silent;
    use game_sim::data::base::{BaseData, Room};

    /// A server that answers each full info request by completing its
    /// buckets, and records what it was sent.
    #[derive(Default)]
    struct Server {
        db: LocationDb,
        sent: Vec<String>,
        answer: bool,
    }

    impl LocationServer for Server {
        fn has_server(&self) -> bool {
            true
        }
        fn send(&mut self, message: ClientMessage) {
            if self.answer {
                match &message {
                    ClientMessage::RequestBaseInfo(r) if r.full == 1 => {
                        self.db.set_mission_offers_complete(r.id);
                        self.db.set_news_complete(r.id);
                    }
                    ClientMessage::RequestLocationInfo(r) if r.full == 1 => {
                        self.db.set_room_characters_complete(r.id);
                        self.db.set_room_script_behaviors_complete(r.id);
                        self.db.set_room_ambient_scripts_complete(r.id);
                    }
                    _ => {}
                }
            }
            let text = format!("{message:?}");
            self.sent
                .push(text.split('(').next().unwrap_or("").to_owned());
        }
        fn db(&mut self) -> &mut LocationDb {
            &mut self.db
        }
        fn wait(&mut self, done: &dyn Fn(&LocationDb) -> bool) -> bool {
            done(&self.db)
        }
    }

    /// Records the host's calls.
    struct Host {
        events: Vec<String>,
        sound: Silent,
    }

    impl LocationHost for Host {
        fn read_room(&mut self, file: &[u8]) -> RoomData {
            self.events
                .push(format!("read {}", String::from_utf8_lossy(file)));
            RoomData::default()
        }
        fn sound(&mut self) -> &mut dyn SoundCalls {
            &mut self.sound
        }
        fn rand(&mut self) -> i32 {
            0
        }
        fn base_loaded(&mut self, base: u32) {
            self.events.push(format!("loaded {base}"));
        }
        fn sync_failed(&mut self) {
            self.events.push("sync failed".to_owned());
        }
        fn room_runtime(&mut self, room: u32, create: bool) {
            self.events.push(format!("runtime {room} {create}"));
        }
        fn room_entered(&mut self, room: u32) {
            self.events.push(format!("entered {room}"));
        }
        fn nav_bar_destroy(&mut self) {
            self.events.push("navbar destroy".to_owned());
        }
        fn arrival_text_destroy(&mut self) {}
        fn inventory_location_left(&mut self) {}
        fn base_changed(&mut self, base: u32, location: u32) {
            self.events.push(format!("base changed {base} {location}"));
        }
        fn screen_room(&mut self, _: u32) {}
    }

    fn list() -> BaseDataList {
        let mut list = BaseDataList::default();
        for (base, rooms) in [(10, [11, 12]), (20, [21, 22])] {
            list.bases.push(BaseData {
                base_id: base,
                start_room: rooms[0],
                price_variance: 0.0,
                ship_repair_cost: 0.33,
                market: BTreeMap::new(),
                rooms: rooms
                    .iter()
                    .map(|&r| {
                        Some(Room {
                            base_id: base,
                            room_id: r,
                            nickname: format!("r{r}").into_bytes(),
                            file: format!("f{r}").into_bytes(),
                        })
                    })
                    .collect(),
            });
        }
        list
    }

    fn setup(answer: bool) -> (LocationController, Server, Host) {
        let mut c = LocationController::new();
        let mut server = Server {
            answer,
            ..Server::default()
        };
        c.build_maps(&list(), &GoodList::default(), &mut server.db);
        let host = Host {
            events: Vec::new(),
            sound: Silent::default(),
        };
        (c, server, host)
    }

    #[test]
    fn entering_a_base_loads_it_and_enters_its_start_room() {
        let (mut c, mut server, mut host) = setup(true);
        c.enter_base(10, &mut server, &mut host);
        assert_eq!(
            server.sent,
            [
                "EnterBase",
                "RequestBaseInfo",
                "EnterLocation",
                "RequestLocationInfo"
            ]
        );
        assert_eq!(
            host.events,
            [
                "read f11",
                "read f12",
                "loaded 10",
                "runtime 11 true",
                "entered 11"
            ]
        );
        assert_eq!((c.current_base, c.current_room), (10, 11));
        assert_eq!(c.ref_counts(10, 11), (1, 1));
        assert_eq!(c.news_id(10), Some(u32::MAX), "no news");
        assert!(c.room(12).is_some(), "every room of the base read");

        // Within the base: the room left, the new one entered.
        server.sent.clear();
        host.events.clear();
        c.change_location(12, &mut server, &mut host);
        assert_eq!(
            server.sent,
            [
                "RequestLocationInfo",
                "ExitLocation",
                "EnterLocation",
                "RequestLocationInfo"
            ]
        );
        assert_eq!(c.ref_counts(10, 11), (1, 0));
        assert!(server.db.room(11).is_none(), "the old room's state closed");

        // Another base: the old one released and exited.
        server.sent.clear();
        host.events.clear();
        c.change_location(21, &mut server, &mut host);
        assert_eq!(
            server.sent,
            [
                "RequestLocationInfo",
                "ExitLocation",
                "RequestBaseInfo",
                "ExitBase",
                "EnterBase",
                "RequestBaseInfo",
                "EnterLocation",
                "RequestLocationInfo"
            ]
        );
        assert!(host.events.contains(&"navbar destroy".to_owned()));
        assert!(host.events.contains(&"base changed 20 21".to_owned()));
        assert_eq!(c.ref_counts(10, 12), (0, 0));
        assert!(c.room(11).is_none(), "the old base's rooms unloaded");

        // Leaving the base.
        server.sent.clear();
        c.leave_base(&mut server, &mut host);
        assert_eq!(
            server.sent,
            [
                "RequestLocationInfo",
                "ExitLocation",
                "RequestBaseInfo",
                "ExitBase"
            ]
        );
        assert_eq!((c.current_base, c.current_room), (0, 0));
    }

    #[test]
    fn an_unanswered_sync_fails_and_stops() {
        let (mut c, mut server, mut host) = setup(false);
        c.set_base(10, &mut server, &mut host);
        assert_eq!(server.sent, ["EnterBase", "RequestBaseInfo"]);
        assert_eq!(host.events.last().map(String::as_str), Some("sync failed"));
        assert!(!host.events.contains(&"loaded 10".to_owned()));
    }

    #[test]
    fn a_queued_change_waits_for_its_delay() {
        let (mut c, mut server, mut host) = setup(true);
        c.set_base(10, &mut server, &mut host);
        c.queue_change(11, 1.0, false);
        c.tick(0.5, &mut server, &mut host);
        assert_eq!(c.current_room, 0);
        c.tick(0.5, &mut server, &mut host);
        assert_eq!(c.current_room, 11);
        // A NaN delay keeps waiting.
        c.queue_change(12, f32::NAN, false);
        c.tick(10.0, &mut server, &mut host);
        assert_eq!(c.current_room, 11);
    }

    #[test]
    fn no_server_notify_sends_nothing_and_waits_for_nothing() {
        let (mut c, mut server, mut host) = setup(false);
        c.no_server_notify = true;
        c.enter_base(10, &mut server, &mut host);
        assert!(server.sent.is_empty());
        assert_eq!(c.current_room, 11);
        assert!(host.events.contains(&"entered 11".to_owned()));
        assert!(!host.events.contains(&"loaded 10".to_owned()));
    }
}
