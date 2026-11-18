//! Bay doors and gates in the world: the playbacks `PlayScript` binds at
//! spawn (`CShip::init`'s `bay_door_anim`, `CSolar::init`'s
//! `open_anim`), `open_bay` and `close_bay`, `CSolar::activate` for
//! gates, the per-frame state updates, and the playback part of
//! `AnimDB::Update` (`Add(int)`, `MixerWalk`, `FilterObjects`). See
//! [`crate::object::anim`] for the state machines.

use neuron_core::anim::AnimScript;
use neuron_core::vfs::{FsDesc, UtfFileSystem};

use crate::equip::types;
use crate::object::anim::{self, Playback};
use crate::object::{KindData, ObjHandle, ObjectKind};

use super::{World, WorldEvent};

/// A playback registered with `AnimDB::Add(PlaybackId)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AnimKind {
    /// A ship's bay doors.
    Bay,
    /// A gate's animation.
    Gate,
}

impl World {
    /// `GetNamedDuration`: the duration of the animation `name` of an
    /// archetype file, `None` when the file has no such animation
    /// (`PlayScript` returns -1).
    pub(crate) fn anim_duration(&self, path: &[u8], name: &[u8]) -> Option<f32> {
        let assets = self.assets.as_ref()?;
        if path.is_empty() || name.is_empty() {
            return None;
        }
        let mut fs = UtfFileSystem::open(
            &assets.host,
            &FsDesc::read(path).with_implementation(b"UTF"),
        )
        .ok()?;
        let script = AnimScript::open(&mut fs).ok().flatten()?;
        let entry = script.get(name)?;
        Some(entry.longest_channel().map_or(-1.0, |c| c.duration()))
    }

    /// The bay and gate playbacks of a new object.
    pub(crate) fn init_animations(&mut self, h: ObjHandle) {
        let Some(o) = self.objects.get(h) else {
            return;
        };
        let (kind, archetype) = (o.kind, o.archetype);
        let path = self.archetype_path(kind, archetype);
        let name = match kind {
            ObjectKind::Ship => self
                .data
                .archetypes
                .ships
                .get(&archetype)
                .map(|s| self.data.strings.get(s.bay_door_anim).to_vec()),
            ObjectKind::Solar => self
                .data
                .archetypes
                .solars
                .get(&archetype)
                .map(|s| self.data.strings.get(s.open_anim).to_vec()),
            _ => None,
        }
        .unwrap_or_default();
        if name.is_empty() {
            return;
        }
        let playback = self.anim_duration(&path, &name).map(Playback::new);
        if let Some(o) = self.objects.get_mut(h) {
            match &mut o.data {
                KindData::Ship(s) => s.bay_anim = playback,
                KindData::Solar(s) => s.gate_anim = playback,
                _ => {}
            }
        }
    }

    fn register_playback(&mut self, h: ObjHandle, kind: AnimKind) {
        if !self.anim_playbacks.contains(&(h, kind)) {
            self.anim_playbacks.push((h, kind));
        }
    }

    fn playback_mut(&mut self, h: ObjHandle, kind: AnimKind) -> Option<&mut Playback> {
        match (&mut self.objects.get_mut(h)?.data, kind) {
            (KindData::Ship(s), AnimKind::Bay) => s.bay_anim.as_mut(),
            (KindData::Solar(s), AnimKind::Gate) => s.gate_anim.as_mut(),
            _ => None,
        }
    }

    /// `AnimDB::Update`'s playback part: every registered playback runs
    /// for `dt` (`MixerWalk`).
    pub(crate) fn advance_playbacks(&mut self, dt: f32) {
        for (h, kind) in self.anim_playbacks.clone() {
            if let Some(p) = self.playback_mut(h, kind) {
                p.advance(dt);
            }
        }
    }

    /// `AnimDB::Update`'s last phase: the finished playbacks leave the
    /// list (`FilterObjects`).
    pub(crate) fn filter_playbacks(&mut self) {
        let list = std::mem::take(&mut self.anim_playbacks);
        self.anim_playbacks = list
            .into_iter()
            .filter(|&(h, kind)| self.playback_mut(h, kind).is_some_and(|p| !p.finished()))
            .collect();
    }

    fn bay_request(&mut self, h: ObjHandle, open: bool) -> f64 {
        let Some(o) = self.objects.get_mut(h) else {
            return 0.0;
        };
        let id = o.simple.id;
        let KindData::Ship(s) = &mut o.data else {
            return 0.0;
        };
        let s = s.as_mut();
        let req = if open {
            anim::open_bay(
                &mut s.bay_door_counter,
                &mut s.bay_state,
                s.bay_anim.as_mut(),
            )
        } else {
            anim::close_bay(
                &mut s.bay_door_counter,
                &mut s.bay_state,
                s.bay_anim.as_mut(),
            )
        };
        if req.play {
            self.register_playback(h, AnimKind::Bay);
        }
        if let Some(opened) = req.changed {
            self.events.push(WorldEvent::BayDoor {
                ship: id,
                open: opened,
            });
            if let Some(eq) = self
                .objects
                .get_mut(h)
                .and_then(|o| o.equipped.as_deref_mut())
            {
                for slot in eq.equipment.traverse(types::SHIELD, false) {
                    if let Some(e) = eq.equipment.get_mut(slot) {
                        e.activate_base(!opened);
                    }
                }
            }
        }
        req.time
    }

    /// `CShip::open_bay`: the doors open on the first request; the time
    /// they need.
    pub fn open_bay(&mut self, h: ObjHandle) -> f64 {
        self.bay_request(h, true)
    }

    /// `CShip::close_bay`: the doors close on the last request.
    pub fn close_bay(&mut self, h: ObjHandle) -> f64 {
        self.bay_request(h, false)
    }

    /// `CShip::update`'s bay part.
    pub(crate) fn update_bay(&mut self, h: ObjHandle) {
        if let Some(KindData::Ship(s)) = self.objects.get_mut(h).map(|o| &mut o.data) {
            let s = s.as_mut();
            anim::update_bay(&mut s.bay_state, s.bay_anim.as_ref());
        }
    }

    /// `CSolar::activate` of a system gate: opens or closes it; whether
    /// the direction changed. `None` for any other object.
    pub fn activate_gate(&mut self, h: ObjHandle, on: bool) -> Option<bool> {
        let o = self.objects.get_mut(h)?;
        if !crate::object::solar::is_system_gate(o.simple.type_bits) {
            return None;
        }
        let KindData::Solar(s) = &mut o.data else {
            return None;
        };
        let old = s.gate_anim_direction;
        s.gate_anim_direction = on;
        Some(old != on)
    }

    /// `CSolar::update_system_gate`.
    pub(crate) fn update_solar_gate(&mut self, h: ObjHandle, dt: f32) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let (id, type_bits) = (o.simple.id, o.simple.type_bits);
        let KindData::Solar(s) = &mut o.data else {
            return;
        };
        if !crate::object::solar::is_system_gate(type_bits) {
            return;
        }
        let finished = s.gate_anim.is_none_or(|p| p.finished());
        let step = anim::update_system_gate(
            &mut s.gate_anim_state,
            &mut s.gate_anim_timer,
            s.gate_anim_direction,
            s.gate_anim.is_some(),
            finished,
            dt,
        );
        if let (Some(reverse), Some(p)) = (step.play, s.gate_anim.as_mut()) {
            p.eval(reverse, -2.0);
        }
        if step.play.is_some() {
            self.register_playback(h, AnimKind::Gate);
        }
        if let Some(code) = step.notify {
            self.events.push(WorldEvent::Gate { solar: id, code });
        }
    }
}
