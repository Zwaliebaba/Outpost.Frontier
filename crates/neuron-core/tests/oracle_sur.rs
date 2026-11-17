//! Oracle vectors for work package P1.7 (`.sur`): `.sur` files and the IVP
//! compact surfaces in them (`CppSource/Common/common_phyarch.cpp`,
//! `CppSource/IVP/ivp_collision/ivp_compact_surface.h`,
//! `CppSource/IVP/ivp_compact_builder/ivp_compact_ledge.h`).
//!
//! The inputs are in `tests/fixtures/oracle/sur/`, written by
//! `make_sur_fixtures.py`; the harness side is
//! `CppSource/Oracle/oracle_sur.cpp`, which loads each file through
//! `PhyArch::LoadSurfaces` and walks every surface with the IVP headers'
//! accessors. Both render the lines of [`render`]; the data dump `sur`
//! renders the same lines for every `.sur` file of the game data.
//!
//! Lines, one JSON object each, every value a string:
//!
//! * `{"file","result","dumps","parts"}`: the load's result (1 or 0), the
//!   diagnostics it reported and the number of parts.
//! * Per part in id order `{"part","not_fixed","extents","hardpoints","surface"}`:
//!   the corners as the hex of their 24 bytes (`-` when never written), the
//!   hardpoint ids in decimal separated by spaces, and whether the part has
//!   a surface.
//! * Per surface `{"surface","size","blob","mass_center","rotation_inertia",
//!   "radius","deviation","root","nodes","ledges","terminal","single_convex"}`:
//!   the byte size the header records, the FNV-1a hash of the blob, the
//!   header fields as raw bits, the counts of nodes and distinct ledges, the
//!   hash of the terminal ledges' offsets (`u32` each, in
//!   `get_all_ledges` order) and the single convex ledge's offset.
//! * Per node in pre-order `{"node","right","ledge","center","radius","box","free"}`,
//!   followed, where a ledge is first reached, by `{"ledge","point_offset",
//!   "client","flags","triangles","unknown_0x0e","points","point_bytes",
//!   "triangle_bytes","opposites"}`: the hashes of the points and the
//!   triangles as stored, and of each edge's opposite as an `i32` byte offset
//!   from the first triangle.

mod support;

use neuron_core::sur::{CompactSurface, SurLoad, SurPart, load};
use support::data::fnv64;
use support::{check_suite, hex, input_lines, read_fixture};

fn words(values: &[u32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}

fn vector_hex(v: neuron_core::math::Vector) -> String {
    hex(&words(&[v.x.to_bits(), v.y.to_bits(), v.z.to_bits()]))
}

fn render_part(id: u32, part: &SurPart, lines: &mut Vec<String>) {
    let extents = part.extents.map_or_else(
        || "-".to_owned(),
        |e| format!("{}{}", vector_hex(e.min), vector_hex(e.max)),
    );
    let hardpoints: Vec<String> = part.hardpoint_ids.iter().map(u32::to_string).collect();
    lines.push(format!(
        "{{\"part\":\"{id}\",\"not_fixed\":\"{}\",\"extents\":\"{extents}\",\"hardpoints\":\"{}\",\"surface\":\"{}\"}}",
        u8::from(part.not_fixed),
        hardpoints.join(" "),
        u8::from(part.surface.is_some())
    ));
}

fn render_surface(id: u32, blob: &[u8], lines: &mut Vec<String>) -> bool {
    let surface = match CompactSurface::parse(blob) {
        Ok(surface) => surface,
        Err(e) => {
            lines.push(format!("{{\"surface\":\"{id}\",\"error\":\"{e}\"}}"));
            return false;
        }
    };
    let terminal: Vec<u32> = surface
        .terminal_ledges()
        .iter()
        .map(|&index| surface.ledges[index].offset)
        .collect();
    let single = surface
        .single_convex()
        .map_or_else(|| "-".to_owned(), |i| surface.ledges[i].offset.to_string());
    lines.push(format!(
        "{{\"surface\":\"{id}\",\"size\":\"{}\",\"blob\":\"{}\",\"mass_center\":\"{}\",\"rotation_inertia\":\"{}\",\"radius\":\"{:08x}\",\"deviation\":\"{}\",\"root\":\"{}\",\"nodes\":\"{}\",\"ledges\":\"{}\",\"terminal\":\"{}\",\"single_convex\":\"{single}\"}}",
        surface.byte_size(),
        fnv64(blob),
        vector_hex(surface.mass_center),
        vector_hex(surface.rotation_inertia),
        surface.upper_limit_radius.to_bits(),
        surface.max_deviation(),
        surface.offset_ledgetree_root,
        surface.nodes.len(),
        surface.ledges.len(),
        fnv64(&words(&terminal)),
    ));
    let mut emitted = 0;
    for node in &surface.nodes {
        lines.push(format!(
            "{{\"node\":\"{}\",\"right\":\"{}\",\"ledge\":\"{}\",\"center\":\"{}\",\"radius\":\"{:08x}\",\"box\":\"{}\",\"free\":\"{}\"}}",
            node.offset,
            node.offset_right_node,
            node.offset_compact_ledge,
            vector_hex(node.center),
            node.radius.to_bits(),
            hex(&node.box_sizes),
            node.free_0
        ));
        let Some(index) = node.ledge else {
            continue;
        };
        if index != emitted {
            continue;
        }
        emitted += 1;
        let ledge = &surface.ledges[index];
        let mut points = Vec::new();
        for p in &ledge.points {
            points.extend(words(&[
                p.position.x.to_bits(),
                p.position.y.to_bits(),
                p.position.z.to_bits(),
                p.hesse_val.to_bits(),
            ]));
        }
        let mut triangles = Vec::new();
        let mut opposites = Vec::new();
        for t in &ledge.triangles {
            triangles.extend(words(&[
                t.tri_info,
                t.edges[0].data,
                t.edges[1].data,
                t.edges[2].data,
            ]));
            for e in &t.edges {
                let at = 16 * e.opposite.triangle + 4 * (usize::from(e.opposite.edge) + 1);
                opposites.extend(words(&[u32::try_from(at).unwrap()]));
            }
        }
        lines.push(format!(
            "{{\"ledge\":\"{}\",\"point_offset\":\"{}\",\"client\":\"{}\",\"flags\":\"{:08x}\",\"triangles\":\"{}\",\"unknown_0x0e\":\"{}\",\"points\":\"{}\",\"point_bytes\":\"{}\",\"triangle_bytes\":\"{}\",\"opposites\":\"{}\"}}",
            ledge.offset,
            ledge.point_offset,
            ledge.client_data,
            ledge.size_and_flags,
            ledge.triangles.len(),
            ledge.unknown_0x0e,
            ledge.points.len(),
            fnv64(&points),
            fnv64(&triangles),
            fnv64(&opposites)
        ));
    }
    true
}

/// The lines for one file; returns whether every surface decoded.
fn render(file: &str, loaded: &SurLoad, lines: &mut Vec<String>) -> bool {
    lines.push(format!(
        "{{\"file\":\"{file}\",\"result\":\"{}\",\"dumps\":\"{}\",\"parts\":\"{}\"}}",
        u8::from(loaded.result.is_ok()),
        loaded.reported(),
        loaded.parts.len()
    ));
    let mut decoded = true;
    for (&id, part) in &loaded.parts {
        render_part(id, part, lines);
    }
    for (&id, part) in &loaded.parts {
        if let Some(blob) = &part.surface {
            decoded &= render_surface(id, blob, lines);
        }
    }
    decoded
}

#[test]
fn sur_load() {
    let mut lines = Vec::new();
    for file in input_lines("sur/files.list") {
        let loaded = load(&read_fixture(&file));
        assert!(render(&file, &loaded, &mut lines), "{file}");
    }
    check_suite("sur_load.jsonl", &lines);
}

#[test]
fn data_dump_sur() {
    let Some(dirs) = support::data::data_dirs("sur") else {
        return;
    };
    let files: Vec<String> = support::data::list_files(&dirs.root)
        .into_iter()
        .filter(|file| file.to_ascii_lowercase().ends_with(".sur"))
        .collect();
    let mut report = support::data::Report::new("sur", &dirs);
    report.file_list(&files);
    let (mut loaded_ok, mut surfaces, mut failed) = (0, 0, Vec::new());
    for file in &files {
        let bytes = std::fs::read(dirs.root.join(file)).unwrap_or_default();
        let loaded = load(&bytes);
        let mut lines = Vec::new();
        if !render(file, &loaded, &mut lines) {
            failed.push(file.clone());
        }
        if loaded.result.is_ok() {
            loaded_ok += 1;
        }
        surfaces += loaded
            .parts
            .values()
            .filter(|p| p.surface.is_some())
            .count();
        report.check(file, &lines);
    }
    report.note(format!(
        "{loaded_ok} of {} files loaded, {surfaces} surfaces, {} files with a surface that does not decode",
        files.len(),
        failed.len()
    ));
    for file in failed.iter().take(10) {
        report.note(format!("surface does not decode: {file}"));
    }
    report.finish();
    assert!(failed.is_empty(), "{} files do not decode", failed.len());
}
