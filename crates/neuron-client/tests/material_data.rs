//! The material map and batching rules of the shipped engine INI
//! (`outdir/NeuronComponents.ini`, decision O15, sections `[MaterialMap]`,
//! `[BatchedMaterials]` and `[MaterialBatcher]`) through the material
//! batcher of work package P4.5. Nothing is compared with the oracle (the
//! synthetic suites of `oracle_vectors.rs` do that for the parser, the
//! regular expressions and the rules); the test checks that every line of
//! the game's map becomes a rule whose pattern compiles, and that names and
//! types the game uses resolve as the comments in the INI say they should.

use std::fs;
use std::path::Path;

use neuron_client::render::material::{Material, MaterialBatcher, MaterialFactory};
use neuron_core::ini::{EngineIni, NoIncludes};

/// Materials are never made here.
struct NoFactory;

impl MaterialFactory<()> for NoFactory {
    fn create(&mut self, _: &mut (), _: &[u8]) -> Result<Box<dyn Material<()>>, i32> {
        Err(-3)
    }
}

#[test]
fn the_shipped_material_map_compiles_and_resolves() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/NeuronComponents.ini");
    let Ok(bytes) = fs::read(&path) else {
        println!("{} is missing: nothing to read", path.display());
        return;
    };
    let mut ini = EngineIni::new();
    ini.load_buffer(&bytes, &mut NoIncludes)
        .expect("the engine INI parses");
    let section = ini.section(b"MaterialMap").expect("[MaterialMap]");
    let mapped = section.entries.iter().filter(|e| e.value.is_some()).count();
    let batcher = MaterialBatcher::new(Box::new(NoFactory), Some(&ini));
    let library = batcher.library();
    assert_eq!(library.rules().len(), mapped, "one rule per map line");
    for rule in library.rules() {
        assert!(
            rule.expression.is_some(),
            "{} does not compile",
            String::from_utf8_lossy(&rule.pattern)
        );
    }
    assert!(!batcher.rules().is_empty());
    for rule in batcher.rules() {
        assert!(rule.expression.is_some());
    }
    let remap = |name: &str, ty: &str| {
        library
            .remap_type(name.as_bytes(), ty.as_bytes())
            .map(|t| String::from_utf8_lossy(&t).into_owned())
            .unwrap_or_default()
    };
    // "exporter generates EcEtOcOt but there is no implementation of it".
    assert_eq!(remap("hull", "EcEtOcOt"), "DcDtOcOt");
    assert_eq!(remap("hull", "DcDtEcEt"), "DcDtEt");
    // "materials that would otherwise get mapped to translucent textures".
    assert_eq!(remap("alpha_mask_fin", "DcDtOcOt"), "DcDt");
    assert_eq!(remap("alpha_mask_fin2side", "DcDtOcOt"), "DcDtTwo");
    assert_eq!(remap("null", "DcDt"), "NullMaterial");
    assert_eq!(remap("plain_hull", "DcDt"), "");
    // The batching rule wraps translucent types.
    let batched = batcher.rules().iter().any(|r| {
        r.kind == 1
            && r.expression
                .as_ref()
                .is_some_and(|e| e.is_match(b"DcDtOcOt"))
    });
    assert!(batched, "[BatchedMaterials] batches DcDtOcOt");
}

/// The UTF archives under `dir`, relative to `root`.
fn archives(root: &Path, dir: &Path, out: &mut Vec<std::path::PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            archives(root, &path, out);
        } else if fs::File::open(&path)
            .and_then(|mut f| {
                let mut magic = [0u8; 4];
                std::io::Read::read_exact(&mut f, &mut magic).map(|()| magic)
            })
            .is_ok_and(|m| &m == b"UTF ")
        {
            out.push(path.strip_prefix(root).unwrap_or(&path).to_path_buf());
        }
    }
}

/// Every `Material library` of the shipped archives (`outdir/data`,
/// decision O15) through the batcher's library with the shipped
/// `[MaterialMap]` and `[BatchedMaterials]`, `Shading`'s factories and the
/// device model, after the archive's own textures: each child whose type
/// (remapped) is one of the 22 kinds or a batched one is created and
/// loaded, verified and resolved; the types `Shading` does not make (the
/// `surfacematerials` kinds, next) are counted. Deleting the groups frees
/// every material and texture playback.
#[test]
fn shipped_material_libraries_create_through_the_factory() {
    use std::collections::BTreeMap;

    use neuron_client::render::ff::defaults::Defaults;
    use neuron_client::render::ff::device::{Device, RecordingBackend};
    use neuron_client::render::material::{Shading, ShadingFactory};
    use neuron_client::render::texture::{FormatClasses, LibraryConfig, TextureLibrary};
    use neuron_core::hash::name_crc32;
    use neuron_core::material_data::{MaterialEntry, MaterialKind, load_material_library};
    use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

    let base = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let root = base.join("data");
    let mut found = Vec::new();
    archives(&root, &root, &mut found);
    let Ok(ini_bytes) = fs::read(base.join("NeuronComponents.ini")) else {
        println!("outdir/NeuronComponents.ini is missing: nothing to create");
        return;
    };
    if found.is_empty() {
        println!("outdir/data has no UTF archives: nothing to create");
        return;
    }
    found.sort();
    let mut ini = EngineIni::new();
    ini.load_buffer(&ini_bytes, &mut NoIncludes)
        .expect("the engine INI parses");
    let device = Device::new(Defaults::builtin(), 640, 480, RecordingBackend::default());
    let mut services = Shading::new(
        device,
        TextureLibrary::new(LibraryConfig::default()),
        FormatClasses::builtin(),
    );
    let mut batcher: MaterialBatcher<Shading<RecordingBackend>> =
        MaterialBatcher::new(Box::new(ShadingFactory::new()), Some(&ini));
    let host = Host::new(&root);
    let (mut libraries, mut children, mut expected, mut missing) = (0, 0, 0, 0);
    let mut others: BTreeMap<String, u32> = BTreeMap::new();
    let mut kinds: BTreeMap<String, u32> = BTreeMap::new();
    let mut groups = Vec::new();
    let mut texture_groups = Vec::new();
    for path in &found {
        let name = path.to_string_lossy().replace('\\', "/");
        let open = || {
            UtfFileSystem::open(
                &host,
                &FsDesc::read(name.as_bytes()).with_implementation(b"UTF"),
            )
        };
        let Ok(mut fs) = open() else {
            continue;
        };
        if fs.open_node(b"Material library").is_err() {
            continue;
        }
        let _ = fs.open_node(b"..");
        libraries += 1;
        if fs.open_node(b"Texture library").is_ok() {
            let _ = fs.open_node(b"..");
            let (textures, mut pipeline) = services.texture_parts();
            let load = textures.load_group(&mut fs, &mut pipeline, true);
            texture_groups.extend(load.group);
        }
        // What the library should make: the parsed children, by remapped type.
        let mut parsed = open().expect("opened before");
        let nodes = load_material_library(&mut parsed).unwrap_or_default();
        let load = batcher
            .library_mut()
            .load_library(&mut services, &mut fs, true);
        assert_eq!(load.result, 0, "{name}");
        let group = load.group.expect("a group");
        let members = batcher
            .library()
            .groups()
            .get(&group)
            .map(|g| g.members.clone())
            .unwrap_or_default();
        for node in nodes {
            let MaterialEntry::Typed { type_name, .. } = &node.entry else {
                continue;
            };
            children += 1;
            let lower = node.name.to_ascii_lowercase();
            let selected = batcher
                .library()
                .remap_type(&lower, type_name)
                .unwrap_or_else(|_| type_name.clone());
            let selected = String::from_utf8_lossy(&selected).into_owned();
            if MaterialKind::by_name(selected.as_bytes()).is_some() {
                expected += 1;
                *kinds.entry(selected.clone()).or_default() += 1;
                if !members.contains(&name_crc32(&node.name)) {
                    missing += 1;
                    println!(
                        "{name}: {} ({selected}) was not created",
                        String::from_utf8_lossy(&node.name)
                    );
                }
            } else {
                *others.entry(selected).or_default() += 1;
            }
        }
        batcher
            .library_mut()
            .apply_verify_group(&mut services, group.cast_signed());
        batcher
            .library_mut()
            .resolve_textures_group(&mut services, group.cast_signed(), 0.0);
        groups.push(group);
    }
    let live = batcher.library().materials().live().count();
    let batched = batcher
        .library()
        .materials()
        .live()
        .filter(|&id| {
            batcher
                .library()
                .materials()
                .get(id)
                .is_some_and(|m| m.is_batched())
        })
        .count();
    println!(
        "{libraries} material libraries: {children} typed children, {expected} of Shading's kinds \
         ({live} materials live, {batched} batched); kinds {kinds:?}; other types {others:?}"
    );
    assert!(libraries > 0);
    assert_eq!(missing, 0, "materials of Shading's kinds not created");
    for group in groups {
        batcher
            .library_mut()
            .delete_group(&mut services, group.cast_signed());
    }
    assert_eq!(
        batcher.library().materials().live().count(),
        0,
        "materials left after their groups were deleted"
    );
    let (textures, mut pipeline) = services.texture_parts();
    for group in texture_groups {
        textures.garbage_collect(group, &mut pipeline);
    }
    assert_eq!(
        services.textures.playbacks().count(),
        0,
        "texture playbacks left after the materials were destroyed"
    );
}
