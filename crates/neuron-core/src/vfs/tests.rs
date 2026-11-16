use std::fs;
use std::path::PathBuf;

use super::*;
use crate::utf::tests::{Item, build_compact, build_full, sample_items};

/// A scratch tree: `Data/Sub/File.TXT`, `Data/Empty.bin`, `Data/Mesh.utf`,
/// `Data/Small.utf` (compact), `Data/plain.cmp` (not an archive), and a
/// second root `Patch/File.txt`.
fn tree(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("neuron-core-vfs-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(dir.join("Data").join("Sub")).unwrap();
    fs::create_dir_all(dir.join("Patch")).unwrap();
    fs::write(dir.join("Data").join("Sub").join("File.TXT"), b"base").unwrap();
    fs::write(dir.join("Data").join("Empty.bin"), b"").unwrap();
    fs::write(
        dir.join("Data").join("Mesh.utf"),
        build_full(&sample_items()),
    )
    .unwrap();
    fs::write(
        dir.join("Data").join("Small.utf"),
        build_compact(&sample_items()),
    )
    .unwrap();
    fs::write(dir.join("Data").join("plain.cmp"), b"not an archive").unwrap();
    fs::write(dir.join("Patch").join("File.txt"), b"patch").unwrap();
    dir
}

fn read_all(file: &mut VfsFile) -> Vec<u8> {
    let mut buf = vec![0u8; file.size().unwrap() as usize];
    if !buf.is_empty() {
        let got = file.read(&mut buf).unwrap();
        buf.truncate(got);
    }
    buf
}

#[test]
fn directory_file_systems() {
    let dir = tree("dos");
    let host = Host::new(&dir);
    let mut root = create_file_system(&host, &FsDesc::read(b"data")).unwrap();
    assert_eq!(root.kind(), FsKind::DosDirectory);
    assert_eq!(root.file_sys_path(JOIN), b"data");
    assert_eq!(root.access(), GENERIC_READ);
    // Relative to "\", whatever the root.
    assert_eq!(root.resolve_path(b"x", JOIN), (true, b"\\x".to_vec()));

    let mut file = root.open_file(&FsDesc::read(b"SUB\\file.txt")).unwrap();
    assert_eq!(read_all(&mut file), b"base");
    let mut buf = [0u8; 8];
    file.seek(0, SeekOrigin::Begin).unwrap();
    assert_eq!(file.read(&mut buf), Err(VfsError::ShortRead { read: 4 }));
    assert_eq!(file.map_view(1, 0).unwrap(), b"ase");
    assert_eq!(file.seek(1, SeekOrigin::End), Ok(3));
    let mut empty = root.open_file(&FsDesc::read(b"empty.bin")).unwrap();
    assert_eq!(empty.read(&mut []), Ok(0));
    assert_eq!(empty.seek(0, SeekOrigin::Begin), Err(VfsError::SeekFailed));
    assert_eq!(empty.map_view(0, 0), Err(VfsError::AccessDenied));
    assert_eq!(
        root.open_file(&FsDesc::read(b"sub")).unwrap_err(),
        VfsError::FileNotFound
    );

    assert_eq!(root.path_attributes(b"sub"), Ok(ATTRIBUTE_DIRECTORY));
    assert_eq!(
        root.path_attributes(b"nothing"),
        Err(VfsError::FileNotFound)
    );
    assert_eq!(root.find(b"*"), Err(VfsError::BadPathname));
    assert_eq!(root.current_path(64), Ok(b"\\".to_vec()));

    root.open_node(b"sub").unwrap();
    assert_eq!(root.file_sys_path(JOIN), b"data");
    assert_eq!(root.resolve_path(b"x", JOIN), (true, b"\\sub\\x".to_vec()));
    let mut file = root.open_file(&FsDesc::read(b"file.txt")).unwrap();
    assert_eq!(read_all(&mut file), b"base");
    assert_eq!(root.open_node(b"file.txt"), Err(VfsError::BadPathname));
    root.open_node(b"..").unwrap();
    assert!(root.open_file(&FsDesc::read(b"empty.bin")).is_ok());

    // Children.
    let sub = root.create_child(&FsDesc::read(b"Sub")).unwrap();
    assert_eq!(sub.kind(), FsKind::DosDirectory);
    assert_eq!(sub.access(), 0);
    let mut one = root
        .create_child(&FsDesc::read(b"sub\\file.txt").with_implementation(b"DOS"))
        .unwrap();
    assert_eq!(one.kind(), FsKind::DosFile);
    assert_eq!(one.file_sys_path(JOIN), b"sub\\file.txt");
    let mut buf = [0u8; 4];
    assert_eq!(one.read(&mut buf), Ok(4));
    assert_eq!(&buf, b"base");
    assert_eq!(
        one.create_child(&FsDesc::read(b"x")).unwrap_err(),
        VfsError::Generic
    );
    let archive = root.create_child(&FsDesc::read(b"mesh.utf")).unwrap();
    assert_eq!(archive.kind(), FsKind::Utf);
    assert_eq!(archive.file_sys_path(JOIN), b"mesh.utf");
    assert_eq!(
        root.create_child(&FsDesc::read(b"plain.cmp")).unwrap_err(),
        VfsError::NotAnArchive
    );
    assert_eq!(
        root.create_child(&FsDesc::read(b"mesh.utf").with_implementation(b"dos"))
            .unwrap_err(),
        VfsError::NotAnArchive
    );
    assert_eq!(
        root.create_child(&FsDesc::read(b"missing")).unwrap_err(),
        VfsError::Generic
    );
    let mut write = FsDesc::read(b"Sub");
    write.access |= GENERIC_WRITE;
    assert_eq!(root.create_child(&write).unwrap_err(), VfsError::Generic);

    // Top level.
    let top = create_file_system(
        &host,
        &FsDesc::read(b"Data\\Mesh.utf").with_implementation(b"DOS"),
    )
    .unwrap();
    assert_eq!(top.kind(), FsKind::DosFile);
    assert_eq!(
        create_file_system(&host, &FsDesc::read(b"nothing")).unwrap_err(),
        VfsError::OpenFailed
    );
    let _ = fs::remove_dir_all(&dir);
}

const JOIN: usize = path::JOIN_CHARS;

#[test]
fn utf_file_systems() {
    let dir = tree("utf");
    let host = Host::new(&dir);
    for name in [&b"Data\\Mesh.utf"[..], b"Data\\Small.utf"] {
        let mut fs = create_file_system(&host, &FsDesc::read(name)).unwrap();
        assert_eq!(fs.kind(), FsKind::Utf);
        assert_eq!(fs.file_sys_path(JOIN), name);
        assert_eq!(fs.current_path(64), Err(VfsError::AccessDenied));
        assert_eq!(fs.path_attributes(b"mesh"), Ok(ATTRIBUTE_DIRECTORY));
        assert_eq!(fs.path_attributes(b"mesh\\sphere"), Ok(0));
        assert_eq!(fs.path_attributes(b"none"), Err(VfsError::BadPathname));
        let mut file = fs
            .open_file(&FsDesc::read(b"MESH\\geometry\\VMeshData"))
            .unwrap();
        assert_eq!(read_all(&mut file), b"vertices");
        assert_eq!(file.map_view(0, 0), Err(VfsError::AccessDenied));
        let mut empty = [0u8; 0];
        assert_eq!(file.read(&mut empty), Err(VfsError::InvalidParameter));
        let mut buf = [0u8; 4];
        assert_eq!(file.read(&mut buf), Err(VfsError::ReadFault));
        assert_eq!(file.seek(9, SeekOrigin::Begin), Err(VfsError::SeekFailed));
        assert_eq!(file.seek(3, SeekOrigin::End), Ok(5));
        assert_eq!(file.read(&mut buf), Ok(3));
        assert_eq!(&buf[..3], b"ces");
        assert_eq!(
            fs.open_file(&FsDesc::read(b"zero")).unwrap().read(&mut buf),
            Err(VfsError::ReadFault)
        );

        // OpenNode, relative find and "..".
        fs.open_node(b"Mesh").unwrap();
        let names: Vec<Vec<u8>> = fs
            .find(b"*")
            .unwrap()
            .into_iter()
            .map(|e| e.data.name)
            .collect();
        assert_eq!(names, [b"Geometry".to_vec(), b"Sphere".to_vec()]);
        assert_eq!(fs.resolve_path(b"x", JOIN), (true, b"\\Mesh\\x".to_vec()));
        let mut file = fs.open_file(&FsDesc::read(b"sphere")).unwrap();
        assert_eq!(read_all(&mut file), [1, 2, 3]);
        assert_eq!(fs.open_node(b"sphere"), Ok(()));
        assert_eq!(fs.open_node(b"nothing"), Err(VfsError::BadPathname));
        fs.open_node(b"..").unwrap();
        fs.open_node(b"..").unwrap();
        assert_eq!(fs.resolve_path(b"x", JOIN), (true, b"\\x".to_vec()));
        assert_eq!(fs.find(b"nothing*"), Ok(Vec::new()));

        // Clones by path and by find token.
        let mesh = fs.create_child(&FsDesc::read(b"mesh\\")).unwrap();
        assert_eq!(mesh.file_sys_path(JOIN), b"mesh\\");
        assert_eq!(mesh.current_path(64), Ok(b"\\".to_vec()));
        let mut geo = mesh.create_child(&FsDesc::read(b"geometry")).unwrap();
        assert_eq!(geo.file_sys_path(JOIN), b"geometry\\");
        let entries = geo.find(b"*.txt").unwrap();
        assert_eq!(entries.len(), 1);
        let mut readme = geo
            .create_child(&FsDesc::read(b"ignored").with_found(entries[0].token))
            .unwrap();
        assert_eq!(readme.file_sys_path(JOIN), b"ignored");
        assert_eq!(readme.current_path(64), Ok(Vec::new()));
        let mut buf = [0u8; 16];
        assert_eq!(readme.read(&mut buf), Ok(5));
        assert_eq!(&buf[..5], b"hello");
        assert_eq!(readme.map_view(1, 2).unwrap(), b"el");
        assert_eq!(readme.map_view(1, 5), Err(VfsError::AccessDenied));
        let file = geo.open_file(&FsDesc::read(b"readme.txt")).unwrap();
        assert_eq!(file.map_view(0, 0).unwrap(), b"hello");
        assert_eq!(readme.position(), Err(VfsError::AccessDenied));
        assert_eq!(geo.open_node(b".."), Err(VfsError::BadPathname));
        assert!(fs.create_child(&FsDesc::read(b"none")).is_err());
        assert_eq!(
            fs.create_child(&FsDesc::read(b"mesh").with_implementation(b"DOS"))
                .unwrap_err(),
            VfsError::Generic
        );
        // A token from another archive opens nothing.
        let other = create_file_system(&host, &FsDesc::read(name)).unwrap();
        assert_eq!(
            other
                .create_child(&FsDesc::read(b"x").with_found(entries[0].token))
                .unwrap_err(),
            VfsError::Generic
        );
    }
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn find_data() {
    let full = UtfFileSystem::from_bytes(build_full(&sample_items()), b"x").unwrap();
    let entries = full.find(b"mesh\\*").unwrap();
    let geometry = &entries[0].data;
    assert_eq!(geometry.attributes, 0x10);
    assert_eq!(geometry.size, 0);
    assert_eq!(
        geometry.times,
        [
            crate::utf::packed_to_file_time(0x0021_6C21),
            None,
            crate::utf::packed_to_file_time(0x6000_2C21)
        ]
    );
    assert!(geometry.times[0].is_some());
    assert_eq!(entries[1].data.attributes, 0x80);
    assert_eq!(entries[1].data.size, 3);
    let compact = UtfFileSystem::from_bytes(build_compact(&sample_items()), b"x").unwrap();
    let entries = compact.find(b"*").unwrap();
    assert_eq!(entries[0].data.size, 0);
    assert_eq!(entries[0].data.times, [None; 3]);
}

#[test]
fn memory_file_systems() {
    let mut mem = MemFileSystem::new(
        Some(b"mem\\root\\"),
        GENERIC_READ | GENERIC_WRITE,
        2,
        MemSource::Copy(None),
    );
    assert_eq!(mem.size(), Ok(0));
    assert_eq!(mem.write(b"hello"), Ok(5));
    assert_eq!(mem.buffer().len(), 0x10000);
    assert_eq!(mem.size(), Ok(5));
    assert_eq!(mem.seek(0, SeekOrigin::Begin), Ok(0));
    let mut buf = [0u8; 8];
    assert_eq!(mem.read(&mut buf), Ok(5));
    assert_eq!(&buf[..5], b"hello");
    assert_eq!(mem.seek(0x10, SeekOrigin::End), Ok(0xFFF0));
    mem.set_end_of_file().unwrap();
    assert_eq!(mem.size(), Ok(0xFFF0));
    assert_eq!(mem.resolve_path(b"..\\x", 64), (true, b"mem\\x".to_vec()));
    assert_eq!(mem.file_sys_path(64), b"mem\\root\\");
    assert_eq!(mem.path_attributes(b"x"), Ok(0));
    assert_eq!(mem.open_node(b"x"), Err(VfsError::Failed));

    let mut fixed = MemFileSystem::new(None, GENERIC_WRITE, 3, MemSource::Wrap(vec![0; 4]));
    assert_eq!(fixed.size(), Ok(4));
    assert_eq!(fixed.write(b"abcdef"), Ok(4));
    assert_eq!(fixed.write(b"x"), Ok(0));
    assert_eq!(fixed.buffer(), b"abcd");
    let mut read_only = MemFileSystem::new(None, GENERIC_READ, 3, MemSource::Copy(Some(b"xyz")));
    assert_eq!(read_only.write(b"q"), Err(VfsError::Failed));
    assert_eq!(read_only.size(), Ok(3));

    let archive = MemFileSystem::new(
        None,
        GENERIC_READ,
        3,
        MemSource::Copy(Some(&build_full(&sample_items()))),
    );
    let fs = UtfFileSystem::open_memory(&archive, &FsDesc::read(b"mem.utf")).unwrap();
    assert!(fs.open_file(&FsDesc::read(b"mesh\\sphere")).is_ok());
    let compact = MemFileSystem::new(
        None,
        GENERIC_READ,
        3,
        MemSource::Copy(Some(&build_compact(&sample_items()))),
    );
    assert_eq!(
        UtfFileSystem::open_memory(&compact, &FsDesc::read(b"m")).unwrap_err(),
        VfsError::OpenFailed
    );
    let empty = MemFileSystem::new(None, GENERIC_READ, 3, MemSource::Copy(None));
    assert_eq!(
        UtfFileSystem::open_memory(&empty, &FsDesc::read(b"m")).unwrap_err(),
        VfsError::Generic
    );
}

#[test]
fn search_paths() {
    let dir = tree("search");
    let host = Host::new(&dir);
    let mut search = SearchPath::new();
    search.set_path(&host, b"patch;;missing;data\\sub;data");
    assert_eq!(search.path(), b"patch;;missing;data\\sub;data");
    assert_eq!(search.get_path(5), (5, b"patc".to_vec()));
    let opened: Vec<bool> = search.roots().iter().map(Option::is_some).collect();
    assert_eq!(opened, [true, false, true, true]);
    let desc = FsDesc::read(b"file.txt").with_implementation(b"DOS");
    let mut found = search.create(&desc).unwrap();
    let mut buf = [0u8; 5];
    assert_eq!(found.read(&mut buf), Ok(5));
    assert_eq!(&buf, b"patch");
    let mesh = search.create(&FsDesc::read(b"mesh.utf")).unwrap();
    assert_eq!(mesh.kind(), FsKind::Utf);
    assert_eq!(
        search.create(&FsDesc::read(b"nothing")).unwrap_err(),
        VfsError::Generic
    );
    assert_eq!(
        search.create(&FsDesc::read(b"plain.cmp")).unwrap_err(),
        VfsError::NotAnArchive
    );
    search.set_path(&host, b"");
    assert_eq!(search.create(&desc).unwrap_err(), VfsError::OpenFailed);
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn malformed_archives_report_errors() {
    let mut bytes = build_full(&[Item::File(b"a", b"x".to_vec())]);
    let tree = crate::utf::HEADER_SIZE;
    bytes[tree + 0x10..tree + 0x14].copy_from_slice(&0x00FF_0000u32.to_le_bytes());
    let fs = UtfFileSystem::from_bytes(bytes, b"bad").unwrap();
    assert!(matches!(
        fs.open_file(&FsDesc::read(b"a")),
        Err(VfsError::Malformed(_))
    ));
    assert!(matches!(fs.find(b"*"), Err(VfsError::Malformed(_))));
}
