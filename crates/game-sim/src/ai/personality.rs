//! The pilot personality the behaviours read (`pub::AI::Personality`,
//! `include/common_pub.h`, constructors in `Common/common_pub.cpp`).
//!
//! The structure belongs to `Common`; the loader that fills it from the
//! pilot INI files is Content's (`Content/PilotPersonalities.cpp`) and
//! ports with the NPC pilots in `game-logic` (P3.6). Field names are the
//! INI keys that loader stores at each offset; angles are radians.
//!
//! [`Personality::to_bytes`] and [`Personality::from_bytes`] keep the
//! reference's 0x3D8-byte layout (little-endian, padding zero), which the
//! oracle compares and the behaviour-tick recorder writes.

/// A plain field of the layout.
trait Field: Sized {
    fn put(&self, out: &mut [u8]);
    fn get(bytes: &[u8]) -> Self;
}

impl Field for f32 {
    fn put(&self, out: &mut [u8]) {
        out[..4].copy_from_slice(&self.to_le_bytes());
    }
    fn get(bytes: &[u8]) -> Self {
        f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
    }
}

impl Field for i32 {
    fn put(&self, out: &mut [u8]) {
        out[..4].copy_from_slice(&self.to_le_bytes());
    }
    fn get(bytes: &[u8]) -> Self {
        i32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
    }
}

impl Field for u32 {
    fn put(&self, out: &mut [u8]) {
        out[..4].copy_from_slice(&self.to_le_bytes());
    }
    fn get(bytes: &[u8]) -> Self {
        u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
    }
}

impl Field for u8 {
    fn put(&self, out: &mut [u8]) {
        out[0] = *self;
    }
    fn get(bytes: &[u8]) -> Self {
        bytes[0]
    }
}

/// A sub-structure of fixed size whose fields sit at fixed offsets.
macro_rules! record {
    ($(#[$m:meta])* $name:ident, $size:expr, {
        $($(#[$fm:meta])* $field:ident : $ty:ty = $off:expr, $def:expr;)*
    }) => {
        $(#[$m])*
        #[derive(Clone, Copy, Debug, PartialEq)]
        pub struct $name {
            $($(#[$fm])* pub $field: $ty,)*
        }

        impl Default for $name {
            /// The reference's constructor.
            fn default() -> Self {
                Self { $($field: $def,)* }
            }
        }

        impl $name {
            /// The size in bytes.
            pub const SIZE: usize = $size;

            fn write(&self, out: &mut [u8]) {
                $(Field::put(&self.$field, &mut out[$off..]);)*
            }

            fn read(bytes: &[u8]) -> Self {
                Self { $($field: Field::get(&bytes[$off..]),)* }
            }
        }
    };
}

/// π/4 as the constructors store it.
const QUARTER_PI: f32 = core::f32::consts::FRAC_PI_4;
/// π/6.
const SIXTH_PI: f32 = core::f32::consts::FRAC_PI_6;
/// π/8.
const EIGHTH_PI: f32 = core::f32::consts::FRAC_PI_8;

record!(
    /// `EvadeDodgeUseStruct`, `[EvadeDodgeBlock]`.
    EvadeDodge, 0x58, {
    /// `evade_activate_range`.
    evade_activate_range: f32 = 0x00, 500.0;
    /// `evade_dodge_style_weight`, `WAGGLE`.
    style_weight_waggle: f32 = 0x04, 0.0;
    /// `evade_dodge_style_weight`, `WAGGLE_RANDOM`.
    style_weight_waggle_random: f32 = 0x08, 0.0;
    /// `evade_dodge_style_weight`, `SLIDE`.
    style_weight_slide: f32 = 0x0c, 0.0;
    /// `evade_dodge_style_weight`, `CORKSCREW`.
    style_weight_corkscrew: f32 = 0x10, 0.0;
    /// `evade_dodge_cone_angle`.
    cone_angle: f32 = 0x14, QUARTER_PI;
    /// `evade_dodge_cone_angle_variance_percent`.
    cone_angle_variance_percent: f32 = 0x18, 0.0;
    /// `evade_dodge_waggle_axis_cone_angle`.
    waggle_axis_cone_angle: f32 = 0x1c, 0.0;
    /// `evade_dodge_roll_angle`.
    roll_angle: f32 = 0x20, 0.0;
    /// `evade_dodge_interval_time`.
    interval_time: f32 = 0x24, 1.0;
    /// `evade_dodge_interval_time_variance_percent`.
    interval_time_variance_percent: f32 = 0x28, 0.0;
    /// `evade_dodge_distance`.
    distance: f32 = 0x2c, 100.0;
    /// `evade_dodge_time`.
    time: f32 = 0x30, 10.0;
    /// `evade_dodge_slide_throttle`.
    slide_throttle: f32 = 0x34, 0.6;
    /// `evade_dodge_turn_throttle`.
    turn_throttle: f32 = 0x38, 0.6;
    /// `evade_dodge_corkscrew_turn_throttle`.
    corkscrew_turn_throttle: f32 = 0x3c, 0.15;
    /// `evade_dodge_corkscrew_roll_throttle`.
    corkscrew_roll_throttle: f32 = 0x40, 1.0;
    /// `evade_dodge_corkscrew_roll_flip_direction` (a byte).
    corkscrew_roll_flip_direction: u8 = 0x44, 0;
    /// `evade_dodge_direction_weight`, `LEFT`.
    direction_weight_left: f32 = 0x48, 0.0;
    /// `evade_dodge_direction_weight`, `RIGHT`.
    direction_weight_right: f32 = 0x4c, 0.0;
    /// `evade_dodge_direction_weight`, `UP`.
    direction_weight_up: f32 = 0x50, 0.0;
    /// `evade_dodge_direction_weight`, `DOWN`.
    direction_weight_down: f32 = 0x54, 0.0;
});

record!(
    /// `EvadeBreakUseStruct`, `[EvadeBreakBlock]`.
    EvadeBreak, 0x3c, {
    /// `evade_break_time`.
    time: f32 = 0x00, 3.0;
    /// `evade_break_interval_time`.
    interval_time: f32 = 0x04, 100_000.0;
    /// `evade_break_afterburner_delay`.
    afterburner_delay: f32 = 0x08, 0.0;
    /// `evade_break_afterburner_delay_variance_percent`.
    afterburner_delay_variance_percent: f32 = 0x0c, 0.0;
    /// `evade_break_direction_weight`, `LEFT`.
    direction_weight_left: f32 = 0x10, 0.0;
    /// `evade_break_direction_weight`, `RIGHT`.
    direction_weight_right: f32 = 0x14, 0.0;
    /// `evade_break_direction_weight`, `UP`.
    direction_weight_up: f32 = 0x18, 0.0;
    /// `evade_break_direction_weight`, `DOWN`.
    direction_weight_down: f32 = 0x1c, 0.0;
    /// `evade_break_roll_throttle`.
    roll_throttle: f32 = 0x20, 0.0;
    /// `evade_break_turn_throttle`.
    turn_throttle: f32 = 0x24, 1.0;
    /// `evade_break_style_weight`, `SIDEWAYS`.
    style_weight_sideways: f32 = 0x28, 0.0;
    /// `evade_break_style_weight`, `OUTRUN`.
    style_weight_outrun: f32 = 0x2c, 0.0;
    /// `evade_break_style_weight`, `REVERSE`.
    style_weight_reverse: f32 = 0x30, 0.0;
    /// `evade_break_attempt_reverse_time`.
    attempt_reverse_time: f32 = 0x34, 4.0;
    /// `evade_break_reverse_distance`.
    reverse_distance: f32 = 0x38, 150.0;
});

record!(
    /// `BuzzHeadTowardUseStruct`, `[BuzzHeadTowardBlock]`.
    BuzzHeadToward, 0x60, {
    /// `buzz_max_time_to_head_away`.
    max_time_to_head_away: f32 = 0x00, 4.0;
    /// `buzz_head_toward_style_weight`, `STRAIGHT_TO`.
    style_weight_straight_to: f32 = 0x04, 0.0;
    /// `buzz_head_toward_style_weight`, `SLIDE`.
    style_weight_slide: f32 = 0x08, 0.0;
    /// `buzz_head_toward_style_weight`, `WAGGLE`.
    style_weight_waggle: f32 = 0x0c, 0.0;
    /// `buzz_min_distance_to_head_toward`.
    min_distance_to_head_toward: f32 = 0x10, 200.0;
    /// `buzz_min_distance_to_head_toward_variance_percent`.
    min_distance_to_head_toward_variance_percent: f32 = 0x14, 1.0;
    /// `buzz_head_toward_engine_throttle`.
    engine_throttle: f32 = 0x18, 1.0;
    /// `buzz_head_toward_turn_throttle`.
    turn_throttle: f32 = 0x1c, 1.0;
    /// `buzz_head_toward_roll_throttle`.
    roll_throttle: f32 = 0x20, 0.0;
    /// `buzz_head_toward_roll_flip_direction` (a byte).
    roll_flip_direction: u8 = 0x24, 0;
    /// `buzz_dodge_direction_weight`, `LEFT`.
    dodge_direction_weight_left: f32 = 0x28, 0.0;
    /// `buzz_dodge_direction_weight`, `RIGHT`.
    dodge_direction_weight_right: f32 = 0x2c, 0.0;
    /// `buzz_dodge_direction_weight`, `UP`.
    dodge_direction_weight_up: f32 = 0x30, 0.0;
    /// `buzz_dodge_direction_weight`, `DOWN`.
    dodge_direction_weight_down: f32 = 0x34, 0.0;
    /// `buzz_dodge_turn_throttle`.
    dodge_turn_throttle: f32 = 0x38, 1.0;
    /// `buzz_dodge_cone_angle`.
    dodge_cone_angle: f32 = 0x3c, QUARTER_PI;
    /// `buzz_dodge_cone_angle_variance_percent`.
    dodge_cone_angle_variance_percent: f32 = 0x40, 0.0;
    /// `buzz_dodge_waggle_axis_cone_angle`.
    dodge_waggle_axis_cone_angle: f32 = 0x44, 0.0;
    /// `buzz_dodge_roll_angle`.
    dodge_roll_angle: f32 = 0x48, 0.0;
    /// `buzz_dodge_interval_time`.
    dodge_interval_time: f32 = 0x4c, 2.0;
    /// `buzz_dodge_interval_time_variance_percent`.
    dodge_interval_time_variance_percent: f32 = 0x50, 0.0;
    /// `buzz_slide_throttle`.
    slide_throttle: f32 = 0x54, 1.0;
    /// `buzz_slide_interval_time`.
    slide_interval_time: f32 = 0x58, 2.0;
    /// `buzz_slide_interval_time_variance_percent`.
    slide_interval_time_variance_percent: f32 = 0x5c, 0.4;
});

record!(
    /// `BuzzPassByUseStruct`, `[BuzzPassByBlock]`.
    BuzzPassBy, 0x34, {
    /// `buzz_pass_by_style_weight`, `STRAIGHT_BY`.
    style_weight_straight_by: f32 = 0x00, 0.0;
    /// `buzz_pass_by_style_weight`, `BREAK_AWAY`.
    style_weight_break_away: f32 = 0x04, 0.0;
    /// `buzz_pass_by_style_weight`, `ENGINE_KILL`.
    style_weight_engine_kill: f32 = 0x08, 0.0;
    /// `buzz_distance_to_pass_by`.
    distance_to_pass_by: f32 = 0x0c, 100.0;
    /// `buzz_pass_by_time`.
    time: f32 = 0x10, 3.0;
    /// `buzz_drop_bomb_on_pass_by` (a byte).
    drop_bomb_on_pass_by: u8 = 0x14, 0;
    /// `buzz_break_direction_weight`, `LEFT`.
    break_direction_weight_left: f32 = 0x18, 0.0;
    /// `buzz_break_direction_weight`, `RIGHT`.
    break_direction_weight_right: f32 = 0x1c, 0.0;
    /// `buzz_break_direction_weight`, `UP`.
    break_direction_weight_up: f32 = 0x20, 0.0;
    /// `buzz_break_direction_weight`, `DOWN`.
    break_direction_weight_down: f32 = 0x24, 0.0;
    /// `buzz_break_direction_cone_angle`, the INI value as it is (no
    /// conversion to radians).
    break_direction_cone_angle: f32 = 0x28, 0.0;
    /// `buzz_break_turn_throttle`.
    break_turn_throttle: f32 = 0x2c, 1.0;
    /// `buzz_pass_by_roll_throttle`.
    roll_throttle: f32 = 0x30, 0.0;
});

record!(
    /// `TrailUseStruct`, `[TrailBlock]`.
    Trail, 0x1c, {
    /// `trail_lock_cone_angle`.
    lock_cone_angle: f32 = 0x00, SIXTH_PI;
    /// `trail_break_time`.
    break_time: f32 = 0x04, 2.5;
    /// `trail_min_no_lock_time`.
    min_no_lock_time: f32 = 0x08, 4.0;
    /// `trail_break_roll_throttle`.
    break_roll_throttle: f32 = 0x0c, 0.0;
    /// `trail_break_afterburner` (a byte).
    break_afterburner: u8 = 0x10, 0;
    /// `trail_max_turn_throttle`.
    max_turn_throttle: f32 = 0x14, 1.0;
    /// `trail_distance`.
    distance: f32 = 0x18, 150.0;
});

record!(
    /// `StrafeUseStruct`, `[StrafeBlock]`.
    Strafe, 0x0c, {
    /// `strafe_run_away_distance`.
    run_away_distance: f32 = 0x00, 150.0;
    /// `strafe_attack_throttle`.
    attack_throttle: f32 = 0x04, 1.0;
    /// `strafe_turn_throttle`.
    turn_throttle: f32 = 0x08, 1.0;
});

record!(
    /// `EngineKillUseStruct`, `[EngineKillBlock]`.
    EngineKill, 0x14, {
    /// `engine_kill_search_time`.
    search_time: f32 = 0x00, 6.0;
    /// `engine_kill_face_time`.
    face_time: f32 = 0x04, 3.0;
    /// `engine_kill_use_afterburner`: TRUE/FALSE as 1.0/0.0.
    use_afterburner: f32 = 0x08, 0.0;
    /// `engine_kill_afterburner_time`.
    afterburner_time: f32 = 0x0c, 0.5;
    /// `engine_kill_max_target_distance`.
    max_target_distance: f32 = 0x10, 700.0;
});

record!(
    /// `RepairUseStruct`, `[RepairBlock]`.
    Repair, 0x18, {
    /// `use_shield_repair_pre_delay`.
    shield_pre_delay: f32 = 0x00, 2.0;
    /// `use_shield_repair_at_damage_percent`.
    shield_at_damage_percent: f32 = 0x04, 0.5;
    /// `use_shield_repair_post_delay`.
    shield_post_delay: f32 = 0x08, 3.0;
    /// `use_hull_repair_pre_delay`.
    hull_pre_delay: f32 = 0x0c, 1.0;
    /// `use_hull_repair_at_damage_percent`.
    hull_at_damage_percent: f32 = 0x10, 0.5;
    /// `use_hull_repair_post_delay`.
    hull_post_delay: f32 = 0x14, 1.0;
});

record!(
    /// `GunUseStruct`, `[GunBlock]`.
    Gun, 0x40, {
    /// `fire_style`: 0 `MULTIPLE`, 1 anything else.
    fire_style: i32 = 0x00, 0;
    /// `gun_fire_interval_time`.
    fire_interval_time: f32 = 0x04, 0.0;
    /// `gun_fire_interval_variance_percent`.
    fire_interval_variance_percent: f32 = 0x08, 0.5;
    /// `gun_fire_burst_interval_time`.
    fire_burst_interval_time: f32 = 0x0c, 1.5;
    /// `gun_fire_burst_interval_variance_percent`.
    fire_burst_interval_variance_percent: f32 = 0x10, 0.1;
    /// `gun_fire_no_burst_interval_time`.
    fire_no_burst_interval_time: f32 = 0x14, 0.1;
    /// `gun_fire_accuracy_cone_angle`.
    fire_accuracy_cone_angle: f32 = 0x18, 0.0;
    /// `gun_fire_accuracy_power`.
    fire_accuracy_power: f32 = 0x1c, 1.0;
    /// `gun_fire_accuracy_power_npc`.
    fire_accuracy_power_npc: f32 = 0x20, 6.0;
    /// `gun_range_threshold`.
    range_threshold: f32 = 0x24, 1.0;
    /// `gun_range_threshold_variance_percent`.
    range_threshold_variance_percent: f32 = 0x28, 0.2;
    /// `gun_target_point_switch_time`.
    target_point_switch_time: f32 = 0x2c, 1.5;
    /// `auto_turret_interval_time`.
    auto_turret_interval_time: f32 = 0x30, 0.0;
    /// `auto_turret_burst_interval_time`.
    auto_turret_burst_interval_time: f32 = 0x34, 1.0;
    /// `auto_turret_no_burst_interval_time`.
    auto_turret_no_burst_interval_time: f32 = 0x38, 0.0;
    /// `auto_turret_burst_interval_variance_percent`.
    auto_turret_burst_interval_variance_percent: f32 = 0x3c, 0.1;
});

record!(
    /// `MineUseStruct`, `[MineBlock]`.
    Mine, 0x0c, {
    /// `mine_launch_interval`.
    launch_interval: f32 = 0x00, 0.0;
    /// `mine_launch_cone_angle`.
    launch_cone_angle: f32 = 0x04, EIGHTH_PI;
    /// `mine_launch_range`.
    launch_range: f32 = 0x08, 400.0;
});

record!(
    /// `MissileUseStruct`, `[MissileBlock]`.
    Missile, 0x24, {
    /// `missile_launch_range`.
    launch_range: f32 = 0x00, -1.0;
    /// `missile_launch_allow_out_of_range` (a byte).
    launch_allow_out_of_range: u8 = 0x04, 0;
    /// `missile_launch_interval_time`.
    launch_interval_time: f32 = 0x08, 5.0;
    /// `missile_launch_interval_variance_percent`.
    launch_interval_variance_percent: f32 = 0x0c, 0.4;
    /// `missile_launch_cone_angle`.
    launch_cone_angle: f32 = 0x10, EIGHTH_PI;
    /// `anti_cruise_missile_min_distance`.
    anti_cruise_min_distance: f32 = 0x14, 150.0;
    /// `anti_cruise_missile_max_distance`.
    anti_cruise_max_distance: f32 = 0x18, 2500.0;
    /// `anti_cruise_missile_pre_fire_delay`.
    anti_cruise_pre_fire_delay: f32 = 0x1c, 2.0;
    /// `anti_cruise_missile_interval_time`.
    anti_cruise_interval_time: f32 = 0x20, 5.0;
});

record!(
    /// `DamageReactionStruct`, `[DamageReactionBlock]`. The last two
    /// dwords are not written by the reference's constructor (the C++
    /// port zeroes them) and no key writes them.
    DamageReaction, 0x44, {
    /// `evade_break_damage_trigger_percent`.
    evade_break_trigger_percent: f32 = 0x00, 1.0;
    /// `evade_dodge_more_damage_trigger_percent`.
    evade_dodge_more_trigger_percent: f32 = 0x04, 0.2;
    /// `drop_mines_damage_trigger_percent`.
    drop_mines_trigger_percent: f32 = 0x08, 1.0;
    /// `drop_mines_damage_trigger_time`.
    drop_mines_trigger_time: f32 = 0x0c, 1.0;
    /// `engine_kill_face_damage_trigger_percent`.
    engine_kill_face_trigger_percent: f32 = 0x10, 1.0;
    /// `engine_kill_face_damage_trigger_time`.
    engine_kill_face_trigger_time: f32 = 0x14, 2.0;
    /// `roll_damage_trigger_percent`.
    roll_trigger_percent: f32 = 0x18, 1.0;
    /// `roll_damage_trigger_time`.
    roll_trigger_time: f32 = 0x1c, 1.0;
    /// `afterburner_damage_trigger_percent`.
    afterburner_trigger_percent: f32 = 0x20, 1.0;
    /// `afterburner_damage_trigger_time`.
    afterburner_trigger_time: f32 = 0x24, 1.5;
    /// `brake_reverse_damage_trigger_percent`.
    brake_reverse_trigger_percent: f32 = 0x28, 1.0;
    /// `fire_missiles_damage_trigger_percent`.
    fire_missiles_trigger_percent: f32 = 0x2c, 1.0;
    /// `fire_missiles_damage_trigger_time`.
    fire_missiles_trigger_time: f32 = 0x30, 0.5;
    /// `fire_guns_damage_trigger_percent`.
    fire_guns_trigger_percent: f32 = 0x34, 1.0;
    /// `fire_guns_damage_trigger_time`.
    fire_guns_trigger_time: f32 = 0x38, 1.0;
    /// +0x3c, meaning unknown.
    unknown_3c: f32 = 0x3c, 0.0;
    /// +0x40, meaning unknown.
    unknown_40: f32 = 0x40, 0.0;
});

record!(
    /// `MissileReactionStruct`, `[MissileReactionBlock]`.
    MissileReaction, 0x10, {
    /// `evade_break_missile_reaction_time`.
    evade_break_time: f32 = 0x00, 0.0;
    /// `evade_slide_missile_reaction_time`.
    evade_slide_time: f32 = 0x04, 0.0;
    /// `evade_afterburn_missile_reaction_time`.
    evade_afterburn_time: f32 = 0x08, 0.0;
    /// `evade_missile_distance`.
    evade_distance: f32 = 0x0c, 250.0;
});

record!(
    /// `CountermeasureUseStruct`, `[CountermeasureBlock]`.
    Countermeasure, 0x08, {
    /// `countermeasure_active_time`.
    active_time: f32 = 0x00, 1.0;
    /// `countermeasure_unactive_time`.
    unactive_time: f32 = 0x04, 0.0;
});

record!(
    /// `FormationUseStruct`, `[FormationBlock]`.
    Formation, 0x58, {
    /// `force_attack_formation_active_time`.
    force_attack_active_time: f32 = 0x00, 1.0;
    /// `force_attack_formation_unactive_time`.
    force_attack_unactive_time: f32 = 0x04, 0.0;
    /// `break_formation_damage_trigger_percent`.
    break_damage_trigger_percent: f32 = 0x08, 1.0;
    /// `break_formation_damage_trigger_time`.
    break_damage_trigger_time: f32 = 0x0c, 4.0;
    /// `break_apart_formation_damage_trigger_percent`.
    break_apart_damage_trigger_percent: f32 = 0x10, 1.0;
    /// `break_apart_formation_damage_trigger_time`.
    break_apart_damage_trigger_time: f32 = 0x14, 4.0;
    /// `break_formation_missile_reaction_time`.
    break_missile_reaction_time: f32 = 0x18, 0.0;
    /// `break_apart_formation_missile_reaction_time`.
    break_apart_missile_reaction_time: f32 = 0x1c, 4.0;
    /// `break_apart_formation_on_buzz_head_toward` (a byte).
    break_apart_on_buzz_head_toward: u8 = 0x20, 0;
    /// `break_formation_on_buzz_head_toward_time`.
    break_on_buzz_head_toward_time: f32 = 0x24, 4.0;
    /// `regroup_formation_on_buzz_head_toward` (a byte).
    regroup_on_buzz_head_toward: u8 = 0x28, 0;
    /// `break_apart_formation_on_buzz_pass_by` (a byte).
    break_apart_on_buzz_pass_by: u8 = 0x29, 0;
    /// `break_formation_on_buzz_pass_by_time`.
    break_on_buzz_pass_by_time: f32 = 0x2c, 4.0;
    /// `regroup_formation_on_buzz_pass_by` (a byte).
    regroup_on_buzz_pass_by: u8 = 0x30, 0;
    /// `break_apart_formation_on_evade_dodge` (a byte).
    break_apart_on_evade_dodge: u8 = 0x31, 0;
    /// `break_formation_on_evade_dodge_time`.
    break_on_evade_dodge_time: f32 = 0x34, 4.0;
    /// `regroup_formation_on_evade_dodge` (a byte).
    regroup_on_evade_dodge: u8 = 0x38, 0;
    /// `break_apart_formation_on_evade_break` (a byte).
    break_apart_on_evade_break: u8 = 0x39, 0;
    /// `break_formation_on_evade_break_time`.
    break_on_evade_break_time: f32 = 0x3c, 4.0;
    /// `regroup_formation_on_evade_break` (a byte).
    regroup_on_evade_break: u8 = 0x40, 0;
    /// `formation_exit_mode`: 0 break away from centre, 1 with
    /// afterburner, 2 brake and reverse, 3 outrun, 5 nothing.
    exit_mode: i32 = 0x44, 5;
    /// `formation_exit_top_turn_break_away_throttle`.
    exit_top_turn_break_away_throttle: f32 = 0x48, 1.0;
    /// `formation_exit_roll_outrun_throttle`.
    exit_roll_outrun_throttle: f32 = 0x4c, 0.0;
    /// `formation_exit_max_time`.
    exit_max_time: f32 = 0x50, 0.0;
    /// `leader_makes_me_tougher` (a byte).
    leader_makes_me_tougher: u8 = 0x54, 0;
});

/// One `attack_preference` / `attack_order` record of the job block.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AttackPreference {
    /// The ship class (11 anything; 12 ends the list).
    pub ship_class: i32,
    /// The range.
    pub range: f32,
    /// The weapon types: 1 guns, 2 guided, 4 unguided, 8 torpedo.
    pub weapon_flags: u32,
}

/// `JobStruct`, `[JobBlock]` (0x138 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Job {
    /// +0x000, read by `get_range_threshold_value`; no key writes it.
    pub unknown_00: i32,
    /// +0x004 `scene_toughness_threshold` (0 easiest to 4 hardest).
    pub scene_toughness_threshold: i32,
    /// +0x008 `target_toughness_preference`.
    pub target_toughness_preference: i32,
    /// +0x00c, no key writes it.
    pub unknown_0c: f32,
    /// +0x010 twelve records and the terminator.
    pub attack_preferences: [AttackPreference; 13],
    /// +0x0ac `attack_subtarget_order` (0 guns to 5 hull, 6 anything;
    /// 7 ends the list).
    pub attack_subtarget_order: [i32; 8],
    /// +0x0cc `wait_for_leader_target` (a byte; not set by the
    /// constructor, zero in the C++ port).
    pub wait_for_leader_target: u8,
    /// +0x0d0 `maximum_leader_target_distance`.
    pub maximum_leader_target_distance: f32,
    /// +0x0d4 `field_targeting`: never, low density, high density, always.
    pub field_targeting: i32,
    /// +0x0d8 `loot_preference` (1 commodities, 2 equipment, 4 potions).
    pub loot_preference: i32,
    /// +0x0dc `loot_flee_threshold`.
    pub loot_flee_threshold: i32,
    /// +0x0e0 the number of `scan_for_cargo` ids.
    pub scan_for_cargo_count: i32,
    /// +0x0e4 the faction's `scan_for_cargo` archetype ids.
    pub scan_for_cargo: [u32; 16],
    /// +0x124 `flee_scene_threat_style`.
    pub flee_scene_threat_style: i32,
    /// +0x128 `flee_when_hull_damaged_percent`.
    pub flee_when_hull_damaged_percent: f32,
    /// +0x12c `flee_when_leader_flees_style` (a byte).
    pub flee_when_leader_flees_style: u8,
    /// +0x12d `flee_no_weapons_style` (a byte).
    pub flee_no_weapons_style: u8,
    /// +0x12e `allow_player_targeting` (a byte).
    pub allow_player_targeting: u8,
    /// +0x130 the scan chance, [`Personality::SCAN_CHANCE_DEFAULT`] when
    /// not set.
    pub scan_chance: f32,
    /// +0x134 `force_attack_formation` (a byte).
    pub force_attack_formation: u8,
    /// +0x135 set with that key (a byte).
    pub force_attack_formation_set: u8,
}

impl Default for Job {
    /// The reference's constructor.
    fn default() -> Self {
        let mut attack_preferences = [AttackPreference::default(); 13];
        attack_preferences[0] = AttackPreference {
            ship_class: 11,
            range: 5000.0,
            weapon_flags: 0xf,
        };
        attack_preferences[1].ship_class = 12;
        let mut attack_subtarget_order = [0; 8];
        attack_subtarget_order[0] = 6;
        attack_subtarget_order[1] = 7;
        Self {
            unknown_00: 0,
            scene_toughness_threshold: 2,
            target_toughness_preference: 2,
            unknown_0c: 10_000.0,
            attack_preferences,
            attack_subtarget_order,
            wait_for_leader_target: 0,
            maximum_leader_target_distance: 5000.0,
            field_targeting: 3,
            loot_preference: 0,
            loot_flee_threshold: 2,
            scan_for_cargo_count: 0,
            scan_for_cargo: [0; 16],
            flee_scene_threat_style: 3,
            flee_when_hull_damaged_percent: 0.0,
            flee_when_leader_flees_style: 1,
            flee_no_weapons_style: 1,
            allow_player_targeting: 1,
            scan_chance: Personality::SCAN_CHANCE_DEFAULT,
            force_attack_formation: 0,
            force_attack_formation_set: 0,
        }
    }
}

impl Job {
    /// The size in bytes.
    pub const SIZE: usize = 0x138;

    fn write(&self, out: &mut [u8]) {
        self.unknown_00.put(&mut out[0x00..]);
        self.scene_toughness_threshold.put(&mut out[0x04..]);
        self.target_toughness_preference.put(&mut out[0x08..]);
        self.unknown_0c.put(&mut out[0x0c..]);
        for (i, p) in self.attack_preferences.iter().enumerate() {
            let at = 0x10 + i * 0x0c;
            p.ship_class.put(&mut out[at..]);
            p.range.put(&mut out[at + 4..]);
            p.weapon_flags.put(&mut out[at + 8..]);
        }
        for (i, v) in self.attack_subtarget_order.iter().enumerate() {
            v.put(&mut out[0xac + i * 4..]);
        }
        self.wait_for_leader_target.put(&mut out[0xcc..]);
        self.maximum_leader_target_distance.put(&mut out[0xd0..]);
        self.field_targeting.put(&mut out[0xd4..]);
        self.loot_preference.put(&mut out[0xd8..]);
        self.loot_flee_threshold.put(&mut out[0xdc..]);
        self.scan_for_cargo_count.put(&mut out[0xe0..]);
        for (i, v) in self.scan_for_cargo.iter().enumerate() {
            v.put(&mut out[0xe4 + i * 4..]);
        }
        self.flee_scene_threat_style.put(&mut out[0x124..]);
        self.flee_when_hull_damaged_percent.put(&mut out[0x128..]);
        self.flee_when_leader_flees_style.put(&mut out[0x12c..]);
        self.flee_no_weapons_style.put(&mut out[0x12d..]);
        self.allow_player_targeting.put(&mut out[0x12e..]);
        self.scan_chance.put(&mut out[0x130..]);
        self.force_attack_formation.put(&mut out[0x134..]);
        self.force_attack_formation_set.put(&mut out[0x135..]);
    }

    fn read(b: &[u8]) -> Self {
        Self {
            unknown_00: Field::get(&b[0x00..]),
            scene_toughness_threshold: Field::get(&b[0x04..]),
            target_toughness_preference: Field::get(&b[0x08..]),
            unknown_0c: Field::get(&b[0x0c..]),
            attack_preferences: core::array::from_fn(|i| {
                let at = 0x10 + i * 0x0c;
                AttackPreference {
                    ship_class: Field::get(&b[at..]),
                    range: Field::get(&b[at + 4..]),
                    weapon_flags: Field::get(&b[at + 8..]),
                }
            }),
            attack_subtarget_order: core::array::from_fn(|i| Field::get(&b[0xac + i * 4..])),
            wait_for_leader_target: Field::get(&b[0xcc..]),
            maximum_leader_target_distance: Field::get(&b[0xd0..]),
            field_targeting: Field::get(&b[0xd4..]),
            loot_preference: Field::get(&b[0xd8..]),
            loot_flee_threshold: Field::get(&b[0xdc..]),
            scan_for_cargo_count: Field::get(&b[0xe0..]),
            scan_for_cargo: core::array::from_fn(|i| Field::get(&b[0xe4 + i * 4..])),
            flee_scene_threat_style: Field::get(&b[0x124..]),
            flee_when_hull_damaged_percent: Field::get(&b[0x128..]),
            flee_when_leader_flees_style: Field::get(&b[0x12c..]),
            flee_no_weapons_style: Field::get(&b[0x12d..]),
            allow_player_targeting: Field::get(&b[0x12e..]),
            scan_chance: Field::get(&b[0x130..]),
            force_attack_formation: Field::get(&b[0x134..]),
            force_attack_formation_set: Field::get(&b[0x135..]),
        }
    }
}

/// `Personality::TOUGHNESS_TYPE`.
pub type ToughnessType = i32;

/// `pub::AI::Personality` (0x3d8 bytes): sixteen blocks back to back.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Personality {
    /// +0x000.
    pub evade_dodge: EvadeDodge,
    /// +0x058.
    pub evade_break: EvadeBreak,
    /// +0x094.
    pub buzz_head_toward: BuzzHeadToward,
    /// +0x0f4.
    pub buzz_pass_by: BuzzPassBy,
    /// +0x128.
    pub trail: Trail,
    /// +0x144.
    pub strafe: Strafe,
    /// +0x150.
    pub engine_kill: EngineKill,
    /// +0x164.
    pub repair: Repair,
    /// +0x17c.
    pub gun: Gun,
    /// +0x1bc.
    pub mine: Mine,
    /// +0x1c8.
    pub missile: Missile,
    /// +0x1ec.
    pub damage_reaction: DamageReaction,
    /// +0x230.
    pub missile_reaction: MissileReaction,
    /// +0x240.
    pub countermeasure: Countermeasure,
    /// +0x248.
    pub formation: Formation,
    /// +0x2a0.
    pub job: Job,
}

impl Personality {
    /// The size in bytes.
    pub const SIZE: usize = 0x3d8;

    /// `SCAN_CHANCE_DEFAULT`: "not set".
    pub const SCAN_CHANCE_DEFAULT: f32 = -1.0;

    /// The layout, padding zero.
    #[must_use]
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = vec![0; Self::SIZE];
        self.evade_dodge.write(&mut out[0x000..]);
        self.evade_break.write(&mut out[0x058..]);
        self.buzz_head_toward.write(&mut out[0x094..]);
        self.buzz_pass_by.write(&mut out[0x0f4..]);
        self.trail.write(&mut out[0x128..]);
        self.strafe.write(&mut out[0x144..]);
        self.engine_kill.write(&mut out[0x150..]);
        self.repair.write(&mut out[0x164..]);
        self.gun.write(&mut out[0x17c..]);
        self.mine.write(&mut out[0x1bc..]);
        self.missile.write(&mut out[0x1c8..]);
        self.damage_reaction.write(&mut out[0x1ec..]);
        self.missile_reaction.write(&mut out[0x230..]);
        self.countermeasure.write(&mut out[0x240..]);
        self.formation.write(&mut out[0x248..]);
        self.job.write(&mut out[0x2a0..]);
        out
    }

    /// A personality from its layout; `None` when `bytes` is not 0x3d8
    /// bytes long.
    #[must_use]
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        (bytes.len() == Self::SIZE).then(|| Self {
            evade_dodge: EvadeDodge::read(&bytes[0x000..]),
            evade_break: EvadeBreak::read(&bytes[0x058..]),
            buzz_head_toward: BuzzHeadToward::read(&bytes[0x094..]),
            buzz_pass_by: BuzzPassBy::read(&bytes[0x0f4..]),
            trail: Trail::read(&bytes[0x128..]),
            strafe: Strafe::read(&bytes[0x144..]),
            engine_kill: EngineKill::read(&bytes[0x150..]),
            repair: Repair::read(&bytes[0x164..]),
            gun: Gun::read(&bytes[0x17c..]),
            mine: Mine::read(&bytes[0x1bc..]),
            missile: Missile::read(&bytes[0x1c8..]),
            damage_reaction: DamageReaction::read(&bytes[0x1ec..]),
            missile_reaction: MissileReaction::read(&bytes[0x230..]),
            countermeasure: Countermeasure::read(&bytes[0x240..]),
            formation: Formation::read(&bytes[0x248..]),
            job: Job::read(&bytes[0x2a0..]),
        })
    }

    /// `get_toughness_ratio`: a fixed table by toughness.
    #[must_use]
    pub fn toughness_ratio(kind: ToughnessType) -> f32 {
        match kind {
            0 => 0.15,
            1 => 0.35,
            2 => 0.65,
            3 => 0.85,
            4 => 1.0,
            _ => 0.5,
        }
    }

    /// `get_range_threshold_value`: 5000, times 2/3 or 1/3 by the job's
    /// first dword; unrounded.
    #[must_use]
    pub fn range_threshold_value(&self) -> f64 {
        let base = 5000.0_f64;
        match self.job.unknown_00 {
            // The doubles 2/3 and 1/3 the reference multiplies by.
            1 => base * (2.0 / 3.0),
            2 => base * (1.0 / 3.0),
            _ => base,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes_add_up() {
        let sum = EvadeDodge::SIZE
            + EvadeBreak::SIZE
            + BuzzHeadToward::SIZE
            + BuzzPassBy::SIZE
            + Trail::SIZE
            + Strafe::SIZE
            + EngineKill::SIZE
            + Repair::SIZE
            + Gun::SIZE
            + Mine::SIZE
            + Missile::SIZE
            + DamageReaction::SIZE
            + MissileReaction::SIZE
            + Countermeasure::SIZE
            + Formation::SIZE
            + Job::SIZE;
        assert_eq!(sum, Personality::SIZE);
    }

    #[test]
    fn bytes_round_trip() {
        let mut p = Personality::default();
        p.job.scan_for_cargo[3] = 0xdead_beef;
        p.formation.regroup_on_evade_break = 1;
        let bytes = p.to_bytes();
        assert_eq!(bytes.len(), Personality::SIZE);
        assert_eq!(Personality::from_bytes(&bytes), Some(p));
        // EvadeActivateRange 500.0 at +0; ScanChance -1.0 at +0x2a0+0x130.
        assert_eq!(bytes[..4], 500.0_f32.to_le_bytes());
        assert_eq!(bytes[0x3d0..0x3d4], (-1.0_f32).to_le_bytes());
    }

    #[test]
    fn methods() {
        assert_eq!(Personality::toughness_ratio(2), 0.65);
        assert_eq!(Personality::toughness_ratio(9), 0.5);
        let mut p = Personality::default();
        assert_eq!(p.range_threshold_value(), 5000.0);
        p.job.unknown_00 = 2;
        assert_eq!(
            p.range_threshold_value().to_bits(),
            (5000.0_f64 * 0.333_333_333_333_333_3).to_bits()
        );
    }
}
