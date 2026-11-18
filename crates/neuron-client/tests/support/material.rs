//! P4.5: the material regular expressions, the material library, the
//! global material records, material animation and the material batcher,
//! as `CppSource/Oracle/oracle_material.cpp` writes them (line formats at
//! the top of that file), over `tests/fixtures/oracle/material_*.list`.
//!
//! The materials are the harness's fakes (`FOracleMaterial`): numbered in
//! creation order from 1 in each case, made by type name through the
//! factory or directly, each writing a line for every call it receives.
//! Their types: any name of [`NORMAL_TYPES`] is a material; `BadInit`
//! fails `Initialize`; `BatchedMaterial` has the batched face and accepts
//! an inner type of [`NORMAL_TYPES`]; `NotMaterial` is a component without
//! the material face; every other name is unknown to the factory.
//!
//! The cases keep to what the reference defines: no entry whose material
//! was cleared or destroyed is used, and no queued submission reads past
//! the arrays it was given.

use std::cell::{Cell, RefCell};
use std::collections::BTreeMap;
use std::rc::{Rc, Weak};

use neuron_client::render::material::animation::AnimationData;
use neuron_client::render::material::batcher::{
    BatchDevice, BatcherEvents, DrawArgs, IbScratch, MaterialBatcher, SubmitContext, SubmitInput,
    VbListing, VertexSource, VertexStream,
};
use neuron_client::render::material::{
    GlobalRecords, GroupId, InstanceId, Material, MaterialAnimations, MaterialFactory, MaterialId,
    MaterialLibrary, MaterialRegex,
};
use neuron_core::hash::name_crc32;
use neuron_core::ini::{EngineIni, NoIncludes};
use neuron_core::material_data::{MaDelta, MaKey};
use neuron_core::vfs::FileSystem;

use super::{fixtures, hex, input_lines, unhex};

const UNTOUCHED: u32 = 0xCDCD_CDCD;

/// The types the fake factory makes as plain materials.
pub const NORMAL_TYPES: [&str; 9] = [
    "DcDt",
    "DcDtTwo",
    "DcDtOcOt",
    "DcDtEcOcOt",
    "BtDetailMapMaterial",
    "NebulaTwo",
    "NullMaterial",
    "HighGlassMaterial",
    "Plain",
];

fn int(text: &str) -> i64 {
    text.parse::<i64>()
        .unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn hex32(text: &str) -> u32 {
    u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn bits(text: &str) -> f32 {
    f32::from_bits(hex32(text))
}

fn floats_hex(values: &[f32]) -> String {
    values
        .iter()
        .map(|v| format!("{:08x}", v.to_bits()))
        .collect::<Vec<_>>()
        .join(",")
}

// --- material_regex ------------------------------------------------------------------------

/// `material_regex.list`: a pattern and its subjects (hex, comma-separated)
/// per line; the program, the search hints and a match flag per subject.
pub fn regex_lines() -> Vec<String> {
    input_lines("material_regex.list")
        .iter()
        .enumerate()
        .map(|(case, line)| {
            let (pattern, subjects) = line.split_once(' ').unwrap_or((line, ""));
            let pattern = unhex(pattern);
            let regex = MaterialRegex::compile(&pattern);
            let matches: String = subjects
                .split(',')
                .map(|s| {
                    let text = unhex(s);
                    if regex.as_ref().is_some_and(|r| r.is_match(&text)) {
                        '1'
                    } else {
                        '0'
                    }
                })
                .collect();
            match regex {
                Some(r) => {
                    let must = r
                        .must()
                        .map_or_else(|| "-".to_owned(), |(o, l)| format!("{o},{l}"));
                    format!(
                        "{{\"case\":\"{case}\",\"prog\":\"{}\",\"start\":\"{:02x}\",\"anch\":\"{}\",\"must\":\"{must}\",\"match\":\"{matches}\"}}",
                        hex(r.program()),
                        r.start_character(),
                        u8::from(r.anchored()),
                    )
                }
                None => format!("{{\"case\":\"{case}\",\"prog\":\"-\",\"match\":\"{matches}\"}}"),
            }
        })
        .collect()
}

// --- the fake materials --------------------------------------------------------------------

/// What the fakes, the device and the events sink write to, in call
/// order: the lines are shared with the events sink.
#[derive(Debug, Default)]
pub struct Log {
    pub case: usize,
    pub lines: Rc<RefCell<Vec<String>>>,
    /// `GetPipelineState(7)`: the value written, `None` for a failing call.
    pub mode: Option<i32>,
}

impl Log {
    fn new(case: usize) -> Log {
        Log {
            case,
            lines: Rc::default(),
            mode: Some(1),
        }
    }

    fn event(&mut self, fields: &str) {
        push_event(&self.lines, self.case, fields);
    }

    fn drain(&self, out: &mut Vec<String>) {
        out.append(&mut self.lines.borrow_mut());
    }
}

fn push_event(lines: &RefCell<Vec<String>>, case: usize, fields: &str) {
    lines
        .borrow_mut()
        .push(format!("{{\"case\":\"{case}\",{fields}}}"));
}

struct Fake {
    serial: u32,
    kind: String,
    flags: u32,
    batched: bool,
}

fn context_fields(context: &SubmitContext<'_>, args: Option<&DrawArgs>) -> String {
    let mut fields = format!(
        "\"inst\":\"{}\",\"src\":\"{}\",\"pos\":\"{}\"",
        context.instance,
        context.vertices.map_or(0, |v| v.count),
        {
            let p = &context
                .vertices
                .expect("the batcher always has a source")
                .position;
            let len = p.count.wrapping_mul(p.stride) as usize;
            p.data
                .as_deref()
                .map_or_else(String::new, |d| hex(&d[..len.min(d.len())]))
        }
    );
    fields.push_str(&format!(
        ",\"vb\":\"{}\",\"ib\":\"{}\"",
        context.vb.map_or_else(
            || "-".to_owned(),
            |v| format!("{},{},{}", v.vertex_buffer, v.fvf, v.base_vertex)
        ),
        context.ib.map_or_else(
            || "-".to_owned(),
            |i| format!("{},{}", i.index_buffer, i.bias)
        ),
    ));
    if let Some(a) = args {
        let idx: Vec<String> = (0..a.index_span as usize)
            .map(|i| context.indices[a.ib_start as usize + i].to_string())
            .collect();
        fields.push_str(&format!(
            ",\"prim\":\"{}\",\"start\":\"{}\",\"count\":\"{}\",\"ibs\":\"{}\",\"span\":\"{}\",\"flags\":\"{}\",\"idx\":\"{}\"",
            a.prim_type,
            a.start,
            a.count,
            a.ib_start,
            a.index_span,
            a.flags,
            idx.join(",")
        ));
    }
    fields
}

impl Material<Log> for Fake {
    fn initialize(&mut self, log: &mut Log) -> i32 {
        log.event(&format!("\"ev\":\"init\",\"mat\":\"{}\"", self.serial));
        if self.kind == "BadInit" { -1 } else { 0 }
    }

    fn set_name(&mut self, _name: &[u8]) -> i32 {
        0
    }

    fn load(&mut self, _log: &mut Log, _fs: &mut dyn FileSystem) -> i32 {
        -1
    }

    fn apply_verify(&mut self, log: &mut Log) -> i32 {
        log.event(&format!("\"ev\":\"verify\",\"mat\":\"{}\"", self.serial));
        0
    }

    fn resolve_textures(&mut self, log: &mut Log, delta: f32) -> i32 {
        log.event(&format!(
            "\"ev\":\"resolve\",\"mat\":\"{}\",\"d\":\"{:08x}\"",
            self.serial,
            delta.to_bits()
        ));
        0
    }

    fn render_flags(&mut self, log: &mut Log) -> u32 {
        log.event(&format!("\"ev\":\"flags\",\"mat\":\"{}\"", self.serial));
        self.flags
    }

    fn is_batched(&self) -> bool {
        self.batched
    }

    fn create_inner_material(
        &mut self,
        log: &mut Log,
        _factory: &mut dyn MaterialFactory<Log>,
        type_name: &[u8],
    ) -> i32 {
        log.event(&format!(
            "\"ev\":\"inner\",\"mat\":\"{}\",\"type\":\"{}\"",
            self.serial,
            hex(type_name)
        ));
        if NORMAL_TYPES.iter().any(|t| t.as_bytes() == type_name) {
            0
        } else {
            -1
        }
    }

    fn submit(&mut self, log: &mut Log, context: &SubmitContext<'_>, args: &DrawArgs) -> i32 {
        let fields = context_fields(context, Some(args));
        log.event(&format!(
            "\"ev\":\"submit\",\"mat\":\"{}\",{fields}",
            self.serial
        ));
        0
    }

    fn batch_bind(&mut self, log: &mut Log, context: &SubmitContext<'_>) -> i32 {
        let fields = context_fields(context, None);
        log.event(&format!(
            "\"ev\":\"bind\",\"mat\":\"{}\",{fields}",
            self.serial
        ));
        0
    }

    fn batch_teardown(&mut self, log: &mut Log, context: &SubmitContext<'_>) -> i32 {
        let fields = context_fields(context, None);
        log.event(&format!(
            "\"ev\":\"teardown\",\"mat\":\"{}\",{fields}",
            self.serial
        ));
        0
    }

    fn batch_submit(&mut self, log: &mut Log, context: &SubmitContext<'_>, args: &DrawArgs) -> i32 {
        let fields = context_fields(context, Some(args));
        log.event(&format!(
            "\"ev\":\"bsubmit\",\"mat\":\"{}\",{fields}",
            self.serial
        ));
        0
    }

    fn destroy(&mut self, log: &mut Log) {
        log.event(&format!("\"ev\":\"destroy\",\"mat\":\"{}\"", self.serial));
    }
}

/// The serials of a case's materials, counted from 1.
#[derive(Default)]
struct Registry {
    next: Cell<u32>,
}

impl Registry {
    fn make(&self, kind: &str, flags: u32, batched: bool) -> Box<Fake> {
        let serial = self.next.get() + 1;
        self.next.set(serial);
        Box::new(Fake {
            serial,
            kind: kind.to_owned(),
            flags,
            batched,
        })
    }

    /// The serial of the material made last.
    fn last(&self) -> u32 {
        self.next.get()
    }
}

struct FakeFactory(Rc<Registry>);

impl MaterialFactory<Log> for FakeFactory {
    fn create(&mut self, _log: &mut Log, type_name: &[u8]) -> Result<Box<dyn Material<Log>>, i32> {
        let kind = String::from_utf8_lossy(type_name).into_owned();
        if NORMAL_TYPES.contains(&kind.as_str()) || kind == "BadInit" {
            return Ok(self.0.make(&kind, 0, false));
        }
        if kind == "BatchedMaterial" {
            return Ok(self.0.make(&kind, 0, true));
        }
        // `NotMaterial` (a component without the material face) and names
        // the manager does not know.
        Err(-3)
    }
}

/// Material handles by serial. A handle the library entered without the
/// test seeing it is the material the registry made last.
#[derive(Default)]
struct Handles {
    by_serial: BTreeMap<u32, MaterialId>,
}

impl Handles {
    fn note(&mut self, material: Option<MaterialId>, registry: &Registry) {
        if let Some(id) = material
            && self.serial(id).is_none()
        {
            self.by_serial.insert(registry.last(), id);
        }
    }

    fn serial(&self, id: MaterialId) -> Option<u32> {
        self.by_serial
            .iter()
            .find(|&(_, &v)| v == id)
            .map(|(&s, _)| s)
    }

    fn id(&self, serial: &str) -> MaterialId {
        let serial: u32 = serial
            .trim_start_matches('#')
            .parse()
            .unwrap_or_else(|e| panic!("{serial}: {e}"));
        *self
            .by_serial
            .get(&serial)
            .unwrap_or_else(|| panic!("no material {serial}"))
    }
}

fn load_ini(name: &str) -> EngineIni {
    let path = fixtures().join(name);
    let bytes = std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let mut ini = EngineIni::default();
    ini.load_named(name.as_bytes(), &bytes, &mut NoIncludes)
        .unwrap_or_else(|e| panic!("{name}: {e}"));
    ini
}

// --- material_library ----------------------------------------------------------------------

struct LibraryCase {
    case: usize,
    log: Log,
    registry: Rc<Registry>,
    handles: Handles,
    library: Option<MaterialLibrary<Log>>,
    batcher: Option<MaterialBatcher<Log>>,
    ini: Option<EngineIni>,
}

impl LibraryCase {
    fn lib(&mut self) -> &mut MaterialLibrary<Log> {
        match (&mut self.library, &mut self.batcher) {
            (Some(l), _) => l,
            (None, Some(b)) => b.library_mut(),
            (None, None) => panic!("no library"),
        }
    }

    fn mat_text(&self, material: Option<MaterialId>) -> String {
        match material {
            None => "-".to_owned(),
            Some(id) => self
                .handles
                .serial(id)
                .map_or_else(|| "?".to_owned(), |s| s.to_string()),
        }
    }

    fn mat_refs(&mut self, material: Option<MaterialId>) -> String {
        match material {
            None => "-".to_owned(),
            Some(id) => self.lib().materials().refs(id).unwrap_or(0).to_string(),
        }
    }

    fn line(&self, op: usize, result: i32, extra: &str) -> String {
        format!(
            "{{\"case\":\"{}\",\"op\":\"{op}\",\"r\":\"{result}\"{extra}}}",
            self.case
        )
    }

    /// A material result: the serial and its reference count.
    fn mat_fields(&mut self, material: Option<MaterialId>) -> String {
        format!(
            ",\"mat\":\"{}\",\"mrefs\":\"{}\"",
            self.mat_text(material),
            self.mat_refs(material)
        )
    }

    fn dump(&mut self, out: &mut Vec<String>) {
        let case = self.case;
        let entries: Vec<(u32, u32, Option<MaterialId>)> = self
            .lib()
            .map()
            .iter()
            .map(|(&crc, e)| (crc, e.refs, e.material))
            .collect();
        for (crc, refs, material) in entries {
            let fields = self.mat_fields(material);
            out.push(format!(
                "{{\"case\":\"{case}\",\"crc\":\"{crc:08x}\",\"refs\":\"{refs}\"{fields}}}"
            ));
        }
        let lib = self.lib();
        let mut lines: Vec<String> = lib
            .groups()
            .iter()
            .map(|(&id, g)| {
                let members: Vec<String> = g.members.iter().map(|m| format!("{m:08x}")).collect();
                format!(
                    "{{\"case\":\"{case}\",\"group\":\"{id}\",\"gid\":\"{}\",\"name\":\"{}\",\"members\":\"{}\"}}",
                    g.id,
                    g.name.as_deref().map_or_else(|| "-".to_owned(), hex),
                    members.join(",")
                )
            })
            .collect();
        lines.extend(lib.rules().iter().enumerate().map(|(i, r)| {
            format!(
                "{{\"case\":\"{case}\",\"rule\":\"{i}\",\"key\":\"{}\",\"name\":\"{}\",\"pat\":\"{}\",\"type\":\"{}\",\"ok\":\"{}\"}}",
                r.key,
                u8::from(r.name_pattern),
                hex(&r.pattern),
                hex(&r.type_name),
                u8::from(r.expression.is_some())
            )
        }));
        out.extend(lines);
        if let Some(batcher) = &self.batcher {
            out.extend(batch_rule_lines(case, batcher));
        }
    }

    fn batcher(&mut self) -> &mut MaterialBatcher<Log> {
        self.batcher
            .as_mut()
            .unwrap_or_else(|| panic!("no batcher"))
    }

    /// Runs one operation; `None` for one that writes no line of its own.
    fn run(&mut self, op: usize, f: &[&str], out: &mut Vec<String>) -> Option<String> {
        let crc = |name: &str| name_crc32(name.as_bytes());
        let opt = |text: &str| (text != "-").then(|| text.as_bytes().to_vec());
        let mut log = Log {
            case: self.case,
            lines: Rc::clone(&self.log.lines),
            mode: Some(1),
        };
        let line = match f[0] {
            "library" => {
                let factory = Box::new(FakeFactory(Rc::clone(&self.registry)));
                self.library = Some(MaterialLibrary::new(factory));
                self.line(op, 0, "")
            }
            "batcher" => {
                let factory = Box::new(FakeFactory(Rc::clone(&self.registry)));
                self.batcher = Some(MaterialBatcher::new(factory, self.ini.as_ref()));
                self.line(op, 0, "")
            }
            "ini" => {
                self.ini = Some(load_ini(f[1]));
                self.line(op, 0, "")
            }
            "parsemap" => {
                let ini = self.ini.take().unwrap_or_else(|| panic!("no ini"));
                if let Some(section) = ini.section(b"MaterialMap") {
                    self.lib().parse_material_map(section);
                }
                self.ini = Some(ini);
                self.line(op, 0, "")
            }
            "rule" => {
                let r = self
                    .lib()
                    .add_map_rule(int(f[1]) as i32, f[2].as_bytes(), f[3].as_bytes());
                self.line(op, r, "")
            }
            "findrule" | "remap" => {
                let result = if f[0] == "findrule" {
                    self.lib().find_map_rule(int(f[1]) as i32, f[2].as_bytes())
                } else {
                    self.lib().remap_type(f[1].as_bytes(), f[2].as_bytes())
                };
                let (r, text) = match result {
                    Ok(t) => (0, hex(&t)),
                    Err(r) => (r, String::new()),
                };
                self.line(op, r, &format!(",\"type\":\"{text}\""))
            }
            "unrule" => {
                let r = self
                    .lib()
                    .remove_map_rule(int(f[1]) as i32, f[2].as_bytes());
                self.line(op, r, "")
            }
            "resolve" => {
                let ty = opt(f[2]);
                let text = hex(self.lib().resolve_type(f[1].as_bytes(), ty.as_deref()));
                self.line(op, 0, &format!(",\"type\":\"{text}\""))
            }
            "brule" => {
                let r = self.batcher().add_rule(int(f[1]) as u32, f[2].as_bytes());
                self.line(op, r, "")
            }
            "bunrule" => {
                let r = self
                    .batcher()
                    .remove_rule(int(f[1]) as u32, f[2].as_bytes());
                self.line(op, r, "")
            }
            "bclear" => {
                let r = self.batcher().clear_rules();
                self.line(op, r, "")
            }
            "create" | "generic" => {
                let remap = int(f[3]) != 0;
                let (name, ty) = (f[1].as_bytes(), f[2].as_bytes());
                let record = if f[0] == "create" {
                    self.lib().create_after_map(&mut log, name, ty, remap)
                } else {
                    self.lib().create_generic_record(&mut log, name, ty, remap)
                };
                let text = match record {
                    Some(crc) => {
                        let material = self.lib().entry(crc).and_then(|e| e.material);
                        self.handles.note(material, &self.registry);
                        format!(",\"rec\":\"{crc:08x}\"{}", self.mat_fields(material))
                    }
                    None => ",\"rec\":\"-\"".to_owned(),
                };
                self.line(op, 0, &text)
            }
            "foc" => match self
                .lib()
                .find_or_create(&mut log, f[1].as_bytes(), f[2].as_bytes())
            {
                Ok(m) => {
                    self.handles.note(m, &self.registry);
                    let fields = self.mat_fields(m);
                    self.line(op, 0, &fields)
                }
                Err(r) => self.line(op, r, ",\"mat\":\"-\""),
            },
            "find" => match self.lib().find_material_by_crc(crc(f[1])) {
                Ok(m) => {
                    let fields = self.mat_fields(m);
                    self.line(op, 0, &fields)
                }
                Err(r) => self.line(op, r, ",\"mat\":\"-\""),
            },
            "enumm" => match self
                .lib()
                .enum_group_material(int(f[1]) as u32, int(f[2]) as u32)
            {
                Ok(m) => {
                    let fields = self.mat_fields(m);
                    self.line(op, 0, &fields)
                }
                Err(r) => self.line(op, r, ",\"mat\":\"-\""),
            },
            "group" => {
                let id = self.lib().allocate_group();
                self.line(op, 0, &format!(",\"v\":\"{id}\""))
            }
            "gdel" => {
                let r = self.lib().delete_group(&mut log, int(f[1]) as i32);
                self.line(op, r, "")
            }
            "gname" => {
                let name = opt(f[2]);
                let r = self.lib().set_group_name(int(f[1]) as i32, name.as_deref());
                self.line(op, r, "")
            }
            "getname" => {
                let (r, text) = match self.lib().group_name(int(f[1]) as i32, int(f[2]) as u32) {
                    Ok(t) => (0, hex(&t)),
                    Err(r) => (r, String::new()),
                };
                self.line(op, r, &format!(",\"text\":\"{text}\""))
            }
            "gcount" => {
                let v = self.lib().group_count();
                self.line(op, 0, &format!(",\"v\":\"{v}\""))
            }
            "enumg" => {
                let (r, v) = match self.lib().enum_group(int(f[1]) as u32) {
                    Ok(v) => (0, v),
                    Err(r) => (r, UNTOUCHED),
                };
                self.line(op, r, &format!(",\"v\":\"{v}\""))
            }
            "mcount" => {
                let (r, v) = match self.lib().group_material_count(int(f[1]) as u32) {
                    Ok(v) => (0, v),
                    Err(r) => (r, -7),
                };
                self.line(op, r, &format!(",\"v\":\"{v}\""))
            }
            "add" => {
                let material = match f[3] {
                    "-" => None,
                    spec if spec.starts_with("new:") => {
                        let fake = self.registry.make(&spec[4..], 0, false);
                        let id = self.lib().materials_mut().insert(fake, 1);
                        self.handles.note(Some(id), &self.registry);
                        Some(id)
                    }
                    spec => Some(self.handles.id(spec)),
                };
                let r = self
                    .lib()
                    .add_material(int(f[1]) as i32, crc(f[2]), material);
                self.line(op, r, "")
            }
            "remove" => {
                let r = self
                    .lib()
                    .remove_material(&mut log, int(f[1]) as u32, crc(f[2]));
                self.line(op, r, "")
            }
            "verify" => {
                let r = self.lib().apply_verify_group(&mut log, int(f[1]) as i32);
                self.line(op, r, "")
            }
            "resolvet" => {
                let r = self
                    .lib()
                    .resolve_textures_group(&mut log, int(f[1]) as i32, bits(f[2]));
                self.line(op, r, "")
            }
            "release" | "addref" => {
                let id = self.handles.id(f[1]);
                let v = if f[0] == "release" {
                    self.lib().materials_mut().release(id, &mut log)
                } else {
                    self.lib().materials_mut().add_ref(id)
                };
                self.line(op, 0, &format!(",\"v\":\"{v}\""))
            }
            "unsup" => {
                let r = self.lib().unsupported_operation();
                self.line(op, r, "")
            }
            "dump" => {
                self.log.drain(out);
                self.dump(out);
                return None;
            }
            other => panic!("material_library: unknown operation {other}"),
        };
        Some(line)
    }
}

/// `material_library.list`.
pub fn library_lines() -> Vec<String> {
    let mut out = Vec::new();
    for (case, line) in input_lines("material_library.list").iter().enumerate() {
        let mut runner = LibraryCase {
            case,
            log: Log::new(case),
            registry: Rc::new(Registry::default()),
            handles: Handles::default(),
            library: None,
            batcher: None,
            ini: None,
        };
        for (op, text) in line.split(" ; ").enumerate() {
            let fields: Vec<&str> = text.split(' ').collect();
            let line = runner.run(op, &fields, &mut out);
            runner.log.drain(&mut out);
            out.extend(line);
        }
    }
    out
}

fn batch_rule_lines(case: usize, batcher: &MaterialBatcher<Log>) -> Vec<String> {
    batcher
        .rules()
        .iter()
        .enumerate()
        .map(|(i, r)| {
            format!(
                "{{\"case\":\"{case}\",\"brule\":\"{i}\",\"kind\":\"{}\",\"pat\":\"{}\",\"ok\":\"{}\"}}",
                r.kind,
                hex(&r.pattern),
                u8::from(r.expression.is_some())
            )
        })
        .collect()
}

// --- material_global -----------------------------------------------------------------------

/// `material_global.list`: one script over the process-wide records.
pub fn global_lines() -> Vec<String> {
    let mut records = GlobalRecords::new();
    let mut out = Vec::new();
    for (case, line) in input_lines("material_global.list").iter().enumerate() {
        for (op, text) in line.split(" ; ").enumerate() {
            let f: Vec<&str> = text.split(' ').collect();
            let head = format!("{{\"case\":\"{case}\",\"op\":\"{op}\"");
            let index = || int(f[1]) as u32;
            let value = || int(f[2]) as u32;
            out.push(match f[0] {
                "set" => {
                    records.set_record(index(), value());
                    format!("{head},\"r\":\"0\"}}")
                }
                "setv" => {
                    records.set_value(index(), value());
                    format!("{head},\"r\":\"0\"}}")
                }
                "get" => {
                    let v = records.value(index()).unwrap_or(UNTOUCHED);
                    format!("{head},\"r\":\"0\",\"v\":\"{v}\"}}")
                }
                "value" => {
                    let v = records.value(index()).unwrap_or(UNTOUCHED);
                    format!("{head},\"v\":\"{v}\"}}")
                }
                "act" => {
                    records.activate(index());
                    format!("{head}}}")
                }
                "init" => {
                    records.initialize();
                    format!("{head}}}")
                }
                other => panic!("material_global: unknown operation {other}"),
            });
        }
    }
    out
}

// --- material_animation --------------------------------------------------------------------

fn anim_data(spec: &str) -> (String, AnimationData) {
    let parts: Vec<&str> = spec.split(':').collect();
    let name = parts[0].to_owned();
    let floats = |text: &str| -> Vec<u32> {
        if text.is_empty() {
            Vec::new()
        } else {
            text.split('/').map(hex32).collect()
        }
    };
    let deltas = floats(parts[3]);
    let keys = floats(parts[4]);
    let data = AnimationData {
        crc: name_crc32(name.as_bytes()),
        count: int(parts[1]) as u32,
        flags: int(parts[2]) as u32,
        deltas: deltas
            .chunks(5)
            .map(|c| MaDelta {
                duration: c[0],
                rates: [c[1], c[2], c[3], c[4]],
            })
            .collect(),
        keys: keys
            .chunks(4)
            .map(|c| MaKey {
                values: [c[0], c[1], c[2], c[3]],
            })
            .collect(),
    };
    (name, data)
}

/// `material_animation.list`.
pub fn animation_lines() -> Vec<String> {
    let mut out = Vec::new();
    for (case, line) in input_lines("material_animation.list").iter().enumerate() {
        let mut library = MaterialAnimations::new();
        let mut groups: Vec<Option<GroupId>> = Vec::new();
        let mut instances: Vec<InstanceId> = Vec::new();
        let mut tags: Vec<(Weak<AnimationData>, String)> = Vec::new();
        for (op, text) in line.split(" ; ").enumerate() {
            let f: Vec<&str> = text.split(' ').collect();
            let head = format!("{{\"case\":\"{case}\",\"op\":\"{op}\"");
            let tag_of = |data: &Rc<AnimationData>, tags: &[(Weak<AnimationData>, String)]| {
                tags.iter()
                    .find(|(w, _)| w.upgrade().is_some_and(|d| Rc::ptr_eq(&d, data)))
                    .map_or_else(|| "?".to_owned(), |(_, t)| t.clone())
            };
            let inst = |text: &str| instances[int(text) as usize];
            match f[0] {
                "group" => {
                    let specs: Vec<(String, AnimationData)> = f[1]
                        .split(',')
                        .filter(|s| !s.is_empty())
                        .map(anim_data)
                        .collect();
                    let names: Vec<String> = specs.iter().map(|(n, _)| n.clone()).collect();
                    let id = library.add_group(specs.into_iter().map(|(_, d)| d).collect());
                    let g = groups.len();
                    groups.push(id);
                    if let Some(id) = id {
                        let entries = library.group(id).unwrap_or_default();
                        for (index, (_, data)) in entries.iter().enumerate() {
                            tags.push((Rc::downgrade(data), format!("{g}.{index}")));
                        }
                    }
                    let _ = names;
                    out.push(format!(
                        "{head},\"v\":\"{}\"}}",
                        if id.is_some() {
                            g.to_string()
                        } else {
                            "-".to_owned()
                        }
                    ));
                }
                "unload" => {
                    let r = match groups[int(f[1]) as usize] {
                        Some(id) => library.unload_group(id),
                        None => 0,
                    };
                    out.push(format!("{head},\"r\":\"{r}\"}}"));
                }
                "inst" => match library.create_instance(name_crc32(f[1].as_bytes())) {
                    Ok(id) => {
                        instances.push(id);
                        out.push(format!(
                            "{head},\"r\":\"0\",\"v\":\"{}\"}}",
                            instances.len() - 1
                        ));
                    }
                    Err(r) => out.push(format!("{head},\"r\":\"{r}\",\"v\":\"-\"}}")),
                },
                "release" => {
                    let v = library.release(inst(f[1]));
                    out.push(format!("{head},\"v\":\"{v}\"}}"));
                }
                "addref" => {
                    let v = library.add_ref(inst(f[1]));
                    out.push(format!("{head},\"v\":\"{v}\"}}"));
                }
                "tick" => {
                    let r = library.tick_instances(int(f[1]) as i32, bits(f[2]));
                    out.push(format!("{head},\"r\":\"{r}\"}}"));
                }
                "itick" => {
                    if let Some(i) = library.instance_mut(inst(f[1])) {
                        i.tick(bits(f[2]));
                    }
                    out.push(format!("{head}}}"));
                }
                "reset" => {
                    if let Some(i) = library.instance_mut(inst(f[1])) {
                        i.reset_time();
                    }
                    out.push(format!("{head}}}"));
                }
                "uv" => {
                    let uv = library
                        .instance(inst(f[1]))
                        .and_then(|i| i.uv_matrix())
                        .unwrap_or_else(|| panic!("uv of a missing instance"));
                    out.push(format!(
                        "{head},\"r\":\"1\",\"m\":\"{}\"}}",
                        floats_hex(&uv)
                    ));
                }
                "state" => {
                    let i = library
                        .instance(inst(f[1]))
                        .unwrap_or_else(|| panic!("state of a missing instance"));
                    let data = i.data().unwrap_or_else(|| panic!("no data"));
                    out.push(format!(
                        "{head},\"frame\":\"{}\",\"time\":\"{:08x}\",\"refs\":\"{}\",\"data\":\"{}\",\"drefs\":\"{}\"}}",
                        i.frame,
                        i.time.to_bits(),
                        i.refs(),
                        tag_of(data, &tags),
                        Rc::strong_count(data)
                    ));
                }
                "map" => {
                    for (crc, data) in library.map() {
                        out.push(format!(
                            "{{\"case\":\"{case}\",\"crc\":\"{crc:08x}\",\"data\":\"{}\",\"drefs\":\"{}\"}}",
                            tag_of(data, &tags),
                            Rc::strong_count(data)
                        ));
                    }
                }
                other => panic!("material_animation: unknown operation {other}"),
            }
        }
    }
    out
}

// --- material_batcher ----------------------------------------------------------------------

impl BatchDevice for Log {
    fn select_vb(&mut self, vertex_buffer: u32) -> i32 {
        self.event(&format!("\"ev\":\"selvb\",\"h\":\"{vertex_buffer}\""));
        if vertex_buffer >= 100 { -1 } else { 0 }
    }

    fn select_ib(&mut self, index_buffer: u32, base_vertex: u32) -> i32 {
        self.event(&format!(
            "\"ev\":\"selib\",\"h\":\"{index_buffer}\",\"base\":\"{base_vertex}\""
        ));
        if index_buffer >= 100 { -1 } else { 0 }
    }

    fn pipeline_state(&mut self, id: u32) -> Option<i32> {
        self.event(&format!("\"ev\":\"state\",\"id\":\"{id}\""));
        self.mode
    }

    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        self.event(&format!(
            "\"ev\":\"modelview\",\"m\":\"{}\"",
            floats_hex(matrix)
        ));
    }

    fn set_world(&mut self, matrix: &[f32; 12]) {
        self.event(&format!(
            "\"ev\":\"world\",\"m\":\"{}\"",
            floats_hex(matrix)
        ));
    }

    fn set_view(&mut self, matrix: &[f32; 12]) {
        self.event(&format!("\"ev\":\"view\",\"m\":\"{}\"", floats_hex(matrix)));
    }
}

/// The events sink (`IMaterialBatcherEvents`), writing into the case's
/// shared lines.
struct Events {
    case: usize,
    lines: Rc<RefCell<Vec<String>>>,
    serials: Rc<RefCell<BTreeMap<MaterialId, u32>>>,
}

impl Events {
    fn push(&self, fields: &str) {
        push_event(&self.lines, self.case, fields);
    }

    fn serial(&self, material: MaterialId) -> u32 {
        self.serials.borrow().get(&material).copied().unwrap_or(0)
    }
}

impl BatcherEvents for Events {
    fn begin_flush(&mut self, clear: u8) {
        self.push(&format!("\"ev\":\"bflush\",\"clear\":\"{clear}\""));
    }

    fn end_flush(&mut self, clear: u8) {
        self.push(&format!("\"ev\":\"eflush\",\"clear\":\"{clear}\""));
    }

    fn begin_record(&mut self, material: MaterialId, tag: usize, tag_context: usize) {
        let serial = self.serial(material);
        self.push(&format!(
            "\"ev\":\"brec\",\"mat\":\"{serial}\",\"tag\":\"{tag}\",\"ctx\":\"{tag_context}\""
        ));
    }

    fn end_record(&mut self, material: MaterialId, tag: usize, tag_context: usize) {
        let serial = self.serial(material);
        self.push(&format!(
            "\"ev\":\"erec\",\"mat\":\"{serial}\",\"tag\":\"{tag}\",\"ctx\":\"{tag_context}\""
        ));
    }
}

fn matrix(text: &str) -> [f32; 12] {
    let values: Vec<f32> = text.split(',').map(bits).collect();
    core::array::from_fn(|i| values[i])
}

fn numbers(text: &str) -> Vec<u32> {
    text.split(',').map(|v| int(v) as u32).collect()
}

/// The state of one `material_batcher.list` case.
struct BatcherCase {
    case: usize,
    log: Log,
    registry: Rc<Registry>,
    handles: Handles,
    serials: Rc<RefCell<BTreeMap<MaterialId, u32>>>,
    batcher: Option<MaterialBatcher<Log>>,
    sources: BTreeMap<String, Rc<VertexSource>>,
    indices: BTreeMap<String, Rc<[u16]>>,
    matrices: BTreeMap<String, [f32; 12]>,
    submissions: usize,
}

impl BatcherCase {
    fn batcher(&mut self) -> &mut MaterialBatcher<Log> {
        self.batcher
            .as_mut()
            .unwrap_or_else(|| panic!("no batcher"))
    }

    fn note(&mut self, material: Option<MaterialId>) {
        self.handles.note(material, &self.registry);
        if let Some(id) = material
            && let Some(serial) = self.handles.serial(id)
        {
            self.serials.borrow_mut().insert(id, serial);
        }
    }

    fn state_lines(&mut self, out: &mut Vec<String>) {
        let case = self.case;
        let batcher = self
            .batcher
            .as_ref()
            .unwrap_or_else(|| panic!("no batcher"));
        for index in 0..2 {
            let bin = batcher.bin(index).unwrap_or_else(|| panic!("bin {index}"));
            let queued: Vec<String> = bin
                .queued
                .iter()
                .map(|(m, n)| format!("{}:{n}", self.handles.serial(*m).unwrap_or(0)))
                .collect();
            out.push(format!(
                "{{\"case\":\"{case}\",\"bin\":\"{index}\",\"key\":\"{}\",\"sort\":\"{}\",\"records\":\"{}\",\"queue\":\"{}\"}}",
                bin.order_key,
                bin.sort_kind,
                bin.record_count,
                queued.join(",")
            ));
        }
        let order = batcher.bin_order();
        out.push(format!(
            "{{\"case\":\"{case}\",\"order\":\"{},{}\",\"selected\":\"{}\",\"tag\":\"{}\",\"tagctx\":\"{}\"}}",
            order[0] as i32,
            order[1] as i32,
            batcher.selected_bin(),
            batcher.record_tag(),
            batcher.record_tag_context()
        ));
        out.extend(batch_rule_lines(case, batcher));
    }

    /// Runs one operation; `None` for one that writes no line of its own.
    fn run(&mut self, f: &[&str], out: &mut Vec<String>) -> Option<String> {
        let mut log = Log {
            case: self.case,
            lines: Rc::clone(&self.log.lines),
            mode: self.log.mode,
        };
        let n = |i: usize| int(f[i]);
        let r = match f[0] {
            "startup" => {
                let ini = load_ini(f[1]);
                let factory = Box::new(FakeFactory(Rc::clone(&self.registry)));
                let batcher = MaterialBatcher::new(factory, Some(&ini));
                let config = batcher.config().clone();
                self.batcher = Some(batcher);
                // A new batcher has a new arena: its handles start again.
                self.handles = Handles::default();
                self.serials.borrow_mut().clear();
                return Some(format!(
                    ",\"r\":\"0\",\"size\":\"{}\",\"bin1\":\"{}\"",
                    config.buffer_size, config.bin1_sort_order
                ));
            }
            "mat" => {
                let fake = self.registry.make("Q", hex32(f[1]), n(2) != 0);
                let serial = fake.serial;
                let id = self.batcher().library_mut().materials_mut().insert(fake, 1);
                self.note(Some(id));
                return Some(format!(",\"v\":\"{serial}\""));
            }
            "create" => {
                let record = self.batcher().library_mut().create_after_map(
                    &mut log,
                    f[1].as_bytes(),
                    f[2].as_bytes(),
                    true,
                );
                let Some(crc) = record else {
                    return Some(",\"rec\":\"-\"".to_owned());
                };
                let material = self.batcher().library().entry(crc).and_then(|e| e.material);
                self.note(material);
                let serial = material
                    .and_then(|id| self.handles.serial(id))
                    .map_or_else(|| "-".to_owned(), |s| s.to_string());
                return Some(format!(",\"rec\":\"{crc:08x}\",\"mat\":\"{serial}\""));
            }
            "vsrc" => {
                let data: Vec<u8> = f[4]
                    .split(',')
                    .flat_map(|t| hex32(t).to_le_bytes())
                    .collect();
                let count = n(2) as u32;
                let source = VertexSource {
                    flags: 0,
                    position: VertexStream {
                        data: Some(data),
                        byte_size: 12,
                        stride: n(3) as u32,
                        index: f.get(5).map(|t| numbers(t)),
                        count,
                    },
                    fvf: 2,
                    count,
                    ..VertexSource::default()
                };
                self.sources.insert(f[1].to_owned(), Rc::new(source));
                return None;
            }
            "idx" => {
                let values: Vec<u16> = numbers(f[2]).into_iter().map(|v| v as u16).collect();
                self.indices.insert(f[1].to_owned(), Rc::from(values));
                return None;
            }
            "xf" => {
                self.matrices.insert(f[1].to_owned(), matrix(f[2]));
                return None;
            }
            "mode" => {
                self.log.mode = if f[1] == "none" {
                    None
                } else {
                    Some(n(1) as i32)
                };
                return None;
            }
            "events" => {
                let events: Option<Box<dyn BatcherEvents>> = (f[1] == "on").then(|| {
                    Box::new(Events {
                        case: self.case,
                        lines: Rc::clone(&self.log.lines),
                        serials: Rc::clone(&self.serials),
                    }) as Box<dyn BatcherEvents>
                });
                self.batcher().set_events(events)
            }
            "sub" => {
                // mat prim start count ibs span flags depth vsrc idx world view vb ib
                self.submissions += 1;
                let vb = numbers(f[13]);
                let ib = numbers(f[14]);
                let input = SubmitInput {
                    instance: self.submissions,
                    world: self.matrices[f[11]],
                    view: self.matrices[f[12]],
                    vertices: Rc::clone(&self.sources[f[9]]),
                    indices: Rc::clone(&self.indices[f[10]]),
                    vb: VbListing {
                        vertex_buffer: vb[0],
                        fvf: vb[1],
                        base_vertex: vb[2],
                    },
                    ib: IbScratch {
                        index_buffer: ib[0],
                        bias: ib[1],
                    },
                    replaced: None,
                    field20: 0,
                };
                let args = DrawArgs {
                    prim_type: n(2) as u32,
                    start: n(3) as u32,
                    count: n(4) as u32,
                    ib_start: n(5) as u32,
                    index_span: n(6) as u32,
                    flags: hex32(f[7]),
                };
                let material = self.handles.id(f[1]);
                self.batcher()
                    .submit(&mut log, material, &input, &args, hex32(f[8]))
            }
            "rule" => self.batcher().add_rule(n(1) as u32, f[2].as_bytes()),
            "unrule" => self.batcher().remove_rule(n(1) as u32, f[2].as_bytes()),
            "clearrules" => self.batcher().clear_rules(),
            "select" => self.batcher().select_bin(n(1) as u32),
            "sort" => self.batcher().set_bin_sort_kind(n(1) as u32, n(2) as u32),
            "order" => self.batcher().set_bin_order(n(1) as u32, n(2) as i32),
            "flushbin" => self.batcher().flush_bin(&mut log, n(1) as u32, n(2) as u8),
            "flush" => self.batcher().flush_all(&mut log, n(1) as u8),
            "clearbins" => self.batcher().clear_bins(),
            "noop" => self.batcher().no_op(),
            "tag" => self.batcher().set_record_tag(n(1) as usize),
            "tagctx" => self.batcher().set_record_tag_context(n(1) as usize),
            "bins" => {
                self.log.drain(out);
                self.state_lines(out);
                return None;
            }
            other => panic!("material_batcher: unknown operation {other}"),
        };
        Some(format!(",\"r\":\"{r}\""))
    }
}

/// `material_batcher.list`.
pub fn batcher_lines() -> Vec<String> {
    let mut out = Vec::new();
    for (case, line) in input_lines("material_batcher.list").iter().enumerate() {
        let mut runner = BatcherCase {
            case,
            log: Log::new(case),
            registry: Rc::new(Registry::default()),
            handles: Handles::default(),
            serials: Rc::default(),
            batcher: None,
            sources: BTreeMap::new(),
            indices: BTreeMap::new(),
            matrices: BTreeMap::new(),
            submissions: 0,
        };
        for (op, text) in line.split(" ; ").enumerate() {
            let fields: Vec<&str> = text.split(' ').collect();
            let tail = runner.run(&fields, &mut out);
            runner.log.drain(&mut out);
            if let Some(tail) = tail {
                out.push(format!("{{\"case\":\"{case}\",\"op\":\"{op}\"{tail}}}"));
            }
        }
    }
    out
}
