//! The second pass of the scene-script parser, `FSceneScriptLuaLoadParser::
//! Build` in `CppSource/SceneScript/SceneScriptLua.cpp`: the entities and
//! events of an evaluated script, as the descriptors the reference hands to
//! its `IScriptEngine`.
//!
//! The reference walks `entities` and then `events` with Lua's `foreach`
//! and calls the engine as it goes: `Slot35AddEntity` and `Slot34SetUserProp`
//! per entity, `Slot38AddEvent` per event (after building it in a temporary
//! `FSceneScriptEvent`), `Slot37UpdateEntity` for a positioned sound and
//! `ISceneScriptComContext::Slot3` for a user event's text. [`build`] returns
//! those calls in order as [`Step`]s. It keeps the reference's reads: which
//! keys it looks at, the default it keeps when a key is missing or holds the
//! wrong kind of value, which numbers it stores as floats and which it chops
//! to integers (to 64 bits, keeping the low 32, as its `FISTP` does), and
//! the order of the calls. Arithmetic follows design/decisions.md T11.
//!
//! The parser creates param curves and motion paths through the component
//! manager; here they are data ([`ParamCurve`], [`PathObject`]) holding what
//! the parser gave them. Evaluating them is the work of the curve and spline
//! ports.

use std::collections::HashMap;
use std::fmt;

use neuron_core::num;

use super::entity::{
    AudioProps, EntityDesc, EntityTail, Fog, LightProps, MotionPathDesc, PathFactory, PathInit,
    PathObject, SceneProps, kind as entity_kind, light_bit,
};
use super::event::{
    CURVE_CLASSES, EventCommon, EventDesc, EventPayload, IkKey, ParamCurve, Reverb, flag,
    kind as event_kind, spatial_bit,
};
use super::load::{LoadError, LoadSummary, get_number, load};
use super::number;
use super::table::Value;
use super::vm::Vm;

/// One call the reference's parser makes on its engine, in order.
#[derive(Clone, Debug, PartialEq)]
pub enum Step {
    /// `Slot35AddEntity`. The engine numbers entities from 0 in the order
    /// they are added; that number is the handle the later steps use.
    AddEntity(EntityDesc),
    /// `Slot34SetUserProp`, for each string or number value of the entity's
    /// `userprops`. The key is `None` for a key that is neither (a null
    /// pointer in the reference).
    SetUserProp {
        entity: i32,
        key: Option<Vec<u8>>,
        value: Vec<u8>,
    },
    /// `Slot37UpdateEntity`, before a START_SOUND event whose flags hold
    /// `POSITION`. The reference passes the engine's own descriptor of the
    /// entity with the bits of its first position word replaced by the
    /// entity's flags with bit 1 set; `desc` is the descriptor this build
    /// added with that change.
    UpdateEntity { entity: i32, desc: EntityDesc },
    /// `Slot38AddEvent`.
    AddEvent(EventDesc),
}

/// What a build produced.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Scene {
    pub steps: Vec<Step>,
}

impl Scene {
    /// The entity descriptors, by handle.
    pub fn entities(&self) -> impl Iterator<Item = &EntityDesc> {
        self.steps.iter().filter_map(|s| match s {
            Step::AddEntity(e) => Some(e),
            _ => None,
        })
    }

    /// The event descriptors, in the order they were added.
    pub fn events(&self) -> impl Iterator<Item = &EventDesc> {
        self.steps.iter().filter_map(|s| match s {
            Step::AddEvent(e) => Some(e),
            _ => None,
        })
    }
}

/// The parser's module-lifetime state besides the Lua state: the handle of
/// the last entity added under each name (`G_LuaEntityHandles`), which no
/// load clears. An event names its entities through it, so a script can
/// reach a handle an earlier script published.
#[derive(Clone, Debug, Default)]
pub struct BuildState {
    handles: HashMap<Vec<u8>, i32>,
}

impl BuildState {
    #[must_use]
    pub fn new() -> BuildState {
        BuildState::default()
    }

    /// The handle published under `name`.
    #[must_use]
    pub fn handle(&self, name: &[u8]) -> Option<i32> {
        self.handles.get(name).copied()
    }
}

/// Where the reference would stop with an access violation; the port stops
/// with this error instead (divergence D240).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BuildError {
    /// The first pass, run again because it found no entities and no
    /// duration, failed.
    Load(LoadError),
    /// The reference reads a C string where Lua gave it a null pointer
    /// (the value is not a string or a number).
    NullString(&'static str),
    /// The reference reads the descriptor of an event's entity, which
    /// names no entity of this scene.
    NoEntity(&'static str),
}

impl fmt::Display for BuildError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Load(e) => write!(f, "{e}"),
            Self::NullString(what) => write!(f, "the scene script's {what} is not a string"),
            Self::NoEntity(what) => write!(f, "the scene script's {what} names no entity"),
        }
    }
}

impl std::error::Error for BuildError {}

/// Builds the scene of the script `bytes`, after [`load`] gave `summary`
/// for it in `vm`, as `FScriptEngine::Slot4Load` calls the parser.
/// `com_context` tells whether the engine has a COM context
/// (`Slot40SetCom`); without one the reference adds no user events.
pub fn build(
    vm: &mut Vm,
    state: &mut BuildState,
    bytes: &[u8],
    summary: LoadSummary,
    com_context: bool,
) -> Result<Scene, BuildError> {
    // `Build` runs `Load` again when the first pass found neither.
    let summary = if summary.entities == 0 && summary.duration_ms == 0 {
        load(vm, bytes).map_err(BuildError::Load)?
    } else {
        summary
    };
    let mut b = Builder {
        vm,
        state,
        duration_ms: summary.duration_ms,
        com_context,
        steps: Vec::new(),
        entities: Vec::new(),
    };
    if summary.entities != 0 {
        for (key, value) in b.foreach("entities") {
            b.entity(&key, &value)?;
        }
    }
    if summary.events != 0 {
        for (_, value) in b.foreach("events") {
            b.event(&value)?;
        }
    }
    Ok(Scene { steps: b.steps })
}

/// `lua_getnumber`.
fn number(v: &Value) -> f64 {
    get_number(v)
}

/// A value stored to a `float`.
fn float(v: &Value) -> f32 {
    number(v) as f32
}

/// `LuaNumberTobaselineInt`: chopped to 64 bits, the low word kept.
fn chop(v: &Value) -> i32 {
    num::trunc_to_i32_via_i64(number(v))
}

/// `LuaMillisecondsTobaselineInt`: seconds times 1000, chopped likewise.
fn millis(v: &Value) -> i32 {
    num::trunc_to_i32_via_i64(number(v) * 1000.0)
}

/// The chop of a stored float (`StoreLightColorIntegers`,
/// `StoreSceneAmbientIntegers`).
fn chop_float(x: f32) -> i32 {
    num::trunc_to_i32_via_i64(f64::from(x))
}

/// `lua_isstring`: a string or a number.
fn is_string(v: &Value) -> bool {
    matches!(v, Value::String(_) | Value::Number(_))
}

/// `lua_getstring`: a string up to its first NUL, a number as its text,
/// `None` (a null pointer) for anything else.
fn string(v: &Value) -> Option<Vec<u8>> {
    match v {
        Value::String(s) => Some(s.split(|&b| b == 0).next().unwrap_or(&[]).to_vec()),
        Value::Number(x) => Some(number::format(*x)),
        _ => None,
    }
}

/// `ScaleLightAngle`: degrees to radians with the float constant.
fn radians(degrees: f32) -> f32 {
    (f64::from(degrees) * f64::from(DEGREE)) as f32
}

/// The float π/180 (bits `3c8efa35`).
const DEGREE: f32 = 0.017_453_292;
/// The float 1/255 (bits `3b808081`).
const BY_255: f32 = 1.0 / 255.0;
/// The float π (bits `40490fdb`).
const PI: f32 = std::f32::consts::PI;
const IDENTITY: [f32; 9] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0];

/// A colour channel scaled to 0..1 (`fild`, `fmul` by the float 1/255,
/// stored to a float).
fn unit_color(c: i32) -> f32 {
    (f64::from(c) * f64::from(BY_255)) as f32
}

struct Builder<'a> {
    vm: &'a Vm,
    state: &'a mut BuildState,
    duration_ms: u32,
    com_context: bool,
    steps: Vec<Step>,
    /// The step of each entity, by handle.
    entities: Vec<usize>,
}

impl Builder<'_> {
    /// The entries `foreach` visits in the table a global holds. The
    /// reference calls whatever the global `foreach` holds (divergence
    /// D242).
    fn foreach(&self, global: &str) -> Vec<(Value, Value)> {
        match self.vm.global(global) {
            Value::Table(id) => self
                .vm
                .table(*id)
                .entries()
                .map(|(k, v)| (k.clone(), v.clone()))
                .collect(),
            _ => Vec::new(),
        }
    }

    /// `GetFieldOrNil`.
    fn field(&self, table: &Value, key: &str) -> Value {
        match table {
            Value::Table(id) => self.vm.table(*id).field(key).clone(),
            _ => Value::Nil,
        }
    }

    /// `GetIndexOrNil` and the probes of the vector readers.
    fn index(&self, table: &Value, i: u32) -> Value {
        match table {
            Value::Table(id) => self.vm.table(*id).get(&Value::Number(f64::from(i))).clone(),
            _ => Value::Nil,
        }
    }

    /// The vector readers (`ReadVec3Complete`, `ReadLightInlineVector`,
    /// `ReadSpatialEventVector`, ...): items 1 to `N` as floats when none is
    /// nil, else zeros.
    fn vector<const N: usize>(&self, table: &Value) -> [f32; N] {
        let items: Vec<Value> = (1..=N as u32).map(|i| self.index(table, i)).collect();
        if items.iter().any(Value::is_nil) {
            return [0.0; N];
        }
        std::array::from_fn(|i| float(&items[i]))
    }

    fn entity_desc(&self, handle: i32) -> Option<&EntityDesc> {
        let step = *self.entities.get(usize::try_from(handle).ok()?)?;
        match &self.steps[step] {
            Step::AddEntity(e) => Some(e),
            _ => None,
        }
    }

    // --- Entities (`ForeachEntity`) -------------------------------------

    fn entity(&mut self, key: &Value, table: &Value) -> Result<(), BuildError> {
        if table.as_table().is_none() {
            return Ok(());
        }
        let type_value = self.field(table, "type");
        if type_value.is_nil() {
            // The reference returns here without closing its Lua block
            // (divergence D241).
            return Ok(());
        }
        let kind = chop(&type_value);
        let flags = chop(&self.field(table, "flags"));
        let name_value = self.field(table, "entity_name");
        let name = string(if is_string(&name_value) {
            &name_value
        } else {
            key
        });
        let template_value = self.field(table, "template_name");
        let mut template = if is_string(&template_value) {
            string(&template_value).map(|t| if t == b"NULL" { Vec::new() } else { t })
        } else {
            None
        };
        // `static_cast<unsigned char>` of the chopped number.
        let group = |b: &Self, key: &str| {
            let v = b.field(table, key);
            if v.is_nil() {
                0
            } else {
                chop(&v).to_le_bytes()[0]
            }
        };
        let (lt_grp, srt_grp, usr_flg) = (
            group(self, "lt_grp"),
            group(self, "srt_grp"),
            group(self, "usr_flg"),
        );
        let tail = match kind {
            entity_kind::COMPOUND | entity_kind::DEFORMABLE | entity_kind::PSYS => {
                if template.is_none() {
                    template.clone_from(&name);
                }
                let psys = kind == entity_kind::PSYS;
                let props = self.field(table, if psys { "psysprops" } else { "compoundprops" });
                let mut value = 0.0;
                if props.as_table().is_some() {
                    let v = self.field(&props, if psys { "sparam" } else { "floor_height" });
                    if !v.is_nil() {
                        value = float(&v);
                    }
                }
                if psys {
                    EntityTail::Psys(value)
                } else {
                    EntityTail::FloorHeight(value)
                }
            }
            entity_kind::CAMERA | entity_kind::MONITOR => self.camera(table),
            entity_kind::LIGHT => {
                let mut props = LightProps::entity_defaults();
                let light = self.field(table, "lightprops");
                if light.as_table().is_some() {
                    let mask = self.light_props(&light, &mut props);
                    if mask & light_bit::TYPE == 0 {
                        convert_compact_light(&mut props);
                    }
                }
                EntityTail::Light(props)
            }
            entity_kind::SOUND => {
                let audio = self.field(table, "audioprops");
                if template.is_none() && audio.as_table().is_some() {
                    template = string(&self.field(&audio, "archetype"));
                }
                EntityTail::Sound(self.audio(&audio, AudioProps::default()).0)
            }
            entity_kind::MARKER | entity_kind::HARDPOINT => EntityTail::None,
            entity_kind::SCENE | entity_kind::SUB_SCENE => EntityTail::Scene(self.scene(table)),
            entity_kind::MOTION_PATH => {
                EntityTail::MotionPath(self.motion_path(&self.field(table, "pathprops")))
            }
            _ => return Ok(()),
        };
        let mut desc = EntityDesc {
            kind,
            flags,
            pos: [0.0; 3],
            orient: IDENTITY,
            name,
            template_name: template,
            lt_grp,
            srt_grp,
            usr_flg,
            tail,
        };
        self.entity_spatial(table, &mut desc);
        let handle = i32::try_from(self.entities.len()).expect("fewer than 2^31 entities");
        // The reference copies the name into its map after the engine call;
        // a null name faults there.
        let published = desc.name.clone();
        self.entities.push(self.steps.len());
        self.steps.push(Step::AddEntity(desc));
        let published = published.ok_or(BuildError::NullString("entity name"))?;
        self.state.handles.insert(published, handle);
        let props = self.field(table, "userprops");
        if let Value::Table(id) = props {
            for (k, v) in self.vm.table(id).entries() {
                if !is_string(v) {
                    continue;
                }
                let value = string(v).expect("a string or a number has text");
                self.steps.push(Step::SetUserProp {
                    entity: handle,
                    key: string(k),
                    value,
                });
            }
        }
        Ok(())
    }

    /// `LoadEntitySpatialDesc`: a present `pos` replaces the position (zero
    /// when incomplete); each present row of `orient` replaces a column.
    fn entity_spatial(&self, table: &Value, desc: &mut EntityDesc) {
        let spatial = self.field(table, "spatialprops");
        if spatial.is_nil() {
            return;
        }
        let pos = self.field(&spatial, "pos");
        if !pos.is_nil() {
            desc.pos = self.vector(&pos);
        }
        let orient = self.field(&spatial, "orient");
        if orient.is_nil() {
            return;
        }
        set_columns(self, &orient, &mut desc.orient);
    }

    fn camera(&self, table: &Value) -> EntityTail {
        // 60 degrees, 0.1, 100000, 4/3 as raw float bits.
        let mut out = [
            f32::from_bits(0x4270_0000),
            f32::from_bits(0x3dcc_cccd),
            f32::from_bits(0x47c3_5000),
            f32::from_bits(0x3faa_aaab),
        ];
        let camera = self.field(table, "cameraprops");
        if camera.as_table().is_some() {
            // fovh, hvaspect, nearplane, farplane into fovh, aspect, near, far.
            for (key, at) in [
                ("fovh", 0),
                ("hvaspect", 3),
                ("nearplane", 1),
                ("farplane", 2),
            ] {
                let v = self.field(&camera, key);
                if !v.is_nil() {
                    out[at] = float(&v);
                }
            }
        }
        EntityTail::Camera {
            fovh: out[0],
            near_plane: out[1],
            far_plane: out[2],
            aspect: out[3],
        }
    }

    /// The eight audio keys into `out`, and the mask of those present
    /// (`LoadAudioEntityDesc`, the audio event).
    fn audio(&self, audio: &Value, mut out: AudioProps) -> (AudioProps, i32) {
        let mut mask = 0;
        if audio.as_table().is_none() {
            return (out, mask);
        }
        let keys = [
            "attenuation",
            "pan",
            "dmin",
            "dmax",
            "ain",
            "aout",
            "atout",
            "rmix",
        ];
        for (bit, key) in keys.into_iter().enumerate() {
            let v = self.field(audio, key);
            if v.is_nil() {
                continue;
            }
            mask |= 1 << bit;
            match bit {
                0 => out.attenuation = float(&v),
                1 => out.pan = float(&v),
                2 => out.dmin = float(&v),
                3 => out.dmax = float(&v),
                4 => out.ain = chop(&v),
                5 => out.aout = chop(&v),
                6 => out.atout = float(&v),
                _ => out.rmix = float(&v),
            }
        }
        (out, mask)
    }

    /// `LoadSceneEntityDesc`.
    fn scene(&self, table: &Value) -> SceneProps {
        let mut scene = SceneProps {
            duration_ms: self.duration_ms,
            // `ReadSceneStartMilliseconds`: nil reads as 0.
            start_ms: millis(&self.field(table, "start_time")),
            up: 6,
            front: 6,
            ..SceneProps::default()
        };
        let ambient = self.field(table, "ambient");
        if !ambient.is_nil() {
            scene.ambient = self.vector::<3>(&ambient).map(chop_float);
        }
        let up = self.field(table, "up");
        if !up.is_nil() {
            scene.up = chop(&up);
        }
        let front = self.field(table, "front");
        if !front.is_nil() {
            scene.front = chop(&front);
        }
        scene.fog = self.fog(table, Fog::default()).0;
        scene
    }

    /// `LoadFogKeys`: the fog keys of `table` into `out`, and the mask of
    /// those present.
    fn fog(&self, table: &Value, mut out: Fog) -> (Fog, i32) {
        let mut mask = 0;
        // `ReadFogByte`: 0 only for a number equal to zero.
        let byte = |v: &Value| u8::from(number(v) != 0.0);
        let v = self.field(table, "fogon");
        if !v.is_nil() {
            out.on = byte(&v);
            mask |= 0x1;
        }
        let v = self.field(table, "fogtable");
        if !v.is_nil() {
            out.table = byte(&v);
            mask |= 0x2;
        }
        let v = self.field(table, "fogcolor");
        if !v.is_nil() {
            out.color = self.vector::<3>(&v).map(chop_float);
            mask |= 0x4;
        }
        let v = self.field(table, "fogmode");
        if !v.is_nil() {
            out.mode = chop(&v);
            mask |= 0x8;
        }
        for (key, bit, slot) in [
            ("fogstart", 0x10, &mut out.start),
            ("fogend", 0x20, &mut out.end),
            ("fogdensity", 0x40, &mut out.density),
        ] {
            let v = self.field(table, key);
            if !v.is_nil() {
                *slot = float(&v);
                mask |= bit;
            }
        }
        (out, mask)
    }

    /// `InstantiateMotionPath`.
    fn motion_path(&self, table: &Value) -> MotionPathDesc {
        let mut desc = MotionPathDesc::default();
        if table.as_table().is_none() {
            return desc;
        }
        let type_value = self.field(table, "path_type");
        let explicit = !type_value.is_nil();
        desc.path_type = if explicit {
            string(&type_value)
        } else {
            Some(PathFactory::CrSpline.name().as_bytes().to_vec())
        };
        // An unregistered name (or a null one, which the component manager
        // answers with itself) gives no object with `IPersistString`
        // (divergence D244).
        let Some(factory) = desc.path_type.as_deref().and_then(PathFactory::from_name) else {
            return desc;
        };
        let init = if explicit {
            let data = self.field(table, "path_data");
            if data.is_nil() {
                PathInit::Empty
            } else {
                PathInit::Text(string(&data))
            }
        } else {
            let points = self.field(table, "control_pts");
            if points.is_nil() {
                PathInit::Empty
            } else {
                let mut count = 0;
                while !self.index(&points, count + 1).is_nil() {
                    count += 1;
                }
                PathInit::Points(
                    (1..=count)
                        .map(|i| self.vector(&self.index(&points, i)))
                        .collect(),
                )
            }
        };
        desc.path = Some(PathObject { factory, init });
        desc
    }

    /// `LoadLightProps`: the light keys of `light` into `props`; returns the
    /// mask of those present. A present key counts even when its value is
    /// not a number.
    fn light_props(&self, light: &Value, props: &mut LightProps) -> i32 {
        let mut mask = 0;
        let v = self.field(light, "on");
        if !v.is_nil() {
            props.on = chop(&v);
            mask |= light_bit::ON;
        }
        let v = self.field(light, "infinite");
        if !v.is_nil() {
            props.infinite = chop(&v);
            mask |= light_bit::INFINITE;
        }
        let v = self.field(light, "color");
        if !v.is_nil() {
            props.color = self.vector::<3>(&v).map(chop_float);
            mask |= light_bit::COLOR;
        }
        for (key, bit, slot) in [
            ("diffuse", light_bit::DIFFUSE, &mut props.diffuse),
            ("specular", light_bit::SPECULAR, &mut props.specular),
            ("ambient", light_bit::AMBIENT, &mut props.ambient),
        ] {
            let v = self.field(light, key);
            if !v.is_nil() {
                let [r, g, b] = self.vector(&v);
                *slot = [r, g, b, 1.0];
                mask |= bit;
            }
        }
        let v = self.field(light, "direction");
        if !v.is_nil() {
            props.direction = self.vector(&v);
            mask |= light_bit::DIRECTION;
        }
        let v = self.field(light, "atten");
        if !v.is_nil() {
            props.atten = self.vector(&v);
            mask |= light_bit::ATTEN;
        }
        let v = self.field(light, "type");
        if !v.is_nil() {
            props.light_type = chop(&v);
            mask |= light_bit::TYPE;
        }
        let v = self.field(light, "range");
        if !v.is_nil() {
            props.range_temp = float(&v);
            props.range = props.range_temp;
            mask |= light_bit::RANGE;
        }
        let v = self.field(light, "cutoff");
        if !v.is_nil() {
            props.cutoff_temp = float(&v);
            props.cutoff = radians(props.cutoff_temp);
            mask |= light_bit::CUTOFF;
        }
        let v = self.field(light, "theta");
        if !v.is_nil() {
            props.theta = radians(float(&v));
            mask |= light_bit::THETA;
        }
        mask
    }

    // --- Events (`ForeachEvent`) ----------------------------------------

    fn event(&mut self, table: &Value) -> Result<(), BuildError> {
        if table.as_table().is_none() {
            return Ok(());
        }
        let mut kind = chop(&self.index(table, 2));
        let mut common = EventCommon {
            kind: 0,
            start_ms: millis(&self.index(table, 1)),
            flags: 0,
            event_flags: 0,
            duration_ms: 0,
            primary: -1,
            secondary: -1,
            param_curve: None,
            pcurve_period: -1,
        };
        if kind == 0 {
            return Ok(());
        }
        let names = self.index(table, 3);
        if names.as_table().is_none() {
            return Ok(());
        }
        let mut primary = None;
        let name = self.index(&names, 1);
        if !name.is_nil() {
            let name = string(&name).ok_or(BuildError::NullString("event entity name"))?;
            if let Some(handle) = self.state.handle(&name) {
                common.primary = handle;
                primary = self.entity_desc(handle).cloned();
            }
        }
        let mut secondary = None;
        let name = self.index(&names, 2);
        if !name.is_nil() {
            let name = string(&name).ok_or(BuildError::NullString("event entity name"))?;
            // A name not found only warns; the event is still built.
            if let Some(handle) = self.state.handle(&name) {
                common.secondary = handle;
                secondary = self.entity_desc(handle).cloned();
            }
        }
        let props = self.event_common(table, &mut common)?;
        if kind == event_kind::PATH_ANIM
            && secondary
                .as_ref()
                .is_some_and(|e| (1..=entity_kind::HARDPOINT).contains(&e.kind))
        {
            kind = event_kind::ATTACH_ENTITY;
        } else if kind == event_kind::START {
            let entity = primary
                .as_ref()
                .ok_or(BuildError::NoEntity("START event"))?;
            kind = if entity.kind == entity_kind::SOUND {
                event_kind::START_SOUND
            } else {
                event_kind::START_MOTION
            };
        }
        if kind == event_kind::START_SOUND {
            if common.flags & flag::POSITION != 0 {
                let mut desc = primary
                    .clone()
                    .ok_or(BuildError::NoEntity("positioned START_SOUND event"))?;
                desc.pos[0] = f32::from_bits(desc.flags.cast_unsigned() | 2);
                self.steps.push(Step::UpdateEntity {
                    entity: common.primary,
                    desc,
                });
            }
            common.flags &= !(flag::POSITION | flag::ORIENTATION);
        }
        if kind == event_kind::START_IK && common.start_ms == 0 {
            common.start_ms = 1;
        }
        common.kind = kind;
        let Some(payload) = self.payload(kind, &props, &mut common, primary.as_ref())? else {
            return Ok(());
        };
        self.steps
            .push(Step::AddEvent(EventDesc { common, payload }));
        Ok(())
    }

    /// `ParseEventCommon`: the fourth item, the event's properties.
    fn event_common(&self, table: &Value, common: &mut EventCommon) -> Result<Value, BuildError> {
        let props = self.index(table, 4);
        if props.as_table().is_none() {
            return Ok(props);
        }
        let v = self.field(&props, "duration");
        if v.is_nil() {
            common.flags |= flag::USE_SCRIPT_DURATION;
        } else {
            common.duration_ms = millis(&v);
        }
        let v = self.field(&props, "flags");
        if !v.is_nil() {
            common.flags = chop(&v);
        }
        let v = self.field(&props, "event_flags");
        if !v.is_nil() {
            common.event_flags = chop(&v);
        }
        let v = self.field(&props, "param_curve");
        if v.as_table().is_some() {
            common.param_curve = self.param_curve(&v);
        }
        let v = self.field(&props, "pcurve_period");
        if !v.is_nil() {
            common.pcurve_period = chop(&v);
        }
        Ok(props)
    }

    /// `CreateCurveFromLua`.
    fn param_curve(&self, table: &Value) -> Option<ParamCurve> {
        let class = self.field(table, "CLSID");
        let points = self.field(table, "points");
        if class.is_nil() || points.is_nil() {
            return None;
        }
        // An unregistered or null name creates no curve (the manager
        // answers a null name with itself, which is no curve either;
        // divergence D244). Every key read stays on the reference's Lua
        // stack, which overflows for a long list (divergence D243).
        let name = string(&class)?;
        let class = *CURVE_CLASSES.iter().find(|c| c.as_bytes() == name)?;
        let mut keys = Vec::new();
        let mut at = 1;
        loop {
            let row = self.index(&points, at);
            if row.is_nil() {
                break;
            }
            let item = |i| self.index(&row, i);
            // Only the first item is tested for nil.
            if !item(1).is_nil() {
                keys.push([
                    float(&item(1)),
                    float(&item(2)),
                    float(&item(3)),
                    float(&item(4)),
                ]);
            }
            at += 1;
        }
        Some(ParamCurve { class, keys })
    }

    /// A string the event copies into its own storage
    /// (`FSceneScriptConnectString::Assign`), which reads it as a C string.
    fn assigned(v: &Value, what: &'static str) -> Result<Vec<u8>, BuildError> {
        string(v).ok_or(BuildError::NullString(what))
    }

    /// A float key of the properties, kept when absent
    /// (`ReadbaselineFloatField`).
    fn read_float(&self, props: &Value, key: &str, out: &mut f32) {
        let v = self.field(props, key);
        if !v.is_nil() {
            *out = float(&v);
        }
    }

    /// An integer key of the properties, kept when absent
    /// (`ReadbaselineIntField`).
    fn read_int(&self, props: &Value, key: &str, out: &mut i32) {
        let v = self.field(props, key);
        if !v.is_nil() {
            *out = chop(&v);
        }
    }

    /// The typed part of the event, as the parser's per-type function
    /// fills the temporary event's descriptor; `None` where the reference
    /// adds no event.
    #[allow(clippy::too_many_lines)]
    fn payload(
        &self,
        kind: i32,
        props: &Value,
        common: &mut EventCommon,
        primary: Option<&EntityDesc>,
    ) -> Result<Option<EventPayload>, BuildError> {
        let is_table = props.as_table().is_some();
        let payload = match kind {
            event_kind::USER_EVENT => {
                let text = self.field(props, "user_event_string");
                if !is_table || text.is_nil() || !self.com_context {
                    return Ok(None);
                }
                EventPayload::User {
                    payload_word: 0,
                    text: string(&text),
                }
            }
            event_kind::SET_CAMERA => EventPayload::SetCamera,
            event_kind::START_SOUND => {
                let mut start_time = 0;
                self.read_int(props, "start_time", &mut start_time);
                EventPayload::StartSound { start_time }
            }
            event_kind::LIGHT_PROP_ANIM => {
                let mut target = LightProps::default();
                let mut mask = 0;
                let light = self.field(props, "lightprops");
                if light.as_table().is_some() {
                    mask = self.light_props(&light, &mut target);
                    light_event_post(&mut target, &mut mask);
                }
                EventPayload::Light { mask, target }
            }
            event_kind::CAMERA_PROP_ANIM => {
                let mut mask = 0;
                let mut target = [0.0; 4];
                let camera = self.field(props, "cameraprops");
                if camera.as_table().is_some() {
                    for (i, key) in ["fovh", "aspect", "near", "far"].into_iter().enumerate() {
                        let v = self.field(&camera, key);
                        if !v.is_nil() {
                            mask |= 1 << i;
                            target[i] = float(&v);
                        }
                    }
                }
                EventPayload::Camera { mask, target }
            }
            event_kind::PATH_ANIM => {
                let (mut start, mut stop, mut up, mut front) = (0.0, 1.0, 1, 5);
                let mut offset = [0.0; 3];
                if is_table {
                    self.read_float(props, "start_percent", &mut start);
                    self.read_float(props, "percent1", &mut start);
                    self.read_float(props, "stop_percent", &mut stop);
                    self.read_float(props, "percent2", &mut stop);
                    self.read_int(props, "up", &mut up);
                    self.read_int(props, "front", &mut front);
                    let v = self.field(props, "offset");
                    if !v.is_nil() {
                        offset = self.vector(&v);
                    }
                }
                EventPayload::Path {
                    start_percent: start,
                    stop_percent: stop,
                    percent_delta: 1.0,
                    offset,
                    up,
                    front,
                }
            }
            event_kind::SPATIAL_PROP_ANIM => self.spatial(props, common)?,
            event_kind::ATTACH_ENTITY => {
                let mut offset = [0.0; 3];
                let (mut target_type, mut up, mut front) = (0, 1, 5);
                let mut target_part = None;
                if is_table {
                    let v = self.field(props, "offset");
                    if !v.is_nil() {
                        offset = self.vector(&v);
                    }
                    let v = self.field(props, "target_part");
                    if !v.is_nil() {
                        target_part = Some(Self::assigned(&v, "ATTACH_ENTITY target_part")?);
                    }
                    self.read_int(props, "target_type", &mut target_type);
                    self.read_int(props, "up", &mut up);
                    self.read_int(props, "front", &mut front);
                }
                EventPayload::Attach {
                    offset,
                    target_type,
                    up,
                    front,
                    target_part,
                }
            }
            event_kind::CONNECT_HARDPOINTS => {
                // Borrowed Lua strings: a null pointer passes through.
                let name = |key| {
                    let v = self.field(props, key);
                    if v.is_nil() { None } else { string(&v) }
                };
                EventPayload::Connect {
                    hardpoint: name("hardpoint"),
                    parent_hardpoint: name("parent_hardpoint"),
                }
            }
            event_kind::START_MOTION => self.motion(props, common, primary)?,
            event_kind::START_IK => self.ik(props)?,
            event_kind::START_SUB_SCENE => {
                let mut start_time = 0.0;
                if is_table {
                    self.read_float(props, "start_time", &mut start_time);
                }
                EventPayload::SubScene { start_time }
            }
            event_kind::START_PSYS => EventPayload::StartPsys,
            event_kind::PSYS_PROP_ANIM => {
                let (mut mask, mut sparam) = (0, 0.0);
                let psys = self.field(props, "psysprops");
                if psys.as_table().is_some() {
                    let v = self.field(&psys, "sparam");
                    if !v.is_nil() {
                        mask |= 1;
                        sparam = float(&v);
                    }
                }
                EventPayload::PsysProp { mask, sparam }
            }
            event_kind::AUDIO_PROP_ANIM => {
                let defaults = AudioProps {
                    dmin: 50.0,
                    dmax: 1000.0,
                    ain: 0x168,
                    aout: 0x168,
                    ..AudioProps::default()
                };
                let (target, mask) = self.audio(&self.field(props, "audioprops"), defaults);
                EventPayload::Audio { mask, target }
            }
            event_kind::FOG_PROP_ANIM => {
                let fog = self.field(props, "fogprops");
                let (target, mask) = if fog.as_table().is_some() {
                    self.fog(&fog, Fog::default())
                } else {
                    (Fog::default(), 0)
                };
                EventPayload::Fog { mask, target }
            }
            event_kind::REVERB_PROP_ANIM => {
                let reverb = self.field(props, "reverbprops");
                let mut target = Reverb::default();
                let mut mask = 0;
                if reverb.as_table().is_some() {
                    let v = self.field(&reverb, "env");
                    if !v.is_nil() {
                        target.environment = chop(&v);
                        mask |= 1;
                    }
                    for (key, bit, slot) in [
                        ("vol", 2, &mut target.volume),
                        ("decay", 4, &mut target.decay),
                        ("damping", 8, &mut target.damping),
                    ] {
                        let v = self.field(&reverb, key);
                        if !v.is_nil() {
                            *slot = float(&v);
                            mask |= bit;
                        }
                    }
                }
                EventPayload::Reverb { mask, target }
            }
            event_kind::FLOOR_HEIGHT_ANIM => {
                let mut floor_height = 0.0;
                if is_table {
                    self.read_float(props, "floor_height", &mut floor_height);
                }
                // These two are read even when the properties are no table.
                let mut target_part = None;
                let v = self.field(props, "target_part");
                if !v.is_nil() {
                    target_part = Some(Self::assigned(&v, "FLOOR_HEIGHT_ANIM target_part")?);
                }
                let mut target_type = 0;
                self.read_int(props, "target_type", &mut target_type);
                EventPayload::FloorHeight {
                    floor_height,
                    target_part,
                    target_type,
                }
            }
            event_kind::SUBTITLE => {
                let mut strid = 0;
                if is_table {
                    self.read_int(props, "strid", &mut strid);
                }
                EventPayload::Subtitle {
                    strid: strid.cast_unsigned(),
                }
            }
            _ => return Ok(None),
        };
        Ok(Some(payload))
    }

    /// `LoadSpatialPropAnim` into the packed spatial descriptor.
    fn spatial(&self, props: &Value, common: &mut EventCommon) -> Result<EventPayload, BuildError> {
        let mut mask = 0;
        let mut orient = IDENTITY;
        let mut position = [0.0; 3];
        let mut quaternion = [1.0, 0.0, 0.0, 0.0];
        let (mut axis_rot_angle, mut axis_rot_axis) = (0.0, 1);
        let mut target_part = None;
        let mut target_type = 0;
        if props.as_table().is_some() {
            // `LoadSpatialEventPose`.
            let spatial = self.field(props, "spatialprops");
            if !spatial.is_nil() {
                let v = self.field(&spatial, "axisrot");
                if v.as_table().is_some() {
                    mask |= spatial_bit::AXIS_ROT;
                    axis_rot_angle = float(&self.index(&v, 1));
                    axis_rot_axis = chop(&self.index(&v, 2));
                }
                let v = self.field(&spatial, "pos");
                if !v.is_nil() {
                    mask |= spatial_bit::POS;
                    position = self.vector(&v);
                }
                let q = self.field(&spatial, "q_orient");
                if q.is_nil() {
                    let v = self.field(&spatial, "orient");
                    if !v.is_nil() {
                        mask |= spatial_bit::ORIENT;
                        set_columns(self, &v, &mut orient);
                    }
                } else {
                    mask |= spatial_bit::ORIENT;
                    quaternion = self.vector(&q);
                    common.flags |= flag::QUAT_ORIENT;
                }
            }
            let v = self.field(props, "target_part");
            if !v.is_nil() {
                target_part = Some(Self::assigned(&v, "SPATIAL_PROP_ANIM target_part")?);
            }
            self.read_int(props, "target_type", &mut target_type);
        }
        Ok(EventPayload::Spatial {
            mask,
            orient,
            position,
            quaternion,
            axis_rot_angle,
            axis_rot_axis,
            target_part,
            target_type,
        })
    }

    /// `ParseMotionEvent`.
    fn motion(
        &self,
        props: &Value,
        common: &mut EventCommon,
        primary: Option<&EntityDesc>,
    ) -> Result<EventPayload, BuildError> {
        let (mut start_time, mut trans_time, mut time_scale, mut weight) = (0.0, 0.0, 1.0, 1.0);
        let (mut heading, mut trans_scale) = (-1.0, 1.0);
        let (mut animation, mut locked_bone) = (None, None);
        if props.as_table().is_some() {
            let v = self.field(props, "animation");
            if is_string(&v) {
                animation = string(&v);
            }
            let v = self.field(props, "locked_bone");
            if is_string(&v) {
                locked_bone = string(&v);
            }
            self.read_float(props, "start_time", &mut start_time);
            self.read_float(props, "trans_time", &mut trans_time);
            self.read_float(props, "time_scale", &mut time_scale);
            self.read_float(props, "weight", &mut weight);
            let v = self.field(props, "loop");
            if !v.is_nil() && number(&v) > 0.0 {
                common.flags |= 2;
            }
            let entity = primary.ok_or(BuildError::NoEntity("START_MOTION event"))?;
            if entity.kind == entity_kind::COMPOUND {
                self.read_float(props, "trans_scale", &mut trans_scale);
            } else {
                self.read_float(props, "heading", &mut heading);
            }
        }
        Ok(EventPayload::Motion {
            start_time,
            trans_time,
            time_scale,
            weight,
            animation,
            field_40: 0,
            heading,
            locked_bone,
            trans_scale,
            field_50: 0,
        })
    }

    /// `ParseStartIkEvent` and `LoadIkProps`.
    fn ik(&self, props: &Value) -> Result<EventPayload, BuildError> {
        let mut key = IkKey::Event;
        let (mut count_to_root, mut up, mut front, mut target_type) = (0, 1, 5, 0);
        let (mut damping, mut transition_duration) = (1.0, 1.0);
        let (mut point_at, mut move_to) = (0, 0);
        let (mut end_effector, mut target_part) = (None, None);
        if props.as_table().is_some() {
            let v = self.field(props, "end_effector");
            let end = if v.is_nil() { None } else { string(&v) };
            let v = self.field(props, "target_part");
            let part = if v.is_nil() { None } else { string(&v) };
            self.read_int(props, "target_type", &mut target_type);
            let v = self.field(props, "ik_id");
            if !v.is_nil() {
                key = IkKey::Id(chop(&v).cast_unsigned());
            }
            self.read_int(props, "count_to_root", &mut count_to_root);
            self.read_float(props, "damping", &mut damping);
            self.read_int(props, "up", &mut up);
            self.read_int(props, "front", &mut front);
            // `IsIkLuaNumberNonzero`: NaN counts as nonzero.
            let flag = |key| {
                let v = self.field(props, key);
                (!v.is_nil()).then(|| u8::from(number(&v) != 0.0))
            };
            if let Some(b) = flag("point_at") {
                point_at = b;
            }
            if let Some(b) = flag("move_to") {
                move_to = b;
            }
            self.read_float(props, "transition_duration", &mut transition_duration);
            // Both names are copied into the event, missing or not.
            end_effector = Some(end.ok_or(BuildError::NullString("START_IK end_effector"))?);
            target_part = Some(part.ok_or(BuildError::NullString("START_IK target_part"))?);
        }
        Ok(EventPayload::Ik {
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
        })
    }
}

/// Each present row of the script's `orient` replaces a column of `out`;
/// an incomplete row gives a zero column (`ReadEntityOrientationColumn`,
/// the orientation of `LoadSpatialEventPose`).
fn set_columns(b: &Builder<'_>, orient: &Value, out: &mut [f32; 9]) {
    for column in 0..3 {
        let row = b.index(orient, column + 1);
        if row.is_nil() {
            continue;
        }
        let values: [f32; 3] = b.vector(&row);
        for (i, value) in values.into_iter().enumerate() {
            out[i * 3 + column as usize] = value;
        }
    }
}

/// `ApplyLightEventPost`: a light event's colour also sets its diffuse
/// colour, `infinite` selects a directional or point light, and a negative
/// cutoff a spot light without a cone.
fn light_event_post(props: &mut LightProps, mask: &mut i32) {
    if *mask & light_bit::COLOR != 0 {
        let [r, g, b] = props.color.map(unit_color);
        props.diffuse = [r, g, b, 1.0];
        *mask |= light_bit::DIFFUSE;
    }
    if *mask & light_bit::INFINITE != 0 {
        props.light_type = if props.infinite == 0 { 2 } else { 3 };
        *mask = (*mask & !light_bit::INFINITE) | light_bit::TYPE;
    }
    // `FCOMP` with +0: only an ordered "less" passes.
    if *mask & light_bit::CUTOFF != 0 && props.cutoff < 0.0 {
        props.light_type = 1;
        props.cutoff = 0.0;
        props.theta = 0.0;
        *mask |= light_bit::TYPE;
    }
}

/// `ConvertCompactLight` (`SceneScriptScene.cpp`, the x87 routine
/// `ConvertCompactLightbaseline`): a LIGHT entity whose script gives no
/// `type` takes its type, cone, diffuse colour, range and attenuation from
/// the compact fields.
pub(crate) fn convert_compact_light(props: &mut LightProps) {
    let pi = f64::from(PI);
    if props.infinite != 0 {
        props.light_type = 3;
    } else if props.cutoff_temp < 0.0 {
        props.light_type = 1;
        props.theta = PI;
        props.cutoff = PI;
    } else {
        props.light_type = 2;
        // The product stays on the 53-bit stack through both compares; NaN
        // fails "less" and takes π.
        let mut theta = f64::from(props.cutoff_temp) * f64::from(DEGREE);
        if theta >= pi || theta.is_nan() {
            theta = pi;
        }
        props.theta = theta as f32;
        let mut cutoff = theta * f64::from(1.1_f32);
        if cutoff >= pi || cutoff.is_nan() {
            cutoff = pi;
        }
        props.cutoff = cutoff as f32;
    }
    let [r, g, b] = props.color.map(unit_color);
    props.diffuse = [r, g, b, 1.0];
    let range = f64::from(props.range_temp);
    if range < 0.0001 {
        // `FSQRT` of the largest float.
        props.range = f64::from(f32::MAX).sqrt() as f32;
        props.atten = [0.0, f32::from_bits(0x38d1_b717), 0.0];
    } else {
        props.range = (range + range) as f32;
        props.atten = [1.0, 0.0, (1.0 / (range * range * 0.25)) as f32];
    }
    props.specular = [0.0; 4];
    props.ambient = [0.0; 4];
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constants_have_the_reference_bits() {
        assert_eq!(DEGREE.to_bits(), 0x3c8e_fa35);
        assert_eq!(BY_255.to_bits(), 0x3b80_8081);
        assert_eq!(PI.to_bits(), 0x4049_0fdb);
        assert_eq!(1.1_f32.to_bits(), 0x3f8c_cccd);
        assert_eq!(f64::from(PI).to_bits(), 0x4009_21fb_6000_0000);
        assert_eq!(0.0001_f64.to_bits(), 0x3f1a_36e2_eb1c_432d);
        assert_eq!(f64::from(f32::MAX).to_bits(), 0x47ef_ffff_e000_0000);
    }

    #[test]
    fn compact_lights_convert_like_the_x87_routine() {
        let mut p = LightProps::entity_defaults();
        convert_compact_light(&mut p);
        assert_eq!(p.light_type, 2);
        // 180 degrees exceed π as a double product: both angles clamp.
        assert_eq!(p.theta, PI);
        assert_eq!(p.cutoff, PI);
        assert_eq!(p.diffuse, [1.0; 4]);
        assert_eq!(p.range, 2000.0);
        assert_eq!(p.atten, [1.0, 0.0, 1.0 / 250_000.0]);

        let mut p = LightProps {
            cutoff_temp: -1.0,
            range_temp: 0.0,
            ..LightProps::default()
        };
        convert_compact_light(&mut p);
        assert_eq!((p.light_type, p.theta, p.cutoff), (1, PI, PI));
        assert_eq!(p.range.to_bits(), 0x5f7f_ffff);
        assert_eq!(p.atten[1], 0.0001);

        let mut p = LightProps {
            infinite: 1,
            theta: 5.0,
            ..LightProps::default()
        };
        convert_compact_light(&mut p);
        assert_eq!((p.light_type, p.theta), (3, 5.0));
    }

    #[test]
    fn light_event_post_selects_the_type() {
        let mut p = LightProps {
            color: [255, 0, 128],
            infinite: 1,
            cutoff: -0.5,
            ..LightProps::default()
        };
        let mut mask = light_bit::COLOR | light_bit::INFINITE | light_bit::CUTOFF;
        light_event_post(&mut p, &mut mask);
        assert_eq!(p.diffuse[0], 1.0);
        assert_eq!(p.diffuse[2].to_bits(), 0x3f00_8081);
        assert_eq!((p.light_type, p.cutoff), (1, 0.0));
        assert_eq!(
            mask,
            light_bit::COLOR | light_bit::DIFFUSE | light_bit::CUTOFF | light_bit::TYPE
        );
    }
}
