//! The client's star system (`star_sys.cpp`; work package P5.4a): the
//! system file the client reads when it enters a system
//! (`StarSysLoadIni`), as data the space state and the other streams read.
//!
//! `StarSysLoadIni` reads the system file section by section and hands
//! each to its owner at once: the space and ambient colours, the far clip,
//! the system music, the archetype, sound and voice preloads, the star
//! sphere, the dust, the light sources, the objects, the texture panels,
//! the asteroid fields and the nebulae. The port reads the file into
//! [`StarSystemFile`] in the same order and with the same rules (the last
//! value of a key wins where the C++ overwrites, every section kept where
//! the C++ makes one object per section), and the owners take their part:
//!
//! - the environment (P5.8): [`StarSystemFile::background`],
//!   [`StarSystemFile::dust`], [`StarSystemFile::asteroids`],
//!   [`StarSystemFile::nebulae`], [`StarSystemFile::texture_panels`];
//! - the lights (P5.6): [`StarSystemFile::light_sources`], reader
//!   positions of the `[LightSource]` sections, which `GameLightLoad`
//!   reads;
//! - the system's solars (P5.4): [`StarSystemFile::objects`], the
//!   `[Object]` sections (`SpawnWorldSolarFromObjectSection`);
//! - the music (P5.5): [`StarSystemFile::music`];
//! - the preloads: [`StarSystemFile::preloads`].

use game_sim::data::DataSource;
use game_sim::object::World;

use crate::objects::{ClientObject, ClientObjectKind, ClientObjects};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32;

/// The C++ source the star system's reports name.
pub const STAR_SYS_CPP: &str = "Client\\space\\StarSys.cpp";

/// `TString<256>`: the asteroid and nebula file names keep 255 bytes.
pub const FILE_NAME_CAPACITY: usize = 0x100;

/// `TString<64>`: the zone names keep 63 bytes.
pub const ZONE_NAME_CAPACITY: usize = 0x40;

/// The system music (`[Music]`; `G_MusicSystemSpace`, `Danger`,
/// `Battle`), hashed with `CreateID`; 0 for one the file does not name.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SystemMusic {
    /// `space`.
    pub space: u32,
    /// `danger`.
    pub danger: u32,
    /// `battle`.
    pub battle: u32,
}

/// The archetype kinds `[Archetype]` preloads by name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PreloadKind {
    /// `ship` (`Archetype::GetShipByName`).
    Ship,
    /// `equipment`.
    Equipment,
    /// `simple`.
    Simple,
    /// `solar`.
    Solar,
    /// `asteroid`.
    Asteroid,
}

/// One `[Archetype]` entry, in file order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Preload {
    /// `snd`: the sound archetype preloaded (`PreloadClientSoundArch`).
    Sound(u32),
    /// `voice`: the voice and its message (`PreloadVoiceLine`).
    Voice {
        /// The voice.
        voice: u32,
        /// The message.
        message: u32,
    },
    /// An archetype by kind and nickname id, loaded with its companion.
    Archetype {
        /// The kind.
        kind: PreloadKind,
        /// `CreateID` of the nickname.
        id: u32,
    },
    /// Any other key: the C++ warns ("unknown (or old-style) preload
    /// archetype", then "Failed to preload archetype") and loads nothing.
    /// The line is kept. A known kind whose nickname names no archetype
    /// gets the second warning from the archetype lookup, which is the
    /// preloader's.
    Unknown(Vec<u8>),
}

/// The star sphere's layer names (`[Background]`; `FStarSphereBackground`
/// layers 1, 2 and 0). A `[Background]` section empties all three first.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Background {
    /// `basic_stars` (layer 1).
    pub basic_stars: Vec<u8>,
    /// `complex_stars`, or `file` (layer 2).
    pub complex_stars: Vec<u8>,
    /// `nebulae` (layer 0).
    pub nebulae: Vec<u8>,
}

/// An `[Asteroids]` or `[Nebula]` section's file and zone, bounded as the
/// C++'s `TString`s bound them.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ZoneFile {
    /// `file`.
    pub file: Vec<u8>,
    /// `zone`.
    pub zone: Vec<u8>,
}

/// The system file as `StarSysLoadIni` reads it.
#[derive(Clone, Debug, Default)]
pub struct StarSystemFile {
    /// The file's bytes, for the section readers that start from a
    /// position ([`Self::light_sources`], [`Self::objects`]).
    pub contents: Vec<u8>,
    /// `[System]` / `[SystemInfo]` `space_color` (`G_SpaceColor`), each
    /// channel truncated to its low byte; `None` when no section sets it.
    pub space_color: Option<[u8; 3]>,
    /// `space_farclip` (`SpaceSetFarClip`).
    pub far_clip: Option<f32>,
    /// `[Music]`.
    pub music: SystemMusic,
    /// `[Archetype]` entries, in order.
    pub preloads: Vec<Preload>,
    /// `[Ambient]` `color` (`G_StarSysAmbientRgb`).
    pub ambient: Option<[u8; 3]>,
    /// `[Background]`: the last section's layers.
    pub background: Option<Background>,
    /// `[Dust]` `spacedust`, for each section in order
    /// (`SpaceDustSetSystemDust`, called once per section; an empty name
    /// for a section without the key).
    pub dust: Vec<Vec<u8>>,
    /// `[LightSource]`: the reader position after each section's header.
    pub light_sources: Vec<u32>,
    /// `[Object]`: the reader position after each section's header.
    pub objects: Vec<u32>,
    /// `[TexturePanels]` `file` names, in order.
    pub texture_panels: Vec<Vec<u8>>,
    /// `[Asteroids]` sections, in order (`CliAsteroidCreateAndLoad`); the
    /// first also loads the system's common asteroids
    /// (`CmnAsteroid::Load`, once per file).
    pub asteroids: Vec<ZoneFile>,
    /// `[Nebula]` sections that named a `file`, in order. A section with
    /// several `file` keys makes a nebula for each and keeps the last
    /// (the earlier ones are dropped, as the C++ drops them).
    pub nebulae: Vec<ZoneFile>,
    /// The warnings the C++ writes while reading, without the source-file
    /// prefix.
    pub reports: Vec<String>,
}

impl StarSystemFile {
    /// `StarSysLoadIni(path)` through the data files: `None` when the file
    /// is missing or empty (the C++ warns "unknown system" and returns).
    #[must_use]
    pub fn load(source: &dyn DataSource, path: &[u8]) -> Option<Self> {
        Self::read(neuron_core::hash::c_str(path), source.read(path)?)
    }

    /// `StarSysLoadIni(path)` on the file's bytes; `None` when the reader
    /// cannot open them (the C++ warns "unknown system" and returns).
    #[must_use]
    pub fn read(path: &[u8], contents: Vec<u8>) -> Option<Self> {
        let mut reader = IniReader::new();
        if !reader.open(path, contents.clone()) {
            return None;
        }
        let mut file = Self {
            contents,
            ..Self::default()
        };
        while reader.read_header() {
            if reader.is_header(b"System") || reader.is_header(b"SystemInfo") {
                while reader.read_value() {
                    if reader.is_value(b"space_color") {
                        file.space_color = Some(color(&mut reader));
                    } else if reader.is_value(b"space_farclip") {
                        file.far_clip = Some(float(&mut reader, 0));
                    }
                }
            } else if reader.is_header(b"Music") {
                file.read_music(&mut reader, path);
            } else if reader.is_header(b"Archetype") {
                file.read_preloads(&mut reader);
            } else if reader.is_header(b"Ambient") {
                while reader.read_value() {
                    if reader.is_value(b"color") {
                        file.ambient = Some(color(&mut reader));
                    }
                }
            } else if reader.is_header(b"Background") {
                let mut background = Background::default();
                while reader.read_value() {
                    if reader.is_value(b"basic_stars") {
                        background.basic_stars = reader.value_string();
                    } else if reader.is_value(b"complex_stars") || reader.is_value(b"file") {
                        background.complex_stars = reader.value_string();
                    } else if reader.is_value(b"nebulae") {
                        background.nebulae = reader.value_string();
                    }
                }
                file.background = Some(background);
            } else if reader.is_header(b"Dust") {
                let mut dust = Vec::new();
                while reader.read_value() {
                    if reader.is_value(b"spacedust") {
                        dust = reader.value_string();
                    }
                }
                file.dust.push(dust);
            } else if reader.is_header(b"LightSource") {
                file.light_sources.push(reader.tell());
            } else if reader.is_header(b"Object") {
                file.objects.push(reader.tell());
            } else if reader.is_header(b"TexturePanels") {
                while reader.read_value() {
                    if reader.is_value(b"file") {
                        file.texture_panels.push(reader.value_string());
                    }
                }
            } else if reader.is_header(b"Asteroids") {
                file.asteroids.push(zone_file(&mut reader, None));
            } else if reader.is_header(b"Nebula") {
                let mut made = false;
                let nebula = zone_file(&mut reader, Some(&mut made));
                if made {
                    file.nebulae.push(nebula);
                }
            }
        }
        Some(file)
    }

    fn read_music(&mut self, reader: &mut IniReader, path: &[u8]) {
        while reader.read_value() {
            if reader.is_value(b"space") {
                self.music.space = create_id(&reader.value_string());
            } else if reader.is_value(b"danger") {
                self.music.danger = create_id(&reader.value_string());
            } else if reader.is_value(b"battle") {
                self.music.battle = create_id(&reader.value_string());
            } else if reader.is_value(b"explore") {
                self.reports.push(format!(
                    "*** WARNING: explore music is obsolete in system file {}, valid types are space, danger and battle",
                    String::from_utf8_lossy(path)
                ));
            }
        }
    }

    fn read_preloads(&mut self, reader: &mut IniReader) {
        while reader.read_value() {
            let value = reader.value_string();
            if reader.is_value(b"snd") {
                self.preloads.push(Preload::Sound(create_id(&value)));
                continue;
            }
            if reader.is_value(b"voice") {
                self.preloads.push(Preload::Voice {
                    voice: create_id(&reader.value_string_at(0)),
                    message: create_id(&reader.value_string_at(1)),
                });
                continue;
            }
            let kind = [
                (b"ship".as_slice(), PreloadKind::Ship),
                (b"equipment", PreloadKind::Equipment),
                (b"simple", PreloadKind::Simple),
                (b"solar", PreloadKind::Solar),
                (b"asteroid", PreloadKind::Asteroid),
            ]
            .into_iter()
            .find(|(name, _)| reader.is_value(name))
            .map(|(_, kind)| kind);
            match kind {
                Some(kind) => self.preloads.push(Preload::Archetype {
                    kind,
                    id: create_id(&value),
                }),
                None => {
                    let line = reader.line();
                    self.reports.push(format!(
                        "*** WARNING: unknown (or old-style) preload archetype\n  ({})",
                        String::from_utf8_lossy(&line)
                    ));
                    // No archetype was looked up, so the C++ also reports
                    // the failed preload.
                    self.reports.push(format!(
                        "*** WARNING: Failed to preload archetype\n  {}",
                        String::from_utf8_lossy(&line)
                    ));
                    self.preloads.push(Preload::Unknown(line));
                }
            }
        }
    }
}

/// The path of a system's file (`Universe::get_filename` of the system's
/// `file`); `None` for a system the universe does not have.
#[must_use]
pub fn system_file_path(world: &World, system: u32) -> Option<Vec<u8>> {
    let universe = &world.data().universe;
    let sys = universe.systems.get(&system)?;
    Some(universe.system_file_path(world.strings().get(sys.file)))
}

/// `StarSysEnter(system)`'s part over the world and the object database:
/// the system's physics started (`PhySys::Startup`, so creates are taken),
/// then `StarSysLoadIni`: the spatial set freed, every `[Object]` solar
/// made in file order (`SpawnWorldSolarFromObjectSection`; in single
/// player these are the solars the server's `load_solars` wraps) and
/// registered, and the spatial set rebuilt. The file as read is returned;
/// `None` when it cannot be opened ("unknown system"), after which the
/// set is rebuilt as the C++ does.
pub fn enter_system(
    world: &mut World,
    objects: &mut ClientObjects,
    system: u32,
) -> Option<StarSystemFile> {
    world.load_system(system);
    objects.physics_active = true;
    objects.free_spatial();
    let file = system_file_path(world, system).and_then(|path| {
        let source = world.data_source()?;
        StarSystemFile::load(source, &path)
    });
    if file.is_some() {
        for h in world.load_system_solars(system) {
            let Some(o) = world.get(h) else { continue };
            let object = ClientObject {
                id: o.simple.id,
                kind: ClientObjectKind::Solar,
                archetype: o.archetype,
                handle: Some(h),
                remote_physics: false,
            };
            objects.register(object, world);
        }
    }
    objects.rebuild_spatial(world);
    file
}

/// What [`SystemMusicState::update`] reads of the frame (`StarSysUpdate`'s
/// music part).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MusicFrame {
    /// The frame's `dt`.
    pub dt: f32,
    /// The background music source has finished (`ISoundSource::IsFinished`
    /// of `BindClientBackgroundMusic`); false without one.
    pub background_finished: bool,
    /// The music of the smallest zone with property 0x400000 at the
    /// player's position, when it names one (`GetSmallestZoneWithPropertiesAt`,
    /// `IZone::GetMusic`); read only without a space override.
    pub zone_music: Option<u32>,
    /// The player's game object's +0x158 word is set (its jump or dock
    /// state).
    pub player_jumping: bool,
    /// `MUSIC_CROSS_FADE_DELAY` (`constants.ini`, 30 by default).
    pub cross_fade_delay: f32,
}

/// A music change `StarSysUpdate` asks of the sound layer
/// (`SetClientBackgroundMusic(track, no_fade, fade)`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MusicChange {
    /// The track (0 is silence).
    pub track: u32,
    /// `G_MusicOverrideNoFade`.
    pub no_fade: bool,
    /// `G_MusicOverrideFade`.
    pub fade: f32,
}

/// `star_sys.cpp`'s music state: the system's tracks, the override, and
/// the space / danger / battle / override machine with its cross fade.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SystemMusicState {
    /// The system's tracks (`[Music]`).
    pub system: SystemMusic,
    /// `G_MusicSystemOverride`.
    pub system_override: u32,
    /// `G_MusicOverrideSpace`, `Danger`, `Battle`, `Track`.
    pub override_music: SystemMusic,
    /// `G_MusicOverrideTrack`.
    pub override_track: u32,
    /// `G_MusicOverrideNoFade`.
    pub override_no_fade: bool,
    /// `G_MusicOverrideFade`.
    pub override_fade: f32,
    /// `G_MusicState`: 0 space, 1 danger, 2 battle, 3 override, 4 silence.
    pub state: i32,
    /// `G_MusicTrack`: the track last asked for.
    pub track: u32,
    /// `G_bMusicCrossFading`.
    pub cross_fading: bool,
    /// `G_MusicCrossFadeTime`.
    pub cross_fade_time: f32,
    /// `G_bMusicBattleHit`: a hostile hit or target since the last frame.
    pub battle_hit: bool,
    /// `G_MusicDangerScanTime`.
    pub danger_scan_time: f32,
    /// `G_bMusicDanger`.
    pub danger: bool,
}

impl SystemMusicState {
    /// `StarSysEnter`'s music reset, with the system's tracks from its file.
    #[must_use]
    pub fn enter(system: SystemMusic) -> Self {
        Self {
            system,
            system_override: 0,
            override_music: SystemMusic::default(),
            override_track: 0,
            override_no_fade: true,
            override_fade: -1.0,
            state: 0,
            track: 0,
            cross_fading: false,
            cross_fade_time: 0.0,
            battle_hit: false,
            danger_scan_time: 1000.0,
            danger: false,
        }
    }

    /// `StarSysSetMusicOverride`.
    pub fn set_override(&mut self, music: SystemMusic, track: u32, fade: f32, no_fade_flag: bool) {
        self.override_music = music;
        self.override_track = track;
        self.override_fade = fade;
        self.override_no_fade = !no_fade_flag;
    }

    /// `StarSysClearMusicOverride`.
    pub fn clear_override(&mut self) {
        self.override_music = SystemMusic::default();
        self.override_track = 0;
        self.override_fade = -1.0;
        self.override_no_fade = true;
    }

    /// `StarSysNoteDamage` and `StarSysNoteTargetChange`'s outcome: a
    /// hostile hit or target this frame.
    pub fn note_battle(&mut self) {
        self.battle_hit = true;
    }

    /// `StarSysUpdate`'s music part. `scanner_sees_hostile` is asked only
    /// when the two-second danger scan is due (`ScannerSeesHostile`). The
    /// change to play, if the track differs from the last one.
    pub fn update(
        &mut self,
        frame: &MusicFrame,
        scanner_sees_hostile: impl FnOnce() -> bool,
    ) -> Option<MusicChange> {
        let pick = |o: u32, s: u32| if o != 0 { o } else { s };
        if frame.background_finished {
            self.clear_override();
        }
        let mut space = pick(self.override_music.space, self.system.space);
        let danger = pick(self.override_music.danger, self.system.danger);
        let battle = pick(self.override_music.battle, self.system.battle);
        let override_track = pick(self.override_track, self.system_override);
        if self.override_music.space == 0
            && let Some(zone) = frame.zone_music
        {
            space = zone;
        }
        let was_battle = self.state == 2;
        let battle_hit = self.battle_hit;
        self.battle_hit = false;

        // The sum is compared unrounded and stored as a float.
        let cross = if was_battle && battle_hit {
            self.cross_fading = true;
            Some(0.0 + f64::from(frame.dt))
        } else if self.cross_fading {
            Some(f64::from(self.cross_fade_time) + f64::from(frame.dt))
        } else {
            None
        };
        if let Some(sum) = cross {
            #[allow(clippy::cast_possible_truncation)]
            let stored = sum as f32;
            self.cross_fade_time = stored;
            if sum >= f64::from(frame.cross_fade_delay) {
                self.cross_fading = false;
                self.cross_fade_time = 0.0;
                self.danger_scan_time = 1000.0;
            }
        }
        if override_track != 0 {
            self.cross_fading = false;
        } else if self.cross_fading {
            return None;
        }

        let scan = f64::from(self.danger_scan_time) + f64::from(frame.dt);
        #[allow(clippy::cast_possible_truncation)]
        let scan_stored = scan as f32;
        self.danger_scan_time = scan_stored;
        if scan >= 2.0 {
            self.danger = scanner_sees_hostile();
            self.danger_scan_time = 0.0;
        }

        let mut state = if battle_hit {
            2
        } else if self.danger {
            1
        } else if frame.player_jumping {
            4
        } else {
            0
        };
        if override_track != 0 {
            state = 3;
        }
        if state != self.state {
            self.state = state;
            self.cross_fading = true;
            self.cross_fade_time = 0.0;
        }
        let track = match self.state {
            0 => space,
            1 => danger,
            2 => battle,
            3 => override_track,
            _ => 0,
        };
        (self.track != track).then(|| {
            self.track = track;
            MusicChange {
                track,
                no_fade: self.override_no_fade,
                fade: self.override_fade,
            }
        })
    }
}

/// `get_value_float(index)` stored to a `float`.
fn float(reader: &mut IniReader, index: u32) -> f32 {
    #[allow(clippy::cast_possible_truncation)]
    let value = reader.value_float(index) as f32;
    value
}

/// `FloatToByte` of the first three fields: `ftol`, then the low byte.
fn color(reader: &mut IniReader) -> [u8; 3] {
    let mut rgb = [0; 3];
    for (index, out) in (0u32..).zip(rgb.iter_mut()) {
        let [low, ..] = trunc_to_i32(f64::from(float(reader, index))).to_le_bytes();
        *out = low;
    }
    rgb
}

/// `CopyTruncated` into a `TString` of `capacity` bytes.
fn bounded(value: &[u8], capacity: usize) -> Vec<u8> {
    let end = value
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(value.len())
        .min(capacity - 1);
    value[..end].to_vec()
}

/// An `[Asteroids]` or `[Nebula]` section's `file` and `zone`, the last of
/// each kept; `made` is set when a `file` key was read (a nebula is made).
fn zone_file(reader: &mut IniReader, mut made: Option<&mut bool>) -> ZoneFile {
    let mut out = ZoneFile::default();
    while reader.read_value() {
        if reader.is_value(b"file") {
            out.file = bounded(&reader.value_string(), FILE_NAME_CAPACITY);
            if let Some(made) = made.as_deref_mut() {
                *made = true;
            }
        } else if reader.is_value(b"zone") {
            out.zone = bounded(&reader.value_string(), ZONE_NAME_CAPACITY);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    const SYSTEM: &[u8] = b"[SystemInfo]\nspace_color = 10.9, 300, -1\nspace_farclip = 25000\n\
[Music]\nspace = music_space\nexplore = old\n[Archetype]\nship = li_fighter\nsnd = boom\n\
voice = pilot, msg_1\nbogus = x\n[Ambient]\ncolor = 30, 40, 50\n[Background]\n\
basic_stars = a.cmp\nfile = b.cmp\n[Dust]\nspacedust = dust\n[Dust]\n[LightSource]\n\
nickname = l1\n[Object]\nnickname = o1\n[Object]\nnickname = o2\n[TexturePanels]\nfile = p1\n\
file = p2\n[Asteroids]\nfile = af.ini\nzone = z1\n[Nebula]\nzone = z2\n[Nebula]\nfile = n1\n\
file = n2\nzone = z3\n";

    #[test]
    fn the_sections_read_as_star_sys_load_ini_reads_them() {
        let f = StarSystemFile::read(b"test.ini", SYSTEM.to_vec()).expect("opens");
        // ftol then the low byte: 10.9 -> 10, 300 -> 44, -1 -> 255.
        assert_eq!(f.space_color, Some([10, 44, 255]));
        assert_eq!(f.far_clip, Some(25000.0));
        assert_eq!(f.music.space, create_id(b"music_space"));
        assert_eq!((f.music.danger, f.music.battle), (0, 0));
        assert_eq!(f.preloads.len(), 4);
        assert_eq!(
            f.preloads[0],
            Preload::Archetype {
                kind: PreloadKind::Ship,
                id: create_id(b"li_fighter")
            }
        );
        assert_eq!(f.preloads[1], Preload::Sound(create_id(b"boom")));
        assert!(matches!(f.preloads[3], Preload::Unknown(_)));
        assert_eq!(f.ambient, Some([30, 40, 50]));
        let bg = f.background.as_ref().expect("background");
        assert_eq!(
            (bg.basic_stars.as_slice(), bg.complex_stars.as_slice()),
            (b"a.cmp".as_slice(), b"b.cmp".as_slice())
        );
        assert_eq!(f.dust, [b"dust".to_vec(), Vec::new()]);
        assert_eq!((f.light_sources.len(), f.objects.len()), (1, 2));
        assert_eq!(f.texture_panels, [b"p1".to_vec(), b"p2".to_vec()]);
        assert_eq!(f.asteroids[0].file, b"af.ini");
        // The nebula without a file makes none; the one with two keeps the last.
        assert_eq!(f.nebulae.len(), 1);
        assert_eq!(
            (f.nebulae[0].file.as_slice(), f.nebulae[0].zone.as_slice()),
            (b"n2".as_slice(), b"z3".as_slice())
        );
        assert_eq!(f.reports.len(), 3);

        // An [Object] position reads that section's values.
        let mut reader = IniReader::new();
        assert!(reader.open(b"test.ini", f.contents.clone()));
        reader.seek(f.objects[1]);
        assert!(reader.read_value());
        assert_eq!(reader.value_string(), b"o2");
    }

    fn frame(dt: f32) -> MusicFrame {
        MusicFrame {
            dt,
            background_finished: false,
            zone_music: None,
            player_jumping: false,
            cross_fade_delay: 30.0,
        }
    }

    #[test]
    fn the_music_machine_switches_as_star_sys_update() {
        let tracks = SystemMusic {
            space: 1,
            danger: 2,
            battle: 3,
        };
        let mut m = SystemMusicState::enter(tracks);
        // First frame: the scan is due (1000 s), nothing hostile: space.
        let c = m.update(&frame(0.1), || false).expect("a change");
        assert_eq!((c.track, c.no_fade, c.fade), (1, true, -1.0));
        assert_eq!(m.update(&frame(0.1), || unreachable!()), None);

        // A hostile hit: battle at once, then the cross fade holds 30 s.
        m.note_battle();
        assert_eq!(m.update(&frame(0.1), || false).map(|c| c.track), Some(3));
        assert!(m.cross_fading);
        for _ in 0..299 {
            assert_eq!(m.update(&frame(0.1), || true), None);
        }
        // The fade ends, the scan is due again and sees a hostile: danger.
        assert_eq!(m.update(&frame(0.2), || true).map(|c| c.track), Some(2));

        // An override wins over the fade; the background ending clears it.
        m.set_override(SystemMusic::default(), 9, 2.5, true);
        let c = m.update(&frame(0.1), || true).expect("override");
        assert_eq!((c.track, c.no_fade, c.fade), (9, false, 2.5));
        let mut f = frame(0.1);
        f.background_finished = true;
        m.update(&f, || false);
        assert_eq!(m.override_track, 0);
    }

    #[test]
    fn zone_music_replaces_the_space_track() {
        let mut m = SystemMusicState::enter(SystemMusic {
            space: 1,
            ..SystemMusic::default()
        });
        let mut f = frame(0.1);
        f.zone_music = Some(7);
        assert_eq!(m.update(&f, || false).map(|c| c.track), Some(7));
    }

    #[test]
    fn names_are_bounded_as_the_tstrings() {
        let long = vec![b'a'; 300];
        assert_eq!(bounded(&long, FILE_NAME_CAPACITY).len(), 255);
        assert_eq!(bounded(b"ab\0cd", ZONE_NAME_CAPACITY), b"ab");
    }
}
