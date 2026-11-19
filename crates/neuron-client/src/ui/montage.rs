//! The button montage (`nn_montage_draw.cpp`): the tile atlas behind the
//! NeuroNet's and the HUD's 2D icons. `interface\ButtonMontage.ini` names
//! square tiles on the shapes of `Interface\ButtonTextures.ini`; a button
//! whose mesh path names a tile draws the tile as a screen-space icon
//! through the Win3D camera instead of its mesh (`NnMontageDrawTileAt`),
//! into the "buttons" screen-quad list that `Win3DDrawAll` flushes after
//! the roots drew (`NnMontageFlushButtons`).
//!
//! The reference loads the montage on first use (`LoadButtonMontage`
//! through `ButtonMontageEnsure` or a button's tile search); the engine
//! loads it when it starts the UI manager (divergence D511).

use neuron_core::hash::name_crc32;
use neuron_core::ini::IniReader;

use super::quad_batch::{Frame, IconCamera, QuadBatcher, QuadDevice};
use super::services::MontageTile;
use super::shape::{ShapeDb, ShapeTextures, TexShape};

/// `Interface\ButtonTextures.ini`.
pub const BUTTON_TEXTURES_INI: &[u8] = b"Interface\\ButtonTextures.ini";
/// `interface\ButtonMontage.ini`.
pub const BUTTON_MONTAGE_INI: &[u8] = b"interface\\ButtonMontage.ini";

/// `D3DRS_ZWRITEENABLE`, `D3DRS_ZFUNC`.
const RS_ZWRITEENABLE: u32 = 14;
const RS_ZFUNC: u32 = 23;
/// `0.00390625f`: the tile's inset, a 256th.
const INSET: f32 = 0.003_906_25;

/// `G_ButtonsMaterialKey`: the CRC of "buttons".
#[must_use]
pub fn buttons_key() -> u32 {
    name_crc32(b"buttons")
}

/// One `FButtonMontageTile` with its ini key.
#[derive(Clone, Debug, PartialEq)]
pub struct MontageEntry {
    /// The ini key (a mesh path).
    pub name: Vec<u8>,
    pub tile: MontageTile,
}

/// The montage: the shapes of `ButtonTextures.ini` and the tiles of
/// `ButtonMontage.ini` in file order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ButtonMontage {
    /// `G_bButtonMontageLoaded`: set by the first load, never cleared.
    pub loaded: bool,
    pub shapes: ShapeDb,
    pub tiles: Vec<MontageEntry>,
}

fn r(v: f64) -> f32 {
    v as f32
}

impl ButtonMontage {
    /// `LoadButtonMontage`: once, the shapes of `textures` (the text of
    /// `ButtonTextures.ini`) and the tiles of every `[Montage]` section of
    /// `montage` (the text of `ButtonMontage.ini`; `size`, `texture`,
    /// `num_pages` and one key per tile with the optional scale), each tile
    /// one step on from the last along its row.
    pub fn load(
        &mut self,
        textures: Option<&[u8]>,
        montage: Option<&[u8]>,
        lib: &mut dyn ShapeTextures,
    ) {
        if self.loaded {
            return;
        }
        self.loaded = true;
        if let Some(text) = textures {
            self.shapes.load(text, lib);
        }
        let find = |name: &[u8]| self.shapes.find(name).copied();
        let montage32 = find(b"Montage32");
        let montage64 = find(b"Montage64");
        let targeting = find(b"HUD_targetingelement");
        let talk = find(b"HUD_talkelement");
        let Some(text) = montage else {
            return;
        };
        let mut ini = IniReader::new();
        if !ini.open_memory(text) || !ini.read_header() {
            return;
        }
        loop {
            let (mut step, mut u, mut v) = (0.0f32, 0.0f32, 0.0f32);
            let (mut texture, mut size) = (0i32, 0i32);
            if ini.is_header(b"Montage") && ini.read_value() {
                loop {
                    if ini.is_value(b"size") {
                        size = ini.value_int(0);
                        let tiles_per_row = if size > 0 { 0x100 / size } else { 4 };
                        step = r(1.0 / f64::from(tiles_per_row));
                        u = 0.0;
                        v = 0.0;
                    } else if ini.is_value(b"texture") {
                        texture = ini.value_int(0);
                    } else if ini.is_value(b"num_pages") {
                        let _ = ini.value_int(0);
                    } else {
                        let shape: Option<TexShape> = if texture == 2 {
                            targeting
                        } else if texture == 3 {
                            step = 0.5;
                            talk
                        } else if size == 0x20 {
                            montage32
                        } else if size == 0x40 {
                            montage64
                        } else {
                            None
                        };
                        let scale = if ini.is_value_empty(1) {
                            0.0
                        } else {
                            r(ini.value_float(1))
                        };
                        self.tiles.push(MontageEntry {
                            name: ini.name(),
                            tile: MontageTile {
                                index: self.tiles.len(),
                                scale,
                                u,
                                v,
                                step,
                                shape,
                            },
                        });
                        let next_u = f64::from(step) + f64::from(u);
                        u = r(next_u);
                        if next_u > f64::from(0.99f32) {
                            u = 0.0;
                            let next_v = f64::from(step) + f64::from(v);
                            v = r(next_v);
                            if next_v > f64::from(0.99f32) {
                                v = 0.0;
                            }
                        }
                    }
                    if !ini.read_value() {
                        break;
                    }
                }
            }
            if !ini.read_header() {
                break;
            }
        }
    }

    /// `FindButtonMontageTile` (and the buttons' inline search): the first
    /// tile whose key equals `path` without case.
    #[must_use]
    pub fn find(&self, path: &[u8]) -> Option<MontageTile> {
        self.tiles
            .iter()
            .find(|e| e.name.eq_ignore_ascii_case(path))
            .map(|e| e.tile)
    }

    /// `ClearButtonMontage`: the tiles and the shapes gone; the loaded
    /// flag stays set.
    pub fn clear(&mut self, lib: &mut dyn ShapeTextures) {
        self.tiles.clear();
        self.shapes.clear(lib);
    }
}

/// `NnMontageDrawTileAt`: false without a tile or its shape; else frame 0
/// of the shape's texture, its UV rect replaced by the tile's square inset
/// by a 256th and mirrored per `mode` (0 plain, 1 horizontal, 2 vertical,
/// 3 both; any other value keeps the frame's rect), and one icon of `size`
/// at `position` through `camera` in the colour of the RGBA bytes, on the
/// "buttons" list. True then.
#[allow(clippy::too_many_arguments)]
pub fn draw_tile_at(
    quads: &mut QuadBatcher,
    lib: &mut dyn ShapeTextures,
    camera: &IconCamera,
    tile: Option<&MontageTile>,
    position: [f32; 3],
    rgba: [u8; 4],
    size: f32,
    mode: i32,
    angle: f32,
) -> bool {
    let Some(tile) = tile else {
        return false;
    };
    let Some(shape) = tile.shape else {
        return false;
    };
    let (handle, uv) = shape
        .texture
        .map_or((0, [0, 0, 0x3F80_0000, 0x3F80_0000]), |t| lib.frame(t, 0));
    let inset = f64::from(INSET);
    let low0 = r(f64::from(tile.u) + inset);
    let low1 = r(f64::from(tile.v) + inset);
    let high0 = r(f64::from(tile.step) + f64::from(tile.u) - inset);
    let high1 = r(f64::from(tile.step) + f64::from(tile.v) - inset);
    let rect = match mode {
        0 => [low0, low1, high0, high1],
        1 => [high0, low1, low0, high1],
        2 => [low0, high1, high0, low1],
        3 => [high0, high1, low0, low1],
        _ => uv.map(f32::from_bits),
    };
    quads.set_material_key(buttons_key());
    quads.begin_frame(
        Frame {
            texture: handle,
            u0: rect[0],
            v0: rect[1],
            u1: rect[2],
            v1: rect[3],
        },
        2,
        true,
    );
    let argb = (u32::from(rgba[3]) << 24)
        | (u32::from(rgba[0]) << 16)
        | (u32::from(rgba[1]) << 8)
        | u32::from(rgba[2]);
    quads.enqueue_icon(camera, position, argb, size, angle);
    true
}

/// `NnMontageFlushButtons`: depth function `ALWAYS` and no depth write
/// around the flush of the "buttons" list, both put back.
pub fn flush_buttons(device: &mut dyn QuadDevice, quads: &mut QuadBatcher) {
    let saved_func = device.render_state(RS_ZFUNC);
    device.set_render_state(RS_ZFUNC, 8);
    let saved_write = device.render_state(RS_ZWRITEENABLE);
    device.set_render_state(RS_ZWRITEENABLE, 0);
    quads.flush(device, buttons_key());
    device.set_render_state(RS_ZWRITEENABLE, saved_write);
    device.set_render_state(RS_ZFUNC, saved_func);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ui::shape::FixedTextures;

    const MONTAGE: &[u8] = b"[Montage]\nsize = 64\nnum_pages = 1\na.3db = x.tga, 0.5\nb.3db = y.tga\nc.3db = z.tga, 0\nd.3db = w.tga\ne.3db = v.tga\n[Montage]\nsize = 32\ntexture = 3\nf.3db = u.tga\ng.3db = t.tga\n[Other]\nh.3db = s.tga\n";

    #[test]
    fn the_tiles_step_along_rows_of_the_shapes() {
        let mut m = ButtonMontage::default();
        let mut lib = FixedTextures::default();
        m.load(None, Some(MONTAGE), &mut lib);
        assert!(m.loaded);
        let t: Vec<_> = m
            .tiles
            .iter()
            .map(|e| (e.tile.u, e.tile.v, e.tile.step, e.tile.scale))
            .collect();
        assert_eq!(
            t,
            [
                (0.0, 0.0, 0.25, 0.5),
                (0.25, 0.0, 0.25, 0.0),
                (0.5, 0.0, 0.25, 0.0),
                (0.75, 0.0, 0.25, 0.0),
                (0.0, 0.25, 0.25, 0.0),
                (0.0, 0.0, 0.5, 0.0),
                (0.5, 0.0, 0.5, 0.0),
            ]
        );
        assert_eq!(m.find(b"B.3DB").map(|t| t.index), Some(1));
        assert_eq!(m.find(b"h.3db"), None);
        // A second load does nothing.
        m.load(None, Some(MONTAGE), &mut lib);
        assert_eq!(m.tiles.len(), 7);
    }

    #[test]
    fn a_tile_without_a_shape_draws_nothing() {
        let mut quads = QuadBatcher::new();
        let mut lib = FixedTextures::default();
        let cam = IconCamera {
            view: [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0],
            z_near: 0.1,
            x_scale_over_near: 5000.0,
            y_scale_over_near: -5000.0,
            viewport: [0, 0, 639, 479],
        };
        let tile = MontageTile::default();
        assert!(!draw_tile_at(
            &mut quads,
            &mut lib,
            &cam,
            Some(&tile),
            [0.0, 0.0, -1.0],
            [255; 4],
            0.05,
            0,
            0.0
        ));
        assert!(!draw_tile_at(
            &mut quads,
            &mut lib,
            &cam,
            None,
            [0.0, 0.0, -1.0],
            [255; 4],
            0.05,
            0,
            0.0
        ));
        let tile = MontageTile {
            shape: Some(TexShape {
                texture: Some(3),
                ..TexShape::default()
            }),
            step: 0.25,
            ..MontageTile::default()
        };
        assert!(draw_tile_at(
            &mut quads,
            &mut lib,
            &cam,
            Some(&tile),
            [0.0, 0.0, -1.0],
            [0x10, 0x20, 0x30, 0x40],
            0.05,
            0,
            0.0
        ));
        let list = quads.list(buttons_key()).expect("the buttons list");
        let node = list
            .live
            .iter()
            .find(|n| n.texture == 103)
            .expect("the node");
        let rec = node.records[0];
        assert_eq!(rec.color, 0x4010_2030);
        assert!((rec.u0 - 0.003_906_25).abs() < 1e-7);
    }
}
