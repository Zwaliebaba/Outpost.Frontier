//! Base data: each base's start room, prices, rooms and market
//! (`BaseDataList_load`, `BaseDataList_load_market_data`), ported from
//! `CppSource/Common/common_basedata.cpp`.
//!
//! `bases` in the `[Data]` list walks the universe's bases in id order and
//! reads each base's own INI (`[Base]`/`[BaseInfo]` and `[Room]` blocks).
//! A room is only named here (`RoomData::init`); its own INI (cameras,
//! hotspots, character placements, set scripts) is read when a client
//! enters it and belongs to the base screens (P5.9). `markets` files then
//! add `[BaseGood]` blocks to the bases.

use std::collections::BTreeMap;

use neuron_core::id::{create_id, make_location_id};
use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32;

use super::goods::GoodList;
use super::string_cache::StringCache;
use super::universe::Universe;
use super::{DataSource, Reports, lossy, open_ini, value_f32};

const SOURCE: &str = "Common\\BaseData.cpp";

/// `TransactionType`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TransactionType(pub i32);

impl TransactionType {
    /// The base buys and sells (0).
    pub const BUY_AND_SELL: Self = Self(0);
    /// The base only buys (1).
    pub const DEMAND_ONLY: Self = Self(1);
    /// The base only sells (2).
    pub const SALE_ONLY: Self = Self(2);
}

/// `MarketGoodInfo`: one good on a base's market.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MarketGood {
    /// The good.
    pub good_id: u32,
    /// The good's price times the multiplier, truncated.
    pub price: f32,
    /// Value 3.
    pub min_inventory: i32,
    /// Value 4.
    pub max_inventory: i32,
    /// Value 5.
    pub transaction: TransactionType,
    /// Value 1 (0).
    pub min_level: f32,
    /// Value 2 (-1).
    pub min_rep: f32,
}

/// A room as the base list names it (`RoomData::init`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Room {
    /// The base.
    pub base_id: u32,
    /// `CreateID("<base id in hex>_<nickname>")`.
    pub room_id: u32,
    /// `nickname`.
    pub nickname: Vec<u8>,
    /// `file`.
    pub file: Vec<u8>,
}

/// `BaseData`.
#[derive(Clone, Debug, PartialEq)]
pub struct BaseData {
    /// The base.
    pub base_id: u32,
    /// `start_room`: the room id.
    pub start_room: u32,
    /// `price_variance`.
    pub price_variance: f32,
    /// `ship_repair_cost` (0.33).
    pub ship_repair_cost: f32,
    /// The market, by good id.
    pub market: BTreeMap<u32, MarketGood>,
    /// The `[Room]` blocks in file order; `None` for a block without a
    /// nickname or file (the C++ keeps an empty record).
    pub rooms: Vec<Option<Room>>,
}

impl BaseData {
    fn new(base_id: u32) -> Self {
        Self {
            base_id,
            start_room: 0,
            price_variance: 0.0,
            ship_repair_cost: 0.33,
            market: BTreeMap::new(),
            rooms: Vec::new(),
        }
    }

    /// `read_from_ini`.
    fn read(&mut self, source: &dyn DataSource, path: &[u8], reports: &mut Reports) {
        let Some(mut reader) = open_ini(source, path) else {
            reports.push(
                SOURCE,
                0xea,
                format!("*** ERROR: Cannot open base .ini file {}", lossy(path)),
            );
            return;
        };
        while reader.read_header() {
            if reader.is_header(b"Base") || reader.is_header(b"BaseInfo") {
                self.read_base_block(&mut reader, reports);
            } else if reader.is_header(b"Room") {
                self.read_room_block(&mut reader, reports);
            } else {
                reports.push(
                    SOURCE,
                    0xe2,
                    format!(
                        "*** ERROR: Invalid ini block in file {}",
                        lossy(reader.file_name())
                    ),
                );
            }
        }
    }

    fn read_base_block(&mut self, reader: &mut IniReader, reports: &mut Reports) {
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                continue;
            }
            if reader.is_value(b"start_room") {
                self.start_room = make_location_id(self.base_id, &reader.value_string());
            } else if reader.is_value(b"price_variance") {
                self.price_variance = value_f32(reader, 0);
            } else if reader.is_value(b"ship_repair_cost") {
                self.ship_repair_cost = value_f32(reader, 0);
            } else {
                reports.push(
                    SOURCE,
                    0x89,
                    format!(
                        "*** ERROR: Invalid ini data in file {}",
                        lossy(reader.file_name())
                    ),
                );
            }
        }
        if self.start_room == 0 {
            reports.push(
                SOURCE,
                0x90,
                format!(
                    "*** ERROR: No start room in base ini file {}",
                    lossy(reader.file_name())
                ),
            );
        }
    }

    fn read_room_block(&mut self, reader: &mut IniReader, reports: &mut Reports) {
        let mut nickname = Vec::new();
        let mut file = Vec::new();
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                nickname = neuron_core::hash::c_str(&reader.value_string()).to_vec();
            } else if reader.is_value(b"file") {
                file = neuron_core::hash::c_str(&reader.value_string()).to_vec();
            } else {
                reports.push(
                    SOURCE,
                    0xb5,
                    format!(
                        "*** ERROR: Invalid ini data in file {}",
                        lossy(reader.file_name())
                    ),
                );
            }
        }
        if nickname.is_empty() || file.is_empty() {
            reports.push(
                SOURCE,
                0xc1,
                format!(
                    "*** ERROR: Invalid [Room] entry in ini file {}, nickname or file is missing",
                    lossy(reader.file_name())
                ),
            );
            self.rooms.push(None);
            return;
        }
        self.rooms.push(Some(Room {
            base_id: self.base_id,
            room_id: make_location_id(self.base_id, &nickname),
            nickname,
            file,
        }));
    }
}

/// `BaseDataList`.
#[derive(Clone, Debug, Default)]
pub struct BaseDataList {
    /// The bases in universe order.
    pub bases: Vec<BaseData>,
    by_id: BTreeMap<u32, usize>,
    rooms_by_id: BTreeMap<u32, (usize, usize)>,
}

impl BaseDataList {
    /// `get_base_data`.
    #[must_use]
    pub fn base(&self, base_id: u32) -> Option<&BaseData> {
        self.by_id.get(&base_id).map(|&i| &self.bases[i])
    }

    /// `get_room_data`.
    #[must_use]
    pub fn room(&self, room_id: u32) -> Option<&Room> {
        let &(base, room) = self.rooms_by_id.get(&room_id)?;
        self.bases[base].rooms[room].as_ref()
    }

    /// `BaseDataList::load`: every base of the universe, in id order.
    pub fn load(
        &mut self,
        source: &dyn DataSource,
        universe: &Universe,
        strings: &StringCache,
        reports: &mut Reports,
    ) {
        for (&id, base) in &universe.bases {
            let mut data = BaseData::new(0);
            if base.file.is_empty() {
                reports.push(
                    SOURCE,
                    0x11b,
                    format!("*** ERROR: No ini file for base {id}"),
                );
            } else {
                data.base_id = id;
                data.read(source, strings.get(base.file), reports);
            }
            self.bases.push(data);
        }
        for (index, data) in self.bases.iter().enumerate() {
            self.by_id.entry(data.base_id).or_insert(index);
            for (room_index, room) in data.rooms.iter().enumerate() {
                let room_id = room.as_ref().map_or(0, |r| r.room_id);
                self.rooms_by_id
                    .entry(room_id)
                    .or_insert((index, room_index));
            }
        }
    }

    /// `load_market_data(path)`.
    pub fn load_market_data(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        universe: &Universe,
        goods: &GoodList,
        reports: &mut Reports,
    ) {
        let Some(mut reader) = open_ini(source, path) else {
            reports.push(
                SOURCE,
                0x1d2,
                format!("*** ERROR: Cannot open market .ini file {}", lossy(path)),
            );
            return;
        };
        while reader.read_header() {
            if !reader.is_header(b"BaseGood") {
                reports.push(
                    SOURCE,
                    0x1ca,
                    format!(
                        "*** WARNING: Unknown ini block [{}] in market .ini file {} - ignoring",
                        lossy(&reader.header()),
                        lossy(path)
                    ),
                );
                continue;
            }
            let mut current: Option<usize> = None;
            while reader.read_value() {
                if reader.is_value(b"base") {
                    let nickname = reader.value_string();
                    let id = universe.lookup_base(&nickname, reports);
                    if id == 0 {
                        reports.push(
                            SOURCE,
                            0x153,
                            format!(
                                "*** WARNING: base name {} is unknown in market data file {}",
                                lossy(&nickname),
                                lossy(path)
                            ),
                        );
                    } else {
                        current = self.by_id.get(&id).copied();
                        if current.is_none() {
                            reports.push(
                                SOURCE,
                                0x1e8,
                                format!("*** ERROR: Could not find base {id}"),
                            );
                        }
                    }
                } else if reader.is_value(b"MarketGood") {
                    let good = goods
                        .find_by_id(create_id(&reader.value_string_at(0)))
                        .map_or(0, |g| g.id);
                    let mut level = 0.0;
                    let mut rep = -1.0;
                    let mut min_inventory = 0;
                    let mut max_inventory = 0;
                    let mut transaction = 0;
                    let mut price_mult = 1.0;
                    if !reader.is_value_empty(1) {
                        level = value_f32(&mut reader, 1);
                    }
                    if !reader.is_value_empty(2) {
                        rep = value_f32(&mut reader, 2);
                    }
                    if !reader.is_value_empty(3) {
                        min_inventory = reader.value_int(3);
                    }
                    if !reader.is_value_empty(4) {
                        max_inventory = reader.value_int(4);
                    }
                    if !reader.is_value_empty(5) {
                        transaction = reader.value_int(5);
                    }
                    if !reader.is_value_empty(6) {
                        price_mult = value_f32(&mut reader, 6);
                    }
                    if !reader.is_value_empty(7) {
                        reader.value_float(7);
                    }
                    let keep = good != 0
                        && current.is_some()
                        && (min_inventory != 0
                            || max_inventory != 0
                            || transaction != 0
                            || price_mult != 1.0
                            || level != 0.0
                            || rep != -1.0);
                    if let (true, Some(index), Some(info)) = (keep, current, goods.find_by_id(good))
                    {
                        let product = f64::from(info.price) * f64::from(price_mult);
                        let entry = MarketGood {
                            good_id: good,
                            price: trunc_to_i32(product) as f32,
                            min_inventory,
                            max_inventory,
                            transaction: TransactionType(transaction),
                            min_level: level,
                            min_rep: rep,
                        };
                        self.bases[index].market.insert(good, entry);
                    }
                } else {
                    reports.push(
                        SOURCE,
                        0x1c2,
                        format!(
                            "*** WARNING: Unknown ini data: [{}]  {} = {} in market .ini file - ignoring",
                            lossy(&reader.header()),
                            lossy(&reader.name()),
                            lossy(&reader.value_string())
                        ),
                    );
                }
            }
        }
    }
}
