//! What the comparison of the port's client with the C++ client's oracle
//! files keeps of `session.jsonl` and `tree.jsonl` (decision O53 (6)):
//! the lines with the fields that differ by design masked as `"*"`.
//!
//! Under O53 (4) a mask other than `rtti` names an open temporary
//! divergence row, lists exact records and fields, goes with the PR that
//! removes the row, and is counted: [`Masked::hidden`] gives the fields
//! each row's masks hid, which the comparison prints.
//!
//! - `session.jsonl`: the `screen`, `dump` and `waitscreen` records and
//!   the script lines' results (`script`), without the `start` record (its
//!   paths). Masked: the `dump` records' `shot` (D1114: the port takes no
//!   screenshot); removed: the intro base's `room` screen line (D1107: not
//!   entered yet).
//! - `tree.jsonl`: every line of every dump, `rtti` masked (the C++ type
//!   names, O53 (4)); the dump heads' `shot` (D1114) and `room` (D1107);
//!   for the port's own client, [`PORT_RUN`]'s nodes (D1111). The text
//!   windows' font-dependent heights (the `h` of `tw` and `tc`) are masked
//!   only for a test run without the client's fonts, never in CI.

use std::collections::BTreeMap;

/// The masked value.
const MASK: &str = "\"*\"";

/// The end of the JSON value that starts at `at`: a string, an array or an
/// object up to its closing character, else a scalar up to the next `,`,
/// `]` or `}`.
fn value_end(line: &str, at: usize) -> usize {
    let bytes = line.as_bytes();
    let mut depth = 0usize;
    let mut in_string = false;
    let mut i = at;
    while i < bytes.len() {
        let b = bytes[i];
        if in_string {
            match b {
                b'\\' => i += 1,
                b'"' => {
                    in_string = false;
                    if depth == 0 {
                        return i + 1;
                    }
                }
                _ => {}
            }
        } else {
            match b {
                b'"' => in_string = true,
                b'[' | b'{' => depth += 1,
                b']' | b'}' | b',' if depth == 0 => return i,
                b']' | b'}' => {
                    depth -= 1;
                    if depth == 0 {
                        return i + 1;
                    }
                }
                _ => {}
            }
        }
        i += 1;
    }
    bytes.len()
}

/// The line with the value of the first `"key":` at or after `from`
/// masked; whether the line has one.
fn mask_from(line: &mut String, from: usize, key: &str) -> bool {
    let pattern = format!("\"{key}\":");
    let Some(i) = line[from..].find(&pattern) else {
        return false;
    };
    let start = from + i + pattern.len();
    let end = value_end(line, start);
    line.replace_range(start..end, MASK);
    true
}

/// The line with the value of the first `"key":` masked.
#[must_use]
pub fn mask(line: &str, key: &str) -> String {
    let mut line = line.to_owned();
    let _ = mask_from(&mut line, 0, key);
    line
}

/// The line with the `h` of its `tw` and `tc` objects masked; how many.
fn mask_heights(line: &mut String) -> usize {
    let mut n = 0;
    for object in ["\"tw\":{", "\"tc\":{"] {
        if let Some(at) = line.find(object) {
            n += usize::from(mask_from(line, at, "h"));
        }
    }
    n
}

/// A mask of tree nodes: the `fields` of the nodes named `names` in the
/// dumps `dumps`, for the divergence row `row`.
#[derive(Clone, Copy, Debug)]
pub struct NodeMask {
    pub row: &'static str,
    pub dumps: &'static [u32],
    pub names: &'static [&'static str],
    pub fields: &'static [&'static str],
}

impl NodeMask {
    fn applies(&self, line: &str) -> bool {
        self.dumps
            .iter()
            .any(|d| line.starts_with(&format!("{{\"dump\":{d},")))
            && self
                .names
                .iter()
                .any(|n| line.contains(&format!("\"name\":\"{n}\",")))
    }
}

/// The nodes the port's own client shows differently on `menu_walk.txt`
/// (its preferences window has no key names, device modes or abilities,
/// D1111): the Options page's key names (dumps 7 to 9), and its gamma row
/// and the video mode's text (dumps 8 and 9, the video tab). Remove them
/// with D1111.
pub const PORT_RUN: &[NodeMask] = &[
    // D1111: `GetKeyNameTextW`'s names of the controls tab's bindings.
    NodeMask {
        row: "D1111",
        dumps: &[7, 8, 9],
        names: &[
            "IKey_Row0",
            "IKey_Row1",
            "IKey_Row2",
            "IKey_Row3",
            "IKey_Row4",
            "IKey_Row5",
            "IKey_Row6",
            "IKey_Row7",
            "IKey_Row9",
            "IKey_Row10",
            "IKey_Row11",
            "IKey_Row12",
            "IKey_alt_Row0",
            "IKey_alt_Row1",
            "IKey_alt_Row6",
            "IKey_alt_Row7",
        ],
        fields: &["text"],
    },
    // D1111: the gamma row, which `QueryDeviceAbility(1)` shows.
    NodeMask {
        row: "D1111",
        dumps: &[8, 9],
        names: &["opt_text 2", "opt_smallText 2"],
        fields: &["flags", "text", "vis"],
    },
    NodeMask {
        row: "D1111",
        dumps: &[8, 9],
        names: &["opt_scroll 2"],
        fields: &["flags", "vis"],
    },
    // D1111: the video mode's text, with the depth of the device's mode.
    NodeMask {
        row: "D1111",
        dumps: &[8, 9],
        names: &["video_mode_text"],
        fields: &["text"],
    },
];

/// A bounded tolerance on an exact field (decision O53 (4)): a permanent
/// divergence row may let `field` of the object `object` of the nodes
/// selected in `dumps` differ by at most `bound`, never more than the row
/// describes; the lines must be equal apart from it.
#[derive(Clone, Copy, Debug)]
pub struct NodeTolerance {
    pub row: &'static str,
    pub dumps: &'static [u32],
    /// The nodes, by a `"key":"value"` of their line (`name` or `text`).
    pub select: &'static [(&'static str, &'static str)],
    /// The object holding the field (`"tw"`, `"tc"`).
    pub object: &'static str,
    pub field: &'static str,
    /// The largest difference allowed, in the field's unit.
    pub bound: f32,
}

impl NodeTolerance {
    fn applies(&self, line: &str) -> bool {
        self.dumps
            .iter()
            .any(|d| line.starts_with(&format!("{{\"dump\":{d},")))
            && self
                .select
                .iter()
                .any(|(k, v)| line.contains(&format!("\"{k}\":\"{v}\",")))
    }
}

/// The port's own client's tolerances on `menu_walk.txt`. D290: the hosted
/// runner has no `Agency FB`, GDI substitutes Arial and the port O29's
/// Bahnschrift SemiCondensed, and the main menu column's "Normal" style
/// autoscales one pixel apart (27 against 26); the column's five captions
/// and its version text take that line height times 0.002 as their `h`
/// (`FONT_LINE_SCALE`), so one pixel is 0.002. Owner, 2026-11-27: exactly
/// one pixel on those six nodes, counted.
pub const PORT_RUN_TOLERANCES: &[NodeTolerance] = &[
    NodeTolerance {
        row: "D290",
        dumps: &[1, 2, 5, 6],
        select: &[
            ("text", "NEW GAME"),
            ("text", "LOAD GAME"),
            ("text", "MULTIPLAYER"),
            ("text", "OPTIONS"),
            ("text", "EXIT"),
        ],
        object: "tw",
        field: "h",
        bound: 0.002,
    },
    NodeTolerance {
        row: "D290",
        dumps: &[1, 2, 5, 6],
        select: &[("name", "Version")],
        object: "tc",
        field: "h",
        bound: 0.002,
    },
];

/// The hex `float` value of `field` in `object` of `line`, with its span.
fn object_field(line: &str, object: &str, field: &str) -> Option<(f32, std::ops::Range<usize>)> {
    let at = line.find(&format!("\"{object}\":{{"))?;
    let key = format!("\"{field}\":\"");
    let start = at + line[at..].find(&key)? + key.len();
    let end = start + line[start..].find('"')?;
    let bits = u32::from_str_radix(&line[start..end], 16).ok()?;
    Some((f32::from_bits(bits), start..end))
}

/// `port`'s lines that differ from `cpp`'s at the same place only in a
/// tolerated field, and by at most its bound, take `cpp`'s line; each is
/// counted under its row in `port`'s `tolerated`.
pub fn tolerate(port: &mut Masked, cpp: &Masked, tolerances: &[NodeTolerance]) {
    for (ours, theirs) in port.lines.iter_mut().zip(&cpp.lines) {
        if ours == theirs {
            continue;
        }
        for t in tolerances
            .iter()
            .filter(|t| t.applies(ours) && t.applies(theirs))
        {
            let (Some((a, ours_span)), Some((b, theirs_span))) = (
                object_field(ours, t.object, t.field),
                object_field(theirs, t.object, t.field),
            ) else {
                continue;
            };
            let mut replaced = ours.clone();
            replaced.replace_range(ours_span, &theirs[theirs_span]);
            // The bound in double, with a margin for its own rounding.
            let within = (f64::from(a) - f64::from(b)).abs() <= f64::from(t.bound) + 1e-6;
            if replaced == *theirs && within {
                *ours = replaced;
                *port.tolerated.entry(t.row).or_default() += 1;
                break;
            }
        }
    }
}

/// Comparable lines and the fields each mask hid, by its divergence row
/// (`rtti` under "O53 (4)", the heights under "fonts").
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Masked {
    pub lines: Vec<String>,
    pub hidden: BTreeMap<&'static str, usize>,
    /// The lines a bounded tolerance took, by its row ([`tolerate`]).
    pub tolerated: BTreeMap<&'static str, usize>,
}

impl Masked {
    fn count(&mut self, row: &'static str, n: usize) {
        if n != 0 {
            *self.hidden.entry(row).or_default() += n;
        }
    }

    /// `"D1107 1, D1114 9"`: the counts, for the comparison's summary.
    #[must_use]
    pub fn summary(&self) -> String {
        if self.hidden.is_empty() && self.tolerated.is_empty() {
            return "none".into();
        }
        self.hidden
            .iter()
            .map(|(row, n)| format!("{row} {n}"))
            .chain(
                self.tolerated
                    .iter()
                    .map(|(row, n)| format!("{row} within tolerance {n}")),
            )
            .collect::<Vec<_>>()
            .join(", ")
    }
}

/// The comparable records of `session.jsonl`.
#[must_use]
pub fn session_records(lines: &[String]) -> Masked {
    let mut out = Masked::default();
    for l in lines {
        if !["screen", "dump", "waitscreen", "script"]
            .iter()
            .any(|r| l.contains(&format!("\"rec\":\"{r}\"")))
        {
            continue;
        }
        // D1107: the port does not enter the intro base yet.
        if l.contains("\"rec\":\"screen\"") && l.contains("\"kind\":\"room\"") {
            out.count("D1107", 1);
            continue;
        }
        let mut line = l.clone();
        if l.contains("\"rec\":\"dump\"") {
            // D1114: no screenshot.
            let n = usize::from(mask_from(&mut line, 0, "shot"));
            out.count("D1114", n);
        }
        out.lines.push(line);
    }
    out
}

/// The comparable lines of `tree.jsonl`, with `nodes`' masks; `heights`
/// keeps the text windows' heights.
#[must_use]
pub fn tree_records(lines: &[String], heights: bool, nodes: &[NodeMask]) -> Masked {
    let mut out = Masked::default();
    for l in lines.iter().filter(|l| !l.is_empty()) {
        let mut line = l.clone();
        if l.contains("\"rec\":\"dump\"") {
            // D1114: no screenshot; D1107: the intro base's room.
            let n = usize::from(mask_from(&mut line, 0, "shot"));
            out.count("D1114", n);
            let n = usize::from(mask_from(&mut line, 0, "room"));
            out.count("D1107", n);
            out.lines.push(line);
            continue;
        }
        let n = usize::from(mask_from(&mut line, 0, "rtti"));
        out.count("O53 (4)", n);
        for m in nodes.iter().filter(|m| m.applies(l)) {
            let n = m
                .fields
                .iter()
                .filter(|f| mask_from(&mut line, 0, f))
                .count();
            out.count(m.row, n);
        }
        if !heights {
            let n = mask_heights(&mut line);
            out.count("fonts", n);
        }
        out.lines.push(line);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_masks_replace_only_their_values() {
        assert_eq!(
            mask(r#"{"pos":["a","]"],"ctl":{"half":[1,2]},"n":7}"#, "pos"),
            r#"{"pos":"*","ctl":{"half":[1,2]},"n":7}"#
        );
        assert_eq!(
            mask(r#"{"ctl":{"half":[1,2]},"n":7}"#, "half"),
            r#"{"ctl":{"half":"*"},"n":7}"#
        );
        assert_eq!(mask(r#"{"n":7}"#, "n"), r#"{"n":"*"}"#);
        let head = r#"{"frame":31,"rec":"dump","dump":1,"label":"menu-1","client":6,"menu":1,"room":3060605825,"shot":"shot_0001_menu-1.bmp","roots":6,"nodes":28}"#;
        assert_eq!(
            tree_records(&[head.to_owned()], true, &[]).lines,
            [
                r#"{"frame":31,"rec":"dump","dump":1,"label":"menu-1","client":6,"menu":1,"room":"*","shot":"*","roots":6,"nodes":28}"#
            ]
        );
        let node = r#"{"dump":1,"i":3,"class":"TextWin","rtti":"class \"x\" TextWin","name":"a","text":"\"h\":1","tw":{"scale":"3f800000","w":"3e4ccccd","h":"3d4ccccd"}}"#;
        assert_eq!(
            tree_records(&[node.to_owned()], false, &[]).lines,
            [
                r#"{"dump":1,"i":3,"class":"TextWin","rtti":"*","name":"a","text":"\"h\":1","tw":{"scale":"3f800000","w":"3e4ccccd","h":"*"}}"#
            ]
        );
        let session: Vec<String> = [
            r#"{"frame":0,"rec":"start","version":1}"#,
            r#"{"frame":0,"rec":"screen","kind":"room","id":3060605825}"#,
            r#"{"frame":0,"rec":"screen","kind":"client","id":6}"#,
            r#"{"frame":31,"rec":"dump","dump":1,"cause":"screen","label":"menu-1","tree":true,"shot":""}"#,
            r#"{"frame":398,"rec":"quit","why":"script"}"#,
        ]
        .map(String::from)
        .to_vec();
        assert_eq!(
            session_records(&session).lines,
            [
                r#"{"frame":0,"rec":"screen","kind":"client","id":6}"#,
                r#"{"frame":31,"rec":"dump","dump":1,"cause":"screen","label":"menu-1","tree":true,"shot":"*"}"#,
            ]
        );
        assert_eq!(session_records(&session).summary(), "D1107 1, D1114 1");
    }

    #[test]
    fn the_port_runs_masks_take_only_their_nodes_and_are_counted() {
        let lines: Vec<String> = [
            r#"{"dump":7,"i":191,"class":"TextControl","rtti":"r","name":"IKey_Row0","flags":8,"text":"---"}"#,
            r#"{"dump":7,"i":236,"class":"TextControl","rtti":"r","name":"IKey_Row10x","flags":8,"text":"E"}"#,
            r#"{"dump":6,"i":191,"class":"TextControl","rtti":"r","name":"IKey_Row0","flags":8,"text":"W"}"#,
            r#"{"dump":8,"i":53,"class":"TextControl","rtti":"r","name":"opt_text 2","flags":8,"vis":false,"text":null}"#,
        ]
        .map(String::from)
        .to_vec();
        let m = tree_records(&lines, true, PORT_RUN);
        assert_eq!(
            m.lines,
            [
                r#"{"dump":7,"i":191,"class":"TextControl","rtti":"*","name":"IKey_Row0","flags":8,"text":"*"}"#,
                r#"{"dump":7,"i":236,"class":"TextControl","rtti":"*","name":"IKey_Row10x","flags":8,"text":"E"}"#,
                r#"{"dump":6,"i":191,"class":"TextControl","rtti":"*","name":"IKey_Row0","flags":8,"text":"W"}"#,
                r#"{"dump":8,"i":53,"class":"TextControl","rtti":"*","name":"opt_text 2","flags":"*","vis":"*","text":"*"}"#,
            ]
        );
        assert_eq!(m.summary(), "D1111 4, O53 (4) 4");
    }

    #[test]
    fn the_d290_tolerance_takes_one_pixel_on_its_nodes_only() {
        let line = |dump: u32, text: &str, h: &str, x: &str| {
            format!(
                r#"{{"dump":{dump},"i":19,"p":13,"d":1,"class":null,"rtti":"*","name":"","flags":11,"pos":["{x}"],"text":"{text}","tw":{{"scale":"3f800000","w":"3e800000","h":"{h}"}}}}"#
            )
        };
        // The CI run's pair: 27 px * 0.002 against 26 px * 0.002.
        let cpp = Masked {
            lines: vec![
                line(1, "NEW GAME", "3d54fdf4", "bee8f5c3"),
                line(1, "NEW GAME", "3d54fdf4", "bee8f5c3"),
                line(1, "NEW GAME", "3d54fdf4", "bee8f5c3"),
                line(3, "NEW GAME", "3d54fdf4", "bee8f5c3"),
                line(1, "OTHER", "3d54fdf4", "bee8f5c3"),
            ],
            ..Masked::default()
        };
        let mut port = Masked {
            lines: vec![
                line(1, "NEW GAME", "3d5d2f1b", "bee8f5c3"),
                // Two pixels (0.056).
                line(1, "NEW GAME", "3d656042", "bee8f5c3"),
                // One pixel, but another field differs too.
                line(1, "NEW GAME", "3d5d2f1b", "00000000"),
                // A dump the tolerance does not name.
                line(3, "NEW GAME", "3d5d2f1b", "bee8f5c3"),
                // A node it does not name.
                line(1, "OTHER", "3d5d2f1b", "bee8f5c3"),
            ],
            ..Masked::default()
        };
        tolerate(&mut port, &cpp, PORT_RUN_TOLERANCES);
        let equal: Vec<bool> = port
            .lines
            .iter()
            .zip(&cpp.lines)
            .map(|(a, b)| a == b)
            .collect();
        assert_eq!(equal, [true, false, false, false, false]);
        assert_eq!(port.tolerated.get("D290"), Some(&1));
        assert_eq!(port.summary(), "D290 within tolerance 1");
    }
}
