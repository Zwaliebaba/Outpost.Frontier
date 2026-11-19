//! Saving a scene: `FSceneScriptScene::SaveToFileSystem` of
//! `SceneScriptScene.cpp` with its writer buffer (`FSceneScriptSaveBuf`)
//! and the helpers it formats through.
//!
//! The writer prints the scene back as Lua source: `duration`, then
//! `entities` from each entity's descriptor and its world pose, then
//! `events` from each event's descriptor in master order. The text goes
//! through a 512-byte buffer that is flushed with one `Write` whenever the
//! next piece would not fit; each helper of the reference is one call
//! here, so the writes come in the same pieces. Numbers are printed as the
//! Universal C Runtime's `%f` prints them; durations and start times are
//! milliseconds scaled by 0.001 in `f64` and stored to a `float` first.
//!
//! The reference's text is reproduced as it is, including what does not
//! read back as written: `q_orient` holds four words of the matrix made
//! from the quaternion, a START_MOTION event without an animation leaves
//! its property table unopened, strings are not escaped (only template
//! names are, and only `\`, CR, LF and backspace), and a USER_EVENT's text
//! is written only when a COM context hands it out.

use super::super::entity::{AudioProps, Fog, LightProps, kind as entity_kind, light_bit};
use super::super::event::{EventDesc, EventPayload, Reverb, flag, kind, spatial_bit};
use super::super::script_engine::ComContext;
use super::Player;
use super::controls::EventRef;
use super::entity::Entity;
use super::math::quat_to_matrix;

/// `FSceneScriptSaveBuf`'s initial capacity.
const CAPACITY: usize = 512;
/// `kSceneScriptFlagEntityRelative`, `ParentChild`, `OrientRelative`.
const FLAG_ENTITY_RELATIVE: i32 = 0x20;
const FLAG_PARENT_CHILD: i32 = 0x40;
const FLAG_ORIENT_RELATIVE: i32 = 0x80;

const ENTITY_TYPE_NAMES: [&str; 14] = [
    "UNKNOWN_ENTITY",
    "COMPOUND",
    "DEFORMABLE",
    "CAMERA",
    "MONITOR",
    "LIGHT",
    "SOUND",
    "MARKER",
    "HARDPOINT",
    "SCENE",
    "SUB_SCENE",
    "MOTION_PATH",
    "DELETED",
    "PSYS",
];

/// The registration names in the reference's order, and the event type
/// each stands for.
const EVENT_TYPE_NAMES: [(i32, &str); 20] = [
    (0, "UNDEFINED_EVENT"),
    (10, "START_MOTION"),
    (3, "START_SOUND"),
    (11, "START_IK"),
    (6, "START_PATH_ANIMATION"),
    (8, "ATTACH_ENTITY"),
    (4, "START_LIGHT_PROP_ANIM"),
    (5, "START_CAMERA_PROP_ANIM"),
    (7, "START_SPATIAL_PROP_ANIM"),
    (2, "SET_CAMERA"),
    (9, "CONNECT_HARDPOINTS"),
    (12, "START_SUB_SCENE"),
    (1, "USER_EVENT"),
    (13, "START_PSYS"),
    (14, "START_PSYS_PROP_ANIM"),
    (15, "START_AUDIO_PROP_ANIM"),
    (16, "START_FOG_PROP_ANIM"),
    (17, "START_REVERB_PROP_ANIM"),
    (18, "START_FLR_HEIGHT_ANIM"),
    (19, "SUBTITLE"),
];

const AXIS_NAMES: [&str; 6] = [
    "X_AXIS",
    "Y_AXIS",
    "Z_AXIS",
    "NEG_X_AXIS",
    "NEG_Y_AXIS",
    "NEG_Z_AXIS",
];

/// `EntityTypeName`.
fn entity_type_name(kind: i32) -> &'static str {
    usize::try_from(kind)
        .ok()
        .and_then(|k| ENTITY_TYPE_NAMES.get(k))
        .copied()
        .unwrap_or(ENTITY_TYPE_NAMES[0])
}

/// `EventTypeName`: the registration name whose type is `kind`.
fn event_type_name(kind: i32) -> &'static str {
    EVENT_TYPE_NAMES
        .iter()
        .find(|(k, _)| *k == kind)
        .map_or(EVENT_TYPE_NAMES[0].1, |(_, name)| name)
}

/// `kAxisNames[axis]`. The reference indexes its six names with any value
/// and reads past them for one outside 0 to 5; such an axis is written as
/// the reference writes a null name, `NULL`, which reads back as nil
/// (divergence D541).
fn axis_name(axis: i32) -> Option<&'static [u8]> {
    usize::try_from(axis)
        .ok()
        .and_then(|a| AXIS_NAMES.get(a))
        .map(|n| n.as_bytes())
}

/// `printf("%.<precision>f")` of the Universal C Runtime: the value
/// correctly rounded; NaN as `nan`, or `-nan(ind)` for the negative
/// default NaN and `-nan` for other negative ones.
pub(super) fn printf_fixed(v: f64, precision: usize) -> String {
    if v.is_nan() {
        let bits = v.to_bits();
        return match (bits >> 63 != 0, bits & ((1 << 51) - 1) == 0) {
            (false, _) => "nan".to_owned(),
            (true, true) => "-nan(ind)".to_owned(),
            (true, false) => "-nan".to_owned(),
        };
    }
    format!("{v:.precision$}")
}

/// `%.6f` of a `float` promoted to `double`.
fn f6(v: f32) -> String {
    printf_fixed(f64::from(v), 6)
}

/// `%.3f` of a `float`.
fn f3(v: f32) -> String {
    printf_fixed(f64::from(v), 3)
}

/// `SaveBufAppendUnsignedMilliseconds`'s value: the milliseconds times
/// 0.001, stored to a `float`.
fn seconds_unsigned(ms: u32) -> f32 {
    (f64::from(ms) * 0.001) as f32
}

/// `SaveBufAppendSignedMilliseconds`'s value.
fn seconds_signed(ms: i32) -> f32 {
    (f64::from(ms) * 0.001) as f32
}

/// `ShouldSaveNonzeroFloat`: the value is not 0 (NaN is written).
fn nonzero(v: f32) -> bool {
    !(v == 0.0)
}

/// `SaveBufAppendVector`'s text for a format of three `%.6f`.
fn vector(prefix: &str, v: &[f32], suffix: &str) -> String {
    format!("{prefix}{}, {}, {}{suffix}", f6(v[0]), f6(v[1]), f6(v[2]))
}

/// `SaveMatrixValues`: the nine words promoted by columns (0, 3, 6, 1, 4,
/// 7, 2, 5, 8); `count` of them are consumed by the format.
fn matrix_values(m: &[f32; 9], count: usize) -> Vec<String> {
    [0, 3, 6, 1, 4, 7, 2, 5, 8][..count]
        .iter()
        .map(|&i| f6(m[i]))
        .collect()
}

/// The `target_type` literal of the spatial, attach, IK and floor writers.
fn target_type_literal(target_type: i32) -> &'static [u8] {
    match target_type {
        1 => b", target_type = HARDPOINT",
        2 => b", target_type = PART",
        _ => b", target_type = ROOT",
    }
}

/// The bytes of a C string up to its first NUL.
fn c_str(bytes: &[u8]) -> &[u8] {
    bytes.split(|&b| b == 0).next().unwrap_or_default()
}

/// `EscapeSceneScriptString` into the writer's 1024-byte buffer: `\`, CR,
/// LF and backspace become two-byte escapes while more than two bytes are
/// left; the text ends at the capacity, the last byte giving way to the
/// terminator when it is reached.
fn escape(source: &[u8]) -> Vec<u8> {
    const ESCAPE_CAPACITY: usize = 1024;
    let mut out = Vec::new();
    let mut remaining = ESCAPE_CAPACITY;
    for &byte in c_str(source) {
        if remaining == 0 {
            break;
        }
        let escape = match byte {
            b'\\' => Some(b'\\'),
            b'\r' => Some(b'r'),
            b'\n' => Some(b'n'),
            8 => Some(b'b'),
            _ => None,
        };
        match escape {
            Some(e) if remaining > 2 => {
                out.extend_from_slice(&[b'\\', e]);
                remaining -= 2;
            }
            _ => {
                out.push(byte);
                remaining -= 1;
            }
        }
    }
    if remaining == 0 {
        out.pop();
    }
    out
}

/// `FSceneScriptSaveBuf` and its append helpers. Each method stands for
/// one helper of the reference and keeps its flush rule.
struct SaveBuf<'a> {
    out: &'a mut dyn FnMut(&[u8]) -> usize,
    data: Vec<u8>,
    cap: usize,
}

impl SaveBuf<'_> {
    /// `SaveBufFlush`: one write of the pending bytes, which are dropped
    /// whatever it wrote; false for a short write.
    fn flush(&mut self) -> bool {
        if self.data.is_empty() {
            return true;
        }
        let written = (self.out)(&self.data);
        let complete = written == self.data.len();
        self.data.clear();
        complete
    }

    /// `SaveBufAppendText`, and the helpers that format into a scratch
    /// buffer and append it (`SaveBufAppendInteger`, `Literal`, `Float`,
    /// `Vector`, `Color`, `SaveMatrixValues`, the curve's points): a flush
    /// first when the text does not fit.
    fn text(&mut self, text: &[u8]) {
        if self.data.len() + text.len() > self.cap {
            self.flush();
        }
        self.data.extend_from_slice(text);
    }

    /// `SaveBufAppendStringValue` with the format `prefix%ssuffix`; a null
    /// value prints as `NULL`. A text longer than the buffer goes out in
    /// pieces of its capacity. For a text of 509 to 511 bytes the
    /// reference pads the first piece with NULs, drops up to 3 pending
    /// bytes and reads past the text; here the text is appended whole
    /// (divergence D542).
    fn value(&mut self, prefix: &[u8], value: Option<&[u8]>, suffix: &[u8]) {
        let value = value.map_or(&b"NULL"[..], c_str);
        let mut formatted = Vec::with_capacity(prefix.len() + value.len() + suffix.len());
        formatted.extend_from_slice(prefix);
        formatted.extend_from_slice(value);
        formatted.extend_from_slice(suffix);
        let length = formatted.len();
        if self.data.len() + length > self.cap {
            self.flush();
        }
        // The allocation the reference tests first: the format and the
        // value, two bytes more.
        let mut remaining = length + 4;
        let mut cursor = 0;
        if remaining > self.cap && length < self.cap {
            remaining = length;
        }
        while remaining > self.cap {
            // The pending bytes were flushed or there were none.
            self.data.clear();
            self.data
                .extend_from_slice(&formatted[cursor..cursor + self.cap]);
            cursor += self.cap;
            remaining = length - cursor;
            self.flush();
        }
        if remaining != 0 {
            self.data.extend_from_slice(&formatted[cursor..]);
        }
    }

    /// `SaveBufAppendVariadic`: formats into the space left; when the text
    /// does not fit, a flush (whose failure drops the text) and then the
    /// capacity doubled until it fits.
    fn variadic(&mut self, text: &[u8]) {
        if text.len() <= self.cap - self.data.len() {
            self.data.extend_from_slice(text);
            return;
        }
        if !self.flush() {
            return;
        }
        while text.len() > self.cap {
            self.cap *= 2;
        }
        self.data.extend_from_slice(text);
    }

    /// `SaveBufAppendStringFormat`: a flag token, ` +` before every token
    /// but the first.
    fn token(&mut self, token: &str, started: &mut bool) {
        let text = if *started {
            format!(" +{token}")
        } else {
            token.to_owned()
        };
        *started = true;
        if self.data.len() + text.len() > self.cap {
            self.flush();
        }
        self.data.extend_from_slice(text.as_bytes());
    }

    /// `SaveBufAppendLiteral` and the other one-text helpers.
    fn str(&mut self, text: &str) {
        self.text(text.as_bytes());
    }

    /// `SaveBufAppendStringValue(Buf, "%s = ", key)`.
    fn key(&mut self, key: &str) {
        self.value(b"", Some(key.as_bytes()), b" = ");
    }
}

/// An entity's descriptor tail (`Snap` after `+0x44`) as the words of the
/// reference's union: the view its owner type last wrote over the common
/// constructor's camera defaults.
fn tail_words(e: &Entity) -> [u32; 36] {
    let mut w = [0u32; 36];
    let camera = [60.0_f32, 0.1, 100_000.0, 4.0 / 3.0];
    for (word, value) in w.iter_mut().zip(camera) {
        *word = value.to_bits();
    }
    match e.owner_type {
        entity_kind::COMPOUND | entity_kind::DEFORMABLE => w[0] = e.snap_floor_height.to_bits(),
        entity_kind::CAMERA | entity_kind::MONITOR => {
            for (word, value) in w.iter_mut().zip(e.snap_camera) {
                *word = value.to_bits();
            }
        }
        entity_kind::LIGHT => w = light_words(&e.snap_light),
        entity_kind::SOUND => w[..8].copy_from_slice(&audio_words(&e.snap_audio)),
        entity_kind::SCENE | entity_kind::SUB_SCENE => w[..15].copy_from_slice(&e.snap_scene),
        // The path type's and the object's pointers; a view that reads them
        // as numbers reads 0 here (divergence D540).
        entity_kind::MOTION_PATH => {
            w[0] = 0;
            w[1] = 0;
        }
        entity_kind::PSYS => w[0] = e.snap_psys.to_bits(),
        _ => {}
    }
    w
}

fn light_words(l: &LightProps) -> [u32; 36] {
    let mut w = [0u32; 36];
    let f = |v: f32| v.to_bits();
    for i in 0..3 {
        w[i] = l.color[i] as u32;
        w[3 + i] = f(l.direction[i]);
        w[31 + i] = f(l.atten[i]);
    }
    w[6] = f(l.range_temp);
    w[7] = f(l.cutoff_temp);
    w[8] = l.on as u32;
    w[9] = l.infinite as u32;
    w[10] = l.light_type as u32;
    for i in 0..4 {
        w[11 + i] = f(l.diffuse[i]);
        w[15 + i] = f(l.specular[i]);
        w[19 + i] = f(l.ambient[i]);
    }
    for i in 0..6 {
        w[23 + i] = l.unused_17[i] as u32;
    }
    w[29] = f(l.range);
    w[30] = l.unused_1e;
    w[34] = f(l.theta);
    w[35] = f(l.cutoff);
    w
}

fn light_from_words(w: &[u32; 36]) -> LightProps {
    let f = |i: usize| f32::from_bits(w[i]);
    let i32_at = |i: usize| w[i] as i32;
    LightProps {
        color: [i32_at(0), i32_at(1), i32_at(2)],
        direction: [f(3), f(4), f(5)],
        range_temp: f(6),
        cutoff_temp: f(7),
        on: i32_at(8),
        infinite: i32_at(9),
        light_type: i32_at(10),
        diffuse: [f(11), f(12), f(13), f(14)],
        specular: [f(15), f(16), f(17), f(18)],
        ambient: [f(19), f(20), f(21), f(22)],
        unused_17: [
            i32_at(23),
            i32_at(24),
            i32_at(25),
            i32_at(26),
            i32_at(27),
            i32_at(28),
        ],
        range: f(29),
        unused_1e: w[30],
        atten: [f(31), f(32), f(33)],
        theta: f(34),
        cutoff: f(35),
    }
}

fn audio_words(a: &AudioProps) -> [u32; 8] {
    [
        a.attenuation.to_bits(),
        a.pan.to_bits(),
        a.dmin.to_bits(),
        a.dmax.to_bits(),
        a.ain as u32,
        a.aout as u32,
        a.atout.to_bits(),
        a.rmix.to_bits(),
    ]
}

/// `SaveLightAngle`: radians times 57.29578 (`0x42652ee0`), stored to a
/// `float`.
fn light_angle(v: f32) -> String {
    let scale = f32::from_bits(0x4265_2ee0);
    let scaled = (f64::from(v) * f64::from(scale)) as f32;
    format!("{}, ", f6(scaled))
}

fn yn(v: bool) -> &'static [u8] {
    if v { b"Y" } else { b"N" }
}

/// `SaveLightKey`.
fn light_key(buf: &mut SaveBuf<'_>, prefix: &[u8], key: &[u8]) {
    buf.value(b"", Some(prefix), b"");
    buf.value(b"", Some(key), b" = ");
}

/// `SaveLightProps`: each field the mask selects, as prefix, key, value
/// and suffix.
fn save_light(buf: &mut SaveBuf<'_>, prefix: &[u8], suffix: &[u8], p: &LightProps, mask: u32) {
    let has = |bit: i32| mask & bit as u32 != 0;
    let color = |c: &[i32; 3]| format!("{{{}, {}, {}}}, ", c[0], c[1], c[2]);
    let triple = |v: &[f32]| vector("{ ", v, " }, ");
    if has(light_bit::ON) {
        light_key(buf, prefix, b"on");
        buf.value(b"", Some(yn(p.on != 0)), b", ");
        buf.value(b"", Some(suffix), b"");
    }
    if has(light_bit::COLOR) {
        light_key(buf, prefix, b"color");
        buf.str(&color(&p.color));
        buf.value(b"", Some(suffix), b"");
    }
    for (bit, key, v) in [
        (light_bit::DIFFUSE, &b"diffuse"[..], &p.diffuse[..3]),
        (light_bit::SPECULAR, b"specular", &p.specular[..3]),
        (light_bit::AMBIENT, b"ambient", &p.ambient[..3]),
        (light_bit::DIRECTION, b"direction", &p.direction[..]),
    ] {
        if has(bit) {
            light_key(buf, prefix, key);
            buf.str(&triple(v));
            buf.value(b"", Some(suffix), b"");
        }
    }
    if has(light_bit::RANGE) {
        light_key(buf, prefix, b"range");
        buf.str(&format!("{}, ", f6(p.range)));
        buf.value(b"", Some(suffix), b"");
    }
    if has(light_bit::CUTOFF) {
        light_key(buf, prefix, b"cutoff");
        buf.str(&light_angle(p.cutoff));
        buf.value(b"", Some(suffix), b"");
    }
    if has(light_bit::TYPE) {
        light_key(buf, prefix, b"type");
        let name: &[u8] = match p.light_type {
            2 => b"L_SPOT",
            3 => b"L_DIRECT",
            _ => b"L_POINT",
        };
        buf.value(b"", Some(name), b", ");
        buf.value(b"", Some(suffix), b"");
    }
    if has(light_bit::THETA) {
        light_key(buf, prefix, b"theta");
        buf.str(&light_angle(p.theta));
        buf.value(b"", Some(suffix), b"");
    }
    if has(light_bit::ATTEN) {
        light_key(buf, prefix, b"atten");
        let a = |i: usize| printf_fixed(f64::from(p.atten[i]), 10);
        buf.str(&format!("{{ {}, {}, {} }}, ", a(0), a(1), a(2)));
        buf.value(b"", Some(suffix), b"");
    }
}

/// `SaveFogProps`.
fn save_fog(buf: &mut SaveBuf<'_>, p: &Fog, mask: u8) {
    if mask & 0x1 != 0 {
        buf.key("fogon");
        buf.value(b"", Some(yn(p.on != 0)), b", ");
    }
    if mask & 0x2 != 0 {
        buf.key("fogtable");
        buf.value(b"", Some(yn(p.table != 0)), b", ");
    }
    if mask & 0x4 != 0 {
        buf.key("fogcolor");
        buf.str(&format!(
            "{{{}, {}, {}}}, ",
            p.color[0], p.color[1], p.color[2]
        ));
    }
    if mask & 0x8 != 0 {
        buf.key("fogmode");
        let name: &[u8] = match p.mode {
            1 => b"F_EXP",
            2 => b"F_EXP2",
            3 => b"F_LINEAR",
            _ => b"F_NONE",
        };
        buf.value(b"", Some(name), b", ");
    }
    for (bit, key, v) in [
        (0x10, &b"fogstart"[..], p.start),
        (0x20, b"fogend", p.end),
        (0x40, b"fogdensity", p.density),
    ] {
        if mask & bit != 0 {
            buf.value(b"", Some(key), b" = ");
            buf.str(&format!("{}, ", f6(v)));
        }
    }
}

/// `SaveReverbProps`.
fn save_reverb(buf: &mut SaveBuf<'_>, p: &Reverb, mask: u8) {
    if mask & 0x1 != 0 {
        buf.key("env");
        buf.str(&format!("{}, ", p.environment));
    }
    for (bit, key, v) in [
        (0x2, &b"vol"[..], p.volume),
        (0x4, b"decay", p.decay),
        (0x8, b"damping", p.damping),
    ] {
        if mask & bit != 0 {
            buf.value(b"", Some(key), b" = ");
            buf.str(&format!("{}, ", f6(v)));
        }
    }
}

/// `SaveUseScriptDuration`.
fn use_script_duration(buf: &mut SaveBuf<'_>, desc: &EventDesc) {
    if desc.common.flags & flag::USE_SCRIPT_DURATION != 0 {
        buf.str(", flags = USE_SCRIPT_DURATION");
    }
}

/// The `event_flags` the writers add when it is not 0.
fn event_flags(buf: &mut SaveBuf<'_>, desc: &EventDesc) {
    if desc.common.event_flags != 0 {
        buf.str(&format!(", event_flags = {}", desc.common.event_flags));
    }
}

/// `SavePathAttachFlags`.
fn path_attach_flags(buf: &mut SaveBuf<'_>, desc: &EventDesc, attach: bool) {
    let flags = desc.common.flags;
    if flags & 0xf == 0 {
        return;
    }
    buf.str(", flags =");
    let mut started = false;
    for (bit, token) in [
        (flag::USE_SCRIPT_DURATION, " USE_SCRIPT_DURATION"),
        (flag::POSITION, " POSITION"),
        (flag::ORIENTATION, " ORIENTATION"),
        (flag::LOOK_AT, " LOOK_AT"),
        (FLAG_ENTITY_RELATIVE, " ENTITY_RELATIVE"),
    ] {
        if flags & bit != 0 {
            buf.token(token, &mut started);
        }
    }
    if attach && flags & FLAG_ORIENT_RELATIVE != 0 {
        buf.token(" ORIENTATION_RELATIVE", &mut started);
    }
    if attach && flags & FLAG_PARENT_CHILD != 0 {
        buf.token(" PARENT_CHILD", &mut started);
    }
}

/// The up and front axes of the path, attach and IK writers.
fn axes(buf: &mut SaveBuf<'_>, up: i32, front: i32) {
    buf.value(b", up = ", axis_name(up), b"");
    buf.value(b", front = ", axis_name(front), b"");
}

impl Player {
    /// `FSceneScriptScene::SaveToFileSystem`: writes the scene as Lua
    /// source through `write`, which stands for `IFileSystem::Write` and
    /// returns the bytes it took. The duration is the one the save parser
    /// was given; `com` is the engine's COM context, which hands out the
    /// text of user events (`Slot5`, `Slot4`). Path entities' paths are
    /// saved through their objects, which reopens a closed path, as in the
    /// reference.
    pub fn save_to(
        &mut self,
        mut com: Option<&mut dyn ComContext>,
        duration_ms: u32,
        write: &mut dyn FnMut(&[u8]) -> usize,
    ) {
        let mut buf = SaveBuf {
            out: write,
            data: Vec::with_capacity(CAPACITY),
            cap: CAPACITY,
        };
        buf.str(&format!(
            "duration = {};\r\n\r\n",
            f3(seconds_unsigned(duration_ms))
        ));
        buf.str("entities =\r\n{\r\n");
        let handles: Vec<i32> = self.world.entities.keys().copied().collect();
        for (ordinal, &handle) in handles.iter().enumerate() {
            let (pos, orient) = self.world.world_pose(handle).expect("listed");
            buf.str(if ordinal == 0 {
                "\r\n\t{\r\n"
            } else {
                ",\r\n\r\n\t{\r\n"
            });
            let e = self.world.get(handle).expect("listed");
            save_entity(&mut buf, e, &pos, &orient);
        }
        buf.str("\r\n};\r\n");
        buf.str("\r\nevents =\r\n{");
        for index in 0..self.master.len() {
            let key = self.master[index];
            // `as_deref_mut` keeps the object's `'static` bound, which the
            // reborrow cannot meet behind `&mut`.
            #[allow(clippy::option_as_ref_deref)]
            let com = com.as_mut().map(|c| -> &mut dyn ComContext { &mut **c });
            self.save_event(&mut buf, key, index, com);
        }
        buf.str("\r\n};\r\n");
        buf.flush();
    }

    /// [`Self::save_to`] into memory: the text the writes make.
    pub fn save_text(&mut self, com: Option<&mut dyn ComContext>, duration_ms: u32) -> Vec<u8> {
        let mut text = Vec::new();
        self.save_to(com, duration_ms, &mut |bytes| {
            text.extend_from_slice(bytes);
            bytes.len()
        });
        text
    }

    fn save_event(
        &self,
        buf: &mut SaveBuf<'_>,
        key: usize,
        index: usize,
        com: Option<&mut dyn ComContext>,
    ) {
        let event = self.ev(key);
        let desc = &event.desc;
        let common = &desc.common;
        let primary = self.world.entities.get(&common.primary);
        let secondary = if common.secondary >= 0 {
            self.world.entities.get(&common.secondary)
        } else {
            None
        };
        let name = |e: Option<&Entity>| e.and_then(|e| e.name.clone());
        buf.value(
            b"",
            Some(if index == 0 { &b""[..] } else { b"," }),
            b"\r\n\t",
        );
        buf.str(&format!(
            "{{{}",
            f3(seconds_unsigned(common.start_ms as u32))
        ));
        buf.value(b", ", Some(event_type_name(common.kind).as_bytes()), b"");
        buf.value(b", {\"", name(primary).as_deref(), b"\"");
        if secondary.is_some() {
            buf.value(b", \"", name(secondary).as_deref(), b"\"}");
        } else {
            buf.str("}");
        }
        let ty = common.kind;
        if ty == kind::SET_CAMERA || !(1..=19).contains(&ty) {
            buf.str("}");
            return;
        }
        let duration = f3(seconds_signed(common.duration_ms));
        if ty == kind::USER_EVENT {
            buf.str(",  {");
            buf.str(&format!("duration = {duration}"));
        } else if ty != kind::START_MOTION {
            buf.str(&format!(", {{duration = {duration}"));
        }
        let curve = |buf: &mut SaveBuf<'_>| {
            if let Some(curve) = event.curve.as_ref() {
                let count = curve.key_count();
                if count != 0 {
                    let mut header = Some(format!(
                        ", param_curve = {{CLSID = \"{}\", points = {{",
                        curve.class_name()
                    ));
                    for i in 0..count {
                        let id = curve.id_by_index(i as i32);
                        if id == -1 {
                            continue;
                        }
                        let Some(k) = curve.read_key(id) else {
                            continue;
                        };
                        let point = format!(
                            "{{ {}, {}, {}, {} }},",
                            f6(k[0]),
                            f6(k[1]),
                            f6(k[2]),
                            f6(k[3])
                        );
                        let text = match header.take() {
                            Some(h) => h + &point,
                            None => point,
                        };
                        buf.str(&text);
                    }
                    buf.str("}}");
                }
                buf.str(&format!(", pcurve_period = {}", common.pcurve_period));
            }
        };
        match &desc.payload {
            EventPayload::User { .. } if ty == kind::USER_EVENT => {
                event_flags(buf, desc);
                if let Some(com) = com {
                    let event_ref = EventRef { id: event.id, desc };
                    let size = com.user_event_string_size(event_ref);
                    if size != 0 {
                        // The reference's buffer is not cleared; a context
                        // that writes no terminator leaves it read past its
                        // text (divergence D543).
                        let mut buffer = vec![0u8; size as usize + 1];
                        if com.user_event_string(event_ref, &mut buffer, size) == 0 {
                            buf.value(b", user_event_string = \"", Some(&buffer), b"\"");
                        }
                    }
                }
            }
            EventPayload::StartSound { start_time } if ty == kind::START_SOUND => {
                if *start_time != 0 {
                    buf.str(&format!(", start_time = {start_time}"));
                }
                if common.flags & 0xf != 0 {
                    buf.str(", flags =");
                    let mut started = false;
                    for (bit, token) in [
                        (flag::USE_SCRIPT_DURATION, " USE_SCRIPT_DURATION"),
                        (2, " SPATIAL"),
                        (8, " LOOP"),
                        (4, " STREAM"),
                    ] {
                        if common.flags & bit != 0 {
                            buf.token(token, &mut started);
                        }
                    }
                }
            }
            EventPayload::Light { mask, target } if ty == kind::LIGHT_PROP_ANIM => {
                buf.str(", lightprops = {");
                save_light(buf, b"", b"", target, *mask as u32);
                buf.str("}");
                use_script_duration(buf, desc);
                curve(buf);
            }
            EventPayload::Camera { mask, target } if ty == kind::CAMERA_PROP_ANIM => {
                buf.str(", cameraprops = {");
                let mut wrote = false;
                for (i, key) in [&b"fovh"[..], b"aspect", b"near", b"far"]
                    .into_iter()
                    .enumerate()
                {
                    if mask & (1 << i) == 0 {
                        continue;
                    }
                    if wrote {
                        buf.str(", ");
                    }
                    buf.value(b"", Some(key), b" = ");
                    buf.str(&f6(target[i]));
                    wrote = true;
                }
                buf.str("}");
                use_script_duration(buf, desc);
                event_flags(buf, desc);
                curve(buf);
            }
            EventPayload::Path {
                start_percent,
                stop_percent,
                offset,
                up,
                front,
                ..
            } if ty == kind::PATH_ANIM => {
                buf.str(&format!(", start_percent = {}", f6(*start_percent)));
                buf.str(&format!(", stop_percent = {}", f6(*stop_percent)));
                buf.str(&vector(", offset = { ", offset, " }"));
                axes(buf, *up, *front);
                path_attach_flags(buf, desc, false);
                event_flags(buf, desc);
                curve(buf);
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
            } if ty == kind::SPATIAL_PROP_ANIM => {
                if let Some(part) = target_part {
                    buf.value(b", target_part = \"", Some(part), b"\"");
                }
                buf.text(target_type_literal(*target_type));
                buf.str(", spatialprops = {");
                let wrote_position = mask & spatial_bit::POS != 0;
                if wrote_position {
                    buf.key("pos");
                    buf.str(&vector("{ ", position, " }"));
                }
                if mask & spatial_bit::ORIENT != 0 {
                    if wrote_position {
                        buf.str(", ");
                    }
                    if common.flags & flag::QUAT_ORIENT != 0 {
                        // The matrix of the quaternion, printed with the
                        // first four of its column-promoted words.
                        let m = quat_to_matrix(quaternion);
                        let v = matrix_values(&m, 4);
                        buf.str(&format!(
                            "q_orient = {{ {}, {}, {}, {} }}",
                            v[0], v[1], v[2], v[3]
                        ));
                    } else {
                        let v = matrix_values(orient, 9);
                        buf.str(&format!(
                            "orient = {{ {{ {}, {}, {} }},{{ {}, {}, {} }},{{ {}, {}, {} }} }}",
                            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]
                        ));
                    }
                }
                if mask & spatial_bit::AXIS_ROT != 0 {
                    if wrote_position {
                        buf.str(", ");
                    }
                    buf.key("axisrot");
                    buf.str(&format!("{{{}, ", f6(*axis_rot_angle)));
                    buf.value(b"", axis_name(*axis_rot_axis), b"}");
                }
                buf.str("}");
                use_script_duration(buf, desc);
                event_flags(buf, desc);
                curve(buf);
            }
            EventPayload::Attach {
                offset,
                target_type,
                up,
                front,
                target_part,
            } if ty == kind::ATTACH_ENTITY => {
                buf.str(&vector(", offset = { ", offset, " }"));
                axes(buf, *up, *front);
                if let Some(part) = target_part {
                    buf.value(b", target_part = \"", Some(part), b"\"");
                }
                buf.text(target_type_literal(*target_type));
                path_attach_flags(buf, desc, true);
            }
            EventPayload::Connect {
                hardpoint,
                parent_hardpoint,
            } if ty == kind::CONNECT_HARDPOINTS => {
                buf.value(b", hardpoint = \"", hardpoint.as_deref(), b"\"");
                buf.value(
                    b", parent_hardpoint = \"",
                    parent_hardpoint.as_deref(),
                    b"\"",
                );
                use_script_duration(buf, desc);
            }
            EventPayload::Motion {
                start_time,
                trans_time,
                time_scale,
                weight,
                animation,
                heading,
                locked_bone,
                trans_scale,
                ..
            } if ty == kind::START_MOTION => {
                if let Some(animation) = animation {
                    buf.value(b", {animation = \"", Some(animation), b"\"");
                }
                if let Some(bone) = locked_bone {
                    buf.value(b", locked_bone = \"", Some(bone), b"\"");
                }
                buf.str(&format!(", duration = {duration}"));
                if nonzero(*start_time) {
                    buf.str(&format!(", start_time = {}", f3(*start_time)));
                }
                if nonzero(*trans_time) {
                    buf.str(&format!(", trans_time = {}", f3(*trans_time)));
                }
                if nonzero(*time_scale) {
                    buf.str(&format!(", time_scale = {}", f6(*time_scale)));
                }
                if nonzero(*weight) {
                    buf.str(&format!(", weight = {}", f6(*weight)));
                }
                if primary.is_some_and(|p| p.kind == entity_kind::DEFORMABLE) {
                    buf.str(&format!(", heading = {}", f6(*heading)));
                } else if nonzero(*trans_scale) {
                    buf.str(&format!(", trans_scale = {}", f6(*trans_scale)));
                }
                use_script_duration(buf, desc);
            }
            EventPayload::Ik {
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
                ..
            } if ty == kind::START_IK => {
                buf.value(b", end_effector = \"", end_effector.as_deref(), b"\"");
                buf.str(&format!(", count_to_root = {count_to_root}"));
                if let Some(part) = target_part {
                    buf.value(b", target_part = \"", Some(part), b"\"");
                }
                buf.text(target_type_literal(*target_type));
                buf.str(&format!(", damping = {}", f6(*damping)));
                axes(buf, *up, *front);
                buf.str(&format!(", point_at = {point_at}"));
                buf.str(&format!(", move_to = {move_to}"));
                buf.str(&format!(
                    ", transition_duration = {}",
                    f3(*transition_duration)
                ));
                use_script_duration(buf, desc);
            }
            EventPayload::SubScene { start_time } if ty == kind::START_SUB_SCENE => {
                if nonzero(*start_time) {
                    // `%d` of the promoted double reads its low word.
                    let low = f64::from(*start_time).to_bits() as u32 as i32;
                    buf.str(&format!(", start_time = {low}"));
                }
                if common.flags & flag::USE_SCRIPT_DURATION != 0 {
                    buf.str(", flags =");
                    let mut started = false;
                    buf.token(" USE_SCRIPT_DURATION", &mut started);
                }
            }
            EventPayload::PsysProp { mask, sparam } if ty == kind::PSYS_PROP_ANIM => {
                event_flags(buf, desc);
                buf.str(", psysprops = {");
                if mask & 1 != 0 {
                    buf.key("sparam");
                    buf.str(&f6(*sparam));
                }
                buf.str("}");
                curve(buf);
            }
            EventPayload::Audio { mask, target } if ty == kind::AUDIO_PROP_ANIM => {
                buf.str(", audioprops = {");
                let words = audio_words(target);
                let mut wrote = false;
                for (i, key) in [
                    &b"attenuation"[..],
                    b"pan",
                    b"dmin",
                    b"dmax",
                    b"ain",
                    b"aout",
                    b"atout",
                    b"rmix",
                ]
                .into_iter()
                .enumerate()
                {
                    if mask & (1 << i) == 0 {
                        continue;
                    }
                    if wrote {
                        buf.str(", ");
                    }
                    buf.value(b"", Some(key), b" = ");
                    if i == 4 || i == 5 {
                        // `%.6f` of a double made of the integer and, as its
                        // high word, the address of the secondary's
                        // descriptor; here the high word is 0 (divergence
                        // D544).
                        buf.str(&printf_fixed(f64::from_bits(u64::from(words[i])), 6));
                    } else {
                        buf.str(&f6(f32::from_bits(words[i])));
                    }
                    wrote = true;
                }
                buf.str("}");
                use_script_duration(buf, desc);
                curve(buf);
            }
            EventPayload::Fog { mask, target } if ty == kind::FOG_PROP_ANIM => {
                event_flags(buf, desc);
                buf.str(", fogprops = {");
                save_fog(buf, target, *mask as u8);
                buf.str("}");
                curve(buf);
            }
            EventPayload::Reverb { mask, target } if ty == kind::REVERB_PROP_ANIM => {
                event_flags(buf, desc);
                buf.str(", reverbprops = {");
                save_reverb(buf, target, *mask as u8);
                buf.str("}");
                curve(buf);
            }
            EventPayload::FloorHeight {
                floor_height,
                target_part,
                target_type,
            } if ty == kind::FLOOR_HEIGHT_ANIM => {
                if let Some(part) = target_part {
                    buf.value(b", target_part = \"", Some(part), b"\"");
                }
                buf.text(target_type_literal(*target_type));
                buf.str(&format!(", floor_height = {}", f6(*floor_height)));
                use_script_duration(buf, desc);
                event_flags(buf, desc);
                curve(buf);
            }
            EventPayload::Subtitle { strid } if ty == kind::SUBTITLE && *strid != 0 => {
                buf.str(&format!(", strid = {strid}"));
            }
            _ => {}
        }
        if matches!(
            ty,
            kind::START_MOTION
                | kind::START_SOUND
                | kind::START_IK
                | kind::ATTACH_ENTITY
                | kind::CONNECT_HARDPOINTS
                | kind::START_SUB_SCENE
                | kind::SUBTITLE
                | kind::START_PSYS
        ) {
            event_flags(buf, desc);
        }
        buf.str("}");
        buf.str("}");
    }
}

/// One entity of the `entities` table, from `{` to its last field.
fn save_entity(buf: &mut SaveBuf<'_>, e: &mut Entity, pos: &[f32; 3], orient: &[f32; 9]) {
    let ty = e.kind;
    buf.value(b"\t\tentity_name = \"", e.name.as_deref(), b"\",\r\n");
    buf.value(
        b"\t\ttype = ",
        Some(entity_type_name(ty).as_bytes()),
        b",\r\n",
    );
    let template = e.template_name.as_deref().map(escape).unwrap_or_default();
    buf.value(b"\t\ttemplate_name = \"", Some(&template), b"\",\r\n");
    buf.str(&format!("\t\tlt_grp = {},", e.groups[0]));
    buf.str(&format!(" srt_grp = {},", e.groups[1] as i8));
    buf.str(&format!(" usr_flg = {}", e.groups[2]));
    let flags = e.flags;
    if flags != 0 {
        let mut started = false;
        let mut known: u32 = 0x11;
        buf.str(",\r\n\t\tflags =");
        if flags & 1 != 0 {
            buf.token(" REFERENCE", &mut started);
        }
        if flags & 0x10 != 0 {
            buf.token(" HIDDEN", &mut started);
        }
        match ty {
            1 | 2 | 13 => {
                known = 0x17;
                if flags & 4 != 0 {
                    buf.token(" LIT_DYNAMIC", &mut started);
                }
                if flags & 2 != 0 {
                    buf.token(" LIT_AMBIENT", &mut started);
                }
            }
            6 => {
                known = 0x1f;
                if flags & 2 != 0 {
                    buf.token(" SPATIAL", &mut started);
                }
                if flags & 8 != 0 {
                    buf.token(" LOOP", &mut started);
                }
                if flags & 4 != 0 {
                    buf.token(" STREAM", &mut started);
                }
            }
            9 => {
                known = 0x31;
                if flags & 0x20 != 0 {
                    buf.token(" FOG_PROPS_REMOVED", &mut started);
                }
            }
            _ => {}
        }
        let unknown = flags as u32 & !known;
        if unknown != 0 {
            if started {
                buf.str("+");
            }
            buf.str(&format!(" {}", unknown as i32));
        }
    }
    if matches!(ty, 1 | 2 | 3 | 5 | 6 | 7 | 9 | 10 | 11 | 13) {
        buf.str(",\r\n\t\tspatialprops =\r\n\t\t{\r\n");
        buf.str(&vector("\t\t\tpos = { ", pos, " },\r\n"));
        let v = matrix_values(orient, 9);
        buf.str(&format!(
            "\t\t\torient = {{ {{ {}, {}, {} }},{{ {}, {}, {} }},{{ {}, {}, {} }} }}\r\n",
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]
        ));
        buf.str("\t\t}");
    }
    let w = tail_words(e);
    let word = |i: usize| f32::from_bits(w[i]);
    match ty {
        3 => {
            buf.str(",\r\n\t\tcameraprops =\r\n\t\t{\r\n");
            buf.str(&format!("\t\t\tfovh = {},\r\n", f6(word(0))));
            buf.str(&format!("\t\t\thvaspect = {},\r\n", f6(word(3))));
            buf.str(&format!("\t\t\tnearplane = {},\r\n", f6(word(1))));
            buf.str(&format!("\t\t\tfarplane = {}\r\n", f6(word(2))));
            buf.str("\t\t}");
        }
        2 => {
            buf.str(",\r\n\t\tcompoundprops =\r\n\t\t{\r\n");
            buf.str(&format!("\t\t\tfloor_height = {}\r\n", f6(word(0))));
            buf.str("\t\t}");
        }
        5 => {
            buf.str(",\r\n\t\tlightprops =\r\n\t\t{\r\n");
            save_light(buf, b"\t\t\t", b"\r\n", &light_from_words(&w), u32::MAX);
            buf.str("\t\t}");
        }
        6 => {
            buf.str(",\r\n\t\taudioprops =\r\n\t\t{\r\n");
            for (i, key) in [
                "attenuation",
                "pan",
                "dmin",
                "dmax",
                "ain",
                "aout",
                "atout",
                "rmix",
            ]
            .into_iter()
            .enumerate()
            {
                let value = if i == 4 || i == 5 {
                    w[i].to_string()
                } else {
                    f6(word(i))
                };
                buf.variadic(format!("\t\t\t{key} = {value},\r\n").as_bytes());
            }
            buf.str("\t\t}");
        }
        9 | 10 => {
            buf.value(b",\r\n\t\tup = ", axis_name(w[5] as i32), b"");
            buf.value(b",\r\n\t\tfront = ", axis_name(w[6] as i32), b"");
            buf.str(&format!(
                ",\r\n\t\tambient = {{{},{},{}}}",
                w[0] as i32, w[1] as i32, w[2] as i32
            ));
        }
        11 => {
            buf.str(",\r\n\t\tpathprops =\r\n\t\t{\r\n");
            let own = e.owner_type == entity_kind::MOTION_PATH;
            let path_type = if own { e.path_type.clone() } else { None };
            buf.value(b"\r\n\t\t\tpath_type = \"", path_type.as_deref(), b"\"");
            let object = if own {
                match &mut e.snap_path {
                    Some(replaced) => replaced.as_mut(),
                    None => e.path.as_mut(),
                }
            } else {
                None
            };
            if let Some(path) = object {
                let size = path.estimate_size();
                if size != 0 {
                    // Without points the reference prints the buffer it
                    // never wrote; here it is empty (divergence D543).
                    let (_, text) = path.save_text(size);
                    let text = text.unwrap_or_default();
                    buf.value(b",\r\n\t\t\tpath_data = \"", Some(&text), b"\"\r\n");
                }
            }
            buf.str("\t\t}");
        }
        13 => {
            buf.str(",\r\n\t\tpsysprops =\r\n\t\t{\r\n");
            buf.str(&format!("\t\t\tsparam = {}\r\n", f6(word(0))));
            buf.str("\t\t}");
        }
        _ => {}
    }
    if !e.user_props.is_empty() {
        buf.str(",\r\n\t\tuserprops =\r\n\t\t{\r\n");
        for (key, value) in &e.user_props {
            buf.value(b"\t\t\t", Some(key), b" = ");
            buf.value(b"\"", Some(value), b"\",\r\n");
        }
        buf.str("\t\t}");
    }
    buf.str("\r\n\t}");
}
