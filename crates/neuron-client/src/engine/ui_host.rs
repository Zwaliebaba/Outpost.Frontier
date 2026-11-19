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
//! `Win3DBeginDraw` and the batch reset run in [`DrawHost::begin_draw`];
//! the line batchers' flush, the font atlas's overlay flush and the
//! batcher's bin order in [`DrawHost::end_draw`]. The big-image tiles a
//! rich-text window hands back are drawn (`ScreenQuadDrawBigImageTile`)
//! at the host's next call (divergence D483).

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
use crate::ui::quad_batch::{QuadBatcher, QuadDevice};
use crate::ui::shape::ShapeTextures;
use crate::ui::tree::{DrawHost, MeshPass};
use crate::ui::widescreen::shear_modelview;
use crate::ui::win3d_immediate::{FfPipeline, Win3dLayer, capability, rs};
use crate::ui::win3d_renderables::{
    Batch, LineBatchers, Win3dView, font_atlas_key, prep_ui_overlay,
};

use super::StackShapes;

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
    /// `GetGameOption(SHIP WIRES)` (0xB), which gates the tinted submits.
    pub ship_wires: bool,
    /// Control instances submitted.
    pub meshes: usize,
    /// Big-image tiles drawn.
    pub tile_draws: usize,
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
            ship_wires: true,
            meshes: 0,
            tile_draws: 0,
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
        let mut view = Win3dMeshView(&mut self.camera);
        self.manager
            .render_instance(self.textures.0, &mut view, instance, 1.0, &request);
        self.meshes += 1;
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
        if !self.ship_wires {
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
            text_enabled: true,
        })
    }

    /// `Win3DBeginDraw` (lighting, texturing, culling, depth test and fog
    /// off, the camera's viewport and perspective, the batcher's second
    /// bin first, the ambient white and no dithering), then the line
    /// batchers' reset.
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
        self.set_render_state(rs::AMBIENT, 0xFFFF_FFFF);
        self.set_render_state(rs::DITHERENABLE, 0);
        self.batchers.reset();
    }

    /// The line batchers' flush, the font atlas's overlay flush and the
    /// batcher's bin order back.
    fn end_draw(&mut self) {
        self.drain_tiles();
        let shading = &mut self.textures.0.shading;
        let mut pipe = FfPipeline {
            device: &mut shading.device,
            handles: &shading.handles,
        };
        let view = Win3dView::of(&mut self.camera);
        self.batchers.flush_all(self.layer, &mut pipe, &view);
        prep_ui_overlay(self.layer, &mut pipe, self.quads, font_atlas_key());
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
                front_cull, scale, ..
            } => {
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
            MeshPass::Direct { scale, .. } => {
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
                let mut view = Win3dMeshView(&mut self.camera);
                self.manager
                    .render_instance(self.textures.0, &mut view, instance, 1.0, &request);
                self.meshes += 1;
            }
        }
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        &mut self.any
    }
}
