//! The server's loot wrapper (`Loot`, `Server/server_loot.cpp`) through
//! the world's hooks: the unseen-player check and the contents' decay
//! after the container's own update, the damage of its contents, and its
//! destruction (`ObjectDestroyed`, the explosion and the split of a cargo
//! pod's stack).

use game_sim::equip::{Fate, SUBOBJ_ID_POWER, SUBOBJ_ID_ROOT, SubObjId};
use game_sim::object::damage::DamageList;
use game_sim::object::{KindData, ObjHandle, ObjectKind};
use neuron_server::controller::ControllerId;

use crate::events::{ControllerMessage, ObjectDestroyed};
use crate::server::Cx;
use crate::world::Hooks;

/// The cause of the decay's damage list.
const CAUSE_DECAY: game_sim::object::damage::DamageCause = 0x14;

/// `Loot`'s own fields.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LootState {
    /// `CheckTimer`: time since the last unseen-player check.
    pub check_timer: f32,
    /// `UnseenLife`: time left while no player is near.
    pub unseen_life: f32,
    /// `DecayAccum`: the contents' accumulated decay.
    pub decay_accum: f32,
}

/// The loot state of a container, made with the constructor's values.
fn state(cx: &mut Cx<'_>, h: ObjHandle) -> Option<LootState> {
    let life = cx.world.data().constants.general.loot_unseen_life_time;
    let o = cx.logic.objects.entry(cx.world, h)?;
    Some(*o.loot.get_or_insert(LootState {
        check_timer: 0.0,
        unseen_life: life,
        decay_accum: 0.0,
    }))
}

fn set_state(cx: &mut Cx<'_>, h: ObjHandle, s: LootState) {
    if let Some(o) = cx.logic.objects.entry(cx.world, h) {
        o.loot = Some(s);
    }
}

fn destroyed(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.logic.objects.get(h).is_some_and(|o| o.destroyed) || cx.world.get(h).is_none_or(|o| o.dead)
}

/// `Loot::update` after the container's own update: the decay, then,
/// unless the container is invincible, the unseen check every 3 s: a
/// player's ship within `LOOT_UNSEEN_RADIUS` restores its unseen life,
/// and a container whose unseen life ran out is destroyed (kind 0).
pub fn after_update(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) {
    if destroyed(cx, h) {
        return;
    }
    update_decay(cx, h, dt);
    if destroyed(cx, h) {
        return;
    }
    let invincible = cx.logic.objects.get(h).is_some_and(|o| o.invincible);
    if invincible {
        return;
    }
    let Some(mut s) = state(cx, h) else {
        return;
    };
    let check = f64::from(dt) + f64::from(s.check_timer);
    s.check_timer = check as f32;
    s.unseen_life = (f64::from(s.unseen_life) - f64::from(dt)) as f32;
    if check > 3.0 {
        let g = &cx.world.data().constants.general;
        let (radius, life) = (g.loot_unseen_radius, g.loot_unseen_life_time);
        let radius_sq = (f64::from(radius) * f64::from(radius)) as f32;
        let Some(o) = cx.world.get(h) else {
            return;
        };
        let (system, mine) = (o.physical.system, o.engine.position);
        for other in cx.world.objects_in(system) {
            let Some(p) = cx.world.get(other) else {
                continue;
            };
            if !p.as_ship().is_some_and(|s| s.is_player) {
                continue;
            }
            let t = p.engine.position;
            let d = [
                f64::from(t.x) - f64::from(mine.x),
                f64::from(t.y) - f64::from(mine.y),
                f64::from(t.z) - f64::from(mine.z),
            ];
            if (d[0] * d[0] + d[2] * d[2]) + d[1] * d[1] < f64::from(radius_sq) {
                s.unseen_life = life;
            }
        }
        if s.unseen_life < 0.0 {
            set_state(cx, h, s);
            let id = cx.world.get(h).map_or(0, |o| o.simple.id);
            crate::world::destroy_object(cx, h, 0, id);
            return;
        }
        s.check_timer = 0.0;
    }
    set_state(cx, h, s);
}

/// `Loot::update_decay`: a commodity's decay per second accumulates; past
/// the contents' hit points a single unit is destroyed with its container
/// (kind 1), otherwise one unit goes (a cause 0x14 list) and the
/// accumulator drops by the hit points.
fn update_decay(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let KindData::Loot(l) = &o.data else {
        return;
    };
    let (contents, units, id) = (l.contents_arch, l.units, o.simple.id);
    let Some(arch) = cx.world.data().archetypes.equipment.get(&contents) else {
        return;
    };
    let game_sim::data::archetype::equipment::EquipmentKind::Commodity {
        decay_per_second, ..
    } = &arch.kind
    else {
        return;
    };
    let decay = f64::from(*decay_per_second) * f64::from(dt);
    let hit_pts = arch.root.hit_pts;
    // `!(decay > 0)`: a NaN decay is none.
    if decay <= 0.0 || decay.is_nan() {
        return;
    }
    let Some(mut s) = state(cx, h) else {
        return;
    };
    let accum = decay + f64::from(s.decay_accum);
    s.decay_accum = accum as f32;
    set_state(cx, h, s);
    let full = f64::from(hit_pts);
    if accum <= full || accum.is_nan() || full.is_nan() {
        return;
    }
    if units == 1 {
        crate::world::destroy_object(cx, h, 1, id);
        return;
    }
    let mut list = DamageList::new(CAUSE_DECAY, 0, 0);
    list.add_damage_entry(SUBOBJ_ID_POWER, 0.0, Fate::None);
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
    if let Some(mut s) = state(cx, h) {
        s.decay_accum = (f64::from(s.decay_accum) - f64::from(hit_pts)) as f32;
        set_state(cx, h, s);
    }
}

/// `Loot::apply_damage_list`'s entries other than the root's: a positive
/// value is the contents' hit points, otherwise one unit goes (the count
/// wraps at 0, as the C++'s unsigned does) and the contents' hit points
/// are the archetype's.
pub fn apply_contents(cx: &mut Cx<'_>, h: ObjHandle, list: &DamageList) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    let KindData::Loot(l) = &o.data else {
        return;
    };
    let full = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&l.contents_arch)
        .map_or(0.0, |a| a.root.hit_pts);
    let Some(o) = cx.world.get_mut(h) else {
        return;
    };
    let KindData::Loot(l) = &mut o.data else {
        return;
    };
    for entry in &list.entries {
        if entry.sub_obj_id == SUBOBJ_ID_ROOT {
            continue;
        }
        if entry.hit_pts > 0.0 {
            l.contents_hit_pts = entry.hit_pts;
        } else {
            l.units = l.units.wrapping_sub(1);
            l.contents_hit_pts = full;
        }
    }
}

/// `Loot::destroy`: `ObjectDestroyed` (no owner, nothing dropped), the
/// base destruction, then, destroyed by damage (kind 1), the container's
/// explosion and the split of a cargo pod's remaining stack.
pub fn destroy(cx: &mut Cx<'_>, h: ObjHandle, kind: i32, killer: u32) -> bool {
    let Some(o) = cx.world.get(h) else {
        return true;
    };
    if o.kind != ObjectKind::Loot {
        return true;
    }
    let (id, system, position) = (o.simple.id, o.physical.system, o.engine.position);
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::ObjectDestroyed(ObjectDestroyed {
            id,
            killer,
            system,
            owner_player: 0,
            kind,
            position,
            dropped_equip: Vec::new(),
        }),
    );
    crate::objects::base_destroy(cx, h, kind);
    if kind == 1 {
        let mut hooks = Hooks {
            logic: &mut *cx.logic,
        };
        cx.world.explode_loot_hooked(h, &mut hooks);
        let spawns = cx.world.loot_split(h);
        if !spawns.is_empty() {
            crate::objects::create_loot(cx, system, id, &spawns);
        }
    }
    true
}

/// `SrvTractor::notify` (`Server/server_classes.cpp:6461-6500`) with event
/// 0x18, an arm's consume timer ran out (`TractorArm::Update`): with a
/// target, the ship's room for the contents
/// (`CShip::get_space_for_cargo_type`); with room, the loot in the
/// ship's system (`find_object` with the loot type bits) that is not being
/// destroyed goes into the hold ([`tractor_loot`]). The C++ reads an owner
/// that is not a ship through null; the port's tractors are on ships, and
/// an object without a hold takes nothing. The event reaches the server
/// after the ship's equipment pass (D653).
pub fn tractor_consume(cx: &mut Cx<'_>, ship: ObjHandle, tractor: SubObjId, target: ObjHandle) {
    let Some(contents) = cx.world.get(target).and_then(|o| match &o.data {
        KindData::Loot(l) => Some(l.contents_arch),
        _ => None,
    }) else {
        return;
    };
    let Some(space) = cx.world.space_for_cargo_type(ship, contents) else {
        return;
    };
    if space == 0 {
        return;
    }
    let Some(system) = cx.world.get(ship).map(|o| o.physical.system) else {
        return;
    };
    let id = cx.world.get(target).map_or(0, |o| o.simple.id);
    let Some(loot) = crate::replication::find_in_system(cx.world, system, id)
        .filter(|&h| cx.world.get(h).is_some_and(|o| o.kind == ObjectKind::Loot))
    else {
        return;
    };
    if destroyed(cx, loot) {
        return;
    }
    tractor_loot(cx, loot, ship, space as u32, tractor);
}

/// `StarSystem::tractor_loot(Loot, Ship, Space, TractorId)`
/// (`Server/server_classes.cpp:6337-6447`): the ship takes the smaller of
/// its room and the units.
///
/// - The health the units carry: a good that does not combine keeps its
///   contents' hit points over the archetype's; a commodity that decays
///   and is taken whole keeps what its decay left (the contents' hit
///   points less the decay so far, 0 unless positive, over the
///   archetype's); any other is whole (1).
/// - The units join the hold ([`crate::cargo::add_cargo`], mission cargo
///   when the loot is), then the controllers hear of it (`LootAcquired`:
///   the loot, the ship, the stack's id, the good, the units), then every
///   player of the system that can see the ship (`ITEMTRACTORED`,
///   [`crate::replication::item_tractored`]).
/// - Part of a stack leaves the rest in space. Otherwise the ship's
///   tractor lets go of the loot (`CETractor::RemoveTarget`), except a
///   player's ship's in single player, and the loot is destroyed (kind 2,
///   by the ship) with no units left.
pub fn tractor_loot(
    cx: &mut Cx<'_>,
    loot: ObjHandle,
    ship: ObjHandle,
    space: u32,
    tractor: SubObjId,
) {
    use game_sim::data::archetype::equipment::EquipmentKind;
    let Some(o) = cx.world.get(loot) else {
        return;
    };
    let KindData::Loot(l) = &o.data else {
        return;
    };
    let (loot_id, contents, units, contents_hit_pts, temporary) = (
        o.simple.id,
        l.contents_arch,
        l.units,
        l.contents_hit_pts,
        l.temporary,
    );
    let take = space.min(units);
    let data = cx.world.data_arc();
    let (arch_hit_pts, decays) =
        data.archetypes
            .equipment
            .get(&contents)
            .map_or((0.0, false), |a| {
                let decays = matches!(
                    a.kind,
                    EquipmentKind::Commodity { decay_per_second, .. } if decay_per_second > 0.0
                );
                (a.root.hit_pts, decays)
            });
    let mut health = 1.0f32;
    if !data.goods.arch_is_combinable(contents) {
        health = (f64::from(contents_hit_pts) / f64::from(arch_hit_pts)) as f32;
    } else if decays && take == units {
        let accum = cx
            .logic
            .objects
            .get(loot)
            .and_then(|o| o.loot)
            .map_or(0.0, |s| s.decay_accum);
        let mut remaining = f64::from(contents_hit_pts) - f64::from(accum);
        // `FCOM / TEST AH,0x41 / JZ`: 0 unless strictly positive (NaN too).
        if remaining <= 0.0 || remaining.is_nan() {
            remaining = 0.0;
        }
        health = (remaining / f64::from(arch_hit_pts)) as f32;
    }
    let stack = crate::cargo::add_cargo(cx, ship, contents, take, health, temporary);
    let (ship_id, system, owner) = cx.world.get(ship).map_or((0, 0, 0), |o| {
        (o.simple.id, o.physical.system, o.simple.owner_player)
    });
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::LootAcquired {
            loot: loot_id,
            ship: ship_id,
            sub_object: stack,
            good: data.goods.arch_to_good(contents),
            count: take as i32,
        },
    );
    crate::replication::item_tractored(cx, system, loot_id, ship_id, tractor, take);
    if take != units {
        if let Some(KindData::Loot(l)) = cx.world.get_mut(loot).map(|o| &mut o.data) {
            l.units = units - take;
        }
        return;
    }
    if !(owner != 0 && cx.world.config().single_player) {
        cx.world.tractor_remove_target(ship, tractor, loot_id);
    }
    crate::world::destroy_object(cx, loot, 2, ship_id);
    if let Some(KindData::Loot(l)) = cx.world.get_mut(loot).map(|o| &mut o.data) {
        l.units = 0;
    }
}

/// `Archetype::Equipment::get_loot_appearance` (`Common/
/// common_archetype_equipment.cpp:383-428`): a commodity's, repair kit's
/// or shield battery's loot appearance when it has one, else the item
/// itself. An appearance the data lacks reads as none.
#[must_use]
pub fn loot_appearance(world: &game_sim::object::World, contents: u32) -> u32 {
    use game_sim::data::archetype::equipment::EquipmentKind;
    let data = world.data();
    let appearance = match data.archetypes.equipment.get(&contents).map(|a| &a.kind) {
        Some(
            EquipmentKind::Commodity {
                loot_appearance, ..
            }
            | EquipmentKind::RepairKit { loot_appearance }
            | EquipmentKind::ShieldBattery { loot_appearance },
        ) => *loot_appearance,
        _ => None,
    };
    appearance
        .filter(|id| data.archetypes.equipment.contains_key(id))
        .unwrap_or(contents)
}

/// `StarSystem::spawn_mined_loot(Pos, ContainerArchId, ContentsArchId,
/// Count)` (`Server/server_classes.cpp:6235`, `IServerImpl::MineAsteroid`):
/// a new object id is taken first (and spent on the early outs); an
/// unknown contents archetype makes nothing. The container is the
/// container archetype when it is not 0 and known, else the contents'
/// loot appearance; one container of `count` units with the archetypes'
/// hit points, at the position with the identity orientation, a velocity
/// drawn inside a sphere of 20 and a spin inside one of 3, tractorable by
/// the AI, is created from nobody in mode 3
/// ([`crate::objects::create_loot_mode`]).
pub fn spawn_mined_loot(
    cx: &mut Cx<'_>,
    system: u32,
    position: neuron_core::math::Vector,
    container: u32,
    contents: u32,
    count: i32,
) {
    let id = cx.world.allocate_id();
    let Some(contents_hit_pts) = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&contents)
        .map(|a| a.root.hit_pts)
    else {
        return;
    };
    let container = if container != 0
        && cx
            .world
            .data()
            .archetypes
            .equipment
            .contains_key(&container)
    {
        container
    } else {
        loot_appearance(cx.world, contents)
    };
    let hit_pts = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&container)
        .map_or(0.0, |a| a.root.hit_pts);
    let velocity = game_sim::object::csys::random_inside_sphere(20.0, cx.world.rng());
    let angular_velocity = game_sim::object::csys::random_inside_sphere(3.0, cx.world.rng());
    let spawn = game_sim::object::world::LootSpawn {
        archetype: container,
        contents,
        units: count as u32,
        contents_hit_pts,
        owner: 0,
        position,
        velocity,
        id: Some(id),
        hit_pts,
        orientation: neuron_core::math::Matrix::IDENTITY,
        angular_velocity,
        name_id: 0,
        temporary: false,
        can_ai_tractor: true,
    };
    crate::objects::create_loot_mode(cx, system, 0, 3, &[spawn]);
}
