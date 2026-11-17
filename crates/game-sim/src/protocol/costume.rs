//! `Costume` (`include/common_reputation.h`, `common_costume.cpp`) and the
//! forms it takes on the wire.
//!
//! A costume is four body-part IDs and up to eight accessory IDs. Four
//! encodings carry it:
//!
//! | Form | Fields | C++ |
//! | --- | --- | --- |
//! | [`Costume::put_marshal`] | 4 parts, `u32` count, the accessories | `MarshalBuffer::put_Costume` |
//! | [`Costume::put_comm`] | parts 0 and 1, `u8` count, the accessories | `SENDCOMM` |
//! | [`Costume::put_id_block`] | parts 1, 0, 2, 3, `u32` count, the accessories | `CHARACTERINFO`'s costume blocks |
//! | [`CreateCostume`] | body, head, `u8` count, the accessories | `CREATESHIP`, `CREATESOLAR` |
//!
//! The C++ copies a count above eight past the accessory array, in both
//! directions; the marshal and comm forms refuse it (divergence D93).

use super::field::{count_u8, count_u32, get_u32s, put_u32s};
use super::{WireError, WireReader, WireWriter};

/// The accessory slots of a costume.
pub const MAX_ACCESSORIES: usize = 8;

/// A costume.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Costume {
    /// The body-part IDs (`PartIds`).
    pub parts: [u32; 4],
    /// The accessory IDs, at most [`MAX_ACCESSORIES`] where a `Costume`
    /// holds them.
    pub accessories: Vec<u32>,
}

fn check_slots(count: usize) -> Result<(), WireError> {
    if count > MAX_ACCESSORIES {
        Err(WireError::LengthOutOfRange {
            length: count,
            max: MAX_ACCESSORIES,
        })
    } else {
        Ok(())
    }
}

impl Costume {
    /// `MarshalBuffer::put_Costume`: the four parts, the count, the
    /// accessories, every one a dword.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above eight accessories; otherwise
    /// the writer's errors.
    pub fn put_marshal(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        check_slots(self.accessories.len())?;
        put_u32s(writer, &self.parts)?;
        writer.put_u32(count_u32(self.accessories.len())?)?;
        put_u32s(writer, &self.accessories)
    }

    /// `MarshalBuffer::get_Costume`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input (the C++ stores zeros or
    /// stale values) and [`WireError::LengthOutOfRange`] above eight
    /// accessories.
    pub fn get_marshal(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let mut parts = [0; 4];
        for part in &mut parts {
            *part = reader.get_u32()?;
        }
        let count = reader.get_u32()?;
        check_slots(usize::try_from(count).unwrap_or(usize::MAX))?;
        Ok(Self {
            parts,
            accessories: get_u32s(reader, count)?,
        })
    }

    /// The `SENDCOMM` form: parts 0 and 1, a count byte, the accessories.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above eight accessories.
    pub fn put_comm(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        check_slots(self.accessories.len())?;
        writer.put_u32(self.parts[0])?;
        writer.put_u32(self.parts[1])?;
        writer.put_u8(count_u8(self.accessories.len())?)?;
        put_u32s(writer, &self.accessories)
    }

    /// The `SENDCOMM` form; parts 2 and 3 come back zero.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input and
    /// [`WireError::LengthOutOfRange`] above eight accessories.
    pub fn get_comm(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let part0 = reader.get_u32()?;
        let part1 = reader.get_u32()?;
        let count = reader.get_u8()?;
        check_slots(usize::from(count))?;
        Ok(Self {
            parts: [part0, part1, 0, 0],
            accessories: get_u32s(reader, u32::from(count))?,
        })
    }

    /// The `CHARACTERINFO` block: parts 1, 0, 2, 3, a dword count, the
    /// accessories. The block's array is allocated by count, so any count
    /// travels.
    ///
    /// # Errors
    /// The writer's errors.
    pub fn put_id_block(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        let [p0, p1, p2, p3] = self.parts;
        put_u32s(writer, &[p1, p0, p2, p3])?;
        writer.put_u32(count_u32(self.accessories.len())?)?;
        put_u32s(writer, &self.accessories)
    }

    /// The `CHARACTERINFO` block.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get_id_block(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let p1 = reader.get_u32()?;
        let p0 = reader.get_u32()?;
        let p2 = reader.get_u32()?;
        let p3 = reader.get_u32()?;
        let count = reader.get_u32()?;
        Ok(Self {
            parts: [p0, p1, p2, p3],
            accessories: get_u32s(reader, count)?,
        })
    }
}

/// The costume of the create-object block (`FRpcServerCreateObjectCostume`):
/// the body and head IDs and the accessories. The hands never travel.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CreateCostume {
    /// `CostumeBody`.
    pub body: u32,
    /// `CostumeHead`.
    pub head: u32,
    /// `CostumeAccessory`, up to 255 on the wire.
    pub accessories: Vec<u32>,
}

impl CreateCostume {
    /// Body, head, a count byte, the accessories.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above 255 accessories.
    pub fn put(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        let count = count_u8(self.accessories.len())?;
        writer.put_u32(self.body)?;
        writer.put_u32(self.head)?;
        writer.put_u8(count)?;
        put_u32s(writer, &self.accessories)
    }

    /// The inverse of [`CreateCostume::put`].
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let body = reader.get_u32()?;
        let head = reader.get_u32()?;
        let count = reader.get_u8()?;
        Ok(Self {
            body,
            head,
            accessories: get_u32s(reader, u32::from(count))?,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn forms() {
        let costume = Costume {
            parts: [1, 2, 3, 4],
            accessories: vec![9],
        };
        let mut writer = WireWriter::new();
        costume.put_comm(&mut writer).unwrap();
        costume.put_id_block(&mut writer).unwrap();
        let bytes = writer.into_bytes();
        assert_eq!(&bytes[..9], [1, 0, 0, 0, 2, 0, 0, 0, 1]);
        assert_eq!(&bytes[13..17], [2, 0, 0, 0]);
        let mut reader = WireReader::new(&bytes);
        let comm = Costume::get_comm(&mut reader).unwrap();
        assert_eq!(comm.parts, [1, 2, 0, 0]);
        assert_eq!(Costume::get_id_block(&mut reader).unwrap(), costume);
        let too_many = Costume {
            parts: [0; 4],
            accessories: vec![0; 9],
        };
        assert!(too_many.put_comm(&mut WireWriter::new()).is_err());
    }
}
