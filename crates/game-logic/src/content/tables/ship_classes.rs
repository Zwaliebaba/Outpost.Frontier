//! The ship classes of `missions\shipclasses.ini` and the mission
//! properties of `ships\shiparch.ini` (TU51, `Content/RandomMissionDB.cpp:680-830`).

use std::collections::BTreeMap;

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::ship_id;

/// `AppendUnique`.
fn append_unique(values: &mut Vec<u32>, value: u32) {
    if !values.contains(&value) {
        values.push(value);
    }
}

/// `LoadShipClasses`: each `[ShipClass]`'s members, a member that names an
/// earlier class expanded to its members, each id once; an existing class
/// keeps its first definition.
pub fn load_ship_classes(table: &mut BTreeMap<u32, Vec<u32>>, reader: &mut IniReader) {
    while reader.read_header() {
        if !reader.is_header(b"ShipClass") {
            continue;
        }
        let mut members = Vec::new();
        let mut class = 0;
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                class = create_id(&reader.value_string());
            } else if reader.is_value(b"member") {
                let member = create_id(&reader.value_string());
                match table.get(&member) {
                    Some(nested) => {
                        for &id in nested {
                            append_unique(&mut members, id);
                        }
                    }
                    None => append_unique(&mut members, member),
                }
            }
        }
        table.entry(class).or_insert(members);
    }
}

/// `FindShipClassByMembers`: the first class, in key order, whose members
/// are these in this order.
#[must_use]
pub fn find_class_by_members(table: &BTreeMap<u32, Vec<u32>>, members: &[u32]) -> Option<u32> {
    table
        .iter()
        .find(|(_, m)| m.as_slice() == members)
        .map(|(&id, _)| id)
}

/// `LoadShipMissionProperties`: each `[Ship]`'s `mission_property` ids under
/// its ship id. The C++ reads the file once per process (a static flag);
/// the port reads it once per table set, which is the same for the one
/// load GameSupport makes.
pub fn load_ship_mission_properties(table: &mut BTreeMap<u32, Vec<u32>>, reader: &mut IniReader) {
    let berths = create_id(b"can_use_berths");
    let medium = create_id(b"can_use_medium_moors");
    let large = create_id(b"can_use_large_moors");
    while reader.read_header() {
        if !reader.is_header(b"Ship") {
            continue;
        }
        let mut properties = Vec::new();
        let mut ship = 0;
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                ship_id(&mut ship, &reader.value_string());
            } else if reader.is_value(b"mission_property") {
                let value = reader.value_string();
                if eq_ignore_case(&value, b"can_use_berths") {
                    append_unique(&mut properties, berths);
                } else if eq_ignore_case(&value, b"can_use_med_moors") {
                    append_unique(&mut properties, medium);
                } else if eq_ignore_case(&value, b"can_use_large_moors") {
                    append_unique(&mut properties, large);
                }
            }
        }
        table.entry(ship).or_insert(properties);
    }
}
