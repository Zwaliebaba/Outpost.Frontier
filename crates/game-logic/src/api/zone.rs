//! `pub::Zone` (`Server/server_pub.cpp:6371-6619`): the universe's zones
//! by id ([`game_sim::data::universe::Universe::zones`], the global map of
//! `Universe::get_zone`), their frame, shape and properties, and the shape
//! queries of [`game_sim::data::zone::Zone`]. Every function looks the zone
//! up first; a miss is reported ([`super::misc::get_zone`]) and answers as
//! the C++ does.
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `VerifyId` | 6375 | [`verify_id`] |
//! | `GetId` | 6384 | [`id`] |
//! | `GetSystem` | 6397 | [`system`] |
//! | `GetPopulation` | 6412 | [`population`] |
//! | `GetPos` | 6430 | [`pos`] |
//! | `GetOrientation` | 6445 | [`orientation`] |
//! | `GetRadius` | 6460 | [`radius`] |
//! | `GetSize` | 6469 | [`size`] |
//! | `GetShape` | 6483 | [`shape`] |
//! | `ClassifyPoint` | 6498 | [`classify_point`] |
//! | `InZone` | 6512 | [`in_zone`] |
//! | `GetDistance` | 6526 | [`distance`] |
//! | `Intersect` | 6540 | [`intersect`] |
//! | `GetProperties` | 6554 | [`properties`] |
//! | `GetLootableInfo` | 6571 | [`lootable_info`] |
//! | `GetName` | 6607 | [`name`] |
//!
//! The C++ returns `ClassifyPoint`'s and `GetDistance`'s float on the x87
//! stack; the port rounds them to `f32` here, where Content stores them.

use game_sim::data::zone::{Zone, ZoneShape};
use neuron_core::math::{Matrix, Vector};

use super::PubError;
use super::misc::{get_zone, get_zone_by_name};
use crate::server::Cx;

/// The zone of `id`, with a miss reported, read through `read`.
fn with_zone<T>(cx: &mut Cx<'_>, id: u32, read: impl FnOnce(&Zone) -> T) -> Option<T> {
    let data = cx.world.data_arc();
    get_zone(&data, &mut cx.logic.reports, id).map(read)
}

/// `VerifyId`: a zone has the id.
pub fn verify_id(cx: &mut Cx<'_>, zone: u32) -> bool {
    with_zone(cx, zone, |_| ()).is_some()
}

/// `GetId`: the id of the zone named `name` (the system is not used), 0
/// for none.
pub fn id(cx: &mut Cx<'_>, system: u32, name: &[u8]) -> u32 {
    let _ = system;
    let data = cx.world.data_arc();
    get_zone_by_name(&data, &mut cx.logic.reports, name).map_or(0, |z| z.nickname)
}

/// `GetSystem`: the zone's system, 0 for an unknown zone.
pub fn system(cx: &mut Cx<'_>, zone: u32) -> u32 {
    with_zone(cx, zone, |z| z.system).unwrap_or(0)
}

/// `GetPopulation`: the zone's faction weights (`faction` lines, the
/// faction's `MakeId` and its weight). The C++ hands out a pointer to the
/// zone's own vector; the port a copy.
///
/// # Errors
///
/// -2 for an unknown zone (the C++ writes null).
pub fn population(cx: &mut Cx<'_>, zone: u32) -> Result<Vec<(u32, f32)>, PubError> {
    with_zone(cx, zone, |z| z.population.clone()).ok_or(PubError::BAD_ID)
}

/// `GetPos`: the zone's position, zero for an unknown zone.
pub fn pos(cx: &mut Cx<'_>, zone: u32) -> Vector {
    with_zone(cx, zone, |z| z.pos).unwrap_or(Vector::ZERO)
}

/// `GetOrientation`.
///
/// # Errors
///
/// -2 for an unknown zone.
pub fn orientation(cx: &mut Cx<'_>, zone: u32) -> Result<Matrix, PubError> {
    with_zone(cx, zone, |z| z.orientation).ok_or(PubError::BAD_ID)
}

/// `GetRadius`: the shape's bounding radius, 0 for an unknown zone.
pub fn radius(cx: &mut Cx<'_>, zone: u32) -> f32 {
    with_zone(cx, zone, Zone::radius).unwrap_or(0.0)
}

/// `GetSize`.
///
/// # Errors
///
/// -2 for an unknown zone.
pub fn size(cx: &mut Cx<'_>, zone: u32) -> Result<Vector, PubError> {
    with_zone(cx, zone, |z| z.size).ok_or(PubError::BAD_ID)
}

/// `GetShape`.
///
/// # Errors
///
/// -2 for an unknown zone (the C++ writes shape 0 then).
pub fn shape(cx: &mut Cx<'_>, zone: u32) -> Result<ZoneShape, PubError> {
    with_zone(cx, zone, |z| z.shape).ok_or(PubError::BAD_ID)
}

/// `ClassifyPoint`: below 1 inside, 1 for an unknown zone.
pub fn classify_point(cx: &mut Cx<'_>, zone: u32, point: Vector) -> f32 {
    with_zone(cx, zone, |z| z.classify_point(point) as f32).unwrap_or(1.0)
}

/// `InZone(point, margin)`: false for an unknown zone.
pub fn in_zone(cx: &mut Cx<'_>, zone: u32, point: Vector, margin: f32) -> bool {
    with_zone(cx, zone, |z| z.in_zone(point, margin)).unwrap_or(false)
}

/// `GetDistance`: the signed distance to the surface (negative inside, not
/// clamped), 0 for an unknown zone.
pub fn distance(cx: &mut Cx<'_>, zone: u32, point: Vector) -> f32 {
    with_zone(cx, zone, |z| z.distance(point) as f32).unwrap_or(0.0)
}

/// `Intersect(start, end)`: where the segment enters the zone; none for an
/// unknown zone. A shape without a clip of its own is reported and clipped
/// as its bounding sphere.
pub fn intersect(cx: &mut Cx<'_>, zone: u32, start: Vector, end: Vector) -> Option<Vector> {
    let data = cx.world.data_arc();
    let z = get_zone(&data, &mut cx.logic.reports, zone)?;
    z.intersect(start, end, &mut cx.logic.reports)
}

/// `GetProperties`: the zone's property flags.
///
/// # Errors
///
/// -2 for an unknown zone.
pub fn properties(cx: &mut Cx<'_>, zone: u32) -> Result<u32, PubError> {
    with_zone(cx, zone, |z| z.property_flags).ok_or(PubError::BAD_ID)
}

/// What `GetLootableInfo` writes: the five fields of the zone's loot
/// record, cleared when it has none.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LootableInfo {
    /// `out_archetype`: the record's `AsteroidLootCommodityId` (+0x08).
    pub archetype: u32,
    /// `out_loot_table`: its `DynamicLootCommodityId` (+0x0c).
    pub loot_table: u32,
    /// `out_min_level`: its `AsteroidLootDifficulty` (+0x20).
    pub min_level: i32,
    /// `out_max_level`: its `AsteroidLootCountMin` (+0x10).
    pub max_level: i32,
    /// The sixth parameter: its `AsteroidLootCountMax` (+0x14).
    pub field6: i32,
}

/// `GetLootableInfo`: the loot record an asteroid field gave the zone, in
/// the C++'s field order (which does not follow the names); all zero when
/// the zone has none.
///
/// # Errors
///
/// -2 for an unknown zone (the outputs are cleared).
pub fn lootable_info(cx: &mut Cx<'_>, zone: u32) -> Result<LootableInfo, PubError> {
    with_zone(cx, zone, |z| {
        z.lootable
            .map_or_else(LootableInfo::default, |l| LootableInfo {
                archetype: l.asteroid_loot_commodity,
                loot_table: l.dynamic_loot_commodity,
                min_level: l.asteroid_loot_difficulty,
                max_level: l.asteroid_loot_count_min,
                field6: l.asteroid_loot_count_max,
            })
    })
    .ok_or(PubError::BAD_ID)
}

/// `GetName`: the zone's `ids_name`.
///
/// # Errors
///
/// -2 for an unknown zone.
pub fn name(cx: &mut Cx<'_>, zone: u32) -> Result<u32, PubError> {
    with_zone(cx, zone, |z| z.ids_name as u32).ok_or(PubError::BAD_ID)
}
