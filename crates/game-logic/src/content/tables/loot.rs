//! The loot properties and phantom loot of `missions\lootprops.ini` (TU35,
//! `Content/LootProps.cpp:54-165`).

use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

/// The float factor both loaders multiply a percentage by.
const PERCENT: f32 = 0.01;

/// A percentage as the C++ stores it: the value times the float 0.01 in
/// double precision, stored as a float.
fn percent(value: f64) -> f32 {
    (value * f64::from(PERCENT)) as f32
}

/// One `[mLootProps]` (`FLootProps`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LootProps {
    /// `nickname`, `CreateID`.
    pub nickname: u32,
    /// `drop_properties` value 0 times 0.01.
    pub drop_chance: f32,
    /// `drop_properties` values 1 to 5. Their readers name them (P3.6).
    pub drop_properties: [i32; 5],
}

/// `LoadLootProps`: one record per `[mLootProps]`, in file order.
pub fn load_loot_props(table: &mut Vec<LootProps>, reader: &mut IniReader) {
    while reader.read_header() {
        if !reader.is_header(b"mLootProps") {
            continue;
        }
        let mut record = LootProps::default();
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                record.nickname = create_id(&reader.value_string());
            } else if reader.is_value(b"drop_properties") {
                record.drop_chance = percent(reader.value_float(0));
                record.drop_properties = [1, 2, 3, 4, 5].map(|i| reader.value_int(i));
            }
        }
        table.push(record);
    }
}

/// `FindLootProps` (and the `default_commodity` and `default_equipment`
/// lookups): the first record of a nickname id.
#[must_use]
pub fn find_loot_props(table: &[LootProps], nickname: u32) -> Option<&LootProps> {
    table.iter().find(|l| l.nickname == nickname)
}

/// One `[PhantomLoot]` (`FPhantomLoot`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PhantomLoot {
    /// `nickname`, `CreateID`.
    pub nickname: u32,
    /// `toughness_range` value 0.
    pub toughness_min: i32,
    /// `toughness_range` value 1, -1 read as `i32::MAX`.
    pub toughness_max: i32,
    /// `percent_chance` times 0.01.
    pub percent_chance: f32,
    /// `num_to_drop` value 0.
    pub num_to_drop_min: i32,
    /// `num_to_drop` value 1.
    pub num_to_drop_max: i32,
}

/// `LoadPhantomLoot`: one record per `[PhantomLoot]`, in file order.
pub fn load_phantom_loot(table: &mut Vec<PhantomLoot>, reader: &mut IniReader) {
    while reader.read_header() {
        if !reader.is_header(b"PhantomLoot") {
            continue;
        }
        let mut record = PhantomLoot::default();
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                record.nickname = create_id(&reader.value_string());
            } else if reader.is_value(b"toughness_range") {
                record.toughness_min = reader.value_int(0);
                record.toughness_max = match reader.value_int(1) {
                    -1 => i32::MAX,
                    max => max,
                };
            } else if reader.is_value(b"percent_chance") {
                record.percent_chance = percent(reader.value_float(0));
            } else if reader.is_value(b"num_to_drop") {
                record.num_to_drop_min = reader.value_int(0);
                record.num_to_drop_max = reader.value_int(1);
            }
        }
        table.push(record);
    }
}

/// `CollectPhantomLoot`: every record whose toughness range holds
/// `toughness`, in file order.
pub fn collect_phantom_loot(
    table: &[PhantomLoot],
    toughness: i32,
) -> impl Iterator<Item = &PhantomLoot> {
    table
        .iter()
        .filter(move |p| p.toughness_min <= toughness && toughness <= p.toughness_max)
}
