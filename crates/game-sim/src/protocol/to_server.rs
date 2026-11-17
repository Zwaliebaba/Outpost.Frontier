//! Channel 3: the 60 messages from a client to the server
//! (`include/rpc_packet_codec.h`, `RemoteCommon/remote_packet_codecs.cpp`).
//!
//! The scalar messages (`FRpcScalarPacket`) carry one field named `value`,
//! and the dword records (`FRpcClientDword*Packet`) fields named after
//! their offsets: the C++ has not recovered what most of them mean.

use super::field::{count_u8, count_u32, get_u32s, put_u32s, put_units};
use super::records::CollisionGroup;
use super::{Body, WireContext, WireError, WireReader, WireWriter};

/// A character ID (`CHARACTER_ID`): the save file's name, at most 511
/// bytes.
pub const MAX_CHARACTER_ID: usize = 0x1ff;

/// Writes a character ID: a `u16` length and the bytes.
///
/// # Errors
/// [`WireError::LengthOutOfRange`] above [`MAX_CHARACTER_ID`] bytes.
pub fn put_character_id(writer: &mut WireWriter, id: &[u8]) -> Result<(), WireError> {
    super::field::count_fits(id.len(), MAX_CHARACTER_ID)?;
    writer.put_bytes_u16(id)
}

/// Reads a character ID. A length of 512 or more is refused, as the
/// server's decoder refuses it (error `0x4a`).
///
/// # Errors
/// [`WireError::LengthOutOfRange`] for a long ID, [`WireError::Underrun`]
/// for a short input.
pub fn get_character_id(reader: &mut WireReader<'_>) -> Result<Vec<u8>, WireError> {
    let length = reader.get_u16()?;
    super::field::count_fits(usize::from(length), MAX_CHARACTER_ID)?;
    Ok(reader.get_bytes(usize::from(length))?.to_vec())
}

/// `FLPACKET_CLIENT_LOGIN`: the account name and a second string.
///
/// The encoder lower-cases the account name (`_wcslwr` in the C locale:
/// `A` to `Z` only) before it writes it; the decoder keeps what arrives.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Login {
    /// The create flag byte.
    pub create_flag: u8,
    /// The account name, at most 255 units.
    pub account_name: Vec<u16>,
    /// The second string (`Field_0x220`), at most 255 units.
    pub second: Vec<u16>,
}

/// `_wcslwr` in the C locale.
#[must_use]
pub fn lower_ascii_units(units: &[u16]) -> Vec<u16> {
    units
        .iter()
        .map(|&u| {
            if (0x41..=0x5a).contains(&u) {
                u + 0x20
            } else {
                u
            }
        })
        .collect()
}

impl Body for Login {
    const ID: u32 = 0x30001;
    const NAME: &'static str = "FLPACKET_CLIENT_LOGIN";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        let account = count_u8(self.account_name.len())?;
        let second = count_u8(self.second.len())?;
        writer.put_u8(self.create_flag)?;
        writer.put_u8(account)?;
        writer.put_u8(second)?;
        put_units(writer, &lower_ascii_units(&self.account_name))?;
        put_units(writer, &self.second)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let create_flag = reader.get_u8()?;
        let account = reader.get_u8()?;
        let second = reader.get_u8()?;
        Ok(Self {
            create_flag,
            account_name: reader.get_wide(usize::from(account))?,
            second: reader.get_wide(usize::from(second))?,
        })
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_MUNCOLLISION`: a munition hit.
    pub struct MunCollision = 0x30003, "FLPACKET_CLIENT_MUNCOLLISION" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
        /// The object hit.
        pub object_id: u32,
        /// `Field_0x0c`, a sub-object.
        pub field_0c: u16,
        /// Where it hit.
        pub hit_position: [f32; 3],
    }
}

scalar_messages! {
    /// `FLPACKET_CLIENT_REQUESTLAUNCH`.
    RequestLaunch = 0x30004, "FLPACKET_CLIENT_REQUESTLAUNCH", u32;
    /// `FLPACKET_CLIENT_ENTERBASE`.
    EnterBase = 0x30007, "FLPACKET_CLIENT_ENTERBASE", u32;
    /// `FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE`.
    SystemSwitchOutComplete = 0x3000b, "FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE", u32;
    /// `FLPACKET_CLIENT_EXITBASE`.
    ExitBase = 0x3000d, "FLPACKET_CLIENT_EXITBASE", u32;
    /// `FLPACKET_CLIENT_ENTERLOCATION`.
    EnterLocation = 0x3000e, "FLPACKET_CLIENT_ENTERLOCATION", u32;
    /// `FLPACKET_CLIENT_EXITLOCATION`.
    ExitLocation = 0x3000f, "FLPACKET_CLIENT_EXITLOCATION", u32;
    /// `FLPACKET_CLIENT_GFSELECTOBJECT`.
    GfSelectObject = 0x30013, "FLPACKET_CLIENT_GFSELECTOBJECT", u32;
    /// `FLPACKET_CLIENT_REQSHIPARCH`.
    ReqShipArch = 0x30015, "FLPACKET_CLIENT_REQSHIPARCH", u32;
    /// `FLPACKET_CLIENT_REQSETCASH`.
    ReqSetCash = 0x3001b, "FLPACKET_CLIENT_REQSETCASH", i32;
    /// `FLPACKET_CLIENT_REQCHANGECASH`.
    ReqChangeCash = 0x3001c, "FLPACKET_CLIENT_REQCHANGECASH", i32;
    /// `FLPACKET_CLIENT_DOCK`.
    Dock = 0x30024, "FLPACKET_CLIENT_DOCK", u32;
    /// `FLPACKET_CLIENT_INTERFACEITEMUSED`.
    InterfaceItemUsed = 0x30035, "FLPACKET_CLIENT_INTERFACEITEMUSED", u32;
    /// `FLPACKET_CLIENT_REQHULLSTATUS`.
    ReqHullStatus = 0x3003b, "FLPACKET_CLIENT_REQHULLSTATUS", f32 => x87;
    /// `FLPACKET_CLIENT_ABORT_MISSION`.
    AbortMission = 0x30041, "FLPACKET_CLIENT_ABORT_MISSION", u32;
    /// `FLPACKET_CLIENT_MISSIONSAVEB`.
    MissionSaveB = 0x30045, "FLPACKET_CLIENT_MISSIONSAVEB", u32;
    /// `FLPACKET_CLIENT_REQDIFFICULTYSCALE`.
    ReqDifficultyScale = 0x30046, "FLPACKET_CLIENT_REQDIFFICULTYSCALE", f32 => x87;
    /// `FLPACKET_CLIENT_RTCDONE`.
    RtcDone = 0x30047, "FLPACKET_CLIENT_RTCDONE", u32;
}

empty_messages! {
    /// `FLPACKET_CLIENT_REQUESTCHARINFO`.
    RequestCharInfo = 0x30005, "FLPACKET_CLIENT_REQUESTCHARINFO";
    /// `FLPACKET_CLIENT_GFREQUESTSHIPINFO`.
    GfRequestShipInfo = 0x3000a, "FLPACKET_CLIENT_GFREQUESTSHIPINFO";
    /// `FLPACKET_CLIENT_REQUESTCREATESHIP`.
    RequestCreateShip = 0x30010, "FLPACKET_CLIENT_REQUESTCREATESHIP";
    /// `FLPACKET_CLIENT_DBGFREESYSTEM`.
    DbgFreeSystem = 0x3002e, "FLPACKET_CLIENT_DBGFREESYSTEM";
    /// `FLPACKET_CLIENT_REQUESTNEWCHARINFO`.
    RequestNewCharInfo = 0x30038, "FLPACKET_CLIENT_REQUESTNEWCHARINFO";
    /// `FLPACKET_CLIENT_SKIP_AUTOSAVE`.
    SkipAutosave = 0x30042, "FLPACKET_CLIENT_SKIP_AUTOSAVE";
}

/// Declares the character-ID messages.
macro_rules! character_id_messages {
    ($($(#[$meta:meta])* $name:ident = $id:literal, $cname:literal;)*) => {
        $(
            $(#[$meta])*
            #[derive(Clone, Debug, Default, PartialEq, Eq)]
            pub struct $name {
                /// The character ID.
                pub character_id: Vec<u8>,
            }

            impl Body for $name {
                const ID: u32 = $id;
                const NAME: &'static str = $cname;

                fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
                    put_character_id(writer, &self.character_id)
                }

                fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
                    Ok(Self { character_id: get_character_id(reader)? })
                }
            }
        )*
    };
}

character_id_messages! {
    /// `FLPACKET_CLIENT_SELECTCHARACTER`.
    SelectCharacter = 0x30006, "FLPACKET_CLIENT_SELECTCHARACTER";
    /// `FLPACKET_CLIENT_DESTROYCHAR`.
    DestroyChar = 0x3003a, "FLPACKET_CLIENT_DESTROYCHAR";
}

wire_message! {
    /// `FLPACKET_CLIENT_REQUESTBASEINFO`.
    pub struct RequestBaseInfo = 0x30008, "FLPACKET_CLIENT_REQUESTBASEINFO" {
        /// The base.
        pub id: u32,
        /// The raw full byte.
        pub full: u8,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_REQUESTLOCATIONINFO`.
    pub struct RequestLocationInfo = 0x30009, "FLPACKET_CLIENT_REQUESTLOCATIONINFO" {
        /// The location.
        pub id: u32,
        /// The raw full byte.
        pub full: u8,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_OBJCOLLISION`.
    pub struct ObjCollision = 0x3000c, "FLPACKET_CLIENT_OBJCOLLISION" {
        /// The colliding object.
        pub object_id: u32,
        /// Its sub-object.
        pub sub_obj_id: u16,
        /// The other object.
        pub target_object_id: u32,
        /// Its sub-object.
        pub target_sub_obj_id: u16,
        /// `Field_0x10`.
        pub field_10: f32 => x87,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_GFGOODSELL`.
    pub struct GfGoodSell = 0x30011, "FLPACKET_CLIENT_GFGOODSELL" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
        /// `Field_0x08`.
        pub field_08: u32,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_GFGOODBUY`.
    pub struct GfGoodBuy = 0x30012, "FLPACKET_CLIENT_GFGOODBUY" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
        /// `Field_0x08`.
        pub field_08: u32,
        /// `Field_0x0c`.
        pub field_0c: u32,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_MISSIONRESPONSE`.
    pub struct MissionResponse = 0x30014, "FLPACKET_CLIENT_MISSIONRESPONSE" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// The mission.
        pub mission_id: u32,
        /// The raw accepted byte.
        pub accepted: u8,
    }
}

equip_list_messages! {
    /// `FLPACKET_CLIENT_REQEQUIPMENT`.
    ReqEquipment = 0x30016, "FLPACKET_CLIENT_REQEQUIPMENT";
    /// `FLPACKET_CLIENT_REQCARGO`.
    ReqCargo = 0x30017, "FLPACKET_CLIENT_REQCARGO";
}

/// Declares the item-change requests (`FRpcItemChangePacketBase`).
macro_rules! item_change_messages {
    ($($(#[$meta:meta])* $name:ident = $id:literal, $cname:literal, $selector:ident;)*) => {
        $(
            $(#[$meta])*
            #[derive(Clone, Debug, Default, PartialEq)]
            pub struct $name {
                /// The item.
                pub $selector: u32,
                /// The count.
                pub count: i32,
                /// The health, through the x87 stack.
                pub health: f32,
                /// The raw mounted byte.
                pub mounted: u8,
                /// The hardpoint block as the client builds it.
                pub hardpoint: Vec<u8>,
            }

            impl Body for $name {
                const ID: u32 = $id;
                const NAME: &'static str = $cname;

                fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
                    writer.put_u32(self.$selector)?;
                    writer.put_i32(self.count)?;
                    writer.put_f32_x87(self.health)?;
                    writer.put_u8(self.mounted)?;
                    writer.put_bytes_u32(&self.hardpoint)
                }

                fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
                    Ok(Self {
                        $selector: reader.get_u32()?,
                        count: reader.get_i32()?,
                        health: reader.get_f32()?,
                        mounted: reader.get_u8()?,
                        hardpoint: reader.get_bytes_u32()?.to_vec(),
                    })
                }
            }
        )*
    };
}

item_change_messages! {
    /// `FLPACKET_CLIENT_REQADDITEM`.
    ReqAddItem = 0x30018, "FLPACKET_CLIENT_REQADDITEM", arch_id;
    /// `FLPACKET_CLIENT_REQMODIFYITEM`.
    ReqModifyItem = 0x3001a, "FLPACKET_CLIENT_REQMODIFYITEM", equip_id;
}

wire_message! {
    /// `FLPACKET_CLIENT_REQREMOVEITEM`.
    pub struct ReqRemoveItem = 0x30019, "FLPACKET_CLIENT_REQREMOVEITEM" {
        /// The item.
        pub equip_id: u32,
        /// The count.
        pub count: i32,
    }
}

/// `FLPACKET_CLIENT_SAVEGAME`: the save's description. The character ID
/// the C++ packet holds does not travel.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SaveGame {
    /// The description, UTF-16.
    pub description: Vec<u16>,
}

impl Body for SaveGame {
    const ID: u32 = 0x3001e;
    const NAME: &'static str = "FLPACKET_CLIENT_SAVEGAME";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(count_u32(self.description.len())?)?;
        put_units(writer, &self.description)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let count = reader.get_u32()?;
        let count = super::field::wire_len(count, 2, reader)?;
        Ok(Self {
            description: reader.get_wide(count)?,
        })
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_MINEASTEROID`.
    pub struct MineAsteroid = 0x30020, "FLPACKET_CLIENT_MINEASTEROID" {
        /// The system.
        pub system_id: u32,
        /// Where.
        pub position: [f32; 3],
        /// The loot.
        pub loot_id: u32,
        /// The secondary loot.
        pub secondary_loot: u32,
        /// How much.
        pub quantity: u32,
    }
}

/// `FLPACKET_CLIENT_DBGCREATESHIP`: a debug spawn.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DbgCreateShip {
    /// `Field_0x00` to `Field_0x10`.
    pub fields: [u32; 5],
    /// `Field_0x14`.
    pub vector_a: [f32; 3],
    /// `Field_0x20`.
    pub vector_b: [f32; 3],
    /// `Field_0x2c`, `w`, `x`, `y`, `z`.
    pub orientation: [f32; 4],
    /// `Field_0x3c`.
    pub field_3c: u32,
    /// The wide string.
    pub wide: Vec<u16>,
    /// The first block.
    pub data_a: Vec<u8>,
    /// The second block.
    pub data_b: Vec<u8>,
    /// `Field_0x50` to `Field_0x5c`.
    pub tail: [u32; 4],
    /// The dword array.
    pub array: Vec<u32>,
}

impl Body for DbgCreateShip {
    const ID: u32 = 0x30022;
    const NAME: &'static str = "FLPACKET_CLIENT_DBGCREATESHIP";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        put_u32s(writer, &self.fields)?;
        writer.put_vector(self.vector_a)?;
        writer.put_vector(self.vector_b)?;
        writer.put_quaternion(self.orientation)?;
        writer.put_u32(self.field_3c)?;
        writer.put_u32(count_u32(self.wide.len())?)?;
        put_units(writer, &self.wide)?;
        writer.put_bytes_u32(&self.data_a)?;
        writer.put_bytes_u32(&self.data_b)?;
        put_u32s(writer, &self.tail)?;
        writer.put_u32(count_u32(self.array.len())?)?;
        put_u32s(writer, &self.array)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let mut fields = [0; 5];
        for field in &mut fields {
            *field = reader.get_u32()?;
        }
        let vector_a = reader.get_vector()?;
        let vector_b = reader.get_vector()?;
        let orientation = reader.get_quaternion()?;
        let field_3c = reader.get_u32()?;
        let wide_count = reader.get_u32()?;
        let wide_count = super::field::wire_len(wide_count, 2, reader)?;
        let wide = reader.get_wide(wide_count)?;
        let data_a = reader.get_bytes_u32()?.to_vec();
        let data_b = reader.get_bytes_u32()?.to_vec();
        let mut tail = [0; 4];
        for field in &mut tail {
            *field = reader.get_u32()?;
        }
        let count = reader.get_u32()?;
        Ok(Self {
            fields,
            vector_a,
            vector_b,
            orientation,
            field_3c,
            wide,
            data_a,
            data_b,
            tail,
            array: get_u32s(reader, count)?,
        })
    }
}

/// `FLPACKET_CLIENT_DBGLOADSYSTEM`: a system name, its terminator
/// included.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DbgLoadSystem {
    /// The bytes.
    pub data: Vec<u8>,
}

impl Body for DbgLoadSystem {
    const ID: u32 = 0x30023;
    const NAME: &'static str = "FLPACKET_CLIENT_DBGLOADSYSTEM";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_bytes_u32(&self.data)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            data: reader.get_bytes_u32()?.to_vec(),
        })
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_DBGDESTROYOBJECT`.
    pub struct DbgDestroyObject = 0x30025, "FLPACKET_CLIENT_DBGDESTROYOBJECT" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u16,
    }
}

/// `FLPACKET_CLIENT_TRADERESPONSE`: a block of bytes.
///
/// The C++ client's encoder writes the block's address after its size,
/// not its bytes; the server's decoder reads the bytes. This encoder
/// writes the bytes (divergence D107).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TradeResponse {
    /// The bytes.
    pub data: Vec<u8>,
}

impl Body for TradeResponse {
    const ID: u32 = 0x30027;
    const NAME: &'static str = "FLPACKET_CLIENT_TRADERESPONSE";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_bytes_u32(&self.data)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            data: reader.get_bytes_u32()?.to_vec(),
        })
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_CARGOSCAN`.
    pub struct CargoScan = 0x3002b, "FLPACKET_CLIENT_CARGOSCAN" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
    }
}

/// `FLPACKET_CLIENT_DBGCONSOLE`: a dword and a 128-byte command.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DbgConsole {
    /// `Field_0x00`.
    pub field_00: u32,
    /// The command buffer.
    pub command: [u8; 128],
}

impl Default for DbgConsole {
    fn default() -> Self {
        Self {
            field_00: 0,
            command: [0; 128],
        }
    }
}

impl Body for DbgConsole {
    const ID: u32 = 0x3002d;
    const NAME: &'static str = "FLPACKET_CLIENT_DBGCONSOLE";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.field_00)?;
        writer.put_bytes(&self.command)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let field_00 = reader.get_u32()?;
        let mut command = [0; 128];
        command.copy_from_slice(reader.get_bytes(128)?);
        Ok(Self { field_00, command })
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_SETMANEUVER`.
    pub struct SetManeuver = 0x3002f, "FLPACKET_CLIENT_SETMANEUVER" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// The ship.
        pub object_id: u32,
        /// `Field_0x08`.
        pub field_08: u32,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_DBGRELOCATE_SHIP`.
    pub struct DbgRelocateShip = 0x30030, "FLPACKET_CLIENT_DBGRELOCATE_SHIP" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// Where to.
        pub position: [f32; 3],
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_REQUEST_EVENT`.
    pub struct RequestEvent = 0x30031, "FLPACKET_CLIENT_REQUEST_EVENT" {
        /// The event type.
        pub event_type: u8,
        /// The first argument.
        pub arg1: u32,
        /// The second argument.
        pub arg2: u32,
        /// The third argument.
        pub arg3: u32,
        /// `Field_0x10`.
        pub field_10: u8,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_REQUEST_CANCEL`.
    pub struct RequestCancel = 0x30032, "FLPACKET_CLIENT_REQUEST_CANCEL" {
        /// The event type.
        pub event_type: u8,
        /// The first argument.
        pub arg1: u32,
        /// The second argument.
        pub arg2: u32,
        /// `Field_0x0c`.
        pub field_0c: u8,
    }
}

/// `FLPACKET_CLIENT_REQCOLLISIONGROUPS`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ReqCollisionGroups {
    /// The groups, at most 65 535.
    pub groups: Vec<CollisionGroup>,
}

impl Body for ReqCollisionGroups {
    const ID: u32 = 0x30036;
    const NAME: &'static str = "FLPACKET_CLIENT_REQCOLLISIONGROUPS";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        CollisionGroup::put_list(writer, &self.groups)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            groups: CollisionGroup::get_list(reader)?,
        })
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_COMMCOMPLETE`.
    pub struct CommComplete = 0x30037, "FLPACKET_CLIENT_COMMCOMPLETE" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
        /// `Field_0x08`.
        pub field_08: u32,
        /// `Field_0x0c`.
        pub field_0c: u32,
    }
}

/// `FLPACKET_CLIENT_CREATENEWCHAR`: the name and four choices.
///
/// The server's decoder keeps 23 units of the name (`wcsncpy` into 24);
/// that is the receiver's business, the wire carries the whole name. The
/// C++ decoder misreads the fields after an empty name (it does not move
/// past the count); this one does not (divergence D108).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CreateNewChar {
    /// The name, at most 65 535 units.
    pub name: Vec<u16>,
    /// `Field_0x80`, the faction.
    pub faction: u32,
    /// `Field_0x84`, the base.
    pub base: u32,
    /// `Field_0x88`, the package.
    pub package: u32,
    /// `Field_0x8c`, the pilot.
    pub pilot: u32,
}

impl Body for CreateNewChar {
    const ID: u32 = 0x30039;
    const NAME: &'static str = "FLPACKET_CLIENT_CREATENEWCHAR";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_wide_u16(&self.name)?;
        writer.put_u32(self.faction)?;
        writer.put_u32(self.base)?;
        writer.put_u32(self.package)?;
        writer.put_u32(self.pilot)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            name: reader.get_wide_u16()?,
            faction: reader.get_u32()?,
            base: reader.get_u32()?,
            package: reader.get_u32()?,
            pilot: reader.get_u32()?,
        })
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_GFGOODVAPORIZED`.
    pub struct GfGoodVaporized = 0x3003c, "FLPACKET_CLIENT_GFGOODVAPORIZED" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
        /// `Field_0x08`.
        pub field_08: u32,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_BADLANDSOBJCOLLISION`.
    pub struct BadlandsObjCollision = 0x3003d, "FLPACKET_CLIENT_BADLANDSOBJCOLLISION" {
        /// The object.
        pub object_id: u32,
        /// `Field_0x08`.
        pub field_08: u16,
        /// `Field_0x00`.
        pub field_00: u32,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_LAUNCHCOMPLETE`.
    pub struct LaunchComplete = 0x3003e, "FLPACKET_CLIENT_LAUNCHCOMPLETE" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_HAIL`.
    pub struct Hail = 0x3003f, "FLPACKET_CLIENT_HAIL" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
        /// `Field_0x08`.
        pub field_08: u32,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_REQUEST_USE_ITEM`.
    pub struct RequestUseItem = 0x30040, "FLPACKET_CLIENT_REQUEST_USE_ITEM" {
        /// The ship.
        pub object_id: u32,
        /// The item.
        pub item_id: u16,
        /// How many.
        pub count: u16,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_JUMPINCOMPLETE`.
    pub struct JumpInComplete = 0x30043, "FLPACKET_CLIENT_JUMPINCOMPLETE" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
    }
}

wire_message! {
    /// `FLPACKET_CLIENT_REQINVINCIBILITY`.
    pub struct ReqInvincibility = 0x30044, "FLPACKET_CLIENT_REQINVINCIBILITY" {
        /// The ship.
        pub ship_id: u32,
        /// The reason byte.
        pub reason: u8,
        /// The raw invincible byte.
        pub invincible: u8,
    }
}

catalog! {
    /// A channel 3 message.
    pub enum ClientMessage {
        Login,
        MunCollision,
        RequestLaunch,
        RequestCharInfo,
        SelectCharacter,
        EnterBase,
        RequestBaseInfo,
        RequestLocationInfo,
        GfRequestShipInfo,
        SystemSwitchOutComplete,
        ObjCollision,
        ExitBase,
        EnterLocation,
        ExitLocation,
        RequestCreateShip,
        GfGoodSell,
        GfGoodBuy,
        GfSelectObject,
        MissionResponse,
        ReqShipArch,
        ReqEquipment,
        ReqCargo,
        ReqAddItem,
        ReqRemoveItem,
        ReqModifyItem,
        ReqSetCash,
        ReqChangeCash,
        SaveGame,
        MineAsteroid,
        DbgCreateShip,
        DbgLoadSystem,
        Dock,
        DbgDestroyObject,
        TradeResponse,
        CargoScan,
        DbgConsole,
        DbgFreeSystem,
        SetManeuver,
        DbgRelocateShip,
        RequestEvent,
        RequestCancel,
        InterfaceItemUsed,
        ReqCollisionGroups,
        CommComplete,
        RequestNewCharInfo,
        CreateNewChar,
        DestroyChar,
        ReqHullStatus,
        GfGoodVaporized,
        BadlandsObjCollision,
        LaunchComplete,
        Hail,
        RequestUseItem,
        AbortMission,
        SkipAutosave,
        JumpInComplete,
        ReqInvincibility,
        MissionSaveB,
        ReqDifficultyScale,
        RtcDone,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn catalog_size() {
        assert_eq!(ClientMessage::IDS.len(), 60);
        let mut ids = ClientMessage::IDS.to_vec();
        ids.sort_unstable();
        ids.dedup();
        assert_eq!(ids.len(), 60);
    }

    #[test]
    fn login_lowercases_on_encode() {
        let login = Login {
            create_flag: 1,
            account_name: vec![0x41, 0x7a, 0xc0],
            second: vec![],
        };
        let bytes = login.to_wire(&WireContext::CLIENT).unwrap();
        assert_eq!(bytes, [0x01, 0x03, 1, 3, 0, 0x61, 0, 0x7a, 0, 0xc0, 0]);
    }

    #[test]
    fn character_ids_are_bounded() {
        let long = SelectCharacter {
            character_id: vec![b'a'; 512],
        };
        assert!(long.to_wire(&WireContext::CLIENT).is_err());
        let bytes = [0x06, 0x03, 0x00, 0x02];
        assert!(ClientMessage::from_wire(&WireContext::CLIENT, &bytes).is_err());
    }
}
