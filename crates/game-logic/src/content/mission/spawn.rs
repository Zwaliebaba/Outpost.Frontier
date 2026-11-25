//! The mission object's spawns and the objects it keeps (TU38,
//! `Content/MissionObject.cpp`; `Content/MissionSave.cpp`'s snapshot; work
//! package P3.5c2, record 0008 point 6):
//!
//! - the spawns: [`spawn_ship_group`] (`SpawnMissionShipGroup`,
//!   `MissionObject.cpp:2862-3181`), [`spawn_solar`] and
//!   [`spawn_solar_record`] (`:3184-3459`), [`spawn_loot`] (`:3365-3429`);
//! - the registration of the mission's NPC ships on `NpcShipCreated`
//!   ([`npc_ship_created`]: `RegisterNpc`, `:3474-3499`, the script's
//!   `SubmitNpcPersonality`, `MissionScript.cpp:1977-2034`, and the mission
//!   object's case, `MissionObject.cpp:1869-1912`, with the random
//!   encounters' arms of `AttachRandEncNpc`, `:2633-2789`);
//! - the jumpers (`OnShipJump`, `UpdateJumpers`, `PlaceJumpersAtLaunch`,
//!   `:2345-2506`), the respawns (`RespawnMissionObjects`, `:2508-2561`),
//!   the snapshot (`SnapshotMissionObjects` and `SnapshotVibeInfos`,
//!   `MissionSave.cpp:223-680`) and `ApplyVibeInfos` (`:682-700`);
//! - the mission object's `ObjectDestroyed` case (`MissionObject.cpp:1693-1803`)
//!   and the lookups the actions share (`GetMissionObjectName`,
//!   `MarkMissionObject`, `FindNpcById`, `FindSolarById`, `FindLootById`,
//!   `GetLootId`).
//!
//! The C++ keeps the mission's NPC ships by pointer; the port keeps their
//! keys in Content's arena and looks them up again. An NPC ship that
//! SpaceSupport deleted before the mission hears its `ObjectDestroyed`
//! (SpaceSupport comes first on the bus) is read from freed memory by the
//! C++; the port reads what the mission noted of it when it registered it
//! (its space object and nickname, D979).
//!
//! The name builder's gender and voice picks and the reputation handles of
//! a mission's named NPCs are the P3.6c follow-up's (`npc_names`); the
//! space costume pick, which only the mission spawns use, is here.
//!
//! The C++'s comparisons are kept as written (a NaN fails an x87 `FCOMP`).
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use std::collections::BTreeMap;

use game_sim::ai::personality::Personality;
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Transform, Vector, transform_xform};
use neuron_server::controller::ControllerId;

use super::MissionScript;
use super::obj_list::ObjListDirective;
use super::object::{Jumper, MissionLoot, MissionObject, MissionShip, ShipGroup};
use super::object_save::{RandEncRespawn, VibeInfo};
use super::rand_enc::{MISSION_RANDOM_ENCOUNTER, RandEncArm, ShipChoice};
use super::runtime::{
    self, ID_MASK, find_npc_by_name, label_object_id, player_state, script, script_mut,
};
use crate::api;
use crate::api::ai::{BaseOp, CallbackArg, Op, SetPersonalityParams};
use crate::api::space_obj::{CargoDesc, EqInfo, LootInfo, SolarInfo};
use crate::content::directives::{self, DirectiveList};
use crate::content::guides::GuideDesc;
use crate::content::npc_names::{self, NpcNameParams};
use crate::content::npc_ships::{self, NpcShipParams};
use crate::content::ships::ShipKey;
use crate::content::tables::voices::find_voice_properties;
use crate::content::{dock_queues, formations, systems};
use crate::events::{ControllerMessage, NpcShipKey};
use crate::server::Cx;

/// `GIdLocalObjective`: `CreateID("LOCAL_OBJECTIVE")`, the objective list
/// a snapshot gives a ship whose own directives it keeps.
pub const LOCAL_OBJECTIVE: &[u8] = b"LOCAL_OBJECTIVE";

/// `GPlayerLabel`.
pub const PLAYER_LABEL: &[u8] = b"Player";

/// `CreateID("no_ol")`: a spawn action's object list that keeps the
/// record's.
pub const NO_OBJECT_LIST: &[u8] = b"no_ol";

fn object<'c>(cx: &'c Cx<'_>, id: ControllerId) -> Option<&'c MissionObject> {
    script(cx, id).map(|s| &s.object)
}

fn object_mut<'c>(cx: &'c mut Cx<'_>, id: ControllerId) -> Option<&'c mut MissionObject> {
    script_mut(cx, id).map(|s| &mut s.object)
}

/// The mission player's system record, when the player has a state and
/// the record exists (`PlayerState->GetSystemRecord()`).
#[must_use]
pub fn player_system(cx: &Cx<'_>, id: ControllerId) -> Option<u32> {
    let s = script(cx, id)?;
    player_state(cx, s)?
        .system
        .filter(|system| cx.logic.content.state.systems.contains_key(system))
}

/// The mission player's ship, 0 without a state or a ship.
#[must_use]
pub fn player_ship_id(cx: &Cx<'_>, id: ControllerId) -> u32 {
    script(cx, id)
        .and_then(|s| player_state(cx, s))
        .and_then(|p| p.ship.as_ref())
        .map_or(0, |s| s.ship)
}

/// `GetLocation`'s position and orientation, zero and identity when it
/// fails (the C++'s outputs are not written then; the port's start there).
fn location(cx: &Cx<'_>, object: u32) -> (Vector, Matrix) {
    api::space_obj::location(cx, object).unwrap_or((Vector::ZERO, Matrix::IDENTITY))
}

// ---- the placements -------------------------------------------------------------------------

/// `PlaceFacingAnchor(Anchor, Heading, Range, Out)` (`ContentDataPath.cpp:37-98`):
/// the point `range` from the anchor at `heading`, facing away from it.
#[must_use]
pub fn place_facing_anchor(anchor: &Transform, heading: f32, range: f32) -> Transform {
    let (h, r) = (f64::from(heading), f64::from(range));
    let offset = Vector::new((h.cos() * r) as f32, 0.0, (-(h.sin() * r)) as f32);
    let position = transform_xform(anchor, offset);
    let mut fx = (f64::from(position.x) - f64::from(anchor.translation.x)) as f32;
    let mut fy = (f64::from(position.y) - f64::from(anchor.translation.y)) as f32;
    let mut fz = (f64::from(position.z) - f64::from(anchor.translation.z)) as f32;
    let (dx, dy, dz) = (f64::from(fx), f64::from(fy), f64::from(fz));
    let inverse = 1.0 / ((dy * dy + dz * dz) + dx * dx).sqrt();
    fx = (dx * inverse) as f32;
    fy = (dy * inverse) as f32;
    fz = (dz * inverse) as f32;
    let zero = 0.0_f64;
    let y_zero = f64::from(fy) * zero;
    let mut rx = (f64::from(fz) - y_zero) as f32;
    let mut ry = (f64::from(fx) * zero - f64::from(fz) * zero) as f32;
    let mut rz = (y_zero - f64::from(fx)) as f32;
    let (ex, ey, ez) = (f64::from(rx), f64::from(ry), f64::from(rz));
    let inverse = 1.0 / ((ez * ez + ey * ey) + ex * ex).sqrt();
    rx = (ex * inverse) as f32;
    ry = (ey * inverse) as f32;
    rz = (ez * inverse) as f32;
    let (fx64, fy64, fz64) = (f64::from(fx), f64::from(fy), f64::from(fz));
    let (rx64, ry64, rz64) = (f64::from(rx), f64::from(ry), f64::from(rz));
    let ux = (-(fz64 * ry64 - fy64 * rz64)) as f32;
    let uy = (-(rz64 * fx64 - fz64 * rx64)) as f32;
    let uz = (-(fy64 * rx64 - ry64 * fx64)) as f32;
    Transform::new(
        Matrix::from_rows([[rx, ux, fx], [ry, uy, fy], [rz, uz, fz]]),
        position,
    )
}

/// `IsPosEmpty` then, when the place is taken, `GetEmptyPos` from
/// `base` in a 3D volume with `radius` as start and clearance; a positive
/// radius only (`FCOMP 0.0f`, greater falls through).
fn empty_place(cx: &mut Cx<'_>, system: u32, base: &Transform, radius: f32) -> Vector {
    if !(radius > 0.0) {
        return base.translation;
    }
    let empty = api::space_obj::is_pos_empty(cx, system, base.translation, radius).unwrap_or(true);
    if empty {
        return base.translation;
    }
    api::space_obj::empty_pos(cx, system, base, radius, radius, 3).unwrap_or(base.translation)
}

// ---- the ship spawn ---------------------------------------------------------------------------

/// The `[ObjList]` of `list` as counted objects, made from the parsed list
/// the first time it is handed out; `None` for an unknown list.
pub fn obj_list_refs(cx: &mut Cx<'_>, id: ControllerId, list: u32) -> Option<DirectiveList> {
    let object = object(cx, id)?;
    if let Some(refs) = object.obj_list_refs.get(&list) {
        return Some(refs.clone());
    }
    let parsed = object.obj_lists.get(&list)?.clone();
    let arena = &mut cx.logic.content.state.directives;
    let refs: DirectiveList = parsed
        .into_iter()
        .map(|d| create_directive(arena, d))
        .collect();
    object_mut(cx, id)?.obj_list_refs.insert(list, refs.clone());
    Some(refs)
}

/// A counted object holding `directive` (one reference, the holder's).
fn create_directive(
    arena: &mut directives::DirectiveArena,
    directive: ObjListDirective,
) -> directives::DirectiveRef {
    let key = arena.create(directive.kind);
    if let Some(d) = arena.get_mut(key) {
        d.name = directive.name;
    }
    key
}

/// Releases the mission's counted `[ObjList]` objects (the map's
/// `FMissionDirectiveList`s, `ObjLists.clear()` of `EndMission`).
pub fn release_obj_lists(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(object) = object_mut(cx, id) else {
        return;
    };
    let lists = std::mem::take(&mut object.obj_list_refs);
    for (_, mut list) in lists {
        directives::clear_list(&mut cx.logic.content.state.directives, &mut list);
    }
}

/// `SpawnMissionShipGroup(Group)` (`MissionObject.cpp:2862-3181`): one NPC
/// ship parameter block per ship slot of `group` that names an `[NPC]`
/// (its reputation from a generated name or its `individual_name`, the
/// mission's or `npcships.ini`'s archetype, its voice and costume or the
/// faction's picks, the slot's place, labels, cargo, priority and hit
/// points, the `[ObjList]` or `[MetaBehavior]` it starts with), laid out
/// by the record's formation and made in the record's system or the
/// player's (`CreateNpcShips`) with the `arrival_obj` solar as dock target.
#[allow(clippy::too_many_lines)]
pub fn spawn_ship_group(cx: &mut Cx<'_>, id: ControllerId, group: &ShipGroup) {
    let mut placement = Transform::new(group.orientation, group.position);
    let mut no_formation = false;
    let mut blocks: Vec<NpcShipParams> = Vec::new();
    let tables = cx.logic.content.tables.clone();
    for (index, ship) in group.ships.iter().enumerate() {
        let Some(npc) = object(cx, id)
            .and_then(|o| o.npcs.get(&ship.npc_id))
            .cloned()
        else {
            continue;
        };
        let mut params = NpcShipParams::default();
        let mut name = NpcNameParams::default();
        if ship.random_name {
            if npc.voice == 0 {
                let _ = npc_names::pick_npc_name_gender(cx, npc.affiliation, &mut name);
            } else {
                match find_voice_properties(&tables.voice_properties, npc.voice) {
                    Some(v) if v.supports_roles(1) => name.gender = 1,
                    Some(v) if v.supports_roles(2) => name.gender = 2,
                    _ => {
                        let _ = npc_names::pick_npc_name_gender(cx, npc.affiliation, &mut name);
                    }
                }
            }
            let _ = npc_names::build_default_npc_name(cx, npc.affiliation, &mut name, false);
            params.reputation = Some(npc_names::create_npc_name_reputation(cx, &name));
        } else {
            params.reputation = Some(npc_names::create_msn_npc_reputation(
                cx,
                npc.individual_name.cast_unsigned(),
                npc.affiliation,
            ));
        }
        params.nickname = ship.nickname.clone();
        let arch = object(cx, id)
            .and_then(|o| o.npc_ship_archs.get(&npc.npc_ship_arch))
            .or_else(|| tables.npc_ship(npc.npc_ship_arch))
            .cloned();
        let Some(arch) = arch else {
            npc_ships::release_params(cx, params);
            continue;
        };
        // `ApplyNpcShipArch(Params, Arch, null, null)` (`NpcShip.cpp:155-202`).
        npc_ships::apply_npc_ship_arch(cx, &mut params, &arch, None, 0);
        params.voice = npc.voice;
        params.costume = npc.appearance.clone();
        if npc.appearance.part_ids[0] & ID_MASK == 0 || npc.voice & ID_MASK == 0 {
            let props = tables.faction_props(npc.affiliation);
            if let Some(props) = props {
                if npc.appearance.part_ids[0] & ID_MASK == 0
                    && let Some(costume) = npc_names::pick_space_costume(cx, props, name.gender)
                {
                    params.costume = costume;
                }
                if npc.voice & ID_MASK == 0
                    && let Some(voice) =
                        npc_names::pick_voice(cx, props, name.gender.cast_unsigned())
                {
                    params.voice = voice;
                }
            }
        }

        // The slot's place.
        let mut local = Transform::IDENTITY;
        if group.rel_pos {
            let anchor_object = if eq_ignore_case(c_str(&group.rel_pos_obj), PLAYER_LABEL) {
                let ship = player_ship_id(cx, id);
                (ship != 0).then_some(ship)
            } else {
                script(cx, id)
                    .and_then(|s| find_npc_by_name(cx, s, &group.rel_pos_obj))
                    .map(|key| npc_ships::space_obj(cx, key))
            };
            let mut anchor = Transform::IDENTITY;
            if let Some(object) = anchor_object {
                let (position, orientation) = location(cx, object);
                anchor = Transform::new(orientation, position);
            }
            if group.has_orientation {
                let (h, r) = (
                    f64::from(group.rel_pos_heading),
                    f64::from(group.rel_pos_range),
                );
                let offset = Vector::new((h.cos() * r) as f32, 0.0, (-(h.sin() * r)) as f32);
                local.translation = transform_xform(&anchor, offset);
            } else {
                local = place_facing_anchor(&anchor, group.rel_pos_heading, group.rel_pos_range);
            }
        } else {
            local = Transform::new(group.orientation, group.position);
        }
        if let Some(system) = player_system(cx, id) {
            local.translation = empty_place(cx, system, &local, ship.radius);
        }
        if index == 0 {
            placement = local;
        }

        params.position = local.translation;
        params.orientation = local.rotation;
        params.creation_type = 0;
        params.controller = id.0;
        params.field_0x118 = ship.label_ids.clone();
        params.field_0x110 = group.field_0xe0;
        params.cargo = ship
            .cargo
            .iter()
            .map(|&(good, count)| CargoDesc {
                good,
                count: count.cast_unsigned(),
            })
            .collect();
        params.priority = ship.priority;
        params.hit_points = ship.hit_points;

        // The first slot of that nickname takes this slot's jumper flag.
        if let Some(named) = object_mut(cx, id).and_then(|o| o.find_ship_mut(&params.nickname)) {
            named.jumper = ship.jumper;
        }
        if let Some(list) = obj_list_refs(cx, id, ship.init_objectives) {
            let arena = &mut cx.logic.content.state.directives;
            directives::clear_list(arena, &mut params.directives);
            params.directives = directives::copy_list(arena, &list);
        }
        let meta = object(cx, id)
            .and_then(|o| o.meta_behaviors.get(&ship.init_meta_behavior))
            .map(|list| list.first().cloned());
        if let Some(first) = meta {
            directives::clear_list(
                &mut cx.logic.content.state.directives,
                &mut params.directives,
            );
            // The C++ reads the first object without an empty test; an
            // empty list gives no guide here (D980).
            params.guide = first.map(GuideDesc::from_meta);
        }
        blocks.push(params);
    }

    if group.formation != 0 {
        if tables.formation(group.formation).is_some() {
            dock_queues::place_formation_blocks(cx, &mut blocks, Some(group.formation), &placement);
        } else {
            no_formation = true;
        }
    }

    let mut dock_target = group.arrival_obj_id;
    if !c_str(&group.arrival_obj).is_empty() {
        dock_target = script(cx, id).map_or(0, |s| runtime::solar_id(cx, s, &group.arrival_obj));
    }
    let system = if group.system != 0 {
        Some(group.system).filter(|s| cx.logic.content.state.systems.contains_key(s))
    } else {
        player_system(cx, id)
    };
    if let Some(system) = system
        && !no_formation
    {
        for params in &blocks {
            let _ = npc_ships::create_in_system(cx, system, params, dock_target);
        }
    }
    for params in blocks {
        npc_ships::release_params(cx, params);
    }
}

// ---- the solar and loot spawns ---------------------------------------------------------------

/// `SpawnMissionSolar(Name, Placement)` (`MissionObject.cpp:3432-3458`):
/// every `[MsnSolar]` named `name` (`_stricmp`) is made, at `placement`
/// when given (its own transform kept and restored); the last result.
pub fn spawn_solar(
    cx: &mut Cx<'_>,
    id: ControllerId,
    name: &[u8],
    placement: Option<&Transform>,
) -> bool {
    let mut result = false;
    let count = object(cx, id).map_or(0, |o| o.solars.len());
    for index in 0..count {
        let Some(solar) = object(cx, id).and_then(|o| o.solars.get(index)) else {
            break;
        };
        if !eq_ignore_case(c_str(&solar.nickname), c_str(name)) {
            continue;
        }
        let saved = (solar.position, solar.orientation);
        if let Some(p) = placement
            && let Some(solar) = object_mut(cx, id).and_then(|o| o.solars.get_mut(index))
        {
            solar.position = p.translation;
            solar.orientation = p.rotation;
        }
        result = spawn_solar_record(cx, id, index);
        if let Some(solar) = object_mut(cx, id).and_then(|o| o.solars.get_mut(index)) {
            solar.position = saved.0;
            solar.orientation = saved.1;
        }
    }
    result
}

/// `SpawnMissionSolarRecord(Solar)` (`MissionObject.cpp:3184-3362`): the
/// solar of `[MsnSolar]` `index` in the player's system, its id `GetNicknameId`
/// of its nickname, moved to an empty spot when its place is taken, its
/// reputation allocated from its name with its faction, the faction's
/// costume and voice picks when it has none; then the `NOTHING` state graph
/// with its pilot, its labels, `[Invulnerables]`, `[Decloaked]`,
/// `[MarkObj]` and the system record's mission objects. False without a
/// player or a system record, or when the solar is not made.
#[allow(clippy::too_many_lines)]
pub fn spawn_solar_record(cx: &mut Cx<'_>, id: ControllerId, index: usize) -> bool {
    let Some(system) = player_system(cx, id) else {
        return false;
    };
    let Some(record) = object(cx, id).and_then(|o| o.solars.get(index)).cloned() else {
        return false;
    };
    let player = object(cx, id).map_or(0, |o| o.player);
    let space_obj = api::misc::nickname_id(c_str(&record.nickname));
    if let Some(solar) = object_mut(cx, id).and_then(|o| o.solars.get_mut(index)) {
        solar.space_obj_id = space_obj;
    }
    // The gender slot of the faction's picks (D981): baseline's stack dword
    // holds the system record's pointer, or 3 after `GetEmptyPos`; a
    // faction with voices overwrites it. Neither matches a costume's gender.
    let mut gender_slot = -1_i32;
    let mut position = record.position;
    if record.radius > 0.0 {
        let empty = api::space_obj::is_pos_empty(cx, system, record.position, record.radius)
            .unwrap_or(true);
        if !empty {
            gender_slot = 3;
            let base = Transform::new(record.orientation, record.position);
            position =
                api::space_obj::empty_pos(cx, system, &base, record.radius, record.radius, 3)
                    .unwrap_or(record.position);
        }
    }
    let mut info = SolarInfo::new(
        system,
        record.archetype_id,
        c_str(&record.nickname),
        position,
    );
    info.loadout = record.loadout_id;
    info.vibe = record.field_0x458;
    info.orientation = record.orientation;
    info.docking_base = record.base_id;
    info.visit = record.visit;
    info.costume = record.space_costume.clone();
    info.voice = record.voice_id;
    info.vibe =
        api::reputation::alloc(cx, &FmtStr::new(record.name), &FmtStr::new(0)).unwrap_or(info.vibe);
    let _ = api::reputation::set_affiliation(cx, info.vibe, record.faction);
    if record.space_costume.part_ids[0] & ID_MASK == 0 || record.voice_id & ID_MASK == 0 {
        let tables = cx.logic.content.tables.clone();
        if let Some(props) = tables.faction_props(record.faction) {
            let gender = npc_names::pick_gender(cx, props).unwrap_or(gender_slot);
            if record.space_costume.part_ids[0] & ID_MASK == 0
                && let Some(costume) = npc_names::pick_space_costume(cx, props, gender)
            {
                info.costume = costume;
            }
            if record.voice_id & ID_MASK == 0
                && let Some(voice) = npc_names::pick_voice(cx, props, gender.cast_unsigned())
            {
                info.voice = voice;
            }
        }
    }
    if api::space_obj::create_solar(cx, &info).is_err() {
        return false;
    }
    let state_graph = cx
        .world
        .data()
        .state_graphs
        .find(b"NOTHING", game_sim::ai::state_graph::GraphType::Leader);
    let op = SetPersonalityParams {
        state_graph,
        content_callback: CallbackArg::Null,
        directive_callback: CallbackArg::Null,
        set_personality: record.pilot,
        personality: if record.pilot {
            record.personality
        } else {
            Personality::default()
        },
    };
    let _ = api::ai::submit_state(
        cx,
        space_obj,
        &BaseOp::new(Op::SetPersonalityParams(Box::new(op))),
    );
    let Some(object) = object_mut(cx, id) else {
        return true;
    };
    if let Some(solar) = object.solars.get_mut(index) {
        solar.spawned = 1;
    }
    for &label in &record.label_ids {
        object
            .formation_members
            .entry(label)
            .or_default()
            .push(space_obj);
    }
    let invulnerables: Vec<_> = object
        .invulnerables
        .iter()
        .filter(|i| c_str(&i.nickname) == c_str(&record.nickname))
        .cloned()
        .collect();
    let decloaked = object
        .decloaked
        .iter()
        .filter(|n| c_str(n) == c_str(&record.nickname))
        .count();
    let marked = object
        .mark_obj_names
        .iter()
        .any(|n| c_str(n) == c_str(&record.nickname));
    for i in invulnerables {
        let _ = api::space_obj::set_invincible(cx, space_obj, true, i.param3, i.duration);
    }
    for _ in 0..decloaked {
        let _ = api::space_obj::activate_equipment(cx, space_obj, &cloak_info(false));
    }
    if marked {
        let _ = api::player::mark_obj(cx, player, space_obj, 1);
    }
    systems::add_mission_object(cx, system, space_obj);
    true
}

/// `EQInfo {Type 2, EquipId on}`: the cloak.
fn cloak_info(on: bool) -> EqInfo {
    EqInfo {
        kind: 2,
        on,
        count: 0,
        target: 0,
    }
}

/// `SpawnMissionLoot(Loot)` (`MissionObject.cpp:3365-3429`): the loot of
/// `loot` in the player's system (at its anchor's position plus the
/// offset when it has `rel_pos_obj`; nothing when the anchor is not
/// found), then its labels and `[Invulnerables]`. The record given is the
/// one the spawn writes into (the mission's own or an action's copy).
pub fn spawn_loot(cx: &mut Cx<'_>, id: ControllerId, loot: &mut MissionLoot) {
    let Some(system) = player_system(cx, id) else {
        return;
    };
    loot.system_id = system;
    loot.can_ai_tractor = false;
    loot.mission = loot.cannot_jettison;
    let mut position = loot.position;
    if loot.rel_pos {
        let mut anchor = 0;
        if player_ship_id(cx, id) != 0
            && let Some(s) = script(cx, id)
        {
            anchor = runtime::label_ship_id(cx, s, &loot.rel_pos_obj);
            if anchor & ID_MASK == 0 {
                anchor = loot_id(s, &loot.rel_pos_obj);
                if anchor & ID_MASK == 0 {
                    anchor = runtime::solar_id(cx, s, &loot.rel_pos_obj);
                }
            }
        }
        if anchor & ID_MASK == 0 {
            return;
        }
        let Ok((at, _)) = api::space_obj::location(cx, anchor) else {
            return;
        };
        position = Vector::new(
            at.x + loot.rel_pos_offset.x,
            at.y + loot.rel_pos_offset.y,
            at.z + loot.rel_pos_offset.z,
        );
        loot.position = position;
    }
    let info = LootInfo {
        system,
        contents: loot.equip_arch_id,
        name_id: loot.name_id,
        owner: loot.owner_obj_id,
        velocity: loot.velocity,
        position,
        orientation: loot.orientation,
        angular_velocity: loot.angular_velocity,
        units: loot.units,
        hit_pts_scale: loot.hit_pts_scale,
        can_ai_tractor: loot.can_ai_tractor,
        mission: loot.mission,
        mark_player: loot.mark_for_player,
    };
    let Ok(space_obj) = api::space_obj::create_loot(cx, &info) else {
        return;
    };
    loot.space_obj_id = space_obj;
    loot.spawned = 1;
    let Some(object) = object_mut(cx, id) else {
        return;
    };
    for &label in &loot.label_ids {
        object
            .formation_members
            .entry(label)
            .or_default()
            .push(space_obj);
    }
    let invulnerables: Vec<_> = object
        .invulnerables
        .iter()
        .filter(|i| c_str(&i.nickname) == c_str(&loot.nickname))
        .cloned()
        .collect();
    for i in invulnerables {
        let _ = api::space_obj::set_invincible(cx, space_obj, true, i.param3, i.duration);
    }
}

/// `GetLootId` (`MissionObject.cpp:2113-2122`): the spawned loot of that
/// nickname (`strcmp`), 0 when none.
#[must_use]
pub fn loot_id(s: &MissionScript, label: &[u8]) -> u32 {
    s.object
        .loot
        .iter()
        .find(|l| c_str(&l.nickname) == c_str(label))
        .map_or(0, |l| l.space_obj_id)
}

/// `DestroyMissionObjects` (`MissionObject.cpp:3461-3470`): every
/// `[MsnSolar]` unspawned, then the player's system record destroys its
/// mission objects.
pub fn destroy_mission_objects(cx: &mut Cx<'_>, id: ControllerId) {
    if let Some(object) = object_mut(cx, id) {
        for solar in &mut object.solars {
            solar.spawned = 0;
        }
    }
    if let Some(system) = player_system(cx, id) {
        systems::destroy_mission_objects(cx, system);
    }
}

// ---- the lookups ------------------------------------------------------------------------------

/// `FindNpcById`: the live mission NPC with that space object.
#[must_use]
pub fn find_npc_by_id(cx: &Cx<'_>, id: ControllerId, object: u32) -> Option<NpcShipKey> {
    let key = *self::object(cx, id)?.npc_objects.get(&object)?;
    cx.logic
        .content
        .state
        .npc_ships
        .contains_key(&key)
        .then_some(key)
}

/// `GetMissionObjectName` (`MissionObject.cpp:2232-2274`): an NPC's
/// nickname, a solar's, `Player` for the player's ship, a loot's; empty.
#[must_use]
pub fn mission_object_name(cx: &Cx<'_>, id: ControllerId, object: u32) -> Vec<u8> {
    if let Some(key) = find_npc_by_id(cx, id, object)
        && let Some(n) = cx.logic.content.state.npc_ships.get(&key)
    {
        return c_str(&n.nickname).to_vec();
    }
    let Some(o) = self::object(cx, id) else {
        return Vec::new();
    };
    if let Some(s) = o.solars.iter().find(|s| s.space_obj_id == object) {
        return c_str(&s.nickname).to_vec();
    }
    let Some(s) = script(cx, id) else {
        return Vec::new();
    };
    let Some(state) = player_state(cx, s) else {
        return Vec::new();
    };
    if state.ship.as_ref().is_some_and(|p| p.ship == object) {
        return PLAYER_LABEL.to_vec();
    }
    o.loot
        .iter()
        .find(|l| l.space_obj_id == object)
        .map_or_else(Vec::new, |l| c_str(&l.nickname).to_vec())
}

/// `MarkMissionObject(Object, Mode, Name)` (`MissionObject.cpp:2277-2315`):
/// `pub::Player::MarkObj`, then mode 0 drops the name from `[MarkObj]`,
/// mode 1 adds it once; another mode answers false. True without a player.
pub fn mark_mission_object(
    cx: &mut Cx<'_>,
    id: ControllerId,
    object: u32,
    mode: i32,
    name: &[u8],
) -> bool {
    let Some(s) = script(cx, id) else { return true };
    if player_state(cx, s).is_none() {
        return true;
    }
    let player = s.object.player;
    let mut result = api::player::mark_obj(cx, player, object, mode).is_ok();
    let Some(o) = object_mut(cx, id) else {
        return result;
    };
    let listed = o
        .mark_obj_names
        .iter()
        .position(|n| c_str(n) == c_str(name));
    match mode {
        0 => {
            if let Some(at) = listed {
                o.mark_obj_names.remove(at);
            }
        }
        1 => {
            if listed.is_none() {
                o.mark_obj_names.push(c_str(name).to_vec());
            }
        }
        _ => result = false,
    }
    result
}

// ---- NpcShipCreated ---------------------------------------------------------------------------

/// The script's `NpcShipCreated` case for one of its own ships
/// (`MissionScript.cpp:1280-1290`): `RegisterNpc` and
/// `SubmitNpcPersonality(npc, 0)`; the engine dispatches the trigger event
/// after it.
pub fn register_and_submit(cx: &mut Cx<'_>, id: ControllerId, key: NpcShipKey) {
    register_npc(cx, id, key);
    submit_npc_personality(cx, key, 0);
}

/// `RegisterNpc` (`MissionObject.cpp:3474-3499`): the ship into the
/// mission's NPC map (an id already there keeps its entry), then its
/// `[Invulnerables]` and `[Decloaked]` rows.
fn register_npc(cx: &mut Cx<'_>, id: ControllerId, key: NpcShipKey) {
    let Some(n) = cx.logic.content.state.npc_ships.get(&key) else {
        return;
    };
    let (ship, nickname) = (n.base.ship, n.nickname.clone());
    let Some(o) = object_mut(cx, id) else { return };
    o.npc_objects.entry(ship).or_insert(key);
    o.npc_names.entry(ship).or_insert_with(|| nickname.clone());
    let invulnerables: Vec<_> = o
        .invulnerables
        .iter()
        .filter(|i| c_str(&i.nickname) == c_str(&nickname))
        .cloned()
        .collect();
    let decloaked = o
        .decloaked
        .iter()
        .filter(|d| c_str(d) == c_str(&nickname))
        .count();
    for i in invulnerables {
        let _ = api::space_obj::set_invincible(cx, ship, true, i.param3, i.duration);
    }
    for _ in 0..decloaked {
        let _ = api::space_obj::activate_equipment(cx, ship, &cloak_info(false));
    }
}

/// `FMissionScript::SubmitNpcPersonality(Npc, Mode)` (`MissionScript.cpp:1977-2034`):
/// the ship's personality, its job's flee and scene fields cleared (mode
/// 0) or taken from its pilot (mode 1), submitted again with its state
/// graph and both callbacks.
pub fn submit_npc_personality(cx: &mut Cx<'_>, key: NpcShipKey, mode: i32) {
    let Some(n) = cx.logic.content.state.npc_ships.get(&key) else {
        return;
    };
    let (flid, callback, archetype, own_pilot) =
        (n.base.ship, n.callback, n.base.archetype, n.field_0x1cc);
    let Some(mut personality) = api::ai::personality(cx, flid) else {
        return;
    };
    if mode == 0 {
        let job = &mut personality.job;
        job.unknown_00 = 0;
        job.scene_toughness_threshold = 4;
        job.loot_preference = 0;
        job.flee_scene_threat_style = 4;
        job.flee_when_hull_damaged_percent = 0.0;
        job.flee_when_leader_flees_style = 0;
        job.flee_no_weapons_style = 0;
    } else if mode == 1 {
        let tables = cx.logic.content.tables.clone();
        // The ship's own pilot (`+0x1cc`), else the pilot of the first
        // `npcships.ini` entry of its archetype (`FindNpcShipArchByShip`);
        // nothing loads without one.
        let mut from = Personality::default();
        let loaded = if own_pilot == 0 {
            tables
                .npc_ships
                .values()
                .find(|a| a.ship_archetype == archetype)
                .is_some_and(|a| tables.pilots.personality(a.pilot, &mut from))
        } else {
            tables.pilots.personality(own_pilot, &mut from)
        };
        if loaded {
            let job = &mut personality.job;
            job.flee_scene_threat_style = from.job.flee_scene_threat_style;
            job.flee_when_hull_damaged_percent = from.job.flee_when_hull_damaged_percent;
            job.flee_when_leader_flees_style = from.job.flee_when_leader_flees_style;
            job.flee_no_weapons_style = from.job.flee_no_weapons_style;
            job.scene_toughness_threshold = from.job.scene_toughness_threshold;
            job.unknown_00 = from.job.unknown_00;
            job.loot_preference = from.job.loot_preference;
        }
    }
    let op = SetPersonalityParams {
        state_graph: api::ai::state_graph_id(cx, flid),
        content_callback: CallbackArg::Set(callback),
        directive_callback: CallbackArg::Set(callback),
        set_personality: true,
        personality,
    };
    let _ = api::ai::submit_state(
        cx,
        flid,
        &BaseOp::new(Op::SetPersonalityParams(Box::new(op))),
    );
}

/// The mission object's `NpcShipCreated` case for one of its own ships
/// (`MissionObject.cpp:1869-1912`): the `[MarkObj]` mark, a jumper slot's
/// entry, the random encounter's arm (`AttachRandEncNpc`), and every label
/// of the ship into the label map.
pub fn npc_ship_created(cx: &mut Cx<'_>, id: ControllerId, key: NpcShipKey) {
    let Some(n) = cx.logic.content.state.npc_ships.get(&key) else {
        return;
    };
    let (ship, nickname) = (n.base.ship, n.nickname.clone());
    let Some(s) = script(cx, id) else { return };
    let player = s.object.player;
    let marked = s
        .object
        .mark_obj_names
        .iter()
        .any(|m| c_str(&nickname) == c_str(m));
    if marked {
        let _ = api::player::mark_obj(cx, player, ship, 1);
    }
    let jumper = object(cx, id)
        .and_then(|o| o.find_ship(&nickname))
        .is_some_and(|slot| slot.jumper);
    if jumper && let Some(o) = object_mut(cx, id) {
        o.jumpers.push(Jumper { npc: key, state: 0 });
    }
    attach_rand_enc_npc(cx, id, key);
    let labels = cx
        .logic
        .content
        .state
        .npc_ships
        .get(&key)
        .map(|n| n.field_0x17c.clone())
        .unwrap_or_default();
    if let Some(o) = object_mut(cx, id) {
        for label in labels {
            o.formation_members.entry(label).or_default().push(ship);
        }
    }
}

/// `AttachRandEncNpc` (`MissionObject.cpp:2633-2789`): a ship carrying a
/// random encounter's id joins the arm of its formation (or a new arm
/// `"<encounter>_frm<n>"`), its life becomes 24 s; once every ship of the
/// encounter is made, with an arrival object the arms' ships form one
/// formation picked by weight, and `EncounterLaunched` goes to the mission.
fn attach_rand_enc_npc(cx: &mut Cx<'_>, id: ControllerId, key: NpcShipKey) {
    let Some(n) = cx.logic.content.state.npc_ships.get(&key) else {
        return;
    };
    let (labels, ship, formation_id) = (n.field_0x17c.clone(), n.base.ship, n.base.formation_id);
    let Some(o) = object(cx, id) else { return };
    let Some(at) = o
        .rand_encs
        .iter()
        .position(|e| labels.contains(&e.nickname_id))
    else {
        return;
    };
    if let Some(n) = cx.logic.content.state.npc_ships.get_mut(&key) {
        n.life_time = 24.0;
    }
    let Some(o) = object_mut(cx, id) else { return };
    let enc = &mut o.rand_encs[at];
    let arm_id = if let Some(arm) = enc
        .arms
        .iter_mut()
        .find(|a| a.formation_ref == formation_id)
    {
        arm.members.push(ship);
        arm.id
    } else {
        let name = format!(
            "{}_frm{}",
            String::from_utf8_lossy(c_str(&enc.nickname)),
            enc.formation_index
        );
        let arm_id = create_id(name.as_bytes());
        enc.arms.push(RandEncArm {
            name: arm_id,
            formation_ref: formation_id,
            id: arm_id,
            members: vec![ship],
        });
        enc.formation_index += 1;
        arm_id
    };
    enc.npc_count += 1;
    let (count, spawned, dock_target, encounter) = (
        enc.npc_count,
        enc.params.as_ref().map_or(0, |p| p.ships_spawned),
        enc.params.as_ref().map_or(0, |p| p.dock_target),
        enc.nickname_id,
    );
    let arm_ids: Vec<u32> = enc.arms.iter().map(|a| a.id).collect();
    let weights: Vec<(Option<u32>, f32)> = enc
        .params
        .as_ref()
        .map(|p| {
            p.formations
                .iter()
                .map(|f| (f.formation, f.weight))
                .collect()
        })
        .unwrap_or_default();
    if let Some(n) = cx.logic.content.state.npc_ships.get_mut(&key) {
        n.field_0x17c.push(arm_id);
    }
    if spawned.cast_unsigned() != count {
        return;
    }
    if dock_target & ID_MASK != 0 {
        let members: Vec<ShipKey> = object(cx, id).map_or_else(Vec::new, |o| {
            arm_ids
                .iter()
                .flat_map(|a| o.formation_members.get(a).into_iter().flatten())
                .filter_map(|m| o.npc_objects.get(m).copied())
                .map(ShipKey::Npc)
                .collect()
        });
        if members.is_empty() {
            formations::remove_from_formation(cx, ShipKey::Npc(key));
        } else {
            let mut total = 0.0_f32;
            for &(_, w) in &weights {
                total = (f64::from(total) + f64::from(w)) as f32;
            }
            let mut picked = None;
            if total != 0.0 {
                let roll = (f64::from(cx.world.rng().rand())
                    * f64::from(1.0_f32 / 32768.0)
                    * f64::from(total)) as f32;
                let mut sum = 0.0_f32;
                for &(formation, w) in &weights {
                    sum = (f64::from(sum) + f64::from(w)) as f32;
                    if roll <= sum {
                        picked = Some(formation);
                        break;
                    }
                }
            }
            // Nothing picked: baseline passes the list sentinel's value
            // (D982); the port passes no formation.
            let mut list = members;
            formations::form_formation(cx, &mut list, picked.flatten(), false);
        }
    }
    let encounters = [encounter];
    let _ = cx.dispatch(
        id,
        &mut ControllerMessage::EncounterLaunched {
            encounters: &encounters,
        },
    );
    if let Some(o) = object_mut(cx, id) {
        o.rand_encs[at].npc_count = 0;
    }
}

/// `FindRandEncFormationNpc` (`MissionObject.cpp:2061-2094`): the first
/// arm named `label` of any random encounter, its first ship that is a
/// mission NPC and leads or flies alone.
#[must_use]
pub fn find_rand_enc_formation_npc(
    cx: &Cx<'_>,
    s: &MissionScript,
    label: &[u8],
) -> Option<NpcShipKey> {
    let name = create_id(c_str(label));
    for enc in &s.object.rand_encs {
        for arm in enc.arms.iter().filter(|a| a.name == name) {
            for member in &arm.members {
                let Some(&key) = s.object.npc_objects.get(member) else {
                    continue;
                };
                let Some(n) = cx.logic.content.state.npc_ships.get(&key) else {
                    continue;
                };
                if n.base.formation_slot == 0 || n.base.formation_id == u32::MAX {
                    return Some(key);
                }
            }
        }
    }
    None
}

/// `RemoveRandEncMember` (`MissionObject.cpp:2586-2611`).
fn remove_rand_enc_member(o: &mut MissionObject, ship: u32) {
    for enc in &mut o.rand_encs {
        for at in 0..enc.arms.len() {
            let Some(m) = enc.arms[at].members.iter().position(|&x| x == ship) else {
                continue;
            };
            enc.arms[at].members.remove(m);
            if enc.arms[at].members.is_empty() {
                enc.arms.remove(at);
            }
            break;
        }
        if enc.arms.is_empty() {
            enc.formation_index = 0;
        }
    }
}

// ---- ObjectDestroyed ---------------------------------------------------------------------------

/// The mission object's `ObjectDestroyed` case (`MissionObject.cpp:1693-1803`)
/// up to its failure tests, which the caller runs: the NPC's slot gets its
/// jumper flag back, its jumper entry and arm go, single player's system
/// record forgets the object, the NPC leaves the mission (deleted when it
/// is in no system) or a solar or loot record is marked destroyed, and its
/// labels go.
pub fn object_destroyed(cx: &mut Cx<'_>, id: ControllerId, object: u32, system: u32) {
    let Some(o) = self::object(cx, id) else {
        return;
    };
    let listed = o.npc_objects.get(&object).copied();
    let name = o.npc_names.get(&object).cloned();
    if listed.is_some()
        && let Some(name) = name
        && let Some(o) = object_mut(cx, id)
        && let Some(slot) = o.find_ship_mut(&name)
    {
        slot.jumper = slot.original_jump_flag;
    }
    let jumper_ships: Vec<u32> = self::object(cx, id)
        .map(|o| {
            o.jumpers
                .iter()
                .map(|j| jumper_ship(cx, o, j.npc))
                .collect()
        })
        .unwrap_or_default();
    let single_player = api::misc::single_player(cx);
    let record = player_system(cx, id);
    let Some(o) = object_mut(cx, id) else { return };
    if let Some(at) = jumper_ships.iter().position(|&s| s == object) {
        o.jumpers.remove(at);
    }
    remove_rand_enc_member(o, object);
    if single_player && let Some(record) = record {
        systems::remove_mission_object(cx, record, object);
    }
    let Some(o) = object_mut(cx, id) else { return };
    if let Some(key) = o.npc_objects.remove(&object) {
        o.npc_names.remove(&object);
        if system & ID_MASK == 0 {
            npc_ships::destroy(cx, key);
        }
    } else if let Some(solar) = o.solars.iter_mut().find(|s| s.space_obj_id == object) {
        solar.spawned = 2;
        solar.space_obj_id = 0;
    } else if let Some(loot) = o.loot.iter_mut().find(|l| l.space_obj_id == object) {
        loot.spawned = 2;
        loot.space_obj_id = 0;
    }
    let Some(o) = object_mut(cx, id) else { return };
    for members in o.formation_members.values_mut() {
        members.retain(|&m| m != object);
    }
    o.formation_members.retain(|_, m| !m.is_empty());
}

/// A jumper's ship: its space object, or the one the mission noted for it
/// when the NPC is gone (D979).
fn jumper_ship(cx: &Cx<'_>, o: &MissionObject, key: NpcShipKey) -> u32 {
    if let Some(n) = cx.logic.content.state.npc_ships.get(&key) {
        return n.base.ship;
    }
    o.npc_objects
        .iter()
        .find(|&(_, &k)| k == key)
        .map_or(0, |(&ship, _)| ship)
}

// ---- the jumpers -----------------------------------------------------------------------------

/// `FContentNpcShip::Relocate(NewSystem, Position, Orientation)`
/// (`NpcShip.cpp:861-897`): the ship leaves its record; leaving every
/// system tells its controller (`NpcSystemExit`); then `RelocateShip` and
/// the new record.
pub fn relocate_npc(
    cx: &mut Cx<'_>,
    key: NpcShipKey,
    system: Option<u32>,
    position: Vector,
    orientation: Matrix,
) {
    let Some(n) = cx.logic.content.state.npc_ships.get(&key) else {
        return;
    };
    let (ship, old, controller) = (n.base.ship, n.base.system, n.controller);
    if let Some(old) = old {
        systems::remove_space_obj(cx, old, ship, false);
    }
    let new_id = match (system, old) {
        (Some(s), _) => s,
        (None, Some(old)) => {
            let _ = cx.dispatch(
                ControllerId(controller),
                &mut ControllerMessage::NpcSystemExit { ship, system: old },
            );
            0
        }
        (None, None) => 0,
    };
    relocate_ship(cx, ship, new_id, position, orientation);
    if let Some(s) = system {
        if let Some(record) = cx.logic.content.state.systems.get_mut(&s) {
            record.space_objs.push(key);
        }
        if let Some(n) = cx.logic.content.state.npc_ships.get_mut(&key) {
            n.base.system = Some(s);
        }
    } else if let Some(n) = cx.logic.content.state.npc_ships.get_mut(&key) {
        n.base.system = None;
    }
}

/// TU53 `RelocateShip` (`DockQueues.cpp:539-546`): `pub::SpaceObj::Relocate`,
/// the ship's docking cancelled when it leaves every system, the dock
/// queues updated.
pub fn relocate_ship(
    cx: &mut Cx<'_>,
    ship: u32,
    system: u32,
    position: Vector,
    orientation: Matrix,
) {
    let _ = api::space_obj::relocate(cx, ship, system, position, orientation);
    if system == 0 {
        dock_queues::cancel_ship_docking(cx, ship, 0, false);
    }
    dock_queues::update_dock_queues(cx);
}

/// `OnShipJump` (`MissionObject.cpp:2345-2385`, `Msg0x25`): the player's
/// ship takes its jumpers (state 0 to 2) out of space with it once its
/// gate's target is known; a mission NPC's own jump takes its entry (state
/// 1).
pub fn on_ship_jump(cx: &mut Cx<'_>, id: ControllerId, ship: u32) {
    let Some(s) = script(cx, id) else { return };
    let Some(own) = player_state(cx, s)
        .and_then(|p| p.ship.as_ref())
        .map(|p| (p.ship, p.target))
    else {
        return;
    };
    if ship == own.0 {
        let Ok((system, gate)) = api::space_obj::jump_target(cx, own.1) else {
            return;
        };
        let Some(o) = object_mut(cx, id) else { return };
        o.jump_system = system;
        o.jump_gate = gate;
        let mut leaving = Vec::new();
        for j in &mut o.jumpers {
            if j.state == 0 {
                j.state = 2;
                leaving.push(j.npc);
            }
        }
        for key in leaving {
            relocate_npc(cx, key, None, Vector::ZERO, Matrix::IDENTITY);
        }
        return;
    }
    let Some(key) = object(cx, id).and_then(|o| o.npc_objects.get(&ship).copied()) else {
        return;
    };
    let Some(nickname) = cx
        .logic
        .content
        .state
        .npc_ships
        .get(&key)
        .map(|n| n.nickname.clone())
    else {
        return;
    };
    let names: Vec<Vec<u8>> = object(cx, id).map_or_else(Vec::new, |o| {
        o.jumpers
            .iter()
            .map(|j| {
                cx.logic
                    .content
                    .state
                    .npc_ships
                    .get(&j.npc)
                    .map(|n| n.nickname.clone())
                    .unwrap_or_default()
            })
            .collect()
    });
    let Some(at) = names.iter().position(|n| c_str(n) == c_str(&nickname)) else {
        return;
    };
    let Some(o) = object_mut(cx, id) else { return };
    o.jumpers[at].state = 1;
    let npc = o.jumpers[at].npc;
    relocate_npc(cx, npc, None, Vector::ZERO, Matrix::IDENTITY);
}

/// `JumpInComplete` (`MissionObject.cpp:1630-1634`): a pending jump starts
/// its timer.
pub fn jump_in_complete(cx: &mut Cx<'_>, id: ControllerId) {
    if let Some(o) = object_mut(cx, id)
        && o.jump_pending
    {
        o.jump_timer = 0.0;
    }
}

/// `UpdateJumpers(DeltaTime)` (`MissionObject.cpp:2388-2422`): while a
/// jump waits and the timer has not passed the delay, the frame time is
/// added; then every state-2 jumper jumps in at the gate, loses its
/// directives and guide, and `Slot28` gets `{ship, system}`.
pub fn update_jumpers(cx: &mut Cx<'_>, id: ControllerId, dt: f32) {
    let Some(o) = object_mut(cx, id) else { return };
    if !o.jump_pending || o.jump_timer == -1.0 {
        return;
    }
    if o.jump_delay as f32 > o.jump_timer {
        o.jump_timer = (f64::from(o.jump_timer) + f64::from(dt)) as f32;
        return;
    }
    let (system, gate) = (o.jump_system, o.jump_gate);
    let arriving: Vec<usize> = o
        .jumpers
        .iter()
        .enumerate()
        .filter(|(_, j)| j.state == 2)
        .map(|(i, _)| i)
        .collect();
    for at in arriving {
        let Some(key) = object(cx, id)
            .and_then(|o| o.jumpers.get(at))
            .map(|j| j.npc)
        else {
            continue;
        };
        // `FindSystemInfo(&JumpSystemId)` is not tested; a jumper without a
        // record stays out (D983).
        if cx.logic.content.state.systems.contains_key(&system) {
            if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
                record.space_objs.push(key);
            }
            if let Some(n) = cx.logic.content.state.npc_ships.get_mut(&key) {
                n.base.system = Some(system);
            }
            let ship = npc_ships::space_obj(cx, key);
            let _ = api::space_obj::jump_in(cx, ship, gate);
            npc_ships::cancel_directives(cx, ShipKey::Npc(key));
            crate::content::guides::set_guide(cx, key, None, false, false);
            super::engine::slot28(cx, id, [ship, system, 0]);
        }
        if let Some(j) = object_mut(cx, id).and_then(|o| o.jumpers.get_mut(at)) {
            j.state = 0;
        }
    }
    if let Some(o) = object_mut(cx, id) {
        o.jump_pending = false;
        o.jump_timer = -1.0;
    }
}

/// `PlaceJumpersAtLaunch(Parms)` (`MissionObject.cpp:2425-2506`,
/// `SystemEnter`): every state-1 jumper is placed by the object the player
/// arrived from (700 above a gate or hole, at a dock, else at the player's
/// ship) at an empty spot and comes into the system without its
/// directives, `Slot28` getting `{ship, system, object}`; a state-2 jumper
/// sets the pending jump.
pub fn place_jumpers_at_launch(cx: &mut Cx<'_>, id: ControllerId, system: u32, arrival: u32) {
    let count = object(cx, id).map_or(0, |o| o.jumpers.len());
    for at in 0..count {
        let Some(j) = object(cx, id).and_then(|o| o.jumpers.get(at)).copied() else {
            break;
        };
        if j.state != 1 {
            if j.state == 2
                && let Some(o) = object_mut(cx, id)
            {
                o.jump_pending = true;
            }
            continue;
        }
        let kind = api::space_obj::object_type(cx, arrival).unwrap_or(0);
        let mut placement = Transform::IDENTITY;
        let mut selection = 0;
        let mut orientation = Matrix::IDENTITY;
        if kind & 0x40 != 0 {
            let (mut p, o) = location(cx, arrival);
            p.z = (f64::from(p.z) + 700.0) as f32;
            orientation = o;
            placement = Transform::new(o, p);
        } else if kind & 0x800 != 0 {
            let (p, o) = location(cx, arrival);
            orientation = o;
            placement = Transform::new(o, p);
        } else if script(cx, id).and_then(|s| player_state(cx, s)).is_some() {
            let (p, o) = location(cx, player_ship_id(cx, id));
            orientation = o;
            placement = Transform::new(o, p);
            selection = 3;
        }
        let launch_radius = api::space_obj::radius(cx, arrival).map_or(0.0, |(r, _)| r);
        let ship = npc_ships::space_obj(cx, j.npc);
        let npc_radius = api::space_obj::radius(cx, ship).map_or(0.0, |(r, _)| r);
        let start = (f64::from(launch_radius) * 1.25) as f32;
        let Ok(position) =
            api::space_obj::empty_pos(cx, system, &placement, start, npc_radius, selection)
        else {
            continue;
        };
        let record = Some(system).filter(|s| cx.logic.content.state.systems.contains_key(s));
        relocate_npc(cx, j.npc, record, position, orientation);
        npc_ships::cancel_directives(cx, ShipKey::Npc(j.npc));
        super::engine::slot28(cx, id, [ship, system, arrival]);
        if let Some(j) = object_mut(cx, id).and_then(|o| o.jumpers.get_mut(at)) {
            j.state = 0;
        }
    }
}

// ---- the respawns and the snapshot -------------------------------------------------------------

/// `RespawnMissionObjects(bKeepHitPoints)` (`MissionObject.cpp:2508-2561`):
/// every respawn record is made again (its slots' hit points -1 unless
/// kept), then each random encounter's saved formation through its
/// `[MsnRandEnc]` (P3.6d); the first ship of each record takes its
/// directives at its priority; both lists empty.
pub fn respawn_mission_objects(cx: &mut Cx<'_>, id: ControllerId, keep_hit_points: bool) {
    let Some(o) = object_mut(cx, id) else { return };
    if !keep_hit_points {
        for group in &mut o.respawn_ships {
            for slot in &mut group.ships {
                slot.hit_points = -1;
            }
        }
    }
    let groups = o.respawn_ships.clone();
    for group in &groups {
        spawn_ship_group(cx, id, group);
    }
    // Every random-encounter formation through the first `[MsnRandEnc]` of
    // its id: `FRandEncParams::SpawnFormation` with the record's placement,
    // count, leader directives, ship choices, names and priority, no dock
    // target (the parameters taken out of the record for the call, which
    // makes ships).
    let respawns = object(cx, id).map_or_else(Vec::new, |o| o.rand_enc_respawns.clone());
    for mut respawn in respawns {
        let found = object_mut(cx, id).and_then(|o| {
            let at = o
                .rand_encs
                .iter()
                .position(|r| r.nickname_id == respawn.encounter_id)?;
            Some((at, o.rand_encs[at].params.take()))
        });
        let Some((at, Some(mut params))) = found else {
            continue;
        };
        let arena = &mut cx.logic.content.state.directives;
        let mut list: DirectiveList = respawn
            .directives
            .iter()
            .cloned()
            .map(|d| create_directive(arena, d))
            .collect();
        crate::content::random_encounters::spawn_formation(
            cx,
            &mut params,
            &respawn.placement,
            respawn.count,
            &list,
            Some(&respawn.choices),
            Some(&mut respawn.names),
            respawn.priority,
            0,
        );
        directives::clear_list(&mut cx.logic.content.state.directives, &mut list);
        if let Some(r) = object_mut(cx, id).and_then(|o| o.rand_encs.get_mut(at)) {
            r.params = Some(params);
        }
    }
    for group in &groups {
        let Some(first) = group.ships.first() else {
            continue;
        };
        let Some(key) = script(cx, id).and_then(|s| find_npc_by_name(cx, s, &first.nickname))
        else {
            continue;
        };
        // The slot's guide (none on the shipped data, D985) or its
        // directives.
        let arena = &mut cx.logic.content.state.directives;
        let mut list: DirectiveList = first
            .directives
            .iter()
            .cloned()
            .map(|d| create_directive(arena, d))
            .collect();
        let priority = npc_ships::priority(cx, ShipKey::Npc(key));
        npc_ships::set_directives(cx, ShipKey::Npc(key), &list, priority);
        directives::clear_list(&mut cx.logic.content.state.directives, &mut list);
    }
    if let Some(o) = object_mut(cx, id) {
        o.respawn_ships.clear();
        o.rand_enc_respawns.clear();
    }
}

/// The ship's hit points (`GetHealth`'s current value through `ftol`).
fn hit_points(cx: &Cx<'_>, ship: u32) -> i32 {
    let current = api::space_obj::health(cx, ship).map_or(0.0, |(c, _)| c);
    neuron_core::num::trunc_to_i32(f64::from(current))
}

/// The ship's directives as values (`TakeDirectives`: a copy of its list).
fn directives_of(cx: &Cx<'_>, key: NpcShipKey) -> Vec<ObjListDirective> {
    let state = &cx.logic.content.state;
    state
        .npc_ships
        .get(&key)
        .map(|n| {
            n.directives
                .list
                .iter()
                .filter_map(|&d| state.directives.get(d).cloned())
                .collect()
        })
        .unwrap_or_default()
}

/// `SnapshotMissionObjects` (`MissionSave.cpp:223-484`): the respawn lists
/// rebuilt from the live objects: one `[MsnShip]` record per live mission
/// ship alone and per formation group (its ships' hit points and priority,
/// the leader's place in the player's system, its directives), one record
/// per random encounter formation, the spawned solars of the player's
/// system by name; then the attitudes ([`snapshot_vibe_infos`]).
#[allow(clippy::too_many_lines)]
pub fn snapshot_mission_objects(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(o) = object_mut(cx, id) else { return };
    o.respawn_ships.clear();
    o.rand_enc_respawns.clear();
    o.respawn_solars.clear();
    let random = create_id(MISSION_RANDOM_ENCOUNTER);
    let local = create_id(LOCAL_OBJECTIVE);
    let player_record = player_system(cx, id);
    let mut formation_npcs: BTreeMap<u32, Vec<NpcShipKey>> = BTreeMap::new();
    let mut formation_refs: Vec<u32> = Vec::new();
    let npcs: Vec<NpcShipKey> =
        object(cx, id).map_or_else(Vec::new, |o| o.npc_objects.values().copied().collect());
    for key in npcs {
        let Some(n) = cx.logic.content.state.npc_ships.get(&key).cloned() else {
            continue;
        };
        if !n.field_0x17c.contains(&random) {
            if n.life_time != -1.0 {
                continue;
            }
            let formation_ref = n.base.formation_id;
            if formation_ref != u32::MAX {
                formation_npcs.entry(formation_ref).or_default().push(key);
                if !formation_refs.contains(&formation_ref) {
                    formation_refs.push(formation_ref);
                }
                continue;
            }
            let Some(mut copy) = object(cx, id)
                .and_then(|o| o.find_ship_group(&n.nickname))
                .cloned()
            else {
                continue;
            };
            if let Some(f) = n.base.formation {
                copy.formation = f;
            }
            let priority = n.directives.priority;
            let hp = hit_points(cx, n.base.ship);
            let jumper = {
                // The ship is not tested for null in the C++ (D985).
                let Some(o) = object_mut(cx, id) else { return };
                let Some(slot) = o.find_ship_mut(&n.nickname) else {
                    continue;
                };
                slot.priority = priority;
                slot.hit_points = hp;
                slot.jumper
            };
            match n.base.system {
                Some(system) => {
                    if player_record == Some(system) {
                        let (p, m) = location(cx, n.base.ship);
                        copy.position = p;
                        copy.orientation = m;
                    }
                }
                None => {
                    if jumper {
                        copy.arrival_obj_id = object(cx, id).map_or(0, |o| o.jump_gate);
                    }
                }
            }
            copy.ships.clear();
            let hp = hit_points(cx, n.base.ship);
            if let Some(o) = object_mut(cx, id)
                && let Some(slot) = o.find_ship_mut(&n.nickname)
            {
                slot.hit_points = hp;
                copy.ships.push(slot.clone());
            }
            let Some(first) = copy.ships.first_mut() else {
                continue;
            };
            if n.guide.is_none() {
                first.init_objectives = local;
                first.directives = directives_of(cx, key);
            }
            if let Some(o) = object_mut(cx, id) {
                o.respawn_ships.push(copy);
            }
            continue;
        }

        // A random encounter's ship: the first encounter whose id it carries.
        let Some(encounter) = object(cx, id)
            .and_then(|o| {
                o.rand_encs
                    .iter()
                    .find(|e| n.field_0x17c.contains(&e.nickname_id))
            })
            .map(|e| e.nickname_id)
        else {
            continue;
        };
        let leader = n.base.formation_slot == 0 || n.base.formation_id == u32::MAX;
        let (position, orientation) = location(cx, n.base.ship);
        let hp = hit_points(cx, n.base.ship);
        let directives = directives_of(cx, key);
        let choice = ShipChoice {
            ship_classes: n.base.class_ids.clone(),
            weight: 1.0,
        };
        let Some(o) = object_mut(cx, id) else { return };
        let mut found = false;
        for record in o
            .rand_enc_respawns
            .iter_mut()
            .filter(|r| r.formation_ref == n.base.formation_id)
        {
            record.count += 1;
            found = true;
            record.choices.push(choice.clone());
            record.names.push(n.nickname.clone());
            if !leader {
                continue;
            }
            record.placement = Transform::new(orientation, position);
            record.priority = n.directives.priority;
            record.hit_points = hp;
            if n.guide.is_none() {
                record.directives = directives.clone();
            }
        }
        if found {
            continue;
        }
        let mut record = RandEncRespawn {
            encounter_id: encounter,
            formation_ref: n.base.formation_id,
            priority: n.directives.priority,
            count: 1,
            ..RandEncRespawn::default()
        };
        record.choices.push(choice);
        record.names.push(n.nickname.clone());
        if leader {
            record.placement = Transform::new(orientation, position);
            record.hit_points = hp;
            if n.guide.is_none() {
                record.directives = directives;
            }
        }
        o.rand_enc_respawns.push(record);
    }

    // One record per formation group, in first-seen order.
    for formation_ref in formation_refs {
        let mut dest = ShipGroup::default();
        let mut ships: Vec<MissionShip> = Vec::new();
        let mut found_group = false;
        let mut abandon = false;
        for &key in formation_npcs.get(&formation_ref).into_iter().flatten() {
            let Some(n) = cx.logic.content.state.npc_ships.get(&key).cloned() else {
                continue;
            };
            if n.field_0x17c.contains(&random) {
                break;
            }
            let priority = n.directives.priority;
            let hp = hit_points(cx, n.base.ship);
            let Some(slot) = object_mut(cx, id).and_then(|o| o.find_ship_mut(&n.nickname)) else {
                continue;
            };
            slot.priority = priority;
            slot.hit_points = hp;
            let slot = slot.clone();
            if n.base.formation_slot != 0 && n.base.formation_id != u32::MAX {
                ships.push(slot);
                continue;
            }
            let Some(group) = object(cx, id)
                .and_then(|o| o.find_ship_group(&n.nickname))
                .cloned()
            else {
                abandon = true;
                break;
            };
            found_group = true;
            dest = group;
            if let Some(f) = n.base.formation {
                dest.formation = f;
            }
            match n.base.system {
                Some(system) => {
                    if player_record == Some(system) {
                        let (p, m) = location(cx, n.base.ship);
                        dest.position = p;
                        dest.orientation = m;
                    }
                }
                None => {
                    if slot.jumper {
                        dest.arrival_obj_id = object(cx, id).map_or(0, |o| o.jump_gate);
                    }
                }
            }
            ships.push(slot);
            if n.guide.is_none() {
                let directives = directives_of(cx, key);
                if let Some(first) = ships.first_mut() {
                    first.init_objectives = local;
                    first.directives = directives;
                }
            }
        }
        if !abandon && found_group {
            dest.ships = ships;
        }
        // `std::sort` by the ships' formation slots (both NPCs found).
        let slots: Vec<Option<u32>> = dest
            .ships
            .iter()
            .map(|slot| {
                script(cx, id)
                    .and_then(|s| find_npc_by_name(cx, s, &slot.nickname))
                    .and_then(|k| cx.logic.content.state.npc_ships.get(&k))
                    .map(|n| n.base.formation_slot)
            })
            .collect();
        let mut order: Vec<usize> = (0..dest.ships.len()).collect();
        order.sort_by(|&a, &b| match (slots[a], slots[b]) {
            (Some(x), Some(y)) => (x.cast_signed()).cmp(&y.cast_signed()),
            _ => std::cmp::Ordering::Equal,
        });
        dest.ships = order.into_iter().map(|i| dest.ships[i].clone()).collect();
        if let Some(o) = object_mut(cx, id) {
            o.respawn_ships.push(dest);
        }
    }

    // The spawned solars of the player's system.
    if let Some(system) = player_record
        && let Some(o) = object_mut(cx, id)
    {
        let names: Vec<Vec<u8>> = o
            .solars
            .iter()
            .filter(|s| s.system_id == system && s.spawned == 1)
            .map(|s| s.nickname.clone())
            .collect();
        o.respawn_solars.extend(names);
    }
    snapshot_vibe_infos(cx, id);
}

/// `GetSpaceObjAttitude` (TU53, `MissionSave.cpp:170-178`): the attitude
/// of one object's reputation towards the other's.
fn space_obj_attitude(cx: &mut Cx<'_>, from: u32, to: u32) -> Option<f32> {
    let a = api::space_obj::rep(cx, from).unwrap_or(0);
    let b = api::space_obj::rep(cx, to).unwrap_or(0);
    api::reputation::attitude(cx, a, b).ok()
}

/// `IsVibeParty`: an NPC whose life is -1 or that carries the mission
/// encounter label.
fn is_vibe_party(cx: &Cx<'_>, key: NpcShipKey) -> bool {
    let random = create_id(MISSION_RANDOM_ENCOUNTER);
    cx.logic
        .content
        .state
        .npc_ships
        .get(&key)
        .is_some_and(|n| n.life_time == -1.0 || n.field_0x17c.contains(&random))
}

fn vibe_row(a: &[u8], b: &[u8], attitude: f32) -> VibeInfo {
    VibeInfo {
        object_a: c_str(a).to_vec(),
        object_b: c_str(b).to_vec(),
        rep_a: 0,
        rep_b: 0,
        attitude,
    }
}

/// `SnapshotVibeInfos` (`MissionSave.cpp:487-679`): the attitudes between
/// the live mission NPCs, the spawned solars of the player's system, the
/// player and the solar reputations. A label row's two reputations are
/// unset in the C++ and read only for an empty name; the port zeroes them.
#[allow(clippy::too_many_lines)]
pub fn snapshot_vibe_infos(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(o) = object_mut(cx, id) else { return };
    o.vibe_infos.clear();
    let Some(system) = player_system(cx, id) else {
        return;
    };
    let Some(o) = object(cx, id) else { return };
    let player = o.player;
    let reps = o.solar_reps.clone();
    let npcs: Vec<(u32, NpcShipKey)> = o.npc_objects.iter().map(|(&s, &k)| (s, k)).collect();
    let solars: Vec<(u32, Vec<u8>)> = o
        .solars
        .iter()
        .filter(|s| s.system_id == system && s.spawned == 1)
        .map(|s| (s.space_obj_id, s.nickname.clone()))
        .collect();
    let name_of = |cx: &Cx<'_>, k: NpcShipKey| {
        cx.logic
            .content
            .state
            .npc_ships
            .get(&k)
            .map(|n| n.nickname.clone())
            .unwrap_or_default()
    };
    let mut rows = Vec::new();
    let player_rep = |cx: &mut Cx<'_>| api::player::rep(cx, player).unwrap_or(0);
    for &(ship, key) in &npcs {
        if !is_vibe_party(cx, key) {
            continue;
        }
        let name = name_of(cx, key);
        let own = api::space_obj::rep(cx, ship).unwrap_or(0);
        let p = player_rep(cx);
        let attitude = api::reputation::attitude(cx, own, p).unwrap_or(0.0);
        rows.push(vibe_row(&name, PLAYER_LABEL, attitude));
        for &(other, other_key) in &npcs {
            if !is_vibe_party(cx, other_key) {
                continue;
            }
            if let Some(attitude) = space_obj_attitude(cx, ship, other) {
                rows.push(vibe_row(&name, &name_of(cx, other_key), attitude));
            }
        }
        for (solar, solar_name) in &solars {
            if let Some(attitude) = space_obj_attitude(cx, ship, *solar) {
                rows.push(vibe_row(&name, solar_name, attitude));
            }
        }
        let own = api::space_obj::rep(cx, ship).unwrap_or(-1);
        for &rep in &reps {
            rows.push(VibeInfo {
                object_a: Vec::new(),
                object_b: name.clone(),
                rep_a: rep,
                rep_b: -1,
                attitude: api::reputation::attitude(cx, rep, own).unwrap_or(0.0),
            });
            rows.push(VibeInfo {
                object_a: name.clone(),
                object_b: Vec::new(),
                rep_a: -1,
                rep_b: rep,
                attitude: api::reputation::attitude(cx, own, rep).unwrap_or(0.0),
            });
        }
    }
    for (solar, solar_name) in &solars {
        let own = api::space_obj::rep(cx, *solar).unwrap_or(0);
        let p = player_rep(cx);
        let attitude = api::reputation::attitude(cx, own, p).unwrap_or(0.0);
        rows.push(vibe_row(solar_name, PLAYER_LABEL, attitude));
        for &(ship, key) in &npcs {
            if !is_vibe_party(cx, key) {
                continue;
            }
            if let Some(attitude) = space_obj_attitude(cx, *solar, ship) {
                rows.push(vibe_row(solar_name, &name_of(cx, key), attitude));
            }
        }
        for (other, other_name) in &solars {
            if let Some(attitude) = space_obj_attitude(cx, *solar, *other) {
                rows.push(vibe_row(solar_name, other_name, attitude));
            }
        }
        let own = api::space_obj::rep(cx, *solar).unwrap_or(-1);
        for &rep in &reps {
            rows.push(VibeInfo {
                object_a: Vec::new(),
                object_b: solar_name.clone(),
                rep_a: rep,
                rep_b: -1,
                attitude: api::reputation::attitude(cx, rep, own).unwrap_or(0.0),
            });
            rows.push(VibeInfo {
                object_a: solar_name.clone(),
                object_b: Vec::new(),
                rep_a: -1,
                rep_b: rep,
                attitude: api::reputation::attitude(cx, own, rep).unwrap_or(0.0),
            });
        }
    }
    let p = api::player::rep(cx, player).unwrap_or(-1);
    for &rep in &reps {
        rows.push(VibeInfo {
            object_a: Vec::new(),
            object_b: PLAYER_LABEL.to_vec(),
            rep_a: rep,
            rep_b: -1,
            attitude: api::reputation::attitude(cx, rep, p).unwrap_or(0.0),
        });
    }
    if let Some(o) = object_mut(cx, id) {
        o.vibe_infos = rows;
    }
}

/// `ApplyVibeInfos` (`MissionSave.cpp:682-700`): each row's attitude set
/// again, a named party's reputation from its label's object.
pub fn apply_vibe_infos(cx: &mut Cx<'_>, id: ControllerId) {
    let rows = object(cx, id)
        .map(|o| o.vibe_infos.clone())
        .unwrap_or_default();
    for row in rows {
        let rep_of = |cx: &mut Cx<'_>, name: &[u8], own: i32| {
            if c_str(name).is_empty() {
                return own;
            }
            let object = script(cx, id).map_or(0, |s| label_object_id(cx, s, name));
            api::space_obj::rep(cx, object).unwrap_or(-1)
        };
        let a = rep_of(cx, &row.object_a, row.rep_a);
        let b = rep_of(cx, &row.object_b, row.rep_b);
        let _ = api::reputation::set_attitude(cx, a, b, row.attitude);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_anchor_placement_faces_away_from_the_anchor() {
        let placed = place_facing_anchor(&Transform::IDENTITY, 0.0, 100.0);
        assert_eq!(placed.translation, Vector::new(100.0, 0.0, 0.0));
        // The third column is the forward axis: from the anchor outwards.
        let m = placed.rotation.m;
        assert_eq!([m[0][2], m[1][2], m[2][2]], [1.0, 0.0, 0.0]);
        // Right is (z, 0, -x) of the forward axis, normalised; up their
        // cross product, negated.
        assert_eq!([m[0][0], m[1][0], m[2][0]], [0.0, 0.0, -1.0]);
        assert_eq!([m[0][1], m[1][1], m[2][1]], [0.0, 1.0, 0.0]);
    }

    #[test]
    fn an_arm_left_without_members_goes() {
        let mut o = MissionObject::new(ControllerId(1));
        o.rand_encs.push(super::super::rand_enc::MissionRandEnc {
            nickname: b"enc".to_vec(),
            enabled: true,
            nickname_id: create_id(b"enc"),
            formation_index: 2,
            params: None,
            npc_count: 0,
            arms: vec![
                RandEncArm {
                    name: 1,
                    formation_ref: 9,
                    id: 1,
                    members: vec![10, 11],
                },
                RandEncArm {
                    name: 2,
                    formation_ref: 8,
                    id: 2,
                    members: vec![12],
                },
            ],
        });
        remove_rand_enc_member(&mut o, 12);
        assert_eq!(o.rand_encs[0].arms.len(), 1);
        assert_eq!(o.rand_encs[0].formation_index, 2);
        remove_rand_enc_member(&mut o, 10);
        remove_rand_enc_member(&mut o, 11);
        // No arm left: the counter starts again.
        assert!(o.rand_encs[0].arms.is_empty());
        assert_eq!(o.rand_encs[0].formation_index, 0);
    }
}
