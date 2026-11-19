//! The client's performance options (`Client/perf_options.cpp`; work
//! package P4.1): the 36 named float options of `PerfOptions.ini` in the
//! per-user folder, the file's `[Display]` mode, and the low-machine
//! warning's skip flag.
//!
//! `LoadPerfOptions` trusts a file only when its `[PerfVersion]` is 5 or
//! more, every known option it lists is not negative, and it lists exactly
//! as many known options as its `[PerfCount]` claims; then the options
//! whose value changed are applied. The `[Display]` block fills the
//! caller's mode whatever the verdict. `SavePerfOptions` writes the version,
//! the skip flag, the count, every option applied at least once and the
//! display mode the device took.
//!
//! The C++ applies an option through a callback per option, most of them
//! into the game's objects and the renderer's settings (P5). Here
//! [`PerfOptions::apply_changed`] records each option's applied value and
//! returns the options it applied, for the caller to act on.
//! `InitializeEngine`'s first-run seed (`NnPreferences::SeedGraphicsDefaults`:
//! `optlist.ini`'s defaults and the machine's graphics score) belongs to the
//! options menu (P5) and is not run (divergence D533).

use neuron_core::ini::IniReader;
use neuron_core::msvcrt::format_g;
use neuron_core::num::trunc_to_i32;

/// The file's name in the per-user folder.
pub const FILE_NAME: &str = "PerfOptions.ini";

/// The version `SavePerfOptions` writes and `LoadPerfOptions` requires.
pub const VERSION: f64 = 5.0;

/// The number of options.
pub const OPTION_COUNT: usize = 36;

/// The options' names and the text IDs of their menu names, in the C++'s
/// registration order (the order of applying and saving).
pub const OPTIONS: [(&[u8], u32); OPTION_COUNT] = [
    (b"mouse_speed", 0x5dd),
    (b"gamma_ramp", 0x534),
    (b"DitherControl", 0x67f),
    (b"MipFilterControlCallback", 0),
    (b"TurnSensitivity", 0x614),
    (b"generic_effect_perf", 0x5cb),
    (b"Nebula", 0x57d),
    (b"SunEffect", 0x5ca),
    (b"Asteroids", 0x57c),
    (b"space_dust_detail", 0x57a),
    (b"star_sphere", 0x57b),
    (b"AtmosphereMaterialConfig", 0x5c9),
    (b"HUDAnimConfig", 0x65e),
    (b"level_of_detail", 0x5d5),
    (b"use_environment_maps", 0x657),
    (b"high_detail_textures", 0x658),
    (b"sound_max_dist_scale", 0x77a),
    (b"sound_level_of_detail", 0x77b),
    (b"USE_3D_SOUNDS", 0x77e),
    (b"audio_music", 0x539),
    (b"audio_ambient", 0x584),
    (b"audio_sfx", 0x538),
    (b"audio_interface", 0x583),
    (b"audio_voice", 0x581),
    (b"rtc_performance", 0x656),
    (b"general_graphics", 0),
    (b"advanced_graphics", 0),
    (b"default_graphics", 0),
    (b"skip_transition_scripts", 0x5d7),
    (b"enable_tooltips", 0x578),
    (b"enable_rollover", 0x579),
    (b"cockpit", 0x5da),
    (b"USE_DYNAMIC_COMM_HEAD", 0x631),
    (b"DIFFICULTY_SCALE", 0x57f),
    (b"autohide_maneuver_bar", 0x613),
    (b"max_submitted_sounds", 0x687),
];

/// One option's state: the value last read or set, and the value last
/// applied (-1 until it is applied, as the C++ marks "never").
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PerfOption {
    pub value: f32,
    pub applied: f32,
}

impl Default for PerfOption {
    fn default() -> Self {
        PerfOption {
            value: 0.0,
            applied: -1.0,
        }
    }
}

/// The `[Display]` block: the part of the display mode the file keeps
/// (`FPerfDisplaySettings`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PerfDisplay {
    pub width: i32,
    pub height: i32,
    pub color_bpp: i32,
    pub depth_bpp: i32,
}

/// The options' registry and the low-machine warning's flag.
#[derive(Clone, Debug, PartialEq)]
pub struct PerfOptions {
    pub options: [PerfOption; OPTION_COUNT],
    /// `SkipMachineWarnings`.
    pub skip_machine_warnings: bool,
}

impl Default for PerfOptions {
    fn default() -> Self {
        PerfOptions {
            options: [PerfOption::default(); OPTION_COUNT],
            skip_machine_warnings: false,
        }
    }
}

/// `ftol` of a `float`: the double the INI reader returns is stored to a
/// float, then truncated.
#[allow(clippy::cast_possible_truncation)]
fn ftol_float(value: f64) -> i32 {
    trunc_to_i32(f64::from(value as f32))
}

impl PerfOptions {
    /// `FindPerfOption`: the first option named exactly `name`.
    #[must_use]
    pub fn find(name: &[u8]) -> Option<usize> {
        OPTIONS.iter().position(|(n, _)| *n == name)
    }

    /// `FindPerfOptionByIds`: the first option with the menu text `ids`
    /// (0 finds the first option without one).
    #[must_use]
    pub fn find_by_ids(ids: u32) -> Option<usize> {
        OPTIONS.iter().position(|&(_, i)| i == ids)
    }

    /// The applied value of the option `name`.
    #[must_use]
    pub fn applied(&self, name: &[u8]) -> Option<f32> {
        Self::find(name).map(|i| self.options[i].applied)
    }

    /// `ApplyChangedPerfOptions`: every option whose value differs from
    /// its applied value is applied, in registry order; returns them.
    pub fn apply_changed(&mut self) -> Vec<usize> {
        let mut applied = Vec::new();
        for (index, option) in self.options.iter_mut().enumerate() {
            #[allow(clippy::float_cmp)]
            if option.applied == option.value {
                continue;
            }
            option.applied = option.value;
            applied.push(index);
        }
        applied
    }

    /// `LoadPerfOptions` over the file's contents (`None` when it cannot
    /// be read). `display` takes the `[Display]` block when given. Returns
    /// the options applied when the file is current, `None` otherwise.
    pub fn load(
        &mut self,
        contents: Option<Vec<u8>>,
        mut display: Option<&mut PerfDisplay>,
    ) -> Option<Vec<usize>> {
        let mut reader = IniReader::new();
        if !contents.is_some_and(|c| reader.open(FILE_NAME.as_bytes(), c)) {
            return None;
        }
        let mut ok = false;
        let mut version_ok = false;
        let mut expected = 0;
        let mut parsed = 0;
        if reader.read_header() {
            loop {
                if reader.is_header(b"PerfVersion") {
                    if reader.read_value() {
                        loop {
                            if reader.is_value(b"version") {
                                ok = reader.value_float(0) >= VERSION;
                            } else if reader.is_value(b"SkipMachineWarnings") {
                                self.skip_machine_warnings = reader.value_bool(0);
                            }
                            if !reader.read_value() {
                                break;
                            }
                        }
                        // The verdict is kept and restored after every
                        // accepted option.
                        version_ok = ok;
                    }
                } else if reader.is_header(b"PerfCount") {
                    if reader.read_value() {
                        loop {
                            if reader.is_value(b"count") {
                                expected = ftol_float(reader.value_float(0));
                            }
                            if !reader.read_value() {
                                break;
                            }
                        }
                    }
                } else if reader.is_header(b"PerfOptions") {
                    // "perfoptions out of date -- reverting to defaults".
                    if !ok {
                        break;
                    }
                    if reader.read_value() {
                        loop {
                            if !ok {
                                break;
                            }
                            if let Some(index) = Self::find(&reader.name()) {
                                #[allow(clippy::cast_possible_truncation)]
                                let value = reader.value_float(0) as f32;
                                parsed += 1;
                                if value >= 0.0 {
                                    self.options[index].value = value;
                                    ok = version_ok;
                                } else {
                                    ok = false;
                                    version_ok = false;
                                }
                            }
                            if !reader.read_value() {
                                break;
                            }
                        }
                    }
                } else if reader.is_header(b"Display")
                    && let Some(display) = display.as_deref_mut()
                    && reader.read_value()
                {
                    loop {
                        if reader.is_value(b"size") {
                            display.width = reader.value_int(0);
                            display.height = reader.value_int(1);
                        } else if reader.is_value(b"color_bpp") {
                            display.color_bpp = reader.value_int(0);
                        } else if reader.is_value(b"depth_bpp") {
                            display.depth_bpp = reader.value_int(0);
                        }
                        if !reader.read_value() {
                            break;
                        }
                    }
                }
                if !reader.read_header() {
                    break;
                }
            }
            // Trusted only when it lists exactly as many known options as
            // it claims.
            if parsed != expected {
                ok = false;
            }
        }
        reader.close();
        ok.then(|| self.apply_changed())
    }

    /// `SavePerfOptions`: the file's text, with the C++'s text-mode line
    /// ends (`\r\n`).
    #[must_use]
    pub fn save(&self, display: &PerfDisplay) -> Vec<u8> {
        let mut text = String::new();
        text.push_str(&format!(
            "[PerfVersion]\nversion={}\n",
            format_g(VERSION, 6)
        ));
        text.push_str(&format!(
            "SkipMachineWarnings={}\n\n",
            if self.skip_machine_warnings {
                "TRUE"
            } else {
                "FALSE"
            }
        ));
        #[allow(clippy::cast_precision_loss)]
        let count = OPTION_COUNT as f64;
        text.push_str(&format!("[PerfCount]\ncount={}\n\n", format_g(count, 6)));
        text.push_str("[PerfOptions]\n");
        let mut bytes = text.into_bytes();
        for ((name, _), option) in OPTIONS.iter().zip(&self.options) {
            // Only options applied at least once.
            if option.applied >= 0.0 {
                bytes.extend_from_slice(name);
                bytes
                    .extend_from_slice(format!(" = {:.2}\n", f64::from(option.applied)).as_bytes());
            }
        }
        bytes.extend_from_slice(
            format!(
                "\n[Display]\nsize= {}, {}\ncolor_bpp= {}\ndepth_bpp= {}\n",
                display.width, display.height, display.color_bpp, display.depth_bpp
            )
            .as_bytes(),
        );
        let mut out = Vec::with_capacity(bytes.len() + 32);
        for b in bytes {
            if b == b'\n' {
                out.push(b'\r');
            }
            out.push(b);
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_saved_file_loads_back() {
        let mut options = PerfOptions::default();
        for (i, o) in options.options.iter_mut().enumerate() {
            #[allow(clippy::cast_precision_loss)]
            {
                o.value = i as f32 / 8.0;
            }
        }
        options.skip_machine_warnings = true;
        assert_eq!(options.apply_changed().len(), OPTION_COUNT);
        let display = PerfDisplay {
            width: 1280,
            height: 960,
            color_bpp: 32,
            depth_bpp: 24,
        };
        let text = options.save(&display);
        let mut back = PerfOptions::default();
        let mut mode = PerfDisplay {
            width: 0,
            height: 0,
            color_bpp: -1,
            depth_bpp: -1,
        };
        let applied = back.load(Some(text), Some(&mut mode)).unwrap();
        // Index 0 holds 0.0 and was applied (from -1); all others too.
        assert_eq!(applied.len(), OPTION_COUNT);
        assert_eq!(mode, display);
        assert!(back.skip_machine_warnings);
        // Two decimals: 0.125 is written as 0.12.
        assert_eq!(back.applied(b"TurnSensitivity"), Some(0.5));
        assert_eq!(back.applied(b"DitherControl"), Some(0.25));
        assert_eq!(back.applied(b"MipFilterControlCallback"), Some(0.38));
        assert_eq!(PerfOptions::find_by_ids(0), Some(3));
    }

    #[test]
    fn a_file_without_the_options_is_not_current_but_keeps_its_mode() {
        let options = PerfOptions::default();
        let display = PerfDisplay {
            width: 1024,
            height: 768,
            color_bpp: 32,
            depth_bpp: 24,
        };
        let text = options.save(&display);
        assert!(
            String::from_utf8_lossy(&text)
                .contains("[PerfOptions]\r\n\r\n[Display]\r\nsize= 1024, 768\r\n")
        );
        let mut back = PerfOptions::default();
        let mut mode = PerfDisplay {
            width: 0,
            height: 0,
            color_bpp: -1,
            depth_bpp: -1,
        };
        assert!(back.load(Some(text), Some(&mut mode)).is_none());
        assert_eq!(mode, display);
        assert!(back.load(None, Some(&mut mode)).is_none());
    }
}
