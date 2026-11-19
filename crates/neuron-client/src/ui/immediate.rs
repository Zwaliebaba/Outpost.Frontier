//! The calls controls make to the Win3D immediate-mode layer
//! (`win3d_immediate.h`, `Win3DBindModelview` of `star_sphere.cpp`, and the
//! render states some draws set on the pipeline directly).
//!
//! The reference's controls draw their frames, arrows, rules and thumbs
//! through an OpenGL-like layer: `Win3DBegin` with a primitive, colours,
//! texture coordinates and vertices, `Win3DEnd`; capabilities switched with
//! `Win3DEnable` and `Win3DDisable`; a 2D projection pushed around screen
//! space draws. The layer turns them into batched vertex lists for the
//! pipeline (`win3d_immediate.cpp`, `win3d_renderables.cpp`). Here the
//! controls make the same calls on an [`Immediate`] the draw host hands
//! them ([`super::tree::DrawHost::immediate`]); the layer that turns them
//! into draws is not ported yet. [`ImmediateLog`] records the calls.

use neuron_core::math::Transform;

/// `EWin3DPrimitive`.
pub mod primitive {
    pub const POINTS: u32 = 1;
    pub const LINES: u32 = 2;
    pub const LINE_STRIP: u32 = 3;
    pub const TRIANGLES: u32 = 4;
    pub const TRIANGLE_STRIP: u32 = 5;
    pub const TRIANGLE_FAN: u32 = 6;
    pub const LINE_LOOP: u32 = 7;
    pub const QUADS: u32 = 8;
    pub const POLYGON: u32 = 9;
}

/// `EWin3DCapability`.
pub mod capability {
    pub const CULL_FACE: u32 = 0xB44;
    pub const LIGHTING: u32 = 0xB50;
    pub const FOG: u32 = 0xB60;
    pub const DEPTH_TEST: u32 = 0xB71;
    pub const BLEND: u32 = 0xBE2;
    pub const TEXTURE_2D: u32 = 0xDE1;
}

/// The Direct3D 8 render states the control draws set.
pub mod render_state {
    pub const ZENABLE: u32 = 7;
    pub const ZWRITEENABLE: u32 = 14;
    pub const SRCBLEND: u32 = 19;
    pub const DESTBLEND: u32 = 20;
    pub const CULLMODE: u32 = 22;
    pub const ZFUNC: u32 = 23;
    pub const ALPHABLENDENABLE: u32 = 27;
    pub const LIGHTING: u32 = 137;
}

/// The immediate-mode calls, one method per function of the reference.
pub trait Immediate {
    /// `Win3DBegin`.
    fn begin(&mut self, primitive: u32);
    /// `Win3DEnd`.
    fn end(&mut self);
    /// `Win3DVertex3f`.
    fn vertex(&mut self, x: f32, y: f32, z: f32);
    /// `Win3DColor3ub` (the alpha byte 0xFF).
    fn color3ub(&mut self, r: u8, g: u8, b: u8);
    /// `Win3DColor4ubv`: the four bytes of a little-endian `0xAABBGGRR`
    /// word, R first.
    fn color4ubv(&mut self, rgba: [u8; 4]);
    /// `Win3DTexCoord2f`.
    fn tex_coord(&mut self, u: f32, v: f32);
    /// `Win3DEnable`.
    fn enable(&mut self, capability: u32);
    /// `Win3DDisable`.
    fn disable(&mut self, capability: u32);
    /// `Win3DBindTexture` with a GPU handle (0: none).
    fn bind_texture(&mut self, handle: u32);
    /// `Win3DPushProjection`.
    fn push_projection(&mut self);
    /// `Win3DPopProjection`.
    fn pop_projection(&mut self);
    /// `Win3DSetOrtho2D`: the surface in pixels.
    fn set_ortho_2d(&mut self);
    /// `Win3DBindModelview(nullptr, xform)`: the model-view from an
    /// object's orientation and position.
    fn bind_modelview(&mut self, transform: &Transform);
    /// The pipeline's `SetRenderState`.
    fn set_render_state(&mut self, state: u32, value: u32);
}

/// One recorded call.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Call {
    Begin(u32),
    End,
    Vertex(f32, f32, f32),
    Color3ub(u8, u8, u8),
    Color4ubv([u8; 4]),
    TexCoord(f32, f32),
    Enable(u32),
    Disable(u32),
    BindTexture(u32),
    PushProjection,
    PopProjection,
    SetOrtho2D,
    BindModelview(Transform),
    SetRenderState(u32, u32),
}

/// An [`Immediate`] that records every call.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ImmediateLog {
    pub calls: Vec<Call>,
}

impl Immediate for ImmediateLog {
    fn begin(&mut self, primitive: u32) {
        self.calls.push(Call::Begin(primitive));
    }
    fn end(&mut self) {
        self.calls.push(Call::End);
    }
    fn vertex(&mut self, x: f32, y: f32, z: f32) {
        self.calls.push(Call::Vertex(x, y, z));
    }
    fn color3ub(&mut self, r: u8, g: u8, b: u8) {
        self.calls.push(Call::Color3ub(r, g, b));
    }
    fn color4ubv(&mut self, rgba: [u8; 4]) {
        self.calls.push(Call::Color4ubv(rgba));
    }
    fn tex_coord(&mut self, u: f32, v: f32) {
        self.calls.push(Call::TexCoord(u, v));
    }
    fn enable(&mut self, capability: u32) {
        self.calls.push(Call::Enable(capability));
    }
    fn disable(&mut self, capability: u32) {
        self.calls.push(Call::Disable(capability));
    }
    fn bind_texture(&mut self, handle: u32) {
        self.calls.push(Call::BindTexture(handle));
    }
    fn push_projection(&mut self) {
        self.calls.push(Call::PushProjection);
    }
    fn pop_projection(&mut self) {
        self.calls.push(Call::PopProjection);
    }
    fn set_ortho_2d(&mut self) {
        self.calls.push(Call::SetOrtho2D);
    }
    fn bind_modelview(&mut self, transform: &Transform) {
        self.calls.push(Call::BindModelview(*transform));
    }
    fn set_render_state(&mut self, state: u32, value: u32) {
        self.calls.push(Call::SetRenderState(state, value));
    }
}
