//! The client's effect archetypes (`vis_effect_arch.cpp`, `EffectArch.cpp`
//! and `effect_type_info.cpp`, `EffectTypes.cpp`; work package P5.6): the
//! `[Data] effects` files' `[Effect]`, `[EffectLOD]`, `[EffectType]`,
//! `[VisEffect]` and `[BeamSpear]` / `[BeamBolt]` sections, read in that
//! test order by [`EffectArchDb::load_ini`].
//!
//! [`EffectArchDb`] holds what the reference keeps in globals: the
//! `EffectArch` registry by nickname id, the `VisEffect` and beam maps by
//! nickname (compared without case, as the `StringAlloc` keys intern), the
//! 47-row effect-type table seeded with the exe's `.data` and the eight
//! LOD rows. The effects' run-time parts (the `.ale` loads and the texture
//! slots a `VisEffect` takes on its first reference, the particles, the
//! beam geometry) are the later increments of P5.6.
//!
//! The reports the reference writes (`FDUMP` and `INI_Reader::log_link`)
//! go to [`EffectArchDb::reports`] in order.

use std::collections::BTreeMap;

use game_sim::data::DataSource;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

/// `kEffectTypeCount`: the rows; a name not found answers this.
pub const EFFECT_TYPE_COUNT: usize = 0x2f;
/// `kEffectLodTypeCount`.
pub const EFFECT_LOD_TYPE_COUNT: usize = 7;
/// `kEffectLodTableRows`.
pub const EFFECT_LOD_TABLE_ROWS: usize = 8;

const COLOR_SCALE: f32 = 0.003_921_569;
const EFFECT_ARCH_CPP: &str = "Client\\cmn\\EffectArch.cpp";
const EFFECT_TYPES_CPP: &str = "Client\\Space\\EffectTypes.cpp";
const VIS_EFFECT_ARCH_CPP: &str = "Client\\cmn\\VisEffectArch.cpp";

fn r(v: f64) -> f32 {
    #[allow(clippy::cast_possible_truncation)]
    let f = v as f32;
    f
}

/// `get_value_float(i)` as the reference stores it (a float).
fn float(reader: &mut IniReader, i: u32) -> f32 {
    r(reader.value_float(i))
}

/// `FEffectTypeInfo` (0x28 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EffectTypeInfo {
    /// +0x00 `EFT_*`.
    pub nickname: &'static str,
    /// +0x04.
    pub priority: f32,
    /// +0x08.
    pub generic_priority: f32,
    /// +0x0c the `EFT_LOD_*` row.
    pub lod_type: i32,
    /// +0x10.
    pub radius: f32,
    /// +0x14 `EXIST_OFFSCREEN`.
    pub visibility_offscreen: bool,
    /// +0x15 `UPDATE_OFFSCREEN`.
    pub update_offscreen: bool,
    /// +0x18 negative: the effect runs until killed (`EffectArch` flags bit 0).
    pub run_time: f32,
    /// +0x1c.
    pub fps: f32,
    /// +0x20.
    pub pbubble_near: f32,
    /// +0x24.
    pub pbubble_far: f32,
}

#[allow(clippy::too_many_arguments)]
const fn row(
    nickname: &'static str,
    priority: f32,
    generic_priority: f32,
    lod_type: i32,
    radius: f32,
    visibility: u8,
    update: u8,
    run_time: f32,
    pbubble_near: f32,
    pbubble_far: f32,
) -> EffectTypeInfo {
    EffectTypeInfo {
        nickname,
        priority,
        generic_priority,
        lod_type,
        radius,
        visibility_offscreen: visibility != 0,
        update_offscreen: update != 0,
        run_time,
        fps: 5.0,
        pbubble_near,
        pbubble_far,
    }
}

/// `G_EffectTypeTable` as the exe's `.data` seeds it.
pub const EFFECT_TYPES: [EffectTypeInfo; EFFECT_TYPE_COUNT] = [
    row("EFT_NONE", 0.0, 0.0, 0, 10.0, 0, 0, 15.0, 100.0, 500.0),
    row(
        "EFT_EXPLOSION_SMALL",
        1.0,
        0.5,
        3,
        10.0,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_EXPLOSION_MEDIUM",
        1.0,
        0.5,
        4,
        50.0,
        1,
        1,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_EXPLOSION_LARGE",
        1.0,
        0.5,
        5,
        100.0,
        1,
        1,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_EXPLOSION_DEBRIS",
        0.0,
        0.0,
        3,
        10.0,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_EXPLOSION_SHIP",
        1.0,
        0.5,
        4,
        50.0,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_EXPLOSION_MISSILE",
        0.0,
        0.0,
        6,
        7.5,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_EXPLOSION_MINE",
        0.0,
        0.0,
        6,
        7.5,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_MUZZLE_FLASH",
        0.0,
        0.0,
        6,
        5.0,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_MUZZLE_LARGE_FLASH",
        1.0,
        0.5,
        6,
        7.5,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_MISSILE_DRIVE",
        0.5,
        0.5,
        2,
        100.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_WEAPON_PROJ",
        1.0,
        0.5,
        2,
        15.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_WEAPON_LARGE_PROJ",
        1.0,
        0.5,
        5,
        50.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_WEAPON_IMPACT",
        0.5,
        0.5,
        6,
        7.5,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_WEAPON_LARGE_IMPACT",
        1.0,
        0.5,
        4,
        20.0,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_WEAPON_COUNTER_MEASURE",
        1.0,
        0.5,
        6,
        7.5,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_WEAPON_MINE",
        1.0,
        0.5,
        4,
        20.0,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_DAMAGE_HULL",
        0.5,
        0.5,
        6,
        7.5,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_DAMAGE_LARGE_SHIELD",
        1.0,
        0.5,
        5,
        100.0,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_DAMAGE_SHIELD",
        0.5,
        0.5,
        3,
        5.0,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_DAMAGE_CRUISE",
        0.5,
        0.5,
        6,
        7.5,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_DAMAGE_SMOKE",
        0.5,
        0.5,
        4,
        20.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_DAMAGE_LARGE_SMOKE",
        0.5,
        0.5,
        5,
        20.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_DEBRIS_TRAIL",
        0.5,
        0.5,
        3,
        5.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_ENGINE_CONTRAIL",
        0.0,
        0.0,
        3,
        5.0,
        1,
        1,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_ENGINE_TRAIL",
        1.0,
        0.5,
        1,
        100.0,
        1,
        1,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_ENGINE_LARGE_TRAIL",
        1.0,
        0.5,
        1,
        100.0,
        1,
        1,
        -1.0,
        500.0,
        2000.0,
    ),
    row(
        "EFT_ENGINE_FIRE",
        0.5,
        0.5,
        6,
        7.5,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_ENGINE_LARGE_FIRE",
        0.5,
        0.5,
        6,
        25.0,
        1,
        0,
        -1.0,
        250.0,
        1000.0,
    ),
    row(
        "EFT_ENGINE_THRUSTER",
        0.0,
        0.0,
        6,
        7.5,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_EQUIP_CLOAK",
        1.0,
        0.5,
        3,
        5.0,
        0,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_EQUIP_LARGE_CLOAK",
        1.0,
        0.5,
        4,
        100.0,
        0,
        0,
        -1.0,
        500.0,
        1500.0,
    ),
    row(
        "EFT_EQUIP_ATTACHED",
        1.0,
        0.5,
        5,
        100.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_EQUIP_ATTACHED_LARGE",
        1.0,
        0.5,
        5,
        500.0,
        1,
        0,
        -1.0,
        1000.0,
        5000.0,
    ),
    row(
        "EFT_EQUIP_ATTACHED_INSANELY_LARGE",
        1.0,
        0.5,
        5,
        500.0,
        1,
        0,
        -1.0,
        1000.0,
        5000.0,
    ),
    row(
        "EFT_MISC_DUST",
        0.5,
        0.5,
        0,
        500.0,
        1,
        1,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_MOTION_DUST",
        1.0,
        1.0,
        0,
        500.0,
        1,
        1,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_JUMP_EFFECTS",
        1.0,
        0.5,
        0,
        0.0,
        1,
        1,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_JUMP_GATE_EFFECTS",
        1.0,
        0.5,
        0,
        0.0,
        1,
        1,
        -1.0,
        1000.0,
        2500.0,
    ),
    row(
        "EFT_LANE_ACTIVE",
        0.5,
        0.5,
        0,
        50.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row("EFT_LANE_TUBE", 1.0, 0.5, 0, 50.0, 1, 0, -1.0, 100.0, 500.0),
    row("EFT_LANE_SHIP", 0.5, 0.5, 4, 50.0, 0, 0, 15.0, 100.0, 500.0),
    row(
        "EFT_LANE_SHIP_TRAVEL",
        0.5,
        0.5,
        4,
        50.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_CUSTOM_EFFECTS",
        0.0,
        0.0,
        5,
        -1.0,
        0,
        0,
        15.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_CODE_PROJ_EFFECT",
        1.0,
        0.5,
        2,
        15.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_TRACTORBEAM",
        1.0,
        1.0,
        0,
        -1.0,
        1,
        1,
        -1.0,
        100.0,
        500.0,
    ),
    row(
        "EFT_FLASHLIGHT",
        1.0,
        1.0,
        0,
        -1.0,
        1,
        0,
        -1.0,
        100.0,
        500.0,
    ),
];

/// `kEffectLodIniNames` (`[EffectLOD] type`).
const LOD_INI_NAMES: [&str; EFFECT_LOD_TYPE_COUNT] = [
    "none",
    "lod_type_trail",
    "lod_type_weapon",
    "lod_type_small",
    "lod_type_medium",
    "lod_type_large",
    "lod_type_small_distant",
];

/// `kEffectLodTypeNames` (`[EffectType] lod_type`).
const LOD_TYPE_NAMES: [&str; EFFECT_LOD_TYPE_COUNT] = [
    "EFT_LOD_NONE",
    "EFT_LOD_TRAIL",
    "EFT_LOD_WEAPON",
    "EFT_LOD_SMALL",
    "EFT_LOD_MEDIUM",
    "EFT_LOD_LARGE",
    "EFT_LOD_SMALL_DISTANT",
];

/// One `[EffectLOD]` row (-1 until read).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EffectLodSizes {
    /// `max_lod_screen_size`.
    pub max_lod_screen_size: f32,
    /// `min_lod_screen_size`.
    pub min_lod_screen_size: f32,
    /// `min_screen_size`.
    pub min_screen_size: f32,
}

impl Default for EffectLodSizes {
    fn default() -> Self {
        Self {
            max_lod_screen_size: -1.0,
            min_lod_screen_size: -1.0,
            min_screen_size: -1.0,
        }
    }
}

/// `FBeamEffectArch` (0x80 bytes): a `[BeamSpear]` (0) or `[BeamBolt]` (1).
#[derive(Clone, Debug, PartialEq)]
pub struct BeamEffectArch {
    /// +0x00.
    pub nickname: Vec<u8>,
    /// +0x04 0 spear (one segment), 1 bolt (several).
    pub segment_mode: i32,
    /// +0x08 `head_texture`: `ball` 1, else 0.
    pub uv_mode_v: i32,
    /// +0x0c `trail_texture`: `thin` 1, else 0.
    pub uv_mode_u: i32,
    /// +0x10 `tip_color`.
    pub tip_color: [f32; 3],
    /// +0x1c `tail_color`.
    pub tail_color: [f32; 3],
    /// +0x28 `core_color`.
    pub core_color: [f32; 3],
    /// +0x34 `sec_core_color`.
    pub sec_core_color: [f32; 3],
    /// +0x40 `outter_color`.
    pub outer_color: [f32; 3],
    /// +0x4c `sec_outter_color`.
    pub sec_outer_color: [f32; 3],
    /// +0x58 `head_brightness`, clamped to [0.01, 1].
    pub head_brightness: f32,
    /// +0x5c `trail_brightness`, clamped to [0.01, 1].
    pub trail_brightness: f32,
    /// +0x60 `tip_length`, at least `core_width`.
    pub tip_length: f32,
    /// +0x64 `tail_length`, at least `core_width` (spear) or
    /// `sec_core_width` (bolt).
    pub tail_length: f32,
    /// +0x68 `core_length`.
    pub core_length: f32,
    /// +0x6c `core_width`.
    pub core_width: f32,
    /// +0x70 `sec_core_width`.
    pub sec_core_width: f32,
    /// +0x74 `head_width`, at least `core_width`.
    pub head_width: f32,
    /// +0x78 `flash_size`.
    pub flash_size: f32,
    /// +0x7c (1.0, never read from the file).
    pub pad_7c: f32,
}

impl Default for BeamEffectArch {
    /// `BeamArchInitDefaults`: every float 1.0.
    fn default() -> Self {
        Self {
            nickname: Vec::new(),
            segment_mode: 0,
            uv_mode_v: 0,
            uv_mode_u: 0,
            tip_color: [1.0; 3],
            tail_color: [1.0; 3],
            core_color: [1.0; 3],
            sec_core_color: [1.0; 3],
            outer_color: [1.0; 3],
            sec_outer_color: [1.0; 3],
            head_brightness: 1.0,
            trail_brightness: 1.0,
            tip_length: 1.0,
            tail_length: 1.0,
            core_length: 1.0,
            core_width: 1.0,
            sec_core_width: 1.0,
            head_width: 1.0,
            flash_size: 1.0,
            pad_7c: 1.0,
        }
    }
}

impl BeamEffectArch {
    /// `BeamArchClamp`.
    fn clamp(&mut self) {
        if self.tip_length < self.core_width {
            self.tip_length = self.core_width;
        }
        if self.segment_mode == 0 {
            if self.tail_length < self.core_width {
                self.tail_length = self.core_width;
            }
        } else if self.segment_mode == 1 && self.tail_length < self.sec_core_width {
            self.tail_length = self.sec_core_width;
        }
        if self.head_width < self.core_width {
            self.head_width = self.core_width;
        }
        // A NaN stays as read, as the reference's ordered tests leave it.
        self.head_brightness = self.head_brightness.clamp(0.01, 1.0);
        self.trail_brightness = self.trail_brightness.clamp(0.01, 1.0);
    }

    /// `BeamArchParseValue`: one key, true when known.
    fn read_value(&mut self, reader: &mut IniReader) -> bool {
        let rgb = |reader: &mut IniReader| {
            let s = |v: f32| r(f64::from(v) * f64::from(COLOR_SCALE));
            [
                s(float(reader, 0)),
                s(float(reader, 1)),
                s(float(reader, 2)),
            ]
        };
        if reader.is_value(b"tip_color") {
            self.tip_color = rgb(reader);
        } else if reader.is_value(b"tail_color") {
            self.tail_color = rgb(reader);
        } else if reader.is_value(b"core_color") {
            self.core_color = rgb(reader);
        } else if reader.is_value(b"sec_core_color") {
            self.sec_core_color = rgb(reader);
        } else if reader.is_value(b"outter_color") {
            self.outer_color = rgb(reader);
        } else if reader.is_value(b"sec_outter_color") {
            self.sec_outer_color = rgb(reader);
        } else if reader.is_value(b"tip_length") {
            self.tip_length = float(reader, 0);
        } else if reader.is_value(b"tail_length") {
            self.tail_length = float(reader, 0);
        } else if reader.is_value(b"core_length") {
            self.core_length = float(reader, 0);
        } else if reader.is_value(b"core_width") {
            self.core_width = float(reader, 0);
        } else if reader.is_value(b"sec_core_width") {
            self.sec_core_width = float(reader, 0);
        } else if reader.is_value(b"head_brightness") {
            self.head_brightness = float(reader, 0);
        } else if reader.is_value(b"trail_brightness") {
            self.trail_brightness = float(reader, 0);
        } else if reader.is_value(b"flash_size") {
            self.flash_size = float(reader, 0);
        } else if reader.is_value(b"head_width") {
            self.head_width = float(reader, 0);
        } else if reader.is_value(b"head_texture") {
            // `Match5`: the whole value, NUL included, against the word.
            self.uv_mode_v = i32::from(reader.value_string() == b"ball");
        } else if reader.is_value(b"trail_texture") {
            self.uv_mode_u = i32::from(reader.value_string() == b"thin");
        } else {
            return false;
        }
        true
    }
}

/// `VisEffect` (0x38 bytes): an `.ale` effect and its texture libraries.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VisEffectArch {
    /// +0x00.
    pub nickname: Vec<u8>,
    /// +0x04 `effect_crc` (the `get_value_int` bits).
    pub effect_crc: u32,
    /// +0x08 `particlefx`, the `.ale` path.
    pub particlefx: Vec<u8>,
    /// +0x0c `textures`, one per key.
    pub textures: Vec<Vec<u8>>,
    /// +0x1c bit 0 `has_alpha`.
    pub has_alpha: bool,
}

/// `EffectArch` (0x58 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct EffectArch {
    /// +0x00 the `EFT_*` row ([`EFFECT_TYPE_COUNT`] for an unknown name).
    pub effect_type: usize,
    /// +0x04 `CreateID(nickname)`.
    pub nickname_id: u32,
    /// +0x08 `vis_effect`, the [`VisEffectArch`]'s nickname when found.
    pub vis_effect: Option<Vec<u8>>,
    /// +0x10 `vis_generic`, when found.
    pub vis_generic: Option<Vec<u8>>,
    /// +0x14 `vis_beam`, the [`BeamEffectArch`]'s nickname when found.
    pub vis_beam: Option<Vec<u8>>,
    /// +0x18 `snd_effect`'s id, when the sounds know it.
    pub snd_effect: Option<u32>,
    /// The last `snd_effect` value as read. The reference keeps only the
    /// lookup's result; the data dump compares the names (P5.6).
    pub snd_effect_name: Option<Vec<u8>>,
    /// +0x20 `lgt_effect`, at most 31 characters.
    pub lgt_effect: Vec<u8>,
    /// +0x40.
    pub lgt_range_scale: f32,
    /// +0x44.
    pub lgt_time_scale: f32,
    /// +0x48.
    pub lgt_radius: f32,
    /// +0x4c `lgt_color` (r, g, b; a stays 0xff).
    pub lgt_color: [u8; 4],
    /// +0x50 bit 0: the effect type's `run_time` is negative.
    pub flags: u8,
}

impl Default for EffectArch {
    /// `EffectArchConstruct`.
    fn default() -> Self {
        Self {
            effect_type: 0,
            nickname_id: 0,
            vis_effect: None,
            vis_generic: None,
            vis_beam: None,
            snd_effect: None,
            snd_effect_name: None,
            lgt_effect: Vec::new(),
            lgt_range_scale: 1.0,
            lgt_time_scale: 1.0,
            lgt_radius: 0.0,
            lgt_color: [0xff; 4],
            flags: 0,
        }
    }
}

/// The client's effect archetype data.
#[derive(Clone, Debug)]
pub struct EffectArchDb {
    /// `G_EffectArchs`, by nickname id.
    pub effects: BTreeMap<u32, EffectArch>,
    /// `G_VisEffects`, by nickname (lower case).
    pub vis_effects: BTreeMap<Vec<u8>, VisEffectArch>,
    /// `G_BeamArches`, by nickname (lower case).
    pub beams: BTreeMap<Vec<u8>, BeamEffectArch>,
    /// `G_EffectTypeTable`.
    pub types: [EffectTypeInfo; EFFECT_TYPE_COUNT],
    /// `G_EffectLodTable`.
    pub lods: [EffectLodSizes; EFFECT_LOD_TABLE_ROWS],
    /// The reports, in order.
    pub reports: Vec<String>,
    /// The spellings `StringAlloc(name, false)` hands back, by lower-case
    /// name: the first spelling interned wins. The reference's cache is the
    /// process's; the owner carries this one from load to load (D1403).
    pub interned: BTreeMap<Vec<u8>, Vec<u8>>,
}

impl Default for EffectArchDb {
    fn default() -> Self {
        Self {
            effects: BTreeMap::new(),
            vis_effects: BTreeMap::new(),
            beams: BTreeMap::new(),
            types: EFFECT_TYPES,
            lods: [EffectLodSizes::default(); EFFECT_LOD_TABLE_ROWS],
            interned: BTreeMap::new(),
            reports: Vec::new(),
        }
    }
}

fn key(name: &[u8]) -> Vec<u8> {
    name.to_ascii_lowercase()
}

/// `EffectTypeIndexFromName`: the row whose name matches exactly
/// (`strcmp`), [`EFFECT_TYPE_COUNT`] when none does.
#[must_use]
pub fn effect_type_index(name: &[u8]) -> usize {
    EFFECT_TYPES
        .iter()
        .position(|t| t.nickname.as_bytes() == name)
        .unwrap_or(EFFECT_TYPE_COUNT)
}

impl EffectArchDb {
    /// `EffectArchLoadIni(path, false)`: one effects file from `source`;
    /// returns how many effect archetypes the registry gained.
    pub fn load_ini(
        &mut self,
        source: &dyn DataSource,
        path: &[u8],
        sound_known: &dyn Fn(u32) -> bool,
    ) -> usize {
        let before = self.effects.len();
        let Some(mut reader) = game_sim::data::open_ini(source, path) else {
            return 0;
        };
        self.read(&mut reader, sound_known);
        self.effects.len() - before
    }

    /// Every `[Data] effects` file of `config`, in file order
    /// (`DataConfigProgress`'s `effects` key).
    #[must_use]
    pub fn from_config(
        source: &dyn DataSource,
        config: &game_sim::data::config::DataConfig,
        sound_known: &dyn Fn(u32) -> bool,
    ) -> Self {
        let mut db = Self::default();
        for entry in &config.entries {
            if entry.key.eq_ignore_ascii_case(b"effects") {
                db.load_ini(source, &entry.value, sound_known);
            }
        }
        db
    }

    /// The sections of an open reader, in the reference's test order.
    pub fn read(&mut self, reader: &mut IniReader, sound_known: &dyn Fn(u32) -> bool) {
        while reader.read_header() {
            if reader.is_header(b"Effect") {
                self.load_effect(reader, sound_known);
            } else if reader.is_header(b"EffectLOD") {
                self.load_lod(reader);
            } else if reader.is_header(b"EffectType") {
                self.load_type(reader);
            } else if reader.is_header(b"VisEffect") {
                self.load_vis_effect(reader);
            } else if reader.is_header(b"BeamSpear") || reader.is_header(b"BeamBolt") {
                self.load_beam(reader);
            }
        }
    }

    /// The archetype of a nickname id (`FindEffectArch`).
    #[must_use]
    pub fn find(&self, nickname_id: u32) -> Option<&EffectArch> {
        self.effects.get(&nickname_id)
    }

    /// `FindVisEffectByNickname`.
    #[must_use]
    pub fn vis_effect(&self, nickname: &[u8]) -> Option<&VisEffectArch> {
        self.vis_effects.get(&key(nickname))
    }

    /// `FindBeamEffectArchByNickname`.
    #[must_use]
    pub fn beam(&self, nickname: &[u8]) -> Option<&BeamEffectArch> {
        self.beams.get(&key(nickname))
    }

    fn nickname_text(id: u32) -> String {
        format!("{id}")
    }

    /// The `[Effect]` branch: a new archetype, its values, then the
    /// registry; a repeated nickname is warned about and dropped (the
    /// reference's reload merge is not taken: `[Data]` loads with false).
    fn load_effect(&mut self, reader: &mut IniReader, sound_known: &dyn Fn(u32) -> bool) {
        let mut arch = EffectArch::default();
        while reader.read_value() {
            self.effect_value(&mut arch, reader, sound_known);
        }
        if self.effects.contains_key(&arch.nickname_id) {
            self.reports.push(format!(
                "{EFFECT_ARCH_CPP}(252) : *** WARNING: [Effect] nickname = {} value is not unique. Aborted second version.",
                Self::nickname_text(arch.nickname_id)
            ));
            return;
        }
        self.effects.insert(arch.nickname_id, arch);
    }

    /// `EffectArchParseValue`.
    fn effect_value(
        &mut self,
        arch: &mut EffectArch,
        reader: &mut IniReader,
        sound_known: &dyn Fn(u32) -> bool,
    ) {
        if reader.is_value(b"nickname") {
            arch.nickname_id = create_id(&reader.value_string());
        } else if reader.is_value(b"effect_type") {
            let index = effect_type_index(&reader.value_string());
            arch.effect_type = index;
            if index == EFFECT_TYPE_COUNT {
                self.reports.push(String::new());
                self.reports.push(format!(
                    "{EFFECT_ARCH_CPP}(58) : ERROR:General:unknown effect type"
                ));
                return;
            }
            if self.types[index].run_time < 0.0 {
                arch.flags |= 1;
            } else {
                arch.flags &= !1;
            }
        } else if reader.is_value(b"vis_effect") || reader.is_value(b"vis_generic") {
            let generic = reader.is_value(b"vis_generic");
            let name = self.intern(reader.value_string());
            let found = self.vis_effect(&name).map(|v| v.nickname.clone());
            if found.is_none() {
                self.reports.push(format!(
                    "[Effect] {} couldn't find [VisEffect] {}.",
                    Self::nickname_text(arch.nickname_id),
                    String::from_utf8_lossy(&name)
                ));
            }
            if generic {
                arch.vis_generic = found;
            } else {
                arch.vis_effect = found;
            }
        } else if reader.is_value(b"vis_beam") {
            let name = self.intern(reader.value_string());
            arch.vis_beam = self.beam(&name).map(|b| b.nickname.clone());
            if arch.vis_beam.is_none() {
                self.reports.push(format!(
                    "[Effect] {} couldn't find [VisBeam] {}.",
                    Self::nickname_text(arch.nickname_id),
                    String::from_utf8_lossy(&name)
                ));
            }
        } else if reader.is_value(b"snd_effect") {
            let name = reader.value_string();
            let id = create_id(&name);
            arch.snd_effect_name = Some(name);
            arch.snd_effect = sound_known(id).then_some(id);
            if arch.snd_effect.is_none() {
                self.reports.push(format!(
                    "[Effect] {} couldn't find [Sound] {id}.",
                    Self::nickname_text(arch.nickname_id)
                ));
            }
        } else if reader.is_value(b"lgt_effect") {
            let mut name = reader.value_string();
            name.truncate(0x1f);
            arch.lgt_effect = name;
        } else if reader.is_value(b"lgt_range_scale") {
            arch.lgt_range_scale = float(reader, 0);
        } else if reader.is_value(b"lgt_time_scale") {
            arch.lgt_time_scale = float(reader, 0);
        } else if reader.is_value(b"lgt_radius") {
            arch.lgt_radius = float(reader, 0);
        } else if reader.is_value(b"lgt_color") {
            for (i, c) in (0u32..).zip(arch.lgt_color.iter_mut().take(3)) {
                let v = f64::from(float(reader, i));
                // `_ftol`, the low byte kept.
                #[allow(clippy::cast_sign_loss, clippy::cast_possible_truncation)]
                {
                    *c = neuron_core::num::trunc_to_i32_via_i64(v) as u8;
                }
            }
        }
    }

    /// `LoadEffectTypeSection`: an unknown nickname keeps the previous row.
    fn load_type(&mut self, reader: &mut IniReader) {
        let mut current: Option<usize> = None;
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                let index = effect_type_index(&reader.value_string());
                if index >= EFFECT_TYPE_COUNT {
                    self.reports.push(format!(
                        "{EFFECT_TYPES_CPP}(76) : WARNING:General:EffectTypes: unknown type"
                    ));
                    self.reports.push("EffectTypes".to_owned());
                } else {
                    current = Some(index);
                }
                continue;
            }
            let Some(i) = current else {
                continue;
            };
            let t = &mut self.types[i];
            if reader.is_value(b"priority") {
                t.priority = float(reader, 0);
            } else if reader.is_value(b"generic_priority") {
                t.generic_priority = float(reader, 0);
            } else if reader.is_value(b"lod_type") {
                let value = reader.value_string();
                if let Some(lod) = (1..EFFECT_LOD_TYPE_COUNT)
                    .find(|&l| value.eq_ignore_ascii_case(LOD_TYPE_NAMES[l].as_bytes()))
                {
                    #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
                    {
                        t.lod_type = lod as i32;
                    }
                }
            } else if reader.is_value(b"radius") {
                let mut radius = float(reader, 0);
                if radius < 0.0 {
                    let name = t.nickname;
                    self.reports.push(format!(
                        "{EFFECT_TYPES_CPP}(124) : *** ERROR: effect type ({name}) has negative radius"
                    ));
                    radius = 1.0;
                }
                self.types[i].radius = radius;
            } else if reader.is_value(b"visibility") {
                t.visibility_offscreen = reader
                    .value_string()
                    .eq_ignore_ascii_case(b"EXIST_OFFSCREEN");
            } else if reader.is_value(b"update") {
                t.update_offscreen = reader
                    .value_string()
                    .eq_ignore_ascii_case(b"UPDATE_OFFSCREEN");
            } else if reader.is_value(b"run_time") {
                t.run_time = float(reader, 0);
            } else if reader.is_value(b"fps") {
                t.fps = float(reader, 0);
            } else if reader.is_value(b"pbubble") {
                t.pbubble_near = float(reader, 0);
                t.pbubble_far = float(reader, 1);
                if t.pbubble_far < t.pbubble_near {
                    self.reports.push(format!(
                        "{EFFECT_TYPES_CPP}(193) : WARNING:General:pbubble ranges backwards"
                    ));
                    self.reports.push("EffectTypes".to_owned());
                }
            } else {
                let name = t.nickname;
                let file = String::from_utf8_lossy(reader.file_name()).into_owned();
                self.reports.push(format!(
                    "{EFFECT_TYPES_CPP}(199) : *** ERROR: EffectType ({name}) has unknown parameter ({file})"
                ));
            }
        }
    }

    /// `LoadEffectLodSection`: the first value must be `type`.
    fn load_lod(&mut self, reader: &mut IniReader) {
        if !reader.read_value() || !reader.is_value(b"type") {
            return;
        }
        let name = reader.value_string();
        let Some(lod) = (1..EFFECT_LOD_TYPE_COUNT)
            .find(|&l| name.eq_ignore_ascii_case(LOD_INI_NAMES[l].as_bytes()))
        else {
            self.reports.push(format!(
                "{VIS_EFFECT_ARCH_CPP}(105) : *** ERROR: invalid LOD type ({})",
                String::from_utf8_lossy(&name)
            ));
            self.reports.push(String::new());
            return;
        };
        let row = &mut self.lods[lod];
        while reader.read_value() {
            if reader.is_value(b"max_lod_screen_size") {
                row.max_lod_screen_size = float(reader, 0);
            } else if reader.is_value(b"min_lod_screen_size") {
                row.min_lod_screen_size = float(reader, 0);
            } else if reader.is_value(b"min_screen_size") {
                row.min_screen_size = float(reader, 0);
            }
        }
    }

    /// `StringAlloc(text, false)`: the first spelling of `text` interned.
    fn intern(&mut self, text: Vec<u8>) -> Vec<u8> {
        self.interned.entry(key(&text)).or_insert(text).clone()
    }

    /// `VisEffectLoadSection`: `nickname` selects (finds or inserts) the
    /// entry; keys before it are dropped.
    fn load_vis_effect(&mut self, reader: &mut IniReader) {
        let mut current: Option<Vec<u8>> = None;
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                let nick = self.intern(reader.value_string());
                let k = key(&nick);
                self.vis_effects.entry(k.clone()).or_default().nickname = nick;
                current = Some(k);
                continue;
            }
            let Some(entry) = current.as_ref().and_then(|k| self.vis_effects.get_mut(k)) else {
                continue;
            };
            if reader.is_value(b"effect_crc") {
                #[allow(clippy::cast_sign_loss)]
                {
                    entry.effect_crc = reader.value_int(0) as u32;
                }
            } else if reader.is_value(b"has_alpha") {
                entry.has_alpha = reader.value_bool(0);
            } else if reader.is_value(b"particlefx") {
                entry.particlefx = reader.value_string();
            } else if reader.is_value(b"textures") {
                entry.textures.push(reader.value_string());
            }
        }
    }

    /// `LoadBeamSection`: `nickname` selects (finds or inserts with the
    /// defaults) the beam, its kind from the header; the last selected one
    /// is clamped at the section's end.
    fn load_beam(&mut self, reader: &mut IniReader) {
        let mut current: Option<Vec<u8>> = None;
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                let nick = self.intern(reader.value_string());
                let k = key(&nick);
                let beam = self.beams.entry(k.clone()).or_default();
                beam.nickname = nick;
                if reader.is_header(b"BeamSpear") {
                    beam.segment_mode = 0;
                } else if reader.is_header(b"BeamBolt") {
                    beam.segment_mode = 1;
                }
                current = Some(k);
            } else if let Some(beam) = current.as_ref().and_then(|k| self.beams.get_mut(k)) {
                beam.read_value(reader);
            }
        }
        if let Some(beam) = current.as_ref().and_then(|k| self.beams.get_mut(k)) {
            beam.clamp();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn db_of(text: &[u8]) -> EffectArchDb {
        let mut reader = IniReader::new();
        assert!(reader.open_memory(text));
        let mut db = EffectArchDb::default();
        db.read(&mut reader, &|id| id == create_id(b"fire_snd"));
        db
    }

    #[test]
    fn beams_read_their_kind_defaults_and_clamps() {
        let db = db_of(
            b"[BeamBolt]\nnickname = Bolt_A\nhead_texture = ball\ntrail_texture = wide\n\
              tip_color = 255, 0, 51\ncore_width = 2\nsec_core_width = 3\ntail_length = 1\n\
              head_brightness = 5\ntrail_brightness = 0\n",
        );
        let b = db.beam(b"bolt_a").expect("case-free lookup");
        assert_eq!(b.segment_mode, 1);
        assert_eq!((b.uv_mode_v, b.uv_mode_u), (1, 0));
        assert_eq!(b.tip_color[0], 1.0);
        assert_eq!(b.tip_color[1], 0.0);
        // Lengths at least the widths, brightness within [0.01, 1].
        assert_eq!(b.tip_length, 2.0);
        assert_eq!(b.tail_length, 3.0);
        assert_eq!(b.head_width, 2.0);
        assert_eq!((b.head_brightness, b.trail_brightness), (1.0, 0.01));
        assert_eq!(b.pad_7c, 1.0);
    }

    #[test]
    fn effects_resolve_their_parts_and_types() {
        let db = db_of(
            b"[EffectType]\nnickname = EFT_WEAPON_PROJ\nrun_time = 3\nradius = -2\n\
              [VisEffect]\nnickname = gun_ale\nparticlefx = fx\\gun.ale\ntextures = fx\\a.txm\n\
              textures = fx\\b.txm\nhas_alpha = true\neffect_crc = -5\n\
              [BeamSpear]\nnickname = spear\n\
              [Effect]\nnickname = gun_fx\neffect_type = EFT_WEAPON_PROJ\nvis_effect = GUN_ALE\n\
              vis_beam = spear\nsnd_effect = fire_snd\nlgt_color = 255, 300, 7\n\
              [Effect]\nnickname = gun_fx\n\
              [Effect]\nnickname = odd\neffect_type = EFT_NOPE\nvis_effect = none\n",
        );
        let t = &db.types[effect_type_index(b"EFT_WEAPON_PROJ")];
        assert_eq!((t.run_time, t.radius), (3.0, 1.0));
        let v = db.vis_effect(b"gun_ale").expect("vis");
        assert_eq!(v.textures.len(), 2);
        assert!(v.has_alpha);
        assert_eq!(v.effect_crc, (-5i32) as u32);
        let e = db.find(create_id(b"gun_fx")).expect("effect");
        assert_eq!(e.vis_effect.as_deref(), Some(&b"gun_ale"[..]));
        assert_eq!(e.vis_beam.as_deref(), Some(&b"spear"[..]));
        assert_eq!(e.snd_effect, Some(create_id(b"fire_snd")));
        assert_eq!(e.snd_effect_name.as_deref(), Some(&b"fire_snd"[..]));
        assert_eq!(e.flags & 1, 0, "run_time 3 is not negative");
        assert_eq!(e.lgt_color, [255, 44, 7, 255]);
        let odd = db.find(create_id(b"odd")).expect("kept");
        assert_eq!(odd.effect_type, EFFECT_TYPE_COUNT);
        assert!(odd.vis_effect.is_none());
        assert!(db.reports.iter().any(|r| r.contains("not unique")));
        assert!(db.reports.iter().any(|r| r.contains("unknown effect type")));
        assert!(db.reports.iter().any(|r| r.contains("negative radius")));
        assert!(
            db.reports
                .iter()
                .any(|r| r.contains("couldn't find [VisEffect] none"))
        );
    }

    #[test]
    fn lod_rows_need_type_first() {
        let db = db_of(
            b"[EffectLOD]\ntype = lod_type_small\nmax_lod_screen_size = 0.5\nmin_screen_size = 0.01\n\
              [EffectLOD]\nmin_screen_size = 3\n[EffectLOD]\ntype = bogus\n",
        );
        assert_eq!(db.lods[3].max_lod_screen_size, 0.5);
        assert_eq!(db.lods[3].min_lod_screen_size, -1.0);
        assert!(
            db.reports
                .iter()
                .any(|r| r.contains("invalid LOD type (bogus)"))
        );
    }
}
