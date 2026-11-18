//! `FontImp` (`font_imp.cpp`): one font of `fonts.ini`, a face realised at
//! a pixel height that follows the surface.
//!
//! The height is `fixed_height` times the surface height plus a delta the
//! autoscale search finds: starting from that height it steps one pixel
//! at a time until the width of a calibration pangram, over the surface
//! width, crosses the share the pangram takes at 800 x 600. The delta is
//! cached per surface width. Every face call first re-runs this when the
//! layout generation has moved (`RefreshScale`).

use std::collections::BTreeMap;

use neuron_core::num::trunc_to_i32;

use super::{FaceHandle, FontCx, LogFont, TextMetrics, wide_c_str};

/// The calibration pangram every autoscale search measures.
pub const PANGRAM: &str = "In the beginning the Earth was without form and void.";

/// The reference width factor (1/800) of the first search's target.
const PANGRAM_WIDTH_SCALE: f32 = 0.00125;

/// `TString<64>` assignment: at most 63 bytes.
fn tstring64(value: &[u8]) -> Vec<u8> {
    let value = neuron_core::hash::c_str(value);
    value[..value.len().min(63)].to_vec()
}

/// `FFontDesc`: what `Init` builds a font from.
#[derive(Clone, Debug, PartialEq)]
pub struct FontDesc {
    pub face: Vec<u8>,
    /// A share of the surface height; 0 turns the autoscale search off.
    pub fixed_height: f32,
    pub bold: bool,
    pub italic: bool,
    pub underline: bool,
}

/// `FontImp`.
#[derive(Clone, Debug)]
pub struct Font {
    name: Vec<u8>,
    id: Vec<u8>,
    face: Vec<u8>,
    fixed_height: f32,
    init_line_height: i32,
    log_font: LogFont,
    handle: Option<FaceHandle>,
    realized_height: i32,
    metrics: TextMetrics,
    layout_generation: i32,
    scale_delta_by_width: BTreeMap<i32, i32>,
}

impl Default for Font {
    fn default() -> Self {
        Self::new()
    }
}

impl Font {
    /// The constructor: empty strings and cache, everything zero.
    #[must_use]
    pub fn new() -> Font {
        Font {
            name: Vec::new(),
            id: Vec::new(),
            face: Vec::new(),
            fixed_height: 0.0,
            init_line_height: 0,
            log_font: LogFont::new(b"", 0, false, false, false),
            handle: None,
            realized_height: 0,
            metrics: TextMetrics::default(),
            layout_generation: 0,
            scale_delta_by_width: BTreeMap::new(),
        }
    }

    /// `SetName`: the id takes the nickname only while it is empty, the
    /// name always.
    pub fn set_name(&mut self, nickname: &[u8]) {
        if self.id.is_empty() {
            self.id = tstring64(nickname);
        }
        self.name = tstring64(nickname);
    }

    /// `Init`: the previous face forgotten, the id `"<face>,0"`, the face
    /// and its `LOGFONTA`, then the autoscale (the generation is 0, so it
    /// always runs) and the line height snapshot. Always succeeds.
    ///
    /// The reference formats the id with `"%s,%d"` and no second argument,
    /// so its number is a stack word (divergence D292).
    pub fn init(&mut self, cx: &mut FontCx<'_>, desc: &FontDesc) -> bool {
        self.handle = None;
        self.layout_generation = 0;
        let mut id = tstring64(&desc.face);
        id.extend_from_slice(b",0");
        self.id = tstring64(&id);
        self.face = tstring64(&desc.face);
        self.log_font = LogFont::new(&self.face, 0, desc.bold, desc.italic, desc.underline);
        self.fixed_height = desc.fixed_height;
        self.refresh_scale(cx, false);
        self.init_line_height = self.metrics.height;
        true
    }

    /// `Realize`: (re)create the face at `height`. Nothing to do when a
    /// face exists at that `lfHeight` or with that `tmHeight`. On success
    /// the metrics are read and the generation taken; on failure the face
    /// is gone and the previous metrics stay.
    fn realize(&mut self, cx: &mut FontCx<'_>, height: i32) -> bool {
        if let Some(face) = self.handle {
            if self.realized_height == height || height == self.metrics.height {
                return true;
            }
            cx.backend.delete_font(face);
            self.handle = None;
        }
        self.log_font.height = height;
        let Some(face) = cx.backend.create_font(&self.log_font) else {
            return false;
        };
        self.handle = Some(face);
        self.realized_height = height;
        self.metrics = cx.backend.text_metrics(face);
        self.layout_generation = cx.env.layout_generation;
        true
    }

    /// The pangram's width as `MeasureString` gives it, read unsigned.
    fn pangram_width(&mut self, cx: &mut FontCx<'_>) -> u32 {
        let pangram: Vec<u16> = PANGRAM.encode_utf16().collect();
        self.measure_string(cx, Some(&pangram)).0.cast_unsigned()
    }

    /// `SearchScaleDelta`: from `round(fixed * screen_h)`, one pixel at a
    /// time towards the height whose pangram ratio crosses `target`.
    fn search_scale_delta(
        &mut self,
        cx: &mut FontCx<'_>,
        fixed: f32,
        screen_w: i32,
        screen_h: i32,
        target: f32,
    ) -> i32 {
        let start = trunc_to_i32((f64::from(screen_h) * f64::from(fixed) + 0.5).floor());
        let mut delta = 0i32;
        self.realize(cx, start);
        let width_f = screen_w as f32;
        let target = f64::from(target);
        let mut ratio = f64::from(self.pangram_width(cx)) / f64::from(width_f);
        let step = if ratio > target { -1 } else { 1 };
        if (ratio - target).abs() > 0.001 {
            loop {
                if self.line_height(cx).cast_unsigned() > start.wrapping_add(delta).cast_unsigned()
                {
                    break;
                }
                self.realize(cx, start.wrapping_add(step).wrapping_add(delta));
                ratio = f64::from(self.pangram_width(cx)) / f64::from(width_f);
                if step > 0 {
                    if ratio > target {
                        break;
                    }
                    delta += step;
                } else {
                    delta += step;
                    if ratio <= target {
                        break;
                    }
                    if start.wrapping_add(delta) <= 4 {
                        break;
                    }
                }
                let diff = (ratio - target).abs();
                if diff.is_nan() || diff <= 0.001 {
                    break;
                }
            }
        }
        delta
    }

    /// The surface width both searches use and cache by: with widescreen
    /// support, the width of a 4:3 surface of the same height, rounded.
    fn surface_width(cx: &FontCx<'_>) -> i32 {
        let env = cx.env;
        if !env.widescreen {
            return env.surface_width;
        }
        let w = i64::from(env.surface_width);
        let h = i64::from(env.surface_height);
        let scale = if h <= 0 || w * 3 <= h * 4 {
            1.0
        } else {
            (w as f64 * 3.0) / (h as f64 * 4.0)
        };
        trunc_to_i32((f64::from(env.surface_width) / scale + 0.5).floor())
    }

    /// The pixel height `round(fixed * 600)` the targets are measured at.
    fn reference_height(&self) -> i32 {
        trunc_to_i32((f64::from(self.fixed_height) * 600.0 + 0.5).floor())
    }

    /// The cached delta for the surface width, searched on a miss with the
    /// target `pangram width at round(fixed * 600)` times 0.00125.
    fn compute_scale_delta(&mut self, cx: &mut FontCx<'_>) -> i32 {
        if self.fixed_height == 0.0 {
            return 0;
        }
        let surface_width = Self::surface_width(cx);
        if let Some(&delta) = self.scale_delta_by_width.get(&surface_width) {
            return delta;
        }
        let had_font = self.handle.is_some();
        let old_height = self.realized_height;
        self.realize(cx, self.reference_height());
        let width = self.pangram_width(cx);
        let target = (f64::from(width) * f64::from(PANGRAM_WIDTH_SCALE)) as f32;
        let delta = self.search_scale_delta(
            cx,
            self.fixed_height,
            surface_width,
            cx.env.surface_height,
            target,
        );
        self.scale_delta_by_width.insert(surface_width, delta);
        if had_font {
            self.realize(cx, old_height);
        }
        delta
    }

    /// The same cache, with the target `MeasurePangramRatio(round(fixed *
    /// 600), 800)`: the pangram width over 800.
    fn compute_scale_delta_by_ratio(&mut self, cx: &mut FontCx<'_>) -> i32 {
        if self.fixed_height == 0.0 {
            return 0;
        }
        let surface_width = Self::surface_width(cx);
        if let Some(&delta) = self.scale_delta_by_width.get(&surface_width) {
            return delta;
        }
        let had_font = self.handle.is_some();
        let old_height = self.realized_height;
        self.realize(cx, self.reference_height());
        let width = self.pangram_width(cx);
        let target = (f64::from(width) / 800.0) as f32;
        let delta = self.search_scale_delta(
            cx,
            self.fixed_height,
            surface_width,
            cx.env.surface_height,
            target,
        );
        self.scale_delta_by_width.insert(surface_width, delta);
        if had_font {
            self.realize(cx, old_height);
        }
        delta
    }

    /// `RefreshScale`, the prelude of every face call: when the generation
    /// moved, take it and realise at `round(delta + fixed * surface_h)`.
    fn refresh_scale(&mut self, cx: &mut FontCx<'_>, ratio_target: bool) {
        if self.layout_generation == cx.env.layout_generation {
            return;
        }
        self.layout_generation = cx.env.layout_generation;
        let base = (f64::from(self.fixed_height) * f64::from(cx.env.surface_height)) as f32;
        let delta = if ratio_target {
            self.compute_scale_delta_by_ratio(cx)
        } else {
            self.compute_scale_delta(cx)
        };
        let height = trunc_to_i32((f64::from(delta) + f64::from(base) + 0.5).floor());
        self.realize(cx, height);
    }

    /// The id (`Slot00`).
    #[must_use]
    pub fn id(&self) -> &[u8] {
        &self.id
    }

    /// `GetName`: the nickname.
    #[must_use]
    pub fn name(&self) -> &[u8] {
        &self.name
    }

    /// The face name the INI gave.
    #[must_use]
    pub fn face_name(&self) -> &[u8] {
        &self.face
    }

    /// `GetFixedHeight`.
    #[must_use]
    pub fn fixed_height(&self) -> f32 {
        self.fixed_height
    }

    /// The `lfHeight` of the face last realised.
    #[must_use]
    pub fn realized_height(&self) -> i32 {
        self.realized_height
    }

    /// The metrics of the face last realised.
    #[must_use]
    pub fn metrics(&self) -> TextMetrics {
        self.metrics
    }

    /// `Slot03`: the face, after the autoscale prelude.
    pub fn face(&mut self, cx: &mut FontCx<'_>) -> Option<FaceHandle> {
        self.refresh_scale(cx, false);
        self.handle
    }

    /// `GetLogFont`, after the autoscale prelude.
    pub fn log_font(&mut self, cx: &mut FontCx<'_>) -> LogFont {
        self.refresh_scale(cx, false);
        self.log_font
    }

    /// `GetGlyphAdvance`: `GetCharWidthW` of one character (0 when it
    /// fails), after the ratio prelude.
    pub fn glyph_advance(&mut self, cx: &mut FontCx<'_>, ch: u16) -> f32 {
        self.refresh_scale(cx, true);
        let width = self
            .handle
            .and_then(|face| cx.backend.char_width(face, ch))
            .unwrap_or(0);
        width as f32
    }

    /// `GetLineHeight`: `tmHeight`, or the snapshot of `Init` without a
    /// face.
    pub fn line_height(&mut self, cx: &mut FontCx<'_>) -> i32 {
        self.refresh_scale(cx, false);
        if self.handle.is_some() {
            self.metrics.height
        } else {
            self.init_line_height
        }
    }

    /// `Slot07`: the `tmHeight` snapshot `Init` took.
    #[must_use]
    pub fn init_line_height(&self) -> i32 {
        self.init_line_height
    }

    /// `MeasureString` with the length -1: a null text is 0 x 0, any other
    /// (up to its NUL) `GetTextExtentPointW`, 0 x 0 when that fails.
    ///
    /// The reference measures with the per-character widths instead when
    /// the Windows major version is 4 or less; the port runs on later
    /// versions only. A font whose face could not be created measures as
    /// 0 x 0 (divergence D291).
    pub fn measure_string(&mut self, cx: &mut FontCx<'_>, text: Option<&[u16]>) -> (i32, i32) {
        self.refresh_scale(cx, false);
        let Some(text) = text else {
            return (0, 0);
        };
        let Some(face) = self.handle else {
            return (0, 0);
        };
        cx.backend
            .text_extent(face, wide_c_str(text))
            .unwrap_or((0, 0))
    }

    /// `ProbeString` with the length -1: how many leading characters fit
    /// in `max_width`, the sums compared unsigned.
    pub fn probe_string(&mut self, cx: &mut FontCx<'_>, max_width: i32, text: &[u16]) -> i32 {
        self.refresh_scale(cx, true);
        let text = wide_c_str(text);
        let limit = max_width.cast_unsigned();
        let mut count = 0;
        let mut sum = 0u32;
        for &ch in text {
            if sum >= limit {
                break;
            }
            let width = self
                .handle
                .and_then(|face| cx.backend.char_width(face, ch))
                .unwrap_or(0);
            let next = width.cast_unsigned().wrapping_add(sum);
            if next > limit {
                break;
            }
            count += 1;
            sum = next;
        }
        count
    }

    /// `GetCharWidths` over every unit of `text` (NULs included, as the
    /// caller gives the count): each width (0 when it fails) into `out`,
    /// the sum returned.
    pub fn char_widths(&mut self, cx: &mut FontCx<'_>, text: &[u16], out: &mut [i32]) -> i32 {
        self.refresh_scale(cx, true);
        let mut sum = 0i32;
        for (slot, &ch) in out.iter_mut().zip(text) {
            let width = self
                .handle
                .and_then(|face| cx.backend.char_width(face, ch))
                .unwrap_or(0);
            sum = sum.wrapping_add(width);
            *slot = width;
        }
        sum
    }
}
