//! Oracle suites and data dumps of work package P1.7: texture levels,
//! texture libraries and materials (`neuron_core::{image, material_data}`
//! against `CppSource/Shading/`). The harness file
//! `CppSource/Oracle/oracle_shading.cpp` writes the same lines; the inputs
//! come from `tests/fixtures/oracle/make_shading_fixtures.py`.
//!
//! The texture library's renderer is replaced on both sides by the
//! recording device described in `neuron_core::image::library`.

mod support;

use neuron_core::image::dds::{self, ImageInfo};
use neuron_core::image::format::PixelFormat;
use neuron_core::image::format_map::FormatMaps;
use neuron_core::image::library::{
    GroupEntry, LevelSource, LoadOutcome, LoaderSettings, TextureEvent, TextureLoader,
};
use neuron_core::image::tga::{self, TgaHeader};
use neuron_core::material_data::schema::{SchemaValue, value_type};
use neuron_core::material_data::{
    KINDS, Material, MaterialEntry, PropertyValue, load_material_animations, load_material_library,
};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};
use support::data::fnv64;
use support::{check_suite, fixtures, hex, input_lines, unhex};

fn info_fields(info: Option<&ImageInfo>) -> String {
    match info {
        None => "\"dds\":\"0\"".to_owned(),
        Some(i) => format!(
            "\"dds\":\"1\",\"w\":\"{}\",\"h\":\"{}\",\"d\":\"{}\",\"mips\":\"{}\",\"fmt\":\"{:08x}\",\"rtype\":\"{}\"",
            i.width, i.height, i.depth, i.mip_levels, i.format, i.resource_type
        ),
    }
}

/// The library settings of a suite line: `lod_load square pow2 lod_min
/// square_only`.
fn settings_from(fields: &[&str]) -> LoaderSettings {
    let n = |i: usize| fields.get(i).and_then(|t| t.parse().ok()).unwrap_or(0);
    LoaderSettings {
        lod_load: n(0),
        square_policy: n(1),
        pow2_policy: n(2),
        lod_min: n(3),
        square_textures_only: n(4),
        ..LoaderSettings::default()
    }
}

fn settings_text(s: &LoaderSettings) -> String {
    format!(
        "{},{},{},{},{}",
        s.lod_load, s.square_policy, s.pow2_policy, s.lod_min, s.square_textures_only
    )
}

fn event_line(event: &TextureEvent) -> String {
    match event {
        TextureEvent::ImageInfo { size, info } => format!(
            "{{\"ev\":\"info\",\"size\":\"{size}\",{}}}",
            info_fields(info.as_ref())
        ),
        TextureEvent::Create {
            width,
            height,
            format,
            levels,
            handle,
            result,
        } => format!(
            "{{\"ev\":\"create\",\"w\":\"{width}\",\"h\":\"{height}\",\"fmt\":\"{}\",\"levels\":\"{levels}\",\"handle\":\"{handle}\",\"result\":\"{result}\"}}",
            format.0
        ),
        TextureEvent::Level(level) => {
            let (src, need, data) = match &level.source {
                LevelSource::Mapped { offset, data, need } => {
                    (format!("mips+{offset:x}"), *need, data.as_slice())
                }
                LevelSource::Decoded(data) => {
                    ("decoded".to_owned(), data.len() as u64, data.as_slice())
                }
            };
            let palette = level.palette.as_deref().map_or("-".to_owned(), fnv64);
            format!(
                "{{\"ev\":\"level\",\"handle\":\"{}\",\"level\":\"{}\",\"w\":\"{}\",\"h\":\"{}\",\"pitch\":\"{}\",\"fmt\":\"{}\",\"src\":\"{src}\",\"need\":\"{need}\",\"got\":\"{}\",\"fnv\":\"{}\",\"pal\":\"{palette}\",\"result\":\"{}\"}}",
                level.handle,
                level.level,
                level.width.cast_signed(),
                level.height.cast_signed(),
                level.pitch,
                level.format.0,
                data.len(),
                fnv64(data),
                level.result
            )
        }
        TextureEvent::Cubemap {
            data,
            info,
            handle,
            result,
        } => format!(
            "{{\"ev\":\"cube\",\"size\":\"{}\",\"fnv\":\"{}\",{},\"handle\":\"{handle}\",\"result\":\"{result}\"}}",
            data.len(),
            fnv64(data),
            info_fields(info.as_ref())
        ),
        TextureEvent::Destroy { handle } => {
            format!("{{\"ev\":\"destroy\",\"handle\":\"{handle}\"}}")
        }
    }
}

/// `LoadTextureGroup` on an opened archive: whether `Texture library`
/// opened, then per child its renderer calls, its result and its record.
/// Returns the loader and the children loaded.
fn texture_lines(
    fs: &mut dyn FileSystem,
    settings: LoaderSettings,
    lines: &mut Vec<String>,
) -> (TextureLoader, Vec<GroupEntry>) {
    let mut loader = TextureLoader::new(settings);
    let Some(entries) = loader.load_group(fs) else {
        lines.push("{\"texlib\":\"0\"}".to_owned());
        return (loader, Vec::new());
    };
    lines.push("{\"texlib\":\"1\"}".to_owned());
    for entry in &entries {
        for event in &loader.events[entry.events.clone()] {
            lines.push(event_line(event));
        }
        let (result, record) = match entry.outcome {
            LoadOutcome::Fault => ("fault".to_owned(), None),
            LoadOutcome::Done { result, record } => {
                (result.to_string(), record.filter(|_| result >= 0))
            }
        };
        lines.push(format!(
            "{{\"entry\":\"{}\",\"result\":\"{result}\"}}",
            hex(&entry.name)
        ));
        let Some(record) = record else {
            continue;
        };
        let r = &loader.records[record];
        let children: Vec<String> = r
            .children
            .iter()
            .map(|c| c.map_or("-".to_owned(), |c| hex(&loader.records[c].name)))
            .collect();
        lines.push(format!(
            "{{\"record\":\"{}\",\"handle\":\"{}\",\"refs\":\"{}\",\"rate\":\"{:08x}\",\"frames\":\"{}\",\"children\":\"{}\"}}",
            hex(&r.name),
            r.handle,
            r.ref_count,
            r.tile_rate,
            r.frames.len(),
            children.join(",")
        ));
        for (index, frame) in r.frames.iter().enumerate() {
            lines.push(format!(
                "{{\"frame\":\"{index}\",\"child\":\"{}\",\"uv\":\"{:08x},{:08x},{:08x},{:08x}\"}}",
                frame.child, frame.uv[0], frame.uv[1], frame.uv[2], frame.uv[3]
            ));
        }
    }
    (loader, entries)
}

fn value_text(kind: u32, words: &[u32; 4], text: Option<&[u8]>) -> String {
    if matches!(kind, value_type::STRING | value_type::TEXTURE_NAME) {
        return format!("{kind}:s:{}", text.map_or("-".to_owned(), hex));
    }
    let count = match kind {
        value_type::VEC3 => 3,
        value_type::VEC4 => 4,
        _ => 1,
    };
    let words: Vec<String> = words[..count].iter().map(|w| format!("{w:08x}")).collect();
    format!("{kind}:{}", words.join(","))
}

fn schema_value(value: &SchemaValue) -> String {
    value_text(value.kind, &value.words, value.text)
}

fn parameter_line(index: usize, value: &PropertyValue) -> String {
    format!(
        "{{\"p\":\"{index}\",\"v\":\"{}\"}}",
        value_text(value.kind, &value.words, value.text.as_deref())
    )
}

/// The `Material library` and `MaterialAnim` nodes of an opened archive.
fn material_lines(fs: &mut dyn FileSystem, lines: &mut Vec<String>) -> (usize, usize) {
    let mut materials = 0;
    match load_material_library(fs) {
        None => lines.push("{\"matlib\":\"0\"}".to_owned()),
        Some(nodes) => {
            lines.push("{\"matlib\":\"1\"}".to_owned());
            for node in nodes {
                let name = hex(&node.name);
                match node.entry {
                    MaterialEntry::Duplicate => {
                        lines.push(format!("{{\"mat\":\"{name}\",\"entry\":\"dup\"}}"));
                    }
                    MaterialEntry::NotOpened => {
                        lines.push(format!("{{\"mat\":\"{name}\",\"entry\":\"noopen\"}}"));
                    }
                    MaterialEntry::NoType => {
                        lines.push(format!("{{\"mat\":\"{name}\",\"entry\":\"notype\"}}"));
                    }
                    MaterialEntry::Typed {
                        type_name,
                        material,
                    } => {
                        let kind = material.as_ref().map_or("-", |m| m.kind.name);
                        lines.push(format!(
                            "{{\"mat\":\"{name}\",\"type\":\"{}\",\"kind\":\"{kind}\"}}",
                            hex(&type_name)
                        ));
                        if let Some(material) = material {
                            materials += 1;
                            for (index, value) in material.parameters().iter().enumerate() {
                                lines.push(parameter_line(index, value));
                            }
                            lines.push(format!(
                                "{{\"blend\":\"{}\"}}",
                                u8::from(material.fields.alpha_blend)
                            ));
                        }
                    }
                }
            }
        }
    }
    let mut anims = 0;
    match load_material_animations(fs) {
        None => lines.push("{\"anims\":\"0\"}".to_owned()),
        Some(list) => {
            lines.push("{\"anims\":\"1\"}".to_owned());
            for anim in list {
                anims += 1;
                let array = |data: &Option<Vec<u8>>, asked: u32| {
                    data.as_ref().map_or("-".to_owned(), |d| {
                        format!("{asked},{},{}", d.len(), fnv64(d))
                    })
                };
                lines.push(format!(
                    "{{\"anim\":\"{}\",\"crc\":\"{:08x}\",\"count\":\"{}\",\"flags\":\"{:08x}\",\"deltas\":\"{}\",\"keys\":\"{}\"}}",
                    hex(&anim.name),
                    anim.crc,
                    anim.count,
                    anim.flags,
                    array(&anim.deltas, anim.deltas_asked),
                    array(&anim.keys, anim.keys_asked)
                ));
            }
        }
    }
    (materials, anims)
}

fn open_archive(host: &Host, path: &str) -> Option<UtfFileSystem> {
    UtfFileSystem::open(
        host,
        &FsDesc::read(path.as_bytes()).with_implementation(b"UTF"),
    )
    .ok()
}

#[test]
fn shading_tga() {
    let mut lines = Vec::new();
    for (index, line) in input_lines("shading/tga.list").iter().enumerate() {
        let bytes = unhex(line);
        let header = TgaHeader::parse(&bytes).expect("fixture header");
        let format = header.source_format();
        let mut out = format!(
            "{{\"i\":\"{index}\",\"supported\":\"{}\",\"format\":\"{}:{}\"",
            u8::from(header.is_supported()),
            u8::from(format.is_some()),
            format.unwrap_or(PixelFormat::UNKNOWN).0
        );
        if header.is_supported() {
            let image = tga::decode(&bytes).expect("fixture level");
            out.push_str(&format!(
                ",\"decoded\":\"{}\",\"w\":\"{}\",\"h\":\"{}\",\"pitch\":\"{}\",\"pixels\":\"{}\",\"palette\":\"{}\"",
                image.format.0,
                image.width,
                image.height,
                image.pitch,
                hex(&image.pixels),
                image.palette.as_deref().map_or("-".to_owned(), hex)
            ));
        }
        out.push('}');
        lines.push(out);
    }
    check_suite("shading_tga.jsonl", &lines);
}

#[test]
fn shading_dds() {
    let mut lines = Vec::new();
    for (index, line) in input_lines("shading/dds.list").iter().enumerate() {
        let bytes = unhex(line);
        lines.push(format!(
            "{{\"i\":\"{index}\",\"size\":\"{}\",{}}}",
            bytes.len(),
            info_fields(dds::image_info(&bytes).as_ref())
        ));
    }
    check_suite("shading_dds.jsonl", &lines);
}

fn format_map_line(step: &str, maps: &FormatMaps) -> String {
    let rows: Vec<String> = maps
        .rows
        .iter()
        .map(|row| {
            let values: Vec<String> = row.value.iter().map(u32::to_string).collect();
            let masks: String = row
                .mask
                .iter()
                .map(|&m| if m != 0 { '1' } else { '0' })
                .collect();
            format!("{}/{masks}/{:08x}", values.join("."), row.fourcc)
        })
        .collect();
    let picks: Vec<String> = (0..22)
        .map(|id| {
            maps.pick(PixelFormat(id))
                .map_or("-".to_owned(), |f| f.0.to_string())
        })
        .collect();
    format!(
        "{{\"step\":\"{step}\",\"rows\":\"{}\",\"pick\":\"{}\"}}",
        rows.join(";"),
        picks.join(",")
    )
}

#[test]
fn shading_format_maps() {
    let mut maps = FormatMaps::defaults();
    let mut lines = Vec::new();
    for step in input_lines("shading/format_maps.list") {
        match step.as_str() {
            "defaults" => maps = FormatMaps::defaults(),
            "empty" => maps = FormatMaps::empty(),
            _ => {
                let text = unhex(step.strip_prefix("line ").expect("a line step"));
                let mut buffer = vec![0u8; 1024];
                buffer[..text.len()].copy_from_slice(&text);
                maps.parse_line(&mut buffer);
            }
        }
        lines.push(format_map_line(&step, &maps));
    }
    check_suite("shading_format_maps.jsonl", &lines);
}

#[test]
fn shading_schema() {
    let mut lines = Vec::new();
    for kind in KINDS {
        let schema = kind.schema();
        lines.push(format!(
            "{{\"kind\":\"{}\",\"class\":\"{}\",\"count\":\"{}\"}}",
            kind.name,
            kind.class_name,
            schema.len()
        ));
        for (index, p) in schema.iter().enumerate() {
            lines.push(format!(
                "{{\"i\":\"{index}\",\"flags\":\"{:08x}\",\"name\":\"{}\",\"desc\":\"{}\",\"cur\":\"{}\",\"min\":\"{}\",\"max\":\"{}\"}}",
                p.flags,
                hex(p.name),
                hex(p.description),
                schema_value(&p.current),
                schema_value(&p.minimum),
                schema_value(&p.maximum)
            ));
        }
        let material = Material::new(kind);
        for (index, value) in material.parameters().iter().enumerate() {
            lines.push(parameter_line(index, value));
        }
    }
    check_suite("shading_schema.jsonl", &lines);
}

#[test]
fn shading_texlib() {
    let host = Host::new(fixtures());
    let mut lines = Vec::new();
    for line in input_lines("shading/texlib.list") {
        let fields: Vec<&str> = line.split(' ').collect();
        let settings = settings_from(&fields[1..]);
        let head = format!(
            "{{\"archive\":\"{}\",\"settings\":\"{}\"",
            fields[0],
            settings_text(&settings)
        );
        match open_archive(&host, fields[0]) {
            None => lines.push(format!("{head},\"open\":\"0\"}}")),
            Some(mut fs) => {
                lines.push(format!("{head},\"open\":\"1\"}}"));
                texture_lines(&mut fs, settings, &mut lines);
            }
        }
    }
    check_suite("shading_texlib.jsonl", &lines);
}

#[test]
fn shading_material() {
    let host = Host::new(fixtures());
    let mut lines = Vec::new();
    for archive in input_lines("shading/material.list") {
        match open_archive(&host, &archive) {
            None => lines.push(format!("{{\"archive\":\"{archive}\",\"open\":\"0\"}}")),
            Some(mut fs) => {
                lines.push(format!("{{\"archive\":\"{archive}\",\"open\":\"1\"}}"));
                material_lines(&mut fs, &mut lines);
            }
        }
    }
    check_suite("shading_material.jsonl", &lines);
}

/// Whether an opened archive has the named node at its root.
fn has_node(fs: &mut UtfFileSystem, name: &[u8]) -> bool {
    let opened = fs.open_node(name).is_ok();
    if opened {
        let _ = fs.open_node(b"..");
    }
    opened
}

/// Every UTF archive in the original data with a `Texture library` node,
/// loaded with the default settings (Gate G1). Each level the renderer
/// would receive is also converted to RGBA8, which the oracle cannot
/// check; failures are counted in the summary.
#[test]
fn data_dump_texture() {
    let Some(dirs) = support::data::data_dirs("texture") else {
        return;
    };
    let host = Host::new(&dirs.root);
    let mut files = Vec::new();
    for file in support::data::list_files(&dirs.root) {
        if support::data::magic(&dirs.root, &file) != b"UTF " {
            continue;
        }
        if let Some(mut fs) = open_archive(&host, &file)
            && has_node(&mut fs, b"Texture library")
        {
            files.push(file);
        }
    }
    let mut report = support::data::Report::new("texture", &dirs);
    report.file_list(&files);
    let settings = LoaderSettings::default();
    let (mut entries, mut loaded, mut faults, mut levels, mut converted) = (0, 0, 0, 0, 0);
    let mut unconverted = std::collections::BTreeMap::new();
    for file in &files {
        let mut lines = vec![format!(
            "{{\"file\":\"{file}\",\"settings\":\"{}\"}}",
            settings_text(&settings)
        )];
        let Some(mut fs) = open_archive(&host, file) else {
            continue;
        };
        let (loader, group) = texture_lines(&mut fs, settings.clone(), &mut lines);
        entries += group.len();
        for entry in group {
            match entry.outcome {
                LoadOutcome::Fault => faults += 1,
                LoadOutcome::Done { result, .. } if result >= 0 => loaded += 1,
                LoadOutcome::Done { .. } => {}
            }
        }
        for event in &loader.events {
            if let TextureEvent::Level(level) = event {
                levels += 1;
                match level.to_rgba8() {
                    Ok(_) => converted += 1,
                    Err(e) => *unconverted.entry(format!("{e}")).or_insert(0) += 1,
                }
            }
        }
        report.check(file, &lines);
    }
    report.note(format!(
        "{entries} texture nodes, {loaded} loaded, {faults} faults; {levels} level uploads, {converted} converted to RGBA8"
    ));
    for (error, count) in unconverted {
        report.note(format!("  not converted ({count}): {error}"));
    }
    report.finish();
}

/// Every UTF archive in the original data with a `Material library` or a
/// `MaterialAnim` node (Gate G1).
#[test]
fn data_dump_material() {
    let Some(dirs) = support::data::data_dirs("material") else {
        return;
    };
    let host = Host::new(&dirs.root);
    let mut files = Vec::new();
    for file in support::data::list_files(&dirs.root) {
        if support::data::magic(&dirs.root, &file) != b"UTF " {
            continue;
        }
        if let Some(mut fs) = open_archive(&host, &file)
            && (has_node(&mut fs, b"Material library") || has_node(&mut fs, b"MaterialAnim"))
        {
            files.push(file);
        }
    }
    let mut report = support::data::Report::new("material", &dirs);
    report.file_list(&files);
    let (mut materials, mut anims) = (0, 0);
    for file in &files {
        let mut lines = vec![format!("{{\"file\":\"{file}\"}}")];
        let Some(mut fs) = open_archive(&host, file) else {
            continue;
        };
        let (m, a) = material_lines(&mut fs, &mut lines);
        materials += m;
        anims += a;
        report.check(file, &lines);
    }
    report.note(format!(
        "{materials} materials of a Shading kind, {anims} animations"
    ));
    report.finish();
}
