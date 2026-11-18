//! A strict XML 1.0 reader over UTF-16 text with SAX-style callbacks: the
//! part of MSXML's `SAXXMLReader` the RDLX parser relies on
//! (`XMLReader::read_buffer` in `CppSource/Common/common_rdlio.cpp`).
//!
//! The reference hands the text to MSXML as a BSTR and treats "the parse
//! failed" as "this text is not markup" (`AppendTextMaybeXmlRdl`,
//! `LoadIdsIntoRdl`), so well-formedness is part of the behaviour: plain
//! text such as `<Ship Type>` or `Trade & Group Offers` must be rejected.
//! The callbacks run while the text is read; an error stops the reader,
//! and what the callbacks did before it stays done, as in the reference.
//!
//! Read as MSXML reads with its default features (namespaces on, prefixes
//! off): the XML declaration, comments, processing instructions, CDATA,
//! the five predefined entities and character references, end-of-line
//! and attribute-value normalisation, and namespace prefixes (the
//! callbacks see local names; `xmlns` attributes are not reported). A
//! document type declaration is rejected (divergence D232).
//!
//! When the callbacks run is the reader's own choice where MSXML's is not
//! known: an element's start after its whole start tag, character data
//! when markup ends the run (never a run cut short by an error or by the
//! end of the text). Only text that is not well-formed can tell the
//! difference (divergence D232, with the name classes, which are those of
//! XML 1.0 fifth edition).

use std::fmt;

/// An attribute as the start callback sees it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Attribute {
    /// The local name (after a namespace prefix).
    pub local: Vec<u16>,
    /// The normalised value.
    pub value: Vec<u16>,
}

/// The content callbacks (`ISAXContentHandler`'s `startElement`,
/// `endElement` and `characters`).
pub trait Handler {
    /// A start tag was read.
    fn start_element(&mut self, local: &[u16], attributes: &[Attribute]);
    /// An end tag, or the end of an empty-element tag, was read.
    fn end_element(&mut self, local: &[u16]);
    /// A run of character data was read.
    fn characters(&mut self, text: &[u16]);
}

/// Why the text is not well-formed XML.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct XmlError {
    /// The UTF-16 unit where the reader stopped.
    pub position: usize,
    /// What it found there.
    pub reason: &'static str,
}

impl fmt::Display for XmlError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} at unit {}", self.reason, self.position)
    }
}

impl std::error::Error for XmlError {}

/// Reads `text` as one XML document, calling `handler` as it goes.
///
/// # Errors
///
/// The first place where `text` is not a well-formed document.
pub fn parse(text: &[u16], handler: &mut dyn Handler) -> Result<(), XmlError> {
    Reader {
        units: text,
        pos: 0,
        handler,
        open: Vec::new(),
        namespaces: Vec::new(),
    }
    .document()
}

const INVALID: u32 = u32::MAX;

fn is_char(c: u32) -> bool {
    matches!(c, 0x9 | 0xA | 0xD | 0x20..=0xD7FF | 0xE000..=0xFFFD | 0x1_0000..=0x10_FFFF)
}

fn is_space(c: u32) -> bool {
    matches!(c, 0x20 | 0x9 | 0xA | 0xD)
}

fn is_name_start(c: u32) -> bool {
    matches!(c,
        0x3A | 0x41..=0x5A | 0x5F | 0x61..=0x7A | 0xC0..=0xD6 | 0xD8..=0xF6 | 0xF8..=0x2FF
        | 0x370..=0x37D | 0x37F..=0x1FFF | 0x200C..=0x200D | 0x2070..=0x218F
        | 0x2C00..=0x2FEF | 0x3001..=0xD7FF | 0xF900..=0xFDCF | 0xFDF0..=0xFFFD
        | 0x1_0000..=0xE_FFFF)
}

fn is_name_char(c: u32) -> bool {
    is_name_start(c)
        || matches!(c, 0x2D | 0x2E | 0x30..=0x39 | 0xB7 | 0x300..=0x36F | 0x203F..=0x2040)
}

fn push_char(out: &mut Vec<u16>, c: u32) {
    if let Some(ch) = char::from_u32(c) {
        let mut buffer = [0u16; 2];
        out.extend_from_slice(ch.encode_utf16(&mut buffer));
    }
}

fn units_of(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

/// The prefix and local name of a qualified name; `None` when the name has
/// a colon at either end or more than one.
fn split_qname(name: &[u16]) -> Option<(Option<&[u16]>, &[u16])> {
    let colon = u16::from(b':');
    let mut parts = name.split(|&u| u == colon);
    let first = parts.next()?;
    match (parts.next(), parts.next()) {
        (None, _) => Some((None, first)),
        (Some(local), None) if !first.is_empty() && !local.is_empty() => Some((Some(first), local)),
        _ => None,
    }
}

struct Reader<'a> {
    units: &'a [u16],
    pos: usize,
    handler: &'a mut dyn Handler,
    /// Qualified names of the open elements.
    open: Vec<Vec<u16>>,
    /// Declared prefixes and the depth of the element that declared them.
    namespaces: Vec<(Vec<u16>, usize)>,
}

impl Reader<'_> {
    fn error<T>(&self, reason: &'static str) -> Result<T, XmlError> {
        Err(XmlError {
            position: self.pos,
            reason,
        })
    }

    /// The code point at the cursor and its width in units; a lone
    /// surrogate is [`INVALID`].
    fn peek(&self) -> Option<(u32, usize)> {
        let unit = *self.units.get(self.pos)?;
        match unit {
            0xD800..=0xDBFF => match self.units.get(self.pos + 1) {
                Some(&low @ 0xDC00..=0xDFFF) => Some((
                    0x1_0000 + ((u32::from(unit) - 0xD800) << 10) + (u32::from(low) - 0xDC00),
                    2,
                )),
                _ => Some((INVALID, 1)),
            },
            0xDC00..=0xDFFF => Some((INVALID, 1)),
            _ => Some((u32::from(unit), 1)),
        }
    }

    fn peek_char(&self) -> Option<u32> {
        self.peek().map(|(c, _)| c)
    }

    fn bump(&mut self) -> Option<u32> {
        let (c, width) = self.peek()?;
        self.pos += width;
        Some(c)
    }

    fn at(&self, text: &str) -> bool {
        let mut at = self.pos;
        text.encode_utf16().all(|unit| {
            let found = self.units.get(at) == Some(&unit);
            at += 1;
            found
        })
    }

    fn eat(&mut self, text: &str) -> bool {
        let found = self.at(text);
        if found {
            self.pos += text.encode_utf16().count();
        }
        found
    }

    fn expect(&mut self, text: &str, reason: &'static str) -> Result<(), XmlError> {
        if self.eat(text) {
            Ok(())
        } else {
            self.error(reason)
        }
    }

    fn skip_space(&mut self) -> bool {
        let start = self.pos;
        while self.peek_char().is_some_and(is_space) {
            self.pos += 1;
        }
        self.pos != start
    }

    fn name(&mut self) -> Result<Vec<u16>, XmlError> {
        let start = self.pos;
        match self.peek_char() {
            Some(c) if is_name_start(c) => {}
            _ => return self.error("expected a name"),
        }
        while self.peek_char().is_some_and(is_name_char) {
            self.bump();
        }
        Ok(self.units[start..self.pos].to_vec())
    }

    fn document(&mut self) -> Result<(), XmlError> {
        // A byte-order mark is not part of the document.
        if self.units.first() == Some(&0xFEFF) {
            self.pos = 1;
        }
        if self.at("<?xml")
            && self
                .units
                .get(self.pos + 5)
                .is_some_and(|&u| is_space(u.into()))
        {
            self.xml_declaration()?;
        }
        self.misc()?;
        if self.at("<!DOCTYPE") {
            return self.error("document type declarations are not read");
        }
        if !self.at("<") || self.peek_char().is_none() {
            return self.error("expected the root element");
        }
        self.pos += 1;
        self.element()?;
        self.misc()?;
        if self.pos != self.units.len() {
            return self.error("content after the root element");
        }
        Ok(())
    }

    /// Comments, processing instructions and white space.
    fn misc(&mut self) -> Result<(), XmlError> {
        loop {
            self.skip_space();
            if self.eat("<!--") {
                self.comment()?;
            } else if self.eat("<?") {
                self.processing_instruction()?;
            } else {
                return Ok(());
            }
        }
    }

    fn xml_declaration(&mut self) -> Result<(), XmlError> {
        self.pos += 5;
        let mut seen = 0;
        loop {
            let spaced = self.skip_space();
            if self.eat("?>") {
                break;
            }
            if !spaced {
                return self.error("expected white space in the XML declaration");
            }
            let name = self.name()?;
            self.skip_space();
            self.expect("=", "expected '='")?;
            self.skip_space();
            let value = self.declaration_value()?;
            let ascii = |text: &str| name == units_of(text);
            let order = if ascii("version") {
                if value != units_of("1.0") {
                    return self.error("unsupported XML version");
                }
                1
            } else if ascii("encoding") {
                2
            } else if ascii("standalone") {
                if value != units_of("yes") && value != units_of("no") {
                    return self.error("standalone must be yes or no");
                }
                3
            } else {
                return self.error("unknown pseudo-attribute in the XML declaration");
            };
            if order <= seen || (seen == 0 && order != 1) {
                return self.error("XML declaration out of order");
            }
            seen = order;
        }
        if seen == 0 {
            return self.error("the XML declaration needs a version");
        }
        Ok(())
    }

    fn declaration_value(&mut self) -> Result<Vec<u16>, XmlError> {
        let Some(quote) = self.peek_char().filter(|&c| c == 0x22 || c == 0x27) else {
            return self.error("expected a quoted value");
        };
        self.pos += 1;
        let start = self.pos;
        while let Some(c) = self.peek_char() {
            if c == quote {
                let value = self.units[start..self.pos].to_vec();
                self.pos += 1;
                return Ok(value);
            }
            if !(c == 0x2D
                || c == 0x2E
                || c == 0x5F
                || c < 0x80 && (c as u8).is_ascii_alphanumeric())
            {
                return self.error("invalid character in the XML declaration");
            }
            self.pos += 1;
        }
        self.error("unterminated value")
    }

    fn comment(&mut self) -> Result<(), XmlError> {
        loop {
            if self.at("--") {
                if self.eat("-->") {
                    return Ok(());
                }
                return self.error("'--' inside a comment");
            }
            match self.bump() {
                Some(c) if is_char(c) => {}
                Some(_) => return self.error("invalid character"),
                None => return self.error("unterminated comment"),
            }
        }
    }

    fn processing_instruction(&mut self) -> Result<(), XmlError> {
        let target = self.name()?;
        if target.len() == 3
            && target
                .iter()
                .zip(b"xml")
                .all(|(&u, &b)| u < 0x80 && (u as u8).eq_ignore_ascii_case(&b))
        {
            return self.error("reserved processing-instruction target");
        }
        if self.eat("?>") {
            return Ok(());
        }
        if !self.skip_space() {
            return self.error("expected white space after the target");
        }
        loop {
            if self.eat("?>") {
                return Ok(());
            }
            match self.bump() {
                Some(c) if is_char(c) => {}
                Some(_) => return self.error("invalid character"),
                None => return self.error("unterminated processing instruction"),
            }
        }
    }

    /// `&...;` after the `&`: the character it stands for.
    fn reference(&mut self) -> Result<u32, XmlError> {
        if self.eat("#x") {
            self.char_reference(16)
        } else if self.eat("#") {
            self.char_reference(10)
        } else {
            let name = self.name()?;
            self.expect(";", "expected ';'")?;
            let entity = [
                ("lt", 0x3C),
                ("gt", 0x3E),
                ("amp", 0x26),
                ("apos", 0x27),
                ("quot", 0x22),
            ]
            .iter()
            .find(|(text, _)| name == units_of(text));
            match entity {
                Some(&(_, c)) => Ok(c),
                None => self.error("undeclared entity"),
            }
        }
    }

    fn char_reference(&mut self, radix: u32) -> Result<u32, XmlError> {
        let mut value: u32 = 0;
        let mut digits = 0;
        while let Some(digit) = self
            .peek_char()
            .and_then(|c| char::from_u32(c)?.to_digit(radix))
        {
            value = value.saturating_mul(radix).saturating_add(digit);
            digits += 1;
            self.pos += 1;
        }
        if digits == 0 {
            return self.error("expected digits");
        }
        self.expect(";", "expected ';'")?;
        if !is_char(value) {
            return self.error("character reference to an invalid character");
        }
        Ok(value)
    }

    /// The element after its `<`, through its end tag.
    fn element(&mut self) -> Result<(), XmlError> {
        let qname = self.name()?;
        let depth = self.open.len() + 1;
        let mut raw: Vec<(Vec<u16>, Vec<u16>)> = Vec::new();
        let empty = loop {
            let spaced = self.skip_space();
            if self.eat("/>") {
                break true;
            }
            if self.eat(">") {
                break false;
            }
            if !spaced {
                return self.error("expected white space before an attribute");
            }
            let name = self.name()?;
            self.skip_space();
            self.expect("=", "expected '='")?;
            self.skip_space();
            let value = self.attribute_value()?;
            if raw.iter().any(|(n, _)| *n == name) {
                return self.error("duplicate attribute");
            }
            raw.push((name, value));
        };
        // Namespace declarations first, then the names that use them.
        let xmlns = units_of("xmlns");
        let mut attributes = Vec::new();
        for (name, value) in &raw {
            match split_qname(name) {
                Some((None, local)) if *local == xmlns[..] => {}
                Some((Some(prefix), local)) if *prefix == xmlns[..] => {
                    if value.is_empty() {
                        return self.error("empty namespace name");
                    }
                    self.namespaces.push((local.to_vec(), depth));
                }
                Some(_) => {}
                None => return self.error("malformed qualified name"),
            }
        }
        for (name, value) in raw {
            let Some((prefix, local)) = split_qname(&name) else {
                return self.error("malformed qualified name");
            };
            if local == xmlns || prefix == Some(&xmlns) {
                continue;
            }
            self.check_prefix(prefix)?;
            attributes.push(Attribute {
                local: local.to_vec(),
                value,
            });
        }
        let Some((prefix, local)) = split_qname(&qname) else {
            return self.error("malformed qualified name");
        };
        self.check_prefix(prefix)?;
        let local = local.to_vec();
        self.handler.start_element(&local, &attributes);
        if empty {
            self.handler.end_element(&local);
            self.namespaces.retain(|(_, d)| *d < depth);
            return Ok(());
        }
        self.open.push(qname);
        self.content()?;
        self.handler.end_element(&local);
        self.open.pop();
        self.namespaces.retain(|(_, d)| *d < depth);
        Ok(())
    }

    fn check_prefix(&self, prefix: Option<&[u16]>) -> Result<(), XmlError> {
        match prefix {
            Some(p)
                if *p != units_of("xml")[..] && !self.namespaces.iter().any(|(n, _)| n == p) =>
            {
                self.error("undeclared namespace prefix")
            }
            _ => Ok(()),
        }
    }

    fn attribute_value(&mut self) -> Result<Vec<u16>, XmlError> {
        let Some(quote) = self.peek_char().filter(|&c| c == 0x22 || c == 0x27) else {
            return self.error("expected a quoted value");
        };
        self.pos += 1;
        let mut value = Vec::new();
        loop {
            match self.peek_char() {
                None => return self.error("unterminated attribute value"),
                Some(c) if c == quote => {
                    self.pos += 1;
                    return Ok(value);
                }
                Some(0x3C) => return self.error("'<' in an attribute value"),
                Some(0x26) => {
                    self.pos += 1;
                    let c = self.reference()?;
                    push_char(&mut value, c);
                }
                Some(0xD) => {
                    self.pos += 1;
                    if self.peek_char() == Some(0xA) {
                        self.pos += 1;
                    }
                    value.push(0x20);
                }
                Some(0x9 | 0xA) => {
                    self.pos += 1;
                    value.push(0x20);
                }
                Some(c) if is_char(c) => {
                    self.bump();
                    push_char(&mut value, c);
                }
                Some(_) => return self.error("invalid character"),
            }
        }
    }

    fn flush(&mut self, pending: &mut Vec<u16>) {
        if !pending.is_empty() {
            self.handler.characters(pending);
            pending.clear();
        }
    }

    /// Content up to and including the end tag of the innermost open
    /// element.
    fn content(&mut self) -> Result<(), XmlError> {
        let mut pending = Vec::new();
        loop {
            match self.peek_char() {
                None => return self.error("missing end tag"),
                Some(0x3C) => {
                    if self.eat("</") {
                        self.flush(&mut pending);
                        let name = self.name()?;
                        if self.open.last() != Some(&name) {
                            return self.error("end tag does not match the start tag");
                        }
                        self.skip_space();
                        self.expect(">", "expected '>'")?;
                        return Ok(());
                    }
                    if self.eat("<!--") {
                        self.flush(&mut pending);
                        self.comment()?;
                    } else if self.eat("<![CDATA[") {
                        self.flush(&mut pending);
                        let mut data = Vec::new();
                        while !self.eat("]]>") {
                            match self.bump() {
                                None => return self.error("unterminated CDATA section"),
                                Some(0xD) => {
                                    if self.peek_char() == Some(0xA) {
                                        self.pos += 1;
                                    }
                                    data.push(0xA);
                                }
                                Some(c) if is_char(c) => push_char(&mut data, c),
                                Some(_) => return self.error("invalid character"),
                            }
                        }
                        self.flush(&mut data);
                    } else if self.eat("<?") {
                        self.flush(&mut pending);
                        self.processing_instruction()?;
                    } else {
                        self.pos += 1;
                        self.flush(&mut pending);
                        self.element()?;
                    }
                }
                Some(0x26) => {
                    self.pos += 1;
                    let c = self.reference()?;
                    push_char(&mut pending, c);
                }
                Some(0xD) => {
                    self.pos += 1;
                    if self.peek_char() == Some(0xA) {
                        self.pos += 1;
                    }
                    pending.push(0xA);
                }
                Some(c) if is_char(c) => {
                    if self.at("]]>") {
                        return self.error("']]>' in character data");
                    }
                    self.bump();
                    push_char(&mut pending, c);
                }
                Some(_) => return self.error("invalid character"),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Log(Vec<String>);

    impl Handler for Log {
        fn start_element(&mut self, local: &[u16], attributes: &[Attribute]) {
            let mut line = format!("<{}", String::from_utf16_lossy(local));
            for a in attributes {
                line += &format!(
                    " {}={:?}",
                    String::from_utf16_lossy(&a.local),
                    String::from_utf16_lossy(&a.value)
                );
            }
            self.0.push(line);
        }
        fn end_element(&mut self, local: &[u16]) {
            self.0
                .push(format!("</{}", String::from_utf16_lossy(local)));
        }
        fn characters(&mut self, text: &[u16]) {
            self.0.push(format!("{:?}", String::from_utf16_lossy(text)));
        }
    }

    fn run(text: &str) -> (Vec<String>, Result<(), XmlError>) {
        let mut log = Log::default();
        let units: Vec<u16> = text.encode_utf16().collect();
        let result = parse(&units, &mut log);
        (log.0, result)
    }

    #[test]
    fn reads_a_card() {
        let (log, result) = run("<RDL><PUSH/><TEXT>a &lt;b&gt; &amp; c</TEXT><PARA/><POP/></RDL>");
        assert!(result.is_ok());
        assert_eq!(
            log,
            [
                "<RDL",
                "<PUSH",
                "</PUSH",
                "<TEXT",
                "\"a <b> & c\"",
                "</TEXT",
                "<PARA",
                "</PARA",
                "<POP",
                "</POP",
                "</RDL"
            ]
        );
    }

    #[test]
    fn normalises_ends_of_lines_and_attribute_values() {
        let (log, result) = run(
            "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n<a b='x\r\ny\tz&#10;'>1\r\n2\r3<![CDATA[<&]]></a>",
        );
        assert!(result.is_ok());
        assert_eq!(log, ["<a b=\"x y z\\n\"", "\"1\\n2\\n3\"", "\"<&\"", "</a"]);
    }

    #[test]
    fn rejects_plain_text() {
        for text in [
            "Object Unknown",
            "Trade & Group Offers",
            "<Ship Type>",
            "<< PREV",
            "",
            "<a></b>",
            "<a/><b/>",
            "<a b='1' b='2'/>",
            "<a>]]></a>",
            "<a>&nbsp;</a>",
            "<a>&#1;</a>",
            "<p:a/>",
            "<!DOCTYPE a><a/>",
            "<a b=1/>",
            "<a b='<'/>",
            "<a\u{0}/>",
        ] {
            assert!(run(text).1.is_err(), "{text:?}");
        }
    }

    #[test]
    fn events_before_an_error_stay() {
        let (log, result) = run("<Value>");
        assert!(result.is_err());
        assert_eq!(log, ["<Value"]);
        let (log, result) = run("<a>text<b/>more");
        assert!(result.is_err());
        assert_eq!(log, ["<a", "\"text\"", "<b", "</b"]);
    }

    #[test]
    fn namespaces_give_local_names() {
        let (log, result) = run("<p:a xmlns:p='urn:x' xmlns='urn:y' p:b='1'><c/></p:a>");
        assert!(result.is_ok());
        assert_eq!(log, ["<a b=\"1\"", "<c", "</c", "</a"]);
    }

    #[test]
    fn surrogates() {
        let mut units: Vec<u16> = "<a>".encode_utf16().collect();
        units.extend([0xD83D, 0xDE00]);
        units.extend("</a>".encode_utf16());
        assert!(parse(&units, &mut Log::default()).is_ok());
        units[3] = 0xDE00;
        assert!(parse(&units, &mut Log::default()).is_err());
    }
}
