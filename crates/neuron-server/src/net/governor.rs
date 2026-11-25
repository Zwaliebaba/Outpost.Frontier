//! The server's send batching and the link saturation its observers read
//! (`CDPClientProxy`, `NeuronRuntime/CDPClientProxy.cpp`; decision T17).
//!
//! - [`Batcher`]: `SendMsgs` (`:46-153`) collects queued messages into
//!   batches of at most [`BATCH_LIMIT`] message bytes; a message that does
//!   not fit closes the batch, and one larger than the limit goes alone.
//!   Each batch is deflated when that makes it smaller ([`Compression`]).
//! - [`SaturationGovernor`]: `UpdateQueueHistory` (`:186-202`) samples the
//!   bytes waiting to be sent at most once a second into a three-slot
//!   history, seeded with 1000 bytes per slot; the saturation is the
//!   integer mean of the history times 0.0005, so a mean of 1800 queued
//!   bytes reaches the 0.9 at which `HasBandwidth` stops the observers
//!   (`Server/server_observer.cpp:70-76`).

use std::time::{Duration, Instant};

use neuron_core::net::frame::{
    BatchEncoder, Compression, FrameError, MAX_MESSAGE_LEN, encode_batch,
};
use neuron_core::net::wire::HEADER_LEN;

/// Message bytes in one batch: `CDPClientProxy::SendMsgs`' `Batch[0x400]`.
pub const BATCH_LIMIT: usize = 0x400;

/// Saturation per queued byte (`UpdateQueueHistory`).
pub const SATURATION_PER_BYTE: f64 = 0.0005;

/// The history's seed (`QueueHistory[3] = {1000, 1000, 1000}`).
pub const HISTORY_SEED: u32 = 1000;

/// How often the history takes a sample: once `timeGetTime` has moved on
/// by more than 1000 ms.
pub const SAMPLE_INTERVAL: Duration = Duration::from_millis(1000);

/// The batch being filled for one connection.
#[derive(Debug, Default)]
pub struct Batcher {
    batch: BatchEncoder,
    message_bytes: usize,
}

impl Batcher {
    /// An empty batch.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            batch: BatchEncoder::new(),
            message_bytes: 0,
        }
    }

    /// Messages waiting in the batch.
    #[must_use]
    pub const fn pending_messages(&self) -> usize {
        self.batch.message_count()
    }

    /// Message bytes waiting in the batch, without the length prefixes.
    #[must_use]
    pub const fn pending_bytes(&self) -> usize {
        self.message_bytes
    }

    /// Adds one encoded message. Frames that are complete as a result, in
    /// order, are appended to `ready`: the batch the message closed, then
    /// the message alone when it is larger than [`BATCH_LIMIT`].
    ///
    /// # Errors
    /// [`FrameError::MessageTooShort`] or [`FrameError::TooLong`] for a
    /// message the framing cannot carry; nothing changes then.
    pub fn push(&mut self, message: &[u8], ready: &mut Vec<Vec<u8>>) -> Result<(), FrameError> {
        if message.len() < HEADER_LEN {
            return Err(FrameError::MessageTooShort(message.len()));
        }
        if message.len() > MAX_MESSAGE_LEN {
            return Err(FrameError::TooLong {
                length: message.len(),
                max: MAX_MESSAGE_LEN,
            });
        }
        if self.message_bytes + message.len() > BATCH_LIMIT {
            if let Some(frame) = self.flush() {
                ready.push(frame);
            }
            if message.len() > BATCH_LIMIT {
                ready.push(encode_batch([message], Compression::IfSmaller)?);
                return Ok(());
            }
        }
        self.batch.push(message)?;
        self.message_bytes += message.len();
        Ok(())
    }

    /// The frame of the batch so far, if it holds a message.
    pub fn flush(&mut self) -> Option<Vec<u8>> {
        if self.batch.is_empty() {
            return None;
        }
        self.message_bytes = 0;
        std::mem::take(&mut self.batch)
            .finish(Compression::IfSmaller)
            .ok()
    }
}

/// `CDPClientProxy`'s queue history and link saturation.
#[derive(Clone, Debug)]
pub struct SaturationGovernor {
    history: [u32; 3],
    last_sample: Instant,
    interval: Duration,
    saturation: f64,
}

impl SaturationGovernor {
    /// A connection's governor, created at `now` (the proxy's constructor
    /// sets `LastHistoryUpdateTime`), sampling every `interval`
    /// ([`SAMPLE_INTERVAL`] in the game).
    #[must_use]
    pub const fn new(now: Instant, interval: Duration) -> Self {
        Self {
            history: [HISTORY_SEED; 3],
            last_sample: now,
            interval,
            saturation: 0.0,
        }
    }

    /// `UpdateQueueHistory`: when more than the interval has passed since
    /// the last sample, shifts `queued_bytes` into the history and
    /// recomputes the saturation.
    pub fn update(&mut self, now: Instant, queued_bytes: u64) {
        if now.saturating_duration_since(self.last_sample) <= self.interval {
            return;
        }
        self.last_sample = now;
        self.history = [
            u32::try_from(queued_bytes).unwrap_or(u32::MAX),
            self.history[0],
            self.history[1],
        ];
        // `unsigned long` arithmetic: the sum wraps, the mean truncates.
        let sum = self.history[0]
            .wrapping_add(self.history[1])
            .wrapping_add(self.history[2]);
        self.saturation = f64::from(sum / 3) * SATURATION_PER_BYTE;
    }

    /// `GetLinkSaturation`.
    #[must_use]
    pub const fn saturation(&self) -> f64 {
        self.saturation
    }

    /// The history, newest first.
    #[must_use]
    pub const fn history(&self) -> [u32; 3] {
        self.history
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::net::frame::{Frame, decode_frame};

    fn message(opcode: u8, length: usize) -> Vec<u8> {
        let mut bytes = vec![opcode, 1];
        bytes.extend((0..length - 2).map(|i| (i * 7 + usize::from(opcode)) as u8));
        bytes
    }

    fn messages_of(frame: &[u8]) -> Vec<Vec<u8>> {
        let (decoded, used) = decode_frame(frame).unwrap().unwrap();
        assert_eq!(used, frame.len());
        let Frame::Batch(messages) = decoded else {
            panic!("not a batch: {decoded:?}");
        };
        messages
    }

    #[test]
    fn batches_close_at_the_limit() {
        let mut batcher = Batcher::new();
        let mut ready = Vec::new();
        let a = message(1, 0x200);
        let b = message(2, 0x200);
        let c = message(3, 2);
        batcher.push(&a, &mut ready).unwrap();
        batcher.push(&b, &mut ready).unwrap();
        // Exactly the limit fits (`BatchSize + Size > sizeof(Batch)`).
        assert!(ready.is_empty());
        assert_eq!(batcher.pending_bytes(), BATCH_LIMIT);
        assert_eq!(batcher.pending_messages(), 2);
        batcher.push(&c, &mut ready).unwrap();
        assert_eq!(ready.len(), 1);
        assert_eq!(messages_of(&ready[0]), [a, b]);
        assert_eq!(batcher.pending_messages(), 1);
        let rest = batcher.flush().unwrap();
        assert_eq!(messages_of(&rest), [c]);
        assert!(batcher.flush().is_none());
        assert_eq!(batcher.pending_bytes(), 0);
    }

    #[test]
    fn a_large_message_goes_alone_after_the_batch() {
        let mut batcher = Batcher::new();
        let mut ready = Vec::new();
        let small = message(1, 10);
        let large = message(2, BATCH_LIMIT + 1);
        batcher.push(&small, &mut ready).unwrap();
        batcher.push(&large, &mut ready).unwrap();
        assert_eq!(ready.len(), 2);
        assert_eq!(messages_of(&ready[0]), [small]);
        assert_eq!(messages_of(&ready[1]), [large]);
        assert_eq!(batcher.pending_messages(), 0);

        let mut ready = Vec::new();
        assert_eq!(
            batcher.push(&[1], &mut ready),
            Err(FrameError::MessageTooShort(1))
        );
        assert!(
            batcher
                .push(&vec![0; MAX_MESSAGE_LEN + 1], &mut ready)
                .is_err()
        );
        assert!(ready.is_empty());
    }

    #[test]
    fn saturation_follows_the_cpp_history() {
        let start = Instant::now();
        let mut governor = SaturationGovernor::new(start, SAMPLE_INTERVAL);
        assert_eq!(governor.saturation(), 0.0);
        // Not more than the interval yet.
        governor.update(start + SAMPLE_INTERVAL, 5000);
        assert_eq!(governor.history(), [1000; 3]);
        let first = start + SAMPLE_INTERVAL + Duration::from_millis(1);
        governor.update(first, 3400);
        assert_eq!(governor.history(), [3400, 1000, 1000]);
        // (3400 + 2000) / 3 = 1800 -> 0.9.
        assert_eq!(governor.saturation(), 1800.0 * 0.0005);
        governor.update(first + Duration::from_millis(500), 0);
        assert_eq!(governor.history(), [3400, 1000, 1000]);
        let second = first + Duration::from_millis(1001);
        governor.update(second, 1);
        // 4401 / 3 truncates to 1467.
        assert_eq!(governor.saturation(), 1467.0 * 0.0005);
        let mut now = second;
        for _ in 0..3 {
            now += Duration::from_millis(1001);
            governor.update(now, 0);
        }
        assert_eq!(governor.saturation(), 0.0);
        now += Duration::from_millis(1001);
        governor.update(now, u64::MAX);
        assert_eq!(governor.history()[0], u32::MAX);
    }
}
