//! `MarshalBuffer`, `Common`'s length-prefixed encoding of scalars,
//! strings, format strings, costumes and the equipment and goods lists
//! (`common_marshalbuffer.cpp`).
//!
//! Scalars are raw little-endian copies. A string is a `u32` length and
//! its bytes, or the length `0xFFFFFFFF` for a null string; a wide string
//! counts UTF-16 units. A reader treats a length of `0x4000` or more as
//! null too.
//!
//! The C++ writer has no bound and the reader returns the bytes of its own
//! `this` pointer when the input runs out. [`MarshalWriter`] grows without a
//! bound as well; [`MarshalReader`] fails with [`WireError::Underrun`]
//! (divergence D101).

use super::costume::Costume;
use super::field::{count_u32, wire_len};
use super::fmtstr::FmtStr;
use super::{WireError, WireReader, WireWriter};

/// The length prefix of a null string.
pub const NULL_LENGTH: u32 = 0xffff_ffff;

/// A string length at or above this reads as null.
pub const MAX_STRING_LENGTH: u32 = 0x4000;

/// One equipment item of an `EquipDescList` as `put_EquipDescList` writes
/// it. `Cached` and `Owner` do not travel.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EquipDesc {
    /// `ArchId`.
    pub arch_id: u32,
    /// `Hardpoint`, null for none.
    pub hardpoint: Option<Vec<u8>>,
    /// `Count`.
    pub count: i32,
    /// `Health`.
    pub health: f32,
    /// `Id`, widened to a dword on the wire.
    pub id: u16,
    /// `bEquipped`, a 0 or 1 byte.
    pub equipped: bool,
    /// `bMission`, a 0 or 1 byte.
    pub mission: bool,
}

/// One item of a `GoodDescList`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GoodDesc {
    /// `GoodId`.
    pub good_id: u32,
    /// `Count`.
    pub count: i32,
    /// `Price`.
    pub price: f32,
    /// The fourth dword (`Field0C`).
    pub field_0c: f32,
}

/// The writing half of `MarshalBuffer`.
#[derive(Clone, Debug, Default)]
pub struct MarshalWriter {
    writer: WireWriter,
}

impl MarshalWriter {
    /// An empty writer.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            writer: WireWriter::new(),
        }
    }

    /// The bytes written (`get_num_written` of them).
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        self.writer.as_bytes()
    }

    /// The finished buffer.
    #[must_use]
    pub fn into_bytes(self) -> Vec<u8> {
        self.writer.into_bytes()
    }

    /// `put_void`.
    ///
    /// # Errors
    /// None in practice: the writer has no limit.
    pub fn put_void(&mut self, bytes: &[u8]) -> Result<(), WireError> {
        self.writer.put_bytes(bytes)
    }
    /// `put_U8`.
    ///
    /// # Errors
    /// As [`MarshalWriter::put_void`].
    pub fn put_u8(&mut self, value: u8) -> Result<(), WireError> {
        self.writer.put_u8(value)
    }
    /// `put_U16`.
    ///
    /// # Errors
    /// As [`MarshalWriter::put_void`].
    pub fn put_u16(&mut self, value: u16) -> Result<(), WireError> {
        self.writer.put_u16(value)
    }
    /// `put_U32`, `put_FL_ID`.
    ///
    /// # Errors
    /// As [`MarshalWriter::put_void`].
    pub fn put_u32(&mut self, value: u32) -> Result<(), WireError> {
        self.writer.put_u32(value)
    }
    /// `put_int`.
    ///
    /// # Errors
    /// As [`MarshalWriter::put_void`].
    pub fn put_int(&mut self, value: i32) -> Result<(), WireError> {
        self.writer.put_i32(value)
    }
    /// `put_float`: the bits.
    ///
    /// # Errors
    /// As [`MarshalWriter::put_void`].
    pub fn put_float(&mut self, value: f32) -> Result<(), WireError> {
        self.writer.put_f32(value)
    }
    /// `put_bool`: one byte, 1 or 0.
    ///
    /// # Errors
    /// As [`MarshalWriter::put_void`].
    pub fn put_bool(&mut self, value: bool) -> Result<(), WireError> {
        self.writer.put_u8(u8::from(value))
    }

    /// `put_string`: the length and the bytes, or [`NULL_LENGTH`] for none.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] for a string of `u32::MAX` bytes or
    /// more.
    pub fn put_string(&mut self, text: Option<&[u8]>) -> Result<(), WireError> {
        match text {
            None => self.writer.put_u32(NULL_LENGTH),
            Some(bytes) => {
                let length = count_u32(bytes.len())?;
                if length == NULL_LENGTH {
                    return Err(WireError::LengthOutOfRange {
                        length: bytes.len(),
                        max: bytes.len() - 1,
                    });
                }
                self.writer.put_u32(length)?;
                self.writer.put_bytes(bytes)
            }
        }
    }

    /// `put_wstring`: the length in units and the units, or
    /// [`NULL_LENGTH`] for none.
    ///
    /// # Errors
    /// As [`MarshalWriter::put_string`].
    pub fn put_wstring(&mut self, text: Option<&[u16]>) -> Result<(), WireError> {
        match text {
            None => self.writer.put_u32(NULL_LENGTH),
            Some(units) => {
                self.writer.put_u32(count_u32(units.len())?)?;
                self.writer.put_wide(units)
            }
        }
    }

    /// `put_FmtStr`: the flattened string.
    ///
    /// # Errors
    /// As [`FmtStr::flatten`].
    pub fn put_fmt_str(&mut self, fmt: &FmtStr) -> Result<(), WireError> {
        fmt.put(&mut self.writer)
    }

    /// `put_Costume`.
    ///
    /// # Errors
    /// As [`Costume::put_marshal`].
    pub fn put_costume(&mut self, costume: &Costume) -> Result<(), WireError> {
        costume.put_marshal(&mut self.writer)
    }

    /// `put_EquipDescList`: the count, then per item the arch ID, the
    /// hardpoint string, count, health, ID (a dword) and the two flags.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above `i32::MAX` items.
    pub fn put_equip_desc_list(&mut self, items: &[EquipDesc]) -> Result<(), WireError> {
        self.put_int(count_i32(items.len())?)?;
        for item in items {
            self.put_u32(item.arch_id)?;
            self.put_string(item.hardpoint.as_deref())?;
            self.put_int(item.count)?;
            self.put_float(item.health)?;
            self.put_u32(u32::from(item.id))?;
            self.put_u8(u8::from(item.equipped))?;
            self.put_u8(u8::from(item.mission))?;
        }
        Ok(())
    }

    /// `put_GoodDescList`: the count, then the four dwords of each item.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above `i32::MAX` items.
    pub fn put_good_desc_list(&mut self, items: &[GoodDesc]) -> Result<(), WireError> {
        self.put_int(count_i32(items.len())?)?;
        for item in items {
            self.put_u32(item.good_id)?;
            self.put_int(item.count)?;
            self.put_float(item.price)?;
            self.put_float(item.field_0c)?;
        }
        Ok(())
    }
}

fn count_i32(length: usize) -> Result<i32, WireError> {
    i32::try_from(length).map_err(|_| WireError::LengthOutOfRange {
        length,
        max: usize::try_from(i32::MAX).unwrap_or(usize::MAX),
    })
}

/// The reading half of `MarshalBuffer`.
#[derive(Clone, Debug)]
pub struct MarshalReader<'a> {
    reader: WireReader<'a>,
}

impl<'a> MarshalReader<'a> {
    /// A reader at the start of `bytes` (`use_read_buffer`).
    #[must_use]
    pub const fn new(bytes: &'a [u8]) -> Self {
        Self {
            reader: WireReader::new(bytes),
        }
    }

    /// Bytes consumed.
    #[must_use]
    pub const fn position(&self) -> usize {
        self.reader.position()
    }

    /// `is_empty`: nothing left.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.reader.is_empty()
    }

    /// `get_void` of `count` bytes.
    ///
    /// # Errors
    /// [`WireError::Underrun`] when fewer remain; the C++ copies nothing
    /// and moves on.
    pub fn get_void(&mut self, count: usize) -> Result<&'a [u8], WireError> {
        self.reader.get_bytes(count)
    }
    /// `skip_over`.
    ///
    /// # Errors
    /// As [`MarshalReader::get_void`]; the C++ moves past the end.
    pub fn skip_over(&mut self, count: usize) -> Result<(), WireError> {
        self.reader.get_bytes(count).map(|_| ())
    }
    /// `get_U8`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] at the end.
    pub fn get_u8(&mut self) -> Result<u8, WireError> {
        self.reader.get_u8()
    }
    /// `get_U16`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] at the end.
    pub fn get_u16(&mut self) -> Result<u16, WireError> {
        self.reader.get_u16()
    }
    /// `get_U32`, `get_FL_ID`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] at the end.
    pub fn get_u32(&mut self) -> Result<u32, WireError> {
        self.reader.get_u32()
    }
    /// `get_int`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] at the end; the C++ returns 0.
    pub fn get_int(&mut self) -> Result<i32, WireError> {
        self.reader.get_i32()
    }
    /// `get_float`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] at the end.
    pub fn get_float(&mut self) -> Result<f32, WireError> {
        self.reader.get_f32()
    }
    /// `get_bool`: true only for the byte 1.
    ///
    /// # Errors
    /// [`WireError::Underrun`] at the end.
    pub fn get_bool(&mut self) -> Result<bool, WireError> {
        self.reader.get_u8().map(|b| b == 1)
    }

    fn get_length(&mut self) -> Result<Option<u32>, WireError> {
        let length = self.reader.get_u32()?;
        Ok((length != NULL_LENGTH && length < MAX_STRING_LENGTH).then_some(length))
    }

    /// `get_string`: the bytes, or `None` for a null string or a length of
    /// [`MAX_STRING_LENGTH`] or more (whose bytes stay unread).
    ///
    /// # Errors
    /// [`WireError::Underrun`] when the input ends inside the string.
    pub fn get_string(&mut self) -> Result<Option<Vec<u8>>, WireError> {
        match self.get_length()? {
            None => Ok(None),
            Some(length) => {
                let length = wire_len(length, 1, &self.reader)?;
                self.reader.get_bytes(length).map(|b| Some(b.to_vec()))
            }
        }
    }

    /// `get_string_to_buffer` with a buffer of `max_len` bytes: at most
    /// `max_len - 1` bytes are kept, the whole string is consumed. A null
    /// string gives an empty one.
    ///
    /// # Errors
    /// As [`MarshalReader::get_string`].
    pub fn get_string_to_buffer(&mut self, max_len: usize) -> Result<Vec<u8>, WireError> {
        let mut text = self.get_string()?.unwrap_or_default();
        text.truncate(max_len.saturating_sub(1));
        Ok(text)
    }

    /// `get_wstring`.
    ///
    /// # Errors
    /// As [`MarshalReader::get_string`].
    pub fn get_wstring(&mut self) -> Result<Option<Vec<u16>>, WireError> {
        match self.get_length()? {
            None => Ok(None),
            Some(length) => {
                let length = wire_len(length, 2, &self.reader)?;
                self.reader.get_wide(length).map(Some)
            }
        }
    }

    /// `get_FmtStr`.
    ///
    /// # Errors
    /// As [`FmtStr::get`].
    pub fn get_fmt_str(&mut self) -> Result<FmtStr, WireError> {
        FmtStr::get(&mut self.reader)
    }

    /// `get_Costume`.
    ///
    /// # Errors
    /// As [`Costume::get_marshal`].
    pub fn get_costume(&mut self) -> Result<Costume, WireError> {
        Costume::get_marshal(&mut self.reader)
    }

    /// `get_EquipDescList`. A negative count reads no item; the ID keeps the
    /// low 16 bits of its dword.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get_equip_desc_list(&mut self) -> Result<Vec<EquipDesc>, WireError> {
        let count = u32::try_from(self.get_int()?).unwrap_or(0);
        // Each item takes at least 22 bytes.
        let _ = wire_len(count, 22, &self.reader)?;
        let mut items = Vec::new();
        for _ in 0..count {
            let arch_id = self.get_u32()?;
            let hardpoint = self.get_string()?;
            let count = self.get_int()?;
            let health = self.get_float()?;
            let [id_low, id_high, ..] = self.get_u32()?.to_le_bytes();
            let equipped = self.get_u8()? == 1;
            let mission = self.get_u8()? == 1;
            items.push(EquipDesc {
                arch_id,
                hardpoint,
                count,
                health,
                id: u16::from_le_bytes([id_low, id_high]),
                equipped,
                mission,
            });
        }
        Ok(items)
    }

    /// `get_GoodDescList`. A negative count reads no item.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get_good_desc_list(&mut self) -> Result<Vec<GoodDesc>, WireError> {
        let count = u32::try_from(self.get_int()?).unwrap_or(0);
        let count = wire_len(count, 16, &self.reader)?;
        let mut items = Vec::with_capacity(count);
        for _ in 0..count {
            items.push(GoodDesc {
                good_id: self.get_u32()?,
                count: self.get_int()?,
                price: self.get_float()?,
                field_0c: self.get_float()?,
            });
        }
        Ok(items)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strings_and_nulls() {
        let mut writer = MarshalWriter::new();
        writer.put_string(None).unwrap();
        writer.put_string(Some(b"abc")).unwrap();
        writer.put_wstring(Some(&[0x41])).unwrap();
        writer.put_bool(true).unwrap();
        writer.put_u32(MAX_STRING_LENGTH).unwrap();
        let bytes = writer.into_bytes();
        assert_eq!(
            &bytes[..11],
            [0xff, 0xff, 0xff, 0xff, 3, 0, 0, 0, b'a', b'b', b'c']
        );
        let mut reader = MarshalReader::new(&bytes);
        assert_eq!(reader.get_string().unwrap(), None);
        assert_eq!(reader.get_string_to_buffer(3).unwrap(), b"ab");
        assert_eq!(reader.get_wstring().unwrap(), Some(vec![0x41]));
        assert!(reader.get_bool().unwrap());
        assert_eq!(reader.get_string().unwrap(), None);
        assert!(reader.is_empty());
        assert!(reader.get_u8().is_err());
    }

    #[test]
    fn lists_round_trip() {
        let equip = vec![EquipDesc {
            arch_id: 7,
            hardpoint: Some(b"HpWeapon01".to_vec()),
            count: 2,
            health: 0.5,
            id: 34,
            equipped: true,
            mission: false,
        }];
        let goods = vec![GoodDesc {
            good_id: 1,
            count: -1,
            price: 1.0,
            field_0c: 2.0,
        }];
        let mut writer = MarshalWriter::new();
        writer.put_equip_desc_list(&equip).unwrap();
        writer.put_good_desc_list(&goods).unwrap();
        let bytes = writer.into_bytes();
        let mut reader = MarshalReader::new(&bytes);
        assert_eq!(reader.get_equip_desc_list().unwrap(), equip);
        assert_eq!(reader.get_good_desc_list().unwrap(), goods);
    }
}
