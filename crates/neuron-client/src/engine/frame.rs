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
//! 3. The state's world tick: the scene scripts' timelines, the
//!    deformable models with something on their lists advanced as the
//!    game's character objects advance them (`MixerWalk` under the root
//!    bone, the tree updated and the bones' transforms handed over, the
//!    list swept; [`super::deform_anim`]), the particle
//!    effects' transforms and update (at most a tenth of a second, as
//!    `SpaceEffectsDrawFrame` clamps it; the mesh factory's new meshes
//!    are loaded and placed after it, D493), the client's sound tick
//!    (`TickClientSoundMixer` and the mixer's tick, with the reverb the
//!    scripts set; [`super::sound`]), then
//!    `TickMaterialAnimationPlayback` (the texture library's playbacks and
//!    the material animations).
//! 4. `BeginClientRenderFrame`: the dither and mip-filter states of
//!    `TryBeginFrame`.
//! 5. The clear (`ClearWin3DBuffers(0x4100)` after a black clear colour),
//!    the frame's camera (`Win3DApplyCamera`), then the scene: the backdrop and the star sphere through
//!    `FLAppearance`, the models' instance trees through the render manager
//!    (`RenderInstance`), the deformable models (`IDeformable::Draw`), the
//!    particle effects between `BeginDraw` and `EndDraw` (the particle part
//!    of `SpaceEffectsDrawFrame`: `LESSEQUAL` and no dithering around it,
//!    the `BATCH_PARTICLES` option handed to `SetUseMaterialBatcher`; the
//!    child effects draw as their appearances' children and the mesh
//!    appearances' meshes through the render manager).
//! 6. `ClearWin3DBuffers(0x100)`, `Win3DSetDepthFunc(4)`, `Win3DDrawAll`
//!    through the engine's draw host ([`super::ui_host::UiHost`]: the
//!    immediate-mode layer, the line batchers, the control meshes with
//!    their tints and window views, the button montage's icons, the
//!    rich-text tiles, the font atlas's overlay flush and the IME's overlay
//!    pass, with the engine's game options), the screen-quad lists the
//!    draw left, and last the software cursor
//!    (`MouseFrameUpdate`'s `DrawCursor`).
//! 7. `EndClientRenderScene`: the batcher's `FlushAll(1)` and the render
//!    states the next frame starts from; then `PresentClientRenderFrame`.
//!
//! Stubbed, recorded in the P4.16 row of design/README.md: the sound
//! mixer's tick (its sources are the game's, P5.5), the local server's
//! tick and the state machine (P5.1, P5.3), the game's own scene order
//! (the space and room cameras' `DrawScene`, P5.4, P5.9, of which the
//! frame keeps the camera's application), the fade, and the frame clock
//! (the caller's `game_app::timing`).

use crate::input::mouse::MousePlatform;
use crate::render::appearance::{AppearanceServices, Camera, MeshView};
use crate::render::ff::device::{Backend, ClearFlags};
use crate::render::particles::ParticleDraw;
use crate::render::stack::{StackAppearance, draw_deformable};
use crate::ui::message::Input;
use crate::ui::quad_batch::{FfQuadDevice, QuadBatcher};
use crate::ui::win3d_immediate::{FfPipeline, Win3dLayer, capability};

use super::ui_host::UiHost;
use super::{EffectServices, Engine, RenderSide, apply_extents};

/// `D3DRS_ZFUNC`.
const RS_ZFUNC: u32 = 23;
/// `D3DRS_DITHERENABLE`.
const RS_DITHERENABLE: u32 = 26;
/// `D3DTSS_MIPFILTER`.
const TSS_MIPFILTER: u32 = 18;
/// The longest step the effects take (`SpaceEffectsDrawFrame`'s clamp).
const EFFECT_DT_MAX: f32 = 0.1;
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
    /// Sources in the client's playing list after the sound tick.
    pub sounds: usize,
    /// Screen-quad lists flushed after the interface's draw.
    pub quad_lists: usize,
    /// Draw packets of the interface pass (the controls, their meshes,
    /// the text and the cursor).
    pub ui_draws: usize,
    /// Control instances the interface submitted.
    pub ui_meshes: usize,
    /// Rich-text tiles the interface drew.
    pub ui_tiles: usize,
    /// Button montage icons the interface queued.
    pub ui_montage_icons: usize,
    /// Whether the software cursor was drawn.
    pub cursor: bool,
}

/// `EndSceneHalf`'s states through the immediate-mode layer: fog off,
/// depth write on, blending off, the depth test on, `LESSEQUAL`, back
/// faces culled.
fn end_scene<B: Backend>(layer: &mut Win3dLayer, pipe: &mut FfPipeline<'_, B>) {
    layer.disable(pipe, capability::FOG);
    layer.set_depth_write(pipe, 1);
    layer.disable(pipe, capability::BLEND);
    layer.enable(pipe, capability::DEPTH_TEST);
    layer.set_depth_func(pipe, 4);
    layer.enable(pipe, capability::CULL_FACE);
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
        report.animations = self.advance_deformables(dt_f32);
        {
            // SpaceEffectsDrawFrame's ClampedDt: at most a tenth of a second.
            let effect_dt = if dt_f32 > EFFECT_DT_MAX {
                EFFECT_DT_MAX
            } else {
                dt_f32
            };
            let mut side = self.render.borrow_mut();
            let mut services = EffectServices {
                stack: &mut side.stack,
                manager: None,
                scene: &self.scene,
                data: self.data.as_ref(),
                meshes: Some(&mut self.fx_meshes),
            };
            for live in self.effects.iter().flatten() {
                live.effect.compose(&mut self.particles, &mut services);
                live.effect
                    .update(&mut self.particles, effect_dt, &mut services);
            }
        }
        self.settle_fx_meshes();
        // The client's sound tick (`TickClientSoundMixer`, which the game
        // states run in their update) and the reverb the scripts set.
        self.tick_sounds(dt);
        report.sounds = self.sounds_playing();
        let mut side = self.render.borrow_mut();
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
        {
            let shading = &mut side.stack.shading;
            let mut pipe = FfPipeline {
                device: &mut shading.device,
                handles: &shading.handles,
            };
            apply_camera(&mut self.win3d, &mut pipe, camera);
        }
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
        for instance in self.deformables.borrow_mut().iter_mut().flatten() {
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
            // SpaceEffectsDrawFrame's particle part: LESSEQUAL and no
            // dithering around the draw, the batcher option handed over.
            let RenderSide { stack, manager } = &mut *side;
            let z_func = ParticleDraw::render_state(stack, RS_ZFUNC);
            ParticleDraw::set_render_state(stack, RS_ZFUNC, 4);
            let dither = ParticleDraw::render_state(stack, RS_DITHERENABLE);
            ParticleDraw::set_render_state(stack, RS_DITHERENABLE, 0);
            self.particles
                .set_use_material_batcher(self.particle_batcher);
            let mut services = EffectServices {
                stack,
                manager: Some(manager),
                scene: &self.scene,
                data: self.data.as_ref(),
                meshes: Some(&mut self.fx_meshes),
            };
            if self.particles.begin_draw(&mut services) >= 0 {
                for live in self.effects.iter().flatten() {
                    live.effect.draw(&mut self.particles, camera, &mut services);
                    report.effects += 1;
                }
                self.particles.end_draw(camera, &mut services);
            }
            ParticleDraw::set_render_state(stack, RS_DITHERENABLE, dither);
            ParticleDraw::set_render_state(stack, RS_ZFUNC, z_func);
        }

        // 6. The interface over the scene.
        self.elapsed += dt;
        let ui_draws_before = side.stack.shading.device.draw_count;
        {
            let shading = &mut side.stack.shading;
            shading.device.clear(ClearFlags::DEPTH, None);
            let mut pipe = FfPipeline {
                device: &mut shading.device,
                handles: &shading.handles,
            };
            self.win3d.set_depth_func(&mut pipe, 4);
        }
        if let (Some(ui), Some(data)) = (self.ui.as_mut(), self.data.as_ref()) {
            let RenderSide { stack, manager } = &mut *side;
            let camera = ui.camera;
            let mut host = UiHost::new(
                stack,
                manager,
                data,
                self.appearance.as_ref(),
                &mut self.win3d,
                &mut self.batchers,
                &mut self.quads,
                &camera,
                super::ui_host::UiPassState {
                    options: &self.options,
                    lights: &mut self.lights,
                    ime_boxes: self.ime.as_ref().is_some_and(|f| !f.is_empty()),
                },
            );
            ui.draw_all(&mut host);
            report.ui_meshes = host.meshes;
            report.ui_tiles = host.tile_draws;
            report.ui_montage_icons = host.montage_icons;
        }
        report.quad_lists = flush_quads(&mut self.quads, &mut side.stack.shading);
        report.cursor = draw_cursor(
            &mut self.cursors,
            self.mouse.as_ref(),
            self.ui.as_ref(),
            self.data.as_ref(),
            self.elapsed,
            &mut side.stack,
        );
        let shading = &mut side.stack.shading;
        report.ui_draws = usize::try_from(shading.device.draw_count - ui_draws_before).unwrap_or(0);

        // 7. EndClientRenderScene and PresentClientRenderFrame.
        side.stack.flush_all(1);
        {
            let shading = &mut side.stack.shading;
            let mut pipe = FfPipeline {
                device: &mut shading.device,
                handles: &shading.handles,
            };
            end_scene(&mut self.win3d, &mut pipe);
        }
        side.stack.shading.device.present();
        // `Win3DOnPresentFrame`: the cache follows the `LIKE_PETAL` option.
        self.win3d
            .on_present_frame(self.options.get(super::options::LIKE_PETAL));
        drop(side);
        apply_extents(&self.scene, &self.links);
        self.frames += 1;
        report
    }
}

/// `Win3DApplyCamera`: the perspective from the camera's vertical angle,
/// aspect and planes (read far, near, aspect, field of view) through the
/// layer's projection cache, then its view transform as the modelview;
/// with the pipeline's view-space lights off, an identity view and the
/// camera's view as the world.
fn apply_camera<B: Backend>(
    layer: &mut Win3dLayer,
    pipe: &mut FfPipeline<'_, B>,
    camera: &mut dyn Camera,
) {
    let far = camera.z_far();
    let near = camera.z_near();
    let aspect = camera.aspect();
    let fov = camera.fov_y_deg();
    let view = camera.view_transform();
    layer.apply_camera(pipe, fov, aspect, near, far, &view);
}

/// `DrawCursor` at the end of `MouseFrameUpdate`: the cursor in use
/// animated on the client clock and drawn at the software cursor as one
/// pre-transformed quad (divergence D346). Whether it was drawn.
fn draw_cursor<B: Backend + 'static>(
    cursors: &mut crate::ui::cursor::Cursors,
    mouse: Option<&crate::input::mouse::Mouse>,
    ui: Option<&crate::ui::tree::Ui>,
    data: Option<&neuron_core::vfs::Host>,
    elapsed: f64,
    stack: &mut crate::render::stack::RenderStack<B>,
) -> bool {
    let (Some(mouse), Some(ui), Some(data)) = (mouse, ui, data) else {
        return false;
    };
    if !cursors.ready {
        return false;
    }
    #[allow(clippy::cast_possible_truncation)]
    let Some(shape) = cursors.animate(elapsed as f32) else {
        return false;
    };
    let Some(cursor) = cursors.current() else {
        return false;
    };
    let handle = {
        let mut lib = super::StackShapes::new(stack, data);
        shape.gpu_handle(&mut lib, 0)
    };
    let (x, y, _) = mouse.cursor();
    let surface = ui.camera.surface;
    let shading = &mut stack.shading;
    let mut device = FfQuadDevice {
        device: &mut shading.device,
        handles: &shading.handles,
    };
    crate::ui::cursor::draw(&mut device, cursor, &shape, handle, (x, y), &surface);
    true
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
