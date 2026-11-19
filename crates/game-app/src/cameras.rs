//! The scene camera the client's frame draws with until the game's
//! cameras exist (the space, room and script cameras of P5.4 and P5.9).

use neuron_client::render::appearance::Camera;

/// A camera at the origin looking down -z with the Win3D camera's planes
/// and a 4:3 field of view, over the whole back buffer. Every sphere is
/// classified as visible.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StillCamera {
    width: u32,
    height: u32,
}

impl StillCamera {
    /// The camera for a back buffer of `(width, height)`.
    #[must_use]
    pub fn new((width, height): (u32, u32)) -> StillCamera {
        StillCamera { width, height }
    }
}

impl Camera for StillCamera {
    fn position(&mut self) -> [f32; 3] {
        [0.0; 3]
    }
    fn orientation(&mut self) -> [f32; 9] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    }
    fn viewport(&mut self) -> [i32; 4] {
        let w = i32::try_from(self.width).unwrap_or(i32::MAX);
        let h = i32::try_from(self.height).unwrap_or(i32::MAX);
        [0, 0, w - 1, h - 1]
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        35.0
    }
    fn fov_y_deg(&mut self) -> f32 {
        50.0
    }
    fn z_near(&mut self) -> f32 {
        0.1
    }
    fn z_far(&mut self) -> f32 {
        1000.0
    }
    fn aspect(&mut self) -> f32 {
        if self.height == 0 {
            1.0
        } else {
            (f64::from(self.width) / f64::from(self.height)) as f32
        }
    }
    fn x_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn y_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn project(&mut self, _world: [f32; 3], _out: &mut [f32; 3]) -> i32 {
        0
    }
    fn unproject(&mut self, _out: &mut [f32; 3], _x: f32, _y: f32) {}
    fn classify_sphere(&mut self, _centre: [f32; 3], _radius: f32) -> i32 {
        4
    }
    fn view_transform(&mut self) -> [f32; 12] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_viewport_covers_the_back_buffer() {
        let mut camera = StillCamera::new((1024, 768));
        assert_eq!(camera.viewport(), [0, 0, 1023, 767]);
        assert!((camera.aspect() - 4.0 / 3.0).abs() < 1e-6);
        assert_eq!(camera.classify_sphere([0.0; 3], 1.0), 4);
    }
}
