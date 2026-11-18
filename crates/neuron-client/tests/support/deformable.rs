//! The P4.7 deformable suite: `render::deformable` against recording
//! services with the contract `CppSource/Oracle/oracle_deformable.cpp`
//! states (its header lists the lines), over the operations of
//! `tests/fixtures/oracle/deformable.list` (`make_deformable_fixtures.py`).

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::rc::Rc;

use neuron_client::render::deformable::eye::{
    EyeMaterial, EyeServices, ParamValue, parameter_table,
};
use neuron_client::render::deformable::ik::AimDesc;
use neuron_client::render::deformable::instance::{GR_GENERIC, GR_OK};
use neuron_client::render::deformable::math::{
    JointLimits, euler_clamp, look_at, quat_exp, quat_log,
};
use neuron_client::render::deformable::mesh::fvf_stride;
use neuron_client::render::deformable::svd::{SvdMatrix, pseudo_inverse};
use neuron_client::render::deformable::{
    BoneLink, BoneSlot, ChannelSample, DeformInstance, DeformMesh, DeformPart, DeformServices,
    Deformable, EngineHandle, Instance, LodHolder, MeshArchetype, Skeleton, UvBinding,
};
use neuron_client::render::material::{
    DrawArgs, IbScratch, Material, MaterialId, Materials, SubmitContext, VbListing, VertexLock,
    VertexSource, VertexStream,
};
use neuron_client::render::mesh::MeshCamera;
use neuron_core::hash::name_crc32;
use neuron_core::mesh_data::dfm::{DfmFaceGroup, DfmMesh, SkinPair, SkinVertex, UvDeformRecord};
use neuron_core::mesh_data::trimesh::rebase;
use neuron_core::vfs::FileSystem;

use super::{hex, input_lines, unhex};

type Log = Rc<RefCell<Vec<String>>>;

fn f(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn fs(values: &[f32]) -> String {
    values.iter().map(|&v| f(v)).collect::<Vec<_>>().join(",")
}

fn hex_word(text: &str) -> u32 {
    u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn hex_float(text: &str) -> f32 {
    f32::from_bits(hex_word(text))
}

fn hex_floats(text: &str) -> Vec<f32> {
    if text == "-" || text.is_empty() {
        return Vec::new();
    }
    text.split(',').map(hex_float).collect()
}

fn dec(text: &str) -> i32 {
    text.parse().unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn decs(text: &str) -> Vec<i32> {
    if text == "-" || text.is_empty() {
        return Vec::new();
    }
    text.split(',').map(dec).collect()
}

fn arr<const N: usize>(v: &[f32]) -> [f32; N] {
    core::array::from_fn(|i| v.get(i).copied().unwrap_or(0.0))
}

fn fnv(bytes: &[u8]) -> String {
    super::data::fnv64(bytes)
}

fn stream_text(s: &VertexStream, src_count: u32) -> String {
    let elements = match s.data.as_deref() {
        Some(data) if s.count != 0 => {
            let mut bytes = Vec::new();
            for i in 0..s.count as usize {
                let at = i * s.stride as usize;
                for k in 0..s.byte_size as usize {
                    bytes.push(data.get(at + k).copied().unwrap_or(0));
                }
            }
            fnv(&bytes)
        }
        _ => "-".to_owned(),
    };
    let index = match &s.index {
        Some(index) => {
            let mut bytes = Vec::new();
            for i in 0..src_count as usize {
                bytes.extend_from_slice(&index.get(i).copied().unwrap_or(0).to_le_bytes());
            }
            fnv(&bytes)
        }
        None => "-".to_owned(),
    };
    format!(
        ";{}/{}/{}/{elements}/{index}",
        s.count, s.stride, s.byte_size
    )
}

fn source_text(d: &VertexSource) -> String {
    let mut out = format!("{:x},{},{:x}", d.fvf, d.count, d.flags);
    for s in [&d.position, &d.normal, &d.diffuse, &d.tex[0], &d.tex[1]] {
        out += &stream_text(s, d.count);
    }
    out
}

const IDENTITY12: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];
const IDENTITY9: [f32; 9] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0];

/// A material the arena hands an id to.
struct Dummy;

impl Material<()> for Dummy {
    fn initialize(&mut self, _: &mut ()) -> i32 {
        0
    }
    fn set_name(&mut self, _: &[u8]) -> i32 {
        0
    }
    fn load(&mut self, _: &mut (), _: &mut dyn FileSystem) -> i32 {
        0
    }
    fn apply_verify(&mut self, _: &mut ()) -> i32 {
        0
    }
    fn resolve_textures(&mut self, _: &mut (), _: f32) -> i32 {
        0
    }
    fn render_flags(&mut self, _: &mut ()) -> u32 {
        0
    }
    fn submit(&mut self, _: &mut (), _: &SubmitContext<'_>, _: &DrawArgs) -> i32 {
        0
    }
}

#[derive(Default)]
struct Sample {
    a: Vec<f32>,
    b: Vec<f32>,
    alpha: f32,
}

/// The recording services (`FDefState` and the fakes).
struct Rec {
    log: Log,
    case: usize,
    // engine
    matrix: HashMap<i32, [f32; 12]>,
    parent: HashMap<i32, i32>,
    relative: HashMap<i32, [u32; 23]>,
    position: HashMap<i32, [f32; 3]>,
    orientation: HashMap<i32, [f32; 9]>,
    joint_params: HashMap<i32, Vec<f32>>,
    skel_handle: i32,
    link_root: i32,
    links: Vec<(i32, i32)>,
    next_instance: i32,
    hardpoints: bool,
    hardpoint_names: HashMap<i32, Vec<Vec<u8>>>,
    connect_result: u32,
    // animation
    play_results: Vec<i32>,
    next_play: i32,
    filtered: BTreeSet<i32>,
    durations: HashMap<i32, f32>,
    touch: HashMap<i32, Vec<f32>>,
    root_data: bool,
    root_result: u8,
    root_vec: Vec<f32>,
    named: Option<f32>,
    eval_result: i32,
    objects: BTreeSet<(i32, u32)>,
    first_field: HashMap<i32, u32>,
    blocked: BTreeSet<i32>,
    // channels
    next_channel: u32,
    in_mixer: BTreeSet<u32>,
    samples: HashMap<u32, Sample>,
    // renderer, pipeline, buffers
    render_rank: i32,
    state: Option<i32>,
    render_states: HashMap<u32, i32>,
    texture_formats: HashMap<u32, i32>,
    vb_data: HashMap<u32, Vec<u8>>,
    ib_data: HashMap<u32, Vec<u16>>,
    live_vb: BTreeSet<u32>,
    live_ib: BTreeSet<u32>,
    next_vb: u32,
    next_ib: u32,
    draw_result: i32,
    // textures
    textures: HashMap<Vec<u8>, (i32, u32)>,
    records: Vec<Vec<u8>>,
    next_playback: i32,
    // materials
    fakes: HashMap<MaterialId, String>,
    eyes: HashMap<MaterialId, EyeMaterial>,
}

impl Rec {
    fn new(log: Log, case: usize) -> Self {
        Self {
            log,
            case,
            matrix: HashMap::new(),
            parent: HashMap::new(),
            relative: HashMap::new(),
            position: HashMap::new(),
            orientation: HashMap::new(),
            joint_params: HashMap::new(),
            skel_handle: 0x50,
            link_root: -1,
            links: Vec::new(),
            next_instance: 0x500,
            hardpoints: true,
            hardpoint_names: HashMap::new(),
            connect_result: 0,
            play_results: Vec::new(),
            next_play: 0x700,
            filtered: BTreeSet::new(),
            durations: HashMap::new(),
            touch: HashMap::new(),
            root_data: true,
            root_result: 0,
            root_vec: Vec::new(),
            named: None,
            eval_result: 0,
            objects: BTreeSet::new(),
            first_field: HashMap::new(),
            blocked: BTreeSet::new(),
            next_channel: 0x900,
            in_mixer: BTreeSet::new(),
            samples: HashMap::new(),
            render_rank: 3,
            state: Some(1),
            render_states: HashMap::new(),
            texture_formats: HashMap::new(),
            vb_data: HashMap::new(),
            ib_data: HashMap::new(),
            live_vb: BTreeSet::new(),
            live_ib: BTreeSet::new(),
            next_vb: 0x100,
            next_ib: 0x200,
            draw_result: 0,
            textures: HashMap::new(),
            records: Vec::new(),
            next_playback: 0x40,
            fakes: HashMap::new(),
            eyes: HashMap::new(),
        }
    }

    fn ev(&self, body: &str) {
        self.log
            .borrow_mut()
            .push(format!("{{\"case\":\"{}\",{body}}}", self.case));
    }

    fn context(c: &SubmitContext<'_>) -> String {
        let vb = c.vb.map_or_else(
            || "-".to_owned(),
            |l| format!("{:x},{:x},{:x}", l.vertex_buffer, l.fvf, l.base_vertex),
        );
        let ib = c.ib.map_or_else(
            || "-".to_owned(),
            |s| format!("{:x},{:x}", s.index_buffer, s.bias),
        );
        #[allow(clippy::cast_possible_truncation)]
        let inst = c.instance as u32 as i32;
        format!(
            "\"inst\":\"{inst}\",\"world\":\"{}\",\"view\":\"{}\",\"vb\":\"{vb}\",\"ib\":\"{ib}\",\"f20\":\"{}\"",
            fs(c.world),
            fs(c.view),
            u8::from(c.field20 != 0)
        )
    }
}

impl DeformServices for Rec {
    fn create_archetype(&mut self, _: &[u8], _: &mut dyn FileSystem) -> EngineHandle {
        -1
    }
    fn create_skeleton_archetype(&mut self, _: &[u8], _: &mut dyn FileSystem) -> EngineHandle {
        -1
    }
    fn release_handle(&mut self, handle: EngineHandle) {
        self.ev(&format!("\"ev\":\"relh\",\"h\":\"{handle}\""));
    }
    fn query_vmesh_part(&mut self, _: EngineHandle) -> Result<u32, i32> {
        Err(-1)
    }
    fn add_ref_vmesh_part(&mut self, _: u32) {}
    fn release_vmesh_part(&mut self, _: u32) {}
    fn draw_vmesh_part(&mut self, _: u32, instance: Instance) {
        self.ev(&format!(
            "\"ev\":\"vmdraw\",\"part\":\"1\",\"inst\":\"{instance}\""
        ));
    }
    fn create_instance(
        &mut self,
        handle: EngineHandle,
        link: Option<&mut dyn BoneLink>,
    ) -> Instance {
        if handle == self.skel_handle {
            let root = self.link_root;
            self.ev(&format!(
                "\"ev\":\"cinst\",\"h\":\"{handle}\",\"r\":\"{root}\""
            ));
            let links = self.links.clone();
            if let Some(link) = link {
                for (inst, arch) in links {
                    let r = link.link(self, inst, arch);
                    self.ev(&format!(
                        "\"ev\":\"link\",\"inst\":\"{inst}\",\"arch\":\"{arch}\",\"r\":\"{}\"",
                        u8::from(r)
                    ));
                }
            }
            return root;
        }
        let made = self.next_instance;
        self.next_instance += 1;
        self.ev(&format!(
            "\"ev\":\"cinst\",\"h\":\"{handle}\",\"r\":\"{made}\""
        ));
        made
    }
    fn destroy_instance(&mut self, instance: Instance) {
        self.ev(&format!("\"ev\":\"dinst\",\"inst\":\"{instance}\""));
    }
    fn set_instance_transform(&mut self, instance: Instance, bone: usize) -> Option<[f32; 12]> {
        self.ev(&format!(
            "\"ev\":\"setxf\",\"inst\":\"{instance}\",\"bone\":\"{bone}\""
        ));
        if instance == -1 {
            return None;
        }
        Some(*self.matrix.entry(instance).or_insert(IDENTITY12))
    }
    fn instance_parent(&mut self, instance: Instance) -> Instance {
        let r = self.parent.get(&instance).copied().unwrap_or(-1);
        self.ev(&format!(
            "\"ev\":\"parent\",\"inst\":\"{instance}\",\"r\":\"{r}\""
        ));
        r
    }
    fn instance_relative(&mut self, instance: Instance) -> [u32; 23] {
        // Not recorded (see oracle_deformable.cpp).
        *self.relative.entry(instance).or_insert([0; 23])
    }
    fn instance_position(&mut self, instance: Instance) -> [f32; 3] {
        self.ev(&format!("\"ev\":\"pos\",\"inst\":\"{instance}\""));
        *self.position.entry(instance).or_insert([0.0; 3])
    }
    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9] {
        self.ev(&format!("\"ev\":\"ori\",\"inst\":\"{instance}\""));
        *self.orientation.entry(instance).or_insert(IDENTITY9)
    }
    fn set_instance_position(&mut self, instance: Instance, position: &[f32; 3]) {
        self.ev(&format!(
            "\"ev\":\"setpos\",\"inst\":\"{instance}\",\"v\":\"{}\"",
            fs(position)
        ));
    }
    fn set_instance_orientation(&mut self, instance: Instance, rows: &[f32; 9]) {
        self.ev(&format!(
            "\"ev\":\"setori\",\"inst\":\"{instance}\",\"v\":\"{}\"",
            fs(rows)
        ));
    }
    fn joint_params(&mut self, instance: Instance, _: u32) -> Vec<f32> {
        let params = self.joint_params.entry(instance).or_default().clone();
        self.ev(&format!(
            "\"ev\":\"jp\",\"inst\":\"{instance}\",\"n\":\"{}\"",
            params.len()
        ));
        params
    }
    fn has_hardpoints(&mut self) -> bool {
        self.ev(&format!(
            "\"ev\":\"hpq\",\"r\":\"{}\"",
            if self.hardpoints { 0 } else { -1 }
        ));
        self.hardpoints
    }
    fn hardpoint_names(&mut self, archetype: EngineHandle) -> Vec<Vec<u8>> {
        self.ev(&format!("\"ev\":\"enumhp\",\"arch\":\"{archetype}\""));
        self.hardpoint_names
            .get(&archetype)
            .cloned()
            .unwrap_or_default()
    }
    fn connect_hardpoints(&mut self, a: Instance, b: Instance, name: &[u8]) -> u32 {
        self.ev(&format!(
            "\"ev\":\"connhp\",\"a\":\"{a}\",\"b\":\"{b}\",\"name\":\"{}\",\"r\":\"{}\"",
            String::from_utf8_lossy(name),
            self.connect_result
        ));
        self.connect_result
    }
    fn play_script(&mut self, name: u32, root: Instance, extra: u32, script: u32) -> i32 {
        let mut r = self.next_play;
        self.next_play += 1;
        if !self.play_results.is_empty() {
            r = self.play_results.remove(0);
        }
        self.ev(&format!(
            "\"ev\":\"play\",\"name\":\"{name:x}\",\"root\":\"{root}\",\"extra\":\"{extra:x}\",\"sink\":\"1\",\"script\":\"{script:x}\",\"r\":\"{r}\""
        ));
        r
    }
    fn destroy_script(&mut self, handle: i32) {
        self.ev(&format!("\"ev\":\"dscript\",\"h\":\"{handle}\""));
    }
    fn filter_objects(&mut self, handle: i32) -> u32 {
        let r = u32::from(self.filtered.contains(&handle));
        self.ev(&format!(
            "\"ev\":\"filter\",\"h\":\"{handle}\",\"r\":\"{r}\""
        ));
        r
    }
    fn root_eval(
        &mut self,
        handle: i32,
        mut vec: [f32; 3],
        flags: u32,
        time: f32,
        scale: f32,
        d: u32,
        flag_f: u32,
    ) -> Option<(u8, [f32; 3])> {
        if !self.root_data {
            return None;
        }
        for (k, v) in self.root_vec.iter().take(3).enumerate() {
            vec[k] = *v;
        }
        self.ev(&format!(
            "\"ev\":\"root\",\"h\":\"{handle}\",\"flags\":\"{flags:x}\",\"t\":\"{}\",\"s\":\"{}\",\"d\":\"{d:x}\",\"f\":\"{flag_f:x}\",\"one\":\"{}\",\"r\":\"{}\",\"v\":\"{}\"",
            f(time),
            f(scale),
            f(1.0),
            self.root_result,
            fs(&vec)
        ));
        Some((self.root_result, vec))
    }
    fn named_duration(&mut self, name: u32, extra: u32) -> Option<f32> {
        self.ev(&format!(
            "\"ev\":\"named\",\"name\":\"{name:x}\",\"extra\":\"{extra:x}\",\"r\":\"{}\",\"v\":\"{}\"",
            u8::from(self.named.is_some()),
            f(self.named.unwrap_or(0.0))
        ));
        self.named
    }
    fn eval_playback(
        &mut self,
        handle: i32,
        flags: u32,
        time: f32,
        scale: f32,
        d: u32,
        flag_f: u32,
        xform: &[f32; 12],
    ) -> i32 {
        self.ev(&format!(
            "\"ev\":\"evalpb\",\"h\":\"{handle}\",\"flags\":\"{flags:x}\",\"t\":\"{}\",\"s\":\"{}\",\"d\":\"{d:x}\",\"f\":\"{flag_f:x}\",\"one\":\"{:x}\",\"x\":\"{}\",\"r\":\"{}\"",
            f(time),
            f(scale),
            1.0f32.to_bits(),
            fs(xform),
            self.eval_result
        ));
        self.eval_result
    }
    fn has_object(&mut self, handle: i32, key: u32) -> u32 {
        let r = u32::from(self.objects.contains(&(handle, key)));
        self.ev(&format!(
            "\"ev\":\"hasobj\",\"h\":\"{handle}\",\"key\":\"{key:x}\",\"r\":\"{r}\""
        ));
        r
    }
    fn first_instance_field(&mut self, handle: i32) -> Option<u32> {
        let v = self.first_field.get(&handle).copied().unwrap_or(0);
        self.ev(&format!(
            "\"ev\":\"firstf\",\"h\":\"{handle}\",\"v\":\"{v:x}\""
        ));
        Some(v)
    }
    fn duration_if_named(&mut self, handle: i32) -> Option<f32> {
        let found = self.durations.get(&handle).copied();
        self.ev(&format!(
            "\"ev\":\"durn\",\"h\":\"{handle}\",\"r\":\"{}\",\"v\":\"{}\"",
            u8::from(found.is_some()),
            f(found.unwrap_or(0.0))
        ));
        found
    }
    fn touch_mapped(&mut self, handle: i32) -> ([f32; 3], [f32; 3]) {
        self.ev(&format!("\"ev\":\"touch\",\"h\":\"{handle}\""));
        let v = self.touch.get(&handle).cloned().unwrap_or_default();
        let mut pos = [0.0; 3];
        let mut extra = [0.0; 3];
        for (k, x) in v.iter().take(6).enumerate() {
            if k < 3 {
                pos[k] = *x;
            } else {
                extra[k - 3] = *x;
            }
        }
        (pos, extra)
    }
    fn set_overlay_pos(&mut self, handle: i32, pos: &[f32; 3]) {
        self.ev(&format!(
            "\"ev\":\"ovpos\",\"h\":\"{handle}\",\"v\":\"{}\"",
            fs(pos)
        ));
    }
    fn set_overlay_extra(&mut self, handle: i32, extra: &[f32; 9]) {
        self.ev(&format!(
            "\"ev\":\"ovextra\",\"h\":\"{handle}\",\"v\":\"{}\"",
            fs(extra)
        ));
    }
    fn add_overlay_pos(&mut self, handle: i32, delta: &[f32; 3]) {
        self.ev(&format!(
            "\"ev\":\"addov\",\"h\":\"{handle}\",\"v\":\"{}\"",
            fs(delta)
        ));
    }
    fn find_object(&mut self, handle: i32, kind: u32, instance: Instance) -> u32 {
        let r = handle
            .cast_unsigned()
            .wrapping_mul(0x100)
            .wrapping_add(instance.cast_unsigned().wrapping_mul(2))
            .wrapping_add(kind);
        self.ev(&format!(
            "\"ev\":\"findobj\",\"h\":\"{handle}\",\"kind\":\"{kind}\",\"inst\":\"{instance}\",\"r\":\"{r:x}\""
        ));
        r
    }
    fn register_handle(&mut self, key: i32) {
        self.ev(&format!("\"ev\":\"reghandle\",\"k\":\"{key}\""));
    }
    fn has_handle(&mut self, key: i32) -> u8 {
        let r = u8::from(self.blocked.contains(&key));
        self.ev(&format!(
            "\"ev\":\"hashandle\",\"k\":\"{key}\",\"r\":\"{r}\""
        ));
        r
    }
    fn has_any_handle(&mut self, key: i32) {
        self.ev(&format!("\"ev\":\"hasany\",\"k\":\"{key}\""));
    }
    fn create_typed_channel(&mut self, _: u32, joint: Instance, bind: Instance) -> u32 {
        let key = self.next_channel;
        self.next_channel += 1;
        self.ev(&format!(
            "\"ev\":\"ctyped\",\"type\":\"4\",\"cookie\":\"0,0,0,{joint}\",\"bind\":\"{bind}\",\"r\":\"{key:x}\""
        ));
        key
    }
    fn play_channel(&mut self, key: u32, blob: &[u32; 7]) {
        let b = blob
            .iter()
            .map(|w| format!("{w:x}"))
            .collect::<Vec<_>>()
            .join(",");
        self.ev(&format!(
            "\"ev\":\"cplay\",\"key\":\"{key:x}\",\"blob\":\"{b}\""
        ));
    }
    fn destroy_channel(&mut self, key: u32) {
        self.ev(&format!("\"ev\":\"cdestroy\",\"key\":\"{key:x}\""));
    }
    fn channel_in_mixer(&mut self, key: u32) -> u8 {
        let r = u8::from(self.in_mixer.contains(&key));
        self.ev(&format!(
            "\"ev\":\"inmixer\",\"key\":\"{key:x}\",\"r\":\"{r}\""
        ));
        r
    }
    fn sample_channel(&mut self, key: u32, time: f32) -> ChannelSample {
        let s = self.samples.entry(key).or_default();
        s.a.resize(8, 0.0);
        s.b.resize(8, 0.0);
        let out = ChannelSample {
            a: s.a.clone(),
            b: s.b.clone(),
            alpha: s.alpha,
        };
        self.ev(&format!(
            "\"ev\":\"sample\",\"key\":\"{key:x}\",\"t\":\"{}\"",
            f(time)
        ));
        out
    }
    fn render_instance(&mut self, instance: Instance, flags: u32, arg: u32) -> i32 {
        self.ev(&format!(
            "\"ev\":\"rinst\",\"inst\":\"{instance}\",\"thr\":\"{}\",\"flags\":\"{flags:x}\",\"m\":\"0\",\"arg\":\"{arg:x}\",\"r\":\"{}\"",
            f(1.0),
            self.render_rank
        ));
        self.render_rank
    }
    fn pipeline_state(&mut self, id: u32) -> Option<i32> {
        match self.state {
            None => self.ev(&format!("\"ev\":\"pstate\",\"id\":\"{id}\",\"r\":\"-\"")),
            Some(v) => self.ev(&format!("\"ev\":\"pstate\",\"id\":\"{id}\",\"r\":\"{v}\"")),
        }
        self.state
    }
    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        self.ev(&format!("\"ev\":\"modelview\",\"m\":\"{}\"", fs(matrix)));
    }
    fn set_world(&mut self, matrix: &[f32; 12]) {
        self.ev(&format!("\"ev\":\"world\",\"m\":\"{}\"", fs(matrix)));
    }
    fn set_world_n(&mut self, index: u32, matrix: &[f32; 12]) {
        self.ev(&format!(
            "\"ev\":\"worldn\",\"i\":\"{index}\",\"m\":\"{}\"",
            fs(matrix)
        ));
    }
    fn render_state(&mut self, state: u32) -> i32 {
        let v = self.render_states.get(&state).copied().unwrap_or(0);
        self.ev(&format!("\"ev\":\"getrs\",\"s\":\"{state}\",\"v\":\"{v}\""));
        v
    }
    fn set_render_state(&mut self, state: u32, value: i32) {
        self.ev(&format!(
            "\"ev\":\"setrs\",\"s\":\"{state}\",\"v\":\"{value}\""
        ));
        self.render_states.insert(state, value);
    }
    fn is_vb_valid(&mut self, vertex_buffer: u32) -> i32 {
        let r = i32::from(self.live_vb.contains(&vertex_buffer));
        self.ev(&format!(
            "\"ev\":\"isvb\",\"vb\":\"{vertex_buffer:x}\",\"r\":\"{r}\""
        ));
        r
    }
    fn select_vb(&mut self, vertex_buffer: u32) -> i32 {
        let r = if self.live_vb.contains(&vertex_buffer) {
            0
        } else {
            -1
        };
        self.ev(&format!(
            "\"ev\":\"selvb\",\"vb\":\"{vertex_buffer:x}\",\"r\":\"{r}\""
        ));
        r
    }
    fn create_vb(&mut self, fvf: u32, count: u32) -> Result<u32, i32> {
        let handle = self.next_vb;
        self.next_vb += 1;
        self.live_vb.insert(handle);
        self.vb_data
            .insert(handle, vec![0; count as usize * fvf_stride(fvf) as usize]);
        self.ev(&format!(
            "\"ev\":\"createvb\",\"fvf\":\"{fvf:x}\",\"n\":\"{count}\",\"vb\":\"{handle:x}\""
        ));
        Ok(handle)
    }
    fn lock_vb(&mut self, vertex_buffer: u32, count: u32) -> Result<(u32, Vec<u8>), i32> {
        self.ev(&format!(
            "\"ev\":\"lockvb\",\"vb\":\"{vertex_buffer:x}\",\"n\":\"{count}\""
        ));
        let data = self.vb_data.entry(vertex_buffer).or_default();
        if data.len() < count as usize * 0x20 {
            data.resize(count as usize * 0x20, 0);
        }
        Ok((0, data.clone()))
    }
    fn copy_vertices(
        &mut self,
        vertex_buffer: u32,
        base: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
        _: &mut [u8],
    ) -> i32 {
        self.ev(&format!(
            "\"ev\":\"copyv\",\"vb\":\"{vertex_buffer:x}\",\"base\":\"{base}\",\"start\":\"{start}\",\"n\":\"{count}\",\"src\":\"{}\"",
            source_text(source)
        ));
        0
    }
    fn unlock_vb(&mut self, vertex_buffer: u32, locked: Vec<u8>) {
        self.ev(&format!(
            "\"ev\":\"unlockvb\",\"vb\":\"{vertex_buffer:x}\",\"data\":\"{}\"",
            hex(&locked)
        ));
        self.vb_data.insert(vertex_buffer, locked);
    }
    fn destroy_vb(&mut self, vertex_buffer: u32) {
        self.ev(&format!(
            "\"ev\":\"destroyvb\",\"vb\":\"{vertex_buffer:x}\""
        ));
        self.live_vb.remove(&vertex_buffer);
    }
    fn is_ib_valid(&mut self, index_buffer: u32) -> i32 {
        let r = i32::from(self.live_ib.contains(&index_buffer));
        self.ev(&format!(
            "\"ev\":\"isib\",\"ib\":\"{index_buffer:x}\",\"r\":\"{r}\""
        ));
        r
    }
    fn select_ib(&mut self, index_buffer: u32, base_vertex: u32) -> i32 {
        let r = if self.live_ib.contains(&index_buffer) {
            0
        } else {
            -1
        };
        self.ev(&format!(
            "\"ev\":\"selib\",\"ib\":\"{index_buffer:x}\",\"base\":\"{base_vertex}\",\"r\":\"{r}\""
        ));
        r
    }
    fn create_ib(&mut self, count: u32) -> Result<u32, i32> {
        let handle = self.next_ib;
        self.next_ib += 1;
        self.live_ib.insert(handle);
        self.ib_data.insert(handle, vec![0; count as usize]);
        self.ev(&format!(
            "\"ev\":\"createib\",\"n\":\"{count}\",\"ib\":\"{handle:x}\""
        ));
        Ok(handle)
    }
    fn lock_ib(&mut self, index_buffer: u32, count: u32) -> Result<u32, i32> {
        self.ev(&format!(
            "\"ev\":\"lockib\",\"ib\":\"{index_buffer:x}\",\"n\":\"{count}\""
        ));
        let data = self.ib_data.entry(index_buffer).or_default();
        if data.len() < count as usize {
            data.resize(count as usize, 0);
        }
        Ok(0)
    }
    fn unlock_ib(&mut self, index_buffer: u32, written: &[u16]) {
        let data = self.ib_data.entry(index_buffer).or_default();
        for (k, w) in written.iter().enumerate() {
            if let Some(d) = data.get_mut(k) {
                *d = *w;
            }
        }
        let text = data
            .iter()
            .map(u16::to_string)
            .collect::<Vec<_>>()
            .join(",");
        self.ev(&format!(
            "\"ev\":\"unlockib\",\"ib\":\"{index_buffer:x}\",\"data\":\"{text}\""
        ));
    }
    fn destroy_ib(&mut self, index_buffer: u32) {
        self.ev(&format!("\"ev\":\"destroyib\",\"ib\":\"{index_buffer:x}\""));
        self.live_ib.remove(&index_buffer);
    }
    fn find_material(&mut self, _: &[u8], _: u32) -> (i32, Option<MaterialId>) {
        (GR_GENERIC, None)
    }
    fn release_material(&mut self, material: MaterialId) {
        if let Some(name) = self.fakes.get(&material) {
            self.ev(&format!("\"ev\":\"mrelease\",\"m\":\"{name}\""));
        }
    }
    fn material_class_name(&mut self, material: MaterialId) -> Vec<u8> {
        if self.eyes.contains_key(&material) {
            return b"EyeMaterial".to_vec();
        }
        let name = self.fakes.get(&material).cloned().unwrap_or_default();
        self.ev(&format!(
            "\"ev\":\"class\",\"m\":\"{}\"",
            name.split(':').next().unwrap_or("")
        ));
        let class = name.split(':').nth(1).unwrap_or("").to_owned();
        class.into_bytes()
    }
    fn bind(
        &mut self,
        material: MaterialId,
        context: &SubmitContext<'_>,
        binding: Option<&mut UvBinding>,
    ) {
        if let Some(mut eye) = self.eyes.remove(&material) {
            eye.bind(self, Some(context), binding);
            self.eyes.insert(material, eye);
            return;
        }
        let name = self.fakes[&material]
            .split(':')
            .next()
            .unwrap_or("")
            .to_owned();
        self.ev(&format!(
            "\"ev\":\"bind\",\"m\":\"{name}\",{}",
            Self::context(context)
        ));
    }
    fn submit(&mut self, material: MaterialId, context: &SubmitContext<'_>, args: &DrawArgs) {
        if let Some(mut eye) = self.eyes.remove(&material) {
            eye.submit(self, context, args);
            self.eyes.insert(material, eye);
            return;
        }
        let name = self.fakes[&material]
            .split(':')
            .next()
            .unwrap_or("")
            .to_owned();
        self.ev(&format!(
            "\"ev\":\"submit\",\"m\":\"{name}\",{},\"prim\":\"{}\",\"start\":\"{}\",\"count\":\"{}\",\"ibs\":\"{}\",\"span\":\"{}\",\"flags\":\"{:x}\"",
            Self::context(context),
            args.prim_type,
            args.start,
            args.count,
            args.ib_start,
            args.index_span,
            args.flags
        ));
    }
    fn teardown(&mut self, material: MaterialId, context: &SubmitContext<'_>) {
        if let Some(mut eye) = self.eyes.remove(&material) {
            eye.teardown(self, Some(context));
            self.eyes.insert(material, eye);
            return;
        }
        let name = self.fakes[&material]
            .split(':')
            .next()
            .unwrap_or("")
            .to_owned();
        self.ev(&format!(
            "\"ev\":\"teardown\",\"m\":\"{name}\",{}",
            Self::context(context)
        ));
    }
    fn rand(&mut self) -> i32 {
        0
    }
    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str) {
        self.ev(&format!(
            "\"ev\":\"dump\",\"flags\":\"{flags:x}\",\"file\":\"{file}\",\"line\":\"{line:x}\",\"msg\":\"{}\"",
            hex(message.as_bytes())
        ));
    }
}

impl EyeServices for Rec {
    fn set_material(&mut self, block: &[u32; 17]) {
        let v: Vec<f32> = block.iter().map(|&w| f32::from_bits(w)).collect();
        self.ev(&format!("\"ev\":\"setmat\",\"v\":\"{}\"", fs(&v)));
    }
    fn set_render_state(&mut self, state: u32, value: u32) {
        DeformServices::set_render_state(self, state, value.cast_signed());
    }
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        self.ev(&format!(
            "\"ev\":\"tss\",\"st\":\"{stage}\",\"s\":\"{state}\",\"v\":\"{}\"",
            value.cast_signed()
        ));
    }
    fn set_stage_texture(&mut self, stage: u32, handle: u32) {
        self.ev(&format!(
            "\"ev\":\"tex\",\"st\":\"{stage}\",\"h\":\"{handle:x}\""
        ));
    }
    fn set_texture_transform(&mut self, stage: u32, matrix: &[f32; 16]) {
        self.ev(&format!(
            "\"ev\":\"ttrans\",\"st\":\"{stage}\",\"m\":\"{}\"",
            fs(matrix)
        ));
    }
    fn texture_format(&mut self, handle: u32) -> Result<i32, i32> {
        let found = self.texture_formats.get(&handle).copied();
        self.ev(&format!(
            "\"ev\":\"texfmt\",\"h\":\"{handle:x}\",\"r\":\"{}\",\"v\":\"{}\"",
            if found.is_some() { 0 } else { -1 },
            found.unwrap_or(0)
        ));
        found.ok_or(GR_GENERIC)
    }
    fn verify_state(&mut self) -> i32 {
        self.ev("\"ev\":\"verify\"");
        GR_OK
    }
    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> i32 {
        self.ev(&format!(
            "\"ev\":\"dip\",\"prim\":\"{prim}\",\"min\":\"{min_index}\",\"nv\":\"{vertex_count}\",\"start\":\"{start_index}\",\"n\":\"{count}\",\"r\":\"{}\"",
            self.draw_result
        ));
        self.draw_result
    }
    fn draw_indexed_vb(
        &mut self,
        prim: u32,
        vertex_buffer: u32,
        base_vertex: u32,
        vertex_count: u32,
        _: u32,
        span: u32,
    ) -> i32 {
        self.ev(&format!(
            "\"ev\":\"dipvb\",\"prim\":\"{prim}\",\"vb\":\"{vertex_buffer:x}\",\"base\":\"{base_vertex}\",\"n\":\"{vertex_count}\",\"span\":\"{span}\""
        ));
        GR_OK
    }
    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        self.ev(&format!(
            "\"ev\":\"lockvm\",\"fvf\":\"{fvf:x}\",\"n\":\"{count}\",\"r\":\"-1\""
        ));
        Err(GR_GENERIC)
    }
    fn copy_vertex_data(&mut self, _: &mut VertexLock, _: u32, _: &VertexSource, _: u32, _: u32) {}
    fn unlock_vertex_memory(&mut self, _: VertexLock) {}
    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<u32> {
        let hit = self.textures.contains_key(name);
        let mut record = 0;
        if hit {
            self.records.push(name.to_vec());
            record = self.records.len();
        }
        self.ev(&format!(
            "\"ev\":\"findtex\",\"name\":\"{}\",\"crc\":\"{crc:08x}\",\"r\":\"{record}\"",
            String::from_utf8_lossy(name)
        ));
        hit.then(|| u32::try_from(record).unwrap_or(0))
    }
    fn frame_count(&mut self, record: u32) -> i32 {
        let n = self.textures[&self.records[record as usize - 1]].0;
        self.ev(&format!(
            "\"ev\":\"fcount\",\"rec\":\"{record}\",\"v\":\"{n}\""
        ));
        n
    }
    fn create_playback(&mut self, record: u32) -> Result<i32, i32> {
        let made = self.next_playback;
        self.next_playback += 1;
        self.ev(&format!(
            "\"ev\":\"cplayback\",\"rec\":\"{record}\",\"id\":\"{made}\""
        ));
        Ok(made)
    }
    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<u32> {
        let mut v = 0;
        if id >= 0x40
            && let Some(name) = self.records.get((id - 0x40) as usize)
        {
            v = self.textures[name].1;
        }
        self.ev(&format!(
            "\"ev\":\"getframe\",\"id\":\"{id}\",\"frame\":\"{frame}\",\"v\":\"{v:x}\""
        ));
        Some(v)
    }
    fn release_texture_record(&mut self, record: u32) {
        self.ev(&format!("\"ev\":\"reltex\",\"rec\":\"{record}\""));
    }
    fn dec_ref_texture(&mut self, id: i32) {
        self.ev(&format!("\"ev\":\"decref\",\"id\":\"{id}\""));
    }
    fn instance_parent(&mut self, instance: Instance) -> Instance {
        DeformServices::instance_parent(self, instance)
    }
    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9] {
        DeformServices::instance_orientation(self, instance)
    }
    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str) {
        DeformServices::dump(self, flags, file, line, message);
    }
}

/// The camera (`ICamera`).
struct Camera {
    log: Log,
    case: usize,
    view: [f32; 12],
    classify: i32,
}

impl Camera {
    fn ev(&self, body: &str) {
        self.log
            .borrow_mut()
            .push(format!("{{\"case\":\"{}\",{body}}}", self.case));
    }
}

impl MeshCamera for Camera {
    fn view_transform(&mut self) -> [f32; 12] {
        self.ev("\"ev\":\"camview\"");
        self.view
    }
    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32 {
        self.ev(&format!(
            "\"ev\":\"classify\",\"c\":\"{}\",\"r\":\"{}\",\"res\":\"{}\"",
            fs(&centre),
            f(radius),
            self.classify
        ));
        self.classify
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        0.0
    }
    fn viewport(&mut self) -> [i32; 4] {
        [0; 4]
    }
    fn x_scale_over_near(&mut self) -> f64 {
        0.0
    }
    fn z_near(&mut self) -> f32 {
        0.0
    }
    fn position(&mut self) -> [f32; 3] {
        [0.0; 3]
    }
}

#[derive(Default)]
struct MeshBuild {
    name: String,
    flag70: bool,
    points: Vec<f32>,
    point_indices: Vec<i32>,
    uvs: Vec<f32>,
    uv_indices: Option<Vec<i32>>,
    skin: Vec<(i32, Vec<(i32, f32)>)>,
    groups: Vec<(String, Vec<i32>)>,
    uv_deform: Vec<Vec<f32>>,
}

/// Where the latest holder is: waiting for its part, or in a part.
enum HolderAt {
    None,
    Pending(LodHolder),
    Part(usize),
}

struct Case {
    rec: Rec,
    camera: Camera,
    module: Deformable,
    arena: Materials<()>,
    materials: HashMap<String, MaterialId>,
    build: MeshBuild,
    bones: Vec<f32>,
    holder: HolderAt,
    parts: Vec<DeformPart>,
    instance: Option<DeformInstance>,
    aims: Vec<Option<u32>>,
    aim_targets: Vec<([f32; 3], [f32; 9])>,
}

impl Case {
    fn op(&self, body: &str) {
        self.rec.ev(body);
    }

    fn holder_mut(&mut self) -> &mut LodHolder {
        match &mut self.holder {
            HolderAt::Pending(h) => h,
            HolderAt::Part(i) => {
                let i = *i;
                let part = match self.instance.as_mut() {
                    Some(inst) => &mut inst.parts[i],
                    None => &mut self.parts[i],
                };
                part.lod.as_mut().expect("a holder")
            }
            HolderAt::None => panic!("no holder"),
        }
    }

    fn holder_bound(&self) -> usize {
        match &self.holder {
            HolderAt::Pending(h) => h.bound(),
            HolderAt::Part(i) => {
                let part = match self.instance.as_ref() {
                    Some(inst) => &inst.parts[*i],
                    None => &self.parts[*i],
                };
                part.lod.as_ref().map_or(0, LodHolder::bound)
            }
            HolderAt::None => 0,
        }
    }

    fn part_mut(&mut self, index: usize) -> &mut DeformPart {
        match self.instance.as_mut() {
            Some(inst) => &mut inst.parts[index],
            None => &mut self.parts[index],
        }
    }

    fn inst(&mut self) -> &mut DeformInstance {
        self.instance.as_mut().expect("an instance")
    }

    fn bone_text(holder: &LodHolder, index: usize) -> String {
        let b = &holder.bones[index];
        format!(
            "\"inst\":\"{}\",\"flags\":\"{:x}\",\"local\":\"{}\",\"matrix\":\"{}\"",
            b.instance,
            b.flags,
            fs(&b.local),
            fs(&b.matrix)
        )
    }

    fn make_mesh(&mut self) -> MeshArchetype {
        let b = &self.build;
        let mut tables = DfmMesh {
            face_group_count: i32::try_from(b.groups.len()).unwrap_or(0),
            face_groups: Vec::new(),
            point_indices: b.point_indices.iter().map(|&i| i.cast_unsigned()).collect(),
            vertices: b.points.as_chunks::<6>().0.to_vec(),
            uv_indices: b
                .uv_indices
                .as_ref()
                .map(|v| v.iter().map(|&i| i.cast_unsigned()).collect()),
            uvs: b.uvs.as_chunks::<2>().0.to_vec(),
            skin_pairs: Vec::new(),
            skin: Vec::new(),
            uv_deform: b
                .uv_deform
                .iter()
                .map(|r| {
                    #[allow(clippy::cast_possible_truncation)]
                    UvDeformRecord {
                        bone_id: r[0] as i32,
                        vertex_count: r[1] as i32,
                        plane_distance: r[2],
                        bone_x_to_u: r[3],
                        bone_y_to_v: r[4],
                        min_du: r[5],
                        max_du: r[6],
                        min_dv: r[7],
                        max_dv: r[8],
                    }
                })
                .collect(),
        };
        let mut first = 0u32;
        for (count, pairs) in &b.skin {
            tables.skin.push(SkinVertex {
                bone_count: *count,
                first_pair: first,
            });
            for &(bone_id, weight) in pairs {
                tables.skin_pairs.push(SkinPair { bone_id, weight });
                first += 1;
            }
        }
        let mut materials = Vec::new();
        for (name, indices) in &b.groups {
            let mut group = DfmFaceGroup::default();
            if name != "-" {
                group.material_name = Some(name.as_bytes().to_vec());
                group.material_crc = name_crc32(name.as_bytes());
                let mut idx: Vec<u16> = indices
                    .iter()
                    .map(|&i| u16::try_from(i).unwrap_or(0))
                    .collect();
                let (min, max) = rebase(&mut idx);
                group.index_min = min;
                group.index_max = max;
                group.indices = idx;
            }
            materials.push(self.materials.get(name).copied());
            tables.face_groups.push(group);
        }
        let with_class: Vec<(Option<MaterialId>, Vec<u8>)> = materials
            .into_iter()
            .map(|m| {
                (
                    m,
                    m.map(|m| self.rec.material_class_name(m))
                        .unwrap_or_default(),
                )
            })
            .collect();
        let mut mesh = MeshArchetype::from_tables(&tables, &with_class, self.holder_bound())
            .expect("bound skin");
        mesh.flag70 = self.build.flag70;
        mesh
    }

    fn cell_text(cell: &ParamValue) -> String {
        if cell.ty == 5 || cell.ty == 7 {
            return format!(
                "{}:{}",
                cell.ty,
                cell.text
                    .as_ref()
                    .map_or_else(|| "-".to_owned(), |t| hex(t))
            );
        }
        let three = cell.ty == 3 || cell.ty == 4;
        format!(
            "{}:{:x},{:x},{:x},{:x}",
            cell.ty,
            cell.words[0],
            if three { cell.words[1] } else { 0 },
            if three { cell.words[2] } else { 0 },
            if cell.ty == 4 { cell.words[3] } else { 0 }
        )
    }

    fn eye_state(eye: &EyeMaterial) -> String {
        let row = eye.rows.first().cloned().unwrap_or_default();
        format!(
            "\"v\":\"{}\",\"tr\":\"{}\",\"one\":\"{}\",\"row\":\"{},{:x},{},{},{},{}\",\"name\":\"{}\"",
            fs(&eye.colours),
            u8::from(eye.translucent),
            u8::from(eye.one_bit_alpha),
            row.handle,
            row.frame,
            row.wrap_mode,
            row.address_u,
            row.address_v,
            u8::from(row.single_frame),
            row.name.as_ref().map_or_else(|| "-".to_owned(), |n| hex(n))
        )
    }

    #[allow(clippy::too_many_lines)]
    fn run_eye(&mut self, t: &[&str]) {
        let id = self.materials[t[2]];
        let mut eye = self.rec.eyes.remove(&id).expect("an eye material");
        let rec = &mut self.rec;
        match t[1] {
            "set" => {
                let index = usize::try_from(dec(t[3])).unwrap_or(usize::MAX);
                let mut cell = ParamValue::of(dec(t[4]));
                if cell.ty == 5 || cell.ty == 7 {
                    cell.text = (t[5] != "-").then(|| {
                        let b = unhex(t[5]);
                        neuron_core::hash::c_str(&b).to_vec()
                    });
                } else {
                    for (k, w) in t[5].split(',').take(4).enumerate() {
                        cell.words[k] = hex_word(w);
                    }
                }
                let r = eye.set_value(index, &cell, rec);
                rec.ev(&format!(
                    "\"op\":\"eyeset\",\"r\":\"{r}\",{}",
                    Self::eye_state(&eye)
                ));
            }
            "get" => {
                let index = usize::try_from(dec(t[3])).unwrap_or(usize::MAX);
                let (r, cell) = match eye.get_value(index) {
                    Some(cell) => (GR_OK, cell),
                    None => (GR_GENERIC, ParamValue::default()),
                };
                rec.ev(&format!(
                    "\"op\":\"eyeget\",\"r\":\"{r}\",\"cell\":\"{}\"",
                    Self::cell_text(&cell)
                ));
            }
            "desc" => {
                let text = parameter_table()
                    .iter()
                    .map(|p| {
                        format!(
                            "{:x},{},{},{},{}",
                            p.flags,
                            p.key,
                            Self::cell_text(&p.value),
                            Self::cell_text(&p.min),
                            Self::cell_text(&p.max)
                        )
                    })
                    .collect::<Vec<_>>()
                    .join(";");
                rec.ev(&format!("\"op\":\"eyedesc\",\"v\":\"{text}\""));
            }
            "name" => {
                let name = (t[3] != "-").then(|| unhex(t[3]));
                eye.set_name(name.as_deref());
                let (r, v) = match &eye.name {
                    Some(n) => (GR_OK, hex(n)),
                    None => (GR_GENERIC, String::new()),
                };
                rec.ev(&format!("\"op\":\"eyename\",\"r\":\"{r}\",\"v\":\"{v}\""));
            }
            "setup" => {
                let r = eye.setup(rec);
                rec.ev(&format!(
                    "\"op\":\"eyesetup\",\"r\":\"{r}\",{}",
                    Self::eye_state(&eye)
                ));
            }
            "resolve" => {
                let r = eye.resolve_textures(rec);
                rec.ev(&format!(
                    "\"op\":\"eyeresolve\",\"r\":\"{r}\",{}",
                    Self::eye_state(&eye)
                ));
            }
            "verify" => {
                let r = eye.apply_verify(rec);
                rec.ev(&format!("\"op\":\"eyeverify\",\"r\":\"{r}\""));
            }
            "flags" => {
                rec.ev(&format!(
                    "\"op\":\"eyeflags\",\"v\":\"{:x}\"",
                    eye.render_flags()
                ));
            }
            "bind" => {
                if t[3] == "null" {
                    eye.bind(rec, None, None);
                    eye.teardown(rec, None);
                    rec.ev("\"op\":\"eyebind\"");
                } else {
                    let r = hex_floats(t[4]);
                    let bone_index = usize::try_from(dec(t[3])).unwrap_or(0);
                    self.rec.eyes.insert(id, eye);
                    let (local, instance) = {
                        let b = &self.holder_mut().bones[bone_index];
                        (b.local, b.instance)
                    };
                    let mut eye = self.rec.eyes.remove(&id).expect("an eye material");
                    let rec = &mut self.rec;
                    #[allow(clippy::cast_possible_truncation)]
                    let record = UvDeformRecord {
                        bone_id: r[0] as i32,
                        vertex_count: r[1] as i32,
                        plane_distance: r[2],
                        bone_x_to_u: r[3],
                        bone_y_to_v: r[4],
                        min_du: r[5],
                        max_du: r[6],
                        min_dv: r[7],
                        max_dv: r[8],
                    };
                    let mut binding = UvBinding {
                        record: Some(record),
                        bone_local: local,
                        bone_instance: instance,
                        eye_uv_transform_count: 0,
                    };
                    let ctx = SubmitContext {
                        instance: instance.cast_unsigned() as usize,
                        world: &IDENTITY12,
                        view: &IDENTITY12,
                        vertices: None,
                        indices: &[],
                        vb: None,
                        ib: None,
                        replaced: None,
                        field20: 1,
                    };
                    eye.bind(rec, Some(&ctx), Some(&mut binding));
                    eye.teardown(rec, Some(&ctx));
                    rec.ev(&format!(
                        "\"op\":\"eyebind\",\"n\":\"{}\"",
                        binding.eye_uv_transform_count
                    ));
                    self.rec.eyes.insert(id, eye);
                    return;
                }
            }
            "submit" => {
                rec.draw_result = dec(t[3]);
                let vb = VbListing {
                    vertex_buffer: 0x123,
                    fvf: 0x112,
                    base_vertex: 5,
                };
                let ib = IbScratch {
                    index_buffer: 0x456,
                    bias: 7,
                };
                let ctx = SubmitContext {
                    instance: 0,
                    world: &IDENTITY12,
                    view: &IDENTITY12,
                    vertices: None,
                    indices: &[],
                    vb: Some(&vb),
                    ib: Some(&ib),
                    replaced: None,
                    field20: 0,
                };
                let args = DrawArgs {
                    prim_type: 4,
                    start: 2,
                    count: 9,
                    ib_start: 3,
                    index_span: 12,
                    flags: 0x100,
                };
                let r = eye.submit(rec, &ctx, &args);
                rec.draw_result = 0;
                rec.ev(&format!("\"op\":\"eyesubmit\",\"r\":\"{r}\""));
            }
            other => panic!("unknown eye operation {other}"),
        }
        self.rec.eyes.insert(id, eye);
    }

    #[allow(clippy::too_many_lines)]
    fn run(&mut self, t: &[&str]) {
        match t[0] {
            "bones" => {
                self.bones = hex_floats(t[1]);
                self.op(&format!(
                    "\"op\":\"bones\",\"n\":\"{}\"",
                    self.bones.len() / 12
                ));
            }
            "link" => {
                self.rec.link_root = dec(t[1]);
                self.rec.links.clear();
                if t.len() > 2 && t[2] != "-" {
                    for pair in t[2].split(',') {
                        let (a, b) = pair.split_once(':').expect("a pair");
                        self.rec.links.push((dec(a), dec(b)));
                    }
                }
            }
            "matrix" => {
                self.rec.matrix.insert(dec(t[1]), arr(&hex_floats(t[2])));
            }
            "holder" => {
                let count = self.bones.len() / 12;
                let mut skeleton = Skeleton::default();
                for index in 0..count {
                    skeleton.bones.push(BoneSlot {
                        archetype: 0x100 + i32::try_from(index).unwrap_or(0),
                        bone_to_root: arr(&self.bones[index * 12..index * 12 + 12]),
                    });
                }
                skeleton.archetype = self.rec.skel_handle;
                let mut holder = LodHolder::default();
                let r = holder.attach_skeleton(skeleton, &mut self.rec);
                let slots = holder
                    .bones
                    .iter()
                    .map(|b| format!("{}:{:x}", b.instance, b.flags))
                    .collect::<Vec<_>>()
                    .join(",");
                self.op(&format!(
                    "\"op\":\"holder\",\"r\":\"{}\",\"bound\":\"{}\",\"slots\":\"{slots}\"",
                    u8::from(r),
                    holder.bind_cursor
                ));
                self.holder = HolderAt::Pending(holder);
            }
            "mat" => {
                let id = self.arena.insert(Box::new(Dummy), 1);
                self.rec.fakes.insert(id, format!("{}:{}", t[1], t[2]));
                self.materials.insert(t[1].to_owned(), id);
            }
            "eyemat" => {
                let id = self.arena.insert(Box::new(Dummy), 1);
                let mut eye = EyeMaterial::new();
                let r = eye.initialize(&mut self.rec);
                self.op(&format!(
                    "\"op\":\"eyemat\",\"r\":\"{r}\",\"v\":\"{}\",\"tr\":\"{}\"",
                    fs(&eye.colours),
                    u8::from(eye.translucent)
                ));
                self.rec.eyes.insert(id, eye);
                self.materials.insert(t[1].to_owned(), id);
            }
            "mesh" => {
                self.build = MeshBuild {
                    name: t[1].to_owned(),
                    flag70: dec(t[2]) != 0,
                    ..MeshBuild::default()
                };
            }
            "pts" => self.build.points = hex_floats(t[1]),
            "pidx" => self.build.point_indices = decs(t[1]),
            "uv" => self.build.uvs = hex_floats(t[1]),
            "uvidx" => self.build.uv_indices = Some(decs(t[1])),
            "skin" => {
                for vertex in t[1].split(',') {
                    let mut parts = vertex.split(':');
                    let count = dec(parts.next().expect("a count"));
                    let pairs = parts
                        .map(|p| {
                            let (a, b) = p.split_once('/').expect("a pair");
                            (dec(a), hex_float(b))
                        })
                        .collect();
                    self.build.skin.push((count, pairs));
                }
            }
            "grp" => self
                .build
                .groups
                .push((t[1].to_owned(), decs(t.get(2).copied().unwrap_or("-")))),
            "uvd" => self.build.uv_deform.push(hex_floats(t[1])),
            "endmesh" => {
                let mut mesh = self.make_mesh();
                let (radius, centre) = mesh.bound_sphere();
                let lists = mesh
                    .bone_verts
                    .iter()
                    .map(|l| l.len().to_string())
                    .collect::<Vec<_>>()
                    .join(",");
                let uvb = mesh
                    .face_groups
                    .iter()
                    .map(|g| g.uv_bone_index.cast_signed().to_string())
                    .collect::<Vec<_>>()
                    .join(",");
                let (ic, vc) = (mesh.index_count(), mesh.vertex_capacity());
                let name = self.build.name.clone();
                self.module
                    .meshes
                    .insert(name.as_bytes(), DeformMesh::Dfm(Box::new(mesh)));
                self.op(&format!(
                    "\"op\":\"mesh\",\"name\":\"{name}\",\"ic\":\"{ic}\",\"vc\":\"{vc}\",\"lists\":\"{lists}\",\"uvb\":\"{uvb}\",\"r\":\"{}\",\"c\":\"{}\"",
                    f(radius),
                    fs(&centre)
                ));
            }
            "part" => {
                let ids: Vec<_> = t[1]
                    .split(',')
                    .map(|n| {
                        self.module
                            .meshes
                            .find(n.as_bytes())
                            .expect("a cached mesh")
                    })
                    .collect();
                let HolderAt::Pending(holder) =
                    core::mem::replace(&mut self.holder, HolderAt::None)
                else {
                    panic!("no pending holder");
                };
                let mut part = DeformPart::default();
                let r = part.bind_meshes(holder, &ids, &self.module.meshes);
                self.op(&format!(
                    "\"op\":\"part\",\"r\":\"{}\",\"vcap\":\"{}\",\"icap\":\"{}\"",
                    u8::from(r),
                    part.vb_capacity,
                    part.ib_capacity
                ));
                self.parts.push(part);
                self.holder = HolderAt::Part(self.parts.len() - 1);
            }
            "inst" => {
                let parts = core::mem::take(&mut self.parts);
                self.instance = Some(DeformInstance::with_parts(parts, 0, hex_word(t[1])));
                self.op("\"op\":\"inst\"");
            }
            "sink" => self.inst().sink = 1,
            "bone" => {
                let index = usize::try_from(dec(t[1])).unwrap_or(0);
                let v = hex_floats(t[3]);
                let holder = self.holder_mut();
                match t[2] {
                    "mat" => holder.bones[index].set_matrix(&arr(&v)),
                    "pos" => holder.bones[index].set_position(&arr(&v)),
                    _ => holder.bones[index].set_orientation(&arr(&v)),
                }
                let text = Self::bone_text(self.holder_mut(), index);
                self.op(&format!("\"op\":\"bone\",{text}"));
            }
            "dirty" => {
                let index = usize::try_from(dec(t[1])).unwrap_or(0);
                let r = self.holder_mut().bones[index].mark_world_dirty();
                let text = Self::bone_text(self.holder_mut(), index);
                self.op(&format!("\"op\":\"dirty\",\"r\":\"{r}\",{text}"));
            }
            "showbone" => {
                let index = usize::try_from(dec(t[1])).unwrap_or(0);
                let text = Self::bone_text(self.holder_mut(), index);
                self.op(&format!("\"op\":\"showbone\",{text}"));
            }
            "lod" => {
                let index = usize::try_from(dec(t[1])).unwrap_or(0);
                let part = self.part_mut(index);
                let r = part.select_lod(dec(t[2]));
                let (flags, idx) = (part.flags, part.lod_index);
                self.op(&format!(
                    "\"op\":\"lod\",\"r\":\"{}\",\"flags\":\"{flags:x}\",\"idx\":\"{idx}\"",
                    u8::from(r)
                ));
            }
            "setlod" => {
                self.module.rate = hex_float(t[3]);
                let lods = decs(t[2]);
                let rate = self.module.rate;
                let inst = self.instance.as_mut().expect("an instance");
                inst.set_lod(hex_float(t[1]), &lods, rate);
                let flags = inst
                    .parts
                    .iter()
                    .map(|p| format!("{:x}", p.flags))
                    .collect::<Vec<_>>()
                    .join(",");
                let cookie = inst.rate_cookie;
                self.op(&format!(
                    "\"op\":\"setlod\",\"r\":\"0\",\"cookie\":\"{}\",\"flags\":\"{flags}\"",
                    f(cookie)
                ));
            }
            "drawpart" => {
                let index = usize::try_from(dec(t[1])).unwrap_or(0);
                let inst = self.instance.as_mut().expect("an instance");
                let r = inst.parts[index].draw_live(
                    &mut self.camera,
                    &mut self.module.meshes,
                    &mut self.rec,
                );
                let flags = inst.parts[index].flags;
                self.op(&format!(
                    "\"op\":\"drawpart\",\"r\":\"{r}\",\"flags\":\"{flags:x}\""
                ));
            }
            "draw" => {
                let inst = self.instance.as_mut().expect("an instance");
                let lod = inst.draw(
                    &mut self.camera,
                    hex_word(t[1]),
                    hex_word(t[2]),
                    &mut self.module.meshes,
                    &mut self.rec,
                );
                self.op(&format!("\"op\":\"draw\",\"lod\":\"{lod}\""));
            }
            "sphere" => {
                let inst = self.instance.as_ref().expect("an instance");
                let (r, radius, centre) = match inst.bound_sphere(&mut self.module.meshes) {
                    Some((radius, centre)) => (GR_OK, radius, centre),
                    None => (GR_GENERIC, -1.0, [-1.0; 3]),
                };
                self.op(&format!(
                    "\"op\":\"sphere\",\"r\":\"{r}\",\"rad\":\"{}\",\"c\":\"{}\"",
                    f(radius),
                    fs(&centre)
                ));
            }
            "stats" => {
                let inst = self.instance.as_ref().expect("an instance");
                let (r, v) = match inst.mesh_stats(&self.module.meshes) {
                    Some(v) => (GR_OK, v),
                    None => (GR_GENERIC, [0; 9]),
                };
                let text = v.iter().map(u32::to_string).collect::<Vec<_>>().join(",");
                self.op(&format!("\"op\":\"stats\",\"r\":\"{r}\",\"v\":\"{text}\""));
            }
            "first" => {
                let v = self
                    .instance
                    .as_ref()
                    .map_or(-1, DeformInstance::first_bone);
                self.op(&format!("\"op\":\"first\",\"v\":\"{v}\""));
            }
            "walkhp" => {
                let inst = self.instance.as_ref().expect("an instance");
                let (log, case) = (self.rec.log.clone(), self.rec.case);
                let r = inst.walk_hardpoints(&mut self.rec, &mut |_, bone, name| {
                    log.borrow_mut().push(format!(
                        "{{\"case\":\"{case}\",\"ev\":\"hpvisit\",\"inst\":\"{bone}\",\"name\":\"{}\"}}",
                        String::from_utf8_lossy(name)
                    ));
                });
                self.op(&format!("\"op\":\"walkhp\",\"r\":\"{}\"", u8::from(r)));
            }
            other => self.run_more(other, t),
        }
    }
}

impl Case {
    #[allow(clippy::too_many_lines)]
    fn run_more(&mut self, op: &str, t: &[&str]) {
        match op {
            "connect" => {
                self.instance
                    .as_mut()
                    .expect("an instance")
                    .connect_parts(&mut self.rec);
                self.op("\"op\":\"connect\"");
            }
            "hp" => {
                let names = if t.len() > 2 && t[2] != "-" {
                    t[2].split(',').map(|n| n.as_bytes().to_vec()).collect()
                } else {
                    Vec::new()
                };
                self.rec.hardpoint_names.insert(dec(t[1]), names);
            }
            "nohp" => self.rec.hardpoints = false,
            "connres" => self.rec.connect_result = hex_word(t[1]),
            "parent" => {
                self.rec.parent.insert(dec(t[1]), dec(t[2]));
            }
            "rel" => {
                let mut words = [0u32; 23];
                for (k, w) in t[2].split(',').take(23).enumerate() {
                    words[k] = hex_word(w);
                }
                self.rec.relative.insert(dec(t[1]), words);
            }
            "pos" => {
                self.rec.position.insert(dec(t[1]), arr(&hex_floats(t[2])));
            }
            "ori" => {
                self.rec
                    .orientation
                    .insert(dec(t[1]), arr(&hex_floats(t[2])));
            }
            "jp" => {
                self.rec.joint_params.insert(dec(t[1]), hex_floats(t[2]));
            }
            "block" => {
                self.rec.blocked.insert(dec(t[1]));
            }
            "cam" => {
                if t[1] == "view" {
                    self.camera.view = arr(&hex_floats(t[2]));
                } else {
                    self.camera.classify = dec(t[2]);
                }
            }
            "pstate" => self.rec.state = (t[1] != "-").then(|| dec(t[1])),
            "rank" => self.rec.render_rank = dec(t[1]),
            "aim" => {
                let pos: [f32; 3] = arr(&hex_floats(t[4]));
                let orient: [f32; 9] = arr(&hex_floats(t[5]));
                self.aim_targets.push((pos, orient));
                let desc = AimDesc {
                    root: dec(t[1]),
                    end: dec(t[2]),
                    flags: hex_word(t[3]),
                    target_pos: pos,
                    target_orient: orient,
                    damping: hex_float(t[6]),
                    locks: decs(t[7]),
                };
                let inst = self.instance.as_mut().expect("an instance");
                let node = inst.start_aim(&desc, hex_word(t[8]), &mut self.rec);
                let mut detail = String::new();
                if node >= 0 {
                    let id = u32::try_from(node).unwrap_or(0);
                    let e = inst.aim_mut(id).expect("an aim event");
                    let n = usize::try_from(e.joint_count).unwrap_or(0);
                    let ids = e.joint_ids[..n]
                        .iter()
                        .map(i32::to_string)
                        .collect::<Vec<_>>()
                        .join(",");
                    let mats = e.bind_mats[..n]
                        .iter()
                        .map(|m| fs(m))
                        .collect::<Vec<_>>()
                        .join(";");
                    let locks = e.locks[..n]
                        .iter()
                        .map(u8::to_string)
                        .collect::<Vec<_>>()
                        .join(",");
                    detail = format!(
                        ",\"n\":\"{n}\",\"f34\":\"{:x}\",\"ids\":\"{ids}\",\"bind\":\"{mats}\",\"locks\":\"{locks}\"",
                        e.flags34 & 3
                    );
                    self.aims.push(Some(id));
                } else {
                    self.aims.push(None);
                }
                let r = if node >= 0 { GR_OK } else { GR_GENERIC };
                self.op(&format!("\"op\":\"aim\",\"r\":\"{r}\"{detail}"));
            }
            "target" => {
                let index = usize::try_from(dec(t[1])).unwrap_or(0);
                let pos: [f32; 3] = arr(&hex_floats(t[2]));
                let orient: [f32; 9] = arr(&hex_floats(t[3]));
                self.aim_targets[index] = (pos, orient);
                if let Some(Some(node)) = self.aims.get(index).copied()
                    && let Some(e) = self.inst().aim_mut(node)
                {
                    e.set_targets(pos, orient);
                }
            }
            "apply" => {
                let node = self.aims[usize::try_from(dec(t[1])).unwrap_or(0)].expect("an aim");
                let channel = usize::try_from(dec(t[2])).unwrap_or(0);
                let (skip, joint) = (dec(t[3]) != 0, dec(t[4]));
                let inst = self.instance.as_mut().expect("an instance");
                let e = inst.aim_mut(node).expect("an aim event");
                let key = e.channels[channel];
                let mut out = [-1.0f32; 8];
                let r = e.apply(&mut out, key, skip, joint, &mut self.rec);
                let n = usize::try_from(e.joint_count).unwrap_or(0);
                let quats = (0..n)
                    .map(|j| fs(&e.quats[j * 4..j * 4 + 4]))
                    .collect::<Vec<_>>()
                    .join(";");
                let tick = e.tick;
                self.op(&format!(
                    "\"op\":\"apply\",\"r\":\"{r}\",\"tick\":\"{tick}\",\"out\":\"{}\",\"quats\":\"{quats}\"",
                    fs(&out[..4])
                ));
            }
            "stopaim" => {
                if let Some(Some(node)) = self
                    .aims
                    .get(usize::try_from(dec(t[1])).unwrap_or(0))
                    .copied()
                {
                    self.instance
                        .as_mut()
                        .expect("an instance")
                        .stop_aim(node, &mut self.rec);
                }
                self.op("\"op\":\"stopaim\"");
            }
            "play" | "playalt" => {
                let w = |i: usize| hex_word(t[i]);
                let inst = self.instance.as_mut().expect("an instance");
                let handle = if op == "play" {
                    inst.play(
                        w(1),
                        w(2),
                        f32::from_bits(w(3)),
                        w(4),
                        f32::from_bits(w(5)),
                        w(6),
                        w(7),
                        f32::from_bits(w(8)),
                        w(9),
                        &mut self.rec,
                    )
                } else {
                    inst.play_chain(
                        w(1),
                        w(2),
                        f32::from_bits(w(3)),
                        w(4),
                        f32::from_bits(w(5)),
                        w(6),
                        w(7),
                        f32::from_bits(w(8)),
                        dec(t[9]),
                        &mut self.rec,
                    )
                };
                let r = if handle == -1 { GR_GENERIC } else { GR_OK };
                let o24 = inst.offset24;
                self.op(&format!(
                    "\"op\":\"{op}\",\"r\":\"{r}\",\"h\":\"{handle}\",\"o24\":\"{}\"",
                    f(o24)
                ));
            }
            "playres" => self.rec.play_results.push(dec(t[1])),
            "filter" => {
                self.rec.filtered.insert(dec(t[1]));
            }
            "durn" => {
                self.rec.durations.insert(dec(t[1]), hex_float(t[2]));
            }
            "touch" => {
                self.rec.touch.insert(dec(t[1]), hex_floats(t[2]));
            }
            "rootres" => {
                self.rec.root_data = t[1] != "-";
                self.rec.root_result = if self.rec.root_data {
                    u8::try_from(dec(t[1])).unwrap_or(0)
                } else {
                    0
                };
                self.rec.root_vec = hex_floats(t.get(2).copied().unwrap_or("-"));
            }
            "named" => self.rec.named = (t[1] != "-").then(|| hex_float(t[1])),
            "evalres" => self.rec.eval_result = dec(t[1]),
            "object" => {
                self.rec.objects.insert((dec(t[1]), hex_word(t[2])));
            }
            "firstf" => {
                self.rec.first_field.insert(dec(t[1]), hex_word(t[2]));
            }
            "mixer" => {
                self.rec.in_mixer.insert(hex_word(t[1]));
            }
            "samp" => {
                let s = self.rec.samples.entry(hex_word(t[1])).or_default();
                s.alpha = hex_float(t[2]);
                s.a = hex_floats(t[3]);
                s.b = hex_floats(t[4]);
            }
            "height" => {
                let inst = self.instance.as_mut().expect("an instance");
                inst.set_height(hex_float(t[1]), &mut self.rec);
                let o24 = inst.offset24;
                self.op(&format!(
                    "\"op\":\"height\",\"r\":\"0\",\"o24\":\"{}\"",
                    f(o24)
                ));
            }
            "posepos" => {
                let v: [f32; 3] = arr(&hex_floats(t[1]));
                self.instance
                    .as_mut()
                    .expect("an instance")
                    .set_pose_pos(&v, &mut self.rec);
                self.op("\"op\":\"posepos\",\"r\":\"0\"");
            }
            "poseori" => {
                let v: [f32; 9] = arr(&hex_floats(t[1]));
                self.instance
                    .as_mut()
                    .expect("an instance")
                    .set_pose_ori(&v, &mut self.rec);
                self.op("\"op\":\"poseori\",\"r\":\"0\"");
            }
            "count" => {
                let inst = self.instance.as_mut().expect("an instance");
                let n = inst.list_count(&mut self.rec);
                let o24 = inst.offset24;
                self.op(&format!(
                    "\"op\":\"count\",\"r\":\"0\",\"v\":\"{n}\",\"o24\":\"{}\"",
                    f(o24)
                ));
            }
            "destroyall" => {
                self.instance
                    .as_mut()
                    .expect("an instance")
                    .destroy_all(&mut self.rec);
                self.op("\"op\":\"destroyall\"");
            }
            "destroyh" => {
                self.instance
                    .as_mut()
                    .expect("an instance")
                    .destroy_handle(dec(t[1]), &mut self.rec);
                self.op("\"op\":\"destroyh\"");
            }
            "onloop" => {
                let mut x: [f32; 12] = arr(&hex_floats(t[2]));
                let inst = self.instance.as_ref().expect("an instance");
                let fwd = inst.on_loop(hex_word(t[1]), &mut x, &mut self.rec);
                self.op(&format!(
                    "\"op\":\"onloop\",\"fwd\":\"{}\",\"x\":\"{}\"",
                    u8::from(fwd),
                    fs(&x)
                ));
            }
            "onkeys" => {
                let inst = self.instance.as_ref().expect("an instance");
                let cookie = inst.on_keys(hex_word(t[1]), &mut self.rec);
                self.op(&format!(
                    "\"op\":\"onkeys\",\"fwd\":\"{}\",\"cookie\":\"{:x}\"",
                    u8::from(cookie.is_some()),
                    cookie.unwrap_or(u32::MAX)
                ));
            }
            "destroy" => {
                if let Some(inst) = self.instance.take() {
                    self.module.destroy(inst, &mut self.rec);
                }
                self.op(&format!(
                    "\"op\":\"destroy\",\"cache\":\"{}\"",
                    self.module.meshes.len()
                ));
            }
            "svd" => {
                let (rows, cols) = (dec(t[1]), dec(t[2]));
                let v = hex_floats(t[3]);
                let mut source = SvdMatrix::new(rows, cols);
                for r in 0..rows {
                    for c in 0..cols {
                        source.set(r, c, v[usize::try_from(r * cols + c).unwrap_or(0)]);
                    }
                }
                let mut out = SvdMatrix::default();
                if !pseudo_inverse(&source, &mut out).converged {
                    DeformServices::dump(
                        &mut self.rec,
                        0x0010_0002,
                        "Deformable2\\Mat.h",
                        0x2E8,
                        "svdcmp: didnt approach a finite soln.",
                    );
                }
                let mut cells = Vec::new();
                for r in 0..out.rows {
                    for c in 0..out.cols {
                        cells.push(f(out.at(r, c)));
                    }
                }
                self.op(&format!(
                    "\"op\":\"svd\",\"rows\":\"{}\",\"cols\":\"{}\",\"v\":\"{}\"",
                    out.rows,
                    out.cols,
                    cells.join(",")
                ));
            }
            "euler" => {
                let l = hex_floats(t[1]);
                let limits = JointLimits {
                    roll: [l[0], l[1]],
                    yaw: [l[2], l[3]],
                    pitch: [l[4], l[5]],
                };
                let out = euler_clamp(&limits, &arr(&hex_floats(t[2])));
                self.op(&format!("\"op\":\"euler\",\"v\":\"{}\"", fs(&out)));
            }
            "look" => {
                #[allow(clippy::cast_possible_truncation)]
                let flags = hex_word(t[1]) as u8;
                let out = look_at(&arr(&hex_floats(t[2])), &arr(&hex_floats(t[3])), flags);
                self.op(&format!("\"op\":\"look\",\"v\":\"{}\"", fs(&out)));
            }
            "qlog" => {
                let out = quat_log(&arr(&hex_floats(t[1])), &arr(&hex_floats(t[2])));
                self.op(&format!("\"op\":\"qlog\",\"v\":\"{}\"", fs(&out)));
            }
            "qexp" => {
                let out = quat_exp(&arr(&hex_floats(t[1])));
                self.op(&format!("\"op\":\"qexp\",\"v\":\"{}\"", fs(&out)));
            }
            "tex" => {
                self.rec
                    .textures
                    .insert(t[1].as_bytes().to_vec(), (dec(t[2]), hex_word(t[3])));
            }
            "texfmt" => {
                self.rec.texture_formats.insert(hex_word(t[1]), dec(t[2]));
            }
            "eye" => self.run_eye(t),
            other => panic!("unknown operation {other}"),
        }
    }
}

/// The suite's lines: every case of `deformable.list` in order.
pub fn lines() -> Vec<String> {
    let log: Log = Rc::new(RefCell::new(Vec::new()));
    for (case, line) in input_lines("deformable.list").iter().enumerate() {
        let mut runner = Case {
            rec: Rec::new(log.clone(), case),
            camera: Camera {
                log: log.clone(),
                case,
                view: IDENTITY12,
                classify: 4,
            },
            module: Deformable::default(),
            arena: Materials::new(),
            materials: HashMap::new(),
            build: MeshBuild::default(),
            bones: Vec::new(),
            holder: HolderAt::None,
            parts: Vec::new(),
            instance: None,
            aims: Vec::new(),
            aim_targets: Vec::new(),
        };
        for op in line.split(" ; ") {
            let t: Vec<&str> = op.split(' ').collect();
            runner.run(&t);
        }
    }
    let out = log.borrow().clone();
    let _ = BTreeMap::<u8, u8>::new();
    out
}
