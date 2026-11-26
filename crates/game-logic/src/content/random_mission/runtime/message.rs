//! The routed messages of the random missions (record 0008 point 3): one
//! [`RmMessage`] per C++ message object, neuron-core's
//! `StateMachineMessage` header and one payload variant for each class
//! (`RandomMissionRuntime.h:82`, `:222`, `:233`,
//! `RandomMissionMachine.h:35-98`, `RandomMissionTriggers.cpp:47-62`).
//!
//! **The message types.** The C++ hands them out from Common's counter in
//! the static initialisers of the DLL's units (`GenerateUniqueMessageType`:
//! TU27's `GDestroyMissionMessageType`, TU57's `GRmMsgTriggerFired`, TU61's
//! 21 globals), so their values follow the link order of the units. Only
//! their identity is read, and no value leaves Content, so the port names
//! each type ([`MessageType`]) and gives it a value of its own; the traces
//! compare the names.

use neuron_core::math::Vector;
use neuron_core::util::state_machine::{SMM_CHANGE_STATE, SMM_TIMEOUT, StateMachineMessage};

use super::super::offer::Conversation;

/// A routed message's type: Common's two static ones and Content's.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MessageType {
    /// `SMM_CHANGE_STATE` (1).
    ChangeState,
    /// `SMM_TIMEOUT` (2).
    Timeout,
    /// `GRmMsgStartMachine`: the mission to a runtime machine.
    StartMachine,
    /// `GRmMsgMachineStarted`: a machine to the mission, running.
    MachineStarted,
    /// `GRmMsgMachineSucceeded`.
    MachineSucceeded,
    /// `GRmMsgMachineFailed` ([`Body::MachineFailed`]).
    MachineFailed,
    /// `GRmMsgRefreshObjectives`.
    RefreshObjectives,
    /// `GRmMsgType06fd3ba8`: a tracked ship was destroyed ([`Body::Ship`]).
    ShipDestroyed,
    /// `GRmMsgType06fd3ba4`: every ship of a group was destroyed.
    GroupDestroyed,
    /// `GRmMsgType06fd3ba0`: the flagged ships of a group were destroyed.
    FlaggedShipsLost,
    /// `GRmMsgType06fd3b9c`: a tracked solar was destroyed ([`Body::Solar`]).
    SolarDestroyed,
    /// `GRmMsgType06fd3b98`: a critical loot was spawned ([`Body::Loot`]).
    CriticalLootSpawned,
    /// `GRmMsgType06fd3b94`: a loot was destroyed by a player.
    LootDestroyedByPlayer,
    /// `GRmMsgType06fd3b90`: a loot was picked up.
    LootPickedUp,
    /// `GRmMsgType06fd3b8c`: no critical loot is left.
    CriticalLootGone,
    /// `GRmMsgType06fd3b88`: the holder of a loot docked
    /// ([`Body::LootPlayer`]).
    LootHolderDocked,
    /// `GRmMsgType06fd3b84`: a loot was created again.
    LootRespawned,
    /// `GRmMsgType06fd3b80`: a loot was destroyed otherwise.
    LootDestroyed,
    /// `GRmMsgType06fd3b7c`: the countdown's 5 s tick.
    CountdownTick,
    /// `GRmMsgType06fd3b78`: a delayed ship group ([`Body::Cookie`]).
    Cookie,
    /// `GRmMsgType06fd3b74`: a comm sequence conversation
    /// ([`Body::Conversation`]).
    Conversation,
    /// `GRmMsgPlayerLeft` ([`Body::PlayerLeft`]).
    PlayerLeft,
    /// `GRmMsgType06fd3b6c`: a loot to drop next to a ship.
    LootDrop,
    /// `GRmMsgTriggerFired` ([`Body::TriggerFired`]).
    TriggerFired,
}

impl MessageType {
    /// Every type, in the value order of [`MessageType::value`].
    pub const ALL: [Self; 24] = [
        Self::ChangeState,
        Self::Timeout,
        Self::StartMachine,
        Self::MachineStarted,
        Self::MachineSucceeded,
        Self::MachineFailed,
        Self::RefreshObjectives,
        Self::ShipDestroyed,
        Self::GroupDestroyed,
        Self::FlaggedShipsLost,
        Self::SolarDestroyed,
        Self::CriticalLootSpawned,
        Self::LootDestroyedByPlayer,
        Self::LootPickedUp,
        Self::CriticalLootGone,
        Self::LootHolderDocked,
        Self::LootRespawned,
        Self::LootDestroyed,
        Self::CountdownTick,
        Self::Cookie,
        Self::Conversation,
        Self::PlayerLeft,
        Self::LootDrop,
        Self::TriggerFired,
    ];

    /// The header's `MessageType`: Common's two static values, then the
    /// port's own from 0x100.
    #[must_use]
    pub fn value(self) -> u32 {
        match self {
            Self::ChangeState => SMM_CHANGE_STATE,
            Self::Timeout => SMM_TIMEOUT,
            other => 0x100 + Self::ALL.iter().position(|&t| t == other).unwrap_or(0) as u32,
        }
    }

    /// The type of a header's value.
    #[must_use]
    pub fn of(value: u32) -> Option<Self> {
        Self::ALL.iter().copied().find(|t| t.value() == value)
    }

    /// The name the traces give the type: the C++ global's without its
    /// `GRmMsg` prefix (`Timeout` and `ChangeState` for Common's).
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::ChangeState => "ChangeState",
            Self::Timeout => "Timeout",
            Self::StartMachine => "StartMachine",
            Self::MachineStarted => "MachineStarted",
            Self::MachineSucceeded => "MachineSucceeded",
            Self::MachineFailed => "MachineFailed",
            Self::RefreshObjectives => "RefreshObjectives",
            Self::ShipDestroyed => "Type06fd3ba8",
            Self::GroupDestroyed => "Type06fd3ba4",
            Self::FlaggedShipsLost => "Type06fd3ba0",
            Self::SolarDestroyed => "Type06fd3b9c",
            Self::CriticalLootSpawned => "Type06fd3b98",
            Self::LootDestroyedByPlayer => "Type06fd3b94",
            Self::LootPickedUp => "Type06fd3b90",
            Self::CriticalLootGone => "Type06fd3b8c",
            Self::LootHolderDocked => "Type06fd3b88",
            Self::LootRespawned => "Type06fd3b84",
            Self::LootDestroyed => "Type06fd3b80",
            Self::CountdownTick => "Type06fd3b7c",
            Self::Cookie => "Type06fd3b78",
            Self::Conversation => "Type06fd3b74",
            Self::PlayerLeft => "PlayerLeft",
            Self::LootDrop => "Type06fd3b6c",
            Self::TriggerFired => "TriggerFired",
        }
    }
}

/// A message's payload: what its class adds after the header.
#[derive(Clone, Debug, PartialEq)]
pub enum Body {
    /// A bare `StateMachineMessage`.
    Plain,
    /// `FRmMachineFailedMessage`: `+0x1c` the reason.
    MachineFailed {
        /// `Reason`.
        reason: i32,
    },
    /// `FRmPlayerLeftMessage`.
    PlayerLeft {
        /// `PlayerId`.
        player: u32,
    },
    /// `FRmTriggerFiredMessage`.
    TriggerFired {
        /// `Cookie`.
        cookie: u32,
        /// `bFinal`: the trigger will not fire again.
        last: bool,
        /// `Activator`.
        activator: u32,
    },
    /// `FRmSolarMessage`.
    Solar {
        /// `ObjectId`.
        object: u32,
    },
    /// `FRmLootMessage`.
    Loot {
        /// `ObjectId`.
        object: u32,
        /// `Value`: a player's ship, a count or 0.
        value: u32,
    },
    /// `FRmLootPlayerMessage`.
    LootPlayer {
        /// `ObjectId`.
        object: u32,
        /// `Value0`: message 0x27's ship.
        ship: u32,
        /// `Value1`: message 0x27's base.
        base: u32,
    },
    /// `FRmShipMessage`.
    Ship {
        /// `Group`: the record's group serial.
        group: u32,
        /// `ObjectId`.
        object: u32,
    },
    /// `FRmCookieMessage`.
    Cookie {
        /// `Cookie`: a group serial.
        cookie: u32,
    },
    /// `FRmConversationMessage`: its own copy of the conversation (the
    /// clone the constructor and `clone` make).
    Conversation(Box<Conversation>),
}

/// A routed message: the header and the payload.
#[derive(Clone, Debug, PartialEq)]
pub struct RmMessage {
    /// The `StateMachineMessage` part.
    pub header: StateMachineMessage,
    /// The class's fields.
    pub body: Body,
}

impl RmMessage {
    /// A message of `kind` with `body`, not addressed yet.
    #[must_use]
    pub fn new(kind: MessageType, body: Body) -> Self {
        Self {
            header: StateMachineMessage::new(kind.value()),
            body,
        }
    }

    /// A bare message of `kind`.
    #[must_use]
    pub fn plain(kind: MessageType) -> Self {
        Self::new(kind, Body::Plain)
    }

    /// The message's type.
    #[must_use]
    pub fn kind(&self) -> Option<MessageType> {
        MessageType::of(self.header.message_type)
    }

    /// The C++'s recurring test (`DmIsForCurrentState`,
    /// `RmTriggerIsForCurrentState`, `RmMessageOf`): a message for any
    /// state or for `state`, of type `kind`.
    #[must_use]
    pub fn is(&self, kind: MessageType, state: i32) -> bool {
        (self.header.delivery_state == -1 || self.header.delivery_state == state)
            && self.header.message_type == kind.value()
    }
}

/// A controller message as the random missions' machines read it: the
/// `SMControllerEvent` that `FRandomMissionObject::HandleMissionMessage`
/// builds (its id and the parameter words the handlers read). A save's
/// file and a load's reader stay with the caller.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Controller {
    /// 0x20 `{frame time}`.
    Heartbeat(f32),
    /// 0x39: an object left space.
    ObjectDestroyed {
        /// `Parms[0]`.
        object: u32,
        /// `Parms[1]`: the killer.
        killer: u32,
        /// `Parms[3]`: the owning player.
        player: u32,
        /// `Parms[4]`: the destroy kind (the loot's cause).
        kind: i32,
        /// `Parms[5..8]`.
        position: Vector,
    },
    /// 0x0d (`FSubObjDamageEvent`).
    ObjectDamaged {
        /// `ObjId`.
        object: u32,
        /// `RelativeDamage` (`+0x1c`).
        relative_damage: f32,
    },
    /// 0x18 `{loot, ship, slot, good, units}`.
    LootAcquired {
        /// The loot.
        loot: u32,
        /// The ship.
        ship: u32,
        /// The cargo slot.
        slot: u16,
        /// The good.
        good: u32,
        /// The units.
        count: i32,
    },
    /// 0x27 `{ship, base}`.
    Docked {
        /// The ship.
        ship: u32,
        /// The base.
        base: u32,
    },
    /// 0x3d `{system}`.
    SystemUnloaded(u32),
    /// 0x36 `{system, player}`.
    SpaceEnter {
        /// The system.
        system: u32,
        /// The player.
        player: u32,
    },
    /// 0x3b `{system, from, player}`.
    SystemEnter {
        /// The system.
        system: u32,
        /// The player.
        player: u32,
    },
    /// 0x02 `{base, player}`: the player.
    BaseExit(u32),
    /// 0x0a.
    GroupMemberJoined(u32),
    /// 0x09.
    GroupMemberLeft(u32),
    /// 0x05 `{player, reader}`.
    PlayerLoad(u32),
    /// 0x04 `{client, file}`.
    PlayerSave(u32),
    /// 0x08.
    PlayerLogout(u32),
    /// 0x24 `{player, mission}`.
    AbortMission(u32),
    /// 0x12.
    DockRequest,
    /// 0x25 `{ship, ..}`: the ship.
    ShipJumped(u32),
    /// 0x06 `{player, base}`: the player.
    PlayerLaunch(u32),
    /// 0x37 `{system, player}`: the player.
    SpaceExit(u32),
    /// 0x26.
    JumpInComplete,
    /// 0x1006 `{ship, word, 1}`: the word of the ship's first directive.
    CommLinesDone(u32),
    /// 0x1010 `{npc}`: the NPC ship.
    NpcShipCreated(crate::events::NpcShipKey),
    /// 0x1009.
    FormationLeaderChanged,
    /// Any other message, by id.
    Other,
}

/// A controller event: the message's id and what the handlers read.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ControllerEvent {
    /// The controller message id (`SMControllerEvent::ControllerId`).
    pub id: u32,
    /// Its parameters.
    pub message: Controller,
}
