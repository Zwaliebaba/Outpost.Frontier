//! Obstacle avoidance (`Common/common_obstacleavoidance.cpp`): the
//! `ObstacleAvoidance` probe a `PathfindManager` embeds at `+0x268`, its
//! slot average, and the obstacle-candidate helpers
//! (`ResolveObstacleCandidateSlot`, `NudgeObstaclePathPoint`). The slerp of
//! the same file is [`super::steer::rotate_toward_by_slerp`].
//!
//! Not here yet: `PathfindManager::ClassifyObstaclePhase`,
//! `PathfindManager::UpdateHeadingBlend` and `FindClosestObstacleToAvoid`,
//! which walk the scanner's scan list and the spatial partition.
//!
//! The sines and cosines come from the Rust standard library where the
//! reference uses `FSIN` / `FCOS` (decision T11), so the probe directions
//! and the turned headings are Tier B.

use neuron_core::math::{
    Quaternion, Transform, Vector, quaternion_to_matrix, rotate, transform_matrix, transform_xform,
};

use super::fp::{add, column2, cross, e, inline_fast_length, len_sq_xyz, neg, r, scale, sub};
use super::raycast::{Capsule, ray_segment_capsule_first_hit};
use super::steer::rotate_toward_by_slerp;
use super::world::{AiRand, AiWorld, ObjectView, SpatialQueries, class};

/// Number of deflection slots.
pub const SLOT_COUNT: usize = 25;

/// `2π` as the reference's double constant.
const TWO_PI: f64 = 6.283_185_482_025_146_5;
/// `1/32768`, the scale of a `rand()` draw.
const RAND_SCALE: f64 = 3.051_757_812_5e-5;
/// The writable `.data` float `GAvoidTurnGain`.
const AVOID_TURN_GAIN: f32 = 2.0;

/// `ObstacleAvoidance::Slot`: a remembered deflection (point and normal)
/// with its cooldown. `ResolveObstacleCandidateSlot` and
/// `NudgeObstaclePathPoint` reuse the record as a capsule (radius, start,
/// segment).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AvoidSlot {
    /// +0x00 the cooldown; above 0 while the slot counts (the capsule
    /// radius in the candidate helpers).
    pub timer: f32,
    /// +0x04 the point (the capsule start).
    pub point: Vector,
    /// +0x10 the direction (the capsule segment).
    pub direction: Vector,
}

impl AvoidSlot {
    /// The slot as the capsule the candidate helpers test.
    #[must_use]
    pub fn capsule(&self) -> Capsule {
        Capsule {
            radius: self.timer,
            start: self.point,
            dir: self.direction,
        }
    }
}

/// The process-wide index table `Update` shuffles once
/// (`GAvoidIndexTable`, `GAvoidIndexTableShuffled`). Every entry is zero
/// and nothing reads it, so only the flag remains: the first `Update` of
/// any ship that gets past the early exits draws 50 `rand()` values. The
/// caller owns one per simulation (decision T10: no globals).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AvoidIndexTable {
    /// Whether the shuffle has run.
    pub shuffled: bool,
}

impl AvoidIndexTable {
    /// The one-time shuffle: 25 rounds of two draws.
    fn shuffle_once<R: AiRand + ?Sized>(&mut self, rand: &mut R) {
        if self.shuffled {
            return;
        }
        for _ in 0..SLOT_COUNT {
            let _ = rand.rand();
            let _ = rand.rand();
        }
        self.shuffled = true;
    }
}

/// `ObstacleAvoidance` (`common_aisteer.h`, 0x318 bytes): the nose probe of
/// one ship. The owner and the physics system are the caller's (the
/// `PathfindManager` passes its ship and the system it is in).
#[derive(Clone, Debug, PartialEq)]
pub struct ObstacleAvoidance {
    /// +0x04 the physics system the probes query; 0 for none.
    pub phys_system_id: u32,
    /// +0x08 `-min.z` of the owner's archetype box.
    pub nose_distance: f32,
    /// +0x0c `max(|max.x - min.x|, |max.y - min.y|)`.
    pub breadth: f32,
    /// +0x10 the running time that turns the probe spiral.
    pub elapsed_time: f32,
    /// +0x14 the deflection slots.
    pub slots: [AvoidSlot; SLOT_COUNT],
    /// +0x2d0 the round-robin cursor.
    pub cursor_index: i32,
    /// +0x2d4 steering around a contact.
    pub active: bool,
    /// +0x2d8 the nearest blocking hit point.
    pub contact_point: Vector,
    /// +0x2e4 its normal.
    pub contact_normal: Vector,
    /// +0x2f0 the id of the last object the probe hit.
    pub contact_object_id: u32,
    /// +0x2f4 the escape heading.
    pub avoid_direction: Vector,
    /// +0x300 a point along it.
    pub avoid_point: Vector,
    /// +0x30c the probe hit something this tick.
    pub has_contact: bool,
    /// +0x310 counts down after an escape.
    pub contact_timer: f32,
    /// +0x314 distance to the contact over speed.
    pub contact_close_rate: f32,
}

impl Default for ObstacleAvoidance {
    fn default() -> Self {
        Self::new()
    }
}

impl ObstacleAvoidance {
    /// `ObstacleAvoidance::ObstacleAvoidance` (`common_obstacleavoidance.cpp:50`):
    /// the slot timers and the cursor zeroed, then `ResetContact`. The
    /// fields the constructor leaves alone are zero here.
    #[must_use]
    pub fn new() -> Self {
        Self {
            phys_system_id: 0,
            nose_distance: 0.0,
            breadth: 0.0,
            elapsed_time: 0.0,
            slots: [AvoidSlot::default(); SLOT_COUNT],
            cursor_index: 0,
            active: false,
            contact_point: Vector::ZERO,
            contact_normal: Vector::ZERO,
            contact_object_id: 0,
            avoid_direction: Vector::ZERO,
            avoid_point: Vector::ZERO,
            has_contact: false,
            contact_timer: 0.0,
            contact_close_rate: 0.0,
        }
    }

    /// `ObstacleAvoidance::ResetContact` (`common_obstacleavoidance.cpp:64`).
    pub fn reset_contact(&mut self) {
        self.active = false;
        self.has_contact = false;
        self.contact_object_id = 0;
        self.contact_close_rate = 0.0;
        self.contact_timer = 0.0;
    }

    /// `ObstacleAvoidance::AttachOwner` (`common_obstacleavoidance.cpp:76`):
    /// the elapsed time zeroed, the nose distance and breadth from the
    /// owner's archetype box (zero when `get_archetype_extents` fails).
    pub fn attach_owner(&mut self, owner: &ObjectView) {
        self.elapsed_time = 0.0;
        let (min, max) = owner.extents.unwrap_or((Vector::ZERO, Vector::ZERO));
        self.nose_distance = -min.z;
        let span_x = r((e(max.x) - e(min.x)).abs());
        let span_y = r((e(max.y) - e(min.y)).abs());
        self.breadth = if span_x > span_y { span_x } else { span_y };
    }

    /// `ObstacleAvoidance::GetCloseRateScale` (`common_obstacleavoidance.cpp:100`):
    /// 1 when inactive or when the owner watches no ship, else 0.8 above a
    /// close rate of 5, 0.4 below 1 and `((rate - 1) / 4)^2 * 0.4 + 0.4`
    /// between (the `_CIpow` is a double square here; Tier B).
    #[must_use]
    pub fn close_rate_scale(&self, owner_has_ship: bool) -> f32 {
        if !self.active || !owner_has_ship {
            return 1.0;
        }
        let rate = e(self.contact_close_rate);
        if rate > 5.0 {
            return 0.8;
        }
        if rate < 1.0 {
            return 0.4;
        }
        let excess = (rate - 1.0) * 0.25;
        r(excess * excess * e(0.4) + e(0.4))
    }

    /// `ObstacleAvoidance::GetAvoidDirection` (`common_obstacleavoidance.cpp:127`):
    /// the escape heading and the contact's id while active.
    #[must_use]
    pub fn get_avoid_direction(&self) -> Option<(Vector, u32)> {
        self.active
            .then_some((self.avoid_direction, self.contact_object_id))
    }

    /// `ObstacleAvoidance::GetAvoidPoint` (`common_obstacleavoidance.cpp:139`).
    #[must_use]
    pub fn get_avoid_point(&self) -> Option<(Vector, u32)> {
        self.active
            .then_some((self.avoid_point, self.contact_object_id))
    }

    /// The shared "drop everything" exit.
    fn clear(&mut self) {
        self.contact_object_id = 0;
        self.contact_close_rate = 0.0;
        self.contact_timer = 0.0;
        self.active = false;
        self.has_contact = false;
    }

    /// `ObstacleAvoidance::Update` (`common_obstacleavoidance.cpp:151`): one
    /// tick of the probe for `ship`, steering toward `target` (the
    /// `PathfindManager`'s cached target position; ignored when its squared
    /// length is not above 1). Ages the slots, casts the spiral probe from
    /// the nose, and when it is blocked casts three random rays into a cone
    /// ahead, remembers their hits in the slots and turns the heading toward
    /// their average. Draws `rand()` for the one-time table shuffle and two
    /// per cone ray.
    ///
    /// A ship that cannot be inspected leaves the state as it is (the
    /// reference dereferences the missing object; D160).
    #[allow(clippy::too_many_arguments)]
    pub fn update<W, S, R>(
        &mut self,
        world: &W,
        spatial: &S,
        ship: W::Handle,
        target: Vector,
        dt: f32,
        rand: &mut R,
        table: &mut AvoidIndexTable,
    ) where
        W: AiWorld + ?Sized,
        S: SpatialQueries + ?Sized,
        R: AiRand + ?Sized,
    {
        let Some(view) = world.inspect(ship) else {
            return;
        };

        for slot in &mut self.slots {
            if slot.timer > 0.0 {
                let left = e(slot.timer) - e(dt);
                slot.timer = r(left);
                if left < 0.0 {
                    slot.timer = 0.0;
                }
            }
        }
        self.elapsed_time = r(e(dt) + e(self.elapsed_time));
        let left = e(self.contact_timer) - e(dt);
        self.contact_timer = r(left);
        if left < 0.0 {
            self.contact_timer = 0.0;
        }

        if self.phys_system_id == 0 {
            self.clear();
            return;
        }

        let speed_ext = inline_fast_length(len_sq_xyz(view.velocity));
        let speed = r(speed_ext);
        if speed_ext < 1.0 {
            self.clear();
            return;
        }

        let spiral_angle = r((e(6.89) * e(self.elapsed_time)) % TWO_PI);
        let ship_xform = view.transform();
        let rotation = ship_xform.rotation;
        let forward = neg(column2(&rotation));
        let nose = add(ship_xform.translation, scale(forward, self.nose_distance));

        let look_ahead_ext = e(50.0) * e(self.nose_distance);
        let mut look_ahead = r(look_ahead_ext);
        if look_ahead_ext == 0.0 {
            self.clear();
            return;
        }
        if look_ahead > 750.0 {
            look_ahead = 750.0;
        }

        let spiral_radius = e(900.0) / e(look_ahead) * e(self.breadth);
        let probe_local = Vector::new(
            r(e(spiral_angle).cos() * spiral_radius),
            r(e(spiral_angle).sin() * spiral_radius),
            -900.0,
        );
        let probe_end = add(nose, transform_matrix(&rotation, probe_local));
        let hits = spatial.find_ray_collisions(self.phys_system_id, nose, probe_end, 8);
        let own_type = view.type_flags;

        table.shuffle_once(rand);

        let have_target = ((e(target.z) * e(target.z) + e(target.y) * e(target.y))
            + e(target.x) * e(target.x))
            > 1.0;

        let mut blocked = false;
        self.has_contact = false;
        for hit in &hits {
            let Some(hit_view) = world.handle_of(hit.object).and_then(|h| world.inspect(h)) else {
                continue;
            };
            if !hit_view.is(class::SIMPLE) {
                continue;
            }
            let hit_type = hit_view.type_flags;
            if hit_type & 0x0020_c150 == 0 || hit_type == own_type {
                continue;
            }
            let to_hit = sub(hit.point, nose);
            let hit_distance = r(inline_fast_length(len_sq_zyx(to_hit)));
            if have_target {
                let to_target = sub(target, nose);
                if inline_fast_length(len_sq_zyx(to_target)) < e(hit_distance) {
                    self.clear();
                    return;
                }
            }
            self.has_contact = true;
            self.contact_object_id = hit_view.id;
            if hit_distance < look_ahead {
                self.contact_point = hit.point;
                self.contact_normal = hit.normal;
                blocked = true;
            }
        }

        if !blocked {
            self.steer_to_target(spatial, nose, forward, target, have_target);
            return;
        }

        self.active = true;
        let cone_radius = r(e(look_ahead) * 0.5);
        let cone_depth = -look_ahead;
        for _ in 0..3 {
            let radius = r(f64::from(rand.rand()) * RAND_SCALE * e(cone_radius));
            let angle = f64::from(rand.rand()) * RAND_SCALE * TWO_PI;
            let cone_local = Vector::new(
                r(angle.cos() * e(radius)),
                r(angle.sin() * e(radius)),
                cone_depth,
            );
            let cone_end = add(transform_matrix(&rotation, cone_local), nose);
            // The normalised cone direction is computed and never read.
            let cone_hits = spatial.find_ray_collisions(self.phys_system_id, nose, cone_end, 1);
            let Some(cone_hit) = cone_hits.first() else {
                continue;
            };
            if let Some(index) = self.choose_slot() {
                let slot = &mut self.slots[index];
                slot.point = cone_hit.point;
                slot.direction = cone_hit.normal;
                slot.timer = 1.0;
            }
        }

        let Some(escape) = average_active_direction(&self.slots, &ship_xform) else {
            self.clear();
            return;
        };
        self.avoid_direction = escape;
        self.contact_timer = 1.0;

        let to_contact = sub(self.contact_point, nose);
        let contact_len_sq = len_sq_zyx(to_contact);
        self.contact_close_rate = if speed > 1.0 {
            r(inline_fast_length(contact_len_sq) / e(speed))
        } else {
            0.0
        };

        let a = self.avoid_direction;
        let escape_dot_ext =
            (e(forward.z) * e(a.z) + e(forward.y) * e(a.y)) + e(forward.x) * e(a.x);
        let escape_dot = r(escape_dot_ext);
        let heading = if escape_dot_ext.abs() <= 0.999 {
            let axis = cross(forward, a);
            let mut turn_angle = (1.0 - e(escape_dot)) * e(0.2);
            if e(escape_dot) > 0.0 {
                turn_angle = 0.0;
            }
            turn_about(axis, turn_angle * e(0.5), forward)
        } else {
            forward
        };

        self.avoid_direction = heading;
        let reach_x = r(e(heading.x) * e(look_ahead) * e(10.0));
        let reach_y = e(heading.y) * e(look_ahead) * e(10.0);
        let reach_z_scaled = r(e(heading.z) * e(look_ahead));
        self.avoid_point = Vector::new(
            r(e(reach_x) + e(nose.x)),
            r(reach_y + e(nose.y)),
            r(e(reach_z_scaled) * e(10.0) + e(nose.z)),
        );
    }

    /// The unblocked tail of `Update`: entered only while the contact timer
    /// is negative, which the clamp above never leaves it, so the reference
    /// always clears `Active` here; the rest is kept as written.
    fn steer_to_target<S: SpatialQueries + ?Sized>(
        &mut self,
        spatial: &S,
        nose: Vector,
        forward: Vector,
        target: Vector,
        have_target: bool,
    ) {
        if !(self.contact_timer < 0.0) || !have_target {
            self.active = false;
            return;
        }
        if spatial
            .find_ray_collisions(self.phys_system_id, nose, target, 1)
            .is_empty()
        {
            self.active = false;
            return;
        }
        let mut to_target = sub(target, nose);
        let inv = 1.0 / len_sq_zyx(to_target).sqrt();
        to_target = Vector::new(
            r(e(to_target.x) * inv),
            r(e(to_target.y) * inv),
            r(e(to_target.z) * inv),
        );
        let (t, f) = (to_target, forward);
        let axis = Vector::new(
            r(e(t.z) * e(f.y) - e(f.z) * e(t.y)),
            r(e(f.z) * e(t.x) - e(t.z) * e(f.x)),
            r(e(t.y) * e(f.x) - e(f.y) * e(t.x)),
        );
        let dot = (e(t.z) * e(f.z) + e(f.y) * e(t.y)) + e(f.x) * e(t.x);
        let heading = if dot.abs() > 0.98 {
            to_target
        } else {
            let half = (1.0 - dot) * e(AVOID_TURN_GAIN) * e(0.5);
            turn_about(axis, half, forward)
        };
        self.avoid_direction = heading;
        self.avoid_point = add(scale(heading, 1000.0), nose);
    }

    /// The slot choice of `Update`: from the cursor, the first expired slot
    /// (the cursor becomes the step count); otherwise the slot indexed by
    /// the step of the lowest timer below 1e8 (the step, not the slot it
    /// compared, as in the reference). `None` when no timer is below 1e8,
    /// where the reference writes the slot before the array (D161).
    fn choose_slot(&mut self) -> Option<usize> {
        let mut lowest = 100_000_000.0_f32;
        let mut lowest_step: Option<usize> = None;
        let mut cursor = self.cursor_index;
        for step in 0..SLOT_COUNT {
            let index = usize::try_from(cursor.rem_euclid(25)).unwrap_or(0);
            let timer = self.slots[index].timer;
            if timer <= 0.0 {
                self.cursor_index = i32::try_from(step).unwrap_or(0);
                return Some(index);
            }
            if lowest > timer {
                lowest = timer;
                lowest_step = Some(step);
            }
            cursor = cursor.wrapping_add(1);
        }
        lowest_step
    }
}

/// `(z*z + y*y) + x*x`, unrounded (`LenSqZyx`).
fn len_sq_zyx(v: Vector) -> f64 {
    (e(v.z) * e(v.z) + e(v.y) * e(v.y)) + e(v.x) * e(v.x)
}

/// `sqrt((x*x + y*y) + z*z)` (`VectorLengthX87`).
fn vector_length_x87(v: Vector) -> f64 {
    len_sq_xyz(v).sqrt()
}

/// The quaternion turn `Update` builds twice: `(cos h, axis * sin h /
/// |axis|)` through `QuaternionToMatrix`, applied to `v`.
fn turn_about(axis: Vector, half_angle: f64, v: Vector) -> Vector {
    let axis_scale = r(half_angle.sin() / len_sq_zyx(axis).sqrt());
    let quat = Quaternion::new(
        r(half_angle.cos()),
        r(e(axis.x) * e(axis_scale)),
        r(e(axis.y) * e(axis_scale)),
        r(e(axis.z) * e(axis_scale)),
    );
    transform_matrix(&quaternion_to_matrix(&quat), v)
}

/// `ObstacleAvoidance_AverageActiveDirection` (`common_obstacleavoidance.cpp:604`):
/// the mean over the live slots of `2 * unit(ship - point) + direction`,
/// normalised when its squared length is above 1e-5; `None` without a
/// live slot.
#[must_use]
pub fn average_active_direction(slots: &[AvoidSlot], ship: &Transform) -> Option<Vector> {
    let mut sum = Vector::ZERO;
    let mut count = 0_i32;
    for slot in slots {
        if !(slot.timer > 0.0) {
            continue;
        }
        let mut away = sub(ship.translation, slot.point);
        let inv = 1.0 / len_sq_zyx(away).sqrt();
        away = Vector::new(r(e(away.x) * inv), r(e(away.y) * inv), r(e(away.z) * inv));
        let term = Vector::new(
            r((e(away.x) + e(away.x)) + e(slot.direction.x)),
            r((e(away.y) + e(away.y)) + e(slot.direction.y)),
            r((e(away.z) + e(away.z)) + e(slot.direction.z)),
        );
        count += 1;
        sum = Vector::new(
            r(e(term.x) + e(sum.x)),
            r(e(term.y) + e(sum.y)),
            r(e(term.z) + e(sum.z)),
        );
    }
    if count <= 0 {
        return None;
    }
    let inv_count = 1.0 / f64::from(count);
    let mut out = Vector::new(
        r(e(sum.x) * inv_count),
        r(e(sum.y) * inv_count),
        r(e(sum.z) * inv_count),
    );
    let len_sq = len_sq_xyz(out);
    if !(len_sq > 1e-5) {
        return Some(out);
    }
    let inv = 1.0 / len_sq.sqrt();
    out = Vector::new(r(inv * e(out.x)), r(inv * e(out.y)), r(inv * e(out.z)));
    Some(out)
}

/// `FastInvSqrtNewton` (`common_obstacleavoidance.cpp:751`): the
/// `0x5f3759df` guess from the signed bits and `(1.5 - ((y*y)*x)*0.5) * y`,
/// unrounded.
#[must_use]
pub fn fast_inv_sqrt_newton(x: f32) -> f64 {
    let bits = i32::from_ne_bytes(x.to_bits().to_ne_bytes());
    let guess_bits = 0x5f37_59df_i32.wrapping_sub(bits >> 1);
    let y = e(f32::from_bits(u32::from_ne_bytes(guess_bits.to_ne_bytes())));
    (e(1.5) - ((y * y) * e(x)) * e(0.5)) * y
}

/// What `NudgeObstaclePathPoint` answers.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Nudge {
    /// 0: the path point to use (`point` itself, or the capsule hit of the
    /// direction swung 0.3 rad toward it).
    Point(Vector),
    /// 1: a degenerate geometry or a point aligned behind the obstacle.
    Degenerate,
    /// 2: the capsule hit is well inside the obstacle's distance.
    Inside,
    /// 3: a capsule cast missed.
    Miss,
}

impl Nudge {
    /// The reference's return value.
    #[must_use]
    pub fn code(self) -> i32 {
        match self {
            Self::Point(_) => 0,
            Self::Degenerate => 1,
            Self::Inside => 2,
            Self::Miss => 3,
        }
    }
}

/// `NudgeObstaclePathPoint` (`common_obstacleavoidance.cpp:768`): cast from
/// the middle of the candidate capsule toward the obstacle and decide
/// whether `point` can be used as the path point around it.
#[must_use]
pub fn nudge_obstacle_path_point(
    candidate: &AvoidSlot,
    point: Vector,
    obstacle: &Transform,
    extra_check: bool,
) -> Nudge {
    let capsule = candidate.capsule();
    let midpoint = add(candidate.point, scale(candidate.direction, 0.5));
    let obstacle_pos = obstacle.translation;

    let mut dir_to_obstacle = sub(obstacle_pos, midpoint);
    let inv = 1.0 / vector_length_x87(dir_to_obstacle);
    dir_to_obstacle = Vector::new(
        r(e(dir_to_obstacle.x) * inv),
        r(e(dir_to_obstacle.y) * inv),
        r(e(dir_to_obstacle.z) * inv),
    );
    let Some(hit0) = ray_segment_capsule_first_hit(midpoint, dir_to_obstacle, &capsule) else {
        return Nudge::Miss;
    };

    let unit_or_none = |v: Vector| -> Option<Vector> {
        let length = len_sq_zyx(v).sqrt();
        if length < 1.0 {
            return None;
        }
        Some(super::fp::normalize_by_length(v, r(length)))
    };
    let Some(obstacle_dir) = unit_or_none(sub(obstacle_pos, midpoint)) else {
        return Nudge::Degenerate;
    };
    let Some(point_dir) = unit_or_none(sub(point, midpoint)) else {
        return Nudge::Degenerate;
    };
    let mut obstacle_to_point = sub(point, obstacle_pos);
    let dist = len_sq_zyx(obstacle_to_point).sqrt();
    if dist < 1.0 {
        return Nudge::Degenerate;
    }
    let inv = 1.0 / dist;
    obstacle_to_point = Vector::new(
        r(e(obstacle_to_point.x) * inv),
        r(e(obstacle_to_point.y) * inv),
        r(e(obstacle_to_point.z) * inv),
    );

    let (p, o) = (point_dir, obstacle_dir);
    let align = (e(p.z) * e(o.z) + e(p.y) * e(o.y)) + e(o.x) * e(p.x);
    if align > e(0.9) {
        if !extra_check {
            return Nudge::Degenerate;
        }
        let nf = neg(column2(&obstacle.rotation));
        let t = obstacle_to_point;
        let facing = (e(nf.z) * e(t.z) + e(nf.y) * e(t.y)) + e(nf.x) * e(t.x);
        if !(facing < e(0.85)) {
            return Nudge::Degenerate;
        }
    }

    let len_sq_a = r(len_sq_zyx(sub(hit0, midpoint)));
    let len_sq_b = len_sq_zyx(sub(obstacle_pos, midpoint));
    let approx_a = inline_fast_length(e(len_sq_a)) * e(1.2);
    let approx_b = inline_fast_length(len_sq_b);
    if approx_a < approx_b {
        return Nudge::Inside;
    }

    let len_sq_c = r(len_sq_zyx(sub(point, obstacle_pos)));
    if !(fast_inv_sqrt_newton(len_sq_c) * e(len_sq_c) > e(100.0)) {
        return Nudge::Point(point);
    }

    let rotated = rotate_toward_by_slerp(obstacle_dir, point_dir, 0.3);
    match ray_segment_capsule_first_hit(midpoint, rotated, &capsule) {
        Some(hit) => Nudge::Point(hit),
        None => Nudge::Miss,
    }
}

/// `ResolveObstacleCandidateSlot` (`common_obstacleavoidance.cpp:948`): the
/// capsule standing for an obstacle. Without an archetype box, a sphere of
/// the larger of the physical radius and the atmosphere range at the
/// centre (segment `(0, 1, 0)`); with one, a capsule along the box's
/// strictly longest extent, its radius half the larger other extent, in
/// world space.
#[must_use]
pub fn resolve_obstacle_candidate_slot(candidate: &ObjectView) -> AvoidSlot {
    let xform = candidate.transform();
    let Some((min, max)) = candidate.extents else {
        let mut timer = candidate.radius;
        if candidate.atmosphere_range > candidate.radius {
            timer = candidate.atmosphere_range;
        }
        return AvoidSlot {
            timer,
            point: candidate.center,
            direction: Vector::new(0.0, 1.0, 0.0),
        };
    };

    let sum = add(min, max);
    let centre = scale(sum, 0.5);
    // The reference transforms the local centre and never reads it.
    let _ = transform_xform(&xform, centre);
    let extent_x = e(max.x) - e(min.x);
    let extent_y = r(e(max.y) - e(min.y));
    let extent_z = r(e(max.z) - e(min.z));

    // The strictly longest extent, X then Y, else Z; the other two take
    // the centre. Every "greater" leaves ties and NaN to the later arm.
    let (point, end, across);
    if extent_x > e(extent_y) && extent_x > e(extent_z) {
        point = Vector::new(min.x, centre.y, centre.z);
        end = Vector::new(max.x, centre.y, centre.z);
        across = if extent_y > extent_z {
            e(extent_y)
        } else {
            e(extent_z)
        };
    } else if e(extent_y) > extent_x && extent_y > extent_z {
        point = Vector::new(centre.x, min.y, centre.z);
        end = Vector::new(centre.x, max.y, centre.z);
        across = if extent_x > e(extent_z) {
            extent_x
        } else {
            e(extent_z)
        };
    } else {
        point = Vector::new(centre.x, centre.y, min.z);
        end = Vector::new(centre.x, centre.y, max.z);
        across = if extent_x > e(extent_y) {
            extent_x
        } else {
            e(extent_y)
        };
    }
    let direction = sub(end, point);
    AvoidSlot {
        timer: r(across * 0.5),
        point: transform_xform(&xform, point),
        direction: rotate(&xform, direction),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ai::world::{AiCommand, MotionModel, RayContact};
    use neuron_core::math::Matrix;
    use neuron_core::rng::CrtRand;

    #[test]
    fn average_of_two_slots() {
        let mut slots = [AvoidSlot::default(); SLOT_COUNT];
        slots[3] = AvoidSlot {
            timer: 1.0,
            point: Vector::new(0.0, 0.0, -10.0),
            direction: Vector::new(0.0, 0.0, 1.0),
        };
        let ship = Transform::IDENTITY;
        let dir = average_active_direction(&slots, &ship).expect("one live slot");
        assert!((dir.z - 1.0).abs() < 1e-6, "{dir:?}");
        assert_eq!(
            average_active_direction(&[AvoidSlot::default(); 4], &ship),
            None
        );
    }

    #[test]
    fn candidate_capsule_along_longest_extent() {
        let view = ObjectView {
            orientation: Matrix::IDENTITY,
            position: Vector::new(100.0, 0.0, 0.0),
            extents: Some((Vector::new(-10.0, -1.0, -2.0), Vector::new(10.0, 1.0, 2.0))),
            ..ObjectView::default()
        };
        let slot = resolve_obstacle_candidate_slot(&view);
        assert_eq!(slot.point, Vector::new(90.0, 0.0, 0.0));
        assert_eq!(slot.direction, Vector::new(20.0, 0.0, 0.0));
        assert_eq!(slot.timer, 2.0);
        let sphere = ObjectView {
            radius: 5.0,
            atmosphere_range: 7.0,
            center: Vector::new(1.0, 2.0, 3.0),
            ..ObjectView::default()
        };
        let slot = resolve_obstacle_candidate_slot(&sphere);
        assert_eq!(slot.timer, 7.0);
        assert_eq!(slot.direction, Vector::new(0.0, 1.0, 0.0));
    }

    #[test]
    fn close_rate_scale_bands() {
        let mut oa = ObstacleAvoidance::new();
        assert_eq!(oa.close_rate_scale(true), 1.0);
        oa.active = true;
        oa.contact_close_rate = 6.0;
        assert_eq!(oa.close_rate_scale(true), 0.8);
        assert_eq!(oa.close_rate_scale(false), 1.0);
        oa.contact_close_rate = 0.5;
        assert_eq!(oa.close_rate_scale(true), 0.4);
        oa.contact_close_rate = 3.0;
        assert!((oa.close_rate_scale(true) - 0.5).abs() < 1e-6);
    }

    /// One ship and one wall, for `Update`.
    struct OneShip {
        ship: ObjectView,
        wall: ObjectView,
    }

    impl AiWorld for OneShip {
        type Handle = u32;
        fn inspect(&self, h: u32) -> Option<ObjectView> {
            match h {
                1 => Some(self.ship.clone()),
                2 => Some(self.wall.clone()),
                _ => None,
            }
        }
        fn handle_of(&self, id: u32) -> Option<u32> {
            Some(id)
        }
        fn system_objects(&self, _: u32, _: u32) -> Vec<u32> {
            vec![1, 2]
        }
        fn attitude(&self, _: u32, _: u32) -> Option<f32> {
            None
        }
        fn attitude_symmetrical(&self, _: u32, _: u32) -> Option<(f32, f32)> {
            None
        }
        fn hardpoint(&self, _: u32, _: &[u8]) -> Option<(Vector, Matrix)> {
            None
        }
        fn motion(&self, _: u32) -> Option<Box<dyn MotionModel + '_>> {
            None
        }
        fn system_of(&self, _: u32) -> Option<u32> {
            Some(1)
        }
        fn command(&mut self, _: u32, _: AiCommand<u32>) -> i32 {
            0
        }
    }

    /// Every ray hits the wall at z = -50.
    struct Wall;

    impl SpatialQueries for Wall {
        fn find_ray_collisions(
            &self,
            _: u32,
            from: Vector,
            to: Vector,
            _: usize,
        ) -> Vec<RayContact> {
            if to.z < -50.0 {
                let t = (-50.0 - from.z) / (to.z - from.z);
                vec![RayContact {
                    object: 2,
                    point: Vector::new(
                        from.x + (to.x - from.x) * t,
                        from.y + (to.y - from.y) * t,
                        -50.0,
                    ),
                    normal: Vector::new(0.0, 0.0, 1.0),
                    distance: 0.0,
                }]
            } else {
                Vec::new()
            }
        }
        fn find_ray_intersect(
            &self,
            _: u32,
            _: u32,
            _: Vector,
            _: Vector,
            _: usize,
        ) -> Vec<RayContact> {
            Vec::new()
        }
        fn find_sphere_collisions(&self, _: u32, _: Vector, _: f32, _: usize) -> Vec<u32> {
            Vec::new()
        }
    }

    fn world() -> OneShip {
        OneShip {
            ship: ObjectView {
                id: 1,
                class: class::SHIP,
                type_flags: 0x10000,
                orientation: Matrix::IDENTITY,
                velocity: Vector::new(0.0, 0.0, -100.0),
                extents: Some((Vector::new(-5.0, -2.0, -10.0), Vector::new(5.0, 2.0, 10.0))),
                ..ObjectView::default()
            },
            wall: ObjectView {
                id: 2,
                class: class::SOLAR,
                type_flags: 0x100,
                ..ObjectView::default()
            },
        }
    }

    #[test]
    fn blocked_probe_turns_away() {
        let w = world();
        let mut oa = ObstacleAvoidance::new();
        oa.attach_owner(&w.ship);
        assert_eq!(oa.nose_distance, 10.0);
        assert_eq!(oa.breadth, 10.0);
        oa.phys_system_id = 1;
        let mut rand = CrtRand::with_seed(3);
        let mut table = AvoidIndexTable::default();
        oa.update(&w, &Wall, 1, Vector::ZERO, 0.1, &mut rand, &mut table);
        assert!(table.shuffled);
        assert!(oa.active);
        assert!(oa.has_contact);
        assert_eq!(oa.contact_object_id, 2);
        assert_eq!(oa.contact_timer, 1.0);
        // The escape points back out of the wall, so the heading turns
        // away from -Z.
        let (dir, id) = oa.get_avoid_direction().expect("active");
        assert_eq!(id, 2);
        assert!(dir.z > -1.0, "{dir:?}");
        // 50 shuffle draws and two per cone ray.
        let mut reference = CrtRand::with_seed(3);
        for _ in 0..56 {
            reference.rand();
        }
        assert_eq!(rand, reference);
    }

    #[test]
    fn slow_or_unbound_probe_clears() {
        let mut w = world();
        let mut oa = ObstacleAvoidance::new();
        oa.attach_owner(&w.ship);
        oa.active = true;
        let mut rand = CrtRand::with_seed(3);
        let mut table = AvoidIndexTable::default();
        oa.update(&w, &Wall, 1, Vector::ZERO, 0.1, &mut rand, &mut table);
        assert!(!oa.active, "no physics system");
        oa.phys_system_id = 1;
        oa.active = true;
        w.ship.velocity = Vector::new(0.0, 0.0, -0.5);
        oa.update(&w, &Wall, 1, Vector::ZERO, 0.1, &mut rand, &mut table);
        assert!(!oa.active, "too slow");
        assert!(!table.shuffled);
    }

    #[test]
    fn slot_choice_takes_expired_then_lowest_step() {
        let mut oa = ObstacleAvoidance::new();
        oa.cursor_index = 3;
        assert_eq!(oa.choose_slot(), Some(3));
        assert_eq!(oa.cursor_index, 0);
        for (i, slot) in oa.slots.iter_mut().enumerate() {
            slot.timer = 1.0 - i as f32 * 0.01;
        }
        oa.cursor_index = 0;
        assert_eq!(oa.choose_slot(), Some(24));
    }

    #[test]
    fn nudge_codes() {
        let candidate = AvoidSlot {
            timer: 5.0,
            point: Vector::new(0.0, -50.0, 0.0),
            direction: Vector::new(0.0, 100.0, 0.0),
        };
        let obstacle = Transform::new(Matrix::IDENTITY, Vector::new(100.0, 0.0, 0.0));
        // Point on top of the obstacle.
        assert_eq!(
            nudge_obstacle_path_point(&candidate, obstacle.translation, &obstacle, false),
            Nudge::Degenerate
        );
        let n =
            nudge_obstacle_path_point(&candidate, Vector::new(0.0, 0.0, 300.0), &obstacle, true);
        assert!(
            matches!(n, Nudge::Point(_) | Nudge::Inside | Nudge::Miss),
            "{n:?}"
        );
        assert_eq!(Nudge::Inside.code(), 2);
        assert!((fast_inv_sqrt_newton(4.0) - 0.5).abs() < 2e-3);
    }
}
