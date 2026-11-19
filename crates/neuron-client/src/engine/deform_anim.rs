//! Animations and aim events on the engine's deformable models (work
//! packages P4.7 and P4.16): the `IDeformable` calls that play a script on
//! a model's skeleton (`PlayInstanceAnim`, `PlayInstanceAnimChain`), stop
//! its list (`DestroyAllList`) and aim its joints (`StartAim`, `StopAim`),
//! made on the model's own list of animations and aim events, and the tick
//! the game's character objects give each model
//! (`FEngineCharacterBody::Advance`: `MixerWalk` under the root bone,
//! `UpdateInstance`, `GetListCount`), which the engine's frame runs for
//! every model with something on its list.
//!
//! The scripts play through the animation system with the model as their
//! channels' sink: while the mixer walks, the model's `OnKeys` and
//! `OnLoop` run as the channels tick ([`DeformSink`]; an `OnLoop` resets
//! the height and heading the channel loops from), and what they forward
//! to the caller's sink is queued ([`Engine::take_deformable_events`]) for
//! the models a caller listens to ([`Engine::listen_deformable`]). An aim
//! event's typed channels are owned by the event: at each tick the
//! animation system calls the engine's owner adapter, which runs the
//! event's `IkApply` on the engine the channels are ticked on and writes
//! the joint's solved quaternion into the channel's sample, which the
//! mixer then writes into the joint.

use std::cell::Ref;
use std::sync::Arc;

use neuron_core::anim::AnimScript;
use neuron_core::scene::{Animation, InstanceId};

use crate::render::deformable::instance::ListPayload;
use crate::render::deformable::{AimDesc, DeformInstance, DeformServices};
use crate::render::ff::device::Backend;
use crate::render::stack::StackDeform;

use super::{
    AnimationsId, DeformableEvent, DeformableId, Engine, EngineError, lossy, sink_token,
    walk_deformable,
};

/// The arguments of `PlayInstanceAnim` after the script and the name
/// (`IDeformable::Play`'s `ArgC` to `ArgH`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DeformablePlay {
    /// `ArgC`: the start time; above 0 the root data's motion up to it
    /// moves the start place.
    pub time: f32,
    /// `ArgD`: the blend-in duration.
    pub blend: f32,
    /// `ArgE`: the speed.
    pub speed: f32,
    /// `ArgF`: the scale.
    pub scale: f32,
    /// `ArgG`: the play flags (bit 1 loop, bit 2 ping-pong, ...).
    pub flags: u32,
    /// `ArgH`: the heading about y; below 0 the root bone's own.
    pub twist: f32,
}

impl DeformablePlay {
    /// The arguments the game's character bodies play with
    /// (`FEngineCharacterBody::PlayAnimation`): from the start, no blend,
    /// normal speed and scale, the root bone's heading; looping on
    /// request.
    #[must_use]
    pub fn character(looping: bool) -> DeformablePlay {
        DeformablePlay {
            time: 0.0,
            blend: 0.0,
            speed: 1.0,
            scale: 1.0,
            flags: if looping { 2 } else { 0 },
            twist: -1.0,
        }
    }
}

impl<B: Backend + 'static> Engine<B> {
    /// The animation system.
    #[must_use]
    pub fn animation(&self) -> Ref<'_, Animation> {
        self.animation.borrow()
    }

    /// The animations of an `.anm` file of the data (`OpenScript`): the
    /// script and the names of its animations.
    ///
    /// # Errors
    ///
    /// The file does not open or holds no `Animation` node.
    pub fn load_animations(
        &mut self,
        path: &[u8],
    ) -> Result<(AnimationsId, Vec<Vec<u8>>), EngineError> {
        let mut fs = self.open_archive(path)?;
        let script = AnimScript::open(&mut fs)
            .map_err(|e| EngineError::Animation(format!("{}: {e:?}", lossy(path))))?
            .ok_or_else(|| EngineError::Animation(format!("{}: no animations", lossy(path))))?;
        let names = script.entries().map(|(_, e)| e.name.clone()).collect();
        let handle = self.deform_host.add_script(Arc::new(script));
        Ok((AnimationsId(handle as usize), names))
    }

    /// Runs `f` on a model with the services its `IDeformable` calls go
    /// to, the model current in the deformables' host.
    fn with_model<R>(
        &mut self,
        id: DeformableId,
        f: impl FnOnce(&mut DeformInstance, &mut dyn DeformServices) -> R,
    ) -> Option<R> {
        let mut models = self.deformables.borrow_mut();
        let instance = models.get_mut(id.0)?.as_mut()?;
        let mut side = self.render.borrow_mut();
        self.deform_host.set_current(Some(id.0));
        let mut services = StackDeform::new(&mut side.stack, &mut self.deform_host);
        let out = f(instance, &mut services);
        self.deform_host.set_current(None);
        Some(out)
    }

    fn script_and_name(
        &mut self,
        animations: AnimationsId,
        name: &[u8],
    ) -> Result<(u32, u32), EngineError> {
        let script = u32::try_from(animations.0).unwrap_or(0);
        if self.deform_host.script(script).is_none() {
            return Err(EngineError::Animation(
                "no such animation script".to_owned(),
            ));
        }
        Ok((script, self.deform_host.names.intern(name)))
    }

    /// `IDeformable::Play` (`PlayInstanceAnim`): the animation `name` of
    /// `animations` played on the model's skeleton under its root bone,
    /// from the place and heading `play` asks, and put on the model's
    /// list. The playback handle.
    ///
    /// # Errors
    ///
    /// No such model or script; the script has no such animation, or it
    /// did not start.
    pub fn play_deformable(
        &mut self,
        id: DeformableId,
        animations: AnimationsId,
        name: &[u8],
        play: &DeformablePlay,
    ) -> Result<u32, EngineError> {
        let (script, anim) = self.script_and_name(animations, name)?;
        let handle = self
            .with_model(id, |instance, services| {
                instance.play(
                    script,
                    anim,
                    play.time,
                    play.blend.to_bits(),
                    play.speed,
                    play.scale.to_bits(),
                    play.flags,
                    play.twist,
                    0,
                    services,
                )
            })
            .ok_or_else(|| EngineError::Animation("no such deformable model".to_owned()))?;
        if handle == -1 {
            return Err(EngineError::Animation(format!(
                "{} did not start",
                lossy(name)
            )));
        }
        Ok(handle.cast_unsigned())
    }

    /// `IDeformable::PlayChain` (`PlayInstanceAnimChain`): as
    /// [`Engine::play_deformable`], then the playback moved so that the
    /// joint `joint` starts where it stands. The playback handle.
    ///
    /// # Errors
    ///
    /// As [`Engine::play_deformable`].
    pub fn play_deformable_chain(
        &mut self,
        id: DeformableId,
        animations: AnimationsId,
        name: &[u8],
        play: &DeformablePlay,
        joint: InstanceId,
    ) -> Result<u32, EngineError> {
        let (script, anim) = self.script_and_name(animations, name)?;
        let joint = i32::try_from(joint.index()).unwrap_or(-1);
        let handle = self
            .with_model(id, |instance, services| {
                instance.play_chain(
                    script,
                    anim,
                    play.time,
                    play.blend.to_bits(),
                    play.speed,
                    play.scale.to_bits(),
                    play.flags,
                    play.twist,
                    joint,
                    services,
                )
            })
            .ok_or_else(|| EngineError::Animation("no such deformable model".to_owned()))?;
        if handle == -1 {
            return Err(EngineError::Animation(format!(
                "{} did not start",
                lossy(name)
            )));
        }
        Ok(handle.cast_unsigned())
    }

    /// Plays the animation `name` of `animations` on a deformable model as
    /// the game's character bodies do ([`DeformablePlay::character`]),
    /// replacing what was on its list. The engine's frame then advances
    /// the model. The playback handle.
    ///
    /// # Errors
    ///
    /// As [`Engine::play_deformable`].
    pub fn play_deformable_animation(
        &mut self,
        id: DeformableId,
        animations: AnimationsId,
        name: &[u8],
        looping: bool,
    ) -> Result<u32, EngineError> {
        if self.deformable_root(id).is_none() {
            return Err(EngineError::Animation(
                "no such deformable model".to_owned(),
            ));
        }
        self.stop_deformable_animation(id);
        self.play_deformable(id, animations, name, &DeformablePlay::character(looping))
    }

    /// `IDeformable::DestroyAllList`: the model's animations and aim events
    /// stop and go; the model keeps the pose it has.
    pub fn stop_deformable_animation(&mut self, id: DeformableId) {
        let _ = self.with_model(id, |instance, services| instance.destroy_all(services));
    }

    /// `IDeformable::GetListCount`: the model's list after a sweep.
    pub fn deformable_list_count(&mut self, id: DeformableId) -> usize {
        self.with_model(id, |instance, services| instance.list_count(services))
            .unwrap_or(0)
    }

    /// The animations on the deformable models' lists.
    #[must_use]
    pub fn animations_playing(&self) -> usize {
        self.deformables
            .borrow()
            .iter()
            .flatten()
            .map(|i| {
                i.list
                    .iter()
                    .filter(|n| matches!(n.payload, ListPayload::Anim(_)))
                    .count()
            })
            .sum()
    }

    /// `IDeformable::StartAim`: an aim event on the model's chain from
    /// `desc.root` down to `desc.end`, its typed channels played with
    /// `play_extra` as their blend. The node's handle.
    ///
    /// # Errors
    ///
    /// No such model, a joint of the chain is driven by another aim, or
    /// the chain is too long.
    pub fn start_deformable_aim(
        &mut self,
        id: DeformableId,
        desc: &AimDesc,
        play_extra: u32,
    ) -> Result<u32, EngineError> {
        let node = self
            .with_model(id, |instance, services| {
                instance.start_aim(desc, play_extra, services)
            })
            .ok_or_else(|| EngineError::Animation("no such deformable model".to_owned()))?;
        u32::try_from(node).map_err(|_| EngineError::Animation("the aim did not start".to_owned()))
    }

    /// The targets of an aim event (the floats the reference's aim
    /// description points to).
    pub fn set_deformable_aim_targets(
        &mut self,
        id: DeformableId,
        node: u32,
        position: [f32; 3],
        orientation: [f32; 9],
    ) {
        if let Some(mut instance) = self.deformable_mut(id)
            && let Some(event) = instance.aim_mut(node)
        {
            event.set_targets(position, orientation);
        }
    }

    /// `IDeformable::StopAim`: the aim event stops; the sweep drops it
    /// once its channels have left the mixer.
    pub fn stop_deformable_aim(&mut self, id: DeformableId, node: u32) {
        let _ = self.with_model(id, |instance, services| instance.stop_aim(node, services));
    }

    /// Hands the model's `OnKeys` and `OnLoop` forwards to the engine's
    /// queue (the caller's sink of `IDeformable::Create`).
    pub fn listen_deformable(&mut self, id: DeformableId) {
        if let Some(mut instance) = self.deformable_mut(id) {
            instance.sink = sink_token(id.0);
        }
    }

    /// What the listened models' sinks received since the last call.
    pub fn take_deformable_events(&mut self) -> Vec<DeformableEvent> {
        std::mem::take(&mut self.deformable_events)
    }

    /// `FEngineCharacterBody::Advance(dt)` for every model with something on
    /// its list: the mixer walked under its root bone (the instance's
    /// sinks and the aim events' owners called as the channels tick), its
    /// tree updated and its bound bones handed their world transforms,
    /// then `GetListCount`'s sweep. The models advanced.
    pub(super) fn advance_deformables(&mut self, dt: f32) -> usize {
        let count = self.deformables.borrow().len();
        let mut advanced = 0;
        for index in 0..count {
            let listed = self
                .deformables
                .borrow()
                .get(index)
                .and_then(Option::as_ref)
                .is_some_and(|i| !i.list.is_empty());
            if !listed {
                continue;
            }
            walk_deformable(
                &self.scene,
                &self.animation,
                &self.deformables,
                &mut self.deform_host.names,
                &mut self.deformable_events,
                index,
                dt,
            );
            let _ = self.with_model(DeformableId(index), |instance, services| {
                instance.list_count(services)
            });
            advanced += 1;
        }
        advanced
    }
}
