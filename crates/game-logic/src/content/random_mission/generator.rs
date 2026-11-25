//! The offer generator (TU63, `Content/RandomMissionGen.cpp:1097-3402`):
//! `FRandomMissionGenerator::BuildOffer` with the offer node, the tree
//! paths and their pick, the path filters and their `CreateOffer`, the ship
//! and solar builders, the texts, objectives and comm sequences (work
//! package P3.7a, record 0008 point 8).
//!
//! Every draw is from the world's CRT generator, the one Content's `srand`
//! reseeds ([`rebuild_offer`]). Three counters outside the seed take part:
//! the vignette ship and solar names and the ship group serial, in
//! [`super::RmState`]; and the faction name reservations an assassination
//! target takes ([`crate::content::npc_names`]).

// The C++'s negated float tests (`!(x > 0.0f)`) send NaN to the failing
// side; the port spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use game_sim::object::csys::random_inside_ring;
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::num::trunc_to_i32;

use super::offer::{
    self, Conversation, ConversationLine, LootSpawn, Objective, Offer, ShipComponent, ShipGroup,
    ShipSpawn, SolarComponent, SolarSpawn,
};
use super::tables::{KillableSolar, add_unique};
use super::vignette::{
    DifficultyWindow, OfferText, ParamText, TreePath, VignetteNode, VignetteTree,
};
use super::{RmData, register_dest_system};
use crate::api;
use crate::content::cargo;
use crate::content::encounter_params::EncounterShip;
use crate::content::npc_names;
use crate::server::Cx;

/// The offer request (`FRmOfferQuery`, 0x34 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct OfferQuery {
    /// `+0x00` (-1).
    pub offer_group: i32,
    /// `+0x04` (-1).
    pub enemy_group: u32,
    /// `+0x08`.
    pub reward: i32,
    /// `+0x0c`.
    pub rep_reward: f32,
    /// `+0x10`.
    pub rep_hit: f32,
    /// `+0x14`.
    pub offer_base: u32,
    /// `+0x18`.
    pub difficulty: f32,
    /// `+0x1c`.
    pub objective_zone: u32,
    /// `+0x20`.
    pub objective_pos: Vector,
    /// `+0x2c`.
    pub player: u32,
    /// `+0x30`.
    pub dest_sys: u32,
}

impl Default for OfferQuery {
    fn default() -> Self {
        Self {
            offer_group: -1,
            enemy_group: u32::MAX,
            reward: 0,
            rep_reward: 0.0,
            rep_hit: 0.0,
            offer_base: 0,
            difficulty: 0.0,
            objective_zone: 0,
            objective_pos: Vector::ZERO,
            player: 0,
            dest_sys: 0,
        }
    }
}

/// What `FinalizeMission` (`DestroyMission.cpp:620-650`) leaves for one
/// rebuild: its result (0 with an offer, 2 without), the tree path
/// `BuildOffer` wrote and the offer the mission keeps.
#[derive(Clone, Debug, PartialEq)]
pub struct Rebuilt {
    /// 0 or 2.
    pub result: i32,
    /// `FRandomMissionData::TreePath`.
    pub path: TreePath,
    /// The offer appended to the mission's list.
    pub offer: Option<Offer>,
}

/// The offer half of `FRandomMissionLoader::FinalizeMission`: the world's
/// generator seeded with `seed`, then `BuildOffer` over `query`. A built
/// offer is kept; one that was made but not built is released at once, as
/// the C++'s holder releases it.
pub fn rebuild_offer(cx: &mut Cx<'_>, query: &OfferQuery, seed: u32) -> Rebuilt {
    let mut path = TreePath::default();
    cx.world.rng().seed(seed);
    let (built, made) = build_offer(cx, query, &mut path);
    let offer = if built {
        made
    } else {
        if let Some(o) = &made {
            offer::release_offer(cx, o);
        }
        None
    };
    Rebuilt {
        result: if offer.is_some() { 0 } else { 2 },
        path,
        offer,
    }
}

/// `FRandomMissionGenerator::BuildOffer` (`RandomMissionGen.cpp:3048-3094`):
/// the destination registered, the query checked, then the offer node, the
/// tree path (written to `path`) and the offer built from it. The second
/// value is what `*OutOffer` holds at the end, which a failed build can set.
pub fn build_offer(
    cx: &mut Cx<'_>,
    query: &OfferQuery,
    path: &mut TreePath,
) -> (bool, Option<Offer>) {
    register_dest_system(cx, query.dest_sys);
    let q = query;
    if q.offer_group == -1
        || q.enemy_group == u32::MAX
        || q.reward <= 0
        || !(q.rep_reward > 0.0)
        || !(q.rep_hit < 0.0)
        || q.offer_base == 0
        || q.difficulty < 0.0
        || q.difficulty.is_nan()
        || q.difficulty > 100.0
        || q.objective_zone == 0
    {
        return (false, None);
    }
    let (x, y, z) = (
        f64::from(q.objective_pos.x),
        f64::from(q.objective_pos.y),
        f64::from(q.objective_pos.z),
    );
    if x * x + y * y + z * z == 0.0 || q.player == 0 || q.dest_sys == 0 {
        return (false, None);
    }
    let rm = cx.logic.content.rm.clone();
    let Some(node) = pick_offer_node(cx, &rm.tree, q) else {
        return (false, None);
    };
    if !choose_tree_path(cx, &rm.tree, path, node, q.difficulty, q.offer_group) {
        return (false, None);
    }
    build_offer_from_path(cx, &rm, path, q)
}

/// `PickOfferNode` (`:1508-1547`): a weighted pick among the leaves whose
/// offer and hostile groups hold the query's and one of whose windows holds
/// its difficulty. `None` for no candidate; a pick past the end, which the
/// C++ reads after the last candidate, is `None` in the reconstruction and
/// the port.
fn pick_offer_node(cx: &mut Cx<'_>, tree: &VignetteTree, q: &OfferQuery) -> Option<u32> {
    let mut ids = Vec::new();
    let mut weights = Vec::new();
    for leaf in &tree.leaves {
        let Some(node) = tree.nodes.get(leaf) else {
            continue;
        };
        if !node.params.offer_groups.contains(&(q.offer_group as u32))
            || !node.params.hostile_groups.contains(&q.enemy_group)
        {
            continue;
        }
        if node.windows.iter().any(|w| w.is_in_range(q.difficulty)) {
            ids.push(node.node_id);
            weights.push(node.params.weight);
        }
    }
    super::tables::weighted_pick(cx, &weights).map(|i| ids[i])
}

/// `CollectTreePaths` (`:1557-1598`): every path from `id` up to the root
/// through parents with a window overlapping `window` and an offer group
/// list that is empty or holds the group; the bits of the current branch
/// are `choices` (the C++'s function-static list).
fn collect_tree_paths(
    tree: &VignetteTree,
    out: &mut Vec<TreePath>,
    id: u32,
    window: DifficultyWindow,
    offer_group: i32,
    choices: &mut Vec<u8>,
) {
    let Some(node) = tree.nodes.get(&id) else {
        return;
    };
    if node.parents.is_empty() {
        out.push(TreePath {
            bits: choices.clone(),
        });
        return;
    }
    for parent_id in &node.parents {
        let Some(parent) = tree.nodes.get(parent_id) else {
            continue;
        };
        let bit = u8::from(parent.children.first() == Some(&id));
        if !parent.windows.iter().any(|w| w.overlaps(&window)) {
            continue;
        }
        let groups = &parent.params.offer_groups;
        if !groups.is_empty() && !groups.contains(&(offer_group as u32)) {
            continue;
        }
        choices.insert(0, bit);
        let shifted = DifficultyWindow {
            min: (f64::from(window.min) - f64::from(parent.params.difficulty.max)) as f32,
            max: (f64::from(window.max) - f64::from(parent.params.difficulty.min)) as f32,
        };
        collect_tree_paths(tree, out, parent.node_id, shifted, offer_group, choices);
        choices.remove(0);
    }
}

/// `PickTreePath` (`:1605-1663`): each path weighted by the product of its
/// decisions' chances from the root (each product stored as a float), then
/// a weighted pick. A zero sum or a pick past the end returns past the
/// vector in the C++; the reconstruction and the port pick nothing.
fn pick_tree_path(cx: &mut Cx<'_>, tree: &VignetteTree, paths: &[TreePath]) -> Option<usize> {
    let mut weights = Vec::with_capacity(paths.len());
    for path in paths {
        let mut weight = 1.0f32;
        let mut current = tree.root;
        for &bit in &path.bits {
            let Some(node) = tree.nodes.get(&current) else {
                break;
            };
            let chance = if node.first_child_chance == -1.0 {
                1.0 / node.children.len() as f64
            } else {
                f64::from(node.first_child_chance)
            };
            if bit == 1 {
                weight = (chance * f64::from(weight)) as f32;
                current = node.children.first().copied().unwrap_or(u32::MAX);
            } else {
                weight = ((1.0 - chance) * f64::from(weight)) as f32;
                current = node.children.get(1).copied().unwrap_or(u32::MAX);
            }
        }
        weights.push(weight);
    }
    let sum: f64 = weights.iter().fold(0.0, |s, &w| s + f64::from(w));
    let total = sum as f32;
    if sum == 0.0 {
        return None;
    }
    let target =
        (f64::from(cx.world.rng().rand()) * f64::from(3.051_757_8e-5f32) * f64::from(total)) as f32;
    let mut acc = 0.0;
    for (index, &w) in weights.iter().enumerate() {
        acc += f64::from(w);
        if f64::from(target) <= acc {
            return Some(index);
        }
    }
    None
}

/// `ChooseTreePath` (`:1669-1684`): the paths from the offer node through
/// windows overlapping `[d, d]`, and the weighted pick copied into `path`;
/// true as soon as one path exists. A pick of nothing dereferences null in
/// the reconstruction; the port keeps `path` (D873).
fn choose_tree_path(
    cx: &mut Cx<'_>,
    tree: &VignetteTree,
    path: &mut TreePath,
    node: u32,
    difficulty: f32,
    offer_group: i32,
) -> bool {
    let mut paths = Vec::new();
    let mut choices = Vec::new();
    let window = DifficultyWindow {
        min: difficulty,
        max: difficulty,
    };
    collect_tree_paths(tree, &mut paths, node, window, offer_group, &mut choices);
    if paths.is_empty() {
        return false;
    }
    if let Some(i) = pick_tree_path(cx, tree, &paths) {
        path.bits.clone_from(&paths[i].bits);
    }
    true
}

/// The five flags of the base path filter (`FRmPathFilter` `+0x04..+0x08`):
/// `random_mission`, `destroy_solars_mission`, `assassinate_mission`,
/// `destroy_ships_mission`, `loot_item_mission`.
type BaseFlags = [bool; 5];

const BASE_NAMES: [&[u8]; 5] = [
    b"random_mission",
    b"destroy_solars_mission",
    b"assassinate_mission",
    b"destroy_ships_mission",
    b"loot_item_mission",
];

/// The destroy-solars filter's flags (`+0x0c..+0x19`), in field order.
const DESTROY_SOLARS_NAMES: [&[u8]; 14] = [
    b"big_solar",
    b"pk_big_solar",
    b"defensive_solars",
    b"pk_defensive_solars",
    b"cavalry",
    b"hostile_ships",
    b"pk_hostile_ships",
    b"hostile_ships_at_installation",
    b"hostile_ships_after_30_s",
    b"hostile_waves",
    b"friendly_ships",
    b"friendly_ships_at_installation",
    b"friendly_ships_after_30_s",
    b"friendly_waves",
];

/// `GRmAssassinateNodeNames` (`:1395-1405`).
pub const ASSASSINATE_NAMES: [&[u8]; 27] = [
    b"assassinate_ship",
    b"assassinate_solar",
    b"target_drops_critical_loot",
    b"bring_back_loot",
    b"tractor_in_loot",
    b"destroy_loot",
    b"target_spawns_with_wave",
    b"pre_battle",
    b"target_spawns_at_pre_battle",
    b"pre_battle_non_target_wave",
    b"pre_battle_wave_runs",
    b"pk_pre_battle_wave",
    b"target_runs_to_main_battle",
    b"main_battle",
    b"defensive_solars_at_main_battle",
    b"target_spawns_at_main_battle",
    b"main_battle_non_target_wave",
    b"wave_just_after_main_battle_starts",
    b"continuous_reenforcements",
    b"pk_all_ships",
    b"pre_battle_wave_1",
    b"pre_battle_wave_2",
    b"main_battle_wave_1",
    b"main_battle_wave_2",
    b"friendly_ships_at_main_battle",
    b"friendly_ships_after_30_s",
    b"friendly_ships_come_in_to_whoop_up",
];

/// The path filters (`FRmPathFilter` and its five derived classes).
#[derive(Clone, Copy, Debug, PartialEq)]
enum Filter {
    /// The base filter (vtable `06fbcf6c`).
    Base(BaseFlags),
    /// The destroy-solars filter (`06fbd034`) and, with `cavalry`, the
    /// cavalry filter (`06fbd164`), whose slots 1 and 2 answer false.
    DestroySolars {
        base: BaseFlags,
        flags: [bool; 14],
        cavalry: bool,
    },
    /// The assassinate filter (`06fbd01c`).
    Assassinate { base: BaseFlags, flags: [bool; 27] },
    /// The destroy-ships (`06fbd004`) and loot-item (`06fbcfec`) filters,
    /// which never create an offer.
    Inert,
}

/// The node's nickname against `names` in order; `branch` matches with no
/// flag when `branch` is set.
fn apply_names(
    node: &VignetteNode,
    bit: u8,
    names: &[&[u8]],
    flags: &mut [bool],
    branch: bool,
) -> bool {
    if bit == 0 {
        return false;
    }
    for (i, name) in names.iter().enumerate() {
        if node.nickname == create_id(name) {
            flags[i] = true;
            return true;
        }
    }
    branch && node.nickname == create_id(b"branch")
}

impl Filter {
    /// Slot 2, `ApplyNode`.
    fn apply_node(&mut self, node: &VignetteNode, bit: u8) -> bool {
        match self {
            Self::Base(base) => apply_names(node, bit, &BASE_NAMES, base, false),
            Self::DestroySolars {
                flags,
                cavalry: false,
                ..
            } => apply_names(node, bit, &DESTROY_SOLARS_NAMES, flags, true),
            Self::Assassinate { flags, .. } => {
                apply_names(node, bit, &ASSASSINATE_NAMES, flags, true)
            }
            Self::DestroySolars { cavalry: true, .. } | Self::Inert => false,
        }
    }

    /// Slot 1, `HasMissionType`.
    fn has_mission_type(&self) -> bool {
        match self {
            Self::Base(b) => b[1] || b[2] || b[3] || b[4],
            Self::DestroySolars {
                flags,
                cavalry: false,
                ..
            } => flags[4],
            _ => false,
        }
    }

    /// Slot 3, `Specialize`.
    fn specialize(&self) -> Option<Self> {
        match *self {
            Self::Base(base) => {
                if base[1] {
                    Some(Self::DestroySolars {
                        base,
                        flags: [false; 14],
                        cavalry: false,
                    })
                } else if base[2] {
                    Some(Self::Assassinate {
                        base,
                        flags: [false; 27],
                    })
                } else if base[3] || base[4] {
                    Some(Self::Inert)
                } else {
                    None
                }
            }
            Self::DestroySolars {
                flags,
                cavalry: false,
                ..
            } if flags[4] => Some(Self::DestroySolars {
                base: [false; 5],
                flags: [false; 14],
                cavalry: true,
            }),
            _ => None,
        }
    }

    /// Slot 4, `CreateOffer`: the success and what `*OutOffer` holds.
    fn create_offer(
        &self,
        cx: &mut Cx<'_>,
        rm: &RmData,
        q: &OfferQuery,
        out: Option<Offer>,
    ) -> (bool, Option<Offer>) {
        match self {
            Self::DestroySolars { flags, .. } => {
                if let Some(old) = &out {
                    offer::release_offer(cx, old);
                }
                destroy_solars_offer(cx, rm, flags, q)
            }
            Self::Assassinate { flags, .. } => assassinate_offer(cx, rm, flags, q, out),
            Self::Base(_) | Self::Inert => {
                if let Some(old) = &out {
                    offer::release_offer(cx, old);
                }
                (false, None)
            }
        }
    }
}

/// `BuildOfferFromPath` (`:3107-3164`): the filters fed the path's nodes
/// from the root (a node whose allowable zone types miss the objective
/// zone's vignette type stops the walk), the last filter's offer, then its
/// base fields, texts, objectives and comm sequences.
fn build_offer_from_path(
    cx: &mut Cx<'_>,
    rm: &RmData,
    path: &TreePath,
    q: &OfferQuery,
) -> (bool, Option<Offer>) {
    let tree = &rm.tree;
    let mut filter = Filter::Base([false; 5]);
    // The record and the zone are read without a test in the C++; the
    // port takes no zone type without them (D876).
    let zone_types = cx
        .logic
        .content
        .state
        .systems
        .get(&q.dest_sys)
        .and_then(|s| {
            s.find_zone(q.objective_zone)
                .map(|i| s.zones[i].vignette_type)
        })
        .unwrap_or(0);
    let mut node = tree.nodes.get(&tree.root);
    let mut complete = true;
    for &bit in &path.bits {
        let Some(n) = node else {
            complete = false;
            break;
        };
        if n.params.allowable_zone_types & zone_types == 0 {
            complete = false;
            break;
        }
        if filter.apply_node(n, bit)
            && filter.has_mission_type()
            && let Some(next) = filter.specialize()
        {
            filter = next;
        }
        node = tree.step(n, bit);
    }
    if !complete {
        return (false, None);
    }
    let (created, made) = filter.create_offer(cx, rm, q, None);
    let Some(mut offer) = made else {
        return (false, None);
    };
    if !created {
        return (false, Some(offer));
    }
    offer.system = system_record(cx, q.dest_sys);
    offer.offer_group = q.offer_group;
    offer.hostile_group = q.enemy_group;
    offer.difficulty = remap_offer_difficulty(q.difficulty);
    offer.reward = q.reward;
    offer.rep_hit = q.rep_hit;
    offer.rep_reward = q.rep_reward;
    offer.offer_base = q.offer_base;
    let tables = cx.logic.content.tables.clone();
    // `GetFactionProps` is not tested in the C++ (D876).
    offer.mc_costume = tables
        .faction_props(q.offer_group as u32)
        .map_or(0, |p| p.mc_costume);
    if let Ok(voice) = api::misc::voice_id(b"mc_leg_m01") {
        offer.voice = voice;
    }
    build_offer_texts(cx, tree, q, path, &mut offer);
    if offer.machine_type > 0 && offer.machine_type <= 3 {
        build_comm_sequences(cx, tree, path, &mut offer);
    }
    (true, Some(offer))
}

/// `FindSystemInfo(&DestSys)` as an id: the system when Content has its
/// record, else 0.
fn system_record(cx: &Cx<'_>, system: u32) -> u32 {
    if cx.logic.content.state.systems.contains_key(&system) {
        system
    } else {
        0
    }
}

/// `RemapRmOfferDifficulty` (`:1876-1899`): the difficulty mapped to the
/// rank shown, `ln(d) * 6.3693 + 13.668` from 0.900376 on, a cubic below,
/// at least 0, rounded half up, at least 1 (record 0008 point 8: `f64::ln`
/// for x87 `FYL2X`).
#[must_use]
pub fn remap_offer_difficulty(difficulty: f32) -> f32 {
    let d = f64::from(difficulty);
    let mut mapped = if !(d < 0.900_376) {
        d.ln() * 6.3693 + 13.668
    } else {
        d * 28.682 + (d * d * d * 15.193 - d * d * 28.236) - 1.0113
    };
    if mapped < 0.0 {
        mapped = 0.0;
    }
    let rank = trunc_to_i32((mapped + 0.5).floor()) as f32;
    if 1.0 > rank { 1.0 } else { rank }
}

/// `FRmOfferText::Build` (`:740-782`): each entry whose form passes (1 with
/// the offer or hostile group singular, 2 with it plural, 3 always) built
/// as a mad lib of its string id with the offer's parameters, and appended.
fn build_offer_text(
    cx: &mut Cx<'_>,
    text: &OfferText,
    offer_singular: bool,
    hostile_singular: bool,
    out: &mut FmtStr,
    source: &Offer,
) {
    for entry in &text.entries {
        let has1 = entry.params.contains(&1);
        let has2 = entry.params.contains(&2);
        let build = match entry.form {
            1 => {
                (has1 && offer_singular) || (!(has1 && offer_singular) && has2 && hostile_singular)
            }
            2 => {
                (has1 && !offer_singular)
                    || (!(has1 && !offer_singular) && has2 && !hostile_singular)
            }
            3 => true,
            _ => false,
        };
        if !build {
            continue;
        }
        let mut part = FmtStr::new(entry.str_id as u32);
        for &param in &entry.params {
            offer::text_param(cx, source, param, &mut part);
        }
        out.append_fmt_str(&part);
    }
}

/// `BuildOfferTexts` (`:3191-3251`): down the path from the root, each
/// node's objective texts (a later node wins), one of its offer texts
/// (picked at random among several; `replace` starts the text again), and
/// its reward and failure texts when not empty; then the objectives.
fn build_offer_texts(
    cx: &mut Cx<'_>,
    tree: &VignetteTree,
    q: &OfferQuery,
    path: &TreePath,
    offer: &mut Offer,
) {
    let tables = cx.logic.content.tables.clone();
    // `GetFactionProps` is not tested in the C++ (D876).
    let singular = |group: u32| {
        tables
            .faction_props(group)
            .is_some_and(|p| p.singular_nickname)
    };
    let offer_singular = singular(q.offer_group as u32);
    let hostile_singular = singular(q.enemy_group);
    let mut objectives: std::collections::BTreeMap<i32, ParamText> =
        std::collections::BTreeMap::new();
    offer.text.mad_lib_id = 0x50001;
    let mut node = tree.nodes.get(&tree.root);
    let mut bit_index = 0;
    while let Some(n) = node {
        for (key, text) in &n.params.objective_texts {
            objectives.insert(*key, text.clone());
        }
        if !n.params.offer_texts.is_empty() {
            let index = super::tables::random_index(cx, n.params.offer_texts.len());
            let chosen = &n.params.offer_texts[index];
            let mut text = std::mem::replace(&mut offer.text, FmtStr::new(0));
            if !chosen.append {
                text = FmtStr::new(0x50001);
            }
            build_offer_text(
                cx,
                chosen,
                offer_singular,
                hostile_singular,
                &mut text,
                offer,
            );
            offer.text = text;
        }
        if !n.params.reward_text.is_empty() {
            offer.reward_text = n.params.reward_text.clone();
        }
        if !n.params.failure_text.is_empty() {
            offer.failure_text = n.params.failure_text.clone();
        }
        if n.children.is_empty() {
            break;
        }
        // The C++ reads the path without an end test (D873).
        let Some(&bit) = path.bits.get(bit_index) else {
            break;
        };
        node = tree.step(n, bit);
        bit_index += 1;
    }
    build_objectives(q, offer, &objectives);
}

/// `BuildObjectives` (`:3267-3345`): one objective per text, the guides
/// (keys 1 to 3) with the marker of the objective; into the general (1, 2,
/// 9 to 13), solar (4, 5), target ship (3, 19 to 21) or ship (22 to 24)
/// component under the key with 19/22 as 6, 20/23 as 7 and 21/24 as 8; a
/// key already there keeps its objective. Any other key goes through a null
/// map in the C++; the port drops it (D872).
fn build_objectives(
    q: &OfferQuery,
    offer: &mut Offer,
    objectives: &std::collections::BTreeMap<i32, ParamText>,
) {
    for (&key, text) in objectives {
        let objective = if matches!(key, 1..=3) {
            Objective {
                kind: 2,
                text: text.clone(),
                marker_system: q.dest_sys,
                marker_value: 0x44d,
                marker_position: q.objective_pos,
            }
        } else {
            Objective {
                kind: 3,
                text: text.clone(),
                marker_system: 0,
                marker_value: 0,
                marker_position: Vector::ZERO,
            }
        };
        let map = match key {
            1 | 2 | 9..=13 => &mut offer.general.objectives,
            4 | 5 => &mut offer.solars.component.objectives,
            3 | 19..=21 => &mut offer.target_ships.component.objectives,
            22..=24 => &mut offer.ships.component.objectives,
            _ => continue,
        };
        let key = match key {
            19 | 22 => 6,
            20 | 23 => 7,
            21 | 24 => 8,
            k => k,
        };
        map.entry(key).or_insert(objective);
    }
}

/// `BuildCommSequences` (`:3360-3402`): down the path, one conversation per
/// comm sequence into the general component, with its trigger, a delay of
/// `min + (max - min) * rand() / 32768` and its lines.
fn build_comm_sequences(cx: &mut Cx<'_>, tree: &VignetteTree, path: &TreePath, offer: &mut Offer) {
    let mut node = tree.nodes.get(&tree.root);
    let mut bit_index = 0;
    while let Some(n) = node {
        for seq in &n.params.comm_sequences {
            let fraction = f64::from(cx.world.rng().rand()) * f64::from(1.0f32 / 32768.0);
            let delay = (f64::from(seq.max_delay) - f64::from(seq.min_delay)) * fraction
                + f64::from(seq.min_delay);
            offer.general.conversations.push(Conversation {
                ready: false,
                delay: delay as f32,
                trigger: seq.trigger,
                lines: seq
                    .lines
                    .iter()
                    .map(|l| ConversationLine {
                        speaker: l.speaker,
                        target: seq.target,
                        text: l.text.clone(),
                    })
                    .collect(),
            });
        }
        if n.children.is_empty() {
            break;
        }
        let Some(&bit) = path.bits.get(bit_index) else {
            break;
        };
        node = tree.step(n, bit);
        bit_index += 1;
    }
}

/// The formation size a ship block gets (`FRmShipSize`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ShipSize {
    /// The `NpcRankToDiff` entry of the count.
    pub difficulty: f32,
    /// The NPC level.
    pub level: i32,
    /// The number of ships; 0 when none.
    pub count: i32,
}

/// A ship block (`FRmShipBlock`, the 0x20-byte stack record).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct ShipBlock {
    f00: u8,
    f01: u8,
    f02: u8,
    f03: u8,
    f04: u8,
    size: ShipSize,
    f14: i32,
    f18: i32,
    f1c: i32,
}

impl ShipBlock {
    /// `ResetRmShipBlock`: the five flags, the size zeroed, `+0x14` 0 and
    /// the two ints.
    fn reset(f00: u8, f01: u8, f02: u8, f03: u8, f04: u8, f18: i32, f1c: i32) -> Self {
        Self {
            f00,
            f01,
            f02,
            f03,
            f04,
            size: ShipSize::default(),
            f14: 0,
            f18,
            f1c,
        }
    }

    /// `CopyFriendlyRmShipBlock`: with no count, level 0 becomes 1 and the
    /// count 1.
    fn friendly_copy(&self) -> Self {
        let mut copy = *self;
        if self.size.count == 0 {
            if self.size.level == 0 {
                copy.size.level = 1;
            }
            copy.size.count = 1;
        }
        copy
    }
}

/// `GRmFormationSizeWeights`: single player, then multiplayer, by count.
const FORMATION_SIZE_WEIGHTS: [[f32; 9]; 2] = [
    [0.0, 0.0, 3.0, 9.0, 6.0, 2.0, 1.0, 1.0, 1.0],
    [0.0, 0.0, 2.0, 3.0, 9.0, 6.0, 4.0, 2.0, 1.0],
];

/// `PickRmShipFormationSize` (`:1918-1993`): per NPC level 1 to 25 of the
/// faction's `class_fighter` archetypes, the `NpcRankToDiff` count whose
/// difficulty is nearest, then a weighted pick by count. With nothing
/// picked only the count is set to 0. The C++ inserts an empty list for a
/// level the table lacks (`operator[]`); the port reads it as empty (D878).
fn pick_ship_formation_size(
    cx: &mut Cx<'_>,
    rm: &RmData,
    out: &mut ShipSize,
    difficulty: f32,
    faction: u32,
) {
    let tables = cx.logic.content.tables.clone();
    let line = EncounterShip {
        ship_classes: vec![create_id(b"class_fighter")],
        ..EncounterShip::default()
    };
    let mut levels = [false; 0x1a];
    for id in cargo::faction_ship_line_archs(&tables, faction, &line) {
        let level = tables.npc_ship(id).map_or(0, |a| a.level);
        // The C++ flags a level without a bound (D877).
        if level != 0
            && let Some(slot) = usize::try_from(level).ok().and_then(|l| levels.get_mut(l))
        {
            *slot = true;
        }
    }
    let empty = Vec::new();
    let diffs_of = |level: i32| {
        rm.tables
            .npc_rank_to_difficulty
            .get(&level)
            .unwrap_or(&empty)
    };
    let mut counts = [-1i32; 0x1a];
    for (level, count) in counts.iter_mut().enumerate().skip(1) {
        if !levels[level] {
            continue;
        }
        let diffs = diffs_of(level as i32);
        for (i, &d) in diffs.iter().enumerate() {
            if d > difficulty {
                if i != 0 {
                    let below = f64::from(difficulty) - f64::from(diffs[i - 1]);
                    let above = f64::from(d) - f64::from(difficulty);
                    let picked = if above < below { i } else { i - 1 };
                    *count = picked as i32 + 1;
                }
                break;
            }
        }
    }
    let weights = &FORMATION_SIZE_WEIGHTS[usize::from(!api::misc::single_player(cx))];
    let mut choices: Vec<(ShipSize, f32)> = Vec::new();
    for (level, &count) in counts.iter().enumerate().skip(1) {
        if count < 1 {
            continue;
        }
        let diffs = diffs_of(level as i32);
        choices.push((
            ShipSize {
                difficulty: diffs[(count - 1) as usize],
                level: level as i32,
                count,
            },
            // The count indexes nine weights without a bound in the C++;
            // the shipped table has at most eight values per rank.
            weights.get(count as usize).copied().unwrap_or(0.0),
        ));
    }
    if !choices.is_empty() {
        let mut sum = 0.0f32;
        for (_, w) in &choices {
            sum = (f64::from(sum) + f64::from(*w)) as f32;
        }
        if sum != 0.0 {
            let target = (f64::from(cx.world.rng().rand() as f32)
                * f64::from(1.0f32 / 32768.0)
                * f64::from(sum)) as f32;
            let mut running = 0.0f32;
            for (size, w) in &choices {
                running = (f64::from(running) + f64::from(*w)) as f32;
                if target <= running {
                    *out = *size;
                    return;
                }
            }
        }
    }
    out.count = 0;
}

/// `BuildRmShips` (`:2022-2101`): `size.count` ships of `group_id` into the
/// group, at most one per slot of the faction's `fighters` formation, each
/// a `class_fighter` / `D<level>` archetype named `Vignette_ship_%X`; the
/// first with the loot and the first values, the others with the others.
#[allow(clippy::too_many_arguments)]
fn build_ships(
    cx: &mut Cx<'_>,
    group: &mut ShipGroup,
    group_id: u32,
    size: ShipSize,
    first_d8: i32,
    other_d8: i32,
    first_dc: i32,
    other_dc: i32,
    loot: Option<&LootSpawn>,
    named: bool,
    match_level: bool,
) -> bool {
    if size.count == 0 {
        return false;
    }
    let tables = cx.logic.content.tables.clone();
    // The faction's properties and the formation are not tested in the
    // C++ (D876).
    if let Some(formation) = tables
        .faction_props(group_id)
        .and_then(|p| p.formation(create_id(b"fighters")))
    {
        group.formation = formation;
    }
    let mut slots_left = tables
        .formation(group.formation)
        .map_or(0, |f| f.slots.len() as i32);
    let mut classes = Vec::new();
    add_unique(&mut classes, create_id(b"class_fighter"));
    let level_class = if size.level == 0 {
        "D1".to_owned()
    } else {
        format!("D{}", size.level)
    };
    add_unique(&mut classes, create_id(level_class.as_bytes()));
    let line = EncounterShip {
        ship_classes: classes,
        ..EncounterShip::default()
    };
    let Some(arch_id) = cargo::pick_faction_ship_line_arch(cx, &tables, group_id, &line, -1) else {
        return false;
    };
    let (arch_level, arch_nickname) = tables
        .npc_ship(arch_id)
        .map_or((0, arch_id), |a| (a.level, a.nickname));
    if match_level && (size.level == 0 || (arch_level - size.level).abs() >= 2) {
        return false;
    }
    let mut ship = ShipSpawn {
        nickname: Vec::new(),
        random_name: 1,
        affiliation: group_id,
        npc_ship_arch: arch_nickname,
        target_name: false,
        field_0xd8: 2,
        field_0xdc: 1,
        loot: Vec::new(),
    };
    let mut first = true;
    for _ in 0..size.count {
        let slots = slots_left;
        slots_left -= 1;
        if slots <= 0 {
            break;
        }
        if first {
            first = false;
            if let Some(l) = loot {
                ship.loot.push(l.clone());
            }
            ship.field_0xd8 = first_d8;
            ship.field_0xdc = first_dc;
            if first_dc == 2 {
                ship.random_name = 0;
                ship.target_name = named;
            }
        } else {
            ship.loot.clear();
            ship.field_0xd8 = other_d8;
            ship.field_0xdc = other_dc;
            ship.random_name = 1;
            ship.target_name = false;
        }
        let rm = &mut cx.logic.content.state.rm;
        let mut name = format!("Vignette_ship_{:X}", rm.ship_counter).into_bytes();
        rm.ship_counter = rm.ship_counter.wrapping_add(1);
        name.truncate(0x2f);
        ship.nickname = name;
        group.ships.push(ship.clone());
    }
    true
}

/// `BuildRmShipGroup` (`:2119-2200`): a new ship group of the block in the
/// component (the offer group's when friendly, else the hostile group's)
/// at `position`, with a critical loot (block `+0x04`) or, when the drop
/// chance beats a roll, a bonus loot for its first ship.
#[allow(clippy::too_many_arguments)]
fn build_ship_group(
    cx: &mut Cx<'_>,
    rm: &RmData,
    q: &OfferQuery,
    component: &mut ShipComponent,
    block: &ShipBlock,
    position: Vector,
    friendly: bool,
    named: bool,
) -> bool {
    component.component.active = true;
    let group_id = if friendly {
        q.offer_group as u32
    } else {
        q.enemy_group
    };
    let serial = {
        let rm_state = &mut cx.logic.content.state.rm;
        rm_state.ship_group_serial = rm_state.ship_group_serial.wrapping_add(1);
        rm_state.ship_group_serial
    };
    let mut group = ShipGroup {
        field_0x00: block.f00,
        field_0x01: 0,
        field_0x02: block.f03,
        field_0x04: block.f18,
        field_0x08: block.f1c,
        system: system_record(cx, q.dest_sys),
        orientation: Matrix::IDENTITY,
        position,
        field_0x40: if friendly { 0.9 } else { -0.9 },
        formation: create_id(b"transport_helion"),
        ships: Vec::new(),
        serial,
    };
    let (mut first_d8, mut other_d8) = (0, 0);
    if block.f02 != 0 {
        (first_d8, other_d8) = (2, 2);
    }
    if block.f01 != 0 {
        (first_d8, other_d8) = (2, 0);
    }
    let mut looted = true;
    if block.f04 == 0 {
        let roll = (f64::from(cx.world.rng().rand()) * f64::from(1.0f32 / 32768.0)) as f32;
        looted = rm.tables.bonus_loot_drop_chance > roll;
    }
    let loot = if looted {
        let mut l = LootSpawn {
            kind: 0,
            field_0x100: 0,
            critical: None,
            archetype: 0,
            units: 0,
            system: 0,
            hit_pts_scale: 0.0,
            can_ai_tractor: 0,
            rel_pos: 0,
            rel_pos_offset: Vector::ZERO,
        };
        if block.f04 != 0 {
            // A missing record is read through null in the C++ (D875).
            let critical = rm.tables.pick_critical_loot(cx, group_id, block.f14 as u32);
            l.kind = 2;
            l.critical = critical;
            l.field_0x100 = block.f14;
            l.archetype = critical
                .and_then(|i| rm.tables.critical_loot.get(i))
                .map_or(0, |r| r.archetype);
            l.units = 1;
        } else {
            l.kind = 1;
            // A missing record is read through null in the C++ (D875).
            if let Some(bonus) = rm.tables.pick_bonus_loot(cx, q.enemy_group, q.difficulty) {
                l.archetype = bonus.archetype;
                let min = bonus.num_to_drop_min;
                let spread = bonus.num_to_drop_max.wrapping_sub(min).wrapping_add(1);
                l.units =
                    (cx.world.rng().rand().wrapping_mul(spread) / 0x8000).wrapping_add(min) as u32;
            }
        }
        l.rel_pos = 1;
        l.system = q.dest_sys;
        l.hit_pts_scale =
            (f64::from(cx.world.rng().rand()) * f64::from(1.0f32 / 32768.0) * f64::from(0.7f32)
                + f64::from(0.3f32)) as f32;
        Some(l)
    } else {
        None
    };
    let first_dc = if block.f01 != 0 { 2 } else { 1 };
    let built = build_ships(
        cx,
        &mut group,
        group_id,
        block.size,
        first_d8,
        other_d8,
        first_dc,
        1,
        loot.as_ref(),
        named,
        !friendly,
    );
    component.groups.push(group);
    built
}

/// The pilots of vignette solars (`.rdata 06fbcbf8`).
const SOLAR_PILOTS: [&[u8]; 4] = [
    b"pilot_rm_solar_easiest",
    b"pilot_rm_solar_easy",
    b"pilot_rm_solar_hard",
    b"pilot_rm_solar_hardest",
];

/// `FRmDestroySolarsFilter::SpawnSolars` and `FRmAssassinateFilter::SpawnSolars`
/// (`:2439-2524`, `:2966-3038`): for each entry, one solar per formation
/// slot of its type while nothing of it was spawned or the committed
/// difficulty is below `difficulty`. `pk` gives the destroy-solars filter's
/// `pk_big_solar` and `pk_defensive_solars`; the assassinate filter has
/// none and every mode is 0.
#[allow(clippy::too_many_arguments)]
fn spawn_solars(
    cx: &mut Cx<'_>,
    rm: &RmData,
    q: &OfferQuery,
    enemy: u32,
    difficulty: f32,
    formation: u32,
    entries: &[(KillableSolar, f32)],
    out: &mut Vec<SolarSpawn>,
    pk: Option<(bool, bool)>,
) {
    let Some(formation) = rm.tables.solar_formations.get(&formation) else {
        return;
    };
    let cap = f32::from_bits(0x3f7f_be77);
    let mut committed = 0.0f32;
    for (arch, scaled) in entries {
        let mode = match pk {
            Some((big, _)) if arch.solar_type == 0 && big => 2,
            Some((_, defensive)) if arch.solar_type == 1 && defensive => 2,
            _ => 0,
        };
        let mut used = 0.0f32;
        for slot in &formation.slots {
            if slot.solar_type == arch.solar_type && (used == 0.0 || committed < difficulty) {
                let rm_state = &mut cx.logic.content.state.rm;
                let mut name = format!("Vignette_solar_{:X}", rm_state.solar_counter).into_bytes();
                rm_state.solar_counter = rm_state.solar_counter.wrapping_add(1);
                name.truncate(0x2f);
                let mut fraction = ((f64::from(*scaled) - f64::from(arch.difficulty_min))
                    / (f64::from(arch.difficulty_max) - f64::from(arch.difficulty_min)))
                    as f32;
                if !(fraction < cap) {
                    fraction = cap;
                }
                let pilot_index = trunc_to_i32(f64::from(arch.pilot_choices) * f64::from(fraction));
                // The index reads past the four names without a bound in the
                // C++ (D877).
                let pilot = usize::try_from(pilot_index)
                    .ok()
                    .and_then(|i| SOLAR_PILOTS.get(i))
                    .map_or(0, |n| create_id(n));
                let hitpoints = trunc_to_i32(
                    (f64::from(arch.hitpoints_max) - f64::from(arch.hitpoints_min))
                        * f64::from(fraction)
                        + f64::from(arch.hitpoints_min),
                );
                used = (f64::from(*scaled) + f64::from(used)) as f32;
                out.push(SolarSpawn {
                    nickname: name,
                    faction: enemy,
                    orientation: slot.orientation,
                    position: Vector::new(
                        (f64::from(slot.position.x) + f64::from(q.objective_pos.x)) as f32,
                        (f64::from(slot.position.y) + f64::from(q.objective_pos.y)) as f32,
                        (f64::from(slot.position.z) + f64::from(q.objective_pos.z)) as f32,
                    ),
                    archetype: arch.archetype,
                    loadout: arch.loadout,
                    pilot,
                    has_pilot: 1,
                    field_0x4c8: -0.9,
                    arch: arch.nickname,
                    arch_string_id: arch.string_id,
                    solar_type: arch.solar_type,
                    dest_sys: q.dest_sys,
                    mode_flag: mode,
                    invincible_duration: 0.0,
                    field_0x4e0: 0,
                    hitpoints,
                    loot: Vec::new(),
                });
            }
            if f64::from(used) + f64::from(committed) >= f64::from(difficulty) {
                break;
            }
        }
        committed = (f64::from(used) + f64::from(committed)) as f32;
    }
}

/// `pub::System::GetNestedProperties` into a slot that starts at 0.
fn nested_properties(cx: &mut Cx<'_>, q: &OfferQuery) -> u32 {
    api::system::nested_properties(cx, q.dest_sys, q.objective_pos).unwrap_or(0)
}

/// `f32 * f32` as the x87 evaluates it and stores it.
fn mul(a: f32, b: f32) -> f32 {
    (f64::from(a) * f64::from(b)) as f32
}

/// `FRmDestroySolarsFilter::FillSolarSpawns` (`:2371-2424`).
fn fill_solar_spawns(
    cx: &mut Cx<'_>,
    rm: &RmData,
    f: &[bool; 14],
    q: &OfferQuery,
    solars: &mut SolarComponent,
) -> bool {
    solars.position = q.objective_pos;
    solars.component.active = true;
    let mut types = Vec::new();
    if f[0] {
        add_unique(&mut types, 0);
    }
    if f[2] {
        add_unique(&mut types, 1);
    }
    let Some(formation) = rm.tables.pick_solar_formation(cx, &types) else {
        return false;
    };
    let nested = nested_properties(cx, q);
    let mut entries = Vec::new();
    for &kind in &types {
        let mut scaled = q.difficulty;
        if kind == 0 {
            if f[7] {
                scaled = mul(scaled, 0.4);
            }
            if f[2] {
                scaled = mul(scaled, 0.6);
            }
        } else if kind == 1 {
            if f[7] {
                scaled = mul(scaled, 0.4);
            }
            if f[0] {
                scaled = mul(scaled, 0.399_999_98);
            }
        }
        let Some(entry) = rm
            .tables
            .pick_killable_solar(cx, q.enemy_group, kind, nested, scaled)
        else {
            return false;
        };
        entries.push((entry.clone(), scaled));
    }
    spawn_solars(
        cx,
        rm,
        q,
        q.enemy_group,
        q.difficulty,
        formation,
        &entries,
        &mut solars.solars,
        Some((f[1], f[3])),
    );
    true
}

/// `FRmDestroySolarsFilter::CreateOffer` (`:2231-2359`).
fn destroy_solars_offer(
    cx: &mut Cx<'_>,
    rm: &RmData,
    f: &[bool; 14],
    q: &OfferQuery,
) -> (bool, Option<Offer>) {
    let mut offer = Offer::new(1);
    offer.general.active = true;
    offer.kind = 1;
    let mut success = fill_solar_spawns(cx, rm, f, q, &mut offer.solars);
    let position = q.objective_pos;
    offer.ships.position = position;
    let (at_installation, after_30_s, waves, pk) = (f[7], f[8], f[9], f[6]);
    let mut divisor = f64::from(i32::from(after_30_s) + i32::from(at_installation))
        + f64::from(i32::from(waves)) * 0.5;
    if !(1.0 <= divisor) {
        divisor = 1.0;
    }
    let hostile_share = (f64::from(0.6f32) / divisor * f64::from(q.difficulty)) as f32;
    let friendly_share = mul(hostile_share, 0.5);
    let offer_group = q.offer_group as u32;
    let mut ships = std::mem::take(&mut offer.ships);
    if at_installation {
        let mut block = ShipBlock {
            f00: u8::from(pk),
            f02: u8::from(pk),
            f1c: -1,
            ..ShipBlock::default()
        };
        pick_ship_formation_size(cx, rm, &mut block.size, hostile_share, q.enemy_group);
        let built = build_ship_group(cx, rm, q, &mut ships, &block, position, false, false);
        success = success && built;
    }
    if f[11] {
        let mut block = ShipBlock {
            f18: 1,
            f1c: 1,
            ..ShipBlock::default()
        };
        pick_ship_formation_size(cx, rm, &mut block.size, friendly_share, offer_group);
        let friendly = block.friendly_copy();
        let built = build_ship_group(cx, rm, q, &mut ships, &friendly, position, true, false);
        success = success && built;
    }
    if after_30_s {
        let (f18, f1c) = if at_installation { (3, 1) } else { (1, 8) };
        let mut block = ShipBlock {
            f00: u8::from(pk),
            f02: u8::from(pk),
            f18,
            f1c,
            ..ShipBlock::default()
        };
        pick_ship_formation_size(cx, rm, &mut block.size, hostile_share, q.enemy_group);
        let built = build_ship_group(cx, rm, q, &mut ships, &block, position, false, false);
        success = success && built;
    }
    if f[12] {
        let mut block = ShipBlock {
            f18: 1,
            f1c: 9,
            ..ShipBlock::default()
        };
        pick_ship_formation_size(cx, rm, &mut block.size, friendly_share, offer_group);
        let friendly = block.friendly_copy();
        let built = build_ship_group(cx, rm, q, &mut ships, &friendly, position, true, false);
        success = success && built;
    }
    if waves {
        let odd = (at_installation || after_30_s) && cx.world.rng().rand() % 2 != 0;
        let (f18, f1c) = if odd { (3, 1) } else { (1, 0xf0) };
        let mut block = ShipBlock {
            f02: u8::from(pk),
            f18,
            f1c,
            ..ShipBlock::default()
        };
        pick_ship_formation_size(cx, rm, &mut block.size, hostile_share, q.enemy_group);
        let built = build_ship_group(cx, rm, q, &mut ships, &block, position, false, false);
        success = success && built;
    }
    offer.ships = ships;
    (success, Some(offer))
}

/// The critical loot kind of a target block: `bring_back_loot` 3, else
/// `tractor_in_loot` 2, else `destroy_loot` 1, else 0.
fn critical_loot_kind(f: &[bool; 27]) -> i32 {
    if f[3] {
        3
    } else if f[4] {
        2
    } else {
        i32::from(f[5])
    }
}

/// `FRmAssassinateFilter::CreateOffer` (`:2539-2615`).
fn assassinate_offer(
    cx: &mut Cx<'_>,
    rm: &RmData,
    f: &[bool; 27],
    query: &OfferQuery,
    out: Option<Offer>,
) -> (bool, Option<Offer>) {
    let mut offer = Offer::new(if f[0] { 2 } else { 3 });
    offer.general.active = true;
    offer.kind = if f[0] {
        if f[3] {
            4
        } else if f[4] {
            5
        } else if f[5] {
            3
        } else {
            2
        }
    } else {
        0
    };
    let mut q = *query;
    if f[2] && f[3] {
        q.difficulty = mul(q.difficulty, 0.9);
    }
    if f[0] {
        // The C++ reads the names without a test (D874).
        let has_names = cx
            .logic
            .content
            .tables
            .faction_names(q.enemy_group)
            .is_some_and(|n| !n.male_first_names.is_empty());
        if !has_names {
            offer::release_offer(cx, &offer);
            return (false, out);
        }
        offer.target_name.gender = 1;
        let mut name = std::mem::take(&mut offer.target_name);
        let built = npc_names::build_default_npc_name(cx, q.enemy_group, &mut name, true);
        offer.target_name = name;
        if !built {
            offer::release_offer(cx, &offer);
            return (false, out);
        }
    }
    let mut count = 0;
    if f[7] {
        count = if f[19] {
            i32::from(f[21]) + 1
        } else {
            i32::from(f[9]) + i32::from(f[8])
        };
    }
    let main = if f[19] {
        i32::from(f[18]) + i32::from(f[23]) * 2
    } else {
        i32::from(f[17]) + i32::from(f[18]) + i32::from(f[16]) + i32::from(f[15])
    };
    count += main;
    let share = if count != 0 {
        (f64::from(q.difficulty) / ((f64::from(count) + 1.0) * 0.5)) as f32
    } else {
        0.0
    };
    let mut success = true;
    if f[7] {
        let mut target = std::mem::take(&mut offer.target_ships);
        success = fill_target_ships(cx, rm, f, &q, &mut target, share);
        offer.target_ships = target;
    }
    let mut ships = std::mem::take(&mut offer.ships);
    let mut solars = std::mem::take(&mut offer.solars);
    let built = fill_ship_groups(cx, rm, f, &q, &mut ships, share, &mut solars);
    offer.ships = ships;
    offer.solars = solars;
    success = success && built;
    if let Some(old) = &out {
        offer::release_offer(cx, old);
    }
    (success, Some(offer))
}

/// `FRmAssassinateFilter::FillTargetShips` (`:2677-2768`).
fn fill_target_ships(
    cx: &mut Cx<'_>,
    rm: &RmData,
    f: &[bool; 27],
    q: &OfferQuery,
    target: &mut ShipComponent,
    share: f32,
) -> bool {
    let mut f50 = 0;
    let mut f54 = -1;
    let mut success = true;
    let offset = random_inside_ring(6000.0, 6000.0 - 500.0, 300.0, cx.world.rng());
    let position = Vector::new(
        (f64::from(q.objective_pos.x) + f64::from(offset.x)) as f32,
        (f64::from(q.objective_pos.y) + f64::from(offset.y)) as f32,
        (f64::from(q.objective_pos.z) + f64::from(offset.z)) as f32,
    );
    target.position = position;
    let mut block;
    if f[19] {
        if f[10] {
            f50 = 2;
            f54 = 1;
        }
        block = ShipBlock::reset(1, 0, 1, 0, 0, 0, -1);
        pick_ship_formation_size(cx, rm, &mut block.size, share, q.enemy_group);
        success = build_ship_group(cx, rm, q, target, &block, position, false, false);
        if f[21] {
            block.f18 = 3;
            block.f1c = 1;
            pick_ship_formation_size(cx, rm, &mut block.size, share, q.enemy_group);
            let built = build_ship_group(cx, rm, q, target, &block, position, false, false);
            success = success && built;
        }
    } else {
        if f[9] {
            block = ShipBlock::reset(1, 0, 1, 0, 0, 0, -1);
            if !f[8] {
                if f[10] {
                    f50 = 2;
                    f54 = 1;
                } else {
                    f50 = 0;
                }
            }
            pick_ship_formation_size(cx, rm, &mut block.size, share, q.enemy_group);
            success = build_ship_group(cx, rm, q, target, &block, position, false, false);
        }
        if f[8] {
            block = ShipBlock::reset(1, 1, 0, 1, u8::from(f[2]), 0, -1);
            if f[2] {
                block.f14 = critical_loot_kind(f);
            }
            if f[9] {
                let ships = target.groups.last().map_or(0, |g| g.ships.len());
                if ships > 1 {
                    block.f18 = 3;
                    block.f1c = 1;
                } else {
                    block.f18 = 2;
                    block.f1c = 8;
                }
            } else {
                block.f18 = 0;
            }
            f50 = 3;
            f54 = 0x63;
            pick_ship_formation_size(cx, rm, &mut block.size, mul(share, 1.5), q.enemy_group);
            let built = build_ship_group(cx, rm, q, target, &block, position, false, true);
            success = success && built;
        }
    }
    target.field_0x50 = f50;
    target.field_0x54 = f54;
    success
}

/// `FRmAssassinateFilter::FillShipGroups` (`:2787-2920`).
fn fill_ship_groups(
    cx: &mut Cx<'_>,
    rm: &RmData,
    f: &[bool; 27],
    q: &OfferQuery,
    ships: &mut ShipComponent,
    share: f32,
    solars: &mut SolarComponent,
) -> bool {
    let position = q.objective_pos;
    let mut success = true;
    if f[14] {
        success = fill_defensive_solars(cx, rm, q, solars);
    }
    ships.position = q.objective_pos;
    let mut friendly_share = mul(share, 0.5);
    let offer_group = q.offer_group as u32;
    let enemy = q.enemy_group;
    if f[19] {
        let mut hostile = ShipBlock::reset(1, 0, 1, 0, 0, 0, -1);
        let mut friendly = ShipBlock::reset(0, 0, 0, 0, 0, 2, 1);
        if f[22] {
            pick_ship_formation_size(cx, rm, &mut hostile.size, share, enemy);
            let built = build_ship_group(cx, rm, q, ships, &hostile, position, false, false);
            success = success && built;
        }
        if f[24] {
            pick_ship_formation_size(cx, rm, &mut friendly.size, friendly_share, offer_group);
            let copy = friendly.friendly_copy();
            let built = build_ship_group(cx, rm, q, ships, &copy, position, true, false);
            success = success && built;
        }
        if f[23] {
            if f[22] {
                hostile.f18 = 3;
                hostile.f1c = 1;
            } else {
                hostile.f18 = 1;
                hostile.f1c = 8;
            }
            pick_ship_formation_size(cx, rm, &mut hostile.size, share, enemy);
            let built = build_ship_group(cx, rm, q, ships, &hostile, position, false, false);
            success = success && built;
        }
        if f[25] {
            friendly.f18 = 1;
            friendly.f1c = 0x3c;
            pick_ship_formation_size(cx, rm, &mut friendly.size, friendly_share, offer_group);
            let copy = friendly.friendly_copy();
            let built = build_ship_group(cx, rm, q, ships, &copy, position, true, false);
            success = success && built;
        }
        if f[18] {
            hostile.f00 = 0;
            hostile.f02 = 1;
            hostile.f18 = 2;
            hostile.f1c = 0xf0;
            pick_ship_formation_size(cx, rm, &mut hostile.size, share, enemy);
            let built = build_ship_group(cx, rm, q, ships, &hostile, position, false, false);
            return success && built;
        }
        if f[26] {
            friendly.f18 = 2;
            friendly.f1c = 9;
            pick_ship_formation_size(cx, rm, &mut friendly.size, friendly_share, offer_group);
            for wave in 0..3 {
                if wave != 0 {
                    friendly.f1c = 0x19;
                }
                let copy = friendly.friendly_copy();
                let built = build_ship_group(cx, rm, q, ships, &copy, position, true, false);
                success = success && built;
            }
        }
        return success;
    }
    let mut hostile;
    if f[16] {
        let t = u8::from(f[15]);
        hostile = ShipBlock::reset(t, 0, t, 0, 0, 0, -1);
        pick_ship_formation_size(cx, rm, &mut hostile.size, share, enemy);
        let built = build_ship_group(cx, rm, q, ships, &hostile, position, false, false);
        success = success && built;
    }
    if f[15] {
        hostile = ShipBlock::reset(1, 1, 0, 1, 0, 0, -1);
        if f[16] {
            hostile.f18 = 2;
            hostile.f1c = 0x2d;
        } else {
            hostile.f18 = 0;
        }
        hostile.f04 = u8::from(f[2]);
        if f[2] {
            hostile.f14 = critical_loot_kind(f);
        }
        friendly_share = mul(share, 1.5);
        if !f[16] {
            friendly_share = mul(friendly_share, 1.3);
        }
        pick_ship_formation_size(cx, rm, &mut hostile.size, friendly_share, enemy);
        let built = build_ship_group(cx, rm, q, ships, &hostile, position, false, true);
        success = success && built;
    }
    if f[17] {
        hostile = ShipBlock::reset(0, 0, 0, 0, 0, 2, 0xf0);
        pick_ship_formation_size(cx, rm, &mut hostile.size, share, enemy);
        let built = build_ship_group(cx, rm, q, ships, &hostile, position, false, false);
        success = success && built;
    }
    if f[18] {
        hostile = ShipBlock::reset(0, 0, 0, 0, 0, 2, 0x78);
        pick_ship_formation_size(cx, rm, &mut hostile.size, share, enemy);
        let built = build_ship_group(cx, rm, q, ships, &hostile, position, false, false);
        success = success && built;
    }
    success
}

/// `FRmAssassinateFilter::FillDefensiveSolars` (`:2929-2960`): one to three
/// defensive solars at the objective.
fn fill_defensive_solars(
    cx: &mut Cx<'_>,
    rm: &RmData,
    q: &OfferQuery,
    solars: &mut SolarComponent,
) -> bool {
    let count = cx.world.rng().rand().wrapping_mul(3) / 0x8000 + 1;
    solars.component.active = true;
    solars.position = q.objective_pos;
    let types = [1];
    let Some(formation) = rm.tables.pick_solar_formation(cx, &types) else {
        return false;
    };
    let nested = nested_properties(cx, q);
    let total = mul(q.difficulty, 0.25);
    let per_solar = (f64::from(total) / f64::from(count)) as f32;
    let Some(entry) = rm
        .tables
        .pick_killable_solar(cx, q.enemy_group, 1, nested, per_solar)
    else {
        return false;
    };
    let entries = [(entry.clone(), per_solar)];
    spawn_solars(
        cx,
        rm,
        q,
        q.enemy_group,
        total,
        formation,
        &entries,
        &mut solars.solars,
        None,
    );
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_rank_is_at_least_one() {
        assert_eq!(remap_offer_difficulty(0.0), 1.0);
        assert_eq!(remap_offer_difficulty(1.0), 14.0);
        assert_eq!(remap_offer_difficulty(100.0), 43.0);
    }

    #[test]
    fn the_destroy_solars_filter_takes_cavalry_once() {
        let mut node = VignetteNode::default();
        let mut filter = Filter::Base([false; 5]);
        node.nickname = create_id(b"destroy_solars_mission");
        assert!(filter.apply_node(&node, 1));
        assert!(filter.has_mission_type());
        filter = filter.specialize().expect("a destroy-solars filter");
        node.nickname = create_id(b"cavalry");
        assert!(!filter.apply_node(&node, 0));
        assert!(filter.apply_node(&node, 1));
        assert!(filter.has_mission_type());
        let cavalry = filter.specialize().expect("the cavalry filter");
        assert!(!cavalry.has_mission_type());
        assert_eq!(
            cavalry,
            Filter::DestroySolars {
                base: [false; 5],
                flags: [false; 14],
                cavalry: true
            }
        );
    }
}
