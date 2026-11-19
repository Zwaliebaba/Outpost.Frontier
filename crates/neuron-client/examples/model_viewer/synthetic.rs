//! The synthetic scene: a model made here, so that the viewer's path runs
//! without the original data (CI, decision O15). It is a UTF archive in
//! the layout the engine loads from a `.3db` file: a `VMeshLibrary` with
//! one mesh (a cube with face normals), a `Material library` with one
//! `DcDt` material, and a `VMeshPart` that draws the whole mesh.

use neuron_core::hash::name_crc32;

/// The name the archive is made under.
pub const MODEL: &[u8] = b"VIEWER\\viewer_cube.3db";
/// The mesh's name in the library.
const MESH: &str = "viewer_cube.lod0.vms";
/// The material's name.
const MATERIAL: &str = "viewer_cube_material";
/// The material's diffuse colour (and so its ambient one).
const COLOUR: [f32; 3] = [1.0, 0.55, 0.15];

/// `D3DFVF_XYZ | D3DFVF_NORMAL`.
const FVF: u16 = 0x12;
/// `D3DPT_TRIANGLELIST`.
const TRIANGLE_LIST: u32 = 4;

/// One node of the archive.
enum Node {
    File(&'static str, Vec<u8>),
    Dir(&'static str, Vec<Node>),
}

const MAGIC: u32 = 0x2046_5455;
const VERSION: u32 = 0x101;
const HEADER: usize = 0x38;
const ENTRY: usize = 0x2C;
const TIMES: [u32; 3] = [0x2C21_0021, 0, 0x2C21_6000];

fn put(buf: &mut [u8], at: usize, value: u32) {
    buf[at..at + 4].copy_from_slice(&value.to_le_bytes());
}

fn offset(n: usize) -> u32 {
    u32::try_from(n).expect("a small archive")
}

/// The bytes of a UTF archive whose root directory holds `children`.
fn archive(children: &[Node]) -> Vec<u8> {
    struct Writer {
        tree: Vec<u8>,
        strings: Vec<u8>,
        data: Vec<u8>,
    }
    impl Writer {
        fn name(&mut self, name: &str) -> u32 {
            let at = offset(self.strings.len());
            self.strings.extend_from_slice(name.as_bytes());
            self.strings.push(0);
            at
        }
        fn times(&mut self, at: usize) {
            for (i, t) in TIMES.iter().enumerate() {
                put(&mut self.tree, at + 0x20 + 4 * i, *t);
            }
        }
        fn place(&mut self, items: &[Node]) -> u32 {
            if items.is_empty() {
                return 0;
            }
            let first = self.tree.len();
            self.tree.resize(first + ENTRY * items.len(), 0);
            for (i, item) in items.iter().enumerate() {
                let at = first + i * ENTRY;
                let next = if i + 1 < items.len() {
                    offset(at + ENTRY)
                } else {
                    0
                };
                put(&mut self.tree, at, next);
                match item {
                    Node::File(name, bytes) => {
                        let name = self.name(name);
                        put(&mut self.tree, at + 4, name);
                        put(&mut self.tree, at + 8, 0x80);
                        put(&mut self.tree, at + 0x10, offset(self.data.len()));
                        put(&mut self.tree, at + 0x1C, offset(bytes.len()));
                        self.data.extend_from_slice(bytes);
                    }
                    Node::Dir(name, children) => {
                        let name = self.name(name);
                        put(&mut self.tree, at + 4, name);
                        put(&mut self.tree, at + 8, 0x10);
                        let child = self.place(children);
                        put(&mut self.tree, at + 0x10, child);
                    }
                }
                self.times(at);
            }
            offset(first)
        }
    }
    let mut w = Writer {
        tree: vec![0; ENTRY],
        strings: Vec::new(),
        data: Vec::new(),
    };
    let root = w.name("\\");
    put(&mut w.tree, 4, root);
    put(&mut w.tree, 8, 0x10);
    w.times(0);
    let first = w.place(children);
    put(&mut w.tree, 0x10, first);
    let t = HEADER;
    let s = t + w.tree.len();
    let d = s + w.strings.len();
    let mut out = vec![0u8; HEADER];
    for (at, value) in [
        (0, MAGIC),
        (4, VERSION),
        (8, offset(t)),
        (0x14, offset(ENTRY)),
        (0x18, offset(s)),
        (0x24, offset(d)),
    ] {
        put(&mut out, at, value);
    }
    out.extend_from_slice(&w.tree);
    out.extend_from_slice(&w.strings);
    out.extend_from_slice(&w.data);
    out
}

fn f32s(values: &[f32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}

/// The cube's 24 vertices (position, normal) and 72 indices: each face's
/// two triangles in both windings.
fn cube() -> (Vec<[f32; 6]>, Vec<u16>) {
    let mut vertices = Vec::new();
    let mut indices = Vec::new();
    // Each face: its normal and two in-plane axes whose cross product is
    // the normal.
    let faces: [([f32; 3], [f32; 3], [f32; 3]); 6] = [
        ([1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]),
        ([-1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, 1.0, 0.0]),
        ([0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [1.0, 0.0, 0.0]),
        ([0.0, -1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]),
        ([0.0, 0.0, 1.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]),
        ([0.0, 0.0, -1.0], [0.0, 1.0, 0.0], [1.0, 0.0, 0.0]),
    ];
    for (n, u, v) in faces {
        let base = u16::try_from(vertices.len()).expect("24 vertices");
        for (a, b) in [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)] {
            let p: [f32; 3] = core::array::from_fn(|i| n[i] + a * u[i] + b * v[i]);
            vertices.push([p[0], p[1], p[2], n[0], n[1], n[2]]);
        }
        // Both windings: the cube shows whichever way the pipeline culls.
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3]);
        indices.extend([base, base + 2, base + 1, base, base + 3, base + 2]);
    }
    (vertices, indices)
}

/// The `VMeshData` leaf of the cube.
fn vmesh_data() -> Vec<u8> {
    let (vertices, indices) = cube();
    let vertex_count = u16::try_from(vertices.len()).expect("24 vertices");
    let index_count = u16::try_from(indices.len()).expect("72 indices");
    let mut out = vec![1u8, 0, 0, 0];
    out.extend_from_slice(&TRIANGLE_LIST.to_le_bytes());
    for word in [1u16, index_count, FVF, vertex_count] {
        out.extend_from_slice(&word.to_le_bytes());
    }
    out.extend_from_slice(&name_crc32(MATERIAL.as_bytes()).to_le_bytes());
    for word in [0u16, vertex_count - 1, index_count, 0xCC] {
        out.extend_from_slice(&word.to_le_bytes());
    }
    for i in &indices {
        out.extend_from_slice(&i.to_le_bytes());
    }
    for v in &vertices {
        out.extend(f32s(v));
    }
    out
}

/// The `VMeshRef` leaf: the whole mesh, its bounds and sphere.
fn vmesh_ref() -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&0x3Cu32.to_le_bytes());
    out.extend_from_slice(&name_crc32(MESH.as_bytes()).to_le_bytes());
    for word in [0u16, 24, 0, 72, 0, 1] {
        out.extend_from_slice(&word.to_le_bytes());
    }
    out.extend(f32s(&[
        1.0,
        -1.0,
        1.0,
        -1.0,
        1.0,
        -1.0,
        0.0,
        0.0,
        0.0,
        3.0f32.sqrt(),
    ]));
    out
}

/// The archive's bytes.
pub fn model_bytes() -> Vec<u8> {
    let mut type_name = b"DcDt".to_vec();
    type_name.push(0);
    archive(&[
        Node::File("Exporter Version", b"model_viewer\0".to_vec()),
        Node::Dir(
            "VMeshLibrary",
            vec![Node::Dir(MESH, vec![Node::File("VMeshData", vmesh_data())])],
        ),
        Node::Dir(
            "Material library",
            vec![Node::Dir(
                MATERIAL,
                vec![
                    Node::File("Type", type_name),
                    Node::File("Dc", f32s(&COLOUR)),
                ],
            )],
        ),
        Node::Dir("VMeshPart", vec![Node::File("VMeshRef", vmesh_ref())]),
    ])
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::mesh_data::vmesh::{VMeshData, VMeshRef};
    use neuron_core::vfs::{FileSystem, FsDesc, UtfFileSystem};

    fn leaf(fs: &mut UtfFileSystem, path: &[u8]) -> Vec<u8> {
        let mut file = fs.open_file(&FsDesc::read(path)).expect("the leaf");
        let size = file.size().expect("its size") as usize;
        let mut buf = vec![0; size];
        assert_eq!(file.read(&mut buf).expect("read"), size);
        buf
    }

    #[test]
    fn the_archive_parses_as_a_mesh_and_a_material() {
        let mut fs = UtfFileSystem::from_bytes(model_bytes(), MODEL).expect("a UTF archive");
        let data = leaf(&mut fs, b"VMeshLibrary\\viewer_cube.lod0.vms\\VMeshData");
        let mesh = VMeshData::parse(&data).expect("the mesh");
        assert_eq!(mesh.indices.len(), 72);
        assert_eq!(mesh.face_groups.len(), 1);
        let reference = VMeshRef::parse(&leaf(&mut fs, b"VMeshPart\\VMeshRef")).expect("the ref");
        assert_eq!(reference.mesh_crc, name_crc32(MESH.as_bytes()));
        assert_eq!(reference.vertex_count, 24);
        assert!((reference.radius - 3.0f32.sqrt()).abs() < 1e-6);
        let type_name = leaf(&mut fs, b"Material library\\viewer_cube_material\\Type");
        assert_eq!(type_name, b"DcDt\0");
    }
}
