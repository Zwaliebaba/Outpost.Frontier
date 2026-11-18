//! The engine side of a playing scene: the control interfaces of
//! `CppSource/include/scenescript_controls.h` that an engine object
//! attached to an entity answers.
//!
//! The reference binds each interface with `QueryInterface` when the host
//! attaches an engine object to an entity (`FSceneScriptScene::Attach`), and
//! calls whichever it holds. Here the host attaches one [`EntityControls`]
//! value; [`EntityControls::interfaces`] says which interfaces it answers,
//! and the scene calls only the methods of those, in the reference's order.
//! Methods have defaults, so a host implements the interfaces it answers.
//! Result codes are the reference's (`0` for success).

use crate::scene_script::entity::{AudioProps, LightProps};
use crate::scene_script::event::EventDesc;

/// The interfaces an engine object answers (`Iids::*`).
pub mod interface {
    /// `IGeoTransformControl`.
    pub const GEO: u32 = 0x1;
    /// `ICameraControl`.
    pub const CAMERA: u32 = 0x2;
    /// `ICompoundControl`.
    pub const COMPOUND: u32 = 0x4;
    /// `IIKControl`.
    pub const IK: u32 = 0x8;
    /// `ILightControl`.
    pub const LIGHT: u32 = 0x10;
    /// `ID3DLightControl`.
    pub const D3D_LIGHT: u32 = 0x20;
    /// `ISoundControl`.
    pub const SOUND: u32 = 0x40;
    /// `IUserEventControl`.
    pub const USER_EVENT: u32 = 0x80;
    /// `ISubtitleControl`.
    pub const SUBTITLE: u32 = 0x100;
    /// `IFogControl`.
    pub const FOG: u32 = 0x200;
    /// `IReverbControl`.
    pub const REVERB: u32 = 0x400;
    /// `IPSysControl`.
    pub const PSYS: u32 = 0x800;
    /// `IMotionPath`.
    pub const MOTION_PATH: u32 = 0x1000;
    /// `IScriptControl` (a sub-scene's script engine).
    pub const SCRIPT: u32 = 0x2000;
}

/// An event as the reference hands it to a control: the engine's id of
/// the event (`UniqueId`) and its descriptor as the event holds it.
#[derive(Clone, Copy, Debug)]
pub struct EventRef<'a> {
    pub id: i32,
    pub desc: &'a EventDesc,
}

/// The engine object attached to an entity. Each group of methods is one
/// interface; the scene calls a group only when [`Self::interfaces`] holds
/// its bit.
#[allow(unused_variables)]
pub trait EntityControls {
    /// The interfaces this object answers, as [`interface`] bits.
    fn interfaces(&self) -> u32;

    // IGeoTransformControl
    fn set_position(&mut self, position: [f32; 3]) {}
    fn set_orientation(&mut self, orientation: [f32; 9]) {}
    fn get_position(&mut self, out: &mut [f32; 3]) {}
    fn get_orientation(&mut self, out: &mut [f32; 9]) {}

    // ICameraControl
    fn set_fov(&mut self, value: f32) {}
    fn set_aspect(&mut self, value: f32) {}
    fn set_near(&mut self, value: f32) {}
    fn set_far(&mut self, value: f32) {}
    fn get_fov(&mut self, out: &mut f32) {}
    fn get_aspect(&mut self, out: &mut f32) {}
    fn get_near(&mut self, out: &mut f32) {}
    fn get_far(&mut self, out: &mut f32) {}

    // ICompoundControl
    fn start_motion(&mut self, event: EventRef<'_>) -> i32 {
        -1
    }
    fn stop_motion(&mut self, event: EventRef<'_>) {}
    fn set_floor_height(&mut self, height: f32) {}
    fn advance_time(&mut self, delta_ms: i32) {}
    fn connect_hardpoints(&mut self, event: EventRef<'_>) -> i32 {
        -1
    }
    fn disconnect_hardpoints(&mut self, event: EventRef<'_>) {}
    /// `GetHardpointXform`: the transform (rotation, then position) of a
    /// hardpoint or part; `xform` holds what the caller put there.
    fn get_hardpoint_xform(
        &mut self,
        target_type: i32,
        name: Option<&[u8]>,
        xform: &mut [f32; 12],
    ) {
    }
    fn get_floor_height(&mut self, out: &mut f32) {}

    // IIKControl
    fn start_ik(&mut self, event: EventRef<'_>, position: [f32; 3], orientation: [f32; 9]) -> i32 {
        -1
    }
    fn stop_ik(&mut self, event: EventRef<'_>) {}
    fn update_ik(&mut self, event: EventRef<'_>, position: [f32; 3], orientation: [f32; 9]) {}

    // ILightControl
    fn light_set_color(&mut self, rgb: [i32; 3]) {}
    fn light_set_direction(&mut self, direction: [f32; 3]) {}
    fn light_set_range(&mut self, range: f32) {}
    fn light_set_cutoff(&mut self, radians: f32) {}
    fn light_set_on(&mut self, on: i32) {}
    fn light_set_is_spot(&mut self, is_spot: i32) {}
    /// `GetColor`: three words, which every caller reads as integers.
    fn light_get_color(&mut self, out: &mut [i32; 3]) {}
    fn light_get_direction(&mut self, out: &mut [f32; 3]) {}
    fn light_get_range(&mut self, out: &mut f32) {}
    fn light_get_cutoff(&mut self, out: &mut f32) {}
    /// Slot 13, read into the light's `on` word.
    fn light_get_slot13(&mut self, out: &mut i32) {}
    /// Slot 14, read into the light's `infinite` word.
    fn light_get_slot14(&mut self, out: &mut i32) {}

    // ID3DLightControl
    fn d3d_set_on(&mut self, on: i32) {}
    /// `SetLight`: the `D3DLIGHT8` words, `light_type` to `cutoff`.
    fn d3d_set_light(&mut self, light: &LightProps) {}
    fn d3d_set_direction(&mut self, direction: [f32; 3]) {}
    fn d3d_get_on(&mut self, out: &mut i32) {}
    /// `GetLight`: fills `light_type` to `cutoff`.
    fn d3d_get_light(&mut self, light: &mut LightProps) {}
    fn d3d_get_direction(&mut self, out: &mut [f32; 3]) {}

    // IFogControl
    fn fog_set_on(&mut self, on: u8) {}
    fn fog_set_table_mode(&mut self, table: u8, mode: u32) {}
    fn fog_set_range(&mut self, start: f32, end: f32, density: f32) {}
    fn fog_set_color(&mut self, rgb: [i32; 3]) {}
    /// `GetPacked`: the eight fog words.
    fn fog_get_packed(&mut self, out: &mut [u32; 8]) {}

    // IReverbControl
    fn set_reverb(&mut self, environment: u32, volume: f32, decay: f32, damping: f32) {}
    /// `GetReverb`: environment (its word), volume, decay, damping.
    fn get_reverb(&mut self, out: &mut [u32; 4]) {}

    // IPSysControl
    fn psys_start(&mut self) -> i32 {
        -1
    }
    fn psys_stop(&mut self) {}
    fn psys_tick(&mut self, delta_ms: i32) {}
    fn psys_set_sparam(&mut self, value: f32) {}
    fn psys_get_sparam(&mut self, out: &mut f32) {}

    // ISoundControl
    fn start_sound(&mut self, event: EventRef<'_>, time_ms: i32) -> i32 {
        -1
    }
    /// `StopSound`, which the reference hands the common descriptor only.
    fn stop_sound(&mut self, event: EventRef<'_>) {}
    fn set_audio_props(&mut self, props: &AudioProps, mask: i32) {}
    fn get_audio_props(&mut self, props: &mut AudioProps, mask: i32) {}

    // ISubtitleControl
    fn show_subtitle(&mut self, string_id: i32) {}
    fn hide_subtitle(&mut self, string_id: i32) {}

    // IUserEventControl
    fn user_event_start(&mut self, event: EventRef<'_>) -> i32 {
        -1
    }
    fn user_event_stop(&mut self, event: EventRef<'_>) {}

    // IScriptControl
    fn script_start(&mut self, start_ms: i32) -> i32 {
        -1
    }
    fn script_tick(&mut self, delta_ms: i32) {}
    fn script_stop(&mut self) {}

    // IMotionPath
    fn path_evaluate(&mut self, t: f32, out: &mut [f32; 3]) -> i32 {
        -1
    }
    fn path_orientation(&mut self, t: f32, out: &mut [f32; 4]) -> i32 {
        -1
    }
    fn path_look_at(&mut self, t: f32, out: &mut [f32; 3]) -> i32 {
        -1
    }
}
