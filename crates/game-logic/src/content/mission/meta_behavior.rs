//! The `[MetaBehavior]` objects (TU37, `Content/Guides.h:158-231`,
//! `Guides.cpp:156-245`; work package P3.5a2): the descriptions of the goto
//! and dock guides (types 5 and 6) that `Act_GiveMB` and `init_metabehavior`
//! hand to an NPC ship. `LoadMetaBehavior` makes one per `MB_GOTOGUIDE` or
//! `MB_DOCKGUIDE` line, keeps the whole line and parses the values. The
//! parse draws no `rand()`; the dock guide takes a `pub::GetNicknameId`.
//! The guides themselves are P3.6c's.

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;

use super::field::{Field, record};
use super::{f32_at, nickname_id, tstring, value_tstring};

record! {
    /// `FMbGotoGuide`'s members (`FMbDockGuide` derives from it). Its ship
    /// (+0x0c) is set only while `Act_GiveMB` hands it over, and its leg
    /// list (+0x118) stays empty: neither is kept.
    MbGuide {
        /// `Line` (+0x10, `TString<0x100>`): `get_line_ptr()`.
        line: Vec<u8> = Vec::new(),
        /// +0x114: the goto guide's value 9, `NORMAL` 1, `ALWAYS_EXECUTE` 0
        /// (0).
        priority: i32 = 0,
        /// +0x124: values 1 to 3 (0; the C++ constructor leaves it unset
        /// and the reconstruction zeroes it).
        first_position: Vector = Vector::ZERO,
        /// +0x130: the goto guide's values 4 to 6 (0, as `first_position`).
        destination: Vector = Vector::ZERO,
        /// +0x13c: the goto guide's int value 7, the dock guide's float
        /// value 5.
        range: f32 = 0.0,
        /// +0x140, value 0 (`TString<0x30>`).
        target_name: Vec<u8> = Vec::new(),
        /// +0x174 (0).
        field_0x174: u8 = 0,
        /// +0x175 (0).
        field_0x175: u8 = 0,
        /// +0x176, the goto guide's bool value 8 (1).
        field_0x176: u8 = 1,
    }
}

/// One `[MetaBehavior]` object.
#[derive(Clone, Debug, PartialEq)]
pub enum MetaBehavior {
    /// `MB_GOTOGUIDE` (type 5).
    GotoGuide(MbGuide),
    /// `MB_DOCKGUIDE` (type 6) and its dock target (+0x178, value 4).
    DockGuide(MbGuide, u32),
}

impl MetaBehavior {
    /// `Type` (+0x08): 5 or 6.
    #[must_use]
    pub fn type_code(&self) -> i32 {
        match self {
            Self::GotoGuide(_) => 5,
            Self::DockGuide(..) => 6,
        }
    }

    /// The key of the line that made it.
    #[must_use]
    pub fn key(&self) -> &'static str {
        match self {
            Self::GotoGuide(_) => "MB_GOTOGUIDE",
            Self::DockGuide(..) => "MB_DOCKGUIDE",
        }
    }

    /// The guide's members.
    #[must_use]
    pub fn guide(&self) -> &MbGuide {
        match self {
            Self::GotoGuide(g) | Self::DockGuide(g, _) => g,
        }
    }

    /// The fields in the oracle dump's order: the goto guide's, then the
    /// dock guide's target.
    #[must_use]
    pub fn fields(&self) -> Vec<(&'static str, Field)> {
        let mut out = self.guide().fields();
        if let Self::DockGuide(_, target) = self {
            out.push(("dock_target_id", Field::U32(*target)));
        }
        out
    }
}

/// `_stricmp(get_value_string(index), text) == 0`.
fn value_is(reader: &IniReader, index: u32, text: &[u8]) -> bool {
    eq_ignore_case(c_str(&reader.value_string_at(index)), text)
}

/// `FMbGotoGuide::Parse` (`Guides.cpp:183-203`): the floats are read z,
/// y, x as the C++ reads them.
fn parse_goto(reader: &mut IniReader, g: &mut MbGuide) {
    g.target_name = value_tstring(reader, 0, 0x30);
    g.first_position.z = f32_at(reader, 3);
    g.first_position.y = f32_at(reader, 2);
    g.first_position.x = f32_at(reader, 1);
    g.destination.z = f32_at(reader, 6);
    g.destination.y = f32_at(reader, 5);
    g.destination.x = f32_at(reader, 4);
    g.range = reader.value_int(7) as f32;
    if !reader.is_value_empty(8) {
        g.field_0x176 = u8::from(reader.value_bool(8));
    }
    if reader.is_value_empty(9) {
        return;
    }
    if value_is(reader, 9, b"NORMAL") {
        g.priority = 1;
    } else if value_is(reader, 9, b"ALWAYS_EXECUTE") {
        g.priority = 0;
    }
}

/// `FMbDockGuide::Parse` (`Guides.cpp:207-218`): the destination is not
/// written.
fn parse_dock(reader: &mut IniReader, g: &mut MbGuide) -> u32 {
    g.target_name = value_tstring(reader, 0, 0x30);
    let y = f32_at(reader, 2);
    let x = f32_at(reader, 1);
    let z = f32_at(reader, 3);
    g.first_position = Vector::new(x, y, z);
    let target = nickname_id(&reader.value_string_at(4));
    g.range = f32_at(reader, 5);
    target
}

/// `LoadMetaBehavior` (`Guides.cpp:225-262`): one `[MetaBehavior]`
/// section into `list`. A `nickname` sets `nickname` to `CreateID` of the
/// whole value (it is not reset first: the caller starts it at 0);
/// `system` is skipped; `MB_GOTOGUIDE` and `MB_DOCKGUIDE` append a new
/// object with the whole line, parsed; other keys are skipped.
pub fn load(reader: &mut IniReader, nickname: &mut u32) -> Vec<MetaBehavior> {
    let mut list = Vec::new();
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
            continue;
        }
        if reader.is_value(b"system") {
            continue;
        }
        let dock = reader.is_value(b"MB_DOCKGUIDE");
        if !dock && !reader.is_value(b"MB_GOTOGUIDE") {
            continue;
        }
        let mut guide = MbGuide {
            line: tstring(&reader.line(), 0x100),
            ..MbGuide::default()
        };
        list.push(if dock {
            let target = parse_dock(reader, &mut guide);
            MetaBehavior::DockGuide(guide, target)
        } else {
            parse_goto(reader, &mut guide);
            MetaBehavior::GotoGuide(guide)
        });
    }
    list
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parsed(text: &str) -> (u32, Vec<MetaBehavior>) {
        let mut reader = IniReader::new();
        reader.open_memory(text.as_bytes());
        assert!(reader.read_header());
        let mut nickname = 0;
        let list = load(&mut reader, &mut nickname);
        (nickname, list)
    }

    #[test]
    fn a_goto_guide_reads_its_ten_values() {
        let (nickname, list) = parsed(
            "[MetaBehavior]\nnickname = escort\nsystem = li01\n\
             MB_GotoGuide = Trent, 1, 2, 3, 4, 5, 6, 750, false, NORMAL\nMB_Other = 1\n",
        );
        assert_eq!(nickname, create_id(b"escort"));
        assert_eq!(list.len(), 1);
        let g = list[0].guide();
        assert_eq!(list[0].type_code(), 5);
        assert_eq!(g.target_name, b"Trent");
        assert_eq!(g.first_position, Vector::new(1.0, 2.0, 3.0));
        assert_eq!(g.destination, Vector::new(4.0, 5.0, 6.0));
        assert_eq!((g.range, g.field_0x176, g.priority), (750.0, 0, 1));
        assert_eq!(
            g.line,
            b"MB_GotoGuide = Trent, 1, 2, 3, 4, 5, 6, 750, false, NORMAL"
        );
    }

    #[test]
    fn a_dock_guide_has_a_target_and_no_destination() {
        let (nickname, list) =
            parsed("[MetaBehavior]\nMB_DOCKGUIDE = Trent, 1, 2, 3, Li01_01_Base, 12.5\n");
        assert_eq!(nickname, 0);
        let MetaBehavior::DockGuide(g, target) = &list[0] else {
            panic!("a dock guide");
        };
        assert_eq!(*target, create_id(b"Li01_01_Base"));
        assert_eq!(g.destination, Vector::ZERO);
        assert_eq!((g.range, g.field_0x176), (12.5, 1));
        assert_eq!(list[0].fields().last().unwrap().0, "dock_target_id");
    }
}
