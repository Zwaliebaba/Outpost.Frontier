//! The bar characters of TU17 (`Content/BaseDB.cpp:600-1520`): the people
//! standing in a base's rooms while players are in them, their offers and
//! the bar scenes of the RTC scripts (work package P3.6e, record 0008
//! points 7 and 10).
//!
//! - **Room states** ([`RoomState`], `FBaseRoomState`, `ContentBase`
//!   `+0x70`): per room, the placement locks, the characters (the front of
//!   the list first) and one reference per visit.
//! - **A player entering** ([`on_player_enter`], `FContentBase::OnPlayerEnter`,
//!   `:1411-1495`, GameSupport's `BaseEnter`): one character per room
//!   fixture for the player, unless a lock holds its placement or the
//!   player already has the fixture's role there; each new character gets
//!   an offer ([`choose_offer`], `FBaseNpc::ChooseOffer`, `:1521-1700`) and
//!   a script ([`super::generic_scripts::start_bar_character_script`]).
//! - **A player leaving** ([`on_player_exit`], `:1498-1518`): the visit's
//!   references dropped, a character whose uses reach 0 destroyed
//!   ([`remove_bar_character`]).
//! - **Bar scenes** ([`create_bar_scene`], [`remove_bar_scene`],
//!   `:1059-1272`): an RTC script's characters and locked placements for one
//!   player, which P3.5c4's cut scenes create and remove.
//!
//! Where the C++ holds a list iterator the port holds the character's key
//! ([`BarCharacter::key`], unique within its base) and looks the character
//! up again after every call out (record 0008 point 1). A mission offer owns
//! the loader of its random mission; dropping the offer releases it
//! ([`release_offer`]), as the C++'s last reference does.

use game_sim::data::costume::Costume;
use game_sim::protocol::base_props::CharacterProperties;
use game_sim::protocol::costume::Costume as WireCostume;
use neuron_core::hash::{c_str, stricmp};
use neuron_core::num::trunc_to_i32;

use super::bases::{BaseNpc, BaseNpcBribe, BaseNpcKnow, BaseNpcRumor, ContentBase, REP_THRESHOLDS};
use super::generic_scripts;
use super::npc_desc::NpcCharacterDesc;
use super::players::PlayerState;
use super::random_mission::tables::round_money;
use super::random_mission::vendor::{self, BarOffer, BribeOffer};
use super::tables::factions::faction_hostile_groups;
use crate::api;
use crate::server::Cx;

/// `BAR_OFFER_*`: the weights of `ChooseOffer`'s first pick.
const MISSION_WEIGHT: f32 = 30.0;
const BRIBE_WEIGHT: f32 = 20.0;
const KNOW_WEIGHT: f32 = 15.0;

/// A `TString<N>` assignment: at most `N - 1` bytes, up to the first NUL.
fn tstring(text: &[u8], size: usize) -> Vec<u8> {
    let mut text = c_str(text).to_vec();
    text.truncate(size - 1);
    text
}

/// `FBarCharacterDesc` (0xb8 bytes): what `AddBarCharacter` is given.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BarCharacterDesc {
    /// `PlayerId`: the session filter (0 by default).
    pub player: u32,
    /// `Affiliation`.
    pub affiliation: u32,
    /// `IndividualName`.
    pub individual_name: i32,
    /// `Appearance`.
    pub appearance: Costume,
    /// `Voice`.
    pub voice: u32,
    /// `RoomId`.
    pub room: u32,
    /// `Placement` (-1 by default).
    pub placement: i32,
    /// `Role` (a `TString<32>`).
    pub role: Vec<u8>,
    /// `Fidget` (a `TString<64>`).
    pub fidget: Vec<u8>,
    /// `Nickname` (cleared by default).
    pub nickname: u32,
}

impl Default for BarCharacterDesc {
    /// `FBarCharacterDesc::FBarCharacterDesc` (`:767-781`). The fields the
    /// C++ leaves unset start at 0.
    fn default() -> Self {
        Self {
            player: 0,
            affiliation: 0,
            individual_name: 0,
            appearance: Costume::default(),
            voice: 0,
            room: 0,
            placement: -1,
            role: Vec::new(),
            fidget: Vec::new(),
            nickname: 0,
        }
    }
}

impl BarCharacterDesc {
    /// The description of an NPC record (`FNpcCharacterDesc`) for `player`
    /// in `room`, as `OnPlayerEnter` and `CreateBarScene` fill it.
    #[must_use]
    pub fn from_npc(npc: &NpcCharacterDesc, player: u32, room: u32) -> Self {
        Self {
            player,
            affiliation: npc.affiliation,
            individual_name: npc.individual_name,
            appearance: npc.appearance.clone(),
            voice: npc.voice,
            room,
            ..Self::default()
        }
    }
}

/// `FBarCharacter` (0x5c bytes): a character in a room's list.
#[derive(Clone, Debug, PartialEq)]
pub struct BarCharacter {
    /// The port's key for the C++'s list iterator, unique within the base.
    pub key: u32,
    /// `UseCount` (1): one per visit or scene holding it.
    pub use_count: i32,
    /// `Behavior` (`0xffffffff`): `CharacterBehaviorCreate`'s handle.
    pub behavior: u32,
    /// `CharacterId`: `CharacterCreate`'s handle.
    pub character_id: u32,
    /// `RoomId`.
    pub room: u32,
    /// `Affiliation`.
    pub affiliation: u32,
    /// `Placement`.
    pub placement: i32,
    /// `Role` (a `TString<32>`).
    pub role: Vec<u8>,
    /// `PlayerId`.
    pub player: u32,
    /// `Gender` (`GetCostumeSkeletonGender`).
    pub gender: i32,
    /// `Voice`.
    pub voice: u32,
    /// `Nickname`: the GF_NPC's nickname id.
    pub nickname: u32,
    /// `TalkCount`: counted by `GFObjSelect`.
    pub talk_count: i32,
    /// `Offer`.
    pub offer: BarOffer,
}

/// `FBarPlacementLock`: a placement a bar scene holds for a player.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PlacementLock {
    /// `PlayerId`.
    pub player: u32,
    /// `Placement`.
    pub placement: i32,
    /// `Count`: 1 on insert, one more per repeat.
    pub count: i32,
}

/// `FBaseRoomState` (0x24 bytes): one room's bar state.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RoomState {
    /// `Locks`.
    pub locks: Vec<PlacementLock>,
    /// `Characters`, the front of the list first.
    pub characters: Vec<BarCharacter>,
    /// `CharacterRefs`: one character key per visit reference.
    pub character_refs: Vec<u32>,
}

impl RoomState {
    /// `HasLock(PlayerId, Placement)` (`:944-952`).
    #[must_use]
    pub fn has_lock(&self, player: u32, placement: i32) -> bool {
        self.locks
            .iter()
            .any(|l| l.player == player && l.placement == placement)
    }

    /// `HasCharacter(PlayerId, Placement)` (`:955-963`).
    #[must_use]
    pub fn has_character(&self, player: u32, placement: i32) -> bool {
        self.characters
            .iter()
            .any(|c| c.player == player && c.placement == placement)
    }

    /// `AddLock(PlayerId, Placement)` (`:966-981`).
    pub fn add_lock(&mut self, player: u32, placement: i32) {
        if let Some(lock) = self
            .locks
            .iter_mut()
            .find(|l| l.player == player && l.placement == placement)
        {
            lock.count += 1;
            return;
        }
        self.locks.push(PlacementLock {
            player,
            placement,
            count: 1,
        });
    }

    /// `FindCharacter(Role, PlayerId, &Out)` (`:984-996`): the first
    /// character of the player whose role equals `role` (`_stricmp`).
    #[must_use]
    pub fn find_character(&self, role: &[u8], player: u32) -> Option<&BarCharacter> {
        self.characters
            .iter()
            .find(|c| stricmp(c_str(role), &c.role).is_eq() && c.player == player)
    }

    /// `CountCharacters(PlayerId)` (`:999-1008`).
    #[must_use]
    pub fn count_characters(&self, player: u32) -> i32 {
        self.characters
            .iter()
            .filter(|c| c.player == player)
            .count() as i32
    }
}

/// `FBarScene` (0x30 bytes): an RTC script's characters and locked
/// placements for one player.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BarScene {
    /// `SceneId`.
    pub scene_id: i32,
    /// `PlayerId`.
    pub player: u32,
    /// `RoomId`.
    pub room: u32,
    /// `Placements`: one lock each.
    pub placements: Vec<i32>,
    /// `Characters`: one use each, the first the scene's behaviour holder.
    pub characters: Vec<u32>,
}

/// One `[Char]` role of an RTC script (`FRtcScriptRole`, 0x3c bytes).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RtcScriptRole {
    /// `Role` (a `TString<32>`; empty for no fixed character).
    pub role: Vec<u8>,
    /// `Character`: the specific NPC the role names (`GSpecificNpcs`).
    pub character: Option<NpcCharacterDesc>,
    /// `Spot`: `GetCharacterPlacementByName`, -1 initially.
    pub spot: i32,
    /// `Fidget`.
    pub fidget: Vec<u8>,
}

/// An RTC script (`FCharacterEncounter`, TU23
/// `Content/RtcScripts.h:26-59`), which the bar scene creator reads; its
/// loader is [`super::rtc_scripts::load`] (P3.5c4).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CharacterEncounter {
    /// `LocationId`.
    pub location: u32,
    /// `BaseId`: the `Location` line's base.
    pub base_id: u32,
    /// `bAutoplay`.
    pub autoplay: bool,
    /// `Roles`, one per `[Char]`.
    pub roles: Vec<RtcScriptRole>,
    /// `ReservedSpots`, the `[Reserve]` placements.
    pub reserved_spots: Vec<i32>,
    /// `MissionTextId`.
    pub mission_text_id: i32,
    /// `NoOfferTextId`.
    pub no_offer_text_id: i32,
    /// `LoadedMissionTextId`: `mission_text_id` as read.
    pub loaded_mission_text_id: i32,
    /// `RelocateLocationId`.
    pub relocate_location: u32,
    /// `StartRoomLocationId`.
    pub start_room_location: u32,
    /// `SequenceId`.
    pub sequence_id: u32,
    /// `Actions`: `{type, text}` (greet 0, walkup 1, action 2, offer 3,
    /// decision 4, accept 5, decline 6).
    pub actions: Vec<(i32, Vec<u8>)>,
}

impl CharacterEncounter {
    /// `FindActionText(Type)`: the text of the first action of the type.
    #[must_use]
    pub fn find_action_text(&self, kind: i32) -> Option<&[u8]> {
        self.actions
            .iter()
            .find(|(k, _)| *k == kind)
            .map(|(_, t)| t.as_slice())
    }
}

/// The base record of `base`.
fn base_ref<'a>(cx: &'a Cx<'_>, base: u32) -> Option<&'a ContentBase> {
    cx.logic.content.state.bases.get(&base)
}

/// The room and the place in its list of the character `key`.
fn locate(base: &ContentBase, key: u32) -> Option<(u32, usize)> {
    base.room_states.iter().find_map(|(&room, state)| {
        state
            .characters
            .iter()
            .position(|c| c.key == key)
            .map(|at| (room, at))
    })
}

/// The character `key` of `base`.
#[must_use]
pub fn character<'a>(cx: &'a Cx<'_>, base: u32, key: u32) -> Option<&'a BarCharacter> {
    let record = base_ref(cx, base)?;
    let (room, at) = locate(record, key)?;
    record.room_states.get(&room).map(|s| &s.characters[at])
}

/// The character `key` of `base`, to change between call-outs.
pub fn character_mut<'a>(cx: &'a mut Cx<'_>, base: u32, key: u32) -> Option<&'a mut BarCharacter> {
    let record = cx.logic.content.state.bases.get_mut(&base)?;
    let (room, at) = locate(record, key)?;
    record
        .room_states
        .get_mut(&room)
        .map(|s| &mut s.characters[at])
}

/// The keys of a room's characters, the front of the list first (empty
/// without a room state).
#[must_use]
pub fn room_character_keys(cx: &Cx<'_>, base: u32, room: u32) -> Vec<u32> {
    base_ref(cx, base)
        .and_then(|b| b.room_states.get(&room))
        .map(|s| s.characters.iter().map(|c| c.key).collect())
        .unwrap_or_default()
}

/// `~FBarOffer` reaching the last reference: a mission offer's
/// `~FBarMissionOffer` gives its loader slot 5 (empty) and `Release`.
pub fn release_offer(cx: &mut Cx<'_>, offer: BarOffer) {
    if let BarOffer::Mission(mut data) = offer {
        data.release(cx);
    }
}

/// `FBarCharacter::SetOffer(&Offer)` (`:878-881`): the new offer replaces
/// the old one, which is released.
pub fn set_offer(cx: &mut Cx<'_>, base: u32, key: u32, offer: BarOffer) {
    let Some(character) = character_mut(cx, base, key) else {
        release_offer(cx, offer);
        return;
    };
    let old = std::mem::replace(&mut character.offer, offer);
    release_offer(cx, old);
}

/// `FBarCharacter::SetBehavior(NewBehavior)` (`:869-875`): an attached
/// behaviour destroyed, the new one set on the record and kept.
pub fn set_behavior(cx: &mut Cx<'_>, base: u32, key: u32, behavior: u32) {
    let Some(c) = character(cx, base, key) else {
        return;
    };
    let (mut old, id) = (c.behavior, c.character_id);
    if old != u32::MAX {
        api::gf::character_behavior_destroy(cx, &mut old);
    }
    api::gf::character_set_behavior(cx, id, behavior);
    if let Some(c) = character_mut(cx, base, key) {
        c.behavior = behavior;
    }
}

/// `FBarCharacter::FBarCharacter` and `Create(Desc)` (`:784-866`): the
/// character's costume gender, its script (the fidget, or a generic fidget
/// script of its gender and placement posture when it has none), its
/// placement's name, then `pub::GF::CharacterCreate` with the player as
/// the session filter.
///
/// A failed posture lookup leaves the C++'s posture holding the bits of the
/// description pointer; the port reads posture 0 (D1001).
pub fn create(cx: &mut Cx<'_>, desc: &BarCharacterDesc, key: u32) -> BarCharacter {
    let gender = api::gf::costume_skeleton_gender(cx, &desc.appearance);
    let script = if desc.fidget.is_empty() {
        let posture = usize::try_from(desc.placement)
            .ok()
            .and_then(|p| api::gf::character_placement_posture(cx, desc.room, p).ok())
            .unwrap_or(0);
        generic_scripts::pick_fidget_script(cx, gender, posture as i32)
    } else {
        Some(desc.fidget.clone())
    };
    let location = usize::try_from(desc.placement)
        .ok()
        .map(|p| api::gf::character_placement_name(cx, desc.room, p));
    // `CharacterProperties::init` of the description.
    let props = CharacterProperties {
        key: 0,
        session_id: 0,
        room_id: desc.room,
        name_ids: desc.individual_name as u32,
        faction_id: desc.affiliation,
        name_wide: None,
        name_ansi: location,
        char_type: 0,
        posture: 0,
        costume: WireCostume {
            parts: desc.appearance.part_ids,
            accessories: desc.appearance.accessory_ids.clone(),
        },
        script,
        behavior_id: u32::MAX,
        voice_id: desc.voice,
    };
    let character_id = api::gf::character_create(cx, &props, desc.player);
    BarCharacter {
        key,
        use_count: 1,
        behavior: u32::MAX,
        character_id,
        room: desc.room,
        affiliation: desc.affiliation,
        placement: desc.placement,
        role: tstring(&desc.role, 32),
        player: desc.player,
        gender,
        voice: desc.voice,
        nickname: desc.nickname,
        talk_count: 0,
        offer: BarOffer::None,
    }
}

/// The next character key of `base` (0 for a base without a record).
fn next_key(cx: &mut Cx<'_>, base: u32) -> u32 {
    match cx.logic.content.state.bases.get_mut(&base) {
        Some(record) => {
            record.last_character_key = record.last_character_key.wrapping_add(1);
            record.last_character_key
        }
        None => 0,
    }
}

/// Puts a made character at the front of its room's list, the room state
/// made on demand (an existing one is kept). `None` for a base without a
/// record.
fn insert_front(cx: &mut Cx<'_>, base: u32, character: BarCharacter) -> Option<u32> {
    let record = cx.logic.content.state.bases.get_mut(&base)?;
    let key = character.key;
    record
        .room_states
        .entry(character.room)
        .or_default()
        .characters
        .insert(0, character);
    Some(key)
}

/// `FContentBase::AddBarCharacter(Desc)` (`:1011-1023`): the character made
/// and put at the front of its room's list. Its key, `None` for a base
/// without a record (where the character is made all the same).
pub fn add_bar_character(cx: &mut Cx<'_>, base: u32, desc: &BarCharacterDesc) -> Option<u32> {
    let key = next_key(cx, base);
    let character = create(cx, desc, key);
    insert_front(cx, base, character)
}

/// `FContentBase::RemoveBarCharacter(Unused, Character, KeepRoles)`
/// (`:1026-1056`): one use dropped; at none, unless the character's role is
/// one of `keep_roles` (`strcmp`), a character with an offer loses its
/// behaviour, the character's record is destroyed and the character leaves
/// its room (its offer released).
pub fn remove_bar_character(cx: &mut Cx<'_>, base: u32, key: u32, keep_roles: Option<&[Vec<u8>]>) {
    let Some(c) = character_mut(cx, base, key) else {
        return;
    };
    c.use_count -= 1;
    if c.use_count != 0 {
        return;
    }
    if let Some(keep) = keep_roles
        && keep.iter().any(|r| c_str(r) == c.role.as_slice())
    {
        return;
    }
    let (has_offer, mut behavior, id) = (c.offer.kind() != 0, c.behavior, c.character_id);
    if has_offer {
        if behavior != u32::MAX {
            api::gf::character_behavior_destroy(cx, &mut behavior);
        }
        api::gf::character_set_behavior(cx, id, u32::MAX);
        if let Some(c) = character_mut(cx, base, key) {
            c.behavior = u32::MAX;
        }
    }
    let mut handle = id;
    api::gf::character_destroy(cx, &mut handle);
    let removed = cx
        .logic
        .content
        .state
        .bases
        .get_mut(&base)
        .and_then(|record| {
            let (room, at) = locate(record, key)?;
            record
                .room_states
                .get_mut(&room)
                .map(|s| s.characters.remove(at))
        });
    if let Some(removed) = removed {
        release_offer(cx, removed.offer);
    }
}

/// `FContentBase::FindBarCharacter(&Out, &OutRoom, CharacterId)`
/// (`:1274-1292`): the room and key of the character with this
/// `CharacterCreate` handle, rooms in id order.
#[must_use]
pub fn find_bar_character(base: &ContentBase, character_id: u32) -> Option<(u32, u32)> {
    base.room_states.iter().find_map(|(&room, state)| {
        state
            .characters
            .iter()
            .find(|c| c.character_id == character_id)
            .map(|c| (room, c.key))
    })
}

/// `FContentBase::OnPlayerEnter(PlayerId)` (`:1411-1495`), from
/// GameSupport's `BaseEnter`: for every fixture of every room, nothing when
/// a lock holds its placement for the player; the player's character of
/// the fixture's role (`_stricmp`) gets one more use and one more visit
/// reference; otherwise the fixture's GF_NPC becomes a character for the
/// player with a visit reference, an offer ([`choose_offer`]) and its
/// script. Nothing for a base without a record.
pub fn on_player_enter(cx: &mut Cx<'_>, base: u32, player: u32) {
    let Some(record) = base_ref(cx, base) else {
        return;
    };
    let rooms = record.rooms.clone();
    for room in &rooms {
        for fixture in &room.fixtures {
            let reused =
                {
                    let Some(record) = cx.logic.content.state.bases.get_mut(&base) else {
                        return;
                    };
                    match record.room_states.get_mut(&room.room_id) {
                        None => false,
                        Some(state) if state.has_lock(player, fixture.placement) => true,
                        Some(state) => {
                            match state.characters.iter_mut().find(|c| {
                                stricmp(&fixture.role, &c.role).is_eq() && c.player == player
                            }) {
                                Some(c) => {
                                    c.use_count += 1;
                                    let key = c.key;
                                    state.character_refs.push(key);
                                    true
                                }
                                None => false,
                            }
                        }
                    }
                };
            if reused {
                continue;
            }
            // The fixture's GF_NPC (the C++ holds a pointer into the map).
            let Some(npc) = base_ref(cx, base).and_then(|b| b.npcs.get(&fixture.npc).cloned())
            else {
                continue;
            };
            let desc = BarCharacterDesc {
                placement: fixture.placement,
                role: fixture.role.clone(),
                fidget: fixture.fidget.clone(),
                nickname: npc.desc.nickname,
                ..BarCharacterDesc::from_npc(&npc.desc, player, room.room_id)
            };
            let Some(key) = add_bar_character(cx, base, &desc) else {
                continue;
            };
            if let Some(state) = cx
                .logic
                .content
                .state
                .bases
                .get_mut(&base)
                .and_then(|b| b.room_states.get_mut(&room.room_id))
            {
                state.character_refs.push(key);
            }
            let offer = choose_offer(cx, &npc, player);
            set_offer(cx, base, key, offer);
            generic_scripts::start_bar_character_script(cx, base, key);
        }
    }
}

/// `FContentBase::OnPlayerExit(PlayerId)` (`:1498-1518`), from
/// GameSupport's `BaseExit`: in every room (by id), each visit reference to
/// a character of the player drops a use ([`remove_bar_character`]) and is
/// erased. A reference whose character is gone is erased too (the C++
/// reads the freed node; D1001).
pub fn on_player_exit(cx: &mut Cx<'_>, base: u32, player: u32) {
    let Some(record) = base_ref(cx, base) else {
        return;
    };
    let rooms: Vec<u32> = record.room_states.keys().copied().collect();
    for room in rooms {
        let mut at = 0;
        while let Some(key) = base_ref(cx, base)
            .and_then(|b| b.room_states.get(&room))
            .and_then(|s| s.character_refs.get(at).copied())
        {
            let owner = character(cx, base, key).map(|c| c.player);
            if owner.is_some_and(|p| p != player) {
                at += 1;
                continue;
            }
            if owner.is_some() {
                remove_bar_character(cx, base, key, None);
            }
            if let Some(state) = cx
                .logic
                .content
                .state
                .bases
                .get_mut(&base)
                .and_then(|b| b.room_states.get_mut(&room))
                && at < state.character_refs.len()
            {
                state.character_refs.remove(at);
            }
        }
    }
}

/// The weighted pick of `ChooseOffer` (`PickWeighted`, `BaseDB.cpp:641-672`):
/// the sum and the running total in double, `None` for a sum of 0; the
/// roll `(float)(rand() * 2^-15 * (float)sum)`, the first entry the roll
/// does not pass. The float the pick leaves in its stack slot comes back
/// with it (the sum, then the roll).
fn pick_weighted(cx: &mut Cx<'_>, weights: &[f32]) -> (Option<usize>, f32) {
    let sum: f64 = weights.iter().copied().map(f64::from).sum();
    let sum_value = sum as f32;
    if sum == 0.0 {
        return (None, sum_value);
    }
    let roll = (f64::from(cx.world.rng().rand())
        * f64::from(1.0f32 / 32768.0)
        * f64::from(sum_value)) as f32;
    let mut running = 0.0f64;
    for (index, &w) in weights.iter().enumerate() {
        running += f64::from(w);
        if f64::from(roll) <= running {
            return (Some(index), roll);
        }
    }
    (None, roll)
}

/// `PickRandom` (`:675-683`): `None` for none, index 0 of one without a
/// draw, else `rand() * n / 0x8000`.
fn pick_random(cx: &mut Cx<'_>, count: usize) -> Option<usize> {
    if count == 0 {
        return None;
    }
    if count == 1 {
        return Some(0);
    }
    let n = count as i32;
    Some((cx.world.rng().rand().wrapping_mul(n) / 0x8000) as usize)
}

/// The vibe's feeling towards a group, 0 when `GetReputation` fails.
fn feeling(cx: &mut Cx<'_>, vibe: i32, group: u32) -> f32 {
    api::reputation::reputation(cx, vibe, group).unwrap_or(0.0)
}

/// `FBaseNpcBribe::IsOffered(Player)` (`BaseNpcRecords.cpp:56-62`): the
/// player's feeling towards the bribe's group is below 0.6.
pub fn bribe_offered(cx: &mut Cx<'_>, bribe: &BaseNpcBribe, vibe: i32) -> bool {
    feeling(cx, vibe, bribe.group) < 0.6
}

/// `FBaseNpcKnow::IsOffered(Player, Feeling)` (`:66-71`): the feeling
/// reaches the level's threshold and the player has no rumor record of it.
#[must_use]
pub fn know_offered(know: &BaseNpcKnow, state: &PlayerState, feeling: f32) -> bool {
    let threshold = REP_THRESHOLDS[know.rep_level as usize & 3];
    feeling >= threshold && state.find_rumor(know.field_0x04 as u32) == 0
}

/// `FBaseNpcRumor::GetWeight(Player, Feeling)` (`:109-121`): 0 unless the
/// player's stage lies in the rumor's range, the feeling reaches the
/// level's threshold and the player has not heard it; then 0x40 for a
/// rumor bounded on both ends, else `(level + 1) * 5`.
///
/// A level outside the four thresholds, which the C++ reads past its table,
/// weighs 0 (D1001).
#[must_use]
pub fn rumor_weight(rumor: &BaseNpcRumor, state: &PlayerState, feeling: f32) -> i32 {
    let stage = state.mission_number as i32;
    if stage < rumor.from_stage || stage > rumor.to_stage {
        return 0;
    }
    let Some(&threshold) = usize::try_from(rumor.rep_level)
        .ok()
        .and_then(|l| REP_THRESHOLDS.get(l))
    else {
        return 0;
    };
    // `!(feeling >= threshold)`: a NaN feeling fails.
    if feeling.partial_cmp(&threshold).is_none_or(|o| o.is_lt()) {
        return 0;
    }
    if state.find_rumor(rumor.ids[0] as u32) >= 1 {
        return 0;
    }
    if rumor.from_stage > 0 && rumor.to_stage < 0x2a {
        return 0x40;
    }
    (rumor.rep_level + 1) * 5
}

/// `FBaseNpc::ChooseOffer(Out, Player)` (`BaseDB.cpp:1521-1700`): what the
/// GF_NPC offers the player, picked by weight among a mission (30, when it
/// has `misn` lines), a bribe (20, when one is offered), a piece of
/// knowledge (15, when one is offered) and a rumor (the best rumor weight).
/// - A mission: a random `misn`; with the player's ship (or, when
///   `GetShipID` fails, the bits of the pick left in its slot) not 0,
///   `CreateBarMissionOffer` for the player's base and the NPC's
///   affiliation; a failed offer's loader is released.
/// - A bribe: a random offered bribe; its discount is 0.6 minus the
///   player's feeling towards the group, its price `cost * discount * 10`
///   rounded as random-mission money, its hostile groups the group's.
/// - Knowledge: a random offered piece.
/// - A rumor: one of the weighted rumors by weight.
///
/// A player without a state, which the C++ dereferences, gets no offer
/// (D1001).
pub fn choose_offer(cx: &mut Cx<'_>, npc: &BaseNpc, player: u32) -> BarOffer {
    let Some(state) = cx.logic.content.state.players.get(&player).cloned() else {
        return BarOffer::None;
    };
    let mut kinds: Vec<(i32, f32)> = Vec::with_capacity(5);
    // Both game mode flags are 1 and never written.
    let single = api::misc::single_player(cx);
    let enabled = (!single && super::bases::MISSION_TYPES_IN_MULTIPLAYER)
        || (single && super::bases::MISSION_TYPES_IN_SINGLE_PLAYER);
    if enabled && !npc.missions.is_empty() {
        kinds.push((2, MISSION_WEIGHT));
    }
    let npc_feeling = feeling(cx, state.reputation, npc.desc.affiliation);
    let mut bribe = false;
    for b in &npc.bribes {
        if bribe_offered(cx, b, state.reputation) {
            bribe = true;
            break;
        }
    }
    if bribe {
        kinds.push((1, BRIBE_WEIGHT));
    }
    if npc
        .knows
        .iter()
        .any(|k| know_offered(k, &state, npc_feeling))
    {
        kinds.push((4, KNOW_WEIGHT));
    }
    let best = npc
        .rumors
        .iter()
        .map(|r| rumor_weight(r, &state, npc_feeling) as u32)
        .max()
        .unwrap_or(0);
    if best != 0 {
        kinds.push((3, best as i32 as f32));
    }
    if kinds.is_empty() {
        return BarOffer::None;
    }
    let weights: Vec<f32> = kinds.iter().map(|k| k.1).collect();
    let (picked, slot) = pick_weighted(cx, &weights);
    let Some(picked) = picked else {
        return BarOffer::None;
    };
    match kinds[picked].0 {
        2 => {
            let Some(index) = pick_random(cx, npc.missions.len()) else {
                return BarOffer::None;
            };
            let ship = api::player::ship_id(cx, player).unwrap_or(slot.to_bits());
            if ship == 0 {
                return BarOffer::None;
            }
            let base = state.base.unwrap_or(0);
            let (made, mut data) = vendor::create_bar_mission_offer(
                cx,
                &npc.missions[index],
                player,
                base,
                npc.desc.affiliation,
            );
            if made {
                BarOffer::Mission(data)
            } else {
                data.release(cx);
                BarOffer::None
            }
        }
        1 => {
            let mut candidates = npc.bribes.clone();
            let pick = loop {
                let Some(at) = pick_random(cx, candidates.len()) else {
                    return BarOffer::None;
                };
                if bribe_offered(cx, &candidates[at], state.reputation) {
                    break candidates[at];
                }
                candidates.remove(at);
            };
            let shortfall = feeling(cx, state.reputation, pick.group);
            let discount = (f64::from(0.6f32) - f64::from(shortfall)) as f32;
            let cost = f64::from(pick.cost as u32);
            let price = trunc_to_i32(cost * f64::from(discount) * 10.0);
            let mut ids = [0u32; 3];
            faction_hostile_groups(&cx.logic.content.tables.empathy, pick.group, &mut ids);
            BarOffer::Bribe(BribeOffer {
                price: round_money(price as u32) as i32,
                group: pick.group,
                ids,
                discount,
                text_ids: pick.text_ids,
            })
        }
        4 => {
            let mut candidates = npc.knows.clone();
            loop {
                let Some(at) = pick_random(cx, candidates.len()) else {
                    return BarOffer::None;
                };
                if know_offered(&candidates[at], &state, npc_feeling) {
                    break BarOffer::Know(candidates[at]);
                }
                candidates.remove(at);
            }
        }
        _ => {
            let rumors: Vec<(BaseNpcRumor, f32)> = npc
                .rumors
                .iter()
                .filter_map(|r| {
                    let w = rumor_weight(r, &state, npc_feeling);
                    (w != 0).then(|| (*r, f64::from(w as u32) as f32))
                })
                .collect();
            let weights: Vec<f32> = rumors.iter().map(|r| r.1).collect();
            let (picked, _) = pick_weighted(cx, &weights);
            BarOffer::Rumor(picked.map(|i| rumors[i].0))
        }
    }
}

/// `GBarSceneNextId` (1): the next bar scene id.
pub const FIRST_SCENE_ID: i32 = 1;

/// `FContentBase::CreateBarScene(Script, PlayerId, ControllerId)`
/// (`:1121-1253`), for TU21's `StartRtcScript` (P3.5c4): a new scene id,
/// registered with the base; the script's room gets a lock per reserved
/// spot and, from the last role to the first, one character per role (the
/// player's character of a named role reused with one more use, otherwise
/// a new one from the role's NPC at the front of the room's list). The
/// first role's character gets the behaviour built from the script (its
/// offer dropped), the others without an offer lose theirs. The scene is
/// appended to the base's scenes. Returns the scene id; nothing but the id
/// for a base without a record.
///
/// A role without an NPC record, which the C++ dereferences, makes a
/// character of the default description (D1001).
pub fn create_bar_scene(
    cx: &mut Cx<'_>,
    base: u32,
    script: &CharacterEncounter,
    player: u32,
    controller: u32,
) -> i32 {
    let scene_id = cx.logic.content.state.next_bar_scene;
    cx.logic.content.state.next_bar_scene = scene_id.wrapping_add(1);
    if !cx.logic.content.state.bases.contains_key(&base) {
        return scene_id;
    }
    cx.logic
        .content
        .state
        .bar_scene_bases
        .entry(scene_id)
        .or_insert(base);
    let mut scene = BarScene {
        scene_id,
        player,
        room: script.location,
        ..BarScene::default()
    };
    let mut behavior = generic_scripts::BehaviorDescription {
        field_05: u8::from(script.autoplay),
        field_00: script.location as i32,
        field_258: script.sequence_id,
        field_06: 0,
        field_10: 0,
        field_04: 1,
        ..generic_scripts::BehaviorDescription::default()
    };
    for (kind, value) in [(1, 0), (0, 0), (2, 0), (3, 0), (4, 1), (5, 2), (6, 3)] {
        behavior.append_text(script.find_action_text(kind), value);
    }
    behavior.field_0c = script.start_room_location;
    behavior.field_08 = script.relocate_location;
    behavior.name = game_sim::protocol::fmtstr::FmtStr::new(script.mission_text_id as u32);
    behavior.field_21c = if script.find_action_text(3).is_some() {
        3
    } else {
        5
    };
    behavior.filter = player;
    behavior.field_220 = controller;
    behavior.field_224 = 0;
    if let Some(record) = cx.logic.content.state.bases.get_mut(&base) {
        let state = record.room_states.entry(script.location).or_default();
        for &spot in &script.reserved_spots {
            state.add_lock(player, spot);
            scene.placements.push(spot);
        }
    }
    let count = script.roles.len();
    behavior.ids = vec![(0, None); count];
    for index in (0..count).rev() {
        let role = &script.roles[index];
        let found = if role.role.is_empty() {
            None
        } else {
            base_ref(cx, base)
                .and_then(|b| b.room_states.get(&script.location))
                .and_then(|s| s.find_character(&role.role, player))
                .map(|c| (c.key, c.role.clone(), c.character_id))
        };
        let key = match found {
            Some((key, name, id)) => {
                if let Some(c) = character_mut(cx, base, key) {
                    c.use_count += 1;
                }
                behavior.ids[index] = (id, Some(name));
                key
            }
            None => {
                let npc = role.character.clone().unwrap_or_default();
                let desc = BarCharacterDesc {
                    placement: role.spot,
                    role: tstring(&role.role, 32),
                    fidget: tstring(&role.fidget, 64),
                    ..BarCharacterDesc::from_npc(&npc, player, script.location)
                };
                let key = next_key(cx, base);
                let made = create(cx, &desc, key);
                behavior.ids[index] = (made.character_id, Some(tstring(&role.role, 32)));
                let _ = insert_front(cx, base, made);
                key
            }
        };
        if index == 0 {
            let handle = behavior.create(cx);
            set_offer(cx, base, key, BarOffer::None);
            set_behavior(cx, base, key, handle);
        } else if character(cx, base, key).is_some_and(|c| c.offer.kind() == 0) {
            set_behavior(cx, base, key, u32::MAX);
        }
        scene.characters.insert(0, key);
    }
    if let Some(record) = cx.logic.content.state.bases.get_mut(&base) {
        record.bar_scenes.push(scene);
    }
    scene_id
}

/// `FBarScene::Release(KeepRoles)` (`:1059-1118`): the first character
/// loses the scene's behaviour unless its role is one of `keep_roles`
/// (`strcmp`); every character drops a use ([`remove_bar_character`]) and
/// every placement one count of the player's lock, erased at none.
///
/// A scene without characters and with roles to keep, whose first
/// character the C++ reads all the same, keeps nothing; a room without a
/// state, whose null the lock loop dereferences, has no locks (D1001).
fn release_scene(cx: &mut Cx<'_>, base: u32, scene: &BarScene, keep_roles: Option<&[Vec<u8>]>) {
    let first = scene.characters.first().copied();
    let mut keep_first = false;
    if let (Some(keep), Some(first)) = (keep_roles, first)
        && !keep.is_empty()
        && let Some(c) = character(cx, base, first)
    {
        keep_first = keep.iter().any(|r| c.role.as_slice() == c_str(r));
    }
    if let Some(first) = first
        && !keep_first
        && let Some(c) = character(cx, base, first)
    {
        let (mut old, id) = (c.behavior, c.character_id);
        if old != u32::MAX {
            api::gf::character_behavior_destroy(cx, &mut old);
        }
        api::gf::character_set_behavior(cx, id, u32::MAX);
        if let Some(c) = character_mut(cx, base, first) {
            c.behavior = u32::MAX;
        }
    }
    for &key in &scene.characters {
        remove_bar_character(cx, base, key, keep_roles);
    }
    let Some(state) = cx
        .logic
        .content
        .state
        .bases
        .get_mut(&base)
        .and_then(|b| b.room_states.get_mut(&scene.room))
    else {
        return;
    };
    for &placement in &scene.placements {
        if let Some(at) = state
            .locks
            .iter()
            .position(|l| l.player == scene.player && l.placement == placement)
        {
            state.locks[at].count -= 1;
            if state.locks[at].count == 0 {
                state.locks.remove(at);
            }
        }
    }
}

/// `FContentBase::RemoveBarScene(SceneId, KeepRoles)` (`:1256-1271`): the
/// first scene of the id released and erased, then the id forgotten
/// whether or not a scene was found.
pub fn remove_bar_scene(cx: &mut Cx<'_>, base: u32, scene_id: i32, keep_roles: Option<&[Vec<u8>]>) {
    let scene = cx.logic.content.state.bases.get_mut(&base).and_then(|b| {
        let at = b.bar_scenes.iter().position(|s| s.scene_id == scene_id)?;
        Some(b.bar_scenes.remove(at))
    });
    if let Some(scene) = scene {
        release_scene(cx, base, &scene, keep_roles);
    }
    cx.logic.content.state.bar_scene_bases.remove(&scene_id);
}

/// `CompareRmOfferDifficulty(Player, Difficulty)` (TU47
/// `GenericScripts.cpp:239-275`): the difficulty of the player's stage in
/// the rank table (the exact record, else the last one below it, else the
/// first; an empty table gives 0) against `difficulty`: -1 below
/// `stage / tolerance`, 1 above `stage * tolerance`, else 0, in double.
#[must_use]
pub fn compare_rm_offer_difficulty(cx: &Cx<'_>, stage: u32, difficulty: f32) -> i32 {
    let tolerance = f64::from(((f64::from(1.3689f32) - 1.0) * 0.5 + 1.0) as f32);
    let stage = stage as i32;
    let table = &cx.logic.content.tables.rank_difficulties;
    let mut previous = table.first().map_or(0.0, |r| r.difficulty);
    let mut found = None;
    for r in table {
        if r.stage == stage {
            found = Some(r.difficulty);
            break;
        }
        if r.stage > stage {
            break;
        }
        previous = r.difficulty;
    }
    let stage_difficulty = f64::from(found.unwrap_or(previous));
    if stage_difficulty / tolerance > f64::from(difficulty) {
        return -1;
    }
    if f64::from(difficulty) > stage_difficulty * tolerance {
        return 1;
    }
    0
}

/// The mission data a mission offer holds (`Offer->Data.Mission`).
pub(crate) fn offer_mission(cx: &Cx<'_>, offer: &BarOffer) -> Option<mission_data::Snapshot> {
    let BarOffer::Mission(data) = offer else {
        return None;
    };
    let key = data.mission?;
    let m = cx.logic.content.state.rm.missions.get(key)?;
    Some(mission_data::Snapshot {
        difficulty: m.difficulty,
        description: m.description.clone(),
    })
}

/// The two fields of a random mission the bar scripts read.
pub(crate) mod mission_data {
    use game_sim::protocol::fmtstr::FmtStr;

    /// `FRandomMissionData::Difficulty` and `Description`.
    #[derive(Clone, Debug)]
    pub struct Snapshot {
        /// `Difficulty` (`+0x4c`).
        pub difficulty: f32,
        /// `Description` (`+0x24`).
        pub description: FmtStr,
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use game_sim::data::config::GameData;
    use game_sim::object::World;
    use game_sim::object::world::WorldConfig;

    use super::*;
    use crate::content::tables::ranks::RankDifficulty;
    use crate::server::{Logic, RecordingSink};

    fn with_cx(run: impl FnOnce(&mut Cx<'_>)) {
        let mut world = World::new(Arc::new(GameData::new()), WorldConfig::default());
        let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, true);
        let mut cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        run(&mut cx);
    }

    fn character(key: u32, role: &[u8], player: u32, id: u32) -> BarCharacter {
        BarCharacter {
            key,
            use_count: 1,
            behavior: u32::MAX,
            character_id: id,
            room: 9,
            affiliation: u32::MAX,
            placement: 0,
            role: role.to_vec(),
            player,
            gender: 1,
            voice: 0,
            nickname: 0,
            talk_count: 0,
            offer: BarOffer::None,
        }
    }

    #[test]
    fn the_room_state_counts_locks_and_finds_roles_without_case() {
        let mut state = RoomState::default();
        state.add_lock(7, 3);
        state.add_lock(7, 3);
        state.add_lock(8, 3);
        assert_eq!(state.locks.len(), 2);
        assert_eq!(state.locks[0].count, 2);
        assert!(state.has_lock(7, 3) && !state.has_lock(7, 4));
        state.characters.push(character(1, b"Bartender", 7, 11));
        state.characters.push(character(2, b"trader", 8, 12));
        assert_eq!(
            state.find_character(b"bartender", 7).map(|c| c.key),
            Some(1)
        );
        assert!(state.find_character(b"bartender", 8).is_none());
        assert_eq!(state.count_characters(7), 1);
        assert!(state.has_character(8, 0));
    }

    #[test]
    fn a_rumor_weighs_by_its_bounds_and_level() {
        with_cx(|cx| {
            let mut state = PlayerState::new(cx, 7);
            state.mission_number = 5;
            let mut rumor = BaseNpcRumor {
                from_stage: 0,
                to_stage: 0x2a,
                rep_level: 1,
                ids: [100, 0, 0, 0, 0, 0, 0, 0],
            };
            // An open range: (level + 1) * 5, once the feeling reaches 0.2.
            assert_eq!(rumor_weight(&rumor, &state, 0.2), 10);
            assert_eq!(rumor_weight(&rumor, &state, 0.1), 0);
            assert_eq!(rumor_weight(&rumor, &state, f32::NAN), 0);
            // Bounded on both ends: 0x40.
            rumor.from_stage = 1;
            rumor.to_stage = 9;
            assert_eq!(rumor_weight(&rumor, &state, 0.5), 0x40);
            // Heard already, or out of the stage range: 0.
            state.add_rumor(100);
            assert_eq!(rumor_weight(&rumor, &state, 0.5), 0);
            rumor.ids[0] = 101;
            state.mission_number = 10;
            assert_eq!(rumor_weight(&rumor, &state, 0.5), 0);
            // A level past the thresholds weighs nothing (D1001).
            state.mission_number = 5;
            rumor.rep_level = 4;
            assert_eq!(rumor_weight(&rumor, &state, 1.0), 0);
        });
    }

    #[test]
    fn a_weighted_pick_takes_the_first_total_the_roll_does_not_pass() {
        with_cx(|cx| {
            cx.world.rng().seed(1);
            // rand() = 41 from seed 1: 41 / 32768 * 65 = 0.0813...
            let (picked, slot) = pick_weighted(cx, &[30.0, 20.0, 15.0]);
            assert_eq!(picked, Some(0));
            assert!(slot > 0.08 && slot < 0.09, "{slot}");
            assert_eq!(pick_weighted(cx, &[0.0, 0.0]), (None, 0.0));
            assert_eq!(pick_random(cx, 0), None);
            let before = cx.world.rng().clone();
            assert_eq!(pick_random(cx, 1), Some(0));
            assert_eq!(*cx.world.rng(), before, "one choice draws nothing");
        });
    }

    #[test]
    fn a_scenes_release_keeps_the_roles_and_unlocks_the_placements() {
        with_cx(|cx| {
            let mut record = ContentBase::new(5);
            let mut state = RoomState::default();
            let mut actor = character(1, b"actor", 7, 0);
            actor.use_count = 2;
            state.characters.push(actor);
            state.add_lock(7, 4);
            state.add_lock(7, 4);
            record.room_states.insert(9, state);
            record.bar_scenes.push(BarScene {
                scene_id: 3,
                player: 7,
                room: 9,
                placements: vec![4],
                characters: vec![1],
            });
            cx.logic.content.state.bases.insert(5, record);
            cx.logic.content.state.bar_scene_bases.insert(3, 5);
            remove_bar_scene(cx, 5, 3, Some(&[b"actor".to_vec()]));
            let record = &cx.logic.content.state.bases[&5];
            assert!(record.bar_scenes.is_empty());
            let state = &record.room_states[&9];
            assert_eq!(state.characters[0].use_count, 1);
            assert_eq!(state.locks[0].count, 1);
            assert!(cx.logic.content.state.bar_scene_bases.is_empty());
        });
    }

    #[test]
    fn the_offer_difficulty_is_judged_against_the_stage_window() {
        with_cx(|cx| {
            let mut tables = (*cx.logic.content.tables).clone();
            tables.rank_difficulties = vec![
                RankDifficulty {
                    stage: 0,
                    difficulty: 1.0,
                },
                RankDifficulty {
                    stage: 10,
                    difficulty: 10.0,
                },
            ];
            cx.logic.content.tables = Arc::new(tables);
            // Stage 5 reads stage 0's difficulty; the tolerance is 1.18445.
            assert_eq!(compare_rm_offer_difficulty(cx, 5, 0.5), -1);
            assert_eq!(compare_rm_offer_difficulty(cx, 5, 1.0), 0);
            assert_eq!(compare_rm_offer_difficulty(cx, 5, 1.2), 1);
            assert_eq!(compare_rm_offer_difficulty(cx, 10, 10.0), 0);
        });
    }
}
