//! The style registry (`StyleCollection`, `common_renderdisplaylist.cpp`)
//! and the `[Style]` sections of `rich_fonts.ini` that fill it
//! (`LoadRichFontsFromIni`, `menu_fonts_load.cpp`).

use std::collections::BTreeMap;

use neuron_core::hash::eq_ignore_case;
use neuron_core::ini::IniReader;

use crate::text::rdl::{Justify, PackedAttributes, RdlNode, RenderDisplayList, TextAttributes};

/// `StyleCollection`: display lists by 16-bit style ID. A style node
/// executes its style's list; the layout lays that list out in its place.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct StyleCollection {
    styles: BTreeMap<u16, RenderDisplayList>,
    /// What a lookup of an unknown ID gives (the function-local static).
    empty: RenderDisplayList,
}

impl StyleCollection {
    #[must_use]
    pub fn new() -> StyleCollection {
        StyleCollection::default()
    }

    /// `set_style(Id, Rdl)`: the style becomes a clone of every node of
    /// `list`.
    pub fn set_style(&mut self, id: u16, list: &RenderDisplayList) {
        let nodes = list.nodes.iter().map(RdlNode::cloned).collect();
        self.styles.insert(id, RenderDisplayList { nodes });
    }

    /// `set_style(Id, Attrs)`: the style becomes one attribute node that
    /// sets every bit to `attrs`.
    pub fn set_style_attributes(&mut self, id: u16, attrs: PackedAttributes) {
        let node = RdlNode::Attributes(TextAttributes::new(attrs, 0xFFFF_FFFF, 0));
        self.styles
            .insert(id, RenderDisplayList { nodes: vec![node] });
    }

    /// `get_style`: the style's list, or an empty list for an unknown ID
    /// (nothing is inserted).
    #[must_use]
    pub fn get(&self, id: u16) -> &RenderDisplayList {
        self.styles.get(&id).unwrap_or(&self.empty)
    }

    /// The IDs registered, in order.
    pub fn ids(&self) -> impl Iterator<Item = u16> + '_ {
        self.styles.keys().copied()
    }
}

/// The style ID of a `name` value: CRC-16/CCITT seeded `0xFFFF`, no final
/// XOR, over the bytes. The reference's register keeps bits above the low
/// word; they never reach it.
#[must_use]
pub fn style_id(name: &[u8]) -> u16 {
    let mut crc: u32 = 0xFFFF;
    for &byte in name {
        let swapped = ((crc & 0xFF) << 8) | ((crc >> 8) & 0xFF);
        crc = (crc & 0xFFFF_0000) | u32::from(byte);
        crc ^= swapped;
        crc ^= (crc & 0xFF) >> 4;
        crc ^= crc << 12;
        crc ^= (crc & 0xFF) << 5;
    }
    (crc & 0xFFFF) as u16
}

/// The `[Style]` part of `LoadRichFontsFromIni`: one style per section, in
/// file order, as `(id, list)`; a later section with the same ID replaces
/// an earlier one when they are registered.
///
/// A section starts from white, no style bits and an empty mask; `font`
/// puts its value in bits 3 to 7, `bold`, `italic` and `underline` set
/// their bits, `color = r, g, b` the colour bytes, each adding its bits to
/// the mask; `justify = left | center | right` (any case) appends a
/// justification node. The list is those justification nodes, then one
/// attribute node of the attributes under the mask. The ID comes from the
/// last `name` line and is not reset between sections; before the first
/// `name` it is 0 (the original reads an uninitialised stack slot; the
/// reference port starts at 0 too).
#[must_use]
pub fn read_rich_styles(contents: &[u8]) -> Vec<(u16, RenderDisplayList)> {
    let mut out = Vec::new();
    let mut reader = IniReader::new();
    if !reader.open_memory(contents) || !reader.read_header() {
        return out;
    }
    let mut id: u16 = 0;
    loop {
        if reader.is_header(b"Style") {
            let mut packed: u32 = 0xFFFF_FF00;
            let mut mask: u32 = 0;
            let mut list = RenderDisplayList::new();
            let set_flags = |packed: &mut u32, flags: u8| {
                *packed = (*packed & 0xFFFF_FF00) | u32::from(flags);
            };
            while reader.read_value() {
                let flags = (packed & 0xFF) as u8;
                if reader.is_value(b"name") {
                    id = style_id(&reader.value_string());
                } else if reader.is_value(b"font") {
                    let font = reader.value_int(0) as u8;
                    set_flags(&mut packed, (font << 3) ^ (flags & 7));
                    mask |= 0xF8;
                } else if reader.is_value(b"bold") {
                    let value = u8::from(reader.value_bool(0));
                    set_flags(&mut packed, flags ^ ((value ^ flags) & 1));
                    mask |= 1;
                } else if reader.is_value(b"italic") {
                    let value = u8::from(reader.value_bool(0));
                    set_flags(&mut packed, flags ^ (((value << 1) ^ flags) & 2));
                    mask |= 2;
                } else if reader.is_value(b"underline") {
                    let value = u8::from(reader.value_bool(0));
                    set_flags(&mut packed, flags ^ (((value << 2) ^ flags) & 4));
                    mask |= 4;
                } else if reader.is_value(b"color") {
                    let red = u32::from(reader.value_int(0) as u8);
                    let green = u32::from(reader.value_int(1) as u8);
                    let blue = u32::from(reader.value_int(2) as u8);
                    packed = (packed & 0xFF) | (red << 8) | (green << 16) | (blue << 24);
                    mask |= 0xFFFF_FF00;
                } else if reader.is_value(b"justify") {
                    let value = reader.value_string();
                    let justify = if eq_ignore_case(&value, b"left") {
                        Some(Justify::Left)
                    } else if eq_ignore_case(&value, b"center") {
                        Some(Justify::Center)
                    } else if eq_ignore_case(&value, b"right") {
                        Some(Justify::Right)
                    } else {
                        None
                    };
                    if let Some(justify) = justify {
                        list.push(RdlNode::Justify(justify));
                    }
                }
            }
            list.push(RdlNode::Attributes(TextAttributes::new(packed, mask, 0)));
            out.push((id, list));
        }
        if !reader.read_header() {
            break;
        }
    }
    out
}

/// [`read_rich_styles`] registered in order with
/// [`StyleCollection::set_style`].
pub fn apply_rich_styles(contents: &[u8], styles: &mut StyleCollection) {
    for (id, list) in read_rich_styles(contents) {
        styles.set_style(id, &list);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn style_sections_build_lists() {
        let text = b"[TrueType]\nfont = 0, Arial, 10\n[Style]\nname = STYLE_A\nfont = 5\nbold = true\nitalic = false\ncolor = 135, 195, 224\njustify = Center\n[Style]\nunderline = true\njustify = middle\n";
        let styles = read_rich_styles(text);
        assert_eq!(styles.len(), 2);
        let id = style_id(b"STYLE_A");
        assert_eq!(styles[0].0, id);
        assert_eq!(
            styles[0].1.nodes,
            [
                RdlNode::Justify(Justify::Center),
                RdlNode::Attributes(TextAttributes::new(0xE0C3_8729, 0xFFFF_FFFB, 0)),
            ]
        );
        // The second section keeps the first one's ID.
        assert_eq!(styles[1].0, id);
        assert_eq!(
            styles[1].1.nodes,
            [RdlNode::Attributes(TextAttributes::new(0xFFFF_FF04, 4, 0))]
        );
        let mut collection = StyleCollection::new();
        apply_rich_styles(text, &mut collection);
        assert_eq!(collection.get(id).nodes.len(), 1);
        assert!(collection.get(id.wrapping_add(1)).nodes.is_empty());
    }

    #[test]
    fn style_ids_are_crc_ccitt() {
        // CRC-16/CCITT-FALSE of "123456789".
        assert_eq!(style_id(b"123456789"), 0x29B1);
        assert_eq!(style_id(b""), 0xFFFF);
    }
}
