//! The base records the server sends to a player in a base (mission
//! vendor offers and answers, news, characters, ambient scenes), ported
//! from `CppSource/Common/common_baseproperties.cpp`: their fields and
//! their `marshal` / `unmarshal` through `MarshalBuffer`.
//!
//! The C++ reads each dword through a stack cell that starts as the
//! buffer's address, so a truncated buffer gives fields that depend on the
//! process; here a short buffer is an error, as everywhere in
//! [`MarshalReader`] (divergence D101). `CostumeHint` copies a `Costume`
//! as raw memory, which is no format, and the client-only helpers
//! (`get_icon_texture` and the like) stay with the base screens (P5.10).

use super::WireError;
use super::costume::Costume;
use super::fmtstr::FmtStr;
use super::marshal::{MarshalReader, MarshalWriter};

/// A mission vendor offer (`MissionVendorProperties`).
#[derive(Clone, Debug, PartialEq)]
pub struct MissionVendorProperties {
    /// `Key` (-1 by default).
    pub key: i32,
    /// `BaseId`.
    pub base_id: u32,
    /// `TargetId`.
    pub target_id: u32,
    /// `SystemId`.
    pub system_id: u32,
    /// `MissionClass`.
    pub mission_class: u32,
    /// `Title`.
    pub title: FmtStr,
    /// `Description`.
    pub description: FmtStr,
    /// `Briefing`.
    pub briefing: FmtStr,
    /// `IconTextureId`.
    pub icon_texture_id: u32,
}

impl Default for MissionVendorProperties {
    fn default() -> Self {
        Self {
            key: -1,
            base_id: 0,
            target_id: 0,
            system_id: 0,
            mission_class: 0,
            title: FmtStr::default(),
            description: FmtStr::default(),
            briefing: FmtStr::default(),
            icon_texture_id: 0,
        }
    }
}

impl MissionVendorProperties {
    /// `marshal`.
    ///
    /// # Errors
    /// As the [`MarshalWriter`] calls.
    pub fn marshal(&self, w: &mut MarshalWriter) -> Result<(), WireError> {
        w.put_int(self.key)?;
        w.put_u32(self.base_id)?;
        w.put_u32(self.target_id)?;
        w.put_u32(self.system_id)?;
        w.put_u32(self.mission_class)?;
        w.put_fmt_str(&self.title)?;
        w.put_fmt_str(&self.description)?;
        w.put_fmt_str(&self.briefing)?;
        w.put_u32(self.icon_texture_id)
    }

    /// `unmarshal`.
    ///
    /// # Errors
    /// [`WireError::Underrun`] for a short buffer, and the format strings'
    /// errors.
    pub fn unmarshal(r: &mut MarshalReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            key: r.get_int()?,
            base_id: r.get_u32()?,
            target_id: r.get_u32()?,
            system_id: r.get_u32()?,
            mission_class: r.get_u32()?,
            title: r.get_fmt_str()?,
            description: r.get_fmt_str()?,
            briefing: r.get_fmt_str()?,
            icon_texture_id: r.get_u32()?,
        })
    }
}

/// The answer to a mission offer (`MissionVendorAcceptance`).
#[derive(Clone, Debug, PartialEq)]
pub struct MissionVendorAcceptance {
    /// `Key` (-1 by default).
    pub key: i32,
    /// `TargetId`.
    pub target_id: u32,
    /// `bAccepted`.
    pub accepted: bool,
    /// `Reason`.
    pub reason: FmtStr,
}

impl Default for MissionVendorAcceptance {
    fn default() -> Self {
        Self {
            key: -1,
            target_id: 0,
            accepted: false,
            reason: FmtStr::default(),
        }
    }
}

impl MissionVendorAcceptance {
    /// `marshal`.
    ///
    /// # Errors
    /// As the [`MarshalWriter`] calls.
    pub fn marshal(&self, w: &mut MarshalWriter) -> Result<(), WireError> {
        w.put_int(self.key)?;
        w.put_u32(self.target_id)?;
        w.put_bool(self.accepted)?;
        w.put_fmt_str(&self.reason)
    }

    /// `unmarshal`.
    ///
    /// # Errors
    /// As [`MissionVendorProperties::unmarshal`].
    pub fn unmarshal(r: &mut MarshalReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            key: r.get_int()?,
            target_id: r.get_u32()?,
            accepted: r.get_bool()?,
            reason: r.get_fmt_str()?,
        })
    }
}

/// A news item (`NewsBroadcastProperties`).
#[derive(Clone, Debug, PartialEq)]
pub struct NewsBroadcastProperties {
    /// `Key` (-1 by default).
    pub key: i32,
    /// `BaseId`.
    pub base_id: u32,
    /// `bFlag1`.
    pub flag1: bool,
    /// `bFlag2`.
    pub flag2: bool,
    /// `Category`.
    pub category: u32,
    /// `Headline`.
    pub headline: FmtStr,
    /// `Body`.
    pub body: FmtStr,
    /// `Logo`.
    pub logo: FmtStr,
    /// `LogoTexture` (none: the client shows `genericnews`).
    pub logo_texture: Option<Vec<u8>>,
    /// `LogoTextureId`.
    pub logo_texture_id: u32,
}

impl Default for NewsBroadcastProperties {
    fn default() -> Self {
        Self {
            key: -1,
            base_id: 0,
            flag1: false,
            flag2: false,
            category: 0,
            headline: FmtStr::default(),
            body: FmtStr::default(),
            logo: FmtStr::default(),
            logo_texture: None,
            logo_texture_id: 0,
        }
    }
}

impl NewsBroadcastProperties {
    /// `marshal`.
    ///
    /// # Errors
    /// As the [`MarshalWriter`] calls.
    pub fn marshal(&self, w: &mut MarshalWriter) -> Result<(), WireError> {
        w.put_int(self.key)?;
        w.put_u32(self.base_id)?;
        w.put_bool(self.flag1)?;
        w.put_bool(self.flag2)?;
        w.put_u32(self.category)?;
        w.put_fmt_str(&self.headline)?;
        w.put_fmt_str(&self.body)?;
        w.put_fmt_str(&self.logo)?;
        w.put_string(self.logo_texture.as_deref())?;
        w.put_u32(self.logo_texture_id)
    }

    /// `unmarshal`.
    ///
    /// # Errors
    /// As [`MissionVendorProperties::unmarshal`].
    pub fn unmarshal(r: &mut MarshalReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            key: r.get_int()?,
            base_id: r.get_u32()?,
            flag1: r.get_bool()?,
            flag2: r.get_bool()?,
            category: r.get_u32()?,
            headline: r.get_fmt_str()?,
            body: r.get_fmt_str()?,
            logo: r.get_fmt_str()?,
            logo_texture: r.get_string()?,
            logo_texture_id: r.get_u32()?,
        })
    }
}

/// A character in a base room (`CharacterProperties`).
#[derive(Clone, Debug, PartialEq)]
pub struct CharacterProperties {
    /// `Key`.
    pub key: u32,
    /// `SessionId`.
    pub session_id: u32,
    /// `RoomId`.
    pub room_id: u32,
    /// `NameIds`.
    pub name_ids: u32,
    /// `FactionId` (none by default).
    pub faction_id: u32,
    /// `NameWide`.
    pub name_wide: Option<Vec<u16>>,
    /// `NameAnsi`: the character placement's name.
    pub name_ansi: Option<Vec<u8>>,
    /// `CharType`.
    pub char_type: u32,
    /// `Posture`.
    pub posture: u32,
    /// `Cost`.
    pub costume: Costume,
    /// `Script`: the idle script.
    pub script: Option<Vec<u8>>,
    /// `BehaviorId` (none by default).
    pub behavior_id: u32,
    /// `VoiceId`.
    pub voice_id: u32,
}

impl Default for CharacterProperties {
    fn default() -> Self {
        Self {
            key: 0,
            session_id: 0,
            room_id: 0,
            name_ids: 0,
            faction_id: 0xffff_ffff,
            name_wide: None,
            name_ansi: None,
            char_type: 0,
            posture: 0,
            costume: Costume::default(),
            script: None,
            behavior_id: 0xffff_ffff,
            voice_id: 0,
        }
    }
}

impl CharacterProperties {
    /// `marshal`: note the field order, which is not the declaration
    /// order.
    ///
    /// # Errors
    /// As the [`MarshalWriter`] calls.
    pub fn marshal(&self, w: &mut MarshalWriter) -> Result<(), WireError> {
        w.put_u32(self.key)?;
        w.put_u32(self.session_id)?;
        w.put_u32(self.room_id)?;
        w.put_u32(self.name_ids)?;
        w.put_u32(self.faction_id)?;
        w.put_wstring(self.name_wide.as_deref())?;
        w.put_costume(&self.costume)?;
        w.put_string(self.script.as_deref())?;
        w.put_u32(self.behavior_id)?;
        w.put_string(self.name_ansi.as_deref())?;
        w.put_u32(self.char_type)?;
        w.put_u32(self.posture)?;
        w.put_u32(self.voice_id)
    }

    /// `unmarshal`.
    ///
    /// # Errors
    /// As [`MissionVendorProperties::unmarshal`].
    pub fn unmarshal(r: &mut MarshalReader<'_>) -> Result<Self, WireError> {
        let key = r.get_u32()?;
        let session_id = r.get_u32()?;
        let room_id = r.get_u32()?;
        let name_ids = r.get_u32()?;
        let faction_id = r.get_u32()?;
        let name_wide = r.get_wstring()?;
        let costume = r.get_costume()?;
        let script = r.get_string()?;
        let behavior_id = r.get_u32()?;
        let name_ansi = r.get_string()?;
        Ok(Self {
            key,
            session_id,
            room_id,
            name_ids,
            faction_id,
            name_wide,
            name_ansi,
            char_type: r.get_u32()?,
            posture: r.get_u32()?,
            costume,
            script,
            behavior_id,
            voice_id: r.get_u32()?,
        })
    }
}

/// An ambient scene of a room (`AmbientScriptProperties`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AmbientScriptProperties {
    /// `Key` (-1 by default).
    pub key: i32,
    /// `RoomId`.
    pub room_id: u32,
    /// `ScriptId`.
    pub script_id: u32,
    /// `bFlag` (set by default).
    pub flag: bool,
    /// `Name`.
    pub name: Option<Vec<u8>>,
}

impl Default for AmbientScriptProperties {
    fn default() -> Self {
        Self {
            key: -1,
            room_id: 0,
            script_id: 0,
            flag: true,
            name: None,
        }
    }
}

impl AmbientScriptProperties {
    /// `marshal`.
    ///
    /// # Errors
    /// As the [`MarshalWriter`] calls.
    pub fn marshal(&self, w: &mut MarshalWriter) -> Result<(), WireError> {
        w.put_int(self.key)?;
        w.put_u32(self.room_id)?;
        w.put_u32(self.script_id)?;
        w.put_bool(self.flag)?;
        w.put_string(self.name.as_deref())
    }

    /// `unmarshal`.
    ///
    /// # Errors
    /// As [`MissionVendorProperties::unmarshal`].
    pub fn unmarshal(r: &mut MarshalReader<'_>) -> Result<Self, WireError> {
        Ok(Self {
            key: r.get_int()?,
            room_id: r.get_u32()?,
            script_id: r.get_u32()?,
            flag: r.get_bool()?,
            name: r.get_string()?,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn records_round_trip() {
        let mut title = FmtStr::new(1234);
        title.append_int(7);
        let offer = MissionVendorProperties {
            key: 3,
            base_id: 4,
            title,
            ..MissionVendorProperties::default()
        };
        let character = CharacterProperties {
            name_wide: Some(vec![0x48, 0x69]),
            script: Some(b"idle".to_vec()),
            costume: Costume {
                parts: [1, 2, 3, 4],
                accessories: vec![9],
            },
            ..CharacterProperties::default()
        };
        let news = NewsBroadcastProperties {
            logo_texture: Some(b"logo".to_vec()),
            flag2: true,
            ..NewsBroadcastProperties::default()
        };
        let mut w = MarshalWriter::new();
        offer.marshal(&mut w).expect("offer");
        character.marshal(&mut w).expect("character");
        news.marshal(&mut w).expect("news");
        AmbientScriptProperties::default()
            .marshal(&mut w)
            .expect("ambient");
        MissionVendorAcceptance::default()
            .marshal(&mut w)
            .expect("acceptance");
        let bytes = w.into_bytes();
        let mut r = MarshalReader::new(&bytes);
        assert_eq!(
            MissionVendorProperties::unmarshal(&mut r).expect("offer"),
            offer
        );
        assert_eq!(
            CharacterProperties::unmarshal(&mut r).expect("character"),
            character
        );
        assert_eq!(
            NewsBroadcastProperties::unmarshal(&mut r).expect("news"),
            news
        );
        assert_eq!(
            AmbientScriptProperties::unmarshal(&mut r).expect("ambient"),
            AmbientScriptProperties::default()
        );
        assert_eq!(
            MissionVendorAcceptance::unmarshal(&mut r).expect("acceptance"),
            MissionVendorAcceptance::default()
        );
        assert!(r.is_empty());
        assert!(MissionVendorAcceptance::unmarshal(&mut MarshalReader::new(&bytes[..5])).is_err());
    }
}
