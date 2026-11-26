//! The step of a [`PhysicsWorld`]: the PSI, contacts, phantoms, and the
//! queries that run against the world.

use parry3d::math::{Isometry, Real};
use parry3d::query::{Ray, ShapeCastOptions, cast_shapes, intersection_test};
use parry3d::shape::Ball;

use super::{
    Body, BodyHandle, CollisionEvent, ControllerContext, ControllerHost, ControllerKind, GolemKind,
    IvpQuat, IvpRandom, IvpTime, MOTION_IDLE_TIME, Phantom, PhantomEvent, PhantomHandle,
    PhysicsWorld, PsiClock, RayHit, Vector, cross, dot, e, names_collide, r, revive_core,
    short_name, unit_psi_begin, unit_psi_end,
};
use crate::physics::golem::GolemOutcome;
use crate::physics::ivp::core::FLAG_FIXED_MASK;
use crate::physics::ivp::psi;
use crate::physics::shape::{self, Geometry};

/// A phantom's description (`PhySys::PhantomParms` and the shape
/// argument of one of the three `CreatePhantom` overloads).
#[derive(Clone, Debug)]
pub struct PhantomDesc {
    /// The volume: [`Geometry::ball`], [`Geometry::cuboid`] or a part's
    /// surface.
    pub geometry: Geometry,
    /// The world position.
    pub position: Vector,
    /// The world orientation.
    pub orientation: crate::math::Matrix,
    /// +0x06 the collision-group name; bodies with the same name are not
    /// intruders.
    pub name: Vec<u8>,
    /// +0x40 whether a listener receives enter and leave events.
    pub listener: bool,
}

/// A beam's ray for [`PhysicsWorld::generate_beam_collision`]
/// (`PhysicalSystem::GenerateCollisions`).
#[derive(Clone, Debug)]
pub struct BeamQuery {
    /// The beam's key.
    pub user_key: u64,
    /// The start: the last position when the beam moved, else its position.
    pub start: Vector,
    /// The direction, as given (not normalised).
    pub direction: Vector,
    /// The length.
    pub length: f32,
    /// The beam's collision group; hits in it are skipped.
    pub collision_group: Vec<u8>,
    /// The beam's velocity, for the relative speed.
    pub velocity: Vector,
}

#[derive(Clone, Copy)]
struct Impact {
    a: BodyHandle,
    b: BodyHandle,
    time: f32,
    point: [f32; 3],
    normal: [f32; 3],
    part_a: u32,
    part_b: u32,
}

impl PhysicsWorld {
    /// `IVP_Environment_SimulateDTime`: one PSI of the clamped step, then
    /// the contacts and phantoms of that PSI. The collision list holds the
    /// events of this step only when the caller clears it first
    /// ([`PhysicsWorld::clear_collisions`]), as `PhysicalSystem::Update` does.
    pub fn simulate_dtime(
        &mut self,
        delta_time: f32,
        host: &mut dyn ControllerHost,
        random: &mut IvpRandom,
    ) {
        self.seed_fresh_phantom_overlaps();
        let mut clock = self.clock;
        clock.simulate_dtime(delta_time, |c| self.simulate_psi(c, host, random));
        self.clock = clock;
        self.update_phantoms();
    }

    /// Empties the collision list (`Collisions.clear()` at the start of
    /// `PhysicalSystem::Update`).
    pub fn clear_collisions(&mut self) {
        self.collisions.clear();
    }

    /// `PhySys::GetCollisions`.
    #[must_use]
    pub fn collisions(&self) -> &[CollisionEvent] {
        &self.collisions
    }

    /// The phantom events since the last call.
    pub fn take_phantom_events(&mut self) -> Vec<PhantomEvent> {
        std::mem::take(&mut self.phantom_events)
    }

    fn simulate_psi(
        &mut self,
        clock: &PsiClock,
        host: &mut dyn ControllerHost,
        random: &mut IvpRandom,
    ) {
        self.clock = *clock;
        let now = clock.current_time;
        // The wake queue (`IVP_Environment_Process_Wake_Queue`), from the
        // last queued core to the first: each revived unit goes to the
        // front of the simulated list, so the list starts in queue order.
        let queue = std::mem::take(&mut self.wake_queue);
        for &index in queue.iter().rev() {
            let Some(slot) = self.cores.get_mut(index) else {
                continue;
            };
            slot.wake_pending = false;
            if slot.simulation_disabled || !slot.core.is_frozen() {
                continue;
            }
            revive_core(&mut slot.core, &mut slot.unit_state, clock);
            let objects = slot.objects.clone();
            for o in objects {
                if !self.moving.contains(&o) {
                    self.moving.push(o);
                }
            }
            self.active.retain(|c| *c != index);
            self.active.insert(0, index);
        }
        // The simulation units, in `simulate_sim_units_psi`'s order.
        let event = clock.event_sim();
        let mut simulated = Vec::new();
        let mut trace = self.unit_trace.is_some().then(Vec::new);
        let trace_cores = self
            .unit_trace
            .as_ref()
            .is_some_and(|t| t.len() < super::UNIT_CORE_PSIS);
        let mut cores_trace = Vec::new();
        for index in super::unit_run_order(&self.active) {
            let Some(slot) = self.cores.get_mut(index) else {
                continue;
            };
            if slot.simulation_disabled || slot.core.is_frozen() {
                continue;
            }
            let before = trace_cores.then(|| crate::physics::replay::CoreRecord {
                core: slot.core.clone(),
                unit_state: slot.unit_state,
            });
            unit_psi_begin(&mut slot.core, &mut slot.unit_state, now);
            let motion_check = self.movement_check.tick(random);
            let mut controllers = slot.buckets.clone();
            controllers.sort_by_key(|(priority, _, _)| *priority);
            for (_, _, kind) in controllers.iter().rev() {
                self.run_controller(index, *kind, event, now, host);
            }
            if let Some(t) = trace.as_mut() {
                t.push((self.unit_key(index).unwrap_or(0), motion_check));
            }
            let Some(slot) = self.cores.get_mut(index) else {
                continue;
            };
            simulated.push(index);
            if unit_psi_end(
                &mut slot.core,
                &mut slot.unit_state,
                now,
                motion_check,
                MOTION_IDLE_TIME,
            ) {
                let objects = slot.objects.clone();
                self.moving.retain(|m| !objects.contains(m));
                self.active.retain(|c| *c != index);
            }
            if let (Some(before), Some(slot)) = (before, self.cores.get(index)) {
                let golem = slot
                    .objects
                    .first()
                    .and_then(|b| self.bodies.get(b.0))
                    .and_then(|b| match &b.golem {
                        Some(GolemKind::Part(g)) => {
                            let (t, c) = (g.target_quat, g.current_target_quat);
                            Some([t.x, t.y, t.z, t.w, c.x, c.y, c.z, c.w])
                        }
                        _ => None,
                    });
                cores_trace.push((
                    before,
                    crate::physics::replay::CoreRecord {
                        core: slot.core.clone(),
                        unit_state: slot.unit_state,
                    },
                    golem,
                ));
            }
        }
        if trace_cores {
            self.unit_core_trace.push(cores_trace);
        }
        if let (Some(t), Some(all)) = (trace, self.unit_trace.as_mut()) {
            all.push(t);
        }
        let commit = clock.commit_event_sim();
        for index in simulated.iter().rev() {
            if let Some(slot) = self.cores.get_mut(*index) {
                psi::calc_next_psi_matrix(
                    &mut slot.core,
                    commit,
                    now,
                    &self.limits,
                    clock.inv_delta_time,
                );
            }
        }
        self.contacts(clock);
    }

    fn run_controller(
        &mut self,
        core: super::Index,
        kind: ControllerKind,
        event: psi::EventSim,
        now: IvpTime,
        host: &mut dyn ControllerHost,
    ) {
        match kind {
            ControllerKind::Standard => {
                if let Some(slot) = self.cores.get_mut(core) {
                    slot.core
                        .gravity_controller_step(event.delta_time, [0.0; 3]);
                }
            }
            ControllerKind::External { key, handle } => {
                let beam = if let Some(slot) = self.cores.get_mut(core) {
                    let mut context = ControllerContext {
                        event,
                        current_time: now,
                        core: &mut slot.core,
                        beam: None,
                    };
                    host.do_simulation_controller(key, &mut context);
                    context.beam
                } else {
                    None
                };
                if let Some((position, orientation, optimize)) = beam
                    && let Some(&body) = self.controllers.get(handle.0)
                {
                    self.beam(body, position, &orientation, optimize);
                }
            }
            ControllerKind::Golem(body) => {
                let Some(b) = self.bodies.get_mut(body.0) else {
                    return;
                };
                let shift = b.shift_core_f_object;
                let Some(slot) = self.cores.get_mut(core) else {
                    return;
                };
                let outcome = match &mut b.golem {
                    Some(GolemKind::Part(g)) => g.simulate(&mut slot.core, event, now, shift),
                    Some(GolemKind::Remote(rc)) => rc.simulate(&slot.core, event, shift),
                    None => return,
                };
                if let GolemOutcome::Beam { rotation, position } = outcome {
                    self.beam_body(body, rotation, position, true);
                }
            }
        }
    }

    // ---- contacts (the port's solver, decision O19) --------------------

    fn collidable(&self, h: BodyHandle) -> Option<&Body> {
        let body = self.bodies.get(h.0)?;
        if !body.collisions_enabled || body.geometry.ledges.is_empty() {
            return None;
        }
        Some(body)
    }

    fn pair_allowed(&self, a: &Body, b: &Body) -> bool {
        if a.core == b.core || !names_collide(&a.name, &b.name) {
            return false;
        }
        self.collision_filter
            .is_none_or(|filter| filter(a.client_class, b.client_class))
    }

    fn body_motion(&self, h: BodyHandle, clock: &PsiClock) -> Option<(Isometry<Real>, [f32; 3])> {
        let (q, p) = self.object_pose_at(h, clock.time_of_last_psi)?;
        let core = self.core_of(h)?;
        let velocity = if core.is_frozen() {
            [0.0; 3]
        } else {
            core.delta_world_f_core_psis
        };
        Some((shape::isometry(q, p), velocity))
    }

    fn find_impact(&self, a: BodyHandle, b: BodyHandle, clock: &PsiClock) -> Option<Impact> {
        let (body_a, body_b) = (self.bodies.get(a.0)?, self.bodies.get(b.0)?);
        let (iso_a, vel_a) = self.body_motion(a, clock)?;
        let (iso_b, vel_b) = self.body_motion(b, clock)?;
        let options = ShapeCastOptions {
            max_time_of_impact: clock.delta_time,
            target_distance: 0.0,
            stop_at_penetration: true,
            compute_impact_geometry_on_penetration: true,
        };
        let (va, vb) = (shape::to_vector(vel_a), shape::to_vector(vel_b));
        let mut best: Option<Impact> = None;
        for la in body_a.geometry.ledges.iter() {
            if body_a.detached_parts.contains(&la.part_id) {
                continue;
            }
            for lb in body_b.geometry.ledges.iter() {
                if body_b.detached_parts.contains(&lb.part_id) {
                    continue;
                }
                let Ok(Some(hit)) =
                    cast_shapes(&iso_a, &va, &*la.shape, &iso_b, &vb, &*lb.shape, options)
                else {
                    continue;
                };
                if best.is_some_and(|i| i.time <= hit.time_of_impact) {
                    continue;
                }
                let t = hit.time_of_impact;
                let mut at = iso_a;
                at.translation.vector += va * t;
                let point = at * hit.witness1;
                let normal = at.rotation * hit.normal1.into_inner();
                best = Some(Impact {
                    a,
                    b,
                    time: t,
                    point: shape::from_point(&point),
                    normal: shape::from_vector(&normal),
                    part_a: la.part_id,
                    part_b: lb.part_id,
                });
            }
        }
        best
    }

    fn contacts(&mut self, clock: &PsiClock) {
        // Every collidable body once: whether its core is still, and its
        // swept bounding sphere (origin, radius plus the PSI's motion).
        struct Candidate {
            handle: BodyHandle,
            still: bool,
            origin: [f64; 3],
            reach: f64,
        }
        let mut candidates = Vec::new();
        for index in self.bodies.indices() {
            let handle = BodyHandle(index);
            let Some(body) = self.collidable(handle) else {
                continue;
            };
            let Some(slot) = self.cores.get(body.core) else {
                continue;
            };
            let still = slot.simulation_disabled || slot.core.is_frozen();
            let Some((iso, velocity)) = self.body_motion(handle, clock) else {
                continue;
            };
            let t = iso.translation.vector;
            let speed = shape::to_vector(velocity).norm();
            candidates.push(Candidate {
                handle,
                still,
                origin: [f64::from(t.x), f64::from(t.y), f64::from(t.z)],
                reach: f64::from(body.bound_radius)
                    + f64::from(speed) * f64::from(clock.delta_time),
            });
        }
        let mut impacts = Vec::new();
        for (i, a) in candidates.iter().enumerate() {
            for b in &candidates[i + 1..] {
                if a.still && b.still {
                    continue;
                }
                // The bounding spheres, swept over the PSI, keep the pair
                // apart: no impact (a conservative early out).
                let d = [0, 1, 2].map(|k| a.origin[k] - b.origin[k]);
                let gap = ((d[0] * d[0] + d[1] * d[1]) + d[2] * d[2]).sqrt();
                if gap > (a.reach + b.reach) * 1.001 + 0.01 {
                    continue;
                }
                let (Some(body_a), Some(body_b)) = (self.body(a.handle), self.body(b.handle))
                else {
                    continue;
                };
                if !self.pair_allowed(body_a, body_b) {
                    continue;
                }
                if let Some(impact) = self.find_impact(a.handle, b.handle, clock) {
                    impacts.push(impact);
                }
            }
        }
        for impact in impacts {
            self.resolve(impact, clock);
        }
    }

    /// The inverse mass and a function applying the world inverse inertia,
    /// zero for fixed or disabled cores.
    fn response_terms(&self, h: BodyHandle) -> Option<(f64, [[f64; 3]; 3], [f64; 3])> {
        let body = self.bodies.get(h.0)?;
        let slot = self.cores.get(body.core)?;
        let core = &slot.core;
        let rot = core.m_world_f_core_last_psi.rows.map(|row| row.map(e));
        if slot.simulation_disabled || core.flags & FLAG_FIXED_MASK != 0 {
            return Some((0.0, rot, [0.0; 3]));
        }
        Some((e(core.inv_mass), rot, core.inv_rot_inertia.map(e)))
    }

    fn resolve(&mut self, impact: Impact, clock: &PsiClock) {
        let (Some(ta), Some(tb)) = (self.response_terms(impact.a), self.response_terms(impact.b))
        else {
            return;
        };
        let (Some(core_a), Some(core_b)) = (self.core_of(impact.a), self.core_of(impact.b)) else {
            return;
        };
        let t = impact.time;
        let center = |c: &crate::physics::ivp::core::Core| {
            [0, 1, 2]
                .map(|i| e(c.pos_world_f_core_last_psi[i]) + e(c.delta_world_f_core_psis[i]) * e(t))
        };
        let p = impact.point.map(e);
        let (ca, cb) = (center(core_a), center(core_b));
        let ra = [p[0] - ca[0], p[1] - ca[1], p[2] - ca[2]];
        let rb = [p[0] - cb[0], p[1] - cb[1], p[2] - cb[2]];
        let world_spin = |c: &crate::physics::ivp::core::Core, rot: &[[f64; 3]; 3]| {
            let w = c.rot_speed.map(e);
            [0, 1, 2].map(|i| rot[i][0] * w[0] + rot[i][1] * w[1] + rot[i][2] * w[2])
        };
        let point_velocity =
            |c: &crate::physics::ivp::core::Core, rot: &[[f64; 3]; 3], lever: [f64; 3]| {
                let wv = cross(world_spin(c, rot), lever);
                let v = if c.is_frozen() {
                    [0.0; 3]
                } else {
                    c.speed.map(e)
                };
                [v[0] + wv[0], v[1] + wv[1], v[2] + wv[2]]
            };
        let va = point_velocity(core_a, &ta.1, ra);
        let vb = point_velocity(core_b, &tb.1, rb);
        let n = impact.normal.map(e);
        let rel = [vb[0] - va[0], vb[1] - va[1], vb[2] - va[2]];
        let vn = dot(rel, n);
        if !(vn < 0.0) {
            return;
        }
        let inv_inertia_world = |terms: &(f64, [[f64; 3]; 3], [f64; 3]), x: [f64; 3]| {
            let (_, rot, inv) = terms;
            let local = [0, 1, 2].map(|j| rot[0][j] * x[0] + rot[1][j] * x[1] + rot[2][j] * x[2]);
            let scaled = [local[0] * inv[0], local[1] * inv[1], local[2] * inv[2]];
            [0, 1, 2].map(|i| rot[i][0] * scaled[0] + rot[i][1] * scaled[1] + rot[i][2] * scaled[2])
        };
        let effective = |dir: [f64; 3]| {
            let ka = dot(cross(inv_inertia_world(&ta, cross(ra, dir)), ra), dir);
            let kb = dot(cross(inv_inertia_world(&tb, cross(rb, dir)), rb), dir);
            ta.0 + tb.0 + ka + kb
        };
        let k_n = effective(n);
        if !(k_n > 0.0) {
            return;
        }
        let jn = -(1.0 + self.elasticity) * vn / k_n;
        let tangent = [rel[0] - vn * n[0], rel[1] - vn * n[1], rel[2] - vn * n[2]];
        let vt = dot(tangent, tangent).sqrt();
        let mut impulse = [jn * n[0], jn * n[1], jn * n[2]];
        if vt > 1e-9 {
            let dir = [tangent[0] / vt, tangent[1] / vt, tangent[2] / vt];
            let k_t = effective(dir);
            if k_t > 0.0 {
                let jt = (vt / k_t).min(self.friction * jn);
                for i in 0..3 {
                    impulse[i] -= jt * dir[i];
                }
            }
        }
        // Two moving cores in contact share a friction system, a controller
        // of both: IVP merges their units and splits them again when the
        // contact ends, rebuilding the controller buckets each time.
        if ta.0 > 0.0 && tb.0 > 0.0 {
            self.rebuild_controller_buckets(impact.a);
            self.rebuild_controller_buckets(impact.b);
        }
        let relative_velocity = [0, 1, 2].map(|i| r(va[i] - vb[i]));
        self.apply_impulse(impact.b, impulse, rb, t);
        self.apply_impulse(impact.a, impulse.map(|c| -c), ra, t);
        self.record_collision(impact, relative_velocity, clock);
    }

    fn apply_impulse(&mut self, h: BodyHandle, impulse: [f64; 3], lever: [f64; 3], t: f32) {
        let Some(terms) = self.response_terms(h) else {
            return;
        };
        let (inv_mass, rot, inv) = terms;
        if inv_mass == 0.0 {
            return;
        }
        let ang_world = cross(lever, impulse);
        let local = [0, 1, 2].map(|j| {
            rot[0][j] * ang_world[0] + rot[1][j] * ang_world[1] + rot[2][j] * ang_world[2]
        });
        let d_rot = [0, 1, 2].map(|i| r(local[i] * inv[i]));
        let d_speed = impulse.map(|c| r(c * inv_mass));
        let frozen = self.core_of(h).is_some_and(|c| c.is_frozen());
        if frozen {
            if let Some(core) = self.core_of_mut(h) {
                core.add_speed_change(d_speed);
                core.add_rot_speed_change(d_rot);
            }
            self.wakeup(h);
            return;
        }
        if let Some(core) = self.core_of_mut(h) {
            let old = core.delta_world_f_core_psis;
            for i in 0..3 {
                core.speed[i] += d_speed[i];
                core.rot_speed[i] += d_rot[i];
            }
            core.delta_world_f_core_psis = core.speed;
            for i in 0..3 {
                core.pos_world_f_core_last_psi[i] += (old[i] - core.speed[i]) * t;
            }
        }
    }

    fn record_collision(&mut self, impact: Impact, relative_velocity: [f32; 3], clock: &PsiClock) {
        let now = clock.time_of_last_psi;
        let key = (impact.a, impact.b);
        let since = self
            .pair_last_collision
            .get(&key)
            .map_or(f64::INFINITY, |last| now.since(*last));
        let single = |w: &Self, h: BodyHandle| w.bodies.get(h.0).is_some_and(|b| b.single_contact);
        if since > self.min_time_between_collisions {
            self.pair_last_collision.insert(key, now);
            let (Some(ka), Some(kb)) = (self.user_key(impact.a), self.user_key(impact.b)) else {
                return;
            };
            self.collisions.push(CollisionEvent {
                object_a: ka,
                object_b: kb,
                part_id_a: impact.part_a,
                part_id_b: impact.part_b,
                point: Vector::from_words(impact.point),
                surface_normal: Vector::from_words(impact.normal.map(|c| -c)),
                relative_velocity: Vector::from_words(relative_velocity),
            });
        }
        for h in [impact.a, impact.b] {
            if single(self, h) {
                self.deactivate(h);
                self.set_collision_state(h, false);
            }
        }
    }

    // ---- phantoms -----------------------------------------------------

    /// `PhySys::CreatePhantom` (all three overloads; the shape is in the
    /// description).
    pub fn create_phantom(&mut self, desc: PhantomDesc) -> PhantomHandle {
        let q = IvpQuat::from_math(crate::math::matrix_to_quaternion(&desc.orientation));
        PhantomHandle(self.phantoms.insert(Phantom {
            geometry: desc.geometry,
            rotation: q,
            position: desc.position.to_words(),
            attached: None,
            name: short_name(&desc.name),
            enabled: true,
            listener: desc.listener,
            intruders: Vec::new(),
            pending: Vec::new(),
            fresh: true,
        }))
    }

    /// `PhySys::DestroyPhantom`: no leave events (the listener is dropped
    /// first, as `phantom_is_going_to_be_deleted_event` does).
    pub fn destroy_phantom(&mut self, h: PhantomHandle) {
        let _ = self.phantoms.remove(h.0);
    }

    /// `PhySys::AttachPhantom` (`IVP_Merge_Object_Into_Core`): the phantom
    /// joins the body's core at its current pose and follows the body. The
    /// core's upper limit radius and surface deviation grow to the
    /// phantom's own plus `distance`, the distance from `object_position`
    /// (the object's `WorldPosition`) to the phantom's position, as for an
    /// attached body ([`PhysicsWorld::attach`]).
    pub fn attach_phantom(
        &mut self,
        h: PhantomHandle,
        body: BodyHandle,
        object_position: Vector,
    ) -> bool {
        let now = self.clock.current_time;
        let Some((bq, bp)) = self.object_pose_at(body, now) else {
            return false;
        };
        let Some(p) = self.phantoms.get(h.0) else {
            return false;
        };
        if p.attached.is_some() {
            return false;
        }
        // The distance on the FPU stack, stored as a float.
        let dx = e(object_position.x) - e(p.position[0]);
        let dy = e(object_position.y) - e(p.position[1]);
        let dz = e(object_position.z) - e(p.position[2]);
        let distance = r(((dz * dz + dy * dy) + dx * dx).sqrt());
        let (old_ulr, old_msd) = p.geometry.radius_and_deviation([0.0; 3]);
        let mut m = crate::physics::ivp::math::IvpMatrix::IDENTITY;
        bq.set_matrix(&mut m);
        m.vv = bp;
        let rel_position = m.vimult4(p.position);
        let rel_rotation = IvpQuat::inv_mult(&bq, &p.rotation);
        if let Some(core) = self.core_of_mut(body) {
            let radius = e(distance) + e(old_ulr);
            if radius > e(core.upper_limit_radius) {
                core.upper_limit_radius = r(radius);
            }
            let limit = e(distance) + e(old_msd);
            if limit > e(core.max_surface_deviation) {
                core.max_surface_deviation = r(limit);
            }
            core.recompute_inverse_inertia();
        }
        if let Some(p) = self.phantoms.get_mut(h.0) {
            p.attached = Some(body);
            p.rotation = rel_rotation;
            p.position = rel_position;
        }
        true
    }

    /// `PhySys::DetachPhantom`: the phantom stays where it is.
    pub fn detach_phantom(&mut self, h: PhantomHandle) -> bool {
        let Some(p) = self.phantoms.get(h.0) else {
            return false;
        };
        let (rotation, position) = self.phantom_pose(p);
        match self.phantoms.get_mut(h.0) {
            Some(p) => {
                p.attached = None;
                p.rotation = rotation;
                p.position = position;
                true
            }
            None => false,
        }
    }

    /// `PhySys::GetCollisionStatePhantom`.
    #[must_use]
    pub fn phantom_collision_state(&self, h: PhantomHandle) -> bool {
        self.phantoms.get(h.0).is_some_and(|p| p.enabled)
    }

    /// `PhySys::SetCollisionStatePhantom`: a disabled phantom has no
    /// intruders.
    pub fn set_phantom_collision_state(&mut self, h: PhantomHandle, enable: bool) {
        if let Some(p) = self.phantoms.get_mut(h.0) {
            p.enabled = enable;
        }
    }

    /// The phantom's current intruders, in the order they entered.
    #[must_use]
    pub fn phantom_intruders(&self, h: PhantomHandle) -> Vec<u64> {
        self.phantoms.get(h.0).map_or_else(Vec::new, |p| {
            p.intruders
                .iter()
                .filter_map(|b| self.user_key(*b))
                .collect()
        })
    }

    fn phantom_pose(&self, p: &Phantom) -> (IvpQuat, [f32; 3]) {
        match p
            .attached
            .and_then(|b| self.object_pose_at(b, self.clock.current_time))
        {
            Some((q, pos)) => {
                let mut m = crate::physics::ivp::math::IvpMatrix::IDENTITY;
                q.set_matrix(&mut m);
                m.vv = pos;
                (IvpQuat::mult(&q, &p.rotation), m.vmult4(p.position))
            }
            None => (p.rotation, p.position),
        }
    }

    /// `PhySys::GetClosestPhantomIntruderPos`: the intruder nearest the
    /// phantom's centre that `filter` accepts, and the nearest point of its
    /// shape.
    #[must_use]
    pub fn closest_phantom_intruder(
        &self,
        h: PhantomHandle,
        filter: impl Fn(u64) -> bool,
    ) -> Option<(u64, Vector)> {
        let p = self.phantoms.get(h.0)?;
        let (_, center) = self.phantom_pose(p);
        let mut best: Option<(f32, u64, [f32; 3])> = None;
        for &b in &p.intruders {
            let Some(key) = self.user_key(b) else {
                continue;
            };
            if !filter(key) {
                continue;
            }
            let Some((q, pos)) = self.object_pose_at(b, self.clock.current_time) else {
                continue;
            };
            let iso = shape::isometry(q, pos);
            let body = &self.bodies.get(b.0)?;
            for ledge in body.geometry.ledges.iter() {
                let projection = ledge
                    .shape
                    .project_point(&iso, &shape::to_point(center), true);
                let point = shape::from_point(&projection.point);
                let d = [0, 1, 2].map(|i| point[i] - center[i]);
                let dist = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
                if best.is_none_or(|(bd, _, _)| dist < bd) {
                    best = Some((dist, key, point));
                }
            }
        }
        best.map(|(_, key, point)| (key, Vector::from_words(point)))
    }

    /// Whether the phantom `p` overlaps the body `h` at the bodies' poses
    /// now: the body collides, is not the phantom's own and is not of its
    /// collision group, and a ledge of each meets.
    fn phantom_overlaps(&self, p: &Phantom, h: BodyHandle) -> bool {
        if p.attached == Some(h) {
            return false;
        }
        let Some(body) = self.collidable(h) else {
            return false;
        };
        if !names_collide(&p.name, &body.name) {
            return false;
        }
        let Some((bq, bp)) = self.object_pose_at(h, self.clock.current_time) else {
            return false;
        };
        let (q, pos) = self.phantom_pose(p);
        let iso = shape::isometry(q, pos);
        // The radius around the phantom's origin that holds its ledges, for
        // the bounding-sphere early out below.
        let reach = f64::from(super::bound_radius(&p.geometry));
        let origin = iso.translation.vector;
        let body_iso = shape::isometry(bq, bp);
        // The two bounding spheres apart: no ledge can meet (a conservative
        // early out, as the contact pass's; the outcome is the exact
        // test's).
        let t = body_iso.translation.vector;
        let d = [
            f64::from(origin.x) - f64::from(t.x),
            f64::from(origin.y) - f64::from(t.y),
            f64::from(origin.z) - f64::from(t.z),
        ];
        let gap = ((d[0] * d[0] + d[1] * d[1]) + d[2] * d[2]).sqrt();
        if gap > (reach + f64::from(body.bound_radius)) * 1.001 + 0.01 {
            return false;
        }
        p.geometry.ledges.iter().any(|pl| {
            body.geometry.ledges.iter().any(|bl| {
                !body.detached_parts.contains(&bl.part_id)
                    && intersection_test(&iso, &*pl.shape, &body_iso, &*bl.shape).unwrap_or(false)
            })
        })
    }

    /// The start of a step: a phantom or a body made since the phantoms
    /// last looked (`update_phantoms`, at the end of the step before)
    /// overlaps at the pose it was made at. IVP makes their mindists when
    /// the object or phantom enters the OV tree and evaluates them in the
    /// next PSI at the pose before it, so such an overlap is an intruder of
    /// this step, as an overlap at the end of the step before is (D177).
    fn seed_fresh_phantom_overlaps(&mut self) {
        let fresh_bodies: Vec<BodyHandle> = self
            .bodies
            .indices()
            .into_iter()
            .map(BodyHandle)
            .filter(|&h| self.bodies.get(h.0).is_some_and(|b| b.phantom_fresh))
            .collect();
        for index in self.phantoms.indices() {
            let Some(p) = self.phantoms.get(index) else {
                continue;
            };
            if !p.enabled || (!p.fresh && fresh_bodies.is_empty()) {
                continue;
            }
            let candidates: Vec<BodyHandle> = if p.fresh {
                self.bodies.indices().into_iter().map(BodyHandle).collect()
            } else {
                fresh_bodies.clone()
            };
            let found: Vec<BodyHandle> = candidates
                .into_iter()
                .filter(|&h| !p.pending.contains(&h) && self.phantom_overlaps(p, h))
                .collect();
            if let Some(p) = self.phantoms.get_mut(index) {
                p.pending.extend(found);
            }
        }
        for index in self.phantoms.indices() {
            if let Some(p) = self.phantoms.get_mut(index) {
                p.fresh = false;
            }
        }
        for h in fresh_bodies {
            if let Some(b) = self.bodies.get_mut(h.0) {
                b.phantom_fresh = false;
            }
        }
    }

    fn update_phantoms(&mut self) {
        for index in self.phantoms.indices() {
            let Some(p) = self.phantoms.get(index) else {
                continue;
            };
            let mut inside = Vec::new();
            if p.enabled {
                for h in self.bodies.indices().into_iter().map(BodyHandle) {
                    if self.phantom_overlaps(p, h) {
                        inside.push(h);
                    }
                }
            }
            let listener = p.listener;
            let previous = p.intruders.clone();
            // IVP reports an intruder from its mindist with the phantom, and
            // `recalc_all_exact_mindists` evaluates the mindists in each PSI
            // after the cores' next matrices are committed, at the PSI's
            // time: the pose this test saw at the end of the step before.
            // So the intruders of a step are the bodies the phantom
            // overlapped at the end of the previous step, whether or not it
            // still overlaps them now (divergence register, D177). A
            // disabled phantom has none at once.
            let now_in: Vec<BodyHandle> = if p.enabled {
                p.pending
                    .iter()
                    .copied()
                    .filter(|h| self.collidable(*h).is_some())
                    .collect()
            } else {
                Vec::new()
            };
            let mut events = Vec::new();
            for h in &previous {
                if !now_in.contains(h)
                    && let Some(key) = self.user_key(*h)
                {
                    events.push(PhantomEvent {
                        phantom: PhantomHandle(index),
                        user_key: key,
                        entered: false,
                    });
                }
            }
            let mut kept: Vec<BodyHandle> = previous
                .iter()
                .copied()
                .filter(|h| now_in.contains(h))
                .collect();
            for h in &now_in {
                if !previous.contains(h) {
                    kept.push(*h);
                    if let Some(key) = self.user_key(*h) {
                        events.push(PhantomEvent {
                            phantom: PhantomHandle(index),
                            user_key: key,
                            entered: true,
                        });
                    }
                }
            }
            if let Some(p) = self.phantoms.get_mut(index) {
                p.intruders = kept;
                p.pending = inside;
            }
            if listener {
                self.phantom_events.extend(events);
            }
        }
    }

    // ---- queries ------------------------------------------------------

    /// The ray of a query from `from` to `to`, as `PhysicalSystem::
    /// FindRayCollisions` and `PhySys::FindRayIntersect` build its template
    /// (`Common/common_physicalsystem.cpp:693-716`,
    /// `common_physys_query.cpp:766-789`): the start, the direction (the
    /// difference times the inverse length, each stored as a float) and the
    /// length (a float). `z_first`: `FindRayIntersect`'s length sums the
    /// unrounded z square first.
    fn query_ray(from: Vector, to: Vector, z_first: bool) -> Option<IvpRay> {
        let dx = r(e(to.x) - e(from.x));
        let dy = r(e(to.y) - e(from.y));
        let dz_exact = e(to.z) - e(from.z);
        let dz = r(dz_exact);
        let length = if z_first {
            ((dz_exact * e(dz) + e(dx) * e(dx)) + e(dy) * e(dy)).sqrt()
        } else {
            ((e(dy) * e(dy) + e(dz) * e(dz)) + e(dx) * e(dx)).sqrt()
        };
        if !(length > 0.0) {
            return None;
        }
        let inv = 1.0 / length;
        Some(IvpRay {
            start: from.to_words(),
            dir: [r(e(dx) * inv), r(e(dy) * inv), r(e(dz) * inv)],
            length: r(length),
        })
    }

    /// The nearest hit of a ray on each body, nearest first: IVP's ray
    /// solver (`IVP/ivp_collision/ivp_ray_solver.cpp`), which all three
    /// queries use. `GenerateCollisions` and `FindRayCollisions` cast
    /// against every object (`check_ray_against_all_objects_in_sim`,
    /// `Common/common_physicalsystem.cpp:444-445`, `:720-721`), through
    /// `check_ray_against_ov_node` to `check_ray_against_object`;
    /// `FindRayIntersect` calls `check_ray_against_object` on its one object
    /// (`common_physys_query.cpp:694-695`). `check_ray_against_object` sends
    /// a polygon object's ledges through `PhySurfaceManager::
    /// insert_all_ledges_hitting_ray` (`common_physys_build.cpp:169-185`,
    /// detached parts masked) to the ledge test, and a ball to the ball test.
    ///
    /// Against every object (`only` is `None`) an object is tested only
    /// when its OV element passes the sphere test
    /// of `check_ray_against_ov_node` (`:323-347`): the core's position at
    /// the start of its PSI (`MWorldFCore`, not the pose the narrow test
    /// uses) within half the ray's length plus the core's upper limit
    /// radius of the ray's centre, and within that radius of the ray's line.
    /// A fast object whose pose now crosses the ray can fail it, and the
    /// ray then passes through. `culled`, when given, receives each such
    /// body the narrow test would have hit; without it a body the sphere
    /// test culls skips the narrow test, which could only fill `culled`.
    ///
    /// A ray does not hit a ledge it starts in: the ledge test needs a face
    /// that faces the ray with the start strictly in front of it
    /// (`IVP_Ray_Solver_Os::check_ray_against_compact_ledge_os`, `:438-466`),
    /// and the ball test returns when the start is inside the ball
    /// (`:230-236`). `inside` receives each body with a ledge that contains
    /// the start.
    fn ivp_ray_hits(
        &self,
        ray_in: &IvpRay,
        only: Option<BodyHandle>,
        inside: &mut Vec<BodyHandle>,
        mut culled: Option<&mut Vec<BodyHandle>>,
    ) -> Vec<RayHit> {
        let IvpRay { start, dir, length } = *ray_in;
        if !(length > 0.0) {
            return Vec::new();
        }
        let half = e(length) * 0.5;
        let centre = [0, 1, 2].map(|k| r(half * e(dir[k]) + e(start[k])));
        let ray = Ray::new(shape::to_point(start), shape::to_vector(dir));
        let mut hits = Vec::new();
        for h in self.bodies.indices().into_iter().map(BodyHandle) {
            if only.is_some_and(|o| o != h) {
                continue;
            }
            let Some(body) = self.bodies.get(h.0) else {
                continue;
            };
            let passes_ov = only.is_some()
                || self.cores.get(body.core).is_none_or(|slot| {
                    let core = &slot.core;
                    let radius = core.upper_limit_radius;
                    let reach = r(half + e(radius));
                    let c = core.m_world_f_core_last_psi.vv;
                    let d = [0, 1, 2].map(|k| r(e(c[k]) - e(centre[k])));
                    let quad = (e(d[1]) * e(d[1]) + e(d[2]) * e(d[2])) + e(d[0]) * e(d[0]);
                    if !(e(reach) * e(reach) > quad) {
                        return false;
                    }
                    let (a, b) = (dir.map(e), d.map(e));
                    let cross = [
                        r(a[1] * b[2] - a[2] * b[1]),
                        r(b[0] * a[2] - a[0] * b[2]),
                        r(a[0] * b[1] - a[1] * b[0]),
                    ];
                    let line = (e(cross[0]) * e(cross[0]) + e(cross[1]) * e(cross[1]))
                        + e(cross[2]) * e(cross[2]);
                    e(radius) * e(radius) > line
                });
            // A body the sphere test culls yields no hit and is never
            // `inside`; only a caller that keeps `culled` needs its narrow
            // test.
            if !passes_ov && culled.is_none() {
                continue;
            }
            let Some((q, pos)) = self.object_pose_at(h, self.clock.current_time) else {
                continue;
            };
            let iso = shape::isometry(q, pos);
            let mut best: Option<RayHit> = None;
            for ledge in body.geometry.ledges.iter() {
                if body.detached_parts.contains(&ledge.part_id) {
                    continue;
                }
                if ledge.shape.contains_point(&iso, &ray.origin) {
                    if passes_ov && inside.last() != Some(&h) {
                        inside.push(h);
                    }
                    continue;
                }
                let Some(hit) = ledge
                    .shape
                    .cast_ray_and_get_normal(&iso, &ray, length, true)
                else {
                    continue;
                };
                if best.is_some_and(|b| b.distance <= hit.time_of_impact) {
                    continue;
                }
                let point = ray.point_at(hit.time_of_impact);
                best = Some(RayHit {
                    body: h,
                    user_key: body.user_key,
                    point: Vector::from_words(shape::from_point(&point)),
                    normal: Vector::from_words(shape::from_vector(&hit.normal)),
                    part_id: ledge.part_id,
                    distance: hit.time_of_impact,
                });
            }
            if !passes_ov {
                if best.is_some()
                    && let Some(culled) = culled.as_deref_mut()
                {
                    culled.push(h);
                }
                continue;
            }
            hits.extend(best);
        }
        hits.sort_by(|a, b| a.distance.total_cmp(&b.distance));
        hits
    }

    /// The objects (user keys) whose ledges contained the start of a beam's
    /// ray since the last call, so that the ray did not hit them
    /// (diagnostic).
    pub fn take_beam_starts_inside(&mut self) -> Vec<u64> {
        std::mem::take(&mut self.beam_starts_inside)
    }

    /// The objects (user keys) a beam's ray crossed since the last call
    /// that IVP's OV sphere test kept it from hitting (diagnostic).
    pub fn take_beam_ov_culled(&mut self) -> Vec<u64> {
        std::mem::take(&mut self.beam_ov_culled)
    }

    /// `PhySys::FindRayCollisions`: the nearest hit per body, nearest
    /// first, at most `max_hits`.
    #[must_use]
    pub fn find_ray_collisions(&self, from: Vector, to: Vector, max_hits: usize) -> Vec<RayHit> {
        let Some(ray) = Self::query_ray(from, to, false) else {
            return Vec::new();
        };
        let mut hits = self.ivp_ray_hits(&ray, None, &mut Vec::new(), None);
        hits.truncate(max_hits);
        hits
    }

    /// `PhySys::FindRayIntersect`: the ray against one body.
    #[must_use]
    pub fn find_ray_intersect(
        &self,
        body: BodyHandle,
        from: Vector,
        to: Vector,
        max_hits: usize,
    ) -> Vec<RayHit> {
        let Some(ray) = Self::query_ray(from, to, true) else {
            return Vec::new();
        };
        let mut hits = self.ivp_ray_hits(&ray, Some(body), &mut Vec::new(), None);
        hits.truncate(max_hits);
        hits
    }

    /// `PhySys::FindSphereCollisions`: the bodies a sphere touches, in
    /// body order, at most `max_objects`.
    #[must_use]
    pub fn find_sphere_collisions(
        &self,
        center: Vector,
        radius: f32,
        max_objects: usize,
    ) -> Vec<u64> {
        let ball = Ball::new(radius.abs());
        let sphere_iso = shape::isometry(IvpQuat::IDENTITY, center.to_words());
        let mut out = Vec::new();
        for h in self.bodies.indices().into_iter().map(BodyHandle) {
            if out.len() >= max_objects {
                break;
            }
            let Some(body) = self.bodies.get(h.0) else {
                continue;
            };
            let Some((q, pos)) = self.object_pose_at(h, self.clock.current_time) else {
                continue;
            };
            let iso = shape::isometry(q, pos);
            let touches = body.geometry.ledges.iter().any(|l| {
                !body.detached_parts.contains(&l.part_id)
                    && intersection_test(&sphere_iso, &ball, &iso, &*l.shape).unwrap_or(false)
            });
            if touches {
                out.push(body.user_key);
            }
        }
        out
    }

    /// `PhysicalSystem::GenerateCollisions` for a beam (a ray, not a
    /// body): the nearest hit outside the beam's collision group becomes
    /// a collision event, which is also returned.
    pub fn generate_beam_collision(&mut self, beam: &BeamQuery) -> Option<CollisionEvent> {
        let mut start = beam.start;
        let mut length = beam.length;
        let fudge = self.bullet_length_fudge;
        if e(fudge) * e(0.5f32) < e(length) {
            length = r(e(fudge) + e(length));
            let scaled = beam.direction.to_words().map(|c| r(e(fudge) * e(c)));
            let half = scaled.map(|c| r(0.5 * e(c)));
            start = Vector::new(
                r(e(start.x) - e(half[0])),
                r(e(start.y) - e(half[1])),
                r(e(start.z) - e(half[2])),
            );
        }
        let dir = beam.direction.to_words();
        let group = short_name(&beam.collision_group);
        let (mut inside, mut culled) = (Vec::new(), Vec::new());
        let ray = IvpRay {
            start: start.to_words(),
            dir,
            length,
        };
        let hits = self.ivp_ray_hits(&ray, None, &mut inside, Some(&mut culled));
        for b in inside {
            if let Some(body) = self.bodies.get(b.0) {
                self.beam_starts_inside.push(body.user_key);
            }
        }
        for b in culled {
            if let Some(body) = self.bodies.get(b.0)
                && !(body.name[0] != 0 && group[0] != 0 && body.name == group)
            {
                self.beam_ov_culled.push(body.user_key);
            }
        }
        let hit = hits.into_iter().find(|h| {
            let name = self.bodies.get(h.body.0).map(|b| b.name).unwrap_or([0; 8]);
            !(name[0] != 0 && group[0] != 0 && name == group)
        })?;
        let step = dir.map(|c| r(e(hit.distance) * e(c)));
        let point = Vector::new(
            r(e(start.x) + e(step[0])),
            r(e(start.y) + e(step[1])),
            r(e(start.z) + e(step[2])),
        );
        let surface = self
            .core_of(hit.body)
            .map_or([0.0; 3], |c| c.surface_speed(point.to_words()));
        let event = CollisionEvent {
            object_a: hit.user_key,
            object_b: beam.user_key,
            part_id_a: hit.part_id,
            part_id_b: 0,
            point,
            surface_normal: hit.normal,
            relative_velocity: Vector::new(
                r(e(beam.velocity.x) - e(surface[0])),
                r(e(beam.velocity.y) - e(surface[1])),
                r(e(beam.velocity.z) - e(surface[2])),
            ),
        };
        self.collisions.push(event);
        Some(event)
    }
}

/// A ray as IVP's ray solver takes it (`IVP_Ray_Solver_Template`): the
/// start, the direction as given (normalised by the caller) and the length.
#[derive(Clone, Copy, Debug)]
struct IvpRay {
    start: [f32; 3],
    dir: [f32; 3],
    length: f32,
}
