//! The immediate-mode layer suite (`CppSource/Oracle/oracle_ui_immediate.cpp`
//! writes the same lines): one case of `ui_immediate.list` run on the
//! port's layer, line batchers and overlay pass over a recorder that writes
//! each renderer call as the C++ recorder does (the formats are listed in
//! the C++ file).

use std::collections::{BTreeMap, BTreeSet};

use neuron_client::text::font::quad::{ScreenQuadRecord, TlVertex};
use neuron_client::ui::quad_batch::{Frame, QuadBatcher, QuadDevice, vertex_bytes};
use neuron_client::ui::win3d_immediate::{
    LineVertex, QUERY_VIEWPORT, Vertex, Win3dLayer, Win3dPipeline, sphere_points,
};
use neuron_client::ui::win3d_renderables::{
    Batch, LineBatchers, Win3dView, font_atlas_key, prep_ui_overlay,
};

use super::hex;

fn words(values: &[f32]) -> String {
    let bytes: Vec<u8> = values.iter().flat_map(|v| v.to_le_bytes()).collect();
    hex(&bytes)
}

/// The recorder: render and stage states read back the last value set (0
/// before any), pipeline states answer once the case sets them, the known
/// handles are textures, the projection is the last one set.
struct Recorder {
    case: usize,
    lines: Vec<String>,
    states: BTreeMap<u32, u32>,
    stage_states: BTreeMap<(u32, u32), u32>,
    pipeline_states: BTreeMap<i32, i32>,
    known: BTreeSet<u32>,
    projection: [f32; 16],
}

impl Recorder {
    fn event(&mut self, fields: &str) {
        self.lines
            .push(format!("{{\"case\":\"{}\",{fields}}}", self.case));
    }
}

impl QuadDevice for Recorder {
    fn render_state(&mut self, state: u32) -> u32 {
        let v = self.states.get(&state).copied().unwrap_or(0);
        self.event(&format!(
            "\"ev\":\"get\",\"s\":\"{state}\",\"v\":\"{v:08x}\""
        ));
        v
    }
    fn set_render_state(&mut self, state: u32, value: u32) {
        self.states.insert(state, value);
        self.event(&format!(
            "\"ev\":\"rs\",\"s\":\"{state}\",\"v\":\"{value:08x}\""
        ));
    }
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        self.stage_states.insert((stage, state), value);
        self.event(&format!(
            "\"ev\":\"tss\",\"st\":\"{stage}\",\"t\":\"{state}\",\"v\":\"{value:08x}\""
        ));
    }
    fn set_pipeline_state(&mut self, id: i32, value: i32) {
        self.event(&format!("\"ev\":\"ps\",\"id\":\"{id}\",\"v\":\"{value}\""));
    }
    fn set_stage_texture(&mut self, texture: u32) {
        self.event(&format!(
            "\"ev\":\"tex\",\"st\":\"0\",\"h\":\"{texture:08x}\""
        ));
    }
    fn draw_indexed_quads(&mut self, vertices: &[TlVertex], indices: &[u16]) {
        let n = vertices.len();
        self.event(&format!("\"ev\":\"qvlock\",\"fvf\":\"1c4\",\"n\":\"{n}\""));
        let bytes: Vec<u8> = vertices.iter().flat_map(vertex_bytes).collect();
        self.event(&format!("\"ev\":\"qverts\",\"data\":\"{}\"", hex(&bytes)));
        self.event(&format!("\"ev\":\"qilock\",\"n\":\"{}\"", indices.len()));
        let idx: Vec<u8> = indices.iter().flat_map(|i| i.to_le_bytes()).collect();
        self.event(&format!("\"ev\":\"qidx\",\"data\":\"{}\"", hex(&idx)));
        self.event("\"ev\":\"qselect\",\"base\":\"0\"");
        self.event(&format!(
            "\"ev\":\"qdraw\",\"prim\":\"4\",\"min\":\"0\",\"n\":\"{n}\",\"start\":\"0\",\"count\":\"{}\"",
            indices.len()
        ));
    }
}

fn vertex_data(vertices: &[Vertex]) -> String {
    let bytes: Vec<u8> = vertices.iter().flat_map(Vertex::bytes).collect();
    hex(&bytes)
}

impl Win3dPipeline for Recorder {
    fn stage_state(&mut self, stage: u32, state: u32) -> u32 {
        let v = self.stage_states.get(&(stage, state)).copied().unwrap_or(0);
        self.event(&format!(
            "\"ev\":\"gtss\",\"st\":\"{stage}\",\"t\":\"{state}\",\"v\":\"{v:08x}\""
        ));
        v
    }
    fn pipeline_state(&mut self, id: i32) -> Option<i32> {
        let v = self.pipeline_states.get(&id).copied();
        let r = v.map_or_else(|| "-".to_owned(), |v| v.to_string());
        self.event(&format!("\"ev\":\"gps\",\"id\":\"{id}\",\"r\":\"{r}\""));
        v
    }
    fn is_texture(&mut self, handle: u32) -> bool {
        let known = self.known.contains(&handle);
        self.event(&format!(
            "\"ev\":\"istex\",\"h\":\"{handle:08x}\",\"r\":\"{}\"",
            u8::from(known)
        ));
        known
    }
    fn set_modelview(&mut self, m: &[f32; 12]) {
        self.event(&format!("\"ev\":\"mv\",\"m\":\"{}\"", words(m)));
    }
    fn set_world(&mut self, m: &[f32; 12]) {
        self.event(&format!("\"ev\":\"world\",\"m\":\"{}\"", words(m)));
    }
    fn set_view(&mut self, m: &[f32; 12]) {
        self.event(&format!("\"ev\":\"view\",\"m\":\"{}\"", words(m)));
    }
    fn set_ortho(&mut self, left: f32, right: f32, bottom: f32, top: f32, near: f32, far: f32) {
        let a = words(&[left, right, bottom, top, near, far]);
        self.event(&format!("\"ev\":\"ortho\",\"a\":\"{a}\""));
    }
    fn set_perspective(&mut self, fov_deg: f32, aspect: f32, near: f32, far: f32) {
        let a = words(&[fov_deg, aspect, near, far]);
        self.event(&format!("\"ev\":\"persp\",\"a\":\"{a}\""));
    }
    fn projection(&mut self) -> [f32; 16] {
        self.event("\"ev\":\"gproj\"");
        self.projection
    }
    fn set_projection(&mut self, m: &[f32; 16]) {
        self.projection = *m;
        self.event(&format!("\"ev\":\"sproj\",\"m\":\"{}\"", words(m)));
    }
    fn set_viewport_rect(&mut self, x: i32, y: i32, width: i32, height: i32) {
        self.event(&format!(
            "\"ev\":\"vp\",\"x\":\"{x}\",\"y\":\"{y}\",\"w\":\"{width}\",\"h\":\"{height}\""
        ));
    }
    fn draw_primitive(&mut self, prim: u32, vertices: &[Vertex]) {
        self.event(&format!(
            "\"ev\":\"draw\",\"prim\":\"{prim}\",\"fvf\":\"142\",\"n\":\"{}\",\"data\":\"{}\"",
            vertices.len(),
            vertex_data(vertices)
        ));
    }
    fn draw_indexed_primitive(&mut self, prim: u32, vertices: &[Vertex], indices: &[u16]) {
        let idx: Vec<u8> = indices.iter().flat_map(|i| i.to_le_bytes()).collect();
        self.event(&format!(
            "\"ev\":\"drawi\",\"prim\":\"{prim}\",\"fvf\":\"142\",\"n\":\"{}\",\"data\":\"{}\",\"idx\":\"{}\"",
            vertices.len(),
            vertex_data(vertices),
            hex(&idx)
        ));
    }
    fn draw_line_batch(&mut self, vertices: &[LineVertex], indices: &[u16]) {
        let n = vertices.len();
        let bytes: Vec<u8> = vertices.iter().flat_map(LineVertex::bytes).collect();
        self.event(&format!(
            "\"ev\":\"lverts\",\"fvf\":\"42\",\"n\":\"{n}\",\"data\":\"{}\"",
            hex(&bytes)
        ));
        let idx: Vec<u8> = indices.iter().flat_map(|i| i.to_le_bytes()).collect();
        self.event(&format!(
            "\"ev\":\"lidx\",\"n\":\"{}\",\"data\":\"{}\"",
            indices.len(),
            hex(&idx)
        ));
        self.event("\"ev\":\"selvb\"");
        self.event("\"ev\":\"selib\",\"base\":\"0\"");
        self.event(&format!(
            "\"ev\":\"ldraw\",\"prim\":\"4\",\"min\":\"0\",\"n\":\"{n}\",\"start\":\"0\",\"count\":\"{}\"",
            indices.len()
        ));
    }
}

fn h32(field: &str) -> u32 {
    u32::from_str_radix(field, 16).unwrap()
}

fn f(field: &str) -> f32 {
    f32::from_bits(h32(field))
}

fn int(field: &str) -> i32 {
    field.parse().unwrap()
}

fn state_line(layer: &Win3dLayer) -> String {
    let (kind, params) = layer.projection_cache();
    let vp = layer.get_viewport(QUERY_VIEWPORT).unwrap_or([0; 4]);
    format!(
        "\"ev\":\"state\",\"colour\":\"{:08x}\",\"nv\":\"{}\",\"ni\":\"{}\",\"kind\":\"{kind}\",\
         \"params\":\"{}\",\"set\":\"{}\",\"skip\":\"{}\",\"depth\":\"{} {} {}\",\"blend\":\"{} {} {}\",\
         \"vp\":\"{} {} {} {}\",\"mv\":\"{}\"",
        layer.colour,
        layer.vertex_count(),
        layer.index_count(),
        words(&params),
        layer.set_count,
        layer.skip_count,
        u8::from(layer.depth_test),
        layer.depth_write,
        layer.depth_func,
        layer.alpha_blend,
        layer.src_blend,
        layer.dest_blend,
        vp[0],
        vp[1],
        vp[2],
        vp[3],
        words(&layer.modelview)
    )
}

/// The lines of one case of `ui_immediate.list`.
#[allow(clippy::too_many_lines)]
pub fn lines(case: usize, line: &str) -> Vec<String> {
    let mut rec = Recorder {
        case,
        lines: Vec::new(),
        states: BTreeMap::new(),
        stage_states: BTreeMap::new(),
        pipeline_states: BTreeMap::new(),
        known: BTreeSet::new(),
        projection: [0.0; 16],
    };
    let mut layer = Win3dLayer::new(800, 600);
    let mut batchers = LineBatchers::new();
    batchers.reset();
    let mut quads = QuadBatcher::new();
    let mut camera = Win3dView {
        fov_y_deg: 0.0,
        aspect: 0.0,
        z_near: 0.0,
        z_far: 0.0,
        viewport: [0; 4],
        view: [0.0; 12],
    };
    for op in line.split(" ; ") {
        let x: Vec<&str> = op.split(' ').collect();
        match x[0] {
            "surface" => {
                layer.surface = (int(x[1]).cast_unsigned(), int(x[2]).cast_unsigned());
            }
            "reset" => layer.reset_state(),
            "begin" => layer.begin(int(x[1]).cast_unsigned()),
            "end" => layer.end(&mut rec),
            "v" => layer.vertex(f(x[1]), f(x[2]), f(x[3])),
            "c4f" => layer.color4f(f(x[1]), f(x[2]), f(x[3]), f(x[4])),
            "c3f" => layer.color3f(f(x[1]), f(x[2]), f(x[3])),
            "c3ub" => layer.color3ub(
                x[1].parse().unwrap(),
                x[2].parse().unwrap(),
                x[3].parse().unwrap(),
            ),
            "c4ub" => layer.color4ub(
                x[1].parse().unwrap(),
                x[2].parse().unwrap(),
                x[3].parse().unwrap(),
                x[4].parse().unwrap(),
            ),
            "tc" => layer.tex_coord(f(x[1]), f(x[2])),
            "tex" => layer.bind_texture(h32(x[1])),
            "en" => layer.enable(&mut rec, h32(x[1])),
            "dis" => layer.disable(&mut rec, h32(x[1])),
            "linear" => layer.set_texture_filter_linear(&mut rec),
            "point" => layer.set_texture_filter_point(&mut rec),
            "zfunc" => layer.set_depth_func(&mut rec, int(x[1]).cast_unsigned()),
            "blend" => layer.set_blend_func(
                &mut rec,
                int(x[1]).cast_unsigned(),
                int(x[2]).cast_unsigned(),
            ),
            "zwrite" => layer.set_depth_write(&mut rec, int(x[1]).cast_unsigned()),
            "vp" => layer.set_viewport(&mut rec, None),
            "vpcam" => layer.set_viewport(&mut rec, Some(camera.viewport)),
            "vprect" => {
                layer.set_viewport_rect(&mut rec, int(x[1]), int(x[2]), int(x[3]), int(x[4]));
            }
            "getvp" => {
                let q = h32(x[1]).cast_signed();
                let v = layer.get_viewport(q).unwrap_or([-1; 4]);
                rec.event(&format!(
                    "\"ev\":\"getvp\",\"q\":\"{q:x}\",\"v\":\"{} {} {} {}\"",
                    v[0], v[1], v[2], v[3]
                ));
            }
            "ortho" => layer.set_projection(
                &mut rec,
                [f(x[1]), f(x[2]), f(x[3]), f(x[4]), f(x[5]), f(x[6])],
            ),
            "ortho2d" => layer.set_ortho_2d(&mut rec),
            "orthocam" => layer.set_ortho_2d_for(&mut rec, camera.viewport),
            "persp" => layer.set_perspective(&mut rec, f(x[1]), f(x[2]), f(x[3]), f(x[4])),
            "applycam" => camera.apply(&mut layer, &mut rec),
            "push" => layer.push_projection(&mut rec),
            "pop" => layer.pop_projection(&mut rec),
            "cache" => layer.cache_enabled = int(x[1]) != 0,
            "cam" => {
                camera = Win3dView {
                    fov_y_deg: f(x[1]),
                    aspect: f(x[2]),
                    z_near: f(x[3]),
                    z_far: f(x[4]),
                    viewport: [int(x[5]), int(x[6]), int(x[7]), int(x[8])],
                    view: std::array::from_fn(|i| f(x[9 + i])),
                };
            }
            "ps" => {
                rec.pipeline_states.insert(int(x[1]), int(x[2]));
            }
            "known" => {
                rec.known.insert(h32(x[1]));
            }
            "get" => {
                rec.states.insert(int(x[1]).cast_unsigned(), h32(x[2]));
            }
            "clear" => layer.set_pipeline_clear_color(&mut rec, f(x[1]), f(x[2]), f(x[3]), f(x[4])),
            "sphere" => {
                let (cols, rows) = (
                    usize::try_from(int(x[2])).unwrap(),
                    usize::try_from(int(x[3])).unwrap(),
                );
                let points = sphere_points(f(x[1]), cols, rows);
                layer.draw_sphere_grid(&mut rec, &points, cols, rows);
            }
            "line" => {
                let batch = if x[1] == "A" { Batch::A } else { Batch::B };
                batchers.add_line(
                    batch,
                    &mut layer,
                    &mut rec,
                    &camera,
                    [f(x[2]), f(x[3]), f(x[4])],
                    [f(x[5]), f(x[6]), f(x[7])],
                    h32(x[8]),
                    h32(x[9]),
                    f(x[10]),
                );
            }
            "flush" => batchers.flush_all(&mut layer, &mut rec, &camera),
            "breset" => batchers.reset(),
            "font" => {
                quads.set_material_key(font_atlas_key());
                quads.begin_frame(Frame::texture_only(h32(x[6])), 2, false);
                quads.append_record(ScreenQuadRecord {
                    x0: f(x[1]),
                    y0: f(x[2]),
                    x1: f(x[3]),
                    y1: f(x[4]),
                    depth_a: 0.0,
                    depth_b: 1.0,
                    u0: 0.0,
                    v0: 0.0,
                    u1: 1.0,
                    v1: 1.0,
                    color: h32(x[5]),
                });
            }
            "overlay" => prep_ui_overlay(&mut layer, &mut rec, &mut quads, font_atlas_key()),
            "state" => {
                let line = state_line(&layer);
                rec.event(&line);
            }
            other => panic!("unknown operation {other}"),
        }
    }
    rec.lines
}
