//! The shipped texture libraries (`outdir/data`, decision O15) through the
//! texture runtime of work package P4.4: every UTF archive under the data
//! tree with a `Texture library` node is loaded by
//! [`TextureLibrary::load_group`] into the device model, as the client
//! loads it, then its group is collected again. Nothing is written or
//! compared with the oracle (`neuron-core`'s `data_dump_texture` does that
//! for the loading rules); the test checks that every texture the loader
//! makes reaches the device in a format the device holds, that every frame
//! of a loaded record names a device texture, and that collecting the
//! groups leaves no texture behind.

use std::fs;
use std::path::{Path, PathBuf};

use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::ff::device::{Device, RecordingBackend, ResourceRecord};
use neuron_client::render::texture::{
    Abilities, DevicePipeline, FormatClasses, HandleTable, LibraryConfig, TextureLibrary,
};
use neuron_core::image::LoadOutcome;
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

fn files(root: &Path, dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            files(root, &path, out);
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

#[test]
fn shipped_texture_libraries_load_into_the_device() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut archives = Vec::new();
    files(&root, &root, &mut archives);
    if archives.is_empty() {
        println!("outdir/data has no UTF archives: nothing to load");
        return;
    }
    archives.sort();
    let host = Host::new(&root);
    let mut device = Device::new(Defaults::builtin(), 640, 480, RecordingBackend::default());
    let mut handles = HandleTable::new();
    let classes = FormatClasses::builtin();
    let mut library = TextureLibrary::new(LibraryConfig::default());
    let (mut archives_loaded, mut entries, mut loaded, mut failed, mut faults) = (0, 0, 0, 0, 0);
    let mut groups = Vec::new();
    for path in &archives {
        let name = path.to_string_lossy().replace('\\', "/");
        let Ok(mut fs) = UtfFileSystem::open(
            &host,
            &FsDesc::read(name.as_bytes()).with_implementation(b"UTF"),
        ) else {
            continue;
        };
        if fs.open_node(b"Texture library").is_err() {
            continue;
        }
        let _ = fs.open_node(b"..");
        let mut pipeline = DevicePipeline {
            device: &mut device,
            handles: &mut handles,
            classes: &classes,
            abilities: Abilities::default(),
        };
        let load = library.load_group(&mut fs, &mut pipeline, true);
        assert_eq!(load.result, 0, "{name}");
        archives_loaded += 1;
        for entry in &load.entries {
            entries += 1;
            match entry.outcome {
                LoadOutcome::Done {
                    result,
                    record: Some(record),
                } if result >= 0 => {
                    loaded += 1;
                    let r = library.record(record).expect("a record");
                    for (frame, _) in r.frames.iter().enumerate() {
                        let (code, out) = library.texture_frame(Some(record), frame as i32);
                        assert_eq!(code, 0, "{name}: frame {frame}");
                        assert!(
                            handles.get(out.handle).is_some(),
                            "{name}: {} frame {frame} has no device texture",
                            String::from_utf8_lossy(&entry.name)
                        );
                    }
                }
                LoadOutcome::Done { result, .. } => {
                    println!(
                        "{name}: {} failed ({result})",
                        String::from_utf8_lossy(&entry.name)
                    );
                    failed += 1;
                }
                LoadOutcome::Fault => faults += 1,
            }
        }
        groups.extend(load.group);
    }
    device.sync_resources();
    let live = handles.len();
    let uploads = device
        .backend
        .resources
        .iter()
        .filter(|r| matches!(r, ResourceRecord::TextureLevel { .. }))
        .count();
    println!(
        "{archives_loaded} texture libraries: {entries} entries, {loaded} loaded, {failed} failed, \
         {faults} faults; {live} device textures, {uploads} level events"
    );
    assert!(archives_loaded > 0);
    assert_eq!((failed, faults), (0, 0));
    assert!(live > 0);
    let mut pipeline = DevicePipeline {
        device: &mut device,
        handles: &mut handles,
        classes: &classes,
        abilities: Abilities::default(),
    };
    for group in groups {
        library.garbage_collect(group, &mut pipeline);
    }
    assert_eq!(
        handles.len(),
        0,
        "textures left after the groups were collected"
    );
    assert_eq!(library.loader().canonical_len(), 0);
}
