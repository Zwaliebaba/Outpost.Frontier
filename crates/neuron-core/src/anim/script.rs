//! Animation scripts: `FAnimScript` and its loaders in
//! `CppSource/SceneRuntime/SceneRuntime.cpp` (`AnimationOpenScript`,
//! `LoadAnimScriptNames`, `LoadAnimScriptsByNameList`,
//! `LoadAnimScriptMaps`, `ReadUtfNameCrc`, `LoadChannelNode`,
//! `AnimationRemoveScriptNames`, `AnimationEnumFsScripts`).
//!
//! A script maps the case-folded CRC of each animation name
//! ([`name_crc32`]) to a [`ScriptEntry`], visited in ascending key order as
//! the reference's `std::map` is. Two names with the same CRC (names that
//! differ only in case, for instance) share one entry: the second load
//! renames it, appends its tracks and may replace its longest channel.
//!
//! Names compare as the reference compares them: `Root height` exactly,
//! the map prefixes `Joint map`, `Object map` and `Event map` as
//! case-sensitive prefixes, and file system lookups (`Channel`, `Parent
//! name`) through the file system's own rules.

use std::collections::BTreeMap;

use crate::diag::{DumpFlags, dump};
use crate::hash::{c_str, name_crc32};
use crate::vfs::{ATTRIBUTE_DIRECTORY, FileSystem, FsDesc};

use super::channel::{LoadedChannel, load_channel, read_whole};
use super::{AnimError, lift};

const ANIMATION: &[u8] = b"Animation";
const SCRIPT: &[u8] = b"Script";
const PARENT_DIR: &[u8] = b"..";
const ALL: &[u8] = b"*";
const ROOT_HEIGHT: &[u8] = b"Root height";
const JOINT_MAP: &[u8] = b"Joint map";
const OBJECT_MAP: &[u8] = b"Object map";
const EVENT_MAP: &[u8] = b"Event map";
const PARENT_NAME: &[u8] = b"Parent name";
const CHILD_NAME: &[u8] = b"Child name";
const CHANNEL: &[u8] = b"Channel";

/// Which map a track came from. The reference does not keep it; the port
/// records it from the directory name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MapKind {
    /// A directory whose name starts with `Joint map`.
    Joint,
    /// A directory whose name starts with `Object map`.
    Object,
    /// Any other directory except `Event map*`: a track with no names and
    /// no channel.
    Other,
}

/// A `Parent name` or `Child name` file: the string and its CRC.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MapName {
    /// The bytes before the first NUL of the file. A file with no NUL
    /// gives all its bytes and an empty file nothing (the reference hashes
    /// past its buffer there; divergence register).
    pub name: Vec<u8>,
    /// [`name_crc32`] of `name`.
    pub crc: u32,
}

/// One map of an animation (`FAnimTrack`).
#[derive(Clone, Debug, PartialEq)]
pub struct Track {
    /// The map directory's name (not kept by the reference).
    pub map_name: Vec<u8>,
    /// The kind of map.
    pub kind: MapKind,
    /// `Parent name`, when the map has one and is a joint or object map.
    pub parent: Option<MapName>,
    /// `Child name`, when the map has one and is a joint map.
    pub child: Option<MapName>,
    /// The channel archetype, when the map has a `Channel` node it could
    /// open. A failed load is kept, as the reference keeps the archetype.
    pub channel: Option<LoadedChannel>,
}

impl Track {
    fn new(map_name: &[u8], kind: MapKind) -> Self {
        Self {
            map_name: map_name.to_vec(),
            kind,
            parent: None,
            child: None,
            channel: None,
        }
    }

    /// The reference's `ParentCrc`: 0 without a parent name.
    #[must_use]
    pub fn parent_crc(&self) -> u32 {
        self.parent.as_ref().map_or(0, |n| n.crc)
    }

    /// The reference's `ChildCrc`: 0 without a child name.
    #[must_use]
    pub fn child_crc(&self) -> u32 {
        self.child.as_ref().map_or(0, |n| n.crc)
    }
}

/// One animation (`FAnimNameEntry`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ScriptEntry {
    /// The name it was last loaded under.
    pub name: Vec<u8>,
    /// The `Root height` value: `Some` once the file was opened, with the
    /// bytes one 4-byte read copied over the previous value (initially 0).
    pub root_height: Option<f32>,
    /// The tracks in file order.
    pub tracks: Vec<Track>,
    /// The index in `tracks` of the channel with the longest duration (the
    /// first of equal ones) in the last load that found one.
    pub longest: Option<usize>,
}

impl ScriptEntry {
    /// The longest channel (`GetNamedDuration` reads its duration).
    #[must_use]
    pub fn longest_channel(&self) -> Option<&LoadedChannel> {
        self.tracks.get(self.longest?)?.channel.as_ref()
    }

    /// `LoadAnimScriptMaps(name, fs)`.
    fn load_maps(&mut self, name: &[u8], fs: &dyn FileSystem) -> Result<(), AnimError> {
        self.name = name.to_vec();
        let Ok(found) = lift(fs.find(ALL))? else {
            return Ok(());
        };
        let mut best = -1.0f32;
        for item in found {
            let name = item.data.name.as_slice();
            if item.data.attributes & ATTRIBUTE_DIRECTORY == 0 {
                if name != ROOT_HEIGHT {
                    continue;
                }
                let desc = FsDesc::read(name).with_found(item.token);
                let Ok(mut file) = lift(fs.open_file(&desc))? else {
                    continue;
                };
                let (_, bytes) = read_whole(&mut file, 4)?;
                let mut value = self.root_height.unwrap_or(0.0).to_le_bytes();
                value[..bytes.len()].copy_from_slice(&bytes);
                self.root_height = Some(f32::from_le_bytes(value));
                continue;
            }
            if name.first() == Some(&b'.') {
                continue;
            }
            let desc = FsDesc::read(name).with_found(item.token);
            let map_fs = lift(fs.create_child(&desc))?;
            if name.starts_with(EVENT_MAP) {
                continue;
            }
            let kind = if name.starts_with(JOINT_MAP) {
                MapKind::Joint
            } else if name.starts_with(OBJECT_MAP) {
                MapKind::Object
            } else {
                MapKind::Other
            };
            let mut track = Track::new(name, kind);
            // A map the file system could not open stays empty: the
            // reference calls through a null file system there.
            if let (Ok(mut map_fs), MapKind::Joint | MapKind::Object) = (map_fs, kind) {
                track.parent = read_name(map_fs.as_ref(), PARENT_NAME)?;
                if kind == MapKind::Joint {
                    track.child = read_name(map_fs.as_ref(), CHILD_NAME)?;
                }
                track.channel = load_channel_node(map_fs.as_mut())?;
            }
            if let Some(channel) = &track.channel {
                let duration = channel.duration();
                if best < duration {
                    self.longest = Some(self.tracks.len());
                    best = duration;
                }
            }
            self.tracks.push(track);
        }
        Ok(())
    }
}

/// `ReadUtfNameCrc(fs, name)`: `None` when the file cannot be opened.
fn read_name(fs: &dyn FileSystem, file: &[u8]) -> Result<Option<MapName>, AnimError> {
    let Ok(mut handle) = lift(fs.open_file(&FsDesc::read(file)))? else {
        return Ok(None);
    };
    let size = lift(handle.size())?.unwrap_or(0);
    let (_, bytes) = read_whole(&mut handle, size)?;
    let name = c_str(&bytes).to_vec();
    Ok(Some(MapName {
        crc: name_crc32(&name),
        name,
    }))
}

/// `LoadChannelNode(fs)`.
fn load_channel_node(fs: &mut dyn FileSystem) -> Result<Option<LoadedChannel>, AnimError> {
    if lift(fs.open_node(CHANNEL))?.is_err() {
        return Ok(None);
    }
    let channel = load_channel(fs)?;
    let _ = lift(fs.open_node(PARENT_DIR))?;
    Ok(Some(channel))
}

/// A report `AddScriptNames` or `RemoveScriptNames` makes through `FDUMP`
/// with the severity "warning".
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ScriptWarning {
    /// "Null name in animation sub set. Aborting subset load."
    NullName,
    /// "Unknown or old style animation": the name is not a directory.
    NotADirectory(Vec<u8>),
    /// "Failed to open animation": the file system for it was not created.
    OpenFailed(Vec<u8>),
    /// "Failed to remove animation": no entry has the name's CRC.
    RemoveFailed(Vec<u8>),
}

impl ScriptWarning {
    fn report(&self) {
        let message = match self {
            Self::NullName => "Null name in animation sub set. Aborting subset load.\n".to_owned(),
            Self::NotADirectory(name) => format!(
                "Unknown or old style animation \"{}\". Skipping.\n",
                String::from_utf8_lossy(name)
            ),
            Self::OpenFailed(name) => format!(
                "Failed to open animation \"{}\". Skipping.\n",
                String::from_utf8_lossy(name)
            ),
            Self::RemoveFailed(name) => format!(
                "Failed to remove animation \"{}\".\n",
                String::from_utf8_lossy(name)
            ),
        };
        dump(DumpFlags::WARNING, &message);
    }
}

/// An animation script (`FAnimScript`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AnimScript {
    entries: BTreeMap<u32, ScriptEntry>,
}

impl AnimScript {
    /// `CreateEmptyScript`.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `OpenScript(fs)`: every animation under `Animation\Script` of `fs`.
    /// `None` (the reference's invalid script, -1) when `fs` cannot open
    /// `Animation`. Leaves `fs` in the state the reference leaves it.
    ///
    /// # Errors
    ///
    /// The archive is malformed.
    pub fn open(fs: &mut dyn FileSystem) -> Result<Option<Self>, AnimError> {
        if lift(fs.open_node(ANIMATION))?.is_err() {
            return Ok(None);
        }
        let mut script = Self::new();
        script.load_all(fs)?;
        let _ = lift(fs.open_node(PARENT_DIR))?;
        Ok(Some(script))
    }

    /// `LoadAnimScriptNames`.
    fn load_all(&mut self, fs: &mut dyn FileSystem) -> Result<(), AnimError> {
        if lift(fs.open_node(SCRIPT))?.is_err() {
            return Ok(());
        }
        if let Ok(found) = lift(fs.find(ALL))? {
            for item in found {
                let name = item.data.name.as_slice();
                if name.first() == Some(&b'.') {
                    continue;
                }
                let desc = FsDesc::read(name).with_found(item.token);
                let nested = lift(fs.create_child(&desc))?;
                if item.data.attributes & ATTRIBUTE_DIRECTORY == 0 {
                    continue;
                }
                let entry = self.entries.entry(name_crc32(name)).or_default();
                match nested {
                    Ok(nested) => entry.load_maps(name, nested.as_ref())?,
                    // The reference walks a null file system here.
                    Err(_) => entry.name = name.to_vec(),
                }
            }
        }
        let _ = lift(fs.open_node(PARENT_DIR))?;
        Ok(())
    }

    /// `AddScriptNames(fs, names)`: the named animations under
    /// `Animation\Script` of `fs` that the script does not have yet. A
    /// `None` name ends the list. `None` (the reference's -1) when `fs`
    /// cannot open `Animation`; otherwise the warnings reported.
    ///
    /// # Errors
    ///
    /// The archive is malformed.
    pub fn add_names(
        &mut self,
        fs: &mut dyn FileSystem,
        names: &[Option<&[u8]>],
    ) -> Result<Option<Vec<ScriptWarning>>, AnimError> {
        if lift(fs.open_node(ANIMATION))?.is_err() {
            return Ok(None);
        }
        let mut warnings = Vec::new();
        if lift(fs.open_node(SCRIPT))?.is_ok() {
            for name in names {
                let Some(name) = name.map(c_str) else {
                    warnings.push(ScriptWarning::NullName);
                    break;
                };
                let attributes = lift(fs.path_attributes(name))?.unwrap_or(0);
                if attributes & ATTRIBUTE_DIRECTORY == 0 {
                    warnings.push(ScriptWarning::NotADirectory(name.to_vec()));
                    continue;
                }
                let crc = name_crc32(name);
                if self.entries.contains_key(&crc) {
                    continue;
                }
                match lift(fs.create_child(&FsDesc::read(name)))? {
                    Ok(nested) => self
                        .entries
                        .entry(crc)
                        .or_default()
                        .load_maps(name, nested.as_ref())?,
                    Err(_) => warnings.push(ScriptWarning::OpenFailed(name.to_vec())),
                }
            }
            let _ = lift(fs.open_node(PARENT_DIR))?;
        }
        let _ = lift(fs.open_node(PARENT_DIR))?;
        for warning in &warnings {
            warning.report();
        }
        Ok(Some(warnings))
    }

    /// `RemoveScriptNames(names)`: `None` names are skipped. Returns the
    /// warnings reported for names the script does not have.
    pub fn remove_names(&mut self, names: &[Option<&[u8]>]) -> Vec<ScriptWarning> {
        let mut warnings = Vec::new();
        for name in names.iter().flatten() {
            let name = c_str(name);
            if self.entries.remove(&name_crc32(name)).is_none() {
                let warning = ScriptWarning::RemoveFailed(name.to_vec());
                warning.report();
                warnings.push(warning);
            }
        }
        warnings
    }

    /// `EnumFsScripts(fs)`: the directories under `Animation\Script` of
    /// `fs`, in find order, except names starting with `.`. `None` (the
    /// reference's -1) when either directory cannot be opened.
    ///
    /// # Errors
    ///
    /// The archive is malformed.
    pub fn enum_fs_scripts(fs: &mut dyn FileSystem) -> Result<Option<Vec<Vec<u8>>>, AnimError> {
        if lift(fs.open_node(ANIMATION))?.is_err() {
            return Ok(None);
        }
        if lift(fs.open_node(SCRIPT))?.is_err() {
            let _ = lift(fs.open_node(PARENT_DIR))?;
            return Ok(None);
        }
        let mut names = Vec::new();
        if let Ok(found) = lift(fs.find(ALL))? {
            for item in found {
                if item.data.name.first() != Some(&b'.')
                    && item.data.attributes & ATTRIBUTE_DIRECTORY != 0
                {
                    names.push(item.data.name);
                }
            }
        }
        let _ = lift(fs.open_node(PARENT_DIR))?;
        let _ = lift(fs.open_node(PARENT_DIR))?;
        Ok(Some(names))
    }

    /// The entries in the reference's order, ascending by name CRC.
    pub fn entries(&self) -> impl Iterator<Item = (u32, &ScriptEntry)> {
        self.entries.iter().map(|(&crc, entry)| (crc, entry))
    }

    /// The entry for `name` (`FindAnimName(GetCRC32(name))`).
    #[must_use]
    pub fn get(&self, name: &[u8]) -> Option<&ScriptEntry> {
        self.entries.get(&name_crc32(name))
    }

    /// The entry with name CRC `crc`.
    #[must_use]
    pub fn get_by_crc(&self, crc: u32) -> Option<&ScriptEntry> {
        self.entries.get(&crc)
    }

    /// `GetScriptNameCount`.
    #[must_use]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Whether the script has no entries.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// `GetNamedDuration(name)`: the longest channel's duration, `None`
    /// for a missing name. The reference calls through a null channel for
    /// an entry without one; the port answers `None` there too.
    #[must_use]
    pub fn named_duration(&self, name: &[u8]) -> Option<f32> {
        Some(self.get(name)?.longest_channel()?.duration())
    }
}
