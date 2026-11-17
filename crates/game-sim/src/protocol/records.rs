//! Records several messages share: collision groups, equipment, damage
//! and the create-object block of `CREATESHIP` and `CREATESOLAR`
//! (`remote_server_channel_codecs.cpp`, `remote_packet_codecs.cpp`).

use neuron_core::net::wire::quantise::quantise_fraction;

use super::costume::CreateCostume;
use super::field::{count_u8, count_u16, count_u32, get_list};
use super::{WireContext, WireError, WireReader, WireWriter};

/// A collision group's health (`CollisionGroupDesc`): the group ID and a
/// fraction of its hit points.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct CollisionGroup {
    /// `Id`.
    pub id: u16,
    /// `Health`, sent through the x87 stack.
    pub health: f32,
}

impl CollisionGroup {
    /// `RpcWireEncodeCollisionGroups`: a `u16` count, then per group the ID
    /// and the health.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above 65 535 groups.
    pub fn put_list(writer: &mut WireWriter, groups: &[Self]) -> Result<(), WireError> {
        writer.put_u16(count_u16(groups.len())?)?;
        for group in groups {
            writer.put_u16(group.id)?;
            writer.put_f32_x87(group.health)?;
        }
        Ok(())
    }

    /// `RpcWireDecodeCollisionGroups`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get_list(reader: &mut WireReader<'_>) -> Result<Vec<Self>, WireError> {
        let count = reader.get_u16()?;
        get_list(reader, u32::from(count), 6, Self::get)
    }

    fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            id: reader.get_u16()?,
            health: reader.get_f32()?,
        })
    }

    /// The `CHARACTERINFO` form: a `u32` count, then ID and health.
    ///
    /// # Errors
    /// The writer's errors.
    pub fn put_list_u32(writer: &mut WireWriter, groups: &[Self]) -> Result<(), WireError> {
        writer.put_u32(count_u32(groups.len())?)?;
        for group in groups {
            writer.put_u16(group.id)?;
            writer.put_f32_x87(group.health)?;
        }
        Ok(())
    }

    /// The `CHARACTERINFO` form.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get_list_u32(reader: &mut WireReader<'_>) -> Result<Vec<Self>, WireError> {
        let count = reader.get_u32()?;
        get_list(reader, count, 6, Self::get)
    }
}

/// One equipment record of `SETEQUIPMENT`, `SETCARGO`,
/// `REMOTEOBJECTCARGOUPDATE` and `CHARACTERINFO` (`FRpcServerEquipRecord`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EquipRecord {
    /// `Count`.
    pub count: i32,
    /// `Health`, sent through the x87 stack.
    pub health: f32,
    /// `ArchId`, the full archetype ID.
    pub arch_id: u32,
    /// `Id`.
    pub id: u16,
    /// `bEquipped`, the raw byte.
    pub equipped: u8,
    /// The byte after it (`Field_0x0f`); no builder sets it.
    pub field_0f: u8,
    /// The hardpoint name as the record stores it, terminator included
    /// (`strlen + 1` bytes); empty for the internal cargo bay.
    pub hardpoint: Vec<u8>,
}

impl EquipRecord {
    /// Count, health, arch ID, ID, the two bytes, the `u16` hardpoint size
    /// and its bytes.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] for a hardpoint above 65 535 bytes.
    pub fn put(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        let size = count_u16(self.hardpoint.len())?;
        writer.put_i32(self.count)?;
        writer.put_f32_x87(self.health)?;
        writer.put_u32(self.arch_id)?;
        writer.put_u16(self.id)?;
        writer.put_u8(self.equipped)?;
        writer.put_u8(self.field_0f)?;
        writer.put_u16(size)?;
        writer.put_bytes(&self.hardpoint)
    }

    /// The inverse of [`EquipRecord::put`].
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let count = reader.get_i32()?;
        let health = reader.get_f32()?;
        let arch_id = reader.get_u32()?;
        let id = reader.get_u16()?;
        let equipped = reader.get_u8()?;
        let field_0f = reader.get_u8()?;
        let size = reader.get_u16()?;
        let hardpoint = reader.get_bytes(usize::from(size))?.to_vec();
        Ok(Self {
            count,
            health,
            arch_id,
            id,
            equipped,
            field_0f,
            hardpoint,
        })
    }

    /// The list form of `FRpcServerEquipListPacketBase`: a `u16` count and
    /// the records.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above 65 535 records.
    pub fn put_list(writer: &mut WireWriter, records: &[Self]) -> Result<(), WireError> {
        writer.put_u16(count_u16(records.len())?)?;
        records.iter().try_for_each(|r| r.put(writer))
    }

    /// The list form.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get_list(reader: &mut WireReader<'_>) -> Result<Vec<Self>, WireError> {
        let count = reader.get_u16()?;
        get_list(reader, u32::from(count), 18, Self::get)
    }
}

/// One damage entry of `DAMAGEOBJECT` (`DamageEntry`, fate as a byte).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DamageEntry {
    /// `SubObjId`.
    pub sub_obj_id: u16,
    /// `Fate`, its low byte.
    pub fate: u8,
    /// `HitPts`, through the x87 stack both ways.
    pub hit_points: f32,
}

/// The damage list of `DAMAGEOBJECT` (`DamageList`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DamageList {
    /// `bDestroyed`, the raw byte.
    pub destroyed: u8,
    /// `Cause`.
    pub cause: u32,
    /// `InflictorId`.
    pub inflictor_id: u32,
    /// The entries, at most 65 535.
    pub entries: Vec<DamageEntry>,
}

impl DamageList {
    /// Destroyed byte, cause, inflictor, `u16` count, then per entry the
    /// sub-object, the fate byte and the hit points.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] above 65 535 entries.
    pub fn put(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        let count = count_u16(self.entries.len())?;
        writer.put_u8(self.destroyed)?;
        writer.put_u32(self.cause)?;
        writer.put_u32(self.inflictor_id)?;
        writer.put_u16(count)?;
        for entry in &self.entries {
            writer.put_u16(entry.sub_obj_id)?;
            writer.put_u8(entry.fate)?;
            writer.put_f32_x87(entry.hit_points)?;
        }
        Ok(())
    }

    /// The inverse of [`DamageList::put`]; the hit points go through the
    /// x87 stack here too (`RpcServerX87CopyFloat`).
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let destroyed = reader.get_u8()?;
        let cause = reader.get_u32()?;
        let inflictor_id = reader.get_u32()?;
        let count = reader.get_u16()?;
        let entries = get_list(reader, u32::from(count), 7, |r| {
            Ok(DamageEntry {
                sub_obj_id: r.get_u16()?,
                fate: r.get_u8()?,
                hit_points: f32::from_bits(neuron_core::net::wire::x87_copy_f32(r.get_u32()?)),
            })
        })?;
        Ok(Self {
            destroyed,
            cause,
            inflictor_id,
            entries,
        })
    }
}

/// The hardpoint dictionary of the create-object block: codes 1 to 42
/// (`GRpcServerHardpointNames`).
pub const HARDPOINT_NAMES: [&str; 42] = [
    "HpCM01",
    "HpCargo01",
    "HpCargo02",
    "HpContrail01",
    "HpContrail02",
    "HpDockLight01",
    "HpDockLight02",
    "HpDockLight03",
    "HpHeadlight",
    "HpMine01",
    "HpRunningLight01",
    "HpRunningLight02",
    "HpRunningLight03",
    "HpRunningLight04",
    "HpRunningLight05",
    "HpRunningLight06",
    "HpRunningLight07",
    "HpRunningLight10",
    "HpRunningLight11",
    "HpRunningLight12",
    "HpRunningLight13",
    "HpShield01",
    "HpThruster01",
    "HpTurret01",
    "HpTurret02",
    "HpTurret03",
    "HpTurret04",
    "HpTurret05",
    "HpTurret06",
    "HpTurret07",
    "HpTurret08",
    "HpTurret09",
    "HpTurret_U1_01",
    "HpTurret_U1_02",
    "HpTurret_U1_03",
    "HpTurret_U1_04",
    "HpTurret_U1_05",
    "HpTurret_U3_01",
    "HpWeapon01",
    "HpWeapon02",
    "HpWeapon03",
    "HpWeapon04",
];

/// The hardpoint of a create-object equipment record.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum CreateHardpoint {
    /// No hardpoint (flag `0x20`): internal equipment.
    #[default]
    None,
    /// A code of [`HARDPOINT_NAMES`], 1 to 42 (the decoder accepts any
    /// non-zero byte and finds no name for an unknown one).
    Code(u8),
    /// A name outside the dictionary as the record stores it, terminator
    /// included (flag `0x10`): a length byte, then the bytes.
    Text(Vec<u8>),
}

impl CreateHardpoint {
    /// The form the encoder picks for a name (without its terminator):
    /// the dictionary code, or the name and a terminator.
    #[must_use]
    pub fn from_name(name: &[u8]) -> Self {
        HARDPOINT_NAMES
            .iter()
            .position(|n| n.as_bytes() == name)
            .and_then(|index| u8::try_from(index + 1).ok())
            .map_or_else(
                || {
                    let mut text = name.to_vec();
                    text.push(0);
                    Self::Text(text)
                },
                Self::Code,
            )
    }

    /// The name a code stands for (`RpcServerHardpointCodeToName`).
    #[must_use]
    pub fn code_name(code: u8) -> Option<&'static str> {
        usize::from(code)
            .checked_sub(1)
            .and_then(|index| HARDPOINT_NAMES.get(index))
            .copied()
    }
}

/// One equipment record of the create-object block, as its flag byte
/// encodes it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CreateEquip {
    /// `bEquipped != 0` (flag `0x01`).
    pub equipped: bool,
    /// `Field_0x0f != 0` (flag `0x02`).
    pub field_0f: bool,
    /// `Count`: flag `0x80` for 1, a dword above 255 (flag `0x04`),
    /// otherwise a byte. A negative count does not travel.
    pub count: i32,
    /// The health byte (× 255), or `None` for exactly 1.0 (flag `0x40`).
    pub health: Option<u8>,
    /// The archetype's small ID (`Archetype::LargeIDToSmallID`).
    pub archetype: u16,
    /// `Id`: a word above 255 (flag `0x08`), otherwise a byte.
    pub id: u16,
    /// The hardpoint.
    pub hardpoint: CreateHardpoint,
}

impl CreateEquip {
    /// The health field for a health value: `None` for exactly 1.0, as the
    /// encoder's `FCOMP` against 1.0 decides, otherwise the × 255 byte.
    #[must_use]
    pub fn health_byte(health: f32) -> Option<u8> {
        (f64::from(health) != 1.0).then(|| quantise_fraction(health))
    }

    fn put(&self, writer: &mut WireWriter) -> Result<(), WireError> {
        if self.count < 0 {
            return Err(WireError::Invalid("negative create-object equipment count"));
        }
        let mut flags = u8::from(self.equipped);
        if self.field_0f {
            flags |= 0x02;
        }
        if self.count > 0xff {
            flags |= 0x04;
        }
        if self.id > 0xff {
            flags |= 0x08;
        }
        match &self.hardpoint {
            CreateHardpoint::None => flags |= 0x20,
            CreateHardpoint::Text(_) => flags |= 0x10,
            CreateHardpoint::Code(0) => return Err(WireError::Invalid("hardpoint code 0")),
            CreateHardpoint::Code(_) => {}
        }
        if self.count == 1 {
            flags |= 0x80;
        }
        if self.health.is_none() {
            flags |= 0x40;
        }
        writer.put_u8(flags)?;
        if flags & 0x80 == 0 {
            if flags & 0x04 != 0 {
                writer.put_i32(self.count)?;
            } else {
                writer.put_u8(self.count.to_le_bytes()[0])?;
            }
        }
        if let Some(health) = self.health {
            writer.put_u8(health)?;
        }
        writer.put_u16(self.archetype)?;
        if flags & 0x08 != 0 {
            writer.put_u16(self.id)?;
        } else {
            writer.put_u8(self.id.to_le_bytes()[0])?;
        }
        match &self.hardpoint {
            CreateHardpoint::None => Ok(()),
            CreateHardpoint::Code(code) => writer.put_u8(*code),
            CreateHardpoint::Text(text) => {
                writer.put_u8(count_u8(text.len())?)?;
                writer.put_bytes(text)
            }
        }
    }

    fn get(reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let flags = reader.get_u8()?;
        let count = if flags & 0x80 != 0 {
            1
        } else if flags & 0x04 != 0 {
            reader.get_i32()?
        } else {
            i32::from(reader.get_u8()?)
        };
        let health = if flags & 0x40 != 0 {
            None
        } else {
            Some(reader.get_u8()?)
        };
        let archetype = reader.get_u16()?;
        let id = if flags & 0x08 != 0 {
            reader.get_u16()?
        } else {
            u16::from(reader.get_u8()?)
        };
        let hardpoint = if flags & 0x20 != 0 {
            CreateHardpoint::None
        } else if flags & 0x10 != 0 {
            let length = reader.get_u8()?;
            CreateHardpoint::Text(reader.get_bytes(usize::from(length))?.to_vec())
        } else {
            CreateHardpoint::Code(reader.get_u8()?)
        };
        Ok(Self {
            equipped: flags & 0x01 != 0,
            field_0f: flags & 0x02 != 0,
            count,
            health,
            archetype,
            id,
            hardpoint,
        })
    }
}

/// A collision group of the create-object block: the ID's low byte and
/// the health × 255.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CreateCollisionGroup {
    /// The group ID's low byte.
    pub id: u8,
    /// The health byte (`RpcServerQuantize255`).
    pub health: u8,
}

/// A damage entry of the create-object block, single player only.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct CreateDamageEntry {
    /// `SubObjId`.
    pub sub_obj_id: u16,
    /// `Fate`, a dword here.
    pub fate: u32,
    /// `HitPts`, through the x87 stack on encode.
    pub hit_points: f32,
}

/// The create-object block at the start of `CREATESHIP` and `CREATESOLAR`
/// (`FRpcServerCreateObjectFields`, `RpcServerCreateObjectEncode`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CreateObject {
    /// `ObjId`.
    pub obj_id: u32,
    /// The archetype's small ID.
    pub archetype: u16,
    /// `DockingBaseId`.
    pub docking_base_id: u32,
    /// `VibeGroupId`.
    pub vibe_group_id: u32,
    /// The costume.
    pub costume: CreateCostume,
    /// `VoiceId`.
    pub voice_id: u32,
    /// `Position`, raw floats.
    pub position: [f32; 3],
    /// The orientation bytes `x`, `y`, `z`, `w`
    /// ([`super::quantise_orientation`]).
    pub orientation: [u8; 4],
    /// The hull byte (`RpcServerQuantizeHull`: hit points over the
    /// archetype's maximum × 255).
    pub hull: u8,
    /// `Field_0x6c`; single player only.
    pub field_6c: u32,
    /// The equipment, at most 65 535 records.
    pub equipment: Vec<CreateEquip>,
    /// The collision groups, at most 255.
    pub collision_groups: Vec<CreateCollisionGroup>,
    /// The damage list's entries; single player only.
    pub damage: Vec<CreateDamageEntry>,
}

impl CreateObject {
    /// Writes the block.
    ///
    /// # Errors
    /// [`WireError::LengthOutOfRange`] for a list that does not fit its
    /// count, [`WireError::Invalid`] for a record the flags cannot carry.
    pub fn put(&self, ctx: &WireContext, writer: &mut WireWriter) -> Result<(), WireError> {
        writer.put_u32(self.obj_id)?;
        writer.put_u16(self.archetype)?;
        writer.put_u32(self.docking_base_id)?;
        writer.put_u32(self.vibe_group_id)?;
        self.costume.put(writer)?;
        writer.put_u32(self.voice_id)?;
        writer.put_vector(self.position)?;
        writer.put_bytes(&self.orientation)?;
        writer.put_u8(self.hull)?;
        if ctx.single_player {
            writer.put_u32(self.field_6c)?;
        }
        writer.put_u16(count_u16(self.equipment.len())?)?;
        for record in &self.equipment {
            record.put(writer)?;
        }
        writer.put_u8(count_u8(self.collision_groups.len())?)?;
        for group in &self.collision_groups {
            writer.put_u8(group.id)?;
            writer.put_u8(group.health)?;
        }
        if ctx.single_player {
            writer.put_u32(count_u32(self.damage.len())?)?;
            for entry in &self.damage {
                writer.put_u16(entry.sub_obj_id)?;
                writer.put_u32(entry.fate)?;
                writer.put_f32_x87(entry.hit_points)?;
            }
        }
        Ok(())
    }

    /// Reads the block.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short input.
    pub fn get(ctx: &WireContext, reader: &mut WireReader<'_>) -> Result<Self, WireError> {
        let obj_id = reader.get_u32()?;
        let archetype = reader.get_u16()?;
        let docking_base_id = reader.get_u32()?;
        let vibe_group_id = reader.get_u32()?;
        let costume = CreateCostume::get(reader)?;
        let voice_id = reader.get_u32()?;
        let position = reader.get_vector()?;
        let mut orientation = [0; 4];
        orientation.copy_from_slice(reader.get_bytes(4)?);
        let hull = reader.get_u8()?;
        let field_6c = if ctx.single_player {
            reader.get_u32()?
        } else {
            0
        };
        let equipment_count = reader.get_u16()?;
        let equipment = get_list(reader, u32::from(equipment_count), 4, CreateEquip::get)?;
        let group_count = reader.get_u8()?;
        let collision_groups = get_list(reader, u32::from(group_count), 2, |r| {
            Ok(CreateCollisionGroup {
                id: r.get_u8()?,
                health: r.get_u8()?,
            })
        })?;
        let damage = if ctx.single_player {
            let count = reader.get_u32()?;
            get_list(reader, count, 10, |r| {
                Ok(CreateDamageEntry {
                    sub_obj_id: r.get_u16()?,
                    fate: r.get_u32()?,
                    hit_points: r.get_f32()?,
                })
            })?
        } else {
            Vec::new()
        };
        Ok(Self {
            obj_id,
            archetype,
            docking_base_id,
            vibe_group_id,
            costume,
            voice_id,
            position,
            orientation,
            hull,
            field_6c,
            equipment,
            collision_groups,
            damage,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hardpoint_dictionary() {
        assert_eq!(
            CreateHardpoint::from_name(b"HpCM01"),
            CreateHardpoint::Code(1)
        );
        assert_eq!(
            CreateHardpoint::from_name(b"HpWeapon04"),
            CreateHardpoint::Code(42)
        );
        assert_eq!(
            CreateHardpoint::from_name(b"HpX"),
            CreateHardpoint::Text(b"HpX\0".to_vec())
        );
        assert_eq!(CreateHardpoint::code_name(0), None);
        assert_eq!(CreateHardpoint::code_name(43), None);
        assert_eq!(CreateHardpoint::code_name(23), Some("HpThruster01"));
    }

    #[test]
    fn create_equip_flags() {
        let record = CreateEquip {
            equipped: true,
            field_0f: false,
            count: 1,
            health: None,
            archetype: 5,
            id: 300,
            hardpoint: CreateHardpoint::Code(3),
        };
        let mut writer = WireWriter::new();
        record.put(&mut writer).unwrap();
        assert_eq!(writer.as_bytes(), [0xc9, 5, 0, 0x2c, 1, 3]);
        let bytes = writer.into_bytes();
        assert_eq!(
            CreateEquip::get(&mut WireReader::new(&bytes)).unwrap(),
            record
        );
        assert_eq!(CreateEquip::health_byte(1.0), None);
        assert_eq!(CreateEquip::health_byte(0.5), Some(127));
    }
}
