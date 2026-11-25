//! The populators, SpacePop and BasePop (`Content/Populators.cpp`; record
//! 0008 point 2). Their handlers, the encounters and the spawn placement
//! are P3.6d's.
//!
//! - **SpacePop** (`FSpacePop::FSpacePop`, `:237-241`): the controller's id
//!   into `GSpacePopControllerId`, then a 3 s heartbeat (one `rand()`). No
//!   destructor.
//! - **BasePop** (`FBasePop::FBasePop`, `:970-977`): a heartbeat of -1 (none;
//!   the bus draws its `rand()` all the same), then the instance into the
//!   process-wide `GBasePopInstance`. No destructor: the pointer is never
//!   cleared (D772). A second BasePop executes `int3` in the C++ (D771).
//!
//! P3.4d brings two pieces the system records need: SpacePop's solar
//! detection flag (`SpacePopSetSolarDetection`, `SpacePopGetSolarDetection`,
//! `:1064-1076`), which a system's full load sets in single player, and
//! the zone lists of a position (`CollectSpacePopZones`, `:661-786`), which
//! the lane rings' attack zones are made from.

// The C++'s `!(x > 0.0f)` tests send NaN to the failing side; the port
// spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use std::collections::BTreeMap;

use neuron_server::controller::ControllerId;

use neuron_core::math::{Matrix, Transform, Vector};

use super::Instance;
use super::systems::SystemInfo;
use crate::api;
use crate::server::Cx;

/// SpacePop's heartbeat period in seconds (`:240`).
pub const SPACE_POP_HEARTBEAT: f32 = 3.0;

/// BasePop's heartbeat period: none (`:973`).
pub const BASE_POP_HEARTBEAT: f32 = -1.0;

/// `FBasePop`'s own state (`:796-807`, 0x1c bytes): the room characters
/// it placed for each player (`PlayerNpcs`), which its handler fills
/// (P3.6d).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BasePop {
    /// `PlayerNpcs`: by player id, the keys of the bar characters placed
    /// for them (the C++ keeps iterators into the base's bar list).
    pub player_npcs: BTreeMap<u32, Vec<u32>>,
}

/// `FSpacePop::FSpacePop(&ControllerId)`.
pub fn construct_space_pop(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    cx.logic.content.state.space_pop = id;
    super::set_heartbeat(cx, id, SPACE_POP_HEARTBEAT);
    Instance::SpacePop
}

/// `FBasePop::FBasePop(ControllerId)`. A second BasePop becomes the
/// instance, as the C++ continues after its breakpoint (D771).
pub fn construct_base_pop(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    super::set_heartbeat(cx, id, BASE_POP_HEARTBEAT);
    cx.logic.content.state.base_pop = Some(id);
    Instance::BasePop(BasePop::default())
}

/// `GBasePopInstance` as the C++ reads it (`BasePopHasPlayer`,
/// `:1043-1049`): the BasePop created last, when its controller still has
/// an instance; a torn-down BasePop reads as none (D772).
#[must_use]
pub fn base_pop<'a>(cx: &'a Cx<'_>) -> Option<&'a BasePop> {
    let content = &cx.logic.content;
    match content.instance(content.state.base_pop?) {
        Some(Instance::BasePop(pop)) => Some(pop),
        _ => None,
    }
}

/// `BasePopHasPlayer(PlayerId)` (`:1043-1049`): the BasePop instance
/// exists and tracks room characters for the player.
#[must_use]
pub fn base_pop_has_player(cx: &Cx<'_>, player: u32) -> bool {
    base_pop(cx).is_some_and(|pop| pop.player_npcs.contains_key(&player))
}

/// `SpacePopSetSolarDetection(Enabled)`: stored in single player only.
pub fn set_solar_detection(cx: &mut Cx<'_>, enabled: bool) {
    if api::misc::single_player(cx) {
        cx.logic.content.state.solar_detection = enabled;
    }
}

/// `SpacePopGetSolarDetection`: the stored flag in single player, else on.
#[must_use]
pub fn solar_detection(cx: &Cx<'_>) -> bool {
    !api::misc::single_player(cx) || cx.logic.content.state.solar_detection
}

/// The zone lists of `CollectSpacePopZones`, each zone by its index in the
/// system record's zones.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SpacePopZones {
    /// The populated zones within 10 of the position: those with a patrol
    /// path, and the smallest of the others at a random place.
    pub at_ship: Vec<usize>,
    /// The `population_additive` zones within 2000 not in the first list.
    pub near: Vec<usize>,
    /// The `population_additive` zones of the view ahead within 4000.
    pub ahead: Vec<usize>,
    /// A zone at the position has density 0: the population is blocked
    /// and the other lists are empty.
    pub blocked: bool,
}

/// `CollectSpacePopZones(System, Placement, ...)` (`:661-786`).
///
/// - **At the ship**: the zones `pub::System::InZones` finds within 10 (at
///   most 20). A populated zone with a patrol path is taken; of the
///   others only the one with the smallest radius (0 for a lane zone; the
///   first on a tie) survives, the rest's ids zeroed in the found list. The
///   survivor goes to a random place of the list (`rand() * (n + 1) /
///   0x8000`).
/// - A zone at the ship with density 0 blocks the population.
/// - **Near**: the zones within 2000 not in the first found list that are
///   populated and `population_additive`.
/// - **Ahead**: the zones of the view frustum (1.2217305 by 0.9670910
///   radians, 4000 deep) in neither found list whose surface is within
///   4000, populated and `population_additive`.
pub fn collect_space_pop_zones(
    cx: &mut Cx<'_>,
    record: &SystemInfo,
    orientation: Matrix,
    position: Vector,
) -> SpacePopZones {
    let mut out = SpacePopZones::default();
    let system = record.system;
    let mut at_ids = api::system::in_zones(cx, system, position, 10.0, 0x14).unwrap_or_default();
    let mut best: Option<usize> = None;
    let mut best_radius = -1.0_f32;
    for index in 0..at_ids.len() {
        let Some(z) = record.find_zone(at_ids[index]) else {
            continue;
        };
        let zone = &record.zones[z];
        if zone.unpopulated {
            continue;
        }
        if zone.path.is_some() {
            out.at_ship.push(z);
            continue;
        }
        let radius = if zone.tradelane {
            0.0
        } else {
            api::zone::radius(cx, zone.zone_id)
        };
        if best_radius == -1.0 || best_radius > radius {
            if let Some(b) = best {
                at_ids[b] = 0;
            }
            best_radius = radius;
            best = Some(index);
        } else {
            at_ids[index] = 0;
        }
    }
    if let Some(b) = best {
        let count = out.at_ship.len() as i32;
        let slot = cx.world.rng().rand().wrapping_mul(count + 1) / 0x8000;
        if let Some(z) = record.find_zone(at_ids[b]) {
            let slot = usize::try_from(slot).unwrap_or(0).min(out.at_ship.len());
            out.at_ship.insert(slot, z);
        }
    }
    if out
        .at_ship
        .iter()
        .any(|&z| record.zones[z].population.density == 0)
    {
        out.blocked = true;
        return out;
    }
    let near_ids = api::system::in_zones(cx, system, position, 2000.0, 0x14).unwrap_or_default();
    for &id in &near_ids {
        if at_ids.contains(&id) {
            continue;
        }
        if let Some(z) = record.find_zone(id) {
            let zone = &record.zones[z];
            if !zone.unpopulated && zone.population.population_additive {
                out.near.push(z);
            }
        }
    }
    let view = Transform {
        rotation: orientation,
        translation: position,
    };
    let angle_x = f32::from_bits(0x3f9c_61aa);
    let angle_y = f32::from_bits(0x3f77_954d);
    let ahead_ids = api::system::in_zones_frustum(cx, system, view, angle_x, angle_y, 4000.0, 0x14)
        .unwrap_or_default();
    for &id in &ahead_ids {
        if at_ids.contains(&id) || near_ids.contains(&id) {
            continue;
        }
        let Some(z) = record.find_zone(id) else {
            continue;
        };
        if !(api::zone::distance(cx, id, position) <= 4000.0) {
            continue;
        }
        let zone = &record.zones[z];
        if zone.unpopulated || !zone.population.population_additive {
            continue;
        }
        out.ahead.push(z);
    }
    out
}

/// `SpacePopAddExclusionSphere` (`Content/Populators.cpp:1161-1184`): a
/// sphere is kept unless one within 0.01 of each coordinate (squared
/// differences at most `0.0001f`) and less than 1 apart in radius is there.
pub fn add_exclusion_sphere(cx: &mut Cx<'_>, sphere: [f32; 4]) {
    let spheres = &mut cx.logic.content.state.exclusion_spheres;
    if spheres.iter().any(|s| same_sphere(s, &sphere)) {
        return;
    }
    spheres.push(sphere);
}

/// `SpacePopRemoveExclusionSphere` (`Content/Populators.cpp:1186-1209`):
/// the first sphere [`add_exclusion_sphere`] would call the same.
pub fn remove_exclusion_sphere(cx: &mut Cx<'_>, sphere: [f32; 4]) {
    let spheres = &mut cx.logic.content.state.exclusion_spheres;
    if let Some(at) = spheres.iter().position(|s| same_sphere(s, &sphere)) {
        spheres.remove(at);
    }
}

/// The sphere test the two share, in `f64` as the x87 runs it.
fn same_sphere(known: &[f32; 4], sphere: &[f32; 4]) -> bool {
    let limit = f64::from(0.0001f32);
    for axis in 0..3 {
        let d = f64::from(known[axis]) - f64::from(sphere[axis]);
        if !(d * d <= limit) {
            return false;
        }
    }
    let d = f64::from(known[3]) - f64::from(sphere[3]);
    d < 1.0 && d > -1.0
}
