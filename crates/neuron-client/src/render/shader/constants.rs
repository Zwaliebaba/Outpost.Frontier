//! The constant buffer of the fixed-function shaders (decision record 0004,
//! point 6): [`pack`] lays a [`DrawConstants`] out as
//! `shaders/fixed_function.hlsl` declares its `cbuffer Draw`.
//!
//! The layout is 16-byte registers; [`reg`] names the first register of
//! each field, and the HLSL places every field with `packoffset` at the
//! same register (a test reads the HLSL and compares). Matrices are stored
//! row-major as the C++ keeps them and the shaders are compiled with
//! row-major packing (`/Zpr`), so `mul(v, m)` is Direct3D's `v * M`.
//!
//! Work Direct3D 8 did once per draw is done here, in `f64` and rounded to
//! `f32` at the store: world-view products, the normal matrices (inverse
//! transpose of the world-view's upper 3x3), lights moved into camera
//! space, and the spot cone cosines. Lighting is computed in camera space,
//! as Direct3D 8 computes it.

use crate::render::ff::draw::{DrawConstants, SHADER_STAGES, StageOps};
use crate::render::ff::fvf::{Element, Fvf};
use crate::render::ff::lights::{LightData, LightType, MAX_DRAW_LIGHTS};
use crate::render::ff::state::{Color, MaterialSource, RawValue, VertexBlend};
use crate::render::ff::transforms::Matrix16;

use super::samplers;

/// The first register of each field of `cbuffer Draw`.
pub mod reg {
    /// `float4x4 worldview[4]`: world matrix `n` times the view.
    pub const WORLDVIEW: u32 = 0;
    /// `float4x4 normal[4]`: the inverse transpose of each world-view's
    /// upper 3x3, in the upper 3x3.
    pub const NORMAL: u32 = 16;
    pub const PROJECTION: u32 = 32;
    /// `float4x4 tex_transform[2]`.
    pub const TEX_TRANSFORM: u32 = 36;
    pub const MATERIAL_DIFFUSE: u32 = 44;
    pub const MATERIAL_AMBIENT: u32 = 45;
    pub const MATERIAL_SPECULAR: u32 = 46;
    pub const MATERIAL_EMISSIVE: u32 = 47;
    pub const GLOBAL_AMBIENT: u32 = 48;
    /// Power, fog start, fog end, fog density.
    pub const SCALARS: u32 = 49;
    pub const FOG_COLOR: u32 = 50;
    pub const TEXTURE_FACTOR: u32 = 51;
    /// Viewport x, y, width, height.
    pub const VIEWPORT: u32 = 52;
    /// Pixel offset x and y in clip space per unit of `w` (`+1/width`,
    /// `-1/height`), alpha
    /// reference (0 to 255), unused.
    pub const OFFSETS: u32 = 53;
    /// `uint4`: vertex flags, blend matrix count, fog bits, alpha function.
    pub const FLAGS: u32 = 54;
    /// `uint4`: diffuse, specular, ambient and emissive material sources.
    pub const SOURCES: u32 = 55;
    /// `uint4`: light count, texture-coordinate sizes, sampler of stage 0
    /// and of stage 1.
    pub const COUNTS: u32 = 56;
    /// `uint4 stage[6]`: three registers per stage.
    pub const STAGES: u32 = 57;
    /// `Light lights[16]`: seven registers per light.
    pub const LIGHTS: u32 = 63;
    /// Registers per light.
    pub const LIGHT_REGISTERS: u32 = 7;
    /// Registers in use.
    pub const END: u32 = LIGHTS + LIGHT_REGISTERS * 16;
}

/// The size of the constant buffer in bytes: the registers in use rounded
/// up to Direct3D 12's 256-byte constant-buffer alignment.
pub const CONSTANT_BYTES: usize = (reg::END as usize * 16).div_ceil(256) * 256;

/// Bits of the vertex-flags word (register [`reg::FLAGS`], `x`).
pub mod vertex_flags {
    pub const PRETRANSFORMED: u32 = 1 << 0;
    pub const HAS_NORMAL: u32 = 1 << 1;
    pub const HAS_DIFFUSE: u32 = 1 << 2;
    pub const HAS_SPECULAR: u32 = 1 << 3;
    pub const LIGHTING: u32 = 1 << 4;
    pub const COLOR_VERTEX: u32 = 1 << 5;
    pub const SPECULAR_ENABLE: u32 = 1 << 6;
    pub const LOCAL_VIEWER: u32 = 1 << 7;
    pub const NORMALIZE_NORMALS: u32 = 1 << 8;
    /// Stage 0 and stage 1 have a texture bound.
    pub const TEXTURE0: u32 = 1 << 9;
    pub const TEXTURE1: u32 = 1 << 10;
}

/// Bits of the fog word (register [`reg::FLAGS`], `z`).
pub mod fog_bits {
    pub const ENABLE: u32 = 1 << 0;
    /// `D3DFOGMODE` of `FOGTABLEMODE` at bits 4 to 7.
    pub const TABLE_SHIFT: u32 = 4;
    /// `D3DFOGMODE` of `FOGVERTEXMODE` at bits 8 to 11.
    pub const VERTEX_SHIFT: u32 = 8;
    pub const RANGE: u32 = 1 << 12;
}

/// The viewport a draw renders into, in pixels (`D3DVIEWPORT8`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ViewportRect {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

/// A buffer of `CONSTANT_BYTES` bytes being filled register by register.
struct Registers(Vec<u8>);

impl Registers {
    fn at(&mut self, register: u32, word: u32) -> &mut [u8] {
        let at = register as usize * 16 + word as usize * 4;
        &mut self.0[at..at + 4]
    }
    fn f(&mut self, register: u32, word: u32, value: f32) {
        self.at(register, word)
            .copy_from_slice(&value.to_le_bytes());
    }
    fn u(&mut self, register: u32, word: u32, value: u32) {
        self.at(register, word)
            .copy_from_slice(&value.to_le_bytes());
    }
    fn f4(&mut self, register: u32, values: [f32; 4]) {
        for (w, v) in values.into_iter().enumerate() {
            self.f(register, w as u32, v);
        }
    }
    fn u4(&mut self, register: u32, values: [u32; 4]) {
        for (w, v) in values.into_iter().enumerate() {
            self.u(register, w as u32, v);
        }
    }
    fn matrix(&mut self, register: u32, m: &Matrix16) {
        for row in 0..4 {
            self.f4(
                register + row as u32,
                [m[row * 4], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]],
            );
        }
    }
}

type M64 = [f64; 16];

fn widen(m: &Matrix16) -> M64 {
    m.map(f64::from)
}

fn narrow(m: &M64) -> Matrix16 {
    m.map(|v| v as f32)
}

/// `a * b` for row-major matrices (row vectors: `v * a * b`).
fn mul(a: &M64, b: &M64) -> M64 {
    let mut out = [0.0; 16];
    for r in 0..4 {
        for c in 0..4 {
            let mut sum = 0.0;
            for k in 0..4 {
                sum += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = sum;
        }
    }
    out
}

/// The inverse transpose of the upper 3x3 of `m`, in the upper 3x3 of a
/// 4x4 whose rest is the identity's. A singular matrix gives its
/// transposed adjugate unscaled, which keeps directions.
fn normal_matrix(m: &M64) -> M64 {
    let a = |r: usize, c: usize| m[r * 4 + c];
    // Cofactors: the inverse transpose is the cofactor matrix over the
    // determinant.
    let cof = [
        [
            a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1),
            a(1, 2) * a(2, 0) - a(1, 0) * a(2, 2),
            a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0),
        ],
        [
            a(0, 2) * a(2, 1) - a(0, 1) * a(2, 2),
            a(0, 0) * a(2, 2) - a(0, 2) * a(2, 0),
            a(0, 1) * a(2, 0) - a(0, 0) * a(2, 1),
        ],
        [
            a(0, 1) * a(1, 2) - a(0, 2) * a(1, 1),
            a(0, 2) * a(1, 0) - a(0, 0) * a(1, 2),
            a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0),
        ],
    ];
    let det = a(0, 0) * cof[0][0] + a(0, 1) * cof[0][1] + a(0, 2) * cof[0][2];
    let scale = if det == 0.0 { 1.0 } else { 1.0 / det };
    let mut out = [0.0; 16];
    for r in 0..3 {
        for c in 0..3 {
            out[r * 4 + c] = cof[r][c] * scale;
        }
    }
    out[15] = 1.0;
    out
}

/// A point (`w = 1`) or a direction (`w = 0`) through `m`.
fn transform(v: [f32; 3], w: f64, m: &M64) -> [f64; 3] {
    let v = [f64::from(v[0]), f64::from(v[1]), f64::from(v[2]), w];
    std::array::from_fn(|c| (0..4).map(|k| v[k] * m[k * 4 + c]).sum())
}

/// A `D3DCOLOR` as four floats, red to alpha.
fn color(c: Color) -> [f32; 4] {
    let n = |v: u8| (f64::from(v) / 255.0) as f32;
    [n(c.r()), n(c.g()), n(c.b()), n(c.a())]
}

/// The number of world matrices a blend mode mixes; the last weight is
/// one minus the others, as in Direct3D 8. Tweening and unknown modes
/// draw with the world matrix alone.
fn blend_matrices(mode: VertexBlend) -> u32 {
    match mode {
        VertexBlend::Weights1 => 2,
        VertexBlend::Weights2 => 3,
        VertexBlend::Weights3 => 4,
        _ => 1,
    }
}

/// Texture-coordinate sizes for the shader: four bits per set, the float
/// count of each set the vertex has, 0 for a set it lacks.
fn texcoord_sizes(fvf: Fvf) -> u32 {
    fvf.layout()
        .iter()
        .filter_map(|(e, _)| match e {
            Element::TexCoord { set, floats } => Some(u32::from(*floats) << (u32::from(*set) * 4)),
            _ => None,
        })
        .fold(0, |a, b| a | b)
}

fn source(s: MaterialSource) -> u32 {
    s.to_raw()
}

fn stage_words(stage: &StageOps, bound: bool) -> [[u32; 4]; 3] {
    let [c0, c1, c2] = stage.color_args.map(RawValue::to_raw);
    let [a0, a1, a2] = stage.alpha_args.map(RawValue::to_raw);
    [
        [stage.color_op.to_raw(), c0, c1, c2],
        [stage.alpha_op.to_raw(), a0, a1, a2],
        [
            stage.result.to_raw(),
            stage.coord.to_raw(),
            stage.transform_flags.to_raw(),
            u32::from(bound),
        ],
    ]
}

/// One light in camera space, as the shader's `Light` struct holds it.
fn pack_light(r: &mut Registers, slot: u32, light: &LightData, view: &M64) {
    let base = reg::LIGHTS + slot * reg::LIGHT_REGISTERS;
    r.f4(base, light.diffuse());
    r.f4(base + 1, light.specular());
    r.f4(base + 2, light.ambient());
    let [px, py, pz] = transform(light.position(), 1.0, view);
    r.f4(base + 3, [px as f32, py as f32, pz as f32, light.range()]);
    let [dx, dy, dz] = transform(light.direction(), 0.0, view);
    r.f4(base + 4, [dx as f32, dy as f32, dz as f32, light.falloff()]);
    let [a0, a1, a2] = light.attenuation();
    let kind = match light.light_type() {
        LightType::Point => 1.0,
        LightType::Spot => 2.0,
        LightType::Directional => 3.0,
        LightType::Other(_) => 0.0,
    };
    r.f4(base + 5, [a0, a1, a2, kind]);
    let half_cos = |angle: f32| (f64::from(angle) * 0.5).cos() as f32;
    r.f4(
        base + 6,
        [half_cos(light.theta()), half_cos(light.phi()), 0.0, 0.0],
    );
}

/// The constant buffer for one draw: `constants` for vertices of `fvf`,
/// with textures bound on the stages `bound` marks, into `viewport`.
#[must_use]
pub fn pack(
    constants: &DrawConstants,
    fvf: Fvf,
    bound: [bool; SHADER_STAGES],
    viewport: ViewportRect,
) -> Vec<u8> {
    let mut r = Registers(vec![0; CONSTANT_BYTES]);
    let view = widen(&constants.view);
    let worlds = [
        constants.world,
        constants.blend_worlds[0],
        constants.blend_worlds[1],
        constants.blend_worlds[2],
    ];
    for (i, world) in worlds.iter().enumerate() {
        let wv = mul(&widen(world), &view);
        r.matrix(reg::WORLDVIEW + 4 * i as u32, &narrow(&wv));
        r.matrix(reg::NORMAL + 4 * i as u32, &narrow(&normal_matrix(&wv)));
    }
    r.matrix(reg::PROJECTION, &constants.projection);
    for (i, stage) in constants.stages.iter().enumerate() {
        if let Some(m) = &stage.transform {
            r.matrix(reg::TEX_TRANSFORM + 4 * i as u32, m);
        }
    }

    let layout = fvf.layout();
    let has = |want: Element| layout.iter().any(|(e, _)| *e == want);
    let mut flags = 0;
    let mut set = |on: bool, bit: u32| {
        if on {
            flags |= bit;
        }
    };
    set(constants.pretransformed, vertex_flags::PRETRANSFORMED);
    set(has(Element::Normal), vertex_flags::HAS_NORMAL);
    set(has(Element::Diffuse), vertex_flags::HAS_DIFFUSE);
    set(has(Element::Specular), vertex_flags::HAS_SPECULAR);
    set(constants.color_vertex, vertex_flags::COLOR_VERTEX);
    set(bound[0], vertex_flags::TEXTURE0);
    set(bound[1], vertex_flags::TEXTURE1);

    let mut lights = 0;
    if let Some(lighting) = &constants.lighting {
        set(true, vertex_flags::LIGHTING);
        set(lighting.local_viewer, vertex_flags::LOCAL_VIEWER);
        set(lighting.normalize_normals, vertex_flags::NORMALIZE_NORMALS);
        let m = &lighting.material;
        r.f4(reg::MATERIAL_DIFFUSE, m.diffuse());
        r.f4(reg::MATERIAL_AMBIENT, m.ambient());
        r.f4(reg::MATERIAL_SPECULAR, m.specular());
        r.f4(reg::MATERIAL_EMISSIVE, m.emissive());
        r.f4(reg::GLOBAL_AMBIENT, color(lighting.ambient));
        r.f(reg::SCALARS, 0, m.power());
        r.u4(
            reg::SOURCES,
            [
                source(lighting.diffuse_source),
                source(lighting.specular_source),
                source(lighting.ambient_source),
                source(lighting.emissive_source),
            ],
        );
        for (_, light) in lighting.lights.iter().take(MAX_DRAW_LIGHTS) {
            pack_light(&mut r, lights, light, &view);
            lights += 1;
        }
    }
    // SPECULARENABLE adds the specular colour lit or not.
    set(constants.specular_enable, vertex_flags::SPECULAR_ENABLE);
    let mut fog = 0;
    if let Some(f) = &constants.fog {
        fog = fog_bits::ENABLE
            | (f.table.to_raw() & 0xf) << fog_bits::TABLE_SHIFT
            | (f.vertex.to_raw() & 0xf) << fog_bits::VERTEX_SHIFT
            | if f.range { fog_bits::RANGE } else { 0 };
        r.f(reg::SCALARS, 1, f.start);
        r.f(reg::SCALARS, 2, f.end);
        r.f(reg::SCALARS, 3, f.density);
        r.f4(reg::FOG_COLOR, color(f.color));
    }
    let (alpha_func, alpha_ref) = match constants.alpha_test {
        Some((func, reference)) => (func.to_raw(), f32::from(reference)),
        None => (0, 0.0),
    };
    r.f4(reg::TEXTURE_FACTOR, color(constants.texture_factor));
    r.f4(
        reg::VIEWPORT,
        [viewport.x, viewport.y, viewport.width, viewport.height],
    );
    // Half a pixel right and down (record 0004, point 7, as corrected):
    // Direct3D 8 samples a pixel at its integer coordinates, Direct3D 12 at
    // its centre. One pixel is 2/width of clip space.
    let offset = |size: f32| {
        if size > 0.0 {
            (1.0 / f64::from(size)) as f32
        } else {
            0.0
        }
    };
    r.f4(
        reg::OFFSETS,
        [
            offset(viewport.width),
            -offset(viewport.height),
            alpha_ref,
            0.0,
        ],
    );
    r.u4(
        reg::FLAGS,
        [
            flags,
            blend_matrices(constants.vertex_blend),
            fog,
            alpha_func,
        ],
    );
    let samplers: [u32; SHADER_STAGES] =
        std::array::from_fn(|i| samplers::index(&constants.stages[i]));
    r.u4(
        reg::COUNTS,
        [lights, texcoord_sizes(fvf), samplers[0], samplers[1]],
    );
    for (i, stage) in constants.stages.iter().enumerate() {
        for (k, words) in stage_words(stage, bound[i]).into_iter().enumerate() {
            r.u4(reg::STAGES + 3 * i as u32 + k as u32, words);
        }
    }
    r.0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::ff::defaults::Defaults;
    use crate::render::ff::draw::DrawConstants;
    use crate::render::ff::lights::Lights;
    use crate::render::ff::model::StateModel;
    use crate::render::ff::state::RenderState;
    use crate::render::ff::transforms::Transforms;

    fn f(bytes: &[u8], register: u32, word: u32) -> f32 {
        let at = register as usize * 16 + word as usize * 4;
        f32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
    }

    fn u(bytes: &[u8], register: u32, word: u32) -> u32 {
        let at = register as usize * 16 + word as usize * 4;
        u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
    }

    const VIEWPORT: ViewportRect = ViewportRect {
        x: 0.0,
        y: 0.0,
        width: 640.0,
        height: 480.0,
    };

    fn light(kind: u32, position: [f32; 3], direction: [f32; 3]) -> LightData {
        let mut w = [0u32; 26];
        w[0] = kind;
        // Diffuse white.
        w[1..5].fill(1.0_f32.to_bits());
        w[13..16].copy_from_slice(&position.map(f32::to_bits));
        w[16..19].copy_from_slice(&direction.map(f32::to_bits));
        w[19] = 100.0_f32.to_bits(); // range
        w[20] = 1.0_f32.to_bits(); // falloff
        w[21] = 1.0_f32.to_bits(); // attenuation 0
        w[24] = 0.5_f32.to_bits(); // theta
        w[25] = 1.0_f32.to_bits(); // phi
        LightData(w)
    }

    #[test]
    fn an_unlit_draw_packs_flags_offsets_and_stages() {
        let m = StateModel::new(Defaults::builtin());
        let t = Transforms::new(true);
        let l = Lights::new();
        // XYZ | DIFFUSE | TEX1 with a 2D set.
        let fvf = Fvf(0x142);
        let c = DrawConstants::new(&m, &t, &l, fvf);
        let b = pack(&c, fvf, [true, false], VIEWPORT);
        assert_eq!(b.len(), CONSTANT_BYTES);
        let flags = u(&b, reg::FLAGS, 0);
        assert_eq!(flags, vertex_flags::HAS_DIFFUSE | vertex_flags::TEXTURE0);
        // One world matrix, no fog, no alpha test.
        assert_eq!(u(&b, reg::FLAGS, 1), 1);
        assert_eq!(u(&b, reg::FLAGS, 2), 0);
        assert_eq!(u(&b, reg::FLAGS, 3), 0);
        // Half a pixel right and down.
        assert_eq!(f(&b, reg::OFFSETS, 0), 1.0 / 640.0);
        assert_eq!(f(&b, reg::OFFSETS, 1), -1.0 / 480.0);
        assert_eq!(f(&b, reg::VIEWPORT, 2), 640.0);
        // Set 0 has two floats; no other set.
        assert_eq!(u(&b, reg::COUNTS, 1), 2);
        // Stage 0's built-in operations: MODULATE of TEXTURE and CURRENT,
        // SELECTARG1 of TEXTURE for alpha; stage 1 disabled.
        assert_eq!(u(&b, reg::STAGES, 0), c.stages[0].color_op.to_raw());
        assert_eq!(u(&b, reg::STAGES + 1, 0), c.stages[0].alpha_op.to_raw());
        assert_eq!(u(&b, reg::STAGES + 2, 3), 1);
        assert_eq!(u(&b, reg::STAGES + 5, 3), 0);
        assert_eq!(u(&b, reg::STAGES + 3, 0), c.stages[1].color_op.to_raw());
        // World 0 times the view, rounded once.
        let wv = narrow(&mul(&widen(&c.world), &widen(&c.view)));
        for (i, want) in wv.iter().enumerate() {
            let got = f(&b, reg::WORLDVIEW + (i / 4) as u32, (i % 4) as u32);
            assert_eq!(got, *want);
        }
    }

    #[test]
    fn lights_move_into_camera_space() {
        let mut m = StateModel::new(Defaults::builtin());
        m.set_render_state(RenderState::Lighting(true));
        let mut t = Transforms::new(false);
        // A view that moves the world 10 along z.
        t.set_view(&[1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 10.0]);
        let mut l = Lights::new();
        l.add_light(1, light(1, [1.0, 2.0, 3.0], [0.0, 0.0, 1.0]));
        l.set_light_enable(1, true);
        l.flush().unwrap();
        let fvf = Fvf(0x112);
        let c = DrawConstants::new(&m, &t, &l, fvf);
        let b = pack(&c, fvf, [false; 2], VIEWPORT);
        assert_eq!(u(&b, reg::COUNTS, 0), 1);
        let v = widen(&c.view);
        let want = transform([1.0, 2.0, 3.0], 1.0, &v);
        for (i, w) in want.into_iter().enumerate() {
            assert_eq!(f(&b, reg::LIGHTS + 3, i as u32), w as f32);
        }
        assert_eq!(f(&b, reg::LIGHTS + 3, 3), 100.0);
        assert_eq!(f(&b, reg::LIGHTS + 5, 3), 1.0);
        assert_eq!(f(&b, reg::LIGHTS + 6, 0), (0.25_f64.cos()) as f32);
        assert_eq!(f(&b, reg::LIGHTS + 6, 1), (0.5_f64.cos()) as f32);
        assert_ne!(u(&b, reg::FLAGS, 0) & vertex_flags::LIGHTING, 0);
    }

    #[test]
    fn normal_matrices_undo_scale() {
        // A world that scales x by 2: normals scale x by 1/2.
        let mut m: M64 = [0.0; 16];
        m[0] = 2.0;
        m[5] = 1.0;
        m[10] = 1.0;
        m[15] = 1.0;
        let n = normal_matrix(&m);
        assert_eq!(n[0], 0.5);
        assert_eq!(n[5], 1.0);
        assert_eq!(n[10], 1.0);
        assert_eq!(n[15], 1.0);
        // A singular matrix keeps its adjugate.
        let z = normal_matrix(&[0.0; 16]);
        assert!(z.iter().all(|v| v.is_finite()));
    }
}
