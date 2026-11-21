//! The server's loot wrapper (`Loot`, `Server/server_loot.cpp`) through
//! the world's hooks: the unseen-player check and the contents' decay
//! after the container's own update, the damage of its contents, and its
//! destruction (`ObjectDestroyed`, the explosion and the split of a cargo
//! pod's stack).

use game_sim::equip::{Fate, SUBOBJ_ID_POWER, SUBOBJ_ID_ROOT};
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
