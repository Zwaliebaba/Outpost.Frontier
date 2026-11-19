//! The engine's interface draw (`Win3DDrawAll` and the calls its controls
//! make): [`UiHost`] is the [`DrawHost`] the frame hands the UI manager,
//! over the engine's render stack, render manager, texture library and
//! data. It is the controls' immediate-mode layer ([`Immediate`] on the
//! [`Win3dLayer`] and the line batchers), their texture-shape library
//! ([`ShapeTextures`] through [`StackShapes`]), the texture pipeline and
//! quad sink of their string paints and rich-text windows, and the mesh
//! pass of their engine instances (`SubmitWin3DEngineInstance` and its
//! tinted, material and outlined variants of `win3d_engine_submit.cpp`)
//! through the render manager with the Win3D camera.
//!
//! `Win3DBeginDraw` (with the engine lights' clear) and the batch reset
//! run in [`DrawHost::begin_draw`]; the button montage's flush, the line
//! batchers' flush, the font atlas's overlay flush, the IME's overlay pass
//! and the batcher's bin order in [`DrawHost::end_draw`]. The big-image
//! tiles a rich-text window hands back are drawn
//! (`ScreenQuadDrawBigImageTile`) at the host's next call (divergence
//! D482). A body pass writes its tint into the holder's floats, which the
//! interface's materials read as the instance's user data (D440); a static
//! control's window view fits a camera of its own to the window (D441).
//! The draw reads the engine's game options ([`GameOptions`], D512).

use std::any::Any;

use neuron_core::image::dds::ImageInfo;
use neuron_core::image::{FormatMaps, PixelFormat};
use neuron_core::material_data::PropertyValue;
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::vfs::Host;

use crate::render::appearance::Appearance;
use crate::render::ff::device::Backend;
use crate::render::mesh::RenderManager;
use crate::render::mesh::manager::DrawRequest;
use crate::render::mesh::services::MeshCamera;
use crate::render::stack::RenderStack;
use crate::render::texture::big_image::TileDraw;
use crate::render::texture::pipeline::{Abilities, GR_GENERIC, GR_INVALID_PARMS, TexturePipeline};
use crate::text::font::string::PaintCx;
use crate::ui::camera::Camera;
use crate::ui::control::ControlMesh;
use crate::ui::controls::rich_text_win::RichTextCx;
use crate::ui::immediate::Immediate;
use crate::ui::montage;
use crate::ui::quad_batch::{IconCamera, QuadBatcher, QuadDevice};
use crate::ui::services::MontageTile;
use crate::ui::shape::ShapeTextures;
use crate::ui::tree::{DrawHost, MeshPass, WindowView};
use crate::ui::widescreen::shear_modelview;
use crate::ui::win3d_immediate::{FfPipeline, QUERY_VIEWPORT, Win3dLayer, capability, rs};
use crate::ui::win3d_renderables::{
    Batch, LineBatchers, Win3dView, font_atlas_key, ime_focus_draw_pass, prep_ui_overlay,
};

use super::StackShapes;
use super::lights::EngineLights;
use super::options::{self, GameOptions};

/// The Win3D camera as the render manager's camera.
pub struct Win3dMeshView<'a>(pub &'a mut Camera);

impl MeshCamera for Win3dMeshView<'_> {
    fn view_transform(&mut self) -> [f32; 12] {
        self.0.view_transform().to_words()
    }
    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32 {
        self.0.classify_sphere(centre, radius)
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        self.0.half_fov_x_deg
    }
    fn viewport(&mut self) -> [i32; 4] {
        self.0.viewport
    }
    fn x_scale_over_near(&mut self) -> f64 {
        self.0.x_scale_over_near()
    }
    fn z_near(&mut self) -> f32 {
        self.0.z_near
    }
    fn position(&mut self) -> [f32; 3] {
        let p = self.0.world_pos;
        [p.x, p.y, p.z]
    }
}

/// [`TexturePipeline`] on the render stack's device and texture numbers.
pub struct StackTextures<'a, B: Backend + 'static>(pub &'a mut RenderStack<B>);

impl<B: Backend + 'static> TexturePipeline for StackTextures<'_, B> {
    fn abilities(&self) -> Abilities {
        self.0.shading.abilities
    }
    fn create_texture(
        &mut self,
        width: u32,
        height: u32,
        format: PixelFormat,
        levels: u32,
    ) -> Result<u32, i32> {
        self.0
            .shading
            .pipeline()
            .create_texture(width, height, format, levels)
    }
    fn set_level_data(
        &mut self,
        handle: u32,
        level: u32,
        width: u32,
        height: u32,
        pitch: u32,
        format: PixelFormat,
        pixels: &[u8],
        palette: Option<&[u8]>,
    ) -> i32 {
        self.0
            .shading
            .pipeline()
            .set_level_data(handle, level, width, height, pitch, format, pixels, palette)
    }
    fn load_cubemap(&mut self, data: &[u8], info: Option<&ImageInfo>) -> Result<u32, i32> {
        self.0.shading.pipeline().load_cubemap(data, info)
    }
    fn destroy_texture(&mut self, handle: u32) -> i32 {
        self.0.shading.pipeline().destroy_texture(handle)
    }
    fn texture_format(&self, handle: u32) -> Result<PixelFormat, i32> {
        let shading = &self.0.shading;
        let texture = shading.handles.get(handle).ok_or(GR_INVALID_PARMS)?;
        shading
            .device
            .textures
            .get(texture)
            .map(|t| t.format)
            .map_err(|_| GR_GENERIC)
    }
}

/// `D3DCMP_LESSEQUAL` and `D3DCMP_ALWAYS`.
const CMP_LESS_EQUAL: u32 = 4;
const CMP_ALWAYS: u32 = 8;
/// `kInv255`: the holder tint's byte scale (a `float`).
const INV_255: f32 = 1.0 / 255.0;

/// What the interface pass reads of the rest of the engine besides the
/// render parts: the game options, the engine lights' registered list and
/// whether the IME focus has input boxes.
pub struct UiPassState<'a> {
    pub options: &'a GameOptions,
    pub lights: &'a mut EngineLights,
    /// `IMEFocus::GetSingleton()` and `NumInputBoxes() != 0`.
    pub ime_boxes: bool,
}

/// `SetHolderTint`: the bytes R, G, B and A over 255 into floats 0, 1, 2
/// and 4 (float 3 keeps its value).
#[must_use]
pub fn holder_tint(floats: [f32; 5], rgba: [u8; 4]) -> [f32; 5] {
    let f = |b: u8| (f64::from(b) * f64::from(INV_255)) as f32;
    [f(rgba[0]), f(rgba[1]), f(rgba[2]), floats[3], f(rgba[3])]
}

/// A static control's window camera (`SpaceCameraSetViewportEx`, the
/// near and far planes, `SpaceCameraSetFovY` on the control's own
/// `FSpaceCamera`, which sits at the origin looking down -Z).
#[must_use]
pub fn window_camera(surface: crate::ui::widescreen::Surface, view: &WindowView) -> Camera {
    let mut camera = Camera::new(surface);
    camera.set_viewport(view.rect);
    camera.set_near_far(view.z_near, view.z_far);
    camera.set_fov_y(view.fov_y_deg);
    camera
}

/// The draw host of the engine's interface pass.
pub struct UiHost<'a, B: Backend + 'static> {
    textures: StackTextures<'a, B>,
    manager: &'a mut RenderManager<RenderStack<B>>,
    data: &'a Host,
    appearance: Option<&'a Appearance>,
    layer: &'a mut Win3dLayer,
    batchers: &'a mut LineBatchers,
    quads: &'a mut QuadBatcher,
    formats: FormatMaps,
    camera: Camera,
    base_center_x: f32,
    shift: f32,
    tiles: Vec<TileDraw>,
    options: GameOptions,
    lights: &'a mut EngineLights,
    ime_boxes: bool,
    /// A static control's window camera and the viewport it saved.
    window: Option<(Camera, [i32; 4])>,
    /// Control instances submitted.
    pub meshes: usize,
    /// Big-image tiles drawn.
    pub tile_draws: usize,
    /// Montage icons queued.
    pub montage_icons: usize,
    any: (),
}

impl<'a, B: Backend + 'static> UiHost<'a, B> {
    /// The host over the engine's parts, with the Win3D camera.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        stack: &'a mut RenderStack<B>,
        manager: &'a mut RenderManager<RenderStack<B>>,
        data: &'a Host,
        appearance: Option<&'a Appearance>,
        layer: &'a mut Win3dLayer,
        batchers: &'a mut LineBatchers,
        quads: &'a mut QuadBatcher,
        camera: &Camera,
        state: UiPassState<'a>,
    ) -> UiHost<'a, B> {
        let formats = stack.shading.textures.format_maps().clone();
        UiHost {
            textures: StackTextures(stack),
            manager,
            data,
            appearance,
            layer,
            batchers,
            quads,
            formats,
            camera: *camera,
            base_center_x: camera.screen_center_x,
            shift: 0.0,
            tiles: Vec::new(),
            options: state.options.clone(),
            lights: state.lights,
            ime_boxes: state.ime_boxes,
            window: None,
            meshes: 0,
            tile_draws: 0,
            montage_icons: 0,
            any: (),
        }
    }

    /// The layer and the pipeline over the device, apart.
    fn parts(&mut self) -> (&mut Win3dLayer, FfPipeline<'_, B>) {
        let shading = &mut self.textures.0.shading;
        (
            &mut *self.layer,
            FfPipeline {
                device: &mut shading.device,
                handles: &shading.handles,
            },
        )
    }

    fn apply_camera(&mut self) {
        let view = Win3dView::of(&mut self.camera);
        let (layer, mut pipe) = self.parts();
        view.apply(layer, &mut pipe);
    }

    fn flush_batches(&mut self) {
        let shading = &mut self.textures.0.shading;
        let mut pipe = FfPipeline {
            device: &mut shading.device,
            handles: &shading.handles,
        };
        let view = Win3dView::of(&mut self.camera);
        self.batchers.flush_all(self.layer, &mut pipe, &view);
        self.batchers.reset();
    }

    /// The tiles rich-text windows handed back since the last call, drawn.
    fn drain_tiles(&mut self) {
        if self.tiles.is_empty() {
            return;
        }
        let tiles = std::mem::take(&mut self.tiles);
        let shading = &mut self.textures.0.shading;
        let mut pipe = FfPipeline {
            device: &mut shading.device,
            handles: &shading.handles,
        };
        for tile in tiles {
            self.quads.draw_big_image_tile(
                self.layer,
                &mut pipe,
                tile.handle,
                tile.color,
                tile.dest,
                tile.uv,
            );
            self.tile_draws += 1;
        }
    }

    fn render_state(&mut self, state: u32) -> u32 {
        self.parts().1.render_state(state)
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        self.parts().1.set_render_state(state, value);
    }

    /// `ApplyCullAndLighting`: a front-culled or lit-off byte gives
    /// `LESSEQUAL` and no depth write, else `ALWAYS` and depth write.
    fn depth_pair(&mut self, byte: bool) {
        if byte {
            self.set_render_state(rs::ZFUNC, CMP_LESS_EQUAL);
            self.set_render_state(rs::ZWRITEENABLE, 0);
        } else {
            self.set_render_state(rs::ZFUNC, CMP_ALWAYS);
            self.set_render_state(rs::ZWRITEENABLE, 1);
        }
    }

    /// `IRenderer2::RenderInstance(camera, instance, 1.0, flags, matrix,
    /// material)` with the scaled identity of a scale other than 1.
    fn render(&mut self, instance: usize, mut flags: u32, scale: f32, extra: Option<u32>) {
        let matrix = (scale != 1.0).then(|| {
            flags |= 0x30;
            let mut m = Matrix::IDENTITY;
            for (i, row) in m.m.iter_mut().enumerate() {
                for (j, v) in row.iter_mut().enumerate() {
                    *v = ((if i == j { 1.0f64 } else { 0.0 }) * f64::from(scale)) as f32;
                }
            }
            Transform::new(m, Vector::ZERO)
        });
        let request = DrawRequest {
            flags,
            matrix,
            extra: extra.and_then(|id| self.material_id(id)),
        };
        let camera = match self.window.as_mut() {
            Some((camera, _)) => camera,
            None => &mut self.camera,
        };
        let mut view = Win3dMeshView(camera);
        self.manager
            .render_instance(self.textures.0, &mut view, instance, 1.0, &request);
        self.meshes += 1;
    }

    /// `SetHolderTint` on the floats instance `instance` has as its user
    /// data.
    fn set_holder_tint(&mut self, instance: usize, rgba: [u8; 4]) {
        if let Some(floats) = self.textures.0.user_floats.borrow().get(&instance) {
            floats.set(holder_tint(floats.get(), rgba));
        }
    }

    fn material_id(&self, index: u32) -> Option<crate::render::material::MaterialId> {
        self.appearance?.override_material(index).ok()
    }

    /// Parameter 0 of an override material, the colour bytes over 255.
    fn tint_material(&mut self, index: u32, rgba: [u8; 4], param_10: Option<bool>) {
        let Some(id) = self.material_id(index) else {
            return;
        };
        let mut words = [0u32; 4];
        for (w, b) in words.iter_mut().zip(rgba) {
            *w = ((f64::from(b) * 0.003_921_568_627_450_98) as f32).to_bits();
        }
        let colour = PropertyValue {
            kind: 4,
            words,
            text: None,
        };
        let _ = self.textures.0.set_material_parameter(id, 0, &colour);
        if let Some(flag) = param_10 {
            let value = PropertyValue {
                kind: 1,
                words: [u32::from(flag), 0, 0, 0],
                text: None,
            };
            let _ = self.textures.0.set_material_parameter(id, 0xA, &value);
        }
    }

    /// `SubmitWin3DTintedEngineInstance` with material selector 1.
    fn submit_tinted(
        &mut self,
        instance: usize,
        flags: u32,
        rgba: [u8; 4],
        byte: bool,
        scale: f32,
    ) {
        if self.options.get(options::SHIP_WIRES) == 0 {
            return;
        }
        let saved_func = self.render_state(rs::ZFUNC);
        let saved_write = self.render_state(rs::ZWRITEENABLE);
        self.depth_pair(byte);
        self.tint_material(1, rgba, None);
        self.render(instance, flags, scale, Some(1));
        self.set_render_state(rs::ZFUNC, saved_func);
        self.set_render_state(rs::ZWRITEENABLE, saved_write);
    }

    /// `SubmitWin3DMaterialEngineInstance` with override material 0.
    #[allow(clippy::too_many_arguments)]
    fn submit_material(
        &mut self,
        instance: usize,
        front_cull: bool,
        rgba: [u8; 4],
        byte: bool,
        scale: f32,
        param_10: bool,
        flags: u32,
    ) {
        let saved_func = self.render_state(rs::ZFUNC);
        let saved_write = self.render_state(rs::ZWRITEENABLE);
        self.depth_pair(byte);
        self.tint_material(0, rgba, Some(param_10));
        let mut flags = flags | 0x100;
        if !front_cull {
            flags |= 0x2000;
        }
        self.render(instance, flags, scale, Some(0));
        self.set_render_state(rs::ZFUNC, saved_func);
        self.set_render_state(rs::ZWRITEENABLE, saved_write);
    }
}

impl<B: Backend + 'static> Immediate for UiHost<'_, B> {
    fn begin(&mut self, primitive: u32) {
        self.layer.begin(primitive);
    }
    fn end(&mut self) {
        let (layer, mut pipe) = self.parts();
        layer.end(&mut pipe);
    }
    fn vertex(&mut self, x: f32, y: f32, z: f32) {
        self.layer.vertex(x, y, z);
    }
    fn color3ub(&mut self, r: u8, g: u8, b: u8) {
        self.layer.color3ub(r, g, b);
    }
    fn color4ubv(&mut self, rgba: [u8; 4]) {
        self.layer.color4ub(rgba[0], rgba[1], rgba[2], rgba[3]);
    }
    fn tex_coord(&mut self, u: f32, v: f32) {
        self.layer.tex_coord(u, v);
    }
    fn enable(&mut self, cap: u32) {
        let (layer, mut pipe) = self.parts();
        layer.enable(&mut pipe, cap);
    }
    fn disable(&mut self, cap: u32) {
        let (layer, mut pipe) = self.parts();
        layer.disable(&mut pipe, cap);
    }
    fn bind_texture(&mut self, handle: u32) {
        self.layer.bind_texture(handle);
    }
    fn push_projection(&mut self) {
        let (layer, mut pipe) = self.parts();
        layer.push_projection(&mut pipe);
    }
    fn pop_projection(&mut self) {
        let (layer, mut pipe) = self.parts();
        layer.pop_projection(&mut pipe);
    }
    fn set_ortho_2d(&mut self) {
        let (layer, mut pipe) = self.parts();
        layer.set_ortho_2d(&mut pipe);
    }
    fn bind_modelview(&mut self, transform: &Transform) {
        let sheared = shear_modelview(&self.camera, self.shift, transform).unwrap_or(*transform);
        let words = sheared.to_words();
        let (layer, mut pipe) = self.parts();
        layer.bind_modelview(&mut pipe, None, &words, &words);
    }
    fn set_render_state(&mut self, state: u32, value: u32) {
        UiHost::set_render_state(self, state, value);
    }
    fn batch_line(&mut self, from: Vector, to: Vector, argb: u32, rgb: u32, width: f32) {
        let shading = &mut self.textures.0.shading;
        let mut pipe = FfPipeline {
            device: &mut shading.device,
            handles: &shading.handles,
        };
        let view = Win3dView::of(&mut self.camera);
        self.batchers.add_line(
            Batch::A,
            self.layer,
            &mut pipe,
            &view,
            [from.x, from.y, from.z],
            [to.x, to.y, to.z],
            argb,
            rgb,
            width,
        );
    }
    fn flush_batches(&mut self) {
        UiHost::flush_batches(self);
    }
    fn set_depth_write(&mut self, on: bool) {
        let (layer, mut pipe) = self.parts();
        layer.set_depth_write(&mut pipe, u32::from(on));
    }
}

impl<B: Backend + 'static> ShapeTextures for UiHost<'_, B> {
    fn find(&mut self, name: &[u8]) -> Option<u32> {
        StackShapes::new(self.textures.0, self.data).find(name)
    }
    fn add_ref(&mut self, record: u32) {
        StackShapes::new(self.textures.0, self.data).add_ref(record);
    }
    fn release(&mut self, record: u32) {
        StackShapes::new(self.textures.0, self.data).release(record);
    }
    fn load_group(&mut self, file: &[u8]) -> Option<i32> {
        StackShapes::new(self.textures.0, self.data).load_group(file)
    }
    fn garbage_collect(&mut self, group: i32) {
        StackShapes::new(self.textures.0, self.data).garbage_collect(group);
    }
    fn generic_slot_4(&mut self) -> u32 {
        StackShapes::new(self.textures.0, self.data).generic_slot_4()
    }
    fn set_generic_slot_4(&mut self, value: u32) {
        StackShapes::new(self.textures.0, self.data).set_generic_slot_4(value);
    }
    fn frame_handle(&mut self, record: u32, frame: i32) -> u32 {
        StackShapes::new(self.textures.0, self.data).frame_handle(record, frame)
    }
    fn texture_dim(&mut self, handle: u32) -> (u32, u32) {
        StackShapes::new(self.textures.0, self.data).texture_dim(handle)
    }
}

impl<B: Backend + 'static> DrawHost for UiHost<'_, B> {
    fn quads(&mut self) -> &mut QuadBatcher {
        self.quads
    }

    fn paint_cx(&mut self) -> Option<PaintCx<'_>> {
        Some(PaintCx {
            pipeline: &mut self.textures,
            sink: &mut *self.quads,
            text_enabled: self.options.get(options::TEXT) != 0,
        })
    }

    fn interface_enabled(&self) -> bool {
        self.options.get(options::INTERFACE) != 0
    }

    fn game_option(&self, index: u32) -> i32 {
        self.options.get(index)
    }

    fn scroll_bars_enabled(&self) -> bool {
        self.options.get(options::DRAW_SCROLL_BAR_CONTROLS) != 0
    }

    /// `Win3DBeginDraw` (lighting, texturing, culling, depth test and fog
    /// off, the camera's viewport and perspective, the batcher's second
    /// bin first, the engine lights cleared, the ambient white and no
    /// dithering), then the line batchers' reset. The button montage is
    /// loaded already (`ButtonMontageEnsure`, D511).
    fn begin_draw(&mut self, camera: &Camera) {
        self.camera = *camera;
        self.base_center_x = camera.screen_center_x;
        self.shift = 0.0;
        {
            let (layer, mut pipe) = self.parts();
            for cap in [
                capability::LIGHTING,
                capability::TEXTURE_2D,
                capability::CULL_FACE,
                capability::DEPTH_TEST,
                capability::FOG,
            ] {
                layer.disable(&mut pipe, cap);
            }
        }
        let viewport = self.camera.viewport;
        {
            let (layer, mut pipe) = self.parts();
            layer.set_viewport(&mut pipe, Some(viewport));
        }
        self.apply_camera();
        let _ = self.textures.0.with_batcher(|b, _| b.set_bin_order(1, -1));
        self.clear_engine_lights();
        self.set_render_state(rs::AMBIENT, 0xFFFF_FFFF);
        self.set_render_state(rs::DITHERENABLE, 0);
        self.batchers.reset();
    }

    /// `Win3DDrawAll`'s tail: the button montage's flush, the line
    /// batchers' flush, the font atlas's overlay flush, the IME's overlay
    /// pass, and the batcher's bin order back.
    fn end_draw(&mut self) {
        self.drain_tiles();
        let shading = &mut self.textures.0.shading;
        let mut pipe = FfPipeline {
            device: &mut shading.device,
            handles: &shading.handles,
        };
        montage::flush_buttons(&mut pipe, self.quads);
        let view = Win3dView::of(&mut self.camera);
        self.batchers.flush_all(self.layer, &mut pipe, &view);
        prep_ui_overlay(self.layer, &mut pipe, self.quads, font_atlas_key());
        if self.ime_boxes {
            ime_focus_draw_pass(self.layer, &mut pipe, self.quads);
        }
        let _ = self.textures.0.with_batcher(|b, _| b.set_bin_order(1, 1));
    }

    /// `FWidescreenHudShiftScope` with `bDraw`: the batches queued under
    /// the old camera flushed, then the new camera applied.
    fn camera_changed(&mut self, camera: &Camera) {
        self.drain_tiles();
        self.flush_batches();
        self.camera = *camera;
        self.shift = (f64::from(camera.screen_center_x) - f64::from(self.base_center_x)) as f32;
        self.apply_camera();
    }

    fn rich_text_cx(&mut self) -> Option<RichTextCx<'_>> {
        self.drain_tiles();
        let device = &self.textures.0.shading.device;
        let v = device.viewport();
        let i = |x: u32| i32::try_from(x).unwrap_or(i32::MAX);
        let projection = device.transforms.projection();
        Some(RichTextCx {
            viewport: [i(v.x), i(v.y), i(v.width), i(v.height)],
            projection,
            pipeline: &mut self.textures,
            formats: &self.formats,
            tiles: &mut self.tiles,
        })
    }

    fn immediate(&mut self) -> Option<&mut dyn Immediate> {
        self.drain_tiles();
        Some(self)
    }

    fn shape_textures(&mut self) -> Option<&mut dyn ShapeTextures> {
        Some(self)
    }

    fn submit_mesh(&mut self, mesh: &dyn ControlMesh, pass: MeshPass) {
        self.drain_tiles();
        let Some(instance) = mesh.engine_instance() else {
            return;
        };
        let instance = instance as usize;
        match pass {
            MeshPass::Plain => {
                self.set_render_state(rs::ZFUNC, CMP_ALWAYS);
                self.set_render_state(rs::ZWRITEENABLE, 1);
                self.depth_pair(false);
                self.render(instance, 0, 1.0, None);
            }
            MeshPass::Body {
                tint,
                front_cull,
                scale,
            } => {
                self.set_holder_tint(instance, tint);
                self.depth_pair(front_cull);
                self.render(instance, 0, scale, None);
            }
            MeshPass::Tinted { tint, lighting } => {
                self.submit_tinted(instance, 0x8200, tint, lighting, 1.0);
            }
            MeshPass::TintedScaled {
                tint,
                front_cull,
                lighting,
                scale,
            } => {
                let flags = if front_cull { 0x8200 } else { 0xA200 };
                self.submit_tinted(instance, flags, tint, lighting, scale);
            }
            MeshPass::Outlined { wire, scale } => {
                self.submit_material(instance, true, [0; 4], false, scale, false, 0);
                self.submit_tinted(instance, 0x8100, wire, true, scale);
            }
            MeshPass::Direct { tint, scale } => {
                self.set_holder_tint(instance, tint);
                self.set_render_state(rs::ZFUNC, CMP_ALWAYS);
                self.set_render_state(rs::ZWRITEENABLE, 1);
                let flags = 0x30;
                let mut m = Matrix::IDENTITY;
                for (i, row) in m.m.iter_mut().enumerate() {
                    row[i] = scale;
                }
                let request = DrawRequest {
                    flags,
                    matrix: Some(Transform::new(m, Vector::ZERO)),
                    extra: None,
                };
                let camera = match self.window.as_mut() {
                    Some((camera, _)) => camera,
                    None => &mut self.camera,
                };
                let mut view = Win3dMeshView(camera);
                self.manager
                    .render_instance(self.textures.0, &mut view, instance, 1.0, &request);
                self.meshes += 1;
            }
        }
    }

    /// `StaticControl::Draw`'s window: the viewport saved
    /// (`Win3DGetViewport`), a camera fitted to the rectangle with the
    /// Win3D camera's planes and the vertical angle
    /// (`SpaceCameraSetViewportEx`, `SpaceCameraSetFovY`) made current
    /// (`Win3DSetViewport`, `Win3DApplyCameraPerspective`); `None` applies
    /// the Win3D camera and puts the saved viewport back
    /// (`Win3DApplyCamera`, `Win3DSetViewportRect`).
    fn window_view(&mut self, view: Option<WindowView>) {
        self.drain_tiles();
        match view {
            Some(view) => {
                let saved = self.layer.get_viewport(QUERY_VIEWPORT).unwrap_or([0; 4]);
                let camera = window_camera(self.camera.surface, &view);
                let (layer, mut pipe) = self.parts();
                layer.set_viewport(&mut pipe, Some(camera.viewport));
                layer.set_perspective(
                    &mut pipe,
                    camera.fov_y_deg,
                    camera.aspect,
                    camera.z_near,
                    camera.z_far,
                );
                self.window = Some((camera, saved));
            }
            None => {
                let Some((_, saved)) = self.window.take() else {
                    return;
                };
                self.apply_camera();
                let (layer, mut pipe) = self.parts();
                layer.set_viewport_rect(&mut pipe, saved[0], saved[1], saved[2], saved[3]);
            }
        }
    }

    /// `EngineLightClearRegisteredList`.
    fn clear_engine_lights(&mut self) {
        let lights = &mut self.textures.0.shading.device.lights;
        self.lights.clear_registered(lights);
    }

    /// `NnMontageDrawTileAt(tile, where, rgba, size, 0, 0)` through the
    /// Win3D camera.
    fn draw_montage_tile(
        &mut self,
        tile: Option<MontageTile>,
        at: Vector,
        rgba: [u8; 4],
        size: f32,
    ) -> bool {
        self.drain_tiles();
        let camera = IconCamera::of(&mut self.camera);
        let mut lib = StackShapes::new(self.textures.0, self.data);
        let drawn = montage::draw_tile_at(
            self.quads,
            &mut lib,
            &camera,
            tile.as_ref(),
            [at.x, at.y, at.z],
            rgba,
            size,
            0,
            0.0,
        );
        if drawn {
            self.montage_icons += 1;
        }
        drawn
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        &mut self.any
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ui::widescreen::Surface;

    /// The window camera of four rectangles on a 1024 x 768 surface, against
    /// a local 32-bit x87 build of the reference's `SpaceCameraSetViewport`
    /// and `SpaceCameraSetFovY` (space_camera.cpp cannot be compiled into the
    /// oracle): aspect, half angle, vertical angle, scales, planes, centre.
    #[test]
    fn the_window_camera_is_the_references() {
        let expected: [([i32; 4], [u32; 11]); 4] = [
            (
                [322, 221, 456, 311],
                [
                    0x3fbd_e3de,
                    0x4238_5b8e,
                    0x420c_0000,
                    0x4281_f623,
                    0xc281_f623,
                    0x3f31_8b83,
                    0x3f38_6da3,
                    0x3f51_b3f3,
                    0x3f12_d5e8,
                    0x43c2_c000,
                    0x4385_4000,
                ],
            ),
            (
                [0, 0, 1023, 767],
                [
                    0x3faa_aaab,
                    0x422c_2258,
                    0x420c_0000,
                    0x4409_1a2a,
                    0xc409_1a2a,
                    0x3f3b_1fd3,
                    0x3f2e_b37e,
                    0x3f51_b3f3,
                    0x3f12_d5e8,
                    0x4400_0000,
                    0x43c0_0000,
                ],
            ),
            (
                [10, 600, 90, 700],
                [
                    0x3f4d_4e93,
                    0x41ea_8831,
                    0x420c_0000,
                    0x4290_3e32,
                    0xc290_3e32,
                    0x3f5f_36b1,
                    0x3efa_b141,
                    0x3f51_b3f3,
                    0x3f12_d5e8,
                    0x424a_0000,
                    0x4422_a000,
                ],
            ),
            (
                [205, 161, 273, 230],
                [
                    0x3f7c_57c5,
                    0x420a_7457,
                    0x420c_0000,
                    0x4247_f0d4,
                    0xc247_f0d3,
                    0x3f52_b038,
                    0x3f11_6b06,
                    0x3f51_b3f3,
                    0x3f12_d5e8,
                    0x436f_8000,
                    0x4344_0000,
                ],
            ),
        ];
        for (rect, bits) in expected {
            let c = window_camera(
                Surface::new(1024, 768),
                &WindowView {
                    rect,
                    z_near: 0.1,
                    z_far: 1000.0,
                    fov_y_deg: 35.0,
                },
            );
            let got = [
                c.aspect,
                c.half_fov_x_deg,
                c.fov_y_deg,
                c.x_scale,
                c.y_scale,
                c.plane_x[0],
                c.plane_x[2],
                c.plane_y[1],
                c.plane_y[2],
                c.screen_center_x,
                c.screen_center_y,
            ]
            .map(f32::to_bits);
            assert_eq!(got, bits, "{rect:?}");
            assert_eq!((c.z_near, c.z_far), (0.1, 1000.0));
        }
    }

    #[test]
    fn the_holder_tint_keeps_float_three() {
        let t = holder_tint([1.0, 1.0, 1.0, 0.5, 1.0], [255, 0, 128, 64]);
        assert_eq!(t[3], 0.5);
        assert_eq!(t[0], 1.0);
        assert_eq!(t[1], 0.0);
        assert_eq!(t[2].to_bits(), 0x3f00_8081);
    }
}
