//! Player groups (work package P3.2): `CPlayerGroup`
//! (`Server/server_classes.cpp:4420-4945`) and `PlayerDB::SendGroupID`.
//!
//! The C++ keeps each group on the heap, found by id through
//! `s_GroupIDToGroupPtrMap`, with the ids from `s_uiGroupID`; the port
//! keeps them in [`Groups`] by the same ids, and a slot names its group by
//! id. Group notices travel as chat messages from the group-control id
//! (`0x00010004`): a type and a player, eight bytes.
//!
//! When a player's ship joins or leaves a group its system's players hear
//! of it (`StarSystem_SendGroupChange`, [`crate::replication`]).

use std::collections::{BTreeMap, BTreeSet};

use neuron_server::controller::ControllerId;

use crate::client;
use crate::events::ControllerMessage;
use crate::player;
use crate::server::Cx;

/// The chat id group notices come from (`GGroupChatSourceId`).
pub const GROUP_CONTROL_CHAT_ID: u32 = 0x0001_0004;
/// The chat id group chat comes from (`GDeliverChatSourceId`).
pub const GROUP_CHAT_ID: u32 = 0x0001_0003;
/// The most members a group has.
pub const MAX_MEMBERS: usize = 8;

/// The group notices (`BaseGroupMessage::Type`) the server sends.
pub mod notice {
    /// You invited the player.
    pub const INVITED: u32 = 0x3;
    /// The player invited you.
    pub const INVITED_YOU: u32 = 0x4;
    /// You joined the player's group.
    pub const JOINED: u32 = 0x5;
    /// The player joined your group.
    pub const MEMBER_JOINED: u32 = 0x6;
    /// You were taken out of the group.
    pub const REMOVED: u32 = 0x7;
    /// The player left your group.
    pub const MEMBER_LEFT: u32 = 0x8;
}

/// One group (`CPlayerGroup`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Group {
    /// The members, by session (`m_Members`, a `std::set`).
    pub members: BTreeSet<u32>,
    /// The invited players (`m_Invites`).
    pub invites: BTreeSet<u32>,
}

/// Every group, by id.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Groups {
    last_id: u32,
    by_id: BTreeMap<u32, Group>,
}

impl Groups {
    /// `new CPlayerGroup`: the next id.
    pub fn create(&mut self) -> u32 {
        self.last_id = self.last_id.wrapping_add(1);
        self.by_id.insert(self.last_id, Group::default());
        self.last_id
    }

    /// `delete group`: the id is given back when it was the last one made.
    pub fn destroy(&mut self, id: u32) {
        if id == self.last_id {
            self.last_id = self.last_id.wrapping_sub(1);
        }
        self.by_id.remove(&id);
    }

    /// `FromGroupID`.
    #[must_use]
    pub fn get(&self, id: u32) -> Option<&Group> {
        self.by_id.get(&id)
    }

    /// The group, to change.
    pub fn get_mut(&mut self, id: u32) -> Option<&mut Group> {
        self.by_id.get_mut(&id)
    }

    /// `GetMember(index)`: 0 outside the members.
    #[must_use]
    pub fn member(&self, id: u32, index: i32) -> u32 {
        let Some(group) = self.get(id) else {
            return 0;
        };
        usize::try_from(index)
            .ok()
            .and_then(|i| group.members.iter().nth(i).copied())
            .unwrap_or(0)
    }
}

/// A group notice: the eight-byte block (`FGroupMsgPayload`) from the
/// group-control id to `recipient`'s chat id.
pub fn send_notice(cx: &mut Cx<'_>, recipient: u32, kind: u32, param: u32) {
    let mut data = Vec::with_capacity(8);
    data.extend_from_slice(&kind.to_le_bytes());
    data.extend_from_slice(&(param & 0xFFFF).to_le_bytes());
    cx.logic
        .chat
        .send(recipient & 0xFFFF, GROUP_CONTROL_CHAT_ID, data);
}

fn members(cx: &Cx<'_>, id: u32) -> Vec<u32> {
    cx.logic
        .groups
        .get(id)
        .map(|g| g.members.iter().copied().collect())
        .unwrap_or_default()
}

/// `PlayerDB::SendGroupID`: every valid player learns the player's group.
pub fn send_group_id(cx: &mut Cx<'_>, player: u32, group: u32) {
    let max = cx.logic.players.max_players() as u32;
    for other in 1..=max {
        if cx.logic.players.is_valid(other) {
            client::misc_obj_update_player_group(cx, other, player, group);
        }
    }
}

/// The player's ship takes the group id (`CShip::GroupId`), when the player
/// has a ship in the world; then the players of the slot's system hear of
/// it (`StarSystem_SendGroupChange`, [`crate::replication::group_changed`]).
fn set_ship_group(cx: &mut Cx<'_>, player: u32, group: u32) {
    let Some((ship, system)) = cx
        .logic
        .players
        .get(player)
        .map(|s| (s.ship_object_id, s.system_id))
    else {
        return;
    };
    let Some(h) = cx.world.find(ship) else {
        return;
    };
    let Some(data) = cx.world.get_mut(h).and_then(|o| o.as_ship_mut()) else {
        return;
    };
    data.group_id = group;
    if cx.logic.server_world.systems.contains_key(&system) {
        crate::replication::group_changed(cx, system, ship);
    }
}

/// `CPlayerGroup::AddMember`: each member and the joiner learn of each
/// other; the joiner takes the group's mission, or the group the joiner's
/// when the group has none; the joiner becomes a member, every player
/// learns its group, `GroupMemberJoined` is broadcast and its ship takes
/// the group id.
pub fn add_member(cx: &mut Cx<'_>, id: u32, player: u32) -> bool {
    let current = members(cx, id);
    for &member in &current {
        send_notice(cx, member, notice::MEMBER_JOINED, player);
    }
    for &member in &current {
        send_notice(cx, player, notice::MEMBER_JOINED, member);
    }
    let first = |f: fn(&player::PlayerData) -> u32| {
        current
            .iter()
            .filter_map(|&m| cx.logic.players.get(m).map(f))
            .find(|&v| v != 0)
            .unwrap_or(0)
    };
    let group_mission = first(|p| p.group_mission_id);
    let group_set_by = first(|p| p.group_mission_set_by);
    let (joiner_mission, joiner_set_by) = cx
        .logic
        .players
        .get(player)
        .map_or((0, 0), |p| (p.group_mission_id, p.group_mission_set_by));
    if joiner_mission == 0 || group_mission != 0 {
        if let Some(p) = cx.logic.players.get_mut(player) {
            p.group_mission_id = group_mission;
            p.group_mission_set_by = group_set_by;
        }
        client::misc_obj_update_mission(cx, player, group_mission, group_set_by);
    } else {
        for &member in &current {
            if let Some(p) = cx.logic.players.get_mut(member) {
                p.group_mission_id = joiner_mission;
                p.group_mission_set_by = joiner_set_by;
            }
            client::misc_obj_update_mission(cx, member, joiner_mission, joiner_set_by);
        }
    }
    if let Some(group) = cx.logic.groups.get_mut(id) {
        group.members.insert(player);
    }
    send_group_id(cx, player, id);
    if let Some(p) = cx.logic.players.get_mut(player) {
        p.group = Some(id);
    }
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::GroupMemberJoined { player },
    );
    set_ship_group(cx, player, id);
    true
}

/// `CPlayerGroup::DelMember`: `false` for a non-member. `GroupMemberLeft`
/// is broadcast before the player leaves; the members hear of it and the
/// player of its removal; the player's mission and objectives are cleared,
/// every player learns it has no group, and its ship leaves the group.
pub fn del_member(cx: &mut Cx<'_>, id: u32, player: u32) -> bool {
    if !cx
        .logic
        .groups
        .get(id)
        .is_some_and(|g| g.members.contains(&player))
    {
        return false;
    }
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::GroupMemberLeft { player },
    );
    if let Some(group) = cx.logic.groups.get_mut(id) {
        group.members.remove(&player);
    }
    for member in members(cx, id) {
        send_notice(cx, member, notice::MEMBER_LEFT, player);
    }
    send_notice(cx, player, notice::REMOVED, player);
    if let Some(p) = cx.logic.players.get_mut(player) {
        p.group_mission_id = 0;
        p.group_mission_set_by = 0;
    }
    client::misc_obj_update_mission(cx, player, 0, 0);
    client::clear_mission_objectives(cx, player);
    send_group_id(cx, player, 0);
    if let Some(p) = cx.logic.players.get_mut(player) {
        p.group = None;
    }
    set_ship_group(cx, player, 0);
    true
}

/// `CPlayerGroup::HandleClientLogout`: the player's invitation and
/// membership end; a group left empty is destroyed.
pub fn handle_client_logout(cx: &mut Cx<'_>, id: u32, player: u32) {
    if let Some(group) = cx.logic.groups.get_mut(id) {
        group.invites.remove(&player);
    }
    del_member(cx, id, player);
    if cx
        .logic
        .groups
        .get(id)
        .is_some_and(|g| g.members.is_empty())
    {
        cx.logic.groups.destroy(id);
    }
}

/// `CPlayerGroup::SendGroup`: a notice to every member.
pub fn send_group(cx: &mut Cx<'_>, id: u32, kind: u32, param: u32) {
    for member in members(cx, id) {
        send_notice(cx, member, kind, param);
    }
}

/// `CPlayerGroup::DeliverChat`: an encoded chat message to every member,
/// from the group chat id.
pub fn deliver_chat(cx: &mut Cx<'_>, id: u32, data: &[u8]) {
    for member in members(cx, id) {
        cx.logic
            .chat
            .send(member & 0xFFFF, GROUP_CHAT_ID, data.to_vec());
    }
}

/// `CPlayerGroup::RewardMembers`: each member's cash, counted as earned.
pub fn reward_members(cx: &mut Cx<'_>, id: u32, amount: i32) {
    for member in members(cx, id) {
        player::adjust_cash(cx, member, amount, true);
    }
}

/// `CPlayerGroup::GetMissionID`: the first member's non-zero mission.
#[must_use]
pub fn mission_id(cx: &Cx<'_>, id: u32) -> u32 {
    members(cx, id)
        .iter()
        .filter_map(|&m| cx.logic.players.get(m))
        .map(|p| p.group_mission_id)
        .find(|&v| v != 0)
        .unwrap_or(0)
}

/// `CPlayerGroup::GetMissionSetBy`.
#[must_use]
pub fn mission_set_by(cx: &Cx<'_>, id: u32) -> u32 {
    members(cx, id)
        .iter()
        .filter_map(|&m| cx.logic.players.get(m))
        .map(|p| p.group_mission_set_by)
        .find(|&v| v != 0)
        .unwrap_or(0)
}

/// `CPlayerGroup::SetMissionID`: every member takes the mission and hears
/// of it.
pub fn set_mission_id(cx: &mut Cx<'_>, id: u32, mission: u32, set_by: u32) {
    for member in members(cx, id) {
        if let Some(p) = cx.logic.players.get_mut(member) {
            p.group_mission_id = mission;
            p.group_mission_set_by = set_by;
        }
        client::misc_obj_update_mission(cx, member, mission, set_by);
    }
}
