//! A `tracing` subscriber that writes events to [`LogChannels`]: the role
//! of the `FDUMP` hook `ErrorHandler::activate` installs.
//!
//! An event from [`super::dump`] carries its flags word and its whole line.
//! Any other event is given the flags of its level in the general category
//! and the line `"<file>(<line>) : <KIND>:General:<message>"`. Spans are
//! accepted and ignored.

use std::fmt::{self, Write as _};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, PoisonError};

use tracing::field::{Field, Visit};
use tracing::span::{Attributes, Id, Record};
use tracing::{Event, Metadata, Subscriber};

use super::channels::LogChannels;
use super::{Category, DumpFlags, Severity, general_line};

/// Routes `tracing` events into a [`LogChannels`] value.
#[derive(Debug)]
pub struct ChannelSubscriber {
    channels: Mutex<LogChannels>,
    next_span: AtomicU64,
}

impl ChannelSubscriber {
    /// A subscriber writing to `channels`.
    #[must_use]
    pub fn new(channels: LogChannels) -> Self {
        Self {
            channels: Mutex::new(channels),
            next_span: AtomicU64::new(1),
        }
    }

    /// Runs `f` on the channels, for example to add one or to disable a
    /// log id.
    pub fn with_channels<R>(&self, f: impl FnOnce(&mut LogChannels) -> R) -> R {
        let mut channels = self.channels.lock().unwrap_or_else(PoisonError::into_inner);
        f(&mut channels)
    }

    /// The channels, for `deactivate`.
    #[must_use]
    pub fn into_channels(self) -> LogChannels {
        self.channels
            .into_inner()
            .unwrap_or_else(PoisonError::into_inner)
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

impl Subscriber for ChannelSubscriber {
    fn enabled(&self, _metadata: &Metadata<'_>) -> bool {
        true
    }

    fn new_span(&self, _span: &Attributes<'_>) -> Id {
        Id::from_u64(self.next_span.fetch_add(1, Ordering::Relaxed).max(1))
    }

    fn record(&self, _span: &Id, _values: &Record<'_>) {}

    fn record_follows_from(&self, _span: &Id, _follows: &Id) {}

    fn event(&self, event: &Event<'_>) {
        let mut fields = EventFields::default();
        event.record(&mut fields);
        let (flags, line) = if let Some(bits) = fields.flags {
            (
                DumpFlags::from_bits((bits & 0xFFFF_FFFF) as u32),
                fields.message,
            )
        } else {
            let metadata = event.metadata();
            let severity = Severity::from_level(*metadata.level());
            let line = general_line(
                metadata.file().unwrap_or("?"),
                metadata.line().unwrap_or(0),
                severity,
                &fields.message,
            );
            (DumpFlags::new(Category::GENERAL, severity), line)
        };
        self.with_channels(|channels| channels.dump(flags, line.as_bytes()));
    }

    fn enter(&self, _span: &Id) {}

    fn exit(&self, _span: &Id) {}
}

#[cfg(test)]
mod tests {
    use std::io::Write;
    use std::sync::Arc;

    use super::*;
    use crate::diag::channels::{ANY_LOG, Sink};
    use crate::diag::dump;

    #[derive(Clone, Default)]
    struct Shared(Arc<Mutex<Vec<u8>>>);

    impl Write for Shared {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            self.0
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .extend_from_slice(buf);
            Ok(buf.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn events_reach_the_channels() {
        let out = Shared::default();
        let sink = out.clone();
        let mut channels = LogChannels::new(Box::new(move |_: &[u8]| {
            Some(Box::new(sink.clone()) as Sink)
        }));
        assert!(channels.new_log(b"spew.txt", ANY_LOG, 3, false, false));
        let subscriber = Arc::new(ChannelSubscriber::new(channels));
        tracing::subscriber::with_default(Arc::clone(&subscriber), || {
            dump(DumpFlags::WARNING, "a(1) : WARNING:General:first");
            dump(DumpFlags::TRACE_1, "dropped by the limit");
            let _span = tracing::info_span!("section").entered();
            tracing::error!("second");
        });
        let text = String::from_utf8(out.0.lock().unwrap_or_else(PoisonError::into_inner).clone())
            .unwrap_or_default();
        let mut lines = text.lines();
        assert_eq!(lines.next(), Some("a(1) : WARNING:General:first"));
        let second = lines.next().unwrap_or_default();
        assert!(second.ends_with(") : ERROR:General:second"), "{second}");
        assert!(second.contains("subscriber.rs("), "{second}");
        assert_eq!(lines.next(), None);
        assert_eq!(
            subscriber.with_channels(|c| (c.warnings(), c.errors())),
            (1, 1)
        );
    }
}
