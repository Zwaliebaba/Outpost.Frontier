//! The scene's events at run time (`FSceneScriptEvent` of
//! `SceneScriptScene.cpp`): the descriptor as applied, the end time, the
//! curve, the values captured when the event starts, and the per-type
//! start, tick and stop.

use super::controls::{EventRef, interface};
use super::entity::World;
use super::math::{
    self, IDENTITY9, Mat9, Vec3, Xform, accumulate_axis_delta, axis_rotation, build_axis_frame,
    ext, floor_hardpoint, floor_wrap_scale, lerp_audio_i, lerp_direction, lerp_f, lerp_i,
    lerp_position, lerp_x, matrix_to_quat, mul_by_inverse_csys, mul_matrix, mul_transform, pack,
    quat_to_matrix, rotate, round, slerp, spatial_wrap_scale, transform,
};
use crate::scene_script::curve::Curve;
use crate::scene_script::entity::{AudioProps, LightProps, kind as entity_kind, light_bit};
use crate::scene_script::event::{EventDesc, EventPayload, IkKey, flag, kind};

/// `kSceneScriptFlagEntityRelative`, `ParentChild`, `OrientRelative`.
const FLAG_ENTITY_RELATIVE: i32 = 0x20;
const FLAG_PARENT_CHILD: i32 = 0x40;
const FLAG_ORIENT_RELATIVE: i32 = 0x80;
/// `kSceneScriptTargetPart`.
const TARGET_PART: i32 = 2;

/// `baselineStillRunningPredicate`: before the end time (unsigned), or
/// for ever with a negative end time.
fn still_running(time: i32, end: i32) -> bool {
    (time as u32) < (end as u32) || end < 0
}

/// A runtime event.
pub struct Event {
    /// The engine's id (`UniqueId`).
    pub(super) id: i32,
    /// The descriptor as applied; START_IK's key is the event itself.
    pub(super) desc: EventDesc,
    /// `EndTimeMs`, -1 for none.
    pub(super) end_ms: i32,
    pub(super) curve: Option<Curve>,
    // Spatial.
    spatial_wrap_last: f32,
    axis_rot_applied: f32,
    capture_pos: Vec3,
    capture_orient: Mat9,
    capture_quat: [f32; 4],
    target_pos: Vec3,
    target_orient: Mat9,
    target_quat: [f32; 4],
    // Property captures.
    cam_capture: [f32; 4],
    light_capture: LightProps,
    sparam_capture: f32,
    audio_capture: AudioProps,
    fog_capture: [u32; 8],
    reverb_capture: [u32; 4],
    floor_capture: f32,
    floor_target: f32,
    floor_wrap_last: f32,
    attach_xform: Xform,
    ik_effector_xform: Xform,
}

impl Event {
    /// The typed constructor (`FSceneScriptEvent(Type, Desc)`): the
    /// descriptor's words, the end time `start + duration`, the curve the
    /// parser made, and the type's initial targets.
    pub(super) fn new(id: i32, desc: &EventDesc) -> Event {
        let mut desc = desc.clone();
        if let EventPayload::Ik { key, .. } = &mut desc.payload {
            *key = IkKey::Event;
        }
        let curve = desc.common.param_curve.as_ref().and_then(Curve::from_desc);
        let end_ms = desc.common.start_ms.wrapping_add(desc.common.duration_ms);
        let mut event = Event {
            id,
            desc,
            end_ms,
            curve,
            spatial_wrap_last: 0.0,
            axis_rot_applied: 0.0,
            capture_pos: [0.0; 3],
            capture_orient: IDENTITY9,
            capture_quat: [1.0, 0.0, 0.0, 0.0],
            target_pos: [0.0; 3],
            target_orient: IDENTITY9,
            target_quat: [1.0, 0.0, 0.0, 0.0],
            cam_capture: [0.0; 4],
            light_capture: LightProps::default(),
            sparam_capture: 0.0,
            audio_capture: AudioProps::default(),
            fog_capture: [0; 8],
            reverb_capture: [0; 4],
            floor_capture: 0.0,
            floor_target: 0.0,
            floor_wrap_last: 0.0,
            attach_xform: pack(&IDENTITY9, &[0.0; 3]),
            // Read before it is written when the primary has no compound
            // control; the reference reads heap garbage (divergence D248).
            ik_effector_xform: [0.0; 12],
        };
        match &event.desc.payload {
            EventPayload::Spatial {
                orient,
                position,
                quaternion,
                ..
            } => {
                event.target_orient = *orient;
                event.target_pos = *position;
                event.target_quat = if event.desc.common.flags & flag::QUAT_ORIENT != 0 {
                    *quaternion
                } else {
                    matrix_to_quat(orient)
                };
            }
            EventPayload::FloorHeight { floor_height, .. } => event.floor_target = *floor_height,
            _ => {}
        }
        event
    }

    pub(super) fn kind(&self) -> i32 {
        self.desc.common.kind
    }

    pub(super) fn start_ms(&self) -> i32 {
        self.desc.common.start_ms
    }

    pub(super) fn primary(&self) -> i32 {
        self.desc.common.primary
    }

    pub(super) fn secondary(&self) -> i32 {
        self.desc.common.secondary
    }

    fn flags(&self) -> i32 {
        self.desc.common.flags
    }

    /// `ApplyEndTime`: none with `USE_SCRIPT_DURATION`, else start plus
    /// duration.
    fn apply_end_time(&mut self) {
        self.end_ms = if self.flags() & flag::USE_SCRIPT_DURATION != 0 {
            -1
        } else {
            self.desc
                .common
                .duration_ms
                .wrapping_add(self.desc.common.start_ms)
        };
    }

    /// `EvaluateProgressExtended`: 1 for no duration; else the curve at
    /// the elapsed share (or, with a period and unless `no_wrap`, at the
    /// elapsed time modulo the period), or the share itself.
    fn progress(&self, time: i32, no_wrap: bool) -> f64 {
        let duration = self.desc.common.duration_ms;
        if duration == 0 {
            return 1.0;
        }
        let elapsed = time.wrapping_sub(self.start_ms()) as u32;
        let period = self.desc.common.pcurve_period;
        if !no_wrap
            && let Some(curve) = &self.curve
            && period > 0
        {
            return curve
                .wrap_evaluate(elapsed as i32, period)
                .expect("a positive period");
        }
        let quotient = f64::from(elapsed) / f64::from(duration);
        if !no_wrap && self.curve.is_none() {
            return quotient;
        }
        let ratio = round(quotient);
        match &self.curve {
            Some(curve) => curve.evaluate(ratio),
            None => ext(ratio),
        }
    }

    /// `EvaluateControlBackedCurve`: the path parameter, `start + scale *
    /// progress`, unrounded; 1 for a zero scale.
    fn path_parameter(&self, scale: f32, start: f32) -> impl Fn(i32) -> f64 + '_ {
        move |time| {
            if scale == 0.0 {
                return 1.0;
            }
            let elapsed = time.wrapping_sub(self.start_ms()) as u32;
            let quotient = f64::from(elapsed) / f64::from(self.desc.common.duration_ms);
            let ratio = round(quotient);
            let value = match &self.curve {
                Some(curve) if self.desc.common.pcurve_period > 0 => curve
                    .wrap_evaluate(elapsed as i32, self.desc.common.pcurve_period)
                    .expect("a positive period"),
                Some(curve) => curve.evaluate(ratio),
                None => quotient,
            };
            value * ext(scale) + ext(start)
        }
    }

    /// `Start`: whether the event becomes active. `time` is the clock the
    /// tick advances to.
    pub(super) fn start(&mut self, w: &mut World, time: i32) -> bool {
        match self.kind() {
            kind::START_IK => {
                self.apply_end_time();
                let (p, s) = (self.primary(), self.secondary());
                if !w.entities.contains_key(&p) || !w.entities.contains_key(&s) {
                    return false;
                }
                if !w.entities[&p].has(interface::IK) {
                    return false;
                }
                let (pos, orient) = self.ik_pose(w);
                let r = EventRef {
                    id: self.id,
                    desc: &self.desc,
                };
                let ctl = w.get(p).and_then(|e| e.ctl(interface::IK)).expect("bound");
                ctl.start_ik(r, pos, orient) == 0
            }
            kind::START_SUB_SCENE => {
                self.apply_end_time();
                let start_time = match self.desc.payload {
                    EventPayload::SubScene { start_time } => start_time,
                    _ => 0.0,
                };
                let value = neuron_core::num::trunc_to_i32_via_i64(ext(start_time));
                match w.get(self.primary()).and_then(|e| e.ctl(interface::SCRIPT)) {
                    Some(c) => c.script_start(value) == 0,
                    None => false,
                }
            }
            kind::START_PSYS => {
                // No end-time prologue: the end stays start plus duration.
                match w.get(self.primary()).and_then(|e| e.ctl(interface::PSYS)) {
                    Some(c) => c.psys_start() == 0,
                    None => false,
                }
            }
            kind::FLOOR_HEIGHT_ANIM => self.start_floor(w),
            kind::REVERB_PROP_ANIM => self.start_reverb(w),
            kind::FOG_PROP_ANIM => self.start_fog(w),
            kind::PSYS_PROP_ANIM => self.start_psys_prop(w),
            kind::AUDIO_PROP_ANIM => self.start_audio(w),
            kind::USER_EVENT => {
                self.apply_end_time();
                let r = EventRef {
                    id: self.id,
                    desc: &self.desc,
                };
                match w
                    .get(self.primary())
                    .and_then(|e| e.ctl(interface::USER_EVENT))
                {
                    Some(c) => c.user_event_start(r) == 0,
                    None => false,
                }
            }
            kind::CAMERA_PROP_ANIM => {
                if let Some(e) = w.get(self.primary())
                    && e.kind == entity_kind::CAMERA
                {
                    self.cam_capture = e.capture_camera();
                }
                self.apply_end_time();
                true
            }
            kind::SPATIAL_PROP_ANIM => self.start_spatial(w),
            kind::ATTACH_ENTITY => self.start_attach(w),
            kind::SET_CAMERA => {
                self.start_set_camera(w);
                false
            }
            kind::PATH_ANIM => {
                if let EventPayload::Path {
                    start_percent,
                    stop_percent,
                    percent_delta,
                    ..
                } = &mut self.desc.payload
                {
                    *percent_delta = round(ext(*stop_percent) - ext(*start_percent));
                }
                self.apply_end_time();
                true
            }
            kind::START_MOTION => {
                self.apply_end_time();
                let r = EventRef {
                    id: self.id,
                    desc: &self.desc,
                };
                match w
                    .get(self.primary())
                    .and_then(|e| e.ctl(interface::COMPOUND))
                {
                    Some(c) => c.start_motion(r) == 0,
                    None => false,
                }
            }
            kind::START_SOUND => {
                self.apply_end_time();
                let r = EventRef {
                    id: self.id,
                    desc: &self.desc,
                };
                match w.get(self.primary()).and_then(|e| e.ctl(interface::SOUND)) {
                    Some(c) => c.start_sound(r, time) == 0,
                    None => false,
                }
            }
            kind::LIGHT_PROP_ANIM => {
                if let Some(e) = w.get(self.primary())
                    && matches!(e.kind, entity_kind::LIGHT | entity_kind::SCENE)
                {
                    self.light_capture = e.capture_light();
                    e.unpack_light(&self.light_capture);
                }
                self.apply_end_time();
                true
            }
            kind::SUBTITLE => {
                let strid = self.strid();
                if let Some(c) = w.get(self.primary()).and_then(subtitle) {
                    c.show_subtitle(strid);
                }
                self.apply_end_time();
                true
            }
            kind::CONNECT_HARDPOINTS => {
                self.apply_end_time();
                let r = EventRef {
                    id: self.id,
                    desc: &self.desc,
                };
                match w
                    .get(self.primary())
                    .and_then(|e| e.ctl(interface::COMPOUND))
                {
                    Some(c) => c.connect_hardpoints(r) == 0,
                    None => false,
                }
            }
            _ => false,
        }
    }

    fn strid(&self) -> i32 {
        match self.desc.payload {
            EventPayload::Subtitle { strid } => strid as i32,
            _ => 0,
        }
    }

    /// `Stop`.
    pub(super) fn stop(&mut self, w: &mut World) {
        let r = EventRef {
            id: self.id,
            desc: &self.desc,
        };
        let p = self.primary();
        match self.kind() {
            kind::START_MOTION => {
                if let Some(c) = w.get(p).and_then(|e| e.ctl(interface::COMPOUND)) {
                    c.stop_motion(r);
                }
            }
            kind::START_SOUND => {
                if let Some(c) = w.get(p).and_then(|e| e.ctl(interface::SOUND)) {
                    c.stop_sound(r);
                }
            }
            kind::START_IK => {
                if w.entities.contains_key(&p)
                    && w.entities.contains_key(&self.secondary())
                    && let Some(c) = w.get(p).and_then(|e| e.ctl(interface::IK))
                {
                    c.stop_ik(r);
                }
            }
            kind::CONNECT_HARDPOINTS => {
                if let Some(c) = w.get(p).and_then(|e| e.ctl(interface::COMPOUND)) {
                    c.disconnect_hardpoints(r);
                }
            }
            kind::START_SUB_SCENE => {
                if let Some(c) = w.get(p).and_then(|e| e.ctl(interface::SCRIPT)) {
                    c.script_stop();
                }
            }
            kind::START_PSYS => {
                if let Some(c) = w.get(p).and_then(|e| e.ctl(interface::PSYS)) {
                    c.psys_stop();
                }
            }
            kind::SUBTITLE => {
                let strid = self.strid();
                if let Some(c) = w.get(p).and_then(subtitle) {
                    c.hide_subtitle(strid);
                }
            }
            kind::USER_EVENT => {
                if let Some(c) = w.get(p).and_then(|e| e.ctl(interface::USER_EVENT)) {
                    c.user_event_stop(r);
                }
            }
            _ => {}
        }
    }

    /// `StillRunning`: the tick of the animations, then whether the event
    /// goes on.
    pub(super) fn still_running(&mut self, w: &mut World, time: i32) -> bool {
        match self.kind() {
            kind::LIGHT_PROP_ANIM => self.tick_light(w, time),
            kind::START_IK => {
                let (p, s) = (self.primary(), self.secondary());
                if w.entities.contains_key(&p)
                    && w.entities.contains_key(&s)
                    && w.entities[&p].has(interface::IK)
                {
                    let (pos, orient) = self.ik_pose(w);
                    let r = EventRef {
                        id: self.id,
                        desc: &self.desc,
                    };
                    let ctl = w.get(p).and_then(|e| e.ctl(interface::IK)).expect("bound");
                    ctl.update_ik(r, pos, orient);
                }
            }
            kind::SET_CAMERA => return false,
            kind::FLOOR_HEIGHT_ANIM => self.tick_floor(w, time),
            kind::REVERB_PROP_ANIM => self.tick_reverb(w, time),
            kind::FOG_PROP_ANIM => self.tick_fog(w, time),
            kind::PSYS_PROP_ANIM => self.tick_psys(w, time),
            kind::AUDIO_PROP_ANIM => self.tick_audio(w, time),
            kind::CAMERA_PROP_ANIM => self.tick_camera(w, time),
            kind::SPATIAL_PROP_ANIM => self.tick_spatial(w, time),
            kind::ATTACH_ENTITY => self.tick_attach(w),
            kind::PATH_ANIM => self.tick_path(w, time),
            _ => {}
        }
        still_running(time, self.end_ms)
    }

    /// `StartSetCamera`: links the camera (secondary) and the monitor
    /// (primary), copies the camera's properties and live pose to the
    /// monitor. Never stays active.
    fn start_set_camera(&mut self, w: &mut World) {
        let (dest, source) = (self.primary(), self.secondary());
        if !w.entities.contains_key(&dest) || source < 0 || !w.entities.contains_key(&source) {
            return;
        }
        if w.add_link(source, dest) {
            w.unbind_monitor_children(dest);
            w.add_link(dest, source);
        }
        let props = w.get(source).expect("checked").capture_camera();
        w.apply_camera(dest, props);
        let (pos, orient) = w.get(source).expect("checked").live_pose();
        w.set_pos(dest, pos);
        w.set_orient(dest, orient);
    }

    fn start_spatial(&mut self, w: &mut World) -> bool {
        self.axis_rot_applied = 0.0;
        let csys = w.csys;
        if let Some(e) = w.get(self.primary()) {
            self.capture_pos = e.relative_position(&csys);
            self.capture_orient = e.relative_orientation(&csys);
            let orient = e.relative_orientation(&csys);
            self.capture_quat = matrix_to_quat(&orient);
        }
        if self.secondary() != -1
            && let Some(e) = w.get(self.secondary())
        {
            self.target_pos = e.relative_position(&csys);
        }
        self.spatial_wrap_last = 0.0;
        self.apply_end_time();
        true
    }

    /// The secondary's frame, with a part's or hardpoint's transform when
    /// the event names one and the secondary is a compound.
    fn child_frame(&self, w: &mut World, part: Option<&[u8]>, target_type: i32) -> Xform {
        let csys = w.csys;
        let e = w.get(self.secondary()).expect("checked");
        let mut child = pack(&IDENTITY9, &[0.0; 3]);
        let pos = e.relative_position(&csys);
        child[9..].copy_from_slice(&pos);
        let orient = e.relative_orientation(&csys);
        child[..9].copy_from_slice(&orient);
        if part.is_some()
            && target_type != 0
            && matches!(e.kind, entity_kind::COMPOUND | entity_kind::DEFORMABLE)
            && let Some(c) = e.ctl(interface::COMPOUND)
        {
            c.get_hardpoint_xform(target_type, part, &mut child);
            child = mul_by_inverse_csys(&csys, &child);
        }
        child
    }

    fn attach_fields(&self) -> (Vec3, i32, i32, i32, Option<Vec<u8>>) {
        match &self.desc.payload {
            EventPayload::Attach {
                offset,
                target_type,
                up,
                front,
                target_part,
            } => (*offset, *target_type, *up, *front, target_part.clone()),
            _ => ([0.0; 3], 0, 1, 5, None),
        }
    }

    fn start_attach(&mut self, w: &mut World) -> bool {
        self.apply_end_time();
        let (p, s) = (self.primary(), self.secondary());
        if !w.entities.contains_key(&p) || !w.entities.contains_key(&s) {
            return true;
        }
        let (_, target_type, _, _, part) = self.attach_fields();
        let child = self.child_frame(w, part.as_deref(), target_type);
        let csys = w.csys;
        let e = w.get(p).expect("checked");
        let mut primary = pack(&IDENTITY9, &[0.0; 3]);
        let pos = e.relative_position(&csys);
        primary[9..].copy_from_slice(&pos);
        let orient = e.relative_orientation(&csys);
        primary[..9].copy_from_slice(&orient);
        let rotation: Mat9 = child[..9].try_into().expect("nine words");
        let position: Vec3 = child[9..].try_into().expect("three words");
        let back = math::transpose_transform(&rotation, &position).map(|v| -v);
        let c = &child;
        let transposed = [c[0], c[3], c[6], c[1], c[4], c[7], c[2], c[5], c[8]];
        self.attach_xform = mul_transform(&pack(&transposed, &back), &primary);
        true
    }

    fn tick_attach(&mut self, w: &mut World) {
        let (p, s) = (self.primary(), self.secondary());
        if !w.entities.contains_key(&p) || !w.entities.contains_key(&s) {
            return;
        }
        let (offset_in, target_type, up, front, part) = self.attach_fields();
        let child = self.child_frame(w, part.as_deref(), target_type);
        let flags = self.flags();
        if flags & FLAG_PARENT_CHILD != 0 {
            let result = mul_transform(&child, &self.attach_xform);
            if flags & flag::POSITION != 0 {
                w.set_pos(p, result[9..].try_into().expect("three words"));
            }
            if flags & flag::ORIENTATION != 0 {
                w.set_orient(p, result[..9].try_into().expect("nine words"));
            }
            return;
        }
        let offset = || {
            if flags & FLAG_ENTITY_RELATIVE != 0 {
                rotate(&child, &offset_in)
            } else {
                offset_in
            }
        };
        let add = |o: Vec3| [0, 1, 2].map(|i| round(ext(child[9 + i]) + ext(o[i])));
        if flags & flag::POSITION != 0 {
            w.set_pos(p, add(offset()));
        }
        if flags & flag::ORIENTATION != 0 {
            if flags & FLAG_ORIENT_RELATIVE != 0 {
                let result = mul_transform(&child, &self.attach_xform);
                w.set_orient(p, result[..9].try_into().expect("nine words"));
            } else {
                w.set_orient(p, child[..9].try_into().expect("nine words"));
            }
        } else if flags & flag::LOOK_AT != 0 {
            let target = add(offset());
            let csys = w.csys;
            let e = w.get(p).expect("checked");
            let origin = e.relative_position(&csys);
            let (dx, dy, dz) = (
                ext(target[0]) - ext(origin[0]),
                ext(target[1]) - ext(origin[1]),
                ext(target[2]) - ext(origin[2]),
            );
            let direction = [round(dx), round(dy), round(dz)];
            let length = (ext(direction[0]) * ext(direction[0]) + dy * dy) + dz * dz;
            if length != 0.0 {
                // The reference's frame is an uninitialised local; columns
                // the axes do not name keep stack garbage (divergence D248).
                let mut orientation = IDENTITY9;
                build_axis_frame(
                    &mut orientation,
                    &direction,
                    front as u32,
                    up as u32,
                    w.scene_front,
                    w.scene_up,
                );
                w.set_orient(p, orientation);
            }
        }
    }

    fn path_fields(&self) -> (f32, f32, Vec3, i32, i32) {
        match &self.desc.payload {
            EventPayload::Path {
                start_percent,
                percent_delta,
                offset,
                up,
                front,
                ..
            } => (*start_percent, *percent_delta, *offset, *up, *front),
            _ => (0.0, 1.0, [0.0; 3], 1, 5),
        }
    }

    fn tick_path(&mut self, w: &mut World, time: i32) {
        let (p, s) = (self.primary(), self.secondary());
        if !w.entities.contains_key(&p) || !w.entities.contains_key(&s) {
            return;
        }
        if w.entities[&s].path_binding == super::entity::PathBinding::None {
            return;
        }
        let (start, delta, offset, up, front) = self.path_fields();
        let percent = round(self.path_parameter(delta, start)(time));
        let csys = w.csys;
        let primary = w.get(p).expect("checked");
        let mut position = primary.relative_position(&csys);
        let mut orientation = primary.relative_orientation(&csys);
        let flags = self.flags();
        // The reference's output buffer is a local the path writes; a path
        // that answers 0 without writing leaves it unset (divergence D248).
        let mut vector = [0.0; 3];
        if flags & flag::POSITION != 0 && path_evaluate(w, s, percent, &mut vector) == 0 {
            let e = w.get(s).expect("checked");
            let mut xform = pack(&IDENTITY9, &[0.0; 3]);
            let orient = e.relative_orientation(&csys);
            xform[..9].copy_from_slice(&orient);
            let pos = e.relative_position(&csys);
            xform[9..].copy_from_slice(&pos);
            let moved = [0, 1, 2].map(|i| round(ext(vector[i]) + ext(offset[i])));
            position = transform(&xform, &moved);
        }
        if flags & flag::ORIENTATION != 0 {
            if flags & flag::LOOK_AT != 0 {
                if path_look_at(w, s, percent, &mut vector) == 0 {
                    let e = w.get(s).expect("checked");
                    let mut xform = pack(&IDENTITY9, &[0.0; 3]);
                    let orient = e.relative_orientation(&csys);
                    xform[..9].copy_from_slice(&orient);
                    vector = rotate(&xform, &vector);
                    build_axis_frame(
                        &mut orientation,
                        &vector,
                        front as u32,
                        up as u32,
                        w.scene_front,
                        w.scene_up,
                    );
                }
            } else {
                let mut quat = [0.0; 4];
                if path_orientation(w, s, percent, &mut quat) == 0 {
                    let path_matrix = quat_to_matrix(&quat);
                    let e = w.get(s).expect("checked");
                    let secondary = e.relative_orientation(&csys);
                    orientation = mul_matrix(&secondary, &path_matrix);
                }
            }
        }
        w.set_orient(p, orientation);
        w.set_pos(p, position);
    }

    fn tick_spatial(&mut self, w: &mut World, time: i32) {
        let EventPayload::Spatial {
            mask,
            axis_rot_angle,
            axis_rot_axis,
            target_part,
            target_type,
            ..
        } = &self.desc.payload
        else {
            return;
        };
        let (mask, angle, axis, target_type) =
            (*mask, *axis_rot_angle, *axis_rot_axis, *target_type);
        let part = target_part.clone();
        if mask == 0 {
            return;
        }
        let progress = self.progress(time, false);
        let (u, scale) = spatial_wrap_scale(&mut self.spatial_wrap_last, progress);
        let csys = w.csys;
        let Some(primary) = w.get(self.primary()) else {
            return;
        };
        self.capture_pos = primary.relative_position(&csys);
        let orient = primary.relative_orientation(&csys);
        self.capture_quat = matrix_to_quat(&orient);
        if self.secondary() != -1
            && let Some(e) = w.get(self.secondary())
        {
            self.target_pos = e.relative_position(&csys);
            let orient = e.relative_orientation(&csys);
            self.target_quat = matrix_to_quat(&orient);
            if part.is_some()
                && target_type != 0
                && matches!(e.kind, entity_kind::COMPOUND | entity_kind::DEFORMABLE)
                && let Some(c) = e.ctl(interface::COMPOUND)
            {
                let mut hardpoint = pack(&IDENTITY9, &[0.0; 3]);
                c.get_hardpoint_xform(target_type, part.as_deref(), &mut hardpoint);
                hardpoint = mul_by_inverse_csys(&csys, &hardpoint);
                self.target_orient = hardpoint[..9].try_into().expect("nine words");
                self.target_pos = hardpoint[9..].try_into().expect("three words");
                self.target_quat = matrix_to_quat(&self.target_orient);
            }
        }
        let p = self.primary();
        if mask & crate::scene_script::event::spatial_bit::POS != 0 {
            w.set_pos(p, lerp_position(&self.capture_pos, &self.target_pos, scale));
        }
        if mask & crate::scene_script::event::spatial_bit::ORIENT != 0 {
            let quat = slerp(&self.capture_quat, &self.target_quat, scale);
            w.set_orient(p, quat_to_matrix(&quat));
        }
        if mask & crate::scene_script::event::spatial_bit::AXIS_ROT != 0 {
            let delta = accumulate_axis_delta(u, angle, &mut self.axis_rot_applied);
            w.set_orient(p, axis_rotation(&self.capture_quat, axis, delta));
        }
    }

    fn tick_camera(&mut self, w: &mut World, time: i32) {
        let EventPayload::Camera { mask, target } = &self.desc.payload else {
            return;
        };
        let (mask, mut targets) = (*mask, *target);
        if mask == 0 {
            return;
        }
        if self.secondary() != -1
            && let Some(e) = w.get(self.secondary())
            && e.kind == entity_kind::CAMERA
        {
            targets = e.capture_camera();
        }
        if !w.entities.contains_key(&self.primary()) {
            return;
        }
        let progress = self.progress(time, false);
        let e = w.get(self.primary()).expect("checked");
        let mut values = e.camera_runtime;
        let c = &self.cam_capture;
        // Fov (bit 0), aspect (bit 3), near (bit 1), far (bit 2).
        for (bit, index) in [(1, 0), (8, 3), (2, 1), (4, 2)] {
            if mask & bit != 0 {
                values[index] = lerp_x(c[index], targets[index], progress);
            }
        }
        w.apply_camera(self.primary(), values);
    }

    fn start_psys_prop(&mut self, w: &mut World) -> bool {
        if let Some(e) = w.get(self.primary())
            && e.kind == entity_kind::PSYS
        {
            let mut value = e.psys_sparam;
            if let Some(c) = e.ctl(interface::PSYS) {
                c.psys_get_sparam(&mut value);
            }
            e.psys_sparam = value;
            self.sparam_capture = value;
        }
        self.apply_end_time();
        true
    }

    fn tick_psys(&mut self, w: &mut World, time: i32) {
        let EventPayload::PsysProp { mask, sparam } = self.desc.payload else {
            return;
        };
        if mask == 0 {
            return;
        }
        let mut target = sparam;
        if self.secondary() != -1
            && let Some(e) = w.get(self.secondary())
            && e.kind == entity_kind::PSYS
        {
            target = e.psys_sparam;
        }
        if !w.entities.contains_key(&self.primary()) {
            return;
        }
        let scale = round(self.progress(time, false));
        let capture = self.sparam_capture;
        let e = w.get(self.primary()).expect("checked");
        if e.kind == entity_kind::PSYS {
            let mut result = e.psys_sparam;
            if mask & 1 != 0 {
                result = lerp_f(capture, target, scale);
            }
            e.apply_sparam(result);
        }
    }

    fn start_audio(&mut self, w: &mut World) -> bool {
        if let Some(e) = w.get(self.primary())
            && e.kind == entity_kind::SOUND
        {
            self.audio_capture = e.capture_audio();
        }
        self.apply_end_time();
        true
    }

    fn tick_audio(&mut self, w: &mut World, time: i32) {
        let EventPayload::Audio { mask, target } = &self.desc.payload else {
            return;
        };
        let (mask, mut to) = (*mask, target.clone());
        if mask == 0 {
            return;
        }
        if self.secondary() != -1
            && let Some(e) = w.get(self.secondary())
            && e.kind == entity_kind::SOUND
        {
            to = e.audio.clone();
        }
        if !w.entities.contains_key(&self.primary()) {
            return;
        }
        let p = self.progress(time, false);
        let e = w.get(self.primary()).expect("checked");
        let c = &self.audio_capture;
        let mut out = e.audio.clone();
        if mask & 1 != 0 {
            out.attenuation = lerp_x(c.attenuation, to.attenuation, p);
        }
        if mask & 2 != 0 {
            out.pan = lerp_x(c.pan, to.pan, p);
        }
        if mask & 4 != 0 {
            out.dmin = lerp_x(c.dmin, to.dmin, p);
        }
        if mask & 8 != 0 {
            out.dmax = lerp_x(c.dmax, to.dmax, p);
        }
        if mask & 0x10 != 0 {
            out.ain = lerp_audio_i(c.ain, to.ain, p);
        }
        if mask & 0x20 != 0 {
            out.aout = lerp_audio_i(c.aout, to.aout, p);
        }
        if mask & 0x40 != 0 {
            out.atout = lerp_x(c.atout, to.atout, p);
        }
        if mask & 0x80 != 0 {
            out.rmix = lerp_x(c.rmix, to.rmix, p);
        }
        e.apply_audio(&out);
    }

    fn start_fog(&mut self, w: &mut World) -> bool {
        if let Some(e) = w.get(self.primary())
            && e.kind == entity_kind::SCENE
        {
            let mut words: [u32; 8] = e.scene_packed[7..].try_into().expect("eight words");
            if let Some(c) = e.ctl(interface::FOG) {
                c.fog_get_packed(&mut words);
            }
            e.scene_packed[7..].copy_from_slice(&words);
            self.fog_capture = words;
        }
        self.apply_end_time();
        true
    }

    fn tick_fog(&mut self, w: &mut World, time: i32) {
        let EventPayload::Fog { mask, target } = &self.desc.payload else {
            return;
        };
        let (mask, target) = (*mask, target.clone());
        if mask == 0 || !w.entities.contains_key(&self.primary()) {
            return;
        }
        let u = round(self.progress(time, false));
        let key = self.primary();
        let e = w.get(key).expect("checked");
        if e.kind != entity_kind::SCENE {
            return;
        }
        let mut out = e.scene_packed;
        let cap = &self.fog_capture;
        let mut flags = out[7].to_le_bytes();
        if u >= 1.0 {
            if mask & 1 != 0 {
                flags[0] = target.on;
            }
            if mask & 2 != 0 {
                flags[1] = target.table;
            }
            out[7] = u32::from_le_bytes(flags);
            if mask & 8 != 0 {
                out[11] = target.mode as u32;
            }
        }
        let lerp_word = |from: u32, to: f32| lerp_f(f32::from_bits(from), to, u).to_bits();
        if mask & 0x10 != 0 {
            out[12] = lerp_word(cap[5], target.start);
        }
        if mask & 0x20 != 0 {
            out[13] = lerp_word(cap[6], target.end);
        }
        if mask & 0x40 != 0 {
            out[14] = lerp_word(cap[7], target.density);
        }
        if mask & 4 != 0 {
            for i in 0..3 {
                out[8 + i] = lerp_i(cap[1 + i] as i32, target.color[i], u) as u32;
            }
        }
        w.publish_scene(key, out, 0);
        w.get(key).expect("checked").publish_fog(mask as u8);
    }

    fn start_reverb(&mut self, w: &mut World) -> bool {
        if let Some(e) = w.get(self.primary())
            && e.kind == entity_kind::SCENE
        {
            if let Some(c) = e.ctl(interface::REVERB) {
                c.get_reverb(&mut self.reverb_capture);
            } else {
                self.reverb_capture = [0, 0, 1.0_f32.to_bits(), 1.0_f32.to_bits()];
            }
        }
        self.apply_end_time();
        true
    }

    fn tick_reverb(&mut self, w: &mut World, time: i32) {
        let EventPayload::Reverb { mask, target } = &self.desc.payload else {
            return;
        };
        let (mask, target) = (*mask, target.clone());
        if mask == 0 || !w.entities.contains_key(&self.primary()) {
            return;
        }
        let u = round(self.progress(time, true));
        let cap = self.reverb_capture;
        let e = w.get(self.primary()).expect("checked");
        if e.kind != entity_kind::SCENE {
            return;
        }
        let Some(c) = e.ctl(interface::REVERB) else {
            return;
        };
        let mut out = cap;
        if u >= 1.0 && mask & 1 != 0 {
            out[0] = target.environment as u32;
        }
        let f = |w: u32| f32::from_bits(w);
        if mask & 2 != 0 {
            out[1] = lerp_f(f(cap[1]), target.volume, u).to_bits();
        }
        if mask & 4 != 0 {
            out[2] = lerp_f(f(cap[2]), target.decay, u).to_bits();
        }
        if mask & 8 != 0 {
            out[3] = lerp_f(f(cap[3]), target.damping, u).to_bits();
        }
        c.set_reverb(out[0], f(out[1]), f(out[2]), f(out[3]));
    }

    fn floor_fields(&self) -> (Option<Vec<u8>>, i32) {
        match &self.desc.payload {
            EventPayload::FloorHeight {
                target_part,
                target_type,
                ..
            } => (target_part.clone(), *target_type),
            _ => (None, 0),
        }
    }

    fn start_floor(&mut self, w: &mut World) -> bool {
        if let Some(e) = w.get(self.primary())
            && e.kind == entity_kind::DEFORMABLE
        {
            if let Some(c) = e.ctl(interface::COMPOUND) {
                c.get_floor_height(&mut self.floor_capture);
            } else {
                e.floor_height = e.snap_floor_height;
                self.floor_capture = e.snap_floor_height;
            }
        }
        let csys = w.csys;
        if self.secondary() != -1
            && let Some(e) = w.get(self.secondary())
        {
            self.floor_target = e.relative_position(&csys)[1];
        }
        self.floor_wrap_last = 0.0;
        self.apply_end_time();
        true
    }

    fn tick_floor(&mut self, w: &mut World, time: i32) {
        let progress = self.progress(time, false);
        let scale = floor_wrap_scale(&mut self.floor_wrap_last, progress);
        if !w.entities.contains_key(&self.primary()) {
            return;
        }
        let csys = w.csys;
        {
            let e = w.get(self.primary()).expect("checked");
            if let Some(c) = e.ctl(interface::COMPOUND) {
                c.get_floor_height(&mut self.floor_capture);
            } else {
                self.floor_capture = e.floor_height;
            }
        }
        if self.secondary() != -1
            && let Some(e) = w.get(self.secondary())
        {
            self.floor_target = e.relative_position(&csys)[1];
            let (part, target_type) = self.floor_fields();
            if part.is_some()
                && target_type != 0
                && matches!(e.kind, entity_kind::COMPOUND | entity_kind::DEFORMABLE)
                && let Some(c) = e.ctl(interface::COMPOUND)
            {
                let mut hardpoint = pack(&IDENTITY9, &[0.0; 3]);
                c.get_hardpoint_xform(target_type, part.as_deref(), &mut hardpoint);
                hardpoint = floor_hardpoint(&csys, &hardpoint);
                self.floor_target = hardpoint[10];
            }
        }
        let e = w.get(self.primary()).expect("checked");
        let height = lerp_f(self.floor_capture, self.floor_target, scale);
        if let Some(c) = e.ctl(interface::COMPOUND) {
            c.set_floor_height(height);
            e.floor_height = height;
        }
    }

    fn tick_light(&mut self, w: &mut World, time: i32) {
        let EventPayload::Light { mask, target } = &self.desc.payload else {
            return;
        };
        let (mask, mut target) = (*mask, target.clone());
        if mask == 0 {
            return;
        }
        if self.secondary() != -1
            && let Some(e) = w.get(self.secondary())
        {
            if e.kind == entity_kind::LIGHT {
                let mut live = e.light.clone();
                if e.has(interface::D3D_LIGHT) {
                    let c = e.controls.as_deref_mut().expect("bound");
                    c.d3d_get_light(&mut live);
                    c.d3d_get_on(&mut live.on);
                    c.d3d_get_direction(&mut live.direction);
                } else if e.has(interface::LIGHT) {
                    let c = e.controls.as_deref_mut().expect("bound");
                    c.light_get_color(&mut live.color);
                    c.light_get_direction(&mut live.direction);
                    c.light_get_range(&mut live.range_temp);
                    c.light_get_cutoff(&mut live.cutoff_temp);
                    c.light_get_slot14(&mut live.infinite);
                    c.light_get_slot13(&mut live.on);
                    crate::scene_script::build::convert_compact_light(&mut live);
                    live.unused_1e = 1.0_f32.to_bits();
                }
                e.light = live.clone();
                target = live;
            } else if e.kind == entity_kind::SCENE {
                let mut color = [
                    e.scene_packed[0] as i32,
                    e.scene_packed[1] as i32,
                    e.scene_packed[2] as i32,
                ];
                if let Some(c) = e.ctl(interface::LIGHT) {
                    c.light_get_color(&mut color);
                }
                for (word, value) in e.scene_packed[..3].iter_mut().zip(color) {
                    *word = value as u32;
                }
                target.color = color;
            }
        }
        let key = self.primary();
        if !w.entities.contains_key(&key) {
            return;
        }
        let u = round(self.progress(time, false));
        let e = w.get(key).expect("checked");
        let mut out = match e.kind {
            entity_kind::LIGHT => e.light.clone(),
            entity_kind::SCENE => LightProps {
                color: [
                    e.scene_packed[0] as i32,
                    e.scene_packed[1] as i32,
                    e.scene_packed[2] as i32,
                ],
                ..LightProps::default()
            },
            _ => LightProps::default(),
        };
        eval_light(&mut out, &self.light_capture, &target, mask, u);
        if e.kind == entity_kind::LIGHT {
            e.apply_light_runtime(&out);
        } else if e.kind == entity_kind::SCENE {
            let mut words = e.scene_packed;
            for (word, value) in words[..3].iter_mut().zip(out.color) {
                *word = value as u32;
            }
            w.publish_scene(key, words, 0xff);
        }
    }

    /// `BuildIkPose`: the end effector's transform from the primary, the
    /// target from the secondary (or its part), the frame looking from the
    /// effector to the target, all in world space.
    fn ik_pose(&mut self, w: &mut World) -> (Vec3, Mat9) {
        let EventPayload::Ik {
            up,
            front,
            end_effector,
            target_part,
            target_type,
            ..
        } = &self.desc.payload
        else {
            return ([0.0; 3], IDENTITY9);
        };
        let (up, front, target_type) = (*up, *front, *target_type);
        let (effector, part) = (end_effector.clone(), target_part.clone());
        let csys = w.csys;
        let p = w.get(self.primary()).expect("checked");
        if let Some(c) = p.ctl(interface::COMPOUND) {
            c.get_hardpoint_xform(
                TARGET_PART,
                effector.as_deref(),
                &mut self.ik_effector_xform,
            );
            self.ik_effector_xform = mul_by_inverse_csys(&csys, &self.ik_effector_xform);
        }
        let mut orient = IDENTITY9;
        let s = w.get(self.secondary()).expect("checked");
        let mut pos = s.relative_position(&csys);
        if part.is_some()
            && target_type != 0
            && matches!(s.kind, entity_kind::COMPOUND | entity_kind::DEFORMABLE)
            && let Some(c) = s.ctl(interface::COMPOUND)
        {
            let mut hardpoint = pack(&orient, &pos);
            c.get_hardpoint_xform(target_type, part.as_deref(), &mut hardpoint);
            hardpoint = mul_by_inverse_csys(&csys, &hardpoint);
            orient = hardpoint[..9].try_into().expect("nine words");
            pos = hardpoint[9..].try_into().expect("three words");
        }
        let stable = pos;
        let x = &self.ik_effector_xform;
        let direction = [0, 1, 2].map(|i| round(ext(pos[i]) - ext(x[9 + i])));
        build_axis_frame(
            &mut orient,
            &direction,
            front as u32,
            up as u32,
            w.scene_front,
            w.scene_up,
        );
        let pos = transform(&csys.words(), &stable);
        let orient = mul_matrix(&csys.orient, &orient);
        (pos, orient)
    }
}

/// `SubtitleOf`: the subtitle control of the types that can show one.
fn subtitle(
    e: &mut super::entity::Entity,
) -> Option<&mut (dyn super::controls::EntityControls + 'static)> {
    if !matches!(
        e.kind,
        entity_kind::DEFORMABLE
            | entity_kind::CAMERA
            | entity_kind::MONITOR
            | entity_kind::SOUND
            | entity_kind::SCENE
    ) {
        return None;
    }
    e.ctl(interface::SUBTITLE)
}

fn path_evaluate(w: &mut World, key: i32, t: f32, out: &mut Vec3) -> i32 {
    let e = w.get(key).expect("checked");
    match e.path_binding {
        super::entity::PathBinding::Own => e.path.as_ref().map_or(-1, |p| p.evaluate(t, out)),
        super::entity::PathBinding::Host => e
            .controls
            .as_deref_mut()
            .map_or(-1, |c| c.path_evaluate(t, out)),
        super::entity::PathBinding::None => -1,
    }
}

fn path_look_at(w: &mut World, key: i32, t: f32, out: &mut Vec3) -> i32 {
    let e = w.get(key).expect("checked");
    match e.path_binding {
        super::entity::PathBinding::Own => e.path.as_ref().map_or(-1, |p| p.look_at(t, out)),
        super::entity::PathBinding::Host => e
            .controls
            .as_deref_mut()
            .map_or(-1, |c| c.path_look_at(t, out)),
        super::entity::PathBinding::None => -1,
    }
}

fn path_orientation(w: &mut World, key: i32, t: f32, out: &mut [f32; 4]) -> i32 {
    let e = w.get(key).expect("checked");
    match e.path_binding {
        super::entity::PathBinding::Own => e.path.as_ref().map_or(-1, |p| p.orientation_at(t, out)),
        super::entity::PathBinding::Host => e
            .controls
            .as_deref_mut()
            .map_or(-1, |c| c.path_orientation(t, out)),
        super::entity::PathBinding::None => -1,
    }
}

/// `EvalLightProps`: each masked property from the capture toward the
/// target; the discrete ones (type, infinite, on) switch once the
/// progress reaches 1.
fn eval_light(out: &mut LightProps, cap: &LightProps, to: &LightProps, mask: i32, u: f32) {
    let done = u >= 1.0;
    if mask & light_bit::RANGE != 0 {
        out.range = lerp_f(cap.range, to.range, u);
    }
    if mask & light_bit::CUTOFF != 0 {
        out.cutoff = lerp_f(cap.cutoff, to.cutoff, u);
    }
    if mask & light_bit::TYPE != 0 && done {
        out.light_type = to.light_type;
    }
    if mask & light_bit::THETA != 0 {
        out.theta = lerp_f(cap.theta, to.theta, u);
    }
    let lerp3 = |o: &mut [f32; 4], c: &[f32; 4], t: &[f32; 4]| {
        for i in 0..3 {
            o[i] = lerp_f(c[i], t[i], u);
        }
    };
    if mask & light_bit::SPECULAR != 0 {
        lerp3(&mut out.specular, &cap.specular, &to.specular);
    }
    if mask & light_bit::AMBIENT != 0 {
        lerp3(&mut out.ambient, &cap.ambient, &to.ambient);
    }
    if mask & light_bit::DIFFUSE != 0 {
        lerp3(&mut out.diffuse, &cap.diffuse, &to.diffuse);
    }
    if mask & light_bit::ATTEN != 0 {
        for i in 0..3 {
            out.atten[i] = lerp_f(cap.atten[i], to.atten[i], u);
        }
    }
    if mask & light_bit::COLOR != 0 {
        for i in 0..3 {
            out.color[i] = lerp_i(cap.color[i], to.color[i], u);
        }
    }
    if mask & light_bit::INFINITE != 0 && done {
        out.infinite = to.infinite;
    }
    if mask & light_bit::DIRECTION != 0 {
        out.direction = lerp_direction(&cap.direction, &to.direction, u);
    }
    if mask & light_bit::ON != 0 && done {
        out.on = to.on;
    }
}
