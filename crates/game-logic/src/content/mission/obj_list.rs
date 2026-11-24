//! The `[ObjList]` directives (TU40, `Content/MissionObjList.h`,
//! `MissionObjList.cpp`; record 0008 point 6, work package P3.5a2): one
//! [`DirectiveKind`] variant for each class the factory builds
//! (`CreateObjListDirective`, cases 0 to 0x16 and 0x19), each a struct of
//! its class's members with the `pub::AI` op it embeds (game-sim's
//! directive ops, game-logic's `Set*Params`), and the parse of its line.
//!
//! [`KEYS`] is the `is_value` chain of `LoadObjList` (23 keys; the tractor
//! beam of case 0x19 has none, only the guides build it). A directive keeps
//! the whole line as its name (`TString<0x100>`), then its class parses the
//! values. The parse draws no `rand()`; `MakeNewFormation` and
//! `FollowPlayer` look their formation up in Content's formations, `Dock`,
//! `InstantTradelane` and `Launch` take `pub::GetNicknameId`s. `Execute`
//! is P3.5c's: the C++ shares one directive object between the mission's
//! map and the ships it is given to (a reference count) and writes the
//! resolved targets into it when it runs.

use game_sim::ai::behavior::{
    DockOp, FaceOp, FollowOp, GotoOp, GuideOp, InstantTradeLaneOp, LaunchOp, TractorBeamOp, TrailOp,
};
use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;

use super::field::Field;
use super::{f32_at, nickname_id, tstring, value_tstring};
use crate::api::ai::{SetFlagParams, SetZoneBehaviorParams};
use crate::server::Cx;

/// `TString<0x30>`, the directives' object and target names.
const NAME: usize = 0x30;

/// `TString<0x100>`, a directive's line.
const LINE: usize = 0x100;

/// The `[ObjList]` keys in the order `LoadObjList` tests them, with the
/// factory case each makes.
pub const KEYS: [(&[u8], u8); 23] = [
    (b"Avoidance", 0x0),
    (b"BreakFormation", 0x11),
    (b"CancelObjective", 0x12),
    (b"Delay", 0x1),
    (b"Idle", 0x2),
    (b"Dock", 0x13),
    (b"InstantTradelane", 0x14),
    (b"Follow", 0x7),
    (b"Guide", 0x8),
    (b"GuideShip", 0x9),
    (b"GotoShip", 0x3),
    (b"GotoSpline", 0x4),
    (b"GotoVec", 0x5),
    (b"Face", 0xa),
    (b"Launch", 0x15),
    (b"MakeNewFormation", 0xf),
    (b"FollowPlayer", 0x10),
    (b"SetPriority", 0xc),
    (b"SetLifeTime", 0xb),
    (b"StayInRange", 0xd),
    (b"StayOutOfRange", 0xe),
    (b"Trail", 0x6),
    (b"WaitForPlayerManeuver", 0x16),
];

/// The goto op's fields, each name after `$prefix`.
macro_rules! goto_fields {
    ($prefix:literal, $op:expr) => {{
        let op: &GotoOp = &$op;
        [
            (concat!($prefix, "kind"), Field::I32(op.kind)),
            (
                concat!($prefix, "destination"),
                Field::Vector(op.destination),
            ),
            (concat!($prefix, "field_1c"), Field::I32(op.field_1c)),
            (
                concat!($prefix, "words_20"),
                Field::U32s(op.words_20.to_vec()),
            ),
            (concat!($prefix, "range"), Field::F32(op.range)),
            (concat!($prefix, "field_54"), Field::F32(op.field_54)),
            (
                concat!($prefix, "field_58"),
                Field::U32(u32::from(op.field_58)),
            ),
            (
                concat!($prefix, "field_59"),
                Field::U32(u32::from(op.field_59)),
            ),
            (concat!($prefix, "cruise"), Field::U32(u32::from(op.cruise))),
            (
                concat!($prefix, "no_cruise"),
                Field::U32(u32::from(op.no_cruise)),
            ),
            (concat!($prefix, "field_5c"), Field::U32(op.field_5c)),
            (concat!($prefix, "field_60"), Field::F32(op.field_60)),
            (concat!($prefix, "field_64"), Field::F32(op.field_64)),
            (concat!($prefix, "field_68"), Field::U32(op.field_68)),
            (
                concat!($prefix, "field_6c"),
                Field::U32(u32::from(op.field_6c)),
            ),
        ]
    }};
}

/// The follow op's fields, each name after `$prefix`.
macro_rules! follow_fields {
    ($prefix:literal, $op:expr) => {{
        let op: &FollowOp = &$op;
        [
            (concat!($prefix, "target"), Field::U32(op.target)),
            (concat!($prefix, "distance"), Field::F32(op.distance)),
            (concat!($prefix, "offset"), Field::Vector(op.offset)),
            (concat!($prefix, "range"), Field::F32(op.range)),
        ]
    }};
}

/// Factory case 0, `Avoidance`: a `SetFlagParams`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Avoidance {
    /// The op's `Priority` (+0x08).
    pub priority: u32,
    /// +0x10c.
    pub op: SetFlagParams,
}

/// Factory case 1, `Delay`: a `DirectiveDelayOp`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Delay {
    /// The op's `Priority`.
    pub priority: u32,
    /// The op's `DelaySeconds` (0).
    pub delay_seconds: f32,
}

/// Factory case 2, `Idle`: a `DirectiveIdleOp`; its `Parse` is the base's.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Idle {
    /// The op's `Priority`.
    pub priority: u32,
}

/// Factory cases 3, 4 and 5, `GotoShip`, `GotoSpline` and `GotoVec`: a
/// `DirectiveGotoOp`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Goto {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x10c.
    pub op: GotoOp,
    /// +0x17c, 1 from `Parse` unless its last value gives one.
    pub field_0x17c: i32,
    /// `GotoShip`'s +0x180 (value 1), the op's `Field_1C` when it runs;
    /// empty for the other two.
    pub target_name: Vec<u8>,
    /// The object name (`GotoShip` +0x1b4, the others +0x180), the op's
    /// `Field_5C` when it runs.
    pub object_name: Vec<u8>,
}

/// Factory case 6, `Trail`: a `DirectiveTrailOp`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Trail {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x10c.
    pub op: TrailOp,
    /// +0x128, the op's target when it runs.
    pub target_name: Vec<u8>,
}

/// Factory case 7, `Follow`: a `DirectiveFollowOp`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Follow {
    /// +0x10c, 1 from `Parse`.
    pub field_0x10c: i32,
    /// +0x110, the op's target when it runs.
    pub target_name: Vec<u8>,
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x144.
    pub op: FollowOp,
}

/// Factory cases 8 and 9, `Guide` and `GuideShip`: a `DirectiveGuideOp`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Guide {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x10c.
    pub op: GuideOp,
    /// +0x1c8, 1 from `Parse` unless its last value gives one.
    pub field_0x1c8: i32,
    /// `GuideShip`'s +0x1cc (value 5), the goto part's `Field_1C` when it
    /// runs; empty for `Guide`.
    pub object_name: Vec<u8>,
    /// Value 0 (`Guide` +0x1cc, `GuideShip` +0x200), the goto part's
    /// `Field_5C` when it runs.
    pub target_name: Vec<u8>,
}

/// Factory case 0xa, `Face`: a `DirectiveFaceOp`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Face {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x10c.
    pub op: FaceOp,
    /// +0x154, the op's target when it runs.
    pub target_name: Vec<u8>,
}

/// Factory case 0xb, `SetLifeTime`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SetLifeTime {
    /// +0x10c, i0.
    pub life_time: i32,
}

/// Factory case 0xc, `SetPriority`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SetPriority {
    /// +0x10c: `NORMAL` 1, `ALWAYS_EXECUTE` 0, else 0 as constructed.
    pub priority: i32,
}

/// Factory cases 0xd and 0xe, `StayInRange` and `StayOutOfRange`: a
/// `SetZoneBehaviorParams`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Zone {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x10c.
    pub op: SetZoneBehaviorParams,
    /// +0x14c, the op's target when it runs.
    pub object_name: Vec<u8>,
}

/// Factory cases 0xf and 0x10, `MakeNewFormation` and `FollowPlayer`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Formation {
    /// +0x10c, `FindFormation(CreateID(s0))`: the formation's id when
    /// Content has it.
    pub formation: Option<u32>,
    /// +0x110, the member names (`TString<0x30>`).
    pub member_names: Vec<Vec<u8>>,
    /// `MakeNewFormation`'s +0x120, `ADD_FOLLOWERS`; false for
    /// `FollowPlayer`.
    pub add_followers: bool,
}

/// Factory case 0x13, `Dock`: a `DirectiveDockOp`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Dock {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x10c.
    pub op: DockOp,
    /// +0x138, the op's `Field_1C` when it runs.
    pub object_name: Vec<u8>,
}

/// Factory case 0x14, `InstantTradelane`: a `DirectiveInstantTradelaneOp`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct InstantTradelane {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x10c.
    pub op: InstantTradeLaneOp,
}

/// Factory case 0x15, `Launch`: a `DirectiveLaunchOp`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Launch {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x10c.
    pub op: LaunchOp,
}

/// Factory case 0x16, `WaitForPlayerManeuver`: a
/// `DirectiveWaitForPlayerManeuverOp`, whose name points at +0x11c.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct WaitForPlayerManeuver {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x11c, `strcpy` of s0 (D820).
    pub behavior_name: Vec<u8>,
}

/// Factory case 0x19: a `DirectiveTractorBeamOp`; no key makes it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TractorBeam {
    /// The op's `Priority`.
    pub priority: u32,
    /// +0x10c.
    pub op: TractorBeamOp,
}

/// A directive's class, one variant per factory case.
#[derive(Clone, Debug, PartialEq)]
pub enum DirectiveKind {
    /// 0 `Avoidance`.
    Avoidance(Avoidance),
    /// 1 `Delay`.
    Delay(Delay),
    /// 2 `Idle`.
    Idle(Idle),
    /// 3 `GotoShip`.
    GotoShip(Goto),
    /// 4 `GotoSpline`.
    GotoSpline(Goto),
    /// 5 `GotoVec`.
    GotoVec(Goto),
    /// 6 `Trail`.
    Trail(Trail),
    /// 7 `Follow`.
    Follow(Follow),
    /// 8 `Guide`.
    Guide(Guide),
    /// 9 `GuideShip`.
    GuideShip(Guide),
    /// 0xa `Face`.
    Face(Face),
    /// 0xb `SetLifeTime`.
    SetLifeTime(SetLifeTime),
    /// 0xc `SetPriority`.
    SetPriority(SetPriority),
    /// 0xd `StayInRange`.
    StayInRange(Zone),
    /// 0xe `StayOutOfRange`.
    StayOutOfRange(Zone),
    /// 0xf `MakeNewFormation`.
    MakeNewFormation(Formation),
    /// 0x10 `FollowPlayer`.
    FollowPlayer(Formation),
    /// 0x11 `BreakFormation`.
    BreakFormation,
    /// 0x12 `CancelObjective`.
    CancelObjective,
    /// 0x13 `Dock`.
    Dock(Dock),
    /// 0x14 `InstantTradelane`.
    InstantTradelane(InstantTradelane),
    /// 0x15 `Launch`.
    Launch(Launch),
    /// 0x16 `WaitForPlayerManeuver`.
    WaitForPlayerManeuver(WaitForPlayerManeuver),
    /// 0x19, the tractor beam.
    TractorBeam(TractorBeam),
}

impl DirectiveKind {
    /// `CreateObjListDirective(Type)`: the class with its constructor's
    /// values; none for 0x17, 0x18 and above 0x19.
    #[must_use]
    pub fn create(factory: u8) -> Option<Self> {
        Some(match factory {
            0x0 => Self::Avoidance(Avoidance::default()),
            0x1 => Self::Delay(Delay::default()),
            0x2 => Self::Idle(Idle::default()),
            0x3 => Self::GotoShip(Goto::default()),
            0x4 => Self::GotoSpline(Goto::default()),
            0x5 => Self::GotoVec(Goto::default()),
            0x6 => Self::Trail(Trail::default()),
            0x7 => Self::Follow(Follow::default()),
            0x8 => Self::Guide(Guide::default()),
            0x9 => Self::GuideShip(Guide::default()),
            0xa => Self::Face(Face::default()),
            0xb => Self::SetLifeTime(SetLifeTime::default()),
            0xc => Self::SetPriority(SetPriority::default()),
            0xd => Self::StayInRange(Zone::default()),
            0xe => Self::StayOutOfRange(Zone::default()),
            0xf => Self::MakeNewFormation(Formation::default()),
            0x10 => Self::FollowPlayer(Formation::default()),
            0x11 => Self::BreakFormation,
            0x12 => Self::CancelObjective,
            0x13 => Self::Dock(Dock::default()),
            0x14 => Self::InstantTradelane(InstantTradelane::default()),
            0x15 => Self::Launch(Launch::default()),
            0x16 => Self::WaitForPlayerManeuver(WaitForPlayerManeuver::default()),
            0x19 => Self::TractorBeam(TractorBeam::default()),
            _ => return None,
        })
    }

    /// The factory case.
    #[must_use]
    pub fn factory(&self) -> u8 {
        match self {
            Self::Avoidance(_) => 0x0,
            Self::Delay(_) => 0x1,
            Self::Idle(_) => 0x2,
            Self::GotoShip(_) => 0x3,
            Self::GotoSpline(_) => 0x4,
            Self::GotoVec(_) => 0x5,
            Self::Trail(_) => 0x6,
            Self::Follow(_) => 0x7,
            Self::Guide(_) => 0x8,
            Self::GuideShip(_) => 0x9,
            Self::Face(_) => 0xa,
            Self::SetLifeTime(_) => 0xb,
            Self::SetPriority(_) => 0xc,
            Self::StayInRange(_) => 0xd,
            Self::StayOutOfRange(_) => 0xe,
            Self::MakeNewFormation(_) => 0xf,
            Self::FollowPlayer(_) => 0x10,
            Self::BreakFormation => 0x11,
            Self::CancelObjective => 0x12,
            Self::Dock(_) => 0x13,
            Self::InstantTradelane(_) => 0x14,
            Self::Launch(_) => 0x15,
            Self::WaitForPlayerManeuver(_) => 0x16,
            Self::TractorBeam(_) => 0x19,
        }
    }

    /// `GetType` (slot 5): the factory case, but 5 for `Guide` and
    /// `GuideShip`, 0xf for `FollowPlayer` and 0x13 for `InstantTradelane`,
    /// as their vtables answer.
    #[must_use]
    pub fn type_code(&self) -> i32 {
        match self {
            Self::Guide(_) | Self::GuideShip(_) => 5,
            Self::FollowPlayer(_) => 0xf,
            Self::InstantTradelane(_) => 0x13,
            other => i32::from(other.factory()),
        }
    }

    /// The `[ObjList]` key of the class (`TractorBeam` for case 0x19).
    #[must_use]
    pub fn key(&self) -> &'static str {
        let factory = self.factory();
        KEYS.iter()
            .find(|(_, f)| *f == factory)
            .and_then(|(k, _)| std::str::from_utf8(k).ok())
            .unwrap_or("TractorBeam")
    }

    /// The fields in the oracle dump's order.
    #[must_use]
    pub fn fields(&self) -> Vec<(&'static str, Field)> {
        let mut out = Vec::new();
        match self {
            Self::Avoidance(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("flag_id", Field::I32(d.op.flag_id)));
                out.push(("value", Field::U32(u32::from(d.op.value))));
            }
            Self::Delay(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("delay_seconds", Field::F32(d.delay_seconds)));
            }
            Self::Idle(d) => out.push(("priority", Field::U32(d.priority))),
            Self::GotoShip(d) | Self::GotoSpline(d) | Self::GotoVec(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.extend(goto_fields!("", d.op));
                out.push(("field_0x17c", Field::I32(d.field_0x17c)));
                out.push(("target_name", Field::Bytes(d.target_name.clone())));
                out.push(("object_name", Field::Bytes(d.object_name.clone())));
            }
            Self::Trail(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("target", Field::U32(d.op.target)));
                out.push(("distance", Field::F32(d.op.distance)));
                out.push(("field_14", Field::F32(d.op.field_14)));
                out.push(("field_18", Field::F32(d.op.field_18)));
                out.push(("target_name", Field::Bytes(d.target_name.clone())));
            }
            Self::Follow(d) => {
                out.push(("field_0x10c", Field::I32(d.field_0x10c)));
                out.push(("target_name", Field::Bytes(d.target_name.clone())));
                out.push(("priority", Field::U32(d.priority)));
                out.extend(follow_fields!("", d.op));
            }
            Self::Guide(d) | Self::GuideShip(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.extend(goto_fields!("goto_", d.op.goto));
                out.extend(follow_fields!("follow_", d.op.follow));
                out.push(("field_a0", Field::U32(u32::from(d.op.field_a0))));
                out.push(("field_a4", Field::F32(d.op.field_a4)));
                out.push(("field_a8", Field::F32(d.op.field_a8)));
                out.push(("field_ac", Field::I32(d.op.field_ac)));
                out.push(("field_b0", Field::F32(d.op.field_b0)));
                out.push(("field_b4", Field::F32(d.op.field_b4)));
                out.push(("field_b8", Field::F32(d.op.field_b8)));
                out.push(("field_0x1c8", Field::I32(d.field_0x1c8)));
                out.push(("object_name", Field::Bytes(d.object_name.clone())));
                out.push(("target_name", Field::Bytes(d.target_name.clone())));
            }
            Self::Face(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("mode", Field::I32(d.op.mode)));
                out.push(("target", Field::U32(d.op.target)));
                out.push(("point", Field::Vector(d.op.point)));
                out.push(("orientation", Field::Matrix(d.op.orientation)));
                out.push(("field_44", Field::U32(u32::from(d.op.field_44))));
                out.push(("target_name", Field::Bytes(d.target_name.clone())));
            }
            Self::SetLifeTime(d) => out.push(("life_time", Field::I32(d.life_time))),
            Self::SetPriority(d) => out.push(("priority", Field::I32(d.priority))),
            Self::StayInRange(d) | Self::StayOutOfRange(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("mode", Field::I32(d.op.mode)));
                out.push(("invert_mode", Field::I32(d.op.invert_mode)));
                out.push(("target_point", Field::Vector(d.op.target_point)));
                out.push(("target_id", Field::U32(d.op.target_id)));
                out.push(("box_min", Field::Vector(d.op.box_min)));
                out.push(("box_max", Field::Vector(d.op.box_max)));
                out.push(("radius", Field::F32(d.op.radius)));
                out.push(("object_name", Field::Bytes(d.object_name.clone())));
            }
            Self::MakeNewFormation(d) | Self::FollowPlayer(d) => {
                out.push(("formation", Field::from(&d.formation)));
                out.push(("member_names", Field::Strings(d.member_names.clone())));
                out.push(("add_followers", Field::Bool(d.add_followers)));
            }
            Self::BreakFormation | Self::CancelObjective => {}
            Self::Dock(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("dock_object", Field::U32(d.op.dock_object)));
                out.push(("field_10", Field::U32(d.op.field_10)));
                out.push(("field_14", Field::U32(u32::from(d.op.field_14))));
                out.push(("port", Field::I32(d.op.port)));
                out.push(("field_1c", Field::U32(d.op.field_1c)));
                out.push(("field_20", Field::F32(d.op.field_20)));
                out.push(("field_24", Field::F32(d.op.field_24)));
                out.push(("field_28", Field::U32(d.op.field_28)));
                out.push(("object_name", Field::Bytes(d.object_name.clone())));
            }
            Self::InstantTradelane(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("entrance", Field::U32(d.op.entrance)));
                out.push(("exit", Field::U32(d.op.exit)));
                out.push(("speed", Field::F32(d.op.speed)));
            }
            Self::Launch(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("launch_object", Field::U32(d.op.launch_object)));
                out.push(("index", Field::I32(d.op.index)));
                out.push(("terminal", Field::I32(d.op.terminal)));
            }
            Self::WaitForPlayerManeuver(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("behavior_name", Field::Bytes(d.behavior_name.clone())));
            }
            Self::TractorBeam(d) => {
                out.push(("priority", Field::U32(d.priority)));
                out.push(("target", Field::U32(d.op.target)));
                out.push(("field_10", Field::U32(u32::from(d.op.field_10))));
            }
        }
        out
    }

    /// The class's `Parse` (slot 4) of the current line; the base's (none)
    /// for `Idle`, `BreakFormation`, `CancelObjective` and the tractor beam.
    pub fn parse(&mut self, cx: &Cx<'_>, reader: &mut IniReader) {
        match self {
            Self::Avoidance(d) => {
                d.op.flag_id = 0;
                d.op.value = u8::from(value_is(reader, 0, b"true"));
            }
            Self::Delay(d) => d.delay_seconds = f32_at(reader, 0),
            Self::GotoShip(d) => parse_goto_ship(reader, d),
            Self::GotoSpline(d) => parse_goto_spline(reader, d),
            Self::GotoVec(d) => parse_goto_vec(reader, d),
            Self::Trail(d) => {
                d.target_name = value_tstring(reader, 0, NAME);
                d.op.distance = int_f32(reader, 1);
            }
            Self::Follow(d) => parse_follow(reader, d),
            Self::Guide(d) => parse_guide(reader, d),
            Self::GuideShip(d) => parse_guide_ship(reader, d),
            Self::Face(d) => parse_face(reader, d),
            Self::SetLifeTime(d) => d.life_time = reader.value_int(0),
            Self::SetPriority(d) => {
                if value_is(reader, 0, b"NORMAL") {
                    d.priority = 1;
                } else if value_is(reader, 0, b"ALWAYS_EXECUTE") {
                    d.priority = 0;
                }
            }
            Self::StayInRange(d) => {
                d.op.invert_mode = 0;
                parse_zone(reader, d);
            }
            Self::StayOutOfRange(d) => {
                d.op.invert_mode = 1;
                parse_zone(reader, d);
            }
            Self::MakeNewFormation(d) => parse_make_new_formation(cx, reader, d),
            Self::FollowPlayer(d) => parse_follow_player(cx, reader, d),
            Self::Dock(d) => parse_dock(reader, d),
            Self::InstantTradelane(d) => {
                d.op.entrance = nickname_id(&reader.value_string_at(0));
                d.op.exit = if reader.is_value_empty(1) {
                    0
                } else {
                    nickname_id(&reader.value_string_at(1))
                };
            }
            Self::Launch(d) => parse_launch(reader, d),
            Self::WaitForPlayerManeuver(d) => {
                // strcpy into the 0x20 bytes at +0x11c, unbounded (D820).
                d.behavior_name = c_str(&reader.value_string_at(0)).to_vec();
            }
            Self::Idle(_) | Self::BreakFormation | Self::CancelObjective | Self::TractorBeam(_) => {
            }
        }
    }
}

/// One directive of an `[ObjList]`: its line and its class.
#[derive(Clone, Debug, PartialEq)]
pub struct ObjListDirective {
    /// `Name` (+0x08, `TString<0x100>`): `get_line_ptr()`, the whole line.
    pub name: Vec<u8>,
    /// The class and its members.
    pub kind: DirectiveKind,
}

/// `_stricmp(get_value_string(index), text) == 0`.
fn value_is(reader: &IniReader, index: u32, text: &[u8]) -> bool {
    eq_ignore_case(c_str(&reader.value_string_at(index)), text)
}

/// `static_cast<float>(get_value_int(index))`.
fn int_f32(reader: &IniReader, index: u32) -> f32 {
    reader.value_int(index) as f32
}

/// Three ints as a vector.
fn int_vector(reader: &IniReader, index: u32) -> Vector {
    Vector::new(
        int_f32(reader, index),
        int_f32(reader, index + 1),
        int_f32(reader, index + 2),
    )
}

/// `StoreFloatBits(get_value_float(index))`.
fn float_bits(reader: &mut IniReader, index: u32) -> u32 {
    f32_at(reader, index).to_bits()
}

/// `ParseGotoKind` (value `index`): `goto` leaves both flags,
/// `goto_no_cruise` sets the no-cruise flag (`Field_5B`), `goto_cruise`
/// the cruise flag (`Field_5A`).
fn goto_kind(reader: &IniReader, index: u32, op: &mut GotoOp) {
    if value_is(reader, index, b"goto") {
        return;
    }
    if value_is(reader, index, b"goto_no_cruise") {
        op.no_cruise = 1;
        return;
    }
    if value_is(reader, index, b"goto_cruise") {
        op.cruise = 1;
    }
}

/// The tail the three gotos share from value `index` on: the object
/// name, `Field_60` and `Field_64` (ints), `Field_68` (a float's bits) and
/// +0x17c, each optional one only when present.
fn goto_object_tail(reader: &mut IniReader, index: u32, d: &mut Goto) {
    if reader.is_value_empty(index) {
        return;
    }
    d.object_name = value_tstring(reader, index, NAME);
    d.op.field_60 = int_f32(reader, index + 1);
    d.op.field_64 = int_f32(reader, index + 2);
    if !reader.is_value_empty(index + 3) {
        d.op.field_68 = float_bits(reader, index + 3);
    }
    if !reader.is_value_empty(index + 4) {
        d.field_0x17c = reader.value_int(index + 4);
    }
}

/// `FObjListGotoShip::Parse`: the target kind 0 (an object).
fn parse_goto_ship(reader: &mut IniReader, d: &mut Goto) {
    d.field_0x17c = 1;
    goto_kind(reader, 0, &mut d.op);
    d.op.kind = 0;
    d.target_name = value_tstring(reader, 1, NAME);
    d.op.range = f32_at(reader, 2);
    d.op.field_58 = u8::from(value_is(reader, 3, b"true"));
    if !reader.is_value_empty(4) {
        d.op.field_54 = int_f32(reader, 4);
    }
    goto_object_tail(reader, 5, d);
}

/// `FObjListGotoSpline::Parse`: the target kind 2 (a spline); its four
/// points come as values 1-3 (+0x38), 4-6 (+0x20), 7-9 (+0x2c) and 10-12
/// (+0x44), each stored as a float's bits; the range is a float.
fn parse_goto_spline(reader: &mut IniReader, d: &mut Goto) {
    d.field_0x17c = 1;
    goto_kind(reader, 0, &mut d.op);
    d.op.kind = 2;
    // The words from +0x20: value 1 goes to word 6 (+0x38), and so on.
    const WORDS: [usize; 12] = [6, 7, 8, 0, 1, 2, 3, 4, 5, 9, 10, 11];
    for (value, word) in (1..).zip(WORDS) {
        d.op.words_20[word] = float_bits(reader, value);
    }
    d.op.range = f32_at(reader, 13);
    d.op.field_58 = u8::from(reader.value_bool(14));
    if !reader.is_value_empty(15) {
        d.op.field_54 = int_f32(reader, 15);
    }
    goto_object_tail(reader, 16, d);
}

/// `FObjListGotoVec::Parse`: the target kind 1 (a point); the point and
/// the range are ints.
fn parse_goto_vec(reader: &mut IniReader, d: &mut Goto) {
    d.field_0x17c = 1;
    goto_kind(reader, 0, &mut d.op);
    d.op.kind = 1;
    d.op.destination = int_vector(reader, 1);
    d.op.range = int_f32(reader, 4);
    d.op.field_58 = u8::from(reader.value_bool(5));
    if !reader.is_value_empty(6) {
        d.op.field_54 = int_f32(reader, 6);
    }
    goto_object_tail(reader, 7, d);
}

/// `FObjListFollow::Parse`: a distance of exactly -1 becomes 2500.
fn parse_follow(reader: &IniReader, d: &mut Follow) {
    d.field_0x10c = 1;
    d.target_name = value_tstring(reader, 0, NAME);
    d.op.distance = int_f32(reader, 1);
    if d.op.distance == -1.0 {
        d.op.distance = 2500.0;
    }
    if !reader.is_value_empty(2) {
        d.op.offset = int_vector(reader, 2);
    }
    if !reader.is_value_empty(5) {
        d.op.range = int_f32(reader, 5);
    }
}

/// The optional tail both guides share from value `index` on: `Field_A0`
/// (a bool), `Field_AC` (an int), `Field_A4`, `Field_B0`, `Field_B4`,
/// `Field_A8` (floats) and +0x1c8.
fn guide_tail(reader: &mut IniReader, index: u32, d: &mut Guide) {
    if !reader.is_value_empty(index) {
        d.op.field_a0 = u8::from(reader.value_bool(index));
    }
    if !reader.is_value_empty(index + 1) {
        d.op.field_ac = reader.value_int(index + 1);
    }
    if !reader.is_value_empty(index + 2) {
        d.op.field_a4 = f32_at(reader, index + 2);
    }
    if !reader.is_value_empty(index + 3) {
        d.op.field_b0 = f32_at(reader, index + 3);
    }
    if !reader.is_value_empty(index + 4) {
        d.op.field_b4 = f32_at(reader, index + 4);
    }
    if !reader.is_value_empty(index + 5) {
        d.op.field_a8 = f32_at(reader, index + 5);
    }
    if !reader.is_value_empty(index + 6) {
        d.field_0x1c8 = reader.value_int(index + 6);
    }
}

/// `FObjListGuide::Parse`: the goto part's kind 1 (a point).
fn parse_guide(reader: &mut IniReader, d: &mut Guide) {
    d.field_0x1c8 = 1;
    d.target_name = value_tstring(reader, 0, NAME);
    d.op.follow.offset = int_vector(reader, 1);
    d.op.goto.kind = 1;
    goto_kind(reader, 4, &mut d.op.goto);
    d.op.goto.destination = int_vector(reader, 5);
    d.op.goto.range = int_f32(reader, 8);
    d.op.goto.field_58 = u8::from(reader.value_bool(9));
    d.op.goto.field_54 = int_f32(reader, 10);
    d.op.goto.field_60 = int_f32(reader, 11);
    d.op.goto.field_64 = int_f32(reader, 12);
    guide_tail(reader, 13, d);
}

/// `FObjListGuideShip::Parse`: the goto part's kind 0 (an object).
fn parse_guide_ship(reader: &mut IniReader, d: &mut Guide) {
    d.field_0x1c8 = 1;
    d.target_name = value_tstring(reader, 0, NAME);
    d.op.follow.offset = int_vector(reader, 1);
    d.op.goto.kind = 0;
    goto_kind(reader, 4, &mut d.op.goto);
    d.object_name = value_tstring(reader, 5, NAME);
    d.op.goto.range = int_f32(reader, 6);
    d.op.goto.field_58 = u8::from(reader.value_bool(7));
    d.op.goto.field_54 = int_f32(reader, 8);
    d.op.goto.field_60 = int_f32(reader, 9);
    d.op.goto.field_64 = int_f32(reader, 10);
    guide_tail(reader, 11, d);
}

/// `FObjListFace::Parse`: `OBJECT` (mode 1) a name, `POINT` (mode 2) three
/// floats; any other value 0 leaves the mode and reads nothing more.
fn parse_face(reader: &mut IniReader, d: &mut Face) {
    if value_is(reader, 0, b"OBJECT") {
        d.op.mode = 1;
    } else if value_is(reader, 0, b"POINT") {
        d.op.mode = 2;
    }
    if d.op.mode == 1 {
        d.target_name = value_tstring(reader, 1, NAME);
        if !reader.is_value_empty(2) {
            d.op.field_44 = u8::from(reader.value_bool(2));
        }
        return;
    }
    if d.op.mode != 2 {
        return;
    }
    let x = f32_at(reader, 1);
    let y = f32_at(reader, 2);
    let z = f32_at(reader, 3);
    d.op.point = Vector::new(x, y, z);
    if !reader.is_value_empty(4) {
        d.op.field_44 = u8::from(reader.value_bool(4));
    }
}

/// `ParseZone`, after the class's invert mode: with value 5 a box (mode
/// 1, six floats); then with value 3 (counted from the start) a sphere
/// (mode 0, three ints and an int radius) from the next value, else an
/// object (mode 2, a name and an int radius); a present false value after
/// them sets invert mode 2.
fn parse_zone(reader: &mut IniReader, d: &mut Zone) {
    let mut index = 0;
    if !reader.is_value_empty(5) {
        d.op.mode = 1;
        let corner = |reader: &mut IniReader, at: u32| {
            let x = f32_at(reader, at);
            let y = f32_at(reader, at + 1);
            let z = f32_at(reader, at + 2);
            Vector::new(x, y, z)
        };
        d.op.box_min = corner(reader, 0);
        d.op.box_max = corner(reader, 3);
        index = 6;
    }
    if reader.is_value_empty(3) {
        d.op.mode = 2;
        d.object_name = value_tstring(reader, index, NAME);
        d.op.radius = int_f32(reader, index + 1);
        index += 2;
    } else {
        d.op.mode = 0;
        d.op.target_point = int_vector(reader, index);
        d.op.radius = int_f32(reader, index + 3);
        index += 4;
    }
    if !reader.is_value_empty(index) && !reader.value_bool(index) {
        d.op.invert_mode = 2;
    }
}

/// `FObjListMakeNewFormation::Parse`: the formation of `CreateID(s0)`;
/// without it nothing more. From value 1 to the first empty one,
/// `ADD_FOLLOWERS` sets the flag and any other value (`Player` too) is a
/// member.
fn parse_make_new_formation(cx: &Cx<'_>, reader: &IniReader, d: &mut Formation) {
    d.add_followers = false;
    let id = create_id(&reader.value_string_at(0));
    d.formation = cx.logic.content.tables.formation(id).map(|_| id);
    if d.formation.is_none() {
        return;
    }
    let mut index = 1;
    while !reader.is_value_empty(index) {
        if !value_is(reader, index, b"Player") && value_is(reader, index, b"ADD_FOLLOWERS") {
            d.add_followers = true;
        } else {
            d.member_names.push(value_tstring(reader, index, NAME));
        }
        index += 1;
    }
}

/// `FObjListFollowPlayer::Parse`: the formation of `CreateID(s0)`; without
/// it, or without slots, nothing more. Values 1 to the slot count + 1 that
/// are present are members.
fn parse_follow_player(cx: &Cx<'_>, reader: &IniReader, d: &mut Formation) {
    let id = create_id(&reader.value_string_at(0));
    let formation = cx.logic.content.tables.formation(id);
    d.formation = formation.map(|_| id);
    let slots = formation.map_or(0, |f| f.slots.len());
    if slots == 0 {
        return;
    }
    let last = u32::try_from(slots + 1).unwrap_or(u32::MAX);
    for index in 1..=last {
        if !reader.is_value_empty(index) {
            d.member_names.push(value_tstring(reader, index, NAME));
        }
    }
}

/// `FObjListDock::Parse`: the dock object; value 1 `NONE` or a nickname;
/// then optionally the watched object and its two thresholds (ints).
fn parse_dock(reader: &IniReader, d: &mut Dock) {
    d.op.dock_object = nickname_id(&reader.value_string_at(0));
    if reader.is_value_empty(1) {
        d.op.field_10 = 0;
        return;
    }
    d.op.field_10 = if value_is(reader, 1, b"NONE") {
        0
    } else {
        nickname_id(&reader.value_string_at(1))
    };
    if reader.is_value_empty(2) {
        return;
    }
    d.object_name = value_tstring(reader, 2, NAME);
    d.op.field_20 = int_f32(reader, 3);
    d.op.field_24 = int_f32(reader, 4);
}

/// `FObjListLaunch::Parse`: the launch object, the index (an int) and the
/// terminal type by name (any other name leaves it).
fn parse_launch(reader: &IniReader, d: &mut Launch) {
    const TERMINALS: [(&[u8], i32); 8] = [
        (b"NONE", 0),
        (b"BERTH", 1),
        (b"PAD", 2),
        (b"MOOR_SMALL", 3),
        (b"MOOR_MEDIUM", 4),
        (b"MOOR_LARGE", 5),
        (b"RING", 6),
        (b"JUMP_GATE", 7),
    ];
    d.op.launch_object = nickname_id(&reader.value_string_at(0));
    d.op.index = reader.value_int(1);
    if let Some(&(_, terminal)) = TERMINALS.iter().find(|(n, _)| value_is(reader, 2, n)) {
        d.op.terminal = terminal;
    }
}

/// `LoadObjList` (`MissionObjList.cpp:229-262`): one `[ObjList]` section.
/// The nickname is `CreateID("")` until a `nickname` gives the whole value's
/// (the last one wins); `system` is skipped; each directive key appends a
/// new directive with the whole line as its name, parsed by its class;
/// other keys are skipped.
pub fn load(cx: &Cx<'_>, reader: &mut IniReader) -> (u32, Vec<ObjListDirective>) {
    let mut nickname = create_id(b"");
    let mut list = Vec::new();
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            nickname = create_id(&reader.value_string());
            continue;
        }
        if reader.is_value(b"system") {
            continue;
        }
        let Some(kind) = KEYS
            .iter()
            .find(|(key, _)| reader.is_value(key))
            .and_then(|&(_, factory)| DirectiveKind::create(factory))
        else {
            continue;
        };
        let mut directive = ObjListDirective {
            name: tstring(&reader.line(), LINE),
            kind,
        };
        directive.kind.parse(cx, reader);
        list.push(directive);
    }
    (nickname, list)
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use game_sim::data::config::GameData;
    use game_sim::object::World;
    use game_sim::object::world::WorldConfig;

    use super::*;
    use crate::content::tables::ContentTables;
    use crate::content::tables::formations::{Formation as Table, FormationSlot};
    use crate::server::{Logic, RecordingSink};

    fn parsed(text: &str) -> (u32, Vec<ObjListDirective>) {
        let mut world = World::new(Arc::new(GameData::new()), WorldConfig::default());
        let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, true);
        let mut tables = ContentTables::default();
        let slot = FormationSlot {
            position: [0.0; 3],
            player_slot: false,
        };
        tables.formations.insert(
            create_id(b"wedge"),
            Table {
                nickname: create_id(b"wedge"),
                slots: vec![slot; 2],
            },
        );
        logic.content.tables = Arc::new(tables);
        let cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        let mut reader = IniReader::new();
        reader.open_memory(text.as_bytes());
        assert!(reader.read_header());
        load(&cx, &mut reader)
    }

    #[test]
    fn the_keys_make_their_classes() {
        let (nickname, list) = parsed(
            "[ObjList]\nsystem = li01\nnickname = first\nGotoVec = goto_cruise, 1, 2, 3, 400, true\n\
             nickname = second\navoidance = true\nUnknown = 1\nSetLifetime = 30\n",
        );
        assert_eq!(nickname, create_id(b"second"));
        assert_eq!(list.len(), 3);
        assert_eq!(list[0].name, b"GotoVec = goto_cruise, 1, 2, 3, 400, true");
        let DirectiveKind::GotoVec(goto) = &list[0].kind else {
            panic!("a goto");
        };
        assert_eq!(goto.op.kind, 1);
        assert_eq!(goto.op.destination, Vector::new(1.0, 2.0, 3.0));
        assert_eq!(goto.op.range, 400.0);
        assert_eq!(
            (goto.op.cruise, goto.op.no_cruise, goto.op.field_58),
            (1, 0, 1)
        );
        assert_eq!(goto.field_0x17c, 1);
        assert_eq!(list[1].kind.key(), "Avoidance");
        assert_eq!(
            list[2].kind,
            DirectiveKind::SetLifeTime(SetLifeTime { life_time: 30 })
        );
    }

    #[test]
    fn a_list_without_a_nickname_has_the_empty_id() {
        assert_eq!(parsed("[ObjList]\nIdle = 1\n").0, create_id(b""));
    }

    #[test]
    fn the_type_codes_follow_the_vtables() {
        let codes: Vec<i32> = [0x8, 0x9, 0x10, 0x14, 0x19]
            .iter()
            .map(|&f| DirectiveKind::create(f).unwrap().type_code())
            .collect();
        assert_eq!(codes, [5, 5, 0xf, 0x13, 0x19]);
        assert!(DirectiveKind::create(0x17).is_none());
        assert_eq!(DirectiveKind::create(0x19).unwrap().key(), "TractorBeam");
    }

    #[test]
    fn a_zone_with_value_3_is_a_sphere() {
        let (_, list) = parsed(
            "[ObjList]\nStayInRange = 10, 20, 30, 5000, false\nStayOutOfRange = Li01_Planet, 900\n",
        );
        let DirectiveKind::StayInRange(z) = &list[0].kind else {
            panic!("a zone");
        };
        assert_eq!((z.op.mode, z.op.invert_mode), (0, 2));
        assert_eq!(z.op.target_point, Vector::new(10.0, 20.0, 30.0));
        assert_eq!(z.op.radius, 5000.0);
        let DirectiveKind::StayOutOfRange(z) = &list[1].kind else {
            panic!("a zone");
        };
        assert_eq!((z.op.mode, z.op.invert_mode), (2, 1));
        assert_eq!(z.object_name, b"Li01_Planet");
    }

    #[test]
    fn the_formations_are_looked_up() {
        let (_, list) = parsed(
            "[ObjList]\nMakeNewFormation = wedge, a, Player, ADD_FOLLOWERS, b\n\
             MakeNewFormation = none, a\nFollowPlayer = wedge, x, , y, z\n",
        );
        let DirectiveKind::MakeNewFormation(f) = &list[0].kind else {
            panic!("a formation");
        };
        assert_eq!(f.formation, Some(create_id(b"wedge")));
        assert_eq!(
            f.member_names,
            [b"a".to_vec(), b"Player".to_vec(), b"b".to_vec()]
        );
        assert!(f.add_followers);
        assert_eq!(
            list[1].kind,
            DirectiveKind::MakeNewFormation(Formation::default())
        );
        let DirectiveKind::FollowPlayer(f) = &list[2].kind else {
            panic!("a formation");
        };
        // Two slots: values 1 to 3, the empty one skipped.
        assert_eq!(f.member_names, [b"x".to_vec(), b"y".to_vec()]);
    }

    #[test]
    fn a_follow_of_minus_one_follows_at_2500() {
        let (_, list) = parsed("[ObjList]\nFollow = leader, -1, 10, 0, 20\n");
        let DirectiveKind::Follow(f) = &list[0].kind else {
            panic!("a follow");
        };
        assert_eq!(f.op.distance, 2500.0);
        assert_eq!(f.op.offset, Vector::new(10.0, 0.0, 20.0));
        assert_eq!(f.op.range, 400.0);
    }

    #[test]
    fn a_spline_keeps_its_points_in_the_c_order() {
        let (_, list) = parsed(
            "[ObjList]\nGotoSpline = goto, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 300, false\n",
        );
        let DirectiveKind::GotoSpline(g) = &list[0].kind else {
            panic!("a spline");
        };
        let words: Vec<f32> = g.op.words_20.iter().map(|&w| f32::from_bits(w)).collect();
        assert_eq!(
            words,
            [
                4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 1.0, 2.0, 3.0, 10.0, 11.0, 12.0
            ]
        );
        assert_eq!((g.op.kind, g.op.range), (2, 300.0));
    }
}
