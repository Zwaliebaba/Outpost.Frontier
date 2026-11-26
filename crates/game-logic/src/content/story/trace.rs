//! The Tier B story trace (P3.5d, record 0008 point 10 and question 9;
//! `design/sim-records.md`, `story.jsonl`): what the oracle's
//! `Content/oracle_story_session.cpp` writes from the hooks of
//! `Content/oracle_story_trace.h` in Story and StoryLauncher, recorded by the
//! port at the same points. Off unless a session turns it on ([`start`]);
//! nothing else reads it.
//!
//! One JSON object per line, ids unsigned decimal, a `float` as its bits in
//! lower-case hexadecimal:
//!
//! - `{"rec":"message","step":S,"msg":M}`: a message to Story's
//!   `HandleMessage`, before its case runs (`Story.cpp:438`);
//! - `{"rec":"event","step":S,"kind":K,"chapter":C,"controller":I}`:
//!   Story's `OnMissionEvent`, before it runs (`Story.cpp:270`);
//! - `{"rec":"create","step":S,"name":"X","id":I}`: a
//!   `pub::Controller::Create` of Story (a chapter's mission) or of
//!   StoryLauncher (Story), with the id it gave;
//! - `{"rec":"destroy","step":S,"id":I}`: a `pub::Controller::Destroy` of
//!   Story, before it is made;
//! - `{"rec":"state","step":S,"num":N,"worth":"%08x","bought":0|1,"mission":"X","controller":C}`:
//!   Story's state when its handler or `OnMissionEvent` returns, with
//!   `GStoryMissionController`.

use neuron_server::controller::ControllerId;

use super::Story;
use crate::content::Instance;
use crate::server::Cx;

/// The trace being recorded: the session's step and the lines so far.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StoryTrace {
    /// The session's step, written into every record.
    pub step: u32,
    /// The records, in order.
    pub lines: Vec<String>,
}

/// Turns the trace on (empty).
pub fn start(cx: &mut Cx<'_>) {
    cx.logic.content.state.story_trace = Some(StoryTrace::default());
}

/// Turns the trace off and returns its records.
pub fn stop(cx: &mut Cx<'_>) -> Vec<String> {
    cx.logic
        .content
        .state
        .story_trace
        .take()
        .map(|t| t.lines)
        .unwrap_or_default()
}

/// Appends a line the session builds (its `step`, `send` and `end`
/// records).
pub fn push(cx: &mut Cx<'_>, line: String) {
    if let Some(t) = cx.logic.content.state.story_trace.as_mut() {
        t.lines.push(line);
    }
}

/// The session's current step.
pub fn set_step(cx: &mut Cx<'_>, step: u32) {
    if let Some(t) = cx.logic.content.state.story_trace.as_mut() {
        t.step = step;
    }
}

fn record(cx: &mut Cx<'_>, build: impl FnOnce(u32) -> String) {
    if let Some(t) = cx.logic.content.state.story_trace.as_mut() {
        let line = build(t.step);
        t.lines.push(line);
    }
}

/// `OracleStoryTraceMessage`: a message to Story.
pub(crate) fn message(cx: &mut Cx<'_>, msg: u32) {
    record(cx, |step| {
        format!("{{\"rec\":\"message\",\"step\":{step},\"msg\":{msg}}}")
    });
}

/// `OracleStoryTraceEvent`: Story's `OnMissionEvent`.
pub(crate) fn event(cx: &mut Cx<'_>, kind: u32, chapter: u32, controller: u32) {
    record(cx, |step| {
        format!(
            "{{\"rec\":\"event\",\"step\":{step},\"kind\":{kind},\"chapter\":{chapter},\"controller\":{controller}}}"
        )
    });
}

/// `OracleStoryTraceCreate`: a create of Story's or StoryLauncher's.
pub(crate) fn create(cx: &mut Cx<'_>, name: &[u8], id: ControllerId) {
    let name = String::from_utf8_lossy(name).into_owned();
    record(cx, |step| {
        format!(
            "{{\"rec\":\"create\",\"step\":{step},\"name\":\"{name}\",\"id\":{}}}",
            id.0
        )
    });
}

/// `OracleStoryTraceDestroy`: a destroy of Story's.
pub(crate) fn destroy(cx: &mut Cx<'_>, id: u32) {
    record(cx, |step| {
        format!("{{\"rec\":\"destroy\",\"step\":{step},\"id\":{id}}}")
    });
}

/// `OracleStoryTraceState`: Story `id`'s state as its handler returns.
pub(crate) fn state(cx: &mut Cx<'_>, id: ControllerId) {
    if cx.logic.content.state.story_trace.is_none() {
        return;
    }
    let Some(Instance::Story(story)) = cx.logic.content.instance(id) else {
        return;
    };
    let line = state_fields(story, cx.logic.content.state.story_mission);
    record(cx, |step| {
        format!("{{\"rec\":\"state\",\"step\":{step},{line}}}")
    });
}

fn state_fields(story: &Story, controller: ControllerId) -> String {
    format!(
        "\"num\":{},\"worth\":\"{:08x}\",\"bought\":{},\"mission\":\"{}\",\"controller\":{}",
        story.mission_num,
        story.delta_worth.to_bits(),
        u32::from(story.ship_bought),
        String::from_utf8_lossy(&story.mission_name),
        controller.0
    )
}
