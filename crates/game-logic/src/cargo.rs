//! A ship's cargo on the server (work package P3.3, the object part): the
//! jettison (`Ship::slot_38`, `tick_jettison`, `compute_jettison_launch`,
//! `collect_droppable_equip`'s queue, `Server/server_gameobject.cpp:5128`,
//! `5257`, `5498`, `5729`) and the cargo decay (`EqObj::decay_cargo`,
//! `:4155`, run from `EqObj::update` once its timer passes 1 s).
//!
//! - **The jettison** ([`jettison`]): `CargoJettisoned` goes to the bus;
//!   a stack in a pod loses the pod as loot (fate 4, cause 0x16); any
//!   other is queued, the bay opens and, outside single player, the units
//!   leave the hold. While the bay is open, `tick_jettison` (in the ship's
//!   update, after its own) sends the front of the queue out as one loot
//!   container (mode 1) every `JETTISON_CLEARANCE_TIME` from the bay's
//!   surface hardpoint towards its external one (or along -Y from the
//!   ship), and closes the bay once the queue is empty and the time ran
//!   out. What is still queued drops with the ship when it is destroyed.
//! - **The decay** ([`decay_cargo`]): every stack of a commodity with a
//!   decay rate loses its rate times the elapsed time in hit points (cause
//!   0x14); a spent unit leaves its stack (the world's stack rule, through
//!   `ServerHooks::cargo_stack_rule`), and a spent single unit is
//!   destroyed. Solars' cargo does not decay.
//!
//! - **Cargo added and removed in space** ([`add_cargo`],
//!   [`remove_cargo`]): `StarSystem::add_cargo` and `remove_cargo`
//!   (`Server/server_classes.cpp:10793`, `10990`), which
//!   `pub::Player::AddCargo` and `RemoveCargo` use for a player in space:
//!   the ship's stack and its owner's record change together, and the
//!   observers that see the ship hear of the stack (`OBJECTCARGOUPDATE`).
//!
//! The observers' jettison and cargo notices (`notify_jettison`,
//! [`crate::replication::cargo_updated`]) are replication's; their call
//! sites are in this module.

use game_sim::data::equip_desc::EquipDesc;
use game_sim::equip::{EquipKind, Fate, SubObjId, types};
use game_sim::object::damage::{self, DamageList};
use game_sim::object::world::LootSpawn;
use game_sim::object::{ObjHandle, ObjectKind, ship::BayState};
use neuron_core::math::{Matrix, Vector};
use neuron_server::controller::ControllerId;

use crate::events::ControllerMessage;
use crate::server::Cx;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `RandUnit`'s scale, 2^-15.
const RAND_UNIT: f32 = 1.0 / 32768.0;

/// The cause of the jettison's pod damage list.
pub const CAUSE_JETTISON_POD: damage::DamageCause = 0x16;
/// The cause of the cargo decay's damage list.
pub const CAUSE_CARGO_DECAY: damage::DamageCause = 0x14;

/// `Ship`'s jettison state (`Jettison`, `JettisonTimer`,
/// `PendingBayCloses`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct JettisonState {
    /// `Jettison.Items`: the stacks waiting to leave, front first.
    pub queue: Vec<EquipDesc>,
    /// `JettisonTimer`: the clearance time left (0 when idle).
    pub timer: f32,
    /// `PendingBayCloses`: the bay closes owed for emptied stacks.
    pub pending_bay_closes: i32,
}

fn is_destroyed(cx: &Cx<'_>, h: ObjHandle) -> bool {
    cx.logic.objects.get(h).is_some_and(|o| o.destroyed) || cx.world.get(h).is_none_or(|o| o.dead)
}

fn jettison_state<'c>(cx: &'c mut Cx<'_>, h: ObjHandle) -> Option<&'c mut JettisonState> {
    Some(&mut crate::ship::ship_state(cx, h)?.jettison)
}

/// `Ship::slot_38(EquipId, Count, SourceClient)`, the jettison request: -5
/// for a ship outside a system or destroyed, -2 when the item is not a
/// cargo stack. At most the stack's count (an unsigned comparison) goes;
/// `CargoJettisoned` reaches the bus with the good. A stack in a pod loses
/// the pod (`destroy_equip` with fate 4 in a cause-0x16 list from the ship,
/// applied at once). Otherwise the stack's description with that count is
/// queued (-5 when the stack is empty), the bay opens, outside single
/// player the units leave the stack, and the observers hear of it
/// (`notify_jettison`). `SourceClient` is not read. 0 otherwise.
pub fn jettison(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    equip: SubObjId,
    count: i32,
    source_client: i32,
) -> i32 {
    let _ = source_client;
    let Some(o) = cx.world.get(h) else {
        return -5;
    };
    let Some(system) = o.simple.system else {
        return -5;
    };
    if o.kind != ObjectKind::Ship || is_destroyed(cx, h) {
        return -5;
    }
    let (id, owner) = (o.simple.id, o.simple.owner_player);
    let cargo = o
        .equipped
        .as_deref()
        .and_then(|eq| eq.equipment.by_id(equip))
        .and_then(|e| match &e.kind {
            EquipKind::Cargo(c) => Some((e.arch_id, c.count, c.pod)),
            _ => None,
        });
    let Some((arch_id, stack, pod)) = cargo else {
        return -2;
    };
    let mut amount = count as u32;
    if !(amount < stack) {
        amount = stack;
    }
    let good = cx.world.data().goods.arch_to_good(arch_id);
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::CargoJettisoned {
            ship: id,
            equip,
            good,
            count: amount as i32,
        },
    );
    if let Some(pod) = pod {
        let mut list = DamageList::new(CAUSE_JETTISON_POD, id, owner);
        crate::fuses::compute(cx, h, &mut list, |t, _, _, list| {
            damage::destroy_equip(t, pod, Fate::Loot, list);
        });
        let mut hooks = crate::world::Hooks {
            logic: &mut *cx.logic,
        };
        cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
        return 0;
    }
    let Some(mut desc) = cx.world.equip_desc(h, equip) else {
        return -5;
    };
    desc.count = amount as i32;
    if let Some(j) = jettison_state(cx, h) {
        j.queue.push(desc);
    }
    cx.world.open_bay(h);
    if !cx.world.config().single_player
        && let Some(item) = cx
            .world
            .get_mut(h)
            .and_then(|o| o.equipped.as_deref_mut())
            .and_then(|eq| eq.equipment.by_id_mut(equip))
    {
        let max = item.arch.hit_pts;
        if let EquipKind::Cargo(c) = &mut item.kind {
            c.remove_from_stack(amount, max);
        }
    }
    notify_jettison(cx, system, h, equip, amount as i32);
    0
}

/// `Ship::tick_jettison`: with an empty queue a running timer counts down
/// and, once it is spent, the owed bay closes are made and it stops. With a
/// queue a running timer counts down to 0; at 0, with the bay open, the
/// front stack leaves as one container: a new id, the contents and their
/// hit points (the stack's health of the archetype's), the loot appearance
/// (the contents' hit points when it is the contents itself, else its
/// own), the stack's count, the launch of [`compute_jettison_launch`], a
/// spin from two `rand()` draws about the launch's x and z axes; then
/// `StarSystem::create_loot` (mode 1, from the ship). The stack leaves the
/// queue (one more bay close owed), and the timer restarts at
/// `JETTISON_CLEARANCE_TIME`. A stack whose archetype is unknown leaves
/// without a container (D652).
pub fn tick_jettison(cx: &mut Cx<'_>, h: ObjHandle, dt: f32) {
    let Some(j) = jettison_state(cx, h) else {
        return;
    };
    if j.queue.is_empty() {
        if j.timer == 0.0 {
            return;
        }
        j.timer = r(e(j.timer) - e(dt));
        if j.timer > 0.0 || j.timer.is_nan() {
            return;
        }
        let closes = std::mem::take(&mut j.pending_bay_closes);
        j.timer = 0.0;
        for _ in 0..closes.max(0) {
            cx.world.close_bay(h);
        }
        return;
    }
    if j.timer != 0.0 {
        j.timer = r(e(j.timer) - e(dt));
        if j.timer <= 0.0 {
            j.timer = 0.0;
        }
    }
    if j.timer != 0.0 {
        return;
    }
    let bay_open = cx
        .world
        .get(h)
        .and_then(|o| o.as_ship())
        .is_some_and(|s| s.bay_state == BayState::Open);
    if !bay_open {
        return;
    }
    let Some(desc) = cx
        .logic
        .objects
        .get(h)
        .and_then(|o| o.ship.as_ref())
        .and_then(|s| s.jettison.queue.first().cloned())
    else {
        return;
    };
    let loot_id = cx.world.allocate_id();
    let contents = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&desc.arch_id)
        .cloned();
    if let Some(contents) = contents {
        let appearance = loot_appearance(cx, &contents);
        let contents_hit_pts = r(e(desc.health) * e(contents.root.hit_pts));
        let hit_pts = if appearance.root.nickname == contents.root.nickname {
            contents_hit_pts
        } else {
            appearance.root.hit_pts
        };
        let (position, orientation, velocity) = compute_jettison_launch(cx, h);
        let m = orientation.m;
        let a = r(e(cx.world.rng().rand() as f32 * RAND_UNIT) * e(0.8) - e(0.4));
        let ax = r(e(m[0][0]) * e(a));
        let ay = r(e(m[1][0]) * e(a));
        let az = r(e(m[2][0]) * e(a));
        let b = r(e(cx.world.rng().rand() as f32 * RAND_UNIT) * e(0.8) - e(0.4));
        let angular_velocity = Vector::new(
            r(e(m[0][2]) * e(b) + e(ax)),
            r(e(m[1][2]) * e(b) + e(ay)),
            r(e(m[2][2]) * e(b) + e(az)),
        );
        let (system, ship_id) = cx
            .world
            .get(h)
            .map_or((0, 0), |o| (o.physical.system, o.simple.id));
        let spawn = LootSpawn {
            archetype: appearance.root.nickname,
            contents: contents.root.nickname,
            units: desc.count as u32,
            contents_hit_pts,
            owner: ship_id,
            position,
            velocity,
            id: Some(loot_id),
            hit_pts,
            orientation,
            angular_velocity,
            name_id: 0,
            temporary: false,
            can_ai_tractor: true,
        };
        crate::objects::create_loot_mode(cx, system, ship_id, 1, &[spawn]);
    }
    let Some(j) = jettison_state(cx, h) else {
        return;
    };
    // `set_count(count - Record.Count)`: the whole stack left.
    if let Some(front) = j.queue.first_mut() {
        front.count -= desc.count;
        if front.count == 0 {
            j.pending_bay_closes += 1;
            j.queue.remove(0);
        }
    }
    j.timer = game_sim::object::ship::JETTISON_CLEARANCE_TIME;
}

/// `get_loot_appearance`: a commodity's loot appearance, else the item
/// itself (also when the appearance is not an equipment archetype).
fn loot_appearance(
    cx: &Cx<'_>,
    contents: &game_sim::data::archetype::equipment::Equipment,
) -> game_sim::data::archetype::equipment::Equipment {
    match &contents.kind {
        game_sim::data::archetype::equipment::EquipmentKind::Commodity {
            loot_appearance, ..
        } => loot_appearance
            .and_then(|id| cx.world.data().archetypes.equipment.get(&id).cloned())
            .unwrap_or_else(|| contents.clone()),
        _ => contents.clone(),
    }
}

/// `Ship::compute_jettison_launch`: with both bay hardpoints of the ship's
/// archetype on its instance tree, the surface hardpoint's world position
/// and orientation, and the direction from it to the external one;
/// otherwise the ship's position and orientation and the direction -Y.
/// The direction scaled to `(rand() * 2^-15 * 0.5 + 0.75) *
/// JETTISONED_CARGO_VELOCITY` (the scale stored as a float), plus the
/// ship's velocity. Returns the position, orientation and velocity.
pub fn compute_jettison_launch(cx: &mut Cx<'_>, h: ObjHandle) -> (Vector, Matrix, Vector) {
    let Some(o) = cx.world.get(h) else {
        return (Vector::ZERO, Matrix::IDENTITY, Vector::ZERO);
    };
    let (ship_position, ship_orientation, archetype) =
        (o.engine.position, o.engine.orientation, o.archetype);
    let names = cx.world.data().archetypes.ships.get(&archetype).map(|s| {
        (
            cx.world.strings().get(s.hp_bay_surface).to_vec(),
            cx.world.strings().get(s.hp_bay_external).to_vec(),
        )
    });
    let bay = names.and_then(|(surface, external)| {
        let surface = cx.world.hardpoint_world(h, &surface)?;
        let external = cx.world.hardpoint_world(h, &external)?;
        Some((surface, external.0))
    });
    let (position, orientation, dir) = match bay {
        Some(((position, orientation), external)) => (
            position,
            orientation,
            Vector::new(
                r(e(external.x) - e(position.x)),
                r(e(external.y) - e(position.y)),
                r(e(external.z) - e(position.z)),
            ),
        ),
        None => {
            let m = ship_orientation.m;
            (
                ship_position,
                ship_orientation,
                Vector::new(-m[0][1], -m[1][1], -m[2][1]),
            )
        }
    };
    let draw = cx.world.rng().rand() as f32 * RAND_UNIT;
    let velocity_constant = cx.world.data().constants.general.jettisoned_cargo_velocity;
    let speed = (e(draw) * 0.5 + 0.75) * e(velocity_constant);
    let length = ((e(dir.x) * e(dir.x) + e(dir.y) * e(dir.y)) + e(dir.z) * e(dir.z)).sqrt();
    let scale = r(speed / length);
    let push = Vector::new(
        r(e(scale) * e(dir.x)),
        r(e(scale) * e(dir.y)),
        r(e(scale) * e(dir.z)),
    );
    let ship_velocity = cx.world.velocity(h);
    let velocity = Vector::new(
        r(e(ship_velocity.x) + e(push.x)),
        r(e(ship_velocity.y) + e(push.y)),
        r(e(ship_velocity.z) + e(push.z)),
    );
    (position, orientation, velocity)
}

/// `Ship::collect_droppable_equip`'s own part: the stacks still waiting to
/// be jettisoned, after `EqObj`'s items.
#[must_use]
pub fn queued_jettison(cx: &Cx<'_>, h: ObjHandle) -> Vec<EquipDesc> {
    cx.logic
        .objects
        .get(h)
        .and_then(|o| o.ship.as_ref())
        .map(|s| s.jettison.queue.clone())
        .unwrap_or_default()
}

/// `EqObj::decay_cargo(Elapsed)`: nothing for a solar or an object without
/// decaying cargo (`DecayingCargoCount`); a player's open trade freezes it
/// (trades are not ported yet: none is open). Each stack with units whose
/// decay rate times `elapsed` (stored as a float) is positive takes an
/// entry in a cause-0x14 list: its hit points less the amount when that
/// stays positive (and does not round to 0); otherwise 0, with fate
/// destroyed for a single unit. A list with entries is applied.
pub fn decay_cargo(cx: &mut Cx<'_>, h: ObjHandle, elapsed: f32) {
    let Some(o) = cx.world.get(h) else {
        return;
    };
    if o.kind == ObjectKind::Solar {
        return;
    }
    let Some(eq) = o.equipped.as_deref() else {
        return;
    };
    if eq.equipment.decaying_cargo <= 0 {
        return;
    }
    let mut list = DamageList::new(CAUSE_CARGO_DECAY, 0, 0);
    for slot in eq.equipment.traverse(types::CARGO, false) {
        let Some(item) = eq.equipment.get(slot) else {
            continue;
        };
        let EquipKind::Cargo(c) = &item.kind else {
            continue;
        };
        if c.count == 0 {
            continue;
        }
        let amount = r(e(c.decay_per_second) * e(elapsed));
        if amount <= 0.0 || amount.is_nan() {
            continue;
        }
        let left = e(c.hit_pts) - e(amount);
        if left > 0.0 {
            let hit_pts = r(left);
            if hit_pts != 0.0 {
                list.add_damage_entry(item.id, hit_pts, Fate::None);
                continue;
            }
        }
        let fate = if c.count == 1 {
            Fate::Destroyed
        } else {
            Fate::None
        };
        list.add_damage_entry(item.id, 0.0, fate);
    }
    if list.entries.is_empty() {
        return;
    }
    let mut hooks = crate::world::Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(h, &list, &mut hooks);
}

/// `StarSystem::notify_jettison(Obj, EquipId, Count)`
/// (`Server/server_classes.cpp:10118`): the owner's record and the
/// observers' `JETTISONCARGO`, through replication's fan-out.
fn notify_jettison(cx: &mut Cx<'_>, system: u32, ship: ObjHandle, equip: SubObjId, count: i32) {
    crate::replication::cargo_jettisoned(cx, system, ship, equip, count);
}

const STAR_SYS: &str = "Server\\space\\StarSys.cpp";

/// The slot `add_cargo` and `remove_cargo` update: the ship's
/// `OwnerPlayer`, when that slot has a client and this ship as its ship.
fn owning_session(cx: &Cx<'_>, h: ObjHandle) -> Option<u32> {
    let o = cx.world.get(h)?;
    let (owner, id) = (o.simple.owner_player, o.simple.id);
    (owner != 0
        && cx
            .logic
            .players
            .get(owner)
            .is_some_and(|p| p.client_id != 0 && p.ship_object_id == id))
    .then_some(owner)
}

/// The id of the first working cargo stack of the archetype
/// (`CEquipTraverser(EquipTypeCargo)`), and whether it holds units.
fn first_stack_of(cx: &Cx<'_>, h: ObjHandle, arch: u32) -> Option<(SubObjId, bool)> {
    let eq = cx.world.get(h)?.equipped.as_deref()?;
    eq.equipment
        .traverse(types::CARGO, false)
        .into_iter()
        .filter_map(|slot| eq.equipment.get(slot))
        .find(|item| item.arch_id == arch)
        .map(|item| {
            let units = matches!(&item.kind, EquipKind::Cargo(c) if c.count != 0);
            (item.id, units)
        })
}

/// The stack's id when the item with the id is a cargo stack.
fn cargo_stack(cx: &Cx<'_>, h: ObjHandle, id: SubObjId) -> Option<SubObjId> {
    let eq = cx.world.get(h)?.equipped.as_deref()?;
    let item = eq.equipment.by_id(id)?;
    matches!(item.kind, EquipKind::Cargo(_)).then_some(item.id)
}

/// `StarSystem::add_cargo(Obj, ArchId, Count, Health, bMission)`: units of a
/// good join the ship's hold.
///
/// - A combinable good, unless `mission`, joins the first working stack of
///   its archetype when that stack holds units (`AddToStack`); a found
///   stack without units makes the ship drop its destroyed items
///   (`CEquipManager::CleanUp`) and a new stack follows. The owner's
///   record takes the units as cargo (no notice, no `no_combine`).
/// - Otherwise a new internal stack (`mission` marks it) is added with the
///   id the owner's record gives it (`no_combine` is `mission`), or, for a
///   ship no valid player owns, the ship's own next id
///   ([`crate::ship::allocate_equip_id`]).
///
/// Then every player of the system that sees the ship hears of the stack
/// ([`crate::replication::cargo_updated`]); without a stack that has units
/// the failure is reported instead. Answers the stack's id, or
/// `SUBOBJ_ID_NONE` when there is none.
///
/// The C++ reads a ship without equipment through null (none of a server's
/// ships lacks it; here nothing is added) and runs on a null system for a
/// ship in transit; the port adds the stack and tells nobody (D694).
pub fn add_cargo(
    cx: &mut Cx<'_>,
    h: ObjHandle,
    arch: u32,
    count: u32,
    health: f32,
    mission: bool,
) -> SubObjId {
    let max = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&arch)
        .map_or(0.0, |a| a.root.hit_pts);
    let mut stack = None;
    let mut combined = false;
    if cx.world.data().goods.arch_is_combinable(arch) && !mission {
        match first_stack_of(cx, h, arch) {
            Some((id, true)) => {
                if let Some(c) = cx
                    .world
                    .get_mut(h)
                    .and_then(|o| o.equipped.as_deref_mut())
                    .and_then(|eq| eq.equipment.by_id_mut(id))
                    .and_then(|item| match &mut item.kind {
                        EquipKind::Cargo(c) => Some(c),
                        _ => None,
                    })
                {
                    c.add_to_stack(count, health, max);
                }
                stack = Some(id);
                combined = true;
            }
            Some((_, false)) => {
                let solar = cx.world.get(h).is_some_and(|o| o.kind == ObjectKind::Solar);
                if let Some(eq) = cx.world.get_mut(h).and_then(|o| o.equipped.as_deref_mut()) {
                    eq.equipment.clean_up(solar);
                }
            }
            None => {}
        }
    }
    let session = owning_session(cx, h);
    let bay = cx.world.strings().cargo_bay();
    if combined {
        if let Some(session) = session {
            crate::player::add_equipment(
                cx,
                session,
                arch,
                bay,
                count as i32,
                health,
                0,
                false,
                false,
            );
        }
    } else {
        let id = match session {
            Some(session) => crate::player::add_equipment(
                cx,
                session,
                arch,
                bay,
                count as i32,
                health,
                0,
                mission,
                false,
            ),
            None => crate::ship::allocate_equip_id(cx, h),
        };
        let mut desc = EquipDesc::new(cx.world.strings());
        desc.id = id;
        desc.arch_id = arch;
        desc.count = count as i32;
        desc.health = health;
        desc.equipped = 0;
        desc.mission = mission;
        desc.make_internal(cx.world.strings());
        if cx.world.get(h).is_some_and(|o| o.equipped.is_some()) {
            cx.world.add_equip_item(h, &desc);
            stack = cargo_stack(cx, h, id);
        }
    }
    let described = stack.and_then(|id| cx.world.equip_desc(h, id));
    match described {
        Some(desc) => {
            let (system, object) = cx
                .world
                .get(h)
                .map_or((0, 0), |o| (o.physical.system, o.simple.id));
            crate::replication::cargo_updated(cx, system, object, &desc);
        }
        None => cx.logic.reports.push(
            STAR_SYS,
            0xd8e,
            "*** ERROR: StarSys::add_cargo().  Failed to get client cargo description.",
        ),
    }
    stack.unwrap_or(game_sim::equip::SUBOBJ_ID_NONE)
}

/// `StarSystem::remove_cargo(Obj, EquipId, Count)`: units leave a cargo
/// stack of the ship (`RemoveFromStack`; the top unit is whole again).
/// Nothing happens when the item is not a stack or holds no units.
/// Otherwise the owner's record loses them (no notice) and every player of
/// the system that sees the ship hears of the stack, with what is left
/// (`OBJECTCARGOUPDATE`; a stack left empty travels with count 0). A ship
/// in transit tells nobody (D694).
pub fn remove_cargo(cx: &mut Cx<'_>, h: ObjHandle, id: SubObjId, count: i32) {
    let Some(stack) = cargo_stack(cx, h, id) else {
        return;
    };
    let Some(mut desc) = cx.world.equip_desc(h, stack) else {
        // The stack holds no units.
        return;
    };
    let max = cx
        .world
        .data()
        .archetypes
        .equipment
        .get(&desc.arch_id)
        .map_or(0.0, |a| a.root.hit_pts);
    let Some(item) = cx
        .world
        .get_mut(h)
        .and_then(|o| o.equipped.as_deref_mut())
        .and_then(|eq| eq.equipment.by_id_mut(stack))
    else {
        return;
    };
    if let EquipKind::Cargo(c) = &mut item.kind {
        c.remove_from_stack(count as u32, max);
        desc.count = c.count as i32;
    }
    desc.health = item.relative_health() as f32;
    if let Some(session) = owning_session(cx, h) {
        crate::player::remove_equipment(cx, session, id, count, false);
    }
    let (system, object) = cx
        .world
        .get(h)
        .map_or((0, 0), |o| (o.physical.system, o.simple.id));
    crate::replication::cargo_updated(cx, system, object, &desc);
}
