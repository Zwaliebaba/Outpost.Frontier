use super::dfm::{self, DfmMeshError};
use super::fvf::Channel;
use super::library::{self, LibraryWarning, MeshLoadError};
use super::part::{self, PartError, WireError};
use super::render::{self, CLIENT_PLUGINS, RenderClaim, RenderPlugin};
use super::sphere;
use super::trimesh::{self, TRIANGLE_LIST, TRIANGLE_STRIP};
use super::vmesh::{VMeshData, VMeshError, VMeshRef, VWireData};
use super::{printed_index, printed_name};
use crate::hash::name_crc32;
use crate::utf::tests::{Item, build_full};
use crate::vfs::{FileSystem, UtfFileSystem};

use Item::{Dir, File};

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

/// A `VMeshData` leaf: groups are (material, begin, end, count).
fn vmesh_data(
    flags: u8,
    fvf: u16,
    groups: &[(u32, u16, u16, u16)],
    indices: &[u16],
    vertex_count: u16,
) -> Vec<u8> {
    let stride = super::fvf::Fvf(u32::from(fvf)).stride() as usize;
    let mut b = vec![flags, 0, 0, 0];
    b.extend_from_slice(&4u32.to_le_bytes());
    b.extend(le16(&[
        u16::try_from(groups.len()).unwrap(),
        u16::try_from(indices.len()).unwrap(),
        fvf,
        vertex_count,
    ]));
    for &(material, begin, end, count) in groups {
        b.extend_from_slice(&material.to_le_bytes());
        b.extend(le16(&[begin, end, count, 0xCC]));
    }
    b.extend(le16(indices));
    for v in 0..usize::from(vertex_count) * stride {
        b.push(u8::try_from(v % 251).unwrap());
    }
    b
}

fn vmesh_ref(crc: u32, ranges: [u16; 6]) -> Vec<u8> {
    let mut b = le32(&[0x3C, crc]);
    b.extend(le16(&ranges));
    b.extend(lef(&[
        1.0, -1.0, 2.0, -2.0, 3.0, -3.0, 0.5, 0.25, 0.125, 4.0,
    ]));
    b
}

fn wire(crc: u32, indices: &[u16]) -> Vec<u8> {
    let mut b = le16(&[0x10, 0]);
    b.extend_from_slice(&crc.to_le_bytes());
    b.extend(le16(&[3, 0, u16::try_from(indices.len()).unwrap(), 9]));
    b.extend(le16(indices));
    b
}

fn fs(items: &[Item]) -> UtfFileSystem {
    UtfFileSystem::from_bytes(build_full(items), b"test.3db").unwrap()
}

#[test]
fn vmesh_data_parses_and_gathers_channels() {
    let bytes = vmesh_data(
        1,
        0x152,
        &[(7, 2, 4, 3), (8, 0, 1, 3)],
        &[0, 1, 2, 1, 0, 1],
        5,
    );
    let data = VMeshData::parse(&bytes).unwrap();
    assert_eq!(data.face_groups.len(), 2);
    assert_eq!(data.face_groups[0].vertex_span(), 3);
    assert_eq!(data.stride(), 0x24);
    assert_eq!(data.buffer_indices().unwrap(), [2, 3, 4, 1, 0, 1]);
    let diffuse = data.channel_record(Channel::Diffuse, 1, 2).unwrap();
    assert_eq!(diffuse.offset, 0x24 + 0x20);
    assert_eq!(data.positions().unwrap().unwrap().len(), 5);
    assert!(data.tex_coords(1).unwrap().is_none());
    assert_eq!(
        VMeshData::parse(&vmesh_data(0, 0x112, &[], &[], 0)),
        Err(VMeshError::OldFormat)
    );
    assert!(matches!(
        VMeshData::parse(&bytes[..bytes.len() - 1]),
        Err(VMeshError::Truncated { .. })
    ));
    let short = vmesh_data(1, 0x112, &[(0, 0, 2, 4)], &[0, 1, 2], 3);
    assert_eq!(
        VMeshData::parse(&short).unwrap().buffer_indices(),
        Err(VMeshError::OutOfRange)
    );
}

#[test]
fn ref_and_wire_records() {
    let r = VMeshRef::parse(&vmesh_ref(0xABCD, [1, 2, 3, 4, 0, 2])).unwrap();
    assert_eq!(r.mesh_crc, 0xABCD);
    assert_eq!(r.radius, 4.0);
    assert_eq!(r.center, [0.5, 0.25, 0.125]);
    let data = VMeshData::parse(&vmesh_data(
        1,
        0x112,
        &[(0, 0, 2, 3), (0, 0, 2, 6)],
        &[0; 9],
        3,
    ))
    .unwrap();
    assert_eq!(r.triangle_count(&data), Ok(3));
    assert!(VMeshRef::parse(&[0; 0x3B]).is_err());
    let w = VWireData::parse(&wire(5, &[0, 1, 1, 2])).unwrap();
    assert_eq!((w.mesh_crc, w.index_count, w.vertex_count), (5, 4, 9));
    assert!(VWireData::parse(&wire(5, &[0, 1])[..0x11]).is_err());
}

fn library_items() -> Vec<Item> {
    vec![Dir(
        b"VMeshLibrary",
        vec![
            Dir(
                b"a.lod0.vms",
                vec![File(
                    b"VMeshData",
                    vmesh_data(1, 0x112, &[(1, 0, 2, 3)], &[0, 1, 2], 3),
                )],
            ),
            Dir(
                b"b.lod1.vms",
                vec![File(
                    b"VMeshData",
                    vmesh_data(1, 0x112, &[(1, 1, 3, 3)], &[0, 1, 2], 4),
                )],
            ),
            Dir(
                b"c.vms",
                vec![File(b"VMeshData", vmesh_data(1, 0x142, &[], &[], 2))],
            ),
            Dir(
                b"A.LOD0.VMS",
                vec![File(b"VMeshData", vmesh_data(1, 0x2, &[], &[], 1))],
            ),
            Dir(
                b"old.vms",
                vec![File(b"VMeshData", vmesh_data(0, 0x112, &[], &[], 0))],
            ),
            Dir(b"empty.vms", vec![File(b"VMeshData", Vec::new())]),
            Dir(b".hidden", vec![]),
            File(b"file.vms", vec![1]),
            Dir(
                b"d.vms",
                vec![File(b"VMeshData", vmesh_data(1, 0x112, &[], &[], 1))],
            ),
        ],
    )]
}

#[test]
fn library_groups_runs_of_one_format() {
    let mut fs = fs(&library_items());
    let library = library::load(&mut fs, -1).unwrap().unwrap();
    assert_eq!(fs.current_dir(), b"\\");
    // A.LOD0.VMS has a's CRC and is a's entry again.
    assert_eq!(library.entries, [0, 1, 2, 0, 3]);
    assert_eq!(library.meshes[0].crc, name_crc32(b"a.lod0.vms"));
    assert_eq!(library.groups.len(), 3);
    // a.lod0.vms and b.lod1.vms start a group; c.vms (another format) a
    // second; A.LOD0.VMS repeats a, which then moves into a third group
    // with d.vms.
    assert_eq!(library.groups[0].members, [1]);
    assert_eq!(
        (
            library.groups[0].vertex_total,
            library.groups[0].index_total
        ),
        (4, 3)
    );
    assert_eq!(library.groups[1].members, [2]);
    assert_eq!(library.groups[2].members, [0, 3]);
    assert_eq!(library.meshes[0].group, Some(2));
    let (buffer, starts) = library.groups[0].index_buffer(&library).unwrap();
    assert_eq!(buffer, [1, 2, 3]);
    assert_eq!(starts, [0]);
    let (buffer, starts) = library.groups[2].index_buffer(&library).unwrap();
    assert_eq!(buffer, [0, 1, 2]);
    assert_eq!(starts, [0, 3]);
    assert_eq!(library.groups[2].base_vertices(&library), [0, 3]);
    assert_eq!(
        library.warnings,
        [
            LibraryWarning::MeshFailed {
                name: b"old.vms".to_vec(),
                error: MeshLoadError::Parse(VMeshError::OldFormat)
            },
            LibraryWarning::MeshFailed {
                name: b"empty.vms".to_vec(),
                error: MeshLoadError::ReadFailed
            },
        ]
    );
    assert_eq!(library.dump_count(), 4);
    let filtered = library::load(&mut super::tests::fs(&library_items()), 1)
        .unwrap()
        .unwrap();
    // The filter skips a.lod0.vms; strstr is case-sensitive, so A.LOD0.VMS stays.
    assert_eq!(filtered.meshes.len(), 4);
    assert!(
        library::load(&mut super::tests::fs(&[]), -1)
            .unwrap()
            .is_none()
    );
}

fn part(crc: u32) -> Item {
    Dir(
        b"VMeshPart",
        vec![File(b"VMeshRef", vmesh_ref(crc, [0, 3, 0, 3, 0, 1]))],
    )
}

fn switch(values: &[f32]) -> Item {
    File(b"Switch2", lef(values))
}

#[test]
fn multi_level_offers_every_level_to_every_plugin() {
    let items = vec![Dir(
        b"MultiLevel",
        vec![
            switch(&[0.0, 10.0, 20.0]),
            Dir(b"Level0", vec![part(1)]),
            Dir(
                b"Level1",
                vec![part(2), Dir(b"Sphere", vec![File(b"Sides", le32(&[0]))])],
            ),
        ],
    )];
    let mut fs = fs(&items);
    let archetype = render::create_archetype(&mut fs, &CLIENT_PLUGINS)
        .unwrap()
        .unwrap();
    assert!(archetype.multi_level);
    assert_eq!(archetype.switch, [0.0, 10.0, 20.0]);
    assert_eq!(archetype.levels.len(), 2);
    assert!(
        matches!(&archetype.levels[0].claim, RenderClaim::VMesh(p) if p.mesh_ref.mesh_crc == 1)
    );
    // The sphere plugin comes last and keeps level 1.
    assert_eq!(archetype.levels[1].plugin, 2);

    let one = vec![Dir(b"MultiLevel", vec![switch(&[5.0])]), part(3)];
    let archetype = render::create_archetype(&mut super::tests::fs(&one), &CLIENT_PLUGINS)
        .unwrap()
        .unwrap();
    assert!(archetype.multi_level && archetype.levels.is_empty() && archetype.switch.is_empty());

    let missing_level = vec![
        Dir(
            b"MultiLevel",
            vec![switch(&[0.0, 1.0, 2.0]), Dir(b"Level0", vec![part(1)])],
        ),
        part(4),
    ];
    let archetype =
        render::create_archetype(&mut super::tests::fs(&missing_level), &CLIENT_PLUGINS)
            .unwrap()
            .unwrap();
    assert!(!archetype.multi_level);
    assert_eq!(archetype.switch, [0.0, render::DEFAULT_LOD_DISTANCE]);

    let no_switch = vec![Dir(b"MultiLevel", vec![Dir(b"Level0", vec![part(1)])])];
    let archetype = render::multi_level(&super::tests::fs(&no_switch), &[RenderPlugin::VMesh])
        .unwrap()
        .unwrap();
    assert_eq!(archetype.switch, [0.0, 0.0]);
    assert!(
        render::multi_level(&super::tests::fs(&no_switch), &[])
            .unwrap()
            .is_none()
    );
}

#[test]
fn part_and_wire_nodes() {
    let mut fs = fs(&[Dir(b"VMeshPart", vec![]), Dir(b"VMeshWire", vec![])]);
    assert_eq!(part::claim(&mut fs).unwrap(), Err(PartError::NoRef));
    assert_eq!(fs.current_dir(), b"\\");
    assert_eq!(
        part::load_wire(&mut fs).unwrap(),
        Some(Err(WireError::ReadFailed))
    );
    assert_eq!(fs.current_dir(), b"\\VMeshWire\\");

    let mut fs = super::tests::fs(&[Dir(
        b"VMeshWire",
        vec![File(b"VWireData", wire(7, &[1, 2]))],
    )]);
    let w = part::load_wire(&mut fs).unwrap().unwrap().unwrap();
    assert_eq!(w.indices, [1, 2]);
    assert_eq!(fs.current_dir(), b"\\");
    assert_eq!(part::claim(&mut fs).unwrap(), Err(PartError::NoNode));
}

#[test]
fn trimesh_loads_pools_and_groups() {
    let items = vec![Dir(
        b"Mesh",
        vec![
            Dir(
                b"Geometry",
                vec![
                    File(b"Points", lef(&[0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0])),
                    File(b"Point_indices", le32(&[0, 1, 0])),
                    File(b"UV1", lef(&[0.5, 0.5])),
                    File(b"UV1_indices", le32(&[0, 0])),
                    File(b"UV0_anim_lookup", le32(&[0, u32::MAX])),
                    File(b"UV0_frame_count", le32(&[3])),
                    File(b"UV0_fps", lef(&[2.0])),
                ],
            ),
            Dir(
                b"Face_groups",
                vec![
                    File(b"Count", le32(&[3])),
                    Dir(
                        b"Group1",
                        vec![
                            File(b"Face_indices", le16(&[5, 6, 7])),
                            File(b"Material_name", b"Hull\0x".to_vec()),
                        ],
                    ),
                    Dir(
                        b"Group0",
                        vec![File(b"Tristrip_indices", le16(&[4, 5, 6, 7]))],
                    ),
                    Dir(b"Group3", vec![File(b"Face_indices", le16(&[1, 2, 3]))]),
                    Dir(b"Group01", vec![File(b"Face_indices", le16(&[1, 2, 3]))]),
                ],
            ),
        ],
    )];
    let mut fs = fs(&items);
    let mesh = trimesh::load(&mut fs, false).unwrap().unwrap();
    assert_eq!(fs.current_dir(), b"\\");
    let g = &mesh.geometry;
    assert_eq!(g.positions.values.len(), 2);
    assert_eq!(g.src_fvf, 0x202);
    assert_eq!(g.src_count, 2);
    let anim = g.uv_anim.as_ref().unwrap();
    assert_eq!(anim.lookup, [0, -1]);
    assert_eq!(anim.period, 1.0);
    assert_eq!(mesh.groups.len(), 2);
    assert_eq!(mesh.groups[0].node_index, 0);
    assert_eq!(mesh.groups[0].primitive_type, TRIANGLE_LIST);
    assert_eq!(
        mesh.groups[0].indices.as_deref(),
        Some(&[0, 1, 2, 1, 3, 2][..])
    );
    assert_eq!((mesh.groups[0].min_index, mesh.groups[0].max_index), (4, 7));
    assert_eq!(mesh.groups[1].material_name.as_deref(), Some(&b"hull"[..]));
    assert_eq!(mesh.groups[1].material_crc, name_crc32(b"hull"));
    assert_eq!(mesh.dump_count(), 1);
    let strips = trimesh::load(&mut super::tests::fs(&items), true)
        .unwrap()
        .unwrap();
    assert_eq!(strips.groups[0].primitive_type, TRIANGLE_STRIP);
    assert!(
        trimesh::load(&mut super::tests::fs(&[]), false)
            .unwrap()
            .is_none()
    );
}

#[test]
fn sphere_names_by_index() {
    let items = vec![Dir(
        b"Sphere",
        vec![
            File(b"Radius", lef(&[300.0])),
            File(b"Sides", le32(&[3])),
            File(b"M2", b"Planet_CAP".to_vec()),
            File(b"M0", b"side".to_vec()),
            File(b"M3", b"extra".to_vec()),
        ],
    )];
    let mut fs = fs(&items);
    let s = sphere::load(&mut fs).unwrap().unwrap();
    assert_eq!(s.radius, 300.0);
    assert_eq!(
        s.material_names,
        [(0, b"side".to_vec()), (2, b"planet_cap".to_vec())]
    );
    assert_eq!(s.dump_count(), 1);
    let bare = sphere::load(&mut super::tests::fs(&[Dir(b"Sphere", vec![])]))
        .unwrap()
        .unwrap();
    assert_eq!((bare.radius, bare.sides, bare.dump_count()), (-1.0, 0, 2));
}

#[test]
fn printed_indices() {
    assert_eq!(printed_index(b"group12", b"Group"), Some(12));
    assert_eq!(printed_index(b"Group012", b"Group"), None);
    assert_eq!(printed_index(b"Group-1", b"Group"), Some(u32::MAX));
    assert_eq!(printed_index(b"Group+1", b"Group"), None);
    assert_eq!(printed_name(b"M", u32::MAX), b"M-1");
}

fn dfm_mesh() -> Item {
    Dir(
        b"Mesh0",
        vec![
            Dir(
                b"Face_groups",
                vec![
                    File(b"Count", le32(&[2])),
                    Dir(
                        b"Group0",
                        vec![
                            File(b"Material_name", b"skin\0".to_vec()),
                            File(b"Tristrip_indices", le16(&[3, 4, 5, 6])),
                        ],
                    ),
                    Dir(
                        b"Group1",
                        vec![
                            File(b"Material_name", b"eye\0".to_vec()),
                            File(b"Face_indices", le16(&[0, 1, 2])),
                        ],
                    ),
                    Dir(b"Group2", vec![File(b"Material_name", b"x\0".to_vec())]),
                ],
            ),
            Dir(
                b"Geometry",
                vec![
                    File(b"Point_indices", le32(&[0, 1, 2, 1, 9])),
                    File(b"Points", lef(&[0.0; 9])),
                    File(b"Vertex_normals", lef(&[1.0; 9])),
                    File(b"UV0", lef(&[0.5; 4])),
                    File(b"Point_bone_count", le32(&[1, 2, 0])),
                    File(b"Bone_id_chain", le32(&[0, 1, 0])),
                    File(b"Bone_weight_chain", lef(&[1.0, 0.5, 0.5])),
                    File(b"UV_bone_id", le32(&[1])),
                    File(b"UV_vertex_count", le32(&[2])),
                    File(b"UV_plane_distance", lef(&[0.1])),
                    File(b"Bone_X_to_U_scale", lef(&[0.2])),
                    File(b"Bone_Y_to_V_scale", lef(&[0.3])),
                    File(b"Min_du", lef(&[0.4])),
                    File(b"Max_du", lef(&[0.5])),
                    File(b"Min_dv", lef(&[0.6])),
                    File(b"Max_dv", lef(&[0.7])),
                ],
            ),
        ],
    )
}

fn bone(name: &'static [u8], scale: f32) -> Item {
    Dir(name, vec![File(b"Bone to root", lef(&[scale; 12]))])
}

#[test]
fn dfm_skeleton_fractions_and_meshes() {
    let items = vec![
        bone(b"Root.3db", 1.0),
        bone(b"Head.3db", 2.0),
        Dir(b"Bad.3db", vec![File(b"Bone to root", vec![0; 0x2F])]),
        Dir(
            b"MultiLevel",
            vec![
                File(b"Fractions", lef(&[1.0, 0.5])),
                dfm_mesh(),
                Dir(b"Mesh1", vec![]),
            ],
        ),
    ];
    let mut fs = fs(&items);
    let file = dfm::load(&mut fs).unwrap();
    assert_eq!(file.bones.len(), 2);
    assert_eq!(file.bones[1].name, b"Head.3db");
    assert_eq!(
        (file.lod_cap, file.fractions.as_slice()),
        (2, &[1.0, 0.5][..])
    );
    assert_eq!(file.meshes.len(), 2);
    assert_eq!(file.meshes[1].mesh, Some(Err(DfmMeshError::NoFaceGroups)));
    let mesh = file.meshes[0].mesh.as_ref().unwrap().as_ref().unwrap();
    assert_eq!(mesh.face_groups.len(), 2);
    assert_eq!(mesh.face_groups[0].indices, [0, 1, 2, 1, 3, 2]);
    assert_eq!(mesh.face_groups[0].index_min, 3);
    assert_eq!(mesh.face_groups[1].material_crc, name_crc32(b"eye"));
    assert_eq!(mesh.vertices.len(), 3);
    assert_eq!(mesh.skin[2].first_pair, 3);
    assert_eq!(mesh.vertex_remap(), [vec![0], vec![1, 3], vec![2]]);
    assert_eq!(mesh.bone_vertex_lists(2).unwrap(), [vec![0, 1], vec![1]]);
    assert_eq!(mesh.bone_vertex_lists(1), Err(DfmMeshError::SkinOutOfRange));
    assert_eq!(mesh.uv_deform[0].vertex_count, 2);
    assert_eq!(mesh.index_count(), 9);

    let flat = vec![bone(b"Root.3db", 1.0), Dir(b"Mesh0", vec![])];
    let file = dfm::load(&mut super::tests::fs(&flat)).unwrap();
    assert_eq!(
        (file.multi_level, file.lod_cap, file.fractions.as_slice()),
        (false, 1, &[1.0][..])
    );
    assert_eq!(file.meshes.len(), 1);
}

#[test]
fn dfm_refuses_short_arrays() {
    let items = vec![Dir(
        b"Mesh0",
        vec![
            Dir(b"Face_groups", vec![]),
            Dir(
                b"Geometry",
                vec![
                    File(b"Points", lef(&[0.0; 6])),
                    File(b"Vertex_normals", lef(&[0.0; 3])),
                ],
            ),
        ],
    )];
    let mut fs = fs(&items);
    let file = dfm::load(&mut fs).unwrap();
    assert_eq!(file.meshes[0].mesh, Some(Err(DfmMeshError::ShortNormals)));
}

struct Mutator(u32);

impl Mutator {
    fn next(&mut self) -> u32 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.0 = x;
        x
    }

    fn below(&mut self, n: usize) -> usize {
        self.next() as usize % n.max(1)
    }
}

fn exercise(bytes: Vec<u8>) {
    let Ok(base) = UtfFileSystem::from_bytes(bytes, b"m.3db") else {
        return;
    };
    let plugins = [
        RenderPlugin::VMesh,
        RenderPlugin::TriMesh { tristrips: true },
        RenderPlugin::SphereMesh,
    ];
    let mut fs = base.clone();
    if let Ok(Some(library)) = library::load(&mut fs, 0) {
        for group in &library.groups {
            let _ = group.index_buffer(&library);
        }
        for mesh in &library.meshes {
            for channel in Channel::ALL {
                let _ =
                    mesh.data
                        .channel_bytes(channel, 0, u32::from(mesh.data.header.vertex_count));
            }
        }
    }
    let _ = render::create_archetype(&mut base.clone(), &plugins);
    let _ = render::create_archetype(&mut base.clone(), &CLIENT_PLUGINS);
    let _ = part::load_wire(&mut base.clone());
    if let Ok(file) = dfm::load(&mut base.clone()) {
        for node in &file.meshes {
            if let Some(Ok(mesh)) = &node.mesh {
                let _ = mesh.vertex_remap();
                let _ = mesh.bone_vertex_lists(file.bones.len());
            }
        }
    }
}

#[test]
fn mutated_archives_never_panic() {
    let mut mesh_items = library_items();
    mesh_items.push(Dir(
        b"MultiLevel",
        vec![switch(&[0.0, 1.0]), Dir(b"Level0", vec![part(1)])],
    ));
    mesh_items.push(Dir(
        b"VMeshWire",
        vec![File(b"VWireData", wire(1, &[0, 1]))],
    ));
    mesh_items.push(Dir(
        b"Sphere",
        vec![File(b"Sides", le32(&[2])), File(b"M1", b"m".to_vec())],
    ));
    let dfm_items = vec![
        bone(b"A.3db", 1.0),
        Dir(
            b"MultiLevel",
            vec![File(b"Fractions", lef(&[1.0])), dfm_mesh()],
        ),
    ];
    let seeds = [build_full(&mesh_items), build_full(&dfm_items)];
    for seed in &seeds {
        exercise(seed.clone());
    }
    let mut rng = Mutator(0x5EED_1234);
    for round in 0..3000 {
        let mut bytes = seeds[round % 2].clone();
        for _ in 0..=rng.below(6) {
            match rng.below(4) {
                0 => {
                    let at = rng.below(bytes.len());
                    bytes[at] = rng.next() as u8;
                }
                1 => {
                    let at = rng.below(bytes.len().saturating_sub(4));
                    let value = match rng.below(4) {
                        0 => rng.next(),
                        1 => rng.next() % 0x200,
                        2 => u32::MAX - rng.next() % 16,
                        _ => 0,
                    };
                    bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
                }
                2 => {
                    let keep = rng.below(bytes.len() + 1);
                    bytes.truncate(keep.max(0x39));
                }
                _ => {
                    let at = rng.below(bytes.len());
                    bytes.insert(at, rng.next() as u8);
                }
            }
        }
        exercise(bytes);
    }
}

#[test]
fn leaf_parsers_never_panic_on_any_prefix() {
    let data = vmesh_data(1, 0x252, &[(1, 0, 2, 3)], &[0, 1, 2], 3);
    for len in 0..=data.len() {
        let _ = VMeshData::parse(&data[..len]);
        let _ = VMeshRef::parse(&data[..len]);
        let _ = VWireData::parse(&data[..len]);
    }
    let fs: &dyn FileSystem = &fs(&[]);
    let _ = fs.kind();
}
