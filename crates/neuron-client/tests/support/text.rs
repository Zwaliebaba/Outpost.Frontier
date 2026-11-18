//! The text suites' line formats (`CppSource/Oracle/oracle_text.cpp`
//! writes the same):
//!
//! ```text
//! {"lua":"<double bits>","text":"<hex>"}
//! {"count":"<n>","format":"<hex or ->","args":"<args or ->","result":"<return value>","buffer":"<hex of the n bytes>"}
//! {"case":"<n>","ok":"<0 or 1>","nodes":"<node> ..."}
//! ```
//!
//! The printf buffer is filled with 0xAA before the call. A node is
//! `tra:<attrs>:<mask>:<default mask>` (8 hex digits each),
//! `text:<units>` and `ptr:<units>` (4 hex digits per UTF-16 unit),
//! `stack:<op>`, `pos:<relH>:<relV>:<v bits>:<h bits>`, `para`,
//! `just:<value>`, `style:<id>`, or `other`.

use std::fmt::Write as _;

use neuron_client::text::rdl::{RdlNode, RenderDisplayList};
use neuron_core::msvcrt::{PrintfArg, format_lua_number, numeric_vsnprintf};

use super::{hex, unhex};

/// The fill byte of the printf buffer.
const FILL: u8 = 0xAA;

fn units_hex(units: &[u16]) -> String {
    units.iter().fold(String::new(), |mut out, u| {
        let _ = write!(out, "{u:04x}");
        out
    })
}

/// One line of `number_text.list` rendered.
pub fn number_line(line: &str) -> String {
    let mut fields = line.split(' ');
    match fields.next() {
        Some("lua") => {
            let field = fields.next().unwrap();
            let value = f64::from_bits(u64::from_str_radix(field, 16).unwrap());
            let text = format_lua_number(value);
            format!(
                "{{\"lua\":\"{field}\",\"text\":\"{}\"}}",
                hex(text.as_bytes())
            )
        }
        Some("printf") => {
            let count: usize = fields.next().unwrap().parse().unwrap();
            let format_field = fields.next().unwrap();
            let format = if format_field == "-" {
                Vec::new()
            } else {
                unhex(format_field)
            };
            let arg_fields: Vec<&str> = fields.collect();
            let args: Vec<PrintfArg> = arg_fields
                .iter()
                .map(|a| match a.split_once(':') {
                    Some(("i", v)) => {
                        PrintfArg::Int(u32::from_str_radix(v, 16).unwrap().cast_signed())
                    }
                    Some(("d", v)) => {
                        PrintfArg::Double(f64::from_bits(u64::from_str_radix(v, 16).unwrap()))
                    }
                    _ => panic!("bad argument {a}"),
                })
                .collect();
            let printed =
                numeric_vsnprintf(count, &format, &args).unwrap_or_else(|e| panic!("{line}: {e}"));
            let mut buffer = vec![FILL; count];
            buffer[..printed.stored.len()].copy_from_slice(&printed.stored);
            let args_text = if arg_fields.is_empty() {
                "-".to_owned()
            } else {
                arg_fields.join(" ")
            };
            format!(
                "{{\"count\":\"{count}\",\"format\":\"{format_field}\",\"args\":\"{args_text}\",\"result\":\"{}\",\"buffer\":\"{}\"}}",
                printed.result,
                hex(&buffer)
            )
        }
        _ => panic!("bad line {line}"),
    }
}

/// A node as the dump writes it.
pub fn node(node: &RdlNode) -> String {
    match node {
        RdlNode::Attributes(a) => {
            format!("tra:{:08x}:{:08x}:{:08x}", a.attrs, a.mask, a.default_mask)
        }
        RdlNode::Text(text) => format!("text:{}", units_hex(text)),
        RdlNode::TextPtr(text) => format!("ptr:{}", units_hex(text)),
        RdlNode::AttribStack(op) => format!("stack:{}", op.value()),
        RdlNode::Position(p) => format!(
            "pos:{}:{}:{:08x}:{:08x}",
            u8::from(p.rel_h),
            u8::from(p.rel_v),
            p.pos_v.to_bits(),
            p.pos_h.to_bits()
        ),
        RdlNode::Paragraph => "para".to_owned(),
        RdlNode::Justify(j) => format!("just:{}", j.value()),
        RdlNode::Style(id) => format!("style:{id}"),
        RdlNode::Null | RdlNode::Image(_) | RdlNode::Clip(_) | RdlNode::Ref(_) => {
            "other".to_owned()
        }
    }
}

/// One line of `rdl_markup.list` rendered.
pub fn markup_line(
    case: usize,
    line: &str,
    read: impl FnOnce(&mut RenderDisplayList, &[u16]) -> bool,
) -> String {
    let bytes = if line == "-" { Vec::new() } else { unhex(line) };
    let units: Vec<u16> = bytes
        .as_chunks::<2>()
        .0
        .iter()
        .map(|&pair| u16::from_le_bytes(pair))
        .collect();
    let mut list = RenderDisplayList::new();
    let ok = read(&mut list, &units);
    let nodes: Vec<String> = list.nodes.iter().map(node).collect();
    format!(
        "{{\"case\":\"{case}\",\"ok\":\"{}\",\"nodes\":\"{}\"}}",
        u8::from(ok),
        nodes.join(" ")
    )
}
