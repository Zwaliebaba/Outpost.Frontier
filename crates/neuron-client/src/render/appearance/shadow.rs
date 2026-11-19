//! The blob shadows (`DrawShadowLike`, `shadows_inl.h`): one textured quad
//! of `efxshadow` under each instance, faded and scaled by its height,
//! drawn as a triangle list of FVF 0x142 vertices (position, diffuse
//! colour, one texture coordinate pair) in vertex memory.
//!
//! The arithmetic is the reference's x87 order in `f64`, rounded to `f32`
//! at each store to a float (decision T11); the quad's angle goes through
//! `atan2`, `sin` and `cos` (divergence D322).

use neuron_core::diag::{self, Severity};
use neuron_core::hash::name_crc32;
use neuron_core::num::trunc_to_i32;

use crate::render::material::{GR_GENERIC, GR_OK};
use crate::render::mesh::Instance;

use super::Appearance;
use super::camera::Camera;
use super::services::AppearanceServices;

/// An instance to shade and its radius (`FShadowInstance`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ShadowInstance {
    /// The engine instance.
    pub instance: Instance,
    /// Its radius: the quad's height above the instance's origin.
    pub radius: f32,
}

/// What a shadow draw does with the height (the four flag bytes of the
/// reference's slots 11 and 12).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ShadowFlags {
    /// Fade the opacity with the height (`0.8 - fade`), else 0.6.
    pub fade_alpha: bool,
    /// Scale the quad with the height (the scale stays for later draws).
    pub fade_scale: bool,
    /// Add the extents' top to the radius.
    pub raise: bool,
    /// Take the extents from the renderer's bounds of the archetype.
    pub bounds: bool,
}

/// `DrawFadingShadows`' flags (`G_Slot11FlagA` to `D`).
pub const FADING: ShadowFlags = ShadowFlags {
    fade_alpha: true,
    fade_scale: true,
    raise: false,
    bounds: true,
};

/// `DrawFixedShadows`' flags (`G_Slot12FlagA` to `D`).
pub const FIXED: ShadowFlags = ShadowFlags {
    fade_alpha: false,
    fade_scale: false,
    raise: true,
    bounds: false,
};

/// The vertex format (`kShadowFvf`, XYZ | DIFFUSE | TEX1).
pub const SHADOW_FVF: u32 = 0x142;
/// The device ability the draw needs (`QueryDeviceAbility(9)`).
pub const SHADOW_ABILITY: u32 = 9;
/// The texture's name.
pub const SHADOW_TEXTURE: &str = "efxshadow";

const SHADOW_FILE: &str = "SurfaceMaterials\\shadows_inl.h";

/// `kShadowDrawRs`: render states and values, in order.
pub const SHADOW_RENDER_STATES: [(u32, u32); 5] =
    [(0x89, 0), (0x1D, 0), (0x1B, 1), (0x13, 5), (0x14, 6)];
/// `kShadowDrawTss0`: stage 0's states and values.
pub const SHADOW_STAGE0: [(u32, u32); 6] = [(1, 4), (2, 2), (3, 1), (4, 4), (5, 2), (6, 1)];
/// `kShadowDrawTss1`: stage 1's.
pub const SHADOW_STAGE1: [(u32, u32); 6] = [(1, 1), (2, 2), (3, 1), (4, 1), (5, 2), (6, 1)];

const D3DRS_ZWRITEENABLE: u32 = 14;
const D3DRS_CULLMODE: u32 = 22;
const D3DRS_ZFUNC: u32 = 23;
const D3DRS_ZBIAS: u32 = 47;
const D3DTSS_TEXCOORDINDEX: u32 = 11;
/// `D3DPT_TRIANGLELIST`.
const TRIANGLE_LIST: u32 = 4;

/// The states the draw sets and restores afterwards, in the order it sets
/// them (`FShadowRenderStateScope`s).
const SCOPED_STATES: [(u32, u32); 4] = [
    (D3DRS_ZFUNC, 4),
    (D3DRS_CULLMODE, 1),
    (D3DRS_ZBIAS, 4),
    (D3DRS_ZWRITEENABLE, 0),
];

const FALLBACK_EXTENTS: [f32; 6] = [0.28, -0.28, 0.0, 0.05, 0.24, -0.24];
const Z_BIAS: f32 = -0.1;
const FADE_RANGE: f32 = 50.0;
const FADE_QUAD: f32 = 4.0;
const SCALE_BIAS: f64 = 0.8;
const ALPHA_SUB: f32 = 0.8;
const ALPHA_CONST: f32 = 0.6;
const COLOR_SCALE: f32 = 255.0;

/// A corner the writer emits (`FShadowCorner`).
#[derive(Clone, Copy, Debug, Default)]
struct Corner {
    xyz: [f32; 3],
    colour: u32,
    uv: [f32; 2],
}

/// `FShadowVertWriter`: the vertex layout of the lock's format and the
/// quad expander (corners 0, 1, 2, 3, then 0 and 2 again).
struct Writer {
    colour_offset: usize,
    uv_offset: usize,
    stride: usize,
    written: u32,
    colour: u32,
    uv: [f32; 2],
    expander: u32,
    saved: [Corner; 2],
}

impl Writer {
    /// `ShadowWriterReset`: the offsets of the colour and the texture
    /// coordinates and the stride in `fvf`.
    fn new(fvf: u32) -> Writer {
        let position = if fvf & 0xC == 0xC {
            28
        } else if fvf & 4 != 0 {
            16
        } else if fvf & 2 != 0 {
            12
        } else {
            0
        };
        let normal = if fvf & 0x10 != 0 { 12 } else { 0 };
        let colour = (if fvf & 0x40 != 0 { 4 } else { 0 }) + (if fvf & 0x80 != 0 { 4 } else { 0 });
        let mut texture = 0;
        for index in 0..((fvf >> 8) & 15) {
            texture += match (fvf >> ((16 + index * 2) & 31)) & 3 {
                0 => 8,
                1 => 12,
                2 => 16,
                _ => 4,
            };
        }
        Writer {
            colour_offset: position + normal,
            uv_offset: position + normal + colour,
            stride: position + normal + colour + texture,
            written: 0,
            colour: 0xFFFF_FFFF,
            uv: [0.0; 2],
            expander: 0,
            saved: [Corner::default(); 2],
        }
    }

    fn put(bytes: &mut [u8], at: usize, word: u32) {
        if let Some(slot) = bytes.get_mut(at..at + 4) {
            slot.copy_from_slice(&word.to_le_bytes());
        }
    }

    /// `ShadowWriterRaw`.
    fn raw(&mut self, bytes: &mut [u8], corner: &Corner) {
        let base = self.written as usize * self.stride;
        for (index, value) in corner.xyz.iter().enumerate() {
            Writer::put(bytes, base + index * 4, value.to_bits());
        }
        Writer::put(bytes, base + self.colour_offset, corner.colour);
        Writer::put(bytes, base + self.uv_offset, corner.uv[0].to_bits());
        Writer::put(bytes, base + self.uv_offset + 4, corner.uv[1].to_bits());
        self.written += 1;
    }

    /// `ShadowWriterEmit`.
    fn emit(&mut self, bytes: &mut [u8], uv: [f32; 2], xyz: [f32; 3]) {
        self.uv = uv;
        let corner = Corner {
            xyz,
            colour: self.colour,
            uv: self.uv,
        };
        self.raw(bytes, &corner);
        let index = self.expander;
        if index & 1 == 0 {
            self.saved[(index >> 1) as usize] = corner;
        }
        self.expander = index + 1;
        if self.expander > 3 {
            self.expander = 0;
            let saved = self.saved;
            self.raw(bytes, &saved[0]);
            self.raw(bytes, &saved[1]);
        }
    }
}

impl Appearance {
    /// `DrawShadowLike`: the result the reference's two draws discard.
    pub(super) fn draw_shadows<S: AppearanceServices + ?Sized>(
        &mut self,
        services: &mut S,
        camera: &mut dyn Camera,
        shadows: &[ShadowInstance],
        flags: ShadowFlags,
    ) -> i32 {
        // The reference calls through the pipeline, the vertex-buffer
        // manager and the texture library whether or not they were
        // acquired (divergence D323).
        if !(self.faces.pipeline && self.faces.vertex_buffers && self.faces.textures) {
            return GR_GENERIC;
        }
        if services.device_ability(SHADOW_ABILITY).unwrap_or(0) == 0 {
            return GR_GENERIC;
        }
        if shadows.is_empty() {
            return GR_OK;
        }
        let mut handle = 0;
        let record =
            match services.find_texture(SHADOW_TEXTURE, name_crc32(SHADOW_TEXTURE.as_bytes())) {
                Ok(record) => {
                    if let Some(frame) = services.record_frame(&record, 0) {
                        handle = frame.handle;
                    }
                    Some(record)
                }
                Err(_) => {
                    services.dump(
                        diag::DumpFlags::WARNING.bits().cast_signed(),
                        &diag::general_line(
                            SHADOW_FILE,
                            0x24,
                            Severity::Warning,
                            "couldnt find texture 'efxshadow' (from efx.txm?)",
                        ),
                    );
                    None
                }
            };
        if handle != 0 {
            self.shadow_quads(services, camera, shadows, flags, handle);
        }
        if let Some(record) = record {
            services.release_texture(record);
        }
        if handle == 0 { GR_GENERIC } else { GR_OK }
    }

    /// The states, the quads and the draw, with the states restored.
    fn shadow_quads<S: AppearanceServices + ?Sized>(
        &mut self,
        services: &mut S,
        camera: &mut dyn Camera,
        shadows: &[ShadowInstance],
        flags: ShadowFlags,
        handle: u32,
    ) {
        for (state, value) in SHADOW_RENDER_STATES {
            services.set_render_state(state, value);
        }
        let view = camera.view_transform();
        services.set_modelview(&view);
        let mut saved = [0u32; 4];
        for (slot, (state, value)) in saved.iter_mut().zip(SCOPED_STATES) {
            *slot = services.render_state(state);
            services.set_render_state(state, value);
        }
        services.set_stage_state(0, D3DTSS_TEXCOORDINDEX, 0);
        services.set_stage_texture(0, handle);
        for (state, value) in SHADOW_STAGE0 {
            services.set_stage_state(0, state, value);
        }
        for (state, value) in SHADOW_STAGE1 {
            services.set_stage_state(1, state, value);
        }

        let quad_verts = (shadows.len() as u32).wrapping_mul(4);
        let lock_count = quad_verts.wrapping_add((quad_verts / 4).wrapping_mul(2));
        if let Ok(mut lock) = services.lock_vertex_memory(SHADOW_FVF, lock_count) {
            let listing = lock.listing;
            let mut writer = Writer::new(listing.fvf);
            for shadow in shadows {
                self.shadow_quad(services, &mut writer, &mut lock.bytes, shadow, flags);
            }
            services.unlock_vertex_memory(lock);
            if writer.written != 0 {
                services.draw_vb(
                    TRIANGLE_LIST,
                    listing.vertex_buffer,
                    listing.base_vertex,
                    writer.written,
                );
            }
        }

        for ((state, _), value) in SCOPED_STATES.iter().zip(saved).rev() {
            services.set_render_state(*state, value);
        }
    }

    /// One instance's quad, or nothing when it is too high or invisible.
    fn shadow_quad<S: AppearanceServices + ?Sized>(
        &mut self,
        services: &mut S,
        writer: &mut Writer,
        bytes: &mut [u8],
        shadow: &ShadowInstance,
        flags: ShadowFlags,
    ) {
        let t = services.instance_transform(shadow.instance);
        let mut pos = [t[9], t[10], t[11]];
        let archetype = services.instance_archetype(shadow.instance);
        services.release_handle(archetype);
        let mut extents = [0.0f32; 6];
        let mut fallback = true;
        // Without the renderer the reference calls through null; here the
        // bounds fail (divergence D323).
        if flags.bounds && archetype != -1 && self.faces.renderer {
            let (result, written) = services.bounds(archetype, 1.0);
            if let Some(written) = written {
                extents = written;
            }
            fallback = result < 0;
        }
        if fallback {
            extents = FALLBACK_EXTENTS;
            let bias = f64::from(Z_BIAS);
            for (axis, column) in pos.iter_mut().zip([2, 5, 8]) {
                let offset = (bias * f64::from(t[column])) as f32;
                *axis = (f64::from(offset) + f64::from(*axis)) as f32;
            }
        }
        let mut radius = shadow.radius;
        if flags.raise {
            radius = (f64::from(radius) + f64::from(extents[3])) as f32;
        }
        let mut fade = f64::from(pos[1]) - f64::from(radius);
        if fade < 0.0 {
            fade = 0.0;
        }
        if fade > f64::from(FADE_RANGE) {
            return;
        }
        fade /= f64::from(FADE_RANGE);
        if flags.fade_scale {
            self.shadow_scale = ((fade * fade) * f64::from(FADE_QUAD) + SCALE_BIAS) as f32;
        }
        let mut alpha = f64::from(ALPHA_CONST);
        if flags.fade_alpha {
            alpha = f64::from(ALPHA_SUB) - fade;
            if alpha < 0.0 {
                return;
            }
            if alpha > 1.0 {
                alpha = 1.0;
            }
        }
        if !(alpha > 0.0) {
            return;
        }
        let colour_byte = trunc_to_i32((alpha * alpha) * f64::from(COLOR_SCALE));
        writer.colour = (colour_byte.cast_unsigned() << 24) | 0x00FF_FFFF;

        let angle = f64::from(t[2]).atan2(f64::from(t[8]));
        let scale = f64::from(self.shadow_scale);
        let sin_a = angle.sin() * scale;
        let cos_a = angle.cos() * scale;
        let e = extents.map(f64::from);
        let width_x = e[0] - e[1];
        let width_z = (e[4] - e[5]) as f32;
        let centre_x = ((0.5 * width_x + e[1]) + f64::from(pos[0])) as f32;
        let centre_z = ((f64::from(width_z) * 0.5 + e[5]) + f64::from(pos[2])) as f32;
        let hx = f64::from((width_x * cos_a) as f32);
        let hy = f64::from((width_x * sin_a) as f32);
        let zx = f64::from((f64::from(width_z) * cos_a) as f32);
        let zy = f64::from((f64::from(width_z) * sin_a) as f32);
        let (cx, cz) = (f64::from(centre_x), f64::from(centre_z));
        let left = f64::from((cx - hx) as f32);
        writer.emit(
            bytes,
            [0.0, 0.0],
            [(left + zy) as f32, radius, ((zx + hy) + cz) as f32],
        );
        writer.emit(
            bytes,
            [0.0, 1.0],
            [(left - zy) as f32, radius, ((hy + cz) - zx) as f32],
        );
        let back = cz - hy;
        let back_stored = f64::from(back as f32);
        writer.emit(
            bytes,
            [1.0, 1.0],
            [((hx + cx) - zy) as f32, radius, (back - zx) as f32],
        );
        writer.emit(
            bytes,
            [1.0, 0.0],
            [((zy + hx) + cx) as f32, radius, (back_stored + zx) as f32],
        );
    }
}
