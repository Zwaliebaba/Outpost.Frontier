//! The fields of a parsed record as the oracle's mission dump writes them
//! (`CppSource/Content/oracle_content_dump.cpp`): each record lists its
//! fields in the order of the C++ dumper, so the port and the reference
//! render the same line from the same values.
//!
//! [`record!`] defines a record with its C++ constructor's values as
//! [`Default`] and its [`fields`](BaseEnter) in dump order. The formats
//! (decision record 0008, Verification; `design/sim-records.md`):
//!
//! - unsigned and signed values in decimal, flags as 0 or 1;
//! - a float as the eight lower-case hex digits of its bit pattern;
//! - a string as the hex of its bytes;
//! - vectors, matrices (row by row), transforms (rotation, then
//!   translation) and quaternions (W, X, Y, Z) as their floats joined by
//!   `,`;
//! - a costume as `head,body,left,right/count/accessories`;
//! - lists joined by `,`, a pair as `a:b`, a loadout line as
//!   `id:archetype:count:health:equipped:mission:hardpoint`, an NPC entry as
//!   `label:system`.

use std::fmt::Write as _;

use game_sim::data::costume::Costume;
use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::string_cache::StringCache;
use neuron_core::math::{Matrix, Quaternion, Transform, Vector};

use super::condition::NpcSystemEntry;

#[cfg(doc)]
use super::condition::BaseEnter;

/// One field's value, for the dump.
#[derive(Clone, Debug, PartialEq)]
pub enum Field {
    /// An unsigned value.
    U32(u32),
    /// A signed value.
    I32(i32),
    /// A float.
    F32(f32),
    /// A flag.
    Bool(bool),
    /// A string.
    Bytes(Vec<u8>),
    /// A vector.
    Vector(Vector),
    /// A matrix.
    Matrix(Matrix),
    /// A transform.
    Transform(Transform),
    /// A quaternion.
    Quaternion(Quaternion),
    /// A costume.
    Costume(Costume),
    /// Unsigned values.
    U32s(Vec<u32>),
    /// Strings.
    Strings(Vec<Vec<u8>>),
    /// Pairs.
    Pairs(Vec<[u32; 2]>),
    /// Signed pairs (the cargo of a mission ship: good, count).
    Cargo(Vec<(u32, i32)>),
    /// Loadout lines.
    Loadout(Vec<EquipDesc>),
    /// The ship entries of `Cnd_NPCSystemEnter` and `Cnd_NPCSystemExit`.
    Entries(Vec<(Vec<u8>, u32)>),
}

fn bits(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

/// The hex of a string's bytes.
#[must_use]
pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

fn floats(values: &[f32]) -> String {
    values
        .iter()
        .map(|&v| bits(v))
        .collect::<Vec<_>>()
        .join(",")
}

/// A costume as the dump writes it.
#[must_use]
pub fn costume(c: &Costume) -> String {
    let parts: Vec<String> = c.part_ids.iter().map(u32::to_string).collect();
    let accessories: Vec<String> = c.accessory_ids.iter().map(u32::to_string).collect();
    format!(
        "{}/{}/{}",
        parts.join(","),
        c.accessory_ids.len(),
        accessories.join(",")
    )
}

impl Field {
    /// The value as the dump writes it; `strings` resolves a loadout's
    /// hardpoints.
    #[must_use]
    pub fn render(&self, strings: &StringCache) -> String {
        match self {
            Self::U32(v) => v.to_string(),
            Self::I32(v) => v.to_string(),
            Self::F32(v) => bits(*v),
            Self::Bool(v) => u8::from(*v).to_string(),
            Self::Bytes(v) => hex(v),
            Self::Vector(v) => floats(&v.to_words()),
            Self::Matrix(m) => floats(&m.to_words()),
            Self::Transform(t) => floats(&t.to_words()),
            Self::Quaternion(q) => floats(&q.to_words()),
            Self::Costume(c) => costume(c),
            Self::U32s(v) => v.iter().map(u32::to_string).collect::<Vec<_>>().join(","),
            Self::Strings(v) => v.iter().map(|s| hex(s)).collect::<Vec<_>>().join(","),
            Self::Pairs(v) => v
                .iter()
                .map(|[a, b]| format!("{a}:{b}"))
                .collect::<Vec<_>>()
                .join(","),
            Self::Cargo(v) => v
                .iter()
                .map(|(a, b)| format!("{a}:{b}"))
                .collect::<Vec<_>>()
                .join(","),
            Self::Loadout(v) => v
                .iter()
                .map(|d| {
                    format!(
                        "{}:{}:{}:{}:{}:{}:{}",
                        d.id,
                        d.arch_id,
                        d.count,
                        bits(d.health),
                        d.equipped,
                        u8::from(d.mission),
                        hex(strings.get(d.hardpoint))
                    )
                })
                .collect::<Vec<_>>()
                .join(","),
            Self::Entries(v) => v
                .iter()
                .map(|(label, system)| format!("{}:{system}", hex(label)))
                .collect::<Vec<_>>()
                .join(","),
        }
    }
}

/// `name=value` pairs, each after one space, as the dump writes them.
#[must_use]
pub fn render_fields(fields: &[(&'static str, Field)], strings: &StringCache) -> String {
    let mut out = String::new();
    for (name, value) in fields {
        let _ = write!(out, " {name}={}", value.render(strings));
    }
    out
}

macro_rules! from_field {
    ($($ty:ty => $variant:ident),* $(,)?) => {
        $(impl From<&$ty> for Field {
            fn from(value: &$ty) -> Self {
                Self::$variant(value.clone())
            }
        })*
    };
}

from_field! {
    u32 => U32,
    i32 => I32,
    f32 => F32,
    bool => Bool,
    Vec<u8> => Bytes,
    Vector => Vector,
    Matrix => Matrix,
    Transform => Transform,
    Quaternion => Quaternion,
    Costume => Costume,
    Vec<u32> => U32s,
    Vec<Vec<u8>> => Strings,
    Vec<[u32; 2]> => Pairs,
    Vec<(u32, i32)> => Cargo,
    Vec<EquipDesc> => Loadout,
}

impl From<&u8> for Field {
    fn from(value: &u8) -> Self {
        Self::U32(u32::from(*value))
    }
}

impl From<&[u32; 4]> for Field {
    fn from(value: &[u32; 4]) -> Self {
        Self::U32s(value.to_vec())
    }
}

impl From<&[u32; 5]> for Field {
    fn from(value: &[u32; 5]) -> Self {
        Self::U32s(value.to_vec())
    }
}

/// A pointer the C++ keeps (a formation, a system record) as its key:
/// dumped as whether it is set.
impl From<&Option<u32>> for Field {
    fn from(value: &Option<u32>) -> Self {
        Self::Bool(value.is_some())
    }
}

impl From<&Vec<NpcSystemEntry>> for Field {
    fn from(value: &Vec<NpcSystemEntry>) -> Self {
        Self::Entries(
            value
                .iter()
                .map(|e| (e.label.clone(), e.system_id))
                .collect(),
        )
    }
}

/// A record: a struct with public fields, its C++ constructor's values as
/// [`Default`] and `fields()` in the dump's order. The fields of an
/// optional `kept { .. }` block are not in `fields()` (lists the dump
/// writes as lines of their own, or values it does not write).
macro_rules! record {
    (
        $(#[$doc:meta])*
        $name:ident {
            $($(#[$fdoc:meta])* $field:ident: $ty:ty = $default:expr,)*
        }
        $(kept {
            $($(#[$xdoc:meta])* $xfield:ident: $xty:ty = $xdefault:expr,)*
        })?
    ) => {
        $(#[$doc])*
        #[derive(Clone, Debug, PartialEq)]
        pub struct $name {
            $($(#[$fdoc])* pub $field: $ty,)*
            $($($(#[$xdoc])* pub $xfield: $xty,)*)?
        }

        impl Default for $name {
            fn default() -> Self {
                Self {
                    $($field: $default,)*
                    $($($xfield: $xdefault,)*)?
                }
            }
        }

        impl $name {
            /// The fields in the oracle dump's order.
            #[must_use]
            pub fn fields(&self) -> Vec<(&'static str, $crate::content::mission::field::Field)> {
                vec![$((stringify!($field), $crate::content::mission::field::Field::from(&self.$field)),)*]
            }
        }
    };
}

pub(crate) use record;
