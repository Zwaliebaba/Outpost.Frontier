//! `FDeformInstance` and the `IDeformable` slots: creation from part
//! descriptions (`ConnectInstanceParts`: the skeleton, the holder, the
//! levels and the hardpoint connections), the list of playing animations
//! and aim events, the pose calls that steer the animations' overlays,
//! levels, the draw and its level result, bounds, statistics and the
//! hardpoint walk; and the module's state ([`Deformable`]).

use neuron_core::hash::{eq_ignore_case, name_crc32};
use neuron_core::mesh_data::dfm;
use neuron_core::msvcrt::{atoi, wcstoul};
use neuron_core::vfs::{FileSystem, FsDesc};

use crate::render::material::MaterialId;
use crate::render::mesh::MeshCamera;

use super::ik::{AimDesc, IkEvent};
use super::math::{
    axis_angle_to_quat, mul_matrix9, quat_to_matrix, slerp4, transform_matrix9, transform_xform12,
};
use super::mesh::{DeformMesh, MeshArchetype, MeshCache, MeshId, VMeshWrapper};
use super::part::DeformPart;
use super::services::DeformServices;
use super::skeleton::{LodHolder, Skeleton};
use super::{INSTANCE_NONE, Instance, dump, file};

#[allow(clippy::cast_possible_truncation)]
fn f(x: f64) -> f32 {
    x as f32
}

fn add(a: f32, b: f32) -> f32 {
    f(f64::from(a) + f64::from(b))
}

/// The visit of a hardpoint walk: the services, the bone's instance and the
/// hardpoint's name.
pub type HardpointVisit<'a> = dyn FnMut(&mut dyn DeformServices, Instance, &[u8]) + 'a;

/// `GR_OK`.
pub const GR_OK: i32 = 0;
/// `GR_GENERIC`.
pub const GR_GENERIC: i32 = -1;
/// `GR_INVALID_PARMS`.
pub const GR_INVALID_PARMS: i32 = -2;

/// `G_RandToUnit`.
const RAND_TO_UNIT: f32 = 3.051_851e-5;

/// One level of a part description (`FDeformLodDesc`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LodDesc {
    /// The part file when the description has no path (level 0 only).
    pub name: Vec<u8>,
    /// Written back: the `Fractions` value of each level loaded.
    pub fraction: f32,
}

/// A part description (`FDeformPartDesc`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PartDesc {
    /// In: the most levels to load; out: how many were.
    pub lod_count: i32,
    /// The levels.
    pub lods: Vec<LodDesc>,
    /// The part file.
    pub path: Option<Vec<u8>>,
    /// Bit 1: skip level 0; bit 0: stop after level 0.
    pub flags: u32,
    /// Levels below this compose their bones (`Flag70`).
    pub lod_limit: u32,
}

/// What a list node holds: an animation playing on the instance (its
/// playback handle) or an aim event.
#[derive(Clone, Debug, PartialEq)]
pub enum ListPayload {
    /// `Active == 0`: a playback handle (-1 once torn down).
    Anim(i32),
    /// `Active != 0`: the aim event (`None` once torn down).
    Aim(Option<Box<IkEvent>>),
}

/// A list node (`FIkListNode`).
#[derive(Clone, Debug, PartialEq)]
pub struct ListNode {
    /// The node's handle for `StopAim` (the reference's node pointer).
    pub id: u32,
    /// `Extra8`: the height the animation was started at.
    pub y: f32,
    /// The payload.
    pub payload: ListPayload,
}

/// `FDeformInstance`.
#[derive(Clone, Debug, PartialEq)]
pub struct DeformInstance {
    /// The parts.
    pub parts: Vec<DeformPart>,
    /// The playing animations and aim events, oldest first.
    pub list: Vec<ListNode>,
    next_id: u32,
    /// The caller's channel sink (`UserAt18`); 0 for none.
    pub sink: u32,
    /// The caller's script (`UserAt1C`).
    pub script: u32,
    /// The height (`Offset20`).
    pub offset20: f32,
    /// The root's resting height less the height (`Offset24`).
    pub offset24: f32,
    /// The time left until the skins are marked stale (`RateCookie`).
    pub rate_cookie: f32,
}

impl DeformInstance {
    /// `ConstructInstanceBody`: the cookie a random share of the rate.
    fn new(rate: f32, services: &mut dyn DeformServices) -> Self {
        let rate_cookie = if rate > 0.0 {
            let r = services.rand();
            #[allow(clippy::cast_precision_loss)]
            let r = r as f32;
            f(f64::from(r) * f64::from(rate) * f64::from(RAND_TO_UNIT))
        } else {
            rate
        };
        Self {
            parts: Vec::new(),
            list: Vec::new(),
            next_id: 1,
            sink: 0,
            script: 0,
            offset20: 0.0,
            offset24: 0.0,
            rate_cookie,
        }
    }

    /// An instance over parts made elsewhere (no part files), for callers
    /// that bind holders and meshes themselves.
    #[must_use]
    pub fn with_parts(parts: Vec<DeformPart>, sink: u32, script: u32) -> Self {
        Self {
            parts,
            list: Vec::new(),
            next_id: 1,
            sink,
            script,
            offset20: 0.0,
            offset24: 0.0,
            rate_cookie: 0.0,
        }
    }

    fn root_bone(&self) -> Option<&super::skeleton::BoneRuntime> {
        self.parts.first()?.lod.as_ref()?.bones.first()
    }

    /// The first part's root bone instance (`FirstBoneInstance`): -1
    /// without parts.
    #[must_use]
    pub fn first_bone(&self) -> Instance {
        self.root_bone().map_or(INSTANCE_NONE, |b| b.instance)
    }

    fn root_local(&self) -> [f32; 12] {
        self.root_bone().map_or([0.0; 12], |b| b.local)
    }

    fn push_node(&mut self, y: f32, payload: ListPayload) -> u32 {
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1);
        self.list.push(ListNode { id, y, payload });
        id
    }

    fn teardown_payload(payload: &mut ListPayload, services: &mut dyn DeformServices) {
        match payload {
            ListPayload::Aim(event) => {
                if let Some(mut e) = event.take() {
                    e.destroy(services);
                }
            }
            ListPayload::Anim(handle) => {
                services.destroy_script(*handle);
                *handle = -1;
            }
        }
    }

    /// `DestroyAllList` (slot 9).
    pub fn destroy_all(&mut self, services: &mut dyn DeformServices) {
        for mut node in core::mem::take(&mut self.list) {
            Self::teardown_payload(&mut node.payload, services);
        }
    }

    /// `DestroyListByHandle` (slot 10): the first animation with the
    /// handle.
    pub fn destroy_handle(&mut self, handle: i32, services: &mut dyn DeformServices) {
        if let Some(at) = self
            .list
            .iter()
            .position(|n| matches!(n.payload, ListPayload::Anim(h) if h == handle))
        {
            let mut node = self.list.remove(at);
            Self::teardown_payload(&mut node.payload, services);
        }
    }

    /// `SweepExpiredList`: stopped aim events out of the mixer and
    /// animations the animation system filters are dropped; an empty list
    /// takes the root's height as the resting height.
    pub fn sweep(&mut self, services: &mut dyn DeformServices) {
        let mut index = 0;
        while index < self.list.len() {
            let drop = match &self.list[index].payload {
                ListPayload::Aim(event) => event.as_ref().is_some_and(|e| {
                    e.flags34 & 1 != 0
                        && e.channels
                            .first()
                            .is_none_or(|&key| services.channel_in_mixer(key) == 0)
                }),
                ListPayload::Anim(handle) => services.filter_objects(*handle) != 0,
            };
            if drop {
                let mut node = self.list.remove(index);
                Self::teardown_payload(&mut node.payload, services);
            } else {
                index += 1;
            }
        }
        if self.list.is_empty() {
            self.offset24 = f(f64::from(self.root_local()[10]) - f64::from(self.offset20));
        }
    }

    /// `PlayInstanceAnim` (slot 7): the script played on the root bone
    /// with the instance as its sink; with a time, the root data's motion;
    /// the start transform (the root's place at the animation's height,
    /// turned by the twist or the root's own heading) evaluated; the
    /// animation listed. The handle, -1 on a failure.
    #[allow(clippy::too_many_arguments)]
    pub fn play(
        &mut self,
        name: u32,
        extra: u32,
        time: f32,
        d: u32,
        scale: f32,
        flag_f: u32,
        g: u32,
        twist: f32,
        script: u32,
        services: &mut dyn DeformServices,
    ) -> i32 {
        self.sweep(services);
        let bone = self.first_bone();
        let script = if script == 0 { self.script } else { script };
        let handle = services.play_script(name, bone, extra, script);
        if handle == -1 {
            return -1;
        }
        let local = self.root_local();
        let bone_pos = [local[9], local[10], local[11]];
        let mut vec = [0.0f32; 3];
        if time > 0.0
            && let Some((touched, out)) = services.root_eval(handle, vec, g, time, scale, d, flag_f)
        {
            vec = out;
            if touched != 0 {
                vec[2] = 0.0;
                vec[0] = 0.0;
            }
        }
        let y = match services.named_duration(name, extra) {
            Some(duration) => add(duration, self.offset20),
            None => add(self.offset24, self.offset20),
        };
        let pos = [
            add(bone_pos[0], vec[0]),
            add(vec[1], y),
            add(bone_pos[2], vec[2]),
        ];
        let up = [0.0, 1.0, 0.0];
        let quat = if twist < 0.0 {
            let angle = f(f64::from(local[2]).atan2(f64::from(local[8])));
            axis_angle_to_quat(&up, angle)
        } else {
            let half = f64::from(twist) * 0.5;
            let s = f(half.sin() / 1.0f64.sqrt());
            [f(half.cos()), up[0] * s, up[1] * s, up[2] * s]
        };
        let m = quat_to_matrix(&quat);
        let mut xform = [0.0f32; 12];
        xform[..9].copy_from_slice(&m);
        xform[9..].copy_from_slice(&pos);
        if services.eval_playback(handle, g, time, scale, d, flag_f, &xform) < 0 {
            services.destroy_script(handle);
            return -1;
        }
        self.push_node(y, ListPayload::Anim(handle));
        handle
    }

    /// `PlayInstanceAnimChain` (slot 8): [`Self::play`], then the chain
    /// from `joint` up to the root bone sampled at `time` and walked back
    /// down to find where the animation puts the joint; the difference to
    /// where it is now becomes the playback's overlay offset.
    #[allow(clippy::too_many_arguments)]
    pub fn play_chain(
        &mut self,
        name: u32,
        extra: u32,
        time: f32,
        d: u32,
        scale: f32,
        flag_f: u32,
        g: u32,
        twist: f32,
        joint: Instance,
        services: &mut dyn DeformServices,
    ) -> i32 {
        let start = services.instance_position(joint);
        let handle = self.play(name, extra, time, d, scale, flag_f, g, twist, 0, services);
        let stop = self.first_bone();
        let mut relatives = Vec::new();
        let mut quats = Vec::new();
        let mut cursor = joint;
        while cursor != stop {
            let parent = services.instance_parent(cursor);
            relatives.push(services.instance_relative(cursor));
            let channel = services.find_object(handle, 0, cursor);
            let s = services.sample_channel(channel, time);
            let q = |w: &[f32]| core::array::from_fn(|k| w.get(k).copied().unwrap_or(0.0));
            quats.push(slerp4(&q(&s.a), &q(&s.b), s.alpha));
            cursor = parent;
        }
        let root_channel = services.find_object(handle, 1, stop);
        let s = services.sample_channel(root_channel, time);
        let w = |v: &[f32], i: usize| v.get(i).copied().unwrap_or(0.0);
        let pos_a = [w(&s.a, 0), w(&s.a, 1), w(&s.a, 2)];
        let pos_b = [w(&s.b, 0), w(&s.b, 1), w(&s.b, 2)];
        let diff: [f32; 3] = core::array::from_fn(|k| f(f64::from(pos_b[k]) - f64::from(pos_a[k])));
        let mut tail: [f32; 3] =
            core::array::from_fn(|k| f(f64::from(diff[k]) * f64::from(s.alpha)));
        let mut accum: [f32; 3] = core::array::from_fn(|k| add(tail[k], pos_a[k]));
        let qa = [w(&s.a, 3), w(&s.a, 4), w(&s.a, 5), w(&s.a, 6)];
        let qb = [w(&s.b, 3), w(&s.b, 4), w(&s.b, 5), w(&s.b, 6)];
        let root_quat = slerp4(&qa, &qb, s.alpha);
        let mut running = quat_to_matrix(&root_quat);
        for index in (0..quats.len()).rev() {
            let step = quat_to_matrix(&quats[index]);
            let rel = relatives[index].map(f32::from_bits);
            let mul = mul_matrix9(&running, &step);
            let rel_m: [f32; 9] = core::array::from_fn(|k| rel[7 + k]);
            let rot = mul_matrix9(&mul, &rel_m);
            let xf_a = transform_matrix9(&rot, &[rel[4], rel[5], rel[6]]);
            let xf_b = transform_matrix9(&running, &[rel[1], rel[2], rel[3]]);
            let sum_z = add(xf_b[2], accum[2]);
            let x = f(f64::from(xf_b[0]) + f64::from(accum[0]) - f64::from(xf_a[0]));
            let y = f(f64::from(xf_b[1]) + f64::from(accum[1]) - f64::from(xf_a[1]));
            let z = f(f64::from(sum_z) - f64::from(xf_a[2]));
            accum = [x, y, z];
            tail = accum;
            running = rot;
        }
        let local = self.root_local();
        let diff_z = f(f64::from(start[2]) - f64::from(local[11]));
        let delta = [
            f(f64::from(start[0]) - f64::from(local[9]) - f64::from(tail[0])),
            f(f64::from(start[1]) - f64::from(local[10]) - f64::from(tail[1])),
            f(f64::from(diff_z) - f64::from(tail[2])),
        ];
        services.add_overlay_pos(handle, &delta);
        handle
    }

    /// `OnKeys` (`IChannelSink` slot 0): the cookie forwarded to the
    /// caller's sink (the first animation's instance field that holds the
    /// channel); `None` without a sink.
    pub fn on_keys(&self, key: u32, services: &mut dyn DeformServices) -> Option<u32> {
        if self.sink == 0 {
            return None;
        }
        let mut cookie = 0;
        for node in &self.list {
            if let ListPayload::Anim(handle) = node.payload
                && services.has_object(handle, key) != 0
            {
                cookie = services.first_instance_field(handle).unwrap_or(0);
                break;
            }
        }
        Some(cookie)
    }

    /// `OnLoop` (`IChannelSink` slot 2): for the first animation that holds
    /// the channel, the transform's height and heading are reset (the
    /// rotation becomes the heading about y). Whether the caller's sink is
    /// called with it.
    pub fn on_loop(
        &self,
        key: u32,
        xform: &mut [f32; 12],
        services: &mut dyn DeformServices,
    ) -> bool {
        let mut found = None;
        for node in &self.list {
            if let ListPayload::Anim(handle) = node.payload
                && services.has_object(handle, key) != 0
            {
                found = Some(handle);
                break;
            }
        }
        let Some(handle) = found else {
            return false;
        };
        let height = match services.duration_if_named(handle) {
            Some(duration) => add(duration, self.offset20),
            None => add(self.offset24, self.offset20),
        };
        xform[10] = height;
        let half = f64::from(xform[2]).atan2(f64::from(xform[8])) * 0.5;
        let sin_scaled = f(half.sin() / 1.0f64.sqrt());
        let side = sin_scaled * 0.0;
        let quat = [f(half.cos()), side, sin_scaled, side];
        let m = quat_to_matrix(&quat);
        xform[..9].copy_from_slice(&m);
        self.sink != 0
    }

    /// `StartAim` (slot 11): the node handle, -1 on a failure.
    pub fn start_aim(
        &mut self,
        desc: &AimDesc,
        play_extra: u32,
        services: &mut dyn DeformServices,
    ) -> i64 {
        let bind = self.first_bone();
        let owner = self.next_id;
        match IkEvent::start(desc, bind, play_extra, owner, services) {
            Ok(event) => i64::from(self.push_node(0.0, ListPayload::Aim(Some(Box::new(event))))),
            Err(Some(event)) => {
                self.push_node(0.0, ListPayload::Aim(Some(event)));
                -1
            }
            Err(None) => -1,
        }
    }

    /// The aim event of a node.
    pub fn aim_mut(&mut self, node: u32) -> Option<&mut IkEvent> {
        self.list
            .iter_mut()
            .find(|n| n.id == node)
            .and_then(|n| match &mut n.payload {
                ListPayload::Aim(Some(e)) => Some(e.as_mut()),
                _ => None,
            })
    }

    /// `StopAim` (slot 12).
    pub fn stop_aim(&mut self, node: u32, services: &mut dyn DeformServices) {
        if let Some(event) = self.aim_mut(node) {
            event.stop(services);
        }
    }

    /// `SetLod` (slot 15): each part's level; a change starts the rate
    /// over; when the rate's time runs out every part's skin is marked
    /// stale.
    pub fn set_lod(&mut self, dt: f32, lods: &[i32], rate: f32) {
        let mut changed = false;
        for (part, &lod) in self.parts.iter_mut().zip(lods) {
            changed |= part.select_lod(lod);
        }
        if changed {
            self.rate_cookie = -1.0;
        }
        let remaining = f64::from(self.rate_cookie) - f64::from(dt);
        self.rate_cookie = f(remaining);
        if !(remaining < 0.0) && remaining != 0.0 {
            return;
        }
        self.rate_cookie = rate;
        for part in &mut self.parts {
            part.mark_skin_dirty();
        }
    }

    /// `DrawInstanceParts` (slot 16): each part classified by its sphere
    /// in view space and drawn when visible enough; the level result
    /// merged over the parts and, at 3, with the renderer's rank of the
    /// root instance. A part whose sphere is not answered keeps the radius
    /// and centre before it; the reference starts the radius from the
    /// camera pointer's bits, the port from 0 (divergence D397).
    pub fn draw(
        &mut self,
        camera: &mut dyn MeshCamera,
        render_arg: u32,
        flags: u32,
        cache: &mut MeshCache,
        services: &mut dyn DeformServices,
    ) -> i32 {
        let mut out = 0;
        if self.parts.is_empty() {
            return out;
        }
        let mut radius = 0.0f32;
        let mut centre = [0.0f32; 3];
        for index in 0..self.parts.len() {
            if let Some((r, c)) = self.parts[index].bound_sphere(cache) {
                radius = r;
                centre = c;
            }
            let view = camera.view_transform();
            let probe = transform_xform12(&view, &centre);
            let rank = camera.classify_sphere(probe, radius);
            if rank > 2 {
                self.parts[index].draw_live(camera, cache, services);
            }
            if index == 0 {
                out = rank;
            } else if out != rank {
                if out < rank {
                    out = rank;
                }
                if out >= 3 {
                    out = 3;
                }
            }
        }
        if out < 3 {
            return out;
        }
        let rank = services.render_instance(self.first_bone(), flags | 0x108, render_arg);
        if out != rank {
            if out < rank {
                out = rank;
            }
            if out >= 3 {
                out = 3;
            }
        }
        out
    }

    /// `SetPosePos` (slot 19).
    pub fn set_pose_pos(&mut self, pos: &[f32; 3], services: &mut dyn DeformServices) {
        self.sweep(services);
        let mut vec = *pos;
        let mut hit = false;
        for index in 0..self.list.len() {
            let ListPayload::Anim(handle) = self.list[index].payload else {
                continue;
            };
            let sum = match services.duration_if_named(handle) {
                Some(duration) => {
                    let (_pos, extra) = services.touch_mapped(handle);
                    f64::from(duration) + f64::from(extra[1])
                }
                None => f64::from(self.offset24) + 0.0,
            };
            let y = f(sum + f64::from(self.offset20));
            vec[1] = y;
            services.set_overlay_pos(handle, &[pos[0], y, pos[2]]);
            hit = true;
        }
        if !hit {
            vec[1] = add(self.offset24, self.offset20);
        }
        services.set_instance_position(self.first_bone(), &vec);
    }

    /// `SetPoseOri` (slot 20).
    pub fn set_pose_ori(&mut self, ori: &[f32; 9], services: &mut dyn DeformServices) {
        for node in &self.list {
            if let ListPayload::Anim(handle) = node.payload {
                services.set_overlay_extra(handle, ori);
            }
        }
        services.set_instance_orientation(self.first_bone(), ori);
    }

    /// `SetHeight` (slot 24).
    pub fn set_height(&mut self, height: f32, services: &mut dyn DeformServices) {
        self.offset20 = height;
        self.sweep(services);
        for index in 0..self.list.len() {
            let ListPayload::Anim(handle) = self.list[index].payload else {
                continue;
            };
            let (base, plus, reference) = match services.duration_if_named(handle) {
                Some(duration) => {
                    let (pos, extra) = services.touch_mapped(handle);
                    (duration, extra[1], pos[1])
                }
                None => (self.offset24, 0.0, self.root_local()[10]),
            };
            let delta = ((f64::from(base) + f64::from(plus)) + f64::from(self.offset20))
                - f64::from(reference);
            if delta.abs() > 1.192_092_895_507_812_5e-7 {
                services.add_overlay_pos(handle, &[0.0, f(delta), 0.0]);
            }
        }
    }

    /// `GetListCount` (slot 26): after a sweep.
    pub fn list_count(&mut self, services: &mut dyn DeformServices) -> usize {
        self.sweep(services);
        self.list.len()
    }

    /// `CopyList` (slot 27): each node's handle (an aim node's id where the
    /// reference copies its event pointer: divergence D398).
    #[must_use]
    pub fn copy_list(&self) -> Vec<i64> {
        self.list
            .iter()
            .map(|n| match n.payload {
                ListPayload::Anim(h) => i64::from(h),
                ListPayload::Aim(_) => i64::from(n.id),
            })
            .collect()
    }

    /// `GetMeshStats` (slot 37): parts, the lowest level, vertices,
    /// indices, points, skinned points, bound bones, skinned bones, eye
    /// transforms (a VMesh level's counters read 0 where the reference
    /// reads past it: divergence D398). `None` without parts.
    #[must_use]
    pub fn mesh_stats(&self, cache: &MeshCache) -> Option<[u32; 9]> {
        if self.parts.is_empty() {
            return None;
        }
        let mut out = [0u32; 9];
        out[0] = u32::try_from(self.parts.len()).unwrap_or(u32::MAX);
        out[1] = u32::MAX;
        for part in &self.parts {
            let lod = part.lod_index.cast_unsigned();
            if lod < out[1] {
                out[1] = lod;
            }
            out[6] = out[6].wrapping_add(
                part.lod
                    .as_ref()
                    .map_or(0, |h| h.bind_cursor.cast_unsigned()),
            );
            if let Some(mesh) = part.current.and_then(|m| cache.get(m)) {
                out[2] = out[2].wrapping_add(mesh.vertex_capacity());
                out[3] = out[3].wrapping_add(mesh.index_count());
                if let DeformMesh::Dfm(m) = mesh {
                    out[4] =
                        out[4].wrapping_add(u32::try_from(m.vertices.len()).unwrap_or(u32::MAX));
                    out[5] = out[5].wrapping_add(m.skinned_vertex_count);
                    out[7] = out[7].wrapping_add(m.skinned_bone_count);
                    out[8] = out[8].wrapping_add(m.eye_uv_transform_count);
                }
            }
        }
        Some(out)
    }

    /// `GetBoundSphere` (slot 38): the first part's centre and the radius
    /// reaching every part's sphere; `None` when it is not above 0.
    pub fn bound_sphere(&self, cache: &mut MeshCache) -> Option<(f32, [f32; 3])> {
        let first_part = self.parts.first()?;
        let (mut best, first) = first_part.bound_sphere(cache).unwrap_or((0.0, [0.0; 3]));
        for part in &self.parts {
            let (radius, centre) = part.bound_sphere(cache).unwrap_or((0.0, [0.0; 3]));
            let d: [f64; 3] = core::array::from_fn(|k| f64::from(centre[k]) - f64::from(first[k]));
            let reach = (d[2] * d[2] + d[1] * d[1] + d[0] * d[0]).sqrt() + f64::from(radius);
            if reach > f64::from(best) {
                best = f(reach);
            }
        }
        if !(best > 0.0) {
            return None;
        }
        Some((best, first))
    }

    /// `WalkInstanceHardpoints`: for each bound bone of each part, the
    /// hardpoint names of its archetype (`EnumerateHardpoints`), each
    /// visited with the bone's instance as the enumeration delivers it.
    /// `false` without the hardpoint component.
    pub fn walk_hardpoints(
        &self,
        services: &mut dyn DeformServices,
        visit: &mut HardpointVisit<'_>,
    ) -> bool {
        if !services.has_hardpoints() {
            services.dump(
                dump::WARNING,
                file::DEFORMABLE_OBJECT,
                0x4EF,
                "Deform: Unable to acquire IID_IHardpoint, unable to continue",
            );
            return false;
        }
        for part in &self.parts {
            let Some(holder) = part.lod.as_ref() else {
                continue;
            };
            for bone in 0..holder.bound() {
                let instance = holder.bones[bone].instance;
                let archetype = holder
                    .skeleton
                    .bones
                    .get(bone)
                    .map_or(INSTANCE_NONE, |b| b.archetype);
                for name in services.hardpoint_names(archetype) {
                    visit(services, instance, &name);
                }
            }
        }
        true
    }

    /// The hardpoint walk (slot 36) as a list of bone instances and names;
    /// `None` without the hardpoint component.
    pub fn hardpoints(
        &self,
        services: &mut dyn DeformServices,
    ) -> Option<Vec<(Instance, Vec<u8>)>> {
        let mut out = Vec::new();
        self.walk_hardpoints(services, &mut |_, instance, name| {
            out.push((instance, name.to_vec()))
        })
        .then_some(out)
    }

    /// `ConnectInstanceParts`' tail: the parts' bones connected by
    /// hardpoint names (the first bone with a name kept, each later one
    /// connected to it), then every bound bone but the root whose instance
    /// has no parent destroyed.
    pub fn connect_parts(&mut self, services: &mut dyn DeformServices) {
        if !services.has_hardpoints() {
            services.dump(
                dump::WARNING,
                file::DEFORMABLE_OBJECT,
                0x2B,
                "Deform: Unable to acquire IID_IHardpoint, unable to continue",
            );
        }
        // HardpointConnectCallback: the first bone with a name is kept, each
        // later one connected to it.
        let mut pairs: Vec<(u32, Instance, Instance)> = Vec::new();
        self.walk_hardpoints(services, &mut |services, bone, name| {
            let crc = name_crc32(name);
            let at = pairs.iter().position(|p| p.0 == crc).unwrap_or_else(|| {
                pairs.push((crc, INSTANCE_NONE, INSTANCE_NONE));
                pairs.len() - 1
            });
            if pairs[at].1 == INSTANCE_NONE {
                pairs[at].1 = bone;
                return;
            }
            pairs[at].2 = bone;
            if services.connect_hardpoints(pairs[at].1, bone, name) != 0 {
                let n = String::from_utf8_lossy(name);
                services.dump(
                    dump::ERROR,
                    file::DEFORMABLE_OBJECT,
                    0x53,
                    &format!("Deformable: create: cannot connect parts {n}<->{n}"),
                );
            }
        });
        for part in &mut self.parts {
            let Some(holder) = part.lod.as_mut() else {
                continue;
            };
            for bone in 1..holder.bound() {
                let slot_instance = holder.bones[bone].instance;
                if services.instance_parent(slot_instance) == INSTANCE_NONE {
                    services.destroy_instance(slot_instance);
                    holder.bones[bone].instance = INSTANCE_NONE;
                }
            }
        }
    }

    /// `TeardownInstance`: the list torn down, the root instance destroyed,
    /// the parts reset last to first.
    pub fn teardown(&mut self, cache: &mut MeshCache, services: &mut dyn DeformServices) {
        for node in &mut self.list {
            Self::teardown_payload(&mut node.payload, services);
        }
        self.list.clear();
        if !self.parts.is_empty() {
            let root = self.first_bone();
            if root != INSTANCE_NONE {
                services.destroy_instance(root);
            }
            for part in self.parts.iter_mut().rev() {
                part.reset(cache, services);
            }
            self.parts.clear();
        }
    }
}

/// The module's state: `DeformRate` (`G_DeformRate`) and the mesh cache.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Deformable {
    /// The skin rate: seconds between forced skins, `1 / DeformRate`.
    pub rate: f32,
    /// `G_MeshCache`.
    pub meshes: MeshCache,
}

/// `ParseDeformRateValue`: the value cut at the first character outside
/// the profile charset; true/yes 1, false/no 0, hexadecimal with an x,
/// else `atoi`.
#[must_use]
pub fn parse_rate(text: &[u8]) -> u32 {
    const CHARSET: &[u8] = b"0123456789truefalsynoxTRUEFALSYNOX+-";
    let span = text.iter().take_while(|c| CHARSET.contains(c)).count();
    let text = &text[..span];
    if eq_ignore_case(text, b"true") || eq_ignore_case(text, b"yes") {
        return 1;
    }
    if eq_ignore_case(text, b"false") || eq_ignore_case(text, b"no") {
        return 0;
    }
    if text.contains(&b'x') || text.contains(&b'X') {
        let wide: Vec<u16> = text.iter().map(|&c| u16::from(c)).collect();
        return wcstoul(&wide, 16);
    }
    atoi(text).cast_unsigned()
}

impl Deformable {
    /// The state with `[Deformable] DeformRate` applied (`ApplyParsedRate`).
    #[must_use]
    pub fn new(rate_text: Option<&[u8]>) -> Self {
        let rate = rate_text.map_or(0, parse_rate);
        Self {
            rate: if rate == 0 {
                0.0
            } else {
                f(1.0 / f64::from(rate))
            },
            meshes: MeshCache::default(),
        }
    }

    /// `LoadDeformMesh`: the cached mesh of `name`, else its tables from
    /// the mesh node (its materials looked up), else the node as a VMesh
    /// archetype; `None` with the warning when neither loads.
    pub fn load_mesh(
        &mut self,
        holder: &LodHolder,
        fs: &mut dyn FileSystem,
        name: &[u8],
        flag70: bool,
        services: &mut dyn DeformServices,
    ) -> Option<MeshId> {
        if let Some(id) = self.meshes.find(name) {
            return Some(id);
        }
        let tables = dfm::load_mesh(fs).ok();
        let mesh = match tables {
            Some(Ok(tables)) => {
                // LoadFaceGroups looks every material up; BindUvBoneIdsToFaceGroups
                // asks each found one's class name after the other loaders.
                let found: Vec<Option<MaterialId>> = tables
                    .face_groups
                    .iter()
                    .map(|g| {
                        let n = g.material_name.as_ref()?;
                        let (result, material) = services.find_material(n, name_crc32(n));
                        if result < 0 {
                            services.dump(
                                dump::WARNING,
                                file::DEFORMABLE_PART_ARCHETYPE,
                                0x45,
                                "couldnt find material ??? for ",
                            );
                            return None;
                        }
                        material
                    })
                    .collect();
                let materials: Vec<(Option<MaterialId>, Vec<u8>)> = found
                    .into_iter()
                    .map(|m| {
                        (
                            m,
                            m.map(|m| services.material_class_name(m))
                                .unwrap_or_default(),
                        )
                    })
                    .collect();
                match MeshArchetype::from_tables(&tables, &materials, holder.bound()) {
                    Ok(mut m) => {
                        m.flag70 = flag70;
                        Some(DeformMesh::Dfm(Box::new(m)))
                    }
                    Err(_) => {
                        for (m, _) in materials.iter().rev() {
                            if let Some(m) = m {
                                services.release_material(*m);
                            }
                        }
                        None
                    }
                }
            }
            Some(Err(dfm::DfmMeshError::NoFaceGroups | dfm::DfmMeshError::NoGeometry)) => {
                let handle = services.create_archetype(name, fs);
                match services.query_vmesh_part(handle) {
                    Ok(part) => {
                        services.add_ref_vmesh_part(part);
                        services.release_vmesh_part(part);
                        Some(DeformMesh::VMesh(VMeshWrapper {
                            handle,
                            part: Some(part),
                        }))
                    }
                    Err(_) => {
                        services.release_handle(handle);
                        None
                    }
                }
            }
            _ => None,
        };
        let Some(mesh) = mesh else {
            services.dump(
                dump::WARNING,
                file::DEFORMABLE_MESH,
                0x39,
                &format!(
                    "Deformable: Could not load mesh {}",
                    String::from_utf8_lossy(name)
                ),
            );
            return None;
        };
        Some(self.meshes.insert(name, mesh))
    }

    /// `DeformableCreate` with `ConnectInstanceParts`: per part description
    /// its file opened (`open`), its skeleton loaded and attached, its
    /// levels loaded as the description's flags and limit allow, the
    /// descriptions updated; then the parts' bones connected by hardpoint
    /// names and every unparented bone instance but the roots destroyed.
    ///
    /// # Errors
    ///
    /// `GR_INVALID_PARMS` without part descriptions.
    pub fn create(
        &mut self,
        parts: &mut [PartDesc],
        sink: u32,
        script: u32,
        open: &mut dyn FnMut(&PartDesc) -> Option<Box<dyn FileSystem>>,
        services: &mut dyn DeformServices,
    ) -> Result<DeformInstance, i32> {
        if parts.is_empty() {
            return Err(GR_INVALID_PARMS);
        }
        let mut instance = DeformInstance::new(self.rate, services);
        instance.sink = sink;
        instance.script = script;
        instance.parts = vec![DeformPart::default(); parts.len()];
        for (index, desc) in parts.iter_mut().enumerate() {
            let Some(mut root) = open(desc) else { continue };
            let path = desc
                .path
                .clone()
                .or_else(|| desc.lods.first().map(|l| l.name.clone()))
                .unwrap_or_default();
            // The reference needs a backslash and a dot (divergence D395).
            let start = path.iter().rposition(|&c| c == b'\\').map_or(0, |p| p + 1);
            let mut base = path[start..].to_vec();
            if let Some(dot) = base.iter().position(|&c| c == b'.') {
                base.truncate(dot);
            }
            let mut skel_name = b"skel_".to_vec();
            skel_name.extend_from_slice(&base);
            let mut skeleton = Skeleton::default();
            if !skeleton.load(&skel_name, root.as_mut(), services) {
                services.dump(
                    dump::WARNING,
                    file::DEFORMABLE_OBJECT,
                    0xB0,
                    "couldnt load skeleton",
                );
                skeleton.release_handles(services);
                continue;
            }
            let mut holder = LodHolder::default();
            holder.attach_skeleton(skeleton, services);
            let mut fractions = [0.0f32; 8];
            let mut lod_cap;
            if root.open_node(b"MultiLevel").is_ok() {
                lod_cap = 0;
                if let Ok(mut file) = root.open_file(&FsDesc::read(b"Fractions")) {
                    let available = file.size().unwrap_or(0) >> 2;
                    let wanted = available.min(8) as usize;
                    let mut buf = vec![0u8; wanted * 4];
                    let got = file.read(&mut buf).unwrap_or(0);
                    for (k, c) in buf[..got].as_chunks::<4>().0.iter().enumerate() {
                        fractions[k] = f32::from_le_bytes(*c);
                    }
                    lod_cap = i32::try_from(wanted).unwrap_or(0);
                }
            } else {
                lod_cap = 1;
                fractions[0] = 1.0;
            }
            if lod_cap > desc.lod_count {
                lod_cap = desc.lod_count;
            }
            if lod_cap <= 0 {
                desc.lod_count = 0;
                services.dump(
                    dump::WARNING,
                    file::DEFORMABLE_OBJECT,
                    0xD7,
                    "no meshes found",
                );
                // The reference leaks the holder and dereferences the part's
                // null holder later (divergence D395).
                continue;
            }
            let mut loaded: Vec<MeshId> = Vec::new();
            let entries = root.find(b"Mesh*").unwrap_or_default();
            for (lod, entry) in entries.iter().enumerate() {
                let skip = if lod == 0 {
                    desc.flags & 2 == 2
                } else {
                    if desc.flags & 1 == 1 {
                        break;
                    }
                    false
                };
                if skip {
                    continue;
                }
                let child = FsDesc::read(&entry.data.name).with_found(entry.token);
                let mut full = false;
                if let Ok(mut mesh_fs) = root.create_child(&child) {
                    let mut name = base.clone();
                    #[allow(clippy::cast_possible_truncation)]
                    name.push(b'0'.wrapping_add(lod as u8));
                    let flag70 = (lod as u64) < u64::from(desc.lod_limit);
                    if let Some(mesh) =
                        self.load_mesh(&holder, mesh_fs.as_mut(), &name, flag70, services)
                    {
                        let slot = loaded.len();
                        if let Some(l) = desc.lods.get_mut(slot) {
                            l.fraction = fractions.get(lod).copied().unwrap_or(0.0);
                        }
                        loaded.push(mesh);
                        full = i32::try_from(loaded.len()).unwrap_or(i32::MAX) >= lod_cap;
                    }
                }
                if full {
                    break;
                }
            }
            desc.lod_count = i32::try_from(loaded.len()).unwrap_or(0);
            instance.parts[index].bind_meshes(holder, &loaded, &self.meshes);
        }

        instance.connect_parts(services);
        Ok(instance)
    }

    /// `DeformableDestroy`.
    pub fn destroy(&mut self, mut instance: DeformInstance, services: &mut dyn DeformServices) {
        instance.teardown(&mut self.meshes, services);
    }
}
