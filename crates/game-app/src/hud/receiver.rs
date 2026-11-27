//! `HUD_Receiver` (`hud_comm.cpp`, the `[Receiver]` section of `hud.ini`):
//! the comm receiver. It owns two comm channels: channel A, with the
//! talking-head window and its caption, fed from the player's inbox, and
//! channel B, which only speaks. Unlike the other HUD windows it is a plain
//! `Win3D` (no `WinControl`, no name, no class name) with one child, the
//! low-detail comm button.
//!
//! The port so far: the constructor (its section, the comm window's keys,
//! the speaker's pose and the fixed window spot), the button, `Init` and
//! the button's half of `Update`. The channels, the inboxes they read, the
//! portrait light and camera, and `Draw` come with the HUD's comm
//! (divergence D1115).

use game_sim::data::zone::euler_matrix;
use neuron_client::ui::children::{ButtonInit, create_button_control};
use neuron_client::ui::tree::{ACTIVE, NodeId, UNREGISTERED, Ui, VISIBLE, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::HudItem;

/// The factory's section.
pub const SECTION: &[u8] = b"Receiver";
/// `HUDItem::Name`.
pub const NAME: &[u8] = b"RadioReceiver";
/// The low-detail comm button's name and mesh.
pub const BUTTON_NAME: &[u8] = b"LowEndCommButton";
pub const BUTTON_MESH: &[u8] = b"interface\\HUD\\HUD_genericcomm.3db";
/// `kHudCommCpp`, the warnings' file name.
const HUD_COMM_CPP: &str = r"Client\Space\HUD_Comm.cpp";

/// `FCommWindow`: channel A's talking-head window, as its keys set it.
#[derive(Clone, Debug, PartialEq)]
pub struct CommWindow {
    /// `+0x00`.
    pub open: bool,
    /// `+0x04`: the screen rect of corners 0 and 3.
    pub rect: [i32; 4],
    /// `+0x24` "win_pos" (0.3, 0.3, -1).
    pub win_pos: Vector,
    /// `+0x30` "btn_pos" (0, 0, -2).
    pub btn_pos: Vector,
    /// `+0x3c`, `+0x40` "win_size" (0.2, 0.2).
    pub win_size: (f32, f32),
    /// `+0x44`: 1 open, 0 closed.
    pub open_fraction: f32,
    /// `+0x48` "fly_time" (0.5); read by nothing.
    pub fly_time: f32,
    /// `+0x4c`, `+0x4d`.
    pub open_request: bool,
    pub close_request: bool,
    /// `+0x4e` "static" (1).
    pub static_noise: bool,
    /// `+0x50` "corner": the corner mesh's path (the mesh and its palette
    /// colour come with the comm's draw).
    pub corner: Option<Vec<u8>>,
}

impl Default for CommWindow {
    fn default() -> Self {
        Self {
            open: false,
            rect: [0, 0, 1, 1],
            win_pos: Vector::new(0.3, 0.3, -1.0),
            btn_pos: Vector::new(0.0, 0.0, -2.0),
            win_size: (0.2, 0.2),
            open_fraction: 0.0,
            fly_time: 0.5,
            open_request: false,
            close_request: false,
            static_noise: true,
            corner: None,
        }
    }
}

/// `get_value_float(index)` as the float the reference stores.
fn float(reader: &mut IniReader, index: u32) -> f32 {
    reader.value_float(index) as f32
}

impl CommWindow {
    /// `FCommWindow::ReadIni`: whether the key is one of the window's.
    pub fn read_ini(&mut self, reader: &mut IniReader) -> bool {
        if reader.is_value(b"corner") {
            self.corner = Some(reader.value_string());
            return true;
        }
        if reader.is_value(b"btn_pos") || reader.is_value(b"win_pos") {
            // The reference reads value 1 first, then 0, then 2.
            let y = float(reader, 1);
            let x = float(reader, 0);
            let z = float(reader, 2);
            let v = Vector::new(x, y, z);
            if reader.is_value(b"btn_pos") {
                self.btn_pos = v;
            } else {
                self.win_pos = v;
            }
            return true;
        }
        if reader.is_value(b"win_size") {
            let x = float(reader, 0);
            let y = float(reader, 1);
            self.win_size = (x, y);
            return true;
        }
        if reader.is_value(b"fly_time") {
            self.fly_time = float(reader, 0);
            return true;
        }
        if reader.is_value(b"static") {
            // `FCOMP 0.0`: 0 only when equal to zero.
            self.static_noise = float(reader, 0) != 0.0;
            return true;
        }
        false
    }
}

/// The portrait camera's settings (`FGameCamera`, `+0x4b8`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PortraitCamera {
    /// "video_fovx" (`SpaceCameraSetHalfFovX`; 35 from `GameCameraConstruct`).
    pub half_fov_x: f32,
    pub z_near: f32,
    pub z_far: f32,
}

/// The receiver (`HUD_Receiver`'s `Win3D` face) and what its section set.
#[derive(Debug)]
pub struct HudReceiver {
    /// Channel A's window.
    pub window: CommWindow,
    /// `+0x47c` "speaker_rotate" (`EulerMatrix`; zero until the file sets
    /// it). Channel A takes a copy.
    pub speaker_rotate: Matrix,
    /// `+0x4a0` "male_speaker_offset", `+0x4ac` "female_speaker_offset".
    pub male_offset: Vector,
    pub female_offset: Vector,
    pub camera: PortraitCamera,
    /// `+0x474` "backdrop": the texture's name (`TextureRefLoad`).
    pub backdrop: Option<Vec<u8>>,
    /// `+0x5b4`.
    pub low_end_comm_button: Option<NodeId>,
}

impl Default for HudReceiver {
    fn default() -> Self {
        Self {
            window: CommWindow::default(),
            speaker_rotate: Matrix::from_rows([[0.0; 3]; 3]),
            male_offset: Vector::ZERO,
            female_offset: Vector::ZERO,
            camera: PortraitCamera {
                half_fov_x: 35.0,
                z_near: 1.4,
                z_far: 50.0,
            },
            backdrop: None,
            low_end_comm_button: None,
        }
    }
}

/// The three floats of a vector key.
fn vector(reader: &mut IniReader) -> Vector {
    let x = float(reader, 0);
    let y = float(reader, 1);
    let z = float(reader, 2);
    Vector::new(x, y, z)
}

impl HudReceiver {
    /// The constructor's section loop: the window's keys first, then the
    /// speaker's pose, the camera's angle and the backdrop. The other keys
    /// of the shipped section (`object`, `action`, `shut`, `open`, `delay`,
    /// `tool_tip_id`) are read by nothing. Then channel A's window takes its
    /// fixed spot (-1.068, 0.65, -2.392).
    pub fn read(reader: &mut IniReader, warn: &mut dyn FnMut(String)) -> Self {
        let mut this = Self::default();
        while reader.read_value() {
            if this.window.read_ini(reader) {
                continue;
            }
            if reader.is_value(b"speaker_offset") {
                warn(format!(
                    "{HUD_COMM_CPP}(2023) : *** WARNING: speaker_offset is obsolete in hud ini \
                     file {}, use male_speaker_offset or female_speaker_offset instead\n",
                    String::from_utf8_lossy(reader.file_name())
                ));
            } else if reader.is_value(b"male_speaker_offset") {
                this.male_offset = vector(reader);
            } else if reader.is_value(b"female_speaker_offset") {
                this.female_offset = vector(reader);
            } else if reader.is_value(b"speaker_rotate") {
                this.speaker_rotate = euler_matrix(vector(reader));
            } else if reader.is_value(b"video_fovx") {
                this.camera.half_fov_x = float(reader, 0);
            } else if reader.is_value(b"backdrop") {
                this.backdrop = Some(reader.value_string());
            }
        }
        this.window.win_pos = Vector::new(-1.068, 0.65, -2.392);
        this
    }
}

impl Widget for HudReceiver {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
    /// `HUD_Receiver::Update`, the button's half: shown only while channel
    /// A's window is open (and the portrait is off or not drawn), hidden
    /// otherwise; then, unless the local server's tick is skipped, the child
    /// walk. The channels' tick follows with the comm.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(button) = self.low_end_comm_button
            && let Some(n) = ui.node_mut(button)
        {
            // The window never opens before the channels are ported, so the
            // portrait's performance slot is not read.
            if self.window.open {
                n.flags |= ACTIVE | VISIBLE;
            } else {
                n.flags &= !(ACTIVE | VISIBLE);
            }
        }
        if ui.services.skip_local_server_tick() {
            return;
        }
        ui.base_update(id);
    }
    /// Slot 12 is `Update`.
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        self.update(ui, id);
    }
}

/// The element (`HUD_Receiver`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudReceiverItem {
    pub window: NodeId,
}

impl HudItem for HudReceiverItem {
    fn name(&self) -> &[u8] {
        NAME
    }
    fn type_name(&self) -> &'static str {
        "class HUD_Receiver"
    }
    /// `HUD_Receiver::Init` zeroes both comm inboxes (not ported yet,
    /// D1115) and answers true.
    fn init(&mut self, _ui: &mut Ui) -> bool {
        true
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// `HUDFACTORY<HUD_Receiver>::Create`: the constructor. The object is put
/// in no tree and registered, active and visible (`Flags & 0xfb | 3`),
/// without `Create`; it reads its section, then makes the low-detail comm
/// button (`ButtonControl::Init` at (-0.1617, 0.0986, -0.362), inactive and
/// hidden). The section's warnings are dropped (the loader's hook takes
/// only its own).
pub fn create(reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let mut warnings = Vec::new();
    create_with(reader, ui, &mut |w| warnings.push(w))
}

/// [`create`] with the section's warnings to `warn`.
pub fn create_with(
    reader: &mut IniReader,
    ui: &mut Ui,
    warn: &mut dyn FnMut(String),
) -> Option<Box<dyn HudItem>> {
    let id = ui.alloc(Some(Box::new(HudReceiver::default())));
    ui.reparent(id, None);
    ui.register(id);
    if let Some(n) = ui.node_mut(id) {
        n.flags = (n.flags & !UNREGISTERED) | ACTIVE | VISIBLE;
    }
    let mut read = HudReceiver::read(reader, warn);
    let button = create_button_control(
        ui,
        id,
        BUTTON_NAME,
        Some(BUTTON_MESH),
        &ButtonInit {
            position: Vector::new(-0.1617, 0.0986, -0.362),
            half_extents: Vector::ZERO,
            ..ButtonInit::default()
        },
    );
    if let Some(button) = button
        && let Some(n) = ui.node_mut(button)
    {
        n.flags &= !(ACTIVE | VISIBLE);
    }
    read.low_end_comm_button = button;
    if let Some(w) = ui.widget_mut::<HudReceiver>(id) {
        *w = read;
    }
    Some(Box::new(HudReceiverItem { window: id }))
}

#[cfg(test)]
mod tests {
    use neuron_client::ui::widescreen::Surface;

    use super::*;

    const SECTION_TEXT: &str = "[Receiver]\nobject = interface\\HUD\\hud_CommsIcon.3db\n\
        shut = -0.497999996, 0.365999013, -1\ndelay = 0.150000006\nvideo_fovx = 5\n\
        speaker_rotate = -180, 0, 0\nmale_speaker_offset = 0, -1.25, -2.1500001\n\
        female_speaker_offset = 0, -1.14999998, -2.1500001\nbackdrop = backdrop\nstatic = 0\n\
        win_pos = -0.400000006, 0.25, -1\nwin_size = 0.200000003, 0.3\nbtn_pos = 1, 2, -1\n\
        fly_time = 1\ncorner = a.3db\nspeaker_offset = 1, 2, 3\ntool_tip_id = 1021\n";

    fn reader(text: &str) -> IniReader {
        let mut reader = IniReader::new();
        assert!(reader.open(b"hud.ini", text.as_bytes().to_vec()));
        assert!(reader.read_header());
        reader
    }

    #[test]
    fn the_section_sets_the_window_the_speaker_and_the_camera() {
        let mut warnings = Vec::new();
        let r = HudReceiver::read(&mut reader(SECTION_TEXT), &mut |w| warnings.push(w));
        assert_eq!(r.camera.half_fov_x, 5.0);
        assert_eq!((r.camera.z_near, r.camera.z_far), (1.4, 50.0));
        assert_eq!(
            r.speaker_rotate,
            euler_matrix(Vector::new(-180.0, 0.0, 0.0))
        );
        assert_eq!(
            r.male_offset,
            Vector::new(0.0, -1.25, -2.150_000_1_f64 as f32)
        );
        assert_eq!(
            r.female_offset,
            Vector::new(0.0, -1.15, -2.150_000_1_f64 as f32)
        );
        assert_eq!(r.backdrop.as_deref(), Some(&b"backdrop"[..]));
        assert!(!r.window.static_noise);
        assert_eq!(r.window.win_size, (0.2, 0.3));
        assert_eq!(r.window.btn_pos, Vector::new(1.0, 2.0, -1.0));
        assert_eq!(r.window.fly_time, 1.0);
        assert_eq!(r.window.corner.as_deref(), Some(&b"a.3db"[..]));
        // The file's `win_pos` gives way to the fixed spot.
        assert_eq!(r.window.win_pos, Vector::new(-1.068, 0.65, -2.392));
        assert_eq!(
            warnings,
            [
                "Client\\Space\\HUD_Comm.cpp(2023) : *** WARNING: speaker_offset is obsolete in \
                 hud ini file hud.ini, use male_speaker_offset or female_speaker_offset instead\n"
            ]
        );
    }

    #[test]
    fn an_empty_section_keeps_the_defaults_but_the_window_spot() {
        let r = HudReceiver::read(&mut reader("[Receiver]\n"), &mut |_| {});
        assert_eq!(r.speaker_rotate, Matrix::from_rows([[0.0; 3]; 3]));
        assert_eq!(r.camera.half_fov_x, 35.0);
        assert_eq!(
            CommWindow {
                win_pos: Vector::new(0.3, 0.3, -1.0),
                ..r.window
            },
            CommWindow::default()
        );
    }

    #[test]
    fn the_receiver_is_a_nameless_root_with_a_hidden_comm_button() {
        let mut ui = Ui::new(Surface::new(1024, 768));
        let mut item = create(&mut reader("[Receiver]\n"), &mut ui).expect("an element");
        assert_eq!(item.name(), NAME);
        assert!(item.init(&mut ui));
        let id = item.window().unwrap();
        let node = ui.node(id).unwrap();
        assert_eq!(
            (node.name(), node.class_name, node.flags),
            (&b""[..], None, 0x0b)
        );
        assert!(ui.win_control(id).is_none());
        let button = ui.widget::<HudReceiver>(id).unwrap().low_end_comm_button;
        let button = button.expect("the button");
        assert_eq!(ui.children(id), [button]);
        let b = ui.node(button).unwrap();
        assert_eq!(
            (b.name(), b.class_name, b.flags),
            (BUTTON_NAME, Some("ButtonControl"), 0x08)
        );
        let c = ui.win_control(button).unwrap();
        assert_eq!(c.offset, Vector::new(-0.1617, 0.0986, -0.362));
        assert_eq!(c.mesh_path, BUTTON_MESH);
        ui.update_all(0.0);
        assert_eq!(ui.node(button).unwrap().flags, 0x08);
        assert_eq!(ui.node(id).unwrap().flags, 0x0b);
    }
}
