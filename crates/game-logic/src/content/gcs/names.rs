//! The voice line names of TU25 (`Content/Gcs.cpp:85-409`, `:661-711`):
//! the `TString<0x40>` helpers, the voice line pick (`PickVoiceLine`,
//! [`voice_line`]) and the names a line speaks for an object, a place, a
//! number, a dock approach and a flee reason.
//!
//! A `TString<0x40>` holds at most 0x3f characters: a copy or a format is
//! clamped ([`t64`]), an append keeps what fits ([`append`]).

use super::super::formations;
use super::super::guides;
use super::super::ships::{self, ShipKey};
use crate::api;
use crate::server::Cx;

/// The longest text a `TString<0x40>` keeps.
const TSTRING_MAX: usize = 0x3f;

/// `MakeTString64` and `FormatTString64`'s copy: clamped to 0x3f bytes.
#[must_use]
pub fn t64(mut text: Vec<u8>) -> Vec<u8> {
    text.truncate(TSTRING_MAX);
    text
}

/// `AppendTString64(Str, Tail)`: the tail clamped to the room left.
pub fn append(text: &mut Vec<u8>, tail: &[u8]) {
    let room = 0x40_usize.saturating_sub(text.len());
    let mut count = tail.len();
    if count >= room {
        count = room.saturating_sub(1);
    }
    text.extend_from_slice(&tail[..count]);
}

/// `MakeVoiceLineName(Name, Suffix, VoiceId)` (`:134-143`): the voice line
/// `<name>_NN<suffix>` of `PickVoiceLine` (one `rand()` when the voice has
/// the name's permutations), empty without the voice or the permutations.
pub fn voice_line(cx: &mut Cx<'_>, name: &[u8], suffix: &[u8], voice: u32) -> Vec<u8> {
    let name = t64(name.to_vec());
    super::super::npc_ships::pick_voice_line(cx, &name, suffix, voice).unwrap_or_default()
}

/// `HasFormationWingmen(Ship)` (`:320-326`): the ship's formation group
/// has more than one member. A ship that is gone has none.
#[must_use]
pub fn has_formation_wingmen(cx: &Cx<'_>, ship: Option<ShipKey>) -> bool {
    let Some(ship) = ship else {
        return false;
    };
    let state = &cx.logic.content.state;
    formations::collect_members(state, ships::formation_id(state, ship)).len() > 1
}

/// The ship's guide type (`FContentNpcShip::GetGuideType`, `NpcShip.cpp:981-986`):
/// 0 without a guide, and for a ship that is no NPC's.
#[must_use]
pub fn guide_type(cx: &Cx<'_>, ship: Option<ShipKey>) -> i32 {
    let Some(ShipKey::Npc(npc)) = ship else {
        return 0;
    };
    cx.logic
        .content
        .state
        .npc_ships
        .get(&npc)
        .and_then(|s| s.guide)
        .map_or(0, |g| guides::guide_type(cx, g))
}

/// The guide's two targets (slot `+0x34`, `GetTargets`); none without a
/// guide.
#[must_use]
pub fn guide_targets(cx: &Cx<'_>, ship: Option<ShipKey>) -> guides::Targets {
    let Some(ShipKey::Npc(npc)) = ship else {
        return guides::Targets::default();
    };
    cx.logic
        .content
        .state
        .npc_ships
        .get(&npc)
        .and_then(|s| s.guide)
        .map_or_else(guides::Targets::default, |g| guides::targets(cx, g))
}

/// `GetFactionMsgIdPrefix(Affiliation, &Out)` and the `"_short"` of the
/// line constructors (`MakeShortFactionName`, `:666-675`): the ship's
/// faction's `msg_id_prefix`, `"gcs_refer_faction_player"` for a faction
/// without properties, then `"_short"`. A ship that is gone, whose handle
/// the C++ reads unchecked, has no faction (D935).
pub fn short_faction(cx: &mut Cx<'_>, ship: Option<ShipKey>) -> Vec<u8> {
    let affiliation = ship.map_or(u32::MAX, |k| ships::affiliation(cx, k));
    let mut name = cx
        .logic
        .content
        .tables
        .faction_props(affiliation)
        .map_or_else(
            || b"gcs_refer_faction_player".to_vec(),
            |p| p.msg_id_prefix.clone(),
        );
    append(&mut name, b"_short");
    name
}

/// The two names of the ship's reputation (`pub::Reputation::GetName`);
/// empty patterns when the call fails.
fn ship_names(
    cx: &mut Cx<'_>,
    ship: ShipKey,
) -> (
    game_sim::protocol::fmtstr::FmtStr,
    game_sim::protocol::fmtstr::FmtStr,
) {
    let vibe = ships::reputation(&cx.logic.content.state, ship);
    api::reputation::name(cx, vibe).unwrap_or_default()
}

/// `MakeObjectReferenceName(Kind, Object, Suffix)` (`:189-286`): the name
/// of a ship of the system.
///
/// - 0: an NPC ship's line of business for a dock hail by its guide type
///   (2 `gcs_dhail_mbid_trader`, 3, 4, 7 `..._patrol`, plural with
///   wingmen), then `_leg` for a lawful faction, else `_ill`;
/// - 1: an NPC's formation designation (`gcs_refer_formationdesig_NN`), a
///   player's `..._01`;
/// - 2: an NPC's designation number (`gcs_misc_number_N`, the suffix
///   appended), a player's `(id - 1) % 20 + 1`;
/// - 3: an NPC's rolled number (`RollNameValue`, one `rand()`), a
///   player's 1, with a dash (`-`).
///
/// A ship that is gone is no NPC's here and no player's: the names a
/// player gets need its state, so it gets none (D935).
pub fn reference_name(
    cx: &mut Cx<'_>,
    kind: i32,
    ship: Option<ShipKey>,
    suffix: Option<&[u8]>,
) -> Vec<u8> {
    let npc = matches!(ship, Some(ShipKey::Npc(_)));
    match kind {
        0 => {
            if !npc {
                return Vec::new();
            }
            let plural = has_formation_wingmen(cx, ship);
            let name: &[u8] = match guide_type(cx, ship) {
                2 if plural => b"gcs_dhail_mbid_traders",
                2 => b"gcs_dhail_mbid_trader",
                3 | 4 | 7 if plural => b"gcs_dhail_mbid_patrollers",
                3 | 4 | 7 => b"gcs_dhail_mbid_patrol",
                _ => return Vec::new(),
            };
            let mut out = name.to_vec();
            let affiliation = ship.map_or(u32::MAX, |k| ships::affiliation(cx, k));
            // The properties are not null-tested; none read as unlawful
            // (D935).
            let lawful = cx
                .logic
                .content
                .tables
                .faction_props(affiliation)
                .is_some_and(|p| p.legality == super::super::tables::factions::legality::LAWFUL);
            append(&mut out, if lawful { b"_leg" } else { b"_ill" });
            out
        }
        1 => {
            let Some(ship) = ship.filter(|_| npc) else {
                return if ship.is_some() {
                    b"gcs_refer_formationdesig_01".to_vec()
                } else {
                    Vec::new()
                };
            };
            let (first, _) = ship_names(cx, ship);
            let (ok, value) = super::super::npc_names::first_name_value(&first);
            if !ok {
                return Vec::new();
            }
            t64(format!(
                "gcs_refer_formationdesig_{:02}",
                value.wrapping_sub(0x304af)
            )
            .into_bytes())
        }
        2 => match ship {
            Some(key @ ShipKey::Npc(_)) => {
                let (first, _) = ship_names(cx, key);
                let (ok, value) = super::super::npc_names::first_name_last_value(&first);
                if !ok {
                    return Vec::new();
                }
                let mut out = format!("gcs_misc_number_{value}").into_bytes();
                if let Some(s) = suffix {
                    out.extend_from_slice(s);
                }
                t64(out)
            }
            Some(ShipKey::Player(player)) => {
                let number = player.wrapping_sub(1) % 20 + 1;
                t64(format!("gcs_misc_number_{number}").into_bytes())
            }
            None => Vec::new(),
        },
        3 => match ship {
            Some(ShipKey::Npc(_)) => {
                let value = cx.world.rng().rand() * 5 / 32768 + 1;
                t64(format!("gcs_misc_number_{value}-").into_bytes())
            }
            Some(ShipKey::Player(_)) => b"gcs_misc_number_1-".to_vec(),
            None => Vec::new(),
        },
        _ => Vec::new(),
    }
}

/// `MakeObjectLocationName(SpaceObjId)` (`:288-318`): the `msg_id_prefix`
/// of the place the object stands for and a dash: its docking target's
/// base record (none without the record), or for a gate or a hole (type
/// `0xc40`) its target system's. Empty, without the dash, for anything
/// else.
pub fn location_name(cx: &mut Cx<'_>, object: u32) -> Vec<u8> {
    let base = api::space_obj::docking_target(cx, object).unwrap_or(0);
    let mut out = Vec::new();
    if base == 0 {
        let object_type = api::space_obj::object_type(cx, object).unwrap_or(0);
        if object_type & 0xc40 == 0 {
            return out;
        }
        let (system, _) = api::space_obj::jump_target(cx, object).unwrap_or((0, 0));
        out = t64(api::misc::translate_system_to_msg_id_prefix(cx, system).unwrap_or_default());
    } else if let Some(record) = cx.logic.content.state.bases.get(&base) {
        out = t64(record.msg_id_prefix.clone());
    }
    append(&mut out, b"-");
    out
}

/// `MakeNumberNames(Number, Suffix, &Out)` (`:328-361`): one
/// `gcs_misc_number_<digit>` per decimal digit, most significant first,
/// the last digit with the suffix; 0 is `gcs_misc_number_0<suffix>`, a
/// negative number nothing.
#[must_use]
pub fn number_names(number: i32, suffix: &[u8]) -> Vec<Vec<u8>> {
    let mut out: Vec<Vec<u8>> = Vec::new();
    if number == 0 {
        let mut name = b"gcs_misc_number_0".to_vec();
        name.extend_from_slice(suffix);
        out.push(t64(name));
        return out;
    }
    let mut number = number;
    let mut first = true;
    while number > 0 {
        let digit = number % 10;
        number /= 10;
        let mut name = format!("gcs_misc_number_{digit}").into_bytes();
        if first {
            append(&mut name, suffix);
            first = false;
        }
        out.insert(0, name);
    }
    out
}

/// `MakeDockApproachName(DockType, Suffix)` (`:363-386`):
/// `gcs_dockrequest_todock` (1), `..._tomoor` (3 to 5), `..._toland` (6)
/// with the suffix; empty for any other type.
#[must_use]
pub fn dock_approach(dock_type: u32, suffix: &[u8]) -> Vec<u8> {
    let name: &[u8] = match dock_type {
        1 => b"gcs_dockrequest_todock",
        3..=5 => b"gcs_dockrequest_tomoor",
        6 => b"gcs_dockrequest_toland",
        _ => return Vec::new(),
    };
    let mut out = name.to_vec();
    out.extend_from_slice(suffix);
    t64(out)
}

/// `MakeFleeReasonName(Reason, VoiceId)` (`:388-403`): the voice line (with
/// a dash) of a flee reason: 0 tough, 2 damaged, 3 no weapons, any other
/// other.
pub fn flee_reason(cx: &mut Cx<'_>, reason: i32, voice: u32) -> Vec<u8> {
    let name: &[u8] = match reason {
        0 => b"gcs_combat_fleereason_tough",
        2 => b"gcs_combat_fleereason_damaged",
        3 => b"gcs_combat_fleereason_noweapons",
        _ => b"gcs_combat_fleereason_other",
    };
    voice_line(cx, name, b"-", voice)
}

/// `MakeFormationVoiceLineName(Ship, Name, Suffix, VoiceId)` (`:681-690`):
/// `<name>_p` with formation wingmen, else `<name>_s`, as a voice line.
pub fn formation_voice_line(
    cx: &mut Cx<'_>,
    ship: Option<ShipKey>,
    name: &[u8],
    suffix: &[u8],
    voice: u32,
) -> Vec<u8> {
    let plural = has_formation_wingmen(cx, ship);
    let mut format = name.to_vec();
    format.extend_from_slice(if plural { b"_p" } else { b"_s" });
    voice_line(cx, &t64(format), suffix, voice)
}

/// `MakeSystemPrefixName(SystemId)` (`:702-710`): the system's
/// `msg_id_prefix` (empty when unknown) and a dash.
pub fn system_prefix(cx: &mut Cx<'_>, system: u32) -> Vec<u8> {
    let mut out = t64(api::misc::translate_system_to_msg_id_prefix(cx, system).unwrap_or_default());
    append(&mut out, b"-");
    out
}

/// A good's `msg_id_prefix` (`pub::TranslateGoodToMsgIdPrefix`, its status
/// ignored).
pub fn good_prefix(cx: &Cx<'_>, good: u32) -> Vec<u8> {
    t64(api::misc::translate_good_to_msg_id_prefix(cx, good).unwrap_or_default())
}

/// A ship archetype's `msg_id_prefix` (`pub::TranslateShipToMsgIdPrefix`,
/// its status ignored).
pub fn ship_prefix(cx: &Cx<'_>, archetype: u32) -> Vec<u8> {
    t64(api::misc::translate_ship_to_msg_id_prefix(cx, archetype).unwrap_or_default())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_are_spoken_digit_by_digit() {
        assert_eq!(number_names(0, b"-"), [b"gcs_misc_number_0-".to_vec()]);
        assert_eq!(
            number_names(21, b"-"),
            [
                b"gcs_misc_number_2".to_vec(),
                b"gcs_misc_number_1-".to_vec()
            ]
        );
        assert!(number_names(-3, b"-").is_empty());
    }

    #[test]
    fn appends_keep_what_fits() {
        let mut s = vec![b'a'; 0x3e];
        append(&mut s, b"xyz");
        assert_eq!(s.len(), 0x3f);
        assert_eq!(
            dock_approach(4, b"_number"),
            b"gcs_dockrequest_tomoor_number"
        );
        assert!(dock_approach(2, b"").is_empty());
    }
}
