//! Event descriptors: what the parser's second pass hands to
//! `IScriptEngine::Slot38AddEvent` for each event of a scene script
//! (`FSceneScriptEventDesc` and the typed descriptors of
//! `CppSource/include/scenescript_iface.h`), as the parser's temporary
//! `FSceneScriptEvent` (`SceneScriptScene.cpp`) lays them out.
//!
//! As in [`super::entity`], integer words stay integers and a C string is
//! `Option<Vec<u8>>`, `None` for a null pointer.

use super::entity::{AudioProps, Fog, LightProps};

/// Event types (descriptor `+0x00`).
pub mod kind {
    pub const USER_EVENT: i32 = 1;
    pub const SET_CAMERA: i32 = 2;
    pub const START_SOUND: i32 = 3;
    pub const LIGHT_PROP_ANIM: i32 = 4;
    pub const CAMERA_PROP_ANIM: i32 = 5;
    pub const PATH_ANIM: i32 = 6;
    pub const SPATIAL_PROP_ANIM: i32 = 7;
    pub const ATTACH_ENTITY: i32 = 8;
    pub const CONNECT_HARDPOINTS: i32 = 9;
    pub const START_MOTION: i32 = 10;
    pub const START_IK: i32 = 11;
    pub const START_SUB_SCENE: i32 = 12;
    pub const START_PSYS: i32 = 13;
    pub const PSYS_PROP_ANIM: i32 = 14;
    pub const AUDIO_PROP_ANIM: i32 = 15;
    pub const FOG_PROP_ANIM: i32 = 16;
    pub const REVERB_PROP_ANIM: i32 = 17;
    pub const FLOOR_HEIGHT_ANIM: i32 = 18;
    pub const SUBTITLE: i32 = 19;
    /// `START`: becomes START_SOUND for a SOUND entity, else START_MOTION.
    pub const START: i32 = 23;
}

/// Common descriptor flags (`kSceneScriptFlag*` of `SceneScriptScene.h`).
pub mod flag {
    pub const USE_SCRIPT_DURATION: i32 = 0x1;
    pub const POSITION: i32 = 0x2;
    pub const ORIENTATION: i32 = 0x4;
    pub const LOOK_AT: i32 = 0x8;
    pub const QUAT_ORIENT: i32 = 0x10;
}

/// The mask bits of a spatial event (`kSceneScriptSpatialBit*`).
pub mod spatial_bit {
    pub const POS: i32 = 0x1;
    pub const ORIENT: i32 = 0x2;
    pub const AXIS_ROT: i32 = 0x4;
}

/// The param-curve factories of `SceneScript.dll`, in its registration
/// order.
pub const CURVE_CLASSES: [&str; 11] = [
    "CatmullRomPCurve",
    "SceneScriptParamCurve",
    "LinearPCurve",
    "SceneScriptLPCurve",
    "SmoothPCurve",
    "StepPCurve",
    "RampUpPCurve",
    "RampDownPCurve",
    "BumpOutPCurve",
    "BumpInPCurve",
    "FreeFormPCurve",
];

/// A param curve the parser created from `param_curve`
/// (`CreateCurveFromLua`): its factory and the keys in the order the
/// parser added them (`IParamCurve::AddKey`). Sorting and evaluating them
/// is left to the curve port (`ParamCurve.cpp`).
#[derive(Clone, Debug, PartialEq)]
pub struct ParamCurve {
    /// One of [`CURVE_CLASSES`].
    pub class: &'static str,
    pub keys: Vec<[f32; 4]>,
}

/// The common eleven words of an event descriptor, without the two the
/// engine ignores or replaces: `+0x10`, which the parser leaves 0, and the
/// unique id, the parser's temporary event's serial number, which the
/// engine overwrites with its own.
#[derive(Clone, Debug, PartialEq)]
pub struct EventCommon {
    pub kind: i32,
    pub start_ms: i32,
    pub flags: i32,
    pub event_flags: i32,
    pub duration_ms: i32,
    /// The entity the event drives, as an engine handle; -1 for none.
    pub primary: i32,
    pub secondary: i32,
    pub param_curve: Option<ParamCurve>,
    /// `pcurve_period` (`+0x28`); -1 when the script gives none.
    pub pcurve_period: i32,
}

/// Four reverb words (`FSceneScriptReverb4`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Reverb {
    pub environment: i32,
    pub volume: f32,
    pub decay: f32,
    pub damping: f32,
}

/// The key of a START_IK descriptor (`+0x2c`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IkKey {
    /// The address of the event that holds the descriptor, the reference's
    /// default; the engine replaces it with its own event.
    Event,
    /// `ik_id`, chopped to 32 bits.
    Id(u32),
}

/// The words after the common part, by event type.
#[derive(Clone, Debug, PartialEq)]
pub enum EventPayload {
    /// USER_EVENT. The word the descriptor carries is always 0; the text
    /// is `user_event_string`, which the parser hands to the COM context
    /// (`ISceneScriptComContext::Slot3`) after the event.
    User {
        payload_word: u32,
        text: Option<Vec<u8>>,
    },
    SetCamera,
    StartSound {
        start_time: i32,
    },
    Light {
        mask: i32,
        target: LightProps,
    },
    Camera {
        mask: i32,
        target: [f32; 4],
    },
    Path {
        start_percent: f32,
        stop_percent: f32,
        percent_delta: f32,
        offset: [f32; 3],
        up: i32,
        front: i32,
    },
    Spatial {
        mask: i32,
        orient: [f32; 9],
        position: [f32; 3],
        /// W, X, Y, Z.
        quaternion: [f32; 4],
        axis_rot_angle: f32,
        axis_rot_axis: i32,
        target_part: Option<Vec<u8>>,
        target_type: i32,
    },
    Attach {
        offset: [f32; 3],
        target_type: i32,
        up: i32,
        front: i32,
        target_part: Option<Vec<u8>>,
    },
    Connect {
        hardpoint: Option<Vec<u8>>,
        parent_hardpoint: Option<Vec<u8>>,
    },
    Motion {
        start_time: f32,
        trans_time: f32,
        time_scale: f32,
        weight: f32,
        animation: Option<Vec<u8>>,
        /// A byte the parser leaves 0 (`+0x40`).
        field_40: u8,
        heading: f32,
        locked_bone: Option<Vec<u8>>,
        trans_scale: f32,
        /// A word the parser leaves 0 (`+0x50`).
        field_50: i32,
    },
    Ik {
        key: IkKey,
        count_to_root: i32,
        up: i32,
        front: i32,
        damping: f32,
        point_at: u8,
        move_to: u8,
        transition_duration: f32,
        end_effector: Option<Vec<u8>>,
        target_part: Option<Vec<u8>>,
        target_type: i32,
    },
    SubScene {
        start_time: f32,
    },
    StartPsys,
    PsysProp {
        mask: i32,
        sparam: f32,
    },
    Audio {
        mask: i32,
        target: AudioProps,
    },
    Fog {
        mask: i32,
        target: Fog,
    },
    Reverb {
        mask: i32,
        target: Reverb,
    },
    FloorHeight {
        floor_height: f32,
        target_part: Option<Vec<u8>>,
        target_type: i32,
    },
    Subtitle {
        strid: u32,
    },
}

/// An event descriptor.
#[derive(Clone, Debug, PartialEq)]
pub struct EventDesc {
    pub common: EventCommon,
    pub payload: EventPayload,
}
