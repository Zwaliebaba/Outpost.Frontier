//! The run-time suites of `CppSource/Oracle/oracle_scene_script_runtime.cpp`:
//! param curves, spline paths and scene playback, in the lines the harness
//! writes. Playback drives the scene with recording engine objects whose
//! getters answer what the harness's do and whose setters store, so the
//! call logs of both sides can be compared line by line.

use std::cell::RefCell;
use std::rc::Rc;

use neuron_client::scene_script::curve::Curve;
use neuron_client::scene_script::entity::{AudioProps, LightProps};
use neuron_client::scene_script::event::EventDesc;
use neuron_client::scene_script::playback::{EntityControls, EventRef, Player};
use neuron_client::scene_script::spline::SplinePath;
use neuron_client::scene_script::{BuildState, Vm, build, load};

use super::{hex, input_lines, scene_script_build, unhex};

fn w(x: f32) -> String {
    format!("{:08x}", x.to_bits())
}

fn wi(x: i32) -> String {
    format!("{:08x}", x.cast_unsigned())
}

fn ws(xs: &[f32]) -> String {
    xs.iter().map(|x| w(*x)).collect::<Vec<_>>().join(" ")
}

fn wis(xs: &[i32]) -> String {
    xs.iter().map(|x| wi(*x)).collect::<Vec<_>>().join(" ")
}

fn hexf(token: &str) -> f32 {
    f32::from_bits(u32::from_str_radix(token, 16).unwrap_or_else(|e| panic!("{token}: {e}")))
}

fn text(s: Option<&[u8]>) -> String {
    s.map_or_else(|| "null".to_owned(), |s| format!("s{}", hex(s)))
}

/// An `f64` as the 10 bytes of the x87 extended value it is exactly, most
/// significant first, or `nan`.
fn x87(v: f64) -> String {
    if v.is_nan() {
        return "nan".to_owned();
    }
    let bits = v.to_bits();
    let sign = (bits >> 63) as u16;
    let exponent = ((bits >> 52) & 0x7ff) as i32;
    let fraction = bits & ((1 << 52) - 1);
    let (e80, mantissa) = if exponent == 0x7ff {
        (0x7fff, 1u64 << 63)
    } else if exponent == 0 && fraction == 0 {
        (0, 0)
    } else if exponent == 0 {
        let shift = fraction.leading_zeros() - 11;
        (
            (1 - 1023 + 16383 - shift as i32) as u16,
            (fraction << (11 + shift)) | (1 << 63),
        )
    } else {
        (
            (exponent - 1023 + 16383) as u16,
            (1 << 63) | (fraction << 11),
        )
    };
    format!("{:04x}{mantissa:016x}", (sign << 15) | e80)
}

// ------------------------------------------------------------------ curves

/// The lines of `scene_script_curve.list`.
pub fn curve_lines() -> Vec<String> {
    let mut lines = Vec::new();
    for (case, line) in input_lines("scene_script_curve.list").iter().enumerate() {
        let tokens: Vec<&str> = line.split(' ').collect();
        let curve = Curve::new(tokens[0]);
        lines.push(format!(
            "{{\"case\":\"{case}\",\"curve\":\"{}\"}}",
            if curve.is_some() { tokens[0] } else { "null" }
        ));
        let Some(mut curve) = curve else {
            continue;
        };
        for (op, token) in tokens.iter().enumerate().skip(1) {
            let args: Vec<&str> = token[1..].split(',').collect();
            let head = format!("{{\"case\":\"{case}\",\"op\":\"{op}\",");
            let f = |i: usize| hexf(args[i]);
            let int = |i: usize| args[i].parse::<i32>().unwrap();
            lines.push(match token.as_bytes()[0] {
                b'a' => format!("{head}\"add\":\"{}\"}}", curve.add_key(f(0), f(1), f(2), f(3))),
                b'r' => format!(
                    "{head}\"replace\":\"{}\"}}",
                    u8::from(curve.replace(int(0), [f(1), f(2), f(3), f(4)]))
                ),
                b'd' => format!("{head}\"delete\":\"{}\"}}", u8::from(curve.delete_key(int(0)))),
                b'e' => format!("{head}\"eval\":\"{}\"}}", x87(curve.evaluate(f(0)))),
                b'w' => format!(
                    "{head}\"wrap\":\"{}\"}}",
                    x87(curve.wrap_evaluate(int(0), int(1)).unwrap())
                ),
                b's' => {
                    let mut keys = Vec::new();
                    for index in 0.. {
                        let id = curve.id_by_index(index);
                        if id == -1 {
                            break;
                        }
                        let k = curve.read_key(id).unwrap_or_default();
                        keys.push(format!("{id}:{}", ws(&k)));
                    }
                    format!(
                        "{head}\"class\":\"{}\",\"display\":\"{}\",\"count\":\"{}\",\"keys\":\"{}\"}}",
                        text(Some(curve.class_name().as_bytes())),
                        text(Some(curve.display_name().as_bytes())),
                        curve.key_count(),
                        keys.join(",")
                    )
                }
                _ => panic!("bad op {token}"),
            });
        }
    }
    lines
}

// ----------------------------------------------------------------- splines

const SENTINEL: u32 = 0xdead_beef;

fn sentinel<const N: usize>() -> [f32; N] {
    [f32::from_bits(SENTINEL); N]
}

/// The lines of `scene_script_spline.list`.
pub fn spline_lines() -> Vec<String> {
    let mut lines = Vec::new();
    for (case, line) in input_lines("scene_script_spline.list").iter().enumerate() {
        let tokens: Vec<&str> = line.split(' ').collect();
        let mut path = SplinePath::new(tokens[0] == "o");
        for (op, token) in tokens.iter().enumerate().skip(1) {
            let letter = token.as_bytes()[0];
            let rest = &token[1..];
            let args: Vec<&str> = rest.split(',').collect();
            let f: Vec<f32> = if matches!(letter, b'L' | b'Z' | b'G' | b'C' | b'N' | b'I') {
                Vec::new()
            } else {
                args.iter()
                    .map(|a| if a.is_empty() { 0.0 } else { hexf(a) })
                    .collect()
            };
            let index = || args[0].parse::<i32>().unwrap() as u32;
            let mut v = String::new();
            let r = match letter {
                b'P' => {
                    let points: Vec<[f32; 3]> = if rest.is_empty() {
                        Vec::new()
                    } else {
                        f.chunks(3).map(|c| [c[0], c[1], c[2]]).collect()
                    };
                    path.load_points(&points)
                }
                b'L' => {
                    if *token == "L!" {
                        path.load_persist(None)
                    } else {
                        path.load_persist(Some(&unhex(rest)))
                    }
                }
                b'C' => path.set_closed(args[0] != "0"),
                b'S' => path.set_point(index(), [f[1], f[2], f[3]]),
                b'Q' => path.set_orientation(index(), [f[1], f[2], f[3], f[4]]),
                b'E' => {
                    let mut out = sentinel::<3>();
                    let r = path.evaluate(f[0], &mut out);
                    v = ws(&out);
                    r
                }
                b'K' => {
                    let mut out = sentinel::<3>();
                    let r = path.look_at(f[0], &mut out);
                    v = ws(&out);
                    r
                }
                b'O' => {
                    let mut out = sentinel::<4>();
                    let r = path.orientation_at(f[0], &mut out);
                    v = ws(&out);
                    r
                }
                b'T' => {
                    let mut out: [f32; 12] = f[1..13].try_into().unwrap();
                    let r = path.transform(f[0], &mut out);
                    v = ws(&out);
                    r
                }
                b'N' => {
                    let mut out = sentinel::<1>()[0];
                    let r = path.length(&mut out);
                    v = w(out);
                    r
                }
                b'G' => {
                    let i = index();
                    let (mut p, mut t, mut q, mut x) = (
                        sentinel::<3>(),
                        sentinel::<1>()[0],
                        sentinel::<4>(),
                        sentinel::<12>(),
                    );
                    let r6 = path.point(i, &mut p);
                    let r7 = path.time(i, &mut t);
                    let r12 = path.orientation(i, &mut q);
                    let r15 = path.transform_at(i, &mut x);
                    v = format!(
                        "{} {} {} {} {} {} {} {}",
                        wi(r6),
                        ws(&p),
                        wi(r7),
                        w(t),
                        wi(r12),
                        ws(&q),
                        wi(r15),
                        ws(&x)
                    );
                    0
                }
                b'Z' => match path.save(args[0].parse().unwrap()) {
                    Ok(t) => {
                        v = format!("s{}", hex(&t));
                        0
                    }
                    Err(r) => {
                        v = "-".to_owned();
                        r
                    }
                },
                b'I' => {
                    v = format!(
                        "{:08x} {:08x} {:08x}",
                        path.live_points(),
                        u32::from(path.closed()),
                        path.estimate_size()
                    );
                    0
                }
                _ => panic!("bad op {token}"),
            };
            lines.push(format!(
                "{{\"case\":\"{case}\",\"op\":\"{op}\",\"f\":\"{}\",\"r\":\"{r}\",\"v\":\"{v}\"}}",
                char::from(letter)
            ));
        }
    }
    lines
}

// ---------------------------------------------------------------- playback

type Log = Rc<RefCell<Vec<String>>>;

/// The harness's recording engine object (`FStubObject`).
struct Stub {
    handle: i32,
    mask: u32,
    log: Log,
    pos: [f32; 3],
    orient: [f32; 9],
    /// Fov, near, far, aspect.
    camera: [f32; 4],
    floor: f32,
    sparam: f32,
    reverb: [u32; 4],
    fog: [u32; 8],
    audio: AudioProps,
    light_color: [i32; 3],
    light_direction: [f32; 3],
    light_range: f32,
    light_cutoff: f32,
    light_slot13: i32,
    light_slot14: i32,
    d3d_on: i32,
    d3d_light: LightProps,
    d3d_direction: [f32; 3],
}

impl Stub {
    fn new(handle: i32, mask: u32, log: Log) -> Stub {
        let mut d3d_light = LightProps {
            light_type: 3,
            diffuse: [0.5, 0.5, 0.5, 1.0],
            range: 50.0,
            unused_1e: 1.0_f32.to_bits(),
            atten: [1.0, 0.0, 0.0],
            theta: 0.25,
            cutoff: 0.5,
            ..LightProps::default()
        };
        d3d_light.unused_17[4] = (-1.0_f32).to_bits() as i32;
        Stub {
            handle,
            mask,
            log,
            pos: [0.0; 3],
            orient: [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
            camera: [1.0, 0.5, 2000.0, 1.25],
            floor: 0.25,
            sparam: 0.5,
            reverb: [3, 0.5_f32.to_bits(), 1.5_f32.to_bits(), 0.75_f32.to_bits()],
            fog: [
                0x0101,
                10,
                20,
                30,
                1,
                5.0_f32.to_bits(),
                50.0_f32.to_bits(),
                0.01_f32.to_bits(),
            ],
            audio: AudioProps {
                attenuation: -3.0,
                pan: 0.25,
                dmin: 2.0,
                dmax: 500.0,
                ain: 180,
                aout: 270,
                atout: -1.0,
                rmix: 0.5,
            },
            light_color: [200, 100, 50],
            light_direction: [0.0, 0.0, -1.0],
            light_range: 100.0,
            light_cutoff: 0.5,
            light_slot13: 1,
            light_slot14: 0,
            d3d_on: 1,
            d3d_light,
            d3d_direction: [0.0, -1.0, 0.0],
        }
    }

    fn call(&self, name: &str, args: &str) {
        self.log.borrow_mut().push(format!(
            "{{\"c\":\"{}\",\"f\":\"{name}\",\"a\":\"{args}\"}}",
            self.handle
        ));
    }
}

fn common_words(e: &EventRef<'_>) -> String {
    let c = &e.desc.common;
    wis(&[
        c.kind,
        c.start_ms,
        c.flags,
        c.event_flags,
        0,
        c.duration_ms,
        c.primary,
        c.secondary,
        c.pcurve_period,
        i32::from(c.param_curve.is_some()),
    ])
}

fn desc_text(e: &EventRef<'_>) -> String {
    format!(
        "{} | {}",
        common_words(e),
        scene_script_build::payload(&e.desc.payload).join(" ")
    )
}

fn d3d_words(l: &LightProps) -> Vec<u32> {
    let mut v = vec![l.light_type as u32];
    v.extend(l.diffuse.iter().map(|x| x.to_bits()));
    v.extend(l.specular.iter().map(|x| x.to_bits()));
    v.extend(l.ambient.iter().map(|x| x.to_bits()));
    v.extend(l.unused_17.iter().map(|x| *x as u32));
    v.push(l.range.to_bits());
    v.push(l.unused_1e);
    v.extend(l.atten.iter().map(|x| x.to_bits()));
    v.push(l.theta.to_bits());
    v.push(l.cutoff.to_bits());
    v
}

fn copy_d3d(from: &LightProps, to: &mut LightProps) {
    to.light_type = from.light_type;
    to.diffuse = from.diffuse;
    to.specular = from.specular;
    to.ambient = from.ambient;
    to.unused_17 = from.unused_17;
    to.range = from.range;
    to.unused_1e = from.unused_1e;
    to.atten = from.atten;
    to.theta = from.theta;
    to.cutoff = from.cutoff;
}

fn audio_words(a: &AudioProps) -> String {
    format!(
        "{} {} {}",
        ws(&[a.attenuation, a.pan, a.dmin, a.dmax]),
        wis(&[a.ain, a.aout]),
        ws(&[a.atout, a.rmix])
    )
}

impl EntityControls for Stub {
    fn interfaces(&self) -> u32 {
        self.mask
    }

    fn set_position(&mut self, p: [f32; 3]) {
        self.call("SetPosition", &ws(&p));
        self.pos = p;
    }
    fn set_orientation(&mut self, m: [f32; 9]) {
        self.call("SetOrientation", &ws(&m));
        self.orient = m;
    }
    fn get_position(&mut self, out: &mut [f32; 3]) {
        self.call("GetPosition", "");
        *out = self.pos;
    }
    fn get_orientation(&mut self, out: &mut [f32; 9]) {
        self.call("GetOrientation", "");
        *out = self.orient;
    }

    fn set_fov(&mut self, v: f32) {
        self.call("SetFov", &w(v));
        self.camera[0] = v;
    }
    fn set_aspect(&mut self, v: f32) {
        self.call("SetAspect", &w(v));
        self.camera[3] = v;
    }
    fn set_near(&mut self, v: f32) {
        self.call("SetNear", &w(v));
        self.camera[1] = v;
    }
    fn set_far(&mut self, v: f32) {
        self.call("SetFar", &w(v));
        self.camera[2] = v;
    }
    fn get_fov(&mut self, out: &mut f32) {
        self.call("GetFov", "");
        *out = self.camera[0];
    }
    fn get_aspect(&mut self, out: &mut f32) {
        self.call("GetAspect", "");
        *out = self.camera[3];
    }
    fn get_near(&mut self, out: &mut f32) {
        self.call("GetNear", "");
        *out = self.camera[1];
    }
    fn get_far(&mut self, out: &mut f32) {
        self.call("GetFar", "");
        *out = self.camera[2];
    }

    fn start_motion(&mut self, e: EventRef<'_>) -> i32 {
        self.call("StartMotion", &desc_text(&e));
        0
    }
    fn stop_motion(&mut self, e: EventRef<'_>) {
        self.call("StopMotion", &desc_text(&e));
    }
    fn set_floor_height(&mut self, h: f32) {
        self.call("SetFloorHeight", &w(h));
        self.floor = h;
    }
    fn advance_time(&mut self, d: i32) {
        self.call("AdvanceTime", &wi(d));
    }
    fn connect_hardpoints(&mut self, e: EventRef<'_>) -> i32 {
        self.call("ConnectHardpoints", &desc_text(&e));
        0
    }
    fn disconnect_hardpoints(&mut self, e: EventRef<'_>) {
        self.call("DisconnectHardpoints", &desc_text(&e));
    }
    fn get_hardpoint_xform(&mut self, t: i32, name: Option<&[u8]>, x: &mut [f32; 12]) {
        self.call("GetHardpointXform", &format!("{} {}", wi(t), text(name)));
        *x = [
            1.0,
            0.0,
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
            0.0,
            1.0,
            name.map_or(0, <[u8]>::len) as f32,
            t as f32,
            1.5,
        ];
    }
    fn get_floor_height(&mut self, out: &mut f32) {
        self.call("GetFloorHeight", "");
        *out = self.floor;
    }

    fn start_ik(&mut self, e: EventRef<'_>, p: [f32; 3], m: [f32; 9]) -> i32 {
        self.call(
            "StartIk",
            &format!("{} | {} {}", desc_text(&e), ws(&p), ws(&m)),
        );
        0
    }
    fn stop_ik(&mut self, e: EventRef<'_>) {
        self.call("StopIk", &desc_text(&e));
    }
    fn update_ik(&mut self, e: EventRef<'_>, p: [f32; 3], m: [f32; 9]) {
        self.call(
            "UpdateIk",
            &format!("{} | {} {}", desc_text(&e), ws(&p), ws(&m)),
        );
    }

    fn light_set_color(&mut self, rgb: [i32; 3]) {
        self.call("LSetColor", &wis(&rgb));
        self.light_color = rgb;
    }
    fn light_set_direction(&mut self, d: [f32; 3]) {
        self.call("LSetDirection", &ws(&d));
        self.light_direction = d;
    }
    fn light_set_range(&mut self, v: f32) {
        self.call("LSetRange", &w(v));
        self.light_range = v;
    }
    fn light_set_cutoff(&mut self, v: f32) {
        self.call("LSetCutoff", &w(v));
        self.light_cutoff = v;
    }
    fn light_set_on(&mut self, v: i32) {
        self.call("LSetOn", &wi(v));
        self.light_slot13 = v;
    }
    fn light_set_is_spot(&mut self, v: i32) {
        self.call("LSetIsSpot", &wi(v));
    }
    fn light_get_color(&mut self, out: &mut [i32; 3]) {
        self.call("LGetColor", "");
        *out = self.light_color;
    }
    fn light_get_direction(&mut self, out: &mut [f32; 3]) {
        self.call("LGetDirection", "");
        *out = self.light_direction;
    }
    fn light_get_range(&mut self, out: &mut f32) {
        self.call("LGetRange", "");
        *out = self.light_range;
    }
    fn light_get_cutoff(&mut self, out: &mut f32) {
        self.call("LGetCutoff", "");
        *out = self.light_cutoff;
    }
    fn light_get_slot13(&mut self, out: &mut i32) {
        self.call("LGetSlot13", "");
        *out = self.light_slot13;
    }
    fn light_get_slot14(&mut self, out: &mut i32) {
        self.call("LGetSlot14", "");
        *out = self.light_slot14;
    }

    fn d3d_set_on(&mut self, v: i32) {
        self.call("DSetOn", &wi(v));
        self.d3d_on = v;
    }
    fn d3d_set_light(&mut self, l: &LightProps) {
        let words: Vec<String> = d3d_words(l).iter().map(|x| format!("{x:08x}")).collect();
        self.call("DSetLight", &words.join(" "));
        copy_d3d(l, &mut self.d3d_light);
    }
    fn d3d_set_direction(&mut self, d: [f32; 3]) {
        self.call("DSetDirection", &ws(&d));
        self.d3d_direction = d;
    }
    fn d3d_get_on(&mut self, out: &mut i32) {
        self.call("DGetOn", "");
        *out = self.d3d_on;
    }
    fn d3d_get_light(&mut self, out: &mut LightProps) {
        self.call("DGetLight", "");
        copy_d3d(&self.d3d_light, out);
    }
    fn d3d_get_direction(&mut self, out: &mut [f32; 3]) {
        self.call("DGetDirection", "");
        *out = self.d3d_direction;
    }

    fn fog_set_on(&mut self, on: u8) {
        self.call("FSetOn", &wi(i32::from(on)));
        self.fog[0] = (self.fog[0] & !0xff) | u32::from(on);
    }
    fn fog_set_table_mode(&mut self, table: u8, mode: u32) {
        self.call(
            "FSetTableMode",
            &format!("{} {}", wi(i32::from(table)), wi(mode as i32)),
        );
        self.fog[0] = (self.fog[0] & !0xff00) | (u32::from(table) << 8);
        self.fog[4] = mode;
    }
    fn fog_set_range(&mut self, start: f32, end: f32, density: f32) {
        self.call("FSetRangeValue", &ws(&[start, end, density]));
        self.fog[5] = start.to_bits();
        self.fog[6] = end.to_bits();
        self.fog[7] = density.to_bits();
    }
    fn fog_set_color(&mut self, rgb: [i32; 3]) {
        self.call("FSetColor", &wis(&rgb));
        for (word, c) in self.fog[1..4].iter_mut().zip(rgb) {
            *word = c as u32;
        }
    }
    fn fog_get_packed(&mut self, out: &mut [u32; 8]) {
        self.call("FGetPacked", "");
        *out = self.fog;
    }

    fn set_reverb(&mut self, env: u32, vol: f32, decay: f32, damping: f32) {
        self.call(
            "SetReverb",
            &format!("{} {}", wi(env as i32), ws(&[vol, decay, damping])),
        );
        self.reverb = [env, vol.to_bits(), decay.to_bits(), damping.to_bits()];
    }
    fn get_reverb(&mut self, out: &mut [u32; 4]) {
        self.call("GetReverb", "");
        *out = self.reverb;
    }

    fn psys_start(&mut self) -> i32 {
        self.call("PStart", "");
        0
    }
    fn psys_stop(&mut self) {
        self.call("PStop", "");
    }
    fn psys_tick(&mut self, d: i32) {
        self.call("PTick", &wi(d));
    }
    fn psys_set_sparam(&mut self, v: f32) {
        self.call("PSetSparam", &w(v));
        self.sparam = v;
    }
    fn psys_get_sparam(&mut self, out: &mut f32) {
        self.call("PGetSparam", "");
        *out = self.sparam;
    }

    fn start_sound(&mut self, e: EventRef<'_>, time: i32) -> i32 {
        self.call("StartSound", &format!("{} | {}", desc_text(&e), wi(time)));
        0
    }
    fn stop_sound(&mut self, e: EventRef<'_>) {
        self.call("StopSound", &common_words(&e));
    }
    fn set_audio_props(&mut self, p: &AudioProps, mask: i32) {
        self.call("SetAudioProps", &format!("{} {}", audio_words(p), wi(mask)));
        self.audio = p.clone();
    }
    fn get_audio_props(&mut self, p: &mut AudioProps, mask: i32) {
        self.call("GetAudioProps", &wi(mask));
        *p = self.audio.clone();
    }

    fn show_subtitle(&mut self, id: i32) {
        self.call("ShowSubtitle", &wi(id));
    }
    fn hide_subtitle(&mut self, id: i32) {
        self.call("HideSubtitle", &wi(id));
    }

    fn user_event_start(&mut self, e: EventRef<'_>) -> i32 {
        self.call("UStart", &desc_text(&e));
        0
    }
    fn user_event_stop(&mut self, e: EventRef<'_>) {
        self.call("UStop", &desc_text(&e));
    }

    fn script_start(&mut self, start: i32) -> i32 {
        self.call("SStart", &wi(start));
        0
    }
    fn script_tick(&mut self, d: i32) {
        self.call("STick", &wi(d));
    }
    fn script_stop(&mut self) {
        self.call("SStop", "");
    }

    fn path_evaluate(&mut self, t: f32, out: &mut [f32; 3]) -> i32 {
        self.call("MEvaluate", &w(t));
        *out = [t, 2.0 * t, 3.0 * t];
        0
    }
    fn path_orientation(&mut self, t: f32, out: &mut [f32; 4]) -> i32 {
        self.call("MOrient", &w(t));
        *out = [1.0, 0.0, 0.0, 0.0];
        0
    }
    fn path_look_at(&mut self, t: f32, out: &mut [f32; 3]) -> i32 {
        self.call("MLookAt", &w(t));
        *out = [0.0, 0.0, 1.0];
        0
    }
}

fn flush(lines: &mut Vec<String>, log: &Log) {
    lines.append(&mut log.borrow_mut());
}

/// The lines of `scene_script_playback.list`.
pub fn playback_lines() -> Vec<String> {
    let mut lines = Vec::new();
    for (case, line) in input_lines("scene_script_playback.list").iter().enumerate() {
        let fields: Vec<&str> = line.split(' ').collect();
        let chunk = unhex(fields[2]);
        let mut vm = Vm::new();
        let mut state = BuildState::new();
        let Ok(summary) = load(&mut vm, &chunk) else {
            lines.push(format!("{{\"case\":\"{case}\",\"load\":\"-1\"}}"));
            continue;
        };
        let scene = build(&mut vm, &mut state, &chunk, summary, true).unwrap();
        let mut player = Player::new();
        player.load(&scene, summary.duration_ms);
        lines.push(format!(
            "{{\"case\":\"{case}\",\"load\":\"0\",\"duration\":\"{}\",\"entities\":\"{}\",\"events\":\"{}\"}}",
            player.duration(),
            player.live_entity_count(),
            player.event_count()
        ));
        let events: Vec<EventDesc> = player.events().map(|(_, d)| d.clone()).collect();
        for (index, d) in events.iter().enumerate() {
            let c = &d.common;
            lines.push(format!(
                "{{\"case\":\"{case}\",\"event\":\"{index}\",\"type\":\"{}\",\"start\":\"{}\",\"primary\":\"{}\",\
                 \"secondary\":\"{}\"}}",
                c.kind, c.start_ms, c.primary, c.secondary
            ));
        }
        let log: Log = Rc::new(RefCell::new(Vec::new()));
        let attach = |player: &mut Player, spec: &str| {
            let (handle, mask) = spec.split_once(':').unwrap();
            let handle: i32 = handle.parse().unwrap();
            let mask = u32::from_str_radix(mask, 16).unwrap();
            (
                handle,
                player.attach(handle, Box::new(Stub::new(handle, mask, log.clone()))),
            )
        };
        if fields[0] != "-" {
            for spec in fields[0].split(',') {
                let (handle, r) = attach(&mut player, spec);
                lines.push(format!(
                    "{{\"case\":\"{case}\",\"attach\":\"{handle}\",\"r\":\"{r}\"}}"
                ));
            }
        }
        for op in fields[1].split(',') {
            let arg = &op[1..];
            let num = || arg.parse::<i32>().unwrap();
            let r = match op.as_bytes()[0] {
                b's' => player.start(num()),
                b't' => player.tick(num()),
                b'x' => player.stop(),
                b'p' => player.pause(),
                b'l' => {
                    player.set_loop(num() != 0);
                    0
                }
                b'd' => player.detach(num()),
                b'a' => attach(&mut player, arg).1,
                b'r' => player.remove_entity(num()),
                b'P' => {
                    let v: Vec<f32> = arg.split('/').map(hexf).collect();
                    player.set_position([v[0], v[1], v[2]]);
                    0
                }
                b'R' => {
                    let v: Vec<f32> = arg.split('/').map(hexf).collect();
                    player.set_orientation(v.try_into().unwrap());
                    0
                }
                _ => panic!("bad op {op}"),
            };
            flush(&mut lines, &log);
            lines.push(format!(
                "{{\"case\":\"{case}\",\"op\":\"{op}\",\"r\":\"{r}\",\"clock\":\"{}\",\"state\":\"{}\"}}",
                player.clock(),
                player.state()
            ));
        }
        let handles: Vec<i32> = player.entity_handles().collect();
        for handle in handles {
            let (pos, orient) = player.entity_local_pose(handle).unwrap();
            let (wpos, worient) = player.entity_pose(handle).unwrap();
            lines.push(format!(
                "{{\"case\":\"{case}\",\"entity\":\"{handle}\",\"pos\":\"{}\",\"orient\":\"{}\",\"world\":\"{} {}\"}}",
                ws(&pos),
                ws(&orient),
                ws(&wpos),
                ws(&worient)
            ));
        }
        flush(&mut lines, &log);
    }
    lines
}

/// Tier B comparison for the suites with trigonometry: lines and tokens
/// must match, a float word may differ from the committed one by at most
/// `ulps` units in the last place when both are finite normal floats.
pub fn check_within(name: &str, lines: &[String], ulps: u32) {
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        super::check_suite(name, lines);
        return;
    }
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let committed =
        std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
    let words = |s: &str| -> Vec<String> {
        s.split(|c: char| !c.is_ascii_alphanumeric())
            .filter(|t| !t.is_empty())
            .map(str::to_owned)
            .collect()
    };
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        if *want == got {
            continue;
        }
        let (a, b) = (words(want), words(got));
        let close = a.len() == b.len()
            && a.iter().zip(&b).all(|(x, y)| {
                if x == y {
                    return true;
                }
                let (Ok(x), Ok(y)) = (u32::from_str_radix(x, 16), u32::from_str_radix(y, 16))
                else {
                    return false;
                };
                let (fx, fy) = (f32::from_bits(x), f32::from_bits(y));
                fx.is_normal() && fy.is_normal() && (x >> 31) == (y >> 31) && x.abs_diff(y) <= ulps
            });
        assert!(
            close,
            "{name} line {}:\n  committed {want}\n  port      {got}",
            index + 1
        );
    }
}
