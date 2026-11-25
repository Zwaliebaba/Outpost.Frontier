//! The dedicated server's log (`ServerLogf`,
//! `OutpostServer/OutpostServerView.cpp:177-252`, and the log settings of
//! `FServerControl`, `OutpostServer/ServerControl.cpp:150-235`).
//!
//! Every report reaches the log as an `FDUMP` flags word and a line. A
//! line passes when its signed level nibble is at most the log level
//! (1 to 7, 4 by default) and, unless its level is 0, when its category
//! (`flags >> 4`) is one of the listed categories (`0x10000` by default;
//! the command line could add more). A passing line is cut to 0x400 bytes,
//! gets a newline when it has none, and goes to the console, to the log
//! file when one is open (flushed after each line) and, at levels 0 and 1,
//! to the error log, whose first line in a process is a header with the
//! version and whose lines are numbered in hexadecimal.
//!
//! The C++ writes the console into a rich-edit view (coloured by level,
//! trimmed at `MaxConsoleLines`) and `OutputDebugString`; the headless
//! server writes it to a stream, the standard output. [`ServerLogSubscriber`]
//! is the `tracing` subscriber that hands events to a [`ServerLog`], as the
//! view's constructor puts `ServerLogf` in place of `FDUMP`.

use std::fmt::{self, Write as _};
use std::fs::OpenOptions;
use std::io::{self, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, PoisonError};

use neuron_core::diag::{Category, DumpFlags, Severity};
use tracing::field::{Field, Visit};
use tracing::span::{Attributes, Id, Record};
use tracing::{Event, Level, Metadata, Subscriber};

/// `FServerControl::LogLevel`'s start value.
pub const DEFAULT_LOG_LEVEL: i32 = 4;

/// The size of `ServerLogf`'s line buffer.
pub const LINE_CAPACITY: usize = 0x400;

/// The flags of the application's own notices (`ErrorCode{0x100004}`).
pub const APP_NOTICE: DumpFlags = DumpFlags::TRACE_1;

/// Which lines the log shows (`LogLevel`, `LogCategories`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LogFilter {
    level: i32,
    categories: Vec<i32>,
}

impl Default for LogFilter {
    /// Level 4 and the general category (`FServerControl::FServerControl`).
    fn default() -> Self {
        Self {
            level: DEFAULT_LOG_LEVEL,
            categories: vec![Category::GENERAL.0],
        }
    }
}

impl LogFilter {
    /// The level.
    #[must_use]
    pub const fn level(&self) -> i32 {
        self.level
    }

    /// The listed categories, the general one first.
    #[must_use]
    pub fn categories(&self) -> &[i32] {
        &self.categories
    }

    /// `SetLogLevel`: 1 to 7 is taken; any other value is refused and the
    /// level kept.
    pub fn set_level(&mut self, level: i32) -> bool {
        if (1..8).contains(&level) {
            self.level = level;
            true
        } else {
            false
        }
    }

    /// `AddLogCategory`: appended, duplicates included.
    pub fn add_category(&mut self, category: i32) {
        self.categories.push(category);
    }

    /// `ServerLogf`'s two tests.
    #[must_use]
    pub fn accepts(&self, flags: DumpFlags) -> bool {
        let level = flags.severity_nibble();
        if level > self.level {
            return false;
        }
        level == 0 || self.categories.contains(&flags.category())
    }
}

/// `_vsnprintf(buffer, 0x400, ...)` and the newline `ServerLogf` appends:
/// the text cut to 0x400 bytes (on a character boundary), then a newline
/// when it does not end with one and there is room.
#[must_use]
pub fn format_line(text: &str) -> String {
    let mut end = text.len().min(LINE_CAPACITY);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    let mut line = text[..end].to_owned();
    if !line.ends_with('\n') && line.len() < LINE_CAPACITY {
        line.push('\n');
    }
    line
}

/// The version the error log's header names (`GServerVersion`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ServerVersion {
    /// The major number.
    pub major: u8,
    /// The minor number.
    pub minor: u8,
    /// The build.
    pub build: u16,
}

/// The error log (`c:\OutpostServerErrors.log` in the C++): appended to,
/// one header per process, each line numbered.
#[derive(Debug)]
pub struct ErrorLog {
    path: PathBuf,
    version: ServerVersion,
    next_line: u32,
}

impl ErrorLog {
    /// The error log at `path`.
    #[must_use]
    pub const fn new(path: PathBuf, version: ServerVersion) -> Self {
        Self {
            path,
            version,
            next_line: 1,
        }
    }

    /// The text one line appends: the header first in a process, then
    /// `"%6x: %s\n"`, whose line already ends with its newline.
    fn entry(&mut self, line: &str) -> String {
        let mut text = String::new();
        if self.next_line == 1 {
            let v = self.version;
            let _ = writeln!(
                text,
                "****** New Log (Version {}.{} Build {}) ******",
                v.major, v.minor, v.build
            );
        }
        let _ = writeln!(text, "{:6x}: {line}", self.next_line);
        self.next_line += 1;
        text
    }

    /// Appends `line`; a file that cannot be opened is skipped, the line
    /// number advances only when it was written, as the C++'s `fopen`
    /// test does.
    fn append(&mut self, line: &str) {
        let Ok(mut file) = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)
        else {
            return;
        };
        let text = self.entry(line);
        let _ = file.write_all(text.as_bytes());
    }
}

/// Where the log writes.
pub struct ServerLog {
    filter: LogFilter,
    console: Box<dyn Write + Send>,
    file: Option<Box<dyn Write + Send>>,
    errors: Option<ErrorLog>,
}

impl fmt::Debug for ServerLog {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ServerLog")
            .field("filter", &self.filter)
            .field("file", &self.file.is_some())
            .field("errors", &self.errors)
            .finish_non_exhaustive()
    }
}

impl ServerLog {
    /// A log writing to `console` with `filter`, without a log file or an
    /// error log.
    #[must_use]
    pub fn new(console: Box<dyn Write + Send>, filter: LogFilter) -> Self {
        Self {
            filter,
            console,
            file: None,
            errors: None,
        }
    }

    /// `OpenLog`: the log file, truncated (`std::ios::out`).
    ///
    /// # Errors
    ///
    /// The file cannot be created.
    pub fn open_file(&mut self, path: &std::path::Path) -> io::Result<()> {
        let file = std::fs::File::create(path)?;
        self.file = Some(Box::new(file));
        Ok(())
    }

    /// A log file that is a stream already, for tests.
    pub fn set_file(&mut self, file: Box<dyn Write + Send>) {
        self.file = Some(file);
    }

    /// The error log of the lines at levels 0 and 1.
    pub fn set_error_log(&mut self, errors: ErrorLog) {
        self.errors = Some(errors);
    }

    /// The filter.
    #[must_use]
    pub const fn filter(&self) -> &LogFilter {
        &self.filter
    }

    /// The filter, to change.
    pub fn filter_mut(&mut self) -> &mut LogFilter {
        &mut self.filter
    }

    /// `ServerLogf(flags, text)`: true when the line was shown.
    pub fn write(&mut self, flags: DumpFlags, text: &str) -> bool {
        if !self.filter.accepts(flags) {
            return false;
        }
        let line = format_line(text);
        let _ = self.console.write_all(line.as_bytes());
        let _ = self.console.flush();
        if let Some(file) = self.file.as_mut() {
            let _ = file.write_all(line.as_bytes());
            let _ = file.flush();
        }
        if flags.severity_nibble() == 0 || flags.severity_nibble() == 1 {
            if let Some(errors) = self.errors.as_mut() {
                errors.append(&line);
            }
        }
        true
    }

    /// The application's start-up notices, which the C++ logs between
    /// `SetLogLevel(4)` and the restore of the saved level: shown at level
    /// 4 whatever the configured level.
    pub fn write_notice(&mut self, text: &str) -> bool {
        let saved = self.filter.level;
        self.filter.level = DEFAULT_LOG_LEVEL;
        let shown = self.write(APP_NOTICE, text);
        self.filter.level = saved;
        shown
    }
}

/// The `tracing` target of the application's start-up notices
/// ([`ServerLog::write_notice`]).
pub const NOTICE_TARGET: &str = "server_notice";

/// The crates whose events below a warning reach the log; the network
/// libraries' own diagnostics stay out.
const OWN_TARGETS: &[&str] = &[
    "neuron_",
    "game_",
    "outpost",
    "OutpostServer",
    neuron_core::diag::DUMP_TARGET,
    NOTICE_TARGET,
];

/// A `tracing` subscriber writing to a [`ServerLog`]. An event from
/// [`neuron_core::diag::dump`] carries its flags word; any other event is
/// given its level's flags in the general category. Events below a
/// warning from crates outside the workspace are not enabled. Spans are
/// accepted and ignored.
#[derive(Debug)]
pub struct ServerLogSubscriber {
    log: Mutex<ServerLog>,
    next_span: AtomicU64,
}

impl ServerLogSubscriber {
    /// A subscriber writing to `log`.
    #[must_use]
    pub fn new(log: ServerLog) -> Self {
        Self {
            log: Mutex::new(log),
            next_span: AtomicU64::new(1),
        }
    }

    /// Runs `f` on the log.
    pub fn with_log<R>(&self, f: impl FnOnce(&mut ServerLog) -> R) -> R {
        let mut log = self.log.lock().unwrap_or_else(PoisonError::into_inner);
        f(&mut log)
    }
}

#[derive(Default)]
struct EventFields {
    flags: Option<u64>,
    message: String,
}

impl Visit for EventFields {
    fn record_u64(&mut self, field: &Field, value: u64) {
        if field.name() == "flags" {
            self.flags = Some(value);
        }
    }

    fn record_i64(&mut self, field: &Field, value: i64) {
        if field.name() == "flags" {
            self.flags = Some(u64::from_ne_bytes(value.to_ne_bytes()));
        }
    }

    fn record_str(&mut self, field: &Field, value: &str) {
        if field.name() == "message" {
            self.message.push_str(value);
        }
    }

    fn record_debug(&mut self, field: &Field, value: &dyn fmt::Debug) {
        if field.name() == "message" {
            let _ = write!(self.message, "{value:?}");
        }
    }
}

impl Subscriber for ServerLogSubscriber {
    fn enabled(&self, metadata: &Metadata<'_>) -> bool {
        *metadata.level() <= Level::WARN
            || OWN_TARGETS
                .iter()
                .any(|prefix| metadata.target().starts_with(prefix))
    }

    fn new_span(&self, _span: &Attributes<'_>) -> Id {
        Id::from_u64(self.next_span.fetch_add(1, Ordering::Relaxed).max(1))
    }

    fn record(&self, _span: &Id, _values: &Record<'_>) {}

    fn record_follows_from(&self, _span: &Id, _follows: &Id) {}

    fn event(&self, event: &Event<'_>) {
        let mut fields = EventFields::default();
        event.record(&mut fields);
        let notice = event.metadata().target() == NOTICE_TARGET;
        let flags = fields.flags.map_or_else(
            || {
                DumpFlags::new(
                    Category::GENERAL,
                    Severity::from_level(*event.metadata().level()),
                )
            },
            |bits| DumpFlags::from_bits((bits & 0xFFFF_FFFF) as u32),
        );
        self.with_log(|log| {
            if notice {
                log.write_notice(&fields.message);
            } else {
                log.write(flags, &fields.message);
            }
        });
    }

    fn enter(&self, _span: &Id) {}

    fn exit(&self, _span: &Id) {}
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::*;

    #[derive(Clone, Default)]
    struct Shared(Arc<Mutex<Vec<u8>>>);

    impl Write for Shared {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            self.0.lock().unwrap().extend_from_slice(buf);
            Ok(buf.len())
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    impl Shared {
        fn text(&self) -> String {
            String::from_utf8(self.0.lock().unwrap().clone()).unwrap()
        }
    }

    #[test]
    fn the_filter_tests_the_level_then_the_category() {
        let mut filter = LogFilter::default();
        assert!(filter.accepts(DumpFlags::TRACE_1));
        assert!(filter.accepts(DumpFlags::NOTICE));
        // Level 5 is above the default.
        assert!(!filter.accepts(DumpFlags::from_parts(0x10000, 5)));
        // A level-2 line of another category is filtered; level 0 is not.
        let command_line = DumpFlags::from_bits(0x10_0052);
        assert_eq!(command_line.category(), 0x10005);
        assert!(!filter.accepts(command_line));
        assert!(filter.accepts(DumpFlags::from_parts(0x10005, 0)));
        filter.add_category(0x10005);
        assert!(filter.accepts(command_line));
        // A negative nibble is below every level.
        assert!(filter.accepts(DumpFlags::from_bits(0x10_000F)));
        assert!(!filter.set_level(0));
        assert!(!filter.set_level(8));
        assert!(filter.set_level(1));
        assert!(!filter.accepts(DumpFlags::WARNING));
        assert!(filter.accepts(DumpFlags::ERROR));
    }

    #[test]
    fn lines_get_one_newline_and_are_cut() {
        assert_eq!(format_line("a"), "a\n");
        assert_eq!(format_line("a\n"), "a\n");
        assert_eq!(format_line(""), "\n");
        let long = "x".repeat(0x500);
        assert_eq!(format_line(&long).len(), LINE_CAPACITY);
        // A three-byte character straddles the cut: 1023 bytes stay and the
        // newline fits.
        let long = "€".repeat(0x200);
        let cut = format_line(&long);
        assert_eq!(cut.len(), LINE_CAPACITY);
        assert!(cut.ends_with('\n'));
    }

    #[test]
    fn the_log_writes_console_file_and_errors() {
        let console = Shared::default();
        let file = Shared::default();
        let dir = std::env::temp_dir().join(format!("outpost-log-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let errors = dir.join("errors.log");
        let _ = std::fs::remove_file(&errors);
        let mut log = ServerLog::new(Box::new(console.clone()), LogFilter::default());
        log.set_file(Box::new(file.clone()));
        let version = ServerVersion {
            major: 1,
            minor: 0,
            build: 11,
        };
        log.set_error_log(ErrorLog::new(errors.clone(), version));
        assert!(log.write(DumpFlags::NOTICE, "hello"));
        assert!(!log.write(DumpFlags::from_parts(0x10000, 6), "hidden"));
        assert!(log.write(DumpFlags::ERROR, "bad\n"));
        assert!(log.write(DumpFlags::FATAL, "worse"));
        assert_eq!(console.text(), "hello\nbad\nworse\n");
        assert_eq!(file.text(), console.text());
        let written = std::fs::read_to_string(&errors).unwrap();
        assert_eq!(
            written,
            "****** New Log (Version 1.0 Build 11) ******\n     1: bad\n\n     2: worse\n\n"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn notices_show_at_level_four_whatever_the_level() {
        let console = Shared::default();
        let mut filter = LogFilter::default();
        filter.set_level(1);
        let mut log = ServerLog::new(Box::new(console.clone()), filter);
        assert!(!log.write(APP_NOTICE, "quiet"));
        assert!(log.write_notice("Loading initial data..."));
        assert_eq!(log.filter().level(), 1);
        assert_eq!(console.text(), "Loading initial data...\n");
    }

    #[test]
    fn the_subscriber_routes_dumps_and_events() {
        let console = Shared::default();
        let subscriber = ServerLogSubscriber::new(ServerLog::new(
            Box::new(console.clone()),
            LogFilter::default(),
        ));
        tracing::subscriber::with_default(subscriber, || {
            neuron_core::diag::dump(DumpFlags::NOTICE, "LOG: Create account x");
            neuron_core::diag::dump(DumpFlags::from_bits(0x10_0052), "filtered");
            tracing::info!(target: "neuron_server::net", "a notice");
            tracing::info!(target: "quinn::endpoint", "not ours");
            tracing::warn!(target: "quinn::endpoint", "a warning");
            tracing::trace!(target: NOTICE_TARGET, "Server running, ready for log in.");
        });
        assert_eq!(
            console.text(),
            "LOG: Create account x\na notice\na warning\nServer running, ready for log in.\n"
        );
    }
}
