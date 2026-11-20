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
