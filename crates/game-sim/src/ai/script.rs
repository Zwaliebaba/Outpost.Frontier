//! `ScriptBehavior` (`Common/common_scriptbehavior.cpp`): a scripted
//! character behaviour as Content describes it and the server sends it
//! (`FLPACKET_SERVER_GFSCRIPTBEHAVIOR`): ids with names, named words,
//! three words, a name and a description, through `MarshalBuffer`.

use crate::protocol::WireError;
use crate::protocol::fmtstr::FmtStr;
use crate::protocol::marshal::{MarshalReader, MarshalWriter};

/// The most entries of either list (`IdArray[32]`, `StringArray2[32]`).
pub const MAX_ENTRIES: usize = 32;

/// One scripted behaviour (0x260 bytes).
#[derive(Clone, Debug)]
pub struct ScriptBehavior {
    /// +0x00 the key (-1 until assigned).
    pub behavior_id: i32,
    /// +0x04.
    pub field_04: u32,
    /// +0x08 (1).
    pub flag_08: bool,
    /// +0x09 (0).
    pub flag_09: bool,
    /// +0x0a (1).
    pub flag_0a: bool,
    /// +0x0c.
    pub field_0c: u32,
    /// +0x10.
    pub field_10: u32,
    /// +0x14 (1).
    pub flag_14: bool,
    /// +0x18 / +0x98 the ids and their strings (at most 32).
    pub ids: Vec<(u32, Option<Vec<u8>>)>,
    /// +0x11c / +0x19c the named words (at most 32).
    pub words: Vec<(Option<Vec<u8>>, u32)>,
    /// +0x220.
    pub field_220: u32,
    /// +0x224.
    pub field_224: u32,
    /// +0x228.
    pub field_228: u32,
    /// +0x22c the name.
    pub name: FmtStr,
    /// +0x244 the description.
    pub description: FmtStr,
    /// +0x25c.
    pub field_25c: u32,
}

impl Default for ScriptBehavior {
    /// `ScriptBehavior::clear`.
    fn default() -> Self {
        Self {
            behavior_id: -1,
            field_04: 0,
            flag_08: true,
            flag_09: false,
            flag_0a: true,
            field_0c: 0,
            field_10: 0,
            flag_14: true,
            ids: Vec::new(),
            words: Vec::new(),
            field_220: 0,
            field_224: 0,
            field_228: 0,
            name: FmtStr::default(),
            description: FmtStr::default(),
            field_25c: 0,
        }
    }
}

fn count(n: usize) -> i32 {
    i32::try_from(n).unwrap_or(i32::MAX)
}

impl ScriptBehavior {
    /// `get_key`.
    #[must_use]
    pub fn key(&self) -> u32 {
        u32::from_ne_bytes(self.behavior_id.to_ne_bytes())
    }

    /// `marshal`.
    ///
    /// # Errors
    /// The writer's.
    pub fn marshal(&self, buf: &mut MarshalWriter) -> Result<(), WireError> {
        buf.put_int(self.behavior_id)?;
        buf.put_u32(self.field_04)?;
        buf.put_bool(self.flag_08)?;
        buf.put_bool(self.flag_09)?;
        buf.put_bool(self.flag_0a)?;
        buf.put_u32(self.field_0c)?;
        buf.put_u32(self.field_10)?;
        buf.put_bool(self.flag_14)?;
        buf.put_int(count(self.ids.len()))?;
        for (id, text) in &self.ids {
            buf.put_u32(*id)?;
            buf.put_string(text.as_deref())?;
        }
        buf.put_int(count(self.words.len()))?;
        for (text, word) in &self.words {
            buf.put_string(text.as_deref())?;
            buf.put_u32(*word)?;
        }
        buf.put_u32(self.field_220)?;
        buf.put_u32(self.field_224)?;
        buf.put_u32(self.field_228)?;
        buf.put_fmt_str(&self.name)?;
        buf.put_fmt_str(&self.description)?;
        buf.put_u32(self.field_25c)
    }

    /// `unmarshal`. A list count outside 0 to 32 is refused (D196).
    ///
    /// # Errors
    /// The reader's, and [`WireError::LengthOutOfRange`] for a count.
    pub fn unmarshal(buf: &mut MarshalReader<'_>) -> Result<Self, WireError> {
        let entries = |n: i32| {
            usize::try_from(n).ok().filter(|&n| n <= MAX_ENTRIES).ok_or(
                WireError::LengthOutOfRange {
                    length: usize::try_from(n).unwrap_or(usize::MAX),
                    max: MAX_ENTRIES,
                },
            )
        };
        let mut s = Self {
            behavior_id: buf.get_int()?,
            field_04: buf.get_u32()?,
            flag_08: buf.get_bool()?,
            flag_09: buf.get_bool()?,
            flag_0a: buf.get_bool()?,
            field_0c: buf.get_u32()?,
            field_10: buf.get_u32()?,
            flag_14: buf.get_bool()?,
            ..Self::default()
        };
        for _ in 0..entries(buf.get_int()?)? {
            let id = buf.get_u32()?;
            s.ids.push((id, buf.get_string()?));
        }
        for _ in 0..entries(buf.get_int()?)? {
            let text = buf.get_string()?;
            s.words.push((text, buf.get_u32()?));
        }
        s.field_220 = buf.get_u32()?;
        s.field_224 = buf.get_u32()?;
        s.field_228 = buf.get_u32()?;
        s.name = buf.get_fmt_str()?;
        s.description = buf.get_fmt_str()?;
        s.field_25c = buf.get_u32()?;
        Ok(s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips() {
        let s = ScriptBehavior {
            behavior_id: 5,
            ids: vec![(7, Some(b"seven".to_vec())), (8, None)],
            words: vec![(Some(b"w".to_vec()), 3)],
            field_25c: 9,
            ..ScriptBehavior::default()
        };
        let mut w = MarshalWriter::new();
        s.marshal(&mut w).unwrap_or_default();
        let bytes = w.into_bytes();
        let mut r = MarshalReader::new(&bytes);
        let back = ScriptBehavior::unmarshal(&mut r).ok();
        assert!(r.is_empty());
        let back = back.unwrap_or_default();
        assert_eq!(back.key(), 5);
        assert_eq!(back.ids, s.ids);
        assert_eq!(back.words, s.words);
        assert_eq!(back.field_25c, 9);
        assert!(back.flag_08 && !back.flag_09);
    }

    #[test]
    fn refuses_long_lists() {
        let mut w = MarshalWriter::new();
        for v in [1i32, 0] {
            w.put_int(v).unwrap_or_default();
        }
        for _ in 0..3 {
            w.put_bool(true).unwrap_or_default();
        }
        for _ in 0..2 {
            w.put_u32(0).unwrap_or_default();
        }
        w.put_bool(true).unwrap_or_default();
        w.put_int(33).unwrap_or_default();
        let bytes = w.into_bytes();
        let mut r = MarshalReader::new(&bytes);
        assert!(matches!(
            ScriptBehavior::unmarshal(&mut r),
            Err(WireError::LengthOutOfRange { length: 33, .. })
        ));
    }
}
