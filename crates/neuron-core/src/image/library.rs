//! What the texture library does with the `Texture library` node of an
//! archive, up to the renderer (`LoadTextureGroup`, `LoadTextureRecord`,
//! `LoadDdsTexture`, `LoadTgaTexture`, `ProbeTextureCubemap` and
//! `LoadLegacyTextureRecord` in `TextureLibrary.cpp`).
//!
//! [`TextureLoader`] runs the reference's loading rules against a
//! [`FileSystem`] and records, instead of calling a device, every call the
//! reference makes to the renderer: the image-information query, texture
//! creation, the upload of each level (size, pitch, source format, the
//! source bytes and the palette) and the cube-map load. It also keeps the
//! library's records: name, frames, child records and reference counts.
//!
//! The device is replaced by a fixed contract that the oracle harness's
//! recording renderer implements the same way:
//!
//! * `CreateTexture` fails (-1) when the width, the height or the level
//!   count is 0, or the level count exceeds `1 + log2(max(w, h))`;
//!   otherwise it returns the next handle, counting from 1.
//! * `SetTextureLevelData` returns -2 for handle 0 or source format 0 (as
//!   `DX8RENDERER` does), -1 for a level the texture does not have, else
//!   0.
//! * `LoadCubemap` (cube maps supported unless
//!   [`LoaderSettings::cube_maps`] is off) opens and maps `CUBE`, returns
//!   -8 when it cannot be opened and -1 when it cannot be mapped, and
//!   succeeds with the next handle when [`super::dds::image_info`]
//!   describes a cube texture, else -1. The reference calls
//!   `D3DXCreateCubeTextureFromFileInMemory`, which needs a device.
//! * `GetImageInfo` is `D3DXGetImageInfoFromFileInMemory` itself in the
//!   harness (`CppSource/Oracle/oracle_shading.cpp`), so
//!   [`super::dds::image_info`] is checked against D3DX 8.1.
//!
//! Where the reference reads outside a buffer or divides by zero, the
//! port reports [`Fault`] (see the divergence register).
//!
//! That contract is [`ContractRenderer`], which the plain entry points
//! ([`TextureLoader::load_group`], [`TextureLoader::load_record`],
//! [`TextureLoader::release`]) use. The `_with` forms take any
//! [`TextureRenderer`] instead: the client's texture runtime (work package
//! P4.4) passes its device there, so that the device's answers steer the
//! rules as the reference's renderer does. The events are recorded either
//! way.

use std::collections::BTreeMap;
use std::ops::Range;

use super::dds::{self, ImageInfo};
use super::format::PixelFormat;
use super::format_map::FormatMaps;
use super::tga::{self, TgaHeader};
use crate::hash::name_crc32;
use crate::vfs::{FileSystem, FindToken, FsDesc, VfsFile};

/// `GR_GENERIC`.
pub const GR_GENERIC: i32 = -1;
/// The loaders' "not this format, try the next" result.
pub const NOT_THIS_FORMAT: i32 = -8;
/// Records the TGA loader keeps across loads (`TgaMappedResources`).
pub const TGA_MAP_RECORDS: usize = 10;

/// A step the reference cannot complete without undefined behaviour or a
/// processor exception: a division by zero in the DDS pitch, a TGA
/// loader without `MIP0`, a TGA level with a negative size, or a legacy
/// child or frame table too large to allocate.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fault;

/// The library's settings and the device abilities it reads.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LoaderSettings {
    /// Generic slot 0, `TEXTURE_LOD_LOAD`: 1 when the engine INI does not
    /// set it.
    pub lod_load: u32,
    /// Generic slot 2: 2 makes every texture square, 1 only when the
    /// device asks for square textures.
    pub square_policy: u32,
    /// Generic slot 3: 1 or 2 round sizes up to powers of two.
    pub pow2_policy: u32,
    /// Generic slot 4, `TEXTURE_LOD_LOAD_MIN`: top levels are skipped while
    /// a side exceeds it; 0 skips none.
    pub lod_min: u32,
    /// Device ability 5: the device wants square textures.
    pub square_textures_only: u32,
    /// Device ability 8: cube maps.
    pub cube_maps: bool,
    /// The format rules.
    pub format_maps: FormatMaps,
}

impl Default for LoaderSettings {
    fn default() -> Self {
        Self {
            lod_load: 1,
            square_policy: 0,
            pow2_policy: 0,
            lod_min: 0,
            square_textures_only: 0,
            cube_maps: true,
            format_maps: FormatMaps::defaults(),
        }
    }
}

/// Where a level's bytes come from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LevelSource {
    /// A slice of the mapped `MIPS` file, as the DDS loader passes it.
    Mapped {
        /// Offset of the level in the file.
        offset: u64,
        /// The bytes from `offset` that the upload reads (`pitch` times the
        /// rows, block rows for DXT), cut at the end of the file.
        data: Vec<u8>,
        /// How many bytes the upload reads.
        need: u64,
    },
    /// Pixels the TGA decoder produced.
    Decoded(Vec<u8>),
}

impl LevelSource {
    /// The bytes.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        match self {
            Self::Mapped { data, .. } | Self::Decoded(data) => data,
        }
    }
}

/// One `SetTextureLevelData` call.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LevelUpload {
    /// The texture, 0 where creation failed.
    pub handle: u32,
    /// The level.
    pub level: u32,
    /// Width of the source rectangle.
    pub width: u32,
    /// Height of the source rectangle.
    pub height: u32,
    /// Bytes from one row (or block row) to the next.
    pub pitch: u32,
    /// The source format.
    pub format: PixelFormat,
    /// The source bytes.
    pub source: LevelSource,
    /// The 768-byte RGB palette of a [`PixelFormat::P8`] level.
    pub palette: Option<Vec<u8>>,
    /// The device's result.
    pub result: i32,
}

impl LevelUpload {
    /// The level as RGBA8 (see [`super::convert`]).
    ///
    /// # Errors
    ///
    /// The conversion's.
    pub fn to_rgba8(&self) -> Result<Vec<u8>, super::convert::ConvertError> {
        super::convert::to_rgba8(
            self.format,
            self.width,
            self.height,
            self.pitch,
            self.source.bytes(),
            self.palette.as_deref(),
        )
    }
}

/// A call the reference makes to the renderer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TextureEvent {
    /// `GetImageInfo` on a mapped `MIPS` file; `None` where D3DX would not
    /// describe it as a DDS file.
    ImageInfo {
        /// Bytes of the file.
        size: u32,
        /// The information.
        info: Option<ImageInfo>,
    },
    /// `CreateTexture`.
    Create {
        /// Width.
        width: u32,
        /// Height.
        height: u32,
        /// The requested format.
        format: PixelFormat,
        /// Levels.
        levels: u32,
        /// The handle, 0 when creation failed.
        handle: u32,
        /// The result.
        result: i32,
    },
    /// `SetTextureLevelData`.
    Level(LevelUpload),
    /// `LoadCubemap` on a mapped `CUBE` file.
    Cubemap {
        /// The file.
        data: Vec<u8>,
        /// What [`dds::image_info`] says about it.
        info: Option<ImageInfo>,
        /// The handle, 0 on failure.
        handle: u32,
        /// The result.
        result: i32,
    },
    /// `DestroyTexture`.
    Destroy {
        /// The handle.
        handle: u32,
    },
}

/// One frame of a record: a child record and its texture rectangle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureFrame {
    /// Index into the record's children, as stored.
    pub child: u32,
    /// The rectangle as `f32` bit patterns.
    pub uv: [u32; 4],
}

/// A texture record (`FTextureEntry`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextureRecord {
    /// The name it was loaded by.
    pub name: Vec<u8>,
    /// Its own texture; 0 for an animated record.
    pub handle: u32,
    /// The reference count.
    pub ref_count: i32,
    /// Frames.
    pub frames: Vec<TextureFrame>,
    /// Child records, by index into [`TextureLoader::records`]. A record
    /// with its own texture is its own only child.
    pub children: Vec<Option<usize>>,
    /// The frame rate as an `f32` bit pattern (0 for a still texture).
    pub tile_rate: u32,
    /// False once the record was destroyed.
    pub alive: bool,
}

/// The result of one `LoadTextureRecord`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LoadOutcome {
    /// The `GENRESULT` and the record it produced.
    Done {
        /// The result.
        result: i32,
        /// The record, for a result of 0 or more.
        record: Option<usize>,
    },
    /// See [`Fault`].
    Fault,
}

/// One child of a `Texture library` node and what loading it did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GroupEntry {
    /// The child's name.
    pub name: Vec<u8>,
    /// The outcome.
    pub outcome: LoadOutcome,
    /// The events the load recorded, an index range into
    /// [`TextureLoader::events`].
    pub events: Range<usize>,
}

/// The renderer calls the loading rules make (`CreateTexture`,
/// `SetTextureLevelData`, `LoadCubemap` and `DestroyTexture` of the
/// reference's `IRenderPipeline` and `IRPTexture`).
pub trait TextureRenderer {
    /// `CreateTexture`: a handle other than 0, or the failure's result.
    ///
    /// # Errors
    ///
    /// The renderer's result.
    fn create_texture(
        &mut self,
        width: u32,
        height: u32,
        format: PixelFormat,
        levels: u32,
    ) -> Result<u32, i32>;
    /// `SetTextureLevelData` with the upload's fields; its `result` is not
    /// set yet. Returns the result.
    fn set_level_data(&mut self, upload: &LevelUpload) -> i32;
    /// `LoadCubemap` on a mapped `CUBE` file, which
    /// [`super::dds::image_info`] describes as `info`.
    ///
    /// # Errors
    ///
    /// The renderer's result.
    fn load_cubemap(&mut self, data: &[u8], info: Option<&ImageInfo>) -> Result<u32, i32>;
    /// `DestroyTexture`.
    fn destroy_texture(&mut self, handle: u32);
}

/// The fixed renderer contract of the module documentation, which the
/// oracle harness's recording renderer implements the same way.
#[derive(Clone, Debug)]
pub struct ContractRenderer {
    next_handle: u32,
    /// The level count of each created texture, by handle.
    created_levels: BTreeMap<u32, u32>,
}

impl Default for ContractRenderer {
    fn default() -> Self {
        Self {
            next_handle: 1,
            created_levels: BTreeMap::new(),
        }
    }
}

impl TextureRenderer for ContractRenderer {
    fn create_texture(
        &mut self,
        width: u32,
        height: u32,
        _format: PixelFormat,
        levels: u32,
    ) -> Result<u32, i32> {
        if width == 0 || height == 0 || levels == 0 || levels > full_chain(width, height) {
            return Err(GR_GENERIC);
        }
        let handle = self.next_handle;
        self.next_handle += 1;
        self.created_levels.insert(handle, levels);
        Ok(handle)
    }

    fn set_level_data(&mut self, upload: &LevelUpload) -> i32 {
        if upload.handle == 0 || upload.format == PixelFormat::UNKNOWN {
            -2
        } else if self
            .created_levels
            .get(&upload.handle)
            .is_none_or(|&n| upload.level >= n)
        {
            GR_GENERIC
        } else {
            0
        }
    }

    fn load_cubemap(&mut self, _data: &[u8], info: Option<&ImageInfo>) -> Result<u32, i32> {
        if info.is_some_and(|i| i.resource_type == dds::RTYPE_CUBETEXTURE) {
            let handle = self.next_handle;
            self.next_handle += 1;
            Ok(handle)
        } else {
            Err(GR_GENERIC)
        }
    }

    fn destroy_texture(&mut self, _handle: u32) {}
}

/// A texture library and its recorded renderer calls.
#[derive(Clone, Debug, Default)]
pub struct TextureLoader {
    settings: LoaderSettings,
    /// Every record ever made, live or destroyed.
    pub records: Vec<TextureRecord>,
    by_crc: BTreeMap<u32, usize>,
    /// The renderer calls in order.
    pub events: Vec<TextureEvent>,
    /// The renderer of the plain entry points.
    contract: ContractRenderer,
    tga_maps: [Option<Vec<u8>>; TGA_MAP_RECORDS],
}

/// One DDS mip cursor (`FTextureDdsCursor`).
struct DdsCursor {
    offset: u64,
    bytes: u32,
    minimum_block: u32,
    width: u32,
    height: u32,
}

impl DdsCursor {
    /// `Advance`: no floor on the dimensions.
    fn advance(&mut self) {
        self.offset += u64::from(self.bytes);
        self.bytes >>= 2;
        if self.bytes < self.minimum_block {
            self.bytes = self.minimum_block;
        }
        self.width >>= 1;
        self.height >>= 1;
    }
}

/// `RoundTexturePowerOfTwo`.
#[must_use]
pub fn round_power_of_two(value: u32) -> u32 {
    if value & value.wrapping_sub(1) == 0 {
        return value;
    }
    let mut v = value;
    let mut shift = 0u32;
    while v != 1 {
        v >>= 1;
        shift += 1;
    }
    1u32 << ((shift + 1) & 31)
}

fn full_chain(width: u32, height: u32) -> u32 {
    32 - width.max(height).leading_zeros()
}

fn read_exact_u32(fs: &dyn FileSystem, name: &[u8]) -> Option<u32> {
    let mut file = fs.open_file(&FsDesc::read(name)).ok()?;
    let mut buf = [0u8; 4];
    match file.read(&mut buf) {
        Ok(4) => Some(u32::from_le_bytes(buf)),
        _ => None,
    }
}

/// `fld dword` then `fstp dword`: a signalling NaN comes back quiet.
#[must_use]
pub const fn x87_float_copy(bits: u32) -> u32 {
    if bits & 0x7F80_0000 == 0x7F80_0000 && bits & 0x007F_FFFF != 0 {
        bits | 0x0040_0000
    } else {
        bits
    }
}

fn map_whole(file: &VfsFile) -> Option<&[u8]> {
    file.map_view(0, 0).ok()
}

impl TextureLoader {
    /// An empty library.
    #[must_use]
    pub fn new(settings: LoaderSettings) -> Self {
        Self {
            settings,
            ..Self::default()
        }
    }

    /// The settings.
    #[must_use]
    pub fn settings(&self) -> &LoaderSettings {
        &self.settings
    }

    /// The settings, to change between loads (the reference reads its
    /// generic slots and device abilities at each load).
    pub fn settings_mut(&mut self) -> &mut LoaderSettings {
        &mut self.settings
    }

    /// Runs `f` with the contract renderer.
    fn with_contract<T>(&mut self, f: impl FnOnce(&mut Self, &mut ContractRenderer) -> T) -> T {
        let mut contract = std::mem::take(&mut self.contract);
        let out = f(self, &mut contract);
        self.contract = contract;
        out
    }

    /// The records the name map indexes, in the map's order (unsigned
    /// CRC): `(crc, record)`.
    pub fn canonical(&self) -> impl Iterator<Item = (u32, usize)> + '_ {
        self.by_crc.iter().map(|(&crc, &record)| (crc, record))
    }

    /// The number of records the name map indexes.
    #[must_use]
    pub fn canonical_len(&self) -> usize {
        self.by_crc.len()
    }

    /// The record the name map holds under `crc` (`FindEntryByCrc`).
    #[must_use]
    pub fn find_crc(&self, crc: u32) -> Option<usize> {
        self.by_crc.get(&crc).copied()
    }

    /// Removes `crc` from the name map (`CanonicalRecords.erase`); the
    /// record it held.
    pub fn unindex(&mut self, crc: u32) -> Option<usize> {
        self.by_crc.remove(&crc)
    }

    /// `StoreByNameCrc`: indexes a record by its name's CRC unless the map
    /// already holds that CRC.
    pub fn index(&mut self, record: usize) {
        self.store_by_crc(record);
    }

    /// Adds a record as `LoadLegacyTextureRecord` leaves one before its
    /// optional retain: indexed by its name, the reference count as given.
    pub fn insert_record(&mut self, record: TextureRecord) -> usize {
        let index = self.records.len();
        self.records.push(record);
        self.store_by_crc(index);
        index
    }

    /// `DestroyEntry`: the record's own texture is destroyed and the
    /// record is dead. The name map is not changed.
    pub fn destroy_record_with(&mut self, record: usize, renderer: &mut dyn TextureRenderer) {
        let handle = self.records[record].handle;
        if handle != 0 {
            self.events.push(TextureEvent::Destroy { handle });
            renderer.destroy_texture(handle);
        }
        self.records[record].alive = false;
    }

    /// The live record a name's CRC finds (`FindEntryByCrc`).
    #[must_use]
    pub fn find(&self, name: &[u8]) -> Option<usize> {
        self.by_crc.get(&name_crc32(name)).copied()
    }

    /// `LoadTextureGroup`: every child of `Texture library` whose name
    /// does not start with `.`, in find order, each loaded and, when that
    /// succeeds, retained once more by its playback group. `None` when the
    /// node cannot be opened.
    pub fn load_group(&mut self, fs: &mut dyn FileSystem) -> Option<Vec<GroupEntry>> {
        self.load_group_each(fs, |_, _| {})
    }

    /// [`Self::load_group`] with `renderer`.
    pub fn load_group_with(
        &mut self,
        fs: &mut dyn FileSystem,
        renderer: &mut dyn TextureRenderer,
    ) -> Option<Vec<GroupEntry>> {
        self.load_group_each_with(fs, renderer, |_, _| {})
    }

    /// [`Self::load_group`], calling `visit` after each child with the
    /// library as that child left it (a record's reference count can grow
    /// with later children).
    pub fn load_group_each(
        &mut self,
        fs: &mut dyn FileSystem,
        visit: impl FnMut(&Self, &GroupEntry),
    ) -> Option<Vec<GroupEntry>> {
        self.with_contract(|this, contract| this.load_group_each_with(fs, contract, visit))
    }

    /// [`Self::load_group_each`] with `renderer`.
    pub fn load_group_each_with(
        &mut self,
        fs: &mut dyn FileSystem,
        renderer: &mut dyn TextureRenderer,
        mut visit: impl FnMut(&Self, &GroupEntry),
    ) -> Option<Vec<GroupEntry>> {
        fs.open_node(b"Texture library").ok()?;
        let found = fs.find(b"*.*").unwrap_or_default();
        let mut entries = Vec::new();
        for entry in found {
            let name = entry.data.name;
            if name.first() == Some(&b'.') {
                continue;
            }
            let start = self.events.len();
            let outcome = self.load_record_with(&*fs, &name, entry.token, false, renderer);
            if let LoadOutcome::Done {
                result,
                record: Some(record),
            } = outcome
                && result >= 0
            {
                self.add_ref(record);
            }
            let group_entry = GroupEntry {
                name,
                outcome,
                events: start..self.events.len(),
            };
            visit(self, &group_entry);
            entries.push(group_entry);
        }
        let _ = fs.open_node(b"..");
        Some(entries)
    }

    /// `LoadTextureRecord(root, path, ...)`: the record a name's CRC
    /// already finds, or a new one from the first loader that recognises
    /// the node: DDS, TGA, cube map, then the animated form.
    pub fn load_record(
        &mut self,
        root: &dyn FileSystem,
        path: &[u8],
        found: Option<FindToken>,
        retain: bool,
    ) -> LoadOutcome {
        self.with_contract(|this, contract| {
            this.load_record_with(root, path, found, retain, contract)
        })
    }

    /// [`Self::load_record`] with `renderer`.
    pub fn load_record_with(
        &mut self,
        root: &dyn FileSystem,
        path: &[u8],
        found: Option<FindToken>,
        retain: bool,
        renderer: &mut dyn TextureRenderer,
    ) -> LoadOutcome {
        match self.try_load_record(root, path, found, retain, renderer) {
            Ok((result, record)) => LoadOutcome::Done { result, record },
            Err(Fault) => LoadOutcome::Fault,
        }
    }

    fn try_load_record(
        &mut self,
        root: &dyn FileSystem,
        path: &[u8],
        found: Option<FindToken>,
        retain: bool,
        renderer: &mut dyn TextureRenderer,
    ) -> Result<(i32, Option<usize>), Fault> {
        if let Some(existing) = self.find(path) {
            if retain {
                self.add_ref(existing);
            }
            return Ok((0, Some(existing)));
        }
        let Ok(texture) = root.create_child(&FsDesc::read(path).with_found(found)) else {
            return Ok((GR_GENERIC, None));
        };
        let mut handle = 0;
        let mut result = self.load_dds(&*texture, &mut handle, renderer)?;
        if result == NOT_THIS_FORMAT {
            result = self.load_tga(&*texture, &mut handle, renderer)?;
        }
        if result == NOT_THIS_FORMAT {
            result = self.probe_cube(&*texture, &mut handle, renderer);
        }
        let mut record = None;
        if result == NOT_THIS_FORMAT {
            (result, record) = self.load_legacy(root, &*texture, path, retain, renderer)?;
        }
        if handle != 0 && result >= 0 {
            record = Some(self.create_record(handle, path, retain));
        }
        Ok((result, record))
    }

    fn create(
        &mut self,
        width: u32,
        height: u32,
        format: PixelFormat,
        levels: u32,
        renderer: &mut dyn TextureRenderer,
    ) -> u32 {
        let (handle, result) = match renderer.create_texture(width, height, format, levels) {
            Ok(handle) => (handle, 0),
            Err(result) => (0, result),
        };
        self.events.push(TextureEvent::Create {
            width,
            height,
            format,
            levels,
            handle,
            result,
        });
        handle
    }

    #[allow(clippy::too_many_arguments)]
    fn upload(
        &mut self,
        handle: u32,
        level: u32,
        width: u32,
        height: u32,
        pitch: u32,
        format: PixelFormat,
        source: LevelSource,
        palette: Option<Vec<u8>>,
        renderer: &mut dyn TextureRenderer,
    ) -> i32 {
        let mut upload = LevelUpload {
            handle,
            level,
            width,
            height,
            pitch,
            format,
            source,
            palette,
            result: 0,
        };
        upload.result = renderer.set_level_data(&upload);
        let result = upload.result;
        self.events.push(TextureEvent::Level(upload));
        result
    }

    /// `SelectTextureMipLevels`: halves both sides while one exceeds
    /// [`LoaderSettings::lod_min`]; the number of halvings.
    #[must_use]
    pub fn select_mip_levels(&self, width: &mut u32, height: &mut u32) -> u32 {
        let limit = self.settings.lod_min;
        let mut count = 0;
        if limit != 0 {
            while *width > limit || *height > limit {
                *width >>= 1;
                *height >>= 1;
                count += 1;
            }
        }
        count
    }

    /// `SelectTextureDimensions`: powers of two and square sides by the
    /// policies; a zero side is returned as it is.
    #[must_use]
    pub fn select_dimensions(&self, width: u32, height: u32) -> (u32, u32) {
        let (mut w, mut h) = (width, height);
        if width == 0 || height == 0 {
            return (w, h);
        }
        if matches!(self.settings.pow2_policy, 1 | 2) {
            w = round_power_of_two(w);
            h = round_power_of_two(h);
        }
        let mut square = self.settings.square_policy == 2;
        if self.settings.square_policy == 1 {
            square = w != h && self.settings.square_textures_only != 0;
        }
        if square {
            let side = w.max(h);
            w = side;
            h = side;
        }
        (w, h)
    }

    fn pick(&self, source: PixelFormat) -> PixelFormat {
        self.settings
            .format_maps
            .pick(source)
            .unwrap_or(PixelFormat::UNKNOWN)
    }

    fn load_dds(
        &mut self,
        texture: &dyn FileSystem,
        out: &mut u32,
        renderer: &mut dyn TextureRenderer,
    ) -> Result<i32, Fault> {
        let Ok(file) = texture.open_file(&FsDesc::read(b"MIPS")) else {
            return Ok(NOT_THIS_FORMAT);
        };
        let Some(view) = map_whole(&file) else {
            return Ok(NOT_THIS_FORMAT);
        };
        let info = dds::image_info(view);
        self.events.push(TextureEvent::ImageInfo {
            size: file.size().unwrap_or(0),
            info,
        });
        let Some(info) = info else {
            return Ok(GR_GENERIC);
        };
        let source = PixelFormat::from_d3d_format(info.format);
        let pitch_flag = dds::dword(view, 0x08) & dds::FLAG_PITCH != 0;
        let mut minimum_block = 1;
        if !pitch_flag {
            minimum_block = match &dds::dword(view, 0x54).to_le_bytes() {
                b"DXT1" => 8,
                b"DXT2" | b"DXT3" | b"DXT4" | b"DXT5" => 16,
                _ => return Ok(GR_GENERIC),
            };
        }
        let (mut width, mut height) = (info.width, info.height);
        let mut skip = 0;
        let mut levels = 1u32;
        if self.settings.lod_load != 0 {
            skip = self.select_mip_levels(&mut width, &mut height);
            levels = info.mip_levels.wrapping_sub(skip);
            if levels.cast_signed() <= 0 {
                return Ok(GR_GENERIC);
            }
        }
        let (dw, dh) = self.select_dimensions(width, height);
        let target = self.pick(source);
        let handle = self.create(dw, dh, target, levels, renderer);
        if handle == 0 {
            return Ok(GR_GENERIC);
        }
        let header_height = dds::dword(view, 0x0C);
        let linear = dds::dword(view, 0x14);
        let mut cursor = DdsCursor {
            offset: dds::DATA_OFFSET as u64,
            bytes: if pitch_flag {
                linear.wrapping_mul(header_height)
            } else {
                linear
            },
            minimum_block,
            width: dds::dword(view, 0x10),
            height: header_height,
        };
        for _ in 0..skip {
            cursor.advance();
        }
        let bit_count = dds::dword(view, 0x58);
        for level in 0..levels {
            let pitch = if pitch_flag {
                (bit_count >> 3).wrapping_mul(cursor.width)
            } else {
                let blocks = cursor.width.wrapping_add(3) >> 2;
                if blocks == 0 {
                    return Err(Fault);
                }
                cursor.bytes / blocks
            };
            let rows = if source.is_dxt() {
                cursor.height.wrapping_add(3) >> 2
            } else {
                cursor.height
            };
            let need = u64::from(pitch) * u64::from(rows);
            let start = usize::try_from(cursor.offset)
                .unwrap_or(usize::MAX)
                .min(view.len());
            let end = usize::try_from(cursor.offset.saturating_add(need))
                .unwrap_or(usize::MAX)
                .min(view.len());
            let source_bytes = LevelSource::Mapped {
                offset: cursor.offset,
                data: view[start..end.max(start)].to_vec(),
                need,
            };
            let result = self.upload(
                handle,
                level,
                cursor.width,
                cursor.height,
                pitch,
                source,
                source_bytes,
                None,
                renderer,
            );
            if result < 0 {
                return Ok(GR_GENERIC);
            }
            cursor.advance();
        }
        *out = handle;
        Ok(0)
    }

    fn padded_header(view: &[u8]) -> TgaHeader {
        let mut bytes = [0u8; tga::HEADER_SIZE];
        let n = view.len().min(bytes.len());
        bytes[..n].copy_from_slice(&view[..n]);
        TgaHeader::parse(&bytes).unwrap_or_else(|| unreachable!("18 header bytes"))
    }

    fn load_tga(
        &mut self,
        texture: &dyn FileSystem,
        out: &mut u32,
        renderer: &mut dyn TextureRenderer,
    ) -> Result<i32, Fault> {
        let found = texture.find(b"MIP*").unwrap_or_default();
        let Some(first) = found.first() else {
            return Ok(NOT_THIS_FORMAT);
        };
        if first.data.is_dir() {
            return Ok(NOT_THIS_FORMAT);
        }
        let mut count = 0i32;
        let mut largest = 0i32;
        for entry in &found {
            let name = &entry.data.name;
            // cFileName[4] != 0: too many mips. A name of three bytes, or a
            // fourth byte that is not a digit, would index outside the ten
            // records in the reference; the port treats it the same way.
            let index =
                i32::from(name.get(3).copied().unwrap_or(0).cast_signed()) - i32::from(b'0');
            if name.len() > 4 || !(0..TGA_MAP_RECORDS as i32).contains(&index) {
                return Ok(NOT_THIS_FORMAT);
            }
            largest = largest.max(index);
            let opened = texture
                .open_file(&FsDesc::read(name).with_found(entry.token))
                .ok()
                .and_then(|file| map_whole(&file).map(<[u8]>::to_vec));
            if let Some(view) = opened {
                self.tga_maps[index as usize] = Some(view);
                count += 1;
            }
        }
        let Some(header_view) = self.tga_maps[0].as_deref() else {
            return Err(Fault);
        };
        let header = Self::padded_header(header_view);
        let mut handle = 0;
        let mut levels = 0i32;
        let mut skip = 0u32;
        if let Some(source) = header.source_format() {
            let mut width = i32::from(header.width).cast_unsigned();
            let mut height = i32::from(header.height).cast_unsigned();
            levels = 1;
            if self.settings.lod_load != 0 {
                skip = self.select_mip_levels(&mut width, &mut height);
                levels = count.cast_unsigned().wrapping_sub(skip).cast_signed();
                if levels <= 0 {
                    levels = 1;
                    if skip.cast_signed() >= count {
                        skip = count.wrapping_sub(1).cast_unsigned();
                    }
                }
            }
            let target = self.pick(source);
            let (dw, dh) = self.select_dimensions(width, height);
            handle = self.create(dw, dh, target, levels.cast_unsigned(), renderer);
        }
        for level in 0..levels.max(0).cast_unsigned() {
            let index = skip as usize;
            skip = skip.wrapping_add(1);
            let Some(blob) = self.tga_maps.get(index).and_then(Option::as_ref) else {
                continue;
            };
            if !Self::padded_header(blob).is_supported() {
                continue;
            }
            let image = match tga::decode(blob) {
                Ok(image) => image,
                Err(tga::TgaError::NegativeSize) => return Err(Fault),
                Err(_) => continue,
            };
            let palette = (image.format == PixelFormat::P8)
                .then(|| image.palette_rgb())
                .flatten();
            let (width, height, pitch, format) =
                (image.width, image.height, image.pitch, image.format);
            self.upload(
                handle,
                level,
                width,
                height,
                pitch,
                format,
                LevelSource::Decoded(image.pixels),
                palette,
                renderer,
            );
        }
        self.tga_maps.fill(None);
        *out = handle;
        Ok(if handle != 0 { 0 } else { GR_GENERIC })
    }

    fn probe_cube(
        &mut self,
        texture: &dyn FileSystem,
        out: &mut u32,
        renderer: &mut dyn TextureRenderer,
    ) -> i32 {
        if !self.settings.cube_maps {
            return NOT_THIS_FORMAT;
        }
        let Ok(file) = texture.open_file(&FsDesc::read(b"CUBE")) else {
            return NOT_THIS_FORMAT;
        };
        let Some(view) = map_whole(&file) else {
            return GR_GENERIC;
        };
        let info = dds::image_info(view);
        let (handle, result) = match renderer.load_cubemap(view, info.as_ref()) {
            Ok(handle) => (handle, 0),
            Err(result) => (0, result),
        };
        self.events.push(TextureEvent::Cubemap {
            data: view.to_vec(),
            info,
            handle,
            result,
        });
        *out = handle;
        result
    }

    fn load_legacy(
        &mut self,
        root: &dyn FileSystem,
        texture: &dyn FileSystem,
        name: &[u8],
        retain: bool,
        renderer: &mut dyn TextureRenderer,
    ) -> Result<(i32, Option<usize>), Fault> {
        let Some(texture_count) = read_exact_u32(texture, b"Texture count") else {
            return Ok((NOT_THIS_FORMAT, None));
        };
        if texture_count >= 0x2000_0000 {
            // The reference's child table cannot be reserved.
            return Err(Fault);
        }
        let mut children = Vec::new();
        for index in 0..texture_count {
            let mut child_name = name.to_vec();
            child_name.push(b'_');
            child_name.extend_from_slice(index.to_string().as_bytes());
            match self.try_load_record(root, &child_name, None, true, renderer)? {
                (result, record) if result >= 0 => children.push(record),
                _ => {
                    for child in children.into_iter().flatten() {
                        self.release_with(child, true, renderer);
                    }
                    return Ok((GR_GENERIC, None));
                }
            }
        }
        let fps = read_exact_u32(texture, b"FPS").unwrap_or(24.0f32.to_bits());
        let Some(frame_count) = read_exact_u32(texture, b"Frame count") else {
            return Ok((GR_GENERIC, None));
        };
        if u64::from(frame_count) * 20 > u64::from(u32::MAX) {
            // The reference's frame table cannot be allocated.
            return Err(Fault);
        }
        let Ok(mut rects) = texture.open_file(&FsDesc::read(b"Frame rects")) else {
            return Ok((GR_GENERIC, None));
        };
        if frame_count == 0 {
            // The reference reads the whole leaf into the empty table, whose
            // data pointer is null; the file system refuses that read.
            return Ok((GR_GENERIC, None));
        }
        let frame_bytes = frame_count * 20;
        if u64::from(frame_bytes) > u64::from(rects.size().unwrap_or(0)) {
            return Ok((GR_GENERIC, None));
        }
        let mut table = vec![0u8; frame_bytes as usize];
        match rects.read(&mut table) {
            Ok(n) if n == table.len() => {}
            _ => return Ok((GR_GENERIC, None)),
        }
        let frames = table
            .as_chunks::<20>()
            .0
            .iter()
            .map(|f| {
                let word = |i: usize| u32::from_le_bytes([f[i], f[i + 1], f[i + 2], f[i + 3]]);
                TextureFrame {
                    child: word(0),
                    uv: [4, 8, 12, 16].map(|i| x87_float_copy(word(i))),
                }
            })
            .collect();
        let record = self.records.len();
        self.records.push(TextureRecord {
            name: name.to_vec(),
            handle: 0,
            ref_count: 0,
            frames,
            children: children.clone(),
            tile_rate: fps,
            alive: true,
        });
        for child in children.into_iter().flatten() {
            self.release_with(child, false, renderer);
        }
        self.store_by_crc(record);
        if retain {
            self.add_ref(record);
        }
        Ok((0, Some(record)))
    }

    /// `CreateTextureRecord`: a record of one frame over the whole of its
    /// own texture, indexed by its name and retained when asked.
    pub fn create_record(&mut self, handle: u32, name: &[u8], retain: bool) -> usize {
        let record = self.records.len();
        self.records.push(TextureRecord {
            name: name.to_vec(),
            handle,
            ref_count: 0,
            frames: vec![TextureFrame {
                child: 0,
                uv: [0, 0, 1.0f32.to_bits(), 1.0f32.to_bits()],
            }],
            children: vec![Some(record)],
            tile_rate: 0,
            alive: true,
        });
        self.store_by_crc(record);
        if retain {
            self.add_ref(record);
        }
        record
    }

    fn store_by_crc(&mut self, record: usize) {
        let crc = name_crc32(&self.records[record].name);
        self.by_crc.entry(crc).or_insert(record);
    }

    /// `AddRefEntryRecursive`.
    pub fn add_ref(&mut self, record: usize) {
        self.records[record].ref_count = self.records[record].ref_count.wrapping_add(1);
        if self.records[record].frames.len() <= 1 {
            return;
        }
        for child in self.records[record].children.clone().into_iter().flatten() {
            self.add_ref(child);
        }
    }

    /// `ReleaseEntryReference`: children first for a record of several
    /// frames; with `cleanup`, a count that reaches 0 destroys the record
    /// the name's CRC finds.
    pub fn release(&mut self, record: usize, cleanup: bool) -> i32 {
        self.with_contract(|this, contract| this.release_with(record, cleanup, contract))
    }

    /// [`Self::release`] with `renderer`.
    pub fn release_with(
        &mut self,
        record: usize,
        cleanup: bool,
        renderer: &mut dyn TextureRenderer,
    ) -> i32 {
        if self.records[record].frames.len() > 1 {
            for child in self.records[record].children.clone().into_iter().flatten() {
                self.release_with(child, cleanup, renderer);
            }
        }
        let remaining = self.records[record].ref_count.wrapping_sub(1);
        self.records[record].ref_count = remaining;
        if remaining > 0 || !cleanup {
            return remaining;
        }
        let crc = name_crc32(&self.records[record].name);
        if let Some(indexed) = self.by_crc.remove(&crc) {
            self.destroy_record_with(indexed, renderer);
        }
        remaining
    }
}
