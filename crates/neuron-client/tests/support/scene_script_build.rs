//! The engine calls of the scene-script parser's second pass, in the
//! lines `CppSource/Oracle/oracle_scene_script_build.cpp` writes: a status
//! line for the first pass, then one line per call. Entity tails and event
//! payloads are the descriptor words after the common part, in layout
//! order, with string pointers spelled out; the user properties of an
//! entity are sorted (divergence D200).

use neuron_client::scene_script::entity::{
    AudioProps, EntityDesc, EntityTail, Fog, LightProps, PathInit,
};
use neuron_client::scene_script::event::{EventDesc, EventPayload, IkKey};
use neuron_client::scene_script::{BuildState, Scene, Step, Vm, build, load};

use super::hex;

fn word_f(x: f32) -> String {
    format!("{:08x}", x.to_bits())
}

fn word_i(x: i32) -> String {
    format!("{:08x}", x.cast_unsigned())
}

fn word_u(x: u32) -> String {
    format!("{x:08x}")
}

fn floats(xs: &[f32]) -> Vec<String> {
    xs.iter().copied().map(word_f).collect()
}

fn ints(xs: &[i32]) -> Vec<String> {
    xs.iter().copied().map(word_i).collect()
}

fn text(s: Option<&[u8]>) -> String {
    s.map_or_else(|| "null".to_owned(), |s| format!("s{}", hex(s)))
}

fn light(p: &LightProps) -> Vec<String> {
    let mut w = ints(&p.color);
    w.extend(floats(&p.direction));
    w.extend(floats(&[p.range_temp, p.cutoff_temp]));
    w.extend(ints(&[p.on, p.infinite, p.light_type]));
    w.extend(floats(&p.diffuse));
    w.extend(floats(&p.specular));
    w.extend(floats(&p.ambient));
    w.extend(ints(&p.unused_17));
    w.push(word_f(p.range));
    w.push(word_u(p.unused_1e));
    w.extend(floats(&p.atten));
    w.extend(floats(&[p.theta, p.cutoff]));
    w
}

fn audio(a: &AudioProps) -> Vec<String> {
    let mut w = floats(&[a.attenuation, a.pan, a.dmin, a.dmax]);
    w.extend(ints(&[a.ain, a.aout]));
    w.extend(floats(&[a.atout, a.rmix]));
    w
}

fn fog(f: &Fog) -> Vec<String> {
    let mut w = vec![word_u(u32::from(f.on) | (u32::from(f.table) << 8))];
    w.extend(ints(&f.color));
    w.push(word_i(f.mode));
    w.extend(floats(&[f.start, f.end, f.density]));
    w
}

fn tail(t: &EntityTail) -> String {
    let words = match t {
        EntityTail::FloorHeight(x) | EntityTail::Psys(x) => vec![word_f(*x)],
        EntityTail::Camera {
            fovh,
            near_plane,
            far_plane,
            aspect,
        } => floats(&[*fovh, *near_plane, *far_plane, *aspect]),
        EntityTail::Light(p) => light(p),
        EntityTail::Sound(a) => audio(a),
        EntityTail::None => Vec::new(),
        EntityTail::Scene(s) => {
            let mut w = ints(&s.ambient);
            w.push(word_u(s.duration_ms));
            w.extend(ints(&[s.start_ms, s.up, s.front]));
            w.extend(fog(&s.fog));
            w
        }
        EntityTail::MotionPath(m) => {
            let mut out = text(m.path_type.as_deref()) + " ";
            match &m.path {
                None => out += "null",
                Some(path) => {
                    out += &format!("o{}", hex(path.factory.name().as_bytes()));
                    match &path.init {
                        PathInit::Empty => {}
                        PathInit::Text(t) => out += &format!(" l{}", text(t.as_deref())),
                        PathInit::Points(points) => {
                            out += &format!(" p{}", points.len());
                            if !points.is_empty() {
                                let flat: Vec<f32> = points.iter().flatten().copied().collect();
                                out += " ";
                                out += &floats(&flat).join(" ");
                            }
                        }
                    }
                }
            }
            return out;
        }
    };
    words.join(" ")
}

fn entity_line(handle: usize, e: &EntityDesc) -> String {
    format!(
        "{{\"entity\":\"{handle}\",\"type\":\"{}\",\"flags\":\"{}\",\"pos\":\"{}\",\"orient\":\"{}\",\
         \"name\":\"{}\",\"template\":\"{}\",\"groups\":\"{} {} {}\",\"tail\":\"{}\"}}",
        e.kind,
        e.flags,
        floats(&e.pos).join(" "),
        floats(&e.orient).join(" "),
        text(e.name.as_deref()),
        text(e.template_name.as_deref()),
        e.lt_grp,
        e.srt_grp,
        e.usr_flg,
        tail(&e.tail)
    )
}

pub fn payload(p: &EventPayload) -> Vec<String> {
    match p {
        EventPayload::User { payload_word, .. } => vec![word_u(*payload_word)],
        EventPayload::SetCamera | EventPayload::StartPsys => Vec::new(),
        EventPayload::StartSound { start_time } => vec![word_i(*start_time)],
        EventPayload::Light { mask, target } => {
            let mut w = vec![word_i(*mask)];
            w.extend(light(target));
            w
        }
        EventPayload::Camera { mask, target } => {
            let mut w = vec![word_i(*mask)];
            w.extend(floats(target));
            w
        }
        EventPayload::Path {
            start_percent,
            stop_percent,
            percent_delta,
            offset,
            up,
            front,
        } => {
            let mut w = floats(&[*start_percent, *stop_percent, *percent_delta]);
            w.extend(floats(offset));
            w.extend(ints(&[*up, *front]));
            w
        }
        EventPayload::Spatial {
            mask,
            orient,
            position,
            quaternion,
            axis_rot_angle,
            axis_rot_axis,
            target_part,
            target_type,
        } => {
            let mut w = vec![word_i(*mask)];
            w.extend(floats(orient));
            w.extend(floats(position));
            w.extend(floats(quaternion));
            w.push(word_f(*axis_rot_angle));
            w.push(word_i(*axis_rot_axis));
            w.push(text(target_part.as_deref()));
            w.push(word_i(*target_type));
            w
        }
        EventPayload::Attach {
            offset,
            target_type,
            up,
            front,
            target_part,
        } => {
            let mut w = floats(offset);
            w.extend(ints(&[*target_type, *up, *front]));
            w.push(text(target_part.as_deref()));
            w
        }
        EventPayload::Connect {
            hardpoint,
            parent_hardpoint,
        } => vec![
            text(hardpoint.as_deref()),
            text(parent_hardpoint.as_deref()),
        ],
        EventPayload::Motion {
            start_time,
            trans_time,
            time_scale,
            weight,
            animation,
            field_40,
            heading,
            locked_bone,
            trans_scale,
            field_50,
        } => {
            let mut w = floats(&[*start_time, *trans_time, *time_scale, *weight]);
            w.push(text(animation.as_deref()));
            w.push(word_u(u32::from(*field_40)));
            w.push(word_f(*heading));
            w.push(text(locked_bone.as_deref()));
            w.push(word_f(*trans_scale));
            w.push(word_i(*field_50));
            w
        }
        EventPayload::Ik {
            key,
            count_to_root,
            up,
            front,
            damping,
            point_at,
            move_to,
            transition_duration,
            end_effector,
            target_part,
            target_type,
        } => {
            let mut w = vec![match key {
                IkKey::Event => "self".to_owned(),
                IkKey::Id(id) => word_u(*id),
            }];
            w.extend(ints(&[*count_to_root, *up, *front]));
            w.push(word_f(*damping));
            w.push(word_u(u32::from(*point_at) | (u32::from(*move_to) << 8)));
            w.push(word_f(*transition_duration));
            w.push(text(end_effector.as_deref()));
            w.push(text(target_part.as_deref()));
            w.push(word_i(*target_type));
            w
        }
        EventPayload::SubScene { start_time } => vec![word_f(*start_time)],
        EventPayload::PsysProp { mask, sparam } => vec![word_i(*mask), word_f(*sparam)],
        EventPayload::Audio { mask, target } => {
            let mut w = vec![word_i(*mask)];
            w.extend(audio(target));
            w
        }
        EventPayload::Fog { mask, target } => {
            let mut w = vec![word_i(*mask)];
            w.extend(fog(target));
            w
        }
        EventPayload::Reverb { mask, target } => vec![
            word_i(*mask),
            word_i(target.environment),
            word_f(target.volume),
            word_f(target.decay),
            word_f(target.damping),
        ],
        EventPayload::FloorHeight {
            floor_height,
            target_part,
            target_type,
        } => vec![
            word_f(*floor_height),
            text(target_part.as_deref()),
            word_i(*target_type),
        ],
        EventPayload::Subtitle { strid } => vec![word_u(*strid)],
    }
}

fn event_lines(e: &EventDesc) -> Vec<String> {
    let c = &e.common;
    let curve = c.param_curve.as_ref().map_or_else(
        || "null".to_owned(),
        |curve| {
            let mut out = format!("s{}", hex(curve.class.as_bytes()));
            for key in &curve.keys {
                out += " ";
                out += &floats(key).join(" ");
            }
            out
        },
    );
    let mut lines = vec![format!(
        "{{\"event\":\"{}\",\"start\":\"{}\",\"flags\":\"{}\",\"event_flags\":\"{}\",\"field10\":\"0\",\
         \"duration\":\"{}\",\"primary\":\"{}\",\"secondary\":\"{}\",\"curve\":\"{curve}\",\
         \"period\":\"{}\",\"payload\":\"{}\"}}",
        c.kind,
        c.start_ms,
        c.flags,
        c.event_flags,
        c.duration_ms,
        c.primary,
        c.secondary,
        c.pcurve_period,
        payload(&e.payload).join(" ")
    )];
    if let EventPayload::User { text: t, .. } = &e.payload {
        lines.push(format!("{{\"user_string\":\"{}\"}}", text(t.as_deref())));
    }
    lines
}

/// The lines of a build's calls.
pub fn render(scene: &Scene) -> Vec<String> {
    let mut lines = Vec::new();
    let mut props: Vec<String> = Vec::new();
    let mut handle = 0;
    let flush = |lines: &mut Vec<String>, props: &mut Vec<String>| {
        props.sort();
        lines.append(props);
    };
    for step in &scene.steps {
        if let Step::SetUserProp { entity, key, value } = step {
            props.push(format!(
                "{{\"userprop\":\"{entity}\",\"key\":\"{}\",\"value\":\"{}\"}}",
                text(key.as_deref()),
                text(Some(value))
            ));
            continue;
        }
        flush(&mut lines, &mut props);
        match step {
            Step::AddEntity(e) => {
                lines.push(entity_line(handle, e));
                handle += 1;
            }
            Step::UpdateEntity { entity, desc } => lines.push(format!(
                "{{\"update\":\"{entity}\",\"pos0\":\"{}\"}}",
                word_f(desc.pos[0])
            )),
            Step::AddEvent(e) => lines.extend(event_lines(e)),
            Step::SetUserProp { .. } => unreachable!("handled above"),
        }
    }
    flush(&mut lines, &mut props);
    lines
}

/// One load as `FScriptEngine::Slot4Load` runs it: the first pass, then
/// the second when the first succeeded. `head` starts the status line.
pub fn run(
    vm: &mut Vm,
    state: &mut BuildState,
    bytes: &[u8],
    com_context: bool,
    head: &str,
) -> Vec<String> {
    let summary = match load(vm, bytes) {
        Ok(summary) => summary,
        Err(_) => return vec![format!("{head}\"load\":\"-1\"}}")],
    };
    let mut lines = vec![format!(
        "{head}\"load\":\"0\",\"entities\":\"{}\",\"events\":\"{}\",\"duration\":\"{}\"}}",
        summary.entities, summary.events, summary.duration_ms
    )];
    let scene = build(vm, state, bytes, summary, com_context)
        .unwrap_or_else(|e| panic!("{head}: the build failed: {e}"));
    lines.extend(render(&scene));
    lines
}
