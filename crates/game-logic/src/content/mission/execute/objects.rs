//! The actions of the mission object's spawns and objects
//! (`Content/MissionAct.cpp`; work package P3.5c2): `Act::Execute` of the
//! 22 classes [`super::is_ported`] lists after group A.
//!
//! The C++'s comparisons are kept as written: an x87 `FCOMP` branch is
//! false for a NaN on either side.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::hash::c_str;
use neuron_core::id::create_id;
use neuron_core::math::{
    Matrix, Vector, mul_matrix, quaternion_to_matrix, transform_matrix, transform_xform,
};
use neuron_server::controller::ControllerId;

use super::super::action::{Action, SpawnShipRel};
use super::super::object::Jumper;
use super::super::runtime::{
    ID_MASK, find_npc_by_name, label_object_id, player_state, script, script_mut,
};
use super::super::spawn::{self, PLAYER_LABEL};
use crate::api;
use crate::api::ai::{BaseOp, Op};
use crate::content::formations;
use crate::content::ships;
use crate::server::Cx;
use game_sim::ai::behavior::DirectiveOp;

/// The mission's object id of `label` (`GetLabelObjectId`), 0 without
/// the script.
fn label_object(cx: &Cx<'_>, id: ControllerId, label: &[u8]) -> u32 {
    script(cx, id).map_or(0, |s| label_object_id(cx, s, label))
}

/// The actions of P3.5c2; true (the C++'s result, unused) for the others,
/// which [`super::execute`] runs itself or leaves without effect (D970).
#[allow(clippy::too_many_lines)]
pub fn execute(cx: &mut Cx<'_>, id: ControllerId, action: &mut Action) -> bool {
    let Some(player) = script(cx, id).map(|s| s.object.player) else {
        return true;
    };
    match action {
        Action::SpawnShip(a) => {
            // `FindShipGroup`: the first record named so (`strcmp`).
            let Some(mut group) = script(cx, id)
                .and_then(|s| {
                    s.object
                        .ships
                        .iter()
                        .find(|g| c_str(&g.nickname) == c_str(&a.name))
                })
                .cloned()
            else {
                return true;
            };
            for slot in &mut group.ships {
                slot.hit_points = slot.field_0x80;
            }
            if a.obj_list != 0 {
                // The first slot is read without a test (D986).
                if a.obj_list != create_id(spawn::NO_OBJECT_LIST)
                    && let Some(first) = group.ships.first_mut()
                {
                    first.init_objectives = a.obj_list;
                }
                if a.position {
                    group.rel_pos = false;
                    group.position = a.placement.translation;
                }
                if a.orientation {
                    group.orientation = a.placement.rotation;
                }
                if a.rename
                    && let Some(first) = group.ships.first_mut()
                {
                    first.nickname = a.new_nickname.clone();
                }
            }
            spawn::spawn_ship_group(cx, id, &group);
            true
        }
        Action::SpawnShipRel(a) => {
            let count = script(cx, id).map_or(0, |s| s.object.ships.len());
            for at in 0..count {
                let Some(record) = script(cx, id).and_then(|s| s.object.ships.get(at)).cloned()
                else {
                    break;
                };
                let name = c_str(&a.name);
                // The first slot of its own nickname, or the first slot
                // of a record of that nickname.
                let Some(slot) = record
                    .ships
                    .iter()
                    .find(|slot| c_str(&slot.nickname) == name || c_str(&record.nickname) == name)
                    .cloned()
                else {
                    continue;
                };
                let mut group = record;
                group.ships = vec![slot];
                if a.obj_list != 0 {
                    if a.obj_list != create_id(spawn::NO_OBJECT_LIST) {
                        group.ships[0].init_objectives = a.obj_list;
                    }
                    let anchor = label_object(cx, id, &a.rel_obj);
                    let (orientation, position) = spawn_placement(cx, a, anchor, group.position);
                    group.orientation = orientation;
                    group.position = position;
                    if a.rename {
                        group.ships[0].nickname = a.new_nickname.clone();
                    }
                }
                spawn::spawn_ship_group(cx, id, &group);
            }
            true
        }
        Action::SpawnFormation(a) => {
            let Some(at) = script(cx, id).and_then(|s| {
                s.object
                    .ships
                    .iter()
                    .position(|g| c_str(&g.nickname) == c_str(&a.name))
            }) else {
                return true;
            };
            // The record's own slots get their hit points.
            let Some(group) = script_mut(cx, id).and_then(|s| s.object.ships.get_mut(at)) else {
                return true;
            };
            for slot in &mut group.ships {
                slot.hit_points = slot.field_0x80;
            }
            let mut group = group.clone();
            if a.position {
                group.position = a.placement.translation;
                if a.orientation {
                    group.orientation = a.placement.rotation;
                }
            }
            spawn::spawn_ship_group(cx, id, &group);
            true
        }
        Action::SpawnSolar(a) => {
            let Some(orientation) = script(cx, id)
                .and_then(|s| {
                    s.object
                        .solars
                        .iter()
                        .find(|o| c_str(&o.nickname) == c_str(&a.name))
                })
                .map(|o| o.orientation)
            else {
                return true;
            };
            if a.position {
                // The record's orientation is kept on the action.
                if !a.orientation {
                    a.placement.rotation = orientation;
                }
                let placement = a.placement;
                spawn::spawn_solar(cx, id, &a.name, Some(&placement));
                return true;
            }
            spawn::spawn_solar(cx, id, &a.name, None);
            true
        }
        Action::SpawnLoot(a) => {
            let Some(at) = script(cx, id).and_then(|s| {
                s.object
                    .loot
                    .iter()
                    .position(|l| c_str(&l.nickname) == c_str(&a.name))
            }) else {
                return true;
            };
            if !a.position {
                // The record itself is written.
                let Some(mut loot) = script(cx, id).and_then(|s| s.object.loot.get(at)).cloned()
                else {
                    return true;
                };
                spawn::spawn_loot(cx, id, &mut loot);
                if let Some(slot) = script_mut(cx, id).and_then(|s| s.object.loot.get_mut(at)) {
                    *slot = loot;
                }
                return true;
            }
            // A copy is written and dropped.
            let Some(mut loot) = script(cx, id).and_then(|s| s.object.loot.get(at)).cloned() else {
                return true;
            };
            loot.position = a.placement.translation;
            if a.orientation {
                loot.orientation = a.placement.rotation;
            }
            spawn::spawn_loot(cx, id, &mut loot);
            true
        }
        Action::Destroy(a) => {
            let object = label_object(cx, id, &a.label);
            if object & ID_MASK != 0 {
                let _ = api::space_obj::destroy(cx, object, a.destroy_mode);
            }
            true
        }
        Action::Jumper(a) => {
            let npcs: Vec<_> = script(cx, id)
                .map(|s| s.object.npc_objects.values().copied().collect())
                .unwrap_or_default();
            for key in npcs {
                let Some(n) = cx.logic.content.state.npc_ships.get(&key) else {
                    continue;
                };
                if !n.field_0x17c.contains(&a.label_id) {
                    continue;
                }
                let (nickname, ship) = (n.nickname.clone(), n.base.ship);
                let Some(s) = script(cx, id) else { break };
                if s.object.find_ship(&nickname).is_none() {
                    continue;
                }
                let state = &cx.logic.content.state;
                // `FindJumperNpc` (`strcmp` of the nicknames) and
                // `RemoveJumperNpc` (the first of the ship's id).
                let named = s.object.jumpers.iter().any(|j| {
                    state
                        .npc_ships
                        .get(&j.npc)
                        .is_some_and(|n| c_str(&n.nickname) == c_str(&nickname))
                });
                let of_ship = s.object.jumpers.iter().position(|j| {
                    state
                        .npc_ships
                        .get(&j.npc)
                        .is_some_and(|n| n.base.ship == ship)
                });
                let Some(s) = script_mut(cx, id) else { break };
                if let Some(slot) = s.object.find_ship_mut(&nickname) {
                    slot.jumper = a.jumper;
                }
                if a.jumper {
                    if !named {
                        s.object.jumpers.push(Jumper { npc: key, state: 0 });
                    }
                } else if let Some(at) = of_ship {
                    s.object.jumpers.remove(at);
                }
            }
            true
        }
        Action::RelocateShip(a) => {
            let object = label_object(cx, id, &a.label);
            if object & ID_MASK == 0 {
                return false;
            }
            // The player's system record is read without a test (D986).
            if let Some(system) = spawn::player_system(cx, id) {
                spawn::relocate_ship(
                    cx,
                    object,
                    system,
                    a.placement.translation,
                    a.placement.rotation,
                );
            }
            true
        }
        Action::RelocateForm(a) => {
            let Some(key) = script(cx, id).and_then(|s| find_npc_by_name(cx, s, &a.label)) else {
                return false;
            };
            if script(cx, id).and_then(|s| player_state(cx, s)).is_none() {
                return false;
            }
            let state = &cx.logic.content.state;
            let Some(n) = state.npc_ships.get(&key) else {
                return false;
            };
            let formation = n.base.formation;
            let members = formations::collect_members(state, n.base.formation_id);
            let tables = cx.logic.content.tables.clone();
            for member in members {
                let slot = ships::formation_slot(&cx.logic.content.state, member);
                // `GetSlotTransform` of the formation's identity transform;
                // a slot it rejects, or no formation, leaves zero (D986).
                let (slot_position, slot_orientation) = formation
                    .and_then(|f| tables.formation(f))
                    .and_then(|f| f.slots.get(slot as usize))
                    .map_or((Vector::ZERO, Matrix::from_rows([[0.0; 3]; 3])), |s| {
                        (
                            Vector::new(
                                s.position[0] + 0.0,
                                s.position[1] + 0.0,
                                s.position[2] + 0.0,
                            ),
                            Matrix::IDENTITY,
                        )
                    });
                let position = transform_xform(&a.placement, slot_position);
                let orientation = mul_matrix(&a.placement.rotation, &slot_orientation);
                let ship = ships::id(&cx.logic.content.state, member);
                if let Some(system) = spawn::player_system(cx, id) {
                    spawn::relocate_ship(cx, ship, system, position, orientation);
                }
            }
            true
        }
        Action::SetOrient(a) => {
            let object = label_object(cx, id, &a.label);
            if object & ID_MASK == 0 {
                return false;
            }
            let position = api::space_obj::location(cx, object).map_or(Vector::ZERO, |(p, _)| p);
            let Some(system) = spawn::player_system(cx, id) else {
                return true;
            };
            let orientation = quaternion_to_matrix(&a.orientation);
            spawn::relocate_ship(cx, object, system, position, orientation);
            true
        }
        Action::MovePlayer(a) => {
            move_mission_player(cx, id, a.position, a.radius_sq);
            true
        }
        Action::SetShipAndLoadout(a) => {
            let _ = api::player::set_ship_and_loadout(cx, player, a.ship_arch_id, &a.loadout);
            true
        }
        Action::RemoveCargo(a) => {
            let Some(s) = script(cx, id) else { return true };
            let loot = spawn::loot_id(s, &a.loot);
            // `FindLootById` of that id: an unspawned record's 0 finds the
            // first record not spawned.
            let Some(arch) = s
                .object
                .loot
                .iter()
                .find(|l| l.space_obj_id == loot)
                .map(|l| l.equip_arch_id)
            else {
                return true;
            };
            let good = api::misc::translate_arch_to_good(cx, arch).unwrap_or(0);
            let item = api::player::enumerate_cargo(cx, player)
                .unwrap_or_default()
                .into_iter()
                .find(|line| line.good == good)
                .map_or(0, |line| line.id);
            if u32::from(item) & ID_MASK == 0 {
                return true;
            }
            let _ = api::player::remove_cargo(cx, player, item, a.count.cast_unsigned());
            true
        }
        Action::AdjHealth(a) => {
            let members = script(cx, id)
                .and_then(|s| s.object.formation_members.get(&a.formation_id))
                .cloned()
                .unwrap_or_default();
            let mut healths = std::collections::BTreeMap::new();
            for member in members {
                let mut health = api::space_obj::relative_health(cx, member).unwrap_or(0.0);
                health += a.delta;
                // `FCOMP` against 0 and 1; a NaN passes.
                health = health.clamp(0.0, 1.0);
                healths.entry(member).or_insert(health);
            }
            for (member, health) in healths {
                let _ = api::space_obj::set_relative_health(cx, member, health);
            }
            true
        }
        Action::LightFuse(a) => {
            let object = label_object(cx, id, &a.label);
            if object & ID_MASK == 0 {
                return false;
            }
            let _ = api::space_obj::light_fuse(cx, object, &a.fuse, a.delay);
            true
        }
        Action::Invulnerable(a) => {
            invulnerable(cx, id, a);
            true
        }
        Action::Cloak(a) => {
            let key = create_id(c_str(&a.formation));
            let members = script(cx, id)
                .and_then(|s| s.object.formation_members.get(&key))
                .cloned()
                .unwrap_or_default();
            for member in members {
                let name = spawn::mission_object_name(cx, id, member);
                if let Some(s) = script_mut(cx, id) {
                    let listed = s
                        .object
                        .decloaked
                        .iter()
                        .position(|n| c_str(n) == c_str(&name));
                    if a.cloak {
                        if let Some(at) = listed {
                            s.object.decloaked.remove(at);
                        }
                    } else if listed.is_none() {
                        s.object.decloaked.push(name);
                    }
                }
                let info = api::space_obj::EqInfo {
                    kind: 2,
                    on: a.cloak,
                    count: 0,
                    target: 0,
                };
                let _ = api::space_obj::activate_equipment(cx, member, &info);
            }
            true
        }
        Action::MarkObj(a) => mark_obj(cx, id, &a.label, a.mode),
        Action::SetLifeTime(a) => {
            if let Some(key) = script(cx, id).and_then(|s| find_npc_by_name(cx, s, &a.label)) {
                if let Some(n) = cx.logic.content.state.npc_ships.get_mut(&key) {
                    n.life_time = a.life_time;
                    n.life_left = a.life_time;
                }
                return true;
            }
            let label = create_id(c_str(&a.label));
            let npcs: Vec<_> = script(cx, id)
                .map(|s| s.object.npc_objects.values().copied().collect())
                .unwrap_or_default();
            for key in npcs {
                if let Some(n) = cx.logic.content.state.npc_ships.get_mut(&key)
                    && n.field_0x17c.contains(&label)
                {
                    n.life_time = a.life_time;
                    n.life_left = a.life_time;
                }
            }
            true
        }
        Action::LockDock(a) => {
            let gate = api::misc::nickname_id(c_str(&a.gate));
            let single_player = api::misc::single_player(cx);
            let locks = &mut cx.logic.content.state.gate_locks;
            if c_str(&a.target) == b"Player" {
                match a.mode {
                    1 => locks.register(player, gate),
                    0 => {
                        locks.unregister(player, gate, single_player);
                        locks.unregister(0, gate, single_player);
                    }
                    _ => {}
                }
            } else if c_str(&a.target) == b"Everyone" {
                match a.mode {
                    1 => {
                        locks.register(player, gate);
                        locks.register_npc(gate);
                    }
                    0 => {
                        locks.remove_npc(gate);
                        locks.unregister(player, gate, single_player);
                        locks.unregister(0, gate, single_player);
                    }
                    _ => {}
                }
            }
            true
        }
        Action::DockRequest(a) => {
            // `GetFirstContentPlayerState`; its ship is read without a test
            // (D986).
            let Some(ship) = cx
                .logic
                .content
                .state
                .players
                .values()
                .next()
                .map(|p| p.ship.as_ref().map_or(0, |s| s.ship))
            else {
                return false;
            };
            if ship != 0 {
                let _ = api::space_obj::dock_request(cx, ship, a.target_id);
            }
            true
        }
        Action::PobjIdle(_) => {
            // The player's ship is read without a test (D986).
            let ship = spawn::player_ship_id(cx, id);
            if script(cx, id).and_then(|s| player_state(cx, s)).is_some() && ship != 0 {
                let _ = api::ai::submit_directive(
                    cx,
                    ship,
                    &BaseOp::new(Op::Directive(DirectiveOp::Idle)),
                );
            }
            true
        }
        _ => true,
    }
}

/// `ComputeSpawnPlacement(Mode, RelObjId, Offset, Heading, ...)`
/// (`MissionAct.cpp:4256-4330`): the orientation of `Act_SpawnShipRel`
/// relative to the object (turned by the heading when it is not below 0),
/// and the position by the mode (0 2500 m back towards the offset's point,
/// 1 the offset added, 2 the offset turned by the object's orientation and
/// added; another mode keeps `position`). The location's outputs start at
/// zero, as the reconstruction's do, where `GetLocation` fails.
fn spawn_placement(
    cx: &Cx<'_>,
    a: &SpawnShipRel,
    anchor: u32,
    position: Vector,
) -> (Matrix, Vector) {
    let zero = Matrix::from_rows([[0.0; 3]; 3]);
    let (object_position, object_rotation) =
        api::space_obj::location(cx, anchor).unwrap_or((Vector::ZERO, zero));
    let mut heading = Matrix::IDENTITY;
    if !(a.heading < 0.0) && !a.heading.is_nan() {
        let angle = a.heading * 0.017_453_292_f32;
        let (c, s) = (f64::from(angle).cos(), f64::from(angle).sin());
        heading.m[0][0] = c as f32;
        heading.m[0][2] = s as f32;
        heading.m[2][0] = (-s) as f32;
        heading.m[2][2] = c as f32;
    }
    let rotation = mul_matrix(&heading, &object_rotation);
    let offset = a.offset.translation;
    let position = match a.mode {
        2 => {
            let turned = transform_matrix(&object_rotation, offset);
            Vector::new(
                turned.x + object_position.x,
                turned.y + object_position.y,
                turned.z + object_position.z,
            )
        }
        1 => Vector::new(
            object_position.x + offset.x,
            object_position.y + offset.y,
            object_position.z + offset.z,
        ),
        0 => {
            let dx = object_position.x - offset.x;
            let dy = object_position.y - offset.y;
            let dz = object_position.z - offset.z;
            let length_sq = (f64::from(dx) * f64::from(dx)
                + f64::from(dy) * f64::from(dy)
                + f64::from(dz) * f64::from(dz)) as f32;
            let bits = 0x5f37_59df_i32.wrapping_sub(length_sq.to_bits().cast_signed() >> 1);
            let guess = f64::from(f32::from_bits(bits.cast_unsigned()));
            let inverse = (1.5 - guess * guess * f64::from(length_sq) * 0.5) * guess;
            let (nx, ny, nz) = (
                f64::from(dx) * inverse,
                f64::from(dy) * inverse,
                f64::from(dz) * inverse,
            );
            let scale = f64::from((6_250_000.0 / (nz * nz + ny * ny + nx * nx)).sqrt() as f32);
            let (ox, oy, oz) = (
                (nx * scale) as f32,
                (ny * scale) as f32,
                (nz * scale) as f32,
            );
            Vector::new(
                object_position.x - ox,
                object_position.y - oy,
                object_position.z - oz,
            )
        }
        _ => position,
    };
    (rotation, position)
}

/// `MoveMissionPlayer(Script, Position, RadiusSq)` (`MissionScript.cpp:616-645`):
/// the player's ship beyond the radius of the point is relocated there in
/// its system, keeping its orientation.
fn move_mission_player(cx: &mut Cx<'_>, id: ControllerId, position: Vector, radius_sq: f32) {
    let ship = spawn::player_ship_id(cx, id);
    if ship == 0 {
        return;
    }
    let (at, orientation) =
        api::space_obj::location(cx, ship).unwrap_or((Vector::ZERO, Matrix::IDENTITY));
    let dx = at.x - position.x;
    let dy = at.y - position.y;
    let dz = at.z - position.z;
    let distance_sq = f64::from(dy) * f64::from(dy)
        + f64::from(dz) * f64::from(dz)
        + f64::from(dx) * f64::from(dx);
    if !(distance_sq > f64::from(radius_sq)) {
        return;
    }
    // The player's system record is read without a test (D986).
    if let Some(system) = spawn::player_system(cx, id) {
        spawn::relocate_ship(cx, ship, system, position, orientation);
    }
}

/// `Act_Invulnerable::Execute` (`MissionAct.cpp:2908-2985`).
fn invulnerable(cx: &mut Cx<'_>, id: ControllerId, a: &super::super::action::Invulnerable) {
    let key = create_id(c_str(&a.label));
    if key == create_id(PLAYER_LABEL) {
        if script(cx, id).and_then(|s| player_state(cx, s)).is_some() {
            let ship = spawn::player_ship_id(cx, id);
            let _ = api::space_obj::set_invincible(cx, ship, a.invulnerable, a.param3, a.duration);
        }
        return;
    }
    let members = script(cx, id)
        .and_then(|s| s.object.formation_members.get(&key))
        .cloned()
        .unwrap_or_default();
    if members.is_empty() {
        let Some(system) = spawn::player_system(cx, id) else {
            return;
        };
        let object = crate::content::systems::find_object_id_by_name(cx, system, &a.label, id.0);
        if object & ID_MASK != 0 {
            let _ =
                api::space_obj::set_invincible(cx, object, a.invulnerable, a.param3, a.duration);
        }
        return;
    }
    for member in members {
        let _ = api::space_obj::set_invincible(cx, member, a.invulnerable, a.param3, a.duration);
        let name = spawn::mission_object_name(cx, id, member);
        let Some(s) = script_mut(cx, id) else { return };
        let listed = s
            .object
            .invulnerables
            .iter()
            .position(|i| c_str(&i.nickname) == c_str(&name));
        if a.invulnerable {
            if listed.is_none() {
                s.object
                    .invulnerables
                    .push(super::super::object_save::Invulnerable {
                        nickname: name,
                        duration: a.duration,
                        param3: a.param3,
                    });
            }
        } else if let Some(at) = listed {
            s.object.invulnerables.remove(at);
        }
    }
}

/// `Act_MarkObj::Execute` (`MissionAct.cpp:3005-3060`): with no label at or
/// after the key in the label map, the label's object is marked (the
/// reconstruction's reading of baseline's test of the head node, D987);
/// a key with no members does nothing; else each member is marked by its
/// mission name, and an unset member answers false.
fn mark_obj(cx: &mut Cx<'_>, id: ControllerId, label: &[u8], mode: i32) -> bool {
    let key = create_id(c_str(label));
    let Some(s) = script(cx, id) else { return true };
    if s.object.formation_members.range(key..).next().is_none() {
        let object = label_object_id(cx, s, label);
        spawn::mark_mission_object(cx, id, object, mode, label);
        return true;
    }
    let Some(members) = s.object.formation_members.get(&key).cloned() else {
        return true;
    };
    let mut result = true;
    for member in members {
        let name = if let Some(key) = spawn::find_npc_by_id(cx, id, member) {
            cx.logic
                .content
                .state
                .npc_ships
                .get(&key)
                .map(|n| c_str(&n.nickname).to_vec())
                .unwrap_or_default()
        } else {
            let s = script(cx, id);
            s.and_then(|s| s.object.solars.iter().find(|o| o.space_obj_id == member))
                .map(|o| c_str(&o.nickname).to_vec())
                .or_else(|| {
                    s.and_then(|s| s.object.loot.iter().find(|l| l.space_obj_id == member))
                        .map(|l| c_str(&l.nickname).to_vec())
                })
                .unwrap_or_default()
        };
        if member & ID_MASK != 0 {
            spawn::mark_mission_object(cx, id, member, mode, &name);
        } else {
            result = false;
        }
    }
    result
}
