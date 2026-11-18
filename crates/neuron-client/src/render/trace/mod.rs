//! Renderer traces (record 0004, section 2): the reader of the JSONL lines
//! `RenderTrace.dll` writes, and model replay, which applies each recorded
//! call to a [`Device`] and compares every value the C++ handed back to its
//! caller with the model's.
//!
//! The reader takes format versions 0 and 1; version 1 adds header
//! defaults, frame images and `LightData` lines, which replay honours when
//! present. Buffer data goes where the C++ put it: unlock data at the
//! recorded first vertex or index, copies at the recorded output offset,
//! since the C++ ring cursors advance in frames the trace skips.

pub mod json;

use std::collections::{BTreeMap, HashMap};
use std::fmt;

use neuron_core::image::PixelFormat;

use crate::render::ff::buffers::{IbHandle, IndexSource, VbHandle};
use crate::render::ff::defaults::Defaults;
use crate::render::ff::device::{Device, RecordingBackend};
use crate::render::ff::fvf::Fvf;
use crate::render::ff::lights::{LightData, Material};
use crate::render::ff::state::PipelineProperty;
use crate::render::ff::textures::TextureHandle;
use json::Json;

/// The C++ result codes replay compares with.
const GR_OK: i64 = 0;
const GR_GENERIC: i64 = -1;
const E_FAIL: i64 = 0x8000_4005_u32 as i32 as i64;

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
    /// Version 1: the reference image of a frame.
    FrameImage {
        frame: i64,
        image: String,
    },
    End,
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
}

impl fmt::Display for TraceError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Json(e) => write!(f, "bad trace line: {e}"),
            Self::UnknownLine(l) => write!(f, "unknown trace line: {l}"),
            Self::UnknownMethod(m) => write!(f, "unknown method {m}"),
            Self::BadArgument { method, arg } => write!(f, "{method}: bad argument {arg}"),
            Self::MissingBlob(h) => write!(f, "missing blob {h}"),
        }
    }
}

impl std::error::Error for TraceError {}

/// Reads one line.
pub fn parse_line(text: &str) -> Result<Line, TraceError> {
    let v = json::parse(text).map_err(TraceError::Json)?;
    let int = |k: &str| v.get(k).and_then(Json::as_i64);
    if v.get("format").is_some() {
        return Ok(Line::Header {
            version: int("version").unwrap_or(0),
        });
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
    if let Some(image) = v.get("frame_image").and_then(Json::as_str) {
        return Ok(Line::FrameImage {
            frame: int("frame").unwrap_or(0),
            image: image.to_owned(),
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
    "CopyVertices",
    "LightData",
];

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

    fn viewspace(&self) -> bool {
        self.device
            .state
            .pipeline(PipelineProperty::ViewspaceLights)
            != 0
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
            "CreateBuffers" => {
                self.device.reset_render_states();
                let v = self.viewspace();
                self.device.transforms.apply_identity(v);
                self.device.lights.reset();
            }
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

    /// Replays a whole trace text: every call line in order. Returns the
    /// number of calls applied.
    pub fn replay(&mut self, text: &str) -> Result<u64, TraceError> {
        let mut applied = 0;
        for line in text.lines().filter(|l| !l.trim().is_empty()) {
            if let Line::Call(call) = parse_line(line)? {
                self.apply(&call)?;
                applied += 1;
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
        let trace = r#"{"format":"outpost-render-trace","version":0}
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
}
