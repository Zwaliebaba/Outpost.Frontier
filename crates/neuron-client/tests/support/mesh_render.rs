//! The P4.6 mesh render suite: `render::mesh` against recording services
//! with the contract `CppSource/Oracle/oracle_mesh_render.cpp` states
//! (its header lists the lines), over the archives and operations of
//! `tests/fixtures/oracle/mesh_render.list` (`make_mesh_fixtures.py`).

use std::cell::RefCell;
use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::rc::Rc;

use neuron_client::render::material::{
    DrawArgs, Material, MaterialId, Materials, SubmitContext, VbListing, VertexLock, VertexSource,
    VertexStream,
};
use neuron_client::render::mesh::manager::{DrawRequest, GeometryQuery};
use neuron_client::render::mesh::wire::draw_wire;
use neuron_client::render::mesh::{
    FIELD20_ENGINE, Instance, MeshCamera, MeshIid, MeshServices, RenderComponent, RenderManager,
    SpherePlugin, TriMeshPlugin, VMeshLibraryLoad, VMeshPlugin, VMeshRegistry, VMeshWire,
};
use neuron_core::hash::name_crc32;
use neuron_core::math::Transform;
use neuron_core::vfs::{FileSystem, FsDesc, UtfFileSystem};

use super::{fixtures, hex, input_lines};

type Log = Rc<RefCell<Vec<String>>>;

fn fnv(bytes: &[u8]) -> String {
    super::data::fnv64(bytes)
}

fn f(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn f3(v: [f32; 3]) -> String {
    format!("{},{},{}", f(v[0]), f(v[1]), f(v[2]))
}

fn w12(m: &[f32; 12]) -> String {
    m.iter().map(|&v| f(v)).collect::<Vec<_>>().join(",")
}

fn hex_word(text: &str) -> u32 {
    u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn hex_float(text: &str) -> f32 {
    f32::from_bits(hex_word(text))
}

fn hex_floats(text: &str) -> Vec<f32> {
    text.split(',').map(hex_float).collect()
}

fn dec(text: &str) -> i32 {
    text.parse().unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn u16_bytes(values: &[u16]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
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

fn source_text(d: Option<&VertexSource>) -> String {
    let Some(d) = d else {
        return "-".to_owned();
    };
    let mut out = format!("{:x},{},{:x}", d.fvf, d.count, d.flags);
    for s in [&d.position, &d.normal, &d.diffuse, &d.tex[0], &d.tex[1]] {
        out += &stream_text(s, d.count);
    }
    out
}

/// A material the arena hands an id to; the fake keeps its state.
pub struct Dummy;

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

struct Mat {
    name: String,
    crc: u32,
    flags: u32,
    class: String,
    param: Option<u32>,
    refs: u32,
    id: MaterialId,
}

struct Fake {
    log: Log,
    case: usize,
    state: Option<i32>,
    lock_fail: bool,
    next_base: u32,
    next_vb: u32,
    next_ib: u32,
    live_vb: Vec<u32>,
    live_ib: Vec<u32>,
    ib_data: HashMap<u32, Vec<u16>>,
    scratch_next: u32,
    transforms: HashMap<Instance, [f32; 12]>,
    archetypes: HashMap<Instance, i32>,
    extents: HashMap<Instance, (f32, [f32; 3])>,
    kids: HashMap<Instance, Vec<Instance>>,
    materials: Vec<Mat>,
}

const IDENTITY: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];

fn event(log: &Log, case: usize, body: &str) {
    log.borrow_mut()
        .push(format!("{{\"case\":\"{case}\",{body}}}"));
}

impl Fake {
    fn ev(&self, body: &str) {
        event(&self.log, self.case, body);
    }

    fn mat(&mut self, id: MaterialId) -> &mut Mat {
        self.materials
            .iter_mut()
            .find(|m| m.id == id)
            .expect("a known material")
    }

    fn name_of(&self, id: Option<MaterialId>) -> String {
        id.and_then(|id| self.materials.iter().find(|m| m.id == id))
            .map_or_else(|| "-".to_owned(), |m| m.name.clone())
    }

    fn context(&self, c: &SubmitContext<'_>) -> String {
        let vb = c.vb.map_or_else(
            || "-".to_owned(),
            |l| format!("{:x},{:x},{:x}", l.vertex_buffer, l.fvf, l.base_vertex),
        );
        let ib = c.ib.map_or_else(
            || "-".to_owned(),
            |s| format!("{:x},{:x}", s.index_buffer, s.bias),
        );
        let f20 = match c.field20 {
            0 => "0".to_owned(),
            FIELD20_ENGINE => "engine".to_owned(),
            _ => "?".to_owned(),
        };
        format!(
            "\"inst\":\"{}\",\"world\":\"{}\",\"view\":\"{}\",\"vb\":\"{vb}\",\"ib\":\"{ib}\",\"rep\":\"{}\",\"f20\":\"{f20}\"",
            c.instance,
            w12(c.world),
            w12(c.view),
            self.name_of(c.replaced)
        )
    }
}

impl MeshServices for Fake {
    fn pipeline_state(&mut self, id: u32) -> Option<i32> {
        match self.state {
            Some(v) => {
                self.ev(&format!("\"ev\":\"pstate\",\"id\":\"{id}\",\"r\":\"{v}\""));
                Some(v)
            }
            None => {
                self.ev(&format!("\"ev\":\"pstate\",\"id\":\"{id}\",\"r\":\"-\""));
                None
            }
        }
    }
    fn set_modelview(&mut self, m: &[f32; 12]) {
        self.ev(&format!("\"ev\":\"modelview\",\"m\":\"{}\"", w12(m)));
    }
    fn set_world(&mut self, m: &[f32; 12]) {
        self.ev(&format!("\"ev\":\"world\",\"m\":\"{}\"", w12(m)));
    }
    fn set_view(&mut self, m: &[f32; 12]) {
        self.ev(&format!("\"ev\":\"view\",\"m\":\"{}\"", w12(m)));
    }
    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        if self.lock_fail {
            self.lock_fail = false;
            self.ev(&format!(
                "\"ev\":\"lockvm\",\"fvf\":\"{fvf:x}\",\"n\":\"{count}\",\"r\":\"-1\""
            ));
            return Err(-1);
        }
        self.ev(&format!(
            "\"ev\":\"lockvm\",\"fvf\":\"{fvf:x}\",\"n\":\"{count}\",\"r\":\"0\""
        ));
        let listing = VbListing {
            vertex_buffer: 0x900,
            fvf,
            base_vertex: self.next_base,
        };
        self.next_base = self.next_base.wrapping_add(count);
        Ok(VertexLock {
            listing,
            bytes: vec![0; count as usize * 64 + 64],
            offset: 0,
        })
    }
    fn copy_vertex_data(
        &mut self,
        _dest: &mut [u8],
        fvf: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    ) -> i32 {
        self.ev(&format!(
            "\"ev\":\"copyvd\",\"fvf\":\"{fvf:x}\",\"start\":\"{start}\",\"n\":\"{count}\",\"src\":\"{}\"",
            source_text(Some(source))
        ));
        0
    }
    fn unlock_vertex_memory(&mut self, lock: VertexLock) {
        self.ev(&format!(
            "\"ev\":\"unlockvm\",\"vb\":\"{:x}\",\"base\":\"{:x}\"",
            lock.listing.vertex_buffer, lock.listing.base_vertex
        ));
    }
    fn select_vb(&mut self, vb: u32) -> i32 {
        let r = if self.live_vb.contains(&vb) { 0 } else { -1 };
        self.ev(&format!("\"ev\":\"selvb\",\"vb\":\"{vb:x}\",\"r\":\"{r}\""));
        r
    }
    fn create_vb(&mut self, fvf: u32, count: u32) -> Result<u32, i32> {
        let handle = self.next_vb;
        self.next_vb += 1;
        self.live_vb.push(handle);
        self.ev(&format!("\"ev\":\"createvb\",\"fvf\":\"{fvf:x}\",\"n\":\"{count}\",\"r\":\"0\",\"vb\":\"{handle:x}\""));
        Ok(handle)
    }
    fn lock_vb(&mut self, vb: u32, count: u32) -> i32 {
        self.ev(&format!(
            "\"ev\":\"lockvb\",\"vb\":\"{vb:x}\",\"n\":\"{count}\",\"r\":\"0\""
        ));
        0
    }
    fn copy_vertices(
        &mut self,
        vb: u32,
        base: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    ) -> (i32, u32) {
        self.ev(&format!(
            "\"ev\":\"copyv\",\"vb\":\"{vb:x}\",\"base\":\"{base}\",\"start\":\"{start}\",\"n\":\"{count}\",\"src\":\"{}\"",
            source_text(Some(source))
        ));
        (0, base)
    }
    fn unlock_vb(&mut self, vb: u32) {
        self.ev(&format!("\"ev\":\"unlockvb\",\"vb\":\"{vb:x}\""));
    }
    fn destroy_vb(&mut self, vb: u32) {
        self.ev(&format!("\"ev\":\"destroyvb\",\"vb\":\"{vb:x}\""));
        self.live_vb.retain(|&h| h != vb);
    }
    fn select_ib(&mut self, ib: u32, base: u32) -> i32 {
        let r = if ib == u32::MAX || self.live_ib.contains(&ib) {
            0
        } else {
            -1
        };
        self.ev(&format!(
            "\"ev\":\"selib\",\"ib\":\"{ib:x}\",\"base\":\"{base}\",\"r\":\"{r}\""
        ));
        r
    }
    fn create_ib(&mut self, count: u32) -> Result<u32, i32> {
        let handle = self.next_ib;
        self.next_ib += 1;
        self.live_ib.push(handle);
        self.ib_data.insert(handle, vec![0; count as usize]);
        self.ev(&format!(
            "\"ev\":\"createib\",\"n\":\"{count}\",\"r\":\"0\",\"ib\":\"{handle:x}\""
        ));
        Ok(handle)
    }
    fn lock_ib(&mut self, ib: u32, count: u32) -> i32 {
        self.ev(&format!(
            "\"ev\":\"lockib\",\"ib\":\"{ib:x}\",\"n\":\"{count}\",\"r\":\"0\""
        ));
        0
    }
    fn unlock_ib(&mut self, ib: u32, written: &[u16]) {
        let data = self.ib_data.entry(ib).or_default();
        for (slot, &value) in data.iter_mut().zip(written) {
            *slot = value;
        }
        let h = fnv(&u16_bytes(data));
        self.ev(&format!(
            "\"ev\":\"unlockib\",\"ib\":\"{ib:x}\",\"h\":\"{h}\""
        ));
    }
    fn destroy_ib(&mut self, ib: u32) {
        self.ev(&format!("\"ev\":\"destroyib\",\"ib\":\"{ib:x}\""));
        self.live_ib.retain(|&h| h != ib);
    }
    fn copy_scratch_indices(&mut self, indices: &[u16]) -> u32 {
        let start = self.scratch_next;
        self.ev(&format!(
            "\"ev\":\"copyidx\",\"n\":\"{}\",\"h\":\"{}\",\"start\":\"{start}\"",
            indices.len(),
            fnv(&u16_bytes(indices))
        ));
        self.scratch_next += u32::try_from(indices.len()).unwrap_or(u32::MAX);
        start
    }
    fn draw_indexed(&mut self, prim: u32, min: u32, nv: u32, start: u32, count: u32) -> i32 {
        self.ev(&format!(
            "\"ev\":\"dip\",\"prim\":\"{prim}\",\"min\":\"{min}\",\"nv\":\"{nv}\",\"start\":\"{start}\",\"n\":\"{count}\""
        ));
        0
    }
    fn instance_transform(&mut self, instance: Instance) -> [f32; 12] {
        self.ev(&format!("\"ev\":\"xform\",\"inst\":\"{instance}\""));
        self.transforms.get(&instance).copied().unwrap_or(IDENTITY)
    }
    fn instance_archetype(&mut self, instance: Instance) -> i32 {
        let r = self.archetypes.get(&instance).copied().unwrap_or(-1);
        self.ev(&format!(
            "\"ev\":\"arch\",\"inst\":\"{instance}\",\"r\":\"{r}\""
        ));
        r
    }
    fn release_handle(&mut self, archetype: i32) {
        self.ev(&format!("\"ev\":\"relh\",\"arch\":\"{archetype}\""));
    }
    fn instance_extent(&mut self, instance: Instance) -> Option<(f32, [f32; 3])> {
        match self.extents.get(&instance).copied() {
            Some((r, c)) => {
                self.ev(&format!(
                    "\"ev\":\"getext\",\"inst\":\"{instance}\",\"r\":\"{}\",\"c\":\"{}\"",
                    f(r),
                    f3(c)
                ));
                Some((r, c))
            }
            None => {
                self.ev(&format!(
                    "\"ev\":\"getext\",\"inst\":\"{instance}\",\"r\":\"-\",\"c\":\"-\""
                ));
                None
            }
        }
    }
    fn set_instance_extent(&mut self, instance: Instance, radius: f32, centre: [f32; 3]) {
        self.ev(&format!(
            "\"ev\":\"setext\",\"inst\":\"{instance}\",\"r\":\"{}\",\"c\":\"{}\"",
            f(radius),
            f3(centre)
        ));
        self.extents.insert(instance, (radius, centre));
    }
    fn related_instance(
        &mut self,
        instance: Instance,
        previous: Option<Instance>,
    ) -> Option<Instance> {
        let kids = self.kids.get(&instance).cloned().unwrap_or_default();
        let next = match previous {
            None => kids.first().copied(),
            Some(p) => kids.windows(2).find(|w| w[0] == p).map(|w| w[1]),
        };
        let show = |v: Option<Instance>| v.map_or_else(|| "-1".to_owned(), |v| v.to_string());
        self.ev(&format!(
            "\"ev\":\"child\",\"inst\":\"{instance}\",\"prev\":\"{}\",\"r\":\"{}\"",
            show(previous),
            show(next)
        ));
        next
    }
    fn instance_root(&mut self, instance: Instance) {
        self.ev(&format!("\"ev\":\"root\",\"inst\":\"{instance}\""));
    }
    fn find_material(&mut self, crc: u32) -> (i32, Option<MaterialId>) {
        let found = self.materials.iter_mut().find(|m| m.crc == crc);
        match found {
            Some(m) => {
                m.refs += 1;
                let (name, id) = (m.name.clone(), m.id);
                self.ev(&format!(
                    "\"ev\":\"findmat\",\"crc\":\"{crc:08x}\",\"r\":\"0\",\"m\":\"{name}\""
                ));
                (0, Some(id))
            }
            None => {
                self.ev(&format!(
                    "\"ev\":\"findmat\",\"crc\":\"{crc:08x}\",\"r\":\"-1\",\"m\":\"-\""
                ));
                (-1, None)
            }
        }
    }
    fn add_ref_material(&mut self, id: MaterialId) {
        let m = self.mat(id);
        m.refs += 1;
        let line = format!(
            "\"ev\":\"addref\",\"m\":\"{}\",\"refs\":\"{}\"",
            m.name, m.refs
        );
        self.ev(&line);
    }
    fn release_material(&mut self, id: MaterialId) {
        let m = self.mat(id);
        m.refs = m.refs.saturating_sub(1);
        let line = format!(
            "\"ev\":\"release\",\"m\":\"{}\",\"refs\":\"{}\"",
            m.name, m.refs
        );
        self.ev(&line);
    }
    fn material_flags(&mut self, id: MaterialId) -> u32 {
        let m = self.mat(id);
        let (line, v) = (
            format!(
                "\"ev\":\"flags\",\"m\":\"{}\",\"v\":\"{:x}\"",
                m.name, m.flags
            ),
            m.flags,
        );
        self.ev(&line);
        v
    }
    fn material_class(&mut self, id: MaterialId) -> Vec<u8> {
        let m = self.mat(id);
        let (line, v) = (
            format!(
                "\"ev\":\"class\",\"m\":\"{}\",\"v\":\"{}\"",
                m.name, m.class
            ),
            m.class.clone(),
        );
        self.ev(&line);
        v.into_bytes()
    }
    fn material_parameter_count(&mut self, id: MaterialId) -> u32 {
        let m = self.mat(id);
        let count = if m.param.is_some() { 9 } else { 0 };
        let line = format!("\"ev\":\"pcount\",\"m\":\"{}\",\"v\":\"{count}\"", m.name);
        self.ev(&line);
        count
    }
    fn material_parameter(&mut self, id: MaterialId, index: u32) -> (u32, u32) {
        let m = self.mat(id);
        let value = m.param.unwrap_or(0);
        let line = format!(
            "\"ev\":\"param\",\"m\":\"{}\",\"i\":\"{index}\",\"t\":\"1\",\"v\":\"{value:08x}\"",
            m.name
        );
        self.ev(&line);
        (1, value)
    }
    fn bind(&mut self, id: MaterialId, context: &SubmitContext<'_>) {
        let line = format!(
            "\"ev\":\"bind\",\"m\":\"{}\",{}",
            self.name_of(Some(id)),
            self.context(context)
        );
        self.ev(&line);
    }
    fn submit(&mut self, id: MaterialId, context: &SubmitContext<'_>, a: &DrawArgs) {
        let mut indices = Vec::new();
        for i in a.ib_start..a.ib_start + a.index_span {
            indices.push(context.indices.get(i as usize).copied().unwrap_or(0));
        }
        let line = format!(
            "\"ev\":\"submit\",\"m\":\"{}\",{},\"prim\":\"{}\",\"start\":\"{}\",\"count\":\"{}\",\"ibs\":\"{}\",\"span\":\"{}\",\"flags\":\"{:x}\",\"src\":\"{}\",\"idx\":\"{}\"",
            self.name_of(Some(id)),
            self.context(context),
            a.prim_type,
            a.start,
            a.count,
            a.ib_start,
            a.index_span,
            a.flags,
            source_text(context.vertices),
            fnv(&u16_bytes(&indices))
        );
        self.ev(&line);
    }
    fn teardown(&mut self, id: MaterialId, context: &SubmitContext<'_>) {
        let line = format!(
            "\"ev\":\"teardown\",\"m\":\"{}\",{}",
            self.name_of(Some(id)),
            self.context(context)
        );
        self.ev(&line);
    }
    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str) {
        self.ev(&format!(
            "\"ev\":\"dump\",\"flags\":\"{flags:x}\",\"file\":\"{file}\",\"line\":\"{line:x}\",\"msg\":\"{}\"",
            hex(message.as_bytes())
        ));
    }
    fn log_event(&mut self, _name: &str, _value: f32, _flags: u32) {}
}

struct Camera {
    log: Log,
    case: usize,
    view: [f32; 12],
    viewport: [i32; 4],
    x_scale: f64,
    z_near: f32,
    position: [f32; 3],
    classify: i32,
}

impl MeshCamera for Camera {
    fn view_transform(&mut self) -> [f32; 12] {
        event(&self.log, self.case, "\"ev\":\"camview\"");
        self.view
    }
    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32 {
        event(
            &self.log,
            self.case,
            &format!(
                "\"ev\":\"classify\",\"c\":\"{}\",\"r\":\"{}\",\"res\":\"{}\"",
                f3(centre),
                f(radius),
                self.classify
            ),
        );
        self.classify
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        event(&self.log, self.case, "\"ev\":\"fov\"");
        45.0
    }
    fn viewport(&mut self) -> [i32; 4] {
        event(&self.log, self.case, "\"ev\":\"viewport\"");
        self.viewport
    }
    fn x_scale_over_near(&mut self) -> f64 {
        event(&self.log, self.case, "\"ev\":\"xscale\"");
        self.x_scale
    }
    fn z_near(&mut self) -> f32 {
        event(&self.log, self.case, "\"ev\":\"znear\"");
        self.z_near
    }
    fn position(&mut self) -> [f32; 3] {
        event(&self.log, self.case, "\"ev\":\"campos\"");
        self.position
    }
}

fn iid(name: &str) -> MeshIid {
    match name {
        "mesh" => MeshIid::Mesh,
        "hook" => MeshIid::MatHookUp,
        "renderable" => MeshIid::RenderableMesh,
        "component" => MeshIid::NeuronComponent,
        "trianim" => MeshIid::TriMeshAnim,
        "sphere" => MeshIid::SphereMesh,
        "vmesh" => MeshIid::VMeshLibrary,
        "wire" => MeshIid::VMeshWire,
        _ => MeshIid::Other,
    }
}

struct Case {
    fake: Fake,
    camera: Camera,
    manager: Option<RenderManager<Fake>>,
    registry: Rc<RefCell<VMeshRegistry>>,
    libraries: BTreeMap<String, VMeshLibraryLoad>,
    wire: VMeshWire,
    archive: Option<UtfFileSystem>,
    arena: Materials<()>,
}

impl Case {
    fn new(log: &Log, case: usize) -> Case {
        let registry = VMeshRegistry::shared();
        let plugins: Vec<Box<dyn RenderComponent<Fake>>> = vec![
            Box::new(VMeshPlugin::new(Rc::clone(&registry))),
            Box::new(TriMeshPlugin::new(false)),
            Box::new(SpherePlugin::new()),
        ];
        Case {
            fake: Fake {
                log: Rc::clone(log),
                case,
                state: Some(1),
                lock_fail: false,
                next_base: 0,
                next_vb: 0x100,
                next_ib: 0x200,
                live_vb: Vec::new(),
                live_ib: Vec::new(),
                ib_data: HashMap::new(),
                scratch_next: 0,
                transforms: HashMap::new(),
                archetypes: HashMap::new(),
                extents: HashMap::new(),
                kids: HashMap::new(),
                materials: Vec::new(),
            },
            camera: Camera {
                log: Rc::clone(log),
                case,
                view: IDENTITY,
                viewport: [0, 0, 639, 479],
                x_scale: 1.0,
                z_near: 1.0,
                position: [0.0; 3],
                classify: 4,
            },
            manager: Some(RenderManager::new(plugins)),
            registry,
            libraries: BTreeMap::new(),
            wire: VMeshWire::new(),
            archive: None,
            arena: Materials::new(),
        }
    }

    fn op(&self, body: &str) {
        self.fake.ev(body);
    }

    fn node_fs(archive: &Option<UtfFileSystem>, path: &str) -> Box<dyn FileSystem> {
        let root = archive.as_ref().expect("an archive");
        if path == "." {
            Box::new(root.clone())
        } else {
            root.create_child(&FsDesc::read(path.as_bytes()))
                .expect("a child node")
        }
    }

    fn lod_text(&self, id: i32) -> String {
        let manager = self.manager.as_ref().expect("a manager");
        let Some(set) = manager.lod_set(id) else {
            return "\"lods\":\"-\"".to_owned();
        };
        let switch = set
            .switch
            .iter()
            .map(|&v| f(v))
            .collect::<Vec<_>>()
            .join(",");
        let plugins = set
            .entries
            .iter()
            .map(|e| e.plugin.map_or(-1, |p| p as i32).to_string())
            .collect::<Vec<_>>()
            .join(",");
        format!(
            "\"lods\":\"{}\",\"cur\":\"{}\",\"switch\":\"{switch}\",\"plugins\":\"{plugins}\"",
            set.count(),
            set.current
        )
    }

    #[allow(clippy::too_many_lines)]
    fn run(&mut self, t: &[&str]) {
        let k = t[0];
        let manager = self.manager.as_mut();
        let Some(manager) = manager else {
            panic!("operation {k} after end");
        };
        let fake = &mut self.fake;
        match k {
            "fs" => {
                let bytes =
                    fs::read(fixtures().join(t[1])).unwrap_or_else(|e| panic!("{}: {e}", t[1]));
                self.archive =
                    Some(UtfFileSystem::from_bytes(bytes, t[1].as_bytes()).expect("a UTF archive"));
                self.op("\"op\":\"fs\"");
            }
            "mat" => {
                let crc = t[2]
                    .strip_prefix('#')
                    .map_or_else(|| name_crc32(t[2].as_bytes()), hex_word);
                let id = self.arena.insert(Box::new(Dummy), 1);
                fake.materials.push(Mat {
                    name: t[1].to_owned(),
                    crc,
                    flags: hex_word(t[3]),
                    class: t[4].to_owned(),
                    param: t.get(5).map(|p| hex_word(p)),
                    refs: 1,
                    id,
                });
                self.op(&format!(
                    "\"op\":\"mat\",\"m\":\"{}\",\"crc\":\"{crc:08x}\"",
                    t[1]
                ));
            }
            "xform" => {
                let v = hex_floats(t[2]);
                fake.transforms
                    .insert(hex_word(t[1]) as usize, v.try_into().expect("12 words"));
            }
            "ext" => {
                let c = hex_floats(t[3]);
                fake.extents.insert(
                    hex_word(t[1]) as usize,
                    (hex_float(t[2]), [c[0], c[1], c[2]]),
                );
            }
            "instarch" => {
                fake.archetypes.insert(hex_word(t[1]) as usize, dec(t[2]));
            }
            "kids" => {
                let list = t[2].split(',').map(|p| hex_word(p) as usize).collect();
                fake.kids.insert(hex_word(t[1]) as usize, list);
            }
            "cam" => match t[1] {
                "view" => self.camera.view = hex_floats(t[2]).try_into().expect("12 words"),
                "vp" => {
                    for i in 0..4 {
                        self.camera.viewport[i] = dec(t[2 + i]);
                    }
                }
                "xs" => {
                    self.camera.x_scale =
                        f64::from_bits(u64::from_str_radix(t[2], 16).expect("hex"))
                }
                "zn" => self.camera.z_near = hex_float(t[2]),
                "pos" => {
                    let v = hex_floats(t[2]);
                    self.camera.position = [v[0], v[1], v[2]];
                }
                "class" => self.camera.classify = dec(t[2]),
                other => panic!("cam {other}"),
            },
            "pstate" => fake.state = if t[1] == "-" { None } else { Some(dec(t[1])) },
            "failvm" => fake.lock_fail = true,
            "lib" => {
                let mut root = Self::node_fs(&self.archive, ".");
                let list =
                    self.registry
                        .borrow_mut()
                        .load_library(&mut self.fake, root.as_mut(), -1);
                let count = list
                    .as_ref()
                    .map_or_else(|| "-".to_owned(), |l| l.len().to_string());
                self.libraries.insert(t[1].to_owned(), list);
                self.op(&format!("\"op\":\"lib\",\"n\":\"{count}\""));
            }
            "unlib" => {
                let list = self.libraries.remove(t[1]).flatten();
                self.registry
                    .borrow_mut()
                    .unload_library(&mut self.fake, list);
                self.op("\"op\":\"unlib\"");
            }
            "arch" => {
                let id = dec(t[1]);
                let mut node = Self::node_fs(&self.archive, t[2]);
                let r = manager.create_archetype(fake, id, node.as_mut());
                let text = self.lod_text(id);
                self.op(&format!("\"op\":\"arch\",\"r\":\"{r}\",{text}"));
            }
            "dup" => {
                let r = manager.duplicate_archetype(fake, dec(t[1]), dec(t[2]));
                let text = self.lod_text(dec(t[1]));
                self.op(&format!("\"op\":\"dup\",\"r\":\"{r}\",{text}"));
            }
            "destroy" => {
                let r = manager.destroy_archetype(fake, dec(t[1]));
                self.op(&format!("\"op\":\"destroy\",\"r\":\"{r}\""));
            }
            "inst" => {
                let r = manager.create_instance(fake, hex_word(t[1]) as usize, dec(t[2]));
                self.op(&format!("\"op\":\"inst\",\"r\":\"{r}\""));
            }
            "uninst" => {
                let r = manager.destroy_instance(fake, hex_word(t[1]) as usize);
                self.op(&format!("\"op\":\"uninst\",\"r\":\"{r}\""));
            }
            "tick" => {
                let r = manager.update_instance(fake, hex_word(t[1]) as usize, hex_word(t[2]));
                self.op(&format!("\"op\":\"tick\",\"r\":\"{r}\""));
            }
            "notify" => {
                let r = manager.notify(fake, hex_word(t[1]));
                self.op(&format!("\"op\":\"notify\",\"r\":\"{r}\""));
            }
            "draw" => {
                let mut matrix = None;
                let mut extra = None;
                for token in &t[4..] {
                    if let Some(m) = token.strip_prefix("m=") {
                        matrix = Some(Transform::from_words(
                            hex_floats(m).try_into().expect("12 words"),
                        ));
                    } else if let Some(x) = token.strip_prefix("x=") {
                        extra = fake.materials.iter().find(|m| m.name == x).map(|m| m.id);
                    }
                }
                let request = DrawRequest {
                    flags: hex_word(t[3]),
                    matrix,
                    extra,
                };
                let r = manager.render_instance(
                    fake,
                    &mut self.camera,
                    hex_word(t[1]) as usize,
                    hex_float(t[2]),
                    &request,
                );
                self.op(&format!("\"op\":\"draw\",\"r\":\"{r}\""));
            }
            "lods" | "ilods" => {
                let (count, current) = if k == "lods" {
                    (
                        manager.archetype_lod_count(dec(t[1])),
                        manager.archetype_current_lod(dec(t[1])),
                    )
                } else {
                    let inst = hex_word(t[1]) as usize;
                    let count = manager.instance_lod_count(fake, inst);
                    (count, manager.instance_current_lod(fake, inst))
                };
                let r = |v: &Result<i32, i32>| v.map_or_else(|e| e, |_| 0);
                let v = |v: &Result<i32, i32>| v.unwrap_or(-9);
                self.op(&format!(
                    "\"op\":\"{k}\",\"r\":\"{},{}\",\"n\":\"{}\",\"cur\":\"{}\"",
                    r(&count),
                    r(&current),
                    v(&count),
                    v(&current)
                ));
            }
            "setlod" => {
                let r = manager.set_archetype_lod(dec(t[1]), dec(t[2]));
                self.op(&format!("\"op\":\"setlod\",\"r\":\"{r}\""));
            }
            "ilod" => {
                let r = manager.set_instance_lod(fake, hex_word(t[1]) as usize, dec(t[2]));
                self.op(&format!("\"op\":\"ilod\",\"r\":\"{r}\""));
            }
            "range" => {
                let (r, (a, b)) = match manager.archetype_lod_range(dec(t[1]), dec(t[2])) {
                    Ok(v) => (0, v),
                    Err(e) => (e, (-9.0, -9.0)),
                };
                self.op(&format!(
                    "\"op\":\"range\",\"r\":\"{r}\",\"a\":\"{}\",\"b\":\"{}\"",
                    f(a),
                    f(b)
                ));
            }
            "setrange" => {
                let r = manager.set_archetype_lod_range(
                    dec(t[1]),
                    dec(t[2]),
                    hex_float(t[3]),
                    hex_float(t[4]),
                );
                self.op(&format!("\"op\":\"setrange\",\"r\":\"{r}\""));
            }
            "geom" | "geomat" => {
                let query = GeometryQuery {
                    mode: dec(t[3]),
                    flags: hex_word(t[4]),
                    out: true,
                };
                let (r, out) = if k == "geom" {
                    manager.query_geometry(dec(t[1]), hex_float(t[2]), query)
                } else {
                    manager.query_geometry_at_lod(dec(t[1]), dec(t[2]), query)
                };
                self.op(&format!(
                    "\"op\":\"{k}\",\"r\":\"{r}\",\"v\":\"{}\"",
                    out.unwrap_or(-9)
                ));
            }
            "bounds" | "centroid" => {
                let (r, values) = if k == "bounds" {
                    let (r, v) = manager.bounds(dec(t[1]), hex_float(t[2]));
                    (r, v.map_or_else(|| vec![-9.0; 6], |v| v.to_vec()))
                } else {
                    let (r, v) = manager.centroid(dec(t[1]), hex_float(t[2]));
                    (r, v.map_or_else(|| vec![-9.0; 3], |v| v.to_vec()))
                };
                let v = values.iter().map(|&x| f(x)).collect::<Vec<_>>().join(",");
                self.op(&format!("\"op\":\"{k}\",\"r\":\"{r}\",\"v\":\"{v}\""));
            }
            "root" => {
                let r = manager.set_archetype_root_selection(dec(t[1]), hex_word(t[2]));
                let (r2, flags) = manager.archetype_root_selection(dec(t[1]));
                self.op(&format!(
                    "\"op\":\"root\",\"r\":\"{r},{r2}\",\"v\":\"{flags}\""
                ));
            }
            "qarch" => {
                let r = manager.query_archetype(dec(t[1]), iid(t[2]));
                self.op(&format!("\"op\":\"qarch\",\"r\":\"{r}\""));
            }
            "qinst" => {
                let r = manager.query_instance(fake, hex_word(t[1]) as usize, iid(t[2]));
                self.op(&format!("\"op\":\"qinst\",\"r\":\"{r}\""));
            }
            "wire" => {
                let mut node = Self::node_fs(&self.archive, t[2]);
                let r = self
                    .wire
                    .create_archetype(&mut self.fake, dec(t[1]), node.as_mut());
                self.op(&format!("\"op\":\"wire\",\"r\":\"{r}\""));
            }
            "wdraw" => {
                let r = match self.wire.query_archetype(dec(t[1]), MeshIid::VMeshWire) {
                    Some(wire) => draw_wire(&wire, &mut self.registry.borrow_mut(), &mut self.fake),
                    None => -1,
                };
                self.op(&format!("\"op\":\"wdraw\",\"r\":\"{r}\""));
            }
            "sphmax" | "sphradius" | "sphscale" => {
                let anim = manager
                    .find_sphere_instance(hex_word(t[1]) as usize)
                    .expect("a sphere");
                match k {
                    "sphmax" => anim.set_max_level(dec(t[2])),
                    "sphradius" => anim.set_radius(hex_float(t[2])),
                    _ => anim.set_lod_scale(hex_float(t[2])),
                }
                self.op(&format!("\"op\":\"{k}\""));
            }
            "sphlevel" => {
                let anim = manager
                    .find_sphere_instance(hex_word(t[1]) as usize)
                    .expect("a sphere");
                anim.update_level(hex_float(t[2]));
                let slot = &anim.slots[0];
                let line = format!(
                    "\"op\":\"sphlevel\",\"level\":\"{}\",\"nv\":\"{}\",\"ni\":\"{}\",\"src\":\"{}\",\"idx\":\"{}\"",
                    anim.cached_level,
                    anim.vertex_count,
                    slot.indices.len(),
                    source_text(Some(&anim.vertices)),
                    fnv(&u16_bytes(&slot.indices))
                );
                self.op(&line);
            }
            "end" => {
                let manager = self.manager.take().expect("a manager");
                manager.destroy(&mut self.fake);
                self.op("\"op\":\"end\"");
            }
            other => panic!("mesh_render: unknown operation {other}"),
        }
    }
}

/// The suite's lines.
pub fn lines() -> Vec<String> {
    let log: Log = Rc::new(RefCell::new(Vec::new()));
    for (index, line) in input_lines("mesh_render.list").iter().enumerate() {
        let mut case = Case::new(&log, index);
        for op in line.split(" ; ") {
            let tokens: Vec<&str> = op.split(' ').collect();
            case.run(&tokens);
        }
    }
    log.take()
}
