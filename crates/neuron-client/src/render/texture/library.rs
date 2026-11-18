//! The texture library at run time (`FTextureLibrary`, the
//! `ITextureLibrary2` slots of `CppSource/Shading/TextureLibrary.cpp`):
//! named groups of playbacks, playback records with their animation,
//! reference counting of texture records, garbage collection, and the
//! renderer calls loading and releasing make.
//!
//! The loading rules and the record model are `neuron-core`'s
//! [`TextureLoader`] (work package P1.7); here it runs against a
//! [`TexturePipeline`] instead of the recording contract, and the slots
//! the reference keeps beside its records are added: groups, playbacks,
//! the generic slots and the device abilities.
//!
//! Records are named by their index in [`TextureLoader::records`] where
//! the reference passes `FTextureEntry` pointers; `None` stands for a null
//! pointer. A destroyed record is treated as `None` where the reference
//! would use freed memory (divergence D263).

use std::collections::{BTreeMap, BTreeSet};

use neuron_core::hash::name_crc32;
use neuron_core::image::{
    FormatMaps, GroupEntry, LoadOutcome, LoaderSettings, PixelFormat, TextureFrame, TextureLoader,
    TextureRecord,
};
use neuron_core::ini::{EngineIni, EngineSection};
use neuron_core::vfs::FileSystem;

use super::format_class::read_line_into;
use super::pipeline::{AsRenderer, GR_GENERIC, TexturePipeline};
use super::playback::{FrameOut, Playback, Source, frame_out};
use crate::render::ff::defaults::profile_number;

/// A texture record, by its index in [`TextureLoader::records`].
pub type RecordId = usize;

/// The number of generic slots (`GenericSlots[7]`).
pub const GENERIC_SLOTS: usize = 7;

/// The largest child index [`TextureLibrary::set_sub_texture`] accepts;
/// above it the reference indexes past its table or allocates without
/// bound (divergence D263).
pub const MAX_SUB_TEXTURE: u32 = 0xFFFF;

/// What the library reads from the engine INI at initialisation
/// (`FTexLibAggregate::Initialize`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LibraryConfig {
    /// `[TextureLibrary] TEXTURE_LOD_LOAD` (generic slot 0), 1 when unset.
    pub lod_load: u32,
    /// `[TextureLibrary] TEXTURE_LOD_LOAD_MIN` (generic slot 4), 0 when
    /// unset.
    pub lod_min: u32,
    /// The built-in format maps, then `[TextureFormatMaps]`.
    pub format_maps: FormatMaps,
}

impl Default for LibraryConfig {
    fn default() -> Self {
        LibraryConfig {
            lod_load: 1,
            lod_min: 0,
            format_maps: FormatMaps::defaults(),
        }
    }
}

impl LibraryConfig {
    /// `InitializeFormatMaps` and the two profile reads of `Initialize`.
    #[must_use]
    pub fn from_engine_ini(ini: &EngineIni) -> LibraryConfig {
        let mut config = LibraryConfig::default();
        if let Some(section) = ini.section(b"TextureLibrary") {
            if let Some(value) = section.value(b"TEXTURE_LOD_LOAD") {
                config.lod_load = profile_number(value);
            }
            if let Some(value) = section.value(b"TEXTURE_LOD_LOAD_MIN") {
                config.lod_min = profile_number(value);
            }
        }
        if let Some(section) = ini.section(b"TextureFormatMaps") {
            load_format_maps(&mut config.format_maps, section);
        }
        config
    }
}

/// `FormatMapLoadFromParser`: each line of the section through
/// `FormatMapParseLine`, in one 1024-byte buffer that keeps what earlier
/// lines left past the terminator.
pub fn load_format_maps(maps: &mut FormatMaps, section: &EngineSection) {
    let mut buffer = [0u8; 1024];
    let mut index = 0;
    while read_line_into(section, index, &mut buffer) {
        index += 1;
        maps.parse_line(&mut buffer);
    }
}

/// A named group of playbacks (`FTextureGroup`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Group {
    pub name: Option<Vec<u8>>,
    /// Playback ids.
    pub members: Vec<i32>,
}

/// The result of [`TextureLibrary::load_group`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GroupLoad {
    /// `GR_OK` when the `Texture library` node opened, else `GR_GENERIC`.
    pub result: i32,
    /// The new group's id, -1 when the node did not open; `None` when no
    /// group was asked for.
    pub group: Option<i32>,
    /// Every child and what loading it did. A result below 0 other than
    /// -7 is the reference's "failed to load" warning.
    pub entries: Vec<GroupEntry>,
}

/// The texture library.
#[derive(Clone, Debug)]
pub struct TextureLibrary {
    loader: TextureLoader,
    format_maps: FormatMaps,
    groups: BTreeMap<i32, Group>,
    playbacks: BTreeMap<i32, Playback>,
    active: BTreeSet<i32>,
    next_playback_id: i32,
    next_id: i32,
    generic_slots: [u32; GENERIC_SLOTS],
}

impl TextureLibrary {
    /// The constructor and `Initialize`: generic slot 0 from
    /// `TEXTURE_LOD_LOAD`, slot 4 from `TEXTURE_LOD_LOAD_MIN`.
    #[must_use]
    pub fn new(config: LibraryConfig) -> TextureLibrary {
        let mut generic_slots = [0; GENERIC_SLOTS];
        generic_slots[0] = config.lod_load;
        generic_slots[4] = config.lod_min;
        let settings = LoaderSettings {
            format_maps: config.format_maps.clone(),
            ..LoaderSettings::default()
        };
        TextureLibrary {
            loader: TextureLoader::new(settings),
            format_maps: config.format_maps,
            groups: BTreeMap::new(),
            playbacks: BTreeMap::new(),
            active: BTreeSet::new(),
            next_playback_id: 1,
            next_id: 1,
            generic_slots,
        }
    }

    /// The records and the loading rules.
    #[must_use]
    pub fn loader(&self) -> &TextureLoader {
        &self.loader
    }

    /// A record.
    #[must_use]
    pub fn record(&self, record: RecordId) -> Option<&TextureRecord> {
        self.loader.records.get(record)
    }

    fn live(&self, record: Option<RecordId>) -> Option<RecordId> {
        record.filter(|&r| self.loader.records.get(r).is_some_and(|r| r.alive))
    }

    /// The record a name's CRC finds in the name map.
    #[must_use]
    pub fn find(&self, name: &[u8]) -> Option<RecordId> {
        self.loader.find(name)
    }

    /// `CreateTextureRecord`: a record of one frame over the whole of
    /// `handle`, indexed by its name unless the name's CRC is taken.
    pub fn create_record(&mut self, handle: u32, name: &[u8], retain: bool) -> RecordId {
        self.loader.create_record(handle, name, retain)
    }

    /// A record built by the caller, as `LoadLegacyTextureRecord` leaves an
    /// animated one before retaining it: indexed by its name unless the
    /// name's CRC is taken.
    pub fn insert_record(&mut self, record: TextureRecord) -> RecordId {
        self.loader.insert_record(record)
    }

    /// A playback.
    #[must_use]
    pub fn playback(&self, id: i32) -> Option<&Playback> {
        if id == -1 {
            return None;
        }
        self.playbacks.get(&id)
    }

    /// The playbacks, by id.
    pub fn playbacks(&self) -> impl Iterator<Item = (i32, &Playback)> {
        self.playbacks.iter().map(|(&id, p)| (id, p))
    }

    /// The playbacks `TickAllPlayback` advances: those whose source had
    /// more than one frame when they were made.
    pub fn active(&self) -> impl Iterator<Item = i32> + '_ {
        self.active.iter().copied()
    }

    /// The groups, by id.
    pub fn groups(&self) -> impl Iterator<Item = (i32, &Group)> {
        self.groups.iter().map(|(&id, g)| (id, g))
    }

    /// The format maps (`PickGpuFormat`'s rows).
    #[must_use]
    pub fn format_maps(&self) -> &FormatMaps {
        &self.format_maps
    }

    fn source(&self, record: RecordId) -> Source {
        let r = &self.loader.records[record];
        Source {
            frames: u32::try_from(r.frames.len()).unwrap_or(u32::MAX),
            rate: f32::from_bits(r.tile_rate),
        }
    }

    /// The handle of child `index` of `record`, 0 where the reference
    /// reads a null or destroyed child (divergence D263).
    fn child_handle(&self, record: RecordId, index: u32) -> u32 {
        match self.loader.records[record].children.get(index as usize) {
            Some(&Some(child)) if self.loader.records[child].alive => {
                self.loader.records[child].handle
            }
            _ => 0,
        }
    }

    fn clear_events(&mut self) {
        self.loader.events.clear();
    }

    /// `RefreshDeviceAbilities` and the generic slots, as each load reads
    /// them.
    fn refresh(&mut self, pipeline: &dyn TexturePipeline) {
        let square_only = pipeline.abilities().square_only;
        let slots = self.generic_slots;
        let settings = self.loader.settings_mut();
        settings.lod_load = slots[0];
        settings.square_policy = slots[2];
        settings.pow2_policy = slots[3];
        settings.lod_min = slots[4];
        settings.square_textures_only = square_only;
        settings.cube_maps = true;
    }

    // --- Slots 3 to 8 ---------------------------------------------------

    /// `SetGenericSlot`: `GR_GENERIC` for an index outside the seven slots,
    /// where the reference writes past them (divergence D263).
    pub fn set_generic_slot(&mut self, index: i32, value: u32) -> i32 {
        match usize::try_from(index).ok().filter(|&i| i < GENERIC_SLOTS) {
            Some(i) => {
                self.generic_slots[i] = value;
                0
            }
            None => GR_GENERIC,
        }
    }

    /// `GetGenericSlot`.
    #[must_use]
    pub fn generic_slot(&self, index: i32) -> Option<u32> {
        usize::try_from(index)
            .ok()
            .and_then(|i| self.generic_slots.get(i).copied())
    }

    /// `LoadTextureGroup`: every child of `Texture library` loaded, each
    /// that loads gets a playback; with `want_group`, the playbacks become
    /// a new group.
    pub fn load_group(
        &mut self,
        fs: &mut dyn FileSystem,
        pipeline: &mut dyn TexturePipeline,
        want_group: bool,
    ) -> GroupLoad {
        self.refresh(pipeline);
        let entries = self
            .loader
            .load_group_with(fs, &mut AsRenderer(pipeline))
            .map(|entries| {
                entries
                    .into_iter()
                    .map(|mut e| {
                        e.events = 0..0;
                        e
                    })
                    .collect::<Vec<_>>()
            });
        self.clear_events();
        let opened = entries.is_some();
        let entries = entries.unwrap_or_default();
        let mut members = Vec::new();
        for entry in &entries {
            if let LoadOutcome::Done {
                result,
                record: Some(record),
            } = entry.outcome
                && result >= 0
            {
                // The loader has retained the record as CreatePlaybackGroup
                // does first.
                members.push(self.new_playback(record));
            }
        }
        let group = want_group.then(|| {
            if opened {
                let id = self.next_id;
                self.groups.insert(
                    id,
                    Group {
                        name: None,
                        members,
                    },
                );
                self.next_id = id.wrapping_add(1);
                id
            } else {
                -1
            }
        });
        GroupLoad {
            result: if opened { 0 } else { GR_GENERIC },
            group,
            entries,
        }
    }

    /// `LoadTextureLibrary`: one child of `Texture library` (of the current
    /// node when that does not open), not retained.
    pub fn load_library(
        &mut self,
        fs: &mut dyn FileSystem,
        name: &[u8],
        pipeline: &mut dyn TexturePipeline,
    ) -> LoadOutcome {
        self.refresh(pipeline);
        let opened = fs.open_node(b"Texture library").is_ok();
        let outcome =
            self.loader
                .load_record_with(&*fs, name, None, false, &mut AsRenderer(pipeline));
        if opened {
            let _ = fs.open_node(b"..");
        }
        self.clear_events();
        outcome
    }

    /// `GarbageCollect`: releases a group's playbacks and removes the group
    /// (not group 0), then destroys every indexed record without
    /// references.
    pub fn garbage_collect(&mut self, group: i32, pipeline: &mut dyn TexturePipeline) -> i32 {
        if group != 0
            && let Some(g) = self.groups.remove(&group)
        {
            for member in g.members {
                self.release_playback(member, pipeline);
            }
        }
        let dead: Vec<(u32, RecordId)> = self
            .loader
            .canonical()
            .filter(|&(_, r)| self.loader.records[r].ref_count <= 0)
            .collect();
        for (crc, record) in dead {
            // A renamed record can stay indexed under its old name after
            // it was destroyed; the reference would destroy it again.
            if self.loader.records[record].alive {
                self.loader
                    .destroy_record_with(record, &mut AsRenderer(pipeline));
            }
            self.loader.unindex(crc);
        }
        self.clear_events();
        0
    }

    /// `TickAllPlayback`: every active playback advanced by the same
    /// rounded milliseconds, in id order; nothing unless `unused` is 0.
    pub fn tick_all(&mut self, unused: i32, delta_seconds: f32) -> i32 {
        if unused != 0 || self.active.is_empty() {
            return 0;
        }
        let milliseconds = (f64::from(delta_seconds) * 1000.0) as f32;
        let ids: Vec<i32> = self.active.iter().copied().collect();
        for id in ids {
            self.advance_ms(id, milliseconds);
        }
        0
    }

    fn advance_ms(&mut self, id: i32, milliseconds: f32) {
        let Some(source) = self.playbacks.get(&id).map(|p| p.source) else {
            return;
        };
        let from = self.source(source);
        if let Some(p) = self.playbacks.get_mut(&id) {
            p.advance(milliseconds, from);
        }
    }

    // --- Slots 9 to 16 --------------------------------------------------

    /// `SetGroupName`: groups 0 and -1 have no name.
    pub fn set_group_name(&mut self, id: i32, name: Option<&[u8]>) -> i32 {
        if id == -1 || id == 0 {
            return GR_GENERIC;
        }
        match self.groups.get_mut(&id) {
            Some(group) => {
                group.name = name.map(|n| n.split(|&b| b == 0).next().unwrap_or_default().to_vec());
                0
            }
            None => GR_GENERIC,
        }
    }

    /// `GetGroupName` into a buffer of `size` bytes: at most `size - 1`
    /// bytes of the name (none for `size` 0, where the reference writes
    /// before the buffer).
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for groups 0, -1 and unknown ids.
    pub fn group_name(&self, id: i32, size: u32) -> Result<Vec<u8>, i32> {
        if id == -1 || id == 0 {
            return Err(GR_GENERIC);
        }
        let group = self.groups.get(&id).ok_or(GR_GENERIC)?;
        let name = group.name.as_deref().unwrap_or_default();
        let keep = name.len().min(size.saturating_sub(1) as usize);
        Ok(name[..keep].to_vec())
    }

    /// `FindOrCreateByName`: fails when the name's CRC is indexed;
    /// otherwise a record without texture, returned retained when `take`.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` when the name is taken.
    pub fn find_or_create_by_name(
        &mut self,
        name: &[u8],
        take: bool,
    ) -> Result<Option<RecordId>, i32> {
        let crc = name_crc32(name);
        if self.loader.find_crc(crc).is_some() {
            return Err(GR_GENERIC);
        }
        self.loader.create_record(0, name, false);
        self.find_by_crc(crc, take)
    }

    /// `FindTextureByCrc`: the indexed record, retained when `take`.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` on a miss.
    pub fn find_by_crc(&mut self, crc: u32, take: bool) -> Result<Option<RecordId>, i32> {
        let record = self.loader.find_crc(crc).ok_or(GR_GENERIC)?;
        if !take {
            return Ok(None);
        }
        self.loader.add_ref(record);
        Ok(Some(record))
    }

    /// [`Self::find_by_crc`] by name, as callers build the query.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` on a miss.
    pub fn find_texture(&mut self, name: &[u8], take: bool) -> Result<Option<RecordId>, i32> {
        self.find_by_crc(name_crc32(name), take)
    }

    fn new_playback(&mut self, record: RecordId) -> i32 {
        let id = self.next_playback_id;
        self.next_playback_id = id.wrapping_add(1);
        let mut playback = Playback::new(record, self.source(record));
        playback.ref_count += 1;
        if playback.frame_count > 1 {
            self.active.insert(id);
        }
        self.playbacks.insert(id, playback);
        id
    }

    /// `CreatePlaybackGroup`: retains the record, then, when `want_id`, a
    /// playback with one reference; active when the record has more than
    /// one frame.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for no record.
    pub fn create_playback(
        &mut self,
        record: Option<RecordId>,
        want_id: bool,
    ) -> Result<Option<i32>, i32> {
        let record = self.live(record).ok_or(GR_GENERIC)?;
        self.loader.add_ref(record);
        if !want_id {
            return Ok(None);
        }
        Ok(Some(self.new_playback(record)))
    }

    /// `ReleaseTextureRecord`: `ReleaseEntryReference` with cleanup.
    pub fn release_record(
        &mut self,
        record: Option<RecordId>,
        pipeline: &mut dyn TexturePipeline,
    ) -> i32 {
        let Some(record) = self.live(record) else {
            return GR_GENERIC;
        };
        self.loader
            .release_with(record, true, &mut AsRenderer(pipeline));
        self.clear_events();
        0
    }

    /// `AddRefById`: the playback's own count.
    pub fn add_ref_id(&mut self, id: i32) -> i32 {
        if id == -1 {
            return GR_GENERIC;
        }
        match self.playbacks.get_mut(&id) {
            Some(p) => {
                p.ref_count = p.ref_count.wrapping_add(1);
                0
            }
            None => GR_GENERIC,
        }
    }

    /// `ReleasePlaybackReference`: -1 for an unknown id, else the count
    /// left (decremented whatever it was); at 0 the source is released and
    /// the playback removed.
    fn release_playback(&mut self, id: i32, pipeline: &mut dyn TexturePipeline) -> i32 {
        if id == -1 {
            return -1;
        }
        let Some(p) = self.playbacks.get_mut(&id) else {
            return -1;
        };
        p.ref_count = p.ref_count.wrapping_sub(1);
        let remaining = p.ref_count;
        if remaining == 0 {
            let source = p.source;
            if self.live(Some(source)).is_some() {
                self.loader
                    .release_with(source, true, &mut AsRenderer(pipeline));
            }
            self.playbacks.remove(&id);
            self.active.remove(&id);
            self.clear_events();
        }
        remaining.cast_signed()
    }

    /// `DecRefById`.
    pub fn dec_ref_id(&mut self, id: i32, pipeline: &mut dyn TexturePipeline) -> i32 {
        if self.release_playback(id, pipeline) == -1 {
            GR_GENERIC
        } else {
            0
        }
    }

    // --- Slots 17 to 28 -------------------------------------------------

    /// `CopyName` into a buffer of `size` bytes: at most `size - 1` bytes
    /// (unsigned, so `size` 0 copies the whole name).
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for no record.
    pub fn copy_name(&self, record: Option<RecordId>, size: u32) -> Result<Vec<u8>, i32> {
        let record = self.live(record).ok_or(GR_GENERIC)?;
        let name = &self.loader.records[record].name;
        let name = name.split(|&b| b == 0).next().unwrap_or_default();
        let keep = name.len().min(size.wrapping_sub(1) as usize);
        Ok(name[..keep].to_vec())
    }

    /// `SetRecordName`: only when the new name's CRC is indexed; that
    /// entry is removed, the record renamed and indexed under the new
    /// name. Its old entry stays.
    pub fn set_record_name(&mut self, record: Option<RecordId>, name: &[u8]) -> i32 {
        let crc = name_crc32(name);
        let Some(record) = self.live(record) else {
            return GR_GENERIC;
        };
        if self.loader.find_crc(crc).is_none() {
            return GR_GENERIC;
        }
        self.loader.unindex(crc);
        self.loader.records[record].name =
            name.split(|&b| b == 0).next().unwrap_or_default().to_vec();
        self.loader.index(record);
        0
    }

    /// `SetSubTexture`: places the indexed record whose texture is
    /// `texture` (the first in CRC order) in child slot `index` of
    /// `record`, releasing what was there and retaining `record` (not the
    /// child, as in the reference). When no record has that texture, a
    /// record `anon<texture>` is made for it, and the slot set is the new
    /// record's own.
    pub fn set_sub_texture(
        &mut self,
        record: Option<RecordId>,
        index: i32,
        texture: u32,
        pipeline: &mut dyn TexturePipeline,
    ) -> i32 {
        let Some(parent) = self.live(record) else {
            return GR_GENERIC;
        };
        let slot = index.cast_unsigned();
        if slot > MAX_SUB_TEXTURE {
            return GR_GENERIC;
        }
        let required = slot as usize + 1;
        let child = self
            .loader
            .canonical()
            .map(|(_, r)| r)
            .find(|&r| self.loader.records[r].handle == texture);
        if let Some(child) = child {
            if let Some(&Some(old)) = self.loader.records[parent].children.get(slot as usize)
                && self.loader.records[old].alive
            {
                self.loader
                    .release_with(old, true, &mut AsRenderer(pipeline));
            }
            if parent != child {
                self.loader.add_ref(parent);
            }
            let children = &mut self.loader.records[parent].children;
            if children.len() < required {
                children.resize(required, None);
            }
            children[slot as usize] = Some(child);
        } else {
            let name = format!("anon{}", texture.cast_signed());
            let child = self.loader.create_record(texture, name.as_bytes(), false);
            let children = &mut self.loader.records[child].children;
            if children.len() < required {
                children.resize(required, None);
            }
            children[slot as usize] = Some(child);
        }
        self.clear_events();
        0
    }

    fn frame_index(&self, record: RecordId, index: i32) -> Option<usize> {
        let count = self.loader.records[record].frames.len();
        let mut selected = index.cast_unsigned();
        if selected == 0xFFFF_FFFE {
            selected = u32::try_from(count).unwrap_or(u32::MAX).wrapping_sub(1);
        }
        (usize::try_from(selected).ok()? < count).then_some(selected as usize)
    }

    /// `SetFrameHandle`: the frame's first word, which is a child index.
    /// Frame -2 is the last.
    pub fn set_frame_handle(&mut self, record: Option<RecordId>, index: i32, value: u32) -> i32 {
        let Some(record) = self.live(record) else {
            return GR_GENERIC;
        };
        match self.frame_index(record, index) {
            Some(i) => {
                self.loader.records[record].frames[i].child = value;
                0
            }
            None => GR_GENERIC,
        }
    }

    /// `SetFrameRect`.
    pub fn set_frame_rect(&mut self, record: Option<RecordId>, index: i32, uv: [f32; 4]) -> i32 {
        let Some(record) = self.live(record) else {
            return GR_GENERIC;
        };
        match self.frame_index(record, index) {
            Some(i) => {
                self.loader.records[record].frames[i].uv = uv.map(f32::to_bits);
                0
            }
            None => GR_GENERIC,
        }
    }

    /// `GetTextureFrame`: frame -2 is the last; a miss writes
    /// [`FrameOut::EMPTY`] and fails.
    #[must_use]
    pub fn texture_frame(&self, record: Option<RecordId>, frame: i32) -> (i32, FrameOut) {
        let Some(record) = self.live(record) else {
            return (GR_GENERIC, FrameOut::EMPTY);
        };
        let frames: &[TextureFrame] = &self.loader.records[record].frames;
        let count = u32::try_from(frames.len()).unwrap_or(u32::MAX);
        let mut selected = frame.cast_unsigned();
        if selected == 0xFFFF_FFFE {
            selected = count.wrapping_sub(1);
        }
        match frame_out(frames, selected, |c| self.child_handle(record, c)) {
            Some(out) => (0, out),
            None => (GR_GENERIC, FrameOut::EMPTY),
        }
    }

    /// `GetGroupPixelFormat`: the format of the first child's texture;
    /// format 0 and `GR_GENERIC` without one.
    pub fn group_pixel_format(
        &self,
        record: Option<RecordId>,
        pipeline: &dyn TexturePipeline,
    ) -> (i32, PixelFormat) {
        if let Some(record) = self.live(record)
            && !self.loader.records[record].frames.is_empty()
        {
            let handle = self.child_handle(record, 0);
            if handle != 0 {
                return match pipeline.texture_format(handle) {
                    Ok(format) => (0, format),
                    Err(result) => (result, PixelFormat::UNKNOWN),
                };
            }
        }
        (GR_GENERIC, PixelFormat::UNKNOWN)
    }

    /// `SetTileRate`.
    pub fn set_tile_rate(&mut self, record: Option<RecordId>, rate: f32) -> i32 {
        match self.live(record) {
            Some(r) => {
                self.loader.records[r].tile_rate = rate.to_bits();
                0
            }
            None => GR_GENERIC,
        }
    }

    /// `GetTileRate`: 0 and `GR_GENERIC` for no record.
    #[must_use]
    pub fn tile_rate(&self, record: Option<RecordId>) -> (i32, f32) {
        match self.live(record) {
            Some(r) => (0, f32::from_bits(self.loader.records[r].tile_rate)),
            None => (GR_GENERIC, 0.0),
        }
    }

    /// `GetFrameCount`: 0 and `GR_GENERIC` for no record or no frames.
    #[must_use]
    pub fn frame_count(&self, record: Option<RecordId>) -> (i32, i32) {
        match self.live(record) {
            Some(r) if !self.loader.records[r].frames.is_empty() => {
                (0, self.loader.records[r].frames.len() as i32)
            }
            _ => (GR_GENERIC, 0),
        }
    }

    /// `ResolveTextureRecord`: the playback's source, retained; the
    /// reference retains it before checking that it is indexed, so a
    /// failure on an unindexed source keeps the reference.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for an unknown id or an unindexed source.
    pub fn resolve(&mut self, id: i32) -> Result<RecordId, i32> {
        let source = self.playback(id).ok_or(GR_GENERIC)?.source;
        if self.live(Some(source)).is_none() {
            return Err(GR_GENERIC);
        }
        self.loader.add_ref(source);
        if self.loader.canonical().any(|(_, r)| r == source) {
            Ok(source)
        } else {
            Err(GR_GENERIC)
        }
    }

    /// `GetFrame`: frame -1 is the current frame, a frame past the end the
    /// last; an empty source gives [`FrameOut::EMPTY`].
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for an unknown id.
    pub fn frame(&mut self, id: i32, frame: i32) -> Result<FrameOut, i32> {
        if id == -1 {
            return Err(GR_GENERIC);
        }
        let mut playback = self.playbacks.get(&id).ok_or(GR_GENERIC)?.clone();
        let source = playback.source;
        let out = playback
            .read_frame(
                frame.cast_unsigned(),
                &self.loader.records[source].frames,
                |c| self.child_handle(source, c),
            )
            .unwrap_or(FrameOut::EMPTY);
        self.playbacks.insert(id, playback);
        Ok(out)
    }

    fn with_playback<T>(
        &mut self,
        id: i32,
        f: impl FnOnce(&mut Playback, Source) -> T,
    ) -> Result<T, i32> {
        if id == -1 {
            return Err(GR_GENERIC);
        }
        let source = self.playbacks.get(&id).ok_or(GR_GENERIC)?.source;
        let from = self.source(source);
        let p = self.playbacks.get_mut(&id).ok_or(GR_GENERIC)?;
        Ok(f(p, from))
    }

    // --- Slots 29 to 37 -------------------------------------------------

    /// `SetElapsedTime`: the frame follows.
    pub fn set_elapsed(&mut self, id: i32, seconds: f32) -> i32 {
        self.with_playback(id, |p, from| p.set_elapsed(seconds, from))
            .map_or(GR_GENERIC, |()| 0)
    }

    /// `GetElapsedTime`, rounded to `f32`.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for an unknown id.
    pub fn elapsed(&self, id: i32) -> Result<f32, i32> {
        self.playback(id)
            .map(|p| p.elapsed as f32)
            .ok_or(GR_GENERIC)
    }

    /// `SetFrameIndex`: ignored past the source's frames, but succeeds.
    pub fn set_frame_index(&mut self, id: i32, index: u32) -> i32 {
        self.with_playback(id, |p, from| {
            let _ = p.set_frame(index, from);
        })
        .map_or(GR_GENERIC, |()| 0)
    }

    /// `GetFrameIndex`.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for an unknown id.
    pub fn frame_index_of(&self, id: i32) -> Result<u32, i32> {
        self.playback(id).map(|p| p.current_frame).ok_or(GR_GENERIC)
    }

    /// `SetLocalRate`.
    pub fn set_local_rate(&mut self, id: i32, rate: f32) -> i32 {
        self.with_playback(id, |p, from| p.set_local_rate(rate, from))
            .map_or(GR_GENERIC, |()| 0)
    }

    /// `GetEffectiveRate`.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for an unknown id.
    pub fn effective_rate(&self, id: i32) -> Result<f32, i32> {
        let p = self.playback(id).ok_or(GR_GENERIC)?;
        Ok(p.rate(self.source(p.source)))
    }

    /// `SetWrapMode` (0 clamp, 1 loop, 2 ping-pong; others hold time at
    /// 0 when advanced).
    pub fn set_wrap(&mut self, id: i32, mode: i32) -> i32 {
        self.with_playback(id, |p, _| p.wrap = mode)
            .map_or(GR_GENERIC, |()| 0)
    }

    /// `GetWrapMode`.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for an unknown id.
    pub fn wrap(&self, id: i32) -> Result<i32, i32> {
        self.playback(id).map(|p| p.wrap).ok_or(GR_GENERIC)
    }

    /// `AdvanceTime`: seconds rounded to `f32` milliseconds.
    pub fn advance(&mut self, id: i32, delta_seconds: f32) -> i32 {
        let milliseconds = (f64::from(delta_seconds) * 1000.0) as f32;
        self.with_playback(id, |p, from| p.advance(milliseconds, from))
            .map_or(GR_GENERIC, |()| 0)
    }

    // --- Slots 38 to 45 -------------------------------------------------

    /// `PeekNextGroupId`: the group count plus one, not the id counter.
    #[must_use]
    pub fn peek_next_group_id(&self) -> u32 {
        (self.groups.len() as u32).wrapping_add(1)
    }

    /// `EnumGroup`: ordinal 0 is group 0, then the groups in id order.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` past the end.
    pub fn enum_group(&self, index: i32) -> Result<i32, i32> {
        let index = index.cast_unsigned();
        if index >= (self.groups.len() as u32).wrapping_add(1) {
            return Err(GR_GENERIC);
        }
        if index == 0 {
            return Ok(0);
        }
        Ok(*self
            .groups
            .keys()
            .nth(index as usize - 1)
            .unwrap_or_else(|| unreachable!("bounded above")))
    }

    /// `GetSubCount`: the indexed records for group 0, else the group's
    /// playbacks.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` for an unknown group.
    pub fn sub_count(&self, id: i32) -> Result<i32, i32> {
        if id == 0 {
            return Ok(self.loader.canonical_len() as i32);
        }
        self.groups
            .get(&id)
            .map(|g| g.members.len() as i32)
            .ok_or(GR_GENERIC)
    }

    /// `EnumTexture`: for group 0 the `index`th indexed record, retained
    /// when `take`; for another group the source of its `index`th playback
    /// through [`Self::resolve`], whose failure leaves `None` but succeeds.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` past the end or for an unknown group.
    pub fn enum_texture(
        &mut self,
        id: i32,
        index: i32,
        take: bool,
    ) -> Result<Option<RecordId>, i32> {
        if id == 0 {
            let record = self
                .loader
                .canonical()
                .nth(index.cast_unsigned() as usize)
                .map(|(_, r)| r)
                .ok_or(GR_GENERIC)?;
            if !take {
                return Ok(None);
            }
            self.loader.add_ref(record);
            return Ok(Some(record));
        }
        let group = self.groups.get(&id).ok_or(GR_GENERIC)?;
        let member = *group
            .members
            .get(index.cast_unsigned() as usize)
            .ok_or(GR_GENERIC)?;
        Ok(self.resolve(member).ok())
    }

    /// `AddSubTexture`: a new playback of `record` in the group.
    pub fn add_sub_texture(&mut self, group: i32, record: Option<RecordId>) -> i32 {
        if group == 0 || !self.groups.contains_key(&group) {
            return GR_GENERIC;
        }
        match self.create_playback(record, true) {
            Ok(Some(id)) => {
                if let Some(g) = self.groups.get_mut(&group) {
                    g.members.push(id);
                }
                0
            }
            _ => GR_GENERIC,
        }
    }

    /// `RemoveSubTexture`: the first playback whose source is `record` is
    /// released and leaves the group. Each member is resolved (and the
    /// resolution released again) on the way. A member that does not
    /// resolve makes the reference retry it for ever; the port stops there
    /// with `GR_GENERIC` (divergence D263).
    pub fn remove_sub_texture(
        &mut self,
        group: i32,
        record: Option<RecordId>,
        pipeline: &mut dyn TexturePipeline,
    ) -> i32 {
        if group == 0 || !self.groups.contains_key(&group) {
            return GR_GENERIC;
        }
        let mut index = 0;
        loop {
            let Some(&member) = self.groups.get(&group).and_then(|g| g.members.get(index)) else {
                return GR_GENERIC;
            };
            let Ok(resolved) = self.resolve(member) else {
                return GR_GENERIC;
            };
            self.release_record(Some(resolved), pipeline);
            if Some(resolved) == record {
                self.dec_ref_id(member, pipeline);
                if let Some(g) = self.groups.get_mut(&group) {
                    g.members.remove(index);
                }
                return 0;
            }
            index += 1;
        }
    }

    /// `PickGpuFormat`: DXT1 to DXT5 as they are, else the format class of
    /// the first matching format map.
    #[must_use]
    pub fn pick_gpu_format(&self, source: PixelFormat) -> Option<PixelFormat> {
        self.format_maps.pick(source)
    }

    /// `AllocateNewTextureId`: an empty, unnamed group.
    pub fn allocate_group(&mut self) -> i32 {
        let id = self.next_id;
        self.groups.insert(id, Group::default());
        self.next_id = id.wrapping_add(1);
        id
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::ini::NoIncludes;

    #[test]
    fn config_from_the_engine_ini() {
        let mut ini = EngineIni::new();
        ini.load_buffer(
            b"[TextureLibrary]\nTEXTURE_LOD_LOAD = false\nTEXTURE_LOD_LOAD_MIN = 0x80\n\
              [TextureFormatMaps]\nclear\n32:8:8:8:8:8 = DAA4\n",
            &mut NoIncludes,
        )
        .unwrap();
        let config = LibraryConfig::from_engine_ini(&ini);
        assert_eq!((config.lod_load, config.lod_min), (0, 128));
        assert_eq!(
            config.format_maps.pick(PixelFormat::A8R8G8B8),
            Some(PixelFormat::DAA4)
        );
        assert_eq!(config.format_maps.pick(PixelFormat::R5G6B5), None);
        let library = TextureLibrary::new(config);
        assert_eq!(library.generic_slot(0), Some(0));
        assert_eq!(library.generic_slot(4), Some(128));
        assert_eq!(library.generic_slot(7), None);
        assert_eq!(LibraryConfig::default().lod_load, 1);
    }
}
