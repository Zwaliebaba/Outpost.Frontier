use super::*;

const TETRA: [[u16; 3]; 4] = [[0, 1, 2], [0, 3, 1], [1, 3, 2], [2, 3, 0]];

fn put(out: &mut Vec<u8>, words: &[u32]) {
    for w in words {
        out.extend_from_slice(&w.to_le_bytes());
    }
}

fn opposite_offset(triangles: &[[u16; 3]], t: usize, e: usize) -> i32 {
    let (a, b) = (triangles[t][e], triangles[t][(e + 1) % 3]);
    for (u, tri) in triangles.iter().enumerate() {
        for f in 0..3 {
            if tri[f] == b && tri[(f + 1) % 3] == a {
                let here = (16 * t + 4 * (e + 1)) as i32;
                let there = (16 * u + 4 * (f + 1)) as i32;
                return (there - here) / 4;
            }
        }
    }
    panic!("open mesh");
}

/// A surface with one terminal ledge (a tetrahedron), points inline.
fn tetra_surface() -> Vec<u8> {
    let n = TETRA.len();
    let ledge = 0x30usize;
    let points = ledge + 16 + 16 * n;
    let node = points + 16 * 4;
    let size = node + 0x1c;
    let mut out = Vec::new();
    put(
        &mut out,
        &[
            1.0f32.to_bits(),
            2.0f32.to_bits(),
            3.0f32.to_bits(),
            0.5f32.to_bits(),
            0.25f32.to_bits(),
            0.125f32.to_bits(),
            4.0f32.to_bits(),
            ((size as u32) << 8) | 0xC7,
            node as u32,
            0,
            0,
            0,
        ],
    );
    let ledge_size = (16 + 16 * n + 16 * 4) as u32;
    put(
        &mut out,
        &[
            (points - ledge) as u32,
            77,
            ((ledge_size / 16) << 8) | 4,
            n as u32,
        ],
    );
    for (t, tri) in TETRA.iter().enumerate() {
        let mut words = vec![(t as u32) | (1 << 12)];
        for (e, &start) in tri.iter().enumerate() {
            let opp = opposite_offset(&TETRA, t, e);
            words.push(u32::from(start) | (((opp as u32) & 0x7fff) << 16));
        }
        put(&mut out, &words);
    }
    for p in 0..4u32 {
        put(
            &mut out,
            &[
                (p as f32).to_bits(),
                1.0f32.to_bits(),
                (-(p as f32)).to_bits(),
                0,
            ],
        );
    }
    let back = (ledge as i32 - node as i32) as u32;
    put(&mut out, &[0, back, 0, 0, 0, 2.0f32.to_bits()]);
    out.extend_from_slice(&[1, 2, 3, 0]);
    assert_eq!(out.len(), size);
    out
}

/// A chunk: its tag and payload.
type Chunk<'a> = (&'a [u8; 4], Vec<u8>);

fn sur_file(parts: &[(u32, Vec<Chunk<'_>>)]) -> Vec<u8> {
    let mut out = b"vers".to_vec();
    out.extend_from_slice(&2.0f32.to_le_bytes());
    for (id, chunks) in parts {
        out.extend_from_slice(&id.to_le_bytes());
        out.extend_from_slice(&(chunks.len() as i32).to_le_bytes());
        for (tag, payload) in chunks {
            out.extend_from_slice(*tag);
            out.extend_from_slice(payload);
        }
    }
    out
}

fn surf_payload(blob: &[u8]) -> Vec<u8> {
    let mut out = (blob.len() as u32).to_le_bytes().to_vec();
    out.extend_from_slice(blob);
    out
}

fn exts_payload(min: f32, max: f32) -> Vec<u8> {
    let mut out = Vec::new();
    for v in [min, min, min, max, max, max] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    out
}

fn hpid_payload(ids: &[u32]) -> Vec<u8> {
    let mut out = (ids.len() as i32).to_le_bytes().to_vec();
    for id in ids {
        out.extend_from_slice(&id.to_le_bytes());
    }
    out
}

fn sample_file() -> Vec<u8> {
    sur_file(&[
        (
            7,
            vec![
                (b"!fxd", Vec::new()),
                (b"exts", exts_payload(-1.0, 1.0)),
                (b"surf", surf_payload(&tetra_surface())),
                (b"hpid", hpid_payload(&[5, 9, 5])),
            ],
        ),
        (3, vec![(b"EXTS", exts_payload(0.0, 2.0))]),
    ])
}

#[test]
fn loads_parts_and_chunks() {
    let load = load(&sample_file());
    assert_eq!(load.result, Ok(()));
    assert_eq!(load.reported(), 0);
    let ids: Vec<u32> = load.parts.keys().copied().collect();
    assert_eq!(ids, [3, 7]);
    let part = &load.parts[&7];
    assert!(!part.not_fixed);
    assert_eq!(part.hardpoint_ids, [5, 9]);
    assert_eq!(part.extents.unwrap().max, Vector::new(1.0, 1.0, 1.0));
    assert!(load.parts[&3].not_fixed);
    assert!(load.parts[&3].surface.is_none());
}

#[test]
fn decodes_the_tetrahedron() {
    let surface = CompactSurface::parse(&tetra_surface()).unwrap();
    assert_eq!(surface.max_deviation(), 0xC7);
    assert_eq!(surface.byte_size() as usize, tetra_surface().len());
    assert_eq!(surface.nodes.len(), 1);
    assert!(surface.nodes[0].is_terminal());
    assert_eq!(surface.nodes[0].box_sizes, [1, 2, 3]);
    assert_eq!(surface.terminal_ledges(), [0]);
    assert_eq!(surface.single_convex(), Some(0));
    let ledge = &surface.ledges[0];
    assert_eq!(ledge.client_data, 77);
    assert_eq!(ledge.unknown_flag_bits(), 1);
    assert_eq!(ledge.points.len(), 4);
    assert_eq!(ledge.triangles.len(), 4);
    for (t, triangle) in ledge.triangles.iter().enumerate() {
        assert_eq!(triangle.tri_index() as usize, t);
        assert_eq!(triangle.pierce_index(), 1);
        for e in 0..3u8 {
            let here = EdgeRef {
                triangle: t,
                edge: e,
            };
            let opposite = ledge.edge(here).opposite;
            // The opposite edge runs the other way.
            assert_eq!(
                ledge.edge(opposite).start_point_index(),
                ledge
                    .edge(CompactLedge::next_edge(here))
                    .start_point_index()
            );
            assert_eq!(ledge.edge(opposite).opposite, here);
        }
    }
    assert_eq!(
        ledge
            .start_point(EdgeRef {
                triangle: 1,
                edge: 1
            })
            .position,
        Vector::new(3.0, 1.0, -3.0)
    );
}

#[test]
fn header_failures() {
    assert_eq!(load(b"ver").result, Err(SurError::NoTag));
    assert_eq!(load(b"surfxxxx").result, Err(SurError::OldFormat));
    assert_eq!(load(b"VERS\0\0").result, Err(SurError::TruncatedVersion));
    let mut one = b"vers".to_vec();
    one.extend_from_slice(&1.0f32.to_le_bytes());
    assert_eq!(load(&one).result, Err(SurError::BadVersion(0x3F80_0000)));
    assert_eq!(load(&one).reported(), 1);
    let mut empty = b"vErS".to_vec();
    empty.extend_from_slice(&2.0f32.to_le_bytes());
    assert_eq!(load(&empty).result, Ok(()));
    // One to three bytes where a part id would start end the file.
    empty.extend_from_slice(&[1, 2, 3]);
    let tail = load(&empty);
    assert_eq!(tail.result, Ok(()));
    assert!(tail.parts.is_empty());
}

#[test]
fn unknown_chunk_keeps_earlier_parts() {
    let bytes = sur_file(&[
        (1, vec![(b"exts", exts_payload(0.0, 1.0))]),
        (2, vec![(b"!fxd", Vec::new()), (b"su\0f", Vec::new())]),
    ]);
    let load = load(&bytes);
    assert_eq!(
        load.result,
        Err(SurError::UnknownChunk {
            part_id: 2,
            tag: *b"su\0f"
        })
    );
    assert_eq!(load.reported(), 1);
    assert_eq!(load.parts.len(), 2);
    assert!(!load.parts[&2].not_fixed);
}

#[test]
fn repeated_part_is_reset() {
    let bytes = sur_file(&[
        (
            4,
            vec![
                (b"!fxd", Vec::new()),
                (b"hpid", hpid_payload(&[1])),
                (b"surf", surf_payload(&tetra_surface())),
            ],
        ),
        (4, vec![(b"hpid", hpid_payload(&[2]))]),
    ]);
    let load = load(&bytes);
    assert_eq!(load.result, Ok(()));
    assert_eq!(load.duplicate_part_ids, [4]);
    assert_eq!(load.reported(), 1);
    let part = &load.parts[&4];
    assert!(part.not_fixed);
    assert!(part.surface.is_none());
    assert_eq!(part.extents, Some(Extents::default()));
    assert_eq!(part.hardpoint_ids, [2]);
}

#[test]
fn part_without_exts_has_no_extents() {
    let load = load(&sur_file(&[(9, vec![(b"!fxd", Vec::new())])]));
    assert_eq!(load.parts[&9].extents, None);
}

#[test]
fn truncation_inside_a_part_is_an_error() {
    let full = sample_file();
    // Every cut after the header that does not fall on a part boundary.
    let second_part = full.len() - (8 + 4 + 24);
    for cut in 8..full.len() {
        let result = load(&full[..cut]).result;
        let boundary = cut == 8 || cut == second_part;
        let short_id = (8..12).contains(&cut) || (second_part..second_part + 4).contains(&cut);
        if boundary || short_id {
            assert_eq!(result, Ok(()), "cut {cut}");
        } else {
            assert!(
                matches!(result, Err(SurError::Truncated { .. })),
                "cut {cut}: {result:?}"
            );
        }
    }
}

#[test]
fn compact_errors() {
    let good = tetra_surface();
    assert_eq!(
        CompactSurface::parse(&good[..0x20]),
        Err(CompactError::TooShort)
    );
    let node = good.len() - 0x1c;

    let mut bad = good.clone();
    bad[0x20..0x24].copy_from_slice(&(good.len() as i32).to_le_bytes());
    assert!(matches!(
        CompactSurface::parse(&bad),
        Err(CompactError::NodeOutOfRange { .. })
    ));

    // A terminal root without a ledge.
    let mut bad = good.clone();
    bad[node..node + 4].copy_from_slice(&0i32.to_le_bytes());
    bad[node + 4..node + 8].copy_from_slice(&0i32.to_le_bytes());
    assert_eq!(
        CompactSurface::parse(&bad),
        Err(CompactError::TerminalWithoutLedge { node: node as u32 })
    );

    let mut bad = good.clone();
    let back = i32::from_le_bytes(good[node + 4..node + 8].try_into().unwrap());
    bad[node + 4..node + 8].copy_from_slice(&(back + 4).to_le_bytes());
    assert_eq!(
        CompactSurface::parse(&bad),
        Err(CompactError::MisalignedLedge { offset: 0x34 })
    );

    let mut bad = good.clone();
    bad[0x3c..0x3e].copy_from_slice(&(-1i16).to_le_bytes());
    assert_eq!(
        CompactSurface::parse(&bad),
        Err(CompactError::NegativeTriangleCount { ledge: 0x30 })
    );

    let mut bad = good.clone();
    bad[0x3c..0x3e].copy_from_slice(&100i16.to_le_bytes());
    assert_eq!(
        CompactSurface::parse(&bad),
        Err(CompactError::TrianglesOutOfRange { ledge: 0x30 })
    );

    let mut bad = good.clone();
    bad[0x30..0x34].copy_from_slice(&0x7000i32.to_le_bytes());
    assert_eq!(
        CompactSurface::parse(&bad),
        Err(CompactError::PointsOutOfRange { ledge: 0x30 })
    );

    // Edge 0 of triangle 0 names the info dword of triangle 1.
    let mut bad = good.clone();
    let edge = 0x30 + 16 + 4;
    let data = u32::from_le_bytes(good[edge..edge + 4].try_into().unwrap());
    let patched = (data & 0x8000_FFFF) | (3 << 16);
    bad[edge..edge + 4].copy_from_slice(&patched.to_le_bytes());
    assert_eq!(
        CompactSurface::parse(&bad),
        Err(CompactError::OppositeOutOfRange {
            ledge: 0x30,
            triangle: 0,
            edge: 0
        })
    );
}

#[test]
fn inner_node_revisit_is_an_error() {
    // Root inner node whose left son is the terminal node after it and whose
    // right son is the root itself.
    let mut blob = tetra_surface();
    let terminal = blob.len() - 0x1c;
    let ledge_back = i32::from_le_bytes(blob[terminal + 4..terminal + 8].try_into().unwrap());
    // Move the terminal node one slot later and put an inner root before it.
    let terminal_bytes = blob[terminal..].to_vec();
    blob.truncate(terminal);
    let root = blob.len();
    put(&mut blob, &[0, 0, 0, 0, 0, 0]);
    blob.extend_from_slice(&[0; 4]);
    blob.extend_from_slice(&terminal_bytes);
    let moved = root + 0x1c;
    blob[moved + 4..moved + 8].copy_from_slice(&(ledge_back - 0x1c).to_le_bytes());
    blob[0x20..0x24].copy_from_slice(&(root as i32).to_le_bytes());
    // Right son = the left son.
    blob[root..root + 4].copy_from_slice(&0x1ci32.to_le_bytes());
    assert_eq!(
        CompactSurface::parse(&blob),
        Err(CompactError::NodeRevisited {
            offset: moved as u32
        })
    );
    // Right son = the terminal node's own slot one further: out of range.
    blob[root..root + 4].copy_from_slice(&0x38i32.to_le_bytes());
    assert!(matches!(
        CompactSurface::parse(&blob),
        Err(CompactError::NodeOutOfRange { .. })
    ));
}

/// Deterministic mutations of a valid file: neither the loader nor the
/// decoder may panic.
#[test]
fn mutations_never_panic() {
    let base = sample_file();
    let mut state = 0x2545_F491_4F6C_DD1Du64;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    for round in 0..4000 {
        let mut bytes = base.clone();
        let edits = 1 + (next() % 4) as usize;
        for _ in 0..edits {
            let at = (next() as usize) % bytes.len();
            match next() % 3 {
                0 => bytes[at] ^= 1 << (next() % 8),
                1 => bytes[at] = (next() & 0xFF) as u8,
                _ => bytes[at] = [0x00, 0x7F, 0x80, 0xFF][(next() % 4) as usize],
            }
        }
        if round % 7 == 0 {
            let cut = (next() as usize) % bytes.len();
            bytes.truncate(cut);
        }
        let load = load(&bytes);
        for part in load.parts.values() {
            if let Some(Ok(surface)) = part.compact_surface() {
                let _ = surface.terminal_ledges();
                let _ = surface.single_convex();
            }
        }
    }
    // The blob alone, every byte flipped once.
    let blob = tetra_surface();
    for at in 0..blob.len() {
        for bit in 0..8 {
            let mut bytes = blob.clone();
            bytes[at] ^= 1 << bit;
            if let Ok(surface) = CompactSurface::parse(&bytes) {
                let _ = surface.terminal_ledges();
            }
        }
    }
}
