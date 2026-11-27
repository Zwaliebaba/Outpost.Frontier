//! A room's file as the client reads it (`RoomData::read_from_ini` and its
//! block readers, `Common/common_roomdata.cpp`; `SetpointProperties::
//! extract_from_name`, `Common/common_roominfo.cpp`), work package P5.9.
//!
//! `[Room]` (or `[Room_Info]`) names the set script, whose entities give the
//! room's cameras, lights, props, setpoints and character placements, the
//! scene's ambient colour and the shadow and glare lights; its other keys
//! give the animations, the goods cart script, the shadow cut-off and the
//! scenes. `[Sound]` gives the music and the ambient sound, `[Camera]` the
//! camera's name, `[CharacterPlacement]` a placement's start script,
//! `[PlayerShipPlacement]` and `[ForSaleShipPlacement]` the ship
//! placements, `[Hotspot]` the hotspots and `[FlashlightSet]` and
//! `[FlashlightLine]` the flashlight sets. The obsolete headers and keys
//! only report.
//!
//! The set script runs on the client's scene-script engine (`SceneProgramLoad`:
//! a fresh engine's `Slot4Load`), read through the entities' descriptors
//! (`Slot9Entity`) and user properties (`Slot33UserProp`). The server's
//! subset of this file (the placements) is `game_sim::data::room`.

use game_sim::data::{DataSource, Reports, open_ini};
use neuron_client::scene_script::playback::Player;
use neuron_client::scene_script::{ScriptModule, build, load};
use neuron_core::hash::eq_ignore_case;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector, transform_matrix};
use neuron_core::num::trunc_to_i64;

const ROOM_DATA: &str = "Common\\RoomData.cpp";
const SCENE_PROGRAM: &str = "Common\\SceneProgram.cpp";
const SETPOINT: &str = "Common\\SetpointProperties.cpp";
const PETAL: &str = "Common\\PetalDB.cpp";

/// The scene-script entity types the set script files (`kSceneScriptEntity*`).
mod kind {
    pub const COMPOUND: i32 = 1;
    pub const CAMERA: i32 = 3;
    pub const LIGHT: i32 = 5;
    pub const MARKER: i32 = 7;
    pub const SCENE: i32 = 9;
}

/// A position and an orientation (`Csys`), the 3x3 row by row.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Frame {
    pub pos: [f32; 3],
    pub orient: [f32; 9],
}

impl Frame {
    /// `Csys::set_identity`.
    pub const IDENTITY: Frame = Frame {
        pos: [0.0; 3],
        orient: [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
    };

    /// The twelve words: the position, then the 3x3.
    #[must_use]
    pub fn words(&self) -> [f32; 12] {
        let mut w = [0.0; 12];
        w[..3].copy_from_slice(&self.pos);
        w[3..].copy_from_slice(&self.orient);
        w
    }
}

/// `SetpointProperties`: what a setpoint's name encodes, with the
/// constructor's defaults for what it does not.
#[derive(Clone, Debug, PartialEq)]
pub struct SetpointProps {
    /// The name has the `zs` or `zg` prefix.
    pub valid: bool,
    pub zone_script: bool,
    /// `pc` 1, `npc` 0, `none` -1, `rpc` 2; -1 by default.
    pub char_type: i32,
    /// The third token, lowered.
    pub segment: Option<Vec<u8>>,
    /// The fourth token, `strtol` base 10; -1 by default.
    pub zone: i32,
    /// `NameBuffer`: the fifth token, upper-cased (the 16 bytes the
    /// reference's buffer holds).
    pub name: Vec<u8>,
    /// `stand` 0, `sitlow` 1, `sithigh` 2, anything else 3; -1 by default.
    pub posture: i32,
}

impl Default for SetpointProps {
    fn default() -> Self {
        SetpointProps {
            valid: false,
            zone_script: false,
            char_type: -1,
            segment: None,
            zone: -1,
            name: Vec::new(),
            posture: -1,
        }
    }
}

/// `strtol(text, &end, 10)`: the value (clamped as the CRT clamps) and
/// whether the whole text was read.
fn strtol(text: &[u8]) -> (i32, bool) {
    let mut at = text
        .iter()
        .position(|b| !matches!(b, b' ' | b'\t'..=b'\r'))
        .unwrap_or(text.len());
    let negative = text.get(at) == Some(&b'-');
    if matches!(text.get(at), Some(b'-' | b'+')) {
        at += 1;
    }
    let start = at;
    let mut value: i64 = 0;
    while let Some(d) = text.get(at).filter(|b| b.is_ascii_digit()) {
        value = (value * 10 + i64::from(d - b'0')).min(i64::from(i32::MAX) + 1);
        at += 1;
    }
    if at == start {
        // No digits: `end` is the text itself.
        return (0, text.is_empty());
    }
    let value = if negative { -value } else { value };
    let clamped = value.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32;
    (clamped, at == text.len())
}

impl SetpointProps {
    /// `extract_from_name` on a fresh object. A name with a missing token
    /// is reported; the reference then compares or copies the null token
    /// and crashes, where the port takes an empty one.
    #[must_use]
    pub fn from_name(full: &[u8], reports: &mut Reports) -> SetpointProps {
        let mut props = SetpointProps::default();
        let lowered = full.to_ascii_lowercase();
        let mut tokens = lowered
            .split(|&b| b == b'/')
            .filter(|t| !t.is_empty())
            .map(<[u8]>::to_vec);
        let prefix = tokens.next();
        let char_type = tokens.next();
        let segment = tokens.next();
        let zone = tokens.next();
        let name = tokens.next();
        let posture = tokens.next().unwrap_or_default();
        let Some(prefix) = prefix.filter(|p| p == b"zs" || p == b"zg") else {
            return props;
        };
        props.valid = true;
        let shown = lossy(full);
        if char_type.is_none() || segment.is_none() || zone.is_none() || name.is_none() {
            reports.push(
                SETPOINT,
                0x78,
                format!("*** ERROR: setpoint name {shown} does not match naming convention"),
            );
        }
        props.zone_script = prefix == b"zs";
        let char_type = char_type.unwrap_or_default();
        match char_type.as_slice() {
            b"pc" => props.char_type = 1,
            b"npc" => props.char_type = 0,
            b"none" => props.char_type = -1,
            b"rpc" => props.char_type = 2,
            other => reports.push(
                SETPOINT,
                0x9e,
                format!(
                    "*** ERROR: setpoint character type {} (of {shown}) is not pc, npc, none, or rpc",
                    lossy(other)
                ),
            ),
        }
        props.segment = Some(segment.unwrap_or_default());
        let zone = zone.unwrap_or_default();
        let (value, whole) = strtol(&zone);
        props.zone = value;
        if !whole {
            reports.push(
                SETPOINT,
                0xac,
                format!(
                    "*** ERROR: setpoint zone number {} (of {shown}) is not a number",
                    lossy(&zone)
                ),
            );
        }
        // `set_segment`: `strcpy` into the 16-byte buffer, then upper-cased.
        let mut upper = name.unwrap_or_default().to_ascii_uppercase();
        upper.truncate(16);
        props.name = upper;
        props.posture = match posture.as_slice() {
            b"stand" => 0,
            b"sitlow" => 1,
            b"sithigh" => 2,
            other => {
                if !props.zone_script {
                    reports.push(
                        SETPOINT,
                        0xc8,
                        format!(
                            "*** ERROR: invalid posture {} in setpoint marker {shown} - if this is a special relocation marker, mark it with Zs",
                            lossy(other)
                        ),
                    );
                }
                3
            }
        };
        props
    }
}

/// `RoomCameraInfo`.
#[derive(Clone, Debug, PartialEq)]
pub struct RoomCamera {
    pub name: Option<Vec<u8>>,
    pub frame: Frame,
    pub fov: f32,
    pub near: f32,
    pub far: f32,
}

/// `RoomLightInfo`: a light of the set script that is on.
#[derive(Clone, Debug, PartialEq)]
pub struct RoomLight {
    pub frame: Frame,
    pub direction: [f32; 3],
    /// The 26 words of the light from its type (`D3DLIGHT8`): the type,
    /// the diffuse colour and the rest of the light's properties.
    pub d3d: [u32; 26],
    /// The entity's light group byte.
    pub light_group: u8,
}

/// `RoomPropInfo`: a compound of the set script.
#[derive(Clone, Debug, PartialEq)]
pub struct RoomProp {
    pub template: Option<Vec<u8>>,
    pub name: Option<Vec<u8>>,
    pub category: i32,
    pub frame: Frame,
    pub visible: bool,
    pub lit: bool,
    pub fog: bool,
    pub room_main: bool,
    pub light_group: u8,
    pub layer: u8,
    pub render_type: u32,
    pub loadout: Vec<u8>,
    pub priority: u32,
    pub running_lights: bool,
}

/// `SetpointInfo`: a camera or marker of the set script.
#[derive(Clone, Debug, PartialEq)]
pub struct Setpoint {
    pub name: Option<Vec<u8>>,
    pub frame: Frame,
    pub props: SetpointProps,
}

/// `CharPlaceInfo`: a place a character can stand.
#[derive(Clone, Debug, PartialEq)]
pub struct CharPlacement {
    pub name: Option<Vec<u8>>,
    pub frame: Frame,
    pub char_type: i32,
    pub posture: i32,
    pub zone_script: bool,
    pub start_script: Option<Vec<u8>>,
}

/// `ShipPlaceInfo`: `[PlayerShipPlacement]` or `[ForSaleShipPlacement]`.
#[derive(Clone, Debug, PartialEq)]
pub struct ShipPlacement {
    pub nickname: Option<Vec<u8>>,
    pub frame: Frame,
    pub player_ship: bool,
    pub for_sale: bool,
    pub landing_script: Option<Vec<u8>>,
    pub launching_script: Option<Vec<u8>>,
}

/// `RoomButtonInfo`: a `[Hotspot]`.
#[derive(Clone, Debug, PartialEq)]
pub struct Hotspot {
    pub name: Option<Vec<u8>>,
    pub room_switch: Option<Vec<u8>>,
    pub virtual_room: Option<Vec<u8>>,
    pub set_virtual_room: Option<Vec<u8>>,
    /// The behaviour's option value, -1 for none.
    pub behavior: i32,
    pub state_send: u32,
    pub state_read: u32,
    pub state_transitions: [Option<Vec<u8>>; 4],
}

/// `FlashLightSetInfo` as the file gives it (the frames come with
/// `fixup`).
#[derive(Clone, Debug, PartialEq)]
pub struct FlashLightSet {
    /// Colour, scale, gap, blink and end pause.
    pub params: [f32; 7],
    /// 1 for a set, 2 for a line.
    pub mode: i32,
    /// `HardpointNames` and `SetpointNames`: one slot per light, the two
    /// keys sharing the count.
    pub hardpoints: Vec<Option<Vec<u8>>>,
    pub setpoints: Vec<Option<Vec<u8>>>,
    /// `NumLightsAlt` ("numlights" of a line).
    pub num_lights_alt: i32,
}

impl Default for FlashLightSet {
    /// `FlashLightSetInfo::clear`.
    fn default() -> Self {
        FlashLightSet {
            params: [1.0, 1.0, 1.0, 2.0, 0.1, 0.2, 1.0],
            mode: 0,
            hardpoints: Vec::new(),
            setpoints: Vec::new(),
            num_lights_alt: 0,
        }
    }
}

/// `AmbientScriptInfo`: a `scene =` line.
#[derive(Clone, Debug, PartialEq)]
pub struct RoomScene {
    /// 1 ambient, 2 traffic, 3 once.
    pub kind: i32,
    pub path: Vec<u8>,
    pub priority: u32,
}

/// A named light of the set script (`LtShadowSource`, `LtGlareSource`).
#[derive(Clone, Debug, PartialEq)]
pub struct NamedLight {
    pub on: bool,
    /// The light is directional: `direction` holds its world direction.
    pub world_dir: bool,
    pub position: [f32; 3],
    pub direction: [f32; 3],
    pub color: [f32; 3],
}

impl Default for NamedLight {
    fn default() -> Self {
        NamedLight {
            on: false,
            world_dir: true,
            position: [0.0; 3],
            direction: [0.0, 1.0, 0.0],
            color: [0.0; 3],
        }
    }
}

/// `RoomData` after `load` (`read_from_ini`), with `clear`'s defaults for
/// what the file does not give.
#[derive(Clone, Debug, PartialEq)]
pub struct RoomData {
    /// The `animation` (true: loops) and `animation_oneshot` names.
    pub animations: Vec<(Vec<u8>, bool)>,
    pub goodscart_script: Option<Vec<u8>>,
    pub goodscart_priority: u32,
    pub cutoff_shadow: Option<f32>,
    pub extra_shadow_height: f32,
    pub music: u32,
    pub music_loop: bool,
    pub ambient: u32,
    pub ambient_loop: bool,
    pub ambient_color: [f32; 3],
    pub shadow_light: NamedLight,
    pub glare_light: NamedLight,
    pub cameras: Vec<RoomCamera>,
    pub lights: Vec<RoomLight>,
    pub props: Vec<RoomProp>,
    pub setpoints: Vec<Setpoint>,
    pub characters: Vec<CharPlacement>,
    pub ships: Vec<ShipPlacement>,
    pub hotspots: Vec<Hotspot>,
    pub flashlights: Vec<FlashLightSet>,
    pub scenes: Vec<RoomScene>,
    /// `[Camera]` `name`.
    pub camera_name: Option<Vec<u8>>,
}

impl Default for RoomData {
    fn default() -> Self {
        RoomData {
            animations: Vec::new(),
            goodscart_script: None,
            goodscart_priority: 0,
            cutoff_shadow: None,
            extra_shadow_height: 0.0,
            music: 0,
            music_loop: true,
            ambient: 0,
            ambient_loop: true,
            ambient_color: [1.0; 3],
            shadow_light: NamedLight::default(),
            glare_light: NamedLight::default(),
            cameras: Vec::new(),
            lights: Vec::new(),
            props: Vec::new(),
            setpoints: Vec::new(),
            characters: Vec::new(),
            ships: Vec::new(),
            hotspots: Vec::new(),
            flashlights: Vec::new(),
            scenes: Vec::new(),
            camera_name: None,
        }
    }
}

/// A name for a report.
fn lossy(bytes: &[u8]) -> String {
    String::from_utf8_lossy(neuron_core::hash::c_str(bytes)).into_owned()
}

/// `CreateID`.
fn id(text: &[u8]) -> u32 {
    neuron_core::id::create_id(text)
}

/// `get_value_float(i)`: the reader's number rounded to a float.
fn float(ini: &mut IniReader, index: u32) -> f32 {
    ini.value_float(index) as f32
}

/// `PetalInterfaceDatabase::get_category`: -1 for an empty or unknown name
/// (the unknown one reported).
fn petal_category(name: &[u8], reports: &mut Reports) -> i32 {
    const NAMES: [&[u8]; 7] = [
        b"Room",
        b"Spaceship",
        b"Solar",
        b"Asteroid",
        b"Equipment",
        b"Equipment Cart",
        b"Prop",
    ];
    if name.is_empty() {
        return -1;
    }
    match NAMES.iter().position(|n| *n == name) {
        Some(i) => i as i32,
        None => {
            reports.push(
                PETAL,
                0xbb,
                format!("*** WARNING: Unknown Petal category name {}", lossy(name)),
            );
            -1
        }
    }
}

/// `select_option_from_string` with `read_Hotspot_block`'s behaviours: the
/// text up to the first delimiter of `strtok(",;/ ")`, compared with each
/// option (`stricmp`); -1 for none.
fn hotspot_behavior(text: &[u8]) -> i32 {
    const OPTIONS: [(&[u8], i32); 12] = [
        (b"ExitDoor", 1),
        (b"NewsVendor", 2),
        (b"MissionVendor", 3),
        (b"Launch", 4),
        (b"StartDealer", 5),
        (b"StartShipDealer", 7),
        (b"FrontDesk", 6),
        (b"StartEquipDealer", 8),
        (b"MoveRight", 9),
        (b"MoveLeft", 10),
        (b"VirtualRoom", 11),
        (b"Repair", 12),
    ];
    let delim = |b: &u8| matches!(b, b',' | b';' | b'/' | b' ');
    let Some(start) = text.iter().position(|b| !delim(b)) else {
        return -1;
    };
    // `strtok` ends the token at its first delimiter; the compare reads
    // the string from its start.
    let end = text[start..]
        .iter()
        .position(delim)
        .map_or(text.len(), |i| start + i);
    let compared = &text[..end];
    OPTIONS
        .iter()
        .find(|(name, _)| eq_ignore_case(compared, name))
        .map_or(-1, |&(_, value)| value)
}

/// `_ftol` of the reader's unrounded number, the low 32 bits kept.
fn ftol_u32(value: f64) -> u32 {
    trunc_to_i64(value) as u32
}

/// The 26 words of a light from its type.
fn d3d_words(l: &neuron_client::scene_script::entity::LightProps) -> [u32; 26] {
    let mut w = Vec::with_capacity(26);
    w.push(l.light_type as u32);
    w.extend(l.diffuse.iter().map(|v| v.to_bits()));
    w.extend(l.specular.iter().map(|v| v.to_bits()));
    w.extend(l.ambient.iter().map(|v| v.to_bits()));
    w.extend(l.unused_17.iter().map(|&v| v as u32));
    w.push(l.range.to_bits());
    w.push(l.unused_1e);
    w.extend(l.atten.iter().map(|v| v.to_bits()));
    w.push(l.theta.to_bits());
    w.push(l.cutoff.to_bits());
    w.try_into().unwrap_or([0; 26])
}

/// `TurnLightDirection`: the direction turned by the frame's 3x3
/// (`TransformMatrix`) and normalised by `1 / sqrt` in double.
fn turn_light_direction(frame: &Frame, dir: [f32; 3]) -> [f32; 3] {
    let o = frame.orient;
    let m = Matrix {
        m: [[o[0], o[1], o[2]], [o[3], o[4], o[5]], [o[6], o[7], o[8]]],
    };
    let t = transform_matrix(&m, Vector::new(dir[0], dir[1], dir[2]));
    let (x, y, z) = (f64::from(t.x), f64::from(t.y), f64::from(t.z));
    let inv = 1.0 / (x * x + y * y + z * z).sqrt();
    [(x * inv) as f32, (y * inv) as f32, (z * inv) as f32]
}

/// `AmbientChannel`: a scene word as a signed int times 1/255 in double.
fn ambient_channel(word: u32) -> f32 {
    const INV_255: f64 = 0.003_921_568_627_450_98;
    (f64::from(word.cast_signed()) * INV_255) as f32
}

impl RoomData {
    /// `RoomData::load` on a room not yet loaded (`read_from_ini(File)`):
    /// a file that cannot be opened gives the defaults. The set scripts run
    /// in `module`, the client's scene-script module.
    #[must_use]
    pub fn read(
        source: &dyn DataSource,
        path: &[u8],
        module: &mut ScriptModule,
        reports: &mut Reports,
    ) -> RoomData {
        let mut room = RoomData::default();
        let Some(mut ini) = open_ini(source, path) else {
            return room;
        };
        let file = lossy(path);
        while ini.read_header() {
            if ini.is_header(b"Room") || ini.is_header(b"Room_Info") {
                room.read_room_info(&mut ini, source, module, reports);
            } else if ini.is_header(b"Room_Sound") || ini.is_header(b"Sound") {
                room.read_sound(&mut ini);
            } else if ini.is_header(b"Camera") {
                room.read_camera(&mut ini, reports);
            } else if ini.is_header(b"CharacterPlacement") {
                room.read_character_placement(&mut ini, reports);
            } else if ini.is_header(b"PlayerShipPlacement") {
                room.read_ship_placement(&mut ini, true, reports);
            } else if ini.is_header(b"ForSaleShipPlacement") {
                room.read_ship_placement(&mut ini, false, reports);
            } else if ini.is_header(b"Hotspot") {
                room.read_hotspot(&mut ini, reports);
            } else if ini.is_header(b"FlashlightSet") {
                room.read_flashlights(&mut ini, 1);
            } else if ini.is_header(b"FlashlightLine") {
                room.read_flashlights(&mut ini, 2);
            } else {
                let obsolete: &[(&[u8], u32)] = &[
                    (b"Cloud", 0x7fe),
                    (b"Fog", 0x807),
                    (b"GoodsCartPlacement", 0x813),
                    (b"RepairRobotPlacement", 0x817),
                    (b"GoodsPilePlacement", 0x81b),
                    (b"ShipPlacement", 0x81f),
                    (b"MonitorPlacement", 0x82b),
                    (b"Effect", 0x82f),
                    (b"AvailableShip", 0x833),
                    (b"Billboard", 0x843),
                    (b"Light", 0x847),
                    (b"Lighting", 0x84b),
                ];
                if let Some(&(name, line)) = obsolete.iter().find(|(n, _)| ini.is_header(n)) {
                    reports.push(
                        ROOM_DATA,
                        line,
                        format!(
                            "*** WARNING: [{}] is obsolete in {file}",
                            String::from_utf8_lossy(name)
                        ),
                    );
                }
            }
        }
        room
    }

    /// `find_setpoint`: the last setpoint of that name (`stricmp`), the
    /// miss reported.
    #[must_use]
    pub fn find_setpoint(
        &self,
        name: &[u8],
        script: &[u8],
        reports: &mut Reports,
    ) -> Option<&Setpoint> {
        let found = self
            .setpoints
            .iter()
            .rev()
            .find(|s| s.name.as_deref().is_some_and(|n| eq_ignore_case(name, n)));
        if found.is_none() {
            reports.push(
                ROOM_DATA,
                0x5a8,
                format!(
                    "*** ERROR: script {} calls for setpoint {} which does not exist in room .ini",
                    lossy(script),
                    lossy(name)
                ),
            );
        }
        found
    }

    fn read_room_info(
        &mut self,
        ini: &mut IniReader,
        source: &dyn DataSource,
        module: &mut ScriptModule,
        reports: &mut Reports,
    ) {
        while ini.read_value() {
            if ini.is_value(b"set_script") {
                let script = ini.value_string();
                self.read_set_script(source, &script, module, reports);
            } else if ini.is_value(b"animation") || ini.is_value(b"animation_oneshot") {
                let looping = ini.is_value(b"animation");
                self.animations.push((ini.value_string(), looping));
            } else if ini.is_value(b"goodscart_script") {
                self.goodscart_script = Some(ini.value_string());
                self.goodscart_priority = 0;
                if !ini.is_value_empty(1) {
                    self.goodscart_priority = id(&ini.value_string_at(1));
                }
            } else if ini.is_value(b"cutoff_shadow_z") {
                self.cutoff_shadow = Some(float(ini, 0));
            } else if ini.is_value(b"extra_shadow_height_offset") {
                self.extra_shadow_height = float(ini, 0);
            } else if ini.is_value(b"scene") {
                self.read_ambient_script(ini, reports);
            } else {
                let obsolete: &[(&[u8], u32)] = &[
                    (b"mesh", 0x3d7),
                    (b"background_script", 0x3f5),
                    (b"background_script_oneshot", 0x3f9),
                    (b"mission_vendor_type", 0x3fd),
                    (b"goodscart_script_fighter", 0x410),
                    (b"goodscart_script_elite", 0x414),
                    (b"goodscart_script_freighter", 0x418),
                    (b"news_material", 0x427),
                    (b"is_outside", 0x42b),
                ];
                if let Some(&(name, line)) = obsolete.iter().find(|(n, _)| ini.is_value(n)) {
                    reports.push(
                        ROOM_DATA,
                        line,
                        format!(
                            "*** WARNING: {} is obsolete in {}",
                            String::from_utf8_lossy(name),
                            lossy(ini.file_name())
                        ),
                    );
                }
            }
        }
    }

    /// `read_ambient_script`: `scene = [all,] ambient|traffic|once,
    /// script.thn [, priority]`.
    fn read_ambient_script(&mut self, ini: &mut IniReader, reports: &mut Reports) {
        let mut values = Vec::new();
        while values.len() < 4 && !ini.is_value_empty(values.len() as u32) {
            values.push(ini.value_string_at(values.len() as u32));
        }
        let first = usize::from(values.first().is_some_and(|v| eq_ignore_case(v, b"all")));
        let count = values.len() - first;
        let file = lossy(ini.file_name());
        if count != 2 && count != 3 {
            reports.push(
                ROOM_DATA,
                0x38a,
                format!("*** ERROR: Invalid scene = specification in [Room] block in file {file}"),
            );
            return;
        }
        let kind_name = &values[first];
        let kind = if eq_ignore_case(kind_name, b"ambient") {
            1
        } else if eq_ignore_case(kind_name, b"traffic") {
            reports.push(
                ROOM_DATA,
                0x3a3,
                format!("*** WARNING: traffic ambient is obsolete in room .ini {file}"),
            );
            2
        } else if eq_ignore_case(kind_name, b"once") {
            3
        } else {
            reports.push(
                ROOM_DATA,
                0x3ad,
                format!("*** ERROR: Invalid scene = specification in [Room] block in file {file}"),
            );
            return;
        };
        let priority = if count == 3 {
            id(&values[first + 2])
        } else {
            0
        };
        self.scenes.push(RoomScene {
            kind,
            path: values[first + 1].clone(),
            priority,
        });
    }

    /// `read_set_script`: every entity of the script filed by type, then a
    /// placement per setpoint that follows the `zs`/`zg` convention. A
    /// script that cannot be read gives nothing, not even the placement
    /// pass; one that cannot be run gives no entities.
    fn read_set_script(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        module: &mut ScriptModule,
        reports: &mut Reports,
    ) {
        if path.is_empty() {
            reports.push(
                SCENE_PROGRAM,
                0x18,
                "*** WARNING: Skipping null script filename...",
            );
            return;
        }
        let Some(bytes) = source.read(path) else {
            reports.push(
                SCENE_PROGRAM,
                0x30,
                format!("*** ERROR: Cannot load script file {}", lossy(path)),
            );
            return;
        };
        // `SceneProgramLoad`: a fresh engine's `Slot4Load`, whose result
        // nobody tests.
        let mut player = Player::new();
        if let Ok(summary) = load(&mut module.vm, &bytes)
            && let Ok(scene) = build(&mut module.vm, &mut module.state, &bytes, summary, false)
        {
            player.load(&scene, summary.duration_ms);
        }
        let handles: Vec<i32> = player.entity_handles().collect();
        let count = player.live_entity_count() as i32;
        for index in 0..count {
            // `Slot9Entity(Index)` takes the key, `Slot7EntityHandle(Index)`
            // the ordinal's key; the shipped scripts number them alike.
            let Some(e) = player.entity_snap(index) else {
                continue;
            };
            let handle = handles.get(index as usize).copied().unwrap_or(-1);
            let user = |key: &[u8]| player.user_prop(handle, key).map(<[u8]>::to_vec);
            let frame = Frame {
                pos: e.pos,
                orient: e.orient,
            };
            let flags = e.flags.cast_unsigned();
            let lit = (flags >> 2) & 1 != 0;
            let visible = (flags >> 1) & 1 != 0;
            let render_type = if e.usr_flg & 2 != 0 {
                2
            } else {
                u32::from(e.usr_flg & 1)
            };
            let name = e.name.map(<[u8]>::to_vec);
            match e.kind {
                kind::COMPOUND => {
                    let fog = user(b"nofog").is_none();
                    let mut room_main = user(b"hardpoint").is_some();
                    let Some(category) = user(b"category") else {
                        reports.push(
                            ROOM_DATA,
                            0x293,
                            format!(
                                "*** ERROR: Entity {} in hardpoint script {} does not have a category.",
                                lossy(name.as_deref().unwrap_or_default()),
                                lossy(path)
                            ),
                        );
                        continue;
                    };
                    let category = petal_category(&category, reports);
                    if category == 0 {
                        room_main = true;
                    }
                    let loadout = user(b"loadout").unwrap_or_default();
                    let priority = user(b"priority").map_or(0, |p| id(&p));
                    let running_lights = user(b"running_lights").is_some();
                    self.props.push(RoomProp {
                        template: e.template_name.map(<[u8]>::to_vec),
                        name,
                        category,
                        frame,
                        visible,
                        lit,
                        fog,
                        room_main,
                        light_group: e.lt_grp,
                        layer: e.srt_grp,
                        render_type,
                        loadout,
                        priority,
                        running_lights,
                    });
                }
                kind::CAMERA => {
                    self.cameras.push(RoomCamera {
                        name: name.clone(),
                        frame,
                        fov: e.camera[0],
                        near: e.camera[1],
                        far: e.camera[2],
                    });
                    self.add_setpoint(name, frame, reports);
                }
                kind::LIGHT => {
                    let l = e.light;
                    let color = [l.diffuse[0], l.diffuse[1], l.diffuse[2]];
                    let named = name.as_deref().and_then(|n| {
                        if eq_ignore_case(n, b"LtShadowSource") {
                            Some(&mut self.shadow_light)
                        } else if eq_ignore_case(n, b"LtGlareSource") {
                            Some(&mut self.glare_light)
                        } else {
                            None
                        }
                    });
                    if let Some(target) = named {
                        target.on = true;
                        if l.light_type == 3 {
                            target.direction = turn_light_direction(&frame, l.direction);
                            target.world_dir = true;
                        } else {
                            target.position = frame.pos;
                            target.world_dir = false;
                        }
                        target.color = color;
                    } else if l.on != 0 {
                        self.lights.push(RoomLight {
                            frame,
                            direction: l.direction,
                            d3d: d3d_words(l),
                            light_group: e.lt_grp,
                        });
                    }
                }
                kind::MARKER => self.add_setpoint(name, frame, reports),
                kind::SCENE => {
                    self.ambient_color = [
                        ambient_channel(e.scene[0]),
                        ambient_channel(e.scene[1]),
                        ambient_channel(e.scene[2]),
                    ];
                }
                _ => {}
            }
        }
        for s in &self.setpoints {
            if !s.props.valid {
                continue;
            }
            self.characters.push(CharPlacement {
                name: s.name.clone(),
                frame: s.frame,
                char_type: s.props.char_type,
                posture: s.props.posture,
                zone_script: s.props.zone_script,
                start_script: None,
            });
        }
    }

    /// `add_set_script_Setpoint`. A setpoint without a name crashes the
    /// reference in `extract_from_name`'s `strlen`; the port reads it as
    /// an empty name.
    fn add_setpoint(&mut self, name: Option<Vec<u8>>, frame: Frame, reports: &mut Reports) {
        let props = SetpointProps::from_name(name.as_deref().unwrap_or_default(), reports);
        self.setpoints.push(Setpoint { name, frame, props });
    }

    /// `read_Sound_block`.
    fn read_sound(&mut self, ini: &mut IniReader) {
        while ini.read_value() {
            let music = ini.is_value(b"music");
            if !music && !ini.is_value(b"ambient") {
                continue;
            }
            let value = id(&ini.value_string_at(0));
            let looping =
                ini.is_value_empty(1) || !eq_ignore_case(&ini.value_string_at(1), b"oneshot");
            if music {
                self.music = value;
                self.music_loop = looping;
            } else {
                self.ambient = value;
                self.ambient_loop = looping;
            }
        }
    }

    /// `read_Camera_block`.
    fn read_camera(&mut self, ini: &mut IniReader, reports: &mut Reports) {
        while ini.read_value() {
            if ini.is_value(b"name") {
                self.camera_name = Some(ini.value_string());
            } else if let Some(line) = [
                (b"index".as_slice(), 0x4d7),
                (b"near_plane", 0x4db),
                (b"far_plane", 0x4df),
            ]
            .iter()
            .find(|(n, _)| ini.is_value(n))
            .map(|&(_, l)| l)
            {
                reports.push(
                    ROOM_DATA,
                    line,
                    format!(
                        "*** WARNING: obsolete [Camera] key in {}, use a camera name",
                        lossy(ini.file_name())
                    ),
                );
            }
        }
    }

    /// `read_CharacterPlacement_block`: the start script of the last
    /// placement of that name. A block without a name is reported; the
    /// reference then compares a null name (undefined), the port finds
    /// nothing.
    fn read_character_placement(&mut self, ini: &mut IniReader, reports: &mut Reports) {
        let mut name = None;
        let mut start = None;
        while ini.read_value() {
            if ini.is_value(b"name") {
                name = Some(ini.value_string());
            } else if ini.is_value(b"start_script") {
                start = Some(ini.value_string());
            } else if ini.is_value(b"zoom_in") || ini.is_value(b"zoom_out") {
                reports.push(
                    ROOM_DATA,
                    if ini.is_value(b"zoom_in") {
                        0x635
                    } else {
                        0x639
                    },
                    format!(
                        "*** ERROR: zoom in file {} is obsolete",
                        lossy(ini.file_name())
                    ),
                );
            }
        }
        let file = lossy(ini.file_name());
        let Some(name) = name else {
            reports.push(
                ROOM_DATA,
                0x647,
                format!("*** ERROR: [CharacterPlacement] in file {file} did not supply name"),
            );
            return;
        };
        match self
            .characters
            .iter_mut()
            .rev()
            .find(|c| c.name.as_deref().is_some_and(|n| eq_ignore_case(&name, n)))
        {
            Some(place) => place.start_script = start,
            None => reports.push(
                ROOM_DATA,
                0x656,
                format!(
                    "*** ERROR: [CharacterPlacement] in file {file} refers to setpoint {} which does not exist",
                    lossy(&name)
                ),
            ),
        }
    }

    /// `read_PlayerShipPlacement_block` and
    /// `read_ForSaleShipPlacement_block`: the placement is added first.
    fn read_ship_placement(&mut self, ini: &mut IniReader, player: bool, reports: &mut Reports) {
        let mut place = ShipPlacement {
            nickname: None,
            frame: Frame::IDENTITY,
            player_ship: player,
            for_sale: !player,
            landing_script: None,
            launching_script: None,
        };
        while ini.read_value() {
            if ini.is_value(b"name") {
                let file = ini.file_name().to_vec();
                let setpoint = ini.value_string();
                if let Some(found) = self.find_setpoint(&setpoint, &file, reports) {
                    if place.nickname.is_none() {
                        place.nickname.clone_from(&found.name);
                    }
                    place.frame = found.frame;
                }
            } else if player && ini.is_value(b"landing_script") {
                place.landing_script = Some(ini.value_string());
            } else if player && ini.is_value(b"launching_script") {
                place.launching_script = Some(ini.value_string());
            }
        }
        self.ships.push(place);
    }

    /// `read_Hotspot_block`: the hotspot is added first.
    fn read_hotspot(&mut self, ini: &mut IniReader, reports: &mut Reports) {
        let mut spot = Hotspot {
            name: None,
            room_switch: None,
            virtual_room: None,
            set_virtual_room: None,
            behavior: -1,
            state_send: 0,
            state_read: 0,
            state_transitions: [None, None, None, None],
        };
        while ini.read_value() {
            if ini.is_value(b"name") {
                spot.name = Some(ini.value_string());
            } else if ini.is_value(b"behavior") {
                spot.behavior = hotspot_behavior(&ini.value_string());
            } else if ini.is_value(b"zoom_in") || ini.is_value(b"zoom_out") {
                reports.push(
                    ROOM_DATA,
                    if ini.is_value(b"zoom_in") {
                        0x6e7
                    } else {
                        0x6eb
                    },
                    "*** ERROR: zoom is obsolete in ini file",
                );
            } else if ini.is_value(b"room_switch") {
                spot.room_switch = Some(ini.value_string());
            } else if ini.is_value(b"virtual_room") {
                spot.virtual_room = Some(ini.value_string());
            } else if ini.is_value(b"set_virtual_room") {
                spot.set_virtual_room = Some(ini.value_string());
            } else if ini.is_value(b"state_send") {
                spot.state_send |= ftol_u32(ini.value_float(0));
            } else if ini.is_value(b"state_read") {
                spot.state_read |= ftol_u32(ini.value_float(0));
            } else if ini.is_value(b"state_transition")
                && let Some(slot) = spot.state_transitions.iter_mut().find(|s| s.is_none())
            {
                *slot = Some(ini.value_string());
            }
        }
        self.hotspots.push(spot);
    }

    /// `read_FlashLightSet_block` (mode 1) and `read_FlashLightLine_block`
    /// (mode 2, with `numlights`).
    fn read_flashlights(&mut self, ini: &mut IniReader, mode: i32) {
        // 1/255 as the float at .rdata 0639ac78.
        let byte_to_unit = f64::from(f32::from_bits(0x3b80_8081));
        let mut set = FlashLightSet {
            mode,
            ..FlashLightSet::default()
        };
        let slot = |set: &mut FlashLightSet, hardpoint: bool, value: Vec<u8>| {
            set.hardpoints.push(None);
            set.setpoints.push(None);
            let last = set.hardpoints.len() - 1;
            if hardpoint {
                set.hardpoints[last] = Some(value);
            } else {
                set.setpoints[last] = Some(value);
            }
        };
        while ini.read_value() {
            if ini.is_value(b"color") {
                set.params[0] = float(ini, 0);
                set.params[1] = float(ini, 1);
                set.params[2] = float(ini, 2);
            } else if ini.is_value(b"icolor") {
                set.params[0] = (f64::from(float(ini, 0)) * byte_to_unit) as f32;
                set.params[1] = (f64::from(float(ini, 1)) * byte_to_unit) as f32;
                set.params[2] = (ini.value_float(2) * byte_to_unit) as f32;
            } else if ini.is_value(b"scale") {
                set.params[3] = float(ini, 0);
            } else if ini.is_value(b"gap") {
                set.params[4] = float(ini, 0);
            } else if ini.is_value(b"blink") {
                set.params[5] = float(ini, 0);
            } else if ini.is_value(b"endpause") {
                set.params[6] = float(ini, 0);
            } else if ini.is_value(b"hardpoint") {
                slot(&mut set, true, ini.value_string());
            } else if ini.is_value(b"setpoint") {
                slot(&mut set, false, ini.value_string());
            } else if mode == 2 && ini.is_value(b"numlights") {
                set.num_lights_alt = trunc_to_i64(ini.value_float(0)) as i32;
            }
        }
        self.flashlights.push(set);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn setpoint_names_keep_the_constructor_defaults() {
        let mut reports = Reports::default();
        let p = SetpointProps::from_name(b"Camera01", &mut reports);
        assert_eq!(p, SetpointProps::default());
        let p = SetpointProps::from_name(b"Zg/NPC/Bar/2/Barkeep/SitLow", &mut reports);
        assert!(p.valid && !p.zone_script);
        assert_eq!((p.char_type, p.zone, p.posture), (0, 2, 1));
        assert_eq!(p.segment.as_deref(), Some(b"bar".as_slice()));
        assert_eq!(p.name, b"BARKEEP");
        let p = SetpointProps::from_name(b"zs/xx/a/1/b", &mut reports);
        assert_eq!(p.char_type, -1, "an unknown type leaves the default");
        assert_eq!(p.posture, 3);
    }

    #[test]
    fn hotspot_behaviours_compare_the_first_token() {
        assert_eq!(hotspot_behavior(b"exitdoor"), 1);
        assert_eq!(hotspot_behavior(b"Repair, x"), 12);
        assert_eq!(hotspot_behavior(b",Repair"), -1);
        assert_eq!(hotspot_behavior(b"nothing"), -1);
    }
}
