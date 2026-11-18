//! Renderer traces (record 0004, section 2): the reader of the JSONL lines
//! `RenderTrace.dll` writes, and model replay, which applies each recorded
//! call to a [`Device`] and compares every value the C++ handed back to its
//! caller with the model's.
//!
//! The reader takes format version 1 and rejects any other (record 0004,
//! point 11). Version 1 adds the defaults line after each `CreateBuffers`,
//! a reference image per selected frame and the synthetic `LightData`
//! lines, which give the light data the C++ read at a light flush. Buffer
//! data goes where the C++ put it: unlock data at the recorded first vertex
//! or index, copies at the recorded output offset, since the C++ ring
//! cursors advance in frames the trace skips.

pub mod json;

use std::collections::{BTreeMap, HashMap};
use std::fmt;

use neuron_core::image::PixelFormat;

use crate::render::ff::buffers::{IbHandle, IndexSource, VbHandle};
use crate::render::ff::defaults::Defaults;
use crate::render::ff::device::{Device, RecordingBackend};
use crate::render::ff::fvf::Fvf;
use crate::render::ff::lights::{LightData, Material};
use crate::render::ff::state::{PIPELINE_SLOTS, PipelineProperty, RenderStateId};
use crate::render::ff::textures::TextureHandle;
use crate::render::ff::vertex_copy::{Source, Stream};
use json::Json;

/// The C++ result codes replay compares with.
const GR_OK: i64 = 0;
const GR_GENERIC: i64 = -1;
const E_FAIL: i64 = 0x8000_4005_u32 as i32 as i64;

/// The trace format version this reader takes.
pub const FORMAT_VERSION: i64 = 1;

/// A line of a trace.
#[derive(Clone, Debug, PartialEq)]
pub enum Line {
    Header {
        version: i64,
    },
    Call(Call),
    BeginFrame {
        frame: i64,
        snapshot_lines: i64,
    },
    EndFrame {
        frame: i64,
    },
    /// The renderer's defaults after a `CreateBuffers`, as its getters read
    /// them: render states by Direct3D 8 number and pipeline properties by
    /// id, in the order of the line. Also a snapshot line.
    Defaults(DefaultsLine),
    /// The reference image of a selected frame: the blob of a BMP file, or
    /// `None` with the proxy's note when it could not take one.
    FrameImage {
        frame: i64,
        image: Option<String>,
        width: i64,
        height: i64,
        note: Option<String>,
    },
    End,
}

/// A defaults line (record 0004, point 11, as amended on 2026-11-18).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DefaultsLine {
    /// (Direct3D 8 number, value) for each render state of the table.
    pub render: Vec<(u32, u32)>,
    /// (id, value) for pipeline ids 0 to 14.
    pub pipeline: Vec<(i32, i32)>,
    pub snapshot: bool,
}

/// A recorded call.
#[derive(Clone, Debug, PartialEq)]
pub struct Call {
    pub seq: i64,
    pub frame: i64,
    pub interface: String,
    pub method: String,
    pub args: Json,
    pub ret: i64,
    pub snapshot: bool,
    pub synthetic: bool,
}

/// Why a trace could not be read or replayed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TraceError {
    Json(json::JsonError),
    /// A line that is no header, call or marker.
    UnknownLine(String),
    /// A method the replay does not know (record 0004, point 13).
    UnknownMethod(String),
    /// A missing or malformed argument.
    BadArgument {
        method: String,
        arg: &'static str,
    },
    /// A blob the source does not have.
    MissingBlob(String),
    /// A header of a format version other than [`FORMAT_VERSION`].
    UnsupportedVersion(i64),
}

impl fmt::Display for TraceError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Json(e) => write!(f, "bad trace line: {e}"),
            Self::UnknownLine(l) => write!(f, "unknown trace line: {l}"),
            Self::UnknownMethod(m) => write!(f, "unknown method {m}"),
            Self::BadArgument { method, arg } => write!(f, "{method}: bad argument {arg}"),
            Self::MissingBlob(h) => write!(f, "missing blob {h}"),
            Self::UnsupportedVersion(v) => write!(
                f,
                "trace format version {v}; this reader takes version {FORMAT_VERSION}"
            ),
        }
    }
}

impl std::error::Error for TraceError {}

/// The object of a defaults line: `{"<number>":<value>,...}`.
fn number_map(v: Option<&Json>, arg: &'static str) -> Result<Vec<(i64, i64)>, TraceError> {
    let bad = || TraceError::BadArgument {
        method: "defaults".to_owned(),
        arg,
    };
    match v {
        None => Ok(Vec::new()),
        Some(Json::Object(items)) => items
            .iter()
            .map(|(k, value)| Some((k.parse::<i64>().ok()?, value.as_i64()?)))
            .collect::<Option<Vec<_>>>()
            .ok_or_else(bad),
        Some(_) => Err(bad()),
    }
}

/// Reads one line. A header of another format version is an error.
pub fn parse_line(text: &str) -> Result<Line, TraceError> {
    let v = json::parse(text).map_err(TraceError::Json)?;
    let int = |k: &str| v.get(k).and_then(Json::as_i64);
    if v.get("format").is_some() {
        let version = int("version").unwrap_or(0);
        if version != FORMAT_VERSION {
            return Err(TraceError::UnsupportedVersion(version));
        }
        return Ok(Line::Header { version });
    }
    if let Some(d) = v.get("defaults") {
        let render = number_map(d.get("render"), "render")?
            .into_iter()
            .map(|(k, value)| (k as u32, value as u32))
            .collect();
        let pipeline = number_map(d.get("pipeline"), "pipeline")?
            .into_iter()
            .map(|(k, value)| (k as i32, value as i32))
            .collect();
        return Ok(Line::Defaults(DefaultsLine {
            render,
            pipeline,
            snapshot: v.get("snapshot").is_some(),
        }));
    }
    if v.get("begin_frame").is_some() {
        return Ok(Line::BeginFrame {
            frame: int("frame").unwrap_or(0),
            snapshot_lines: int("snapshot_lines").unwrap_or(0),
        });
    }
    if v.get("end_frame").is_some() {
        return Ok(Line::EndFrame {
            frame: int("frame").unwrap_or(0),
        });
    }
    if let Some(image) = v.get("frame_image") {
        return Ok(Line::FrameImage {
            frame: int("frame").unwrap_or(0),
            image: image.as_str().map(str::to_owned),
            width: int("width").unwrap_or(0),
            height: int("height").unwrap_or(0),
            note: v.get("note").and_then(Json::as_str).map(str::to_owned),
        });
    }
    if v.get("end").is_some() {
        return Ok(Line::End);
    }
    let Some(method) = v.get("method").and_then(Json::as_str) else {
        return Err(TraceError::UnknownLine(text.chars().take(80).collect()));
    };
    Ok(Line::Call(Call {
        seq: int("seq").unwrap_or(0),
        frame: int("frame").unwrap_or(0),
        interface: v
            .get("interface")
            .and_then(Json::as_str)
            .unwrap_or_default()
            .to_owned(),
        method: method.to_owned(),
        args: v.get("args").cloned().unwrap_or(Json::Object(Vec::new())),
        ret: int("ret").unwrap_or(0),
        snapshot: v.get("snapshot").is_some(),
        synthetic: v.get("synthetic").is_some(),
    }))
}

/// Where blob bytes come from.
pub trait BlobSource {
    fn blob(&self, hash: &str) -> Option<Vec<u8>>;
}

/// No blobs: every byte value must be inline.
pub struct NoBlobs;

impl BlobSource for NoBlobs {
    fn blob(&self, _hash: &str) -> Option<Vec<u8>> {
        None
    }
}

/// A value the model handed back that differs from the C++'s.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Mismatch {
    pub seq: i64,
    pub method: String,
    pub what: String,
    pub want: String,
    pub got: String,
}

/// What a trace id names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Object {
    Texture(TextureHandle),
    Vb(VbHandle),
    Ib(IbHandle),
}

/// Methods that do not change the model; their recorded results stand.
const ANSWERED: &[&str] = &[
    "Initialize",
    "Startup",
    "ShutdownPipeline",
    "GetDeviceInfo",
    "QueryDeviceAbility",
    "GetDisplayModeCount",
    "GetDisplayModeEnum",
    "SelectMode",
    "GetModeBlob",
    "DestroyGpuResources",
    "LockBuffer",
    "UnlockBuffer",
    "GetBufferInterface",
    "GetDeviceStats",
    "SetWindow",
    "GetWindow",
    "IsTexture",
    "GetTextureFormat",
    "GetTextureDim",
    "GetTextureInterface",
    "BlitTexture",
    "SetRenderTarget",
    "GetRenderTarget",
    "VerifyState",
    "VbMgrInit",
    "UnsupportedManagerOperation",
    "UnsupportedVertexBufferOperation",
    "GetVbInfo",
    "IsVbValid",
    "GetIbCapacity",
    "IsIbValid",
    "GetLight",
    "SetLookat",
    "BeginScene",
    "EndScene",
    "SetGammaFunction",
    "SetGammaRamp",
    "GetGammaRamp",
    "SetGammaEnable",
    "GetGammaEnable",
    "PrintScreen",
    "LoadTexture",
    "LoadSurfaceData",
    "GetImageInfo",
    "LoadCubemap",
    "CopyVertexData",
];

/// A stream's bytes, element size, stride, indices and count.
type StreamParts = (Vec<u8>, u32, u32, Vec<u32>, u32);

/// A resolved vertex source of a trace (`SrcData`): each stream's bytes
/// placed at their offsets from element 0, and its indices from vertex 0.
#[derive(Default)]
struct OwnedSource {
    flags: u32,
    fvf: u32,
    count: u32,
    /// Position, normal, diffuse, tex 0, tex 1: bytes, element size,
    /// stride, indices, count.
    streams: [StreamParts; 5],
}

impl OwnedSource {
    fn source(&self) -> Source<'_> {
        let s = |i: usize| {
            let (data, byte_size, stride, index, count) = &self.streams[i];
            Stream {
                data,
                byte_size: *byte_size,
                stride: *stride,
                index,
                count: *count,
            }
        };
        Source {
            flags: self.flags,
            position: s(0),
            normal: s(1),
            diffuse: s(2),
            tex: [s(3), s(4)],
            fvf: self.fvf,
            count: self.count,
        }
    }
}

/// Model replay of a trace.
pub struct ModelReplay<'a> {
    pub device: Device<RecordingBackend>,
    ids: HashMap<i64, Object>,
    blobs: &'a dyn BlobSource,
    pub mismatches: Vec<Mismatch>,
    /// Calls whose effect is not modelled yet, by method.
    pub answered: BTreeMap<String, u32>,
    pub calls: u64,
}

fn hex_word(v: &Json) -> Option<u32> {
    u32::from_str_radix(v.as_str()?, 16).ok()
}

fn words(v: &Json) -> Option<Vec<u32>> {
    v.as_array()?.iter().map(hex_word).collect()
}

fn floats<const N: usize>(v: &Json) -> Option<[f32; N]> {
    let w = words(v)?;
    (w.len() == N).then(|| std::array::from_fn(|i| f32::from_bits(w[i])))
}

impl<'a> ModelReplay<'a> {
    #[must_use]
    pub fn new(defaults: Defaults, width: u32, height: u32, blobs: &'a dyn BlobSource) -> Self {
        ModelReplay {
            device: Device::new(defaults, width, height, RecordingBackend::default()),
            ids: HashMap::new(),
            blobs,
            mismatches: Vec::new(),
            answered: BTreeMap::new(),
            calls: 0,
        }
    }

    fn arg<'c>(call: &'c Call, name: &'static str) -> Result<&'c Json, TraceError> {
        call.args.get(name).ok_or(TraceError::BadArgument {
            method: call.method.clone(),
            arg: name,
        })
    }

    fn int(call: &Call, name: &'static str) -> Result<i64, TraceError> {
        Self::arg(call, name)?
            .as_i64()
            .ok_or(TraceError::BadArgument {
                method: call.method.clone(),
                arg: name,
            })
    }

    fn u(call: &Call, name: &'static str) -> Result<u32, TraceError> {
        Ok(Self::int(call, name)? as u32)
    }

    fn float(call: &Call, name: &'static str) -> Result<f32, TraceError> {
        hex_word(Self::arg(call, name)?)
            .map(f32::from_bits)
            .ok_or(TraceError::BadArgument {
                method: call.method.clone(),
                arg: name,
            })
    }

    fn words_arg<const N: usize>(call: &Call, name: &'static str) -> Result<[u32; N], TraceError> {
        words(Self::arg(call, name)?)
            .filter(|w| w.len() == N)
            .map(|w| std::array::from_fn(|i| w[i]))
            .ok_or(TraceError::BadArgument {
                method: call.method.clone(),
                arg: name,
            })
    }

    fn floats_arg<const N: usize>(call: &Call, name: &'static str) -> Result<[f32; N], TraceError> {
        floats::<N>(Self::arg(call, name)?).ok_or(TraceError::BadArgument {
            method: call.method.clone(),
            arg: name,
        })
    }

    /// Byte data: `{"size":N,"hex":..}` or `{"size":N,"blob":..}`.
    fn data(&self, call: &Call, name: &'static str) -> Result<Vec<u8>, TraceError> {
        let v = Self::arg(call, name)?;
        if let Some(hex) = v.get("hex").and_then(Json::as_str) {
            return (0..hex.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(hex.get(i..i + 2).unwrap_or("zz"), 16).ok())
                .collect::<Option<Vec<u8>>>()
                .ok_or(TraceError::BadArgument {
                    method: call.method.clone(),
                    arg: name,
                });
        }
        if let Some(hash) = v.get("blob").and_then(Json::as_str) {
            return self
                .blobs
                .blob(hash)
                .ok_or_else(|| TraceError::MissingBlob(hash.to_owned()));
        }
        Err(TraceError::BadArgument {
            method: call.method.clone(),
            arg: name,
        })
    }

    fn indices(&self, call: &Call, name: &'static str) -> Result<Vec<u16>, TraceError> {
        let bytes = self.data(call, name)?;
        Ok(bytes
            .as_chunks::<2>()
            .0
            .iter()
            .map(|c| u16::from_le_bytes(*c))
            .collect())
    }

    fn compare<T: fmt::Debug + PartialEq>(&mut self, call: &Call, what: &str, want: T, got: T) {
        if want != got {
            self.mismatches.push(Mismatch {
                seq: call.seq,
                method: call.method.clone(),
                what: what.to_owned(),
                want: format!("{want:?}"),
                got: format!("{got:?}"),
            });
        }
    }

    fn compare_ret(&mut self, call: &Call, got: i64) {
        self.compare(call, "ret", call.ret, got);
    }

    fn texture(
        &self,
        call: &Call,
        name: &'static str,
    ) -> Result<Option<TextureHandle>, TraceError> {
        match Self::int(call, name)? {
            0 => Ok(None),
            id => match self.ids.get(&id) {
                Some(Object::Texture(t)) => Ok(Some(*t)),
                _ => Err(TraceError::BadArgument {
                    method: call.method.clone(),
                    arg: name,
                }),
            },
        }
    }

    /// A vertex buffer argument: `None` for -1 (the selected one).
    fn vb(&mut self, call: &Call, name: &'static str) -> Result<Option<VbHandle>, TraceError> {
        let v = Self::arg(call, name)?;
        if let Some(fvf) = v.get("pool_fvf").and_then(Json::as_i64) {
            return Ok(Some(self.device.buffers.pool_vb(Fvf(fvf as u32), 0)));
        }
        match v.as_i64() {
            Some(-1) => Ok(None),
            Some(id) => match self.ids.get(&id) {
                Some(Object::Vb(h)) => Ok(Some(*h)),
                _ => Err(TraceError::BadArgument {
                    method: call.method.clone(),
                    arg: name,
                }),
            },
            None => Err(TraceError::BadArgument {
                method: call.method.clone(),
                arg: name,
            }),
        }
    }

    fn ib(&self, call: &Call, name: &'static str) -> Result<Option<IndexSource>, TraceError> {
        match Self::int(call, name)? {
            0 => Ok(None),
            -1 => Ok(Some(IndexSource::Scratch)),
            id => match self.ids.get(&id) {
                Some(Object::Ib(h)) => Ok(Some(IndexSource::Buffer(*h))),
                _ => Err(TraceError::BadArgument {
                    method: call.method.clone(),
                    arg: name,
                }),
            },
        }
    }

    fn bytes_of(&self, v: &Json, method: &str) -> Result<Vec<u8>, TraceError> {
        let bad = || TraceError::BadArgument {
            method: method.to_owned(),
            arg: "SrcData",
        };
        if let Some(hex) = v.get("hex").and_then(Json::as_str) {
            return (0..hex.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(hex.get(i..i + 2).unwrap_or("zz"), 16).ok())
                .collect::<Option<Vec<u8>>>()
                .ok_or_else(bad);
        }
        if let Some(hash) = v.get("blob").and_then(Json::as_str) {
            return self
                .blobs
                .blob(hash)
                .ok_or_else(|| TraceError::MissingBlob(hash.to_owned()));
        }
        Err(bad())
    }

    /// `SrcData` of `CopyVertices` as an owned source.
    fn vertex_source(&self, call: &Call, start: u32) -> Result<OwnedSource, TraceError> {
        let d = Self::arg(call, "SrcData")?;
        let int = |v: &Json, k: &str| v.get(k).and_then(Json::as_i64).unwrap_or(0) as u32;
        let mut out = OwnedSource {
            flags: int(d, "Flags"),
            fvf: int(d, "SrcFvf"),
            count: int(d, "SrcCount"),
            ..OwnedSource::default()
        };
        if let Some(i) = d.get("Interleaved") {
            let stride = int(i, "Stride");
            let mut data = vec![0; (int(i, "first") * stride) as usize];
            data.extend(self.bytes_of(i.get("data").unwrap_or(&Json::Null), &call.method)?);
            out.streams[0] = (data, int(i, "VertexBytes"), stride, Vec::new(), out.count);
            return Ok(out);
        }
        for (slot, name) in ["Position", "Normal", "Diffuse", "Tex0", "Tex1"]
            .into_iter()
            .enumerate()
        {
            let Some(st) = d.get(name) else {
                continue;
            };
            let stride = int(st, "Stride");
            let mut data = vec![0; (int(st, "first") * stride) as usize];
            data.extend(self.bytes_of(st.get("data").unwrap_or(&Json::Null), &call.method)?);
            let mut index = vec![0u32; start as usize];
            match st.get("index") {
                Some(Json::Array(items)) => {
                    index.extend(items.iter().map(|v| v.as_i64().unwrap_or(0) as u32))
                }
                Some(v @ Json::Object(_)) => {
                    let b = self.bytes_of(v, &call.method)?;
                    index.extend(b.as_chunks::<4>().0.iter().map(|c| u32::from_le_bytes(*c)));
                }
                _ => {}
            }
            out.streams[slot] = (data, int(st, "ByteSize"), stride, index, int(st, "Count"));
        }
        Ok(out)
    }

    fn viewspace(&self) -> bool {
        self.device
            .state
            .pipeline(PipelineProperty::ViewspaceLights)
            != 0
    }

    /// The tail of `CreateBuffers` the model has: the default render and
    /// stage states, the identity transforms, the lights sent again.
    fn create_buffers(&mut self) {
        self.device.reset_render_states();
        let v = self.viewspace();
        self.device.transforms.apply_identity(v);
        self.device.lights.reset();
    }

    /// A defaults line: the render-state defaults are replaced, the values
    /// of the pipeline slots the built-in table enables are set (the
    /// others stay as `SetPipelineState` leaves them, unchanged), and the
    /// tail of `CreateBuffers` runs again. Right after `CreateBuffers` that
    /// tail gives the same state a second time, with the new defaults; in a
    /// snapshot the line stands where `CreateBuffers` stood among the kept
    /// lines (the proxy drops every other slot but the lights' there).
    pub fn apply_defaults(&mut self, line: &DefaultsLine) -> Result<(), TraceError> {
        for &(number, value) in &line.render {
            let id = RenderStateId::from_raw(number).ok_or(TraceError::BadArgument {
                method: "defaults".to_owned(),
                arg: "render",
            })?;
            self.device.state.set_render_default(id, value);
        }
        for &(id, value) in &line.pipeline {
            if usize::try_from(id).is_ok_and(|i| i < PIPELINE_SLOTS) {
                self.device.state.set_pipeline_raw(id, value);
            }
        }
        self.create_buffers();
        Ok(())
    }

    /// Applies one call.
    pub fn apply(&mut self, call: &Call) -> Result<(), TraceError> {
        self.calls += 1;
        let m = call.method.as_str();
        match m {
            "SetRenderState" => {
                let r = self
                    .device
                    .state
                    .set_render_state_raw(Self::u(call, "State")?, Self::u(call, "Value")?);
                self.compare_ret(call, if r.is_ok() { GR_OK } else { GR_GENERIC });
            }
            "GetRenderState" => {
                let got = self
                    .device
                    .state
                    .render_state_raw(Self::u(call, "State")?)
                    .ok();
                let want = Self::arg(call, "Out")?.as_i64().map(|v| v as u32);
                self.compare(call, "Out", want, got);
            }
            "SetTextureStageState" => {
                let r = self.device.state.set_stage_state_raw(
                    Self::u(call, "Stage")?,
                    Self::u(call, "Type")?,
                    Self::u(call, "Value")?,
                );
                self.compare_ret(call, if r.is_ok() { GR_OK } else { GR_GENERIC });
            }
            "GetTextureStageState" => {
                let got = self
                    .device
                    .state
                    .stage_state_raw(Self::u(call, "Stage")?, Self::u(call, "Type")?)
                    .ok();
                let want = Self::arg(call, "Out")?.as_i64().map(|v| v as u32);
                self.compare(call, "Out", want, got);
            }
            "ResetRenderStatesToDefaults" => self.device.reset_render_states(),
            "CreateBuffers" => self.create_buffers(),
            "SetPipelineState" => self.device.state.set_pipeline_raw(
                Self::int(call, "Id")? as i32,
                Self::int(call, "Value")? as i32,
            ),
            "GetPipelineState" => {
                let got = self
                    .device
                    .state
                    .pipeline_raw(Self::int(call, "Id")? as i32);
                let want = Self::arg(call, "Out")?.as_i64().map(|v| v as u32);
                if got.is_some() {
                    self.compare(call, "Out", want, got);
                }
            }
            "SetWorld" => {
                let w = Self::floats_arg::<12>(call, "M12")?;
                self.device.transforms.set_world(&w);
            }
            "GetWorld" => {
                let got = self.device.transforms.world().map(f32::to_bits);
                let want = Self::words_arg::<12>(call, "Out12")?;
                self.compare(call, "Out12", want, got);
            }
            "SetView" => {
                let w = Self::floats_arg::<12>(call, "M12")?;
                self.device.transforms.set_view(&w);
            }
            "GetView" => {
                let got = self.device.transforms.view().map(f32::to_bits);
                let want = Self::words_arg::<12>(call, "Out12")?;
                self.compare(call, "Out12", want, got);
            }
            "SetModelview" => {
                let w = Self::floats_arg::<12>(call, "M12")?;
                self.device.set_modelview(&w);
            }
            "GetModelview" => {
                let got = self.device.modelview().map(f32::to_bits);
                let want = Self::words_arg::<12>(call, "Out12")?;
                self.compare(call, "Out12", want, got);
            }
            "SetProjection" => {
                let w = Self::floats_arg::<16>(call, "M16")?;
                self.device.transforms.set_projection(&w);
            }
            "GetProjection" => {
                let got = self.device.transforms.projection().map(f32::to_bits);
                let want = Self::words_arg::<16>(call, "Out16")?;
                self.compare(call, "Out16", want, got);
            }
            "SetOrtho" => self.device.transforms.set_ortho(
                Self::float(call, "Left")?,
                Self::float(call, "Right")?,
                Self::float(call, "Bottom")?,
                Self::float(call, "Top")?,
                Self::float(call, "NearPlane")?,
                Self::float(call, "FarPlane")?,
            ),
            "SetPerspective" => {
                let r = self.device.transforms.set_perspective(
                    Self::float(call, "FovDeg")?,
                    Self::float(call, "Aspect")?,
                    Self::float(call, "NearPlane")?,
                    Self::float(call, "FarPlane")?,
                );
                self.compare_ret(call, if r.is_ok() { GR_OK } else { GR_GENERIC });
            }
            "SetWorldN" => {
                let w = Self::floats_arg::<12>(call, "M12")?;
                self.device
                    .transforms
                    .set_world_n(Self::u(call, "WorldIndex")?, &w);
            }
            "SetTextureStageTransform" => {
                let w = Self::floats_arg::<16>(call, "Matrix16")?;
                let stage = Self::u(call, "Stage")? as usize;
                if stage < crate::render::ff::state::STAGES {
                    self.device.transforms.set_texture(stage, &w);
                }
            }
            "GetTextureStageTransform" => {
                let stage = Self::u(call, "Stage")? as usize;
                if stage < crate::render::ff::state::STAGES {
                    let got = self.device.transforms.texture(stage).map(f32::to_bits);
                    let want = Self::words_arg::<16>(call, "OutMatrix16")?;
                    self.compare(call, "OutMatrix16", want, got);
                }
            }
            "SetMaterial" => {
                let w = Self::words_arg::<17>(call, "Material")?;
                self.device.lights.set_material(Material(w));
            }
            "GetMaterial" => {
                let got = self.device.lights.material().0;
                let want = Self::words_arg::<17>(call, "Material")?;
                self.compare(call, "Material", want, got);
            }
            "SetLight" => {
                let data = LightData(Self::words_arg::<26>(call, "Light")?);
                self.device
                    .lights
                    .set_light(Self::int(call, "Handle")? as u64, data);
            }
            "AddLight" => {
                let data = LightData(Self::words_arg::<26>(call, "Light")?);
                self.device
                    .lights
                    .add_light(Self::int(call, "Handle")? as u64, data);
            }
            "UpdateLight" => {
                let data = LightData(Self::words_arg::<26>(call, "Light")?);
                self.device
                    .lights
                    .update_light(Self::int(call, "Handle")? as u64, data);
            }
            // Synthetic: the D3DLIGHT8 behind the light's handle as the C++
            // flush is about to read it. It is not `UpdateLight`, which also
            // marks the data dirty and queues the light: the renderer saw no
            // call, so only the stored bytes change.
            "LightData" => {
                let data = LightData(Self::words_arg::<26>(call, "Light")?);
                let _ = self
                    .device
                    .lights
                    .replace_data(Self::int(call, "Handle")? as u64, data);
            }
            "RemoveLight" | "DestroyLight" => {
                let _ = self
                    .device
                    .lights
                    .remove_light(Self::int(call, "Handle")? as u64);
            }
            "SetLightEnable" => self.device.lights.set_light_enable(
                Self::int(call, "Handle")? as u64,
                Self::int(call, "Enable")? != 0,
            ),
            "GetLightEnable" => {
                let got = i64::from(
                    self.device
                        .lights
                        .light_enable(Self::int(call, "Handle")? as u64),
                );
                let want = Self::int(call, "OutEnable")?;
                self.compare(call, "OutEnable", want, got);
            }
            "CreateTexture" => {
                let id = Self::int(call, "OutHandle")?;
                let r = self.device.textures.create(
                    Self::u(call, "Width")?,
                    Self::u(call, "Height")?,
                    PixelFormat(Self::u(call, "Format")?),
                    Self::u(call, "Levels")?,
                    Self::u(call, "Flags")?,
                );
                if let Ok(h) = r
                    && id > 0
                {
                    self.ids.insert(id, Object::Texture(h));
                }
            }
            "DestroyTexture" => {
                if let Some(t) = self.texture(call, "Handle")? {
                    let _ = self.device.destroy_texture(t);
                }
            }
            "LockTexture" => {}
            "UnlockTexture" => {
                if let (Some(t), Ok(data)) =
                    (self.texture(call, "Handle")?, self.data(call, "data"))
                {
                    let level = Self::u(call, "Level")?;
                    if let Ok((_, bytes)) = self.device.textures.lock(t, level) {
                        let n = bytes.len().min(data.len());
                        bytes[..n].copy_from_slice(&data[..n]);
                    }
                    let _ = self.device.textures.unlock(t, level);
                }
            }
            "SetTextureLevelData" => {
                if let Some(t) = self.texture(call, "Handle")? {
                    self.device.unbind_texture(t);
                    let pixels = self.data(call, "Pixels")?;
                    let palette = match Self::arg(call, "B")? {
                        Json::Null => None,
                        _ => Some(self.data(call, "B")?),
                    };
                    let lod = self.device.state.pipeline(PipelineProperty::TextureLod) != 0;
                    let _ = self.device.textures.set_level_data(
                        t,
                        Self::u(call, "Level")?,
                        Self::u(call, "SrcW")?,
                        Self::u(call, "SrcH")?,
                        Self::u(call, "Pitch")?,
                        PixelFormat(Self::u(call, "SrcFormat")?),
                        &pixels,
                        palette.as_deref(),
                        lod,
                    );
                }
            }
            "SetTextureStageTexture" => {
                let t = self.texture(call, "Handle")?;
                self.device
                    .set_stage_texture(Self::u(call, "Stage")? as usize, t);
            }
            "GetTextureStageTexture" => {
                let got = self.device.stage_texture(Self::u(call, "Stage")? as usize);
                let want = self.texture(call, "OutHandle")?;
                self.compare(call, "OutHandle", want, got);
            }
            "CreateVb" => {
                let id = Self::int(call, "Out")?;
                let h = self.device.buffers.create_vb(
                    Fvf(Self::u(call, "Fvf")?),
                    Self::u(call, "Count")?,
                    Self::u(call, "Flags")?,
                );
                if id > 0 {
                    self.ids.insert(id, Object::Vb(h));
                }
            }
            "DestroyVb" => {
                if let Some(h) = self.vb(call, "Handle")? {
                    let _ = self.device.buffers.destroy_vb(h);
                }
            }
            "ResizeVb" => {
                if let Some(h) = self.vb(call, "Handle")? {
                    let _ = self.device.buffers.resize_vb(
                        h,
                        Fvf(Self::u(call, "Fvf")?),
                        Self::u(call, "Count")?,
                    );
                }
            }
            "LockVb" | "LockIb" => {}
            "UnlockVb" | "UnlockVertexMemory" => {
                let h = match m {
                    "UnlockVb" => self.vb(call, "Handle")?,
                    _ => self.device.buffers.streams.vertex_buffer,
                };
                if let (Some(h), Some(first)) = (h, Self::arg(call, "FirstVertex")?.as_i64()) {
                    let stride = Self::u(call, "Stride")?;
                    let data = self.data(call, "data")?;
                    let _ = self
                        .device
                        .buffers
                        .replay_write_vb(h, first as u32 * stride, &data);
                }
            }
            "LockVertexMemory" => {
                let out = Self::arg(call, "Out")?;
                let fvf = Fvf(Self::u(call, "Fvf")?);
                let h = self.device.buffers.pool_vb(fvf, Self::u(call, "Count")?);
                self.device.buffers.streams.vertex_buffer = Some(h);
                self.device.buffers.streams.fvf = 0;
                if let Some(id) = out.get("id").and_then(Json::as_i64) {
                    self.ids.insert(id, Object::Vb(h));
                }
            }
            "CopyVertices" => {
                let start = Self::u(call, "StartVertex")?;
                let count = Self::u(call, "Count")?;
                let src = self.vertex_source(call, start)?;
                let h = match self.vb(call, "Handle")? {
                    Some(h) => Some(h),
                    None => Some(self.device.buffers.pool_vb(Fvf(src.fvf), count)),
                };
                if let (Some(h), Some(at)) = (h, Self::arg(call, "OutOffset")?.as_i64()) {
                    let _ = self.device.buffers.replay_copy_vertices(
                        h,
                        at as u32,
                        &src.source(),
                        start,
                        count,
                    );
                }
            }
            "CreateIb" => {
                let id = Self::int(call, "Out")?;
                let h = self
                    .device
                    .buffers
                    .create_ib(Self::u(call, "Count")?, Self::u(call, "Flags")?);
                if id > 0 {
                    self.ids.insert(id, Object::Ib(h));
                }
            }
            "DestroyIb" => {
                if let Some(IndexSource::Buffer(h)) = self.ib(call, "Handle")? {
                    let _ = self.device.buffers.destroy_ib(h);
                }
            }
            "ResizeIb" => {
                if let Some(s) = self.ib(call, "Handle")? {
                    let _ = self.device.buffers.resize_ib(s, Self::u(call, "Count")?);
                }
            }
            "UnlockIb" => {
                if let (Some(s), Some(first)) = (
                    self.ib(call, "Handle")?,
                    Self::arg(call, "FirstIndex")?.as_i64(),
                ) {
                    let data = self.indices(call, "data")?;
                    let _ = self.device.buffers.replay_write_ib(s, first as u32, &data);
                }
            }
            "CopyIndices" => {
                if let (Some(s), Some(at)) = (
                    self.ib(call, "Handle")?,
                    Self::arg(call, "OutOffset")?.as_i64(),
                ) {
                    let data = self.indices(call, "SrcData")?;
                    let _ = self.device.buffers.replay_write_ib(s, at as u32, &data);
                }
            }
            "SelectVb" => {
                let h = self.vb(call, "Handle")?;
                let r = match Self::arg(call, "Handle")?.as_i64() {
                    Some(0) => self.device.buffers.select_vb(None),
                    _ => self
                        .device
                        .buffers
                        .select_vb(h.or(self.device.buffers.streams.vertex_buffer)),
                };
                self.compare_ret(call, if r.is_ok() { GR_OK } else { E_FAIL });
            }
            "SelectIb" => {
                let s = self.ib(call, "Handle")?;
                let r = self
                    .device
                    .buffers
                    .select_ib(s, Self::u(call, "BaseVertexIndex")?);
                self.compare_ret(call, if r.is_ok() { GR_OK } else { E_FAIL });
            }
            "DrawPrimitive" => {
                let verts = self.data(call, "Verts")?;
                let r = self.device.draw_up(
                    Self::u(call, "Prim")?,
                    Fvf(Self::u(call, "Fvf")?),
                    &verts,
                    Self::u(call, "Count")?,
                );
                self.compare(call, "drawn", call.ret >= 0, r.is_ok());
            }
            "DrawIndexedPrimitiveUp" => {
                let verts = self.data(call, "Verts")?;
                let indices = self.indices(call, "Indices")?;
                let n = (Self::u(call, "IndexCount")? as usize).min(indices.len());
                let r = self.device.draw_indexed_up(
                    Self::u(call, "Prim")?,
                    Fvf(Self::u(call, "Fvf")?),
                    &verts,
                    Self::u(call, "VertCount")?,
                    &indices[..n],
                );
                self.compare(call, "drawn", call.ret >= 0, r.is_ok());
            }
            "DrawPrimitiveVb" => {
                let vb = self.vb(call, "Vb")?;
                let r = self.device.draw_vb(
                    Self::u(call, "Prim")?,
                    vb,
                    Self::u(call, "StartVertex")?,
                    Self::u(call, "Count")?,
                );
                self.compare(call, "drawn", call.ret >= 0, r.is_ok());
            }
            "DrawIndexedPrimitiveVb" => {
                let vb = self.vb(call, "Vb")?;
                let indices = self.indices(call, "Indices")?;
                let n = (Self::u(call, "IndexCount")? as usize).min(indices.len());
                let r = self.device.draw_indexed_vb(
                    Self::u(call, "Prim")?,
                    vb,
                    Self::u(call, "BaseVertex")?,
                    Self::u(call, "NumVerts")?,
                    &indices[..n],
                );
                self.compare(call, "drawn", call.ret >= 0, r.is_ok());
            }
            "DrawIndexedPrimitive" => {
                let r = self.device.draw_indexed(
                    Self::u(call, "Prim")?,
                    Self::u(call, "MinIndex")?,
                    Self::u(call, "NumVertices")?,
                    Self::u(call, "StartIndex")?,
                    Self::u(call, "Count")?,
                );
                self.compare(call, "drawn", call.ret >= 0, r.is_ok());
            }
            "ClearBuffers" => self.device.clear(Self::u(call, "Flags")?, None),
            "SwapBuffers" => self.device.present(),
            "SetViewportRect" => self.device.set_viewport(
                Self::u(call, "X")?,
                Self::u(call, "Y")?,
                Self::u(call, "W")?,
                Self::u(call, "H")?,
            ),
            "SetDepthRange" => self
                .device
                .set_depth_range(Self::float(call, "MinZ")?, Self::float(call, "MaxZ")?),
            "VbMgrReleaseAll" => self.device.buffers.release_all(),
            _ if ANSWERED.contains(&m) => {
                *self.answered.entry(m.to_owned()).or_default() += 1;
            }
            _ => return Err(TraceError::UnknownMethod(m.to_owned())),
        }
        Ok(())
    }

    /// Replays a whole trace text: every call line and defaults line in
    /// order. Returns the number of calls applied.
    pub fn replay(&mut self, text: &str) -> Result<u64, TraceError> {
        let mut applied = 0;
        for line in text.lines().filter(|l| !l.trim().is_empty()) {
            match parse_line(line)? {
                Line::Call(call) => {
                    self.apply(&call)?;
                    applied += 1;
                }
                Line::Defaults(d) => self.apply_defaults(&d)?,
                _ => {}
            }
        }
        Ok(applied)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::ff::device::VertexData;

    #[test]
    fn reads_markers_and_calls() {
        assert_eq!(
            parse_line(r#"{"format":"outpost-render-trace","version":1}"#).unwrap(),
            Line::Header { version: 1 }
        );
        assert_eq!(
            parse_line(r#"{"frame":3,"begin_frame":true,"seq":9,"snapshot_lines":2}"#).unwrap(),
            Line::BeginFrame {
                frame: 3,
                snapshot_lines: 2
            }
        );
        let Line::Call(c) = parse_line(r#"{"seq":1,"frame":0,"interface":"IRenderPipeline","method":"SwapBuffers","args":{},"ret":0}"#).unwrap() else {
            panic!("a call")
        };
        assert_eq!(c.method, "SwapBuffers");
    }

    #[test]
    fn replays_state_transforms_and_a_draw() {
        let trace = r#"{"format":"outpost-render-trace","version":1}
{"seq":1,"frame":0,"interface":"IRenderPipeline","method":"SetRenderState","args":{"State":23,"Value":4},"ret":0}
{"seq":2,"frame":0,"interface":"IRenderPipeline","method":"GetRenderState","args":{"State":23,"Out":4},"ret":0}
{"seq":3,"frame":0,"interface":"IRenderPipeline","method":"SetWorld","args":{"M12":["3f800000","00000000","00000000","00000000","3f800000","00000000","00000000","00000000","3f800000","41200000","00000000","00000000"]},"ret":0}
{"seq":4,"frame":0,"interface":"IRenderPipeline","method":"GetWorld","args":{"Out12":["3f800000","00000000","00000000","00000000","3f800000","00000000","00000000","00000000","3f800000","41200000","00000000","00000000"]},"ret":0}
{"seq":5,"frame":0,"interface":"IRenderPipeline","method":"SetPerspective","args":{"FovDeg":"42340000","Aspect":"3f800000","NearPlane":"00000000","FarPlane":"447a0000"},"ret":-1}
{"seq":6,"frame":0,"interface":"IRenderPipeline","method":"DrawPrimitive","args":{"Prim":1,"Fvf":66,"Verts":{"size":16,"hex":"0000803f0000803f0000803fffffffff"},"Count":1,"Unused":null},"ret":0}
{"seq":7,"frame":0,"interface":"IRPTexture","method":"PrintScreen","args":{},"ret":0}
{"end":true,"frame":0,"seq":8}"#;
        let mut r = ModelReplay::new(Defaults::builtin(), 640, 480, &NoBlobs);
        assert_eq!(r.replay(trace).unwrap(), 7);
        assert!(r.mismatches.is_empty(), "{:?}", r.mismatches);
        assert_eq!(r.answered.get("PrintScreen"), Some(&1));
        let p = &r.device.backend.draws[0];
        assert_eq!(
            p.vertices,
            VertexData::Bytes(vec![
                0, 0, 128, 63, 0, 0, 128, 63, 0, 0, 128, 63, 255, 255, 255, 255
            ])
        );
    }

    #[test]
    fn reports_mismatches_and_unknown_methods() {
        let mut r = ModelReplay::new(Defaults::builtin(), 640, 480, &NoBlobs);
        r.replay(r#"{"seq":1,"frame":0,"interface":"IRenderPipeline","method":"GetRenderState","args":{"State":23,"Out":4},"ret":0}"#)
            .unwrap();
        assert_eq!(r.mismatches.len(), 1);
        assert_eq!(r.mismatches[0].want, "Some(4)");
        assert_eq!(r.mismatches[0].got, "Some(2)");
        assert_eq!(
            r.replay(
                r#"{"seq":2,"frame":0,"interface":"X","method":"Frobnicate","args":{},"ret":0}"#
            ),
            Err(TraceError::UnknownMethod("Frobnicate".into()))
        );
    }

    #[test]
    fn buffer_data_goes_to_the_recorded_offsets() {
        let trace = r#"{"seq":1,"frame":0,"interface":"IRPVertexBuffer","method":"CreateVb","args":{"Fvf":66,"Count":8,"Out":5,"Flags":0},"ret":0}
{"seq":2,"frame":0,"interface":"IRPVertexBuffer","method":"UnlockVb","args":{"Handle":5,"FirstVertex":2,"Count":1,"Stride":16,"data":{"size":16,"hex":"01010101010101010101010101010101"}},"ret":0}
{"seq":3,"frame":0,"interface":"IRPIndexBuffer","method":"CopyIndices","args":{"Handle":-1,"OffsetIn":0,"OutOffset":100,"SrcData":{"size":4,"hex":"01000200"},"Count":2},"ret":0}"#;
        let mut r = ModelReplay::new(Defaults::builtin(), 640, 480, &NoBlobs);
        r.replay(trace).unwrap();
        let Some(Object::Vb(h)) = r.ids.get(&5).copied() else {
            panic!("the buffer has id 5")
        };
        assert_eq!(&r.device.buffers.vb(h).unwrap().bytes()[32..48], &[1; 16]);
        assert_eq!(
            &r.device.buffers.ib(IndexSource::Scratch).unwrap().indices()[100..102],
            &[1, 2]
        );
    }

    #[test]
    fn copy_vertices_converts_the_recorded_source() {
        let trace = r#"{"seq":1,"frame":0,"interface":"IRPVertexBuffer","method":"CreateVb","args":{"Fvf":594,"Count":4,"Out":7,"Flags":0},"ret":0}
{"seq":2,"frame":0,"interface":"IRPVertexBuffer","method":"CopyVertices","args":{"Handle":7,"OffsetIn":0,"OutOffset":1,"DstFvf":594,"SrcData":{"Flags":0,"SrcFvf":274,"SrcCount":1,"Position":{"ByteSize":12,"Stride":12,"Count":1,"Element":12,"first":0,"last":0,"data":{"size":12,"hex":"010101010101010101010101"}},"Normal":{"ByteSize":12,"Stride":12,"Count":1,"Element":12,"first":0,"last":0,"data":{"size":12,"hex":"020202020202020202020202"}},"Tex0":{"ByteSize":8,"Stride":8,"Count":1,"Element":8,"first":0,"last":0,"data":{"size":8,"hex":"0303030303030303"}}},"StartVertex":0,"Count":1},"ret":0}"#;
        let mut r = ModelReplay::new(Defaults::builtin(), 640, 480, &NoBlobs);
        r.replay(trace).unwrap();
        let Some(Object::Vb(h)) = r.ids.get(&7).copied() else {
            panic!("the buffer has id 7")
        };
        // 0x112 into 0x252 (stride 0x2C): position, normal, a gap, tex 0.
        let b = &r.device.buffers.vb(h).unwrap().bytes()[0x2C..0x58];
        assert_eq!(&b[..12], &[1; 12]);
        assert_eq!(&b[12..24], &[2; 12]);
        assert_eq!(&b[24..28], &[0; 4]);
        assert_eq!(&b[28..36], &[3; 8]);
    }

    #[test]
    fn rejects_other_format_versions() {
        for (line, version) in [
            (r#"{"format":"outpost-render-trace","version":0}"#, 0),
            (r#"{"format":"outpost-render-trace"}"#, 0),
            (r#"{"format":"outpost-render-trace","version":2}"#, 2),
        ] {
            assert_eq!(
                parse_line(line),
                Err(TraceError::UnsupportedVersion(version))
            );
        }
        let mut r = ModelReplay::new(Defaults::builtin(), 640, 480, &NoBlobs);
        assert_eq!(
            r.replay(r#"{"format":"outpost-render-trace","version":0,"renderer":"DirectX8"}"#),
            Err(TraceError::UnsupportedVersion(0))
        );
    }

    #[test]
    fn reads_defaults_lines() {
        let line = r#"{"defaults":{"render":{"7":1,"23":4,"57":4294967295},"pipeline":{"0":1,"10":-1,"14":0}},"seq":12,"frame":0,"synthetic":true}"#;
        assert_eq!(
            parse_line(line).unwrap(),
            Line::Defaults(DefaultsLine {
                render: vec![(7, 1), (23, 4), (57, u32::MAX)],
                pipeline: vec![(0, 1), (10, -1), (14, 0)],
                snapshot: false,
            })
        );
        let snapshot = r#"{"snapshot":3,"defaults":{"render":{},"pipeline":{}},"seq":12,"frame":0,"synthetic":true,"note":"a getter failed; its entry is left out"}"#;
        assert_eq!(
            parse_line(snapshot).unwrap(),
            Line::Defaults(DefaultsLine {
                render: Vec::new(),
                pipeline: Vec::new(),
                snapshot: true,
            })
        );
        assert!(matches!(
            parse_line(r#"{"defaults":{"render":{"x":1}},"seq":1,"frame":0}"#),
            Err(TraceError::BadArgument { arg: "render", .. })
        ));
    }

    #[test]
    fn a_defaults_line_replaces_the_defaults_and_resets() {
        let trace = r#"{"format":"outpost-render-trace","version":1}
{"seq":1,"frame":0,"interface":"IRenderPipeline","method":"CreateBuffers","args":{},"ret":0}
{"defaults":{"render":{"23":4,"15":1},"pipeline":{"8":65280,"14":5}},"seq":1,"frame":0,"synthetic":true}
{"seq":2,"frame":0,"interface":"IRenderPipeline","method":"GetRenderState","args":{"State":23,"Out":4},"ret":0}
{"seq":3,"frame":0,"interface":"IRenderPipeline","method":"GetRenderState","args":{"State":15,"Out":1},"ret":0}
{"seq":4,"frame":0,"interface":"IRenderPipeline","method":"GetPipelineState","args":{"Id":8,"Out":65280},"ret":0}
{"seq":5,"frame":0,"interface":"IRenderPipeline","method":"GetPipelineState","args":{"Id":14,"Out":0},"ret":0}
{"seq":6,"frame":0,"interface":"IRenderPipeline","method":"SetRenderState","args":{"State":23,"Value":7},"ret":0}
{"seq":7,"frame":0,"interface":"IRenderPipeline","method":"ResetRenderStatesToDefaults","args":{},"ret":0}
{"seq":8,"frame":0,"interface":"IRenderPipeline","method":"GetRenderState","args":{"State":23,"Out":4},"ret":0}"#;
        let mut r = ModelReplay::new(Defaults::builtin(), 640, 480, &NoBlobs);
        assert_eq!(r.replay(trace).unwrap(), 8);
        assert!(r.mismatches.is_empty(), "{:?}", r.mismatches);
        assert_eq!(
            r.device.state.defaults().render_state(RenderStateId::ZFunc),
            4
        );
        // Slot 0xE is not in the table: the line leaves it alone.
        assert_eq!(r.device.state.pipeline_raw(0xE), Some(0));
        // A state the table does not know is an error.
        assert!(matches!(
            r.replay(r#"{"defaults":{"render":{"138":1}},"seq":9,"frame":0,"synthetic":true}"#),
            Err(TraceError::BadArgument { arg: "render", .. })
        ));
    }

    #[test]
    fn reads_frame_images_and_their_notes() {
        assert_eq!(
            parse_line(r#"{"frame":5,"frame_image":"0123456789abcdef","width":640,"height":480}"#)
                .unwrap(),
            Line::FrameImage {
                frame: 5,
                image: Some("0123456789abcdef".into()),
                width: 640,
                height: 480,
                note: None,
            }
        );
        assert_eq!(
            parse_line(r#"{"frame":6,"frame_image":null,"note":"PrintScreen returned -2"}"#)
                .unwrap(),
            Line::FrameImage {
                frame: 6,
                image: None,
                width: 0,
                height: 0,
                note: Some("PrintScreen returned -2".into()),
            }
        );
    }

    /// A `LightData` line's 26 words: a directional light of red `r`.
    fn light_words(r: f32) -> String {
        let mut w = [0u32; 26];
        w[0] = 3;
        w[1] = r.to_bits();
        w[21] = 1.0f32.to_bits();
        let items: Vec<String> = w.iter().map(|x| format!("\"{x:08x}\"")).collect();
        format!("[{}]", items.join(","))
    }

    #[test]
    fn light_data_lines_give_the_data_the_flush_reads() {
        let draw = r#"{"seq":0,"frame":0,"interface":"IRenderPipeline","method":"DrawPrimitive","args":{"Prim":1,"Fvf":66,"Verts":{"size":16,"hex":"0000803f0000803f0000803fffffffff"},"Count":1,"Unused":null},"ret":0}"#;
        let call = |method: &str, args: String| {
            format!(
                r#"{{"seq":0,"frame":0,"interface":"IRenderPipeline","method":"{method}","args":{{{args}}},"ret":0}}"#
            )
        };
        let light_data = |r: f32| {
            format!(
                r#"{{"seq":0,"frame":0,"interface":"IRenderPipeline","method":"LightData","args":{{"Handle":3,"Light":{}}},"synthetic":true}}"#,
                light_words(r)
            )
        };
        let trace = [
            call(
                "AddLight",
                format!(r#""Handle":3,"Light":{}"#, light_words(0.5)),
            ),
            call("SetLightEnable", r#""Handle":3,"Enable":1"#.to_owned()),
            // The caller wrote its struct again before the draw read it.
            light_data(0.75),
            draw.to_owned(),
        ]
        .join("\n");
        let mut r = ModelReplay::new(Defaults::builtin(), 640, 480, &NoBlobs);
        r.replay(&trace).unwrap();
        let red = |r: &ModelReplay<'_>| {
            r.device
                .lights
                .device_light(0)
                .unwrap()
                .data
                .unwrap()
                .diffuse()[0]
        };
        assert_eq!(red(&r), 0.75);
        assert_eq!(r.answered.get("LightData"), None);
        // A LightData line marks nothing dirty: without an UpdateLight the
        // device keeps the data it has.
        r.replay(&[light_data(0.25), draw.to_owned()].join("\n"))
            .unwrap();
        assert_eq!(red(&r), 0.75);
        // An unknown handle is ignored.
        r.replay(&light_data(0.1).replace(r#""Handle":3"#, r#""Handle":9"#))
            .unwrap();
        assert!(r.mismatches.is_empty(), "{:?}", r.mismatches);
    }
}
