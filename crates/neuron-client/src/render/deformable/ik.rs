//! Aim solving (`deformIK.cpp` in the reference's diagnostics): the aim
//! event `FIkEvent`, its typed channels (one per joint, the event their
//! owner), the single-joint look-at with the Euler clamp, and the two
//! damped-least-squares chain solvers over the SVD pseudo-inverse.
//!
//! The reference keeps the chain's current and trial poses in module
//! globals (`G_IkCurRot`, `G_IkCurPos`, `G_IkNewPos`, `G_IkNewRot`); every
//! solve writes them before it reads them, so they are a solve's locals
//! here. The targets are values the caller sets
//! ([`IkEvent::set_targets`]) where the reference borrows the caller's
//! floats (divergence D392).

use super::math::{
    JointLimits, euler_clamp, euler_rebuild, look_at, matrix_to_quat, mul_matrix9, quat_exp,
    quat_log, quat_to_matrix, transform_matrix9, transpose9, vec3,
};
use super::services::DeformServices;
use super::svd::{SvdMatrix, mul_ab, mul_vec, pseudo_inverse, transpose};
use super::{INSTANCE_NONE, Instance, dump, file};

#[allow(clippy::cast_possible_truncation)]
fn f(x: f64) -> f32 {
    x as f32
}

/// `G_IkMaxJoints`.
pub const MAX_JOINTS: usize = 4;

/// The aim description `StartAim` reads (`FDeformAimDesc` and the lock
/// list after it).
#[derive(Clone, Debug, PartialEq)]
pub struct AimDesc {
    /// The chain's root (+0x04).
    pub root: Instance,
    /// The end effector (+0x08).
    pub end: Instance,
    /// +0x0C: bit 0 the target is a point (else a direction), bits 2 to 4
    /// the look-at axis, bit 5 solve the position, bit 6 the orientation,
    /// bit 7 no rest pull and no Euler clamp, bit 8 the play header flag.
    pub flags: u32,
    /// The target point (+0x10's floats).
    pub target_pos: [f32; 3],
    /// The target orientation (+0x14's floats).
    pub target_orient: [f32; 9],
    /// The damping (+0x18).
    pub damping: f32,
    /// The locked joints (+0x1C count, +0x20 ids).
    pub locks: Vec<Instance>,
}

/// `FIkEvent`.
#[derive(Clone, Debug, PartialEq)]
pub struct IkEvent {
    /// The chain's joints, root side first (`JointIds`).
    pub joint_ids: Vec<Instance>,
    /// The joints' rotations when the aim started (`BindMats`).
    pub bind_mats: Vec<[f32; 9]>,
    /// `StartJoint`.
    pub start_joint: Instance,
    /// `EndJoint`.
    pub end_joint: Instance,
    /// One channel key per joint.
    pub channels: Vec<u32>,
    /// The solved local rotations, four words per joint.
    pub quats: Vec<f32>,
    /// The target point.
    pub target_pos: [f32; 3],
    /// The target orientation.
    pub target_orient: [f32; 9],
    /// The aim flags (`Flags28`).
    pub flags28: u32,
    /// One byte per joint: locked.
    pub locks: Vec<u8>,
    /// The lock count (`Extra30`).
    pub lock_count: i32,
    /// Bit 0: stopped; bit 1: the chain holds a sphere joint with an axis
    /// (the alternative solver).
    pub flags34: u8,
    /// The damping (`Damp38`).
    pub damping: f32,
    /// The channel tick (`Tick3C`): the solve runs on tick 0.
    pub tick: i32,
    /// `JointCount`; 0 until set (divergence D393).
    pub joint_count: i32,
}

/// One damped-least-squares solve's frame (`FIkDlsFrame`) with the chain's
/// poses.
struct Frame {
    pos: bool,
    ori: bool,
    pos_dim: i32,
    task_dim: i32,
    dof: i32,
    joint_iters: usize,
    end: usize,
    counter: i32,
    counter_floor: i32,
    alpha: f32,
    accum: f32,
    target_pos: [f32; 3],
    target_q: [f32; 4],
    held3: [f32; 3],
    /// `Rest9` followed by the Jacobian's cells.
    block: [f32; 9 + 144],
    j: SvdMatrix,
    jtj: SvdMatrix,
    jt: SvdMatrix,
    pinv: SvdMatrix,
    error: ([f32; 12], i32),
    achieved: ([f32; 12], i32),
    jte: ([f32; 12], i32),
    delta: ([f32; 12], i32),
    pull: ([f32; 12], i32),
    cur_rot: [[f32; 9]; MAX_JOINTS + 1],
    cur_pos: [[f32; 3]; MAX_JOINTS + 1],
    new_pos: [[f32; 3]; MAX_JOINTS + 1],
    new_rot: [[f32; 9]; MAX_JOINTS + 1],
}

impl Frame {
    fn new() -> Self {
        Self {
            pos: false,
            ori: false,
            pos_dim: 0,
            task_dim: 0,
            dof: 0,
            joint_iters: 0,
            end: 0,
            counter: 0,
            counter_floor: 0,
            alpha: 0.0,
            accum: 0.0,
            target_pos: [0.0; 3],
            target_q: [0.0; 4],
            held3: [0.0; 3],
            block: [0.0; 9 + 144],
            j: SvdMatrix::default(),
            jtj: SvdMatrix::default(),
            jt: SvdMatrix::default(),
            pinv: SvdMatrix::default(),
            error: ([0.0; 12], 0),
            achieved: ([0.0; 12], 0),
            jte: ([0.0; 12], 0),
            delta: ([0.0; 12], 0),
            pull: ([0.0; 12], 0),
            cur_rot: [[0.0; 9]; MAX_JOINTS + 1],
            cur_pos: [[0.0; 3]; MAX_JOINTS + 1],
            new_pos: [[0.0; 3]; MAX_JOINTS + 1],
            new_rot: [[0.0; 9]; MAX_JOINTS + 1],
        }
    }
}

fn bone_arm(relative: &[u32; 23]) -> [f32; 3] {
    [
        f32::from_bits(relative[1]),
        f32::from_bits(relative[2]),
        f32::from_bits(relative[3]),
    ]
}

impl IkEvent {
    /// The targets the solvers read (the caller's floats in the
    /// reference).
    pub fn set_targets(&mut self, pos: [f32; 3], orient: [f32; 9]) {
        self.target_pos = pos;
        self.target_orient = orient;
    }

    fn joint(&self, index: usize) -> Instance {
        self.joint_ids.get(index).copied().unwrap_or(INSTANCE_NONE)
    }

    fn locked(&self, index: usize) -> bool {
        self.locks.get(index).is_some_and(|&l| l != 0)
    }

    fn count(&self) -> usize {
        usize::try_from(self.joint_count).unwrap_or(0)
    }

    /// Writes the floats `GetJointParams` answered at word `at` of the
    /// quaternions (a loose joint's seven run into the next joint's, and
    /// past the end are dropped: divergence D393).
    fn put_quat_words(&mut self, at: usize, words: &[f32]) {
        for (k, &w) in words.iter().enumerate() {
            if let Some(q) = self.quats.get_mut(at + k) {
                *q = w;
            }
        }
    }

    /// `StartAim` (`AllocAimEvent`) after the node is linked: the chain
    /// from the end effector up to (not including) the root, each joint
    /// registered with the animation system (a blocked one stops the aim);
    /// then the event, its joint count (at most [`MAX_JOINTS`]), and per
    /// joint a typed channel played with the aim's header, the bind
    /// rotation and the lock byte. `Err` for the reference's -1 (the
    /// event made but failed, when the joint count is too large).
    ///
    /// # Errors
    ///
    /// `Err(None)` when a joint is blocked (no event), `Err(Some(event))`
    /// when the joint count is refused (the event, stopped, stays on the
    /// list).
    pub fn start(
        desc: &AimDesc,
        bind: Instance,
        play_extra: u32,
        owner: u32,
        services: &mut dyn DeformServices,
    ) -> Result<Self, Option<Box<Self>>> {
        let mut chain = Vec::new();
        let mut sphere_axis = false;
        let mut cursor = desc.end;
        if cursor != desc.root {
            loop {
                if services.has_handle(cursor) != 0 {
                    let mut walk = desc.end;
                    while walk != cursor {
                        services.has_any_handle(walk);
                        walk = services.instance_parent(walk);
                    }
                    services.dump(
                        dump::WARNING,
                        file::DEFORMABLE_OBJECT,
                        0x44D,
                        "deform::start_aim(): instance blocked -- exiting",
                    );
                    return Err(None);
                }
                services.register_handle(cursor);
                chain.push(cursor);
                let parent = services.instance_parent(cursor);
                let relative = services.instance_relative(cursor);
                if relative[0] == 4 {
                    sphere_axis = true;
                    let axis = [
                        f32::from_bits(relative[4]),
                        f32::from_bits(relative[5]),
                        f32::from_bits(relative[6]),
                    ];
                    let (x, y, z) = (f64::from(axis[0]), f64::from(axis[1]), f64::from(axis[2]));
                    if !((x * x + y * y + z * z).sqrt() > 1.0e-5) {
                        sphere_axis = false;
                    }
                }
                cursor = parent;
                if parent == desc.root {
                    break;
                }
            }
        }
        let count = chain.len();
        let joint_ids: Vec<Instance> = chain.iter().rev().copied().collect();
        let mut event = Self {
            joint_ids: joint_ids.clone(),
            bind_mats: vec![[0.0; 9]; count],
            start_joint: desc.root,
            end_joint: desc.end,
            channels: vec![0; count],
            quats: vec![0.0; count * 4],
            target_pos: desc.target_pos,
            target_orient: desc.target_orient,
            flags28: desc.flags,
            locks: vec![0; count],
            lock_count: i32::try_from(desc.locks.len()).unwrap_or(i32::MAX),
            flags34: 0,
            damping: desc.damping,
            tick: 0,
            joint_count: 0,
        };
        if count > MAX_JOINTS {
            services.dump(
                dump::WARNING,
                file::DEFORM_IK,
                0x15,
                &format!("too many joints specified for ik event. {count} / {MAX_JOINTS}"),
            );
            event.channels.clear();
            event.stop(services);
            return Err(Some(Box::new(event)));
        }
        event.joint_count = i32::try_from(count).unwrap_or(0);
        let blob = [
            0,
            (desc.flags >> 1) & 0x80,
            1.0f32.to_bits(),
            play_extra,
            1.0f32.to_bits(),
            1.0f32.to_bits(),
            0,
        ];
        for (index, &joint) in joint_ids.iter().enumerate() {
            let key = services.create_typed_channel(owner, joint, bind);
            event.channels[index] = key;
            services.play_channel(key, &blob);
            let relative = services.instance_relative(joint);
            let limits = JointLimits::from_relative(&relative);
            let mid = |l: [f32; 2]| f((f64::from(l[1]) + f64::from(l[0])) * 0.5);
            event.bind_mats[index] =
                euler_rebuild(mid(limits.yaw), mid(limits.pitch), mid(limits.roll));
            let mut params = [0.0f32; 7];
            for (k, w) in services.joint_params(joint, 0).iter().take(7).enumerate() {
                params[k] = *w;
            }
            event.bind_mats[index] = quat_to_matrix(&[params[0], params[1], params[2], params[3]]);
            if desc.locks.contains(&joint) {
                event.locks[index] = 1;
            }
        }
        event.flags34 = (event.flags34 & !2) | (u8::from(sphere_axis) << 1);
        Ok(event)
    }

    /// `StopAimNode`: the stop bit, then `HasAnyHandle` on every joint
    /// from the end effector up to the root.
    pub fn stop(&mut self, services: &mut dyn DeformServices) {
        self.flags34 |= 1;
        let mut cursor = self.end_joint;
        while cursor != self.start_joint {
            services.has_any_handle(cursor);
            cursor = services.instance_parent(cursor);
        }
    }

    /// The destructor: every channel destroyed.
    pub fn destroy(&mut self, services: &mut dyn DeformServices) {
        for index in 0..self.count() {
            if let Some(&key) = self.channels.get(index) {
                services.destroy_channel(key);
            }
        }
        self.joint_count = 0;
    }

    /// `IkApply`, the channel owner's callback for the channel `matching`:
    /// a stopped event answers the joint's own parameters; otherwise the
    /// first tick of a round (unless the bind descriptor's skip flag is
    /// set) solves, and the channel's joint gets its solved quaternion.
    /// `out` receives the words; the result is the reference's.
    pub fn apply(
        &mut self,
        out: &mut [f32],
        matching: u32,
        skip: bool,
        gate_joint: Instance,
        services: &mut dyn DeformServices,
    ) -> i32 {
        let count = self.count();
        if self.flags34 & 1 != 0 {
            for index in 0..count {
                if self.channels.get(index) == Some(&matching) {
                    let words = services.joint_params(self.joint(index), 0);
                    for (k, w) in words.iter().enumerate() {
                        if let Some(o) = out.get_mut(k) {
                            *o = *w;
                        }
                    }
                    return 1;
                }
            }
            return 1;
        }
        let old_tick = self.tick;
        self.tick = old_tick + 1;
        if old_tick == 0 && !skip {
            let relative = services.instance_relative(gate_joint);
            if self.joint_count == 1 {
                self.solve_single(gate_joint, &JointLimits::from_relative(&relative), services);
            } else if self.flags34 & 2 != 0 {
                self.solve_dls(true, services);
            } else {
                self.solve_dls(false, services);
            }
        }
        if self.tick == self.joint_count {
            self.tick = 0;
        }
        for index in 0..count {
            if self.channels.get(index) == Some(&matching) {
                for k in 0..4 {
                    if let (Some(o), Some(q)) = (out.get_mut(k), self.quats.get(index * 4 + k)) {
                        *o = *q;
                    }
                }
                return 0;
            }
        }
        0
    }

    /// The single-joint body of `IkApply`: the target direction (or
    /// orientation, bit 6) in the parent's frame, looked at along the
    /// flags' axis, held to the joint's Euler limits unless bit 7.
    fn solve_single(
        &mut self,
        joint: Instance,
        limits: &JointLimits,
        services: &mut dyn DeformServices,
    ) {
        let parent = services.instance_parent(joint);
        let local = services.instance_orientation(parent);
        let trans = transpose9(&local);
        let result = if self.flags28 & 0x40 != 0 {
            mul_matrix9(&trans, &self.target_orient)
        } else {
            let p = services.instance_position(joint);
            let target = self.target_pos;
            let src = if self.flags28 & 1 != 0 {
                [
                    f(f64::from(target[0]) - f64::from(p[0])),
                    f(f64::from(target[1]) - f64::from(p[1])),
                    f(f64::from(target[2]) - f64::from(p[2])),
                ]
            } else {
                target
            };
            let (x, y, z) = (f64::from(src[0]), f64::from(src[1]), f64::from(src[2]));
            let inv = 1.0 / (y * y + z * z + x * x).sqrt();
            let dir = [f(x * inv), f(y * inv), f(z * inv)];
            #[allow(clippy::cast_possible_truncation)]
            let look = look_at(&dir, &local, self.flags28 as u8);
            mul_matrix9(&trans, &look)
        };
        let result = if self.flags28 & 0x80 == 0 {
            euler_clamp(limits, &result)
        } else {
            result
        };
        let q = matrix_to_quat(&result);
        self.put_quat_words(0, &q);
    }

    /// `IkSolveDls` (`alt` false: the chain end's link has no lever arm
    /// and a locked end joint frees its columns) and `IkSolveDlsAlt`
    /// (`alt`: every joint's link has one).
    #[allow(clippy::too_many_lines)]
    fn solve_dls(&mut self, alt: bool, services: &mut dyn DeformServices) {
        let mut fr = Frame::new();
        let flags = self.flags28;
        fr.pos = (flags >> 5) & 1 != 0;
        fr.ori = (flags >> 6) & 1 != 0;
        let count = self.joint_count;
        if alt {
            fr.dof = count * 3;
        } else {
            fr.dof = (if fr.ori { count } else { count - 1 }) * 3;
        }
        fr.pos_dim = if fr.pos { 3 } else { 0 };
        fr.task_dim = fr.pos_dim + if fr.ori { 3 } else { 0 };
        if fr.ori || (alt && fr.pos) {
            fr.dof += self.lock_count * -3;
        } else if fr.pos {
            let mut locked = self.lock_count;
            if locked > 0 && self.locked(self.count().wrapping_sub(1)) {
                locked -= 1;
            }
            fr.dof += locked * -3;
        }

        // IkDlsReadStart.
        let start_pos = services.instance_position(self.start_joint);
        fr.new_pos[0] = start_pos;
        fr.cur_pos[0] = start_pos;
        fr.new_rot[0] = services.instance_orientation(self.start_joint);
        fr.cur_rot[0] = fr.new_rot[0];
        let n = self.count().min(MAX_JOINTS);
        for joint in 0..n {
            let id = self.joint(joint);
            let words = services.joint_params(id, 0);
            self.put_quat_words(joint * 4, &words);
            if !alt {
                let _ = services.instance_relative(id);
            }
            fr.cur_pos[joint + 1] = services.instance_position(id);
            fr.cur_rot[joint + 1] = services.instance_orientation(id);
        }

        fr.end = n;
        if fr.pos {
            fr.target_pos = self.target_pos;
        }
        let target_mat = if fr.ori { self.target_orient } else { [0.0; 9] };
        let mut need = false;
        if fr.pos {
            let e = fr.cur_pos[fr.end];
            let t = fr.target_pos;
            if alt {
                let d: [f64; 3] =
                    core::array::from_fn(|k| f64::from(f(f64::from(t[k]) - f64::from(e[k]))));
                need = ((d[1] * d[1] + d[2] * d[2]) + d[0] * d[0]).sqrt() > 0.001;
            } else {
                let d: [f64; 3] = core::array::from_fn(|k| f64::from(t[k]) - f64::from(e[k]));
                need = ((d[2] * d[2] + d[1] * d[1]) + d[0] * d[0]).sqrt() > 0.001;
            }
        }
        fr.target_q = matrix_to_quat(&target_mat);
        if !need {
            if !fr.ori {
                return;
            }
            // IkDlsOriOff.
            let cur_q = matrix_to_quat(&fr.cur_rot[fr.end]);
            let l = quat_log(&cur_q, &fr.target_q);
            let (x, y, z) = (f64::from(l[0]), f64::from(l[1]), f64::from(l[2]));
            if !(((y * y + z * z) + x * x).sqrt() > 0.01) {
                return;
            }
        }

        // IkDlsSetup.
        fr.j.rows = fr.task_dim;
        fr.j.cols = fr.dof;
        fr.jt.rows = fr.dof;
        fr.jt.cols = fr.task_dim;
        fr.jtj.rows = fr.task_dim + fr.dof;
        fr.jtj.cols = fr.dof;
        fr.delta.1 = fr.dof;
        fr.achieved.1 = fr.task_dim;
        fr.counter = 1;
        fr.counter_floor = 1;
        fr.alpha = 1.0;
        fr.accum = 0.0;
        fr.joint_iters = if alt || fr.ori {
            n
        } else {
            n.saturating_sub(1)
        };

        loop {
            let limit = if alt || fr.ori { n + 1 } else { n };
            let mut col = 0i32;
            for link in 1..limit {
                if fr.pos {
                    let arm = if !alt && link == n {
                        [0.0; 3]
                    } else {
                        let relative = services.instance_relative(self.joint(link));
                        transform_matrix9(&fr.cur_rot[link], &bone_arm(&relative))
                    };
                    if self.locked(link - 1) {
                        add_skew(&mut fr, col, arm);
                    } else {
                        write_skew(&mut fr, col, arm);
                    }
                }
                if fr.ori && !self.locked(link - 1) {
                    let first = col + 12 * fr.pos_dim;
                    write_identity(&mut fr, first);
                }
                if !self.locked(link - 1) {
                    col += 3;
                }
            }
            if !self.round(&mut fr, services) {
                break;
            }
        }
        self.write_quats(&fr);
    }

    /// `IkDlsRound`: `J^T J + lambda^2 I` inverted, then trial steps of
    /// halving size until the residual stops falling; the step's share
    /// accumulates. Whether another round runs.
    #[allow(clippy::too_many_lines)]
    fn round(&mut self, fr: &mut Frame, services: &mut dyn DeformServices) -> bool {
        // The Jacobian as SvdMatrix cells.
        for r in 0..12i32 {
            for c in 0..12i32 {
                let value = fr.block[(9 + r * 12 + c) as usize];
                fr.j.set(r, c, value);
            }
        }
        let j = fr.j.clone();
        transpose(&j, &mut fr.jt);
        let jt = fr.jt.clone();
        mul_ab(&jt, &j, &mut fr.jtj);
        // IkDlsDamp.
        let lam2 = f(f64::from(self.damping) * f64::from(self.damping));
        for r in 0..fr.jtj.rows {
            for c in 0..fr.jtj.cols {
                if r == c {
                    let v = f(f64::from(lam2) + f64::from(fr.jtj.at(r, c)));
                    fr.jtj.set(r, c, v);
                }
            }
        }
        fr.pinv.rows = fr.jtj.cols;
        fr.pinv.cols = fr.jtj.rows;
        let jtj = fr.jtj.clone();
        if !pseudo_inverse(&jtj, &mut fr.pinv).converged {
            services.dump(
                dump::WARNING,
                file::MAT,
                0x2E8,
                "svdcmp: didnt approach a finite soln.",
            );
        }

        let pull = (self.flags28 >> 7) & 1 == 0;
        fr.pull.1 = fr.dof;
        if pull {
            self.fill_rest_pull(fr);
        }

        let mut step = 2.0f32;
        let mut prev_err = f32::MAX;
        let mut err_delta;
        loop {
            step = f(f64::from(step) * 0.5);
            let end = fr.cur_pos[fr.end];
            let diff: [f32; 3] =
                core::array::from_fn(|k| f(f64::from(fr.target_pos[k]) - f64::from(end[k])));
            let divisor = if fr.counter >= 1 {
                fr.counter
            } else {
                fr.counter_floor
            };
            let scale = 1.0 / f64::from(divisor);
            fr.counter -= 1;
            fr.error.1 = fr.task_dim;
            if fr.pos {
                fr.error.0[0] = f(f64::from(diff[0]) * scale);
                fr.error.0[1] = f(f64::from(diff[1]) * scale);
                fr.error.0[2] = f(scale * f64::from(diff[2]));
            }
            if fr.ori {
                let cur_q = matrix_to_quat(&fr.cur_rot[fr.end]);
                let l = quat_log(&cur_q, &fr.target_q);
                put_vec(&mut fr.error.0, fr.pos_dim, l);
            }
            fr.jte.1 = fr.error.1;
            let jt = fr.jt.clone();
            let error = fr.error.0;
            mul_vec(&jt, &error, &mut fr.jte.0, &mut fr.jte.1);
            if pull {
                let gain = f(f64::from(self.damping) * 0.25);
                for i in 0..usize::try_from(fr.jte.1).unwrap_or(0).min(12) {
                    fr.jte.0[i] =
                        f(f64::from(gain) * f64::from(fr.pull.0[i]) + f64::from(fr.jte.0[i]));
                }
            }
            let pinv = fr.pinv.clone();
            let jte = fr.jte.0;
            mul_vec(&pinv, &jte, &mut fr.delta.0, &mut fr.delta.1);

            let old_end = end;
            self.step_joints(fr, services);
            if fr.pos {
                let new_end = fr.new_pos[fr.end];
                for k in 0..3 {
                    fr.achieved.0[k] = f(f64::from(new_end[k]) - f64::from(old_end[k]));
                }
            }
            if fr.ori {
                let new_q = matrix_to_quat(&fr.new_rot[fr.end]);
                let l = quat_log(&new_q, &fr.target_q);
                put_vec(&mut fr.achieved.0, fr.pos_dim, l);
            }
            // IkDlsResidual.
            let mut sum = 0.0f64;
            for i in 0..usize::try_from(fr.task_dim).unwrap_or(0).min(12) {
                let d = f64::from(fr.error.0[i]) - f64::from(fr.achieved.0[i]);
                sum += d * d;
            }
            err_delta = f(sum - f64::from(prev_err));
            prev_err = f(sum);
            if !(sum > 0.01) {
                break;
            }
            if !(f64::from(err_delta) < -1.0e-5) {
                break;
            }
        }

        fr.accum = f(f64::from(step) * f64::from(fr.alpha) + f64::from(fr.accum));
        let remain = 1.0 - f64::from(fr.accum);
        let remain_f = f(remain);
        if remain < f64::from(fr.alpha) {
            fr.alpha = remain_f;
        }
        if (f64::from(fr.accum) - 1.0).abs() <= 0.01 {
            return false;
        }
        !(err_delta >= 0.0)
    }

    /// `IkDlsFillRestPull`: for each unlocked joint, the log of the step
    /// from its current rotation to its parent's rotation times its bind
    /// rotation.
    fn fill_rest_pull(&self, fr: &mut Frame) {
        let mut at = 0usize;
        for joint in 0..fr.joint_iters {
            if self.locked(joint) {
                continue;
            }
            let bind = self.bind_mats.get(joint).copied().unwrap_or_default();
            let rest = mul_matrix9(&fr.cur_rot[joint], &bind);
            fr.block[..9].copy_from_slice(&rest);
            let cur_q = matrix_to_quat(&fr.cur_rot[joint + 1]);
            let rest_q = matrix_to_quat(&rest);
            let l = quat_log(&cur_q, &rest_q);
            for (k, v) in l.iter().enumerate() {
                if let Some(cell) = fr.pull.0.get_mut(at + k) {
                    *cell = *v;
                }
            }
            at += 3;
        }
    }

    /// `IkDlsStepJoints`: each joint rotated by half its step (a locked
    /// joint repeats the last unlocked one's) and the chain's trial
    /// positions rebuilt.
    fn step_joints(&self, fr: &mut Frame, services: &mut dyn DeformServices) {
        let mut at = 0usize;
        for joint in 0..fr.joint_iters {
            let relative = services.instance_relative(self.joint(joint));
            if !self.locked(joint) {
                fr.held3 = [
                    fr.delta.0.get(at).copied().unwrap_or(0.0),
                    fr.delta.0.get(at + 1).copied().unwrap_or(0.0),
                    fr.delta.0.get(at + 2).copied().unwrap_or(0.0),
                ];
                at += 3;
            }
            let axis = fr.held3.map(|h| f(f64::from(h) * 0.5));
            let step_q = quat_exp(&axis);
            let step_m = quat_to_matrix(&step_q);
            fr.new_rot[joint + 1] = mul_matrix9(&step_m, &fr.cur_rot[joint + 1]);
            let arm = transform_matrix9(&fr.new_rot[joint], &bone_arm(&relative));
            for k in 0..3 {
                fr.new_pos[joint + 1][k] = f(f64::from(arm[k]) + f64::from(fr.new_pos[joint][k]));
            }
        }
    }

    /// `IkDlsWriteQuats`: each unlocked joint's local rotation,
    /// `transpose(NewRot[i]) * NewRot[i + 1]`, as a quaternion.
    fn write_quats(&mut self, fr: &Frame) {
        for joint in 0..fr.joint_iters {
            if self.locked(joint) {
                continue;
            }
            let parent_t = transpose9(&fr.new_rot[joint]);
            let local = mul_matrix9(&parent_t, &fr.new_rot[joint + 1]);
            let q = matrix_to_quat(&local);
            self.put_quat_words(joint * 4, &q);
        }
    }
}

fn put_vec(cells: &mut [f32; 12], at: i32, v: [f32; 3]) {
    for (k, value) in v.iter().enumerate() {
        if let Some(c) = usize::try_from(at).ok().and_then(|a| cells.get_mut(a + k)) {
            *c = *value;
        }
    }
}

fn set_cell(fr: &mut Frame, index: i32, value: f32) {
    if let Some(c) = usize::try_from(9 + index)
        .ok()
        .and_then(|i| fr.block.get_mut(i))
    {
        *c = value;
    }
}

fn cell(fr: &Frame, index: i32) -> f32 {
    usize::try_from(9 + index)
        .ok()
        .and_then(|i| fr.block.get(i))
        .copied()
        .unwrap_or(0.0)
}

/// `IkDlsWriteSkew`.
fn write_skew(fr: &mut Frame, col: i32, a: [f32; 3]) {
    set_cell(fr, col, 0.0);
    set_cell(fr, col + 1, a[2]);
    set_cell(fr, col + 2, -a[1]);
    set_cell(fr, col + 12, -a[2]);
    set_cell(fr, col + 13, 0.0);
    set_cell(fr, col + 14, a[0]);
    set_cell(fr, col + 24, a[1]);
    set_cell(fr, col + 25, -a[0]);
    set_cell(fr, col + 26, 0.0);
}

/// `IkDlsAddSkew`: a locked joint's terms added into the previous three
/// columns.
fn add_skew(fr: &mut Frame, col: i32, a: [f32; 3]) {
    let add = |x: f32, y: f32| f(f64::from(x) + f64::from(y));
    let sub = |x: f32, y: f32| f(f64::from(x) - f64::from(y));
    let v = add(a[2], cell(fr, col - 2));
    set_cell(fr, col - 2, v);
    let v = sub(cell(fr, col - 1), a[1]);
    set_cell(fr, col - 1, v);
    let v = sub(cell(fr, col + 9), a[2]);
    set_cell(fr, col + 9, v);
    let v = add(a[0], cell(fr, col + 11));
    set_cell(fr, col + 11, v);
    let v = add(a[1], cell(fr, col + 21));
    set_cell(fr, col + 21, v);
    let v = sub(cell(fr, col + 22), a[0]);
    set_cell(fr, col + 22, v);
}

/// `IkDlsWriteIdentity`.
fn write_identity(fr: &mut Frame, first: i32) {
    for (offset, value) in [
        (0, 1.0),
        (1, 0.0),
        (2, 0.0),
        (12, 0.0),
        (13, 1.0),
        (14, 0.0),
        (24, 0.0),
        (25, 0.0),
        (26, 1.0),
    ] {
        set_cell(fr, first + offset, value);
    }
}

/// The words of a vector at `at` of a joint payload.
#[must_use]
pub fn relative_vec(relative: &[u32; 23], at: usize) -> [f32; 3] {
    let floats: Vec<f32> = relative.iter().map(|w| f32::from_bits(*w)).collect();
    vec3(&floats, at)
}
