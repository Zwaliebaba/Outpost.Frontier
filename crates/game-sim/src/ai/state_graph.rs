//! The behaviour state graphs (`pub::StateGraph`,
//! `Common/common_pubstategraph.cpp`), the behaviour type names
//! (`BehaviorTypes`, `common_behaviortypes.cpp`) and the weighted
//! behaviour choice the behaviour manager makes from a graph row
//! (`IBehaviorManager::UpdateBehaviorTick`, `common_behaviormanager.cpp`).
//!
//! `DATA\AI\state_graph.db` is text read with `fscanf`: `state_graph_count
//! N`, `behavior_count 21`, then per graph a name, a type token (`ESCORT`
//! anywhere in it makes an escort graph, anything else a leader graph)
//! and 21 × 21 transition weights, row by row: row `from`, column `to`.

use neuron_core::hash::stricmp;
use neuron_core::vfs::Host;

use super::world::AiRand;

/// The number of behaviours, and the rows and columns of a graph
/// (`kStateGraphBehaviors`).
pub const BEHAVIOR_COUNT: usize = 21;

/// A graph name keeps at most this many bytes (`kStateGraphNameChars - 1`).
pub const NAME_CHARS: usize = 63;

/// The path the game loads (`refresh_state_graph`).
pub const STATE_GRAPH_PATH: &[u8] = b"AI\\state_graph.db";

/// `pub::StateGraph::Type`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GraphType {
    /// `LEADER` (0).
    Leader = 0,
    /// `ESCORT` (1).
    Escort = 1,
}

impl GraphType {
    /// The reference's value.
    #[must_use]
    pub fn value(self) -> i32 {
        self as i32
    }
}

/// One graph (`IStateGraph`, 0x72c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct StateGraph {
    /// +0x00 the id: the graph's index in the file.
    pub id: i32,
    /// +0x04 the type.
    pub kind: GraphType,
    /// +0x08 the name, at most [`NAME_CHARS`] bytes.
    pub name: Vec<u8>,
    /// +0x48 the transition weights, `weights[from][to]`.
    pub weights: [[f32; BEHAVIOR_COUNT]; BEHAVIOR_COUNT],
}

/// Why a load stopped (the reference's `FDUMP` errors).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LoadError {
    /// The file is missing ("unable to load state graph definitions").
    Missing,
    /// `behavior_count` is not 21 ("state graph definitions don't sync
    /// with current behavior list").
    BehaviorCount(i32),
}

/// The loaded graphs (the module's `GStateGraphs` array).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct StateGraphDb {
    /// The graphs in file order.
    pub graphs: Vec<StateGraph>,
}

/// The token scanner `fscanf` is over the file: whitespace is skipped
/// before each conversion; a conversion that does not match consumes
/// nothing more and leaves its target as it was.
struct Scanner<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl Scanner<'_> {
    fn skip_space(&mut self) {
        while self.pos < self.bytes.len() && self.bytes[self.pos].is_ascii_whitespace() {
            self.pos += 1;
        }
    }

    /// `%Ns` (`width` bytes at most); `None` at the end of the file.
    fn token(&mut self, width: usize) -> Option<&[u8]> {
        self.skip_space();
        let start = self.pos;
        while self.pos < self.bytes.len()
            && !self.bytes[self.pos].is_ascii_whitespace()
            && self.pos - start < width
        {
            self.pos += 1;
        }
        (self.pos > start).then(|| &self.bytes[start..self.pos])
    }

    /// The length of the longest prefix at `pos` that `pattern` accepts.
    fn digits_at(&self, mut at: usize) -> usize {
        let start = at;
        while at < self.bytes.len() && self.bytes[at].is_ascii_digit() {
            at += 1;
        }
        at - start
    }

    /// `%d`, wrapping like the CRT's conversion of a long digit run.
    fn int(&mut self) -> Option<i32> {
        self.skip_space();
        let mut at = self.pos;
        let negative = match self.bytes.get(at) {
            Some(b'-') => {
                at += 1;
                true
            }
            Some(b'+') => {
                at += 1;
                false
            }
            _ => false,
        };
        let count = self.digits_at(at);
        if count == 0 {
            return None;
        }
        let mut value = 0_i32;
        for &b in &self.bytes[at..at + count] {
            value = value.wrapping_mul(10).wrapping_add(i32::from(b - b'0'));
        }
        self.pos = at + count;
        Some(if negative {
            value.wrapping_neg()
        } else {
            value
        })
    }

    /// `%f`: sign, digits, a point, digits and an exponent that has
    /// digits, correctly rounded to `f32` as the Universal CRT does.
    fn float(&mut self) -> Option<f32> {
        self.skip_space();
        let start = self.pos;
        let mut at = start;
        if matches!(self.bytes.get(at), Some(b'-' | b'+')) {
            at += 1;
        }
        let int_digits = self.digits_at(at);
        at += int_digits;
        let mut frac_digits = 0;
        if self.bytes.get(at) == Some(&b'.') {
            frac_digits = self.digits_at(at + 1);
            at += 1 + frac_digits;
        }
        if int_digits + frac_digits == 0 {
            return None;
        }
        if matches!(self.bytes.get(at), Some(b'e' | b'E')) {
            let mut exp = at + 1;
            if matches!(self.bytes.get(exp), Some(b'-' | b'+')) {
                exp += 1;
            }
            let exp_digits = self.digits_at(exp);
            if exp_digits > 0 {
                at = exp + exp_digits;
            }
        }
        self.pos = at;
        let text = std::str::from_utf8(&self.bytes[start..at]).ok()?;
        let text = text.strip_suffix('.').unwrap_or(text);
        text.parse::<f32>().ok()
    }
}

impl StateGraphDb {
    /// `pub::StateGraph::LoadFromFile` over the file's bytes (`None` when
    /// the file is missing). After a failed load the reference keeps its
    /// previous graph count with the array freed; the port answers an
    /// error and the caller keeps an empty database (D192).
    pub fn load(bytes: Option<&[u8]>) -> Result<Self, LoadError> {
        let bytes = bytes.ok_or(LoadError::Missing)?;
        let mut scan = Scanner { bytes, pos: 0 };
        let _ = scan.token(usize::MAX);
        let graph_count = scan.int().unwrap_or(0);
        let _ = scan.token(usize::MAX);
        let behavior_count = scan.int().unwrap_or(0);
        if usize::try_from(behavior_count).ok() != Some(BEHAVIOR_COUNT) {
            return Err(LoadError::BehaviorCount(behavior_count));
        }
        let mut graphs = Vec::new();
        for index in 0..graph_count.max(0) {
            let name = scan.token(255).map(<[u8]>::to_vec).unwrap_or_default();
            let type_token = scan.token(255).map(<[u8]>::to_vec).unwrap_or_default();
            let kind = if type_token.windows(6).any(|w| w == b"ESCORT") {
                GraphType::Escort
            } else {
                GraphType::Leader
            };
            let mut weights = [[0.0_f32; BEHAVIOR_COUNT]; BEHAVIOR_COUNT];
            for row in &mut weights {
                for cell in row.iter_mut() {
                    if let Some(value) = scan.float() {
                        *cell = value;
                    }
                }
            }
            graphs.push(StateGraph {
                id: index,
                kind,
                name: name.into_iter().take(NAME_CHARS).collect(),
                weights,
            });
        }
        Ok(Self { graphs })
    }

    /// `refresh_state_graph`: [`STATE_GRAPH_PATH`] under the data folder.
    pub fn load_host(data: &Host) -> Result<Self, LoadError> {
        let file = data.read_file(STATE_GRAPH_PATH).ok();
        Self::load(file.as_ref().map(|f| f.data.as_slice()))
    }

    /// `get_state_graph(name, type)`: the id of the first graph with that
    /// name (case-insensitive) and type, -1 when there is none.
    #[must_use]
    pub fn find(&self, name: &[u8], kind: GraphType) -> i32 {
        self.graphs
            .iter()
            .find(|g| stricmp(&g.name, name).is_eq() && g.kind == kind)
            .map_or(-1, |g| g.id)
    }

    /// `get_state_graph(id, type)`: the graph with the name of graph `id`
    /// and the given type. The reference does not check `id` and reads
    /// past the array for one out of range; that answers -1 here (D190).
    #[must_use]
    pub fn find_by_id(&self, id: i32, kind: GraphType) -> i32 {
        let Some(graph) = usize::try_from(id).ok().and_then(|i| self.graphs.get(i)) else {
            return -1;
        };
        self.find(&graph.name, kind)
    }

    /// `get_state_graph_internal(id)`: the first graph whose id is `id`.
    #[must_use]
    pub fn get(&self, id: i32) -> Option<&StateGraph> {
        self.graphs.iter().find(|g| g.id == id)
    }
}

/// The behaviour type names in id order (`BehaviorTypes`' registry).
pub const BEHAVIOR_NAMES: [&str; BEHAVIOR_COUNT] = [
    "NULL",
    "Buzz",
    "Goto",
    "Trail",
    "Flee",
    "Evade",
    "Idle",
    "Dock",
    "Launch",
    "InstantTradeLane",
    "Formation",
    "Large Ship (move)",
    "Cruise",
    "Strafe",
    "Guide",
    "Face",
    "Loot",
    "Follow",
    "DrasticEvade",
    "FreeFlight",
    "Delay",
];

/// `BehaviorTypes::get_behavior_id`: case-insensitive, -1 when unknown.
#[must_use]
pub fn behavior_id(name: &[u8]) -> i32 {
    BEHAVIOR_NAMES
        .iter()
        .position(|n| stricmp(n.as_bytes(), name).is_eq())
        .and_then(|i| i32::try_from(i).ok())
        .unwrap_or(-1)
}

/// `BehaviorTypes::get_behavior_name`: `BEHAVIOR_ID_NONE` for -1. The
/// reference indexes its registry without a range check; any other id out
/// of range answers `None` here (D190).
#[must_use]
pub fn behavior_name(id: i32) -> Option<&'static str> {
    if id == -1 {
        return Some("BEHAVIOR_ID_NONE");
    }
    usize::try_from(id)
        .ok()
        .and_then(|i| BEHAVIOR_NAMES.get(i))
        .copied()
}

/// One entry of the behaviour manager's score table
/// (`BehaviorScores`, 21 entries of `{float Weight; int BehaviorIndex}`).
/// The table lives in the manager and keeps entries from earlier
/// evaluations past the current count, which the draw can read.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BehaviorScore {
    /// The weight, then the prefix sum.
    pub weight: f32,
    /// The behaviour.
    pub behavior: i32,
}

/// A candidate behaviour's own answer to the manager's poll
/// (`Behavior::Slot5`): 0 is "certainly me", 2 to 4 "certainly not".
pub type Verdict = i32;

/// What the re-evaluation decided.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BehaviorChoice {
    /// No candidate scored: the manager retires an armed directive and
    /// falls back to its default behaviour.
    NoneEligible,
    /// The last candidate that scored exactly 1 (no draw).
    Certain(i32),
    /// The weighted draw's choice. The reference stops with `INT3` when
    /// the chosen entry is -1 and reads the dword after the table when no
    /// prefix sum exceeds the draw; that dword is reported as `None`
    /// here (D191).
    Drawn(Option<i32>),
}

/// The behaviour re-evaluation of `UpdateBehaviorTick`: scores the 21
/// candidates from the graph row of `current` and the candidates'
/// verdicts, then picks one. `armed_directive` is the directive's
/// behaviour when the directive is armed (`DirDwords[0] == 1`); that
/// behaviour's weight becomes 0.001. `poll(index)` is asked only for
/// candidates with a non-zero weight, in index order. Draws one random
/// value when there is something to draw from and nothing scored 1.
pub fn choose_behavior(
    graph: &StateGraph,
    current: usize,
    armed_directive: Option<i32>,
    scores: &mut [BehaviorScore; BEHAVIOR_COUNT],
    mut poll: impl FnMut(usize) -> Verdict,
    rand: &mut impl AiRand,
) -> BehaviorChoice {
    let row = graph
        .weights
        .get(current)
        .copied()
        .unwrap_or([0.0; BEHAVIOR_COUNT]);
    let mut count = 0;
    let mut unity = false;
    for (index, &graph_weight) in row.iter().enumerate() {
        let index_i32 = i32::try_from(index).unwrap_or(-1);
        let weight = if armed_directive == Some(index_i32) {
            0.001
        } else {
            graph_weight
        };
        if f64::from(weight) == 0.0 {
            continue;
        }
        let value = match poll(index) {
            0 => 1.0,
            2..=4 => 0.0,
            _ => weight,
        };
        if !(f64::from(value) > 0.0) {
            continue;
        }
        scores[count] = BehaviorScore {
            weight: value,
            behavior: index_i32,
        };
        if f64::from(value) == 1.0 {
            unity = true;
        }
        count += 1;
    }
    if count == 0 {
        return BehaviorChoice::NoneEligible;
    }
    if unity {
        let chosen = scores[..count]
            .iter()
            .rev()
            .find(|s| f64::from(s.weight) == 1.0)
            .map_or(-1, |s| s.behavior);
        return BehaviorChoice::Certain(chosen);
    }
    let mut total = 0.0_f64;
    for score in &mut scores[..count] {
        let own = f64::from(score.weight);
        score.weight = (total + own) as f32;
        total += own;
    }
    let total_f = total as f32;
    let unit = f64::from(rand.rand()) * f64::from(1.0_f32 / 32768.0);
    let pick = ((f64::from(total_f) - f64::from(1.0e-5_f32)) * unit + 1.0e-5) as f32;
    let picked = scores.iter().position(|s| s.weight > pick);
    BehaviorChoice::Drawn(picked.map(|i| scores[i].behavior))
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::rng::CrtRand;

    fn file(graphs: &[(&str, &str, f32)]) -> Vec<u8> {
        let mut text = format!("state_graph_count {}\nbehavior_count 21\n", graphs.len());
        for (name, kind, fill) in graphs {
            text.push_str(&format!("\n{name}\n{kind}\n"));
            for row in 0..21 {
                for col in 0..21 {
                    let v = if row == col { *fill } else { 0.0 };
                    text.push_str(&format!("{v:.2} "));
                }
                text.push('\n');
            }
        }
        text.into_bytes()
    }

    #[test]
    fn loads_and_finds() {
        let bytes = file(&[("FIGHTER", "LEADER", 0.5), ("FIGHTER", "ESCORT", 0.25)]);
        let db = StateGraphDb::load(Some(&bytes)).unwrap_or_default();
        assert_eq!(db.graphs.len(), 2);
        assert_eq!(db.graphs[1].kind, GraphType::Escort);
        assert_eq!(db.graphs[1].weights[3][3], 0.25);
        assert_eq!(db.find(b"fighter", GraphType::Escort), 1);
        assert_eq!(db.find(b"FIGHTER", GraphType::Leader), 0);
        assert_eq!(db.find(b"GUNBOAT", GraphType::Leader), -1);
        assert_eq!(db.find_by_id(1, GraphType::Leader), 0);
        assert_eq!(db.find_by_id(7, GraphType::Leader), -1);
        assert_eq!(db.get(1).map(|g| g.id), Some(1));
    }

    #[test]
    fn rejects_other_behavior_counts() {
        assert_eq!(StateGraphDb::load(None), Err(LoadError::Missing));
        let bytes = b"state_graph_count 1\nbehavior_count 20\n".to_vec();
        assert_eq!(
            StateGraphDb::load(Some(&bytes)),
            Err(LoadError::BehaviorCount(20))
        );
    }

    #[test]
    fn short_file_leaves_zeros() {
        let bytes = b"state_graph_count 2\nbehavior_count 21\nA LEADER 1.5 2".to_vec();
        let db = StateGraphDb::load(Some(&bytes)).unwrap_or_default();
        assert_eq!(db.graphs.len(), 2);
        assert_eq!(db.graphs[0].weights[0][..3], [1.5, 2.0, 0.0]);
        assert_eq!(db.graphs[1].name, b"");
        assert_eq!(db.graphs[1].id, 1);
    }

    #[test]
    fn names() {
        assert_eq!(behavior_id(b"dock"), 7);
        assert_eq!(behavior_id(b"nope"), -1);
        assert_eq!(behavior_name(-1), Some("BEHAVIOR_ID_NONE"));
        assert_eq!(behavior_name(20), Some("Delay"));
        assert_eq!(behavior_name(21), None);
    }

    #[test]
    fn choice() {
        let mut graph = StateGraph {
            id: 0,
            kind: GraphType::Leader,
            name: b"G".to_vec(),
            weights: [[0.0; 21]; 21],
        };
        graph.weights[2][5] = 0.5;
        graph.weights[2][6] = 0.5;
        let mut scores = [BehaviorScore::default(); 21];
        let mut rand = CrtRand::with_seed(1);
        let certain = choose_behavior(
            &graph,
            2,
            None,
            &mut scores,
            |i| if i == 6 { 0 } else { 1 },
            &mut rand,
        );
        assert_eq!(certain, BehaviorChoice::Certain(6));
        let drawn = choose_behavior(&graph, 2, None, &mut scores, |_| 1, &mut rand);
        assert!(
            matches!(drawn, BehaviorChoice::Drawn(Some(5 | 6))),
            "{drawn:?}"
        );
        let none = choose_behavior(&graph, 0, None, &mut scores, |_| 1, &mut rand);
        assert_eq!(none, BehaviorChoice::NoneEligible);
        let armed = choose_behavior(&graph, 0, Some(9), &mut scores, |_| 1, &mut rand);
        assert_eq!(armed, BehaviorChoice::Drawn(Some(9)));
    }
}
