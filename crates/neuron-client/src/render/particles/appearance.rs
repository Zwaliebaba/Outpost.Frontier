//! The appearances' simulation (`particlefx_appearances.cpp` and the
//! appearance parts of `particlefx_genericfx.cpp` and
//! `particlefx_common.cpp`): the field calls around an emission
//! (`Pad45`, `Pad46`), the spawn into the class's pool (`Pad47`), and the
//! per-frame update: age, the texture frame, the fields on every particle
//! (`ApplyFieldsToPool`) and the integration (`IntegratePool`).
//!
//! Drawing (vertices, the draw contexts, bounds) is the second increment
//! of P4.8. `FxParticleAppearance` and `FxMeshAppearance` spawn child
//! effects and meshes; they come with it and are not simulated here: they
//! only age and finish.

use std::rc::Rc;

use neuron_core::fx_data::NodeClass;
use neuron_core::math::{Transform, Vector, transform_xform};
use neuron_core::num::trunc_to_i32_via_i64;

use super::ParticleFx;
use super::node::{AppearanceState, Behaviour, FLAG_DONE, NodeId, TextureWrapper};
use super::pool::PoolKind;
use super::props::{BillboardProps, PropsData};
use super::ramp::round;
use super::services::ParticleServices;
use super::system::pool_of;

/// The name every texture query carries.
const TEXTURE_NAME: &[u8] = b"???";
/// `PerpAxisParallelEpsilon`, `0x358637BE`.
const PERP_EPSILON: f32 = f32::from_bits(0x3586_37BE);

impl TextureWrapper {
    /// `SetTexture({"???", crc})`: a new CRC releases the playback.
    fn set(&mut self, crc: u32, svc: &mut dyn ParticleServices) {
        if self.crc == crc {
            return;
        }
        if self.handle != -1 {
            svc.dec_ref_texture(self.handle);
            self.handle = -1;
        }
        self.crc = crc;
    }

    /// `Resolve`: the record of the CRC, a playback of it, its frame count
    /// and its first frame.
    fn resolve(&mut self, svc: &mut dyn ParticleServices) {
        if self.handle != -1 || self.crc == 0 {
            return;
        }
        let Some(record) = svc.find_texture(TEXTURE_NAME, self.crc) else {
            return;
        };
        let id = svc.create_playback(record);
        if let Some(count) = svc.frame_count(record) {
            self.frame_count = count;
        }
        svc.release_texture_record(record);
        self.handle = id;
        self.frame.handle = 0;
        svc.set_wrap_mode(id, 0);
        if let Some(frame) = svc.texture_frame(self.handle, -1) {
            self.frame = frame;
        }
    }

    /// `Advance(dt)`.
    fn advance(&mut self, dt: f32, svc: &mut dyn ParticleServices) {
        if self.handle == -1 {
            return;
        }
        svc.advance_time(self.handle, dt);
        if let Some(frame) = svc.texture_frame(self.handle, -1) {
            self.frame = frame;
        }
    }

    /// The frame lookup of the common frame ramp at `position`.
    fn lookup(&mut self, position: f32, svc: &mut dyn ParticleServices) {
        if self.frame_count != 1 && self.handle != -1 {
            let span = self.frame_count.wrapping_sub(1).cast_unsigned();
            let frame = trunc_to_i32_via_i64(f64::from(span) * f64::from(position));
            if let Some(out) = svc.texture_frame(self.handle, frame) {
                self.frame = out;
            }
        }
    }
}

/// The billboard slot holding `BasicApp_CommonTexFrame` in a class.
fn common_frame_slot(
    class: NodeClass,
    props: &BillboardProps,
) -> Option<&super::ramp::NestedSingle> {
    match class {
        NodeClass::BasicAppearance => props.frame.as_ref(),
        NodeClass::RectAppearance | NodeClass::OrientedAppearance => {
            props.common_tex_frame.as_ref()
        }
        _ => props.axis_override.as_ref(),
    }
}

impl ParticleFx {
    pub(crate) fn app(&self, id: NodeId) -> &AppearanceState {
        match &self.n(id).behaviour {
            Behaviour::Appearance(a) => a,
            _ => unreachable!("not an appearance"),
        }
    }

    pub(crate) fn app_mut(&mut self, id: NodeId) -> &mut AppearanceState {
        match &mut self.n_mut(id).behaviour {
            Behaviour::Appearance(a) => a,
            _ => unreachable!("not an appearance"),
        }
    }

    /// `AssociateField(field)`: appended with a reference (a `None` slot for
    /// a second node that is no field).
    pub fn associate_field(&mut self, appearance: NodeId, field: Option<NodeId>) {
        if let Some(f) = field {
            self.add_ref(f);
        }
        self.app_mut(appearance).fields.push(field);
    }

    /// The emitter's `AssociateAppearance(appearance)`: the reference moves
    /// to the new appearance, the history resets, and the appearance counts
    /// one more emitter (also when it was already associated).
    pub fn associate_appearance(
        &mut self,
        emitter: NodeId,
        appearance: Option<NodeId>,
        svc: &mut dyn ParticleServices,
    ) {
        let old = self.n(emitter).emitter().and_then(|e| e.appearance);
        if old != appearance {
            if let Some(a) = appearance {
                self.add_ref(a);
            }
            if let Behaviour::Emitter(e) = &mut self.n_mut(emitter).behaviour {
                e.appearance = appearance;
            }
            if let Some(o) = old {
                self.release(o, svc);
            }
        }
        if let Behaviour::Emitter(e) = &mut self.n_mut(emitter).behaviour {
            e.reset_history = true;
        }
        if let Some(a) = appearance {
            self.app_mut(a).emitter_refs += 1;
        }
    }

    /// `Pad45`: every field's `BeginField` on this appearance.
    pub(crate) fn appearance_begin_fields(&mut self, id: NodeId) {
        let fields = self.app(id).fields.clone();
        for field in fields.into_iter().flatten() {
            self.field_begin(field, id);
        }
    }

    /// `Pad46(six, age, duration)`: a duration under 0.01 fails; otherwise
    /// every field acts on a copy of the particle, and one that changed the
    /// age to past the duration fails it.
    pub(crate) fn appearance_check(
        &mut self,
        id: NodeId,
        six: &[f32; 6],
        age: f32,
        duration: f32,
    ) -> bool {
        if f64::from(duration) < 0.01 {
            return false;
        }
        let mut copy = *six;
        let mut age_copy = age;
        let fields = self.app(id).fields.clone();
        // A missing field is a null pointer the reference calls through
        // (D371).
        for field in fields.into_iter().flatten() {
            self.field_apply(field, &mut copy, &mut age_copy);
            if age_copy != age && age_copy > duration {
                return false;
            }
        }
        true
    }

    /// Takes the pool's oldest particle away from its owner when the pool
    /// is full (`SpawnListParticle`'s head, `EvictOldestListParticle`).
    fn evict_if_full(&mut self, kind: PoolKind) {
        let pool = self.pools.get(kind);
        if pool.free_count() != 0 {
            return;
        }
        let Some(oldest) = pool.oldest() else {
            return;
        };
        let owner = pool.item(oldest).owner;
        if let Some(owner) =
            owner.filter(|&o| self.node(o).is_some_and(|n| n.appearance().is_some()))
        {
            let app = self.app(owner);
            let (mut head, mut tail) = (app.head, app.tail);
            self.pools
                .get_mut(kind)
                .unlink_particle(&mut head, &mut tail, oldest);
            let app = self.app_mut(owner);
            app.head = head;
            app.tail = tail;
            app.live = app.live.wrapping_sub(1);
        }
        self.pools.get_mut(kind).free(oldest);
    }

    /// Allocates and fills a particle for `owner` (the shared fill of the
    /// spawns): position and velocity, owner, age, duration and its
    /// reciprocal, one more live particle.
    fn new_particle(
        &mut self,
        kind: PoolKind,
        owner: NodeId,
        six: &[f32; 6],
        age: f32,
        duration: f32,
    ) -> Option<u32> {
        let index = self.pools.get_mut(kind).alloc()?;
        let item = self.pools.get_mut(kind).item_mut(index);
        item.pos = [six[0], six[1], six[2]];
        item.vel = [six[3], six[4], six[5]];
        item.owner = Some(owner);
        item.age = age;
        item.duration = duration;
        item.inv_duration = round(1.0 / f64::from(duration));
        let app = self.app_mut(owner);
        app.live = app.live.wrapping_add(1);
        Some(index)
    }

    /// `Pad47(six, age, duration)`: the class's spawn.
    pub(crate) fn appearance_spawn(&mut self, id: NodeId, six: &[f32; 6], age: f32, duration: f32) {
        let class = self.n(id).class();
        let Some(kind) = pool_of(class) else {
            return;
        };
        if matches!(kind, PoolKind::Particle | PoolKind::Mesh) {
            // Child effects and meshes: the second increment.
            return;
        }
        if self.pools.get(kind).capacity() == 0 {
            // The reference stores through a null item (D371).
            return;
        }
        self.evict_if_full(kind);
        let Some(index) = self.new_particle(kind, id, six, age, duration) else {
            return;
        };
        let app = self.app_mut(id);
        let (mut head, mut tail) = (app.head, app.tail);
        let pool = self.pools.get_mut(kind);
        if kind == PoolKind::Beam {
            pool.item_mut(index).extra = [0.0; 3];
            pool.prepend_particle(&mut head, &mut tail, index);
        } else {
            pool.append_particle(&mut head, &mut tail, index);
        }
        let app = self.app_mut(id);
        app.head = head;
        app.tail = tail;
        match kind {
            PoolKind::Beam => app.ribbon_parity = !app.ribbon_parity,
            PoolKind::Perp => {
                let eps = f64::from(PERP_EPSILON);
                let dx = f64::from(six[3]) - 1.0;
                let parallel = dx * dx <= eps
                    && f64::from(six[4]) * f64::from(six[4]) <= eps
                    && f64::from(six[5]) * f64::from(six[5]) <= eps;
                let axis = if parallel {
                    [0.0, 0.0, 1.0]
                } else {
                    [1.0, 0.0, 0.0]
                };
                // The axis goes on the list's tail.
                if let Some(t) = tail {
                    self.pools.get_mut(kind).item_mut(t).extra = axis;
                }
            }
            PoolKind::Dust => {
                let view = Transform::from_words(app.view_xform);
                let p = transform_xform(&view, Vector::new(six[0], six[1], six[2])).to_words();
                if let Some(t) = tail {
                    self.pools.get_mut(kind).item_mut(t).extra = p;
                }
            }
            _ => {}
        }
    }

    /// The appearance's update (`Pad5`).
    pub(crate) fn appearance_update(
        &mut self,
        id: NodeId,
        dt: f32,
        svc: &mut dyn ParticleServices,
    ) {
        let props_rc = Rc::clone(&self.n(id).props);
        let class = props_rc.class;
        let PropsData::Billboard(props) = &props_rc.data else {
            // Particle and mesh appearances: their update is the second
            // increment; here they age and finish without particles.
            let node = self.n_mut(id);
            node.age = round(f64::from(dt) + f64::from(node.age));
            let app = self.app(id);
            if app.emitter_refs == 0 && app.head.is_none() {
                self.n_mut(id).flags |= FLAG_DONE;
            }
            return;
        };
        if class == NodeClass::DustAppearance {
            self.app_mut(id).last_dt = dt;
        }
        let node = self.n_mut(id);
        node.age = round(f64::from(dt) + f64::from(node.age));
        let (outer, age) = (node.outer_time, node.age);
        let app = self.app(id);
        if app.emitter_refs == 0 && app.head.is_none() {
            self.n_mut(id).flags |= FLAG_DONE;
        }
        let mut texture = self.app(id).texture;
        texture.set(props.texture_crc, svc);
        if texture.handle == -1 {
            texture.resolve(svc);
        }
        if class != NodeClass::BeamAppearance {
            if props.flags & 8 == 8 {
                let position = common_frame_slot(class, props)
                    .map_or(0.0, |ramp| round(ramp.eval(outer, age)));
                texture.lookup(position, svc);
            } else if texture.frame_count != 1 {
                texture.advance(dt, svc);
            }
        }
        self.app_mut(id).texture = texture;
        self.apply_fields_to_pool(id, svc);
        self.integrate(id, dt);
    }

    /// `ApplyFieldsToPool(owner, false)`: a dead field leaves the list
    /// (releasing it); every other one begins on the appearance and acts on
    /// each of its particles in list order.
    fn apply_fields_to_pool(&mut self, id: NodeId, svc: &mut dyn ParticleServices) {
        let Some(kind) = pool_of(self.n(id).class()) else {
            return;
        };
        let mut position = 0;
        while position < self.app(id).fields.len() {
            let Some(field) = self.app(id).fields[position] else {
                // A null slot: the reference calls through it (D371).
                position += 1;
                continue;
            };
            if self.n(field).is_dead() {
                self.app_mut(id).fields.remove(position);
                self.release(field, svc);
                continue;
            }
            self.field_begin(field, id);
            let mut cursor = self.app(id).head;
            while let Some(index) = cursor {
                let item = *self.pools.get(kind).item(index);
                let mut pos_vel = [
                    item.pos[0],
                    item.pos[1],
                    item.pos[2],
                    item.vel[0],
                    item.vel[1],
                    item.vel[2],
                ];
                let mut age = item.age;
                self.field_apply(field, &mut pos_vel, &mut age);
                let item = self.pools.get_mut(kind).item_mut(index);
                item.pos = [pos_vel[0], pos_vel[1], pos_vel[2]];
                item.vel = [pos_vel[3], pos_vel[4], pos_vel[5]];
                item.age = age;
                cursor = item.next;
            }
            position += 1;
        }
    }

    /// `IntegratePool(owner, pool, dt)`: every particle ages; one past its
    /// duration leaves the list and goes back to the pool, the others move
    /// by their velocity.
    fn integrate(&mut self, id: NodeId, dt: f32) {
        let Some(kind) = pool_of(self.n(id).class()) else {
            return;
        };
        let dt64 = f64::from(dt);
        let mut cursor = self.app(id).head;
        while let Some(index) = cursor {
            let pool = self.pools.get_mut(kind);
            let item = pool.item_mut(index);
            item.age = round(dt64 + f64::from(item.age));
            if item.age > item.duration {
                cursor = item.next;
                let app = self.app_mut(id);
                let (mut head, mut tail) = (app.head, app.tail);
                let pool = self.pools.get_mut(kind);
                pool.unlink_particle(&mut head, &mut tail, index);
                let app = self.app_mut(id);
                app.head = head;
                app.tail = tail;
                app.live = app.live.wrapping_sub(1);
                self.pools.get_mut(kind).free(index);
            } else {
                let dx = dt64 * f64::from(item.vel[0]);
                let dy = dt64 * f64::from(item.vel[1]);
                let dz = round(dt64 * f64::from(item.vel[2]));
                item.pos[0] = round(dx + f64::from(item.pos[0]));
                item.pos[1] = round(dy + f64::from(item.pos[1]));
                item.pos[2] = round(f64::from(dz) + f64::from(item.pos[2]));
                cursor = item.next;
            }
        }
    }
}
