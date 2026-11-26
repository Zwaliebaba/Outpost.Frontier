//! The synthetic messages of the protocol suites. `oracle_protocol*.cpp`
//! fills the C++ packets from the same generator in the same order; each
//! function here names the C++ members it mirrors where they differ.
//!
//! Values that the oracle cannot reproduce are fixed: no archetype is
//! loaded in the harness, so every small archetype ID is 0 and every
//! maximum of hit points 1.

use game_sim::protocol::common::*;
use game_sim::protocol::costume::{Costume, CreateCostume};
use game_sim::protocol::records::{
    CollisionGroup, CreateCollisionGroup, CreateDamageEntry, CreateEquip, CreateHardpoint,
    CreateObject, DamageEntry, DamageList, EquipRecord,
};
use game_sim::protocol::to_client::*;
use game_sim::protocol::to_server::*;
use game_sim::protocol::{
    Body, ClientMessage, CommonMessage, Direction, ServerMessage, WireContext, quantise_orientation,
};
use neuron_core::net::wire::quantise::{
    quantise_fraction, quantise_hull, quantise_tenths, quantise_unit,
};

use super::Gen;

/// The three C++ modes: `SetIsMPServer(true)`; multiplayer client; single
/// player.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// A multiplayer server (`mps`).
    Server,
    /// A multiplayer client (`mpc`).
    Client,
    /// Single player (`sp`).
    Single,
}

impl Mode {
    /// The name in the vector lines.
    pub fn name(self) -> &'static str {
        match self {
            Self::Server => "mps",
            Self::Client => "mpc",
            Self::Single => "sp",
        }
    }

    /// The context a channel's messages use in this mode.
    pub fn context(self, channel: u8) -> WireContext {
        match (self, channel) {
            (Self::Server, _) => WireContext::SERVER,
            (Self::Client, _) => WireContext::CLIENT,
            (Self::Single, 2) => WireContext::SINGLE_PLAYER_SERVER,
            (Self::Single, _) => WireContext::SINGLE_PLAYER_CLIENT,
        }
    }
}

/// One suite entry: a packet, its modes and its number of cases.
pub struct Entry {
    /// The packet ID.
    pub id: u32,
    /// The C++ name.
    pub name: &'static str,
    /// The modes, in line order.
    pub modes: &'static [Mode],
    /// Cases per mode.
    pub cases: u32,
}

const fn entry<B: Body>(modes: &'static [Mode], cases: u32) -> Entry {
    Entry {
        id: B::ID,
        name: B::NAME,
        modes,
        cases,
    }
}

const MPC: &[Mode] = &[Mode::Client];
const MPS: &[Mode] = &[Mode::Server];
const MPS_SP: &[Mode] = &[Mode::Server, Mode::Single];

/// Channel 1, in the order of the suite.
pub const COMMON: &[Entry] = &[
    entry::<UpdateObject>(MPC, 2),
    entry::<FireWeapon>(&[Mode::Client, Mode::Single], 3),
    entry::<SetTarget>(MPC, 2),
    entry::<ChatMsg>(MPC, 2),
    entry::<ActivateEquip>(MPC, 2),
    entry::<ActivateCruise>(MPC, 2),
    entry::<GoTradeLane>(MPC, 2),
    entry::<StopTradeLane>(MPC, 2),
    entry::<SetWeaponGroup>(MPC, 2),
    entry::<PlayerTrade>(&[Mode::Server, Mode::Client], 9),
    entry::<SetVisitedState>(MPC, 2),
    entry::<JettisonCargo>(MPC, 2),
    entry::<ActivateThrusters>(MPC, 2),
    entry::<RequestBestPath>(MPC, 2),
    entry::<RequestGroupPositions>(MPC, 2),
    entry::<RequestPlayerStats>(MPC, 2),
    entry::<SetMissionLog>(MPC, 2),
    entry::<RequestRankLevel>(MPC, 2),
    entry::<PopUpDialog>(MPC, 2),
    entry::<SetInterfaceState>(MPC, 2),
    entry::<TractorObjects>(MPC, 2),
];

/// The actions of the `PLAYER_TRADE` cases.
pub const TRADE_ACTIONS: [u8; 9] = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x03];

fn byte_block(g: &mut Gen, case: u32, length: usize) -> Vec<u8> {
    g.bytes(if case == 0 { 0 } else { length })
}

/// A `FIREWEAPON` target component of case `case`.
fn fire_value(g: &mut Gen, case: u32) -> f32 {
    match case {
        0 => g.unit() * 32.0,
        1 => (i32::try_from(g.u32() % 16001).unwrap_or(0) - 8000) as f32,
        _ => g.f32(),
    }
}

/// The channel 1 message of `id`, case `case`, in `mode`.
pub fn common(id: u32, case: u32, mode: Mode, g: &mut Gen) -> CommonMessage {
    let ctx = mode.context(1);
    match id {
        UpdateObject::ID => {
            let flags = g.u8();
            let object_id = g.u32();
            let position = g.vector();
            let orientation = quantise_orientation(g.unit_quaternion());
            let extra = quantise_unit(g.unit());
            let update_time = g.f32();
            UpdateObject {
                flags,
                object_id,
                position,
                orientation,
                extra,
                update_time,
            }
            .into()
        }
        FireWeapon::ID => {
            let object_id = g.u32();
            let x = fire_value(g, case);
            let y = fire_value(g, case);
            let z = fire_value(g, case);
            let count = [1, 0, 255][case as usize];
            let hardpoints = g.words(count);
            let target = if ctx.single_player {
                [x, y, z].map(FireComponent::Full)
            } else {
                FireWeapon::target_from([x, y, z])
            };
            FireWeapon {
                object_id,
                target,
                hardpoints,
            }
            .into()
        }
        SetTarget::ID => SetTarget {
            object_id: g.u32(),
            target_id: g.u32(),
            sub_obj_id: g.u16(),
            target_sub_obj_id: g.u16(),
        }
        .into(),
        ChatMsg::ID => {
            let data = byte_block(g, case, 5);
            ChatMsg {
                data,
                target_id: g.u32(),
                field_04: g.u32(),
            }
            .into()
        }
        ActivateEquip::ID => ActivateEquip {
            object_id: g.u32(),
            equip_id: g.u16(),
            active: g.u8(),
        }
        .into(),
        ActivateCruise::ID => ActivateCruise {
            object_id: g.u32(),
            active: g.u8(),
            field_05: g.u8(),
        }
        .into(),
        GoTradeLane::ID => GoTradeLane {
            object_id: g.u32(),
            tradelane_ring1: g.u32(),
            tradelane_ring2: g.u32(),
            field_0c: g.u8(),
        }
        .into(),
        StopTradeLane::ID => StopTradeLane {
            ship_id: g.u32(),
            tradelane_ring1: g.u32(),
            tradelane_ring2: g.u32(),
        }
        .into(),
        SetWeaponGroup::ID => SetWeaponGroup {
            data: byte_block(g, case, 7),
        }
        .into(),
        SetVisitedState::ID => SetVisitedState {
            data: byte_block(g, case, 7),
        }
        .into(),
        RequestBestPath::ID => RequestBestPath {
            data: byte_block(g, case, 7),
        }
        .into(),
        RequestGroupPositions::ID => RequestGroupPositions {
            data: byte_block(g, case, 7),
        }
        .into(),
        RequestPlayerStats::ID => RequestPlayerStats {
            data: byte_block(g, case, 7),
        }
        .into(),
        SetMissionLog::ID => SetMissionLog {
            data: byte_block(g, case, 7),
        }
        .into(),
        RequestRankLevel::ID => RequestRankLevel {
            data: byte_block(g, case, 7),
        }
        .into(),
        SetInterfaceState::ID => SetInterfaceState {
            data: byte_block(g, case, 7),
        }
        .into(),
        PlayerTrade::ID => {
            let action = TRADE_ACTIONS[case as usize];
            let mut trade = PlayerTrade {
                action,
                ..PlayerTrade::default()
            };
            match action {
                0x02 => trade.reason = i8::from_le_bytes([g.u8()]),
                0x04 => trade.money = g.i32(),
                0x08 => {
                    trade.equip_arch_id = g.u32();
                    trade.equip_count = g.i32();
                    trade.equip_health = g.f32();
                    trade.equip_id = g.u16();
                }
                0x10 => {
                    trade.equip_arch_id = g.u32();
                    trade.equip_count = g.i32();
                    trade.equip_id = g.u16();
                }
                0x40 => trade.accepted = g.u8() & 1 != 0,
                _ => {}
            }
            if matches!(action, 0x01 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 | 0x80) {
                let target = g.u32();
                let counted =
                    ctx.direction == Direction::ToClient || matches!(action, 0x01 | 0x20 | 0x80);
                trade.target_player = if counted { target } else { 0 };
            }
            trade.into()
        }
        JettisonCargo::ID => JettisonCargo {
            object_id: g.u32(),
            item_id: g.u16(),
            count: g.u32(),
        }
        .into(),
        ActivateThrusters::ID => ActivateThrusters {
            object_id: g.u32(),
            active: g.u8(),
        }
        .into(),
        PopUpDialog::ID => {
            let data_a = byte_block(g, case, 3);
            let data_b = byte_block(g, case, 3);
            PopUpDialog {
                data_a,
                data_b,
                field_10: g.u32(),
            }
            .into()
        }
        TractorObjects::ID => {
            let object_id = g.u32();
            let tractor_equip_id = g.u16();
            let target_ids = g.dwords(if case == 0 { 0 } else { 300 });
            TractorObjects {
                object_id,
                tractor_equip_id,
                target_ids,
            }
            .into()
        }
        _ => panic!("no channel 1 message {id:#x}"),
    }
}

/// Channel 3, in the order of the suite. `TRADERESPONSE` has no vector:
/// the C++ encoder writes a pointer (divergence D107).
pub const CLIENT: &[Entry] = &[
    entry::<Login>(MPC, 2),
    entry::<MunCollision>(MPC, 2),
    entry::<RequestLaunch>(MPC, 2),
    entry::<RequestCharInfo>(MPC, 1),
    entry::<SelectCharacter>(MPC, 2),
    entry::<EnterBase>(MPC, 2),
    entry::<RequestBaseInfo>(MPC, 2),
    entry::<RequestLocationInfo>(MPC, 2),
    entry::<GfRequestShipInfo>(MPC, 1),
    entry::<SystemSwitchOutComplete>(MPC, 2),
    entry::<ObjCollision>(MPC, 2),
    entry::<ExitBase>(MPC, 2),
    entry::<EnterLocation>(MPC, 2),
    entry::<ExitLocation>(MPC, 2),
    entry::<RequestCreateShip>(MPC, 1),
    entry::<GfGoodSell>(MPC, 2),
    entry::<GfGoodBuy>(MPC, 2),
    entry::<GfSelectObject>(MPC, 2),
    entry::<MissionResponse>(MPC, 2),
    entry::<ReqShipArch>(MPC, 2),
    entry::<ReqEquipment>(MPC, 2),
    entry::<ReqCargo>(MPC, 2),
    entry::<ReqAddItem>(MPC, 2),
    entry::<ReqRemoveItem>(MPC, 2),
    entry::<ReqModifyItem>(MPC, 2),
    entry::<ReqSetCash>(MPC, 2),
    entry::<ReqChangeCash>(MPC, 2),
    entry::<SaveGame>(MPC, 2),
    entry::<MineAsteroid>(MPC, 2),
    entry::<DbgCreateShip>(MPC, 2),
    entry::<DbgLoadSystem>(MPC, 2),
    entry::<Dock>(MPC, 2),
    entry::<DbgDestroyObject>(MPC, 2),
    entry::<CargoScan>(MPC, 2),
    entry::<DbgConsole>(MPC, 2),
    entry::<DbgFreeSystem>(MPC, 1),
    entry::<SetManeuver>(MPC, 2),
    entry::<DbgRelocateShip>(MPC, 2),
    entry::<RequestEvent>(MPC, 2),
    entry::<RequestCancel>(MPC, 2),
    entry::<InterfaceItemUsed>(MPC, 2),
    entry::<ReqCollisionGroups>(MPC, 2),
    entry::<CommComplete>(MPC, 2),
    entry::<RequestNewCharInfo>(MPC, 1),
    entry::<CreateNewChar>(MPC, 2),
    entry::<DestroyChar>(MPC, 2),
    entry::<ReqHullStatus>(MPC, 2),
    entry::<GfGoodVaporized>(MPC, 2),
    entry::<BadlandsObjCollision>(MPC, 2),
    entry::<LaunchComplete>(MPC, 2),
    entry::<Hail>(MPC, 2),
    entry::<RequestUseItem>(MPC, 2),
    entry::<AbortMission>(MPC, 2),
    entry::<SkipAutosave>(MPC, 1),
    entry::<JumpInComplete>(MPC, 2),
    entry::<ReqInvincibility>(MPC, 2),
    entry::<MissionSaveB>(MPC, 2),
    entry::<ReqDifficultyScale>(MPC, 2),
    entry::<RtcDone>(MPC, 2),
];

/// The hardpoint names of the equipment records, by record index; `None`
/// for the internal cargo bay.
pub const HARDPOINTS: [Option<&str>; 4] = [
    None,
    Some("HpWeapon01"),
    Some("HpTurret_U1_03"),
    Some("HpCustom7"),
];

/// Equipment record `index` of an equipment list.
fn equip_record(g: &mut Gen, index: usize) -> EquipRecord {
    let count = g.i32();
    let health = g.f32();
    let arch_id = g.u32();
    let id = g.u16();
    let equipped = g.u8();
    let field_0f = g.u8();
    let hardpoint = HARDPOINTS[index % 4]
        .map(|name| {
            let mut bytes = name.as_bytes().to_vec();
            bytes.push(0);
            bytes
        })
        .unwrap_or_default();
    EquipRecord {
        count,
        health,
        arch_id,
        id,
        equipped,
        field_0f,
        hardpoint,
    }
}

fn equip_records(g: &mut Gen, count: usize) -> Vec<EquipRecord> {
    (0..count).map(|index| equip_record(g, index)).collect()
}

fn collision_groups(g: &mut Gen, count: usize) -> Vec<CollisionGroup> {
    (0..count)
        .map(|_| {
            let id = g.u16();
            CollisionGroup {
                id,
                health: g.f32(),
            }
        })
        .collect()
}

fn text_units(g: &mut Gen, count: usize) -> Vec<u16> {
    (0..count).map(|_| g.text_unit()).collect()
}

fn text_bytes(g: &mut Gen, count: usize) -> Vec<u8> {
    (0..count).map(|_| g.text_byte()).collect()
}

/// The channel 3 message of `id`, case `case`.
pub fn client(id: u32, case: u32, g: &mut Gen) -> ClientMessage {
    let long = case != 0;
    match id {
        Login::ID => {
            let create_flag = g.u8();
            let mut account_name = text_units(g, if long { 5 } else { 0 });
            if long {
                account_name.push(0x00c0);
            }
            let second = text_units(g, if long { 3 } else { 0 });
            Login {
                create_flag,
                account_name,
                second,
            }
            .into()
        }
        MunCollision::ID => MunCollision {
            field_00: g.u32(),
            field_04: g.u32(),
            object_id: g.u32(),
            field_0c: g.u16(),
            hit_position: g.vector(),
        }
        .into(),
        RequestLaunch::ID => RequestLaunch { value: g.u32() }.into(),
        EnterBase::ID => EnterBase { value: g.u32() }.into(),
        SystemSwitchOutComplete::ID => SystemSwitchOutComplete { value: g.u32() }.into(),
        ExitBase::ID => ExitBase { value: g.u32() }.into(),
        EnterLocation::ID => EnterLocation { value: g.u32() }.into(),
        ExitLocation::ID => ExitLocation { value: g.u32() }.into(),
        GfSelectObject::ID => GfSelectObject { value: g.u32() }.into(),
        ReqShipArch::ID => ReqShipArch { value: g.u32() }.into(),
        ReqSetCash::ID => ReqSetCash { value: g.i32() }.into(),
        ReqChangeCash::ID => ReqChangeCash { value: g.i32() }.into(),
        Dock::ID => Dock { value: g.u32() }.into(),
        InterfaceItemUsed::ID => InterfaceItemUsed { value: g.u32() }.into(),
        ReqHullStatus::ID => ReqHullStatus { value: g.f32() }.into(),
        AbortMission::ID => AbortMission { value: g.u32() }.into(),
        MissionSaveB::ID => MissionSaveB { value: g.u32() }.into(),
        ReqDifficultyScale::ID => ReqDifficultyScale { value: g.f32() }.into(),
        RtcDone::ID => RtcDone { value: g.u32() }.into(),
        RequestCharInfo::ID => RequestCharInfo {}.into(),
        GfRequestShipInfo::ID => GfRequestShipInfo {}.into(),
        RequestCreateShip::ID => RequestCreateShip {}.into(),
        DbgFreeSystem::ID => DbgFreeSystem {}.into(),
        RequestNewCharInfo::ID => RequestNewCharInfo {}.into(),
        SkipAutosave::ID => SkipAutosave {}.into(),
        SelectCharacter::ID => SelectCharacter {
            character_id: text_bytes(g, if long { MAX_CHARACTER_ID } else { 0 }),
        }
        .into(),
        DestroyChar::ID => DestroyChar {
            character_id: text_bytes(g, if long { 9 } else { 0 }),
        }
        .into(),
        RequestBaseInfo::ID => RequestBaseInfo {
            id: g.u32(),
            full: g.u8(),
        }
        .into(),
        RequestLocationInfo::ID => RequestLocationInfo {
            id: g.u32(),
            full: g.u8(),
        }
        .into(),
        ObjCollision::ID => ObjCollision {
            object_id: g.u32(),
            sub_obj_id: g.u16(),
            target_object_id: g.u32(),
            target_sub_obj_id: g.u16(),
            field_10: g.f32(),
        }
        .into(),
        GfGoodSell::ID => GfGoodSell {
            field_00: g.u32(),
            field_04: g.u32(),
            field_08: g.u32(),
        }
        .into(),
        GfGoodBuy::ID => GfGoodBuy {
            field_00: g.u32(),
            field_04: g.u32(),
            field_08: g.u32(),
            field_0c: g.u32(),
        }
        .into(),
        MissionResponse::ID => MissionResponse {
            field_00: g.u32(),
            mission_id: g.u32(),
            accepted: g.u8(),
        }
        .into(),
        ReqEquipment::ID => ReqEquipment {
            records: equip_records(g, if long { 4 } else { 0 }),
        }
        .into(),
        ReqCargo::ID => ReqCargo {
            records: equip_records(g, if long { 4 } else { 0 }),
        }
        .into(),
        ReqAddItem::ID => {
            let arch_id = g.u32();
            let count = g.i32();
            let health = g.f32();
            let mounted = g.u8();
            let hardpoint = if long {
                b"HpTurret01\0".to_vec()
            } else {
                Vec::new()
            };
            ReqAddItem {
                arch_id,
                count,
                health,
                mounted,
                hardpoint,
            }
            .into()
        }
        ReqModifyItem::ID => {
            let equip_id = g.u32();
            let count = g.i32();
            let health = g.f32();
            let mounted = g.u8();
            let hardpoint = if long {
                b"HpWeapon03\0".to_vec()
            } else {
                Vec::new()
            };
            ReqModifyItem {
                equip_id,
                count,
                health,
                mounted,
                hardpoint,
            }
            .into()
        }
        ReqRemoveItem::ID => ReqRemoveItem {
            equip_id: g.u32(),
            count: g.i32(),
        }
        .into(),
        SaveGame::ID => SaveGame {
            character_id: Vec::new(),
            description: g.words(if long { 9 } else { 0 }),
        }
        .into(),
        MineAsteroid::ID => MineAsteroid {
            system_id: g.u32(),
            position: g.vector(),
            loot_id: g.u32(),
            secondary_loot: g.u32(),
            quantity: g.u32(),
        }
        .into(),
        DbgCreateShip::ID => {
            let mut fields = [0; 5];
            for field in &mut fields {
                *field = g.u32();
            }
            let vector_a = g.vector();
            let vector_b = g.vector();
            let orientation = g.quaternion();
            let field_3c = g.u32();
            let wide = g.words(if long { 3 } else { 0 });
            let data_a = g.bytes(if long { 2 } else { 0 });
            let data_b = g.bytes(if long { 5 } else { 0 });
            let mut tail = [0; 4];
            for field in &mut tail {
                *field = g.u32();
            }
            let array = g.dwords(if long { 2 } else { 0 });
            DbgCreateShip {
                fields,
                vector_a,
                vector_b,
                orientation,
                field_3c,
                wide,
                data_a,
                data_b,
                tail,
                array,
            }
            .into()
        }
        DbgLoadSystem::ID => DbgLoadSystem {
            data: g.bytes(if long { 6 } else { 0 }),
        }
        .into(),
        DbgDestroyObject::ID => DbgDestroyObject {
            field_00: g.u32(),
            field_04: g.u16(),
        }
        .into(),
        CargoScan::ID => CargoScan {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        DbgConsole::ID => {
            let field_00 = g.u32();
            let mut command = [0; 128];
            for byte in &mut command {
                *byte = g.u8();
            }
            DbgConsole { field_00, command }.into()
        }
        SetManeuver::ID => SetManeuver {
            field_00: g.u32(),
            object_id: g.u32(),
            field_08: g.u32(),
        }
        .into(),
        DbgRelocateShip::ID => DbgRelocateShip {
            field_00: g.u32(),
            position: g.vector(),
        }
        .into(),
        RequestEvent::ID => RequestEvent {
            event_type: g.u8(),
            arg1: g.u32(),
            arg2: g.u32(),
            arg3: g.u32(),
            field_10: g.u8(),
        }
        .into(),
        RequestCancel::ID => RequestCancel {
            event_type: g.u8(),
            arg1: g.u32(),
            arg2: g.u32(),
            field_0c: g.u8(),
        }
        .into(),
        ReqCollisionGroups::ID => ReqCollisionGroups {
            groups: collision_groups(g, if long { 3 } else { 0 }),
        }
        .into(),
        CommComplete::ID => CommComplete {
            field_00: g.u32(),
            field_04: g.u32(),
            field_08: g.u32(),
            field_0c: g.u32(),
        }
        .into(),
        CreateNewChar::ID => {
            let name = text_units(g, if long { 23 } else { 0 });
            CreateNewChar {
                name,
                faction: g.u32(),
                base: g.u32(),
                package: g.u32(),
                pilot: g.u32(),
            }
            .into()
        }
        GfGoodVaporized::ID => GfGoodVaporized {
            field_00: g.u32(),
            field_04: g.u32(),
            field_08: g.u32(),
        }
        .into(),
        BadlandsObjCollision::ID => BadlandsObjCollision {
            object_id: g.u32(),
            field_08: g.u16(),
            field_00: g.u32(),
        }
        .into(),
        LaunchComplete::ID => LaunchComplete {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        Hail::ID => Hail {
            field_00: g.u32(),
            field_04: g.u32(),
            field_08: g.u32(),
        }
        .into(),
        RequestUseItem::ID => RequestUseItem {
            object_id: g.u32(),
            item_id: g.u16(),
            count: g.u16(),
        }
        .into(),
        JumpInComplete::ID => JumpInComplete {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        ReqInvincibility::ID => ReqInvincibility {
            ship_id: g.u32(),
            reason: g.u8(),
            invincible: g.u8(),
        }
        .into(),
        TradeResponse::ID => TradeResponse {
            data: g.bytes(if long { 3 } else { 0 }),
        }
        .into(),
        _ => panic!("no channel 3 message {id:#x}"),
    }
}

/// Channel 2, in the order of the suite.
pub const SERVER: &[Entry] = &[
    entry::<ConnectResponse>(MPS, 2),
    entry::<LoginResponse>(MPS, 2),
    entry::<CharacterInfo>(MPS, 3),
    entry::<CreateShip>(MPS_SP, 3),
    entry::<DamageObject>(MPS, 2),
    entry::<DestroyObject>(MPS, 2),
    entry::<Launch>(MPS, 2),
    entry::<CharSelectVerified>(MPS, 2),
    entry::<ActivateObject>(MPS, 2),
    entry::<Land>(MPS, 2),
    entry::<SetStartRoom>(MPS, 2),
    entry::<GfCompleteNewsBroadcastList>(MPS, 2),
    entry::<GfCompleteCharList>(MPS, 2),
    entry::<GfCompleteMissionComputerList>(MPS, 2),
    entry::<GfCompleteScriptBehaviorList>(MPS, 2),
    entry::<GfCompleteAmbientScriptList>(MPS, 2),
    entry::<GfDestroyNewsBroadcast>(MPS, 2),
    entry::<GfDestroyCharacter>(MPS, 2),
    entry::<GfDestroyMissionComputer>(MPS, 2),
    entry::<GfDestroyScriptBehavior>(MPS, 2),
    entry::<GfDestroyAmbientScript>(MPS, 2),
    entry::<GfScriptBehavior>(MPS, 2),
    entry::<GfUpdateMissionComputer>(MPS, 2),
    entry::<GfUpdateNewsBroadcast>(MPS, 2),
    entry::<GfUpdateAmbientScript>(MPS, 2),
    entry::<GfMissionVendorAcceptance>(MPS, 2),
    entry::<SystemSwitchOut>(MPS, 2),
    entry::<SystemSwitchIn>(MPS, 2),
    entry::<SetShipArch>(MPS, 2),
    entry::<SetEquipment>(MPS, 2),
    entry::<SetCargo>(MPS, 2),
    entry::<GfUpdateChar>(MPS, 2),
    entry::<RequestCreateShipResp>(MPS, 2),
    entry::<CreateLoot>(MPS_SP, 2),
    entry::<SetReputation>(MPS_SP, 3),
    entry::<AdjustAttitude>(MPS, 2),
    entry::<SetGroupFeelings>(MPS, 2),
    entry::<CreateMine>(MPS_SP, 2),
    entry::<CreateCounter>(MPS, 2),
    entry::<SetAddItem>(MPS, 2),
    entry::<SetRemoveItem>(MPS, 2),
    entry::<SetCash>(MPS, 2),
    entry::<ExplodeAsteroidMine>(MPS, 2),
    entry::<RequestSpaceScript>(MPS, 2),
    entry::<SetMissionObjectiveState>(MPS, 2),
    entry::<ReplaceMissionObjective>(MPS, 2),
    entry::<SetMissionObjectives>(MPS, 2),
    entry::<CreateGuided>(MPS, 2),
    entry::<ItemTractored>(MPS, 2),
    entry::<ScanNotify>(MPS, 2),
    entry::<RepairObject>(MPS, 2),
    entry::<RemoteObjectCargoUpdate>(MPS, 2),
    entry::<SetNumKills>(MPS, 2),
    entry::<SetMissionSuccesses>(MPS, 2),
    entry::<SetMissionFailures>(MPS, 2),
    entry::<BurnFuse>(MPS, 2),
    entry::<CreateSolar>(MPS_SP, 2),
    entry::<SetStoryCue>(MPS, 2),
    entry::<RequestReturned>(MPS, 2),
    entry::<SetMissionMessage>(MPS, 2),
    entry::<MarkObj>(MPS, 2),
    entry::<CfgInterfaceNotification>(MPS, 2),
    entry::<SetCollisionGroups>(MPS, 2),
    entry::<SetHullStatus>(MPS, 2),
    entry::<SetGuidedTarget>(MPS, 2),
    entry::<SetCamera>(MPS, 2),
    entry::<RevertCamera>(MPS, 1),
    entry::<LoadHint>(MPS, 2),
    entry::<SetDirective>(MPS, 2),
    entry::<SendComm>(MPS, 3),
    entry::<UseItem>(MPS, 2),
    entry::<PlayerList>(MPS, 2),
    entry::<FormationUpdate>(MPS, 2),
    entry::<MiscObjUpdate>(MPS, 3),
    entry::<ObjectCargoUpdate>(MPS, 2),
    entry::<SendNnMessage>(MPS, 2),
    entry::<SetMusic>(MPS, 2),
    entry::<CancelMusic>(MPS, 1),
    entry::<PlaySoundEffect>(MPS, 2),
    entry::<GfMissionVendorWhyEmpty>(MPS, 2),
    entry::<MissionSaveA>(MPS, 2),
];

/// A costume: four parts, `count` accessories.
fn costume(g: &mut Gen, count: usize) -> Costume {
    let mut parts = [0; 4];
    for part in &mut parts {
        *part = g.u32();
    }
    Costume {
        parts,
        accessories: g.dwords(count),
    }
}

/// The equipment of the create-object block, record `index`:
/// `CEqObjCreateInfo`'s record as the C++ fills `FRpcServerEquipRecord`.
fn create_equip(g: &mut Gen, index: usize) -> CreateEquip {
    let equipped = g.u8() & 1;
    let field_0f = g.u8() & 1;
    // Count: 1, a byte, a dword above 255, by index.
    let count = match index % 3 {
        0 => 1,
        1 => i32::try_from(g.u32() % 256).unwrap_or(0),
        _ => 256 + i32::try_from(g.u32() % 0x1000).unwrap_or(0),
    };
    // Health: exactly 1.0 for even records.
    let health = if index.is_multiple_of(2) {
        1.0
    } else {
        g.unit()
    };
    let _arch_id = g.u32();
    let id = if index.is_multiple_of(2) {
        g.u16() & 0xff
    } else {
        g.u16() | 0x100
    };
    let hardpoint = HARDPOINTS[index % 4].map_or(CreateHardpoint::None, |name| {
        CreateHardpoint::from_name(name.as_bytes())
    });
    CreateEquip {
        equipped: equipped != 0,
        field_0f: field_0f != 0,
        count,
        health: CreateEquip::health_byte(health),
        archetype: 0,
        id,
        hardpoint,
    }
}

/// The create-object block with `count` of each list.
fn create_object(g: &mut Gen, count: usize, single_player: bool) -> CreateObject {
    let obj_id = g.u32();
    let _archetype = g.u32();
    let docking_base_id = g.u32();
    let vibe_group_id = g.u32();
    let body = g.u32();
    let head = g.u32();
    let accessories = g.dwords(count);
    let voice_id = g.u32();
    let position = g.vector();
    let orientation = quantise_orientation(g.unit_quaternion());
    let hit_points = g.i32();
    let field_6c = g.u32();
    let equipment = (0..count).map(|index| create_equip(g, index)).collect();
    let collision_groups = (0..count)
        .map(|_| {
            let id = g.u8();
            CreateCollisionGroup {
                id,
                health: quantise_fraction(g.unit()),
            }
        })
        .collect();
    let damage: Vec<CreateDamageEntry> = (0..count)
        .map(|_| {
            let sub_obj_id = g.u16();
            let fate = g.u32();
            CreateDamageEntry {
                sub_obj_id,
                fate,
                hit_points: g.f32(),
            }
        })
        .collect();
    CreateObject {
        obj_id,
        archetype: 0,
        docking_base_id,
        vibe_group_id,
        costume: CreateCostume {
            body,
            head,
            accessories,
        },
        voice_id,
        position,
        orientation,
        hull: quantise_hull(hit_points, 1),
        field_6c: if single_player { field_6c } else { 0 },
        equipment,
        collision_groups,
        damage: if single_player { damage } else { Vec::new() },
    }
}

/// The channel 2 message of `id`, case `case`, in `mode`.
pub fn server(id: u32, case: u32, mode: Mode, g: &mut Gen) -> ServerMessage {
    let single_player = mode == Mode::Single;
    let long = case != 0;
    match id {
        ConnectResponse::ID => ConnectResponse { value: g.u32() }.into(),
        LoginResponse::ID => LoginResponse { value: g.u8() }.into(),
        GfCompleteNewsBroadcastList::ID => GfCompleteNewsBroadcastList { value: g.u32() }.into(),
        GfCompleteCharList::ID => GfCompleteCharList { value: g.u32() }.into(),
        GfCompleteMissionComputerList::ID => {
            GfCompleteMissionComputerList { value: g.u32() }.into()
        }
        GfCompleteScriptBehaviorList::ID => GfCompleteScriptBehaviorList { value: g.u32() }.into(),
        GfCompleteAmbientScriptList::ID => GfCompleteAmbientScriptList { value: g.u32() }.into(),
        SetShipArch::ID => SetShipArch { value: g.u32() }.into(),
        SetCash::ID => SetCash { value: g.u32() }.into(),
        SetNumKills::ID => SetNumKills { value: g.u32() }.into(),
        SetMissionSuccesses::ID => SetMissionSuccesses { value: g.u32() }.into(),
        SetMissionFailures::ID => SetMissionFailures { value: g.u32() }.into(),
        SetStoryCue::ID => SetStoryCue { value: g.u32() }.into(),
        SetHullStatus::ID => SetHullStatus { value: g.f32() }.into(),
        SendNnMessage::ID => SendNnMessage { value: g.u32() }.into(),
        PlaySoundEffect::ID => PlaySoundEffect { value: g.u32() }.into(),
        GfMissionVendorWhyEmpty::ID => GfMissionVendorWhyEmpty { value: g.u8() }.into(),
        MissionSaveA::ID => MissionSaveA { value: g.u32() }.into(),
        RevertCamera::ID => RevertCamera {}.into(),
        CancelMusic::ID => CancelMusic {}.into(),
        SetEquipment::ID => SetEquipment {
            records: equip_records(g, if long { 4 } else { 0 }),
        }
        .into(),
        SetCargo::ID => SetCargo {
            records: equip_records(g, if long { 4 } else { 0 }),
        }
        .into(),
        GfScriptBehavior::ID => GfScriptBehavior {
            data: g.bytes(if long { 9 } else { 0 }),
        }
        .into(),
        GfUpdateMissionComputer::ID => GfUpdateMissionComputer {
            data: g.bytes(if long { 9 } else { 0 }),
        }
        .into(),
        GfUpdateNewsBroadcast::ID => GfUpdateNewsBroadcast {
            data: g.bytes(if long { 9 } else { 0 }),
        }
        .into(),
        GfUpdateAmbientScript::ID => GfUpdateAmbientScript {
            data: g.bytes(if long { 9 } else { 0 }),
        }
        .into(),
        GfMissionVendorAcceptance::ID => GfMissionVendorAcceptance {
            data: g.bytes(if long { 9 } else { 0 }),
        }
        .into(),
        GfUpdateChar::ID => GfUpdateChar {
            data: g.bytes(if long { 9 } else { 0 }),
        }
        .into(),
        SetStartRoom::ID => SetStartRoom {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        GfDestroyNewsBroadcast::ID => GfDestroyNewsBroadcast {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        GfDestroyCharacter::ID => GfDestroyCharacter {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        GfDestroyMissionComputer::ID => GfDestroyMissionComputer {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        GfDestroyScriptBehavior::ID => GfDestroyScriptBehavior {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        GfDestroyAmbientScript::ID => GfDestroyAmbientScript {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        SystemSwitchOut::ID => SystemSwitchOut {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        ScanNotify::ID => ScanNotify {
            field_00: g.u32(),
            field_04: g.u32(),
        }
        .into(),
        Land::ID => Land {
            field_00: g.u32(),
            field_04: g.u32(),
            field_08: g.u32(),
        }
        .into(),
        SetRemoveItem::ID => SetRemoveItem {
            field_00: g.u32(),
            field_04: g.u32(),
            field_08: g.u32(),
        }
        .into(),
        CharacterInfo::ID => {
            let count = [0, 1, 2][case as usize];
            let records = (0..count).map(|index| character_record(g, index)).collect();
            CharacterInfo {
                records,
                save_failure_count: g.u32(),
            }
            .into()
        }
        CreateShip::ID => {
            let count = [0, 1, 3][case as usize];
            let object = create_object(g, count, single_player);
            let using_tradelane = g.u8() & 1 != 0;
            let marked_by = g.u8() & 1 != 0;
            let velocity = g.vector();
            let throttle = quantise_unit(g.unit());
            let field_c0 = g.u16();
            let reputation_rank = g.u8();
            // Case 1 has an owner.
            let owner = (case == 1).then(|| {
                let player = g.u8() | 1;
                let field_c4 = g.u16();
                let display_name = text_units(g, 7);
                CreateShipOwner {
                    player,
                    field_c4,
                    display_name: if single_player {
                        Vec::new()
                    } else {
                        display_name
                    },
                }
            });
            let name_a = g.bytes(if case == 2 { 6 } else { 0 });
            let name_b = g.bytes(if case == 2 { 3 } else { 0 });
            let affiliation = g.u32();
            let attitude = quantise_unit(g.unit());
            let multiplayer_names = !single_player && owner.is_none();
            CreateShip {
                object,
                using_tradelane,
                marked_by,
                velocity,
                throttle,
                field_c0,
                reputation_rank,
                owner,
                names: if multiplayer_names {
                    (name_a, name_b)
                } else {
                    (Vec::new(), Vec::new())
                },
                affiliation: if single_player { 0 } else { affiliation },
                attitude: if single_player { 0 } else { attitude },
            }
            .into()
        }
        DamageObject::ID => {
            let object_id = g.u32();
            // `bDestroyed` is a C++ `bool`: 0 or 1.
            let destroyed = g.u8() & 1;
            let cause = g.u32();
            let inflictor_id = g.u32();
            let entries = (0..if long { 3 } else { 0 })
                .map(|_| {
                    let sub_obj_id = g.u16();
                    let fate = g.u8();
                    DamageEntry {
                        sub_obj_id,
                        fate,
                        hit_points: g.f32(),
                    }
                })
                .collect();
            DamageObject {
                object_id,
                damage: DamageList {
                    destroyed,
                    cause,
                    inflictor_id,
                    entries,
                },
            }
            .into()
        }
        DestroyObject::ID => DestroyObject {
            object_id: g.u32(),
            field_04: g.u8(),
        }
        .into(),
        Launch::ID => Launch {
            field_00: g.u32(),
            field_04: g.u32(),
            field_08: g.u32(),
            position: g.vector(),
            orientation: g.quaternion(),
        }
        .into(),
        CharSelectVerified::ID => {
            let rep_instance = g.u32();
            let low = g.u32();
            let high = g.u32() & 0x7fef_ffff;
            CharSelectVerified {
                rep_instance,
                time: f64::from_bits(u64::from(high) << 32 | u64::from(low)),
            }
            .into()
        }
        ActivateObject::ID => ActivateObject {
            object_id: g.u32(),
            activate: g.u8(),
            field_08: g.u32(),
        }
        .into(),
        SystemSwitchIn::ID => SystemSwitchIn {
            field_00: g.u32(),
            field_04: g.u32(),
            position: g.vector(),
            orientation: g.quaternion(),
        }
        .into(),
        RequestCreateShipResp::ID => RequestCreateShipResp {
            field_00: g.u8(),
            field_04: g.u32(),
        }
        .into(),
        CreateLoot::ID => {
            let owner_id = g.u32();
            let kind = g.u8();
            let loot = (0..if long { 2 } else { 0 })
                .map(|_| {
                    let loot_id = g.u32();
                    let _contents = g.u32();
                    let contents_hit_points = g.f32();
                    let units = g.u16();
                    let _container = g.u32();
                    let hit_points = g.f32();
                    let name_override = g.u32();
                    let position = g.vector();
                    let velocity = g.vector();
                    let orientation = g.quaternion();
                    let marked_by = g.u8();
                    let temporary = g.u8();
                    LootRecord {
                        loot_id,
                        contents: 0,
                        contents_hit_points,
                        units,
                        container: 0,
                        hit_points,
                        name_override,
                        position,
                        velocity: if single_player { velocity } else { [0.0; 3] },
                        orientation: if single_player {
                            orientation
                        } else {
                            [1.0, 0.0, 0.0, 0.0]
                        },
                        marked_by,
                        temporary,
                    }
                })
                .collect();
            CreateLoot {
                owner_id,
                kind,
                loot,
            }
            .into()
        }
        SetReputation::ID => {
            let target = g.u32();
            // Case 0: no affiliation, nothing else; 1: everything; 2: more
            // than 255 relations.
            let affiliation = if case == 0 { 0xffff_ffff } else { g.u32() };
            let display_name = if case == 1 {
                let mut name = text_units(g, 6);
                name.push(0);
                name
            } else {
                Vec::new()
            };
            let name_a = if case == 1 { g.bytes(8) } else { vec![0; 4] };
            let name_b = if case == 1 { g.bytes(5) } else { vec![0; 4] };
            let relations: Vec<Relation> = (0..[0, 3, 300][case as usize])
                .map(|_| {
                    let feeling = g.f32();
                    Relation {
                        feeling,
                        group_id: g.u32(),
                    }
                })
                .collect();
            let attitude = g.f32();
            let names = single_player && (name_a[..4] != [0; 4] || name_b[..4] != [0; 4]);
            SetReputation {
                target,
                affiliation,
                display_name: if single_player {
                    display_name
                } else {
                    Vec::new()
                },
                names: if names {
                    (name_a, name_b)
                } else {
                    (Vec::new(), Vec::new())
                },
                relations: if single_player { relations } else { Vec::new() },
                attitude: if single_player { 0.0 } else { attitude },
            }
            .into()
        }
        AdjustAttitude::ID => AdjustAttitude {
            field_00: g.u32(),
            field_04: g.f32(),
        }
        .into(),
        SetGroupFeelings::ID => {
            let name = if long {
                b"fc_x_grp\0".to_vec()
            } else {
                Vec::new()
            };
            let relations = (0..if long { 3 } else { 0 })
                .map(|_| {
                    let feeling = g.f32();
                    Relation {
                        feeling,
                        group_id: g.u32(),
                    }
                })
                .collect();
            SetGroupFeelings { name, relations }.into()
        }
        CreateMine::ID => {
            let obj_id = g.u32();
            let _archetype = g.u32();
            let owner_id = g.u32();
            let sub_obj_id = g.u16();
            let position = g.vector();
            let orientation = g.quaternion();
            let velocity = g.vector();
            let field_38 = g.vector();
            CreateMine {
                obj_id,
                archetype: 0,
                owner_id,
                sub_obj_id,
                position,
                orientation: if single_player {
                    orientation
                } else {
                    [1.0, 0.0, 0.0, 0.0]
                },
                velocity: if single_player { velocity } else { [0.0; 3] },
                field_38: if single_player { field_38 } else { [0.0; 3] },
            }
            .into()
        }
        CreateCounter::ID => CreateCounter {
            archetype: g.u32(),
            obj_id: g.u32(),
            field_08: g.u32(),
            owner_id: g.u32(),
            sub_obj_id: g.u16(),
            orientation: g.quaternion(),
            position: g.vector(),
            velocity: g.vector(),
            field_3c: g.vector(),
        }
        .into(),
        SetAddItem::ID => {
            let arch_id = g.u32();
            let item_id = g.u32();
            let count = g.i32();
            let health = g.f32();
            let equipped = g.u8();
            let mission = g.u8();
            let field_0c = g.u32();
            SetAddItem {
                arch_id,
                id: item_id,
                count,
                health,
                equipped,
                mission,
                field_0c,
                hardpoint: if long {
                    b"HpShield01\0".to_vec()
                } else {
                    Vec::new()
                },
            }
            .into()
        }
        ExplodeAsteroidMine::ID => ExplodeAsteroidMine {
            field_00: g.u32(),
            position: g.vector(),
        }
        .into(),
        RequestSpaceScript::ID => RequestSpaceScript {
            field_00: g.u32(),
            name: if long {
                b"scripts/x.thn\0".to_vec()
            } else {
                Vec::new()
            },
        }
        .into(),
        SetMissionObjectiveState::ID => SetMissionObjectiveState {
            field_00: g.u32(),
            field_04: g.u32(),
            field_08: g.u16(),
        }
        .into(),
        ReplaceMissionObjective::ID => {
            let field_00 = g.u32();
            let state = g.u32();
            ReplaceMissionObjective {
                field_00,
                state,
                text: g.bytes(if long { 13 } else { 0 }),
            }
            .into()
        }
        SetMissionObjectives::ID => {
            let field_00 = g.u32();
            let state = g.u32();
            let text_a = g.bytes(if long { 7 } else { 0 });
            let text_b = g.bytes(if long { 2 } else { 0 });
            let objectives = (0..if long { 3 } else { 0 })
                .map(|index| {
                    let state = g.u8();
                    MissionObjective {
                        state,
                        text: g.bytes(index * 3),
                    }
                })
                .collect();
            SetMissionObjectives {
                field_00,
                state,
                text_a,
                text_b,
                objectives,
            }
            .into()
        }
        CreateGuided::ID => {
            let _archetype = g.u32();
            let owner_id = g.u32();
            let sub_obj_id = g.u16();
            let target_id = g.u32();
            let target_sub_obj_id = g.u16();
            let obj_id = g.u32();
            let position = g.vector();
            let orientation = quantise_orientation(g.unit_quaternion());
            CreateGuided {
                archetype: 0,
                owner_id,
                sub_obj_id,
                target_id,
                target_sub_obj_id,
                obj_id,
                position,
                orientation,
            }
            .into()
        }
        ItemTractored::ID => ItemTractored {
            loot_id: g.u32(),
            ship_id: g.u32(),
            tractor_id: g.u16(),
            units: g.u16(),
        }
        .into(),
        RepairObject::ID => RepairObject {
            object_id: g.u32(),
            equip_id: g.u32(),
            points: g.i32(),
        }
        .into(),
        RemoteObjectCargoUpdate::ID => {
            let object_id = g.u32();
            let flag = g.u8();
            RemoteObjectCargoUpdate {
                object_id,
                flag,
                records: equip_records(g, if long { 4 } else { 0 }),
            }
            .into()
        }
        BurnFuse::ID => BurnFuse {
            source_id: g.u32(),
            object_id: g.u32(),
            fuse_id: g.u32(),
            sub_obj_id: g.u16(),
            at_t: g.f32(),
            lifetime: g.f32(),
            lit: g.u8(),
        }
        .into(),
        CreateSolar::ID => {
            let count = if long { 2 } else { 0 };
            let object = create_object(g, count, single_player);
            let field_9c = g.u8();
            let marked_by = g.u8();
            let affiliation = g.u32();
            let attitude = quantise_unit(g.unit());
            let name_a = g.bytes(if long { 6 } else { 0 });
            let name_b = g.bytes(if long { 3 } else { 0 });
            if single_player {
                CreateSolar {
                    object,
                    field_9c,
                    marked_by,
                    ..CreateSolar::default()
                }
                .into()
            } else {
                CreateSolar {
                    object,
                    field_9c,
                    marked_by,
                    affiliation,
                    attitude,
                    names: (name_a, name_b),
                }
                .into()
            }
        }
        RequestReturned::ID => RequestReturned {
            object_id: g.u32(),
            request_type: g.u8(),
            response: g.u8(),
            field_26: g.u8(),
        }
        .into(),
        SetMissionMessage::ID => {
            let kind = g.u32();
            SetMissionMessage {
                kind,
                text: g.bytes(if long { 11 } else { 0 }),
            }
            .into()
        }
        MarkObj::ID => MarkObj {
            object_id: g.u32(),
            mark_type: g.u8(),
        }
        .into(),
        CfgInterfaceNotification::ID => CfgInterfaceNotification {
            notif_type: g.u32(),
            enable: g.u8(),
            param: g.u32(),
        }
        .into(),
        SetCollisionGroups::ID => SetCollisionGroups {
            groups: collision_groups(g, if long { 3 } else { 0 }),
        }
        .into(),
        SetGuidedTarget::ID => SetGuidedTarget {
            field_20: g.u32(),
            field_24: g.u32(),
            field_28: g.u16(),
        }
        .into(),
        SetCamera::ID => SetCamera {
            position: g.vector(),
            orientation: g.quaternion(),
            fov: g.f32(),
            aspect: g.f32(),
        }
        .into(),
        LoadHint::ID => {
            let kind = g.u32();
            LoadHint {
                kind,
                data: g.bytes(if long { 10 } else { 0 }),
            }
            .into()
        }
        SetDirective::ID => SetDirective {
            directive: directive(g),
        }
        .into(),
        SendComm::ID => {
            let field_20 = g.u32();
            let field_24 = g.u32();
            let field_28 = g.u32();
            // Case 0: no costume; 1: a costume with three accessories; 2:
            // eight accessories and 127 text IDs.
            let costume_flag = if case == 0 { 0 } else { 1 };
            let costume = if case == 0 {
                Costume::default()
            } else {
                let full = costume(g, [0, 3, 8][case as usize]);
                Costume {
                    parts: [full.parts[0], full.parts[1], 0, 0],
                    accessories: full.accessories,
                }
            };
            let field_60 = g.u32();
            let text_ids = g.dwords([0, 2, 127][case as usize]);
            let mood = g.u16();
            let duration = quantise_tenths(g.unit() * 8.0 + 2.0);
            SendComm {
                field_20,
                field_24,
                field_28,
                costume_flag,
                costume,
                field_60,
                text_ids,
                mood,
                duration,
            }
            .into()
        }
        UseItem::ID => UseItem {
            object_id: g.u32(),
            equip_id: g.u16(),
            count: g.u16(),
        }
        .into(),
        PlayerList::ID => {
            let kind = g.u32();
            let player_id = g.u32();
            let field_2c = g.u8();
            let name = if long {
                let mut name = text_units(g, 5);
                name.push(0);
                name
            } else {
                Vec::new()
            };
            PlayerList {
                kind,
                player_id,
                field_2c,
                name,
            }
            .into()
        }
        FormationUpdate::ID => FormationUpdate {
            leader_id: g.u32(),
            offset: g.vector(),
        }
        .into(),
        MiscObjUpdate::ID => {
            // Case 0: no field; 1: every field; 2: the text and the IDs.
            let flags = [0x0000, 0x01ff, 0x0110][case as usize];
            let mut message = MiscObjUpdate {
                flags,
                ..MiscObjUpdate::default()
            };
            if flags & 0x08 != 0 {
                message.field_28 = g.u32();
            }
            if flags & 0x04 != 0 {
                message.field_24 = g.u32();
            }
            if flags & 0x01 != 0 {
                message.field_34 = g.u32();
            }
            if flags & 0x02 != 0 {
                message.field_38 = g.u32();
                message.field_3c = g.u32();
            }
            if flags & 0x10 != 0 {
                message.text = text_units(g, if case == 1 { 4 } else { 9 });
            }
            if flags & 0x20 != 0 {
                message.field_54 = g.u32();
            }
            if flags & 0x40 != 0 {
                message.field_2c = g.u16();
            }
            if flags & 0x80 != 0 {
                message.field_30 = g.u32();
            }
            if flags & 0x100 != 0 {
                message.ids = g.dwords(if case == 1 { 2 } else { 255 });
            }
            message.into()
        }
        ObjectCargoUpdate::ID => ObjectCargoUpdate {
            object_id: g.u32(),
            arch_id: g.u32(),
            equip_id: g.u16(),
            count: g.u16(),
            status: g.f32(),
            mission: g.u8(),
        }
        .into(),
        SetMusic::ID => SetMusic {
            field_20: g.u32(),
            field_24: g.u32(),
            field_28: g.u32(),
            field_2c: g.u32(),
            field_30: g.u8(),
            field_34: g.f32(),
        }
        .into(),
        _ => panic!("no channel 2 message {id:#x}"),
    }
}

/// A character record of `CHARACTERINFO`.
fn character_record(g: &mut Gen, index: usize) -> CharacterRecord {
    let character_id = text_bytes(g, 11);
    let is_restart = g.u8();
    let is_auto_save = g.u8();
    let name = text_units(g, 5 + index);
    let description = text_units(g, index * 2);
    let mut dwords = [0; 11];
    for value in &mut dwords {
        *value = g.u32();
    }
    let hull_status = g.f32();
    let num_kills = g.u32();
    let num_mission_failures = g.u32();
    let num_mission_successes = g.u32();
    let com_costume = (index == 0).then(|| costume(g, 2));
    let base_costume = (index == 0).then(|| costume(g, 0));
    let equipment = equip_records(g, index * 3);
    let collision_groups = collision_groups(g, index + 1);
    let [
        descrip_str_id,
        timestamp_high,
        timestamp_low,
        ship_archetype,
        money,
        system_id,
        base_id,
        location_id,
        voice_id,
        rank,
        field_24c,
    ] = dwords;
    CharacterRecord {
        character_id,
        is_restart,
        is_auto_save,
        name,
        description,
        descrip_str_id,
        timestamp_high,
        timestamp_low,
        ship_archetype,
        money,
        system_id,
        base_id,
        location_id,
        voice_id,
        rank,
        field_24c,
        hull_status,
        num_kills,
        num_mission_failures,
        num_mission_successes,
        com_costume,
        base_costume,
        equipment,
        collision_groups,
    }
}

/// A directive filled field by field in offset order.
fn directive(g: &mut Gen) -> Directive {
    let field_00 = g.u32();
    let field_04 = g.u32();
    let field_08 = g.u8();
    let field_0c = g.f32();
    let field_10 = g.u32();
    let field_14 = g.i32();
    let field_18 = g.i32();
    let field_1c = g.u32();
    let field_20 = g.f32();
    let field_24 = g.f32();
    let field_28 = g.u32();
    let field_2c = g.f32();
    let field_30 = g.vector();
    let field_3c = g.f32();
    let field_40 = g.i32();
    let field_44 = g.vector();
    let field_50 = g.i32();
    let mut blocks = [[0; 3]; 4];
    for block in &mut blocks {
        for word in block.iter_mut() {
            *word = g.u32();
        }
    }
    let field_84 = g.f32();
    let field_88 = g.f32();
    let mut tail = [0; 5];
    for byte in &mut tail {
        *byte = g.u8();
    }
    Directive {
        field_00,
        field_04,
        field_08,
        field_0c,
        field_10,
        field_14,
        field_18,
        field_1c,
        field_20,
        field_24,
        field_28,
        field_2c,
        field_30,
        field_3c,
        field_40,
        field_44,
        field_50,
        blocks,
        field_84,
        field_88,
        tail,
    }
}
