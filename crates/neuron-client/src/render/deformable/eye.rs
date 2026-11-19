//! The `EyeMaterial` (`EyeMaterial.cpp`): the material the Deformable
//! component registers for the names `^eye*`. Its ten parameters (four
//! colours, specular power, opacity, the texture's name, wrap mode and
//! address modes) live in a `D3DMATERIAL8` block and one texture row;
//! `Bind` sets the stage-0 texture with fixed stage and render states and,
//! for a face group bound to an eye bone, offsets the texture by the
//! bone's direction (`EyeUvClampPair`) through the stage-0 texture
//! transform.
//!
//! What the material calls is [`EyeServices`]. The value cells keep the
//! reference's tagged words; strings are owned bytes.

use neuron_core::hash::name_crc32;
use neuron_core::material_data::LeafWrite;
use neuron_core::vfs::{FileSystem, FsDesc};

use crate::render::material::{DrawArgs, SubmitContext, VertexLock, VertexSource};

use super::math::transpose_transform9;
use super::{INSTANCE_NONE, Instance, UvBinding, dump, file};

/// `GR_OK`.
const GR_OK: i32 = 0;
/// `GR_GENERIC`.
const GR_GENERIC: i32 = -1;

/// The class name (`ClassName`, slot 20).
pub const CLASS_NAME: &str = "EyeMaterial";
/// The material-map rule the component adds (`AddMapRule(0, "^eye*",
/// "EyeMaterial")`).
pub const MAP_PATTERN: &str = "^eye*";

/// `D3DRS_COLORVERTEX`.
const RS_COLOR_VERTEX: u32 = 141;
/// `D3DTSS_TEXCOORDINDEX`.
const TSS_TEXCOORD_INDEX: u32 = 11;
/// `D3DTSS_ADDRESSU`.
const TSS_ADDRESS_U: u32 = 13;
/// `D3DTSS_ADDRESSV`.
const TSS_ADDRESS_V: u32 = 14;
/// `D3DTSS_TEXTURETRANSFORMFLAGS`.
const TSS_TEXTURE_TRANSFORM_FLAGS: u32 = 24;

/// Stage 0's fixed states (`G_EyeStage0States`).
const STAGE0: [(u32, u32); 6] = [(1, 4), (2, 2), (3, 0), (4, 4), (5, 2), (6, 0)];
/// Stage 1's (`G_EyeStage1States`).
const STAGE1: [(u32, u32); 6] = [(1, 1), (2, 2), (3, 1), (4, 1), (5, 2), (6, 1)];
/// The render states when translucent (`G_EyeBlendStates`).
const BLEND: [(u32, u32); 5] = [(137, 1), (29, 0), (27, 1), (19, 5), (20, 6)];
/// The render states when opaque (`G_EyeOpaqueStates`).
const OPAQUE: [(u32, u32); 3] = [(137, 1), (29, 0), (27, 0)];

/// `G_ParamRangeSlack`: `FLT_EPSILON`.
const RANGE_SLACK: f32 = f32::EPSILON;
/// `G_EyeUvDeadZone`.
const UV_DEAD_ZONE: f64 = 1.0e-4;

/// The calls the material makes: `IRenderPipeline`, `IRPDraw`,
/// `IVertexBufferManager`, `ITextureLibrary2` and the engine.
pub trait EyeServices {
    /// `SetMaterial` with the seventeen words of the `D3DMATERIAL8`.
    fn set_material(&mut self, block: &[u32; 17]);
    /// `SetRenderState`.
    fn set_render_state(&mut self, state: u32, value: u32);
    /// `SetTextureStageState`.
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32);
    /// `SetTextureStageTexture` (a frame handle; 0 for none).
    fn set_stage_texture(&mut self, stage: u32, handle: u32);
    /// `SetTextureStageTransform`.
    fn set_texture_transform(&mut self, stage: u32, matrix: &[f32; 16]);
    /// `GetTextureFormat(frame, &format)`.
    ///
    /// # Errors
    ///
    /// The renderer's result.
    fn texture_format(&mut self, handle: u32) -> Result<i32, i32>;
    /// `VerifyState`.
    fn verify_state(&mut self) -> i32;
    /// `IRPDraw::DrawIndexedPrimitive`.
    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> i32;
    /// `DrawIndexedPrimitiveVb(prim, vb, base, count, indices + ib_start,
    /// span, 0)`.
    fn draw_indexed_vb(
        &mut self,
        prim: u32,
        vertex_buffer: u32,
        base_vertex: u32,
        vertex_count: u32,
        ib_start: u32,
        span: u32,
    ) -> i32;
    /// `LockVertexMemory(fvf, count, &lock)`.
    ///
    /// # Errors
    ///
    /// The manager's result.
    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32>;
    /// `CopyVertexData`.
    fn copy_vertex_data(
        &mut self,
        lock: &mut VertexLock,
        fvf: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    );
    /// `UnlockVertexMemory`.
    fn unlock_vertex_memory(&mut self, lock: VertexLock);
    /// `FindTextureByCrc({name, crc}, &out)`: the record, `None` on a miss.
    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<u32>;
    /// `GetFrameCount(record, &count)`.
    fn frame_count(&mut self, record: u32) -> i32;
    /// `CreatePlaybackGroup(record, &id)`.
    ///
    /// # Errors
    ///
    /// The library's result.
    fn create_playback(&mut self, record: u32) -> Result<i32, i32>;
    /// `GetFrame(id, frame, &out)`: the word written, `None` for none.
    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<u32>;
    /// `ReleaseTextureRecord`.
    fn release_texture_record(&mut self, record: u32);
    /// `DecRefById`.
    fn dec_ref_texture(&mut self, id: i32);
    /// `IEngine::GetInstanceParent`.
    fn instance_parent(&mut self, instance: Instance) -> Instance;
    /// `IEngine::GetInstanceOrientation`.
    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9];
    /// `FDUMP`.
    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str);
}

/// A tagged value cell (`FMaterialParamValue`): 1 an integer, 2 a float,
/// 3 and 4 three and four floats, 5 and 7 a string, 6 a component, 8 a
/// wrap mode, 9 an address mode, 10 a word.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ParamValue {
    /// `Type`.
    pub ty: i32,
    /// `Value` (floats as their bits).
    pub words: [u32; 4],
    /// The string of types 5 and 7 (`None` for null).
    pub text: Option<Vec<u8>>,
}

impl ParamValue {
    /// A cell of `ty` (`FMaterialParamValue(int)`).
    #[must_use]
    pub fn of(ty: i32) -> Self {
        Self {
            ty,
            ..Self::default()
        }
    }

    /// A float cell.
    #[must_use]
    pub fn float(value: f32) -> Self {
        Self {
            ty: 2,
            words: [value.to_bits(), 0, 0, 0],
            text: None,
        }
    }

    /// A three-float cell.
    #[must_use]
    pub fn vec3(v: [f32; 3]) -> Self {
        Self {
            ty: 3,
            words: [v[0].to_bits(), v[1].to_bits(), v[2].to_bits(), 0],
            text: None,
        }
    }

    /// A string cell of type 7.
    #[must_use]
    pub fn texture(name: &[u8]) -> Self {
        Self {
            ty: 7,
            words: [0; 4],
            text: Some(name.to_vec()),
        }
    }
}

/// One entry of the parameter table (`FMaterialParamDesc`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParamDesc {
    /// `Flags`: 0x20000 the U address bits, 0x40000 the V.
    pub flags: u32,
    /// The file key.
    pub key: &'static str,
    /// The display name.
    pub name: &'static str,
    /// The default.
    pub value: ParamValue,
    /// The minimum.
    pub min: ParamValue,
    /// The maximum.
    pub max: ParamValue,
}

/// The ten entries (`FEyeMaterial::GetParameterTable`).
#[must_use]
pub fn parameter_table() -> Vec<ParamDesc> {
    let ones = ParamValue::vec3([1.0; 3]);
    let zeros = ParamValue::vec3([0.0; 3]);
    let colour = |key, name, default: &ParamValue| ParamDesc {
        flags: 0,
        key,
        name,
        value: default.clone(),
        min: zeros.clone(),
        max: ones.clone(),
    };
    let scalar = |key, name, default: f32| ParamDesc {
        flags: 0,
        key,
        name,
        value: ParamValue::float(default),
        min: ParamValue::float(0.0),
        max: ParamValue::float(1.0),
    };
    let word = |ty: i32, flags, name, default: u32| ParamDesc {
        flags,
        key: "Dt_flags",
        name,
        value: ParamValue {
            ty,
            words: [default, 0, 0, 0],
            text: None,
        },
        min: ParamValue::of(ty),
        max: ParamValue::of(ty),
    };
    vec![
        colour("Ac", "Ambient Color", &ones),
        colour("Dc", "Diffuse Color", &ones),
        colour("Ec", "Emissive Color", &zeros),
        colour("Sc", "Specular Color", &zeros),
        scalar("Sp", "Specular Power", 0.0),
        scalar("Oc", "Opacity ", 1.0),
        ParamDesc {
            flags: 0,
            key: "Dt_name",
            name: "Texture Name",
            value: ParamValue::texture(b"(None)"),
            min: ParamValue::of(7),
            max: ParamValue::of(7),
        },
        word(8, 0, "Texture Wrap Mode", 4),
        word(9, 0x20000, "Texture U Address Mode", 2),
        word(9, 0x40000, "Texture V Address Mode", 2),
    ]
}

/// One texture row (`FMaterialTextureRow`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextureRow {
    /// The texture's name.
    pub name: Option<Vec<u8>>,
    /// The playback group; -1 for none.
    pub handle: i32,
    /// The current frame handle.
    pub frame: u32,
    /// "Texture Wrap Mode" (default 4).
    pub wrap_mode: u32,
    /// "Texture U Address Mode".
    pub address_u: u32,
    /// "Texture V Address Mode".
    pub address_v: u32,
    /// The texture has one frame.
    pub single_frame: bool,
}

impl Default for TextureRow {
    fn default() -> Self {
        Self {
            name: None,
            handle: -1,
            frame: 0,
            wrap_mode: 4,
            address_u: 0,
            address_v: 0,
            single_frame: false,
        }
    }
}

/// The alpha bits and the has-alpha byte of a `PixelFormat` id
/// (`G_PixelFormatDescs`; ids above 14 give entry 0).
fn pixel_alpha(id: i32) -> (u32, bool) {
    match id {
        5 => (4, true),
        6 | 10 => (1, true),
        7 => (8, true),
        11..=14 => (0, true),
        _ => (0, false),
    }
}

/// `FEyeMaterial`.
#[derive(Clone, Debug, PartialEq)]
pub struct EyeMaterial {
    /// The name (`SetName`: lower case, at most 63 bytes).
    pub name: Option<Vec<u8>>,
    /// The texture rows: one per type-7 parameter after `Initialize`.
    pub rows: Vec<TextureRow>,
    /// The `D3DMATERIAL8`: diffuse, ambient, specular, emissive (four
    /// floats each), power.
    pub colours: [f32; 17],
    /// `bTranslucent`.
    pub translucent: bool,
    /// `bOneBitAlpha`.
    pub one_bit_alpha: bool,
}

impl Default for EyeMaterial {
    fn default() -> Self {
        Self::new()
    }
}

impl EyeMaterial {
    /// The constructor: the colours zero, diffuse and ambient 1.0.
    #[must_use]
    pub fn new() -> Self {
        let mut colours = [0.0; 17];
        for c in &mut colours[..8] {
            *c = 1.0;
        }
        Self {
            name: None,
            rows: Vec::new(),
            colours,
            translucent: false,
            one_bit_alpha: false,
        }
    }

    /// `Initialize`: one texture row per type-7 parameter, then every
    /// default stored.
    pub fn initialize(&mut self, services: &mut dyn EyeServices) -> i32 {
        let table = parameter_table();
        let rows = table.iter().filter(|p| p.value.ty == 7).count();
        self.rows = vec![TextureRow::default(); rows];
        for (index, entry) in table.iter().enumerate() {
            self.store(index, &entry.value, services);
        }
        GR_OK
    }

    fn colour_offset(index: usize) -> Option<usize> {
        match index {
            0 => Some(4),
            1 => Some(0),
            2 => Some(12),
            3 => Some(8),
            4 => Some(16),
            5 => Some(3),
            _ => None,
        }
    }

    /// The words at a parameter's address (`GetParameterAddress`), `None`
    /// for a string or no address.
    fn read_words(&self, index: usize) -> Option<[u32; 4]> {
        if let Some(at) = Self::colour_offset(index) {
            return Some(core::array::from_fn(|k| {
                self.colours.get(at + k).map_or(0, |c| c.to_bits())
            }));
        }
        let row = self.rows.first()?;
        Some(match index {
            7 => [
                row.wrap_mode,
                row.address_u,
                row.address_v,
                u32::from(row.single_frame),
            ],
            8 => [row.address_u, row.address_v, u32::from(row.single_frame), 0],
            9 => [row.address_v, u32::from(row.single_frame), 0, 0],
            _ => return None,
        })
    }

    fn word_count(ty: i32) -> usize {
        match ty {
            1 | 2 | 8 | 9 | 10 => 1,
            3 => 3,
            4 => 4,
            _ => 0,
        }
    }

    /// `StoreParameterValue`: the cell written at the parameter's address;
    /// then "Dc" copies the diffuse colour to the ambient and "Oc"
    /// recomputes the translucency.
    pub fn store(
        &mut self,
        index: usize,
        cell: &ParamValue,
        services: &mut dyn EyeServices,
    ) -> i32 {
        let n = Self::word_count(cell.ty);
        if let Some(at) = Self::colour_offset(index) {
            for (c, w) in self.colours.iter_mut().skip(at).zip(&cell.words[..n]) {
                *c = f32::from_bits(*w);
            }
        } else if let Some(row) = self.rows.first_mut() {
            match index {
                6 if cell.ty == 5 || cell.ty == 7 => row.name.clone_from(&cell.text),
                7..=9 => {
                    let mut fields = [
                        row.wrap_mode,
                        row.address_u,
                        row.address_v,
                        u32::from(row.single_frame),
                    ];
                    for (field, w) in fields.iter_mut().skip(index - 7).zip(&cell.words[..n]) {
                        *field = *w;
                    }
                    row.wrap_mode = fields[0];
                    row.address_u = fields[1];
                    row.address_v = fields[2];
                    row.single_frame = fields[3] & 0xFF != 0;
                }
                _ => {}
            }
        }
        match parameter_table().get(index).map(|p| p.key) {
            Some("Dc") => {
                for k in 0..4 {
                    self.colours[4 + k] = self.colours[k];
                }
                GR_OK
            }
            Some("Oc") => self.update_translucency(services),
            _ => GR_OK,
        }
    }

    /// `GetParameterValue`: the parameter's current value, `None` past the
    /// table.
    #[must_use]
    pub fn get_value(&self, index: usize) -> Option<ParamValue> {
        let ty = parameter_table().get(index)?.value.ty;
        let mut out = ParamValue::of(ty);
        if ty == 5 || ty == 7 {
            out.text = self.rows.first().and_then(|r| r.name.clone());
            return Some(out);
        }
        let words = self.read_words(index)?;
        let n = Self::word_count(ty);
        out.words[..n].copy_from_slice(&words[..n]);
        Some(out)
    }

    /// `ValueWithinDeclaredRange`: the cell's type must be the
    /// parameter's; float components must lie within the bounds widened by
    /// `FLT_EPSILON`.
    #[must_use]
    pub fn in_range(entry: &ParamDesc, cell: &ParamValue) -> bool {
        if cell.ty != entry.value.ty {
            return false;
        }
        let components = match entry.value.ty {
            2 => 1,
            3 => 3,
            4 => 4,
            _ => return true,
        };
        for k in 0..components {
            let test = f64::from(f32::from_bits(cell.words[k]));
            if f64::from(f32::from_bits(entry.max.words[k])) + f64::from(RANGE_SLACK) <= test {
                return false;
            }
            if f64::from(f32::from_bits(entry.min.words[k])) - f64::from(RANGE_SLACK) >= test {
                return false;
            }
        }
        true
    }

    /// `SetParameterValue`.
    pub fn set_value(
        &mut self,
        index: usize,
        cell: &ParamValue,
        services: &mut dyn EyeServices,
    ) -> i32 {
        let table = parameter_table();
        let Some(entry) = table.get(index) else {
            return GR_GENERIC;
        };
        if !Self::in_range(entry, cell) {
            return GR_GENERIC;
        }
        self.store(index, cell, services);
        GR_OK
    }

    /// `UpdateTranslucency`: translucent when the opacity is below 1, or
    /// when the first row's frame has a format with more than one bit of
    /// alpha. The value `Setup` returns.
    pub fn update_translucency(&mut self, services: &mut dyn EyeServices) -> i32 {
        self.translucent = self.colours[3] < 1.0;
        self.one_bit_alpha = false;
        if self.translucent {
            return 1;
        }
        let frame = self.rows.first().map_or(0, |r| r.frame);
        if frame == 0 {
            return 0;
        }
        let format = match services.texture_format(frame) {
            Ok(format) => format,
            Err(result) => return result,
        };
        let (bits, has_alpha) = pixel_alpha(format);
        self.one_bit_alpha = bits == 1;
        if !(10..=14).contains(&format) && !has_alpha {
            self.translucent = false;
            return 0;
        }
        if self.one_bit_alpha {
            self.translucent = false;
            return 0;
        }
        self.translucent = true;
        1
    }

    /// `Setup`: each row's texture found by name and given a playback
    /// group (the first frame read when it has one frame), the old group
    /// released, a failed lookup of any name but "(None)" noticed; then
    /// the translucency.
    pub fn setup(&mut self, services: &mut dyn EyeServices) -> i32 {
        for index in 0..self.rows.len() {
            let row = &mut self.rows[index];
            let old = row.handle;
            row.frame = 0;
            let mut verified = true;
            row.handle = -1;
            row.single_frame = true;
            if let Some(name) = row.name.clone() {
                match services.find_texture(&name, name_crc32(&name)) {
                    None => verified = name == b"(None)",
                    Some(texture) => {
                        let count = services.frame_count(texture);
                        row.single_frame = count == 1;
                        if let Ok(handle) = services.create_playback(texture) {
                            row.handle = handle;
                            if row.single_frame
                                && let Some(frame) = services.texture_frame(handle, 0)
                            {
                                row.frame = frame;
                            }
                        }
                        services.release_texture_record(texture);
                    }
                }
            }
            if old != -1 {
                services.dec_ref_texture(old);
            }
            if !verified {
                let show = |t: &Option<Vec<u8>>| {
                    t.as_ref().map_or_else(
                        || "(null)".to_owned(),
                        |t| String::from_utf8_lossy(t).into_owned(),
                    )
                };
                let message = format!(
                    "texture verify failed: {}: {}",
                    show(&self.name),
                    show(&self.rows[index].name)
                );
                services.dump(dump::NOTICE, file::MATERIAL_IMPL, 0x23F, &message);
            }
        }
        self.update_translucency(services)
    }

    /// `ResolveTextures`: each row's frame re-read unless it has one
    /// frame; 0 without a group.
    pub fn resolve_textures(&mut self, services: &mut dyn EyeServices) -> i32 {
        for row in &mut self.rows {
            if row.handle == -1 {
                row.frame = 0;
            } else if !row.single_frame
                && let Some(frame) = services.texture_frame(row.handle, -1)
            {
                row.frame = frame;
            }
        }
        GR_OK
    }

    fn block(&self) -> [u32; 17] {
        self.colours.map(f32::to_bits)
    }

    fn has_diffuse(context: Option<&SubmitContext<'_>>) -> bool {
        context
            .and_then(|c| c.vertices)
            .is_some_and(|v| v.diffuse.data.is_some())
    }

    /// `FEyeMaterial::Bind`.
    pub fn bind(
        &mut self,
        services: &mut dyn EyeServices,
        context: Option<&SubmitContext<'_>>,
        binding: Option<&mut UvBinding>,
    ) -> i32 {
        services.set_material(&self.block());
        if Self::has_diffuse(context) {
            services.set_render_state(RS_COLOR_VERTEX, 1);
        }
        let row = self.rows.first().cloned().unwrap_or_default();
        services.set_stage_texture(0, row.frame);
        services.set_stage_state(0, TSS_TEXCOORD_INDEX, row.wrap_mode.saturating_sub(4));
        services.set_stage_state(0, TSS_ADDRESS_U, row.address_u.wrapping_add(1));
        services.set_stage_state(0, TSS_ADDRESS_V, row.address_v.wrapping_add(1));
        for (state, value) in STAGE0 {
            services.set_stage_state(0, state, value);
        }
        for (state, value) in STAGE1 {
            services.set_stage_state(1, state, value);
        }
        let states: &[(u32, u32)] = if self.translucent { &BLEND } else { &OPAQUE };
        for &(state, value) in states {
            services.set_render_state(state, value);
        }
        if context.is_none() {
            return GR_OK;
        }
        let Some(binding) = binding else {
            return GR_OK;
        };
        let Some(record) = binding.record else {
            return GR_OK;
        };
        let local = binding.bone_local;
        let mut axis = [local[2], local[5], local[8]];
        let parent = services.instance_parent(binding.bone_instance);
        if parent != INSTANCE_NONE {
            let orientation = services.instance_orientation(parent);
            axis = transpose_transform9(&orientation, &axis);
        }
        let Some((u, v)) = uv_clamp_pair(&record, &axis) else {
            return GR_OK;
        };
        let transform = [
            1.0, 0.0, u, 0.0, 0.0, 1.0, v, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
        ];
        services.set_stage_state(0, TSS_TEXTURE_TRANSFORM_FLAGS, 2);
        services.set_texture_transform(0, &transform);
        binding.eye_uv_transform_count += 1;
        GR_OK
    }

    /// `FEyeMaterial::Teardown`: nothing without a context; else the
    /// texture transform off and the vertex colour off with a diffuse
    /// stream.
    pub fn teardown(
        &mut self,
        services: &mut dyn EyeServices,
        context: Option<&SubmitContext<'_>>,
    ) -> i32 {
        if context.is_none() {
            return GR_OK;
        }
        services.set_stage_state(0, TSS_TEXTURE_TRANSFORM_FLAGS, 0);
        if Self::has_diffuse(context) {
            services.set_render_state(RS_COLOR_VERTEX, 0);
        }
        GR_OK
    }

    /// `Submit`: through `IRPDraw` with flag 0x100, else through the
    /// context's vertex buffer, else copied through vertex memory.
    pub fn submit(
        &mut self,
        services: &mut dyn EyeServices,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        if args.flags & 0x100 != 0 {
            let bias = context.ib.map_or(0, |ib| ib.bias);
            let result = services.draw_indexed(
                args.prim_type,
                args.start,
                args.count,
                bias.wrapping_add(args.ib_start),
                args.index_span,
            );
            if result >= 0 {
                return result;
            }
        }
        let mut result = GR_GENERIC;
        if let Some(vb) = context.vb {
            result = services.draw_indexed_vb(
                args.prim_type,
                vb.vertex_buffer,
                vb.base_vertex.wrapping_add(args.start),
                args.count,
                args.ib_start,
                args.index_span,
            );
        }
        if result >= 0 {
            return result;
        }
        let Some(source) = context.vertices else {
            return result;
        };
        if args.count == 0 || args.index_span == 0 {
            return result;
        }
        let mut lock = match services.lock_vertex_memory(source.fvf, args.count) {
            Ok(lock) => lock,
            Err(r) => return r,
        };
        let fvf = lock.listing.fvf;
        services.copy_vertex_data(&mut lock, fvf, source, args.start, args.count);
        let listing = lock.listing;
        services.unlock_vertex_memory(lock);
        services.draw_indexed_vb(
            args.prim_type,
            listing.vertex_buffer,
            listing.base_vertex,
            args.count,
            args.ib_start,
            args.index_span,
        )
    }

    /// `GetRenderFlags`: 0xA translucent, else 8.
    #[must_use]
    pub fn render_flags(&self) -> u32 {
        if self.translucent { 0xA } else { 8 }
    }

    /// `ApplyVerify`: `Setup`, `Bind(null)`, `VerifyState`,
    /// `Teardown(null)`; the verify result.
    pub fn apply_verify(&mut self, services: &mut dyn EyeServices) -> i32 {
        self.setup(services);
        self.bind(services, None, None);
        let result = services.verify_state();
        self.teardown(services, None);
        result
    }

    /// `SetName`: a lower-case copy of at most 63 bytes.
    pub fn set_name(&mut self, name: Option<&[u8]>) -> i32 {
        self.name = name.map(|n| {
            let n = neuron_core::hash::c_str(n);
            n[..n.len().min(0x3F)].to_ascii_lowercase()
        });
        GR_OK
    }

    /// The destructor: every row's playback released.
    pub fn destroy(&mut self, services: &mut dyn EyeServices) {
        for row in &mut self.rows {
            if row.handle != -1 {
                services.dec_ref_texture(row.handle);
                row.handle = -1;
            }
        }
    }

    /// `LoadFromFileSystem`: every parameter whose key reads is stored.
    pub fn load(&mut self, fs: &dyn FileSystem, services: &mut dyn EyeServices) -> i32 {
        for (index, entry) in parameter_table().iter().enumerate() {
            if entry.flags & 1 != 0 {
                continue;
            }
            let mut cell = ParamValue::of(entry.value.ty);
            let key = entry.key.as_bytes();
            let ok = match cell.ty {
                1 | 2 => read_words(fs, key, 1)
                    .map(|w| cell.words[0] = w[0])
                    .is_some(),
                3 | 4 => read_words(fs, key, cell.ty.cast_unsigned() as usize)
                    .map(|w| cell.words[..w.len()].copy_from_slice(&w))
                    .is_some(),
                5 | 7 => read_block(fs, key)
                    .map(|b| cell.text = Some(neuron_core::hash::c_str(&b).to_vec()))
                    .is_some(),
                8 => read_words(fs, key, 1)
                    .map(|w| cell.words[0] = (w[0] >> 4) & 0xF)
                    .is_some(),
                9 => read_words(fs, key, 1)
                    .map(|w| cell.words[0] = address_bits(w[0], address_which(entry.flags)))
                    .is_some(),
                _ => true,
            };
            if ok {
                self.store(index, &cell, services);
            }
        }
        GR_OK
    }

    /// `SaveProperties`: the leaves each parameter writes, in order; the
    /// wrap and address modes replace their bits in the stored word (read
    /// back, the writes before included).
    #[must_use]
    pub fn save(&self, fs: &dyn FileSystem) -> Vec<LeafWrite> {
        let mut writes: Vec<LeafWrite> = Vec::new();
        let stored = |writes: &[LeafWrite], key: &[u8]| {
            writes
                .iter()
                .rev()
                .find(|w| w.name == key)
                .and_then(|w| {
                    w.bytes
                        .get(..4)
                        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                })
                .or_else(|| read_words(fs, key, 1).map(|w| w[0]))
                .unwrap_or(0)
        };
        for (index, entry) in parameter_table().iter().enumerate() {
            if entry.flags & 1 != 0 {
                continue;
            }
            let Some(cell) = self.get_value(index) else {
                continue;
            };
            let key = entry.key.as_bytes();
            let bytes: Vec<u8> = match cell.ty {
                1 | 2 => cell.words[0].to_le_bytes().to_vec(),
                3 | 4 => cell.words[..cell.ty.cast_unsigned() as usize]
                    .iter()
                    .flat_map(|w| w.to_le_bytes())
                    .collect(),
                5 | 7 => {
                    let mut text = cell.text.clone().unwrap_or_default();
                    text.push(0);
                    text
                }
                8 => {
                    let mut raw = stored(&writes, key);
                    raw ^= ((cell.words[0] << 4) ^ raw) & 0xF0;
                    raw.to_le_bytes().to_vec()
                }
                9 => {
                    let mut raw = stored(&writes, key);
                    let which = address_which(entry.flags);
                    let mode = cell.words[0];
                    if which == 0 {
                        raw ^= (raw ^ mode) & 3;
                    }
                    raw ^= ((mode << 2) ^ raw) & 0xC;
                    raw.to_le_bytes().to_vec()
                }
                _ => continue,
            };
            writes.push(LeafWrite {
                name: key.to_vec(),
                bytes,
            });
        }
        writes
    }
}

/// `EyeUvClampPair`: the bone direction's x and y scaled by the row's
/// factors and plane distance; `None` inside the dead zone, else clamped
/// to the row's bounds.
#[must_use]
pub fn uv_clamp_pair(
    record: &neuron_core::mesh_data::dfm::UvDeformRecord,
    axis: &[f32; 3],
) -> Option<(f32, f32)> {
    #[allow(clippy::cast_possible_truncation)]
    let mut u = (f64::from(record.bone_x_to_u)
        * f64::from(axis[0])
        * f64::from(record.plane_distance)) as f32;
    let mut v =
        f64::from(axis[1]) * f64::from(record.bone_y_to_v) * f64::from(record.plane_distance);
    if !(f64::from(u).abs() > UV_DEAD_ZONE) && !(v.abs() > UV_DEAD_ZONE) {
        return None;
    }
    if u < record.min_du {
        u = record.min_du;
    } else if u > record.max_du {
        u = record.max_du;
    }
    if v < f64::from(record.min_dv) {
        v = f64::from(record.min_dv);
    } else if v > f64::from(record.max_dv) {
        v = f64::from(record.max_dv);
    }
    #[allow(clippy::cast_possible_truncation)]
    Some((u, v as f32))
}

/// `MaterialAddressModeWhich`: 0 for the U bits, 1 for the V.
fn address_which(flags: u32) -> u32 {
    if flags & 0x20000 != 0 {
        0
    } else if flags & 0x40000 != 0 {
        1
    } else {
        0
    }
}

/// `MaterialAddressModeBits`.
fn address_bits(flags: u32, which: u32) -> u32 {
    match which {
        0 => flags & 3,
        1 => (flags >> 2) & 3,
        _ => 0,
    }
}

/// `n` words of a leaf: the read must deliver all of them.
fn read_words(fs: &dyn FileSystem, key: &[u8], n: usize) -> Option<Vec<u32>> {
    let mut file = fs.open_file(&FsDesc::read(key)).ok()?;
    let mut buf = vec![0u8; n * 4];
    if file.read(&mut buf).ok()? != n * 4 {
        return None;
    }
    Some(
        buf.as_chunks::<4>()
            .0
            .iter()
            .map(|c| u32::from_le_bytes(*c))
            .collect(),
    )
}

/// A whole leaf: its size, then a read that must deliver all of it.
fn read_block(fs: &dyn FileSystem, key: &[u8]) -> Option<Vec<u8>> {
    let mut file = fs.open_file(&FsDesc::read(key)).ok()?;
    let size = file.size().ok()? as usize;
    let mut buf = vec![0u8; size];
    if file.read(&mut buf).ok()? != size {
        return None;
    }
    Some(buf)
}
