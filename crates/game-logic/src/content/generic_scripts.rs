//! The generic bar scripts of TU34 (`Content/GenericScripts.cpp`,
//! `scripts\gcs\genericScripts.ini`) and the bar behaviour builder that
//! picks them (work package P3.6e, record 0008 points 7 and 10).
//!
//! - **The table** ([`GenericScripts`], `GGenericScripts`): 0x108 script
//!   lists, by segment, gender and posture. BaseSupport's constructor loads
//!   it ([`load_generic_scripts`], `:84-141`) and its destructor empties it
//!   (`:144-148`); it is process-wide, so it lives in
//!   [`ContentState`](super::ContentState). The GCS exclusions it is picked
//!   against are a static table ([`super::tables::ContentTables::gcs_exclusions`]).
//! - **The pick** ([`pick_generic_script`], `:151-203`): a random script of
//!   a list that the character's voice is not excluded from.
//! - **The behaviour** ([`start_bar_character_script`], `:720-725`, over
//!   `BuildBarBehavior`, `:589-716`): a bar character on a placement gets
//!   the scripts of its role, its vnpc record and its offer, made with
//!   `pub::GF::CharacterBehaviorCreate`.
//!
//! The loader came with P3.6e rather than P3.6b (GCS and voices) because
//! BaseSupport's constructor and the bar characters need it.

use game_sim::ai::script::ScriptBehavior;
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::hash::{c_str, eq_ignore_case, stricmp};
use neuron_core::ini::IniReader;

use super::bar::{self, BarCharacter};
use super::players::PlayerState;
use super::random_mission::vendor::BarOffer;
use crate::api;
use crate::server::Cx;

/// `scripts\gcs\genericScripts.ini`, BaseSupport's constructor's file.
pub const FILE: &[u8] = b"scripts\\gcs\\genericScripts.ini";

/// The number of script lists: 0x42 segments, 2 genders, 2 postures.
pub const SLOTS: usize = 0x108;

/// `GGenericScriptSegments` (`:38-58`): the segment names; each value is
/// its index.
pub const SEGMENTS: [&[u8]; 0x42] = [
    b"FIDGET", b"A01aG", b"A02aG", b"A02cG", b"A02dG", b"A02eB", b"A03aG", b"A04aG", b"C01aR",
    b"D01aN", b"D01bG", b"D01cG", b"D01dG", b"D01fG", b"D01fGm", b"D01hG", b"D01kG", b"D01kGm",
    b"D02aP", b"D02aPm", b"D02bP", b"D02dP", b"D02eP", b"D03aN", b"D04aG", b"D04bB", b"D05aP",
    b"D05bP", b"D06aG", b"D07aG", b"D07bG", b"F01aB", b"F01bB", b"F01dG", b"F01eG", b"F01fG",
    b"F01jG", b"F02aP", b"F03aR", b"F03bR", b"F03cR", b"F03dR", b"F04aG", b"F04dG", b"F04fG",
    b"F04gP", b"F05aG", b"F05bG", b"F05cG", b"F06aP", b"F07aP", b"F07bP", b"F07cP", b"F08dR",
    b"F08bR", b"F08cR", b"F09bP", b"F10aR", b"F10bR", b"F10bRm", b"W01cG", b"W01bB", b"W02aF",
    b"W03bB", b"W04bP", b"W04cP",
];

/// `GGenericScripts`: the script names of each list, index
/// `posture + 2 * (gender + 2 * segment)`, where the loader's gender is 0
/// for `male` and its posture 0 for `stand`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GenericScripts {
    /// The 0x108 lists.
    pub slots: Vec<Vec<Vec<u8>>>,
}

impl Default for GenericScripts {
    fn default() -> Self {
        Self {
            slots: vec![Vec::new(); SLOTS],
        }
    }
}

impl GenericScripts {
    /// `UnloadGenericScripts` (`:144-148`): every list emptied.
    pub fn unload(&mut self) {
        self.slots.iter_mut().for_each(Vec::clear);
    }
}

/// `LoadGenericScripts(Path)` (`:84-141`) over an opened file: in every
/// `[GenericScripts]`, from gender 2, posture 2 and segment 0x42, each
/// `script` appended to the list its `set_gender` (`male` 0, else 1),
/// `set_posture` (`stand` 0, else 1) and `set_segment` (`_stricmp` against
/// the names, 0x42 when none matches) lines name.
///
/// A script before its `set_*` lines or after an unknown segment lands past
/// the table in the C++; the port drops it (D1000).
pub fn load_generic_scripts(table: &mut GenericScripts, reader: &mut IniReader) {
    while reader.read_header() {
        if !reader.is_header(b"GenericScripts") {
            continue;
        }
        let (mut gender, mut posture, mut segment) = (2usize, 2usize, 0x42usize);
        while reader.read_value() {
            if reader.is_value(b"set_gender") {
                gender = usize::from(!eq_ignore_case(c_str(&reader.value_string()), b"male"));
            } else if reader.is_value(b"set_posture") {
                posture = usize::from(!eq_ignore_case(c_str(&reader.value_string()), b"stand"));
            } else if reader.is_value(b"set_segment") {
                let name = reader.value_string();
                segment = SEGMENTS
                    .iter()
                    .position(|s| stricmp(s, c_str(&name)).is_eq())
                    .unwrap_or(0x42);
            } else if reader.is_value(b"script") {
                let script = c_str(&reader.value_string()).to_vec();
                let slot = posture + 2 * (gender + 2 * segment);
                if let Some(list) = table.slots.get_mut(slot) {
                    list.push(script);
                }
            }
        }
    }
}

/// BaseSupport's constructor's `LoadGenericScripts("scripts\\gcs\\genericScripts.ini")`
/// into Content's table; nothing when the file does not open.
pub fn load(cx: &mut Cx<'_>) {
    let source = super::data_source(cx);
    let mut reader = super::tables::open(&*source, FILE);
    load_generic_scripts(&mut cx.logic.content.state.generic_scripts, &mut reader);
}

/// The pick idiom (`PickIndex`, `:75-81`): index 0 of one without a draw,
/// else `rand() * n / 0x8000`.
fn pick_index(cx: &mut Cx<'_>, count: usize) -> usize {
    if count == 1 {
        return 0;
    }
    (cx.world.rng().rand().wrapping_mul(count as i32) / 0x8000) as usize
}

/// `PickGenericScript(Segment, Gender, Posture, VoiceId)` (`:151-203`): a
/// random script of the list (gender 1 the male lists, posture 0 the stand
/// lists), among those the voice's GCS exclusions do not name (the whole
/// name, without case); `None` when there is none.
pub fn pick_generic_script(
    cx: &mut Cx<'_>,
    segment: usize,
    gender: i32,
    posture: i32,
    voice: u32,
) -> Option<Vec<u8>> {
    let slot = usize::from(posture != 0) + 2 * (usize::from(gender != 1) + 2 * segment);
    let scripts = cx.logic.content.state.generic_scripts.slots.get(slot)?.clone();
    let tables = std::sync::Arc::clone(&cx.logic.content.tables);
    match tables.gcs_exclusions.get(&voice) {
        Some(excluded) => {
            let candidates: Vec<&Vec<u8>> = scripts
                .iter()
                .filter(|s| !excluded.iter().any(|n| eq_ignore_case(s, n)))
                .collect();
            if candidates.is_empty() {
                return None;
            }
            let index = pick_index(cx, candidates.len());
            candidates.get(index).map(|s| (*s).clone())
        }
        None => {
            if scripts.is_empty() {
                return None;
            }
            let index = pick_index(cx, scripts.len());
            scripts.get(index).cloned()
        }
    }
}

/// `PickFidgetScript(Gender, Posture)` (`:206-209`): segment 0 (`FIDGET`)
/// for voice 0.
pub fn pick_fidget_script(cx: &mut Cx<'_>, gender: i32, posture: i32) -> Option<Vec<u8>> {
    pick_generic_script(cx, 0, gender, posture, 0)
}

/// `CharacterBehaviorDescription` (`include/common_iface.h:1611-1660`):
/// what Content gives `pub::GF::CharacterBehaviorCreate`.
#[derive(Clone, Debug, PartialEq)]
pub struct BehaviorDescription {
    /// `Field00`: the room.
    pub field_00: i32,
    /// `Field04` (1).
    pub field_04: u8,
    /// `Field05`.
    pub field_05: u8,
    /// `Field06` (1).
    pub field_06: u8,
    /// `Field08`.
    pub field_08: u32,
    /// `Field0c`.
    pub field_0c: u32,
    /// `Field10` (1).
    pub field_10: u8,
    /// `IdArray` and `StringArray`, `IdArrayCount` of them.
    pub ids: Vec<(u32, Option<Vec<u8>>)>,
    /// `StringArray2` and `StringArray2Dword`.
    pub words: Vec<(Option<Vec<u8>>, u32)>,
    /// `Field21c`.
    pub field_21c: u32,
    /// `Field220`.
    pub field_220: u32,
    /// `Field224`.
    pub field_224: u32,
    /// `NameFmtStr`.
    pub name: FmtStr,
    /// `DescFmtStr`.
    pub description: FmtStr,
    /// `Field258`.
    pub field_258: u32,
    /// `Filter`: the session filter.
    pub filter: u32,
}

impl Default for BehaviorDescription {
    /// The constructor.
    fn default() -> Self {
        Self {
            field_00: 0,
            field_04: 1,
            field_05: 0,
            field_06: 1,
            field_08: 0,
            field_0c: 0,
            field_10: 1,
            ids: Vec::new(),
            words: Vec::new(),
            field_21c: 0,
            field_220: 0,
            field_224: 0,
            name: FmtStr::default(),
            description: FmtStr::default(),
            field_258: 0,
            filter: 0,
        }
    }
}

impl BehaviorDescription {
    /// `AppendBehaviorText(Behavior, Text, Value)` (`:212-221`): a text that
    /// is there and not empty is appended with the value. The C++ has 32
    /// slots and no bound; the port keeps every text.
    pub fn append_text(&mut self, text: Option<&[u8]>, value: u32) {
        if let Some(text) = text
            && !c_str(text).is_empty()
        {
            self.words.push((Some(c_str(text).to_vec()), value));
        }
    }

    /// `ScriptBehavior::init(Description)` (`Common/common_scriptbehavior.cpp:184-214`).
    #[must_use]
    pub fn script_behavior(&self) -> ScriptBehavior {
        ScriptBehavior {
            behavior_id: -1,
            field_04: self.field_00 as u32,
            flag_08: self.field_04 != 0,
            flag_09: self.field_05 != 0,
            flag_0a: self.field_06 != 0,
            field_0c: self.field_08,
            field_10: self.field_0c,
            flag_14: self.field_10 != 0,
            ids: self.ids.clone(),
            words: self.words.clone(),
            field_220: self.field_21c,
            field_224: self.field_220,
            field_228: self.field_224,
            name: self.name.clone(),
            description: self.description.clone(),
            field_25c: self.field_258,
        }
    }

    /// `pub::GF::CharacterBehaviorCreate(*this)`: the behaviour's handle.
    pub fn create(&self, cx: &mut Cx<'_>) -> u32 {
        api::gf::character_behavior_create(cx, &self.script_behavior(), self.filter)
    }

    /// `CreateBarBehavior` (`:568-575`): `0xffffffff` without a script,
    /// else the behaviour made.
    pub fn create_bar_behavior(&self, cx: &mut Cx<'_>) -> u32 {
        if self.words.is_empty() {
            return u32::MAX;
        }
        self.create(cx)
    }
}

/// The roles that sell something (`IsShopRole`, `:277-282`).
#[must_use]
pub fn is_shop_role(role: &[u8]) -> bool {
    [&b"trader"[..], b"shipdealer", b"equipment"]
        .iter()
        .any(|r| stricmp(r, c_str(role)).is_eq())
}

/// `IsBartenderRole` (`:284-287`).
#[must_use]
pub fn is_bartender_role(role: &[u8]) -> bool {
    stricmp(b"bartender", c_str(role)).is_eq()
}

/// The `rand() * 101 / 32768` roll (`RollPercent`, `:580-583`).
fn roll_percent(cx: &mut Cx<'_>) -> u32 {
    (cx.world.rng().rand().wrapping_mul(101) / 0x8000) as u32
}

/// What the builder reads of a character, its state and its place.
struct Built<'a> {
    character: &'a BarCharacter,
    posture: i32,
    state: &'a PlayerState,
    base: u32,
    single: bool,
}

impl Built<'_> {
    /// `AppendGenericScript` (`:296-303`): a script of the segment for the
    /// character's gender, posture and voice, appended with the value.
    fn append(&self, cx: &mut Cx<'_>, b: &mut BehaviorDescription, segment: usize, value: u32) {
        let script = pick_generic_script(
            cx,
            segment,
            self.character.gender,
            self.posture,
            self.character.voice,
        );
        b.append_text(script.as_deref(), value);
    }

    fn shop(&self) -> bool {
        is_shop_role(&self.character.role)
    }

    fn bartender(&self) -> bool {
        is_bartender_role(&self.character.role)
    }

    /// `BuildBartenderScripts` (`:381-405`).
    fn bartender_scripts(&self, cx: &mut Cx<'_>, b: &mut BehaviorDescription, vnpc: u32) {
        if self.character.talk_count != 0 {
            self.append(cx, b, if self.single { 0xd } else { 0xe }, 0);
            self.append(cx, b, 0x15, 0);
            return;
        }
        if !self.single {
            self.append(cx, b, 0x11, 0);
            self.append(cx, b, 0x1b, 0);
            return;
        }
        if vnpc > 1 {
            self.append(cx, b, 0x10, 0);
        } else {
            self.append(cx, b, 0xf, 0);
            self.append(cx, b, 0x16, 0);
            self.append(cx, b, 0x17, 0);
        }
        self.append(cx, b, 0x1b, 0);
    }

    /// `BuildVnpcScripts` (`:409-449`): false only for a vnpc value 3 of 1,
    /// which ends the builder without offer scripts.
    fn vnpc_scripts(&self, cx: &mut Cx<'_>, b: &mut BehaviorDescription, vnpc: u32) -> bool {
        if self.shop() {
            return true;
        }
        if self.bartender() {
            self.bartender_scripts(cx, b, vnpc);
            return true;
        }
        let value = self
            .state
            .find_vnpc_value3(self.base, self.character.nickname)
            .unwrap_or(0);
        if value == 0 {
            self.bartender_scripts(cx, b, vnpc);
            return true;
        }
        if value == 1 {
            self.append(cx, b, 0xc, 0);
            return false;
        }
        if !self.single {
            return true;
        }
        if value == 2 {
            self.append(cx, b, 0xb, 0);
            self.append(cx, b, 0x1b, 0);
        } else {
            self.append(cx, b, 0xa, 0);
            self.append(cx, b, 0x14, 0);
        }
        true
    }

    /// `BuildMissionOfferScripts` (`:331-377`).
    fn mission_offer_scripts(&self, cx: &mut Cx<'_>, b: &mut BehaviorDescription) {
        b.field_21c = 3;
        self.append(cx, b, 0x24, 0);
        if self.state.live_mission & 0x3fff_ffff != 0 {
            self.append(cx, b, 0x2c, 0);
            return;
        }
        let BarOffer::Mission(data) = &self.character.offer else {
            return;
        };
        if let Err((_, code)) = data.loader.can_accept(cx) {
            match code {
                1 => self.append(cx, b, 0x2b, 0),
                2 => self.append(cx, b, 0x2a, 0),
                _ => {}
            }
            return;
        }
        let mission = bar::offer_mission(cx, &self.character.offer);
        if self.single {
            self.append(cx, b, 0x2d, 0);
            let difficulty = mission.as_ref().map_or(0.0, |m| m.difficulty);
            let fit = bar::compare_rm_offer_difficulty(cx, self.state.mission_number, difficulty);
            let segment = match fit {
                f if f < 0 => 0x2e,
                0 => 0x2f,
                _ => 0x30,
            };
            self.append(cx, b, segment, 0);
        } else {
            self.append(cx, b, 0x31, 0);
        }
        self.append(cx, b, 8, 1);
        self.append(cx, b, 0x32, 2);
        self.append(cx, b, 0x39, 2);
        self.append(cx, b, 0x34, 3);
        self.append(cx, b, if self.single { 0x3a } else { 0x3b }, 3);
        self.append(cx, b, 0x2c, 6);
        if let Some(m) = mission {
            b.name = m.description;
        }
    }

    /// `BuildOfferScripts` (`:453-565`). A failed cash read leaves the C++'s
    /// cash holding the posture (a bribe) or the bits of the character
    /// pointer (knowledge); the port reads the posture and "enough"
    /// (D1001).
    fn offer_scripts(&self, cx: &mut Cx<'_>, b: &mut BehaviorDescription, vnpc: bool) {
        let tail = if self.single { 0x3a } else { 0x3b };
        match &self.character.offer {
            BarOffer::Mission(_) => self.mission_offer_scripts(cx, b),
            BarOffer::Bribe(bribe) => {
                self.append(cx, b, 0x23, 0);
                self.append(cx, b, 0x25, 0);
                self.append(cx, b, 8, 1);
                self.append(cx, b, 0x32, 2);
                self.append(cx, b, 0x34, 3);
                let cash = api::player::inspect_cash(cx, self.character.player)
                    .unwrap_or(self.posture);
                let short = (cash as u32) < (bribe.price as u32);
                self.append(cx, b, if short { 0x35 } else { 0x39 }, 2);
                self.append(cx, b, tail, 3);
                let mut text = FmtStr::new(bribe.text_ids as u32);
                text.append_rep_group(bribe.group);
                text.append_int(bribe.price as u32);
                for &id in bribe.ids.iter().take_while(|&&id| id != u32::MAX) {
                    text.append_rep_group(id);
                }
                b.name = text;
                b.field_21c = 1;
            }
            BarOffer::Know(know) => {
                self.append(cx, b, if self.bartender() { 0x20 } else { 0x22 }, 0);
                self.append(cx, b, 0x25, 0);
                match know.rep_level {
                    0 | 1 => self.append(cx, b, 0x26, 0),
                    2 => self.append(cx, b, 0x27, 0),
                    3 => self.append(cx, b, 0x28, 0),
                    _ => {}
                }
                self.append(cx, b, 8, 1);
                let cash = api::player::inspect_cash(cx, self.character.player)
                    .map_or(u32::MAX, |c| c as u32);
                if cash < know.price as u32 {
                    self.append(cx, b, 0x35, 2);
                } else {
                    self.append(cx, b, 8, 4);
                    self.append(cx, b, 0x38, 2);
                    self.append(cx, b, 0x39, 2);
                }
                self.append(cx, b, 0x33, 3);
                self.append(cx, b, tail, 3);
                // `FormatKnowOfferText` (`:318-327`): the draw is made, and
                // `rand() / 0x8000` is always 0.
                let mut description = FmtStr::new(0x3efe);
                description.append_string(know.field_0x04 as u32);
                b.description = description;
                let id = (cx.world.rng().rand() / 0x8000 + 0x3efd) as u32;
                let mut name = FmtStr::new(id);
                name.append_string(know.field_0x08 as u32);
                name.append_int(know.price as u32);
                b.name = name;
                b.field_21c = 4;
            }
            BarOffer::Rumor(rumor) => {
                self.append(cx, b, if self.bartender() { 0x1f } else { 0x21 }, 0);
                self.append(cx, b, 0x25, 0);
                self.append(cx, b, 0x36, 0);
                self.append(cx, b, 8, 1);
                self.append(cx, b, 0x38, 0);
                self.append(cx, b, 0x39, 0);
                // The ids after the first up to the first 0; the C++ reads
                // on past the eight (D1001).
                if let Some(rumor) = rumor {
                    let mut text = FmtStr::new(rumor.ids[0] as u32);
                    for &id in rumor.ids[1..].iter().take_while(|&&id| id != 0) {
                        text.append_string(id as u32);
                    }
                    b.description = text;
                }
                b.field_21c = 2;
            }
            BarOffer::None => {
                if self.bartender() {
                    self.append(cx, b, 5, 0);
                    return;
                }
                if self.shop() {
                    return;
                }
                if !vnpc {
                    self.append(cx, b, 1, 0);
                }
                self.append(cx, b, 2, 0);
            }
        }
    }
}

/// `BuildBarBehavior(Character)` (`:589-716`): the behaviour of a bar
/// character: the scripts of its role for a character without a vnpc
/// record (the bartender's greetings, a patron's, nothing for a shop), then
/// for a character whose affiliation does not dislike the player its vnpc
/// and offer scripts, for a disliked shop or bartender the same, and for a
/// disliked patron its brush-off; `0xffffffff` when no script was added.
///
/// A character whose player has no state, which the C++ dereferences, gets
/// none (D1001).
fn build_bar_behavior(cx: &mut Cx<'_>, base: u32, key: u32) -> u32 {
    let Some(character) = bar::character(cx, base, key).cloned() else {
        return u32::MAX;
    };
    let shop = is_shop_role(&character.role);
    let mut b = BehaviorDescription {
        field_00: character.room as i32,
        field_05: 0,
        field_06: 1,
        field_10: 1,
        field_04: 0,
        field_08: 0,
        field_0c: 0,
        field_224: 1,
        field_21c: if shop { 2 } else { 0 },
        field_220: character.character_id,
        filter: character.player,
        ..BehaviorDescription::default()
    };
    let Some(state) = cx.logic.content.state.players.get(&character.player).cloned() else {
        return u32::MAX;
    };
    // A failed lookup leaves the C++'s posture at 0.
    let posture = usize::try_from(character.placement)
        .ok()
        .and_then(|p| api::gf::character_placement_posture(cx, character.room, p).ok())
        .unwrap_or(0) as i32;
    let state_base = state.base.unwrap_or(0);
    let vnpc = state.vnpc_value2(state_base, character.nickname);
    let attitude =
        api::reputation::reputation(cx, state.reputation, character.affiliation).unwrap_or(0.0);
    let friendly = attitude >= 0.0;
    let single = api::misc::single_player(cx);
    let built = Built {
        character: &character,
        posture,
        state: &state,
        base: state_base,
        single,
    };
    let bartender = built.bartender();
    let has_offer = character.offer.kind() != 0;

    if vnpc == 0 {
        if bartender {
            built.append(cx, &mut b, 0x3d, 0);
            built.append(cx, &mut b, 0x3f, 0);
            built.append(cx, &mut b, 0x40, 0);
            let chance = roll_percent(cx) < 0x19;
            if single && chance {
                for segment in [9, 0x12, 0x19, 0x1a] {
                    built.append(cx, &mut b, segment, 0);
                }
            } else {
                built.append(cx, &mut b, 9, 0);
                built.append(cx, &mut b, 0x13, 0);
            }
            if roll_percent(cx) < 0x14 {
                built.append(cx, &mut b, 0x3e, 0);
            }
        } else if !shop {
            built.append(cx, &mut b, 0x3c, 0);
            built.append(cx, &mut b, 0x41, 0);
            let chance = roll_percent(cx) < 0x19;
            if single && chance {
                for segment in [9, 0x12, 0x18, 0x1a] {
                    built.append(cx, &mut b, segment, 0);
                }
            } else {
                built.append(cx, &mut b, 9, 0);
                built.append(cx, &mut b, 0x13, 0);
            }
            built.append(cx, &mut b, 0x1c, 0);
            let local = state
                .base
                .and_then(|id| cx.logic.content.state.bases.get(&id))
                .map_or(u32::MAX, |r| r.local_faction);
            let segment = if local == character.affiliation {
                0x1d
            } else {
                0x1e
            };
            built.append(cx, &mut b, segment, 0);
        }
    }

    if friendly {
        if vnpc != 0 {
            if !built.vnpc_scripts(cx, &mut b, vnpc) {
                return b.create_bar_behavior(cx);
            }
        } else if has_offer && roll_percent(cx) < 0x14 && !shop && !bartender {
            built.append(cx, &mut b, 1, 0);
            built.append(cx, &mut b, 4, 0);
        }
        built.offer_scripts(cx, &mut b, vnpc != 0);
    } else if shop || bartender {
        if vnpc != 0 && !built.vnpc_scripts(cx, &mut b, vnpc) {
            return b.create_bar_behavior(cx);
        }
        built.offer_scripts(cx, &mut b, vnpc != 0);
    } else {
        if vnpc == 0 {
            built.append(cx, &mut b, 1, 0);
        }
        built.append(cx, &mut b, 3, 0);
        if vnpc != 0 {
            built.append(cx, &mut b, 6, 0);
            built.append(cx, &mut b, 7, 0);
        }
    }
    b.create_bar_behavior(cx)
}

/// `StartBarCharacterScript(Character)` (`:720-725`): a character on a
/// placement gets the behaviour [`build_bar_behavior`] makes
/// ([`bar::set_behavior`]); one off any placement keeps what it has.
pub fn start_bar_character_script(cx: &mut Cx<'_>, base: u32, key: u32) {
    let Some(character) = bar::character(cx, base, key) else {
        return;
    };
    if character.placement == -1 {
        return;
    }
    let behavior = build_bar_behavior(cx, base, key);
    bar::set_behavior(cx, base, key, behavior);
}
