//! Texture shapes (`texture_shapes.cpp`, the reference's `ShapeDB`): named
//! sub-rectangles of textures with the UV rect and a centred scale and
//! offset, loaded from shape INI files (`mouse.ini`, the HUD's, the
//! effects', `ButtonTextures.ini`, `UITextures.ini`, ...).
//!
//! A `[Texture]` section loads a texture archive (`file`) and picks a
//! texture by name; `[Shape]` sections (or `shape_name`, `tex_shape` and
//! `dim` in the `[Texture]` section) cut shapes out of it. Keys compare
//! without case and are cut to 23 bytes. The texture library calls are
//! behind [`ShapeTextures`].

use std::collections::BTreeMap;

use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32;

/// The texture library calls a shape database makes. Records and GPU
/// handles are the library's numbers.
pub trait ShapeTextures {
    /// `FindTextureByCrc` by name, its reference released at once: the
    /// record, if there is one.
    fn find(&mut self, name: &[u8]) -> Option<u32>;
    /// Takes a reference on a record (`CreatePlaybackGroup(record, 0)`).
    fn add_ref(&mut self, record: u32);
    /// `ReleaseTextureRecord`.
    fn release(&mut self, record: u32);
    /// Opens the archive `file` and loads it as a texture group
    /// (`LoadTextureGroup`); `None` when the file is missing.
    fn load_group(&mut self, file: &[u8]) -> Option<i32>;
    /// `GarbageCollect` of a group this load made.
    fn garbage_collect(&mut self, group: i32);
    /// Generic slot 4 (`GetGenericSlot`), saved and cleared for a load.
    fn generic_slot_4(&mut self) -> u32;
    fn set_generic_slot_4(&mut self, value: u32);
    /// `GetTextureFrame(record, frame)`: the GPU handle (word 0).
    fn frame_handle(&mut self, record: u32, frame: i32) -> u32;
    /// `GetTextureDim`: width and height of a GPU texture (0 when
    /// unknown).
    fn texture_dim(&mut self, handle: u32) -> (u32, u32);
}

/// `FTexShape`: the record and the rect.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TexShape {
    pub texture: Option<u32>,
    pub u0: f32,
    pub v0: f32,
    pub u1: f32,
    pub v1: f32,
    pub pixel_w: i32,
    pub pixel_h: i32,
    pub scale_u: f32,
    pub scale_v: f32,
    pub offset_u: f32,
    pub offset_v: f32,
}

fn r(v: f64) -> f32 {
    v as f32
}

impl TexShape {
    /// `TexShapeSetRect`: the rect in pixels (a rect within 0..1 is a
    /// fraction of the texture, a zero extent the whole texture), the UV
    /// rect inset by a hundredth of a pixel, the pixel size, and the
    /// centred scale and offset from the corners (negative corners from the
    /// rect).
    #[allow(clippy::too_many_arguments)]
    pub fn set_rect(
        &mut self,
        lib: &mut dyn ShapeTextures,
        mut x: f32,
        mut y: f32,
        mut w: f32,
        mut h: f32,
        mut ul_u: f32,
        mut ul_v: f32,
        mut lr_u: f32,
        mut lr_v: f32,
    ) {
        let handle = self.texture.map_or(0, |t| lib.frame_handle(t, 0));
        let (tw, th) = lib.texture_dim(handle);
        let tex_w = tw as f32;
        let tex_h = th as f32;
        let (tw, th) = (f64::from(tex_w), f64::from(tex_h));
        if x <= 1.0 && y <= 1.0 {
            x = r(tw * f64::from(x));
            y = r(th * f64::from(y));
        }
        if w == 0.0 || h == 0.0 {
            w = tex_w;
            h = tex_h;
        } else if w <= 1.0 && h <= 1.0 {
            w = r(tw * f64::from(w));
            h = r(th * f64::from(h));
        }
        let inset = f64::from(0.01f32);
        let right = r(f64::from(x) + f64::from(w));
        let bottom = r(f64::from(y) + f64::from(h));
        self.u0 = r((f64::from(x) + inset) / tw);
        self.v0 = r((f64::from(y) + inset) / th);
        self.u1 = r((f64::from(right) - inset) / tw);
        self.v1 = r((f64::from(bottom) - inset) / th);
        self.pixel_w = trunc_to_i32(f64::from(w));
        self.pixel_h = trunc_to_i32(f64::from(h));
        if ul_u < 0.0 {
            ul_u = x;
        }
        if ul_v < 0.0 {
            ul_v = y;
        }
        if lr_u < 0.0 {
            lr_u = right;
        }
        if lr_v < 0.0 {
            lr_v = bottom;
        }
        let span_u = r(f64::from(lr_u) - f64::from(ul_u));
        let span_v = r(f64::from(lr_v) - f64::from(ul_v));
        // `(int)span % 2 == 1` through the reference's signed bit test.
        let half_pixel_u = if trunc_to_i32(f64::from(span_u)) % 2 == 1 {
            0.5
        } else {
            0.0
        };
        let div_w = if w == 0.0 { 1.0 } else { w };
        let div_h = if h == 0.0 { 1.0 } else { h };
        let half_w = r(f64::from(div_w) * 0.5);
        let half_h = r(f64::from(div_h) * 0.5);
        let inv_half_w = r(1.0 / f64::from(half_w));
        let inv_half_h = r(1.0 / f64::from(half_h));
        let center_x = r(f64::from(x) + f64::from(half_w));
        let center_y = r(f64::from(y) + f64::from(half_h));
        let mid_u = r(f64::from(ul_u) + f64::from(span_u) * 0.5 + half_pixel_u);
        let mid_v = r(f64::from(ul_v) + f64::from(span_v) * 0.5);
        self.scale_u = r(f64::from(inv_half_w) * f64::from(span_u) * 0.5);
        self.scale_v = r(f64::from(inv_half_h) * f64::from(span_v) * 0.5);
        self.offset_u = r((f64::from(mid_u) - f64::from(center_x)) * f64::from(inv_half_w));
        self.offset_v = r((f64::from(mid_v) - f64::from(center_y)) * f64::from(inv_half_h));
    }

    /// `TexShapeGetGpuHandle`: the texture's frame handle, 0 without one.
    pub fn gpu_handle(&self, lib: &mut dyn ShapeTextures, frame: i32) -> u32 {
        self.texture.map_or(0, |t| lib.frame_handle(t, frame))
    }
}

/// A `TString<24>` key: at most 23 bytes.
#[must_use]
pub fn key(name: &[u8]) -> Vec<u8> {
    let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
    name[..end.min(23)].to_vec()
}

/// The `_stricmp` order: lower-case ASCII.
fn fold(name: &[u8]) -> Vec<u8> {
    key(name).iter().map(u8::to_ascii_lowercase).collect()
}

/// `FShapeDB`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ShapeDb {
    shapes: BTreeMap<Vec<u8>, (Vec<u8>, TexShape)>,
}

impl ShapeDb {
    #[must_use]
    pub fn new() -> ShapeDb {
        ShapeDb::default()
    }

    /// `Count`.
    #[must_use]
    pub fn len(&self) -> usize {
        self.shapes.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.shapes.is_empty()
    }

    /// `Find`: the shape under a name (case-insensitive, cut to 23 bytes).
    #[must_use]
    pub fn find(&self, name: &[u8]) -> Option<&TexShape> {
        self.shapes.get(&fold(name)).map(|(_, s)| s)
    }

    /// The shapes with their names as first inserted, in key order.
    pub fn iter(&self) -> impl Iterator<Item = (&[u8], &TexShape)> {
        self.shapes.values().map(|(n, s)| (n.as_slice(), s))
    }

    /// `FindOrCreate`.
    fn find_or_create(&mut self, name: &[u8]) -> &mut TexShape {
        &mut self
            .shapes
            .entry(fold(name))
            .or_insert_with(|| (key(name), TexShape::default()))
            .1
    }

    /// `Clear`: every record released.
    pub fn clear(&mut self, lib: &mut dyn ShapeTextures) {
        for (_, shape) in self.shapes.values_mut() {
            if let Some(t) = shape.texture.take() {
                lib.release(t);
            }
        }
        self.shapes.clear();
    }

    /// `ShapeDB::Load` over an INI's text: the shapes added; returns how
    /// many the database holds.
    pub fn load(&mut self, text: &[u8], lib: &mut dyn ShapeTextures) -> usize {
        let saved_slot = lib.generic_slot_4();
        lib.set_generic_slot_4(0);
        let mut groups = Vec::new();
        let mut ini = IniReader::new();
        if ini.open_memory(text) {
            self.load_sections(&mut ini, lib, &mut groups);
        }
        for group in groups {
            lib.garbage_collect(group);
        }
        lib.set_generic_slot_4(saved_slot);
        self.len()
    }

    fn set_new(&mut self, name: &[u8], texture: u32, lib: &mut dyn ShapeTextures) -> Option<()> {
        let shape = self.find_or_create(name);
        if shape.texture.is_some() {
            return None;
        }
        lib.add_ref(texture);
        shape.texture = Some(texture);
        shape.set_rect(lib, 0.0, 0.0, 0.0, 0.0, -1.0, -1.0, -1.0, -1.0);
        Some(())
    }

    fn load_sections(
        &mut self,
        ini: &mut IniReader,
        lib: &mut dyn ShapeTextures,
        groups: &mut Vec<i32>,
    ) {
        let mut current_file: Vec<u8> = Vec::new();
        let mut current_texture: Option<u32> = None;
        while ini.read_header() {
            let mut current_shape: Option<Vec<u8>> = None;
            if ini.is_header(b"Texture") {
                while ini.read_value() {
                    if ini.is_value(b"file") {
                        current_texture = None;
                        current_shape = None;
                        let value = ini.value_string_at(0);
                        let value = &value[..value.len().min(0x103)];
                        if value.is_empty()
                            || neuron_core::hash::eq_ignore_case(&current_file, value)
                        {
                            continue;
                        }
                        current_file = value.to_vec();
                        if let Some(group) = lib.load_group(&current_file) {
                            groups.push(group);
                        }
                    } else if ini.is_value(b"name") || ini.is_value(b"texture_name") {
                        current_shape = None;
                        let name = ini.value_string_at(0);
                        current_texture = lib.find(&name[..name.len().min(0x103)]);
                    } else if ini.is_value(b"shape_name") {
                        let name = ini.value_string_at(0);
                        let name = &name[..name.len().min(0x103)];
                        current_shape = match current_texture {
                            Some(t) => self.set_new(name, t, lib).map(|()| name.to_vec()),
                            // The reference attaches the null record (and
                            // complains); the shape then has no texture.
                            None => {
                                let shape = self.find_or_create(name);
                                (shape.texture.is_none()).then(|| name.to_vec())
                            }
                        };
                    } else if ini.is_value(b"tex_shape") {
                        current_shape = None;
                        current_texture = None;
                        let name = ini.value_string_at(0);
                        if let Some(t) = lib.find(&name) {
                            let _ = self.set_new(&name, t, lib);
                        }
                    } else if ini.is_value(b"dim") {
                        if current_texture.is_none() {
                            continue;
                        }
                        let Some(name) = current_shape.take() else {
                            continue;
                        };
                        let mut v = [0.0f32; 8];
                        for (i, slot) in v.iter_mut().enumerate() {
                            let i = i as u32;
                            *slot = if i >= 4 && ini.is_value_empty(i) {
                                0.0
                            } else {
                                ini.value_float(i) as f32
                            };
                        }
                        let shape = *self.find_or_create(&name);
                        let mut shape = shape;
                        shape.set_rect(lib, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
                        *self.find_or_create(&name) = shape;
                    }
                }
            } else if ini.is_header(b"Shape") {
                let mut name: Vec<u8> = Vec::new();
                let (mut x, mut y, mut w, mut h) = (0.0f32, 0.0f32, 0.0f32, 0.0f32);
                let (mut ul_u, mut ul_v, mut lr_u, mut lr_v) = (-1.0f32, -1.0f32, -1.0f32, -1.0f32);
                while ini.read_value() {
                    if ini.is_value(b"name") || ini.is_value(b"shape_name") {
                        let v = ini.value_string_at(0);
                        name = v[..v.len().min(0x103)].to_vec();
                    } else if ini.is_value(b"x") {
                        x = ini.value_float(0) as f32;
                    } else if ini.is_value(b"y") {
                        y = ini.value_float(0) as f32;
                    } else if ini.is_value(b"w") {
                        w = ini.value_float(0) as f32;
                    } else if ini.is_value(b"h") {
                        h = ini.value_float(0) as f32;
                    } else if ini.is_value(b"upper_left") {
                        ul_u = ini.value_float(0) as f32;
                        ul_v = ini.value_float(1) as f32;
                    } else if ini.is_value(b"lower_right") {
                        lr_u = (ini.value_float(0) + 1.0) as f32;
                        lr_v = (ini.value_float(1) + 1.0) as f32;
                    } else if ini.is_value(b"dim") {
                        x = ini.value_float(0) as f32;
                        y = ini.value_float(1) as f32;
                        w = ini.value_float(2) as f32;
                        h = ini.value_float(3) as f32;
                        ul_u = ini.value_float(4) as f32;
                        ul_v = ini.value_float(5) as f32;
                        lr_u = ini.value_float(6) as f32;
                        lr_v = ini.value_float(7) as f32;
                    }
                }
                if name.is_empty() {
                    continue;
                }
                let Some(texture) = current_texture else {
                    continue;
                };
                if self.set_new(&name, texture, lib).is_some() {
                    let mut shape = *self.find_or_create(&name);
                    shape.set_rect(lib, x, y, w, h, ul_u, ul_v, lr_u, lr_v);
                    *self.find_or_create(&name) = shape;
                }
            }
        }
    }
}

/// A [`ShapeTextures`] of fixed-size textures for tests and data checks:
/// every name is a texture of the given size, every file a group.
#[derive(Clone, Debug, Default)]
pub struct FixedTextures {
    pub size: (u32, u32),
    pub names: Vec<Vec<u8>>,
    pub files: Vec<Vec<u8>>,
    pub refs: BTreeMap<u32, i32>,
    pub collected: Vec<i32>,
    pub slot_4: u32,
}

impl ShapeTextures for FixedTextures {
    fn find(&mut self, name: &[u8]) -> Option<u32> {
        if name.is_empty() {
            return None;
        }
        let i = match self
            .names
            .iter()
            .position(|n| neuron_core::hash::eq_ignore_case(n, name))
        {
            Some(i) => i,
            None => {
                self.names.push(name.to_vec());
                self.names.len() - 1
            }
        };
        Some(i as u32 + 1)
    }
    fn add_ref(&mut self, record: u32) {
        *self.refs.entry(record).or_default() += 1;
    }
    fn release(&mut self, record: u32) {
        *self.refs.entry(record).or_default() -= 1;
    }
    fn load_group(&mut self, file: &[u8]) -> Option<i32> {
        self.files.push(file.to_vec());
        Some(self.files.len() as i32)
    }
    fn garbage_collect(&mut self, group: i32) {
        self.collected.push(group);
    }
    fn generic_slot_4(&mut self) -> u32 {
        self.slot_4
    }
    fn set_generic_slot_4(&mut self, value: u32) {
        self.slot_4 = value;
    }
    fn frame_handle(&mut self, record: u32, _frame: i32) -> u32 {
        record + 100
    }
    fn texture_dim(&mut self, handle: u32) -> (u32, u32) {
        if handle == 0 { (0, 0) } else { self.size }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shapes_from_a_texture_and_shape_sections() {
        let mut lib = FixedTextures {
            size: (256, 256),
            slot_4: 9,
            ..FixedTextures::default()
        };
        let mut db = ShapeDb::new();
        let n = db.load(
            b"[Texture]\nfile = screens\\cursor.TXM\nname = newcursors\n\n\
[Shape]\nname = Arrow\nx = 0\ny = 64\nw = 64\nh = 64\n\n\
[Shape]\nname = whole\n\n\
[Shape]\nname = averyveryverylongshapename_cut\nx = 0.5\ny = 0.5\nw = 0.25\nh = 0.25\n",
            &mut lib,
        );
        assert_eq!(n, 3);
        assert_eq!(lib.files, vec![b"screens\\cursor.TXM".to_vec()]);
        assert_eq!(lib.collected, vec![1]);
        assert_eq!(lib.slot_4, 9);
        let arrow = db.find(b"ARROW").unwrap();
        assert_eq!((arrow.pixel_w, arrow.pixel_h), (64, 64));
        assert_eq!(arrow.u0, (0.01f64 as f32 as f64 / 256.0) as f32);
        assert_eq!(arrow.v1, ((128.0 - f64::from(0.01f32)) / 256.0) as f32);
        // An even span: centred, unit scale.
        assert_eq!((arrow.scale_u, arrow.offset_u), (1.0, 0.0));
        let whole = db.find(b"whole").unwrap();
        assert_eq!((whole.pixel_w, whole.pixel_h), (256, 256));
        let cut = db.find(b"averyveryverylongshapen").unwrap();
        assert_eq!((cut.pixel_w, cut.pixel_h), (64, 64));
        assert_eq!(cut.u0, ((128.0 + f64::from(0.01f32)) / 256.0) as f32);
        assert_eq!(lib.refs.get(&1), Some(&3));
        db.clear(&mut lib);
        assert_eq!(lib.refs.get(&1), Some(&0));
    }

    #[test]
    fn odd_spans_move_the_u_centre_half_a_pixel() {
        let mut lib = FixedTextures {
            size: (64, 64),
            ..FixedTextures::default()
        };
        let mut s = TexShape {
            texture: Some(1),
            ..TexShape::default()
        };
        s.set_rect(&mut lib, 2.0, 2.0, 5.0, 5.0, -1.0, -1.0, -1.0, -1.0);
        assert_eq!(s.offset_u, (0.5 / 2.5f64) as f32);
        assert_eq!(s.offset_v, 0.0);
    }
}
