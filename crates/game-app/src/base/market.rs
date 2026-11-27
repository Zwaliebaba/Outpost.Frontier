//! A base's market as the client holds it (`client_base_market.cpp`, work
//! package P5.9): every good of the goods list with the base's entry or the
//! good's own price, its stock flag and a price scale the base's
//! `price_variance` draws, filled when the base loads (`ClientMarketFill`),
//! and the lookups the dealer screens read (P5.10).

use std::collections::BTreeMap;

use game_sim::data::base::{MarketGood, TransactionType};
use game_sim::protocol::to_server::{ClientMessage, GfGoodBuy, GfGoodSell};
use neuron_core::num::trunc_to_i32;

/// A good of the goods list, as the fill reads it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ListGood {
    /// `GoodInfo +0x48`.
    pub id: u32,
    /// `GoodInfo +0x58`.
    pub price: f32,
}

/// `FClientMarketGood`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClientMarketGood {
    /// The base's entry, or the good's id and price.
    pub info: MarketGood,
    /// `0x7f00_0000` when the entry has a minimum and a positive maximum.
    pub stock: i32,
    /// `1 - variance + rand() / 32768 * 2 * variance`.
    pub price_scale: f32,
}

/// `FClientMarket`, by good id.
pub type ClientMarket = BTreeMap<u32, ClientMarketGood>;

/// `ClientMarketFill`: every good of `goods` in order, from the base's
/// `market` or the good's own price, each drawing one `rand`.
pub fn fill(
    market: &mut ClientMarket,
    price_variance: f32,
    base_market: &BTreeMap<u32, MarketGood>,
    goods: &[ListGood],
    rand: &mut dyn FnMut() -> i32,
) {
    if goods.is_empty() {
        return;
    }
    let low = (1.0 - f64::from(price_variance)) as f32;
    let range = ((f64::from(price_variance) + 1.0) - f64::from(low)) as f32;
    for good in goods {
        let info = base_market.get(&good.id).copied().unwrap_or(MarketGood {
            good_id: good.id,
            price: good.price,
            min_inventory: 0,
            max_inventory: 0,
            transaction: TransactionType::BUY_AND_SELL,
            min_level: 0.0,
            min_rep: 0.0,
        });
        let stock = if info.min_inventory != 0 && info.max_inventory > 0 {
            0x7F00_0000
        } else {
            0
        };
        let roll = f64::from(rand());
        let price_scale = (roll * (1.0 / 32768.0) * f64::from(range) + f64::from(low)) as f32;
        market.insert(
            good.id,
            ClientMarketGood {
                info,
                stock,
                price_scale,
            },
        );
    }
}

/// `ClientMarketGetPrice`: the price times the scale, truncated, at most
/// 9999999, and 1 below 0; 0 for a good not on the market.
#[must_use]
pub fn price(market: &ClientMarket, good: u32) -> f32 {
    let Some(entry) = market.get(&good) else {
        return 0.0;
    };
    let truncated = trunc_to_i32(f64::from(entry.info.price) * f64::from(entry.price_scale));
    let mut price = truncated as f32;
    if f64::from(truncated) > 9_999_999.0 {
        price = 9_999_999.0;
    }
    if price < 0.0 {
        price = 1.0;
    }
    price
}

/// `ClientMarketGetTransactionType`: -1 for a good not on the market.
#[must_use]
pub fn transaction_type(market: &ClientMarket, good: u32) -> i32 {
    market.get(&good).map_or(-1, |e| e.info.transaction.0)
}

/// `ClientMarketGetMinLevel`: 0 for a good not on the market.
#[must_use]
pub fn min_level(market: &ClientMarket, good: u32) -> f32 {
    market.get(&good).map_or(0.0, |e| e.info.min_level)
}

/// `ClientMarketGetMinRep`: -1 for a good not on the market.
#[must_use]
pub fn min_rep(market: &ClientMarket, good: u32) -> f32 {
    market.get(&good).map_or(-1.0, |e| e.info.min_rep)
}

/// `ClientMarketRequestBuy`'s message: `{base, 0, good, count}`.
#[must_use]
pub fn buy_request(base: u32, good: u32, count: i32) -> ClientMessage {
    ClientMessage::GfGoodBuy(GfGoodBuy {
        field_00: base,
        field_04: 0,
        field_08: good,
        field_0c: count.cast_unsigned(),
    })
}

/// `ClientMarketRequestSell`'s message: `{base, good, count}`.
#[must_use]
pub fn sell_request(base: u32, good: u32, count: i32) -> ClientMessage {
    ClientMessage::GfGoodSell(GfGoodSell {
        field_00: base,
        field_04: good,
        field_08: count.cast_unsigned(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_fill_draws_a_scale_per_good_and_keeps_the_base_entries() {
        let goods = [
            ListGood {
                id: 1,
                price: 100.0,
            },
            ListGood { id: 2, price: 50.0 },
        ];
        let mut base = BTreeMap::new();
        base.insert(
            2,
            MarketGood {
                good_id: 2,
                price: 80.0,
                min_inventory: 5,
                max_inventory: 10,
                transaction: TransactionType::SALE_ONLY,
                min_level: 3.0,
                min_rep: 0.5,
            },
        );
        let mut rolls = [0, 16384].into_iter();
        let mut market = ClientMarket::new();
        fill(&mut market, 0.25, &base, &goods, &mut || {
            rolls.next().unwrap()
        });
        // rand 0: the low end; rand 16384: the middle.
        assert_eq!(market[&1].price_scale, 0.75);
        assert_eq!(market[&2].price_scale, 1.0);
        assert_eq!((market[&1].stock, market[&2].stock), (0, 0x7F00_0000));
        assert_eq!(price(&market, 1), 75.0);
        assert_eq!(price(&market, 2), 80.0);
        assert_eq!(price(&market, 9), 0.0);
        assert_eq!(transaction_type(&market, 2), 2);
        assert_eq!((min_level(&market, 2), min_rep(&market, 9)), (3.0, -1.0));
    }
}
