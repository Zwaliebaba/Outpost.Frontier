//! The controller messages (work package P3.2; decision record 0006,
//! point 6): the 81 ids of `pub::Controller::EMessage`
//! (`include/common_server_controller.h`) as one typed enum. Server.dll
//! sends 63 of them (`0x00..0x41`) and Content 17 (`0x1000..0x1011`);
//! `NpcSystemEnter` (`0x100f`) is never sent and only serves as a
//! mission-trigger event.
//!
//! Each variant carries what its senders store, field for field (the P3.2
//! survey of every sender; the header's comment is wrong for
//! `LootAcquired`, whose sub-object id comes before the good, and for
//! `FormationLeaderChanged`, which carries ships, not ids). Fields a
//! handler may change are the out-parameters the sender reads back after
//! the dispatch: `EquipDropLoot::desc`, `ObjectDestroyed::dropped_equip` and
//! `DeathFuseLit::lifetime`; `FreeTerminalRequest`'s answer, which the C++
//! passes through a global (`pub::ReportFreeTerminal`), is its
//! `reported` field. Resources a message lends are borrows. Truncated
//! pointers become keys. Fields whose meaning the C++ does not show keep a
//! name after their position.

use std::io::Write;

use game_sim::data::equip_desc::EquipDesc;
use game_sim::object::damage::DamageCause;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;

use neuron_server::controller::ControllerId;

/// A Content NPC ship (`FContentNpcShip`, `FContentShipBase`): the key the
/// content side gives it (P3.6).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct NpcShipKey(pub u32);

/// A server phantom (`ServerPhantom*`, carried as a dword by
/// `PhantomEnter` and `PhantomExit`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PhantomKey(pub u32);

/// `CommResult` (`include/common_server_types.h:334`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CommResult {
    /// 0.
    Success,
    /// 1.
    Failed,
    /// 2.
    Cancelled,
    /// 3.
    Timeout,
    /// Any other value, as sent.
    Other(u32),
}

impl CommResult {
    /// The value from its dword.
    #[must_use]
    pub const fn from_u32(value: u32) -> Self {
        match value {
            0 => Self::Success,
            1 => Self::Failed,
            2 => Self::Cancelled,
            3 => Self::Timeout,
            other => Self::Other(other),
        }
    }

    /// The dword.
    #[must_use]
    pub const fn value(self) -> u32 {
        match self {
            Self::Success => 0,
            Self::Failed => 1,
            Self::Cancelled => 2,
            Self::Timeout => 3,
            Self::Other(v) => v,
        }
    }
}

/// `FObjectDestroyedEvent` (`include/server_gameobject.h:187`).
#[derive(Clone, Debug, PartialEq)]
pub struct ObjectDestroyed {
    /// The object.
    pub id: u32,
    /// The killer (destroy's second argument).
    pub killer: u32,
    /// The system.
    pub system: u32,
    /// The owning player (an equipped object's; 0 for loot and objects in
    /// transit).
    pub owner_player: u32,
    /// Destroy's kind.
    pub kind: i32,
    /// Where it was.
    pub position: Vector,
    /// The equipment it drops; Content may rewrite it before the sender
    /// drops what is left (an out-parameter).
    pub dropped_equip: Vec<EquipDesc>,
}

/// `FSubObjDamageEvent` and `FSolarHitEvent` (both 0x24 bytes,
/// `server_gameobject.h:205`, `server_gameobject.cpp:6258`); a solar's
/// root hit leaves the type fields and the damage 0 and the relative health
/// 1.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ObjectDamaged {
    /// The object.
    pub id: u32,
    /// The system.
    pub system: u32,
    /// The inflictor.
    pub inflictor: u32,
    /// The equipment's type (`0x40000` for the power, 0 for the root and
    /// collision groups).
    pub equip_type: u32,
    /// The collision group's type, or 0.
    pub collision_group_type: u32,
    /// The cause.
    pub cause: DamageCause,
    /// Hit points before minus after.
    pub damage: f32,
    /// The damage over the maximum.
    pub relative_damage: f32,
    /// The hit points left over the maximum.
    pub relative_health: f32,
}

/// `FEquipLootEvent` (`server_gameobject.h:225`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EquipDropLoot {
    /// The system.
    pub system: u32,
    /// The inflictor.
    pub inflictor: u32,
    /// The object the equipment leaves.
    pub object: u32,
    /// The cause.
    pub cause: DamageCause,
    /// The item; Content may rewrite it and the sender loots what is left
    /// (an out-parameter).
    pub desc: EquipDesc,
}

/// `FDeathFuseEvent` (`server_gameobject.h:236`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DeathFuseLit {
    /// The system.
    pub system: u32,
    /// The object.
    pub object: u32,
    /// Its type (`get_type`).
    pub object_type: u32,
    /// 0 when sent; Content writes the death conversation's length, which
    /// the sender reads back (an out-parameter).
    pub lifetime: f32,
}

/// One message to the controllers.
pub enum ControllerMessage<'a> {
    /// 0x00: a player's cash, cargo or equipment changed.
    PlayerWorthChanged {
        /// The player's client id.
        client: u32,
    },
    /// 0x01.
    BaseEnter {
        /// The base.
        base: u32,
        /// The player.
        player: u32,
    },
    /// 0x02.
    BaseExit {
        /// The base.
        base: u32,
        /// The player.
        player: u32,
    },
    /// 0x03 (`SPScanCargo`).
    CargoScanned {
        /// The scanning object.
        scanner: u32,
        /// The scanned one.
        target: u32,
    },
    /// 0x04: a character save is being written; controllers append their
    /// sections to the file.
    PlayerSave {
        /// The player's client id.
        client: u32,
        /// The open save.
        file: &'a mut dyn Write,
    },
    /// 0x05: a character file is being read (the server's character load,
    /// or Story to its mission).
    PlayerLoad {
        /// The player.
        player: u32,
        /// The reader over the file; handlers move it.
        reader: &'a mut IniReader,
    },
    /// 0x06.
    PlayerLaunch {
        /// The player.
        player: u32,
        /// The base left.
        base: u32,
    },
    /// 0x07.
    PlayerLogin {
        /// The player.
        player: u32,
    },
    /// 0x08.
    PlayerLogout {
        /// The player.
        player: u32,
    },
    /// 0x09: sent before the member leaves the group's set.
    GroupMemberLeft {
        /// The player.
        player: u32,
    },
    /// 0x0a: sent after the member joined.
    GroupMemberJoined {
        /// The player.
        player: u32,
    },
    /// 0x0b (`IServerImpl::CommComplete`; Content's conversation resend to
    /// SpaceSupport).
    CommComplete {
        /// +0x00: the listener.
        listener: u32,
        /// +0x04: the system.
        system: u32,
        /// +0x08: the speaker.
        speaker: u32,
        /// +0x0c: the comm line.
        line: u32,
        /// +0x10: the result.
        result: CommResult,
    },
    /// 0x0c: sent to every controller when one is destroyed.
    ControllerDestroyed {
        /// The destroyed controller.
        controller: ControllerId,
    },
    /// 0x0d.
    ObjectDamaged(ObjectDamaged),
    /// 0x10 (a ship's request cancelled, type 0).
    DockCancel {
        /// The ship.
        ship: u32,
        /// The dock target.
        target: u32,
        /// The cancel's third argument (meaning unknown).
        arg: u32,
    },
    /// 0x11.
    DockComplete {
        /// The ship.
        ship: u32,
        /// The dock.
        dock: u32,
        /// The system.
        system: u32,
    },
    /// 0x12 (a ship's request, type 0).
    DockRequest {
        /// The ship.
        ship: u32,
        /// The dock target.
        target: u32,
        /// The system.
        system: u32,
        /// The request's flag (`bFlag == 1`).
        flag: bool,
    },
    /// 0x13.
    EquipDropLoot(EquipDropLoot),
    /// 0x14 (a ship's request, type 2).
    TradelaneRequest {
        /// The ship.
        ship: u32,
        /// The ring.
        ring: u32,
        /// The far ring.
        end_ring: u32,
        /// The system.
        system: u32,
    },
    /// 0x15 (a ship's request cancelled, type 2).
    TradelaneRelease {
        /// The ship.
        ship: u32,
        /// The ring.
        ring: u32,
        /// The system.
        system: u32,
    },
    /// 0x16.
    TradelaneEnter {
        /// The ship.
        ship: u32,
        /// The entrance ring.
        entrance: u32,
        /// The exit ring.
        exit: u32,
        /// The system.
        system: u32,
    },
    /// 0x17.
    TradelaneExit {
        /// The ship.
        ship: u32,
        /// The entrance ring.
        entrance: u32,
        /// The exit ring.
        exit: u32,
        /// The system.
        system: u32,
    },
    /// 0x18: loot tractored into a ship.
    LootAcquired {
        /// The loot.
        loot: u32,
        /// The ship.
        ship: u32,
        /// The cargo's sub-object id (`add_cargo`'s answer).
        sub_object: u16,
        /// The good.
        good: u32,
        /// How many.
        count: i32,
    },
    /// 0x19 (a ship's request, type 1).
    FormationRequest {
        /// The ship.
        ship: u32,
        /// The leader.
        leader: u32,
        /// The system.
        system: u32,
    },
    /// 0x1a (a ship's request cancelled, type 1).
    FormationLeave {
        /// The ship.
        ship: u32,
        /// The leader.
        leader: u32,
        /// The system.
        system: u32,
    },
    /// 0x1b.
    GfObjSelect {
        /// The object.
        object: u32,
        /// The player.
        player: u32,
    },
    /// 0x1c: `SGFGoodBuyInfo`'s four dwords (meanings unproven) and the
    /// player.
    GfGoodBuy {
        /// The info's dwords.
        info: [u32; 4],
        /// The player.
        player: u32,
    },
    /// 0x1d: `SGFGoodSellInfo`'s three dwords and the player.
    GfGoodSell {
        /// The info's dwords.
        info: [u32; 3],
        /// The player.
        player: u32,
    },
    /// 0x1e: `SGFGoodVaporizedInfo`'s three dwords and the player.
    GfGoodVaporized {
        /// The info's dwords.
        info: [u32; 3],
        /// The player.
        player: u32,
    },
    /// 0x1f.
    Hail {
        /// The object.
        object: u32,
        /// The first argument (meaning unknown).
        arg1: u32,
        /// The second argument (meaning unknown).
        arg2: u32,
    },
    /// 0x20: whole heartbeat periods elapsed, delivered by the heartbeat
    /// walk.
    Heartbeat {
        /// The elapsed time.
        elapsed: f32,
    },
    /// 0x21.
    InterfaceItemUsed {
        /// The player.
        player: u32,
        /// The item.
        item: u32,
    },
    /// 0x22.
    RtcDone {
        /// The player.
        player: u32,
        /// The cut scene.
        rtc: u32,
    },
    /// 0x23.
    CargoJettisoned {
        /// The ship.
        ship: u32,
        /// The cargo's sub-object id.
        equip: u16,
        /// The good.
        good: u32,
        /// How many.
        count: i32,
    },
    /// 0x24.
    AbortMission {
        /// The player.
        player: u32,
        /// The mission.
        mission: u32,
    },
    /// 0x25: a ship left its system, through a gate
    /// (`SystemSwitchOutComplete`) or by dying (the dying-ship pump).
    Msg0x25 {
        /// The ship.
        ship: u32,
        /// The ship's death-sequence object (the gate it jumped through;
        /// for a dying ship it was zeroed when the ship was queued).
        death_sequence: u32,
        /// The system left.
        system: u32,
    },
    /// 0x26.
    JumpInComplete {
        /// The client's argument (meaning unknown).
        arg: u32,
        /// The ship.
        ship: u32,
    },
    /// 0x27: a ship of the dock queue lands or stops landing.
    Msg0x27 {
        /// The ship.
        ship: u32,
        /// The base.
        base: u32,
        /// The dock target, when it is not itself on a base.
        target: u32,
        /// Landing (Content cancels the docking).
        land: bool,
    },
    /// 0x28.
    LaunchComplete {
        /// The client's argument (the base or dock).
        id: u32,
        /// The ship.
        ship: u32,
    },
    /// 0x29.
    LocationEnter {
        /// The location.
        location: u32,
        /// The player.
        player: u32,
    },
    /// 0x2a.
    LocationExit {
        /// The location.
        location: u32,
        /// The player.
        player: u32,
    },
    /// 0x2b: one per loot spawned.
    LootSpawned {
        /// The loot.
        loot: u32,
        /// Its source (and owner).
        source: u32,
        /// The good.
        good: u32,
        /// How many.
        count: i32,
    },
    /// 0x2c (`IServerImpl::MissionResponse`; Content to a new mission).
    MissionResponse {
        /// +0x00: the offer (the server's first argument; Content's new
        /// controller id).
        offer: u32,
        /// +0x04: the mission (Content reads it as the kind: 2 vendor, 1
        /// bar character, 0 random).
        kind: u32,
        /// Accepted.
        accepted: bool,
        /// The player.
        player: u32,
    },
    /// 0x2d.
    PhantomEnter {
        /// The object that entered.
        object: u32,
        /// The phantom.
        phantom: PhantomKey,
        /// The system.
        system: u32,
    },
    /// 0x2e.
    PhantomExit {
        /// The object that left.
        object: u32,
        /// The phantom.
        phantom: PhantomKey,
        /// The system.
        system: u32,
    },
    /// 0x2f (single player only).
    PlayerManeuver {
        /// The player.
        player: u32,
        /// The object.
        object: u32,
        /// The maneuver (`XSetManeuver`'s third dword).
        value: u32,
    },
    /// 0x30.
    PopUpDialog {
        /// The player.
        player: u32,
        /// The button.
        button: u32,
    },
    /// 0x31: the player and nine dwords of the client's request (meanings
    /// unknown).
    RequestBestPath {
        /// The player.
        player: u32,
        /// The request's dwords at +0x00, +0x1c, +0x0c, +0x10, +0x14,
        /// +0x20, +0x24, +0x28 and +0x30.
        words: [u32; 9],
    },
    /// 0x32: a launch looks for a free terminal; SpaceSupport answers
    /// through `pub::ReportFreeTerminal`.
    FreeTerminalRequest {
        /// The player.
        player: u32,
        /// The dock target.
        target: u32,
        /// The reported terminal, -1 until a handler reports one (read back
        /// by the sender).
        reported: i32,
    },
    /// 0x33.
    RequestPlayerStats {
        /// The player.
        player: u32,
    },
    /// 0x35.
    PlayerShipArchSet {
        /// The player's client id.
        client: u32,
    },
    /// 0x36.
    SpaceEnter {
        /// The system.
        system: u32,
        /// The player.
        player: u32,
        /// The launch object (0 for none).
        launch_object: u32,
        /// The launch kind (-1 when none).
        launch_kind: i32,
    },
    /// 0x37: the player's ship was destroyed.
    SpaceExit {
        /// The system.
        system: u32,
        /// The player.
        player: u32,
    },
    /// 0x38.
    DeathFuseLit(DeathFuseLit),
    /// 0x39.
    ObjectDestroyed(ObjectDestroyed),
    /// 0x3a.
    SystemExit {
        /// The system left.
        system: u32,
        /// The destination system (0 for none).
        destination: u32,
        /// The player.
        player: u32,
    },
    /// 0x3b.
    SystemEnter {
        /// The system entered.
        system: u32,
        /// The previous system.
        from: u32,
        /// The player.
        player: u32,
        /// The arrival object (0 for none).
        arrival: u32,
        /// The arrival position.
        position: Vector,
    },
    /// 0x3c.
    SystemLoaded {
        /// The system.
        system: u32,
    },
    /// 0x3d.
    SystemUnloaded {
        /// The system.
        system: u32,
    },
    /// 0x3e: a timer of `pub::SetTimer`, to its controller.
    TimerExpired {
        /// The timer's first word.
        param1: u32,
        /// Its second word.
        param2: u32,
    },
    /// 0x3f (`IServerImpl::TradeResponse`; field meanings unproven).
    TradeResponse {
        /// +0x00.
        field_00: i32,
        /// +0x04.
        field_04: i32,
        /// +0x08.
        flag_08: bool,
        /// The goods offered: good, count, price and the fourth dword.
        offered: Vec<[u32; 4]>,
        /// The goods requested.
        requested: Vec<[u32; 4]>,
        /// The player.
        player: u32,
    },
    /// 0x40: the single-player state or the world file is being read.
    LoadGame {
        /// The reader; handlers read and reset it.
        reader: &'a mut IniReader,
    },
    /// 0x41: the single-player state is being written.
    SaveGame {
        /// The open file; handlers append to it.
        file: &'a mut dyn Write,
    },
    /// 0x1000: Story to its mission.
    MissionStart,
    /// 0x1001.
    CombatState {
        /// The state's type.
        state: u32,
        /// Its first parameter.
        param1: u32,
        /// Its second parameter.
        param2: u32,
        /// The system.
        system: u32,
    },
    /// 0x1002, to SpaceSupport.
    CombatDrift {
        /// The ship.
        ship: u32,
        /// The system.
        system: u32,
    },
    /// 0x1003, to SpaceSupport.
    NpcTurnedHostile {
        /// The object.
        object: u32,
        /// The other object.
        other: u32,
        /// The system.
        system: u32,
    },
    /// 0x1004, to SpaceSupport.
    FleeRequest {
        /// The ship.
        ship: u32,
        /// The reason.
        reason: u32,
        /// The system.
        system: u32,
    },
    /// 0x1005.
    DamageReputation {
        /// Provoked.
        provoked: bool,
        /// The attacker.
        attacker: u32,
        /// The victim (a solar's parent when it has one).
        victim: u32,
        /// The system.
        system: u32,
    },
    /// 0x1006: an NPC ship finished its comm lines, to its controller.
    Msg0x1006 {
        /// The ship.
        ship: u32,
        /// A dword of its first directive, by the directive's type.
        directive_word: u32,
        /// Always 1.
        one: u32,
    },
    /// 0x1007: a dock target became occupied.
    JumpgateAct {
        /// The dock target.
        target: u32,
    },
    /// 0x1008, to the ship's controller.
    LeadObject {
        /// The leading object.
        object: u32,
        /// The led object.
        other: u32,
        /// The progress.
        progress: u32,
        /// The value.
        value: f32,
        /// The position.
        position: Vector,
    },
    /// 0x1009.
    FormationLeaderChanged {
        /// The new leader.
        new_leader: NpcShipKey,
        /// The old leader.
        old_leader: NpcShipKey,
    },
    /// 0x100a, to the ship's controller.
    NpcSystemExit {
        /// The ship.
        ship: u32,
        /// The system left.
        system: u32,
    },
    /// 0x100b.
    PlayerMissionChanged {
        /// The mission controller (0 when leaving).
        mission: ControllerId,
        /// The player.
        player: u32,
    },
    /// 0x100c, to the mission's controller: the encounters launched.
    EncounterLaunched {
        /// Their nickname ids.
        encounters: &'a [u32],
    },
    /// 0x100d.
    RumorHeard {
        /// The player.
        player: u32,
        /// The role, at most 31 bytes.
        role: Vec<u8>,
    },
    /// 0x100e, to SpaceSupport.
    ShipInSights {
        /// The ship.
        ship: u32,
        /// The object.
        object: u32,
        /// The system.
        system: u32,
    },
    /// 0x100f: never sent; a mission-trigger event with two or three
    /// dwords (the NPC or player ship, the system, the arrival object).
    NpcSystemEnter {
        /// The ship.
        ship: u32,
        /// The system.
        system: u32,
        /// The arrival object, when given.
        object: Option<u32>,
    },
    /// 0x1010.
    NpcShipCreated {
        /// The new ship.
        ship: NpcShipKey,
    },
    /// 0x1011, to the ship's controller.
    TetherObject {
        /// The first object.
        object: u32,
        /// The second object.
        other: u32,
    },
}

impl std::fmt::Debug for ControllerMessage<'_> {
    /// The enumerator's name and id; the lent files and readers have no
    /// text form.
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}({:#x})", self.name(), self.id())
    }
}

impl ControllerMessage<'_> {
    /// The `pub::Controller::EMessage` value.
    #[must_use]
    pub fn id(&self) -> u32 {
        use ControllerMessage as M;
        match self {
            M::PlayerWorthChanged { .. } => 0x00,
            M::BaseEnter { .. } => 0x01,
            M::BaseExit { .. } => 0x02,
            M::CargoScanned { .. } => 0x03,
            M::PlayerSave { .. } => 0x04,
            M::PlayerLoad { .. } => 0x05,
            M::PlayerLaunch { .. } => 0x06,
            M::PlayerLogin { .. } => 0x07,
            M::PlayerLogout { .. } => 0x08,
            M::GroupMemberLeft { .. } => 0x09,
            M::GroupMemberJoined { .. } => 0x0A,
            M::CommComplete { .. } => 0x0B,
            M::ControllerDestroyed { .. } => 0x0C,
            M::ObjectDamaged(_) => 0x0D,
            M::DockCancel { .. } => 0x10,
            M::DockComplete { .. } => 0x11,
            M::DockRequest { .. } => 0x12,
            M::EquipDropLoot(_) => 0x13,
            M::TradelaneRequest { .. } => 0x14,
            M::TradelaneRelease { .. } => 0x15,
            M::TradelaneEnter { .. } => 0x16,
            M::TradelaneExit { .. } => 0x17,
            M::LootAcquired { .. } => 0x18,
            M::FormationRequest { .. } => 0x19,
            M::FormationLeave { .. } => 0x1A,
            M::GfObjSelect { .. } => 0x1B,
            M::GfGoodBuy { .. } => 0x1C,
            M::GfGoodSell { .. } => 0x1D,
            M::GfGoodVaporized { .. } => 0x1E,
            M::Hail { .. } => 0x1F,
            M::Heartbeat { .. } => 0x20,
            M::InterfaceItemUsed { .. } => 0x21,
            M::RtcDone { .. } => 0x22,
            M::CargoJettisoned { .. } => 0x23,
            M::AbortMission { .. } => 0x24,
            M::Msg0x25 { .. } => 0x25,
            M::JumpInComplete { .. } => 0x26,
            M::Msg0x27 { .. } => 0x27,
            M::LaunchComplete { .. } => 0x28,
            M::LocationEnter { .. } => 0x29,
            M::LocationExit { .. } => 0x2A,
            M::LootSpawned { .. } => 0x2B,
            M::MissionResponse { .. } => 0x2C,
            M::PhantomEnter { .. } => 0x2D,
            M::PhantomExit { .. } => 0x2E,
            M::PlayerManeuver { .. } => 0x2F,
            M::PopUpDialog { .. } => 0x30,
            M::RequestBestPath { .. } => 0x31,
            M::FreeTerminalRequest { .. } => 0x32,
            M::RequestPlayerStats { .. } => 0x33,
            M::PlayerShipArchSet { .. } => 0x35,
            M::SpaceEnter { .. } => 0x36,
            M::SpaceExit { .. } => 0x37,
            M::DeathFuseLit(_) => 0x38,
            M::ObjectDestroyed(_) => 0x39,
            M::SystemExit { .. } => 0x3A,
            M::SystemEnter { .. } => 0x3B,
            M::SystemLoaded { .. } => 0x3C,
            M::SystemUnloaded { .. } => 0x3D,
            M::TimerExpired { .. } => 0x3E,
            M::TradeResponse { .. } => 0x3F,
            M::LoadGame { .. } => 0x40,
            M::SaveGame { .. } => 0x41,
            M::MissionStart => 0x1000,
            M::CombatState { .. } => 0x1001,
            M::CombatDrift { .. } => 0x1002,
            M::NpcTurnedHostile { .. } => 0x1003,
            M::FleeRequest { .. } => 0x1004,
            M::DamageReputation { .. } => 0x1005,
            M::Msg0x1006 { .. } => 0x1006,
            M::JumpgateAct { .. } => 0x1007,
            M::LeadObject { .. } => 0x1008,
            M::FormationLeaderChanged { .. } => 0x1009,
            M::NpcSystemExit { .. } => 0x100A,
            M::PlayerMissionChanged { .. } => 0x100B,
            M::EncounterLaunched { .. } => 0x100C,
            M::RumorHeard { .. } => 0x100D,
            M::ShipInSights { .. } => 0x100E,
            M::NpcSystemEnter { .. } => 0x100F,
            M::NpcShipCreated { .. } => 0x1010,
            M::TetherObject { .. } => 0x1011,
        }
    }

    /// The name of the `EMessage` enumerator.
    #[must_use]
    pub fn name(&self) -> &'static str {
        NAMES
            .iter()
            .find(|(id, _)| *id == self.id())
            .map_or("?", |(_, name)| name)
    }
}

/// Every id and the name of its `EMessage` enumerator, in id order.
pub const NAMES: [(u32, &str); 81] = [
    (0x00, "PlayerWorthChanged"),
    (0x01, "BaseEnter"),
    (0x02, "BaseExit"),
    (0x03, "CargoScanned"),
    (0x04, "PlayerSave"),
    (0x05, "PlayerLoad"),
    (0x06, "PlayerLaunch"),
    (0x07, "PlayerLogin"),
    (0x08, "PlayerLogout"),
    (0x09, "GroupMemberLeft"),
    (0x0A, "GroupMemberJoined"),
    (0x0B, "CommComplete"),
    (0x0C, "ControllerDestroyed"),
    (0x0D, "ObjectDamaged"),
    (0x10, "DockCancel"),
    (0x11, "DockComplete"),
    (0x12, "DockRequest"),
    (0x13, "EquipDropLoot"),
    (0x14, "TradelaneRequest"),
    (0x15, "TradelaneRelease"),
    (0x16, "TradelaneEnter"),
    (0x17, "TradelaneExit"),
    (0x18, "LootAcquired"),
    (0x19, "FormationRequest"),
    (0x1A, "FormationLeave"),
    (0x1B, "GFObjSelect"),
    (0x1C, "GFGoodBuy"),
    (0x1D, "GFGoodSell"),
    (0x1E, "GFGoodVaporized"),
    (0x1F, "Hail"),
    (0x20, "Heartbeat"),
    (0x21, "InterfaceItemUsed"),
    (0x22, "RtcDone"),
    (0x23, "CargoJettisoned"),
    (0x24, "AbortMission"),
    (0x25, "Msg0x25"),
    (0x26, "JumpInComplete"),
    (0x27, "Msg0x27"),
    (0x28, "LaunchComplete"),
    (0x29, "LocationEnter"),
    (0x2A, "LocationExit"),
    (0x2B, "LootSpawned"),
    (0x2C, "MissionResponse"),
    (0x2D, "PhantomEnter"),
    (0x2E, "PhantomExit"),
    (0x2F, "PlayerManeuver"),
    (0x30, "PopUpDialog"),
    (0x31, "RequestBestPath"),
    (0x32, "FreeTerminalRequest"),
    (0x33, "RequestPlayerStats"),
    (0x35, "PlayerShipArchSet"),
    (0x36, "SpaceEnter"),
    (0x37, "SpaceExit"),
    (0x38, "DeathFuseLit"),
    (0x39, "ObjectDestroyed"),
    (0x3A, "SystemExit"),
    (0x3B, "SystemEnter"),
    (0x3C, "SystemLoaded"),
    (0x3D, "SystemUnloaded"),
    (0x3E, "TimerExpired"),
    (0x3F, "TradeResponse"),
    (0x40, "LoadGame"),
    (0x41, "SaveGame"),
    (0x1000, "MissionStart"),
    (0x1001, "CombatState"),
    (0x1002, "CombatDrift"),
    (0x1003, "NpcTurnedHostile"),
    (0x1004, "FleeRequest"),
    (0x1005, "DamageReputation"),
    (0x1006, "Msg0x1006"),
    (0x1007, "JumpgateAct"),
    (0x1008, "LeadObject"),
    (0x1009, "FormationLeaderChanged"),
    (0x100A, "NpcSystemExit"),
    (0x100B, "PlayerMissionChanged"),
    (0x100C, "EncounterLaunched"),
    (0x100D, "RumorHeard"),
    (0x100E, "ShipInSights"),
    (0x100F, "NpcSystemEnter"),
    (0x1010, "NpcShipCreated"),
    (0x1011, "TetherObject"),
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ids_and_names_follow_the_header() {
        // Neither module sends 0x0e, 0x0f or 0x34.
        assert!(NAMES.iter().all(|(id, _)| ![0x0E, 0x0F, 0x34].contains(id)));
        assert!(NAMES.windows(2).all(|w| w[0].0 < w[1].0));
        let header = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../CppSource/include/common_server_controller.h"),
        );
        if let Ok(header) = header {
            for (id, name) in NAMES {
                let line = header
                    .lines()
                    .find(|l| l.trim_start().starts_with(&format!("{name} ")))
                    .unwrap_or_else(|| panic!("{name} is not in the header"));
                assert!(
                    line.contains(&format!("= 0x{id:02x}"))
                        || line.contains(&format!("= 0x{id:x}")),
                    "{name}: {line}"
                );
            }
        }
        let message = ControllerMessage::Heartbeat { elapsed: 1.0 };
        assert_eq!((message.id(), message.name()), (0x20, "Heartbeat"));
        let mut file = Vec::new();
        let message = ControllerMessage::SaveGame { file: &mut file };
        assert_eq!(format!("{message:?}"), "SaveGame(0x41)");
        let encounters = [1, 2];
        let message = ControllerMessage::EncounterLaunched {
            encounters: &encounters,
        };
        assert_eq!(message.name(), "EncounterLaunched");
    }

    #[test]
    fn comm_results_round_trip() {
        for v in 0..6 {
            assert_eq!(CommResult::from_u32(v).value(), v);
        }
    }
}
