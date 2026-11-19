//! The P4.8 particle suites through the port, with the contract of
//! `CppSource/Oracle/oracle_particles.cpp`: `particle_ramp` (every ramp and
//! transform property of a node library evaluated at a list of times) and
//! `particle_sim` (effects driven by the client's calls, with dumps of the
//! node state, the particle lists, the pools and the texture calls).

use std::fmt::Write as _;

use neuron_client::render::particles::ramp::{
    AnimatedRamp, AnimatedTransform, ColorRampRamp, NestedSingle, SingleRampRamp,
};
use neuron_client::render::particles::services::{FrameOut, TextureRecord};
use neuron_client::render::particles::{
    Effect, EffectError, NodeId, ParticleFx, ParticleServices, PoolKind, pool_of,
};
use neuron_core::fx_data::props::{APPEARANCE_LOD_CURVE, kind};
use neuron_core::fx_data::{
    AleFile, ClassData, NodeClass, PropertySet, billboard_tables, name_hash,
};

use super::{input_lines, unhex};

fn f(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn floats(values: &[f32], separator: &str) -> String {
    values
        .iter()
        .map(|&v| f(v))
        .collect::<Vec<_>>()
        .join(separator)
}

fn hex_float(text: &str) -> f32 {
    f32::from_bits(u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}")))
}

// --- The archive the two streams make --------------------------------------------------

/// A UTF archive (the 0x2C layout of `neuron_core::utf`) with the two
/// directories of an `.ale` file, each holding its leaf: what the oracle's
/// in-memory file system serves.
fn ale_archive(nodes: &[u8], effects: &[u8]) -> Vec<u8> {
    let names: [&[u8]; 2] = [b"ParticleFxNodeLibrary", b"ALEffectLib"];
    let leaves = [nodes, effects];
    let mut strings = Vec::new();
    let mut name_offset = |name: &[u8]| {
        let offset = strings.len();
        strings.extend_from_slice(name);
        strings.push(0);
        u32::try_from(offset).unwrap_or(0)
    };
    // Entries: root, then the two directories, then each directory's leaf.
    let root_name = name_offset(b"\\");
    let dir_names = names.map(&mut name_offset);
    let leaf_names = names.map(&mut name_offset);
    let mut tree = vec![0_u8; 0x2C * 5];
    let mut put = |at: usize, words: &[(usize, u32)]| {
        for &(offset, value) in words {
            tree[at + offset..at + offset + 4].copy_from_slice(&value.to_le_bytes());
        }
    };
    put(0, &[(0, 0), (4, root_name), (8, 0x10), (0x10, 0x2C)]);
    let mut data = Vec::new();
    for i in 0..2 {
        let dir = 0x2C * (1 + i);
        let leaf = 0x2C * (3 + i);
        let next = if i == 0 { 0x2C * 2 } else { 0 };
        put(
            dir,
            &[
                (0, next as u32),
                (4, dir_names[i]),
                (8, 0x10),
                (0x10, leaf as u32),
            ],
        );
        put(
            leaf,
            &[
                (0, 0),
                (4, leaf_names[i]),
                (8, 0x80),
                (0x10, data.len() as u32),
                (0x1C, leaves[i].len() as u32),
            ],
        );
        data.extend_from_slice(leaves[i]);
    }
    let t = 0x38_u32;
    let s = t + tree.len() as u32;
    let d = s + strings.len() as u32;
    let head = [0x2046_5455, 0x101, t, 0, 0, 0x2C, s, 0, 0, d, 0, 0, 0, 0];
    let mut out: Vec<u8> = head.iter().flat_map(|w: &u32| w.to_le_bytes()).collect();
    out.extend_from_slice(&tree);
    out.extend_from_slice(&strings);
    out.extend_from_slice(&data);
    out
}

fn load(nodes_hex: &str, effects_hex: &str) -> AleFile {
    AleFile::parse(ale_archive(&unhex(nodes_hex), &unhex(effects_hex))).expect("an archive")
}

fn code(result: &Result<(), neuron_core::fx_data::FxError>) -> i32 {
    result
        .as_ref()
        .map_or_else(neuron_core::fx_data::FxError::code, |()| 0)
}

// --- particle_ramp --------------------------------------------------------------------

/// The probes of `oracle_fx_data.cpp`: hashes a class owns that its rows miss.
const PROBES: [(u32, u32); 6] = [
    (0x0C53_B008, 0x200),
    (0x0BA0_B3BB, 0x105),
    (0xF000_4D58, 0x105),
    (0xE462_B789, 0x202),
    (0x061A_AF28, 0x202),
    (0x16FF_3474, 0x202),
];

/// A property's run-time value as the set's getter hands it out.
enum Evaluable {
    Single(NestedSingle),
    Color(ColorRampRamp),
    Transform(AnimatedTransform),
}

/// The getter of `kind` for `hash`: a billboard set hands out its slot
/// whichever child kind it holds.
fn evaluable(set: &PropertySet, hash: u32, kind: u32) -> Option<Evaluable> {
    if let (ClassData::Billboard(b), Some(tables)) = (&set.data, billboard_tables(set.class)) {
        let slot = match kind {
            kind::SINGLE_RAMP_RAMP => {
                if let Some(&(_, slot)) = tables.single_ramps.iter().find(|(h, _)| *h == hash) {
                    Some(slot)
                } else if hash == APPEARANCE_LOD_CURVE {
                    return Some(Evaluable::Single(NestedSingle::Ramp(SingleRampRamp::new(
                        &b.lod_curve,
                    ))));
                } else {
                    None
                }
            }
            kind::ANIMATED_SINGLE_RAMP => tables
                .animated_ramps
                .iter()
                .find(|(h, _)| *h == hash)
                .map(|&(_, s)| s),
            _ => None,
        };
        if let Some(slot) = slot {
            return b
                .slot(slot)
                .map(|s| Evaluable::Single(NestedSingle::new(s)));
        }
        if kind != kind::COLOR_RAMP_RAMP && kind != kind::TRANSFORM {
            return None;
        }
    }
    match kind {
        kind::SINGLE_RAMP_RAMP => set
            .single_ramp_ramp(hash)
            .map(|r| Evaluable::Single(NestedSingle::Ramp(SingleRampRamp::new(r)))),
        kind::ANIMATED_SINGLE_RAMP => set
            .animated_single_ramp(hash)
            .map(|r| Evaluable::Single(NestedSingle::Animated(AnimatedRamp::new(r)))),
        kind::COLOR_RAMP_RAMP => set
            .color_ramp_ramp(hash)
            .map(|r| Evaluable::Color(ColorRampRamp::new(r))),
        kind::TRANSFORM => set
            .transform(hash)
            .map(|t| Evaluable::Transform(AnimatedTransform::new(t))),
        _ => None,
    }
}

/// `particle_ramp.list`.
pub fn ramp_lines() -> Vec<String> {
    let mut lines = Vec::new();
    for (case, line) in input_lines("particle_ramp.list").iter().enumerate() {
        let mut parts = line.split(' ');
        let nodes = parts.next().unwrap_or_default();
        let times: Vec<(f32, f32)> = parts
            .next()
            .unwrap_or_default()
            .split(',')
            .map(|pair| {
                let (a, b) = pair.split_once(':').unwrap_or_default();
                (hex_float(a), hex_float(b))
            })
            .collect();
        let ale = load(nodes, "");
        lines.push(format!(
            "{{\"case\":\"{case}\",\"load\":\"{}\"}}",
            code(&ale.nodes_result)
        ));
        for (index, node) in ale.nodes.nodes().iter().enumerate() {
            let set = &node.props;
            let mut rows: Vec<(u32, u32)> = set.class.rows().map(|r| (r.hash, r.kind)).collect();
            let listed = rows.clone();
            rows.extend(PROBES.iter().filter(|p| !listed.contains(p)));
            for (hash, kind) in rows {
                let Some(value) = evaluable(set, hash, kind) else {
                    continue;
                };
                let values: Vec<String> = times
                    .iter()
                    .map(|&(outer, inner)| match &value {
                        Evaluable::Single(r) => format!("{:016x}", r.eval(outer, inner).to_bits()),
                        Evaluable::Color(r) => {
                            let rgb = r.eval_rgb(outer, inner);
                            let packed = r.eval_packed(outer, inner);
                            format!("{}.{packed:08x}", floats(&rgb, "."))
                        }
                        Evaluable::Transform(t) => floats(&t.evaluate(outer, inner), "."),
                    })
                    .collect();
                lines.push(format!(
                    "{{\"case\":\"{case}\",\"node\":\"{index}\",\"hash\":\"{hash:08x}\",\"kind\":\"{kind:x}\",\"v\":\"{}\"}}",
                    values.join(",")
                ));
            }
        }
    }
    lines
}

// --- particle_sim ---------------------------------------------------------------------

/// The recording texture library of the contract.
struct Textures {
    head: String,
    entries: Vec<(u32, i32)>,
    playbacks: Vec<(i32, usize)>,
    next_id: i32,
    lines: Vec<String>,
}

impl Textures {
    fn line(&mut self, text: &str) {
        self.lines.push(format!("{}{text}}}", self.head));
    }
}

impl ParticleServices for Textures {
    fn find_texture(&mut self, _name: &[u8], crc: u32) -> Option<TextureRecord> {
        let found = self.entries.iter().position(|e| e.0 == crc);
        let r = if found.is_some() { "0" } else { "-1" };
        self.line(&format!(
            "\"ev\":\"find\",\"crc\":\"{crc:08x}\",\"r\":\"{r}\""
        ));
        found.map(|i| u32::try_from(i + 1).unwrap_or(0))
    }
    fn create_playback(&mut self, record: TextureRecord) -> i32 {
        let id = self.next_id;
        self.next_id += 1;
        self.playbacks.push((id, record as usize - 1));
        self.line(&format!(
            "\"ev\":\"play\",\"rec\":\"{record}\",\"id\":\"{id}\""
        ));
        id
    }
    fn frame_count(&mut self, record: TextureRecord) -> Option<i32> {
        let n = self.entries[record as usize - 1].1;
        self.line(&format!(
            "\"ev\":\"frames\",\"rec\":\"{record}\",\"n\":\"{n}\""
        ));
        Some(n)
    }
    fn release_texture_record(&mut self, record: TextureRecord) {
        self.line(&format!("\"ev\":\"trel\",\"rec\":\"{record}\""));
    }
    fn set_wrap_mode(&mut self, id: i32, mode: i32) {
        self.line(&format!("\"ev\":\"wrap\",\"id\":\"{id}\",\"m\":\"{mode}\""));
    }
    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<FrameOut> {
        if !self.playbacks.iter().any(|p| p.0 == id) {
            self.line(&format!(
                "\"ev\":\"frame\",\"id\":\"{id}\",\"f\":\"{frame}\",\"r\":\"-1\""
            ));
            return None;
        }
        self.line(&format!(
            "\"ev\":\"frame\",\"id\":\"{id}\",\"f\":\"{frame}\",\"r\":\"0\""
        ));
        #[allow(clippy::cast_precision_loss)]
        let u0 = 0.125_f32 * frame as f32;
        Some(FrameOut {
            handle: (id * 100 + frame).cast_unsigned(),
            uv: [u0.to_bits(), 0, 1.0_f32.to_bits(), 0.5_f32.to_bits()],
        })
    }
    fn advance_time(&mut self, id: i32, seconds: f32) {
        self.line(&format!(
            "\"ev\":\"adv\",\"id\":\"{id}\",\"dt\":\"{}\"",
            f(seconds)
        ));
    }
    fn dec_ref_texture(&mut self, id: i32) {
        self.line(&format!("\"ev\":\"decref\",\"id\":\"{id}\""));
    }
}

#[derive(Default)]
struct Slot {
    effect: Option<Effect>,
    parent: Option<NodeId>,
}

const POOLS: [(&str, PoolKind); 8] = [
    ("basic", PoolKind::Basic),
    ("particle", PoolKind::Particle),
    ("rect", PoolKind::Rect),
    ("perp", PoolKind::Perp),
    ("mesh", PoolKind::Mesh),
    ("oriented", PoolKind::Oriented),
    ("dust", PoolKind::Dust),
    ("beam", PoolKind::Beam),
];

fn record_of(effect: &Effect, node: Option<NodeId>, parent: Option<NodeId>) -> String {
    let Some(node) = node else {
        return "-".to_owned();
    };
    if let Some(index) = effect.records().iter().position(|r| r.node == node) {
        return index.to_string();
    }
    if Some(node) == parent {
        "p".to_owned()
    } else {
        "?".to_owned()
    }
}

fn dump(
    fx: &ParticleFx,
    effect: &Effect,
    parent: Option<NodeId>,
    head: &str,
    out: &mut Vec<String>,
) {
    for (index, record) in effect.records().iter().enumerate() {
        let node = fx.node(record.node).expect("a live record");
        let class = node.class();
        let mut line = format!(
            "{head}\"op\":\"node\",\"r\":\"{index}\",\"id\":\"{}\",\"kind\":\"{}\",\"class\":\"{}\",\"refs\":\"{}\",\"flags\":\"{:08x}\",",
            record.id,
            record.kind,
            String::from_utf8_lossy(class.factory_name()),
            node.refs(),
            node.flags
        );
        let _ = write!(
            line,
            "\"age\":\"{}\",\"dur\":\"{}\",\"outer\":\"{}\",\"lod\":\"{}\",\"parent\":\"{}\",\"local\":\"{}\",\"world\":\"{}\"",
            f(node.age),
            f(node.duration),
            f(node.outer_time),
            f(node.lod_input),
            record_of(effect, node.parent, parent),
            floats(&node.local, ","),
            floats(&node.world, ",")
        );
        if let Some(e) = node.emitter() {
            let _ = write!(
                line,
                ",\"app\":\"{}\",\"last\":\"{}\",\"press\":\"{}\",\"life\":\"{}\",\"reset\":\"{}\",\"prev\":\"{}\",\"frac\":\"{}\",\"x\":\"{}\"",
                record_of(effect, e.appearance, parent),
                f(e.last_emit),
                f(e.pressure),
                f(e.init_life_span),
                u8::from(e.reset_history),
                floats(&e.prev_pos, ","),
                f(e.fraction),
                floats(&e.xform, ",")
            );
            let s = e.shape;
            let shape: Vec<f32> = match class {
                NodeClass::ConeEmitter => {
                    vec![s.min_radius, s.radius_range, s.min_spread, s.spread_range]
                }
                NodeClass::CubeEmitter => {
                    vec![s.min_spread, s.spread_range, s.width, s.height, s.depth]
                }
                _ => vec![s.min_radius, s.radius_range],
            };
            let _ = write!(line, ",\"shape\":\"{}\"", floats(&shape, ","));
        } else if let Some(s) = node.field() {
            let c = floats(&s.cache, ",");
            match class {
                NodeClass::GravityField => {
                    let _ = write!(line, ",\"g\":\"{}\",\"c\":\"{c}\"", f(s.magnitude));
                }
                NodeClass::AirField => {
                    let _ = write!(
                        line,
                        ",\"ap\":\"{}\",\"mag\":\"{}\",\"bl\":\"{}\",\"c\":\"{c}\"",
                        f(s.approach),
                        f(s.magnitude),
                        f(s.blend)
                    );
                }
                NodeClass::RadialField => {
                    let _ = write!(
                        line,
                        ",\"mag\":\"{}\",\"ap\":\"{}\",\"bl\":\"{}\",\"ds\":\"{}\",\"c\":\"{c}\"",
                        f(s.magnitude),
                        f(s.approach),
                        f(s.blend),
                        f(s.dist_scale)
                    );
                }
                NodeClass::TurbulenceField => {
                    let _ = write!(
                        line,
                        ",\"ap\":\"{}\",\"bl\":\"{}\",\"mag\":\"{}\"",
                        f(s.approach),
                        f(s.blend),
                        f(s.magnitude)
                    );
                }
                NodeClass::CollideField => {
                    let _ = write!(
                        line,
                        ",\"rf\":\"{}\",\"dt\":\"{}\",\"n\":\"{}\",\"co\":\"{}\",\"sn\":\"{}\"",
                        f(s.reflect),
                        f(s.update_dt),
                        floats(&s.normal, ","),
                        floats(&s.corner, ","),
                        floats(&s.scaled_normal, ",")
                    );
                }
                NodeClass::DustField => {
                    let _ = write!(
                        line,
                        ",\"rad\":\"{}\",\"rsq\":\"{}\",\"dt\":\"{}\"",
                        f(s.radius),
                        f(s.radius_sq),
                        f(s.update_dt)
                    );
                }
                _ => {}
            }
        } else if let Some(a) = node.appearance() {
            let fields: Vec<String> = a
                .fields
                .iter()
                .map(|&field| record_of(effect, field, parent))
                .collect();
            let t = a.texture;
            let frame = [
                f32::from_bits(t.frame.handle),
                f32::from_bits(t.frame.uv[0]),
                f32::from_bits(t.frame.uv[1]),
                f32::from_bits(t.frame.uv[2]),
                f32::from_bits(t.frame.uv[3]),
            ];
            let _ = write!(
                line,
                ",\"live\":\"{}\",\"erefs\":\"{}\",\"fields\":\"{}\",\"tex\":\"{:08x},{},{},{}\"",
                a.live,
                a.emitter_refs,
                fields.join(","),
                t.crc,
                t.handle,
                t.frame_count,
                floats(&frame, ",")
            );
            if class == NodeClass::DustAppearance {
                let _ = write!(line, ",\"ldt\":\"{}\"", f(a.last_dt));
            }
            if class == NodeClass::BeamAppearance {
                let _ = write!(line, ",\"par\":\"{}\"", u8::from(a.ribbon_parity));
            }
        }
        out.push(line + "}");
        let (Some(a), Some(kind)) = (node.appearance(), pool_of(class)) else {
            continue;
        };
        let pool = fx.pools.get(kind);
        let mut cursor = a.head;
        while let Some(i) = cursor {
            let p = pool.item(i);
            let mut words = vec![
                p.pos[0],
                p.pos[1],
                p.pos[2],
                p.vel[0],
                p.vel[1],
                p.vel[2],
                p.age,
                p.duration,
                p.inv_duration,
            ];
            match class {
                NodeClass::PerpAppearance | NodeClass::DustAppearance => words.extend(p.extra),
                NodeClass::BeamAppearance => words.push(p.extra[0]),
                _ => {}
            }
            out.push(format!(
                "{head}\"op\":\"part\",\"r\":\"{index}\",\"i\":\"{i}\",\"p\":\"{}\"}}",
                floats(&words, ",")
            ));
            cursor = p.next;
        }
    }
}

fn dump_pools(fx: &ParticleFx, head: &str, out: &mut Vec<String>) {
    for (name, kind) in POOLS {
        let pool = fx.pools.get(kind);
        let used: Vec<String> = pool.used().iter().map(u32::to_string).collect();
        out.push(format!(
            "{head}\"op\":\"pool\",\"k\":\"{name}\",\"free\":\"{}\",\"used\":\"{}\"}}",
            pool.free_count(),
            used.join(",")
        ));
    }
}

fn xform(text: &str) -> [f32; 12] {
    let words: Vec<f32> = text.split(',').map(hex_float).collect();
    core::array::from_fn(|i| words[i])
}

/// `particle_sim.list`.
#[allow(clippy::too_many_lines)]
pub fn sim_lines() -> Vec<String> {
    let mut out = Vec::new();
    for (case, line) in input_lines("particle_sim.list").iter().enumerate() {
        let head = format!("{{\"case\":\"{case}\",");
        let mut fx = ParticleFx::new();
        let mut tex = Textures {
            head: head.clone(),
            entries: Vec::new(),
            playbacks: Vec::new(),
            next_id: 1,
            lines: Vec::new(),
        };
        let mut slots: [Slot; 4] = Default::default();
        for op in line.split(" ; ") {
            let p: Vec<&str> = op.split(' ').collect();
            let slot_index = |i: usize| p[i].parse::<usize>().unwrap_or(0);
            match p[0] {
                "ale" => {
                    let ale = load(p[1], p[2]);
                    fx.load(&ale);
                    out.push(format!(
                        "{head}\"op\":\"ale\",\"nodes\":\"{}\",\"effects\":\"{}\"}}",
                        code(&ale.nodes_result),
                        code(&ale.effects_result)
                    ));
                }
                "pool" => {
                    let kind = POOLS.iter().find(|(n, _)| *n == p[1]).map(|e| e.1);
                    if let Some(kind) = kind {
                        fx.pools.reallocate(kind, p[2].parse().unwrap_or(0));
                    }
                    out.push(format!("{head}\"op\":\"pool\"}}"));
                }
                "seed" => {
                    fx.rand.state = p[1].parse().unwrap_or(0);
                    fx.rand.counter = p[2].parse().unwrap_or(0);
                    out.push(format!("{head}\"op\":\"seed\"}}"));
                }
                "tex" => {
                    let crc = u32::from_str_radix(p[1], 16).unwrap_or(0);
                    tex.entries.push((crc, p[2].parse().unwrap_or(0)));
                    out.push(format!("{head}\"op\":\"tex\"}}"));
                }
                "effect" => {
                    let s = slot_index(1);
                    let parent = fx.create_node(NodeClass::Node);
                    slots[s].parent = Some(parent);
                    let hash = name_hash(&unhex(p[2]));
                    let result = fx.create_effect(parent, hash, &mut tex);
                    out.append(&mut tex.lines);
                    match result {
                        Ok(effect) => {
                            out.push(format!(
                                "{head}\"op\":\"effect\",\"r\":\"0\",\"n\":\"{}\",\"roots\":\"{}\"}}",
                                effect.records().len(),
                                effect.root_count()
                            ));
                            slots[s].effect = Some(effect);
                        }
                        Err(e) => {
                            let r = match e {
                                EffectError::UnknownEffect => -8,
                                EffectError::UnknownNode(_) => -1,
                            };
                            out.push(format!(
                                "{head}\"op\":\"effect\",\"r\":\"{r}\",\"n\":\"-\",\"roots\":\"-\"}}"
                            ));
                        }
                    }
                }
                "xform" => {
                    let effect = slots[slot_index(1)].effect.as_ref().expect("an effect");
                    let ok = effect.set_transform(&mut fx, p[2].parse().unwrap_or(0), &xform(p[3]));
                    let r = if ok { 0 } else { -2 };
                    out.push(format!("{head}\"op\":\"xform\",\"r\":\"{r}\"}}"));
                }
                "outer" | "age" | "lod" => {
                    let effect = slots[slot_index(1)].effect.as_ref().expect("an effect");
                    let value = hex_float(p[2]);
                    match p[0] {
                        "outer" => effect.set_outer_time(&mut fx, value),
                        "age" => effect.set_age(&mut fx, value),
                        _ => effect.set_lod_input(&mut fx, value),
                    }
                    out.push(format!("{head}\"op\":\"{}\"}}", p[0]));
                }
                "step" => {
                    let effect = slots[slot_index(1)].effect.as_ref().expect("an effect");
                    effect.compose(&mut fx);
                    effect.update(&mut fx, hex_float(p[2]), &mut tex);
                    out.append(&mut tex.lines);
                    out.push(format!("{head}\"op\":\"step\"}}"));
                }
                "stop" => {
                    let effect = slots[slot_index(1)].effect.as_mut().expect("an effect");
                    effect.dispatch(&mut fx, p[2].parse().unwrap_or(0), &mut tex);
                    out.append(&mut tex.lines);
                    out.push(format!("{head}\"op\":\"stop\"}}"));
                }
                "done" => {
                    let effect = slots[slot_index(1)].effect.as_ref().expect("an effect");
                    let r = u8::from(effect.finished(&fx, p[2].parse().unwrap_or(0)));
                    out.push(format!("{head}\"op\":\"done\",\"r\":\"{r}\"}}"));
                }
                "release" => {
                    let s = slot_index(1);
                    if let Some(effect) = slots[s].effect.take() {
                        effect.release(&mut fx, &mut tex);
                    }
                    if let Some(parent) = slots[s].parent.take() {
                        fx.release(parent, &mut tex);
                    }
                    out.append(&mut tex.lines);
                    out.push(format!("{head}\"op\":\"release\"}}"));
                }
                "rand" => out.push(format!(
                    "{head}\"op\":\"rand\",\"s\":\"{:08x}\",\"c\":\"{:08x}\"}}",
                    fx.rand.state, fx.rand.counter
                )),
                "pools" => dump_pools(&fx, &head, &mut out),
                "dump" => {
                    let s = &slots[slot_index(1)];
                    let effect = s.effect.as_ref().expect("an effect");
                    dump(
                        &fx,
                        effect,
                        s.parent,
                        &format!("{head}\"e\":\"{}\",", p[1]),
                        &mut out,
                    );
                }
                other => panic!("particle_sim: unknown operation {other}"),
            }
        }
    }
    out
}
