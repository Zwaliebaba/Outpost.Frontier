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
use super::{DamageState, Hit, HitRule, NoServerHooks, ServerHooks, TickList, World, WorldEvent};

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

impl World {
    /// The launch context of one launcher of an equipped object.
    pub(crate) fn launch_context(&self, h: ObjHandle, id: SubObjId) -> Option<LaunchContext> {
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
    /// missiles (`launch_guided`). Without an aim point, or for a
    /// projectile with `force_gun_ori`, a gun fires forward
    /// (`SrvGun::fire_forward`: the barrels' average position plus their
    /// average direction scaled to 1e10). Returns each gun's result.
    pub fn fire(
        &mut self,
        h: ObjHandle,
        guns: &[SubObjId],
        aim: Option<Vector>,
    ) -> Vec<(SubObjId, FireResult)> {
        self.fire_made(h, guns, aim).0
    }

    /// [`World::fire`] from the AI (`AiCommand::FireGuns`): the shot, then
    /// the server's part of it after the behaviour tick
    /// ([`super::ServerRequest::Fired`]).
    pub(crate) fn fire_for_ai(&mut self, h: ObjHandle, guns: &[SubObjId], aim: Option<Vector>) {
        let (results, guided) = self.fire_made(h, guns, aim);
        self.push_server_request(super::ServerRequest::Fired {
            ship: h,
            aim,
            results,
            guided,
        });
    }

    /// [`World::fire`] with the guided munitions it made and their guns.
    #[allow(clippy::type_complexity)]
    fn fire_made(
        &mut self,
        h: ObjHandle,
        guns: &[SubObjId],
        aim: Option<Vector>,
    ) -> (Vec<(SubObjId, FireResult)>, Vec<(SubObjId, ObjHandle)>) {
        let mut out = Vec::new();
        let mut made_guided = Vec::new();
        let Some(o) = self.objects.get(h) else {
            return (out, made_guided);
        };
        if o.dead {
            return (out, made_guided);
        }
        let system = o.physical.system;
        let owner_id = o.simple.id;
        for &id in guns {
            let Some(cx) = self.launch_context(h, id) else {
                continue;
            };
            let forward = self
                .objects
                .get(h)
                .and_then(|o| o.equipped.as_deref())
                .and_then(|eq| eq.equipment.by_id(id))
                .and_then(|e| match &e.kind {
                    EquipKind::Gun(g) => Some(g),
                    _ => None,
                })
                .map(|g| {
                    (
                        g.launcher.projectile.is_some_and(|p| p.force_gun_ori),
                        cx.forward_target(&g.launcher.barrels),
                    )
                });
            let target = match (aim, forward) {
                (Some(t), Some((false, _))) => t,
                (_, Some((_, f))) => f,
                (Some(t), None) => t,
                (None, None) => continue,
            };
            let mut shots: Vec<ProjLaunchInfo> = Vec::new();
            let mut munition: Option<ProjectileArch> = None;
            let mut result = FireResult::Destroyed;
            let mut rng = std::mem::take(&mut self.rng);
            if let Some(eq) = self
                .objects
                .get_mut(h)
                .and_then(|o| o.equipped.as_deref_mut())
            {
                let crate::object::Equipped {
                    equipment, power, ..
                } = eq;
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
                    angular_velocity: Vector::ZERO,
                };
                let made = if m.seeker != 0 {
                    self.create_guided(system, &spawn)
                } else {
                    self.create_bullet(system, &spawn)
                };
                if let Ok(b) = made {
                    if m.seeker != 0 {
                        made_guided.push((id, b));
                    }
                    let bullet = self.objects.get(b).map_or(0, |o| o.simple.id);
                    self.events.push(WorldEvent::Fired {
                        shooter: owner_id,
                        equip: id,
                        bullet,
                    });
                }
            }
        }
        if let Some(log) = self.fire_log.as_mut() {
            log.push((owner_id, out.clone()));
        }
        (out, made_guided)
    }

    /// The launcher part of an object's gun or dropper.
    fn launcher_of(&self, h: ObjHandle, id: SubObjId) -> Option<&crate::equip::launcher::Launcher> {
        let equip = self
            .objects
            .get(h)?
            .equipped
            .as_deref()?
            .equipment
            .by_id(id)?;
        match &equip.kind {
            EquipKind::Gun(g) => Some(&g.launcher),
            EquipKind::MineDropper(l) | EquipKind::CmDropper(l) => Some(l),
            _ => None,
        }
    }

    /// `CELauncher::Projectile`: what a gun, mine dropper or
    /// counter-measure dropper fires.
    #[must_use]
    pub fn launcher_projectile(&self, h: ObjHandle, id: SubObjId) -> Option<ProjectileArch> {
        self.launcher_of(h, id)?.projectile
    }

    /// `CELauncher::Fire` of one launcher (vt +0x90; `CEGun::CanFire` with
    /// its muzzle cone for a gun, `CEMineDropper::CanFire` with its cruise
    /// test for a mine dropper): the refire clock restarts unless the
    /// refire delay refused the shot. `None` for an item that is no
    /// launcher.
    pub fn launcher_fire(
        &mut self,
        h: ObjHandle,
        id: SubObjId,
        target: Vector,
    ) -> Option<FireResult> {
        let cx = self.launch_context(h, id)?;
        let equip = self
            .objects
            .get_mut(h)?
            .equipped
            .as_deref_mut()?
            .equipment
            .by_id_mut(id)?;
        let (launcher, result) = match &mut equip.kind {
            EquipKind::Gun(g) => return Some(g.fire(&cx, target)),
            EquipKind::MineDropper(l) => {
                let result = l.can_fire(&cx);
                let result = if result == FireResult::Ok && cx.cruise_active {
                    FireResult::Cruise
                } else {
                    result
                };
                (l, result)
            }
            EquipKind::CmDropper(l) => {
                let result = l.can_fire(&cx);
                (l, result)
            }
            _ => return None,
        };
        if result != FireResult::RefireDelay {
            launcher.refire_elapsed = 0.0;
        }
        Some(result)
    }

    /// `CELauncher::AmmoNeedsMet`: the ammunition for every barrel is in
    /// the hold (always for a projectile that needs none).
    #[must_use]
    pub fn launcher_ammo_needs_met(&self, h: ObjHandle, id: SubObjId) -> bool {
        match (self.launch_context(h, id), self.launcher_of(h, id)) {
            (Some(cx), Some(l)) => l.ammo_needs_met(cx.ammo),
            _ => false,
        }
    }

    /// `ConsumeFireResources` (`CELauncher`'s, and `CEGun`'s, which also
    /// restarts the lock clock): `DrawAmmoFromCargo` (the first cargo stack
    /// of the projectile loses one unit per barrel when the projectile
    /// needs ammunition), then `DrawPower` per barrel.
    pub fn launcher_consume_fire_resources(&mut self, h: ObjHandle, id: SubObjId) {
        let Some(l) = self.launcher_of(h, id) else {
            return;
        };
        let count = l.barrel_count() as u32;
        let projectile = l.projectile;
        let Some(eq) = self
            .objects
            .get_mut(h)
            .and_then(|o| o.equipped.as_deref_mut())
        else {
            return;
        };
        let crate::object::Equipped {
            equipment, power, ..
        } = eq;
        if let Some(p) = projectile
            && p.requires_ammo
        {
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
        if let Some(equip) = equipment.by_id_mut(id) {
            match &mut equip.kind {
                EquipKind::Gun(g) => {
                    g.launcher.draw_power(power);
                    g.lock_elapsed = 0.0;
                }
                EquipKind::MineDropper(l) | EquipKind::CmDropper(l) => l.draw_power(power),
                _ => {}
            }
        }
    }

    /// `CELauncher::ComputeLaunchInfo` (vt +0x98): one record per barrel, a
    /// gun's with its dispersion (`rand()` draws) and its lock.
    pub fn launcher_launch_info(
        &mut self,
        h: ObjHandle,
        id: SubObjId,
        target: Vector,
    ) -> Vec<ProjLaunchInfo> {
        let Some(cx) = self.launch_context(h, id) else {
            return Vec::new();
        };
        let mut rng = std::mem::take(&mut self.rng);
        let shots = match self
            .objects
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.by_id(id))
            .map(|e| &e.kind)
        {
            Some(EquipKind::Gun(g)) => g.launch_info(&cx, target, &mut rng),
            Some(EquipKind::MineDropper(l) | EquipKind::CmDropper(l)) => (0..l.barrel_count())
                .map(|b| l.launch_info_one_barrel(&cx, b, target))
                .collect(),
            _ => Vec::new(),
        };
        self.rng = rng;
        shots
    }

    /// `GetAvgBarrelPosWS` and `GetAvgBarrelDirWS` of a launcher.
    #[must_use]
    pub fn launcher_barrel_average(&self, h: ObjHandle, id: SubObjId) -> Option<(Vector, Vector)> {
        let cx = self.launch_context(h, id)?;
        let barrels = &self.launcher_of(h, id)?.barrels;
        Some((
            cx.barrel_average(barrels, false),
            cx.barrel_average(barrels, true),
        ))
    }

    /// `CEGun::GetTarget`: the gun's lock target (while it exists) and
    /// sub-target; `None` for an item that is no gun.
    #[must_use]
    pub fn gun_target(&self, h: ObjHandle, id: SubObjId) -> Option<(Option<ObjHandle>, SubObjId)> {
        let equip = self
            .objects
            .get(h)?
            .equipped
            .as_deref()?
            .equipment
            .by_id(id)?;
        match &equip.kind {
            EquipKind::Gun(g) => Some((g.target.filter(|t| self.objects.contains(*t)), g.lock_sub)),
            _ => None,
        }
    }

    /// Starts (or stops) keeping every [`World::fire`] call's results
    /// (diagnostic): the shooter's id and each gun's result.
    pub fn set_fire_log(&mut self, on: bool) {
        self.fire_log = on.then(Vec::new);
    }

    /// The fire results kept since the last call.
    pub fn take_fire_log(&mut self) -> Vec<super::FireCall> {
        self.fire_log
            .as_mut()
            .map(std::mem::take)
            .unwrap_or_default()
    }

    /// The objects a bullet's ray started inside since the last call
    /// (diagnostic): the ray does not hit them, as IVP's ray solver does
    /// not.
    pub fn take_beam_starts_inside(&mut self, system: u32) -> Vec<ObjHandle> {
        self.physics
            .world_mut(system)
            .map(|w| {
                w.take_beam_starts_inside()
                    .into_iter()
                    .map(ObjHandle::from_key)
                    .collect()
            })
            .unwrap_or_default()
    }

    /// The objects a bullet's ray crossed since the last call that IVP's
    /// OV sphere test kept it from hitting (diagnostic): their core at the
    /// start of the PSI was out of the test's reach.
    pub fn take_beam_ov_culled(&mut self, system: u32) -> Vec<ObjHandle> {
        self.physics
            .world_mut(system)
            .map(|w| {
                w.take_beam_ov_culled()
                    .into_iter()
                    .map(ObjHandle::from_key)
                    .collect()
            })
            .unwrap_or_default()
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
    pub(crate) fn process_collisions(&mut self, system: u32, hooks: &mut dyn ServerHooks) {
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
            let (a, b) = (
                ObjHandle::from_key(ev.object_a),
                ObjHandle::from_key(ev.object_b),
            );
            let (Some((a, sub_a)), Some((b, sub_b))) = (
                self.part_sub_obj(a, ev.part_id_a),
                self.part_sub_obj(b, ev.part_id_b),
            ) else {
                continue;
            };
            if a == b {
                continue;
            }
            self.on_collision(a, sub_a, b, sub_b, ev.point, hooks);
            self.on_collision(b, sub_b, a, sub_a, ev.point, hooks);
        }
    }

    /// `StarSystem_ResolveCollisionSide` (`Server/IServerImpl.cpp:3610`):
    /// the object and the sub-object of a body's part. An equipped object
    /// hit anywhere but on its shield gives the hit to its first shield
    /// while that shield is functioning (`:3667-3678`).
    fn part_sub_obj(&self, h: ObjHandle, part_id: u32) -> Option<(ObjHandle, SubObjId)> {
        let o = self.objects.get(h)?;
        if o.is(class::BEAM) {
            return Some((h, SUBOBJ_ID_ROOT));
        }
        // A physically attached item's body stands for its owner's item.
        let (owner, sub) = if let KindData::EquipmentObject {
            owner: Some(owner),
            equip,
        } = o.data
        {
            (owner, equip)
        } else {
            let instance = o.part_to_inst(part_id);
            (h, self.inst_to_sub_obj_id(h, instance))
        };
        if neuron_core::id::sub_object::is_shield_equip_id(sub) {
            return Some((owner, sub));
        }
        let shield = self
            .objects
            .get(owner)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.first(types::SHIELD))
            .filter(|e| match &e.kind {
                EquipKind::Shield(s) => s.is_functioning(super::spawn::shield_status(e)),
                _ => false,
            });
        Some((owner, shield.map_or(sub, |e| e.id)))
    }

    /// `GameObject::on_collision` (and `Bullet::on_collision`): a bullet
    /// hits the other object with its munition and dies; anything else
    /// deals the collision damage of the relative speed to the other.
    fn on_collision(
        &mut self,
        h: ObjHandle,
        sub: SubObjId,
        other: ObjHandle,
        other_sub: SubObjId,
        point: Vector,
        hooks: &mut dyn ServerHooks,
    ) {
        let (Some(o), Some(other_obj)) = (self.objects.get(h), self.objects.get(other)) else {
            return;
        };
        // `Guided::on_collision` runs whether or not the other side's
        // report destroyed the missile first.
        if o.kind == ObjectKind::Guided {
            self.guided_collision(h, other, other_sub, hooks);
            return;
        }
        if o.dead {
            return;
        }
        if let KindData::Beam(b) = &o.data {
            let (munition, shooter) = (b.munition, b.projectile.owner_id);
            let target_id = other_obj.simple.id;
            self.munition_collision_hooked(other, shooter, munition, other_sub, point, hooks);
            self.destroy_hooked(h, 1, target_id, hooks);
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
        let inflictor_owner = self.objects.get(h).map_or(0, |o| o.simple.owner_player);
        self.collision_damage_hooked(id, inflictor_owner, other, other_sub, amount, hooks);
    }

    /// `StarSystem::collision_damage`: unless the PvP gate holds it, a live
    /// target's `damage_sub_obj` takes `amount` on `other_sub` in a cause-1
    /// list from `source` (owned by `inflictor_owner`), which is then
    /// applied. The world's collision pass and a client's collision report
    /// (`IServerImpl::SPObjCollision`) both end here.
    pub fn collision_damage_hooked(
        &mut self,
        source: u32,
        inflictor_owner: u32,
        other: ObjHandle,
        other_sub: SubObjId,
        amount: f32,
        hooks: &mut dyn ServerHooks,
    ) {
        if hooks.pvp_blocked(self, source, other) {
            return;
        }
        let Some(target) = self.objects.get(other) else {
            return;
        };
        if target.dead {
            return;
        }
        let mut list = DamageList::new(CAUSE_GUN, source, inflictor_owner);
        match target.kind {
            // `Bullet::damage_sub_obj` does nothing.
            ObjectKind::Beam => return,
            // `Guided::damage_sub_obj` (and `Mine`'s) destroys.
            ObjectKind::Guided | ObjectKind::Mine => list.destroyed = true,
            _ => match hooks.hit_rule(self, other, Hit::SubObj) {
                HitRule::Ignore => {}
                HitRule::Destroy => list.destroyed = true,
                rule => {
                    let mut state = hooks.damage_state(self, other, inflictor_owner);
                    state.root_immune = rule == HitRule::RootImmune;
                    self.with_damage_target_state(other, state, |t, in_group, mount_of| {
                        damage_sub_obj(t, other_sub, amount, &mut list, in_group, mount_of);
                    });
                }
            },
        }
        self.apply_damage_list_hooked(other, &list, hooks);
    }

    /// `StarSystem::munition_collision`: the target's `munition_hit` fills a
    /// damage list (cause 1 for guns, 2 for missiles, 3 for mines), which is
    /// then applied.
    pub fn munition_collision(
        &mut self,
        target: ObjHandle,
        shooter: u32,
        munition: u32,
        sub: SubObjId,
        point: Vector,
    ) {
        self.munition_collision_hooked(target, shooter, munition, sub, point, &mut NoServerHooks);
    }

    /// [`World::munition_collision`] with the server's hooks.
    pub fn munition_collision_hooked(
        &mut self,
        target: ObjHandle,
        shooter: u32,
        munition: u32,
        sub: SubObjId,
        point: Vector,
        hooks: &mut dyn ServerHooks,
    ) {
        if hooks.pvp_blocked(self, shooter, target) {
            return;
        }
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
        match hooks.hit_rule(
            self,
            target,
            Hit::Munition {
                seeker: m.seeker != 0,
            },
        ) {
            HitRule::Ignore => {}
            HitRule::Destroy => list.destroyed = true,
            rule => {
                let immune = rule == HitRule::RootImmune;
                if immune && sub == SUBOBJ_ID_ROOT {
                    hooks.root_hit_ignored(self, target, &list);
                }
                let mut state = hooks.damage_state(self, target, inflictor_owner);
                state.root_immune = immune;
                self.with_damage_target_state(target, state, |t, in_group, mount_of| {
                    damage::munition_hit(t, &m, sub, &mut list, in_group, mount_of);
                });
            }
        }
        let (hull_before, shield_before) = self.objects.get(target).map_or((0.0, None), |t| {
            let shield = t.equipped.as_deref().and_then(|eq| {
                eq.equipment
                    .first(crate::equip::types::SHIELD)
                    .and_then(|e| match &e.kind {
                        crate::equip::EquipKind::Shield(s) => Some((e.id, s.hit_pts)),
                        _ => None,
                    })
            });
            (t.simple.hit_pts, shield)
        });
        self.events.push(WorldEvent::MunitionHit {
            target: target_id,
            shooter,
            munition,
            sub,
            damage: list.clone(),
            hull_before,
            shield_before,
        });
        self.apply_damage_list_hooked(target, &list, hooks);
    }

    /// Builds the [`DamageTarget`] of an object, without server state, and
    /// runs `f` with it.
    pub fn with_damage_target(
        &self,
        h: ObjHandle,
        f: impl FnOnce(
            &DamageTarget<'_>,
            &dyn Fn(&damage::ArchGroup, Option<neuron_core::scene::InstanceId>) -> bool,
            &dyn Fn(SubObjId) -> Option<neuron_core::scene::InstanceId>,
        ),
    ) {
        self.with_damage_target_state(h, DamageState::default(), f);
    }

    /// [`World::with_damage_target`] with the server's state of the object
    /// ([`ServerHooks::damage_state`]): its invincibility, the difficulty
    /// scale and whether its death fuse burns.
    pub fn with_damage_target_state(
        &self,
        h: ObjHandle,
        state: DamageState,
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
            Some(eq) => (
                &eq.equipment,
                &eq.arch_groups[..],
                eq.power,
                eq.max_power,
                eq.weapons_offline,
            ),
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
                Some(EquipKind::Shield(s)) => {
                    s.weapon_modifier(weapon, &shield_types(shield), mods)
                }
                _ => 1.0,
            }
        };
        let g = &self.data.constants.general;
        let t = DamageTarget {
            hit_pts: o.simple.hit_pts,
            max_hit_pts: o.simple.max_hit_pts,
            owner_player: o.simple.owner_player,
            difficulty_scale: state.difficulty_scale,
            invincible: state.invincible,
            equipment,
            groups,
            power,
            max_power,
            weapons_offline,
            single_player: self.config.single_player,
            player_attached_equip_scale: g.player_attached_equip_hit_pts_scale,
            player_group_scale: g.player_collision_group_hit_pts_scale,
            shield_hull_damage_factor: self.data.constants.shield.hull_damage_factor,
            death_fuse_burning: state.death_fuse_burning,
            root_immune: state.root_immune,
            explosion_resistance: self
                .data
                .archetypes
                .ships
                .get(&o.archetype)
                .map(|a| a.eq_obj.root.explosion_resistance)
                .or_else(|| {
                    self.data
                        .archetypes
                        .solars
                        .get(&o.archetype)
                        .map(|a| a.eq_obj.root.explosion_resistance)
                })
                .unwrap_or(1.0),
            shield_modifier: &modifier,
        };
        let in_group = |g: &damage::ArchGroup, i: Option<neuron_core::scene::InstanceId>| {
            g.is_inst_in_group(i)
        };
        let mount_of = |id: SubObjId| -> Option<neuron_core::scene::InstanceId> {
            equipment
                .by_id(id)
                .and_then(|e| e.mount.as_ref())
                .and_then(|m| m.instance)
        };
        f(&t, &in_group, &mount_of);
    }

    /// `EqObj::apply_damage_list` (and `GameObject`'s for objects without
    /// equipment): the root, power, group and equipment entries, then the
    /// object's destruction when the list says so.
    pub fn apply_damage_list(&mut self, h: ObjHandle, list: &DamageList) {
        self.apply_damage_list_hooked(h, list, &mut NoServerHooks);
    }

    /// [`World::apply_damage_list`] with the server's hooks: the server
    /// sees the list first and may keep it (`is_damage_blocked`, the
    /// damage-callback latch), then the entries are written, then the
    /// server's part runs (its dispatches), then a destroyed object is
    /// destroyed (`destroy(1, inflictor)`).
    pub fn apply_damage_list_hooked(
        &mut self,
        h: ObjHandle,
        list: &DamageList,
        hooks: &mut dyn ServerHooks,
    ) {
        if self.objects.get(h).is_none_or(|o| o.dead) {
            return;
        }
        let mut owned = list.clone();
        if !hooks.before_damage_list(self, h, &mut owned) {
            return;
        }
        let list = &owned.clone();
        let now = self.time;
        let stack_rule = hooks.cargo_stack_rule(self, h);
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
                let max = equip.arch.hit_pts;
                let crate::equip::Equip { kind, events, .. } = equip;
                if let EquipKind::Shield(s) = kind {
                    s.set_hit_points(entry.hit_pts, status, now, events);
                } else if let EquipKind::Cargo(c) = kind
                    && stack_rule
                    && c.count > 1
                {
                    // `EqObj::apply_equip_entry`: a spent unit of a stack
                    // leaves it.
                    if entry.hit_pts == 0.0 {
                        c.remove_from_stack(1, max);
                    } else {
                        c.hit_pts = entry.hit_pts;
                    }
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
        self.sync_shield_collisions(h);
        hooks.after_damage_list(self, h, &mut owned);
        if owned.destroyed && !weapons_offline {
            self.kill(h, owned.inflictor_id, hooks);
        }
        hooks.damage_list_done(self, h);
    }

    /// The sphere of an object's body (centre of mass and the core's
    /// radius), or its position and centred radius without one.
    pub(crate) fn body_sphere(&self, h: ObjHandle) -> (f32, Vector) {
        let Some(o) = self.objects.get(h) else {
            return (0.0, Vector::ZERO);
        };
        if let (Some(body), Some(w)) = (o.physical.body, self.physics.world(o.physical.system))
            && let (Some(core), Some(center)) = (w.core(body), w.center_of_mass(body))
        {
            return (core.upper_limit_radius, center);
        }
        (o.engine.radius, o.engine.position)
    }

    /// The sphere of an instance subtree (`GetInstanceExtent`), in world
    /// space.
    pub(crate) fn instance_sphere(
        &mut self,
        instance: Option<neuron_core::scene::InstanceId>,
    ) -> Option<(f32, Vector)> {
        let instance = instance?;
        let (radius, center) = self.scene.instance_extent(Some(instance), 0);
        if radius < 0.0 {
            return None;
        }
        let t = self.scene.instance_transform(Some(instance));
        Some((radius, neuron_core::math::transform_xform(&t, center)))
    }

    /// `StarSystem::explode`'s damage part: every equipped object whose
    /// body sphere meets the blast takes `EqObj::explosion_hit`, the
    /// spheres taken from its body and instance tree (divergence
    /// register), and the list is applied.
    pub fn explode(
        &mut self,
        system: u32,
        blast: &damage::Blast,
        inflictor: u32,
        skip: Option<ObjHandle>,
    ) {
        self.explode_caused(system, blast, damage::CAUSE_EXPLOSION, inflictor, skip, 0.0);
    }

    /// [`World::explode`] with the damage lists' cause, then the impulse
    /// pass: every object not owned by a player whose body sphere meets
    /// the blast is pushed away from it at the blast's centre
    /// (`PhySys::AddImpulseAtPoint`) by `impulse` (none when it is 0).
    pub fn explode_caused(
        &mut self,
        system: u32,
        blast: &damage::Blast,
        cause: damage::DamageCause,
        inflictor: u32,
        skip: Option<ObjHandle>,
        impulse: f32,
    ) {
        self.explode_caused_hooked(
            system,
            blast,
            cause,
            inflictor,
            skip,
            impulse,
            &mut NoServerHooks,
        );
    }

    /// [`World::explode_caused`] with the server's hooks.
    #[allow(clippy::too_many_arguments)]
    pub fn explode_caused_hooked(
        &mut self,
        system: u32,
        blast: &damage::Blast,
        cause: damage::DamageCause,
        inflictor: u32,
        skip: Option<ObjHandle>,
        impulse: f32,
        hooks: &mut dyn ServerHooks,
    ) {
        self.explode_passes(
            system,
            blast,
            cause,
            inflictor,
            (skip, skip),
            impulse,
            hooks,
        );
    }

    /// `StarSystem::explode` for a blast an object sets off on itself (a
    /// server fuse's `impulse`): inflicted by the object, whose damage pass
    /// skips it (`ExplodingObjId`) and whose impulse pass pushes it too.
    #[allow(clippy::too_many_arguments)]
    pub fn explode_self_hooked(
        &mut self,
        system: u32,
        blast: &damage::Blast,
        cause: damage::DamageCause,
        exploding: ObjHandle,
        impulse: f32,
        hooks: &mut dyn ServerHooks,
    ) {
        let inflictor = self.objects.get(exploding).map_or(0, |o| o.simple.id);
        self.explode_passes(
            system,
            blast,
            cause,
            inflictor,
            (Some(exploding), None),
            impulse,
            hooks,
        );
    }

    /// The damage pass skipping `skip.0`, then the impulse pass skipping
    /// `skip.1`.
    #[allow(clippy::too_many_arguments)]
    fn explode_passes(
        &mut self,
        system: u32,
        blast: &damage::Blast,
        cause: damage::DamageCause,
        inflictor: u32,
        skip: (Option<ObjHandle>, Option<ObjHandle>),
        impulse: f32,
        hooks: &mut dyn ServerHooks,
    ) {
        self.explode_damage(system, blast, cause, inflictor, skip.0, hooks);
        if impulse == 0.0 {
            return;
        }
        let skip = skip.1;
        let pushed: Vec<ObjHandle> = self
            .objects_in(system)
            .into_iter()
            .filter(|h| Some(*h) != skip)
            .filter(|h| {
                self.objects
                    .get(*h)
                    .is_some_and(|o| o.simple.owner_player == 0 && o.physical.body.is_some())
            })
            .collect();
        for h in pushed {
            let (radius, center) = self.body_sphere(h);
            let reach = f64::from(blast.radius) + f64::from(radius);
            let d = [
                f64::from(center.x) - f64::from(blast.position.x),
                f64::from(center.y) - f64::from(blast.position.y),
                f64::from(center.z) - f64::from(blast.position.z),
            ];
            if d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > reach * reach {
                continue;
            }
            // Float arithmetic, as the reference stores each step.
            let dir = Vector::new(r(d[0]), r(d[1]), r(d[2]));
            let length_sq = r((e(dir.y) * e(dir.y) + e(dir.z) * e(dir.z)) + e(dir.x) * e(dir.x));
            if length_sq == 0.0 {
                // `random_on_unit_sphere` is not drawn (D176).
                continue;
            }
            let inverse = r(1.0 / e(length_sq).sqrt());
            let push = Vector::new(
                r(e(r(e(dir.x) * e(inverse))) * e(impulse)),
                r(e(r(e(dir.y) * e(inverse))) * e(impulse)),
                r(e(r(e(dir.z) * e(inverse))) * e(impulse)),
            );
            if let Some((system, body)) = self.body(h)
                && let Some(world) = self.physics.world_mut(system)
            {
                world.add_impulse_at_point(body, push, blast.position);
            }
        }
    }

    fn explode_damage(
        &mut self,
        system: u32,
        blast: &damage::Blast,
        cause: damage::DamageCause,
        inflictor: u32,
        skip: Option<ObjHandle>,
        hooks: &mut dyn ServerHooks,
    ) {
        let candidates: Vec<ObjHandle> = self
            .objects_in(system)
            .into_iter()
            .filter(|h| Some(*h) != skip)
            .filter(|h| {
                self.objects
                    .get(*h)
                    .is_some_and(|o| !o.dead && o.equipped.is_some())
            })
            .collect();
        let inflictor_owner = self
            .find(inflictor)
            .and_then(|i| self.objects.get(i))
            .map_or(0, |o| o.simple.owner_player);
        for h in candidates {
            if hooks.pvp_blocked(self, inflictor, h) {
                continue;
            }
            let hull = self.body_sphere(h);
            let reach = f64::from(blast.radius) + f64::from(hull.0);
            let d = [
                f64::from(blast.position.x) - f64::from(hull.1.x),
                f64::from(blast.position.y) - f64::from(hull.1.y),
                f64::from(blast.position.z) - f64::from(hull.1.z),
            ];
            if d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > reach * reach {
                continue;
            }
            let (mounts, groups): (Vec<_>, Vec<_>) =
                match self.objects.get(h).and_then(|o| o.equipped.as_deref()) {
                    Some(eq) => (
                        eq.equipment
                            .iter()
                            .map(|e| {
                                (
                                    e.id,
                                    e.attached
                                        .as_ref()
                                        .and_then(|a| a.attachment)
                                        .and_then(|a| a.root_instance()),
                                )
                            })
                            .collect(),
                        eq.arch_groups
                            .iter()
                            .map(|g| (g.id, g.root_instance))
                            .collect(),
                    ),
                    None => (Vec::new(), Vec::new()),
                };
            // `get_explosion_dmg_bounding_sphere` for the hull's passes.
            let explosion = self.explosion_dmg_bounding_sphere(h).filter(|s| s.0 >= 0.0);
            let mut spheres = damage::BlastSpheres {
                hull: explosion.unwrap_or(hull),
                physical: hull,
                ..damage::BlastSpheres::default()
            };
            for (id, instance) in mounts {
                if let Some((r, c)) = self.instance_sphere(instance) {
                    spheres.equipment.push((id, r, c));
                }
            }
            for (id, instance) in groups {
                if let Some((r, c)) = self.instance_sphere(instance) {
                    spheres.groups.push((id, r, c));
                }
            }
            let mut list = DamageList::new(cause, inflictor, inflictor_owner);
            let rule = hooks.hit_rule(self, h, Hit::Explosion);
            if rule == HitRule::Ignore {
                continue;
            }
            let mut state = hooks.damage_state(self, h, inflictor_owner);
            state.root_immune = rule == HitRule::RootImmune;
            let mut hull_reached = false;
            self.with_damage_target_state(h, state, |t, in_group, mount_of| {
                hull_reached =
                    damage::explosion_hit(t, blast, &spheres, &mut list, in_group, mount_of);
            });
            if hull_reached && state.root_immune {
                hooks.root_hit_ignored(self, h, &list);
            }
            if rule == HitRule::Destroy {
                list.destroyed = true;
            }
            self.apply_damage_list_hooked(h, &list, hooks);
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
            matches!(
                o.kind,
                ObjectKind::Beam
                    | ObjectKind::Guided
                    | ObjectKind::Mine
                    | ObjectKind::CounterMeasure
            )
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
        // `Solar::damage_sub_obj` ignores a root hit on an indestructible
        // solar.
        if !(t.root_immune && sub == SUBOBJ_ID_ROOT) {
            damage::damage_hull(t, amount, list);
        }
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
