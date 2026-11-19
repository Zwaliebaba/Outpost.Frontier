//! The engine on Windows: the Direct3D 12 backend for a window (or
//! offscreen), on the first hardware adapter and on WARP when there is
//! none, as `RenderMgrCreateBuffers` creates the device for the window.

use ::windows::Win32::Foundation::HWND;

use crate::platform::d3d12::backend::D3d12Backend;
use crate::platform::d3d12::{AdapterChoice, GpuError};

use super::{Engine, EngineConfig, EngineError};

/// The engine over Direct3D 12.
pub type D3d12Engine = Engine<D3d12Backend>;

/// Why the Windows engine could not be built.
#[derive(Debug)]
pub enum WindowsEngineError {
    /// The device or the swap chain.
    Gpu(GpuError),
    /// The engine itself.
    Engine(EngineError),
}

impl std::fmt::Display for WindowsEngineError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Gpu(e) => write!(f, "the renderer: {e}"),
            Self::Engine(e) => e.fmt(f),
        }
    }
}

impl std::error::Error for WindowsEngineError {}

/// The backend for a back buffer of `width` by `height`: `adapter`, or the
/// first hardware adapter falling back to WARP when `adapter` is `None`;
/// presenting into `window` when one is given, else offscreen.
///
/// # Errors
///
/// No adapter gives a device, or the swap chain cannot be made.
pub fn create_backend(
    adapter: Option<AdapterChoice>,
    window: Option<HWND>,
    width: u32,
    height: u32,
) -> Result<D3d12Backend, GpuError> {
    let mut backend = match adapter {
        Some(choice) => D3d12Backend::new(choice, width, height)?,
        None => D3d12Backend::new(AdapterChoice::Hardware, width, height)
            .or_else(|_| D3d12Backend::new(AdapterChoice::Warp, width, height))?,
    };
    if let Some(window) = window {
        backend.attach_window(window)?;
    }
    Ok(backend)
}

/// The engine over Direct3D 12 for `config`'s display mode, presenting
/// into `window` (offscreen when `None`).
///
/// # Errors
///
/// The backend or the engine could not be built.
pub fn create_engine(
    config: EngineConfig,
    adapter: Option<AdapterChoice>,
    window: Option<HWND>,
) -> Result<D3d12Engine, WindowsEngineError> {
    let (width, height) = config.mode.back_buffer();
    let backend =
        create_backend(adapter, window, width, height).map_err(WindowsEngineError::Gpu)?;
    let mut engine = Engine::new(config, backend).map_err(WindowsEngineError::Engine)?;
    // The window's IME for the boxes the interface registers.
    if let Some(hwnd) = window {
        engine.set_ime_context(Box::new(crate::platform::ime::WindowIme::new(hwnd)));
    }
    Ok(engine)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::appearance::Camera;
    use crate::window::DisplayMode;

    struct Fixed;

    impl Camera for Fixed {
        fn position(&mut self) -> [f32; 3] {
            [0.0; 3]
        }
        fn orientation(&mut self) -> [f32; 9] {
            [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
        }
        fn viewport(&mut self) -> [i32; 4] {
            [0, 0, 63, 47]
        }
        fn half_fov_x_deg(&mut self) -> f32 {
            35.0
        }
        fn fov_y_deg(&mut self) -> f32 {
            50.0
        }
        fn z_near(&mut self) -> f32 {
            1.0
        }
        fn z_far(&mut self) -> f32 {
            1000.0
        }
        fn aspect(&mut self) -> f32 {
            4.0 / 3.0
        }
        fn x_scale_over_near(&mut self) -> f64 {
            1.0
        }
        fn y_scale_over_near(&mut self) -> f64 {
            1.0
        }
        fn project(&mut self, _: [f32; 3], _: &mut [f32; 3]) -> i32 {
            0
        }
        fn unproject(&mut self, _: &mut [f32; 3], _: f32, _: f32) {}
        fn classify_sphere(&mut self, _: [f32; 3], _: f32) -> i32 {
            4
        }
        fn view_transform(&mut self) -> [f32; 12] {
            [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
        }
    }

    /// The engine on WARP, offscreen: built, one frame run (the clear to
    /// black is read back), torn down in the reference's order.
    #[test]
    fn engine_runs_a_frame_on_warp() {
        let dir = std::env::temp_dir().join("outpost-engine-warp");
        std::fs::create_dir_all(&dir).unwrap();
        let mode = DisplayMode {
            width: 64,
            height: 48,
            fullscreen: false,
        };
        let engine = create_engine(
            EngineConfig::new(&dir, mode),
            Some(AdapterChoice::Warp),
            None,
        );
        let mut engine = match engine {
            Ok(e) => e,
            Err(WindowsEngineError::Gpu(e)) => {
                println!("no WARP device ({e}): skipped");
                return;
            }
            Err(e) => panic!("{e}"),
        };
        let lifecycle = engine.lifecycle();
        engine.frame(super::super::FrameInput::idle(1.0 / 60.0), &mut Fixed);
        let pixels = engine.device().backend.read_back().expect("read back");
        assert_eq!(pixels.len(), 64 * 48 * 4);
        assert!(
            pixels
                .as_chunks::<4>()
                .0
                .iter()
                .all(|p| *p == [0, 0, 0, 0xFF]),
            "the frame clears to opaque black"
        );
        assert!(engine.device().backend.errors.is_empty());
        drop(engine);
        assert_eq!(
            lifecycle.stages(super::super::Phase::Teardown),
            super::super::TEARDOWN_ORDER.to_vec()
        );
    }
}
