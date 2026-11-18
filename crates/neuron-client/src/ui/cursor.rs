//! The software cursor's named cursors (`input_manager.cpp`: `LoadCursors`,
//! `MouseSetCursor`, `CursorAnimate`, `CursorComputeQuad`, `DrawCursor`).
//!
//! `mouse.ini` holds the cursor shapes (a [`super::shape::ShapeDb`]) and up
//! to 20 `[Cursor]` records: a nickname, an animation (a `printf` format
//! taking the frame, a frame count and a rate) or a fixed shape, the
//! hotspot, a colour, a blend mode, a scale, an angle and a spin. The
//! cursor in use is drawn at the end of the frame as one quad, rotated
//! about the hotspot and scaled with the surface width (with widescreen
//! support, the width of the 4:3 equivalent), in a 2D projection with depth
//! test, culling and alpha test off.
//!
//! The reference draws through the Win3D immediate-mode layer in an
//! orthographic projection; here the quad goes to the device as four
//! pre-transformed vertices at the same pixels (divergence D346).

use neuron_core::ini::IniReader;
use neuron_core::msvcrt::{PrintfArg, legacy_format};
use neuron_core::num::trunc_to_i32;

use super::quad_batch::QuadDevice;
use super::shape::{ShapeDb, ShapeTextures, TexShape, key};
use super::widescreen::{Surface, equivalent_surface_width};
use crate::text::font::quad::TlVertex;

/// The most cursors kept (`kMaxCursors`).
pub const MAX_CURSORS: usize = 0x14;
/// `kCursorQuadScale`: a cursor's pixels per surface pixel of width.
pub const QUAD_SCALE: f32 = 0.001_25;

/// `FMouseCursor`.
#[derive(Clone, Debug, PartialEq)]
pub struct Cursor {
    /// The nickname, at most 31 bytes.
    pub name: Vec<u8>,
    /// The shape's name, at most 23 bytes.
    pub shape_name: Vec<u8>,
    /// Whether the shape is looked up (the reference caches its pointer).
    resolved: bool,
    pub hotspot_x: f32,
    pub hotspot_y: f32,
    /// R, G, B, A.
    pub color: [u8; 4],
    pub blend: i32,
    pub scale: f32,
    pub angle: f32,
    pub spin: f32,
    pub frame_count: u32,
    pub frame: u32,
    pub fps: f32,
    /// The animation's shape-name format, at most 31 bytes.
    pub anim_name: Vec<u8>,
}

impl Cursor {
    /// `CursorConstruct`.
    #[must_use]
    pub fn new(name: &[u8]) -> Cursor {
        Cursor {
            name: name[..name.len().min(31)].to_vec(),
            shape_name: Vec::new(),
            resolved: false,
            hotspot_x: 0.0,
            hotspot_y: 0.0,
            color: [0xFF; 4],
            blend: 1,
            scale: 1.0,
            angle: 0.0,
            spin: 0.0,
            frame_count: 0,
            frame: 0,
            fps: 0.0,
            anim_name: Vec::new(),
        }
    }
}

fn clamp_byte(v: i32) -> u8 {
    v.clamp(0, 0xFF) as u8
}

/// The cursors of `mouse.ini`, the one in use and the "DEFAULT" alias.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Cursors {
    pub cursors: Vec<Cursor>,
    current: Option<usize>,
    default_name: Vec<u8>,
    /// `G_bCursorsReady`.
    pub ready: bool,
    /// The shapes of `mouse.ini`.
    pub shapes: ShapeDb,
}

impl Cursors {
    #[must_use]
    pub fn new() -> Cursors {
        Cursors::default()
    }

    /// `MouseInitCursors` over `mouse.ini`'s text: the shapes, the cursors,
    /// ready, and "DEFAULT" selected.
    pub fn init(&mut self, text: &[u8], lib: &mut dyn ShapeTextures) {
        self.shapes.load(text, lib);
        self.load_cursors(text);
        self.ready = true;
        self.set_cursor(Some(b"DEFAULT"), false);
    }

    /// `LoadCursors`: each `[Cursor]` with a `nickname` (cursors past 20 are
    /// read and dropped).
    pub fn load_cursors(&mut self, text: &[u8]) {
        let mut ini = IniReader::new();
        if !ini.open_memory(text) || !ini.read_header() {
            return;
        }
        loop {
            if ini.is_header(b"Cursor") && ini.read_value() {
                let mut has_nickname = false;
                loop {
                    if ini.is_value(b"nickname") {
                        has_nickname = true;
                        break;
                    }
                    if !ini.read_value() {
                        break;
                    }
                }
                if has_nickname {
                    let mut cursor = Cursor::new(&ini.value_string_at(0));
                    while ini.read_value() {
                        Self::read_key(&mut ini, &mut cursor);
                    }
                    if self.cursors.len() < MAX_CURSORS {
                        self.cursors.push(cursor);
                    }
                }
            }
            if !ini.read_header() {
                break;
            }
        }
    }

    fn read_key(ini: &mut IniReader, cursor: &mut Cursor) {
        if ini.is_value(b"shape") {
            cursor.shape_name = key(&ini.value_string_at(0));
        } else if ini.is_value(b"hotspot") {
            let x = ini.value_float(0) as f32;
            cursor.hotspot_y = ini.value_float(1) as f32;
            cursor.hotspot_x = x;
        } else if ini.is_value(b"blend") {
            cursor.blend = trunc_to_i32(ini.value_float(0));
        } else if ini.is_value(b"color") {
            let red = trunc_to_i32(ini.value_float(0));
            let green = trunc_to_i32(ini.value_float(1));
            let blue = trunc_to_i32(ini.value_float(2));
            let mut alpha = trunc_to_i32(ini.value_float(3));
            if alpha == 0 {
                alpha = 0xFF;
            }
            cursor.color = [
                clamp_byte(red),
                clamp_byte(green),
                clamp_byte(blue),
                clamp_byte(alpha),
            ];
        } else if ini.is_value(b"anim") {
            let format = ini.value_string_at(0);
            let format = &format[..format.len().min(0x7F)];
            let frames = ini.value_float(1);
            let fps = ini.value_float(2) as f32;
            cursor.anim_name = format[..format.len().min(31)].to_vec();
            cursor.frame_count = trunc_to_i32(frames).cast_unsigned();
            cursor.fps = fps;
        } else if ini.is_value(b"angle") {
            cursor.angle = ini.value_float(0) as f32;
        } else if ini.is_value(b"spin") {
            cursor.spin = ini.value_float(0) as f32;
        } else if ini.is_value(b"scale") {
            cursor.scale = ini.value_float(0) as f32;
        }
    }

    /// The cursor in use.
    #[must_use]
    pub fn current(&self) -> Option<&Cursor> {
        self.current.and_then(|i| self.cursors.get(i))
    }

    /// `MouseSetCursor`: the cursor called `name` ("DEFAULT" is the name the
    /// last `set_default` call stored); the one in use is left alone; a
    /// change restarts its animation.
    pub fn set_cursor(&mut self, name: Option<&[u8]>, set_default: bool) {
        if set_default {
            self.default_name = name
                .map(|n| n[..n.len().min(31)].to_vec())
                .unwrap_or_default();
        }
        if !self.ready {
            return;
        }
        let eq = neuron_core::hash::eq_ignore_case;
        if let (Some(current), Some(name)) = (self.current(), name)
            && eq(&current.name, name)
        {
            return;
        }
        let found = name.and_then(|name| {
            let wanted: &[u8] = if eq(name, b"DEFAULT") {
                &self.default_name
            } else {
                name
            };
            self.cursors.iter().position(|c| eq(&c.name, wanted))
        });
        if found == self.current {
            return;
        }
        self.current = found;
        if let Some(c) = found.and_then(|i| self.cursors.get_mut(i))
            && c.frame_count != 0
        {
            c.frame = 0;
        }
    }

    /// `MouseIsArrowCursor`.
    #[must_use]
    pub fn is_arrow(&self) -> bool {
        self.current()
            .is_some_and(|c| neuron_core::hash::eq_ignore_case(&c.name, b"Arrow"))
    }

    /// `CursorAnimate` on the cursor in use: an animated cursor's frame from
    /// the time and its shape name formatted; the shape looked up; a
    /// spinning cursor's angle from the time. Returns the shape.
    pub fn animate(&mut self, time: f32) -> Option<TexShape> {
        let index = self.current?;
        let cursor = &mut self.cursors[index];
        if !cursor.anim_name.is_empty() && (cursor.frame_count > 1 || cursor.shape_name.is_empty())
        {
            if cursor.fps != 0.0 {
                let tick = trunc_to_i32(f64::from(time) * f64::from(cursor.fps)).cast_unsigned();
                cursor.frame = tick % cursor.frame_count;
            }
            // `_vsnprintf` into 0x18 bytes, the last one zeroed.
            let name = legacy_format(
                &cursor.anim_name,
                &[PrintfArg::Int(cursor.frame.cast_signed())],
            )
            .unwrap_or_default();
            cursor.shape_name = key(&name[..name.len().min(0x17)]);
            cursor.resolved = false;
        }
        cursor.resolved = true;
        if cursor.spin != 0.0 {
            cursor.angle = (f64::from(time) * f64::from(cursor.spin)) as f32;
        }
        self.shapes.find(&cursor.shape_name).copied()
    }
}

/// `CursorComputeQuad`: the four corners (x, y pairs) of the cursor quad
/// at the cursor position, rotated by the angle about the hotspot and
/// scaled by the cursor's scale and the surface width.
#[must_use]
pub fn compute_quad(
    cursor: &Cursor,
    shape: &TexShape,
    x: i32,
    y: i32,
    surface: &Surface,
) -> [f32; 8] {
    let surface_scale = (equivalent_surface_width(surface) * f64::from(QUAD_SCALE)) as f32;
    let width = f64::from(shape.pixel_w);
    let height = f64::from(shape.pixel_h);
    let angle = f64::from(cursor.angle);
    let sine = angle.sin() * f64::from(cursor.scale) * f64::from(surface_scale);
    let cosine = (angle.cos() * f64::from(cursor.scale) * f64::from(surface_scale)) as f32;
    let cos = f64::from(cosine);
    let (hx, hy) = (f64::from(cursor.hotspot_x), f64::from(cursor.hotspot_y));
    let ax = (f64::from(x) - cos * hx + sine * hy) as f32;
    let ay = (f64::from(y) - sine * hx - cos * hy) as f32;
    let right_x = f64::from(ax) + cos * width;
    let sine_width = (sine * width) as f32;
    let sine_height = (sine * height) as f32;
    let cosine_height = cos * height;
    [
        ax,
        ay,
        right_x as f32,
        (sine * width + f64::from(ay)) as f32,
        (right_x - f64::from(sine_height)) as f32,
        (f64::from(sine_width) + cosine_height + f64::from(ay)) as f32,
        (f64::from(ax) - f64::from(sine_height)) as f32,
        (cosine_height + f64::from(ay)) as f32,
    ]
}

/// The blend switch of `DrawCursor`: source and destination factors.
#[must_use]
pub fn blend_factors(blend: i32) -> (u32, u32) {
    match blend {
        1 => (2, 2),
        2 => (2, 4),
        3 => (5, 2),
        4 => (5, 6),
        5 => (5, 4),
        _ => (2, 1),
    }
}

/// `D3DRS_ZENABLE`.
const RS_ZENABLE: u32 = 7;
/// `D3DRS_ALPHATESTENABLE`.
const RS_ALPHATESTENABLE: u32 = 0x0F;

/// `DrawCursor`'s draw: the states saved and set, the texture with point
/// filtering, the quad, the states restored in reverse order. `texture` is
/// the shape's GPU handle.
pub fn draw(
    device: &mut dyn QuadDevice,
    cursor: &Cursor,
    shape: &TexShape,
    texture: u32,
    position: (i32, i32),
    surface: &Surface,
) {
    let (src, dst) = blend_factors(cursor.blend);
    let states = [
        (RS_ZENABLE, 0),
        (super::quad_batch::RS_CULLMODE, 1),
        (RS_ALPHATESTENABLE, 0),
    ];
    let mut saved = Vec::with_capacity(6);
    for (state, value) in states {
        saved.push((state, device.render_state(state)));
        device.set_render_state(state, value);
    }
    device.set_stage_texture(texture);
    device.set_stage_state(0, 0x0D, 3);
    device.set_stage_state(0, 0x0E, 3);
    device.set_stage_state(0, 0x0D, 1);
    device.set_stage_state(0, 0x0E, 1);
    for (state, value) in [
        (super::quad_batch::RS_SRCBLEND, src),
        (super::quad_batch::RS_DESTBLEND, dst),
        (
            super::quad_batch::RS_ALPHABLENDENABLE,
            u32::from(cursor.blend != 0),
        ),
    ] {
        saved.push((state, device.render_state(state)));
        device.set_render_state(state, value);
    }
    let q = compute_quad(cursor, shape, position.0, position.1, surface);
    let [r, g, b, a] = cursor.color;
    let color = u32::from_be_bytes([a, r, g, b]);
    let v = |x: f32, y: f32, u: f32, v: f32| TlVertex {
        x,
        y,
        z: 0.0,
        rhw: 1.0,
        color,
        specular: 0,
        u,
        v,
    };
    let vertices = [
        v(q[0], q[1], shape.u0, shape.v0),
        v(q[2], q[3], shape.u1, shape.v0),
        v(q[4], q[5], shape.u1, shape.v1),
        v(q[6], q[7], shape.u0, shape.v1),
    ];
    device.draw_indexed_quads(&vertices, &[0, 1, 2, 0, 2, 3]);
    for (state, value) in saved.into_iter().rev() {
        device.set_render_state(state, value);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ui::quad_batch::{DeviceCall, RecordingQuadDevice};
    use crate::ui::shape::FixedTextures;

    const INI: &[u8] = b"[Texture]\nfile = screens\\cursor.TXM\nname = newcursors\n\n\
[Shape]\nname = arrow\nx = 0\ny = 64\nw = 64\nh = 64\n\n\
[Shape]\nname = spin1\nx = 64\ny = 64\nw = 64\nh = 64\n\n\
[Cursor]\nnickname = arrow\nanim = arrow, 1, 0\nhotspot = 20, 18\nblend = 4\ncolor = 255, 255, 255, 0\n\n\
[Cursor]\nnickname = spinner\nanim = spin%d, 2, 4\nhotspot = 32, 32\nspin = 10\nscale = 0.5\n\n\
[Cursor]\nanim = nameless, 1, 0\n";

    fn cursors() -> Cursors {
        let mut lib = FixedTextures {
            size: (256, 256),
            ..FixedTextures::default()
        };
        let mut c = Cursors::new();
        c.init(INI, &mut lib);
        c
    }

    #[test]
    fn cursors_load_and_select() {
        let mut c = cursors();
        assert_eq!(c.cursors.len(), 2);
        assert_eq!(c.cursors[0].color, [255, 255, 255, 255]);
        assert_eq!(c.cursors[1].frame_count, 2);
        // No default stored yet: "DEFAULT" finds nothing.
        assert!(c.current().is_none());
        c.set_cursor(Some(b"ARROW"), true);
        assert!(c.is_arrow());
        c.set_cursor(Some(b"spinner"), false);
        c.set_cursor(Some(b"DEFAULT"), false);
        assert!(c.is_arrow());
        let shape = c.animate(0.0).unwrap();
        assert_eq!(shape.pixel_w, 64);
        c.set_cursor(Some(b"spinner"), false);
        // 1.3 s at 4 frames a second: tick 5, frame 1, "spin1".
        let shape = c.animate(1.3).unwrap();
        assert_eq!(c.current().unwrap().shape_name, b"spin1");
        assert_eq!(c.current().unwrap().angle, 13.0);
        assert_eq!(shape.u0, ((64.0 + f64::from(0.01f32)) / 256.0) as f32);
    }

    #[test]
    fn the_quad_scales_with_the_4_by_3_width() {
        let c = cursors();
        let shape = *c.shapes.find(b"arrow").unwrap();
        let q = compute_quad(&c.cursors[0], &shape, 100, 50, &Surface::new(800, 600));
        // Scale 1 at 800 wide: the hotspot moves to the cursor.
        assert_eq!(q, [80.0, 32.0, 144.0, 32.0, 144.0, 96.0, 80.0, 96.0]);
        let wide = compute_quad(&c.cursors[0], &shape, 100, 50, &Surface::new(1066, 600));
        assert_eq!(wide, q);
        let mut d = RecordingQuadDevice::default();
        draw(
            &mut d,
            &c.cursors[0],
            &shape,
            7,
            (100, 50),
            &Surface::new(800, 600),
        );
        assert!(d.calls.contains(&DeviceCall::RenderState(0x13, 5)));
        assert_eq!(d.draws()[0].0[2].u, shape.u1);
        assert_eq!(d.draws()[0].0[0].color, 0xFFFF_FFFF);
        assert_eq!(d.calls.last(), Some(&DeviceCall::RenderState(7, 0)));
    }
}
