//! The client's `[Sound]` archetypes (`Client\cmn\SoundArch.cpp`, ported in
//! `CppSource/OutpostFrontier/Client/menu_sound_arch.cpp`): the table the
//! sound tables of `OutpostFrontier.ini`'s `sounds =` keys fill
//! (`LoadClientSoundArchIni`), each archetype's level-of-detail link, its
//! use count and the mixer archetype made on first use
//! (`ArchAcquire`, `CreateSoundArchWav`).
//!
//! The table is the reference's `std::set` ordered by nickname id; the
//! walks over it (preload, unload, the level of detail, shutdown) go in
//! that order. The archetype's slots of `Archetype::ISound` are methods
//! keyed by the nickname id ([`SoundArchTable::linked`],
//! [`SoundArchTable::sm_arch_id`] and the rest).

use std::collections::BTreeMap;

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32_via_i64;
use neuron_core::rng::CrtRand;

/// `kDefaultRangeMin`: the constructor's minimum distance.
pub const DEFAULT_RANGE_MIN: f32 = 50.0;
/// `kDefaultRangeMax`: the constructor's maximum distance.
pub const DEFAULT_RANGE_MAX: f32 = 300.0;
/// `kDefaultFlags`: the constructor's flag word.
pub const DEFAULT_FLAGS: u32 = 0x1000_0000;

/// Bits of the archetype's flag word (`+0x18`).
pub mod flags {
    /// The attenuation in decibels, a signed byte.
    pub const ATTENUATION: u32 = 0xFF;
    /// `crv_pitch`'s random-pitch range, a signed byte at bit 8.
    pub const PITCH_RANGE: u32 = 0xFF00;
    /// The source type (`type =`): 0 normal, 1 interface, 2 voice, 3 music,
    /// 4 ambience.
    pub const TYPE: u32 = 0xF_0000;
    /// The persistence bits (`persistent =`): 1 space, 2 gameflow.
    pub const PERSISTENT: u32 = 0xF0_0000;
    /// `pitch_bendable`.
    pub const PITCH_BENDABLE: u32 = 0x0100_0000;
    /// The random pitch of `crv_pitch`.
    pub const RANDOM_PITCH: u32 = 0x0200_0000;
    /// `streamer`.
    pub const STREAMER: u32 = 0x0400_0000;
    /// `is_2d`.
    pub const IS_2D: u32 = 0x0800_0000;
    /// The level-of-detail link has been resolved.
    pub const LOD_RESOLVED: u32 = 0x2000_0000;
}

/// One `[Sound]` archetype (`FClientSoundArch`, `Archetype::SoundImpl`).
#[derive(Clone, Debug, PartialEq)]
pub struct SoundArch {
    /// `CreateID(nickname)`, the table's key.
    pub nickname_id: u32,
    /// The nickname as written (the port's copy; the reference keeps the id).
    pub nickname: Vec<u8>,
    /// `file =`: the wav, relative to the data.
    pub file: Option<Vec<u8>>,
    /// The mixer archetype, -1 until made.
    pub sm_arch_id: i32,
    /// `range =`'s first value.
    pub range_min: f32,
    /// `range =`'s second value, or thirty times the first.
    pub range_max: f32,
    /// The flag word (see [`flags`]).
    pub flags: u32,
    /// `lod1 =` and `lod2 =` (`CreateID`; 0 for none).
    pub lod: [u32; 2],
    /// The level-of-detail link: this archetype, another one, or none.
    pub linked: Option<u32>,
    /// The use count of an archetype whose link is none (`+0x28`).
    pub unlinked_uses: u16,
    /// The use count (`+0x2a`).
    pub use_count: u16,
}

impl SoundArch {
    /// The constructor's archetype.
    #[must_use]
    pub fn new() -> SoundArch {
        SoundArch {
            nickname_id: 0,
            nickname: Vec::new(),
            file: None,
            sm_arch_id: -1,
            range_min: DEFAULT_RANGE_MIN,
            range_max: DEFAULT_RANGE_MAX,
            flags: DEFAULT_FLAGS,
            lod: [0; 2],
            linked: None,
            unlinked_uses: 0,
            use_count: 0,
        }
    }

    /// Slot 2: the type nibble.
    #[must_use]
    pub fn source_type(&self) -> u32 {
        (self.flags >> 16) & 0xF
    }

    /// Slot 6: the attenuation, the flag word's signed low byte.
    #[must_use]
    pub fn attenuation(&self) -> i32 {
        i32::from(self.flags.to_le_bytes()[0].cast_signed())
    }

    /// Slot 9.
    #[must_use]
    pub fn is_streamer(&self) -> bool {
        self.flags & flags::STREAMER != 0
    }

    /// Slot 10.
    #[must_use]
    pub fn is_2d(&self) -> bool {
        self.flags & flags::IS_2D != 0
    }

    /// Slot 13.
    #[must_use]
    pub fn pitch_bendable(&self) -> bool {
        self.flags & flags::PITCH_BENDABLE != 0
    }

    /// The persistence bits (`(flags >> 20) & 0xf`).
    #[must_use]
    pub fn persistence(&self) -> u32 {
        (self.flags >> 20) & 0xF
    }
}

impl Default for SoundArch {
    fn default() -> Self {
        SoundArch::new()
    }
}

/// What the archetypes ask of the sound manager: a mixer archetype for a
/// wav (`CreateSoundArchWav`) and its destruction (`ISoundManager` slot 34).
pub trait WavFactory {
    /// `CreateSoundArchWav`: the mixer archetype of `request`, or -1.
    fn create_wav(&mut self, request: &WavRequest<'_>) -> i32;
    /// `DestroyArchetype`.
    fn destroy_wav(&mut self, id: i32);
}

/// The archetype slots `CreateSoundArchWav` reads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WavRequest<'a> {
    /// Slot 1: the wav's name (`None` for an archetype without a file).
    pub file: Option<&'a [u8]>,
    /// The archive the name lives in (a voice profile's `.utf`), `None`
    /// for the data.
    pub archive: Option<&'a [u8]>,
    /// Slot 10.
    pub is_2d: bool,
    /// Slot 13.
    pub pitch_bendable: bool,
}

/// `CreateID("NONE")`: a level-of-detail slot naming it links to no
/// archetype.
#[must_use]
pub fn none_id() -> u32 {
    create_id(b"NONE")
}

/// The `[Sound]` table (`G_SoundArchs`) and the sound level of detail
/// (`G_SoundLod`).
#[derive(Clone, Debug, Default)]
pub struct SoundArchTable {
    archs: BTreeMap<u32, SoundArch>,
    lod: i32,
}

impl SoundArchTable {
    /// An empty table at level of detail 0.
    #[must_use]
    pub fn new() -> SoundArchTable {
        SoundArchTable::default()
    }

    /// The archetypes in nickname-id order.
    pub fn iter(&self) -> impl Iterator<Item = &SoundArch> {
        self.archs.values()
    }

    /// How many archetypes there are.
    #[must_use]
    pub fn len(&self) -> usize {
        self.archs.len()
    }

    /// No archetypes.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.archs.is_empty()
    }

    /// The archetype with the nickname id `id`.
    #[must_use]
    pub fn get(&self, id: u32) -> Option<&SoundArch> {
        self.archs.get(&id)
    }

    /// `LookupClientSoundArch`: the registered archetype of `id`; none for
    /// 0 or an unknown id.
    #[must_use]
    pub fn lookup(&self, id: u32) -> Option<u32> {
        (id != 0 && self.archs.contains_key(&id)).then_some(id)
    }

    /// The archetype of a nickname (`CreateID` of it).
    #[must_use]
    pub fn lookup_name(&self, nickname: &[u8]) -> Option<u32> {
        self.lookup(create_id(nickname))
    }

    /// The current level of detail.
    #[must_use]
    pub fn lod(&self) -> i32 {
        self.lod
    }

    fn arch(&self, id: u32) -> &SoundArch {
        self.archs
            .get(&id)
            .unwrap_or_else(|| panic!("no [Sound] archetype {id:08x}"))
    }

    fn arch_mut(&mut self, id: u32) -> &mut SoundArch {
        self.archs
            .get_mut(&id)
            .unwrap_or_else(|| panic!("no [Sound] archetype {id:08x}"))
    }

    /// `ArchResolveLod`: link the archetype for level `lod`, moving its
    /// outstanding wav references from the old link to the new one. A level
    /// above 2 reads the fields after the two ids in the reference; the
    /// port takes no id (divergence D522).
    pub fn resolve_lod(&mut self, id: u32, lod: i32, wavs: &mut dyn WavFactory) {
        let arch = self.arch_mut(id);
        arch.flags |= flags::LOD_RESOLVED;
        if arch.linked.is_none() {
            arch.linked = Some(id);
        }
        let nickname_id = arch.nickname_id;
        let lod_id = match lod {
            1 => arch.lod[0],
            2 => arch.lod[1],
            _ => 0,
        };
        let mut target = Some(id);
        if lod_id != 0 && lod_id != nickname_id {
            target = if lod_id == none_id() {
                None
            } else {
                self.lookup(lod_id)
            };
        }
        if target == self.arch(id).linked {
            return;
        }
        let uses = self.use_count(id, wavs);
        for _ in 0..uses.max(0) {
            self.release(id, wavs);
            if let Some(t) = target {
                self.acquire(t, wavs);
            }
        }
        self.arch_mut(id).linked = target;
    }

    /// Slot 12 (`ArchGetLinked`): resolve the level of detail once, then
    /// the link.
    pub fn linked(&mut self, id: u32, wavs: &mut dyn WavFactory) -> Option<u32> {
        if self.arch(id).flags & flags::LOD_RESOLVED == 0 {
            let lod = self.lod;
            self.resolve_lod(id, lod, wavs);
        }
        self.arch(id).linked
    }

    /// Slot 0: the mixer archetype of the link, -1 without one.
    pub fn sm_arch_id(&mut self, id: u32, wavs: &mut dyn WavFactory) -> i32 {
        match self.linked(id, wavs) {
            Some(l) if l == id => self.arch(id).sm_arch_id,
            Some(l) => self.sm_arch_id(l, wavs),
            None => -1,
        }
    }

    /// Slot 5: the use count of the link (sign-extended).
    pub fn use_count(&mut self, id: u32, wavs: &mut dyn WavFactory) -> i32 {
        match self.linked(id, wavs) {
            Some(l) if l == id => i32::from(self.arch(id).use_count.cast_signed()),
            Some(l) => self.use_count(l, wavs),
            None => i32::from(self.arch(id).unlinked_uses.cast_signed()),
        }
    }

    /// Slot 11: the random pitch of `crv_pitch`, `rand() % (2n+1) - n` for a
    /// positive range `n`, from the link; 0 otherwise. The generator is the
    /// caller's (divergence D523).
    pub fn random_pitch(&mut self, id: u32, rand: &mut CrtRand, wavs: &mut dyn WavFactory) -> f32 {
        match self.linked(id, wavs) {
            Some(l) if l == id => {
                let arch = self.arch(id);
                if arch.flags & flags::RANDOM_PITCH == 0 {
                    return 0.0;
                }
                let range = i32::from(arch.flags.to_le_bytes()[1].cast_signed());
                if range <= 0 {
                    return 0.0;
                }
                let value = rand.rand() % (2 * range + 1) - range;
                value as f32
            }
            Some(l) => self.random_pitch(l, rand, wavs),
            None => 0.0,
        }
    }

    /// Slot 3 (`ArchAcquire`): through the link, make the mixer archetype
    /// on first use and count the use; true when there is a mixer archetype.
    /// An archetype linked to none counts its own use and answers true.
    pub fn acquire(&mut self, id: u32, wavs: &mut dyn WavFactory) -> bool {
        match self.linked(id, wavs) {
            Some(l) if l == id => {
                let arch = self.arch(id);
                if arch.sm_arch_id == -1 {
                    let request = WavRequest {
                        file: arch.file.as_deref(),
                        archive: None,
                        is_2d: arch.is_2d(),
                        pitch_bendable: arch.pitch_bendable(),
                    };
                    let sm = wavs.create_wav(&request);
                    self.arch_mut(id).sm_arch_id = sm;
                }
                let arch = self.arch_mut(id);
                arch.use_count = arch.use_count.wrapping_add(1);
                arch.sm_arch_id != -1
            }
            Some(l) => self.acquire(l, wavs),
            None => {
                let arch = self.arch_mut(id);
                arch.unlinked_uses = arch.unlinked_uses.wrapping_add(1);
                true
            }
        }
    }

    /// Slot 4 (`ArchReleaseWav`): through the link, drop a use; at or below
    /// zero the mixer archetype is destroyed. The remaining count.
    pub fn release(&mut self, id: u32, wavs: &mut dyn WavFactory) -> i32 {
        match self.linked(id, wavs) {
            Some(l) if l == id => {
                let arch = self.arch_mut(id);
                if arch.use_count != 0 {
                    arch.use_count -= 1;
                }
                if arch.use_count.cast_signed() <= 0 && arch.sm_arch_id != -1 {
                    let sm = arch.sm_arch_id;
                    wavs.destroy_wav(sm);
                    self.arch_mut(id).sm_arch_id = -1;
                }
                i32::from(self.arch(id).use_count.cast_signed())
            }
            Some(l) => self.release(l, wavs),
            None => {
                let arch = self.arch_mut(id);
                if arch.unlinked_uses != 0 {
                    arch.unlinked_uses -= 1;
                }
                i32::from(arch.unlinked_uses.cast_signed())
            }
        }
    }

    /// `SoundArchRedefine`: the same file without `force` keeps the mixer
    /// archetype (every field copied but it); otherwise the uses are
    /// released, the fields copied and the uses taken again.
    fn redefine(&mut self, id: u32, src: SoundArch, force: bool, wavs: &mut dyn WavFactory) {
        let same_file = {
            let this = self.arch(id);
            this.file.as_deref().unwrap_or_default() == src.file.as_deref().unwrap_or_default()
        };
        if !force && same_file {
            let keep = self.arch(id).sm_arch_id;
            copy_fields(self.arch_mut(id), src);
            self.arch_mut(id).sm_arch_id = keep;
            return;
        }
        let uses = self.use_count(id, wavs);
        for _ in 0..uses.max(0) {
            self.release(id, wavs);
        }
        copy_fields(self.arch_mut(id), src);
        for _ in 0..uses.max(0) {
            self.acquire(id, wavs);
        }
    }

    /// `LoadClientSoundArchIni`: every `[Sound]` of an INI file into the
    /// table; the number of archetypes the table grew by. A nickname already
    /// present redefines that archetype (the reference's collision test
    /// compares two texts of one id, so it never fires).
    pub fn load_ini(&mut self, bytes: &[u8], force: bool, wavs: &mut dyn WavFactory) -> usize {
        let before = self.archs.len();
        let mut reader = IniReader::new();
        if !reader.open_memory(bytes) {
            return 0;
        }
        while reader.read_header() {
            if !reader.is_header(b"Sound") {
                continue;
            }
            let mut scratch = SoundArch {
                range_min: -1.0,
                range_max: -1.0,
                ..SoundArch::new()
            };
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    let nick = reader.value_string();
                    scratch.nickname_id = create_id(&nick);
                    scratch.nickname = nick;
                } else {
                    parse_value(&mut scratch, &mut reader);
                }
            }
            if scratch.nickname_id == 0 {
                continue;
            }
            // The attenuation clamped to [-96, 0], back into the low byte.
            let atten = f64::from(scratch.attenuation());
            let clamped = if atten > 0.0 {
                0.0
            } else if atten < -96.0 {
                -96.0
            } else {
                atten as f32
            };
            let low = trunc_to_i32_via_i64(f64::from(clamped)).cast_unsigned() & 0xFF;
            scratch.flags = (scratch.flags & 0xFFFF_FF00) | low;
            if scratch.range_min < 0.0 {
                scratch.range_min = -1.0;
                scratch.range_max = -1.0;
            }
            let id = scratch.nickname_id;
            // A redefinition needs the whole table (its uses move through
            // the links), so the entry API does not fit.
            #[allow(clippy::map_entry)]
            if self.archs.contains_key(&id) {
                self.redefine(id, scratch, force, wavs);
            } else {
                self.archs.insert(id, scratch);
            }
        }
        self.archs.len() - before
    }

    /// `PreloadClientSoundArchesByCategory`: each archetype whose
    /// persistence bits meet `mask` and whose slot 0 answers -1 takes one use.
    pub fn preload_category(&mut self, mask: u32, wavs: &mut dyn WavFactory) {
        let ids: Vec<u32> = self.archs.keys().copied().collect();
        for id in ids {
            if self.arch(id).persistence() & mask & 0xF == 0 {
                continue;
            }
            if self.sm_arch_id(id, wavs) == -1 {
                self.acquire(id, wavs);
            }
        }
    }

    /// `UnloadClientSoundArchesByCategory`: the same walk and test, one
    /// release each.
    pub fn unload_category(&mut self, mask: u32, wavs: &mut dyn WavFactory) {
        let ids: Vec<u32> = self.archs.keys().copied().collect();
        for id in ids {
            if self.arch(id).persistence() & mask & 0xF == 0 {
                continue;
            }
            if self.sm_arch_id(id, wavs) == -1 {
                self.release(id, wavs);
            }
        }
    }

    /// `SetClientSoundLod`: a new level re-resolves every link in id order.
    pub fn set_lod(&mut self, lod: i32, wavs: &mut dyn WavFactory) {
        if lod == self.lod {
            return;
        }
        self.lod = lod;
        let ids: Vec<u32> = self.archs.keys().copied().collect();
        for id in ids {
            self.resolve_lod(id, lod, wavs);
        }
    }

    /// `ShutdownClientSoundArch`: every archetype released until it answers
    /// 0, then the table emptied.
    pub fn shutdown(&mut self, wavs: &mut dyn WavFactory) {
        let ids: Vec<u32> = self.archs.keys().copied().collect();
        for id in ids {
            while self.release(id, wavs) != 0 {}
        }
        self.archs.clear();
    }
}

/// `SoundArchCopyFields`: every field from `src`, the flag word's top two
/// bits kept.
fn copy_fields(this: &mut SoundArch, src: SoundArch) {
    let keep = this.flags & 0xC000_0000;
    *this = SoundArch {
        flags: (src.flags & 0x3FFF_FFFF) | keep,
        ..src
    };
}

/// `ParseSoundType`'s table.
const TYPES: [&[u8]; 5] = [b"normal", b"interface", b"voice", b"music", b"ambience"];

/// `ParseSoundValue`: one key of a `[Sound]`; false for an unknown key.
fn parse_value(arch: &mut SoundArch, reader: &mut IniReader) -> bool {
    let bit = |arch: &mut SoundArch, reader: &mut IniReader, mask: u32| {
        let on = reader.value_bool(0);
        arch.flags = (arch.flags & !mask) | if on { mask } else { 0 };
    };
    if reader.is_value(b"file") {
        arch.file = Some(reader.value_string());
    } else if reader.is_value(b"attenuation") {
        let atten = trunc_to_i32_via_i64(reader.value_float(0));
        arch.flags = (arch.flags & 0xFFFF_FF00) | (atten.cast_unsigned() & 0xFF);
    } else if reader.is_value(b"range") {
        arch.range_min = reader.value_float(0) as f32;
        arch.range_max = if reader.is_value_empty(1) {
            (f64::from(arch.range_min) * 30.0) as f32
        } else {
            reader.value_float(1) as f32
        };
    } else if reader.is_value(b"persistent") {
        let value = reader.value_string();
        if eq_ignore_case(b"space", &value) {
            arch.flags = (arch.flags & 0xFF1F_FFFF) | 0x10_0000;
        } else if eq_ignore_case(b"gameflow", &value) {
            arch.flags = (arch.flags & 0xFF2F_FFFF) | 0x20_0000;
        }
    } else if reader.is_value(b"type") {
        let value = reader.value_string();
        if let Some(index) = TYPES.iter().position(|t| eq_ignore_case(t, &value)) {
            let id = u32::try_from(index).unwrap_or(0);
            arch.flags = (((id << 16) ^ arch.flags) & flags::TYPE) ^ arch.flags;
        }
    } else if reader.is_value(b"streamer") {
        bit(arch, reader, flags::STREAMER);
    } else if reader.is_value(b"is_2d") {
        bit(arch, reader, flags::IS_2D);
    } else if reader.is_value(b"pitch_bendable") {
        bit(arch, reader, flags::PITCH_BENDABLE);
    } else if reader.is_value(b"crv_pitch") {
        arch.flags |= flags::PITCH_BENDABLE | flags::RANDOM_PITCH;
        let pitch = reader.value_float(0) as f32;
        let byte = trunc_to_i32_via_i64(f64::from(pitch)).cast_unsigned() & 0xFF;
        arch.flags = (arch.flags & 0xFFFF_00FF) | (byte << 8);
    } else {
        let index = if reader.is_value(b"lod1") {
            0
        } else if reader.is_value(b"lod2") {
            1
        } else {
            return false;
        };
        let id = create_id(&reader.value_string());
        for slot in &mut arch.lod[index..] {
            *slot = id;
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Wavs {
        next: i32,
        log: Vec<String>,
    }

    impl WavFactory for Wavs {
        fn create_wav(&mut self, request: &WavRequest<'_>) -> i32 {
            let Some(file) = request.file else {
                return -1;
            };
            if file.starts_with(b"missing") {
                return -1;
            }
            self.next += 1;
            self.log.push(format!("create {}", self.next));
            self.next
        }
        fn destroy_wav(&mut self, id: i32) {
            self.log.push(format!("destroy {id}"));
        }
    }

    const INI: &[u8] = b"[Sound]\nnickname = a\nfile = a.wav\nattenuation = 7\nrange = 10\n\
        [Sound]\nnickname = b\nfile = missing.wav\nattenuation = -120\nrange = -1, 5\n\
        type = music\npersistent = space\nis_2d = true\n\
        [Sound]\nnickname = c\nfile = c.wav\nlod1 = a\nlod2 = NONE\ncrv_pitch = 3\n\
        [Sound]\nfile = nobody.wav\n";

    #[test]
    fn the_table_reads_its_keys_as_the_reference() {
        let mut wavs = Wavs::default();
        let mut t = SoundArchTable::new();
        assert_eq!(t.load_ini(INI, false, &mut wavs), 3);
        let a = t.get(create_id(b"a")).unwrap();
        assert_eq!(
            (a.attenuation(), a.range_min, a.range_max),
            (0, 10.0, 300.0)
        );
        let b = t.get(create_id(b"b")).unwrap();
        assert_eq!(b.attenuation(), -96);
        assert_eq!((b.range_min, b.range_max), (-1.0, -1.0));
        assert_eq!((b.source_type(), b.persistence(), b.is_2d()), (3, 1, true));
        let c = t.get(create_id(b"c")).unwrap();
        assert_eq!(c.lod, [create_id(b"a"), none_id()]);
        assert_eq!(c.flags & 0xFF00, 0x300);
        assert!(c.pitch_bendable());
    }

    #[test]
    fn uses_make_and_destroy_the_mixer_archetype() {
        let mut wavs = Wavs::default();
        let mut t = SoundArchTable::new();
        t.load_ini(INI, false, &mut wavs);
        let a = create_id(b"a");
        assert!(t.acquire(a, &mut wavs));
        assert!(t.acquire(a, &mut wavs));
        assert_eq!(t.sm_arch_id(a, &mut wavs), 1);
        assert_eq!(t.release(a, &mut wavs), 1);
        assert_eq!(t.release(a, &mut wavs), 0);
        assert_eq!(wavs.log, ["create 1", "destroy 1"]);
        assert_eq!(t.sm_arch_id(a, &mut wavs), -1);
        // A missing file: the use counts, the answer is false.
        let b = create_id(b"b");
        assert!(!t.acquire(b, &mut wavs));
        assert_eq!(t.use_count(b, &mut wavs), 1);
    }

    #[test]
    fn the_level_of_detail_moves_the_uses() {
        let mut wavs = Wavs::default();
        let mut t = SoundArchTable::new();
        t.load_ini(INI, false, &mut wavs);
        let (a, c) = (create_id(b"a"), create_id(b"c"));
        assert!(t.acquire(c, &mut wavs));
        t.set_lod(1, &mut wavs);
        // c's use moved to a.
        assert_eq!(t.get(c).unwrap().linked, Some(a));
        assert_eq!(t.get(a).unwrap().use_count, 1);
        assert_eq!(t.get(c).unwrap().use_count, 0);
        t.set_lod(2, &mut wavs);
        assert_eq!(t.get(c).unwrap().linked, None);
        assert_eq!(t.sm_arch_id(c, &mut wavs), -1);
        assert!(t.acquire(c, &mut wavs));
        assert_eq!(t.get(c).unwrap().unlinked_uses, 1);
    }
}
