//! `FmtStr`, the formatted string of mission text, comms and names
//! (`common_fmtstr.cpp`), and its flattened form.
//!
//! A format string is a mad-lib ID (a text resource with placeholders)
//! and a list of values. Each value carries a type tag, a flag byte (the
//! running index of its kind when the string was built) and a payload:
//! a nested format string (tag `0x21`), a five-dword navigation marker
//! (tag `0x4E`) or one dword.
//!
//! Flattened (`FmtStr::flatten`, little-endian):
//!
//! | Field | Width |
//! | --- | --- |
//! | mad-lib ID | `u32` |
//! | value count | `u16` |
//! | per value: tag, flags | `u16`, `u8` |
//! | payload: a nested string flattened, 5 × `u32`, or `u32` | |
//!
//! `unflatten` in the C++ reads without a bound and recurses without a
//! limit; [`FmtStr::get`] checks every field and refuses nesting deeper than
//! [`MAX_DEPTH`] (divergence D92).

use super::{WireError, WireReader, WireWriter};

/// Tag of a nested format string.
pub const TAG_NESTED: u16 = 0x21;
/// Tag of a navigation marker.
pub const TAG_NAV_MARKER: u16 = 0x4e;
/// Tag of `append_system`, `'S'`.
pub const TAG_SYSTEM: u16 = 0x53;
/// Tag of `append_base`, `'B'`.
pub const TAG_BASE: u16 = 0x42;
/// Tag of `append_string`, `'s'`.
pub const TAG_STRING: u16 = 0x73;
/// Tag of `append_good`, `'G'`.
pub const TAG_GOOD: u16 = 0x47;
/// Tag of `append_int`, `'d'`.
pub const TAG_INT: u16 = 0x64;
/// Tag of `append_rep_instance`, `'R'`.
pub const TAG_REP_INSTANCE: u16 = 0x52;
/// Tag of `append_rep_group`, `'F'`.
pub const TAG_REP_GROUP: u16 = 0x46;
/// Tag of `append_zone_id`, `'Z'`.
pub const TAG_ZONE_ID: u16 = 0x5a;
/// Tag of `append_spaceobj_id`, `'O'`.
pub const TAG_SPACE_OBJ_ID: u16 = 0x4f;
/// Tag of `append_installation`, `'I'`.
pub const TAG_INSTALLATION: u16 = 0x49;
/// Tag of `append_loot`, `'L'`.
pub const TAG_LOOT: u16 = 0x4c;

/// The deepest nesting the decoder accepts.
pub const MAX_DEPTH: usize = 32;

/// One value of a format string.
#[derive(Clone, Debug, PartialEq)]
pub enum FmtValue {
    /// A dword with its tag (any tag but `0x21` and `0x4E`).
    Value {
        /// The type tag.
        tag: u16,
        /// The flag byte.
        flags: u8,
        /// The ID or number.
        value: u32,
    },
    /// A nested format string, tag `0x21`.
    Nested {
        /// The flag byte.
        flags: u8,
        /// The nested string.
        fmt: FmtStr,
    },
    /// A navigation marker, tag `0x4E`: five dwords (two IDs and a
    /// position as float bits, in the C++ comparison).
    NavMarker {
        /// The flag byte.
        flags: u8,
        /// The five dwords.
        data: [u32; 5],
    },
}

impl FmtValue {
    /// The type tag.
    #[must_use]
    pub const fn tag(&self) -> u16 {
        match self {
            Self::Value { tag, .. } => *tag,
            Self::Nested { .. } => TAG_NESTED,
            Self::NavMarker { .. } => TAG_NAV_MARKER,
        }
    }

    /// The flag byte.
    #[must_use]
    pub const fn flags(&self) -> u8 {
        match self {
            Self::Value { flags, .. }
            | Self::Nested { flags, .. }
            | Self::NavMarker { flags, .. } => *flags,
        }
    }

    /// The bytes this value flattens to (`Val::get_flattened_size`).
    #[must_use]
    pub fn flattened_size(&self) -> usize {
        match self {
            Self::Value { .. } => 7,
            Self::NavMarker { .. } => 0x17,
            Self::Nested { fmt, .. } => 3 + fmt.flattened_size(),
        }
    }
}

/// A formatted string.
///
/// Equality compares the mad-lib ID and the values, as `operator==` does,
/// but structurally: the C++ compares the marker's last three dwords as
/// floats with a tolerance.
#[derive(Clone, Debug, Default)]
pub struct FmtStr {
    /// The mad-lib ID (`begin_mad_lib`).
    pub mad_lib_id: u32,
    /// The values in order.
    pub values: Vec<FmtValue>,
    /// The per-kind running indices `append_*` stamp into the flags
    /// (`ValueIndex`); not on the wire, and zero after a decode.
    counters: [u8; 14],
}

impl PartialEq for FmtStr {
    fn eq(&self, other: &Self) -> bool {
        self.mad_lib_id == other.mad_lib_id && self.values == other.values
    }
}

/// Counter slots of the `append_*` kinds (`FmtStr::ValueKind`).
mod kind {
    pub const NAV_MARKER: usize = 0;
    pub const SYSTEM: usize = 1;
    pub const BASE: usize = 2;
    pub const STRING: usize = 3;
    pub const GOOD: usize = 4;
    pub const INT: usize = 6;
    pub const REP_INSTANCE: usize = 7;
    pub const REP_GROUP: usize = 8;
    pub const ZONE_ID: usize = 9;
    pub const SPACE_OBJ_ID: usize = 10;
    pub const NESTED: usize = 11;
    pub const INSTALLATION: usize = 12;
    pub const LOOT: usize = 13;
}

impl FmtStr {
    /// An empty string for `mad_lib_id`.
    #[must_use]
    pub fn new(mad_lib_id: u32) -> Self {
        Self {
            mad_lib_id,
            values: Vec::new(),
            counters: [0; 14],
        }
    }

    fn append_counted(&mut self, slot: usize, tag: u16, value: u32) {
        let flags = self.counters[slot];
        self.counters[slot] = flags.wrapping_add(1);
        self.values.push(FmtValue::Value { tag, flags, value });
    }

    /// `append_system`.
    pub fn append_system(&mut self, id: u32) {
        self.append_counted(kind::SYSTEM, TAG_SYSTEM, id);
    }
    /// `append_base`.
    pub fn append_base(&mut self, id: u32) {
        self.append_counted(kind::BASE, TAG_BASE, id);
    }
    /// `append_string` (a text resource ID).
    pub fn append_string(&mut self, id: u32) {
        self.append_counted(kind::STRING, TAG_STRING, id);
    }
    /// `append_good`.
    pub fn append_good(&mut self, id: u32) {
        self.append_counted(kind::GOOD, TAG_GOOD, id);
    }
    /// `append_int`.
    pub fn append_int(&mut self, value: u32) {
        self.append_counted(kind::INT, TAG_INT, value);
    }
    /// `append_rep_instance`.
    pub fn append_rep_instance(&mut self, id: i32) {
        self.append_counted(
            kind::REP_INSTANCE,
            TAG_REP_INSTANCE,
            u32::from_le_bytes(id.to_le_bytes()),
        );
    }
    /// `append_rep_group`.
    pub fn append_rep_group(&mut self, id: u32) {
        self.append_counted(kind::REP_GROUP, TAG_REP_GROUP, id);
    }
    /// `append_zone_id`.
    pub fn append_zone_id(&mut self, id: u32) {
        self.append_counted(kind::ZONE_ID, TAG_ZONE_ID, id);
    }
    /// `append_spaceobj_id`.
    pub fn append_spaceobj_id(&mut self, id: u32) {
        self.append_counted(kind::SPACE_OBJ_ID, TAG_SPACE_OBJ_ID, id);
    }
    /// `append_installation`.
    pub fn append_installation(&mut self, id: i32) {
        self.append_counted(
            kind::INSTALLATION,
            TAG_INSTALLATION,
            u32::from_le_bytes(id.to_le_bytes()),
        );
    }
    /// `append_loot`.
    pub fn append_loot(&mut self, id: i32) {
        self.append_counted(kind::LOOT, TAG_LOOT, u32::from_le_bytes(id.to_le_bytes()));
    }

    /// `append_nav_marker`.
    pub fn append_nav_marker(&mut self, data: [u32; 5]) {
        let flags = self.counters[kind::NAV_MARKER];
        self.counters[kind::NAV_MARKER] = flags.wrapping_add(1);
        self.values.push(FmtValue::NavMarker { flags, data });
    }

    /// `append_fmt_str`: a copy of `other` as a nested value. The copy
    /// keeps `other`'s counters, as `operator=` does.
    pub fn append_fmt_str(&mut self, other: &Self) {
        let flags = self.counters[kind::NESTED];
        self.counters[kind::NESTED] = flags.wrapping_add(1);
        self.values.push(FmtValue::Nested {
            flags,
            fmt: other.clone(),
        });
    }

    /// The bytes [`FmtStr::flatten`] writes (`RpcGetFlattenedFmtStrSize`).
    #[must_use]
    pub fn flattened_size(&self) -> usize {
        6 + self
            .values
            .iter()
            .map(FmtValue::flattened_size)
            .sum::<usize>()
    }

    /// The flattened form (`FmtStr::flatten`).
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] for more than 65 535 values in one
    /// list; the C++ truncates the count (divergence D88).
    pub fn flatten(&self) -> Result<Vec<u8>, WireError> {
        let mut writer = WireWriter::new();
        self.put(&mut writer)?;
        Ok(writer.into_bytes())
    }

    /// Writes the flattened form.
    ///
    /// # Errors
    /// As [`FmtStr::flatten`], and [`WireError::Overflow`] at the writer's
    /// limit.
    pub fn put(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        let count = super::field::count_u16(self.values.len())?;
        writer.put_u32(self.mad_lib_id)?;
        writer.put_u16(count)?;
        for value in &self.values {
            writer.put_u16(value.tag())?;
            writer.put_u8(value.flags())?;
            match value {
                FmtValue::Value { tag, value, .. } => {
                    if matches!(*tag, TAG_NESTED | TAG_NAV_MARKER) {
                        return Err(WireError::Invalid("FmtStr value with a structured tag"));
                    }
                    writer.put_u32(*value)?;
                }
                FmtValue::Nested { fmt, .. } => fmt.put(writer)?,
                FmtValue::NavMarker { data, .. } => super::field::put_u32s(writer, data)?,
            }
        }
        Ok(())
    }

    /// The format string `bytes` start with, and the bytes it took
    /// (`FmtStr::unflatten`).
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the input ends inside it, or
    /// [`WireError::Invalid`] for nesting deeper than [`MAX_DEPTH`].
    pub fn unflatten(bytes: &[u8]) -> Result<(Self, usize), WireError> {
        let mut reader = WireReader::new(bytes);
        let fmt = Self::get(&mut reader)?;
        Ok((fmt, reader.position()))
    }

    /// Reads a flattened format string.
    ///
    /// # Errors
    /// As [`FmtStr::unflatten`].
    pub fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Self::get_nested(reader, 0)
    }

    fn get_nested(reader: &mut WireReader<'_>, depth: usize) -> Result<Self, WireError> {
        if depth > MAX_DEPTH {
            return Err(WireError::Invalid("FmtStr nested too deep"));
        }
        let mad_lib_id = reader.get_u32()?;
        let count = reader.get_u16()?;
        // Each value takes at least 7 bytes; bound the allocation by the input.
        let mut values = Vec::with_capacity(usize::from(count).min(reader.remaining() / 7));
        for _ in 0..count {
            let tag = reader.get_u16()?;
            let flags = reader.get_u8()?;
            values.push(match tag {
                TAG_NESTED => FmtValue::Nested {
                    flags,
                    fmt: Self::get_nested(reader, depth + 1)?,
                },
                TAG_NAV_MARKER => {
                    let mut data = [0; 5];
                    for word in &mut data {
                        *word = reader.get_u32()?;
                    }
                    FmtValue::NavMarker { flags, data }
                }
                _ => FmtValue::Value {
                    tag,
                    flags,
                    value: reader.get_u32()?,
                },
            });
        }
        Ok(Self {
            mad_lib_id,
            values,
            counters: [0; 14],
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flatten_layout() {
        let mut inner = FmtStr::new(7);
        inner.append_int(5);
        let mut fmt = FmtStr::new(0x0102_0304);
        fmt.append_system(0xaabb_ccdd);
        fmt.append_system(1);
        fmt.append_fmt_str(&inner);
        fmt.append_nav_marker([1, 2, 3, 4, 5]);
        let bytes = fmt.flatten().unwrap();
        assert_eq!(bytes.len(), fmt.flattened_size());
        assert_eq!(&bytes[..6], [4, 3, 2, 1, 4, 0]);
        assert_eq!(&bytes[6..13], [0x53, 0, 0, 0xdd, 0xcc, 0xbb, 0xaa]);
        // The second system value carries the running index 1.
        assert_eq!(&bytes[13..16], [0x53, 0, 1]);
        // The nested string: tag, flags, then its own header.
        assert_eq!(&bytes[20..29], [0x21, 0, 0, 7, 0, 0, 0, 1, 0]);
        let (back, used) = FmtStr::unflatten(&bytes).unwrap();
        assert_eq!(used, bytes.len());
        assert_eq!(back, fmt);
    }

    #[test]
    fn truncated_and_deep_input_fails() {
        assert!(FmtStr::unflatten(&[1, 0, 0, 0, 1, 0, 0x64, 0]).is_err());
        let mut deep = Vec::new();
        for _ in 0..40 {
            deep.extend_from_slice(&[0, 0, 0, 0, 1, 0, 0x21, 0, 0]);
        }
        deep.extend_from_slice(&[0, 0, 0, 0, 0, 0]);
        assert!(matches!(
            FmtStr::unflatten(&deep),
            Err(WireError::Invalid(_))
        ));
    }
}
