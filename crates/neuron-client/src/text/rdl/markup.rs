//! RDLX, the XML markup of rich text: the reader of
//! `XMLReader::read_buffer` with its content handler (`ParseRdlxTag`,
//! `ParseRdlxEndElement`, `ParseRdlxCharacters` in
//! `CppSource/Common/common_rdlio.cpp`), and a writer.
//!
//! The cards use seven tags; the reader knows nine:
//!
//! | Tag | Node | Attributes |
//! | --- | --- | --- |
//! | `RDL` | none | |
//! | `TRA` | [`RdlNode::Attributes`] | `mask`, `def`, `data` (numbers), `color` (`#RRGGBB` with the two digits of each channel read swapped, a name, a number minus one, or `default`), `font` (a 1-based index or `default`), `bold`, `italic`, `underline` (`true`, `false` or `default`) |
//! | `TEXT` | an empty [`RdlNode::Text`] the character data appends to | |
//! | `PARA` | [`RdlNode::Paragraph`] | |
//! | `POS` | [`RdlNode::Position`] | `h`, `v` (integers), `relH`, `relV` (booleans) |
//! | `STYLE` | [`RdlNode::Style`] | `id` |
//! | `JUST` | [`RdlNode::Justify`] | `loc`: `c` centre, `r` right, else left |
//! | `PUSH`, `POP` | [`RdlNode::AttribStack`] | |
//!
//! Names and keywords are compared as the reference compares them,
//! `wcsncmp` over the length of the text read: a tag, attribute name or
//! value matches a keyword when it is a prefix of it (`P` is `PARA`, an
//! empty value is `default`). Numbers are `wcstol`, or `wcstoul` in base
//! 16 after `0x`. Character data appends to the last node only while a
//! `TEXT` element is open and that node is a [`RdlNode::Text`].
//!
//! The reference's `XMLWriter::write_buffer` always fails; [`write`] is the
//! port's own, for turning node lists back into card markup.

use std::fmt;

use neuron_core::msvcrt::{wcstol, wcstoul, wide_digit};

use super::xml::{self, Attribute, Handler};
use super::{
    BOLD_BIT, COLOR_BITS, FONT_BITS, ITALIC_BIT, Justify, Position, RdlNode, RenderDisplayList,
    StackOp, TextAttributes, UNDERLINE_BIT,
};

/// The colour names `color` takes (`GRdlxNamedColors`), as `0x00BBGGRR`.
const NAMED_COLORS: [(&str, u32); 8] = [
    ("gray", 0x0080_8080),
    ("blue", 0x00E0_4848),
    ("green", 0x001D_BF3B),
    ("aqua", 0x00E0_C387),
    ("red", 0x001D_1DBF),
    ("fuchsia", 0x00C2_0088),
    ("yellow", 0x0052_EAF5),
    ("white", 0x00FF_FFFF),
];

/// The colour a bad `color` value gets.
const WHITE: u32 = 0x00FF_FFFF;

/// `wcsncmp(text, keyword, text.len()) == 0`: `text` is a prefix of
/// `keyword`.
fn matches(text: &[u16], keyword: &str) -> bool {
    text.len() <= keyword.len() && keyword.encode_utf16().zip(text).all(|(k, &t)| k == t)
}

/// `iswdigit`, taken as the decimal digits `_wchartodigit` knows; above
/// U+00FF the runtime asks `GetStringTypeW`, whose digit class is wider
/// (divergence D233).
fn is_digit(unit: u16) -> bool {
    wide_digit(unit).is_some()
}

/// The first unit of a value; the reference reads the unit after an empty
/// value, which is taken as the end of the text.
fn first(value: &[u16]) -> u16 {
    value.first().copied().unwrap_or(0)
}

/// `ParseRdlxNumber`: `wcstoul` in base 16 when the value is longer than
/// two units and starts with `0x`, else `wcstol` in base 10.
///
/// The reference parses a copy of the value and the unit after it; that
/// unit is the end of the value here (the reference's own comment calls
/// the rest undefined behaviour; divergence D233).
fn number(value: &[u16]) -> u32 {
    if value.len() > 2 && value[0] == u16::from(b'0') && value[1] == u16::from(b'x') {
        wcstoul(value, 16)
    } else {
        u32::from_ne_bytes(wcstol(value, 10).to_ne_bytes())
    }
}

/// `ParseRdlxBool`: a prefix of `true` is true; anything else is false.
/// The reference also writes a warning to its log when the value is not a
/// prefix of `false`, as it does for a bad colour; the port does not
/// (divergence D233).
fn boolean(value: &[u16]) -> bool {
    matches(value, "true")
}

/// `RdlxHexNibble`: a digit's value (`iswdigit`, then the unit minus
/// `'0'`), a letter `A`..`F` or `a`..`f`, else 0.
fn hex_nibble(unit: u16) -> u32 {
    let digit = is_digit(unit);
    let unit = u32::from(unit);
    if digit {
        unit.wrapping_sub(0x30)
    } else if (0x41..0x47).contains(&unit) {
        unit - 0x37
    } else if (0x61..0x67).contains(&unit) {
        unit - 0x57
    } else {
        0
    }
}

/// The content handler (`RDLXParser`) over a list.
struct Sink<'a> {
    list: &'a mut RenderDisplayList,
    /// `bTextNodeOpen`: a `TEXT` element has made its node.
    text_open: bool,
}

impl Sink<'_> {
    fn attributes(attributes: &[Attribute]) -> TextAttributes {
        let mut mask = 0u32;
        let mut default_bits = 0u32;
        let mut attrs = 0u32;
        for Attribute { local, value } in attributes {
            let explicit = !matches(value, "default");
            if matches(local, "mask") {
                mask = number(value);
            } else if matches(local, "def") {
                default_bits = number(value);
            } else if matches(local, "data") {
                attrs = number(value);
            } else if matches(local, "color") {
                let color = if first(value) == u16::from(b'#') {
                    if value.len() == 7 {
                        let channel = |low: usize, high: usize| {
                            (hex_nibble(value[high])
                                .wrapping_mul(0x10)
                                .wrapping_add(hex_nibble(value[low])))
                                & 0xFF
                        };
                        Some(channel(1, 2) | channel(3, 4) << 8 | channel(5, 6) << 16)
                    } else {
                        Some(WHITE)
                    }
                } else if !explicit {
                    None
                } else if !is_digit(first(value)) {
                    Some(
                        NAMED_COLORS
                            .iter()
                            .find(|(name, _)| matches(value, name))
                            .map_or(WHITE, |&(_, color)| color),
                    )
                } else {
                    Some(number(value).wrapping_sub(1))
                };
                match color {
                    Some(color) => {
                        attrs = (attrs & 0xFF) | color << 8;
                        mask |= COLOR_BITS;
                    }
                    None => {
                        mask |= COLOR_BITS;
                        default_bits |= COLOR_BITS;
                    }
                }
            } else if matches(local, "font") {
                if explicit {
                    let [font, ..] = number(value).to_le_bytes();
                    let [old, ..] = attrs.to_le_bytes();
                    let byte = font.wrapping_mul(8).wrapping_sub(1) ^ (!old & 7);
                    attrs = (attrs & !0xFF) | u32::from(byte);
                    mask |= FONT_BITS;
                } else {
                    mask |= FONT_BITS;
                    default_bits |= FONT_BITS;
                }
            } else {
                let bit = if matches(local, "bold") {
                    BOLD_BIT
                } else if matches(local, "italic") {
                    ITALIC_BIT
                } else if matches(local, "underline") {
                    UNDERLINE_BIT
                } else {
                    continue;
                };
                if explicit {
                    if boolean(value) {
                        attrs |= bit;
                    } else {
                        attrs &= !bit;
                    }
                } else {
                    default_bits |= bit;
                }
                mask |= bit;
            }
        }
        TextAttributes::new(attrs, mask, default_bits)
    }

    fn position(attributes: &[Attribute]) -> Position {
        let mut position = Position {
            rel_h: false,
            rel_v: false,
            pos_v: 0.0,
            pos_h: 0.0,
        };
        for Attribute { local, value } in attributes {
            // `FILD` of the `wcstol` result, `FSTP` to a float.
            if matches(local, "v") {
                position.pos_v = wcstol(value, 10) as f32;
            } else if matches(local, "h") {
                position.pos_h = wcstol(value, 10) as f32;
            } else if matches(local, "relV") {
                position.rel_v = boolean(value);
            } else if matches(local, "relH") {
                position.rel_h = boolean(value);
            }
        }
        position
    }
}

impl Handler for Sink<'_> {
    fn start_element(&mut self, local: &[u16], attributes: &[Attribute]) {
        let node = if matches(local, "RDL") {
            return;
        } else if matches(local, "TRA") {
            RdlNode::Attributes(Self::attributes(attributes))
        } else if matches(local, "TEXT") {
            if self.text_open {
                return;
            }
            self.text_open = true;
            RdlNode::Text(Vec::new())
        } else if matches(local, "PARA") {
            RdlNode::Paragraph
        } else if matches(local, "POS") {
            RdlNode::Position(Self::position(attributes))
        } else if matches(local, "STYLE") {
            let mut id = 0u16;
            for Attribute { local, value } in attributes {
                if matches(local, "id") {
                    let [low, high, ..] = number(value).to_le_bytes();
                    id = u16::from_le_bytes([low, high]);
                }
            }
            RdlNode::Style(id)
        } else if matches(local, "JUST") {
            let mut justify = Justify::Left;
            for Attribute { local, value } in attributes {
                if matches(local, "loc") {
                    justify = match u8::try_from(first(value)) {
                        Ok(b'C' | b'c') => Justify::Center,
                        Ok(b'R' | b'r') => Justify::Right,
                        _ => Justify::Left,
                    };
                }
            }
            RdlNode::Justify(justify)
        } else if matches(local, "PUSH") {
            RdlNode::AttribStack(StackOp::Push)
        } else if matches(local, "POP") {
            RdlNode::AttribStack(StackOp::Pop)
        } else {
            return;
        };
        self.list.push(node);
    }

    fn end_element(&mut self, local: &[u16]) {
        // `RDL` and `TRA` are tried first: `T` matches `TRA` and never
        // reaches `TEXT`.
        if matches(local, "RDL") || matches(local, "TRA") {
            return;
        }
        if matches(local, "TEXT") {
            self.text_open = false;
        }
    }

    fn characters(&mut self, text: &[u16]) {
        if !self.text_open {
            return;
        }
        if let Some(RdlNode::Text(run)) = self.list.nodes.last_mut() {
            run.extend_from_slice(text);
        }
    }
}

/// `XMLReader::read_buffer`: appends the nodes `text` describes to `list`;
/// whether `text` is well-formed XML. The nodes read before an error stay.
pub fn read(list: &mut RenderDisplayList, text: &[u16]) -> bool {
    let mut sink = Sink {
        list,
        text_open: false,
    };
    xml::parse(text, &mut sink).is_ok()
}

/// [`read`] of UTF-8 text.
pub fn read_str(list: &mut RenderDisplayList, text: &str) -> bool {
    let units: Vec<u16> = text.encode_utf16().collect();
    read(list, &units)
}

/// A node [`write`] has no markup for.
#[derive(Clone, Debug, PartialEq)]
pub struct WriteError {
    /// The index of the node in the list.
    pub index: usize,
    /// Why it cannot be written.
    pub reason: &'static str,
}

impl fmt::Display for WriteError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "node {}: {}", self.index, self.reason)
    }
}

impl std::error::Error for WriteError {}

/// Writes `list` as an `<RDL>` document that [`read`] turns back into the
/// same nodes, in the style of the cards.
///
/// A [`RdlNode::TextPtr`] is written as text and reads back as a
/// [`RdlNode::Text`].
///
/// # Errors
///
/// A node with no markup that reads back to it: [`RdlNode::Null`],
/// images, clip rectangles, references, the clear and unnamed stack
/// operations, unnamed justifications, positions that are not integers
/// (or are -0), and text holding a character XML cannot carry.
pub fn write(list: &RenderDisplayList) -> Result<String, WriteError> {
    let mut out = String::from("<RDL>");
    for (index, node) in list.nodes.iter().enumerate() {
        let fail = |reason| WriteError { index, reason };
        match node {
            RdlNode::Attributes(attributes) => write_attributes(&mut out, attributes),
            RdlNode::Text(text) | RdlNode::TextPtr(text) => {
                out.push_str("<TEXT>");
                write_text(&mut out, text).map_err(fail)?;
                out.push_str("</TEXT>");
            }
            RdlNode::Paragraph => out.push_str("<PARA/>"),
            RdlNode::AttribStack(StackOp::Push) => out.push_str("<PUSH/>"),
            RdlNode::AttribStack(StackOp::Pop) => out.push_str("<POP/>"),
            RdlNode::Justify(justify) => {
                let loc = match justify {
                    Justify::Left => "l",
                    Justify::Center => "c",
                    Justify::Right => "r",
                    Justify::Other(_) => return Err(fail("no markup for this justification")),
                };
                out.push_str(&format!("<JUST loc=\"{loc}\"/>"));
            }
            RdlNode::Position(position) => {
                let h =
                    integer(position.pos_h).ok_or_else(|| fail("position is not an integer"))?;
                let v =
                    integer(position.pos_v).ok_or_else(|| fail("position is not an integer"))?;
                out.push_str(&format!(
                    "<POS h=\"{h}\" v=\"{v}\" relH=\"{}\" relV=\"{}\"/>",
                    position.rel_h, position.rel_v
                ));
            }
            RdlNode::Style(id) => out.push_str(&format!("<STYLE id=\"{id}\"/>")),
            RdlNode::AttribStack(_) => return Err(fail("no markup for this stack operation")),
            RdlNode::Null => return Err(fail("no markup for a null node")),
            RdlNode::Image(_) => return Err(fail("no markup for an image")),
            RdlNode::Clip(_) => return Err(fail("no markup for a clip rectangle")),
            RdlNode::Ref(_) => return Err(fail("no markup for a list reference")),
        }
    }
    out.push_str("</RDL>");
    Ok(out)
}

/// The integer `(float)wcstol(text)` turns into `value`.
fn integer(value: f32) -> Option<i64> {
    if !value.is_finite() || value.fract() != 0.0 || value.to_bits() == (-0.0f32).to_bits() {
        return None;
    }
    let wide = f64::from(value);
    // Above i32::MAX, wcstol saturates; 2^31 is the only float there that
    // a saturated value rounds to.
    if wide == 2_147_483_648.0 {
        return Some(i64::from(i32::MAX));
    }
    (f64::from(i32::MIN)..=f64::from(i32::MAX))
        .contains(&wide)
        .then(|| neuron_core::num::trunc_to_i64(wide))
}

/// The attributes in the cards' style (`bold`, `italic`, `underline`,
/// `font`, `color`, each a value or `default`) when that reads back
/// exactly, else as `data`, `mask` and `def` numbers.
fn write_attributes(out: &mut String, attributes: &TextAttributes) {
    let TextAttributes {
        attrs,
        mask,
        default_mask,
    } = *attributes;
    let groups = [BOLD_BIT, ITALIC_BIT, UNDERLINE_BIT, FONT_BITS, COLOR_BITS];
    let friendly = groups.iter().all(|&group| {
        let m = mask & group;
        let d = default_mask & group;
        (m == 0 || m == group) && (d == 0 || (d == group && m == group && attrs & group == 0))
    });
    out.push_str("<TRA");
    if friendly {
        for (group, name) in [
            (BOLD_BIT, "bold"),
            (ITALIC_BIT, "italic"),
            (UNDERLINE_BIT, "underline"),
            (FONT_BITS, "font"),
            (COLOR_BITS, "color"),
        ] {
            if mask & group == 0 {
                continue;
            }
            let value = if default_mask & group != 0 {
                "default".to_owned()
            } else if group == FONT_BITS {
                (((attrs & FONT_BITS) >> 3) + 1).to_string()
            } else if group == COLOR_BITS {
                // Each channel's two digits are read swapped.
                let [_, r, g, b] = attrs.to_le_bytes();
                [r, g, b].iter().fold(String::from("#"), |text, c| {
                    text + &format!("{:X}{:X}", c & 0xF, c >> 4)
                })
            } else {
                (attrs & group != 0).to_string()
            };
            out.push_str(&format!(" {name}=\"{value}\""));
        }
    } else {
        out.push_str(&format!(
            " data=\"0x{attrs:x}\" mask=\"0x{mask:x}\" def=\"0x{default_mask:x}\""
        ));
    }
    out.push_str("/>");
}

/// Character data with `&`, `<` and `>` escaped, and a carriage return as
/// a character reference so it is not read as a line end.
fn write_text(out: &mut String, text: &[u16]) -> Result<(), &'static str> {
    for c in char::decode_utf16(text.iter().copied()) {
        let c = c.map_err(|_| "text holds an unpaired surrogate")?;
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '\r' => out.push_str("&#13;"),
            '\t' | '\n' | '\u{20}'..='\u{D7FF}' | '\u{E000}'..='\u{FFFD}' | '\u{10000}'.. => {
                out.push(c);
            }
            _ => return Err("text holds a character XML cannot carry"),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn read_nodes(text: &str) -> (bool, Vec<RdlNode>) {
        let mut list = RenderDisplayList::new();
        let ok = read_str(&mut list, text);
        (ok, list.nodes)
    }

    fn units(text: &str) -> Vec<u16> {
        text.encode_utf16().collect()
    }

    fn tra(attrs: u32, mask: u32, default_mask: u32) -> RdlNode {
        RdlNode::Attributes(TextAttributes::new(attrs, mask, default_mask))
    }

    #[test]
    fn reads_the_card_tags() {
        let (ok, nodes) = read_nodes(
            "<RDL><PUSH/><JUST loc=\"c\"/><TRA bold=\"true\" color=\"#FF0000\"/>\
             <TEXT>A &lt;b&gt; c</TEXT><PARA/><TRA font=\"9\"/><TRA color=\"default\"/><POP/></RDL>",
        );
        assert!(ok);
        assert_eq!(
            nodes,
            [
                RdlNode::AttribStack(StackOp::Push),
                RdlNode::Justify(Justify::Center),
                tra(0xFF01, COLOR_BITS | BOLD_BIT, 0),
                RdlNode::Text(units("A <b> c")),
                RdlNode::Paragraph,
                tra(0x40, FONT_BITS, 0),
                tra(0, COLOR_BITS, COLOR_BITS),
                RdlNode::AttribStack(StackOp::Pop),
            ]
        );
    }

    #[test]
    fn compares_like_wcsncmp() {
        // `P` is PARA, `T` is TRA, `d` is `def`, an empty value is `default`.
        let (ok, nodes) = read_nodes("<R><P/><T d=\"0x10\" b=\"\"/><TE>x</TE><PO r=\"\"/></R>");
        assert!(ok);
        assert_eq!(
            nodes,
            [
                RdlNode::Paragraph,
                tra(0, BOLD_BIT, 0x10 | BOLD_BIT),
                RdlNode::Text(units("x")),
                RdlNode::Position(Position {
                    rel_h: false,
                    rel_v: true,
                    pos_v: 0.0,
                    pos_h: 0.0
                }),
            ]
        );
        // A name longer than the keyword does not match.
        assert_eq!(read_nodes("<PARAS/>").1, []);
    }

    #[test]
    fn text_appends_only_to_its_open_node() {
        let (_, nodes) =
            read_nodes("<RDL><TEXT>a<PARA/>b</TEXT>c<TEXT><TEXT>d</TEXT>e</TEXT></RDL>");
        assert_eq!(
            nodes,
            [
                RdlNode::Text(units("a")),
                RdlNode::Paragraph,
                RdlNode::Text(units("d")),
            ]
        );
        // `</T>` matches TRA and does not close the text.
        let (_, nodes) = read_nodes("<RDL><TEXT><T>a</T>b</TEXT></RDL>");
        assert_eq!(nodes, [RdlNode::Text(Vec::new()), tra(0, 0, 0)]);
    }

    #[test]
    fn colours_fonts_and_numbers() {
        let color = |value: &str| match &read_nodes(&format!("<TRA color=\"{value}\"/>")).1[0] {
            RdlNode::Attributes(a) => a.attrs >> 8,
            other => panic!("{other:?}"),
        };
        assert_eq!(color("#123456"), 0x65_43_21);
        assert_eq!(color("#12345"), WHITE);
        assert_eq!(color("g"), 0x80_8080);
        assert_eq!(color("purple"), WHITE);
        assert_eq!(color("256"), 0xFF);
        assert_eq!(color("0"), 0xFF_FFFF);
        let (_, nodes) = read_nodes("<TRA bold=\"true\" font=\"0\" data=\"0x1ff\" mask=\"-1\"/>");
        // data replaces the bold bit and the font byte; the font byte keeps
        // bits 0..=2 of what it replaces.
        assert_eq!(nodes, [tra(0x1FF, u32::MAX, 0)]);
        let (_, nodes) = read_nodes("<TRA bold=\"true\" font=\"33\"/>");
        assert_eq!(nodes, [tra(0x01, BOLD_BIT | FONT_BITS, 0)]);
        let (_, nodes) =
            read_nodes("<RDL><STYLE id=\"0x10005\"/><POS h=\"-3\" v=\"12x\" relH=\"yes\"/></RDL>");
        assert_eq!(
            nodes,
            [
                RdlNode::Style(5),
                RdlNode::Position(Position {
                    rel_h: false,
                    rel_v: false,
                    pos_v: 12.0,
                    pos_h: -3.0
                }),
            ]
        );
    }

    #[test]
    fn writes_what_it_reads() {
        let list = RenderDisplayList {
            nodes: vec![
                RdlNode::AttribStack(StackOp::Push),
                tra(0xFF01, COLOR_BITS | BOLD_BIT, 0),
                tra(0x40, FONT_BITS | UNDERLINE_BIT, 0),
                tra(0, COLOR_BITS | ITALIC_BIT, COLOR_BITS),
                tra(0x1234_5678, 0xFFFF_0F0F, 0x0F00_0000),
                RdlNode::Text(units("a & <b>\r\n\u{1F600}")),
                RdlNode::Text(Vec::new()),
                RdlNode::Position(Position {
                    rel_h: true,
                    rel_v: false,
                    pos_v: -7.0,
                    pos_h: 2_147_483_648.0,
                }),
                RdlNode::Style(65535),
                RdlNode::Justify(Justify::Right),
                RdlNode::Paragraph,
                RdlNode::AttribStack(StackOp::Pop),
            ],
        };
        let text = write(&list).unwrap();
        assert!(
            text.starts_with("<RDL><PUSH/><TRA bold=\"true\" color=\"#FF0000\"/>"),
            "{text}"
        );
        let mut back = RenderDisplayList::new();
        assert!(read_str(&mut back, &text), "{text}");
        assert_eq!(back, list);
    }

    #[test]
    fn refuses_what_it_cannot_write() {
        for node in [
            RdlNode::Null,
            RdlNode::AttribStack(StackOp::Clear),
            RdlNode::Justify(Justify::Other(3)),
            RdlNode::Position(Position {
                rel_h: false,
                rel_v: false,
                pos_v: 0.5,
                pos_h: 0.0,
            }),
            RdlNode::Position(Position {
                rel_h: false,
                rel_v: false,
                pos_v: -0.0,
                pos_h: 0.0,
            }),
            RdlNode::Text(vec![0x1]),
            RdlNode::Text(vec![0xD800]),
        ] {
            let list = RenderDisplayList { nodes: vec![node] };
            assert!(write(&list).is_err(), "{list:?}");
        }
    }
}
