//! Channel 1: the 21 messages that travel both ways
//! (`include/rpc_packet_codec.h`, `RemoteCommon/remote_packet_codecs.cpp`).
//!
//! `CYCLOPSMESSAGE_COMMON_UPDATEOBJECT` is a second C++ name of
//! [`UpdateObject`]'s ID, not a message of its own.
//!
//! Two messages depend on the [`WireContext`]: [`FireWeapon`] has a
//! single-player form, and [`PlayerTrade`] sends its target player
//! depending on the direction.

use neuron_core::net::wire::quantise::{
    FireTier, expand_fire_medium, expand_fire_small, fire_tier, quantise_fire_medium,
    quantise_fire_small,
};

use super::field::{count_u8, count_u16, get_u32s, put_u32s};
use super::{Body, Direction, WireContext, WireError, WireReader, WireWriter};

wire_message! {
    /// `FLPACKET_COMMON_UPDATEOBJECT`: an object's position and orientation,
    /// 28 bytes with the header.
    pub struct UpdateObject = 0x10001, "FLPACKET_COMMON_UPDATEOBJECT" {
        /// The first byte (`Field_0x00`).
        pub flags: u8,
        /// The object.
        pub object_id: u32,
        /// The position, raw floats.
        pub position: [f32; 3],
        /// The orientation bytes `x`, `y`, `z`, `w`
        /// ([`super::quantise_orientation`]).
        pub orientation: [u8; 4],
        /// The fifth quantised value (`Field_0x28` × 127).
        pub extra: u8,
        /// The update time.
        pub update_time: f32 => x87,
    }
}

/// One `FIREWEAPON` target component as it travels.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum FireComponent {
    /// The float itself.
    Full(f32),
    /// One byte, `value × 127/64`.
    Small(u8),
    /// One word, `value × 32767/8192`.
    Medium(u16),
}

impl Default for FireComponent {
    fn default() -> Self {
        Self::Full(f32::MAX)
    }
}

impl FireComponent {
    /// The component the encoder picks for `value` (`RpcFirePickTier`):
    /// `medium` is false for `z`.
    #[must_use]
    pub fn from_value(value: f32, medium: bool) -> Self {
        match fire_tier(value, medium) {
            FireTier::Small => Self::Small(quantise_fire_small(value)),
            FireTier::Medium => Self::Medium(quantise_fire_medium(value)),
            FireTier::Full => Self::Full(value),
        }
    }

    /// The value the receiver computes.
    #[must_use]
    pub fn value(self) -> f32 {
        match self {
            Self::Full(value) => value,
            Self::Small(byte) => expand_fire_small(byte),
            Self::Medium(word) => expand_fire_medium(word),
        }
    }

    const fn bits(self) -> u8 {
        match self {
            Self::Full(_) => 0,
            Self::Small(_) => 1,
            Self::Medium(_) => 2,
        }
    }

    fn put(self, writer: &mut WireWriter) -> Result<(), WireError> {
        match self {
            Self::Full(value) => writer.put_f32_x87(value),
            Self::Small(byte) => writer.put_u8(byte),
            Self::Medium(word) => writer.put_u16(word),
        }
    }

    fn get(reader: &mut WireReader<'_>, bits: u8) -> Result<Self, WireError> {
        if bits & 1 != 0 {
            reader.get_u8().map(Self::Small)
        } else if bits & 2 != 0 {
            reader.get_u16().map(Self::Medium)
        } else {
            reader.get_f32().map(Self::Full)
        }
    }
}

/// `FLPACKET_COMMON_FIREWEAPON`: a ship fires the guns on some hardpoints
/// at a target position.
///
/// Multiplayer: a flag byte with two bits per axis (`x` and `y` small or
/// medium, `z` small) and `0x40` for exactly one hardpoint, the three
/// components, the hardpoint count unless it is one, the hardpoints.
/// Single player: the target as three floats, the count, the hardpoints;
/// a quantised component is sent as its value there.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct FireWeapon {
    /// The firing object.
    pub object_id: u32,
    /// The target `x`, `y`, `z`.
    pub target: [FireComponent; 3],
    /// The hardpoint IDs, at most 255.
    pub hardpoints: Vec<u16>,
}

impl FireWeapon {
    /// The components the multiplayer encoder picks for a target position.
    #[must_use]
    pub fn target_from(position: [f32; 3]) -> [FireComponent; 3] {
        [
            FireComponent::from_value(position[0], true),
            FireComponent::from_value(position[1], true),
            FireComponent::from_value(position[2], false),
        ]
    }
}

impl Body for FireWeapon {
    const ID: u32 = 0x10002;
    const NAME: &'static str = "FLPACKET_COMMON_FIREWEAPON";

    fn encode_body(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        let count = count_u8(self.hardpoints.len())?;
        writer.put_u32(self.object_id)?;
        if ctx.single_player {
            writer.put_vector(self.target.map(FireComponent::value))?;
            writer.put_u8(count)?;
        } else {
            let [x, y, z] = self.target;
            let mut flags = x.bits() | y.bits() << 2 | z.bits() << 4;
            if count == 1 {
                flags |= 0x40;
            }
            writer.put_u8(flags)?;
            x.put(writer)?;
            y.put(writer)?;
            z.put(writer)?;
            if count != 1 {
                writer.put_u8(count)?;
            }
        }
        for hardpoint in &self.hardpoints {
            writer.put_u16(*hardpoint)?;
        }
        Ok(())
    }

    fn decode_body(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let object_id = reader.get_u32()?;
        let (target, count) = if ctx.single_player {
            let target = reader.get_vector()?.map(FireComponent::Full);
            (target, reader.get_u8()?)
        } else {
            let flags = reader.get_u8()?;
            let x = FireComponent::get(reader, flags)?;
            let y = FireComponent::get(reader, flags >> 2)?;
            let z = FireComponent::get(reader, flags >> 4)?;
            let count = if flags & 0x40 != 0 {
                1
            } else {
                reader.get_u8()?
            };
            ([x, y, z], count)
        };
        let hardpoints = super::field::get_list(reader, u32::from(count), 2, WireReader::get_u16)?;
        Ok(Self {
            object_id,
            target,
            hardpoints,
        })
    }
}

wire_message! {
    /// `FLPACKET_COMMON_SETTARGET`.
    pub struct SetTarget = 0x10004, "FLPACKET_COMMON_SETTARGET" {
        /// The targeting object.
        pub object_id: u32,
        /// The target.
        pub target_id: u32,
        /// The targeting sub-object.
        pub sub_obj_id: u16,
        /// The targeted sub-object.
        pub target_sub_obj_id: u16,
    }
}

/// `FLPACKET_COMMON_CHATMSG`: binary rich text ([`super::rdl`]) to a
/// target.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ChatMsg {
    /// The message, binary RDL records.
    pub data: Vec<u8>,
    /// The recipient.
    pub target_id: u32,
    /// The second dword (`Field_0x04`).
    pub field_04: u32,
}

impl Body for ChatMsg {
    const ID: u32 = 0x10005;
    const NAME: &'static str = "FLPACKET_COMMON_CHATMSG";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_bytes_u32(&self.data)?;
        writer.put_u32(self.target_id)?;
        writer.put_u32(self.field_04)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            data: reader.get_bytes_u32()?.to_vec(),
            target_id: reader.get_u32()?,
            field_04: reader.get_u32()?,
        })
    }
}

wire_message! {
    /// `FLPACKET_COMMON_ACTIVATEEQUIP`.
    pub struct ActivateEquip = 0x10008, "FLPACKET_COMMON_ACTIVATEEQUIP" {
        /// The object.
        pub object_id: u32,
        /// The equipment ID.
        pub equip_id: u16,
        /// The raw active byte.
        pub active: u8,
    }
}

wire_message! {
    /// `FLPACKET_COMMON_ACTIVATECRUISE`.
    pub struct ActivateCruise = 0x1000e, "FLPACKET_COMMON_ACTIVATECRUISE" {
        /// The object.
        pub object_id: u32,
        /// The raw active byte.
        pub active: u8,
        /// The second byte (`Field_0x05`).
        pub field_05: u8,
    }
}

wire_message! {
    /// `FLPACKET_COMMON_GOTRADELANE`.
    pub struct GoTradeLane = 0x1000f, "FLPACKET_COMMON_GOTRADELANE" {
        /// The ship.
        pub object_id: u32,
        /// The first ring.
        pub tradelane_ring1: u32,
        /// The second ring.
        pub tradelane_ring2: u32,
        /// The fourth value, a byte (`Field_0x0c`).
        pub field_0c: u8,
    }
}

wire_message! {
    /// `FLPACKET_COMMON_STOPTRADELANE`.
    pub struct StopTradeLane = 0x10010, "FLPACKET_COMMON_STOPTRADELANE" {
        /// The ship.
        pub ship_id: u32,
        /// The first ring.
        pub tradelane_ring1: u32,
        /// The second ring.
        pub tradelane_ring2: u32,
    }
}

buffer_message! {
    /// `FLPACKET_COMMON_SET_WEAPON_GROUP`: the client's weapon groups.
    pub struct SetWeaponGroup = 0x10011, "FLPACKET_COMMON_SET_WEAPON_GROUP";
}

/// `FLPACKET_COMMON_PLAYER_TRADE`: one step of a trade between players.
///
/// The action byte selects the fields: 0x02 the reason, 0x04 the money,
/// 0x08 and 0x10 an equipment item (0x08 with its health), 0x40 the
/// accepted byte. Every action but 0x02 and the unknown ones also names the
/// target player. That dword travels from the server always, from a client
/// only for the actions 0x01, 0x20 and 0x80: the C++ writes it after the
/// message's counted length otherwise (divergence D106).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PlayerTrade {
    /// The action (the low byte of `Action`).
    pub action: u8,
    /// Action 0x02: the reason, a signed byte.
    pub reason: i8,
    /// Action 0x04: the money.
    pub money: i32,
    /// Actions 0x08 and 0x10: the item's archetype.
    pub equip_arch_id: u32,
    /// Actions 0x08 and 0x10: the item count.
    pub equip_count: i32,
    /// Action 0x08: the item's health, through the x87 stack.
    pub equip_health: f32,
    /// Actions 0x08 and 0x10: the item ID.
    pub equip_id: u16,
    /// Action 0x40: accepted (any non-zero byte reads as true).
    pub accepted: bool,
    /// The target player, where it travels.
    pub target_player: u32,
}

impl PlayerTrade {
    fn target_on_wire(&self, direction: Direction) -> bool {
        let needs_target = !matches!(self.action, 0x02)
            && matches!(self.action, 0x01 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 | 0x80);
        needs_target
            && (direction == Direction::ToClient || matches!(self.action, 0x01 | 0x20 | 0x80))
    }
}

impl Body for PlayerTrade {
    const ID: u32 = 0x10012;
    const NAME: &'static str = "FLPACKET_COMMON_PLAYER_TRADE";

    fn encode_body(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u8(self.action)?;
        match self.action {
            0x02 => writer.put_i8(self.reason)?,
            0x04 => writer.put_i32(self.money)?,
            0x08 => {
                writer.put_u32(self.equip_arch_id)?;
                writer.put_i32(self.equip_count)?;
                writer.put_f32_x87(self.equip_health)?;
                writer.put_u16(self.equip_id)?;
            }
            0x10 => {
                writer.put_u32(self.equip_arch_id)?;
                writer.put_i32(self.equip_count)?;
                writer.put_u16(self.equip_id)?;
            }
            0x40 => writer.put_u8(u8::from(self.accepted))?,
            _ => {}
        }
        if self.target_on_wire(ctx.direction) {
            writer.put_u32(self.target_player)?;
        }
        Ok(())
    }

    fn decode_body(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let mut trade = Self {
            action: reader.get_u8()?,
            ..Self::default()
        };
        match trade.action {
            0x02 => trade.reason = reader.get_i8()?,
            0x04 => trade.money = reader.get_i32()?,
            0x08 => {
                trade.equip_arch_id = reader.get_u32()?;
                trade.equip_count = reader.get_i32()?;
                trade.equip_health = reader.get_f32()?;
                trade.equip_id = reader.get_u16()?;
            }
            0x10 => {
                trade.equip_arch_id = reader.get_u32()?;
                trade.equip_count = reader.get_i32()?;
                trade.equip_id = reader.get_u16()?;
            }
            0x40 => trade.accepted = reader.get_u8()? != 0,
            _ => {}
        }
        if trade.target_on_wire(ctx.direction) {
            trade.target_player = reader.get_u32()?;
        }
        Ok(trade)
    }
}

buffer_message! {
    /// `FLPACKET_COMMON_SET_VISITED_STATE`.
    pub struct SetVisitedState = 0x10013, "FLPACKET_COMMON_SET_VISITED_STATE";
}

wire_message! {
    /// `FLPACKET_COMMON_JETTISONCARGO`.
    pub struct JettisonCargo = 0x10014, "FLPACKET_COMMON_JETTISONCARGO" {
        /// The ship.
        pub object_id: u32,
        /// The cargo item.
        pub item_id: u16,
        /// The count.
        pub count: u32,
    }
}

wire_message! {
    /// `FLPACKET_COMMON_ACTIVATETHRUSTERS`.
    pub struct ActivateThrusters = 0x10015, "FLPACKET_COMMON_ACTIVATETHRUSTERS" {
        /// The ship.
        pub object_id: u32,
        /// The raw active byte.
        pub active: u8,
    }
}

buffer_message! {
    /// `FLPACKET_COMMON_REQUEST_BEST_PATH`.
    pub struct RequestBestPath = 0x10016, "FLPACKET_COMMON_REQUEST_BEST_PATH";
}

buffer_message! {
    /// `FLPACKET_COMMON_REQUEST_GROUP_POSITIONS`.
    pub struct RequestGroupPositions = 0x10017, "FLPACKET_COMMON_REQUEST_GROUP_POSITIONS";
}

buffer_message! {
    /// `FLPACKET_COMMON_REQUEST_PLAYER_STATS`.
    pub struct RequestPlayerStats = 0x10018, "FLPACKET_COMMON_REQUEST_PLAYER_STATS";
}

buffer_message! {
    /// `FLPACKET_COMMON_SET_MISSION_LOG`.
    pub struct SetMissionLog = 0x10019, "FLPACKET_COMMON_SET_MISSION_LOG";
}

buffer_message! {
    /// `FLPACKET_COMMON_REQUEST_RANK_LEVEL`.
    pub struct RequestRankLevel = 0x1001a, "FLPACKET_COMMON_REQUEST_RANK_LEVEL";
}

/// `FLPACKET_COMMON_POP_UP_DIALOG`: two blocks and a dword.
///
/// The C++ encoder announces the second block's size but copies as many
/// bytes of it as the first block has; the two agree for every dialog the
/// game builds. This encoder writes the second block whole (divergence
/// D105).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PopUpDialog {
    /// The first block.
    pub data_a: Vec<u8>,
    /// The second block.
    pub data_b: Vec<u8>,
    /// The last dword (`Field_0x10`).
    pub field_10: u32,
}

impl Body for PopUpDialog {
    const ID: u32 = 0x1001b;
    const NAME: &'static str = "FLPACKET_COMMON_POP_UP_DIALOG";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_bytes_u32(&self.data_a)?;
        writer.put_bytes_u32(&self.data_b)?;
        writer.put_u32(self.field_10)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            data_a: reader.get_bytes_u32()?.to_vec(),
            data_b: reader.get_bytes_u32()?.to_vec(),
            field_10: reader.get_u32()?,
        })
    }
}

buffer_message! {
    /// `FLPACKET_COMMON_SET_INTERFACE_STATE`.
    pub struct SetInterfaceState = 0x1001c, "FLPACKET_COMMON_SET_INTERFACE_STATE";
}

/// `FLPACKET_COMMON_TRACTOROBJECTS`: a ship's tractor beam and its targets.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TractorObjects {
    /// The ship.
    pub object_id: u32,
    /// The tractor equipment ID.
    pub tractor_equip_id: u16,
    /// The targets, at most 65 535.
    pub target_ids: Vec<u32>,
}

impl Body for TractorObjects {
    const ID: u32 = 0x1001d;
    const NAME: &'static str = "FLPACKET_COMMON_TRACTOROBJECTS";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        let count = count_u16(self.target_ids.len())?;
        writer.put_u32(self.object_id)?;
        writer.put_u16(self.tractor_equip_id)?;
        writer.put_u16(count)?;
        put_u32s(writer, &self.target_ids)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let object_id = reader.get_u32()?;
        let tractor_equip_id = reader.get_u16()?;
        let count = reader.get_u16()?;
        Ok(Self {
            object_id,
            tractor_equip_id,
            target_ids: get_u32s(reader, u32::from(count))?,
        })
    }
}

catalog! {
    /// A channel 1 message.
    pub enum CommonMessage {
        UpdateObject,
        FireWeapon,
        SetTarget,
        ChatMsg,
        ActivateEquip,
        ActivateCruise,
        GoTradeLane,
        StopTradeLane,
        SetWeaponGroup,
        PlayerTrade,
        SetVisitedState,
        JettisonCargo,
        ActivateThrusters,
        RequestBestPath,
        RequestGroupPositions,
        RequestPlayerStats,
        SetMissionLog,
        RequestRankLevel,
        PopUpDialog,
        SetInterfaceState,
        TractorObjects,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn update_object_is_28_bytes() {
        let message = UpdateObject::default();
        assert_eq!(message.to_wire(&WireContext::SERVER).unwrap().len(), 28);
        assert_eq!(CommonMessage::IDS.len(), 21);
    }

    #[test]
    fn fire_weapon_forms() {
        let message = FireWeapon {
            object_id: 1,
            target: FireWeapon::target_from([1.0, 100.0, 1e6]),
            hardpoints: vec![7],
        };
        let bytes = message.to_wire(&WireContext::CLIENT).unwrap();
        assert_eq!(bytes[6], 0x01 | 0x08 | 0x40);
        let back = CommonMessage::from_wire(&WireContext::CLIENT, &bytes).unwrap();
        assert_eq!(back, CommonMessage::FireWeapon(message.clone()));
        let sp = message.to_wire(&WireContext::SINGLE_PLAYER_CLIENT).unwrap();
        assert_eq!(sp.len(), 2 + 4 + 12 + 1 + 2);
    }

    #[test]
    fn player_trade_target_by_direction() {
        let trade = PlayerTrade {
            action: 0x04,
            money: 5,
            target_player: 9,
            ..PlayerTrade::default()
        };
        assert_eq!(
            trade.to_wire(&WireContext::SERVER).unwrap().len(),
            2 + 1 + 4 + 4
        );
        assert_eq!(
            trade.to_wire(&WireContext::CLIENT).unwrap().len(),
            2 + 1 + 4
        );
        let request = PlayerTrade {
            action: 0x01,
            target_player: 9,
            ..PlayerTrade::default()
        };
        assert_eq!(
            request.to_wire(&WireContext::CLIENT).unwrap().len(),
            2 + 1 + 4
        );
    }
}
