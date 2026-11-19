//! Widescreen anchoring: the reference's port-only widescreen support
//! (`OUTPOST_FRONTIER_WIDESCREEN_SUPPORT`, on in the reference build;
//! `widescreen.h`, `hud_widescreen.cpp`).
//!
//! The original game shows 4:3 modes only. With the support the cameras
//! go "Hor+" ([`hor_plus_scale`]): a camera whose viewport spans the full
//! surface width widens its horizontal angle by the surface's aspect over
//! 4:3, so the Win3D interface keeps the size and place a 4:3 surface of
//! the same height gives it, in the middle, and the extra width is added
//! at the sides. The in-flight HUD's corner windows are then pushed out to
//! the edges: while such a window is drawn, updated or offered the mouse,
//! the Win3D camera is sheared so that every point, at any depth, lands
//! [`shift_pixels`] pixels to the side ([`Shift`]).
//!
//! Which windows move is the game's knowledge. The reference tests window
//! names and C++ types (`HUD_Status` and the NeuroNet buttons to the
//! right; `HUD_Target`, `HUD_ContactList`, the comm window and the main
//! menu column to the left, its "Version" caption to the right; three
//! children by name stay centred), and the NeuroNet button flying into the
//! centred screen loses its share with the move. Here each node carries an
//! [`Anchor`] the game sets (and updates, for a moving button): a centred
//! node keeps its subtree in the middle, and an object takes the share of
//! the nearest edge anchor above it (divergence D343).

use neuron_core::math::{Transform, Vector};

use super::camera::Camera;

/// The surface the interface is drawn on (`G_RenderSurfaceWidth`,
/// `G_RenderSurfaceHeight`) and whether widescreen support is on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Surface {
    pub width: i32,
    pub height: i32,
    /// `OUTPOST_FRONTIER_WIDESCREEN_SUPPORT`; on in the reference build.
    pub widescreen: bool,
}

impl Default for Surface {
    fn default() -> Self {
        Surface::new(0, 0)
    }
}

impl Surface {
    /// A surface with widescreen support on.
    #[must_use]
    pub fn new(width: i32, height: i32) -> Surface {
        Surface {
            width,
            height,
            widescreen: true,
        }
    }
}

/// `WidescreenHorPlusScale`: the surface aspect over 4:3 for a viewport as
/// wide as a surface wider than 4:3, otherwise 1 (and always 1 without
/// widescreen support).
#[must_use]
pub fn hor_plus_scale(surface: &Surface, viewport_width: i64) -> f64 {
    if !surface.widescreen {
        return 1.0;
    }
    let width = i64::from(surface.width);
    let height = i64::from(surface.height);
    if height <= 0 || viewport_width != width {
        return 1.0;
    }
    if width * 3 <= height * 4 {
        return 1.0;
    }
    (width as f64 * 3.0) / (height as f64 * 4.0)
}

/// `WidescreenEquivalentSurfaceWidth`: the width of a 4:3 surface of the
/// same height (the surface width when it is not wider than 4:3).
#[must_use]
pub fn equivalent_surface_width(surface: &Surface) -> f64 {
    f64::from(surface.width) / hor_plus_scale(surface, i64::from(surface.width))
}

/// Where a node sits on a widescreen surface.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum Anchor {
    /// The node follows its root (a root with it stays in the 4:3 middle).
    #[default]
    Inherit,
    /// The node and everything under it stay in the middle, whatever its
    /// root does (`IsCentredChild`).
    Centre,
    /// The signed share of the spare width the node and its subtree move
    /// by, +1 to the right edge, -1 to the left, unless a nearer anchor
    /// below says otherwise.
    Edge(f32),
}

/// The pixel shift of a share: half the spare width in whole pixels, 0 on
/// a surface less than two pixels wider than its 4:3 equivalent
/// (`WidescreenHudShiftPixelsFor`'s tail).
#[must_use]
pub fn shift_pixels(surface: &Surface, share: f32) -> f32 {
    if share == 0.0 {
        return 0.0;
    }
    let spare = (f64::from(surface.width) - equivalent_surface_width(surface)) * 0.5;
    if !(spare >= 1.0) {
        return 0.0;
    }
    (f64::from(share) * spare.floor() + 0.5).floor() as f32
}

/// The camera state a shift replaced (`G_BaseView`, `G_BaseViewPos`,
/// `G_BaseScreenCenterX`) and the shift itself (`G_HudShift`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Shift {
    pub pixels: f32,
    base_view: Transform,
    base_screen_center_x: f32,
}

/// The shear of a 3x4 transform for a shift of `s = pixels / x_scale`:
/// row 0 minus `s` times row 2, and the translation's x minus `s` times
/// its z.
fn shear(view: &Transform, s: f64) -> Transform {
    let mut out = *view;
    for column in 0..3 {
        out.rotation.m[0][column] = (f64::from(view.rotation.m[0][column])
            - s * f64::from(view.rotation.m[2][column])) as f32;
    }
    out.translation.x = (f64::from(view.translation.x) - s * f64::from(view.translation.z)) as f32;
    out
}

impl Shift {
    /// `SetShift`: the previous shift undone, then the new one applied to
    /// the camera's view transform (rebuilt first when dirty) and screen
    /// centre.
    pub fn set(&mut self, camera: &mut Camera, pixels: f32) {
        if self.pixels != 0.0 {
            self.restore(camera);
        }
        self.pixels = pixels;
        if pixels == 0.0 {
            return;
        }
        let view = camera.view_transform();
        self.base_view = view;
        self.base_screen_center_x = camera.screen_center_x;
        let s = f64::from(pixels) / f64::from(camera.x_scale);
        camera.set_view_cache(shear(&view, s));
        camera.screen_center_x = (f64::from(camera.screen_center_x) + f64::from(pixels)) as f32;
    }

    /// `RestoreCamera`: the saved view and centre back, the view marked for
    /// a rebuild.
    fn restore(&self, camera: &mut Camera) {
        camera.set_view_cache(self.base_view);
        camera.screen_center_x = self.base_screen_center_x;
        camera.mark_view_dirty();
    }

    /// `WidescreenHudShearModelview`: a modelview bound without the camera,
    /// sheared like the view; `None` without a shift.
    #[must_use]
    pub fn shear_modelview(&self, camera: &Camera, modelview: &Transform) -> Option<Transform> {
        if self.pixels == 0.0 {
            return None;
        }
        let s = f64::from(self.pixels) / f64::from(camera.x_scale);
        Some(shear(modelview, s))
    }
}

/// `WidescreenHudShearModelview` for a shift of `pixels` (`G_HudShift`):
/// a modelview bound without the camera, sheared like the view; `None`
/// without a shift.
#[must_use]
pub fn shear_modelview(camera: &Camera, pixels: f32, modelview: &Transform) -> Option<Transform> {
    if pixels == 0.0 {
        return None;
    }
    Some(shear(
        modelview,
        f64::from(pixels) / f64::from(camera.x_scale),
    ))
}

/// `WidescreenHudCursorRay`: the cursor's ray through the camera as it is
/// now; unlike the input handler's normalisation a zero vector gives zero.
#[must_use]
pub fn cursor_ray(camera: &Camera, cursor_x: i32, cursor_y: i32) -> Vector {
    let world = camera.unproject(cursor_x as f32, cursor_y as f32);
    let (x, y, z) = (f64::from(world.x), f64::from(world.y), f64::from(world.z));
    let length = (x * x + y * y + z * z).sqrt();
    let inverse = if length > 0.0 { 1.0 / length } else { 0.0 };
    Vector::new(
        (x * inverse) as f32,
        (y * inverse) as f32,
        (z * inverse) as f32,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_hor_plus_factor() {
        let s = Surface::new(1920, 1080);
        assert_eq!(hor_plus_scale(&s, 1920), 1920.0 * 3.0 / (1080.0 * 4.0));
        assert_eq!(hor_plus_scale(&s, 800), 1.0);
        assert_eq!(hor_plus_scale(&Surface::new(1024, 768), 1024), 1.0);
        assert_eq!(hor_plus_scale(&Surface::new(1280, 1024), 1280), 1.0);
        assert_eq!(equivalent_surface_width(&s), 1440.0);
    }

    #[test]
    fn shifts_are_whole_pixels_of_half_the_spare_width() {
        let s = Surface::new(1920, 1080);
        assert_eq!(shift_pixels(&s, 1.0), 240.0);
        assert_eq!(shift_pixels(&s, -1.0), -240.0);
        assert_eq!(shift_pixels(&s, 0.5), 120.0);
        // 1366 x 768: the equivalent is 1024, the spare 171.
        assert_eq!(shift_pixels(&Surface::new(1366, 768), 1.0), 171.0);
        assert_eq!(shift_pixels(&Surface::new(1024, 768), 1.0), 0.0);
        let mut off = s;
        off.widescreen = false;
        assert_eq!(shift_pixels(&off, 1.0), 0.0);
    }

    #[test]
    fn a_shift_moves_projections_by_its_pixels_at_any_depth() {
        let surface = Surface::new(1920, 1080);
        let mut cam = Camera::win3d(surface);
        let mut shift = Shift::default();
        let base = cam.view_transform();
        shift.set(&mut cam, 240.0);
        assert_eq!(cam.screen_center_x, 960.0 + 240.0);
        // A point at depth d moves s * d sideways in view space.
        let sheared = cam.view_transform();
        let s = 240.0 / f64::from(cam.x_scale);
        assert_eq!(
            sheared.rotation.m[0][2],
            (f64::from(base.rotation.m[0][2]) - s) as f32
        );
        shift.set(&mut cam, 0.0);
        assert_eq!(cam.screen_center_x, 960.0);
        assert_eq!(cam.view_transform(), base);
    }
}
