//! The vignette tree of `RandomMissions\VignetteParams.ini` (TU63,
//! `Content/RandomMissionGen.cpp:396-1090`): the nodes with their
//! parameters (difficulty windows, offer and objective texts, comm
//! sequences, groups, zone types), the generator root's `Load`, `Link` and
//! `PropagateDifficultyWindows`, and the decision path (`FRmTreePath`) an
//! offer is built from (work package P3.7a, record 0008 point 8).
//!
//! The tree never changes after `Link`, so it lives in Content's
//! random-mission data behind an `Arc` with the other tables.

// The C++'s negated float tests (`!(x > 0.0f)`) send NaN to the failing
// side; the port spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use std::collections::BTreeMap;

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::tables::add_unique;
use crate::api;
use crate::content::tables::reputation_group;
use crate::server::Cx;

/// `GRmTextParamNames` (`RandomMissionRuntime.cpp:78-94`).
pub const TEXT_PARAM_NAMES: [(&[u8], i32); 15] = [
    (b"FSTRING_PARAM_NONE", 0),
    (b"OFFER_GROUP", 1),
    (b"HOSTILE_GROUP", 2),
    (b"BIG_SOLAR", 3),
    (b"OTHER_SOLAR", 4),
    (b"CRITICAL_LOOT", 5),
    (b"TARGET_FULL_NAME", 6),
    (b"REWARD_MONEY", 7),
    (b"FACTION_REWARD", 8),
    (b"FACTION_HIT", 9),
    (b"TIME_TO_RETURN_TO_BATTLE", 10),
    (b"MISSION_DIFFICULTY", 11),
    (b"OFFER_BASE", 12),
    (b"TARGET_ZONE", 13),
    (b"TARGET_SYSTEM", 14),
];

/// `GRmCommTriggerNames` (`RandomMissionRuntime.cpp:96-114`).
pub const COMM_TRIGGER_NAMES: [(&[u8], i32); 17] = [
    (b"NONE", 0),
    (b"IN_SPACE_IN_SYSTEM", 1),
    (b"IN_SPACE_NOT_IN_SYSTEM", 2),
    (b"LEFT_COMBAT_AREA", 11),
    (b"REENTERED_COMBAT_AREA", 12),
    (b"GLOBAL_SUCCESS", 13),
    (b"GLOBAL_FAIL_COMBAT_RANGE", 14),
    (b"GLOBAL_FAIL_DESTROYED_CRITICAL_LOOT", 15),
    (b"GLOBAL_FAIL_OTHER", 16),
    (b"PLAYER_IN_PRE_BATTLE_SPAWN_RANGE", 3),
    (b"SUCCESS_KILLED_ALL_GOING_TO_MAIN", 4),
    (b"SUCCESS_NOT_KILLED_ALL_GOING_TO_MAIN", 5),
    (b"PLAYER_IN_MAIN_BATTLE_SOLAR_SPAWN_RANGE", 6),
    (b"PLAYER_IN_MAIN_BATTLE_SHIP_SPAWN_RANGE", 7),
    (b"FIRST_FRIENDLY_SPAWN", 8),
    (b"FIRST_BIG_SHIP_SPAWN", 10),
    (b"FIRST_DEFENSIVE_SHIP_SPAWN", 9),
];

/// `GRmCommTargetNames` (`RandomMissionRuntime.cpp:116-129`).
pub const COMM_TARGET_NAMES: [(&[u8], i32); 12] = [
    (b"NONE", 0),
    (b"BASE", 1),
    (b"BIG_SOLAR", 2),
    (b"BIG_SHIP", 3),
    (b"DEFENSIVE_SHIP", 4),
    (b"FRIENDLY_SHIP", 5),
    (b"PLAYER", 6),
    (b"ALL_PLAYERS", 7),
    (b"PLAYERS_IN_RANGE", 8),
    (b"PLAYERS_OUT_OF_RANGE", 9),
    (b"PLAYERS_IN_SYSTEM", 10),
    (b"PLAYERS_OUT_OF_SYSTEM", 11),
];

/// The objective names of `FRmObjectiveText::Parse`
/// (`RandomMissionGen.cpp:788-831`), in its order.
pub const OBJECTIVE_NAMES: [(&[u8], i32); 16] = [
    (b"guide_to_main_battle", 2),
    (b"guide_to_pre_battle", 1),
    (b"guide_to_main_from_pre", 3),
    (b"kill_big_solar", 4),
    (b"kill_other_solars", 5),
    (b"pb_kill_ships", 0x13),
    (b"pb_kill_big_ships", 0x14),
    (b"pb_dont_kill_ships", 0x15),
    (b"mb_kill_ships", 0x16),
    (b"mb_kill_big_ships", 0x17),
    (b"mb_dont_kill_ships", 0x18),
    (b"return_to_battle", 9),
    (b"destroy_loot", 0xa),
    (b"tractor_loot", 0xb),
    (b"return_loot", 0xc),
    (b"jettison_loot", 0xd),
];

fn lookup(table: &[(&[u8], i32)], name: &[u8]) -> Option<i32> {
    table
        .iter()
        .find(|(n, _)| eq_ignore_case(c_str(name), n))
        .map(|&(_, v)| v)
}

/// A difficulty window (`FRmDifficultyWindow`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DifficultyWindow {
    /// `+0x04`.
    pub min: f32,
    /// `+0x08`.
    pub max: f32,
}

impl DifficultyWindow {
    /// `IsInRange`: `min <= d <= max` (a NaN is outside).
    #[must_use]
    pub fn is_in_range(&self, difficulty: f32) -> bool {
        self.min <= difficulty && difficulty <= self.max
    }

    /// `Overlaps`: this holds the other's minimum or maximum, or the other
    /// holds this minimum.
    #[must_use]
    pub fn overlaps(&self, other: &Self) -> bool {
        self.is_in_range(other.min) || self.is_in_range(other.max) || other.is_in_range(self.min)
    }

    /// `Add`: both bounds plus the other's, each stored as a float.
    fn add(&mut self, other: &Self) {
        self.min = (f64::from(other.min) + f64::from(self.min)) as f32;
        self.max = (f64::from(other.max) + f64::from(self.max)) as f32;
    }
}

/// `FRmDifficultyWindowList::AddUnique`: appended unless a window with an
/// equal minimum and maximum is there.
fn add_unique_window(list: &mut Vec<DifficultyWindow>, window: DifficultyWindow) {
    if !list
        .iter()
        .any(|w| window.min == w.min && window.max == w.max)
    {
        list.push(window);
    }
}

/// A text with parameters (`FRmParamText`): a string id and the
/// [`TEXT_PARAM_NAMES`] values the run time builds it with.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ParamText {
    /// `+0x1c`.
    pub str_id: i32,
    /// `+0x20`.
    pub params: Vec<i32>,
}

impl ParamText {
    /// `FRmParamText::Parse` (`RandomMissionRuntime.cpp:152-172`): the
    /// string id at `index`, then each following value's parameter
    /// appended. A name not in the table appends the previous lookup's
    /// value; for the first value the C++ reads an uninitialised slot, the
    /// reconstruction and the port 0.
    pub fn parse(&mut self, reader: &IniReader, index: u32) {
        self.str_id = reader.value_int(index);
        let mut value = index + 1;
        let mut param = 0;
        while !reader.is_value_empty(value) {
            if let Some(p) = lookup(&TEXT_PARAM_NAMES, &reader.value_string_at(value)) {
                param = p;
            }
            self.params.push(param);
            value += 1;
        }
    }

    /// `IsEmpty`: no string id.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.str_id == 0
    }
}

/// One entry of an `offer_text` (`FRmOfferTextEntry`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OfferTextEntry {
    /// The string id.
    pub str_id: i32,
    /// `singular` 1, `plural` 2, 3 by default.
    pub form: i32,
    /// The [`TEXT_PARAM_NAMES`] values.
    pub params: Vec<i32>,
}

/// One `offer_text` line (`FRmOfferText`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct OfferText {
    /// `append` true, `replace` false.
    pub append: bool,
    /// The entries.
    pub entries: Vec<OfferTextEntry>,
}

impl OfferText {
    /// `FRmOfferText::Parse` (`RandomMissionGen.cpp:673-724`).
    fn parse(&mut self, reader: &IniReader) {
        let mode = reader.value_string_at(0);
        if eq_ignore_case(&mode, b"append") {
            self.append = true;
        } else if eq_ignore_case(&mode, b"replace") {
            self.append = false;
        }
        let mut index = 1;
        let mut new_entry = true;
        while !reader.is_value_empty(index) {
            if new_entry {
                new_entry = false;
                self.entries.push(OfferTextEntry {
                    str_id: 0,
                    form: 3,
                    params: Vec::new(),
                });
            }
            let entry = self.entries.last_mut().expect("an entry was pushed");
            let token = reader.value_string_at(index);
            if neuron_core::msvcrt::atoi(&token) != 0 {
                entry.str_id = reader.value_int(index);
                index += 1;
                while !reader.is_value_empty(index) {
                    let Some(p) = lookup(&TEXT_PARAM_NAMES, &reader.value_string_at(index)) else {
                        break;
                    };
                    entry.params.push(p);
                    index += 1;
                }
                new_entry = true;
            } else {
                if eq_ignore_case(&token, b"singular") {
                    entry.form = 1;
                } else if eq_ignore_case(&token, b"plural") {
                    entry.form = 2;
                }
                index += 1;
            }
        }
    }
}

/// One line of a comm sequence (`FRmCommLine`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CommLine {
    /// A [`COMM_TARGET_NAMES`] value (1 by default).
    pub speaker: i32,
    /// The text, at most 0x3f bytes.
    pub text: Vec<u8>,
}

/// One `comm_sequence` (`FRmCommSequence`).
#[derive(Clone, Debug, PartialEq)]
pub struct CommSequence {
    /// A [`COMM_TRIGGER_NAMES`] value.
    pub trigger: i32,
    /// A [`COMM_TARGET_NAMES`] value (1 by default).
    pub target: i32,
    /// Value 2; no reader is known.
    pub field_0x08: f32,
    /// Value 3.
    pub min_delay: f32,
    /// Value 4.
    pub max_delay: f32,
    /// The `{speaker, text}` pairs from value 5.
    pub lines: Vec<CommLine>,
}

impl CommSequence {
    /// `FRmCommSequence::Parse` (`RandomMissionGen.cpp:857-925`): for the
    /// triggers 13 to 16 a maximum delay above 5 becomes 5, and then the
    /// minimum at most 5.
    fn parse(reader: &mut IniReader) -> Self {
        let mut s = Self {
            trigger: 0,
            target: 1,
            field_0x08: 0.0,
            min_delay: 0.0,
            max_delay: 0.0,
            lines: Vec::new(),
        };
        if let Some(t) = lookup(&COMM_TRIGGER_NAMES, &reader.value_string_at(0)) {
            s.trigger = t;
        }
        if let Some(t) = lookup(&COMM_TARGET_NAMES, &reader.value_string_at(1)) {
            s.target = t;
        }
        s.field_0x08 = reader.value_float(2) as f32;
        s.min_delay = reader.value_float(3) as f32;
        s.max_delay = reader.value_float(4) as f32;
        if (13..=16).contains(&s.trigger) && s.max_delay > 5.0 {
            s.max_delay = 5.0;
            if !(s.min_delay < 5.0) {
                s.min_delay = 5.0;
            }
        }
        let mut index = 5;
        while !reader.is_value_empty(index) {
            let mut line = CommLine {
                speaker: 1,
                text: Vec::new(),
            };
            if let Some(t) = lookup(&COMM_TARGET_NAMES, &reader.value_string_at(index)) {
                line.speaker = t;
            }
            index += 1;
            let mut text = c_str(&reader.value_string_at(index)).to_vec();
            text.truncate(0x3f);
            line.text = text;
            index += 1;
            s.lines.push(line);
        }
        s
    }
}

/// A node's parameters (`FRmVignetteParams`).
#[derive(Clone, Debug, PartialEq)]
pub struct VignetteParams {
    /// `weight` (0; 1 after a node's load when still 0).
    pub weight: f32,
    /// The `offer_text` lines.
    pub offer_texts: Vec<OfferText>,
    /// `objective_text`: an objective ([`OBJECTIVE_NAMES`]) to its text.
    pub objective_texts: BTreeMap<i32, ParamText>,
    /// `failure_text`.
    pub failure_text: ParamText,
    /// `reward_text`.
    pub reward_text: ParamText,
    /// `difficulty = min, max`.
    pub difficulty: DifficultyWindow,
    /// `offer_group`, each once (`all`: every reputation group).
    pub offer_groups: Vec<u32>,
    /// `hostile_group`, likewise.
    pub hostile_groups: Vec<u32>,
    /// The `comm_sequence` lines.
    pub comm_sequences: Vec<CommSequence>,
    /// `allowable_zone_types`: open 1, field 2, exclusion 4 (7).
    pub allowable_zone_types: u32,
}

impl Default for VignetteParams {
    fn default() -> Self {
        Self {
            weight: 0.0,
            offer_texts: Vec::new(),
            objective_texts: BTreeMap::new(),
            failure_text: ParamText::default(),
            reward_text: ParamText::default(),
            difficulty: DifficultyWindow::default(),
            offer_groups: Vec::new(),
            hostile_groups: Vec::new(),
            comm_sequences: Vec::new(),
            allowable_zone_types: 7,
        }
    }
}

/// One node of the tree (`FRmVignetteNode`).
#[derive(Clone, Debug, PartialEq)]
pub struct VignetteNode {
    /// `nickname` or `documentation`, `CreateID` of the whole value.
    pub nickname: u32,
    /// `node_id` (-1).
    pub node_id: u32,
    /// The parents' node ids, filled by `Link` (once per child line).
    pub parents: Vec<u32>,
    /// `child_node`.
    pub children: Vec<u32>,
    /// `implemented` (true); `Link` clears it below an unimplemented node.
    pub implemented: bool,
    /// The difficulty windows `PropagateDifficultyWindows` gives.
    pub windows: Vec<DifficultyWindow>,
    /// The parameters.
    pub params: VignetteParams,
    /// The chance of the first child on a path; -1 means one over the
    /// child count. No loader writes it.
    pub first_child_chance: f32,
}

impl Default for VignetteNode {
    fn default() -> Self {
        Self {
            nickname: 0,
            node_id: u32::MAX,
            parents: Vec::new(),
            children: Vec::new(),
            implemented: true,
            windows: Vec::new(),
            params: VignetteParams::default(),
            first_child_chance: -1.0,
        }
    }
}

impl VignetteNode {
    /// `FRmVignetteNode::Load` (`RandomMissionGen.cpp:1051-1060`): each
    /// value to the common keys, else to the parameters; a weight of 0
    /// becomes 1.
    fn load(&mut self, cx: &Cx<'_>, all_groups: &mut Option<Vec<u32>>, reader: &mut IniReader) {
        while reader.read_value() {
            if !self.parse_common_key(reader) {
                self.parse_param_key(cx, all_groups, reader);
            }
        }
        if self.params.weight == 0.0 {
            self.params.weight = 1.0;
        }
    }

    /// `ParseCommonKey` (`:1064-1088`).
    fn parse_common_key(&mut self, reader: &mut IniReader) -> bool {
        if reader.is_value(b"node_id") {
            self.node_id = reader.value_int(0) as u32;
        } else if reader.is_value(b"child_node") {
            self.children.push(reader.value_int(0) as u32);
        } else if reader.is_value(b"implemented") {
            self.implemented = reader.value_bool(0);
        } else if reader.is_value(b"nickname") || reader.is_value(b"documentation") {
            self.nickname = create_id(c_str(&reader.value_string()));
        } else {
            return false;
        }
        true
    }

    /// `FRmVignetteParams::ParseKey` (`:969-1035`).
    fn parse_param_key(
        &mut self,
        cx: &Cx<'_>,
        all_groups: &mut Option<Vec<u32>>,
        reader: &mut IniReader,
    ) {
        let p = &mut self.params;
        if reader.is_value(b"weight") {
            p.weight = reader.value_float(0) as f32;
        } else if reader.is_value(b"offer_text") {
            let mut text = OfferText::default();
            text.parse(reader);
            p.offer_texts.push(text);
        } else if reader.is_value(b"objective_text") {
            // `FRmObjectiveText::Parse`: a repeated objective parses into
            // the existing text. An unknown name keys the text with the
            // object's address in the C++; the port skips it (D872).
            if let Some(key) = lookup(&OBJECTIVE_NAMES, &reader.value_string_at(0)) {
                p.objective_texts.entry(key).or_default().parse(reader, 1);
            }
        } else if reader.is_value(b"failure_text") {
            p.failure_text.parse(reader, 0);
        } else if reader.is_value(b"reward_text") {
            p.reward_text.parse(reader, 0);
        } else if reader.is_value(b"difficulty") {
            p.difficulty = DifficultyWindow {
                min: reader.value_float(0) as f32,
                max: reader.value_float(1) as f32,
            };
        } else if reader.is_value(b"offer_group") {
            parse_groups(cx, all_groups, reader, &mut p.offer_groups);
        } else if reader.is_value(b"hostile_group") {
            parse_groups(cx, all_groups, reader, &mut p.hostile_groups);
        } else if reader.is_value(b"comm_sequence") {
            p.comm_sequences.push(CommSequence::parse(reader));
        } else if reader.is_value(b"allowable_zone_types") {
            p.allowable_zone_types = 0;
            let mut index = 0;
            while !reader.is_value_empty(index) {
                let word = reader.value_string_at(index);
                if eq_ignore_case(&word, b"open") {
                    p.allowable_zone_types |= 1;
                } else if eq_ignore_case(&word, b"field") {
                    p.allowable_zone_types |= 2;
                } else if eq_ignore_case(&word, b"exclusion") {
                    p.allowable_zone_types |= 4;
                }
                index += 1;
            }
        }
    }
}

/// `ParseRmReputationGroups` (`RandomMissionGen.cpp:943-965`): value 0
/// `all` assigns every reputation group (enumerated once into
/// `GRmAllGroups`, each group once in enumeration order); otherwise each
/// value's group is added once.
fn parse_groups(
    cx: &Cx<'_>,
    all_groups: &mut Option<Vec<u32>>,
    reader: &IniReader,
    groups: &mut Vec<u32>,
) {
    if eq_ignore_case(&reader.value_string_at(0), b"all") {
        let all = all_groups.get_or_insert_with(|| {
            let mut list = Vec::new();
            api::reputation::enumerate_groups(cx, |group| {
                add_unique(&mut list, group);
                true
            });
            list
        });
        groups.clone_from(all);
        return;
    }
    let mut index = 0;
    while !reader.is_value_empty(index) {
        add_unique(groups, reputation_group(cx, &reader.value_string_at(index)));
        index += 1;
    }
}

/// The generator root's tree (`FRandomMissionGenerator`).
#[derive(Clone, Debug, PartialEq)]
pub struct VignetteTree {
    /// `bLoaded`: set at the end of `Load`.
    pub loaded: bool,
    /// `bLinked`: set at the end of `Link`.
    pub linked: bool,
    /// `RootNodeId` (-1 until linked).
    pub root: u32,
    /// The implemented nodes without children, in key order.
    pub leaves: Vec<u32>,
    /// The nodes by id.
    pub nodes: BTreeMap<u32, VignetteNode>,
    /// `GRmAllGroups`: the groups of `all`, once enumerated.
    pub all_groups: Option<Vec<u32>>,
}

impl Default for VignetteTree {
    fn default() -> Self {
        Self {
            loaded: false,
            linked: false,
            root: u32::MAX,
            leaves: Vec::new(),
            nodes: BTreeMap::new(),
            all_groups: None,
        }
    }
}

impl VignetteTree {
    /// `FRandomMissionGenerator::Load` (`RandomMissionGen.cpp:410-424`):
    /// every header adds a node (a header that is not a node's adds a
    /// default node, keyed -1 unless another was), unique by id; then
    /// `Link`.
    pub fn load(&mut self, cx: &Cx<'_>, reader: &mut IniReader) {
        while reader.read_header() {
            let mut node = VignetteNode::default();
            if reader.is_header(b"DecisionNode")
                || reader.is_header(b"DocumentationNode")
                || reader.is_header(b"DataNode")
            {
                node.load(cx, &mut self.all_groups, reader);
            }
            self.nodes.entry(node.node_id).or_insert(node);
        }
        self.link();
        self.loaded = true;
    }

    /// `Link` (`:436-489`).
    fn link(&mut self) {
        if self.linked {
            return;
        }
        // 1. Every child id gets its parent's node id; a child without a
        // node gets a default one (`operator[]`).
        let keys: Vec<u32> = self.nodes.keys().copied().collect();
        for key in keys {
            let (parent_id, children) = {
                let n = &self.nodes[&key];
                (n.node_id, n.children.clone())
            };
            for child in children {
                self.nodes.entry(child).or_default().parents.push(parent_id);
            }
        }
        // 2. The smallest key, then the first parents up to a node with
        // none.
        let Some(&first) = self.nodes.keys().next() else {
            return;
        };
        self.root = first;
        while let Some(&parent) = self.nodes.get(&self.root).and_then(|n| n.parents.first()) {
            self.root = parent;
        }
        // 3. Below an unimplemented node, every implemented child and its
        // subtree is disabled, in key order.
        let keys: Vec<u32> = self.nodes.keys().copied().collect();
        for key in keys {
            let (implemented, children) = {
                let n = &self.nodes[&key];
                (n.implemented, n.children.clone())
            };
            if implemented {
                continue;
            }
            for child in children {
                self.disable(child);
            }
        }
        // 4. The implemented leaves.
        self.leaves = self
            .nodes
            .values()
            .filter(|n| n.implemented && n.children.is_empty())
            .map(|n| n.node_id)
            .collect();
        // 5.
        self.propagate_windows(self.root);
        self.linked = true;
    }

    /// The disable step of `Link` and `DisableChildren` (`:493-504`): an
    /// implemented node is disabled with its children, recursively.
    fn disable(&mut self, id: u32) {
        let children = match self.nodes.get_mut(&id) {
            Some(n) if n.implemented => {
                n.implemented = false;
                n.children.clone()
            }
            _ => return,
        };
        for child in children {
            self.disable(child);
        }
    }

    /// `PropagateDifficultyWindows` (`:513-559`): a node without windows
    /// takes its own difficulty; each child gets, once each, every window of
    /// this node shifted by the child's difficulty (or the child's own when
    /// this node has none), and the recursion goes into the child's node id.
    fn propagate_windows(&mut self, id: u32) {
        let Some(node) = self.nodes.get_mut(&id) else {
            return;
        };
        if node.windows.is_empty() {
            let own = node.params.difficulty;
            node.windows.push(own);
        }
        let windows = node.windows.clone();
        let children = node.children.clone();
        for child_key in children {
            let Some(child) = self.nodes.get_mut(&child_key) else {
                continue;
            };
            let mut shifted = windows.clone();
            if shifted.is_empty() {
                add_unique_window(&mut shifted, child.params.difficulty);
            } else {
                for w in &mut shifted {
                    w.add(&child.params.difficulty);
                }
            }
            for w in shifted {
                add_unique_window(&mut child.windows, w);
            }
            let next = child.node_id;
            self.propagate_windows(next);
        }
    }

    /// The node a path's bit leads to from `node`: the first child for 1,
    /// the second for any other. A missing second child is read past the
    /// vector in the C++; the port stops the walk there (D873).
    #[must_use]
    pub fn step(&self, node: &VignetteNode, bit: u8) -> Option<&VignetteNode> {
        let child = if bit == 1 {
            node.children.first()
        } else {
            node.children.get(1)
        }?;
        self.nodes.get(child)
    }
}

/// One decision path from the root (`FRmTreePath`): 1 for the first child,
/// 0 for the second. Saved as `tree_path = TFFT...`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TreePath {
    /// `+0x04`: one byte per decision.
    pub bits: Vec<u8>,
}

impl TreePath {
    /// `Save`'s text: `T` for a non-zero bit, `F` for zero.
    #[must_use]
    pub fn text(&self) -> String {
        self.bits
            .iter()
            .map(|&b| if b == 0 { 'F' } else { 'T' })
            .collect()
    }
}
