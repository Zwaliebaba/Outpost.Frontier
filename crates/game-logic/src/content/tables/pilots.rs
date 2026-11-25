//! The pilot personalities of `missions\pilots_population.ini` and
//! `missions\pilots_story.ini` (TU45, `Content/PilotPersonalities.cpp`): the
//! sixteen block tables, the `[Pilot]` references, and the personality a
//! pilot gives (`GetPilotPersonality`, `ApplyPilotJob`).
//!
//! The `loot_preference` test is the reconstruction's case-sensitive
//! `strstr`, so the shipped data's lower-case values give every job 0.
//! Whether the original compares that way is decision record 0008's open
//! question 4; until the owner answers it the port parses as the
//! reconstruction does.

use std::collections::BTreeMap;

use game_sim::ai::personality::{
    AttackPreference, BuzzHeadToward, BuzzPassBy, Countermeasure, DamageReaction, EngineKill,
    EvadeBreak, EvadeDodge, Formation, Gun, Job, Mine, Missile, MissileReaction, Personality,
    Repair, Strafe, Trail,
};
use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

/// `.rdata` degrees to radians, a float.
const DEG_TO_RAD: f32 = 0.017_453_292;

/// The ship class names of `attack_order` and `attack_preference`
/// (`.rdata` 06fb95e8), with their ids.
const SHIP_CLASSES: [(&[u8], i32); 12] = [
    (b"FIGHTER", 0),
    (b"FREIGHTER", 1),
    (b"TRANSPORT", 2),
    (b"GUNBOAT", 3),
    (b"CRUISER", 4),
    (b"CAPITAL", 5),
    (b"TRADELANE", 6),
    (b"JUMPGATE", 7),
    (b"WEAPONS_PLATFORM", 8),
    (b"DESTROYABLE_DEPOT", 9),
    (b"SOLAR", 10),
    (b"ANYTHING", 11),
];

/// The class id that ends the attack preferences.
const END_OF_PREFERENCES: i32 = 0xc;

/// A `[Pilot]`'s references (`FPilotRefs`), 0 for none.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PilotRefs {
    /// `inherit`.
    pub inherit: u32,
    /// `evade_dodge_id`.
    pub evade_dodge: u32,
    /// `evade_break_id`.
    pub evade_break: u32,
    /// `buzz_head_toward_id`.
    pub buzz_head_toward: u32,
    /// `buzz_pass_by_id`.
    pub buzz_pass_by: u32,
    /// `trail_id`.
    pub trail: u32,
    /// `strafe_id`.
    pub strafe: u32,
    /// `engine_kill_id`.
    pub engine_kill: u32,
    /// `repair_id`.
    pub repair: u32,
    /// `gun_id`.
    pub gun: u32,
    /// `mine_id`.
    pub mine: u32,
    /// `missile_id`.
    pub missile: u32,
    /// `damage_reaction_id`.
    pub damage_reaction: u32,
    /// `missile_reaction_id`.
    pub missile_reaction: u32,
    /// `countermeasure_id`.
    pub countermeasure: u32,
    /// `formation_id`.
    pub formation: u32,
    /// `job_id`.
    pub job: u32,
}

impl PilotRefs {
    /// The references in `FPilotRefs` order, `inherit` first.
    #[must_use]
    pub fn ids(&self) -> [u32; 17] {
        [
            self.inherit,
            self.evade_dodge,
            self.evade_break,
            self.buzz_head_toward,
            self.buzz_pass_by,
            self.trail,
            self.strafe,
            self.engine_kill,
            self.repair,
            self.gun,
            self.mine,
            self.missile,
            self.damage_reaction,
            self.missile_reaction,
            self.countermeasure,
            self.formation,
            self.job,
        ]
    }

    /// Every non-empty block reference of `from` over ours (not its
    /// `inherit`).
    fn inherit_from(&mut self, from: &Self) {
        let pairs = [
            (&mut self.evade_dodge, from.evade_dodge),
            (&mut self.evade_break, from.evade_break),
            (&mut self.buzz_head_toward, from.buzz_head_toward),
            (&mut self.buzz_pass_by, from.buzz_pass_by),
            (&mut self.trail, from.trail),
            (&mut self.strafe, from.strafe),
            (&mut self.engine_kill, from.engine_kill),
            (&mut self.repair, from.repair),
            (&mut self.gun, from.gun),
            (&mut self.mine, from.mine),
            (&mut self.missile, from.missile),
            (&mut self.damage_reaction, from.damage_reaction),
            (&mut self.missile_reaction, from.missile_reaction),
            (&mut self.countermeasure, from.countermeasure),
            (&mut self.formation, from.formation),
            (&mut self.job, from.job),
        ];
        for (ours, theirs) in pairs {
            if theirs != 0 {
                *ours = theirs;
            }
        }
    }
}

/// The seventeen maps of TU45, each keyed by nickname id; a nickname seen
/// before keeps its first entry.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Pilots {
    /// `[Pilot]`.
    pub pilots: BTreeMap<u32, PilotRefs>,
    /// `[EvadeDodgeBlock]`.
    pub evade_dodge: BTreeMap<u32, EvadeDodge>,
    /// `[EvadeBreakBlock]`.
    pub evade_break: BTreeMap<u32, EvadeBreak>,
    /// `[BuzzHeadTowardBlock]`.
    pub buzz_head_toward: BTreeMap<u32, BuzzHeadToward>,
    /// `[BuzzPassByBlock]`.
    pub buzz_pass_by: BTreeMap<u32, BuzzPassBy>,
    /// `[TrailBlock]`.
    pub trail: BTreeMap<u32, Trail>,
    /// `[StrafeBlock]`.
    pub strafe: BTreeMap<u32, Strafe>,
    /// `[EngineKillBlock]`.
    pub engine_kill: BTreeMap<u32, EngineKill>,
    /// `[RepairBlock]`.
    pub repair: BTreeMap<u32, Repair>,
    /// `[GunBlock]`.
    pub gun: BTreeMap<u32, Gun>,
    /// `[MineBlock]`.
    pub mine: BTreeMap<u32, Mine>,
    /// `[MissileBlock]`.
    pub missile: BTreeMap<u32, Missile>,
    /// `[DamageReactionBlock]`.
    pub damage_reaction: BTreeMap<u32, DamageReaction>,
    /// `[MissileReactionBlock]`.
    pub missile_reaction: BTreeMap<u32, MissileReaction>,
    /// `[CountermeasureBlock]`.
    pub countermeasure: BTreeMap<u32, Countermeasure>,
    /// `[FormationBlock]`.
    pub formation: BTreeMap<u32, Formation>,
    /// `[JobBlock]`.
    pub job: BTreeMap<u32, Job>,
}

/// `ParseTrueFalse`: `TRUE` is true, anything but `FALSE` too.
fn true_false(text: &[u8]) -> u8 {
    u8::from(eq_ignore_case(text, b"TRUE") || !eq_ignore_case(text, b"FALSE"))
}

/// The JobBlock toughness keys, `EQUAL` (2) for an unknown one.
fn toughness(text: &[u8]) -> i32 {
    [&b"EASIEST"[..], b"EASY", b"EQUAL", b"HARD", b"HARDEST"]
        .iter()
        .position(|k| eq_ignore_case(text, k))
        .map_or(2, |i| i as i32)
}

/// A float value as the C++ stores it.
fn float(reader: &mut IniReader, index: u32) -> f32 {
    reader.value_float(index) as f32
}

/// An angle in degrees, times the float `DEG_TO_RAD` in double precision
/// and stored as a float.
fn angle(reader: &mut IniReader) -> f32 {
    (reader.value_float(0) * f64::from(DEG_TO_RAD)) as f32
}

/// A `<name>, <weight>` value: the weight goes to the slot whose name
/// matches without regard to case.
fn weight(reader: &mut IniReader, slots: &mut [(&[u8], &mut f32)]) {
    let name = reader.value_string_at(0);
    let value = float(reader, 1);
    if let Some((_, slot)) = slots.iter_mut().find(|(n, _)| eq_ignore_case(&name, n)) {
        **slot = value;
    }
}

/// `strtok` over `" ,\t\n"`.
fn tokens(text: &[u8]) -> impl Iterator<Item = &[u8]> {
    text.split(|&b| matches!(b, b' ' | b',' | b'\t' | b'\n'))
        .filter(|t| !t.is_empty())
}

fn load_evade_dodge(reader: &mut IniReader, out: &mut EvadeDodge, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"evade_activate_range") {
            out.evade_activate_range = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_cone_angle") {
            out.cone_angle = angle(reader);
        } else if reader.is_value(b"evade_dodge_cone_angle_variance_percent") {
            out.cone_angle_variance_percent = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_waggle_axis_cone_angle") {
            out.waggle_axis_cone_angle = angle(reader);
        } else if reader.is_value(b"evade_dodge_roll_angle") {
            out.roll_angle = angle(reader);
        } else if reader.is_value(b"evade_dodge_interval_time") {
            out.interval_time = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_interval_time_variance_percent") {
            out.interval_time_variance_percent = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_distance") {
            out.distance = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_time") {
            out.time = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_slide_throttle") {
            out.slide_throttle = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_turn_throttle") {
            out.turn_throttle = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_corkscrew_roll_throttle") {
            out.corkscrew_roll_throttle = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_corkscrew_roll_flip_direction") {
            out.corkscrew_roll_flip_direction = true_false(&reader.value_string());
        } else if reader.is_value(b"evade_dodge_corkscrew_turn_throttle") {
            out.corkscrew_turn_throttle = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_direction_weight") {
            weight(
                reader,
                &mut [
                    (b"RIGHT", &mut out.direction_weight_right),
                    (b"LEFT", &mut out.direction_weight_left),
                    (b"UP", &mut out.direction_weight_up),
                    (b"DOWN", &mut out.direction_weight_down),
                ],
            );
        } else if reader.is_value(b"evade_dodge_style_weight") {
            weight(
                reader,
                &mut [
                    (b"WAGGLE", &mut out.style_weight_waggle),
                    (b"WAGGLE_RANDOM", &mut out.style_weight_waggle_random),
                    (b"SLIDE", &mut out.style_weight_slide),
                    (b"CORKSCREW", &mut out.style_weight_corkscrew),
                ],
            );
        }
    }
}

fn load_evade_break(reader: &mut IniReader, out: &mut EvadeBreak, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"evade_break_time") {
            out.time = float(reader, 0);
        } else if reader.is_value(b"evade_break_interval_time") {
            out.interval_time = float(reader, 0);
        } else if reader.is_value(b"evade_break_afterburner_delay") {
            out.afterburner_delay = float(reader, 0);
        } else if reader.is_value(b"evade_break_afterburner_delay_variance_percent") {
            out.afterburner_delay_variance_percent = float(reader, 0);
        } else if reader.is_value(b"evade_break_direction_weight") {
            weight(
                reader,
                &mut [
                    (b"RIGHT", &mut out.direction_weight_right),
                    (b"LEFT", &mut out.direction_weight_left),
                    (b"UP", &mut out.direction_weight_up),
                    (b"DOWN", &mut out.direction_weight_down),
                ],
            );
        } else if reader.is_value(b"evade_break_roll_throttle") {
            out.roll_throttle = float(reader, 0);
        } else if reader.is_value(b"evade_break_turn_throttle") {
            out.turn_throttle = float(reader, 0);
        } else if reader.is_value(b"evade_break_style_weight") {
            weight(
                reader,
                &mut [
                    (b"SIDEWAYS", &mut out.style_weight_sideways),
                    (b"OUTRUN", &mut out.style_weight_outrun),
                    (b"REVERSE", &mut out.style_weight_reverse),
                ],
            );
        } else if reader.is_value(b"evade_break_attempt_reverse_time") {
            out.attempt_reverse_time = float(reader, 0);
        } else if reader.is_value(b"evade_break_reverse_distance") {
            out.reverse_distance = float(reader, 0);
        }
    }
}

fn load_buzz_head_toward(reader: &mut IniReader, out: &mut BuzzHeadToward, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"buzz_head_toward_style_weight") {
            weight(
                reader,
                &mut [
                    (b"STRAIGHT_TO", &mut out.style_weight_straight_to),
                    (b"SLIDE", &mut out.style_weight_slide),
                    (b"WAGGLE", &mut out.style_weight_waggle),
                ],
            );
        } else if reader.is_value(b"buzz_min_distance_to_head_toward") {
            out.min_distance_to_head_toward = float(reader, 0);
        } else if reader.is_value(b"buzz_min_distance_to_head_toward_variance_percent") {
            out.min_distance_to_head_toward_variance_percent = float(reader, 0);
        } else if reader.is_value(b"buzz_max_time_to_head_away") {
            out.max_time_to_head_away = float(reader, 0);
        } else if reader.is_value(b"buzz_head_toward_engine_throttle") {
            out.engine_throttle = float(reader, 0);
        } else if reader.is_value(b"buzz_head_toward_turn_throttle") {
            out.turn_throttle = float(reader, 0);
        } else if reader.is_value(b"buzz_head_toward_roll_throttle") {
            out.roll_throttle = float(reader, 0);
        } else if reader.is_value(b"buzz_head_toward_roll_flip_direction") {
            out.roll_flip_direction = true_false(&reader.value_string());
        } else if reader.is_value(b"buzz_slide_throttle") {
            out.slide_throttle = float(reader, 0);
        } else if reader.is_value(b"buzz_slide_interval_time") {
            out.slide_interval_time = float(reader, 0);
        } else if reader.is_value(b"buzz_slide_interval_time_variance_percent") {
            out.slide_interval_time_variance_percent = float(reader, 0);
        } else if reader.is_value(b"buzz_dodge_direction_weight") {
            weight(
                reader,
                &mut [
                    (b"RIGHT", &mut out.dodge_direction_weight_right),
                    (b"LEFT", &mut out.dodge_direction_weight_left),
                    (b"UP", &mut out.dodge_direction_weight_up),
                    (b"DOWN", &mut out.dodge_direction_weight_down),
                ],
            );
        } else if reader.is_value(b"buzz_dodge_turn_throttle") {
            out.dodge_turn_throttle = float(reader, 0);
        } else if reader.is_value(b"buzz_dodge_cone_angle") {
            out.dodge_cone_angle = angle(reader);
        } else if reader.is_value(b"buzz_dodge_cone_angle_variance_percent") {
            out.dodge_cone_angle_variance_percent = float(reader, 0);
        } else if reader.is_value(b"buzz_dodge_waggle_axis_cone_angle") {
            out.dodge_waggle_axis_cone_angle = angle(reader);
        } else if reader.is_value(b"buzz_dodge_roll_angle") {
            out.dodge_roll_angle = angle(reader);
        } else if reader.is_value(b"buzz_dodge_interval_time") {
            out.dodge_interval_time = float(reader, 0);
        } else if reader.is_value(b"buzz_dodge_interval_time_variance_percent") {
            out.dodge_interval_time_variance_percent = float(reader, 0);
        }
    }
}

fn load_buzz_pass_by(reader: &mut IniReader, out: &mut BuzzPassBy, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"buzz_pass_by_style_weight") {
            weight(
                reader,
                &mut [
                    (b"BREAK_AWAY", &mut out.style_weight_break_away),
                    (b"STRAIGHT_BY", &mut out.style_weight_straight_by),
                    (b"ENGINE_KILL", &mut out.style_weight_engine_kill),
                ],
            );
        } else if reader.is_value(b"buzz_distance_to_pass_by") {
            out.distance_to_pass_by = float(reader, 0);
        } else if reader.is_value(b"buzz_pass_by_time") {
            out.time = float(reader, 0);
        } else if reader.is_value(b"buzz_break_direction_weight") {
            weight(
                reader,
                &mut [
                    (b"RIGHT", &mut out.break_direction_weight_right),
                    (b"LEFT", &mut out.break_direction_weight_left),
                    (b"UP", &mut out.break_direction_weight_up),
                    (b"DOWN", &mut out.break_direction_weight_down),
                ],
            );
        } else if reader.is_value(b"buzz_break_direction_cone_angle") {
            // Kept in degrees, as the C++ does.
            out.break_direction_cone_angle = float(reader, 0);
        } else if reader.is_value(b"buzz_break_turn_throttle") {
            out.break_turn_throttle = float(reader, 0);
        } else if reader.is_value(b"buzz_pass_by_roll_throttle") {
            out.roll_throttle = float(reader, 0);
        } else if reader.is_value(b"buzz_drop_bomb_on_pass_by") {
            out.drop_bomb_on_pass_by = true_false(&reader.value_string());
        }
    }
}

fn load_trail(reader: &mut IniReader, out: &mut Trail, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"trail_lock_cone_angle") {
            out.lock_cone_angle = angle(reader);
        } else if reader.is_value(b"trail_break_time") {
            out.break_time = float(reader, 0);
        } else if reader.is_value(b"trail_min_no_lock_time") {
            out.min_no_lock_time = float(reader, 0);
        } else if reader.is_value(b"trail_break_roll_throttle") {
            out.break_roll_throttle = float(reader, 0);
        } else if reader.is_value(b"trail_break_afterburner") {
            out.break_afterburner = true_false(&reader.value_string());
        } else if reader.is_value(b"trail_max_turn_throttle") {
            out.max_turn_throttle = float(reader, 0);
        } else if reader.is_value(b"trail_distance") {
            out.distance = float(reader, 0);
        }
    }
}

fn load_strafe(reader: &mut IniReader, out: &mut Strafe, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"strafe_run_away_distance") {
            out.run_away_distance = float(reader, 0);
        } else if reader.is_value(b"strafe_attack_throttle") {
            out.attack_throttle = float(reader, 0);
        } else if reader.is_value(b"strafe_turn_throttle") {
            out.turn_throttle = float(reader, 0);
        }
    }
}

fn load_engine_kill(reader: &mut IniReader, out: &mut EngineKill, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"engine_kill_search_time") {
            out.search_time = float(reader, 0);
        } else if reader.is_value(b"engine_kill_face_time") {
            out.face_time = float(reader, 0);
        } else if reader.is_value(b"engine_kill_use_afterburner") {
            // The bool goes through `FILD` into the float.
            out.use_afterburner = f32::from(true_false(&reader.value_string()));
        } else if reader.is_value(b"engine_kill_afterburner_time") {
            out.afterburner_time = float(reader, 0);
        } else if reader.is_value(b"engine_kill_max_target_distance") {
            out.max_target_distance = float(reader, 0);
        }
    }
}

fn load_repair(reader: &mut IniReader, out: &mut Repair, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"use_shield_repair_pre_delay") {
            out.shield_pre_delay = float(reader, 0);
        } else if reader.is_value(b"use_shield_repair_post_delay") {
            out.shield_post_delay = float(reader, 0);
        } else if reader.is_value(b"use_shield_repair_at_damage_percent") {
            out.shield_at_damage_percent = float(reader, 0);
        } else if reader.is_value(b"use_hull_repair_pre_delay") {
            out.hull_pre_delay = float(reader, 0);
        } else if reader.is_value(b"use_hull_repair_post_delay") {
            out.hull_post_delay = float(reader, 0);
        } else if reader.is_value(b"use_hull_repair_at_damage_percent") {
            out.hull_at_damage_percent = float(reader, 0);
        }
    }
}

fn load_gun(reader: &mut IniReader, out: &mut Gun, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"gun_fire_interval_time") {
            out.fire_interval_time = float(reader, 0);
        } else if reader.is_value(b"gun_fire_interval_variance_percent") {
            out.fire_interval_variance_percent = float(reader, 0);
        } else if reader.is_value(b"gun_fire_burst_interval_time") {
            out.fire_burst_interval_time = float(reader, 0);
        } else if reader.is_value(b"gun_fire_burst_interval_variance_percent") {
            out.fire_burst_interval_variance_percent = float(reader, 0);
        } else if reader.is_value(b"auto_turret_burst_interval_variance_percent") {
            out.auto_turret_burst_interval_variance_percent = float(reader, 0);
        } else if reader.is_value(b"gun_fire_no_burst_interval_time") {
            out.fire_no_burst_interval_time = float(reader, 0);
        } else if reader.is_value(b"gun_fire_accuracy_cone_angle") {
            out.fire_accuracy_cone_angle = angle(reader);
        } else if reader.is_value(b"gun_fire_accuracy_power") {
            out.fire_accuracy_power = float(reader, 0);
        } else if reader.is_value(b"gun_fire_accuracy_power_npc") {
            out.fire_accuracy_power_npc = float(reader, 0);
        } else if reader.is_value(b"gun_target_point_switch_time") {
            out.target_point_switch_time = float(reader, 0);
        } else if reader.is_value(b"gun_range_threshold") {
            out.range_threshold = float(reader, 0);
        } else if reader.is_value(b"gun_range_threshold_variance_percent") {
            out.range_threshold_variance_percent = float(reader, 0);
        } else if reader.is_value(b"auto_turret_interval_time") {
            out.auto_turret_interval_time = float(reader, 0);
        } else if reader.is_value(b"auto_turret_burst_interval_time") {
            out.auto_turret_burst_interval_time = float(reader, 0);
        } else if reader.is_value(b"auto_turret_no_burst_interval_time") {
            out.auto_turret_no_burst_interval_time = float(reader, 0);
        } else if reader.is_value(b"fire_style") {
            // "MULTIPLE" stores 0; anything else 1.
            out.fire_style = i32::from(!eq_ignore_case(&reader.value_string(), b"MULTIPLE"));
        }
    }
}

fn load_mine(reader: &mut IniReader, out: &mut Mine, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"mine_launch_interval") {
            out.launch_interval = float(reader, 0);
        } else if reader.is_value(b"mine_launch_cone_angle") {
            out.launch_cone_angle = angle(reader);
        } else if reader.is_value(b"mine_launch_range") {
            out.launch_range = float(reader, 0);
        }
    }
}

fn load_missile(reader: &mut IniReader, out: &mut Missile, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"missile_launch_range") {
            out.launch_range = float(reader, 0);
        } else if reader.is_value(b"missile_launch_allow_out_of_range") {
            out.launch_allow_out_of_range = true_false(&reader.value_string());
        } else if reader.is_value(b"missile_launch_interval_time") {
            out.launch_interval_time = float(reader, 0);
        } else if reader.is_value(b"missile_launch_interval_variance_percent") {
            out.launch_interval_variance_percent = float(reader, 0);
        } else if reader.is_value(b"missile_launch_cone_angle") {
            out.launch_cone_angle = angle(reader);
        } else if reader.is_value(b"anti_cruise_missile_min_distance") {
            out.anti_cruise_min_distance = float(reader, 0);
        } else if reader.is_value(b"anti_cruise_missile_max_distance") {
            out.anti_cruise_max_distance = float(reader, 0);
        } else if reader.is_value(b"anti_cruise_missile_pre_fire_delay") {
            out.anti_cruise_pre_fire_delay = float(reader, 0);
        } else if reader.is_value(b"anti_cruise_missile_interval_time") {
            out.anti_cruise_interval_time = float(reader, 0);
        }
    }
}

fn load_damage_reaction(reader: &mut IniReader, out: &mut DamageReaction, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"evade_break_damage_trigger_percent") {
            out.evade_break_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"evade_dodge_more_damage_trigger_percent") {
            out.evade_dodge_more_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"drop_mines_damage_trigger_percent") {
            out.drop_mines_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"drop_mines_damage_trigger_time") {
            out.drop_mines_trigger_time = float(reader, 0);
        } else if reader.is_value(b"engine_kill_face_damage_trigger_percent") {
            out.engine_kill_face_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"engine_kill_face_damage_trigger_time") {
            out.engine_kill_face_trigger_time = float(reader, 0);
        } else if reader.is_value(b"roll_damage_trigger_percent") {
            out.roll_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"roll_damage_trigger_time") {
            out.roll_trigger_time = float(reader, 0);
        } else if reader.is_value(b"afterburner_damage_trigger_percent") {
            out.afterburner_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"afterburner_damage_trigger_time") {
            out.afterburner_trigger_time = float(reader, 0);
        } else if reader.is_value(b"brake_reverse_damage_trigger_percent") {
            out.brake_reverse_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"fire_guns_damage_trigger_percent") {
            out.fire_guns_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"fire_guns_damage_trigger_time") {
            out.fire_guns_trigger_time = float(reader, 0);
        } else if reader.is_value(b"fire_missiles_damage_trigger_percent") {
            out.fire_missiles_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"fire_missiles_damage_trigger_time") {
            out.fire_missiles_trigger_time = float(reader, 0);
        }
    }
}

fn load_missile_reaction(reader: &mut IniReader, out: &mut MissileReaction, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"evade_missile_distance") {
            out.evade_distance = float(reader, 0);
        } else if reader.is_value(b"evade_break_missile_reaction_time") {
            out.evade_break_time = float(reader, 0);
        } else if reader.is_value(b"evade_slide_missile_reaction_time") {
            out.evade_slide_time = float(reader, 0);
        } else if reader.is_value(b"evade_afterburn_missile_reaction_time") {
            out.evade_afterburn_time = float(reader, 0);
        }
    }
}

fn load_countermeasure(reader: &mut IniReader, out: &mut Countermeasure, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"countermeasure_active_time") {
            out.active_time = float(reader, 0);
        } else if reader.is_value(b"countermeasure_unactive_time") {
            out.unactive_time = float(reader, 0);
        }
    }
}

fn load_formation_block(reader: &mut IniReader, out: &mut Formation, nickname: &mut u32) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"force_attack_formation_active_time") {
            out.force_attack_active_time = float(reader, 0);
        } else if reader.is_value(b"force_attack_formation_unactive_time") {
            out.force_attack_unactive_time = float(reader, 0);
        } else if reader.is_value(b"break_formation_damage_trigger_percent") {
            out.break_damage_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"break_formation_damage_trigger_time") {
            out.break_damage_trigger_time = float(reader, 0);
        } else if reader.is_value(b"break_apart_formation_damage_trigger_percent") {
            out.break_apart_damage_trigger_percent = float(reader, 0);
        } else if reader.is_value(b"break_apart_formation_damage_trigger_time") {
            out.break_apart_damage_trigger_time = float(reader, 0);
        } else if reader.is_value(b"break_formation_missile_reaction_time") {
            out.break_missile_reaction_time = float(reader, 0);
        } else if reader.is_value(b"break_apart_formation_missile_reaction_time") {
            out.break_apart_missile_reaction_time = float(reader, 0);
        } else if reader.is_value(b"break_apart_formation_on_buzz_head_toward") {
            out.break_apart_on_buzz_head_toward = true_false(&reader.value_string());
        } else if reader.is_value(b"break_formation_on_buzz_head_toward_time") {
            out.break_on_buzz_head_toward_time = float(reader, 0);
        } else if reader.is_value(b"regroup_formation_on_buzz_head_toward") {
            out.regroup_on_buzz_head_toward = true_false(&reader.value_string());
        } else if reader.is_value(b"break_apart_formation_on_buzz_pass_by") {
            out.break_apart_on_buzz_pass_by = true_false(&reader.value_string());
        } else if reader.is_value(b"break_formation_on_buzz_pass_by_time") {
            out.break_on_buzz_pass_by_time = float(reader, 0);
        } else if reader.is_value(b"regroup_formation_on_buzz_pass_by") {
            out.regroup_on_buzz_pass_by = true_false(&reader.value_string());
        } else if reader.is_value(b"break_apart_formation_on_evade_dodge") {
            out.break_apart_on_evade_dodge = true_false(&reader.value_string());
        } else if reader.is_value(b"break_formation_on_evade_dodge_time") {
            out.break_on_evade_dodge_time = float(reader, 0);
        } else if reader.is_value(b"regroup_formation_on_evade_dodge") {
            out.regroup_on_evade_dodge = true_false(&reader.value_string());
        } else if reader.is_value(b"break_apart_formation_on_evade_break") {
            out.break_apart_on_evade_break = true_false(&reader.value_string());
        } else if reader.is_value(b"break_formation_on_evade_break_time") {
            out.break_on_evade_break_time = float(reader, 0);
        } else if reader.is_value(b"regroup_formation_on_evade_break") {
            out.regroup_on_evade_break = true_false(&reader.value_string());
        } else if reader.is_value(b"formation_exit_mode") {
            // No match leaves the mode as it was.
            let text = reader.value_string();
            let modes: [(&[u8], i32); 5] = [
                (b"BREAK_AWAY_FROM_CENTER", 0),
                (b"BREAK_AWAY_FROM_CENTER_AFTERBURNER", 1),
                (b"BRAKE_REVERSE", 2),
                (b"OUTRUN", 3),
                (b"NOTHING", 5),
            ];
            if let Some((_, mode)) = modes.iter().find(|(n, _)| eq_ignore_case(&text, n)) {
                out.exit_mode = *mode;
            }
        } else if reader.is_value(b"formation_exit_top_turn_break_away_throttle") {
            out.exit_top_turn_break_away_throttle = float(reader, 0);
        } else if reader.is_value(b"formation_exit_roll_outrun_throttle") {
            out.exit_roll_outrun_throttle = float(reader, 0);
        } else if reader.is_value(b"formation_exit_max_time") {
            out.exit_max_time = float(reader, 0);
        } else if reader.is_value(b"leader_makes_me_tougher") {
            out.leader_makes_me_tougher = true_false(&reader.value_string());
        }
    }
}

/// Writes record `index` of the attack preferences; past the array the
/// C++ writes out of bounds and the port drops the write (D782).
fn preference(job: &mut Job, index: usize) -> Option<&mut AttackPreference> {
    job.attack_preferences.get_mut(index)
}

/// `attack_preference = <class>, <range>, <weapon types>` into record
/// `index`, then the terminator after it, matched or not.
fn load_attack_preference(job: &mut Job, reader: &mut IniReader, index: usize) {
    if index == 0xc {
        return;
    }
    let class = reader.value_string_at(0);
    if let Some((_, id)) = SHIP_CLASSES.iter().find(|(n, _)| eq_ignore_case(&class, n)) {
        let range = float(reader, 1);
        // `_strupr` of the weapon types, then `strstr` for each word.
        let weapons = reader.value_string_at(2).to_ascii_uppercase();
        let has = |word: &[u8]| weapons.windows(word.len()).any(|w| w == word);
        let mut flags = 0;
        for (word, bit) in [
            (&b"GUNS"[..], 1),
            (b"GUIDED", 2),
            (b"UNGUIDED", 4),
            (b"TORPEDO", 8),
        ] {
            if has(word) {
                flags |= bit;
            }
        }
        if let Some(record) = preference(job, index) {
            record.ship_class = *id;
            record.range = range;
            record.weapon_flags = flags;
        }
    }
    if let Some(record) = preference(job, index + 1) {
        record.ship_class = END_OF_PREFERENCES;
    }
}

/// `attack_order`: each class token in a record of its own (`ANYTHING`
/// takes two, as in the C++), range 5000 and every weapon, then the
/// terminator.
fn load_attack_order(job: &mut Job, text: &[u8]) {
    let mut count = 0;
    for token in tokens(text) {
        if eq_ignore_case(token, b"ANYTHING") {
            if let Some(record) = preference(job, count) {
                record.ship_class = 0xb;
            }
            count += 1;
        }
        for (name, id) in SHIP_CLASSES {
            if eq_ignore_case(token, name) {
                if let Some(record) = preference(job, count) {
                    record.ship_class = id;
                    record.range = 5000.0;
                    record.weapon_flags = 0xf;
                }
                count += 1;
            }
        }
    }
    if let Some(record) = preference(job, count) {
        record.ship_class = END_OF_PREFERENCES;
    }
}

/// `attack_subtarget_order`: the tokens as subtarget kinds, ended by 7.
/// A token may match more than one test, each appending. Writes past the
/// eight slots are dropped (D782).
fn load_attack_subtarget_order(job: &mut Job, text: &[u8]) {
    let order = &mut job.attack_subtarget_order;
    let mut count = 0;
    let mut put = |count: &mut usize, value: i32| {
        if let Some(slot) = order.get_mut(*count) {
            *slot = value;
        }
        *count += 1;
    };
    let kinds: [(&[u8], i32); 7] = [
        (b"ANYTHING", 6),
        (b"GUNS", 0),
        (b"TURRETS", 1),
        (b"LAUNCHERS", 2),
        (b"TOWERS", 3),
        (b"ENGINES", 4),
        (b"HULL", 5),
    ];
    for token in tokens(text) {
        for (name, kind) in kinds {
            if eq_ignore_case(token, name) {
                put(&mut count, kind);
            }
        }
    }
    put(&mut count, 7);
}

/// `strstr`, case-sensitive.
fn contains(text: &[u8], word: &[u8]) -> bool {
    text.windows(word.len()).any(|w| w == word)
}

fn load_job(reader: &mut IniReader, out: &mut Job, nickname: &mut u32) {
    let mut preference_index = 0;
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            *nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"loot_preference") {
            // The reconstruction's case-sensitive `strstr` (decision record
            // 0008, open question 4): the shipped lower-case values give 0.
            let text = reader.value_string();
            out.loot_preference = if contains(&text, b"LT_ALL") {
                7
            } else if contains(&text, b"LT_NONE") {
                0
            } else {
                i32::from(contains(&text, b"LT_COMMODITIES"))
                    | if contains(&text, b"LT_EQUIPMENT") {
                        2
                    } else {
                        0
                    }
                    | if contains(&text, b"LT_POTIONS") { 4 } else { 0 }
            };
        } else if reader.is_value(b"loot_flee_threshold") {
            out.loot_flee_threshold = toughness(&reader.value_string());
        } else if reader.is_value(b"flee_scene_threat_style") {
            out.flee_scene_threat_style = toughness(&reader.value_string());
        } else if reader.is_value(b"flee_when_hull_damaged_percent") {
            out.flee_when_hull_damaged_percent = float(reader, 0);
        } else if reader.is_value(b"flee_when_leader_flees_style") {
            out.flee_when_leader_flees_style = true_false(&reader.value_string());
        } else if reader.is_value(b"flee_no_weapons_style") {
            out.flee_no_weapons_style = true_false(&reader.value_string());
        } else if reader.is_value(b"scene_toughness_threshold") {
            out.scene_toughness_threshold = toughness(&reader.value_string());
        } else if reader.is_value(b"target_toughness_preference") {
            out.target_toughness_preference = toughness(&reader.value_string());
        } else if reader.is_value(b"wait_for_leader_target") {
            out.wait_for_leader_target = true_false(&reader.value_string());
        } else if reader.is_value(b"maximum_leader_target_distance") {
            out.maximum_leader_target_distance = float(reader, 0);
        } else if reader.is_value(b"combat_drift_distance") {
            // Recognised; nothing is stored.
        } else if reader.is_value(b"attack_order") {
            load_attack_order(out, &reader.value_string());
        } else if reader.is_value(b"attack_preference") {
            load_attack_preference(out, reader, preference_index);
            preference_index += 1;
        } else if reader.is_value(b"attack_subtarget_order") {
            load_attack_subtarget_order(out, &reader.value_string());
        } else if reader.is_value(b"field_targeting") {
            let text = reader.value_string();
            out.field_targeting = [&b"NEVER"[..], b"LOW_DENSITY", b"HIGH_DENSITY", b"ALWAYS"]
                .iter()
                .position(|k| eq_ignore_case(&text, k))
                .map_or(1, |i| i as i32);
        } else if reader.is_value(b"force_attack_formation") {
            out.force_attack_formation = true_false(&reader.value_string());
            out.force_attack_formation_set = 1;
        } else if reader.is_value(b"allow_player_targeting") {
            out.allow_player_targeting = true_false(&reader.value_string());
        }
    }
}

/// The block reference a `[Pilot]` key names, in the C++'s test order.
fn reference<'r>(refs: &'r mut PilotRefs, reader: &IniReader) -> Option<&'r mut u32> {
    let keys: [&[u8]; 16] = [
        b"evade_dodge_id",
        b"evade_break_id",
        b"buzz_head_toward_id",
        b"buzz_pass_by_id",
        b"trail_id",
        b"strafe_id",
        b"engine_kill_id",
        b"repair_id",
        b"gun_id",
        b"mine_id",
        b"missile_id",
        b"damage_reaction_id",
        b"missile_reaction_id",
        b"countermeasure_id",
        b"formation_id",
        b"job_id",
    ];
    let index = keys.iter().position(|k| reader.is_value(k))?;
    let slots = [
        &mut refs.evade_dodge,
        &mut refs.evade_break,
        &mut refs.buzz_head_toward,
        &mut refs.buzz_pass_by,
        &mut refs.trail,
        &mut refs.strafe,
        &mut refs.engine_kill,
        &mut refs.repair,
        &mut refs.gun,
        &mut refs.mine,
        &mut refs.missile,
        &mut refs.damage_reaction,
        &mut refs.missile_reaction,
        &mut refs.countermeasure,
        &mut refs.formation,
        &mut refs.job,
    ];
    slots.into_iter().nth(index)
}

/// One `[Pilot]`; a nickname seen before keeps its first definition.
fn load_pilot(pilots: &mut BTreeMap<u32, PilotRefs>, reader: &mut IniReader) {
    let mut refs = PilotRefs::default();
    let mut nickname = 0;
    while reader.read_value() {
        let id = create_id(&reader.value_string());
        if reader.is_value(b"nickname") {
            nickname = id;
        } else if reader.is_value(b"inherit") {
            refs.inherit = id;
            if refs.inherit != 0
                && let Some(parent) = pilots.get(&refs.inherit)
            {
                refs.inherit_from(parent);
            }
        } else if let Some(slot) = reference(&mut refs, reader) {
            *slot = id;
        }
    }
    if nickname != 0 {
        pilots.entry(nickname).or_insert(refs);
    }
}

/// Files a block under its nickname, the first kept, when it has one.
fn file<T>(table: &mut BTreeMap<u32, T>, nickname: u32, block: T) {
    if nickname != 0 {
        table.entry(nickname).or_insert(block);
    }
}

/// `LoadPilotPersonalities`: every block and `[Pilot]` of the file.
pub fn load_pilot_personalities(pilots: &mut Pilots, reader: &mut IniReader) {
    macro_rules! block {
        ($loader:ident, $table:ident) => {{
            let mut block = Default::default();
            let mut nickname = 0;
            $loader(reader, &mut block, &mut nickname);
            file(&mut pilots.$table, nickname, block);
        }};
    }
    while reader.read_header() {
        if reader.is_header(b"EvadeDodgeBlock") {
            block!(load_evade_dodge, evade_dodge);
        } else if reader.is_header(b"EvadeBreakBlock") {
            block!(load_evade_break, evade_break);
        } else if reader.is_header(b"BuzzHeadTowardBlock") {
            block!(load_buzz_head_toward, buzz_head_toward);
        } else if reader.is_header(b"BuzzPassByBlock") {
            block!(load_buzz_pass_by, buzz_pass_by);
        } else if reader.is_header(b"TrailBlock") {
            block!(load_trail, trail);
        } else if reader.is_header(b"StrafeBlock") {
            block!(load_strafe, strafe);
        } else if reader.is_header(b"EngineKillBlock") {
            block!(load_engine_kill, engine_kill);
        } else if reader.is_header(b"RepairBlock") {
            block!(load_repair, repair);
        } else if reader.is_header(b"GunBlock") {
            block!(load_gun, gun);
        } else if reader.is_header(b"MineBlock") {
            block!(load_mine, mine);
        } else if reader.is_header(b"MissileBlock") {
            block!(load_missile, missile);
        } else if reader.is_header(b"DamageReactionBlock") {
            block!(load_damage_reaction, damage_reaction);
        } else if reader.is_header(b"MissileReactionBlock") {
            block!(load_missile_reaction, missile_reaction);
        } else if reader.is_header(b"CountermeasureBlock") {
            block!(load_countermeasure, countermeasure);
        } else if reader.is_header(b"FormationBlock") {
            block!(load_formation_block, formation);
        } else if reader.is_header(b"JobBlock") {
            block!(load_job, job);
        } else if reader.is_header(b"Pilot") {
            load_pilot(&mut pilots.pilots, reader);
        }
    }
}

impl Pilots {
    /// `ApplyPilotJob`: the job block over the personality's job. Its
    /// `force_attack_formation` replaces ours only when the block set it.
    /// False for no job or an unknown one.
    pub fn apply_job(&self, job: u32, personality: &mut Personality) -> bool {
        if job == 0 {
            return false;
        }
        let Some(block) = self.job.get(&job) else {
            return false;
        };
        let set = block.force_attack_formation_set != 0;
        let force = if set {
            block.force_attack_formation
        } else {
            personality.job.force_attack_formation
        };
        personality.job = *block;
        personality.job.force_attack_formation = force;
        if set {
            personality.job.force_attack_formation_set = 1;
        }
        true
    }

    /// `GetPilotPersonality`: each referenced block that exists over the
    /// personality, then with a job the inherited pilot's job and our own.
    /// False for an unknown pilot, the personality untouched.
    pub fn personality(&self, pilot: u32, out: &mut Personality) -> bool {
        let Some(refs) = self.pilots.get(&pilot) else {
            return false;
        };
        fn apply<T: Copy>(table: &BTreeMap<u32, T>, id: u32, slot: &mut T) {
            if id != 0
                && let Some(block) = table.get(&id)
            {
                *slot = *block;
            }
        }
        apply(&self.evade_dodge, refs.evade_dodge, &mut out.evade_dodge);
        apply(&self.evade_break, refs.evade_break, &mut out.evade_break);
        apply(
            &self.buzz_head_toward,
            refs.buzz_head_toward,
            &mut out.buzz_head_toward,
        );
        apply(&self.buzz_pass_by, refs.buzz_pass_by, &mut out.buzz_pass_by);
        apply(&self.trail, refs.trail, &mut out.trail);
        apply(&self.strafe, refs.strafe, &mut out.strafe);
        apply(&self.engine_kill, refs.engine_kill, &mut out.engine_kill);
        apply(&self.repair, refs.repair, &mut out.repair);
        apply(&self.gun, refs.gun, &mut out.gun);
        apply(&self.mine, refs.mine, &mut out.mine);
        apply(&self.missile, refs.missile, &mut out.missile);
        apply(
            &self.damage_reaction,
            refs.damage_reaction,
            &mut out.damage_reaction,
        );
        apply(
            &self.missile_reaction,
            refs.missile_reaction,
            &mut out.missile_reaction,
        );
        apply(
            &self.countermeasure,
            refs.countermeasure,
            &mut out.countermeasure,
        );
        apply(&self.formation, refs.formation, &mut out.formation);
        if refs.job != 0 {
            if refs.inherit != 0
                && let Some(parent) = self.pilots.get(&refs.inherit)
            {
                self.apply_job(parent.job, out);
            }
            self.apply_job(refs.job, out);
        }
        true
    }

    /// `GetPilotPersonalityWithJob`: the pilot's personality, then a
    /// non-empty job over it.
    pub fn personality_with_job(&self, pilot: u32, job: u32, out: &mut Personality) -> bool {
        if !self.personality(pilot, out) {
            return false;
        }
        job == 0 || self.apply_job(job, out)
    }
}
