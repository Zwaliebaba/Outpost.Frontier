//! The BaseSupport controller (`Content/BaseSupport.cpp`, TU19; record 0008
//! point 2; work package P3.6e).
//!
//! - **Constructor** (`FBaseSupport::FBaseSupport`, `:210-215`): no
//!   heartbeat, so no `rand()`; the generic bar scripts are read
//!   ([`generic_scripts::load`], `LoadGenericScripts("scripts\\gcs\\genericScripts.ini")`).
//! - **Destructor** (`:217-220`): `UnloadGenericScripts`.
//! - **The base visits** ([`BaseVisit`], `GBaseVisits`, `:487`): per player in
//!   a base, the news made for him and his mission vendor offers.
//! - **Handler** ([`handle_message`], `:271-484`):
//!   - `BaseEnter`: the mission vendor offers (when the player has a ship)
//!     and the news of the base for the player's stage;
//!   - `BaseExit`: the news and the offers dropped, the base's rooms
//!     unloaded;
//!   - `GFObjSelect`: a bar character talked to (its vnpc record, its count;
//!     a rumor told, `RumorHeard`, and the other tellers of the rumor
//!     silenced), then the room's scripts restarted;
//!   - `LocationEnter` and `PlayerMissionChanged`: the room's scripts
//!     restarted;
//!   - `MissionResponse`: an accepted vendor offer starts its random
//!     mission (`CanAccept`, the controller, the group's join, the answer to
//!     the player), an accepted bar offer is taken (`FBarOffer::Accept`);
//!     then the room's scripts restarted.
//!
//! The bar characters themselves are GameSupport's on `BaseEnter` and
//! `BaseExit` ([`bar::on_player_enter`], [`bar::on_player_exit`]); the
//! "ambient" and "costume" parts of a bar character are its fidget script
//! and its costume's gender ([`bar::create`]). BaseSupport has no
//! `LocationExit` case; GameSupport's clears the player's room.

use std::collections::BTreeMap;

use game_sim::protocol::base_props::NewsBroadcastProperties;
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::hash::c_str;
use neuron_core::id::create_id;
use neuron_server::controller::{ControllerId, Priority};

use super::random_mission::vendor::{self, BarOffer, VendorOffer};
use super::tables::news::NewsItem;
use super::{Instance, bar, generic_scripts, players};
use crate::api;
use crate::events::ControllerMessage;
use crate::server::{CreateParams, Cx};

/// `FBaseVisit` (0x1c bytes): a player's visit of a base.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BaseVisit {
    /// `NewsHandles`: `pub::GF::NewsBroadcastCreate`'s.
    pub news: Vec<u32>,
    /// `VendorOffers`.
    pub vendor_offers: Vec<VendorOffer>,
}

/// `GBaseVisits`: the visits by player id.
pub type BaseVisits = BTreeMap<u32, BaseVisit>;

/// `FBaseSupport::FBaseSupport(ControllerId)`: the generic scripts read.
pub fn construct(cx: &mut Cx<'_>, _id: ControllerId) -> Instance {
    generic_scripts::load(cx);
    Instance::BaseSupport
}

/// `FBaseSupport::~FBaseSupport`: `UnloadGenericScripts`.
pub fn destruct(cx: &mut Cx<'_>, _id: ControllerId) {
    cx.logic.content.state.generic_scripts.unload();
}

/// `NewsBroadcastProperties::init` (`Common/common_baseproperties.cpp:352-365`)
/// of a news item's description for `base`: the key's names follow the
/// C++'s fields (`category` the headline, `headline` the body, `text` the
/// logo), the `logo` string the texture and `CreateID` of the `audio`
/// string its id.
#[must_use]
pub fn news_properties(item: &NewsItem, base: u32) -> NewsBroadcastProperties {
    NewsBroadcastProperties {
        key: -1,
        base_id: base,
        flag1: item.breaking,
        flag2: item.autoselect,
        category: item.icon,
        headline: item.category.clone(),
        body: item.headline.clone(),
        logo: item.text.clone(),
        logo_texture: (!item.logo.is_empty()).then(|| item.logo.clone()),
        logo_texture_id: create_id(&item.audio),
    }
}

/// `StartRoomCharacterScripts(State)` (`:228-240`): every character of the
/// player's room restarts its script, the front of the list first. Nothing
/// without the player's base record or a state of the room.
pub fn start_room_character_scripts(cx: &mut Cx<'_>, player: u32) {
    let Some(state) = cx.logic.content.state.players.get(&player) else {
        return;
    };
    let (Some(base), room) = (state.base, state.location) else {
        return;
    };
    for key in bar::room_character_keys(cx, base, room) {
        generic_scripts::start_bar_character_script(cx, base, key);
    }
}

/// `OnRumorTold(State, Teller)` (`:246-267`): every character of the
/// player's room offering the teller's rumor (`Ids[0]`) restarts its
/// script; the others among them first lose their offer.
fn on_rumor_told(cx: &mut Cx<'_>, player: u32, base: u32, teller: u32, rumor: i32) {
    let Some(room) = cx.logic.content.state.players.get(&player).map(|s| s.location) else {
        return;
    };
    for key in bar::room_character_keys(cx, base, room) {
        let same = bar::character(cx, base, key).is_some_and(|c| {
            matches!(c.offer, BarOffer::Rumor(Some(r)) if r.ids[0] == rumor)
        });
        if !same {
            continue;
        }
        if key != teller {
            bar::set_offer(cx, base, key, BarOffer::None);
        }
        generic_scripts::start_bar_character_script(cx, base, key);
    }
}

/// `RefreshMissionVendorOffers(State)` (`:490-497`), after a bribe: the
/// visit's vendor offers released and built again for the player's base.
/// Nothing without a visit.
pub fn refresh_mission_vendor_offers(cx: &mut Cx<'_>, player: u32) {
    let Some(visit) = cx.logic.content.state.base_visits.get_mut(&player) else {
        return;
    };
    let mut offers = std::mem::take(&mut visit.vendor_offers);
    vendor::clear_mission_vendor_offers(cx, &mut offers);
    let base = cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .and_then(|s| s.base)
        .unwrap_or(0);
    let built = vendor::build_mission_vendor_offers(cx, base, player);
    if let Some(visit) = cx.logic.content.state.base_visits.get_mut(&player) {
        visit.vendor_offers = built;
    }
}

/// `BaseEnter` (`:275-309`): a visit of the player with the mission vendor
/// offers, when `GetShipID` gives a ship, and the news of the base whose
/// stage range holds the player's stage (each made for every session); a
/// second visit of the player is dropped as the C++'s insert drops it.
///
/// A player or base without a record, which the C++ dereferences, has no
/// news (D1001).
fn base_enter(cx: &mut Cx<'_>, base: u32, player: u32) {
    let ship = api::player::ship_id(cx, player).unwrap_or(0);
    let mut visit = BaseVisit::default();
    if ship != 0 {
        visit.vendor_offers = vendor::build_mission_vendor_offers(cx, base, player);
    }
    let stage = cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map(|s| s.mission_number as i32);
    if let Some(stage) = stage
        && cx.logic.content.state.bases.contains_key(&base)
    {
        let tables = std::sync::Arc::clone(&cx.logic.content.tables);
        for item in &tables.news {
            if !item.bases.contains(&base) || stage < item.from_stage || stage > item.to_stage {
                continue;
            }
            let handle = api::gf::news_broadcast_create(cx, &news_properties(item, base), 0);
            visit.news.push(handle);
        }
    }
    cx.logic
        .content
        .state
        .base_visits
        .entry(player)
        .or_insert(visit);
}

/// `BaseExit` (`:311-341`): the visit's news destroyed and its vendor
/// offers released, the visit dropped; then the base's rooms unloaded.
///
/// The C++ breaks (`int3`) when the player has no visit and when BasePop
/// still has room characters for him; the port goes on (D1002).
fn base_exit(cx: &mut Cx<'_>, base: u32, player: u32) {
    if let Some(mut visit) = cx.logic.content.state.base_visits.remove(&player) {
        for mut handle in visit.news.drain(..) {
            api::gf::news_broadcast_destroy(cx, &mut handle);
        }
        vendor::clear_mission_vendor_offers(cx, &mut visit.vendor_offers);
    }
    if !cx.logic.content.state.bases.contains_key(&base) {
        return;
    }
    super::bases::unload_base(cx, base);
}

/// `GFObjSelect` (`:343-381`): the player talked to the bar character
/// `object` of his base: the vnpc record and the talk counted; a rumor is
/// added to the player's rumors, `RumorHeard {player, role}` goes to every
/// controller and the rumor's tellers in the room are handled
/// ([`on_rumor_told`]); any other offer restarts the room's scripts.
fn obj_select(cx: &mut Cx<'_>, object: u32, player: u32) {
    let Some(base) = cx.logic.content.state.players.get(&player).and_then(|s| s.base) else {
        return;
    };
    let Some((_, key)) = cx
        .logic
        .content
        .state
        .bases
        .get(&base)
        .and_then(|b| bar::find_bar_character(b, object))
    else {
        return;
    };
    let Some(character) = bar::character_mut(cx, base, key) else {
        return;
    };
    let nickname = character.nickname;
    character.talk_count += 1;
    let offer = character.offer.clone();
    let role = character.role.clone();
    if let Some(state) = cx.logic.content.state.players.get_mut(&player) {
        state.record_vnpc_talk(base, nickname);
    }
    let BarOffer::Rumor(rumor) = offer else {
        start_room_character_scripts(cx, player);
        return;
    };
    // A rumor offer without its record is read through null in the C++
    // (D1001): its id is taken as 0.
    let rumor_id = rumor.map_or(0, |r| r.ids[0]);
    if let Some(state) = cx.logic.content.state.players.get_mut(&player) {
        state.add_rumor(rumor_id as u32);
    }
    let mut role = c_str(&role).to_vec();
    role.truncate(31);
    let mut message = ControllerMessage::RumorHeard { player, role };
    let _ = api::controller::send_message(cx, ControllerId::BROADCAST, &mut message);
    on_rumor_told(cx, player, base, key, rumor_id);
}

/// `MissionResponse` for a vendor offer (`:403-453`): the visit's offer of
/// the id, if any, answered: unless the player's group runs a mission, the
/// loader's `CanAccept`; an acceptable offer becomes the `DestroyMission`
/// controller of its type, the group joins it and the response goes to it
/// as it is. The answer (the reason 0x730 for a group already on a
/// mission) goes to the player; an accepted offer's record is destroyed and
/// its loader released.
///
/// A player without a visit, whose `end()` the C++ reads, has no offer
/// (D1001).
fn vendor_response(cx: &mut Cx<'_>, offer_id: u32, player: u32) {
    let Some(offer) = cx
        .logic
        .content
        .state
        .base_visits
        .get(&player)
        .and_then(|v| v.vendor_offers.iter().find(|o| o.offer_id == offer_id))
        .cloned()
    else {
        return;
    };
    let mut reason = FmtStr::default();
    let mut accept = false;
    if players::group_live_mission(cx, player) == 0 {
        match offer.data.loader.can_accept(cx) {
            Ok(()) => accept = true,
            Err((why, _)) => reason = why,
        }
        if accept && let Some(key) = offer.data.mission {
            let controller = api::controller::create(
                cx,
                b"content.dll",
                &offer.data.kind,
                &CreateParams::RandomMission(key),
                Priority::AboveNormal,
            );
            players::join_mission_as_group(cx, player, controller.0, 0, 0, false, 0);
            let mut forward = ControllerMessage::MissionResponse {
                offer: offer_id,
                kind: 2,
                accepted: true,
                player,
            };
            let _ = api::controller::send_message(cx, controller, &mut forward);
        }
    } else {
        reason = FmtStr::new(0x730);
    }
    api::gf::mission_vendor_acceptance(cx, offer.handle, accept, &reason, player);
    if !accept {
        return;
    }
    let taken = cx
        .logic
        .content
        .state
        .base_visits
        .get_mut(&player)
        .and_then(|v| {
            let at = v
                .vendor_offers
                .iter()
                .position(|o| o.offer_id == offer_id)?;
            Some(v.vendor_offers.remove(at))
        });
    if let Some(mut taken) = taken {
        api::gf::mission_vendor_offer_destroy(cx, &mut taken.handle);
        taken.data.release(cx);
    }
}

/// `MissionResponse` for a bar character's offer (`:454-473`): every
/// character of the player's room with the id whose offer is accepted
/// (`FBarOffer::Accept`, a bribe's vendor offers rebuilt) loses its
/// behaviour and its offer.
///
/// A base or room the C++ reads without a test, missing, has no character
/// (D1001).
fn bar_response(cx: &mut Cx<'_>, character_id: u32, player: u32) {
    let Some((Some(base), room)) = cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map(|s| (s.base, s.location))
    else {
        return;
    };
    for key in bar::room_character_keys(cx, base, room) {
        let Some(c) = bar::character(cx, base, key) else {
            continue;
        };
        if c.character_id != character_id {
            continue;
        }
        let (offer, nickname) = (c.offer.clone(), c.nickname);
        let accepted = vendor::accept(cx, &offer, player, nickname);
        if accepted.refresh_vendor_offers {
            refresh_mission_vendor_offers(cx, player);
        }
        if accepted.accepted {
            bar::set_behavior(cx, base, key, u32::MAX);
            bar::set_offer(cx, base, key, BarOffer::None);
        }
    }
}

/// `FBaseSupport::HandleMessage` (`:271-484`).
pub fn handle_message(cx: &mut Cx<'_>, _id: ControllerId, message: &mut ControllerMessage<'_>) {
    match *message {
        ControllerMessage::BaseEnter { base, player } => base_enter(cx, base, player),
        ControllerMessage::BaseExit { base, player } => base_exit(cx, base, player),
        ControllerMessage::GfObjSelect { object, player } => obj_select(cx, object, player),
        ControllerMessage::LocationEnter { player, .. }
        | ControllerMessage::PlayerMissionChanged { player, .. } => {
            if cx.logic.content.state.players.contains_key(&player) {
                start_room_character_scripts(cx, player);
            }
        }
        ControllerMessage::MissionResponse {
            offer,
            kind,
            accepted,
            player,
        } => {
            if !accepted {
                return;
            }
            match kind {
                2 => vendor_response(cx, offer, player),
                1 => bar_response(cx, offer, player),
                _ => {}
            }
            if cx.logic.content.state.players.contains_key(&player) {
                start_room_character_scripts(cx, player);
            }
        }
        _ => {}
    }
}
