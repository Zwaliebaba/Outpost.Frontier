//! Entity descriptors: what the parser's second pass hands to
//! `IScriptEngine::Slot35AddEntity` for each entity of a scene script
//! (`FSceneScriptEntitySnap` of `CppSource/include/scenescript_iface.h`).
//!
//! The fields keep the reference's types: a word the reference stores as
//! an integer is an integer here, even where it holds a colour or an axis.
//! A C string the descriptor points to is `Option<Vec<u8>>`, `None` for a
//! null pointer, its bytes up to the first NUL otherwise.

/// Entity types (descriptor `+0x00`), the numbers the scripts' `COMPOUND`,
/// `CAMERA`, ... globals hold.
pub mod kind {
    pub const COMPOUND: i32 = 1;
    pub const DEFORMABLE: i32 = 2;
    pub const CAMERA: i32 = 3;
    pub const MONITOR: i32 = 4;
    pub const LIGHT: i32 = 5;
    pub const SOUND: i32 = 6;
    pub const MARKER: i32 = 7;
    pub const HARDPOINT: i32 = 8;
    pub const SCENE: i32 = 9;
    pub const SUB_SCENE: i32 = 10;
    pub const MOTION_PATH: i32 = 11;
    pub const PSYS: i32 = 13;
}

/// The mask bits `LoadLightProps` reports, one per key it found
/// (`kSceneScriptLightBit*` of `SceneScriptScene.h`).
pub mod light_bit {
    pub const COLOR: i32 = 0x1;
    pub const DIRECTION: i32 = 0x2;
    pub const RANGE: i32 = 0x4;
    pub const CUTOFF: i32 = 0x8;
    pub const ON: i32 = 0x10;
    pub const INFINITE: i32 = 0x20;
    pub const ATTEN: i32 = 0x40;
    pub const THETA: i32 = 0x80;
    pub const TYPE: i32 = 0x100;
    pub const SPECULAR: i32 = 0x200;
    pub const AMBIENT: i32 = 0x400;
    pub const DIFFUSE: i32 = 0x800;
}

/// The 36 words of light properties (`FSceneScriptLightProps`): the tail
/// of a LIGHT entity's descriptor and the target of a light event. Colours
/// are integers (0 to 255 in the scripts), angles radians.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LightProps {
    pub color: [i32; 3],
    pub direction: [f32; 3],
    /// `range` as the script gives it.
    pub range_temp: f32,
    /// `cutoff` in degrees, as the script gives it.
    pub cutoff_temp: f32,
    pub on: i32,
    pub infinite: i32,
    pub light_type: i32,
    pub diffuse: [f32; 4],
    pub specular: [f32; 4],
    pub ambient: [f32; 4],
    pub unused_17: [i32; 6],
    pub range: f32,
    /// A word the defaults set to the bits of 1.0; nothing reads it.
    pub unused_1e: u32,
    pub atten: [f32; 3],
    pub theta: f32,
    pub cutoff: f32,
}

impl LightProps {
    /// `SceneScriptInitLightEntityDefaults` (`SceneScriptScene.cpp`): a
    /// white point light of range 1000, with the defaults' cone angles
    /// (0.8π and π) stored as raw bits.
    #[must_use]
    pub fn entity_defaults() -> LightProps {
        LightProps {
            color: [0xff; 3],
            direction: [0.0, 0.0, 1.0],
            range_temp: 1000.0,
            cutoff_temp: 180.0,
            on: 1,
            infinite: 0,
            light_type: 2,
            diffuse: [1.0; 4],
            range: 1000.0,
            unused_1e: 1.0_f32.to_bits(),
            atten: [0.0, 0.0, 0.0001],
            theta: f32::from_bits(0x4020_d97c),
            cutoff: f32::from_bits(0x4049_0fdb),
            ..LightProps::default()
        }
    }
}

/// The eight audio words (`FSceneScriptAudio8`): the tail of a SOUND
/// entity's descriptor and the target of an audio event. The cone angles
/// are integers.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AudioProps {
    pub attenuation: f32,
    pub pan: f32,
    pub dmin: f32,
    pub dmax: f32,
    pub ain: i32,
    pub aout: i32,
    pub atout: f32,
    pub rmix: f32,
}

/// The eight fog words (`FSceneScriptFog8`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Fog {
    pub on: u8,
    pub table: u8,
    pub color: [i32; 3],
    pub mode: i32,
    pub start: f32,
    pub end: f32,
    pub density: f32,
}

/// The 15 words of a SCENE or SUB_SCENE entity's descriptor tail
/// (`LoadSceneEntityDesc`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SceneProps {
    pub ambient: [i32; 3],
    /// The script's duration, from the parser's first pass.
    pub duration_ms: u32,
    pub start_ms: i32,
    pub up: i32,
    pub front: i32,
    pub fog: Fog,
}

/// The factories of `SceneScript.dll` that make motion paths.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PathFactory {
    /// `CV_CRSplinePath`.
    CrSpline,
    /// `CV_CROrientationSplinePath`.
    CrOrientationSpline,
}

impl PathFactory {
    /// The registered name.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            PathFactory::CrSpline => "CV_CRSplinePath",
            PathFactory::CrOrientationSpline => "CV_CROrientationSplinePath",
        }
    }

    pub(super) fn from_name(name: &[u8]) -> Option<PathFactory> {
        [PathFactory::CrSpline, PathFactory::CrOrientationSpline]
            .into_iter()
            .find(|f| f.name().as_bytes() == name)
    }
}

/// How the parser filled a motion path object.
#[derive(Clone, Debug, PartialEq)]
pub enum PathInit {
    /// Created and left empty.
    Empty,
    /// `IPersistString::Load` with the script's `path_data` (`None`: a
    /// null pointer, for a value that is not a string or a number).
    Text(Option<Vec<u8>>),
    /// `ICRSpline::LoadPoints` with the script's `control_pts`.
    Points(Vec<[f32; 3]>),
}

/// A motion path object, created by the parser and owned by the
/// descriptor; [`super::spline::SplinePath::from_desc`] makes the path it
/// describes.
#[derive(Clone, Debug, PartialEq)]
pub struct PathObject {
    pub factory: PathFactory,
    pub init: PathInit,
}

/// The tail of a MOTION_PATH entity's descriptor
/// (`FSceneScriptMotionPathDesc`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MotionPathDesc {
    /// The factory name the parser asked for, as the script gives it, or
    /// `CV_CRSplinePath` when it gives none; `None` without `pathprops`.
    pub path_type: Option<Vec<u8>>,
    /// The object, `None` where the factory is unknown or the object has
    /// no `IPersistString` interface.
    pub path: Option<PathObject>,
}

/// The type-dependent part of an entity descriptor (`+0x44`).
#[derive(Clone, Debug, PartialEq)]
pub enum EntityTail {
    /// COMPOUND and DEFORMABLE: `compoundprops.floor_height`.
    FloorHeight(f32),
    /// CAMERA and MONITOR, in descriptor order.
    Camera {
        fovh: f32,
        near_plane: f32,
        far_plane: f32,
        aspect: f32,
    },
    Light(LightProps),
    Sound(AudioProps),
    /// MARKER and HARDPOINT carry no tail.
    None,
    Scene(SceneProps),
    MotionPath(MotionPathDesc),
    /// PSYS: `psysprops.sparam`.
    Psys(f32),
}

/// An entity descriptor (`FSceneScriptEntitySnap`).
#[derive(Clone, Debug, PartialEq)]
pub struct EntityDesc {
    pub kind: i32,
    /// Bit 0: a reference entity (resolved, not created).
    pub flags: i32,
    pub pos: [f32; 3],
    /// Row-major 3×3; the script's `orient` rows are its columns.
    pub orient: [f32; 9],
    pub name: Option<Vec<u8>>,
    pub template_name: Option<Vec<u8>>,
    pub lt_grp: u8,
    pub srt_grp: u8,
    pub usr_flg: u8,
    pub tail: EntityTail,
}
