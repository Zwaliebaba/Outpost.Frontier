//! Dump lines of the goods, loadouts, factions and archetypes
//! (`CppSource/Oracle/oracle_game_data.cpp` writes the same).

use game_sim::data::archetype::ArchetypeDb;
use game_sim::data::archetype::eq_obj::{CollisionGroup, EqObj};
use game_sim::data::archetype::equipment::{
    Attached, Equipment, EquipmentKind, Launcher, Projectile,
};
use game_sim::data::archetype::root::Root;
use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::goods::GoodList;
use game_sim::data::loadout::LoadoutDb;
use game_sim::data::reputation::Reputation;
use game_sim::data::string_cache::{CacheString, StringCache};

use super::f32_text as f;

/// The JSON text of a byte string: printable ASCII except `"` and `\` as it
/// is, every other byte as `\u00xx` (the harness's `Text`).
pub fn json_text(bytes: &[u8]) -> String {
    let mut out = String::new();
    for &b in bytes {
        if (0x20..0x7F).contains(&b) && b != b'"' && b != b'\\' {
            out.push(char::from(b));
        } else {
            out.push_str(&format!("\\u{b:04x}"));
        }
    }
    out
}

/// A dump line built field by field (the harness's `FLine`).
#[derive(Default)]
pub struct Line(Vec<(String, String)>);

impl Line {
    /// An empty line.
    pub fn new() -> Self {
        Self::default()
    }

    /// Adds a field.
    pub fn add(&mut self, key: &str, value: impl Into<String>) -> &mut Self {
        self.0.push((key.to_owned(), value.into()));
        self
    }

    /// `{"k":"v",...}`.
    pub fn done(&self) -> String {
        let fields: Vec<String> = self
            .0
            .iter()
            .map(|(k, v)| format!("\"{k}\":\"{v}\""))
            .collect();
        format!("{{{}}}", fields.join(","))
    }
}

/// Renders against one string cache.
pub struct Render<'a> {
    /// The cache the handles point into.
    pub strings: &'a StringCache,
}

fn f3(v: [f32; 3]) -> String {
    format!("{},{},{}", f(v[0]), f(v[1]), f(v[2]))
}

fn b(v: bool) -> String {
    u8::from(v).to_string()
}

fn id(v: u32) -> String {
    format!("{v:08x}")
}

impl Render<'_> {
    fn t(&self, s: CacheString) -> String {
        json_text(self.strings.get(s))
    }

    /// An equipment or cargo line (`EquipDesc`).
    fn equip_desc(&self, line: &mut Line, d: &EquipDesc) {
        line.add("id", format!("{:04x}", d.id))
            .add("arch", id(d.arch_id))
            .add("hp", self.t(d.hardpoint))
            .add("internal", b(d.is_internal(self.strings)))
            .add("equipped", d.equipped.to_string())
            .add("health", f(d.health))
            .add("count", d.count.to_string())
            .add("mission", b(d.mission))
            .add("owner", d.owner.to_string());
    }

    /// Kind `goods`, file `goods`: every good in list order with its
    /// add-ons; `self_by_id` / `self_by_arch` tell whether the index finds
    /// this very good.
    pub fn goods(&self, goods: &GoodList) -> Vec<String> {
        let mut lines = Vec::new();
        for g in goods.goods() {
            let by_id = goods.find_by_id(g.id).is_some_and(|x| core::ptr::eq(x, g));
            let by_arch = goods
                .find_by_archetype(g.arch_id)
                .is_some_and(|x| core::ptr::eq(x, g));
            lines.push(
                Line::new()
                    .add("good", id(g.id))
                    .add("cat", id(g.category.value()))
                    .add("arch", id(g.arch_id))
                    .add("ship", id(g.ship_arch_id))
                    .add(
                        "price",
                        format!(
                            "{},{},{},{},{}",
                            f(g.price),
                            f(g.good_buy_price),
                            f(g.bad_buy_price),
                            f(g.good_sell_price),
                            f(g.bad_sell_price)
                        ),
                    )
                    .add("jump", g.jump_dist.to_string())
                    .add("comb", b(g.combinable))
                    .add("shop", self.t(g.shop_archetype))
                    .add("icon", self.t(g.item_icon))
                    .add("mat", self.t(g.material_library))
                    .add("hp_child", self.t(g.hp_child))
                    .add("attach", self.t(g.attachment_archetype))
                    .add("ids", format!("{},{}", g.ids_name, g.ids_info))
                    .add("hull", id(g.hull_good_id))
                    .add(
                        "ammo",
                        format!("{},{}", id(g.free_ammo_arch_id), g.free_ammo_count),
                    )
                    .add("prefix", json_text(&g.msg_id_prefix))
                    .add("self_by_id", b(by_id))
                    .add("self_by_arch", b(by_arch))
                    .done(),
            );
            for (slot, list) in [
                ("low", &g.addon_low),
                ("mid", &g.addon_mid),
                ("high", &g.addon_high),
            ] {
                for addon in list {
                    let mut line = Line::new();
                    line.add("addon", slot);
                    self.equip_desc(&mut line, addon);
                    lines.push(line.done());
                }
            }
        }
        lines
    }

    /// Kind `loadouts`, file `loadouts`: every loadout in id order.
    pub fn loadouts(&self, loadouts: &LoadoutDb) -> Vec<String> {
        let mut lines = Vec::new();
        for l in loadouts.iter() {
            lines.push(
                Line::new()
                    .add("loadout", id(l.nickname_id))
                    .add("n", l.items.len().to_string())
                    .done(),
            );
            for item in &l.items {
                let mut line = Line::new();
                self.equip_desc(&mut line, item);
                lines.push(line.done());
            }
        }
        lines
    }

    /// Kind `reputation`, file `groups`: every group in id order.
    pub fn groups(&self, rep: &Reputation) -> Vec<String> {
        rep.groups()
            .map(|(gid, g)| {
                Line::new()
                    .add("group", format!("{gid:04x}"))
                    .add("nickname", json_text(&g.nickname))
                    .add("name", rep.get_name(gid).to_string())
                    .add("info", rep.get_info_card(gid).to_string())
                    .add("short", rep.get_short_name(gid).to_string())
                    .done()
            })
            .collect()
    }

    fn root(&self, line: &mut Line, r: &Root) {
        line.add("nick", id(r.nickname))
            .add("da", self.t(r.da_archetype))
            .add("type", id(r.object_type))
            .add("ids", format!("{},{}", r.ids_name, r.ids_info))
            .add("hit_pts", f(r.hit_pts))
            .add("mass", f(r.mass))
            .add(
                "expl",
                format!("{},{}", id(r.explosion_arch), f(r.explosion_resistance)),
            )
            .add("ri", f3(r.rotation_inertia))
            .add("has_ri", b(r.has_rotation_inertia))
            .add("phantom", b(r.phantom_physics));
    }

    fn group(&self, lines: &mut Vec<String>, g: &CollisionGroup) {
        lines.push(
            Line::new()
                .add("group", format!("{:04x}", g.id))
                .add("obj", self.t(g.obj))
                .add("type", format!("{:x}", g.group_type))
                .add("hit_pts", g.hit_pts.to_string())
                .add(
                    "flags",
                    format!(
                        "{},{},{}",
                        b(g.separable),
                        b(g.destroy_parent),
                        b(g.root_health_proxy)
                    ),
                )
                .add(
                    "impulse",
                    format!("{},{}", f(g.parent_impulse), f(g.child_impulse)),
                )
                .add("mass", f(g.mass))
                .add("has_ri", b(g.has_rotation_inertia))
                .add("ri", f3(g.rotation_inertia))
                .add(
                    "expl",
                    format!(
                        "{},{},{}",
                        id(g.separation_explosion),
                        id(g.debris_type),
                        f(g.explosion_resistance)
                    ),
                )
                .add(
                    "linked",
                    format!("{:x},{}", g.linked_equip_type, f(g.linked_equip_health)),
                )
                .done(),
        );
        for d in &g.dmg_objs {
            lines.push(
                Line::new()
                    .add("dmg", format!("{},{}", self.t(d.hardpoint), id(d.arch)))
                    .done(),
            );
        }
        for d in &g.group_dmg_objs {
            lines.push(
                Line::new()
                    .add(
                        "group_dmg",
                        format!("{},{}", self.t(d.hardpoint), id(d.arch)),
                    )
                    .done(),
            );
        }
        for fuse in &g.fuses {
            lines.push(
                Line::new()
                    .add(
                        "group_fuse",
                        format!("{},{},{}", id(fuse.fuse), f(fuse.value1), f(fuse.value2)),
                    )
                    .done(),
            );
        }
    }

    fn eq_obj_tail(&self, lines: &mut Vec<String>, e: &EqObj) {
        for g in &e.collision_groups {
            self.group(lines, g);
        }
        for link in &e.shield_links {
            let gens: Vec<String> = link.generator_hps.iter().map(|&h| self.t(h)).collect();
            lines.push(
                Line::new()
                    .add("shield_link", id(link.shield_arch))
                    .add("id", format!("{:04x}", link.equip_id))
                    .add("hp", self.t(link.hardpoint))
                    .add("gens", gens.join(";"))
                    .done(),
            );
        }
        for fuse in &e.fuses {
            lines.push(
                Line::new()
                    .add(
                        "fuse",
                        format!("{},{},{}", id(fuse.fuse), f(fuse.value1), f(fuse.value2)),
                    )
                    .done(),
            );
        }
        for d in &e.docking_spheres {
            lines.push(
                Line::new()
                    .add("dock", d.dock_type.to_string())
                    .add("hp", json_text(&d.hardpoint))
                    .add("radius", f(d.radius))
                    .add("anim", json_text(&d.anim))
                    .done(),
            );
        }
    }

    fn eq_obj_head(line: &mut Line, e: &EqObj) {
        line.add("shield_next", format!("{:04x}", e.shield_id_next))
            .add("veilborn", b(e.veilborn))
            .add("dock_cam", b(e.docking_camera));
    }

    /// Kind `archetypes`, file `solars`.
    pub fn solars(&self, db: &ArchetypeDb) -> Vec<String> {
        let mut lines = Vec::new();
        for s in db.solars.values() {
            let mut line = Line::new();
            self.root(&mut line, &s.eq_obj.root);
            Self::eq_obj_head(&mut line, &s.eq_obj);
            line.add("destructible", b(s.destructible))
                .add("radius", f(s.solar_radius))
                .add("toughness", s.toughness.to_string())
                .add("open_anim", self.t(s.open_anim))
                .add("jump_out_hp", self.t(s.jump_out_hp))
                .add("loadout", id(s.loadout));
            lines.push(line.done());
            self.eq_obj_tail(&mut lines, &s.eq_obj);
        }
        lines
    }

    /// Kind `archetypes`, file `ships`.
    pub fn ships(&self, db: &ArchetypeDb) -> Vec<String> {
        let mut lines = Vec::new();
        for s in db.ships.values() {
            let mut line = Line::new();
            self.root(&mut line, &s.eq_obj.root);
            Self::eq_obj_head(&mut line, &s.eq_obj);
            line.add("prefix", json_text(&s.msg_id_prefix))
                .add(
                    "hps",
                    format!(
                        "{},{},{},{}",
                        self.t(s.bay_door_anim),
                        self.t(s.hp_bay_surface),
                        self.t(s.hp_bay_external),
                        self.t(s.hp_tractor_source)
                    ),
                )
                .add(
                    "ints",
                    format!(
                        "{},{},{},{},{},{},{}",
                        s.ids_info1,
                        s.ids_info2,
                        s.ids_info3,
                        s.ship_class,
                        s.num_exhaust_nozzles,
                        s.nanobot_limit,
                        s.shield_battery_limit
                    ),
                )
                .add(
                    "floats",
                    format!(
                        "{},{},{},{},{},{},{}",
                        f(s.hold_size),
                        f(s.linear_drag),
                        f(s.nudge_force),
                        f(s.strafe_force),
                        f(s.strafe_power_usage),
                        f(s.max_bank_angle),
                        f3(s.angular_drag)
                    ),
                )
                .add("torque", f3(s.steering_torque));
            lines.push(line.done());
            self.eq_obj_tail(&mut lines, &s.eq_obj);
            for l in &s.legal_hps {
                let hps: Vec<String> = l.hardpoints.iter().map(|&h| self.t(h)).collect();
                lines.push(
                    Line::new()
                        .add("hp_type", format!("{:x}", l.hp_type))
                        .add("hps", hps.join(";"))
                        .done(),
                );
            }
        }
        lines
    }

    /// Kind `archetypes`, file `simples` (or `dynamic_asteroids`).
    pub fn roots<'r>(&self, records: impl Iterator<Item = &'r Root>) -> Vec<String> {
        records
            .map(|r| {
                let mut line = Line::new();
                self.root(&mut line, r);
                line.done()
            })
            .collect()
    }

    /// Kind `archetypes`, file `asteroids`.
    pub fn asteroids(&self, db: &ArchetypeDb) -> Vec<String> {
        db.asteroids
            .values()
            .map(|a| {
                let mut line = Line::new();
                self.root(&mut line, &a.root);
                line.add("detect", f(a.detect_radius))
                    .add("offset", f(a.explosion_offset))
                    .add("recharge", f(a.recharge_time));
                line.done()
            })
            .collect()
    }

    fn attached(&self, a: &Attached) -> Vec<String> {
        vec![
            id(a.separation_explosion),
            id(a.debris_type),
            f(a.child_impulse),
            f(a.parent_impulse),
            self.t(a.hp_child),
            a.toughness.to_string(),
        ]
    }

    fn launcher(&self, l: &Launcher) -> Vec<String> {
        let mut out = self.attached(&l.attached);
        out.extend([
            f(l.damage_per_fire),
            f(l.power_usage),
            f(l.refire_delay),
            f(l.muzzle_velocity),
            id(l.projectile_archetype),
            self.t(l.use_animation),
        ]);
        out
    }

    fn projectile(p: &Projectile) -> Vec<String> {
        vec![
            f(p.lifetime),
            f(p.owner_safe_time),
            b(p.requires_ammo),
            b(p.force_gun_ori),
            id(p.loot_appearance.unwrap_or(0)),
        ]
    }

    /// The `data` tokens, `hp_types` and `fov_cos` of an equipment kind.
    fn kind(&self, kind: &EquipmentKind) -> (Vec<String>, Vec<u32>, Option<f32>) {
        let opt = |v: &Option<u32>| id(v.unwrap_or(0));
        match kind {
            EquipmentKind::LootCrate | EquipmentKind::AttachedFx | EquipmentKind::TradeLane => {
                (Vec::new(), Vec::new(), None)
            }
            EquipmentKind::Commodity {
                decay_per_second,
                loot_appearance,
                pod_appearance,
            } => (
                vec![
                    f(*decay_per_second),
                    opt(loot_appearance),
                    opt(pod_appearance),
                ],
                Vec::new(),
                None,
            ),
            EquipmentKind::Power {
                capacity,
                charge_rate,
                thrust_capacity,
                thrust_charge_rate,
            } => (
                vec![
                    f(*capacity),
                    f(*charge_rate),
                    f(*thrust_capacity),
                    f(*thrust_charge_rate),
                ],
                Vec::new(),
                None,
            ),
            EquipmentKind::Engine {
                max_force,
                linear_drag,
                power_usage,
                cruise_power_usage,
                cruise_charge_time,
                indestructible,
                reverse_fraction,
            } => (
                vec![
                    f(*max_force),
                    f(*linear_drag),
                    f(*power_usage),
                    f(*cruise_power_usage),
                    f(*cruise_charge_time),
                    b(*indestructible),
                    f(*reverse_fraction),
                ],
                Vec::new(),
                None,
            ),
            EquipmentKind::Armor { hit_pts_scale } => (vec![f(*hit_pts_scale)], Vec::new(), None),
            EquipmentKind::Shield(a) | EquipmentKind::CargoPod(a) => {
                (self.attached(a), Vec::new(), None)
            }
            EquipmentKind::ShieldGenerator(g) => {
                let mut out = self.attached(&g.attached);
                out.extend([
                    format!("{:x}", g.hp_type),
                    id(g.shield_type),
                    f(g.regeneration_rate),
                    f(g.max_capacity),
                    f(g.constant_power_draw),
                    f(g.rebuild_power_draw),
                    f(g.offline_threshold),
                    f(g.offline_rebuild_time),
                ]);
                (out, g.hp_shield_types.clone(), None)
            }
            EquipmentKind::Thruster {
                attached,
                power_usage,
                max_force,
            } => {
                let mut out = self.attached(attached);
                out.extend([f(*power_usage), f(*max_force)]);
                (out, Vec::new(), None)
            }
            EquipmentKind::Gun(g) => {
                let mut out = self.launcher(&g.launcher);
                out.extend([
                    f(g.dispersion_angle),
                    f(g.turn_rate),
                    f(g.azimuth_min),
                    f(g.azimuth_max),
                    f(g.elevation_min),
                    f(g.elevation_max),
                    b(g.azimuth_valid),
                    b(g.elevation_valid),
                    b(g.auto_turret),
                ]);
                (out, g.hp_gun_types.clone(), None)
            }
            EquipmentKind::MineDropper(l) => (self.launcher(l), Vec::new(), None),
            EquipmentKind::CounterMeasureDropper {
                launcher,
                ai_range_sq,
            } => {
                let mut out = self.launcher(launcher);
                out.push(f(*ai_range_sq));
                (out, Vec::new(), None)
            }
            EquipmentKind::Scanner {
                power_usage,
                range,
                cargo_scan_range,
            } => (
                vec![f(*power_usage), f(*range), f(*cargo_scan_range)],
                Vec::new(),
                None,
            ),
            EquipmentKind::Light {
                always_on,
                docking_light,
            } => (vec![b(*always_on), b(*docking_light)], Vec::new(), None),
            EquipmentKind::Tractor {
                max_length,
                reach_speed,
            } => (vec![f(*max_length), f(*reach_speed)], Vec::new(), None),
            EquipmentKind::InternalFx { use_animation } => {
                (vec![self.t(*use_animation)], Vec::new(), None)
            }
            EquipmentKind::RepairDroid { repair_rate } => (vec![f(*repair_rate)], Vec::new(), None),
            EquipmentKind::RepairKit { loot_appearance }
            | EquipmentKind::ShieldBattery { loot_appearance } => {
                (vec![opt(loot_appearance)], Vec::new(), None)
            }
            EquipmentKind::CloakingDevice {
                attached,
                power_usage,
                cloakin_time,
                cloakout_time,
                cloakin_fx,
                cloakout_fx,
            } => {
                let mut out = self.attached(attached);
                out.extend([
                    f(*power_usage),
                    f(*cloakin_time),
                    f(*cloakout_time),
                    id(*cloakin_fx),
                    id(*cloakout_fx),
                ]);
                (out, Vec::new(), None)
            }
            EquipmentKind::Munition(m) => {
                let mut out = Self::projectile(&m.projectile);
                out.extend([
                    f(m.hull_damage),
                    f(m.energy_damage),
                    id(m.weapon_type),
                    id(m.motor),
                    m.seeker.to_string(),
                    f(m.time_to_lock),
                    f(m.seeker_range),
                    f(m.max_angular_velocity),
                    f(m.detonation_dist_sq),
                    format!("{:x}", m.hp_type),
                    b(m.cruise_disruptor),
                ]);
                (out, Vec::new(), Some(m.seeker_fov_cos))
            }
            EquipmentKind::Mine {
                projectile,
                linear_drag,
                detonation_dist,
                seek_dist,
                acceleration,
                top_speed,
            } => {
                let mut out = Self::projectile(projectile);
                out.extend([
                    f(*linear_drag),
                    f(*detonation_dist),
                    f(*seek_dist),
                    f(*acceleration),
                    f(*top_speed),
                ]);
                (out, Vec::new(), None)
            }
            EquipmentKind::CounterMeasure {
                projectile,
                linear_drag,
                range,
                diversion_pctg,
            } => {
                let mut out = Self::projectile(projectile);
                out.extend([f(*linear_drag), f(*range), diversion_pctg.to_string()]);
                (out, Vec::new(), None)
            }
        }
    }

    /// Kind `archetypes`, file `equipment`. `fov_cos` (a munition's seeker
    /// cone) went through `FCOS` and is compared within 1 ulp.
    pub fn equipment(&self, db: &ArchetypeDb) -> Vec<String> {
        db.equipment
            .values()
            .map(|e: &Equipment| {
                let mut line = Line::new();
                self.root(&mut line, &e.root);
                let (data, hp_types, fov_cos) = self.kind(&e.kind);
                let hp_types: Vec<String> = hp_types.iter().map(|t| format!("{t:x}")).collect();
                line.add("class", format!("{:x}", e.class()))
                    .add("volume", f(e.volume))
                    .add(
                        "base",
                        format!(
                            "{},{},{},{}",
                            b(e.use_count),
                            e.units_per_container,
                            id(e.tractored_explosion),
                            b(e.lootable)
                        ),
                    )
                    .add("data", data.join(","))
                    .add("hp_types", hp_types.join(","))
                    .add("fov_cos", fov_cos.map(f).unwrap_or_default());
                line.done()
            })
            .collect()
    }

    /// Kind `archetypes`, file `motors`.
    pub fn motors(db: &ArchetypeDb) -> Vec<String> {
        db.motors
            .values()
            .map(|m| {
                Line::new()
                    .add("motor", id(m.nickname))
                    .add("delay", f(m.delay))
                    .add("accel", f(m.accel))
                    .add("lifetime", f(m.lifetime))
                    .done()
            })
            .collect()
    }

    /// Kind `archetypes`, file `explosions`.
    pub fn explosions(db: &ArchetypeDb) -> Vec<String> {
        let mut lines = Vec::new();
        for e in db.explosions.values() {
            lines.push(
                Line::new()
                    .add("explosion", id(e.nickname))
                    .add(
                        "damage",
                        format!(
                            "{},{},{},{}",
                            f(e.radius),
                            f(e.impulse),
                            f(e.hull_damage),
                            f(e.energy_damage)
                        ),
                    )
                    .add("process", e.process.to_string())
                    .add("life", format!("{},{}", f(e.lifetime), f(e.lifetime_max)))
                    .add("children", e.num_child_pieces.to_string())
                    .add("debris_impulse", f(e.debris_impulse))
                    .add(
                        "innards",
                        format!(
                            "{},{},{}",
                            f(e.innards_debris_start_time),
                            f(e.innards_debris_num),
                            f(e.innards_debris_radius)
                        ),
                    )
                    .done(),
            );
            for fx in &e.effects {
                lines.push(
                    Line::new()
                        .add("effect", format!("{},{}", id(fx.fx), f(fx.strength)))
                        .done(),
                );
            }
            for o in &e.innards_debris_objects {
                lines.push(Line::new().add("innards_object", id(*o)).done());
            }
            for d in &e.debris_types {
                lines.push(
                    Line::new()
                        .add("debris", format!("{},{}", id(d.debris), f(d.count)))
                        .done(),
                );
            }
        }
        lines
    }

    /// Kind `archetypes`, file `small_ids`.
    pub fn small_ids(db: &ArchetypeDb) -> Vec<String> {
        db.small_ids()
            .iter()
            .enumerate()
            .map(|(i, large)| {
                Line::new()
                    .add("small", (i + 1).to_string())
                    .add("large", id(*large))
                    .done()
            })
            .collect()
    }
}
