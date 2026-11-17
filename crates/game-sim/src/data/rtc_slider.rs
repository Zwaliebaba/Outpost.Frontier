//! The cut-scene performance slider (`RtcSlider`), ported from
//! `CppSource/Common/common_rtcslider.cpp`.
//!
//! `[Data] RtcSlider` reads `rtcslider.ini`: per setting name a threshold
//! and a mode. A `set` setting is enabled while the slider is at or above
//! its threshold, a `set_less` setting while the slider is below it. The
//! first line for a name wins.

use std::collections::BTreeMap;

use neuron_core::id::create_id;

use super::{DataSource, Reports, lossy, open_ini, value_f32};

const SOURCE: &str = "Common\\RtcSlider.cpp";

/// The frame cap when neither cap setting is enabled.
pub const DEFAULT_FPS_CAP: f32 = 200.0;

/// One setting (`FRtcSliderSetting`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Setting {
    /// The name's id.
    pub name_id: u32,
    /// The threshold.
    pub threshold: f32,
    /// 0 for `set`, 1 for `set_less`.
    pub compare_mode: i32,
}

impl Setting {
    /// The setting test of every caller: NaN on either side is disabled.
    #[must_use]
    pub fn is_enabled(&self, slider: f32) -> bool {
        match self.compare_mode {
            0 => self.threshold <= slider,
            1 => slider < self.threshold,
            _ => false,
        }
    }
}

/// The slider and its settings.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RtcSlider {
    /// The slider value (0 at start).
    pub slider: f32,
    /// Settings by name id.
    pub settings: BTreeMap<u32, Setting>,
}

impl RtcSlider {
    /// `RtcSlider::load`.
    pub fn load(&mut self, source: &dyn DataSource, path: &[u8], reports: &mut Reports) {
        let Some(mut reader) = open_ini(source, path) else {
            reports.push(
                SOURCE,
                0xba,
                format!(
                    "*** ERROR: Cannot open rtc performance slider ini file {}",
                    lossy(path)
                ),
            );
            return;
        };
        while reader.read_header() {
            if !reader.is_header(b"RtcSlider") {
                reports.push(
                    SOURCE,
                    0xb4,
                    format!(
                        "*** WARNING: Invalid header {} in rtc performance slider ini file {}",
                        lossy(&reader.header()),
                        lossy(path)
                    ),
                );
                continue;
            }
            while reader.read_value() {
                let (mode, line) = if reader.is_value(b"set") {
                    (0, 0x8d)
                } else if reader.is_value(b"set_less") {
                    (1, 0xa7)
                } else {
                    reports.push(
                        SOURCE,
                        0xad,
                        format!(
                            "*** WARNING: Invalid value {} in rtc performance slider ini file {}",
                            lossy(&reader.value_string()),
                            lossy(path)
                        ),
                    );
                    continue;
                };
                if reader.is_value_empty(0) || reader.is_value_empty(1) {
                    reports.push(
                        SOURCE,
                        line,
                        format!(
                            "*** WARNING: Invalid syntax {} in rtc performance slider ini file {}",
                            lossy(&reader.line()),
                            lossy(path)
                        ),
                    );
                    continue;
                }
                let name_id = create_id(&reader.value_string_at(0));
                let threshold = value_f32(&mut reader, 1);
                self.settings.entry(name_id).or_insert(Setting {
                    name_id,
                    threshold,
                    compare_mode: mode,
                });
            }
        }
    }

    /// `is_enabled(name_crc)`: crc 0 and an unknown crc (reported) are
    /// enabled.
    pub fn is_enabled_id(&self, name_crc: u32, reports: &mut Reports) -> bool {
        if name_crc == 0 {
            return true;
        }
        match self.settings.get(&name_crc) {
            Some(setting) => setting.is_enabled(self.slider),
            None => {
                reports.push(
                    SOURCE,
                    0xd4,
                    format!(
                        "*** WARNING: Cannot find performance slider setting for name_crc {name_crc}"
                    ),
                );
                true
            }
        }
    }

    /// `is_enabled(name)`: an unknown name (reported) is enabled.
    pub fn is_enabled(&self, name: &[u8], reports: &mut Reports) -> bool {
        match self.settings.get(&create_id(name)) {
            Some(setting) => setting.is_enabled(self.slider),
            None => {
                reports.push(
                    SOURCE,
                    0xe1,
                    format!(
                        "*** WARNING: Cannot find performance slider setting for name {}",
                        lossy(name)
                    ),
                );
                true
            }
        }
    }

    /// `get_fps_cap`: 20 when `fps_20_cap` is enabled (or missing), else 30
    /// when `fps_30_cap` is, else 200.
    pub fn fps_cap(&self, reports: &mut Reports) -> f32 {
        if self.is_enabled(b"fps_20_cap", reports) {
            20.0
        } else if self.is_enabled(b"fps_30_cap", reports) {
            30.0
        } else {
            DEFAULT_FPS_CAP
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct One(&'static [u8]);

    impl DataSource for One {
        fn read(&self, _path: &[u8]) -> Option<Vec<u8>> {
            Some(self.0.to_vec())
        }
    }

    #[test]
    fn settings_and_caps() {
        let source = One(
            b"[RtcSlider]\nset = a, 0.5\nset = a, 0.9\nset_less = fps_20_cap, 0.5\n\
            set_less = fps_30_cap, 0.75\nset = b\nbad = 1\n[Other]\n",
        );
        let mut reports = Reports::default();
        let mut s = RtcSlider::default();
        s.load(&source, b"x", &mut reports);
        assert_eq!(reports.0.len(), 3);
        assert_eq!(s.settings.len(), 3);
        assert!(!s.is_enabled(b"a", &mut reports));
        assert_eq!(s.fps_cap(&mut reports), 20.0);
        s.slider = 0.6;
        assert!(s.is_enabled(b"a", &mut reports));
        assert_eq!(s.fps_cap(&mut reports), 30.0);
        s.slider = 0.8;
        assert_eq!(s.fps_cap(&mut reports), 200.0);
        assert!(s.is_enabled_id(0, &mut reports));
    }
}
