//! The base database (work package P3.3): `BaseDB::find_base` and
//! `find_location` (`Server/server_basedb.cpp:867`) over the world's base
//! data, and the market functions of `pub::Market`
//! (`Server/server_pub.cpp:7721`).
//!
//! `BaseDB::init` makes one entry per base of `BaseDataList`, keyed by id
//! with the first base of an id kept, as `BaseDataList` keys them, so a
//! lookup of the entry is a lookup of the data. The run-time parts of the
//! entries (sessions, room records, mission vendors, news) arrive with the
//! rest of the base database.

use game_sim::data::base::{BaseData, Room, TransactionType};
use game_sim::data::goods::GoodCategory;

use crate::server::Cx;

const BASE_DB: &str = "Server\\BaseDB.cpp";

/// `pub::` result: a bad id or a missing base (-4).
pub const BAD_ID: i32 = -4;

/// `BaseDB::find_base`: the base's data, or `None` with the error the C++
/// reports.
pub fn find_base<'w>(cx: &'w mut Cx<'_>, id: u32) -> Option<&'w BaseData> {
    if cx.world.data().bases.base(id).is_none() {
        cx.logic
            .reports
            .push(BASE_DB, 0x659, format!("*** ERROR: Base {id} is invalid"));
        return None;
    }
    cx.world.data().bases.base(id)
}

/// `BaseDB::find_location`.
pub fn find_location<'w>(cx: &'w mut Cx<'_>, id: u32) -> Option<&'w Room> {
    if cx.world.data().bases.room(id).is_none() {
        cx.logic.reports.push(
            BASE_DB,
            0x66d,
            format!("*** ERROR: Location {id} is invalid"),
        );
        return None;
    }
    cx.world.data().bases.room(id)
}

fn has_id(id: u32) -> bool {
    id & 0x3FFF_FFFF != 0
}

/// `pub::Market::GetMinInventory`: the market's minimum, 0 for a good the
/// base does not trade.
///
/// # Errors
///
/// [`BAD_ID`] for an empty id or an unknown base.
pub fn min_inventory(cx: &mut Cx<'_>, base: u32, good: u32) -> Result<i32, i32> {
    if !has_id(base) || !has_id(good) {
        return Err(BAD_ID);
    }
    let data = find_base(cx, base).ok_or(BAD_ID)?;
    Ok(data.market.get(&good).map_or(0, |m| m.min_inventory))
}

/// `pub::Market::GetPrice`: the market's price, else the good's own. The
/// C++ reads the price of a good the goods list lacks through null; the
/// port answers [`BAD_ID`] there (D609).
///
/// # Errors
///
/// [`BAD_ID`] for an empty id, an unknown base or an unknown good.
pub fn price(cx: &mut Cx<'_>, base: u32, good: u32) -> Result<f32, i32> {
    if !has_id(base) || !has_id(good) {
        return Err(BAD_ID);
    }
    let data = find_base(cx, base).ok_or(BAD_ID)?;
    if let Some(entry) = data.market.get(&good) {
        return Ok(entry.price);
    }
    cx.world
        .data()
        .goods
        .find_by_id(good)
        .map(|g| g.price)
        .ok_or(BAD_ID)
}

/// `pub::Market::GetNominalPrice`.
///
/// # Errors
///
/// [`BAD_ID`] for an empty id or an unknown good.
pub fn nominal_price(cx: &Cx<'_>, good: u32) -> Result<f32, i32> {
    if !has_id(good) {
        return Err(BAD_ID);
    }
    cx.world
        .data()
        .goods
        .find_by_id(good)
        .map(|g| g.price)
        .ok_or(BAD_ID)
}

/// `pub::Market::GetGoodJumpDist`.
///
/// # Errors
///
/// [`BAD_ID`] for an unknown good.
pub fn good_jump_dist(cx: &Cx<'_>, good: u32) -> Result<u32, i32> {
    cx.world
        .data()
        .goods
        .find_by_id(good)
        .map(|g| g.jump_dist as u32)
        .ok_or(BAD_ID)
}

/// `pub::Market::IsGoodInDemand`: the base buys the good.
///
/// # Errors
///
/// [`BAD_ID`] for an empty id or an unknown base (the answer is then
/// `false`, as the C++ leaves it).
pub fn is_good_in_demand(cx: &mut Cx<'_>, base: u32, good: u32) -> Result<bool, i32> {
    if !has_id(base) || !has_id(good) {
        return Err(BAD_ID);
    }
    let data = find_base(cx, base).ok_or(BAD_ID)?;
    Ok(data.market.get(&good).is_some_and(|m| {
        m.transaction == TransactionType::BUY_AND_SELL
            || m.transaction == TransactionType::DEMAND_ONLY
    }))
}

/// The commodities a base sells (`for_sale`) or buys, in good id order
/// (`MarketGoodForSale`, `MarketGoodInDemand`).
fn commodities(cx: &mut Cx<'_>, base: u32, for_sale: bool) -> Result<Vec<u32>, i32> {
    if !has_id(base) {
        return Err(BAD_ID);
    }
    let goods = cx.world.data_arc();
    let data = find_base(cx, base).ok_or(BAD_ID)?;
    let wanted = if for_sale {
        TransactionType::SALE_ONLY
    } else {
        TransactionType::DEMAND_ONLY
    };
    Ok(data
        .market
        .values()
        .filter(|m| {
            goods
                .goods
                .find_by_id(m.good_id)
                .is_some_and(|g| g.category == GoodCategory::Commodity)
                && (m.transaction == TransactionType::BUY_AND_SELL || m.transaction == wanted)
        })
        .map(|m| m.good_id)
        .collect())
}

/// `pub::Market::GetCommoditiesForSale` (and `GetNumCommoditiesForSale`,
/// its length): at most `max` of the commodities the base sells.
///
/// # Errors
///
/// [`BAD_ID`] for an empty id or an unknown base.
pub fn commodities_for_sale(cx: &mut Cx<'_>, base: u32, max: usize) -> Result<Vec<u32>, i32> {
    let mut list = commodities(cx, base, true)?;
    list.truncate(max);
    Ok(list)
}

/// `pub::Market::GetCommoditiesInDemand` (and `GetNumCommoditiesInDemand`).
///
/// # Errors
///
/// [`BAD_ID`] for an empty id or an unknown base.
pub fn commodities_in_demand(cx: &mut Cx<'_>, base: u32, max: usize) -> Result<Vec<u32>, i32> {
    let mut list = commodities(cx, base, false)?;
    list.truncate(max);
    Ok(list)
}

// ---------------------------------------------------------------------------
// The run-time base database (`BaseDB`, `FBaseEntry`, `FRoomEntry`,
// `Server/server_basedb.cpp`): who is in each base and room, and the
// records the server keeps there for them (characters, behaviours,
// ambient scripts, mission vendor offers, news), each sent to the
// sessions its filter lets through when it is made, changed or removed.
// ---------------------------------------------------------------------------

use std::collections::BTreeMap;

use game_sim::ai::script::ScriptBehavior;
use game_sim::data::DataSource;
use game_sim::data::room::{RoomFile, read_room_file};
use game_sim::protocol::ToClient;
use game_sim::protocol::base_props::{
    AmbientScriptProperties, CharacterProperties, MissionVendorAcceptance, MissionVendorProperties,
    NewsBroadcastProperties,
};
use game_sim::protocol::costume::Costume as WireCostume;
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::marshal::MarshalWriter;
use game_sim::protocol::to_client::{
    GfCompleteAmbientScriptList, GfCompleteCharList, GfCompleteMissionComputerList,
    GfCompleteNewsBroadcastList, GfCompleteScriptBehaviorList, GfDestroyAmbientScript,
    GfDestroyCharacter, GfDestroyMissionComputer, GfDestroyNewsBroadcast, GfDestroyScriptBehavior,
    GfMissionVendorAcceptance, GfScriptBehavior, GfUpdateAmbientScript, GfUpdateChar,
    GfUpdateMissionComputer, GfUpdateNewsBroadcast, ServerMessage, SetStartRoom,
};
use neuron_core::hash::stricmp;
use neuron_core::id::create_id;

/// A pool of 4096 record handles (`FHandlePool`, a `std::bitset<4096>`
/// with the last position given).
#[derive(Clone, Debug)]
pub struct HandlePool {
    bits: [u32; 128],
    last: u32,
}

impl Default for HandlePool {
    fn default() -> Self {
        Self {
            bits: [0; 128],
            last: 0,
        }
    }
}

impl HandlePool {
    const SIZE: u32 = 0x1000;

    fn test(&self, at: u32) -> bool {
        self.bits[(at >> 5) as usize] & (1 << (at & 0x1F)) != 0
    }

    /// `FHandlePool::set`. The C++ throws `out_of_range` above 4095; the
    /// port ignores such a position (D611).
    pub fn set(&mut self, at: u32, value: bool) {
        if at >= Self::SIZE {
            return;
        }
        if value {
            self.bits[(at >> 5) as usize] |= 1 << (at & 0x1F);
        } else {
            self.bits[(at >> 5) as usize] &= !(1 << (at & 0x1F));
        }
    }

    /// The walk of `allocate` and `allocate_index`: the first free position
    /// after the last one given, wrapping at 4096; `None` once the walk
    /// comes back to it (or, where the C++ walks for ever, after one full
    /// lap, D611).
    fn walk(&mut self) -> Option<u32> {
        let mut searching = true;
        let mut at = self.last + 1;
        if at >= Self::SIZE {
            at = 0;
        }
        for _ in 0..=Self::SIZE {
            if !self.test(at) {
                if !searching {
                    return None;
                }
                self.set(at, true);
                self.last = at;
                return Some(at);
            }
            if !searching {
                return None;
            }
            at += 1;
            if at >= Self::SIZE {
                at = 0;
            } else if at == self.last {
                searching = false;
            }
        }
        None
    }

    /// `allocate`: the position plus one, or -1.
    pub fn allocate(&mut self) -> i32 {
        self.walk().map_or(-1, |at| at as i32 + 1)
    }

    /// `allocate_index`: the position, or `0xffffffff`.
    pub fn allocate_index(&mut self) -> u32 {
        self.walk().unwrap_or(u32::MAX)
    }
}

/// `BroadcastRecord::session_passes`.
#[must_use]
pub fn session_passes(session: u32, filter: u32) -> bool {
    session == 0 || filter == 0 || filter == session
}

/// A record with its session filter and its changed flag.
#[derive(Clone, Debug, PartialEq)]
pub struct Record<P> {
    /// The record.
    pub props: P,
    /// `Filter`: 0 for every session, else the one session that sees it.
    pub filter: u32,
    /// `bDirty`: sent again at the next update.
    pub dirty: bool,
}

/// A room of a base (`FRoomEntry`).
#[derive(Clone, Debug, Default)]
pub struct RoomEntry {
    /// The base.
    pub base_id: u32,
    /// The room.
    pub room_id: u32,
    /// The room's file (`RoomData`), once a player's base load read it.
    pub file: Option<RoomFile>,
    /// The sessions in the room, in arrival order.
    pub sessions: Vec<u32>,
    /// The characters.
    pub characters: Vec<Record<CharacterProperties>>,
    /// The scripted behaviours.
    pub behaviors: Vec<Record<ScriptBehavior>>,
    /// The ambient scripts.
    pub ambient_scripts: Vec<Record<AmbientScriptProperties>>,
}

/// A base (`FBaseEntry`).
#[derive(Clone, Debug, Default)]
pub struct BaseEntry {
    /// The base.
    pub base_id: u32,
    /// `StartRoomId` of its data.
    pub start_room: u32,
    /// Its rooms, in the data's order.
    pub rooms: Vec<RoomEntry>,
    /// The sessions with the base's information, in arrival order.
    pub sessions: Vec<u32>,
    /// The mission vendor offers.
    pub mission_vendors: Vec<Record<MissionVendorProperties>>,
    /// The news.
    pub news: Vec<Record<NewsBroadcastProperties>>,
}

/// The base database (`BaseDB`).
#[derive(Clone, Debug, Default)]
pub struct BaseDb {
    /// The bases, in the base list's order.
    pub bases: Vec<BaseEntry>,
    base_map: BTreeMap<u32, usize>,
    location_map: BTreeMap<u32, (usize, usize)>,
    character_map: BTreeMap<u32, (usize, usize)>,
    behavior_map: BTreeMap<u32, (usize, usize)>,
    ambient_map: BTreeMap<u32, (usize, usize)>,
    vendor_map: BTreeMap<u32, usize>,
    news_map: BTreeMap<u32, usize>,
    character_handles: HandlePool,
    behavior_handles: HandlePool,
    ambient_handles: HandlePool,
    vendor_handles: HandlePool,
    news_handles: HandlePool,
}

fn add_session(list: &mut Vec<u32>, session: u32) {
    if !list.contains(&session) {
        list.push(session);
    }
}

fn remove_session(list: &mut Vec<u32>, session: u32) {
    if let Some(at) = list.iter().position(|&s| s == session) {
        list.remove(at);
    }
}

impl BaseDb {
    /// `BaseDB::init`: one entry per base of the list, its rooms in order;
    /// the first base or room of an id is the one found.
    #[must_use]
    pub fn new(list: &game_sim::data::base::BaseDataList) -> Self {
        let mut db = Self::default();
        for (bi, data) in list.bases.iter().enumerate() {
            let rooms = data
                .rooms
                .iter()
                .flatten()
                .map(|r| RoomEntry {
                    base_id: r.base_id,
                    room_id: r.room_id,
                    ..RoomEntry::default()
                })
                .collect::<Vec<_>>();
            for (ri, room) in rooms.iter().enumerate() {
                db.location_map.entry(room.room_id).or_insert((bi, ri));
            }
            db.base_map.entry(data.base_id).or_insert(bi);
            db.bases.push(BaseEntry {
                base_id: data.base_id,
                start_room: data.start_room,
                rooms,
                ..BaseEntry::default()
            });
        }
        db
    }

    /// The entry of a base.
    #[must_use]
    pub fn base(&self, id: u32) -> Option<&BaseEntry> {
        self.base_map.get(&id).map(|&i| &self.bases[i])
    }

    /// The entry of a room.
    #[must_use]
    pub fn room(&self, id: u32) -> Option<&RoomEntry> {
        self.location_map
            .get(&id)
            .map(|&(b, r)| &self.bases[b].rooms[r])
    }
}

fn base_db<'c>(cx: &'c mut Cx<'_>) -> &'c mut BaseDb {
    if cx.logic.bases.is_none() {
        cx.logic.bases = Some(BaseDb::new(&cx.world.data().bases));
    }
    cx.logic.bases.get_or_insert_with(BaseDb::default)
}

fn invalid_base(cx: &mut Cx<'_>, id: u32) {
    cx.logic
        .reports
        .push(BASE_DB, 0x659, format!("*** ERROR: Base {id} is invalid"));
}

fn invalid_location(cx: &mut Cx<'_>, id: u32) {
    cx.logic.reports.push(
        BASE_DB,
        0x66d,
        format!("*** ERROR: Location {id} is invalid"),
    );
}

fn send(cx: &mut Cx<'_>, session: u32, message: impl Into<ServerMessage>) {
    cx.logic
        .client
        .send(session, ToClient::Server(message.into()));
}

fn marshalled(
    write: impl FnOnce(&mut MarshalWriter) -> Result<(), game_sim::protocol::WireError>,
) -> Vec<u8> {
    let mut w = MarshalWriter::new();
    let _ = write(&mut w);
    w.into_bytes()
}

/// How each record kind travels.
trait Sent {
    fn update(&self) -> ServerMessage;
    fn destroy(&self) -> ServerMessage;
    fn key(&self) -> u32;
}

impl Sent for CharacterProperties {
    fn update(&self) -> ServerMessage {
        GfUpdateChar {
            data: marshalled(|w| self.marshal(w)),
        }
        .into()
    }
    fn destroy(&self) -> ServerMessage {
        GfDestroyCharacter {
            field_00: self.room_id,
            field_04: self.key,
        }
        .into()
    }
    fn key(&self) -> u32 {
        self.key
    }
}

impl Sent for ScriptBehavior {
    fn update(&self) -> ServerMessage {
        GfScriptBehavior {
            data: marshalled(|w| self.marshal(w)),
        }
        .into()
    }
    fn destroy(&self) -> ServerMessage {
        GfDestroyScriptBehavior {
            field_00: self.field_04,
            field_04: self.behavior_id as u32,
        }
        .into()
    }
    fn key(&self) -> u32 {
        self.behavior_id as u32
    }
}

impl Sent for AmbientScriptProperties {
    fn update(&self) -> ServerMessage {
        GfUpdateAmbientScript {
            data: marshalled(|w| self.marshal(w)),
        }
        .into()
    }
    fn destroy(&self) -> ServerMessage {
        GfDestroyAmbientScript {
            field_00: self.room_id,
            field_04: self.key as u32,
        }
        .into()
    }
    fn key(&self) -> u32 {
        self.key as u32
    }
}

impl Sent for MissionVendorProperties {
    fn update(&self) -> ServerMessage {
        GfUpdateMissionComputer {
            data: marshalled(|w| self.marshal(w)),
        }
        .into()
    }
    fn destroy(&self) -> ServerMessage {
        GfDestroyMissionComputer {
            field_00: self.base_id,
            field_04: self.key as u32,
        }
        .into()
    }
    fn key(&self) -> u32 {
        self.key as u32
    }
}

impl Sent for NewsBroadcastProperties {
    fn update(&self) -> ServerMessage {
        GfUpdateNewsBroadcast {
            data: marshalled(|w| self.marshal(w)),
        }
        .into()
    }
    fn destroy(&self) -> ServerMessage {
        GfDestroyNewsBroadcast {
            field_00: self.base_id,
            field_04: self.key as u32,
        }
        .into()
    }
    fn key(&self) -> u32 {
        self.key as u32
    }
}

/// `FRecordList::broadcast` / the creation loop: the record to every
/// session its filter passes.
fn broadcast<P: Sent>(cx: &mut Cx<'_>, sessions: &[u32], record: &Record<P>) {
    for &session in sessions {
        if session_passes(session, record.filter) {
            send(cx, session, record.props.update());
        }
    }
}

/// `FRecordList::remove`: the first record with the key leaves the list,
/// its destroy sent to the sessions its filter passes. `false` when none
/// has the key.
fn remove_record<P: Sent>(
    cx: &mut Cx<'_>,
    sessions: &[u32],
    list: &mut Vec<Record<P>>,
    key: u32,
) -> bool {
    let Some(at) = list.iter().position(|r| r.props.key() == key) else {
        return false;
    };
    let record = list.remove(at);
    for &session in sessions {
        if session_passes(session, record.filter) {
            send(cx, session, record.props.destroy());
        }
    }
    true
}

/// `BaseDB::load_base`: every room of the base reads its file
/// (`RoomData::load`, once).
pub fn load_base(cx: &mut Cx<'_>, source: &dyn DataSource, id: u32) {
    let data = cx.world.data_arc();
    let Some(&bi) = base_db(cx).base_map.get(&id) else {
        invalid_base(cx, id);
        return;
    };
    let mut reports = std::mem::take(&mut cx.logic.reports);
    let rooms: Vec<u32> = base_db(cx).bases[bi]
        .rooms
        .iter()
        .map(|r| r.room_id)
        .collect();
    for (ri, room_id) in rooms.into_iter().enumerate() {
        if base_db(cx).bases[bi].rooms[ri].file.is_some() {
            continue;
        }
        let file = data
            .bases
            .room(room_id)
            .map(|r| read_room_file(source, &r.file, &mut reports))
            .unwrap_or_default();
        base_db(cx).bases[bi].rooms[ri].file = Some(file);
    }
    cx.logic.reports = reports;
}

/// `BaseDB::unload_base`: `RoomData::unload` does nothing; only the id is
/// checked.
pub fn unload_base(cx: &mut Cx<'_>, id: u32) {
    if !base_db(cx).base_map.contains_key(&id) {
        invalid_base(cx, id);
    }
}

/// `FRoomEntry::enter` through `BaseDB::enter_location`: the player's own
/// character record (the fidget script, its costume and voice) is made
/// and sent to the room.
pub fn enter_location(cx: &mut Cx<'_>, session: u32, room: u32) {
    let Some(&(bi, ri)) = base_db(cx).location_map.get(&room) else {
        invalid_location(cx, room);
        return;
    };
    let handle = base_db(cx).character_handles.allocate();
    let (vibe, costume, voice) = cx
        .logic
        .players
        .get(session)
        .map(|s| (s.rep_vibe, s.costume.clone(), s.voice.clone()))
        .unwrap_or_default();
    let name = {
        let (vibes, _) = cx.world.vibes();
        vibes.name(vibe).map_or_else(
            || ">>>BAD   NAME<<<".encode_utf16().collect(),
            |(_, _, display)| display,
        )
    };
    let room_id = base_db(cx).bases[bi].rooms[ri].room_id;
    let props = CharacterProperties {
        key: handle as u32,
        session_id: session,
        room_id,
        name_ids: 0,
        faction_id: u32::MAX,
        name_wide: Some(name),
        name_ansi: None,
        char_type: 1,
        posture: 0,
        costume: WireCostume {
            parts: costume.part_ids,
            accessories: costume.accessory_ids.clone(),
        },
        script: Some(b"scripts\\extras\\player_fidget.thn".to_vec()),
        behavior_id: u32::MAX,
        voice_id: create_id(&voice),
    };
    let record = Record {
        props,
        filter: 0,
        dirty: true,
    };
    let sessions = base_db(cx).bases[bi].rooms[ri].sessions.clone();
    broadcast(cx, &sessions, &record);
    base_db(cx).bases[bi].rooms[ri].characters.push(Record {
        dirty: false,
        ..record
    });
}

/// `BaseDB::exit_location`: the player's own character record leaves the
/// room, and its handle is freed.
pub fn exit_location(cx: &mut Cx<'_>, session: u32, room: u32) {
    let Some(&(bi, ri)) = base_db(cx).location_map.get(&room) else {
        invalid_location(cx, room);
        return;
    };
    let handle = base_db(cx).bases[bi].rooms[ri]
        .characters
        .iter()
        .find(|r| r.props.session_id == session)
        .map_or(0, |r| r.props.key);
    if handle & 0x3FFF_FFFF == 0 {
        return;
    }
    let entry = &mut base_db(cx).bases[bi].rooms[ri];
    let sessions = entry.sessions.clone();
    let mut list = std::mem::take(&mut entry.characters);
    remove_record(cx, &sessions, &mut list, handle);
    let db = base_db(cx);
    db.bases[bi].rooms[ri].characters = list;
    db.character_handles.set(handle.wrapping_sub(1), false);
}

/// `BaseDB::base_info`: a full request adds the session to the base and
/// sends the start room, the mission vendor offers and the news; a short
/// one removes it.
pub fn base_info(cx: &mut Cx<'_>, session: u32, id: u32, full: bool) {
    let Some(&bi) = base_db(cx).base_map.get(&id) else {
        invalid_base(cx, id);
        return;
    };
    let entry = &mut base_db(cx).bases[bi];
    if !full {
        remove_session(&mut entry.sessions, session);
        return;
    }
    if entry.sessions.contains(&session) {
        return;
    }
    entry.sessions.push(session);
    send_start_room(cx, bi, session);
    let entry = base_db(cx).bases[bi].clone();
    for record in &entry.mission_vendors {
        if session_passes(session, record.filter) {
            send(cx, session, record.props.update());
        }
    }
    send(
        cx,
        session,
        GfCompleteMissionComputerList {
            value: entry.base_id,
        },
    );
    for record in &entry.news {
        if session_passes(session, record.filter) {
            send(cx, session, record.props.update());
        }
    }
    send(
        cx,
        session,
        GfCompleteNewsBroadcastList {
            value: entry.base_id,
        },
    );
}

/// `FBaseEntry::send_start_room`: the data's start room, unless a room has
/// a behaviour whose `Field10` names another.
fn send_start_room(cx: &mut Cx<'_>, bi: usize, session: u32) {
    let entry = &base_db(cx).bases[bi];
    let (base_id, default) = (entry.base_id, entry.start_room);
    let start = entry
        .rooms
        .iter()
        .find_map(|r| {
            r.behaviors
                .iter()
                .find(|b| b.props.field_10 != 0)
                .map(|b| b.props.field_10)
        })
        .unwrap_or(default);
    if default == 0 {
        cx.logic.reports.push(
            BASE_DB,
            0x454,
            format!("*** ERROR: No start room in base {base_id}"),
        );
    }
    send(
        cx,
        session,
        SetStartRoom {
            field_00: base_id,
            field_04: start,
        },
    );
}

/// `BaseDB::location_info`: a full request adds the session to the room
/// and sends its characters, behaviours and ambient scripts; a short one
/// removes it.
pub fn location_info(cx: &mut Cx<'_>, session: u32, room: u32, full: bool) {
    let Some(&(bi, ri)) = base_db(cx).location_map.get(&room) else {
        invalid_location(cx, room);
        return;
    };
    let entry = &mut base_db(cx).bases[bi].rooms[ri];
    if !full {
        remove_session(&mut entry.sessions, session);
        return;
    }
    if entry.sessions.contains(&session) {
        return;
    }
    add_session(&mut entry.sessions, session);
    let entry = base_db(cx).bases[bi].rooms[ri].clone();
    for record in &entry.characters {
        if session_passes(session, record.filter) {
            send(cx, session, record.props.update());
        }
    }
    send(
        cx,
        session,
        GfCompleteCharList {
            value: entry.room_id,
        },
    );
    for record in &entry.behaviors {
        if session_passes(session, record.filter) {
            send(cx, session, record.props.update());
        }
    }
    send(
        cx,
        session,
        GfCompleteScriptBehaviorList {
            value: entry.room_id,
        },
    );
    for record in &entry.ambient_scripts {
        if session_passes(session, record.filter) {
            send(cx, session, record.props.update());
        }
    }
    send(
        cx,
        session,
        GfCompleteAmbientScriptList {
            value: entry.room_id,
        },
    );
}

/// `BaseDB::create_character` (`FRoomEntry::create_character`): an NPC in
/// a room. A placement name takes the placement's posture. The C++ reads
/// placement -1 for a name the room lacks and calls through a null room
/// for an unknown one; the port answers -1 there (D611).
pub fn create_character(cx: &mut Cx<'_>, props: &CharacterProperties, filter: u32) -> i32 {
    let Some(&(bi, ri)) = base_db(cx).location_map.get(&props.room_id) else {
        invalid_location(cx, props.room_id);
        return -1;
    };
    let mut posture = u32::MAX;
    if let Some(name) = &props.name_ansi {
        let file = base_db(cx).bases[bi].rooms[ri]
            .file
            .clone()
            .unwrap_or_default();
        let Some(found) = file
            .placements
            .iter()
            .position(|p| stricmp(&p.name, name).is_eq())
        else {
            return -1;
        };
        posture = file.placements[found].posture as u32;
    }
    let db = base_db(cx);
    let handle = db.character_handles.allocate();
    let mut record = Record {
        props: props.clone(),
        filter,
        dirty: true,
    };
    record.props.key = handle as u32;
    record.props.char_type = 0;
    record.props.posture = posture;
    let sessions = db.bases[bi].rooms[ri].sessions.clone();
    broadcast(cx, &sessions, &record);
    record.dirty = false;
    let db = base_db(cx);
    db.bases[bi].rooms[ri].characters.push(record);
    db.character_map.entry(handle as u32).or_insert((bi, ri));
    handle
}

/// `FRoomEntry::get_character_on_placement`: the key of the first
/// character on the room's placement `index` that the session filter
/// passes, else 0.
pub fn character_on_placement(cx: &mut Cx<'_>, room: u32, filter: u32, index: usize) -> u32 {
    let Some(&(bi, ri)) = base_db(cx).location_map.get(&room) else {
        return 0;
    };
    let entry = &base_db(cx).bases[bi].rooms[ri];
    let Some(placement) = entry.file.as_ref().and_then(|f| f.placements.get(index)) else {
        return 0;
    };
    entry
        .characters
        .iter()
        .find(|r| {
            r.props
                .name_ansi
                .as_ref()
                .is_some_and(|n| stricmp(n, &placement.name).is_eq())
                && (filter == 0 || r.filter == 0 || r.filter == filter)
        })
        .map_or(0, |r| r.props.key)
}

fn not_found(cx: &mut Cx<'_>, line: u32, what: &str, key: u32) {
    cx.logic.reports.push(
        BASE_DB,
        line,
        format!("*** ERROR: Could not find {what} {key}"),
    );
}

/// `BaseDB::destroy_character`.
pub fn destroy_character(cx: &mut Cx<'_>, handle: u32) {
    match base_db(cx).character_map.get(&handle).copied() {
        None => {
            not_found(cx, 0x720, "npc character", handle);
            not_found(cx, 0x739, "npc character", handle);
        }
        Some((bi, ri)) => {
            let sessions = base_db(cx).bases[bi].rooms[ri].sessions.clone();
            let mut list = std::mem::take(&mut base_db(cx).bases[bi].rooms[ri].characters);
            remove_record(cx, &sessions, &mut list, handle);
            let db = base_db(cx);
            db.bases[bi].rooms[ri].characters = list;
            db.character_handles.set(handle.wrapping_sub(1), false);
            db.character_map.remove(&handle);
        }
    }
}

/// `BaseDB::set_character_behavior`: the record is marked to be sent at
/// the next update.
pub fn set_character_behavior(cx: &mut Cx<'_>, handle: u32, behavior: u32) {
    let Some((bi, ri)) = base_db(cx).character_map.get(&handle).copied() else {
        not_found(cx, 0x720, "npc character", handle);
        return;
    };
    if let Some(record) = base_db(cx).bases[bi].rooms[ri]
        .characters
        .iter_mut()
        .find(|r| r.props.key == handle)
    {
        record.props.behavior_id = behavior;
        record.dirty = true;
    }
}

/// `BaseDB::create_behavior`: a scripted behaviour in the room `field_04`
/// names. The C++ calls through a null room for an unknown one; the port
/// answers `0xffffffff` there (D611).
pub fn create_behavior(cx: &mut Cx<'_>, props: &ScriptBehavior, filter: u32) -> u32 {
    let Some(&(bi, ri)) = base_db(cx).location_map.get(&props.field_04) else {
        invalid_location(cx, props.field_04);
        return u32::MAX;
    };
    let db = base_db(cx);
    let handle = db.behavior_handles.allocate_index();
    let mut record = Record {
        props: props.clone(),
        filter,
        dirty: true,
    };
    record.props.behavior_id = handle as i32;
    let sessions = db.bases[bi].rooms[ri].sessions.clone();
    broadcast(cx, &sessions, &record);
    record.dirty = false;
    let db = base_db(cx);
    db.bases[bi].rooms[ri].behaviors.push(record);
    db.behavior_map.entry(handle).or_insert((bi, ri));
    handle
}

/// `BaseDB::destroy_behavior`.
pub fn destroy_behavior(cx: &mut Cx<'_>, handle: u32) {
    match base_db(cx).behavior_map.get(&handle).copied() {
        None => {
            not_found(cx, 0x774, "behavior", handle);
            not_found(cx, 0x78d, "behavior", handle);
        }
        Some((bi, ri)) => {
            let sessions = base_db(cx).bases[bi].rooms[ri].sessions.clone();
            let mut list = std::mem::take(&mut base_db(cx).bases[bi].rooms[ri].behaviors);
            remove_record(cx, &sessions, &mut list, handle);
            let db = base_db(cx);
            db.bases[bi].rooms[ri].behaviors = list;
            db.behavior_handles.set(handle, false);
            db.behavior_map.remove(&handle);
        }
    }
}

/// `BaseDB::create_ambient_script`: `0xffffffff` for an unknown room.
pub fn create_ambient_script(cx: &mut Cx<'_>, props: &AmbientScriptProperties, filter: u32) -> u32 {
    let Some(&(bi, ri)) = base_db(cx).location_map.get(&props.room_id) else {
        invalid_location(cx, props.room_id);
        return u32::MAX;
    };
    let db = base_db(cx);
    let handle = db.ambient_handles.allocate_index();
    let mut record = Record {
        props: props.clone(),
        filter,
        dirty: true,
    };
    record.props.key = handle as i32;
    let sessions = db.bases[bi].rooms[ri].sessions.clone();
    broadcast(cx, &sessions, &record);
    record.dirty = false;
    let db = base_db(cx);
    db.bases[bi].rooms[ri].ambient_scripts.push(record);
    db.ambient_map.entry(handle).or_insert((bi, ri));
    handle
}

/// `BaseDB::destroy_ambient_script`.
pub fn destroy_ambient_script(cx: &mut Cx<'_>, handle: u32) {
    match base_db(cx).ambient_map.get(&handle).copied() {
        None => {
            not_found(cx, 0x84a, "ambient script", handle);
            not_found(cx, 0x863, "ambient script", handle);
        }
        Some((bi, ri)) => {
            let sessions = base_db(cx).bases[bi].rooms[ri].sessions.clone();
            let mut list = std::mem::take(&mut base_db(cx).bases[bi].rooms[ri].ambient_scripts);
            remove_record(cx, &sessions, &mut list, handle);
            let db = base_db(cx);
            db.bases[bi].rooms[ri].ambient_scripts = list;
            db.ambient_handles.set(handle, false);
            db.ambient_map.remove(&handle);
        }
    }
}

/// `BaseDB::create_mission_vendor`. The C++ calls through a null base for
/// an unknown one; the port answers `0xffffffff` there (D611).
pub fn create_mission_vendor(cx: &mut Cx<'_>, props: &MissionVendorProperties, filter: u32) -> u32 {
    let Some(&bi) = base_db(cx).base_map.get(&props.base_id) else {
        invalid_base(cx, props.base_id);
        return u32::MAX;
    };
    let db = base_db(cx);
    let handle = db.vendor_handles.allocate_index();
    let mut record = Record {
        props: props.clone(),
        filter,
        dirty: true,
    };
    record.props.key = handle as i32;
    let sessions = db.bases[bi].sessions.clone();
    broadcast(cx, &sessions, &record);
    record.dirty = false;
    let db = base_db(cx);
    db.bases[bi].mission_vendors.push(record);
    db.vendor_map.entry(handle).or_insert(bi);
    handle
}

/// `BaseDB::destroy_mission_vendor`.
pub fn destroy_mission_vendor(cx: &mut Cx<'_>, handle: u32) {
    match base_db(cx).vendor_map.get(&handle).copied() {
        None => {
            not_found(cx, 0x7b2, "mission vendor offer", handle);
            not_found(cx, 0x7cb, "mission vendor offer", handle);
        }
        Some(bi) => {
            let sessions = base_db(cx).bases[bi].sessions.clone();
            let mut list = std::mem::take(&mut base_db(cx).bases[bi].mission_vendors);
            remove_record(cx, &sessions, &mut list, handle);
            let db = base_db(cx);
            db.bases[bi].mission_vendors = list;
            db.vendor_handles.set(handle, false);
            db.vendor_map.remove(&handle);
        }
    }
}

/// `BaseDB::count_mission_vendors`: the offers of a base the session
/// filter passes. The C++ walks a null base for an unknown one; the port
/// counts 0 (D611).
pub fn count_mission_vendors(cx: &mut Cx<'_>, id: u32, filter: u32) -> i32 {
    let Some(&bi) = base_db(cx).base_map.get(&id) else {
        invalid_base(cx, id);
        return 0;
    };
    base_db(cx).bases[bi]
        .mission_vendors
        .iter()
        .filter(|r| filter == 0 || r.filter == 0 || r.filter == filter)
        .count() as i32
}

/// `BaseDB::mission_vendor_acceptance`: the answer to an offer, to one
/// session.
pub fn mission_vendor_acceptance(
    cx: &mut Cx<'_>,
    handle: u32,
    accepted: bool,
    reason: &FmtStr,
    session: u32,
) {
    let Some(bi) = base_db(cx).vendor_map.get(&handle).copied() else {
        not_found(cx, 0x7b2, "mission vendor offer", handle);
        return;
    };
    let base_id = base_db(cx).bases[bi].base_id;
    let acceptance = MissionVendorAcceptance {
        key: handle as i32,
        target_id: base_id,
        accepted,
        reason: reason.clone(),
    };
    let data = marshalled(|w| acceptance.marshal(w));
    send(cx, session, GfMissionVendorAcceptance { data });
}

/// `BaseDB::create_news_broadcast`. The C++ calls through a null base for
/// an unknown one; the port answers `0xffffffff` there (D611).
pub fn create_news_broadcast(cx: &mut Cx<'_>, props: &NewsBroadcastProperties, filter: u32) -> u32 {
    let Some(&bi) = base_db(cx).base_map.get(&props.base_id) else {
        invalid_base(cx, props.base_id);
        return u32::MAX;
    };
    let db = base_db(cx);
    let handle = db.news_handles.allocate_index();
    let mut record = Record {
        props: props.clone(),
        filter,
        dirty: true,
    };
    record.props.key = handle as i32;
    let sessions = db.bases[bi].sessions.clone();
    broadcast(cx, &sessions, &record);
    record.dirty = false;
    let db = base_db(cx);
    db.bases[bi].news.push(record);
    db.news_map.entry(handle).or_insert(bi);
    handle
}

/// `BaseDB::destroy_news_broadcast`.
pub fn destroy_news_broadcast(cx: &mut Cx<'_>, handle: u32) {
    match base_db(cx).news_map.get(&handle).copied() {
        None => {
            not_found(cx, 0x809, "news broadcast", handle);
            not_found(cx, 0x822, "news broadcast", handle);
        }
        Some(bi) => {
            let sessions = base_db(cx).bases[bi].sessions.clone();
            let mut list = std::mem::take(&mut base_db(cx).bases[bi].news);
            remove_record(cx, &sessions, &mut list, handle);
            let db = base_db(cx);
            db.bases[bi].news = list;
            db.news_handles.set(handle, false);
            db.news_map.remove(&handle);
        }
    }
}

/// `BaseDB::update`: in each base with sessions, each room with sessions
/// sends its changed characters, behaviours and ambient scripts; then the
/// base's changed offers and news.
pub fn update(cx: &mut Cx<'_>) {
    let count = base_db(cx).bases.len();
    for bi in 0..count {
        if base_db(cx).bases[bi].sessions.is_empty() {
            continue;
        }
        let rooms = base_db(cx).bases[bi].rooms.len();
        for ri in 0..rooms {
            let room = base_db(cx).bases[bi].rooms[ri].clone();
            if room.sessions.is_empty() {
                continue;
            }
            for record in room.characters.iter().filter(|r| r.dirty) {
                broadcast(cx, &room.sessions, record);
            }
            for record in room.behaviors.iter().filter(|r| r.dirty) {
                broadcast(cx, &room.sessions, record);
            }
            for record in room.ambient_scripts.iter().filter(|r| r.dirty) {
                broadcast(cx, &room.sessions, record);
            }
            let entry = &mut base_db(cx).bases[bi].rooms[ri];
            entry.characters.iter_mut().for_each(|r| r.dirty = false);
            entry.behaviors.iter_mut().for_each(|r| r.dirty = false);
            entry
                .ambient_scripts
                .iter_mut()
                .for_each(|r| r.dirty = false);
        }
        let base = base_db(cx).bases[bi].clone();
        for record in base.mission_vendors.iter().filter(|r| r.dirty) {
            broadcast(cx, &base.sessions, record);
        }
        for record in base.news.iter().filter(|r| r.dirty) {
            broadcast(cx, &base.sessions, record);
        }
        let entry = &mut base_db(cx).bases[bi];
        entry
            .mission_vendors
            .iter_mut()
            .for_each(|r| r.dirty = false);
        entry.news.iter_mut().for_each(|r| r.dirty = false);
    }
}
