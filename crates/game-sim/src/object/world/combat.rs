//! Weapons and damage in the world: firing guns (`EqObj::slot_34`,
//! `SrvGun::fire`, `launch_bullets`), moving bullets (`CBeam::move`), the
//! collision pass (`StarSystem_ProcessCollisions`: beam hits,
//! `GameObject::on_collision`, `StarSystem::collision_damage`,
//! `munition_collision`) and the application of a damage list
//! (`EqObj::apply_damage_list`). The server's bookkeeping around these
//! (fuses, loot drops, controller messages) belongs to P3.3.

use neuron_core::math::Vector;
use neuron_core::physics::{BeamQuery, calc_collision_damage_scaled};

use crate::equip::launcher::{FireResult, LaunchContext, ProjLaunchInfo, ProjectileArch};
use crate::equip::{EquipKind, Fate, SUBOBJ_ID_POWER, SUBOBJ_ID_ROOT, SubObjId, types};
use crate::object::damage::{self, CAUSE_GUN, DamageList, DamageTarget, GroupState};
use crate::object::{KindData, ObjHandle, ObjectKind, class};

use super::spawn::ProjectileSpawn;
use super::{TickList, World, WorldEvent};

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

impl World {
    /// The launch context of one launcher of an equipped object.
    fn launch_context(&self, h: ObjHandle, id: SubObjId) -> Option<LaunchContext> {
        let o = self.objects.get(h)?;
        let eq = o.equipped.as_deref()?;
        let equip = eq.equipment.by_id(id)?;
        let launcher = match &equip.kind {
            EquipKind::Gun(g) => &g.launcher,
            EquipKind::MineDropper(l) | EquipKind::CmDropper(l) => l,
            _ => return None,
        };
        let ammo = match launcher.projectile {
            Some(p) if p.requires_ammo => eq
                .equipment
                .traverse(types::CARGO, false)
                .into_iter()
                .filter_map(|s| eq.equipment.get(s))
                .find(|c| c.arch_id == p.nickname)
                .map_or(0, |c| match &c.kind {
                    EquipKind::Cargo(cargo) => i32::try_from(cargo.count).unwrap_or(i32::MAX),
                    _ => 0,
                }),
            _ => -1,
        };
        let ship = o.as_ship();
        let cruise_active = eq
            .equipment
            .first(types::ENGINE)
            .is_some_and(|e| matches!(&e.kind, EquipKind::Engine(s) if s.cruise_active));
        Some(LaunchContext {
            destroyed: equip.is_destroyed(),
            disabled: equip.is_disabled(eq.weapons_offline),
            owner_power: eq.power,
            ammo,
            cloak_percent: eq.cloak_percent,
            using_tradelane: ship.is_some_and(|s| s.using_tradelane),
            cruise_active: ship.is_some() && cruise_active,
            owner_velocity: self.velocity(h),
            barrel_frames: launcher
                .barrels
                .iter()
                .map(|b| self.instance_frame(b.instance))
                .collect(),
            muzzle_cone_angle: self.data.constants.general.muzzle_cone_angle,
        })
    }

    /// `get_velocity` of an object: its body's (outside single player a
    /// remotely corrected body reports its dead reckoning), a bullet's own,
    /// zero for solars and asteroids.
    #[must_use]
    pub fn velocity(&self, h: ObjHandle) -> Vector {
        let Some(o) = self.objects.get(h) else {
            return Vector::ZERO;
        };
        match &o.data {
            KindData::Beam(b) => return b.velocity,
            KindData::Solar(_) | KindData::Asteroid(_) => return Vector::ZERO,
            _ => {}
        }
        match (o.physical.body, self.physics.world(o.physical.system)) {
            (Some(body), Some(w)) => w.velocity(body, self.config.single_player),
            _ => Vector::ZERO,
        }
    }

    /// `get_mass`: the body's, a bullet's 0, a solar's archetype mass.
    #[must_use]
    pub fn mass(&self, h: ObjHandle) -> f32 {
        let Some(o) = self.objects.get(h) else {
            return 0.0;
        };
        match &o.data {
            KindData::Beam(_) => return 0.0,
            KindData::Solar(_) => return o.arch_mass,
            _ => {}
        }
        match (o.physical.body, self.physics.world(o.physical.system)) {
            (Some(body), Some(w)) => w.mass(body).unwrap_or(o.arch_mass),
            _ => o.arch_mass,
        }
    }

    /// `EqObj::slot_34` with `SrvGun::fire`: each listed gun fires at the
    /// aim point (`CELauncher::Fire`, `ConsumeFireResources`); an unguided
    /// munition goes out as bullets (`launch_bullets`), a guided one as
    /// missiles (`launch_guided`). Returns each gun's result.
    pub fn fire(&mut self, h: ObjHandle, guns: &[SubObjId], target: Vector) -> Vec<(SubObjId, FireResult)> {
        let mut out = Vec::new();
        let Some(o) = self.objects.get(h) else {
            return out;
        };
        if o.dead {
            return out;
        }
        let system = o.physical.system;
        let owner_id = o.simple.id;
        for &id in guns {
            let Some(cx) = self.launch_context(h, id) else {
                continue;
            };
            let mut shots: Vec<ProjLaunchInfo> = Vec::new();
            let mut munition: Option<ProjectileArch> = None;
            let mut result = FireResult::Destroyed;
            let mut rng = std::mem::take(&mut self.rng);
            if let Some(eq) = self.objects.get_mut(h).and_then(|o| o.equipped.as_deref_mut()) {
                let crate::object::Equipped { equipment, power, .. } = eq;
                let ammo_arch = equipment.by_id(id).and_then(|e| match &e.kind {
                    EquipKind::Gun(g) => g.launcher.projectile,
                    _ => None,
                });
                if let Some(equip) = equipment.by_id_mut(id)
                    && let EquipKind::Gun(g) = &mut equip.kind
                {
                    result = g.fire(&cx, target);
                    if result == FireResult::Ok {
                        g.launcher.draw_power(power);
                        g.lock_elapsed = 0.0;
                        shots = g.launch_info(&cx, target, &mut rng);
                        munition = g.launcher.projectile;
                    }
                }
                // `DrawAmmoFromCargo`.
                if result == FireResult::Ok
                    && let Some(p) = ammo_arch
                    && p.requires_ammo
                {
                    let count = shots.len() as u32;
                    for slot in equipment.traverse(types::CARGO, false) {
                        if let Some(c) = equipment.get_mut(slot)
                            && c.arch_id == p.nickname
                        {
                            let max = c.arch.hit_pts;
                            if let EquipKind::Cargo(cargo) = &mut c.kind {
                                cargo.remove_from_stack(count, max);
                            }
                            break;
                        }
                    }
                }
            }
            self.rng = rng;
            out.push((id, result));
            let Some(m) = munition else {
                continue;
            };
            for shot in shots {
                let spawn = ProjectileSpawn {
                    archetype: m.nickname,
                    owner: owner_id,
                    position: shot.pos,
                    orientation: shot.orient,
                    velocity: shot.velocity,
                    target: shot.lock,
                };
                let made = if m.seeker != 0 {
                    self.create_guided(system, &spawn)
                } else {
                    self.create_bullet(system, &spawn)
                };
                if let Ok(b) = made {
                    let bullet = self.objects.get(b).map_or(0, |o| o.simple.id);
                    self.events.push(WorldEvent::Fired {
                        shooter: owner_id,
                        equip: id,
                        bullet,
                    });
                }
            }
        }
        out
    }

    /// The multiplayer step's beam pass: `CBeam::move` on every bullet.
    pub(crate) fn move_beams(&mut self, system: u32, step: f32) {
        for h in self.list(system, TickList::Beams) {
            if let Some(o) = self.objects.get_mut(h)
                && let KindData::Beam(b) = &mut o.data
            {
                b.advance(step, &mut o.engine.position);
            }
        }
    }

    /// `StarSystem_ProcessCollisions`: every bullet's ray
    /// (`PhySys::GenerateCollisions`), then every collision event, each side
    /// reporting itself (`on_collision`).
    pub(crate) fn process_collisions(&mut self, system: u32) {
        for h in self.list(system, TickList::Beams) {
            let Some(o) = self.objects.get(h) else {
                continue;
            };
            let KindData::Beam(b) = &o.data else {
                continue;
            };
            if o.dead {
                continue;
            }
            let pos = o.engine.position;
            let source = b.last_pos(pos);
            let dx = r(e(pos.x) - e(source.x));
            let dy = r(e(pos.y) - e(source.y));
            let dz_exact = e(pos.z) - e(source.z);
            let dz = r(dz_exact);
            let length_x87 = ((dz_exact * e(dz) + e(dx) * e(dx)) + e(dy) * e(dy)).sqrt();
            let length = r(length_x87);
            if !(length_x87 > 0.0001) {
                continue;
            }
            let inv = 1.0 / e(length);
            let direction = Vector::new(r(e(dx) * inv), r(e(dy) * inv), r(e(dz) * inv));
            let query = BeamQuery {
                user_key: h.to_key(),
                start: source,
                direction,
                length,
                collision_group: b.collision_group.clone(),
                velocity: b.velocity,
            };
            if let Some(world) = self.physics.world_mut(system) {
                let _ = world.generate_beam_collision(&query);
            }
        }
        let events: Vec<_> = self
            .physics
            .world(system)
            .map(|w| w.collisions().to_vec())
            .unwrap_or_default();
        for ev in events {
            let (a, b) = (ObjHandle::from_key(ev.object_a), ObjHandle::from_key(ev.object_b));
            let (Some(sub_a), Some(sub_b)) = (self.part_sub_obj(a, ev.part_id_a), self.part_sub_obj(b, ev.part_id_b)) else {
                continue;
            };
            self.on_collision(a, sub_a, b, sub_b, ev.point);
            self.on_collision(b, sub_b, a, sub_a, ev.point);
        }
    }

    /// `StarSystem_ResolveCollisionSide`: the object and the sub-object of
    /// a body's part.
    fn part_sub_obj(&self, h: ObjHandle, part_id: u32) -> Option<SubObjId> {
        let o = self.objects.get(h)?;
        if o.is(class::BEAM) {
            return Some(SUBOBJ_ID_ROOT);
        }
        let instance = o.part_to_inst(part_id);
        Some(self.inst_to_sub_obj_id(h, instance))
    }

    /// `GameObject::on_collision` (and `Bullet::on_collision`): a bullet
    /// hits the other object with its munition and dies; anything else
    /// deals the collision damage of the relative speed to the other.
    fn on_collision(&mut self, h: ObjHandle, sub: SubObjId, other: ObjHandle, other_sub: SubObjId, point: Vector) {
        let (Some(o), Some(other_obj)) = (self.objects.get(h), self.objects.get(other)) else {
            return;
        };
        if o.dead {
            return;
        }
        if let KindData::Beam(b) = &o.data {
            let (munition, shooter) = (b.munition, b.projectile.owner_id);
            let target_id = other_obj.simple.id;
            self.munition_collision(other, shooter, munition, other_sub, point);
            self.destroy(h, target_id);
            return;
        }
        let other_id = other_obj.simple.id;
        if other_id == 0 || other_obj.simple.owner_player != 0 {
            return;
        }
        let own_vel = self.velocity(h);
        let other_vel = self.velocity(other);
        let rx = own_vel.x - other_vel.x;
        let ry = own_vel.y - other_vel.y;
        let rz = own_vel.z - other_vel.z;
        let own_mass = self.mass(h);
        let other_mass = self.mass(other);
        let speed = r(((e(rx) * e(rx) + e(rz) * e(rz)) + e(ry) * e(ry)).sqrt());
        let factor = self.data.constants.general.collision_damage_factor;
        let amount = calc_collision_damage_scaled(factor, speed, other_mass, own_mass);
        let id = self.objects.get(h).map_or(0, |o| o.simple.id);
        self.events.push(WorldEvent::Collision {
            a: id,
            b: other_id,
            speed,
            damage: amount,
        });
        let _ = sub;
        // `StarSystem::collision_damage`.
        let Some(target) = self.objects.get(other) else {
            return;
        };
        if target.dead {
            return;
        }
        let inflictor_owner = self.objects.get(h).map_or(0, |o| o.simple.owner_player);
        let mut list = DamageList::new(CAUSE_GUN, id, inflictor_owner);
        match target.kind {
            // `Bullet::damage_sub_obj` does nothing.
            ObjectKind::Beam => return,
            // `Guided::damage_sub_obj` (and `Mine`'s) destroys.
            ObjectKind::Guided | ObjectKind::Mine => list.destroyed = true,
            _ => self.with_damage_target(other, |t, in_group, mount_of| {
                damage_sub_obj(t, other_sub, amount, &mut list, in_group, mount_of);
            }),
        }
        self.apply_damage_list(other, &list);
    }

    /// `StarSystem::munition_collision`: the target's `munition_hit` fills a
    /// damage list (cause 1 for guns, 2 for missiles, 3 for mines), which is
    /// then applied.
    pub fn munition_collision(&mut self, target: ObjHandle, shooter: u32, munition: u32, sub: SubObjId, point: Vector) {
        let Some(t) = self.objects.get(target) else {
            return;
        };
        if t.dead {
            return;
        }
        let target_id = t.simple.id;
        let Some(arch) = self.data.archetypes.equipment.get(&munition).cloned() else {
            return;
        };
        let cause = match arch.root.object_type {
            0x200_0000 => damage::CAUSE_MISSILE,
            0x100_0000 => damage::CAUSE_MINE,
            _ => CAUSE_GUN,
        };
        let inflictor_owner = self
            .find(shooter)
            .and_then(|s| self.objects.get(s))
            .map_or(0, |s| s.simple.owner_player);
        let mut list = DamageList::new(cause, shooter, inflictor_owner);
        let m = ProjectileArch::of(&arch).unwrap_or_default();
        let _ = point;
        self.with_damage_target(target, |t, in_group, mount_of| {
            damage::munition_hit(t, &m, sub, &mut list, in_group, mount_of);
        });
        self.events.push(WorldEvent::MunitionHit {
            target: target_id,
            shooter,
            munition,
            sub,
            damage: list.clone(),
        });
        self.apply_damage_list(target, &list);
    }

    /// Builds the [`DamageTarget`] of an object and runs `f` with it.
    pub fn with_damage_target(
        &self,
        h: ObjHandle,
        f: impl FnOnce(
            &DamageTarget<'_>,
            &dyn Fn(&damage::ArchGroup, Option<neuron_core::scene::InstanceId>) -> bool,
            &dyn Fn(SubObjId) -> Option<neuron_core::scene::InstanceId>,
        ),
    ) {
        let Some(o) = self.objects.get(h) else {
            return;
        };
        let empty = crate::equip::EquipManager::default();
        let (equipment, groups, power, max_power, weapons_offline) = match o.equipped.as_deref() {
            Some(eq) => (&eq.equipment, &eq.arch_groups[..], eq.power, eq.max_power, eq.weapons_offline),
            None => (&empty, &[][..], 0.0, 0.0, false),
        };
        let mods = &self.data.weapon_mods;
        let shield_types = |shield: SubObjId| -> Vec<u32> {
            let Some(EquipKind::Shield(s)) = equipment.by_id(shield).map(|e| &e.kind) else {
                return Vec::new();
            };
            s.generators
                .iter()
                .filter_map(|g| match equipment.by_id(*g).map(|e| &e.kind) {
                    Some(EquipKind::ShieldGenerator(g)) => Some(g.shield_type),
                    _ => None,
                })
                .collect()
        };
        let modifier = |shield: SubObjId, weapon: u32| -> f64 {
            match equipment.by_id(shield).map(|e| &e.kind) {
                Some(EquipKind::Shield(s)) => s.weapon_modifier(weapon, &shield_types(shield), mods),
                _ => 1.0,
            }
        };
        let g = &self.data.constants.general;
        let t = DamageTarget {
            hit_pts: o.simple.hit_pts,
            max_hit_pts: o.simple.max_hit_pts,
            owner_player: o.simple.owner_player,
            difficulty_scale: None,
            invincible: None,
            equipment,
            groups,
            power,
            max_power,
            weapons_offline,
            single_player: self.config.single_player,
            player_attached_equip_scale: g.player_attached_equip_hit_pts_scale,
            player_group_scale: g.player_collision_group_hit_pts_scale,
            shield_hull_damage_factor: self.data.constants.shield.hull_damage_factor,
            death_fuse_burning: false,
            shield_modifier: &modifier,
        };
        let in_group = |g: &damage::ArchGroup, i: Option<neuron_core::scene::InstanceId>| g.is_inst_in_group(i);
        let mount_of = |id: SubObjId| -> Option<neuron_core::scene::InstanceId> {
            equipment.by_id(id).and_then(|e| e.mount.as_ref()).and_then(|m| m.instance)
        };
        f(&t, &in_group, &mount_of);
    }

    /// `EqObj::apply_damage_list` (and `GameObject`'s for objects without
    /// equipment): the root, power, group and equipment entries, then the
    /// object's destruction when the list says so.
    pub fn apply_damage_list(&mut self, h: ObjHandle, list: &DamageList) {
        let now = self.time;
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        if o.dead {
            return;
        }
        for entry in &list.entries {
            let id = entry.sub_obj_id;
            if id == SUBOBJ_ID_ROOT {
                o.simple.hit_pts = entry.hit_pts;
                continue;
            }
            let Some(eq) = o.equipped.as_deref_mut() else {
                continue;
            };
            if id == SUBOBJ_ID_POWER {
                eq.power = entry.hit_pts;
            } else if neuron_core::id::sub_object::is_arch_group_id(id) {
                if let Some(g) = eq.arch_groups.iter_mut().find(|g| g.id == id) {
                    g.hit_pts = entry.hit_pts;
                    if entry.fate != Fate::None || entry.hit_pts <= 0.0 {
                        g.state = GroupState::Destroyed;
                        g.fate = entry.fate;
                    }
                }
            } else if neuron_core::id::sub_object::is_equip_id(id)
                && let Some(equip) = eq.equipment.by_id_mut(id)
            {
                if equip.is_destroyed() {
                    continue;
                }
                let status = super::spawn::shield_status(equip);
                let crate::equip::Equip { kind, events, .. } = equip;
                if let EquipKind::Shield(s) = kind {
                    s.set_hit_points(entry.hit_pts, status, now, events);
                } else {
                    equip.set_hit_points(entry.hit_pts);
                }
                if entry.hit_pts <= 0.0 && entry.fate != Fate::None {
                    if let Some(a) = &mut equip.attached {
                        a.fate = entry.fate;
                    }
                    equip.destroy();
                }
            }
        }
        let weapons_offline = o.equipped.as_deref().is_some_and(|e| e.weapons_offline);
        if list.destroyed && !weapons_offline {
            self.destroy(h, list.inflictor_id);
        }
    }

    /// The equipped object's kind tests for an equipment slot list by type,
    /// for the scenario harness's fire command: every unguided, intact gun.
    #[must_use]
    pub fn unguided_guns(&self, h: ObjHandle) -> Vec<SubObjId> {
        let Some(eq) = self.objects.get(h).and_then(|o| o.equipped.as_deref()) else {
            return Vec::new();
        };
        eq.equipment
            .traverse(types::ALL, false)
            .into_iter()
            .filter_map(|s| eq.equipment.get(s))
            .filter(|e| matches!(&e.kind, EquipKind::Gun(g) if !g.is_guided()))
            .map(|e| e.id)
            .collect()
    }

    /// Whether an object is a projectile kind.
    #[must_use]
    pub fn is_projectile(&self, h: ObjHandle) -> bool {
        self.objects.get(h).is_some_and(|o| {
            matches!(o.kind, ObjectKind::Beam | ObjectKind::Guided | ObjectKind::Mine | ObjectKind::CounterMeasure)
        })
    }
}

/// `EqObj::damage_sub_obj` (`GameObject::damage_sub_obj` for the root):
/// damage on a sub-object.
pub fn damage_sub_obj(
    t: &DamageTarget<'_>,
    sub: SubObjId,
    amount: f32,
    list: &mut DamageList,
    in_group: &dyn Fn(&damage::ArchGroup, Option<neuron_core::scene::InstanceId>) -> bool,
    mount_of: &dyn Fn(SubObjId) -> Option<neuron_core::scene::InstanceId>,
) {
    if sub == SUBOBJ_ID_ROOT || sub == crate::equip::SUBOBJ_ID_NONE {
        damage::damage_hull(t, amount, list);
        return;
    }
    if neuron_core::id::sub_object::is_arch_group_id(sub) {
        damage::damage_group(t, sub, amount, list, in_group, mount_of);
        return;
    }
    if neuron_core::id::sub_object::is_equip_id(sub)
        && let Some(e) = t.equipment.by_id(sub)
    {
        if e.type_bits == types::SHIELD {
            damage::damage_shield(t, sub, amount, list);
        } else {
            damage::damage_equip(t, sub, amount, list);
        }
    }
}
