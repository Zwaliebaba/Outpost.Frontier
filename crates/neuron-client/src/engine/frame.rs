//! One frame of the engine in the reference's order (work package P4.16).
//!
//! The reference's frame is `WinMain`'s loop (`UpdateClientTiming`,
//! `PumpMessages`, `TickClientFrame`) and the current client state's
//! update, which begins, draws and presents its own frame
//! (`client_frame_tick.cpp`, `gameflow.cpp`'s `UpdateGameflowBaseFrame`,
//! `space_state.cpp`'s update). [`Engine::frame`] runs the engine's part
//! of it:
//!
//! 1. Input: the mouse model's frame update over the platform and its
//!    queued events, then the caller's routed key and character messages,
//!    into the UI manager (`MouseDispatchEvents`, the Win3D input handler).
//! 2. `Win3DUpdateAll`: the UI manager's update.
//! 3. The state's world tick: the scene scripts' timelines, the animation
//!    system's mixer (`MixerWalkAll`) with the animated deformable models'
//!    trees updated and their bones' transforms handed over, the particle
//!    effects' transforms and update, then `TickMaterialAnimationPlayback`
//!    (the texture library's playbacks and the material animations).
//! 4. `BeginClientRenderFrame`: the dither and mip-filter states of
//!    `TryBeginFrame`.
//! 5. The clear (`ClearWin3DBuffers(0x4100)` after a black clear colour),
//!    the frame's camera (`Win3DApplyCamera`), then the scene: the backdrop and the star sphere through
//!    `FLAppearance`, the models' instance trees through the render manager
//!    (`RenderInstance`), the deformable models (`IDeformable::Draw`), the
//!    particle effects between `BeginDraw` and `EndDraw`.
//! 6. `ClearWin3DBuffers(0x100)`, `Win3DSetDepthFunc(4)`, `Win3DDrawAll`
//!    and the screen-quad lists it filled.
//! 7. `EndClientRenderScene`: the batcher's `FlushAll(1)` and the render
//!    states the next frame starts from; then `PresentClientRenderFrame`.
//!
//! Stubbed, recorded in the P4.16 row of design/README.md: the sound
//! mixer's tick (its sources are the game's, P5.5), the local server's
//! tick and the state machine (P5.1, P5.3), the game's own scene order
//! (the space and room cameras' `DrawScene`, P5.4, P5.9, of which the
//! frame keeps the camera's application), the fade, the
//! immediate-mode layer's line batchers (P4.13), and the frame clock (the
//! caller's `game_app::timing`).

use crate::input::mouse::MousePlatform;
use crate::render::appearance::{AppearanceServices, Camera, MeshView};
use crate::render::ff::device::{Backend, ClearFlags, Device};
use crate::render::stack::{StackAppearance, draw_deformable};
use crate::ui::message::Input;
use crate::ui::quad_batch::{FfQuadDevice, QuadBatcher};
use crate::ui::tree::QuadHost;

use super::{Engine, RenderSide, apply_extents, pull_bones};

/// `D3DRS_ZENABLE`.
const RS_ZENABLE: u32 = 7;
/// `D3DRS_ZWRITEENABLE`.
const RS_ZWRITEENABLE: u32 = 14;
/// `D3DRS_CULLMODE`.
const RS_CULLMODE: u32 = 22;
/// `D3DRS_ZFUNC`.
const RS_ZFUNC: u32 = 23;
/// `D3DRS_DITHERENABLE`.
const RS_DITHERENABLE: u32 = 26;
/// `D3DRS_ALPHABLENDENABLE`.
const RS_ALPHABLENDENABLE: u32 = 27;
/// `glDisable(GL_FOG)`'s seven states: `FOGENABLE`, `FOGTABLEMODE`,
/// `FOGVERTEXMODE`, `FOGCOLOR`, `FOGSTART`, `FOGEND`, `FOGDENSITY`.
const FOG_STATES: [u32; 7] = [28, 35, 140, 34, 36, 37, 38];
/// `D3DTSS_MIPFILTER`.
const TSS_MIPFILTER: u32 = 18;
/// `ClearWin3DBuffers(0x4100)`'s clear colour, opaque black.
const CLEAR_BLACK: u32 = 0xFF00_0000;

/// What the caller hands one frame.
pub struct FrameInput<'a> {
    /// The capped frame time (`GetClientFrameTimeCapped`).
    pub delta_seconds: f64,
    /// The frame's timestamp in milliseconds, for the mouse model.
    pub frame_ms: u32,
    /// The mouse device; `None` skips the mouse model's update.
    pub mouse: Option<&'a mut dyn MousePlatform>,
    /// The key and character messages the router gave the UI.
    pub ui_input: Vec<Input>,
}

impl FrameInput<'_> {
    /// A frame of `delta_seconds` with no input.
    #[must_use]
    pub fn idle(delta_seconds: f64) -> FrameInput<'static> {
        FrameInput {
            delta_seconds,
            frame_ms: 0,
            mouse: None,
            ui_input: Vec::new(),
        }
    }
}

/// What one frame did.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameReport {
    /// UI events handled (mouse and routed).
    pub ui_events: usize,
    /// Registry entries the UI update walked.
    pub ui_nodes: usize,
    /// Scene scripts ticked.
    pub scripts: usize,
    /// Effects updated and drawn.
    pub effects: usize,
    /// Model instance trees drawn (the backdrop and star sphere included).
    pub models: usize,
    /// Deformable models drawn.
    pub deformables: usize,
    /// Deformable models whose animation advanced.
    pub animations: usize,
    /// Screen-quad lists flushed.
    pub quad_lists: usize,
}

/// The render-state cache of the Win3D immediate-mode layer
/// (`win3d_immediate.cpp`'s `G_bDepthTest`, `G_DepthWrite`, `G_DepthFunc`,
/// `G_AlphaBlendEnable`), with the calls of it the frame makes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Win3dStates {
    pub depth_test: bool,
    pub depth_write: u32,
    pub depth_func: u32,
    pub alpha_blend: u32,
}

impl Default for Win3dStates {
    fn default() -> Self {
        Win3dStates {
            depth_test: true,
            depth_write: 1,
            depth_func: 4,
            alpha_blend: 0,
        }
    }
}

impl Win3dStates {
    fn depth<B: Backend>(&self, device: &mut Device<B>) {
        let _ = device.state.set_render_state_raw(RS_ZFUNC, self.depth_func);
        let _ = device
            .state
            .set_render_state_raw(RS_ZENABLE, u32::from(self.depth_test));
        let _ = device
            .state
            .set_render_state_raw(RS_ZWRITEENABLE, self.depth_write);
    }

    /// `Win3DSetDepthFunc`.
    pub fn set_depth_func<B: Backend>(&mut self, device: &mut Device<B>, func: u32) {
        self.depth_func = func;
        self.depth(device);
    }

    /// `Win3DSetDepthWrite`.
    pub fn set_depth_write<B: Backend>(&mut self, device: &mut Device<B>, on: u32) {
        self.depth_write = on;
        self.depth(device);
    }

    /// The end of `EndSceneHalf`: fog off, depth write on, blending off,
    /// the depth test on, `LESSEQUAL`, back faces culled.
    pub fn end_scene<B: Backend>(&mut self, device: &mut Device<B>) {
        for state in FOG_STATES {
            let _ = device.state.set_render_state_raw(state, 0);
        }
        self.set_depth_write(device, 1);
        self.alpha_blend = 0;
        let _ = device.state.set_render_state_raw(RS_ALPHABLENDENABLE, 0);
        self.depth_test = true;
        self.depth(device);
        self.set_depth_func(device, 4);
        let _ = device.state.set_render_state_raw(RS_CULLMODE, 2);
    }
}

impl<B: Backend + 'static> Engine<B> {
    /// One frame: the input, the UI update, the world tick, the scene,
    /// the UI draw and the present, in the reference's order (see the
    /// module documentation). `camera` is the scene's camera.
    #[allow(clippy::too_many_lines)]
    pub fn frame(&mut self, input: FrameInput<'_>, camera: &mut dyn Camera) -> FrameReport {
        let mut report = FrameReport::default();
        let dt = input.delta_seconds;
        #[allow(clippy::cast_possible_truncation)]
        let dt_f32 = dt as f32;

        // 1. Input.
        let mut events = Vec::new();
        if let (Some(platform), Some(mouse)) = (input.mouse, self.mouse.as_mut())
            && self.cursors.ready
        {
            mouse.frame_update(platform, input.frame_ms);
            let (x, y, _) = mouse.cursor();
            let wheel = mouse.wheel();
            events.extend(
                mouse
                    .drain_events()
                    .into_iter()
                    .filter_map(|e| Input::from_mouse_event(e, (x, y), wheel)),
            );
        }
        events.extend(input.ui_input);
        if let Some(ui) = self.ui.as_mut() {
            for event in events {
                ui.handle(event);
                report.ui_events += 1;
            }
            // 2. Win3DUpdateAll.
            ui.update_all(dt);
            report.ui_nodes = ui.update_node_count;
        }

        // 3. The world tick.
        let step = (dt * 1000.0).round() as i32;
        for player in self.scripts.iter_mut().flatten() {
            player.tick(step);
            report.scripts += 1;
        }
        if !self.playbacks.is_empty() {
            let mut scene = self.scene.borrow_mut();
            let _ = self.animation.mixer_walk_all(&mut scene, dt_f32);
            for playback in &self.playbacks {
                let Some(Some(instance)) = self.deformables.get_mut(playback.deformable) else {
                    continue;
                };
                let root = u32::try_from(instance.first_bone())
                    .ok()
                    .map(neuron_core::scene::InstanceId::from_index);
                scene.update_instance(root, 0);
                pull_bones(instance, &scene);
                report.animations += 1;
            }
        }
        let mut side = self.render.borrow_mut();
        for live in self.effects.iter().flatten() {
            live.effect.compose(&mut self.particles);
            live.effect
                .update(&mut self.particles, dt_f32, &mut side.stack);
        }
        let shading = &mut side.stack.shading;
        shading.textures.tick_all(0, dt_f32);
        if let Some(animations) = shading.animations.as_mut() {
            animations.tick_instances(-1, dt_f32);
        }

        // 4. BeginClientRenderFrame: TryBeginFrame's two states.
        let device = &mut side.stack.shading.device;
        // Dithering is on only for a 16-bit back buffer; the back buffer is
        // always 32-bit here (D455).
        let _ = device.state.set_render_state_raw(RS_DITHERENABLE, 0);
        let _ = device.state.set_stage_state_raw(0, TSS_MIPFILTER, 1);

        // 5. The clear and the scene.
        device.state.set_pipeline(
            crate::render::ff::state::PipelineProperty::ClearColor,
            CLEAR_BLACK,
        );
        device.clear(ClearFlags::TARGET | ClearFlags::DEPTH, None);
        apply_camera(device, camera);
        {
            let RenderSide { stack, manager } = &mut *side;
            let mut services = StackAppearance { stack, manager };
            for (slot, starsphere) in [(self.backdrop, false), (self.starsphere, true)] {
                let Some(root) = slot.and_then(|m| self.model_root(m)) else {
                    continue;
                };
                let instance = root.index() as usize;
                if let Some(appearance) = self.appearance.as_mut() {
                    if starsphere {
                        appearance.draw_starsphere(&mut services, camera, instance);
                    } else {
                        appearance.draw_backdrop(&mut services, camera, instance);
                    }
                    report.models += 1;
                }
            }
            for (index, model) in self.models.iter().enumerate() {
                let Some(model) = model else { continue };
                let id = super::ModelId(index);
                if self.backdrop == Some(id) || self.starsphere == Some(id) {
                    continue;
                }
                services.render_instance(camera, model.root.index() as usize, 1.0, 0, None);
                report.models += 1;
            }
        }
        for instance in self.deformables.iter_mut().flatten() {
            let RenderSide { stack, manager } = &mut *side;
            draw_deformable(
                instance,
                &mut self.deform,
                &mut MeshView(camera),
                0,
                0,
                stack,
                Some(manager),
                None,
                &mut self.deform_host,
            );
            report.deformables += 1;
        }
        if self.effects.iter().any(Option::is_some) {
            let stack = &mut side.stack;
            if self.particles.begin_draw(stack) >= 0 {
                for live in self.effects.iter().flatten() {
                    live.effect.draw(&mut self.particles, camera, stack);
                    report.effects += 1;
                }
                self.particles.end_draw(camera, stack);
            }
        }

        // 6. The interface over the scene.
        let shading = &mut side.stack.shading;
        shading.device.clear(ClearFlags::DEPTH, None);
        self.win3d.set_depth_func(&mut shading.device, 4);
        if let Some(ui) = self.ui.as_mut() {
            let mut host = QuadHost {
                quads: std::mem::take(&mut self.quads),
            };
            ui.draw_all(&mut host);
            self.quads = host.quads;
        }
        report.quad_lists = flush_quads(&mut self.quads, shading);

        // 7. EndClientRenderScene and PresentClientRenderFrame.
        side.stack.flush_all(1);
        self.win3d.end_scene(&mut side.stack.shading.device);
        side.stack.shading.device.present();
        drop(side);
        apply_extents(&self.scene, &self.links);
        self.frames += 1;
        report
    }
}

/// `Win3DApplyCamera`: the perspective from the camera's vertical angle,
/// aspect and planes (`Win3DSetPerspective`; the Win3D layer's projection
/// cache belongs to the immediate-mode layer, P4.13), then its view
/// transform as the modelview; with
/// the pipeline's view-space lights off, an identity view and the camera's
/// view as the world.
fn apply_camera<B: Backend>(device: &mut Device<B>, camera: &mut dyn Camera) {
    let far = camera.z_far();
    let near = camera.z_near();
    let aspect = camera.aspect();
    let fov = camera.fov_y_deg();
    let _ = device.transforms.set_perspective(fov, aspect, near, far);
    let view = camera.view_transform();
    if device
        .state
        .pipeline(crate::render::ff::state::PipelineProperty::ViewspaceLights)
        != 0
    {
        device.set_modelview(&view);
        return;
    }
    device
        .transforms
        .set_view(&[1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]);
    device.transforms.set_world(&view);
}

/// Every screen-quad list the UI draw filled, flushed to the device in
/// key order, standing for the renderables' `ScreenQuadFlush` of their
/// own lists (D452).
fn flush_quads<B: Backend>(
    quads: &mut QuadBatcher,
    shading: &mut crate::render::material::Shading<B>,
) -> usize {
    let keys: Vec<u32> = quads
        .keys()
        .filter(|&k| quads.list(k).is_some_and(|l| !l.live.is_empty()))
        .collect();
    let mut device = FfQuadDevice {
        device: &mut shading.device,
        handles: &shading.handles,
    };
    for &key in &keys {
        quads.flush(&mut device, key);
    }
    keys.len()
}
