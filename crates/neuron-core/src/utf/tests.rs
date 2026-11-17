use super::*;

/// A tree to build test archives from.
pub(crate) enum Item {
    File(&'static [u8], Vec<u8>),
    Dir(&'static [u8], Vec<Item>),
}

use Item::{Dir, File};

struct Built {
    tree: Vec<u8>,
    strings: Vec<u8>,
    data: Vec<u8>,
}

fn add_name(strings: &mut Vec<u8>, name: &[u8], compact: bool) -> u32 {
    let offset = strings.len() as u32;
    if compact {
        // Compact names sit 4 bytes after their offset.
        strings.extend_from_slice(&[0xAA; 4]);
    }
    strings.extend_from_slice(name);
    strings.push(0);
    offset
}

fn header(node_size: u32, tree: u32, strings: u32, data: u32) -> Vec<u8> {
    let mut h = vec![0u8; HEADER_SIZE];
    for (at, value) in [
        (0x00, MAGIC),
        (0x04, VERSION),
        (0x08, tree),
        (0x14, node_size),
        (0x18, strings),
        (0x24, data),
    ] {
        h[at..at + 4].copy_from_slice(&u32::to_le_bytes(value));
    }
    h
}

fn put(buf: &mut [u8], at: usize, value: u32) {
    buf[at..at + 4].copy_from_slice(&value.to_le_bytes());
}

/// A full (0x2C) archive whose root directory holds `items`.
pub(crate) fn build_full(items: &[Item]) -> Vec<u8> {
    fn place(items: &[Item], b: &mut Built) -> u32 {
        if items.is_empty() {
            return 0;
        }
        let first = b.tree.len();
        b.tree.resize(first + items.len() * 0x2C, 0);
        for (i, item) in items.iter().enumerate() {
            let at = first + i * 0x2C;
            let next = if i + 1 < items.len() {
                (at + 0x2C) as u32
            } else {
                0
            };
            put(&mut b.tree, at, next);
            put(&mut b.tree, at + 0x20, 0x0021_6C21);
            put(&mut b.tree, at + 0x24, 0x0000_0000);
            put(&mut b.tree, at + 0x28, 0x6000_2C21);
            match item {
                File(name, data) => {
                    let name = add_name(&mut b.strings, name, false);
                    put(&mut b.tree, at + 4, name);
                    put(&mut b.tree, at + 8, 0x80);
                    put(&mut b.tree, at + 0x10, b.data.len() as u32);
                    put(&mut b.tree, at + 0x1C, data.len() as u32);
                    b.data.extend_from_slice(data);
                }
                Dir(name, children) => {
                    let name = add_name(&mut b.strings, name, false);
                    put(&mut b.tree, at + 4, name);
                    put(&mut b.tree, at + 8, 0x10);
                    let child = place(children, b);
                    put(&mut b.tree, at + 0x10, child);
                }
            }
        }
        first as u32
    }
    let mut b = Built {
        tree: vec![0u8; 0x2C],
        strings: Vec::new(),
        data: Vec::new(),
    };
    let root_name = add_name(&mut b.strings, b"\\", false);
    put(&mut b.tree, 4, root_name);
    put(&mut b.tree, 8, 0x10);
    let first = place(items, &mut b);
    put(&mut b.tree, 0x10, first);
    assemble(0x2C, b)
}

/// A compact (0x0C) archive whose root directory holds `items`.
pub(crate) fn build_compact(items: &[Item]) -> Vec<u8> {
    fn place(items: &[Item], b: &mut Built) -> u32 {
        let first = b.tree.len();
        b.tree.resize(first + items.len() * 0x0C, 0);
        for (i, item) in items.iter().enumerate() {
            let at = first + i * 0x0C;
            match item {
                File(name, data) => {
                    let name = add_name(&mut b.strings, name, true);
                    put(&mut b.tree, at, name);
                    put(&mut b.tree, at + 4, b.data.len() as u32);
                    put(&mut b.tree, at + 8, data.len() as u32);
                    b.data.extend_from_slice(data);
                }
                Dir(name, children) => {
                    let name = add_name(&mut b.strings, name, true);
                    put(&mut b.tree, at, name | 0x8000_0000);
                    let child = if children.is_empty() {
                        0
                    } else {
                        place(children, b)
                    };
                    put(&mut b.tree, at + 4, child);
                    put(&mut b.tree, at + 8, children.len() as u32);
                }
            }
        }
        first as u32
    }
    let mut b = Built {
        tree: vec![0u8; 0x0C],
        strings: Vec::new(),
        data: Vec::new(),
    };
    let root_name = add_name(&mut b.strings, b"\\", true);
    put(&mut b.tree, 0, root_name | 0x8000_0000);
    let first = place(items, &mut b);
    put(&mut b.tree, 4, first);
    put(&mut b.tree, 8, items.len() as u32);
    assemble(0x0C, b)
}

fn assemble(node_size: u32, b: Built) -> Vec<u8> {
    let tree = HEADER_SIZE as u32;
    let strings = tree + b.tree.len() as u32;
    let data = strings + b.strings.len() as u32;
    let mut out = header(node_size, tree, strings, data);
    out.extend_from_slice(&b.tree);
    out.extend_from_slice(&b.strings);
    out.extend_from_slice(&b.data);
    out
}

pub(crate) fn sample_items() -> Vec<Item> {
    vec![
        Dir(
            b"Mesh",
            vec![
                Dir(
                    b"Geometry",
                    vec![
                        File(b"VMeshData", b"vertices".to_vec()),
                        File(b"readme.txt", b"hello".to_vec()),
                    ],
                ),
                File(b"Sphere", vec![1, 2, 3]),
            ],
        ),
        File(b"dup", b"first".to_vec()),
        File(b"DUP", b"second".to_vec()),
        Dir(b"Empty", vec![]),
        File(b"\xc9t\xe9.bin", vec![0xFF]),
        File(b"zero", Vec::new()),
    ]
}

fn both() -> [UtfArchive; 2] {
    let items = sample_items();
    [
        UtfArchive::parse(build_full(&items)).unwrap(),
        UtfArchive::parse(build_compact(&items)).unwrap(),
    ]
}

fn names(archive: &UtfArchive, nodes: &[NodeRef]) -> Vec<Vec<u8>> {
    nodes
        .iter()
        .map(|&n| archive.name(n).unwrap().to_vec())
        .collect()
}

#[test]
fn header_checks() {
    let good = build_full(&sample_items());
    assert!(UtfArchive::parse(good.clone()).is_ok());
    assert_eq!(
        UtfArchive::parse(good[..0x37].to_vec()).unwrap_err(),
        UtfError::TooShort
    );
    assert_eq!(
        UtfArchive::parse(good[..0x38].to_vec()).unwrap_err(),
        UtfError::TooShort
    );
    let mut bad = good.clone();
    bad[0] = b'X';
    assert_eq!(UtfArchive::parse(bad).unwrap_err(), UtfError::BadMagic);
    let mut bad = good.clone();
    put(&mut bad, 4, 0x100);
    assert_eq!(UtfArchive::parse(bad).unwrap_err(), UtfError::BadVersion);
    let mut bad = good.clone();
    put(&mut bad, 0x14, 0x10);
    assert_eq!(
        UtfArchive::parse(bad).unwrap_err(),
        UtfError::BadNodeSize(0x10)
    );
    let mut bad = good.clone();
    put(&mut bad, 8, good.len() as u32);
    assert_eq!(
        UtfArchive::parse(bad).unwrap_err(),
        UtfError::TreeOutOfRange
    );
    let mut bad = good.clone();
    put(&mut bad, 0x18, u32::MAX);
    assert_eq!(
        UtfArchive::parse(bad).unwrap_err(),
        UtfError::StringsOutOfRange
    );
    // The data offset is never checked.
    let mut odd = good;
    put(&mut odd, 0x24, u32::MAX);
    assert!(UtfArchive::parse(odd).is_ok());
}

#[test]
fn walk_rules() {
    for a in both() {
        let root = a.root();
        let data = a.walk(root, b"mesh\\GEOMETRY/vmeshdata").unwrap().unwrap();
        assert_eq!(a.data(data).unwrap(), b"vertices");
        assert_eq!(a.walk(root, b"\\").unwrap(), Some(root));
        assert_eq!(a.walk(root, b"").unwrap(), Some(root));
        assert_eq!(
            a.walk(root, b"/Mesh").unwrap(),
            a.walk(root, b"Mesh").unwrap()
        );
        // Only one leading separator is skipped.
        assert_eq!(a.walk(root, b"\\\\Mesh").unwrap(), None);
        // A trailing separator stops at whatever was found, even a file.
        let sphere = a.walk(root, b"Mesh\\Sphere").unwrap().unwrap();
        assert_eq!(a.walk(root, b"Mesh\\Sphere\\").unwrap(), Some(sphere));
        // Walking through a file fails.
        assert_eq!(a.walk(root, b"Mesh\\Sphere\\x").unwrap(), None);
        // The first of two names that differ in case wins.
        let dup = a.walk(root, b"DUP").unwrap().unwrap();
        assert_eq!(a.data(dup).unwrap(), b"first");
        // ".." has no meaning to the walker.
        assert_eq!(a.walk(root, b"Mesh\\..").unwrap(), None);
        // A NUL ends the path.
        assert_eq!(
            a.walk(root, b"Mesh\0junk").unwrap(),
            a.walk(root, b"Mesh").unwrap()
        );
    }
}

#[test]
fn long_components_are_cut_before_a_separator() {
    let long: &'static [u8] = Box::leak(vec![b'n'; COMPONENT_LIMIT].into_boxed_slice());
    let items = vec![Dir(long, vec![File(b"f", b"x".to_vec())])];
    let a = UtfArchive::parse(build_full(&items)).unwrap();
    let mut path = vec![b'n'; COMPONENT_LIMIT + 20];
    path.extend_from_slice(b"\\f");
    assert!(a.walk(a.root(), &path).unwrap().is_some());
    // A last component is compared whole.
    assert!(
        a.walk(a.root(), &vec![b'n'; COMPONENT_LIMIT + 1])
            .unwrap()
            .is_none()
    );
}

#[test]
fn find_masks() {
    for a in both() {
        let root = a.root();
        let all = a.find(root, b"*").unwrap().unwrap();
        assert_eq!(
            names(&a, &all),
            [
                &b"Mesh"[..],
                b"dup",
                b"DUP",
                b"Empty",
                b"\xc9t\xe9.bin",
                b"zero"
            ]
        );
        assert_eq!(a.find(root, b"*.*").unwrap().unwrap().len(), 6);
        let bins = a.find(root, b"*.BIN").unwrap().unwrap();
        assert_eq!(names(&a, &bins), [&b"\xc9t\xe9.bin"[..]]);
        let geo = a.find(root, b"\\Mesh\\Geometry\\*.txt").unwrap().unwrap();
        assert_eq!(names(&a, &geo), [&b"readme.txt"[..]]);
        assert_eq!(a.find(root, b"Missing\\*").unwrap(), None);
        assert_eq!(a.find(root, b"zero\\*").unwrap(), None);
        assert_eq!(a.find(root, b"nothing*").unwrap(), Some(Vec::new()));
        let sub = a.walk(root, b"Mesh").unwrap().unwrap();
        assert_eq!(
            names(&a, &a.find(sub, b"s*").unwrap().unwrap()),
            [&b"Sphere"[..]]
        );
    }
}

#[test]
fn empty_directories() {
    let [full, compact] = both();
    assert_eq!(
        full.find(full.root(), b"Empty\\*").unwrap(),
        Some(Vec::new())
    );
    assert_eq!(
        compact.find(compact.root(), b"Empty\\*").unwrap(),
        Some(Vec::new())
    );
}

#[test]
fn compact_count_zero_still_yields_the_first_record() {
    let mut bytes = build_compact(&sample_items());
    // Point "Empty" (the fourth root child, record 4 counting the root) at the
    // records of "Mesh", count 0.
    let tree = HEADER_SIZE;
    let empty = tree + 4 * 0x0C;
    let mesh_children =
        u32::from_le_bytes(bytes[tree + 0x0C + 4..tree + 0x0C + 8].try_into().unwrap());
    put(&mut bytes, empty + 4, mesh_children);
    let a = UtfArchive::parse(bytes).unwrap();
    let found = a.find(a.root(), b"Empty\\*").unwrap().unwrap();
    assert_eq!(names(&a, &found), [&b"Geometry"[..]]);
    // FindChild sees no children.
    let dir = a.walk(a.root(), b"Empty").unwrap().unwrap();
    assert_eq!(a.children(dir).count(), 0);
}

#[test]
fn data_is_cut_at_the_archive_end() {
    let mut bytes = build_full(&[File(b"big", b"0123456789".to_vec())]);
    let a = UtfArchive::parse(bytes.clone()).unwrap();
    let big = a.walk(a.root(), b"big").unwrap().unwrap();
    let size_at = HEADER_SIZE + big.offset() as usize + 0x1C;
    put(&mut bytes, size_at, 1000);
    let a = UtfArchive::parse(bytes.clone()).unwrap();
    assert_eq!(a.data(big).unwrap(), b"0123456789");
    let past = bytes.len() as u32 + 1;
    put(&mut bytes, 0x24, past);
    let a = UtfArchive::parse(bytes).unwrap();
    assert_eq!(a.data(big).unwrap_err(), UtfError::DataOutOfRange);
}

#[test]
fn sibling_cycles_are_reported() {
    let mut bytes = build_full(&[File(b"a", vec![]), File(b"b", vec![])]);
    let a = UtfArchive::parse(bytes.clone()).unwrap();
    let b_node = a.walk(a.root(), b"b").unwrap().unwrap();
    let a_node = a.walk(a.root(), b"a").unwrap().unwrap();
    put(
        &mut bytes,
        HEADER_SIZE + b_node.offset() as usize,
        a_node.offset(),
    );
    let a = UtfArchive::parse(bytes).unwrap();
    // A name found before the loop closes is still found.
    assert_eq!(a.find_child(a.root(), b"b").unwrap(), Some(b_node));
    assert_eq!(a.find_child(a.root(), b"c").unwrap_err(), UtfError::Cycle);
    assert_eq!(a.find(a.root(), b"*").unwrap_err(), UtfError::Cycle);
}

#[test]
fn records_outside_the_archive_are_errors() {
    let mut bytes = build_full(&[File(b"a", vec![])]);
    put(&mut bytes, HEADER_SIZE + 0x10, 0x00FF_FFF0);
    let a = UtfArchive::parse(bytes).unwrap();
    assert!(matches!(
        a.find_child(a.root(), b"a"),
        Err(UtfError::RecordOutOfRange { .. })
    ));
    let mut bytes = build_compact(&[File(b"a", vec![])]);
    put(&mut bytes, HEADER_SIZE + 0x0C, 0x7FFF_0000);
    let a = UtfArchive::parse(bytes).unwrap();
    assert!(matches!(
        a.find_child(a.root(), b"a"),
        Err(UtfError::NameOutOfRange { .. })
    ));
}

#[test]
fn records_decode() {
    let [full, compact] = both();
    let node = full.walk(full.root(), b"Mesh\\Sphere").unwrap().unwrap();
    let r = full.record(node).unwrap();
    assert_eq!(r.name, b"Sphere");
    assert!(!r.is_dir);
    assert_eq!(r.size, 3);
    assert_eq!(r.attributes, 0x80);
    assert_eq!(r.times, Some([0x0021_6C21, 0, 0x6000_2C21]));
    let node = compact.walk(compact.root(), b"Mesh").unwrap().unwrap();
    let r = compact.record(node).unwrap();
    assert!(r.is_dir);
    assert_eq!(r.attributes, ATTRIBUTE_DIRECTORY);
    assert_eq!(r.size, 2);
    assert_eq!(r.times, None);
}

/// xorshift32: a small deterministic generator for the mutation test.
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
    let Ok(a) = UtfArchive::parse(bytes) else {
        return;
    };
    let root = a.root();
    for path in [
        &b""[..],
        b"Mesh",
        b"mesh\\geometry\\vmeshdata",
        b"Mesh\\Sphere",
        b"dup",
        b"Empty\\x",
        b"\\zero",
    ] {
        if let Ok(Some(node)) = a.walk(root, path) {
            let _ = a.record(node);
            let _ = a.data(node);
            let _ = a.find(node, b"*");
            for child in a.children(node).take(64) {
                let _ = child.map(|c| a.record(c));
            }
        }
    }
    for pattern in [
        &b"*"[..],
        b"*.txt",
        b"Mesh\\*",
        b"Mesh\\Geometry\\v*",
        b"?e*",
    ] {
        if let Ok(Some(nodes)) = a.find(root, pattern) {
            for node in nodes.into_iter().take(64) {
                let _ = a.record(node);
                let _ = a.data(node);
            }
        }
    }
}

#[test]
fn mutated_archives_never_panic() {
    let items = sample_items();
    let seeds = [build_full(&items), build_compact(&items)];
    let mut rng = Mutator(0x1234_5678);
    for round in 0..4000 {
        let mut bytes = seeds[round % 2].clone();
        for _ in 0..=rng.below(8) {
            match rng.below(4) {
                0 => {
                    let at = rng.below(bytes.len());
                    bytes[at] = rng.next() as u8;
                }
                1 => {
                    // A random dword: offsets, sizes and counts.
                    let at = rng.below(bytes.len().saturating_sub(4));
                    let value = match rng.below(4) {
                        0 => rng.next(),
                        1 => rng.next() % 0x200,
                        2 => u32::MAX - rng.next() % 16,
                        _ => 0,
                    };
                    put(&mut bytes, at, value);
                }
                2 => {
                    let keep = rng.below(bytes.len() + 1);
                    bytes.truncate(keep.max(HEADER_SIZE + 1));
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
