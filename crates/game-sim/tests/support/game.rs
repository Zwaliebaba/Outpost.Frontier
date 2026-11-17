//! The game databases loaded from the original data, and their dump lines
//! (`CppSource/Oracle/oracle_game_data.cpp` writes the same).
//!
//! The dump root holds `EXE/OutpostFrontier.ini` and `DATA`. The `[Data]`
//! list of that INI is run through [`GameData::load`]; the oracle runs the
//! same list through Common's loaders, skipping the same keys.

use std::path::Path;
use std::sync::OnceLock;

use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::constants::Constants;
use neuron_core::math::Vector;
use neuron_core::vfs::Host;

use super::{f32_text, f64_text};

/// The engine INI that holds the `[Data]` list, relative to the dump root.
pub const ENGINE_INI: &str = "EXE/OutpostFrontier.ini";

/// The loaded data, or `None` when the root has no engine INI.
pub struct Loaded {
    /// The databases.
    pub data: GameData,
}

static LOADED: OnceLock<Option<Loaded>> = OnceLock::new();

/// Loads the data under `root` once per test binary.
pub fn load(root: &Path) -> Option<&'static Loaded> {
    LOADED
        .get_or_init(|| {
            let config = DataConfig::read(&Host::new(root), ENGINE_INI.as_bytes())?;
            let source = Host::new(root.join("DATA"));
            let mut data = GameData::new();
            data.load(&source, &config, None);
            Some(Loaded { data })
        })
        .as_ref()
}

fn vector_text(v: Vector) -> String {
    format!("{},{},{}", f32_text(v.x), f32_text(v.y), f32_text(v.z))
}

fn float_line(key: &str, value: f32) -> String {
    format!("{{\"key\":\"{key}\",\"f\":\"{}\"}}", f32_text(value))
}

fn int_line(key: &str, value: i32) -> String {
    format!("{{\"key\":\"{key}\",\"i\":\"{value}\"}}")
}

fn id_line(key: &str, value: u32) -> String {
    format!("{{\"key\":\"{key}\",\"id\":\"{value:08x}\"}}")
}

fn double_line(key: &str, value: f64) -> String {
    format!("{{\"key\":\"{key}\",\"d\":\"{}\"}}", f64_text(value))
}

fn vector_line(key: &str, value: Vector) -> String {
    format!("{{\"key\":\"{key}\",\"v\":\"{}\"}}", vector_text(value))
}

/// Kind `constants`, file `constants`: every exported global
/// `ReadConstants` fills, then per `OBJECT_TYPE` the collision damage.
pub fn constants_lines(c: &Constants) -> Vec<String> {
    let g = &c.general;
    let e = &c.engine;
    let t = &c.thruster;
    let p = &c.phy_sys;
    let m = &c.comm;
    let mut lines = vec![
        float_line("COLLISION_DAMAGE_FACTOR", g.collision_damage_factor),
        float_line("LOOT_OWNER_SAFE_TIME", g.loot_owner_safe_time),
        float_line("LOOT_UNSEEN_LIFE_TIME", g.loot_unseen_life_time),
        float_line("LOOT_UNSEEN_RADIUS", g.loot_unseen_radius),
        float_line("MUSIC_CROSS_FADE_DELAY", g.music_cross_fade_delay),
        float_line("JETTISONED_CARGO_VELOCITY", g.jettisoned_cargo_velocity),
        id_line("SND_CARGO_JETTISONED", g.snd_cargo_jettisoned.0),
        float_line("MUZZLE_CONE_ANGLE", g.muzzle_cone_angle),
        id_line("FIRE_FAILED_SOUND", g.fire_failed_sound.0),
        float_line("FIRE_FAILED_DELAY", g.fire_failed_delay),
        float_line(
            "PLAYER_COLLISION_GROUP_HIT_PTS_SCALE",
            g.player_collision_group_hit_pts_scale,
        ),
        float_line(
            "PLAYER_ATTACHED_EQUIP_HIT_PTS_SCALE",
            g.player_attached_equip_hit_pts_scale,
        ),
        int_line("MAX_PLAYER_AMMO", g.max_player_ammo),
        float_line("CRUISING_SPEED", e.cruising_speed),
        float_line("CRUISE_ACCEL_TIME", e.cruise_accel_time),
        float_line("CRUISE_DRAG", e.cruise_drag),
        float_line("CRUISE_DISRUPT_TIME", e.cruise_disrupt_time),
        float_line("MAX_DELTA_FX_THROTTLE", e.max_delta_fx_throttle),
        float_line("MAX_ENGINE_FX_THROTTLE", e.max_engine_fx_throttle),
        float_line("THROTTLE_STEADY_TIME", e.throttle_steady_time),
        float_line("THROTTLE_ATTEN_MOD_RANGE", e.throttle_atten_mod_range),
        float_line(
            "DELTA_THROTTLE_ATTEN_MOD_CHANGING",
            e.delta_throttle_atten_mod_changing,
        ),
        float_line(
            "DELTA_THROTTLE_ATTEN_MOD_STEADY",
            e.delta_throttle_atten_mod_steady,
        ),
        float_line("CRUISE_STEADY_TIME", e.cruise_steady_time),
        float_line(
            "DELTA_CRUISE_ATTEN_MOD_STEADY",
            e.delta_cruise_atten_mod_steady,
        ),
        float_line("CRUISE_ATTEN_MOD_RANGE", e.cruise_atten_mod_range),
        float_line("HULL_DAMAGE_FACTOR", c.shield.hull_damage_factor),
        id_line("INTERIOR_SOUND_NAME", t.interior_sound_name.0),
        id_line("EXTERIOR_SOUND_NAME", t.exterior_sound_name.0),
        float_line("MAX_VOLUME_FORCE", t.max_volume_force),
        float_line("MIN_VOLUME_FORCE", t.min_volume_force),
        float_line("INSIDE_CONE_ANGLE", t.inside_cone_angle),
        float_line("OUTSIDE_CONE_ANGLE", t.outside_cone_angle),
        float_line("OUTSIDE_CONE_ATTENUATION", t.outside_cone_attenuation),
        float_line("PHYSICAL_SIM_RATE", p.physical_sim_rate),
        int_line("MAX_SPAWNED_MINDIST_COUNT", p.max_spawned_mindist_count),
        float_line("ANOM_LIMITS_MAX_VELOCITY", p.anom_limits_max_velocity),
        float_line(
            "ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI",
            p.anom_limits_max_angular_velocity_per_psi,
        ),
        double_line("MATERIAL_FRICTION", p.material_friction),
        double_line("MATERIAL_ELASTICITY", p.material_elasticity),
        double_line("MIN_TIME_BETWEEN_COLLISIONS", p.min_time_between_collisions),
        float_line("RMGR_LOOK_AHEAD_TIME_WORLD", p.rmgr_look_ahead_time_world),
        float_line(
            "RMGR_LOOK_AHEAD_MAX_RADIUS_WORLD",
            p.rmgr_look_ahead_max_radius_world,
        ),
        float_line(
            "RMGR_LOOK_AHEAD_MIN_DISTANCE_WORLD",
            p.rmgr_look_ahead_min_distance_world,
        ),
        float_line(
            "RMGR_LOOK_AHEAD_MAX_DISTANCE_WORLD",
            p.rmgr_look_ahead_max_distance_world,
        ),
        float_line(
            "RMGR_LOOK_AHEAD_MIN_SECONDS_WORLD",
            p.rmgr_look_ahead_min_seconds_world,
        ),
        float_line("RMGR_LOOK_AHEAD_TIME_INTRA", p.rmgr_look_ahead_time_intra),
        float_line(
            "RMGR_LOOK_AHEAD_MAX_RADIUS_INTRA",
            p.rmgr_look_ahead_max_radius_intra,
        ),
        float_line(
            "RMGR_LOOK_AHEAD_MIN_DISTANCE_INTRA",
            p.rmgr_look_ahead_min_distance_intra,
        ),
        float_line(
            "RMGR_LOOK_AHEAD_MAX_DISTANCE_INTRA",
            p.rmgr_look_ahead_max_distance_intra,
        ),
        float_line(
            "RMGR_LOOK_AHEAD_MIN_SECONDS_INTRA",
            p.rmgr_look_ahead_min_seconds_intra,
        ),
        float_line("DEFAULT_LINEAR_DAMPING", p.default_linear_damping),
        vector_line("DEFAULT_ANGULAR_DAMPING", p.default_angular_damping),
        float_line("GOLEM_CHILD_MASS", p.golem_child_mass),
        float_line("GOLEM_CHILD_LINEAR_DAMP", p.golem_child_linear_damp),
        vector_line("GOLEM_CHILD_ANGULAR_DAMP", p.golem_child_angular_damp),
        float_line("GOLEM_MAX_DELTA_POSITION", p.golem_max_delta_position),
        float_line("GOLEM_DELTA_ORIENTATION", p.golem_delta_orientation),
        vector_line("GOLEM_MAX_TRANSLATION_FORCE", p.golem_max_translation_force),
        float_line("GOLEM_MAX_TORQUE", p.golem_max_torque),
        float_line("GOLEM_FORCE_FACTOR", p.golem_force_factor),
        float_line("GOLEM_DAMP_FACTOR", p.golem_damp_factor),
        float_line("GOLEM_TORQUE_FACTOR", p.golem_torque_factor),
        float_line("GOLEM_ANGULAR_DAMP_FACTOR", p.golem_angular_damp_factor),
        float_line("RC_MAX_DELTA_POSITION", p.rc_max_delta_position),
        float_line("RC_MAX_DELTA_ORIENTATION", p.rc_max_delta_orientation),
        float_line("COMM_PLAYER_FAR_DIST", m.comm_player_far_dist),
        float_line("COMM_PLAYER_FAR_DIST_ATTEN", m.comm_player_far_dist_atten),
        float_line("CHATTER_MAX_DIST", m.chatter_max_dist),
        float_line("CHATTER_MAX_DIST_ATTEN", m.chatter_max_dist_atten),
        float_line("CHATTER_START_ATTEN", m.chatter_start_atten),
        int_line(
            "COMM_CONFLICT_PRIORITY_CUTOFF",
            m.comm_conflict_priority_cutoff,
        ),
        float_line("WALLA_MAX_DIST", m.walla_max_dist),
        float_line("WALLA_MAX_DIST_ATTEN", m.walla_max_dist_atten),
        float_line("WALLA_START_ATTEN", m.walla_start_atten),
        int_line("WALLA_PRIORITY_CUTOFF", m.walla_priority_cutoff),
    ];
    let unit = Vector {
        x: 1.0,
        y: 0.0,
        z: 0.0,
    };
    for (name, bits) in game_sim::data::archetype::object_type::OBJECT_TYPES {
        lines.push(format!(
            "{{\"collision\":\"{}\",\"damage\":\"{}\",\"calc\":\"{}\"}}",
            String::from_utf8_lossy(name),
            f32_text(c.collision.impact_damage(bits)),
            f32_text(c.collision.collision_damage(bits, unit, unit)),
        ));
    }
    lines
}
