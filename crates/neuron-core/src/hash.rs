//! Name CRC and ASCII case-insensitive comparison (work package P1.1).
//!
//! Ported from `NEURONCOMPONENTS_CRC` and the exported `stricmp` in
//! `CppSource/neuroncomponents/neuroncomponents.cpp`. Every function takes
//! the bytes of a C string and stops at the first NUL byte, as the C++ does.

use core::cmp::Ordering;

/// The C string view of `bytes`: everything before the first NUL byte.
#[must_use]
pub fn c_str(bytes: &[u8]) -> &[u8] {
    match bytes.iter().position(|&b| b == 0) {
        Some(end) => &bytes[..end],
        None => bytes,
    }
}

/// The engine's CRC table. It is not the IEEE table: the exporter hashed
/// mesh and material names with these values.
const CRC_TABLE: [u32; 256] = [
    0x0000_0000,
    0x0907_3096,
    0x120E_612C,
    0x1B09_51BA,
    0xFF6D_C419,
    0xF66A_F48F,
    0xED63_A535,
    0xE464_95A3,
    0xFEDB_8832,
    0xF7DC_B8A4,
    0xECD5_E91E,
    0xE5D2_D988,
    0x01B6_4C2B,
    0x08B1_7CBD,
    0x13B8_2D07,
    0x1ABF_1D91,
    0xFDB7_1064,
    0xF4B0_20F2,
    0xEFB9_7148,
    0xE6BE_41DE,
    0x02DA_D47D,
    0x0BDD_E4EB,
    0x10D4_B551,
    0x19D3_85C7,
    0x036C_9856,
    0x0A6B_A8C0,
    0x1162_F97A,
    0x1865_C9EC,
    0xFC01_5C4F,
    0xF506_6CD9,
    0xEE0F_3D63,
    0xE708_0DF5,
    0xFB6E_20C8,
    0xF269_105E,
    0xE960_41E4,
    0xE067_7172,
    0x0403_E4D1,
    0x0D04_D447,
    0x160D_85FD,
    0x1F0A_B56B,
    0x05B5_A8FA,
    0x0CB2_986C,
    0x17BB_C9D6,
    0x1EBC_F940,
    0xFAD8_6CE3,
    0xF3DF_5C75,
    0xE8D6_0DCF,
    0xE1D1_3D59,
    0x06D9_30AC,
    0x0FDE_003A,
    0x14D7_5180,
    0x1DD0_6116,
    0xF9B4_F4B5,
    0xF0B3_C423,
    0xEBBA_9599,
    0xE2BD_A50F,
    0xF802_B89E,
    0xF105_8808,
    0xEA0C_D9B2,
    0xE30B_E924,
    0x076F_7C87,
    0x0E68_4C11,
    0x1561_1DAB,
    0x1C66_2D3D,
    0xF6DC_4190,
    0xFFDB_7106,
    0xE4D2_20BC,
    0xEDD5_102A,
    0x09B1_8589,
    0x00B6_B51F,
    0x1BBF_E4A5,
    0x12B8_D433,
    0x0807_C9A2,
    0x0100_F934,
    0x1A09_A88E,
    0x130E_9818,
    0xF76A_0DBB,
    0xFE6D_3D2D,
    0xE564_6C97,
    0xEC63_5C01,
    0x0B6B_51F4,
    0x026C_6162,
    0x1965_30D8,
    0x1062_004E,
    0xF406_95ED,
    0xFD01_A57B,
    0xE608_F4C1,
    0xEF0F_C457,
    0xF5B0_D9C6,
    0xFCB7_E950,
    0xE7BE_B8EA,
    0xEEB9_887C,
    0x0ADD_1DDF,
    0x03DA_2D49,
    0x18D3_7CF3,
    0x11D4_4C65,
    0x0DB2_6158,
    0x04B5_51CE,
    0x1FBC_0074,
    0x16BB_30E2,
    0xF2DF_A541,
    0xFBD8_95D7,
    0xE0D1_C46D,
    0xE9D6_F4FB,
    0xF369_E96A,
    0xFA6E_D9FC,
    0xE167_8846,
    0xE860_B8D0,
    0x0C04_2D73,
    0x0503_1DE5,
    0x1E0A_4C5F,
    0x170D_7CC9,
    0xF005_713C,
    0xF902_41AA,
    0xE20B_1010,
    0xEB0C_2086,
    0x0F68_B525,
    0x066F_85B3,
    0x1D66_D409,
    0x1461_E49F,
    0x0EDE_F90E,
    0x07D9_C998,
    0x1CD0_9822,
    0x15D7_A8B4,
    0xF1B3_3D17,
    0xF8B4_0D81,
    0xE3BD_5C3B,
    0xEABA_6CAD,
    0xEDB8_8320,
    0xE4BF_B3B6,
    0xFFB6_E20C,
    0xF6B1_D29A,
    0x12D5_4739,
    0x1BD2_77AF,
    0x00DB_2615,
    0x09DC_1683,
    0x1363_0B12,
    0x1A64_3B84,
    0x016D_6A3E,
    0x086A_5AA8,
    0xEC0E_CF0B,
    0xE509_FF9D,
    0xFE00_AE27,
    0xF707_9EB1,
    0x100F_9344,
    0x1908_A3D2,
    0x0201_F268,
    0x0B06_C2FE,
    0xEF62_575D,
    0xE665_67CB,
    0xFD6C_3671,
    0xF46B_06E7,
    0xEED4_1B76,
    0xE7D3_2BE0,
    0xFCDA_7A5A,
    0xF5DD_4ACC,
    0x11B9_DF6F,
    0x18BE_EFF9,
    0x03B7_BE43,
    0x0AB0_8ED5,
    0x16D6_A3E8,
    0x1FD1_937E,
    0x04D8_C2C4,
    0x0DDF_F252,
    0xE9BB_67F1,
    0xE0BC_5767,
    0xFBB5_06DD,
    0xF2B2_364B,
    0xE80D_2BDA,
    0xE10A_1B4C,
    0xFA03_4AF6,
    0xF304_7A60,
    0x1760_EFC3,
    0x1E67_DF55,
    0x056E_8EEF,
    0x0C69_BE79,
    0xEB61_B38C,
    0xE266_831A,
    0xF96F_D2A0,
    0xF068_E236,
    0x140C_7795,
    0x1D0B_4703,
    0x0602_16B9,
    0x0F05_262F,
    0x15BA_3BBE,
    0x1CBD_0B28,
    0x07B4_5A92,
    0x0EB3_6A04,
    0xEAD7_FFA7,
    0xE3D0_CF31,
    0xF8D9_9E8B,
    0xF1DE_AE1D,
    0x1B64_C2B0,
    0x1263_F226,
    0x096A_A39C,
    0x006D_930A,
    0xE409_06A9,
    0xED0E_363F,
    0xF607_6785,
    0xFF00_5713,
    0xE5BF_4A82,
    0xECB8_7A14,
    0xF7B1_2BAE,
    0xFEB6_1B38,
    0x1AD2_8E9B,
    0x13D5_BE0D,
    0x08DC_EFB7,
    0x01DB_DF21,
    0xE6D3_D2D4,
    0xEFD4_E242,
    0xF4DD_B3F8,
    0xFDDA_836E,
    0x19BE_16CD,
    0x10B9_265B,
    0x0BB0_77E1,
    0x02B7_4777,
    0x1808_5AE6,
    0x110F_6A70,
    0x0A06_3BCA,
    0x0301_0B5C,
    0xE765_9EFF,
    0xEE62_AE69,
    0xF56B_FFD3,
    0xFC6C_CF45,
    0xE00A_E278,
    0xE90D_D2EE,
    0xF204_8354,
    0xFB03_B3C2,
    0x1F67_2661,
    0x1660_16F7,
    0x0D69_474D,
    0x046E_77DB,
    0x1ED1_6A4A,
    0x17D6_5ADC,
    0x0CDF_0B66,
    0x05D8_3BF0,
    0xE1BC_AE53,
    0xE8BB_9EC5,
    0xF3B2_CF7F,
    0xFAB5_FFE9,
    0x1DBD_F21C,
    0x14BA_C28A,
    0x0FB3_9330,
    0x06B4_A3A6,
    0xE2D0_3605,
    0xEBD7_0693,
    0xF0DE_5729,
    0xF9D9_67BF,
    0xE366_7A2E,
    0xEA61_4AB8,
    0xF168_1B02,
    0xF86F_2B94,
    0x1C0B_BE37,
    0x150C_8EA1,
    0x0E05_DF1B,
    0x0702_EF8D,
];

/// The fold the CRC applies to each byte. Bytes 0x80 and above index the
/// fold table with a signed char, so they read the last 128 bytes of the
/// CRC table that precedes it (`FoldCrcChar`).
fn fold_crc_byte(byte: u8) -> u32 {
    if byte >= 0x80 {
        let offset = 0x300 + u32::from(byte);
        (CRC_TABLE[(offset >> 2) as usize] >> ((offset & 3) * 8)) & 0xFF
    } else {
        u32::from(byte.to_ascii_lowercase())
    }
}

fn step_crc(crc: u32, byte: u8) -> u32 {
    let index = fold_crc_byte(byte) ^ (crc & 0xFF);
    CRC_TABLE[index as usize] ^ (crc >> 8)
}

/// `NEURONCOMPONENTS_CRC::GetCRC32(const char*)`: the case-folded name CRC.
#[must_use]
pub fn name_crc32(name: &[u8]) -> u32 {
    !c_str(name)
        .iter()
        .fold(0xFFFF_FFFF, |crc, &b| step_crc(crc, b))
}

/// `GetContinuedCRC32(unsigned long, const char*)`: continues a previous
/// result over more bytes. `continued_name_crc32(0, s) == name_crc32(s)`.
#[must_use]
pub fn continued_name_crc32(initial: u32, name: &[u8]) -> u32 {
    !c_str(name)
        .iter()
        .fold(!initial, |crc, &b| step_crc(crc, b))
}

/// `GetContinuedCRC32(unsigned long, char)`: continues over one byte. A NUL
/// byte is hashed like any other here.
#[must_use]
pub fn continued_name_crc32_byte(initial: u32, byte: u8) -> u32 {
    !step_crc(!initial, byte)
}

/// The exported `stricmp` and the CRT `_stricmp` in the "C" locale: bytes
/// are compared unsigned after folding `A`..=`Z` to lower case. The end of a
/// slice counts as a NUL byte.
#[must_use]
pub fn stricmp(left: &[u8], right: &[u8]) -> Ordering {
    let left = c_str(left);
    let right = c_str(right);
    let mut index = 0;
    loop {
        let l = left.get(index).map_or(0, u8::to_ascii_lowercase);
        let r = right.get(index).map_or(0, u8::to_ascii_lowercase);
        if l != r || l == 0 {
            return l.cmp(&r);
        }
        index += 1;
    }
}

/// `stricmp(left, right) == 0`.
#[must_use]
pub fn eq_ignore_case(left: &[u8], right: &[u8]) -> bool {
    stricmp(left, right) == Ordering::Equal
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn continued_crc_from_zero_equals_plain_crc() {
        for name in [&b""[..], b"li_elite", b"\xE9t\xE9", b"Sector01"] {
            assert_eq!(continued_name_crc32(0, name), name_crc32(name));
        }
    }

    #[test]
    fn crc_folds_ascii_case_only() {
        assert_eq!(name_crc32(b"ABC"), name_crc32(b"abc"));
        assert_ne!(name_crc32(b"\xC9"), name_crc32(b"\xE9"));
    }

    #[test]
    fn byte_steps_compose() {
        let whole = name_crc32(b"ab");
        let stepped = continued_name_crc32_byte(continued_name_crc32_byte(0, b'a'), b'b');
        assert_eq!(whole, stepped);
    }

    #[test]
    fn stricmp_orders_like_lower_case_bytes() {
        assert_eq!(stricmp(b"ABC", b"abc"), Ordering::Equal);
        assert_eq!(stricmp(b"_", b"A"), Ordering::Less);
        assert_eq!(stricmp(b"ab", b"abc"), Ordering::Less);
        assert_eq!(stricmp(b"\xE9", b"z"), Ordering::Greater);
        assert_eq!(stricmp(b"ab\0x", b"AB"), Ordering::Equal);
    }
}
