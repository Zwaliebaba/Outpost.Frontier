//! The multiplayer cheat check (work package P3.2, closed with P3.3's base
//! database): `PlayerData_CheckForCheats` and its four tests,
//! `PlayerData_ReportCheat` and `LoadCheatMessage`
//! (`Server/server_classes.cpp:450-1260`).
//!
//! A save on a multiplayer server is refused when the player's ship
//! carries equipment beyond its hardpoints' class, equipment the base does
//! not sell and the player did not have before, more worth than the
//! player earned, or more cargo than the hold takes; the first finding is
//! reported to the universe chat and to `cheaters.log`, and every later
//! save of the character is refused.

use std::io::Write as _;
use std::path::Path;

use game_sim::data::base::TransactionType;
use game_sim::data::equip_desc::{EquipDesc, list};
use game_sim::data::goods::GoodCategory;
use game_sim::protocol::rdl::{RdlRecord, write_records};
use neuron_core::num::trunc_to_i32;
use neuron_server::account::AccountsRoot;

use crate::player::{CASH_CEILING, PlayerData};
use crate::server::Cx;

/// The shield hardpoint classes, best first (`ShieldSpecialChain`).
const SHIELD_CHAIN: [u32; 30] = [
    0x2e, 0x38, 0x42, 0x2d, 0x37, 0x41, 0x2c, 0x36, 0x40, 0x2b, 0x35, 0x3f, 0x2a, 0x34, 0x3e, 0x29,
    0x33, 0x3d, 0x28, 0x32, 0x3c, 0x27, 0x31, 0x3b, 0x26, 0x30, 0x3a, 0x25, 0x2f, 0x39,
];
/// The gun and turret hardpoint classes, best first (`GunSpecialChain`).
const GUN_CHAIN: [u32; 20] = [
    0x18, 0x22, 0x17, 0x21, 0x16, 0x20, 0x15, 0x1f, 0x14, 0x1e, 0x13, 0x1d, 0x12, 0x1c, 0x11, 0x1b,
    0x10, 0x1a, 0x0f, 0x19,
];

/// The rounding constant of the worth of a non-combinable item
/// (`0.4999989867f`, as its bits).
const ROUNDING: u32 = 0x3effffde;

/// `AClassShieldGenerator`.
const CLASS_SHIELD_GENERATOR: u32 = 0xE;
/// `AClassGun`.
const CLASS_GUN: u32 = 0x11;

/// The reasons `cheaters.log` names (`GCheatReason`).
pub const REASONS: [&str; 5] = [
    "None",
    "Ship Related",
    "Ship or Equipment not sold on base",
    "Equipment exceeds ship specs",
    "Ship capacity exceeded",
];

/// The default prefix of the cheat announcement (`GCheatMessage`).
pub const DEFAULT_MESSAGE: &str = "Possible cheating detected: ";

/// A local time for `cheaters.log` (`SYSTEMTIME`'s fields the log prints).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LocalTime {
    /// The year.
    pub year: u16,
    /// The month, 1 to 12.
    pub month: u16,
    /// The day.
    pub day: u16,
    /// The hour.
    pub hour: u16,
    /// The minute.
    pub minute: u16,
    /// The second.
    pub second: u16,
}

/// `HpChainRank`: the index of `kind` in `chain`, or its length.
fn rank(chain: &[u32], kind: u32) -> usize {
    chain.iter().position(|&c| c == kind).unwrap_or(chain.len())
}

/// `PlayerData_EquipmentExceedsShipSpecs`: a mounted shield generator or
/// gun of a better class than the ship's best hardpoint of that kind.
fn equipment_exceeds_ship_specs(cx: &Cx<'_>, slot: &PlayerData) -> bool {
    let data = cx.world.data();
    let Some(hull) = data.archetypes.ships.get(&slot.ship_archetype) else {
        return false;
    };
    let best = |chain: &[u32]| {
        chain
            .iter()
            .copied()
            .find(|&kind| hull.legal_hps(kind).is_some())
            .unwrap_or(0)
    };
    let hull_shield = rank(&SHIELD_CHAIN, best(&SHIELD_CHAIN));
    let hull_gun = rank(&GUN_CHAIN, best(&GUN_CHAIN));
    let strings = cx.world.strings();
    for item in &slot.equipment {
        if item.is_internal(strings) {
            continue;
        }
        let Some(equipment) = data.archetypes.equipment.get(&item.arch_id) else {
            continue;
        };
        match &equipment.kind {
            game_sim::data::archetype::equipment::EquipmentKind::ShieldGenerator(g)
                if equipment.class() == CLASS_SHIELD_GENERATOR =>
            {
                if rank(&SHIELD_CHAIN, g.hp_type) < hull_shield {
                    return true;
                }
            }
            game_sim::data::archetype::equipment::EquipmentKind::Gun(g)
                if equipment.class() == CLASS_GUN =>
            {
                let best = g.hp_gun_types.iter().copied().max().unwrap_or(0);
                if rank(&GUN_CHAIN, best) < hull_gun {
                    return true;
                }
            }
            _ => {}
        }
    }
    false
}

/// `CountArchInList`: the counts of an archetype's lines, each between 1
/// and 999 999.
fn count_arch(items: &[EquipDesc], arch: u32) -> i32 {
    items
        .iter()
        .filter(|e| e.arch_id == arch && e.count > 0 && e.count < 1_000_000)
        .fold(0i32, |total, e| total.wrapping_add(e.count))
}

fn sold(transaction: TransactionType) -> bool {
    transaction == TransactionType::BUY_AND_SELL || transaction == TransactionType::SALE_ONLY
}

/// `PlayerData_EquipmentNotSoldOnBase`: equipment added since the last
/// snapshot that the base does not sell (or, after a ship change, that the
/// new ship's package does not bring), or a new ship the base does not
/// sell. A ship good whose hull good the goods list lacks is passed over,
/// where the C++ reads through null (D609).
fn equipment_not_sold_on_base(cx: &Cx<'_>, slot: &PlayerData) -> bool {
    if slot.current_base == 0 {
        return false;
    }
    let data = cx.world.data();
    let Some(base) = data.bases.base(slot.current_base) else {
        return false;
    };
    let ship_unchanged = slot.equip_snapshot_arch == slot.ship_archetype;
    let mut addons: Vec<EquipDesc> = Vec::new();
    if !ship_unchanged {
        let ship_good = base.market.values().find_map(|m| {
            if !sold(m.transaction) {
                return None;
            }
            let good = data.goods.find_by_id(m.good_id)?;
            if good.category != GoodCategory::Ship {
                return None;
            }
            let hull = data.goods.find_by_id(good.hull_good_id)?;
            (hull.ship_arch_id == slot.ship_archetype).then_some(good)
        });
        let Some(ship_good) = ship_good else {
            return true;
        };
        addons.clone_from(&ship_good.addon_low);
    }
    let mut work = slot.equipment.clone();
    if ship_unchanged {
        for item in &slot.equipment {
            if list::find_equipment_item(&slot.equip_snapshot, item.id).is_some() {
                list::remove_equipment_item(&mut work, item.id, -1);
            }
        }
    }
    for item in &work {
        let arch = item.arch_id;
        if !data.archetypes.equipment.contains_key(&arch) {
            return true;
        }
        let good = data.goods.arch_to_good(arch);
        if good != 0 && base.market.get(&good).is_some_and(|m| sold(m.transaction)) {
            continue;
        }
        let accounted = count_arch(&slot.equip_snapshot, arch);
        let on_board = count_arch(&slot.equipment, arch);
        if on_board <= accounted {
            continue;
        }
        if ship_unchanged || count_arch(&addons, arch) <= 0 {
            return true;
        }
    }
    false
}

/// `pub::Market::GetPrice` as the worth check calls it: the price, or 0
/// when the call fails (the C++ starts the price at 0 and ignores the
/// result).
fn price_or_zero(cx: &mut Cx<'_>, base: u32, good: u32) -> f32 {
    crate::bases::price(cx, base, good).unwrap_or(0.0)
}

/// `PlayerData_TotalEquipmentValue`: outside a base 0; else the cash, each
/// item's worth at the base (a combinable good's count times its price,
/// another's count times 30 % of its price, rounded), held under the cash
/// ceiling, plus 80 % of the ship's price.
pub fn total_equipment_value(cx: &mut Cx<'_>, session: u32) -> i32 {
    let Some(slot) = cx.logic.players.get(session) else {
        return 0;
    };
    if slot.current_base == 0 {
        return 0;
    }
    let (base, cash, items, ship) = (
        slot.current_base,
        slot.cash,
        slot.equipment.clone(),
        slot.ship_archetype,
    );
    let mut total = cash;
    for item in &items {
        let good = cx.world.data().goods.arch_to_good(item.arch_id);
        if good == 0 {
            continue;
        }
        let price = price_or_zero(cx, base, good);
        let value = if cx.world.data().goods.arch_is_combinable(item.arch_id) {
            item.count.wrapping_mul(trunc_to_i32(f64::from(price)))
        } else {
            let unit = f64::from(0.3f32) * f64::from(price) + f64::from(f32::from_bits(ROUNDING));
            trunc_to_i32(unit).wrapping_mul(item.count)
        };
        if value > CASH_CEILING || total > CASH_CEILING {
            total = CASH_CEILING;
        } else {
            total = total.wrapping_add(value);
        }
    }
    let mut hull_value = 0;
    if ship & 0x3FFF_FFFF != 0
        && let Some(good) = cx.world.data().goods.find_by_ship_arch(ship).map(|g| g.id)
    {
        let price = price_or_zero(cx, base, good);
        hull_value = trunc_to_i32(f64::from(0.8f32) * f64::from(price));
        if hull_value > CASH_CEILING {
            return CASH_CEILING;
        }
    }
    if total > CASH_CEILING {
        return CASH_CEILING;
    }
    total.wrapping_add(hull_value)
}

/// `PlayerData_HoldOverCapacity`: the cargo the ship carries exceeds its
/// hold.
fn hold_over_capacity(cx: &Cx<'_>, slot: &PlayerData) -> bool {
    let data = cx.world.data();
    let Some(hull) = data.archetypes.ships.get(&slot.ship_archetype) else {
        return false;
    };
    let strings = cx.world.strings();
    let occupied: f64 = slot
        .equipment
        .iter()
        .map(|e| {
            let volume = data.archetypes.equipment.get(&e.arch_id).map(|a| a.volume);
            e.cargo_space_occupied(strings, volume)
        })
        .sum();
    occupied > f64::from(hull.hold_size)
}

/// `PlayerData_CheckForCheats`: never in single player; always once a
/// cheat was reported; never outside a base. Otherwise the first finding
/// (specs 3, not sold 2, worth 1, capacity 4) is reported and the
/// character marked.
pub fn check_for_cheats(
    cx: &mut Cx<'_>,
    session: u32,
    accounts: &AccountsRoot,
    now: LocalTime,
) -> bool {
    if cx.world.config().single_player {
        return false;
    }
    let Some(slot) = cx.logic.players.get(session) else {
        return false;
    };
    if slot.cheat_reported {
        return true;
    }
    if slot.current_base == 0 {
        return false;
    }
    let slot = slot.clone();
    let reason = if equipment_exceeds_ship_specs(cx, &slot) {
        3
    } else if equipment_not_sold_on_base(cx, &slot) {
        2
    } else if total_equipment_value(cx, session) > slot.total_earned {
        1
    } else if hold_over_capacity(cx, &slot) {
        4
    } else {
        return false;
    };
    report_cheat(cx, session, reason, accounts, now);
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.cheat_reported = true;
    }
    true
}

/// The narrow copy of a wide name (`NarrowCopy`: each unit's low byte).
fn narrow(text: &[u16]) -> String {
    text.iter()
        .take_while(|&&u| u != 0)
        .map(|&u| char::from(u as u8))
        .collect()
}

/// `PlayerData_ReportCheat`: the finding is reported, announced from the
/// console to the universe chat, and appended to `cheaters.log` with the
/// local time.
pub fn report_cheat(
    cx: &mut Cx<'_>,
    session: u32,
    reason: usize,
    accounts: &AccountsRoot,
    now: LocalTime,
) {
    let vibe = cx.logic.players.get(session).map_or(-1, |s| s.rep_vibe);
    let account = cx
        .logic
        .players
        .get(session)
        .map(|s| s.account_name.clone())
        .unwrap_or_default();
    let name: Vec<u16> = {
        let (vibes, _) = cx.world.vibes();
        vibes.name(vibe).map_or_else(
            || ">>>BAD   NAME<<<".encode_utf16().collect(),
            |(_, _, display)| display,
        )
    };
    let reason_text = REASONS.get(reason).copied().unwrap_or("");
    let (character, account) = (narrow(&name), narrow(&account));
    cx.logic.reports.push(
        "",
        0,
        format!("LOG: Possible cheating detected ({reason_text}) by {character} ({account})!"),
    );
    let (data, ok) = write_records(
        &[
            RdlRecord::Style(0xd681),
            RdlRecord::Text(cx.logic.cheat_message.clone()),
            RdlRecord::Text(name),
        ],
        0x200,
    );
    if ok {
        crate::chat::submit_chat(cx, crate::chat::CONSOLE, &data, crate::chat::UNIVERSE, -1);
    }
    if let Ok(mut log) = std::fs::OpenOptions::new()
        .append(true)
        .create(true)
        .open(accounts.cheaters_log_path())
    {
        let line = format!(
            "{}/{}/{} {:02}:{:02}:{:02} Possible cheating detected ({reason_text}) by {character}({account})\n",
            now.month, now.day, now.year, now.hour, now.minute, now.second
        );
        let _ = log.write_all(&neuron_core::msvcrt::text_mode_write(line.as_bytes()));
    }
}

/// `LoadCheatMessage`: on a multiplayer server, the announcement prefix
/// from `CheatMessage.txt` next to the executable: up to 80 wide
/// characters, the last of which is cut when it fills the buffer.
#[must_use]
pub fn load_cheat_message(exe_dir: &Path, single_player: bool) -> Vec<u16> {
    let default: Vec<u16> = DEFAULT_MESSAGE.encode_utf16().collect();
    if single_player {
        return default;
    }
    let Ok(bytes) = std::fs::read(exe_dir.join("CheatMessage.txt")) else {
        return default;
    };
    let units: Vec<u16> = bytes
        .as_chunks::<2>()
        .0
        .iter()
        .take(0x50)
        .map(|&p| u16::from_le_bytes(p))
        .collect();
    if units.is_empty() {
        return default;
    }
    // The units replace the start of the default buffer; the text ends at
    // the first NUL, at the count, or at 79 units.
    let mut buffer: Vec<u16> = default;
    buffer.resize(0x50, 0);
    buffer[..units.len()].copy_from_slice(&units);
    let end = if units.len() < 0x4f {
        units.len()
    } else {
        0x4f
    };
    buffer.truncate(end);
    let nul = buffer.iter().position(|&u| u == 0).unwrap_or(buffer.len());
    buffer.truncate(nul);
    buffer
}
