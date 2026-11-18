//! P4.5: the 22 material kinds and `BatchedMaterial` over the recording
//! services `CppSource/Oracle/oracle_material_kinds.cpp` implements (its
//! header states the contract and the line formats), on
//! `tests/fixtures/oracle/material_kinds.list`.

use std::collections::{BTreeMap, VecDeque};

use neuron_client::render::material::batcher::{
    BatchDevice, DrawArgs, IbScratch, SubmitContext, VbListing, VertexSource, VertexStream,
};
use neuron_client::render::material::global::UNSET;
use neuron_client::render::material::kinds::fvf_stride;
use neuron_client::render::material::{
    BatchedMaterial, GlobalRecords, Material, MaterialServices, ShadingMaterial, VertexLock,
};
use neuron_client::render::texture::FrameOut;
use neuron_core::hash::name_crc32;
use neuron_core::material_data::{MaterialKind, PropertyValue};

use super::{hex, input_lines, unhex};

fn dec(text: &str) -> i64 {
    text.parse::<i64>()
        .unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn hex_words(text: &str) -> Vec<u32> {
    if text == "-" {
        return Vec::new();
    }
    text.split(',')
        .map(|w| u32::from_str_radix(w, 16).unwrap_or_else(|e| panic!("{w}: {e}")))
        .collect()
}

fn words(values: &[u32]) -> String {
    values
        .iter()
        .map(|v| format!("{v:08x}"))
        .collect::<Vec<_>>()
        .join(",")
}

fn float_words(values: &[f32]) -> String {
    words(&values.iter().map(|v| v.to_bits()).collect::<Vec<_>>())
}

/// The C string a hex field spells.
fn text(field: &str) -> Vec<u8> {
    let bytes = unhex(field);
    bytes[..bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len())].to_vec()
}

struct Texture {
    name: Vec<u8>,
    crc: u32,
    frames: i32,
    handle: u32,
    format: u32,
    play_fail: bool,
}

struct Anim {
    crc: u32,
    fills: bool,
}

/// The recording services.
pub struct Rec {
    case: usize,
    lines: Vec<String>,
    render_states: BTreeMap<u32, u32>,
    verify_queue: VecDeque<i32>,
    mode: Option<i32>,
    rpdraw: i32,
    dvb: i32,
    lock_fail: bool,
    next_base: u32,
    anim_library: bool,
    textures: Vec<Texture>,
    anims: Vec<Anim>,
    playbacks: BTreeMap<i32, usize>,
    next_playback: i32,
    next_anim: i32,
    instances: BTreeMap<i32, bool>,
    globals: GlobalRecords,
}

impl Rec {
    fn new(case: usize) -> Rec {
        Rec {
            case,
            lines: Vec::new(),
            render_states: BTreeMap::new(),
            verify_queue: VecDeque::new(),
            mode: Some(1),
            rpdraw: 0,
            dvb: 0,
            lock_fail: false,
            next_base: 50,
            anim_library: false,
            textures: Vec::new(),
            anims: Vec::new(),
            playbacks: BTreeMap::new(),
            next_playback: 1,
            next_anim: 1,
            instances: BTreeMap::new(),
            globals: GlobalRecords::registered(),
        }
    }

    fn ev(&mut self, fields: &str) {
        self.lines
            .push(format!("{{\"case\":\"{}\",{fields}}}", self.case));
    }

    fn op(&mut self, fields: &str) {
        self.lines
            .push(format!("{{\"case\":\"{}\",\"op\":{fields}}}", self.case));
    }

    fn texture_by_handle(&self, handle: u32) -> Option<&Texture> {
        self.textures.iter().find(|t| t.handle == handle)
    }
}

impl BatchDevice for Rec {
    fn select_vb(&mut self, _: u32) -> i32 {
        -1
    }

    fn select_ib(&mut self, _: u32, _: u32) -> i32 {
        -1
    }

    fn pipeline_state(&mut self, id: u32) -> Option<i32> {
        match self.mode {
            Some(v) => {
                self.ev(&format!("\"ev\":\"state\",\"id\":\"{id}\",\"v\":\"{v}\""));
                Some(v)
            }
            None => {
                self.ev(&format!("\"ev\":\"state\",\"id\":\"{id}\",\"v\":\"-\""));
                None
            }
        }
    }

    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        self.ev(&format!(
            "\"ev\":\"modelview\",\"m\":\"{}\"",
            float_words(matrix)
        ));
    }

    fn set_world(&mut self, matrix: &[f32; 12]) {
        self.ev(&format!(
            "\"ev\":\"world\",\"m\":\"{}\"",
            float_words(matrix)
        ));
    }

    fn set_view(&mut self, _: &[f32; 12]) {}
}

impl MaterialServices for Rec {
    type TextureRecord = usize;
    type Animation = i32;

    fn set_material(&mut self, block: &[u32; 17]) {
        self.ev(&format!("\"ev\":\"mat\",\"m\":\"{}\"", words(block)));
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        self.render_states.insert(state, value);
        self.ev(&format!(
            "\"ev\":\"rs\",\"s\":\"{state}\",\"v\":\"{value}\""
        ));
    }

    fn render_state(&mut self, state: u32) -> u32 {
        let value = self.render_states.get(&state).copied().unwrap_or(0);
        self.ev(&format!(
            "\"ev\":\"getrs\",\"s\":\"{state}\",\"v\":\"{value}\""
        ));
        value
    }

    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        self.ev(&format!(
            "\"ev\":\"tss\",\"st\":\"{stage}\",\"t\":\"{state}\",\"v\":\"{value}\""
        ));
    }

    fn set_stage_texture(&mut self, stage: u32, handle: u32) {
        self.ev(&format!(
            "\"ev\":\"tex\",\"st\":\"{stage}\",\"h\":\"{handle}\""
        ));
    }

    fn set_texture_transform(&mut self, stage: u32, matrix: &[f32; 16]) {
        self.ev(&format!(
            "\"ev\":\"txf\",\"st\":\"{stage}\",\"m\":\"{}\"",
            float_words(matrix)
        ));
    }

    fn texture_format(&mut self, handle: u32) -> Result<u32, i32> {
        match self.texture_by_handle(handle).map(|t| t.format) {
            Some(format) => {
                self.ev(&format!(
                    "\"ev\":\"fmt\",\"h\":\"{handle}\",\"r\":\"0\",\"f\":\"{format}\""
                ));
                Ok(format)
            }
            None => {
                self.ev(&format!(
                    "\"ev\":\"fmt\",\"h\":\"{handle}\",\"r\":\"-1\",\"f\":\"-\""
                ));
                Err(-1)
            }
        }
    }

    fn verify_state(&mut self) -> i32 {
        let result = self.verify_queue.pop_front().unwrap_or(0);
        self.ev(&format!("\"ev\":\"verify\",\"r\":\"{result}\""));
        result
    }

    fn draw_indexed_vb(
        &mut self,
        prim: u32,
        vertex_buffer: u32,
        base_vertex: u32,
        vertex_count: u32,
        indices: &[u16],
    ) -> i32 {
        let list = indices
            .iter()
            .map(u16::to_string)
            .collect::<Vec<_>>()
            .join(",");
        let result = self.dvb;
        self.ev(&format!(
            "\"ev\":\"dvb\",\"prim\":\"{prim}\",\"vb\":\"{vertex_buffer}\",\"base\":\"{base_vertex}\",\"n\":\"{vertex_count}\",\"idx\":\"{list}\",\"r\":\"{result}\""
        ));
        result
    }

    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> i32 {
        let result = self.rpdraw;
        self.ev(&format!(
            "\"ev\":\"rpdraw\",\"prim\":\"{prim}\",\"min\":\"{min_index}\",\"n\":\"{vertex_count}\",\"start\":\"{start_index}\",\"count\":\"{count}\",\"r\":\"{result}\""
        ));
        result
    }

    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        if self.lock_fail {
            self.ev(&format!(
                "\"ev\":\"lock\",\"fvf\":\"{fvf}\",\"n\":\"{count}\",\"r\":\"-1\""
            ));
            return Err(-1);
        }
        self.ev(&format!(
            "\"ev\":\"lock\",\"fvf\":\"{fvf}\",\"n\":\"{count}\",\"r\":\"0\""
        ));
        let base = self.next_base;
        self.next_base = self.next_base.wrapping_add(count);
        Ok(VertexLock {
            listing: VbListing {
                vertex_buffer: 900,
                fvf,
                base_vertex: base,
            },
            bytes: vec![0; (fvf_stride(fvf) * count) as usize],
            offset: 0,
        })
    }

    fn copy_vertex_data(
        &mut self,
        dest: &mut [u8],
        fvf: u32,
        _source: &VertexSource,
        start: u32,
        count: u32,
    ) {
        self.ev(&format!(
            "\"ev\":\"copy\",\"fvf\":\"{fvf}\",\"start\":\"{start}\",\"n\":\"{count}\""
        ));
        let stride = fvf_stride(fvf) as usize;
        for vertex in 0..count as usize {
            for byte in 0..stride {
                if let Some(slot) = dest.get_mut(vertex * stride + byte) {
                    *slot = (0x40 + 7 * vertex + byte) as u8;
                }
            }
        }
    }

    fn unlock_vertex_memory(&mut self, lock: VertexLock) {
        self.ev(&format!(
            "\"ev\":\"unlock\",\"base\":\"{}\",\"bytes\":\"{}\"",
            lock.listing.base_vertex,
            hex(&lock.bytes)
        ));
    }

    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<usize> {
        let found = self.textures.iter().position(|t| t.crc == crc);
        self.ev(&format!(
            "\"ev\":\"find\",\"name\":\"{}\",\"r\":\"{}\"",
            hex(name),
            if found.is_some() { 0 } else { -1 }
        ));
        found
    }

    fn texture_frame_count(&mut self, record: usize) -> Option<i32> {
        let (name, frames) = (
            hex(&self.textures[record].name),
            self.textures[record].frames,
        );
        self.ev(&format!(
            "\"ev\":\"frames\",\"name\":\"{name}\",\"n\":\"{frames}\""
        ));
        Some(frames)
    }

    fn create_playback(&mut self, record: usize) -> Result<i32, i32> {
        let name = hex(&self.textures[record].name);
        if self.textures[record].play_fail {
            self.ev(&format!(
                "\"ev\":\"play\",\"name\":\"{name}\",\"id\":\"-1\",\"r\":\"-1\""
            ));
            return Err(-1);
        }
        let id = self.next_playback;
        self.next_playback += 1;
        self.playbacks.insert(id, record);
        self.ev(&format!(
            "\"ev\":\"play\",\"name\":\"{name}\",\"id\":\"{id}\",\"r\":\"0\""
        ));
        Ok(id)
    }

    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<FrameOut> {
        let Some(&record) = self.playbacks.get(&id) else {
            self.ev(&format!(
                "\"ev\":\"frame\",\"id\":\"{id}\",\"f\":\"{frame}\",\"h\":\"-\",\"r\":\"-1\""
            ));
            return None;
        };
        let t = &self.textures[record];
        let handle = t.handle + if frame == -1 && t.frames > 1 { 100 } else { 0 };
        self.ev(&format!(
            "\"ev\":\"frame\",\"id\":\"{id}\",\"f\":\"{frame}\",\"h\":\"{handle}\",\"r\":\"0\""
        ));
        Some(FrameOut {
            handle,
            uv: [0x3E80_0000, 0, 0x3F80_0000, 0x3F00_0000],
        })
    }

    fn release_texture_record(&mut self, record: usize) {
        let name = hex(&self.textures[record].name);
        self.ev(&format!("\"ev\":\"trel\",\"name\":\"{name}\""));
    }

    fn dec_ref_texture(&mut self, id: i32) {
        self.ev(&format!("\"ev\":\"decref\",\"id\":\"{id}\""));
    }

    fn has_animation_library(&mut self) -> bool {
        let r = if self.anim_library { 0 } else { -1 };
        self.ev(&format!("\"ev\":\"qianim\",\"r\":\"{r}\""));
        self.anim_library
    }

    fn create_animation_instance(&mut self, name: Option<&[u8]>, crc: u32) -> Option<i32> {
        let name_hex = name.map_or_else(|| "-".to_owned(), hex);
        let Some(fills) = self.anims.iter().find(|a| a.crc == crc).map(|a| a.fills) else {
            self.ev(&format!(
                "\"ev\":\"anim\",\"name\":\"{name_hex}\",\"crc\":\"{crc:08x}\",\"id\":\"-\",\"r\":\"-1\""
            ));
            return None;
        };
        let id = self.next_anim;
        self.next_anim += 1;
        self.instances.insert(id, fills);
        self.ev(&format!(
            "\"ev\":\"anim\",\"name\":\"{name_hex}\",\"crc\":\"{crc:08x}\",\"id\":\"{id}\",\"r\":\"0\""
        ));
        Some(id)
    }

    fn release_animation_instance(&mut self, instance: i32) {
        self.ev(&format!("\"ev\":\"animrel\",\"id\":\"{instance}\""));
    }

    fn animation_uv_matrix(&mut self, instance: i32) -> Option<[f32; 16]> {
        let fills = self.instances.get(&instance).copied().unwrap_or(false);
        self.ev(&format!(
            "\"ev\":\"uv\",\"id\":\"{instance}\",\"r\":\"{}\"",
            u8::from(fills)
        ));
        fills.then(|| {
            let mut m = [0.0f32; 16];
            m[0] = 1.0;
            m[5] = 1.0;
            m[10] = 1.0;
            m[15] = 1.0;
            m[8] = 0.25;
            m[9] = -0.5;
            m
        })
    }

    fn instance_extent(&mut self, _: usize, _: usize) -> Option<(f32, [f32; 3])> {
        panic!("the suite gives no engine");
    }

    fn global_record(&mut self, index: u32) -> u32 {
        self.globals.value(index).unwrap_or(UNSET)
    }
}

struct ContextDef {
    source: Option<String>,
    indices: String,
    vb: Option<VbListing>,
    ib: Option<IbScratch>,
    world: String,
    view: String,
    instance: usize,
}

enum Current {
    None,
    Plain(Box<ShadingMaterial<i32>>),
    Batched(BatchedMaterial<Rec>),
}

impl Current {
    fn material(&mut self) -> &mut dyn Material<Rec> {
        match self {
            Current::Plain(m) => &mut **m,
            Current::Batched(m) => m,
            Current::None => panic!("no material"),
        }
    }
}

struct Case {
    rec: Rec,
    current: Current,
    sources: BTreeMap<String, VertexSource>,
    indices: BTreeMap<String, Vec<u16>>,
    matrices: BTreeMap<String, [f32; 12]>,
    contexts: BTreeMap<String, ContextDef>,
}

fn stream(data: &[u32], size: u32, index: &[u32], count: u32) -> VertexStream {
    if data.is_empty() {
        return VertexStream::default();
    }
    VertexStream {
        data: Some(data.iter().flat_map(|w| w.to_le_bytes()).collect()),
        byte_size: size,
        stride: size,
        index: (!index.is_empty()).then(|| index.to_vec()),
        count,
    }
}

fn param_text(value: &PropertyValue) -> String {
    match value.kind {
        5 | 7 => value
            .text
            .as_deref()
            .map_or_else(|| "s-".to_owned(), |t| format!("s{}", hex(t))),
        3 => words(&value.words[..3]),
        4 => words(&value.words),
        _ => words(&value.words[..1]),
    }
}

impl Case {
    fn args(f: &[&str]) -> DrawArgs {
        DrawArgs {
            prim_type: dec(f[2]) as u32,
            start: dec(f[3]) as u32,
            count: dec(f[4]) as u32,
            ib_start: dec(f[5]) as u32,
            index_span: dec(f[6]) as u32,
            flags: u32::from_str_radix(f[7], 16).expect("flags"),
        }
    }

    #[allow(clippy::too_many_lines)]
    fn run(&mut self, f: &[&str]) {
        let rec = &mut self.rec;
        match (f[0], f.len()) {
            ("globals", _) => {
                for index in 0..3 {
                    rec.globals
                        .set_record(index, dec(f[1 + index as usize]) as u32);
                }
            }
            ("tex", _) => {
                let name = text(f[1]);
                rec.textures.push(Texture {
                    crc: name_crc32(&name),
                    name,
                    frames: dec(f[2]) as i32,
                    handle: dec(f[3]) as u32,
                    format: dec(f[4]) as u32,
                    play_fail: dec(f[5]) != 0,
                });
            }
            ("animlib", _) => rec.anim_library = true,
            ("anim", _) => {
                let name = text(f[1]);
                rec.anims.push(Anim {
                    crc: name_crc32(&name),
                    fills: dec(f[2]) != 0,
                });
            }
            ("rs", _) => {
                rec.render_states.insert(dec(f[1]) as u32, dec(f[2]) as u32);
            }
            ("mode", _) => rec.mode = (f[1] != "none").then(|| dec(f[1]) as i32),
            ("rpdraw", _) => rec.rpdraw = dec(f[1]) as i32,
            ("dvb", _) => rec.dvb = dec(f[1]) as i32,
            ("lockfail", _) => rec.lock_fail = dec(f[1]) != 0,
            ("verify", 2) => {
                rec.verify_queue = f[1].split(',').map(|v| dec(v) as i32).collect();
            }
            ("vsrc", _) => {
                let count = dec(f[4]) as u32;
                let index: Vec<u32> = if f[9] == "-" {
                    Vec::new()
                } else {
                    f[9].split(',').map(|v| dec(v) as u32).collect()
                };
                let source = VertexSource {
                    flags: dec(f[2]) as u32,
                    position: stream(&hex_words(f[5]), 12, &index, count),
                    normal: stream(&hex_words(f[6]), 12, &index, count),
                    diffuse: stream(&hex_words(f[7]), 4, &index, count),
                    tex: [
                        stream(&hex_words(f[8]), 8, &index, count),
                        VertexStream::default(),
                    ],
                    fvf: dec(f[3]) as u32,
                    count,
                };
                self.sources.insert(f[1].to_owned(), source);
            }
            ("idx", _) => {
                let list = f[2].split(',').map(|v| dec(v) as u16).collect();
                self.indices.insert(f[1].to_owned(), list);
            }
            ("xf", _) => {
                let w = hex_words(f[2]);
                let mut m = [0.0f32; 12];
                for (slot, word) in m.iter_mut().zip(w) {
                    *slot = f32::from_bits(word);
                }
                self.matrices.insert(f[1].to_owned(), m);
            }
            ("ctx", _) => {
                let parts =
                    |text: &str| -> Vec<u32> { text.split(',').map(|v| dec(v) as u32).collect() };
                let def = ContextDef {
                    source: (f[2] != "-").then(|| f[2].to_owned()),
                    indices: f[3].to_owned(),
                    vb: (f[4] != "-").then(|| {
                        let p = parts(f[4]);
                        VbListing {
                            vertex_buffer: p[0],
                            fvf: p[1],
                            base_vertex: p[2],
                        }
                    }),
                    ib: (f[5] != "-").then(|| {
                        let p = parts(f[5]);
                        IbScratch {
                            index_buffer: p[0],
                            bias: p[1],
                        }
                    }),
                    world: f[6].to_owned(),
                    view: f[7].to_owned(),
                    instance: dec(f[8]) as usize,
                };
                self.contexts.insert(f[1].to_owned(), def);
            }
            ("new", _) => {
                let kind = MaterialKind::by_name(f[1].as_bytes()).expect("a kind");
                let mut material = ShadingMaterial::new(kind);
                let r = Material::<Rec>::initialize(&mut material, rec);
                self.current = Current::Plain(Box::new(material));
                rec.op(&format!("\"new\",\"kind\":\"{}\",\"r\":\"{r}\"", f[1]));
            }
            ("batched", _) => {
                let kind = MaterialKind::by_name(f[1].as_bytes()).expect("a kind");
                let mut material =
                    BatchedMaterial::with_inner(Box::new(ShadingMaterial::<i32>::new(kind)));
                let r = material.initialize(rec);
                self.current = Current::Batched(material);
                rec.op(&format!("\"batched\",\"kind\":\"{}\",\"r\":\"{r}\"", f[1]));
            }
            ("name", _) => {
                let r = self.current.material().set_name(&text(f[1]));
                rec.op(&format!("\"name\",\"r\":\"{r}\""));
            }
            ("set", _) => {
                let kind = dec(f[2]) as u32;
                let mut value = PropertyValue {
                    kind,
                    words: [0; 4],
                    text: None,
                };
                if kind == 5 || kind == 7 {
                    value.text = (f[3] != "-").then(|| text(f[3]));
                } else {
                    for (slot, word) in value.words.iter_mut().zip(hex_words(f[3])) {
                        *slot = word;
                    }
                }
                let index = dec(f[1]) as u32;
                let r = self.current.material().set_parameter(rec, index, &value);
                rec.op(&format!("\"set\",\"i\":\"{index}\",\"r\":\"{r}\""));
            }
            ("flags", _) => {
                let line = match self.current.material().flags(rec) {
                    Ok(v) => format!("\"flags\",\"r\":\"0\",\"v\":\"{v:08x}\""),
                    Err(r) => format!("\"flags\",\"r\":\"{r}\",\"v\":\"cdcdcdcd\""),
                };
                rec.op(&line);
            }
            ("verify", 1) => {
                let r = self.current.material().apply_verify(rec);
                rec.op(&format!("\"verify\",\"r\":\"{r}\""));
            }
            ("state", _) => {
                let Current::Plain(material) = &self.current else {
                    panic!("state of a plain material only");
                };
                let s = material.state();
                let tex = s
                    .textures
                    .iter()
                    .map(|t| format!("{}:{}:{}", t.id, t.frame.handle, u8::from(t.skip_resolve)))
                    .collect::<Vec<_>>()
                    .join(",");
                rec.op(&format!(
                    "\"state\",\"blend\":\"{}\",\"test\":\"{}\",\"ready\":\"{}\",\"passes\":\"{}\",\"second\":\"{}\",\"multi\":\"{}\",\"sen\":\"{}\",\"sver\":\"{}\",\"anim\":\"{}\",\"tex\":\"{tex}\"",
                    u8::from(s.alpha_blend),
                    u8::from(s.alpha_test),
                    u8::from(s.atmosphere_ready),
                    s.detail_pass_count,
                    u8::from(s.detail_second_stage_verified),
                    u8::from(s.detail_multi_pass_enabled),
                    u8::from(s.secondary_enabled),
                    u8::from(s.secondary_verified),
                    u8::from(s.animated),
                ));
            }
            ("resolve", _) => {
                let r = self.current.material().resolve_textures(rec, 0.0);
                rec.op(&format!("\"resolve\",\"r\":\"{r}\""));
            }
            ("bind" | "teardown", _) => {
                let context = Self::context_of(
                    &self.contexts,
                    &self.sources,
                    &self.indices,
                    &self.matrices,
                    f[1],
                );
                let m = self.current.material();
                let r = if f[0] == "bind" {
                    m.bind(rec, context.as_ref())
                } else {
                    m.teardown(rec, context.as_ref())
                };
                rec.op(&format!("\"{}\",\"r\":\"{r}\"", f[0]));
            }
            ("submit" | "bsubmit", _) => {
                let context = Self::context_of(
                    &self.contexts,
                    &self.sources,
                    &self.indices,
                    &self.matrices,
                    f[1],
                )
                .expect("a context");
                let args = Self::args(f);
                let m = self.current.material();
                let r = match m.batch_route(&context, &args) {
                    None => m.submit(rec, &context, &args),
                    Some(Err(r)) => r,
                    Some(Ok(route)) => {
                        rec.ev(&format!("\"ev\":\"selbin\",\"bin\":\"{}\"", route.bin));
                        rec.ev(&format!(
                            "\"ev\":\"bsubmit\",\"prim\":\"{}\",\"start\":\"{}\",\"count\":\"{}\",\"ibs\":\"{}\",\"span\":\"{}\",\"depth\":\"{:08x}\",\"flags\":\"{:x}\"",
                            args.prim_type,
                            args.start,
                            args.count,
                            args.ib_start,
                            args.index_span,
                            route.depth_bits,
                            route.flags
                        ));
                        0
                    }
                };
                rec.op(&format!("\"{}\",\"r\":\"{r}\"", f[0]));
            }
            ("passes", _) => {
                let line = match self.current.material().pass_count() {
                    Ok(v) => format!("\"passes\",\"r\":\"0\",\"v\":\"{v}\""),
                    Err(r) => format!("\"passes\",\"r\":\"{r}\",\"v\":\"-7\""),
                };
                rec.op(&line);
            }
            ("params", _) => {
                let m = self.current.material();
                let count = m.parameter_count().unwrap_or(0);
                let mut lines = Vec::new();
                for index in 0..count {
                    lines.push(match m.parameter(index) {
                        Ok(value) => format!(
                            "\"param\",\"i\":\"{index}\",\"r\":\"0\",\"t\":\"{}\",\"v\":\"{}\"",
                            value.kind,
                            param_text(&value)
                        ),
                        Err(r) => {
                            format!(
                                "\"param\",\"i\":\"{index}\",\"r\":\"{r}\",\"t\":\"0\",\"v\":\"-\""
                            )
                        }
                    });
                }
                for line in lines {
                    rec.op(&line);
                }
            }
            ("bin", _) => {
                let Current::Batched(m) = &self.current else {
                    panic!("bin of a batched material only");
                };
                rec.op(&format!("\"bin\",\"v\":\"{}\"", m.batch_bin()));
            }
            ("setbin", _) => {
                let Current::Batched(m) = &mut self.current else {
                    panic!("setbin of a batched material only");
                };
                m.set_batch_bin(dec(f[1]) as u32);
                rec.op("\"setbin\"");
            }
            ("release", _) => {
                self.current.material().destroy(rec);
                self.current = Current::None;
                rec.op("\"release\"");
            }
            (other, _) => panic!("material_kinds: unknown operation {other}"),
        }
    }

    fn context_of<'a>(
        contexts: &'a BTreeMap<String, ContextDef>,
        sources: &'a BTreeMap<String, VertexSource>,
        indices: &'a BTreeMap<String, Vec<u16>>,
        matrices: &'a BTreeMap<String, [f32; 12]>,
        name: &str,
    ) -> Option<SubmitContext<'a>> {
        if name == "-" {
            return None;
        }
        let def = &contexts[name];
        Some(SubmitContext {
            instance: def.instance,
            world: &matrices[&def.world],
            view: &matrices[&def.view],
            vertices: def.source.as_ref().map(|s| &sources[s]),
            indices: &indices[&def.indices],
            vb: def.vb.as_ref(),
            ib: def.ib.as_ref(),
            replaced: None,
            field20: 0,
        })
    }
}

/// `material_kinds.list` through the recording services.
pub fn lines() -> Vec<String> {
    let mut out = Vec::new();
    for (index, line) in input_lines("material_kinds.list").iter().enumerate() {
        let mut case = Case {
            rec: Rec::new(index),
            current: Current::None,
            sources: BTreeMap::new(),
            indices: BTreeMap::new(),
            matrices: BTreeMap::new(),
            contexts: BTreeMap::new(),
        };
        for op in line.split(" ; ") {
            let fields: Vec<&str> = op.split(' ').collect();
            case.run(&fields);
        }
        out.append(&mut case.rec.lines);
    }
    out
}
