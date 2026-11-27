//! The in-flight cameras (`space_camera.cpp`, `space_camera_game.cpp`,
//! `SpaceAppLoadCamerasIni`; work package P5.4a): the eight cameras of the
//! in-flight controller, their settings from `cameras.ini`, the mode
//! switch and the hard-cut test.
//!
//! The interface the other streams read:
//!
//! - [`SpaceCameras`]: the eight [`GameCamera`]s by [`SpaceCameraMode`],
//!   the active one ([`SpaceCameras::active`],
//!   [`SpaceCameras::active_camera`]), the mode switch
//!   ([`SpaceCameras::switch_mode`]) and the settings load
//!   ([`SpaceCameras::load_ini`]).
//! - [`GameCamera`]: the projection (`FSpaceCamera`, ported in
//!   [`neuron_client::ui::camera::Camera`]) with `FGameCamera`'s resting
//!   angle and cut flag. The HUD projects through
//!   [`SpaceCameras::active_camera`]; the environment reads the same
//!   camera's position for the star sphere, the dust and the fog.
//!
//! Each concrete camera's own motion (first person, chase, turret, rear
//! view, death, script, static, docking), its activation with the window's
//! rectangle and its attach to the player are P5.4b's.

use game_sim::object::ObjHandle;
use neuron_client::ui::camera::Camera;
use neuron_client::ui::widescreen::Surface;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;

/// `kGameCameraDefaultHalfFovXDeg`.
pub const DEFAULT_HALF_FOV_X: f32 = 35.0;

/// `kSpaceCameraCutCosine`: the view direction may turn by up to about 18
/// degrees across a switch before the switch counts as a cut.
pub const CUT_COSINE: f32 = 0.95;

/// The source the camera settings' warnings name.
pub const SPACE_CPP: &str = "Client\\space\\space.cpp";

/// `ESpaceCameraMode`: the eight cameras, in controller order.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum SpaceCameraMode {
    /// `first_person`, the cockpit camera.
    FirstPerson = 0,
    /// `third_person`, the chase camera.
    ThirdPerson = 1,
    /// `turret`.
    Turret = 2,
    /// `rearview`.
    Rearview = 3,
    /// `death`.
    Death = 4,
    /// `script`.
    Script = 5,
    /// `static`.
    Static = 6,
    /// `docking`.
    Docking = 7,
}

impl SpaceCameraMode {
    /// The modes in controller order.
    pub const ALL: [Self; 8] = [
        Self::FirstPerson,
        Self::ThirdPerson,
        Self::Turret,
        Self::Rearview,
        Self::Death,
        Self::Script,
        Self::Static,
        Self::Docking,
    ];

    /// The class name each camera reports (slot 0x54).
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Self::FirstPerson => "first_person",
            Self::ThirdPerson => "third_person",
            Self::Turret => "turret",
            Self::Rearview => "rearview",
            Self::Death => "death",
            Self::Script => "script",
            Self::Static => "static",
            Self::Docking => "docking",
        }
    }

    /// The `cameras.ini` section of the controller's camera (`[WinCamera]`
    /// is the Win3D camera's); `None` for the cameras the file does not
    /// set.
    #[must_use]
    pub fn ini_section(self) -> Option<&'static [u8]> {
        match self {
            Self::FirstPerson => Some(b"CockpitCamera"),
            Self::ThirdPerson => Some(b"ThirdPersonCamera"),
            Self::Turret => Some(b"TurretCamera"),
            Self::Rearview => Some(b"RearViewCamera"),
            Self::Death => Some(b"DeathCamera"),
            Self::Script | Self::Static | Self::Docking => None,
        }
    }

    /// The piloting modes select the cross cursor and draw the software
    /// cursor; the cinematic ones hide it.
    #[must_use]
    pub fn is_piloting(self) -> bool {
        matches!(
            self,
            Self::FirstPerson | Self::ThirdPerson | Self::Turret | Self::Rearview
        )
    }

    fn index(self) -> usize {
        self as usize
    }
}

/// `FGameCamera`: the projection and what the game layer adds to it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GameCamera {
    /// `FSpaceCamera`.
    pub camera: Camera,
    /// +0xc8 the resting half angle, re-cached when `fovx` or `fovy` is
    /// set.
    pub base_half_fov_x: f32,
    /// +0xbc raised when the view jumps (a hard cut).
    pub cut: bool,
    /// +0xc0 the watched object (`BaseWatcher`); `None` when unbound.
    pub watched: Option<ObjHandle>,
    /// +0xd0 the position at the last update.
    pub last_position: Vector,
    /// +0xdc the velocity (`GetVelocity`, slot 0x48).
    pub velocity: Vector,
}

impl GameCamera {
    /// `GameCameraConstruct`: the base camera with the default half angle.
    #[must_use]
    pub fn new(surface: Surface) -> Self {
        let mut camera = Camera::new(surface);
        camera.set_half_fov_x(DEFAULT_HALF_FOV_X);
        Self {
            camera,
            base_half_fov_x: DEFAULT_HALF_FOV_X,
            cut: false,
            watched: None,
            last_position: Vector::ZERO,
            velocity: Vector::ZERO,
        }
    }

    /// `GameCameraAttach` (slot 0x58): the object watched; false when it is
    /// the one already watched (nothing changes).
    pub fn attach(&mut self, object: Option<ObjHandle>) -> bool {
        if object == self.watched {
            return false;
        }
        self.watched = object;
        true
    }

    /// `GameCameraHasNoTarget` (slot 0x60).
    #[must_use]
    pub fn has_no_target(&self) -> bool {
        self.watched.is_none()
    }

    /// `GameCameraActivate` (slot 0x64): the viewport set, then applied
    /// (`GameCameraApplyViewport`, the letterbox's camera forgotten by its
    /// owner). Always true, as the reference's.
    pub fn activate(&mut self, rect: [i32; 4]) -> bool {
        self.camera.set_viewport(rect);
        self.camera.set_viewport(rect);
        true
    }

    /// `GameCameraUpdate` (slot 0x6c): the velocity from the position's
    /// change (skipped for a `dt` of exactly zero), then the position kept.
    pub fn update(&mut self, dt: f32) {
        let p = self.camera.world_pos;
        if dt != 0.0 {
            let d = |a: f32, b: f32| {
                #[allow(clippy::cast_possible_truncation)]
                let v = (f64::from(a) - f64::from(b)) as f32;
                v
            };
            let (dx, dy, dz) = (
                d(p.x, self.last_position.x),
                d(p.y, self.last_position.y),
                d(p.z, self.last_position.z),
            );
            #[allow(clippy::cast_possible_truncation)]
            let inv = (1.0 / f64::from(dt)) as f32;
            let m = |a: f32| {
                #[allow(clippy::cast_possible_truncation)]
                let v = (f64::from(inv) * f64::from(a)) as f32;
                v
            };
            self.velocity = Vector {
                x: m(dx),
                y: m(dy),
                z: m(dz),
            };
        }
        self.last_position = p;
    }

    /// `GameCameraSetProperty`: the base's property; a field-of-view key
    /// also re-caches the resting angle.
    pub fn set_property(&mut self, key: &[u8], value: &[u8]) -> bool {
        if !self.camera.set_property(key, value) {
            return false;
        }
        if key.eq_ignore_ascii_case(b"fovx") || key.eq_ignore_ascii_case(b"fovy") {
            self.base_half_fov_x = self.camera.half_fov_x_deg;
        }
        true
    }
}

/// What a mode switch asks of the cursor.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CursorChange {
    /// The piloting modes: `MouseSetCursor("Cross", true)` and the
    /// software cursor drawn.
    Cross,
    /// The cinematic modes: the software cursor not drawn.
    Hidden,
}

/// The outcome of [`SpaceCameras::switch_mode`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CameraSwitch {
    /// The camera selected; `None` when `player` had a preference other
    /// than 0 or 1.
    pub selected: Option<SpaceCameraMode>,
    /// The cursor change.
    pub cursor: CursorChange,
    /// `player`, `first_person` and `third_person` close the rear and
    /// turret views first (`CloseRearAndTurretViewsForDocking`).
    pub close_rear_and_turret: bool,
}

/// The in-flight controller's cameras (`SpaceApp` +0x00c to +0xba4).
#[derive(Clone, Debug)]
pub struct SpaceCameras {
    cameras: [GameCamera; 8],
    active: Option<SpaceCameraMode>,
    /// +0xba4 the first (0) or third (1) person preference `player`
    /// selects; the controller starts at 1.
    pub player_preference: i32,
    /// `G_SpaceCameraSwitched`: raised by every switch, even one that
    /// selects nothing.
    pub switched: bool,
    /// The last switch was to `script`.
    pub last_was_script: bool,
}

impl SpaceCameras {
    /// The controller's cameras for a surface, none active.
    #[must_use]
    pub fn new(surface: Surface) -> Self {
        Self {
            cameras: [GameCamera::new(surface); 8],
            active: None,
            player_preference: 1,
            switched: false,
            last_was_script: false,
        }
    }

    /// The camera of `mode`.
    #[must_use]
    pub fn camera(&self, mode: SpaceCameraMode) -> &GameCamera {
        &self.cameras[mode.index()]
    }

    /// The camera of `mode`, mutably.
    pub fn camera_mut(&mut self, mode: SpaceCameraMode) -> &mut GameCamera {
        &mut self.cameras[mode.index()]
    }

    /// `GetActiveSpaceCamera`: the installed camera's mode.
    #[must_use]
    pub fn active(&self) -> Option<SpaceCameraMode> {
        self.active
    }

    /// The installed camera (the sound listener and the scene's camera).
    #[must_use]
    pub fn active_camera(&self) -> Option<&GameCamera> {
        self.active.map(|m| self.camera(m))
    }

    /// The installed camera, mutably.
    pub fn active_camera_mut(&mut self) -> Option<&mut GameCamera> {
        self.active.map(|m| &mut self.cameras[m.index()])
    }

    /// `SpaceAppLoadCamerasIni` on the file's bytes: each section's values
    /// to its camera's `SetProperty`, `[WinCamera]` to `win` (the Win3D
    /// camera's base `SetProperty`). The warnings, without the source
    /// prefix, are returned.
    pub fn load_ini(
        &mut self,
        win: Option<&mut Camera>,
        path: &[u8],
        contents: Vec<u8>,
    ) -> Vec<String> {
        let mut reports = Vec::new();
        let mut reader = IniReader::new();
        if !reader.open(path, contents) {
            return reports;
        }
        let mut win = win;
        while reader.read_header() {
            let mode = SpaceCameraMode::ALL
                .into_iter()
                .find(|m| m.ini_section().is_some_and(|s| reader.is_header(s)));
            let is_win = reader.is_header(b"WinCamera");
            if mode.is_none() && !is_win {
                reports.push(format!(
                    "*** WARNING: Unknown camera type? ({})",
                    String::from_utf8_lossy(&reader.header())
                ));
                continue;
            }
            while reader.read_value() {
                let name = reader.name();
                let value = reader.value_string();
                let known = match (mode, win.as_deref_mut()) {
                    (Some(mode), _) => self.cameras[mode.index()].set_property(&name, &value),
                    (None, Some(win)) => win.set_property(&name, &value),
                    // No Win3D camera given: its values are taken.
                    (None, None) => true,
                };
                if !known {
                    reports.push(format!(
                        "*** WARNING: [{}] unknown property '{} = {}'",
                        String::from_utf8_lossy(&reader.header()),
                        String::from_utf8_lossy(&name),
                        String::from_utf8_lossy(&value)
                    ));
                }
            }
        }
        reports
    }

    /// `SwitchSpaceCameraMode(name)`: the name (cut to 31 bytes) picks the
    /// camera; `player` the preference, `first_person` and
    /// `third_person` also set it. The selected camera is installed (a
    /// switch to `script` reinstalls it even when it is active). `None`
    /// for an unknown name, which changes nothing but
    /// [`Self::switched`].
    pub fn switch_mode(&mut self, name: &[u8]) -> Option<CameraSwitch> {
        let name = &name[..name
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(name.len())
            .min(0x1f)];
        self.switched = true;
        self.last_was_script = false;
        let (selected, close) = match name {
            b"player" => (
                match self.player_preference {
                    0 => Some(SpaceCameraMode::FirstPerson),
                    1 => Some(SpaceCameraMode::ThirdPerson),
                    _ => None,
                },
                true,
            ),
            b"first_person" => {
                self.player_preference = 0;
                (Some(SpaceCameraMode::FirstPerson), true)
            }
            b"third_person" => {
                self.player_preference = 1;
                (Some(SpaceCameraMode::ThirdPerson), true)
            }
            _ => {
                let mode = SpaceCameraMode::ALL
                    .into_iter()
                    .find(|m| m.name().as_bytes() == name)?;
                self.last_was_script = mode == SpaceCameraMode::Script;
                (Some(mode), false)
            }
        };
        let piloting = name == b"player" || selected.is_some_and(SpaceCameraMode::is_piloting);
        if let Some(mode) = selected {
            self.install(Some(mode), self.last_was_script);
        }
        Some(CameraSwitch {
            selected,
            cursor: if piloting {
                CursorChange::Cross
            } else {
                CursorChange::Hidden
            },
            close_rear_and_turret: close,
        })
    }

    /// `InstallSpaceCamera`'s bookkeeping: nothing for the active camera
    /// unless `force`; otherwise the camera becomes active and its cut flag
    /// is raised when the view jumps ([`is_hard_cut`]). `None` deactivates.
    pub fn install(&mut self, mode: Option<SpaceCameraMode>, force: bool) {
        if mode == self.active && !force {
            return;
        }
        let old = self.active_camera().map(|c| c.camera.world);
        self.active = mode;
        if let Some(mode) = mode {
            let camera = &mut self.cameras[mode.index()];
            let new = camera.camera.world;
            let old_rows = old.map(|m| rows(&m));
            if is_hard_cut(old_rows.as_ref(), &rows(&new)) {
                camera.cut = true;
            }
        }
    }
}

/// A matrix as the nine row-major floats the C++ camera stores.
fn rows(m: &neuron_core::math::Matrix) -> [f32; 9] {
    let [a, b, c] = m.m;
    [a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]]
}

/// `SpaceCameraIsHardCut`: the dot of the two view axes (column 2) under
/// [`CUT_COSINE`], strictly and ordered (a NaN is no cut), or no camera
/// before.
#[must_use]
pub fn is_hard_cut(old: Option<&[f32; 9]>, new: &[f32; 9]) -> bool {
    let Some(old) = old else {
        return true;
    };
    let dot = (f64::from(new[8]) * f64::from(old[8]) + f64::from(new[5]) * f64::from(old[5]))
        + f64::from(new[2]) * f64::from(old[2]);
    dot < f64::from(CUT_COSINE)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_mode_names_select_as_the_switch_does() {
        let mut cams = SpaceCameras::new(Surface::default());
        let s = cams.switch_mode(b"player").unwrap();
        assert_eq!(s.selected, Some(SpaceCameraMode::ThirdPerson));
        assert_eq!(s.cursor, CursorChange::Cross);
        assert!(cams.camera(SpaceCameraMode::ThirdPerson).cut);
        cams.switch_mode(b"first_person").unwrap();
        assert_eq!(cams.player_preference, 0);
        // The same orientation is no cut.
        assert!(!cams.camera(SpaceCameraMode::FirstPerson).cut);
        let s = cams.switch_mode(b"script").unwrap();
        assert_eq!(
            (s.cursor, cams.last_was_script),
            (CursorChange::Hidden, true)
        );
        cams.switched = false;
        assert!(cams.switch_mode(b"orbit").is_none());
        assert!(cams.switched);
        assert_eq!(cams.active(), Some(SpaceCameraMode::Script));
        cams.player_preference = 2;
        let s = cams.switch_mode(b"player").unwrap();
        assert_eq!((s.selected, s.cursor), (None, CursorChange::Cross));
    }

    #[test]
    fn the_game_camera_keeps_velocity_and_its_target() {
        let mut cam = GameCamera::new(Surface::default());
        cam.camera.world_pos = Vector {
            x: 3.0,
            y: 0.0,
            z: -6.0,
        };
        cam.update(0.5);
        assert_eq!(
            cam.velocity,
            Vector {
                x: 6.0,
                y: 0.0,
                z: -12.0
            }
        );
        // A zero dt keeps the velocity and takes the position.
        cam.camera.world_pos = Vector {
            x: 4.0,
            y: 0.0,
            z: -6.0,
        };
        cam.update(0.0);
        assert_eq!(cam.velocity.x, 6.0);
        assert_eq!(cam.last_position.x, 4.0);
        assert!(cam.has_no_target());
    }

    #[test]
    fn cuts_need_an_ordered_turn() {
        let id = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0];
        let mut turned = id;
        turned[8] = 0.9;
        assert!(is_hard_cut(None, &id));
        assert!(!is_hard_cut(Some(&id), &id));
        assert!(is_hard_cut(Some(&id), &turned));
        turned[8] = f32::NAN;
        assert!(!is_hard_cut(Some(&id), &turned));
    }

    #[test]
    fn the_settings_go_to_their_cameras() {
        let ini = b"[WinCamera]\nfovx = 54.4319992\n[CockpitCamera]\nfovx = 70\nznear = 2\n\
[Bogus]\nx = 1\n[DeathCamera]\nzoom = 3\n"
            .to_vec();
        let mut cams = SpaceCameras::new(Surface::default());
        let mut win = Camera::new(Surface::default());
        let reports = cams.load_ini(Some(&mut win), b"cameras.ini", ini);
        let cockpit = cams.camera(SpaceCameraMode::FirstPerson);
        assert_eq!(cockpit.camera.half_fov_x_deg, 35.0);
        assert_eq!(cockpit.base_half_fov_x, 35.0);
        assert_eq!(cockpit.camera.z_near, 2.0);
        assert_eq!(win.half_fov_x_deg, (54.431_999_2f64 * 0.5) as f32);
        assert_eq!(reports.len(), 2, "{reports:?}");
    }
}
