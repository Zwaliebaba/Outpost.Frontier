//! The NPC character description of TU42 (`FNpcCharacterDesc`,
//! `Content/NpcShips.cpp:148-237`): a `[NPC]` block of
//! `missions\specific_npc.ini` or a `[GF_NPC]` block of `missions\mBases.ini`
//! (work package P3.4c, decision record 0008 point 10).
//!
//! The specific NPCs ([`SpecificNpcs`]) are what StoryLauncher's constructor
//! reads (`StoryLauncher.cpp:39-53`) and its destructor empties
//! (`:57-60`); they live behind an `Arc` in [`ContentState`](super::ContentState)
//! because StoryLauncher, not GameSupport, makes them, and nothing changes
//! them in between. The bar scenes of RTC scripts read them (TU23, P3.6e).

use std::collections::BTreeMap;

use game_sim::data::costume::{Costume, MAX_ACCESSORIES};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::tables::{reputation_group, voice_id};
use crate::api;
use crate::server::Cx;

/// `missions\specific_npc.ini`.
pub const SPECIFIC_NPC_FILE: &[u8] = b"missions\\specific_npc.ini";

/// `FNpcCharacterDesc` (0x44 bytes).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NpcCharacterDesc {
    /// `Appearance`: head, body, left hand, right hand, accessories.
    pub appearance: Costume,
    /// `voice`, `pub::GetVoiceID`.
    pub voice: u32,
    /// `individual_name`.
    pub individual_name: i32,
    /// `affiliation`, `GetReputationGroup` (`0xffffffff` by default).
    pub affiliation: u32,
    /// `nickname`, `CreateID` (0 by default).
    pub nickname: u32,
}

impl Default for NpcCharacterDesc {
    /// The constructor: no parts, no voice, no name, affiliation
    /// `0xffffffff`, nickname cleared.
    fn default() -> Self {
        Self {
            appearance: Costume::default(),
            voice: 0,
            individual_name: 0,
            affiliation: u32::MAX,
            nickname: 0,
        }
    }
}

impl NpcCharacterDesc {
    /// `ParseValue`: one key of the reader's current line; true when the key
    /// is one of `base_appr`, `head`, `body`, `lefthand`, `righthand`,
    /// `accessory`, `nickname`, `voice`, `individual_name` and
    /// `affiliation`. `nickname` also writes `out_nickname`.
    ///
    /// A ninth `accessory` is dropped where the C++ writes past the eight
    /// slots (D782).
    pub fn parse_value(
        &mut self,
        cx: &mut Cx<'_>,
        reader: &IniReader,
        out_nickname: &mut u32,
    ) -> bool {
        if reader.is_value(b"base_appr") {
            // The costume is copied only when its id resolves.
            if let Ok(id) = api::misc::costume_id(cx, &reader.value_string()) {
                self.appearance = api::gf::costume_by_id(cx, id);
            }
        } else if reader.is_value(b"head") {
            self.appearance.part_ids[0] = api::gf::body_part(cx, &reader.value_string(), 1) as u32;
        } else if reader.is_value(b"body") {
            self.appearance.part_ids[1] = api::gf::body_part(cx, &reader.value_string(), 0) as u32;
        } else if reader.is_value(b"lefthand") {
            self.appearance.part_ids[2] = api::gf::body_part(cx, &reader.value_string(), 2) as u32;
        } else if reader.is_value(b"righthand") {
            self.appearance.part_ids[3] = api::gf::body_part(cx, &reader.value_string(), 3) as u32;
        } else if reader.is_value(b"accessory") {
            let accessory = api::gf::accessory(cx, &reader.value_string()) as u32;
            if self.appearance.accessory_ids.len() < MAX_ACCESSORIES {
                self.appearance.accessory_ids.push(accessory);
            }
        } else if reader.is_value(b"nickname") {
            let id = create_id(neuron_core::hash::c_str(&reader.value_string()));
            *out_nickname = id;
            self.nickname = id;
        } else if reader.is_value(b"voice") {
            voice_id(&mut self.voice, &reader.value_string());
        } else if reader.is_value(b"individual_name") {
            self.individual_name = reader.value_int(0);
        } else if reader.is_value(b"affiliation") {
            self.affiliation = reputation_group(cx, &reader.value_string());
        } else {
            return false;
        }
        true
    }

    /// `IsComplete`: a body, a voice and a name.
    #[must_use]
    pub fn is_complete(&self) -> bool {
        self.appearance.part_ids[1] != 0 && self.voice != 0 && self.individual_name != 0
    }
}

/// `LoadNpcCharacterDesc`: [`NpcCharacterDesc::parse_value`] over every
/// remaining value of the section; true when a nickname was read and the
/// description is complete.
pub fn load_npc_character_desc(
    cx: &mut Cx<'_>,
    out_nickname: &mut u32,
    desc: &mut NpcCharacterDesc,
    reader: &mut IniReader,
) -> bool {
    while reader.read_value() {
        desc.parse_value(cx, reader, out_nickname);
    }
    *out_nickname != 0 && desc.is_complete()
}

/// `GSpecificNpcs`: the `[NPC]` descriptions by nickname id.
pub type SpecificNpcs = BTreeMap<u32, NpcCharacterDesc>;

/// The loop of StoryLauncher's constructor: every `[NPC]` of the reader
/// becomes a description (the loader's result is not tested), inserted
/// under its nickname id; a repeated nickname keeps the first.
pub fn load_specific_npcs(cx: &mut Cx<'_>, reader: &mut IniReader) -> SpecificNpcs {
    let mut npcs = SpecificNpcs::new();
    while reader.read_header() {
        if !reader.is_header(b"NPC") {
            continue;
        }
        let mut desc = NpcCharacterDesc::default();
        let mut nickname = 0;
        load_npc_character_desc(cx, &mut nickname, &mut desc, reader);
        npcs.entry(nickname).or_insert(desc);
    }
    npcs
}
