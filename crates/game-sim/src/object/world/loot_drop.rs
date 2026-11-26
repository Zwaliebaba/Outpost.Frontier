//! The server's explosion of a destroyed equipped object and the loot it
//! drops (`EqObj::explode`, `collect_droppable_equip`, `drop_loot` and
//! `build_loot`, `Server/server_gameobject.cpp:2630-2711`, `3928-4058`),
//! and the container of one attached item (`EqObj::build_equip_loot`,
//! `:4058-4153`, with `ComputeExplosiveImpulses`,
//! `Common/common_misc.cpp:1272`): the parts of the C++ wrapper that read
//! and change the shared world. The server calls them from its `destroy`
//! and its `destroy_equip` loot (`game-logic::objects`).

use neuron_core::math::{Matrix, Vector};

use crate::ai::fp::{e, r};
use crate::data::equip_desc::EquipDesc;
use crate::equip::{EquipKind, types};
use crate::object::{ObjHandle, damage};

use super::{LootSpawn, ServerHooks, World};

/// `RandUnit`'s scale: 1/32768.
const RAND_UNIT: f64 = 3.051_757_812_5e-5;

/// `GRandTwoPi`: 2π through a float.
const RAND_TWO_PI: f64 = 6.283_185_482_025_146_5;

/// `drop_loot` stops once this many containers are built.
const MAX_DROPPED: usize = 6;

impl World {
    /// `HardpointPosition`: a hardpoint of the object's instance tree in
    /// world space, through the world transform of the instance it sits on.
    #[must_use]
    pub fn hardpoint_world(&self, h: ObjHandle, name: &[u8]) -> Option<(Vector, Matrix)> {
        let o = self.get(h)?;
        let (found, data) = self.find_hardpoint(o.engine.instance, name)?;
        let instance = self.scene.instance_transform(Some(found));
        let rotated = neuron_core::math::transform_matrix(&instance.rotation, data.position());
        let t = instance.translation;
        let position = Vector::new(
            r(e(rotated.x) + e(t.x)),
            r(e(rotated.y) + e(t.y)),
            r(e(rotated.z) + e(t.z)),
        );
        let orientation = neuron_core::math::mul_matrix(&instance.rotation, &data.orientation());
        Some((position, orientation))
    }

    /// `EqObj::explode`: the archetype's explosion, when it has a radius and
    /// some effect, at the object's position, inflicted by the object
    /// itself (cause 4), skipping the object.
    pub fn explode_object_hooked(&mut self, h: ObjHandle, hooks: &mut dyn ServerHooks) {
        let Some(o) = self.get(h) else {
            return;
        };
        let a = &self.data.archetypes;
        let explosion_arch = match o.kind {
            crate::object::ObjectKind::Ship => a
                .ships
                .get(&o.archetype)
                .map(|s| s.eq_obj.root.explosion_arch),
            crate::object::ObjectKind::Solar => a
                .solars
                .get(&o.archetype)
                .map(|s| s.eq_obj.root.explosion_arch),
            _ => None,
        }
        .unwrap_or(0);
        let id = o.simple.id;
        self.explode_archetype(h, explosion_arch, id, Some(h), hooks);
    }

    /// `Loot::explode_on_destroy`: the container's explosion at its
    /// position, with no inflictor and nothing skipped (cause 4).
    pub fn explode_loot_hooked(&mut self, h: ObjHandle, hooks: &mut dyn ServerHooks) {
        let Some(o) = self.get(h) else {
            return;
        };
        let explosion_arch = self
            .data
            .archetypes
            .equipment
            .get(&o.archetype)
            .map_or(0, |e| e.root.explosion_arch);
        self.explode_archetype(h, explosion_arch, 0, None, hooks);
    }

    fn explode_archetype(
        &mut self,
        h: ObjHandle,
        explosion_arch: u32,
        inflictor: u32,
        skip: Option<ObjHandle>,
        hooks: &mut dyn ServerHooks,
    ) {
        let Some(o) = self.get(h) else {
            return;
        };
        let (system, position) = (o.physical.system, o.engine.position);
        if explosion_arch == 0 {
            return;
        }
        let Some(explosion) = self
            .data
            .archetypes
            .explosions
            .get(&explosion_arch)
            .cloned()
        else {
            return;
        };
        let epsilon = 0.0001f64;
        if !(e(explosion.radius) > epsilon) {
            return;
        }
        if !(e(explosion.impulse) > epsilon)
            && !(e(explosion.hull_damage) > epsilon)
            && !(e(explosion.energy_damage) > epsilon)
        {
            return;
        }
        let blast = damage::Blast {
            position,
            radius: explosion.radius,
            hull_damage: explosion.hull_damage,
            energy_damage: explosion.energy_damage,
        };
        self.explode_caused_hooked(
            system,
            &blast,
            damage::CAUSE_EXPLOSION,
            inflictor,
            skip,
            explosion.impulse,
            hooks,
        );
    }

    /// `Loot::split_stack`: a cargo-pod container holding more units than
    /// its contents' `units_per_container` breaks into at most five
    /// containers at its position, each with the container's velocity plus
    /// a random kick of up to 20 m/s and a random spin (five draws each).
    pub fn loot_split(&mut self, h: ObjHandle) -> Vec<LootSpawn> {
        let Some(o) = self.get(h) else {
            return Vec::new();
        };
        let crate::object::KindData::Loot(l) = &o.data else {
            return Vec::new();
        };
        let a = &self.data.archetypes;
        let pod = a.equipment.get(&o.archetype).is_some_and(|c| {
            matches!(
                c.kind,
                crate::data::archetype::equipment::EquipmentKind::CargoPod(_)
            )
        });
        if !pod {
            return Vec::new();
        }
        let Some(contents) = a.equipment.get(&l.contents_arch).cloned() else {
            return Vec::new();
        };
        let (orientation, position, owner) = (o.engine.orientation, o.engine.position, o.simple.id);
        let (contents_hit_pts, mut remaining) = (l.contents_hit_pts, l.units);
        let per_container = contents.units_per_container as u32;
        if !(per_container < remaining) {
            return Vec::new();
        }
        let appearance = match &contents.kind {
            crate::data::archetype::equipment::EquipmentKind::Commodity {
                loot_appearance, ..
            } => loot_appearance
                .and_then(|id| self.data.archetypes.equipment.get(&id).cloned())
                .unwrap_or_else(|| contents.clone()),
            _ => contents.clone(),
        };
        let mut out = Vec::new();
        let mut index = 0;
        while remaining != 0 && index <= 4 {
            index += 1;
            let id = self.allocate_id();
            let count = remaining.min(per_container);
            let base_velocity = self.velocity(h);
            let speed = f64::from(self.rng.rand()) * (1.0 / 32768.0);
            let dir = self.random_on_unit_sphere();
            let speed2 = speed + speed;
            let push = Vector::new(
                r(speed2 * e(dir.x)),
                r(speed2 * e(dir.y)),
                r(speed2 * e(dir.z)),
            );
            let kick = Vector::new(
                r(e(push.x) * 10.0),
                r(e(push.y) * 10.0),
                r(e(push.z) * 10.0),
            );
            let velocity = Vector::new(
                r(e(kick.x) + e(base_velocity.x)),
                r(e(kick.y) + e(base_velocity.y)),
                r(e(kick.z) + e(base_velocity.z)),
            );
            let m = orientation.m;
            let spin_a = r(f64::from(self.rng.rand()) * (1.0 / 32768.0) * 0.8 - 0.4);
            let spin_x = [
                r(e(m[0][0]) * e(spin_a)),
                r(e(m[1][0]) * e(spin_a)),
                r(e(m[2][0]) * e(spin_a)),
            ];
            let spin_b = r(f64::from(self.rng.rand()) * (1.0 / 32768.0) * 0.8 - 0.4);
            let spin_z = [
                r(e(m[0][2]) * e(spin_b)),
                r(e(m[1][2]) * e(spin_b)),
                r(e(m[2][2]) * e(spin_b)),
            ];
            out.push(LootSpawn {
                archetype: appearance.root.nickname,
                contents: contents.root.nickname,
                units: count,
                contents_hit_pts,
                owner,
                position,
                velocity,
                id: Some(id),
                hit_pts: appearance.root.hit_pts,
                orientation,
                angular_velocity: Vector::new(
                    r(e(spin_x[0]) + e(spin_z[0])),
                    r(e(spin_x[1]) + e(spin_z[1])),
                    r(e(spin_x[2]) + e(spin_z[2])),
                ),
                name_id: 0,
                temporary: false,
                can_ai_tractor: true,
            });
            remaining -= count;
        }
        out
    }

    /// `collect_droppable_equip`: every item, external list first, that is
    /// not destroyed, not a cargo pod, lootable (the archetype allows it and
    /// it is a good) and has a description.
    pub fn droppable_equip(&mut self, h: ObjHandle) -> Vec<EquipDesc> {
        let ids: Vec<_> = self
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .map(|eq| {
                eq.equipment
                    .iter()
                    .filter(|e| !e.is_destroyed())
                    .filter(|e| e.type_bits != types::CARGO_POD)
                    .filter(|e| e.lootable && self.data.goods.arch_to_good(e.arch_id) != 0)
                    .map(|e| e.id)
                    .collect()
            })
            .unwrap_or_default();
        ids.into_iter()
            .filter_map(|id| self.equip_desc(h, id))
            .collect()
    }

    /// `random_on_unit_sphere`: two draws.
    fn random_on_unit_sphere(&mut self) -> Vector {
        let u1 = r(f64::from(self.rng.rand()) * RAND_UNIT);
        let u2 = r(f64::from(self.rng.rand()) * RAND_UNIT);
        let cos_raw = e(u1) + e(u1) - 1.0;
        let mut cos_theta = r(cos_raw);
        if cos_raw < -1.0 {
            cos_theta = -1.0;
        }
        if cos_theta > 1.0 {
            cos_theta = 1.0;
        }
        let theta = e(cos_theta).acos();
        let phi = e(u2) * RAND_TWO_PI;
        let sin_theta = theta.sin();
        Vector::new(
            r(phi.sin() * sin_theta),
            r(theta.cos()),
            r(phi.cos() * sin_theta),
        )
    }

    /// `CExternalEquip::GetCenterOfMass` and `CAttachedEquip`'s, as
    /// `build_loot` places an external item's (or a cargo stack's pod's)
    /// loot: an attachment's bounding-sphere centre in world space, an
    /// external item's hardpoint, the origin when not connected. `None`
    /// for an internal item, which the hardpoint search places.
    fn mounted_center(&mut self, h: ObjHandle, id: crate::equip::SubObjId) -> Option<Vector> {
        let eq = self.get(h)?.equipped.as_deref()?;
        let e = eq.equipment.by_id(id)?;
        let mounted = match &e.kind {
            EquipKind::Cargo(c) => eq.equipment.by_id(c.pod?)?,
            _ if e.mount.is_some() => e,
            _ => return None,
        };
        let mount = mounted.mount.clone()?;
        let attachment = mounted
            .attached
            .as_ref()
            .and_then(|a| a.attachment)
            .and_then(|a| a.root_instance());
        if !mount.connected {
            return Some(Vector::ZERO);
        }
        if let Some(root) = attachment {
            return Some(
                self.instance_sphere(Some(root))
                    .map_or(Vector::ZERO, |(_, c)| c),
            );
        }
        Some(
            self.hardpoint_world(h, &mount.hardpoint)
                .map_or(Vector::ZERO, |(p, _)| p),
        )
    }

    /// `drop_loot`'s containers (`build_loot` for each description, until
    /// six are built): each container holds at most the archetype's
    /// `units_per_container`, starts where the item was (its centre of
    /// mass, else its hardpoint, else the object), with the object's
    /// orientation and velocity plus a random kick of up to 30 m/s, and a
    /// random spin about the object's x and z axes. Five `rand()` draws per
    /// container. A non-mission description builds at most six.
    pub fn loot_drops(&mut self, h: ObjHandle, descs: &[EquipDesc]) -> Vec<LootSpawn> {
        let mut out = Vec::new();
        let mut total = 0;
        for desc in descs {
            if total >= MAX_DROPPED {
                break;
            }
            total += self.build_loot(h, desc, &mut out);
        }
        out
    }

    fn build_loot(&mut self, h: ObjHandle, desc: &EquipDesc, out: &mut Vec<LootSpawn>) -> usize {
        let Some(arch) = self.data.archetypes.equipment.get(&desc.arch_id).cloned() else {
            return 0;
        };
        let Some(o) = self.get(h) else {
            return 0;
        };
        let (orientation, mut position, owner) =
            (o.engine.orientation, o.engine.position, o.simple.id);
        // `get_loot_appearance`: a commodity's appearance, else the item.
        let appearance = match &arch.kind {
            crate::data::archetype::equipment::EquipmentKind::Commodity {
                loot_appearance, ..
            } => loot_appearance
                .and_then(|id| self.data.archetypes.equipment.get(&id).cloned())
                .unwrap_or_else(|| arch.clone()),
            _ => arch.clone(),
        };
        if desc.hardpoint != self.strings.cargo_bay() {
            match self.mounted_center(h, desc.id) {
                Some(center) => position = center,
                None => {
                    let name = self.strings.get(desc.hardpoint).to_vec();
                    if let Some((p, _)) = self.hardpoint_world(h, &name) {
                        position = p;
                    }
                }
            }
        }
        let contents_hit_pts = r(e(desc.health) * e(arch.root.hit_pts));
        let per_container = arch.units_per_container;
        let mut remaining = desc.count;
        let mut built = 0;
        if remaining <= 0 {
            return 0;
        }
        loop {
            if !desc.mission && built > 5 {
                return built;
            }
            let id = self.allocate_id();
            // The unsigned comparison of the C++.
            let count = if (remaining as u32) > (per_container as u32) {
                per_container
            } else {
                remaining
            };
            let mut velocity = self.velocity(h);
            let scale = f64::from(self.rng.rand()) * RAND_UNIT;
            let dir = self.random_on_unit_sphere();
            let push = Vector::new(
                r((scale + scale) * e(dir.x)),
                r((scale + scale) * e(dir.y)),
                r((scale + scale) * e(dir.z)),
            );
            let kick = Vector::new(
                r(e(push.x) * 15.0),
                r(e(push.y) * 15.0),
                r(e(push.z) * 15.0),
            );
            velocity = Vector::new(
                r(e(kick.x) + e(velocity.x)),
                r(e(velocity.y) + e(kick.y)),
                r(e(velocity.z) + e(kick.z)),
            );
            let m = orientation.m;
            let spin_a = r(
                f64::from(self.rng.rand()) * RAND_UNIT * 0.800_000_011_920_929
                    - 0.400_000_005_960_464_5,
            );
            let col0 = [
                r(e(m[0][0]) * e(spin_a)),
                r(e(m[1][0]) * e(spin_a)),
                r(e(m[2][0]) * e(spin_a)),
            ];
            let spin_b = r(
                f64::from(self.rng.rand()) * RAND_UNIT * 0.800_000_011_920_929
                    - 0.400_000_005_960_464_5,
            );
            let col2z = r(e(m[2][2]) * e(spin_b));
            let angular_velocity = Vector::new(
                r(e(m[0][2]) * e(spin_b) + e(col0[0])),
                r(e(m[1][2]) * e(spin_b) + e(col0[1])),
                r(e(col2z) + e(col0[2])),
            );
            out.push(LootSpawn {
                archetype: appearance.root.nickname,
                contents: arch.root.nickname,
                units: count as u32,
                contents_hit_pts,
                owner,
                position,
                velocity,
                id: Some(id),
                hit_pts: appearance.root.hit_pts,
                orientation,
                angular_velocity,
                name_id: 0,
                temporary: desc.mission,
                can_ai_tractor: true,
            });
            built += 1;
            remaining -= count;
            if remaining <= 0 {
                return built;
            }
        }
    }

    /// `CAttachedEquip::GetVelocity`: the velocity of the item's physics
    /// owner (the owner itself for a non-physical attachment, the item's
    /// object for a physical one); zero when it is not connected.
    fn attached_velocity(&self, h: ObjHandle, id: crate::equip::SubObjId) -> Vector {
        let Some(e) = self
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.by_id(id))
        else {
            return Vector::ZERO;
        };
        let attachment = e.attached.as_ref().and_then(|a| a.attachment);
        let connected = e.mount.as_ref().is_some_and(|m| m.connected)
            && attachment.is_some_and(|a| a.root_instance().is_some());
        if !connected {
            return Vector::ZERO;
        }
        match attachment {
            Some(crate::equip::Attachment::Physical { object, .. }) => self.velocity(object),
            _ => self.velocity(h),
        }
    }

    /// `ComputeExplosiveImpulses(instance, linear, angular1, angular2)`'s
    /// second out vector, the one `build_equip_loot` uses: for a joint of
    /// type 1 to 4 the joint's origin (`Relative` words 1 to 3) by the
    /// parent's orientation, otherwise its offset (words 4 to 6) by the
    /// parent's orientation; normalised in doubles (a zero length leaves it).
    fn explosive_impulse(&self, instance: neuron_core::scene::InstanceId) -> Vector {
        let parent = self.scene.instance_parent(Some(instance));
        let words = self
            .scene
            .instance_relative(Some(instance))
            .map_or([0; neuron_core::scene::joint::PAYLOAD_DWORDS], |p| p.0);
        let item = |at: usize| {
            Vector::new(
                f32::from_bits(words[at]),
                f32::from_bits(words[at + 1]),
                f32::from_bits(words[at + 2]),
            )
        };
        let count = words[0] as i32;
        let local = if (1..=4).contains(&count) {
            item(1)
        } else {
            item(4)
        };
        let orientation = self.scene.instance_orientation(parent);
        let mut v = neuron_core::math::transform_matrix(&orientation, local);
        let (x, y, z) = (e(v.x), e(v.y), e(v.z));
        let length = ((x * x + y * y) + z * z).sqrt();
        if length != 0.0 {
            let inverse = 1.0 / length;
            v = Vector::new(r(inverse * x), r(inverse * y), r(inverse * z));
        }
        v
    }

    /// `EqObj::build_equip_loot` (`Server/server_gameobject.cpp:4058-4153`):
    /// one container of the attached item `id`: a new object id, the item's
    /// archetype and hit points; a cargo pod carries its stack (contents,
    /// their hit points and count), any other item itself (one unit); the
    /// item's root instance's position and orientation; the velocity of its
    /// physics owner plus `ComputeExplosiveImpulses`' push times
    /// `child_impulse / mass` and a jitter of `rand() / 32768 * 0.5 + 0.75`;
    /// a spin of three `rand() / 32768` (the first to y, then x and z)
    /// scaled to `rand() / 32768 * 3 + 2`; mission loot when the item is
    /// temporary. Five `rand()` draws. `None` for an item that is not
    /// attached (the C++ casts first).
    pub fn build_equip_loot(
        &mut self,
        h: ObjHandle,
        id: crate::equip::SubObjId,
    ) -> Option<LootSpawn> {
        let (arch_id, hit_pts, pod_cargo, instance, temporary) = {
            let eq = self.get(h)?.equipped.as_deref()?;
            let e = eq.equipment.by_id(id)?;
            let attached = e.attached.as_ref()?;
            let pod_cargo = match &e.kind {
                EquipKind::CargoPod(p) => p.contents,
                _ => None,
            };
            (
                e.arch_id,
                e.hit_points(),
                pod_cargo,
                attached.attachment.and_then(|a| a.root_instance()),
                e.is_temporary(),
            )
        };
        let owner = self.get(h)?.simple.id;
        let loot_id = self.allocate_id();
        let (contents, contents_hit_pts, units) = match pod_cargo.and_then(|c| {
            let eq = self.get(h)?.equipped.as_deref()?;
            let cargo = eq.equipment.by_id(c)?;
            match &cargo.kind {
                EquipKind::Cargo(k) => Some((cargo.arch_id, cargo.hit_points(), k.count)),
                _ => None,
            }
        }) {
            Some(stack) => stack,
            None => (arch_id, hit_pts, 1),
        };
        let position = self.scene.instance_position(instance);
        let orientation = self.scene.instance_orientation(instance);
        let mut impulse = instance.map_or(Vector::ZERO, |i| self.explosive_impulse(i));
        let (child_impulse, mass) = self
            .data
            .archetypes
            .equipment
            .get(&arch_id)
            .map_or((0.0, 0.0), |a| {
                (a.attached().map_or(0.0, |x| x.child_impulse), a.root.mass)
            });
        // The quotient stays on the x87 stack; each component is rounded
        // once.
        let scale = e(child_impulse) / e(mass);
        impulse = Vector::new(
            r(e(impulse.x) * scale),
            r(e(impulse.y) * scale),
            r(e(impulse.z) * scale),
        );
        let jitter = f64::from(self.rng.rand()) * RAND_UNIT * 0.5 + 0.75;
        impulse = Vector::new(
            r(jitter * e(impulse.x)),
            r(jitter * e(impulse.y)),
            r(jitter * e(impulse.z)),
        );
        let own = self.attached_velocity(h, id);
        let velocity = Vector::new(
            r(e(impulse.x) + e(own.x)),
            r(e(impulse.y) + e(own.y)),
            r(e(impulse.z) + e(own.z)),
        );
        let spin_y = r(f64::from(self.rng.rand() as f32) * RAND_UNIT);
        let spin_x = r(f64::from(self.rng.rand() as f32) * RAND_UNIT);
        let spin_z = r(f64::from(self.rng.rand() as f32) * RAND_UNIT);
        let spin = f64::from(self.rng.rand()) * RAND_UNIT * 3.0 + 2.0;
        let length =
            ((e(spin_x) * e(spin_x) + e(spin_y) * e(spin_y)) + e(spin_z) * e(spin_z)).sqrt();
        let factor = r(spin / length);
        let angular_velocity = Vector::new(
            r(e(factor) * e(spin_x)),
            r(e(factor) * e(spin_y)),
            r(e(factor) * e(spin_z)),
        );
        Some(LootSpawn {
            archetype: arch_id,
            contents,
            units,
            contents_hit_pts,
            owner,
            position,
            velocity,
            id: Some(loot_id),
            hit_pts,
            orientation,
            angular_velocity,
            name_id: 0,
            temporary,
            can_ai_tractor: true,
        })
    }
}
