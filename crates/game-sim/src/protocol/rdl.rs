//! Binary rich text: the record stream of `BinaryRDLReader` and
//! `BinaryRDLWriter` (`common_rdlio.cpp`), the chat wire format.
//!
//! Records follow each other: `i32` type, `u32` payload length, payload.
//!
//! | Type | Record | Payload |
//! | --- | --- | --- |
//! | 1 | text attributes | `u32` attributes, `u32` mask (length 8) |
//! | 2 | text | UTF-16 units and a terminating NUL (length `2n + 2`) |
//! | 4 | position | flag byte (bit 0 relative H, bit 1 relative V), `f32` V, `f32` H (length 9) |
//! | 5 | paragraph | none (length 0) |
//! | 6 | style | `u16` style ID (length 2) |
//!
//! The node list itself is the client's (`RenderDisplayList`, P4.12); this
//! module reads and writes the records. Two C++ behaviours carry over: the
//! reader skips every record of length 0, so a paragraph the writer emits
//! is not read back, and an attribute record is read as `attrs & mask`.
//! The C++ reader runs past the input when a record's length passes its
//! end and reads a fixed record's fields past a short payload; this reader
//! stops, or skips the record, and reports failure (divergence D104).

use neuron_core::net::wire::x87_copy_f32;

/// One record.
#[derive(Clone, Debug, PartialEq)]
pub enum RdlRecord {
    /// Type 1: text render attributes (`TRANode`).
    Attributes {
        /// The packed attributes.
        attrs: u32,
        /// The bits the attributes set.
        mask: u32,
    },
    /// Type 2: text, without its terminator.
    Text(Vec<u16>),
    /// Type 4: a position (`PositionNode`).
    Position {
        /// Horizontal position relative (flag bit 0).
        rel_h: bool,
        /// Vertical position relative (flag bit 1).
        rel_v: bool,
        /// The vertical position.
        pos_v: f32,
        /// The horizontal position.
        pos_h: f32,
    },
    /// Type 5: a paragraph break.
    Paragraph,
    /// Type 6: a style (`StyleNode`).
    Style(u16),
}

impl RdlRecord {
    fn size(&self) -> usize {
        match self {
            Self::Attributes { .. } => 0x10,
            Self::Text(units) => units.len() * 2 + 2 + 8,
            Self::Position { .. } => 0x11,
            Self::Paragraph => 8,
            Self::Style(_) => 0xa,
        }
    }
}

fn header(out: &mut Vec<u8>, kind: i32, length: usize) {
    out.extend_from_slice(&kind.to_le_bytes());
    out.extend_from_slice(&u32::try_from(length).unwrap_or(u32::MAX).to_le_bytes());
}

/// `BinaryRDLWriter::write_buffer` into a buffer of `max_size` bytes: the
/// records that fit, and whether all did. The positions go through the
/// x87 stack (a signalling NaN is written quiet).
#[must_use]
pub fn write_records(records: &[RdlRecord], max_size: usize) -> (Vec<u8>, bool) {
    let mut out = Vec::new();
    for record in records {
        if out.len() + record.size() > max_size {
            return (out, false);
        }
        match record {
            RdlRecord::Attributes { attrs, mask } => {
                header(&mut out, 1, 8);
                out.extend_from_slice(&attrs.to_le_bytes());
                out.extend_from_slice(&mask.to_le_bytes());
            }
            RdlRecord::Text(units) => {
                header(&mut out, 2, units.len() * 2 + 2);
                out.extend(units.iter().flat_map(|u| u.to_le_bytes()));
                out.extend_from_slice(&[0, 0]);
            }
            RdlRecord::Position {
                rel_h,
                rel_v,
                pos_v,
                pos_h,
            } => {
                header(&mut out, 4, 9);
                out.push(u8::from(*rel_v) << 1 | u8::from(*rel_h));
                out.extend_from_slice(&x87_copy_f32(pos_v.to_bits()).to_le_bytes());
                out.extend_from_slice(&x87_copy_f32(pos_h.to_bits()).to_le_bytes());
            }
            RdlRecord::Paragraph => header(&mut out, 5, 0),
            RdlRecord::Style(id) => {
                header(&mut out, 6, 2);
                out.extend_from_slice(&id.to_le_bytes());
            }
        }
    }
    (out, true)
}

fn word(bytes: &[u8], at: usize) -> Option<u32> {
    let field = bytes.get(at..at + 4)?;
    Some(u32::from_le_bytes([field[0], field[1], field[2], field[3]]))
}

/// The units of a text payload up to its first NUL (`wcslen`).
fn text_units(payload: &[u8]) -> Vec<u16> {
    payload
        .as_chunks::<2>()
        .0
        .iter()
        .map(|pair| u16::from_le_bytes(*pair))
        .take_while(|unit| *unit != 0)
        .collect()
}

/// Splits `bytes` into `(type, payload)` records; false when a record's
/// length passes the end (the C++ wraps its size and reads on).
fn split(bytes: &[u8]) -> (Vec<(i32, &[u8])>, bool) {
    let mut records = Vec::new();
    let mut rest = bytes;
    while !rest.is_empty() {
        let (Some(kind), Some(length)) = (word(rest, 0), word(rest, 4)) else {
            return (records, false);
        };
        let end = usize::try_from(length).ok().and_then(|l| l.checked_add(8));
        let Some(payload) = end.and_then(|end| rest.get(8..end)) else {
            return (records, false);
        };
        records.push((i32::from_le_bytes(kind.to_le_bytes()), payload));
        rest = &rest[8 + payload.len()..];
    }
    (records, true)
}

/// `BinaryRDLReader::read_buffer`: the records `bytes` hold, and whether
/// every one was understood. Records of length 0 are skipped, an unknown
/// type clears the result and is skipped.
#[must_use]
pub fn read_records(bytes: &[u8]) -> (Vec<RdlRecord>, bool) {
    let (raw, mut ok) = split(bytes);
    let mut records = Vec::new();
    for (kind, payload) in raw {
        if payload.is_empty() {
            continue;
        }
        let record =
            match kind {
                1 => word(payload, 0).zip(word(payload, 4)).map(|(attrs, mask)| {
                    RdlRecord::Attributes {
                        attrs: attrs & mask,
                        mask,
                    }
                }),
                2 => Some(RdlRecord::Text(text_units(payload))),
                4 => (payload.len() >= 9).then(|| RdlRecord::Position {
                    rel_h: payload[0] & 1 != 0,
                    rel_v: payload[0] >> 1 & 1 != 0,
                    pos_v: f32::from_bits(x87_copy_f32(word(payload, 1).unwrap_or(0))),
                    pos_h: f32::from_bits(x87_copy_f32(word(payload, 5).unwrap_or(0))),
                }),
                5 => Some(RdlRecord::Paragraph),
                6 => (payload.len() >= 2)
                    .then(|| RdlRecord::Style(u16::from_le_bytes([payload[0], payload[1]]))),
                _ => None,
            };
        match record {
            Some(record) => records.push(record),
            None => ok = false,
        }
    }
    (records, ok)
}

/// `BinaryRDLReader::extract_text_from_buffer` into a buffer of `max_size`
/// units: the text of every text record, joined and cut to `max_size - 1`
/// units, and whether every record had a type the extractor knows (1 to 5;
/// a style record clears it).
#[must_use]
pub fn extract_text(bytes: &[u8], max_size: usize) -> (Vec<u16>, bool) {
    let (raw, mut ok) = split(bytes);
    let mut text = Vec::new();
    if max_size == 0 {
        return (text, ok);
    }
    for (kind, payload) in raw {
        if payload.is_empty() {
            continue;
        }
        match kind {
            2 => text.extend(text_units(payload)),
            1 | 3..=5 => {}
            _ => ok = false,
        }
    }
    text.truncate(max_size - 1);
    (text, ok)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn write_and_read() {
        let records = vec![
            RdlRecord::Attributes {
                attrs: 0xff,
                mask: 0x0f,
            },
            RdlRecord::Text(vec![0x48, 0x69]),
            RdlRecord::Position {
                rel_h: true,
                rel_v: false,
                pos_v: 1.0,
                pos_h: -2.0,
            },
            RdlRecord::Paragraph,
            RdlRecord::Style(3),
        ];
        let (bytes, ok) = write_records(&records, 1000);
        assert!(ok);
        assert_eq!(bytes.len(), 0x10 + 14 + 0x11 + 8 + 0xa);
        let (back, ok) = read_records(&bytes);
        assert!(ok);
        assert_eq!(back.len(), 4, "the paragraph is not read back");
        assert_eq!(
            back[0],
            RdlRecord::Attributes {
                attrs: 0x0f,
                mask: 0x0f
            }
        );
        let (text, ok) = extract_text(&bytes, 10);
        assert_eq!(text, [0x48, 0x69]);
        assert!(!ok, "a style record clears the extractor's result");
        let (part, ok) = write_records(&records, 0x20);
        assert!(!ok);
        assert_eq!(part.len(), 0x10 + 14);
    }

    #[test]
    fn bad_lengths_stop() {
        let (records, ok) = read_records(&[2, 0, 0, 0, 0xff, 0, 0, 0, 0x41, 0]);
        assert!(records.is_empty());
        assert!(!ok);
        let (records, ok) = read_records(&[9, 0, 0, 0, 1, 0, 0, 0, 0]);
        assert!(records.is_empty());
        assert!(!ok);
    }
}
