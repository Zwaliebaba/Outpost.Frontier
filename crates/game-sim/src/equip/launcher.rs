//! Launchers, guns and droppers (`CppSource/Common/common_celauncher.cpp`,
//! `common_cegun.cpp`, `common_cedroppers.cpp`).
//!
//! A launcher's barrels are the `HPFire0N` hardpoints of its own model;
//! their world frames come from the engine instance tree, which the world
//! reads into a [`LaunchContext`] before it asks a launcher to fire. The
//! server-side wrappers (`SrvGun::fire`, `launch_bullets`) belong to
//! P3.3; the world's fire command ([`crate::object::World::fire`]) runs
//! the same sequence for the scenario harness and the AI.

use neuron_core::math::{Matrix, Vector};
use neuron_core::rng::CrtRand;
use neuron_core::scene::InstanceId;

use crate::data::archetype::equipment::{Equipment, EquipmentKind, Launcher as LauncherData};
use crate::object::csys::{look_matrix, random_vector};

use super::SubObjId;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `FireResult` (`include/common_cequip.h:837`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FireResult {
    /// 0: fired.
    Ok,
    /// The launcher is destroyed.
    Destroyed,
    /// The launcher is disabled.
    Disabled,
    /// The refire delay has not passed.
    RefireDelay,
    /// The owner lacks the power.
    NoPower,
    /// The owner lacks the ammunition.
    NoAmmo,
    /// The owner is cloaked.
    OwnerBlocked,
    /// The owner is in a trade lane.
    Tradelane,
    /// The target is outside the muzzle cone.
    MuzzleCone,
    /// The owner cruises.
    Cruise,
}

/// The projectile archetype a launcher fires (`Archetype::Projectile` and,
/// for munitions, `Archetype::Munition`; for mines and counter-measures the
/// projectile part).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ProjectileArch {
    /// The archetype's nickname id.
    pub nickname: u32,
    /// `lifetime`.
    pub lifetime: f32,
    /// `owner_safe_time`.
    pub owner_safe_time: f32,
    /// `requires_ammo` (+0x78).
    pub requires_ammo: bool,
    /// `force_gun_ori` (+0x79).
    pub force_gun_ori: bool,
    /// `hull_damage` (munitions).
    pub hull_damage: f32,
    /// `energy_damage` (munitions).
    pub energy_damage: f32,
    /// `weapon_type`.
    pub weapon_type: u32,
    /// `motor`.
    pub motor: u32,
    /// `seeker` (+0x90): 0 none, 1 dumb, 2 lock.
    pub seeker: u32,
    /// `time_to_lock`.
    pub time_to_lock: f32,
    /// `seeker_range`.
    pub seeker_range: f32,
    /// `seeker_fov_cos`.
    pub seeker_fov_cos: f32,
}

impl ProjectileArch {
    /// The values of a munition, mine or counter-measure archetype.
    #[must_use]
    pub fn of(arch: &Equipment) -> Option<Self> {
        let p = arch.projectile()?;
        let mut out = Self {
            nickname: arch.root.nickname,
            lifetime: p.lifetime,
            owner_safe_time: p.owner_safe_time,
            requires_ammo: p.requires_ammo,
            force_gun_ori: p.force_gun_ori,
            ..Self::default()
        };
        if let EquipmentKind::Munition(m) = &arch.kind {
            out.hull_damage = m.hull_damage;
            out.energy_damage = m.energy_damage;
            out.weapon_type = m.weapon_type;
            out.motor = m.motor;
            out.seeker = m.seeker;
            out.time_to_lock = m.time_to_lock;
            out.seeker_range = m.seeker_range;
            out.seeker_fov_cos = m.seeker_fov_cos;
        }
        Some(out)
    }
}

/// A barrel (`CELauncher::Barrel`, 0x34 bytes): the `HPFire0N` hardpoint.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Barrel {
    /// The instance carrying the hardpoint.
    pub instance: Option<InstanceId>,
    /// The hardpoint's position in it.
    pub position: Vector,
    /// Its orientation.
    pub orientation: Matrix,
}

/// What a launcher reads about its owner and the instance tree when it
/// fires.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LaunchContext {
    /// `IsDestroyed()` of the launcher.
    pub destroyed: bool,
    /// `IsDisabled()`.
    pub disabled: bool,
    /// The owner's power (+0x198).
    pub owner_power: f32,
    /// `GetAmmoCount()`: -1 when the projectile needs none.
    pub ammo: i32,
    /// The owner's cloak (+0x140).
    pub cloak_percent: f32,
    /// The owner is a ship in a trade lane.
    pub using_tradelane: bool,
    /// The owner is a ship with cruise engaged.
    pub cruise_active: bool,
    /// The owner's velocity (`get_velocity`).
    pub owner_velocity: Vector,
    /// Each bound barrel's world transform (engine instance transform),
    /// in barrel order.
    pub barrel_frames: Vec<(Matrix, Vector)>,
    /// `MUZZLE_CONE_ANGLE`, degrees.
    pub muzzle_cone_angle: f32,
}

impl LaunchContext {
    fn barrel_pos(&self, barrels: &[Barrel], index: usize) -> Vector {
        if self.destroyed {
            return Vector::ZERO;
        }
        match (barrels.get(index), self.barrel_frames.get(index)) {
            (Some(b), Some((rot, pos))) => {
                let t = neuron_core::math::Transform::new(*rot, *pos);
                neuron_core::math::transform_xform(&t, b.position)
            }
            _ => Vector::ZERO,
        }
    }

    fn barrel_dir(&self, barrels: &[Barrel], index: usize) -> Vector {
        if self.destroyed {
            return Vector::ZERO;
        }
        match (barrels.get(index), self.barrel_frames.get(index)) {
            (Some(b), Some((rot, _))) => {
                let world = neuron_core::math::mul_matrix(rot, &b.orientation);
                Vector::new(-world.m[0][2], -world.m[1][2], -world.m[2][2])
            }
            _ => Vector::ZERO,
        }
    }
}

/// One projectile to launch (`ProjLaunchInfo`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ProjLaunchInfo {
    /// The muzzle position.
    pub pos: Vector,
    /// The orientation.
    pub orient: Matrix,
    /// The velocity.
    pub velocity: Vector,
    /// A locked target and its sub-object.
    pub lock: Option<(crate::object::ObjHandle, SubObjId)>,
}

/// `CELauncher`'s state.
#[derive(Clone, Debug, PartialEq)]
pub struct Launcher {
    /// `damage_per_fire`.
    pub damage_per_fire: f32,
    /// +0x8c `power_usage`.
    pub power_usage: f32,
    /// +0x90 `refire_delay`.
    pub refire_delay: f32,
    /// `muzzle_velocity`.
    pub muzzle_velocity: f32,
    /// The projectile archetype's id (+0x98).
    pub projectile_archetype: u32,
    /// +0x4c the projectile archetype, `None` when it is missing (the
    /// reference reports it).
    pub projectile: Option<ProjectileArch>,
    /// +0x50 the time since the last shot (capped at 10000).
    pub refire_elapsed: f32,
    /// The bound barrels, at most eight.
    pub barrels: Vec<Barrel>,
}

impl Launcher {
    /// `CELauncher::CELauncher`.
    #[must_use]
    pub fn new(data: &LauncherData, projectile: Option<&Equipment>) -> Self {
        Self {
            damage_per_fire: data.damage_per_fire,
            power_usage: data.power_usage,
            refire_delay: data.refire_delay,
            muzzle_velocity: data.muzzle_velocity,
            projectile_archetype: data.projectile_archetype,
            projectile: projectile.and_then(ProjectileArch::of),
            refire_elapsed: 0.0,
            barrels: Vec::new(),
        }
    }

    /// `CELauncher::Update`'s own part.
    pub fn update(&mut self, dt: f32) {
        if self.refire_elapsed < 10000.0 {
            self.refire_elapsed = r(e(dt) + e(self.refire_elapsed));
        }
    }

    /// The barrel count (`CountLauncherBarrels`).
    #[must_use]
    pub fn barrel_count(&self) -> usize {
        self.barrels.len().min(8)
    }

    /// `GetPowerDrawPerFire`, unrounded.
    #[must_use]
    pub fn power_draw_per_fire(&self) -> f64 {
        self.barrel_count() as f64 * e(self.power_usage)
    }

    /// `PowerNeedsMet`.
    #[must_use]
    pub fn power_needs_met(&self, owner_power: f32) -> bool {
        e(owner_power) - self.power_draw_per_fire() >= 0.0
    }

    /// `AmmoNeedsMet`.
    #[must_use]
    pub fn ammo_needs_met(&self, ammo: i32) -> bool {
        if ammo < 0 {
            return true;
        }
        let need = self.barrel_count();
        !(need != 0 && (ammo as usize) < need)
    }

    /// `RefireDelayElapsed`.
    #[must_use]
    pub fn refire_delay_elapsed(&self) -> bool {
        self.refire_delay <= self.refire_elapsed
    }

    /// `CELauncher::CanFire`.
    #[must_use]
    pub fn can_fire(&self, cx: &LaunchContext) -> FireResult {
        if cx.destroyed {
            return FireResult::Destroyed;
        }
        if cx.disabled {
            return FireResult::Disabled;
        }
        if !self.refire_delay_elapsed() {
            return FireResult::RefireDelay;
        }
        if !self.power_needs_met(cx.owner_power) {
            return FireResult::NoPower;
        }
        if !self.ammo_needs_met(cx.ammo) {
            return FireResult::NoAmmo;
        }
        if cx.cloak_percent >= 1.0 {
            return FireResult::OwnerBlocked;
        }
        if cx.using_tradelane {
            return FireResult::Tradelane;
        }
        FireResult::Ok
    }

    /// `DrawPower(count)`: the owner's power less the shot, floored at 0.
    pub fn draw_power(&self, owner_power: &mut f32) {
        let cost = self.barrel_count() as f64 * e(self.power_usage);
        if !(cost > 0.0) {
            return;
        }
        let mut next = e(*owner_power) - cost;
        if 0.0 > next {
            next = 0.0;
        }
        *owner_power = r(next);
    }

    /// `ComputeProjectilePosAndVelocity_OneBarrel`.
    #[must_use]
    pub fn projectile_pos_and_velocity(
        &self,
        cx: &LaunchContext,
        barrel: usize,
        target: Vector,
    ) -> (Vector, Vector) {
        let muzzle = cx.barrel_pos(&self.barrels, barrel);
        let dx = e(target.x) - e(muzzle.x);
        let dy = e(target.y) - e(muzzle.y);
        let dz = r(e(target.z) - e(muzzle.z));
        let scale = r(e(self.muzzle_velocity) / ((e(dz) * e(dz) + dy * dy) + dx * dx).sqrt());
        let vel = Vector::new(r(dx * e(scale)), r(dy * e(scale)), r(e(dz) * e(scale)));
        (muzzle, vel)
    }

    /// `CELauncher::ComputeLaunchInfo_OneBarrel`: the muzzle velocity along
    /// the aim, the owner's velocity added, the orientation along the
    /// muzzle velocity alone.
    #[must_use]
    pub fn launch_info_one_barrel(&self, cx: &LaunchContext, barrel: usize, target: Vector) -> ProjLaunchInfo {
        let owner = cx.owner_velocity;
        let muzzle = cx.barrel_pos(&self.barrels, barrel);
        let delta = Vector::new(
            r(e(target.x) - e(muzzle.x)),
            r(e(target.y) - e(muzzle.y)),
            r(e(target.z) - e(muzzle.z)),
        );
        let scale = e(self.muzzle_velocity)
            / ((e(delta.y) * e(delta.y) + e(delta.z) * e(delta.z)) + e(delta.x) * e(delta.x)).sqrt();
        let dir = Vector::new(
            r(e(delta.x) * scale),
            r(e(delta.y) * scale),
            r(e(delta.z) * scale),
        );
        ProjLaunchInfo {
            pos: muzzle,
            velocity: Vector::new(
                r(e(owner.x) + e(dir.x)),
                r(e(dir.y) + e(owner.y)),
                r(e(dir.z) + e(owner.z)),
            ),
            orient: look_matrix(dir),
            lock: None,
        }
    }
}

/// `CEGun`'s state beyond the launcher.
#[derive(Clone, Debug, PartialEq)]
pub struct Gun {
    /// The launcher part.
    pub launcher: Launcher,
    /// `dispersion_angle` (+0xa0).
    pub dispersion_angle: f32,
    /// `turn_rate`.
    pub turn_rate: f32,
    /// `auto_turret`.
    pub auto_turret: bool,
    /// The locked target and its sub-object (`LockWatcher`, `LockSubId`).
    pub target: Option<(crate::object::ObjHandle, SubObjId)>,
    /// +0x26c the time the lock has held.
    pub lock_elapsed: f32,
    /// The point the turret aims at, and its age.
    pub target_point: Option<(Vector, f32)>,
}

impl Gun {
    /// `CEGun::CEGun`.
    #[must_use]
    pub fn new(data: &crate::data::archetype::equipment::Gun, projectile: Option<&Equipment>) -> Self {
        Self {
            launcher: Launcher::new(&data.launcher, projectile),
            dispersion_angle: data.dispersion_angle,
            turn_rate: data.turn_rate,
            auto_turret: data.auto_turret,
            target: None,
            lock_elapsed: 0.0,
            target_point: None,
        }
    }

    /// `is_guided`: the munition has a seeker.
    #[must_use]
    pub fn is_guided(&self) -> bool {
        self.launcher.projectile.is_some_and(|p| p.seeker != 0)
    }

    /// `CEGun::CanFire`: the launcher's tests, then the muzzle cone of the
    /// first barrel, then cruise.
    #[must_use]
    pub fn can_fire(&self, cx: &LaunchContext, target: Vector) -> FireResult {
        let result = self.launcher.can_fire(cx);
        if result != FireResult::Ok {
            return result;
        }
        let cone = r(e(cx.muzzle_cone_angle) * e(0.017_453_292_f32));
        let muzzle = cx.barrel_pos(&self.launcher.barrels, 0);
        let delta = Vector::new(target.x - muzzle.x, target.y - muzzle.y, target.z - muzzle.z);
        let len = (e(delta.z) * e(delta.z) + e(delta.y) * e(delta.y) + e(delta.x) * e(delta.x)).sqrt();
        let inv = 1.0 / len;
        let norm = Vector::new(
            r(e(delta.x) * inv),
            r(e(delta.y) * inv),
            r(e(delta.z) * inv),
        );
        let dir = cx.barrel_dir(&self.launcher.barrels, 0);
        let dot = e(norm.z) * e(dir.z) + e(norm.y) * e(dir.y) + e(norm.x) * e(dir.x);
        let cone_cos = e(cone).cos();
        if cone_cos < dot {
            if cx.cruise_active {
                return FireResult::Cruise;
            }
            return FireResult::Ok;
        }
        FireResult::MuzzleCone
    }

    /// `CEGun::Fire` (`CELauncher::Fire`): the refire clock restarts unless
    /// the refire delay refused the shot.
    pub fn fire(&mut self, cx: &LaunchContext, target: Vector) -> FireResult {
        let result = self.can_fire(cx, target);
        if result != FireResult::RefireDelay {
            self.launcher.refire_elapsed = 0.0;
        }
        result
    }

    /// `CEGun::ComputeLaunchInfo_OneBarrel`: unguided munitions take the
    /// velocity along the aim with the owner's added and look along it;
    /// the dispersion turns it by a random angle (two `rand()` draws for
    /// the axis, three in `RandomVector`, one for the angle).
    #[must_use]
    pub fn launch_info_one_barrel(
        &self,
        cx: &LaunchContext,
        barrel: usize,
        target: Vector,
        rng: &mut CrtRand,
    ) -> ProjLaunchInfo {
        let seeker = self.launcher.projectile.map_or(0, |p| p.seeker);
        let mut out = if seeker == 0 {
            let owner = cx.owner_velocity;
            let (pos, dir) = self.launcher.projectile_pos_and_velocity(cx, barrel, target);
            let velocity = Vector::new(
                r(e(dir.x) + e(owner.x)),
                r(e(dir.y) + e(owner.y)),
                r(e(dir.z) + e(owner.z)),
            );
            ProjLaunchInfo {
                pos,
                velocity,
                orient: look_matrix(velocity),
                lock: None,
            }
        } else {
            self.launcher.launch_info_one_barrel(cx, barrel, target)
        };
        let spread = self.dispersion_angle;
        if spread > 0.0 {
            let vel = out.velocity;
            let rand = random_vector(1.0, rng);
            let perp = Vector::new(
                r(e(vel.z) * e(rand.y) - e(rand.z) * e(vel.y)),
                r(e(rand.z) * e(vel.x) - e(vel.z) * e(rand.x)),
                r(e(vel.y) * e(rand.x) - e(vel.x) * e(rand.y)),
            );
            let speed = r(((e(vel.x) * e(vel.x) + e(vel.y) * e(vel.y)) + e(vel.z) * e(vel.z)).sqrt());
            let draw = rng.rand();
            let lean = (f64::from(draw) * (1.0 / 32768.0) * e(spread)).tan() * e(speed);
            let scale = lean
                / ((e(perp.z) * e(perp.z) + e(perp.y) * e(perp.y)) + e(perp.x) * e(perp.x)).sqrt();
            let lean_x = e(perp.x) * scale;
            let lean_y = r(e(perp.y) * scale);
            let lean_z = r(e(perp.z) * scale);
            let v = Vector::new(
                r(lean_x + e(vel.x)),
                r(e(lean_y) + e(vel.y)),
                r(e(lean_z) + e(vel.z)),
            );
            let keep = e(speed) / ((e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z)).sqrt();
            out.velocity = Vector::new(r(keep * e(v.x)), r(keep * e(v.y)), r(keep * e(v.z)));
            out.orient = look_matrix(out.velocity);
        }
        if seeker == 2
            && let Some(p) = self.launcher.projectile
            && e(p.time_to_lock) - e(self.lock_elapsed) <= 0.0
        {
            out.lock = self.target;
        }
        out
    }

    /// `ComputeLaunchInfo`: one record per barrel.
    #[must_use]
    pub fn launch_info(&self, cx: &LaunchContext, target: Vector, rng: &mut CrtRand) -> Vec<ProjLaunchInfo> {
        (0..self.launcher.barrel_count())
            .map(|b| self.launch_info_one_barrel(cx, b, target, rng))
            .collect()
    }

    /// `CEGun::GetMunitionRange` with the archetype's
    /// `Gun::get_munition_range`: a lock seeker answers its seeker range;
    /// otherwise lifetime times muzzle velocity, or the motor's burn
    /// formula.
    #[must_use]
    pub fn munition_range(&self, destroyed: bool, motor: Option<&crate::data::archetype::misc::Motor>) -> f32 {
        if destroyed {
            return 0.0;
        }
        let Some(p) = self.launcher.projectile else {
            return 0.0;
        };
        if p.seeker == 2 && p.seeker_range > 0.0 && p.seeker_fov_cos < 1.0 {
            return p.seeker_range;
        }
        let mv = e(self.launcher.muzzle_velocity);
        if p.motor == 0 {
            return r(e(p.lifetime) * mv);
        }
        let Some(motor) = motor else {
            return 0.0;
        };
        let burn = e(motor.lifetime);
        let accel = e(motor.accel);
        let mut sum = e(motor.delay) * mv;
        sum += burn * burn * accel * 0.5;
        sum += mv * burn;
        let post = (e(p.lifetime) - burn) * (burn * accel + mv);
        r(post + sum)
    }

    /// `CEGun::Update`'s lock clock, without turret joints: while a target
    /// is locked and the munition needs a lock time, the lock grows while
    /// `can_see` holds and restarts otherwise.
    pub fn update_lock(&mut self, dt: f32, can_see: bool) {
        if self.target.is_none() {
            return;
        }
        let Some(p) = self.launcher.projectile else {
            return;
        };
        if !(p.time_to_lock > 0.0) {
            return;
        }
        self.lock_elapsed = if can_see {
            r(e(dt) + e(self.lock_elapsed))
        } else {
            0.0
        };
    }
}

/// `CEGun::ComputeTimeToTgt`: the earliest positive time at which a shot at
/// `speed` from the origin meets a target at `rel_pos` moving at `rel_vel`.
#[must_use]
pub fn compute_time_to_target(rel_pos: Vector, rel_vel: Vector, speed: f32) -> Option<f32> {
    let dot = e(rel_pos.y) * e(rel_vel.y) + e(rel_pos.z) * e(rel_vel.z) + e(rel_pos.x) * e(rel_vel.x);
    let vel2 = r(e(rel_vel.x) * e(rel_vel.x) + e(rel_vel.y) * e(rel_vel.y) + e(rel_vel.z) * e(rel_vel.z)
        - e(speed) * e(speed));
    let pos2 = e(rel_pos.x) * e(rel_pos.x) + e(rel_pos.y) * e(rel_pos.y) + e(rel_pos.z) * e(rel_pos.z);
    let disc = dot * dot - pos2 * e(vel2);
    if !(disc >= 0.0) {
        return None;
    }
    let root = disc.sqrt();
    let inv = 1.0 / e(vel2);
    let t1 = r((root - dot) * inv);
    let t2 = r((-dot - root) * inv);
    let first = t1 > 0.0;
    let second = t2 > 0.0;
    if !first && !second {
        return None;
    }
    Some(if first && (!second || t1 < t2) { t1 } else { t2 })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn launcher() -> Launcher {
        Launcher {
            damage_per_fire: 0.0,
            power_usage: 10.0,
            refire_delay: 0.2,
            muzzle_velocity: 750.0,
            projectile_archetype: 1,
            projectile: Some(ProjectileArch {
                lifetime: 0.8,
                ..ProjectileArch::default()
            }),
            refire_elapsed: 0.0,
            barrels: vec![Barrel {
                instance: None,
                position: Vector::ZERO,
                orientation: Matrix::IDENTITY,
            }],
        }
    }

    fn cx() -> LaunchContext {
        LaunchContext {
            owner_power: 100.0,
            ammo: -1,
            barrel_frames: vec![(Matrix::IDENTITY, Vector::ZERO)],
            muzzle_cone_angle: 15.0,
            ..LaunchContext::default()
        }
    }

    #[test]
    fn refire_delay_gates_the_shot() {
        let mut gun = Gun {
            launcher: launcher(),
            dispersion_angle: 0.0,
            turn_rate: 0.0,
            auto_turret: false,
            target: None,
            lock_elapsed: 0.0,
            target_point: None,
        };
        let target = Vector::new(0.0, 0.0, -400.0);
        assert_eq!(gun.fire(&cx(), target), FireResult::RefireDelay);
        gun.launcher.update(0.25);
        assert_eq!(gun.fire(&cx(), target), FireResult::Ok);
        assert_eq!(gun.launcher.refire_elapsed, 0.0);
        gun.launcher.update(0.25);
        // Behind the barrel: outside the cone.
        assert_eq!(gun.fire(&cx(), Vector::new(0.0, 0.0, 400.0)), FireResult::MuzzleCone);
        let mut low = cx();
        low.owner_power = 5.0;
        gun.launcher.update(0.25);
        assert_eq!(gun.fire(&low, target), FireResult::NoPower);
    }

    #[test]
    fn unguided_launch_follows_the_aim() {
        let gun = Gun {
            launcher: launcher(),
            dispersion_angle: 0.0,
            turn_rate: 0.0,
            auto_turret: false,
            target: None,
            lock_elapsed: 0.0,
            target_point: None,
        };
        let mut rng = CrtRand::with_seed(1);
        let mut c = cx();
        c.owner_velocity = Vector::new(0.0, 0.0, -10.0);
        let info = gun.launch_info(&c, Vector::new(0.0, 0.0, -400.0), &mut rng);
        assert_eq!(info.len(), 1);
        assert_eq!(info[0].velocity, Vector::new(0.0, 0.0, -760.0));
        assert_eq!(info[0].orient.m[2][2], 1.0);
    }

    #[test]
    fn draw_power_floors_at_zero() {
        let l = launcher();
        let mut p = 4.0;
        l.draw_power(&mut p);
        assert_eq!(p, 0.0);
    }

    #[test]
    fn time_to_target_picks_the_earliest_positive_root() {
        let t = compute_time_to_target(Vector::new(0.0, 0.0, -750.0), Vector::ZERO, 750.0).unwrap();
        assert!((t - 1.0).abs() < 1e-6);
        assert!(compute_time_to_target(Vector::new(0.0, 0.0, -10.0), Vector::new(0.0, 0.0, -1000.0), 750.0).is_none());
    }
}
