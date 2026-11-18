//! The committed oracle vectors of this crate (`tests/vectors/`), rendered
//! through the port. `Oracle.exe neuron-client` writes the same files from
//! the C++ reference (`CppSource/Oracle/oracle_scene_script.cpp`,
//! `oracle_scene_script_build.cpp`, `oracle_text.cpp` and `oracle_audio.cpp`).

mod support;

use neuron_client::scene_script::{BuildState, Vm};
use neuron_client::text::rdl::markup;

use support::audio::{self, check_suite_within};
use support::{check_suite, input_lines, scene_script, scene_script_build, text, unhex};

/// P4.14: the Lua state after each buffer of a case, one fresh state per
/// case (`tests/fixtures/oracle/scene_script.list`).
#[test]
fn scene_script() {
    let mut lines = Vec::new();
    for (case, line) in input_lines("scene_script.list").iter().enumerate() {
        let mut vm = Vm::new();
        for (buffer, text) in line.split(' ').enumerate() {
            let status = scene_script::status(&mut vm, &unhex(text));
            let head =
                format!("{{\"case\":\"{case}\",\"buffer\":\"{buffer}\",\"status\":\"{status}\"}}");
            lines.extend(scene_script::render(&vm, head));
        }
    }
    check_suite("scene_script.jsonl", &lines);
}

/// P4.13, P4.14: `MsvcrtFormatLuaNumber` and `MsvcrtNumericVsnprintf`
/// (`tests/fixtures/oracle/number_text.list`).
#[test]
fn number_text() {
    let lines: Vec<String> = input_lines("number_text.list")
        .iter()
        .map(|line| text::number_line(line))
        .collect();
    check_suite("number_text.jsonl", &lines);
}

/// P4.12: `XMLReader::read_buffer` into an empty list
/// (`tests/fixtures/oracle/rdl_markup.list`).
#[test]
fn rdl_markup() {
    let lines: Vec<String> = input_lines("rdl_markup.list")
        .iter()
        .enumerate()
        .map(|(case, line)| text::markup_line(case, line, markup::read))
        .collect();
    check_suite("rdl_markup.jsonl", &lines);
}

/// P4.10: `MixerApplyInstance` on the software path, one instance per input
/// (`tests/fixtures/oracle/audio/apply.list`).
#[test]
fn audio_apply() {
    check_suite_within(
        "audio_apply.jsonl",
        &audio::apply_lines(),
        audio::apply_bound,
    );
}

/// P4.10: `PitchToMultiplier` and `ScaledDurationMs`.
#[test]
fn audio_pitch() {
    check_suite_within(
        "audio_pitch.jsonl",
        &audio::pitch_lines(),
        audio::pitch_bound,
    );
}

/// P4.10: `MixerSetStartOffset`.
#[test]
fn audio_start() {
    check_suite_within(
        "audio_start.jsonl",
        &audio::start_lines(),
        audio::start_bound,
    );
}

/// P4.10: the listener setters and getters.
#[test]
fn audio_listener() {
    check_suite("audio_listener.jsonl", &audio::listener_lines());
}

/// P4.10: `MixerAllowCreate` and `OneShotAlreadyExpired`.
#[test]
fn audio_range() {
    check_suite_within(
        "audio_range.jsonl",
        &audio::range_lines(),
        audio::range_bound,
    );
}

/// P4.10: `ParseIniUnsigned` and `ParseIniFloat`.
#[test]
fn audio_ini() {
    check_suite("audio_ini.jsonl", &audio::ini_lines());
}

/// P4.10: the clamps of `SetEaxPreview` and `FSoundArchetype::SetGain`.
#[test]
fn audio_eax() {
    check_suite("audio_eax.jsonl", &audio::eax_lines());
}

/// P4.10: scripted `MixerTick` sessions: the ranked voice cap, expiry,
/// range culls, start positions and the per-voice settings.
#[test]
fn audio_tick() {
    check_suite_within("audio_tick.jsonl", &audio::tick_lines(), audio::tick_bound);
}

/// P4.14: the engine calls of the parser's second pass for each buffer of
/// a case, one fresh module state per case, with or without a COM context
/// (`tests/fixtures/oracle/scene_script_build.list`).
#[test]
fn scene_script_build() {
    let mut lines = Vec::new();
    for (case, line) in input_lines("scene_script_build.list").iter().enumerate() {
        let mut parts = line.split(' ');
        let com_context = parts.next() == Some("c");
        let mut vm = Vm::new();
        let mut state = BuildState::new();
        for (buffer, text) in parts.enumerate() {
            let head = format!("{{\"case\":\"{case}\",\"buffer\":\"{buffer}\",");
            lines.extend(scene_script_build::run(
                &mut vm,
                &mut state,
                &unhex(text),
                com_context,
                &head,
            ));
        }
    }
    check_suite("scene_script_build.jsonl", &lines);
}
