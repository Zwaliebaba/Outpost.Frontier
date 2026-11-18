//! X3DAudio (`x3daudio.h`), which the `windows` crate describes only by
//! its constants: the structures, packed to one byte as the header packs
//! them, and a safe wrapper over `X3DAudioInitialize` and
//! `X3DAudioCalculate`, both exported by the XAudio2 library.

use core::ffi::c_void;

use windows::Win32::Media::Audio::XAudio2::{
    X3DAUDIO_CALCULATE_DOPPLER, X3DAUDIO_CALCULATE_MATRIX, X3DAUDIO_HANDLE_BYTESIZE,
};
use windows::core::HRESULT;

use crate::audio::spatial::{CurvePoint, EmitterInputs, ListenerInputs};

/// `X3DAudioInitialize`.
pub type InitializeFn = unsafe extern "C" fn(u32, f32, *mut u8) -> HRESULT;
/// `X3DAudioCalculate`.
pub type CalculateFn =
    unsafe extern "C" fn(*const u8, *const Listener, *const Emitter, u32, *mut DspSettings);

/// `X3DAUDIO_LISTENER`.
#[repr(C, packed(1))]
#[derive(Clone, Copy)]
pub struct Listener {
    pub orient_front: [f32; 3],
    pub orient_top: [f32; 3],
    pub position: [f32; 3],
    pub velocity: [f32; 3],
    pub cone: *const c_void,
}

/// `X3DAUDIO_DISTANCE_CURVE_POINT`.
#[repr(C, packed(1))]
#[derive(Clone, Copy)]
pub struct CurvePointRaw {
    pub distance: f32,
    pub dsp_setting: f32,
}

/// `X3DAUDIO_DISTANCE_CURVE`.
#[repr(C, packed(1))]
#[derive(Clone, Copy)]
pub struct Curve {
    pub points: *const CurvePointRaw,
    pub point_count: u32,
}

/// `X3DAUDIO_EMITTER`.
#[repr(C, packed(1))]
#[derive(Clone, Copy)]
pub struct Emitter {
    pub cone: *const c_void,
    pub orient_front: [f32; 3],
    pub orient_top: [f32; 3],
    pub position: [f32; 3],
    pub velocity: [f32; 3],
    pub inner_radius: f32,
    pub inner_radius_angle: f32,
    pub channel_count: u32,
    pub channel_radius: f32,
    pub channel_azimuths: *const f32,
    pub volume_curve: *const Curve,
    pub lfe_curve: *const Curve,
    pub lpf_direct_curve: *const Curve,
    pub lpf_reverb_curve: *const Curve,
    pub reverb_curve: *const Curve,
    pub curve_distance_scaler: f32,
    pub doppler_scaler: f32,
}

/// `X3DAUDIO_DSP_SETTINGS`.
#[repr(C, packed(1))]
#[derive(Clone, Copy)]
pub struct DspSettings {
    pub matrix_coefficients: *mut f32,
    pub delay_times: *mut f32,
    pub src_channel_count: u32,
    pub dst_channel_count: u32,
    pub lpf_direct_coefficient: f32,
    pub lpf_reverb_coefficient: f32,
    pub reverb_level: f32,
    pub doppler_factor: f32,
    pub emitter_to_listener_angle: f32,
    pub emitter_to_listener_distance: f32,
    pub emitter_velocity_component: f32,
    pub listener_velocity_component: f32,
}

/// What [`X3dAudio::calculate`] gives a voice.
#[derive(Clone, Debug, PartialEq)]
pub struct Calculated {
    /// The output matrix, `destination * sources + source`.
    pub matrix: Vec<f32>,
    /// The Doppler factor for the frequency ratio.
    pub doppler: f32,
}

/// An initialised X3DAudio instance (`X3DAUDIO_HANDLE`).
#[derive(Clone, Copy)]
pub struct X3dAudio {
    handle: [u8; X3DAUDIO_HANDLE_BYTESIZE as usize],
    calculate: CalculateFn,
    output_channels: u32,
}

impl core::fmt::Debug for X3dAudio {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("X3dAudio")
            .field("output_channels", &self.output_channels)
            .finish_non_exhaustive()
    }
}

impl X3dAudio {
    /// `X3DAudioInitialize` for a mastering voice of `channel_mask` with
    /// `output_channels` channels, the speed of sound in metres per second
    /// (velocities are converted to it; see `audio::spatial`).
    pub fn new(
        initialize: InitializeFn,
        calculate: CalculateFn,
        channel_mask: u32,
        output_channels: u32,
        speed_of_sound: f32,
    ) -> Option<X3dAudio> {
        let mut handle = [0u8; X3DAUDIO_HANDLE_BYTESIZE as usize];
        // SAFETY: the handle buffer has the X3DAUDIO_HANDLE_BYTESIZE bytes the
        // function writes; the pointer comes from the library's export table
        // with the signature x3daudio.h declares.
        let hr = unsafe { initialize(channel_mask, speed_of_sound, handle.as_mut_ptr()) };
        if hr.is_err() || output_channels == 0 {
            return None;
        }
        Some(X3dAudio {
            handle,
            calculate,
            output_channels,
        })
    }

    /// `X3DAudioCalculate` with `X3DAUDIO_CALCULATE_MATRIX |
    /// X3DAUDIO_CALCULATE_DOPPLER` for an omnidirectional emitter of
    /// `channels` channels at one point, with `curve` scaled by `scaler`.
    pub fn calculate(
        &self,
        listener: &ListenerInputs,
        emitter: &EmitterInputs,
        channels: u32,
        scaler: f32,
        curve: &[CurvePoint],
    ) -> Calculated {
        let channels = channels.max(1);
        let points: Vec<CurvePointRaw> = curve
            .iter()
            .map(|p| CurvePointRaw {
                distance: p.distance,
                dsp_setting: p.gain,
            })
            .collect();
        let volume_curve = Curve {
            points: points.as_ptr(),
            point_count: u32::try_from(points.len()).unwrap_or(0),
        };
        let azimuths = vec![0.0f32; channels as usize];
        let raw_listener = Listener {
            orient_front: listener.frame.front,
            orient_top: listener.frame.top,
            position: listener.position,
            velocity: listener.velocity,
            cone: core::ptr::null(),
        };
        let raw_emitter = Emitter {
            cone: core::ptr::null(),
            orient_front: [0.0, 0.0, 1.0],
            orient_top: [0.0, 1.0, 0.0],
            position: emitter.position,
            velocity: emitter.velocity,
            inner_radius: 0.0,
            inner_radius_angle: 0.0,
            channel_count: channels,
            channel_radius: 0.0,
            channel_azimuths: azimuths.as_ptr(),
            volume_curve: if points.len() >= 2 {
                &raw const volume_curve
            } else {
                core::ptr::null()
            },
            lfe_curve: core::ptr::null(),
            lpf_direct_curve: core::ptr::null(),
            lpf_reverb_curve: core::ptr::null(),
            reverb_curve: core::ptr::null(),
            curve_distance_scaler: if scaler.is_finite() && scaler > 0.0 {
                scaler
            } else {
                1.0
            },
            doppler_scaler: emitter.doppler_scaler,
        };
        let mut matrix = vec![0.0f32; (channels * self.output_channels) as usize];
        let mut settings = DspSettings {
            matrix_coefficients: matrix.as_mut_ptr(),
            delay_times: core::ptr::null_mut(),
            src_channel_count: channels,
            dst_channel_count: self.output_channels,
            lpf_direct_coefficient: 0.0,
            lpf_reverb_coefficient: 0.0,
            reverb_level: 0.0,
            doppler_factor: 1.0,
            emitter_to_listener_angle: 0.0,
            emitter_to_listener_distance: 0.0,
            emitter_velocity_component: 0.0,
            listener_velocity_component: 0.0,
        };
        // SAFETY: every pointer in the listener, the emitter and the settings
        // points at a local that outlives the call: the curve points, the
        // curve, the azimuth table of `channels` entries and the matrix of
        // `channels * output_channels` entries, the sizes the settings state.
        // The handle was initialised by X3DAudioInitialize.
        unsafe {
            (self.calculate)(
                self.handle.as_ptr(),
                &raw const raw_listener,
                &raw const raw_emitter,
                X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_DOPPLER,
                &raw mut settings,
            );
        }
        let doppler = settings.doppler_factor;
        Calculated { matrix, doppler }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layouts_are_packed_as_the_header_declares() {
        let ptr = core::mem::size_of::<*const c_void>();
        assert_eq!(core::mem::size_of::<Listener>(), 48 + ptr);
        assert_eq!(core::mem::size_of::<CurvePointRaw>(), 8);
        assert_eq!(core::mem::size_of::<Curve>(), ptr + 4);
        assert_eq!(core::mem::size_of::<Emitter>(), 7 * ptr + 48 + 24);
        assert_eq!(core::mem::size_of::<DspSettings>(), 2 * ptr + 40);
    }
}
