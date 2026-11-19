//! Turret guns in the world (`CEGun::Connect`'s joint walk,
//! `ComputeTurretFrame`, `GetTurretOrigin`, `Update`'s aim and lock clock,
//! `SetTarget`, `SetTargetPoint`, `CanPointAt`, `CanSeeTargetObject`,
//! `ComputeTgtLeadPosition`) and the per-frame joint list of `AnimDB`
//! (`Common/common_animdb.cpp`): a joint whose aim moved registers itself
//! during the object updates and turns once in the next
//! [`World::update_anim`].

use neuron_core::id::sub_object;
use neuron_core::math::{Matrix, Vector, transform_matrix};
use neuron_core::scene::{InstanceId, JointPayload};

use crate::equip::launcher::Gun;
use crate::equip::turret::{
    self, JointChannel, SEE_TARGET_ANGLE, SEEKER_LEAD_SPEED, TARGET_POINT_VALID_TIME,
};
use crate::equip::{EquipKind, SUBOBJ_ID_NONE, SubObjId};
use crate::object::ObjHandle;

use super::World;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// One turret joint registered with `AnimDB::Add(VChannel*)` for the next
/// animation update.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AnimChannel {
    owner: ObjHandle,
    gun: SubObjId,
    pitch: bool,
}

/// What the turret functions read about a gun's mount.
struct Mount {
    root: Option<InstanceId>,
    connector: Option<InstanceId>,
}

impl World {
    fn joint_limits(&self, joint: &JointChannel) -> Option<(f32, f32)> {
        let instance = joint.instance?;
        let rel = self.scene.instance_relative(Some(instance))?;
        Some((rel.min(), rel.max()))
    }

    fn relative_or_default(&self, instance: Option<InstanceId>) -> JointPayload {
        self.scene
            .instance_relative(instance)
            .copied()
            .unwrap_or_default()
    }

    /// `CEGun::Connect`'s joint walk (`BindTurretJoints`) and
    /// `ComputeTurretFrame`: a revolute joint on the model's root binds the
    /// yaw, the last revolute child of the root binds the pitch.
    pub(crate) fn locate_joints(
        &self,
        gun: &mut Gun,
        root: Option<InstanceId>,
        connector: Option<InstanceId>,
    ) {
        let bind = |w: &Self, joint: &mut JointChannel, instance: InstanceId| {
            let limits = w
                .scene
                .instance_relative(Some(instance))
                .map(|r| (r.min(), r.max()));
            let angle = w
                .scene
                .joint_params(Some(instance), 0)
                .and_then(|v| v.first().copied());
            joint.bind(instance, limits, angle);
        };
        if let Some(root) = root {
            if self
                .scene
                .instance_relative(Some(root))
                .map(JointPayload::code)
                == Some(2)
            {
                bind(self, &mut gun.yaw, root);
            }
            let mut cursor = None;
            while let Some(child) = self.scene.related_instance(Some(root), 1, cursor) {
                if self
                    .scene
                    .instance_relative(Some(child))
                    .map(JointPayload::code)
                    == Some(2)
                {
                    bind(self, &mut gun.pitch, child);
                }
                cursor = Some(child);
            }
        }
        gun.turret_frame = self.turret_frame(gun, &Mount { root, connector });
    }

    fn turret_frame(&self, gun: &Gun, mount: &Mount) -> Matrix {
        let connector = self.scene.instance_orientation(mount.connector);
        let root_rel = self.relative_or_default(mount.root);
        let yaw_local = if gun.yaw.is_bound() {
            root_rel.axis()
        } else {
            transform_matrix(&root_rel.rotation(), Vector::new(0.0, 1.0, 0.0))
        };
        let pitch_world = match gun.pitch.instance {
            Some(p) => {
                let parent = self.scene.instance_parent(Some(p));
                let parent_ori = self.scene.instance_orientation(parent);
                transform_matrix(&parent_ori, self.relative_or_default(Some(p)).axis())
            }
            None => transform_matrix(
                &self.scene.instance_orientation(mount.root),
                Vector::new(1.0, 0.0, 0.0),
            ),
        };
        turret::compute_turret_frame(&connector, yaw_local, pitch_world)
    }

    fn gun_mount(&self, h: ObjHandle, id: SubObjId) -> Option<Mount> {
        let equip = self
            .objects
            .get(h)?
            .equipped
            .as_deref()?
            .equipment
            .by_id(id)?;
        let root = if equip.is_connected() {
            equip
                .attached
                .as_ref()
                .and_then(|a| a.attachment)
                .and_then(|a| a.root_instance())
        } else {
            None
        };
        let connector = equip
            .mount
            .as_ref()
            .filter(|m| m.connected)
            .and_then(|m| m.instance);
        Some(Mount { root, connector })
    }

    fn gun(&self, h: ObjHandle, id: SubObjId) -> Option<&Gun> {
        match &self
            .objects
            .get(h)?
            .equipped
            .as_deref()?
            .equipment
            .by_id(id)?
            .kind
        {
            EquipKind::Gun(g) => Some(g),
            _ => None,
        }
    }

    fn gun_mut(&mut self, h: ObjHandle, id: SubObjId) -> Option<&mut Gun> {
        match &mut self
            .objects
            .get_mut(h)?
            .equipped
            .as_deref_mut()?
            .equipment
            .by_id_mut(id)?
            .kind
        {
            EquipKind::Gun(g) => Some(g),
            _ => None,
        }
    }

    /// `CEGun::GetTurretOrigin`: the pitch joint's origin, else the yaw
    /// joint's position, else the model root's.
    #[must_use]
    pub fn turret_origin(&self, h: ObjHandle, id: SubObjId) -> Option<Vector> {
        let gun = self.gun(h, id)?;
        let mount = self.gun_mount(h, id)?;
        Some(self.turret_origin_of(gun, &mount))
    }

    fn turret_origin_of(&self, gun: &Gun, mount: &Mount) -> Vector {
        if let Some(p) = gun.pitch.instance {
            let rel = self.relative_or_default(Some(p));
            return turret::pitch_turret_origin(
                self.scene.instance_position(Some(p)),
                &self.scene.instance_orientation(Some(p)),
                rel.offset(),
            );
        }
        if let Some(y) = gun.yaw.instance {
            return self.scene.instance_position(Some(y));
        }
        self.scene.instance_position(mount.root)
    }

    /// `CObject::get_center_of_mass`: with a body `PhySys::GetCenterOfMass`
    /// (the surface's mass centre through the body's object frame now,
    /// with the reference's roundings; the core of a multiplayer body is
    /// pinned to the object's origin, so this is not the core's
    /// position), else the position.
    #[must_use]
    pub fn center_of_mass(&self, h: ObjHandle) -> Vector {
        let Some(o) = self.objects.get(h) else {
            return Vector::ZERO;
        };
        if let (Some(body), Some(w)) = (o.physical.body, self.physics.world(o.physical.system))
            && let Some((m, t)) = w.object_matrix(body)
        {
            return physics_center_of_mass(&m, t, o.physical.mass_center);
        }
        o.engine.position
    }

    /// `CEqObj::get_sub_obj_center_of_mass`: an attached item's or a
    /// group's root instance sphere centre (`GetInstanceExtent`); `None`
    /// for anything else.
    pub fn sub_obj_center_of_mass(&mut self, h: ObjHandle, sub: SubObjId) -> Option<Vector> {
        let eq = self.objects.get(h)?.equipped.as_deref()?;
        let instance = if sub_object::is_equip_id(sub) {
            let equip = eq.equipment.by_id(sub)?;
            equip.mount.as_ref()?;
            if !equip.is_connected() {
                return None;
            }
            equip.attached.as_ref()?.attachment?.root_instance()
        } else if sub_object::is_arch_group_id(sub) {
            eq.arch_groups.iter().find(|g| g.id == sub)?.root_instance
        } else {
            return None;
        };
        self.instance_sphere(instance).map(|(_, c)| c)
    }

    /// `CShip::get_gun_stats`: the cached statistics of the ship's working
    /// guns (an AI ship's auto turrets left out), rebuilt while the cache is
    /// dirty (from the ship's construction and each change of a gun's
    /// activation, `CEGun::Activate`): the guns without a seeker count as
    /// active guns (their farthest range, their mean muzzle velocity), the
    /// others as countermeasure droppers (their farthest range).
    pub fn ship_gun_stats(&mut self, h: ObjHandle) -> Option<crate::object::ship::GunStats> {
        let o = self.objects.get(h)?;
        let ship = o.as_ship()?;
        if !ship.gun_stats_dirty {
            return Some(ship.gun_stats);
        }
        let is_player = ship.is_player;
        let mut stats = crate::object::ship::GunStats::default();
        if let Some(eq) = o.equipped.as_deref() {
            for slot in eq.equipment.traverse(crate::equip::types::GUN, false) {
                let Some(equip) = eq.equipment.get(slot) else {
                    continue;
                };
                let EquipKind::Gun(g) = &equip.kind else {
                    continue;
                };
                if !equip.is_functioning(eq.weapons_offline) || !(is_player || !g.auto_turret) {
                    continue;
                }
                let ammo = g.launcher.projectile.unwrap_or_default();
                let motor = self.data.archetypes.motors.get(&ammo.motor);
                let range = g.munition_range(false, motor);
                if ammo.seeker == 0 {
                    stats.active_guns += 1;
                    if stats.max_munition_range < range {
                        stats.max_munition_range = range;
                    }
                    stats.avg_muzzle_velocity =
                        r(e(stats.avg_muzzle_velocity) + e(g.launcher.muzzle_velocity));
                } else {
                    stats.cm_droppers += 1;
                    if stats.cm_max_munition_range < range {
                        stats.cm_max_munition_range = range;
                    }
                }
            }
        }
        stats.avg_muzzle_velocity = if stats.active_guns != 0 {
            r(e(stats.avg_muzzle_velocity) / e(stats.active_guns as f32))
        } else {
            0.0
        };
        let ship = self.objects.get_mut(h)?.as_ship_mut()?;
        ship.gun_stats_dirty = false;
        ship.gun_stats = stats;
        Some(stats)
    }

    /// `CShip::get_tgt_lead_fire_pos(pos)`: where the ship's guns lead its
    /// target (the sub-target's centre of mass, else the target's), at
    /// their mean muzzle velocity, else the target's position; `Some` when
    /// that point lies within the guns' farthest range (the droppers'
    /// without guns), `None` without a target or out of range.
    pub fn ship_lead_fire_position(&mut self, h: ObjHandle) -> Option<Vector> {
        let ship = self.objects.get(h)?.as_ship()?;
        let target = ship.target.filter(|t| self.objects.contains(*t))?;
        let sub = ship.sub_target;
        let stats = self.ship_gun_stats(h)?;
        let own = self.objects.get(h)?.engine.position;
        let mut lead = None;
        let mut range_sq = 0.0_f32;
        if stats.active_guns > 0 {
            let target_vel = self.velocity(target);
            let target_pos = match self.sub_obj_center_of_mass(target, sub) {
                Some(p) => p,
                None => self.center_of_mass(target),
            };
            let rel_pos = Vector::new(
                r(e(target_pos.x) - e(own.x)),
                r(e(target_pos.y) - e(own.y)),
                r(e(target_pos.z) - e(own.z)),
            );
            let ship_vel = self.velocity(h);
            let rel_vel = Vector::new(
                r(e(target_vel.x) - e(ship_vel.x)),
                r(e(target_vel.y) - e(ship_vel.y)),
                r(e(target_vel.z) - e(ship_vel.z)),
            );
            if let Some(t) = crate::equip::launcher::compute_time_to_target(
                rel_pos,
                rel_vel,
                stats.avg_muzzle_velocity,
            ) {
                let lead_z = r(e(rel_vel.z) * e(t));
                lead = Some(Vector::new(
                    r(e(rel_vel.x) * e(t) + e(target_pos.x)),
                    r(e(rel_vel.y) * e(t) + e(target_pos.y)),
                    r(e(lead_z) + e(target_pos.z)),
                ));
            }
            range_sq = r(e(stats.max_munition_range) * e(stats.max_munition_range));
        } else if stats.cm_droppers > 0 {
            range_sq = r(e(stats.cm_max_munition_range) * e(stats.cm_max_munition_range));
        }
        let out = lead.unwrap_or_else(|| {
            self.objects
                .get(target)
                .map_or(Vector::ZERO, |o| o.engine.position)
        });
        let dx = e(out.x) - e(own.x);
        let dy = e(out.y) - e(own.y);
        let dz = e(out.z) - e(own.z);
        let dist_sq = dz * dz + dy * dy + dx * dx;
        (dist_sq < e(range_sq)).then_some(out)
    }

    /// `CEGun::ComputeTgtLeadPosition`: where the gun leads its locked
    /// target, `None` without a live target or for a destroyed gun.
    pub fn gun_lead_position(&mut self, h: ObjHandle, id: SubObjId) -> Option<Vector> {
        let equip = self
            .objects
            .get(h)?
            .equipped
            .as_deref()?
            .equipment
            .by_id(id)?;
        let EquipKind::Gun(gun) = &equip.kind else {
            return None;
        };
        let target = gun.target.filter(|t| self.objects.contains(*t))?;
        if equip.is_destroyed() {
            return None;
        }
        let lock_sub = gun.lock_sub;
        let barrel = gun.launcher.barrels.first().and_then(|b| b.instance);
        let speed = if gun.launcher.projectile.is_some_and(|p| p.seeker != 0) {
            SEEKER_LEAD_SPEED
        } else {
            gun.launcher.muzzle_velocity
        };
        let equipped_target = self.objects.get(target)?.equipped.is_some();
        let pos = if lock_sub != SUBOBJ_ID_NONE && equipped_target {
            match self.sub_obj_center_of_mass(target, lock_sub) {
                Some(p) => p,
                None => self.center_of_mass(target),
            }
        } else {
            self.center_of_mass(target)
        };
        let vel = self.velocity(target);
        let shooter = barrel.and_then(|i| self.object_of_instance(i)).unwrap_or(h);
        let shooter_pos = self
            .objects
            .get(shooter)
            .map_or(Vector::ZERO, |o| o.engine.position);
        Some(turret::lead_position(
            pos,
            vel,
            shooter_pos,
            self.velocity(shooter),
            speed,
        ))
    }

    /// `ComputeAimAngles`: towards the lead position, or along the stored
    /// target point.
    fn aim_angles(&mut self, h: ObjHandle, id: SubObjId, use_lead: bool) -> Option<(f32, f32)> {
        let lead = if use_lead {
            Some(self.gun_lead_position(h, id).unwrap_or(Vector::ZERO))
        } else {
            None
        };
        let gun = self.gun(h, id)?;
        let mount = self.gun_mount(h, id)?;
        let dir = match lead {
            Some(lead) => {
                let origin = self.turret_origin_of(gun, &mount);
                Vector::new(
                    r(e(lead.x) - e(origin.x)),
                    r(e(lead.y) - e(origin.y)),
                    r(e(lead.z) - e(origin.z)),
                )
            }
            None => gun.target_point,
        };
        let connector = self.scene.instance_orientation(mount.connector);
        Some(turret::aim_angles(&connector, &gun.turret_frame, dir))
    }

    /// Sets both joints' aims, registering a joint for the next animation
    /// update when its aim moved (`AnimDB::Add`).
    fn set_aim(&mut self, h: ObjHandle, id: SubObjId, yaw: f32, pitch: f32) {
        let Some(gun) = self.gun_mut(h, id) else {
            return;
        };
        let add_yaw = gun.yaw.needs_tick(yaw);
        gun.yaw.desired = yaw;
        let add_pitch = gun.pitch.needs_tick(pitch);
        gun.pitch.desired = pitch;
        for (add, is_pitch) in [(add_yaw, false), (add_pitch, true)] {
            let channel = AnimChannel {
                owner: h,
                gun: id,
                pitch: is_pitch,
            };
            if add && !self.anim_channels.contains(&channel) {
                self.anim_channels.push(channel);
            }
        }
    }

    /// `CEGun::SetTarget`: locks a target (and sub-object); a turret whose
    /// target point is stale turns to lead it. False when nothing changed.
    pub fn gun_set_target(
        &mut self,
        h: ObjHandle,
        id: SubObjId,
        target: Option<ObjHandle>,
        sub: SubObjId,
    ) -> bool {
        let Some(gun) = self.gun_mut(h, id) else {
            return false;
        };
        if !gun.set_lock(target, sub) {
            return false;
        }
        let live = target.is_some_and(|t| self.objects.contains(t));
        let aim = self.gun(h, id).is_some_and(|g| {
            g.is_movable() && live && TARGET_POINT_VALID_TIME < g.target_point_age
        });
        if aim && let Some((yaw, pitch)) = self.aim_angles(h, id, true) {
            self.set_aim(h, id, yaw, pitch);
        }
        true
    }

    /// `CEGun::SetTargetPoint`: the turret aims at a world point (kept
    /// relative to the turret origin) for the next two seconds.
    pub fn gun_set_target_point(&mut self, h: ObjHandle, id: SubObjId, point: Vector) {
        let Some(origin) = self.turret_origin(h, id) else {
            return;
        };
        if let Some(gun) = self.gun_mut(h, id) {
            gun.target_point = Vector::new(
                r(e(point.x) - e(origin.x)),
                r(e(point.y) - e(origin.y)),
                r(e(point.z) - e(origin.z)),
            );
            gun.target_point_age = 0.0;
        }
    }

    /// `CEGun::CanPointAt`: whether the turret's joints can bring it to
    /// within `angle` of the point. False for a destroyed gun.
    #[must_use]
    pub fn gun_can_point_at(&self, h: ObjHandle, id: SubObjId, point: Vector, angle: f32) -> bool {
        let Some(equip) = self
            .objects
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.by_id(id))
        else {
            return false;
        };
        let (EquipKind::Gun(gun), Some(mount)) = (&equip.kind, self.gun_mount(h, id)) else {
            return false;
        };
        if equip.is_destroyed() {
            return false;
        }
        let origin = self.turret_origin_of(gun, &mount);
        let connector = self.scene.instance_orientation(mount.connector);
        turret::can_point_at(
            &connector,
            &gun.turret_frame,
            origin,
            point,
            angle,
            self.joint_limits(&gun.yaw),
            self.joint_limits(&gun.pitch),
        )
    }

    /// `CEGun::CanSeeTargetPoint`: the point lies within `angle` of the
    /// first barrel.
    #[must_use]
    pub fn gun_can_see_point(&self, h: ObjHandle, id: SubObjId, point: Vector, angle: f32) -> bool {
        let (Some(cx), Some(gun)) = (self.launch_context(h, id), self.gun(h, id)) else {
            return false;
        };
        let barrels = &gun.launcher.barrels;
        turret::can_see_target_point(
            cx.barrel_pos(barrels, 0),
            cx.barrel_dir(barrels, 0),
            point,
            angle,
        )
    }

    /// `CEGun::CanSeeTargetObject`: a turret with a stale target point
    /// answers from its joints; otherwise the lead position must lie in the
    /// first barrel's cone.
    pub fn gun_can_see_target(&mut self, h: ObjHandle, id: SubObjId, angle: f32) -> bool {
        let Some(gun) = self.gun(h, id) else {
            return false;
        };
        if !gun.target.is_some_and(|t| self.objects.contains(t)) {
            return false;
        }
        if gun.is_movable() && TARGET_POINT_VALID_TIME < gun.target_point_age {
            return gun.yaw.within(angle) && gun.pitch.within(angle);
        }
        match self.gun_lead_position(h, id) {
            Some(lead) => self.gun_can_see_point(h, id, lead, angle),
            None => false,
        }
    }

    /// `CEGun::Update` past the launcher: the turret aims at the target
    /// point while it is fresh, else leads the locked target; then the
    /// lock clock grows while the target is in view.
    pub(crate) fn update_gun_turret(&mut self, h: ObjHandle, id: SubObjId, dt: f32) {
        let Some(gun) = self.gun_mut(h, id) else {
            return;
        };
        if gun.is_movable() {
            let new_age = e(dt) + e(gun.target_point_age);
            gun.target_point_age = r(new_age);
            let live = gun.target.is_some_and(|t| self.objects.contains(t));
            let angles = if new_age < e(TARGET_POINT_VALID_TIME) {
                self.aim_angles(h, id, false)
            } else if live {
                self.aim_angles(h, id, true)
            } else {
                None
            };
            let (yaw, pitch) = angles.unwrap_or((-0.0, 0.0));
            self.set_aim(h, id, yaw, pitch);
        }
        let Some(gun) = self.gun(h, id) else {
            return;
        };
        if !gun.target.is_some_and(|t| self.objects.contains(t)) {
            return;
        }
        if !gun
            .launcher
            .projectile
            .is_some_and(|p| p.time_to_lock > 0.0)
        {
            return;
        }
        let joints = if !gun.is_movable() || !(TARGET_POINT_VALID_TIME < gun.target_point_age) {
            None
        } else {
            Some(gun.yaw.within(SEE_TARGET_ANGLE) && gun.pitch.within(SEE_TARGET_ANGLE))
        };
        let increment = match joints {
            Some(within) => within,
            None => self
                .gun_lead_position(h, id)
                .is_some_and(|lead| self.gun_can_see_point(h, id, lead, SEE_TARGET_ANGLE)),
        };
        if let Some(gun) = self.gun_mut(h, id) {
            gun.lock_elapsed = if increment {
                r(e(dt) + e(gun.lock_elapsed))
            } else {
                0.0
            };
        }
    }

    /// `AnimDB::Update`: the registered bay and gate playbacks run for
    /// `dt`; every joint registered since the last call turns once (`JointChannel::Tick`: towards its aim at the gun's
    /// turn rate, through the engine's joint limits), the owners' instance
    /// trees are recomposed, the joint list empties and the finished
    /// playbacks leave theirs. The multiplayer server
    /// calls it at the start of each frame ([`World::step`] does so outside
    /// single player); a single-player client calls it from its own frame.
    pub fn update_anim(&mut self, dt: f32) {
        self.advance_playbacks(dt);
        // Phase A: the ambient playbacks' roots (`MixerWalk`).
        let mut roots: Vec<InstanceId> = self.walk_ambient(dt);
        let channels = std::mem::take(&mut self.anim_channels);
        for c in channels {
            let Some(gun) = self.gun(c.owner, c.gun) else {
                continue;
            };
            let joint = if c.pitch { gun.pitch } else { gun.yaw };
            let Some(instance) = joint.instance else {
                continue;
            };
            let mut next = joint.tick_target(gun.turn_rate, dt);
            self.scene.set_joint_params(Some(instance), 0, &[next]);
            if let Some(v) = self.scene.joint_params(Some(instance), 0)
                && let Some(&read) = v.first()
            {
                next = read;
            }
            if let Some(gun) = self.gun_mut(c.owner, c.gun) {
                let joint = if c.pitch {
                    &mut gun.pitch
                } else {
                    &mut gun.yaw
                };
                joint.settle(next);
            }
            if let Some(root) = self.objects.get(c.owner).and_then(|o| o.engine.instance)
                && !roots.contains(&root)
            {
                roots.push(root);
            }
        }
        for root in roots {
            self.scene.update_instance(Some(root), 0);
        }
        // Phases D and E: the golems of the free parts the ambient
        // animations move.
        self.push_ambient_targets();
        self.filter_playbacks();
    }

    /// The turret joints waiting for the next [`World::update_anim`], as
    /// (owner, gun, is the pitch joint).
    #[must_use]
    pub fn pending_joints(&self) -> Vec<(ObjHandle, SubObjId, bool)> {
        self.anim_channels
            .iter()
            .map(|c| (c.owner, c.gun, c.pitch))
            .collect()
    }
}

/// `PhySys::GetCenterOfMass`'s product: `center` (object space) through
/// the frame `m`, `t`. The first-column products stay on the FPU, the third
/// row's is stored; the second column is added and the first two rows
/// stored; the third column and the translation are added last, each
/// component rounded once.
#[must_use]
pub(crate) fn physics_center_of_mass(m: &[[f32; 3]; 3], t: Vector, center: [f32; 3]) -> Vector {
    let [c0, c1, c2] = center.map(e);
    let x0 = e(m[0][0]) * c0;
    let y0 = e(m[1][0]) * c0;
    let z0 = r(e(m[2][0]) * c0);
    let x1 = r(e(m[0][1]) * c1 + x0);
    let y1 = r(e(m[1][1]) * c1 + y0);
    let z1 = e(m[2][1]) * c1 + e(z0);
    Vector::new(
        r(e(m[0][2]) * c2 + e(x1) + e(t.x)),
        r(e(m[1][2]) * c2 + e(y1) + e(t.y)),
        r(e(m[2][2]) * c2 + z1 + e(t.z)),
    )
}
