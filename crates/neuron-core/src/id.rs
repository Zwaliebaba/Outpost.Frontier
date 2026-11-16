//! Nickname IDs and sub-object IDs (work package P1.1).
//!
//! Ported from `CppSource/Common/common_id.cpp` and
//! `CppSource/Common/common_subobjectid.cpp`. Names are C string bytes:
//! everything from the first NUL byte on is ignored.

use crate::hash::c_str;

/// `CreateStaticID` copies at most this many bytes of a name.
const CREATE_ID_MAX_BYTES: usize = 0xFF;
/// `MakeId` copies at most this many bytes of a name.
const MAKE_ID_MAX_BYTES: usize = 0x7F;
/// `MakeLocationID` formats into a buffer of this size, terminator included.
const LOCATION_TEXT_BYTES: usize = 0x100;

const CRC30_WIDTH: u32 = 30;
const CRC30_POLYNOMIAL: u32 = 0x8005;
const CRC30_MASK: u32 = (1 << CRC30_WIDTH) - 1;

/// `ReflectBits`: the low `count` bits of `value` reversed, the bits above
/// them kept.
const fn reflect_bits(value: u32, count: u32) -> u32 {
    let mut result = value;
    let mut source = value;
    let mut index = 0;
    while index < count {
        let mask = 1u32 << (count - index - 1);
        if source & 1 == 0 {
            result &= !mask;
        } else {
            result |= mask;
        }
        source >>= 1;
        index += 1;
    }
    result
}

/// The table `CFLIDMaker::Initialize` builds: a reflected 30-bit CRC with
/// polynomial 0x8005.
const CRC30_TABLE: [u32; 256] = {
    let mut table = [0u32; 256];
    let msb = 1u32 << (CRC30_WIDTH - 1);
    let mut byte = 0;
    while byte < 256 {
        let mut work = reflect_bits(byte as u32, 8) << (CRC30_WIDTH - 8);
        let mut bit = 0;
        while bit < 8 {
            work = if work & msb == 0 {
                work << 1
            } else {
                (work << 1) ^ CRC30_POLYNOMIAL
            };
            bit += 1;
        }
        table[byte] = reflect_bits(work, CRC30_WIDTH) & CRC30_MASK;
        byte += 1;
    }
    table
};

/// `CreateID`: the 32-bit ID of a nickname. ASCII letters are folded to
/// lower case and the top bit is always set.
#[must_use]
pub fn create_id(name: &[u8]) -> u32 {
    let name = c_str(name);
    let name = &name[..name.len().min(CREATE_ID_MAX_BYTES)];
    let crc = name.iter().fold(0u32, |crc, &b| {
        let index = (crc & 0xFF) ^ u32::from(b.to_ascii_lowercase());
        CRC30_TABLE[index as usize] ^ (crc >> 8)
    });
    // FinalizeStaticId: the masked CRC is byte-swapped into a 32-bit
    // register and shifted down by the two unused bits.
    let mut folded = 0u32;
    let mut remain = crc & CRC30_MASK;
    let mut shift_pos = 8;
    while shift_pos <= 32 {
        folded |= (remain & 0xFF) << (32 - shift_pos);
        shift_pos += 8;
        remain >>= 8;
    }
    (folded >> (32 - CRC30_WIDTH)) | 0x8000_0000
}

/// The table `MakeId` uses: CRC-16-CCITT, polynomial 0x1021, built MSB
/// first but applied in the reflected direction.
const CRC16_TABLE: [u16; 256] = {
    let mut table = [0u16; 256];
    let mut byte = 0;
    while byte < 256 {
        let mut work = (byte as u32) << 8;
        let mut bit = 0;
        while bit < 8 {
            work = if work & 0x8000 == 0 {
                work << 1
            } else {
                (work << 1) ^ 0x1021
            };
            work &= 0xFFFF;
            bit += 1;
        }
        table[byte] = work as u16;
        byte += 1;
    }
    table
};

/// `MakeId`: the 16-bit ID of a name.
#[must_use]
pub fn make_id(name: &[u8]) -> u16 {
    let name = c_str(name);
    let name = &name[..name.len().min(MAKE_ID_MAX_BYTES)];
    name.iter().fold(0xFFFF_u16, |crc, &b| {
        let index = (crc & 0xFF) ^ u16::from(b.to_ascii_lowercase());
        CRC16_TABLE[usize::from(index)] ^ (crc >> 8)
    })
}

/// `MakeLocationID`: `CreateID` of `"%X_%s"` formatted from the base ID and
/// the name, cut to the 255 bytes the C++ buffer holds.
#[must_use]
pub fn make_location_id(base_id: u32, name: &[u8]) -> u32 {
    let mut text = format!("{base_id:X}_").into_bytes();
    text.extend_from_slice(c_str(name));
    text.truncate(LOCATION_TEXT_BYTES - 1);
    create_id(&text)
}

/// `ID_String`: a nickname ID held by value.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct IdString(pub u32);

impl IdString {
    /// `ID_String::IsEmpty`.
    #[must_use]
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// `ID_String::get_string`: `"0x"` and the ID in lower-case hex, as
    /// `_itoa` with radix 16 writes it.
    #[must_use]
    pub fn to_hex_string(self) -> String {
        format!("0x{:x}", self.0)
    }
}

/// `CFLIDMaker::AllocateObjectID`: dynamic object IDs from 1 upwards, with
/// the top two bits cleared.
#[derive(Clone, Debug)]
pub struct ObjectIdAllocator {
    next: u32,
}

impl Default for ObjectIdAllocator {
    fn default() -> Self {
        Self { next: 1 }
    }
}

impl ObjectIdAllocator {
    /// The next ID.
    pub fn allocate(&mut self) -> u32 {
        let id = self.next;
        self.next = id.wrapping_add(1);
        id & 0x3FFF_FFFF
    }
}

/// Sub-object IDs (`SubObjectID` in `common_subobjectid.cpp`).
pub mod sub_object {
    macro_rules! id_maker {
        ($(#[$doc:meta])* $name:ident, $start:expr) => {
            $(#[$doc])*
            #[derive(Clone, Debug)]
            pub struct $name {
                next: u16,
            }

            impl Default for $name {
                fn default() -> Self {
                    Self { next: $start }
                }
            }

            impl $name {
                /// The next ID. The counter is incremented first, so the
                /// first ID is one above the start value.
                pub fn create(&mut self) -> u16 {
                    self.next = self.next.wrapping_add(1);
                    self.next
                }

                /// Back to the start value.
                pub fn reset(&mut self) {
                    self.next = $start;
                }
            }
        };
    }

    id_maker!(
        /// `CollGroupIdMaker`: collision-group IDs, start value 3.
        CollisionGroupIds,
        3
    );
    id_maker!(
        /// `EquipIdMaker`: equipment IDs, start value 0x21.
        EquipmentIds,
        0x21
    );
    id_maker!(
        /// `ShieldIdMaker`: shield IDs, start value 0xFFF0.
        ShieldIds,
        0xFFF0
    );

    /// `IsArchGroupID`: 3 <= id < 0x21.
    #[must_use]
    pub fn is_arch_group_id(id: u16) -> bool {
        (3..0x21).contains(&id)
    }

    /// `IsEquipID`: 0x21 <= id < 0xFFFF. Shield IDs are inside this range.
    #[must_use]
    pub fn is_equip_id(id: u16) -> bool {
        (0x21..0xFFFF).contains(&id)
    }

    /// `IsShieldEquipID`: 0xFFF0 <= id < 0xFFFF.
    #[must_use]
    pub fn is_shield_equip_id(id: u16) -> bool {
        (0xFFF0..0xFFFF).contains(&id)
    }
}

#[cfg(test)]
mod tests {
    use super::sub_object::*;
    use super::*;

    #[test]
    fn create_id_ignores_case_and_sets_the_top_bit() {
        assert_eq!(create_id(b"Li01"), create_id(b"li01"));
        assert_eq!(create_id(b"x") & 0x8000_0000, 0x8000_0000);
        assert_eq!(create_id(b""), 0x8000_0000);
    }

    #[test]
    fn create_id_hashes_only_the_first_255_bytes() {
        let long = vec![b'a'; 300];
        assert_eq!(create_id(&long), create_id(&long[..255]));
        assert_ne!(create_id(&long[..254]), create_id(&long[..255]));
    }

    #[test]
    fn make_id_hashes_only_the_first_127_bytes() {
        let long = vec![b'q'; 200];
        assert_eq!(make_id(&long), make_id(&long[..127]));
        assert_eq!(make_id(b""), 0xFFFF);
    }

    #[test]
    fn crc16_table_starts_like_the_initialised_data() {
        assert_eq!(&CRC16_TABLE[..3], &[0x0000, 0x1021, 0x2042]);
    }

    #[test]
    fn location_id_formats_upper_hex() {
        assert_eq!(make_location_id(0xAB, b"x"), create_id(b"AB_x"));
    }

    #[test]
    fn object_ids_start_at_one_and_drop_the_top_bits() {
        let mut ids = ObjectIdAllocator::default();
        assert_eq!(ids.allocate(), 1);
        let mut high = ObjectIdAllocator { next: 0xC000_0005 };
        assert_eq!(high.allocate(), 5);
    }

    #[test]
    fn sub_object_ids_pre_increment_and_wrap() {
        let mut groups = CollisionGroupIds::default();
        assert_eq!(groups.create(), 4);
        let mut shields = ShieldIds::default();
        for _ in 0..15 {
            shields.create();
        }
        assert_eq!(shields.create(), 0);
        shields.reset();
        assert_eq!(shields.create(), 0xFFF1);
        assert!(is_arch_group_id(3) && !is_arch_group_id(0x21));
        assert!(is_equip_id(0xFFF5) && !is_equip_id(0xFFFF));
        assert!(is_shield_equip_id(0xFFF0) && !is_shield_equip_id(0xFFFF));
    }

    #[test]
    fn id_string_formats_like_itoa_radix_16() {
        assert_eq!(IdString(0x8ABC_0001).to_hex_string(), "0x8abc0001");
        assert!(IdString(0).is_empty());
    }
}
