//! P4.5: `FLAppearance` over the recording fakes
//! `CppSource/Oracle/oracle_material_appearance.cpp` implements (its header
//! states the contract and the line formats), on
//! `tests/fixtures/oracle/material_appearance.list`.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet};
use std::rc::Rc;

use neuron_client::render::appearance::{
    Appearance, AppearanceServices, Camera, Face, ShadowInstance,
};
use neuron_client::render::material::{
    InstanceValue, InstanceValues, MaterialId, Materials, VbListing, VertexLock,
};
use neuron_client::render::mesh::{Archetype, Instance};
use neuron_client::render::texture::FrameOut;

use super::mesh_render::Dummy;
use super::{hex, input_lines};

type Log = Rc<RefCell<Vec<String>>>;

const IDENTITY: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];
const UNSET: u32 = 0xCDCD_CDCD;

fn dec(text: &str) -> i64 {
    text.parse::<i64>()
        .unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn float(text: &str) -> f32 {
    f32::from_bits(u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}")))
}

fn double(text: &str) -> f64 {
    f64::from_bits(u64::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}")))
}

fn floats<const N: usize>(fields: &[&str]) -> [f32; N] {
    let mut out = [0.0; N];
    for (slot, field) in out.iter_mut().zip(fields) {
        *slot = float(field);
    }
    out
}

fn f(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn words(values: &[f32]) -> String {
    values.iter().map(|v| f(*v)).collect::<Vec<_>>().join(",")
}

fn event(log: &Log, case: usize, body: &str) {
    log.borrow_mut()
        .push(format!("{{\"case\":\"{case}\",{body}}}"));
}

/// The inner camera (`FAppCamera`).
struct InnerCamera {
    log: Log,
    case: usize,
    position: [f32; 3],
    orientation: [f32; 9],
    view: [f32; 12],
    unproject: [f32; 3],
    viewport: [i32; 4],
    half_fov: f32,
    fov_y: f32,
    z_near: f32,
    z_far: f32,
    aspect: f32,
    x_scale: f64,
    y_scale: f64,
    project_result: i32,
    project: [f32; 3],
    classify_result: i32,
}

impl InnerCamera {
    fn new(log: Log, case: usize) -> InnerCamera {
        InnerCamera {
            log,
            case,
            position: [1.0, 2.0, 3.0],
            orientation: [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
            view: IDENTITY,
            unproject: [0.0, 0.0, 1.0],
            viewport: [0, 0, 640, 480],
            half_fov: 45.0,
            fov_y: 60.0,
            z_near: 0.5,
            z_far: 5000.0,
            aspect: 1.25,
            x_scale: 1.0,
            y_scale: 1.0,
            project_result: 1,
            project: [320.0, 240.0, 0.5],
            classify_result: 3,
        }
    }

    fn ev(&self, body: &str) {
        event(&self.log, self.case, body);
    }

    fn slot(&self, name: &str) {
        self.ev(&format!("\"ev\":\"cam\",\"f\":\"{name}\""));
    }
}

impl Camera for InnerCamera {
    fn position(&mut self) -> [f32; 3] {
        self.slot("pos");
        self.position
    }
    fn orientation(&mut self) -> [f32; 9] {
        self.slot("orient");
        self.orientation
    }
    fn viewport(&mut self) -> [i32; 4] {
        self.slot("vp");
        self.viewport
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        self.slot("hfov");
        self.half_fov
    }
    fn fov_y_deg(&mut self) -> f32 {
        self.slot("fovy");
        self.fov_y
    }
    fn z_near(&mut self) -> f32 {
        self.slot("znear");
        self.z_near
    }
    fn z_far(&mut self) -> f32 {
        self.slot("zfar");
        self.z_far
    }
    fn aspect(&mut self) -> f32 {
        self.slot("aspect");
        self.aspect
    }
    fn x_scale_over_near(&mut self) -> f64 {
        self.slot("xscale");
        self.x_scale
    }
    fn y_scale_over_near(&mut self) -> f64 {
        self.slot("yscale");
        self.y_scale
    }
    fn project(&mut self, world: [f32; 3], out: &mut [f32; 3]) -> i32 {
        self.ev(&format!(
            "\"ev\":\"cam\",\"f\":\"proj\",\"w\":\"{}\"",
            words(&world)
        ));
        *out = self.project;
        self.project_result
    }
    fn unproject(&mut self, out: &mut [f32; 3], x: f32, y: f32) {
        self.ev(&format!(
            "\"ev\":\"cam\",\"f\":\"unproj\",\"x\":\"{}\",\"y\":\"{}\"",
            f(x),
            f(y)
        ));
        *out = self.unproject;
    }
    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32 {
        self.ev(&format!(
            "\"ev\":\"cam\",\"f\":\"classify\",\"c\":\"{}\",\"r\":\"{}\"",
            words(&centre),
            f(radius)
        ));
        self.classify_result
    }
    fn view_transform(&mut self) -> [f32; 12] {
        self.slot("view");
        self.view
    }
}

/// The recording services (`FAppState` and the fakes).
struct Rec {
    log: Log,
    case: usize,
    fails: BTreeSet<String>,
    mat_results: BTreeMap<usize, i32>,
    mat_ids: BTreeMap<usize, u32>,
    arena: Materials<()>,
    numbers: Vec<(u32, MaterialId)>,
    ability: Option<u32>,
    shadow_find: i32,
    shadow_frame: i32,
    shadow_handle: u32,
    lock_result: i32,
    next_base: u32,
    transforms: BTreeMap<Instance, [f32; 12]>,
    archetypes: BTreeMap<Instance, Archetype>,
    bounds_results: BTreeMap<Archetype, i32>,
    bounds: BTreeMap<Archetype, [f32; 6]>,
    render_states: BTreeMap<u32, u32>,
    probe: Option<Vec<([f32; 3], f32)>>,
    vis: [u32; 3],
    counters: [u32; 3],
    values: InstanceValues,
    engine: bool,
}

impl Rec {
    fn new(log: Log, case: usize) -> Rec {
        Rec {
            log,
            case,
            fails: BTreeSet::new(),
            mat_results: BTreeMap::new(),
            mat_ids: BTreeMap::new(),
            arena: Materials::new(),
            numbers: Vec::new(),
            ability: Some(1),
            shadow_find: -1,
            shadow_frame: 0,
            shadow_handle: 0,
            lock_result: 0,
            next_base: 30,
            transforms: BTreeMap::new(),
            archetypes: BTreeMap::new(),
            bounds_results: BTreeMap::new(),
            bounds: BTreeMap::new(),
            render_states: BTreeMap::new(),
            probe: None,
            vis: [0; 3],
            counters: [0; 3],
            values: InstanceValues::new(),
            engine: false,
        }
    }

    fn ev(&self, body: &str) {
        event(&self.log, self.case, body);
    }

    fn failed(&self, face: &str) -> bool {
        self.fails.contains(face)
    }

    fn material(&mut self, number: u32) -> MaterialId {
        if let Some(&(_, id)) = self.numbers.iter().find(|(n, _)| *n == number) {
            return id;
        }
        let id = self.arena.insert(Box::new(Dummy), 1);
        self.numbers.push((number, id));
        id
    }

    fn number(&self, id: Option<MaterialId>) -> String {
        id.and_then(|id| self.numbers.iter().find(|(_, m)| *m == id))
            .map_or_else(|| "-".to_owned(), |(n, _)| n.to_string())
    }

    /// `ProbeCamera`: what the renderer reads of the camera it is given.
    fn probe(&self, camera: &mut dyn Camera, spheres: &[([f32; 3], f32)]) {
        let position = camera.position();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"pos\",\"v\":\"{}\"",
            words(&position)
        ));
        let orientation = camera.orientation();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"orient\",\"v\":\"{}\"",
            words(&orientation)
        ));
        let vp = camera.viewport();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"vp\",\"v\":\"{},{},{},{}\"",
            vp[0], vp[1], vp[2], vp[3]
        ));
        let value = camera.half_fov_x_deg();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"hfov\",\"v\":\"{}\"",
            f(value)
        ));
        let value = camera.fov_y_deg();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"fovy\",\"v\":\"{}\"",
            f(value)
        ));
        let value = camera.z_near();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"znear\",\"v\":\"{}\"",
            f(value)
        ));
        let value = camera.z_far();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"zfar\",\"v\":\"{}\"",
            f(value)
        ));
        let value = camera.aspect();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"aspect\",\"v\":\"{}\"",
            f(value)
        ));
        let value = camera.x_scale_over_near();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"xscale\",\"v\":\"{:016x}\"",
            value.to_bits()
        ));
        let value = camera.y_scale_over_near();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"yscale\",\"v\":\"{:016x}\"",
            value.to_bits()
        ));
        let mut projected = [0.0; 3];
        let result = camera.project([0.5, -1.25, 40.0], &mut projected);
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"proj\",\"r\":\"{result}\",\"v\":\"{}\"",
            words(&projected)
        ));
        let mut unprojected = [0.0; 3];
        camera.unproject(&mut unprojected, 320.0, 240.0);
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"unproj\",\"v\":\"{}\"",
            words(&unprojected)
        ));
        for (index, (centre, radius)) in spheres.iter().enumerate() {
            let rank = camera.classify_sphere(*centre, *radius);
            self.ev(&format!(
                "\"ev\":\"probe\",\"f\":\"classify\",\"i\":\"{index}\",\"v\":\"{rank}\""
            ));
        }
        let view = camera.view_transform();
        self.ev(&format!(
            "\"ev\":\"probe\",\"f\":\"view\",\"v\":\"{}\"",
            words(&view)
        ));
    }
}

impl AppearanceServices for Rec {
    type TextureRecord = ();

    fn query(&mut self, face: Face) -> bool {
        let (on, name) = match face {
            Face::Engine => ("outer", "engine"),
            Face::MaterialLibrary => ("system", "matlib"),
            Face::Pipeline => ("system", "pipeline"),
            Face::VertexBuffers => ("system", "vbm"),
            Face::Textures => ("system", "texlib"),
            Face::Renderer => ("engine", "renderer"),
        };
        let ok = !self.failed(name);
        self.ev(&format!(
            "\"ev\":\"qi\",\"on\":\"{on}\",\"face\":\"{name}\",\"r\":\"{}\"",
            if ok { 0 } else { -1 }
        ));
        ok
    }

    fn has_system(&mut self) -> bool {
        !self.failed("system")
    }

    fn publish_engine(&mut self) {
        self.engine = true;
    }

    fn find_or_create(&mut self, name: &str, type_name: &str) -> (i32, Option<MaterialId>) {
        let index = neuron_client::render::appearance::OVERRIDE_NAMES
            .iter()
            .position(|n| *n == name)
            .expect("an override name");
        let result = self.mat_results.get(&index).copied().unwrap_or(0);
        let number = self
            .mat_ids
            .get(&index)
            .copied()
            .unwrap_or(index as u32 + 1);
        let id = (number != 0).then(|| self.material(number));
        self.ev(&format!(
            "\"ev\":\"findorcreate\",\"name\":\"{}\",\"type\":\"{}\",\"outer\":\"0\",\"r\":\"{result}\",\"m\":\"{}\"",
            hex(name.as_bytes()),
            hex(type_name.as_bytes()),
            self.number(id)
        ));
        (result, id)
    }

    fn remove_material(&mut self, name: &str, crc: u32) {
        self.ev(&format!(
            "\"ev\":\"removemat\",\"group\":\"0\",\"name\":\"{}\",\"crc\":\"{crc:08x}\"",
            hex(name.as_bytes())
        ));
    }

    fn set_global_record(&mut self, index: u32, value: u32) {
        self.ev(&format!(
            "\"ev\":\"global\",\"i\":\"{index}\",\"v\":\"{value}\""
        ));
    }

    fn release_material(&mut self, material: MaterialId) {
        self.ev(&format!(
            "\"ev\":\"mrel\",\"m\":\"{}\"",
            self.number(Some(material))
        ));
    }

    fn instance_values(&mut self) -> &mut InstanceValues {
        &mut self.values
    }

    fn device_ability(&mut self, index: u32) -> Option<u32> {
        let shown = self
            .ability
            .map_or_else(|| "-".to_owned(), |v| v.to_string());
        self.ev(&format!(
            "\"ev\":\"ability\",\"i\":\"{index}\",\"v\":\"{shown}\""
        ));
        self.ability
    }

    fn set_perspective(&mut self, fov_y_deg: f32, aspect: f32, near: f32, far: f32) {
        self.ev(&format!(
            "\"ev\":\"persp\",\"fov\":\"{}\",\"aspect\":\"{}\",\"near\":\"{}\",\"far\":\"{}\"",
            f(fov_y_deg),
            f(aspect),
            f(near),
            f(far)
        ));
    }

    fn render_state(&mut self, state: u32) -> u32 {
        let value = self.render_states.get(&state).copied().unwrap_or(7);
        self.ev(&format!(
            "\"ev\":\"getrs\",\"s\":\"{state}\",\"v\":\"{value}\""
        ));
        value
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        self.render_states.insert(state, value);
        self.ev(&format!(
            "\"ev\":\"rs\",\"s\":\"{state}\",\"v\":\"{value}\""
        ));
    }

    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        self.ev(&format!("\"ev\":\"modelview\",\"m\":\"{}\"", words(matrix)));
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

    fn draw_vb(&mut self, prim: u32, vertex_buffer: u32, start: u32, count: u32) {
        self.ev(&format!(
            "\"ev\":\"dpvb\",\"prim\":\"{prim}\",\"vb\":\"{vertex_buffer}\",\"start\":\"{start}\",\"n\":\"{count}\""
        ));
    }

    fn find_texture(&mut self, name: &str, crc: u32) -> Result<(), i32> {
        self.ev(&format!(
            "\"ev\":\"find\",\"name\":\"{}\",\"crc\":\"{crc:08x}\",\"r\":\"{}\"",
            hex(name.as_bytes()),
            self.shadow_find
        ));
        if self.shadow_find == 0 {
            Ok(())
        } else {
            Err(self.shadow_find)
        }
    }

    fn record_frame(&mut self, _: &(), frame: i32) -> Option<FrameOut> {
        self.ev(&format!(
            "\"ev\":\"tframe\",\"f\":\"{frame}\",\"r\":\"{}\",\"h\":\"{}\"",
            self.shadow_frame, self.shadow_handle
        ));
        (self.shadow_frame >= 0).then_some(FrameOut {
            handle: self.shadow_handle,
            uv: [0, 0, 0x3F80_0000, 0x3F80_0000],
        })
    }

    fn release_texture(&mut self, (): ()) {
        self.ev("\"ev\":\"trel\"");
    }

    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        self.ev(&format!(
            "\"ev\":\"lock\",\"fvf\":\"{fvf:x}\",\"n\":\"{count}\",\"r\":\"{}\"",
            self.lock_result
        ));
        if self.lock_result != 0 {
            return Err(self.lock_result);
        }
        let lock = VertexLock {
            listing: VbListing {
                vertex_buffer: 900,
                fvf,
                base_vertex: self.next_base,
            },
            bytes: vec![0; count as usize * 24],
            offset: 0,
        };
        self.next_base += count;
        Ok(lock)
    }

    fn unlock_vertex_memory(&mut self, lock: VertexLock) {
        self.ev(&format!(
            "\"ev\":\"unlock\",\"base\":\"{}\",\"bytes\":\"{}\"",
            lock.listing.base_vertex,
            hex(&lock.bytes)
        ));
    }

    fn instance_transform(&mut self, instance: Instance) -> [f32; 12] {
        self.ev(&format!("\"ev\":\"xform\",\"inst\":\"{instance}\""));
        self.transforms.get(&instance).copied().unwrap_or(IDENTITY)
    }

    fn instance_archetype(&mut self, instance: Instance) -> Archetype {
        let archetype = self.archetypes.get(&instance).copied().unwrap_or(-1);
        self.ev(&format!(
            "\"ev\":\"arch\",\"inst\":\"{instance}\",\"r\":\"{archetype}\""
        ));
        archetype
    }

    fn release_handle(&mut self, archetype: Archetype) {
        self.ev(&format!("\"ev\":\"relh\",\"arch\":\"{archetype}\""));
    }

    fn bounds(&mut self, archetype: Archetype, lod: f32) -> (i32, Option<[f32; 6]>) {
        let result = self.bounds_results.get(&archetype).copied().unwrap_or(-1);
        let written = self.bounds.get(&archetype).copied();
        self.ev(&format!(
            "\"ev\":\"bounds\",\"arch\":\"{archetype}\",\"lod\":\"{}\",\"r\":\"{result}\",\"e\":\"{}\"",
            f(lod),
            written.map_or_else(|| "-".to_owned(), |e| words(&e))
        ));
        (result, written)
    }

    fn render_instance(
        &mut self,
        camera: &mut dyn Camera,
        instance: Instance,
        threshold: f32,
        flags: u32,
        extra: Option<MaterialId>,
    ) {
        self.ev(&format!(
            "\"ev\":\"render\",\"inst\":\"{instance}\",\"t\":\"{}\",\"flags\":\"{flags:x}\",\"matrix\":\"-\",\"extra\":\"{}\"",
            f(threshold),
            self.number(extra)
        ));
        if let Some(spheres) = self.probe.clone() {
            self.probe(camera, &spheres);
        }
        for (counter, add) in self.counters.iter_mut().zip(self.vis) {
            *counter = counter.wrapping_add(add);
        }
    }

    fn reset_visible(&mut self) {
        self.counters = [0; 3];
    }

    fn visible(&mut self) -> [u32; 3] {
        self.counters
    }

    fn dump(&mut self, flags: i32, text: &str) {
        self.ev(&format!(
            "\"ev\":\"dump\",\"flags\":\"{flags:x}\",\"text\":\"{}\"",
            hex(text.as_bytes())
        ));
    }
}

fn op(log: &Log, case: usize, body: &str) {
    log.borrow_mut()
        .push(format!("{{\"case\":\"{case}\",\"op\":{body}}}"));
}

fn shadow_list(field: &str) -> Vec<ShadowInstance> {
    if field == "-" {
        return Vec::new();
    }
    field
        .split(',')
        .map(|pair| {
            let (instance, radius) = pair.split_once(':').expect("inst:radius");
            ShadowInstance {
                instance: dec(instance) as Instance,
                radius: float(radius),
            }
        })
        .collect()
}

fn configure_camera(camera: &mut InnerCamera, f: &[&str]) {
    match f[1] {
        "pos" => camera.position = floats(&f[2..]),
        "orient" => camera.orientation = floats(&f[2..]),
        "view" => camera.view = floats(&f[2..]),
        "unproj" => camera.unproject = floats(&f[2..]),
        "vp" => {
            for (slot, field) in camera.viewport.iter_mut().zip(&f[2..]) {
                *slot = dec(field) as i32;
            }
        }
        "hfov" => camera.half_fov = float(f[2]),
        "fovy" => camera.fov_y = float(f[2]),
        "znear" => camera.z_near = float(f[2]),
        "zfar" => camera.z_far = float(f[2]),
        "aspect" => camera.aspect = float(f[2]),
        "xscale" => camera.x_scale = double(f[2]),
        "yscale" => camera.y_scale = double(f[2]),
        "proj" => {
            camera.project_result = dec(f[2]) as i32;
            camera.project = floats(&f[3..]);
        }
        "classify" => camera.classify_result = dec(f[2]) as i32,
        other => panic!("camera field {other}"),
    }
}

/// One case: its configuration, then its operations.
fn run_case(log: &Log, case: usize, line: &str) {
    let mut rec = Rec::new(log.clone(), case);
    let mut camera = InnerCamera::new(log.clone(), case);
    let mut appearance: Option<Appearance> = None;
    for text in line.split(" ; ") {
        let f: Vec<&str> = text.split(' ').collect();
        match f[0] {
            "fail" => {
                rec.fails.insert(f[1].to_owned());
            }
            "mat" => {
                let index = dec(f[1]) as usize;
                rec.mat_results.insert(index, dec(f[2]) as i32);
                rec.mat_ids
                    .insert(index, if f[3] == "-" { 0 } else { dec(f[3]) as u32 });
            }
            "ability" => rec.ability = (f[1] != "-").then(|| dec(f[1]) as u32),
            "shadowtex" => {
                rec.shadow_find = dec(f[1]) as i32;
                rec.shadow_frame = dec(f[2]) as i32;
                rec.shadow_handle = dec(f[3]) as u32;
            }
            "lock" => rec.lock_result = dec(f[1]) as i32,
            "xf" => {
                rec.transforms
                    .insert(dec(f[1]) as Instance, floats(&f[2..]));
            }
            "arch" => {
                rec.archetypes
                    .insert(dec(f[1]) as Instance, dec(f[2]) as Archetype);
            }
            "bounds" => {
                let archetype = dec(f[1]) as Archetype;
                rec.bounds_results.insert(archetype, dec(f[2]) as i32);
                if f[3] != "-" {
                    let parts: Vec<&str> = f[3].split(',').collect();
                    rec.bounds.insert(archetype, floats(&parts));
                }
            }
            "cam" => configure_camera(&mut camera, &f),
            "probe" => {
                let spheres = if f[1] == "-" {
                    Vec::new()
                } else {
                    f[1].split('/')
                        .map(|s| {
                            let v: Vec<&str> = s.split(',').collect();
                            (floats::<3>(&v), float(v[3]))
                        })
                        .collect()
                };
                rec.probe = Some(spheres);
            }
            "vis" => {
                for (slot, field) in rec.vis.iter_mut().zip(&f[1..]) {
                    *slot = dec(field) as u32;
                }
            }
            "create" => {
                let made = Appearance::create(&mut rec);
                let result = made.as_ref().map_or_else(|e| *e, |_| 0);
                op(
                    log,
                    case,
                    &format!(
                        "\"create\",\"r\":\"{result}\",\"made\":\"{}\"",
                        u8::from(made.is_ok())
                    ),
                );
                appearance = made.ok();
            }
            _ => run_op(
                log,
                case,
                &f,
                &mut rec,
                &mut camera,
                appearance.as_mut().expect("a created appearance"),
            ),
        }
        if f[0] == "release" {
            if let Some(made) = appearance.take() {
                made.release(&mut rec);
            }
            op(log, case, "\"release\",\"r\":\"0\"");
        }
    }
}

fn run_op(
    log: &Log,
    case: usize,
    f: &[&str],
    rec: &mut Rec,
    camera: &mut InnerCamera,
    appearance: &mut Appearance,
) {
    let body = match f[0] {
        "init" => {
            let result = appearance.initialize(rec);
            format!(
                "\"init\",\"r\":\"{result}\",\"engine\":\"{}\"",
                u8::from(rec.engine)
            )
        }
        "set" => {
            let value = InstanceValue::Word(u32::from_str_radix(f[2], 16).expect("a word"));
            let result = appearance.set_instance_value(rec, dec(f[1]) as i32, value);
            format!("\"set\",\"r\":\"{result}\"")
        }
        "get" => {
            let (result, value) = match appearance.instance_value(rec, dec(f[1]) as i32) {
                Ok(InstanceValue::Word(word)) => (0, word),
                Ok(InstanceValue::Name(_)) => panic!("the suite stores words"),
                Err(e) => (e, UNSET),
            };
            format!("\"get\",\"r\":\"{result}\",\"v\":\"{value:08x}\"")
        }
        "remove" => {
            let result = appearance.remove_instance_value(rec, dec(f[1]) as i32);
            format!("\"remove\",\"r\":\"{result}\"")
        }
        "lookup" => {
            let found = rec.values.word(dec(f[1]) as i32);
            format!(
                "\"lookup\",\"r\":\"{}\",\"v\":\"{:08x}\"",
                u8::from(found.is_some()),
                found.unwrap_or(UNSET)
            )
        }
        "destroy" => {
            let result = appearance.destroy_instance(rec, dec(f[1]) as Instance);
            format!("\"destroy\",\"r\":\"{result}\"")
        }
        "light" | "lighton" => {
            let result = if f[0] == "light" {
                appearance.set_directional_light(floats(&f[1..4]), floats(&f[4..7]))
            } else {
                appearance.set_directional_light_enabled(dec(f[1]) as u8)
            };
            let light = appearance.light();
            format!(
                "\"{}\",\"r\":\"{result}\",\"on\":\"{}\",\"d\":\"{}\",\"c\":\"{}\"",
                f[0],
                light.enabled,
                words(&light.direction),
                words(&light.colour)
            )
        }
        "override" => {
            let found = appearance.override_material(dec(f[1]) as u32);
            format!(
                "\"override\",\"r\":\"{}\",\"m\":\"{}\"",
                if found.is_ok() { 0 } else { -1 },
                rec.number(found.ok())
            )
        }
        "backdrop" => {
            let result = appearance.draw_backdrop(rec, camera, dec(f[1]) as Instance);
            format!("\"backdrop\",\"r\":\"{result}\"")
        }
        "stars" => {
            let result = appearance.draw_starsphere(rec, camera, dec(f[1]) as Instance);
            format!("\"stars\",\"r\":\"{result}\"")
        }
        "fading" | "fixed" => {
            let shadows = shadow_list(f[1]);
            let result = if f[0] == "fading" {
                appearance.draw_fading_shadows(rec, camera, &shadows)
            } else {
                appearance.draw_fixed_shadows(rec, camera, &shadows)
            };
            format!(
                "\"{}\",\"r\":\"{result}\",\"scale\":\"{}\"",
                f[0],
                self::f(appearance.shadow_scale())
            )
        }
        "count" => {
            let counts = appearance
                .count_visible_geometry(rec, camera, dec(f[1]) as Instance)
                .unwrap_or([UNSET; 3]);
            format!(
                "\"count\",\"r\":\"0\",\"v\":\"{:08x},{:08x},{:08x}\"",
                counts[0], counts[1], counts[2]
            )
        }
        "blob" => match appearance.camera() {
            None => "\"blob\",\"v\":\"-\"".to_owned(),
            Some(blob) => format!(
                "\"blob\",\"b8\":\"{}\",\"b9\":\"{}\",\"a\":\"{}\",\"b\":\"{}\",\"x\":\"{}\",\"y\":\"{}\"",
                u8::from(blob.fixed_depth),
                u8::from(blob.own_transform),
                words(&blob.transform),
                words(&blob.view),
                words(&blob.right),
                words(&blob.up)
            ),
        },
        "release" => return,
        other => panic!("operation {other}"),
    };
    op(log, case, &body);
}

/// Every case's lines.
pub fn lines() -> Vec<String> {
    let log: Log = Rc::new(RefCell::new(Vec::new()));
    for (case, line) in input_lines("material_appearance.list").iter().enumerate() {
        run_case(&log, case, line);
    }
    log.take()
}
