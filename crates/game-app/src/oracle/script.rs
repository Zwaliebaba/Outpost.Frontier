//! The oracle mode's script and `session.jsonl` (`oracle_mode.cpp`:
//! `Start`, `RunScript`, `Execute`, `Dump`, `Quit`, `OracleFrameTick`,
//! `OracleNoteScreen`; decision O53 (2)).
//!
//! The script runs at the start of each frame, line after line, until a line
//! makes it wait. Every executed line is logged after it ran; every screen
//! change is logged and dumped by itself 30 frames later. What needs the
//! interface (the tree, the screenshot, the key-map command, the key, the
//! click) goes through [`OracleHost`].

use super::json::OracleJson;
use super::tree::TreeDump;

/// The fixed frame (`kOracleFrameSeconds`).
pub const FRAME_SECONDS: f64 = 1.0 / 30.0;

/// The seed (`kOracleRandSeed`, "ORAC").
pub const RAND_SEED: u32 = 0x4f52_4143;

/// Frames between a screen change and its automatic dump
/// (`kOracleSettleFrames`).
pub const SETTLE_FRAMES: u32 = 30;

/// What the script's commands need from the client.
pub trait OracleHost {
    /// `TakeShot(label)`: the screenshot `name` (`shot_NNNN_<label>.bmp`);
    /// false when it failed.
    fn take_shot(&mut self, name: &str) -> bool;
    /// `WriteTree`'s nodes: the interface tree flattened, one line per
    /// node after `"dump":<dump>` (the script writes the head).
    fn tree(&mut self, dump: u32) -> TreeDump;
    /// `DoCommand(nickname)`: the key-map command as a matching key press.
    /// The note either way (empty for none).
    ///
    /// # Errors
    ///
    /// The note when the line did not run.
    fn command(&mut self, nickname: &str) -> Result<String, String>;
    /// `DoKey(arg)`: a key press posted to the window.
    ///
    /// # Errors
    ///
    /// The note when the line did not run.
    fn key(&mut self, arg: &str) -> Result<String, String>;
    /// `DoClick(spec)`: a click on the named target.
    ///
    /// # Errors
    ///
    /// The note when the line did not run.
    fn click(&mut self, spec: &str) -> Result<String, String>;
    /// `G_PendingClientStateId = id`.
    fn set_pending_state(&mut self, id: u32);
}

/// `isspace` in the "C" locale.
fn is_space(c: char) -> bool {
    matches!(c, ' ' | '\t' | '\n' | '\x0b' | '\x0c' | '\r')
}

/// `Trim`.
fn trim(text: &str) -> &str {
    text.trim_matches(is_space)
}

/// `FirstWord`: the first blank-separated word and the trimmed rest.
fn first_word(text: &str) -> (&str, &str) {
    let end = text.find(is_space).unwrap_or(text.len());
    (&text[..end], trim(&text[end..]))
}

/// `ParseUnsigned`: decimal, or hexadecimal after `0x`, through `strtoul`
/// (a sign is taken and a negative value wraps, an overflow saturates); the
/// whole text must be read.
pub(crate) fn parse_unsigned(text: &str) -> Option<u32> {
    if text.is_empty() {
        return None;
    }
    let bytes = text.as_bytes();
    let hex = bytes.len() > 2 && bytes[0] == b'0' && (bytes[1] == b'x' || bytes[1] == b'X');
    let (negative, digits) = match bytes {
        [b'-', rest @ ..] => (true, rest),
        [b'+', rest @ ..] => (false, rest),
        _ => (false, bytes),
    };
    let (radix, digits) = if hex {
        (16, &digits[2..])
    } else {
        (10, digits)
    };
    if digits.is_empty() {
        return None;
    }
    let mut value: u64 = 0;
    for &b in digits {
        let d = char::from(b).to_digit(radix)?;
        value = (value * u64::from(radix) + u64::from(d)).min(u64::from(u32::MAX) + 1);
    }
    let value = u32::try_from(value).unwrap_or(u32::MAX);
    Some(if negative {
        value.wrapping_neg()
    } else {
        value
    })
}

/// `SanitizeLabel`: letters, digits, `-`, `_` and `.` kept, anything else
/// `_`, at most 48 bytes; `dump` for an empty label.
fn sanitize_label(label: &str) -> String {
    let out: String = label
        .bytes()
        .take(48)
        .map(|b| {
            if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.') {
                char::from(b)
            } else {
                '_'
            }
        })
        .collect();
    if out.is_empty() {
        "dump".to_string()
    } else {
        out
    }
}

/// The oracle mode's state (`G`) and its `session.jsonl` lines.
#[derive(Clone, Debug)]
pub struct OracleScript {
    script: Vec<String>,
    next_line: usize,
    frame: u32,
    wait_until: u32,
    wait_screen: Option<(String, u32, u32, usize)>,
    done: bool,
    auto_dump: Option<(u32, String)>,
    last_client: u32,
    last_menu: u32,
    last_room: u32,
    dump_count: u32,
    shot_count: u32,
    session: Vec<String>,
    tree: Vec<String>,
}

impl OracleScript {
    /// `Start(script, out)`: the script's lines (each without its line
    /// end) and the `start` record.
    #[must_use]
    pub fn start(script_text: &str, script_path: &str, out_dir: &str) -> Self {
        let script: Vec<String> = script_text
            .split('\n')
            .map(|l| l.trim_end_matches(['\r', '\n']).to_string())
            .collect();
        // `fgets` reads no line after a final line end.
        let script = match script.split_last() {
            Some((last, rest)) if last.is_empty() => rest.to_vec(),
            _ => script,
        };
        let mut s = Self {
            script,
            next_line: 0,
            frame: 0,
            wait_until: 0,
            wait_screen: None,
            done: false,
            auto_dump: None,
            last_client: u32::MAX,
            last_menu: u32::MAX,
            last_room: u32::MAX,
            dump_count: 0,
            shot_count: 0,
            session: Vec::new(),
            tree: Vec::new(),
        };
        let mut j = s.head("start");
        j.uint("version", 1)
            .str("script", script_path)
            .str("out", out_dir)
            .uint("lines", u32::try_from(s.script.len()).unwrap_or(u32::MAX))
            .double("frame_seconds", FRAME_SECONDS)
            .uint("seed", RAND_SEED)
            .uint("settle_frames", SETTLE_FRAMES);
        s.session.push(j.finish());
        s
    }

    fn head(&self, rec: &str) -> OracleJson {
        let mut j = OracleJson::new();
        j.uint("frame", self.frame).str("rec", rec);
        j
    }

    /// The frame number: `TickClientFrame` calls so far.
    #[must_use]
    pub fn frame(&self) -> u32 {
        self.frame
    }

    /// The script has ended.
    #[must_use]
    pub fn done(&self) -> bool {
        self.done
    }

    /// The `tree.jsonl` lines so far.
    #[must_use]
    pub fn tree(&self) -> &[String] {
        &self.tree
    }

    /// The `session.jsonl` lines so far.
    #[must_use]
    pub fn session(&self) -> &[String] {
        &self.session
    }

    /// `OracleForceDisplayMode`'s `display` record.
    pub fn note_display(&mut self, width: i32, height: i32, color_bits: i32, fullscreen: i32) {
        let mut j = self.head("display");
        j.int("width", width)
            .int("height", height)
            .int("color_bits", color_bits)
            .int("fullscreen", fullscreen);
        self.session.push(j.finish());
    }

    /// `OracleSeedRandom`'s `seed` record, with the server's seed and clock
    /// start (decision O53 (3)).
    pub fn note_seed(&mut self, server_seed: u32, server_clock: f64) {
        let mut j = self.head("seed");
        j.uint("seed", RAND_SEED)
            .uint("server_seed", server_seed)
            .double("server_clock", server_clock);
        self.session.push(j.finish());
    }

    fn last_screen(&mut self, kind: &str) -> Option<&mut u32> {
        match kind {
            "client" => Some(&mut self.last_client),
            "menu" => Some(&mut self.last_menu),
            "room" => Some(&mut self.last_room),
            _ => None,
        }
    }

    /// `OracleNoteScreen(kind, id)`: the screen change logged; its dump due
    /// 30 frames later, replacing a pending one.
    pub fn note_screen(&mut self, kind: &str, id: u32) {
        if let Some(last) = self.last_screen(kind) {
            *last = id;
        }
        let mut j = self.head("screen");
        j.str("kind", kind).uint("id", id);
        self.session.push(j.finish());
        self.auto_dump = Some((self.frame + SETTLE_FRAMES, format!("{kind}-{id}")));
    }

    /// `Dump(label, tree, shot, cause)`.
    fn dump(
        &mut self,
        host: &mut dyn OracleHost,
        label: &str,
        tree: bool,
        shot: bool,
        cause: &str,
    ) {
        self.dump_count += 1;
        let dump = self.dump_count;
        let mut shot_name = String::new();
        if shot {
            self.shot_count += 1;
            let name = format!("shot_{:04}_{}.bmp", self.shot_count, sanitize_label(label));
            if host.take_shot(&name) {
                shot_name = name;
            }
        }
        if tree {
            self.write_tree(host, dump, label, &shot_name);
        }
        let mut j = self.head("dump");
        j.uint("dump", dump)
            .str("cause", cause)
            .str("label", label)
            .bool("tree", tree);
        if shot {
            j.str("shot", &shot_name);
        } else {
            j.null("shot");
        }
        self.session.push(j.finish());
    }

    /// `WriteTree(dump, label, shot)`: the head with the latest screens,
    /// then the host's nodes.
    fn write_tree(&mut self, host: &mut dyn OracleHost, dump: u32, label: &str, shot: &str) {
        let nodes = host.tree(dump);
        let mut j = self.head("dump");
        j.uint("dump", dump)
            .str("label", label)
            .int("client", self.last_client.cast_signed())
            .int("menu", self.last_menu.cast_signed())
            .uint("room", self.last_room);
        if shot.is_empty() {
            j.null("shot");
        } else {
            j.str("shot", shot);
        }
        j.uint("roots", nodes.roots).uint(
            "nodes",
            u32::try_from(nodes.nodes.len()).unwrap_or(u32::MAX),
        );
        self.tree.push(j.finish());
        self.tree.extend(nodes.nodes);
    }

    /// `Quit(why)`: the script ends and the client enters state 1.
    fn quit(&mut self, host: &mut dyn OracleHost, why: &str) {
        self.done = true;
        host.set_pending_state(1);
        let mut j = self.head("quit");
        j.str("why", why);
        self.session.push(j.finish());
    }

    /// `Execute(command, rest)`: true when the line ran; the note says why
    /// not.
    fn execute(
        &mut self,
        host: &mut dyn OracleHost,
        command: &str,
        rest: &str,
    ) -> Result<String, String> {
        match command {
            "wait" => {
                let frames = parse_unsigned(rest).ok_or("bad frame count")?;
                self.wait_until = self.frame.wrapping_add(frames);
            }
            "waitscreen" => {
                let (kind, tail) = first_word(rest);
                let (id, frames) = first_word(tail);
                let usage = || "usage: waitscreen client|menu|room <id> <frames>".to_string();
                if self.last_screen(kind).is_none() {
                    return Err(usage());
                }
                let id = parse_unsigned(id).ok_or_else(usage)?;
                let frames = parse_unsigned(frames).ok_or_else(usage)?;
                self.wait_screen = Some((
                    kind.to_string(),
                    id,
                    self.frame.wrapping_add(frames),
                    self.next_line,
                ));
            }
            "dump" => self.dump(host, rest, true, true, "script"),
            "tree" => self.dump(host, rest, true, false, "script"),
            "shot" => self.dump(host, rest, false, true, "script"),
            "command" => return host.command(rest),
            "key" => return host.key(rest),
            "click" => return host.click(rest),
            "state" => host.set_pending_state(parse_unsigned(rest).ok_or("bad state id")?),
            "log" => {}
            "quit" => self.quit(host, "script"),
            _ => return Err("unknown command".into()),
        }
        Ok(String::new())
    }

    /// `RunScript`: lines until one waits or the script ends.
    fn run(&mut self, host: &mut dyn OracleHost) {
        while !self.done {
            if self.frame < self.wait_until {
                return;
            }
            if let Some((kind, id, deadline, line)) = self.wait_screen.clone() {
                let matched = self.last_screen(&kind).is_some_and(|last| *last == id);
                if !matched && self.frame < deadline {
                    return;
                }
                self.wait_screen = None;
                let mut j = self.head("waitscreen");
                j.uint("line", u32::try_from(line).unwrap_or(u32::MAX))
                    .str("result", if matched { "matched" } else { "timeout" });
                self.session.push(j.finish());
            }
            let Some(line) = self.script.get(self.next_line).cloned() else {
                self.quit(host, "end of script");
                return;
            };
            self.next_line += 1;
            let text = trim(&line);
            if text.is_empty() || text.starts_with('#') {
                continue;
            }
            let (command, rest) = first_word(text);
            let result = self.execute(host, command, rest);
            let mut j = self.head("script");
            j.uint("line", u32::try_from(self.next_line).unwrap_or(u32::MAX))
                .str("text", text)
                .bool("ok", result.is_ok());
            let (Ok(note) | Err(note)) = &result;
            if !note.is_empty() {
                j.str("note", note);
            }
            self.session.push(j.finish());
        }
    }

    /// `OracleFrameTick`: the frame counted, a due screen dump, then the
    /// script.
    pub fn frame_tick(&mut self, host: &mut dyn OracleHost) {
        self.frame += 1;
        if let Some((due, label)) = self.auto_dump.clone()
            && self.frame >= due
        {
            self.auto_dump = None;
            self.dump(host, &label, true, true, "screen");
        }
        self.run(host);
    }
}

/// `OracleParseCommandLine`: `+oracle` as a whole token in any case, then
/// the script and output paths. `None` without it; `Err` when a path is
/// missing (the C++ ends with exit code 3).
pub fn parse_command_line<S: AsRef<str>>(tokens: &[S]) -> Option<Result<(String, String), ()>> {
    let at = tokens
        .iter()
        .position(|t| t.as_ref().eq_ignore_ascii_case("+oracle"))?;
    match (tokens.get(at + 1), tokens.get(at + 2)) {
        (Some(script), Some(out)) => {
            Some(Ok((script.as_ref().to_string(), out.as_ref().to_string())))
        }
        _ => Some(Err(())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Host {
        calls: Vec<String>,
        pending: Option<u32>,
        shot_fails: bool,
    }

    impl OracleHost for Host {
        fn take_shot(&mut self, name: &str) -> bool {
            self.calls.push(format!("shot {name}"));
            !self.shot_fails
        }
        fn tree(&mut self, dump: u32) -> TreeDump {
            self.calls.push(format!("tree {dump}"));
            TreeDump {
                roots: 1,
                nodes: vec![format!("{{\"dump\":{dump},\"i\":0}}")],
            }
        }
        fn command(&mut self, nickname: &str) -> Result<String, String> {
            if nickname == "USER_CANCEL" {
                Ok(String::new())
            } else {
                Err("no such command".into())
            }
        }
        fn key(&mut self, _: &str) -> Result<String, String> {
            Ok(String::new())
        }
        fn click(&mut self, spec: &str) -> Result<String, String> {
            self.calls.push(format!("click {spec}"));
            Ok("press=1 release=0".into())
        }
        fn set_pending_state(&mut self, id: u32) {
            self.pending = Some(id);
        }
    }

    #[test]
    fn the_start_record_reads_as_the_cpp_one() {
        let s = OracleScript::start("wait 1\r\n# c\n\nquit\n", "a\\menu_walk.txt", "out");
        assert_eq!(
            s.session(),
            [
                r#"{"frame":0,"rec":"start","version":1,"script":"a\\menu_walk.txt","out":"out","lines":4,"frame_seconds":"3fa1111111111111","seed":1330790723,"settle_frames":30}"#
            ]
        );
    }

    #[test]
    fn waits_dumps_and_screens_follow_the_frames() {
        let script = "waitscreen menu 1 900\nwait 2\ndump main menu!\nclick text:LOAD GAME\ncommand USER_NOPE\nbogus\nstate 0x6\n";
        let mut s = OracleScript::start(script, "s", "o");
        let mut h = Host::default();
        s.frame_tick(&mut h); // frame 1: waits for the menu
        s.note_screen("menu", 1);
        s.frame_tick(&mut h); // frame 2: matched, then wait 2
        s.frame_tick(&mut h); // frame 3
        s.frame_tick(&mut h); // frame 4: dump and the rest, then the end
        let lines = &s.session()[1..];
        // A line is logged after it ran: the wait for the screen at frame 1.
        assert_eq!(
            lines[0],
            r#"{"frame":1,"rec":"script","line":1,"text":"waitscreen menu 1 900","ok":true}"#
        );
        assert_eq!(
            lines[1],
            r#"{"frame":1,"rec":"screen","kind":"menu","id":1}"#
        );
        assert_eq!(
            lines[2],
            r#"{"frame":2,"rec":"waitscreen","line":1,"result":"matched"}"#
        );
        assert_eq!(
            lines[3],
            r#"{"frame":2,"rec":"script","line":2,"text":"wait 2","ok":true}"#
        );
        assert_eq!(
            lines[4],
            r#"{"frame":4,"rec":"dump","dump":1,"cause":"script","label":"main menu!","tree":true,"shot":"shot_0001_main_menu_.bmp"}"#
        );
        assert_eq!(
            lines[6],
            r#"{"frame":4,"rec":"script","line":4,"text":"click text:LOAD GAME","ok":true,"note":"press=1 release=0"}"#
        );
        assert_eq!(
            lines[7],
            r#"{"frame":4,"rec":"script","line":5,"text":"command USER_NOPE","ok":false,"note":"no such command"}"#
        );
        assert_eq!(
            lines[8],
            r#"{"frame":4,"rec":"script","line":6,"text":"bogus","ok":false,"note":"unknown command"}"#
        );
        assert_eq!(
            lines[10],
            r#"{"frame":4,"rec":"quit","why":"end of script"}"#
        );
        assert!(s.done());
        // `state 0x6` queued state 6, then the end of the script state 1.
        assert_eq!(h.pending, Some(1));
        assert_eq!(h.calls[..2], ["shot shot_0001_main_menu_.bmp", "tree 1"]);
        // The tree's head: the latest screens (-1 and 4294967295 before the
        // first), the shot, and the host's nodes after it.
        assert_eq!(
            s.tree(),
            [
                r#"{"frame":4,"rec":"dump","dump":1,"label":"main menu!","client":-1,"menu":1,"room":4294967295,"shot":"shot_0001_main_menu_.bmp","roots":1,"nodes":1}"#,
                r#"{"dump":1,"i":0}"#
            ]
        );
    }

    #[test]
    fn a_screen_dumps_itself_thirty_frames_later_and_a_timeout_is_logged() {
        let mut s = OracleScript::start("waitscreen room 5 3\n", "s", "o");
        let mut h = Host {
            shot_fails: true,
            ..Host::default()
        };
        s.note_screen("client", 6);
        for _ in 0..29 {
            s.frame_tick(&mut h);
        }
        let lines = s.session();
        assert!(
            lines
                .iter()
                .any(|l| l == r#"{"frame":4,"rec":"waitscreen","line":1,"result":"timeout"}"#)
        );
        assert_eq!(
            lines.last().map(String::as_str),
            Some(r#"{"frame":4,"rec":"quit","why":"end of script"}"#)
        );
        // The script is done, but the frame still counts and the screen's dump
        // comes at frame 30 (30 after frame 0), with a failed screenshot written as "".
        let mut done = s.clone();
        assert!(done.done());
        done.frame_tick(&mut h);
        assert_eq!(
            done.session().last().map(String::as_str),
            Some(
                r#"{"frame":30,"rec":"dump","dump":1,"cause":"screen","label":"client-6","tree":true,"shot":""}"#
            )
        );
    }

    #[test]
    fn numbers_and_labels_read_as_the_cpp_helpers() {
        assert_eq!(parse_unsigned("030"), Some(30));
        assert_eq!(parse_unsigned("0x1F"), Some(31));
        assert_eq!(parse_unsigned("-1"), Some(u32::MAX));
        assert_eq!(parse_unsigned("99999999999"), Some(u32::MAX));
        assert_eq!(parse_unsigned("1x"), None);
        assert_eq!(parse_unsigned(""), None);
        assert_eq!(sanitize_label(""), "dump");
        assert_eq!(sanitize_label("a b/c.d"), "a_b_c.d");
        assert_eq!(
            parse_command_line(&["x", "+ORACLE", "s.txt", "out"]),
            Some(Ok(("s.txt".into(), "out".into())))
        );
        assert_eq!(parse_command_line(&["+oracle", "s.txt"]), Some(Err(())));
        assert_eq!(parse_command_line::<&str>(&["+W"]), None);
    }
}
