//! The client's startup configuration (`Client/startup_config.cpp`,
//! `LoadStartupConfig` and `ApplyDisplayCmdlineOverrides`; work package
//! P4.1): `OutpostFrontier.ini`'s data path, the three startup movies and
//! the `[Data]` paths, the `+Q`/`+X` switches, and the display switches
//! `+B`, `+F`, `+M`, `+W` applied to a display mode.
//!
//! The C++ also resets the game options to their defaults here
//! (`GGameOptionDescs`); the engine holds them at those defaults
//! (`neuron_client::engine::options`, D512), and nothing in the C++ saves
//! or loads them. The display mode the device takes is not this one but
//! `InitializeEngine`'s own block under `PerfOptions.ini`
//! ([`crate::render_mode`]).

use neuron_core::ini::IniReader;
use neuron_core::msvcrt::atoi;

/// `CopyBoundedString`'s capacity for the path fields (`0x104` bytes, the
/// terminator included).
pub const PATH_CAPACITY: usize = 0x104;

/// The number of startup movies the C++ keeps.
pub const MOVIE_SLOTS: usize = 3;

/// The configuration file's name, in the working directory.
pub const INI_NAME: &[u8] = b"OutpostFrontier.ini";

/// The display mode the startup code and the switches set (part of the
/// C++'s `DisplaySettings`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DisplayMode {
    pub width: u32,
    pub height: u32,
    pub fullscreen: bool,
    pub border: bool,
    pub color_bpp: i32,
    pub depth_bpp: i32,
    pub stencil_bpp: i32,
    pub interval: u32,
    pub buffer_count: u32,
    pub refresh_rate: u32,
}

impl Default for DisplayMode {
    /// The values `LoadStartupConfig` puts in before reading anything.
    fn default() -> Self {
        DisplayMode {
            width: 0x400,
            height: 0x300,
            fullscreen: true,
            border: true,
            color_bpp: -1,
            depth_bpp: -1,
            stencil_bpp: 0,
            interval: 1,
            buffer_count: 2,
            refresh_rate: 0x3c,
        }
    }
}

/// What `LoadStartupConfig` fills in.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StartupConfig {
    pub ini_path: Vec<u8>,
    pub data_path: Vec<u8>,
    /// The `[Startup]` movies; an empty entry is skipped when playing.
    pub movie_files: [Vec<u8>; MOVIE_SLOTS],
    pub gate_tunnels: Vec<u8>,
    pub jump_effect: Vec<u8>,
    pub display: DisplayMode,
    /// `+Q` or `-Q` on the command line.
    pub quick: bool,
    /// `+X` sets and `-X` clears the floating-point exception trap.
    pub fp_exception_trap: bool,
    /// Whether the INI file was read (the C++ logs an error otherwise).
    pub ini_read: bool,
}

/// `CopyBoundedString`: at most `PATH_CAPACITY - 1` bytes.
fn bounded(value: &[u8]) -> Vec<u8> {
    value[..value.len().min(PATH_CAPACITY - 1)].to_vec()
}

/// A command-line token's sign and letter: `+x` or `-x` give the upper-case
/// letter and whether the sign is not `-`; any other token gives no letter,
/// so it matches nothing.
fn switch(token: &[u8]) -> (bool, u8, &[u8]) {
    match token.first() {
        Some(&sign @ (b'+' | b'-')) => (
            sign != b'-',
            token.get(1).map_or(0, u8::to_ascii_uppercase),
            token.get(2..).unwrap_or(&[]),
        ),
        _ => (false, 0, token),
    }
}

impl StartupConfig {
    /// `LoadStartupConfig` over the contents of `OutpostFrontier.ini`
    /// (`None` when it cannot be read) and the command-line tokens.
    #[must_use]
    pub fn load(ini: Option<Vec<u8>>, args: &[&[u8]]) -> StartupConfig {
        let mut config = StartupConfig {
            ini_path: INI_NAME.to_vec(),
            data_path: b".\\data".to_vec(),
            movie_files: Default::default(),
            gate_tunnels: Vec::new(),
            jump_effect: Vec::new(),
            display: DisplayMode::default(),
            quick: false,
            fp_exception_trap: false,
            ini_read: false,
        };
        let mut movies = 0;
        let mut reader = IniReader::new();
        if let Some(contents) = ini
            && reader.open(INI_NAME, contents)
        {
            config.ini_read = true;
            while reader.read_header() {
                if reader.is_header(b"OutpostFrontier") {
                    while reader.read_value() {
                        if reader.is_value(b"data path") {
                            config.data_path = bounded(&reader.value_string());
                        }
                    }
                } else if reader.is_header(b"Startup") {
                    while reader.read_value() {
                        // The count is tested after the name.
                        if reader.is_value(b"movie_file") && movies < MOVIE_SLOTS {
                            config.movie_files[movies] = bounded(&reader.value_string());
                            movies += 1;
                        }
                    }
                } else if reader.is_header(b"Data") {
                    while reader.read_value() {
                        if reader.is_value(b"gate_tunnels") {
                            config.gate_tunnels = bounded(&reader.value_string());
                        } else if reader.is_value(b"jump_effect") {
                            config.jump_effect = bounded(&reader.value_string());
                        }
                    }
                }
            }
        }
        for token in args {
            let (plus, letter, _) = switch(token);
            if letter == b'Q' {
                config.quick = true;
            } else if letter == b'X' {
                config.fp_exception_trap = plus;
            }
        }
        config
    }

    /// The path of movie `index`: `"%s\\%s"` of the data path and the
    /// movie, or `None` for an empty slot.
    #[must_use]
    pub fn movie_path(&self, index: usize) -> Option<Vec<u8>> {
        let movie = self.movie_files.get(index)?;
        if movie.is_empty() {
            return None;
        }
        let mut path = self.data_path.clone();
        path.push(b'\\');
        path.extend_from_slice(movie);
        Some(path)
    }
}

/// `ApplyDisplayCmdlineOverrides`: `+B`/`-B` turn the border off/on, `+F`/
/// `-F` fullscreen off/on, `+M<size>` picks a mode by width, `+W` (either
/// sign) windowed. True when any of them was given.
pub fn apply_display_overrides(mode: &mut DisplayMode, args: &[&[u8]]) -> bool {
    let mut found = false;
    for token in args {
        let (plus, letter, tail) = switch(token);
        match letter {
            b'B' => {
                found = true;
                mode.border = !plus;
            }
            b'F' => {
                found = true;
                mode.fullscreen = !plus;
            }
            b'M' => {
                found = true;
                let (w, h) = match atoi(tail) {
                    0x400 => (0x400, 0x300),
                    size if size > 800 => (0x280, 0x1e0),
                    800 => (0x320, 0x258),
                    0x140 => (0x140, 0xf0),
                    0x200 => (0x200, 0x180),
                    _ => (0x280, 0x1e0),
                };
                mode.width = w;
                mode.height = h;
            }
            b'W' => {
                found = true;
                mode.fullscreen = false;
            }
            _ => {}
        }
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    const SHIPPED: &[u8] = b";\r\n; OutpostFrontier - game configuration file\r\n;\r\n\r\n[OutpostFrontier]\r\ndata path= .\\data\r\nlocal_server = rpclocal.dll\r\n\r\n[;Display]\r\n    fullscreen = 1\r\n\r\n[Startup]\r\nmovie_file = movies\\MGS_Logo_Final.wmv\r\nmovie_file = movies\\DA_Logo_Final.wmv\r\nmovie_file = movies\\FL_Intro.wmv\r\nmovie_file = movies\\Fourth.wmv\r\n\r\n[Data]\r\ngate_tunnels = fx\\tunnels.ini\r\njump_effect = fx\\jump.ini\r\n";

    #[test]
    fn the_shipped_file_gives_its_paths_and_three_movies() {
        let c = StartupConfig::load(Some(SHIPPED.to_vec()), &[]);
        assert!(c.ini_read);
        assert_eq!(c.data_path, b".\\data");
        assert_eq!(c.movie_files[0], b"movies\\MGS_Logo_Final.wmv");
        assert_eq!(c.movie_files[2], b"movies\\FL_Intro.wmv");
        assert_eq!(
            c.movie_path(1).unwrap(),
            b".\\data\\movies\\DA_Logo_Final.wmv"
        );
        assert_eq!(c.gate_tunnels, b"fx\\tunnels.ini");
        assert_eq!(c.jump_effect, b"fx\\jump.ini");
        // [;Display] is commented out: the defaults stand.
        assert_eq!(c.display, DisplayMode::default());
    }

    #[test]
    fn without_a_file_the_defaults_stand_and_switches_apply() {
        let c = StartupConfig::load(None, &[b"-q", b"+x", b"Q"]);
        assert!(!c.ini_read);
        assert_eq!(c.data_path, b".\\data");
        assert!(c.movie_path(0).is_none());
        assert!(c.quick);
        assert!(c.fp_exception_trap);
        let c = StartupConfig::load(None, &[b"+X", b"-x"]);
        assert!(!c.fp_exception_trap);
    }

    #[test]
    fn paths_are_bounded_to_259_bytes() {
        let mut ini = b"[OutpostFrontier]\ndata path = ".to_vec();
        ini.extend(std::iter::repeat_n(b'a', 400));
        let c = StartupConfig::load(Some(ini), &[]);
        assert_eq!(c.data_path.len(), PATH_CAPACITY - 1);
    }

    #[test]
    fn display_switches_follow_the_reference_table() {
        let mut m = DisplayMode::default();
        assert!(!apply_display_overrides(&mut m, &[b"Q", b"+Q"]));
        assert!(apply_display_overrides(&mut m, &[b"+B", b"+F", b"+M800"]));
        assert!(!m.border && !m.fullscreen);
        assert_eq!((m.width, m.height), (800, 600));
        let cases: [(&[u8], (u32, u32)); 6] = [
            (b"+M1024", (1024, 768)),
            (b"+M1280", (640, 480)),
            (b"+M320", (320, 240)),
            (b"+M512", (512, 384)),
            (b"+M700", (640, 480)),
            (b"-M", (640, 480)),
        ];
        for (arg, size) in cases {
            apply_display_overrides(&mut m, &[arg]);
            assert_eq!(
                (m.width, m.height),
                size,
                "{}",
                String::from_utf8_lossy(arg)
            );
        }
        apply_display_overrides(&mut m, &[b"-F", b"-B"]);
        assert!(m.fullscreen && m.border);
        apply_display_overrides(&mut m, &[b"-W"]);
        assert!(!m.fullscreen);
    }
}
