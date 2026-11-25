//! The bar and mission vendor offers of random missions (TU35, which the
//! reconstruction put in `Content/LootProps.cpp:195-535`, and
//! `FBarOffer::Accept`, `Content/BaseDB.cpp:884-943`; work package P3.7c,
//! record 0008 points 8 and 10).
//!
//! BaseSupport holds the offers (P3.6e, [`crate::content::base_support`]):
//! a base visit's vendor offers (`FBaseVisit::VendorOffers`, built on
//! entering the base and cleared on leaving it, `BaseSupport.cpp:288`,
//! `:330`, `:495-496`) and the bar characters' offers
//! (`FBaseNpc::ChooseOffer`, `BaseDB.cpp:1521-1620`,
//! [`crate::content::bar::choose_offer`]). This module stops where
//! BaseSupport calls in: [`build_mission_vendor_offers`] returns the records
//! the visit keeps, [`clear_mission_vendor_offers`] takes them back,
//! [`create_bar_mission_offer`] is what `ChooseOffer` calls, [`accept`] is
//! BaseSupport's `MissionResponse` for a bar character, and a bribe's
//! `RefreshMissionVendorOffers` is left to the caller
//! ([`Accepted::refresh_vendor_offers`],
//! [`crate::content::base_support::refresh_mission_vendor_offers`]).

use game_sim::protocol::base_props::MissionVendorProperties;
use neuron_server::controller::{ControllerId, Priority};

use super::mission::{self, Loader, MissionRequest, result};
use super::{RmDataKey, register_dest_system};
use crate::api;
use crate::content::bases::{BaseNpcKnow, BaseNpcMission, BaseNpcRumor, ContentBase};
use crate::content::{cargo, combat_reputation, players};
use crate::events::ControllerMessage;
use crate::server::{CreateParams, Cx};

/// A `TString<24>`: at most 23 bytes.
fn type_string(kind: &[u8]) -> Vec<u8> {
    kind.iter().copied().take(23).collect()
}

/// `FBarMissionOfferData`: a generated random mission and the loader that
/// made it, which the holder owns.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BarMissionOfferData {
    /// `Loader` (`+0x00`).
    pub loader: Loader,
    /// `Mission` (`+0x04`): the mission `CreateMission` handed out.
    pub mission: Option<RmDataKey>,
    /// `Type` (`+0x08`): the mission type, at most 23 bytes.
    pub kind: Vec<u8>,
}

impl BarMissionOfferData {
    /// `~FBarMissionOffer` and the vendor's release: the loader's empty
    /// slot 5 for the mission, then its `Release`.
    pub fn release(&mut self, cx: &mut Cx<'_>) {
        self.loader.release(cx);
    }
}

/// `CreateBarMissionOffer` (`LootProps.cpp:195-229`): a loader for the
/// mission's type, then up to five attempts at a new difficulty each
/// ([`mission::pick_mission_difficulty`]); a difficulty of 0 gives false at
/// once, and only "no offer" (2) is tried again. True when the last attempt
/// made an offer. The data keeps the loader either way; the caller
/// releases it on false.
pub fn create_bar_mission_offer(
    cx: &mut Cx<'_>,
    npc_mission: &BaseNpcMission,
    player: u32,
    base: u32,
    group: u32,
) -> (bool, BarMissionOfferData) {
    let mut out = BarMissionOfferData {
        kind: type_string(&npc_mission.kind),
        ..BarMissionOfferData::default()
    };
    let mut request = MissionRequest {
        player,
        offer_base: base,
        offer_group: group as i32,
        difficulty: 0.0,
    };
    let mut code = 0;
    for _ in 0..5 {
        request.difficulty = mission::pick_mission_difficulty(
            cx,
            player,
            npc_mission.min_difficulty,
            npc_mission.max_difficulty,
        );
        if request.difficulty == 0.0 {
            return (false, out);
        }
        let (c, key) = out.loader.create_mission(cx, &request);
        code = c;
        out.mission = Some(key);
        if code != result::NO_OFFER {
            break;
        }
    }
    (code == result::OK, out)
}

/// `CollectVendorFactions` (`LootProps.cpp:240-275`): `{faction, weight}`
/// of every `[BaseFaction]` of the base with a mission type, minus those
/// the player's vibe is not friendly (1) or neutral (2) towards.
pub fn collect_vendor_factions(cx: &mut Cx<'_>, base: &ContentBase, vibe: i32) -> Vec<(u32, f32)> {
    let mut items: Vec<(u32, f32)> = base
        .factions
        .iter()
        .filter(|f| !f.mission_types.is_empty())
        .map(|f| (f.group, f.weight))
        .collect();
    let mut i = 0;
    while i < items.len() {
        let mask = cargo::vibe_feeling_mask(cx, items[i].0, vibe);
        if mask == 1 || mask == 2 {
            i += 1;
        } else {
            items.remove(i);
        }
    }
    items
}

/// The weighted pick of `GenerateVendorMissions`: the sum and the running
/// total in double, the roll `(float)(rand() / 32768 * (float)sum)`, the
/// first entry the roll does not pass. A sum of 0 draws nothing.
fn pick_weighted(cx: &mut Cx<'_>, weights: impl Iterator<Item = f32> + Clone) -> Option<usize> {
    let sum: f64 = weights.clone().map(f64::from).sum();
    if sum == 0.0 {
        return None;
    }
    let sum_value = sum as f32;
    let roll = (f64::from(cx.world.rng().rand())
        * f64::from(1.0f32 / 32768.0)
        * f64::from(sum_value)) as f32;
    let mut running = 0.0f64;
    for (index, w) in weights.enumerate() {
        running += f64::from(w);
        if f64::from(roll) <= running {
            return Some(index);
        }
    }
    None
}

/// `GenerateVendorMissions` (`LootProps.cpp:282-422`): up to `count`
/// random missions for the factions, in at most `max(2 * count, 20) + 1`
/// attempts, with the player's system registered for them and every
/// registered system released at the end. Each attempt picks a faction and
/// one of its mission types by weight, then a difficulty; 0 there spends
/// the attempt (reason 2), and so does a `CreateMission` failure (3 for no
/// objective, 4 otherwise). Returns 0 with offers, else the last reason,
/// and the offers.
///
/// A pick that falls past the candidates, where the C++ reads the slot
/// after the last one, spends the attempt (D952).
pub fn generate_vendor_missions(
    cx: &mut Cx<'_>,
    player: u32,
    base: &ContentBase,
    factions: &[(u32, f32)],
    count: i32,
) -> (i32, Vec<BarMissionOfferData>) {
    let mut out = Vec::new();
    let mut reason = 0;
    let mut count = count;
    if !factions.is_empty() {
        let doubled = count.wrapping_mul(2) as u32;
        let max_attempts = if doubled >= 0x14 { doubled } else { 0x14 };
        let mut attempt = 0u32;
        // The player's system record, which the C++ dereferences (D951).
        if let Some(system) = cx
            .logic
            .content
            .state
            .players
            .get(&player)
            .and_then(|s| s.system)
        {
            register_dest_system(cx, system);
        }
        while count != 0 && attempt <= max_attempts {
            attempt += 1;
            let Some(picked) = pick_weighted(cx, factions.iter().map(|f| f.1)) else {
                continue;
            };
            let group = factions[picked].0;
            let Some(faction) = base.find_faction(group) else {
                continue;
            };
            let Some(kind) = pick_weighted(cx, faction.mission_types.iter().map(|t| t.weight))
            else {
                continue;
            };
            let mission_type = &faction.mission_types[kind];
            let mut offer = BarMissionOfferData {
                kind: type_string(&mission_type.kind),
                ..BarMissionOfferData::default()
            };
            let difficulty = mission::pick_mission_difficulty(
                cx,
                player,
                mission_type.min_difficulty,
                mission_type.max_difficulty,
            );
            let mut kept = false;
            if difficulty == 0.0 {
                if reason == 0 {
                    reason = 2;
                }
            } else {
                let request = MissionRequest {
                    player,
                    offer_base: base.base_id,
                    offer_group: group as i32,
                    difficulty,
                };
                let (code, key) = offer.loader.create_mission(cx, &request);
                offer.mission = Some(key);
                if code == result::NO_OBJECTIVE {
                    reason = code;
                } else if code != result::OK {
                    reason = 4;
                }
                if code == result::OK {
                    out.push(offer.clone());
                    count -= 1;
                    kept = true;
                }
            }
            if !kept {
                offer.release(cx);
            }
        }
        mission::release_dest_systems(cx);
    }
    if !out.is_empty() {
        return (0, out);
    }
    (reason, out)
}

/// `FBaseVendorOffer`: a vendor offer a base visit keeps.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VendorOffer {
    /// `VendorHandle`: `MissionVendorOfferCreate`'s.
    pub handle: u32,
    /// `OfferId`: the player's vendor offer id.
    pub offer_id: u32,
    /// `Data`: the mission and its loader.
    pub data: BarMissionOfferData,
}

/// `BuildMissionVendorOffers` (`LootProps.cpp:463-520`): the player's
/// vendor factions at the base; none reports reason 1. Otherwise
/// `num_offers[0] + rand() * (num_offers[1] - num_offers[0] + 1) / 32768`
/// missions are generated, the reason is reported whatever it is, and
/// each mission becomes a vendor offer: its description (the loader's slot
/// 6) with the kind 2 in `SystemId` and the player's next vendor offer id
/// in `TargetId`, created through `pub::GF::MissionVendorOfferCreate`. The
/// records are the visit's (BaseSupport, P3.6e). Nothing for a base or a
/// player without a record.
pub fn build_mission_vendor_offers(cx: &mut Cx<'_>, base: u32, player: u32) -> Vec<VendorOffer> {
    let Some(record) = cx.logic.content.state.bases.get(&base).cloned() else {
        return Vec::new();
    };
    let Some(vibe) = cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map(|s| s.reputation)
    else {
        return Vec::new();
    };
    let factions = collect_vendor_factions(cx, &record, vibe);
    if factions.is_empty() {
        api::gf::report_why_mission_vendor_empty(cx, player, 1);
        return Vec::new();
    }
    let range = record
        .vendor_offers_max
        .wrapping_sub(record.vendor_offers_min)
        .wrapping_add(1);
    let count = cx
        .world
        .rng()
        .rand()
        .wrapping_mul(range)
        .wrapping_div(32768)
        .wrapping_add(record.vendor_offers_min);
    let (reason, offers) = generate_vendor_missions(cx, player, &record, &factions, count);
    api::gf::report_why_mission_vendor_empty(cx, player, reason as u32);
    let mut out = Vec::new();
    for data in offers {
        let mut desc = MissionVendorProperties::default();
        let filter = match data.mission {
            Some(key) => data.loader.offer_description(cx, key, &mut desc),
            None => 0,
        };
        desc.system_id = 2;
        let offer_id = cx
            .logic
            .content
            .state
            .players
            .get_mut(&player)
            .map_or(0, players::PlayerState::allocate_vendor_offer_id);
        desc.target_id = offer_id;
        let handle = api::gf::mission_vendor_offer_create(cx, &desc, filter);
        out.push(VendorOffer {
            handle,
            offer_id,
            data,
        });
    }
    out
}

/// `ClearMissionVendorOffers` (`LootProps.cpp:523-535`): every offer's
/// vendor record destroyed and its loader released, then the list
/// emptied.
pub fn clear_mission_vendor_offers(cx: &mut Cx<'_>, offers: &mut Vec<VendorOffer>) {
    for offer in offers.iter_mut() {
        api::gf::mission_vendor_offer_destroy(cx, &mut offer.handle);
        offer.data.release(cx);
    }
    offers.clear();
}

/// `FBarBribeOffer`: a bar character's bribe.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct BribeOffer {
    /// `Price`.
    pub price: i32,
    /// `Group`.
    pub group: u32,
    /// `Ids`.
    pub ids: [u32; 3],
    /// `Discount`: the reputation change.
    pub discount: f32,
    /// `TextIds`.
    pub text_ids: i32,
}

/// `FBarOffer`: what a bar character offers (`EBarOfferKind`). A mission
/// offer owns its loader: the holder releases it when the offer is dropped
/// ([`crate::content::bar::release_offer`]), where the C++'s reference count
/// reaches 0; a clone is a borrowed view and is not released.
#[derive(Clone, Debug, Default, PartialEq)]
pub enum BarOffer {
    /// 0.
    #[default]
    None,
    /// 1, `FBarBribeOffer`.
    Bribe(BribeOffer),
    /// 2, `FBarMissionOffer`.
    Mission(BarMissionOfferData),
    /// 3, a rumor (`FBaseNpcRumor`; `None` where the C++ holds a null
    /// pointer, which `ChooseOffer` leaves when no rumor is picked).
    Rumor(Option<BaseNpcRumor>),
    /// 4, `FBaseNpcKnow`.
    Know(BaseNpcKnow),
}

impl BarOffer {
    /// `Kind`: the `EBarOfferKind` number.
    #[must_use]
    pub fn kind(&self) -> i32 {
        match self {
            Self::None => 0,
            Self::Bribe(_) => 1,
            Self::Mission(_) => 2,
            Self::Rumor(_) => 3,
            Self::Know(_) => 4,
        }
    }
}

/// What [`accept`] answers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Accepted {
    /// `Accept`'s result.
    pub accepted: bool,
    /// A bribe was paid: the caller rebuilds the player's vendor offers
    /// (`RefreshMissionVendorOffers`, BaseSupport, P3.6e).
    pub refresh_vendor_offers: bool,
    /// The random mission's controller a mission offer started, if any.
    pub mission: ControllerId,
}

/// `FBarOffer::Accept(State, Character)` (`BaseDB.cpp:884-943`) for the
/// player `player` and the character whose nickname is `character`.
/// - Knowledge: false when the cash is short, else paid and the rumor
///   added.
/// - Mission: unless the player's group runs a mission, the
///   `DestroyMission` controller of the offer's type over the mission, the
///   group joined to it (`JoinMissionAsGroup` with the base and the
///   character) and `MissionResponse` `{controller, 0, accepted}` sent to
///   it. True.
/// - Bribe: false when the cash is short, else paid, the reputation moved
///   by the discount, and the vendor offers to be rebuilt.
/// - Rumor and none: true.
///
/// A player without a state, which the C++ dereferences, accepts nothing
/// (D951).
pub fn accept(cx: &mut Cx<'_>, offer: &BarOffer, player: u32, character: u32) -> Accepted {
    let Some(state) = cx.logic.content.state.players.get(&player) else {
        return Accepted {
            accepted: false,
            refresh_vendor_offers: false,
            mission: ControllerId(0),
        };
    };
    let vibe = state.reputation;
    let base = state.base.unwrap_or(0);
    let mut out = Accepted {
        accepted: true,
        refresh_vendor_offers: false,
        mission: ControllerId(0),
    };
    match offer {
        BarOffer::Know(know) => {
            let cash = api::player::inspect_cash(cx, player).unwrap_or(0);
            if (cash as u32) < (know.price as u32) {
                out.accepted = false;
                return out;
            }
            let _ = api::player::adjust_cash(cx, player, know.price.wrapping_neg());
            if let Some(state) = cx.logic.content.state.players.get_mut(&player) {
                state.add_rumor(know.field_0x04 as u32);
            }
        }
        BarOffer::Mission(data) => {
            if players::group_live_mission(cx, player) == 0
                && let Some(key) = data.mission
            {
                let controller = api::controller::create(
                    cx,
                    b"content.dll",
                    &data.kind,
                    &CreateParams::RandomMission(key),
                    Priority::AboveNormal,
                );
                players::join_mission_as_group(cx, player, controller.0, base, character, false, 0);
                let mut message = ControllerMessage::MissionResponse {
                    offer: controller.0,
                    kind: 0,
                    accepted: true,
                    player,
                };
                let _ = api::controller::send_message(cx, controller, &mut message);
                out.mission = controller;
            }
        }
        BarOffer::Bribe(bribe) => {
            let cash = api::player::inspect_cash(cx, player).unwrap_or(0);
            if (cash as u32) < (bribe.price as u32) {
                out.accepted = false;
                return out;
            }
            let _ = api::player::adjust_cash(cx, player, bribe.price.wrapping_neg());
            combat_reputation::apply_faction_reputation_change(
                cx,
                vibe,
                bribe.discount,
                bribe.group,
                true,
            );
            out.refresh_vendor_offers = true;
        }
        BarOffer::Rumor(_) | BarOffer::None => {}
    }
    out
}
