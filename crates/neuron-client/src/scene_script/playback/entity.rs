//! The scene's entities at run time (`FSceneScriptEntity` of
//! `SceneScriptScene.cpp`): the descriptor as applied, the pose in the
//! scene's frame, the property caches the animations read and write, the
//! bound controls, and the operations that publish to them.

use std::cmp::Ordering;
use std::collections::BTreeMap;

use super::controls::{EntityControls, interface};
use super::math::{
    self, Csys, IDENTITY9, Mat9, Vec3, color_to_int, ext, invert_csys, mul_matrix, round, transform,
};
use crate::scene_script::build::convert_compact_light;
use crate::scene_script::entity::{
    AudioProps, EntityDesc, EntityTail, LightProps, kind as entity_kind,
};
use crate::scene_script::spline::SplinePath;

/// The interfaces `Attach` asks an engine object for, by entity type.
pub(super) fn bind_set(owner_type: i32) -> u32 {
    use interface::*;
    match owner_type {
        entity_kind::COMPOUND | entity_kind::DEFORMABLE => {
            COMPOUND | IK | GEO | USER_EVENT | SUBTITLE
        }
        entity_kind::CAMERA | entity_kind::MONITOR => GEO | CAMERA | USER_EVENT | SUBTITLE,
        entity_kind::LIGHT => GEO | LIGHT | D3D_LIGHT | USER_EVENT,
        entity_kind::SOUND => SOUND | GEO | USER_EVENT | SUBTITLE,
        entity_kind::SCENE | entity_kind::SUB_SCENE => {
            GEO | LIGHT | USER_EVENT | SCRIPT | FOG | REVERB | SUBTITLE
        }
        entity_kind::MOTION_PATH => GEO | MOTION_PATH | USER_EVENT,
        entity_kind::PSYS => GEO | PSYS | USER_EVENT,
        _ => GEO | USER_EVENT,
    }
}

/// Which motion path a MOTION_PATH entity evaluates.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum PathBinding {
    None,
    /// The path object the parser created, bound by the constructor.
    Own,
    /// The attached engine object's `IMotionPath`.
    Host,
}

/// `FSceneScriptUserPropLess`: bytes compared case-insensitively over the
/// shorter length, then the shorter first.
fn user_prop_cmp(a: &[u8], b: &[u8]) -> Ordering {
    let n = a.len().min(b.len());
    for (x, y) in a[..n].iter().zip(&b[..n]) {
        let (x, y) = (x.to_ascii_lowercase(), y.to_ascii_lowercase());
        if x != y {
            return x.cmp(&y);
        }
    }
    a.len().cmp(&b.len())
}

/// A runtime entity.
pub struct Entity {
    /// `DescriptorOwnerType`: the type it was created as.
    pub(super) owner_type: i32,
    /// `Snap.Type`, which an update may change.
    pub(super) kind: i32,
    pub(super) flags: i32,
    /// The live pose in the scene's frame (`Snap.Pos`, `Snap.Orient`).
    pub(super) pos: Vec3,
    pub(super) orient: Mat9,
    pub(super) name: Option<Vec<u8>>,
    pub(super) template_name: Option<Vec<u8>>,
    pub(super) groups: [u8; 3],
    /// The pose of the last applied descriptor (`LoadPos`, `LoadOrient`).
    pub(super) load_pos: Vec3,
    pub(super) load_orient: Mat9,
    // The descriptor tail, one field per view of the reference's union.
    pub(super) snap_floor_height: f32,
    /// Fov, near, far, aspect.
    pub(super) snap_camera: [f32; 4],
    pub(super) snap_light: LightProps,
    pub(super) snap_audio: AudioProps,
    pub(super) snap_scene: [u32; 15],
    pub(super) snap_psys: f32,
    pub(super) path_type: Option<Vec<u8>>,
    // Runtime state.
    pub(super) controls: Option<Box<dyn EntityControls>>,
    /// The interfaces held (`*Control != nullptr`).
    pub(super) bound: u32,
    pub(super) monitor_links: Vec<i32>,
    /// `CameraRuntimeProps`: fov, near, far, aspect.
    pub(super) camera_runtime: [f32; 4],
    pub(super) floor_height: f32,
    /// `LightEntityDescriptor`.
    pub(super) light: LightProps,
    pub(super) scene_ambient: [i32; 3],
    /// `Audio`.
    pub(super) audio: AudioProps,
    pub(super) psys_sparam: f32,
    /// `SceneFogPacked`: the scene words as last published.
    pub(super) scene_packed: [u32; 15],
    pub(super) path: Option<SplinePath>,
    pub(super) path_binding: PathBinding,
    /// The descriptor's path object (`Snap.MotionPath.MotionPath`) once an
    /// update replaced it; `None` while it is the object the entity was
    /// made with, [`Self::path`].
    pub(super) snap_path: Option<Option<SplinePath>>,
    pub(super) user_props: Vec<(Vec<u8>, Vec<u8>)>,
}

/// The scene words of a SCENE descriptor (`LoadSceneEntityDesc`).
fn scene_words(tail: &EntityTail) -> Option<[u32; 15]> {
    let EntityTail::Scene(s) = tail else {
        return None;
    };
    let f = &s.fog;
    Some([
        s.ambient[0] as u32,
        s.ambient[1] as u32,
        s.ambient[2] as u32,
        s.duration_ms,
        s.start_ms as u32,
        s.up as u32,
        s.front as u32,
        u32::from(f.on) | (u32::from(f.table) << 8),
        f.color[0] as u32,
        f.color[1] as u32,
        f.color[2] as u32,
        f.mode as u32,
        f.start.to_bits(),
        f.end.to_bits(),
        f.density.to_bits(),
    ])
}

impl Entity {
    /// The common default constructor and the typed one up to its
    /// descriptor (`FSceneScriptEntity(Type, Desc, Scene)`), with no
    /// parent frame bound yet.
    pub(super) fn new(desc: &EntityDesc) -> Entity {
        let owner_type = desc.kind;
        let mut light = LightProps::entity_defaults();
        light.unused_1e = 1.0_f32.to_bits();
        let mut entity = Entity {
            owner_type,
            kind: 0,
            flags: 0,
            pos: [0.0; 3],
            orient: IDENTITY9,
            name: None,
            template_name: None,
            groups: [0; 3],
            load_pos: desc.pos,
            load_orient: desc.orient,
            snap_floor_height: 0.0,
            snap_camera: [60.0, 0.1, 100_000.0, 4.0 / 3.0],
            snap_light: LightProps::entity_defaults(),
            snap_audio: AudioProps::default(),
            snap_scene: [0; 15],
            snap_psys: 0.0,
            path_type: None,
            controls: None,
            bound: 0,
            monitor_links: Vec::new(),
            camera_runtime: [0.0; 4],
            floor_height: 0.0,
            light,
            scene_ambient: [0; 3],
            audio: AudioProps::default(),
            psys_sparam: 0.0,
            scene_packed: [0; 15],
            path: None,
            path_binding: PathBinding::None,
            snap_path: None,
            user_props: Vec::new(),
        };
        match owner_type {
            entity_kind::SOUND => {
                let defaults = AudioProps {
                    dmin: 1.0,
                    dmax: 1_000_000_000.0,
                    ain: 0x168,
                    aout: 0x168,
                    ..AudioProps::default()
                };
                entity.audio = defaults.clone();
                entity.snap_audio = defaults;
            }
            entity_kind::SCENE | entity_kind::SUB_SCENE => {
                entity.snap_scene[5] = 6;
                entity.snap_scene[6] = 6;
            }
            _ => {}
        }
        entity.apply_snap(desc, None);
        match owner_type {
            entity_kind::COMPOUND | entity_kind::DEFORMABLE => {
                entity.floor_height = entity.snap_floor_height;
            }
            entity_kind::CAMERA | entity_kind::MONITOR => {
                entity.camera_runtime = entity.snap_camera;
            }
            entity_kind::LIGHT => entity.light = entity.snap_light.clone(),
            entity_kind::SOUND => entity.audio = entity.snap_audio.clone(),
            entity_kind::SCENE | entity_kind::SUB_SCENE => entity.scene_packed = entity.snap_scene,
            entity_kind::MOTION_PATH => {
                if let EntityTail::MotionPath(m) = &desc.tail {
                    entity.path = m.path.as_ref().map(SplinePath::from_desc);
                    if entity.path.is_some() {
                        entity.path_binding = PathBinding::Own;
                    }
                }
            }
            _ => {}
        }
        // The constructors replay the saved pose with nothing bound.
        entity.pos = entity.load_pos;
        entity.orient = entity.load_orient;
        entity
    }

    /// `FSceneScriptScene::ApplyEntitySnap` and `InnerApplyDesc`: the
    /// common words, the pose converted into the parent frame when one is
    /// bound, the saved pose, then the tail of the owner's type when the
    /// descriptor is not a reference and has that type.
    pub(super) fn apply_snap(&mut self, desc: &EntityDesc, parent: Option<&Csys>) -> bool {
        self.kind = desc.kind;
        self.orient = desc.orient;
        self.pos = desc.pos;
        self.groups = [desc.lt_grp, desc.srt_grp, desc.usr_flg];
        self.flags = desc.flags;
        if self.owner_type != entity_kind::SOUND {
            self.name.clone_from(&desc.name);
            self.template_name.clone_from(&desc.template_name);
        }
        if let Some(parent) = parent {
            let inverse = invert_csys(parent, false);
            self.pos = transform(&inverse, &self.pos);
            let rotation: Mat9 = inverse[..9].try_into().expect("nine words");
            self.orient = mul_matrix(&rotation, &self.orient);
        }
        self.load_pos = self.pos;
        self.load_orient = self.orient;
        let own = desc.flags & 1 == 0;
        let mut scene_changed = false;
        match (self.owner_type, &desc.tail) {
            (entity_kind::PSYS, EntityTail::Psys(s)) if own && desc.kind == entity_kind::PSYS => {
                self.snap_psys = *s;
            }
            (entity_kind::SCENE | entity_kind::SUB_SCENE, tail)
                if own && desc.kind == entity_kind::SCENE =>
            {
                if let Some(words) = scene_words(tail) {
                    self.snap_scene = words;
                    scene_changed = true;
                }
            }
            (entity_kind::SOUND, tail) => {
                if let EntityTail::Sound(a) = tail
                    && own
                    && desc.kind == entity_kind::SOUND
                {
                    self.snap_audio = a.clone();
                }
                self.name.clone_from(&desc.name);
                self.template_name.clone_from(&desc.template_name);
            }
            (entity_kind::MOTION_PATH, EntityTail::MotionPath(m))
                if own && desc.kind == entity_kind::MOTION_PATH =>
            {
                // An empty type keeps the empty string; null clears it.
                self.path_type.clone_from(&m.path_type);
            }
            (entity_kind::LIGHT, EntityTail::Light(l))
                if own && desc.kind == entity_kind::LIGHT =>
            {
                self.snap_light = l.clone();
            }
            (entity_kind::COMPOUND | entity_kind::DEFORMABLE, EntityTail::FloorHeight(h))
                if own && matches!(desc.kind, entity_kind::COMPOUND | entity_kind::DEFORMABLE) =>
            {
                self.snap_floor_height = *h;
            }
            (entity_kind::CAMERA | entity_kind::MONITOR, tail) => {
                // The caller unlinks the monitors first (`UnbindMonitorChildren`).
                if let EntityTail::Camera {
                    fovh,
                    near_plane,
                    far_plane,
                    aspect,
                } = tail
                    && own
                    && desc.kind == entity_kind::CAMERA
                {
                    self.snap_camera = [*fovh, *near_plane, *far_plane, *aspect];
                }
            }
            _ => {}
        }
        scene_changed
    }

    pub(super) fn has(&self, bit: u32) -> bool {
        self.bound & bit != 0
    }

    pub(super) fn ctl(&mut self, bit: u32) -> Option<&mut (dyn EntityControls + 'static)> {
        if self.bound & bit == 0 {
            return None;
        }
        self.controls.as_deref_mut()
    }

    /// `GetbaselineRelativePosition`: the live position from the
    /// transform control, into the parent frame; without the control the
    /// position is left as it is.
    pub(super) fn relative_position(&mut self, parent: &Csys) -> Vec3 {
        let mut p = self.pos;
        if let Some(c) = self.ctl(interface::GEO) {
            c.get_position(&mut p);
            self.pos = transform(&invert_csys(parent, false), &p);
        }
        self.pos
    }

    /// `GetbaselineRelativeOrientation`.
    pub(super) fn relative_orientation(&mut self, parent: &Csys) -> Mat9 {
        let mut m = self.orient;
        if let Some(c) = self.ctl(interface::GEO) {
            c.get_orientation(&mut m);
            let inverse = invert_csys(parent, false);
            let rotation: Mat9 = inverse[..9].try_into().expect("nine words");
            self.orient = mul_matrix(&rotation, &m);
        }
        self.orient
    }

    /// `GetLivePose`: the pose from the transform control, else the
    /// local pose.
    pub(super) fn live_pose(&mut self) -> (Vec3, Mat9) {
        let mut pos = self.pos;
        let mut orient = self.orient;
        if let Some(c) = self.ctl(interface::GEO) {
            c.get_position(&mut pos);
            c.get_orientation(&mut orient);
        }
        (pos, orient)
    }

    /// `CaptureAudioProps`.
    pub(super) fn capture_audio(&mut self) -> AudioProps {
        let mut audio = self.audio.clone();
        if let Some(c) = self.ctl(interface::SOUND) {
            c.get_audio_props(&mut audio, -1);
        }
        self.audio = audio;
        self.audio.clone()
    }

    /// `ApplyAudioProps`.
    pub(super) fn apply_audio(&mut self, props: &AudioProps) {
        self.audio = props.clone();
        let audio = self.audio.clone();
        if let Some(c) = self.ctl(interface::SOUND) {
            c.set_audio_props(&audio, -1);
        }
    }

    /// `ApplyPsysSparam`.
    pub(super) fn apply_sparam(&mut self, value: f32) {
        self.psys_sparam = value;
        if let Some(c) = self.ctl(interface::PSYS) {
            c.psys_set_sparam(value);
        }
    }

    /// `CaptureCameraProps`: the control's values into the runtime cache,
    /// then the cache.
    pub(super) fn capture_camera(&mut self) -> [f32; 4] {
        let mut values = self.camera_runtime;
        if let Some(c) = self.ctl(interface::CAMERA) {
            c.get_fov(&mut values[0]);
            c.get_near(&mut values[1]);
            c.get_far(&mut values[2]);
            c.get_aspect(&mut values[3]);
        }
        self.camera_runtime = values;
        values
    }

    /// `CaptureLiveLightProps`: a LIGHT reads its controls into its light
    /// words (the compact control's words converted); a SCENE reads its
    /// colour into the ambient words; others give zeros.
    pub(super) fn capture_light(&mut self) -> LightProps {
        match self.kind {
            entity_kind::LIGHT => {
                let mut out = self.light.clone();
                if self.has(interface::D3D_LIGHT) {
                    let c = self.controls.as_deref_mut().expect("bound");
                    c.d3d_get_light(&mut out);
                    c.d3d_get_on(&mut out.on);
                    c.d3d_get_direction(&mut out.direction);
                } else if self.has(interface::LIGHT) {
                    let c = self.controls.as_deref_mut().expect("bound");
                    c.light_get_color(&mut out.color);
                    c.light_get_direction(&mut out.direction);
                    c.light_get_range(&mut out.range_temp);
                    c.light_get_cutoff(&mut out.cutoff_temp);
                    c.light_get_slot14(&mut out.infinite);
                    c.light_get_slot13(&mut out.on);
                    convert_compact_light(&mut out);
                }
                self.light = out.clone();
                out
            }
            entity_kind::SCENE => {
                let mut out = LightProps {
                    color: self.scene_ambient,
                    ..LightProps::default()
                };
                if let Some(c) = self.ctl(interface::LIGHT) {
                    let mut color = out.color;
                    c.light_get_color(&mut color);
                    self.scene_ambient = color;
                    out.color = color;
                }
                out
            }
            _ => LightProps::default(),
        }
    }

    /// `UnpackEntityLightProps` for the one value later read back: a
    /// scene's ambient colour.
    pub(super) fn unpack_light(&mut self, props: &LightProps) {
        if matches!(self.kind, entity_kind::SCENE | entity_kind::SUB_SCENE) {
            self.scene_ambient = props.color;
        }
    }

    /// `ApplyEntityLightRuntimeProps`.
    pub(super) fn apply_light_runtime(&mut self, props: &LightProps) {
        self.light = props.clone();
        let live = self.light.clone();
        if let Some(c) = self.ctl(interface::D3D_LIGHT) {
            c.d3d_set_on(live.on);
            c.d3d_set_direction(live.direction);
            c.d3d_set_light(&live);
        }
        if let Some(c) = self.ctl(interface::LIGHT) {
            let color = [
                color_to_int(live.diffuse[0]),
                color_to_int(live.diffuse[1]),
                color_to_int(live.diffuse[2]),
            ];
            c.light_set_color(color);
            c.light_set_direction(live.direction);
            c.light_set_range(live.range);
            if live.light_type == 1 {
                c.light_set_cutoff(-1.0);
            } else {
                c.light_set_cutoff(round(ext(live.cutoff) * ext(f32::from_bits(0x3c8e_fa35))));
            }
            c.light_set_is_spot(i32::from(live.light_type == 3));
            c.light_set_on(live.on);
        }
    }

    /// `Fun06f43140`: publishes the fog words to the fog control by mask.
    pub(super) fn publish_fog(&mut self, mask: u8) {
        let p = self.scene_packed;
        let Some(c) = self.ctl(interface::FOG) else {
            return;
        };
        let bytes = p[7].to_le_bytes();
        if mask & 1 != 0 {
            c.fog_set_on(bytes[0]);
        }
        if mask & 0xa != 0 {
            c.fog_set_table_mode(bytes[1], p[11]);
        }
        if mask & 0x70 != 0 {
            c.fog_set_range(
                f32::from_bits(p[12]),
                f32::from_bits(p[13]),
                f32::from_bits(p[14]),
            );
        }
        if mask & 4 != 0 {
            c.fog_set_color([p[8] as i32, p[9] as i32, p[10] as i32]);
        }
    }

    /// `SetUserProp`.
    pub(super) fn set_user_prop(&mut self, key: &[u8], value: &[u8]) {
        match self
            .user_props
            .binary_search_by(|(k, _)| user_prop_cmp(k, key))
        {
            Ok(at) => self.user_props[at].1 = value.to_vec(),
            Err(at) => self.user_props.insert(at, (key.to_vec(), value.to_vec())),
        }
    }

    /// `UserProp`.
    pub(super) fn user_prop(&self, key: &[u8]) -> Option<&[u8]> {
        self.user_props
            .binary_search_by(|(k, _)| user_prop_cmp(k, key))
            .ok()
            .map(|at| self.user_props[at].1.as_slice())
    }
}

/// The entities and the scene state their operations touch.
pub(super) struct World {
    pub(super) entities: BTreeMap<i32, Entity>,
    /// `SceneForwardCsys_`, every entity's and event's parent frame.
    pub(super) csys: Csys,
    /// `SceneUpAxis_`, `SceneFrontAxis_`.
    pub(super) scene_up: i32,
    pub(super) scene_front: i32,
}

/// Recursion through monitor links stops here; the reference recurses
/// until the stack overflows on a cycle of cameras (divergence D247).
const LINK_DEPTH: u32 = 64;

impl World {
    pub(super) fn get(&mut self, key: i32) -> Option<&mut Entity> {
        self.entities.get_mut(&key)
    }

    /// `InnerSetPos`: stores the position; publishes it in world space to
    /// the transform control; a camera (not a monitor) passes it on to its
    /// linked monitors.
    pub(super) fn set_pos(&mut self, key: i32, pos: Vec3) {
        self.set_pos_depth(key, pos, 0);
    }

    fn set_pos_depth(&mut self, key: i32, pos: Vec3, depth: u32) {
        let csys = self.csys;
        let Some(e) = self.entities.get_mut(&key) else {
            return;
        };
        e.pos = pos;
        if let Some(c) = e.ctl(interface::GEO) {
            c.set_position(transform(&csys.words(), &pos));
        }
        if matches!(e.owner_type, entity_kind::CAMERA | entity_kind::MONITOR)
            && e.kind != entity_kind::MONITOR
            && depth < LINK_DEPTH
        {
            for link in e.monitor_links.clone() {
                self.set_pos_depth(link, pos, depth + 1);
            }
        }
    }

    /// `InnerSetOrient`.
    pub(super) fn set_orient(&mut self, key: i32, orient: Mat9) {
        self.set_orient_depth(key, orient, 0);
    }

    fn set_orient_depth(&mut self, key: i32, orient: Mat9, depth: u32) {
        let csys = self.csys;
        let Some(e) = self.entities.get_mut(&key) else {
            return;
        };
        e.orient = orient;
        if let Some(c) = e.ctl(interface::GEO) {
            c.set_orientation(mul_matrix(&csys.orient, &orient));
        }
        if matches!(e.owner_type, entity_kind::CAMERA | entity_kind::MONITOR)
            && e.kind != entity_kind::MONITOR
            && depth < LINK_DEPTH
        {
            for link in e.monitor_links.clone() {
                self.set_orient_depth(link, orient, depth + 1);
            }
        }
    }

    /// `ApplyCameraProps`: the runtime cache, the camera control (fov,
    /// aspect, near, far), then the linked monitors.
    pub(super) fn apply_camera(&mut self, key: i32, values: [f32; 4]) {
        self.apply_camera_depth(key, values, 0);
    }

    fn apply_camera_depth(&mut self, key: i32, values: [f32; 4], depth: u32) {
        let Some(e) = self.entities.get_mut(&key) else {
            return;
        };
        e.camera_runtime = values;
        let v = e.camera_runtime;
        if let Some(c) = e.ctl(interface::CAMERA) {
            c.set_fov(v[0]);
            c.set_aspect(v[3]);
            c.set_near(v[1]);
            c.set_far(v[2]);
        }
        if e.kind != entity_kind::MONITOR && depth < LINK_DEPTH {
            for link in e.monitor_links.clone() {
                self.apply_camera_depth(link, v, depth + 1);
            }
        }
    }

    /// `Fun06f431d0`: the scene words; once the entity is bound to the
    /// scene, words 5 and 6 become the scene's up and front axes; with
    /// mask bit 0 the light control gets the colour.
    pub(super) fn publish_scene(&mut self, key: i32, words: [u32; 15], mask: u8) {
        let Some(e) = self.entities.get_mut(&key) else {
            return;
        };
        e.scene_packed = words;
        self.scene_up = words[5] as i32;
        self.scene_front = words[6] as i32;
        if mask & 1 != 0
            && let Some(c) = e.ctl(interface::LIGHT)
        {
            c.light_set_color([words[0] as i32, words[1] as i32, words[2] as i32]);
        }
    }

    /// `AddMonitorLink`: appends `other` unless present.
    pub(super) fn add_link(&mut self, key: i32, other: i32) -> bool {
        let Some(e) = self.entities.get_mut(&key) else {
            return false;
        };
        if e.monitor_links.contains(&other) {
            return false;
        }
        e.monitor_links.push(other);
        true
    }

    /// `RemoveMonitorLink`: the first match.
    fn remove_link(&mut self, key: i32, other: i32) {
        if let Some(e) = self.entities.get_mut(&key)
            && let Some(at) = e.monitor_links.iter().position(|&l| l == other)
        {
            e.monitor_links.remove(at);
        }
    }

    /// `UnbindMonitorChildren`: for each link, the reciprocal link goes and
    /// so does this one; the walk then continues after the node that
    /// followed, skipping it, as the reference's does. A link of an entity
    /// to itself leaves the reference with a dangling node (divergence
    /// D247); here it is removed once.
    pub(super) fn unbind_monitor_children(&mut self, key: i32) {
        let mut at = 0usize;
        loop {
            let Some(links) = self.entities.get(&key).map(|e| e.monitor_links.clone()) else {
                return;
            };
            if at >= links.len() {
                return;
            }
            let other = links[at];
            if other != key {
                self.remove_link(other, key);
            }
            let Some(e) = self.entities.get_mut(&key) else {
                return;
            };
            let Some(now) = e.monitor_links.iter().position(|&l| l == other) else {
                return;
            };
            let next_was_sentinel = now + 1 >= e.monitor_links.len();
            e.monitor_links.remove(now);
            at = if next_was_sentinel { 0 } else { now + 1 };
        }
    }

    /// `FrameUpdateEntities`' per-entity step.
    pub(super) fn advance(&mut self, key: i32, delta: i32) {
        let Some(e) = self.entities.get_mut(&key) else {
            return;
        };
        match e.kind {
            entity_kind::COMPOUND | entity_kind::DEFORMABLE => {
                if let Some(c) = e.ctl(interface::COMPOUND) {
                    c.advance_time(delta);
                }
            }
            entity_kind::SUB_SCENE => {
                if let Some(c) = e.ctl(interface::SCRIPT) {
                    c.script_tick(delta);
                }
            }
            entity_kind::PSYS => {
                if let Some(c) = e.ctl(interface::PSYS) {
                    c.psys_tick(delta);
                }
            }
            _ => {}
        }
    }

    /// The entity pose in world space (`IScriptEngine` slots 10, 11): the
    /// saved descriptor pose through the scene frame.
    pub(super) fn world_pose(&self, key: i32) -> Option<(Vec3, Mat9)> {
        let e = self.entities.get(&key)?;
        let pos = transform(&self.csys.words(), &e.load_pos);
        let orient = math::mul_matrix(&self.csys.orient, &e.load_orient);
        Some((pos, orient))
    }
}
