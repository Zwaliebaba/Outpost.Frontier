//! The trades between players on the server (work package P3.10b):
//! `IServerImpl`'s trade slots 79 to 86 (`Server/IServerImpl.cpp:2346-2706`),
//! the `PlayerData` trade work they and the slot's owners call
//! (`PlayerData_TerminateTrade`, `PlayerData_CancelTradeRequest`,
//! `PlayerData_SetTradeRequest`, the trade half of `PlayerData_Teardown`;
//! `Server/server_classes.cpp:1459-1545`, `2390-2431`) and
//! `IServerImpl::TradeResponse` (`:1219`).
//!
//! A trade runs in two steps:
//!
//! - **The request.** `RequestTrade(source, target)` records the source's
//!   one outbound request (`trade_request_partner`) and adds it to the
//!   target's inbound list; the target's client hears of it with the
//!   source's ship. A request back from a player already asked starts the
//!   trade at once. `StopTradeRequest` withdraws it.
//! - **The trade.** `InitiateTrade(source, target)` needs the target to
//!   have asked the source; it gives each player its own side
//!   ([`TradeOffer`]) naming the other, ends the target's request and tells
//!   both clients the other's ship. Each side then sets its money
//!   (`SetTradeMoney`) and adds or removes items (`AddTradeEquip`,
//!   `DelTradeEquip`); any change clears both sides' acceptance and both
//!   clients hear it. `AcceptTrade` marks a side; once both are accepted
//!   the trade settles: each side must hold its money and its items, and
//!   each ship's hold must take what comes in less what goes out, else the
//!   trade ends with reason 3 (money or items) or 5 (room). The money
//!   moves both ways (counted as earned), each item leaves its owner's
//!   cargo and joins the other's cargo bay, both ships' holds follow
//!   their owners' records (`CEqObj::sync_cargo`) and the system's other
//!   players hear of both ships' cargo; the trade ends with reason 1.
//!
//! `TerminateTrade(session, reason)` ends both sides and tells both
//! clients the reason; it also ends the trade of a player who fires, drops
//! cargo or uses an item ([`crate::receive`], reason 0), whose ship is
//! destroyed ([`crate::objects`], reason 0, with its request), who logs
//! out or leaves the character (reason 4). The C++ makes no check of the
//! distance between the ships or of where the players are; the clients
//! do.
//!
//! The C++ keeps each side's `CPlayerTradeOffer` behind a pointer in its
//! slot, each naming the other side's offer; both are made and deleted
//! together, so the other side here is the partner slot's
//! [`PlayerData::trade_offer`](crate::player::PlayerData::trade_offer).

use game_sim::data::equip_desc::{EquipDesc, list};
use game_sim::data::loadout::add_equipment_item;
use game_sim::object::{ObjHandle, class};
use game_sim::protocol::common::PlayerTrade;
use game_sim::protocol::marshal::{GoodDesc, MarshalReader};
use game_sim::protocol::{WireError, to_server::TradeResponse};
use neuron_core::num::trunc_to_i32;
use neuron_server::controller::ControllerId;

use crate::client;
use crate::events::ControllerMessage;
use crate::server::Cx;

/// One player's side of a trade (`CPlayerTradeOffer`, `common_tradeoffer.h`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TradeOffer {
    /// The player whose side it is (`SourcePlayerId`, +4).
    pub source: u32,
    /// The other player (`TargetPlayerId`, +8).
    pub target: u32,
    /// The items offered (+0x10), as an `EquipDescList`.
    pub items: Vec<EquipDesc>,
    /// The money offered (`MoneyOffered`, +0x1c, an `unsigned long`).
    pub money: u32,
    /// The side accepted the trade (`Accepted`, +0x20).
    pub accepted: bool,
}

/// `PlayerDB::is_valid` (`GetPlayerBySession`).
fn valid(cx: &Cx<'_>, session: u32) -> bool {
    cx.logic.players.is_valid(session)
}

/// The slot's ship (`ship_object_id`); 0 without a slot.
fn ship_of(cx: &Cx<'_>, session: u32) -> u32 {
    cx.logic
        .players
        .get(session)
        .map_or(0, |s| s.ship_object_id)
}

/// The other side's acceptance is cleared (`(offer+0xc)->SetAccepted(false)`).
fn clear_partner_acceptance(cx: &mut Cx<'_>, partner: u32) {
    if let Some(offer) = cx
        .logic
        .players
        .get_mut(partner)
        .and_then(|p| p.trade_offer.as_mut())
    {
        offer.accepted = false;
    }
}

// ---------------------------------------------------------------------------
// `PlayerData`'s trade work.

/// `PlayerData_TerminateTrade(slot)` (`server_classes.cpp:1459`): a slot in
/// a trade ends it with reason 0 through its client id. Nothing for a
/// session without a slot, which the C++ indexes past the table (D598).
pub fn player_terminate_trade(cx: &mut Cx<'_>, session: u32) {
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    if slot.trade_offer.is_none() {
        return;
    }
    let client = slot.client_id;
    terminate_trade(cx, client, 0);
}

/// `PlayerData_CancelTradeRequest(slot, drop_partner_entry)`
/// (`server_classes.cpp:1488`): a slot's outbound request is withdrawn:
/// the asked player's client hears it (`StopTradeRequest` with this
/// slot's ship), and with `drop_partner_entry` every entry of this
/// slot's client id leaves a valid partner's inbound list.
pub fn cancel_trade_request(cx: &mut Cx<'_>, session: u32, drop_partner_entry: bool) {
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    let partner = slot.trade_request_partner;
    if partner == 0 {
        return;
    }
    let (client, ship) = (slot.client_id, slot.ship_object_id);
    client::player_trade_stop_request(cx, partner, ship);
    if drop_partner_entry
        && cx.logic.players.is_valid(partner)
        && let Some(p) = cx.logic.players.get_mut(partner)
    {
        p.inbound_trade_requests.retain(|&r| r != client);
    }
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.trade_request_partner = 0;
    }
}

/// `PlayerData_SetTradeRequest(slot, target)` (`server_classes.cpp:1535`):
/// the slot's old request is withdrawn; a target slot with a client then
/// takes this slot's client id in its inbound list, the request is the
/// slot's and the target's client hears it with this slot's ship.
pub fn set_trade_request(cx: &mut Cx<'_>, session: u32, target: u32) {
    cancel_trade_request(cx, session, true);
    let Some((client, ship)) = cx
        .logic
        .players
        .get(session)
        .map(|s| (s.client_id, s.ship_object_id))
    else {
        return;
    };
    let Some(t) = cx
        .logic
        .players
        .get_mut(target)
        .filter(|t| t.client_id != 0)
    else {
        return;
    };
    t.inbound_trade_requests.push(client);
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.trade_request_partner = target;
    }
    client::player_trade_request(cx, target, ship);
}

/// The trade half of `PlayerData_Teardown` (`server_classes.cpp:2390-2431`):
/// the slot's outbound request is withdrawn (the partner's client hears
/// it, every entry of this slot leaves the partner's inbound list); every
/// player that asked this one and still has a request withdraws it (the
/// player it asked, normally this one, hears it with the asker's ship);
/// the inbound list is emptied.
pub fn teardown_requests(cx: &mut Cx<'_>, session: u32) {
    let Some(slot) = cx.logic.players.get(session) else {
        return;
    };
    let partner = slot.trade_request_partner;
    let client = slot.client_id;
    if partner != 0 {
        let ship = ship_of(cx, client);
        client::player_trade_stop_request(cx, partner, ship);
        if let Some(p) = cx.logic.players.get_mut(partner) {
            p.inbound_trade_requests.retain(|&r| r != client);
        }
        if let Some(slot) = cx.logic.players.get_mut(session) {
            slot.trade_request_partner = 0;
        }
    }
    let inbound = cx
        .logic
        .players
        .get(session)
        .map(|s| s.inbound_trade_requests.clone())
        .unwrap_or_default();
    for requester in inbound {
        let Some(r) = cx.logic.players.get(requester) else {
            continue;
        };
        let (asked, requester_client) = (r.trade_request_partner, r.client_id);
        if asked == 0 {
            continue;
        }
        let ship = ship_of(cx, requester_client);
        client::player_trade_stop_request(cx, asked, ship);
        if let Some(r) = cx.logic.players.get_mut(requester) {
            r.trade_request_partner = 0;
        }
    }
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.inbound_trade_requests.clear();
    }
}

// ---------------------------------------------------------------------------
// The `IServerImpl` trade slots.

/// `IServerImpl::InitiateTrade(session, target)` (slot 79,
/// `IServerImpl.cpp:2346`): refused with reason 2 to the source for an
/// invalid target, a target that did not ask the source, or the source
/// itself. A trade the source is in ends with reason 4 first; a target in
/// a trade refuses with reason 6 to the source's client alone. Otherwise
/// each player gets its side naming the other, the target's request ends
/// (`StopTradeRequest`) and each client hears the other's ship.
pub fn initiate_trade(cx: &mut Cx<'_>, session: u32, target: u32) {
    let asked = cx
        .logic
        .players
        .get(target)
        .filter(|_| valid(cx, target))
        .map(|t| t.trade_request_partner);
    if asked != Some(session) || session == target {
        terminate_trade(cx, session, 2);
        return;
    }
    if cx
        .logic
        .players
        .get(session)
        .is_some_and(|p| p.trade_offer.is_some())
    {
        terminate_trade(cx, session, 4);
    }
    if cx
        .logic
        .players
        .get(target)
        .is_some_and(|t| t.trade_offer.is_some())
    {
        client::player_trade_terminate(cx, session, 6);
        return;
    }
    if let Some(p) = cx.logic.players.get_mut(session) {
        p.trade_offer = Some(TradeOffer {
            source: session,
            target,
            ..TradeOffer::default()
        });
    }
    if let Some(t) = cx.logic.players.get_mut(target) {
        t.trade_offer = Some(TradeOffer {
            source: target,
            target: session,
            ..TradeOffer::default()
        });
    }
    stop_trade_request(cx, target);
    let target_ship = ship_of(cx, target);
    client::player_trade_initiate(cx, session, target_ship);
    let own_ship = ship_of(cx, session);
    client::player_trade_initiate(cx, target, own_ship);
}

/// `IServerImpl::TerminateTrade(session, reason)` (slot 80,
/// `IServerImpl.cpp:2400`): for a valid session, both sides of its trade
/// end and the partner's client hears the reason; the session's client
/// hears it in any case.
pub fn terminate_trade(cx: &mut Cx<'_>, session: u32, reason: i32) {
    if !valid(cx, session) {
        return;
    }
    let offer = cx
        .logic
        .players
        .get_mut(session)
        .and_then(|p| p.trade_offer.take());
    if let Some(offer) = offer {
        let partner = offer.target;
        let partner_had_side = partner != 0
            && cx
                .logic
                .players
                .get_mut(partner)
                .and_then(|p| p.trade_offer.take())
                .is_some();
        if partner_had_side {
            client::player_trade_terminate(cx, partner, reason);
        }
    }
    client::player_trade_terminate(cx, session, reason);
}

/// `IServerImpl::AcceptTrade(session, accepted)` (slot 81,
/// `IServerImpl.cpp:2463`): a valid session in a trade marks its side and
/// the partner's client hears it with the session's ship. Once both sides
/// are accepted the trade settles (see the module); a partner without a
/// side, which the C++ reads through null and never has, ends nothing.
pub fn accept_trade(cx: &mut Cx<'_>, session: u32, accepted: bool) {
    if !valid(cx, session) {
        return;
    }
    let Some(offer) = cx
        .logic
        .players
        .get_mut(session)
        .and_then(|p| p.trade_offer.as_mut())
    else {
        return;
    };
    offer.accepted = accepted;
    let partner = offer.target;
    let own_ship = ship_of(cx, session);
    client::player_trade_accept(cx, partner, own_ship, accepted);
    let Some(partner_offer) = cx
        .logic
        .players
        .get(partner)
        .and_then(|p| p.trade_offer.clone())
    else {
        return;
    };
    let Some(offer) = cx
        .logic
        .players
        .get(session)
        .and_then(|p| p.trade_offer.clone())
    else {
        return;
    };
    if !partner_offer.accepted || !offer.accepted {
        return;
    }
    settle(cx, session, partner, &offer, &partner_offer);
}

/// The settlement of `AcceptTrade` once both sides are accepted.
fn settle(cx: &mut Cx<'_>, session: u32, partner: u32, offer: &TradeOffer, theirs: &TradeOffer) {
    let cash = |cx: &Cx<'_>, s: u32| {
        cx.logic.players.get(s).map_or(0, |p| {
            u32::from_ne_bytes(crate::player::cash(p).to_ne_bytes())
        })
    };
    if offer.money > cash(cx, session) || theirs.money > cash(cx, partner) {
        terminate_trade(cx, session, 3);
        return;
    }

    // `Server_FindObject` of each ship (destroyed or not, in transit too),
    // narrowed to a `CShip`.
    let ship = |cx: &Cx<'_>, s: u32| {
        let id = ship_of(cx, s);
        cx.world.find(id).filter(|&h| {
            cx.world
                .get(h)
                .is_some_and(|o| o.is(class::SHIP) && o.equipped.is_some())
        })
    };
    let own_ship = ship(cx, session);
    let partner_ship = ship(cx, partner);

    // Each side's items, against a copy of its owner's list.
    let holds = |cx: &Cx<'_>, s: u32, items: &[EquipDesc]| {
        let goods = &cx.world.data().goods;
        let mut cargo = cx
            .logic
            .players
            .get(s)
            .map(|p| p.equipment.clone())
            .unwrap_or_default();
        items.iter().all(|item| {
            match list::find_matching_cargo(&cargo, goods, item.arch_id, item.count, item.health) {
                Some(at) => {
                    let id = cargo[at].id;
                    list::remove_equipment_item(&mut cargo, id, item.count);
                    true
                }
                None => false,
            }
        })
    };
    if !holds(cx, session, &offer.items) || !holds(cx, partner, &theirs.items) {
        terminate_trade(cx, session, 3);
        return;
    }

    // The room each hold has once the trade is done.
    let own_out = offer_volume(cx, &offer.items);
    let partner_out = offer_volume(cx, &theirs.items);
    let own_free = free_hold(cx, session)
        .wrapping_sub(partner_out)
        .wrapping_add(own_out);
    let partner_free = free_hold(cx, partner)
        .wrapping_sub(own_out)
        .wrapping_add(partner_out);
    if own_free < 0 || partner_free < 0 {
        terminate_trade(cx, session, 5);
        return;
    }

    let as_int = |v: u32| i32::from_ne_bytes(v.to_ne_bytes());
    crate::player::adjust_cash(
        cx,
        session,
        as_int(theirs.money.wrapping_sub(offer.money)),
        true,
    );
    crate::player::adjust_cash(
        cx,
        partner,
        as_int(offer.money.wrapping_sub(theirs.money)),
        true,
    );
    let bay = cx.world.strings().cargo_bay();
    for (from, to, items) in [
        (session, partner, &offer.items),
        (partner, session, &theirs.items),
    ] {
        for item in items {
            crate::player::remove_cargo(cx, from, item.arch_id, item.count, item.health, true);
            crate::player::add_equipment(
                cx,
                to,
                item.arch_id,
                bay,
                item.count,
                item.health,
                0,
                false,
                true,
            );
        }
    }

    // `CEqObj::sync_cargo` on both ships, then the system's other players
    // hear of both. The C++ calls both through null for a ship it does not
    // find and reads the system of one in transit through null (D883).
    for (s, h) in [(session, own_ship), (partner, partner_ship)] {
        if let Some(h) = h {
            let equipment = cx
                .logic
                .players
                .get(s)
                .map(|p| p.equipment.clone())
                .unwrap_or_default();
            cx.world.sync_cargo(h, &equipment);
        }
    }
    if let (Some(own), Some(theirs)) = (own_ship, partner_ship)
        && let Some(system) = system_of(cx, own)
    {
        crate::replication::remote_cargo_update(cx, system, own);
        crate::replication::remote_cargo_update(cx, system, theirs);
    }

    terminate_trade(cx, session, 1);
}

/// The system of a found object (`Server_FindObject`'s system out); none
/// in transit.
fn system_of(cx: &Cx<'_>, h: ObjHandle) -> Option<u32> {
    if cx.world.in_transit(h) {
        return None;
    }
    cx.world.get(h).map(|o| o.physical.system)
}

/// `(int)offer.get_cargo_space_occupied()`.
fn offer_volume(cx: &Cx<'_>, items: &[EquipDesc]) -> i32 {
    trunc_to_i32(f64::from(crate::api::player::cargo_space_occupied(
        cx, items,
    )))
}

/// `(int)((double)(float)(int)hold_size - (double)occupied)`: the room
/// left in the hold of the slot's ship archetype. The C++ reads an unknown
/// archetype through null; here its hold is 0 (D883).
fn free_hold(cx: &Cx<'_>, session: u32) -> i32 {
    let Some(p) = cx.logic.players.get(session) else {
        return 0;
    };
    let hold = cx
        .world
        .data()
        .archetypes
        .ships
        .get(&p.ship_archetype)
        .map_or(0.0, |s| s.hold_size);
    let hold = trunc_to_i32(f64::from(hold)) as f32;
    let occupied = crate::api::player::cargo_space_occupied(cx, &p.equipment);
    trunc_to_i32(f64::from(hold) - f64::from(occupied))
}

/// `IServerImpl::SetTradeMoney(session, money)` (slot 82,
/// `IServerImpl.cpp:2594`): a valid session without a trade ends it with
/// reason 2; otherwise both sides' acceptance is cleared, the money is
/// the side's, and the session's and the partner's clients hear it with
/// the session's ship.
pub fn set_trade_money(cx: &mut Cx<'_>, session: u32, money: u32) {
    let Some(partner) = change_side(cx, session) else {
        return;
    };
    if let Some(offer) = cx
        .logic
        .players
        .get_mut(session)
        .and_then(|p| p.trade_offer.as_mut())
    {
        offer.money = money;
        offer.accepted = false;
    }
    let ship = ship_of(cx, session);
    client::player_trade_money(cx, session, ship, money);
    client::player_trade_money(cx, partner, ship, money);
}

/// The start the three changes share: `None` for an invalid session, and
/// for one without a trade after ending it with reason 2; otherwise the
/// partner, whose acceptance is cleared.
fn change_side(cx: &mut Cx<'_>, session: u32) -> Option<u32> {
    if !valid(cx, session) {
        return None;
    }
    let Some(partner) = cx
        .logic
        .players
        .get(session)
        .and_then(|p| p.trade_offer.as_ref())
        .map(|o| o.target)
    else {
        terminate_trade(cx, session, 2);
        return None;
    };
    clear_partner_acceptance(cx, partner);
    Some(partner)
}

/// `IServerImpl::AddTradeEquip(session, equip)` (slot 83,
/// `IServerImpl.cpp:2622`): as [`set_trade_money`], the item joining the
/// side's list (a combinable good onto its line; a line that matches only
/// by archetype is not added, D115).
pub fn add_trade_equip(cx: &mut Cx<'_>, session: u32, equip: &EquipDesc) {
    let Some(partner) = change_side(cx, session) else {
        return;
    };
    let combinable = cx.world.data().goods.arch_is_combinable(equip.arch_id);
    let strings = cx.world.strings();
    if let Some(offer) = cx
        .logic
        .players
        .get_mut(session)
        .and_then(|p| p.trade_offer.as_mut())
    {
        let _ = add_equipment_item(&mut offer.items, equip, combinable, strings);
        offer.accepted = false;
    }
    let ship = ship_of(cx, session);
    client::player_trade_add_equip(cx, session, ship, equip);
    client::player_trade_add_equip(cx, partner, ship, equip);
}

/// `IServerImpl::DelTradeEquip(session, equip)` (slot 84,
/// `IServerImpl.cpp:2651`): as [`set_trade_money`], the count leaving the
/// side's line that matches the good, count and health.
pub fn del_trade_equip(cx: &mut Cx<'_>, session: u32, equip: &EquipDesc) {
    let Some(partner) = change_side(cx, session) else {
        return;
    };
    let goods = &cx.world.data().goods;
    if let Some(offer) = cx
        .logic
        .players
        .get_mut(session)
        .and_then(|p| p.trade_offer.as_mut())
    {
        if let Some(at) = list::find_matching_cargo(
            &offer.items,
            goods,
            equip.arch_id,
            equip.count,
            equip.health,
        ) {
            let id = offer.items[at].id;
            list::remove_equipment_item(&mut offer.items, id, equip.count);
        }
        offer.accepted = false;
    }
    let ship = ship_of(cx, session);
    client::player_trade_del_equip(cx, session, ship, equip);
    client::player_trade_del_equip(cx, partner, ship, equip);
}

/// `IServerImpl::RequestTrade(session, target)` (slot 85,
/// `IServerImpl.cpp:2682`): for a valid session not in a trade and a
/// valid target, the trade starts when the target asked the session
/// ([`initiate_trade`]); otherwise the request is the session's
/// ([`set_trade_request`]).
pub fn request_trade(cx: &mut Cx<'_>, session: u32, target: u32) {
    if !valid(cx, session) || !valid(cx, target) {
        return;
    }
    if cx
        .logic
        .players
        .get(session)
        .is_some_and(|p| p.trade_offer.is_some())
    {
        return;
    }
    if cx
        .logic
        .players
        .get(target)
        .is_some_and(|t| t.trade_request_partner == session)
    {
        initiate_trade(cx, session, target);
        return;
    }
    set_trade_request(cx, session, target);
}

/// `IServerImpl::StopTradeRequest(session)` (slot 86,
/// `IServerImpl.cpp:2700`): a valid session's client hears reason 0 and
/// its request is withdrawn ([`cancel_trade_request`]).
pub fn stop_trade_request(cx: &mut Cx<'_>, session: u32) {
    if !valid(cx, session) {
        return;
    }
    client::player_trade_terminate(cx, session, 0);
    cancel_trade_request(cx, session, true);
}

/// `DispatchClientPacket`'s `FLPACKET_COMMON_PLAYER_TRADE`
/// (`RemoteClient/RemoteClient_main.cpp:131-178`): the action picks the
/// slot. An added item is a new `EquipDesc` with the archetype, count,
/// health and id; a removed one has no health (1). `StopTradeRequest`
/// takes the packet's target as the session whose request ends, as the
/// client's encoder puts its own session there. An unknown action does
/// nothing.
pub fn deliver_player_trade(cx: &mut Cx<'_>, session: u32, packet: &PlayerTrade) {
    let item = |cx: &Cx<'_>, health: Option<f32>| {
        let mut equip = EquipDesc::new(cx.world.strings());
        equip.arch_id = packet.equip_arch_id;
        equip.count = packet.equip_count;
        if let Some(health) = health {
            equip.health = health;
        }
        equip.id = packet.equip_id;
        equip
    };
    match packet.action {
        0x01 => initiate_trade(cx, session, packet.target_player),
        0x02 => terminate_trade(cx, session, i32::from(packet.reason)),
        0x04 => set_trade_money(cx, session, u32::from_ne_bytes(packet.money.to_ne_bytes())),
        0x08 => {
            let equip = item(cx, Some(packet.equip_health));
            add_trade_equip(cx, session, &equip);
        }
        0x10 => {
            let equip = item(cx, None);
            del_trade_equip(cx, session, &equip);
        }
        0x20 => request_trade(cx, session, packet.target_player),
        0x40 => accept_trade(cx, session, packet.accepted),
        0x80 => stop_trade_request(cx, packet.target_player),
        _ => {}
    }
}

/// `IServerImpl::TradeResponse(data, size, session)` (`IServerImpl.cpp:
/// 1219`): the block is a marshalled `TradeResponseInfo` (two `int`s, a
/// `bool`, the offered and the requested `GoodDescList`), which goes to
/// the bus as `TradeResponse` with the session, unchecked. A block that
/// ends inside it, which the C++ reads past (D101), sends nothing (D880).
pub fn trade_response(cx: &mut Cx<'_>, session: u32, packet: &TradeResponse) {
    let read = || -> Result<_, WireError> {
        let mut reader = MarshalReader::new(&packet.data);
        let field_00 = reader.get_int()?;
        let field_04 = reader.get_int()?;
        let flag_08 = reader.get_bool()?;
        let offered = reader.get_good_desc_list()?;
        let requested = reader.get_good_desc_list()?;
        Ok((field_00, field_04, flag_08, offered, requested))
    };
    let Ok((field_00, field_04, flag_08, offered, requested)) = read() else {
        return;
    };
    let words = |goods: &[GoodDesc]| {
        goods
            .iter()
            .map(|g| {
                [
                    g.good_id,
                    u32::from_ne_bytes(g.count.to_ne_bytes()),
                    g.price.to_bits(),
                    g.field_0c.to_bits(),
                ]
            })
            .collect()
    };
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::TradeResponse {
            field_00,
            field_04,
            flag_08,
            offered: words(&offered),
            requested: words(&requested),
            player: session,
        },
    );
}
