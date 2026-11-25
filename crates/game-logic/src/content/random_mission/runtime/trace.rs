//! The Tier B router and machine trace of the random missions (P3.7b,
//! record 0008 point 10; `design/sim-records.md`, `rm.jsonl`): what the
//! oracle's `Content/oracle_rm_session.cpp` writes from the router and the
//! machines' slot 2, recorded by the port at the same points. Off unless a
//! session turns it on ([`start`]); nothing else reads it.
//!
//! One JSON object per line, ids unsigned decimal, a time as its `float`'s
//! bits in lower-case hexadecimal:
//!
//! - `{"rec":"register"|"unregister","step":S,"handler":H}`: a handler's
//!   registration with the router and its removal;
//! - `{"rec":"route","step":S,"type":"T","sender":A,"receiver":B,"state":D,"time":"%08x"}`:
//!   a message routed (its delivery options set; time 0 for one delivered
//!   at once);
//! - `{"rec":"deliver","step":S,"type":"T","sender":A,"receiver":B,"found":0|1}`:
//!   a delivery, at once or from the router's timer;
//! - `{"rec":"sm","step":S,"machine":H,"kind":"K","event":1|2,"state":X}`:
//!   a machine's slot 2 offered an enter (1) or exit (2) event in state
//!   `X >= 0`, so every state change shows as its exit and its enter;
//! - `{"rec":"mission","step":S,"event":"E","value":V}`: the mission's
//!   success, failure, player removal and end.

use neuron_core::util::state_machine::{EventType, StateMachineMessage};

use super::message::MessageType;
use crate::server::Cx;

/// The trace being recorded: the session's step and the lines so far.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RmTrace {
    /// The session's step, written into every record.
    pub step: u32,
    /// The records, in order.
    pub lines: Vec<String>,
}

/// Turns the trace on (empty).
pub fn start(cx: &mut Cx<'_>) {
    cx.logic.content.state.rm.trace = Some(RmTrace::default());
}

/// Turns the trace off and returns its records.
pub fn stop(cx: &mut Cx<'_>) -> Vec<String> {
    cx.logic
        .content
        .state
        .rm
        .trace
        .take()
        .map(|t| t.lines)
        .unwrap_or_default()
}

/// Appends a line the session builds (its `step`, `message` and `end`
/// records).
pub fn push(cx: &mut Cx<'_>, line: String) {
    if let Some(t) = cx.logic.content.state.rm.trace.as_mut() {
        t.lines.push(line);
    }
}

/// The session's current step.
pub fn set_step(cx: &mut Cx<'_>, step: u32) {
    if let Some(t) = cx.logic.content.state.rm.trace.as_mut() {
        t.step = step;
    }
}

fn record(cx: &mut Cx<'_>, build: impl FnOnce(u32) -> String) {
    if let Some(t) = cx.logic.content.state.rm.trace.as_mut() {
        let line = build(t.step);
        t.lines.push(line);
    }
}

/// A type's trace name; a value no type has prints as `?<value>`.
fn type_name(value: u32) -> String {
    MessageType::of(value).map_or_else(|| format!("?{value}"), |t| t.name().to_owned())
}

/// A `register` or `unregister` record.
pub fn handler(cx: &mut Cx<'_>, op: &str, id: u32) {
    record(cx, |step| {
        format!("{{\"rec\":\"{op}\",\"step\":{step},\"handler\":{id}}}")
    });
}

/// A `route` record.
pub fn route(cx: &mut Cx<'_>, header: &StateMachineMessage) {
    let name = type_name(header.message_type);
    record(cx, |step| {
        format!(
            "{{\"rec\":\"route\",\"step\":{step},\"type\":\"{name}\",\"sender\":{},\"receiver\":{},\"state\":{},\"time\":\"{:08x}\"}}",
            header.sender,
            header.receiver,
            header.delivery_state,
            header.delivery_time.to_bits()
        )
    });
}

/// A `deliver` record.
pub fn deliver(cx: &mut Cx<'_>, header: &StateMachineMessage, found: bool) {
    let name = type_name(header.message_type);
    record(cx, |step| {
        format!(
            "{{\"rec\":\"deliver\",\"step\":{step},\"type\":\"{name}\",\"sender\":{},\"receiver\":{},\"found\":{}}}",
            header.sender,
            header.receiver,
            u8::from(found)
        )
    });
}

/// An `sm` record for an enter or exit event in a state; nothing for any
/// other event or for the "any state" call.
pub fn slot2(cx: &mut Cx<'_>, machine: u32, kind: &str, event: EventType, state: i32) {
    if state < 0 || !matches!(event, EventType::Tick | EventType::Message) {
        return;
    }
    let event = event.value();
    record(cx, |step| {
        format!(
            "{{\"rec\":\"sm\",\"step\":{step},\"machine\":{machine},\"kind\":\"{kind}\",\"event\":{event},\"state\":{state}}}"
        )
    });
}

/// A `mission` record.
pub fn mission(cx: &mut Cx<'_>, event: &str, value: i64) {
    record(cx, |step| {
        format!("{{\"rec\":\"mission\",\"step\":{step},\"event\":\"{event}\",\"value\":{value}}}")
    });
}
