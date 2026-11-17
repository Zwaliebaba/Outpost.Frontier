use super::schema::value_type;
use super::*;
use crate::utf::tests::{Item, build_full};
use crate::vfs::{GENERIC_READ, MemFileSystem, MemSource, UtfFileSystem};

fn floats(values: &[f32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}

fn library() -> Vec<Item> {
    vec![
        Item::Dir(
            b"Material library",
            vec![
                Item::File(b"Material count", 3u32.to_le_bytes().to_vec()),
                Item::Dir(
                    b"hull",
                    vec![
                        Item::File(b"Type", b"DcDtOcOt\0".to_vec()),
                        Item::File(b"Dc", floats(&[0.5, 0.25, 0.125])),
                        Item::File(b"Oc", floats(&[0.5])),
                        Item::File(b"Dt_name", b"hull.tga\0junk".to_vec()),
                        Item::File(b"Dt_flags", 0x0000_0029u32.to_le_bytes().to_vec()),
                    ],
                ),
                Item::Dir(b"HULL", vec![Item::File(b"Type", b"DcDt\0".to_vec())]),
                Item::Dir(
                    b"rock",
                    vec![
                        Item::File(b"Type", b"DetailMapMaterial".to_vec()),
                        Item::File(b"flip u", (-3i32).to_le_bytes().to_vec()),
                        Item::File(b"flip v", 7u32.to_le_bytes().to_vec()),
                        Item::File(b"Dm_flags", 0x0000_000Cu32.to_le_bytes().to_vec()),
                        Item::File(b"TileRate", 0x7F80_0001u32.to_le_bytes().to_vec()),
                        Item::File(b"Dc", floats(&[1.0, 2.0])),
                    ],
                ),
                Item::Dir(b"odd", vec![Item::File(b"Type", b"DcDtBt\0".to_vec())]),
                Item::Dir(b"empty", vec![Item::File(b"Type", Vec::new())]),
                Item::File(b"stray", vec![1]),
            ],
        ),
        Item::Dir(
            b"MaterialAnim",
            vec![
                Item::Dir(
                    b"spin",
                    vec![
                        Item::File(b"MACount", 2u32.to_le_bytes().to_vec()),
                        Item::File(b"MAFlags", 5u32.to_le_bytes().to_vec()),
                        Item::File(b"MADeltas", floats(&[1.0, 0.1, 0.2, 0.3, 0.4, 2.0, 0.5])),
                        Item::File(b"MAKeys", floats(&[1.0, 2.0, 3.0, 4.0])),
                    ],
                ),
                Item::Dir(b"none", vec![]),
            ],
        ),
    ]
}

#[test]
fn kinds_and_schemas() {
    assert_eq!(KINDS.len(), 22);
    assert_eq!(
        MaterialKind::by_name(b"DcDtEcOcOtTwo")
            .unwrap()
            .schema()
            .len(),
        10
    );
    assert_eq!(MaterialKind::by_name(b"dcdt"), None);
    let counts: Vec<usize> = KINDS.iter().map(|k| k.schema().len()).collect();
    assert_eq!(
        counts,
        [
            10, 10, 10, 10, 10, 10, 10, 10, 9, 5, 5, 9, 4, 4, 0, 4, 13, 13, 18, 18, 13, 13
        ]
    );
    let blocks: Vec<usize> = KINDS
        .iter()
        .map(|k| Material::new(*k).fields.textures.len())
        .collect();
    assert_eq!(
        blocks,
        [
            1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 0, 1, 2, 2, 3, 3, 2, 2
        ]
    );
}

#[test]
fn initial_values() {
    let material = Material::new(MaterialKind::by_name(b"AtmosphereMaterial").unwrap());
    let params = material.parameters();
    assert_eq!(params.len(), 9);
    assert_eq!(params[2].text.as_deref(), Some(&b"(None)"[..]));
    assert_eq!(params[3].words[0], 4);
    assert_eq!(params[4].words[0], 2);
    assert_eq!(params[8].words[0], 0x3F83_3333);
    let detail = Material::new(MaterialKind::by_name(b"IllumDetailMapMaterial").unwrap());
    assert_eq!(detail.parameter(17).unwrap().words[0], 4.0f32.to_bits());
    assert_eq!(detail.parameter(18), None);
    assert_eq!(
        Material::new(MaterialKind::by_name(b"NullMaterial").unwrap()).parameters(),
        []
    );
}

#[test]
fn material_library_loading() {
    let mem = MemFileSystem::new(
        None,
        GENERIC_READ,
        3,
        MemSource::Copy(Some(&build_full(&library()))),
    );
    let mut fs = UtfFileSystem::open_memory(&mem, &FsDesc::read(b"m.mat")).unwrap();
    let nodes = load_material_library(&mut fs).unwrap();
    let names: Vec<&[u8]> = nodes.iter().map(|n| n.name.as_slice()).collect();
    assert_eq!(names, [&b"hull"[..], b"HULL", b"rock", b"odd", b"empty"]);
    assert_eq!(nodes[1].entry, MaterialEntry::Duplicate);
    assert_eq!(nodes[4].entry, MaterialEntry::NoType);
    let MaterialEntry::Typed {
        type_name,
        material: None,
    } = &nodes[3].entry
    else {
        panic!("odd: {:?}", nodes[3].entry);
    };
    assert_eq!(type_name, b"DcDtBt");

    let MaterialEntry::Typed {
        material: Some(hull),
        ..
    } = &nodes[0].entry
    else {
        panic!("hull");
    };
    let params = hull.parameters();
    // Ac reads back the diffuse colour copied by Dc.
    assert_eq!(
        params[0].words[..3],
        [0.5f32.to_bits(), 0.25f32.to_bits(), 0.125f32.to_bits()]
    );
    assert_eq!(params[5].words[0], 0.5f32.to_bits());
    assert!(hull.fields.alpha_blend);
    assert_eq!(params[6].text.as_deref(), Some(&b"hull.tga"[..]));
    assert_eq!(params[7].words[0], 2);
    assert_eq!((params[8].words[0], params[9].words[0]), (1, 2));
    assert_eq!(params[5].kind, value_type::FLOAT);

    let MaterialEntry::Typed {
        material: Some(rock),
        ..
    } = &nodes[2].entry
    else {
        panic!("rock");
    };
    let params = rock.parameters();
    assert_eq!((params[6].words[0], params[7].words[0]), (0, 1));
    assert_eq!(params[10].words[0], 0);
    assert_eq!(params[11].words[0], 3);
    assert_eq!(params[12].words[0], 0x7FC0_0001);
    // A short Dc leaf leaves the initial colour.
    assert_eq!(params[1].words[..3], [1.0f32.to_bits(); 3]);

    let anims = load_material_animations(&mut fs).unwrap();
    assert_eq!(anims.len(), 2);
    assert_eq!(
        (anims[0].count, anims[0].flags, anims[0].deltas_asked),
        (2, 5, 40)
    );
    assert_eq!(anims[0].deltas.as_ref().map(Vec::len), Some(28));
    assert_eq!(anims[0].delta_records().len(), 1);
    assert_eq!(anims[0].key_records()[0].values[3], 4.0f32.to_bits());
    assert_eq!((anims[1].count, anims[1].deltas.clone()), (0, None));
}

/// Deterministic mutations: loading never panics.
#[test]
fn mutated_libraries_do_not_panic() {
    let base = build_full(&library());
    let mut seed = 0x0BAD_5EEDu32;
    for round in 0..600 {
        let mut bytes = base.clone();
        for _ in 0..1 + round % 4 {
            seed = seed.wrapping_mul(1_103_515_245).wrapping_add(12345);
            let at = (seed >> 8) as usize % bytes.len();
            bytes[at] = (seed >> 5) as u8;
        }
        let mem = MemFileSystem::new(None, GENERIC_READ, 3, MemSource::Copy(Some(&bytes)));
        let Ok(mut fs) = UtfFileSystem::open_memory(&mem, &FsDesc::read(b"m")) else {
            continue;
        };
        if let Some(nodes) = load_material_library(&mut fs) {
            for node in nodes {
                if let MaterialEntry::Typed {
                    material: Some(m), ..
                } = node.entry
                {
                    let _ = m.parameters();
                }
            }
        }
        let _ = load_material_animations(&mut fs);
    }
}
