//! Channel 2: the 81 messages from the server to a client
//! (`include/rpc_server_channel_codec.h`,
//! `RemoteCommon/remote_server_channel_codecs.cpp`).
//!
//! Four messages have a single-player form ([`WireContext`]):
//! [`CreateShip`], [`CreateSolar`], [`CreateLoot`] and [`CreateMine`], and
//! [`SetReputation`] differs between single player and a multiplayer
//! server. The scalar messages carry one field named `value`; fields the
//! C++ names by offset keep that name.

use super::costume::Costume;
use super::field::{count_fits, count_u8, count_u16, count_u32, get_list, put_units};
use super::records::{CollisionGroup, CreateObject, DamageList, EquipRecord};
use super::to_server::{get_character_id, put_character_id};
use super::{Body, WireContext, WireError, WireReader, WireWriter};

scalar_messages! {
    /// `FLPACKET_SERVER_CONNECTRESPONSE`: the client's ID.
    ConnectResponse = 0x20001, "FLPACKET_SERVER_CONNECTRESPONSE", u32;
    /// `FLPACKET_SERVER_LOGINRESPONSE`.
    LoginResponse = 0x20002, "FLPACKET_SERVER_LOGINRESPONSE", u8;
    /// `FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST`.
    GfCompleteNewsBroadcastList = 0x2000e, "FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST", u32;
    /// `FLPACKET_SERVER_GFCOMPLETECHARLIST`.
    GfCompleteCharList = 0x2000f, "FLPACKET_SERVER_GFCOMPLETECHARLIST", u32;
    /// `FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST`.
    GfCompleteMissionComputerList = 0x20010, "FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST", u32;
    /// `FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST`.
    GfCompleteScriptBehaviorList = 0x20011, "FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST", u32;
    /// `FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST`.
    GfCompleteAmbientScriptList = 0x20013, "FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST", u32;
    /// `FLPACKET_SERVER_SETSHIPARCH`.
    SetShipArch = 0x20023, "FLPACKET_SERVER_SETSHIPARCH", u32;
    /// `FLPACKET_SERVER_SETCASH`.
    SetCash = 0x20030, "FLPACKET_SERVER_SETCASH", u32;
    /// `FLPACKET_SERVER_SETNUMKILLS`.
    SetNumKills = 0x2003e, "FLPACKET_SERVER_SETNUMKILLS", u32;
    /// `FLPACKET_SERVER_SETMISSIONSUCCESSES`.
    SetMissionSuccesses = 0x2003f, "FLPACKET_SERVER_SETMISSIONSUCCESSES", u32;
    /// `FLPACKET_SERVER_SETMISSIONFAILURES`.
    SetMissionFailures = 0x20040, "FLPACKET_SERVER_SETMISSIONFAILURES", u32;
    /// `FLPACKET_SERVER_SET_STORY_CUE`.
    SetStoryCue = 0x20043, "FLPACKET_SERVER_SET_STORY_CUE", u32;
    /// `FLPACKET_SERVER_SETHULLSTATUS`.
    SetHullStatus = 0x20049, "FLPACKET_SERVER_SETHULLSTATUS", f32 => x87;
    /// `FLPACKET_SERVER_SENDNNMESSAGE`.
    SendNnMessage = 0x20056, "FLPACKET_SERVER_SENDNNMESSAGE", u32;
    /// `FLPACKET_SERVER_PLAY_SOUND_EFFECT`.
    PlaySoundEffect = 0x20059, "FLPACKET_SERVER_PLAY_SOUND_EFFECT", u32;
    /// `FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY`.
    GfMissionVendorWhyEmpty = 0x2005a, "FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY", u8;
    /// `FLPACKET_SERVER_MISSIONSAVEA`.
    MissionSaveA = 0x2005b, "FLPACKET_SERVER_MISSIONSAVEA", u32;
}

empty_messages! {
    /// `FLPACKET_SERVER_REVERT_CAMERA`.
    RevertCamera = 0x2004c, "FLPACKET_SERVER_REVERT_CAMERA";
    /// `FLPACKET_SERVER_CANCEL_MUSIC`.
    CancelMusic = 0x20058, "FLPACKET_SERVER_CANCEL_MUSIC";
}

equip_list_messages! {
    /// `FLPACKET_SERVER_SETEQUIPMENT`.
    SetEquipment = 0x20024, "FLPACKET_SERVER_SETEQUIPMENT";
    /// `FLPACKET_SERVER_SETCARGO`.
    SetCargo = 0x20025, "FLPACKET_SERVER_SETCARGO";
}

buffer_message! {
    /// `FLPACKET_SERVER_GFSCRIPTBEHAVIOR`.
    pub struct GfScriptBehavior = 0x2001a, "FLPACKET_SERVER_GFSCRIPTBEHAVIOR";
}
buffer_message! {
    /// `FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER`.
    pub struct GfUpdateMissionComputer = 0x2001d, "FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER";
}
buffer_message! {
    /// `FLPACKET_SERVER_GFUPDATENEWSBROADCAST`.
    pub struct GfUpdateNewsBroadcast = 0x2001e, "FLPACKET_SERVER_GFUPDATENEWSBROADCAST";
}
buffer_message! {
    /// `FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT`.
    pub struct GfUpdateAmbientScript = 0x2001f, "FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT";
}
buffer_message! {
    /// `FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE`.
    pub struct GfMissionVendorAcceptance = 0x20020, "FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE";
}
buffer_message! {
    /// `FLPACKET_SERVER_GFUPDATECHAR`.
    pub struct GfUpdateChar = 0x20026, "FLPACKET_SERVER_GFUPDATECHAR";
}

/// Declares the two-dword messages (`FRpcClientDwordPairPacket`).
macro_rules! pair_messages {
    ($($(#[$meta:meta])* $name:ident = $id:literal, $cname:literal;)*) => {
        $(
            wire_message! {
                $(#[$meta])*
                pub struct $name = $id, $cname {
                    /// `Field_0x00`.
                    pub field_00: u32,
                    /// `Field_0x04`.
                    pub field_04: u32,
                }
            }
        )*
    };
}

pair_messages! {
    /// `FLPACKET_SERVER_SETSTARTROOM`.
    SetStartRoom = 0x2000d, "FLPACKET_SERVER_SETSTARTROOM";
    /// `FLPACKET_SERVER_GFDESTROYNEWSBROADCAST`.
    GfDestroyNewsBroadcast = 0x20014, "FLPACKET_SERVER_GFDESTROYNEWSBROADCAST";
    /// `FLPACKET_SERVER_GFDESTROYCHARACTER`.
    GfDestroyCharacter = 0x20015, "FLPACKET_SERVER_GFDESTROYCHARACTER";
    /// `FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER`.
    GfDestroyMissionComputer = 0x20016, "FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER";
    /// `FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR`.
    GfDestroyScriptBehavior = 0x20017, "FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR";
    /// `FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT`.
    GfDestroyAmbientScript = 0x20019, "FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT";
    /// `FLPACKET_SERVER_SYSTEM_SWITCH_OUT`.
    SystemSwitchOut = 0x20021, "FLPACKET_SERVER_SYSTEM_SWITCH_OUT";
    /// `FLPACKET_SERVER_SCANNOTIFY`.
    ScanNotify = 0x20039, "FLPACKET_SERVER_SCANNOTIFY";
}

wire_message! {
    /// `FLPACKET_SERVER_LAND`.
    pub struct Land = 0x2000b, "FLPACKET_SERVER_LAND" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
        /// `Field_0x08`.
        pub field_08: u32,
    }
}

wire_message! {
    /// `FLPACKET_SERVER_SETREMOVEITEM`.
    pub struct SetRemoveItem = 0x2002f, "FLPACKET_SERVER_SETREMOVEITEM" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`.
        pub field_04: u32,
        /// `Field_0x08`.
        pub field_08: u32,
    }
}

/// One saved character of `CHARACTERINFO` (`FRpcServerCharacterRecord`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CharacterRecord {
    /// The character ID (save file name).
    pub character_id: Vec<u8>,
    /// `bIsRestart`, raw.
    pub is_restart: u8,
    /// `bIsAutoSave`, raw.
    pub is_auto_save: u8,
    /// The character's name, at most 65 535 units.
    pub name: Vec<u16>,
    /// The save's description, at most 65 535 units.
    pub description: Vec<u16>,
    /// `DescripStrId`.
    pub descrip_str_id: u32,
    /// The timestamp's high dword (sent before the low one).
    pub timestamp_high: u32,
    /// The timestamp's low dword.
    pub timestamp_low: u32,
    /// The ship archetype.
    pub ship_archetype: u32,
    /// The money.
    pub money: u32,
    /// The system.
    pub system_id: u32,
    /// The base.
    pub base_id: u32,
    /// The location.
    pub location_id: u32,
    /// The voice.
    pub voice_id: u32,
    /// The rank.
    pub rank: u32,
    /// `Field_0x24c`, always 0 from the server.
    pub field_24c: u32,
    /// The hull status, through the x87 stack.
    pub hull_status: f32,
    /// `NumKills`.
    pub num_kills: u32,
    /// `NumMissionFailures`.
    pub num_mission_failures: u32,
    /// `NumMissionSuccesses`.
    pub num_mission_successes: u32,
    /// `ComCostume`.
    pub com_costume: Option<Costume>,
    /// `BaseCostume`.
    pub base_costume: Option<Costume>,
    /// The equipment, at most 255 records.
    pub equipment: Vec<EquipRecord>,
    /// The collision groups.
    pub collision_groups: Vec<CollisionGroup>,
}

fn put_wide_text(writer: &mut WireWriter, units: &[u16]) -> Result<(), WireError> {
    writer.put_wide_u16(units)
}

fn put_costume_block(writer: &mut WireWriter, costume: Option<&Costume>) -> Result<(), WireError> {
    match costume {
        None => writer.put_u8(0),
        Some(costume) => {
            writer.put_u8(1)?;
            costume.put_id_block(writer)
        }
    }
}

fn get_costume_block(reader: &mut WireReader<'_>) -> Result<Option<Costume>, WireError> {
    if reader.get_u8()? == 0 {
        Ok(None)
    } else {
        Costume::get_id_block(reader).map(Some)
    }
}

impl CharacterRecord {
    fn put(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        put_character_id(writer, &self.character_id)?;
        writer.put_u8(self.is_restart)?;
        writer.put_u8(self.is_auto_save)?;
        put_wide_text(writer, &self.name)?;
        put_wide_text(writer, &self.description)?;
        for value in [
            self.descrip_str_id,
            self.timestamp_high,
            self.timestamp_low,
            self.ship_archetype,
            self.money,
            self.system_id,
            self.base_id,
            self.location_id,
            self.voice_id,
            self.rank,
            self.field_24c,
        ] {
            writer.put_u32(value)?;
        }
        writer.put_f32_x87(self.hull_status)?;
        writer.put_u32(self.num_kills)?;
        writer.put_u32(self.num_mission_failures)?;
        writer.put_u32(self.num_mission_successes)?;
        put_costume_block(writer, self.com_costume.as_ref())?;
        put_costume_block(writer, self.base_costume.as_ref())?;
        writer.put_u8(count_u8(self.equipment.len())?)?;
        for record in &self.equipment {
            record.put(writer)?;
        }
        CollisionGroup::put_list_u32(writer, &self.collision_groups)
    }

    fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let character_id = get_character_id(reader)?;
        let is_restart = reader.get_u8()?;
        let is_auto_save = reader.get_u8()?;
        let name = reader.get_wide_u16()?;
        let description = reader.get_wide_u16()?;
        let mut dwords = [0; 11];
        for value in &mut dwords {
            *value = reader.get_u32()?;
        }
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
        let hull_status = reader.get_f32()?;
        let num_kills = reader.get_u32()?;
        let num_mission_failures = reader.get_u32()?;
        let num_mission_successes = reader.get_u32()?;
        let com_costume = get_costume_block(reader)?;
        let base_costume = get_costume_block(reader)?;
        let equipment_count = reader.get_u8()?;
        let equipment = get_list(reader, u32::from(equipment_count), 18, EquipRecord::get)?;
        let collision_groups = CollisionGroup::get_list_u32(reader)?;
        Ok(Self {
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
        })
    }
}

/// `FLPACKET_SERVER_CHARACTERINFO`: the account's characters.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CharacterInfo {
    /// The characters, at most 255.
    pub records: Vec<CharacterRecord>,
    /// `SaveFailureCount`.
    pub save_failure_count: u32,
}

impl Body for CharacterInfo {
    const ID: u32 = 0x20003;
    const NAME: &'static str = "FLPACKET_SERVER_CHARACTERINFO";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u8(count_u8(self.records.len())?)?;
        for record in &self.records {
            record.put(writer)?;
        }
        writer.put_u32(self.save_failure_count)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let count = reader.get_u8()?;
        let records = get_list(reader, u32::from(count), 80, CharacterRecord::get)?;
        Ok(Self {
            records,
            save_failure_count: reader.get_u32()?,
        })
    }
}

/// The owner part of [`CreateShip`] (flag `0x04`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CreateShipOwner {
    /// The owning player's low byte.
    pub player: u8,
    /// `Field_0xc4`, a word.
    pub field_c4: u16,
    /// The display name; multiplayer only, at most 127 units.
    pub display_name: Vec<u16>,
}

/// `FLPACKET_SERVER_CREATESHIP`: the create-object block and the ship's
/// own tail.
///
/// Single player leaves out the display name, the two name blocks, the
/// affiliation and the attitude (`rpclocal`'s payload carries
/// them from the shared world instead).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CreateShip {
    /// The create-object block.
    pub object: CreateObject,
    /// `bUsingTradelane` (flag `0x01`).
    pub using_tradelane: bool,
    /// `bMarkedBy` (flag `0x02`).
    pub marked_by: bool,
    /// The velocity, raw floats.
    pub velocity: [f32; 3],
    /// The throttle × 127 byte.
    pub throttle: u8,
    /// `Field_0xc0`, a word.
    pub field_c0: u16,
    /// The reputation rank's low byte.
    pub reputation_rank: u8,
    /// The owning player, when there is one (flag `0x04`).
    pub owner: Option<CreateShipOwner>,
    /// Multiplayer without an owner: the two flattened name format strings.
    pub names: (Vec<u8>, Vec<u8>),
    /// Multiplayer: the affiliation.
    pub affiliation: u32,
    /// Multiplayer: the attitude × 127 byte.
    pub attitude: u8,
}

impl Body for CreateShip {
    const ID: u32 = 0x20004;
    const NAME: &'static str = "FLPACKET_SERVER_CREATESHIP";

    fn encode_body(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        self.object.put(ctx, writer)?;
        let mut flags = u8::from(self.using_tradelane);
        if self.owner.is_some() {
            flags |= 4;
        }
        if self.marked_by {
            flags |= 2;
        }
        writer.put_u8(flags)?;
        writer.put_vector(self.velocity)?;
        writer.put_u8(self.throttle)?;
        writer.put_u16(self.field_c0)?;
        writer.put_u8(self.reputation_rank)?;
        if let Some(owner) = &self.owner {
            writer.put_u8(owner.player)?;
            writer.put_u16(owner.field_c4)?;
            if !ctx.single_player {
                let length = count_fits(owner.display_name.len(), 0x7f)?;
                writer.put_u8(count_u8(length)?)?;
                put_units(writer, &owner.display_name)?;
            }
        } else if !ctx.single_player {
            writer.put_bytes_u32(&self.names.0)?;
            writer.put_bytes_u32(&self.names.1)?;
        }
        if !ctx.single_player {
            writer.put_u32(self.affiliation)?;
            writer.put_u8(self.attitude)?;
        }
        Ok(())
    }

    fn decode_body(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let object = CreateObject::get(ctx, reader)?;
        let flags = reader.get_u8()?;
        let velocity = reader.get_vector()?;
        let throttle = reader.get_u8()?;
        let field_c0 = reader.get_u16()?;
        let reputation_rank = reader.get_u8()?;
        let mut names = (Vec::new(), Vec::new());
        let owner = if flags & 4 != 0 {
            let player = reader.get_u8()?;
            let field_c4 = reader.get_u16()?;
            let display_name = if ctx.single_player {
                Vec::new()
            } else {
                let length = reader.get_u8()?;
                // The C++ sign-extends the length; 0x80 and above fail.
                count_fits(usize::from(length), 0x7f)?;
                reader.get_wide(usize::from(length))?
            };
            Some(CreateShipOwner {
                player,
                field_c4,
                display_name,
            })
        } else {
            if !ctx.single_player {
                names = (
                    reader.get_bytes_u32()?.to_vec(),
                    reader.get_bytes_u32()?.to_vec(),
                );
            }
            None
        };
        let (affiliation, attitude) = if ctx.single_player {
            (0, 0)
        } else {
            (reader.get_u32()?, reader.get_u8()?)
        };
        Ok(Self {
            object,
            using_tradelane: flags & 1 != 0,
            marked_by: flags & 2 != 0,
            velocity,
            throttle,
            field_c0,
            reputation_rank,
            owner,
            names,
            affiliation,
            attitude,
        })
    }
}

/// `FLPACKET_SERVER_DAMAGEOBJECT`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DamageObject {
    /// The object.
    pub object_id: u32,
    /// The damage.
    pub damage: DamageList,
}

impl Body for DamageObject {
    const ID: u32 = 0x20005;
    const NAME: &'static str = "FLPACKET_SERVER_DAMAGEOBJECT";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.object_id)?;
        self.damage.put(writer)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            object_id: reader.get_u32()?,
            damage: DamageList::get(reader)?,
        })
    }
}

wire_message! {
    /// `FLPACKET_SERVER_DESTROYOBJECT`.
    pub struct DestroyObject = 0x20006, "FLPACKET_SERVER_DESTROYOBJECT" {
        /// The object.
        pub object_id: u32,
        /// `Field_0x04`, raw.
        pub field_04: u8,
    }
}

wire_message! {
    /// `FLPACKET_SERVER_LAUNCH`.
    pub struct Launch = 0x20007, "FLPACKET_SERVER_LAUNCH" {
        /// `Field_0x00`, the ship.
        pub field_00: u32,
        /// `Field_0x04`, the launch object.
        pub field_04: u32,
        /// `Field_0x08`, the launch position index.
        pub field_08: u32,
        /// The position.
        pub position: [f32; 3],
        /// The orientation `w`, `x`, `y`, `z`.
        pub orientation: [f32; 4],
    }
}

wire_message! {
    /// `FLPACKET_SERVER_CHARSELECTVERIFIED`.
    pub struct CharSelectVerified = 0x20008, "FLPACKET_SERVER_CHARSELECTVERIFIED" {
        /// The reputation instance.
        pub rep_instance: u32,
        /// The time, a double through the x87 stack.
        pub time: f64 => x87d,
    }
}

wire_message! {
    /// `FLPACKET_SERVER_ACTIVATEOBJECT`.
    pub struct ActivateObject = 0x2000a, "FLPACKET_SERVER_ACTIVATEOBJECT" {
        /// The object.
        pub object_id: u32,
        /// The raw activate byte.
        pub activate: u8,
        /// `Field_0x08`, the activate-state index.
        pub field_08: u32,
    }
}

wire_message! {
    /// `FLPACKET_SERVER_SYSTEM_SWITCH_IN`.
    pub struct SystemSwitchIn = 0x20022, "FLPACKET_SERVER_SYSTEM_SWITCH_IN" {
        /// `Field_0x00`, the object.
        pub field_00: u32,
        /// `Field_0x04`, the jump object type bits.
        pub field_04: u32,
        /// The position.
        pub position: [f32; 3],
        /// The orientation `w`, `x`, `y`, `z`.
        pub orientation: [f32; 4],
    }
}

wire_message! {
    /// `FLPACKET_SERVER_REQUESTCREATESHIPRESP`.
    pub struct RequestCreateShipResp = 0x20027, "FLPACKET_SERVER_REQUESTCREATESHIPRESP" {
        /// `Field_0x00`, the response byte.
        pub field_00: u8,
        /// `Field_0x04`.
        pub field_04: u32,
    }
}

/// One loot record of `CREATELOOT` (`FRpcServerLootRecord`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LootRecord {
    /// The loot object.
    pub loot_id: u32,
    /// The contents' small archetype ID.
    pub contents: u16,
    /// The contents' hit points, through the x87 stack.
    pub contents_hit_points: f32,
    /// The units.
    pub units: u16,
    /// The container's small archetype ID.
    pub container: u16,
    /// The container's hit points, through the x87 stack.
    pub hit_points: f32,
    /// `NameOverride`.
    pub name_override: u32,
    /// The position.
    pub position: [f32; 3],
    /// Single player: the velocity.
    pub velocity: [f32; 3],
    /// Single player: the orientation `w`, `x`, `y`, `z`.
    pub orientation: [f32; 4],
    /// `bMarkedBy`, raw.
    pub marked_by: u8,
    /// `bTemporary`, raw.
    pub temporary: u8,
}

/// `FLPACKET_SERVER_CREATELOOT`. Single player adds each record's velocity
/// and orientation.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CreateLoot {
    /// The owner.
    pub owner_id: u32,
    /// The kind byte.
    pub kind: u8,
    /// The records, at most 255.
    pub loot: Vec<LootRecord>,
}

impl Body for CreateLoot {
    const ID: u32 = 0x20028;
    const NAME: &'static str = "FLPACKET_SERVER_CREATELOOT";

    fn encode_body(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.owner_id)?;
        writer.put_u8(self.kind)?;
        writer.put_u8(count_u8(self.loot.len())?)?;
        for record in &self.loot {
            writer.put_u32(record.loot_id)?;
            writer.put_u16(record.contents)?;
            writer.put_f32_x87(record.contents_hit_points)?;
            writer.put_u16(record.units)?;
            writer.put_u16(record.container)?;
            writer.put_f32_x87(record.hit_points)?;
            writer.put_u32(record.name_override)?;
            writer.put_vector(record.position)?;
            if ctx.single_player {
                writer.put_vector(record.velocity)?;
                writer.put_quaternion(record.orientation)?;
            }
            writer.put_u8(record.marked_by)?;
            writer.put_u8(record.temporary)?;
        }
        Ok(())
    }

    fn decode_body(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let owner_id = reader.get_u32()?;
        let kind = reader.get_u8()?;
        let count = reader.get_u8()?;
        let loot = get_list(reader, u32::from(count), 36, |r| {
            let loot_id = r.get_u32()?;
            let contents = r.get_u16()?;
            let contents_hit_points = r.get_f32()?;
            let units = r.get_u16()?;
            let container = r.get_u16()?;
            let hit_points = r.get_f32()?;
            let name_override = r.get_u32()?;
            let position = r.get_vector()?;
            let (velocity, orientation) = if ctx.single_player {
                (r.get_vector()?, r.get_quaternion()?)
            } else {
                ([0.0; 3], [1.0, 0.0, 0.0, 0.0])
            };
            Ok(LootRecord {
                loot_id,
                contents,
                contents_hit_points,
                units,
                container,
                hit_points,
                name_override,
                position,
                velocity,
                orientation,
                marked_by: r.get_u8()?,
                temporary: r.get_u8()?,
            })
        })?;
        Ok(Self {
            owner_id,
            kind,
            loot,
        })
    }
}

/// A reputation relation (`Reputation::Relation`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Relation {
    /// The feeling, through the x87 stack.
    pub feeling: f32,
    /// The group.
    pub group_id: u32,
}

/// `FLPACKET_SERVER_SETREPUTATION`.
///
/// A flag byte (rebuilt on every encode), the target, the affiliation when
/// it is not `0xFFFFFFFF`. In single player the display name, the two
/// flattened name strings (sent when the first dword of either is not
/// zero) and the relations follow; a multiplayer server sends the attitude
/// instead.
#[derive(Clone, Debug, PartialEq)]
pub struct SetReputation {
    /// The target.
    pub target: u32,
    /// The affiliation, `0xFFFFFFFF` for none.
    pub affiliation: u32,
    /// Single player: the display name as the server keeps it, its
    /// terminator included; at most 127 units.
    pub display_name: Vec<u16>,
    /// Single player: the two flattened name format strings.
    pub names: (Vec<u8>, Vec<u8>),
    /// Single player: the relations.
    pub relations: Vec<Relation>,
    /// Multiplayer: the attitude, through the x87 stack.
    pub attitude: f32,
}

impl Default for SetReputation {
    fn default() -> Self {
        Self {
            target: 0,
            affiliation: 0xffff_ffff,
            display_name: Vec::new(),
            names: (Vec::new(), Vec::new()),
            relations: Vec::new(),
            attitude: 0.0,
        }
    }
}

fn first_dword(block: &[u8]) -> u32 {
    let mut word = [0; 4];
    for (out, byte) in word.iter_mut().zip(block) {
        *out = *byte;
    }
    u32::from_le_bytes(word)
}

impl Body for SetReputation {
    const ID: u32 = 0x20029;
    const NAME: &'static str = "FLPACKET_SERVER_SETREPUTATION";

    fn encode_body(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        let mut flags = u8::from(self.affiliation != 0xffff_ffff);
        let names = ctx.single_player
            && (first_dword(&self.names.0) != 0 || first_dword(&self.names.1) != 0);
        let relations = if ctx.single_player {
            self.relations.len()
        } else {
            0
        };
        if ctx.single_player {
            if !self.display_name.is_empty() {
                flags |= 4;
            }
            if names {
                flags |= 2;
            }
            if relations > 0xff {
                flags |= 0x10;
            } else if relations != 0 {
                flags |= 8;
            }
        }
        writer.put_u8(flags)?;
        writer.put_u32(self.target)?;
        if flags & 1 != 0 {
            writer.put_u32(self.affiliation)?;
        }
        if flags & 4 != 0 {
            let length = count_fits(self.display_name.len(), 0x7f)?;
            writer.put_u8(count_u8(length)?)?;
            put_units(writer, &self.display_name)?;
        }
        if names {
            writer.put_bytes_u32(&self.names.0)?;
            writer.put_bytes_u32(&self.names.1)?;
        }
        if ctx.encoder_is_mp_server() {
            writer.put_f32_x87(self.attitude)?;
        } else if relations != 0 {
            if flags & 0x10 != 0 {
                writer.put_u16(count_u16(relations)?)?;
            } else {
                writer.put_u8(count_u8(relations)?)?;
            }
            for relation in &self.relations {
                writer.put_f32_x87(relation.feeling)?;
                writer.put_u32(relation.group_id)?;
            }
        }
        Ok(())
    }

    fn decode_body(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let flags = reader.get_u8()?;
        let mut message = Self {
            target: reader.get_u32()?,
            ..Self::default()
        };
        if flags & 1 != 0 {
            message.affiliation = reader.get_u32()?;
        }
        if ctx.single_player {
            if flags & 4 != 0 {
                let length = reader.get_u8()?;
                count_fits(usize::from(length), 0x7f)?;
                message.display_name = reader.get_wide(usize::from(length))?;
            }
            if flags & 2 != 0 {
                message.names = (
                    reader.get_bytes_u32()?.to_vec(),
                    reader.get_bytes_u32()?.to_vec(),
                );
            }
            if flags & 0x18 != 0 {
                let count = if flags & 0x10 != 0 {
                    u32::from(reader.get_u16()?)
                } else {
                    u32::from(reader.get_u8()?)
                };
                message.relations = get_list(reader, count, 8, |r| {
                    Ok(Relation {
                        feeling: r.get_f32()?,
                        group_id: r.get_u32()?,
                    })
                })?;
            }
        } else {
            message.attitude = reader.get_f32()?;
        }
        Ok(message)
    }
}

wire_message! {
    /// `FLPACKET_SERVER_ADJUSTATTITUDE`.
    pub struct AdjustAttitude = 0x2002a, "FLPACKET_SERVER_ADJUSTATTITUDE" {
        /// `Field_0x00`, the group.
        pub field_00: u32,
        /// `Field_0x04`, the change, through the x87 stack.
        pub field_04: f32 => x87,
    }
}

/// `FLPACKET_SERVER_SETGROUPFEELINGS`: a group name and its relations.
///
/// No count travels: the C++ decoder reads a count the encoder never
/// writes. This decoder reads relations up to the end of the message
/// (divergence D109).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SetGroupFeelings {
    /// The name as the server keeps it, terminator included.
    pub name: Vec<u8>,
    /// The relations, at most 65 535.
    pub relations: Vec<Relation>,
}

impl Body for SetGroupFeelings {
    const ID: u32 = 0x2002b;
    const NAME: &'static str = "FLPACKET_SERVER_SETGROUPFEELINGS";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        count_u16(self.relations.len())?;
        writer.put_bytes_u32(&self.name)?;
        for relation in &self.relations {
            writer.put_f32_x87(relation.feeling)?;
            writer.put_u32(relation.group_id)?;
        }
        Ok(())
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let name = reader.get_bytes_u32()?.to_vec();
        let mut relations = Vec::new();
        while !reader.is_empty() {
            relations.push(Relation {
                feeling: reader.get_f32()?,
                group_id: reader.get_u32()?,
            });
        }
        Ok(Self { name, relations })
    }
}

/// `FLPACKET_SERVER_CREATEMINE`. Single player adds the orientation, the
/// velocity and a third vector.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CreateMine {
    /// The mine object.
    pub obj_id: u32,
    /// The small archetype ID.
    pub archetype: u16,
    /// The owner.
    pub owner_id: u32,
    /// The launching sub-object.
    pub sub_obj_id: u16,
    /// The position.
    pub position: [f32; 3],
    /// Single player: the orientation `w`, `x`, `y`, `z`.
    pub orientation: [f32; 4],
    /// Single player: the velocity.
    pub velocity: [f32; 3],
    /// Single player: `Field_0x38`.
    pub field_38: [f32; 3],
}

impl Body for CreateMine {
    const ID: u32 = 0x2002c;
    const NAME: &'static str = "FLPACKET_SERVER_CREATEMINE";

    fn encode_body(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.obj_id)?;
        writer.put_u16(self.archetype)?;
        writer.put_u32(self.owner_id)?;
        writer.put_u16(self.sub_obj_id)?;
        writer.put_vector(self.position)?;
        if ctx.single_player {
            writer.put_quaternion(self.orientation)?;
            writer.put_vector(self.velocity)?;
            writer.put_vector(self.field_38)?;
        }
        Ok(())
    }

    fn decode_body(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let obj_id = reader.get_u32()?;
        let archetype = reader.get_u16()?;
        let owner_id = reader.get_u32()?;
        let sub_obj_id = reader.get_u16()?;
        let position = reader.get_vector()?;
        let (orientation, velocity, field_38) = if ctx.single_player {
            (
                reader.get_quaternion()?,
                reader.get_vector()?,
                reader.get_vector()?,
            )
        } else {
            ([1.0, 0.0, 0.0, 0.0], [0.0; 3], [0.0; 3])
        };
        Ok(Self {
            obj_id,
            archetype,
            owner_id,
            sub_obj_id,
            position,
            orientation,
            velocity,
            field_38,
        })
    }
}

wire_message! {
    /// `FLPACKET_SERVER_CREATECOUNTER`: a countermeasure.
    pub struct CreateCounter = 0x2002d, "FLPACKET_SERVER_CREATECOUNTER" {
        /// The archetype.
        pub archetype: u32,
        /// The object.
        pub obj_id: u32,
        /// `Field_0x08`.
        pub field_08: u32,
        /// The owner.
        pub owner_id: u32,
        /// The launching sub-object.
        pub sub_obj_id: u16,
        /// The orientation `w`, `x`, `y`, `z`.
        pub orientation: [f32; 4],
        /// The position.
        pub position: [f32; 3],
        /// The velocity.
        pub velocity: [f32; 3],
        /// `Field_0x3c`.
        pub field_3c: [f32; 3],
    }
}

/// `FLPACKET_SERVER_SETADDITEM`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SetAddItem {
    /// The archetype.
    pub arch_id: u32,
    /// The item ID.
    pub id: u32,
    /// The count.
    pub count: i32,
    /// The health, through the x87 stack.
    pub health: f32,
    /// `bEquipped`, raw.
    pub equipped: u8,
    /// `bMission`, raw.
    pub mission: u8,
    /// `Field_0x0c`, the add result.
    pub field_0c: u32,
    /// The hardpoint, terminator included; empty for internal items.
    pub hardpoint: Vec<u8>,
}

impl Body for SetAddItem {
    const ID: u32 = 0x2002e;
    const NAME: &'static str = "FLPACKET_SERVER_SETADDITEM";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.arch_id)?;
        writer.put_u32(self.id)?;
        writer.put_i32(self.count)?;
        writer.put_f32_x87(self.health)?;
        writer.put_u8(self.equipped)?;
        writer.put_u8(self.mission)?;
        writer.put_u32(self.field_0c)?;
        writer.put_bytes_u32(&self.hardpoint)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            arch_id: reader.get_u32()?,
            id: reader.get_u32()?,
            count: reader.get_i32()?,
            health: reader.get_f32()?,
            equipped: reader.get_u8()?,
            mission: reader.get_u8()?,
            field_0c: reader.get_u32()?,
            hardpoint: reader.get_bytes_u32()?.to_vec(),
        })
    }
}

wire_message! {
    /// `FLPACKET_SERVER_EXPLODEASTEROIDMINE`.
    pub struct ExplodeAsteroidMine = 0x20031, "FLPACKET_SERVER_EXPLODEASTEROIDMINE" {
        /// `Field_0x00`, the object.
        pub field_00: u32,
        /// The position.
        pub position: [f32; 3],
    }
}

/// `FLPACKET_SERVER_REQUESTSPACESCRIPT`: an object and a script name,
/// terminator included.
///
/// In single player the position and the mode byte follow, as rpclocal's
/// payload carries them (`rpc_payload_ch2.cpp`, `MarshalRequestSpaceScript`
/// and `DeliverRequestSpaceScript`); a multiplayer packet holds them in its
/// object only (`FRpcServerRequestSpaceScriptPacket`, "not on the wire"), so
/// its receiver reads the constructor's zeros and mode 1.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RequestSpaceScript {
    /// `Field_0x00`, the object.
    pub field_00: u32,
    /// The name.
    pub name: Vec<u8>,
    /// Single player: the position's float bits (`Field_0x108` to
    /// `Field_0x110` of the IClient argument).
    pub position: [u32; 3],
    /// Single player: the mode byte, 1 for the argument's priority 1 or any
    /// value but 2, 0 for 2; the client reads 0 as mode 2 and any other
    /// byte as mode 1.
    pub mode: u8,
}

impl Default for RequestSpaceScript {
    fn default() -> Self {
        Self {
            field_00: 0,
            name: Vec::new(),
            position: [0; 3],
            mode: 1,
        }
    }
}

impl RequestSpaceScript {
    /// The sender's mode byte for the argument's `Field_0x114` (the
    /// priority): 0 for 2, else 1.
    #[must_use]
    pub fn mode_byte(priority: u32) -> u8 {
        u8::from(priority != 2)
    }

    /// The receiver's `Field_0x114`: 2 for a mode byte of 0, else 1.
    #[must_use]
    pub fn priority(&self) -> u32 {
        if self.mode == 0 { 2 } else { 1 }
    }
}

impl Body for RequestSpaceScript {
    const ID: u32 = 0x20032;
    const NAME: &'static str = "FLPACKET_SERVER_REQUESTSPACESCRIPT";

    fn encode_body(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.field_00)?;
        writer.put_bytes_u32(&self.name)?;
        if ctx.single_player {
            for word in self.position {
                writer.put_u32(word)?;
            }
            writer.put_u8(self.mode)?;
        }
        Ok(())
    }

    fn decode_body(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let mut message = Self {
            field_00: reader.get_u32()?,
            name: reader.get_bytes_u32()?.to_vec(),
            ..Self::default()
        };
        if ctx.single_player {
            for word in &mut message.position {
                *word = reader.get_u32()?;
            }
            message.mode = reader.get_u8()?;
        }
        Ok(message)
    }
}

wire_message! {
    /// `FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE`.
    pub struct SetMissionObjectiveState = 0x20033, "FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE" {
        /// `Field_0x00`.
        pub field_00: u32,
        /// `Field_0x04`, a byte widened.
        pub field_04: u32,
        /// `Field_0x08`.
        pub field_08: u16,
    }
}

/// `FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ReplaceMissionObjective {
    /// `Field_0x00`, the mission.
    pub field_00: u32,
    /// The state.
    pub state: u32,
    /// The objective, a flattened format string.
    pub text: Vec<u8>,
}

impl Body for ReplaceMissionObjective {
    const ID: u32 = 0x20034;
    const NAME: &'static str = "FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.field_00)?;
        writer.put_u32(self.state)?;
        writer.put_bytes_u32(&self.text)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            field_00: reader.get_u32()?,
            state: reader.get_u32()?,
            text: reader.get_bytes_u32()?.to_vec(),
        })
    }
}

/// One objective of `SETMISSIONOBJECTIVES`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MissionObjective {
    /// The state byte.
    pub state: u8,
    /// The objective, a flattened format string.
    pub text: Vec<u8>,
}

/// `FLPACKET_SERVER_SETMISSIONOBJECTIVES`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SetMissionObjectives {
    /// `Field_0x00`, the mission.
    pub field_00: u32,
    /// The state.
    pub state: u32,
    /// The first text, a flattened format string.
    pub text_a: Vec<u8>,
    /// The second text.
    pub text_b: Vec<u8>,
    /// The objectives; their count travels before the state.
    pub objectives: Vec<MissionObjective>,
}

impl Body for SetMissionObjectives {
    const ID: u32 = 0x20035;
    const NAME: &'static str = "FLPACKET_SERVER_SETMISSIONOBJECTIVES";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.field_00)?;
        writer.put_u32(count_u32(self.objectives.len())?)?;
        writer.put_u32(self.state)?;
        writer.put_bytes_u32(&self.text_a)?;
        writer.put_bytes_u32(&self.text_b)?;
        for objective in &self.objectives {
            writer.put_u8(objective.state)?;
            writer.put_bytes_u32(&objective.text)?;
        }
        Ok(())
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let field_00 = reader.get_u32()?;
        let count = reader.get_u32()?;
        let state = reader.get_u32()?;
        let text_a = reader.get_bytes_u32()?.to_vec();
        let text_b = reader.get_bytes_u32()?.to_vec();
        let objectives = get_list(reader, count, 5, |r| {
            Ok(MissionObjective {
                state: r.get_u8()?,
                text: r.get_bytes_u32()?.to_vec(),
            })
        })?;
        Ok(Self {
            field_00,
            state,
            text_a,
            text_b,
            objectives,
        })
    }
}

wire_message! {
    /// `FLPACKET_SERVER_CREATEGUIDED`: a guided missile.
    pub struct CreateGuided = 0x20037, "FLPACKET_SERVER_CREATEGUIDED" {
        /// The small archetype ID.
        pub archetype: u16,
        /// The owner.
        pub owner_id: u32,
        /// The launching sub-object.
        pub sub_obj_id: u16,
        /// The target.
        pub target_id: u32,
        /// The target's sub-object.
        pub target_sub_obj_id: u16,
        /// The missile object.
        pub obj_id: u32,
        /// The position.
        pub position: [f32; 3],
        /// The orientation bytes `x`, `y`, `z`, `w`.
        pub orientation: [u8; 4],
    }
}

wire_message! {
    /// `FLPACKET_SERVER_ITEMTRACTORED`.
    pub struct ItemTractored = 0x20038, "FLPACKET_SERVER_ITEMTRACTORED" {
        /// The loot object.
        pub loot_id: u32,
        /// The tractoring ship.
        pub ship_id: u32,
        /// The tractor equipment.
        pub tractor_id: u16,
        /// The units taken.
        pub units: u16,
    }
}

wire_message! {
    /// `FLPACKET_SERVER_REPAIROBJECT`.
    pub struct RepairObject = 0x2003c, "FLPACKET_SERVER_REPAIROBJECT" {
        /// The object.
        pub object_id: u32,
        /// The equipment.
        pub equip_id: u32,
        /// The points.
        pub points: i32,
    }
}

/// `FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RemoteObjectCargoUpdate {
    /// The object.
    pub object_id: u32,
    /// The flag byte.
    pub flag: u8,
    /// The records, at most 65 535.
    pub records: Vec<EquipRecord>,
}

impl Body for RemoteObjectCargoUpdate {
    const ID: u32 = 0x2003d;
    const NAME: &'static str = "FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.object_id)?;
        writer.put_u8(self.flag)?;
        EquipRecord::put_list(writer, &self.records)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            object_id: reader.get_u32()?,
            flag: reader.get_u8()?,
            records: EquipRecord::get_list(reader)?,
        })
    }
}

wire_message! {
    /// `FLPACKET_SERVER_BURNFUSE`.
    pub struct BurnFuse = 0x20041, "FLPACKET_SERVER_BURNFUSE" {
        /// The source.
        pub source_id: u32,
        /// The object.
        pub object_id: u32,
        /// The fuse.
        pub fuse_id: u32,
        /// The sub-object.
        pub sub_obj_id: u16,
        /// The start time, through the x87 stack.
        pub at_t: f32 => x87,
        /// The lifetime, through the x87 stack.
        pub lifetime: f32 => x87,
        /// The raw lit byte.
        pub lit: u8,
    }
}

/// `FLPACKET_SERVER_CREATESOLAR`: the create-object block and the solar's
/// tail. Single player leaves out the affiliation, the attitude and the
/// two name strings.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CreateSolar {
    /// The create-object block.
    pub object: CreateObject,
    /// `Field_0x9c`.
    pub field_9c: u8,
    /// `bMarkedBy`, raw.
    pub marked_by: u8,
    /// Multiplayer: the affiliation.
    pub affiliation: u32,
    /// Multiplayer: the attitude × 127 byte.
    pub attitude: u8,
    /// Multiplayer: the two flattened name format strings.
    pub names: (Vec<u8>, Vec<u8>),
}

impl Body for CreateSolar {
    const ID: u32 = 0x20042;
    const NAME: &'static str = "FLPACKET_SERVER_CREATESOLAR";

    fn encode_body(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        self.object.put(ctx, writer)?;
        writer.put_u8(self.field_9c)?;
        writer.put_u8(self.marked_by)?;
        if !ctx.single_player {
            writer.put_u32(self.affiliation)?;
            writer.put_u8(self.attitude)?;
            writer.put_bytes_u32(&self.names.0)?;
            writer.put_bytes_u32(&self.names.1)?;
        }
        Ok(())
    }

    fn decode_body(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let object = CreateObject::get(ctx, reader)?;
        let field_9c = reader.get_u8()?;
        let marked_by = reader.get_u8()?;
        if ctx.single_player {
            return Ok(Self {
                object,
                field_9c,
                marked_by,
                ..Self::default()
            });
        }
        Ok(Self {
            object,
            field_9c,
            marked_by,
            affiliation: reader.get_u32()?,
            attitude: reader.get_u8()?,
            names: (
                reader.get_bytes_u32()?.to_vec(),
                reader.get_bytes_u32()?.to_vec(),
            ),
        })
    }
}

wire_message! {
    /// `FLPACKET_SERVER_REQUEST_RETURNED`.
    pub struct RequestReturned = 0x20044, "FLPACKET_SERVER_REQUEST_RETURNED" {
        /// The requesting object.
        pub object_id: u32,
        /// The request type.
        pub request_type: u8,
        /// The response.
        pub response: u8,
        /// `Field_0x26`.
        pub field_26: u8,
    }
}

/// `FLPACKET_SERVER_SET_MISSION_MESSAGE`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SetMissionMessage {
    /// The message type.
    pub kind: u32,
    /// The text, a flattened format string.
    pub text: Vec<u8>,
}

impl Body for SetMissionMessage {
    const ID: u32 = 0x20045;
    const NAME: &'static str = "FLPACKET_SERVER_SET_MISSION_MESSAGE";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.kind)?;
        writer.put_bytes_u32(&self.text)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            kind: reader.get_u32()?,
            text: reader.get_bytes_u32()?.to_vec(),
        })
    }
}

wire_message! {
    /// `FLPACKET_SERVER_MARKOBJ`.
    pub struct MarkObj = 0x20046, "FLPACKET_SERVER_MARKOBJ" {
        /// The object.
        pub object_id: u32,
        /// The mark type's low byte.
        pub mark_type: u8,
    }
}

wire_message! {
    /// `FLPACKET_SERVER_CFGINTERFACENOTIFICATION`.
    pub struct CfgInterfaceNotification = 0x20047, "FLPACKET_SERVER_CFGINTERFACENOTIFICATION" {
        /// The notification type.
        pub notif_type: u32,
        /// The raw enable byte.
        pub enable: u8,
        /// The parameter.
        pub param: u32,
    }
}

/// `FLPACKET_SERVER_SETCOLLISIONGROUPS`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SetCollisionGroups {
    /// The groups, at most 65 535.
    pub groups: Vec<CollisionGroup>,
}

impl Body for SetCollisionGroups {
    const ID: u32 = 0x20048;
    const NAME: &'static str = "FLPACKET_SERVER_SETCOLLISIONGROUPS";

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
    /// `FLPACKET_SERVER_SETGUIDEDTARGET`.
    pub struct SetGuidedTarget = 0x2004a, "FLPACKET_SERVER_SETGUIDEDTARGET" {
        /// `Field_0x20`.
        pub field_20: u32,
        /// `Field_0x24`.
        pub field_24: u32,
        /// `Field_0x28`.
        pub field_28: u16,
    }
}

wire_message! {
    /// `FLPACKET_SERVER_SET_CAMERA`.
    pub struct SetCamera = 0x2004b, "FLPACKET_SERVER_SET_CAMERA" {
        /// The position.
        pub position: [f32; 3],
        /// The orientation `w`, `x`, `y`, `z`.
        pub orientation: [f32; 4],
        /// The field of view, through the x87 stack.
        pub fov: f32 => x87,
        /// The aspect, through the x87 stack.
        pub aspect: f32 => x87,
    }
}

/// `FLPACKET_SERVER_LOADHINT`: the size travels before the kind.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LoadHint {
    /// `BaseHint::Kind`.
    pub kind: u32,
    /// The bytes.
    pub data: Vec<u8>,
}

impl Body for LoadHint {
    const ID: u32 = 0x2004d;
    const NAME: &'static str = "FLPACKET_SERVER_LOADHINT";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(count_u32(self.data.len())?)?;
        writer.put_u32(self.kind)?;
        writer.put_bytes(&self.data)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let size = reader.get_u32()?;
        let kind = reader.get_u32()?;
        let size = super::field::wire_len(size, 1, reader)?;
        Ok(Self {
            kind,
            data: reader.get_bytes(size)?.to_vec(),
        })
    }
}

/// The directive of `SETDIRECTIVE` (`FPayloadServerSetDirective`), 0x94
/// bytes copied field by field. The padding bytes `+0x09..+0x0b` and
/// `+0x91..+0x93` are never written by the C++ and go out as whatever the
/// encode buffer held; they are zero here (divergence D100).
#[derive(Clone, Debug, PartialEq)]
pub struct Directive {
    /// `+0x00`, the directive kind.
    pub field_00: u32,
    /// `+0x04`.
    pub field_04: u32,
    /// `+0x08`.
    pub field_08: u8,
    /// `+0x0c`, the delay.
    pub field_0c: f32,
    /// `+0x10`, the dock object.
    pub field_10: u32,
    /// `+0x14`.
    pub field_14: i32,
    /// `+0x18`, the dock port.
    pub field_18: i32,
    /// `+0x1c`.
    pub field_1c: u32,
    /// `+0x20`.
    pub field_20: f32,
    /// `+0x24`.
    pub field_24: f32,
    /// `+0x28`, the follow target.
    pub field_28: u32,
    /// `+0x2c`, the follow distance.
    pub field_2c: f32,
    /// `+0x30`, the follow offset.
    pub field_30: [f32; 3],
    /// `+0x3c`.
    pub field_3c: f32,
    /// `+0x40`.
    pub field_40: i32,
    /// `+0x44`, the destination.
    pub field_44: [f32; 3],
    /// `+0x50`.
    pub field_50: i32,
    /// `+0x54`, `+0x60`, `+0x6c`, `+0x78`: four three-dword blocks.
    pub blocks: [[u32; 3]; 4],
    /// `+0x84`.
    pub field_84: f32,
    /// `+0x88`.
    pub field_88: f32,
    /// `+0x8c` to `+0x90`.
    pub tail: [u8; 5],
}

impl Default for Directive {
    fn default() -> Self {
        Self {
            field_00: 0,
            field_04: 0,
            field_08: 0,
            field_0c: 0.0,
            field_10: 0,
            field_14: 0,
            field_18: -1,
            field_1c: 0,
            field_20: 0.0,
            field_24: 0.0,
            field_28: 0,
            field_2c: 0.0,
            field_30: [0.0; 3],
            field_3c: 0.0,
            field_40: 3,
            field_44: [0.0; 3],
            field_50: 0,
            blocks: [[0; 3]; 4],
            field_84: 0.0,
            field_88: 0.0,
            tail: [0; 5],
        }
    }
}

/// `FLPACKET_SERVER_SETDIRECTIVE`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SetDirective {
    /// The directive.
    pub directive: Directive,
}

impl Body for SetDirective {
    const ID: u32 = 0x2004e;
    const NAME: &'static str = "FLPACKET_SERVER_SETDIRECTIVE";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        let d = &self.directive;
        let mut record = WireWriter::new();
        record.put_u32(d.field_00)?;
        record.put_u32(d.field_04)?;
        record.put_u8(d.field_08)?;
        record.put_bytes(&[0; 3])?;
        record.put_f32(d.field_0c)?;
        record.put_u32(d.field_10)?;
        record.put_i32(d.field_14)?;
        record.put_i32(d.field_18)?;
        record.put_u32(d.field_1c)?;
        record.put_f32(d.field_20)?;
        record.put_f32(d.field_24)?;
        record.put_u32(d.field_28)?;
        record.put_f32(d.field_2c)?;
        record.put_vector(d.field_30)?;
        record.put_f32(d.field_3c)?;
        record.put_i32(d.field_40)?;
        record.put_vector(d.field_44)?;
        record.put_i32(d.field_50)?;
        for block in &d.blocks {
            super::field::put_u32s(&mut record, block)?;
        }
        record.put_f32(d.field_84)?;
        record.put_f32(d.field_88)?;
        record.put_bytes(&d.tail)?;
        record.put_bytes(&[0; 3])?;
        // One 0x94-byte field, as the C++ reserves it.
        writer.put_bytes(record.as_bytes())
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let mut r = WireReader::new(reader.get_bytes(0x94)?);
        let field_00 = r.get_u32()?;
        let field_04 = r.get_u32()?;
        let field_08 = r.get_u8()?;
        r.get_bytes(3)?;
        let field_0c = r.get_f32()?;
        let field_10 = r.get_u32()?;
        let field_14 = r.get_i32()?;
        let field_18 = r.get_i32()?;
        let field_1c = r.get_u32()?;
        let field_20 = r.get_f32()?;
        let field_24 = r.get_f32()?;
        let field_28 = r.get_u32()?;
        let field_2c = r.get_f32()?;
        let field_30 = r.get_vector()?;
        let field_3c = r.get_f32()?;
        let field_40 = r.get_i32()?;
        let field_44 = r.get_vector()?;
        let field_50 = r.get_i32()?;
        let mut blocks = [[0; 3]; 4];
        for block in &mut blocks {
            for word in block.iter_mut() {
                *word = r.get_u32()?;
            }
        }
        let field_84 = r.get_f32()?;
        let field_88 = r.get_f32()?;
        let mut tail = [0; 5];
        tail.copy_from_slice(r.get_bytes(5)?);
        Ok(Self {
            directive: Directive {
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
            },
        })
    }
}

/// `FLPACKET_SERVER_SENDCOMM`: a comm message with its speaker's costume.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SendComm {
    /// `Field_0x20`.
    pub field_20: u32,
    /// `Field_0x24`.
    pub field_24: u32,
    /// `Field_0x28`.
    pub field_28: u32,
    /// The costume byte (`Field_0x74`): the costume travels when it is not
    /// zero.
    pub costume_flag: u8,
    /// The costume ([`Costume::put_comm`]); parts 2 and 3 do not travel.
    pub costume: Costume,
    /// `Field_0x60`, the comm type.
    pub field_60: u32,
    /// The text IDs, at most 127 (a signed count byte).
    pub text_ids: Vec<u32>,
    /// The mood's low word.
    pub mood: u16,
    /// The duration byte (`RpcServerSendCommQuantize`: × 0.1 × 255).
    pub duration: u8,
}

impl Body for SendComm {
    const ID: u32 = 0x2004f;
    const NAME: &'static str = "FLPACKET_SERVER_SENDCOMM";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.field_20)?;
        writer.put_u32(self.field_24)?;
        writer.put_u32(self.field_28)?;
        writer.put_u8(self.costume_flag)?;
        if self.costume_flag != 0 {
            self.costume.put_comm(writer)?;
        }
        writer.put_u32(self.field_60)?;
        let count = count_fits(self.text_ids.len(), 0x7f)?;
        writer.put_u8(count_u8(count)?)?;
        super::field::put_u32s(writer, &self.text_ids)?;
        writer.put_u16(self.mood)?;
        writer.put_u8(self.duration)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let field_20 = reader.get_u32()?;
        let field_24 = reader.get_u32()?;
        let field_28 = reader.get_u32()?;
        let costume_flag = reader.get_u8()?;
        let costume = if costume_flag != 0 {
            Costume::get_comm(reader)?
        } else {
            Costume::default()
        };
        let field_60 = reader.get_u32()?;
        // A signed count: 0x80 and above read nothing.
        let count = reader.get_i8()?;
        let count = u32::try_from(count).unwrap_or(0);
        let text_ids = super::field::get_u32s(reader, count)?;
        Ok(Self {
            field_20,
            field_24,
            field_28,
            costume_flag,
            costume,
            field_60,
            text_ids,
            mood: reader.get_u16()?,
            duration: reader.get_u8()?,
        })
    }
}

wire_message! {
    /// `FLPACKET_SERVER_USE_ITEM`.
    pub struct UseItem = 0x20051, "FLPACKET_SERVER_USE_ITEM" {
        /// The object.
        pub object_id: u32,
        /// The equipment.
        pub equip_id: u16,
        /// The count.
        pub count: u16,
    }
}

/// `FLPACKET_SERVER_PLAYERLIST`: one entry of the player list.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PlayerList {
    /// The kind (0 clear, 1 add, 2 remove).
    pub kind: u32,
    /// The player.
    pub player_id: u32,
    /// `Field_0x2c`.
    pub field_2c: u8,
    /// The name's units with its terminator, as they travel; empty for no
    /// name. At most 255.
    pub name: Vec<u16>,
}

impl Body for PlayerList {
    const ID: u32 = 0x20052;
    const NAME: &'static str = "FLPACKET_SERVER_PLAYERLIST";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.kind)?;
        writer.put_u32(self.player_id)?;
        writer.put_u8(self.field_2c)?;
        writer.put_wide_u8(&self.name)
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            kind: reader.get_u32()?,
            player_id: reader.get_u32()?,
            field_2c: reader.get_u8()?,
            name: reader.get_wide_u8()?,
        })
    }
}

wire_message! {
    /// `FLPACKET_SERVER_FORMATION_UPDATE`.
    pub struct FormationUpdate = 0x20053, "FLPACKET_SERVER_FORMATION_UPDATE" {
        /// The leader.
        pub leader_id: u32,
        /// The offset.
        pub offset: [f32; 3],
    }
}

/// `FLPACKET_SERVER_MISCOBJUPDATE`: a flag word, then the fields its bits
/// select, in this order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MiscObjUpdate {
    /// The flags. Bits outside the nine below travel unread.
    pub flags: u16,
    /// `0x08`: `Field_0x28`, the object.
    pub field_28: u32,
    /// `0x04`: `Field_0x24`, the player.
    pub field_24: u32,
    /// `0x01`: `Field_0x34`, the group.
    pub field_34: u32,
    /// `0x02`: `Field_0x38`, the mission.
    pub field_38: u32,
    /// `0x02`: `Field_0x3c`, who set it.
    pub field_3c: u32,
    /// `0x10`: the text (MOTD), at most 65 535 units.
    pub text: Vec<u16>,
    /// `0x20`: `Field_0x54`, the affiliation.
    pub field_54: u32,
    /// `0x40`: `Field_0x2c`, the rank.
    pub field_2c: u16,
    /// `0x80`: `Field_0x30`, the system.
    pub field_30: u32,
    /// `0x100`: the formation members, at most 255.
    pub ids: Vec<u32>,
}

impl Body for MiscObjUpdate {
    const ID: u32 = 0x20054;
    const NAME: &'static str = "FLPACKET_SERVER_MISCOBJUPDATE";

    fn encode_body(&self, _ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        let f = self.flags;
        writer.put_u16(f)?;
        if f & 0x08 != 0 {
            writer.put_u32(self.field_28)?;
        }
        if f & 0x04 != 0 {
            writer.put_u32(self.field_24)?;
        }
        if f & 0x01 != 0 {
            writer.put_u32(self.field_34)?;
        }
        if f & 0x02 != 0 {
            writer.put_u32(self.field_38)?;
            writer.put_u32(self.field_3c)?;
        }
        if f & 0x10 != 0 {
            writer.put_wide_u16(&self.text)?;
        }
        if f & 0x20 != 0 {
            writer.put_u32(self.field_54)?;
        }
        if f & 0x40 != 0 {
            writer.put_u16(self.field_2c)?;
        }
        if f & 0x80 != 0 {
            writer.put_u32(self.field_30)?;
        }
        if f & 0x100 != 0 {
            writer.put_u8(count_u8(self.ids.len())?)?;
            super::field::put_u32s(writer, &self.ids)?;
        }
        Ok(())
    }

    fn decode_body(_ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let flags = reader.get_u16()?;
        let mut message = Self {
            flags,
            ..Self::default()
        };
        if flags & 0x08 != 0 {
            message.field_28 = reader.get_u32()?;
        }
        if flags & 0x04 != 0 {
            message.field_24 = reader.get_u32()?;
        }
        if flags & 0x01 != 0 {
            message.field_34 = reader.get_u32()?;
        }
        if flags & 0x02 != 0 {
            message.field_38 = reader.get_u32()?;
            message.field_3c = reader.get_u32()?;
        }
        if flags & 0x10 != 0 {
            message.text = reader.get_wide_u16()?;
        }
        if flags & 0x20 != 0 {
            message.field_54 = reader.get_u32()?;
        }
        if flags & 0x40 != 0 {
            message.field_2c = reader.get_u16()?;
        }
        if flags & 0x80 != 0 {
            message.field_30 = reader.get_u32()?;
        }
        if flags & 0x100 != 0 {
            let count = reader.get_u8()?;
            message.ids = super::field::get_u32s(reader, u32::from(count))?;
        }
        Ok(message)
    }
}

wire_message! {
    /// `FLPACKET_SERVER_OBJECTCARGOUPDATE`.
    pub struct ObjectCargoUpdate = 0x20055, "FLPACKET_SERVER_OBJECTCARGOUPDATE" {
        /// The object.
        pub object_id: u32,
        /// The archetype.
        pub arch_id: u32,
        /// The equipment.
        pub equip_id: u16,
        /// The count's low word.
        pub count: u16,
        /// The health, through the x87 stack.
        pub status: f32 => x87,
        /// `bMission`, raw.
        pub mission: u8,
    }
}

wire_message! {
    /// `FLPACKET_SERVER_SET_MUSIC`.
    pub struct SetMusic = 0x20057, "FLPACKET_SERVER_SET_MUSIC" {
        /// `Field_0x20`.
        pub field_20: u32,
        /// `Field_0x24`.
        pub field_24: u32,
        /// `Field_0x28`.
        pub field_28: u32,
        /// `Field_0x2c`.
        pub field_2c: u32,
        /// `Field_0x30`.
        pub field_30: u8,
        /// `Field_0x34`, through the x87 stack.
        pub field_34: f32 => x87,
    }
}

catalog! {
    /// A channel 2 message.
    pub enum ServerMessage {
        ConnectResponse,
        LoginResponse,
        CharacterInfo,
        CreateShip,
        DamageObject,
        DestroyObject,
        Launch,
        CharSelectVerified,
        ActivateObject,
        Land,
        SetStartRoom,
        GfCompleteNewsBroadcastList,
        GfCompleteCharList,
        GfCompleteMissionComputerList,
        GfCompleteScriptBehaviorList,
        GfCompleteAmbientScriptList,
        GfDestroyNewsBroadcast,
        GfDestroyCharacter,
        GfDestroyMissionComputer,
        GfDestroyScriptBehavior,
        GfDestroyAmbientScript,
        GfScriptBehavior,
        GfUpdateMissionComputer,
        GfUpdateNewsBroadcast,
        GfUpdateAmbientScript,
        GfMissionVendorAcceptance,
        SystemSwitchOut,
        SystemSwitchIn,
        SetShipArch,
        SetEquipment,
        SetCargo,
        GfUpdateChar,
        RequestCreateShipResp,
        CreateLoot,
        SetReputation,
        AdjustAttitude,
        SetGroupFeelings,
        CreateMine,
        CreateCounter,
        SetAddItem,
        SetRemoveItem,
        SetCash,
        ExplodeAsteroidMine,
        RequestSpaceScript,
        SetMissionObjectiveState,
        ReplaceMissionObjective,
        SetMissionObjectives,
        CreateGuided,
        ItemTractored,
        ScanNotify,
        RepairObject,
        RemoteObjectCargoUpdate,
        SetNumKills,
        SetMissionSuccesses,
        SetMissionFailures,
        BurnFuse,
        CreateSolar,
        SetStoryCue,
        RequestReturned,
        SetMissionMessage,
        MarkObj,
        CfgInterfaceNotification,
        SetCollisionGroups,
        SetHullStatus,
        SetGuidedTarget,
        SetCamera,
        RevertCamera,
        LoadHint,
        SetDirective,
        SendComm,
        UseItem,
        PlayerList,
        FormationUpdate,
        MiscObjUpdate,
        ObjectCargoUpdate,
        SendNnMessage,
        SetMusic,
        CancelMusic,
        PlaySoundEffect,
        GfMissionVendorWhyEmpty,
        MissionSaveA,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn catalog_size() {
        assert_eq!(ServerMessage::IDS.len(), 81);
        let mut ids = ServerMessage::IDS.to_vec();
        ids.sort_unstable();
        ids.dedup();
        assert_eq!(ids.len(), 81);
    }

    #[test]
    fn space_script_carries_position_and_mode_in_single_player_only() {
        let message = RequestSpaceScript {
            field_00: 7,
            name: b"a.thn\0".to_vec(),
            position: [1, 2, 3],
            mode: 0,
        };
        let sp = message.to_wire(&WireContext::SINGLE_PLAYER_SERVER).unwrap();
        let back = ServerMessage::from_wire(&WireContext::SINGLE_PLAYER_SERVER, &sp).unwrap();
        assert_eq!(back, ServerMessage::RequestSpaceScript(message.clone()));
        assert_eq!(message.priority(), 2);
        let mp = message.to_wire(&WireContext::SERVER).unwrap();
        assert_eq!(mp.len(), sp.len() - 13);
        let ServerMessage::RequestSpaceScript(back) =
            ServerMessage::from_wire(&WireContext::SERVER, &mp).unwrap()
        else {
            panic!("wrong message");
        };
        assert_eq!((back.position, back.mode, back.priority()), ([0; 3], 1, 1));
    }

    #[test]
    fn directive_is_0x94_bytes() {
        let message = SetDirective::default();
        let bytes = message.to_wire(&WireContext::SERVER).unwrap();
        assert_eq!(bytes.len(), 2 + 0x94);
        assert_eq!(&bytes[2 + 0x18..2 + 0x1c], [0xff; 4]);
        let back = ServerMessage::from_wire(&WireContext::SERVER, &bytes).unwrap();
        assert_eq!(back, ServerMessage::SetDirective(message));
    }

    #[test]
    fn set_reputation_forms() {
        let message = SetReputation {
            target: 1,
            affiliation: 2,
            display_name: vec![0x41, 0],
            names: (vec![1, 0, 0, 0], vec![]),
            relations: vec![Relation {
                feeling: 0.5,
                group_id: 3,
            }],
            attitude: -0.25,
        };
        let mp = message.to_wire(&WireContext::SERVER).unwrap();
        assert_eq!(mp.len(), 2 + 1 + 4 + 4 + 4);
        let sp = message.to_wire(&WireContext::SINGLE_PLAYER_SERVER).unwrap();
        assert_eq!(sp[2], 0x01 | 0x02 | 0x04 | 0x08);
        let back = ServerMessage::from_wire(&WireContext::SINGLE_PLAYER_SERVER, &sp).unwrap();
        let ServerMessage::SetReputation(back) = back else {
            panic!("wrong message");
        };
        assert_eq!(back.relations, message.relations);
        assert_eq!(back.display_name, message.display_name);
    }
}
