//! Joints: the constraint types of a compound's `Cons` node, the 23-dword
//! payload the engine keeps per joint (`G_EngineJointPayloadDwords`), and
//! the local transform each type derives from its payload and its joint
//! state (`ApplyJointToLocal` and the `Apply*Joint` helpers of
//! `SceneRuntime.cpp`).
//!
//! Payload layout (dword index): 0 the type, 1..=3 the joint origin in the
//! parent, 4..=6 the offset (the translation of a fixed joint), 7..=15 the
//! rotation frame (row-major), 16..=18 the axis, 19 and 20 the minimum and
//! maximum. A sphere joint keeps its three limit pairs at 16, 17 and
//! 19..=22.

use crate::hash::eq_ignore_case;
use crate::math::{
    Matrix, Quaternion, Transform, Vector, mul_matrix, quaternion_to_matrix, transform_matrix,
};

/// Dwords in a joint payload.
pub const PAYLOAD_DWORDS: usize = 0x17;

/// Bytes before a constraint record's data: the parent and child names,
/// 0x40 bytes each.
pub const RECORD_HEADER_BYTES: usize = 0x80;
/// Bytes of one name in a constraint record.
pub const RECORD_NAME_BYTES: usize = 0x40;

/// A joint type, as `Relative[0]` holds it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum JointKind {
    /// 0, `Fix`: a rigid attachment.
    Fix,
    /// 1, `Pris`: a slide along the axis.
    Pris,
    /// 2, `Rev`: a rotation about the axis.
    Rev,
    /// 3, `Cyl`: a slide along and a rotation about the axis.
    Cyl,
    /// 4, `Sphere`: a free rotation.
    Sphere,
    /// 5, `Trans`: a free translation.
    Trans,
    /// 6, `Loose`: a free translation and rotation.
    Loose,
}

impl JointKind {
    /// Every type in code order.
    pub const ALL: [Self; 7] = [
        Self::Fix,
        Self::Pris,
        Self::Rev,
        Self::Cyl,
        Self::Sphere,
        Self::Trans,
        Self::Loose,
    ];

    /// The code the payload stores.
    #[must_use]
    pub const fn code(self) -> u32 {
        match self {
            Self::Fix => 0,
            Self::Pris => 1,
            Self::Rev => 2,
            Self::Cyl => 3,
            Self::Sphere => 4,
            Self::Trans => 5,
            Self::Loose => 6,
        }
    }

    /// The type a code names.
    #[must_use]
    pub const fn from_code(code: u32) -> Option<Self> {
        match code {
            0 => Some(Self::Fix),
            1 => Some(Self::Pris),
            2 => Some(Self::Rev),
            3 => Some(Self::Cyl),
            4 => Some(Self::Sphere),
            5 => Some(Self::Trans),
            6 => Some(Self::Loose),
            _ => None,
        }
    }

    /// The `Cons` file name of the type.
    #[must_use]
    pub const fn name(self) -> &'static [u8] {
        match self {
            Self::Fix => b"Fix",
            Self::Pris => b"Pris",
            Self::Rev => b"Rev",
            Self::Cyl => b"Cyl",
            Self::Sphere => b"Sphere",
            Self::Trans => b"Trans",
            Self::Loose => b"Loose",
        }
    }

    /// `JointTypeFromName`: the type a `Cons` file name names, compared
    /// case-insensitively.
    #[must_use]
    pub fn from_name(name: &[u8]) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|k| eq_ignore_case(name, k.name()))
    }

    /// `JointUtfRecordSize`: bytes per record in the `Cons` file.
    #[must_use]
    pub const fn record_size(self) -> usize {
        match self {
            Self::Fix | Self::Trans | Self::Loose => 0xB0,
            Self::Pris | Self::Rev => 0xD0,
            Self::Cyl => 0xD8,
            Self::Sphere => 0xD4,
        }
    }

    /// The joint parameters `SetJointParams(.., 0, ..)` takes and the mixer
    /// writes (`MixerJointValueCount`): 1 for a slide or a hinge, 2 for a
    /// cylinder, 4 for a sphere (a quaternion), 3 for a translation, 7 for
    /// a loose joint; 0 for a fixed one.
    #[must_use]
    pub const fn value_count(self) -> usize {
        match self {
            Self::Fix => 0,
            Self::Pris | Self::Rev => 1,
            Self::Cyl => 2,
            Self::Sphere => 4,
            Self::Trans => 3,
            Self::Loose => 7,
        }
    }
}

/// `MixerJointValueCount` for a raw type code (0 for anything else).
#[must_use]
pub const fn value_count_for_code(code: u32) -> usize {
    match JointKind::from_code(code) {
        Some(kind) => kind.value_count(),
        None => 0,
    }
}

/// The 23 dwords of a joint, copied as bits.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct JointPayload(pub [u32; PAYLOAD_DWORDS]);

impl Default for JointPayload {
    /// The payload of an instance with no joint: type -1, zeros.
    fn default() -> Self {
        let mut words = [0; PAYLOAD_DWORDS];
        words[0] = 0xFFFF_FFFF;
        Self(words)
    }
}

impl JointPayload {
    /// A zeroed payload of type `code`.
    #[must_use]
    pub const fn zeroed(code: u32) -> Self {
        let mut words = [0; PAYLOAD_DWORDS];
        words[0] = code;
        Self(words)
    }

    /// `RemapJointPayload`: a zeroed payload of type `kind` filled from the
    /// record data after the names (`data` holds `record_size - 0x80`
    /// bytes).
    #[must_use]
    pub fn from_record(kind: JointKind, data: &[u8]) -> Self {
        let source = |i: usize| {
            data.get(i * 4..i * 4 + 4)
                .map_or(0, |b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        };
        let mut payload = Self::zeroed(kind.code());
        let w = &mut payload.0;
        match kind {
            JointKind::Fix | JointKind::Trans | JointKind::Loose => {
                (0..12).for_each(|i| w[4 + i] = source(i));
            }
            JointKind::Pris | JointKind::Rev => (0..20).for_each(|i| w[1 + i] = source(i)),
            JointKind::Cyl => (0..22).for_each(|i| w[1 + i] = source(i)),
            JointKind::Sphere => {
                (0..15).for_each(|i| w[1 + i] = source(i));
                w[16] = source(19);
                w[17] = source(20);
                (0..4).for_each(|i| w[19 + i] = source(15 + i));
            }
        }
        payload
    }

    /// Dword 0.
    #[must_use]
    pub const fn code(&self) -> u32 {
        self.0[0]
    }

    /// The type, when dword 0 names one.
    #[must_use]
    pub const fn kind(&self) -> Option<JointKind> {
        JointKind::from_code(self.0[0])
    }

    /// Dword `index` as a `float`.
    #[must_use]
    pub fn f(&self, index: usize) -> f32 {
        f32::from_bits(self.0[index])
    }

    fn vector(&self, at: usize) -> Vector {
        Vector::new(self.f(at), self.f(at + 1), self.f(at + 2))
    }

    /// Dwords 1..=3.
    #[must_use]
    pub fn origin(&self) -> Vector {
        self.vector(1)
    }

    /// Dwords 4..=6.
    #[must_use]
    pub fn offset(&self) -> Vector {
        self.vector(4)
    }

    /// Dwords 7..=15.
    #[must_use]
    pub fn rotation(&self) -> Matrix {
        Matrix::from_words(core::array::from_fn(|i| self.f(7 + i)))
    }

    /// Dwords 16..=18.
    #[must_use]
    pub fn axis(&self) -> Vector {
        self.vector(16)
    }

    /// Dword 19 (`Relative[0x13]`).
    #[must_use]
    pub fn min(&self) -> f32 {
        self.f(19)
    }

    /// Dword 20 (`Relative[0x14]`).
    #[must_use]
    pub fn max(&self) -> f32 {
        self.f(20)
    }
}

fn add(a: Vector, b: Vector) -> Vector {
    Vector::new(
        (f64::from(a.x) + f64::from(b.x)) as f32,
        (f64::from(a.y) + f64::from(b.y)) as f32,
        (f64::from(a.z) + f64::from(b.z)) as f32,
    )
}

fn sub(a: Vector, b: Vector) -> Vector {
    Vector::new(
        (f64::from(a.x) - f64::from(b.x)) as f32,
        (f64::from(a.y) - f64::from(b.y)) as f32,
        (f64::from(a.z) - f64::from(b.z)) as f32,
    )
}

/// `AxisAngleToQuat`: the rotation by `angle` about `axis` (not
/// normalised: the sine is divided by its length). Tier B: it calls `sin`
/// and `cos`.
#[must_use]
pub fn axis_angle_to_quat(axis: Vector, angle: f32) -> Quaternion {
    let half = f64::from(angle) * 0.5;
    let sine = half.sin();
    let (x, y, z) = (f64::from(axis.x), f64::from(axis.y), f64::from(axis.z));
    let length = (x * x + y * y + z * z).sqrt();
    let k = f64::from((sine / length) as f32);
    Quaternion::new(
        half.cos() as f32,
        (k * x) as f32,
        (k * y) as f32,
        (k * z) as f32,
    )
}

/// `SlideTranslation`: the origin plus `slide` along the axis, minus `v`,
/// with the reference's rounding of the y and z terms.
fn slide_translation(rel: &JointPayload, slide: f32, v: Vector) -> Vector {
    let s = f64::from(slide);
    let ax = f64::from(rel.f(16)) * s;
    let ty = (s * f64::from(rel.f(17))) as f32;
    let tz = (s * f64::from(rel.f(18))) as f32;
    let pz = (f64::from(tz) + f64::from(rel.f(3))) as f32;
    Vector::new(
        ((ax + f64::from(rel.f(1))) - f64::from(v.x)) as f32,
        ((f64::from(ty) + f64::from(rel.f(2))) - f64::from(v.y)) as f32,
        (f64::from(pz) - f64::from(v.z)) as f32,
    )
}

fn quat_at(state: &[f32; 7], at: usize) -> Quaternion {
    Quaternion::new(state[at], state[at + 1], state[at + 2], state[at + 3])
}

/// The local transform of a joint of payload `rel` and state `state`
/// (`ApplyJointToLocal` after its dirty test): `None` for a type code the
/// reference does not apply (it leaves the local transform as it was).
/// `local` is the transform before; only a fixed joint and the cases
/// below replace all of it.
#[must_use]
pub fn joint_local(rel: &JointPayload, state: &[f32; 7]) -> Option<Transform> {
    let rest_translation = |m: &Matrix| sub(rel.origin(), transform_matrix(m, rel.offset()));
    let state_vec = Vector::new(state[0], state[1], state[2]);
    Some(match rel.kind()? {
        JointKind::Fix => Transform::new(rel.rotation(), rel.offset()),
        JointKind::Trans => Transform::new(rel.rotation(), add(state_vec, rel.offset())),
        JointKind::Pris => {
            let m = rel.rotation();
            let rotated = transform_matrix(&m, rel.offset());
            Transform::new(m, slide_translation(rel, state[0], rotated))
        }
        JointKind::Rev => {
            let rot = quaternion_to_matrix(&axis_angle_to_quat(rel.axis(), state[0]));
            let m = mul_matrix(&rot, &rel.rotation());
            Transform::new(m, rest_translation(&m))
        }
        JointKind::Cyl => {
            let rot = quaternion_to_matrix(&axis_angle_to_quat(rel.axis(), state[1]));
            let m = mul_matrix(&rel.rotation(), &rot);
            let rotated = transform_matrix(&m, rel.offset());
            Transform::new(m, slide_translation(rel, state[0], rotated))
        }
        JointKind::Sphere => {
            let m = mul_matrix(&quaternion_to_matrix(&quat_at(state, 3)), &rel.rotation());
            Transform::new(m, rest_translation(&m))
        }
        JointKind::Loose => {
            let m = mul_matrix(&quaternion_to_matrix(&quat_at(state, 3)), &rel.rotation());
            Transform::new(m, add(state_vec, rel.offset()))
        }
    })
}
