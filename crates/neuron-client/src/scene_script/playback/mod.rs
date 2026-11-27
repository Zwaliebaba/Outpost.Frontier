//! Scene playback: the scene object of `SceneScriptScene.cpp`
//! (`FSceneScriptScene`) with the parts of the script engine
//! (`FScriptEngine` of `SceneScript.cpp`) that drive it: loading the
//! parser's output, attaching engine objects to entities, placing the
//! scene, and the timeline.
//!
//! The timeline keeps the reference's three event lists: every event
//! sorted by start time (`MASTER`), the events still to start (`PENDING`)
//! and the running ones (`ACTIVE`), grouped so that an event's secondary
//! entity is updated before it. Each tick starts the events whose start
//! times the clock passed (stopping, at their end times, the events that
//! ended on the way), updates the running ones, and lets compound, sub-
//! scene and particle entities advance. Engine effects go through the
//! [`EntityControls`] attached to an entity; an entity without one keeps
//! its pose and properties as data, as the reference does.
//!
//! Module layout: [`controls`] holds the engine interfaces, `entity` the
//! runtime entities, `event` the runtime events and their per-type
//! start, tick and stop, `math` the arithmetic helpers.

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true
// exactly when the reference takes that side, which needs negated
// comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod controls;
mod entity;
mod event;
mod math;
mod save;

use std::collections::{BTreeMap, BTreeSet};

pub use controls::{EntityControls, EventRef, interface};
pub use math::Csys;

/// An entity's descriptor as the scene holds it (`FSceneScriptEntitySnap`
/// behind `Slot9Entity`): the fields of the last applied descriptor, the
/// pose in the scene's frame and the tail of each kind.
#[derive(Clone, Copy, Debug)]
pub struct EntitySnap<'a> {
    pub kind: i32,
    pub flags: i32,
    pub pos: [f32; 3],
    /// Row-major.
    pub orient: [f32; 9],
    pub name: Option<&'a [u8]>,
    pub template_name: Option<&'a [u8]>,
    pub lt_grp: u8,
    pub srt_grp: u8,
    pub usr_flg: u8,
    /// COMPOUND and DEFORMABLE (`+0x44`).
    pub floor_height: f32,
    /// CAMERA and MONITOR: fov, near, far, aspect.
    pub camera: [f32; 4],
    /// LIGHT.
    pub light: &'a super::entity::LightProps,
    /// SCENE and SUB_SCENE: the fifteen words, the ambient colour first.
    pub scene: [u32; 15],
}

use entity::{Entity, PathBinding, World, bind_set};
use event::Event;

use super::build::{Scene, Step};
use super::entity::{EntityDesc, EntityTail, kind as entity_kind};
use super::event::{EventCommon, EventDesc, EventPayload, kind as event_kind};
use super::script_engine::ComContext;
use super::spline::SplinePath;

/// The playback states (`kSceneScriptPlayback*`): 0 before anything is
/// loaded.
pub mod state {
    pub const LOADED: i32 = 2;
    pub const RESET: i32 = 3;
    pub const DONE: i32 = 4;
    pub const PAUSED: i32 = 5;
    pub const PLAY: i32 = 6;
    pub const LOOP: i32 = 7;
}

/// `GR_OK` and `GR_GENERIC`.
const OK: i32 = 0;
const GENERIC: i32 = -1;

/// A running event, for inspection.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ActiveEvent {
    pub id: i32,
    pub kind: i32,
    pub start_ms: i32,
    pub primary: i32,
    pub secondary: i32,
}

/// A loaded scene and its timeline.
pub struct Player {
    world: World,
    /// `EntityCount_`: the next entity handle.
    entity_count: i32,
    events: Vec<Option<Event>>,
    master: Vec<usize>,
    pending: Vec<usize>,
    active: Vec<usize>,
    /// The end times still to stop at, sorted.
    stop_times: Vec<f32>,
    /// Compound, deformable, sub-scene and particle entities, in the order
    /// they were added.
    frame_entities: Vec<i32>,
    duration: i32,
    clock: i32,
    clock_after_dt: i32,
    state: i32,
    looping: bool,
    loop_carry: i32,
    next_event_id: i32,
    /// The script engine's cached inverse of the scene frame
    /// (`SceneCsys_`), the identity until the frame first moves.
    scene_inverse: math::Xform,
}

impl Default for Player {
    fn default() -> Self {
        Player::new()
    }
}

impl Player {
    /// An empty scene at the origin.
    #[must_use]
    pub fn new() -> Player {
        Player {
            world: World {
                entities: BTreeMap::new(),
                csys: Csys::IDENTITY,
                scene_up: 1,
                scene_front: 2,
            },
            entity_count: 0,
            events: Vec::new(),
            master: Vec::new(),
            pending: Vec::new(),
            active: Vec::new(),
            stop_times: Vec::new(),
            frame_entities: Vec::new(),
            duration: 0,
            clock: 0,
            clock_after_dt: 0,
            state: 0,
            looping: false,
            loop_carry: 0,
            next_event_id: 0,
            scene_inverse: math::pack(&math::IDENTITY9, &[0.0; 3]),
        }
    }

    /// `FScriptEngine::Slot4Load` after a successful first pass: clears a
    /// loaded scene, applies the parser's calls, sorts the events, and
    /// adds the default SET_CAMERA event when the scene has exactly one
    /// monitor and one camera and no SET_CAMERA event of its own.
    pub fn load(&mut self, scene: &Scene, duration: u32) {
        self.load_with_com(scene, duration, None);
    }

    /// [`Self::load`] for an engine with a COM context (`Slot40SetCom`):
    /// after the parser adds a user event it hands the event's text to the
    /// context (`ISceneScriptComContext::Slot3`).
    pub fn load_with_com(
        &mut self,
        scene: &Scene,
        duration: u32,
        mut com: Option<&mut dyn ComContext>,
    ) {
        if self.state != 0 {
            self.clear();
        }
        self.duration = duration as i32;
        for step in &scene.steps {
            match step {
                Step::AddEntity(desc) => {
                    self.add_entity(desc);
                }
                Step::SetUserProp { entity, key, value } => {
                    if let Some(key) = key {
                        self.set_user_prop(*entity, key, value);
                    }
                }
                Step::UpdateEntity { entity, desc } => {
                    self.update_entity(*entity, desc);
                }
                Step::AddEvent(desc) => {
                    let id = self.add_event(desc);
                    // The parser hands the text on whether the engine took
                    // the event or not (then with the id -1).
                    if let (EventPayload::User { text, .. }, Some(com)) =
                        (&desc.payload, com.as_mut())
                    {
                        com.set_user_event_string(EventRef { id, desc }, text.as_deref());
                    }
                }
            }
        }
        self.finish_load(duration);
    }

    /// The end of `Slot4Load` after the parser's second pass, whichever
    /// parser ran it: the scene marked loaded (`MarkLoaded`), the duration
    /// stored and the events sorted (`FinalizeLoad`), and the default
    /// SET_CAMERA event added when the scene has exactly one monitor and
    /// one camera and no SET_CAMERA event of its own.
    pub fn finish_load(&mut self, duration: u32) {
        self.state = state::LOADED;
        self.duration = duration as i32;
        sort_master(&mut self.master, &self.events);
        let (mut monitors, mut cameras, mut monitor, mut camera) = (0, 0, 0, 0);
        for (ordinal, e) in self.world.entities.values().enumerate() {
            if e.kind == entity_kind::MONITOR {
                monitors += 1;
                monitor = ordinal as i32;
            } else if e.kind == entity_kind::CAMERA {
                cameras += 1;
                camera = ordinal as i32;
            }
        }
        if monitors == 1 && cameras == 1 && !self.has_event_kind(event_kind::SET_CAMERA) {
            // The reference constructs a temporary event first, which
            // takes an id.
            self.next_event_id += 1;
            self.add_event(&EventDesc {
                common: EventCommon {
                    kind: event_kind::SET_CAMERA,
                    start_ms: 0,
                    flags: 0,
                    event_flags: 0,
                    duration_ms: 0,
                    primary: monitor,
                    secondary: camera,
                    param_curve: None,
                    pcurve_period: 0,
                },
                payload: EventPayload::SetCamera,
            });
        }
    }

    /// `Clear`: stops what runs and removes every event and entity; the
    /// clocks stay.
    pub fn clear(&mut self) {
        self.stop_all();
        self.frame_entities.clear();
        self.pending.clear();
        for key in std::mem::take(&mut self.master) {
            self.events[key] = None;
        }
        self.world.entities.clear();
        self.entity_count = 0;
        self.duration = 0;
        self.state = 0;
    }

    /// `AddEntityFromDesc` (`Slot35AddEntity`): the entity's handle, or
    /// `None` for a type outside 1 to 13 or 12.
    pub fn add_entity(&mut self, desc: &EntityDesc) -> Option<i32> {
        if !(1..=13).contains(&desc.kind) || desc.kind == 12 {
            return None;
        }
        let entity = Entity::new(desc);
        if matches!(
            entity.owner_type,
            entity_kind::COMPOUND
                | entity_kind::DEFORMABLE
                | entity_kind::SUB_SCENE
                | entity_kind::PSYS
        ) {
            self.frame_entities.push(self.entity_count);
        }
        if entity.owner_type == entity_kind::SCENE {
            self.world.scene_up = entity.snap_scene[5] as i32;
            self.world.scene_front = entity.snap_scene[6] as i32;
        }
        let handle = self.entity_count;
        self.world.entities.entry(handle).or_insert(entity);
        self.entity_count = self.entity_count.wrapping_add(1);
        if self.state == 0 {
            self.state = state::LOADED;
        }
        Some(handle)
    }

    fn exists(&self, index: i32) -> bool {
        (index as u32) < (self.entity_count as u32) && self.world.entities.contains_key(&index)
    }

    /// `UpdateEntityFromDesc` (`Slot37UpdateEntity`).
    pub fn update_entity(&mut self, index: i32, desc: &EntityDesc) -> i32 {
        if !self.exists(index) {
            return GENERIC;
        }
        let csys = self.world.csys;
        let e = self.world.get(index).expect("checked");
        if matches!(e.owner_type, entity_kind::CAMERA | entity_kind::MONITOR) {
            self.world.unbind_monitor_children(index);
        }
        let e = self.world.get(index).expect("checked");
        let scene_changed = e.apply_snap(desc, Some(&csys));
        // `ApplyMotionPathDesc`: the descriptor's path object replaces the
        // one the entity's own descriptor holds; the path the entity
        // evaluates stays the one it was made with.
        if e.owner_type == entity_kind::MOTION_PATH
            && desc.flags & 1 == 0
            && desc.kind == entity_kind::MOTION_PATH
            && let EntityTail::MotionPath(m) = &desc.tail
        {
            e.snap_path = Some(m.path.as_ref().map(SplinePath::from_desc));
        }
        if scene_changed {
            let words = e.snap_scene;
            self.world.publish_scene(index, words, 0xff);
        }
        OK
    }

    /// `SetUserProp` (`Slot34SetUserProp`).
    pub fn set_user_prop(&mut self, index: i32, key: &[u8], value: &[u8]) -> i32 {
        if !self.exists(index) {
            return GENERIC;
        }
        self.world
            .get(index)
            .expect("checked")
            .set_user_prop(key, value);
        OK
    }

    /// `UserProp` (`Slot33UserProp`).
    #[must_use]
    pub fn user_prop(&self, index: i32, key: &[u8]) -> Option<&[u8]> {
        if !self.exists(index) {
            return None;
        }
        self.world.entities[&index].user_prop(key)
    }

    /// `UserPropCount` and `UserPropKey` (`Slot31`, `Slot32`): the
    /// properties in the reference's key order.
    #[must_use]
    pub fn user_props(&self, index: i32) -> &[(Vec<u8>, Vec<u8>)] {
        self.world
            .entities
            .get(&index)
            .map_or(&[], |e| e.user_props.as_slice())
    }

    /// `AddEventFromDesc` (`Slot38AddEvent`): the event's id (numbered from
    /// 0 per scene; divergence D249), or -1 when
    /// its entities do not exist (a negative secondary is allowed except
    /// for SET_CAMERA, PATH_ANIM, ATTACH_ENTITY, CONNECT_HARDPOINTS and
    /// START_IK) or its type is outside 1 to 19.
    pub fn add_event(&mut self, desc: &EventDesc) -> i32 {
        let common = &desc.common;
        if !self.exists(common.primary) {
            return -1;
        }
        if common.secondary < 0 {
            if matches!(
                common.kind,
                event_kind::SET_CAMERA
                    | event_kind::PATH_ANIM
                    | event_kind::ATTACH_ENTITY
                    | event_kind::CONNECT_HARDPOINTS
                    | event_kind::START_IK
            ) {
                return -1;
            }
        } else if !self.exists(common.secondary) {
            return -1;
        }
        if !(1..=19).contains(&common.kind) {
            return -1;
        }
        let id = self.next_event_id;
        self.next_event_id = self.next_event_id.wrapping_add(1);
        let event = Event::new(id, desc);
        let start = event.start_ms() as u32;
        let key = self.events.len();
        self.events.push(Some(event));
        let at = self
            .master
            .iter()
            .position(|&k| self.ev(k).start_ms() as u32 > start)
            .unwrap_or(self.master.len());
        self.master.insert(at, key);
        id
    }

    fn ev(&self, key: usize) -> &Event {
        self.events[key].as_ref().expect("live event")
    }

    fn has_event_kind(&self, kind: i32) -> bool {
        self.master.iter().any(|&k| self.ev(k).kind() == kind)
    }

    /// `RemoveEventById` (`Slot39RemoveEvent`).
    pub fn remove_event(&mut self, id: i32) -> i32 {
        match self.master.iter().position(|&k| self.ev(k).id == id) {
            Some(at) => {
                self.erase_master(at);
                OK
            }
            None => GENERIC,
        }
    }

    /// `EraseMasterEventAt`: out of the master list, stopped where it is
    /// active, out of the pending list (first match).
    fn erase_master(&mut self, at: usize) {
        let key = self.master.remove(at);
        let mut slot = 0;
        while slot < self.active.len() {
            if self.active[slot] == key {
                self.with_event(key, |e, w| e.stop(w));
                self.active.remove(slot);
            } else {
                slot += 1;
            }
        }
        if let Some(p) = self.pending.iter().position(|&k| k == key) {
            self.pending.remove(p);
        }
        self.events[key] = None;
    }

    /// `RemoveEntityAt` (`Slot36RemoveEntity`): the entity and every event
    /// that names it.
    pub fn remove_entity(&mut self, index: i32) -> i32 {
        if !self.exists(index) {
            return GENERIC;
        }
        let mut at = 0;
        while at < self.master.len() {
            let e = self.ev(self.master[at]);
            if e.primary() == index || e.secondary() == index {
                self.erase_master(at);
            } else {
                at += 1;
            }
        }
        self.frame_entities.retain(|&k| k != index);
        if let Some(e) = self.world.get(index)
            && matches!(e.owner_type, entity_kind::CAMERA | entity_kind::MONITOR)
        {
            self.world.unbind_monitor_children(index);
        }
        self.world.entities.remove(&index);
        OK
    }

    /// `Attach` (`Slot12Attach`): binds the interfaces of `controls` that
    /// the entity's type uses. 0 when it answers at least one, else -1;
    /// it is kept either way. A SCENE entity whose axes are unset (6) gets
    /// front -Z (5) and up +Y (1).
    pub fn attach(&mut self, index: i32, controls: Box<dyn EntityControls>) -> i32 {
        if index as u32 >= self.entity_count as u32 {
            return GENERIC;
        }
        let Some(e) = self.world.get(index) else {
            return GENERIC;
        };
        e.bound = controls.interfaces() & bind_set(e.owner_type);
        e.controls = Some(controls);
        if e.owner_type == entity_kind::MOTION_PATH {
            e.path_binding = if e.has(interface::MOTION_PATH) {
                PathBinding::Host
            } else {
                PathBinding::None
            };
        }
        if matches!(e.owner_type, entity_kind::SCENE | entity_kind::SUB_SCENE) {
            if e.snap_scene[6] == 6 {
                e.snap_scene[6] = 5;
                e.scene_packed[6] = 5;
            }
            if e.snap_scene[5] == 6 {
                e.snap_scene[5] = 1;
                e.scene_packed[5] = 1;
            }
        }
        if e.bound != 0 { OK } else { GENERIC }
    }

    /// `Detach` (`Slot13Detach`): releases the bound interfaces. A scene
    /// keeps its reverb control, as the reference's unbind does.
    pub fn detach(&mut self, index: i32) -> i32 {
        if index as u32 >= self.entity_count as u32 {
            return GENERIC;
        }
        let Some(e) = self.world.get(index) else {
            return GENERIC;
        };
        let owner = e.owner_type;
        if matches!(owner, entity_kind::CAMERA | entity_kind::MONITOR) {
            self.world.unbind_monitor_children(index);
        }
        let e = self.world.get(index).expect("checked");
        if matches!(owner, entity_kind::SCENE | entity_kind::SUB_SCENE) {
            e.bound &= interface::REVERB;
        } else {
            e.bound = 0;
        }
        if e.bound == 0 {
            e.controls = None;
        }
        if owner == entity_kind::MOTION_PATH {
            e.path_binding = PathBinding::None;
        }
        OK
    }

    /// `IGeoTransformControl::SetPosition` of the script engine: moves the
    /// scene frame and every placed entity with it.
    pub fn set_position(&mut self, position: [f32; 3]) {
        let old = self.scene_inverse;
        self.world.csys.pos = position;
        self.scene_inverse = math::invert_csys(&self.world.csys, true);
        self.push_geo(&old, false);
    }

    /// `IGeoTransformControl::SetOrientation` of the script engine.
    pub fn set_orientation(&mut self, orientation: [f32; 9]) {
        let old = self.scene_inverse;
        self.world.csys.orient = orientation;
        self.scene_inverse = math::invert_csys(&self.world.csys, true);
        self.push_geo(&old, true);
    }

    /// The scene frame.
    #[must_use]
    pub fn frame(&self) -> Csys {
        self.world.csys
    }

    /// `PushBoundGeoFromScene`: each entity with a transform control reads
    /// its world pose, takes it into the old frame's local space and sets
    /// it again through the new frame.
    fn push_geo(&mut self, old_inverse: &math::Xform, orient: bool) {
        let keys: Vec<i32> = self.world.entities.keys().copied().collect();
        for key in keys {
            let e = self.world.get(key).expect("listed");
            let mut pos = e.pos;
            let mut m = e.orient;
            let Some(c) = e.ctl(interface::GEO) else {
                continue;
            };
            c.get_position(&mut pos);
            if orient {
                c.get_orientation(&mut m);
            }
            let local = math::transform(old_inverse, &pos);
            self.world.set_pos(key, local);
            if orient {
                let rotation: math::Mat9 = old_inverse[..9].try_into().expect("nine words");
                self.world.set_orient(key, math::mul_matrix(&rotation, &m));
            }
        }
    }

    /// `GetEntityPos` and `GetEntityOrient` (`Slot10`, `Slot11`): the
    /// pose of the entity's last descriptor in world space.
    #[must_use]
    pub fn entity_pose(&self, index: i32) -> Option<([f32; 3], [f32; 9])> {
        if !self.exists(index) {
            return None;
        }
        self.world.world_pose(index)
    }

    /// The entity's live pose in the scene frame (`Snap.Pos`,
    /// `Snap.Orient`).
    #[must_use]
    pub fn entity_local_pose(&self, index: i32) -> Option<([f32; 3], [f32; 9])> {
        self.world.entities.get(&index).map(|e| (e.pos, e.orient))
    }

    /// The kind of an entity's owner (`entity_kind`), which decides the
    /// interfaces an attached object binds.
    #[must_use]
    pub fn entity_owner_type(&self, index: i32) -> Option<i32> {
        self.world.entities.get(&index).map(|e| e.owner_type)
    }

    /// `Slot9Entity` (`EntitySnap`): the descriptor an entity holds now,
    /// `None` for a handle without an entity.
    #[must_use]
    pub fn entity_snap(&self, index: i32) -> Option<EntitySnap<'_>> {
        let e = self.world.entities.get(&index)?;
        Some(EntitySnap {
            kind: e.kind,
            flags: e.flags,
            pos: e.pos,
            orient: e.orient,
            name: e.name.as_deref(),
            template_name: e.template_name.as_deref(),
            lt_grp: e.groups[0],
            srt_grp: e.groups[1],
            usr_flg: e.groups[2],
            floor_height: e.snap_floor_height,
            camera: e.snap_camera,
            light: &e.snap_light,
            scene: e.snap_scene,
        })
    }

    /// The handles of the entities, in order (`Slot7EntityHandle`).
    pub fn entity_handles(&self) -> impl Iterator<Item = i32> + '_ {
        self.world.entities.keys().copied()
    }

    /// `Slot6EntityCount`.
    #[must_use]
    pub fn live_entity_count(&self) -> u32 {
        self.world.entities.len() as u32
    }

    /// `Slot14EventCount`.
    #[must_use]
    pub fn event_count(&self) -> u32 {
        self.master.len() as u32
    }

    /// The events in master order, with their ids.
    pub fn events(&self) -> impl Iterator<Item = (i32, &EventDesc)> + '_ {
        self.master.iter().map(|&k| {
            let e = self.ev(k);
            (e.id, &e.desc)
        })
    }

    /// The running events, in update order.
    #[must_use]
    pub fn active(&self) -> Vec<ActiveEvent> {
        self.active
            .iter()
            .map(|&k| {
                let e = self.ev(k);
                ActiveEvent {
                    id: e.id,
                    kind: e.kind(),
                    start_ms: e.start_ms(),
                    primary: e.primary(),
                    secondary: e.secondary(),
                }
            })
            .collect()
    }

    /// `Slot17GetDuration`.
    #[must_use]
    pub fn duration(&self) -> i32 {
        self.duration
    }

    /// `Slot18SetDuration`.
    pub fn set_duration(&mut self, ms: i32) {
        self.duration = ms;
    }

    /// `Slot19GetClock`.
    #[must_use]
    pub fn clock(&self) -> i32 {
        self.clock
    }

    /// `Slot21GetState`.
    #[must_use]
    pub fn state(&self) -> i32 {
        self.state
    }

    /// `Slot20SetLoop`.
    pub fn set_loop(&mut self, looping: bool) {
        self.looping = looping;
    }

    fn with_event<R>(&mut self, key: usize, f: impl FnOnce(&mut Event, &mut World) -> R) -> R {
        let mut event = self.events[key].take().expect("live event");
        let result = f(&mut event, &mut self.world);
        self.events[key] = Some(event);
        result
    }

    /// `RebuildPending`.
    fn rebuild_pending(&mut self) {
        self.pending.clone_from(&self.master);
    }

    /// `StopAll`: forgets the stop times and stops every active event,
    /// first to last.
    fn stop_all(&mut self) {
        self.stop_times.clear();
        while let Some(&key) = self.active.first() {
            self.with_event(key, |e, w| e.stop(w));
            self.active.remove(0);
        }
    }

    /// `FrameUpdateEntities`: each frame entity advances by the time since
    /// the clock; then the clock is `time`.
    fn frame_update(&mut self, time: i32) {
        let delta = time.wrapping_sub(self.clock);
        let mut at = 0;
        while at < self.frame_entities.len() {
            let key = self.frame_entities[at];
            self.world.advance(key, delta);
            at += 1;
        }
        self.clock = time;
    }

    /// `UpdateActive`: the frame update, then each active event's tick;
    /// the ones that end are stopped and removed.
    fn update_active(&mut self, time: i32) {
        self.frame_update(time);
        let mut slot = 0;
        while slot < self.active.len() {
            let key = self.active[slot];
            if self.with_event(key, |e, w| e.still_running(w, time)) {
                slot += 1;
            } else {
                self.with_event(key, |e, w| e.stop(w));
                self.active.remove(slot);
            }
        }
    }

    /// `DrainTimedStops`: updates at each stop time before `time` (the
    /// start of the event about to start), or before the new clock.
    fn drain_stops(&mut self, time: i32, before_start: Option<&mut BTreeSet<i32>>) {
        match before_start {
            Some(updated) => {
                while let Some(&stop) = self.stop_times.first() {
                    if !(f64::from(stop) < f64::from(time as u32)) {
                        break;
                    }
                    let at = neuron_core::num::trunc_to_i32_via_i64(f64::from(stop));
                    self.update_active(at);
                    updated.insert(at);
                    self.stop_times.remove(0);
                }
            }
            None => {
                let rounded = time as f32;
                while let Some(&stop) = self.stop_times.first() {
                    if !(stop < rounded) {
                        break;
                    }
                    let at = neuron_core::num::trunc_to_i32_via_i64(f64::from(stop));
                    self.update_active(at);
                    self.stop_times.remove(0);
                }
            }
        }
    }

    /// `DispatchWindow`: starts the pending events whose start times lie
    /// in `[old, new]` (unsigned), in order. Before each, the stop times
    /// before its start are drained and the active events updated at its
    /// start, once per start time; after each, the active list is regrouped.
    fn dispatch_window(&mut self, old: i32, new: i32) {
        let start_of = |p: &Player, k: usize| p.ev(k).start_ms() as u32;
        let mut from = 0;
        while from < self.pending.len() && start_of(self, self.pending[from]) < old as u32 {
            from += 1;
        }
        if from >= self.pending.len() || start_of(self, self.pending[from]) > new as u32 {
            return;
        }
        let mut to = from;
        while to < self.pending.len() && start_of(self, self.pending[to]) <= new as u32 {
            to += 1;
        }
        let selected: Vec<usize> = self.pending[from..to].to_vec();
        let mut updated = BTreeSet::new();
        for key in selected {
            if !self.world.entities.contains_key(&self.ev(key).primary()) {
                continue;
            }
            let start = self.ev(key).start_ms();
            self.drain_stops(start, Some(&mut updated));
            if !updated.contains(&start) {
                self.update_active(start);
                updated.insert(start);
            }
            let time = self.clock_after_dt;
            if self.with_event(key, |e, w| e.start(w, time)) {
                self.active.push(key);
                let end = self.ev(key).end_ms;
                if end >= 0 {
                    let stop = end as f32;
                    self.stop_times.retain(|&s| s != stop);
                    self.stop_times.push(stop);
                    // A stable sort of values that are never NaN.
                    self.stop_times.sort_by(f32::total_cmp);
                }
            }
            self.rebuild_active_groups();
        }
        self.pending.drain(from..to);
    }

    /// `RebuildActiveGroups`: the active events grouped by primary entity
    /// (in handle order), each group sorted by type, with an event's
    /// secondary's group emitted before it.
    fn rebuild_active_groups(&mut self) {
        let mut groups: BTreeMap<i32, (bool, Vec<usize>)> = BTreeMap::new();
        for &key in &self.active {
            groups
                .entry(self.ev(key).primary())
                .or_insert((false, Vec::new()))
                .1
                .push(key);
        }
        self.active.clear();
        let primaries: Vec<i32> = groups.keys().copied().collect();
        for primary in primaries {
            self.emit_group(&mut groups, primary);
        }
    }

    fn emit_group(&mut self, groups: &mut BTreeMap<i32, (bool, Vec<usize>)>, primary: i32) {
        let Some(group) = groups.get_mut(&primary) else {
            return;
        };
        if group.0 {
            return;
        }
        group.0 = true;
        let mut events = std::mem::take(&mut group.1);
        sort_by_type(&mut events, &self.events);
        for &key in &events {
            let secondary = self.ev(key).secondary();
            if secondary != -1 && groups.contains_key(&secondary) {
                self.emit_group(groups, secondary);
            }
            self.active.push(key);
        }
        if let Some(group) = groups.get_mut(&primary) {
            group.1 = events;
        }
    }

    /// `StartPlayback` (`Slot22Start`).
    pub fn start(&mut self, arg: i32) -> i32 {
        match self.state {
            state::LOADED | state::RESET | state::DONE => {
                self.clock = 0;
                self.rebuild_pending();
            }
            state::LOOP => self.rebuild_pending(),
            state::PAUSED => {
                self.state = state::PLAY;
                self.tick(arg);
                return OK;
            }
            _ => {}
        }
        if self.looping {
            let delta = arg.wrapping_sub(self.loop_carry);
            self.state = state::LOOP;
            self.tick(delta);
            return OK;
        }
        self.state = state::PLAY;
        self.tick(arg);
        OK
    }

    /// `StopPlayback` (`Slot23Stop`).
    pub fn stop(&mut self) -> i32 {
        self.clock = 0;
        self.pending.clear();
        self.rebuild_pending();
        if self.state == state::LOOP {
            self.loop_carry = 0;
        }
        self.stop_all();
        self.state = state::RESET;
        OK
    }

    /// `PausePlayback` (`Slot24Pause`).
    pub fn pause(&mut self) -> i32 {
        if matches!(self.state, state::PLAY | state::LOOP) {
            self.state = state::PAUSED;
            OK
        } else {
            GENERIC
        }
    }

    /// `Tick` (`Slot26Tick`): advances the clock by `dt` (a negative step
    /// restarts from 0) and plays what lies between.
    pub fn tick(&mut self, dt: i32) -> i32 {
        let old = self.clock;
        let mut new = old.wrapping_add(dt);
        if dt < 0 {
            if new < 0 {
                new = 0;
            }
            self.stop_all();
            self.clock = 0;
            self.rebuild_pending();
        }
        let window_old = self.clock;
        self.clock_after_dt = new;
        if (new as u32) < (window_old as u32) {
            return OK;
        }
        if !matches!(self.state, state::PAUSED | state::PLAY | state::LOOP) {
            return OK;
        }
        self.dispatch_window(window_old, new);
        self.drain_stops(new, None);
        self.update_active(new);
        if self.clock != 0 {
            self.frame_update(self.clock);
        }
        if new as u32 > self.duration as u32 {
            if self.looping {
                self.stop();
                let carry = self.clock;
                self.loop_carry = carry;
                self.start(carry);
                return OK;
            }
            self.stop_all();
            self.state = state::DONE;
        }
        OK
    }
}

/// `SortActiveEventTypes` and `PartitionActiveEventTypes`: insertion sort
/// by type for up to 16 events; for more, the reference's median-of-three
/// partitions, then insertion.
fn sort_by_type(events: &mut [usize], arena: &[Option<Event>]) {
    let ty = |k: usize| arena[k].as_ref().expect("live event").kind();
    let insertion = |s: &mut [usize]| {
        for i in 1..s.len() {
            let key = s[i];
            let mut j = i;
            while j > 0 && ty(key) < ty(s[j - 1]) {
                s[j] = s[j - 1];
                j -= 1;
            }
            s[j] = key;
        }
    };
    if events.len() <= 16 {
        insertion(events);
        return;
    }
    partition_by(events, &|k| i64::from(ty(k)), true);
    insertion(&mut events[..16]);
    for i in 16..events.len() {
        let key = events[i];
        let mut j = i;
        while j > 0 && ty(key) < ty(events[j - 1]) {
            events[j] = events[j - 1];
            j -= 1;
        }
        events[j] = key;
    }
}

/// The reference's partition loop over ranges longer than 16, with its
/// median-of-three pivot (`active` selects the active list's variant;
/// the master list's differs on ties) and Hoare partition.
fn partition_by(s: &mut [usize], key: &dyn Fn(usize) -> i64, active: bool) {
    let (mut begin, mut end) = (0usize, s.len());
    while end - begin > 16 {
        let (first, middle, last) = (
            key(s[begin]),
            key(s[begin + (end - begin) / 2]),
            key(s[end - 1]),
        );
        let pivot = if active {
            if first < middle {
                if middle < last {
                    middle
                } else if first < last {
                    last
                } else {
                    first
                }
            } else if first < last {
                first
            } else if middle < last {
                last
            } else {
                middle
            }
        } else if first < middle {
            if middle < last {
                middle
            } else if last <= first {
                first
            } else {
                last
            }
        } else if first < last {
            first
        } else if middle < last {
            last
        } else {
            middle
        };
        let (mut left, mut right) = (begin, end);
        loop {
            while key(s[left]) < pivot {
                left += 1;
            }
            loop {
                right -= 1;
                if !(pivot < key(s[right])) {
                    break;
                }
            }
            if right <= left {
                break;
            }
            s.swap(left, right);
            left += 1;
        }
        if end - left <= left - begin {
            partition_by(&mut s[left..end], key, active);
            end = left;
        } else {
            partition_by(&mut s[begin..left], key, active);
            begin = left;
        }
    }
}

/// `SortMasterEvents`: by unsigned start time; insertion sort for up to
/// 16 events, else the partitions, insertion of the first 16 and of the
/// rest.
fn sort_master(master: &mut [usize], arena: &[Option<Event>]) {
    let key = |k: usize| i64::from(arena[k].as_ref().expect("live event").start_ms() as u32);
    let insertion = |s: &mut [usize]| {
        for i in 1..s.len() {
            let k = s[i];
            let mut j = i;
            while j > 0 && key(k) < key(s[j - 1]) {
                s[j] = s[j - 1];
                j -= 1;
            }
            s[j] = k;
        }
    };
    if master.len() <= 16 {
        insertion(master);
        return;
    }
    partition_by(master, &key, false);
    insertion(&mut master[..16]);
    for i in 16..master.len() {
        let k = master[i];
        let mut j = i;
        while j > 0 && key(k) < key(master[j - 1]) {
            master[j] = master[j - 1];
            j -= 1;
        }
        master[j] = k;
    }
}
