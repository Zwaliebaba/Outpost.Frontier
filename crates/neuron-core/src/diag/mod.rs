//! Diagnostics: the `FDUMP` replacement, the log channels behind it, the
//! host's error callbacks and the tick profiler (work package P1.5,
//! decision T12).
//!
//! In the C++ every module reports through the `FDUMP` function pointer
//! that `neuroncomponents.dll` exports. Its first argument is a flags word:
//! the low nibble is the severity, the bits above it a category. The default
//! handler shows a message box; `ErrorHandler::activate` replaces it with a
//! hook that writes to log channels (`common_errorhandler.cpp`).
//!
//! Here a report is a [`tracing`] event. [`DumpFlags`] keeps the flags word,
//! [`dump`] emits it, [`channels::LogChannels`] is the `ErrorHandler`
//! channel set and [`subscriber::ChannelSubscriber`] routes events into it.
//! The callbacks the C++ stores in process globals (`FatalErrorFn`,
//! `CriticalWarningFn`) are an explicit [`callbacks::ErrorCallbacks`]
//! value.
//!
//! C++ entry points and their replacements:
//!
//! | C++ | Rust |
//! | --- | --- |
//! | `FDUMP(flags, fmt, ...)` | [`dump`], or a `tracing` event |
//! | `DefaultDump` (message box) | none; events go to the installed subscriber |
//! | `ErrorHandler::activate` | [`channels::LogChannels::activate`] and a [`subscriber::ChannelSubscriber`] |
//! | `ErrorHandler::deactivate` | [`channels::LogChannels::deactivate`] |
//! | `ErrorHandler::new_log` | [`channels::LogChannels::new_log`] |
//! | `ErrorHandler::log_enable`, `log_disable`, `is_log_enabled` | the methods of the same names |
//! | `ErrorHandler::log_comment` | [`channels::LogChannels::log_comment`] |
//! | `ErrorHandler::log_last_error` | [`last_error_flags`] and [`dump`] with the OS message |
//! | `FatalErrorFn`, `CriticalWarningFn` | [`callbacks::ErrorReporter`] |
//! | `LogStream::LogEvent` (empty in the shipping build) | not ported |
//! | `FPhysicsTickScope`, `FClientTickScope` | [`tick_profile`] |

pub mod callbacks;
pub mod channels;
pub mod subscriber;
pub mod tick_profile;

use tracing::Level;

/// The `tracing` target of events emitted by [`dump`].
pub const DUMP_TARGET: &str = "fdump";

/// The severities the C++ passes in the low nibble of a flags word. The
/// names are the kinds its format strings print (`"%s(%d) : ERROR:..."`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Severity {
    /// 0, `FATAL` (`SceneRuntime`).
    Fatal,
    /// 1, `ERROR` and `*** ERROR`.
    Error,
    /// 2, `WARNING` and `*** WARNING`.
    Warning,
    /// 3, `NOTICE`.
    Notice,
    /// 4, `TRACE_1`.
    Trace1,
}

impl Severity {
    /// The value in the low nibble.
    #[must_use]
    pub const fn nibble(self) -> u32 {
        match self {
            Self::Fatal => 0,
            Self::Error => 1,
            Self::Warning => 2,
            Self::Notice => 3,
            Self::Trace1 => 4,
        }
    }

    /// The severity a signed nibble names, if any.
    #[must_use]
    pub const fn from_nibble(nibble: i32) -> Option<Self> {
        match nibble {
            0 => Some(Self::Fatal),
            1 => Some(Self::Error),
            2 => Some(Self::Warning),
            3 => Some(Self::Notice),
            4 => Some(Self::Trace1),
            _ => None,
        }
    }

    /// The word the C++ format strings print for this severity.
    #[must_use]
    pub const fn kind(self) -> &'static str {
        match self {
            Self::Fatal => "FATAL",
            Self::Error => "ERROR",
            Self::Warning => "WARNING",
            Self::Notice => "NOTICE",
            Self::Trace1 => "TRACE_1",
        }
    }

    /// The `tracing` level an event of this severity carries.
    #[must_use]
    pub const fn level(self) -> Level {
        match self {
            Self::Fatal | Self::Error => Level::ERROR,
            Self::Warning => Level::WARN,
            Self::Notice => Level::INFO,
            Self::Trace1 => Level::DEBUG,
        }
    }

    /// The severity a `tracing` level maps back to (`TRACE` as `TRACE_1`).
    #[must_use]
    pub fn from_level(level: Level) -> Self {
        if level == Level::ERROR {
            Self::Error
        } else if level == Level::WARN {
            Self::Warning
        } else if level == Level::INFO {
            Self::Notice
        } else {
            Self::Trace1
        }
    }
}

/// The category of a flags word, `flags >> 4`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Category(pub i32);

impl Category {
    /// The only category the C++ uses (`0x10000`, printed as `General`).
    pub const GENERAL: Self = Self(0x10000);
}

/// An `FDUMP` flags word: a category above a 4-bit severity.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct DumpFlags(u32);

impl DumpFlags {
    /// `0x100000`: a fatal report in the general category.
    pub const FATAL: Self = Self::new(Category::GENERAL, Severity::Fatal);
    /// `0x100001`.
    pub const ERROR: Self = Self::new(Category::GENERAL, Severity::Error);
    /// `0x100002`.
    pub const WARNING: Self = Self::new(Category::GENERAL, Severity::Warning);
    /// `0x100003`.
    pub const NOTICE: Self = Self::new(Category::GENERAL, Severity::Notice);
    /// `0x100004`.
    pub const TRACE_1: Self = Self::new(Category::GENERAL, Severity::Trace1);

    /// The flags for `severity` in `category`, as `PlayerDB_MakeDumpFlags`
    /// and `PackNibbleByte` build them: `((c << 4) ^ s) & 0xf ^ (c << 4)`.
    #[must_use]
    pub const fn new(category: Category, severity: Severity) -> Self {
        Self::from_parts(category.0, severity.nibble())
    }

    /// The flags for a raw category and level; only the low nibble of
    /// `level` is kept.
    #[must_use]
    pub const fn from_parts(category: i32, level: u32) -> Self {
        let high = (category as u32) << 4;
        Self(((high ^ level) & 0xF) ^ high)
    }

    /// A flags word as the C++ passes it.
    #[must_use]
    pub const fn from_bits(bits: u32) -> Self {
        Self(bits)
    }

    /// The flags word.
    #[must_use]
    pub const fn bits(self) -> u32 {
        self.0
    }

    /// The severity nibble, sign-extended (`SHL 0x1c; SAR 0x1c`).
    #[must_use]
    pub const fn severity_nibble(self) -> i32 {
        ((self.0 << 28) as i32) >> 28
    }

    /// The severity, when the nibble names one.
    #[must_use]
    pub const fn severity(self) -> Option<Severity> {
        Severity::from_nibble(self.severity_nibble())
    }

    /// `flags >> 4` on the signed word: the value log channels route on.
    #[must_use]
    pub const fn category(self) -> i32 {
        (self.0 as i32) >> 4
    }

    /// The `tracing` level for these flags. Nibbles outside 0 to 4 map to
    /// `TRACE`.
    #[must_use]
    pub const fn level(self) -> Level {
        match self.severity() {
            Some(severity) => severity.level(),
            None => Level::TRACE,
        }
    }
}

/// The flags `ErrorHandler::log_last_error` passes: `(severity & 0xf) |
/// 0x100000`.
#[must_use]
pub const fn last_error_flags(severity: u32) -> DumpFlags {
    DumpFlags((severity & 0xF) | 0x10_0000)
}

/// Reports `message` with `flags`: one `tracing` event at the flags' level,
/// target [`DUMP_TARGET`], with the flags word in the field `flags`.
/// `message` is the whole line, as the C++ formats it.
pub fn dump(flags: DumpFlags, message: &str) {
    let bits = u64::from(flags.bits());
    match flags.level() {
        Level::ERROR => tracing::error!(target: DUMP_TARGET, flags = bits, "{message}"),
        Level::WARN => tracing::warn!(target: DUMP_TARGET, flags = bits, "{message}"),
        Level::INFO => tracing::info!(target: DUMP_TARGET, flags = bits, "{message}"),
        Level::DEBUG => tracing::debug!(target: DUMP_TARGET, flags = bits, "{message}"),
        _ => tracing::trace!(target: DUMP_TARGET, flags = bits, "{message}"),
    }
}

/// Formats a report the way most C++ call sites do:
/// `"<file>(<line>) : <KIND>:General:<message>"`.
#[must_use]
pub fn general_line(file: &str, line: u32, severity: Severity, message: &str) -> String {
    format!("{file}({line}) : {}:General:{message}", severity.kind())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flags_match_the_cpp_constants() {
        assert_eq!(DumpFlags::FATAL.bits(), 0x10_0000);
        assert_eq!(DumpFlags::ERROR.bits(), 0x10_0001);
        assert_eq!(DumpFlags::WARNING.bits(), 0x10_0002);
        assert_eq!(DumpFlags::NOTICE.bits(), 0x10_0003);
        assert_eq!(DumpFlags::TRACE_1.bits(), 0x10_0004);
        assert_eq!(DumpFlags::ERROR.category(), 0x10000);
        assert_eq!(DumpFlags::ERROR.severity(), Some(Severity::Error));
    }

    #[test]
    fn nibble_is_signed() {
        let flags = DumpFlags::from_bits(0x10_000F);
        assert_eq!(flags.severity_nibble(), -1);
        assert_eq!(flags.severity(), None);
        assert_eq!(flags.level(), Level::TRACE);
        assert_eq!(DumpFlags::from_bits(0x8000_0000).category(), -0x0800_0000);
    }

    #[test]
    fn make_flags_keeps_only_the_low_nibble_of_the_level() {
        assert_eq!(DumpFlags::from_parts(0x10000, 0x12).bits(), 0x10_0002);
        assert_eq!(last_error_flags(0x12).bits(), 0x10_0002);
    }

    #[test]
    fn general_line_matches_the_cpp_format() {
        assert_eq!(
            general_line("Common\\CTiming.cpp", 0x56, Severity::Warning, "x"),
            "Common\\CTiming.cpp(86) : WARNING:General:x"
        );
    }
}
