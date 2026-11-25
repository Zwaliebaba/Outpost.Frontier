//! The hand-over from a polling thread to the I/O thread of a QUIC
//! transport (decisions T9, T17, work package P3.8).
//!
//! The game or server thread pushes without blocking; a task on the I/O
//! thread's async runtime awaits the next item. It is a mutex, a queue and
//! the waker of the one task that waits, using the standard library only,
//! so the transports need no channel crate and the runtime no
//! synchronisation feature.

use std::collections::VecDeque;
use std::future::poll_fn;
use std::sync::{Mutex, MutexGuard, PoisonError};
use std::task::{Poll, Waker};

/// A queue one or more threads push to and one async task pops from.
#[derive(Debug)]
pub struct WakeQueue<T> {
    state: Mutex<State<T>>,
}

#[derive(Debug)]
struct State<T> {
    items: VecDeque<T>,
    waker: Option<Waker>,
    closed: bool,
}

impl<T> Default for WakeQueue<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> WakeQueue<T> {
    /// An empty, open queue.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            state: Mutex::new(State {
                items: VecDeque::new(),
                waker: None,
                closed: false,
            }),
        }
    }

    fn lock(&self) -> MutexGuard<'_, State<T>> {
        // A panic while the lock is held leaves the queue itself intact.
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Appends `item` and wakes the waiting task. Never blocks for longer
    /// than the other side's push or pop. Gives the item back when the
    /// queue is closed.
    ///
    /// # Errors
    /// The item, when the queue was closed.
    pub fn push(&self, item: T) -> Result<(), T> {
        let waker = {
            let mut state = self.lock();
            if state.closed {
                return Err(item);
            }
            state.items.push_back(item);
            state.waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
        Ok(())
    }

    /// Closes the queue: later pushes fail, and [`WakeQueue::pop`] answers
    /// `None` once the items already queued are taken.
    pub fn close(&self) {
        let waker = {
            let mut state = self.lock();
            state.closed = true;
            state.waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }

    /// Whether [`WakeQueue::close`] was called.
    #[must_use]
    pub fn is_closed(&self) -> bool {
        self.lock().closed
    }

    /// The next item, waiting for one; `None` when the queue is closed and
    /// empty. Meant for one waiting task at a time.
    pub async fn pop(&self) -> Option<T> {
        poll_fn(|cx| {
            let mut state = self.lock();
            if let Some(item) = state.items.pop_front() {
                Poll::Ready(Some(item))
            } else if state.closed {
                Poll::Ready(None)
            } else {
                state.waker = Some(cx.waker().clone());
                Poll::Pending
            }
        })
        .await
    }

    /// The next item if one is queued.
    #[must_use]
    pub fn try_pop(&self) -> Option<T> {
        self.lock().items.pop_front()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::future::Future;
    use std::pin::pin;
    use std::sync::Arc;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::task::{Context, Wake};

    struct CountingWaker(AtomicUsize);

    impl Wake for CountingWaker {
        fn wake(self: Arc<Self>) {
            self.0.fetch_add(1, Ordering::Relaxed);
        }
    }

    #[test]
    fn pop_waits_for_a_push_and_ends_after_close() {
        let queue = WakeQueue::new();
        let counter = Arc::new(CountingWaker(AtomicUsize::new(0)));
        let waker = Waker::from(Arc::clone(&counter));
        let mut cx = Context::from_waker(&waker);

        let mut first = pin!(queue.pop());
        assert!(first.as_mut().poll(&mut cx).is_pending());
        queue.push(7).unwrap();
        assert_eq!(counter.0.load(Ordering::Relaxed), 1);
        assert_eq!(first.as_mut().poll(&mut cx), Poll::Ready(Some(7)));

        queue.push(8).unwrap();
        queue.close();
        assert_eq!(queue.push(9), Err(9));
        assert!(queue.is_closed());
        let mut second = pin!(queue.pop());
        assert_eq!(second.as_mut().poll(&mut cx), Poll::Ready(Some(8)));
        let mut third = pin!(queue.pop());
        assert_eq!(third.as_mut().poll(&mut cx), Poll::Ready(None));
        assert_eq!(queue.try_pop(), None);
    }

    #[test]
    fn close_wakes_a_waiting_task() {
        let queue = WakeQueue::<u8>::new();
        let counter = Arc::new(CountingWaker(AtomicUsize::new(0)));
        let waker = Waker::from(Arc::clone(&counter));
        let mut cx = Context::from_waker(&waker);
        let mut pop = pin!(queue.pop());
        assert!(pop.as_mut().poll(&mut cx).is_pending());
        queue.close();
        assert_eq!(counter.0.load(Ordering::Relaxed), 1);
        assert_eq!(pop.as_mut().poll(&mut cx), Poll::Ready(None));
    }
}
