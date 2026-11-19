//! The save suite of `CppSource/Oracle/oracle_scene_script_save.cpp`: a
//! script engine driven by a program of slot calls, with a writer that
//! records each `Write`, the two COM contexts and the component manager's
//! file systems of the harness; and the `thn_save` data dump.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;

use neuron_client::scene_script::playback::EventRef;
use neuron_client::scene_script::{ComContext, ScriptEngine, ScriptModule};
use neuron_core::vfs::{FileSystem, FindEntry, FsDesc, FsKind, SeekOrigin, VfsError, VfsFile};

use super::{hex, input_lines, unhex};

type Log = Rc<RefCell<Vec<String>>>;

fn text(s: Option<&[u8]>) -> String {
    s.map_or_else(|| "null".to_owned(), |s| format!("s{}", hex(s)))
}

/// What a write logs: the bytes and, in the suite, the bytes taken.
#[derive(Clone, Copy, Debug)]
enum Writes {
    /// Takes every byte.
    Full,
    /// Takes half (the short-write cases).
    Short,
    /// The data dump: takes every byte and logs no count.
    Dump,
}

/// A file of the harness: the bytes a load reads, or a writer.
#[derive(Debug)]
struct TestFile {
    data: Vec<u8>,
    writes: Option<Writes>,
    log: Log,
    head: String,
}

impl TestFile {
    fn reader(data: &[u8], log: &Log, head: &str) -> TestFile {
        TestFile {
            data: data.to_vec(),
            writes: None,
            log: log.clone(),
            head: head.to_owned(),
        }
    }

    fn writer(writes: Writes, log: &Log, head: &str) -> TestFile {
        TestFile {
            data: Vec::new(),
            writes: Some(writes),
            log: log.clone(),
            head: head.to_owned(),
        }
    }
}

impl FileSystem for TestFile {
    fn kind(&self) -> FsKind {
        FsKind::Memory
    }
    fn create_child(&self, _: &FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError> {
        Err(VfsError::Generic)
    }
    fn open_file(&self, _: &FsDesc<'_>) -> Result<VfsFile, VfsError> {
        Err(VfsError::Failed)
    }
    fn open_node(&mut self, _: &[u8]) -> Result<(), VfsError> {
        Ok(())
    }
    fn path_attributes(&self, _: &[u8]) -> Result<u32, VfsError> {
        Ok(u32::MAX)
    }
    fn find(&self, _: &[u8]) -> Result<Vec<FindEntry>, VfsError> {
        Ok(Vec::new())
    }
    fn resolve_path(&self, _: &[u8], _: usize) -> (bool, Vec<u8>) {
        (true, Vec::new())
    }
    fn file_sys_path(&self, _: usize) -> Vec<u8> {
        b"oracle".to_vec()
    }
    fn current_path(&self, _: usize) -> Result<Vec<u8>, VfsError> {
        Ok(Vec::new())
    }
    fn access(&self) -> u32 {
        0
    }
    /// The harness's `Read` copies from the start of the buffer.
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, VfsError> {
        let n = buf.len().min(self.data.len());
        buf[..n].copy_from_slice(&self.data[..n]);
        Ok(n)
    }
    fn size(&self) -> Result<u32, VfsError> {
        Ok(u32::try_from(self.data.len()).expect("a small buffer"))
    }
    fn seek(&mut self, _: u32, _: SeekOrigin) -> Result<u32, VfsError> {
        Ok(0)
    }
    fn position(&self) -> Result<u32, VfsError> {
        Ok(0)
    }
    fn map_view(&self, _: u32, _: u32) -> Result<&[u8], VfsError> {
        Err(VfsError::Failed)
    }
    fn write(&mut self, data: &[u8]) -> Result<usize, VfsError> {
        let writes = self.writes.unwrap_or(Writes::Full);
        let taken = match writes {
            Writes::Short => data.len() / 2,
            Writes::Full | Writes::Dump => data.len(),
        };
        let line = match writes {
            Writes::Dump => format!("{}\"write\":\"{}\"}}", self.head, hex(data)),
            _ => format!(
                "{}\"write\":\"{}\",\"r\":\"{taken}\"}}",
                self.head,
                hex(data)
            ),
        };
        self.log.borrow_mut().push(line);
        Ok(taken)
    }
}

/// The harness's COM contexts: 1 hands the text back, 2 asks for 4 bytes
/// and fails. Both keep `Slot3`'s text by the event's id.
struct Context {
    kind: i32,
    texts: BTreeMap<i32, Option<Vec<u8>>>,
    log: Option<Log>,
    head: String,
}

impl Context {
    fn line(&self, body: String) {
        if let Some(log) = &self.log {
            log.borrow_mut().push(format!("{}{body}", self.head));
        }
    }
}

/// An event as the context lines name it (its id is the module's serial
/// number in the reference; divergence D249).
fn event(e: EventRef<'_>) -> String {
    let c = &e.desc.common;
    format!(
        "\"start\":\"{}\",\"primary\":\"{}\",\"duration\":\"{}\",\"taken\":\"{}\"",
        c.start_ms,
        c.primary,
        c.duration_ms,
        i32::from(e.id != -1)
    )
}

impl ComContext for Context {
    fn set_user_event_string(&mut self, e: EventRef<'_>, value: Option<&[u8]>) {
        self.texts.insert(e.id, value.map(<[u8]>::to_vec));
        self.line(format!(
            "\"com\":\"3\",{},\"text\":\"{}\",\"r\":\"0\"}}",
            event(e),
            text(value)
        ));
    }

    fn user_event_string_size(&mut self, e: EventRef<'_>) -> u32 {
        let size = if self.kind == 1 {
            match self.texts.get(&e.id) {
                Some(Some(t)) => u32::try_from(t.len() + 1).expect("a short text"),
                _ => 0,
            }
        } else {
            4
        };
        self.line(format!("\"com\":\"5\",{},\"r\":\"{size}\"}}", event(e)));
        size
    }

    fn user_event_string(&mut self, e: EventRef<'_>, buffer: &mut [u8], size: u32) -> i32 {
        let r = if self.kind == 1 {
            let mut t = self.texts.get(&e.id).cloned().flatten().unwrap_or_default();
            t.push(0);
            let n = t.len().min(size as usize);
            buffer[..n].copy_from_slice(&t[..n]);
            0
        } else {
            buffer[..3].copy_from_slice(b"ab\0");
            -1
        };
        self.line(format!(
            "\"com\":\"4\",{},\"size\":\"{size}\",\"r\":\"{r}\"}}",
            event(e)
        ));
        r
    }
}

fn status(engine: &ScriptEngine) -> String {
    let p = engine.player();
    format!(
        "\"state\":\"{}\",\"entities\":\"{}\",\"events\":\"{}\",\"duration\":\"{}\"}}",
        p.state(),
        p.live_entity_count(),
        p.event_count(),
        p.duration()
    )
}

fn hexf(token: &str) -> f32 {
    f32::from_bits(u32::from_str_radix(token, 16).unwrap_or_else(|e| panic!("{token}: {e}")))
}

fn int(token: &str) -> i32 {
    token.parse().unwrap_or_else(|e| panic!("{token}: {e}"))
}

/// One case's program on a fresh engine and module.
fn run_case(head: &str, program: &str, scripts: &[Vec<u8>], log: &Log) {
    let mut module = ScriptModule::new();
    let mut engine = ScriptEngine::new();
    let factory = |desc: &FsDesc<'_>| -> Result<Box<dyn FileSystem>, VfsError> {
        log.borrow_mut().push(format!(
            "{head}\"fs\":\"{}\",\"access\":\"{:08x}\",\"creation\":\"{}\"}}",
            text(Some(desc.path)),
            desc.access,
            desc.creation
        ));
        if desc.access == 0xC000_0000 {
            return Ok(Box::new(TestFile::writer(Writes::Full, log, head)));
        }
        for (index, script) in scripts.iter().enumerate() {
            if desc.path == format!("script{index}.thn").as_bytes() {
                return Ok(Box::new(TestFile::reader(script, log, head)));
            }
        }
        Err(VfsError::OpenFailed)
    };
    for op in program.split(',') {
        let arg = &op[1..];
        let r = match op.as_bytes()[0] {
            b'c' => {
                let com: Option<Rc<RefCell<dyn ComContext>>> = if arg == "0" {
                    None
                } else {
                    Some(Rc::new(RefCell::new(Context {
                        kind: int(arg),
                        texts: BTreeMap::new(),
                        log: Some(log.clone()),
                        head: head.to_owned(),
                    })))
                };
                engine.set_com(com)
            }
            b'L' if arg == "-" => engine.load(&mut module, None),
            b'L' => {
                let mut file = TestFile::reader(&scripts[int(arg) as usize], log, head);
                engine.load(&mut module, Some(&mut file))
            }
            b'l' => {
                let path = match arg {
                    "-" => None,
                    "x" => Some(b"missing.thn".to_vec()),
                    _ => Some(format!("script{arg}.thn").into_bytes()),
                };
                engine.load_path(&mut module, path.as_deref(), &factory)
            }
            b'S' | b's' => {
                let writes = if op == "s" {
                    Writes::Short
                } else {
                    Writes::Full
                };
                let mut file = TestFile::writer(writes, log, head);
                engine.save(Some(&mut file))
            }
            b'N' => engine.save(None),
            b'W' => engine.save_path(b"saved.thn", &factory),
            b't' => engine.player_mut().tick(int(arg)),
            b'g' => engine.player_mut().start(int(arg)),
            b'D' => {
                engine.player_mut().set_duration(int(arg));
                0
            }
            b'u' => {
                let parts: Vec<&str> = arg.split(':').collect();
                engine
                    .player_mut()
                    .set_user_prop(int(parts[0]), &unhex(parts[1]), &unhex(parts[2]))
            }
            b'r' => engine.player_mut().remove_entity(int(arg)),
            b'e' => {
                let id = engine
                    .player()
                    .events()
                    .nth(int(arg) as usize)
                    .map_or(-1, |(id, _)| id);
                engine.player_mut().remove_event(id)
            }
            b'P' | b'O' => {
                let words: Vec<f32> = arg.split('/').map(hexf).collect();
                if op.starts_with('P') {
                    engine
                        .player_mut()
                        .set_position(words[..3].try_into().expect("three words"));
                } else {
                    engine
                        .player_mut()
                        .set_orientation(words[..9].try_into().expect("nine words"));
                }
                0
            }
            _ => panic!("scene_script_save: bad op {op}"),
        };
        let line = format!("{head}\"op\":\"{op}\",\"r\":\"{r}\",{}", status(&engine));
        log.borrow_mut().push(line);
    }
}

/// The suite's lines (`tests/fixtures/oracle/scene_script_save.list`).
pub fn lines() -> Vec<String> {
    let log: Log = Rc::new(RefCell::new(Vec::new()));
    for (case, line) in input_lines("scene_script_save.list").iter().enumerate() {
        let mut fields = line.split(' ');
        let program = fields.next().expect("a program");
        let scripts: Vec<Vec<u8>> = fields.map(unhex).collect();
        run_case(&format!("{{\"case\":\"{case}\","), program, &scripts, &log);
    }
    log.take()
}

/// The `thn_save` dump of one file: loaded with context 1 into a fresh
/// engine and module, then saved.
pub fn dump_lines(file: &str, bytes: &[u8]) -> Vec<String> {
    let log: Log = Rc::new(RefCell::new(Vec::new()));
    let mut module = ScriptModule::new();
    let mut engine = ScriptEngine::new();
    engine.set_com(Some(Rc::new(RefCell::new(Context {
        kind: 1,
        texts: BTreeMap::new(),
        log: None,
        head: String::new(),
    }))));
    let mut source = TestFile::reader(bytes, &log, "{");
    let load = engine.load(&mut module, Some(&mut source));
    let p = engine.player();
    log.borrow_mut().push(format!(
        "{{\"file\":\"{file}\",\"load\":\"{load}\",\"entities\":\"{}\",\"events\":\"{}\",\"duration\":\"{}\"}}",
        p.live_entity_count(),
        p.event_count(),
        p.duration()
    ));
    let save = if load < 0 {
        -1
    } else {
        let mut writer = TestFile::writer(Writes::Dump, &log, "{");
        engine.save(Some(&mut writer))
    };
    log.borrow_mut().push(format!("{{\"save\":\"{save}\"}}"));
    log.take()
}
