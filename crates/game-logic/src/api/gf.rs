//! `pub::GF` (`Server/server_pub.cpp:5350`, `Server\gf\pubGF.cpp`): the
//! base screens' functions over the base database ([`crate::bases`]) and
//! the costume and universe data.
//!
//! The creators take the base database's property records. The C++ takes
//! Content's descriptions and fills the records from them (`Props.init`);
//! that mapping comes with Content's callers (P3.5, P3.6).
//! `EnumerateCharacterPlacementIni` hands Content each `[CharacterPlacement]`
//! block's reader, which the room file does not keep yet; it comes with its
//! caller too.

use game_sim::ai::script::ScriptBehavior;
use game_sim::data::DataSource;
use game_sim::data::costume::{Costume, nickname_id};
use game_sim::data::room::SetpointProps;
use game_sim::protocol::ToClient;
use game_sim::protocol::base_props::{
    AmbientScriptProperties, CharacterProperties, MissionVendorProperties, NewsBroadcastProperties,
};
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::to_client::GfMissionVendorWhyEmpty;
use neuron_core::id::{create_id, make_location_id};
use neuron_core::math::Vector;

use super::PubError;
use crate::bases;
use crate::server::Cx;

const PUB_GF: &str = "Server\\gf\\pubGF.cpp";
const SETPOINT: &str = "Common\\SetpointProperties.cpp";
const CHAR_DESCRIP: &str = "Common\\CharacterDescriptions.cpp";

/// `FindBase`: the name's id.
#[must_use]
pub fn find_base(name: &[u8]) -> u32 {
    create_id(name)
}

/// `FindLocation`: `MakeLocationID(base, name)`.
#[must_use]
pub fn find_location(base: u32, name: &[u8]) -> u32 {
    make_location_id(base, name)
}

/// `GetSpaceflightLocation`: `CreateID("Spaceflight")`.
#[must_use]
pub fn spaceflight_location() -> u32 {
    create_id(b"Spaceflight")
}

/// `GetBasePosition`: the position of the system object that docks with
/// the base. The C++ keeps the last answer keyed by the base alone, so a
/// second call for the same base answers it whatever the system; the port
/// keeps the same cache ([`crate::server::Logic::gf_base_position`]).
///
/// # Errors
///
/// -2 for a base or system id whose low 30 bits are 0, or when no object
/// of the system docks with the base. The C++ reads through a null system
/// for an id Universe lacks; the port answers -2 there too (D615).
pub fn base_position(
    cx: &mut Cx<'_>,
    source: &dyn DataSource,
    system: u32,
    base: u32,
) -> Result<Vector, PubError> {
    if base & 0x3fff_ffff == 0 || system & 0x3fff_ffff == 0 {
        return Err(PubError::BAD_ID);
    }
    if let Some((cached, pos)) = cx.logic.gf_base_position
        && cached == base
    {
        return Ok(pos);
    }
    let data = cx.world.data_arc();
    let Some(sys) = data.universe.systems.get(&system) else {
        return Err(PubError::BAD_ID);
    };
    let objects =
        data.universe
            .system_objects(source, sys, cx.world.strings(), &mut cx.logic.reports);
    let found = objects.iter().find(|o| o.dock_with == base).map(|o| o.pos);
    match found {
        Some(pos) => {
            cx.logic.gf_base_position = Some((base, pos));
            Ok(pos)
        }
        None => Err(PubError::BAD_ID),
    }
}

/// `GetCostumeByID`: the costume's parts and accessories (a missing costume
/// is reported and gives none).
pub fn costume_by_id(cx: &mut Cx<'_>, id: i32) -> Costume {
    let data = cx.world.data_arc();
    data.costumes.get_costume(id as u32, &mut cx.logic.reports)
}

/// `GetBodyPart`: the body part's id by name (`NicknameToCreateId`), or 0
/// after a report.
pub fn body_part(cx: &mut Cx<'_>, name: &[u8], kind: usize) -> i32 {
    let data = cx.world.data_arc();
    match data.costumes.find_bodypart(kind, nickname_id(name)) {
        Some(part) => part.id as i32,
        None => {
            cx.logic.reports.push(
                PUB_GF,
                0x78,
                format!(
                    "*** ERROR: Cannot find bodypart {}",
                    String::from_utf8_lossy(name)
                ),
            );
            0
        }
    }
}

/// `GetAccessory`: the accessory's id by name, or 0 after a report.
pub fn accessory(cx: &mut Cx<'_>, name: &[u8]) -> i32 {
    let data = cx.world.data_arc();
    match data.costumes.accessories.get(&nickname_id(name)) {
        Some(a) => a.id as i32,
        None => {
            cx.logic.reports.push(
                PUB_GF,
                0x87,
                format!(
                    "*** ERROR: Cannot find accessory {}",
                    String::from_utf8_lossy(name)
                ),
            );
            0
        }
    }
}

/// `GetCostumeSkeletonGender` (`CostumeDescriptions::get_costume_gender`):
/// the sex of the first part that has one, body first, then head and the
/// hands; parts of another sex after it are reported. A part id the
/// descriptions lack is read through a null pointer in the C++; the port
/// skips it (D615).
pub fn costume_skeleton_gender(cx: &mut Cx<'_>, costume: &Costume) -> i32 {
    let data = cx.world.data_arc();
    let ids = [
        costume.part_ids[1],
        costume.part_ids[0],
        costume.part_ids[2],
        costume.part_ids[3],
    ];
    let mut gender = 0;
    for (slot, id) in ids.into_iter().enumerate() {
        if id == 0 {
            continue;
        }
        let Some(part) = data.costumes.find_bodypart(slot, id) else {
            continue;
        };
        if part.sex == 0 {
            continue;
        }
        if gender == 0 {
            gender = part.sex;
        } else if gender != part.sex {
            cx.logic.reports.push(
                CHAR_DESCRIP,
                0x2a1,
                format!(
                    "*** ERROR: Costume body/head/lhand/rhand values {}/{}/{}/{} have inconsistent gender",
                    ids[0], ids[1], ids[2], ids[3]
                ),
            );
        }
    }
    gender
}

/// `CharacterCreate`: the character's handle (0 or -1 when the base
/// database refuses it).
pub fn character_create(cx: &mut Cx<'_>, props: &CharacterProperties, filter: u32) -> u32 {
    bases::create_character(cx, props, filter) as u32
}

/// `CharacterDestroy`: a handle whose low 30 bits are not 0 is destroyed
/// and cleared.
pub fn character_destroy(cx: &mut Cx<'_>, handle: &mut u32) {
    if *handle & 0x3fff_ffff != 0 {
        bases::destroy_character(cx, *handle);
        *handle = 0;
    }
}

/// `CharacterSetBehavior`.
pub fn character_set_behavior(cx: &mut Cx<'_>, character: u32, behavior: u32) {
    bases::set_character_behavior(cx, character, behavior);
}

/// `CharacterBehaviorCreate`.
pub fn character_behavior_create(cx: &mut Cx<'_>, behavior: &ScriptBehavior, filter: u32) -> u32 {
    bases::create_behavior(cx, behavior, filter)
}

/// The destroy wrappers' shape: a handle that is not `0xffffffff` is
/// destroyed and set to it.
fn destroy_handle(cx: &mut Cx<'_>, handle: &mut u32, destroy: fn(&mut Cx<'_>, u32)) {
    if *handle != u32::MAX {
        destroy(cx, *handle);
        *handle = u32::MAX;
    }
}

/// `CharacterBehaviorDestroy`.
pub fn character_behavior_destroy(cx: &mut Cx<'_>, handle: &mut u32) {
    destroy_handle(cx, handle, bases::destroy_behavior);
}

/// The placements of a room the base database knows (none when it has
/// no room file).
fn placements(
    cx: &mut Cx<'_>,
    location: u32,
) -> Option<Vec<game_sim::data::room::CharacterPlacement>> {
    let room = bases::base_db(cx).room(location)?;
    Some(
        room.file
            .as_ref()
            .map(|f| f.placements.clone())
            .unwrap_or_default(),
    )
}

/// `GetNumCharacterPlacements`: 0 for an unknown location.
pub fn num_character_placements(cx: &mut Cx<'_>, location: u32) -> usize {
    placements(cx, location).map_or(0, |p| p.len())
}

/// `GetCharacterPlacementByName`: the first placement whose name equals
/// `name` (`_stricmp`).
///
/// # Errors
///
/// -2 for an unknown location, -3 when no placement has the name (the C++
/// sets the index to -1 then).
pub fn character_placement_by_name(
    cx: &mut Cx<'_>,
    location: u32,
    name: &[u8],
) -> Result<usize, PubError> {
    let list = placements(cx, location).ok_or(PubError::BAD_ID)?;
    list.iter()
        .position(|p| neuron_core::hash::stricmp(&p.name, name).is_eq())
        .ok_or(PubError::NOT_FOUND)
}

/// One placement of a location. The C++ indexes the placement array
/// without a bound; the port answers `None` past its end (D615).
fn placement(
    cx: &mut Cx<'_>,
    location: u32,
    index: usize,
) -> Option<Option<game_sim::data::room::CharacterPlacement>> {
    placements(cx, location).map(|list| list.get(index).cloned())
}

/// `GetCharacterPlacementName`: empty for an unknown location.
pub fn character_placement_name(cx: &mut Cx<'_>, location: u32, index: usize) -> Vec<u8> {
    placement(cx, location, index)
        .flatten()
        .map(|p| p.name)
        .unwrap_or_default()
}

/// `GetCharacterPlacementOccupancy`: the placement's character type, 0 for
/// an unknown location.
pub fn character_placement_occupancy(cx: &mut Cx<'_>, location: u32, index: usize) -> u32 {
    placement(cx, location, index)
        .flatten()
        .map_or(0, |p| p.char_type as u32)
}

/// `GetCharacterPlacementPosture`.
///
/// # Errors
///
/// -2 for an unknown location (and, in the port, a placement past the
/// end, D615).
pub fn character_placement_posture(
    cx: &mut Cx<'_>,
    location: u32,
    index: usize,
) -> Result<u32, PubError> {
    placement(cx, location, index)
        .flatten()
        .map(|p| p.posture as u32)
        .ok_or(PubError::BAD_ID)
}

/// `IsCharacterPlacementNormal`: not a zone script; true for an unknown
/// location.
pub fn is_character_placement_normal(cx: &mut Cx<'_>, location: u32, index: usize) -> bool {
    match placement(cx, location, index) {
        None => true,
        Some(p) => p.is_some_and(|p| !p.zone_script),
    }
}

/// `IsCharacterPlacementSpecial`: a zone script; false for an unknown
/// location.
pub fn is_character_placement_special(cx: &mut Cx<'_>, location: u32, index: usize) -> bool {
    placement(cx, location, index)
        .flatten()
        .is_some_and(|p| p.zone_script)
}

/// `GetCharacterOnPlacement`: 0 for an unknown location.
pub fn character_on_placement(cx: &mut Cx<'_>, location: u32, filter: u32, index: usize) -> u32 {
    bases::character_on_placement(cx, location, filter, index)
}

/// `GetCharacterPlacementProperties`: the properties the placement's name
/// encodes, when they are valid; `None` for an unknown location.
pub fn character_placement_properties(
    cx: &mut Cx<'_>,
    location: u32,
    index: usize,
) -> Option<SetpointProps> {
    let p = placement(cx, location, index).flatten()?;
    let props = SetpointProps::from_name(&p.name, &mut cx.logic.reports);
    props.valid.then_some(props)
}

/// `FormCharacterPlacementName` (`SetpointProperties::combine_to_name`):
/// `Zs|Zg / pc|rpc|npc / segment / zone (two digits) / NAME / posture`, the
/// name upper-cased as the reference's `NameBuffer` holds it;
/// `None` for invalid properties. An unknown occupancy or posture is
/// reported and leaves its part out.
pub fn form_character_placement_name(cx: &mut Cx<'_>, props: &SetpointProps) -> Option<Vec<u8>> {
    if !props.valid {
        return None;
    }
    let mut out: Vec<u8> = if props.zone_script { b"Zs/" } else { b"Zg/" }.to_vec();
    match props.char_type {
        1 => out.extend_from_slice(b"pc/"),
        2 => out.extend_from_slice(b"rpc/"),
        0 => out.extend_from_slice(b"npc/"),
        _ => cx.logic.reports.push(
            SETPOINT,
            0xfe,
            "*** ERROR: Invalid occupancy value for setpoint",
        ),
    }
    out.extend_from_slice(&props.segment);
    out.push(b'/');
    out.extend_from_slice(format!("{:02}/", props.zone).as_bytes());
    // `NameBuffer`: the upper-cased name `set_segment` stored.
    out.extend_from_slice(&props.name_buffer);
    out.push(b'/');
    match props.posture {
        0 => out.extend_from_slice(b"stand"),
        1 => out.extend_from_slice(b"sitlow"),
        2 => out.extend_from_slice(b"sithigh"),
        3 => out.extend_from_slice(b"special"),
        _ => cx.logic.reports.push(
            SETPOINT,
            0x127,
            "*** ERROR: Invalid posture value for setpoint",
        ),
    }
    Some(out)
}

/// `MissionVendorOfferCreate`.
pub fn mission_vendor_offer_create(
    cx: &mut Cx<'_>,
    props: &MissionVendorProperties,
    filter: u32,
) -> u32 {
    bases::create_mission_vendor(cx, props, filter)
}

/// `MissionVendorOfferDestroy`.
pub fn mission_vendor_offer_destroy(cx: &mut Cx<'_>, handle: &mut u32) {
    destroy_handle(cx, handle, bases::destroy_mission_vendor);
}

/// `GetMissionVendorOfferCount`.
pub fn mission_vendor_offer_count(cx: &mut Cx<'_>, base: u32, vendor_type: u32) -> i32 {
    bases::count_mission_vendors(cx, base, vendor_type)
}

/// `MissionVendorAcceptance`.
pub fn mission_vendor_acceptance(
    cx: &mut Cx<'_>,
    handle: u32,
    accepted: bool,
    reason: &FmtStr,
    session: u32,
) {
    bases::mission_vendor_acceptance(cx, handle, accepted, reason, session);
}

/// `ReportWhyMissionVendorEmpty`: `SendServerGFMissionVendorWhyEmpty`, the
/// reason in its byte. Always 0.
pub fn report_why_mission_vendor_empty(cx: &mut Cx<'_>, client: u32, reason: u32) -> i32 {
    bases::trace_event(cx, || bases::DbEvent::WhyEmpty { client, reason });
    cx.logic.client.send(
        client,
        ToClient::Server(
            GfMissionVendorWhyEmpty {
                value: reason as u8,
            }
            .into(),
        ),
    );
    0
}

/// `NewsBroadcastCreate`.
pub fn news_broadcast_create(cx: &mut Cx<'_>, props: &NewsBroadcastProperties, filter: u32) -> u32 {
    bases::create_news_broadcast(cx, props, filter)
}

/// `NewsBroadcastDestroy`.
pub fn news_broadcast_destroy(cx: &mut Cx<'_>, handle: &mut u32) {
    destroy_handle(cx, handle, bases::destroy_news_broadcast);
}

/// `AmbientScriptCreate`.
pub fn ambient_script_create(cx: &mut Cx<'_>, props: &AmbientScriptProperties, filter: u32) -> u32 {
    bases::create_ambient_script(cx, props, filter)
}

/// `AmbientScriptDestroy`.
pub fn ambient_script_destroy(cx: &mut Cx<'_>, handle: &mut u32) {
    destroy_handle(cx, handle, bases::destroy_ambient_script);
}

/// `GetRtcPerformanceSlider`: the client's slider (`RtcSlider::get_slider`)
/// on a server that is not a dedicated multiplayer one, else 1.
#[must_use]
pub fn rtc_performance_slider(cx: &Cx<'_>) -> f32 {
    if cx.logic.multiplayer_server {
        1.0
    } else {
        cx.logic.rtc_slider
    }
}
