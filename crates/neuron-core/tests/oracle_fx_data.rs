//! Oracle vectors and the data dump for work package P1.7, effect data
//! (`neuron_core::fx_data`, from `CppSource/particlefx`).
//!
//! The synthetic `.ale` files are in `tests/fixtures/oracle/fx/`, written by
//! `make_fx_data_fixtures.py`. The harness side is
//! `CppSource/Oracle/oracle_fx_data.cpp`: it loads `particlefx.dll`, loads
//! each file into a fresh `FxNodeLibrary` and `FxEffectLibrary` as the
//! client does (node library first, through one UTF file system) and dumps
//! what the libraries then hold through their own interfaces.
//!
//! Per file: a line with both load results; per node its key hash (and a
//! billboard's texture CRC), then one line per row of the class's property
//! table and per probe hash (owned hashes no row lists), each with the
//! getter's result and value; per effect its name, hash and header words,
//! then its node references and pairs. Floats are bit patterns; ramp
//! values longer than 4096 characters are written as their FNV-1a hash.

mod support;

use std::fmt::Write as _;
use std::path::Path;

use neuron_core::fx_data::props::kind;
use neuron_core::fx_data::{
    AnimatedSingle, AnimatedSingleRamp, AnimatedTransform, ClassData, ColorRampRamp, EffectLibrary,
    FxError, NestedRamp, NodeLibrary, PropertySet, RampColor, RampSingle, name_hash,
};
use neuron_core::vfs::{FileSystem, FsDesc, Host, create_file_system};
use support::{check_suite, fixtures, hex, input_lines};

/// Hashes some class owns that its row table does not list, probed for
/// every node.
const PROBES: [(u32, u32); 8] = [
    (0x0C53_B008, kind::SINGLE_RAMP_RAMP),
    (0x0BA0_B3BB, kind::TRANSFORM),
    (0xF000_4D58, kind::TRANSFORM),
    (0xE462_B789, kind::ANIMATED_SINGLE_RAMP),
    (0x061A_AF28, kind::ANIMATED_SINGLE_RAMP),
    (0x16FF_3474, kind::ANIMATED_SINGLE_RAMP),
    (0x0350_3B61, kind::BOOL),
    (0x0ABE_0402, kind::BOOL),
];

/// A value longer than this is written as `fnv64:` and its hash.
const VALUE_LIMIT: usize = 4096;

fn bits(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn name_text(name: Option<&[u8]>) -> String {
    name.map_or_else(|| "-".to_owned(), hex)
}

fn single_text(ramp: &RampSingle) -> String {
    let keys: Vec<String> = ramp
        .keys
        .iter()
        .map(|k| format!("{}={}", bits(k.time), bits(k.value)))
        .collect();
    format!("m{:x}[{}]", ramp.mode, keys.join(","))
}

fn color_text(ramp: &RampColor) -> String {
    let keys: Vec<String> = ramp
        .keys
        .iter()
        .map(|k| format!("{}={:08x}", bits(k.time), k.value))
        .collect();
    format!("m{:x}[{}]", ramp.mode, keys.join(","))
}

fn animated_text(value: &AnimatedSingle) -> String {
    let keys: Vec<String> = value
        .keys()
        .iter()
        .map(|k| {
            format!(
                "{}={}/{}/{}",
                bits(k.time),
                bits(k.value),
                bits(k.in_slope),
                bits(k.out_slope)
            )
        })
        .collect();
    format!(
        "w{}l{:x}[{}]",
        bits(value.weight),
        value.loop_type(),
        keys.join(",")
    )
}

fn nested_text<T>(ramp: &NestedRamp<T>, child: impl Fn(&T) -> String) -> String {
    let keys: Vec<String> = ramp
        .keys
        .iter()
        .map(|k| format!("{}:{}", bits(k.time), child(&k.value)))
        .collect();
    format!("m{:x}{{{}}}", ramp.mode, keys.join(";"))
}

fn animated_ramp_text(ramp: &AnimatedSingleRamp) -> String {
    nested_text(ramp, animated_text)
}

fn transform_text(transform: &AnimatedTransform) -> String {
    let [a, b, c] = transform.modes();
    let channels: Vec<String> = transform.channels.iter().map(animated_ramp_text).collect();
    format!(
        "p{a:x},{b:x},{c:x}f{:x}({})",
        transform.channel_flags,
        channels.join("|")
    )
}

/// The getter of `kind` for `hash`: its result code and value text.
fn property(set: &PropertySet, hash: u32, kind: u32) -> (i32, String) {
    let value = match kind {
        kind::BOOL => set.get_bool(hash).map(|v| v.to_string()),
        kind::INT => set.get_int(hash).map(|v| format!("{v:08x}")),
        kind::FLOAT => set.get_float(hash).map(bits),
        kind::NAME => set.get_name(hash).map(name_text),
        kind::PAIR => set.get_pair(hash).map(|[a, b]| format!("{a:08x},{b:08x}")),
        kind::TRANSFORM => set
            .transform(hash)
            .map(transform_text)
            .ok_or(())
            .map_err(|()| neuron_core::fx_data::PropertyError::Miss),
        kind::SINGLE_RAMP_RAMP => set
            .single_ramp_ramp(hash)
            .map(|r| nested_text(r, single_text))
            .ok_or(neuron_core::fx_data::PropertyError::Miss),
        kind::COLOR_RAMP_RAMP => set
            .color_ramp_ramp(hash)
            .map(|r: &ColorRampRamp| nested_text(r, color_text))
            .ok_or(neuron_core::fx_data::PropertyError::Miss),
        kind::ANIMATED_SINGLE_RAMP => set
            .animated_single_ramp(hash)
            .map(animated_ramp_text)
            .ok_or(neuron_core::fx_data::PropertyError::Miss),
        _ => Err(neuron_core::fx_data::PropertyError::Miss),
    };
    match value {
        Ok(text) if text.len() > VALUE_LIMIT => (
            0,
            format!("fnv64:{}", support::data::fnv64(text.as_bytes())),
        ),
        Ok(text) => (0, text),
        Err(e) => (e.code(), String::new()),
    }
}

fn result_code(result: &Result<(), FxError>) -> i32 {
    result.as_ref().map_or_else(FxError::code, |()| 0)
}

/// The dump of one file system, loaded as the client loads an `.ale`.
fn dump_ale(fs: &mut dyn FileSystem, label: &str) -> Vec<String> {
    let mut nodes = NodeLibrary::new();
    let nodes_result = nodes.load(fs);
    let mut effects = EffectLibrary::new();
    let effects_result = effects.load(fs);
    let mut lines = vec![format!(
        "{{\"file\":\"{label}\",\"nodes\":\"{}\",\"effects\":\"{}\"}}",
        result_code(&nodes_result),
        result_code(&effects_result)
    )];
    for (index, node) in nodes.nodes().iter().enumerate() {
        let mut line = format!("{{\"node\":\"{index}\",\"hash\":\"{:08x}\"", node.hash);
        if let ClassData::Billboard(b) = &node.props.data {
            let _ = write!(line, ",\"texcrc\":\"{:08x}\"", b.texture_crc);
        }
        line.push('}');
        lines.push(line);
        for row in node.props.class.rows() {
            let (code, value) = property(&node.props, row.hash, row.kind);
            lines.push(format!(
                "{{\"p\":\"{}\",\"h\":\"{:08x}\",\"k\":\"{:x}\",\"r\":\"{code}\",\"v\":\"{value}\"}}",
                row.name, row.hash, row.kind
            ));
        }
        for (hash, kind) in PROBES {
            let (code, value) = property(&node.props, hash, kind);
            lines.push(format!(
                "{{\"q\":\"{hash:08x}\",\"k\":\"{kind:x}\",\"r\":\"{code}\",\"v\":\"{value}\"}}"
            ));
        }
    }
    for (index, effect) in effects.effects().iter().enumerate() {
        let [a, b, c, d] = effect.unknown;
        lines.push(format!(
            "{{\"effect\":\"{index}\",\"name\":\"{}\",\"hash\":\"{:08x}\",\"unknown\":\"{a:08x},{b:08x},{c:08x},{d:08x}\"}}",
            hex(&effect.name),
            name_hash(&effect.name)
        ));
        for (j, node) in effect.nodes.iter().enumerate() {
            lines.push(format!(
                "{{\"ref\":\"{j}\",\"kind\":\"{:08x}\",\"node\":\"{:08x}\",\"parent\":\"{:08x}\",\"id\":\"{:08x}\"}}",
                node.kind, node.node_hash, node.parent_id, node.id
            ));
        }
        for (j, pair) in effect.pairs.iter().enumerate() {
            lines.push(format!(
                "{{\"pair\":\"{j}\",\"a\":\"{:08x}\",\"b\":\"{:08x}\"}}",
                pair.first, pair.second
            ));
        }
    }
    lines
}

/// Opens `relative` under `root` as the harness does, through the
/// component manager with the implementation `"UTF"` (the UTF factory, or
/// the DOS factory when that refuses), and dumps it.
fn dump_file(root: &Path, relative: &str) -> (Vec<String>, bool) {
    let host = Host::new(root);
    let desc = FsDesc::read(relative.as_bytes()).with_implementation(b"UTF");
    match create_file_system(&host, &desc) {
        Ok(mut fs) => {
            let lines = dump_ale(fs.as_mut(), relative);
            let ok = lines[0].ends_with("\"nodes\":\"0\",\"effects\":\"0\"}");
            (lines, ok)
        }
        Err(_) => (
            vec![format!("{{\"file\":\"{relative}\",\"open\":\"fail\"}}")],
            false,
        ),
    }
}

#[test]
fn fx_data_suite() {
    let mut lines = Vec::new();
    for file in input_lines("fx/files.list") {
        lines.extend(dump_file(&fixtures(), &format!("fx/{file}")).0);
    }
    check_suite("fx_data.jsonl", &lines);
}

/// The files the dump covers: UTF archives named `*.ale`.
fn is_ale(root: &Path, relative: &str) -> bool {
    relative.to_ascii_lowercase().ends_with(".ale")
        && support::data::magic(root, relative) == b"UTF "
}

#[test]
fn data_dump_fx() {
    let Some(dirs) = support::data::data_dirs("fx") else {
        return;
    };
    let files: Vec<String> = support::data::list_files(&dirs.root)
        .into_iter()
        .filter(|file| is_ale(&dirs.root, file))
        .collect();
    let mut report = support::data::Report::new("fx", &dirs);
    report.file_list(&files);
    let mut loaded = 0;
    let mut nodes = 0;
    let mut effects = 0;
    let mut ignored = 0;
    for file in &files {
        let (lines, ok) = dump_file(&dirs.root, file);
        if ok {
            loaded += 1;
        }
        if let Ok(bytes) = std::fs::read(dirs.root.join(file))
            && let Ok(ale) = neuron_core::fx_data::AleFile::parse(bytes)
        {
            nodes += ale.nodes.nodes().len();
            effects += ale.effects.effects().len();
            ignored += ale
                .nodes
                .nodes()
                .iter()
                .map(|n| n.ignored.len())
                .sum::<usize>();
        }
        report.check(file, &lines);
    }
    report.note(format!(
        "{loaded} of {} files loaded both libraries without error; {nodes} nodes, {effects} effects, {ignored} property records not taken",
        files.len()
    ));
    report.finish();
    assert_eq!(loaded, files.len(), "every .ale file loads");
}
