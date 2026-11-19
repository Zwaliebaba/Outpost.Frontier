//! The script engine: `FScriptEngine` of `SceneScript.cpp`, the object the
//! game creates for each scene script. It owns the scene ([`Player`]) and
//! adds what the scene does not hold: the COM context (`Slot40SetCom`),
//! loading from a file system (`Slot4Load`) or a path (`Slot43LoadPath`),
//! saving through the save parser (`Slot5Save`, `Slot42SetSaveParser`) or
//! to a path (`Slot44SavePath`), and the last path either was given.
//!
//! The parser's module-lifetime state (the Lua state and the published
//! entity handles) is the caller's [`ScriptModule`], which every engine of
//! a process shares, as every script engine of `SceneScript.dll` shares
//! the module's. The component manager that makes file systems for paths
//! is a [`FileSystemFactory`]; [`host_file_systems`] is the reference's
//! (`ReadFile.dll`'s factories over the host).

use std::cell::RefCell;
use std::rc::Rc;

use neuron_core::vfs::{
    self, FileSystem, FsDesc, GENERIC_READ, GENERIC_WRITE, Host, OPEN_EXISTING, VfsError,
};

use super::build::{BuildState, build};
use super::load::load;
use super::playback::{EventRef, Player};
use super::vm::Vm;

/// `GR_OK` and `GR_GENERIC`.
const OK: i32 = 0;
const GENERIC: i32 = -1;
/// `CREATE_ALWAYS`, the disposition `Slot44SavePath` asks for.
pub const CREATE_ALWAYS: u32 = 2;

/// `ISceneScriptComContext`: the game's object that keeps the text of
/// user events, which the scene does not store. The parser hands a user
/// event's `user_event_string` to it after adding the event; the writer
/// asks it for the text again when it saves the event.
pub trait ComContext {
    /// `Slot3`: the text of a user event the parser added (`None` for a
    /// value that is neither a string nor a number). The event's id is the
    /// engine's, -1 when the engine refused the event.
    fn set_user_event_string(&mut self, event: EventRef<'_>, text: Option<&[u8]>);

    /// `Slot5`: the size of the buffer the writer hands to
    /// [`Self::user_event_string`]; 0 writes no text.
    fn user_event_string_size(&mut self, event: EventRef<'_>) -> u32;

    /// `Slot4`: fills `buffer` (`size + 1` bytes, zero-filled here) with
    /// the event's text; the writer writes it, up to its first NUL, when
    /// the result is `GR_OK` (0).
    fn user_event_string(&mut self, event: EventRef<'_>, buffer: &mut [u8], size: u32) -> i32;
}

/// The parser's module-lifetime state: the Lua state the scripts run in
/// and the entity handles they publish (`G_LuaEntityHandles`).
#[derive(Default)]
pub struct ScriptModule {
    pub vm: Vm,
    pub state: BuildState,
}

impl ScriptModule {
    /// A fresh module (`SceneScriptLuaOpen`).
    #[must_use]
    pub fn new() -> ScriptModule {
        ScriptModule::default()
    }
}

/// `ISceneScriptSaveParser`: what `Slot5Save` hands the scene to. The
/// engine gives it the counts and the duration, then calls
/// [`Self::save`].
pub trait SaveParser {
    fn set_entity_count(&mut self, count: u32);
    fn set_event_count(&mut self, count: u32);
    fn set_duration(&mut self, ms: u32);
    /// Writes the scene to `fs`; the result is `Slot5Save`'s.
    fn save(
        &mut self,
        fs: &mut dyn FileSystem,
        player: &mut Player,
        com: Option<&mut dyn ComContext>,
    ) -> i32;
}

/// `FSceneScriptDefaultSaveParser`: the save parser `Slot5Save` makes for
/// the call when none is set. It refuses a scene without entities and
/// without a duration, and otherwise writes the scene
/// ([`Player::save_to`]) and succeeds whatever the writes did.
#[derive(Clone, Copy, Debug, Default)]
pub struct DefaultSaveParser {
    entities: u32,
    events: u32,
    duration_ms: u32,
}

impl SaveParser for DefaultSaveParser {
    fn set_entity_count(&mut self, count: u32) {
        self.entities = count;
    }

    fn set_event_count(&mut self, count: u32) {
        self.events = count;
    }

    fn set_duration(&mut self, ms: u32) {
        self.duration_ms = ms;
    }

    fn save(
        &mut self,
        fs: &mut dyn FileSystem,
        player: &mut Player,
        com: Option<&mut dyn ComContext>,
    ) -> i32 {
        if self.entities == 0 && self.duration_ms == 0 {
            return GENERIC;
        }
        player.save_to(com, self.duration_ms, &mut |bytes| {
            fs.write(bytes).unwrap_or(0)
        });
        OK
    }
}

/// The component manager's `CreateInstance` for a `FileSystem`
/// descriptor, as `Slot43LoadPath` and `Slot44SavePath` call it.
pub type FileSystemFactory<'a> = dyn Fn(&FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError> + 'a;

/// The reference's file-system factories over `host`
/// ([`vfs::create_file_system`]): they open files for reading only, so
/// `Slot44SavePath` gets no file system from them.
pub fn host_file_systems(
    host: &Host,
) -> impl Fn(&FsDesc<'_>) -> Result<Box<dyn FileSystem>, VfsError> + '_ {
    move |desc| vfs::create_file_system(host, desc)
}

/// A factory's failure as the `GENRESULT` it returns.
fn result_code(error: VfsError) -> i32 {
    let code = error.code().cast_signed();
    if code < 0 { code } else { GENERIC }
}

/// `FScriptEngine`.
#[derive(Default)]
pub struct ScriptEngine {
    player: Player,
    com: Option<Rc<RefCell<dyn ComContext>>>,
    save_parser: Option<Box<dyn SaveParser>>,
    /// The `+0xc` string: the last path given to `Slot43LoadPath` (when not
    /// null) or `Slot44SavePath`.
    script_path: Option<Vec<u8>>,
}

impl ScriptEngine {
    /// An engine with an empty scene and no COM context.
    #[must_use]
    pub fn new() -> ScriptEngine {
        ScriptEngine::default()
    }

    /// The scene.
    #[must_use]
    pub fn player(&self) -> &Player {
        &self.player
    }

    /// The scene, for the slots that drive it.
    pub fn player_mut(&mut self) -> &mut Player {
        &mut self.player
    }

    /// `Slot40SetCom`: replaces the COM context (`None` removes it); 0 when
    /// the engine has one afterwards, else -1. The reference counts its
    /// hold twice and releases twice; here the engine holds one `Rc`.
    pub fn set_com(&mut self, com: Option<Rc<RefCell<dyn ComContext>>>) -> i32 {
        self.com = com;
        if self.com.is_some() { OK } else { GENERIC }
    }

    /// The COM context.
    #[must_use]
    pub fn com(&self) -> Option<&Rc<RefCell<dyn ComContext>>> {
        self.com.as_ref()
    }

    /// `Slot42SetSaveParser`: the parser `Slot5Save` uses, `None` for the
    /// default one made per call. Always 0.
    pub fn set_save_parser(&mut self, parser: Option<Box<dyn SaveParser>>) -> i32 {
        self.save_parser = parser;
        OK
    }

    /// The last path given to [`Self::load_path`] or [`Self::save_path`].
    #[must_use]
    pub fn script_path(&self) -> Option<&[u8]> {
        self.script_path.as_deref()
    }

    /// `Slot4Load`: clears a loaded scene, then, with a file system, reads
    /// the whole file and runs the parser's two passes (`Load`, `Build`)
    /// into the scene, the COM context receiving the user events' text.
    /// 0 without a file system or when the first pass succeeds; -1 when the
    /// file is empty or short or the first pass fails, and where the
    /// reference faults in the second pass (divergence D240).
    pub fn load(&mut self, module: &mut ScriptModule, fs: Option<&mut dyn FileSystem>) -> i32 {
        if self.player.state() != 0 {
            self.player.clear();
        }
        let Some(fs) = fs else {
            return OK;
        };
        let Some(bytes) = read_whole(fs) else {
            return GENERIC;
        };
        let Ok(summary) = load(&mut module.vm, &bytes) else {
            return GENERIC;
        };
        let Ok(scene) = build(
            &mut module.vm,
            &mut module.state,
            &bytes,
            summary,
            self.com.is_some(),
        ) else {
            return GENERIC;
        };
        let mut com = self.com.as_ref().map(|c| c.borrow_mut());
        // `as_deref_mut` keeps the object's `'static` bound, which the
        // reborrow cannot meet behind `&mut`.
        #[allow(clippy::option_as_ref_deref)]
        let com = com.as_mut().map(|c| -> &mut dyn ComContext { &mut **c });
        self.player.load_with_com(&scene, summary.duration_ms, com);
        OK
    }

    /// `Slot5Save`: hands the scene to the save parser with the live
    /// entity count, the event count and the duration; its result, or -1
    /// without a file system.
    pub fn save(&mut self, fs: Option<&mut dyn FileSystem>) -> i32 {
        let Some(fs) = fs else {
            return GENERIC;
        };
        let mut default = DefaultSaveParser::default();
        let parser: &mut dyn SaveParser = match self.save_parser.as_deref_mut() {
            Some(parser) => parser,
            None => &mut default,
        };
        parser.set_entity_count(self.player.live_entity_count());
        parser.set_event_count(self.player.event_count());
        parser.set_duration(self.player.duration().cast_unsigned());
        let mut com = self.com.as_ref().map(|c| c.borrow_mut());
        // `as_deref_mut` keeps the object's `'static` bound, which the
        // reborrow cannot meet behind `&mut`.
        #[allow(clippy::option_as_ref_deref)]
        let com = com.as_mut().map(|c| -> &mut dyn ComContext { &mut **c });
        parser.save(fs, &mut self.player, com)
    }

    /// `Slot43LoadPath`: keeps the path and loads the file the factory
    /// opens for reading there ([`Self::load`]); a null path loads from no
    /// file system. The factory's failure is the result.
    pub fn load_path(
        &mut self,
        module: &mut ScriptModule,
        path: Option<&[u8]>,
        factory: &FileSystemFactory<'_>,
    ) -> i32 {
        let mut fs = None;
        if let Some(path) = path {
            self.script_path = Some(path.to_vec());
            let desc = FsDesc {
                implementation: None,
                path,
                access: GENERIC_READ,
                creation: OPEN_EXISTING,
                found: None,
            };
            match factory(&desc) {
                Ok(created) => fs = Some(created),
                Err(e) => return result_code(e),
            }
        }
        // `as_deref_mut` keeps the object's `'static` bound, which the
        // reborrow cannot meet behind `&mut`.
        #[allow(clippy::option_as_ref_deref)]
        let fs = fs.as_mut().map(|fs| -> &mut dyn FileSystem { &mut **fs });
        self.load(module, fs)
    }

    /// `Slot44SavePath`: keeps the path and saves to the file system the
    /// factory makes for reading and writing, created anew
    /// ([`Self::save`]). When the factory fails the reference releases the
    /// null pointer it got; here its failure is the result (divergence
    /// D545).
    pub fn save_path(&mut self, path: &[u8], factory: &FileSystemFactory<'_>) -> i32 {
        self.script_path = Some(path.to_vec());
        let desc = FsDesc {
            implementation: None,
            path,
            access: GENERIC_READ | GENERIC_WRITE,
            creation: CREATE_ALWAYS,
            found: None,
        };
        match factory(&desc) {
            Ok(mut fs) => self.save(Some(&mut *fs)),
            Err(e) => result_code(e),
        }
    }
}

/// `FSceneScriptLuaLoadParser::Load`'s read: the size, then that many
/// bytes; `None` for an empty file or a short read.
fn read_whole(fs: &mut dyn FileSystem) -> Option<Vec<u8>> {
    let size = fs.size().ok()?;
    if size < 1 {
        return None;
    }
    let mut bytes = vec![0u8; size as usize];
    match fs.read(&mut bytes) {
        Ok(read) if read == bytes.len() => Some(bytes),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::vfs::{MemFileSystem, MemSource};

    struct Silent;

    impl ComContext for Silent {
        fn set_user_event_string(&mut self, _: EventRef<'_>, _: Option<&[u8]>) {}
        fn user_event_string_size(&mut self, _: EventRef<'_>) -> u32 {
            0
        }
        fn user_event_string(&mut self, _: EventRef<'_>, _: &mut [u8], _: u32) -> i32 {
            GENERIC
        }
    }

    #[test]
    fn set_com_reports_whether_a_context_is_held() {
        let mut engine = ScriptEngine::new();
        assert_eq!(engine.set_com(Some(Rc::new(RefCell::new(Silent)))), OK);
        assert!(engine.com().is_some());
        assert_eq!(engine.set_com(None), GENERIC);
        assert!(engine.com().is_none());
    }

    /// The default save parser refuses a scene without entities and without
    /// a duration; with a duration it writes the empty tables.
    #[test]
    fn an_empty_scene_saves_only_with_a_duration() {
        let mut engine = ScriptEngine::new();
        assert_eq!(engine.save(None), GENERIC);
        let mut target = MemFileSystem::new(
            None,
            GENERIC_READ | GENERIC_WRITE,
            CREATE_ALWAYS,
            MemSource::Copy(None),
        );
        assert_eq!(engine.save(Some(&mut target)), GENERIC);
        assert_eq!(target.logical_size(), 0);
        engine.player_mut().set_duration(1500);
        assert_eq!(engine.save(Some(&mut target)), OK);
        let text = &target.buffer()[..target.logical_size() as usize];
        assert_eq!(
            text,
            b"duration = 1.500;\r\n\r\nentities =\r\n{\r\n\r\n};\r\n\r\nevents =\r\n{\r\n};\r\n"
        );
    }

    /// The host's factories open files for reading only: a load of a
    /// missing file fails as the DOS factory does, and a save to a path
    /// gets no file system, where the reference releases a null pointer
    /// (divergence D545).
    #[test]
    fn paths_through_the_host_file_systems() {
        let dir = std::env::temp_dir().join(format!("scene-script-paths-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let host = Host::new(&dir);
        let factory = host_file_systems(&host);
        let mut module = ScriptModule::new();
        let mut engine = ScriptEngine::new();
        engine.player_mut().set_duration(1000);
        assert_eq!(engine.save_path(b"saved.thn", &factory), GENERIC);
        assert_eq!(engine.script_path(), Some(&b"saved.thn"[..]));
        assert!(!dir.join("saved.thn").exists());
        let missing = engine.load_path(&mut module, Some(b"missing.thn"), &factory);
        assert_eq!(missing, VfsError::OpenFailed.code().cast_signed());
        assert_eq!(engine.script_path(), Some(&b"missing.thn"[..]));
        assert_eq!(engine.load_path(&mut module, None, &factory), OK);
        assert_eq!(engine.script_path(), Some(&b"missing.thn"[..]));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
