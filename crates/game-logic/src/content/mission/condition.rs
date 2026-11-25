//! The trigger conditions (`Content/MissionCnd.h`, `MissionCnd.cpp`; TU21):
//! one [`Condition`] variant for each of the 44 `Cnd_*` classes (decision
//! record 0008 point 6, "Alternatives": enums, not trait objects), each a
//! struct of its class's fields with its constructor's values as
//! [`Default`], and the parse of its line.
//!
//! [`FACTORY`] is the `is_value` chain of `LoadTrigger`
//! (`MissionScript.cpp:88-133`) with the message ids each condition listens
//! to. The run time (activation, `Matches`, saves) is P3.5c's and P3.5b's.

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;

use super::field::{Field, record};
use super::{
    base_id_of, f32_at, find_location, nickname_id, rep_value_from_name, system_id_into, tstring,
    value_tstring,
};
use crate::server::Cx;

/// One ship of `Cnd_NPCSystemEnter` and `Cnd_NPCSystemExit`
/// (`FNpcSystemEntry`): a label and its system (0 after the parse).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NpcSystemEntry {
    /// `Label` (`TString<0x30>`).
    pub label: Vec<u8>,
    /// `SystemId`.
    pub system_id: u32,
}

record! {
    /// `Cnd_BaseEnter` (`CndBaseEnter`): base entered (event `BaseEnter`).
    BaseEnter {
        /// `pub::GetBaseID(s0)` unless `ANY` (0: any base).
        base_id: u32 = 0,
    }
}

record! {
    /// `Cnd_BaseExit` (`CndBaseExit`): base left (event `BaseExit`).
    BaseExit {
        /// `pub::GetBaseID(s0)` unless `ANY`.
        base_id: u32 = 0,
    }
}

record! {
    /// `Cnd_CargoScanned` (`CndCargoScanned`): cargo scanned (event `CargoScanned`).
    CargoScanned {
        /// s0 (`TString<0x30>`), with s1 present.
        scanner_label: Vec<u8> = Vec::new(),
        /// s1.
        scanned_label: Vec<u8> = Vec::new(),
        /// +0x74, written on activation.
        field_0x74: u32 = 0,
        /// +0x78, written on activation.
        field_0x78: u32 = 0,
    }
}

record! {
    /// `Cnd_CharSelect` (`CndCharSelect`): bar character selected (event `GFObjSelect`).
    CharSelect {
        /// `pub::GF::FindLocation(base, s1)`.
        location_id: u32 = 0,
        /// s0 (`TString<0x20>`).
        character: Vec<u8> = Vec::new(),
        /// `pub::GetBaseID(s2)`.
        base_id: u32 = 0,
    }
}

record! {
    /// `Cnd_CommComplete` (`CndCommComplete`): comm finished (event `CommComplete`).
    CommComplete {
        /// Optional s1.
        source_label: Vec<u8> = Vec::new(),
        /// `pub::GetNicknameId(s0)`.
        comm_id: u32 = 0,
        /// Resolved on activation.
        source_obj_id: u32 = 0,
    }
}

record! {
    /// `Cnd_CmpToPlane` (`CndCmpToPlane`): object against a plane (event `Heartbeat`).
    CmpToPlane {
        /// s1.
        object_label: Vec<u8> = Vec::new(),
        /// f2.
        plane_a: f32 = 0.0,
        /// f3.
        plane_b: f32 = 0.0,
        /// f4.
        plane_c: f32 = 0.0,
        /// f5.
        plane_d: f32 = 0.0,
        /// `inside` 0, `outside` 1.
        side: i32 = 0,
        /// Resolved on activation.
        object_id: u32 = 0,
    }
}

record! {
    /// `Cnd_Destroyed` (`CndDestroyed`): objects destroyed (event `ObjectDestroyed`).
    Destroyed {
        /// Destroyed so far.
        count: i32 = 0,
        /// i1 when at least -1.
        needed: i32 = -1,
        /// `CreateID(s0)`.
        label: u32 = 0,
        /// Optional s3.
        killer_label: Vec<u8> = Vec::new(),
        /// Resolved on activation.
        killer_obj_id: u32 = 0,
        /// `EXPLODE` 1, `SILENT` 0, `ALL` 0x3e7, `ALL_IGNORE_LANDING` 0x3e6.
        mode: i32 = 0x3e6,
    }
}

record! {
    /// `Cnd_DistCircle` (`CndDistCircle`): object through a circle (event `Heartbeat`).
    DistCircle {
        /// s0.
        object_label: Vec<u8> = Vec::new(),
        /// s1.
        circle_obj: Vec<u8> = Vec::new(),
        /// Resolved on activation.
        circle_obj_id: u32 = 0,
        /// Run time.
        last_position: Vector = Vector::ZERO,
        /// Run time.
        circle_center: Vector = Vector::ZERO,
        /// Run time.
        normal: Vector = Vector::ZERO,
        /// Run time.
        radius: f32 = 0.0,
        /// Run time.
        plane_offset: f32 = 0.0,
    }
}

record! {
    /// `Cnd_DistShip` (`CndDistShip`): distance between two objects (event `Heartbeat`).
    DistShip {
        /// s1.
        object_a: Vec<u8> = Vec::new(),
        /// s2.
        object_b: Vec<u8> = Vec::new(),
        /// f3, squared when greater than 0.
        distance_sq: f32 = 0.0,
        /// `inside` 1, `outside` 0.
        inside: bool = false,
        /// Run time.
        tick_timer: f32 = 0.0,
        /// f4 when greater than 0.
        tick_interval: f32 = 0.0,
        /// `TICK_ALWAYS` 0, `TICK_TOWARDS` 1, `TICK_AWAY` 2.
        tick_mode: i32 = 0,
        /// Run time.
        last_distance_sq: f32 = 0.0,
        /// Resolved on activation.
        object_a_id: u32 = 0,
        /// Resolved on activation.
        object_b_id: u32 = 0,
    }
}

record! {
    /// `Cnd_DistVec` (`CndDistVec`): distance of an object from a point (event `Heartbeat`).
    DistVec {
        /// f2, f3, f4.
        position: Vector = Vector::ZERO,
        /// s1.
        object_label: Vec<u8> = Vec::new(),
        /// f5, squared unless at most 0.
        distance_sq: f32 = 0.0,
        /// `inside` 1, `outside` 0.
        inside: bool = false,
        /// Run time.
        tick_timer: f32 = 0.0,
        /// f6 when not less than 0.
        tick_interval: f32 = 0.0,
        /// As `Cnd_DistShip`.
        tick_mode: i32 = 0,
        /// Run time.
        last_distance_sq: f32 = 0.0,
        /// Resolved on activation.
        object_id: u32 = 0,
    }
}

record! {
    /// `Cnd_DistVecLbl` (`CndDistVecLbl`): distance of labelled objects from a point (event `Heartbeat`).
    DistVecLbl {
        /// f3, f4, f5.
        position: Vector = Vector::ZERO,
        /// `CreateID(s2)` (`CreateID(null)` first).
        label: u32 = 0,
        /// f6, squared unless at most 0.
        distance_sq: f32 = 0.0,
        /// `inside` 1, `outside` 0.
        inside: bool = false,
        /// s1 is `any`.
        any: bool = false,
        /// Run time.
        tick_timer: f32 = 0.0,
        /// f7 when not less than 0.
        tick_interval: f32 = 0.0,
    }
}

record! {
    /// `Cnd_EncLaunched` (`CndEncLaunched`): encounter launched (event `EncounterLaunched`).
    EncLaunched {
        /// `CreateID(s0)`.
        encounter_id: u32 = 0,
    }
}

record! {
    /// `Cnd_HasMsn` (`CndHasMsn`): the player has a mission (event `Heartbeat`).
    HasMsn {
        /// `yes` 0, `no` 1.
        wanted: i32 = 0,
    }
}

record! {
    /// `Cnd_HealthDec` (`CndHealthDec`): health fell (event `ObjectDamaged`).
    HealthDec {
        /// s0.
        object_label: Vec<u8> = Vec::new(),
        /// Optional s2.
        attacker_label: Vec<u8> = Vec::new(),
        /// f1.
        health: f32 = 0.0,
        /// Resolved on activation.
        object_id: u32 = 0,
        /// Resolved on activation.
        attacker_id: u32 = 0,
    }
}

record! {
    /// `Cnd_InSpace` (`CndInSpace`): the player is in space (event `Heartbeat`).
    InSpace {
        /// b0.
        in_space: bool = true,
    }
}

record! {
    /// `Cnd_InTradelane` (`CndInTradelane`): an object is in a trade lane (event `Heartbeat`).
    InTradelane {
        /// s1.
        object_label: Vec<u8> = Vec::new(),
        /// b0.
        in_lane: bool = true,
    }
}

record! {
    /// `Cnd_InZone` (`CndInZone`): an object is in a zone (event `Heartbeat`).
    InZone {
        /// b0.
        in_zone: bool = true,
        /// Optional s2: the zone kind.
        zone_flags: u32 = 0,
        /// s1.
        object_label: Vec<u8> = Vec::new(),
        /// Resolved on activation.
        object_id: u32 = 0,
    }
}

record! {
    /// `Cnd_JumpgateAct` (`CndJumpgateAct`): jump gate activated (event `JumpgateAct`).
    JumpgateAct {
        /// s0.
        jumpgate: Vec<u8> = Vec::new(),
        /// `pub::GetNicknameId(s0)`.
        gate_id: u32 = 0,
    }
}

record! {
    /// `Cnd_JumpInComplete` (`CndJumpInComplete`): jump-in finished (event `JumpInComplete`).
    JumpInComplete {
        /// `pub::GetSystemID(s0)` unless `ANY`.
        system_id: u32 = 0,
    }
}

record! {
    /// `Cnd_LaunchComplete` (`CndLaunchComplete`): launch finished (event `LaunchComplete`).
    LaunchComplete {
        /// Optional s1.
        base_nickname: Vec<u8> = Vec::new(),
        /// s0.
        ship_label: Vec<u8> = Vec::new(),
        /// `pub::GetNicknameId(s1)`.
        base_id: u32 = 0,
        /// Resolved on activation.
        ship_id: u32 = 0,
    }
}

record! {
    /// `Cnd_LocEnter` (`CndLocEnter`): location entered (event `LocationEnter`).
    LocEnter {
        /// `pub::GF::FindLocation(base, s0)`.
        location_id: u32 = 0,
        /// `pub::GetBaseID(s1)`.
        base_id: u32 = 0,
    }
}

record! {
    /// `Cnd_LocExit` (`CndLocExit`): location left (event `LocationExit`).
    LocExit {
        /// `pub::GF::FindLocation(base, s0)`.
        location_id: u32 = 0,
        /// `pub::GetBaseID(s1)`.
        base_id: u32 = 0,
    }
}

record! {
    /// `Cnd_LootAcquired` (`CndLootAcquired`): loot picked up (event `LootAcquired`).
    LootAcquired {
        /// `CreateID(s0)` (`CreateID(null)` first).
        loot_label: u32 = 0,
        /// s1.
        ship_label: Vec<u8> = Vec::new(),
        /// Run time.
        count: i32 = 0,
        /// Optional i2.
        needed: i32 = -1,
    }
}

record! {
    /// `Cnd_MsnResponse` (`CndMsnResponse`): answer to the offer (event `MissionResponse`).
    MsnResponse {
        /// `accept` 0, `reject` 1.
        response: i32 = 0,
    }
}

record! {
    /// `Cnd_NPCSystemEnter` (`CndNpcSystemEnter`): ships entered a system (events `NpcSystemEnter`, `Heartbeat`, `NpcShipCreated`).
    NpcSystemEnter {
        /// The labels s1.. (`Player` skipped), then `Player`.
        ships: Vec<NpcSystemEntry> = Vec::new(),
        /// Run time.
        awaited: Vec<NpcSystemEntry> = Vec::new(),
        /// Run time.
        pending: Vec<NpcSystemEntry> = Vec::new(),
        /// `pub::GetSystemID(s0)` unless `ANY`.
        system_id: u32 = 0,
        /// s0 is `ANY`.
        any_system: bool = false,
        /// Run time.
        delay_running: bool = false,
        /// Run time.
        delay_start: f32 = 0.0,
        /// Run time.
        delay_elapsed: f32 = 0.0,
        /// Run time.
        delay: f32 = 0.0,
        /// Run time.
        loaded_same_system: bool = false,
    }
}

record! {
    /// `Cnd_NPCSystemExit` (`CndNpcSystemExit`): ships left a system (event `NpcSystemExit`).
    NpcSystemExit {
        /// The labels s1..
        ships: Vec<NpcSystemEntry> = Vec::new(),
        /// Run time.
        awaited: Vec<NpcSystemEntry> = Vec::new(),
        /// Run time.
        pending: Vec<NpcSystemEntry> = Vec::new(),
        /// `pub::GetSystemID(s0)` unless `ANY`.
        system_id: u32 = 0,
        /// s0 is `ANY`.
        any_system: bool = false,
    }
}

record! {
    /// `Cnd_PlayerLaunch` (`CndPlayerLaunch`): the player launched (event `PlayerLaunch`).
    PlayerLaunch {
    }
}

record! {
    /// `Cnd_PlayerManeuver` (`CndPlayerManeuver`): the player maneuvered (event `PlayerManeuver`).
    PlayerManeuver {
        /// `BehaviorTypes::get_behavior_id(s0)`.
        behavior_id: i32 = 0,
        /// Optional s1.
        target_label: Vec<u8> = Vec::new(),
        /// s1 contains `inverse`.
        inverse: bool = false,
        /// Resolved on activation.
        target_id: u32 = 0,
    }
}

record! {
    /// `Cnd_PopUpDialog` (`CndPopUpDialog`): a pop-up answered (event `PopUpDialog`).
    PopUpDialog {
        /// `YES` 1, `NO` 2, `LATER` 4, `CLOSE` 8.
        button_mask: u32 = 0,
    }
}

record! {
    /// `Cnd_ProjHit` (`CndProjHit`): projectile hits (event `ObjectDamaged`).
    ProjHit {
        /// s0.
        target_label: Vec<u8> = Vec::new(),
        /// Optional s2.
        shooter_label: Vec<u8> = Vec::new(),
        /// i1.
        needed: i32 = 1,
        /// Resolved on activation.
        target_id: u32 = 0,
        /// Resolved on activation.
        shooter_id: u32 = 0,
        /// Run time.
        count: i32 = 0,
    }
}

record! {
    /// `Cnd_ProjHitShipToLbl` (`CndProjHitShipToLbl`): projectile hits on a label (event `ObjectDamaged`).
    ProjHitShipToLbl {
        /// `CreateID(s0)`.
        target_label: u32 = 0,
        /// Optional s2.
        shooter_label: Vec<u8> = Vec::new(),
        /// i1.
        needed: i32 = 1,
        /// Resolved on activation.
        shooter_id: u32 = 0,
        /// Run time.
        count: i32 = 0,
    }
}

record! {
    /// `Cnd_RTCDone` (`CndRtcDone`): a cut scene finished (event `RtcDone`).
    RtcDone {
        /// s0 (`TString<0x80>`).
        rtc_file: Vec<u8> = Vec::new(),
    }
}

record! {
    /// `Cnd_RumorHeard` (`CndRumorHeard`): a rumor heard (event `RumorHeard`).
    RumorHeard {
        /// `pub::GF::FindLocation(base, s1)`.
        location_id: u32 = 0,
        /// s0 (`TString<0x20>`).
        rumor: Vec<u8> = Vec::new(),
        /// `pub::GetBaseID(s2)`.
        base_id: u32 = 0,
    }
}

record! {
    /// `Cnd_SpaceEnter` (`CndSpaceEnter`): space entered (event `SpaceEnter`).
    SpaceEnter {
        /// `pub::GetSystemID(s0)` unless absent or `no_params`.
        system_id: u32 = 0,
    }
}

record! {
    /// `Cnd_SpaceExit` (`CndSpaceExit`): space left (event `SpaceExit`).
    SpaceExit {
        /// `pub::GetSystemID(s0)` unless absent or `no_params`.
        system_id: u32 = 0,
    }
}

record! {
    /// `Cnd_SystemEnter` (`CndSystemEnter`): system entered (event `SystemEnter`).
    SystemEnter {
        /// Optional s1 unless `ANY`.
        from_system_id: u32 = 0,
        /// s0 unless `ANY`.
        system_id: u32 = 0,
    }
}

record! {
    /// `Cnd_SystemExit` (`CndSystemExit`): system left (event `SystemExit`).
    SystemExit {
        /// s0 unless `ANY`.
        system_id: u32 = 0,
        /// Optional s1 unless `ANY`.
        to_system_id: u32 = 0,
    }
}

record! {
    /// `Cnd_TetherBroke` (`CndTetherBroke`): a tether broke (event `TetherObject`).
    TetherBroke {
        /// s0.
        ship_a: Vec<u8> = Vec::new(),
        /// s1.
        ship_b: Vec<u8> = Vec::new(),
        /// Resolved on activation.
        ship_a_id: u32 = 0,
        /// Resolved on activation.
        ship_b_id: u32 = 0,
        /// Run time.
        field_0x7c: bool = false,
        /// f2, squared when greater than 0.
        distance_sq: f32 = 0.0,
        /// Optional f4, squared unless less than 0.
        field_0x84: f32 = 0.0,
        /// Run time.
        field_0x88: f32 = 0.0,
        /// f3.
        field_0x8c: f32 = 0.0,
        /// Run time.
        field_0x90: f32 = 0.0,
        /// Run time.
        field_0x94: f32 = 0.0,
    }
}

record! {
    /// `Cnd_Timer` (`CndTimer`): a timer (event `Heartbeat`).
    Timer {
        /// Run time.
        elapsed: f32 = 0.0,
        /// f0, or a draw between f0 and f1.
        duration: f32 = 0.0,
        /// `RUN_ON_BASE` 1, `RUN_IN_SPACE_ONLY` 0.
        run_on_base: bool = true,
    }
}

record! {
    /// `Cnd_TLEntered` (`CndTlEntered`): trade lane entered (event `TradelaneEnter`).
    TlEntered {
        /// s0.
        ship_label: Vec<u8> = Vec::new(),
        /// s1.
        ring_a: Vec<u8> = Vec::new(),
        /// s2.
        ring_b: Vec<u8> = Vec::new(),
        /// Resolved on activation.
        ship_id: u32 = 0,
        /// `pub::GetNicknameId(s1)`.
        ring_a_id: u32 = 0,
        /// `pub::GetNicknameId(s2)`.
        ring_b_id: u32 = 0,
    }
}

record! {
    /// `Cnd_TLExited` (`CndTlExited`): trade lane left (event `TradelaneExit`).
    TlExited {
        /// s0.
        ship_label: Vec<u8> = Vec::new(),
        /// s1.
        ring_a: Vec<u8> = Vec::new(),
        /// Optional s2.
        ring_b: Vec<u8> = Vec::new(),
        /// Resolved on activation.
        ship_id: u32 = 0,
        /// `pub::GetNicknameId(s2)`.
        ring_b_id: u32 = 0,
        /// `pub::GetNicknameId(s1)` unless `ANY`.
        ring_a_id: u32 = 0,
    }
}

record! {
    /// `Cnd_True` (`CndTrue`): always true (event `Heartbeat`).
    True {
    }
}

record! {
    /// `Cnd_WatchTrigger` (`CndWatchTrigger`): another trigger's state (event `Heartbeat`).
    WatchTrigger {
        /// `CreateID(s0)`.
        watched_trigger_id: u32 = 0,
        /// `CreateID(s1)`.
        wanted_state: u32 = 0,
    }
}

record! {
    /// `Cnd_WatchVibe` (`CndWatchVibe`): an attitude (event `Heartbeat`).
    WatchVibe {
        /// `lt` 0, `gt` 1, `eq` 2.
        compare: i32 = 0,
        /// s0.
        object_a: Vec<u8> = Vec::new(),
        /// s1.
        object_b: Vec<u8> = Vec::new(),
        /// Resolved on activation.
        object_a_id: u32 = 0,
        /// Resolved on activation.
        object_b_id: u32 = 0,
        /// f2, or the value of a `REP_*` name.
        value: f32 = 0.0,
    }
}

/// A condition: one variant per C++ class, in the factory's order.
#[derive(Clone, Debug, PartialEq)]
pub enum Condition {
    /// `Cnd_BaseEnter`.
    BaseEnter(BaseEnter),
    /// `Cnd_BaseExit`.
    BaseExit(BaseExit),
    /// `Cnd_CargoScanned`.
    CargoScanned(CargoScanned),
    /// `Cnd_CharSelect`.
    CharSelect(CharSelect),
    /// `Cnd_CommComplete`.
    CommComplete(CommComplete),
    /// `Cnd_CmpToPlane`.
    CmpToPlane(CmpToPlane),
    /// `Cnd_Destroyed`.
    Destroyed(Destroyed),
    /// `Cnd_DistCircle`.
    DistCircle(DistCircle),
    /// `Cnd_DistShip`.
    DistShip(DistShip),
    /// `Cnd_DistVec`.
    DistVec(DistVec),
    /// `Cnd_DistVecLbl`.
    DistVecLbl(DistVecLbl),
    /// `Cnd_EncLaunched`.
    EncLaunched(EncLaunched),
    /// `Cnd_HasMsn`.
    HasMsn(HasMsn),
    /// `Cnd_HealthDec`.
    HealthDec(HealthDec),
    /// `Cnd_InSpace`.
    InSpace(InSpace),
    /// `Cnd_InTradelane`.
    InTradelane(InTradelane),
    /// `Cnd_InZone`.
    InZone(InZone),
    /// `Cnd_JumpgateAct`.
    JumpgateAct(JumpgateAct),
    /// `Cnd_JumpInComplete`.
    JumpInComplete(JumpInComplete),
    /// `Cnd_LaunchComplete`.
    LaunchComplete(LaunchComplete),
    /// `Cnd_LocEnter`.
    LocEnter(LocEnter),
    /// `Cnd_LocExit`.
    LocExit(LocExit),
    /// `Cnd_LootAcquired`.
    LootAcquired(LootAcquired),
    /// `Cnd_MsnResponse`.
    MsnResponse(MsnResponse),
    /// `Cnd_NPCSystemEnter`.
    NpcSystemEnter(NpcSystemEnter),
    /// `Cnd_NPCSystemExit`.
    NpcSystemExit(NpcSystemExit),
    /// `Cnd_PlayerLaunch`.
    PlayerLaunch(PlayerLaunch),
    /// `Cnd_PlayerManeuver`.
    PlayerManeuver(PlayerManeuver),
    /// `Cnd_PopUpDialog`.
    PopUpDialog(PopUpDialog),
    /// `Cnd_ProjHit`.
    ProjHit(ProjHit),
    /// `Cnd_ProjHitShipToLbl`.
    ProjHitShipToLbl(ProjHitShipToLbl),
    /// `Cnd_RTCDone`.
    RtcDone(RtcDone),
    /// `Cnd_RumorHeard`.
    RumorHeard(RumorHeard),
    /// `Cnd_SpaceEnter`.
    SpaceEnter(SpaceEnter),
    /// `Cnd_SpaceExit`.
    SpaceExit(SpaceExit),
    /// `Cnd_SystemEnter`.
    SystemEnter(SystemEnter),
    /// `Cnd_SystemExit`.
    SystemExit(SystemExit),
    /// `Cnd_TetherBroke`.
    TetherBroke(TetherBroke),
    /// `Cnd_Timer`.
    Timer(Timer),
    /// `Cnd_TLEntered`.
    TlEntered(TlEntered),
    /// `Cnd_TLExited`.
    TlExited(TlExited),
    /// `Cnd_True`.
    True(True),
    /// `Cnd_WatchTrigger`.
    WatchTrigger(WatchTrigger),
    /// `Cnd_WatchVibe`.
    WatchVibe(WatchVibe),
}

impl Condition {
    /// The factory name of the line that made it.
    #[must_use]
    pub fn name(&self) -> &'static str {
        match self {
            Self::BaseEnter(_) => "Cnd_BaseEnter",
            Self::BaseExit(_) => "Cnd_BaseExit",
            Self::CargoScanned(_) => "Cnd_CargoScanned",
            Self::CharSelect(_) => "Cnd_CharSelect",
            Self::CommComplete(_) => "Cnd_CommComplete",
            Self::CmpToPlane(_) => "Cnd_CmpToPlane",
            Self::Destroyed(_) => "Cnd_Destroyed",
            Self::DistCircle(_) => "Cnd_DistCircle",
            Self::DistShip(_) => "Cnd_DistShip",
            Self::DistVec(_) => "Cnd_DistVec",
            Self::DistVecLbl(_) => "Cnd_DistVecLbl",
            Self::EncLaunched(_) => "Cnd_EncLaunched",
            Self::HasMsn(_) => "Cnd_HasMsn",
            Self::HealthDec(_) => "Cnd_HealthDec",
            Self::InSpace(_) => "Cnd_InSpace",
            Self::InTradelane(_) => "Cnd_InTradelane",
            Self::InZone(_) => "Cnd_InZone",
            Self::JumpgateAct(_) => "Cnd_JumpgateAct",
            Self::JumpInComplete(_) => "Cnd_JumpInComplete",
            Self::LaunchComplete(_) => "Cnd_LaunchComplete",
            Self::LocEnter(_) => "Cnd_LocEnter",
            Self::LocExit(_) => "Cnd_LocExit",
            Self::LootAcquired(_) => "Cnd_LootAcquired",
            Self::MsnResponse(_) => "Cnd_MsnResponse",
            Self::NpcSystemEnter(_) => "Cnd_NPCSystemEnter",
            Self::NpcSystemExit(_) => "Cnd_NPCSystemExit",
            Self::PlayerLaunch(_) => "Cnd_PlayerLaunch",
            Self::PlayerManeuver(_) => "Cnd_PlayerManeuver",
            Self::PopUpDialog(_) => "Cnd_PopUpDialog",
            Self::ProjHit(_) => "Cnd_ProjHit",
            Self::ProjHitShipToLbl(_) => "Cnd_ProjHitShipToLbl",
            Self::RtcDone(_) => "Cnd_RTCDone",
            Self::RumorHeard(_) => "Cnd_RumorHeard",
            Self::SpaceEnter(_) => "Cnd_SpaceEnter",
            Self::SpaceExit(_) => "Cnd_SpaceExit",
            Self::SystemEnter(_) => "Cnd_SystemEnter",
            Self::SystemExit(_) => "Cnd_SystemExit",
            Self::TetherBroke(_) => "Cnd_TetherBroke",
            Self::Timer(_) => "Cnd_Timer",
            Self::TlEntered(_) => "Cnd_TLEntered",
            Self::TlExited(_) => "Cnd_TLExited",
            Self::True(_) => "Cnd_True",
            Self::WatchTrigger(_) => "Cnd_WatchTrigger",
            Self::WatchVibe(_) => "Cnd_WatchVibe",
        }
    }

    /// The fields in the oracle dump's order.
    #[must_use]
    pub fn fields(&self) -> Vec<(&'static str, Field)> {
        match self {
            Self::BaseEnter(r) => r.fields(),
            Self::BaseExit(r) => r.fields(),
            Self::CargoScanned(r) => r.fields(),
            Self::CharSelect(r) => r.fields(),
            Self::CommComplete(r) => r.fields(),
            Self::CmpToPlane(r) => r.fields(),
            Self::Destroyed(r) => r.fields(),
            Self::DistCircle(r) => r.fields(),
            Self::DistShip(r) => r.fields(),
            Self::DistVec(r) => r.fields(),
            Self::DistVecLbl(r) => r.fields(),
            Self::EncLaunched(r) => r.fields(),
            Self::HasMsn(r) => r.fields(),
            Self::HealthDec(r) => r.fields(),
            Self::InSpace(r) => r.fields(),
            Self::InTradelane(r) => r.fields(),
            Self::InZone(r) => r.fields(),
            Self::JumpgateAct(r) => r.fields(),
            Self::JumpInComplete(r) => r.fields(),
            Self::LaunchComplete(r) => r.fields(),
            Self::LocEnter(r) => r.fields(),
            Self::LocExit(r) => r.fields(),
            Self::LootAcquired(r) => r.fields(),
            Self::MsnResponse(r) => r.fields(),
            Self::NpcSystemEnter(r) => r.fields(),
            Self::NpcSystemExit(r) => r.fields(),
            Self::PlayerLaunch(r) => r.fields(),
            Self::PlayerManeuver(r) => r.fields(),
            Self::PopUpDialog(r) => r.fields(),
            Self::ProjHit(r) => r.fields(),
            Self::ProjHitShipToLbl(r) => r.fields(),
            Self::RtcDone(r) => r.fields(),
            Self::RumorHeard(r) => r.fields(),
            Self::SpaceEnter(r) => r.fields(),
            Self::SpaceExit(r) => r.fields(),
            Self::SystemEnter(r) => r.fields(),
            Self::SystemExit(r) => r.fields(),
            Self::TetherBroke(r) => r.fields(),
            Self::Timer(r) => r.fields(),
            Self::TlEntered(r) => r.fields(),
            Self::TlExited(r) => r.fields(),
            Self::True(r) => r.fields(),
            Self::WatchTrigger(r) => r.fields(),
            Self::WatchVibe(r) => r.fields(),
        }
    }
}

/// The message ids a condition listens to (`pub::Controller::EMessage`).
pub mod event {
    /// `BaseEnter`.
    pub const BASE_ENTER: u32 = 0x01;
    /// `BaseExit`.
    pub const BASE_EXIT: u32 = 0x02;
    /// `CargoScanned`.
    pub const CARGO_SCANNED: u32 = 0x03;
    /// `PlayerLaunch`.
    pub const PLAYER_LAUNCH: u32 = 0x06;
    /// `CommComplete`.
    pub const COMM_COMPLETE: u32 = 0x0b;
    /// `ObjectDamaged`.
    pub const OBJECT_DAMAGED: u32 = 0x0d;
    /// `TradelaneEnter`.
    pub const TRADELANE_ENTER: u32 = 0x16;
    /// `TradelaneExit`.
    pub const TRADELANE_EXIT: u32 = 0x17;
    /// `LootAcquired`.
    pub const LOOT_ACQUIRED: u32 = 0x18;
    /// `GFObjSelect`.
    pub const GF_OBJ_SELECT: u32 = 0x1b;
    /// `Heartbeat`.
    pub const HEARTBEAT: u32 = 0x20;
    /// `RtcDone`.
    pub const RTC_DONE: u32 = 0x22;
    /// `JumpInComplete`.
    pub const JUMP_IN_COMPLETE: u32 = 0x26;
    /// `LaunchComplete`.
    pub const LAUNCH_COMPLETE: u32 = 0x28;
    /// `LocationEnter`.
    pub const LOCATION_ENTER: u32 = 0x29;
    /// `LocationExit`.
    pub const LOCATION_EXIT: u32 = 0x2a;
    /// `MissionResponse`.
    pub const MISSION_RESPONSE: u32 = 0x2c;
    /// `PlayerManeuver`.
    pub const PLAYER_MANEUVER: u32 = 0x2f;
    /// `PopUpDialog`.
    pub const POP_UP_DIALOG: u32 = 0x30;
    /// `SpaceEnter`.
    pub const SPACE_ENTER: u32 = 0x36;
    /// `SpaceExit`.
    pub const SPACE_EXIT: u32 = 0x37;
    /// `ObjectDestroyed`.
    pub const OBJECT_DESTROYED: u32 = 0x39;
    /// `SystemExit`.
    pub const SYSTEM_EXIT: u32 = 0x3a;
    /// `SystemEnter`.
    pub const SYSTEM_ENTER: u32 = 0x3b;
    /// `JumpgateAct`.
    pub const JUMPGATE_ACT: u32 = 0x1007;
    /// `NpcSystemExit`.
    pub const NPC_SYSTEM_EXIT: u32 = 0x100a;
    /// `EncounterLaunched`.
    pub const ENCOUNTER_LAUNCHED: u32 = 0x100c;
    /// `RumorHeard`.
    pub const RUMOR_HEARD: u32 = 0x100d;
    /// `NpcSystemEnter`.
    pub const NPC_SYSTEM_ENTER: u32 = 0x100f;
    /// `NpcShipCreated`.
    pub const NPC_SHIP_CREATED: u32 = 0x1010;
    /// `TetherObject`.
    pub const TETHER_OBJECT: u32 = 0x1011;
}

/// `GCndFactory` (`MissionScript.cpp:88-133`): the `Cnd_*` names in the
/// order of the `is_value` chain, each with the message ids pushed into the
/// trigger after the parse.
pub const FACTORY: [(&[u8], &[u32]); 44] = {
    use event::*;
    [
        (b"Cnd_BaseEnter", &[BASE_ENTER]),
        (b"Cnd_BaseExit", &[BASE_EXIT]),
        (b"Cnd_CargoScanned", &[CARGO_SCANNED]),
        (b"Cnd_CharSelect", &[GF_OBJ_SELECT]),
        (b"Cnd_CommComplete", &[COMM_COMPLETE]),
        (b"Cnd_CmpToPlane", &[HEARTBEAT]),
        (b"Cnd_Destroyed", &[OBJECT_DESTROYED]),
        (b"Cnd_DistCircle", &[HEARTBEAT]),
        (b"Cnd_DistShip", &[HEARTBEAT]),
        (b"Cnd_DistVec", &[HEARTBEAT]),
        (b"Cnd_DistVecLbl", &[HEARTBEAT]),
        (b"Cnd_EncLaunched", &[ENCOUNTER_LAUNCHED]),
        (b"Cnd_HasMsn", &[HEARTBEAT]),
        (b"Cnd_HealthDec", &[OBJECT_DAMAGED]),
        (b"Cnd_InSpace", &[HEARTBEAT]),
        (b"Cnd_InTradelane", &[HEARTBEAT]),
        (b"Cnd_InZone", &[HEARTBEAT]),
        (b"Cnd_JumpgateAct", &[JUMPGATE_ACT]),
        (b"Cnd_JumpInComplete", &[JUMP_IN_COMPLETE]),
        (b"Cnd_LaunchComplete", &[LAUNCH_COMPLETE]),
        (b"Cnd_LocEnter", &[LOCATION_ENTER]),
        (b"Cnd_LocExit", &[LOCATION_EXIT]),
        (b"Cnd_LootAcquired", &[LOOT_ACQUIRED]),
        (b"Cnd_MsnResponse", &[MISSION_RESPONSE]),
        (
            b"Cnd_NPCSystemEnter",
            &[NPC_SYSTEM_ENTER, HEARTBEAT, NPC_SHIP_CREATED],
        ),
        (b"Cnd_NPCSystemExit", &[NPC_SYSTEM_EXIT]),
        (b"Cnd_PlayerLaunch", &[PLAYER_LAUNCH]),
        (b"Cnd_PlayerManeuver", &[PLAYER_MANEUVER]),
        (b"Cnd_PopUpDialog", &[POP_UP_DIALOG]),
        (b"Cnd_ProjHit", &[OBJECT_DAMAGED]),
        (b"Cnd_ProjHitShipToLbl", &[OBJECT_DAMAGED]),
        (b"Cnd_RTCDone", &[RTC_DONE]),
        (b"Cnd_RumorHeard", &[RUMOR_HEARD]),
        (b"Cnd_SpaceEnter", &[SPACE_ENTER]),
        (b"Cnd_SpaceExit", &[SPACE_EXIT]),
        (b"Cnd_SystemEnter", &[SYSTEM_ENTER]),
        (b"Cnd_SystemExit", &[SYSTEM_EXIT]),
        (b"Cnd_TetherBroke", &[TETHER_OBJECT]),
        (b"Cnd_Timer", &[HEARTBEAT]),
        (b"Cnd_TLEntered", &[TRADELANE_ENTER]),
        (b"Cnd_TLExited", &[TRADELANE_EXIT]),
        (b"Cnd_True", &[HEARTBEAT]),
        (b"Cnd_WatchTrigger", &[HEARTBEAT]),
        (b"Cnd_WatchVibe", &[HEARTBEAT]),
    ]
};

/// `GRandUnitScale` (`MissionCnd.cpp:25`): 2^-15, `rand()`'s scale in
/// `Cnd_Timer::Parse`.
const RAND_UNIT_SCALE: f32 = 1.0 / 32768.0;

/// `SquareIfGreater`: squared only when greater than 0.
fn square_if_greater(value: f32) -> f32 {
    if value > 0.0 { value * value } else { value }
}

/// `SquareUnlessLessOrEqual`: squared when greater or unordered.
fn square_unless_less_or_equal(value: f32) -> f32 {
    if value <= 0.0 { value } else { value * value }
}

/// `SquareUnlessLess`: squared when greater, equal or unordered.
fn square_unless_less(value: f32) -> f32 {
    if value < 0.0 { value } else { value * value }
}

/// `inside` true, `outside` false, else unchanged (`_stricmp`).
fn side(reader: &IniReader, index: u32, slot: &mut bool) {
    let name = reader.value_string_at(index);
    if eq_ignore_case(c_str(&name), b"inside") {
        *slot = true;
    } else if eq_ignore_case(c_str(&name), b"outside") {
        *slot = false;
    }
}

/// `ParseTickMode`: a missing or unknown slot leaves the mode.
fn tick_mode(reader: &IniReader, index: u32, mode: &mut i32) {
    if reader.is_value_empty(index) {
        return;
    }
    let name = reader.value_string_at(index);
    let name = c_str(&name);
    if eq_ignore_case(b"TICK_AWAY", name) {
        *mode = 2;
    } else if eq_ignore_case(b"TICK_TOWARDS", name) {
        *mode = 1;
    } else if eq_ignore_case(b"TICK_ALWAYS", name) {
        *mode = 0;
    }
}

/// `ParseBaseCondition`: s0 into a `TString<0x20>`; unless it is `ANY`
/// (case-sensitive), `pub::GetBaseID` of the copy.
fn base_condition(cx: &mut Cx<'_>, reader: &IniReader, base: &mut u32) {
    let name = value_tstring(reader, 0, 0x20);
    if name != b"ANY" {
        *base = base_id_of(cx, &name);
    }
}

/// `ParseLocationCondition`: the base of s1, then the location s0 in it.
fn location_condition(cx: &mut Cx<'_>, reader: &IniReader, location: &mut u32, base: &mut u32) {
    *base = base_id_of(cx, &reader.value_string_at(1));
    *location = find_location(*base, &reader.value_string_at(0));
}

/// `ParseCharacterCondition`: the name s0, the base s2, the location s1.
fn character_condition(
    cx: &mut Cx<'_>,
    reader: &IniReader,
    name: &mut Vec<u8>,
    base: &mut u32,
    location: &mut u32,
) {
    *name = value_tstring(reader, 0, 0x20);
    *base = base_id_of(cx, &reader.value_string_at(2));
    *location = find_location(*base, &reader.value_string_at(1));
}

/// `ParseSpaceCondition`: s0 into a `TString<0x40>`; with s0 present and
/// not `no_params` (case-sensitive), `pub::GetSystemID(s0)`.
fn space_condition(cx: &mut Cx<'_>, reader: &IniReader, system: &mut u32) {
    let name = value_tstring(reader, 0, 0x40);
    if reader.is_value_empty(0) || name == b"no_params" {
        return;
    }
    system_id_into(cx, system, &reader.value_string_at(0));
}

/// `_stricmp(value, "ANY") == 0`.
fn is_any(reader: &IniReader, index: u32) -> bool {
    eq_ignore_case(c_str(&reader.value_string_at(index)), b"ANY")
}

/// `IsNumericToken`: non-empty and starting with a digit or `-`.
pub(crate) fn is_numeric_token(text: &[u8]) -> bool {
    matches!(c_str(text).first(), Some(b'0'..=b'9' | b'-'))
}

/// The labels of `Cnd_NPCSystemEnter` and `Cnd_NPCSystemExit`: values 1 to
/// 0x31 while present, `Player` skipped when `skip_player`.
fn system_entries(reader: &IniReader, skip_player: bool) -> Vec<NpcSystemEntry> {
    let mut out = Vec::new();
    let mut slot = 1;
    while !reader.is_value_empty(slot) && slot < 0x32 {
        let label = reader.value_string_at(slot);
        if !(skip_player && eq_ignore_case(c_str(&label), b"Player")) {
            out.push(NpcSystemEntry {
                label: tstring(&label, 0x30),
                system_id: 0,
            });
        }
        slot += 1;
    }
    out
}

/// The condition of factory entry `index` ([`FACTORY`]), made with its
/// constructor's values and parsed from the rest of the line
/// (`Cnd::Parse`). `Cnd_Timer` with a range draws one `rand()` from the
/// world's generator.
#[must_use]
pub fn parse(cx: &mut Cx<'_>, reader: &mut IniReader, index: usize) -> Condition {
    match index {
        0 => {
            let mut c = BaseEnter::default();
            base_condition(cx, reader, &mut c.base_id);
            Condition::BaseEnter(c)
        }
        1 => {
            let mut c = BaseExit::default();
            base_condition(cx, reader, &mut c.base_id);
            Condition::BaseExit(c)
        }
        2 => {
            let mut c = CargoScanned::default();
            if !reader.is_value_empty(0) && !reader.is_value_empty(1) {
                c.scanner_label = value_tstring(reader, 0, 0x30);
                c.scanned_label = value_tstring(reader, 1, 0x30);
            }
            Condition::CargoScanned(c)
        }
        3 => {
            let mut c = CharSelect::default();
            character_condition(
                cx,
                reader,
                &mut c.character,
                &mut c.base_id,
                &mut c.location_id,
            );
            Condition::CharSelect(c)
        }
        4 => {
            let mut c = CommComplete {
                comm_id: nickname_id(&reader.value_string_at(0)),
                ..CommComplete::default()
            };
            if !reader.is_value_empty(1) {
                c.source_label = value_tstring(reader, 1, 0x30);
            }
            Condition::CommComplete(c)
        }
        5 => {
            let mut c = CmpToPlane::default();
            let name = reader.value_string_at(0);
            if eq_ignore_case(b"inside", c_str(&name)) {
                c.side = 0;
            } else if eq_ignore_case(b"outside", c_str(&name)) {
                c.side = 1;
            }
            c.object_label = value_tstring(reader, 1, 0x30);
            c.plane_a = f32_at(reader, 2);
            c.plane_b = f32_at(reader, 3);
            c.plane_c = f32_at(reader, 4);
            c.plane_d = f32_at(reader, 5);
            Condition::CmpToPlane(c)
        }
        6 => {
            let mut c = Destroyed {
                label: create_id(&reader.value_string_at(0)),
                ..Destroyed::default()
            };
            if !reader.is_value_empty(1) && reader.value_int(1) >= -1 {
                c.needed = reader.value_int(1);
            }
            if !reader.is_value_empty(2) {
                let mode = reader.value_string_at(2);
                let mode = c_str(&mode);
                if eq_ignore_case(mode, b"EXPLODE") {
                    c.mode = 1;
                } else if eq_ignore_case(mode, b"SILENT") {
                    c.mode = 0;
                } else if eq_ignore_case(mode, b"ALL") {
                    c.mode = 0x3e7;
                } else if eq_ignore_case(mode, b"ALL_IGNORE_LANDING") {
                    c.mode = 0x3e6;
                }
            }
            if !reader.is_value_empty(3) {
                c.killer_label = value_tstring(reader, 3, 0x30);
            }
            Condition::Destroyed(c)
        }
        7 => Condition::DistCircle(DistCircle {
            object_label: value_tstring(reader, 0, 0x30),
            circle_obj: value_tstring(reader, 1, 0x30),
            ..DistCircle::default()
        }),
        8 => {
            let mut c = DistShip::default();
            side(reader, 0, &mut c.inside);
            c.object_a = value_tstring(reader, 1, 0x30);
            c.object_b = value_tstring(reader, 2, 0x30);
            c.distance_sq = square_if_greater(f32_at(reader, 3));
            if !reader.is_value_empty(4) && f32_at(reader, 4) > 0.0 {
                c.tick_interval = f32_at(reader, 4);
            }
            tick_mode(reader, 5, &mut c.tick_mode);
            Condition::DistShip(c)
        }
        9 => {
            let mut c = DistVec::default();
            side(reader, 0, &mut c.inside);
            c.object_label = value_tstring(reader, 1, 0x30);
            c.position = Vector::new(f32_at(reader, 2), f32_at(reader, 3), f32_at(reader, 4));
            c.distance_sq = square_unless_less_or_equal(f32_at(reader, 5));
            if !reader.is_value_empty(6) && f32_at(reader, 6) >= 0.0 {
                c.tick_interval = f32_at(reader, 6);
            }
            tick_mode(reader, 7, &mut c.tick_mode);
            Condition::DistVec(c)
        }
        10 => {
            let mut c = DistVecLbl::default();
            side(reader, 0, &mut c.inside);
            c.any = eq_ignore_case(c_str(&reader.value_string_at(1)), b"any");
            c.label = create_id(&reader.value_string_at(2));
            c.position = Vector::new(f32_at(reader, 3), f32_at(reader, 4), f32_at(reader, 5));
            c.distance_sq = square_unless_less_or_equal(f32_at(reader, 6));
            if !reader.is_value_empty(7) && f32_at(reader, 7) >= 0.0 {
                c.tick_interval = f32_at(reader, 7);
            }
            Condition::DistVecLbl(c)
        }
        11 => Condition::EncLaunched(EncLaunched {
            encounter_id: create_id(&reader.value_string_at(0)),
        }),
        12 => {
            let mut c = HasMsn::default();
            let answer = value_tstring(reader, 0, 0x40);
            if answer == b"yes" {
                c.wanted = 0;
            } else if answer == b"no" {
                c.wanted = 1;
            }
            Condition::HasMsn(c)
        }
        13 => {
            let mut c = HealthDec {
                object_label: value_tstring(reader, 0, 0x30),
                health: f32_at(reader, 1),
                ..HealthDec::default()
            };
            if !reader.is_value_empty(2) {
                c.attacker_label = value_tstring(reader, 2, 0x30);
            }
            Condition::HealthDec(c)
        }
        14 => Condition::InSpace(InSpace {
            in_space: reader.value_bool(0),
        }),
        15 => {
            let in_lane = reader.value_bool(0);
            Condition::InTradelane(InTradelane {
                object_label: value_tstring(reader, 1, 0x30),
                in_lane,
            })
        }
        16 => {
            let mut c = InZone {
                in_zone: reader.value_bool(0),
                ..InZone::default()
            };
            c.object_label = value_tstring(reader, 1, 0x30);
            if !reader.is_value_empty(2) {
                let kind = reader.value_string_at(2);
                let kind = c_str(&kind);
                if eq_ignore_case(b"OBJECT_ZONE", kind) {
                    c.zone_flags = 0x7fc0;
                } else if eq_ignore_case(b"EXCLUSION_ZONE", kind) {
                    c.zone_flags = 0x3_0000;
                } else if eq_ignore_case(b"NEBULA_ZONE", kind) {
                    c.zone_flags = 0x8000;
                } else if eq_ignore_case(b"DUST_ZONE", kind) {
                    c.zone_flags = 0x20_0000;
                }
            }
            Condition::InZone(c)
        }
        17 => {
            let jumpgate = value_tstring(reader, 0, 0x30);
            let gate_id = nickname_id(&jumpgate);
            Condition::JumpgateAct(JumpgateAct { jumpgate, gate_id })
        }
        18 => {
            let mut c = JumpInComplete::default();
            if !is_any(reader, 0) {
                system_id_into(cx, &mut c.system_id, &reader.value_string_at(0));
            }
            Condition::JumpInComplete(c)
        }
        19 => {
            let mut c = LaunchComplete {
                ship_label: value_tstring(reader, 0, 0x30),
                ..LaunchComplete::default()
            };
            if !reader.is_value_empty(1) {
                c.base_nickname = value_tstring(reader, 1, 0x30);
                c.base_id = nickname_id(&c.base_nickname);
            }
            Condition::LaunchComplete(c)
        }
        20 => {
            let mut c = LocEnter::default();
            location_condition(cx, reader, &mut c.location_id, &mut c.base_id);
            Condition::LocEnter(c)
        }
        21 => {
            let mut c = LocExit::default();
            location_condition(cx, reader, &mut c.location_id, &mut c.base_id);
            Condition::LocExit(c)
        }
        22 => {
            let mut c = LootAcquired {
                loot_label: create_id(&reader.value_string_at(0)),
                ship_label: value_tstring(reader, 1, 0x30),
                ..LootAcquired::default()
            };
            if !reader.is_value_empty(2) {
                c.needed = reader.value_int(2);
            }
            Condition::LootAcquired(c)
        }
        23 => {
            let mut c = MsnResponse::default();
            let answer = reader.value_string_at(0);
            if eq_ignore_case(c_str(&answer), b"accept") {
                c.response = 0;
            } else if eq_ignore_case(c_str(&answer), b"reject") {
                c.response = 1;
            }
            Condition::MsnResponse(c)
        }
        24 => {
            let mut c = NpcSystemEnter::default();
            if is_any(reader, 0) {
                c.any_system = true;
            } else {
                system_id_into(cx, &mut c.system_id, &reader.value_string_at(0));
            }
            c.ships = system_entries(reader, true);
            c.ships.push(NpcSystemEntry {
                label: b"Player".to_vec(),
                system_id: 0,
            });
            Condition::NpcSystemEnter(c)
        }
        25 => {
            let mut c = NpcSystemExit::default();
            if is_any(reader, 0) {
                c.any_system = true;
            } else {
                system_id_into(cx, &mut c.system_id, &reader.value_string_at(0));
            }
            c.ships = system_entries(reader, false);
            Condition::NpcSystemExit(c)
        }
        26 => Condition::PlayerLaunch(PlayerLaunch {}),
        27 => {
            let mut c = PlayerManeuver {
                behavior_id: game_sim::ai::state_graph::behavior_id(c_str(
                    &reader.value_string_at(0),
                )),
                ..PlayerManeuver::default()
            };
            if !reader.is_value_empty(1) {
                c.target_label = value_tstring(reader, 1, 0x30);
                c.inverse = c
                    .target_label
                    .windows(b"inverse".len())
                    .any(|w| w == b"inverse");
            }
            Condition::PlayerManeuver(c)
        }
        28 => {
            let mut c = PopUpDialog::default();
            let button = value_tstring(reader, 0, 0x40);
            if eq_ignore_case(&button, b"YES") {
                c.button_mask = 1;
            } else if eq_ignore_case(&button, b"NO") {
                c.button_mask = 2;
            } else if eq_ignore_case(&button, b"LATER") {
                c.button_mask = 4;
            } else if eq_ignore_case(&button, b"CLOSE") {
                c.button_mask = 8;
            }
            Condition::PopUpDialog(c)
        }
        29 => {
            let mut c = ProjHit {
                target_label: value_tstring(reader, 0, 0x30),
                needed: reader.value_int(1),
                ..ProjHit::default()
            };
            if !reader.is_value_empty(2) {
                c.shooter_label = value_tstring(reader, 2, 0x30);
            }
            Condition::ProjHit(c)
        }
        30 => {
            let mut c = ProjHitShipToLbl {
                target_label: create_id(&reader.value_string_at(0)),
                needed: reader.value_int(1),
                ..ProjHitShipToLbl::default()
            };
            if !reader.is_value_empty(2) {
                c.shooter_label = value_tstring(reader, 2, 0x30);
            }
            Condition::ProjHitShipToLbl(c)
        }
        31 => Condition::RtcDone(RtcDone {
            rtc_file: value_tstring(reader, 0, 0x80),
        }),
        32 => {
            let mut c = RumorHeard::default();
            character_condition(cx, reader, &mut c.rumor, &mut c.base_id, &mut c.location_id);
            Condition::RumorHeard(c)
        }
        33 => {
            let mut c = SpaceEnter::default();
            space_condition(cx, reader, &mut c.system_id);
            Condition::SpaceEnter(c)
        }
        34 => {
            let mut c = SpaceExit::default();
            space_condition(cx, reader, &mut c.system_id);
            Condition::SpaceExit(c)
        }
        35 => {
            let mut c = SystemEnter::default();
            if !is_any(reader, 0) {
                system_id_into(cx, &mut c.system_id, &reader.value_string_at(0));
            }
            if !reader.is_value_empty(1) && !is_any(reader, 1) {
                system_id_into(cx, &mut c.from_system_id, &reader.value_string_at(1));
            }
            Condition::SystemEnter(c)
        }
        36 => {
            let mut c = SystemExit::default();
            if !is_any(reader, 0) {
                system_id_into(cx, &mut c.system_id, &reader.value_string_at(0));
            }
            if !reader.is_value_empty(1) && !is_any(reader, 1) {
                system_id_into(cx, &mut c.to_system_id, &reader.value_string_at(1));
            }
            Condition::SystemExit(c)
        }
        37 => {
            let mut c = TetherBroke {
                ship_a: value_tstring(reader, 0, 0x30),
                ship_b: value_tstring(reader, 1, 0x30),
                ..TetherBroke::default()
            };
            c.distance_sq = square_if_greater(f32_at(reader, 2));
            c.field_0x8c = f32_at(reader, 3);
            if !reader.is_value_empty(4) {
                c.field_0x84 = square_unless_less(f32_at(reader, 4));
            }
            Condition::TetherBroke(c)
        }
        38 => {
            let mut c = Timer {
                duration: f32_at(reader, 0),
                ..Timer::default()
            };
            if !reader.is_value_empty(1) {
                let upper = f32_at(reader, 1);
                let lower = c.duration;
                // `float(rand()) * GRandUnitScale`, stored as a float; then
                // `(Upper - Lower) * RandUnit + Lower` in the x87's 53 bits,
                // stored as a float (T11).
                let unit = cx.world.rng().rand() as f32 * RAND_UNIT_SCALE;
                c.duration = ((f64::from(upper) - f64::from(lower)) * f64::from(unit)
                    + f64::from(lower)) as f32;
            }
            if !reader.is_value_empty(2) {
                let mode = reader.value_string_at(2);
                if eq_ignore_case(c_str(&mode), b"RUN_ON_BASE") {
                    c.run_on_base = true;
                } else if eq_ignore_case(c_str(&mode), b"RUN_IN_SPACE_ONLY") {
                    c.run_on_base = false;
                }
            }
            Condition::Timer(c)
        }
        39 => {
            let ring_a = value_tstring(reader, 1, 0x30);
            let ring_b = value_tstring(reader, 2, 0x30);
            Condition::TlEntered(TlEntered {
                ship_label: value_tstring(reader, 0, 0x30),
                ring_a_id: nickname_id(&ring_a),
                ring_b_id: nickname_id(&ring_b),
                ring_a,
                ring_b,
                ship_id: 0,
            })
        }
        40 => {
            let mut c = TlExited {
                ship_label: value_tstring(reader, 0, 0x30),
                ring_a: value_tstring(reader, 1, 0x30),
                ..TlExited::default()
            };
            if !eq_ignore_case(&c.ring_a, b"ANY") {
                c.ring_a_id = nickname_id(&c.ring_a);
            }
            if !reader.is_value_empty(2) {
                c.ring_b = value_tstring(reader, 2, 0x30);
                c.ring_b_id = nickname_id(&c.ring_b);
            }
            Condition::TlExited(c)
        }
        41 => Condition::True(True {}),
        42 => Condition::WatchTrigger(WatchTrigger {
            watched_trigger_id: create_id(&reader.value_string_at(0)),
            wanted_state: create_id(&reader.value_string_at(1)),
        }),
        _ => {
            let mut c = WatchVibe {
                object_a: value_tstring(reader, 0, 0x30),
                object_b: value_tstring(reader, 1, 0x30),
                ..WatchVibe::default()
            };
            c.value = if is_numeric_token(&reader.value_string_at(2)) {
                f32_at(reader, 2)
            } else {
                rep_value_from_name(&value_tstring(reader, 2, 0x40))
            };
            if !reader.is_value_empty(3) {
                let compare = reader.value_string_at(3);
                let compare = c_str(&compare);
                if eq_ignore_case(compare, b"lt") {
                    c.compare = 0;
                } else if eq_ignore_case(compare, b"gt") {
                    c.compare = 1;
                } else if eq_ignore_case(compare, b"eq") {
                    c.compare = 2;
                }
            }
            Condition::WatchVibe(c)
        }
    }
}
