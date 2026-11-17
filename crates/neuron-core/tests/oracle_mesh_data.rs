//! Oracle vectors and data dumps for work package P1.7 (mesh data): the
//! `RendComp` mesh nodes and the `Deformable2` `.dfm` tables. The inputs
//! are under `tests/fixtures/oracle/mesh_data/`, written by
//! `make_mesh_data_fixtures.py`; the C++ side is
//! `CppSource/Oracle/oracle_mesh_data.cpp` (RendComp) and
//! `oracle_mesh_data_dfm.cpp` (Deformable2).
//!
//! Suites: `mesh_fvf.jsonl` (strides and channels of FVF codes),
//! `mesh_files.jsonl` and `dfm_files.jsonl` (the dumps below over the
//! synthetic archives). Data dumps: kinds `mesh` (every UTF `.vms`, `.3db`,
//! `.cmp` and `.sph`) and `dfm` (every UTF `.dfm`). All Tier A.
//!
//! A mesh dump walks every directory of an archive. A directory with a
//! `VMeshLibrary` child is loaded as a library; one with a `VMeshPart`,
//! `MultiLevel`, `Mesh` or `Sphere` child goes through the render manager
//! with the client's plugins; one with a `VMeshWire` child is loaded as a
//! wireframe. Each runs on a freshly opened archive (the root) or a child
//! file system made for the directory. The walk does not enter directories
//! with those names. Large arrays are FNV-1a hashes.

mod support;

use std::fmt::Write as _;

use support::{check_suite, fixtures, hex, input_lines};

use neuron_core::hash::eq_ignore_case;
use neuron_core::mesh_data::dfm::{self, DfmMesh};
use neuron_core::mesh_data::fvf::{Channel, Fvf};
use neuron_core::mesh_data::library::{self, VMeshLibrary};
use neuron_core::mesh_data::part::{self, WireError};
use neuron_core::mesh_data::render::{self, CLIENT_PLUGINS, RenderClaim, RenderPlugin};
use neuron_core::mesh_data::sphere::SphereMesh;
use neuron_core::mesh_data::trimesh::{TriMesh, TriMeshStream};
use neuron_core::mesh_data::vmesh::{VMeshData, VMeshError};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

use support::data::fnv64;

const LIBRARY: &[u8] = b"VMeshLibrary";
const WIRE: &[u8] = b"VMeshWire";
const RENDER: [&[u8]; 4] = [b"VMeshPart", b"MultiLevel", b"Mesh", b"Sphere"];
const WALK_DEPTH: usize = 64;

fn bits(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn bits_list(values: &[f32]) -> String {
    values
        .iter()
        .map(|&v| bits(v))
        .collect::<Vec<_>>()
        .join(",")
}

fn le16(values: &[u16]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}

fn le32(values: &[u32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}

fn lef(values: &[f32]) -> Vec<u8> {
    values
        .iter()
        .flat_map(|v| v.to_bits().to_le_bytes())
        .collect()
}

/// `fnv64` of the first `count` elements, or `short` when there are fewer.
fn fnv_prefix<T>(values: &[T], count: usize, bytes: impl Fn(&[T]) -> Vec<u8>) -> String {
    if values.len() < count {
        "short".to_owned()
    } else {
        fnv64(&bytes(&values[..count]))
    }
}

/// `"<id>:<offset>:<element bytes>"` for each table channel, `-` when
/// absent.
fn channels_text(fvf: Fvf) -> String {
    Channel::ALL
        .into_iter()
        .map(|c| {
            if fvf.has_channel(c) {
                format!(
                    "{:x}:{:x}:{:x}",
                    c.id(),
                    fvf.channel_offset(c),
                    c.element_bytes()
                )
            } else {
                format!("{:x}:-", c.id())
            }
        })
        .collect::<Vec<_>>()
        .join(",")
}

#[test]
fn mesh_fvf() {
    let lines: Vec<String> = input_lines("mesh_data/fvf.list")
        .iter()
        .map(|line| {
            let fvf = Fvf(u32::from_str_radix(line, 16).unwrap_or_else(|e| panic!("{line}: {e}")));
            let present: String = [1u32, 0x800, 8, 2, 4, 0x10, 0x20, 0]
                .iter()
                .map(|&id| if fvf.has_channel_id(id) { '1' } else { '0' })
                .collect();
            format!(
                "{{\"fvf\":\"{:x}\",\"stride\":\"{:x}\",\"dfm_stride\":\"{:x}\",\"present\":\"{present}\",\"channels\":\"{}\"}}",
                fvf.0,
                fvf.stride(),
                fvf.stride(),
                channels_text(fvf)
            )
        })
        .collect();
    check_suite("mesh_fvf.jsonl", &lines);
}

// --- mesh dump ---------------------------------------------------------------

struct Triggers {
    library: bool,
    render: bool,
    wire: bool,
}

fn join(path: &[u8], name: &[u8]) -> Vec<u8> {
    let mut out = path.to_vec();
    if !out.is_empty() {
        out.push(b'\\');
    }
    out.extend_from_slice(name);
    out
}

/// Every directory with a trigger child, depth first in archive order.
fn archetype_dirs(
    fs: &UtfFileSystem,
    path: &[u8],
    depth: usize,
    out: &mut Vec<(Vec<u8>, Triggers)>,
) {
    let pattern = join(path, b"*");
    let Ok(entries) = fs.find(&pattern) else {
        return;
    };
    let dirs: Vec<&[u8]> = entries
        .iter()
        .filter(|e| e.data.is_dir())
        .map(|e| e.data.name.as_slice())
        .collect();
    let named = |n: &[u8]| dirs.iter().any(|&d| eq_ignore_case(d, n));
    let triggers = Triggers {
        library: named(LIBRARY),
        render: RENDER.iter().any(|&n| named(n)),
        wire: named(WIRE),
    };
    if triggers.library || triggers.render || triggers.wire {
        out.push((path.to_vec(), triggers));
    }
    if depth >= WALK_DEPTH {
        return;
    }
    for name in dirs {
        let owned = eq_ignore_case(name, LIBRARY)
            || eq_ignore_case(name, WIRE)
            || RENDER.iter().any(|&n| eq_ignore_case(name, n));
        if owned || name.is_empty() || name.iter().any(|&b| b == b'\\' || b == b'/') {
            continue;
        }
        archetype_dirs(fs, &join(path, name), depth + 1, out);
    }
}

fn library_lines(library: Option<&VMeshLibrary>, lines: &mut Vec<String>) {
    let Some(library) = library else {
        lines.push("{\"library\":\"-\"}".to_owned());
        return;
    };
    lines.push(format!(
        "{{\"library\":\"{}\",\"dumps\":\"{}\"}}",
        library.entries.len(),
        library.dump_count()
    ));
    let mut shown = vec![false; library.meshes.len()];
    for (index, &m) in library.entries.iter().enumerate() {
        let mesh = &library.meshes[m];
        lines.push(format!(
            "{{\"entry\":\"{index}\",\"name\":\"{}\",\"crc\":\"{:08x}\",\"mesh\":\"{m}\"}}",
            hex(&mesh.name),
            mesh.crc
        ));
        if !shown[m] {
            shown[m] = true;
            mesh_data_lines(&mesh.data, lines);
        }
    }
    let mut group_order: Vec<usize> = Vec::new();
    for &m in &library.entries {
        if let Some(g) = library.meshes[m].group
            && !group_order.contains(&g)
        {
            group_order.push(g);
        }
    }
    for (ordinal, &g) in group_order.iter().enumerate() {
        let group = &library.groups[g];
        let members: Vec<String> = group.members.iter().map(ToString::to_string).collect();
        let (ib, starts) = match group.index_buffer(library) {
            Ok((buffer, starts)) => (
                fnv64(&le16(&buffer)),
                starts
                    .iter()
                    .map(|s| format!("{s:x}"))
                    .collect::<Vec<_>>()
                    .join(","),
            ),
            Err(_) => ("oob".to_owned(), String::new()),
        };
        lines.push(format!(
            "{{\"group\":\"{ordinal}\",\"fvf\":\"{:x}\",\"vt\":\"{:x}\",\"it\":\"{:x}\",\"members\":\"{}\",\"ib\":\"{ib}\",\"starts\":\"{starts}\"}}",
            group.fvf,
            group.vertex_total,
            group.index_total,
            members.join(",")
        ));
    }
}

/// `SizeKib`: `(float)((double)(int)size * (1.0 / 1024))`.
fn size_kib(size: usize) -> f32 {
    let size = i32::from_ne_bytes(u32::try_from(size).unwrap_or(u32::MAX).to_ne_bytes());
    #[allow(clippy::cast_possible_truncation)]
    let kib = (f64::from(size) * 0.000_976_562_5) as f32;
    kib
}

fn mesh_data_lines(data: &VMeshData, lines: &mut Vec<String>) {
    let h = &data.header;
    let size = h.vertices_offset() + data.vertices.len() + data.trailing.len();
    lines.push(format!(
        "{{\"flags\":\"{:02x}\",\"u01\":\"{}\",\"prim\":\"{:x}\",\"fg\":\"{:x}\",\"ni\":\"{:x}\",\"fvf\":\"{:x}\",\"nv\":\"{:x}\",\"stride\":\"{:x}\",\"kib\":\"{}\"}}",
        h.flags,
        hex(&h.unknown_01),
        h.primitive_type,
        h.face_group_count,
        h.index_count,
        h.fvf,
        h.vertex_count,
        data.stride(),
        bits(size_kib(size))
    ));
    for (index, g) in data.face_groups.iter().enumerate() {
        lines.push(format!(
            "{{\"fgi\":\"{index}\",\"mat\":\"{:08x}\",\"vb\":\"{:x}\",\"ve\":\"{:x}\",\"ic\":\"{:x}\",\"u0a\":\"{:04x}\"}}",
            g.material_crc, g.vertex_begin, g.vertex_end, g.index_count, g.unknown_0a
        ));
    }
    lines.push(format!(
        "{{\"indices\":\"{}\",\"vertices\":\"{}\"}}",
        fnv64(&le16(&data.indices)),
        fnv64(&data.vertices)
    ));
    for channel in Channel::ALL {
        let count = u32::from(h.vertex_count);
        let Some(record) = data.channel_record(channel, 0, count) else {
            continue;
        };
        let gathered = match data.channel_bytes(channel, 0, count) {
            Ok(Some(items)) => fnv64(&items.concat()),
            Ok(None) | Err(VMeshError::OutOfRange) => "oob".to_owned(),
            Err(_) => "err".to_owned(),
        };
        lines.push(format!(
            "{{\"ch\":\"{:x}\",\"offset\":\"{:x}\",\"elem\":\"{:x}\",\"stride\":\"{:x}\",\"data\":\"{gathered}\"}}",
            channel.id(),
            record.offset,
            record.element_bytes,
            record.stride
        ));
    }
}

fn plugin_name(plugin: RenderPlugin) -> &'static str {
    match plugin {
        RenderPlugin::VMesh => "VMesh",
        RenderPlugin::TriMesh { .. } => "TriMesh",
        RenderPlugin::SphereMesh => "SphereMesh",
    }
}

fn stream_line<T>(
    name: &str,
    stream: &TriMeshStream<T>,
    src_count: u32,
    bytes: impl Fn(&[T]) -> Vec<u8>,
) -> String {
    let idx = stream.indices.as_ref().map_or("-".to_owned(), |indices| {
        fnv_prefix(indices, src_count as usize, le32)
    });
    format!(
        "{{\"stream\":\"{name}\",\"n\":\"{:x}\",\"data\":\"{}\",\"idx\":\"{idx}\"}}",
        stream.values.len(),
        fnv64(&bytes(&stream.values))
    )
}

fn vec3_bytes(values: &[[f32; 3]]) -> Vec<u8> {
    lef(&values.concat())
}

fn vec2_bytes(values: &[[f32; 2]]) -> Vec<u8> {
    lef(&values.concat())
}

fn trimesh_lines(mesh: &TriMesh, lines: &mut Vec<String>) {
    let g = &mesh.geometry;
    lines.push(format!(
        "{{\"trimesh\":\"{:x}\",\"fvf\":\"{:x}\",\"count\":\"{:x}\",\"dumps\":\"{}\"}}",
        g.flags,
        g.src_fvf,
        g.src_count,
        mesh.dump_count()
    ));
    lines.push(stream_line("pos", &g.positions, g.src_count, vec3_bytes));
    if let Some(s) = &g.normals {
        lines.push(stream_line("normal", s, g.src_count, vec3_bytes));
    }
    if let Some(s) = &g.diffuse {
        lines.push(stream_line("diffuse", s, g.src_count, le32));
    }
    if let Some(s) = &g.tex0 {
        lines.push(stream_line("tex0", s, g.src_count, vec2_bytes));
    }
    if let Some(s) = &g.tex1 {
        lines.push(stream_line("tex1", s, g.src_count, vec2_bytes));
    }
    if let Some(anim) = &g.uv_anim {
        let lookup: Vec<u32> = anim
            .lookup
            .iter()
            .map(|&v| u32::from_ne_bytes(v.to_ne_bytes()))
            .collect();
        let frames = anim.frames.as_ref().map_or("-".to_owned(), |frames| {
            let wanted = anim.lookup.len() as u64 * u64::from(anim.frame_count);
            usize::try_from(wanted).map_or("short".to_owned(), |wanted| {
                fnv_prefix(frames, wanted, vec2_bytes)
            })
        });
        lines.push(format!(
            "{{\"uvanim\":\"{:x}\",\"lookup\":\"{}\",\"frames_n\":\"{:x}\",\"frames\":\"{frames}\",\"fps\":\"{}\",\"interp\":\"{:x}\",\"period\":\"{}\"}}",
            anim.lookup.len(),
            fnv64(&le32(&lookup)),
            anim.frame_count,
            bits(anim.fps),
            anim.interpolate,
            bits(anim.period)
        ));
    }
    lines.push(format!("{{\"groups\":\"{}\"}}", mesh.groups.len()));
    for (index, group) in mesh.groups.iter().enumerate() {
        let (n, idx) = group
            .indices
            .as_ref()
            .map_or(("-".to_owned(), "-".to_owned()), |i| {
                (format!("{:x}", i.len()), fnv64(&le16(i)))
            });
        let edges = group.edge_count();
        let ei = group
            .edge_indices
            .as_ref()
            .map_or("-".to_owned(), |e| fnv64(&le16(e)));
        let ea = group.edge_angles.as_ref().map_or("-".to_owned(), |a| {
            fnv_prefix(a, (edges >> 1) as usize, lef)
        });
        lines.push(format!(
            "{{\"tg\":\"{index}\",\"type\":\"{:x}\",\"crc\":\"{:08x}\",\"n\":\"{n}\",\"idx\":\"{idx}\",\"min\":\"{:x}\",\"max\":\"{:x}\",\"edges\":\"{edges:x}\",\"ei\":\"{ei}\",\"ea\":\"{ea}\",\"bucket\":\"{:x}\"}}",
            group.primitive_type, group.material_crc, group.min_index, group.max_index, group.bucket
        ));
    }
}

fn sphere_line(sphere: &SphereMesh) -> String {
    let names: Vec<String> = sphere
        .material_names
        .iter()
        .map(|(i, name)| format!("{i:x}:{}", hex(name)))
        .collect();
    format!(
        "{{\"sphere\":\"{}\",\"sides\":\"{:x}\",\"scale\":\"{}\",\"names\":\"{}\",\"dumps\":\"{}\"}}",
        bits(sphere.radius),
        sphere.sides,
        bits(sphere.scale),
        names.join(";"),
        sphere.dump_count()
    )
}

fn render_lines(fs: &mut dyn FileSystem, lines: &mut Vec<String>) {
    let multi = match render::multi_level(fs, &CLIENT_PLUGINS) {
        Ok(Some(a)) => Some(a),
        Ok(None) => None,
        Err(e) => {
            lines.push(format!("{{\"render\":\"error\",\"error\":\"{e}\"}}"));
            return;
        }
    };
    let archetype = match multi {
        Some(a) => Some(a),
        None => match render::single_level(fs, &CLIENT_PLUGINS) {
            Ok(a) => a,
            Err(e) => {
                lines.push(format!("{{\"render\":\"error\",\"error\":\"{e}\"}}"));
                return;
            }
        },
    };
    let Some(archetype) = archetype else {
        lines.push("{\"render\":\"0\"}".to_owned());
        return;
    };
    lines.push(format!(
        "{{\"render\":\"1\",\"multilevel\":\"{}\",\"levels\":\"{}\",\"switch\":\"{}\"}}",
        u8::from(archetype.multi_level),
        archetype.levels.len(),
        bits_list(&archetype.switch)
    ));
    for (index, level) in archetype.levels.iter().enumerate() {
        lines.push(format!(
            "{{\"level\":\"{index}\",\"plugin\":\"{}\"}}",
            plugin_name(CLIENT_PLUGINS[level.plugin])
        ));
        match &level.claim {
            RenderClaim::VMesh(part) => {
                let r = &part.mesh_ref;
                lines.push(format!(
                    "{{\"ref\":\"{:x}\",\"crc\":\"{:08x}\",\"vs\":\"{:x}\",\"vc\":\"{:x}\",\"is\":\"{:x}\",\"ic\":\"{:x}\",\"fs\":\"{:x}\",\"fc\":\"{:x}\",\"bounds\":\"{}\",\"center\":\"{}\",\"radius\":\"{}\"}}",
                    r.unknown_00,
                    r.mesh_crc,
                    r.vertex_start,
                    r.vertex_count,
                    r.index_start,
                    r.index_count,
                    r.face_group_start,
                    r.face_group_count,
                    bits_list(&r.bounds),
                    bits_list(&r.center),
                    bits(r.radius)
                ));
            }
            RenderClaim::TriMesh(mesh) => trimesh_lines(mesh, lines),
            RenderClaim::Sphere(sphere) => lines.push(sphere_line(sphere)),
        }
    }
}

fn wire_lines(fs: &mut dyn FileSystem, lines: &mut Vec<String>) {
    match part::load_wire(fs) {
        Err(e) => lines.push(format!("{{\"wire\":\"error\",\"error\":\"{e}\"}}")),
        Ok(None) => lines.push("{\"wire\":\"-\"}".to_owned()),
        Ok(Some(Err(WireError::ReadFailed))) => lines.push("{\"wire\":\"0\",\"dumps\":\"1\"}".to_owned()),
        Ok(Some(Err(WireError::Parse(_)))) => lines.push("{\"wire\":\"short\"}".to_owned()),
        Ok(Some(Ok(w))) => lines.push(format!(
            "{{\"wire\":\"1\",\"u00\":\"{:x}\",\"u02\":\"{:x}\",\"crc\":\"{:08x}\",\"bias\":\"{:x}\",\"remap\":\"{:x}\",\"count\":\"{:x}\",\"nv\":\"{:x}\",\"idx\":\"{}\",\"trailing\":\"{:x}\"}}",
            w.unknown_00,
            w.unknown_02,
            w.mesh_crc,
            w.index_bias,
            w.remap_count,
            w.index_count,
            w.vertex_count,
            fnv64(&le16(&w.indices)),
            w.trailing.len()
        )),
    }
}

/// The mesh dump of one archive.
fn render_mesh_file(host: &Host, file: &str) -> Vec<String> {
    let mut lines = Vec::new();
    let desc = FsDesc::read(file.as_bytes()).with_implementation(b"UTF");
    let Ok(root) = UtfFileSystem::open(host, &desc) else {
        lines.push(format!("{{\"file\":\"{file}\",\"open\":\"0\"}}"));
        return lines;
    };
    lines.push(format!("{{\"file\":\"{file}\",\"open\":\"1\"}}"));
    let mut dirs = Vec::new();
    archetype_dirs(&root, b"", 0, &mut dirs);
    for (path, triggers) in dirs {
        lines.push(format!("{{\"node\":\"{}\"}}", hex(&path)));
        let fresh = |root: &UtfFileSystem| -> Option<Box<dyn FileSystem>> {
            if path.is_empty() {
                Some(Box::new(root.clone()))
            } else {
                root.create_child(&FsDesc::read(&path)).ok()
            }
        };
        if triggers.library {
            match fresh(&root) {
                None => lines.push("{\"library\":\"nofs\"}".to_owned()),
                Some(mut fs) => match library::load(fs.as_mut(), -1) {
                    Ok(library) => library_lines(library.as_ref(), &mut lines),
                    Err(e) => lines.push(format!("{{\"library\":\"error\",\"error\":\"{e}\"}}")),
                },
            }
        }
        if triggers.render {
            match fresh(&root) {
                None => lines.push("{\"render\":\"nofs\"}".to_owned()),
                Some(mut fs) => render_lines(fs.as_mut(), &mut lines),
            }
        }
        if triggers.wire {
            match fresh(&root) {
                None => lines.push("{\"wire\":\"nofs\"}".to_owned()),
                Some(mut fs) => wire_lines(fs.as_mut(), &mut lines),
            }
        }
    }
    lines
}

// --- dfm dump ----------------------------------------------------------------

fn dfm_mesh_lines(mesh: &DfmMesh, bone_count: usize, lines: &mut Vec<String>) {
    lines.push(format!(
        "{{\"fgcount\":\"{:x}\",\"groups\":\"{}\"}}",
        mesh.face_group_count,
        mesh.face_groups.len()
    ));
    for (index, g) in mesh.face_groups.iter().enumerate() {
        lines.push(format!(
            "{{\"fg\":\"{index}\",\"kind\":\"{:x}\",\"crc\":\"{:08x}\",\"n\":\"{:x}\",\"idx\":\"{}\",\"min\":\"{:x}\",\"max\":\"{:x}\"}}",
            g.kind,
            g.material_crc,
            g.indices.len(),
            fnv64(&le16(&g.indices)),
            g.index_min,
            g.index_max
        ));
    }
    let vertices: Vec<f32> = mesh.vertices.concat();
    let uvi = mesh.uv_indices.as_ref().map_or("-".to_owned(), |u| {
        fnv_prefix(u, mesh.point_indices.len(), le32)
    });
    let pairs: Vec<u8> = mesh
        .skin_pairs
        .iter()
        .flat_map(|p| {
            let mut b = p.bone_id.to_le_bytes().to_vec();
            b.extend_from_slice(&p.weight.to_bits().to_le_bytes());
            b
        })
        .collect();
    let skin: Vec<u8> = mesh
        .skin
        .iter()
        .flat_map(|s| {
            let mut b = s.bone_count.to_le_bytes().to_vec();
            b.extend_from_slice(&s.first_pair.to_le_bytes());
            b
        })
        .collect();
    lines.push(format!(
        "{{\"points\":\"{:x}\",\"pi\":\"{}\",\"vertices\":\"{:x}\",\"v\":\"{}\",\"uvi\":\"{uvi}\",\"uvs\":\"{:x}\",\"uv\":\"{}\",\"pairs\":\"{:x}\",\"p\":\"{}\",\"skin\":\"{}\"}}",
        mesh.point_indices.len(),
        fnv64(&le32(&mesh.point_indices)),
        mesh.vertices.len(),
        fnv64(&lef(&vertices)),
        mesh.uvs.len(),
        fnv64(&vec2_bytes(&mesh.uvs)),
        mesh.skin_pairs.len(),
        fnv64(&pairs),
        fnv64(&skin)
    ));
    let uvd: Vec<u8> = mesh
        .uv_deform
        .iter()
        .flat_map(|r| {
            let mut b = r.bone_id.to_le_bytes().to_vec();
            b.extend_from_slice(&r.vertex_count.to_le_bytes());
            b.extend(lef(&[
                r.plane_distance,
                r.bone_x_to_u,
                r.bone_y_to_v,
                r.min_du,
                r.max_du,
                r.min_dv,
                r.max_dv,
            ]));
            b
        })
        .collect();
    let mut remap = Vec::new();
    for list in mesh.vertex_remap() {
        remap.extend_from_slice(&u32::try_from(list.len()).unwrap_or(u32::MAX).to_le_bytes());
        remap.extend(le32(&list));
    }
    let bone_lists = match mesh.bone_vertex_lists(bone_count) {
        Ok(lists) => {
            let mut bytes = Vec::new();
            for list in lists {
                bytes.extend_from_slice(
                    &u32::try_from(list.len()).unwrap_or(u32::MAX).to_le_bytes(),
                );
                bytes.extend(le16(&list));
            }
            fnv64(&bytes)
        }
        Err(_) => "invalid".to_owned(),
    };
    lines.push(format!(
        "{{\"uvd\":\"{:x}\",\"u\":\"{}\",\"remap\":\"{}\",\"bonelists\":\"{bone_lists}\"}}",
        mesh.uv_deform.len(),
        fnv64(&uvd),
        fnv64(&remap)
    ));
}

/// The dfm dump of one file.
fn render_dfm_file(host: &Host, file: &str) -> Vec<String> {
    let mut lines = Vec::new();
    let desc = FsDesc::read(file.as_bytes()).with_implementation(b"UTF");
    let Ok(mut root) = UtfFileSystem::open(host, &desc) else {
        lines.push(format!("{{\"file\":\"{file}\",\"open\":\"0\"}}"));
        return lines;
    };
    lines.push(format!("{{\"file\":\"{file}\",\"open\":\"1\"}}"));
    let dfm = match dfm::load(&mut root) {
        Ok(dfm) => dfm,
        Err(e) => {
            lines.push(format!("{{\"dfm\":\"error\",\"error\":\"{e}\"}}"));
            return lines;
        }
    };
    lines.push(format!("{{\"bones\":\"{}\"}}", dfm.bones.len()));
    for bone in &dfm.bones {
        let mut m = String::new();
        for v in bone.bone_to_root {
            let _ = write!(m, "{}", bits(v));
        }
        lines.push(format!(
            "{{\"bone\":\"{}\",\"m\":\"{m}\"}}",
            hex(&bone.name)
        ));
    }
    lines.push(format!(
        "{{\"multilevel\":\"{}\",\"lodcap\":\"{}\",\"fractions\":\"{}\"}}",
        u8::from(dfm.multi_level),
        dfm.lod_cap,
        bits_list(&dfm.fractions)
    ));
    for node in &dfm.meshes {
        let (child, ok) = match &node.mesh {
            None => (0, 0),
            Some(Err(_)) => (1, 0),
            Some(Ok(_)) => (1, 1),
        };
        lines.push(format!(
            "{{\"mesh\":\"{}\",\"child\":\"{child}\",\"ok\":\"{ok}\"}}",
            hex(&node.name)
        ));
        if let Some(Ok(mesh)) = &node.mesh {
            dfm_mesh_lines(mesh, dfm.bones.len(), &mut lines);
        }
    }
    lines
}

#[test]
fn mesh_files() {
    let host = Host::new(fixtures());
    let lines: Vec<String> = input_lines("mesh_data/mesh.list")
        .iter()
        .flat_map(|file| render_mesh_file(&host, file))
        .collect();
    check_suite("mesh_files.jsonl", &lines);
}

#[test]
fn dfm_files() {
    let host = Host::new(fixtures());
    let lines: Vec<String> = input_lines("mesh_data/dfm.list")
        .iter()
        .flat_map(|file| render_dfm_file(&host, file))
        .collect();
    check_suite("dfm_files.jsonl", &lines);
}

// --- data dumps --------------------------------------------------------------

fn extension(file: &str) -> String {
    file.rsplit_once('.')
        .map(|(_, e)| e.to_ascii_lowercase())
        .unwrap_or_default()
}

fn utf_files(root: &std::path::Path, extensions: &[&str]) -> Vec<String> {
    support::data::list_files(root)
        .into_iter()
        .filter(|file| extensions.contains(&extension(file).as_str()))
        .filter(|file| support::data::magic(root, file) == b"UTF ")
        .collect()
}

#[test]
fn data_dump_mesh() {
    let Some(dirs) = support::data::data_dirs("mesh") else {
        return;
    };
    let host = Host::new(&dirs.root);
    let files = utf_files(&dirs.root, &["vms", "3db", "cmp", "sph"]);
    let mut report = support::data::Report::new("mesh", &dirs);
    report.file_list(&files);
    let (mut nodes, mut meshes, mut levels, mut wires, mut errors) = (0, 0, 0, 0, 0);
    for file in &files {
        let lines = render_mesh_file(&host, file);
        for line in &lines {
            nodes += usize::from(line.starts_with("{\"node\""));
            meshes += usize::from(line.starts_with("{\"flags\""));
            levels += usize::from(line.starts_with("{\"level\""));
            wires += usize::from(line.starts_with("{\"wire\":\"1\""));
            if line.contains("\"error\"") || line.contains("\"oob\"") || line.contains(":\"short\"")
            {
                errors += 1;
                report.note(format!("{file}: {line}"));
            }
        }
        report.check(file, &lines);
    }
    report.note(format!(
        "{nodes} archetype nodes, {meshes} VMeshData, {levels} render levels, {wires} wireframes, {errors} lines with errors"
    ));
    report.finish();
}

#[test]
fn data_dump_dfm() {
    let Some(dirs) = support::data::data_dirs("dfm") else {
        return;
    };
    let host = Host::new(&dirs.root);
    let files = utf_files(&dirs.root, &["dfm"]);
    let mut report = support::data::Report::new("dfm", &dirs);
    report.file_list(&files);
    let (mut meshes, mut failed, mut bones) = (0, 0, 0);
    for file in &files {
        let lines = render_dfm_file(&host, file);
        for line in &lines {
            meshes += usize::from(line.starts_with("{\"mesh\""));
            failed += usize::from(line.contains("\"ok\":\"0\""));
            bones += usize::from(line.starts_with("{\"bone\""));
            if line.contains("\"error\"") || line.contains("invalid") || line.contains(":\"short\"")
            {
                report.note(format!("{file}: {line}"));
            }
        }
        report.check(file, &lines);
    }
    report.note(format!(
        "{meshes} meshes ({failed} without tables), {bones} bones"
    ));
    report.finish();
}
