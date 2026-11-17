//! Dump lines of the universe, its zones and the base data
//! (`CppSource/Oracle/oracle_game_universe.cpp` writes the same).

use game_sim::data::base::BaseDataList;
use game_sim::data::universe::Universe;
use game_sim::data::zone::Zone;
use game_sim::data::{DataSource, Reports};
use neuron_core::math::Vector;

use super::f32_text as f;
use super::render::{Line, Render, json_text};

fn id(v: u32) -> String {
    format!("{v:08x}")
}

fn b(v: bool) -> String {
    u8::from(v).to_string()
}

fn vec3(v: Vector) -> String {
    format!("{},{},{}", f(v.x), f(v.y), f(v.z))
}

impl Render<'_> {
    /// Kind `universe`, file `systems`: `DaysPerSecond`, then every system
    /// in id order with its connections and zone ids.
    pub fn systems(&self, u: &Universe) -> Vec<String> {
        let mut lines = vec![
            Line::new()
                .add("days_per_second", super::f64_text(u.days_per_second))
                .done(),
        ];
        for s in u.systems.values() {
            lines.push(
                Line::new()
                    .add("system", id(s.nickname_id))
                    .add("nickname", self.text(s.nickname))
                    .add("prefix", json_text(&s.msg_id_prefix))
                    .add("file", self.text(s.file))
                    .add("strid", s.strid_name.to_string())
                    .add("ids_info", s.ids_info.to_string())
                    .add("visit", s.visit.to_string())
                    .add("scale", f(s.nav_map_scale))
                    .add("pos", vec3(s.pos))
                    .add("music", id(s.space_music))
                    .add("zones", s.zones.len().to_string())
                    .add("conns", s.connections.len().to_string())
                    .done(),
            );
            for c in &s.connections {
                lines.push(
                    Line::new()
                        .add("conn", id(c.goto_system))
                        .add("object", id(c.object))
                        .add("object_id", id(c.object_id))
                        .add("pos", vec3(c.pos))
                        .add("visit", c.visit.to_string())
                        .add("gate", c.gate_type.value().to_string())
                        .add("gate_system", id(u.gate_system(c.object_id)))
                        .done(),
                );
            }
            for z in &s.zones {
                lines.push(Line::new().add("zone_ref", id(*z)).done());
            }
        }
        lines
    }

    /// Kind `universe`, file `objects`: every system's `[Object]` records
    /// as `GetSystemObject` returns them.
    pub fn objects(&self, u: &Universe, source: &dyn DataSource) -> Vec<String> {
        let mut lines = Vec::new();
        let mut reports = Reports::default();
        for s in u.systems.values() {
            for o in u.system_objects(source, s, self.strings, &mut reports) {
                lines.push(
                    Line::new()
                        .add("object", json_text(&o.nickname))
                        .add("system", id(s.nickname_id))
                        .add("pos", vec3(o.pos))
                        .add("arch", id(o.archetype))
                        .add("ids", format!("{},{}", o.ids_name, o.ids_info))
                        .add("rep", json_text(&o.reputation))
                        .add("dock_with", id(o.dock_with))
                        .add("goto", id(o.goto))
                        .add("base", id(o.base))
                        .add("next", json_text(&o.next_ring))
                        .add("prev", json_text(&o.prev_ring))
                        .add("visit", o.visit.to_string())
                        .done(),
                );
            }
        }
        lines
    }

    /// Kind `universe`, file `bases`.
    pub fn bases(&self, u: &Universe) -> Vec<String> {
        u.bases
            .values()
            .map(|base| {
                let ships: Vec<String> = base
                    .ship_sml
                    .iter()
                    .chain(&base.ship_mdm)
                    .chain(&base.ship_lrg)
                    .map(|&v| id(v))
                    .collect();
                let terrain: Vec<String> = [
                    base.terrain_tiny,
                    base.terrain_sml,
                    base.terrain_mdm,
                    base.terrain_lrg,
                    base.terrain_dyna_01,
                    base.terrain_dyna_02,
                ]
                .iter()
                .map(|&v| id(v))
                .collect();
                Line::new()
                    .add("base", id(base.nickname_id))
                    .add("nickname", self.text(base.nickname))
                    .add("strid", base.strid_name.to_string())
                    .add("file", self.text(base.file))
                    .add("system", id(base.system))
                    .add("bgcs", json_text(&base.bgcs_base_run_by))
                    .add(
                        "space_object",
                        format!("{},{}", id(base.space_object), base.space_object_visit),
                    )
                    .add("ships", ships.join(","))
                    .add("terrain", terrain.join(","))
                    .add("autosave", b(base.autosave_forbidden))
                    .done()
            })
            .collect()
    }

    fn zone(&self, lines: &mut Vec<String>, z: &Zone) {
        let o: Vec<String> = z.orientation.to_words().iter().map(|&v| f(v)).collect();
        lines.push(
            Line::new()
                .add("zone", id(z.nickname))
                .add("system", id(z.system))
                .add("orient", o.join(","))
                .add("pos", vec3(z.pos))
                .add("shape", z.shape.value().to_string())
                .add("flags", id(z.property_flags))
                .add("size", vec3(z.size))
                .add(
                    "physics",
                    format!(
                        "{},{},{},{}",
                        f(z.edge_fraction),
                        f(z.drag_modifier),
                        f(z.damage),
                        f(z.interference)
                    ),
                )
                .add("music", id(z.music))
                .add(
                    "dust",
                    format!("{},{}", self.text(z.spacedust), z.spacedust_max_particles),
                )
                .add("fog", vec3(z.fog_color))
                .add("path_label", b(z.path_label))
                .add("visit", z.visit.to_string())
                .add("ids", format!("{},{}", z.ids_name, z.ids_info))
                .add("radius", f(z.radius()))
                .add("physical", b(z.has_physical_effect()))
                .done(),
        );
        for (faction, weight) in &z.population {
            lines.push(
                Line::new()
                    .add("faction", format!("{faction:04x}"))
                    .add("weight", f(*weight))
                    .done(),
            );
        }
    }

    /// Kind `universe`, file `zones`: each system's zones in file order,
    /// systems in id order. `orient` went through sine and cosine (Tier B).
    pub fn zones(&self, u: &Universe) -> Vec<String> {
        let mut lines = Vec::new();
        for s in u.systems.keys() {
            for z in u.system_zones(*s) {
                self.zone(&mut lines, z);
            }
        }
        lines
    }

    /// Kind `bases`, file `base_data`: every base's data with its rooms and
    /// market.
    pub fn base_data(list: &BaseDataList) -> Vec<String> {
        let mut lines = Vec::new();
        for d in &list.bases {
            lines.push(
                Line::new()
                    .add("base_data", id(d.base_id))
                    .add("start", id(d.start_room))
                    .add("variance", f(d.price_variance))
                    .add("repair", f(d.ship_repair_cost))
                    .add("rooms", d.rooms.len().to_string())
                    .add("market", d.market.len().to_string())
                    .done(),
            );
            for room in &d.rooms {
                let mut line = Line::new();
                match room {
                    Some(r) => line
                        .add("room", id(r.room_id))
                        .add("base", id(r.base_id))
                        .add("nickname", json_text(&r.nickname))
                        .add("file", json_text(&r.file)),
                    None => line
                        .add("room", id(0))
                        .add("base", id(0))
                        .add("nickname", "")
                        .add("file", ""),
                };
                lines.push(line.done());
            }
            for m in d.market.values() {
                lines.push(
                    Line::new()
                        .add("good", id(m.good_id))
                        .add("price", f(m.price))
                        .add(
                            "inventory",
                            format!("{},{}", m.min_inventory, m.max_inventory),
                        )
                        .add("type", m.transaction.0.to_string())
                        .add("level", f(m.min_level))
                        .add("rep", f(m.min_rep))
                        .done(),
                );
            }
        }
        lines
    }
}
