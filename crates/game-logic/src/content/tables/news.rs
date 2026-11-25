//! The news items of `missions\news.ini` (TU19, `Content/BaseSupport.cpp:36-190`).

use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::ini::IniReader;

use super::{base_id, stage_value};
use crate::server::Cx;

/// One `[NewsItem]` (`FNewsItem`, with the parts of its
/// `NewsBroadcastDescription` the loader writes).
///
/// The C++ field names do not follow the keys: `category` goes to the
/// description's `Headline`, `headline` to its `Text`, `text` to its
/// `Audio`; the fields here are named after the keys.
#[derive(Clone, Debug, PartialEq)]
pub struct NewsItem {
    /// `base`: each base id once, in file order (0 for an unknown base).
    pub bases: Vec<u32>,
    /// `rank` value 0's stage.
    pub from_stage: i32,
    /// `rank` value 1's stage.
    pub to_stage: i32,
    /// `audio` (a `StringAlloc` string, kept as the file spells it, D785).
    pub audio: Vec<u8>,
    /// `logo` (as `audio`).
    pub logo: Vec<u8>,
    /// Whether an `audio` line set the string: the C++'s pointer is null
    /// without one, which `CreateID` maps to 0 where an empty string is not.
    pub has_audio: bool,
    /// Whether a `logo` line set the string (the texture is null without
    /// one).
    pub has_logo: bool,
    /// `breaking` (`bFlag1`).
    pub breaking: bool,
    /// `autoselect` (`bFlag2`).
    pub autoselect: bool,
    /// `icon` (`Category`): critical 1, world 2, mission 3, system 4,
    /// faction 5, universe 6.
    pub icon: u32,
    /// `category`: a mad-lib id (`Headline`).
    pub category: FmtStr,
    /// `headline`: a mad-lib id (`Text`).
    pub headline: FmtStr,
    /// `text`: a mad-lib id and string ids (`Audio`).
    pub text: FmtStr,
}

impl Default for NewsItem {
    /// `FNewsItem::FNewsItem`: stages 0 to 0x2a.
    fn default() -> Self {
        Self {
            bases: Vec::new(),
            from_stage: 0,
            to_stage: 0x2a,
            audio: Vec::new(),
            logo: Vec::new(),
            has_audio: false,
            has_logo: false,
            breaking: false,
            autoselect: false,
            icon: 0,
            category: FmtStr::default(),
            headline: FmtStr::default(),
            text: FmtStr::default(),
        }
    }
}

/// `LoadNewsItems`: one item per `[NewsItem]`, in file order.
///
/// The stage both `rank` lookups share outlives the values of a section; a
/// name missing from the table reuses the stage before (the C++ leaves it
/// unset before the first; the port starts at 0, D781).
pub fn load_news_items(cx: &mut Cx<'_>, table: &mut Vec<NewsItem>, reader: &mut IniReader) {
    while reader.read_header() {
        if !reader.is_header(b"NewsItem") {
            continue;
        }
        let mut item = NewsItem::default();
        let mut stage = 0;
        while reader.read_value() {
            if reader.is_value(b"base") {
                let base = base_id(cx, &reader.value_string());
                if !item.bases.contains(&base) {
                    item.bases.push(base);
                }
            } else if reader.is_value(b"rank") {
                stage_value(&reader.value_string_at(0), &mut stage);
                item.from_stage = stage;
                stage_value(&reader.value_string_at(1), &mut stage);
                item.to_stage = stage;
            } else if reader.is_value(b"breaking") {
                item.breaking = true;
            } else if reader.is_value(b"autoselect") {
                item.autoselect = true;
            } else if reader.is_value(b"icon") {
                let value = reader.value_string();
                let icons: [&[u8]; 6] = [
                    b"critical",
                    b"world",
                    b"mission",
                    b"system",
                    b"faction",
                    b"universe",
                ];
                if let Some(index) = icons.iter().position(|n| eq_ignore_case(&value, n)) {
                    item.icon = index as u32 + 1;
                }
            } else if reader.is_value(b"category") {
                item.category.mad_lib_id = reader.value_int(0) as u32;
            } else if reader.is_value(b"headline") {
                item.headline.mad_lib_id = reader.value_int(0) as u32;
            } else if reader.is_value(b"text") {
                item.text.mad_lib_id = reader.value_int(0) as u32;
                let mut index = 1;
                while !reader.is_value_empty(index) {
                    item.text.append_string(reader.value_int(index) as u32);
                    index += 1;
                }
            } else if reader.is_value(b"logo") {
                item.logo = c_str(&reader.value_string()).to_vec();
                item.has_logo = true;
            } else if reader.is_value(b"audio") {
                item.audio = c_str(&reader.value_string()).to_vec();
                item.has_audio = true;
            }
        }
        table.push(item);
    }
}
