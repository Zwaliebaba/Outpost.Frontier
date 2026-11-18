//! The material library (`FMaterialLibrary`, the `IMaterialLibrary` slots
//! of `CppSource/Shading/MaterialLibrary.cpp`): type remapping by map
//! rules, the master map of materials by name CRC with its own reference
//! counts, numbered groups of map entries, and creation through the
//! component manager, here a [`MaterialFactory`].
//!
//! A map entry (`FCrcSlot`) holds a count of its own and one material
//! reference; a group lists name CRCs, which may repeat or name nothing.
//! The counts follow the reference exactly, including its quirks: removing
//! a material from a group whose entry count is 2 also drops the entry
//! from the map (keeping the material's reference, a leak), deleting a
//! group drops every entry whose count reaches 1, and storing a CRC that
//! is taken replaces the entry without releasing its material. Where the
//! reference dereferences a missing entry or a material it already
//! cleared, the port leaves things as they are (divergence D302).
//!
//! The reference's `RefCount` is unsigned and compared as signed
//! (`(int)RefCount <= 1`); so is [`MapEntry::refs`].

use std::collections::BTreeMap;

use neuron_core::diag::{self, DumpFlags, Severity};
use neuron_core::hash::{eq_ignore_case, name_crc32};
use neuron_core::ini::EngineSection;
use neuron_core::vfs::{FileSystem, FsDesc};

use super::batcher::BatchRule;
use super::instance::{Material, MaterialFactory, MaterialId, Materials};
use super::regex::{MaterialRegex, search};
use super::{GR_GENERIC, GR_OK, GR_UNSUPPORTED};
use crate::render::texture::format_class::read_line_into;

const SOURCE: &str = "Shading\\Materials\\MaterialLibrary.cpp";

/// Names and types are copied into 64-byte buffers: 63 bytes are kept.
pub const NAME_LIMIT: usize = 0x3F;

/// The `[MaterialMap]` and `[BatchedMaterials]` lines are read into
/// 1024-byte buffers.
pub const LINE_BUFFER: usize = 0x400;

/// A map rule (`FMaterialMapRule`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MapRule {
    /// 0 matches the material name, 1 its type; other keys are kept but
    /// never match.
    pub key: i32,
    /// `key == 0`.
    pub name_pattern: bool,
    /// `None` when the pattern did not compile; such a rule never matches.
    pub expression: Option<MaterialRegex>,
    /// The pattern as given.
    pub pattern: Vec<u8>,
    /// The type it maps to, at most 63 bytes.
    pub type_name: Vec<u8>,
}

/// An entry of the master map (`FCrcSlot`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MapEntry {
    /// The entry's own count (`RefCount`).
    pub refs: u32,
    /// The material; `None` after `RemoveMaterial(0, ...)` cleared it
    /// while the count stayed above 1.
    pub material: Option<MaterialId>,
}

impl MapEntry {
    /// `(int)RefCount`.
    #[must_use]
    pub fn signed_refs(&self) -> i32 {
        self.refs as i32
    }
}

/// A group (`FMaterialGroup`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MaterialGroup {
    /// The group's id.
    pub id: u32,
    /// `None` until named.
    pub name: Option<Vec<u8>>,
    /// Name CRCs of map entries, in insertion order.
    pub members: Vec<u32>,
}

/// The result of [`MaterialLibrary::load_library`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LibraryLoad {
    /// `GR_OK`, or `GR_GENERIC` when `Material library` did not open.
    pub result: i32,
    /// The new group's id, `0xFFFFFFFF` when the node did not open; `None`
    /// when no group was asked for.
    pub group: Option<u32>,
}

fn truncated(text: &[u8], limit: usize) -> Vec<u8> {
    let text = c_str(text);
    text[..text.len().min(limit)].to_vec()
}

fn c_str(text: &[u8]) -> &[u8] {
    neuron_core::hash::c_str(text)
}

/// The material library.
pub struct MaterialLibrary<S: ?Sized> {
    rules: Vec<MapRule>,
    map: BTreeMap<u32, MapEntry>,
    groups: BTreeMap<u32, MaterialGroup>,
    next_group_id: u32,
    materials: Materials<S>,
    factory: Box<dyn MaterialFactory<S>>,
    /// The batching rules of the batcher that owns the library
    /// (`OwnerBatcher->RuleList`); `None` for a library of its own.
    pub(super) batch_rules: Option<Vec<BatchRule>>,
}

impl<S: ?Sized> std::fmt::Debug for MaterialLibrary<S> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MaterialLibrary")
            .field("rules", &self.rules)
            .field("map", &self.map)
            .field("groups", &self.groups)
            .field("next_group_id", &self.next_group_id)
            .finish_non_exhaustive()
    }
}

impl<S: ?Sized> MaterialLibrary<S> {
    /// A library with no rules, materials or groups (the constructor).
    #[must_use]
    pub fn new(factory: Box<dyn MaterialFactory<S>>) -> MaterialLibrary<S> {
        MaterialLibrary {
            rules: Vec::new(),
            map: BTreeMap::new(),
            groups: BTreeMap::new(),
            next_group_id: 1,
            materials: Materials::new(),
            factory,
            batch_rules: None,
        }
    }

    /// The map rules, the most recently added first (the order they are
    /// tried in).
    #[must_use]
    pub fn rules(&self) -> &[MapRule] {
        &self.rules
    }

    /// The master map by name CRC.
    #[must_use]
    pub fn map(&self) -> &BTreeMap<u32, MapEntry> {
        &self.map
    }

    /// The groups by id.
    #[must_use]
    pub fn groups(&self) -> &BTreeMap<u32, MaterialGroup> {
        &self.groups
    }

    /// The materials.
    #[must_use]
    pub fn materials(&self) -> &Materials<S> {
        &self.materials
    }

    /// The materials, mutably: callers release the references the library
    /// hands them here.
    pub fn materials_mut(&mut self) -> &mut Materials<S> {
        &mut self.materials
    }

    /// The factory.
    pub fn factory_mut(&mut self) -> &mut dyn MaterialFactory<S> {
        &mut *self.factory
    }

    /// The map entry of a name CRC.
    #[must_use]
    pub fn entry(&self, crc: u32) -> Option<&MapEntry> {
        self.map.get(&crc)
    }

    // --- map rules ---------------------------------------------------------------------------

    /// `AddMapRule` (slot 3): a rule tried before every earlier one. The
    /// type keeps 63 bytes; a pattern that does not compile is kept and
    /// never matches.
    pub fn add_map_rule(&mut self, key: i32, pattern: &[u8], type_name: &[u8]) -> i32 {
        let pattern = c_str(pattern);
        let type_name = truncated(type_name, NAME_LIMIT);
        let message = format!(
            "Adding material map: {}:{}:{}",
            if key == 1 { "Type" } else { "Name" },
            String::from_utf8_lossy(pattern),
            String::from_utf8_lossy(c_str(&type_name)),
        );
        diag::dump(
            DumpFlags::NOTICE,
            &diag::general_line(SOURCE, 0x362, Severity::Notice, &message),
        );
        self.rules.insert(
            0,
            MapRule {
                key,
                name_pattern: key == 0,
                expression: MaterialRegex::compile(pattern),
                pattern: pattern.to_vec(),
                type_name,
            },
        );
        GR_OK
    }

    /// `FindMapRule` (slot 4): the type of the first rule with this key and
    /// exactly this pattern.
    pub fn find_map_rule(&self, key: i32, pattern: &[u8]) -> Result<Vec<u8>, i32> {
        let pattern = c_str(pattern);
        self.rules
            .iter()
            .find(|r| r.key == key && r.pattern == pattern)
            .map(|r| r.type_name.clone())
            .ok_or(GR_GENERIC)
    }

    /// `RemapType` (slot 5): the type of the first name rule (key 0) whose
    /// pattern matches `name` or type rule (key 1) whose pattern matches
    /// `type_name`.
    pub fn remap_type(&self, name: &[u8], type_name: &[u8]) -> Result<Vec<u8>, i32> {
        for rule in &self.rules {
            if rule.key != 0 && rule.key != 1 {
                continue;
            }
            let candidate = if rule.name_pattern { name } else { type_name };
            if search(rule.expression.as_ref(), Some(candidate)) {
                return Ok(rule.type_name.clone());
            }
        }
        Err(GR_GENERIC)
    }

    /// `RemoveMapRule` (slot 6): removes the first rule with this key and
    /// exactly this pattern.
    pub fn remove_map_rule(&mut self, key: i32, pattern: &[u8]) -> i32 {
        let pattern = c_str(pattern);
        match self
            .rules
            .iter()
            .position(|r| r.key == key && r.pattern == pattern)
        {
            Some(index) => {
                self.rules.remove(index);
                GR_OK
            }
            None => GR_GENERIC,
        }
    }

    /// `ResolveType`: the type of the first name rule matching `name`, or
    /// rule of key 1 matching the type (the name when there is no type);
    /// else the type, or the name.
    #[must_use]
    pub fn resolve_type<'a>(&'a self, name: &'a [u8], type_name: Option<&'a [u8]>) -> &'a [u8] {
        let candidate = type_name.unwrap_or(name);
        for rule in &self.rules {
            if rule.name_pattern {
                if search(rule.expression.as_ref(), Some(name)) {
                    return &rule.type_name;
                }
            } else if rule.key == 1 && search(rule.expression.as_ref(), Some(candidate)) {
                return &rule.type_name;
            }
        }
        c_str(candidate)
    }

    /// `ParseMaterialMap`: one rule per `[MaterialMap]` line that holds an
    /// `=`. `name = pattern = type` is a name rule when the selector is
    /// `name` (any case), a type rule for any other selector; `pattern =
    /// type` is a type rule. Spaces, tabs, `;` and `=` around the parts are
    /// trimmed. Reading stops at the first empty line.
    pub fn parse_material_map(&mut self, section: &EngineSection) {
        let mut line = [0u8; LINE_BUFFER];
        let mut index = 0;
        while read_line_into(section, index, &mut line) {
            if let Some((mode, pattern, type_name)) = parse_map_line(&mut line) {
                self.add_map_rule(mode, &pattern, &type_name);
            }
            index += 1;
        }
    }

    // --- the master map ----------------------------------------------------------------------

    /// `StoreCrc`: the entry of `crc` holds `material` (taking a reference)
    /// with count 1. A material the entry held before is not released.
    fn store_crc(&mut self, crc: u32, material: Option<MaterialId>) {
        if let Some(id) = material {
            self.materials.add_ref(id);
        }
        self.map.insert(crc, MapEntry { refs: 1, material });
    }

    /// `FindMaterialByCrc` (slot 16): the material of a name CRC, with a
    /// reference for the caller. `GR_GENERIC` on a miss.
    pub fn find_material_by_crc(&mut self, crc: u32) -> Result<Option<MaterialId>, i32> {
        let material = self.map.get(&crc).ok_or(GR_GENERIC)?.material;
        if let Some(id) = material {
            self.materials.add_ref(id);
        }
        Ok(material)
    }

    /// `FindOrCreate` (slot 15): the material of `name`'s CRC, created with
    /// [`MaterialLibrary::create_after_map`] (remapping on) when there is
    /// none; with a reference for the caller.
    pub fn find_or_create(
        &mut self,
        services: &mut S,
        name: &[u8],
        type_name: &[u8],
    ) -> Result<Option<MaterialId>, i32> {
        let crc = name_crc32(c_str(name));
        let key = if self.map.contains_key(&crc) {
            crc
        } else {
            self.create_after_map(services, name, type_name, true)
                .ok_or(GR_GENERIC)?
        };
        let material = self.map.get(&key).and_then(|e| e.material);
        if let Some(id) = material {
            self.materials.add_ref(id);
        }
        Ok(material)
    }

    /// `AddMaterial` (slot 17): enters `material` under `crc` and, for a
    /// group other than 0, lists the CRC in the group (before anything
    /// else, so also when the call then fails). An existing entry fails
    /// for group 0 and gains a count and a material reference otherwise; a
    /// new entry starts with count 1, or 2 with a second material
    /// reference for a group.
    pub fn add_material(&mut self, group: i32, crc: u32, material: Option<MaterialId>) -> i32 {
        let material = material.filter(|&id| self.materials.contains(id));
        let in_group = group != 0;
        if in_group {
            let Some(entry) = self.groups.get_mut(&(group as u32)) else {
                return GR_GENERIC;
            };
            entry.members.push(crc);
        }
        if let Some(entry) = self.map.get_mut(&crc) {
            if !in_group {
                return GR_GENERIC;
            }
            entry.refs = entry.refs.wrapping_add(1);
            if let Some(id) = entry.material {
                self.materials.add_ref(id);
            }
            return GR_OK;
        }
        self.store_crc(crc, material);
        if in_group {
            if let Some(entry) = self.map.get_mut(&crc) {
                entry.refs = 2;
            }
            if let Some(id) = material {
                self.materials.add_ref(id);
            }
        }
        GR_OK
    }

    /// Drops one count and one material reference of an entry and clears
    /// its material (the group-0 removal).
    fn release_entry_material(&mut self, services: &mut S, crc: u32) {
        if let Some(entry) = self.map.get_mut(&crc) {
            entry.refs = entry.refs.wrapping_sub(1);
            if let Some(id) = entry.material.take() {
                self.materials.release(id, services);
            }
        }
    }

    /// `RemoveMaterial` (slot 18). For a group: the CRC's first listing is
    /// removed; when the entry's count is at most 1 it leaves the map (its
    /// material keeps the reference), else it loses a count and a material
    /// reference. For group 0: the entry loses a count and its material.
    /// Then, in every case, an entry whose count is at most 1 leaves the
    /// map. `GR_GENERIC` for a missing group, which still runs that last
    /// step.
    pub fn remove_material(&mut self, services: &mut S, group: u32, crc: u32) -> i32 {
        let mut result = GR_OK;
        let has_entry = self.map.contains_key(&crc);
        if group != 0 {
            match self.groups.get_mut(&group) {
                None => result = GR_GENERIC,
                Some(entry) => {
                    if let Some(index) = entry.members.iter().position(|&m| m == crc) {
                        entry.members.remove(index);
                        if let Some(slot) = self.map.get_mut(&crc) {
                            if slot.signed_refs() <= 1 {
                                self.map.remove(&crc);
                                return result;
                            }
                            slot.refs = slot.refs.wrapping_sub(1);
                            if let Some(id) = slot.material {
                                self.materials.release(id, services);
                            }
                        }
                    }
                }
            }
        } else if has_entry {
            self.release_entry_material(services, crc);
        }
        // The reference reads the entry again here even when there is none.
        if self.map.get(&crc).is_some_and(|e| e.signed_refs() <= 1) {
            self.map.remove(&crc);
        }
        result
    }

    // --- groups ------------------------------------------------------------------------------

    /// `AllocateGroup` (slot 7): a new empty, unnamed group.
    pub fn allocate_group(&mut self) -> u32 {
        let id = self.next_group_id;
        self.groups.insert(
            id,
            MaterialGroup {
                id,
                name: None,
                members: Vec::new(),
            },
        );
        self.next_group_id = self.next_group_id.wrapping_add(1);
        id
    }

    /// `UnsupportedOperation` (slot 9).
    #[must_use]
    pub fn unsupported_operation(&self) -> i32 {
        GR_UNSUPPORTED
    }

    /// `DeleteGroup` (slot 10). Group 0: every entry whose count is at most
    /// 1 loses its count and material and leaves the map. Another group:
    /// each listed entry with a count above 1 loses a count and a material
    /// reference, and then, if its count is at most 1, the rest of both and
    /// leaves the map; the group goes. `GR_GENERIC` for -1 or a missing
    /// group.
    pub fn delete_group(&mut self, services: &mut S, group: i32) -> i32 {
        if group == -1 {
            return GR_GENERIC;
        }
        if group == 0 {
            let doomed: Vec<u32> = self
                .map
                .iter()
                .filter(|(_, e)| e.signed_refs() <= 1)
                .map(|(&crc, _)| crc)
                .collect();
            for crc in doomed {
                self.release_entry_material(services, crc);
                self.map.remove(&crc);
            }
            return GR_OK;
        }
        let Some(entry) = self.groups.get(&(group as u32)) else {
            return GR_GENERIC;
        };
        let members = entry.members.clone();
        for crc in members {
            let Some(slot) = self.map.get_mut(&crc) else {
                continue;
            };
            if slot.signed_refs() > 1 {
                slot.refs = slot.refs.wrapping_sub(1);
                if let Some(id) = slot.material {
                    self.materials.release(id, services);
                }
            }
            if self.map.get(&crc).is_some_and(|e| e.signed_refs() <= 1) {
                self.release_entry_material(services, crc);
                self.map.remove(&crc);
            }
        }
        self.groups.remove(&(group as u32));
        GR_OK
    }

    /// The materials of a group's listed entries, or of the whole map for
    /// group 0, in order; `Err` for -1, `Ok(empty)` for a missing group.
    fn group_materials(&self, group: i32) -> Result<Vec<MaterialId>, i32> {
        if group == -1 {
            return Err(GR_GENERIC);
        }
        if group == 0 {
            return Ok(self.map.values().filter_map(|e| e.material).collect());
        }
        let Some(entry) = self.groups.get(&(group as u32)) else {
            return Ok(Vec::new());
        };
        Ok(entry
            .members
            .iter()
            .filter_map(|crc| self.map.get(crc))
            .filter_map(|e| e.material)
            .collect())
    }

    /// `ApplyVerifyGroup` (slot 11): `ApplyVerify` on every material of a
    /// group, or of the map for group 0. `GR_OK` also for a missing group,
    /// `GR_GENERIC` for -1.
    pub fn apply_verify_group(&mut self, services: &mut S, group: i32) -> i32 {
        match self.group_materials(group) {
            Err(result) => result,
            Ok(materials) => {
                for id in materials {
                    if let Some(material) = self.materials.get_mut(id) {
                        material.apply_verify(services);
                    }
                }
                GR_OK
            }
        }
    }

    /// `ResolveTexturesGroup` (slot 12): `ResolveTextures(delta)` on every
    /// material of a group, or of the map for group 0.
    pub fn resolve_textures_group(&mut self, services: &mut S, group: i32, delta: f32) -> i32 {
        match self.group_materials(group) {
            Err(result) => result,
            Ok(materials) => {
                for id in materials {
                    if let Some(material) = self.materials.get_mut(id) {
                        material.resolve_textures(services, delta);
                    }
                }
                GR_OK
            }
        }
    }

    /// `SetGroupName` (slot 13): names a group, or clears its name.
    /// `GR_GENERIC` for -1, 0 and a missing group.
    pub fn set_group_name(&mut self, group: i32, name: Option<&[u8]>) -> i32 {
        if group == -1 || group == 0 {
            return GR_GENERIC;
        }
        match self.groups.get_mut(&(group as u32)) {
            Some(entry) => {
                entry.name = name.map(|n| c_str(n).to_vec());
                GR_OK
            }
            None => GR_GENERIC,
        }
    }

    /// `GetGroupName` (slot 14): the name as a `size`-byte buffer holds it
    /// (at most `size - 1` bytes), empty for an unnamed group. A size of 0
    /// gives an empty name (the reference writes before its buffer).
    pub fn group_name(&self, group: i32, size: u32) -> Result<Vec<u8>, i32> {
        if group == -1 || group == 0 {
            return Err(GR_GENERIC);
        }
        let entry = self.groups.get(&(group as u32)).ok_or(GR_GENERIC)?;
        Ok(match &entry.name {
            Some(name) => {
                let keep = (size as usize).saturating_sub(1).min(name.len());
                name[..keep].to_vec()
            }
            None => Vec::new(),
        })
    }

    /// `GetGroupCount` (slot 19): the groups and the reserved group 0.
    #[must_use]
    pub fn group_count(&self) -> u32 {
        self.groups.len() as u32 + 1
    }

    /// `EnumGroup` (slot 20): 0 for index 0, then the groups by id.
    pub fn enum_group(&self, index: u32) -> Result<u32, i32> {
        if index >= self.group_count() {
            return Err(GR_GENERIC);
        }
        if index == 0 {
            return Ok(0);
        }
        self.groups
            .values()
            .nth(index as usize - 1)
            .map(|g| g.id)
            .ok_or(GR_GENERIC)
    }

    /// `GetGroupMaterialCount` (slot 21): the listings of a group, or the
    /// map entries for group 0.
    pub fn group_material_count(&self, group: u32) -> Result<i32, i32> {
        if group == u32::MAX {
            return Err(GR_GENERIC);
        }
        if group == 0 {
            return Ok(self.map.len() as i32);
        }
        self.groups
            .get(&group)
            .map(|g| g.members.len() as i32)
            .ok_or(GR_GENERIC)
    }

    /// `EnumGroupMaterial` (slot 22): the material of a group's listing, or
    /// of the map's entry for group 0, with a reference for the caller.
    pub fn enum_group_material(
        &mut self,
        group: u32,
        index: u32,
    ) -> Result<Option<MaterialId>, i32> {
        if group == u32::MAX {
            return Err(GR_GENERIC);
        }
        let material = if group == 0 {
            self.map
                .values()
                .nth(index as usize)
                .ok_or(GR_GENERIC)?
                .material
        } else {
            let entry = self.groups.get(&group).ok_or(GR_GENERIC)?;
            let crc = *entry.members.get(index as usize).ok_or(GR_GENERIC)?;
            self.map.get(&crc).ok_or(GR_GENERIC)?.material
        };
        if let Some(id) = material {
            self.materials.add_ref(id);
        }
        Ok(material)
    }

    // --- creation ----------------------------------------------------------------------------

    /// The name and type copies of creation: 63 bytes each, the name in
    /// lower case; the type remapped when asked and a rule matches.
    fn creation_names(
        &self,
        name: &[u8],
        type_name: &[u8],
        remap: bool,
    ) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
        let mut name_copy = truncated(name, NAME_LIMIT);
        name_copy.make_ascii_lowercase();
        let type_copy = truncated(type_name, NAME_LIMIT);
        let selected = if remap {
            self.remap_type(&name_copy, &type_copy)
                .unwrap_or_else(|_| type_copy.clone())
        } else {
            type_copy.clone()
        };
        (name_copy, type_copy, selected)
    }

    /// Initializes, names and enters a new material under `crc`; `None`
    /// (the material destroyed) when `Initialize` fails.
    fn enter_new(
        &mut self,
        services: &mut S,
        mut object: Box<dyn Material<S>>,
        crc: u32,
        name: &[u8],
    ) -> Option<u32> {
        if object.initialize(services) < 0 {
            object.destroy(services);
            return None;
        }
        object.set_name(name);
        let id = self.materials.insert(object, 0);
        self.store_crc(crc, Some(id));
        Some(crc)
    }

    /// `CreateGenericRecord`: a material of the (remapped) type, named with
    /// the lower-case name and entered under the CRC of `name` as given.
    /// The entry's CRC, `None` when the factory or `Initialize` fails.
    pub fn create_generic_record(
        &mut self,
        services: &mut S,
        name: &[u8],
        type_name: &[u8],
        remap: bool,
    ) -> Option<u32> {
        let crc = name_crc32(c_str(name));
        let (name_copy, type_copy, selected) = self.creation_names(name, type_name, remap);
        let object = match self.factory.create(services, &selected) {
            Ok(object) => object,
            Err(_) => {
                let message = format!(
                    "Unable to create material '{}' ('{}', '{}')\n",
                    String::from_utf8_lossy(&name_copy),
                    String::from_utf8_lossy(&type_copy),
                    String::from_utf8_lossy(&selected),
                );
                diag::dump(
                    DumpFlags::WARNING,
                    &diag::general_line(SOURCE, 0x14F, Severity::Warning, &message),
                );
                return None;
            }
        };
        self.enter_new(services, object, crc, &name_copy)
    }

    /// `CreateAfterMap` (slot 26). A library of its own creates with
    /// [`MaterialLibrary::create_generic_record`]. A batcher's library
    /// remaps the type first, then wraps the material in a
    /// `BatchedMaterial` when a batching rule matches (kind 1 the type,
    /// kind 2 the lower-case name); otherwise it creates the remapped type
    /// under the lower-case name (whose CRC differs from the name's when
    /// the name was longer than 63 bytes).
    pub fn create_after_map(
        &mut self,
        services: &mut S,
        name: &[u8],
        type_name: &[u8],
        remap: bool,
    ) -> Option<u32> {
        let Some(batch_rules) = self.batch_rules.as_ref() else {
            return self.create_generic_record(services, name, type_name, remap);
        };
        let crc = name_crc32(c_str(name));
        let (name_copy, type_copy, selected) = self.creation_names(name, type_name, remap);
        let wrap = batch_rules.iter().any(|rule| {
            let candidate = match rule.kind {
                1 => Some(selected.as_slice()),
                2 => Some(name_copy.as_slice()),
                _ => None,
            };
            search(rule.expression.as_ref(), candidate)
        });
        if !wrap {
            return self.create_generic_record(services, &name_copy, &selected, false);
        }
        let mut object = self.factory.create(services, b"BatchedMaterial").ok()?;
        if !object.is_batched() {
            object.destroy(services);
            return None;
        }
        if object.create_inner_material(services, &mut *self.factory, &selected) < 0 {
            let message = format!(
                "Unable to set super type for '{}' ({}) [{}]\n",
                String::from_utf8_lossy(&name_copy),
                String::from_utf8_lossy(&type_copy),
                String::from_utf8_lossy(&selected),
            );
            diag::dump(
                DumpFlags::NOTICE,
                &diag::general_line(
                    "Shading\\Materials\\MaterialBatcher.cpp",
                    0x441,
                    Severity::Notice,
                    &message,
                ),
            );
            object.destroy(services);
            return None;
        }
        self.enter_new(services, object, crc, &name_copy)
    }

    /// `LoadFromFileSystem` (slot 8): every directory child of `Material
    /// library` whose name does not start with `.`. A child whose name CRC
    /// is in the map is taken as it is; another is created from its `Type`
    /// leaf through [`MaterialLibrary::create_after_map`] and loaded from
    /// its node, named with the child's name as found; a material that
    /// fails to load is removed again. With `want_group`, the children
    /// taken or loaded form a new group, each with a count and a material
    /// reference.
    pub fn load_library(
        &mut self,
        services: &mut S,
        fs: &mut dyn FileSystem,
        want_group: bool,
    ) -> LibraryLoad {
        if fs.open_node(b"Material library").is_err() {
            return LibraryLoad {
                result: GR_GENERIC,
                group: want_group.then_some(u32::MAX),
            };
        }
        let mut members = Vec::new();
        let found = fs.find(b"*.*").unwrap_or_default();
        for item in found {
            let name = item.data.name.clone();
            if name.first() == Some(&b'.') || !item.data.is_dir() {
                continue;
            }
            let crc = name_crc32(&name);
            if let Some(entry) = self.map.get_mut(&crc) {
                if want_group {
                    entry.refs = entry.refs.wrapping_add(1);
                    if let Some(id) = entry.material {
                        self.materials.add_ref(id);
                    }
                    members.push(crc);
                }
                continue;
            }
            if fs.open_node(&name).is_err() {
                continue;
            }
            let type_name = fs
                .open_file(&FsDesc::read(b"Type"))
                .ok()
                .and_then(|mut file| {
                    let size = (file.size().ok()? as usize).min(0x103);
                    let mut buf = vec![0u8; size];
                    match file.read(&mut buf) {
                        Ok(n) if n == size => Some(c_str(&buf).to_vec()),
                        _ => None,
                    }
                });
            if let Some(type_name) = type_name
                && let Some(key) = self.create_after_map(services, &name, &type_name, true)
            {
                let material = self.map.get(&key).and_then(|e| e.material);
                let loaded = material
                    .and_then(|id| self.materials.get_mut(id))
                    .map(|m| m.load(services, fs));
                if loaded.is_some_and(|r| r >= 0) {
                    if let Some(m) = material.and_then(|id| self.materials.get_mut(id)) {
                        m.set_name(&name);
                    }
                    if want_group {
                        if let Some(entry) = self.map.get_mut(&key) {
                            entry.refs = entry.refs.wrapping_add(1);
                        }
                        if let Some(id) = material {
                            self.materials.add_ref(id);
                        }
                        members.push(crc);
                    }
                } else {
                    let message = format!(
                        "Unable to load material '{}' as type '{}'\n",
                        String::from_utf8_lossy(&name),
                        String::from_utf8_lossy(&type_name),
                    );
                    diag::dump(
                        DumpFlags::NOTICE,
                        &diag::general_line(SOURCE, 0x1C1, Severity::Notice, &message),
                    );
                    self.remove_material(services, 0, crc);
                }
            }
            let _ = fs.open_node(b"..");
        }
        let _ = fs.open_node(b"..");
        let group = want_group.then(|| {
            let id = self.next_group_id;
            self.groups.insert(
                id,
                MaterialGroup {
                    id,
                    name: None,
                    members,
                },
            );
            self.next_group_id = self.next_group_id.wrapping_add(1);
            id
        });
        LibraryLoad {
            result: GR_OK,
            group,
        }
    }
}

/// Trims backwards from `end` over the bytes in `set` and terminates after
/// the first byte outside it; the reference's `while (*p != 0 && in set)
/// --p; p[1] = 0;`. It stops at index 0 where the reference would walk on
/// before the buffer.
fn trim_back(line: &mut [u8], mut end: usize, set: &[u8]) {
    while line[end] != 0 && set.contains(&line[end]) {
        if end == 0 {
            break;
        }
        end -= 1;
    }
    if end + 1 < line.len() {
        line[end + 1] = 0;
    }
}

fn skip(line: &[u8], mut at: usize, set: &[u8]) -> usize {
    while at < line.len() && set.contains(&line[at]) {
        at += 1;
    }
    at
}

fn c_len(line: &[u8], at: usize) -> usize {
    line[at..]
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(line.len() - at)
}

/// One `[MaterialMap]` line as `ParseMaterialMap` cuts it, in place: the
/// rule's key, pattern and type; `None` without an `=`.
fn parse_map_line(line: &mut [u8]) -> Option<(i32, Vec<u8>, Vec<u8>)> {
    let len = c_len(line, 0);
    let last_eq = line[..len].iter().rposition(|&b| b == b'=')?;
    let first_eq = line[..len].iter().position(|&b| b == b'=')?;
    let mut mode = 1;
    let mut pattern = 0;
    if first_eq != last_eq {
        let selector = skip(line, 0, b" \t");
        trim_back(line, first_eq, b"=; \t");
        let selector_text = &line[selector..selector + c_len(line, selector)];
        mode = if eq_ignore_case(selector_text, b"name") {
            0
        } else {
            1
        };
        pattern = first_eq + 1;
    }
    let pattern = skip(line, pattern, b" \t");
    trim_back(line, last_eq, b"=; \t");
    let type_start = skip(line, last_eq + 1, b" \t");
    let type_len = c_len(line, type_start);
    if type_len > 0 {
        trim_back(line, type_start + type_len - 1, b"; \t");
    } else if type_start > 0 {
        trim_back(line, type_start - 1, b"; \t");
    }
    let pattern_text = line[pattern..pattern + c_len(line, pattern)].to_vec();
    let type_text = line[type_start..type_start + c_len(line, type_start)].to_vec();
    Some((mode, pattern_text, type_text))
}

/// One `[BatchedMaterials]` line as `ParseBatchedMaterials` cuts it: the key
/// and value around the first `=`; `None` without one.
pub(super) fn parse_batched_line(line: &mut [u8]) -> Option<(Vec<u8>, Vec<u8>)> {
    let len = c_len(line, 0);
    let eq = line[..len].iter().position(|&b| b == b'=')?;
    let mut key = 0;
    while line[key] != 0 && line[key] != b';' && (line[key] == b' ' || line[key] == b'\t') {
        key += 1;
    }
    trim_back(line, eq, b"=; \t");
    let mut value = eq + 1;
    while line[value] != 0 && line[value] != b';' && (line[value] == b' ' || line[value] == b'\t') {
        value += 1;
    }
    let value_len = c_len(line, value);
    if value_len > 0 {
        trim_back(line, value + value_len - 1, b"; \t");
    } else if value > 0 {
        trim_back(line, value - 1, b"; \t");
    }
    let key_text = line[key..key + c_len(line, key)].to_vec();
    let value_text = line[value..value + c_len(line, value)].to_vec();
    Some((key_text, value_text))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(text: &str) -> Option<(i32, String, String)> {
        let mut line = [0u8; LINE_BUFFER];
        line[..text.len()].copy_from_slice(text.as_bytes());
        parse_map_line(&mut line).map(|(m, p, t)| {
            (
                m,
                String::from_utf8_lossy(&p).into_owned(),
                String::from_utf8_lossy(&t).into_owned(),
            )
        })
    }

    #[test]
    fn map_lines() {
        assert_eq!(
            parse("name = ^alpha_mask.* = DcDt"),
            Some((0, "^alpha_mask.*".into(), "DcDt".into()))
        );
        assert_eq!(
            parse("Name = ^c_glass$ = HighGlassMaterial"),
            Some((0, "^c_glass$".into(), "HighGlassMaterial".into()))
        );
        assert_eq!(
            parse("EcEtOcOt = DcDtOcOt"),
            Some((1, "EcEtOcOt".into(), "DcDtOcOt".into()))
        );
        assert_eq!(
            parse("type = x.* = DcDt ;"),
            Some((1, "x.*".into(), "DcDt".into()))
        );
        assert_eq!(parse("novalue"), None);
        assert_eq!(parse("a ="), Some((1, "a".into(), String::new())));
    }

    #[test]
    fn batched_lines() {
        let mut line = [0u8; LINE_BUFFER];
        let text = b"Type = .*OcOt";
        line[..text.len()].copy_from_slice(text);
        assert_eq!(
            parse_batched_line(&mut line),
            Some((b"Type".to_vec(), b".*OcOt".to_vec()))
        );
    }
}
