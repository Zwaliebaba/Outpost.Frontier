//! `InitializeEngine`'s render mode (`Client/engine_init.cpp`; work
//! package P4.1): the local mode block `PerfOptions.ini`'s `[Display]`
//! fills, the display switches over it, and `SelectInitialRenderMode`'s
//! choice of the mode the device takes, which `SavePerfOptions` then writes
//! back.
//!
//! The block starts with no size, fullscreen and the adapter's formats.
//! With a size from the file (both sides not 0) the mode is taken as it
//! is; without one the first mode tried is 1024 by 768 at 32 bits on an
//! adapter with more than 16 MiB of video memory and hardware transform
//! and lighting (else 800 by 600 at 16, or at 32 with the memory alone),
//! and in fullscreen at 32 bits the desktop's own size when it is at
//! least 800 by 600 and at least as wide as 4:3 (the reference build's
//! widescreen support). When the device cannot make a mode, 800 by 600 at
//! that depth and then at 16 bits follow. A windowed mode larger than the
//! desktop takes the desktop's width and three quarters of it as its
//! height (`RenderMgrCreateBuffers`).
//!
//! The port's device is a Direct3D 12 swap chain: it makes any size up to
//! [`MAX_SIDE`] and always renders 32-bit colour on a 24-bit depth buffer,
//! so those are the formats the mode records (divergence D532).

use crate::perf_options::PerfDisplay;
use crate::startup_config::DisplayMode;

/// The largest side of a back buffer the port's device makes (the
/// Direct3D 12 texture limit).
pub const MAX_SIDE: u32 = 16_384;

/// The colour depth the port's device renders, in bits.
pub const COLOR_BPP: i32 = 32;

/// The depth buffer's depth bits.
pub const DEPTH_BPP: i32 = 24;

/// `InitializeEngine`'s local mode block before `PerfOptions.ini`: no
/// size, the adapter's formats, fullscreen with a border.
#[must_use]
pub fn initial_mode() -> DisplayMode {
    DisplayMode {
        width: 0,
        height: 0,
        fullscreen: true,
        border: true,
        color_bpp: -1,
        depth_bpp: -1,
        stencil_bpp: 0,
        interval: 1,
        buffer_count: 2,
        refresh_rate: 0x3c,
    }
}

/// The block's part `LoadPerfOptions` reads and writes
/// (`FPerfDisplaySettings`, the mode's width, height and depths).
#[must_use]
pub fn perf_display(mode: &DisplayMode) -> PerfDisplay {
    PerfDisplay {
        width: mode.width.cast_signed(),
        height: mode.height.cast_signed(),
        color_bpp: mode.color_bpp,
        depth_bpp: mode.depth_bpp,
    }
}

/// Puts the `[Display]` values into the block (the C++ reads them into
/// its fields in place).
pub fn set_perf_display(mode: &mut DisplayMode, display: &PerfDisplay) {
    mode.width = display.width.cast_unsigned();
    mode.height = display.height.cast_unsigned();
    mode.color_bpp = display.color_bpp;
    mode.depth_bpp = display.depth_bpp;
}

/// What `SelectInitialRenderMode` asks of the adapter and the system.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AdapterFacts {
    /// `QueryDeviceAbility(3)`: the video memory, in MiB.
    pub video_memory_mib: u32,
    /// `QueryDeviceAbility(4)`: hardware transform and lighting.
    pub hardware_tnl: bool,
    /// `WidescreenGetDesktopSize`: the desktop's registry mode.
    pub desktop: Option<(u32, u32)>,
}

impl AdapterFacts {
    /// The port's Direct3D 12 device on a desktop of `desktop`: an adapter
    /// that passes both of the C++'s tests (D532).
    #[must_use]
    pub fn direct3d12(desktop: Option<(u32, u32)>) -> AdapterFacts {
        AdapterFacts {
            video_memory_mib: u32::MAX,
            hardware_tnl: true,
            desktop,
        }
    }
}

/// `RenderMgrCreateBuffers` on the port's device: the mode the device
/// takes (`g_display`), or `None` when it cannot make it.
#[must_use]
pub fn create_buffers(mode: &DisplayMode, desktop: Option<(u32, u32)>) -> Option<DisplayMode> {
    if mode.width == 0 || mode.height == 0 {
        return None;
    }
    let mut taken = *mode;
    if !taken.fullscreen
        && let Some((right, bottom)) = desktop
        && (right < taken.width || bottom < taken.height)
    {
        taken.width = right;
        taken.height = (right * 3) >> 2;
    }
    if taken.width > MAX_SIDE || taken.height > MAX_SIDE || taken.height == 0 {
        return None;
    }
    taken.color_bpp = COLOR_BPP;
    taken.depth_bpp = DEPTH_BPP;
    Some(taken)
}

/// `SelectInitialRenderMode(mode, mode.fullscreen)`: the first mode of the
/// ladder the device makes; `None` when none is (the C++ then shows its
/// fatal "no display mode" error, text ID `0xc9`).
#[must_use]
pub fn select_initial(mode: &DisplayMode, adapter: &AdapterFacts) -> Option<DisplayMode> {
    let mut local = DisplayMode {
        border: true,
        stencil_bpp: 0,
        interval: 1,
        buffer_count: 2,
        refresh_rate: 0x3c,
        ..*mode
    };
    let (width, height, format) = if mode.width != 0 && mode.height != 0 {
        local = *mode;
        (mode.width, mode.height, mode.color_bpp)
    } else {
        let mut first = (0x320, 0x258, 0x10);
        if adapter.video_memory_mib >= 0x11 {
            first.2 = 0x20;
            if adapter.hardware_tnl {
                first = (0x400, 0x300, 0x20);
            }
        }
        if mode.fullscreen
            && first.2 == 0x20
            && let Some((w, h)) = adapter.desktop
            && w >= 0x320
            && h >= 0x258
            && u64::from(w) * 3 >= u64::from(h) * 4
        {
            first = (w, h, 0x20);
        }
        first
    };
    local.depth_bpp = -1;
    for (w, h, color) in [
        (width, height, format),
        (0x320, 0x258, format),
        (0x320, 0x258, 0x10),
    ] {
        local.width = w;
        local.height = h;
        local.color_bpp = color;
        if let Some(taken) = create_buffers(&local, adapter.desktop) {
            return Some(taken);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::startup_config::apply_display_overrides;

    #[test]
    fn without_a_saved_size_the_adapter_and_desktop_decide() {
        let modern = AdapterFacts::direct3d12(Some((2560, 1440)));
        let m = select_initial(&initial_mode(), &modern).unwrap();
        assert_eq!((m.width, m.height, m.fullscreen), (2560, 1440, true));
        assert_eq!((m.color_bpp, m.depth_bpp), (32, 24));
        // Narrower than 4:3, or windowed: 1024 by 768.
        let tall = AdapterFacts::direct3d12(Some((1080, 1920)));
        assert_eq!(select_initial(&initial_mode(), &tall).unwrap().width, 1024);
        let mut windowed = initial_mode();
        apply_display_overrides(&mut windowed, &[b"+W"]);
        let m = select_initial(&windowed, &modern).unwrap();
        assert_eq!((m.width, m.height, m.fullscreen), (1024, 768, false));
        // An old adapter: 800 by 600, at 16 bits without the memory.
        let old = AdapterFacts {
            video_memory_mib: 16,
            hardware_tnl: true,
            desktop: None,
        };
        let m = select_initial(&initial_mode(), &old).unwrap();
        assert_eq!((m.width, m.height), (800, 600));
    }

    #[test]
    fn a_saved_size_is_taken_and_a_bad_one_falls_back() {
        let modern = AdapterFacts::direct3d12(Some((1920, 1080)));
        let mut mode = initial_mode();
        set_perf_display(
            &mut mode,
            &PerfDisplay {
                width: 1280,
                height: 960,
                color_bpp: 32,
                depth_bpp: 24,
            },
        );
        assert_eq!(perf_display(&mode).width, 1280);
        let m = select_initial(&mode, &modern).unwrap();
        assert_eq!((m.width, m.height, m.fullscreen), (1280, 960, true));
        // A negative width is a huge unsigned one: the device refuses it.
        set_perf_display(
            &mut mode,
            &PerfDisplay {
                width: -5,
                height: 600,
                color_bpp: 16,
                depth_bpp: 16,
            },
        );
        let m = select_initial(&mode, &modern).unwrap();
        assert_eq!((m.width, m.height), (800, 600));
        // A windowed mode larger than the desktop takes its width and
        // three quarters of it.
        mode.width = 2560;
        mode.height = 1600;
        mode.fullscreen = false;
        let m = select_initial(&mode, &modern).unwrap();
        assert_eq!((m.width, m.height), (1920, 1440));
    }
}
