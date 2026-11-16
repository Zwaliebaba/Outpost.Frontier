//! The host's error callbacks: `FatalErrorFn` and `CriticalWarningFn`,
//! which `NeuronRuntime` exports as process globals and the executables set
//! at start-up (`neuronruntime_core.h`).
//!
//! The C++ hosts load a caption and a message by resource id, append the
//! caller's text, and show a message box. The client's fatal handler then
//! exits; the critical warning asks Yes/No and exits on No. Here the host
//! implements [`ErrorReporter`] and hands an [`ErrorCallbacks`] value to the
//! code that reports; library code returns an error after a fatal report
//! instead of relying on the host to end the process.
//!
//! `ErrorHandler::activate` also stores an assert handler. Nothing in the
//! C++ ever calls it, so it has no counterpart.

use std::fmt;

/// Resource ids the callbacks receive or load.
pub mod ids {
    /// The caption of a fatal error box.
    pub const CAPTION_FATAL: u32 = 0xD1;
    /// The caption of a critical warning box.
    pub const CAPTION_CRITICAL_WARNING: u32 = 0xD2;
    /// Critical warning: the sound system did not start
    /// (`NeuronRuntime::Startup`).
    pub const WARNING_AUDIO_STARTUP: u32 = 0xD3;
    /// Critical warning: the network transport could not be created.
    pub const WARNING_NETWORK_STARTUP: u32 = 0xD4;
    /// Fatal error: a required file could not be opened
    /// (`INI_Reader::open` with `bFatal`); the text is the path.
    pub const FATAL_FILE_OPEN: u32 = 200;
}

/// The caption the client shows when resource [`ids::CAPTION_FATAL`] is
/// missing.
pub const FALLBACK_FATAL_CAPTION: &str = "OutpostFrontier Fatal Error";
/// The message the client shows when the fatal message id is missing.
pub const FALLBACK_FATAL_MESSAGE: &str =
    "OutpostFrontier has encountered a fatal error and will now terminate.\n";
/// The caption shown when resource [`ids::CAPTION_CRITICAL_WARNING`] is
/// missing.
pub const FALLBACK_WARNING_CAPTION: &str = "OutpostFrontier Critical Warning";
/// The message shown when the critical warning id is missing.
pub const FALLBACK_WARNING_MESSAGE: &str = "OutpostFrontier has encountered an unknown issue.\n";

/// The box text the hosts build: the message, then `"\n\n"` and the
/// caller's text when there is one.
#[must_use]
pub fn compose_message(message: &str, text: Option<&str>) -> String {
    match text {
        Some(text) => format!("{message}\n\n{text}"),
        None => message.to_owned(),
    }
}

/// What the host does with a report that the C++ sends through
/// `FatalErrorFn` or `CriticalWarningFn`.
pub trait ErrorReporter: Send + Sync {
    /// A fatal error: message resource `id` with optional `text`. The C++
    /// client shows it and exits; the caller of this method stops what it
    /// was doing and returns an error.
    fn fatal_error(&self, id: u32, text: Option<&str>);

    /// A critical warning. Returns whether to continue; the C++ hosts exit
    /// instead of returning `false`.
    fn critical_warning(&self, id: u32, text: Option<&str>) -> bool;
}

/// The reporter used when the host sets none: both reports become `tracing`
/// errors, and a critical warning continues.
#[derive(Clone, Copy, Debug, Default)]
pub struct TracingReporter;

impl ErrorReporter for TracingReporter {
    fn fatal_error(&self, id: u32, text: Option<&str>) {
        tracing::error!(id, text = text.unwrap_or(""), "fatal error");
    }

    fn critical_warning(&self, id: u32, text: Option<&str>) -> bool {
        tracing::error!(id, text = text.unwrap_or(""), "critical warning");
        true
    }
}

/// The callbacks a host registers, passed explicitly to the code that
/// reports.
pub struct ErrorCallbacks {
    reporter: Box<dyn ErrorReporter>,
}

impl fmt::Debug for ErrorCallbacks {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ErrorCallbacks").finish_non_exhaustive()
    }
}

impl Default for ErrorCallbacks {
    fn default() -> Self {
        Self::new(Box::new(TracingReporter))
    }
}

impl ErrorCallbacks {
    /// Callbacks that report to `reporter`.
    #[must_use]
    pub fn new(reporter: Box<dyn ErrorReporter>) -> Self {
        Self { reporter }
    }

    /// Replaces the reporter, as the executables assign the globals.
    pub fn set_reporter(&mut self, reporter: Box<dyn ErrorReporter>) {
        self.reporter = reporter;
    }

    /// `FatalErrorFn(id, text)`.
    pub fn fatal_error(&self, id: u32, text: Option<&str>) {
        self.reporter.fatal_error(id, text);
    }

    /// `CriticalWarningFn(id, text)`.
    pub fn critical_warning(&self, id: u32, text: Option<&str>) -> bool {
        self.reporter.critical_warning(id, text)
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use super::*;

    #[derive(Default)]
    struct Recorder(Mutex<Vec<(u32, Option<String>)>>);

    impl ErrorReporter for Recorder {
        fn fatal_error(&self, id: u32, text: Option<&str>) {
            if let Ok(mut calls) = self.0.lock() {
                calls.push((id, text.map(str::to_owned)));
            }
        }

        fn critical_warning(&self, id: u32, text: Option<&str>) -> bool {
            self.fatal_error(id, text);
            false
        }
    }

    #[test]
    fn callbacks_forward_to_the_reporter() {
        let callbacks = ErrorCallbacks::new(Box::new(Recorder::default()));
        callbacks.fatal_error(ids::FATAL_FILE_OPEN, Some("missing.ini"));
        assert!(!callbacks.critical_warning(ids::WARNING_AUDIO_STARTUP, None));
        assert!(ErrorCallbacks::default().critical_warning(ids::WARNING_AUDIO_STARTUP, None));
    }

    #[test]
    fn message_composition() {
        assert_eq!(compose_message("a\n", Some("b")), "a\n\n\nb");
        assert_eq!(
            compose_message(FALLBACK_WARNING_MESSAGE, None),
            FALLBACK_WARNING_MESSAGE
        );
    }
}
