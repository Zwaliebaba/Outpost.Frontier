//! Oracle vectors for work package P1.3: the UTF container and the file
//! systems (`CppSource/ReadFile`).
//!
//! The inputs are in `tests/fixtures/oracle/vfs/`, written by
//! `make_vfs_fixtures.py`; the harness side is
//! `CppSource/Oracle/oracle_vfs.cpp`, which runs the same operations on
//! the reference's `FileSystem` and `SearchPath` objects. Both run with the
//! fixture directory as the current directory, so relative paths match.
//!
//! Error fields carry the reference's last error after the operation (0
//! when it did not change it). A DOS attribute value keeps only the
//! directory bit, since the other bits come from the host.

use std::collections::HashMap;

mod support;

use neuron_core::utf::{
    Layout, NodeRef, UtfArchive, dos_date_time_to_file_time, fold_wild_char, match_wildcard,
};
use neuron_core::vfs::path::join_root_buffer;
use neuron_core::vfs::{
    ATTRIBUTE_DIRECTORY, FileSystem, FsDesc, FsKind, GENERIC_READ, Host, MemFileSystem, MemSource,
    SearchPath, SeekOrigin, UtfFileSystem, VfsError, VfsFile, create_file_system,
};
use support::{check_suite, fixtures, hex, input_lines, unhex};

/// Reads larger than this are not attempted (none of the fixtures needs one).
const READ_LIMIT: u32 = 0x10000;

fn host() -> Host {
    Host::new(fixtures())
}

fn arg(token: &str) -> Vec<u8> {
    if token == "-" {
        Vec::new()
    } else {
        unhex(token)
    }
}

fn number(token: &str) -> u32 {
    token.parse().unwrap_or_else(|e| panic!("{token}: {e}"))
}

fn kind_name(kind: FsKind) -> &'static str {
    match kind {
        FsKind::DosDirectory => "dos-dir",
        FsKind::DosFile => "dos-file",
        FsKind::Utf => "utf",
        FsKind::Memory => "mem",
    }
}

/// The `CreateInstance` result as the reference returns it.
fn genresult(error: VfsError) -> i32 {
    i32::from_ne_bytes(error.code().to_ne_bytes())
}

fn err_code<T>(result: &Result<T, VfsError>) -> u32 {
    result.as_ref().err().map_or(0, |e| e.code())
}

#[test]
fn vfs_join() {
    let lines: Vec<String> = input_lines("vfs/join.list")
        .iter()
        .map(|line| {
            let parts: Vec<&str> = line.split(':').collect();
            let (root, rel) = (unhex(parts[0]), unhex(parts[1]));
            let n: usize = parts[2].parse().unwrap();
            let (ok, out) = join_root_buffer(&root, &rel, n);
            let mem = MemFileSystem::new(Some(&root), GENERIC_READ, 3, MemSource::Copy(None));
            let (mem_ok, mem_out) = mem.resolve_path(&rel, n);
            format!(
                "{{\"root\":\"{}\",\"rel\":\"{}\",\"n\":\"{n}\",\"ok\":\"{}\",\"out\":\"{}\",\"mem_ok\":\"{}\",\"mem_out\":\"{}\"}}",
                parts[0],
                parts[1],
                u8::from(ok),
                hex(&out),
                u8::from(mem_ok),
                hex(&mem_out)
            )
        })
        .collect();
    check_suite("vfs_join.jsonl", &lines);
}

#[test]
fn vfs_match() {
    let mut lines: Vec<String> = input_lines("vfs/match.hex")
        .iter()
        .map(|line| {
            let (name, pattern) = line.split_once(':').unwrap();
            let matched = match_wildcard(&unhex(name), &unhex(pattern));
            format!(
                "{{\"name\":\"{name}\",\"pattern\":\"{pattern}\",\"match\":\"{}\"}}",
                u8::from(matched)
            )
        })
        .collect();
    let folds: Vec<u8> = (0..=255u8).map(fold_wild_char).collect();
    lines.push(format!("{{\"fold\":\"{}\"}}", hex(&folds)));
    check_suite("vfs_match.jsonl", &lines);
}

#[test]
fn vfs_dostime() {
    let lines: Vec<String> = input_lines("vfs/dostime.list")
        .iter()
        .map(|line| {
            let (date, time) = line.split_once(':').unwrap();
            let ft = dos_date_time_to_file_time(
                u16::from_str_radix(date, 16).unwrap(),
                u16::from_str_radix(time, 16).unwrap(),
            );
            format!(
                "{{\"date\":\"{date}\",\"time\":\"{time}\",\"ok\":\"{}\",\"ft\":\"{:016x}\"}}",
                u8::from(ft.is_some()),
                ft.unwrap_or(0)
            )
        })
        .collect();
    check_suite("vfs_dostime.jsonl", &lines);
}

/// Opens `path` on `fs`, reads it whole, then once more past the end.
fn open_fields(fs: &dyn FileSystem, path: &[u8]) -> String {
    match fs.open_file(&FsDesc::read(path)) {
        Err(e) => format!("\"ok\":\"0\",\"err\":\"{:x}\"", e.code()),
        Ok(mut file) => {
            let size = file.size().unwrap_or(u32::MAX);
            let read = read_fields(&mut file, size);
            let mut probe = [0u8; 4];
            let eof = file.read(&mut probe);
            let (r, got) = match eof {
                Ok(n) => (1, n),
                Err(_) => (0, 0),
            };
            format!(
                "\"ok\":\"1\",\"fsize\":\"{size:08x}\",{read},\"eof\":\"{r},{got},{:x}\"",
                err_code(&eof)
            )
        }
    }
}

/// `"read","got","rerr","data"` of one read of `size` bytes.
fn read_fields(file: &mut VfsFile, size: u32) -> String {
    if size > READ_LIMIT {
        return "\"read\":\"-\"".to_owned();
    }
    let mut buf = vec![0u8; size as usize];
    render_read(file.read(&mut buf), &buf)
}

fn render_read(result: Result<usize, VfsError>, buf: &[u8]) -> String {
    match result {
        Ok(n) => format!(
            "\"read\":\"1\",\"got\":\"{n}\",\"rerr\":\"0\",\"data\":\"{}\"",
            hex(&buf[..n])
        ),
        Err(e) => format!(
            "\"read\":\"0\",\"got\":\"0\",\"rerr\":\"{:x}\",\"data\":\"\"",
            e.code()
        ),
    }
}

fn times_field(fs: &UtfFileSystem, times: [Option<u64>; 3]) -> String {
    if fs.archive().layout() == Layout::Full {
        times
            .iter()
            .map(|t| format!("{:016x}", t.unwrap_or(0)))
            .collect::<Vec<_>>()
            .join(",")
    } else {
        "-".to_owned()
    }
}

fn dump_tree(fs: &UtfFileSystem, prefix: &[u8], depth: u32, lines: &mut Vec<String>) {
    let mut pattern = prefix.to_vec();
    pattern.push(b'*');
    let entries = match fs.find(&pattern) {
        Ok(entries) if !entries.is_empty() => entries,
        other => {
            let err = other.err().map_or(0x12, VfsError::code);
            lines.push(format!(
                "{{\"find\":\"{}\",\"err\":\"{err:x}\"}}",
                hex(&pattern)
            ));
            return;
        }
    };
    for entry in entries {
        let mut path = prefix.to_vec();
        path.extend_from_slice(&entry.data.name);
        let attributes = fs.path_attributes(&path);
        lines.push(format!(
            "{{\"path\":\"{}\",\"attr\":\"{:08x}\",\"size\":\"{:08x}\",\"times\":\"{}\",\"pattr\":\"{:08x}\",\"perr\":\"{:x}\",{}}}",
            hex(&path),
            entry.data.attributes,
            entry.data.size,
            times_field(fs, entry.data.times),
            attributes.as_ref().copied().unwrap_or(0),
            err_code(&attributes),
            open_fields(fs, &path)
        ));
        if entry.data.is_dir() && depth < 6 {
            path.push(b'\\');
            dump_tree(fs, &path, depth + 1, lines);
        }
    }
}

#[test]
fn utf_tree() {
    let host = host();
    let mut lines = Vec::new();
    for archive in input_lines("vfs/utf.list") {
        let desc = FsDesc::read(archive.as_bytes()).with_implementation(b"UTF");
        match UtfFileSystem::open(&host, &desc) {
            Err(e) => lines.push(format!(
                "{{\"archive\":\"{archive}\",\"result\":\"{}\"}}",
                genresult(e)
            )),
            Ok(fs) => {
                lines.push(format!("{{\"archive\":\"{archive}\",\"result\":\"0\"}}"));
                dump_tree(&fs, b"", 0, &mut lines);
            }
        }
    }
    check_suite("utf_tree.jsonl", &lines);
}

/// The operation scripts of `utf_ops.txt`, `dos_ops.txt` and
/// `mem_ops.txt`; see `oracle_vfs.cpp` for the same interpreter.
struct Script {
    host: Host,
    stack: Vec<Box<dyn FileSystem>>,
    mem: Option<MemFileSystem>,
    search: Option<SearchPath>,
    lines: Vec<String>,
}

impl Script {
    fn new() -> Self {
        Self {
            host: host(),
            stack: Vec::new(),
            mem: None,
            search: None,
            lines: Vec::new(),
        }
    }

    fn top(&mut self) -> &mut dyn FileSystem {
        if let Some(mem) = self.mem.as_mut() {
            return mem;
        }
        self.stack.last_mut().expect("no file system").as_mut()
    }

    fn emit(&mut self, op: &str, rest: &str, fields: &str) {
        let sep = if fields.is_empty() { "" } else { "," };
        self.lines.push(format!(
            "{{\"op\":\"{op}\",\"arg\":\"{rest}\"{sep}{fields}}}"
        ));
    }

    /// The result and kind of a creation; pushes what was created.
    fn created(&mut self, result: Result<Box<dyn FileSystem>, VfsError>) -> String {
        match result {
            Ok(fs) => {
                let kind = kind_name(fs.kind());
                self.stack.push(fs);
                format!("\"result\":\"0\",\"kind\":\"{kind}\"")
            }
            Err(VfsError::NotAnArchive) => "\"result\":\"0\",\"kind\":\"nested\"".to_owned(),
            Err(e) => format!("\"result\":\"{}\"", genresult(e)),
        }
    }

    fn self_fields(fs: &mut dyn FileSystem) -> String {
        match fs.size() {
            Err(e) => format!("\"size\":\"ffffffff\",\"serr\":\"{:x}\"", e.code()),
            Ok(size) if size > READ_LIMIT => format!("\"size\":\"{size:08x}\",\"read\":\"-\""),
            Ok(size) => {
                let mut buf = vec![0u8; size as usize];
                let result = fs.read(&mut buf);
                format!("\"size\":\"{size:08x}\",{}", render_read(result, &buf))
            }
        }
    }

    fn run(&mut self, line: &str) {
        let (op, rest) = line.split_once(' ').unwrap_or((line, ""));
        let t: Vec<&str> = rest.split(' ').collect();
        let fields = match op {
            "archive" => {
                self.stack.clear();
                let path = arg(t[0]);
                let desc = FsDesc::read(&path).with_implementation(b"UTF");
                let result = UtfFileSystem::open(&self.host, &desc)
                    .map(|fs| Box::new(fs) as Box<dyn FileSystem>);
                self.created(result)
            }
            "dos" => {
                self.stack.clear();
                let implementation = arg(t[0]);
                let path = arg(t[1]);
                let mut desc = FsDesc::read(&path);
                if t[0] != "-" {
                    desc = desc.with_implementation(&implementation);
                }
                let result = create_file_system(&self.host, &desc);
                self.created(result)
            }
            "child" | "clone" => {
                let (implementation, path) = if op == "child" {
                    (t[0], arg(t[1]))
                } else {
                    ("-", arg(t[0]))
                };
                let implementation_bytes = arg(implementation);
                let mut desc = FsDesc::read(&path);
                if implementation != "-" {
                    desc = desc.with_implementation(&implementation_bytes);
                }
                let result = self.top().create_child(&desc);
                self.created(result)
            }
            "clonefind" => {
                let pattern = arg(t[0]);
                let index = number(t[1]) as usize;
                match self.top().find(&pattern) {
                    Ok(entries) if index < entries.len() => {
                        let entry = &entries[index];
                        let desc = FsDesc::read(&entry.data.name).with_found(entry.token);
                        let name = hex(&entry.data.name);
                        let result = self.top().create_child(&desc);
                        format!("\"name\":\"{name}\",{}", self.created(result))
                    }
                    _ => "\"result\":\"none\"".to_owned(),
                }
            }
            "pop" => {
                self.stack.pop();
                String::new()
            }
            "self" => Self::self_fields(self.top()),
            "readself" => {
                let mut buf = vec![0u8; number(t[0]) as usize];
                let result = self.top().read(&mut buf);
                render_read(result, &buf)
            }
            "seek" => {
                let origin = match number(t[0]) {
                    1 => SeekOrigin::Current,
                    2 => SeekOrigin::End,
                    _ => SeekOrigin::Begin,
                };
                let result = self.top().seek(number(t[1]), origin);
                let value = result.map_or(-1, |v| i32::from_ne_bytes(v.to_ne_bytes()));
                format!("\"result\":\"{value}\"")
            }
            "map" => {
                let (offset, count) = (number(t[0]), number(t[1]));
                match self.top().map_view(offset, count) {
                    Ok(view) => format!("\"ok\":\"1\",\"data\":\"{}\"", hex(view)),
                    Err(e) => format!("\"ok\":\"0\",\"err\":\"{:x}\"", e.code()),
                }
            }
            "syspath" => format!(
                "\"out\":\"{}\"",
                hex(&self.top().file_sys_path(number(t[0]) as usize))
            ),
            "curpath" => match self.top().current_path(number(t[0]) as usize) {
                Ok(path) => format!("\"ok\":\"1\",\"out\":\"{}\"", hex(&path)),
                Err(e) => format!("\"ok\":\"0\",\"err\":\"{:x}\"", e.code()),
            },
            "attr" => {
                let path = arg(t[0]);
                let fs = self.top();
                let dos = matches!(fs.kind(), FsKind::DosDirectory | FsKind::DosFile);
                match fs.path_attributes(&path) {
                    Ok(v) => format!("\"value\":\"{:08x}\",\"err\":\"0\"", v & 0x10),
                    Err(VfsError::FileNotFound) if dos => {
                        "\"value\":\"ffffffff\",\"err\":\"-\"".to_owned()
                    }
                    Err(e) => format!("\"value\":\"00000000\",\"err\":\"{:x}\"", e.code()),
                }
            }
            "find" => {
                let pattern = arg(t[0]);
                // As the harness: a directory's entries are sorted and keep
                // only the directory bit (divergences D31, D37).
                let dos = matches!(self.top().kind(), FsKind::DosDirectory | FsKind::DosFile);
                let result = self.top().find(&pattern);
                let (count, err) = match &result {
                    Ok(entries) => (entries.len(), 0x12),
                    Err(e) => (0, e.code()),
                };
                let mut entries: Vec<String> = result
                    .iter()
                    .flatten()
                    .map(|entry| {
                        let attributes = if dos {
                            entry.data.attributes & ATTRIBUTE_DIRECTORY
                        } else {
                            entry.data.attributes
                        };
                        format!(
                            "{{\"op\":\"entry\",\"name\":\"{}\",\"attr\":\"{attributes:08x}\",\"size\":\"{:08x}\"}}",
                            hex(&entry.data.name),
                            entry.data.size
                        )
                    })
                    .collect();
                if dos {
                    entries.sort();
                }
                self.lines.extend(entries);
                format!("\"count\":\"{count}\",\"err\":\"{err:x}\"")
            }
            "open" => {
                let path = arg(t[0]);
                open_fields(self.top(), &path)
            }
            "resolve" => {
                let (ok, out) = self.top().resolve_path(&arg(t[0]), number(t[1]) as usize);
                format!("\"ok\":\"{}\",\"out\":\"{}\"", u8::from(ok), hex(&out))
            }
            "node" => {
                let result = self.top().open_node(&arg(t[0]));
                format!(
                    "\"ok\":\"{}\",\"err\":\"{:x}\"",
                    u8::from(result.is_ok()),
                    err_code(&result)
                )
            }
            "search" => {
                let mut search = SearchPath::new();
                search.set_path(&self.host, &arg(t[0]));
                let roots: Vec<String> = search
                    .roots()
                    .iter()
                    .map(|r| {
                        r.as_ref().map_or("-".to_owned(), |fs| {
                            format!("{}:{}", kind_name(fs.kind()), hex(&fs.file_sys_path(0x104)))
                        })
                    })
                    .collect();
                self.search = Some(search);
                format!("\"roots\":\"{}\"", roots.join(","))
            }
            "getpath" => {
                let (count, out) = self
                    .search
                    .as_ref()
                    .unwrap()
                    .get_path(number(t[0]) as usize);
                format!("\"ret\":\"{count}\",\"out\":\"{}\"", hex(&out))
            }
            "sfind" => {
                let implementation = arg(t[0]);
                let path = arg(t[1]);
                let mut desc = FsDesc::read(&path);
                if t[0] != "-" {
                    desc = desc.with_implementation(&implementation);
                }
                match self.search.as_ref().unwrap().create(&desc) {
                    Ok(mut fs) => {
                        let mut out = format!(
                            "\"result\":\"0\",\"kind\":\"{}\",\"path\":\"{}\"",
                            kind_name(fs.kind()),
                            hex(&fs.file_sys_path(0x104))
                        );
                        if fs.kind() == FsKind::DosFile {
                            out.push(',');
                            out.push_str(&Self::self_fields(fs.as_mut()));
                        }
                        out
                    }
                    Err(VfsError::NotAnArchive) => {
                        "\"result\":\"0\",\"kind\":\"nested\"".to_owned()
                    }
                    Err(e) => format!("\"result\":\"{}\"", genresult(e)),
                }
            }
            "mem" => {
                let access = u32::from_str_radix(t[0], 16).unwrap();
                let creation = number(t[1]);
                let data = arg(t[3]);
                let path = arg(t[4]);
                let source = match t[2] {
                    "wrap" => MemSource::Wrap(data.clone()),
                    _ => MemSource::Copy((!data.is_empty()).then_some(&data[..])),
                };
                let path = (t[4] != "-").then_some(&path[..]);
                self.mem = Some(MemFileSystem::new(path, access, creation, source));
                "\"result\":\"0\"".to_owned()
            }
            "state" => {
                let mem = self.mem.as_ref().unwrap();
                let buffer = mem.buffer();
                let fnv = buffer.iter().fold(0x811C_9DC5u32, |h, &b| {
                    (h ^ u32::from(b)).wrapping_mul(0x0100_0193)
                });
                format!(
                    "\"size\":\"{:08x}\",\"pos\":\"{}\",\"alloc\":\"{}\",\"fnv\":\"{fnv:08x}\",\"head\":\"{}\"",
                    mem.logical_size(),
                    mem.position().unwrap(),
                    buffer.len(),
                    hex(&buffer[..buffer.len().min(32)])
                )
            }
            "write" | "writeat" => {
                let (position, data) = if op == "write" {
                    (None, arg(t[0]))
                } else {
                    (Some(number(t[0])), arg(t[1]))
                };
                match self.mem.as_mut().unwrap().write_at(&data, position) {
                    Ok(n) => format!("\"ok\":\"1\",\"written\":\"{n}\""),
                    Err(_) => "\"ok\":\"0\",\"written\":\"0\"".to_owned(),
                }
            }
            "read" | "readat" => {
                let (position, n) = if op == "read" {
                    (None, number(t[0]))
                } else {
                    (Some(number(t[0])), number(t[1]))
                };
                let mut buf = vec![0u8; n as usize];
                let got = self
                    .mem
                    .as_mut()
                    .unwrap()
                    .read_at(&mut buf, position)
                    .unwrap();
                format!(
                    "\"ok\":\"1\",\"got\":\"{got}\",\"data\":\"{}\"",
                    hex(&buf[..got])
                )
            }
            "setend" => {
                let result = self.top().set_end_of_file();
                format!("\"ok\":\"{}\"", u8::from(result.is_ok()))
            }
            other => panic!("unknown operation {other}"),
        };
        self.emit(op, rest, &fields);
    }
}

fn run_script(input: &str, suite: &str) {
    let mut script = Script::new();
    for line in input_lines(input) {
        script.run(&line);
    }
    check_suite(suite, &script.lines);
}

#[test]
fn utf_ops() {
    run_script("vfs/utf_ops.txt", "utf_ops.jsonl");
}

#[test]
fn vfs_dos() {
    run_script("vfs/dos_ops.txt", "vfs_dos.jsonl");
}

#[test]
fn vfs_mem() {
    run_script("vfs/mem_ops.txt", "vfs_mem.jsonl");
}

/// The Python walker's view of each well-formed archive
/// (`CppSource/ReadFile/utf_walk_once.py`, run by the fixture generator)
/// equals the tree the Rust reader builds with `FindChild`'s iteration.
#[test]
fn python_walker_agrees() {
    fn dump(archive: &UtfArchive, dir: NodeRef, depth: usize, lines: &mut Vec<String>) {
        for child in archive.children(dir) {
            let child = child.unwrap();
            let record = archive.record(child).unwrap();
            lines.push(format!(
                "{depth}\t{}\t{}\t{:08x}\t{:08x}",
                if record.name.is_empty() {
                    "-".to_owned()
                } else {
                    hex(record.name)
                },
                u8::from(record.is_dir),
                record.data_offset,
                record.size
            ));
            let has_children = match archive.layout() {
                Layout::Full => record.data_offset != 0,
                Layout::Compact => true,
            };
            if record.is_dir && has_children {
                dump(archive, child, depth + 1, lines);
            }
        }
    }

    let walker = input_lines("vfs/walker.txt");
    let mut files: HashMap<String, Vec<String>> = HashMap::new();
    let mut current = String::new();
    for line in walker {
        if let Some(file) = line.strip_prefix("file ") {
            current = file.to_owned();
            files.insert(current.clone(), Vec::new());
        } else {
            files.get_mut(&current).unwrap().push(line);
        }
    }
    assert!(!files.is_empty());
    for (file, want) in files {
        let archive = UtfArchive::parse(support::read_fixture(&file)).unwrap();
        let root = archive.record(archive.root()).unwrap();
        let mut got = vec![format!(
            "root\t{}\t{}\t{:08x}\t{:08x}",
            hex(root.name),
            u8::from(root.is_dir),
            root.data_offset,
            root.size
        )];
        dump(&archive, archive.root(), 0, &mut got);
        assert_eq!(got, want, "{file}");
    }
}
