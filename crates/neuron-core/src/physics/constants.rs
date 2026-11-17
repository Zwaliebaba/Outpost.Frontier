//! The `[PhySys]` constants (`PhySys::PHYSICAL_SIM_RATE` and the rest,
//! `CppSource/Common/common_constants.cpp:239-264`,
//! `common_physys.cpp:101-121`) and their `[PhySysConsts]` section in
//! `constants.ini` (`ReadConstants`, `common_constants.cpp:446-479`).
//!
//! The shipped data sets four of them (`MATERIAL_FRICTION`,
//! `MATERIAL_ELASTICITY`, `DEFAULT_LINEAR_DAMPING`,
//! `DEFAULT_ANGULAR_DAMPING`) and leaves `PHYSICAL_SIM_RATE` at 16
//! (fact F5).

use crate::ini::IniReader;
use crate::math::Vector;

/// Every `PhySys` constant with its compiled-in default.
#[derive(Clone, Debug, PartialEq)]
pub struct PhySysConstants {
    /// `PHYSICAL_SIM_RATE`: sets only a new environment's first PSI length
    /// (fact F16).
    pub physical_sim_rate: f32,
    /// `MAX_SPAWNED_MINDIST_COUNT`: IVP's collision pair limit; kept for the
    /// record, the port's contacts have no such limit.
    pub max_spawned_mindist_count: i32,
    /// `ANOM_LIMITS_MAX_VELOCITY`.
    pub anom_limits_max_velocity: f32,
    /// `ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI`.
    pub anom_limits_max_angular_velocity_per_psi: f32,
    /// `MATERIAL_FRICTION`, the one material's friction.
    pub material_friction: f64,
    /// `MATERIAL_ELASTICITY`, the one material's elasticity.
    pub material_elasticity: f64,
    /// `MIN_TIME_BETWEEN_COLLISIONS`: a pair's collisions closer than this
    /// make no collision event.
    pub min_time_between_collisions: f64,
    /// `RMGR_LOOK_AHEAD_TIME_WORLD`.
    pub rmgr_look_ahead_time_world: f32,
    /// `RMGR_LOOK_AHEAD_MAX_RADIUS_WORLD`.
    pub rmgr_look_ahead_max_radius_world: f32,
    /// `RMGR_LOOK_AHEAD_MIN_DISTANCE_WORLD`.
    pub rmgr_look_ahead_min_distance_world: f32,
    /// `RMGR_LOOK_AHEAD_MAX_DISTANCE_WORLD`.
    pub rmgr_look_ahead_max_distance_world: f32,
    /// `RMGR_LOOK_AHEAD_MIN_SECONDS_WORLD`: `2.1f / PHYSICAL_SIM_RATE` at
    /// start-up (a CRT initializer), not recomputed when the rate changes.
    pub rmgr_look_ahead_min_seconds_world: f32,
    /// `RMGR_LOOK_AHEAD_TIME_INTRA`.
    pub rmgr_look_ahead_time_intra: f32,
    /// `RMGR_LOOK_AHEAD_MAX_RADIUS_INTRA`.
    pub rmgr_look_ahead_max_radius_intra: f32,
    /// `RMGR_LOOK_AHEAD_MIN_DISTANCE_INTRA`.
    pub rmgr_look_ahead_min_distance_intra: f32,
    /// `RMGR_LOOK_AHEAD_MAX_DISTANCE_INTRA`.
    pub rmgr_look_ahead_max_distance_intra: f32,
    /// `RMGR_LOOK_AHEAD_MIN_SECONDS_INTRA`, as the world value.
    pub rmgr_look_ahead_min_seconds_intra: f32,
    /// `DEFAULT_LINEAR_DAMPING`.
    pub default_linear_damping: f32,
    /// `DEFAULT_ANGULAR_DAMPING`.
    pub default_angular_damping: Vector,
    /// `GOLEM_CHILD_MASS`.
    pub golem_child_mass: f32,
    /// `GOLEM_CHILD_LINEAR_DAMP`.
    pub golem_child_linear_damp: f32,
    /// `GOLEM_CHILD_ANGULAR_DAMP`.
    pub golem_child_angular_damp: Vector,
    /// `GOLEM_MAX_DELTA_POSITION`.
    pub golem_max_delta_position: f32,
    /// `GOLEM_DELTA_ORIENTATION`.
    pub golem_delta_orientation: f32,
    /// `GOLEM_MAX_TORQUE`.
    pub golem_max_torque: f32,
    /// `GOLEM_FORCE_FACTOR`.
    pub golem_force_factor: f32,
    /// `GOLEM_DAMP_FACTOR`.
    pub golem_damp_factor: f32,
    /// `GOLEM_TORQUE_FACTOR`.
    pub golem_torque_factor: f32,
    /// `GOLEM_ANGULAR_DAMP_FACTOR`.
    pub golem_angular_damp_factor: f32,
    /// `RC_MAX_DELTA_POSITION`.
    pub rc_max_delta_position: f32,
    /// `RC_MAX_DELTA_ORIENTATION`.
    pub rc_max_delta_orientation: f32,
    /// `GOLEM_MAX_TRANSLATION_FORCE`.
    pub golem_max_translation_force: Vector,
    /// `BULLET_LENGTH_FUDGE`: not read from the data.
    pub bullet_length_fudge: f32,
}

impl Default for PhySysConstants {
    fn default() -> Self {
        let rate = 16.0f32;
        let min_seconds = (f64::from(2.1f32) / f64::from(rate)) as f32;
        Self {
            physical_sim_rate: rate,
            max_spawned_mindist_count: 100,
            anom_limits_max_velocity: 1000.0,
            anom_limits_max_angular_velocity_per_psi: 0.942_477_9,
            material_friction: 0.1,
            material_elasticity: 0.9,
            min_time_between_collisions: 0.5,
            rmgr_look_ahead_time_world: 1.0,
            rmgr_look_ahead_max_radius_world: 5.0,
            rmgr_look_ahead_min_distance_world: 100.0,
            rmgr_look_ahead_max_distance_world: 1000.0,
            rmgr_look_ahead_min_seconds_world: min_seconds,
            rmgr_look_ahead_time_intra: 0.5,
            // 0.89999998 in the reference: the float nearest 0.9.
            rmgr_look_ahead_max_radius_intra: 0.9,
            rmgr_look_ahead_min_distance_intra: 5.0,
            rmgr_look_ahead_max_distance_intra: 50.0,
            rmgr_look_ahead_min_seconds_intra: min_seconds,
            default_linear_damping: 1.0,
            default_angular_damping: Vector::new(2.0, 2.0, 2.0),
            golem_child_mass: 100_000.0,
            golem_child_linear_damp: 1.0,
            golem_child_angular_damp: Vector::new(1.0, 1.0, 1.0),
            golem_max_delta_position: 10.0,
            golem_delta_orientation: 1.570_796_4,
            golem_max_torque: 1.0e12,
            golem_force_factor: 0.8,
            golem_damp_factor: 1.0,
            golem_torque_factor: 0.8,
            golem_angular_damp_factor: 1.0,
            rc_max_delta_position: 50.0,
            rc_max_delta_orientation: 5.0,
            golem_max_translation_force: Vector::new(1.0e12, 1.0e12, 1.0e12),
            bullet_length_fudge: 0.0,
        }
    }
}

impl PhySysConstants {
    /// The `float` key a name sets, in `ReadConstants`' table.
    fn float_key(&mut self, name: &[u8]) -> Option<&mut f32> {
        let key: &mut f32 = match name.to_ascii_uppercase().as_slice() {
            b"PHYSICAL_SIM_RATE" => &mut self.physical_sim_rate,
            b"ANOM_LIMITS_MAX_VELOCITY" => &mut self.anom_limits_max_velocity,
            b"ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI" => {
                &mut self.anom_limits_max_angular_velocity_per_psi
            }
            b"RMGR_LOOK_AHEAD_TIME_WORLD" => &mut self.rmgr_look_ahead_time_world,
            b"RMGR_LOOK_AHEAD_MAX_RADIUS_WORLD" => &mut self.rmgr_look_ahead_max_radius_world,
            b"RMGR_LOOK_AHEAD_MIN_DISTANCE_WORLD" => &mut self.rmgr_look_ahead_min_distance_world,
            b"RMGR_LOOK_AHEAD_MAX_DISTANCE_WORLD" => &mut self.rmgr_look_ahead_max_distance_world,
            b"RMGR_LOOK_AHEAD_MIN_SECONDS_WORLD" => &mut self.rmgr_look_ahead_min_seconds_world,
            b"RMGR_LOOK_AHEAD_TIME_INTRA" => &mut self.rmgr_look_ahead_time_intra,
            b"RMGR_LOOK_AHEAD_MAX_RADIUS_INTRA" => &mut self.rmgr_look_ahead_max_radius_intra,
            b"RMGR_LOOK_AHEAD_MIN_DISTANCE_INTRA" => &mut self.rmgr_look_ahead_min_distance_intra,
            b"RMGR_LOOK_AHEAD_MAX_DISTANCE_INTRA" => &mut self.rmgr_look_ahead_max_distance_intra,
            b"RMGR_LOOK_AHEAD_MIN_SECONDS_INTRA" => &mut self.rmgr_look_ahead_min_seconds_intra,
            b"DEFAULT_LINEAR_DAMPING" => &mut self.default_linear_damping,
            b"GOLEM_CHILD_MASS" => &mut self.golem_child_mass,
            b"GOLEM_CHILD_LINEAR_DAMP" => &mut self.golem_child_linear_damp,
            b"GOLEM_MAX_DELTA_POSITION" => &mut self.golem_max_delta_position,
            b"GOLEM_DELTA_ORIENTATION" => &mut self.golem_delta_orientation,
            b"GOLEM_MAX_TORQUE" => &mut self.golem_max_torque,
            b"GOLEM_FORCE_FACTOR" => &mut self.golem_force_factor,
            b"GOLEM_DAMP_FACTOR" => &mut self.golem_damp_factor,
            b"GOLEM_TORQUE_FACTOR" => &mut self.golem_torque_factor,
            b"GOLEM_ANGULAR_DAMP_FACTOR" => &mut self.golem_angular_damp_factor,
            b"RC_MAX_DELTA_POSITION" => &mut self.rc_max_delta_position,
            b"RC_MAX_DELTA_ORIENTATION" => &mut self.rc_max_delta_orientation,
            _ => return None,
        };
        Some(key)
    }

    /// Reads the values of a `[PhySysConsts]` section whose header the
    /// reader has just read, as `ReadConstants` does: three vector keys,
    /// then the `float` keys (`get_value_float(0)` stored as `float`), the
    /// `double` keys (the unrounded value) and the `int` key (the unrounded
    /// value truncated). Names compare without case, as `is_value` does.
    /// Returns the names the reference reports as unknown (`DumpUnknownConstant`).
    pub fn read_section(&mut self, reader: &mut IniReader) -> Vec<Vec<u8>> {
        let mut unknown = Vec::new();
        while reader.read_value() {
            let vector_key = if reader.is_value(b"DEFAULT_ANGULAR_DAMPING") {
                Some(&mut self.default_angular_damping)
            } else if reader.is_value(b"GOLEM_CHILD_ANGULAR_DAMP") {
                Some(&mut self.golem_child_angular_damp)
            } else if reader.is_value(b"GOLEM_MAX_TRANSLATION_FORCE") {
                Some(&mut self.golem_max_translation_force)
            } else {
                None
            };
            if let Some(target) = vector_key {
                target.x = reader.value_float(0) as f32;
                target.y = reader.value_float(1) as f32;
                target.z = reader.value_float(2) as f32;
                continue;
            }
            let name = reader.name();
            if let Some(target) = self.float_key(&name) {
                *target = reader.value_float(0) as f32;
                continue;
            }
            let double_key = if reader.is_value(b"MATERIAL_FRICTION") {
                Some(&mut self.material_friction)
            } else if reader.is_value(b"MATERIAL_ELASTICITY") {
                Some(&mut self.material_elasticity)
            } else if reader.is_value(b"MIN_TIME_BETWEEN_COLLISIONS") {
                Some(&mut self.min_time_between_collisions)
            } else {
                None
            };
            if let Some(target) = double_key {
                *target = reader.value_float(0);
                continue;
            }
            if reader.is_value(b"MAX_SPAWNED_MINDIST_COUNT") {
                self.max_spawned_mindist_count = crate::num::trunc_to_i32(reader.value_float(0));
                continue;
            }
            unknown.push(name);
        }
        unknown
    }

    /// The anomaly limits a new environment gets.
    #[must_use]
    pub fn anomaly_limits(&self) -> super::ivp::psi::AnomalyLimits {
        super::ivp::psi::AnomalyLimits {
            max_velocity: self.anom_limits_max_velocity,
            max_angular_velocity_per_psi: self.anom_limits_max_angular_velocity_per_psi,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_the_compiled_values() {
        let c = PhySysConstants::default();
        assert_eq!(c.physical_sim_rate, 16.0);
        assert_eq!(c.anom_limits_max_angular_velocity_per_psi, 0.942_477_9);
        assert_eq!(
            c.rmgr_look_ahead_min_seconds_world,
            (2.1f64 as f32 as f64 / 16.0) as f32
        );
        assert_eq!(c.golem_delta_orientation.to_bits(), 0x3fc9_0fdb);
        assert_eq!(c.golem_max_translation_force.x.to_bits(), 0x5368_d4a5);
        assert_eq!(c.rmgr_look_ahead_max_radius_intra, 0.9);
    }

    #[test]
    fn section_as_shipped() {
        let text = b"[PhySysConsts]\nMATERIAL_FRICTION = 0.100000001\nMATERIAL_ELASTICITY = 0.899999976\n\
DEFAULT_LINEAR_DAMPING = 0.5\nDEFAULT_ANGULAR_DAMPING = 0.200000003, 0.200000003, 0.200000003\n\
physical_sim_rate = 20\nMAX_SPAWNED_MINDIST_COUNT = 7.9\nGOLEM_MAX_TRANSLATION_FORCE = 1, 2, 3\nBOGUS = 1\n";
        let mut reader = IniReader::new();
        assert!(reader.open_memory(text));
        assert!(reader.read_header());
        assert!(reader.is_header(b"PhySysConsts"));
        let mut c = PhySysConstants::default();
        let unknown = c.read_section(&mut reader);
        assert_eq!(unknown, vec![b"BOGUS".to_vec()]);
        assert_eq!(c.material_friction, 0.100_000_001);
        assert_eq!(c.material_elasticity, 0.899_999_976);
        assert_eq!(c.default_linear_damping, 0.5);
        assert_eq!(c.default_angular_damping, Vector::new(0.2, 0.2, 0.2));
        assert_eq!(c.physical_sim_rate, 20.0);
        assert_eq!(c.max_spawned_mindist_count, 7);
        assert_eq!(c.golem_max_translation_force, Vector::new(1.0, 2.0, 3.0));
        assert_eq!(
            c.rmgr_look_ahead_min_seconds_world,
            PhySysConstants::default().rmgr_look_ahead_min_seconds_world,
            "the derived default is not recomputed"
        );
        let limits = c.anomaly_limits();
        assert_eq!(limits.max_velocity, 1000.0);
    }
}
