//! `ErrorHandler`: the named log channels the C++ `FDUMP` hook writes to
//! (`common_errorhandler.cpp`).
//!
//! A channel is a file, or one of the virtual sinks `$Text` and `$Window`.
//! The virtual sinks make a channel active but receive nothing: only a
//! channel with a file writes. The host opens files through the
//! [`SinkOpener`] it gives [`LogChannels::new`], so this module does no I/O
//! of its own.

use std::fmt;
use std::io::Write;

use crate::hash::stricmp;
use crate::ini::IniReader;
use crate::num::trunc_to_i32_via_i64;

use super::DumpFlags;

/// A channel's file: what `fopen(name, "wb")` returned.
pub type Sink = Box<dyn Write + Send>;

/// Opens a channel's file for writing, truncating it. `None` when the file
/// cannot be opened; the channel is then dropped, as in the C++.
pub type SinkOpener = Box<dyn FnMut(&[u8]) -> Option<Sink> + Send>;

/// The byte limit of a formatted line: `_vsnprintf` into 0x400 bytes, the
/// last one forced to NUL.
const LINE_LIMIT: usize = 0x3FF;
/// The newline is appended only while `len + 1 < 0x200`.
const NEWLINE_BUFFER: usize = 0x200;
/// `LogEntry`'s embedded file name buffer is 0x80 bytes.
const NAME_LIMIT: usize = 0x7F;
/// The severity limit of a channel the INI does not give one.
pub const DEFAULT_SEVERITY_LIMIT: i32 = 100;
/// The log id that matches every category.
pub const ANY_LOG: i32 = -1;

struct Channel {
    enabled: bool,
    log_id: i32,
    severity_limit: i32,
    active: bool,
    text: bool,
    window: bool,
    file: Option<Sink>,
}

impl Channel {
    /// `bEnabled && (bText || bWindow || File.Handle)`.
    fn refresh_active(&mut self) -> bool {
        self.active = self.enabled && (self.text || self.window || self.file.is_some());
        self.active
    }

    /// `fwrite` and `fflush`; write errors are ignored, as in the C++.
    fn write(&mut self, line: &[u8]) {
        if let Some(file) = &mut self.file {
            let _ = file.write_all(line);
            let _ = file.flush();
        }
    }
}

/// The channel set of `ErrorHandler`, owned by the host instead of living in
/// process globals.
pub struct LogChannels {
    /// Front to back, as the C++ list.
    channels: Vec<Channel>,
    opener: SinkOpener,
    warnings: u32,
    errors: u32,
}

impl fmt::Debug for LogChannels {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("LogChannels")
            .field("channels", &self.channels.len())
            .field("warnings", &self.warnings)
            .field("errors", &self.errors)
            .finish_non_exhaustive()
    }
}

impl LogChannels {
    /// No channels yet; files are opened through `opener`.
    #[must_use]
    pub fn new(opener: SinkOpener) -> Self {
        Self {
            channels: Vec::new(),
            opener,
            warnings: 0,
            errors: 0,
        }
    }

    /// The number of channels.
    #[must_use]
    pub fn len(&self) -> usize {
        self.channels.len()
    }

    /// Whether there are no channels.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.channels.is_empty()
    }

    /// Warnings (severity 2) seen by [`dump`](Self::dump) since
    /// [`activate`](Self::activate). The C++ counts them and never reads
    /// them.
    #[must_use]
    pub fn warnings(&self) -> u32 {
        self.warnings
    }

    /// Errors (severity 1) seen since [`activate`](Self::activate).
    #[must_use]
    pub fn errors(&self) -> u32 {
        self.errors
    }

    /// `ErrorHandler::activate` without the hook installation (the host
    /// installs a [`super::subscriber::ChannelSubscriber`]): zeroes the
    /// counters, then adds a channel for each `log = name[, id[, limit]]`
    /// line of the `[Error]` sections. `ini` is the opened file, or `None`
    /// when it could not be opened.
    pub fn activate(&mut self, ini: Option<&mut IniReader>) {
        self.warnings = 0;
        self.errors = 0;
        let Some(reader) = ini else {
            return;
        };
        if !reader.read_header() {
            return;
        }
        loop {
            if reader.is_header(b"Error") && reader.read_value() {
                loop {
                    if reader.is_value(b"log") {
                        let mut name = reader.value_string_at(0);
                        name.truncate(NAME_LIMIT);
                        let mut log_id = ANY_LOG;
                        let mut limit = DEFAULT_SEVERITY_LIMIT;
                        if !reader.is_value_empty(1) {
                            log_id = trunc_to_i32_via_i64(reader.value_float(1));
                        }
                        if !reader.is_value_empty(2) {
                            limit = trunc_to_i32_via_i64(reader.value_float(2));
                        }
                        self.new_log(&name, log_id, limit, false, false);
                    }
                    if !reader.read_value() {
                        break;
                    }
                }
            }
            if !reader.read_header() {
                break;
            }
        }
    }

    /// `ErrorHandler::deactivate`: closes and removes every channel.
    pub fn deactivate(&mut self) {
        self.channels.clear();
    }

    /// `ErrorHandler::new_log`. A name starting with `$` selects the virtual
    /// sink `$Text` or `$Window` (case-insensitive); any other non-empty
    /// name is a file, truncated to 127 bytes. Returns whether a channel was
    /// added: one without a file or a virtual sink is dropped.
    ///
    /// A channel with a `$Window` sink goes to the back of the list, every
    /// other one to the front.
    pub fn new_log(
        &mut self,
        name: &[u8],
        log_id: i32,
        severity_limit: i32,
        text: bool,
        window: bool,
    ) -> bool {
        let mut channel = Channel {
            enabled: true,
            log_id,
            severity_limit,
            active: false,
            text,
            window,
            file: None,
        };
        let name = crate::hash::c_str(name);
        if name.first() == Some(&b'$') {
            if stricmp(name, b"$Text").is_eq() {
                channel.text = true;
            } else if stricmp(name, b"$Window").is_eq() {
                channel.window = true;
            }
        } else if !name.is_empty() {
            let name = &name[..name.len().min(NAME_LIMIT)];
            channel.file = (self.opener)(name);
        }
        if !channel.refresh_active() {
            return false;
        }
        if channel.window {
            self.channels.push(channel);
        } else {
            self.channels.insert(0, channel);
        }
        true
    }

    /// `ErrorHandler::log_enable`; [`ANY_LOG`] enables every channel.
    pub fn log_enable(&mut self, log_id: i32) {
        for channel in &mut self.channels {
            if log_id == ANY_LOG || channel.log_id == log_id {
                channel.enabled = true;
                channel.refresh_active();
            }
        }
    }

    /// `ErrorHandler::log_disable`; [`ANY_LOG`] disables every channel.
    pub fn log_disable(&mut self, log_id: i32) {
        for channel in &mut self.channels {
            if log_id == ANY_LOG || channel.log_id == log_id {
                channel.enabled = false;
                channel.active = false;
            }
        }
    }

    /// `ErrorHandler::is_log_enabled`: whether the first channel registered
    /// with exactly `log_id` is active. `-1` is not a wildcard here.
    pub fn is_log_enabled(&mut self, log_id: i32) -> bool {
        self.channels
            .iter_mut()
            .find(|c| c.log_id == log_id)
            .is_some_and(Channel::refresh_active)
    }

    /// `ErrorHandler::log_comment`: writes `text` to every active channel
    /// whose log id is `log_id` ([`ANY_LOG`]: every channel).
    pub fn log_comment(&mut self, log_id: i32, text: &[u8]) {
        let line = finish_line(text);
        for channel in &mut self.channels {
            if log_id != ANY_LOG && channel.log_id != log_id {
                continue;
            }
            if !channel.refresh_active() {
                continue;
            }
            channel.write(&line);
        }
    }

    /// The `FDUMP` hook: counts warnings and errors, then writes `text` to
    /// every active channel whose log id is `-1` or the flags' category and
    /// whose severity limit is not below the severity nibble (compared
    /// unsigned). A `$Text` channel gets errors and warnings in upper case.
    pub fn dump(&mut self, flags: DumpFlags, text: &[u8]) {
        let severity = flags.severity_nibble();
        let severity_bits = u32::from_ne_bytes(severity.to_ne_bytes());
        if severity_bits <= 2 {
            if severity == 2 {
                self.warnings = self.warnings.wrapping_add(1);
            }
            if severity == 1 {
                self.errors = self.errors.wrapping_add(1);
            }
        }
        let category = flags.category();
        for channel in &mut self.channels {
            if !channel.active {
                continue;
            }
            if channel.log_id != ANY_LOG && category != channel.log_id {
                continue;
            }
            if severity_bits > u32::from_ne_bytes(channel.severity_limit.to_ne_bytes()) {
                continue;
            }
            let mut truncated = c_line(text).to_vec();
            if channel.text && severity_bits <= 2 {
                truncated.make_ascii_uppercase();
            }
            channel.write(&finish_line(&truncated));
        }
    }
}

/// The text a `%s` format leaves in the 0x400-byte buffer: up to the first
/// NUL, at most 1023 bytes.
fn c_line(text: &[u8]) -> &[u8] {
    let text = crate::hash::c_str(text);
    &text[..text.len().min(LINE_LIMIT)]
}

/// The line as written: a newline is appended when the text is not empty,
/// does not end in one, and is shorter than 511 bytes.
fn finish_line(text: &[u8]) -> Vec<u8> {
    let text = c_line(text);
    let mut line = text.to_vec();
    if !text.is_empty() && text.last() != Some(&b'\n') && text.len() + 1 < NEWLINE_BUFFER {
        line.push(b'\n');
    }
    line
}

/// An opener that creates files with `std::fs`. Names are read as UTF-8,
/// with invalid bytes replaced.
pub fn file_opener() -> SinkOpener {
    Box::new(|name: &[u8]| {
        let path = String::from_utf8_lossy(name).into_owned();
        std::fs::File::create(path)
            .ok()
            .map(|file| Box::new(file) as Sink)
    })
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::sync::{Arc, Mutex};

    use super::*;

    type Files = Arc<Mutex<BTreeMap<Vec<u8>, Vec<u8>>>>;

    struct Memory {
        name: Vec<u8>,
        files: Files,
    }

    impl Write for Memory {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            let mut files = self
                .files
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            files
                .entry(self.name.clone())
                .or_default()
                .extend_from_slice(buf);
            Ok(buf.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    fn memory() -> (LogChannels, Files) {
        let files = Files::default();
        let shared = Arc::clone(&files);
        let opener: SinkOpener = Box::new(move |name: &[u8]| {
            if name.contains(&b'?') {
                return None;
            }
            let mut map = shared
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            map.insert(name.to_vec(), Vec::new());
            Some(Box::new(Memory {
                name: name.to_vec(),
                files: Arc::clone(&shared),
            }) as Sink)
        });
        (LogChannels::new(opener), files)
    }

    fn contents(files: &Files, name: &[u8]) -> Vec<u8> {
        files
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)[name]
            .clone()
    }

    #[test]
    fn routes_by_category_and_severity_limit() {
        let (mut log, files) = memory();
        assert!(log.new_log(b"all.log", ANY_LOG, 7, false, false));
        assert!(log.new_log(b"cat.log", 0x20, 2, false, false));
        log.dump(DumpFlags::from_parts(0x20, 3), b"notice");
        log.dump(DumpFlags::from_parts(0x20, 2), b"warn");
        log.dump(DumpFlags::WARNING, b"general");
        log.dump(DumpFlags::from_bits(0x10_000F), b"negative");
        assert_eq!(contents(&files, b"all.log"), b"notice\nwarn\ngeneral\n");
        assert_eq!(contents(&files, b"cat.log"), b"warn\n");
        assert_eq!((log.warnings(), log.errors()), (2, 0));
    }

    #[test]
    fn virtual_and_failed_channels() {
        let (mut log, _) = memory();
        assert!(log.new_log(b"$text", 1, 7, false, false));
        assert!(log.new_log(b"$WINDOW", 2, 7, false, false));
        assert!(!log.new_log(b"$other", 3, 7, false, false));
        assert!(!log.new_log(b"", 4, 7, false, false));
        assert!(log.new_log(b"", 5, 7, false, true));
        assert!(!log.new_log(b"bad?name", 6, 7, false, false));
        assert_eq!(log.len(), 3);
        assert!(log.is_log_enabled(1));
        assert!(!log.is_log_enabled(-1));
        log.log_disable(ANY_LOG);
        assert!(!log.is_log_enabled(2));
        log.log_enable(2);
        assert!(log.is_log_enabled(2));
        assert!(!log.is_log_enabled(1));
    }

    #[test]
    fn text_flag_upper_cases_errors_and_warnings() {
        let (mut log, files) = memory();
        assert!(log.new_log(b"t.log", ANY_LOG, 100, true, false));
        log.dump(DumpFlags::ERROR, b"Error x");
        log.dump(DumpFlags::NOTICE, b"Notice y");
        log.log_comment(ANY_LOG, b"Comment z\n");
        assert_eq!(
            contents(&files, b"t.log"),
            b"ERROR X\nNotice y\nComment z\n".to_vec()
        );
    }

    #[test]
    fn newline_and_truncation_rules() {
        assert_eq!(finish_line(b""), b"");
        assert_eq!(finish_line(&[b'a'; 510]).len(), 511);
        assert_eq!(finish_line(&[b'a'; 511]).len(), 511);
        assert_eq!(finish_line(&[b'a'; 2000]).len(), 1023);
        assert_eq!(finish_line(b"ab\0cd"), b"ab\n");
    }

    #[test]
    fn activate_reads_error_sections() {
        let (mut log, files) = memory();
        let mut ini = IniReader::new();
        assert!(ini.open(
            b"e.ini",
            b"[Error]\nlog = a.log\nlog = b.log, 32, 2\n[Other]\nlog = c.log\n".to_vec()
        ));
        log.activate(Some(&mut ini));
        assert_eq!(log.len(), 2);
        log.dump(DumpFlags::from_parts(32, 3), b"x");
        assert_eq!(contents(&files, b"a.log"), b"x\n");
        assert_eq!(contents(&files, b"b.log"), b"");
        log.deactivate();
        assert!(log.is_empty());
    }
}
