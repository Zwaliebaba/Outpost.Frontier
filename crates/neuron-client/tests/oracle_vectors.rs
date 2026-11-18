//! The committed oracle vectors of this crate (`tests/vectors/`), rendered
//! through the port. `Oracle.exe neuron-client` writes the same files from
//! the C++ reference (`CppSource/Oracle/oracle_scene_script.cpp`,
//! `oracle_scene_script_build.cpp`, `oracle_scene_script_runtime.cpp`,
//! `oracle_text.cpp`, `oracle_audio.cpp`, `oracle_texture.cpp`,
//! `oracle_big_image.cpp`, `oracle_material.cpp`,
//! `oracle_material_kinds.cpp` and `oracle_ui.cpp`).

mod support;

use neuron_client::scene_script::{BuildState, Vm};
use neuron_client::text::rdl::markup;

use support::audio::{self, check_suite_within};
use support::{
    check_suite, input_lines, material, material_kinds, scene_script, scene_script_build,
    scene_script_runtime, text, texture, ui, unhex,
};

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

/// P4.4: the texture library at run time: records, playbacks and their
/// animation, groups, reference counts and garbage collection
/// (`tests/fixtures/oracle/texture_runtime.list`).
#[test]
fn texture_runtime() {
    check_suite("texture_runtime.jsonl", &texture::runtime_lines());
}

/// P4.4: ximage's `BigImage`: tiles, their uploads and the draw callback
/// (`tests/fixtures/oracle/big_image.list`).
#[test]
fn big_image() {
    check_suite("big_image.jsonl", &texture::big_image_lines());
}

/// P4.14: param curves created by factory name, keys added, replaced and
/// deleted, the curve evaluated (`tests/fixtures/oracle/scene_script_curve.list`).
/// Tier A: the evaluators use no trigonometry.
#[test]
fn scene_script_curve() {
    check_suite(
        "scene_script_curve.jsonl",
        &scene_script_runtime::curve_lines(),
    );
}

/// P4.14: spline paths loaded from points and from text, evaluated, set
/// and saved (`tests/fixtures/oracle/scene_script_spline.list`). Tier B for
/// orientations, which go through `acos`, `sin` and `cos`.
#[test]
fn scene_script_spline() {
    scene_script_runtime::check_within(
        "scene_script_spline.jsonl",
        &scene_script_runtime::spline_lines(),
        4,
    );
}

/// P4.14: scripts loaded through both parser passes into a scene, recording
/// engine objects attached, the timeline driven
/// (`tests/fixtures/oracle/scene_script_playback.list`). Tier B: axis
/// rotations and orientation paths use trigonometry.
#[test]
fn scene_script_playback() {
    scene_script_runtime::check_within(
        "scene_script_playback.jsonl",
        &scene_script_runtime::playback_lines(),
        16,
    );
}

/// P4.5: `FMaterialRegex::Compile` and `Search`: the program bytes, the
/// search hints and the matches (`tests/fixtures/oracle/material_regex.list`).
#[test]
fn material_regex() {
    check_suite("material_regex.jsonl", &material::regex_lines());
}

/// P4.5: the material library: map rules and `[MaterialMap]`, type
/// resolution, creation, the master map, groups and reference counts
/// (`tests/fixtures/oracle/material_library.list`).
#[test]
fn material_library() {
    check_suite("material_library.jsonl", &material::library_lines());
}

/// P4.5: the global material records (`tests/fixtures/oracle/material_global.list`).
#[test]
fn material_global() {
    check_suite("material_global.jsonl", &material::global_lines());
}

/// P4.5: material animation: groups, instances, ticks and texture
/// matrices (`tests/fixtures/oracle/material_animation.list`).
#[test]
fn material_animation() {
    check_suite("material_animation.jsonl", &material::animation_lines());
}

/// P4.5: the material batcher: rules, bins, queued submissions, the depth
/// and triangle sorts and the dispatch (`tests/fixtures/oracle/material_batcher.list`).
#[test]
fn material_batcher() {
    check_suite("material_batcher.jsonl", &material::batcher_lines());
}

/// P4.5: the 22 material kinds and `BatchedMaterial` against recording
/// services: texture verification, `ApplyVerify` and its fallbacks,
/// binding, teardown, the submissions and the parameters
/// (`tests/fixtures/oracle/material_kinds.list`).
#[test]
fn material_kinds() {
    check_suite("material_kinds.jsonl", &material_kinds::lines());
}

/// P4.13: `FUiPageDesc::LoadFromFile` over each file of
/// `tests/fixtures/oracle/ui_page_desc.list`.
#[test]
fn ui_page_desc() {
    let lines: Vec<String> = input_lines("ui_page_desc.list")
        .iter()
        .enumerate()
        .flat_map(|(case, line)| ui::page_desc_lines(case, line))
        .collect();
    check_suite("ui_page_desc.jsonl", &lines);
}

/// P4.13: the screen-quad batcher over the operations of
/// `tests/fixtures/oracle/ui_quad_batch.list`, against a recording device.
#[test]
fn ui_quad_batch() {
    let lines: Vec<String> = input_lines("ui_quad_batch.list")
        .iter()
        .enumerate()
        .flat_map(|(case, line)| ui::quad_batch_lines(case, line))
        .collect();
    check_suite("ui_quad_batch.jsonl", &lines);
}
