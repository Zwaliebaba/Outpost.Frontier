//! The player toughness scale of `missions\PTough.ini` (`Content/Support.cpp:135-194`)
//! and the rank difficulties of `missions\RankDiff.ini` (TU47,
//! `RandomMissionDB.cpp:113-181`).

use game_sim::data::vc6::vc6_sort;
use neuron_core::ini::IniReader;

use super::stage_value;

/// One `ptough_graph_pt` (`FPlayerToughPt`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ToughnessPoint {
    /// The experience (worth), never below the point before it (unsigned).
    pub experience: i32,
    /// The rank, 1 to 40 from the file; the index for an extrapolated
    /// point.
    pub toughness: i32,
}

/// Where the multiplayer extrapolation stops.
const EXPERIENCE_LIMIT: i32 = 999_999_999;

/// `LoadPlayerToughnessScale`: the first `[PlayerToughnessScale]`'s points;
/// on a multiplayer server the scale is then extrapolated by an eighth per
/// point up to [`EXPERIENCE_LIMIT`], each new point's rank its index.
///
/// The C++ loops for ever when the last experience is between -7 and 7, whose
/// eighth is 0; the port stops (D784).
pub fn load_toughness_scale(
    table: &mut Vec<ToughnessPoint>,
    reader: &mut IniReader,
    single_player: bool,
) {
    let mut max_experience = 0_i32;
    while reader.read_header() {
        if !reader.is_header(b"PlayerToughnessScale") {
            continue;
        }
        while reader.read_value() {
            if !reader.is_value(b"ptough_graph_pt") {
                continue;
            }
            let mut experience = reader.value_int(0);
            let toughness = reader.value_int(1).clamp(1, 0x28);
            if (experience as u32) < (max_experience as u32) {
                experience = max_experience;
            }
            max_experience = experience;
            table.push(ToughnessPoint {
                experience,
                toughness,
            });
        }
        break;
    }
    if single_player {
        return;
    }
    while max_experience < EXPERIENCE_LIMIT {
        let step = max_experience / 8;
        if step == 0 {
            break;
        }
        max_experience = max_experience.wrapping_add(step);
        table.push(ToughnessPoint {
            experience: max_experience,
            toughness: table.len() as i32,
        });
    }
}

/// One `rank_diff` (`FRmRankDifficulty`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RankDifficulty {
    /// The story stage (the stage table's value).
    pub stage: i32,
    /// The difficulty, clamped to 0 to 100.
    pub difficulty: f32,
}

/// `LoadRankDifficulties`: every `[RankDiffDB]`'s `rank_diff = <stage>,
/// <difficulty>`, sorted by stage with the VC6 `std::sort`.
///
/// A stage name missing from the table reuses the line before's stage. The
/// C++'s first line would reuse the reader pointer's bits; the port starts
/// at 0 (D781).
pub fn load_rank_difficulties(table: &mut Vec<RankDifficulty>, reader: &mut IniReader) {
    let mut last_stage = 0;
    while reader.read_header() {
        if !reader.is_header(b"RankDiffDB") {
            continue;
        }
        while reader.read_value() {
            if !reader.is_value(b"rank_diff") {
                continue;
            }
            stage_value(&reader.value_string_at(0), &mut last_stage);
            let value = reader.value_float(1) as f32;
            let floor = if value < 0.0 { 0.0 } else { value };
            // `!(floor < 100)`: an unordered value gives 100 too.
            let difficulty = if floor >= 100.0 || floor.is_nan() {
                100.0
            } else if value < 0.0 {
                0.0
            } else {
                value
            };
            table.push(RankDifficulty {
                stage: last_stage,
                difficulty,
            });
        }
    }
    vc6_sort(table, |a, b| a.stage < b.stage);
}
