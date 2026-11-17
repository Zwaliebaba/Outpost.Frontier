//! Body parts, accessories and costumes (`CostumeDescriptions`), ported
//! from `CppSource/Common/common_costume.cpp`.
//!
//! `[Data] bodyparts` reads `bodyparts.ini` (`load_bodyparts`): skeleton
//! sexes, animation name lists, petal animation lists, detail switch tables,
//! accessories and one map of body parts per type (body, head, left hand,
//! right hand). `[Data] costumes` reads `costumes.ini` (`load_costumes`),
//! whose records point at the parts and accessories loaded before. The C++
//! creates the one `CostumeDescriptions` on first use and keeps it; here
//! [`GameData`](super::config::GameData) owns it.
//!
//! Records point at each other through the C++ heap: a body part at the
//! detail switch table and the petal animation list in force when it was
//! read, a costume at its parts and accessories. Here a body part holds the
//! indices of that table and list, and a costume the ids of its parts and
//! accessories; neither map ever replaces a record, so an id names the
//! record the C++ pointer names. None of these loaders interns strings.
//! `DetailSwitchTable::adjust_distance` (x87 tangents, the client's camera)
//! is not ported.

use std::collections::BTreeMap;

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::msvcrt::atoi;

use super::{DataSource, Reports, lossy, open_ini, value_f32};

const SOURCE: &str = "Common\\CharDescrip.cpp";

/// The body-part types, in table order (`kBodypartTypeNames`).
pub const BODYPART_TYPES: [&[u8]; 4] = [b"Body", b"Head", b"LeftHand", b"RightHand"];

/// At most this many accessories per costume: the C++ reference drops
/// the rest (the original wrote past its array).
pub const MAX_ACCESSORIES: usize = 8;

/// `atol(name)`, or `CreateID(name)` when that is 0 (`NicknameToCreateId`).
#[must_use]
pub fn nickname_id(name: &[u8]) -> u32 {
    match atoi(name) {
        0 => create_id(name),
        parsed => u32::from_ne_bytes(parsed.to_ne_bytes()),
    }
}

/// The type index of a header or key name, case-insensitive
/// (`BodypartTypeFromName`).
#[must_use]
pub fn bodypart_type(name: &[u8]) -> Option<usize> {
    BODYPART_TYPES.iter().position(|t| eq_ignore_case(t, name))
}

/// A `[DetailSwitchTable]` (`DetailSwitchTable`).
#[derive(Clone, Debug, PartialEq)]
pub struct DetailSwitchTable {
    /// The `switch` pairs (key, value) in file order.
    pub table: Vec<(f32, f32)>,
    /// Half of `fovx` (20 by default).
    pub fov_x_half: f32,
    /// `sizex` (1280 by default).
    pub size_x: f32,
}

impl Default for DetailSwitchTable {
    fn default() -> Self {
        Self {
            table: Vec::new(),
            fov_x_half: 20.0,
            size_x: 1280.0,
        }
    }
}

impl DetailSwitchTable {
    /// `read_from_ini`: the values of the section.
    pub fn read(&mut self, reader: &mut IniReader) {
        while reader.read_value() {
            if reader.is_value(b"switch") {
                let key = value_f32(reader, 0);
                let value = value_f32(reader, 1);
                self.table.push((key, value));
            } else if reader.is_value(b"fovx") {
                self.fov_x_half = value_f32(reader, 0) * 0.5;
            } else if reader.is_value(b"sizex") {
                self.size_x = value_f32(reader, 0);
            }
        }
    }

    /// `get_switch_distance`: the value for `x`, interpolated between the
    /// keys (written in descending order) and returned unrounded; `None`
    /// where the C++ reads the first entry of an empty table.
    #[must_use]
    pub fn switch_distance(&self, x: f32) -> Option<f64> {
        let first = self.table.first()?;
        let last = self.table.last()?;
        let x = f64::from(x);
        if x >= f64::from(first.0) {
            return Some(f64::from(first.1));
        }
        if x <= f64::from(last.0) {
            return Some(f64::from(last.1));
        }
        for pair in self.table.windows(2) {
            let (a, b) = (f64::from(pair[0].0), f64::from(pair[0].1));
            let (d, c) = (f64::from(pair[1].0), f64::from(pair[1].1));
            if a >= x && d <= x {
                return Some(((x - a) * (c - b)) / (d - a) + b);
            }
        }
        Some(1.0)
    }
}

/// A body part (`bodypart`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bodypart {
    /// [`nickname_id`] of the nickname.
    pub id: u32,
    /// `mesh`.
    pub mesh: Vec<u8>,
    /// The sex of the last `[Skeleton]` (0 none, 1 male, 2 female).
    pub sex: i32,
    /// The detail switch table read last before the part, by index.
    pub switch: Option<usize>,
    /// The petal animation list read last before the part, by index.
    pub petal_animations: Option<usize>,
}

/// An `[Accessory]` (`accessory`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Accessory {
    /// [`nickname_id`] of the nickname (of the empty string without one).
    pub id: u32,
    /// `mesh`.
    pub mesh: Vec<u8>,
    /// `hardpoint`.
    pub accessory_hardpoint: Vec<u8>,
    /// `body_hardpoint`.
    pub character_hardpoint: Vec<u8>,
    /// Every `material_library`, in order.
    pub materials: Vec<Vec<u8>>,
}

/// A `[Costume]` (`costume`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CostumeRecord {
    /// [`nickname_id`] of the nickname (0 without one).
    pub id: u32,
    /// The first non-zero sex of its parts.
    pub gender: i32,
    /// The part of each type, by id in that type's map.
    pub parts: [Option<u32>; 4],
    /// The accessories, by id, at most [`MAX_ACCESSORIES`].
    pub accessories: Vec<u32>,
}

/// The `Costume` a costume fills (`get_costume`): head, body, left hand,
/// right hand, then the accessories.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Costume {
    /// The part ids, head first (0 for none).
    pub part_ids: [u32; 4],
    /// The accessory ids.
    pub accessory_ids: Vec<u32>,
}

/// `CostumeDescriptions`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CostumeDescriptions {
    /// `[Animations] anim`, without repeats (case-insensitive).
    pub animation_names: Vec<Vec<u8>>,
    /// `[Animations] preload`, without repeats.
    pub preload_names: Vec<Vec<u8>>,
    /// One list per `[PetalAnimations]`.
    pub petal_animations: Vec<Vec<Vec<u8>>>,
    /// One table per `[DetailSwitchTable]`.
    pub detail_switch_tables: Vec<DetailSwitchTable>,
    /// The body parts of each type, by id.
    pub bodyparts: [BTreeMap<u32, Bodypart>; 4],
    /// The accessories, by id.
    pub accessories: BTreeMap<u32, Accessory>,
    /// The costumes, by id.
    pub costumes: BTreeMap<u32, CostumeRecord>,
}

fn push_unique(list: &mut Vec<Vec<u8>>, name: Vec<u8>) {
    if !list.iter().any(|entry| eq_ignore_case(&name, entry)) {
        list.push(name);
    }
}

impl CostumeDescriptions {
    /// `load_bodyparts`.
    pub fn load_bodyparts(&mut self, source: &dyn DataSource, path: &[u8], reports: &mut Reports) {
        let Some(mut reader) = open_ini(source, path) else {
            reports.push(
                SOURCE,
                0x3b0,
                format!("*** ERROR: Cannot open bodyparts ini file: {}", lossy(path)),
            );
            return;
        };
        let mut sex = 0;
        let mut switch = None;
        while reader.read_header() {
            if reader.is_header(b"Skeleton") {
                sex = 0;
                while reader.read_value() {
                    if reader.is_value(b"nomotion") {
                        reports.push(SOURCE, 0x33b, "*** WARNING: nomotion is obsolete");
                    } else if reader.is_value(b"sex") {
                        let text = reader.value_string();
                        if eq_ignore_case(&text, b"none") {
                            sex = 0;
                        } else if eq_ignore_case(&text, b"male") {
                            sex = 1;
                        } else if eq_ignore_case(&text, b"female") {
                            sex = 2;
                        }
                    }
                }
            } else if reader.is_header(b"Animations") {
                while reader.read_value() {
                    if reader.is_value(b"anim") {
                        push_unique(&mut self.animation_names, reader.value_string());
                    } else if reader.is_value(b"preload") {
                        push_unique(&mut self.preload_names, reader.value_string());
                    }
                }
            } else if reader.is_header(b"PetalAnimations") {
                let mut names = Vec::new();
                while reader.read_value() {
                    if reader.is_value(b"anim") {
                        push_unique(&mut names, reader.value_string());
                    }
                }
                self.petal_animations.push(names);
            } else if reader.is_header(b"DetailSwitchTable") {
                let mut table = DetailSwitchTable::default();
                table.read(&mut reader);
                self.detail_switch_tables.push(table);
                switch = Some(self.detail_switch_tables.len() - 1);
            } else if reader.is_header(b"Accessory") {
                self.load_accessory(&mut reader);
            } else if let Some(kind) = bodypart_type(&reader.header()) {
                self.load_bodypart(kind, sex, switch, &mut reader, reports);
            } else {
                reports.push(
                    SOURCE,
                    0x3a8,
                    format!(
                        "*** ERROR: Unknown header {} in bodyparts.ini",
                        lossy(&reader.header())
                    ),
                );
            }
        }
    }

    /// `load_bodypart`: a part needs a nickname and a mesh; an id already in
    /// its type's map keeps the first record.
    fn load_bodypart(
        &mut self,
        kind: usize,
        sex: i32,
        switch: Option<usize>,
        reader: &mut IniReader,
        reports: &mut Reports,
    ) {
        let mut nickname = Vec::new();
        let mut mesh = Vec::new();
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                nickname = reader.value_string();
            } else if reader.is_value(b"mesh") {
                mesh = reader.value_string();
            } else if reader.is_value(b"multilevel") {
                reports.push(SOURCE, 0x2c9, "*** WARNING: multilevel is obsolete");
            }
        }
        if nickname.is_empty() || mesh.is_empty() {
            return;
        }
        let id = nickname_id(&nickname);
        let petal_animations = self.petal_animations.len().checked_sub(1);
        self.bodyparts[kind].entry(id).or_insert(Bodypart {
            id,
            mesh,
            sex,
            switch,
            petal_animations,
        });
    }

    /// `load_accessory`: an accessory without a nickname goes in under the
    /// id of the empty string.
    fn load_accessory(&mut self, reader: &mut IniReader) {
        let mut accessory = Accessory::default();
        let mut nickname = Vec::new();
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                nickname = reader.value_string();
            } else if reader.is_value(b"mesh") {
                accessory.mesh = reader.value_string();
            } else if reader.is_value(b"hardpoint") {
                accessory.accessory_hardpoint = reader.value_string();
            } else if reader.is_value(b"body_hardpoint") {
                accessory.character_hardpoint = reader.value_string();
            } else if reader.is_value(b"material_library") {
                accessory.materials.push(reader.value_string());
            }
        }
        accessory.id = nickname_id(&nickname);
        self.accessories.entry(accessory.id).or_insert(accessory);
    }

    /// `load_costumes`. The warnings print an empty costume name, as the
    /// C++ clears its name buffer before every value line.
    pub fn load_costumes(&mut self, source: &dyn DataSource, path: &[u8], reports: &mut Reports) {
        let Some(mut reader) = open_ini(source, path) else {
            reports.push(
                SOURCE,
                0x415,
                format!("*** ERROR: Cannot open costume ini file: {}", lossy(path)),
            );
            return;
        };
        while reader.read_header() {
            if !reader.is_header(b"Costume") {
                reports.push(
                    SOURCE,
                    0x40e,
                    format!(
                        "*** WARNING: Unknown header {} in costumes.ini",
                        lossy(&reader.header())
                    ),
                );
                continue;
            }
            let mut record = CostumeRecord::default();
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    record.id = nickname_id(&reader.value_string());
                } else if reader.is_value(b"accessory") {
                    let name = reader.value_string();
                    let id = nickname_id(&name);
                    if self.accessories.contains_key(&id) {
                        if record.accessories.len() < MAX_ACCESSORIES {
                            record.accessories.push(id);
                        }
                    } else {
                        reports.push(
                            SOURCE,
                            0x3e4,
                            format!(
                                "*** WARNING: Cannot find accessory {} for costume ",
                                lossy(&name)
                            ),
                        );
                    }
                } else {
                    let key = reader.name();
                    let Some(kind) = bodypart_type(&key) else {
                        reports.push(
                            SOURCE,
                            0x404,
                            format!(
                                "*** WARNING: Invalid part type {} for costume ",
                                lossy(&key)
                            ),
                        );
                        continue;
                    };
                    let name = reader.value_string();
                    match self.bodyparts[kind].get(&nickname_id(&name)) {
                        Some(part) => {
                            record.parts[kind] = Some(part.id);
                            if part.sex != 0 && record.gender == 0 {
                                record.gender = part.sex;
                            }
                        }
                        None => reports.push(
                            SOURCE,
                            0x3fc,
                            format!(
                                "*** WARNING: Cannot find bodypart {} (type {}) for costume ",
                                lossy(&name),
                                lossy(&key)
                            ),
                        ),
                    }
                }
            }
            self.costumes.entry(record.id).or_insert(record);
        }
    }

    /// `find_bodypart(type, id)`.
    #[must_use]
    pub fn find_bodypart(&self, kind: usize, id: u32) -> Option<&Bodypart> {
        self.bodyparts.get(kind)?.get(&id)
    }

    /// `find_costume(id)`.
    #[must_use]
    pub fn find_costume(&self, id: u32) -> Option<&CostumeRecord> {
        self.costumes.get(&id)
    }

    /// `get_costume(id, out)`: the costume's ids, head first; a missing
    /// costume is reported and gives no parts and no accessories (the C++
    /// leaves the caller's accessory slots as they were).
    pub fn get_costume(&self, id: u32, reports: &mut Reports) -> Costume {
        let Some(record) = self.find_costume(id) else {
            reports.push(
                SOURCE,
                0x27b,
                format!("*** ERROR: Costume id {id:x} does not exist"),
            );
            return Costume::default();
        };
        let order = [1, 0, 2, 3];
        Costume {
            part_ids: order.map(|kind| record.parts[kind].unwrap_or(0)),
            accessory_ids: record.accessories.clone(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Files(Vec<(&'static [u8], &'static [u8])>);

    impl DataSource for Files {
        fn read(&self, path: &[u8]) -> Option<Vec<u8>> {
            self.0
                .iter()
                .find(|(name, _)| *name == path)
                .map(|(_, data)| data.to_vec())
        }
    }

    #[test]
    fn parts_tables_and_costumes() {
        let files = Files(vec![
            (
                b"b.ini",
                b"[Skeleton]\nsex = female\n[DetailSwitchTable]\nswitch = 100, 10\nswitch = 10, 20\n\
                  fovx = 50\n[PetalAnimations]\nanim = a\nanim = A\nanim = b\n[Body]\nnickname = b1\n\
                  mesh = m1\n[Body]\nnickname = b1\nmesh = other\n[Head]\nnickname = 7\nmesh = h\n\
                  [Accessory]\nnickname = acc\nmaterial_library = x\nmaterial_library = y\n[Wing]\n",
            ),
            (
                b"c.ini",
                b"[Costume]\nnickname = c1\nbody = b1\nhead = 7\naccessory = acc\naccessory = no\n\
                  tail = b1\n[Other]\n",
            ),
        ]);
        let mut reports = Reports::default();
        let mut db = CostumeDescriptions::default();
        db.load_bodyparts(&files, b"b.ini", &mut reports);
        db.load_costumes(&files, b"c.ini", &mut reports);
        let body = &db.bodyparts[0][&create_id(b"b1")];
        assert_eq!(body.mesh, b"m1");
        assert_eq!(body.sex, 2);
        assert_eq!(body.switch, Some(0));
        assert_eq!(body.petal_animations, Some(0));
        assert_eq!(db.petal_animations[0], [b"a".to_vec(), b"b".to_vec()]);
        assert!(db.bodyparts[1].contains_key(&7));
        assert_eq!(db.detail_switch_tables[0].fov_x_half, 25.0);
        assert_eq!(db.detail_switch_tables[0].switch_distance(55.0), Some(15.0));
        assert_eq!(db.accessories[&create_id(b"acc")].materials.len(), 2);
        let costume = db.get_costume(create_id(b"c1"), &mut reports);
        assert_eq!(costume.part_ids, [7, create_id(b"b1"), 0, 0]);
        assert_eq!(costume.accessory_ids, [create_id(b"acc")]);
        assert_eq!(db.costumes[&create_id(b"c1")].gender, 2);
        // The unknown header, the missing accessory, the unknown key and
        // the other header.
        assert_eq!(reports.0.len(), 4);
    }
}
