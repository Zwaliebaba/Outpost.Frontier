//! Goods (`GoodInfoList`, `GoodDesc`), ported from
//! `CppSource/Common/common_goodinfo.cpp`.
//!
//! Every `goods` value of the `[Data]` list adds the `[Good]` blocks of one
//! file to the single good list. A good is kept in file order and indexed by
//! nickname id and by archetype id; a later good with the same key replaces
//! the earlier one in the index. Commodities and equipment take their
//! archetype from `equipment` and inherit names and visual strings from the
//! equipment archetype, which must already be loaded.

use std::collections::BTreeMap;

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::equip_desc::EquipDesc;
use super::string_cache::{CacheString, StringCache};
use super::{DataSource, Reports, lossy, open_ini};

const SOURCE: &str = "Common\\GoodList.cpp";

/// `TString<N>`: at most `N - 1` bytes, as `TString::operator=` copies.
pub(crate) fn tstring(text: &[u8], capacity: usize) -> Vec<u8> {
    let mut text = neuron_core::hash::c_str(text).to_vec();
    text.truncate(capacity - 1);
    text
}

/// `EGoodCategory`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GoodCategory {
    /// `Commodity` (0).
    Commodity,
    /// `Equipment` (1).
    Equipment,
    /// `ShipHull` (2).
    ShipHull,
    /// `Ship` (3).
    Ship,
    /// No `category` key (0xFFFF_FFFF).
    Unknown,
}

impl GoodCategory {
    /// The C++ value.
    #[must_use]
    pub fn value(self) -> u32 {
        match self {
            Self::Commodity => 0,
            Self::Equipment => 1,
            Self::ShipHull => 2,
            Self::Ship => 3,
            Self::Unknown => 0xFFFF_FFFF,
        }
    }
}

/// What a good inherits from its equipment archetype
/// (`Archetype::GetEquipment` in `read_Good_block`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct EquipmentGoodFields {
    /// `IdsName`.
    pub ids_name: i32,
    /// `IdsInfo`.
    pub ids_info: i32,
    /// For attached equipment (`is_attached`): `DA_archetype` and `HP_child`.
    pub attached: Option<(CacheString, CacheString)>,
}

/// The equipment archetypes, as goods see them.
pub trait EquipmentLookup {
    /// The fields of equipment archetype `arch_id`, or `None` when there is
    /// none.
    fn good_fields(&self, arch_id: u32) -> Option<EquipmentGoodFields>;
}

impl EquipmentLookup for () {
    fn good_fields(&self, _arch_id: u32) -> Option<EquipmentGoodFields> {
        None
    }
}

/// `GoodInfo`.
#[derive(Clone, Debug, PartialEq)]
pub struct GoodInfo {
    /// `msg_id_prefix` (`CommReferrable`, a `TString<64>`).
    pub msg_id_prefix: Vec<u8>,
    /// The nickname id.
    pub id: u32,
    /// `category`.
    pub category: GoodCategory,
    /// The equipment archetype (commodities and equipment), else 0.
    pub arch_id: u32,
    /// `ship`.
    pub ship_arch_id: u32,
    /// `price`.
    pub price: f32,
    /// `good_buy_price`, or 0.95 times the price.
    pub good_buy_price: f32,
    /// `bad_buy_price`, or 1.05 times the price.
    pub bad_buy_price: f32,
    /// `good_sell_price`, or 1.05 times the price.
    pub good_sell_price: f32,
    /// `bad_sell_price`, or 0.95 times the price.
    pub bad_sell_price: f32,
    /// `jump_dist` (50).
    pub jump_dist: i32,
    /// `combinable`.
    pub combinable: bool,
    /// `shop_archetype`.
    pub shop_archetype: CacheString,
    /// `item_icon`.
    pub item_icon: CacheString,
    /// `material_library`.
    pub material_library: CacheString,
    /// `HP_child`.
    pub hp_child: CacheString,
    /// `attachment_archetype`.
    pub attachment_archetype: CacheString,
    /// `ids_name`.
    pub ids_name: i32,
    /// `ids_info`.
    pub ids_info: i32,
    /// `hull`: the hull good's id.
    pub hull_good_id: u32,
    /// `addon` lines without a slot.
    pub addon_low: Vec<EquipDesc>,
    /// `addon` lines with slot `mid`.
    pub addon_mid: Vec<EquipDesc>,
    /// `addon` lines with slot `high`.
    pub addon_high: Vec<EquipDesc>,
    /// `free_ammo`: archetype.
    pub free_ammo_arch_id: u32,
    /// `free_ammo`: count.
    pub free_ammo_count: i32,
}

impl Default for GoodInfo {
    fn default() -> Self {
        Self {
            msg_id_prefix: Vec::new(),
            id: 0,
            category: GoodCategory::Unknown,
            arch_id: 0,
            ship_arch_id: 0,
            price: 0.0,
            good_buy_price: 0.0,
            bad_buy_price: 0.0,
            good_sell_price: 0.0,
            bad_sell_price: 0.0,
            jump_dist: 0x32,
            combinable: false,
            shop_archetype: CacheString::default(),
            item_icon: CacheString::default(),
            material_library: CacheString::default(),
            hp_child: CacheString::default(),
            attachment_archetype: CacheString::default(),
            ids_name: 0,
            ids_info: 0,
            hull_good_id: 0,
            addon_low: Vec::new(),
            addon_mid: Vec::new(),
            addon_high: Vec::new(),
            free_ammo_arch_id: 0,
            free_ammo_count: 0,
        }
    }
}

impl GoodInfo {
    /// `has_visual_attachment`.
    #[must_use]
    pub fn has_visual_attachment(&self) -> bool {
        !self.attachment_archetype.is_empty()
    }
}

/// The good list (`GoodInfoList`).
#[derive(Clone, Debug, Default)]
pub struct GoodList {
    goods: Vec<GoodInfo>,
    by_id: BTreeMap<u32, usize>,
    by_arch: BTreeMap<u32, usize>,
}

const CATEGORIES: [(&[u8], GoodCategory); 4] = [
    (b"Commodity", GoodCategory::Commodity),
    (b"Equipment", GoodCategory::Equipment),
    (b"ShipHull", GoodCategory::ShipHull),
    (b"Ship", GoodCategory::Ship),
];

/// The loader's context: the string cache, the archetypes and the reports.
pub struct GoodsContext<'a> {
    /// The string cache.
    pub strings: &'a mut StringCache,
    /// The equipment archetypes.
    pub equipment: &'a dyn EquipmentLookup,
    /// Where messages go.
    pub reports: &'a mut Reports,
}

impl GoodList {
    /// Every good in file order (`get_list`).
    #[must_use]
    pub fn goods(&self) -> &[GoodInfo] {
        &self.goods
    }

    /// `find_by_id`.
    #[must_use]
    pub fn find_by_id(&self, id: u32) -> Option<&GoodInfo> {
        self.by_id.get(&id).map(|&i| &self.goods[i])
    }

    /// `find_by_name`: `CreateID(name)`, then [`GoodList::find_by_id`].
    #[must_use]
    pub fn find_by_name(&self, name: &[u8]) -> Option<&GoodInfo> {
        self.find_by_id(create_id(name))
    }

    /// `find_by_archetype`.
    #[must_use]
    pub fn find_by_archetype(&self, arch_id: u32) -> Option<&GoodInfo> {
        self.by_arch.get(&arch_id).map(|&i| &self.goods[i])
    }

    /// `find_by_ship_arch`: the first good in file order with that ship.
    #[must_use]
    pub fn find_by_ship_arch(&self, arch_id: u32) -> Option<&GoodInfo> {
        self.goods.iter().find(|g| g.ship_arch_id == arch_id)
    }

    /// The index by nickname id, in key order.
    pub fn by_id(&self) -> impl Iterator<Item = (u32, &GoodInfo)> {
        self.by_id.iter().map(|(&k, &i)| (k, &self.goods[i]))
    }

    /// The index by archetype id, in key order.
    pub fn by_archetype(&self) -> impl Iterator<Item = (u32, &GoodInfo)> {
        self.by_arch.iter().map(|(&k, &i)| (k, &self.goods[i]))
    }

    /// `Good2Arch`.
    #[must_use]
    pub fn good_to_arch(&self, good_id: u32) -> u32 {
        self.find_by_id(good_id).map_or(0, |g| g.arch_id)
    }

    /// `Arch2Good`.
    #[must_use]
    pub fn arch_to_good(&self, arch_id: u32) -> u32 {
        self.find_by_archetype(arch_id).map_or(0, |g| g.id)
    }

    /// `arch_is_combinable`.
    #[must_use]
    pub fn arch_is_combinable(&self, arch_id: u32) -> bool {
        self.find_by_archetype(arch_id)
            .is_some_and(|g| g.combinable)
    }

    /// `destroy`.
    pub fn clear(&mut self) {
        *self = Self::default();
    }

    /// `GoodInfoList::load(path)`.
    pub fn load(&mut self, source: &dyn DataSource, path: &[u8], cx: &mut GoodsContext<'_>) {
        let Some(mut reader) = open_ini(source, path) else {
            cx.reports.push(
                SOURCE,
                0x1c2,
                format!("*** ERROR: Cannot open goods file {}", lossy(path)),
            );
            return;
        };
        while reader.read_header() {
            if reader.is_header(b"Good") {
                let info = self.read_good_block(&mut reader, cx);
                if info.id != 0 && info.arch_id != 0 && info.id != info.arch_id {
                    cx.reports.push(
                        SOURCE,
                        0x1b0,
                        format!(
                            "*** WARNING: Good (0x{:x}) nickname different from equipment",
                            info.id
                        ),
                    );
                }
                let index = self.goods.len();
                self.by_id.insert(info.id, index);
                self.by_arch.insert(info.arch_id, index);
                self.goods.push(info);
            } else if reader.is_header(b"GoodType") {
                cx.reports.push(
                    SOURCE,
                    0x1ba,
                    format!(
                        "*** WARNING: [GoodType] is obsolete in ini file {}",
                        lossy(path)
                    ),
                );
            }
        }
    }

    /// `read_Good_block`.
    fn read_good_block(&self, reader: &mut IniReader, cx: &mut GoodsContext<'_>) -> GoodInfo {
        let mut info = GoodInfo::default();
        let mut equipment_arch = 0;
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                let id = create_id(&reader.value_string());
                if self.by_id.contains_key(&id) {
                    // The whole block ends here, the fix-up below included.
                    return info;
                }
                info.id = id;
            } else if reader.is_value(b"equipment") {
                equipment_arch = create_id(&reader.value_string());
            } else if reader.is_value(b"free_ammo") {
                info.free_ammo_arch_id = create_id(&reader.value_string_at(0));
                info.free_ammo_count = reader.value_int(1);
            } else if reader.is_value(b"ids_name") || reader.is_value(b"name") {
                info.ids_name = reader.value_int(0);
            } else if reader.is_value(b"ids_info") || reader.is_value(b"info_desc") {
                info.ids_info = reader.value_int(0);
            } else if reader.is_value(b"category") {
                let text = reader.value_string();
                match CATEGORIES
                    .iter()
                    .find(|(name, _)| eq_ignore_case(&text, name))
                {
                    Some(&(_, category)) => info.category = category,
                    None => cx.reports.push(
                        SOURCE,
                        0xab,
                        format!("*** ERROR: unknown value: category = {}", lossy(&text)),
                    ),
                }
            } else if reader.is_value(b"ship") {
                info.ship_arch_id = create_id(&reader.value_string());
            } else if reader.is_value(b"price") {
                let exact = f64::from(reader.value_int(0));
                let low = f64::from(0.95_f32);
                let high = f64::from(1.05_f32);
                info.price = exact as f32;
                if info.good_buy_price == 0.0 {
                    info.good_buy_price = (low * exact) as f32;
                }
                if info.bad_buy_price == 0.0 {
                    info.bad_buy_price = (high * exact) as f32;
                }
                if info.good_sell_price == 0.0 {
                    info.good_sell_price = (high * exact) as f32;
                }
                if info.bad_sell_price == 0.0 {
                    info.bad_sell_price = (exact * low) as f32;
                }
            } else if reader.is_value(b"good_buy_price") {
                info.good_buy_price = (reader.value_float(0) * f64::from(info.price)) as f32;
            } else if reader.is_value(b"bad_buy_price") {
                info.bad_buy_price = (reader.value_float(0) * f64::from(info.price)) as f32;
            } else if reader.is_value(b"good_sell_price") {
                info.good_sell_price = (reader.value_float(0) * f64::from(info.price)) as f32;
            } else if reader.is_value(b"bad_sell_price") {
                info.bad_sell_price = (reader.value_float(0) * f64::from(info.price)) as f32;
            } else if reader.is_value(b"jump_dist") {
                info.jump_dist = reader.value_int(0);
            } else if reader.is_value(b"shop_archetype") {
                info.shop_archetype = cx.strings.alloc(&reader.value_string(), true);
            } else if reader.is_value(b"material_library") {
                info.material_library = cx.strings.alloc(&reader.value_string(), true);
            } else if reader.is_value(b"item_icon") {
                info.item_icon = cx.strings.alloc(&reader.value_string(), true);
            } else if reader.is_value(b"HP_child") {
                info.hp_child = cx.strings.alloc(&reader.value_string(), false);
            } else if reader.is_value(b"attachment_archetype") {
                info.attachment_archetype = cx.strings.alloc(&reader.value_string(), true);
            } else if reader.is_value(b"hull") {
                let hull = create_id(&reader.value_string());
                match self.find_by_id(hull) {
                    Some(found) => info.hull_good_id = found.id,
                    None => cx.reports.push(
                        SOURCE,
                        0,
                        format!(
                            "hull {} is not a good; the reference dereferences null here",
                            lossy(&reader.value_string())
                        ),
                    ),
                }
            } else if reader.is_value(b"addon") {
                self.read_addon(reader, &mut info, cx.strings);
            } else if reader.is_value(b"combinable") {
                info.combinable = reader.value_bool(0);
            } else if reader.is_value(b"msg_id_prefix") {
                info.msg_id_prefix = tstring(&reader.value_string(), 64);
            }
        }
        if matches!(
            info.category,
            GoodCategory::Commodity | GoodCategory::Equipment
        ) {
            info.arch_id = equipment_arch;
            match cx.equipment.good_fields(equipment_arch) {
                Some(fields) => {
                    if info.ids_name == 0 {
                        info.ids_name = fields.ids_name;
                    }
                    if info.ids_info == 0 {
                        info.ids_info = fields.ids_info;
                    }
                    if let Some((da_archetype, hp_child)) = fields.attached {
                        if info.attachment_archetype.is_empty() && !da_archetype.is_empty() {
                            info.attachment_archetype = da_archetype;
                        }
                        if info.hp_child.is_empty() && !hp_child.is_empty() {
                            info.hp_child = hp_child;
                        }
                    }
                }
                None => cx.reports.push(
                    SOURCE,
                    0,
                    format!(
                        "good 0x{:x}: equipment 0x{equipment_arch:x} is not an archetype; \
                         the reference dereferences null here",
                        info.id
                    ),
                ),
            }
        } else {
            info.arch_id = 0;
        }
        if info.shop_archetype.is_empty() && !info.attachment_archetype.is_empty() {
            info.shop_archetype = info.attachment_archetype;
        }
        info
    }

    fn read_addon(&self, reader: &mut IniReader, info: &mut GoodInfo, strings: &mut StringCache) {
        let arch = create_id(&reader.value_string_at(0));
        let hardpoint = strings.alloc(&reader.value_string_at(1), false);
        let mut count = reader.value_int(2);
        if count <= 0 {
            count = 1;
        }
        let slot = if reader.is_value_empty(3) {
            Vec::new()
        } else {
            tstring(&reader.value_string_at(3), 0x40)
        };
        let target = if eq_ignore_case(&slot, b"mid") {
            &mut info.addon_mid
        } else if eq_ignore_case(&slot, b"high") {
            &mut info.addon_high
        } else {
            &mut info.addon_low
        };
        let mut addon = EquipDesc::new(strings);
        addon.arch_id = arch;
        addon.count = count;
        addon.health = 1.0;
        addon.equipped = 1;
        if eq_ignore_case(strings.get(hardpoint), b"internal") {
            addon.make_internal(strings);
        } else {
            addon.hardpoint = hardpoint;
        }
        target.push(addon);
    }
}

/// `GoodDesc`: a quantity of a good with its price (trade and cargo lists).
#[derive(Clone, Debug, PartialEq)]
pub struct GoodDesc {
    /// The good.
    pub good_id: u32,
    /// How many.
    pub count: i32,
    /// The price.
    pub price: f32,
    /// The field at +0x0C.
    pub field_0c: f32,
}

/// `add_good_item`: combines into the last item of the same good when
/// `combinable` (returns 1), else appends (returns 0).
pub fn add_good_item(
    list: &mut Vec<GoodDesc>,
    good_id: u32,
    count: i32,
    price: f32,
    field_0c: f32,
    combinable: bool,
) -> i32 {
    if combinable && let Some(found) = list.iter_mut().rev().find(|g| g.good_id == good_id) {
        found.count = found.count.wrapping_add(count);
        return 1;
    }
    list.push(GoodDesc {
        good_id,
        count,
        price,
        field_0c,
    });
    0
}

/// `remove_good_item`: takes `count` (all for -1) from the last item of the
/// good; -1 when there is none, 0 when the item is gone, 1 otherwise.
pub fn remove_good_item(list: &mut Vec<GoodDesc>, good_id: u32, count: i32) -> i32 {
    let Some(index) = list.iter().rposition(|g| g.good_id == good_id) else {
        return -1;
    };
    let item = &mut list[index];
    if count == -1 {
        item.count = 0;
    } else {
        item.count = item.count.wrapping_sub(count);
    }
    if item.count != 0 {
        return 1;
    }
    list.remove(index);
    0
}

/// `find_good_item`: the last item of the good.
#[must_use]
pub fn find_good_item(list: &[GoodDesc], good_id: u32) -> Option<&GoodDesc> {
    list.iter().rev().find(|g| g.good_id == good_id)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct One(&'static [u8]);

    impl DataSource for One {
        fn read(&self, _path: &[u8]) -> Option<Vec<u8>> {
            Some(self.0.to_vec())
        }
    }

    struct Equip(CacheString, CacheString);

    impl EquipmentLookup for Equip {
        fn good_fields(&self, arch_id: u32) -> Option<EquipmentGoodFields> {
            (arch_id == create_id(b"gun")).then_some(EquipmentGoodFields {
                ids_name: 7,
                ids_info: 8,
                attached: Some((self.0, self.1)),
            })
        }
    }

    #[test]
    fn goods_load() {
        let text = b"[Good]\nnickname = gun\nequipment = gun\ncategory = equipment\nprice = 100\n\
good_sell_price = 2\ncombinable = true\naddon = a, internal, 0\naddon = b, HpX, 2, MID\n\
[Good]\nnickname = hull\ncategory = shiphull\nprice = 7\n\
[Good]\nnickname = ship\ncategory = ship\nhull = hull\n\
[Good]\nnickname = gun\nprice = 1\n";
        let mut strings = StringCache::new();
        let equip = Equip(
            strings.alloc(b"gun.cmp", true),
            strings.alloc(b"HpConnect", false),
        );
        let mut reports = Reports::default();
        let mut list = GoodList::default();
        let mut cx = GoodsContext {
            strings: &mut strings,
            equipment: &equip,
            reports: &mut reports,
        };
        list.load(&One(text), b"goods.ini", &mut cx);
        assert_eq!(list.goods().len(), 4);
        let gun = list.find_by_name(b"gun").unwrap();
        assert_eq!(gun.price, 100.0);
        assert_eq!(gun.good_buy_price, (100.0 * f64::from(0.95_f32)) as f32);
        assert_eq!(gun.good_sell_price, 200.0);
        assert_eq!(gun.ids_name, 7);
        assert_eq!(strings.get(gun.shop_archetype), b"gun.cmp");
        assert!(gun.addon_low[0].is_internal(&strings));
        assert_eq!(gun.addon_mid[0].count, 2);
        assert!(list.arch_is_combinable(create_id(b"gun")));
        let ship = list.find_by_name(b"ship").unwrap();
        assert_eq!(ship.hull_good_id, create_id(b"hull"));
        // The repeated nickname stops its block and lands under id 0.
        assert_eq!(list.find_by_id(0).unwrap().price, 0.0);
    }

    #[test]
    fn good_items() {
        let mut list = Vec::new();
        assert_eq!(add_good_item(&mut list, 1, 2, 0.0, 0.0, true), 0);
        assert_eq!(add_good_item(&mut list, 1, 3, 0.0, 0.0, false), 0);
        assert_eq!(add_good_item(&mut list, 1, 4, 0.0, 0.0, true), 1);
        assert_eq!(list[1].count, 7);
        assert_eq!(remove_good_item(&mut list, 1, 7), 0);
        assert_eq!(remove_good_item(&mut list, 1, -1), 0);
        assert_eq!(remove_good_item(&mut list, 1, 1), -1);
    }
}
