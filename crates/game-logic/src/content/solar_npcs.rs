//! The solar NPCs of TU24 (`Content/NpcCreate.cpp`): the named `[Object]`s
//! of a system file that carry a behaviour, a base, a docking target, a
//! `reputation` or a `msg_id_prefix` (work package P3.4d, decision record
//! 0008 points 5 and 10).
//!
//! A system record's full load reads them on `SystemLoaded`
//! (`FSystemInfo::LoadSolarNpcs`, [`super::systems`]). A solar NPC with a
//! `reputation` makes itself a reputation handle: its nickname's vibe
//! (`pub::Reputation::GetReputation`) takes that faction as its affiliation
//! (`pub::Reputation::SetAffiliation`, `NpcCreate.cpp:70-104`). This is
//! where the stations, platforms and rings get their factions (fact F17).
//!
//! The NPC's personality state (`SubmitPersonalityState`, its content
//! callback) and its combat-state notice are P3.6c's, with the NPC ships;
//! the record keeps what they read.

use game_sim::ai::personality::Personality;
use game_sim::ai::state_graph::GraphType;
use neuron_core::hash::c_str;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::tables::reputation_group;
use crate::api;
use crate::server::Cx;

/// `TString<0x30>`: at most 0x2f bytes, up to the first NUL.
fn tstring(text: &[u8], size: usize) -> Vec<u8> {
    let mut out = c_str(text).to_vec();
    out.truncate(size - 1);
    out
}

/// The parameter block of one `[Object]` (`FSolarNpcDesc`, 0x470 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct SolarNpcDesc {
    /// `msg_id_prefix` (`CommReferrable`, a `TString<64>`).
    pub msg_id_prefix: Vec<u8>,
    /// `base`: the first base's record, by its id (`FindContentBaseInfo`;
    /// `None` where the C++ gets null).
    pub base: Option<u32>,
    /// `nickname` (a `TString<0x30>`).
    pub nickname: Vec<u8>,
    /// `behavior`: `pub::StateGraph::get_state_graph(value, LEADER)`; -1.
    pub state_graph: i32,
    /// `pilot`: the pilot's personality (`GetPilotPersonality`).
    pub personality: Personality,
    /// Set when a `pilot` was found.
    pub has_personality: bool,
    /// `reputation`: `pub::Reputation::GetReputationGroup`; -1.
    pub reputation_group: u32,
}

impl Default for SolarNpcDesc {
    fn default() -> Self {
        Self {
            msg_id_prefix: Vec::new(),
            base: None,
            nickname: Vec::new(),
            state_graph: -1,
            personality: Personality::default(),
            has_personality: false,
            reputation_group: u32::MAX,
        }
    }
}

/// `ParseSolarNpcKey(Reader, Desc)` (`NpcCreate.cpp:16-58`): one key of an
/// `[Object]`; false only when no key matched.
pub fn parse_solar_npc_key(
    cx: &mut Cx<'_>,
    reader: &mut IniReader,
    desc: &mut SolarNpcDesc,
) -> bool {
    if reader.is_value(b"nickname") {
        desc.nickname = tstring(&reader.value_string(), 0x30);
        return true;
    }
    if reader.is_value(b"base") {
        if desc.base.is_some() {
            return true;
        }
        let base = api::misc::base_id(cx, c_str(&reader.value_string())).unwrap_or(0);
        desc.base = cx
            .logic
            .content
            .state
            .bases
            .contains_key(&base)
            .then_some(base);
        return true;
    }
    if reader.is_value(b"reputation") {
        desc.reputation_group = reputation_group(cx, &reader.value_string());
        return true;
    }
    // `CommReferrable::read_comm_ini_data`.
    if reader.is_value(b"msg_id_prefix") {
        desc.msg_id_prefix = tstring(&reader.value_string(), 64);
        return true;
    }
    if reader.is_value(b"pilot") {
        let pilot = create_id(c_str(&reader.value_string()));
        if cx
            .logic
            .content
            .tables
            .pilots
            .personality(pilot, &mut desc.personality)
        {
            desc.has_personality = true;
        }
        return true;
    }
    if reader.is_value(b"behavior") {
        desc.state_graph = cx
            .world
            .data()
            .state_graphs
            .find(c_str(&reader.value_string()), GraphType::Leader);
        return true;
    }
    false
}

/// A solar NPC (`FSolarNpc`, 0x548 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct SolarNpc {
    /// `SpaceObjId`: `pub::GetNicknameId(nickname)`.
    pub object: u32,
    /// The nickname.
    pub nickname: Vec<u8>,
    /// The base record, by its id.
    pub base: Option<u32>,
    /// `MsgIdPrefix`.
    pub msg_id_prefix: Vec<u8>,
    /// `StateGraph` (`+0x168`).
    pub state_graph: i32,
    /// `bHasPersonality` (`+0x544`).
    pub has_personality: bool,
    /// `Personality` (`+0x16c`): the pilot's when one was found, else the
    /// default.
    pub personality: Personality,
    /// The faction of the `reputation` key (`FSolarNpcDesc +0x46c`).
    pub reputation_group: u32,
    /// The reputation handle's vibe (`FSolarNpcReputation +0x08`), `None`
    /// when the NPC has no handle (no `reputation`, or an id whose low 30
    /// bits are 0).
    pub reputation: Option<i32>,
    /// `+0x164`: set by the combat-state notice (`Type == 0`).
    pub field_0x164: bool,
    /// The content callback's handle (`+0x160`, record 0008 point 4),
    /// given when the NPC submits its state; 0 until then.
    pub callback: u32,
    /// `PendingComms` (TU20 `+0x108`): the mission dialog lines the NPC
    /// says when the line before ends (P3.5c4,
    /// [`super::systems::send_pending_comms`]).
    pub pending_comms: Vec<super::systems::PendingComm>,
}

impl SolarNpc {
    /// `FSolarNpc(Desc)` (`NpcCreate.cpp:70-104`): the object is the
    /// nickname's id; with a `reputation` and an id, the reputation handle
    /// is made: the nickname's vibe (`GetReputation`, -1 when no vibe has
    /// the id) takes the faction as its affiliation (`SetAffiliation`,
    /// whose result is dropped).
    pub fn new(cx: &mut Cx<'_>, desc: &SolarNpcDesc) -> Self {
        let object = api::misc::nickname_id(&desc.nickname);
        let reputation = if object & 0x3fff_ffff != 0 && desc.reputation_group != u32::MAX {
            let vibe = api::reputation::solar_reputation_by_name(cx, &desc.nickname).unwrap_or(-1);
            let _ = api::reputation::set_affiliation(cx, vibe, desc.reputation_group);
            Some(vibe)
        } else {
            None
        };
        Self {
            object,
            nickname: desc.nickname.clone(),
            base: desc.base,
            msg_id_prefix: desc.msg_id_prefix.clone(),
            state_graph: desc.state_graph,
            has_personality: desc.has_personality,
            personality: if desc.has_personality {
                desc.personality
            } else {
                Personality::default()
            },
            reputation_group: desc.reputation_group,
            reputation,
            field_0x164: false,
            callback: 0,
            pending_comms: Vec::new(),
        }
    }
}

/// The `[Object]` part of `FSystemInfo::LoadSolarNpcs` (`SystemInfo.cpp:313-345`)
/// for one section: its keys, then whether it is docked (a base was given,
/// or the named object has a docking target), and the NPC when the named
/// object has a behaviour, a base, a docking target, a reputation or a
/// `msg_id_prefix`.
pub fn read_solar_npc(cx: &mut Cx<'_>, reader: &mut IniReader) -> Option<SolarNpc> {
    let mut desc = SolarNpcDesc::default();
    while reader.read_value() {
        parse_solar_npc_key(cx, reader, &mut desc);
    }
    let docked = if !desc.nickname.is_empty() && desc.base.is_none() {
        let object = api::misc::nickname_id(&desc.nickname);
        api::space_obj::docking_target(cx, object).unwrap_or(0) != 0
    } else {
        desc.base.is_some()
    };
    if !desc.nickname.is_empty()
        && (desc.state_graph != -1
            || desc.base.is_some()
            || docked
            || desc.reputation_group != u32::MAX
            || !desc.msg_id_prefix.is_empty())
    {
        Some(SolarNpc::new(cx, &desc))
    } else {
        None
    }
}

/// `FSolarNpc::SubmitPersonalityState(bSubmit)` (`NpcCreate.cpp:139-153`) of
/// the `at`-th solar NPC of `system`'s record: only with a reputation
/// handle, `bSubmit` and a state graph, `SetPersonalityParams` with the
/// graph, the NPC as the content callback (its handle, given on the first
/// submit), no directive callback and the pilot's personality when it has
/// one; the result is dropped.
pub fn submit_personality_state(cx: &mut Cx<'_>, system: u32, at: usize, submit: bool) {
    let Some(npc) = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .and_then(|r| r.solar_npcs.get(at))
    else {
        return;
    };
    if npc.reputation.is_none() || !submit || npc.state_graph == -1 {
        return;
    }
    let (object, state_graph, has_personality, personality, mut callback) = (
        npc.object,
        npc.state_graph,
        npc.has_personality,
        npc.personality,
        npc.callback,
    );
    if callback == 0 {
        callback = cx
            .logic
            .content
            .state
            .callbacks
            .allocate(super::callbacks::CallbackOwner::Solar { system, object });
        if let Some(npc) = cx
            .logic
            .content
            .state
            .systems
            .get_mut(&system)
            .and_then(|r| r.solar_npcs.get_mut(at))
        {
            npc.callback = callback;
        }
    }
    let op = crate::api::ai::SetPersonalityParams {
        state_graph,
        content_callback: crate::api::ai::CallbackArg::Set(callback),
        directive_callback: crate::api::ai::CallbackArg::Null,
        set_personality: has_personality,
        personality: if has_personality {
            personality
        } else {
            Personality::default()
        },
    };
    let op = crate::api::ai::BaseOp::new(crate::api::ai::Op::SetPersonalityParams(Box::new(op)));
    let _ = api::ai::submit_state(cx, object, &op);
}

/// A report of a solar's manager to the solar NPC: only
/// `combat_state_notification` has a body of its own (`NpcCreate.cpp:116-126`):
/// `+0x164 = (Type == 0)` and `CombatState` to every controller with the
/// record's system. The others keep `ContentCallback`'s empty bodies. The
/// last NPC with the object id answers, as `FindShipOrSolar` finds it.
pub fn content_event(
    cx: &mut Cx<'_>,
    system: u32,
    object: u32,
    event: game_sim::ai::content::CombatEvent,
) {
    let game_sim::ai::content::CombatEvent::State {
        kind,
        owner,
        target,
    } = event
    else {
        return;
    };
    let Some(npc) = cx
        .logic
        .content
        .state
        .systems
        .get_mut(&system)
        .and_then(|r| r.solar_npcs.iter_mut().rev().find(|n| n.object == object))
    else {
        return;
    };
    npc.field_0x164 = kind == 0;
    let _ = cx.dispatch(
        neuron_server::controller::ControllerId::BROADCAST,
        &mut crate::events::ControllerMessage::CombatState {
            state: kind as u32,
            param1: owner,
            param2: target,
            system,
        },
    );
}
