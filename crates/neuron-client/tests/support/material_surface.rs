//! P4.5: the 19 surface materials over the recording services
//! `CppSource/Oracle/oracle_material_surface.cpp` implements (its header
//! states the contract and the line formats), on
//! `tests/fixtures/oracle/material_surface.list`.

use std::collections::{BTreeMap, VecDeque};

use neuron_client::render::material::batcher::{
    BatchDevice, DrawArgs, IbScratch, SubmitContext, VbListing, VertexSource, VertexStream,
};
use neuron_client::render::material::global::UNSET;
use neuron_client::render::material::kinds::fvf_stride;
use neuron_client::render::material::surface::{
    MeshIndices, SurfaceMaterial, SurfaceServices, SurfaceSpec,
};
use neuron_client::render::material::{
    GlobalRecords, Material, MaterialId, MaterialServices, Materials, VertexLock,
};
use neuron_client::render::texture::FrameOut;
use neuron_core::hash::name_crc32;
use neuron_core::material_data::PropertyValue;

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

fn shorts(text: &str) -> Vec<u16> {
    if text == "-" {
        return Vec::new();
    }
    text.split(',').map(|v| dec(v) as u16).collect()
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

#[derive(Default)]
struct Mesh {
    channel_result: i32,
    values: Vec<u32>,
    value_indices: Vec<u16>,
    positions: Vec<u32>,
    position_indices: Vec<u16>,
    count: u32,
}

#[derive(Default)]
struct Candidate {
    batched: bool,
    passes: Option<i32>,
    class: Vec<u8>,
    desc_result: i32,
    tag: Option<Vec<u8>>,
    param: u32,
    bind: i32,
    teardown: i32,
    submit: i32,
}

/// The recording services.
pub struct Rec {
    case: usize,
    lines: Vec<String>,
    render_states: BTreeMap<u32, u32>,
    stage_states: BTreeMap<(u32, u32), u32>,
    last_material: Option<[u32; 17]>,
    verify_queue: VecDeque<i32>,
    rpdraw: i32,
    dvb: i32,
    dpvb: i32,
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
    engine: bool,
    user_data: BTreeMap<usize, Vec<u32>>,
    roots: BTreeMap<usize, usize>,
    values: BTreeMap<i32, u32>,
    names: BTreeMap<i32, Vec<u8>>,
    wires: BTreeMap<usize, bool>,
    meshes: BTreeMap<usize, Mesh>,
    open_queue: VecDeque<i32>,
    candidate: Candidate,
    counters: [u32; 3],
}

impl Rec {
    fn new(case: usize) -> Rec {
        Rec {
            case,
            lines: Vec::new(),
            render_states: BTreeMap::new(),
            stage_states: BTreeMap::new(),
            last_material: None,
            verify_queue: VecDeque::new(),
            rpdraw: 0,
            dvb: 0,
            dpvb: 0,
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
            engine: false,
            user_data: BTreeMap::new(),
            roots: BTreeMap::new(),
            values: BTreeMap::new(),
            names: BTreeMap::new(),
            wires: BTreeMap::new(),
            meshes: BTreeMap::new(),
            open_queue: VecDeque::new(),
            candidate: Candidate::default(),
            counters: [0; 3],
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

    fn lookup(&mut self, id: i32) -> bool {
        let found = id != -1 && (self.values.contains_key(&id) || self.names.contains_key(&id));
        self.ev(&format!(
            "\"ev\":\"ival\",\"id\":\"{id}\",\"r\":\"{}\"",
            if found { 0 } else { -1 }
        ));
        found
    }

    fn face(batched: bool) -> &'static str {
        if batched { "b" } else { "m" }
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
        self.ev(&format!("\"ev\":\"state\",\"id\":\"{id}\",\"v\":\"1\""));
        Some(1)
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
        self.last_material = Some(*block);
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
        self.stage_states.insert((stage, state), value);
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
        panic!("no surface material asks for an extent");
    }

    fn global_record(&mut self, index: u32) -> u32 {
        self.globals.value(index).unwrap_or(UNSET)
    }
}

impl SurfaceServices for Rec {
    fn material(&mut self) -> Option<[u32; 17]> {
        match self.last_material {
            Some(block) => {
                self.ev(&format!(
                    "\"ev\":\"getmat\",\"r\":\"0\",\"m\":\"{}\"",
                    words(&block)
                ));
                Some(block)
            }
            None => {
                self.ev("\"ev\":\"getmat\",\"r\":\"-1\",\"m\":\"-\"");
                None
            }
        }
    }

    fn stage_state(&mut self, stage: u32, state: u32) -> Option<u32> {
        match self.stage_states.get(&(stage, state)).copied() {
            Some(value) => {
                self.ev(&format!(
                    "\"ev\":\"gettss\",\"st\":\"{stage}\",\"t\":\"{state}\",\"r\":\"0\",\"v\":\"{value}\""
                ));
                Some(value)
            }
            None => {
                self.ev(&format!(
                    "\"ev\":\"gettss\",\"st\":\"{stage}\",\"t\":\"{state}\",\"r\":\"-1\",\"v\":\"-\""
                ));
                None
            }
        }
    }

    fn draw_vb(&mut self, prim: u32, vertex_buffer: u32, start: u32, count: u32) -> i32 {
        let r = self.dpvb;
        self.ev(&format!(
            "\"ev\":\"dpvb\",\"prim\":\"{prim}\",\"vb\":\"{vertex_buffer}\",\"start\":\"{start}\",\"n\":\"{count}\",\"r\":\"{r}\""
        ));
        r
    }

    fn record_frame(&mut self, record: usize, frame: i32) -> Option<FrameOut> {
        let (name, handle) = (
            hex(&self.textures[record].name),
            self.textures[record].handle,
        );
        self.ev(&format!(
            "\"ev\":\"tframe\",\"name\":\"{name}\",\"f\":\"{frame}\",\"h\":\"{handle}\",\"r\":\"0\""
        ));
        Some(FrameOut {
            handle,
            uv: [0x3E80_0000, 0, 0x3F80_0000, 0x3F00_0000],
        })
    }

    fn has_engine(&mut self) -> bool {
        self.engine
    }

    fn instance_user_data(&mut self, instance: usize) -> Option<[u32; 5]> {
        match self.user_data.get(&instance).cloned() {
            Some(values) => {
                self.ev(&format!(
                    "\"ev\":\"ud\",\"inst\":\"{instance}\",\"v\":\"{}\"",
                    words(&values[..5])
                ));
                Some([values[0], values[1], values[2], values[3], values[4]])
            }
            None => {
                self.ev(&format!(
                    "\"ev\":\"ud\",\"inst\":\"{instance}\",\"v\":\"-\""
                ));
                None
            }
        }
    }

    fn instance_root(&mut self, instance: usize) -> usize {
        let root = self.roots.get(&instance).copied().unwrap_or(instance);
        self.ev(&format!(
            "\"ev\":\"root\",\"inst\":\"{instance}\",\"r\":\"{root}\""
        ));
        root
    }

    fn instance_relative(&mut self, instance: usize) {
        self.ev(&format!("\"ev\":\"rel\",\"inst\":\"{instance}\""));
    }

    fn query_wire(&mut self, instance: usize) -> bool {
        let found = self.wires.get(&instance).copied().unwrap_or(false);
        self.ev(&format!(
            "\"ev\":\"qinst\",\"inst\":\"{instance}\",\"iid\":\"wire\",\"r\":\"{}\"",
            if found { 0 } else { -1 }
        ));
        found
    }

    fn wire_draw(&mut self, instance: usize) {
        self.ev(&format!("\"ev\":\"wdraw\",\"inst\":\"{instance}\""));
    }

    fn query_mesh(&mut self, instance: usize) -> bool {
        let found = self.meshes.contains_key(&instance);
        self.ev(&format!(
            "\"ev\":\"qinst\",\"inst\":\"{instance}\",\"iid\":\"mesh\",\"r\":\"{}\"",
            if found { 0 } else { -1 }
        ));
        found
    }

    fn mesh_open_face_group(&mut self, instance: usize, index: i32, mode: i32) -> i32 {
        let r = self.open_queue.pop_front().unwrap_or(0);
        self.ev(&format!(
            "\"ev\":\"mopen\",\"inst\":\"{instance}\",\"g\":\"{index}\",\"m\":\"{mode}\",\"r\":\"{r}\""
        ));
        r
    }

    fn mesh_close_face_group(&mut self, instance: usize) {
        self.ev(&format!("\"ev\":\"mclose\",\"inst\":\"{instance}\""));
    }

    fn mesh_open_channels(&mut self, instance: usize, flags: u32) -> i32 {
        let r = self.meshes.entry(instance).or_default().channel_result;
        self.ev(&format!(
            "\"ev\":\"mchan\",\"inst\":\"{instance}\",\"f\":\"{flags}\",\"r\":\"{r}\""
        ));
        r
    }

    fn mesh_lock_channels(&mut self, instance: usize, mask: u32) {
        self.ev(&format!(
            "\"ev\":\"mlock\",\"inst\":\"{instance}\",\"m\":\"{mask}\""
        ));
    }

    fn mesh_unlock_channels(&mut self, instance: usize) {
        self.ev(&format!("\"ev\":\"munlock\",\"inst\":\"{instance}\""));
    }

    fn mesh_close_channels(&mut self, instance: usize) {
        self.ev(&format!("\"ev\":\"mchanclose\",\"inst\":\"{instance}\""));
    }

    fn mesh_data(&mut self, instance: usize, channel: u32) -> Option<Vec<u32>> {
        self.ev(&format!(
            "\"ev\":\"mdata\",\"inst\":\"{instance}\",\"ch\":\"{channel}\""
        ));
        let mesh = self.meshes.entry(instance).or_default();
        Some(if channel == 0x100 {
            mesh.values.clone()
        } else {
            mesh.positions.clone()
        })
    }

    fn mesh_indices(&mut self, instance: usize, channel: u32) -> MeshIndices {
        let mesh = self.meshes.entry(instance).or_default();
        let (indices, count) = if channel == 0x100 {
            (mesh.value_indices.clone(), mesh.value_indices.len() as u32)
        } else {
            (mesh.position_indices.clone(), mesh.count)
        };
        self.ev(&format!(
            "\"ev\":\"midx\",\"inst\":\"{instance}\",\"ch\":\"{channel}\",\"n\":\"{count}\""
        ));
        MeshIndices {
            indices: Some(indices),
            count,
        }
    }

    fn instance_value(&mut self, id: i32) -> Option<u32> {
        if !self.lookup(id) {
            return None;
        }
        // A string value's word is its address, which no material reads as a number.
        self.values.get(&id).copied().or(Some(0))
    }

    fn instance_value_name(&mut self, id: i32) -> Option<Vec<u8>> {
        if !self.lookup(id) {
            return None;
        }
        self.names.get(&id).cloned()
    }

    fn count_visible(&mut self, index_span: u32, count: u32) {
        self.counters[0] += 1;
        self.counters[1] += index_span;
        self.counters[2] += count;
    }

    fn candidate_is_batched(&mut self, _: MaterialId) -> bool {
        let batched = self.candidate.batched;
        self.ev(&format!(
            "\"ev\":\"cqi\",\"r\":\"{}\"",
            if batched { 0 } else { -1 }
        ));
        batched
    }

    fn candidate_bind(&mut self, _: MaterialId, batched: bool, _: &SubmitContext<'_>) -> i32 {
        let r = self.candidate.bind;
        self.ev(&format!(
            "\"ev\":\"cbind\",\"face\":\"{}\",\"r\":\"{r}\"",
            Self::face(batched)
        ));
        r
    }

    fn candidate_teardown(&mut self, _: MaterialId, batched: bool, _: &SubmitContext<'_>) -> i32 {
        let r = self.candidate.teardown;
        self.ev(&format!(
            "\"ev\":\"cteardown\",\"face\":\"{}\",\"r\":\"{r}\"",
            Self::face(batched)
        ));
        r
    }

    fn candidate_submit(
        &mut self,
        _: MaterialId,
        batched: bool,
        _: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let r = self.candidate.submit;
        self.ev(&format!(
            "\"ev\":\"csubmit\",\"face\":\"{}\",\"prim\":\"{}\",\"start\":\"{}\",\"count\":\"{}\",\"ibs\":\"{}\",\"span\":\"{}\",\"flags\":\"{:x}\",\"r\":\"{r}\"",
            Self::face(batched),
            args.prim_type,
            args.start,
            args.count,
            args.ib_start,
            args.index_span,
            args.flags
        ));
        r
    }

    fn candidate_pass_count(&mut self, _: MaterialId) -> Option<i32> {
        let passes = self.candidate.passes;
        match passes {
            Some(v) => self.ev(&format!("\"ev\":\"cpasses\",\"v\":\"{v}\"")),
            None => self.ev("\"ev\":\"cpasses\",\"v\":\"-\""),
        }
        passes
    }

    fn candidate_class_name(&mut self, _: MaterialId) -> Vec<u8> {
        let class = self.candidate.class.clone();
        self.ev(&format!("\"ev\":\"cclass\",\"name\":\"{}\"", hex(&class)));
        class
    }

    fn candidate_parameter_tag(
        &mut self,
        _: MaterialId,
        index: u32,
    ) -> Result<Option<Vec<u8>>, i32> {
        let r = self.candidate.desc_result;
        let tag = self.candidate.tag.clone();
        self.ev(&format!(
            "\"ev\":\"cdesc\",\"i\":\"{index}\",\"r\":\"{r}\",\"tag\":\"{}\"",
            tag.as_deref().map_or_else(|| "-".to_owned(), hex)
        ));
        if r == 0 { Ok(tag) } else { Err(r) }
    }

    fn candidate_parameter_word(&mut self, _: MaterialId, index: u32) -> u32 {
        let v = self.candidate.param;
        self.ev(&format!(
            "\"ev\":\"cparam\",\"i\":\"{index}\",\"v\":\"{v:08x}\""
        ));
        v
    }
}

/// A material id for the candidate the contexts name (the services answer
/// for it, so it names no material).
fn candidate_id() -> MaterialId {
    let mut arena: Materials<Rec> = Materials::new();
    let spec = SurfaceSpec::by_name(b"VisibleCounterMaterial").expect("a factory");
    arena.insert(Box::new(SurfaceMaterial::<i32, usize>::new(spec)), 1)
}

struct ContextDef {
    source: Option<String>,
    indices: String,
    vb: Option<VbListing>,
    ib: Option<IbScratch>,
    world: String,
    view: String,
    instance: usize,
    candidate: bool,
}

struct Case {
    rec: Rec,
    current: Option<Box<SurfaceMaterial<i32, usize>>>,
    candidate: MaterialId,
    sources: BTreeMap<String, VertexSource>,
    indices: BTreeMap<String, Vec<u16>>,
    matrices: BTreeMap<String, [f32; 12]>,
    contexts: BTreeMap<String, ContextDef>,
}

fn stream(data: Option<Vec<u8>>, stride: u32, index: &[u32], count: u32) -> VertexStream {
    let Some(data) = data else {
        return VertexStream::default();
    };
    VertexStream {
        data: Some(data),
        byte_size: stride,
        stride,
        index: (!index.is_empty()).then(|| index.to_vec()),
        count,
    }
}

fn bytes_of(words: &[u32]) -> Option<Vec<u8>> {
    (!words.is_empty()).then(|| words.iter().flat_map(|w| w.to_le_bytes()).collect())
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

    fn material(&mut self) -> &mut SurfaceMaterial<i32, usize> {
        self.current.as_mut().expect("a material")
    }

    #[allow(clippy::too_many_lines)]
    fn run(&mut self, f: &[&str]) {
        let rec = &mut self.rec;
        match (f[0], f.len()) {
            ("global", _) => {
                rec.globals.set_record(dec(f[1]) as u32, dec(f[2]) as u32);
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
            ("tss", _) => {
                rec.stage_states
                    .insert((dec(f[1]) as u32, dec(f[2]) as u32), dec(f[3]) as u32);
            }
            ("rpdraw", _) => rec.rpdraw = dec(f[1]) as i32,
            ("dvb", _) => rec.dvb = dec(f[1]) as i32,
            ("dpvb", _) => rec.dpvb = dec(f[1]) as i32,
            ("lockfail", _) => rec.lock_fail = dec(f[1]) != 0,
            ("verify", 2) => {
                rec.verify_queue = f[1].split(',').map(|v| dec(v) as i32).collect();
            }
            ("engine", _) => rec.engine = dec(f[1]) != 0,
            ("ud", _) => {
                rec.user_data.insert(dec(f[1]) as usize, hex_words(f[2]));
            }
            ("root", _) => {
                rec.roots.insert(dec(f[1]) as usize, dec(f[2]) as usize);
            }
            ("ival", _) => {
                rec.values.insert(dec(f[1]) as i32, hex_words(f[2])[0]);
            }
            ("iname", _) => {
                rec.names.insert(dec(f[1]) as i32, text(f[2]));
            }
            ("wire", _) => {
                rec.wires.insert(dec(f[1]) as usize, dec(f[2]) != 0);
            }
            ("mesh", _) => {
                rec.meshes.insert(
                    dec(f[1]) as usize,
                    Mesh {
                        channel_result: dec(f[2]) as i32,
                        values: hex_words(f[3]),
                        value_indices: shorts(f[4]),
                        positions: hex_words(f[5]),
                        position_indices: shorts(f[6]),
                        count: dec(f[7]) as u32,
                    },
                );
            }
            ("mopen", _) => {
                rec.open_queue = f[1].split(',').map(|v| dec(v) as i32).collect();
            }
            ("cand", _) => {
                rec.candidate = Candidate {
                    batched: dec(f[1]) != 0,
                    passes: (f[2] != "-").then(|| dec(f[2]) as i32),
                    class: text(f[3]),
                    desc_result: dec(f[4]) as i32,
                    tag: (f[5] != "-").then(|| text(f[5])),
                    param: hex_words(f[6])[0],
                    bind: dec(f[7]) as i32,
                    teardown: dec(f[8]) as i32,
                    submit: dec(f[9]) as i32,
                };
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
                    position: stream(bytes_of(&hex_words(f[5])), 12, &index, count),
                    normal: stream(bytes_of(&hex_words(f[6])), 12, &index, count),
                    diffuse: stream(bytes_of(&hex_words(f[7])), 4, &index, count),
                    tex: [
                        stream(bytes_of(&hex_words(f[8])), 8, &index, count),
                        VertexStream::default(),
                    ],
                    fvf: dec(f[3]) as u32,
                    count,
                };
                self.sources.insert(f[1].to_owned(), source);
            }
            ("vsil", _) => {
                let count = dec(f[4]) as u32;
                let fvf = dec(f[3]) as u32;
                let data = bytes_of(&hex_words(f[5])).unwrap_or_default();
                let stride = fvf_stride(fvf);
                let from = |offset: usize| Some(data[offset.min(data.len())..].to_vec());
                let mut offset = 12;
                let mut source = VertexSource {
                    flags: dec(f[2]) as u32,
                    position: stream(from(0), stride, &[], count),
                    fvf,
                    count,
                    ..VertexSource::default()
                };
                if fvf & 0x10 != 0 {
                    source.normal = stream(from(offset), stride, &[], count);
                    offset += 12;
                }
                if fvf & 0x40 != 0 {
                    source.diffuse = stream(from(offset), stride, &[], count);
                    offset += 4;
                }
                source.tex[0] = stream(from(offset), stride, &[], count);
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
                    candidate: f[9] == "c",
                };
                self.contexts.insert(f[1].to_owned(), def);
            }
            ("new", _) => {
                let spec = SurfaceSpec::by_name(f[1].as_bytes()).expect("a surface material");
                let mut material = SurfaceMaterial::new(spec);
                let r = Material::<Rec>::initialize(&mut material, rec);
                self.current = Some(Box::new(material));
                rec.op(&format!(
                    "\"new\",\"kind\":\"{}\",\"made\":\"0\",\"r\":\"{r}\"",
                    f[1]
                ));
            }
            ("name", _) => {
                let name = text(f[1]);
                let r = Material::<Rec>::set_name(self.material(), &name);
                self.rec.op(&format!("\"name\",\"r\":\"{r}\""));
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
                let material = self.current.as_mut().expect("a material");
                let r = material.set_parameter(&mut self.rec, index, &value);
                self.rec
                    .op(&format!("\"set\",\"i\":\"{index}\",\"r\":\"{r}\""));
            }
            ("flags", _) => {
                let material = self.current.as_mut().expect("a material");
                let line = match Material::<Rec>::flags(&mut **material, &mut self.rec) {
                    Ok(v) => format!("\"flags\",\"r\":\"0\",\"v\":\"{v:08x}\""),
                    Err(r) => format!("\"flags\",\"r\":\"{r}\",\"v\":\"cdcdcdcd\""),
                };
                self.rec.op(&line);
            }
            ("verify", 1) => {
                let material = self.current.as_mut().expect("a material");
                let r = material.apply_verify(&mut self.rec);
                self.rec.op(&format!("\"verify\",\"r\":\"{r}\""));
            }
            ("resolve", _) => {
                let delta = f32::from_bits(hex_words(f[1])[0]);
                let material = self.current.as_mut().expect("a material");
                let r = material.resolve_textures(&mut self.rec, delta);
                self.rec.op(&format!("\"resolve\",\"r\":\"{r}\""));
            }
            ("bind" | "teardown", _) => {
                let context = Self::context_of(
                    &self.contexts,
                    &self.sources,
                    &self.indices,
                    &self.matrices,
                    self.candidate,
                    f[1],
                );
                let m = self.current.as_mut().expect("a material");
                let r = if f[0] == "bind" {
                    m.bind(&mut self.rec, context.as_ref())
                } else {
                    m.teardown(&mut self.rec, context.as_ref())
                };
                self.rec.op(&format!("\"{}\",\"r\":\"{r}\"", f[0]));
            }
            ("submit", _) => {
                let context = Self::context_of(
                    &self.contexts,
                    &self.sources,
                    &self.indices,
                    &self.matrices,
                    self.candidate,
                    f[1],
                )
                .expect("a context");
                let args = Self::args(f);
                let m = self.current.as_mut().expect("a material");
                let r = m.submit(&mut self.rec, &context, &args);
                self.rec.op(&format!("\"submit\",\"r\":\"{r}\""));
            }
            ("passes", _) => {
                let line = match Material::<Rec>::pass_count(&*self.material()) {
                    Ok(v) => format!("\"passes\",\"r\":\"0\",\"v\":\"{v}\""),
                    Err(r) => format!("\"passes\",\"r\":\"{r}\",\"v\":\"-7\""),
                };
                self.rec.op(&line);
            }
            ("class", _) => {
                let name = Material::<Rec>::class_name(&*self.material());
                self.rec.op(&format!("\"class\",\"name\":\"{name}\""));
            }
            ("params", _) => {
                let m: &dyn Material<Rec> = &*self.material();
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
                    self.rec.op(&line);
                }
            }
            ("counters", _) => {
                let [a, b, c] = rec.counters;
                rec.op(&format!("\"counters\",\"v\":\"{a},{b},{c}\""));
            }
            ("release", _) => {
                let mut material = self.current.take().expect("a material");
                material.destroy(&mut self.rec);
                self.rec.op("\"release\"");
            }
            (other, _) => panic!("material_surface: unknown operation {other}"),
        }
    }

    fn context_of<'a>(
        contexts: &'a BTreeMap<String, ContextDef>,
        sources: &'a BTreeMap<String, VertexSource>,
        indices: &'a BTreeMap<String, Vec<u16>>,
        matrices: &'a BTreeMap<String, [f32; 12]>,
        candidate: MaterialId,
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
            replaced: def.candidate.then_some(candidate),
            field20: 0,
        })
    }
}

/// `material_surface.list` through the recording services.
pub fn lines() -> Vec<String> {
    let mut out = Vec::new();
    for (index, line) in input_lines("material_surface.list").iter().enumerate() {
        let mut case = Case {
            rec: Rec::new(index),
            current: None,
            candidate: candidate_id(),
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
